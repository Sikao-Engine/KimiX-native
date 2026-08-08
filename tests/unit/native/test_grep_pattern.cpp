// Test for src/runtime/tools/grep_pattern.h (plan: commit 0582e09 "Study from
// hermes" -- kimi-cli grep_local.py newline kernels).
// This test covers:
// - pattern_has_regex_newline: literal newline + odd/even backslash runs
// - multiline_pattern: CRLF normalization, regex \n escape rewrite, real
//   newline rewrite (order-safe)

#include "ut/ut.hpp"
#include <runtime/tools/grep_pattern.h>

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

    "has_regex_newline"_test = [] {
        expect(!pattern_has_regex_newline(sv("abc")));
        expect(pattern_has_regex_newline(sv("a\nb")));           // literal
        expect(pattern_has_regex_newline(sv("a\\nb")));          // odd (1)
        expect(!pattern_has_regex_newline(sv("a\\\\nb")));       // even (2)
        expect(pattern_has_regex_newline(sv("a\\\\\\nb")));      // odd (3)
        expect(pattern_has_regex_newline(sv("a\nb\\nc")));
        expect(pattern_has_regex_newline(sv("\\n")));
        expect(!pattern_has_regex_newline(sv("\\\\n")));
        expect(pattern_has_regex_newline(sv("\\\\\\n")));
        expect(pattern_has_regex_newline(sv("x\\ny")));
        expect(pattern_has_regex_newline(sv("a\r\nb")));         // literal \n
    };

    "multiline_no_newline"_test = [] {
        expect((multiline_pattern(sv("abc")) == "abc"));
        // even backslashes: literal backslash+n search, unchanged
        expect((multiline_pattern(sv("a\\\\nb")) == "a\\\\nb"));
    };

    "multiline_real_newlines"_test = [] {
        expect((multiline_pattern(sv("a\nb")) == "a\\r?\\nb"));
        expect((multiline_pattern(sv("a\r\nb")) == "a\\r?\\nb"));
        expect((multiline_pattern(sv("a\nb\\nc")) == "a\\r?\\nb\\r?\\nc"));
    };

    "multiline_regex_escapes"_test = [] {
        expect((multiline_pattern(sv("a\\nb")) == "a\\r?\\nb"));
        expect((multiline_pattern(sv("\\n")) == "\\r?\\n"));
        expect((multiline_pattern(sv("\\\\\\n")) == "\\r?\\n"));
    };
}
