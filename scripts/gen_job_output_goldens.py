#!/usr/bin/env python3
"""Regenerate tests/unit/builtin_tools/job_output_goldens.inc.

The golden vectors come from running the *real* Python ``job_output`` tool
(``kimix.tools.background.TaskOutput``) from the kimi-agent checkout against
scripted background streams, then recording -- for every case -- the tool's
result (``is_error`` / ``output`` / ``message`` / ``brief``), the id removals it
performed and the registry it left behind.

The C++ port (``src/builtin_tools/job_output_tool.cpp``) is replayed over the
same scripted registry by ``tests/unit/builtin_tools/test_job_output_tool.cpp``
and must reproduce every byte.  Cases run in order on ONE tool instance, so the
finished-task history (``record_finished_task`` / ``get_finished_task``) carries
over exactly as it does in a session.

What is stubbed, and why: the reference's ``_maybe_export_output_async`` /
``_maybe_export_rtk_original_async`` side channels need an LLM and the shared
cache dir (they summarize or re-export long output).  The C++ port represents
them with the injected ``original_path`` field, so the generator replaces both
with pass-through stubs; everything else (``_append_elapsed``, ``_elapsed_tag``,
``_elapsed_suffix``, ``_format_elapsed_seconds``, ``_coerce_seconds``,
``re.compile(...)`` pattern validation, the task registry and the finished-task
history) is the untouched reference implementation.

Usage (any interpreter that can import ``kimix.tools.background``)::

    python scripts/gen_job_output_goldens.py            # rewrite the .inc
    python scripts/gen_job_output_goldens.py --check    # fail when out of date
    python scripts/gen_job_output_goldens.py --dump     # print the goldens
"""

from __future__ import annotations

import argparse
import asyncio
import os
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
KIMI_AGENT_ROOT = Path(os.environ.get("KIMI_AGENT_ROOT", r"C:/dev/kimi-agent"))
OUT_PATH = (PROJECT_ROOT / "tests" / "unit" / "builtin_tools" /
            "job_output_goldens.inc")

#: Display path used by the output_path cases.  Relative to the process CWD so
#: the vector is machine independent; both implementations write the raw output
#: there (the C++ test removes it again).
EXPORT_PATH = ".kimix_cache/job_output_goldens_out.txt"


def _ensure_reference_on_path() -> None:
    for rel in ("kimi-cli/src", "src"):
        p = KIMI_AGENT_ROOT / rel
        if p.is_dir() and str(p) not in sys.path:
            sys.path.insert(0, str(p))


def _import_reference():
    _ensure_reference_on_path()
    from kimix.tools.background import TaskOutput  # noqa: PLC0415
    from kimix.tools.background import utils as bg_utils  # noqa: PLC0415

    return TaskOutput, bg_utils


# ---------------------------------------------------------------------------
# scripted stream
# ---------------------------------------------------------------------------
class ScriptedStream:
    """Duck-typed ``BackgroundStream``: every awaitable is scripted.

    The C++ ``TaskSource`` snapshot of one task is
    ``(task_id, exited, exit_code, elapsed, pending_output)``; this object
    exposes exactly that through the reference's stream interface.
    """

    def __init__(self, spec: dict) -> None:
        self._alive = not spec.get("exited", False)
        self._pending = spec.get("output", "")
        self._exit_code = spec.get("exit_code")
        self._elapsed = spec.get("elapsed")
        self._success = spec.get("success", self._exit_code == 0)
        self.format_output = None

    # -- lifecycle ---------------------------------------------------------
    async def is_started(self) -> bool:
        return True

    async def is_stopped(self) -> bool:
        return not self._alive

    async def thread_is_alive(self) -> bool:
        return self._alive

    async def stop(self) -> bool:
        was_alive = self._alive
        self._alive = False
        return was_alive

    # -- output ------------------------------------------------------------
    async def get_output(self) -> str:
        return self._pending

    async def pop_output(self) -> str:
        out, self._pending = self._pending, ""
        return out

    async def wait_for_output(self, *, timeout, pattern=None,
                              inactivity_timeout=None):
        out, self._pending = self._pending, ""
        self._wait_timeout = timeout
        return out, bool(self._wait_matched), 0.0

    async def wait_with_inactivity_timeout(self, timeout,
                                           inactivity_timeout=None):
        self._wait_timeout = timeout
        if self._wait_completed:
            self._alive = False
        return bool(self._wait_completed), 0.0, False

    async def wait(self, timeout=None) -> None:
        return None

    # -- results -----------------------------------------------------------
    async def success(self) -> bool:
        return self._success

    @property
    def exit_code(self):
        return self._exit_code

    @property
    def process_elapsed(self):
        return self._elapsed

    @property
    def output_truncated(self) -> bool:
        return False


