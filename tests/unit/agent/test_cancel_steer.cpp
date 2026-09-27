// test_cancel_steer.cpp - Unit + turn-level tests for Phase-3 cancellation
// and steering (src/agent/cancel.*, src/agent/steer.*, the turn-loop wiring in
// src/agent/soul.cpp; gaps G7/G8).
//
// Coverage:
// * CancelToken: cancel/reset/generation, chained parent flags (sub-agents).
// * SteerQueue: blank-steer guard, FIFO push/drain, pending/clear, the wake
//   event (request vs push), push_sync consumed/timeout/close semantics.
// * Steer: from_session resolution through the soul registry, pending/clear,
//   push() returning false (and queueing stale) when the soul is idle.
// * Turn level, driven by a scripted BLOCKING backend (no network):
//   - G8: Ctrl-C semantics - cancel the token mid-request from another
//     thread; the backend observes the abort check and returns promptly; the
//     turn reports cancelled, keeps the session and grows NO partial
//     assistant message into the history; TurnEnd is emitted exactly once.
//   - G7 step-boundary: a steer queued mid-turn is consumed BEFORE the turn
//     ends and forces another step (the follow-up never dropped).
//   - G7 mid-stream: request_steer wakes the loop; the in-flight step is
//     interrupted (StepInterrupted + SteerInput on the wire), its partial
//     output is not grown into the context, and a fresh step answers.
//   - G7 stale: steers queued while idle are flushed at turn init.
//
// Framework: Boost.UT (tests/ut/ut.hpp).

#include "ut/ut.hpp"

#include <agent/soul.h>
#include <agent/steer.h>
#include <agent/wire.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

// Scripted backend that can block inside chat() until released or until the
// abort check flips - the harness for both cancellation and mid-stream
// steering scenarios.
class BlockingBackend : public kimix::agent::IChatBackend {
public:
    kimix::vector<kimix::llm::ChatResult> scripted;
    size_t index = 0;
    int calls = 0;
    std::atomic<bool> in_chat{false};
    std::atomic<bool> release{false};
    bool block = false; // chat() waits for release/abort before answering
    // One-shot hook fired at chat() entry (used to steer from "another
    // thread" while the turn is blocked here).
    kimix::function<void()> on_first_chat;

    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk,
         const kimix::llm::AbortCheck *abort) override {
        (void)messages;
        (void)tools;
        (void)on_chunk;
        ++calls;
        in_chat.store(true);
        if (calls == 1 && on_first_chat) {
            on_first_chat();
        }
        if (block && calls == 1) { // only the first request blocks
            for (;;) {
                // The abort check arrives per call (owned by the calling
                // soul's turn) - polling it is what a streaming provider
                // does inside its ContentReceiver.
                if (abort != nullptr && abort->aborted()) {
                    in_chat.store(false);
                    kimix::llm::ChatResult r;
                    r.ok = false;
                    r.error_kind = kimix::llm::ChatErrorKind::aborted;
                    r.error = "request aborted";
                    return r;
                }
                if (release.load(std::memory_order_acquire)) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            release.store(false);
        }
        in_chat.store(false);
        if (index < scripted.size()) {
            return scripted[index++];
        }
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content = "done";
        return r;
    }
    int64_t max_context_size() const override { return 128000; }
    kimix::string model_name() const override { return "blocking"; }
};

kimix::llm::ChatResult ok_text(kimix::string text) {
    kimix::llm::ChatResult r;
    r.ok = true;
    r.content = std::move(text);
    return r;
}

kimix::agent::KimiSoul::options test_options() {
    kimix::agent::KimiSoul::options opts;
    opts.system_prompt = "test";
    opts.auto_compact = false;
    return opts;
}

