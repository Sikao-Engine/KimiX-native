// Test for src/runtime/tools/find_str.h (plan 013 find_in_file).
// This test covers:
// - per-line overlapping matches (readlines() terminator semantics)
// - case-insensitive (ASCII fold) and case-sensitive search
// - needle at line boundaries / spanning the terminator
// - empty needle contract (len+1 matches per line)
// - 0-based line_index / col output

#include "ut/ut.hpp"
#include <runtime/tools/find_str.h>

#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::tools;

namespace {
kimix::string_view sv(const std::string& s) { return kimix::string_view(s); }
} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "find_ci_overlapping"_test = [] {
        const std::string content = "ab\nxab\nABC\nzz\n";
        kimix::vector<find_match> out;
        find_in_file(sv(content), sv("ab"), true, out);
        expect(eq(out.size(), 3u));
        expect(eq(out[0].line_index, 0u));
        expect(eq(out[0].col, 0u));
        expect(eq(out[1].line_index, 1u));
        expect(eq(out[1].col, 1u));
        expect(eq(out[2].line_index, 2u)); // "ABC" lowercased contains "ab"
        expect(eq(out[2].col, 0u));
        // length is the needle byte length
        expect(eq(out[0].length, 2u));
    };

    "find_cs"_test = [] {
        const std::string content = "ab\nxab\nABC\nzz\n";
        kimix::vector<find_match> out;
        find_in_file(sv(content), sv("ab"), false, out);
        expect(eq(out.size(), 2u));
        expect(eq(out[0].line_index, 0u));
        expect(eq(out[1].line_index, 1u));
    };

    "find_overlapping_occurrences"_test = [] {
        // "aaa" contains "aa" at cols 0 and 1 (overlapping)
        const std::string content = "aaa\n";
        kimix::vector<find_match> out;
        find_in_file(sv(content), sv("aa"), true, out);
        expect(eq(out.size(), 2u));
        expect(eq(out[0].col, 0u));
        expect(eq(out[1].col, 1u));
        // multiple per line
        const std::string c2 = "abab\n";
        out.clear();
        find_in_file(sv(c2), sv("ab"), true, out);
        expect(eq(out.size(), 2u));
        expect(eq(out[0].col, 0u));
        expect(eq(out[1].col, 2u));
    };

    "find_terminator_semantics"_test = [] {
        // lines keep their terminator: a needle ending in "\n" matches at
        // the end of a line (reference quirk)
        const std::string content = "ab\n";
        kimix::vector<find_match> out;
        find_in_file(sv(content), sv("b\n"), true, out);
        expect(eq(out.size(), 1u));
        expect(eq(out[0].col, 1u));
        // no trailing newline on the last line
        const std::string c2 = "a\nb";
        out.clear();
        find_in_file(sv(c2), sv("b"), true, out);
        expect(eq(out.size(), 1u));
        expect(eq(out[0].line_index, 1u));
        // empty content -> no lines -> no matches
        out.clear();
        find_in_file(sv(""), sv("x"), true, out);
        expect(out.empty());
    };

    "find_empty_needle"_test = [] {
        // reference find("") semantics: matches at every position 0..len
        // inclusive -> len+1 matches per line
        const std::string content = "a\n";
        kimix::vector<find_match> out;
        find_in_file(sv(content), sv(""), true, out);
        expect(eq(out.size(), 3u)); // line "a\n" has length 2 -> 3 matches
        expect(eq(out[0].col, 0u));
        expect(eq(out[1].col, 1u));
        expect(eq(out[2].col, 2u));
        expect(eq(out[0].length, 0u));
    };

    "find_case_insensitive_ascii_only"_test = [] {
        // ASCII folding covers A-Z only (non-ASCII routed to _compat by shim)
        const std::string content = "HELLO world\n";
        kimix::vector<find_match> out;
        find_in_file(sv(content), sv("hello"), true, out);
        expect(eq(out.size(), 1u));
        expect(eq(out[0].col, 0u));
    };
}
