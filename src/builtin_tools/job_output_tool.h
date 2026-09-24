// job_output_tool.h - C++ port of the kimi-agent `job_output` background-task
// tool (Python class TaskOutput).
//
// Python source of truth (C:/dev/kimi-agent/src/kimix/tools/background/
// __init__.py):
//   TaskOutputParams (job_id|task_id, action, wait|block, timeout|timeout_ms,
//                     output_path, wait_for_pattern, kill)      16-72
//   _normalize_legacy_timeout (timeout_ms -> timeout seconds)    55-62
//   _normalize_kill (kill=True -> action='kill')                 64-72
//   TaskOutput.name / description                                87-95
//   TaskOutput.__call__ dispatch (list / kill / get)             117-141
//   TaskOutput._list_tasks (markdown table)                      142-166
//   TaskOutput._kill_task                                        167-207
//   TaskOutput._process_completed_output                         208-234
//   TaskOutput._get_output                                       235-352
// Also background/utils.py generate_task_id / remove_task_id /
// get_all_tasks (395-459) and the BackgroundStream wait/pop contract
// (116-390).
//
// Design notes (project conventions):
// * namespace kimix::builtin_tools::job_output; TU-local helpers use the
//   `jo_` prefix (kimix-llm builds src/builtin_tools/*.cpp as one unity TU).
// * kimix:: containers only; no RTTI; kernels never throw across the tool
//   boundary.
// * The formatting/decision kernels are pure: they take `job_task` snapshots,
//   so unit tests need no subprocesses. The Tool wrapper binds a `TaskSource`
//   whose default implementation reads the reproc task registry
//   (builtin_tools/process_runner.h) when Session::native_io is set.
// * The rtk/export/summarize side-channels of the Python tool
//   (_maybe_export_rtk_original_async, _summarize_long_output_async) need an
//   LLM or the shared cache dir; they are represented here by the injected
//   `original_path` field and the documented `[... exported to: ...]` suffix.
#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "builtin_tools/tool.h"
#include "builtin_tools/tool_types.h"

