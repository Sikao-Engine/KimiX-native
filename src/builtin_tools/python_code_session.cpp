// python_code_session.cpp - The persistent /code exec context
// (see python_code_session.h).

#include "builtin_tools/python_code_session.h"

#include <cstdio>
#include <ctime>
#include <thread>
#include <chrono>

#ifdef KIMIX_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "builtin_tools/process_runner.h"
#include "builtin_tools/python_tool.h"
#include <llm/yyjson_alc.h>
#include "yyjson.h"

namespace kimix::builtin_tools::python {

namespace {

constexpr int64_t kHandshakeTimeoutMs = 15000;
constexpr int64_t kExecTimeoutMs = 600000; // 10 min per script (bounded waits)
constexpr int64_t kOutputCapChars = 400000;

size_t process_id() {
#ifdef KIMIX_PLATFORM_WINDOWS
    return static_cast<size_t>(::GetCurrentProcessId());
#else
    return static_cast<size_t>(::getpid());
#endif
}

// Read and consume the child's buffered output.
void drain(kimix::string_view task_id, kimix::string &out) {
    if (proc::read_task(task_id, out).failed()) {
        out.clear();
    }
}

// Parse the LAST "@CODE_RESULT@" line in `blob` into `out` (stray prints that
// bypassed the child's redirect land on the real stdout and never carry the
// marker).
bool last_response(const kimix::string &blob, exec_result &out) {
    const size_t pos = blob.rfind("@CODE_RESULT@");
    if (pos == kimix::string::npos) {
        return false;
    }
    const size_t begin = pos + sizeof("@CODE_RESULT@") - 1;
    const size_t end = blob.find('\n', begin);
    if (end == kimix::string::npos) {
        return CodeExecSession::parse_response(blob.substr(begin), out);
    }
    return CodeExecSession::parse_response(blob.substr(begin, end - begin), out);
}


// Poll read_task until one complete "@CODE_RESULT@" response arrives or the
// deadline expires. (proc::wait_task scans the FULL accumulated buffer, so a
// marker from an earlier response would make it return immediately - the
// pending-only cursor of read_task is what defines "a new response".)
bool wait_response(kimix::string_view task_id, int64_t timeout_ms,
                   kimix::string &diagnostics, exec_result &out) {
    kimix::string acc;
    const int64_t deadline =
        static_cast<int64_t>(std::time(nullptr)) * 1000 + timeout_ms;
    while (true) {
        kimix::string chunk;
        if (!proc::read_task(task_id, chunk).failed()) {
            acc += chunk;
        }
        if (last_response(acc, out)) {
            return true;
        }
        if (static_cast<int64_t>(std::time(nullptr)) * 1000 >= deadline) {
            diagnostics = acc;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

} // namespace

bool CodeExecSession::parse_response(kimix::string_view line, exec_result &out) {
    out = exec_result{};
    if (line.empty()) {
        return false;
    }
    // Tolerate (and skip) a leading marker so callers can pass raw lines.
    if (line.compare(0, sizeof("@CODE_RESULT@") - 1, "@CODE_RESULT@") == 0) {
        line.remove_prefix(sizeof("@CODE_RESULT@") - 1);
    }
    kimix::string buffer(line);
    yyjson_doc *doc = yyjson_read_opts(buffer.data(), buffer.size(),
                                       YYJSON_READ_STOP_WHEN_DONE,
                                       &kimix::llm::kYYJsonAlcMi, nullptr);
    if (doc == nullptr) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root == nullptr || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return false;
    }
    yyjson_val *ok = yyjson_obj_get(root, "ok");
    out.ok = yyjson_is_bool(ok) ? yyjson_get_bool(ok) : false;
    const auto field = [&root](const char *key) {
        yyjson_val *v = yyjson_obj_get(root, key);
        return (v != nullptr && yyjson_is_str(v))
                   ? kimix::string(yyjson_get_str(v),
                                   static_cast<size_t>(yyjson_get_len(v)))
                   : kimix::string();
    };
    out.error = field("error");
    out.traceback = field("traceback");
    out.output = field("output");
    out.stderr_text = field("stderr");
    yyjson_doc_free(doc);
    return true;
}

kimix::string CodeExecSession::make_request(kimix::string_view code,
                                            kimix::string_view script_path,
                                            const kimix::vector<kimix::string> &args,
                                            uint64_t request_id) {
    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    yyjson_mut_val *root = yyjson_mut_obj(mdoc);
    yyjson_mut_doc_set_root(mdoc, root);
    yyjson_mut_obj_add(root, yyjson_mut_strcpy(mdoc, "op"),
                       yyjson_mut_strcpy(mdoc, "exec"));
    yyjson_mut_obj_add(root, yyjson_mut_strcpy(mdoc, "code"),
                       yyjson_mut_strcpy(mdoc, kimix::string(code).c_str()));
    yyjson_mut_obj_add(root, yyjson_mut_strcpy(mdoc, "path"),
                       yyjson_mut_strcpy(mdoc, kimix::string(script_path).c_str()));
    yyjson_mut_val *argv = yyjson_mut_arr(mdoc);
    for (const kimix::string &arg : args) {
        yyjson_mut_arr_append(argv, yyjson_mut_strcpy(mdoc, arg.c_str()));
    }
    yyjson_mut_obj_add(root, yyjson_mut_strcpy(mdoc, "argv"), argv);
    yyjson_mut_obj_add(root, yyjson_mut_strcpy(mdoc, "id"),
                       yyjson_mut_uint(mdoc, request_id));
    size_t len = 0;
    char *json = yyjson_mut_write_opts(mdoc, 0, &kimix::llm::kYYJsonAlcMi, &len,
                                       nullptr);
    kimix::string out;
    if (json != nullptr) {
        out.assign(json, len);
        mi_free(json);
    }
    yyjson_mut_doc_free(mdoc);
    return out;
}

bool CodeExecSession::start(kimix::string_view work_dir, kimix::string &error) {
    // Serialize with exec_file: two concurrent starters must not both spawn
    // a child or interleave handshake reads on the task registry entry.
    std::lock_guard<std::mutex> lock(_mutex);
    if (alive()) {
        return true;
    }
    // Resolve the interpreter exactly like the python tool does.
    const kimix::string exe = Python::detect_python_exe(work_dir);
    if (exe.empty()) {
        error = "no python interpreter found";
        return false;
    }
    // Write the driver into the shared temp folder. base_dir is work_dir
    // (ANSI/lossy) or a kimix::to_string() product; the narrow path
    // constructor THROWS std::system_error on bytes the ANSI code page cannot
    // represent (fatal with C++ exceptions disabled), so build wide via the
    // non-throwing helpers.
    kimix::string base_dir;
    if (work_dir.empty()) {
        std::error_code base_ec;
        const kimix::filesystem::path cwd =
            kimix::filesystem::current_path(base_ec);
        if (!base_ec) {
            base_dir = kimix::to_string(cwd);
        }
    } else {
        base_dir = kimix::string(work_dir);
    }
    if (base_dir.empty()) {
        error = "no working directory for the python session";
        return false;
    }
    kimix::filesystem::path base;
    if (!kimix::path_from_narrow(base_dir, base)) {
        kimix::path_from_utf8(base_dir, base);
    }
    kimix::filesystem::path driver =
        base / ".kimix_cache" /
        (kimix::string("tmp_") + kimix::format("{}", process_id())) /
        "kimix_code_exec_ctx.py";
    std::error_code ec;
    kimix::filesystem::create_directories(driver.parent_path(), ec);
    {
        std::FILE *f = std::fopen(kimix::to_string(driver).c_str(), "wb");
        if (f == nullptr) {
            error = "cannot write driver: " + kimix::to_string(driver);
            return false;
        }
        const size_t n =
            std::fwrite(kDriverSource.data(), 1, kDriverSource.size(), f);
        std::fclose(f);
        if (n != kDriverSource.size()) {
            error = "short write: " + kimix::to_string(driver);
            return false;
        }
    }
    proc::run_options opts;
    opts.argv.push_back(exe);
    opts.argv.push_back("-u"); // unbuffered: the protocol depends on flushes
    opts.argv.push_back(kimix::to_string(driver));
    opts.working_directory = base_dir;
    opts.output_cap_chars = kOutputCapChars;
    proc::task_handle handle;
    const tool_error start_error = proc::start_task(opts, handle);
    if (start_error.failed()) {
        error = start_error.message;
        return false;
    }
    _task_id = kimix::string(handle.task_id);
    _work_dir = kimix::string(work_dir);
    // Handshake: the driver answers with a ready line.
    kimix::string diag;
    exec_result ready;
    if (!wait_response(*_task_id, kHandshakeTimeoutMs, diag, ready) ||
        !ready.ok) {
        error = "persistent python session failed to start";
        shutdown();
        return false;
    }
    return true;
}

bool CodeExecSession::ping(kimix::string &error) {
    if (!alive()) {
        error = "session is not running";
        return false;
    }
    kimix::string stale;
    drain(*_task_id, stale); // drop anything left over
    kimix::string request =
        "{\"op\":\"ping\",\"id\":" + kimix::format("{}", next_request_id()) + "}";
    if (proc::send_task(*_task_id, request, /*add_newline=*/true).failed()) {
        error = "cannot write to the persistent python session";
        return false;
    }
    kimix::string diag;
    exec_result pong;
    if (!wait_response(*_task_id, kHandshakeTimeoutMs, diag, pong) ||
        !pong.ok) {
        error = "persistent python session is not responding";
        return false;
    }
    return true;
}

void CodeExecSession::shutdown() {
    if (_task_id.has_value() && !_task_id->empty()) {
        proc::stop_task(*_task_id);
    }
    _task_id.reset();
}

bool CodeExecSession::alive_check(kimix::string &error) const {
    if (!alive()) {
        error = "session is not running";
        return false;
    }
    return true;
}

bool CodeExecSession::exec_file(kimix::string_view script_path,
                                const kimix::vector<kimix::string> &args,
                                exec_result &out, kimix::string &error) {
    out = exec_result{};
    // One critical section per logical request: the ping + send + wait
    // sequence below shares the child's single protocol stream with every
    // other caller of the process-global session, so concurrent exec_file
    // calls must not interleave (ping() is written to run under this lock).
    std::lock_guard<std::mutex> lock(_mutex);
    if (!alive_check(error)) {
        return false;
    }
    if (!ping(error)) {
        shutdown();
        return false;
    }
    // commands.py:693-694: `with open(script_path, ...) as f: s = f.read()`.
    // script_path is a caller-supplied path (UTF-8); no narrow path
    // constructor (see start()): an unrepresentable name just cannot be stat'ed.
    std::error_code ec;
    kimix::filesystem::path path;
    if (!kimix::path_from_utf8(script_path, path)) {
        if (!kimix::path_from_narrow(script_path, path)) {
            error = "cannot stat script: " + kimix::string(script_path);
            return false;
        }
    }
    const auto size = kimix::filesystem::file_size(path, ec);
    kimix::string source;
    if (ec) {
        error = "cannot stat script: " + kimix::string(script_path);
        return false;
    }
    std::FILE *f = std::fopen(kimix::to_string(path).c_str(), "rb");
    if (f == nullptr) {
        error = "cannot open script: " + kimix::string(script_path);
        return false;
    }
    source.resize(static_cast<size_t>(size));
    const size_t n = std::fread(source.data(), 1, source.size(), f);
    std::fclose(f);
    source.resize(n);
    // The reference decodes with errors="replace"; a script file is UTF-8
    // here, so the bytes are handed to the child verbatim.
    const kimix::string request =
        make_request(source, script_path, args, next_request_id());
    if (request.empty()) {
        error = "cannot build the exec request";
        return false;
    }
    if (proc::send_task(*_task_id, request, /*add_newline=*/true).failed()) {
        error = "cannot write to the persistent python session";
        shutdown();
        return false;
    }
    kimix::string diag;
    if (!wait_response(*_task_id, kExecTimeoutMs, diag, out)) {
        error = "the persistent python session did not answer";
        shutdown();
        return false;
    }
    return true; // out.ok says whether the script itself succeeded
}

CodeExecSession::~CodeExecSession() { shutdown(); }

CodeExecSession &code_exec_session() {
    static CodeExecSession session; // the process-global exec_ctx
    return session;
}

} // namespace kimix::builtin_tools::python
