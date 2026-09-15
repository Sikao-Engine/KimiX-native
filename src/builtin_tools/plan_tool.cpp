// plan_tool.cpp - C++ port of the kimi-agent plan-file tools (WritePlan,
// ReadPlan, EditPlan). See plan_tool.h for the reference line map.
//
// Unity-build rules (src/builtin_tools/README.md): every TU-local helper lives
// in an anonymous namespace inside kimix::builtin_tools::plan and carries the
// `pl_` prefix, so the concatenated kimix-llm translation unit cannot collide
// with the other tools' helpers.
//
// Reuse (never re-implemented here):
// * read::split_lines            - aiofiles universal-newline line iteration
// * read::truncate_line_read     - kimi_cli.tools.utils.truncate_line
// * edit::normalize_newlines     - EditPlan._normalize_line_endings
// * edit::find_similar           - EditPlan._find_similar
// * edit::try_strip_match        - EditPlan._try_strip_match
// * edit::best_fuzzy_match       - EditPlan._find_best_fuzzy_match
// The per-edit chain is NOT edit::apply_edit: EditPlan reports no suggestion
// when a fuzzy match succeeds (note/__init__.py 452-458), while the file edit
// tool answers "fuzzy-matched at NN%: '...'". apply_plan_edit below reuses the
// scoring kernels and implements EditPlan's exact return contract.
#include "builtin_tools/plan_tool.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>

#include "builtin_tools/edit_tool.h"
#include "builtin_tools/read_tool.h"
#include "builtin_tools/tool_registry.h"
#include "builtin_tools/utf8_util.h"

namespace kimix::builtin_tools::plan {

namespace {

// ---------------------------------------------------------------------------
// TU-local helpers (pl_ prefix - unity build safety)
// ---------------------------------------------------------------------------

const char *pl_status_string(tool_status status) noexcept {
    switch (status) {
    case tool_status::ok:
        return "ok";
    case tool_status::invalid_input:
        return "invalid_input";
    case tool_status::not_found:
        return "not_found";
    case tool_status::no_change:
        return "no_change";
    case tool_status::ambiguous:
        return "ambiguous";
    case tool_status::blocked:
        return "blocked";
    case tool_status::too_large:
        return "too_large";
    case tool_status::unsupported:
        return "unsupported";
    case tool_status::external_library:
        return "external_library";
    }
    return "unknown";
}

// Python str.count: non-overlapping occurrences scanning left to right.
size_t pl_count(kimix::string_view haystack, kimix::string_view needle) noexcept {
    if (needle.empty()) {
        // Python: "abc".count("") == len + 1. Not reachable from _apply_edit
        // (an empty `old` short-circuits first) but kept correct.
        return haystack.size() + 1;
    }
    size_t count = 0;
    size_t pos = haystack.find(needle);
    while (pos != kimix::string_view::npos) {
        ++count;
        pos = haystack.find(needle, pos + needle.size());
    }
    return count;
}

// Python str.replace(old, new) - every non-overlapping occurrence.
kimix::string pl_replace_all(kimix::string_view text, kimix::string_view old_text,
                             kimix::string_view new_text) {
    if (old_text.empty()) {
        return kimix::string(text);
    }
    kimix::string out;
    out.reserve(text.size());
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t hit = text.find(old_text, pos);
        if (hit == kimix::string_view::npos) {
            out.append(text.data() + pos, text.size() - pos);
            break;
        }
        out.append(text.data() + pos, hit - pos);
        out.append(new_text.data(), new_text.size());
        pos = hit + old_text.size();
    }
    return out;
}

// f"{line_no:6d}\t{truncated}" - right-aligned in a 6-char field.
void pl_append_numbered_line(kimix::string &out, int64_t line_no,
                             kimix::string_view body) {
    char prefix[32];
    const int written = std::snprintf(prefix, sizeof(prefix), "%6lld\t",
                                      static_cast<long long>(line_no));
    if (written > 0) {
        out.append(prefix, static_cast<size_t>(written));
    }
    out.append(body.data(), body.size());
}

// Assemble the ReadPlan message from its parts (" ".join(message_parts)).
kimix::string pl_build_message(size_t rendered, int64_t start_line,
                               bool with_total, int64_t total_lines,
                               bool max_lines_reached, bool max_bytes_reached,
                               kimix::span<const int64_t> truncated) {
    kimix::vector<kimix::string> parts;
    if (rendered > 0) {
        parts.push_back(kimix::format(
            "{} lines read from plan starting from line {}.", rendered,
            start_line));
    } else {
        parts.push_back(kimix::string("No lines read from plan."));
    }
    if (with_total) {
        parts.push_back(kimix::format("Total lines in file: {}.", total_lines));
    }
    if (max_lines_reached) {
        parts.push_back(kimix::format("Max {} lines reached.", k_max_lines));
    } else if (max_bytes_reached) {
        parts.push_back(kimix::format("Max {} bytes reached.", k_max_bytes));
    }
    if (!truncated.empty()) {
        const kimix::string listed = format_line_list(truncated);
        parts.push_back(kimix::format("Lines {} were truncated.",
                                      kimix::string_view(listed)));
    }
    kimix::string message;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            message += ' ';
        }
        message += parts[i];
    }
    return message;
}