namespace kimix::builtin_tools::job_output {

using kimix::builtin_tools::tool_error;
using kimix::builtin_tools::tool_status;
using kimix::builtin_tools::ToolParams;
using kimix::builtin_tools::ValueElement;

// Python defaults (TaskOutputParams).
inline constexpr int64_t k_default_timeout_seconds = 60;
inline constexpr int64_t k_min_timeout_seconds = 1;
inline constexpr int64_t k_max_timeout_seconds = 7200;
// _get_output: inactivity_timeout = min(900, timeout)
inline constexpr int64_t k_inactivity_cap_seconds = 900;
// Description cap of the display block.
inline constexpr size_t k_description_chars = 200;
// MAX_FINISHED_TASKS (background/utils.py 48): the bounded finished-task
// history a later job_output serves a job from.
inline constexpr size_t k_max_finished_tasks = 25;
// common.py ELAPSED_REPORT_MINIMUM_SECONDS (955): sub-second runtimes are
// "pure noise" and are never annotated.
inline constexpr double k_elapsed_report_minimum_seconds = 1.0;

// ---------------------------------------------------------------------------
// Task snapshot (what the pure kernels consume)
// ---------------------------------------------------------------------------
struct job_task {
    kimix::string task_id;
    bool exists = false;
    bool exited = false; // process finished (thread no longer alive)
    kimix::optional<int64_t> exit_code; // set once exited
    double elapsed_seconds = 0.0; // process_elapsed (may be 0 when unknown)
    bool has_elapsed = false;
    kimix::string output; // pending output since the previous read
};

// Injectable task source. The Tool wrapper installs a reproc-backed default;
// unit tests inject a scripted one.
struct TaskSource {
    // Every known task, in registration order.
    kimix::function<kimix::vector<job_task>()> list;
    // Non-blocking read: consume the buffered output of `id`.
    kimix::function<bool(kimix::string_view id, job_task &out)> read;
    // Blocking read: wait up to timeout_ms for `pattern` ("" == wait for exit).
    kimix::function<bool(kimix::string_view id, kimix::string_view pattern,
                         int64_t timeout_ms, bool &matched, job_task &out)>
        wait;
    // Force-stop and return the final output.
    kimix::function<bool(kimix::string_view id, job_task &out)> kill;
    // Drop the id from the registry (background/utils.py remove_task_id).
    kimix::function<void(kimix::string_view id)> remove;
};

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------
struct job_output_params {
    kimix::optional<kimix::string> job_id; // job_id | task_id
    kimix::string action = "get"; // get | list | kill
    bool wait = false; // wait | block
    int64_t timeout = k_default_timeout_seconds;
    kimix::optional<kimix::string> output_path;
    kimix::optional<kimix::string> wait_for_pattern;
};

// TaskOutputParams + the two normalizing validators. Returns
// tool_status::invalid_input with the pydantic wording on a bad action, a
// non-string job_id or an out-of-range timeout. Legacy `timeout_ms`
// (milliseconds) is converted with max(1, ms // 1000) when `timeout` is
// absent; `kill: true` becomes action='kill'.
tool_error parse_params(const ToolParams *params, job_output_params &out);

// ---------------------------------------------------------------------------
// Pure kernels
// ---------------------------------------------------------------------------
// task_id.split("_")[0] when the id contains "_", else "unknown".
kimix::string task_kind(kimix::string_view task_id);

// The display-block kind of _get_output / _get_history_output:
// `job_id.split("_")[0] if job_id else "task"` -- note the *different* default
// ("task", not "unknown") and the falsy-empty branch.
kimix::string display_kind(kimix::string_view job_id);

// One row of the list table.
struct task_row {
    kimix::string task_id;
    kimix::string kind;
    kimix::string status; // "running" | "completed"
    kimix::optional<double> elapsed; // nullopt / 0 renders as "-"
};

task_row make_task_row(kimix::string_view task_id, bool alive,
                       kimix::optional<double> elapsed);

// _list_tasks: the markdown table, or "No running tasks." when empty.
kimix::string format_task_list(kimix::span<const task_row> rows);

// "Task '{id}' not found. Available tasks: [{a, b}]" / "No running task".
kimix::string not_found_message(kimix::string_view task_id,
                                kimix::span<const kimix::string> available);

// _get_output's output_text assembly. `wait_matched` is None-equivalent when
// the optional is empty; `original_path` selects the
// "[original output exported to: ...]" / "[rtk output exported to: ...]"
// suffix (the former when `has_formatter`).
struct get_output_fields {
    kimix::string processed; // post-filter output ("" -> "(no output)")
    kimix::optional<kimix::string> output_path; // exported file (display form)
    kimix::optional<kimix::string> original_path;
    bool has_formatter = false;
    kimix::optional<bool> wait_matched;
    bool task_alive = true;
    kimix::optional<double> elapsed_seconds;
    kimix::string_view job_id;
};
kimix::string build_get_output_text(const get_output_fields &f);

// "[status: running]" / "[status: completed]" / "[status: killed]".
//
// PARITY NOTE: the Python reference *never* appends this to `output` (or to
// anything else) -- the only mention of it in kimi-agent is the tool
// description string ("Every response ends with `[status: ...]`", __init__.py
// 110), which nothing implements. The port therefore reports the status in the
// machine-readable `status_text` JSON field only and keeps `output`/`message`
// byte-identical to the reference; this helper (and the *[status: ...]* text it
// builds) is not appended to any model-visible string.
kimix::string status_suffix(kimix::string_view status);

// _kill_task's SUCCESS fallback: Python's ``processed if processed else
// "(no output)"``.
kimix::string kill_output_text(kimix::string_view processed);
// _kill_task's FAILURE fallback: Python's ``processed if processed else ""``
// (a failed kill keeps an empty output empty -- the two branches differ).
kimix::string kill_failed_output_text(kimix::string_view processed);

// common.py _format_elapsed_seconds (976-991): "1.23s" (< 60s, {:.2f}),
// "1m05s" (< 3600s), "1h02m"; "" when the value is not coercible
// (_coerce_seconds: non-numbers, negatives, NaN and Inf are all None).
kimix::string format_duration(double elapsed_seconds);
// common.py _elapsed_suffix (1011-1020): " (1.23s)", gated by
// ELAPSED_REPORT_MINIMUM_SECONDS = 1.0 (_reportable_seconds 994-1008).
kimix::string format_elapsed_suffix(kimix::optional<double> elapsed_seconds);
// common.py _elapsed_tag (1023-1033): "[Process completed in 1.23s]".
kimix::string format_elapsed_tag(kimix::optional<double> elapsed_seconds);
// common.py _append_elapsed (1036-1057): an empty message becomes the suffix
// itself ("(3.50s)" -- suffix.lstrip() only drops the leading space, the parens
// stay); a message already ending in a "(1.23s)"-style suffix is returned
// unchanged (idempotent).
kimix::string append_elapsed(kimix::string_view message,
                             kimix::optional<double> elapsed_seconds);
// The kill / failure message tail: format_elapsed_suffix.
kimix::string kill_message_suffix(kimix::optional<double> elapsed_seconds);

// "{:.1f}s" / "-" formatting used by the list table (_list_tasks 183 uses a
// plain one-decimal format, NOT _format_elapsed_seconds).
kimix::string format_elapsed_cell(kimix::optional<double> elapsed);
// "\n[Process completed in ...]", or "" when the duration is not reportable.
kimix::string format_completed_banner(kimix::optional<double> elapsed_seconds);

// "output exported to file `{display_path}`" with backslashes normalized.
kimix::string exported_message(kimix::string_view output_path);
kimix::string normalize_display_path(kimix::string_view path);

// Write `content` to `path` (creating the parent directories), the native
// counterpart of the `anyio.open_file(params.output_path, 'w')` blocks in
// _get_output / _kill_task. Returns false when the file could not be written.
bool export_to_file(kimix::string_view path, kimix::string_view content);

// ---------------------------------------------------------------------------
// Finished-task history (background/utils.py FinishedTask / TaskData)
// ---------------------------------------------------------------------------
// One entry of the bounded history. Mirrors FinishedTask (110-137); the
// rtk/summarize side-channels are represented by `original_path`.
struct finished_task_record {
    kimix::string task_id;
    kimix::string output; // raw final output (FinishedTask.output)
    kimix::string processed; // what the caller saw (FinishedTask.processed)
    kimix::string message; // explanatory message (FinishedTask.message)
    bool success = false;
    kimix::optional<int64_t> exit_code;
    kimix::optional<double> elapsed;
    kimix::optional<bool> wait_matched;
    kimix::optional<kimix::string> original_path;
};

// background/utils.py record_finished_task / get_finished_task (565-580):
// bounded (k_max_finished_tasks, oldest evicted first, re-recording an id
// refreshes its recency and fields). Process-wide, mirroring the task
// registry's shape (one process hosts one session's tasks, see the .cpp).
void record_finished_task(const finished_task_record &record);
kimix::optional<finished_task_record> get_finished_task(
    kimix::string_view task_id);
// Test helpers: drop every record / number of retained records.
void clear_finished_tasks();
size_t finished_task_count();

// _get_history_output (__init__.py 275-334): the output text served for a job
// that already left the active registry. `output_path` selects the export
// branch; `wait_matched` is None unless the caller passed a wait_for_pattern
// (matched against the *saved raw* output).
kimix::string build_history_output_text(
    const finished_task_record &record,
    const kimix::optional<kimix::string> &output_path,
    kimix::optional<bool> wait_matched);

// ---------------------------------------------------------------------------
// Tool class
// ---------------------------------------------------------------------------
class JobOutput : public kimix::builtin_tools::Tool {
public:
    explicit JobOutput(kimix::builtin_tools::Session *session);

    void operator()(const ToolParams *parameters) override;
    kimix::vector<char> const &serialized_result() const { return _result; }
    void result_json(kimix::vector<char> &out) const override { out = _result; }

    // Task source. Empty `list` == bind the reproc registry (native_io only).
    TaskSource source;

    // Optional output post-filter (common.py _maybe_export_output_async /
    // _token_filter_output). Identity when unset.
    kimix::function<kimix::string(kimix::string_view)> process_output;

private:
    kimix::vector<char> _result;
};

} // namespace kimix::builtin_tools::job_output
