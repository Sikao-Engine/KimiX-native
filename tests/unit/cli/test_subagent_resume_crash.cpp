// test_subagent_resume_crash.cpp - Regression: the real CLI died with
// 0xC0000409 (abort -> fail-fast) when the model RESUMED a settled sub-agent
// session with `subagent(session_id=..., run_in_background=true)` - observed
// in the real CLI as "execv(bin\release\kimix_cli.exe --config=glm_scnet.json)
// failed(-1073740791)".  Root cause: a settled background run's worker
// std::thread stays joinable until the lazy drain_settled_runs() runs, and
// agent_registry::start_background() (the resume path) replaced the run -
// destroying a joinable std::thread calls std::terminate.
//
// The scripted backend routes steps per CALLER THREAD (the first thread to
// chat is the parent soul; every other thread is a background sub-agent
// child), which keeps the parent/child interleaving deterministic.  The
// parent follows the exact real-CLI sequence inside ONE turn: spawn (bg) ->
// send_message -> resume(session_id, bg).  No manual join happens before the
// resume - the buggy code aborted right there.
//
// Framework: Boost.UT (tests/ut/ut.hpp). No network access: scripted fake
// chat backends.

#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "builtin_tools/agent_tool.h"
#include "builtin_tools/tool_registry.h"

#include "agent/soul.h"

#include "cli/cli_app.h"
#include "cli/cli_common.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>

namespace {
namespace cli = kimix::cli;
using namespace boost::ut;

// A fresh, empty workspace: <temp>/<name>, removed and recreated.
kimix::string ws_dir(const char *name) {
    std::error_code ec;
    const kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) / name;
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

bool has_substr(kimix::string_view haystack, kimix::string_view needle) {
    return cli::contains(haystack, needle);
}

// One scripted backend answer: text + reasoning + tool calls.
struct test_step {
    kimix::string content;
    kimix::string reasoning;
    kimix::vector<kimix::llm::ToolCall> calls;
};

// The scripted IChatBackend with PER-THREAD step queues plus an optional
// pre-chat sleep per parent chat.  The first thread that calls chat() owns
// `parent_steps` (the root soul); every other thread pops from `child_steps`
// (background sub-agent souls).  The sleep gives a background child time to
// SETTLE (finished, worker joined only by the lazy drain) between two parent
// steps, reproducing the real timing of the crash sequence.
class test_backend : public kimix::agent::IChatBackend {
public:
    kimix::vector<test_step> parent_steps;
    kimix::vector<test_step> child_steps;
    kimix::vector<int> parent_sleep_ms; // optional, one entry per parent chat
    std::atomic<int32_t> calls{0};
    int64_t context_size = 100000;
    std::mutex log_mutex;
    kimix::vector<kimix::string> chat_log;

private:
    std::mutex route_mutex;
    std::thread::id parent_thread{};
    bool parent_known = false;
    int32_t parent_chats = 0;

