// job_output_tool.cpp - C++ port of the kimi-agent `job_output` background
// task tool. See job_output_tool.h for the reference line map.
//
// Unity-build rules: TU-local helpers live in an anonymous namespace inside
// kimix::builtin_tools::job_output and carry the `jo_` prefix.
//
// Default TaskSource binding: the reproc task registry from
// builtin_tools/process_runner.h (proc::list_tasks / read_task / wait_task /
// stop_task / remove_task). The Python tool reads BackgroundStream objects out
// of the session task registry; the C++ registry is process-wide, which is the
// same shape because one process hosts one session's tasks.
#include "builtin_tools/job_output_tool.h"

#include <cstdio>

#include "builtin_tools/process_runner.h"
#include "builtin_tools/regex_lite.h"
#include "builtin_tools/tool_registry.h"

namespace kimix::builtin_tools::job_output {

namespace {

const char *jo_status_string(tool_status status) noexcept {
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

// Trim ASCII whitespace from both ends (Python str.strip() for the id fields).
kimix::string jo_strip(kimix::string_view text) {
    size_t b = 0;
    size_t e = text.size();
    auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
               c == '\f';
    };
    while (b < e && is_space(text[b])) {
        ++b;
    }
    while (e > b && is_space(text[e - 1])) {
        --e;
    }
    return kimix::string(text.substr(b, e - b));
}

// "{:.1f}" / "{:.2f}". std::format and Python's format both round the exact
// binary value half-to-even, so the plain format string is byte-identical to
// the reference f-strings (python_tool.cpp format_elapsed relies on the same).
kimix::string jo_format_1f(double value) {
    return kimix::format("{:.1f}", value);
}

kimix::string jo_format_2f(double value) {
    return kimix::format("{:.2f}", value);
}

void jo_error(ToolParams &result, tool_status status, kimix::string_view message,
              kimix::string_view output, kimix::string_view brief) {
    result.values["ok"] = ValueElement::make_bool(false);
    result.values["status"] =
        ValueElement::make_string(kimix::string(jo_status_string(status)));
    result.values["message"] = ValueElement::make_string(kimix::string(message));
    result.values["output"] = ValueElement::make_string(kimix::string(output));
    result.values["brief"] = ValueElement::make_string(kimix::string(brief));
}

void jo_ok(ToolParams &result, kimix::string_view message,
           kimix::string_view output, kimix::string_view brief) {
    result.values["ok"] = ValueElement::make_bool(true);
    result.values["status"] = ValueElement::make_string(kimix::string("ok"));
    result.values["message"] = ValueElement::make_string(kimix::string(message));
    result.values["output"] = ValueElement::make_string(kimix::string(output));
    result.values["brief"] = ValueElement::make_string(kimix::string(brief));
}

// ---------------------------------------------------------------------------
// Default (reproc-backed) task source
// ---------------------------------------------------------------------------
job_task jo_from_summary(const proc::task_summary &s) {
    job_task t;
    t.task_id = s.task_id;
    t.exists = true;
    t.exited = s.exited;
    t.exit_code = s.exit_code;
    t.elapsed_seconds = static_cast<double>(s.elapsed_ms) / 1000.0;
    t.has_elapsed = true;
    return t;
}

kimix::vector<job_task> jo_default_list() {
    kimix::vector<job_task> out;
    const kimix::vector<proc::task_summary> all = proc::list_tasks();
    out.reserve(all.size());
    for (const proc::task_summary &s : all) {
        out.push_back(jo_from_summary(s));
    }
    return out;
}

bool jo_default_read(kimix::string_view id, job_task &out) {
    const proc::task_status_info info = proc::query_task(id);
    if (!info.exists) {
        return false;
    }
    out = jo_from_summary(proc::task_summary{
        kimix::string(id), info.pid, info.exited, info.exit_code,
        info.elapsed_ms});
    kimix::string pending;
    proc::read_task(id, pending);
    out.output = std::move(pending);
    return true;
}

bool jo_default_wait(kimix::string_view id, kimix::string_view pattern,
                     int64_t timeout_ms, bool &matched, job_task &out) {
    const proc::task_status_info before = proc::query_task(id);
    if (!before.exists) {
        return false;
    }
    const proc::task_wait_result tw = proc::wait_task(id, pattern, timeout_ms);
    matched = tw.matched;
    if (!jo_default_read(id, out)) {
        // The task may have been removed while waiting.
        out = jo_from_summary(proc::task_summary{
            kimix::string(id), before.pid, true, before.exit_code,
            tw.elapsed_ms});
    }
    return true;
}

bool jo_default_kill(kimix::string_view id, job_task &out) {
    const proc::task_status_info before = proc::query_task(id);
    if (!before.exists) {
        return false;
    }
    kimix::string final_output;
    const tool_error err = proc::stop_task(id, final_output);
    if (err.failed()) {
        return false;
    }
    out.task_id = kimix::string(id);
    out.exists = true;
    out.exited = true;
    out.exit_code = before.exit_code;
    out.elapsed_seconds = static_cast<double>(before.elapsed_ms) / 1000.0;
    out.has_elapsed = true;
    out.output = std::move(final_output);
    return true;
}

void jo_default_remove(kimix::string_view id) {
    proc::remove_task(id);
}

TaskSource jo_default_source() {
    TaskSource src;
    src.list = []() { return jo_default_list(); };
    src.read = [](kimix::string_view id, job_task &out) {
        return jo_default_read(id, out);
    };
    src.wait = [](kimix::string_view id, kimix::string_view pattern,
                  int64_t timeout_ms, bool &matched, job_task &out) {
        return jo_default_wait(id, pattern, timeout_ms, matched, out);
    };
    src.kill = [](kimix::string_view id, job_task &out) {
        return jo_default_kill(id, out);
    };
    src.remove = [](kimix::string_view id) { jo_default_remove(id); };
    return src;
}

} // namespace

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

tool_error parse_params(const ToolParams *params, job_output_params &out) {
    out = job_output_params{};
    if (params == nullptr) {
        return {tool_status::ok, {}};
    }
    // job_id | task_id
    const ValueElement *id_el = params->get("job_id");
    if (id_el == nullptr || id_el->is_null()) {
        id_el = params->get("task_id");
    }
    if (id_el != nullptr && !id_el->is_null()) {
        if (!id_el->is_string()) {
            return {tool_status::invalid_input, "job_id must be a string"};
        }
        out.job_id = id_el->as_string();
    }
    // action
    kimix::string action = "get";
    if (const ValueElement *a = params->get("action");
        a != nullptr && !a->is_null()) {
        if (!a->is_string()) {
            return {tool_status::invalid_input, "action must be a string"};
        }
        action = a->as_string();
    }
    if (action != "get" && action != "list" && action != "kill") {
        return {tool_status::invalid_input,
                kimix::format("Input should be 'get', 'list' or 'kill' "
                              "(action={})",
                              kimix::string_view(action))};
    }
    // wait | block
    bool wait = false;
    if (const ValueElement *w = params->get("wait");
        w != nullptr && w->is_bool()) {
        wait = w->as_bool();
    } else if (const ValueElement *b = params->get("block");
               b != nullptr && b->is_bool()) {
        wait = b->as_bool();
    }
    out.wait = wait;
    // timeout | timeout_ms (legacy milliseconds -> seconds)
    int64_t timeout = k_default_timeout_seconds;
    bool have_timeout = false;
    if (const ValueElement *t = params->get("timeout");
        t != nullptr && !t->is_null()) {
        if (!t->is_int() && !t->is_uint() && !t->is_real()) {
            return {tool_status::invalid_input, "timeout must be an integer"};
        }
        timeout = t->is_int() ? t->as_int()
                              : (t->is_uint() ? static_cast<int64_t>(t->as_uint())
                                              : static_cast<int64_t>(t->as_real()));
        have_timeout = true;
    } else if (const ValueElement *tm = params->get("timeout_ms");
               tm != nullptr && !tm->is_null()) {
        if (!tm->is_int() && !tm->is_uint()) {
            return {tool_status::invalid_input, "timeout_ms must be an integer"};
        }
        const int64_t ms =
            tm->is_int() ? tm->as_int() : static_cast<int64_t>(tm->as_uint());
        // _normalize_legacy_timeout: max(1, ms // 1000)
        timeout = ms / 1000;
        if (timeout < 1) {
            timeout = 1;
        }
        have_timeout = true;
    }
    if (timeout < k_min_timeout_seconds) {
        return {tool_status::invalid_input,
                kimix::format("timeout must be greater than or equal to {}",
                              k_min_timeout_seconds)};
    }
    if (timeout > k_max_timeout_seconds) {
        return {tool_status::invalid_input,
                kimix::format("timeout must be less than or equal to {}",
                              k_max_timeout_seconds)};
    }
    out.timeout = timeout;
    (void)have_timeout;
    // output_path
    if (const ValueElement *o = params->get("output_path");
        o != nullptr && !o->is_null()) {
        if (!o->is_string()) {
            return {tool_status::invalid_input, "output_path must be a string"};
        }
        out.output_path = o->as_string();
    }
    // wait_for_pattern
    if (const ValueElement *w = params->get("wait_for_pattern");
        w != nullptr && !w->is_null()) {
        if (!w->is_string()) {
            return {tool_status::invalid_input,
                    "wait_for_pattern must be a string"};
        }
        out.wait_for_pattern = w->as_string();
    }
    out.action = action;
    // _normalize_kill: deprecated kill=True -> action='kill'
    if (const ValueElement *k = params->get("kill");
        k != nullptr && k->is_bool() && k->as_bool()) {
        out.action = "kill";
    }
    return {tool_status::ok, {}};
}

// ---------------------------------------------------------------------------
// Pure kernels
// ---------------------------------------------------------------------------

kimix::string task_kind(kimix::string_view task_id) {
    const size_t us = task_id.find('_');
    if (us == kimix::string_view::npos) {
        return "unknown";
    }
    return kimix::string(task_id.substr(0, us));
}

kimix::string format_elapsed_cell(kimix::optional<double> elapsed) {
    // f"{elapsed:.1f}s" if elapsed else "-"
    if (!elapsed.has_value() || *elapsed == 0.0) {
        return "-";
    }
    return jo_format_1f(*elapsed) + "s";
}

kimix::string format_elapsed_paren(kimix::optional<double> elapsed) {
    if (!elapsed.has_value()) {
        return {};
    }
    return " (" + jo_format_1f(*elapsed) + "s)";
}

kimix::string format_completed_banner(double elapsed_seconds) {
    return "\n[Process completed in " + jo_format_2f(elapsed_seconds) + "s]";
}

task_row make_task_row(kimix::string_view task_id, bool alive,
                       kimix::optional<double> elapsed) {
    task_row row;
    row.task_id = kimix::string(task_id);
    row.kind = task_kind(task_id);
    row.status = alive ? "running" : "completed";
    row.elapsed = elapsed;
    return row;
}

kimix::string format_task_list(kimix::span<const task_row> rows) {
    if (rows.empty()) {
        return "No running tasks.";
    }
    kimix::vector<kimix::string> lines;
    lines.reserve(rows.size() + 2);
    lines.push_back("| Task ID | Kind | Status | Elapsed |");
    lines.push_back("|---------|------|--------|---------|");
    for (const task_row &r : rows) {
        lines.push_back(kimix::format("| `{}` | {} | {} | {} |",
                                      kimix::string_view(r.task_id),
                                      kimix::string_view(r.kind),
                                      kimix::string_view(r.status),
                                      kimix::string_view(
                                          format_elapsed_cell(r.elapsed))));
    }
    kimix::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            out.push_back('\n');
        }
        out += lines[i];
    }
    return out;
}

