// Test for src/runtime/tools/compress.h (plan 016 micro-compression kernels).
//
// Covers byte-identical behavior of the native mirrors against the Python
// reference functions in kimi-cli/src/kimi_cli/tools/file/micro_compress.py.

#include "ut/ut.hpp"
#include <runtime/tools/compress.h>

#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::tools;

namespace {
kimix::string_view sv(const std::string& s) { return kimix::string_view(s); }

std::string repeat(const std::string& unit, size_t count) {
    std::string out;
    out.reserve(unit.size() * count);
    for (size_t i = 0; i < count; ++i) {
        out += unit;
    }
    return out;
}
} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "intra_line_dedup_no_change_short_lines"_test = [] {
        expect(eq(compress_intra_line_dedup(sv("hello"), 10, 100), sv("hello")));
        expect(eq(compress_intra_line_dedup(sv("abcabc"), 10, 100), sv("abcabc")));
    };

    "intra_line_dedup_exact_repetition"_test = [] {
        // 3000 = "abc" * 1000; default threshold=2000, max_unit=2048
        std::string line = repeat("abc", 1000);
        std::string expected = "abc ×1000 [+2997 chars elided]";
        expect(eq(compress_intra_line_dedup(sv(line), 2000, 2048), sv(expected)));
    };

    "intra_line_dedup_threshold_gate"_test = [] {
        std::string line = repeat("ab", 100); // length 200
        expect(eq(compress_intra_line_dedup(sv(line), 200, 2048), sv(line)));
        expect(eq(compress_intra_line_dedup(sv(line), 199, 2048), sv("ab ×100 [+198 chars elided]")));
    };

    "intra_line_dedup_max_unit_cap"_test = [] {
        // unit would be 4 but max_unit=2 prevents it
        std::string line = repeat("abcd", 500); // length 2000
        expect(eq(compress_intra_line_dedup(sv(line), 1000, 2), sv(line)));
        // unit 2 is allowed
        std::string line2 = repeat("ab", 1000); // length 2000
        expect(eq(compress_intra_line_dedup(sv(line2), 1000, 2),
                  sv("ab ×1000 [+1998 chars elided]")));
    };

    "intra_line_dedup_no_false_positives"_test = [] {
        std::string line = repeat("abc", 5) + "X" + repeat("abc", 5); // length 31
        expect(eq(compress_intra_line_dedup(sv(line), 10, 100), sv(line)));
    };

    "intra_line_dedup_marker_not_beneficial"_test = [] {
        // "abcabc" unit 3, repeats 2 -> marker would be longer than original
        expect(eq(compress_intra_line_dedup(sv("abcabc"), 1, 2048), sv("abcabc")));
    };

    "intra_line_dedup_multiline_partial_change"_test = [] {
        std::string long_line = repeat("xy", 1500); // length 3000
        std::string input = "short\n" + long_line + "\nalso short\n";
        std::string expected = "short\nxy ×1500 [+2998 chars elided]\nalso short\n";
        expect(eq(compress_intra_line_dedup(sv(input), 2000, 2048), sv(expected)));
    };

    "intra_line_dedup_preserves_empty_and_newlines"_test = [] {
        expect(eq(compress_intra_line_dedup(sv(""), 10, 100), sv("")));
        expect(eq(compress_intra_line_dedup(sv("\n\n"), 10, 100), sv("\n\n")));
    };

    "collapse_whitespace_trailing_strip"_test = [] {
        // Trailing space is stripped; internal spaces/tabs are untouched.
        expect(eq(compress_collapse_whitespace(sv("hello   \tworld \n"), sv("log"), false, true, 1, false, false),
                  sv("hello   \tworld\n")));
    };

    "collapse_whitespace_code_kind_keeps_tabs"_test = [] {
        // Code kind only strips trailing spaces; tabs are preserved.
        expect(eq(compress_collapse_whitespace(sv("hello\t\nworld   \n"), sv("code"), false, true, 1, false, false),
                  sv("hello\t\nworld\n")));
    };

    "collapse_whitespace_blank_collapse"_test = [] {
        expect(eq(compress_collapse_whitespace(sv("a\n\n\n\nb\n"), sv("log"), false, true, 1, false, false),
                  sv("a\n\nb\n")));
    };

    "collapse_whitespace_blank_collapse_zero"_test = [] {
        // max_blanks=0 removes all blank lines, including the trailing empty
        // line produced by the final '\n'.
        expect(eq(compress_collapse_whitespace(sv("a\n\n\nb\n"), sv("log"), false, true, 0, false, false),
                  sv("a\nb")));
    };

    "collapse_whitespace_common_indent"_test = [] {
        std::string input = "    line one\n    line two\n    line three\n";
        std::string expected = "[common-indent: 4 cols removed]\nline one\nline two\nline three\n";
        expect(eq(compress_collapse_whitespace(sv(input), sv("log"), false, true, 1, true, false),
                  sv(expected)));
    };

    "collapse_whitespace_common_indent_code_disabled"_test = [] {
        std::string input = "    line one\n    line two\n";
        expect(eq(compress_collapse_whitespace(sv(input), sv("code"), false, true, 1, true, false),
                  sv(input)));
    };

    "collapse_whitespace_common_indent_lossless_only"_test = [] {
        std::string input = "    line one\n    line two\n";
        expect(eq(compress_collapse_whitespace(sv(input), sv("log"), true, true, 1, true, false),
                  sv(input)));
    };

    "collapse_whitespace_internal_spaces"_test = [] {
        expect(eq(compress_collapse_whitespace(sv("a   b     c"), sv("log"), false, true, 1, false, false),
                  sv("a b c")));
        // leading / trailing runs are NOT internal
        expect(eq(compress_collapse_whitespace(sv("   a   b   "), sv("log"), false, true, 1, false, false),
                  sv("   a b")));
    };

    "collapse_whitespace_internal_spaces_disabled_for_data"_test = [] {
        expect(eq(compress_collapse_whitespace(sv("a   b     c"), sv("data"), false, true, 1, false, false),
                  sv("a   b     c")));
    };

    "collapse_whitespace_no_op"_test = [] {
        std::string input = "already\nclean\n";
        expect(eq(compress_collapse_whitespace(sv(input), sv("log"), false, true, 1, false, false),
                  sv(input)));
    };

    "collapse_whitespace_empty_input"_test = [] {
        expect(eq(compress_collapse_whitespace(sv(""), sv("log"), false, true, 1, true, true), sv("")));
    };

    "renumber_lines_normal"_test = [] {
        expect(eq(compress_renumber_lines(sv("  1\ta\n  2\tb\n  3\tc\n")),
                  sv("1\ta\n2\tb\n3\tc\n")));
    };

    "renumber_lines_mixed_no_op"_test = [] {
        std::string input = "  1\ta\nnot numbered\n  3\tc\n";
        expect(eq(compress_renumber_lines(sv(input)), sv(input)));
    };

    "renumber_lines_skips_blank_and_meta"_test = [] {
        expect(eq(compress_renumber_lines(sv("\n  1\ta\n[meta]\n  2\tb\n")),
                  sv("\n1\ta\n[meta]\n2\tb\n")));
    };

    "renumber_lines_ellipsis_meta"_test = [] {
        // U+2026 HORIZONTAL ELLIPSIS
        std::string input = "\n  1\ta\n\xE2\x80\xA6meta\n  2\tb\n";
        std::string expected = "\n1\ta\n\xE2\x80\xA6meta\n2\tb\n";
        expect(eq(compress_renumber_lines(sv(input)), sv(expected)));
    };

    "renumber_lines_already_compact"_test = [] {
        std::string input = "1\ta\n2\tb\n";
        expect(eq(compress_renumber_lines(sv(input)), sv(input)));
    };

    "renumber_lines_no_substantial_lines"_test = [] {
        std::string input = "\n\n[meta]\n";
        expect(eq(compress_renumber_lines(sv(input)), sv(input)));
    };

    "strip_control_noise_ansi_csi"_test = [] {
        std::string input = std::string(1, '\x1B') + "[31mred" + std::string(1, '\x1B') + "[0m";
        expect(eq(compress_strip_control_noise(sv(input)), sv("red")));
    };

    "strip_control_noise_ansi_osc"_test = [] {
        std::string input = std::string(1, '\x1B') + "]0;title" + std::string(1, '\x07') + "after";
        expect(eq(compress_strip_control_noise(sv(input)), sv("after")));
    };

    "strip_control_noise_cr_progress"_test = [] {
        expect(eq(compress_strip_control_noise(sv("frame1\rframe2\n")), sv("frame2\n")));
        expect(eq(compress_strip_control_noise(sv("a\rb\rc")), sv("c")));
    };

    "strip_control_noise_no_op"_test = [] {
        std::string input = "clean text\nno escapes\n";
        expect(eq(compress_strip_control_noise(sv(input)), sv(input)));
    };

    "strip_control_noise_crlf"_test = [] {
        // CR immediately before LF is the last CR in the line, so the segment
        // after it is empty (mirrors the Python rsplit("\r", 1)[-1] behavior).
        expect(eq(compress_strip_control_noise(sv("line1\r\nline2\r\n")), sv("\n\n")));
    };
}