# ---------------------------------------------------------------------------
# case corpus
# ---------------------------------------------------------------------------
def task(task_id, **kw):
    spec = {"task_id": task_id}
    spec.update(kw)
    return spec


CASES: list[dict] = [
    # ---- action='list' (and the job_id-omitted default) -------------------
    {"name": "list_empty", "params": {"action": "list"}, "tasks": []},
    {"name": "list_default_no_job_id", "params": {}, "tasks": []},
    {"name": "list_default_with_tasks", "params": {}, "tasks": [
        task("bash_1", elapsed=1.5, output="x"),
        task("run_git", exited=True, exit_code=0, elapsed=3.0, output="done"),
    ]},
    {"name": "list_action_with_tasks", "params": {"action": "list"}, "tasks": [
        task("alpha", elapsed=0.0),
        task("beta_2", exited=True, exit_code=1, elapsed=None),
        task("nounderscore", elapsed=12.25),
    ]},
    # Unknown task ids still list every started task.
    {"name": "list_after_unknown_get", "params": {"action": "list"}, "tasks": [
        task("keep_me", elapsed=2.0),
    ]},

    # ---- get: unknown id -------------------------------------------------
    {"name": "get_unknown_no_tasks", "params": {"job_id": "missing"},
     "tasks": []},
    {"name": "get_unknown_with_tasks", "params": {"job_id": "missing"},
     "tasks": [task("t1", elapsed=1.0), task("t2", exited=True, exit_code=0)]},
    {"name": "get_unknown_raw_id_in_message", "params": {"job_id": "  missing  "},
     "tasks": [task("t1", elapsed=1.0)]},
    {"name": "get_unknown_timeout_param", "params": {"job_id": "nope",
                                                    "timeout": 7200},
     "tasks": [task("t1", elapsed=1.0)]},

    # ---- get: running task ----------------------------------------------
    {"name": "get_running_partial", "params": {"job_id": "bash_1"},
     "tasks": [task("bash_1", elapsed=2.0, output="partial\n")]},
    {"name": "get_running_empty_output", "params": {"job_id": "bash_1"},
     "tasks": [task("bash_1", elapsed=2.0, output="")]},
    {"name": "get_running_wait_no_pattern",
     "params": {"job_id": "bash_1", "wait": True, "timeout": 5},
     "tasks": [task("bash_1", elapsed=1.0, output="abc")]},
    {"name": "get_running_wait_completes",
     "params": {"job_id": "bash_1", "wait": True, "timeout": 5},
     "wait_completed": True,
     "tasks": [task("bash_1", exited=True, exit_code=0, elapsed=5.5,
                    output="done")]},
    {"name": "get_running_wait_block_alias",
     "params": {"job_id": "bash_1", "wait": True, "timeout": 2},
     "tasks": [task("bash_1", elapsed=1.0, output="tick")]},
    {"name": "get_running_wait_pattern_matched",
     "params": {"job_id": "bash_1", "wait": True, "timeout": 30,
                "wait_for_pattern": "ready"},
     "wait_matched": True,
     "tasks": [task("bash_1", elapsed=1.0, output="ready\n")]},
    {"name": "get_running_wait_pattern_unmatched",
     "params": {"job_id": "bash_1", "wait": True, "timeout": 30,
                "wait_for_pattern": "ready"},
     "wait_matched": False,
     "tasks": [task("bash_1", elapsed=1.0, output="not yet\n")]},
    {"name": "get_pattern_without_wait",
     "params": {"job_id": "bash_1", "wait_for_pattern": "ready"},
     "tasks": [task("bash_1", elapsed=1.0, output="ready\n")]},

    # ---- get: finished task (success) -----------------------------------
    {"name": "get_finished_success", "params": {"job_id": "bash_1"},
     "tasks": [task("bash_1", exited=True, exit_code=0, elapsed=3.5,
                    output="hello\n")]},
    {"name": "get_finished_success_subsecond", "params": {"job_id": "t_sub"},
     "tasks": [task("t_sub", exited=True, exit_code=0, elapsed=0.4,
                    output="quick\n")]},
    {"name": "get_finished_success_unknown_elapsed",
     "params": {"job_id": "t_noelapsed"},
     "tasks": [task("t_noelapsed", exited=True, exit_code=0, elapsed=None,
                    output="x")]},
    {"name": "get_finished_success_minute", "params": {"job_id": "bash_1"},
     "tasks": [task("bash_1", exited=True, exit_code=0, elapsed=95.0,
                    output="slow\n")]},
    {"name": "get_finished_success_hour", "params": {"job_id": "bash_1"},
     "tasks": [task("bash_1", exited=True, exit_code=0, elapsed=3725.0,
                    output="slower\n")]},
    {"name": "get_finished_no_output", "params": {"job_id": "bash_1"},
     "tasks": [task("bash_1", exited=True, exit_code=0, elapsed=2.0,
                    output="")]},
    {"name": "get_finished_elapsed_exactly_one", "params": {"job_id": "bash_1"},
     "tasks": [task("bash_1", exited=True, exit_code=0, elapsed=1.0,
                    output="unity\n")]},
    {"name": "get_finished_wait_matched_recorded",
     "params": {"job_id": "bash_1", "wait": True, "timeout": 10,
                "wait_for_pattern": "err"},
     "wait_matched": True,
     "tasks": [task("bash_1", exited=True, exit_code=0, elapsed=2.5,
                    output="err ok\n")]},
    {"name": "get_finished_with_output_path",
     "params": {"job_id": "bash_1", "output_path": EXPORT_PATH},
     "tasks": [task("bash_1", exited=True, exit_code=0, elapsed=1.5,
                    output="to file\n")]},
    {"name": "get_running_with_output_path",
     "params": {"job_id": "bash_1", "output_path": EXPORT_PATH},
     "tasks": [task("bash_1", elapsed=1.5, output="still going\n")]},

    # ---- get: finished task (failure) -----------------------------------
    {"name": "get_finished_failure", "params": {"job_id": "bash_1"},
     "tasks": [task("bash_1", exited=True, exit_code=1, elapsed=1.5,
                    output="boom\n")]},
    {"name": "get_finished_failure_empty_output", "params": {"job_id": "bash_1"},
     "tasks": [task("bash_1", exited=True, exit_code=1, elapsed=1.5,
                    output="")]},
    {"name": "get_finished_failure_subsecond", "params": {"job_id": "bash_1"},
     "tasks": [task("bash_1", exited=True, exit_code=7, elapsed=0.25,
                    output="nope")]},
    {"name": "get_finished_failure_with_output_path",
     "params": {"job_id": "bash_1", "output_path": EXPORT_PATH},
     "tasks": [task("bash_1", exited=True, exit_code=2, elapsed=1.5,
                    output="bad\n")]},
    {"name": "get_finished_failure_unknown_elapsed",
     "params": {"job_id": "bash_1"},
     "tasks": [task("bash_1", exited=True, exit_code=1, elapsed=None,
                    output="x")]},

    # ---- action='kill' ---------------------------------------------------
    {"name": "kill_running_success", "params": {"job_id": "bash_1",
                                                "action": "kill"},
     "tasks": [task("bash_1", elapsed=12.5, output="killed out\n",
                    exit_code=0)]},
    {"name": "kill_running_success_no_output",
     "params": {"job_id": "bash_1", "action": "kill"},
     "tasks": [task("bash_1", elapsed=12.5, output="", exit_code=0)]},
    {"name": "kill_running_success_subsecond",
     "params": {"job_id": "bash_1", "action": "kill"},
     "tasks": [task("bash_1", elapsed=0.5, output="bye", exit_code=0)]},
    {"name": "kill_running_failure", "params": {"job_id": "bash_1",
                                                "action": "kill"},
     "tasks": [task("bash_1", elapsed=2.0, output="only this\n",
                    success=False, exit_code=1)]},
    {"name": "kill_running_failure_no_output",
     "params": {"job_id": "bash_1", "action": "kill"},
     "tasks": [task("bash_1", elapsed=2.0, output="", success=False,
                    exit_code=1)]},
    {"name": "kill_running_deprecated_flag",
     "params": {"job_id": "bash_1", "kill": True},
     "tasks": [task("bash_1", elapsed=3.0, output="flag\n", exit_code=0)]},
    {"name": "kill_raw_id_in_brief", "params": {"job_id": "  bash_1  ",
                                                "action": "kill"},
     "tasks": [task("bash_1", elapsed=3.0, output="trimmed\n",
                    exit_code=0)]},
    {"name": "kill_unknown_no_tasks", "params": {"job_id": "zzz",
                                                 "action": "kill"},
     "tasks": []},
    {"name": "kill_unknown_with_tasks", "params": {"job_id": "zzz",
                                                   "action": "kill"},
     "tasks": [task("t1", elapsed=1.0), task("t2", exited=True, exit_code=0)]},
    {"name": "kill_missing_job_id", "params": {"action": "kill"},
     "tasks": [task("t1", elapsed=1.0)]},
    {"name": "kill_elapsed_in_message", "params": {"job_id": "bash_1",
                                                   "action": "kill"},
     "tasks": [task("bash_1", elapsed=65.0, output="long\n", exit_code=0)]},

    # ---- finished-task history (second read of a completed job) ---------
    {"name": "history_first_read", "params": {"job_id": "bash_1"},
     "tasks": [task("bash_1", exited=True, exit_code=0, elapsed=2.0,
                    output="saved\n")]},
    {"name": "history_second_read", "params": {"job_id": "bash_1"},
     "tasks": []},
    {"name": "history_second_read_with_pattern",
     "params": {"job_id": "bash_1", "wait_for_pattern": "saved"},
     "tasks": []},
    {"name": "history_second_read_pattern_unmatched",
     "params": {"job_id": "bash_1", "wait_for_pattern": "absent"},
     "tasks": []},
    {"name": "history_second_read_subsecond_elapsed",
     "params": {"job_id": "t_fast"},
     "tasks": [task("t_fast", exited=True, exit_code=0, elapsed=0.5,
                    output="fast\n")]},
    {"name": "history_second_read_subsecond", "params": {"job_id": "t_fast"},
     "tasks": []},
    {"name": "history_second_read_with_output_path",
     "params": {"job_id": "bash_1", "output_path": EXPORT_PATH},
     "tasks": []},
    {"name": "history_second_read_unknown_id", "params": {"job_id": "never_ran"},
     "tasks": []},
    {"name": "history_failure_first_read", "params": {"job_id": "fail_job"},
     "tasks": [task("fail_job", exited=True, exit_code=3, elapsed=2.0,
                    output="bad\n")]},
    {"name": "history_failure_second_read", "params": {"job_id": "fail_job"},
     "tasks": []},
    {"name": "history_failure_second_read_pattern",
     "params": {"job_id": "fail_job", "wait_for_pattern": "bad"},
     "tasks": []},
    {"name": "history_kill_first", "params": {"job_id": "kill_me",
                                              "action": "kill"},
     "tasks": [task("kill_me", elapsed=4.0, output="kl\n", exit_code=0)]},
    {"name": "history_kill_second_read", "params": {"job_id": "kill_me"},
     "tasks": []},
    {"name": "history_kill_second_kill", "params": {"job_id": "kill_me",
                                                    "action": "kill"},
     "tasks": []},
    {"name": "history_kill_failed_first", "params": {"job_id": "kill_bad",
                                                     "action": "kill"},
     "tasks": [task("kill_bad", elapsed=4.0, output="", success=False,
                    exit_code=1)]},
    {"name": "history_kill_failed_read", "params": {"job_id": "kill_bad"},
     "tasks": []},
    {"name": "history_shadowed_by_registry", "params": {"job_id": "bash_1"},
     "tasks": [task("bash_1", elapsed=1.0, output="live again\n")]},
    {"name": "history_shadowed_by_registry_read", "params": {"job_id": "bash_1"},
     "tasks": []},
    {"name": "history_after_replacement", "params": {"job_id": "reuse_1"},
     "tasks": [task("reuse_1", exited=True, exit_code=0, elapsed=6.0,
                    output="first\n")]},
    {"name": "history_after_replacement_read", "params": {"job_id": "reuse_1"},
     "tasks": []},
    {"name": "list_still_empty_after_history", "params": {"action": "list"},
     "tasks": []},
]


