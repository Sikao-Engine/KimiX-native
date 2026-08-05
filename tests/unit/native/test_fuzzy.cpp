// Test for src/runtime/search/fuzzy.h (plan 005).
// This test covers:
// - add_term/expand: candidate walk (deletes index + DL/freq/prefix gates)
// - deterministic ordering (score desc, term asc)
// - max_expansions cap
// - cache behavior: repeat query is cached; add_term invalidates it
// - term_count/has_term/reset

#include "ut/ut.hpp"
#include <runtime/search/fuzzy.h>

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
}