// Read an int parameter with a default; rejects non-integer JSON types with
// the pydantic wording.
tool_error pl_int_param(const ToolParams *params, kimix::string_view name,
                        int64_t fallback, int64_t &out) {
    out = fallback;
    if (params == nullptr) {
        return {tool_status::ok, {}};
    }
    const ValueElement *el = params->get(name);
    if (el == nullptr || el->is_null()) {
        return {tool_status::ok, {}};
    }
    if (el->is_int()) {
        out = el->as_int();
        return {tool_status::ok, {}};
    }
    if (el->is_uint()) {
        out = static_cast<int64_t>(el->as_uint());
        return {tool_status::ok, {}};
    }
    if (el->is_real()) {
        out = static_cast<int64_t>(el->as_real());
        return {tool_status::ok, {}};
    }
    return {tool_status::invalid_input,
            kimix::format("{} must be an integer", name)};
}

// Read a string parameter, honouring one alias (populate_by_name semantics:
// the canonical name wins when both are present).
tool_error pl_string_param(const ToolParams *params, kimix::string_view name,
                           kimix::string_view alias, bool required,
                           kimix::string &out) {
    out.clear();
    if (params == nullptr) {
        return required ? tool_error{tool_status::invalid_input,
                                     kimix::format("missing required field: {}",
                                                   name)}
                        : tool_error{tool_status::ok, {}};
    }
    const ValueElement *el = params->get(name);
    if ((el == nullptr || !el->is_string()) && !alias.empty()) {
        el = params->get(alias);
    }
    if (el == nullptr || el->is_null()) {
        return required ? tool_error{tool_status::invalid_input,
                                     kimix::format("missing required field: {}",
                                                   name)}
                        : tool_error{tool_status::ok, {}};
    }
    if (!el->is_string()) {
        return {tool_status::invalid_input,
                kimix::format("{} must be a string", name)};
    }
    out = el->as_string();
    return {tool_status::ok, {}};
}

bool pl_bool_param(const ToolParams *params, kimix::string_view name,
                   bool fallback) {
    if (params == nullptr) {
        return fallback;
    }
    const ValueElement *el = params->get(name);
    if (el != nullptr && el->is_bool()) {
        return el->as_bool();
    }
    return fallback;
}

// One Edit object -> plan_edit_item (accepts old|old_string, new|new_string).
tool_error pl_parse_edit_object(const ToolParams &obj, plan_edit_item &out) {
    tool_error err = pl_string_param(&obj, "old", "old_string", true, out.old_text);
    if (err.failed()) {
        return err;
    }
    err = pl_string_param(&obj, "new", "new_string", true, out.new_text);
    if (err.failed()) {
        return err;
    }
    out.replace_all = pl_bool_param(&obj, "replace_all", false);
    return {tool_status::ok, {}};
}

// Resolve the effective plan path for a tool instance.
kimix::string pl_effective_path(const kimix::builtin_tools::Session *session,
                                kimix::string_view override_path) {
    if (!override_path.empty()) {
        return kimix::string(override_path);
    }
    if (session != nullptr) {
        return session->plan_path;
    }
    return {};
}

// Serialize one result envelope.
void pl_serialize(kimix::vector<char> &sink, tool_status status,
                  kimix::string_view message, kimix::string_view output,
                  kimix::string_view brief) {
    ToolParams result;
    result.values["ok"] = ValueElement::make_bool(status == tool_status::ok);
    result.values["status"] =
        ValueElement::make_string(kimix::string(pl_status_string(status)));
    result.values["message"] = ValueElement::make_string(kimix::string(message));
    result.values["output"] = ValueElement::make_string(kimix::string(output));
    result.values["brief"] = ValueElement::make_string(kimix::string(brief));
    sink.clear();
    result.serialize(sink);
}

