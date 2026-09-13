// python_tool_class.cpp - Python Tool subclass implementation (real
// subprocess execution through the reproc process runner).
//
// Split out from python_tool.cpp so the pure kernels stay untouched. The
// class:
//   * `code`   -> writes a temp script under <work_dir>/.kimix_cache/pytmp_<pid>/
//                 (ScriptFileWriter naming scheme) and runs `<python> <script>`
//   * `file`   -> runs an existing script path
//   * `run_in_background` -> proc::start_task; later calls address the task
//     through `task_id` (send input via proc::send_task is not exposed here;
//     use job_output-style polling with wait_for_pattern)
//   * `output_path` -> tee the captured output to a file
// Unity-build rules: TU-local helpers carry the `pyc_` prefix.

#include "builtin_tools/python_tool.h"

#include <cstdio>

#include <core/clock.h>

#include "builtin_tools/process_runner.h"
#include "builtin_tools/tool_registry.h"

namespace kimix::builtin_tools::python {

namespace {

const char *pyc_status_string(tool_status s) noexcept {
    switch (s) {
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

kimix::string pyc_temp_dir(kimix::string_view work_dir) {
    kimix::filesystem::path base;
    if (work_dir.empty()) {
        base = kimix::filesystem::current_path();
    } else {
        base = kimix::filesystem::path(kimix::string(work_dir));
    }
    return kimix::to_string(base / ".kimix_cache" / "pytmp");
}

} // namespace

Python::Python(kimix::builtin_tools::Session *session)
    : kimix::builtin_tools::Tool(session) {}

kimix::string Python::detect_python_exe() {
    namespace fs = kimix::filesystem;
    const auto exists = [](kimix::string_view p) {
        std::error_code ec;
        return fs::is_regular_file(fs::path(kimix::string(p)), ec);
    };
    if (const char *ovr = std::getenv("PYTHON_EXE")) {
        if (*ovr != '\0' && exists(ovr)) {
            return kimix::string(ovr);
        }
    }
    static const char *kCandidates[] = {
        "python.exe", "python3.exe", "python", "python3"};
    const char *path_env = std::getenv("PATH");
    if (path_env != nullptr) {
        kimix::string_view rest(path_env);
#ifdef KIMIX_PLATFORM_WINDOWS
        const char sep = ';';
#else
        const char sep = ':';
#endif
        while (!rest.empty()) {
            const size_t colon = rest.find(sep);
            const kimix::string_view dir =
                (colon == kimix::string_view::npos) ? rest : rest.substr(0, colon);
            for (const char *c : kCandidates) {
                fs::path cand = fs::path(kimix::string(dir)) / c;
                std::error_code ec;
                if (fs::exists(cand, ec)) {
                    return kimix::to_string(cand);
                }
            }
            if (colon == kimix::string_view::npos) {
                break;
            }
            rest.remove_prefix(colon + 1);
        }
    }
    for (const char *c : kCandidates) {
        if (exists(c)) {
            return kimix::string(c);
        }
    }
    return {};
}

void Python::operator()(kimix::builtin_tools::ToolParams const *parameters) {
    using kimix::builtin_tools::ToolParams;
    using kimix::builtin_tools::ValueElement;
    _result.clear();
    ToolParams result;
    auto serialize_status = [&](tool_status st, kimix::string_view msg) {
        result.values["status"] =
            ValueElement::make_string(kimix::string(pyc_status_string(st)));
        result.values["message"] = ValueElement::make_string(kimix::string(msg));
        result.serialize(_result);
    };

    if (parameters == nullptr) {
        serialize_status(tool_status::invalid_input, "missing parameters");
        return;
    }
    if (_session == nullptr || !_session->native_io) {
        serialize_status(tool_status::unsupported,
                         "native python execution requires a native_io session");
        return;
    }
    const kimix::string python_exe = detect_python_exe();
    if (python_exe.empty()) {
        serialize_status(tool_status::invalid_input,
                         "no python interpreter found (set PYTHON_EXE)");
        return;
    }

    kimix::string code;
    kimix::string file;
    if (const ValueElement *c = parameters->get("code");
        c != nullptr && c->is_string()) {
        code = c->as_string();
    }
    if (const ValueElement *f = parameters->get("file");
        f != nullptr && f->is_string()) {
        file = f->as_string();
    }
    if (code.empty() && file.empty()) {
        serialize_status(tool_status::invalid_input,
                         "either 'code' or 'file' is required");
        return;
    }
    bool background = false;
    if (const ValueElement *b = parameters->get("run_in_background");
        b != nullptr && b->is_bool()) {
        background = b->as_bool();
    }
    int64_t timeout_s = 30;
    if (const ValueElement *t = parameters->get("timeout");
        t != nullptr && t->is_int()) {
        timeout_s = t->as_int();
    }
    kimix::string wait_pattern;
    if (const ValueElement *w = parameters->get("wait_for_pattern");
        w != nullptr && w->is_string()) {
        wait_pattern = w->as_string();
    }
    kimix::string output_path;
    if (const ValueElement *o = parameters->get("output_path");
        o != nullptr && o->is_string()) {
        output_path = o->as_string();
    }

    // Resolve the script path: explicit file, or write the inline code to the
    // shared temp folder (ScriptFileWriter naming scheme).
    kimix::string script_path = file;
    if (!code.empty()) {
        const kimix::string tmp_dir = pyc_temp_dir(_session->work_dir);
        std::error_code ec;
        kimix::filesystem::create_directories(kimix::filesystem::path(tmp_dir), ec);
        ScriptFileWriter writer(tmp_dir);
        script_path = writer.plan_path(".py");
        std::FILE *f = std::fopen(script_path.c_str(), "wb");
        if (f == nullptr) {
            serialize_status(tool_status::invalid_input,
                             "cannot write temp script: " + script_path);
            return;
        }
        std::fwrite(code.data(), 1, code.size(), f);
        std::fclose(f);
    } else {
        kimix::filesystem::path p(script_path);
        if (p.is_relative() && !_session->work_dir.empty()) {
            p = kimix::filesystem::path(_session->work_dir) / p;
            script_path = kimix::to_string(p);
        }
        std::error_code ec;
        if (!kimix::filesystem::exists(p, ec)) {
            serialize_status(tool_status::not_found,
                             "script file not found: " + script_path);
            return;
        }
    }

    proc::run_options opts;
    opts.argv.push_back(python_exe);
    opts.argv.push_back(script_path);
    opts.working_directory = _session->work_dir;
    opts.wait_pattern = wait_pattern;
    opts.output_cap_chars = 200000;

    if (background) {
        opts.timeout_ms = 0;
        proc::task_handle handle;
        const tool_error terr = proc::start_task(opts, handle);
        if (terr.failed()) {
            serialize_status(terr.status, terr.message);
            return;
        }
        session_output_block block;
        block.task_id = handle.task_id;
        block.status = "running";
        block.output = kimix::format("python task started (pid {}), script: {}",
                                     handle.pid, script_path);
        result.values["status"] = ValueElement::make_string(kimix::string("ok"));
        result.values["task_id"] = ValueElement::make_string(handle.task_id);
        result.values["message"] = ValueElement::make_string(kimix::string("started"));
        result.values["output"] =
            ValueElement::make_string(build_session_output_block(block));
        result.serialize(_result);
        return;
    }

    // Background continuation: task_id set -> poll an existing task.
    if (const ValueElement *tid = parameters->get("task_id");
        tid != nullptr && tid->is_string() && !tid->as_string().empty()) {
        const kimix::string &task_id = tid->as_string();
        const int64_t wait_ms =
            timeout_s > 0 ? timeout_s * 1000 : (wait_pattern.empty() ? 3000 : 30000);
        const proc::task_wait_result tw = proc::wait_task(task_id, wait_pattern, wait_ms);
        kimix::string out;
        proc::read_task(task_id, out);
        const proc::task_status_info info = proc::query_task(task_id);
        session_output_block block;
        block.task_id = task_id;
        block.status = tw.exited ? "completed" : "running";
        block.output = out;
        if (tw.exited && info.exit_code.has_value()) {
            block.exit_code = static_cast<int32_t>(*info.exit_code);
        }
        block.wait_matched =
            tw.matched ? std::optional<bool>(true) : std::nullopt;
        block.elapsed_seconds = static_cast<double>(tw.elapsed_ms) / 1000.0;
        result.values["status"] = ValueElement::make_string(kimix::string("ok"));
        result.values["task_id"] = ValueElement::make_string(task_id);
        result.values["message"] = ValueElement::make_string(kimix::string("job_output"));
        result.values["output"] =
            ValueElement::make_string(build_session_output_block(block));
        result.serialize(_result);
        return;
    }

    opts.timeout_ms = timeout_s > 0 ? timeout_s * 1000 : 0;
    const proc::run_result rr = proc::run_process(opts);
    if (!rr.spawn_error.empty()) {
        serialize_status(tool_status::invalid_input, rr.spawn_error);
        return;
    }
    if (!output_path.empty()) {
        kimix::filesystem::path op(output_path);
        if (op.is_relative() && !_session->work_dir.empty()) {
            op = kimix::filesystem::path(_session->work_dir) / op;
        }
        std::error_code ec;
        const kimix::filesystem::path parent = op.parent_path();
        if (!parent.empty()) {
            kimix::filesystem::create_directories(parent, ec);
        }
        std::FILE *of = std::fopen(kimix::to_string(op).c_str(), "wb");
        if (of != nullptr) {
            std::fwrite(rr.output.data(), 1, rr.output.size(), of);
            std::fclose(of);
            output_path = kimix::to_string(op);
        }
    }
    session_output_block block;
    block.task_id = "python";
    block.status = rr.killed ? "timeout" : "completed";
    block.output = rr.output;
    block.exit_code = rr.exit_code.has_value()
                          ? std::optional<int32_t>(static_cast<int32_t>(*rr.exit_code))
                          : std::nullopt;
    block.wait_matched = rr.matched ? std::optional<bool>(true) : std::nullopt;
    block.elapsed_seconds = static_cast<double>(rr.elapsed_ms) / 1000.0;
    block.output_truncated = rr.truncated;
    result.values["status"] = ValueElement::make_string(kimix::string("ok"));
    result.values["message"] = ValueElement::make_string(kimix::string("ran python"));
    result.values["output"] =
        ValueElement::make_string(build_session_output_block(block));
    if (!output_path.empty()) {
        result.values["output_path"] = ValueElement::make_string(output_path);
    }
    result.serialize(_result);
}

KIMIX_REGISTER_TOOL(
    Python,
    "Execute Python code or a .py file. Inline code runs through a temp "
    "script; supports background tasks (run_in_background + task_id polling), "
    "wait_for_pattern and output_path.",
    R"JSON({"type":"object","properties":{"code":{"type":"string","description":"Inline Python code to execute"},"file":{"type":"string","description":"Path to a .py file (auto-detected when 'code' ends with .py and exists)"},"output_path":{"type":"string","description":"Save captured output to this file"},"timeout":{"type":"integer","description":"Timeout seconds (default 30)"},"run_in_background":{"type":"boolean","description":"Run detached and return a task_id"},"task_id":{"type":"string","description":"Poll/collect output of a background task"},"wait_for_pattern":{"type":"string","description":"Block until this literal appears in output"}}})JSON");

} // namespace kimix::builtin_tools::python
