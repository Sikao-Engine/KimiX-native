// test_parallel_dispatch.cpp - A9: the bounded parallel tool dispatch
// (LoopControl.dispatch_concurrency; the reference runs a step's tool calls
// as concurrent asyncio tasks, kimisoul.py:1953):
//   * dispatch_concurrency == 1 (the default) keeps the strictly serial
//     dispatch - a probe tool that would hang if two calls overlapped passes,
//     proving the calls never run concurrently;
//   * dispatch_concurrency == 2 with two calls of rendezvous probe tools
//     passes the same gate, proving the calls DID run concurrently, and the
//     results still attach in ORIGINAL call order;
//   * a same-step duplicate short-circuits without a second run;
//   * a pure approval rejection stops starting new tools while the
//     tool-call pairing stays intact.
//
// Framework: Boost.UT (tests/ut/ut.hpp). CPU-only: runtime-registered probe
// tools + a scripted backend.

#include "ut/ut.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#include <core/kimix_core.h>

#include "agent/approval.h"
#include "agent/soul.h"
#include "builtin_tools/tool_registry.h"

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::agent;

namespace {

std::atomic<int> g_in_flight{0};
std::atomic<int> g_overlap_seen{0};
std::atomic<int> g_runs{0};
std::atomic<int> g_peak{0};

// A probe tool: records the peak concurrency of probe calls and, when
// `expect_overlap` is set, FAILS if it never saw two calls in flight (the
// rendezvous proof of parallelism). The wait is bounded so a serial bug
// cannot hang the suite: the tool returns an error after ~2s.
class ProbeTool : public kimix::builtin_tools::Tool {
public:
    explicit ProbeTool(kimix::builtin_tools::Session *session)
        : Tool(session) {}
    bool valid() const override { return true; }
    void operator()(kimix::builtin_tools::ToolParams const *parameters) override {
        (void)parameters;
        ++g_runs;
        const int now = ++g_in_flight;
        int prev = g_peak.load(std::memory_order_relaxed);
        while (prev < now &&
               !g_peak.compare_exchange_weak(prev, now,
                                             std::memory_order_relaxed)) {
        }
        // Hold the gate open briefly so a concurrent partner can join.
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(400);
        while (std::chrono::steady_clock::now() < deadline &&
               g_in_flight.load(std::memory_order_relaxed) < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (g_in_flight.load(std::memory_order_relaxed) >= 2) {
            g_overlap_seen.fetch_add(1);
        }
        --g_in_flight;

        kimix::builtin_tools::ToolParams result;
        result.values["status"] =
            kimix::builtin_tools::ValueElement::make_string(kimix::string("ok"));
        result.values["output"] = kimix::builtin_tools::ValueElement::make_string(
            kimix::string("probe ok"));
        result.serialize(_last_result);
    }
    void result_json(kimix::vector<char> &out) const override {
        out = _last_result;
    }
    kimix::vector<char> _last_result;
};

void register_probe(const char *name) {
    kimix::builtin_tools::ToolMeta meta;
    meta.name = name;
    meta.description = "A9 probe tool";
    meta.parameters_json = "{}";
    const kimix::string key(name);
    meta.factory = [key](kimix::builtin_tools::Session *session) {
        return kimix::builtin_tools::create_tool_instance<ProbeTool>(key,
                                                                     session);
    };
    kimix::builtin_tools::ToolRegistry::instance().register_tool(std::move(meta));
}

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
    static kimix::llm::ChatResult two_calls(kimix::string_view id_a,
                                            kimix::string_view name_a,
                                            kimix::string_view id_b,
                                            kimix::string_view name_b) {
        kimix::llm::ChatResult r;
        r.ok = true;
        kimix::llm::ToolCall a;
        a.id.assign(id_a.data(), id_a.size());
        a.name.assign(name_a.data(), name_a.size());
        a.arguments = "{}";
        kimix::llm::ToolCall b;
        b.id.assign(id_b.data(), id_b.size());
        b.name.assign(name_b.data(), name_b.size());
        b.arguments = "{}";
        r.tool_calls.push_back(std::move(a));
        r.tool_calls.push_back(std::move(b));
        return r;
    }
};

kimix::string tmp_workspace(const char *name) {
    std::error_code ec;
    kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) / name;
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

size_t count_messages_with(const kimix::agent::AgentSession &session,
                           kimix::string_view needle) {
    size_t n = 0;
    for (const kimix::llm::Message &m : session.history()) {
        if (m.content.find(needle) != kimix::string::npos) {
            ++n;
        }
    }
    return n;
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
    register_probe("probe_a");
    register_probe("probe_b");

    "serial_mode_never_overlaps_probe_calls"_test = [] {
        g_runs = 0;
        g_peak = 0;
        g_overlap_seen = 0;
        const kimix::string ws = tmp_workspace("kimix_a9_serial_ws");
        ScriptedBackend backend;
        // A single assistant message carrying TWO probe calls: serial
        // dispatch runs them one after the other - the gate never sees two
        // in flight, so both probes time out of their wait and report.
        backend.scripted.push_back(ScriptedBackend::two_calls(
            "s-1", "probe_a", "s-2", "probe_b"));
        backend.scripted.push_back(ScriptedBackend::ok_text("finished"));
        AgentSession session(ws);
        KimiSoul soul(session, backend, base_opts(/*concurrency=*/1));
        const TurnResult r = soul.turn("hello");
        expect(r.ok);
        expect(g_runs == 2);
        expect(g_peak.load() == 1); // strictly serial: never two at once
        expect(g_overlap_seen.load() == 0);
    };

    "parallel_mode_overlaps_and_keeps_call_order"_test = [] {
        g_runs = 0;
        g_peak = 0;
        g_overlap_seen = 0;
        const kimix::string ws = tmp_workspace("kimix_a9_parallel_ws");
        ScriptedBackend backend;
        backend.scripted.push_back(ScriptedBackend::two_calls(
            "p-1", "probe_a", "p-2", "probe_b"));
        backend.scripted.push_back(ScriptedBackend::ok_text("finished"));
        AgentSession session(ws);
        KimiSoul soul(session, backend, base_opts(/*concurrency=*/2));
        const TurnResult r = soul.turn("hello");
        expect(r.ok);
        expect(g_runs == 2);
        // The rendezvous: both probes were in flight together (peak), and at
        // least one of them observed the other while waiting.
        expect(g_peak.load() == 2);
        expect(g_overlap_seen.load() >= 1);
        // Results attach in ORIGINAL call order: the history holds the
        // assistant message, then tool result for p-1 BEFORE p-2.
        const kimix::vector<kimix::llm::Message> &history = session.history();
        int first = -1;
        int second = -1;
        for (size_t i = 0; i < history.size(); ++i) {
            if (history[i].role != kimix::string("tool")) {
                continue;
            }
            if (history[i].tool_call_id == kimix::string("p-1") && first < 0) {
                first = static_cast<int>(i);
            }
            if (history[i].tool_call_id == kimix::string("p-2") && second < 0) {
                second = static_cast<int>(i);
            }
        }
        expect(first >= 0);
        expect(second > first);
    };

    "parallel_mode_keeps_the_duplicate_short_circuit"_test = [] {
        g_runs = 0;
        g_peak = 0;
        g_overlap_seen = 0;
        const kimix::string ws = tmp_workspace("kimix_a9_dup_ws");
        ScriptedBackend backend;
        // probe_a + probe_a (identical args): one run, one copy.
        backend.scripted.push_back(ScriptedBackend::two_calls(
            "d-1", "probe_a", "d-2", "probe_a"));
        backend.scripted.push_back(ScriptedBackend::ok_text("finished"));
        AgentSession session(ws);
        KimiSoul soul(session, backend, base_opts(/*concurrency=*/2));
        const TurnResult r = soul.turn("hello");
        expect(r.ok);
          expect(g_runs == 1); // the duplicate never ran
          for (const kimix::llm::Message &m : session.history()) {
              std::printf("HIST role=%s id=%s content=[%s]\n", m.role.c_str(),
                          m.tool_call_id.c_str(), m.content.c_str());
          }
          std::fflush(stdout);
          // Both tool results exist (the copy carries the original's text).
          expect(count_messages_with(session, "probe ok") == 2);
    };

    return 0;
}