# ---------------------------------------------------------------------------
# runner
# ---------------------------------------------------------------------------
async def _run_cases(TaskOutput, bg_utils) -> list[dict]:
    from unittest.mock import MagicMock

    session = MagicMock()
    session.custom_data = {}
    tool = TaskOutput(session=session)

    results: list[dict] = []
    for case in CASES:
        params_kwargs = dict(case["params"])
        streams = {}
        for spec in case["tasks"]:
            stream = ScriptedStream(spec)
            stream._wait_matched = case.get("wait_matched", False)
            stream._wait_completed = case.get("wait_completed", False)
            stream._wait_timeout = None
            streams[spec["task_id"]] = stream
        bg_utils._get_or_add_task_data(session).tasks.clear()
        bg_utils._get_or_add_task_data(session).tasks.update(streams)

        from kimix.tools.background import TaskOutputParams  # noqa: PLC0415
        params = TaskOutputParams(**params_kwargs)
        result = await tool(params)

        waited = [s for s in streams.values()
                  if getattr(s, "_wait_timeout", None) is not None]
        results.append({
            "name": case["name"],
            "params": params_kwargs,
            "tasks": case["tasks"],
            "wait_matched": case.get("wait_matched", False),
            "is_error": result.is_error,
            "output": result.output,
            "message": result.message,
            "brief": result.brief,
            "registry_after": list(
                bg_utils._get_or_add_task_data(session).tasks.keys()),
            "removed": [tid for tid in streams
                        if tid not in bg_utils._get_or_add_task_data(session).tasks],
            "wait_timeout_ms": (int(waited[0]._wait_timeout * 1000)
                                if waited else 0),
        })
    return results


