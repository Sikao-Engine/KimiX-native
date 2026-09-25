// python_tool_class.cpp - Python Tool subclass implementation (real
// subprocess execution through the reproc process runner).
//
// Split out from python_tool.cpp so the pure kernels stay untouched. The
// class mirrors the *shape* of kimi-agent's `python` tool
// (src/kimix/tools/py/__init__.py):
//   * `code`   -> file mode when it names an existing ".py" file
//                 (_resolve_script_source priority 1), otherwise inline code
//                 written to the shared temp folder
//                 (<work_dir>/.kimix_cache/tmp_<pid>/<index>.py, the
//                 ScriptFileWriter naming scheme)
//   * `mode`   -> "execute" (bounded foreground run), "send" (background task)
//                 or "interactive" (`python -i`, persistent task); the
//                 deprecated `run`/`background` values and the hidden
//                 `interactive` bool are normalized like
//                 prompt_common.normalize_mode_validator
//   * `task_id`-> continue an existing task: the `code` is written to its
//                 stdin, then output is collected (mirrors _continue_session)
//   * `output_path` -> tee the captured output to a file
//   * interpreter resolution follows _resolve_python_uncached through the
//     ported resolve_python_exe kernel (see detect_python_exe)
//   * failure messages append the ported module_not_found_hint
//
// What stays in Python (see reports/python.md): the fail-fast `compile()`
// pre-check (_syntax_check_error, needs CPython), long-output summarization
// (LLM call), the rtk/dedup/truncate token-filter pipeline, the
// `_append_elapsed` suffix, and the temp-folder lifecycle.
//
// Unity-build rules: TU-local helpers carry the `pyc_` prefix.

#include "builtin_tools/python_tool.h"

#include <cstdio>
#include <cstdlib>

#include <core/clock.h>

#include "builtin_tools/process_runner.h"
#include "builtin_tools/tool_registry.h"

#ifdef KIMIX_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif

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

long pyc_process_id() {
#ifdef KIMIX_PLATFORM_WINDOWS
    return static_cast<long>(::GetCurrentProcessId());
#else
    return static_cast<long>(::getpid());
#endif
}

bool pyc_is_regular_file(kimix::string_view path) {
    std::error_code ec;
    return kimix::filesystem::is_regular_file(
        kimix::filesystem::path(kimix::string(path)), ec);
}

// Working directory the native session anchors relative paths at.
kimix::string pyc_base_dir(kimix::string_view work_dir) {
    if (!work_dir.empty()) {
        return kimix::string(work_dir);
    }
    std::error_code ec;
    return kimix::to_string(kimix::filesystem::current_path(ec));
}

// common.py _temp_folder: <base>/.kimix_cache/tmp_<pid>.  The reference builds
// the relative Path('.kimix_cache') / f'tmp_{os.getpid()}' against the process
// cwd; the native session anchors it at work_dir (tool.h contract) so the temp
// folder and the child's cwd stay consistent.
kimix::string pyc_temp_dir(kimix::string_view work_dir) {
    kimix::filesystem::path base(pyc_base_dir(work_dir));
    return kimix::to_string(base / ".kimix_cache" /
                            (kimix::string("tmp_") +
                             kimix::format("{}", pyc_process_id())));
}

// common.py _display_temp_path: paths inside the shared temp folder are shown
// relative to the base directory in forward-slash form
// (".kimix_cache/tmp_<pid>/0.py"); every other path is only slash-normalized.
kimix::string pyc_display_path(kimix::string_view path,
                               kimix::string_view temp_dir) {
    kimix::string out(path.data(), path.size());
    if (!temp_dir.empty() && out.size() > temp_dir.size() &&
        out.compare(0, temp_dir.size(), temp_dir.data(), temp_dir.size()) == 0 &&
        (out[temp_dir.size()] == '\\' || out[temp_dir.size()] == '/')) {
        kimix::string rel = out.substr(temp_dir.size() + 1);
        kimix::string base = kimix::string(temp_dir.data(), temp_dir.size());
        // Short form relative to <base_dir>: take the last two components of
        // the temp dir (".kimix_cache/tmp_<pid>").
        for (auto &c : base) {
            if (c == '\\') {
                c = '/';
            }
        }
        size_t cut = base.rfind('/');
        cut = (cut == kimix::string::npos) ? 0 : base.rfind('/', cut - 1);
        if (cut != kimix::string::npos) {
            base = base.substr(cut + 1);
        }
        for (auto &c : rel) {
            if (c == '\\') {
                c = '/';
            }
        }
        return base + "/" + rel;
    }
    for (auto &c : out) {
        if (c == '\\') {
            c = '/';
        }
    }
    return out;
}

