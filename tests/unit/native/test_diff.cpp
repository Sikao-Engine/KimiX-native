// Test for src/runtime/diff/diff_engine.h (diff kernel).
// This test covers:
// - empty old/new text
// - identical texts
// - trailing newline handling
// - single-line and multi-line changes
// - multiple hunks
// - context_lines variations
// - inline diff ranges including tabs

#include "ut/ut.hpp"
#include "bench_util.h"
#include <runtime/diff/diff_engine.h>

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <tuple>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using namespace kimix::runtime::diff;

static kimix::string s(const char* text) {
    return kimix::string(text);
}

static bool ranges_equal(const kimix::vector<offset_range>& a,
                         const kimix::vector<offset_range>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].start != b[i].start || a[i].end != b[i].end) {
            return false;
        }
    }
    return true;
}

static bool map_equal(const kimix::vector<int>& a,
                      std::initializer_list<int> b) {
    if (a.size() != b.size()) {
        return false;
    }
    size_t i = 0;
    for (int v : b) {
        if (a[i] != v) {
            return false;
        }
        ++i;
    }
    return true;
}

// --- benchmark data builders (see bench_util.h contract) ---

static kimix::vector<kimix::string> bench_make_lines(size_t n, const char* base) {
    kimix::vector<kimix::string> lines;
    lines.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        lines.emplace_back(kimix::format("{}{}", base, i));
    }
    return lines;
}

enum class bench_change_mode {
    Near,     // 1% of lines changed
    Moderate, // 30% changed
    All,      // every line changed
};

static kimix::vector<kimix::string> bench_derive_new(
    const kimix::vector<kimix::string>& old_lines, bench_change_mode mode) {
    kimix::vector<kimix::string> new_lines;
    new_lines.reserve(old_lines.size());
    for (size_t i = 0; i < old_lines.size(); ++i) {
        bool changed = false;
        switch (mode) {
        case bench_change_mode::Near:
            changed = (i % 100 == 0);
            break;
        case bench_change_mode::Moderate:
            changed = (i % 10 < 3);
            break;
        case bench_change_mode::All:
            changed = true;
            break;
        }
        if (changed) {
            new_lines.emplace_back(kimix::format("chg_{}", i));
        } else {
            new_lines.push_back(old_lines[i]);
        }
    }
    return new_lines;
}

static size_t bench_equal_line_count(const kimix::vector<opcode>& ops) {
    size_t n = 0;
    for (const auto& op : ops) {
        if (op.tag == "equal") {
            n += op.old_end - op.old_start;
        }
    }
    return n;
}

