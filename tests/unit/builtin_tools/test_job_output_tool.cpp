// Test for builtin_tools/job_output_tool.h
// (namespace kimix::builtin_tools::job_output).
//
// Covers:
// - parse_params: defaults, job_id|task_id + wait|block + timeout|timeout_ms
//   aliases, the legacy timeout_ms -> max(1, ms // 1000) conversion, the
//   deprecated kill:true -> action='kill' normalization, action validation,
//   the [1, 7200] timeout bounds, and non-string rejection
// - task_kind: split("_")[0] / "unknown"
// - format_task_list: byte-exact markdown table + "No running tasks."
// - not_found_message: "No running task" vs the available-tasks list
// - format_elapsed_cell / format_elapsed_paren / format_completed_banner
// - build_get_output_text: processed vs "(no output)", the still-running
//   prefix, the export branch, original/rtk suffixes, wait_matched line and
//   the completion banner ordering
// - JobOutput Tool wrapper with an injected TaskSource: list mode (explicit
//   action and the job_id-omitted default), get with/without wait, the
//   not-found path, the non-zero-exit failure path, kill, output_path export
//   and the trailing "[status: ...]" contract
//
// All test logic lives in main() scope; no file-scope static registrations.
#include "ut/ut.hpp"
#include <core/kimix_core.h>
#include "builtin_tools/job_output_tool.h"
#include "builtin_tools/process_runner.h"
#include <string>
#include <vector>

#include <string>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools;
using namespace kimix::builtin_tools::job_output;

namespace {

kimix::string kix(std::string_view sv) {
    return kimix::string(sv.data(), sv.size());
}

std::string sv_of(const kimix::string &s) {
    return std::string(s.data(), s.size());
}

std::string json_of(const kimix::vector<char> &buf) {
    return std::string(buf.data(), buf.size());
}

bool has(const kimix::vector<char> &buf, std::string_view needle) {
    return json_of(buf).find(std::string(needle)) != std::string::npos;
}

// A scripted task source: the tests drive it instead of spawning processes.
struct fake_source {
    kimix::vector<job_task> tasks;
    kimix::vector<kimix::string> removed;
    kimix::vector<kimix::string> killed;
    int wait_calls = 0;
    kimix::string last_wait_pattern;
    int64_t last_wait_timeout_ms = 0;
    bool wait_matched = false;

