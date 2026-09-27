// test_hooks_engine.cpp - F6 (audit G07): the tool lifecycle hooks engine.
// Covers the payload builders (hook_event_name/session_id/cwd + the per-event
// fields, events.py), the regex matcher filtering, the block-aggregation and
// fail-open error isolation (engine.py / runner.py), and the soul wiring in
// execute_tool_call: PreToolUse blocks with the reference wording,
// PostToolUse fires after the run with tool_output, PostToolUseFailure fires
// on the tool's runtime-failure status.
//
// Framework: Boost.UT; every assertion lives in main()-scope lambdas.

#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "agent/hooks_engine.h"
#include "agent/soul.h"
#include "builtin_tools/tool_registry.h"

#include <cstdio>

namespace {

using namespace boost::ut;

class FakeBackend : public kimix::agent::IChatBackend {
public:
    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk) override {
        (void)messages;
        (void)tools;
        (void)on_chunk;
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content = "done";
        return r;
    }
    int64_t max_context_size() const override { return 128000; }
    kimix::string model_name() const override { return "fake"; }
};

kimix::string tmp_workspace(const char *name) {
    std::error_code ec;
    kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) / name;
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

// A hook's captured call: the matcher value + the raw payload.
struct hook_call {
    kimix::string matcher_value;
    kimix::string payload;
};

} // namespace

