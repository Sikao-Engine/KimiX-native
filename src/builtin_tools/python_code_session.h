// python_code_session.h - The persistent /code exec context (H8).
//
// Port of the reference's persistent exec_ctx: the /code CLI command keeps
// ONE long-lived interpreter process whose execution namespace survives
// across /code calls (src/kimix/cli_impl/commands.py:64 `exec_ctx: dict` and
// :700-701 `exec_ctx["__file__"] = str(script_path); exec(s, exec_ctx)`), so
// definitions, imports and module-level state from an earlier script are
// visible to the next one, and `sys.argv` is set per call
// (commands.py:695-697).
//
// DEVIATION (documented): the reference executes the script IN PROCESS
// (exec in the CLI's own interpreter). The native CLI has no embedded
// interpreter, so the analogue is a dedicated child `python` process speaking
// a newline-JSON protocol on stdin/stdout; the child keeps the execution
// namespace alive between requests, sets __file__/sys.argv per request and
// returns the captured output / error / traceback. Script output is captured
// by the child and printed after the run completes (the pre-H8 native
// behaviour was a captured one-shot spawn, so nothing is lost there; the
// reference streams because it shares one console).
//
// Graceful fallback: when the child cannot start (no interpreter, spawn
// failure, handshake timeout) exec_file() answers false and the caller falls
// back to the one-shot `python <script> args...` spawn - the reference's
// behaviour for a missing interpreter on its platforms.
//
// Rules (see .agents/skills/cpp): namespace kimix::builtin_tools::python,
// kimix:: containers, no exceptions, no RTTI.

#pragma once

#include <core/kimix_core.h>

#include "builtin_tools/tool.h"

namespace kimix::builtin_tools::python {

// One exec_file outcome (the reference's exec outcome + captured streams).
struct exec_result {
    bool ok = false;            // the script ran to completion
    kimix::string error;        // str(e) of the failing script ("" when ok)
    kimix::string traceback;    // the formatted traceback ("" when ok)
    kimix::string output;       // captured stdout of the script
    kimix::string stderr_text;  // captured stderr of the script
};

// The session: one long-lived child interpreter + its execution namespace.
// Not thread-safe: the /code command is the only caller (the CLI loop).
class CodeExecSession {
public:
    CodeExecSession() = default;
    ~CodeExecSession();
    CodeExecSession(const CodeExecSession &) = delete;
    CodeExecSession &operator=(const CodeExecSession &) = delete;

    // True while the child is alive (a started handshake not yet shut down).
    bool alive() const noexcept { return _task_id.has_value() && !_task_id->empty(); }

    // Start the child (idempotent: a live session is reused). `work_dir`
    // anchors the driver script's temp folder and the child's cwd. False +
    // `error` when the interpreter cannot be resolved or the handshake does
    // not complete.
    bool start(kimix::string_view work_dir, kimix::string &error);

    // Reset the session (the child is stopped; the next start is fresh).
    void shutdown();

    // Run one script in the persistent namespace: __file__ and sys.argv are
    // set to (script_path, args...), the code executes with the KEPT-ALIVE
    // namespace and stdout/stderr are captured into `out`. A script that
    // fails is reported in `out` (ok == false) and the session STAYS ALIVE -
    // exactly like the reference, where a failing exec() leaves exec_ctx
    // usable for the next /code call. False + `error` only when the session
    // itself is unusable (the caller falls back to a one-shot spawn).
    bool exec_file(kimix::string_view script_path,
                   const kimix::vector<kimix::string> &args, exec_result &out,
                   kimix::string &error);

    // The embedded driver source (exposed for tests): reads newline-JSON
    // requests from stdin and answers with "@CODE_RESULT@"-prefixed JSON
    // lines, keeping the execution namespace in a module-level dict.
    static kimix::string_view driver_source() noexcept { return kDriverSource; }

    // The request envelope for one exec (exposed for tests):
    // {"op":"exec","code":...,"path":...,"argv":[...],"id":<n>}.
    static kimix::string make_request(kimix::string_view code,
                                      kimix::string_view script_path,
                                      const kimix::vector<kimix::string> &args,
                                      uint64_t request_id);

    // Parse one "@CODE_RESULT@"-prefixed line into `out` (exposed for tests).
    static bool parse_response(kimix::string_view line, exec_result &out);

private:
    // Ask the child for a liveness pong (also re-syncs the output stream).
    bool ping(kimix::string &error);
    // alive() + the error message for exec_file.
    bool alive_check(kimix::string &error) const;

    kimix::optional<kimix::string> _task_id;
    kimix::string _work_dir;
    uint64_t _request_id = 0;

    static constexpr kimix::string_view kDriverSource = R"PY(# kimix /code persistent exec_ctx driver (H8).
# One long-lived interpreter for the CLI's /code command: each request line is
# a JSON object {"op":"exec","code":...,"path":...,"argv":[...]}; the code
# runs with exec() in a KEPT-ALIVE namespace (commands.py exec_ctx), with
# __file__ and sys.argv set per request like the reference. Responses are
# single "@CODE_RESULT@"-prefixed JSON lines on stdout.
import contextlib
import io
import json
import sys
import traceback

_CTX = {}
_OUT = sys.stdout


def _emit(obj):
    _OUT.write("@CODE_RESULT@" + json.dumps(obj) + "\n")
    _OUT.flush()


_emit({"ok": True, "ready": True})
while True:
    line = sys.stdin.readline()
    if not line:
        break
    line = line.strip()
    if not line:
        continue
    try:
        req = json.loads(line)
    except Exception as exc:
        _emit({"ok": False, "error": "invalid request: %s" % (exc,)})
        continue
    op = req.get("op", "")
    if op == "ping":
        _emit({"ok": True, "pong": True})
        continue
    if op != "exec":
        _emit({"ok": False, "error": "unknown op: %s" % (op,)})
        continue
    code = req.get("code", "")
    path = req.get("path", "")
    argv = req.get("argv", [])
    out_buf = io.StringIO()
    err_buf = io.StringIO()
    old_argv = sys.argv
    sys.argv = [path] + list(argv)
    _CTX["__file__"] = path
    try:
        with contextlib.redirect_stdout(out_buf), contextlib.redirect_stderr(err_buf):
            exec(compile(code, path, "exec"), _CTX)
        _emit({"ok": True, "output": out_buf.getvalue(),
               "stderr": err_buf.getvalue()})
    except KeyboardInterrupt:
        _emit({"ok": False, "error": "KeyboardInterrupt",
               "output": out_buf.getvalue()})
    except BaseException as exc:  # the script failed; the session stays alive
        _emit({"ok": False, "error": str(exc), "traceback": traceback.format_exc(),
               "output": out_buf.getvalue(), "stderr": err_buf.getvalue()})
    finally:
        sys.argv = old_argv
)PY";
};

// The process-wide /code session (commands.py's module-level exec_ctx is
// process-global in the reference too). Created on first use.
CodeExecSession &code_exec_session();

} // namespace kimix::builtin_tools::python
