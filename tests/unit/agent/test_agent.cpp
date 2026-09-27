// test_agent.cpp - Unit tests for the tool registry, regex_lite, the native
// (reproc) process runner, the native Read/Write/Grep/Bash execution paths
// and the KimiSoul agent turn loop (with a scripted fake chat backend).
//
// Framework: Boost.UT (tests/ut/ut.hpp). No network access: the soul tests
// drive IChatBackend with canned ChatResults.

#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "agent/soul.h"
#include "builtin_tools/bash_tool.h"
#include "builtin_tools/compact_tool.h"
#include "builtin_tools/glob_tool.h"
#include "builtin_tools/grep_tool.h"
#include "builtin_tools/process_runner.h"
#include "builtin_tools/read_tool.h"
#include "builtin_tools/retrieve_tool.h"
#include "builtin_tools/regex_lite.h"
#include "builtin_tools/tool_registry.h"
#include "builtin_tools/write_tool.h"

#include <cstdio>

#include <core/clock.h>

namespace {

using namespace boost::ut;

using kimix::builtin_tools::Session;
using kimix::builtin_tools::ToolParams;
using kimix::builtin_tools::ValueElement;

// Scripted chat backend: pops one canned result per chat() call.
class FakeBackend : public kimix::agent::IChatBackend {
public:
    kimix::vector<kimix::llm::ChatResult> scripted;
    kimix::vector<kimix::vector<kimix::llm::Message>> requests;
    size_t index = 0;

    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk) override {
        (void)tools;
        (void)on_chunk;
        requests.push_back(messages);
        if (index < scripted.size()) {
            return scripted[index++];
        }
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content = "done";
        return r;
    }
    int64_t max_context_size() const override { return 128000; }
    kimix::string model_name() const override { return "fake"; }
};

kimix::string tmp_workspace() {
    std::error_code ec;
    kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) / "kimix_agent_test_ws";
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

// No exceptions (kimix is built with kimix_enable_exception=false), so a
// failed parse of a test literal is reported through Boost.UT instead of by
// throwing std::runtime_error.
ToolParams parse_json(const kimix::string &json) {
    ToolParams p;
    kimix::string err;
    const bool ok =
        p.try_deserialize(kimix::span<char const>(json.data(), json.size()), err);
    expect(ok) << "parse_json: " << err;
    return p;
}

kimix::string result_string(const kimix::vector<char> &buf) {
    return kimix::string(buf.data(), buf.size());
}

} // namespace

