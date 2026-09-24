#!/usr/bin/env python3
"""Regenerate tests/unit/builtin_tools/agent_goldens.inc.

The golden vectors are produced by running the *real* Python implementation of
the sub-agent tools from the kimi-agent checkout
(``kimix/tools/agent/__init__.py`` + ``store.py``) against a stub runtime /
session and recording, after every step:

  * the tool result (``is_error`` / ``output`` / ``message`` / ``brief`` /
    ``extras``) exactly as the Python tool returns it,
  * the persisted store state (``AgentSessionStore`` insertion order, per-entry
    state / total_turns / pending question), the module-level registries
    (``_agent_sessions`` liveness, ``_pending_messages`` counts), the prompts
    actually handed to the sub-agent runner and the prompts saved to the shared
    temp folder.

The C++ port (``src/builtin_tools/agent_tool.cpp``) is replayed over the same
arguments by ``tests/unit/builtin_tools/test_agent_tool.cpp`` and must
reproduce every byte.

Provenance rules (same as scripts/gen_todo_goldens.py):

* The reference is imported straight from the kimi-agent checkout, never from
  ``python/kimix_native`` (the _compat mirrors written alongside the port).
* ``kimix.tools.agent`` is driven through its real body: only the *host*
  boundary is stubbed - ``_create_session_async`` (the SDK session factory),
  ``utils.prompt_async`` (the model loop), ``close_session_async``,
  ``_create_script_file`` / ``_display_temp_path`` (the shared temp folder),
  ``kimi_cli.soul.steer.Steer`` (the running-soul handle) and ``time.time``
  (the clock).  Everything the four tools do themselves (store bookkeeping,
  prompt assembly, message formatting, refusal texts, list_agents JSON) is the
  code under test.
* ``run_in_background`` is always False in the corpus: kimi-agent's ``Agent``
  ignores the flag and always awaits the run, while the C++ port really runs in
  the background; only the foreground path is therefore comparable step by
  step.  The background branch is covered by hand-written C++ assertions.

Usage::

    python scripts/gen_agent_goldens.py            # rewrite the .inc file
    python scripts/gen_agent_goldens.py --check    # fail if it is out of date

``--python`` lets the caller point at the kimi-agent virtualenv interpreter
(``C:/dev/kimi-agent/.venv/Scripts/python.exe``) when the current interpreter
cannot import ``kimix.tools.agent``.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
KIMI_AGENT_ROOT = Path(os.environ.get("KIMI_AGENT_ROOT", r"C:/dev/kimi-agent"))
DEFAULT_AGENT_PYTHON = KIMI_AGENT_ROOT / ".venv" / "Scripts" / "python.exe"
OUT_PATH = PROJECT_ROOT / "tests" / "unit" / "builtin_tools" / "agent_goldens.inc"

#: Prompt/context files the corpus reads.  The C++ replay injects them through
#: the registry's `read_file` hook; the generator materialises them on disk
#: (relative to the process CWD, which is the reference's CWD fallback) so the
#: Python tool reads the very same bytes from the real file system.  MISSING_FILE
#: is a bare name so that the platform's path separator cannot leak into the
#: recorded error text.
PROMPT_FILE = ".kimix_cache/agent_golden_prompt.md"
PROMPT_FILE_TEXT = "Task from a file: do the thing.\nSecond line.\n"
CONTEXT_FILE = ".kimix_cache/agent_golden_context.txt"
CONTEXT_FILE_TEXT = "context body line 1\ncontext body line 2\n"
MISSING_FILE = "agent_golden_missing.txt"

FILES = [
    [PROMPT_FILE, PROMPT_FILE_TEXT],
    [CONTEXT_FILE, CONTEXT_FILE_TEXT],
]


def _ensure_reference_on_path() -> None:
    for rel in ("kimi-cli/src", "bin", "src"):
        p = KIMI_AGENT_ROOT / rel
        if p.is_dir():
            s = str(p)
            while s in sys.path:
                sys.path.remove(s)
            sys.path.insert(0, s)


def _import_reference():
    _ensure_reference_on_path()
    import kimix.tools.agent as agent_mod  # noqa: PLC0415
    from kimix.base import MessageType  # noqa: PLC0415
    import kimi_cli.soul.steer as steer_mod  # noqa: PLC0415
    from kosong.tooling.error import ToolValidateError  # noqa: PLC0415
    from kimix.tools.agent.store import AgentSessionEntry  # noqa: PLC0415

    # Provenance guard: the reference must be the kimi-agent checkout, never a
    # kimix-base mirror (python/kimix_native) that shares a mistake with the
    # port.
    resolved = Path(agent_mod.__file__).resolve()
    if KIMI_AGENT_ROOT.resolve() not in resolved.parents:
        raise RuntimeError(
            "kimix.tools.agent resolved to %s, outside the reference checkout %s"
            % (resolved, KIMI_AGENT_ROOT))

    return {
        "agent": agent_mod,
        "MessageType": MessageType,
        "steer": steer_mod,
        "ToolValidateError": ToolValidateError,
        "AgentSessionEntry": AgentSessionEntry,
    }


# ---------------------------------------------------------------------------
# Stub host boundary
# ---------------------------------------------------------------------------


class Clock:
    """``time.time()`` stand-in: constant inside a step, bumped per step."""

    def __init__(self) -> None:
        self.value = 1000.0

    def time(self) -> float:
        return self.value


class StubSession:
    """Minimal stand-in for ``kimi_cli.session.Session`` / the SDK wrapper."""

    def __init__(self, session_id: str, custom_config: dict | None = None) -> None:
        self.id = session_id
        self.custom_data: dict = {}
        self.custom_config = dict(custom_config or {})
        # work_dir is None -> _session_work_dir() -> None -> Path(".") base dir,
        # exactly like the C++ port's `Session::work_dir == ""` -> "." base dir.
        self.work_dir = None

    def get_custom_config(self) -> dict:
        return self.custom_config

    async def close(self) -> None:
        return None


class FakeSteer:
    """Stand-in for ``kimi_cli.soul.steer.Steer`` (records the pushed text)."""

    def __init__(self, delivered: bool, sink: list) -> None:
        self.delivered = delivered
        self.sink = sink

    async def push(self, content) -> bool:
        self.sink.append(content)
        return self.delivered


class Harness:
    """Owns the stub runtime for one case (one parent session + its store)."""

    def __init__(self, ref, caller_id: str, is_sub_agent: bool,
                 parent_session_id: str, files: list) -> None:
        self.ref = ref
        self.agent = ref["agent"]
        # The module-level registries are process-wide in kimi-agent: start
        # every case from an empty world.
        for name in ("_pending_messages", "_agent_sessions", "_agent_entries",
                     "_children_by_parent", "_child_parent"):
            getattr(self.agent, name).clear()
        self.caller_id = caller_id
        self.clock = Clock()
        self.created: list[dict] = []
        self.prompts: list[str] = []
        self.saved: list[str] = []
        self.pushed: list = []
        self.closed: list[str] = []
        self.steer_mode = "idle"          # none | idle | delivered
        self.steer_targets: set[str] = set()
        self.step_events: list = []
        self.step_error: str | None = None
        self.step_ask: str | None = None
        self.step_session_id: str = ""
        self.runner_turns: list[dict] = []
        self._file_table = list(files)

        self.parent = StubSession(caller_id, {
            "is_sub_agent": is_sub_agent,
            "parent_session_id": parent_session_id,
        })
        self.store = self.agent._get_store(self.parent)

        # ---- host boundary patches -----------------------------------------
        self.agent.time = self.clock
        self.agent.close_session_async = self._close_session_async
        self.agent._create_session_async = self._create_session_async
        self.agent._create_script_file = self._create_script_file
        self.agent._display_temp_path = self._display_temp_path
        self.agent.utils = _UtilsShim(self._prompt_async)
        self._patch_steer()

    # -- host hooks ---------------------------------------------------------
    async def _close_session_async(self, session) -> None:
        self.closed.append(getattr(session, "id", ""))

    async def _create_session_async(self, **kwargs):
        self.created.append(kwargs)
        session = StubSession(kwargs["session_id"])
        return session

    def _create_script_file(self, text: str, ext: str = ".md") -> str:
        name = f".kimix_cache/prompt_{len(self.saved)}{ext}"
        self.saved.append(text)
        return name

    def _display_temp_path(self, path) -> str:
        return str(path)

    def _patch_steer(self) -> None:
        harness = self

        class _StubSteer:
            @classmethod
            def from_session(cls, session):  # noqa: ANN001
                sid = getattr(session, "id", None)
                if harness.steer_mode == "none":
                    return None
                if harness.steer_targets and sid not in harness.steer_targets:
                    return None
                return FakeSteer(harness.steer_mode == "delivered",
                                 harness.pushed)

        self.ref["steer"].Steer = _StubSteer

    async def _prompt_async(self, *, prompt_str, session, output_function,
                            **kwargs) -> None:
        self.prompts.append(prompt_str)
        sid = getattr(session, "id", "")
        self.step_session_id = sid
        collector = self.agent._AgentConversationCollector()
        collector.finalize_user_turn(prompt_str)
        for kind, text in self.step_events:
            if text:
                output_function(text, kind)
                collector.consume(text, kind)
        if self.step_ask is not None:
            entry = self.store.get(sid)
            if entry is not None:
                entry.pending_question = self.step_ask
                entry.state = "awaiting_response"
        if self.step_error is not None:
            err = RuntimeError(self.step_error)
            collector.turns.append(self.agent.ConversationTurn(
                role="error",
                content=str(err),
                timestamp=self.clock.time(),
                metadata={"error_type": type(err).__name__},
            ))
            # Agent.__call__ appends the error turn and *then* flushes the
            # collector (finalize_assistant_turn), so a still-buffered text turn
            # lands after it.
            collector.finalize_assistant_turn()
            self._capture_turns(collector)
            raise err
        # Agent.__call__ flushes the collector (finalize_assistant_turn) before
        # it reads the turn list, so the flushed view is what the C++ runner has
        # to report back.
        collector.finalize_assistant_turn()
        self._capture_turns(collector)

    def _capture_turns(self, collector) -> None:
        """Record the host-side view of a finished turn for the golden runner."""
        turns = collector.turns
        self.runner_turns = [
            {
                "role": t.role,
                "content": t.content if isinstance(t.content, str)
                else json.dumps(t.content),
                "timestamp": t.timestamp,
                "type": (t.metadata or {}).get("type", ""),
            }
            for t in turns
        ]
        # Agent.__call__ substitutes "(no text output)" itself; the runner
        # therefore reports the raw joined assistant text.
        self.runner_output = "".join(
            t.content for t in turns
            if t.role == "assistant" and (t.metadata or {}).get("type") == "text")

    def runner_result(self) -> dict:
        """The host-boundary result for this step.

        kimi-agent's ``Agent`` owns the turn collector (``_AgentConversation
        Collector``) and the C++ port's runner reports the same list back, so the
        payload carries the flushed conversation - including the error turn a
        failed prompt appends - and the joined assistant text.  ``output == ""``
        makes the port substitute "(no text output)" exactly like the reference.
        """
        error = self.step_error
        return {
            "ok": error is None,
            "output": self.runner_output,
            "error": "" if error is None else error,
            "pending_question": self.step_ask,
            "turns": list(self.runner_turns),
        }

    # -- setup helpers ------------------------------------------------------
    def put_entry(self, session_id: str, created_at: float, last_accessed: float,
                  total_turns: int, state: str, is_active: bool = True,
                  pending_question: str | None = None) -> None:
        self.store.put(self.ref["AgentSessionEntry"](
            session=StubSession(session_id),
            session_id=session_id,
            created_at=created_at,
            last_accessed=last_accessed,
            conversation_history=[],
            total_turns=total_turns,
            is_active=is_active,
            pending_question=pending_question,
            state=state,
        ))

    def register_live(self, session_id: str) -> None:
        self.agent._register_agent_session(session_id, StubSession(session_id))

    def queue_pending(self, session_id: str, message: str) -> None:
        self.agent._queue_pending_message(session_id, message)

    # -- tool invocation ----------------------------------------------------
    async def call(self, tool: str, args: dict):
        cls = {
            "subagent": self.agent.Agent,
            "send_message": self.agent.AskAgent,
            "list_agents": self.agent.AgentList,
            "interrupt_agent": self.agent.AgentClose,
        }[tool]
        impl = cls(self.parent)
        return await impl.call(args)


class _UtilsShim:
    def __init__(self, prompt_async) -> None:
        self.prompt_async = prompt_async


_MSG_TYPE = None


def _msg_types(MessageType) -> dict:
    return {
        "text": MessageType.Text,
        "thinking": MessageType.Thinking,
        "tool_call": MessageType.ToolCalling,
        "tool_result": MessageType.ToolResult,
    }


# ---------------------------------------------------------------------------
# Corpus
# ---------------------------------------------------------------------------
# A step is a dict:
#   {"tool": ..., "args": {...},
#    "run": {"events": [(kind, text), ...], "error": str|None, "ask": str|None}}
# and, for standalone single-step cases, the setup keys
#   pre_live / pre_entries / pre_pending / pre_steer / pre_steer_target.


def _reader_events() -> list:
    """A run that exercises every collector branch (text/think/tool)."""
    return [
        ("text", "working on it"),
        ("tool_call", "Read(file.txt)"),
        ("tool_result", "file body"),
        ("thinking", "let me think"),
        ("text", " done"),
    ]


def corpus() -> list[dict]:
    C: list[dict] = []
    add = C.append

    # ---- the full state machine: spawn -> list -> send -> resume -> stop ----
    add({
        "name": "spawn_list_send_resume_interrupt",
        "caller_id": "main-1",
        "steps": [
            {"tool": "subagent",
             "args": {"description": "kid a", "prompt": "Do the thing",
                      "session_id": "kid-a", "close_session": False,
                      "run_in_background": False},
             "run": {"events": _reader_events()}},
            {"tool": "list_agents", "args": {}},
            {"tool": "send_message",
             "args": {"message": "how is it going?", "subagent_id": "kid-a"},
             "steer": "idle"},
            {"tool": "list_agents", "args": {}},
            {"tool": "list_agents", "args": {"scope": "descendants"}},
            {"tool": "subagent",
             "args": {"prompt": "Keep going", "session_id": "kid-a",
                      "close_session": False, "run_in_background": False},
             "run": {"events": [("text", "second turn")]}},
            {"tool": "list_agents", "args": {}},
            {"tool": "interrupt_agent", "args": {"agent_id": "kid-a"}},
            {"tool": "list_agents", "args": {}},
            {"tool": "send_message",
             "args": {"message": "anyone home?", "subagent_id": "kid-a"},
             "steer": "idle"},
            {"tool": "interrupt_agent", "args": {"agent_id": "kid-a"}},
        ],
    })

    # ---- close_session default (True) closes the scratch session ----------
    add({
        "name": "close_session_default",
        "caller_id": "main-2",
        "steps": [
            {"tool": "subagent",
             "args": {"prompt": "one shot", "session_id": "kid-b",
                      "run_in_background": False},
             "run": {"events": [("text", "finished")]}},
            {"tool": "list_agents", "args": {}},
            {"tool": "send_message",
             "args": {"message": "ping", "subagent_id": "kid-b"},
             "steer": "idle"},
            {"tool": "interrupt_agent", "args": {"agent_id": "kid-b"}},
            {"tool": "subagent",
             "args": {"prompt": "again", "session_id": "kid-b",
                      "close_session": False, "run_in_background": False},
             "run": {"events": [("text", "fresh session")]}},
            {"tool": "list_agents", "args": {}},
        ],
    })

    # ---- awaiting_response: the state survives the resume -----------------
    # kimi-agent only records a pending question on an entry that already
    # exists (the sub-agent's AskParent writes into the store entry), so the
    # awaiting flow needs a first run that materialises the entry.
    add({
        "name": "resume_awaiting_response",
        "caller_id": "main-3",
        "steps": [
            {"tool": "subagent",
             "args": {"prompt": "start", "session_id": "kid-c",
                      "close_session": False, "run_in_background": False},
             "run": {"events": [("text", "first turn")]}},
            {"tool": "subagent",
             "args": {"prompt": "ask away", "session_id": "kid-c",
                      "close_session": False, "run_in_background": False},
             "run": {"events": [("text", "I need input")],
                     "ask": "Which format?"}},
            {"tool": "list_agents", "args": {}},
            {"tool": "subagent",
             "args": {"prompt": "continue", "session_id": "kid-c",
                      "close_session": False, "run_in_background": False,
                      "response": "use json"},
             "run": {"events": [("text", "thanks")]}},
            {"tool": "list_agents", "args": {}},
            {"tool": "subagent",
             "args": {"prompt": "and more", "session_id": "kid-c",
                      "close_session": False, "run_in_background": False},
             "run": {"events": [("text", "ok")]}},
            {"tool": "list_agents", "args": {}},
            {"tool": "interrupt_agent", "args": {"agent_id": "kid-c"}},
        ],
    })

    # ---- awaiting_response still pending when the parent pings again ------
    add({
        "name": "resume_awaiting_without_response",
        "caller_id": "main-4",
        "steps": [
            {"tool": "subagent",
             "args": {"prompt": "start", "session_id": "kid-d",
                      "close_session": False, "run_in_background": False},
             "run": {"events": [("text", "first turn")]}},
            {"tool": "subagent",
             "args": {"prompt": "ask away", "session_id": "kid-d",
                      "close_session": False, "run_in_background": False},
             "run": {"events": [("text", "question")],
                     "ask": "Which format?"}},
            {"tool": "send_message",
             "args": {"message": "still there?", "subagent_id": "kid-d"},
             "steer": "idle"},
            {"tool": "subagent",
             "args": {"prompt": "carry on", "session_id": "kid-d",
                      "close_session": False, "run_in_background": False},
             "run": {"events": [("text", "carried on")]}},
            {"tool": "list_agents", "args": {}},
        ],
    })

    # ---- MAX_SESSIONS = 10: LRU eviction on the 11th spawn ---------------
    add({
        "name": "evict_at_max_sessions",
        "caller_id": "main-5",
        "steps": (
            [{"tool": "subagent",
              "args": {"prompt": f"task {i}", "session_id": f"slot-{i}",
                       "close_session": False, "run_in_background": False},
              "run": {"events": [("text", f"done {i}")]}} for i in range(10)]
            + [
                {"tool": "list_agents", "args": {}},
                # A close_session=False resume of an existing entry must NOT
                # evict anything (Python only evicts for a brand-new entry).
                {"tool": "subagent",
                 "args": {"prompt": "again slot-5", "session_id": "slot-5",
                          "close_session": False, "run_in_background": False},
                 "run": {"events": [("text", "ok")]}},
                {"tool": "list_agents", "args": {}},
                # A close_session=True spawn must not evict either.
                {"tool": "subagent",
                 "args": {"prompt": "one shot", "session_id": "slot-tmp",
                          "run_in_background": False},
                 "run": {"events": [("text", "bye")]}},
                {"tool": "list_agents", "args": {}},
                # The 11th kept entry evicts the least recently used one.
                {"tool": "subagent",
                 "args": {"prompt": "task 11", "session_id": "slot-10",
                          "close_session": False, "run_in_background": False},
                 "run": {"events": [("text", "done 11")]}},
                {"tool": "list_agents", "args": {}},
            ]
        ),
    })

    # ---- context_files / context_data / @file prompts --------------------
    add({
        "name": "context_and_prompt_files",
        "caller_id": "main-6",
        "cpp_live_exclude": ["ctx-3"],
        "cpp_note": (
            "the port registers a live session only when it hands a request to "
            "the runner; kimi-agent creates the SDK session before resolving "
            "@prompt, so the failed spawn's id stays registered"),
        "steps": [
            {"tool": "subagent",
             "args": {"prompt": "@" + PROMPT_FILE, "session_id": "ctx-1",
                      "close_session": False, "run_in_background": False},
             "run": {"events": [("text", "read the file")]}},
            {"tool": "subagent",
             "args": {"prompt": "inline task", "session_id": "ctx-2",
                      "close_session": False, "run_in_background": False,
                      "context_files": [CONTEXT_FILE, MISSING_FILE],
                      "context_data": {"alpha": 1, "beta": ["x", "y"],
                                       "gamma": {"n": 2.5, "t": True}}},
             "run": {"events": [("text", "with context")]}},
            {"tool": "subagent",
             "args": {"prompt": "@" + MISSING_FILE, "session_id": "ctx-3",
                      "close_session": False, "run_in_background": False},
             "run": {"events": [("text", "never runs")]}},
            {"tool": "send_message",
             "args": {"message": "queued for a fresh id", "subagent_id": "ghost-1"},
             "steer": "idle"},
            {"tool": "subagent",
             "args": {"prompt": "new session under the queued id",
                      "session_id": "ghost-1", "close_session": False,
                      "run_in_background": False},
             "run": {"events": [("text", "got the queue")]}},
            {"tool": "list_agents", "args": {}},
        ],
    })

    # ---- runner failure ---------------------------------------------------
    add({
        "name": "runner_error",
        "caller_id": "main-7",
        "steps": [
            {"tool": "subagent",
             "args": {"prompt": "will fail", "session_id": "err-1",
                      "close_session": False, "run_in_background": False},
             "run": {"events": [("text", "partial")], "error": "boom"}},
            {"tool": "list_agents", "args": {}},
            {"tool": "send_message",
             "args": {"message": "anyone?", "subagent_id": "err-1"},
             "steer": "idle"},
        ],
    })

    # ---- recursion guard --------------------------------------------------
    add({
        "name": "recursion_guard",
        "caller_id": "sub-1",
        "is_sub_agent": True,
        "parent_session_id": "main-9",
        "steps": [
            {"tool": "subagent",
             "args": {"prompt": "nested", "run_in_background": False},
             "run": {"events": [("text", "never")]}},
            {"tool": "list_agents", "args": {}},
        ],
    })

    # ---- a sub-agent's send_message always targets its parent ------------
    add({
        "name": "sub_agent_messages_parent",
        "caller_id": "sub-2",
        "is_sub_agent": True,
        "parent_session_id": "main-9",
        "steps": [
            {"tool": "send_message",
             "args": {"message": "parent?", "subagent_id": "someone-else"},
             "steer": "idle"},
            {"tool": "send_message",
             "args": {"question": "alias form"},
             "steer": "idle"},
        ],
    })

    # ---- argument aliases + validation -----------------------------------
    add({
        "name": "aliases_and_validation",
        "caller_id": "main-8",
        "steps": [
            {"tool": "subagent",
             "args": {"task": "alias prompt", "session": "alias-1",
                      "close_session": False, "run_in_background": False},
             "run": {"events": [("text", "aliased")]}},
            {"tool": "subagent",
             "args": {"prompt": "no prompt text", "session_id": "alias-2",
                      "close_session": False, "run_in_background": False,
                      "return_history": True, "history_format": "markdown"},
             "run": {"events": [("text", "markdown history")]}},
            {"tool": "subagent",
             "args": {"prompt": "summary please", "session_id": "alias-3",
                      "close_session": False, "run_in_background": False,
                      "return_history": True, "history_format": "summary"},
             "run": {"events": _reader_events()}},
            {"tool": "subagent",
             "args": {"prompt": "json please", "session_id": "alias-4",
                      "close_session": False, "run_in_background": False,
                      "return_history": True},
             "run": {"events": [("text", "one"), ("thinking", "two"),
                                ("tool_call", "Bash(ls)"),
                                ("tool_result", "out")]}},
            {"tool": "subagent", "args": {"description": "no prompt"}},
            {"tool": "subagent",
             "args": {"prompt": "bad format", "history_format": "yaml",
                      "run_in_background": False}},
            {"tool": "subagent",
             "args": {"prompt": 42, "session_id": "coerce-1",
                      "close_session": False, "run_in_background": False,
                      "description": 7},
             "run": {"events": [("text", "coerced int")]}},
            {"tool": "subagent",
             "args": {"prompt": True, "session_id": "coerce-2",
                      "close_session": False, "run_in_background": False},
             "run": {"events": [("text", "coerced bool")]}},
            {"tool": "subagent",
             "args": {"prompt": 2.5, "session_id": "coerce-3",
                      "close_session": False, "run_in_background": False},
             "run": {"events": [("text", "coerced float")]}},
            {"tool": "subagent",
             "args": {"prompt": 3.0, "session_id": "coerce-4",
                      "close_session": False, "run_in_background": False},
             "run": {"events": [("text", "coerced integral float")]}},
            # Python's str(float) is repr(): scientific + two-digit exponent
            # outside its plain range, unlike orjson's JSON rendering.
            {"tool": "subagent",
             "args": {"prompt": 1.0e-5, "session_id": "coerce-5",
                      "close_session": False, "run_in_background": False},
             "run": {"events": [("text", "coerced small float")]}},
            {"tool": "subagent",
             "args": {"prompt": 1.0e16, "session_id": "coerce-6",
                      "close_session": False, "run_in_background": False},
             "run": {"events": [("text", "coerced big float")]}},
            {"tool": "send_message", "args": {"subagent_id": "x"}},
            {"tool": "send_message",
             "args": {"message": 7, "subagent_id": "coerce-1"},
             "steer": "idle"},
            {"tool": "interrupt_agent", "args": {}},
            {"tool": "interrupt_agent", "args": {"agent_id": 5}},
            {"tool": "list_agents", "args": {"scope": 5}},
        ],
    })

    # ---- anonymous spawn: the id is allocated by the tool -----------------
    add({
        "name": "anonymous_spawn",
        "caller_id": "anon-0",
        "steps": [
            {"tool": "subagent",
             "args": {"prompt": "anonymous task", "close_session": False,
                      "run_in_background": False},
             "run": {"events": [("text", "anon done")]}},
            {"tool": "list_agents", "args": {}},
            {"tool": "send_message",
             "args": {"message": "anon ping", "subagent_id": "<NEW0>"},
             "steer": "idle"},
            {"tool": "interrupt_agent", "args": {"agent_id": "<NEW0>"},
                "cpp": {"pending": {"<NEW0>": [
                    "Message from agent 'anon-0':\nanon ping"]},
                 "note": ("kimi-agent's store.close() drops the queued "
                          "message; the port keeps it (documented contract)")}},
            {"tool": "list_agents", "args": {},
             "cpp": {"pending": {"<NEW0>": [
                 "Message from agent 'anon-0':\nanon ping"]},
                 "note": ("kimi-agent's store.close() drops the queued "
                          "message; the port keeps it (documented contract)")}},
        ],
    })

    # ---- send_message rows (fresh session per row) ------------------------
    add({
        "name": "send_no_active_subagents", "caller_id": "row-1",
        "pre_steer": "idle",
        "steps": [{"tool": "send_message", "args": {"message": "hi"}}],
    })
    add({
        "name": "send_unknown_id", "caller_id": "row-2",
        "pre_steer": "idle",
        "steps": [{"tool": "send_message",
                   "args": {"message": "hi", "subagent_id": "nope"}}],
    })
    add({
        "name": "send_self_message", "caller_id": "row-3",
        "pre_live": ["row-3"], "pre_steer": "idle",
        "steps": [{"tool": "send_message",
                   "args": {"message": "hi", "subagent_id": "row-3"}}],
    })
    add({
        "name": "send_target_idle_queues", "caller_id": "row-4",
        "pre_live": ["target-1"],
        "pre_entries": [{"id": "target-1", "created_at": 900.0,
                         "last_accessed": 950.0, "total_turns": 2,
                         "state": "completed"}],
        "pre_steer": "idle", "pre_steer_targets": ["target-1"],
        "steps": [{"tool": "send_message",
                   "args": {"message": "status?", "subagent_id": "target-1"}}],
    })
    add({
        "name": "send_target_not_steerable_queues", "caller_id": "row-5",
        "pre_live": ["target-2"],
        "pre_entries": [{"id": "target-2", "created_at": 900.0,
                         "last_accessed": 950.0, "total_turns": 1,
                         "state": "completed"}],
        "pre_steer": "none",
        "steps": [{"tool": "send_message",
                   "args": {"message": "hello?", "subagent_id": "target-2"}}],
    })
    add({
        "name": "send_target_delivered", "caller_id": "row-6",
        # "delivered" needs a live run on the C++ side; the row's registry starts
        # a gated background run for the target before the step.
        "pre_live": ["target-3"], "pre_steer": "delivered",
        "pre_steer_targets": ["target-3"],
        "pre_entries": [{"id": "target-3", "created_at": 900.0,
                         "last_accessed": 950.0, "total_turns": 1,
                         "state": "running"}],
        "pre_run": "target-3",
        "steps": [{"tool": "send_message",
                   "args": {"message": "go on", "subagent_id": "target-3"}}],
    })
    add({
        "name": "send_target_closed_queues", "caller_id": "row-7",
        "pre_steer": "idle",
        "steps": [{"tool": "send_message",
                   "args": {"message": "come back",
                            "subagent_id": "closed-1"}}],
    })
    add({
        "name": "send_default_most_recent", "caller_id": "row-8",
        "pre_live": ["recent-1", "old-1"],
        "pre_entries": [
            {"id": "old-1", "created_at": 900.0, "last_accessed": 900.0,
             "total_turns": 1, "state": "completed"},
            {"id": "recent-1", "created_at": 950.0, "last_accessed": 990.0,
             "total_turns": 3, "state": "completed"},
        ],
        "pre_steer": "idle", "pre_steer_targets": ["recent-1", "old-1"],
        "steps": [{"tool": "send_message", "args": {"message": "latest?"}}],
    })
    add({
        "name": "send_default_skips_inactive", "caller_id": "row-9",
        "pre_live": ["only-1"],
        "pre_entries": [
            {"id": "gone-1", "created_at": 900.0, "last_accessed": 999.0,
             "total_turns": 1, "state": "completed", "is_active": False},
            {"id": "only-1", "created_at": 950.0, "last_accessed": 960.0,
             "total_turns": 2, "state": "completed"},
        ],
        "pre_steer": "idle", "pre_steer_targets": ["only-1"],
        "steps": [{"tool": "send_message", "args": {"message": "you?"}}],
    })
    add({
        "name": "send_sub_without_parent", "caller_id": "sub-row-1",
        "is_sub_agent": True, "pre_steer": "idle",
        "steps": [{"tool": "send_message", "args": {"message": "hello"}}],
    })
    add({
        "name": "send_sub_parent_unregistered", "caller_id": "sub-row-2",
        "is_sub_agent": True, "parent_session_id": "missing-parent",
        "pre_steer": "idle",
        "steps": [{"tool": "send_message", "args": {"message": "hello"}}],
    })
    add({
        "name": "send_sub_parent_delivered", "caller_id": "sub-row-3",
        "is_sub_agent": True, "parent_session_id": "parent-9",
        "pre_live": ["parent-9"], "pre_steer": "delivered",
        "pre_steer_targets": ["parent-9"], "pre_run": "parent-9",
        "pre_entries": [{"id": "parent-9", "created_at": 900.0,
                         "last_accessed": 950.0, "total_turns": 1,
                         "state": "running"}],
        "steps": [{"tool": "send_message",
                   "args": {"message": "parent, are you there?"}}],
    })

    # ---- interrupt_agent rows --------------------------------------------
    add({
        "name": "interrupt_unknown", "caller_id": "row-10",
        "steps": [{"tool": "interrupt_agent", "args": {"agent_id": "nope"}}],
    })
    add({
        "name": "interrupt_completed", "caller_id": "row-11",
        "pre_live": ["done-1"],
        "pre_entries": [{"id": "done-1", "created_at": 900.0,
                         "last_accessed": 950.0, "total_turns": 4,
                         "state": "completed"}],
        "steps": [{"tool": "interrupt_agent", "args": {"agent_id": "done-1"}},
                  {"tool": "list_agents", "args": {}}],
    })
    add({
        "name": "interrupt_alias_params", "caller_id": "row-12",
        "pre_live": ["done-2"],
        "pre_entries": [{"id": "done-2", "created_at": 900.0,
                         "last_accessed": 950.0, "total_turns": 1,
                         "state": "awaiting_response"}],
        "steps": [{"tool": "interrupt_agent", "args": {"session": "done-2"}},
                  {"tool": "list_agents", "args": {}}],
    })
    add({
        "name": "interrupt_queued_messages", "caller_id": "row-13",
        "pre_live": ["idle-9"],
        "pre_entries": [{"id": "idle-9", "created_at": 900.0,
                         "last_accessed": 950.0, "total_turns": 1,
                         "state": "completed"}],
        "pre_steer": "idle", "pre_steer_targets": ["idle-9"],
        "steps": [
            {"tool": "send_message",
             "args": {"message": "queued then interrupted",
                      "subagent_id": "idle-9"}},
            {"tool": "interrupt_agent", "args": {"agent_id": "idle-9"},
             "cpp": {"pending": {"idle-9": [
                 "Message from agent 'row-13':\nqueued then interrupted"]},
                     "note": ("kimi-agent's store.close() notifies "
                              "_forget_child_session_record, which drops "
                              "_pending_messages for the id; the port keeps "
                              "them (documented contract: queued messages "
                              "survive interrupt_agent and are listed on "
                              "resume)")}},
            {"tool": "list_agents", "args": {},
             "cpp": {"pending": {"idle-9": [
                 "Message from agent 'row-13':\nqueued then interrupted"]},
                     "note": "same queued-message retention as the previous "
                             "step"}},
            {"tool": "subagent",
             "args": {"prompt": "resume the id", "session_id": "idle-9",
                      "close_session": False, "run_in_background": False},
             "cpp": {"prompts": [
                 "resume the id\n\n<pending-messages>\nYou have the "
                 "following queued message(s) from the parent agent (sent "
                 "while you were idle or not running):\n1. Message from agent "
                 "'row-13':\nqueued then interrupted\n</pending-messages>"],
                 "note": "the port still lists the queued message at the "
                         "next prompt (kimi-agent dropped it on close)"},
             "run": {"events": [("text", "resumed")]}},
        ],
    })

    # ---- list_agents rows: orjson rendering of nasty values --------------
    floats = [1000.0, 100.5, 0.0, -0.0, 1e-05, 1e-06, 1e15, 1e16, 1e30,
              123456789.0, 0.1, 1.0 / 3.0, 1234.5678, 2.5e-07]
    for index, value in enumerate(floats):
        add({
            "name": f"list_float_{index}", "caller_id": "row-20",
            "pre_entries": [{"id": f"f-{index}", "created_at": value,
                             "last_accessed": value, "total_turns": index,
                             "state": "completed"}],
            "steps": [{"tool": "list_agents", "args": {}}],
        })
    add({
        "name": "list_long_values", "caller_id": "row-21",
        "pre_entries": [
            {"id": "x" * 300, "created_at": 900.0, "last_accessed": 950.0,
             "total_turns": 12, "state": "a" * 200},
            {"id": "quote\"back\\slash", "created_at": 1.0,
             "last_accessed": 2.0, "total_turns": 0,
             "state": "unicode \u2014 caf\u00e9 \U0001f680"},
            {"id": "ctrl\bbell\fx", "created_at": 3.0, "last_accessed": 4.0,
             "total_turns": 1, "state": "tab\tnl\nret\r"},
        ],
        "steps": [{"tool": "list_agents", "args": {}},
                  {"tool": "list_agents", "args": {"scope": "descendants"}}],
    })
    add({
        "name": "list_empty_and_inactive", "caller_id": "row-22",
        "steps": [{"tool": "list_agents", "args": {}}],
    })

    return C


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------


def _known_ids(case) -> set:
    ids = {case.get("caller_id", "")}
    for step in case["steps"]:
        args = step.get("args", {})
        for key in ("session_id", "session", "subagent_id", "id", "agent_id"):
            if isinstance(args.get(key), str):
                ids.add(args[key])
    for entry in case.get("pre_entries", []):
        ids.add(entry["id"])
    for sid in case.get("pre_live", []):
        ids.add(sid)
    return {i for i in ids if i}


async def _run_corpus() -> list[dict]:
    ref = _import_reference()
    agent = ref["agent"]
    types = _msg_types(ref["MessageType"])
    rows: list[dict] = []

    # Materialise the @file prompt sources for the reference (the C++ replay
    # injects them through the registry reader hook instead).
    for rel, text in FILES:
        path = Path(rel)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="")

    try:
        for case in corpus():
            rows.extend(await _run_case(ref, agent, types, case))
    finally:
        for rel, _ in FILES:
            try:
                Path(rel).unlink()
            except OSError:
                pass
    return rows


async def _run_case(ref, agent, types, case) -> list[dict]:
    harness = Harness(
        ref,
        case.get("caller_id", ""),
        case.get("is_sub_agent", False),
        case.get("parent_session_id", ""),
        FILES,
    )
    harness.steer_mode = case.get("pre_steer", "idle")
    harness.steer_targets = set(case.get("pre_steer_targets", []))

    # ---- explicit setup (row cases) ---------------------------------------
    for session_id in case.get("pre_live", []):
        harness.register_live(session_id)
    for entry in case.get("pre_entries", []):
        harness.put_entry(
            entry["id"], entry["created_at"], entry["last_accessed"],
            entry["total_turns"], entry["state"],
            entry.get("is_active", True), entry.get("pending_question"),
        )
    for target, message in case.get("pre_pending", {}).items():
        for item in message if isinstance(message, list) else [message]:
            harness.queue_pending(target, item)

    rows: list[dict] = []
    fresh_ids: list[str] = []
    #: subagent steps that allocate their own id and must not background
    for index, step in enumerate(case["steps"]):
        harness.clock.value = 1000.0 + 7.0 * index
        step_steer = step.get("steer", case.get("steer", harness.steer_mode))
        harness.steer_mode = step_steer
        run = step.get("run")
        harness.step_events = [(types[kind], text)
                               for kind, text in (run or {}).get("events", [])]
        harness.step_error = (run or {}).get("error")
        harness.step_ask = (run or {}).get("ask")
        harness.prompts = []
        harness.saved = []
        harness.pushed = []
        harness.runner_output = ""
        before_created = len(harness.created)

        args = dict(step.get("args", {}))
        raw_args = json.loads(json.dumps(args))
        args = _substitute_tokens(args, fresh_ids)
        result = await harness.call(step["tool"], args)
        validation_error = isinstance(result, ref["ToolValidateError"])

        # fresh (tool-allocated) session ids, in allocation order: the reference
        # only allocates one when the caller did not name a session.
        explicit = str(args.get("session_id") or args.get("session") or "")
        if step["tool"] == "subagent" and not explicit:
            for call in harness.created[before_created:]:
                sid = call.get("session_id", "")
                if sid and sid not in fresh_ids:
                    fresh_ids.append(sid)

        extras = getattr(result, "extras", None)
        row = {
            "case": case["name"],
            "step": index,
            "tool": step["tool"],
            "caller_id": case.get("caller_id", ""),
            "is_sub_agent": bool(case.get("is_sub_agent", False)),
            "parent_session_id": case.get("parent_session_id", ""),
            "files": FILES,
            "pre_live": list(case.get("pre_live", [])) if index == 0 else [],
            "pre_entries": case.get("pre_entries", []) if index == 0 else [],
            "pre_pending": case.get("pre_pending", {}) if index == 0 else {},
            "pre_steer": case.get("pre_steer", "idle") if index == 0 else "",
            "pre_run": case.get("pre_run", "") if index == 0 else "",
            "args": raw_args,
            "now": harness.clock.value,
            "runner": harness.runner_result() if step["tool"] == "subagent"
            else None,
            "validation_error": validation_error,
            "is_error": bool(getattr(result, "is_error", True)),
            "status": _status_for(step["tool"], result, validation_error),
            "output": "" if validation_error else _as_text(result.output),
            "message": "" if validation_error else (result.message or ""),
            "brief": "" if validation_error else result.brief,
            "has_extras": extras is not None,
            "extras": extras if extras is not None else {},
            "note": "",
            "state": _snapshot(harness, case, fresh_ids),
            "state_cpp_live": None,
            "state_cpp_pending": None,
            "state_cpp_prompts": None,
        }
        if (step["tool"] == "subagent" and not row["is_error"]
                and not validation_error
                and args.get("run_in_background") is not False):
            raise AssertionError(
                "%s step %d: the reference ignores run_in_background, so every "
                "comparable spawn must pass run_in_background=false"
                % (case["name"], index))
        _mask_row(row, fresh_ids)
        _apply_modelling_notes(row)
        _apply_cpp_overrides(row, case, step)
        rows.append(row)
    return rows


def _apply_cpp_overrides(row: dict, case, step) -> None:
    """Hand-transcribed expectations for documented port deviations.

    The replay compares `state` (the reference's state) unless a row carries an
    explicit override; every override is paired with a `note` explaining why the
    port deliberately differs.  See the .inc header.
    """
    overrides = dict(case.get("cpp", {}))
    overrides.update(step.get("cpp", {}))
    if "live" in overrides:
        row["state_cpp_live"] = overrides["live"]
    if "pending" in overrides:
        row["state_cpp_pending"] = overrides["pending"]
    if "prompts" in overrides:
        row["state_cpp_prompts"] = overrides["prompts"]
    note = overrides.get("note") or case.get("cpp_note", "")
    if note:
        row["note"] = note


def _substitute_tokens(value, fresh_ids: list[str]):
    """Expand the <NEWn> tokens of a raw corpus argument tree."""
    if isinstance(value, str):
        for index, session_id in enumerate(fresh_ids):
            value = value.replace(f"<NEW{index}>", session_id)
        return value
    if isinstance(value, list):
        return [_substitute_tokens(v, fresh_ids) for v in value]
    if isinstance(value, dict):
        return {k: _substitute_tokens(v, fresh_ids) for k, v in value.items()}
    return value


def _mask_row(row: dict, fresh_ids: list[str]) -> None:
    """Replace tool-allocated ids with <NEWn> in every recorded string.

    The C++ replay performs the same substitution with the ids its own tool
    allocated for the same step, so both sides compare identical bytes.
    """
    for key in ("output", "message", "brief"):
        row[key] = _mask(row[key], fresh_ids)
    row["state"] = json.loads(_mask(_json(row["state"]), fresh_ids))
    if row["has_extras"]:
        row["extras"] = json.loads(_mask(_json(row["extras"]), fresh_ids))
    if row["runner"] is not None:
        row["runner"] = json.loads(_mask(_json(row["runner"]), fresh_ids))


def _status_for(tool: str, result, validation_error: bool) -> str:
    """Map the Python branch onto the C++ port's tool_status.

    The Python side has no status code (``ToolReturnValue`` only carries
    ``is_error``); the C++ envelope adds one.  This is the documented mapping of
    the port (see src/builtin_tools/agent_tool.cpp):
    """
    if validation_error:
        return "invalid_input"
    if not result.is_error:
        return "ok"
    if tool == "subagent":
        if result.message == "Recursive sub-agent call detected":
            return "blocked"
        if result.brief == "sub-agent task failed":
            return "external_library"
        return "not_found"
    if tool == "send_message":
        if result.message == "Cannot message yourself.":
            return "invalid_input"
        return "not_found"
    return "not_found"


def _as_text(value) -> str:
    if isinstance(value, str):
        return value
    return json.dumps(value, ensure_ascii=False)


def _snapshot(harness: Harness, case, fresh_ids: list[str] | None = None) -> dict:
    """Store + registry state after a step (see the .inc header)."""
    import orjson  # noqa: PLC0415

    agent = harness.agent
    ids = {case.get("caller_id", "")}
    for step in case["steps"]:
        for key in ("session_id", "session", "subagent_id", "id", "agent_id"):
            value = step.get("args", {}).get(key)
            if isinstance(value, str):
                # The corpus may reference a tool-allocated id through a <NEWn>
                # token; the state is keyed by the real id.
                ids.add(_substitute_tokens(value, list(fresh_ids or [])))
    for entry in case.get("pre_entries", []):
        ids.add(entry["id"])
    ids.update(case.get("pre_live", []))
    ids.update(fresh_ids)
    ids.update(harness.store.entries.keys())
    ids.update(agent._agent_sessions.keys())
    ids.update(agent._pending_messages.keys())
    ids.discard("")

    fresh = list(fresh_ids or [])
    entries: dict = {}
    # Sort by the *masked* key so a tool-allocated id cannot flip the object
    # order between runs (uuid4 collides with a literal id differently).
    for session_id in sorted(ids, key=lambda i: _mask(i, fresh)):
        entry = harness.store.get(session_id)
        entries[_mask(session_id, fresh)] = None if entry is None else {
            "created_at": entry.created_at,
            "last_accessed": entry.last_accessed,
            "total_turns": entry.total_turns,
            "state": entry.state,
            "is_active": entry.is_active,
            "pending_question": entry.pending_question,
        }
    excluded = set(case.get("cpp_live_exclude", []))
    live = sorted(_mask(i, fresh) for i in agent._agent_sessions.keys()
                  if i not in excluded)
    pending = {
        _mask(i, fresh): list(messages)
        for i, messages in sorted(agent._pending_messages.items(),
                                  key=lambda kv: _mask(kv[0], fresh))
        if messages
    }
    return {
        "list": _mask(orjson.dumps(harness.store.list_active(),
                                   option=orjson.OPT_INDENT_2).decode(), fresh),
        "live": live,
        "entries": entries,
        "pending": pending,
        "saved": [_mask(s, fresh) for s in harness.saved],
        "prompts": [_mask(p, fresh) for p in harness.prompts],
    }


def _mask(text: str, fresh_ids: list[str]) -> str:
    """Replace tool-allocated session ids with the ``<NEWn>`` token."""
    for index, session_id in enumerate(fresh_ids):
        text = text.replace(session_id, f"<NEW{index}>")
    return text


def _apply_modelling_notes(row: dict) -> None:
    """Document the two states the C++ port cannot represent.

    ``Steer.from_session`` returning None ("no steerable session") and a live
    soul that refuses the push ("not running") are distinct reasons in
    kimi-agent; the C++ registry only knows "live session with no run in
    flight", which it renders as "not running".  Rows that hit the
    un-representable reason keep Python's bytes as ``output`` and record the
    port's rendering in ``output_cpp`` + ``note``.
    """
    row["output_cpp"] = row["output"]
    row["message_cpp"] = row["message"]
    text = row["output"]
    if "is not running (no steerable session)" in text:
        row["output_cpp"] = text.replace("(no steerable session)",
                                         "(not running)")
        row["note"] = ("C++ registry cannot distinguish a live-but-unsteerable "
                       "session from an idle one: reason rendered as "
                       "'not running'")


# ---------------------------------------------------------------------------
# Emit
# ---------------------------------------------------------------------------


def c_literal(text: str, chunk: int = 1000) -> str:
    """Emit one or more adjacent C++ narrow string literals for UTF-8 `text`."""
    data = text.encode("utf-8")
    chunks: list[list[str]] = [[]]
    size = 0
    pending_hex = False
    for byte in data:
        ch = chr(byte)
        if byte == 0x22:
            token = '\\"'
            pending_hex = False
        elif byte == 0x5C:
            token = "\\\\"
            pending_hex = False
        elif byte == 0x0A:
            token = "\\n"
            pending_hex = False
        elif byte == 0x0D:
            token = "\\r"
            pending_hex = False
        elif byte == 0x09:
            token = "\\t"
            pending_hex = False
        elif 0x20 <= byte < 0x7F:
            token = ('""' + ch) if (pending_hex and ch in "0123456789abcdefABCDEF") else ch
            pending_hex = False
        else:
            token = "\\x%02x" % byte
            pending_hex = True
        if size >= chunk and not pending_hex:
            chunks.append([])
            size = 0
        chunks[-1].append(token)
        size += len(token)
    return '\n '.join('"' + "".join(part) + '"' for part in chunks)


def _num(value: object) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    return repr(value)


def _json(value) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def _emit(rows: list[dict]) -> str:
    lines: list[str] = []
    lines.append("// GENERATED by scripts/gen_agent_goldens.py from the kimi-agent")
    lines.append("// reference implementation (src/kimix/tools/agent/__init__.py +")
    lines.append("// store.py) driven against a stub runtime/session.")
    lines.append("// Do not edit by hand - regenerate with:")
    lines.append("//   python scripts/gen_agent_goldens.py")
    lines.append("//")
    lines.append("// One row per tool call.  A row with step == 0 starts a fresh case")
    lines.append("// (fresh parent session + agent_registry, setup from pre_*); later")
    lines.append("// steps continue the previous case.  Fields:")
    lines.append("//   args        raw JSON object handed to the tool")
    lines.append("//   files       JSON [[path, content], ...] the registry reader serves")
    lines.append("//   pre_live    ids whose session was registered in the live map")
    lines.append("//   pre_entries JSON store entries inserted before the step")
    lines.append("//   pre_pending JSON {id: [messages, ...]} queued before the step")
    lines.append("//               (message content shows up in the next resume prompt)")
    lines.append("//   pre_steer   \"idle\" | \"delivered\" | \"none\" (send_message rows)")
    lines.append("//   pre_run     id with an in-flight background run (delivered rows)")
    lines.append("//   runner      JSON runner result for a subagent step (host boundary)")
    lines.append("//   now         the injected clock value for the step")
    lines.append("//   output/message/brief/extras  the Python tool result (extras == {}")
    lines.append("//               when the Python result carried extras=None)")
    lines.append("//   output_cpp/message_cpp  the expected C++ bytes; differs from")
    lines.append("//               output/message only where `note` documents a modelling")
    lines.append("//               gap of the port (never a behavioural fix)")
    lines.append("//   state       JSON: list (orjson OPT_INDENT_2 text of list_active),")
    lines.append("//               live (sorted live ids), entries (per id: store fields or")
    lines.append("//               null), pending (id -> queued count), saved (prompts")
    lines.append("//               written to the shared temp folder), prompts (the")
    lines.append("//               effective prompt handed to the runner, in order)")
    lines.append("//   state_cpp_*  hand-transcribed replacement for one field of `state`")
    lines.append("//               (live/pending/prompts), used only where the port")
    lines.append("//               deliberately deviates; always paired with `note`")
    lines.append("//   note        non-empty when the port deviates from the reference")
    lines.append("//")
    lines.append("// Tool-allocated session ids are masked as <NEW0>, <NEW1>, ... in")
    lines.append("// insertion order; the replay substitutes its own fresh ids.")
    lines.append("struct agent_golden_step {")
    lines.append(" const char *case_name;")
    lines.append(" int32_t step;")
    lines.append(" const char *caller_id; // \"\" == None (no session id)")
    lines.append(" bool is_sub_agent;")
    lines.append(" const char *parent_session_id;")
    lines.append(" const char *files; // JSON [[path, content], ...]")
    lines.append(" const char *pre_live; // JSON [id, ...]")
    lines.append(" const char *pre_entries; // JSON [{...}, ...]")
    lines.append(" const char *pre_pending; // JSON {id: [messages]}")
    lines.append(" const char *pre_steer; // \"\" | idle | delivered | none")
    lines.append(" const char *pre_run; // id with an in-flight background run")
    lines.append(" const char *tool; // subagent | send_message | list_agents | interrupt_agent")
    lines.append(" const char *args; // raw JSON object")
    lines.append(" double now; // injected clock value")
    lines.append(" const char *runner; // JSON runner result (\"\" when unused)")
    lines.append(" bool validation_error; // Python rejected the args before the body")
    lines.append(" bool is_error;")
    lines.append(" const char *status; // C++ tool_status (no Python counterpart)")
    lines.append(" const char *output_cpp;")
    lines.append(" const char *message_cpp;")
    lines.append(" const char *output; // exact Python bytes")
    lines.append(" const char *message; // exact Python bytes")
    lines.append(" const char *brief;")
    lines.append(" bool has_extras;")
    lines.append(" const char *extras; // JSON object (canonical)")
    lines.append(" const char *state; // JSON state snapshot")
    lines.append(" const char *state_cpp_live; // JSON [] override (documented deviation)")
    lines.append(" const char *state_cpp_pending; // JSON {} override (documented deviation)")
    lines.append(" const char *state_cpp_prompts; // JSON [] override (documented deviation)")
    lines.append(" const char *note;")
    lines.append("};")
    lines.append("")
    lines.append("const agent_golden_step kAgentGoldens[] = {")
    for row in rows:
        fields = [
            c_literal(row["case"]),
            str(row["step"]),
            c_literal(row["caller_id"]),
            "true" if row["is_sub_agent"] else "false",
            c_literal(row["parent_session_id"]),
            c_literal(_json(row["files"])),
            c_literal(_json(row["pre_live"])),
            c_literal(_json(row["pre_entries"])),
            c_literal(_json(row["pre_pending"])),
            c_literal(row["pre_steer"]),
            c_literal(row["pre_run"]),
            c_literal(row["tool"]),
            c_literal(_json(row["args"])),
            _num(row["now"]),
            c_literal("" if row["runner"] is None else _json(row["runner"])),
            "true" if row["validation_error"] else "false",
            "true" if row["is_error"] else "false",
            c_literal(row["status"]),
            c_literal(row["output_cpp"]),
            c_literal(row["message_cpp"]),
            c_literal(row["output"]),
            c_literal(row["message"]),
            c_literal(row["brief"]),
            "true" if row["has_extras"] else "false",
            c_literal(_json(row["extras"])),
            c_literal(_json(row["state"])),
            c_literal("" if row["state_cpp_live"] is None
                      else _json(row["state_cpp_live"])),
            c_literal("" if row["state_cpp_pending"] is None
                      else _json(row["state_cpp_pending"])),
            c_literal("" if row["state_cpp_prompts"] is None
                      else _json(row["state_cpp_prompts"])),
            c_literal(row["note"]),
        ]
        lines.append(" {" + ", ".join(fields) + "},")
    lines.append("};")
    lines.append("")
    lines.append("const size_t kAgentGoldenCount =")
    lines.append("    sizeof(kAgentGoldens) / sizeof(kAgentGoldens[0]);")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="fail when the checked-in .inc differs")
    parser.add_argument("--python", default=None,
                        help="interpreter able to import kimix.tools.agent")
    parser.add_argument("--out", default=str(OUT_PATH), help="output .inc path")
    parser.add_argument("--dump", default=None,
                        help="debug: also write the raw rows as JSON here")
    args = parser.parse_args()
    try:
        _import_reference()
    except Exception as exc:  # pragma: no cover - environment issue
        fallback = Path(args.python) if args.python else DEFAULT_AGENT_PYTHON
        if fallback.is_file() and Path(sys.executable) != fallback:
            print("re-running under %s (%s)" % (fallback, exc), file=sys.stderr)
            cmd = [str(fallback), str(Path(__file__).resolve()), *sys.argv[1:]]
            return subprocess.call(cmd)
        print("cannot import kimix.tools.agent from %s: %s"
              % (KIMI_AGENT_ROOT, exc), file=sys.stderr)
        return 2
    rows = asyncio.run(_run_corpus())
    if args.dump:
        Path(args.dump).write_text(
            json.dumps(rows, ensure_ascii=False, indent=1), encoding="utf-8")
    rows = asyncio.run(_run_corpus())
    text = _emit(rows)
    out_path = Path(args.out)
    if args.check:
        existing = out_path.read_text(encoding="utf-8") if out_path.exists() else ""
        if existing != text:
            print("%s is out of date" % out_path, file=sys.stderr)
            return 1
        print("%s is up to date (%d steps)" % (out_path, len(rows)))
        return 0
    out_path.write_text(text, encoding="utf-8", newline="\n")
    print("wrote %s (%d steps, %d cases) [reference %s]"
          % (out_path, len(rows), len({r["case"] for r in rows}),
             _import_reference()["agent"].__file__))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
