// Test for src/runtime/tools/compress.h (plan 016 micro-compression kernels).
//
// Covers byte-identical behavior of the native mirrors against the Python
// reference functions in kimi-cli/src/kimi_cli/tools/file/micro_compress.py.

#include "ut/ut.hpp"
#include "unit/native/bench_util.h"
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

size_t count_occ(std::string_view hay, std::string_view needle) {
    size_t n = 0;
    size_t pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string_view::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

// 10k-line log with `density_pct`% of lines being 2200-char periodic lines
// ("ab"*1100; unit "ab", repeats 1100, elided 2198) and the rest ~95-char
// realistic log lines.  Sized like a long tool-output compaction workload.
std::string make_periodic_log(size_t lines, size_t density_pct) {
    std::string out;
    out.reserve(lines * 400 + lines * 100 * density_pct / 100);
    for (size_t i = 0; i < lines; ++i) {
        if (i % 100 < density_pct) {
            for (size_t k = 0; k < 2200; ++k) {
                out += (k & 1) ? 'b' : 'a';
            }
            out += '\n';
        } else {
            out += "2024-05-01 12:00:00 INFO req=";
            out += std::to_string(i);
            out += " worker=node-7 latency=42ms path=/api/v1/items status=200 ok\n";
        }
    }
    return out;
}

// 10k unique non-periodic log lines (worst case conventionally cited for
// dedup maps; here it exercises the split + no-dedup fast path).
std::string make_distinct_log(size_t lines) {
    std::string out;
    out.reserve(lines * 110);
    for (size_t i = 0; i < lines; ++i) {
        out += "2024-05-01 12:00:00 INFO req=";
        out += std::to_string(i);
        out += " worker=node-";
        out += std::to_string(i % 97);
        out += " path=/api/v1/items/";
        out += std::to_string(i);
        out += " status=200 tok=";
        out += std::to_string(i * 2654435761ull % 1000003);
        out += '\n';
    }
    return out;
}

// Log with `density_pct`% of lines carrying modifiable whitespace
// (internal 3+ space runs, trailing spaces) plus a blank-line run every
// 50 lines.  Optionally 8-space common indent on every non-blank line.
std::string make_ws_log(size_t lines, size_t density_pct, bool common_indent) {
    std::string out;
    out.reserve(lines * 130);
    for (size_t i = 0; i < lines; ++i) {
        if (i % 50 == 0) {
            out += "\n\n"; // blank run of 2 -> collapses to 1 with max_blanks=1
            continue;
        }
        if (common_indent) {
            out.append(8, ' ');
        }
        out += "2024-05-01 12:00:00 INFO msg=hello";
        if (i % 100 < density_pct) {
            out += "   world status=200   \n"; // internal 3+ run + trailing
        } else {
            out += " world status=200\n";
        }
    }
    return out;
}

// 10k numbered lines (all match ^\s*\d+\t); renumber strips the 2 leading
// spaces per line, so output is exactly input.size() - 2*lines.
std::string make_numbered_log(size_t lines) {
    std::string out;
    out.reserve(lines * 64);
    for (size_t i = 1; i <= lines; ++i) {
        out += "  ";
        out += std::to_string(i);
        out += '\t';
        out += "detail text for line ";
        out += std::to_string(i);
        out += '\n';
    }
    return out;
}

// Log with `density_pct`% of lines containing ANSI CSI escapes plus a
// carriage-return progress chain every 50 lines.
std::string make_control_log(size_t lines, size_t density_pct) {
    std::string out;
    out.reserve(lines * 150);
    for (size_t i = 0; i < lines; ++i) {
        if (i % 50 == 0) {
            out += "prog a\rprog b\rprog c\n";
            continue;
        }
        if (i % 100 < density_pct) {
            out += "2024-05-01 12:00:00 INFO \x1B[31mERROR\x1B[0m req=";
            out += std::to_string(i);
            out += " detail=";
            out += std::to_string((i * 7) % 1000);
            out += '\n';
        } else {
            out += "2024-05-01 12:00:00 INFO req=";
            out += std::to_string(i);
            out += " clean\n";
        }
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

    // ------------------------------------------------------------------
    // Benchmarks (kimix_bench harness; "[bench] ..." lines go to stderr).
    // Workloads: 10k-line ~1MB logs; production tool-output compaction.
    // ------------------------------------------------------------------

    "bench_compress_intra_dedup_low_5pct"_test = [] {
        const std::string in = make_periodic_log(10000, 5);
        const kimix::string out = compress_intra_line_dedup(sv(in), 2000, 2048);
        expect(count_occ(out, "[+2198 chars elided]") == 500u);
        expect(out.size() < in.size());
        kimix_bench::run("compress/intra_dedup low 5%", [&] {
            kimix_bench::sink(compress_intra_line_dedup(sv(in), 2000, 2048));
        }, 1, double(in.size()));
    };

    "bench_compress_intra_dedup_high_50pct"_test = [] {
        const std::string in = make_periodic_log(10000, 50);
        const kimix::string out = compress_intra_line_dedup(sv(in), 2000, 2048);
        expect(count_occ(out, "[+2198 chars elided]") == 5000u);
        expect(out.size() < in.size());
        kimix_bench::run("compress/intra_dedup high 50%", [&] {
            kimix_bench::sink(compress_intra_line_dedup(sv(in), 2000, 2048));
        }, 1, double(in.size()));
    };

    "bench_compress_intra_dedup_distinct"_test = [] {
        const std::string in = make_distinct_log(10000);
        const kimix::string out = compress_intra_line_dedup(sv(in), 2000, 2048);
        expect(eq(kimix::string_view(out), kimix::string_view(in)));
        kimix_bench::run("compress/intra_dedup distinct", [&] {
            kimix_bench::sink(compress_intra_line_dedup(sv(in), 2000, 2048));
        }, 1, double(in.size()));
    };

    "bench_compress_intra_dedup_near_periodic_worst"_test = [] {
        // True scan worst case: every divisor of n=2880 is tried and every
        // full-length memcmp fails at the last byte ("ab"*1439 + "az").
        std::string in;
        in.reserve(2000 * 2880);
        for (size_t i = 0; i < 2000; ++i) {
            for (size_t k = 0; k < 1439; ++k) {
                in += (k & 1) ? 'b' : 'a';
            }
            in += "az\n";
        }
        const kimix::string out = compress_intra_line_dedup(sv(in), 2000, 2048);
        expect(eq(kimix::string_view(out), kimix::string_view(in)));
        kimix_bench::run("compress/intra_dedup near-periodic", [&] {
            kimix_bench::sink(compress_intra_line_dedup(sv(in), 2000, 2048));
        }, 1, double(in.size()));
    };

    "bench_compress_collapse_low_5pct"_test = [] {
        const std::string in = make_ws_log(10000, 5, false);
        const kimix::string out = compress_collapse_whitespace(
            sv(in), sv("log"), false, true, 1, false, false);
        expect(out.size() < in.size());
        expect(out.find("\n\n\n") == kimix::string::npos);
        expect(out.find("hello   world") == kimix::string::npos);
        kimix_bench::run("compress/collapse_ws low 5%", [&] {
            kimix_bench::sink(compress_collapse_whitespace(
                sv(in), sv("log"), false, true, 1, false, false));
        }, 1, double(in.size()));
    };

    "bench_compress_collapse_high_50pct"_test = [] {
        const std::string in = make_ws_log(10000, 50, false);
        const kimix::string out = compress_collapse_whitespace(
            sv(in), sv("log"), false, true, 1, false, false);
        expect(out.size() < in.size());
        expect(out.find("\n\n\n") == kimix::string::npos);
        expect(out.find("hello   world") == kimix::string::npos);
        kimix_bench::run("compress/collapse_ws high 50%", [&] {
            kimix_bench::sink(compress_collapse_whitespace(
                sv(in), sv("log"), false, true, 1, false, false));
        }, 1, double(in.size()));
    };

    "bench_compress_collapse_common_indent"_test = [] {
        const std::string in = make_ws_log(10000, 50, true);
        const kimix::string out = compress_collapse_whitespace(
            sv(in), sv("log"), false, true, 1, true, false);
        expect(out.starts_with("[common-indent: 8 cols removed]\n"));
        expect(out.find("\n\n\n") == kimix::string::npos);
        kimix_bench::run("compress/collapse_ws common_indent", [&] {
            kimix_bench::sink(compress_collapse_whitespace(
                sv(in), sv("log"), false, true, 1, true, false));
        }, 1, double(in.size()));
    };

    "bench_compress_renumber_lines"_test = [] {
        const std::string in = make_numbered_log(10000);
        const kimix::string out = compress_renumber_lines(sv(in));
        expect(out.size() == in.size() - 2 * 10000);
        expect(out.find("\n  ") == kimix::string::npos);
        kimix_bench::run("compress/renumber_lines", [&] {
            kimix_bench::sink(compress_renumber_lines(sv(in)));
        }, 1, double(in.size()));
    };

    "bench_compress_strip_control_low_5pct"_test = [] {
        const std::string in = make_control_log(10000, 5);
        const kimix::string out = compress_strip_control_noise(sv(in));
        expect(out.find(char(0x1B)) == kimix::string::npos);
        expect(out.find('\r') == kimix::string::npos);
        expect(out.size() < in.size());
        kimix_bench::run("compress/strip_control low 5%", [&] {
            kimix_bench::sink(compress_strip_control_noise(sv(in)));
        }, 1, double(in.size()));
    };

    "bench_compress_strip_control_high_50pct"_test = [] {
        const std::string in = make_control_log(10000, 50);
        const kimix::string out = compress_strip_control_noise(sv(in));
        expect(out.find(char(0x1B)) == kimix::string::npos);
        expect(out.find('\r') == kimix::string::npos);
        expect(out.size() < in.size());
        kimix_bench::run("compress/strip_control high 50%", [&] {
            kimix_bench::sink(compress_strip_control_noise(sv(in)));
        }, 1, double(in.size()));
    };
}