int main() {
    using namespace boost::ut;

    // =======================================================================
    // Tool registry
    // =======================================================================
    "registry_has_all_builtin_tools"_test = [] {
        auto &reg = kimix::builtin_tools::ToolRegistry::instance();
        expect(reg.size() >= 12u);
        for (const char *name :
             {"bash", "read", "write", "grep", "glob", "compact", "edit",
              "pwsh", "fetch_url", "web_search", "read_image", "retrieve",
              "python"}) {
            const auto *meta = reg.find(kimix::string_view(name));
            expect(meta != nullptr) << name;
            if (meta != nullptr) {
                expect(eq(meta->name, kimix::string(name)));
                expect(!meta->description.empty());
                expect(meta->parameters_json.find("properties") !=
                       kimix::string::npos)
                    << name;
            }
        }
    };

    "registry_case_insensitive_create"_test = [] {
        auto &reg = kimix::builtin_tools::ToolRegistry::instance();
        Session session;
        auto tool = reg.create("bash", &session);
        expect(tool != nullptr);
        auto none = reg.create("no_such_tool", &session);
        expect(none == nullptr);
    };

    // =======================================================================
    // regex_lite
    // =======================================================================
    "regex_lite_basic"_test = [] {
        using kimix::builtin_tools::regex_lite::Regex;
        kimix::string err;
        Regex re;
        expect(re.compile("foo", false, err));
        size_t b = 0, e = 0;
        expect(re.search("xfooy", b, e));
        expect(eq(b, size_t(1)));
        expect(eq(e, size_t(4)));
        expect(!re.search("bar", b, e));

        expect(re.compile("[a-z]+\\d{2,3}", false, err));
        expect(re.search("ab12", b, e));
        expect(re.search("xy999", b, e));
        expect(!re.search("ab1", b, e));

        expect(re.compile("^foo|bar$", false, err));
        expect(re.search("foobar", b, e));
        expect(re.search("xxbar", b, e));
        expect(!re.search("xxbarx", b, e));
    };

    "regex_lite_groups_case_insensitive"_test = [] {
        using kimix::builtin_tools::regex_lite::Regex;
        kimix::string err;
        Regex re;
        expect(re.compile("(\\w+)@(\\w+)\\.(com|org)", true, err));
        size_t b = 0, e = 0;
        expect(re.search("mail Bob@Example.COM ok", b, e));
        size_t gb = 0, ge = 0;
        expect(re.last_group(1, gb, ge));
        const kimix::string subj = "mail Bob@Example.COM ok";
        expect(eq(kimix::string("Bob"),
                  kimix::string(subj.substr(gb, ge - gb))));
        expect(eq(re.group_count(), size_t(3)));

        // Bad patterns must be rejected, not crash.
        Regex bad;
        expect(!bad.compile("(foo", false, err));
        expect(!err.empty());
        expect(!bad.compile("a{2,1}", false, err));
        expect(!bad.compile("*x", false, err));
        expect(!bad.compile("a\\1", false, err));
    };

    "regex_lite_utf8"_test = [] {
        using kimix::builtin_tools::regex_lite::Regex;
        kimix::string err;
        Regex re;
        expect(re.compile("caf.", false, err));
        size_t b = 0, e = 0;
        const kimix::string s = "x caf\xC3\xA9 y";
        expect(re.search(s, b, e));
        expect(eq(kimix::string(s.substr(b, e - b)),
                  kimix::string("caf\xC3\xA9")));
    };

    // =======================================================================
    // process_runner (reproc spawn/poll/drain/kill)
    // =======================================================================
    "process_runner_echo"_test = [] {
        const kimix::string bash =
            kimix::builtin_tools::bash::Bash::detect_bash_path();
        if (bash.empty()) {
            // No bash on this machine: skip (the e2e covers Windows+Git Bash).
            return;
        }
        namespace proc = kimix::builtin_tools::proc;
        proc::run_options opts;
        opts.argv = {bash, "--noprofile", "--norc", "-c",
                     "echo hello_world; echo err_msg >&2; exit 3"};
        opts.timeout_ms = 20000;
        const proc::run_result rr = proc::run_process(opts);
        expect(rr.spawn_error.empty()) << rr.spawn_error;
        expect(rr.exit_code.has_value());
        expect(eq(*rr.exit_code, int64_t(3)));
        expect(rr.output.find("hello_world") != kimix::string::npos);
        expect(rr.output.find("err_msg") != kimix::string::npos);
    };

    "process_runner_timeout_kill"_test = [] {
        const kimix::string bash =
            kimix::builtin_tools::bash::Bash::detect_bash_path();
        if (bash.empty()) {
            return;
        }
        namespace proc = kimix::builtin_tools::proc;
        proc::run_options opts;
        opts.argv = {bash, "--noprofile", "--norc", "-c", "sleep 30"};
        opts.timeout_ms = 1000;
        const int64_t t0 = static_cast<int64_t>(kimix::Clock::now_ms());
        const proc::run_result rr = proc::run_process(opts);
        const int64_t elapsed = kimix::Clock::now_ms() - t0;
        expect(rr.killed);
        expect(elapsed < 10000) << elapsed;
    };

    "process_runner_interactive_task"_test = [] {
        const kimix::string bash =
            kimix::builtin_tools::bash::Bash::detect_bash_path();
        if (bash.empty()) {
            return;
        }
        namespace proc = kimix::builtin_tools::proc;
        proc::run_options opts;
        opts.argv = {bash, "--noprofile", "--norc", "-i"};
        proc::task_handle h;
        const auto terr = proc::start_task(opts, h);
        expect(!terr.failed()) << terr.message;
        expect(!h.task_id.empty());
        expect(!proc::send_task(h.task_id, "echo interactive_ok", true).failed());
        const auto tw = proc::wait_task(h.task_id, "interactive_ok", 15000);
        expect(tw.matched);
        kimix::string out;
        expect(!proc::read_task(h.task_id, out).failed());
        expect(out.find("interactive_ok") != kimix::string::npos);
        expect(!proc::stop_task(h.task_id).failed());
        expect(!proc::query_task(h.task_id).exists);
        proc::stop_all_tasks();
    };

    // =======================================================================
    // Native Read / Write / Grep / Bash through the Tool interface
    // =======================================================================
    "native_write_then_read"_test = [] {
        const kimix::string ws = tmp_workspace();
        Session session;
        session.work_dir = ws;
        session.native_io = true;

        kimix::builtin_tools::write::Write w(&session);
        ToolParams wp = parse_json(R"JSON({"file_path":"out/note.txt","content":"alpha\nbeta\ngamma\n","mode":"overwrite","mkdir":true})JSON");
        w(&wp);
        const auto &wr = w.last_result().values;
        expect(eq(wr.at("status").as_string(), kimix::string("ok")))
            << wr.at("message").as_string();
        std::error_code ec;
        expect(kimix::filesystem::exists(
            kimix::filesystem::path(ws) / "out" / "note.txt", ec));

        kimix::builtin_tools::read::Read r(&session);
        ToolParams rp = parse_json(R"JSON({"file_path":"out/note.txt"})JSON");
        r(&rp);
        ToolParams rr;
        const kimix::string rs = result_string(r.serialized_result());
        rr.deserialize(kimix::span<char const>(rs.data(), rs.size()));
        expect(eq(rr.get("status")->as_string(), kimix::string("ok")));
        expect(rr.get("output")->as_string().find("beta") != kimix::string::npos);
    };

    "native_grep_search"_test = [] {
        const kimix::string ws = tmp_workspace();
        Session session;
        session.work_dir = ws;
        session.native_io = true;
        kimix::builtin_tools::write::Write w(&session);
        ToolParams w1 = parse_json(R"JSON({"file_path":"a.cpp","content":"int needle_here = 1;\n","mkdir":true})JSON");
        w(&w1);
        ToolParams w2 = parse_json(R"JSON({"file_path":"b.txt","content":"nothing useful\nneedle too\n","mkdir":true})JSON");
        w(&w2);

        kimix::builtin_tools::grep::Grep g(&session);
        ToolParams gp;
        gp.values["pattern"] = ValueElement::make_string(kimix::string("needle"));
        gp.values["paths"] = ValueElement::make_string(kimix::string("."));
        gp.values["output_mode"] =
            ValueElement::make_string(kimix::string("files_with_matches"));
        g(&gp);
        ToolParams gr;
        const kimix::string gs = result_string(g.serialized_result());
        gr.deserialize(kimix::span<char const>(gs.data(), gs.size()));
        expect(eq(gr.get("status")->as_string(), kimix::string("ok")));
        expect(eq(gr.get("file_count")->as_int(), int64_t(2)));

        // Content mode with a regex.
        ToolParams gp2;
        gp2.values["pattern"] =
            ValueElement::make_string(kimix::string("needle_\\w+"));
        gp2.values["paths"] = ValueElement::make_string(kimix::string("."));
        gp2.values["output_mode"] =
            ValueElement::make_string(kimix::string("content"));
        g(&gp2);
        ToolParams gr2;
        const kimix::string gs2 = result_string(g.serialized_result());
        gr2.deserialize(kimix::span<char const>(gs2.data(), gs2.size()));
        expect(eq(gr2.get("status")->as_string(), kimix::string("ok")));
        expect(gr2.get("output")->as_string().find("needle_here") !=
               kimix::string::npos);
    };

    "native_glob_and_bash"_test = [] {
        const kimix::string ws = tmp_workspace();
        Session session;
        session.work_dir = ws;
        session.native_io = true;
        kimix::builtin_tools::write::Write w(&session);
        ToolParams w1 = parse_json(R"JSON({"file_path":"src/main.cpp","content":"int main(){}\n","mkdir":true})JSON");
        w(&w1);

        auto reg_tool =
            kimix::builtin_tools::ToolRegistry::instance().create("glob", &session);
        expect(reg_tool != nullptr);
        ToolParams gp = parse_json(R"JSON({"pattern":"**/*.cpp","path":"src"})JSON");
        (*reg_tool)(&gp);
        kimix::vector<char> out;
        reg_tool->result_json(out);
        const kimix::string os(out.data(), out.size());
        expect(os.find("main.cpp") != kimix::string::npos) << os;

        // Bash: real command through reproc (skip without bash).
        if (kimix::builtin_tools::bash::Bash::detect_bash_path().empty()) {
            return;
        }
        auto bash_tool =
            kimix::builtin_tools::ToolRegistry::instance().create("bash", &session);
        expect(bash_tool != nullptr);
        ToolParams bp2;
        bp2.values["cmd"] = ValueElement::make_string(
            kimix::string("cd '") + ws + "' && ls src && echo E2E_OK");
        bp2.values["timeout"] = ValueElement::make_int(30);
        (*bash_tool)(&bp2);
        kimix::vector<char> bout;
        bash_tool->result_json(bout);
        const kimix::string bs(bout.data(), bout.size());
        expect(bs.find("E2E_OK") != kimix::string::npos) << bs;
        expect(bs.find("main.cpp") != kimix::string::npos) << bs;
    };

    // =======================================================================
    // KimiSoul
    // =======================================================================
    "soul_tool_call_with_json_repair"_test = [] {
        const kimix::string ws = tmp_workspace();
        kimix::agent::AgentSession session(ws);
        expect(eq(session.tool_session().work_dir, ws));
        expect(session.tool_session().native_io);
        expect(!session.id().empty());

        FakeBackend backend;
        // Step 1: the model emits a Write tool call with BROKEN JSON
        // (trailing comma + unquoted key) -> the soul must repair it.
        kimix::llm::ChatResult step1;
        step1.ok = true;
        kimix::llm::ToolCall tc;
        tc.id = "call_1";
        tc.name = "write";
        tc.arguments = R"JSON({file_path: "soul_note.txt", "content": "written by soul",})JSON";
        step1.tool_calls.push_back(tc);
        // Step 2: a Read tool call to verify.
        kimix::llm::ChatResult step2;
        step2.ok = true;
        kimix::llm::ToolCall tc2;
        tc2.id = "call_2";
        tc2.name = "read"; // case-insensitive registry lookup
        tc2.arguments = R"JSON({"file_path":"soul_note.txt"})JSON";
        step2.tool_calls.push_back(tc2);
        // Step 3: final text.
        kimix::llm::ChatResult step3;
        step3.ok = true;
        step3.content = "All done.";
        backend.scripted = {step1, step2, step3};
          backend.scripted = {step1, step2, step3};
          // The verification gate (default ON since Phase 1) would nudge this
          // turn - it writes a file without running a verification command -
          // which is reference-faithful but unrelated to what this test
          // exercises (tool-call JSON repair). Disable it here.
          kimix::agent::KimiSoul::options opts;
          opts.loop_control.verification_gate_enabled = false;
          kimix::agent::KimiSoul soul(session, backend, opts);
          const kimix::agent::TurnResult tr = soul.turn("write and read a note");
        expect(tr.ok) << tr.error;
        expect(eq(tr.content, kimix::string("All done.")));
        expect(eq(tr.steps, 3));

          // The tool results landed in the history wrapped in the reference's
          // <system> envelope (E3, message.py tool_result_to_message): the raw
          // result JSON is gone, the human message is the envelope head and
          // the payload text follows it.
          bool saw_write_ok = false;
          bool saw_read_ok = false;
          for (const kimix::llm::Message &m : session.history()) {
              if (m.role == "tool" && m.tool_call_id == "call_1") {
                  saw_write_ok = m.content.find("<system>") == 0 &&
                                 m.content.find("\"status\":") == kimix::string::npos;
              }
              if (m.role == "tool" && m.tool_call_id == "call_2") {
                  saw_read_ok = m.content.find("written by soul") != kimix::string::npos;
              }
          }
        expect(saw_write_ok);
        expect(saw_read_ok);
        std::error_code ec;
        expect(kimix::filesystem::exists(
            kimix::filesystem::path(ws) / "soul_note.txt", ec));
    };

      "soul_unknown_tool_and_bad_json"_test = [] {
          kimix::agent::AgentSession session(tmp_workspace());
          kimix::string err;
          FakeBackend backend;
          kimix::agent::KimiSoul soul(session, backend);
          const kimix::string out = soul.execute_tool_call("NoSuchTool", "{}", err);
          expect(!err.empty());
          // F8 (kosong/tooling/error.py:4-14): an unknown name is the typed
          // ToolNotFoundError. "NoSuchTool" is far from every offered name,
          // so there are no suggestions and the message is the bare form.
          expect(out.find("Tool `NoSuchTool` not found") != kimix::string::npos);
          // Unrepairable JSON surfaces as the typed parse error, not a crash
          // (F10 ToolParseError wording).
          err.clear();
          const kimix::string out2 =
              soul.execute_tool_call("read", "]]not json[[", err);
          expect(!err.empty());
          expect(out2.find("Error parsing JSON arguments") != kimix::string::npos);
      };
      "soul_dispatch_refuses_disabled_tool"_test = [] {
          // F1: tool_definitions() filters by enabled_tools; dispatch must
          // enforce the same allow-list (toolset.py only builds _tool_dict
          // from the enabled tools).  A restricted agent cannot be driven to
          // a tool the manifest did not enable.
          kimix::agent::AgentSession session(tmp_workspace());
          FakeBackend backend;
          kimix::agent::KimiSoul::options opts;
          opts.enabled_tools = {"read"};
          kimix::agent::KimiSoul soul(session, backend, opts);
          expect(eq(soul.tool_definitions().size(), static_cast<size_t>(1)));
          kimix::string err;
          const kimix::string out =
              soul.execute_tool_call("write", R"JSON({"file_path":"x.txt","content":"y"})JSON", err);
          expect(!err.empty());
          expect(out.find("tool is not enabled in this session") != kimix::string::npos)
              << out;
          // The tool genuinely did not run.
          std::error_code ec;
          expect(!kimix::filesystem::exists(kimix::filesystem::path(session.work_dir()) /
                                                "x.txt",
                                            ec));
          // An enabled tool still dispatches (read of a missing file is a
          // not_found tool result, not a refusal).
          err.clear();
          const kimix::string out2 =
              soul.execute_tool_call("read", R"JSON({"file_path":"x.txt"})JSON", err);
          expect(out2.find("not enabled") == kimix::string::npos) << out2;
          // Unknown names keep the typed not-found error (F8).
          err.clear();
          const kimix::string out3 = soul.execute_tool_call("NoSuchTool", "{}", err);
          expect(out3.find("Tool `NoSuchTool` not found") != kimix::string::npos) << out3;
      };
      "soul_read_only_refuses_mutating_tools"_test = [] {
          // F2: the reference's read-only guard (toolset.py:93-105 blocklist,
          // 1347-1362 refusal), enforced at dispatch.
          kimix::agent::AgentSession session(tmp_workspace());
          FakeBackend backend;
          kimix::agent::KimiSoul::options opts;
          opts.read_only = true;
          kimix::agent::KimiSoul soul(session, backend, opts);
          // The exact refusal text for tools that are dispatchable here (the
          // reference fires the guard after the tool is resolved, so a tool
          // this environment cannot run keeps the "not available" error -
          // either way the call does not execute).
          for (const char *blocked : {"edit", "write", "job_output"}) {
              kimix::string err;
                const kimix::string out = soul.execute_tool_call(blocked, "{}", err);
                expect(eq(err,
                          kimix::string("Tool '") + blocked +
                              "' is forbidden in read-only mode. The agent should "
                              "quit the conversation immediately."))
                    << blocked << ": " << err;
                // E3: the refusal reaches the model inside the error envelope.
                expect(eq(out,
                          kimix::string("<system>ERROR: ") + err + "</system>"))
                    << blocked << ": " << out;
            }
          // The rest of the reference's blocklist (toolset.py:93-105) is
          // refused too - when the tool cannot run here the model still gets
          // an explicit refusal instead of an execution.
          for (const char *blocked :
               {"bash", "pwsh", "python", "workflow", "subagent",
                "interrupt_agent"}) {
              kimix::string err;
              const kimix::string out = soul.execute_tool_call(blocked, "{}", err);
              const bool refused =
                  out.find("forbidden in read-only mode") != kimix::string::npos ||
                  out.find("not available in this environment") != kimix::string::npos ||
                  out.find("unknown tool") != kimix::string::npos;
              expect(refused) << blocked << ": " << out;
          }
          // Case-insensitive like the reference's name resolution: "BASH"
          // resolves to the bash tool and is blocked.
          {
              kimix::string err;
              const kimix::string out = soul.execute_tool_call("BASH", "{}", err);
              expect(out.find("forbidden in read-only mode") != kimix::string::npos ||
                     out.find("not available in this environment") != kimix::string::npos)
                  << out;
          }
          // Read-only tools still dispatch: read reports a file result (not a
          // refusal), glob runs.
          {
              kimix::string err;
              const kimix::string out =
                  soul.execute_tool_call("read", R"JSON({"file_path":"x.txt"})JSON", err);
              expect(out.find("forbidden in read-only mode") == kimix::string::npos)
                  << out;
          }
          {
              kimix::string err;
              const kimix::string out =
                  soul.execute_tool_call("glob", R"JSON({"pattern":"*"})JSON", err);
              expect(out.find("forbidden in read-only mode") == kimix::string::npos)
                  << out;
          }
          // Without read_only the same write call dispatches.
          kimix::agent::AgentSession session2(tmp_workspace());
          FakeBackend backend2;
          kimix::agent::KimiSoul soul2(session2, backend2);
          kimix::string err2;
          const kimix::string out2 = soul2.execute_tool_call(
              "write", R"JSON({"file_path":"ro.txt","content":"z"})JSON", err2);
        expect(out2.find("forbidden in read-only mode") == kimix::string::npos)
            << out2;
    };

    "soul_compact_context"_test = [] {
        kimix::agent::AgentSession session(tmp_workspace());
        FakeBackend backend;
        // The compaction LLM call returns a canned summary.
        kimix::llm::ChatResult summary;
        summary.ok = true;
        summary.content = "<current_focus>compacted</current_focus>";
        backend.scripted = {summary};

        kimix::agent::KimiSoul soul(session, backend);
        // Build a long history: user/assistant/tool triples.
        auto &h = session.history();
        for (int i = 0; i < 8; ++i) {
            kimix::llm::Message u;
            u.role = "user";
            u.content = kimix::format("question {} with some padding text to "
                                      "make the token estimate grow", i);
            h.push_back(u);
            kimix::llm::Message a;
            a.role = "assistant";
            a.content = kimix::format("answer {} plus a longer explanation "
                                      "that should be compacted away", i);
            kimix::llm::ToolCall tc;
            tc.id = kimix::format("c{}", i);
            tc.name = "bash";
            tc.arguments = "{\"cmd\":\"ls\"}";
            a.tool_calls.push_back(tc);
            h.push_back(a);
            kimix::llm::Message t;
            t.role = "tool";
            t.tool_call_id = tc.id;
            t.content = "file1\nfile2\n";
            h.push_back(t);
        }
        const size_t before = h.size();
        kimix::string err;
        expect(soul.compact_context("keep the tests", err, /*manual=*/true)) << err;
        expect(h.size() < before);
        expect(eq(soul.compaction_count(), 1));
        // First message is the summary; the tail must not start with an orphan
        // tool message.
        expect(h.front().content.find("compacted") != kimix::string::npos);
        expect(h[1].role != "tool");
        // The compacted tail still contains the most recent exchange.
        expect(h.back().content.find("file1") != kimix::string::npos ||
               h[h.size() - 2].content.find("file1") != kimix::string::npos);
    };

    "soul_auto_compact_in_turn"_test = [] {
        kimix::agent::AgentSession session(tmp_workspace());
        FakeBackend backend;
        kimix::agent::KimiSoul::options opts;
        opts.auto_compact = true;
        opts.reserved_context = 8192;
        // Force compaction by using a tiny context window.
        class TinyBackend : public kimix::agent::IChatBackend {
        public:
            kimix::vector<kimix::llm::ChatResult> scripted;
            size_t index = 0;
            kimix::llm::ChatResult
            chat(const kimix::vector<kimix::llm::Message> &messages,
                 const kimix::vector<kimix::llm::Tool> &tools,
                 const kimix::llm::ChunkCallback &on_chunk) override {
                (void)tools;
                (void)on_chunk;
                (void)messages;
                if (index < scripted.size()) {
                    return scripted[index++];
                }
                kimix::llm::ChatResult r;
                r.ok = true;
                r.content = "final";
                return r;
            }
            int64_t max_context_size() const override { return 200; }
            kimix::string model_name() const override { return "tiny"; }
        };
        TinyBackend tiny;
        kimix::llm::ChatResult summary;
        summary.ok = true;
        summary.content = "<current_focus>summarized</current_focus>";
        kimix::llm::ChatResult final_text;
        final_text.ok = true;
        final_text.content = "wrapped up";
        tiny.scripted = {summary, final_text};

        kimix::agent::KimiSoul soul(session, tiny, opts);
        // Seed a long history so the estimate exceeds the tiny window.
        auto &h = session.history();
        for (int i = 0; i < 6; ++i) {
            kimix::llm::Message u;
            u.role = "user";
            u.content = kimix::string(500, 'x');
            h.push_back(u);
            kimix::llm::Message a;
            a.role = "assistant";
            a.content = kimix::string(500, 'y');
            h.push_back(a);
        }
        const kimix::agent::TurnResult tr = soul.turn("continue please");
        expect(tr.ok) << tr.error;
        expect(tr.compacted);
        expect(eq(soul.compaction_count(), 1));
        expect(eq(tr.content, kimix::string("wrapped up")));
    };

    "soul_estimated_tokens_includes_system_prompt_and_tools"_test = [] {
        // The estimate behind context usage / should_auto_compact must carry
        // the same non-history overhead as the reference's
        // token_count_with_pending (kimisoul.py:1230: "also carries the system
        // prompt and tool schemas"; post-compaction re-add at 2223-2229):
        // effective system prompt + every offered tool's name/description/
        // parameters, on top of the history text.
        kimix::agent::AgentSession session(tmp_workspace());
        FakeBackend backend;
        kimix::agent::KimiSoul::options opts;
        opts.system_prompt = "custom prompt for the token estimate test";
        opts.enabled_tools = {"read"};
        kimix::agent::KimiSoul soul(session, backend, opts);

        using kimix::builtin_tools::compact::estimate_text_tokens;
        int64_t overhead = estimate_text_tokens(opts.system_prompt);
        const kimix::vector<kimix::llm::Tool> defs = soul.tool_definitions();
        expect(eq(defs.size(), static_cast<size_t>(1))) << "enabled_tools=read";
        for (const kimix::llm::Tool &t : defs) {
            overhead += estimate_text_tokens(t.name);
            overhead += estimate_text_tokens(t.description);
            overhead += estimate_text_tokens(t.parameters_json);
        }

        // Empty history: the estimate is exactly the per-request overhead.
        expect(eq(soul.estimated_tokens(), overhead));
        expect(soul.estimated_tokens() > 0);

        // Appending history adds the message text on top of the overhead.
        kimix::llm::Message u;
        u.role = "user";
        u.content = "hello world";
        session.history().push_back(u);
        expect(eq(soul.estimated_tokens(),
                  overhead + estimate_text_tokens(u.content)));
    };

    "soul_auto_compact_uses_overhead_estimate"_test = [] {
        // With the corrected estimate the compaction timing changes: a history
        // whose text alone stays below every trigger boundary must still
        // auto-compact once the system prompt + tool descriptions are counted,
        // because the full next-request input is what the reference feeds
        // should_auto_compact (kimisoul.py:1197-1206 via
        // token_count_with_pending).
        kimix::agent::AgentSession session(tmp_workspace());
        FakeBackend backend;
        kimix::agent::KimiSoul::options opts;
        opts.auto_compact = true;
        opts.auto_compact_ratio = 0.75;
        opts.reserved_context = 64;
        opts.system_prompt = "compact trigger overhead probe prompt";

        class OverheadBackend : public kimix::agent::IChatBackend {
        public:
            kimix::vector<kimix::llm::ChatResult> scripted;
            size_t index = 0;
            int64_t window = 0;
            kimix::llm::ChatResult
            chat(const kimix::vector<kimix::llm::Message> &messages,
                 const kimix::vector<kimix::llm::Tool> &tools,
                 const kimix::llm::ChunkCallback &on_chunk) override {
                (void)messages;
                (void)tools;
                (void)on_chunk;
                if (index < scripted.size()) {
                    return scripted[index++];
                }
                kimix::llm::ChatResult r;
                r.ok = true;
                r.content = "final";
                return r;
            }
            int64_t max_context_size() const override { return window; }
            kimix::string model_name() const override { return "probe"; }
        };
        OverheadBackend probe;
        kimix::agent::KimiSoul sizing(session, probe, opts);

        // Per-request overhead: system prompt + all offered tool schemas
        // (default toolset; this environment's validity gate decides the set).
        using kimix::builtin_tools::compact::estimate_text_tokens;
        int64_t overhead = estimate_text_tokens(opts.system_prompt);
        for (const kimix::llm::Tool &t : sizing.tool_definitions()) {
            overhead += estimate_text_tokens(t.name);
            overhead += estimate_text_tokens(t.description);
            overhead += estimate_text_tokens(t.parameters_json);
        }

        // Seed a history of ~6000 tokens of ASCII - far below any trigger
        // boundary on its own.
        auto &h = session.history();
        for (int i = 0; i < 6; ++i) {
            kimix::llm::Message u;
            u.role = "user";
            u.content = kimix::string(2000, 'x');
            h.push_back(u);
            kimix::llm::Message a;
            a.role = "assistant";
            a.content = kimix::string(2000, 'y');
            h.push_back(a);
        }
        const int64_t history_only =
            sizing.estimated_tokens() - overhead; // history text basis, pre-fix

        // Window midpoint between the two ratio boundaries:
        //   0.75 * W == H + O/2
        // so the history text alone (H) stays under the ratio boundary while
        // the corrected total (H + O) crosses it. H + 2O > 3072 also keeps the
        // history-only basis clear of the reserved boundary (1024 tokens).
        const int64_t window = (4 * history_only + 2 * overhead) / 3;
        expect(history_only + 2 * overhead > 3072);
        probe.window = window;

        kimix::builtin_tools::compact::compaction_trigger_config trigger;
        trigger.trigger_ratio = opts.auto_compact_ratio;
        trigger.max_context_size = window;
        trigger.reserved_context_size = opts.reserved_context;
        trigger.max_tokens = opts.max_tokens;
        trigger.tool_call_buffer_tokens = opts.tool_call_buffer_tokens;
        trigger.safety_margin_tokens = 1024;
        // Pre-fix basis (history only): neither the ratio nor the reserved
        // boundary is crossed.
        expect(!kimix::builtin_tools::compact::should_auto_compact(history_only,
                                                                   trigger));
        // Corrected basis (history + system prompt + tool schemas): the ratio
        // boundary is crossed.
        expect(kimix::builtin_tools::compact::should_auto_compact(
            history_only + overhead, trigger));

        kimix::llm::ChatResult summary;
        summary.ok = true;
        summary.content = "<current_focus>summarized</current_focus>";
        kimix::llm::ChatResult final_text;
        final_text.ok = true;
        final_text.content = "wrapped up";
        probe.scripted = {summary, final_text};

        const kimix::agent::TurnResult tr = sizing.turn("continue please");
        expect(tr.ok) << tr.error;
        expect(tr.compacted);
        expect(eq(sizing.compaction_count(), 1));
    };

    "soul_blank_input_is_ignored"_test = [] {
        // Port of kimi_cli.soul._user_input_is_empty / run_soul's empty-input
        // guard: a blank prompt must not start a turn. Pre-fix the soul appended
        // an empty `user` message and the model answered a spurious "you sent an
        // empty message" turn.
        kimix::agent::AgentSession session(tmp_workspace());
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);

        const kimix::llm::ChatResult reply = [] {
            kimix::llm::ChatResult r;
            r.ok = true;
            r.content = "should never be produced";
            return r;
        }();

        const char *blank[] = {"", " ", "\t\n\r", "\v\f", "\x1c\x1d\x1e\x1f",
                               // U+00A0 NO-BREAK SPACE, U+3000 IDEOGRAPHIC SPACE,
                               // U+2028 LINE SEPARATOR (all stripped by str.strip()).
                               "\xc2\xa0", "\xe3\x80\x80", "\xe2\x80\xa8", " \xc2\xa0 "};
        for (const char *input : blank) {
            backend.scripted = {reply};
            backend.index = 0;
            const kimix::agent::TurnResult tr = soul.turn(input);
            expect(tr.ignored) << "input bytes: " << input;
            expect(!tr.ok);
            expect(eq(tr.steps, 0));
            expect(tr.content.empty());
            expect(tr.error.empty());
            // No history mutation and no LLM call.
            expect(eq(session.history().size(), static_cast<size_t>(0)));
            expect(eq(backend.requests.size(), static_cast<size_t>(0)));
        }

        // A lone 0xFF byte is invalid UTF-8: it decodes to U+FFFD (not
        // whitespace) by design, so it counts as content. Python str cannot hold
        // invalid UTF-8, so there is no reference case to mirror.
        const kimix::agent::TurnResult junk = soul.turn(kimix::string("\xff"));
        expect(!junk.ignored);
        expect(eq(junk.steps, 1));
        expect(eq(session.history().size(), static_cast<size_t>(2))); // user + assistant

        // The helper itself.
        expect(kimix::agent::agent_user_input_is_empty("  \t\n "));
        expect(kimix::agent::agent_user_input_is_empty(""));
        expect(!kimix::agent::agent_user_input_is_empty("x"));
        expect(!kimix::agent::agent_user_input_is_empty("\t x \t"));
    };

    "soul_compaction_preserves_first_message"_test = [] {
        // Phase 6 (SimpleCompaction.prepare, compaction.py:741-747): the very
        // first message is always re-inserted at the head of the preserved tail
        // (primacy bias), so a compaction never loses the original prompt.
        // Reference (verified live against SimpleCompaction.prepare):
        //   [u0, a0(tc), t0, u1, a1(tc), t1, u2, a2, u3]
        //   -> to_preserve == [u0, u3]   (adaptive preserve depth 1)
        kimix::agent::AgentSession session(tmp_workspace());
        FakeBackend backend;
        kimix::llm::ChatResult summary;
        summary.ok = true;
        summary.content = "SUMMARY";
        backend.scripted = {summary};

        auto &h = session.history();
        auto push = [&h](const char *role, const char *text, int calls) {
            kimix::llm::Message m;
            m.role = role;
            m.content = text;
            for (int i = 0; i < calls; ++i) {
                kimix::llm::ToolCall tc;
                tc.id = kimix::format("c{}", i);
                tc.name = "T";
                tc.arguments = "{}";
                m.tool_calls.push_back(tc);
            }
            h.push_back(std::move(m));
        };
        push("user", "u0", 0);
        push("assistant", "a0", 1);
        push("tool", "t0", 0);
        push("user", "u1", 0);
        push("assistant", "a1", 1);
        push("tool", "t1", 0);
        push("user", "u2", 0);
        push("assistant", "a2", 0);
        push("user", "u3", 0);

        kimix::agent::KimiSoul soul(session, backend);
        kimix::string err;
        expect(soul.compact_context("", err, /*manual=*/true)) << err;
        expect(eq(soul.compaction_count(), 1));
        expect(eq(h.size(), static_cast<size_t>(3)));
        // Summary first, then the primacy copy of u0, then the preserved tail.
        expect(h[0].content.find("SUMMARY") != kimix::string::npos);
        expect(eq(h[1].role, kimix::string("user")));
        expect(eq(h[1].content, kimix::string("u0"))) << h[1].content;
        expect(eq(h[2].content, kimix::string("u3")));
    };

    "soul_compaction_keeps_tool_pairs_intact"_test = [] {
        // The balanced-cut snap (kimi_cli/soul/tool_pairing.py) keeps the
        // preserved tail off a mid call/result pair. Reference:
        //   [u0, a0(tc), t0, u1, a1(tc), t1, u2, a2(tc), t2]
        //   -> to_preserve == [u0, a2, t2]
        kimix::agent::AgentSession session(tmp_workspace());
        FakeBackend backend;
        kimix::llm::ChatResult summary;
        summary.ok = true;
        summary.content = "SUMMARY";
        backend.scripted = {summary};

        auto &h = session.history();
        auto push = [&h](const char *role, const char *text, int calls) {
            kimix::llm::Message m;
            m.role = role;
            m.content = text;
            for (int i = 0; i < calls; ++i) {
                kimix::llm::ToolCall tc;
                tc.id = kimix::format("c{}", i);
                tc.name = "T";
                tc.arguments = "{}";
                m.tool_calls.push_back(tc);
            }
            h.push_back(std::move(m));
        };
        push("user", "u0", 0);
        push("assistant", "a0", 1);
        push("tool", "t0", 0);
        push("user", "u1", 0);
        push("assistant", "a1", 1);
        push("tool", "t1", 0);
        push("user", "u2", 0);
        push("assistant", "a2", 1);
        push("tool", "t2", 0);

        kimix::agent::KimiSoul soul(session, backend);
        kimix::string err;
        expect(soul.compact_context("", err, /*manual=*/true)) << err;
        expect(eq(h.size(), static_cast<size_t>(4)));
        expect(h[0].content.find("SUMMARY") != kimix::string::npos);
        // Summary marker matches the reference build (compaction.py:626-628).
        expect(h[0].content.find("Previous context has been compacted") !=
               kimix::string::npos);
        expect(eq(h[1].content, kimix::string("u0")));
        expect(eq(h[2].content, kimix::string("a2")));
        expect(eq(h[3].content, kimix::string("t2")));
        // No orphan tool result at the head of the preserved tail.
        expect(h.size() > 1 && h[1].role != "tool");
        expect(!h[2].tool_calls.empty());
        expect(eq(h[3].role, kimix::string("tool")));

        // A leftover tool result with no matching call cannot be compacted: the
        // reference raises ValueError out of prepare(); the port refuses instead.
        kimix::agent::AgentSession bad_session(tmp_workspace());
        FakeBackend bad_backend;
        auto &bh = bad_session.history();
        kimix::llm::Message u;
        u.role = "user";
        u.content = "u0";
        bh.push_back(u);
        kimix::llm::Message orphan;
        orphan.role = "tool";
        orphan.content = "t0";
        bh.push_back(orphan);
        kimix::llm::Message u2;
        u2.role = "user";
        u2.content = "u1";
        bh.push_back(u2);
        kimix::agent::KimiSoul bad_soul(bad_session, bad_backend);
        kimix::string bad_err;
      expect(!bad_soul.compact_context("", bad_err, /*manual=*/true));
      expect(bad_err.find("unbalanced") != kimix::string::npos) << bad_err;
      expect(eq(bad_soul.compaction_count(), 0));
      expect(eq(bad_session.history().size(), static_cast<size_t>(3)));
  };

    "soul_compaction_shrink_check_noop"_test = [] {
        // C1 (compaction.py:621-659): a summary that is NOT smaller than the
        // region it replaces is discarded - the verbatim history survives and
        // the compaction is a no-op, never a destructive replacement.
        kimix::agent::AgentSession session(tmp_workspace());
        FakeBackend backend;
        kimix::llm::ChatResult long_summary;
        long_summary.ok = true;
        long_summary.content = kimix::string(2000, 's'); // far larger than the region
        backend.scripted = {long_summary};
        auto &h = session.history();
        auto push = [&h](const char *role, const char *text) {
            kimix::llm::Message m;
            m.role = role;
            m.content = text;
            h.push_back(std::move(m));
        };
        push("user", "u0");
        push("assistant", "a0");
        push("user", "u1");
        push("assistant", "a1");
        const size_t before = h.size();
        kimix::agent::KimiSoul soul(session, backend);
        kimix::string err;
        // A no-op compaction is NOT an error (the reference degrades to a
        // no-op CompactionResult and the flow continues).
        expect(soul.compact_context("", err, /*manual=*/true)) << err;
        expect(eq(soul.compaction_count(), 0));
        expect(eq(h.size(), before));
        expect(eq(h[0].content, kimix::string("u0")));
        expect(eq(h[1].content, kimix::string("a0")));
        expect(eq(h[2].content, kimix::string("u1")));
        expect(eq(h[3].content, kimix::string("a1")));
    };

    "soul_compaction_surface_changed_retry"_test = [] {
        // C2 (compaction.py _surface_fingerprint + kimisoul.py
        // _compact_with_stability_retry): a conversation that changes while
        // the summary is generated raises SurfaceChangedError, re-prepares
        // and retries ONCE; a stable second attempt applies.
        class MutatingBackend : public kimix::agent::IChatBackend {
        public:
            kimix::vector<kimix::llm::ChatResult> scripted;
            kimix::agent::AgentSession *target = nullptr;
            size_t mutate_calls = 0; // inject a message on the first N calls
            size_t index = 0;
            size_t calls = 0;
            kimix::llm::ChatResult
            chat(const kimix::vector<kimix::llm::Message> &messages,
                 const kimix::vector<kimix::llm::Tool> &tools,
                 const kimix::llm::ChunkCallback &on_chunk) override {
                (void)tools;
                (void)on_chunk;
                (void)messages;
                ++calls;
                if (target != nullptr && calls <= mutate_calls) {
                    kimix::llm::Message injected;
                    injected.role = "user";
                    injected.content = "injected mid-compaction";
                    target->history().push_back(injected);
                }
                if (index < scripted.size()) {
                    return scripted[index++];
                }
                kimix::llm::ChatResult r;
                r.ok = true;
                r.content = "SUMMARY";
                return r;
            }
            int64_t max_context_size() const override { return 128000; }
            kimix::string model_name() const override { return "mutating"; }
        };
        auto run_case = [](bool manual, size_t mutate_calls, bool expect_ok,
                           int32_t expect_compactions, const char *expect_error) {
            kimix::agent::AgentSession session(tmp_workspace());
            MutatingBackend backend;
            backend.target = &session;
            backend.mutate_calls = mutate_calls;
            kimix::llm::ChatResult s;
            s.ok = true;
            s.content = "SUMMARY";
            backend.scripted = {s, s};
            auto &h = session.history();
            auto push = [&h](const char *role, const char *text) {
                kimix::llm::Message m;
                m.role = role;
                m.content = text;
                h.push_back(std::move(m));
            };
            push("user", "u0");
            push("assistant", "a0");
            push("user", "u1");
            push("assistant", "a1");
            const size_t before = h.size();
            kimix::agent::KimiSoul soul(session, backend);
            kimix::string err;
            expect(eq(soul.compact_context("", err, manual), expect_ok)) << err;
            expect(eq(soul.compaction_count(), expect_compactions));
            if (expect_error != nullptr) {
                expect(eq(err, kimix::string(expect_error))) << err;
                // The caller's own mid-compaction appends remain (the reference
                // never rolls those back); what must NOT happen is the summary
                // being applied - the head stays verbatim.
                expect(eq(h.size(), before + mutate_calls));
                expect(eq(h[0].content, kimix::string("u0")))
                    << "a failed transaction never applies the summary";
            } else {
                expect(eq(soul.compaction_count(), 1));
            }
            return backend.calls;
        };
        // One transient change: the retry re-prepares on the new surface and
        // applies (two LLM calls total).
        expect(eq(run_case(/*manual=*/true, /*mutate_calls=*/1, /*expect_ok=*/true,
                           /*expect_compactions=*/1, nullptr),
                 static_cast<size_t>(2)));
        // A second SurfaceChangedError is a classified failure: manual ->
        // ManualCompactionError("changed") -> slash.py's user-facing text.
        expect(eq(run_case(/*manual=*/true, /*mutate_calls=*/2, /*expect_ok=*/false,
                           /*expect_compactions=*/0,
                           "History changed during compaction; try again."),
                 static_cast<size_t>(2)));
        // Auto classification reports the plain cause instead.
        expect(eq(run_case(/*manual=*/false, /*mutate_calls=*/2, /*expect_ok=*/false,
                           /*expect_compactions=*/0,
                           "conversation changed during compaction"),
                 static_cast<size_t>(2)));
    };

      // =======================================================================
      // E3: tool-result envelope (message.py tool_result_to_message)
      // =======================================================================
      "soul_tool_result_envelope_contract"_test = [] {
          const kimix::string ws = tmp_workspace();
          kimix::agent::AgentSession session(ws);
          FakeBackend backend;
          kimix::agent::KimiSoul soul(session, backend);
          kimix::string err;
          // Success with a message: "<system>{message}</system>" head, raw JSON
          // gone from the model-facing text.
          const kimix::string ok =
              soul.execute_tool_call("write", R"JSON({"file_path":"env.txt","content":"data"})JSON", err);
          expect(err.empty()) << err;
          expect(ok.find("<system>") == 0) << ok;
          expect(ok.find("</system>") != kimix::string::npos) << ok;
          expect(ok.find("\"status\":") == kimix::string::npos) << ok;
          // A deterministic tool failure (file missing -> not_found) is an
          // error envelope WITHOUT the runtime-error sentence.
          err.clear();
          const kimix::string missing =
              soul.execute_tool_call("read", R"JSON({"file_path":"nope.txt"})JSON", err);
          expect(missing.find("<system>ERROR: ") == 0) << missing;
          expect(missing.find("unexpected error") == kimix::string::npos) << missing;
          // A generic runtime failure (status "error", the reference's
          // ToolRuntimeError) earns the "unexpected error" sentence. The pwsh
          // kernel's fix mode reports status "error" for a non-ASCII command;
          // it needs a PowerShell host, so probe the offer first.
          bool pwsh_offered = false;
          for (const kimix::llm::Tool &d : soul.tool_definitions()) {
              if (d.name == "pwsh") {
                  pwsh_offered = true;
              }
          }
          if (pwsh_offered) {
              err.clear();
              const kimix::string runtime = soul.execute_tool_call(
                  "pwsh", "{\"mode\":\"fix\",\"command\":\"echo \xc3\xa9\"}", err);
              expect(runtime.find("<system>ERROR: ") == 0) << runtime;
              expect(runtime.find("This is an unexpected error and the tool is probably "
                                  "not working.") != kimix::string::npos)
                  << runtime;
          }
          // Dispatch-level failures are error envelopes too (F8 wording).
          err.clear();
          const kimix::string unknown = soul.execute_tool_call("NoSuchTool", "{}", err);
          expect(eq(unknown,
                    kimix::string("<system>ERROR: Tool `NoSuchTool` not found</system>")))
              << unknown;
      };
      "soul_tool_result_empty_output_envelope"_test = [] {
          // A successful payload with an empty output and only a message renders
          // as just the <system> head - no stray separator, no output body.
          kimix::agent::AgentSession session(tmp_workspace());
          FakeBackend backend;
          kimix::agent::KimiSoul soul(session, backend);
          kimix::string err;
          const kimix::string ok =
              soul.execute_tool_call("write", R"JSON({"file_path":"e.txt","content":""})JSON", err);
          expect(err.empty()) << err;
          expect(ok.find("<system>") == 0) << ok;
          expect(ok.find("</system>") != kimix::string::npos) << ok;
          expect(ok.find("\n") == kimix::string::npos ||
                 ok.find("\n") > ok.find("</system>"))
              << ok;
      };
      // =======================================================================
      // F4: sanitize_for_tokenizer on every tool output in dispatch
      // =======================================================================
      "soul_dispatch_sanitizes_tool_output"_test = [] {
          const kimix::string ws = tmp_workspace();
          // A zero-width space (U+200B) and a NUL ride into the tool output;
          // the dispatch path must sanitize them away (F4).
          const kimix::filesystem::path path =
              kimix::filesystem::path(ws) / "dirty.txt";
          {
              std::FILE *f = std::fopen(kimix::to_string(path).c_str(), "wb");
              expect(f != nullptr);
              const char bytes[] = {'a', 'b', char(0xE2), char(0x80), char(0x8B),
                                    'c', 'd', char(0x00), 'e'};
              std::fwrite(bytes, 1, sizeof(bytes), f);
              std::fclose(f);
          }
          kimix::agent::AgentSession session(ws);
          FakeBackend backend;
          kimix::agent::KimiSoul soul(session, backend);
          kimix::string err;
          const kimix::string out = soul.execute_tool_call(
              "read", R"JSON({"file_path":"dirty.txt"})JSON", err);
          expect(out.find(std::string("\xe2\x80\x8b")) == kimix::string::npos) << out;
          expect(out.find(char(0x00)) == kimix::string::npos) << out;
          expect(out.find("ab") != kimix::string::npos) << out;
          expect(out.find("cd") != kimix::string::npos) << out;
      };
      // =======================================================================
      // F3: dynamic per-tool output budget
      // =======================================================================
      "soul_dispatch_dynamic_output_budget"_test = [] {
          const kimix::string ws = tmp_workspace();
          // ~24 KiB of varied lines (sanitize's max_repeat=100 dedupe must not
          // shrink it): 200 lines of 120 distinct characters each.
          const kimix::filesystem::path big_path =
              kimix::filesystem::path(ws) / "big.txt";
          {
              std::FILE *f = std::fopen(kimix::to_string(big_path).c_str(), "wb");
              expect(f != nullptr);
              for (int i = 0; i < 200; ++i) {
                  kimix::string line = kimix::format("line {:04d} ", i);
                  for (int j = 0; j < 8; ++j) {
                      line += kimix::format("{:02x}", (i * 31 + j * 7) & 0xFF);
                  }
                  line += " abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOP\n";
                  std::fwrite(line.data(), 1, line.size(), f);
              }
              std::fclose(f);
          }
          class WindowBackend : public kimix::agent::IChatBackend {
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
          int64_t max_context_size() const override { return 4000; }
          kimix::string model_name() const override { return "window"; }
      };
      kimix::agent::AgentSession session(ws);
      WindowBackend backend;
          kimix::agent::KimiSoul::options opts;
          opts.enabled_tools = {"read"};
          opts.system_prompt = "x";
          kimix::agent::KimiSoul soul(session, backend, opts);
          // Expected budget, mirroring toolset.py _estimate_tool_output_byte_budget
          // with the live context: min(ctx*4*0.5, remaining*4*0.9, 128 KiB).
          const int64_t window = backend.max_context_size();
          const int64_t est = soul.estimated_tokens();
          const int64_t remaining = window - est > 0 ? window - est : 0;
          int64_t expected = static_cast<int64_t>(window * 4 * 0.5);
          const int64_t by_remaining =
              static_cast<int64_t>(remaining * 4 * 0.9);
          if (by_remaining < expected) {
              expected = by_remaining;
          }
          if ((128 << 10) < expected) {
              expected = 128 << 10;
          }
          expect(expected > 0) << expected;
          // A missing file passes through untouched by the size guard (error
          // envelope, but not a size error).
          kimix::string err;
          const kimix::string missing_out = soul.execute_tool_call(
              "read", R"JSON({"file_path":"small.txt"})JSON", err);
          expect(missing_out.find("exceeded the maximum allowed size") ==
                 kimix::string::npos)
              << missing_out;
          // The big read overflows the dynamic budget: ToolError envelope with
          // the reference wording and the budget-truncated output.
          const kimix::string out =
              soul.execute_tool_call("read", R"JSON({"file_path":"big.txt"})JSON", err);
          expect(out.find("<system>ERROR: Tool output exceeded the maximum "
                          "allowed size (") == 0)
              << out;
          expect(out.find("bytes; limit ") != kimix::string::npos) << out;
          expect(out.find(kimix::format("limit {} bytes)", expected)) !=
                 kimix::string::npos)
              << out;
          expect(out.find("The result has been truncated.") != kimix::string::npos)
              << out;
          // The truncated output part is at most the budget in bytes and the
          // history never holds a structurally corrupted payload.
          const size_t after_system = out.find("</system>\n");
          expect(after_system != kimix::string::npos);
          expect(out.size() - (after_system + std::string("</system>\n").size()) <=
                 static_cast<size_t>(expected))
              << out.size();
      };
      // =======================================================================
      // C5: the full verbatim compact.md template reaches the summarizer
      // =======================================================================
      "soul_compaction_prompt_full_template"_test = [] {
          kimix::agent::AgentSession session(tmp_workspace());
          FakeBackend backend;
          kimix::llm::ChatResult summary;
          summary.ok = true;
          summary.content = "<current_focus>compacted</current_focus>";
          backend.scripted = {summary};
          kimix::agent::KimiSoul soul(session, backend);
          auto &h = session.history();
          for (int i = 0; i < 8; ++i) {
              kimix::llm::Message u;
              u.role = "user";
              u.content = kimix::format("question {} with some padding text to "
                                        "make the token estimate grow", i);
              h.push_back(u);
              kimix::llm::Message a;
              a.role = "assistant";
              a.content = kimix::format("answer {} plus a longer explanation "
                                        "that should be compacted away", i);
              h.push_back(a);
          }
          kimix::string err;
          expect(soul.compact_context("", err, /*manual=*/true)) << err;
          expect(eq(backend.requests.size(), static_cast<size_t>(1)));
          // C13: the aligned transport replays the region verbatim and sends
          // the compaction instruction as the FINAL message - the template
          // assertions below read that instruction message.
          expect(backend.requests[0].size() >= 3u);
          const kimix::string &body = backend.requests[0].back().content;
          // 14 keep-priorities incl. the previously missing tail.
          expect(body.find("1. **Current Task State**") != kimix::string::npos);
          expect(body.find("14. **Technical Notes**") != kimix::string::npos);
          // "Key logic means" definition + Special Handling.
          expect(body.find("**Key logic** means:") != kimix::string::npos);
          expect(body.find("**Special Handling:**") != kimix::string::npos);
          // Length guidance incl. the aggressive/retentive clause.
          expect(body.find("20-30%") != kimix::string::npos);
          expect(body.find("Err on the side of brevity for aggressive mode and "
                           "completeness for retentive mode.") != kimix::string::npos);
          // All 16 XML blocks (the hand-condensed port had 9).
          for (const char *block :
               {"<current_focus>", "<environment>", "<completed_tasks>",
                "<active_issues>", "<todo>", "<code_state>", "<decisions>",
                "<key_decisions>", "<project_overview>", "<current_state>",
                "<important_files>", "<architecture>", "<dependencies>",
                "<risks_rollback>", "<technical_notes>", "<important_context>"}) {
              expect(body.find(block) != kimix::string::npos) << block;
          }
          // code_state keeps its typed file shape.
          expect(body.find("<file name=\"path/to/file.py\">") != kimix::string::npos);
          // The instruction is sent exactly once (build_compact_message_text
          // already ends with prompt_text).
          const size_t first = body.find("Compact the above agent conversation context");
          expect(first != kimix::string::npos);
          expect(body.find("Compact the above agent conversation context",
                           first + 1) == kimix::string::npos);
          // Default balanced mode guidance is appended after the template.
          expect(body.find("**Compaction Style Guidance:** Be balanced.") !=
                 kimix::string::npos);
          // No cascade below depth 3.
          expect(body.find("flat, deduplicated") == kimix::string::npos);
      };
      // =======================================================================
      // C6: COMPACT_CASCADE at >= 3 in-band summaries
      // =======================================================================
      "soul_compaction_cascade_prompt_at_depth_three"_test = [] {
          kimix::agent::AgentSession session(tmp_workspace());
          FakeBackend backend;
          kimix::llm::ChatResult summary;
          summary.ok = true;
          summary.content = "<current_focus>recursively compacted</current_focus>";
          backend.scripted = {summary};
          kimix::agent::KimiSoul soul(session, backend);
          auto &h = session.history();
          // Three in-band summaries (the reference's cascade marker) inside
          // the compacted region, then a fresh exchange as the preserved tail.
          for (int i = 0; i < 3; ++i) {
              kimix::llm::Message s;
              s.role = "user";
              s.content = "<system>Previous context has been compacted. Here is "
                          "the compaction output:</system>older summary ";
              s.content += std::to_string(i);
              s.content += " with enough padding text to keep the region well "
                           "above the preserved tail in size";
              h.push_back(s);
          }
          for (int i = 0; i < 4; ++i) {
              kimix::llm::Message u;
              u.role = "user";
              u.content = kimix::format("fresh question {} with padding text "
                                        "to make the region large enough", i);
              h.push_back(u);
              kimix::llm::Message a;
              a.role = "assistant";
              a.content = kimix::format("fresh answer {} with padding text "
                                        "that keeps the tail small", i);
              h.push_back(a);
          }
          kimix::string err;
          expect(soul.compact_context("", err, /*manual=*/true)) << err;
          // C13: the aligned transport replays the region verbatim and sends
          // the compaction instruction as the FINAL message - the template
          // assertions below read that instruction message.
          expect(backend.requests[0].size() >= 3u);
          const kimix::string &body = backend.requests[0].back().content;
          // The cascade prompt: flat deduplicated facts, the 8 rules, the
          // typed <facts> bullets and the 14-block XML.
          expect(body.find("flat, deduplicated list of key facts") !=
                 kimix::string::npos)
              << body;
          expect(body.find("**De-duplicate:**") != kimix::string::npos);
          expect(body.find("<facts>") != kimix::string::npos);
          expect(body.find("- [Decision] [Decision description and rationale]") !=
                 kimix::string::npos);
          expect(body.find("- [Error] [Error message and resolution]") !=
                 kimix::string::npos);
          // Cascade XML has <facts> and code_state but no <todo>/<decisions>.
          expect(body.find("<code_state>") != kimix::string::npos);
          expect(body.find("<risks_rollback>") != kimix::string::npos);
      };
      // =======================================================================
      // C7: todo re-injection into the compaction summary
      // =======================================================================
      "soul_compaction_reinjects_active_todos"_test = [] {
          namespace todo = kimix::builtin_tools::todo;
          kimix::agent::AgentSession session(tmp_workspace());
          FakeBackend backend;
          kimix::llm::ChatResult summary;
          summary.ok = true;
          summary.content = "SUMMARY";
          backend.scripted = {summary};
          kimix::agent::KimiSoul soul(session, backend);
          // Seed the session todo tree: a pending root, an in_progress root, a
          // done root (excluded) and a done parent whose pending child must
          // still appear (indented one level).
          session.tool_session().todo_state =
              kimix::shared_ptr<todo::todo_state>(new todo::todo_state());
          todo::todo_state &ts = *session.tool_session().todo_state;
          todo::todo_item pending;
          pending.content = "finish the report";
          pending.status = todo::todo_status::pending;
          ts.todos.push_back(pending);
          todo::todo_item in_progress;
          in_progress.content = "fix the flaky test";
          in_progress.status = todo::todo_status::in_progress;
          ts.todos.push_back(in_progress);
          todo::todo_item done_item;
          done_item.content = "already shipped";
          done_item.status = todo::todo_status::done;
          ts.todos.push_back(done_item);
          todo::todo_item done_parent;
          done_parent.content = "phase one";
          done_parent.status = todo::todo_status::done;
          todo::todo_item child;
          child.content = "phase two follow-up";
          child.status = todo::todo_status::pending;
          done_parent.children.push_back(child);
          ts.todos.push_back(done_parent);
          // A long enough history to compact.
          auto &h = session.history();
          for (int i = 0; i < 8; ++i) {
              kimix::llm::Message u;
              u.role = "user";
              u.content = kimix::format("question {} with padding text to make "
                                        "the token estimate grow", i);
              h.push_back(u);
              kimix::llm::Message a;
              a.role = "assistant";
              a.content = kimix::format("answer {} with padding text", i);
              h.push_back(a);
          }
          kimix::string err;
          expect(soul.compact_context("", err, /*manual=*/true)) << err;
          const kimix::string &summary_msg = session.history().front().content;
          expect(summary_msg.find("[Your active task list was preserved across "
                                  "context compression]") != kimix::string::npos)
              << summary_msg;
          expect(summary_msg.find("- [ ] finish the report (pending)") !=
                 kimix::string::npos);
          expect(summary_msg.find("- [>] fix the flaky test (in_progress)") !=
                 kimix::string::npos);
          // done items are excluded; a done parent's unfinished child is not.
          expect(summary_msg.find("already shipped") == kimix::string::npos);
          expect(summary_msg.find("  - [ ] phase two follow-up (pending)") !=
                 kimix::string::npos);
          // The knob disables the injection (default ON).
          kimix::agent::AgentSession session2(tmp_workspace());
          FakeBackend backend2;
          backend2.scripted = {summary};
          kimix::agent::KimiSoul::options opts2;
          opts2.loop_control.todo_compact_injection_enabled = false;
          kimix::agent::KimiSoul soul2(session2, backend2, opts2);
          auto &h2 = session2.history();
          for (int i = 0; i < 8; ++i) {
              kimix::llm::Message u;
              u.role = "user";
              u.content = kimix::format("question {} with padding text to make "
                                        "the token estimate grow", i);
              h2.push_back(u);
              kimix::llm::Message a;
              a.role = "assistant";
              a.content = kimix::format("answer {} with padding text", i);
              h2.push_back(a);
          }
          kimix::string err2;
          expect(soul2.compact_context("", err2, /*manual=*/true)) << err2;
          expect(session2.history().front().content.find(
                     "preserved across context compression") == kimix::string::npos);
      };
      // =======================================================================
      // C8: pre-compaction export + system prompt line
      // =======================================================================
      "soul_compaction_exports_precompaction_context"_test = [] {
          const kimix::string ws = tmp_workspace();
          kimix::agent::AgentSession session(ws);
          FakeBackend backend;
          kimix::llm::ChatResult summary;
          summary.ok = true;
          summary.content = "SUMMARY";
          backend.scripted = {summary};
          kimix::agent::KimiSoul soul(session, backend);
          auto &h = session.history();
          for (int i = 0; i < 8; ++i) {
              kimix::llm::Message u;
              u.role = "user";
              u.content = kimix::format("question {} with padding text to make "
                                        "the token estimate grow", i);
              h.push_back(u);
              kimix::llm::Message a;
              a.role = "assistant";
              a.content = kimix::format("answer {} with padding text", i);
              h.push_back(a);
          }
          const kimix::filesystem::path export_path =
              kimix::filesystem::path(ws) / ".kimix_cache" / "context_compacted.md";
          std::error_code ec;
          expect(!kimix::filesystem::exists(export_path, ec));
          kimix::string cerr;
          expect(soul.compact_context("", cerr, /*manual=*/true)) << cerr;
          // The whole pre-compaction history was exported before the replace.
          expect(kimix::filesystem::exists(export_path, ec)) << cerr;
          {
              std::FILE *f = std::fopen(kimix::to_string(export_path).c_str(), "rb");
              expect(f != nullptr);
              kimix::string text;
              char buf[65536];
              size_t n = 0;
              while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
                  text.append(buf, n);
              }
              std::fclose(f);
              expect(text.find("# Kimi Session Export") != kimix::string::npos) << text;
              expect(text.find("question 0 with") != kimix::string::npos) << text;
              expect(text.find("question 7 with") != kimix::string::npos) << text;
          }
          // The next request's system prompt advertises the export.
          const kimix::agent::TurnResult tr = soul.turn("hello again");
          expect(tr.ok) << tr.error;
          const kimix::vector<kimix::llm::Message> &last = backend.requests.back();
          expect(last[0].role == "system");
          expect(last[0].content.find("Pre-compaction context exported to: ") !=
                 kimix::string::npos)
              << last[0].content;
          expect(last[0].content.find(".kimix_cache") != kimix::string::npos)
              << last[0].content;
      };
      "soul_retrieve_tool_wired_to_history_index"_test = [] {
          // D4: the system prompt advertises retrieve, so the soul session
          // must wire it: a per-session in-memory HistoryIndex fed on message
          // append, injected into the Retrieve tool's HistoryIndexView in
          // get_tool() (before the validity gate, without which the tool
          // silently vanishes from the tool list).
          kimix::agent::AgentSession session(tmp_workspace());
          FakeBackend backend;
          kimix::agent::KimiSoul soul(session, backend);
          bool offered = false;
          for (const kimix::llm::Tool &d : soul.tool_definitions()) {
              if (d.name == "retrieve") {
                  offered = true;
              }
          }
          expect(offered) << "retrieve must be offered (the system prompt "
                             "advertises it)";
          // Feed history through the append hook (history + index).
          kimix::llm::Message u;
          u.role = "user";
          u.content = "the quick brown fox jumps over the lazy dog";
          session.append_history(u);
          kimix::llm::Message a;
          a.role = "assistant";
          a.content = "a later answer about something else entirely";
          session.append_history(a);
          // Query mode resolves the indexed turn through the real BM25 index;
          // the hit text lands in the envelope's output part (E3).
          kimix::string err;
          const kimix::string out = soul.execute_tool_call(
              "retrieve", R"JSON({"query":"quick brown fox","k":1})JSON", err);
          expect(out.find("quick brown fox") != kimix::string::npos) << out;
          expect(out.find("<system>") == 0) << out;
          expect(out.find("\"status\":") == kimix::string::npos) << out;
          // Id lookup resolves the same turn (the first appended message is
          // turn 0; "prune_0" is the reference's accepted reference spelling).
          const kimix::string by_id = soul.execute_tool_call(
              "retrieve", R"JSON({"id":"prune_0"})JSON", err);
          expect(by_id.find("quick brown fox") != kimix::string::npos) << by_id;
          // A blank query is guidance, not an error: the reference's "No
          // query" message becomes the envelope head.
          const kimix::string blank =
              soul.execute_tool_call("retrieve", R"JSON({})JSON", err);
          expect(blank.find("<system>No query</system>") != kimix::string::npos)
              << blank;
          // Direct history() pushes bypass the index by design (the hook is
          // append_history) - document that with a probe: the second message
          // IS indexed because it went through the hook above.
          expect(eq(session.history_index().turn_count(), static_cast<uint32_t>(2)));
      };
      "soul_tool_definitions_from_registry"_test = [] {
          kimix::agent::AgentSession session(tmp_workspace());
          FakeBackend backend;
          kimix::agent::KimiSoul soul(session, backend);
          const auto defs = soul.tool_definitions();
          // The list is the registry filtered by Tool::valid() (no
          // enabled_tools bound here), so every definition must be a tool that
          // can run here and every runnable tool must be a definition.
      auto &reg = kimix::builtin_tools::ToolRegistry::instance();
      size_t runnable = 0;
      for (const auto &meta : reg.all()) {
          auto probe = meta.factory(&session.tool_session());
          if (probe != nullptr && meta.name == "retrieve") {
              // Retrieve::valid() requires an injected HistoryIndexView; the
              // soul wires it in get_tool() before the gate, so the raw probe
              // must do the same to predict what tool_definitions() lists.
              auto *r = static_cast<kimix::builtin_tools::retrieve::Retrieve *>(
                  probe.get());
              r->view.search_with_recency =
                  [](kimix::string_view, int32_t)
                      -> kimix::vector<kimix::builtin_tools::retrieve::history_turn> {
                      return {};
                  };
              r->view.get_by_id =
                  [](kimix::string_view)
                      -> kimix::optional<kimix::builtin_tools::retrieve::history_turn> {
                      return kimix::optional<
                          kimix::builtin_tools::retrieve::history_turn>();
                  };
          }
          const bool valid = (probe != nullptr) && probe->valid();
          bool listed = false;
              for (const auto &d : defs) {
                  if (d.name == meta.name) {
                      listed = true;
                  }
              }
              expect(eq(listed, valid)) << meta.name;
              if (valid) {
                  ++runnable;
              }
          }
          expect(eq(defs.size(), runnable));
          // Enough survives the gate on any dev machine to keep the agent
          // useful (the file tools, compact, todo, the agent tools, ...).
          expect(defs.size() >= 12u) << defs.size();
          // bash is offered exactly when a bash executable exists, and its
          // schema is the registry's own.
          const bool bash_installed =
              !kimix::builtin_tools::bash::Bash::detect_bash_path().empty();
          bool found_bash = false;
          for (const auto &d : defs) {
              if (d.name == "bash") {
                  found_bash = true;
                  expect(d.parameters_json.find("\"cmd\"") != kimix::string::npos);
              }
          }
          expect(eq(found_bash, bash_installed));
      };

    kimix::builtin_tools::proc::stop_all_tasks();
    return 0;
}
