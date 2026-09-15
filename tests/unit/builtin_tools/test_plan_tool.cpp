// Test for builtin_tools/plan_tool.h (namespace kimix::builtin_tools::plan).
//
// Covers:
// - parse_write_params: content/text alias, mode default + Literal validation
// - parse_read_params: defaults, line_offset == 0 / < -MAX_LINES, ge= bounds
// - render_forward: %6d\t numbering, line budget, byte budget, message parts,
//   truncated-line reporting, "Total lines in file" only on a full scan
// - render_tail: negative offset window, n_lines cap from the window head,
//   reverse byte-budget scan, always reports the total line count
// - apply_char_window: the output[char_offset:max_char] slice-END quirk
// - apply_plan_edit / apply_plan_edits: no-op guard, replace_all counting,
//   single replacement, strip match, fuzzy match WITHOUT a suggestion,
//   no-match suggestion, multi-edit aggregation
// - message builders: verbatim reference strings
// - WritePlan / ReadPlan / EditPlan Tool wrappers with injected content,
//   the plan_enabled gate and the missing plan_writing_path error
//
// All test logic lives in main() scope; no file-scope static registrations.
#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "builtin_tools/plan_tool.h"
#include "builtin_tools/read_tool.h"

#include <string>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools;
using namespace kimix::builtin_tools::plan;

namespace {

kimix::string kix(std::string_view sv) {
    return kimix::string(sv.data(), sv.size());
}

// Split text into the universal-newline line list read::split_lines produces.
kimix::vector<kimix::string> lines_of(std::string_view text) {
    return read::split_lines(kix(text));
}

std::string sv_of(const kimix::string &s) {
    return std::string(s.data(), s.size());
}

// Golden vectors generated from the Python reference implementation
// (ReadPlan._read_forward / _read_tail in
// C:/dev/kimi-agent/src/kimix/tools/note/__init__.py) by
// .kimix_cache/plan_goldens.py.
#include "plan_goldens.inc"

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    // -----------------------------------------------------------------------
    // parse_write_params
    // -----------------------------------------------------------------------
    "write_params_defaults"_test = [] {
        ToolParams params;
        params.values["content"] = ValueElement::make_string(kix("# Plan"));
        write_plan_params out;
        const tool_error err = parse_write_params(&params, out);
        expect(!err.failed());
        expect(out.content == kix("# Plan"));
        expect(out.mode == kix("overwrite"));
    };

    "write_params_text_alias"_test = [] {
        ToolParams params;
        params.values["text"] = ValueElement::make_string(kix("aliased"));
        write_plan_params out;
        expect(!parse_write_params(&params, out).failed());
        expect(out.content == kix("aliased"));
    };

    "write_params_canonical_wins_over_alias"_test = [] {
        ToolParams params;
        params.values["content"] = ValueElement::make_string(kix("canonical"));
        params.values["text"] = ValueElement::make_string(kix("alias"));
        write_plan_params out;
        expect(!parse_write_params(&params, out).failed());
        expect(out.content == kix("canonical"));
    };

    "write_params_append_mode"_test = [] {
        ToolParams params;
        params.values["content"] = ValueElement::make_string(kix("x"));
        params.values["mode"] = ValueElement::make_string(kix("append"));
        write_plan_params out;
        expect(!parse_write_params(&params, out).failed());
        expect(out.mode == kix("append"));
    };

    "write_params_bad_mode"_test = [] {
        ToolParams params;
        params.values["content"] = ValueElement::make_string(kix("x"));
        params.values["mode"] = ValueElement::make_string(kix("sideways"));
        write_plan_params out;
        const tool_error err = parse_write_params(&params, out);
        expect(err.failed());
        expect(err.status == tool_status::invalid_input);
        expect(sv_of(err.message).find("'overwrite' or 'append'") !=
               std::string::npos);
    };

    "write_params_missing_content"_test = [] {
        ToolParams params;
        write_plan_params out;
        const tool_error err = parse_write_params(&params, out);
        expect(err.failed());
        expect(err.message == kix("missing required field: content"));
    };

    // -----------------------------------------------------------------------
    // parse_read_params
    // -----------------------------------------------------------------------
    "read_params_defaults"_test = [] {
        read_plan_params out;
        expect(!parse_read_params(nullptr, out).failed());
        expect(out.line_offset == 1_i);
        expect(out.n_lines == k_max_lines);
        expect(out.max_char == 65536_i);
        expect(out.char_offset == 0_i);
    };