// Whole-file read through stdio (the same approach read_tool/write_tool use).
tool_error pl_read_file(kimix::string_view path, kimix::string &out) {
    namespace fs = kimix::filesystem;
    const fs::path target = fs::path(kimix::string(path));
    std::error_code ec;
    if (!fs::exists(target, ec)) {
        return {tool_status::not_found, plan_missing_message(path)};
    }
    if (!fs::is_regular_file(target, ec)) {
        return {tool_status::invalid_input, plan_not_a_file_message(path)};
    }
    std::FILE *f = std::fopen(kimix::to_string(target).c_str(), "rb");
    if (f == nullptr) {
        return {tool_status::not_found,
                plan_failure_message("read plan", "cannot open plan file")};
    }
    out.clear();
    char buf[65536];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return {tool_status::ok, {}};
}

} // namespace

// ---------------------------------------------------------------------------
// Parameter parsing
// ---------------------------------------------------------------------------

tool_error parse_write_params(const ToolParams *params, write_plan_params &out) {
    out = write_plan_params{};
    tool_error err = pl_string_param(params, "content", "text", true, out.content);
    if (err.failed()) {
        return err;
    }
    kimix::string mode;
    err = pl_string_param(params, "mode", {}, false, mode);
    if (err.failed()) {
        return err;
    }
    if (mode.empty()) {
        mode = "overwrite";
    }
    if (mode != "overwrite" && mode != "append") {
        return {tool_status::invalid_input,
                kimix::format("Input should be 'overwrite' or 'append' "
                              "(mode={})",
                              kimix::string_view(mode))};
    }
    out.mode = std::move(mode);
    return {tool_status::ok, {}};
}

tool_error parse_read_params(const ToolParams *params, read_plan_params &out) {
    out = read_plan_params{};
    tool_error err =
        pl_int_param(params, "line_offset", 1, out.line_offset);
    if (err.failed()) {
        return err;
    }
    err = pl_int_param(params, "n_lines", k_max_lines, out.n_lines);
    if (err.failed()) {
        return err;
    }
    err = pl_int_param(params, "max_char", k_default_max_char, out.max_char);
    if (err.failed()) {
        return err;
    }
    err = pl_int_param(params, "char_offset", 0, out.char_offset);
    if (err.failed()) {
        return err;
    }
    // pydantic Field(ge=...) constraints.
    if (out.n_lines < 1) {
        return {tool_status::invalid_input,
                "n_lines must be greater than or equal to 1"};
    }
    if (out.max_char < 0) {
        return {tool_status::invalid_input,
                "max_char must be greater than or equal to 0"};
    }
    if (out.char_offset < 0) {
        return {tool_status::invalid_input,
                "char_offset must be greater than or equal to 0"};
    }
    // ReadPlanParams._validate_line_offset (107-119).
    if (out.line_offset == 0) {
        return {tool_status::invalid_input,
                "line_offset cannot be 0; use 1 for the first line or -1 for "
                "the last line"};
    }
    if (out.line_offset < -k_max_lines) {
        return {tool_status::invalid_input,
                kimix::format("line_offset cannot be less than -{}. Use a "
                              "positive line_offset with the total line count "
                              "to read from a specific position.",
                              k_max_lines)};
    }
    return {tool_status::ok, {}};
}

tool_error parse_edit_params(const ToolParams *params, edit_plan_params &out) {
    out = edit_plan_params{};
    if (params == nullptr) {
        return {tool_status::invalid_input, "missing required field: edit"};
    }
    const ValueElement *el = params->get("edit");
    if (el == nullptr || el->is_null()) {
        el = params->get("edits");
    }
    if (el == nullptr || el->is_null()) {
        return {tool_status::invalid_input, "missing required field: edit"};
    }
    if (el->is_object()) {
        const ToolParams *obj = el->as_object();
        if (obj == nullptr) {
            return {tool_status::invalid_input, "edit must be an object"};
        }
        plan_edit_item item;
        const tool_error err = pl_parse_edit_object(*obj, item);
        if (err.failed()) {
            return err;
        }
        out.edits.push_back(std::move(item));
        return {tool_status::ok, {}};
    }
    if (!el->is_array()) {
        return {tool_status::invalid_input,
                "edit must be an object or a list of objects"};
    }
    for (const ValueElement &entry : el->as_array()) {
        const ToolParams *obj = entry.as_object();
        if (obj == nullptr) {
            return {tool_status::invalid_input,
                    "edit must be an object or a list of objects"};
        }
        plan_edit_item item;
        const tool_error err = pl_parse_edit_object(*obj, item);
        if (err.failed()) {
            return err;
        }
        out.edits.push_back(std::move(item));
    }
    if (out.edits.empty()) {
        return {tool_status::invalid_input, "edit list must not be empty"};
    }
    return {tool_status::ok, {}};
}

