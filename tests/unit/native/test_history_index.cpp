// Test for src/runtime/index/history_index.h (plan 006).
// This test covers:
// - append + search: results sorted (score desc, turn_id asc), score > 0
// - tie-breaking: identical-text turns keep (score desc, doc_id asc) order
// - eviction: the 501st append drops turn 0, turn_count stays MAX_TURNS
// - mark_compacted flags all currently-indexed turns (reference semantics)
// - get_by_id for present/evicted turn ids
// - save/load round-trip byte-identical; load performs NO tokenization
// - search results identical after save/load
// - pop_front correctness
// - empty index save/load round-trip
// - incremental append after search (no rebuild path)

#include "ut/ut.hpp"
#include "bench_util.h"
#include <runtime/index/history_index.h>

#include <cmath>
#include <cstdint>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::index;

namespace {

turn_meta make_turn(uint32_t id, kimix::string_view text, uint8_t role = 0,
                    bool compacted = false, double ts = 1.0) {
    turn_meta t;
    t.turn_id = id;
    t.timestamp = ts;
    t.role = role;
    t.is_compacted = compacted;
    t.text.assign(text.data(), text.size());
    return t;
}

bool scores_close(double a, double b) {
    return std::abs(a - b) < 1e-9;
}

bool same_result(const turn_meta& a, const turn_meta& b) {
    return a.turn_id == b.turn_id && scores_close(a.score, b.score);
}

// Deterministic pseudo-random text stream (xorshift) — pre-normalized ASCII.
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<uint32_t>(state);
    }
    uint32_t bounded(uint32_t n) { return n == 0 ? 0 : next() % n; }
};

