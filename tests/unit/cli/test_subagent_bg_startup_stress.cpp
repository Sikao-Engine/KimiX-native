// test_subagent_bg_startup_stress.cpp - Stress repro for the real-CLI failure
// where the process died with 0xC0000409 ("execv(bin\debug\kimix_cli.exe ...)
// failed(-1073740791)") right after the model - asked to "try run these
// tools: subagent / send_message / list_agents" - emitted exactly those three
// calls in one step.  The crashed session's artifacts show the sub-agent
// scratch wire.jsonl holding ONLY the metadata record: the child was created
// (scratch dir + wire open) but its first LLMRequest was never logged, so the
// process died while the background child was starting its turn, overlapped
// with the parent turn's continuation.
//
// This suite drives that exact overlap through the production app fixture +
// install_subagent_runner many times: the parent's second chat sleeps in the
// backend (a real provider request takes seconds, the mock/scriped harness
// takes microseconds) so the child's whole startup (soul ctor, tool set
// build, system prompt) runs concurrently with the parent turn.  A reader
// thread mimics the REPL reader routing typed lines as mid-turn steers.
//
// Framework: Boost.UT (tests/ut/ut.hpp). No network access: scripted fake
// chat backends with PER-THREAD step queues (first chatting thread = parent).

#include "ut/ut.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>

#include <core/kimix_core.h>

#include "agent/soul.h"
#include "agent/steer.h"
#include "builtin_tools/agent_tool.h"
#include "builtin_tools/tool_registry.h"

#include "cli/cli_app.h"
#include "cli/cli_common.h"

namespace {
namespace cli = kimix::cli;
using namespace boost::ut;

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

struct test_step {
    kimix::string content;
    kimix::string reasoning;
    kimix::vector<kimix::llm::ToolCall> calls;
};

// Scripted IChatBackend with PER-THREAD step queues plus per-thread sleeps:
// the first thread to chat is the parent soul, every other thread is a
// background sub-agent child.  Sleeping inside chat() reproduces the wall
// clock of a real provider request, widening the parent/child overlap.
class test_backend : public kimix::agent::IChatBackend {
public:
    kimix::vector<test_step> parent_steps;
    kimix::vector<test_step> child_steps;
    kimix::vector<int> parent_sleep_ms; // one entry per parent chat
    kimix::vector<int> child_sleep_ms;  // one entry per child chat
    std::atomic<int32_t> calls{0};
    int64_t context_size = 100000;
    std::mutex log_mutex;
    kimix::vector<kimix::string> chat_log;

private:
    std::mutex route_mutex;
    std::thread::id parent_thread{};
    bool parent_known = false;
    int32_t parent_chats = 0;
    int32_t child_chats = 0;

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
        int sleep_ms = 0;
        {
            std::lock_guard<std::mutex> g(route_mutex);
            if (is_parent) {
                if (static_cast<size_t>(parent_chats) < parent_sleep_ms.size()) {
                    sleep_ms = parent_sleep_ms[static_cast<size_t>(parent_chats)];
                }
                ++parent_chats;
            } else {
                if (static_cast<size_t>(child_chats) < child_sleep_ms.size()) {
                    sleep_ms = child_sleep_ms[static_cast<size_t>(child_chats)];
                }
                ++child_chats;
            }
        }
        if (sleep_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
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
               << (scripted ? "" : " (unscripted)")
               << " tool_calls=" << result.tool_calls.size();
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
    // The crashed session's exact shape, stressed: the model answers the
    // "try run these tools" prompt with the three agent calls in ONE step;
    // the parent's next chat is slow (real provider latency), so the
    // background child's whole turn startup overlaps the parent turn.  A
    // reader thread keeps resolving Steer::from_session and pushing steers
    // like the REPL reader routing typed lines mid-turn.
    "subagent_bg_startup_overlap_stress"_test = [] {
        const int iters = 30;
        const kimix::string child_id =
            "11111111-2222-3333-4444-555555555555";
        for (int iter = 0; iter < iters; ++iter) {
            app_fixture fx;
            expect(fx.init("cli_subagent_bg_stress")) << "app_init: " << fx.error;
            fx.backend.context_size = 100000;

            // Step 1: the three tool calls from the crashed record.
            fx.backend.parent_steps.push_back(
                {"", "", {make_call("call_spawn", "subagent",
                                    "{\"description\":\"probe subagent\","
                                    "\"prompt\":\"Reply PONG and stop.\","
                                    "\"session_id\":\"" + child_id + "\","
                                    "\"run_in_background\":true}"),
                        make_call("call_send", "send_message",
                                  "{\"subagent_id\":\"" + child_id +
                                      "\",\"message\":\"ping\"}"),
                        make_call("call_list", "list_agents", "{}")}});
            // Step 2: the slow parent continuation (backend sleeps 600ms).
            fx.backend.parent_steps.push_back({"PARENT_DONE", "", {}});
            fx.backend.parent_sleep_ms.push_back(0);
            fx.backend.parent_sleep_ms.push_back(600);
            // Child turn: quick single answer.
            fx.backend.child_steps.push_back({"PONG", "", {}});
            fx.backend.child_sleep_ms.push_back(100);

            // Reader thread: resolve the parent soul and push steers for the
            // whole duration of the turn (the REPL reader's mid-turn routing).
            std::atomic<bool> stop{false};
            std::atomic<int32_t> steers_pushed{0};
            std::thread reader([&]() {
                while (!stop.load(std::memory_order_relaxed)) {
                    kimix::optional<kimix::agent::Steer> steer =
                        kimix::agent::Steer::from_session(*fx.app.session);
                    if (steer.has_value()) {
                        if (steer->push("reader steer while the child starts")) {
                            steers_pushed.fetch_add(1);
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            });

            kimix::string turn_error;
            const bool ok = cli::app_run_prompt(
                fx.app, "run the scripted e2e sequence", &turn_error);
            expect(ok) << "iter " << iter << " turn error: " << turn_error;

            // Let the background child settle, then join it like job_output.
            kimix::builtin_tools::agents::agent_registry &reg =
                kimix::builtin_tools::agents::session_registry(
                    &fx.app.session->tool_session());
            bool settled = false;
            for (int i = 0; i < 400 && !settled; ++i) {
                if (reg.run_finished(child_id)) {
                    kimix::builtin_tools::agents::subagent_run_result out;
                    expect(reg.join_run(child_id, out))
                        << "iter " << iter << " join the settled run";
                    reg.clear_run(child_id);
                    settled = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            expect(settled) << "iter " << iter << " child settled";

            stop.store(true, std::memory_order_relaxed);
            reader.join();
            fx.shutdown();
        }
    };
    return 0;
}