// Join a possibly relative script path with the session work directory.
kimix::string pyc_anchor(kimix::string_view path, kimix::string_view work_dir) {
    kimix::filesystem::path p{kimix::string(path)};
    if (p.is_relative() && !work_dir.empty()) {
        p = kimix::filesystem::path{kimix::string(work_dir)} / p;
    }
    return kimix::to_string(p);
}

const char *pyc_source_label(bool is_file_mode) {
    return is_file_mode ? "File" : "Script";
}

// The interpreter Python the native path can fall back to when the reference's
// sys.executable is not observable: the first candidate found on PATH.
kimix::string pyc_python_from_path() {
    namespace fs = kimix::filesystem;
    static const char *kCandidates[] = {"python.exe", "python3.exe", "python",
                                        "python3"};
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
        if (pyc_is_regular_file(c)) {
            return kimix::string(c);
        }
    }
    return {};
}

} // namespace

Python::Python(kimix::builtin_tools::Session *session)
    : kimix::builtin_tools::Tool(session) {}

bool Python::valid() const {
    const kimix::string_view work_dir =
        (_session != nullptr) ? kimix::string_view(_session->work_dir)
                              : kimix::string_view();
    return tool_valid("python", !detect_python_exe(work_dir).empty());
}

kimix::string Python::detect_python_exe() {
    return detect_python_exe(kimix::string_view());
}

// Decision kernel for py/__init__.py _resolve_python_uncached (see the ported
// resolve_python_exe in python_tool.cpp for the pure version):
//   1. KIMIX_PYTHON_EXECUTABLE when set and an existing file.  (PYTHON_EXE is
//      accepted as a port-compatible alias; the reference only knows the
//      KIMIX_* name.)
//   2. <base>/.venv/Scripts/python.exe then <base>/.venv/bin/python, walking up
//      from the session work dir and the process cwd.
//   3. VIRTUAL_ENV: <venv>/Scripts/python.exe then <venv>/bin/python.
//   4. sys.executable — not observable from C++, so the first interpreter found
//      on PATH stands in for it (the pre-port behaviour of this class).
kimix::string Python::detect_python_exe(kimix::string_view work_dir) {
    const auto exists = [](kimix::string_view p) {
        return pyc_is_regular_file(p);
    };
    kimix::string override_exe;
    if (const char *ovr = std::getenv("KIMIX_PYTHON_EXECUTABLE");
        ovr != nullptr && *ovr != '\0') {
        override_exe = kimix::string(ovr);
    } else if (const char *legacy = std::getenv("PYTHON_EXE");
               legacy != nullptr && *legacy != '\0') {
        override_exe = kimix::string(legacy);
    }
    kimix::vector<kimix::string> search_bases;
    if (!work_dir.empty()) {
        search_bases.emplace_back(work_dir);
    }
    std::error_code ec;
    kimix::filesystem::path cwd = kimix::filesystem::current_path(ec);
    if (!ec) {
        search_bases.push_back(kimix::to_string(cwd));
    }
    kimix::string virtual_env;
    if (const char *venv = std::getenv("VIRTUAL_ENV");
        venv != nullptr && *venv != '\0') {
        virtual_env = kimix::string(venv);
    }
    const kimix::string fallback = pyc_python_from_path();
    const kimix::optional<kimix::string> resolved =
        resolve_python_exe(override_exe, search_bases, virtual_env, fallback, exists);
    if (!resolved.has_value()) {
        return {};
    }
    return *resolved;
}

