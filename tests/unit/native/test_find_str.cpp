// Test for src/runtime/tools/find_str.h (plan 013 find_in_file).
// This test covers:
// - per-line overlapping matches (readlines() terminator semantics)
// - case-insensitive (ASCII fold) and case-sensitive search
// - needle at line boundaries / spanning the terminator
// - empty needle contract (len+1 matches per line)
// - 0-based line_index / col output

#include "ut/ut.hpp"
#include "bench_util.h"
#include <runtime/tools/find_str.h>

#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::tools;

namespace {
kimix::string_view sv(const std::string& s) { return kimix::string_view(s); }

// Reference: per-line overlapping search with readlines() terminator
// semantics (the line INCLUDES its trailing '\n') and ASCII-only folding.
// Returns the total match count. O(n*m) - used only as a correctness oracle
// for the benchmark cases, never inside the timed loop.
static size_t ref_find_count(const std::string& content,
                             const std::string& needle, bool ci) {
    size_t count = 0;
    size_t ls = 0;
    while (ls < content.size()) {
        const size_t nl = content.find('\n', ls);
        const size_t le = (nl == std::string::npos) ? content.size() : nl + 1;
        if (needle.empty()) {
            // reference find("") semantics: matches at 0..len inclusive
            count += (le - ls) + 1;
        } else {
            size_t start = ls;
            while (start + needle.size() <= le) {
                bool ok = true;
                for (size_t k = 0; k < needle.size(); ++k) {
                    unsigned char a =
                        static_cast<unsigned char>(content[start + k]);
                    unsigned char b =
                        static_cast<unsigned char>(needle[k]);
                    if (ci) {
                        if (a >= 'A' && a <= 'Z') {
                            a = static_cast<unsigned char>(a + 32);
                        }
                        if (b >= 'A' && b <= 'Z') {
                            b = static_cast<unsigned char>(b + 32);
                        }
                    }
                    if (a != b) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    ++count;
                }
                ++start; // overlapping: resume at idx + 1
            }
        }
        ls = le;
    }
    return count;
}

// ~80-char log lines: every line embeds "ab" (with every 3rd line also
// embedding a capital "AB" so the ci fold path is exercised) and every 5th
// line embeds a 40-char phrase (long-needle workload).
static std::string find_content(size_t byte_goal) {
    const char* const long_phrase =
        "request completed successfully in 42 milliseconds";
    std::string out;
    out.reserve(byte_goal + 96);
    size_t i = 0;
    while (out.size() < byte_goal) {
        std::string line = "2026-08-31 12:00:00 INFO  handling ";
        if (i % 5 == 0) {
            line += long_phrase;
        } else {
            line += (i % 3 == 0) ? "item AB refers to ab-chain"
                                 : "item ab refers to pipeline";
        }
        line += " tag=ab";
        line += std::to_string(i % 9);
        if (line.size() > 80) {
            line.resize(80);
        } else {
            line.append(80 - line.size(), ' ');
        }
        out += line;
        out.push_back('\n');
        ++i;
    }
    return out;
}

// 80 identical chars per line -> heavily overlapping matches ("aa" yields
// 79 matches per line, ~1M total).
static std::string dense_content(size_t line_count) {
    std::string out;
    out.reserve(line_count * 81);
    for (size_t i = 0; i < line_count; ++i) {
        out.append(80, 'a');
        out.push_back('\n');
    }
    return out;
}
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

    // -----------------------------------------------------------------------
    // Benchmarks - find_in_file (kimix_bench contract). 1 MB of log-shaped
    // text; needles: short/long, case-insensitive vs sensitive, dense
    // overlapping matches, and the empty-needle edge (O(n) matches). Every
    // case asserts the match count against the naive reference oracle.
    // -----------------------------------------------------------------------

    "bench_find_short_ci_1mb"_test = [] {
        const std::string content = find_content(1 << 20);
        const std::string needle = "ab";
        kimix::vector<find_match> out;
        size_t total = 0;
        kimix_bench::run("tools/find_short_ci_1mb",
                         [&] {
                             out.clear();
                             find_in_file(kimix::string_view(content),
                                          kimix::string_view(needle), true,
                                          out);
                             total += out.size();
                         },
                         1, static_cast<double>(content.size()));
        expect(eq(out.size(), ref_find_count(content, needle, true)));
        kimix_bench::sink(total);
    };

    "bench_find_short_cs_1mb"_test = [] {
        const std::string content = find_content(1 << 20);
        const std::string needle = "ab";
        kimix::vector<find_match> out;
        size_t total = 0;
        kimix_bench::run("tools/find_short_cs_1mb",
                         [&] {
                             out.clear();
                             find_in_file(kimix::string_view(content),
                                          kimix::string_view(needle), false,
                                          out);
                             total += out.size();
                         },
                         1, static_cast<double>(content.size()));
        expect(eq(out.size(), ref_find_count(content, needle, false)));
        kimix_bench::sink(total);
    };

    "bench_find_long_ci_1mb"_test = [] {
        const std::string content = find_content(1 << 20);
        const std::string needle =
            "request completed successfully in 42 milliseconds";
        kimix::vector<find_match> out;
        size_t total = 0;
        kimix_bench::run("tools/find_long_ci_1mb",
                         [&] {
                             out.clear();
                             find_in_file(kimix::string_view(content),
                                          kimix::string_view(needle), true,
                                          out);
                             total += out.size();
                         },
                         1, static_cast<double>(content.size()));
        expect(eq(out.size(), ref_find_count(content, needle, true)));
        kimix_bench::sink(total);
    };

    "bench_find_dense_overlap_1mb"_test = [] {
        const std::string content = dense_content(13000); // ~1.05 MB
        const std::string needle = "aa";
        kimix::vector<find_match> out;
        size_t total = 0;
        kimix_bench::run("tools/find_dense_overlap_1mb",
                         [&] {
                             out.clear();
                             find_in_file(kimix::string_view(content),
                                          kimix::string_view(needle), false,
                                          out);
                             total += out.size();
                         },
                         1, static_cast<double>(content.size()));
        expect(eq(out.size(), ref_find_count(content, needle, false)));
        kimix_bench::sink(total);
    };

    "bench_find_empty_needle_1mb"_test = [] {
        const std::string content = find_content(1 << 20);
        const std::string needle;
        kimix::vector<find_match> out;
        size_t total = 0;
        kimix_bench::run("tools/find_empty_needle_1mb",
                         [&] {
                             out.clear();
                             find_in_file(kimix::string_view(content),
                                          kimix::string_view(needle), true,
                                          out);
                             total += out.size();
                         },
                         1, static_cast<double>(content.size()));
        // exact reference: sum of (line_len + 1) over all lines
        expect(eq(out.size(), ref_find_count(content, needle, true)));
        kimix_bench::sink(total);
    };
}