// ---------------------------------------------------------------------------
// Render engine
// ---------------------------------------------------------------------------

plan_render render_forward(kimix::span<const kimix::string> lines,
                           int64_t line_offset, int64_t n_lines) {
    plan_render r;
    const int64_t target_lines = std::min(n_lines, k_max_lines);
    int64_t n_bytes = 0;
    int64_t current_line_no = 0;
    kimix::vector<kimix::string> rendered;
    for (const kimix::string &line : lines) {
        ++current_line_no;
        if (current_line_no < line_offset) {
            continue;
        }
        kimix::string truncated;
        read::truncate_line_read(line, k_max_line_length, truncated);
        if (truncated != line) {
            r.truncated_line_numbers.push_back(current_line_no);
        }
        n_bytes += static_cast<int64_t>(truncated.size());
        rendered.push_back(kimix::string());
        pl_append_numbered_line(rendered.back(), current_line_no, truncated);
        if (static_cast<int64_t>(rendered.size()) >= target_lines) {
            r.max_lines_reached = target_lines >= k_max_lines;
            break;
        }
        if (n_bytes >= k_max_bytes) {
            r.max_bytes_reached = true;
            break;
        }
    }
    for (const kimix::string &piece : rendered) {
        r.output += piece;
    }
    r.start_line = line_offset;
    r.total_lines = current_line_no;
    // "Total lines in file" is only reported when the whole file was scanned
    // (fewer lines rendered than requested and no byte-budget stop).
    const bool with_total =
        (static_cast<int64_t>(rendered.size()) < target_lines) &&
        !r.max_bytes_reached;
    r.message = pl_build_message(rendered.size(), r.start_line, with_total,
                                 current_line_no, r.max_lines_reached,
                                 r.max_bytes_reached, r.truncated_line_numbers);
    return r;
}

plan_render render_tail(kimix::span<const kimix::string> lines,
                        int64_t line_offset, int64_t n_lines) {
    plan_render r;
    const int64_t tail_count = std::llabs(line_offset);
    const int64_t line_limit = std::min(n_lines, k_max_lines);

    // Bounded window over the last `tail_count` lines. The reference uses
    // list.append + list.pop(0) (O(n^2)); a ring-equivalent head index keeps
    // the same content in O(n).
    struct tail_row {
        int64_t line_no = 0;
        kimix::string text;
        bool was_truncated = false;
        int64_t byte_len = 0;
    };
    kimix::vector<tail_row> buf;
    buf.reserve(static_cast<size_t>(std::min<int64_t>(tail_count, 4096) + 1));
    int64_t current_line_no = 0;
    for (const kimix::string &line : lines) {
        ++current_line_no;
        tail_row row;
        row.line_no = current_line_no;
        read::truncate_line_read(line, k_max_line_length, row.text);
        row.was_truncated = (row.text != line);
        row.byte_len = static_cast<int64_t>(row.text.size());
        buf.push_back(std::move(row));
        if (static_cast<int64_t>(buf.size()) > tail_count) {
            buf.erase(buf.begin());
        }
    }
    const int64_t total_lines = current_line_no;
    r.total_lines = total_lines;

    // candidates = tail_buf[:line_limit]
    size_t cand_count = buf.size();
    if (line_limit >= 0 && static_cast<int64_t>(cand_count) > line_limit) {
        cand_count = static_cast<size_t>(line_limit);
    }
    r.max_lines_reached =
        (static_cast<int64_t>(buf.size()) > k_max_lines) &&
        (static_cast<int64_t>(cand_count) == k_max_lines);

    bool max_bytes_reached = false;
    if (cand_count > 0) {
        int64_t total_bytes = 0;
        for (size_t i = 0; i < cand_count; ++i) {
            total_bytes += buf[i].byte_len;
        }
        if (total_bytes > k_max_bytes) {
            max_bytes_reached = true;
            // Keep the NEWEST lines that fit: walk backwards until the budget
            // would be exceeded, then drop everything older.
            size_t kept = 0;
            int64_t running = 0;
            for (size_t i = cand_count; i-- > 0;) {
                running += buf[i].byte_len;
                if (running > k_max_bytes) {
                    break;
                }
                ++kept;
            }
            const size_t first = cand_count - kept;
            kimix::vector<tail_row> kept_rows;
            kept_rows.reserve(kept);
            for (size_t i = first; i < cand_count; ++i) {
                kept_rows.push_back(std::move(buf[i]));
            }
            buf = std::move(kept_rows);
            cand_count = buf.size();
        }
    }
    r.max_bytes_reached = max_bytes_reached;

    for (size_t i = 0; i < cand_count; ++i) {
        if (buf[i].was_truncated) {
            r.truncated_line_numbers.push_back(buf[i].line_no);
        }
        pl_append_numbered_line(r.output, buf[i].line_no, buf[i].text);
    }
    r.start_line = (cand_count > 0) ? buf[0].line_no : (total_lines + 1);
    // _read_tail ALWAYS reports the total line count.
    r.message =
        pl_build_message(cand_count, r.start_line, /*with_total=*/true,
                         total_lines, r.max_lines_reached, max_bytes_reached,
                         r.truncated_line_numbers);
    return r;
}