    kimix::vector<test_step> &queue_for(std::thread::id tid, bool &is_parent) {
        std::lock_guard<std::mutex> g(route_mutex);
        if (!parent_known) {
            parent_known = true;
            parent_thread = tid;
        }
        is_parent = (tid == parent_thread);
        return is_parent ? parent_steps : child_steps;
    }

public:
    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &,
         const kimix::vector<kimix::llm::Tool> &,
         const kimix::llm::ChunkCallback &on_chunk,
         const kimix::llm::AbortCheck *) override {
        kimix::llm::ChatResult result;
        result.ok = true;
        const int32_t n = calls.fetch_add(1);
        bool is_parent = false;
        kimix::vector<test_step> &steps = queue_for(std::this_thread::get_id(),
                                                    is_parent);
        if (is_parent) {
            int32_t idx;
            {
                std::lock_guard<std::mutex> g(route_mutex);
                idx = parent_chats++;
            }
            if (static_cast<size_t>(idx) < parent_sleep_ms.size() &&
                parent_sleep_ms[static_cast<size_t>(idx)] > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    parent_sleep_ms[static_cast<size_t>(idx)]));
            }
        }
        const bool scripted = !steps.empty();
        if (scripted) {
            const test_step &step = steps.front();
            result.content = step.content;
            result.reasoning = step.reasoning;
            result.tool_calls = step.calls;
            steps.erase(steps.begin());
        }
        {
            std::lock_guard<std::mutex> g(log_mutex);
            std::ostringstream os;
            os << "chat#" << n << (is_parent ? " PARENT" : " CHILD")
               << (scripted ? "" : " (unscripted)");
            if (!result.content.empty()) {
                os << " content='" << result.content << "'";
            }
            os << " tool_calls=" << result.tool_calls.size();
            chat_log.push_back(kimix::string(os.str()));
        }
        if (on_chunk) {
            if (!result.reasoning.empty()) {
                kimix::llm::Chunk chunk;
                chunk.ok = true;
                chunk.reasoning = result.reasoning;
                on_chunk(chunk);
            }
            if (!result.content.empty()) {
                kimix::llm::Chunk chunk;
                chunk.ok = true;
                chunk.content = result.content;
                on_chunk(chunk);
            }
            for (const kimix::llm::ToolCall &call : result.tool_calls) {
                kimix::llm::Chunk chunk;
                chunk.ok = true;
                chunk.tool_calls.push_back(call);
                on_chunk(chunk);
            }
        }
        return result;
    }

    int64_t max_context_size() const override { return context_size; }
    kimix::string model_name() const override { return "scripted-test-model"; }
};

// A configured app_context with a scripted backend (no network access) and
// the PRODUCTION sub-agent runner (app_rebind_session installs it).
struct app_fixture {
    kimix::string work;
    kimix::string provider;
    cli::cli_options opts;
    cli::app_context app;
    test_backend backend;
    kimix::string error;

    bool init(const char *name) {
        work = ws_dir(name);
        provider = cli::join_path(work, "provider.json");
        kimix::string write_error;
        const kimix::string json =
            "{\"model\":\"scripted-test-model\",\"type\":\"openai\","
            "\"url\":\"http://127.0.0.1:1/v1\",\"api_key\":\"test\","
            "\"max_context_size\":100000,\"max_tokens\":100,"
            "\"loop_control\":{"
            "\"budget_reminder_enabled\":false,"
            "\"context_meter_enabled\":false,"
            "\"todo_reminder_enabled\":false,"
            "\"target_churn_enabled\":false,"
            "\"compact_reminder_enabled\":false,"
            "\"auto_retrieve_history\":false,"
            "\"auto_retrieve_working_memory\":false,"
            "\"auto_retrieve_recency_memory\":false}}";
        if (!cli::write_file(provider, json, write_error)) {
            error = write_error;
            return false;
        }
        opts.config_path = provider;
        opts.config_is_provider_only = true;
        opts.work_dir = work;
        opts.no_color = true;
        return cli::app_init(opts, app, error, &backend);
    }

    void shutdown() {
        app.soul.reset();
        app.session.reset();
        kimix::string close_error;
        app.store.close(true, close_error);
    }
};

// Read the tool-role messages of the history (for diagnostics).
kimix::vector<kimix::string> tool_messages(const cli::app_context &app) {
    kimix::vector<kimix::string> out;
    for (const kimix::llm::Message &m : app.session->history()) {
        if (m.role == kimix::string("tool")) {
            out.push_back(m.content);
        }
    }
    return out;
}

kimix::llm::ToolCall make_call(const char *id, const char *name,
                               const kimix::string &arguments) {
    kimix::llm::ToolCall call;
    call.id = id;
    call.type = "function";
    call.name = name;
    call.arguments = arguments;
    return call;
}

} // namespace

