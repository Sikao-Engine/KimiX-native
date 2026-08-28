// Test for the edit built-in tool kernels (builtin_tools/edit_tool.h).
//
// Covers the plan section 7 kernel list for the `edit` tool:
// - common newline/line helpers: split_lf vs split_lines distinction,
//   join_lf, normalize variants, detect_line_ending, trailing-newline restore
// - fuzz_ratio golden vectors (rapidfuzz fuzz.ratio parity; ASCII, unicode,
//   empty strings, CRLF, long prefixes) + the too_large length gate
// - replace kernels: exact first/all, max_replacements counting, strip-match
//   terminator preservation, fuzzy fallback suggestions, exact error texts
// - diff hunk kernels: normalize, parse (standard/bare/anchor/no-header),
//   apply bottom-up, exact-match ambiguity error, fuzzy threshold/dominance
//   gap, indent adjustment, trailing-newline restore, first-changed line
// - hashline kernels: compute_line_hash goldens, [path#tag] grammar parse,
//   apply with anchor validation, CR-stripped fuzzy fallback, dedupe, overlap
//   detection, bottom-up ordering, mismatch display
// - sloppy kernels: section split, section-star flag, block vs inline, inline rescan,
//   exact + fuzzy block locate, pure-deletion newline swallow, all_match
//   loop, inline first/all replace, missing-selection error text
//
// Golden vectors are pinned inline (harvested from the Python reference,
// kimi-cli/tests/tools/test_edit_*.py and direct python runs). Whitespace is
// built programmatically so the source stays robust.

#include "ut/ut.hpp"

#include "builtin_tools/edit_tool.h"
#include "builtin_tools/tool_types.h"

#include <cmath>
#include <cstdint>
#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;

using kimix::builtin_tools::tool_status;
namespace edit = kimix::builtin_tools::edit;

