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
#include <runtime/diff/diff_engine.h>

#include <algorithm>
#include <string>

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
}