    TaskSource make() {
        // `this` outlives the tool call in every test below.
        TaskSource src;
        src.list = [this]() { return tasks; };
        src.read = [this](kimix::string_view id, job_task &out) {
            for (const job_task &t : tasks) {
                if (t.task_id == id) {
                    out = t;
                    return true;
                }
            }
            return false;
        };
        src.wait = [this](kimix::string_view id, kimix::string_view pattern,
                          int64_t timeout_ms, bool &matched, job_task &out) {
            ++wait_calls;
            last_wait_pattern = kimix::string(pattern);
            last_wait_timeout_ms = timeout_ms;
            matched = wait_matched;
            for (const job_task &t : tasks) {
                if (t.task_id == id) {
                    out = t;
                    return true;
                }
            }
            return false;
        };
        src.kill = [this](kimix::string_view id, job_task &out) {
            for (const job_task &t : tasks) {
                if (t.task_id == id) {
                    killed.emplace_back(id);
                    out = t;
                    out.exited = true;
                    return true;
                }
            }
            return false;
        };
        src.remove = [this](kimix::string_view id) { removed.emplace_back(id); };
        return src;
    }
};

job_task make_task(std::string_view id, bool exited, int64_t exit_code,
                   std::string_view output, double elapsed) {
    job_task t;
    t.task_id = kix(id);
    t.exists = true;
    t.exited = exited;
    if (exited) {
        t.exit_code = exit_code;
    }
    t.output = kix(output);
    t.elapsed_seconds = elapsed;
    t.has_elapsed = true;
    return t;
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    // -----------------------------------------------------------------------
    // parse_params
    // -----------------------------------------------------------------------
    "params_defaults"_test = [] {
        job_output_params out;
        expect(!parse_params(nullptr, out).failed());
        expect(!out.job_id.has_value());
        expect(out.action == kix("get"));
        expect(!out.wait);
        expect(out.timeout == 60_i);
        expect(!out.output_path.has_value());
        expect(!out.wait_for_pattern.has_value());
    };

    "params_task_id_alias"_test = [] {
        ToolParams params;
        params.values["task_id"] = ValueElement::make_string(kix("bash_1"));
        job_output_params out;
        expect(!parse_params(&params, out).failed());
        expect(out.job_id.has_value());
        expect(*out.job_id == kix("bash_1"));
    };

    "params_job_id_wins_over_alias"_test = [] {
        ToolParams params;
        params.values["job_id"] = ValueElement::make_string(kix("a"));
        params.values["task_id"] = ValueElement::make_string(kix("b"));
        job_output_params out;
        expect(!parse_params(&params, out).failed());
        expect(*out.job_id == kix("a"));
    };

    "params_block_alias"_test = [] {
        ToolParams params;
        params.values["block"] = ValueElement::make_bool(true);
        job_output_params out;
        expect(!parse_params(&params, out).failed());
        expect(out.wait);
    };

    "params_legacy_timeout_ms"_test = [] {
        ToolParams params;
        params.values["timeout_ms"] = ValueElement::make_int(45000);
        job_output_params out;
        expect(!parse_params(&params, out).failed());
        expect(out.timeout == 45_i);
    };

    "params_legacy_timeout_ms_floors_at_one"_test = [] {
        ToolParams params;
        params.values["timeout_ms"] = ValueElement::make_int(250);
        job_output_params out;
        expect(!parse_params(&params, out).failed());
        expect(out.timeout == 1_i); // max(1, 250 // 1000)
    };

    "params_timeout_wins_over_timeout_ms"_test = [] {
        ToolParams params;
        params.values["timeout"] = ValueElement::make_int(7);
        params.values["timeout_ms"] = ValueElement::make_int(45000);
        job_output_params out;
        expect(!parse_params(&params, out).failed());
        expect(out.timeout == 7_i);
    };

    "params_timeout_bounds"_test = [] {
        ToolParams params;
        params.values["timeout"] = ValueElement::make_int(0);
        job_output_params out;
        const tool_error low = parse_params(&params, out);
        expect(low.failed());
        expect(sv_of(low.message).find("greater than or equal to 1") !=
               std::string::npos);
        params.values["timeout"] = ValueElement::make_int(7201);
        const tool_error high = parse_params(&params, out);
        expect(high.failed());
        expect(sv_of(high.message).find("less than or equal to 7200") !=
               std::string::npos);
        params.values["timeout"] = ValueElement::make_int(7200);
        expect(!parse_params(&params, out).failed());
        expect(out.timeout == 7200_i);
    };

    "params_kill_flag_normalizes_action"_test = [] {
        ToolParams params;
        params.values["kill"] = ValueElement::make_bool(true);
        params.values["job_id"] = ValueElement::make_string(kix("bash_1"));
        job_output_params out;
        expect(!parse_params(&params, out).failed());
        expect(out.action == kix("kill"));
    };

    "params_bad_action"_test = [] {
        ToolParams params;
        params.values["action"] = ValueElement::make_string(kix("destroy"));
        job_output_params out;
        const tool_error err = parse_params(&params, out);
        expect(err.failed());
        expect(sv_of(err.message).find("'get', 'list' or 'kill'") !=
               std::string::npos);
    };

    "params_bad_job_id_type"_test = [] {
        ToolParams params;
        params.values["job_id"] = ValueElement::make_int(3);
        job_output_params out;
        const tool_error err = parse_params(&params, out);
        expect(err.failed());
        expect(err.message == kix("job_id must be a string"));
    };

    // -----------------------------------------------------------------------
    // task_kind / elapsed formatting
    // -----------------------------------------------------------------------
    "task_kind_splits_on_underscore"_test = [] {
        expect(task_kind("bash_1") == kix("bash"));
        expect(task_kind("run_git") == kix("run"));
        expect(task_kind("python_2") == kix("python"));
        expect(task_kind("noue") == kix("unknown"));
        expect(task_kind("_leading") == kix(""));
        expect(task_kind("") == kix("unknown"));
    };

    "elapsed_cell_formatting"_test = [] {
        expect(format_elapsed_cell(std::nullopt) == kix("-"));
        expect(format_elapsed_cell(0.0) == kix("-")); // falsy in Python
        expect(format_elapsed_cell(1.25) == kix("1.2s"));
        expect(format_elapsed_cell(1.35) == kix("1.4s"));
        expect(format_elapsed_cell(12.0) == kix("12.0s"));
    };

    "elapsed_paren_formatting"_test = [] {
        expect(format_elapsed_paren(std::nullopt).empty());
        expect(format_elapsed_paren(3.14159) == kix(" (3.1s)"));
    };

    "completed_banner_formatting"_test = [] {
        expect(format_completed_banner(1.005) ==
               kix("\n[Process completed in 1.00s]"));
        // 12.345 is not exactly representable; both CPython and std::format
        // round the exact binary value half-to-even to 12.35.
        expect(format_completed_banner(12.345) ==
               kix("\n[Process completed in 12.35s]"));
    };

    "display_path_normalization"_test = [] {
        expect(normalize_display_path("C:\\tmp\\out.txt") ==
               kix("C:/tmp/out.txt"));
        expect(normalize_display_path("/tmp/out.txt") == kix("/tmp/out.txt"));
        expect(exported_message("C:\\a\\b.txt") ==
               kix("output exported to file `C:/a/b.txt`"));
    };

    "status_suffix_contract"_test = [] {
        expect(status_suffix("running") == kix("\n[status: running]"));
        expect(status_suffix("completed") == kix("\n[status: completed]"));
    };

    // -----------------------------------------------------------------------
    // format_task_list / not_found_message
    // -----------------------------------------------------------------------
    "task_list_empty_message"_test = [] {
        expect(format_task_list(kimix::span<const task_row>()) ==
               kix("No running tasks."));
    };

    "task_list_markdown_table"_test = [] {
        const kimix::vector<task_row> rows = {
            make_task_row("bash_1", true, 2.5),
            make_task_row("run_git", false, 0.0),
            make_task_row("nounderscore", true, std::nullopt),
        };
        const std::string table = sv_of(
            format_task_list(kimix::span<const task_row>(rows)));
        const std::string expected =
            "| Task ID | Kind | Status | Elapsed |\n"
            "|---------|------|--------|---------|\n"
            "| `bash_1` | bash | running | 2.5s |\n"
            "| `run_git` | run | completed | - |\n"
            "| `nounderscore` | unknown | running | - |";
        expect(table == expected) << "got:\n" << table;
    };

    "not_found_messages"_test = [] {
        expect(not_found_message("bash_9", kimix::span<const kimix::string>()) ==
               kix("No running task"));
        const kimix::vector<kimix::string> available = {kix("bash_1"),
                                                        kix("run_git")};
        expect(not_found_message(
                   "bash_9", kimix::span<const kimix::string>(available)) ==
               kix("Task 'bash_9' not found. Available tasks: [bash_1, "
                   "run_git]"));
    };

    // -----------------------------------------------------------------------
    // build_get_output_text
    // -----------------------------------------------------------------------
    "get_output_text_plain"_test = [] {
        get_output_fields f;
        f.processed = "hello";
        f.task_alive = true;
        f.job_id = "bash_1";
        expect(build_get_output_text(f) == kix("hello"));
    };

    "get_output_text_no_output_fallback"_test = [] {
        get_output_fields f;
        f.task_alive = false;
        f.job_id = "bash_1";
        expect(build_get_output_text(f) == kix("(no output)"));
    };

    "get_output_text_export_branch"_test = [] {
        get_output_fields f;
        f.processed = "hello";
        f.task_alive = true;
        f.output_path = kimix::string("C:\\out.txt");
        f.job_id = "bash_1";
        expect(build_get_output_text(f) ==
               kix("`bash_1` is still running, call `job_output` again, "
                   "output exported to file `C:/out.txt`"));
        // A finished task drops the still-running prefix.
        f.task_alive = false;
        expect(build_get_output_text(f) ==
               kix("output exported to file `C:/out.txt`"));
    };

    "get_output_text_suffix_ordering"_test = [] {
        get_output_fields f;
        f.processed = "body";
        f.task_alive = false;
        f.original_path = kimix::string("C:\\orig.txt");
        f.has_formatter = true;
        f.wait_matched = false;
        f.elapsed_seconds = 2.5;
        f.job_id = "bash_1";
        const std::string text = sv_of(build_get_output_text(f));
        const std::string expected =
            "body\n"
            "[original output exported to: C:/orig.txt]\n"
            "wait_matched: false\n"
            "[Process completed in 2.50s]";
        expect(text == expected) << "got:\n" << text;
        // Without a registered formatter the rtk wording is used.
        f.has_formatter = false;
        expect(sv_of(build_get_output_text(f)).find(
                   "[rtk output exported to: C:/orig.txt]") !=
               std::string::npos);
    };

    "get_output_text_wait_matched_true"_test = [] {
        get_output_fields f;
        f.processed = "x";
        f.task_alive = true;
        f.wait_matched = true;
        f.job_id = "t";
        expect(sv_of(build_get_output_text(f)).find("\nwait_matched: true") !=
               std::string::npos);
    };

    // -----------------------------------------------------------------------
    // JobOutput Tool wrapper (injected TaskSource)
    // -----------------------------------------------------------------------
    "tool_requires_native_io_without_source"_test = [] {
        Session session; // native_io false, no injected source
        JobOutput tool(&session);
        ToolParams params;
        params.values["action"] = ValueElement::make_string(kix("list"));
        tool(&params);
        expect(has(tool.serialized_result(), "unsupported"));
    };

    "tool_list_action"_test = [] {
        Session session;
        fake_source fake;
        fake.tasks = {make_task("bash_1", false, 0, "", 1.5),
                      make_task("run_git", true, 0, "done", 3.0)};
        JobOutput tool(&session);
        tool.source = fake.make();
        ToolParams params;
        params.values["action"] = ValueElement::make_string(kix("list"));
        tool(&params);
        expect(has(tool.serialized_result(), "| Task ID | Kind | Status | Elapsed |"));
        expect(has(tool.serialized_result(), "`bash_1` | bash | running | 1.5s"));
        expect(has(tool.serialized_result(),
                   "`run_git` | run | completed | 3.0s"));
        expect(has(tool.serialized_result(), "2 background task(s)"));
    };

    "tool_list_is_the_default_without_job_id"_test = [] {
        Session session;
        fake_source fake;
        fake.tasks = {make_task("bash_1", false, 0, "", 0.0)};
        JobOutput tool(&session);
        tool.source = fake.make();
        tool(nullptr); // no params at all -> action=get, job_id absent
        expect(has(tool.serialized_result(), "| Task ID | Kind | Status |"));
        expect(has(tool.serialized_result(), "1 background task(s)"));
    };

    "tool_list_empty"_test = [] {
        Session session;
        fake_source fake;
        JobOutput tool(&session);
        tool.source = fake.make();
        ToolParams params;
        params.values["action"] = ValueElement::make_string(kix("list"));
        tool(&params);
        expect(has(tool.serialized_result(), "No running tasks."));
        expect(has(tool.serialized_result(), "No background tasks"));
    };

    "tool_get_running_task"_test = [] {
        Session session;
        fake_source fake;
        fake.tasks = {make_task("bash_1", false, 0, "partial output", 1.0)};
        JobOutput tool(&session);
        tool.source = fake.make();
        ToolParams params;
        params.values["job_id"] = ValueElement::make_string(kix("bash_1"));
        tool(&params);
        const std::string json = json_of(tool.serialized_result());
        expect(json.find("partial output") != std::string::npos);
        expect(json.find("[status: running]") != std::string::npos);
        expect(json.find("\"kind\":\"bash\"") != std::string::npos);
        expect(json.find("Task output retrieved") != std::string::npos);
        // A finished task is dropped from the registry; a running one is not.
        expect(fake.removed.empty());
    };

    "tool_get_completed_task_removes_id"_test = [] {
        Session session;
        fake_source fake;
        fake.tasks = {make_task("run_git", true, 0, "all done", 2.5)};
        JobOutput tool(&session);
        tool.source = fake.make();
        ToolParams params;
        params.values["job_id"] = ValueElement::make_string(kix("run_git"));
        tool(&params);
        const std::string json = json_of(tool.serialized_result());
        expect(json.find("all done") != std::string::npos);
        expect(json.find("[status: completed]") != std::string::npos);
        expect(json.find("[Process completed in 2.50s]") != std::string::npos);
        expect(fake.removed.size() == 1u);
        expect(fake.removed[0] == kix("run_git"));
    };

    "tool_get_no_output_fallback"_test = [] {
        Session session;
        fake_source fake;
        fake.tasks = {make_task("bash_2", true, 0, "", 0.0)};
        JobOutput tool(&session);
        tool.source = fake.make();
        ToolParams params;
        params.values["job_id"] = ValueElement::make_string(kix("bash_2"));
        tool(&params);
        expect(has(tool.serialized_result(), "(no output)"));
    };

    "tool_get_failed_task"_test = [] {
        Session session;
        fake_source fake;
        fake.tasks = {make_task("run_make", true, 2, "boom", 7.25)};
        JobOutput tool(&session);
        tool.source = fake.make();
        ToolParams params;
        params.values["job_id"] = ValueElement::make_string(kix("run_make"));
        tool(&params);
        const std::string json = json_of(tool.serialized_result());
        expect(json.find("\"ok\":false") != std::string::npos);
        expect(json.find("Task 'run_make' failed") != std::string::npos);
        expect(json.find(" (7.2s)") != std::string::npos)
            << "message was: " << json.c_str();
        expect(json.find("boom") != std::string::npos);
        expect(json.find("[status: completed]") != std::string::npos);
    };

    "tool_get_not_found_lists_available"_test = [] {
        Session session;
        fake_source fake;
        fake.tasks = {make_task("bash_1", false, 0, "", 0.0),
                      make_task("bash_2", false, 0, "", 0.0)};
        JobOutput tool(&session);
        tool.source = fake.make();
        ToolParams params;
        params.values["job_id"] = ValueElement::make_string(kix("nope"));
        tool(&params);
        const std::string json = json_of(tool.serialized_result());
        expect(json.find("Task 'nope' not found. Available tasks: [bash_1, "
                         "bash_2]") != std::string::npos);
        expect(json.find("not_found") != std::string::npos);
    };

    "tool_get_not_found_no_tasks"_test = [] {
        Session session;
        fake_source fake;
        JobOutput tool(&session);
        tool.source = fake.make();
        ToolParams params;
        params.values["job_id"] = ValueElement::make_string(kix("nope"));
        tool(&params);
        expect(has(tool.serialized_result(), "No running task"));
    };

    "tool_get_strips_the_id"_test = [] {
        Session session;
        fake_source fake;
        fake.tasks = {make_task("bash_5", false, 0, "x", 0.0)};
        JobOutput tool(&session);
        tool.source = fake.make();
        ToolParams params;
        params.values["job_id"] = ValueElement::make_string(kix("  bash_5  "));
        tool(&params);
        expect(has(tool.serialized_result(), "[status: running]"));
        expect(!has(tool.serialized_result(), "not found"));
    };

    "tool_wait_passes_timeout_and_pattern"_test = [] {
        Session session;
        fake_source fake;
        fake.tasks = {make_task("bash_6", false, 0, "ready now", 0.0)};
        fake.wait_matched = true;
        JobOutput tool(&session);
        tool.source = fake.make();
        ToolParams params;
        params.values["job_id"] = ValueElement::make_string(kix("bash_6"));
        params.values["wait"] = ValueElement::make_bool(true);
        params.values["timeout"] = ValueElement::make_int(12);
        params.values["wait_for_pattern"] =
            ValueElement::make_string(kix("ready"));
        tool(&params);
        expect(fake.wait_calls == 1);
        expect(fake.last_wait_pattern == kix("ready"));
        expect(fake.last_wait_timeout_ms == 12000_i);
        expect(has(tool.serialized_result(), "wait_matched: true"));
    };

    "tool_wait_without_pattern_waits_for_exit"_test = [] {
        Session session;
        fake_source fake;
        fake.tasks = {make_task("bash_7", true, 0, "final", 1.0)};
        JobOutput tool(&session);
        tool.source = fake.make();
        ToolParams params;
        params.values["job_id"] = ValueElement::make_string(kix("bash_7"));
        params.values["block"] = ValueElement::make_bool(true);
        tool(&params);
        expect(fake.wait_calls == 1);
        expect(fake.last_wait_pattern.empty());
        // No pattern -> no wait_matched line at all.
        expect(!has(tool.serialized_result(), "wait_matched"));
    };

    "tool_invalid_wait_pattern"_test = [] {
        Session session;
        fake_source fake;
        fake.tasks = {make_task("bash_8", false, 0, "", 0.0)};
        JobOutput tool(&session);
        tool.source = fake.make();
        ToolParams params;
        params.values["job_id"] = ValueElement::make_string(kix("bash_8"));
        params.values["wait_for_pattern"] =
            ValueElement::make_string(kix("([unclosed"));
        tool(&params);
        const std::string json = json_of(tool.serialized_result());
        expect(json.find("Invalid wait_for_pattern: ") != std::string::npos);
        expect(json.find("Invalid pattern") != std::string::npos);
        // The pattern is validated even when wait is false, so no read.
        expect(fake.wait_calls == 0);
    };

    "tool_kill_success"_test = [] {
        Session session;
        fake_source fake;
        fake.tasks = {make_task("bash_9", false, 0, "killed output", 4.0)};
        JobOutput tool(&session);
        tool.source = fake.make();
        ToolParams params;
        params.values["job_id"] = ValueElement::make_string(kix("bash_9"));
        params.values["action"] = ValueElement::make_string(kix("kill"));
        tool(&params);
        const std::string json = json_of(tool.serialized_result());
        expect(json.find("Task 'bash_9' killed") != std::string::npos);
        expect(json.find("killed output") != std::string::npos);
        expect(fake.killed.size() == 1u);
        expect(fake.removed.size() == 1u);
    };

    "tool_kill_non_zero_exit"_test = [] {
        Session session;
        fake_source fake;
        job_task failed = make_task("run_x", false, 0, "err", 6.0);
        failed.exit_code = 3; // killed with a non-zero code
        fake.tasks = {failed};
        JobOutput tool(&session);
        tool.source = fake.make();
        ToolParams params;
        params.values["job_id"] = ValueElement::make_string(kix("run_x"));
        params.values["kill"] = ValueElement::make_bool(true); // deprecated flag
        tool(&params);
        const std::string json = json_of(tool.serialized_result());
        expect(json.find("killed (non-zero exit)") != std::string::npos);
        expect(json.find(" (6.0s)") != std::string::npos);
    };

    "tool_kill_missing_id"_test = [] {
        Session session;
        fake_source fake;
        JobOutput tool(&session);
        tool.source = fake.make();
        ToolParams params;
        params.values["action"] = ValueElement::make_string(kix("kill"));
        tool(&params);
        expect(has(tool.serialized_result(),
                   "task_id is required for action='kill'."));
        expect(has(tool.serialized_result(), "Missing task_id"));
    };

    "tool_kill_not_found"_test = [] {
        Session session;
        fake_source fake;
        fake.tasks = {make_task("bash_1", false, 0, "", 0.0)};
        JobOutput tool(&session);
        tool.source = fake.make();
        ToolParams params;
        params.values["job_id"] = ValueElement::make_string(kix("ghost"));
        params.values["action"] = ValueElement::make_string(kix("kill"));
        tool(&params);
        expect(has(tool.serialized_result(),
                   "Task 'ghost' not found. Available tasks: [bash_1]"));
    };

    "tool_output_path_export"_test = [] {
        Session session;
        fake_source fake;
        fake.tasks = {make_task("bash_10", false, 0, "exported body", 0.0)};
        JobOutput tool(&session);
        tool.source = fake.make();
        const kimix::string out_path =
            kimix::to_string(kimix::filesystem::temp_directory_path() /
                             kimix::filesystem::path("jo_test_export.txt"));
        ToolParams params;
        params.values["job_id"] = ValueElement::make_string(kix("bash_10"));
        params.values["output_path"] = ValueElement::make_string(out_path);
        tool(&params);
        const std::string json = json_of(tool.serialized_result());
        expect(json.find("output exported to file `") != std::string::npos);
        expect(json.find("is still running, call `job_output` again") !=
               std::string::npos);
        // The raw output landed in the file.
        std::FILE *f = std::fopen(sv_of(out_path).c_str(), "rb");
        expect(f != nullptr);
        if (f != nullptr) {
            char buf[64] = {0};
            const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
            std::fclose(f);
            expect(std::string(buf, n).find("exported body") !=
                   std::string::npos);
        }
        std::remove(sv_of(out_path).c_str());
    };

    "tool_process_output_hook"_test = [] {
        Session session;
        fake_source fake;
        fake.tasks = {make_task("bash_11", true, 0, "raw", 1.0)};
        JobOutput tool(&session);
        tool.source = fake.make();
        tool.process_output = [](kimix::string_view text) {
            return kimix::string("[filtered]") + kimix::string(text);
        };
        ToolParams params;
        params.values["job_id"] = ValueElement::make_string(kix("bash_11"));
        tool(&params);
        expect(has(tool.serialized_result(), "[filtered]raw"));
    };

      "tool_description_is_capped"_test = [] {
          Session session;
          fake_source fake;
          fake.tasks = {make_task("bash_12", false, 0,
                                  kimix::string(400, 'q'), 0.0)};
          JobOutput tool(&session);
          tool.source = fake.make();
          ToolParams params;
          params.values["job_id"] = ValueElement::make_string(kix("bash_12"));
          tool(&params);
          const std::string json = json_of(tool.serialized_result());
          // The display-block description mirrors output_text[:200].
          const size_t at = json.find("\"description\":\"");
          expect(at != std::string::npos);
          if (at != std::string::npos) {
              const size_t end = json.find('"', at + 15);
              expect(end - (at + 15) <= 200u);
          }
      };

      // --- proc::sanitize_utf8 (captured-output UTF-8 hardening) ------------
      // Regression: a Chinese-locale timeout.exe emits GBK bytes into the
      // captured output; ToolParams::serialize (yyjson) rejects invalid UTF-8
      // and the whole tool result became a hard "tool threw" error.
      "sanitize_utf8_ascii_passthrough"_test = [] {
          expect(proc::sanitize_utf8("") == kimix::string());
          expect(proc::sanitize_utf8("hello world") ==
                 kimix::string("hello world"));
      };
      "sanitize_utf8_valid_multibyte_preserved"_test = [] {
          const kimix::string in = "h\xC3\xA9llo \xE4\xB8\xAD"; // héllo 中
          expect(proc::sanitize_utf8(in) == in);
          const kimix::string in2 = "\xF0\x9F\x98\x80\xC3\xA9"; // 😀 é
          expect(proc::sanitize_utf8(in2) == in2);
      };
      "sanitize_utf8_gbk_bytes_become_replacement"_test = [] {
          // The exact GBK bytes ("错误") captured from timeout.exe in the e2e.
          const kimix::string in = "\xB4\xED\xCE\xF3";
          const kimix::string want =
              "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD";
          expect(proc::sanitize_utf8(in) == want);
      };
      "sanitize_utf8_invalid_lead_between_ascii"_test = [] {
          expect(proc::sanitize_utf8("a\xFFz") ==
                 kimix::string("a\xEF\xBF\xBDz"));
      };
      "sanitize_utf8_truncated_tail_one_replacement"_test = [] {
          expect(proc::sanitize_utf8("\xE4\xB8") == kimix::string("\xEF\xBF\xBD"));
          expect(proc::sanitize_utf8("ab\xF0\x9F") ==
                 kimix::string("ab\xEF\xBF\xBD"));
      };
      "sanitize_utf8_surrogate_range_rejected"_test = [] {
          // CESU-8 surrogate encoding is not a valid scalar value: every
          // byte is rejected individually.
          expect(proc::sanitize_utf8("\xED\xA0\x80") ==
                 kimix::string("\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"));
      };
      "sanitize_utf8_embedded_nul_preserved"_test = [] {
          const kimix::string in("a\0\xB4z", 4);
          const kimix::string want("a\0\xEF\xBF\xBDz", 6);
          expect(proc::sanitize_utf8(in) == want);
      };
  }