int main() {
    using namespace boost::ut;
    using namespace kimix::agent;
    using kimix::agent::hooks::HookEngine;
    using kimix::agent::hooks::HookResult;

    // ── Payload builders (events.py) ────────────────────────────────────────

    "payload_builders_carry_the_reference_fields"_test = [] {
        const kimix::string pre = hooks::pre_tool_use_payload(
            "sess-1", "/tmp/ws", "read", R"({"file_path":"a.txt"})", "call-9");
        expect(pre.find("\"hook_event_name\":\"PreToolUse\"") !=
               kimix::string::npos)
            << pre;
        expect(pre.find("\"session_id\":\"sess-1\"") != kimix::string::npos)
            << pre;
        expect(pre.find("\"cwd\":\"") != kimix::string::npos) << pre;
        expect(pre.find("\"tool_name\":\"read\"") != kimix::string::npos) << pre;
        expect(pre.find("\"file_path\":\"a.txt\"") != kimix::string::npos) << pre;
        expect(pre.find("\"tool_call_id\":\"call-9\"") != kimix::string::npos)
            << pre;

        const kimix::string post = hooks::post_tool_use_payload(
            "sess-1", "/tmp/ws", "read", "{}", "some output", "call-9");
        expect(post.find("\"hook_event_name\":\"PostToolUse\"") !=
               kimix::string::npos)
            << post;
        expect(post.find("\"tool_output\":\"some output\"") !=
               kimix::string::npos)
            << post;

        const kimix::string fail = hooks::post_tool_use_failure_payload(
            "sess-1", "/tmp/ws", "bash", "{}", "boom", "call-9");
        expect(fail.find("\"hook_event_name\":\"PostToolUseFailure\"") !=
               kimix::string::npos)
            << fail;
        expect(fail.find("\"error\":\"boom\"") != kimix::string::npos) << fail;
    };

    // ── Engine: matchers, aggregation, error isolation ─────────────────────

    "engine_runs_only_matching_hooks_and_aggregates_blocks"_test = [] {
        HookEngine engine;
        kimix::vector<hook_call> calls;
          expect(engine.add_hook(
              hooks::kEventPreToolUse, "", // match-all hook
              [&](const kimix::string &payload, kimix::string &) {
                  calls.push_back({"all", payload});
                  HookResult r;
                  r.action = "allow";
                  return r;
              }));
        expect(engine.add_hook(
            hooks::kEventPreToolUse, "wri.*",
            [&](const kimix::string &payload, kimix::string &) {
                calls.push_back({"write", payload});
                HookResult r;
                r.action = "block";
                r.reason = "no writes allowed";
                return r;
            }));
        expect(engine.has_hooks());
        expect(engine.has_hooks_for(hooks::kEventPreToolUse));
        expect(!engine.has_hooks_for(hooks::kEventPostToolUse));

        const kimix::vector<HookResult> results = engine.trigger(
            hooks::kEventPreToolUse, "write", R"({"hook_event_name":"x"})");
        // Only the match-all hook and the "wri.*" hook match "write".
        expect(calls.size() == 2u);
        expect(results.size() == 2u);
        bool blocked = false;
        for (const HookResult &r : results) {
            if (r.blocked()) {
                blocked = true;
                expect(eq(r.reason, kimix::string("no writes allowed")));
            }
        }
        expect(blocked);

        // A non-matching event runs nothing; a non-matching tool value runs
        // only the match-all hook.
        expect(engine.trigger(hooks::kEventPostToolUse, "write", "{}").empty());
        expect(engine.trigger(hooks::kEventPreToolUse, "grep", "{}").size() == 1u);
    };

    "engine_fail_open_on_hook_errors_and_allows_by_default"_test = [] {
        HookEngine engine;
        expect(engine.add_hook(
            hooks::kEventPreToolUse, "",
            [](const kimix::string &, kimix::string &error) {
                error = "hook exploded";
                return HookResult{};
            }));
        const kimix::vector<HookResult> results =
            engine.trigger(hooks::kEventPreToolUse, "read", "{}");
        expect(results.size() == 1u);
        if (results.size() == 1u) {
            expect(!results[0].blocked()); // fail-open
            expect(results[0].stderr_text.find("hook exploded") !=
                   kimix::string::npos);
        }
        expect(engine.summary().size() == 1u);
        expect(engine.summary()[0].second == 1u);
    };

    // ── Soul wiring (toolset.py:1512-1563) ─────────────────────────────────

    "pre_tool_use_block_short_circuits_the_dispatch"_test = [] {
        kimix::agent::AgentSession session(
            tmp_workspace("kimix_test_hooks_ws_block"));
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        soul.hook_engine().add_hook(
            hooks::kEventPreToolUse, "write",
            [](const kimix::string &, kimix::string &) {
                HookResult r;
                r.action = "block";
                r.reason = "never write files";
                return r;
            });
        kimix::string err;
        const kimix::string out = soul.execute_tool_call(
            "write", R"JSON({"file_path":"blocked.txt","content":"x"})JSON", err);
        // toolset.py:1526-1534: message = reason, brief = "Hook blocked".
        expect(eq(out,
                  kimix::string("<system>ERROR: never write files</system>")))
            << out;
        expect(eq(err, kimix::string("never write files")));
        // The tool never ran.
        std::error_code ec;
        expect(!kimix::filesystem::exists(
            kimix::filesystem::path(session.work_dir()) / "blocked.txt", ec));
    };

    "pre_tool_use_block_without_reason_uses_the_default_text"_test = [] {
        kimix::agent::AgentSession session(
            tmp_workspace("kimix_test_hooks_ws_block2"));
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        soul.hook_engine().add_hook(
            hooks::kEventPreToolUse, "",
            [](const kimix::string &, kimix::string &) {
                HookResult r;
                r.action = "block";
                return r; // no reason
            });
        kimix::string err;
        const kimix::string out = soul.execute_tool_call(
            "write", R"JSON({"file_path":"b.txt","content":"x"})JSON", err);
        expect(eq(out, kimix::string(
                           "<system>ERROR: Blocked by PreToolUse hook</system>")))
            << out;
    };

    "post_tool_use_fires_with_tool_output_and_ids"_test = [] {
        const kimix::string ws = tmp_workspace("kimix_test_hooks_ws_post");
        kimix::agent::AgentSession session(ws);
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        kimix::vector<kimix::string> payloads;
        soul.hook_engine().add_hook(
            hooks::kEventPostToolUse, "write",
            [&](const kimix::string &payload, kimix::string &) {
                payloads.push_back(payload);
                return HookResult{};
            });
        kimix::string err;
        // Run a real write so the payload carries the tool output. The CLI
        // host populates the session id before dispatch; install a value here
        // so the payload can be pinned.
        kimix::string write_err;
        session.tool_session().session_id = "sess-hooks-1";
        soul.execute_tool_call(
            "write", R"JSON({"file_path":"post.txt","content":"body"})JSON",
            write_err, "call-77");
        expect(payloads.size() == 1u);
        if (payloads.size() == 1u) {
            expect(payloads[0].find("\"hook_event_name\":\"PostToolUse\"") !=
                   kimix::string::npos)
                << payloads[0];
            expect(payloads[0].find("\"tool_name\":\"write\"") !=
                   kimix::string::npos)
                << payloads[0];
            expect(payloads[0].find("\"tool_call_id\":\"call-77\"") !=
                   kimix::string::npos)
                << payloads[0];
            expect(payloads[0].find("\"cwd\":\"") != kimix::string::npos)
                << payloads[0];
            expect(payloads[0].find("\"session_id\":\"sess-hooks-1\"") !=
                   kimix::string::npos)
                << payloads[0];
        }
        std::error_code ec;
        expect(kimix::filesystem::exists(
            kimix::filesystem::path(ws) / "post.txt", ec));
    };

    "post_tool_use_failure_fires_on_a_runtime_error_status"_test = [] {
        kimix::agent::AgentSession session(
            tmp_workspace("kimix_test_hooks_ws_fail"));
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        kimix::vector<kimix::string> failures;
        soul.hook_engine().add_hook(
            hooks::kEventPostToolUseFailure, "",
            [&](const kimix::string &payload, kimix::string &) {
                failures.push_back(payload);
                return HookResult{};
            });
        kimix::string err;
        // A tool that reports its runtime-failure status ("error"): the pwsh
        // fix mode needs a host; use a deterministic failure instead - a read
        // of a missing file answers not_found (NOT "error"), so drive the
        // failure through a tool whose payload status is "error" via the
        // external-tool channel.
        kimix::builtin_tools::ExternalToolCall call =
            [](kimix::string_view, kimix::string &result_json,
               kimix::string &) {
                result_json =
                    R"({"status":"error","runtime":true,"message":"host exploded"})";
                return true;
            };
        kimix::string reg_error;
        expect(kimix::builtin_tools::ToolRegistry::instance()
                   .register_external_tool("hookfail_probe",
                                           "probe tool", "{}", call,
                                           reg_error));
        kimix::string dispatch_err;
        const kimix::string rendered = soul.execute_tool_call(
            "hookfail_probe", "{}", dispatch_err, "call-31");
        // F10: the runtime marker renders the reference's ToolRuntimeError
        // text inside the unchanged envelope (error.py:37-44 + the
        // message.py "unexpected error" sentence).
        expect(rendered.find("<system>ERROR: Error running tool: host exploded") ==
               0)
            << rendered;
        expect(rendered.find("This is an unexpected error and the tool is "
                             "probably not working.") != kimix::string::npos)
            << rendered;
        expect(failures.size() == 1u); // fired once by the rendered call above
        if (failures.size() == 1u) {
            expect(failures[0].find(
                       "\"hook_event_name\":\"PostToolUseFailure\"") !=
                   kimix::string::npos)
                << failures[0];
            expect(failures[0].find("\"error\":\"host exploded\"") !=
                   kimix::string::npos)
                << failures[0];
        }
        kimix::builtin_tools::ToolRegistry::instance().unregister_tool(
            "hookfail_probe");
    };

    "set_hook_engine_replaces_the_engine"_test = [] {
        kimix::agent::AgentSession session(
            tmp_workspace("kimix_test_hooks_ws_swap"));
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        expect(!soul.hook_engine().has_hooks());
        auto engine = kimix::shared_ptr<HookEngine>(new HookEngine());
        kimix::vector<kimix::string> seen;
        engine->add_hook(hooks::kEventPreToolUse, "",
                         [&](const kimix::string &payload, kimix::string &) {
                             seen.push_back(payload);
                             return HookResult{};
                         });
        soul.set_hook_engine(engine);
        expect(soul.hook_engine().has_hooks());
        kimix::string err;
        soul.execute_tool_call("read", R"JSON({"file_path":"x.txt"})JSON", err);
        expect(seen.size() == 1u);
        // Resetting to empty restores the ungated behaviour.
        soul.set_hook_engine(kimix::shared_ptr<HookEngine>(new HookEngine()));
        err.clear();
        soul.execute_tool_call("read", R"JSON({"file_path":"x.txt"})JSON", err);
        expect(seen.size() == 1u);
    };

    return 0;
}