kimix::string not_found_message(kimix::string_view task_id,
                                kimix::span<const kimix::string> available) {
    if (available.empty()) {
        return "No running task";
    }
    kimix::string joined;
    for (size_t i = 0; i < available.size(); ++i) {
        if (i != 0) {
            joined += ", ";
        }
        joined += available[i];
    }
    return kimix::format("Task '{}' not found. Available tasks: [{}]",
                         task_id, kimix::string_view(joined));
}

kimix::string normalize_display_path(kimix::string_view path) {
    kimix::string out(path);
    for (char &c : out) {
        if (c == '\\') {
            c = '/';
        }
    }
    return out;
}

kimix::string exported_message(kimix::string_view output_path) {
    return "output exported to file `" + normalize_display_path(output_path) +
           "`";
}

bool export_to_file(kimix::string_view path, kimix::string_view content) {
    const kimix::filesystem::path target =
        kimix::filesystem::path(kimix::string(path));
    std::error_code ec;
    const kimix::filesystem::path parent = target.parent_path();
    if (!parent.empty()) {
        kimix::filesystem::create_directories(parent, ec);
    }
    std::FILE *f = std::fopen(kimix::to_string(target).c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    if (!content.empty()) {
        std::fwrite(content.data(), 1, content.size(), f);
    }
    std::fclose(f);
    return true;
}

kimix::string status_suffix(kimix::string_view status) {
    kimix::string out = "\n[status: ";
    out.append(status.data(), status.size());
    out += "]";
    return out;
}

kimix::string kill_output_text(kimix::string_view processed) {
    return processed.empty() ? kimix::string("(no output)")
                             : kimix::string(processed);
}

kimix::string kill_message_suffix(kimix::optional<double> elapsed_seconds) {
    return format_elapsed_paren(elapsed_seconds);
}

kimix::string build_get_output_text(const get_output_fields &f) {
    kimix::string text;
    if (f.output_path.has_value()) {
        // f"{f'`{job_id}` is still running, call `job_output` again, '
        //   if task_alive else ''}output exported to file `{display_path}`"
        if (f.task_alive) {
            text = "`" + kimix::string(f.job_id) +
                   "` is still running, call `job_output` again, ";
        }
        text += exported_message(*f.output_path);
    } else {
        text = f.processed.empty() ? "(no output)" : f.processed;
        if (!f.task_alive && f.original_path.has_value()) {
            const kimix::string display =
                normalize_display_path(*f.original_path);
            text += f.has_formatter
                        ? "\n[original output exported to: " + display + "]"
                        : "\n[rtk output exported to: " + display + "]";
        }
    }
    if (f.wait_matched.has_value()) {
        text += "\nwait_matched: ";
        text += (*f.wait_matched) ? "true" : "false";
    }
    if (!f.task_alive && f.elapsed_seconds.has_value()) {
        text += format_completed_banner(*f.elapsed_seconds);
    }
    return text;
}

// ---------------------------------------------------------------------------
// Tool class
// ---------------------------------------------------------------------------

JobOutput::JobOutput(kimix::builtin_tools::Session *session)
    : kimix::builtin_tools::Tool(session) {}

void JobOutput::operator()(const ToolParams *parameters) {
    _result.clear();
    ToolParams result;

    job_output_params params;
    const tool_error perr = parse_params(parameters, params);
    if (perr.failed()) {
        jo_error(result, perr.status, perr.message, "", "Invalid params");
        result.serialize(_result);
        return;
    }

    TaskSource src = source;
    if (!src.list) {
        if (_session == nullptr || !_session->native_io) {
            jo_error(result, tool_status::unsupported,
                     "native job_output requires a native_io session", "",
                     "Unsupported");
            result.serialize(_result);
            return;
        }
        src = jo_default_source();
    }

    const kimix::vector<job_task> tasks = src.list();

    // ---- action='list' (or job_id omitted with the default action) -------
    const bool list_mode =
        (params.action == "list") ||
        (!params.job_id.has_value() && params.action == "get");
    if (list_mode) {
        kimix::vector<task_row> rows;
        kimix::vector<ValueElement> extras;
        rows.reserve(tasks.size());
        extras.reserve(tasks.size());
        for (const job_task &t : tasks) {
            const bool alive = !t.exited;
            const kimix::optional<double> elapsed =
                t.has_elapsed ? kimix::optional<double>(t.elapsed_seconds)
                              : std::nullopt;
            rows.push_back(make_task_row(t.task_id, alive, elapsed));
            ToolParams entry;
            entry.values["task_id"] = ValueElement::make_string(t.task_id);
            entry.values["kind"] =
                ValueElement::make_string(task_kind(t.task_id));
            entry.values["status"] = ValueElement::make_string(
                alive ? kimix::string("running") : kimix::string("completed"));
            if (elapsed.has_value()) {
                entry.values["elapsed"] = ValueElement::make_real(*elapsed);
            } else {
                entry.values["elapsed"] = ValueElement::make_null();
            }
            extras.push_back(ValueElement::make_object(
                kimix::shared_ptr<ToolParams>(new ToolParams(std::move(entry)))));
        }
        if (rows.empty()) {
            jo_ok(result, "", "No running tasks.", "No background tasks");
            result.values["tasks"] =
                ValueElement::make_array(std::move(extras));
            result.values["status_text"] =
                ValueElement::make_string(kimix::string("completed"));
            result.serialize(_result);
            return;
        }
        const kimix::string table = format_task_list(
            kimix::span<const task_row>(rows));
        jo_ok(result, "", table,
              kimix::format("{} background task(s)", rows.size()));
        result.values["tasks"] = ValueElement::make_array(std::move(extras));
        result.values["status_text"] =
            ValueElement::make_string(kimix::string("completed"));
        result.serialize(_result);
        return;
    }

    // From here on a job_id is required.
    if (!params.job_id.has_value()) {
        jo_error(result, tool_status::invalid_input,
                 "task_id is required for action='kill'.", "",
                 "Missing task_id");
        result.serialize(_result);
        return;
    }
    const kimix::string raw_id = *params.job_id;
    const kimix::string task_id = jo_strip(raw_id);

    kimix::vector<kimix::string> available;
    for (const job_task &t : tasks) {
        available.push_back(t.task_id);
    }

    // ---- action='kill' ---------------------------------------------------
    if (params.action == "kill") {
        job_task killed;
        if (!src.kill || !src.kill(task_id, killed)) {
            const kimix::string message =
                not_found_message(raw_id, kimix::span<const kimix::string>(
                                              available));
            const kimix::string brief = available.empty()
                                            ? kimix::string("No running task")
                                            : kimix::format("Task '{}' not found",
                                                            raw_id);
            jo_error(result, tool_status::not_found, message, "", brief);
            result.serialize(_result);
            return;
        }
        if (src.remove) {
            src.remove(task_id);
        }
        kimix::string processed = killed.output;
        if (process_output) {
            processed = process_output(processed);
        }
        const bool success =
            killed.exit_code.has_value() && *killed.exit_code == 0;
        kimix::string message;
        if (!success) {
            const kimix::optional<double> elapsed =
                killed.has_elapsed
                    ? kimix::optional<double>(killed.elapsed_seconds)
                    : std::nullopt;
        message += kill_message_suffix(elapsed);
        const kimix::string killed_text =
            kill_output_text(processed) + status_suffix("killed");
        jo_error(result, tool_status::external_library, message, killed_text,
                 kimix::format("Task '{}' killed (non-zero exit)", raw_id));
        result.values["status_text"] =
            ValueElement::make_string(kimix::string("killed"));
            result.serialize(_result);
            return;
        }
        jo_ok(result, message, kill_output_text(processed) +
                                   status_suffix("killed"),
              kimix::format("Task '{}' killed", raw_id));
        result.values["status_text"] =
            ValueElement::make_string(kimix::string("killed"));
        result.serialize(_result);
        return;
    }

    // ---- action='get' ----------------------------------------------------
    // A provided wait_for_pattern is compiled (validated) even when `wait` is
    // false - TaskOutput._get_output 261-269.
    if (params.wait_for_pattern.has_value()) {
        kimix::string regex_error;
        regex_lite::Regex probe;
        if (!probe.compile(*params.wait_for_pattern, false, regex_error)) {
            jo_error(result, tool_status::invalid_input,
                     kimix::format("Invalid wait_for_pattern: {}",
                                   kimix::string_view(regex_error)),
                     "", "Invalid pattern");
            result.serialize(_result);
            return;
        }
    }
    job_task task;
    bool matched = false;
    kimix::optional<bool> wait_matched;
    bool found = false;
    if (params.wait) {
        const kimix::string pattern =
            params.wait_for_pattern.value_or(kimix::string());
        if (src.wait) {
            found = src.wait(task_id, pattern, params.timeout * 1000, matched,
                             task);
        }
        if (params.wait_for_pattern.has_value()) {
            wait_matched = matched;
        }
    } else if (src.read) {
        found = src.read(task_id, task);
    }
    if (!found) {
        const kimix::string message =
            not_found_message(raw_id, kimix::span<const kimix::string>(
                                          available));
        const kimix::string brief = available.empty()
                                        ? kimix::string("No running task")
                                        : kimix::format("Task '{}' not found",
                                                        raw_id);
        jo_error(result, tool_status::not_found, message, "", brief);
        result.values["status_text"] =
            ValueElement::make_string(kimix::string("running"));
        result.serialize(_result);
        return;
    }

    const bool task_alive = !task.exited;
    kimix::string processed = task.output;
    if (process_output) {
        processed = process_output(processed);
    }
    kimix::string message;

    if (!task_alive) {
        if (src.remove) {
            src.remove(task_id);
        }
        const bool success =
            task.exit_code.has_value() && *task.exit_code == 0;
        if (!success) {
            // TaskOutput._get_output 299-317: early ToolError return.
            const kimix::optional<double> elapsed =
                task.has_elapsed
                    ? kimix::optional<double>(task.elapsed_seconds)
                    : std::nullopt;
            message += format_elapsed_paren(elapsed);
            kimix::string output_text;
            if (params.output_path.has_value()) {
                job_output::export_to_file(*params.output_path, task.output);
                output_text = exported_message(*params.output_path);
            } else {
                output_text = processed.empty() ? "(no output)" : processed;
            }
            jo_error(result, tool_status::external_library, message,
                     output_text,
                     kimix::format("Task '{}' failed", raw_id));
            result.values["output"] = ValueElement::make_string(
                output_text + status_suffix("completed"));
            result.values["status_text"] =
                ValueElement::make_string(kimix::string("completed"));
            result.serialize(_result);
            return;
        }
    }

    // Optional export to output_path (the Python tool writes the RAW output).
    if (params.output_path.has_value()) {
        job_output::export_to_file(*params.output_path, task.output);
    }

    get_output_fields fields;
    fields.processed = processed;
    fields.output_path = params.output_path;
    fields.original_path = std::nullopt;
    fields.has_formatter = static_cast<bool>(process_output);
    fields.wait_matched = wait_matched;
    fields.task_alive = task_alive;
    fields.elapsed_seconds =
        task.has_elapsed ? kimix::optional<double>(task.elapsed_seconds)
                         : std::nullopt;
    fields.job_id = raw_id;
    kimix::string output_text = build_get_output_text(fields);
    // Tool-description contract: "Every response ends with `[status: ...]`".
    output_text += status_suffix(task_alive ? "running" : "completed");

    jo_ok(result, message, output_text, "Task output retrieved");
    result.values["status_text"] = ValueElement::make_string(
        task_alive ? kimix::string("running") : kimix::string("completed"));
    result.values["task_id"] = ValueElement::make_string(raw_id);
    result.values["kind"] = ValueElement::make_string(task_kind(raw_id));
    result.values["description"] = ValueElement::make_string(
        output_text.substr(0, std::min<size_t>(output_text.size(),
                                               k_description_chars)));
    result.serialize(_result);
}

// Static registration: the registry key is the class name "JobOutput"; the
// agent-facing tool name in kimix.tools.background is "job_output".
KIMIX_REGISTER_TOOL(
    JobOutput,
    "Read a background job. Stream jobs return only output since the previous "
    "read; final-output jobs return their result after settlement. Every "
    "response ends with `[status: ...]`. Reads are non-blocking unless "
    "`wait: true`, which waits up to the configured cap.",
    R"JSON({"type":"object","properties":{"job_id":{"type":"string","description":"Job id returned by the tool that started the background work. When None, lists all tasks. Accepts `job_id` or `task_id`."},"action":{"type":"string","enum":["get","list","kill"],"description":"'get': Return output from the job specified by `job_id` (default). 'list': List all jobs (when job_id is empty). 'kill': Force-stop the job specified by `job_id` and return its final output."},"wait":{"type":"boolean","description":"Block until the job reaches a terminal status or the timeout expires. A timed-out wait returns [status: running] and leaves the job alive. Accepts `wait` or `block`. When False (default), return immediately with whatever output is available so far."},"timeout":{"type":"integer","description":"Max wait in seconds (only meaningful with wait: true). Defaults to the configured wait timeout; capped by the configured maximum. Accepts `timeout` or `timeout_ms`.","minimum":1,"maximum":7200},"output_path":{"type":"string","description":"Output file path."},"wait_for_pattern":{"type":"string","description":"Pattern to wait for in the tool output."},"kill":{"type":"boolean","description":"[Deprecated] Use action='kill' instead."}}})JSON");

} // namespace kimix::builtin_tools::job_output
