// agent/cancel.h - Cancellation token for the agent turn loop (G8).
//
// Port of the reference's run_soul(cancel_event) handle (kimi_cli/soul/
// __init__.py:293-427: an outside asyncio.Event that races the soul task; when
// set, the run is stopped gracefully and RunCancelled is raised) onto the
// exception-free, single-threaded native loop:
// * the token is caller-owned and passed into KimiSoul::turn() per turn, like
//   the reference passes cancel_event into run_soul per run;
// * cancel()/reset() are single lock-free atomic stores, so the flag's raw
//   address may be handed to a Ctrl-C handler (SetConsoleCtrlHandler's console
//   thread on Windows, a sigaction(SIGINT) handler on POSIX) - setting the
//   flag from there stays async-signal-safe;
// * the turn polls cancelled() at every step boundary and through the LLM
//   provider's streaming abort hook (llm::AbortCheck), so a cancelled request
//   stops streaming and returns promptly (the httplib ContentReceiver);
// * `chain()` lets a secondary flag ride along - the sub-agent runner uses it
//   to surface interrupt_agent's cancel flag into the child turn.
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers,
// no exceptions, no RTTI.

#pragma once

#include <atomic>
#include <cstdint>

#include <core/kimix_core.h>

namespace kimix::agent {

class CancelToken {
public:
    // Set the flag. Async-signal-safe: one lock-free atomic store, callable
    // from a Ctrl-C handler on any platform. Idempotent.
    void cancel() noexcept { _flag.store(true, std::memory_order_release); }
    // Clear the flag for a fresh turn. The generation keeps counting so
    // observers can tell a new turn from the cancelled one.
    void reset() noexcept {
        _flag.store(false, std::memory_order_release);
        _generation.fetch_add(1, std::memory_order_relaxed);
    }
    // Own flag OR the chained parent flag (nullptr == none).
    bool cancelled() const noexcept {
        if (_flag.load(std::memory_order_acquire)) {
            return true;
        }
        const std::atomic<bool> *parent = _chained;
        return parent != nullptr && parent->load(std::memory_order_acquire);
    }
    // The raw own flag - the address a signal handler stores into.
    const std::atomic<bool> &flag() const noexcept { return _flag; }
    std::atomic<bool> &flag() noexcept { return _flag; }
    // Bumped by every reset(); monotonic across the token's lifetime.
    uint64_t generation() const noexcept {
        return _generation.load(std::memory_order_relaxed);
    }
    // Secondary flag that also cancels this token (not cleared by reset()):
    // the sub-agent runner chains interrupt_agent's run-cancel flag here so a
    // parent-side abort lands in the child turn without the parent touching
    // the child's own flag.
    void chain(const std::atomic<bool> *parent) noexcept { _chained = parent; }

private:
    std::atomic<bool> _flag{false};
    std::atomic<uint64_t> _generation{0};
    const std::atomic<bool> *_chained = nullptr;
};

} // namespace kimix::agent