static const kimix::builtin_tools::param_alias k_python_aliases[] = {
    // `code` also accepts the reference's `file` alias (kimi-agent Params uses
    // AliasChoices("code", "source_code", "file")); the canonical `file` key is
    // read as a fallback spelling below.
    {"code",
     "script source source_code code_snippet python_code file file_path path "
     "script_path filename script_file"},
    {"output_path", "output output_file save_path out_path"},
    {"timeout", "timeout_seconds timeout_sec"},
    {"mode", "run_mode execution_mode"},
    {"run_in_background", "background async run_async in_background"},
    {"task_id", "job_id job task"},
    {"wait_for_pattern", "wait_pattern pattern wait_for wait_until"},
    {"max_lines", "max_output_lines lines"},
};

void Python::operator()(kimix::builtin_tools::ToolParams const *parameters) {
    // Fuzzy alias matching (tool.h): wrong-but-reasonable argument names
    // ("command" for "cmd") are accepted; the canonical name always wins.
    const kimix::builtin_tools::ToolParams k_resolved =
        kimix::builtin_tools::ToolParams::with_aliases(parameters, k_python_aliases);
    if (parameters != nullptr) {
        parameters = &k_resolved;
    }
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
    auto get_string = [&](const char *key) {
        kimix::string out;
        if (const ValueElement *v = parameters->get(key);
            v != nullptr && v->is_string()) {
            out = v->as_string();
        }
        return out;
    };
    auto get_bool = [&](const char *key) {
        if (const ValueElement *v = parameters->get(key);
            v != nullptr && v->is_bool()) {
            return v->as_bool();
        }
        return false;
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

    // ---- source -----------------------------------------------------------
    kimix::string code = get_string("code");
    if (code.empty()) {
        code = get_string("file"); // reference alias: file -> code
    }
    const kimix::string task_id = get_string("task_id");
    kimix::string output_path = get_string("output_path");
    kimix::string wait_pattern = get_string("wait_for_pattern");

    // ---- mode (normalize_mode_validator) ----------------------------------
    enum class py_mode : uint8_t { execute, send, interactive };
    py_mode mode = py_mode::execute;
    {
        const kimix::string mode_text = get_string("mode");
        if (mode_text == "send" || mode_text == "background") {
            mode = py_mode::send;
        } else if (mode_text == "interactive") {
            mode = py_mode::interactive;
        }
        // `interactive: true` is the reference's deprecated boolean alias; the
        // `run_in_background` bool is this port's older spelling of mode=send.
        if (get_bool("interactive")) {
            mode = py_mode::interactive;
        } else if (get_bool("run_in_background")) {
            mode = py_mode::send;
        }
    }

    // ---- validation (reference Params._validate_source) -------------------
    if (code.empty() && task_id.empty() && mode != py_mode::interactive) {
        serialize_status(
            tool_status::invalid_input,
            "`code` must be provided (unless mode='interactive' or task_id is "
            "set).");
        return;
    }
    if (!task_id.empty() && code.empty()) {
        serialize_status(tool_status::invalid_input,
                         "code cannot be empty when continuing a session via "
                         "task_id");
        return;
    }

    int64_t timeout_s = 30;
    if (const ValueElement *t = parameters->get("timeout");
        t != nullptr && t->is_int()) {
        timeout_s = t->as_int();
    }
    const int64_t timeout_ms = timeout_s > 0 ? timeout_s * 1000 : 0;

    // ---- continuation: send `code` to an existing task --------------------
    if (!task_id.empty()) {
        kimix::string input_text = code;
        if (input_text.empty() || input_text.back() != '\n') {
            input_text.push_back('\n');
        }
        const tool_error send_err = proc::send_task(task_id, input_text, false);
        if (send_err.failed()) {
            serialize_status(send_err.status,
                             kimix::format("Failed to send input to task '{}'",
                                           task_id));
            return;
        }
        const proc::task_wait_result tw =
            proc::wait_task(task_id, wait_pattern, timeout_ms > 0 ? timeout_ms : 3000);
        kimix::string out;
        proc::read_task(task_id, out);
        const proc::task_status_info info = proc::query_task(task_id);
        const bool alive = info.exists && !tw.exited;
        session_output_block block;
        block.task_id = task_id;
        block.status = alive ? "running" : "completed";
        block.output = out;
        if (!alive && info.exit_code.has_value()) {
            block.exit_code = static_cast<int32_t>(*info.exit_code);
        }
        block.wait_matched =
            tw.matched ? kimix::optional<bool>(true) : kimix::optional<bool>();
        const kimix::string hint_exe = detect_python_exe(_session->work_dir);
        const kimix::string hint =
            hint_exe.empty() ? kimix::string()
                             : module_not_found_hint(out, hint_exe);
        result.values["status"] = ValueElement::make_string(kimix::string("ok"));
        result.values["task_id"] = ValueElement::make_string(task_id);
        result.values["message"] = ValueElement::make_string(kimix::format(
            "Data sent to `{}`. Status: {}.", task_id, block.status) + hint);
        result.values["output"] =
            ValueElement::make_string(build_session_output_block(block));
        result.serialize(_result);
        return;
    }

    // ---- script source (_resolve_script_source) ---------------------------
    kimix::string script_path;
    bool is_file_mode = false;
    if (!code.empty()) {
        if (code.size() > 3 && code.compare(code.size() - 3, 3, ".py") == 0) {
            // Priority 1: `code` names an existing .py file -> run it as-is.
            if (pyc_is_regular_file(code)) {
                script_path = code;
                is_file_mode = true;
            } else {
                const kimix::string anchored = pyc_anchor(code, _session->work_dir);
                if (anchored != code && pyc_is_regular_file(anchored)) {
                    script_path = anchored;
                    is_file_mode = true;
                }
            }
        }
        if (script_path.empty()) {
            // Priority 2: inline code -> temp script in the shared temp folder.
            const kimix::string tmp_dir = pyc_temp_dir(_session->work_dir);
            std::error_code ec;
            kimix::filesystem::create_directories(kimix::filesystem::path(tmp_dir),
                                                  ec);
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
        }
    }
    const kimix::string source_label(pyc_source_label(is_file_mode));
    const kimix::string display_path =
        script_path.empty()
            ? kimix::string()
            : pyc_display_path(script_path, pyc_temp_dir(_session->work_dir));

    // ---- interpreter (after validation + source resolution, like the
    //      reference: _resolve_python_uncached runs inside _execute_code) ----
    const kimix::string python_exe = detect_python_exe(_session->work_dir);
    if (python_exe.empty()) {
        serialize_status(
            tool_status::invalid_input,
            "no python interpreter found (set KIMIX_PYTHON_EXECUTABLE)");
        return;
    }

    proc::run_options opts;
    opts.argv.push_back(python_exe);
    if (mode == py_mode::interactive) {
        opts.argv.push_back("-i");
    }
    if (!script_path.empty()) {
        opts.argv.push_back(script_path);
    }
    opts.working_directory = _session->work_dir;
    opts.wait_pattern = wait_pattern;
    opts.output_cap_chars = 200000;

    // ---- send / interactive: start a persistent task ----------------------
    if (mode != py_mode::execute) {
        opts.timeout_ms = 0;
        proc::task_handle handle;
        const tool_error terr = proc::start_task(opts, handle);
        if (terr.failed()) {
            serialize_status(terr.status, terr.message);
            return;
        }
        if (mode == py_mode::interactive) {
            result.values["status"] = ValueElement::make_string(kimix::string("ok"));
            result.values["task_id"] = ValueElement::make_string(handle.task_id);
            result.values["message"] = ValueElement::make_string(kimix::format(
                "Interactive Python started. task_id: `{}`. Use task_id to send "
                "commands and job_output to read results. Send 'exit()' to "
                "close the session.",
                handle.task_id));
            result.serialize(_result);
            return;
        }
        session_output_block block;
        block.task_id = handle.task_id;
        block.status = "running";
        result.values["status"] = ValueElement::make_string(kimix::string("ok"));
        result.values["task_id"] = ValueElement::make_string(handle.task_id);
        result.values["message"] = ValueElement::make_string(kimix::format(
            "{} saved to `{}`. Running in background. task_id: `{}`. Use "
            "`job_output` tool to retrieve output.",
            source_label, display_path, handle.task_id));
        result.values["output"] =
            ValueElement::make_string(build_session_output_block(block));
        result.serialize(_result);
        return;
    }

    // ---- execute: bounded foreground run ----------------------------------
    opts.timeout_ms = timeout_ms;
    const proc::run_result rr = proc::run_process(opts);
    if (!rr.spawn_error.empty()) {
        serialize_status(tool_status::invalid_input, rr.spawn_error);
        return;
    }
    const kimix::string hint = module_not_found_hint(rr.output, python_exe);

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
                          ? kimix::optional<int32_t>(static_cast<int32_t>(*rr.exit_code))
                          : kimix::optional<int32_t>();
    block.wait_matched = rr.matched ? kimix::optional<bool>(true)
                                    : kimix::optional<bool>();
    block.elapsed_seconds = static_cast<double>(rr.elapsed_ms) / 1000.0;
    block.output_truncated = rr.truncated;

    if (!rr.killed && rr.exit_code.has_value() && *rr.exit_code == 0) {
        const kimix::string message =
            kimix::format("{}: `{}`", source_label, display_path);
        result.values["status"] = ValueElement::make_string(kimix::string("ok"));
        result.values["message"] = ValueElement::make_string(message);
        result.values["output"] =
            ValueElement::make_string(build_session_output_block(block));
        result.values["brief"] = ValueElement::make_string(kimix::format(
            "Python {} executed successfully", is_file_mode ? "file" : "code"));
        if (!output_path.empty()) {
            result.values["output_path"] = ValueElement::make_string(output_path);
        }
        result.serialize(_result);
        return;
    }

    // Failure / timeout: the reference emits a ToolError whose message carries
    // the interpreter (plus the pip hint for a ModuleNotFoundError).  The block
    // keeps the bash convention (`status: failed` + the exit code) because the
    // native result shape has no is_error flag.
    kimix::string message;
    if (rr.killed) {
        message = kimix::format("{} saved to `{}`. Running in background. "
                                "task_id: `python`. use `job_output`",
                                source_label, display_path);
        block.status = "timeout";
    } else {
        message = kimix::format("{}: `{}` failed (interpreter: {})", source_label,
                                display_path, python_exe) +
                  hint;
        block.status = "failed";
    }
    result.values["status"] = ValueElement::make_string(kimix::string("ok"));
    result.values["message"] = ValueElement::make_string(message);
    result.values["output"] =
        ValueElement::make_string(build_session_output_block(block));
    result.values["brief"] = ValueElement::make_string(kimix::string(
        rr.killed ? "Timeout" : "Python execution error"));
    result.serialize(_result);
}

KIMIX_REGISTER_TOOL_NAMED_ALIASED(
    Python, "python",
    "Execute Python code or a .py file (auto-detected via `code`). Inline code "
    "runs through a temp script; modes: 'execute' (bounded foreground run), "
    "'send' (background task) and 'interactive' (persistent REPL).",
    R"JSON({"type":"object","properties":{"code":{"type":"string","description":"Inline Python code to execute. Accepts `code`, `source_code` or `file`; a value ending in '.py' that names an existing file is executed as a script."},"output_path":{"type":"string","description":"Save captured output to this file"},"timeout":{"type":"integer","description":"Timeout in seconds (default 30, max 900)"},"mode":{"type":"string","enum":["execute","send","interactive"],"description":"execute: run and wait for completion; send: background, return task_id; interactive: persistent REPL, return task_id"},"task_id":{"type":"string","description":"Continue an existing session. When set, sends 'code' to stdin instead of running a new script"},"wait_for_pattern":{"type":"string","description":"Pattern to wait for in the tool output"},"max_lines":{"type":"integer","description":"Max lines to return. None = unlimited"}}})JSON",
    "Python py");

} // namespace kimix::builtin_tools::python