// Wire capture: records every event as "Type:field" for order/content checks.
struct CaptureSink : kimix::agent::WireSink {
    kimix::vector<kimix::string> events;
    void wire_turn_begin(kimix::string_view user_input) override {
        events.push_back(kimix::string("TurnBegin:") + kimix::string(user_input));
    }
    void wire_turn_end() override { events.push_back("TurnEnd"); }
    void wire_step_begin(int32_t n) override {
        events.push_back(kimix::string("StepBegin:") +
                         std::to_string(n).c_str());
    }
    void wire_step_interrupted() override {
        events.push_back("StepInterrupted");
    }
    void wire_steer_input(kimix::string_view user_input) override {
        events.push_back(kimix::string("SteerInput:") + kimix::string(user_input));
    }
    void wire_step_retry(int32_t, int32_t, int32_t, double, kimix::string_view,
                         int32_t) override {
        events.push_back("StepRetry");
    }
    void wire_status_update(double, int64_t, int64_t, int64_t, int64_t, int64_t,
                            int64_t) override {
        events.push_back("StatusUpdate");
    }
    void wire_compaction_begin(kimix::string_view, kimix::string_view) override {
        events.push_back("CompactionBegin");
    }
    void wire_compaction_end(kimix::string_view, kimix::string_view, int64_t,
                             int64_t, kimix::string_view) override {
        events.push_back("CompactionEnd");
    }
    void wire_llm_request(const kimix::agent::llm_request_record &rec) override {
        events.push_back(kimix::string("LLMRequest:") + rec.kind);
    }
    void wire_llm_tools_snapshot(kimix::string_view,
                                 const kimix::vector<kimix::llm::Tool> &) override {
        events.push_back("LLMToolsSnapshot");
    }
    void wire_mcp_tools_discovered(kimix::string_view, kimix::string_view,
                                   const kimix::vector<kimix::llm::Tool> &,
                                   const kimix::vector<kimix::string> &,
                                   const kimix::vector<kimix::string> &) override {
        events.push_back("MCPToolsDiscovered");
    }
    void wire_approval_request(kimix::string_view, kimix::string_view,
                               kimix::string_view, kimix::string_view,
                               kimix::string_view) override {
        events.push_back("ApprovalRequest");
    }
    void wire_approval_response(kimix::string_view, kimix::string_view,
                                kimix::string_view) override {
        events.push_back("ApprovalResponse");
    }
    void wire_btw_begin(kimix::string_view, kimix::string_view) override {
        events.push_back("BtwBegin");
    }
    void wire_btw_end(kimix::string_view, kimix::string_view,
                      kimix::string_view) override {
        events.push_back("BtwEnd");
    }
    int count(kimix::string_view prefix) const {
        int n = 0;
        for (const kimix::string &e : events) {
            if (e.compare(0, prefix.size(), prefix.data(), prefix.size()) == 0) {
                ++n;
            }
        }
        return n;
    }
};

size_t history_role_count(const kimix::agent::AgentSession &session,
                          kimix::string_view role) {
    size_t n = 0;
    for (const kimix::llm::Message &m : session.history()) {
        if (m.role == role) {
            ++n;
        }
    }
    return n;
}

} // namespace