// Verify the opcodes tile [0, old_n) x [0, new_n) contiguously.
static bool bench_opcodes_tile(const kimix::vector<opcode>& ops,
                               size_t old_n, size_t new_n) {
    size_t oi = 0;
    size_t ni = 0;
    for (const auto& op : ops) {
        if (op.old_start != oi || op.new_start != ni) {
            return false;
        }
        oi = op.old_end;
        ni = op.new_end;
    }
    return oi == old_n && ni == new_n;
}

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "unified_diff_empty_both"_test = [] {
        const auto result = unified_diff("", "", "", true, "\n");
        expect(eq(result.size(), size_t(0)));
    };

    "unified_diff_identical"_test = [] {
        const auto result = unified_diff("a\nb\n", "a\nb\n", "", true, "\n");
        expect(eq(result.size(), size_t(0)));
    };

    "unified_diff_single_line"_test = [] {
        const auto result = unified_diff("hello", "world", "", true, "\n");
        const kimix::string expected =
            "--- a/file\n"
            "+++ b/file\n"
            "@@ -1 +1 @@\n"
            "-hello\n"
            "+world\n";
        expect(eq(result, expected));
    };

    "unified_diff_multi_line_with_path"_test = [] {
        const auto result = unified_diff("a\nb\nc\n", "a\nB\nc\n", "test.txt", true, "\n");
        const kimix::string expected =
            "--- a/test.txt\n"
            "+++ b/test.txt\n"
            "@@ -1,3 +1,3 @@\n"
            " a\n"
            "-b\n"
            "+B\n"
            " c\n";
        expect(eq(result, expected));
    };

    "unified_diff_no_file_header"_test = [] {
        const auto result = unified_diff("a\nb\nc\n", "a\nB\nc\n", "test.txt", false, "\n");
        const kimix::string expected =
            "@@ -1,3 +1,3 @@\n"
            " a\n"
            "-b\n"
            "+B\n"
            " c\n";
        expect(eq(result, expected));
    };

    "unified_diff_custom_lineterm"_test = [] {
        const auto result = unified_diff("a\nb\n", "a\nB\n", "", true, "\r\n");
        const kimix::string expected =
            "--- a/file\r\n"
            "+++ b/file\r\n"
            "@@ -1,2 +1,2 @@\r\n"
            " a\n"
            "-b\n"
            "+B\n";
        expect(eq(result, expected));
    };

    "unified_diff_trailing_newline_added"_test = [] {
        // After normalizing the last line to end with '\n', both inputs become
        // identical ("a\n"), so no diff is produced.
        const auto result = unified_diff("a", "a\n", "", true, "\n");
        expect(eq(result.size(), size_t(0)));
    };

    "unified_diff_multiple_hunks"_test = [] {
        kimix::string old_text;
        kimix::string new_text;
        for (int i = 1; i <= 20; ++i) {
            old_text += kimix::format("{}", i) + "\n";
            if (i == 5) {
                new_text += "X\n";
            } else if (i == 15) {
                new_text += "Y\n";
            } else {
                new_text += kimix::format("{}", i) + "\n";
            }
        }

        const auto result = unified_diff(old_text, new_text, "nums.txt", true, "\n");
        expect(result.find("@@ -2,7 +2,7 @@") != kimix::string::npos)
            << "first hunk header present";
        expect(result.find("@@ -12,7 +12,7 @@") != kimix::string::npos)
            << "second hunk header present";
        expect(result.find("-5\n") != kimix::string::npos) << "first deletion present";
        expect(result.find("+X\n") != kimix::string::npos) << "first insertion present";
        expect(result.find("-15\n") != kimix::string::npos) << "second deletion present";
        expect(result.find("+Y\n") != kimix::string::npos) << "second insertion present";
    };

    "diff_hunks_empty"_test = [] {
        const auto hunks = diff_hunks("", "", 3);
        expect(eq(hunks.size(), size_t(0)));
    };

    "diff_hunks_identical"_test = [] {
        const auto hunks = diff_hunks("a\nb\n", "a\nb\n", 3);
        expect(eq(hunks.size(), size_t(0)));
    };

    "diff_hunks_single_change"_test = [] {
        const auto hunks = diff_hunks("a\nb\nc\n", "a\nB\nc\n", 3);
        expect(eq(hunks.size(), size_t(1)));
        const auto& h = hunks.front();
        expect(eq(h.old_start, size_t(1)));
        expect(eq(h.new_start, size_t(1)));
        expect(eq(h.old_lines.size(), size_t(3)));
        expect(eq(h.new_lines.size(), size_t(3)));
        expect(eq(h.old_lines[0], s("a")));
        expect(eq(h.old_lines[1], s("b")));
        expect(eq(h.old_lines[2], s("c")));
        expect(eq(h.new_lines[0], s("a")));
        expect(eq(h.new_lines[1], s("B")));
        expect(eq(h.new_lines[2], s("c")));
    };

    "diff_hunks_context_lines_zero"_test = [] {
        const auto hunks = diff_hunks("a\nb\nc\n", "a\nB\nc\n", 0);
        expect(eq(hunks.size(), size_t(1)));
        const auto& h = hunks.front();
        expect(eq(h.old_start, size_t(2)));
        expect(eq(h.new_start, size_t(2)));
        expect(eq(h.old_lines.size(), size_t(1)));
        expect(eq(h.new_lines.size(), size_t(1)));
        expect(eq(h.old_lines[0], s("b")));
        expect(eq(h.new_lines[0], s("B")));
    };

    "diff_hunks_context_lines_one"_test = [] {
        const auto hunks = diff_hunks("a\nb\nc\n", "a\nB\nc\n", 1);
        expect(eq(hunks.size(), size_t(1)));
        const auto& h = hunks.front();
        expect(eq(h.old_start, size_t(1)));
        expect(eq(h.new_start, size_t(1)));
        expect(eq(h.old_lines.size(), size_t(3)));
        expect(eq(h.new_lines.size(), size_t(3)));
    };

    "diff_hunks_multiple_hunks"_test = [] {
        kimix::string old_text;
        kimix::string new_text;
        for (int i = 1; i <= 20; ++i) {
            old_text += kimix::format("{}", i) + "\n";
            if (i == 5) {
                new_text += "X\n";
            } else if (i == 15) {
                new_text += "Y\n";
            } else {
                new_text += kimix::format("{}", i) + "\n";
            }
        }

        const auto hunks = diff_hunks(old_text, new_text, 3);
        expect(eq(hunks.size(), size_t(2)));
        expect(eq(hunks[0].old_start, size_t(2)));
        expect(eq(hunks[1].old_start, size_t(12)));
        expect(eq(hunks[0].old_lines.size(), size_t(7)));
        expect(eq(hunks[1].old_lines.size(), size_t(7)));
    };

    "inline_diff_ranges_identical"_test = [] {
        kimix::vector<offset_range> deletes;
        kimix::vector<offset_range> inserts;
        std::tie(deletes, inserts) = inline_diff_ranges("hello", "hello", 0.5, 4);
        expect(eq(deletes.size(), size_t(0)));
        expect(eq(inserts.size(), size_t(0)));
    };

    "inline_diff_ranges_simple"_test = [] {
        kimix::vector<offset_range> deletes;
        kimix::vector<offset_range> inserts;
        std::tie(deletes, inserts) = inline_diff_ranges("hello world", "hello WORLD", 0.5, 4);
        expect(eq(deletes.size(), size_t(1)));
        expect(eq(inserts.size(), size_t(1)));
        expect(eq(deletes[0].start, size_t(6)));
        expect(eq(deletes[0].end, size_t(11)));
        expect(eq(inserts[0].start, size_t(6)));
        expect(eq(inserts[0].end, size_t(11)));
    };

    "inline_diff_ranges_tabs"_test = [] {
        kimix::vector<offset_range> deletes;
        kimix::vector<offset_range> inserts;
        std::tie(deletes, inserts) = inline_diff_ranges("a\tb", "a\tB", 0.5, 4);
        // expandtabs(4): "a   b" and "a   B" (length 5).
        expect(eq(deletes.size(), size_t(1)));
        expect(eq(inserts.size(), size_t(1)));
        expect(eq(deletes[0].start, size_t(4)));
        expect(eq(deletes[0].end, size_t(5)));
        expect(eq(inserts[0].start, size_t(4)));
        expect(eq(inserts[0].end, size_t(5)));
    };

    "inline_diff_ranges_min_ratio"_test = [] {
        kimix::vector<offset_range> deletes;
        kimix::vector<offset_range> inserts;
        std::tie(deletes, inserts) = inline_diff_ranges("abc", "xyz", 0.5, 4);
        expect(eq(deletes.size(), size_t(0)));
        expect(eq(inserts.size(), size_t(0)));
    };

    "inline_diff_ranges_insert_only"_test = [] {
        kimix::vector<offset_range> deletes;
        kimix::vector<offset_range> inserts;
        std::tie(deletes, inserts) = inline_diff_ranges("foo", "foobar", 0.5, 4);
        expect(eq(deletes.size(), size_t(0)));
        expect(eq(inserts.size(), size_t(1)));
        expect(eq(inserts[0].start, size_t(3)));
        expect(eq(inserts[0].end, size_t(6)));
    };

    "split_lines_keepends"_test = [] {
        const auto lines = split_lines("a\nb\r\nc", true);
        expect(eq(lines.size(), size_t(3)));
        expect(eq(lines[0], s("a\n")));
        expect(eq(lines[1], s("b\r\n")));
        expect(eq(lines[2], s("c")));
    };

    "split_lines_no_keepends"_test = [] {
        const auto lines = split_lines("a\nb\r\nc", false);
        expect(eq(lines.size(), size_t(3)));
        expect(eq(lines[0], s("a")));
        expect(eq(lines[1], s("b")));
        expect(eq(lines[2], s("c")));
    };

    "split_lines_unicode_separators"_test = [] {
        // U+2028 LINE SEPARATOR and U+2029 PARAGRAPH SEPARATOR.
        const kimix::string text = "a\xe2\x80\xa8" "b\xe2\x80\xa9" "c";
        const auto lines = split_lines(text, true);
        expect(eq(lines.size(), size_t(3)));
        expect(eq(lines[0], kimix::string("a\xe2\x80\xa8")));
        expect(eq(lines[1], kimix::string("b\xe2\x80\xa9")));
        expect(eq(lines[2], s("c")));
    };

    "build_offset_map_identical"_test = [] {
        kimix::vector<int> out;
        build_offset_map("abc", "abc", 4, out);
        expect(map_equal(out, {0, 1, 2, 3}));
        build_offset_map("", "", 4, out);
        expect(map_equal(out, {0}));
        build_offset_map("a\nb", "a\nb", 4, out);
        expect(map_equal(out, {0, 1, 2, 3}));
    };

    "build_offset_map_tab_expansion"_test = [] {
        kimix::vector<int> out;
        // "\ta" -> "    a" with tab_size 4.
        build_offset_map("\ta", "    a", 4, out);
        expect(map_equal(out, {0, 4, 5}));
        // "a\tb" -> "a   b".
        build_offset_map("a\tb", "a   b", 4, out);
        expect(map_equal(out, {0, 1, 4, 5}));
        // tab_size 2: "\ta" -> "  a".
        build_offset_map("\ta", "  a", 2, out);
        expect(map_equal(out, {0, 2, 3}));
        // tab_size 8: "\tab" -> "        ab".
        build_offset_map("\tab", "        ab", 8, out);
        expect(map_equal(out, {0, 8, 9, 10}));
    };

    "build_offset_map_tab_columns"_test = [] {
        kimix::vector<int> out;
        // Tab at column 3 adds 1 space; at column 4 (multiple) adds 4 spaces.
        build_offset_map("abc\tx", "abc x", 4, out);
        expect(map_equal(out, {0, 1, 2, 3, 4, 5}));
        build_offset_map("abcd\tx", "abcd    x", 4, out);
        expect(map_equal(out, {0, 1, 2, 3, 4, 8, 9}));
        build_offset_map("a\t\n", "a   \n", 4, out);
        expect(map_equal(out, {0, 1, 4, 5}));
    };

    "build_offset_map_unicode"_test = [] {
        kimix::vector<int> out;
        // CJK 世 (3 UTF-8 bytes) + 'a' -> 2 code points.
        const kimix::string raw_cjk = "\xe4\xb8\x96" "a";
        build_offset_map(raw_cjk, raw_cjk, 4, out);
        expect(map_equal(out, {0, 1, 2}));
        // emoji U+1F600 (4 UTF-8 bytes) -> 1 code point.
        const kimix::string raw_emoji = "\xf0\x9f\x98\x80";
        build_offset_map(raw_emoji, raw_emoji, 4, out);
        expect(map_equal(out, {0, 1}));
        // 'e' + combining acute (2 bytes) -> 2 code points.
        const kimix::string raw_comb = "e\xcc\x81";
        build_offset_map(raw_comb, raw_comb, 4, out);
        expect(map_equal(out, {0, 1, 2}));
        // CJK with tab: 世\t界 -> "世   界" (tab at column 1 -> 3 spaces).
        const kimix::string raw_tab = "\xe4\xb8\x96\t\xe7\x95\x8c";
        const kimix::string ren_tab = "\xe4\xb8\x96   \xe7\x95\x8c";
        build_offset_map(raw_tab, ren_tab, 4, out);
        expect(map_equal(out, {0, 1, 4, 5}));
    };

    "build_offset_map_empty_raw"_test = [] {
        kimix::vector<int> out;
        // Fallback for empty raw: [len(rendered)].
        build_offset_map("", "abc", 4, out);
        expect(map_equal(out, {3}));
        build_offset_map("", "", 4, out);
        expect(map_equal(out, {0}));
    };

    "build_offset_map_mismatch_fallback"_test = [] {
        kimix::vector<int> out;
        // rendered shorter: linear fallback.
        build_offset_map("abc", "ab", 4, out);
        expect(map_equal(out, {0, 0, 1, 2}));
        // rendered longer: linear fallback.
        build_offset_map("ab", "abcdef", 4, out);
        expect(map_equal(out, {0, 3, 6}));
        // rendered empty.
        build_offset_map("abc", "", 4, out);
        expect(map_equal(out, {0, 0, 0, 0}));
    };

    "build_offset_map_trailing_newline"_test = [] {
        kimix::vector<int> out;
        build_offset_map("ab\n", "ab\n", 4, out);
        expect(map_equal(out, {0, 1, 2, 3}));
        build_offset_map("a\t\n", "a   \n", 4, out);
        expect(map_equal(out, {0, 1, 4, 5}));
    };

    // --- benchmarks (see bench_util.h contract) ---
    // No hard timing assertions; expect() guards verify the measured path.

    "bench_diff_compute_near_5k"_test = [] {
        const size_t n = 5000;
        const auto old_lines = bench_make_lines(n, "line_N_");
        const auto new_lines = bench_derive_new(old_lines, bench_change_mode::Near);
        kimix::vector<opcode> ops;
        size_t checksum = 0;
        kimix_bench::time_op("diff/compute_opcodes/near_5k", [&] {
            compute_opcodes(old_lines, new_lines, ops);
            checksum += ops.size();
        });
        kimix_bench::sink(checksum);
        expect(eq(bench_equal_line_count(ops), n - 50));
        expect(bench_opcodes_tile(ops, n, n));
    };

    "bench_diff_compute_moderate_5k"_test = [] {
        const size_t n = 5000;
        const auto old_lines = bench_make_lines(n, "line_M_");
        const auto new_lines = bench_derive_new(old_lines, bench_change_mode::Moderate);
        kimix::vector<opcode> ops;
        size_t checksum = 0;
        kimix_bench::time_op("diff/compute_opcodes/moderate_5k", [&] {
            compute_opcodes(old_lines, new_lines, ops);
            checksum += ops.size();
        });
        kimix_bench::sink(checksum);
        expect(eq(bench_equal_line_count(ops), n - (n / 10) * 3));
        expect(bench_opcodes_tile(ops, n, n));
    };

    "bench_diff_compute_different_5k"_test = [] {
        const size_t n = 5000;
        const auto old_lines = bench_make_lines(n, "line_D_");
        const auto new_lines = bench_derive_new(old_lines, bench_change_mode::All);
        kimix::vector<opcode> ops;
        size_t checksum = 0;
        kimix_bench::time_op("diff/compute_opcodes/different_5k", [&] {
            compute_opcodes(old_lines, new_lines, ops);
            checksum += ops.size();
        });
        kimix_bench::sink(checksum);
        expect(eq(bench_equal_line_count(ops), size_t(0)));
        expect(eq(ops.size(), size_t(1)));
        expect(eq(ops.front().tag, kimix::string("replace")));
        expect(bench_opcodes_tile(ops, n, n));
    };

    "bench_diff_compute_scaling_diverse"_test = [] {
        // Growing line counts with distinct lines: measures the per-line
        // matching cost (linear for unique content; no n*m table exists).
        const size_t sizes[] = {1000, 5000, 10000};
        for (size_t n : sizes) {
            const auto lines = bench_make_lines(n, "scale_D_");
            kimix::vector<opcode> ops;
            size_t checksum = 0;
            char name[80];
            std::snprintf(name, sizeof(name),
                          "diff/compute_opcodes/diverse_%zuk", n / 1000);
            kimix_bench::time_op(name, [&] {
                compute_opcodes(lines, lines, ops);
                checksum += ops.size();
            }, 0.05);
            kimix_bench::sink(checksum);
            expect(eq(ops.size(), size_t(1)));
            expect(eq(ops.front().tag, kimix::string("equal")));
            expect(eq(ops.front().old_end - ops.front().old_start, n));
        }
    };

    "bench_diff_compute_scaling_identical"_test = [] {
        // Every line equal to the same string: the classic quadratic DP worst
        // case (SequenceMatcher j2len-style n*m scan per recursion level).
        // Quantifies time growth; memory stays O(n+m) — no full table.
        const size_t sizes[] = {1000, 2000, 3000, 10000};
        for (size_t n : sizes) {
            const auto lines =
                kimix::vector<kimix::string>(n, kimix::string("same_line"));
            kimix::vector<opcode> ops;
            size_t checksum = 0;
            char name[80];
            std::snprintf(name, sizeof(name),
                          "diff/compute_opcodes/identical_%zu", n);
            kimix_bench::time_op(name, [&] {
                compute_opcodes(lines, lines, ops);
                checksum += ops.size();
            }, 0.05);
            kimix_bench::sink(checksum);
            expect(eq(ops.size(), size_t(1)));
            expect(eq(ops.front().old_end - ops.front().old_start, n));
        }
    };

    "bench_diff_unified_1mb"_test = [] {
        // ~1 MB old/new texts with 30% of the lines changed.
        const size_t n = 22000;
        kimix::string old_text;
        kimix::string new_text;
        old_text.reserve(1u << 20);
        new_text.reserve(1u << 20);
        for (size_t i = 0; i < n; ++i) {
            old_text += kimix::format("line_U_{} some longer trailing content here\n", i);
            if (i % 10 < 3) {
                new_text += kimix::format("changed_{} replacement body padding here\n", i);
            } else {
                new_text += kimix::format("line_U_{} some longer trailing content here\n", i);
            }
        }
        kimix::string result;
        size_t checksum = 0;
        const double bytes = double(old_text.size() + new_text.size());
        kimix_bench::run("diff/unified_diff_1mb_30pct", [&] {
            result = unified_diff(old_text, new_text, "big.txt", true, "\n");
            checksum += result.size();
        }, 1, bytes);
        kimix_bench::sink(checksum);
        expect(result.find("--- a/big.txt\n") != kimix::string::npos);
        expect(result.find("+++ b/big.txt\n") != kimix::string::npos);
        expect(result.find("@@") != kimix::string::npos);
        expect(result.find("-line_U_0 some longer trailing content here\n") != kimix::string::npos)
            << "changed line appears as deletion";
        expect(result.find("+changed_0 replacement body padding here\n") != kimix::string::npos)
            << "changed line appears as insertion";
        expect(result.find(" line_U_3 some longer trailing content here\n") != kimix::string::npos)
            << "unchanged line appears as context";
    };

    "bench_diff_inline_minified_10k"_test = [] {
        // Long minified-style lines (10 KB) with a modest changed block.
        const size_t len = 10000;
        kimix::string old_line(len, 'a');
        kimix::string new_line(len, 'a');
        for (size_t k = 0; k < len; ++k) {
            const char c = static_cast<char>('a' + ((k * 7 + k / 997) % 12));
            old_line[k] = c;
            new_line[k] = c;
        }
        for (size_t k = 4000; k < 4064; ++k) {
            new_line[k] = static_cast<char>('A' + (k * 3) % 26);
        }
        kimix::vector<offset_range> deletes;
        kimix::vector<offset_range> inserts;
        size_t checksum = 0;
        const double bytes = double(old_line.size() + new_line.size());
        kimix_bench::run("diff/inline_ranges_minified_10k", [&] {
            std::tie(deletes, inserts) =
                inline_diff_ranges(old_line, new_line, 0.5, 4);
            checksum += deletes.size() + inserts.size();
        }, 1, bytes);
        kimix_bench::sink(checksum);
        expect(!deletes.empty());
        expect(!inserts.empty());
        kimix::vector<int> offmap;
        build_offset_map(old_line, old_line, 4, offmap);
        const size_t expanded = static_cast<size_t>(offmap.back());
        for (const auto& r : deletes) {
            expect(r.start < r.end);
            expect(r.end <= expanded);
        }
        for (const auto& r : inserts) {
            expect(r.start < r.end);
            expect(r.end <= expanded);
        }
    };

    "bench_diff_inline_worst_case_3k"_test = [] {
        // Both lines are a run of the same character: the quadratic blowup
        // case for the inline matcher (n*m pairs per find_longest_match).
        const size_t len = 3000;
        const kimix::string old_line(len, 'a');
        kimix::string new_line(len, 'a');
        new_line[len - 1] = 'b';
        kimix::vector<offset_range> deletes;
        kimix::vector<offset_range> inserts;
        size_t checksum = 0;
        kimix_bench::time_op("diff/inline_ranges_all_same_3k", [&] {
            std::tie(deletes, inserts) =
                inline_diff_ranges(old_line, new_line, 0.5, 4);
            checksum += deletes.size() + inserts.size();
        });
        kimix_bench::sink(checksum);
        expect(eq(deletes.size(), size_t(1)));
        expect(eq(inserts.size(), size_t(1)));
        expect(eq(deletes[0].start, len - 1));
        expect(eq(deletes[0].end, len));
        expect(eq(inserts[0].start, len - 1));
        expect(eq(inserts[0].end, len));
    };
}
