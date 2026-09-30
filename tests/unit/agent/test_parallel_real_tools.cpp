// test_parallel_real_tools.cpp - crash repro for the real-CLI failure where a
// step dispatching bash + todo_list + glob in parallel killed the process
// (session 14c5dc5af05a0d5dcb68bd22f23bfe9f: the wire log ends right after the
// model's tool-call step; no tool result was ever recorded). This suite drives
// the production bounded parallel dispatch (A9, dispatch_concurrency = 3) with
// the three real builtin tools - a fresh instance per call, one shared
// builtin_tools::Session - over many scripted turns, asserting every turn
// completes and every tool result is well-formed.
//
// Framework: Boost.UT (tests/ut/ut.hpp). Environment-dependent: needs a real
// Git Bash for the bash call; when bash is missing the suite prints a skip
// note and passes without exercising the tools (the test-process-runner
// precedent for probing the environment).

#include "ut/ut.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <core/kimix_core.h>

#include "agent/soul.h"
#include "builtin_tools/bash_tool.h"
#include "builtin_tools/tool_registry.h"

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::agent;

namespace {

class ScriptedBackend : public IChatBackend {
public:
    kimix::vector<kimix::llm::ChatResult> scripted;
    size_t index = 0;

    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &,
         const kimix::vector<kimix::llm::Tool> &,
         const kimix::llm::ChunkCallback &,
         const kimix::llm::AbortCheck * /*abort*/) override {
        if (index < scripted.size()) {
            return scripted[index++];
        }
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content = "done";
        return r;
    }
    int64_t max_context_size() const override { return 128000; }
    kimix::string model_name() const override { return "scripted"; }

    static kimix::llm::ChatResult ok_text(kimix::string_view text) {
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content.assign(text.data(), text.size());
        return r;
    }
    // One assistant message carrying the three real-tool calls of the crashed
    // session: bash pwd, todo_list read, glob '*'.
    static kimix::llm::ChatResult crashed_session_step() {
        kimix::llm::ChatResult r;
        r.ok = true;
        const char *ids[3] = {"call_bash", "call_todo", "call_glob"};
        const char *names[3] = {"bash", "todo_list", "glob"};
        const char *args[3] = {
            R"JSON({"command":"pwd && echo ---"})JSON",
            R"JSON({})JSON",
            R"JSON({"pattern":"*"})JSON",
        };
        for (int i = 0; i < 3; ++i) {
            kimix::llm::ToolCall call;
            call.id.assign(ids[i]);
            call.name.assign(names[i]);
            call.arguments = args[i];
            r.tool_calls.push_back(std::move(call));
        }
        return r;
    }
};

kimix::string tmp_workspace(const char *name) {
    std::error_code ec;
    kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) / name;
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    // A couple of files so glob('*') has something to list.
    for (const char *f : {"alpha.txt", "beta.md"}) {
        std::FILE *h = std::fopen((base / f).string().c_str(), "wb");
        if (h != nullptr) {
            std::fputs("x", h);
            std::fclose(h);
        }
    }
    return kimix::to_string(base);
}

KimiSoul::options base_opts(int32_t concurrency) {
    KimiSoul::options opts;
    opts.loop_control.verification_gate_enabled = false;
    opts.loop_control.dispatch_concurrency = concurrency;
    opts.auto_compact = false;
    opts.system_prompt = "test";
    return opts;
}

} // namespace

int main() {
    // Environment probe: the whole point is the REAL bash spawn; without Git
    // Bash there is nothing to reproduce. Skip loudly but pass.
    {
        kimix::builtin_tools::Session probe_session;
        probe_session.native_io = true;
        kimix::unique_ptr<kimix::builtin_tools::Tool> probe =
            kimix::builtin_tools::ToolRegistry::instance().create("bash",
                                                                  &probe_session);
        if (probe == nullptr || !probe->valid()) {
            std::fprintf(stderr,
                         "SKIP: no usable bash in this environment - the "
                         "parallel bash/todo_list/glob repro needs a real "
                         "spawn.\n");
            return 0;
        }
    }

    "parallel_real_tools_bash_todo_glob_stress"_test = [] {
        // KIMIX_A9_WS overrides the workspace so the suite can be pointed at
        // the exact directory a crashed session was scanning (glob('*') over
        // the real home directory, not a sanitized temp tree).
        kimix::string ws = tmp_workspace("kimix_a9_real_tools_ws");
        if (const char *env = std::getenv("KIMIX_A9_WS")) {
            if (*env != '\0') {
                ws = kimix::string(env);
            }
        }
        constexpr int k_turns = 25;
        ScriptedBackend backend;
        for (int i = 0; i < k_turns; ++i) {
            backend.scripted.push_back(ScriptedBackend::crashed_session_step());
            backend.scripted.push_back(ScriptedBackend::ok_text("finished"));
        }
        AgentSession session(ws);
        KimiSoul soul(session, backend, base_opts(/*concurrency=*/3));
        for (int i = 0; i < k_turns; ++i) {
            const TurnResult r = soul.turn("round");
            expect(r.ok) << "turn " << i << " completed";
            // Three tool result messages per turn, each well-formed (a status
            // field - ok/error - and never empty content).
            size_t tool_msgs = 0;
            for (const kimix::llm::Message &m : session.history()) {
                if (m.role != kimix::string("tool")) {
                    continue;
                }
                ++tool_msgs;
                expect(!m.content.empty()) << "tool result has content";
                expect(m.content.find("<system>") != kimix::string::npos)
                    << "tool result carries the envelope";
            }
            expect(tool_msgs == static_cast<size_t>((i + 1) * 3))
                << "three tool results per turn";
        }
    };

    return 0;
}