int main() {
    using kimix::agent::CancelToken;
    using kimix::agent::KimiSoul;
    using kimix::agent::Steer;
    using kimix::agent::SteerQueue;

    // ── CancelToken ─────────────────────────────────────────────────────
    "cancel_token_basic"_test = [] {
        CancelToken token;
        expect(!token.cancelled());
        token.cancel();
        expect(token.cancelled());
        expect(token.flag().load());
        const uint64_t gen0 = token.generation();
        token.reset();
        expect(!token.cancelled());
        expect(token.generation() > gen0);
    };

    "cancel_token_chained_parent"_test = [] {
        CancelToken token;
        std::atomic<bool> parent{false};
        token.chain(&parent);
        expect(!token.cancelled());
        parent.store(true);
        expect(token.cancelled()); // interrupt_agent's flag rides in
        token.reset();             // reset clears OWN flag only
        expect(token.cancelled());
        parent.store(false);
        expect(!token.cancelled());
    };

    // ── SteerQueue ──────────────────────────────────────────────────────
    "steer_queue_blank_rejected"_test = [] {
        SteerQueue q;
        expect(!q.push(""));
        expect(!q.push("   \t "));
        expect(q.pending() == 0);
    };

    "steer_queue_fifo_and_wake"_test = [] {
        SteerQueue q;
        expect(q.push("one"));
        expect(q.request("two")); // step-boundary vs interrupting enqueue
        expect(q.pending() == 2);
        expect(q.wake_peek());      // request raised the wake event
        expect(q.wake_requested()); // ... observed by the test-and-clear
        expect(!q.wake_peek());     // ... and now cleared
        const kimix::vector<kimix::string> drained = q.drain();
        expect(drained.size() == 2);
        expect(drained[0] == "one");
        expect(drained[1] == "two");
        expect(q.pending() == 0);
    };

    "steer_queue_push_sync_consumed"_test = [] {
        SteerQueue q;
        std::atomic<bool> consumed{false};
        std::thread drainer([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            q.drain();
            consumed.store(true);
        });
        expect(q.push_sync("hello", 5.0)); // blocks until the drain
        drainer.join();
        expect(consumed.load());
    };

    "steer_queue_push_sync_timeout_and_close"_test = [] {
        SteerQueue q;
        // Nobody drains: the wait elapses and reports not-consumed.
        expect(!q.push_sync("hello", 0.05));
        // Turn end unblocks a waiting push_sync with false...
        std::atomic<bool> finished{false};
        std::thread waiter([&] {
            q.push_sync("late", -1.0); // no timeout: only close() can unblock
            finished.store(true);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        expect(!finished.load());
        q.close();
        waiter.join();
        expect(finished.load());
        // ...and a closed queue rejects new steers until reopened.
        expect(!q.push("after close"));
        q.reopen();
        expect(q.push("after reopen"));
    };

    // ── Steer API (from_session resolution) ─────────────────────────────
    "steer_from_session"_test = [] {
        kimix::agent::AgentSession session;
        BlockingBackend backend;
        {
            KimiSoul soul(session, backend, test_options());
            auto steer = Steer::from_session(session);
            expect(steer.has_value());
            expect(steer->pending() == 0);
            // Idle push: queued as a stale steer, reported not delivered.
            expect(!steer->push("queued while idle"));
            expect(steer->pending() == 1);
            steer->clear();
            expect(steer->pending() == 0);
            expect(!soul.is_running());
        }
        // The soul unregisters on destruction.
        expect(!Steer::from_session(session).has_value());
    };

    // ── Turn level: G8 cancellation ─────────────────────────────────────
    "turn_cancel_interrupts_in_flight_request"_test = [] {
        kimix::agent::AgentSession session;
        BlockingBackend backend;
        backend.block = true;
        backend.scripted.push_back(ok_text("never delivered"));
        KimiSoul soul(session, backend, test_options());
        CaptureSink wire;
        soul.set_wire_sink(&wire);

        CancelToken token;
        kimix::agent::TurnResult result;
        std::atomic<bool> finished{false};
        const auto started = std::chrono::steady_clock::now();
        std::thread turn_thread([&] {
            result = soul.turn("hello", {}, token);
            finished.store(true);
        });
        // Wait until the request is actually in flight, then Ctrl-C.
        while (!backend.in_chat.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        token.cancel();
        turn_thread.join();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        // The turn aborted promptly (the backend unblocked via the abort
        // check, not via a timeout).
        expect(elapsed < std::chrono::seconds(10));
        expect(finished.load());
        expect(result.cancelled);
        expect(result.error_kind == kimix::agent::TurnErrorKind::cancelled);
        expect(!result.ok);
        // The session is kept: the user message stands, and NO partial
        // assistant message was grown into the history (G8 cleanup).
        expect(history_role_count(session, "user") == 1);
        expect(history_role_count(session, "assistant") == 0);
        // Exactly one turn frame on the wire, with a StepBegin before the end.
        expect(wire.count("TurnBegin:") == 1);
        expect(wire.count("TurnEnd") == 1);
        expect(wire.count("StepBegin:") == 1);
        expect(wire.events.front().compare(0, 10, "TurnBegin:") == 0);
        expect(wire.events.back() == "TurnEnd");
        expect(!soul.is_running());
    };

    "turn_cancel_skips_step_retry_sleep"_test = [] {
        kimix::agent::AgentSession session;
        BlockingBackend backend;
        backend.block = true;
        backend.scripted.push_back(ok_text("x"));
        KimiSoul soul(session, backend, test_options());
        CancelToken token;
        kimix::agent::TurnResult result;
        std::thread turn_thread(
            [&] { result = soul.turn("hello", {}, token); });
        while (!backend.in_chat.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto t0 = std::chrono::steady_clock::now();
        token.cancel();
        turn_thread.join();
        // Cancelled mid-request: no _RateLimitAwareWait sleep may run, so the
        // turn ends in milliseconds.
        expect(std::chrono::steady_clock::now() - t0 <
               std::chrono::seconds(5));
        expect(result.cancelled);
    };

    // ── Turn level: G7 steering ─────────────────────────────────────────
    "turn_step_boundary_steer_forces_another_step"_test = [] {
        kimix::agent::AgentSession session;
        BlockingBackend backend;
        backend.block = true;
        backend.scripted.push_back(ok_text("first answer"));
        backend.scripted.push_back(ok_text("second answer"));
        KimiSoul soul(session, backend, test_options());
        CaptureSink wire;
        soul.set_wire_sink(&wire);

        // While the first request is in flight, the user types a follow-up:
        // a plain (step-boundary) steer. The backend then answers "first
        // answer" - a clean stop - but the pending steer must force another
        // step instead of letting the turn end (kimisoul.py:1352-1357).
        backend.on_first_chat = [&] {
            soul.steer("actually, do the other thing");
            backend.release.store(true);
        };
        const kimix::agent::TurnResult result = soul.turn("original ask");
        expect(result.ok);
        expect(result.content == "second answer");
        expect(result.steps == 2);
        // The steer became a real user message in the kept history.
        expect(history_role_count(session, "user") == 2);
        expect(history_role_count(session, "assistant") == 2);
        // One SteerInput wire record per injected steer.
        expect(wire.count("SteerInput:") == 1);
        expect(wire.count("StepInterrupted") == 0);
        expect(wire.count("TurnEnd") == 1);
    };

    "turn_mid_stream_steer_interrupts_the_step"_test = [] {
        kimix::agent::AgentSession session;
        BlockingBackend backend;
        backend.block = true; // chat() polls the abort check
        // The first request is interrupted before consuming a script entry;
        // the fresh step after the steer injection answers "recovered".
        backend.scripted.push_back(ok_text("recovered"));
        KimiSoul soul(session, backend, test_options());
        CaptureSink wire;
        soul.set_wire_sink(&wire);

        // request_steer from "another thread" while the step streams: the
        // wake event flips the abort check, the backend returns promptly, and
        // the loop takes the steer-interrupt path (StepInterrupted + fresh
        // step with the steer in context).
        backend.on_first_chat = [&] {
            soul.request_steer("wait, redirect!");
        };
        const auto started = std::chrono::steady_clock::now();
        const kimix::agent::TurnResult result = soul.turn("original ask");
        expect(std::chrono::steady_clock::now() - started <
               std::chrono::seconds(10));
        expect(result.ok);
        expect(result.content == "recovered");
        // The interrupted step's partial output is NOT in the history: one
        // user (original) + one steer + one assistant (the recovery answer).
        expect(history_role_count(session, "user") == 2);
        expect(history_role_count(session, "assistant") == 1);
        expect(wire.count("StepInterrupted") == 1);
        expect(wire.count("SteerInput:wait, redirect!") == 1);
        // Step frames: 1 begin, interrupt, 2 begin, end.
        expect(wire.count("StepBegin:") == 2);
        expect(wire.count("TurnEnd") == 1);
    };

    "turn_stale_steer_flushed_at_turn_init"_test = [] {
        kimix::agent::AgentSession session;
        BlockingBackend backend;
        backend.scripted.push_back(ok_text("plain answer"));
        KimiSoul soul(session, backend, test_options());
        // A steer queued while no turn runs is stale: the next turn discards
        // it at init (kimisoul.py:1148-1153) instead of injecting it.
        soul.steer("from the previous conversation");
        expect(soul.pending_steers() == 1);
        const kimix::agent::TurnResult result = soul.turn("fresh ask");
        expect(result.ok);
        expect(result.content == "plain answer");
        expect(history_role_count(session, "user") == 1);
        expect(soul.pending_steers() == 0);
    };

    "turn_blank_steer_never_injected"_test = [] {
        kimix::agent::AgentSession session;
        BlockingBackend backend;
        backend.block = true;
        backend.scripted.push_back(ok_text("answer"));
        backend.scripted.push_back(ok_text("follow-up answer"));
        KimiSoul soul(session, backend, test_options());
        backend.on_first_chat = [&] {
            soul.steer("   "); // blank: dropped, never a user message
            backend.release.store(true);
        };
        const kimix::agent::TurnResult result = soul.turn("ask");
        expect(result.ok);
        expect(result.steps == 1); // no steer consumed -> no extra step
        expect(history_role_count(session, "user") == 1);
    };
}
