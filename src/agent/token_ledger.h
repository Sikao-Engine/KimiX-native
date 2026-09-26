// agent/token_ledger.h - Provider-anchored token accounting for a session.
//
// Port of kimi_cli/soul/context.py's token accounting (context.py:597-680,
// kimisoul.py:1887-1905, 2223-2232): after every successful step the provider-
// measured usage anchors the recorded count (update_token_count(usage.input)),
// and messages appended since the last measurement add an incremental estimate
// on top (token_count_with_pending = recorded + pending). Consumers - the
// should_auto_compact trigger and every "% used" readout - must prefer the
// recorded usage over the pure character heuristic and fall back to the
// heuristic only while nothing has been recorded yet.
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

namespace kimix::agent {

// One session's token ledger (the reference's Context._token_count +
// _pending_token_estimate pair, lifted into an object the soul owns).
//
// The recorded count is the last provider-measured INPUT token total of a
// completed request - it therefore already covers the system prompt and the
// tool schemas, exactly like the reference's usage.input (kosong
// TokenUsage.input = input_other + input_cache_read + input_cache_creation).
// The pending estimate covers messages appended since that measurement.
class TokenLedger {
public:
    // Record a provider-measured input token count and clear the pending
    // estimate (context.py update_token_count: the new snapshot supersedes
    // everything appended so far).
    void update_token_count(int64_t recorded_usage_input) noexcept;

    // Add an incremental estimate for messages appended since the last
    // measurement (context.py append_message: pending += estimate(msgs)).
    void add_pending_estimate(int64_t estimated_new_tokens) noexcept;

    // Post-compaction anchor (kimisoul.py:2223-2232): the reference stores
    // estimate(history) + count_tokens(system_prompt) as the new recorded
    // count with no pending on top.
    void reanchor(int64_t estimated_recorded) noexcept;

    // Reset everything (context.py clear).
    void clear() noexcept;

    // True once a provider measurement (or a post-compaction anchor) exists.
    // Until then consumers must fall back to the char heuristic.
    bool has_recorded_usage() const noexcept { return _has_recorded; }

    // The recorded usage; 0 when nothing was recorded (context.py
    // token_count).
    int64_t token_count() const noexcept { return _token_count; }

    // Recorded usage + the incremental estimate of the in-flight growth
    // (context.py token_count_with_pending).
    int64_t token_count_with_pending() const noexcept {
        return _token_count + _pending_estimate;
    }

private:
    int64_t _token_count = 0;         // Context._token_count
    int64_t _pending_estimate = 0;    // Context._pending_token_estimate
    bool _has_recorded = false;
};

} // namespace kimix::agent