kimix::string apply_char_window(kimix::string_view output, int64_t char_offset,
                                int64_t max_char) {
    // Python: output[char_offset:max_char] - max_char is the slice END.
    if (char_offset < 0) {
        char_offset = 0;
    }
    if (max_char < char_offset) {
        return {};
    }
    const size_t begin =
        utf8_byte_offset_of_code_point(output, static_cast<size_t>(char_offset));
    if (begin >= output.size()) {
        return {};
    }
    const size_t end =
        utf8_byte_offset_of_code_point(output, static_cast<size_t>(max_char));
    if (end <= begin) {
        return {};
    }
    return kimix::string(output.substr(begin, end - begin));
}

kimix::string format_line_list(kimix::span<const int64_t> line_numbers) {
    kimix::string out = "[";
    for (size_t i = 0; i < line_numbers.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += kimix::format("{}", line_numbers[i]);
    }
    out += "]";
    return out;
}

// ---------------------------------------------------------------------------
// Edit kernels
// ---------------------------------------------------------------------------

plan_edit_result apply_plan_edit(kimix::string_view content,
                                 const plan_edit_item &item) {
    plan_edit_result r;
    r.content = kimix::string(content);
    // if not edit.old or edit.old == edit.new: return content, 0, None
    if (item.old_text.empty() || item.old_text == item.new_text) {
        return r;
    }
    const kimix::string norm_content = edit::normalize_newlines(content);
    const kimix::string norm_old = edit::normalize_newlines(item.old_text);
    const kimix::string norm_new = edit::normalize_newlines(item.new_text);

    if (item.replace_all) {
        const size_t count = pl_count(norm_content, norm_old);
        if (count == 0) {
            const edit::optional_text_result sim =
                edit::find_similar(item.old_text, content);
            if (sim.error.failed()) {
                r.error = sim.error;
                return r;
            }
            r.suggestion = sim.text;
            return r;
        }
        r.content = pl_replace_all(norm_content, norm_old, norm_new);
        r.replacements = count;
        return r;
    }

    const size_t idx = norm_content.find(norm_old);
    if (idx != kimix::string::npos) {
        kimix::string updated = norm_content;
        updated.replace(idx, norm_old.size(), norm_new);
        r.content = std::move(updated);
        r.replacements = 1;
        return r;
    }

    const edit::optional_text_result stripped =
        edit::try_strip_match(content, item.old_text, item.new_text);
    if (stripped.error.failed()) {
        r.error = stripped.error;
        return r;
    }
    if (stripped.text.has_value()) {
        r.content = *stripped.text;
        r.replacements = 1;
        return r;
    }

    const edit::fuzzy_match_result fuzzy =
        edit::best_fuzzy_match(item.old_text, content);
    if (fuzzy.error.failed()) {
        r.error = fuzzy.error;
        return r;
    }
    // edit::best_fuzzy_match publishes `score` only when the cutoff (75) is
    // met, while `matched_original` is set for any non-zero score - so the
    // score is the gate, matching Python's `_find_best_fuzzy_match` returning
    // None below the cutoff.
    if (fuzzy.matched_original.has_value() && fuzzy.score > 0.0) {
        const kimix::string matched =
            edit::normalize_newlines(*fuzzy.matched_original);
        // norm_content.replace(normalize(matched), norm_new, 1)
        const size_t hit = norm_content.find(matched);
        if (hit != kimix::string::npos) {
            kimix::string updated = norm_content;
            updated.replace(hit, matched.size(), norm_new);
            r.content = std::move(updated);
        }
        r.replacements = 1;
        // EditPlan returns NO suggestion for a successful fuzzy match.
        return r;
    }

    const edit::optional_text_result sim =
        edit::find_similar(item.old_text, content);
    if (sim.error.failed()) {
        r.error = sim.error;
        return r;
    }
    r.suggestion = sim.text;
    return r;
}