def _patch_side_channels(bg_module) -> None:
    async def _identity(output):
        return output

    async def _no_original(output):
        return None, None

    bg_module._maybe_export_output_async = _identity
    bg_module._maybe_export_rtk_original_async = _no_original


# ---------------------------------------------------------------------------
# C emission
# ---------------------------------------------------------------------------
def _cstr(value: str | None) -> str:
    """Byte-exact C string literal (UTF-8, escapes every non-printable byte)."""
    if value is None:
        return "nullptr"
    out = ['"']
    for byte in value.encode("utf-8"):
        ch = chr(byte)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\r":
            out.append("\\r")
        elif 0x20 <= byte < 0x7F:
            out.append(ch)
        else:
            out.append(f"\\x{byte:02x}")
    out.append('"')
    return "".join(out)


def _optional_int(value) -> str:
    return "nullptr" if value is None else _cstr(str(value))


def _optional_bool(value) -> str:
    if value is None:
        return "false, false"
    return f"true, {'true' if value else 'false'}"


def _float(value) -> str:
    if value is None:
        return "0.0"
    return repr(float(value))


def emit(results: list[dict]) -> str:
    lines: list[str] = []
    lines.append("// GENERATED by scripts/gen_job_output_goldens.py from the")
    lines.append("// kimi-agent Python reference (C:/dev/kimi-agent). Do not edit")
    lines.append("// by hand - regenerate with")
    lines.append("//   python scripts/gen_job_output_goldens.py")
    lines.append("//   python scripts/gen_job_output_goldens.py --check")
    lines.append(f"// Python {sys.version.split()[0]}")
    lines.append("//")
    lines.append("// Every case is one call of TaskOutput(session)(params) against a")
    lines.append("// scripted registry (see the generator for the exact stream stubs).")
    lines.append("// Cases run in order on ONE tool instance, so the finished-task")
    lines.append("// history carries over between them.")
    lines.append("")
    lines.append("struct jo_golden_task {")
    lines.append("    const char *task_id;")
    lines.append("    bool exited;")
    lines.append("    bool has_exit_code;")
    lines.append("    int64_t exit_code;")
    lines.append("    bool has_elapsed;")
    lines.append("    double elapsed;")
    lines.append("    const char *output;")
    lines.append("};")
    lines.append("")
    lines.append("struct jo_golden_case {")
    lines.append("    const char *name;")
    lines.append("    // parameters (nullptr == absent)")
    lines.append("    const char *job_id;")
    lines.append("    const char *action;")
    lines.append("    bool has_wait;")
    lines.append("    bool wait;")
    lines.append("    bool has_timeout;")
    lines.append("    int64_t timeout;")
    lines.append("    const char *output_path;")
    lines.append("    const char *wait_for_pattern;")
    lines.append("    bool kill_flag;")
    lines.append("    // scripted source (the registry before the call)")
    lines.append("    const jo_golden_task *tasks;")
    lines.append("    int task_count;")
    lines.append("    bool wait_matched;")
    lines.append("    // expected result")
    lines.append("    bool is_error;")
    lines.append("    const char *output;")
    lines.append("    const char *message;")
    lines.append("    const char *brief;")
    lines.append("    // space separated ids: removed by the call / left behind")
    lines.append("    const char *removed;")
    lines.append("    const char *registry_after;")
    lines.append("    // ms the tool handed to the blocking wait (0 == no wait call)")
    lines.append("    int64_t wait_timeout_ms;")
    lines.append("};")
    lines.append("")

    for index, case in enumerate(results):
        name = case["name"]
        lines.append(f"// {name}")
        if not case["tasks"]:
            # A zero-length array is ill-formed C++: emit one unused placeholder
            # and a null pointer with task_count 0.
            lines.append(f"const jo_golden_task kJoTasks_{index}[] = {{")
            lines.append("    {nullptr, false, false, 0, false, 0.0, nullptr},")
            lines.append("};")
            lines.append(
                f"const jo_golden_task *const kJoTasksPtr_{index} = nullptr;")
        else:
            lines.append(f"const jo_golden_task kJoTasks_{index}[] = {{")
            for spec in case["tasks"]:
                exit_code = spec.get("exit_code")
                elapsed = spec.get("elapsed")
                lines.append(
                    "    {{{}, {}, {}, {}, {}, {}, {}}},".format(
                        _cstr(spec["task_id"]),
                        "true" if spec.get("exited") else "false",
                        "true" if exit_code is not None else "false",
                        "0" if exit_code is None else str(int(exit_code)),
                        "true" if elapsed is not None else "false",
                        _float(elapsed),
                        _cstr(spec.get("output", ""))))
            lines.append("};")
            lines.append(
                f"const jo_golden_task *const kJoTasksPtr_{index} = kJoTasks_{index};")
        params = case["params"]
        has_wait = "wait" in params
        has_timeout = "timeout" in params
        lines.append("const jo_golden_case kJoCase_{} = {{".format(index))
        lines.append("    {},".format(_cstr(name)))
        lines.append("    {},".format(_cstr(params.get("job_id"))))
        lines.append("    {},".format(_cstr(params.get("action"))))
        lines.append("    {}, {},".format(
            "true" if has_wait else "false",
            "true" if params.get("wait") else "false"))
        lines.append("    {}, {},".format(
            "true" if has_timeout else "false",
            str(int(params.get("timeout", 0)))))
        lines.append("    {},".format(_cstr(params.get("output_path"))))
        lines.append("    {},".format(_cstr(params.get("wait_for_pattern"))))
        lines.append("    {},".format(
            "true" if params.get("kill") else "false"))
        lines.append("    kJoTasksPtr_{}, {},".format(index, len(case["tasks"])))
        lines.append("    {},".format(
            "true" if case["wait_matched"] else "false"))
        lines.append("    {},".format("true" if case["is_error"] else "false"))
        lines.append("    {},".format(_cstr(case["output"])))
        lines.append("    {},".format(_cstr(case["message"])))
        lines.append("    {},".format(_cstr(case["brief"])))
        lines.append("    {},".format(_cstr(" ".join(case["removed"]))))
        lines.append("    {},".format(_cstr(" ".join(case["registry_after"]))))
        lines.append("    {},".format(case["wait_timeout_ms"]))
        lines.append("};")
        lines.append("")

    lines.append("const jo_golden_case *const kJoGoldens[] = {")
    for index in range(len(results)):
        lines.append(f"    &kJoCase_{index},")
    lines.append("};")
    lines.append("const int kJoGoldenCount = "
                 f"{len(results)};")
    lines.append("")
    lines.append("// Not covered (known divergences of the C++ port, see")
    lines.append("// src/builtin_tools/reports/job_output.md):")
    for note in KNOWN_GAP_NOTES:
        lines.append(f"// {note}" if note.startswith(" ") else f"// - {note}")
    lines.append("")
    return "\n".join(lines)