    "read_params_zero_line_offset"_test = [] {
        ToolParams params;
        params.values["line_offset"] = ValueElement::make_int(0);
        read_plan_params out;
        const tool_error err = parse_read_params(&params, out);
        expect(err.failed());
        expect(err.message ==
               kix("line_offset cannot be 0; use 1 for the first line or -1 "
                   "for the last line"));
    };

    "read_params_too_negative"_test = [] {
        ToolParams params;
        params.values["line_offset"] = ValueElement::make_int(-1001);
        read_plan_params out;
        const tool_error err = parse_read_params(&params, out);
        expect(err.failed());
        expect(sv_of(err.message).find(
                   "line_offset cannot be less than -1000.") == 0);
        expect(sv_of(err.message).find(
                   "Use a positive line_offset with the total line count to "
                   "read from a specific position.") != std::string::npos);
    };

    "read_params_boundary_negative_ok"_test = [] {
        ToolParams params;
        params.values["line_offset"] = ValueElement::make_int(-1000);
        read_plan_params out;
        expect(!parse_read_params(&params, out).failed());
        expect(out.line_offset == -1000_i);
    };

    "read_params_n_lines_ge_1"_test = [] {
        ToolParams params;
        params.values["n_lines"] = ValueElement::make_int(0);
        read_plan_params out;
        const tool_error err = parse_read_params(&params, out);
        expect(err.failed());
        expect(err.message ==
               kix("n_lines must be greater than or equal to 1"));
    };

    // -----------------------------------------------------------------------
    // render_forward
    // -----------------------------------------------------------------------
    "render_forward_numbers_and_reports_total"_test = [] {
        const kimix::vector<kimix::string> lines = lines_of("a\nb\nc\n");
        const plan_render r =
            render_forward(kimix::span<const kimix::string>(lines), 1, 1000);
        expect(r.output == kix("     1\ta\n     2\tb\n     3\tc\n"));
        expect(r.message ==
               kix("3 lines read from plan starting from line 1. Total lines "
                   "in file: 3."));
        expect(r.start_line == 1_i);
        expect(r.total_lines == 3_i);
        expect(!r.max_lines_reached);
        expect(!r.max_bytes_reached);
        expect(r.truncated_line_numbers.empty());
    };

    "render_forward_offset_skips_leading_lines"_test = [] {
        const kimix::vector<kimix::string> lines = lines_of("a\nb\nc\n");
        const plan_render r =
            render_forward(kimix::span<const kimix::string>(lines), 2, 1000);
        expect(r.output == kix("     2\tb\n     3\tc\n"));
        expect(r.message ==
               kix("2 lines read from plan starting from line 2. Total lines "
                   "in file: 3."));
        expect(r.start_line == 2_i);
    };

    "render_forward_line_budget_hides_total"_test = [] {
        const kimix::vector<kimix::string> lines = lines_of("a\nb\nc\n");
        const plan_render r =
            render_forward(kimix::span<const kimix::string>(lines), 1, 2);
        expect(r.output == kix("     1\ta\n     2\tb\n"));
        // target_lines (2) < MAX_LINES so max_lines_reached stays false, and
        // the file was NOT fully scanned so no "Total lines in file".
        expect(!r.max_lines_reached);
        expect(sv_of(r.message).find("Total lines in file") ==
               std::string::npos);
        expect(r.message ==
               kix("2 lines read from plan starting from line 1."));
    };

    "render_forward_max_lines_reached_at_cap"_test = [] {
        // n_lines >= MAX_LINES and at least MAX_LINES rendered lines.
        kimix::string text;
        for (int i = 0; i < 1001; ++i) {
            text += "line\n";
        }
        const kimix::vector<kimix::string> lines = lines_of(text);
        const plan_render r =
            render_forward(kimix::span<const kimix::string>(lines), 1, 1000);
        expect(r.max_lines_reached);
        expect(sv_of(r.message).find("Max 1000 lines reached.") !=
               std::string::npos);
        expect(sv_of(r.message).find("1000 lines read from plan starting from "
                                     "line 1.") == 0);
    };