edit_plan_result apply_plan_edits(kimix::string_view content,
                                  kimix::span<const plan_edit_item> edits) {
    edit_plan_result r;
    const kimix::string original(content);
    kimix::string text = original;
    size_t total = 0;
    for (const plan_edit_item &item : edits) {
        const plan_edit_result one = apply_plan_edit(text, item);
        if (one.error.failed()) {
            r.error = one.error;
            r.content = original;
            return r;
        }
        text = one.content;
        total += one.replacements;
        if (one.suggestion.has_value() && !one.suggestion->empty()) {
            r.last_suggestion = one.suggestion;
        }
    }
    r.content = std::move(text);
    r.total_replacements = total;
    r.changed = (r.content != original);
    return r;
}

// ---------------------------------------------------------------------------
// Message composition
// ---------------------------------------------------------------------------

kimix::string no_plan_path_message(kimix::string_view tool_name) {
    kimix::string out(tool_name);
    out += " tool invalid: no plan_writing_path set.";
    return out;
}

kimix::string plan_missing_message(kimix::string_view path) {
    return kimix::format("Plan file `{}` does not exist.", path);
}

kimix::string plan_not_a_file_message(kimix::string_view path) {
    return kimix::format("`{}` is not a file.", path);
}

kimix::string plan_written_message(kimix::string_view mode,
                                   kimix::string_view path) {
    const kimix::string_view action =
        (mode == "append") ? kimix::string_view("appended to")
                           : kimix::string_view("written to");
    return kimix::format("Plan {} {}", action, path);
}

kimix::string no_replacements_message(kimix::string_view suggestion) {
    kimix::string msg =
        "No replacements were made. The old string was not found in the plan "
        "file.";
    if (!suggestion.empty()) {
        msg += "\n\nDid you mean:\n  ";
        msg.append(suggestion.data(), suggestion.size());
    }
    return msg;
}

kimix::string plan_edited_message(size_t edit_count, size_t replacements) {
    return kimix::format(
        "Plan file successfully edited. Applied {} edit(s) with {} total "
        "replacement(s).",
        edit_count, replacements);
}

kimix::string plan_failure_message(kimix::string_view action,
                                   kimix::string_view what) {
    kimix::string out = "Failed to ";
    out.append(action.data(), action.size());
    out += ". Error: ";
    out.append(what.data(), what.size());
    return out;
}

// ---------------------------------------------------------------------------
// File access
// ---------------------------------------------------------------------------

tool_error read_plan_file(kimix::string_view path, kimix::string &out) {
    return pl_read_file(path, out);
}

tool_error write_plan_file(kimix::string_view path, kimix::string_view content,
                           kimix::string_view mode, uint64_t &size_bytes) {
    namespace fs = kimix::filesystem;
    size_bytes = 0;
    const fs::path target = fs::path(kimix::string(path));
    std::error_code ec;
    const fs::path parent = target.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec); // mkdir(parents=True, exist_ok=True)
        if (ec && !fs::is_directory(parent, ec)) {
            return {tool_status::external_library,
                    plan_failure_message("write plan", ec.message())};
        }
    }
    const char *fmode = (mode == "append") ? "ab" : "wb";
    std::FILE *f = std::fopen(kimix::to_string(target).c_str(), fmode);
    if (f == nullptr) {
        return {tool_status::external_library,
                plan_failure_message("write plan", "cannot open plan file")};
    }
    const size_t written =
        content.empty() ? 0 : std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
    if (written != content.size()) {
        return {tool_status::external_library,
                plan_failure_message("write plan", "short write")};
    }
    size_bytes = written;
    return {tool_status::ok, {}};
}

// ---------------------------------------------------------------------------
// WritePlan
// ---------------------------------------------------------------------------

WritePlan::WritePlan(kimix::builtin_tools::Session *session)
    : kimix::builtin_tools::Tool(session) {}