namespace {

kimix::string spaces(size_t n) {
    return kimix::string(n, ' ');
}

template <typename T>
void expect_near(T actual, T expected, T eps = 1e-9) {
    expect(std::abs(actual - expected) <= eps);
}

kimix::string f_ratio_str(kimix::string_view a, kimix::string_view b) {
    const edit::fuzz_ratio_result r = edit::fuzz_ratio(a, b);
    return r.status == tool_status::ok
               ? kimix::format("{}", r.score)
               : kimix::string("too_large");
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));
    // ------------------------------------------------------------------
    // Common helpers
    // ------------------------------------------------------------------
    "split_lf_trailing_empty"_test = [] {
        kimix::vector<kimix::string> lines;
        edit::split_lf("a\nb\n", lines);
        expect(eq(lines.size(), size_t(3)));
        expect(eq(lines[0], kimix::string("a")));
        expect(eq(lines[1], kimix::string("b")));
        expect(eq(lines[2], kimix::string("")));
        edit::split_lf("", lines);
        expect(eq(lines.size(), size_t(1)));
        expect(eq(lines[0], kimix::string("")));
    };

    "split_lines_no_trailing_empty"_test = [] {
        kimix::vector<kimix::string> lines;
        edit::split_lines("a\nb\n", lines);
        expect(eq(lines.size(), size_t(2)));
        expect(eq(lines[0], kimix::string("a")));
        expect(eq(lines[1], kimix::string("b")));
        // lone \r and \r\n are boundaries too
        edit::split_lines("a\rb\r\nc", lines);
        expect(eq(lines.size(), size_t(3)));
        expect(eq(lines[0], kimix::string("a")));
        expect(eq(lines[1], kimix::string("b")));
        expect(eq(lines[2], kimix::string("c")));
        edit::split_lines("", lines);
        expect(eq(lines.size(), size_t(0)));
        edit::split_lines("\n", lines);
        expect(eq(lines.size(), size_t(1)));
        expect(eq(lines[0], kimix::string("")));
    };

    "split_lines_keepends"_test = [] {
        kimix::vector<kimix::string> lines;
        edit::split_lines_keepends("a\r\nb\nc", lines);
        expect(eq(lines.size(), size_t(3)));
        expect(eq(lines[0], kimix::string("a\r\n")));
        expect(eq(lines[1], kimix::string("b\n")));
        expect(eq(lines[2], kimix::string("c")));
    };

    "join_lf_and_normalize"_test = [] {
        kimix::vector<kimix::string> lines = {"a", "b", ""};
        expect(eq(edit::join_lf(lines), kimix::string("a\nb\n")));
        expect(eq(edit::normalize_newlines("a\r\nb\r\nc"), kimix::string("a\nb\nc")));
        expect(eq(edit::normalize_newlines("a\rb\r\nc"), kimix::string("a\rb\nc")));
        expect(eq(edit::normalize_breaks("a\rb\r\nc"), kimix::string("a\nb\nc")));
        expect(eq(edit::detect_line_ending("a\nb"), kimix::string("\n")));
        expect(eq(edit::detect_line_ending("a\r\nb"), kimix::string("\r\n")));
    };

    "restore_trailing_newline"_test = [] {
        kimix::string r = "a\nb";
        edit::restore_trailing_newline(r, true);
        expect(eq(r, kimix::string("a\nb\n")));
        r = "a\nb\n";
        edit::restore_trailing_newline(r, true);
        expect(eq(r, kimix::string("a\nb\n")));
        r = "";
        edit::restore_trailing_newline_nonempty(r, true);
        expect(eq(r, kimix::string("")));
        r = "x";
        edit::restore_trailing_newline_nonempty(r, true);
        expect(eq(r, kimix::string("x\n")));
    };

    "py_strip_and_repr"_test = [] {
        expect(eq(edit::py_strip(spaces(3) + "foo" + spaces(2)), kimix::string("foo")));
        expect(eq(edit::py_strip(spaces(4)), kimix::string("")));
        expect(eq(edit::py_repr("hello"), kimix::string("'hello'")));
        expect(eq(edit::py_repr("a'b"), kimix::string("\"a'b\"")));
        expect(eq(edit::py_repr("a\nb"), kimix::string("'a\\nb'")));
        expect(eq(edit::py_repr("caf\xC3\xA9"), kimix::string("'caf\xC3\xA9'")));
    };

    // ------------------------------------------------------------------
    // fuzz_ratio
    // ------------------------------------------------------------------
    "fuzz_ratio_ascii_goldens"_test = [] {
        expect_near(edit::fuzz_ratio("kitten", "sitting").score, 61.53846153846154);
        expect_near(edit::fuzz_ratio("flaw", "lawn").score, 75.0);
        expect_near(edit::fuzz_ratio("intention", "execution").score, 55.55555555555556);
        expect_near(edit::fuzz_ratio("gumbo", "gambol").score, 72.72727272727273);
        expect_near(edit::fuzz_ratio("hello", "hallo").score, 80.0);
        expect_near(edit::fuzz_ratio(spaces(2) + "leading", "leading").score, 87.5);
        expect_near(edit::fuzz_ratio("trailing" + spaces(2), "trailing").score, 88.88888888888889);
        expect_near(edit::fuzz_ratio("hello world", "helloworld").score, 95.23809523809523);
        expect_near(edit::fuzz_ratio("line1\nline2", "line1\nlineX").score, 90.9090909090909);
        expect_near(edit::fuzz_ratio("abc", "abcX").score, 85.71428571428571);
    };

    "fuzz_ratio_empty_and_equal"_test = [] {
        expect_near(edit::fuzz_ratio("", "").score, 100.0);
        expect_near(edit::fuzz_ratio("", "x").score, 0.0);
        expect_near(edit::fuzz_ratio("x", "").score, 0.0);
        expect_near(edit::fuzz_ratio("abc", "abc").score, 100.0);
    };

    "fuzz_ratio_unicode_goldens"_test = [] {
        expect_near(edit::fuzz_ratio("caf\xC3\xA9 au lait", "cafe au lait").score,
                    91.66666666666666);
        expect_near(edit::fuzz_ratio("\xF0\x9F\x99\x82"
                                     "abc",
                                     "\xF0\x9F\x99\x82"
                                     "abd")
                        .score,
                    75.0);
        expect_near(edit::fuzz_ratio("Stra\xC3\x9F"
                                     "e",
                                     "STRASSE")
                        .score,
                    15.384615384615385);
        expect_near(edit::fuzz_ratio("h\xC3\xA9llo", "h\xC3\xABllo").score, 80.0);
    };

    "fuzz_ratio_long_prefix_and_crlf"_test = [] {
        kimix::string a = kimix::string(500, 'a') + "b" + kimix::string(499, 'c');
        kimix::string b = kimix::string(500, 'a') + "d" + kimix::string(499, 'c');
        expect_near(edit::fuzz_ratio(a, b).score, 99.9);
        expect_near(edit::fuzz_ratio("\r\nline\r\n", "line").score,
                    66.66666666666667);
    };

    "fuzz_ratio_too_large"_test = [] {
        const kimix::string big(edit::k_fuzz_max_len + 1, 'a');
        expect(eq(static_cast<int>(edit::fuzz_ratio(big, "a").status), static_cast<int>(tool_status::too_large)));
        const kimix::string mid(5000, 'a');
        expect(eq(static_cast<int>(edit::fuzz_ratio(
                                       mid, kimix::string(edit::k_fuzz_max_len, 'a'))
                                       .status),
                  static_cast<int>(tool_status::too_large)));
    };

    // ------------------------------------------------------------------
    // Replace kernels
    // ------------------------------------------------------------------
    "replace_exact_single"_test = [] {
        edit::replace_edit_item item;
        item.old_text = "world";
        item.new_text = "there";
        const edit::replace_result r = edit::apply_edit("hello world\n", item);
        expect(eq(r.content, kimix::string("hello there\n")));
        expect(eq(r.replacements, size_t(1)));
        expect(!r.suggestion.has_value());
    };

    "replace_noop_cases"_test = [] {
        edit::replace_edit_item item;
        item.old_text = "";
        item.new_text = "x";
        edit::replace_result r = edit::apply_edit("abc", item);
        expect(eq(r.content, kimix::string("abc")));
        expect(eq(r.replacements, size_t(0)));
        item.old_text = "abc";
        item.new_text = "abc";
        r = edit::apply_edit("abc", item);
        expect(eq(r.content, kimix::string("abc")));
        expect(eq(r.replacements, size_t(0)));
    };

    "replace_all_and_overlap"_test = [] {
        edit::replace_edit_item item;
        item.old_text = "a";
        item.new_text = "b";
        item.replace_all = true;
        edit::replace_result r = edit::apply_edit("a a a", item);
        expect(eq(r.content, kimix::string("b b b")));
        expect(eq(r.replacements, size_t(3)));
        // non-overlapping left-to-right (str.replace semantics)
        item.old_text = "aa";
        r = edit::apply_edit("aaaa", item);
        expect(eq(r.content, kimix::string("bb")));
        expect(eq(r.replacements, size_t(2)));
    };

    "replace_all_max_replacements"_test = [] {
        edit::replace_edit_item item;
        item.old_text = "a";
        item.new_text = "b";
        item.replace_all = true;
        item.max_replacements = 2;
        const edit::replace_result r = edit::apply_edit("a a a", item);
        expect(eq(r.content, kimix::string("b b a")));
        expect(eq(r.replacements, size_t(2)));
        expect(!r.suggestion.has_value());
    };

    "replace_all_miss_suggestion"_test = [] {
        edit::replace_edit_item item;
        item.old_text = "zzz";
        item.new_text = "y";
        item.replace_all = true;
        const edit::replace_result r = edit::apply_edit("hello world\nfoo bar\n", item);
        expect(eq(r.content, kimix::string("hello world\nfoo bar\n")));
        expect(eq(r.replacements, size_t(0)));
        expect(!r.suggestion.has_value()); // no line scores >= 75
    };

    "replace_exact_miss_suggestion"_test = [] {
        edit::replace_edit_item item;
        item.old_text = "helo";
        item.new_text = "Y";
        item.match_mode = "exact";
        const edit::replace_result r = edit::apply_edit("hello\n", item);
        expect(eq(r.content, kimix::string("hello\n")));
        expect(eq(r.replacements, size_t(0)));
        expect(r.suggestion.has_value());
        expect(eq(*r.suggestion, kimix::string("hello")));
    };

    "replace_crlf_normalizes_output"_test = [] {
        edit::replace_edit_item item;
        item.old_text = "a";
        item.new_text = "A";
        const edit::replace_result r = edit::apply_edit("a\r\nb\r\n", item);
        expect(eq(r.content, kimix::string("A\nb\n")));
        expect(eq(r.replacements, size_t(1)));
        // lone \r is left alone by _normalize_line_endings
        const edit::replace_result r2 = edit::apply_edit("a\rb\rc\r", item);
        expect(eq(r2.content, kimix::string("A\rb\rc\r")));
    };

    "replace_fuzzy_fallback_suggestion"_test = [] {
        edit::replace_edit_item item;
        item.old_text = "helloworld";
        item.new_text = "y";
        const edit::replace_result r = edit::apply_edit("hello world\nfoo bar\n", item);
        expect(eq(r.content, kimix::string("y\nfoo bar\n")));
        expect(eq(r.replacements, size_t(1)));
        expect(r.suggestion.has_value());
        expect(eq(*r.suggestion, kimix::string("fuzzy-matched at 95%: 'hello world'")));
    };

    "replace_fuzzy_multiline_suggestion"_test = [] {
        edit::replace_edit_item item;
        item.old_text = "line1\nlineX";
        item.new_text = "y";
        const edit::replace_result r = edit::apply_edit("line1\nline2\nline3\n", item);
        expect(eq(r.content, kimix::string("y\nline3\n")));
        expect(eq(r.replacements, size_t(1)));
        expect(r.suggestion.has_value());
        expect(eq(*r.suggestion, kimix::string("fuzzy-matched at 91%: 'line1\nline2'")));
    };

    "replace_strip_match_preserves_terminator"_test = [] {
        edit::replace_edit_item item;
        item.old_text = "\tfoo bar";
        item.new_text = "FOO";
        const edit::replace_result r = edit::apply_edit("x\n foo bar\nz\n", item);
        expect(eq(r.content, kimix::string("x\n FOO\nz\n")));
        expect(eq(r.replacements, size_t(1)));
        // CRLF terminator preserved by the strip path
        item.old_text = "\tfoo";
        const edit::replace_result r2 =
            edit::apply_edit("x\r\n foo \r\ny\r\n", item);
        expect(eq(r2.content, kimix::string("x\r\n FOO \r\ny\r\n")));
        expect(eq(r2.replacements, size_t(1)));
    };

    "replace_unicode_fuzzy"_test = [] {
        edit::replace_edit_item item;
        item.old_text = "cafe au lait";
        item.new_text = "COFFEE";
        const edit::replace_result r =
            edit::apply_edit("caf\xC3\xA9 au lait\n", item);
        expect(eq(r.content, kimix::string("COFFEE\n")));
        expect(eq(r.replacements, size_t(1)));
        expect(r.suggestion.has_value());
        expect(eq(*r.suggestion, kimix::string("fuzzy-matched at 92%: 'caf\xC3\xA9 au lait'")));
    };

    // ------------------------------------------------------------------
    // Diff hunk kernels
    // ------------------------------------------------------------------
    "diff_normalize"_test = [] {
        const kimix::string in =
            "diff --git a/x b/x\r\n"
            "index 123..456 100644\r\n"
            "--- a/x\r\n"
            "+++ b/x\r\n"
            "@@ -1,3 +1,3 @@\r\n"
            " a\r\n"
            "-b\r\n"
            "+c\r\n"
            "\\ No newline at end of file\r\n";
        expect(eq(edit::normalize_diff(in), kimix::string("@@ -1,3 +1,3 @@\n a\n-b\n+c")));
        expect(eq(edit::normalize_diff("\n\n@@ -1 +1 @@\n-x\n+y\n\n\n"), kimix::string("@@ -1 +1 @@\n-x\n+y")));
        expect(eq(edit::normalize_create_content(
                      "+a\n b\n+c\n\\ No newline at end of file\n"),
                  kimix::string("a\nb\nc\n")));
    };

    "diff_parse_standard_hunks"_test = [] {
        const edit::hunks_result r = edit::parse_diff_hunks(
            "@@ -1,3 +1,3 @@\n a\n-b\n+c\n d\n"
            "@@ -5,2 +5,2 @@ ctx\n e\n-f\n+g\n");
        expect(!r.error.failed());
        expect(eq(r.hunks.size(), size_t(2)));
        expect(r.hunks[0].start_line.has_value());
        expect(eq(*r.hunks[0].start_line, 1));
        expect(!r.hunks[0].change_context.has_value());
        expect(eq(r.hunks[0].lines.size(), size_t(4)));
        expect(eq(static_cast<int>(r.hunks[0].lines[0].kind), static_cast<int>(edit::hunk_line_kind::context)));
        expect(eq(r.hunks[0].lines[0].text, kimix::string("a")));
        expect(eq(static_cast<int>(r.hunks[0].lines[1].kind), static_cast<int>(edit::hunk_line_kind::deleted)));
        expect(eq(r.hunks[0].lines[1].text, kimix::string("b")));
        expect(eq(static_cast<int>(r.hunks[0].lines[2].kind), static_cast<int>(edit::hunk_line_kind::add)));
        expect(eq(r.hunks[0].lines[2].text, kimix::string("c")));
        expect(r.hunks[1].start_line.has_value());
        expect(eq(*r.hunks[1].start_line, 5));
        expect(r.hunks[1].change_context.has_value());
        expect(eq(*r.hunks[1].change_context, kimix::string("ctx")));
    };

    "diff_parse_bare_and_anchor"_test = [] {
        edit::hunks_result r = edit::parse_diff_hunks("@@\n a\n-b\n+c\n");
        expect(!r.error.failed());
        expect(eq(r.hunks.size(), size_t(1)));
        expect(!r.hunks[0].start_line.has_value());
        expect(!r.hunks[0].change_context.has_value());

        r = edit::parse_diff_hunks("@@ 42 @@\n a\n-b\n");
        expect(!r.error.failed());
        expect(eq(r.hunks.size(), size_t(1)));
        expect(r.hunks[0].start_line.has_value());
        expect(eq(*r.hunks[0].start_line, 42));
        expect(r.hunks[0].change_context.has_value());
        expect(eq(*r.hunks[0].change_context, kimix::string("@@")));
    };

    "diff_parse_skips_file_headers_and_blank"_test = [] {
        const edit::hunks_result r =
            edit::parse_diff_hunks("--- a\n+++ b\n@@ -1 +1 @@\n a\n-b\n+c\n");
        expect(!r.error.failed());
        expect(eq(r.hunks.size(), size_t(1)));
        expect(eq(r.hunks[0].lines.size(), size_t(3)));
        const edit::hunks_result empty =
            edit::parse_diff_hunks("--- a\n+++ b\n");
        expect(!empty.error.failed());
        expect(eq(empty.hunks.size(), size_t(0)));
        const edit::hunks_result empty2 = edit::parse_diff_hunks("   \n\n");
        expect(!empty2.error.failed());
        expect(eq(empty2.hunks.size(), size_t(0)));
    };

    "diff_parse_malformed_line_ends_hunk_leniently"_test = [] {
        const edit::hunks_result r = edit::parse_diff_hunks(
            "@@ -1 +1 @@\n a\n-junk line\n b\n@@ -5 +5 @@\n c\n");
        expect(!r.error.failed());
        expect(eq(r.hunks.size(), size_t(1)));
        expect(eq(r.hunks[0].lines.size(), size_t(3)));
        expect(eq(static_cast<int>(r.hunks[0].lines[0].kind), static_cast<int>(edit::hunk_line_kind::context)));
        expect(eq(static_cast<int>(r.hunks[0].lines[1].kind), static_cast<int>(edit::hunk_line_kind::deleted)));
        expect(eq(r.hunks[0].lines[1].text, kimix::string("junk line")));
        expect(eq(static_cast<int>(r.hunks[0].lines[2].kind), static_cast<int>(edit::hunk_line_kind::context)));
        expect(eq(r.hunks[0].lines[2].text, kimix::string("b")));
    };

    "diff_parse_unexpected_content_error"_test = [] {
        const edit::hunks_result r = edit::parse_diff_hunks("hello\nworld\n");
        expect(r.error.failed());
        expect(eq(static_cast<int>(r.error.status), static_cast<int>(tool_status::invalid_input)));
        expect(eq(r.error.message, kimix::string("Unexpected diff content outside a hunk: 'hello'")));
        const edit::hunks_result r2 = edit::parse_diff_hunks("--x\n++y\n@@ -1 +1 @@\n a");
        expect(r2.error.failed());
        expect(eq(r2.error.message, kimix::string("Unexpected diff content outside a hunk: '--x'")));
    };

    "diff_parse_blank_line_inside_hunk_is_context"_test = [] {
        const edit::hunks_result r = edit::parse_diff_hunks("@@ -1 +1 @@\n a\n\n-b\n");
        expect(!r.error.failed());
        expect(eq(r.hunks[0].lines.size(), size_t(3)));
        expect(eq(static_cast<int>(r.hunks[0].lines[1].kind), static_cast<int>(edit::hunk_line_kind::context)));
        expect(eq(r.hunks[0].lines[1].text, kimix::string("")));
    };

    "diff_apply_exact_hunk"_test = [] {
        kimix::vector<edit::diff_hunk> hunks;
        edit::diff_hunk h;
        h.start_line = 2;
        h.lines.push_back({edit::hunk_line_kind::context, "line2"});
        h.lines.push_back({edit::hunk_line_kind::deleted, "line3"});
        h.lines.push_back({edit::hunk_line_kind::add, "line3changed"});
        hunks.push_back(std::move(h));
        const edit::diff_apply_result r =
            edit::apply_diff_hunks(hunks, "line1\nline2\nline3\n");
        expect(!r.error.failed());
        expect(eq(r.content, kimix::string("line1\nline2\nline3changed\n")));
        expect(r.first_changed_line.has_value());
        expect(eq(*r.first_changed_line, 2));
    };

    "diff_apply_bottom_up_order"_test = [] {
        kimix::vector<edit::diff_hunk> hunks;
        edit::diff_hunk h1;
        h1.start_line = 1;
        h1.lines.push_back({edit::hunk_line_kind::deleted, "a"});
        h1.lines.push_back({edit::hunk_line_kind::add, "A"});
        edit::diff_hunk h2;
        h2.start_line = 2;
        h2.lines.push_back({edit::hunk_line_kind::deleted, "b"});
        h2.lines.push_back({edit::hunk_line_kind::add, "B"});
        hunks.push_back(std::move(h1));
        hunks.push_back(std::move(h2));
        const edit::diff_apply_result r =
            edit::apply_diff_hunks(hunks, "a\nb\nc\n");
        expect(!r.error.failed());
        expect(eq(r.content, kimix::string("A\nB\nc\n")));
        expect(eq(*r.first_changed_line, 1));
    };

    "diff_apply_stable_equal_sort_keys"_test = [] {
        kimix::vector<edit::diff_hunk> hunks;
        edit::diff_hunk h1; // start_line None (key 0), applied first
        h1.lines.push_back({edit::hunk_line_kind::deleted, "a"});
        h1.lines.push_back({edit::hunk_line_kind::add, "A"});
        edit::diff_hunk h2; // depends on h1's output
        h2.lines.push_back({edit::hunk_line_kind::deleted, "A"});
        h2.lines.push_back({edit::hunk_line_kind::add, "X"});
        hunks.push_back(std::move(h1));
        hunks.push_back(std::move(h2));
        const edit::diff_apply_result r =
            edit::apply_diff_hunks(hunks, "a\nb\n");
        expect(!r.error.failed());
        expect(eq(r.content, kimix::string("X\nb\n")));
    };

    "diff_apply_multiple_exact_matches_error"_test = [] {
        kimix::vector<edit::diff_hunk> hunks;
        edit::diff_hunk h;
        h.start_line = 1;
        h.lines.push_back({edit::hunk_line_kind::deleted, "x"});
        hunks.push_back(std::move(h));
        const edit::diff_apply_result r = edit::apply_diff_hunks(hunks, "x\nx\n");
        expect(r.error.failed());
        expect(eq(r.error.message, kimix::string("Found multiple matches for hunk at line 1; "
                                                 "add more context lines to disambiguate.")));
    };

    "diff_apply_no_match_errors"_test = [] {
        kimix::vector<edit::diff_hunk> hunks;
        edit::diff_hunk h;
        h.start_line = 9;
        h.lines.push_back({edit::hunk_line_kind::context, "nope"});
        hunks.push_back(std::move(h));
        edit::diff_apply_result r = edit::apply_diff_hunks(hunks, "a\nb\n");
        expect(r.error.failed());
        expect(eq(r.error.message, kimix::string("No match found for hunk anchored at line 9.")));

        hunks.clear();
        edit::diff_hunk h2;
        h2.change_context = "myanchor";
        h2.lines.push_back({edit::hunk_line_kind::deleted, "nope"});
        hunks.push_back(std::move(h2));
        r = edit::apply_diff_hunks(hunks, "a\nb\n");
        expect(r.error.failed());
        expect(eq(r.error.message, kimix::string("No match found for hunk anchored at myanchor.")));
    };

    "diff_apply_fuzzy_fallback"_test = [] {
        kimix::vector<edit::diff_hunk> hunks;
        edit::diff_hunk h;
        h.lines.push_back({edit::hunk_line_kind::deleted, "The quick brown fox"});
        hunks.push_back(std::move(h));
        const edit::diff_apply_result r =
            edit::apply_diff_hunks(hunks, "the quick brown fox\nover the lazy dog\n");
        expect(!r.error.failed());
        expect(eq(r.content, kimix::string("over the lazy dog\n")));
        expect(eq(*r.first_changed_line, 1));
    };

    "diff_apply_fuzzy_dominance_gap"_test = [] {
        const kimix::string content = "alpha beta gamma\nalpha beta gammaX\n";
        kimix::vector<edit::diff_hunk> ambiguous;
        edit::diff_hunk h;
        h.lines.push_back({edit::hunk_line_kind::deleted, "alpha beta gammY"});
        ambiguous.push_back(std::move(h));
        const edit::diff_apply_result r_amb =
            edit::apply_diff_hunks(ambiguous, content);
        expect(r_amb.error.failed());
        expect(eq(r_amb.error.message, kimix::string("No match found for hunk anchored at line ?.")));

        kimix::vector<edit::diff_hunk> dominant;
        edit::diff_hunk h2;
        h2.lines.push_back({edit::hunk_line_kind::deleted, "alpha beta gamma"});
        dominant.push_back(std::move(h2));
        const edit::diff_apply_result r_dom =
            edit::apply_diff_hunks(dominant, content);
        expect(!r_dom.error.failed());
        expect(eq(r_dom.content, kimix::string("alpha beta gammaX\n")));
    };

    "diff_apply_indent_adjustment"_test = [] {
        // pattern lines are 4-space indented, actual is 8-space for the
        // second context line -> majority delta +4... in the golden the
        // delta tie resolves to 0 (set order), so the add stays unindented.
        kimix::vector<edit::diff_hunk> hunks;
        edit::diff_hunk h;
        h.start_line = 1;
        h.lines.push_back({edit::hunk_line_kind::context,
                           spaces(4) + "def foo():"});
        h.lines.push_back({edit::hunk_line_kind::add, "x = 1"});
        h.lines.push_back({edit::hunk_line_kind::context, spaces(4) + "pass"});
        hunks.push_back(std::move(h));
        const edit::diff_apply_result r =
            edit::apply_diff_hunks(hunks, spaces(4) + "def foo():\n" +
                                              spaces(8) + "pass\n");
        expect(!r.error.failed());
        expect(eq(r.content, spaces(4) + "def foo():\nx = 1\n" +
                                 spaces(8) + "pass\n"));
        expect(eq(*r.first_changed_line, 1));
    };

    "diff_apply_indent_majority_delta"_test = [] {
        kimix::vector<edit::diff_hunk> hunks;
        edit::diff_hunk h;
        h.start_line = 1;
        h.lines.push_back({edit::hunk_line_kind::context, "a"});
        h.lines.push_back({edit::hunk_line_kind::context, spaces(2) + "b"});
        h.lines.push_back({edit::hunk_line_kind::add, "c"});
        h.lines.push_back({edit::hunk_line_kind::context, spaces(4) + "d"});
        hunks.push_back(std::move(h));
        const edit::diff_apply_result r = edit::apply_diff_hunks(
            hunks, spaces(2) + "a\n" + spaces(4) + "b\n" + spaces(6) + "d\n");
        expect(!r.error.failed());
        expect(eq(r.content, spaces(2) + "a\n" + spaces(4) + "b\n" +
                                 spaces(2) + "c\n" + spaces(6) + "d\n"));
    };

    "diff_apply_trailing_newline_semantics"_test = [] {
        kimix::vector<edit::diff_hunk> hunks;
        edit::diff_hunk h;
        h.lines.push_back({edit::hunk_line_kind::deleted, "b"});
        h.lines.push_back({edit::hunk_line_kind::add, "B"});
        hunks.push_back(std::move(h));
        const edit::diff_apply_result r1 =
            edit::apply_diff_hunks(hunks, "a\nb\n");
        expect(eq(r1.content, kimix::string("a\nB\n")));
        expect(eq(*r1.first_changed_line, 2));
        const edit::diff_apply_result r2 = edit::apply_diff_hunks(hunks, "a\nb");
        expect(eq(r2.content, kimix::string("a\nB")));
        expect(eq(*r2.first_changed_line, 2));
    };

    "diff_apply_all_add_hunk_at_top"_test = [] {
        kimix::vector<edit::diff_hunk> hunks;
        edit::diff_hunk h;
        h.lines.push_back({edit::hunk_line_kind::add, "z"});
        hunks.push_back(std::move(h));
        const edit::diff_apply_result r =
            edit::apply_diff_hunks(hunks, "a\nb\n");
        expect(!r.error.failed());
        expect(eq(r.content, kimix::string("z\na\nb\n")));
        expect(eq(*r.first_changed_line, 1));
    };

    // ------------------------------------------------------------------
    // Hashline kernels
    // ------------------------------------------------------------------
    "hashline_compute_line_hash_goldens"_test = [] {
        expect(eq(edit::compute_line_hash(1, "hello", std::nullopt), kimix::string("HK")));
        const kimix::string h1 = edit::compute_line_hash(1, "hello", std::nullopt);
        expect(eq(edit::compute_line_hash(2, "world", h1), kimix::string("WV")));
        expect(eq(edit::compute_line_hash(1, spaces(3), std::nullopt), kimix::string("KM")));
        expect(eq(edit::compute_line_hash(2, spaces(3), "AA"), kimix::string("TJ")));
        expect(eq(edit::compute_line_hash(1, "hello\r", std::nullopt), kimix::string("HK")));
        expect(eq(edit::compute_line_hash(1, "\xC3\xA9\xC3\xA9", std::nullopt), kimix::string("ZX")));
        expect(eq(edit::compute_line_hash(1, "", "AB"), kimix::string("ZK")));
    };

    "hashline_parse_sections_and_ops"_test = [] {
        const edit::parse_hashline_result r = edit::parse_hashline_input(
            "*** preamble\n"
            "[a.txt#AB]\n"
            "PUT 1.3:\n"
            "+new1\n"
            "+new2\n"
            "CUT 2.4 @reg\n"
            "[b.txt#CD]\n"
            "REM\n"
            "*** postamble");
        expect(!r.error.failed());
        expect(eq(r.sections.size(), size_t(2)));
        expect(eq(r.sections[0].path, kimix::string("a.txt")));
        expect(eq(r.sections[0].tag, kimix::string("AB")));
        expect(eq(r.sections[0].ops.size(), size_t(2)));
        expect(eq(static_cast<int>(r.sections[0].ops[0].kind), static_cast<int>(edit::hashline_op_kind::put)));
        expect(eq(*r.sections[0].ops[0].start, 1));
        expect(eq(*r.sections[0].ops[0].end, 3));
        expect(eq(static_cast<int>(r.sections[0].ops[0].insert_where_), static_cast<int>(edit::insert_where::replace)));
        expect(!r.sections[0].ops[0].register_.has_value());
        expect(eq(r.sections[0].ops[0].body.size(), size_t(2)));
        expect(eq(r.sections[0].ops[0].body[0], kimix::string("new1")));
        expect(eq(static_cast<int>(r.sections[0].ops[1].kind), static_cast<int>(edit::hashline_op_kind::cut)));
        expect(eq(*r.sections[0].ops[1].start, 2));
        expect(eq(*r.sections[0].ops[1].end, 4));
        expect(eq(r.sections[0].ops[1].register_.has_value(), true));
        expect(eq(*r.sections[0].ops[1].register_, kimix::string("reg")));
        expect(eq(r.sections[1].ops.size(), size_t(1)));
        expect(eq(static_cast<int>(r.sections[1].ops[0].kind), static_cast<int>(edit::hashline_op_kind::rem)));
    };

    "hashline_parse_before_after_paste_mv"_test = [] {
        const edit::parse_hashline_result r = edit::parse_hashline_input(
            "[a.txt#AB]\n"
            "PUT <3:\n"
            "+pre\n"
            "PUT >5 @r\n"
            "[c.txt#XY]\n"
            "MV other.txt\n");
        expect(!r.error.failed());
        expect(eq(r.sections.size(), size_t(2)));
        const auto &ops = r.sections[0].ops;
        expect(eq(ops.size(), size_t(2)));
        expect(eq(static_cast<int>(ops[0].kind), static_cast<int>(edit::hashline_op_kind::put)));
        expect(eq(*ops[0].start, 3));
        expect(eq(static_cast<int>(ops[0].insert_where_), static_cast<int>(edit::insert_where::before)));
        expect(eq(ops[0].body.size(), size_t(1)));
        expect(eq(ops[0].body[0], kimix::string("pre")));
        expect(eq(static_cast<int>(ops[1].kind), static_cast<int>(edit::hashline_op_kind::put)));
        expect(eq(*ops[1].start, 5));
        expect(eq(static_cast<int>(ops[1].insert_where_), static_cast<int>(edit::insert_where::after)));
        expect(ops[1].register_.has_value());
        expect(eq(*ops[1].register_, kimix::string("r")));
        expect(eq(static_cast<int>(r.sections[1].ops[0].kind), static_cast<int>(edit::hashline_op_kind::mv)));
        expect(r.sections[1].ops[0].dest.has_value());
        expect(eq(*r.sections[1].ops[0].dest, kimix::string("other.txt")));
    };

    "hashline_parse_errors"_test = [] {
        edit::parse_hashline_result r = edit::parse_hashline_input("");
        expect(r.error.failed());
        expect(eq(r.error.message, kimix::string("No `[path#tag]` hashline sections found in input.")));

        r = edit::parse_hashline_input("[a.txt#AB]\ngarbage\n");
        expect(r.error.failed());
        expect(eq(r.error.message, kimix::string("Unexpected hashline line: 'garbage'. "
                                                 "Expected a section header `[path#tag]`, a `PUT ...:`, "
                                                 "`CUT ...`, `REM`, or `MV ...`.")));

        r = edit::parse_hashline_input("[a.txt#AB]\nPUT 1.1:\n-bad\n");
        expect(r.error.failed());
        expect(eq(r.error.message, kimix::string("Body rows under `PUT 1.1:` must start with `+`; "
                                                 "rejecting `-` row: '-bad'")));

        r = edit::parse_hashline_input("[a.txt#AB]\nPUT 1.1:\ntext\n");
        expect(r.error.failed());
        expect(eq(r.error.message, kimix::string("Body rows under `PUT 1.1:` must start with `+`; "
                                                 "got: 'text'")));
    };

    "hashline_apply_replace_line"_test = [] {
        const kimix::string h1 = edit::compute_line_hash(1, "line1", std::nullopt);
        const kimix::string h2 = edit::compute_line_hash(2, "line2", h1);
        kimix::vector<edit::hashline_edit> edits;
        edit::hashline_edit e;
        e.op = "replace";
        e.pos = edit::anchor_ref{2, h2};
        e.end = std::nullopt;
        e.lines.push_back("LINE2");
        edits.push_back(std::move(e));
        const edit::apply_hashline_result r =
            edit::apply_hashline_edits("line1\nline2\nline3\n", edits);
        expect(!r.error.failed());
        expect(eq(r.content, kimix::string("line1\nLINE2\nline3\n")));
        expect(eq(*r.first_changed_line, 2));
    };

    "hashline_apply_append_prepend"_test = [] {
        kimix::vector<edit::hashline_edit> edits;
        edit::hashline_edit e;
        e.op = "append";
        e.pos = std::nullopt;
        e.lines.push_back("appended");
        edits.push_back(std::move(e));
        edit::apply_hashline_result r = edit::apply_hashline_edits("line1\n", edits);
        expect(!r.error.failed());
        expect(eq(r.content, kimix::string("line1\nappended\n")));
        expect(eq(*r.first_changed_line, 2));

        edits.clear();
        edit::hashline_edit p;
        p.op = "prepend";
        p.pos = std::nullopt;
        p.lines.push_back("pre");
        edits.push_back(std::move(p));
        r = edit::apply_hashline_edits("line1\n", edits);
        expect(!r.error.failed());
        expect(eq(r.content, kimix::string("pre\nline1\n")));
        expect(eq(*r.first_changed_line, 1));
    };

    "hashline_apply_empty_and_single_empty_line"_test = [] {
        kimix::vector<edit::hashline_edit> edits;
        edit::hashline_edit e;
        e.op = "append";
        e.pos = std::nullopt;
        e.lines.push_back("a");
        edits.push_back(std::move(e));
        edit::apply_hashline_result r = edit::apply_hashline_edits("", edits);
        expect(eq(r.content, kimix::string("a")));
        expect(eq(*r.first_changed_line, 1));

        r = edit::apply_hashline_edits("\n", edits);
        expect(eq(r.content, kimix::string("a\n")));
        expect(eq(*r.first_changed_line, 1));

        const edit::apply_hashline_result none = edit::apply_hashline_edits("x\n", {});
        expect(eq(none.content, kimix::string("x\n")));
        expect(!none.first_changed_line.has_value());
    };

    "hashline_apply_delete_line_and_noop_append"_test = [] {
        const kimix::string h1 = edit::compute_line_hash(1, "l1", std::nullopt);
        const kimix::string h2 = edit::compute_line_hash(2, "l2", h1);
        kimix::vector<edit::hashline_edit> edits;
        edit::hashline_edit e;
        e.op = "replace";
        e.pos = edit::anchor_ref{2, h2};
        e.end = std::nullopt;
        edits.push_back(std::move(e)); // replace with empty body = delete line
        const edit::apply_hashline_result r =
            edit::apply_hashline_edits("l1\nl2\nl3\n", edits);
        expect(!r.error.failed());
        expect(eq(r.content, kimix::string("l1\nl3\n")));
        expect(eq(*r.first_changed_line, 2));

        edits.clear();
        edit::hashline_edit ap;
        ap.op = "append";
        ap.pos = edit::anchor_ref{1, h1};
        // empty body -> no-op, no first-changed
        edits.push_back(std::move(ap));
        const edit::apply_hashline_result r2 =
            edit::apply_hashline_edits("l1\nl2\nl3\n", edits);
        expect(eq(r2.content, kimix::string("l1\nl2\nl3\n")));
        expect(!r2.first_changed_line.has_value());
    };

    "hashline_apply_anchor_mismatch_message"_test = [] {
        kimix::vector<edit::hashline_edit> edits;
        edit::hashline_edit e;
        e.op = "replace";
        e.pos = edit::anchor_ref{1, "ZZ"};
        e.end = std::nullopt;
        e.lines.push_back("X");
        edits.push_back(std::move(e));
        const edit::apply_hashline_result r = edit::apply_hashline_edits("line1\n", edits);
        expect(r.error.failed());
        expect(eq(static_cast<int>(r.error.status), static_cast<int>(tool_status::invalid_input)));
        expect(!r.mismatches.empty());
        expect(eq(r.mismatches[0].line, 1));
        expect(eq(r.mismatches[0].expected, kimix::string("ZZ")));
        expect(eq(r.mismatches[0].actual, kimix::string("ZM")));
        expect(eq(r.error.message, kimix::string("1 line have changed since last read. "
                                                 "Use the updated LINE#ID references shown below "
                                                 "(>>> marks changed lines).\n"
                                                 "\n"
                                                 ">>> 1#ZM:line1")));
    };

    "hashline_apply_cr_stripped_fuzzy_fallback"_test = [] {
        // Anchor hash computed against the \r-stripped line 1.
        const kimix::string h1 = edit::compute_line_hash(1, "l1", std::nullopt);
        kimix::vector<edit::hashline_edit> edits;
        edit::hashline_edit e;
        e.op = "replace";
        e.pos = edit::anchor_ref{1, h1};
        e.end = std::nullopt;
        e.lines.push_back("X");
        edits.push_back(std::move(e));
        const edit::apply_hashline_result r =
            edit::apply_hashline_edits("l1\r\nl2\r\n", edits);
        expect(!r.error.failed());
        expect(eq(r.content, kimix::string("X\nl2\n")));
        expect(eq(*r.first_changed_line, 1));
    };

    "hashline_apply_validation_errors"_test = [] {
        kimix::vector<edit::hashline_edit> edits;
        edit::hashline_edit e;
        e.op = "replace";
        e.pos = edit::anchor_ref{3, "AA"};
        e.end = edit::anchor_ref{2, "BB"};
        e.lines.push_back("X");
        edits.push_back(std::move(e));
        edit::apply_hashline_result r =
            edit::apply_hashline_edits("l1\nl2\nl3\n", edits);
        expect(r.error.failed());
        expect(eq(r.error.message, kimix::string("Range start line 3 must be <= end line 2")));

        edits.clear();
        edit::hashline_edit e2;
        e2.op = "replace";
        e2.pos = edit::anchor_ref{99, "AA"};
        e2.end = std::nullopt;
        e2.lines.push_back("X");
        edits.push_back(std::move(e2));
        r = edit::apply_hashline_edits("l1\n", edits);
        expect(r.error.failed());
        expect(eq(r.error.message, kimix::string("Line 99 does not exist (file has 1 lines)")));

        edits.clear();
        edit::hashline_edit e3;
        e3.op = "replace";
        e3.pos = edit::anchor_ref{0, "AA"};
        e3.end = std::nullopt;
        e3.lines.push_back("X");
        edits.push_back(std::move(e3));
        r = edit::apply_hashline_edits("l1\n", edits);
        expect(r.error.failed());
        expect(eq(r.error.message, kimix::string("Line 0 must be >= 1")));
    };

    "hashline_apply_overlap_errors"_test = [] {
        const kimix::string h1 = edit::compute_line_hash(1, "l1", std::nullopt);
        const kimix::string h2 = edit::compute_line_hash(2, "l2", h1);
        const kimix::string h3 = edit::compute_line_hash(3, "l3", h2);
        kimix::vector<edit::hashline_edit> edits;
        edit::hashline_edit e1;
        e1.op = "replace";
        e1.pos = edit::anchor_ref{1, h1};
        e1.end = edit::anchor_ref{2, h2};
        e1.lines.push_back("X");
        edit::hashline_edit e2;
        e2.op = "replace";
        e2.pos = edit::anchor_ref{2, h2};
        e2.end = edit::anchor_ref{3, h3};
        e2.lines.push_back("Y");
        edits.push_back(std::move(e1));
        edits.push_back(std::move(e2));
        const edit::apply_hashline_result r =
            edit::apply_hashline_edits("l1\nl2\nl3\n", edits);
        expect(r.error.failed());
        expect(eq(r.error.message, kimix::string("Overlapping edits detected. "
                                                 "Combine overlapping edits into a single operation:\n"
                                                 "  - replace at lines 1-2 overlaps with replace at lines 2-3")));
    };

    "hashline_apply_dedupe_identical_edits"_test = [] {
        const kimix::string h1 = edit::compute_line_hash(1, "l1", std::nullopt);
        kimix::vector<edit::hashline_edit> edits;
        edit::hashline_edit e1;
        e1.op = "replace";
        e1.pos = edit::anchor_ref{1, h1};
        e1.end = std::nullopt;
        e1.lines.push_back("X");
        edit::hashline_edit e2 = e1;
        edits.push_back(e1);
        edits.push_back(e2);
        const edit::apply_hashline_result r =
            edit::apply_hashline_edits("l1\nl2\n", edits);
        expect(!r.error.failed());
        expect(eq(r.content, kimix::string("X\nl2\n")));
        expect(eq(*r.first_changed_line, 1));
    };

    "hashline_mismatch_message_multi_context"_test = [] {
        // Two mismatch lines at 1 and 4 in a 10-line file -> context 1..6.
        kimix::vector<edit::hash_mismatch> mismatches;
        mismatches.push_back({1, "ZZ", "YM"});
        mismatches.push_back({4, "ZZ", "PJ"});
        kimix::vector<kimix::string> lines = {
            "l1", "l2", "l3", "l4", "l5", "l6", "l7", "l8", "l9", "l10"};
        const kimix::string msg =
            edit::hashline_mismatch_message(mismatches, lines);
        expect(eq(msg, kimix::string("2 lines have changed since last read. "
                                     "Use the updated LINE#ID references shown below "
                                     "(>>> marks changed lines).\n"
                                     "\n"
                                     ">>> 1#YM:l1\n"
                                     "    2#NQ:l2\n"
                                     "    3#YW:l3\n"
                                     ">>> 4#PJ:l4\n"
                                     "    5#SM:l5\n"
                                     "    6#VM:l6")));
    };

    // ------------------------------------------------------------------
    // Sloppy kernels
    // ------------------------------------------------------------------
    "sloppy_parse_sections_and_modes"_test = [] {
        const edit::parse_sloppy_result r = edit::parse_sloppy_input(
            "\xC2\xA7"
            "foo.txt\n"
            "MATCH\n"
            "\xC2\xBB\n"
            "REWRITE\n"
            "\xC2\xA7"
            "bar.txt\n"
            "line1\n");
        expect(!r.error.failed());
        expect(eq(r.ops.size(), size_t(2)));
        expect(eq(r.ops[0].path, kimix::string("foo.txt")));
        expect(!r.ops[0].all_match);
        expect(r.ops[0].rewrite_lines.has_value());
        expect(eq(r.ops[0].match_lines.size(), size_t(1)));
        expect(eq(r.ops[0].match_lines[0], kimix::string("MATCH")));
        expect(eq(r.ops[0].rewrite_lines->size(), size_t(1)));
        expect(eq((*r.ops[0].rewrite_lines)[0], kimix::string("REWRITE")));
        expect(eq(r.ops[1].path, kimix::string("bar.txt")));
        expect(!r.ops[1].rewrite_lines.has_value());
        expect(eq(r.ops[1].inline_lines.size(), size_t(1)));
        expect(eq(r.ops[1].inline_lines[0].line, kimix::string("line1")));
        expect(eq(r.ops[1].inline_lines[0].selections.size(), size_t(0)));
    };

    "sloppy_parse_all_match_and_path_inheritance"_test = [] {
        const edit::parse_sloppy_result r = edit::parse_sloppy_input(
            "\xC2\xA7*"
            "a.txt\n"
            "x\n"
            "\xC2\xA7\n"
            " y \n");
        expect(!r.error.failed());
        expect(eq(r.ops.size(), size_t(2)));
        expect(r.ops[0].all_match);
        expect(eq(r.ops[0].path, kimix::string("a.txt")));
        expect(eq(r.ops[1].path, kimix::string("a.txt")));
        expect(eq(r.ops[1].inline_lines[0].line, kimix::string(" y ")));
    };

    "sloppy_parse_inline_selections_rescan"_test = [] {
        const edit::parse_sloppy_result r = edit::parse_sloppy_input(
            "\xC2\xA7"
            "f.txt\n"
            "line \xE2\x9F\xAA"
            "old1\xE2\x94\x82"
            "new1\xE2\x9F\xAB"
            " and \xE2\x9F\xAA"
            "old2\xE2\x94\x82"
            "new2\xE2\x9F\xAB\n");
        expect(!r.error.failed());
        expect(eq(r.ops.size(), size_t(1)));
        const auto &sels = r.ops[0].inline_lines[0].selections;
        expect(eq(sels.size(), size_t(2)));
        expect(eq(sels[0].old_text, kimix::string("old1")));
        expect(eq(sels[0].new_text, kimix::string("new1")));
        expect(eq(sels[1].old_text, kimix::string("old2")));
        expect(eq(sels[1].new_text, kimix::string("new2")));
    };

    "sloppy_parse_errors"_test = [] {
        edit::parse_sloppy_result r = edit::parse_sloppy_input("no section here\n");
        expect(r.error.failed());
        expect(eq(r.error.message, kimix::string("No sloppy operations found. Input must start with `\xC2\xA7path`.")));
        r = edit::parse_sloppy_input("\xC2\xA7\nx\n");
        expect(r.error.failed());
        expect(eq(r.error.message, kimix::string("Bare `\xC2\xA7` requires a previous section with a path.")));
    };

    "sloppy_block_exact_and_deletion_swallow"_test = [] {
        edit::sloppy_op op;
        op.path = "f";
        op.match_lines = {"b", "c"};
        op.rewrite_lines = kimix::vector<kimix::string>{"B", "C"};
        edit::sloppy_apply_result r = edit::apply_block_op("a\nb\nc\nd\n", op);
        expect(!r.error.failed());
        expect(eq(r.content, kimix::string("a\nB\nC\nd\n")));

        op.rewrite_lines = kimix::vector<kimix::string>();
        r = edit::apply_block_op("a\nb\nc\nd\n", op);
        expect(!r.error.failed());
        expect(eq(r.content, kimix::string("a\nd\n")));
    };

    "sloppy_block_all_match_non_overlapping"_test = [] {
        edit::sloppy_op op;
        op.path = "f";
        op.all_match = true;
        op.match_lines = {"ab"};
        op.rewrite_lines = kimix::vector<kimix::string>{"X"};
        const edit::sloppy_apply_result r = edit::apply_block_op("ababab", op);
        expect(!r.error.failed());
        expect(eq(r.content, kimix::string("XXX")));
    };

    "sloppy_block_fuzzy_and_errors"_test = [] {
        const edit::fuzzy_block_result fz = edit::find_fuzzy_block(
            "the quick brown fox\njumps over\nlazy dog\n",
            kimix::vector<kimix::string>{"The quick brown fox"});
        expect(!fz.error.failed());
        expect(fz.range.has_value());
        expect(eq(fz.range->begin, uint64_t(0)));
        expect(eq(fz.range->end, uint64_t(19)));

        edit::sloppy_op op;
        op.path = "f";
        op.match_lines = {"nope"};
        op.rewrite_lines = kimix::vector<kimix::string>{"X"};
        edit::sloppy_apply_result r = edit::apply_block_op("zzz\n", op);
        expect(r.error.failed());
        expect(eq(r.error.message, kimix::string("Could not locate MATCH block:\nnope")));
        op.match_lines = {};
        r = edit::apply_block_op("zzz\n", op);
        expect(r.error.failed());
        expect(eq(r.error.message, kimix::string("MATCH block is empty.")));
    };

    "sloppy_inline_apply_first_and_all"_test = [] {
        edit::sloppy_op op;
        op.path = "f";
        edit::sloppy_inline_line entry;
        entry.line = "x old y";
        entry.selections.push_back({"old", "new"});
        op.inline_lines.push_back(std::move(entry));
        edit::sloppy_apply_result r = edit::apply_inline_op("x old y\n", op);
        expect(!r.error.failed());
        expect(eq(r.content, kimix::string("x new y\n")));

        edit::sloppy_op all;
        all.path = "f";
        all.all_match = true;
        edit::sloppy_inline_line all_entry;
        all_entry.line = "l";
        all_entry.selections.push_back({"a", "b"});
        all.inline_lines.push_back(std::move(all_entry));
        r = edit::apply_inline_op("a a a", all);
        expect(eq(r.content, kimix::string("b b b")));

        edit::sloppy_op miss;
        miss.path = "f";
        edit::sloppy_inline_line miss_entry;
        miss_entry.line = "l";
        miss_entry.selections.push_back({"zz", "q"});
        miss.inline_lines.push_back(std::move(miss_entry));
        r = edit::apply_inline_op("abc\n", miss);
        expect(r.error.failed());
        expect(eq(r.error.message, kimix::string("Could not locate inline selection: 'zz'")));
    };

    "sloppy_dispatch"_test = [] {
        edit::sloppy_op block;
        block.path = "f";
        block.match_lines = {"b"};
        block.rewrite_lines = kimix::vector<kimix::string>{"B"};
        edit::sloppy_apply_result r =
            edit::apply_sloppy_op("a\nb\nc\n", block);
        expect(eq(r.content, kimix::string("a\nB\nc\n")));

        edit::sloppy_op inl;
        inl.path = "f";
        edit::sloppy_inline_line entry;
        entry.line = "l";
        entry.selections.push_back({"b", "B"});
        inl.inline_lines.push_back(std::move(entry));
        r = edit::apply_sloppy_op("a\nb\nc\n", inl);
        expect(eq(r.content, kimix::string("a\nB\nc\n")));
    };
}