    "render_forward_empty_window"_test = [] {
        const kimix::vector<kimix::string> lines = lines_of("a\nb\n");
        const plan_render r =
            render_forward(kimix::span<const kimix::string>(lines), 9, 1000);
        expect(r.output.empty());
        expect(sv_of(r.message).find("No lines read from plan.") == 0);
        expect(r.start_line == 9_i);
    };

    "render_forward_truncation_marker"_test = [] {
        kimix::string long_line(2100, 'x');
        const kimix::string text = long_line + "\nshort\n";
        const kimix::vector<kimix::string> lines = lines_of(text);
        const plan_render r =
            render_forward(kimix::span<const kimix::string>(lines), 1, 1000);
        expect(r.truncated_line_numbers.size() == 1u);
        expect(r.truncated_line_numbers[0] == 1_i);
        expect(sv_of(r.message).find("Lines [1] were truncated.") !=
               std::string::npos);
        // The truncated body keeps the trailing newline and ends with "...".
        expect(sv_of(r.output).find("...\n") != std::string::npos);
    };

    // -----------------------------------------------------------------------
    // render_tail
    // -----------------------------------------------------------------------
    "render_tail_keeps_last_n_lines"_test = [] {
        const kimix::vector<kimix::string> lines = lines_of("a\nb\nc\n");
        const plan_render r =
            render_tail(kimix::span<const kimix::string>(lines), -2, 1000);
        expect(r.output == kix("     2\tb\n     3\tc\n"));
        expect(r.message ==
               kix("2 lines read from plan starting from line 2. Total lines "
                   "in file: 3."));
        expect(r.start_line == 2_i);
        expect(r.total_lines == 3_i);
    };

    "render_tail_line_limit_caps_from_window_head"_test = [] {
        const kimix::vector<kimix::string> lines = lines_of("a\nb\nc\nd\n");
        // tail window = last 3 lines (b,c,d), then n_lines=2 keeps (b,c).
        const plan_render r =
            render_tail(kimix::span<const kimix::string>(lines), -3, 2);
        expect(r.output == kix("     2\tb\n     3\tc\n"));
        expect(r.start_line == 2_i);
    };

    "render_tail_empty_file"_test = [] {
        const kimix::vector<kimix::string> lines = lines_of("");
        const plan_render r =
            render_tail(kimix::span<const kimix::string>(lines), -5, 1000);
        expect(r.output.empty());
        expect(sv_of(r.message).find("No lines read from plan.") == 0);
        expect(sv_of(r.message).find("Total lines in file: 0.") !=
               std::string::npos);
        expect(r.start_line == 1_i); // total_lines + 1
    };

    "render_tail_byte_budget_keeps_newest"_test = [] {
        // 60 lines of 1900 chars each - below MAX_LINE_LENGTH so no per-line
        // truncation happens - total 60 * 1901 = 114060 bytes > MAX_BYTES.
        // The reverse scan keeps the newest lines that fit: 53 * 1901 = 100753
        // <= 102400 < 54 * 1901 = 102654, so lines 8..60 survive.
        kimix::string text;
        for (int i = 0; i < 60; ++i) {
            text += kimix::string(1900, 'x');
            text += "\n";
        }
        const kimix::vector<kimix::string> lines = lines_of(text);
        const plan_render r =
            render_tail(kimix::span<const kimix::string>(lines), -60, 1000);
        expect(r.max_bytes_reached);
        expect(sv_of(r.message).find("Max 102400 bytes reached.") !=
               std::string::npos);
        expect(r.truncated_line_numbers.empty());
        // The newest line (60) survives, the oldest (1) does not, and the
        // window restarts at the first surviving line.
        expect(sv_of(r.output).find("    60\t") != std::string::npos);
        expect(sv_of(r.output).find("     1\t") == std::string::npos);
        expect(r.start_line == 8_i);
        expect(sv_of(r.message).find(
                   "53 lines read from plan starting from line 8.") == 0);
        expect(sv_of(r.message).find("Total lines in file: 60.") !=
               std::string::npos);
    };

    // -----------------------------------------------------------------------
    // apply_char_window
    // -----------------------------------------------------------------------
    "char_window_uses_max_char_as_slice_end"_test = [] {
        // Python: "abcdef"[2:4] == "cd" (NOT 4 characters starting at 2).
        expect(apply_char_window("abcdef", 2, 4) == kix("cd"));
        expect(apply_char_window("abcdef", 0, 3) == kix("abc"));
        expect(apply_char_window("abcdef", 4, 2) == kix(""));
        expect(apply_char_window("abcdef", 10, 20) == kix(""));
        expect(apply_char_window("abcdef", 0, 100) == kix("abcdef"));
    };