void WritePlan::operator()(const ToolParams *parameters) {
    _result.clear();
    // Python: `if not _enable_plan: raise SkipThisTool()`
    if (_session != nullptr && !_session->plan_enabled &&
        plan_path_override.empty()) {
        pl_serialize(_result, tool_status::unsupported,
                     "plan tools are disabled for this session", "",
                     "invalid tool.");
        return;
    }
    write_plan_params params;
    const tool_error perr = parse_write_params(parameters, params);
    if (perr.failed()) {
        pl_serialize(_result, perr.status, perr.message, "", "invalid params");
        return;
    }
    const kimix::string path = pl_effective_path(_session, plan_path_override);
    if (path.empty()) {
        pl_serialize(_result, tool_status::invalid_input,
                     no_plan_path_message("WritePlan"), "", "invalid tool.");
        return;
    }
    if (has_injected_content) {
        if (params.mode == "append") {
            injected_content += params.content;
        } else {
            injected_content = params.content;
        }
        pl_serialize(_result, tool_status::ok,
                     plan_written_message(params.mode, path),
                     plan_written_message(params.mode, path), "Write plan");
        return;
    }
    if (_session == nullptr || !_session->native_io) {
        pl_serialize(_result, tool_status::unsupported,
                     "native plan writing requires a native_io session", "",
                     "invalid tool.");
        return;
    }
    uint64_t written = 0;
    const tool_error werr =
        write_plan_file(path, params.content, params.mode, written);
    if (werr.failed()) {
        pl_serialize(_result, werr.status, werr.message, "",
                     "Failed to write plan");
        return;
    }
    const kimix::string message = plan_written_message(params.mode, path);
    pl_serialize(_result, tool_status::ok, message, message, "Write plan");
}

// ---------------------------------------------------------------------------
// ReadPlan
// ---------------------------------------------------------------------------

ReadPlan::ReadPlan(kimix::builtin_tools::Session *session)
    : kimix::builtin_tools::Tool(session) {}

void ReadPlan::operator()(const ToolParams *parameters) {
    _result.clear();
    if (_session != nullptr && !_session->plan_enabled &&
        plan_path_override.empty()) {
        pl_serialize(_result, tool_status::unsupported,
                     "plan tools are disabled for this session", "",
                     "invalid tool.");
        return;
    }
    read_plan_params params;
    const tool_error perr = parse_read_params(parameters, params);
    if (perr.failed()) {
        pl_serialize(_result, perr.status, perr.message, "", "invalid params");
        return;
    }
    const kimix::string path = pl_effective_path(_session, plan_path_override);
    if (path.empty()) {
        pl_serialize(_result, tool_status::invalid_input,
                     no_plan_path_message("ReadPlan"), "", "invalid tool.");
        return;
    }
    kimix::string content;
    if (has_injected_content) {
        content = injected_content;
    } else if (_session != nullptr && _session->native_io) {
        const tool_error rerr = pl_read_file(path, content);
        if (rerr.failed()) {
            const char *brief_text =
                (rerr.status == tool_status::not_found) ? "File not found"
                                                        : "Invalid path";
            pl_serialize(_result, rerr.status, rerr.message, "", brief_text);
            return;
        }
    } else {
        pl_serialize(_result, tool_status::unsupported,
                     "native plan reading requires a native_io session", "",
                     "invalid tool.");
        return;
    }
    const kimix::vector<kimix::string> lines = read::split_lines(content);
    const plan_render rendered =
        (params.line_offset < 0)
            ? render_tail(kimix::span<const kimix::string>(lines),
                          params.line_offset, params.n_lines)
            : render_forward(kimix::span<const kimix::string>(lines),
                             params.line_offset, params.n_lines);
    const kimix::string windowed =
        apply_char_window(rendered.output, params.char_offset, params.max_char);

    ToolParams result;
    result.values["ok"] = ValueElement::make_bool(true);
    result.values["status"] = ValueElement::make_string(kimix::string("ok"));
    result.values["message"] =
        ValueElement::make_string(rendered.message);
    result.values["output"] = ValueElement::make_string(windowed);
    result.values["brief"] = ValueElement::make_string(kimix::string("Read plan"));
    result.values["start_line"] = ValueElement::make_int(rendered.start_line);
    result.values["total_lines"] = ValueElement::make_int(rendered.total_lines);
    result.values["max_lines_reached"] =
        ValueElement::make_bool(rendered.max_lines_reached);
    result.values["max_bytes_reached"] =
        ValueElement::make_bool(rendered.max_bytes_reached);
    kimix::vector<ValueElement> truncated;
    truncated.reserve(rendered.truncated_line_numbers.size());
    for (const int64_t n : rendered.truncated_line_numbers) {
        truncated.push_back(ValueElement::make_int(n));
    }
    result.values["truncated_line_numbers"] =
        ValueElement::make_array(std::move(truncated));
    _result.clear();
    result.serialize(_result);
}

// ---------------------------------------------------------------------------
// EditPlan
// ---------------------------------------------------------------------------

