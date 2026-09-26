// agent/steer.h - Mid-stream steering for the agent turn loop (G7).
//
// Port of kimi_cli/soul/steer.py + the steer machinery in kimisoul.py
// (1002-1058, 1148-1153, 1276-1305, 1352-1355):
// * SteerQueue - the thread-safe soul-side queue (the reference's
//   asyncio.Queue + _steer_wake_event). Two delivery semantics, matching the
//   reference: steer() is step-boundary only (kimisoul.py:1007-1020,
//   "never mid-stream"); request_steer() additionally sets the wake event so
//   the in-flight step is interrupted and the steers injected as follow-up
//   user messages before a fresh step (kimisoul.py:1022-1030, 1276-1305).
// * Steer - the upper-level API used by external callers (steer.py): push()
//   marshals onto the loop from any thread, push_sync() blocks the calling
//   thread until the steer is consumed or the turn ends (steer.py:79-124;
//   the reference blocks on the cross-loop marshalling future, the native
//   version blocks until consumption - the task-specified semantics), plus
//   pending()/clear()/close() introspection (steer.py:130-147) and
//   from_session() resolution (steer.py:39-60) through the soul registry the
//   KimiSoul maintains for its AgentSession.
//
// Empty/whitespace-only steers are dropped (steer.py + kimisoul.py's
// _user_input_is_empty guard), never injected as an empty user message.
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers,
// no exceptions, no RTTI.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include <core/kimix_core.h>

namespace kimix::agent {

class AgentSession;
class KimiSoul;

// ---------------------------------------------------------------------------
// SteerQueue
// ---------------------------------------------------------------------------

// Thread-safe queue + wake event + consumption tracking. The owning loop
// (KimiSoul::turn) drains it; any thread may push. `close()` marks the turn
// end: push_sync waiters unblock and a later push_sync on a closed queue
// returns immediately.
class SteerQueue {
public:
    // Step-boundary enqueue (KimiSoul.steer). False when the content is blank
    // (never queue an empty user message) or the queue is closed.
    bool push(kimix::string_view content);
    // Interrupting enqueue (KimiSoul.request_steer): queue + raise the wake
    // event so the in-flight step is interrupted mid-stream.
    bool request(kimix::string_view content);
    // push() + block the calling thread until every queued steer has been
    // consumed (drained by the loop) or close() fires (turn end), or
    // `timeout_s` elapses (<= 0 == no timeout). True when the steer was
    // consumed; false on timeout/close/blank content.
    bool push_sync(kimix::string_view content, double timeout_s);
    // Take every pending steer, FIFO. Notifies push_sync waiters.
    kimix::vector<kimix::string> drain();
    // Number of queued-but-unconsumed steers (Steer.pending()).
    size_t pending() const;
    // Drop every pending steer without injecting them (Steer.clear(); also
    // the stale-steer flush at turn init, kimisoul.py:1148-1153).
    void clear();
    // Test-and-clear the wake event (did a request_steer's interrupt fire?).
    bool wake_requested() noexcept { return _wake.exchange(false); }
    // Peek without clearing (the streaming abort check polls this).
    bool wake_peek() const noexcept {
        return _wake.load(std::memory_order_acquire);
    }
    // Clear a stale wake flag at step start (kimisoul.py:1281).
    void reset_wake() noexcept { _wake.store(false, std::memory_order_release); }
    // Turn end: unblock push_sync waiters; further push_sync calls return
    // false immediately. The next turn reopens the queue with reopen().
    void close();
    void reopen() noexcept;
    bool closed() const noexcept { return _closed; }

private:
    mutable std::mutex _mutex;
    std::condition_variable _consumed;
    kimix::vector<kimix::string> _queue;
    std::atomic<bool> _wake{false};
    bool _closed = false;
};

// ---------------------------------------------------------------------------
// Steer
// ---------------------------------------------------------------------------

// Push a message into a running agent loop no matter what state it is in -
// including while the model is mid-stream (steer.py class docstring). Construct
// directly from a KimiSoul or resolve one from a session with from_session().
class Steer {
public:
    explicit Steer(KimiSoul &soul) : _soul(&soul) {}

    // Resolve a running soul for `session` (steer.py:39-60's from_session:
    // session._cli.soul -> session.soul -> session._soul). Empty when no soul
    // is registered for the session.
    static kimix::optional<Steer> from_session(AgentSession &session) noexcept;

    // Queue *content* for injection into the current turn. True when it was
    // delivered to a running soul; when the soul is not running the content is
    // still queued (it is discarded as a stale steer at the next turn init,
    // matching the reference) and false is returned.
    bool push(kimix::string_view content);
    // Synchronous wrapper for non-loop threads: blocks until the steer is
    // consumed or the turn ends (with `timeout_s`, <= 0 == wait forever).
    // Only call from a DIFFERENT thread than the one running the turn.
    bool push_sync(kimix::string_view content, double timeout_s);
    // Number of steers queued but not yet consumed (Steer.pending()).
    size_t pending() const;
    // Drain any pending steers without injecting them (Steer.clear()).
    void clear();
    // Detach the soul reference (Steer.close()).
    void close() noexcept { _soul = nullptr; }

private:
    KimiSoul *_soul;
};

// Soul registry backing Steer::from_session: KimiSoul registers itself for
// its AgentSession for the lifetime of the soul (the reference's
// `_cli.soul -> soul -> _soul` attribute chain). Process-wide; a session has
// at most one live soul.
void steer_register_soul(AgentSession &session, KimiSoul &soul) noexcept;
void steer_unregister_soul(AgentSession &session, KimiSoul &soul) noexcept;

} // namespace kimix::agent