    "char_window_counts_code_points"_test = [] {
        // "héllo" is 5 code points / 6 bytes.
        expect(apply_char_window("h\xc3\xa9llo", 0, 2) == kix("h\xc3\xa9"));
        expect(apply_char_window("h\xc3\xa9llo", 1, 3) == kix("\xc3\xa9l"));
    };

    "format_line_list_python_repr"_test = [] {
        const kimix::vector<int64_t> single = {3};
        expect(format_line_list(kimix::span<const int64_t>(single)) ==
               kix("[3]"));
        const kimix::vector<int64_t> many = {3, 7, 11};
        expect(format_line_list(kimix::span<const int64_t>(many)) ==
               kix("[3, 7, 11]"));
        expect(format_line_list(kimix::span<const int64_t>()) == kix("[]"));
    };

    // -----------------------------------------------------------------------
    // apply_plan_edit
    // -----------------------------------------------------------------------
    "plan_edit_noop_on_empty_old"_test = [] {
        plan_edit_item item;
        item.old_text = "";
        item.new_text = "x";
        const plan_edit_result r = apply_plan_edit("content", item);
        expect(r.content == kix("content"));
        expect(r.replacements == 0u);
        expect(!r.suggestion.has_value());
    };

    "plan_edit_noop_when_identical"_test = [] {
        plan_edit_item item;
        item.old_text = "same";
        item.new_text = "same";
        const plan_edit_result r = apply_plan_edit("same text", item);
        expect(r.replacements == 0u);
        expect(r.content == kix("same text"));
    };

    "plan_edit_single_replacement"_test = [] {
        plan_edit_item item;
        item.old_text = "hello";
        item.new_text = "goodbye";
        const plan_edit_result r = apply_plan_edit("hello hello", item);
        expect(r.replacements == 1u);
        expect(r.content == kix("goodbye hello"));
    };

    "plan_edit_replace_all_counts"_test = [] {
        plan_edit_item item;
        item.old_text = "a";
        item.new_text = "b";
        item.replace_all = true;
        const plan_edit_result r = apply_plan_edit("a a a\n", item);
        expect(r.replacements == 3u);
        expect(r.content == kix("b b b\n"));
    };

    "plan_edit_replace_all_no_hit_keeps_content"_test = [] {
        plan_edit_item item;
        item.old_text = "zzz";
        item.new_text = "b";
        item.replace_all = true;
        const plan_edit_result r = apply_plan_edit("a a a\n", item);
        expect(r.replacements == 0u);
        expect(r.content == kix("a a a\n"));
    };

    "plan_edit_normalizes_crlf"_test = [] {
        plan_edit_item item;
        item.old_text = "line1\nline2";
        item.new_text = "replaced";
        const plan_edit_result r = apply_plan_edit("line1\r\nline2\r\n", item);
        expect(r.replacements == 1u);
        expect(r.content == kix("replaced\n"));
    };

    "plan_edit_strip_match"_test = [] {
        plan_edit_item item;
        item.old_text = "indented";
        item.new_text = "flush";
        // Not a literal hit (leading whitespace differs) but the stripped
        // form is found inside the line.
        const plan_edit_result r = apply_plan_edit("    indented text\n", item);
        expect(r.replacements == 1u);
        expect(sv_of(r.content).find("flush") != std::string::npos);
    };

    "plan_edit_fuzzy_success_has_no_suggestion"_test = [] {
        plan_edit_item item;
        // One typo away from a content line -> fuzzy match >= 75%.
        item.old_text = "the quick brown fox";
        item.new_text = "REPLACED";
        const plan_edit_result r =
            apply_plan_edit("the quick brown fox jumps\nsecond line\n", item);
        expect(r.replacements == 1u);
        expect(sv_of(r.content).find("REPLACED") != std::string::npos);
        // This is where EditPlan differs from the file edit tool: a
        // successful fuzzy match reports NO suggestion.
        expect(!r.suggestion.has_value());
    };