kimix::string make_text(Rng& rng, uint32_t words, uint32_t vocab) {
    kimix::string s;
    for (uint32_t i = 0; i < words; ++i) {
        if (i) {
            s.push_back(' ');
        }
        s += "t" + std::to_string(rng.bounded(vocab));
    }
    return s;
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "append_search_ordering"_test = [] {
        HistoryIndex h;
        // doc0 has alpha twice -> strictly higher score than doc1 for "alpha".
        h.append_turns(kimix::span<const turn_meta>({
            make_turn(0, "alpha alpha beta gamma"),
            make_turn(1, "alpha beta gamma delta"),
            make_turn(2, "delta epsilon zeta"),
        }));
        expect(eq(h.turn_count(), 3u));

        auto r = h.search("alpha", 3);
        expect(eq(r.size(), 2u));
        expect(eq(r[0].turn_id, 0u)) << "higher tf must rank first";
        expect(eq(r[1].turn_id, 1u));
        expect(r[0].score > r[1].score);
        expect(r[0].score > 0.0);
        // Scores must be non-increasing across the result list.
        for (size_t i = 1; i < r.size(); ++i) {
            expect(r[i - 1].score >= r[i].score);
        }
        // Copies carry the turn metadata (not just ids).
        expect(eq(r[0].text, kimix::string_view("alpha alpha beta gamma")));

        // Query with no matches -> empty.
        expect(h.search("zzz", 3).empty());
    };

    "tie_break_score_desc_doc_asc"_test = [] {
        HistoryIndex h;
        // Identical texts -> identical scores; ties resolve to lower turn_id.
        h.append_turns(kimix::span<const turn_meta>({
            make_turn(7, "beta gamma shared"),
            make_turn(3, "beta gamma shared"),
            make_turn(9, "beta gamma shared"),
        }));
        auto r = h.search("beta gamma", 3);
        expect(eq(r.size(), 3u));
        expect(eq(r[0].turn_id, 3u));
        expect(eq(r[1].turn_id, 7u));
        expect(eq(r[2].turn_id, 9u));
        expect(scores_close(r[0].score, r[1].score));
        expect(scores_close(r[1].score, r[2].score));
    };

    "eviction_keeps_500_drops_oldest"_test = [] {
        HistoryIndex h;
        kimix::vector<turn_meta> batch;
        batch.reserve(501);
        for (uint32_t i = 0; i <= 500; ++i) {
            batch.push_back(make_turn(i, "alpha beta gamma delta epsilon"));
        }
        h.append_turns(batch);
        expect(eq(h.turn_count(), HistoryIndex::MAX_TURNS));
        expect(eq(h.turn_count(), 500u));
        // Turn 0 was evicted; turns 1..500 remain.
        expect(h.get_by_id(0) == nullptr);
        expect(h.get_by_id(1) != nullptr);
        expect(h.get_by_id(500) != nullptr);
        expect(h.get_by_id(501) == nullptr); // never added
        // Stale doc 0 postings are harmless: search skips evicted ids.
        auto r = h.search("alpha", 3);
        expect(!r.empty());
        for (const auto& t : r) {
            expect(t.turn_id != 0u);
        }
    };

    "mark_compacted_flags_all_current_turns"_test = [] {
        HistoryIndex h;
        h.append_turns(kimix::span<const turn_meta>({
            make_turn(0, "alpha"),
            make_turn(1, "beta"),
            make_turn(2, "gamma"),
        }));
        h.mark_compacted();
        expect(h.get_by_id(0)->is_compacted);
        expect(h.get_by_id(1)->is_compacted);
        expect(h.get_by_id(2)->is_compacted);
        // New turns after mark_compacted are NOT flagged (reference: only the
        // turns present at mark time are marked).
        h.append_turns(kimix::span<const turn_meta>({make_turn(3, "delta")}));
        expect(!h.get_by_id(3)->is_compacted);
        expect(h.get_by_id(0)->is_compacted);
    };

    "get_by_id_present_and_evicted"_test = [] {
        HistoryIndex h;
        h.append_turns(kimix::span<const turn_meta>({
            make_turn(42, "hello world"),
            make_turn(43, "foo bar"),
        }));
        const turn_meta* t = h.get_by_id(42);
        expect(t != nullptr);
        expect(eq(t->turn_id, 42u));
        expect(eq(t->text, kimix::string_view("hello world")));
        expect(h.get_by_id(99) == nullptr);
        expect(h.get_by_id(0) == nullptr);
    };

    "pop_front_drops_oldest_only"_test = [] {
        HistoryIndex h;
        h.append_turns(kimix::span<const turn_meta>({
            make_turn(0, "alpha"),
            make_turn(1, "beta"),
            make_turn(2, "gamma"),
        }));
        h.pop_front();
        expect(eq(h.turn_count(), 2u));
        expect(h.get_by_id(0) == nullptr);
        expect(h.get_by_id(1) != nullptr);
        expect(h.get_by_id(2) != nullptr);
        // Stale postings for doc 0 remain but are skipped by search.
        auto r = h.search("alpha beta", 3);
        expect(r.empty() || r[0].turn_id != 0u);
    };

    "save_load_roundtrip_byte_identical"_test = [] {
        Rng rng(0x6006u);
        HistoryIndex h;
        kimix::vector<turn_meta> turns;
        turns.reserve(120);
        for (uint32_t i = 0; i < 120; ++i) {
            turns.push_back(make_turn(i, make_text(rng, 12, 60),
                                      static_cast<uint8_t>(i % 3),
                                      i % 5 == 0, 1000.0 + i * 1.5));
        }
        h.append_turns(turns);
        h.mark_compacted();
        // mark_compacted flags EVERY turn — mirror that on the input so the
        // metadata round-trip comparison below compares like for like.
        for (auto& t : turns) {
            t.is_compacted = true;
        }

        kimix::string blob;
        h.save_to(blob);
        expect(blob.size() > 40u);
        expect(kimix::string_view(blob.data(), 6) == kimix::string_view("KNHIX1", 6));

        HistoryIndex loaded;
        loaded.load_from(blob);
        expect(eq(loaded.turn_count(), 120u));
        expect(loaded.get_by_id(0) != nullptr);
        expect(loaded.get_by_id(0)->is_compacted);
        expect(eq(loaded.get_by_id(119)->turn_id, 119u));
        expect(eq(loaded.get_by_id(119)->text, turns[119].text));

        // Re-save is byte-identical (deterministic format).
        kimix::string blob2;
        loaded.save_to(blob2);
        expect(eq(blob, blob2));

        // Metadata round-trips exactly (role, timestamp, flags, text).
        for (uint32_t i = 0; i < 120; ++i) {
            const turn_meta* t = loaded.get_by_id(i);
            expect(t != nullptr);
            expect(eq(t->role, turns[i].role));
            expect(scores_close(t->timestamp, turns[i].timestamp));
            expect(eq(t->is_compacted, turns[i].is_compacted));
            expect(eq(t->text, turns[i].text));
        }
    };

    "load_performs_no_tokenization"_test = [] {
        Rng rng(0xBEEFu);
        HistoryIndex h;
        kimix::vector<turn_meta> turns;
        turns.reserve(60);
        for (uint32_t i = 0; i < 60; ++i) {
            turns.push_back(make_turn(i, make_text(rng, 10, 40)));
        }
        h.append_turns(turns);
        // append_turns tokenized exactly once per turn (60), searches add one
        // query tokenization each.
        const uint64_t after_append = h.tokenize_call_count();
        expect(eq(after_append, 60u));
        (void)h.search("alpha beta", 3);
        const uint64_t after_search = h.tokenize_call_count();
        expect(eq(after_search, 61u));

        kimix::string blob;
        h.save_to(blob); // save finalizes but never tokenizes
        expect(eq(h.tokenize_call_count(), after_search));

        HistoryIndex loaded;
        loaded.load_from(blob);
        expect(eq(loaded.turn_count(), 60u));
        // THE assertion: load performs no tokenization.
        expect(eq(loaded.tokenize_call_count(), 0u));

        // And search results are identical after the load.
        auto a = h.search("alpha beta gamma", 5);
        auto b = loaded.search("alpha beta gamma", 5);
        expect(eq(a.size(), b.size()));
        for (size_t i = 0; i < a.size(); ++i) {
            expect(same_result(a[i], b[i])) << "result " << i;
        }
    };

    "search_results_identical_after_save_load"_test = [] {
        Rng rng(0x1234u);
        HistoryIndex h;
        kimix::vector<turn_meta> turns;
        turns.reserve(200);
        for (uint32_t i = 0; i < 200; ++i) {
            turns.push_back(make_turn(i, make_text(rng, 8, 50)));
        }
        h.append_turns(turns);

        const char* queries[] = {"alpha", "beta gamma", "t17", "t3 t41",
                                 "epsilon delta", "gamma"};
        kimix::vector<kimix::vector<turn_meta>> expected;
        for (const char* q : queries) {
            expected.push_back(h.search(q, 5));
        }

        kimix::string blob;
        h.save_to(blob);
        HistoryIndex loaded;
        loaded.load_from(blob);

        for (size_t qi = 0; qi < 6; ++qi) {
            auto got = loaded.search(queries[qi], 5);
            expect(eq(got.size(), expected[qi].size())) << "query " << qi;
            for (size_t i = 0; i < got.size() && i < expected[qi].size(); ++i) {
                expect(same_result(got[i], expected[qi][i]))
                    << "query " << qi << " result " << i;
            }
        }
    };

    "empty_index_roundtrip"_test = [] {
        HistoryIndex h;
        expect(eq(h.turn_count(), 0u));
        expect(h.search("anything", 3).empty());

        kimix::string blob;
        h.save_to(blob);
        expect(blob.size() >= 6u);

        HistoryIndex loaded;
        loaded.load_from(blob);
        expect(eq(loaded.turn_count(), 0u));
        expect(loaded.search("anything", 3).empty());

        kimix::string blob2;
        loaded.save_to(blob2);
        expect(eq(blob, blob2));
    };

    "malformed_blob_resets_object"_test = [] {
        HistoryIndex h;
        h.append_turns(kimix::span<const turn_meta>({
            make_turn(0, "alpha"),
            make_turn(1, "beta"),
        }));
        expect(eq(h.turn_count(), 2u));

        // Wrong magic.
        expect(!h.load_from("garbage-not-a-blob"));
        expect(eq(h.turn_count(), 0u));
        expect(h.search("alpha", 3).empty());

        // Valid magic, truncated body.
        HistoryIndex good;
        good.append_turns(kimix::span<const turn_meta>({make_turn(0, "alpha")}));
        kimix::string blob;
        good.save_to(blob);
        HistoryIndex bad;
        bad.append_turns(kimix::span<const turn_meta>({make_turn(9, "zeta")}));
        expect(!bad.load_from(kimix::string_view(blob.data(), blob.size() - 3)));
        expect(eq(bad.turn_count(), 0u));
        expect(bad.get_by_id(9) == nullptr);
    };

    "incremental_append_after_search_no_rebuild"_test = [] {
        HistoryIndex h;
        h.append_turns(kimix::span<const turn_meta>({
            make_turn(0, "alpha beta gamma"),
            make_turn(1, "delta epsilon"),
        }));
        auto before = h.search("alpha", 3);
        expect(eq(before.size(), 1u));
        expect(eq(before[0].turn_id, 0u));

        // Append AFTER a search finalized the index — must be O(text), no
        // rebuild, and the new turn must be searchable.
        h.append_turns(kimix::span<const turn_meta>({
            make_turn(2, "alpha omega"),
        }));
        expect(eq(h.turn_count(), 3u));
        auto after = h.search("alpha", 3);
        expect(eq(after.size(), 2u));
        expect(eq(after[0].turn_id, 2u)); // alpha tf 1 in a 2-token doc
        expect(eq(after[1].turn_id, 0u));

        // tokenize counter: 3 appends + 2 queries.
        expect(eq(h.tokenize_call_count(), 5u));
    };

    // ---------------------------------------------------------------------
    // Benchmarks: production-sized conversation-history retrieval — 10k turn
    // appends (500 kept, eviction), 1k searches over a 10k-doc index, and
    // save/load round-trips. Every measured iteration validates invariants
    // (turn_count / MAX_TURNS bound, result limits, round-trip metadata).
    // ---------------------------------------------------------------------

    "bench_hist_append_10k_turns"_test = [] {
        constexpr uint32_t kTurns = 10000;
        constexpr size_t kWords = 100;   // ~100 tokens, ~500 B of text per turn
        constexpr uint32_t kVocab = 400;
        Rng rng(0x417D00u);
        kimix::vector<turn_meta> turns;
        turns.reserve(kTurns);
        for (uint32_t i = 0; i < kTurns; ++i) {
            turns.push_back(make_turn(i, make_text(rng, kWords, kVocab)));
        }
        HistoryIndex h;
        uint64_t seen = 0;
        kimix_bench::run("hist/append_10k_turns", [&] {
            h.reset();
            h.append_turns(turns);
            expect(eq(h.turn_count(), HistoryIndex::MAX_TURNS));
            expect(eq(h.tokenize_call_count(), static_cast<uint64_t>(kTurns)));
            seen = h.turn_count();
        }, kTurns, static_cast<double>(kTurns) * 500.0);
        expect(eq(seen, HistoryIndex::MAX_TURNS));
        // Evicted turns are gone from the deque; a query still returns only
        // live turns.
        auto r = h.search("t1 t2 t3", 5);
        expect(r.size() <= 5u);
        kimix_bench::sink(seen);
    };

    "bench_hist_search_1k_queries"_test = [] {
        constexpr uint32_t kTurns = 10000;
        constexpr uint32_t kWords = 100;
        constexpr uint32_t kVocab = 400;
        constexpr uint32_t kQueries = 1000;
        Rng rng(0x53E4A2u);
        kimix::vector<turn_meta> turns;
        turns.reserve(kTurns);
        for (uint32_t i = 0; i < kTurns; ++i) {
            turns.push_back(make_turn(i, make_text(rng, kWords, kVocab)));
        }
        HistoryIndex h;
        h.append_turns(turns); // 500 live turns; 10k docs in the index

        kimix::vector<kimix::string> queries;
        queries.reserve(kQueries);
        Rng qrng(0xD11337u);
        for (uint32_t q = 0; q < kQueries; ++q) {
            queries.push_back(make_text(qrng, 3, kVocab));
        }
        size_t res_total = 0;
        kimix_bench::run("hist/search_1k_queries", [&] {
            size_t acc = 0;
            for (const auto& q : queries) {
                auto r = h.search(q, 5);
                expect(r.size() <= 5u);
                acc += r.size();
            }
            res_total = acc;
        }, kQueries);
        expect(res_total > 0) << "queries never matched (bad corpus?)";
        kimix_bench::sink(res_total);
    };

    "bench_hist_save_load_roundtrip"_test = [] {
        constexpr uint32_t kTurns = 10000;
        constexpr uint32_t kWords = 100;
        constexpr uint32_t kVocab = 400;
        Rng rng(0x5A7E11u);
        kimix::vector<turn_meta> turns;
        turns.reserve(kTurns);
        for (uint32_t i = 0; i < kTurns; ++i) {
            turns.push_back(make_turn(i, make_text(rng, kWords, kVocab),
                                      static_cast<uint8_t>(i % 3), i % 5 == 0,
                                      1000.0 + i * 1.5));
        }
        HistoryIndex h;
        h.append_turns(turns);
        kimix::string blob;
        h.save_to(blob);
        expect(blob.size() > 4u * 1024u * 1024u) << "blob too small: "
                                                 << blob.size();
        // Byte-identity + metadata once, outside the timed loop.
        HistoryIndex verify;
        expect(verify.load_from(blob));
        expect(eq(verify.turn_count(), HistoryIndex::MAX_TURNS));
        kimix::string blob2;
        verify.save_to(blob2);
        expect(eq(blob, blob2));

        uint32_t seen = 0;
        kimix_bench::run("hist/save_load_roundtrip", [&] {
            kimix::string b;
            h.save_to(b);
            HistoryIndex loaded;
            expect(loaded.load_from(b));
            expect(eq(loaded.turn_count(), HistoryIndex::MAX_TURNS));
            expect(eq(b, blob));
            seen = loaded.turn_count();
        }, 1, static_cast<double>(blob.size()));
        expect(eq(seen, HistoryIndex::MAX_TURNS));
        kimix_bench::sink(seen);
    };
}