int main() {
    "subagent_resume_of_settled_session_no_abort"_test = [] {
        app_fixture fx;
        expect(fx.init("cli_subagent_resume_crash")) << "app_init: " << fx.error;
        kimix::builtin_tools::agents::agent_registry &reg =
            kimix::builtin_tools::agents::session_registry(
                &fx.app.session->tool_session());

        // The model's single-turn plan, exactly the real-CLI sequence:
        //   step 1: spawn a background sub-agent
        //   step 2: send_message to it (running or settled - both legal)
        //   step 3: RESUME its session (session_id, run_in_background=true)
        //   step 4: final text
        fx.backend.parent_steps.push_back(
            {"", "", {make_call("call_spawn", "subagent",
                                "{\"prompt\":\"reply PONG\","
                                "\"run_in_background\":true}")}});
        fx.backend.parent_steps.push_back(
            {"", "", {make_call("call_send", "send_message",
                                "{\"message\":\"ping two\"}")}});
        fx.backend.parent_steps.push_back(
            {"", "", {make_call("call_resume", "subagent",
                                "{\"prompt\":\"reply PONG2\","
                                "\"session_id\":\"RESOLVED_AT_RUNTIME\","
                                "\"run_in_background\":true}")}});
        fx.backend.parent_steps.push_back({"E2E_DONE", "", {}});
        // Parent chat #1 (the send_message step) sleeps 400ms first, so the
        // child's first run has settled by then (finished, worker never
        // joined - drain_settled_runs is lazy): the message is queued for the
        // resume and the resume's start_background hits the settled-but-
        // unjoined run exactly like the real CLI.  Chat #2 (the resume step)
        // sleeps too, belt and braces.
        fx.backend.parent_sleep_ms.push_back(0);
        fx.backend.parent_sleep_ms.push_back(400);
        fx.backend.parent_sleep_ms.push_back(400);
        fx.backend.parent_sleep_ms.push_back(0);
        // The child's two turns.
        fx.backend.child_steps.push_back({"PONG", "", {}});
        fx.backend.child_steps.push_back({"PONG2", "", {}});

        // The resume call must carry the REAL session id, which only exists
        // after the spawn ran.  Rewrite the scripted step in place right
        // before the turn: the parent's chat #1 (spawn) is consumed first, so
        // patch after a tiny settle poll for the registered id.
        // Simpler: poll the registry for the child id from a helper thread is
        // overkill - instead patch the step the moment list_active() knows
        // it, from the parent's perspective the spawn result carries it.  We
        // therefore pre-register a DETERMINISTIC id: spawn with a fixed
        // session_id argument so the script can reference it up front.
        const kimix::string child_id = "11111111-2222-3333-4444-555555555555";
        fx.backend.parent_steps[0] =
            {"", "", {make_call("call_spawn", "subagent",
                                "{\"prompt\":\"reply PONG\","
                                "\"session_id\":\"" + child_id + "\","
                                "\"run_in_background\":true}")}};
        fx.backend.parent_steps[2] =
            {"", "", {make_call("call_resume", "subagent",
                                "{\"prompt\":\"reply PONG2\","
                                "\"session_id\":\"" + child_id + "\","
                                "\"run_in_background\":true}")}};

        kimix::string error1;
        const bool ok1 = cli::app_run_prompt(
            fx.app, "run the scripted e2e sequence", &error1);
        expect(ok1) << "turn error: " << error1;

        // The resumed run settles and its output reaches the result.  With
        // close_session=true (the default) the session is closed after the
        // run settles and the result parks in _finished.
        bool resumed_settled = false;
        kimix::builtin_tools::agents::subagent_run_result second;
        for (int i = 0; i < 400 && !resumed_settled; ++i) {
            if (reg.run_finished(child_id)) {
                resumed_settled = reg.join_run(child_id, second);
                reg.clear_run(child_id);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (!resumed_settled) {
            for (const kimix::string &line : tool_messages(fx.app)) {
                expect(false) << "tool result: " << line;
            }
            std::lock_guard<std::mutex> g(fx.backend.log_mutex);
            for (const kimix::string &line : fx.backend.chat_log) {
                expect(false) << "chat log: " << line;
            }
        }
        expect(resumed_settled) << "the resumed background run settled";
        expect(second.ok) << "resumed run error: " << second.error;
        expect(has_substr(second.output, "PONG2"))
            << "the resumed child ran its turn";
        fx.shutdown();
    };
    return 0;
}
