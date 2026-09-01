// Test for src/runtime/search/fuzzy.h (plan 005).
// This test covers:
// - add_term/expand: candidate walk (deletes index + DL/freq/prefix gates)
// - deterministic ordering (score desc, term asc)
// - max_expansions cap
// - cache behavior: repeat query is cached; add_term invalidates it
// - term_count/has_term/reset

#include "ut/ut.hpp"
#include "bench_util.h"
#include <runtime/search/fuzzy.h>

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::search;

namespace {

kimix::vector<kimix::string> terms_of(const kimix::vector<fuzzy_candidate>& c) {
    kimix::vector<kimix::string> out;
    out.reserve(c.size());
    for (const auto& fc : c) {
        out.push_back(fc.term);
    }
    return out;
}

bool contains(const kimix::vector<fuzzy_candidate>& c, const char* term) {
    for (const auto& fc : c) {
        if (fc.term == term) {
            return true;
        }
    }
    return false;
}

// Tool/command-like vocabulary generator used by the benchmarks.
const char* g_syllables[] = {
    "pre", "re", "in", "ex", "con", "dis", "sta", "tor", "scan", "grep",
    "seek", "find", "sort", "list", "open", "read", "write", "file", "path",
    "code", "view", "edit", "split", "join", "trim", "unit", "test", "bench",
    "init", "sync", "load", "dump", "meta", "core", "proc", "exec", "parse",
    "lex", "tok", "fuzz", "hash", "idx", "seg", "buf", "line", "sess", "auth",
    "ctrl", "stat", "fill", "draw", "info", "log",
};
const size_t g_syllable_count = sizeof(g_syllables) / sizeof(g_syllables[0]);

kimix::string make_fuzzy_word(std::mt19937& rng) {
    kimix::string w;
    w.append(g_syllables[rng() % g_syllable_count]);
    w.append(g_syllables[rng() % g_syllable_count]);
    w.append(g_syllables[rng() % g_syllable_count]);
    char buf[8];
    const int n = std::snprintf(buf, sizeof(buf), "%03u",
                                static_cast<unsigned>(rng() % 1000));
    w.append(buf, static_cast<size_t>(n));
    return w;
}

// 1-2 edit near-miss of w that keeps the first character (the kernel's
// prefix_length == 1 gate requires it) — typical typo / close variant.
kimix::string near_miss(std::mt19937& rng, const kimix::string& w) {
    kimix::string q = w;
    if (q.size() < 3) {
        q.push_back('x');
        return q;
    }
    switch (rng() % 4) {
    case 0: // delete one middle char
        q.erase(1 + rng() % (q.size() - 1), 1);
        break;
    case 1: // substitute one middle char
        {
            const size_t p = 1 + rng() % (q.size() - 1);
            char nv = static_cast<char>('a' + rng() % 26);
            if (nv == q[p]) {
                nv = nv == 'z' ? 'a' : static_cast<char>(nv + 1);
            }
            q[p] = nv;
        }
        break;
    case 2: // insert one middle char
        q.insert(1 + rng() % (q.size() - 1), 1,
                 static_cast<char>('a' + rng() % 26));
        break;
    default: // adjacent swap (one transposition)
        {
            const size_t p = 1 + rng() % (q.size() - 2);
            std::swap(q[p], q[p + 1]);
        }
        break;
    }
    if (q == w) {
        q.push_back('x');
    }
    return q;
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "expand_basic"_test = [] {
        SymmetricDeleteIndex sd;
        sd.add_term("hello");
        sd.add_term("help");
        sd.add_term("hell");
        sd.add_term("world");
        sd.add_term("held");

        expect(eq(sd.term_count(), 5u));
        expect(sd.has_term("hello"));
        expect(!sd.has_term("hellx"));

        kimix::vector<fuzzy_candidate> out;
        sd.expand("hello", 2, out);
        // Exact match is included (reference adds pattern itself).
        expect(contains(out, "hello")) << "exact term must be a candidate";
        // "help" is within 1 edit (substitute l->p) of "hello".
        expect(contains(out, "help"));
        expect(contains(out, "hell"));   // delete 'o'
        expect(contains(out, "held"));   // h e l d vs h e l l o: 2 edits
        expect(!contains(out, "world")); // way out of range
        // Deterministic order: exact match first (score 1.0).
        if (!out.empty()) {
            expect(eq(out[0].term, kimix::string("hello")));
            expect(eq(out[0].score, 1.0));
        }
        // Scores are 1/(1+dl): "hell" (dl=1) -> 0.5.
        for (const auto& fc : out) {
            if (fc.term == "hell") {
                expect(std::abs(fc.score - 0.5) < 1e-12);
            }
        }
    };

    "expand_max_expansions_cap"_test = [] {
        SymmetricDeleteIndex sd;
        // 20 terms all within 2 edits of "abc..."-style pattern.
        for (int i = 0; i < 20; ++i) {
            sd.add_term("ab" + std::to_string(i));
        }
        kimix::vector<fuzzy_candidate> out;
        sd.expand("ab0", 2, out, 5);
        expect(eq(out.size(), 5u)) << "max_expansions cap";
        out.clear();
        sd.expand("ab0", 2, out, 50);
        expect(le(out.size(), 20u));
    };

    "expand_cache_and_invalidation"_test = [] {
        SymmetricDeleteIndex sd;
        sd.add_term("cat");
        kimix::vector<fuzzy_candidate> out1, out2;
        sd.expand("cat", 1, out1);
        // Second identical query hits the cache (same content).
        sd.expand("cat", 1, out2);
        expect(eq(out1.size(), out2.size()));
        expect(!out1.empty());
        // add_term invalidates the cache.
        sd.add_term("cut"); // now within 1 edit of "cat"
        kimix::vector<fuzzy_candidate> out3;
        sd.expand("cat", 1, out3);
        expect(contains(out3, "cut")) << "cache must be invalidated by add_term";
    };

    "expand_prefix_gate"_test = [] {
        SymmetricDeleteIndex sd;
        sd.add_term("cat");
        sd.add_term("bat"); // same length, differs in first char -> gate rejects
        sd.add_term("cot"); // first char matches
        kimix::vector<fuzzy_candidate> out;
        sd.expand("cat", 1, out);
        expect(contains(out, "cat"));
        expect(contains(out, "cot"));
        expect(!contains(out, "bat")) << "first-char prefix gate (prefix_length=1)";
    };

    "expand_max_edits_zero"_test = [] {
        SymmetricDeleteIndex sd;
        sd.add_term("cat");
        kimix::vector<fuzzy_candidate> out;
        sd.expand("cat", 0, out);
        expect(eq(out.size(), 1u));
        expect(eq(out[0].term, kimix::string("cat")));
        out.clear();
        sd.expand("car", 0, out);
        expect(out.empty()) << "max_edits=0 returns the exact term only";
    };

    "reset"_test = [] {
        SymmetricDeleteIndex sd;
        sd.add_term("cat");
        expect(eq(sd.term_count(), 1u));
        sd.reset();
        expect(eq(sd.term_count(), 0u));
        expect(!sd.has_term("cat"));
        kimix::vector<fuzzy_candidate> out;
        sd.expand("cat", 1, out);
        expect(out.empty());
    };

    // --- benchmarks (see bench_util.h contract) ---
    // No hard timing assertions; expect() guards keep every measured path
    // correct (index membership + candidate sanity).

    "bench_fuzzy_build_10k"_test = [] {
        // Build the symmetric-delete index from 10k tool/command-like names.
        std::mt19937 rng(2025u);
        SymmetricDeleteIndex::StringSet seen_set;
        kimix::vector<kimix::string> words;
        words.reserve(10000);
        while (words.size() < 10000) {
            kimix::string w = make_fuzzy_word(rng);
            if (seen_set.insert(w).second) {
                words.push_back(std::move(w));
            }
        }
        // Correctness: full build + membership spot checks.
        SymmetricDeleteIndex sd;
        for (const auto& w : words) {
            sd.add_term(w);
        }
        expect(eq(sd.term_count(), size_t(10000)));
        expect(sd.has_term(words[0]));
        expect(sd.has_term(words[9999]));
        kimix::vector<fuzzy_candidate> out;
        sd.expand(words[0], 2, out);
        expect(contains(out, words[0].c_str()))
            << "exact term must be found after build";

        int64_t checksum = 0;
        kimix_bench::run("fuzzy/build_10k_words", [&] {
            SymmetricDeleteIndex idx;
            for (const auto& w : words) {
                idx.add_term(w);
            }
            checksum += static_cast<int64_t>(idx.term_count());
        }, 1);
        kimix_bench::sink(checksum);
    };

    "bench_fuzzy_expand_cold_10k"_test = [] {
        // 10k lookups, cache-cold: 5k exact terms + 5k near-misses (1-2 edits).
        std::mt19937 rng(4242u);
        kimix::vector<kimix::string> words;
        words.reserve(10000);
        SymmetricDeleteIndex sd;
        for (size_t i = 0; i < 10000; ++i) {
            words.push_back(make_fuzzy_word(rng));
            sd.add_term(words.back());
        }
        kimix::vector<kimix::string> queries;
        queries.reserve(10000);
        for (size_t i = 0; i < 5000; ++i) {
            queries.push_back(words[i]);         // exact
            queries.push_back(near_miss(rng, words[5000 + i])); // 1-2 edits
        }
        // Correctness: exact queries return the term first (score 1.0);
        // near-miss queries must still surface the base word.
        kimix::vector<fuzzy_candidate> out;
        for (size_t i = 0; i < queries.size(); i += 2) {
            sd.expand(queries[i], 2, out);
            expect(!out.empty());
            expect(eq(out[0].term, queries[i])) << "exact match ordered first";
        }
        for (size_t i = 1; i < queries.size(); i += 2) {
            sd.expand(queries[i], 2, out);
            expect(contains(out, words[5000 + i / 2].c_str()))
                << "near-miss must find the base term";
        }
        int64_t checksum = 0;
        kimix_bench::run("fuzzy/expand_10k_cold", [&] {
            for (const auto& q : queries) {
                sd.expand(q, 2, out, 50);
                checksum += static_cast<int64_t>(out.size());
            }
        }, queries.size());
        kimix_bench::sink(checksum);
    };

    "bench_fuzzy_expand_hot_200"_test = [] {
        // Cache-hot lookups: 200 repeated near-miss queries (LRU hits).
        std::mt19937 rng(777u);
        SymmetricDeleteIndex sd;
        kimix::vector<kimix::string> words;
        words.reserve(500);
        for (size_t i = 0; i < 500; ++i) {
            words.push_back(make_fuzzy_word(rng));
            sd.add_term(words.back());
        }
        kimix::vector<kimix::string> queries;
        queries.reserve(200);
        for (size_t i = 0; i < 200; ++i) {
            queries.push_back(near_miss(rng, words[i]));
        }
        kimix::vector<fuzzy_candidate> out;
        for (const auto& q : queries) {
            sd.expand(q, 2, out, 50);
            expect(!out.empty()) << "hot queries are near-misses";
        }
        int64_t checksum = 0;
        kimix_bench::run("fuzzy/expand_200_hot", [&] {
            for (const auto& q : queries) {
                sd.expand(q, 2, out, 50);
                checksum += static_cast<int64_t>(out.size());
            }
        }, queries.size());
        kimix_bench::sink(checksum);
    };
}
