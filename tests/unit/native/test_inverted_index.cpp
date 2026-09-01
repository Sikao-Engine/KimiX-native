// Test for src/runtime/index/inverted_index.h (plan 004).
// This test covers:
// - add/finalize/get_postings correctness (tf counts, doc_count, max_doc_id)
// - add-after-finalize (the headline regression: O(tokens), never a rebuild)
// - multi-segment k-way merge correctness
// - re-add of a delta-resident doc replaces postings
// - save/load round-trip byte-identical
// - 500-turn stress: finalize()+search between every append; results equal a
//   batch-built reference index; per-append cost stays flat (median-based)

#include "ut/ut.hpp"
#include "bench_util.h"
#include <runtime/index/inverted_index.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::index;

namespace {

kimix::vector<kimix::string> split_ws(kimix::string_view s) {
    kimix::vector<kimix::string> out;
    kimix::string cur;
    for (char c : s) {
        if (c == ' ') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}


// Convert owned strings to borrowed views for add_document.
kimix::vector<kimix::string_view> as_views(const kimix::vector<kimix::string>& strs) {
    kimix::vector<kimix::string_view> out;
    out.reserve(strs.size());
    for (const auto& s : strs) {
        out.emplace_back(s);
    }
    return out;
}

// Deterministic pseudo-random token stream (xorshift).
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

kimix::string make_tokens(Rng& rng, uint32_t count, uint32_t vocab) {
    kimix::string s;
    for (uint32_t i = 0; i < count; ++i) {
        if (i) {
            s.push_back(' ');
        }
        s += "t" + std::to_string(rng.bounded(vocab));
    }
    return s;
}

kimix::vector<std::pair<uint32_t, uint32_t>> sorted_postings(kimix::span<const postings_entry> pl) {
    kimix::vector<std::pair<uint32_t, uint32_t>> out;
    out.reserve(pl.size());
    for (const auto& e : pl) {
        out.emplace_back(e.doc_id, e.tf);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool postings_equal(kimix::span<const postings_entry> a,
                    kimix::span<const postings_entry> b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].doc_id != b[i].doc_id || a[i].tf != b[i].tf) {
            return false;
        }
    }
    return true;
}

// All distinct terms in a corpus of per-doc token strings.
// NOTE: the hash is std::hash-based (NOT kimix::hash64): referencing a
// kimix-core static symbol here would force the linker to open kimix-core.lib
// alongside runtime.dll's import lib and trip a mimalloc duplicate-symbol
// conflict (mi_malloc defined in both).
struct TestStringHash {
    using is_transparent = void;
    size_t operator()(kimix::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
    size_t operator()(const kimix::string& s) const noexcept {
        return std::hash<std::string_view>{}(kimix::string_view(s));
    }
};
struct TestStringEq {
    using is_transparent = void;
    bool operator()(kimix::string_view a, kimix::string_view b) const noexcept {
        return a == b;
    }
};
kimix::vector<kimix::string> corpus_terms(const kimix::vector<kimix::string>& docs) {
    kimix::unordered_set<kimix::string, TestStringHash, TestStringEq> terms;
    for (const auto& d : docs) {
        for (auto& t : split_ws(d)) {
            terms.insert(t);
        }
    }
    kimix::vector<kimix::string> out(terms.begin(), terms.end());
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "add_finalize_get_postings"_test = [] {
        InvertedIndex idx;
        expect(eq(idx.doc_count(), 0u));
        expect(eq(idx.max_doc_id(), 0u));
        expect(!idx.finalized());

        idx.add_document(0, as_views(split_ws("alpha beta alpha gamma")));
        idx.add_document(1, as_views(split_ws("beta gamma")));
        idx.add_document(4, as_views(split_ws("alpha delta"))); // sparse doc id

        expect(eq(idx.doc_count(), 3u));
        expect(eq(idx.max_doc_id(), 4u));       // Python N = 5
        expect(eq(idx.sum_doc_lengths(), 8ull)); // 4 + 2 + 2
        expect(eq(idx.avg_doc_len(), 8.0 / 3.0));

        // Pre-finalize get_postings works (Python finalizes on demand).
        {
            auto pl = idx.get_postings("alpha");
            expect(eq(pl.size(), 2u));
            const auto sp = sorted_postings(pl);
            expect(sp[0] == std::make_pair(0u, 2u));
            expect(sp[1] == std::make_pair(4u, 1u));
        }
        expect(idx.has_term("alpha"));
        expect(!idx.has_term("zzz"));

        idx.finalize();
        expect(idx.finalized());
        {
            auto pl = idx.get_postings("beta");
            const auto sp = sorted_postings(pl);
            expect(eq(sp.size(), 2u));
            expect(sp[0] == std::make_pair(0u, 1u));
            expect(sp[1] == std::make_pair(1u, 1u));
        }
        expect(idx.get_postings("zzz").empty());
        expect(eq(idx.total_postings(), 7u)); // alpha2 + beta2 + gamma2 + delta1
        expect(eq(idx.doc_length(0), 4u));
        expect(eq(idx.doc_length(4), 2u));
        expect(eq(idx.doc_length(99), 0u));
    };

    "add_after_finalize_core_regression"_test = [] {
        // The headline fix: adding documents after finalize() must be allowed
        // and O(tokens) — Python raises RuntimeError here.
        InvertedIndex idx;
        idx.add_document(0, as_views(split_ws("a b c")));
        idx.finalize();
        idx.add_document(1, as_views(split_ws("b c d")));
        idx.finalize();
        idx.add_document(2, as_views(split_ws("c d e")));

        expect(eq(idx.doc_count(), 3u));
        expect(idx.finalized());
        expect(eq(idx.total_postings(), 9u)); // 3 seg1 + 3 seg2 + 3 delta

        // Multi-segment merge: "c" appears in all three docs.
        auto pl = idx.get_postings("c");
        const auto sp = sorted_postings(pl);
        expect(eq(sp.size(), 3u));
        expect(sp[0] == std::make_pair(0u, 1u));
        expect(sp[1] == std::make_pair(1u, 1u));
        expect(sp[2] == std::make_pair(2u, 1u));

        // "a" only in segment 0 (delta empty after 2nd finalize).
        pl = idx.get_postings("a");
        expect(eq(pl.size(), 1u));
        expect(eq(pl[0].doc_id, 0u));

        // "e" only in the (unfinalized) delta.
        pl = idx.get_postings("e");
        expect(eq(pl.size(), 1u));
        expect(eq(pl[0].doc_id, 2u));
    };

    "delta_resident_readd_replaces"_test = [] {
        InvertedIndex idx;
        idx.add_document(0, as_views(split_ws("a b b"))); // a:1, b:2
        idx.add_document(0, as_views(split_ws("a c")));   // replace: a:1, c:1, no b
        expect(eq(idx.doc_count(), 1u));
        expect(eq(idx.sum_doc_lengths(), 2ull));
        auto pl = idx.get_postings("a");
        expect(eq(pl.size(), 1u));
        expect(eq(pl[0].tf, 1u));
        pl = idx.get_postings("b");
        expect(pl.empty());
        pl = idx.get_postings("c");
        expect(eq(pl.size(), 1u));
        idx.finalize();
        pl = idx.get_postings("a");
        expect(eq(pl.size(), 1u));
        expect(eq(pl[0].tf, 1u));
        expect(idx.get_postings("b").empty());
    };

    "multi_segment_merge_order"_test = [] {
        // Interleave adds/finalizes so terms live in several segments.
        InvertedIndex idx;
        idx.add_document(0, as_views(split_ws("x a")));
        idx.finalize();
        idx.add_document(1, as_views(split_ws("x b")));
        idx.finalize();
        idx.add_document(2, as_views(split_ws("x c")));
        idx.finalize();
        idx.add_document(3, as_views(split_ws("y d")));
        idx.finalize();
        auto pl = idx.get_postings("x");
        expect(eq(pl.size(), 3u));
        expect(eq(pl[0].doc_id, 0u));
        expect(eq(pl[1].doc_id, 1u));
        expect(eq(pl[2].doc_id, 2u));
        pl = idx.get_postings("y");
        expect(eq(pl.size(), 1u));
        expect(eq(pl[0].doc_id, 3u));
        expect(eq(idx.segment_count(), 4u));
        idx.compact();
        expect(eq(idx.segment_count(), 1u));
        pl = idx.get_postings("x");
        expect(eq(pl.size(), 3u));
        expect(eq(pl[0].doc_id, 0u));
        expect(eq(pl[2].doc_id, 2u));
    };

    "auto_compact_threshold"_test = [] {
        InvertedIndex idx;
        for (uint32_t i = 0; i < 20; ++i) {
            idx.add_document(i, as_views(split_ws("t" + std::to_string(i) + " shared")));
            idx.finalize();
        }
        // 20 flushes but the auto-compact keeps the count bounded.
        expect(idx.segment_count() <= InvertedIndex::kMaxSegmentsBeforeCompact);
        // All postings still correct after compaction.
        auto pl = idx.get_postings("shared");
        expect(eq(pl.size(), 20u));
        for (uint32_t i = 0; i < 20; ++i) {
            expect(eq(pl[i].doc_id, i));
        }
    };

    "save_load_roundtrip_byte_identical"_test = [] {
        InvertedIndex idx;
        idx.add_document(0, as_views(split_ws("a b b c")));
        idx.finalize();
        idx.add_document(3, as_views(split_ws("b c d")));
        idx.finalize();
        idx.add_document(7, as_views(split_ws("c d e e e")));
        idx.finalize();

        kimix::string blob;
        idx.save_to(blob);
        expect(blob.size() > 20u);

        InvertedIndex loaded;
        expect(loaded.load_from(blob));
        expect(loaded.finalized());
        expect(eq(loaded.doc_count(), 3u));
        expect(eq(loaded.max_doc_id(), 7u));
        expect(eq(loaded.sum_doc_lengths(), idx.sum_doc_lengths()));
        expect(eq(loaded.avg_doc_len(), idx.avg_doc_len()));

        // Every term's postings match.
        for (auto& t : {"a", "b", "c", "d", "e"}) {
            expect(postings_equal(idx.get_postings(t), loaded.get_postings(t)))
                << "term " << t;
        }
        // Re-save produces the identical blob.
        kimix::string blob2;
        loaded.save_to(blob2);
        expect(eq(blob, blob2));

        // Malformed input is rejected.
        InvertedIndex bad;
        expect(!bad.load_from("garbage"));
        expect(!bad.load_from(kimix::string_view(blob.data(), blob.size() - 1)));
        expect(eq(bad.doc_count(), 0u));
    };

    "reset_clears_everything"_test = [] {
        InvertedIndex idx;
        idx.add_document(0, as_views(split_ws("a b")));
        idx.finalize();
        idx.add_document(1, as_views(split_ws("c")));
        expect(eq(idx.doc_count(), 2u));
        idx.reset();
        expect(eq(idx.doc_count(), 0u));
        expect(eq(idx.total_postings(), 0u));
        expect(!idx.finalized());
        expect(idx.get_postings("a").empty());
    };

    "stress_500_turns_flat_append_and_correct"_test = [] {
        // Plan 004 step 3: the Python pathological pattern — finalize() +
        // search between EVERY append, 500 turns. Assertions:
        //  (1) per-append cost stays flat (median of last 100 adds vs first
        //      100 adds; a full-rebuild-per-append implementation grows
        //      ~linearly and would blow past the 5x bound);
        //  (2) results equal a batch-built reference index (all docs added
        //      before a single finalize — the "full rebuild" oracle).

        constexpr uint32_t kTurns = 500;
        constexpr uint32_t kTokensPerTurn = 40;
        constexpr uint32_t kVocab = 400;

        Rng rng(0xC0FFEEu);
        kimix::vector<kimix::string> docs;
        docs.reserve(kTurns);
        for (uint32_t i = 0; i < kTurns; ++i) {
            docs.push_back(make_tokens(rng, kTokensPerTurn, kVocab));
        }

        // Reference index: batch build (add everything, finalize once).
        InvertedIndex reference;
        for (uint32_t i = 0; i < kTurns; ++i) {
            reference.add_document(i, as_views(split_ws(docs[i])));
        }
        reference.finalize();

        // Stress index: finalize + search between every append.
        InvertedIndex stress;
        kimix::vector<double> add_ms;
        add_ms.reserve(kTurns);
        auto clock = std::chrono::steady_clock::now;
        for (uint32_t i = 0; i < kTurns; ++i) {
            auto t0 = clock();
            stress.add_document(i, as_views(split_ws(docs[i])));
            auto t1 = clock();
            stress.finalize();
            // A search between every append (the Python auto-retrieve path):
            // a few get_postings calls over merged segments.
            (void)stress.get_postings("t" + std::to_string(i % kVocab));
            (void)stress.get_postings("t" + std::to_string((i * 7 + 3) % kVocab));
            add_ms.push_back(
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        }

        // Correctness vs the full rebuild.
        expect(eq(stress.doc_count(), kTurns));
        const auto terms = corpus_terms(docs);
        for (const auto& term : terms) {
            auto a = stress.get_postings(term);
            auto b = reference.get_postings(term);
            expect(postings_equal(a, b)) << "term " << term;
        }
        expect(eq(stress.total_postings(), reference.total_postings()));

        // Flat per-append cost: median(last 100) < 5x median(first 100).
        auto median_of = [](const kimix::vector<double>& v, size_t from, size_t count) {
            kimix::vector<double> slice;
            slice.reserve(count);
            for (size_t i = from; i < from + count && i < v.size(); ++i) {
                slice.push_back(v[i]);
            }
            std::sort(slice.begin(), slice.end());
            return slice[slice.size() / 2];
        };
        const double first_med = median_of(add_ms, 0, 100);
        const double last_med = median_of(add_ms, kTurns - 100, 100);
        expect(first_med > 0.0) << "timer resolution";
        expect(last_med < first_med * 5.0)
            << "append cost grew: first_med=" << first_med
            << " last_med=" << last_med;
    };

    // ---------------------------------------------------------------------
    // Benchmarks: production-sized retrieval index work (5k docs, 1k queries,
    // 500-small-finalize compaction storm, ~10 MB save/load round-trips).
    // Each case validates postings/segment invariants on EVERY measured
    // iteration so a broken path is never timed.
    // ---------------------------------------------------------------------

    "bench_add_5k_docs"_test = [] {
        constexpr uint32_t kDocs = 5000;
        constexpr uint32_t kTokens = 40;
        constexpr uint32_t kVocab = 400;
        Rng rng(0xADD5000u);
        kimix::vector<kimix::string> docs;
        docs.reserve(kDocs);
        for (uint32_t i = 0; i < kDocs; ++i) {
            docs.push_back(make_tokens(rng, kTokens, kVocab));
        }
        // Expected total postings = sum of per-doc DISTINCT terms.
        uint32_t expected_postings = 0;
        for (const auto& d : docs) {
            auto toks = split_ws(d);
            std::sort(toks.begin(), toks.end());
            const auto uniq = std::unique(toks.begin(), toks.end());
            expected_postings += static_cast<uint32_t>(uniq - toks.begin());
        }

        InvertedIndex idx;
        uint32_t seen = 0;
        kimix_bench::run("inv/add_5k_docs", [&] {
            idx.reset();
            for (uint32_t i = 0; i < kDocs; ++i) {
                idx.add_document(i, as_views(split_ws(docs[i])));
            }
            expect(eq(idx.doc_count(), kDocs));
            expect(eq(idx.total_postings(), expected_postings));
        }, kDocs);
        seen = idx.total_postings();
        expect(eq(seen, expected_postings));
        kimix_bench::sink(seen);
    };

    "bench_finalize_500_small"_test = [] {
        // Worst case: 500 tiny add+finalize cycles -> segment growth pushes
        // past kMaxSegmentsBeforeCompact repeatedly (compaction storm).
        constexpr uint32_t kFlushes = 500;
        Rng rng(0xF1A11u);
        kimix::vector<kimix::string> docs;
        docs.reserve(kFlushes);
        for (uint32_t i = 0; i < kFlushes; ++i) {
            docs.push_back(make_tokens(rng, 12, 80));
        }
        InvertedIndex idx;
        uint32_t seen = 0;
        kimix_bench::run("inv/finalize_500_small", [&] {
            idx.reset();
            for (uint32_t i = 0; i < kFlushes; ++i) {
                idx.add_document(i, as_views(split_ws(docs[i])));
                idx.finalize();
            }
            // Auto-compact must keep the segment count bounded.
            expect(idx.segment_count() <= InvertedIndex::kMaxSegmentsBeforeCompact);
            expect(eq(idx.doc_count(), kFlushes));
            expect(idx.finalized());
        }, kFlushes);
        seen = idx.segment_count();
        expect(seen <= InvertedIndex::kMaxSegmentsBeforeCompact);
        kimix_bench::sink(seen);
    };

    // Build a finalized index (single segment) and run 1k queries against
    // it — the zero-copy span path.
    "bench_get_postings_1k_queries_1seg"_test = [] {
        constexpr uint32_t kDocs = 5000;
        constexpr uint32_t kTokens = 40;
        constexpr uint32_t kVocab = 400;
        constexpr uint32_t kQueries = 1000;
        constexpr uint32_t kTermsPerQuery = 5;
        Rng rng(0xE5E6u);
        kimix::vector<kimix::string> docs;
        docs.reserve(kDocs);
        for (uint32_t i = 0; i < kDocs; ++i) {
            docs.push_back(make_tokens(rng, kTokens, kVocab));
        }
        InvertedIndex idx;
        for (uint32_t i = 0; i < kDocs; ++i) {
            idx.add_document(i, as_views(split_ws(docs[i])));
        }
        idx.finalize();
        expect(eq(idx.segment_count(), 1u));

        kimix::vector<kimix::string> queries;
        queries.reserve(kQueries * kTermsPerQuery);
        for (uint32_t q = 0; q < kQueries; ++q) {
            for (uint32_t t = 0; t < kTermsPerQuery; ++t) {
                queries.push_back(make_tokens(rng, 1, kVocab));
            }
        }
        uint64_t expected = 0;
        for (const auto& t : queries) {
            expected += idx.get_postings(t).size();
        }
        uint64_t seen = 0;
        kimix_bench::run("inv/get_postings_1k_queries_1seg", [&] {
            uint64_t acc = 0;
            for (const auto& t : queries) {
                auto pl = idx.get_postings(t); // span path: no copy
                uint32_t sum = 0;
                for (const auto& e : pl) {
                    sum += e.tf; // search-style scan of the postings
                }
                acc += pl.size();
                kimix_bench::sink(sum);
            }
            seen = acc;
        }, kQueries * kTermsPerQuery);
        expect(seen == expected) << "postings scanned diverged";
        kimix_bench::sink(seen);
    };

    // Same 1k queries against 8 segments + a live delta: every term goes
    // through the k-way merge/cache path.
    "bench_get_postings_1k_queries_8seg_delta"_test = [] {
        constexpr uint32_t kDocs = 5000;
        constexpr uint32_t kTokens = 40;
        constexpr uint32_t kVocab = 400;
        constexpr uint32_t kQueries = 1000;
        constexpr uint32_t kTermsPerQuery = 5;
        constexpr uint32_t kSegments = 8;
        Rng rng(0x8E6D3u);
        kimix::vector<kimix::string> docs;
        docs.reserve(kDocs + 1);
        for (uint32_t i = 0; i < kDocs; ++i) {
            docs.push_back(make_tokens(rng, kTokens, kVocab));
        }
        InvertedIndex idx;
        const uint32_t per = kDocs / kSegments; // 625
        for (uint32_t b = 0; b < kSegments; ++b) {
            const uint32_t begin = b * per;
            const uint32_t end = (b == kSegments - 1) ? kDocs : begin + per;
            for (uint32_t i = begin; i < end; ++i) {
                idx.add_document(i, as_views(split_ws(docs[i])));
            }
            idx.finalize();
        }
        expect(eq(idx.segment_count(), kSegments));
        // One unfinalized doc -> live delta -> merged path.
        idx.add_document(kDocs, as_views(split_ws(make_tokens(rng, 40, kVocab))));

        kimix::vector<kimix::string> queries;
        queries.reserve(kQueries * kTermsPerQuery);
        for (uint32_t q = 0; q < kQueries; ++q) {
            for (uint32_t t = 0; t < kTermsPerQuery; ++t) {
                queries.push_back(make_tokens(rng, 1, kVocab));
            }
        }
        uint64_t expected = 0;
        for (const auto& t : queries) {
            expected += idx.get_postings(t).size();
        }
        uint64_t seen = 0;
        kimix_bench::run("inv/get_postings_1k_queries_8seg_delta", [&] {
            uint64_t acc = 0;
            for (const auto& t : queries) {
                auto pl = idx.get_postings(t); // merge path (cached after 1st)
                uint32_t sum = 0;
                for (const auto& e : pl) {
                    sum += e.tf;
                }
                acc += pl.size();
                kimix_bench::sink(sum);
            }
            seen = acc;
        }, kQueries * kTermsPerQuery);
        expect(seen == expected);
        kimix_bench::sink(seen);
    };

    "bench_save_load_10mb"_test = [] {
        // ~10 MB KNIDX1 blob: 5k docs x 250 tokens over a 50k vocabulary.
        constexpr uint32_t kDocs = 5000;
        constexpr uint32_t kTokens = 250;
        constexpr uint32_t kVocab = 50000;
        Rng rng(0x5A7E11u);
        kimix::vector<kimix::string> docs;
        docs.reserve(kDocs);
        for (uint32_t i = 0; i < kDocs; ++i) {
            docs.push_back(make_tokens(rng, kTokens, kVocab));
        }
        InvertedIndex idx;
        for (uint32_t i = 0; i < kDocs; ++i) {
            idx.add_document(i, as_views(split_ws(docs[i])));
        }
        idx.finalize();

        kimix::string blob;
        idx.save_to(blob);
        expect(blob.size() > 6u * 1024u * 1024u) << "blob too small: "
                                                 << blob.size();
        // Byte-identity check once, outside the timed loop.
        InvertedIndex verify;
        expect(verify.load_from(blob));
        expect(eq(verify.doc_count(), kDocs));
        kimix::string blob2;
        verify.save_to(blob2);
        expect(eq(blob, blob2));

        uint32_t seen = 0;
        kimix_bench::run("inv/save_load_10mb", [&] {
            kimix::string b;
            idx.save_to(b);
            InvertedIndex loaded;
            expect(loaded.load_from(b));
            expect(eq(loaded.doc_count(), kDocs));
            expect(eq(b, blob)); // deterministic: byte-identical every time
            seen = loaded.doc_count();
        }, 1, static_cast<double>(blob.size()));
        expect(eq(seen, kDocs));
        kimix_bench::sink(seen);
    };
}
