// agent/token_ledger.cpp - TokenLedger implementation (see token_ledger.h).

#include "agent/token_ledger.h"

namespace kimix::agent {

void TokenLedger::update_token_count(int64_t recorded_usage_input) noexcept {
    // context.py:826-831: _token_count = token_count; _pending = 0; and the
    // usage record persists the snapshot.
    _token_count = recorded_usage_input;
    _pending_estimate = 0;
    _has_recorded = true;
}

void TokenLedger::add_pending_estimate(int64_t estimated_new_tokens) noexcept {
    if (estimated_new_tokens > 0) {
        _pending_estimate += estimated_new_tokens;
    }
}

void TokenLedger::reanchor(int64_t estimated_recorded) noexcept {
    _token_count = estimated_recorded < 0 ? 0 : estimated_recorded;
    _pending_estimate = 0;
    _has_recorded = true;
}

void TokenLedger::clear() noexcept {
    // context.py:852-870 (clear): history, count, pending, checkpoints and the
    // system prompt all reset together.
    _token_count = 0;
    _pending_estimate = 0;
    _has_recorded = false;
}

} // namespace kimix::agent
