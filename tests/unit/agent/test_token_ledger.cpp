// test_token_ledger.cpp - Unit tests for kimix::agent::TokenLedger
// (src/agent/token_ledger.*), the port of kimi_cli/soul/context.py's token
// accounting: update_token_count anchors the recorded provider usage and
// clears the pending estimate; appended messages add incremental estimates;
// token_count_with_pending = recorded + pending.
//
// Framework: Boost.UT (tests/ut/ut.hpp). CPU-only, no network.

#include "ut/ut.hpp"

#include <agent/token_ledger.h>

#include <cstdint>

using namespace boost::ut;
using namespace boost::ut::literals;

int main() {
    using kimix::agent::TokenLedger;

    "fresh_ledger_has_no_recorded_usage"_test = [] {
        TokenLedger ledger;
        expect(!ledger.has_recorded_usage());
        expect(ledger.token_count() == 0_i);
        expect(ledger.token_count_with_pending() == 0_i);
    };

    "update_token_count_anchors_and_clears_pending"_test = [] {
        TokenLedger ledger;
        ledger.add_pending_estimate(120);
        expect(ledger.token_count_with_pending() == 120_i);
        ledger.update_token_count(4096);
        expect(ledger.has_recorded_usage());
        expect(ledger.token_count() == 4096_i);
        // The pending estimate accumulated before the snapshot is superseded.
        expect(ledger.token_count_with_pending() == 4096_i);
    };

    "pending_estimate_accumulates_on_top_of_recorded"_test = [] {
        TokenLedger ledger;
        ledger.update_token_count(8000);
        ledger.add_pending_estimate(100);
        ledger.add_pending_estimate(250);
        expect(ledger.token_count() == 8000_i);
        expect(ledger.token_count_with_pending() == 8350_i);
        // A new provider measurement re-anchors and clears the pending part.
        ledger.update_token_count(9000);
        expect(ledger.token_count_with_pending() == 9000_i);
    };

    "pending_estimate_ignores_non_positive_deltas"_test = [] {
        TokenLedger ledger;
        ledger.update_token_count(100);
        ledger.add_pending_estimate(-50);
        ledger.add_pending_estimate(0);
        expect(ledger.token_count_with_pending() == 100_i);
    };

    "reanchor_sets_recorded_without_pending"_test = [] {
        // kimisoul.py:2223-2232 (post-compaction): estimate(history) +
        // count_tokens(system_prompt), no pending on top.
        TokenLedger ledger;
        ledger.add_pending_estimate(999);
        ledger.reanchor(1234);
        expect(ledger.has_recorded_usage());
        expect(ledger.token_count() == 1234_i);
        expect(ledger.token_count_with_pending() == 1234_i);
    };

    "clear_resets_everything"_test = [] {
        TokenLedger ledger;
        ledger.update_token_count(500);
        ledger.add_pending_estimate(10);
        ledger.clear();
        expect(!ledger.has_recorded_usage());
        expect(ledger.token_count() == 0_i);
        expect(ledger.token_count_with_pending() == 0_i);
    };

    return 0;
}