#: Divergences the golden corpus deliberately does NOT encode, because the C++
#: port cannot reproduce them (documented in the tool report and in
#: python/tests/test_parity_job_output.py where reachable).
KNOWN_GAP_NOTES = [
    "Invalid wait_for_pattern message: the reference reports the CPython",
    "  `regex` module's error text (\"missing ), unterminated subpattern at",
    "  position 0\" for '('), the port reports its own scanner's text",
    "  (\"missing closing ')'\"); the two engines' phrasings cannot be mapped.",
    "Regex wait_for_pattern: the reference matches with the full `regex`",
    "  module, the native wait path (proc::wait_task) matches the pattern",
    "  LITERALLY, so a real regex only ever reports wait_matched=false.",
    "success(): the reference reads the worker's own success flag, the port",
    "  derives it from exit_code == 0 (identical for every process-backed",
    "  task, different for a hypothetical success-with-no-exit-code stream).",
    "rtk/summarize side channels (_maybe_export_output_async /",
    "  _maybe_export_rtk_original_async / the \"[rtk output exported to: ...]\"",
    "  and \"[original output exported to: ...]\" suffixes) are not implemented",
    "  natively; the generator stubs them out for every case.",
]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true",
                        help="fail when the .inc is out of date")
    parser.add_argument("--dump", action="store_true",
                        help="print the goldens instead of writing them")
    parser.add_argument("--python", default=None,
                        help="interpreter that can import kimix.tools.background")
    args = parser.parse_args()

    if args.python and Path(args.python).resolve() != Path(sys.executable).resolve():
        import subprocess
        return subprocess.call([args.python, str(Path(__file__).resolve()),
                                *(["--check"] if args.check else [])])

    TaskOutput, bg_utils = _import_reference()
    import kimix.tools.background as bg_module
    _patch_side_channels(bg_module)

    if args.dump:
        for case in asyncio.run(_run_cases(TaskOutput, bg_utils)):
            print(f"--- {case['name']}: error={case['is_error']} "
                  f"brief={case['brief']!r}")
            print(f"    output={case['output']!r}")
            print(f"    message={case['message']!r} removed={case['removed']} "
                  f"after={case['registry_after']}")
        return 0

    results = asyncio.run(_run_cases(TaskOutput, bg_utils))
    text = emit(results)
    if args.check:
        current = OUT_PATH.read_text(encoding="utf-8") if OUT_PATH.is_file() else ""
        if current != text:
            print(f"STALE: {OUT_PATH} does not match the reference")
            return 1
        print(f"OK: {OUT_PATH} is up to date ({len(results)} cases)")
        return 0

    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUT_PATH.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUT_PATH} ({len(results)} cases, {len(text)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