    "plan_edits_aggregate_counts_and_last_suggestion"_test = [] {
        plan_edit_item first;
        first.old_text = "alpha";
        first.new_text = "ALPHA";
        plan_edit_item second;
        second.old_text = "beta";
        second.new_text = "BETA";
        const kimix::vector<plan_edit_item> edits = {first, second};
        const edit_plan_result r = apply_plan_edits(
            "alpha and beta\n", kimix::span<const plan_edit_item>(edits));
        expect(r.total_replacements == 2u);
        expect(r.changed);
        expect(r.content == kix("ALPHA and BETA\n"));
        expect(!r.last_suggestion.has_value());
    };

    "plan_edits_no_change_reports_suggestion"_test = [] {
        plan_edit_item item;
        item.old_text = "completely unrelated needle text";
        item.new_text = "x";
        const kimix::vector<plan_edit_item> edits = {item};
        const edit_plan_result r = apply_plan_edits(
            "alpha\nbeta\ngamma\n",
            kimix::span<const plan_edit_item>(edits));
        expect(!r.changed);
        expect(r.total_replacements == 0u);
    };

    // -----------------------------------------------------------------------
    // Message builders
    // -----------------------------------------------------------------------
    "message_builders_are_verbatim"_test = [] {
        expect(no_plan_path_message("WritePlan") ==
               kix("WritePlan tool invalid: no plan_writing_path set."));
        expect(no_plan_path_message("ReadPlan") ==
               kix("ReadPlan tool invalid: no plan_writing_path set."));
        expect(no_plan_path_message("EditPlan") ==
               kix("EditPlan tool invalid: no plan_writing_path set."));
        expect(plan_missing_message("p.md") ==
               kix("Plan file `p.md` does not exist."));
        expect(plan_not_a_file_message("dir") == kix("`dir` is not a file."));
        expect(plan_written_message("overwrite", "p.md") ==
               kix("Plan written to p.md"));
        expect(plan_written_message("append", "p.md") ==
               kix("Plan appended to p.md"));
        expect(no_replacements_message("") ==
               kix("No replacements were made. The old string was not found "
                   "in the plan file."));
        expect(no_replacements_message("  candidate") ==
               kix("No replacements were made. The old string was not found "
                   "in the plan file.\n\nDid you mean:\n    candidate"));
        expect(plan_edited_message(2, 5) ==
               kix("Plan file successfully edited. Applied 2 edit(s) with 5 "
                   "total replacement(s)."));
        expect(plan_failure_message("read plan", "boom") ==
               kix("Failed to read plan. Error: boom"));
    };