EditPlan::EditPlan(kimix::builtin_tools::Session *session)
    : kimix::builtin_tools::Tool(session) {}

void EditPlan::operator()(const ToolParams *parameters) {
    _result.clear();
    if (_session != nullptr && !_session->plan_enabled &&
        plan_path_override.empty()) {
        pl_serialize(_result, tool_status::unsupported,
                     "plan tools are disabled for this session", "",
                     "invalid tool.");
        return;
    }
    edit_plan_params params;
    const tool_error perr = parse_edit_params(parameters, params);
    if (perr.failed()) {
        pl_serialize(_result, perr.status, perr.message, "", "invalid params");
        return;
    }
    const kimix::string path = pl_effective_path(_session, plan_path_override);
    if (path.empty()) {
        pl_serialize(_result, tool_status::invalid_input,
                     no_plan_path_message("EditPlan"), "", "invalid tool.");
        return;
    }
    kimix::string content;
    if (has_injected_content) {
        content = injected_content;
    } else if (_session != nullptr && _session->native_io) {
        const tool_error rerr = pl_read_file(path, content);
        if (rerr.failed()) {
            const char *brief_text =
                (rerr.status == tool_status::not_found) ? "File not found"
                                                        : "Invalid path";
            pl_serialize(_result, rerr.status, rerr.message, "", brief_text);
            return;
        }
    } else {
        pl_serialize(_result, tool_status::unsupported,
                     "native plan editing requires a native_io session", "",
                     "invalid tool.");
        return;
    }

    const edit_plan_result applied = apply_plan_edits(
        content, kimix::span<const plan_edit_item>(params.edits));
    if (applied.error.failed()) {
        pl_serialize(_result, applied.error.status,
                     plan_failure_message("edit plan", applied.error.message),
                     "", "Failed to edit plan");
        return;
    }
    if (!applied.changed) {
        const kimix::string suggestion =
            applied.last_suggestion.has_value() ? *applied.last_suggestion
                                                : kimix::string();
        pl_serialize(_result, tool_status::no_change,
                     no_replacements_message(suggestion), "",
                     "No replacements made");
        return;
    }
    if (has_injected_content) {
        injected_content = applied.content;
    } else {
        uint64_t written = 0;
        const tool_error werr =
            write_plan_file(path, applied.content, "overwrite", written);
        if (werr.failed()) {
            pl_serialize(_result, werr.status, werr.message, "",
                         "Failed to edit plan");
            return;
        }
    }
    pl_serialize(_result, tool_status::ok,
                 plan_edited_message(params.edits.size(),
                                     applied.total_replacements),
                 "", "Edit plan");
}

// ---------------------------------------------------------------------------
// Static registration (registry keys are the plain class names: "WritePlan",
// "ReadPlan", "EditPlan" - see tool_registry.h ToolRegistrar).
// ---------------------------------------------------------------------------

KIMIX_REGISTER_TOOL(
    WritePlan, "Write the plan to the plan file.",
    R"JSON({"type":"object","properties":{"content":{"type":"string","description":"Content to write. Accepts `content` or `text`."},"mode":{"type":"string","enum":["overwrite","append"],"description":"Write mode: overwrite or append."}},"required":["content"]})JSON");

KIMIX_REGISTER_TOOL(
    ReadPlan, "Read the plan file.",
    R"JSON({"type":"object","properties":{"line_offset":{"type":"integer","description":"Start line, 1-based. Negative reads from end. Max abs 1000."},"n_lines":{"type":"integer","description":"Lines to read, max 1000.","minimum":1},"max_char":{"type":"integer","description":"Maximum number of characters to return.","minimum":0},"char_offset":{"type":"integer","description":"Character offset to start returning from.","minimum":0}}})JSON");

KIMIX_REGISTER_TOOL(
    EditPlan, "Replace strings in the plan file.",
    R"JSON({"type":"object","properties":{"edit":{"description":"One or more edits. Accepts `edit` or `edits`.","oneOf":[{"type":"object","properties":{"old":{"type":"string","description":"String to replace. Accepts `old` or `old_string`."},"new":{"type":"string","description":"Replacement string. Accepts `new` or `new_string`."},"replace_all":{"type":"boolean","description":"Replace all occurrences."}},"required":["old","new"]},{"type":"array","items":{"type":"object","properties":{"old":{"type":"string"},"new":{"type":"string"},"replace_all":{"type":"boolean"}},"required":["old","new"]}}]}},"required":["edit"]})JSON");

} // namespace kimix::builtin_tools::plan