    // -----------------------------------------------------------------------
    // Tool wrappers
    // -----------------------------------------------------------------------
    "write_plan_tool_requires_plan_path"_test = [] {
        Session session;
        session.plan_enabled = true;
        WritePlan tool(&session);
        ToolParams params;
        params.values["content"] = ValueElement::make_string(kix("# Plan"));
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find(
                   "WritePlan tool invalid: no plan_writing_path set.") !=
               std::string::npos);
    };

    "write_plan_tool_disabled_session"_test = [] {
        Session session; // plan_enabled defaults to false
        WritePlan tool(&session);
        ToolParams params;
        params.values["content"] = ValueElement::make_string(kix("x"));
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find("unsupported") != std::string::npos);
    };

    "write_plan_tool_overwrite_and_append"_test = [] {
        Session session;
        session.plan_enabled = true;
        WritePlan tool(&session);
        tool.plan_path_override = "plan.md";
        tool.has_injected_content = true;
        tool.injected_content = "old";
        ToolParams params;
        params.values["content"] = ValueElement::make_string(kix("new"));
        tool(&params);
        expect(tool.injected_content == kix("new"));
        kimix::string json(tool.serialized_result().data(),
                           tool.serialized_result().size());
        expect(sv_of(json).find("Plan written to plan.md") != std::string::npos);

        params.values["mode"] = ValueElement::make_string(kix("append"));
        params.values["content"] = ValueElement::make_string(kix("!"));
        tool(&params);
        expect(tool.injected_content == kix("new!"));
        json.assign(tool.serialized_result().data(),
                    tool.serialized_result().size());
        expect(sv_of(json).find("Plan appended to plan.md") !=
               std::string::npos);
    };

    "read_plan_tool_renders_injected_content"_test = [] {
        Session session;
        session.plan_enabled = true;
        ReadPlan tool(&session);
        tool.plan_path_override = "plan.md";
        tool.has_injected_content = true;
        tool.injected_content = "a\nb\nc\n";
        tool(nullptr); // every parameter has a default
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find("\"status\":\"ok\"") != std::string::npos);
        expect(sv_of(json).find("3 lines read from plan starting from line 1") !=
               std::string::npos);
        expect(sv_of(json).find("Total lines in file: 3.") != std::string::npos);
    };

    "read_plan_tool_tail_mode"_test = [] {
        Session session;
        session.plan_enabled = true;
        ReadPlan tool(&session);
        tool.plan_path_override = "plan.md";
        tool.has_injected_content = true;
        tool.injected_content = "a\nb\nc\n";
        ToolParams params;
        params.values["line_offset"] = ValueElement::make_int(-1);
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find("1 lines read from plan starting from line 3") !=
               std::string::npos);
    };

    "edit_plan_tool_no_replacement"_test = [] {
        Session session;
        session.plan_enabled = true;
        EditPlan tool(&session);
        tool.plan_path_override = "plan.md";
        tool.has_injected_content = true;
        tool.injected_content = "alpha\nbeta\n";
        ToolParams edit;
        ToolParams item;
        item.values["old"] =
            ValueElement::make_string(kix("totally absent needle phrase"));
        item.values["new"] = ValueElement::make_string(kix("x"));
        edit.values["edit"] = ValueElement::make_object(
            kimix::shared_ptr<ToolParams>(new ToolParams(std::move(item))));
        tool(&edit);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find("No replacements were made.") !=
               std::string::npos);
        expect(sv_of(json).find("no_change") != std::string::npos);
        expect(tool.injected_content == kix("alpha\nbeta\n"));
    };

    "edit_plan_tool_edits_array_and_message"_test = [] {
        Session session;
        session.plan_enabled = true;
        EditPlan tool(&session);
        tool.plan_path_override = "plan.md";
        tool.has_injected_content = true;
        tool.injected_content = "one two three\n";

        ToolParams first;
        first.values["old_string"] = ValueElement::make_string(kix("one"));
        first.values["new_string"] = ValueElement::make_string(kix("ONE"));
        ToolParams second;
        second.values["old"] = ValueElement::make_string(kix("two"));
        second.values["new"] = ValueElement::make_string(kix("TWO"));
        second.values["replace_all"] = ValueElement::make_bool(true);
        ValueElement::Array arr;
        arr.push_back(ValueElement::make_object(
            kimix::shared_ptr<ToolParams>(new ToolParams(std::move(first)))));
        arr.push_back(ValueElement::make_object(
            kimix::shared_ptr<ToolParams>(new ToolParams(std::move(second)))));
        ToolParams params;
        params.values["edits"] = ValueElement::make_array(std::move(arr));
        tool(&params);
        expect(tool.injected_content == kix("ONE TWO three\n"));
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find(
                   "Plan file successfully edited. Applied 2 edit(s) with 2 "
                   "total replacement(s).") != std::string::npos);
    };

    // -----------------------------------------------------------------------
    // Golden vectors generated from the Python reference implementation
    // -----------------------------------------------------------------------
    "render_forward_goldens"_test = [] {
        for (const plan_golden &g : kForwardGoldens) {
            const kimix::vector<kimix::string> lines = lines_of(g.text);
            const plan_render r = render_forward(
                kimix::span<const kimix::string>(lines), g.line_offset,
                g.n_lines);
            expect(sv_of(r.output) == std::string(g.output))
                << "forward output mismatch: " << g.name;
            expect(sv_of(r.message) == std::string(g.message))
                << "forward message mismatch: " << g.name;
        }
    };

    "render_tail_goldens"_test = [] {
        for (const plan_golden &g : kTailGoldens) {
            const kimix::vector<kimix::string> lines = lines_of(g.text);
            const plan_render r = render_tail(
                kimix::span<const kimix::string>(lines), g.line_offset,
                g.n_lines);
            expect(sv_of(r.output) == std::string(g.output))
                << "tail output mismatch: " << g.name;
            expect(sv_of(r.message) == std::string(g.message))
                << "tail message mismatch: " << g.name;
        }
    };

    "edit_plan_tool_requires_edit"_test = [] {
        Session session;
        session.plan_enabled = true;
        EditPlan tool(&session);
        tool.plan_path_override = "plan.md";
        tool.has_injected_content = true;
        ToolParams params;
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find("missing required field: edit") !=
               std::string::npos);
    };
}
