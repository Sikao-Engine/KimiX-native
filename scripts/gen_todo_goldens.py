#!/usr/bin/env python3
"""Regenerate tests/unit/builtin_tools/todo_goldens.inc.

The golden vectors are produced by running the *real* Python implementation of
the todo tools (``kimi_cli.tools.todo.TodoList`` / ``todo_update``) from the
kimi-agent checkout against a stub runtime/session, then recording, after every
step:

  * the tool result (``is_error`` / ``output`` / ``message`` / flattened
    ``TodoDisplayBlock`` items), and
  * the persisted session state (``todos`` + ``archived_todos``).

The C++ port (``src/builtin_tools/todo_tool.cpp``) is replayed over the same
arguments by ``tests/unit/builtin_tools/test_todo_tool.cpp`` and must reproduce
every byte.

Usage (any interpreter that can import ``kimi_cli``; the kimi-agent venv works)::

    python scripts/gen_todo_goldens.py            # rewrite the .inc file
    python scripts/gen_todo_goldens.py --check    # fail if it is out of date

``--python`` lets the caller point at the kimi-agent virtualenv interpreter
(``C:/dev/kimi-agent/.venv/Scripts/python.exe``) when the current interpreter
cannot import ``kimi_cli``.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
KIMI_AGENT_ROOT = Path(os.environ.get("KIMI_AGENT_ROOT", r"C:/dev/kimi-agent"))
DEFAULT_AGENT_PYTHON = KIMI_AGENT_ROOT / ".venv" / "Scripts" / "python.exe"
OUT_PATH = PROJECT_ROOT / "tests" / "unit" / "builtin_tools" / "todo_goldens.inc"


def _ensure_reference_on_path() -> None:
    for rel in ("kimi-cli/src", "src"):
        p = KIMI_AGENT_ROOT / rel
        if p.is_dir() and str(p) not in sys.path:
            sys.path.insert(0, str(p))


def _import_reference():
    _ensure_reference_on_path()
    from kimi_cli.session_state import (  # noqa: PLC0415
        SessionState,
        load_session_state,
        save_session_state,
    )
    from kimi_cli.tools.display import TodoDisplayBlock  # noqa: PLC0415
    from kimi_cli.tools.todo import (  # noqa: PLC0415
        Params,
        TodoList,
        TodoUpdateParams,
        todo_update,
    )
    from kosong.tooling.error import ToolValidateError  # noqa: PLC0415

    return {
        "SessionState": SessionState,
        "load_session_state": load_session_state,
        "save_session_state": save_session_state,
        "TodoDisplayBlock": TodoDisplayBlock,
        "Params": Params,
        "TodoList": TodoList,
        "TodoUpdateParams": TodoUpdateParams,
        "todo_update": todo_update,
        "ToolValidateError": ToolValidateError,
    }


# ---------------------------------------------------------------------------
# Stub runtime / session (mirrors kimi_cli.soul.agent.Runtime surface the tool
# touches: role, session, current_prompt, config.loop_control.todo_max_layers)
# ---------------------------------------------------------------------------


class _LoopControl:
    def __init__(self, todo_max_layers: int) -> None:
        self.todo_max_layers = todo_max_layers


class _Config:
    def __init__(self, todo_max_layers: int) -> None:
        self.loop_control = _LoopControl(todo_max_layers)


class _Session:
    def __init__(self, state_cls, save_fn, directory: Path) -> None:
        self.dir = directory
        self.state = state_cls()
        self._save_fn = save_fn

    def save_state(self) -> None:
        self._save_fn(self.state, self.dir)


class _Runtime:
    def __init__(self, session, max_layers: int, current_prompt: str | None = None) -> None:
        self.session = session
        self.role = "root"
        self.config = _Config(max_layers)
        self.current_prompt = current_prompt


# ---------------------------------------------------------------------------
# Corpus
# ---------------------------------------------------------------------------
# Every case is (name, max_layers, current_prompt, [ (tool, args), ... ]).
# "write" -> TodoList (todo_write), "update" -> todo_update.


def _deep_chain(prefix: str, n: int) -> dict:
    """Nested todo chain L1 -> L2 -> ... -> Ln (JSON dict)."""
    node: dict = {"content": f"{prefix}{n}", "status": "pending"}
    for i in range(n - 1, 0, -1):
        node = {"content": f"{prefix}{i}", "status": "pending", "children": [node]}
    return node


def corpus() -> list[tuple[str, int, str | None, list[tuple[str, dict]]]]:
    C: list[tuple[str, int, str | None, list[tuple[str, dict]]]] = []
    add = C.append

    # ---- read mode / empty state -----------------------------------------
    add(("read_empty", 4, None, [("write", {})]))
    add(("read_empty_after_archive", 4, None, [
        ("write", {"todos": [{"content": "Done A", "status": "done"}]}),
        ("write", {"todos": [], "mode": "clear"}),
        ("write", {}),
    ]))
    add(("write_then_read", 4, None, [
        ("write", {"todos": [
            {"content": "Analyze code", "status": "pending", "notes": ""},
            {"content": "Write tests", "status": "in_progress", "notes": ""},
            {"content": "Read requirements", "status": "done", "notes": ""},
        ]}),
        ("write", {}),
    ]))

    # ---- append / no-op ---------------------------------------------------
    add(("append_empty_is_noop", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "in_progress"}]}),
        ("write", {"todos": []}),
    ]))
    add(("append_empty_noop_on_empty_state", 4, None, [
        ("write", {"todos": []}),
    ]))
    add(("append_merge_status", 4, None, [
        ("write", {"todos": [
            {"content": "A", "status": "pending"},
            {"content": "B", "status": "in_progress"},
            {"content": "C", "status": "pending"},
        ]}),
        ("write", {"todos": [
            {"content": "B", "status": "done"},
            {"content": "C", "status": "in_progress"},
        ]}),
        ("write", {}),
    ]))
    add(("append_preserves_order", 4, None, [
        ("write", {"todos": [
            {"content": "First", "status": "pending"},
            {"content": "Second", "status": "pending"},
            {"content": "Third", "status": "pending"},
        ]}),
        ("write", {"todos": [
            {"content": "Third", "status": "done"},
            {"content": "First", "status": "done"},
        ]}),
        ("write", {}),
    ]))
    add(("append_new_title_appended", 4, None, [
        ("write", {"todos": [{"content": "Old task", "status": "pending"}]}),
        ("write", {"todos": [{"content": "New task", "status": "pending"}]}),
        ("write", {}),
    ]))
    add(("append_single_todo_object", 4, None, [
        ("write", {"todos": [
            {"content": "A", "status": "pending"},
            {"content": "B", "status": "pending"},
        ]}),
        ("write", {"todos": {"content": "B", "status": "done"}}),
        ("write", {}),
    ]))
    add(("append_explicit_mode", 4, None, [
        ("write", {"todos": [{"content": "Old task", "status": "pending"}]}),
        ("write", {"todos": [{"content": "Old task", "status": "done"}], "mode": "append"}),
        ("write", {}),
    ]))

    # ---- status aliases / merge notes ------------------------------------
    add(("status_synonyms", 4, None, [
        ("write", {"todos": [
            {"content": "A", "status": "completed"},
            {"content": "B", "status": "  IN-PROGRESS  "},
            {"content": "C", "status": "Done"},
        ]}),
        ("write", {}),
    ]))
    add(("item_aliases", 4, None, [
        ("write", {"todos": [
            {"task": "from task", "status": "pending"},
            {"title": "from title", "status": "pending"},
            {"todo": "from todo", "status": "pending"},
            {"item": "from item", "status": "pending"},
            {"name": "from name", "status": "pending"},
            {"content": "with description", "status": "pending", "description": "d"},
        ]}),
    ]))
    add(("merge_notes", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "in_progress", "notes": "old"}]}),
        ("write", {"todos": [{"content": "A", "status": "in_progress"}]}),
        ("write", {"todos": [{"content": "A", "status": "in_progress", "notes": "   "}]}),
        ("write", {"todos": [{"content": "A", "status": "in_progress", "notes": "new"}]}),
        ("write", {}),
    ]))
    add(("merge_children_kept", 4, None, [
        ("write", {"todos": [
            {"content": "P", "status": "pending",
             "children": [{"content": "c", "status": "pending"}]},
        ]}),
        ("write", {"todos": [{"content": "P", "status": "in_progress"}]}),
        ("write", {"todos": [{"content": "P", "status": "in_progress", "children": []}]}),
        ("write", {}),
    ]))

    # ---- duplicate / limits ----------------------------------------------
    add(("duplicate_titles", 4, None, [
        ("write", {"todos": [
            {"content": "Task A", "status": "pending"},
            {"content": "Task B", "status": "in_progress"},
            {"content": "Task A", "status": "done"},
        ]}),
    ]))
    add(("duplicate_titles_sorted", 4, None, [
        ("write", {"todos": [
            {"content": "zeta", "status": "pending"},
            {"content": "alpha", "status": "pending"},
            {"content": "zeta", "status": "pending"},
            {"content": "alpha", "status": "pending"},
        ]}),
    ]))

    # ---- replace / clear guards ------------------------------------------
    add(("replace_guard_error", 4, None, [
        ("write", {"todos": [{"content": "Task A", "status": "pending"}]}),
        ("write", {"todos": [{"content": "New task", "status": "pending"}], "mode": "replace"}),
        ("write", {}),
    ]))
    add(("replace_all_done_archives", 4, None, [
        ("write", {"todos": [
            {"content": "Done A", "status": "done"},
            {"content": "Done B", "status": "done"},
        ]}),
        ("write", {"todos": [{"content": "Fresh", "status": "pending"}], "mode": "replace"}),
        ("write", {}),
    ]))
    add(("replace_force_archives_only_dropped", 4, None, [
        ("write", {"todos": [
            {"content": "Done kept", "status": "done"},
            {"content": "Done dropped", "status": "done"},
            {"content": "Pending dropped", "status": "pending"},
        ]}),
        ("write", {"todos": [{"content": "Done kept", "status": "pending"}],
                   "mode": "replace", "force": True}),
        ("write", {}),
    ]))
    add(("replace_empty_list_force", 4, None, [
        ("write", {"todos": [{"content": "Task A", "status": "pending"}]}),
        ("write", {"todos": [], "mode": "replace", "force": True}),
        ("write", {}),
    ]))
    add(("replace_force_empty_old_no_warning", 4, None, [
        ("write", {"todos": [{"content": "New task", "status": "pending"}],
                   "mode": "replace", "force": True}),
    ]))
    add(("clear_guard_error", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "pending"}]}),
        ("write", {"todos": [], "mode": "clear"}),
        ("write", {}),
    ]))
    add(("clear_all_done_ok", 4, None, [
        ("write", {"todos": [
            {"content": "D1", "status": "done"},
            {"content": "D2", "status": "done"},
        ]}),
        ("write", {"todos": [], "mode": "clear"}),
        ("write", {}),
    ]))
    add(("clear_force_discards", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "pending"}]}),
        ("write", {"todos": [], "mode": "clear", "force": True}),
        ("write", {}),
    ]))
    add(("clear_with_todos_error", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "pending"}]}),
        ("write", {"todos": [{"content": "B", "status": "pending"}], "mode": "clear"}),
    ]))
    add(("clear_empty_state", 4, None, [
        ("write", {"todos": [], "mode": "clear"}),
    ]))

    # ---- legacy mode spellings -------------------------------------------
    add(("legacy_modes", 4, None, [
        ("write", {"todos": [{"content": "Old task", "status": "pending"}]}),
        ("write", {"todos": [{"content": "New task", "status": "done"}], "mode": "overwrite"}),
        ("write", {"todos": [{"content": "New task", "status": "done"}],
                   "mode": "force_overwrite"}),
        ("write", {}),
    ]))
    add(("legacy_force_spellings", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "pending"}]}),
        ("write", {"todos": [{"content": "B", "status": "pending"}], "mode": "Force Override"}),
    ]))
    add(("invalid_mode", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "pending"}], "mode": "bogus"}),
    ]))

    # ---- regression guard -------------------------------------------------
    add(("regression_guard", 4, None, [
        ("write", {"todos": [
            {"content": "A", "status": "pending"},
            {"content": "B", "status": "done"},
        ]}),
        ("write", {"todos": [
            {"content": "A", "status": "done"},
            {"content": "B", "status": "pending"},
        ]}),
        ("write", {}),
        ("write", {"todos": [
            {"content": "A", "status": "done"},
            {"content": "B", "status": "pending"},
        ], "force": True}),
        ("write", {}),
    ]))
    add(("regression_replace_mode", 4, None, [
        ("write", {"todos": [{"content": "B", "status": "done"}]}),
        ("write", {"todos": [{"content": "B", "status": "pending"}],
                   "mode": "replace", "force": True}),
        ("write", {}),
    ]))

    # ---- single in_progress ----------------------------------------------
    add(("multi_in_progress_error", 4, None, [
        ("write", {"todos": [
            {"content": "A", "status": "in_progress"},
            {"content": "B", "status": "in_progress"},
            {"content": "C", "status": "in_progress"},
        ], "auto_fix": False}),
    ]))
    add(("auto_fix_keeps_last", 4, None, [
        ("write", {"todos": [
            {"content": "A", "status": "in_progress"},
            {"content": "B", "status": "in_progress"},
        ]}),
        ("write", {}),
    ]))
    add(("auto_fix_demotes_child", 4, None, [
        ("write", {"todos": [
            {"content": "Parent", "status": "in_progress",
             "children": [{"content": "Child", "status": "in_progress"}]},
            {"content": "Sibling", "status": "in_progress"},
        ]}),
        ("write", {}),
    ]))
    add(("auto_fix_phase_log", 4, None, [
        ("write", {"todos": [
            {"content": "Phase 1", "status": "done"},
            {"content": "Phase 2", "status": "in_progress"},
            {"content": "Phase 3", "status": "done"},
            {"content": "Phase 4", "status": "pending"},
            {"content": "Phase 5", "status": "pending"},
            {"content": "Phase 6", "status": "pending"},
        ], "mode": "replace", "force": True}),
        ("write", {"todos": [
            {"content": "Phase 1", "status": "done"},
            {"content": "Phase 2", "status": "in_progress"},
            {"content": "Phase 3", "status": "done"},
            {"content": "Phase 4", "status": "in_progress"},
            {"content": "Phase 5", "status": "pending"},
            {"content": "Phase 6", "status": "pending"},
        ]}),
        ("write", {"todos": [
            {"content": "Phase 1", "status": "done"},
            {"content": "Phase 2", "status": "done"},
            {"content": "Phase 3", "status": "done"},
            {"content": "Phase 4", "status": "in_progress"},
            {"content": "Phase 5", "status": "pending"},
            {"content": "Phase 6", "status": "pending"},
        ]}),
        ("write", {}),
    ]))
    add(("force_bypasses_single_in_progress", 4, None, [
        ("write", {"todos": [
            {"content": "X", "status": "in_progress"},
            {"content": "Y", "status": "in_progress"},
        ], "mode": "replace", "force": True}),
    ]))

    # ---- all-done reminder / prompt --------------------------------------
    add(("all_done_reminder", 4, None, [
        ("write", {"todos": [{"content": "Only done", "status": "done"}]}),
        ("write", {}),
    ]))
    add(("all_done_with_prompt", 4, "Short user prompt", [
        ("write", {"todos": [{"content": "Only done", "status": "done"}]}),
        ("write", {}),
    ]))
    add(("all_done_long_prompt", 4, "L" * 150 + "M" * 100 + "R" * 60, [
        ("write", {"todos": [{"content": "Only done", "status": "done"}]}),
        ("write", {}),
    ]))
    add(("not_all_done_message", 4, None, [
        ("write", {"todos": [
            {"content": "Pending task", "status": "pending"},
            {"content": "Done task", "status": "done"},
        ]}),
        ("write", {}),
    ]))

    # ---- read truncation --------------------------------------------------
    add(("read_truncates_150", 4, None, [
        ("write", {"todos": [{"content": f"Task {i:03d}", "status": "pending"}
                             for i in range(150)], "mode": "replace", "force": True}),
        ("write", {}),
    ]))
    add(("read_exactly_100", 4, None, [
        ("write", {"todos": [{"content": f"Task {i:03d}", "status": "pending"}
                             for i in range(100)], "mode": "replace", "force": True}),
        ("write", {}),
    ]))

    # ---- archive cap ------------------------------------------------------
    add(("archive_capped_500", 4, None, [
        ("write", {"todos": [{"content": f"Old {i}", "status": "done"}
                             for i in range(550)], "mode": "replace", "force": True}),
        ("write", {"todos": [{"content": "Fresh", "status": "pending"}], "mode": "replace"}),
        ("write", {}),
    ]))

    # ---- fuzzy warnings ---------------------------------------------------
    add(("fuzzy_append_warning", 4, None, [
        ("write", {"todos": [{"content": "Implement feature", "status": "pending"}]}),
        ("write", {"todos": [{"content": "Implement featuer", "status": "pending"}]}),
    ]))
    add(("fuzzy_word_reorder_warning", 4, None, [
        ("write", {"todos": [{"content": "Fix bug", "status": "pending"}]}),
        ("write", {"todos": [{"content": "Bug fix", "status": "pending"}]}),
    ]))
    add(("fuzzy_case_only_warning", 4, None, [
        ("write", {"todos": [{"content": "Implement Feature", "status": "pending"}]}),
        ("write", {"todos": [{"content": "implement feature", "status": "pending"}]}),
    ]))
    add(("fuzzy_unrelated_no_warning", 4, None, [
        ("write", {"todos": [{"content": "Alpha", "status": "pending"}]}),
        ("write", {"todos": [{"content": "zzzz qqqq", "status": "pending"}]}),
    ]))
    add(("scope_duplicate_warning", 4, None, [
        ("update", {"parent": "", "title": "Parent"}),
        ("update", {"parent": "Parent", "title": "child"}),
        ("write", {"todos": [{"content": "child", "status": "done"}]}),
        ("write", {}),
    ]))
    add(("scope_duplicate_deep_warning", 4, None, [
        ("update", {"parent": "", "title": "A"}),
        ("update", {"parent": "A", "title": "B"}),
        ("update", {"parent": "B", "title": "deep"}),
        ("write", {"todos": [{"content": "deep", "status": "done"}]}),
    ]))

    # ---- depth cap --------------------------------------------------------
    add(("depth_cap_5_ok_6_rejected", 4, None, [
        ("write", {"todos": [_deep_chain("L", 5)]}),
        ("write", {"todos": [_deep_chain("L", 6)]}),
        ("write", {}),
    ]))
    add(("depth_cap_layers_1", 1, None, [
        ("write", {"todos": [_deep_chain("L", 3)]}),
    ]))
    add(("depth_cap_replace_force", 4, None, [
        ("write", {"todos": [_deep_chain("L", 6)], "mode": "replace", "force": True}),
    ]))

    # ---- JSON-string todos ------------------------------------------------
    add(("todos_json_string_array", 4, None, [
        ("write", {"mode": "overwrite",
                   "todos": '[{"title": "Build DXC", "status": "in_progress", '
                            '"priority": "high"}]'}),
        ("write", {}),
    ]))
    add(("todos_json_string_object", 4, None, [
        ("write", {"mode": "overwrite",
                   "todos": '{"title": "Build DXC", "status": "in_progress"}'}),
    ]))
    add(("todos_json_string_repairable", 4, None, [
        ("write", {"mode": "overwrite",
                   "todos": '[{"title": "Build DXC", "status": "in_progress"'}),
    ]))
    add(("todos_json_string_invalid", 4, None, [
        ("write", {"mode": "overwrite", "todos": "[1, 2, 3]"}),
    ]))
    add(("todos_plain_string", 4, None, [
        ("write", {"mode": "overwrite", "todos": "just a plain title"}),
    ]))

    # ---- todo_update: basics ---------------------------------------------
    add(("update_status", 4, None, [
        ("write", {"todos": [{"content": "Task A", "status": "in_progress"}]}),
        ("update", {"title": "Task A", "status": "done"}),
        ("write", {}),
    ]))
    add(("update_notes_replace_and_clear", 4, None, [
        ("write", {"todos": [{"content": "Task A", "status": "pending", "notes": "old"}]}),
        ("update", {"title": "Task A", "notes": "new note"}),
        ("update", {"title": "Task A"}),
        ("update", {"title": "Task A", "notes": ""}),
        ("update", {"title": "Task A", "notes": "   "}),
        ("write", {}),
    ]))
    add(("update_bare_same_title_preserves", 4, None, [
        ("write", {"todos": [{"content": "Parent", "status": "pending"}]}),
        ("update", {"parent": "Parent", "title": "child", "status": "in_progress",
                    "notes": "keep"}),
        ("update", {"parent": "Parent", "title": "child"}),
        ("update", {"parent": "Parent", "title": "child", "status": "pending"}),
        ("write", {}),
    ]))
    add(("update_no_todos_error", 4, None, [
        ("update", {"title": "Task A", "status": "done"}),
    ]))
    add(("update_missing_title_error", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "pending"}]}),
        ("update", {}),
    ]))
    add(("update_rename", 4, None, [
        ("write", {"todos": [{"content": "Old", "status": "pending"}]}),
        ("update", {"title": "Old", "rename_to": "New"}),
        ("write", {}),
    ]))
    add(("update_rename_collision", 4, None, [
        ("write", {"todos": [
            {"content": "A", "status": "pending"},
            {"content": "B", "status": "pending"},
        ]}),
        ("update", {"title": "A", "rename_to": "B"}),
        ("write", {}),
    ]))
    add(("update_rename_done_item", 4, None, [
        ("write", {"todos": [{"content": "Parent", "status": "pending"}]}),
        ("update", {"parent": "Parent", "title": "old", "status": "done"}),
        ("update", {"parent": "Parent", "title": "old", "rename_to": "new"}),
        ("write", {}),
    ]))
    add(("update_rename_to_self", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "pending"}]}),
        ("update", {"title": "A", "rename_to": "A"}),
    ]))

    # ---- todo_update: fuzzy ----------------------------------------------
    add(("update_fuzzy_match", 4, None, [
        ("write", {"todos": [{"content": "Implement feature", "status": "pending"}]}),
        ("update", {"title": "implement feature", "status": "done"}),
    ]))
    add(("update_fuzzy_disabled", 4, None, [
        ("write", {"todos": [{"content": "Task A", "status": "pending"}]}),
        ("update", {"title": "task a", "status": "done", "fuzzy": False}),
    ]))
    add(("update_fuzzy_no_match", 4, None, [
        ("write", {"todos": [{"content": "Task A", "status": "pending"}]}),
        ("update", {"title": "totally unrelated phrasing", "status": "done"}),
    ]))
    add(("update_nested_by_title", 4, None, [
        ("write", {"todos": [
            {"content": "Parent", "status": "pending",
             "children": [{"content": "Child", "status": "pending"}]},
        ]}),
        ("update", {"title": "Child", "status": "done"}),
        ("write", {}),
    ]))

    # ---- todo_update: regression -----------------------------------------
    add(("update_regression_guard", 4, None, [
        ("write", {"todos": [{"content": "Task A", "status": "done"}]}),
        ("update", {"title": "Task A", "status": "in_progress"}),
        ("write", {}),
        ("update", {"title": "Task A", "status": "in_progress", "force": True}),
        ("write", {}),
    ]))

    # ---- todo_update: complete -------------------------------------------
    add(("update_complete_subtree", 4, None, [
        ("write", {"todos": [
            {"content": "Parent", "status": "pending", "children": [
                {"content": "c1", "status": "pending"},
                {"content": "c2", "status": "in_progress",
                 "children": [{"content": "g", "status": "pending"}]},
            ]},
        ]}),
        ("update", {"title": "Parent", "complete": True}),
        ("write", {}),
    ]))
    add(("update_complete_single", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "in_progress"}]}),
        ("update", {"title": "A", "complete": True}),
    ]))
    add(("update_complete_with_status_done", 4, None, [
        ("write", {"todos": [
            {"content": "P", "status": "pending",
             "children": [{"content": "c", "status": "pending"}]},
        ]}),
        ("update", {"title": "P", "status": "done", "complete": True}),
    ]))
    add(("update_complete_pending_error", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "pending"}]}),
        ("update", {"title": "A", "status": "pending", "complete": True}),
        ("write", {}),
    ]))
    add(("update_complete_missing_error", 4, None, [
        ("write", {"todos": [{"content": "P", "status": "pending"}]}),
        ("update", {"parent": "P", "title": "ghost", "complete": True}),
        ("write", {}),
    ]))
    add(("update_rename_missing_child_error", 4, None, [
        ("write", {"todos": [{"content": "P", "status": "pending"}]}),
        ("update", {"parent": "P", "title": "ghost", "rename_to": "other"}),
    ]))

    # ---- todo_update: parent scope ---------------------------------------
    add(("update_create_child", 4, None, [
        ("write", {"todos": [{"content": "Parent", "status": "pending"}]}),
        ("update", {"parent": "Parent", "title": "Child"}),
        ("update", {"parent": "Parent", "title": "Child", "status": "done"}),
        ("write", {}),
    ]))
    add(("update_create_root_empty_parent", 4, None, [
        ("write", {"todos": [{"content": "Existing", "status": "pending"}]}),
        ("update", {"parent": "", "title": "New Root"}),
        ("write", {}),
    ]))
    add(("update_create_root_on_empty_state", 4, None, [
        ("update", {"parent": "", "title": "A"}),
        ("update", {"parent": "", "title": "B", "status": "in_progress"}),
        ("write", {}),
    ]))
    add(("update_missing_parent", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "pending"}]}),
        ("update", {"parent": "Missing", "title": "Child"}),
    ]))
    add(("update_missing_parent_nofuzzy", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "pending"}]}),
        ("update", {"parent": "Aaa", "title": "Child", "fuzzy": False}),
    ]))
    add(("update_parent_scope_isolation", 4, None, [
        ("write", {"todos": [
            {"content": "P1", "status": "pending",
             "children": [{"content": "Child", "status": "pending"}]},
            {"content": "P2", "status": "pending",
             "children": [{"content": "Child", "status": "pending"}]},
        ]}),
        ("update", {"parent": "P2", "title": "Child", "status": "done"}),
        ("write", {}),
    ]))
    add(("update_parent_fuzzy", 4, None, [
        ("write", {"todos": [{"content": "Implement feature", "status": "pending"}]}),
        ("update", {"parent": "implement feature", "title": "child"}),
    ]))

    # ---- todo_update: batches and string forms ---------------------------
    add(("update_batch_basic", 4, None, [
        ("write", {"todos": [
            {"content": "A", "status": "pending"},
            {"content": "B", "status": "pending"},
        ]}),
        ("update", {"updates": [
            {"title": "A", "status": "done"},
            {"title": "B", "status": "in_progress"},
        ]}),
        ("write", {}),
    ]))
    add(("update_batch_autofix", 4, None, [
        ("write", {"todos": [
            {"content": "A", "status": "pending"},
            {"content": "B", "status": "pending"},
        ]}),
        ("update", {"updates": [
            {"title": "A", "status": "in_progress"},
            {"title": "B", "status": "in_progress"},
        ]}),
        ("write", {}),
    ]))
    add(("update_batch_error_keeps_state", 4, None, [
        ("write", {"todos": [
            {"content": "A", "status": "pending"},
            {"content": "B", "status": "done"},
        ]}),
        ("update", {"updates": [
            {"title": "A", "status": "done"},
            {"title": "B", "status": "in_progress"},
        ]}),
        ("write", {}),
    ]))
    add(("update_batch_common_parent", 4, None, [
        ("write", {"todos": [{"content": "Parent", "status": "pending"}]}),
        ("update", {"parent": "Parent", "updates": [
            {"title": "Child1"},
            {"title": "Child2", "status": "in_progress"},
        ]}),
        ("write", {}),
    ]))
    add(("update_batch_rename_then_child", 4, None, [
        ("write", {"todos": [
            {"content": "Parent", "status": "pending",
             "children": [{"content": "Child", "status": "pending"}]},
        ]}),
        ("update", {"updates": [
            {"title": "Parent", "rename_to": "NewParent"},
            {"parent": "NewParent", "title": "Child", "status": "done"},
        ]}),
        ("write", {}),
    ]))
    add(("update_batch_todos_alias", 4, None, [
        ("write", {"todos": [
            {"content": "A", "status": "pending"},
            {"content": "B", "status": "pending"},
        ]}),
        ("update", {"todos": [
            {"title": "A", "status": "done"},
            {"title": "B", "status": "done"},
        ]}),
    ]))
    add(("update_batch_content_shape", 4, None, [
        ("write", {"todos": [
            {"content": "A", "status": "pending"},
            {"content": "B", "status": "pending"},
        ]}),
        ("update", {"updates": [
            {"content": "A", "status": "done"},
            {"content": "B", "status": "in_progress"},
        ]}),
    ]))
    add(("update_batch_bare_titles", 4, None, [
        ("write", {"todos": [
            {"content": "A", "status": "pending"},
            {"content": "B", "status": "pending"},
        ]}),
        ("update", {"updates": [{"title": "A", "status": "done"}, "B"]}),
    ]))
    add(("update_batch_single_dict", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "pending"}]}),
        ("update", {"updates": {"title": "A", "status": "done"}}),
    ]))
    add(("update_batch_json_string", 4, None, [
        ("write", {"todos": [
            {"content": "A", "status": "pending"},
            {"content": "B", "status": "pending"},
        ]}),
        ("update", {"updates": '[{"title": "A", "status": "done"}, '
                               '{"title": "B", "status": "in_progress"}]'}),
    ]))
    add(("update_batch_broken_json_string", 4, None, [
        ("write", {"todos": [
            {"content": "A", "status": "pending"},
            {"content": "B", "status": "pending"},
        ]}),
        ("update", {"updates": '[{"title": "A", "status": "done",}, {"title": "B",},]'}),
    ]))
    add(("update_batch_single_json_dict_string", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "pending"}]}),
        ("update", {"updates": '{"title": "A", "status": "done"}'}),
    ]))
    add(("update_bare_title_string", 4, None, [
        ("write", {"todos": [{"content": "Fix the bug", "status": "pending"}]}),
        ("update", {"updates": "Fix the bug"}),
    ]))
    add(("update_mixed_fields_rejected", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "pending"}]}),
        ("update", {"title": "A", "updates": [{"title": "B"}]}),
    ]))
    add(("update_batch_create_root_children", 4, None, [
        ("update", {"parent": "", "updates": [{"title": "A"}, {"title": "B"}]}),
        ("write", {}),
    ]))

    # ---- todo_update: content alias / display ----------------------------
    add(("update_content_alias", 4, None, [
        ("write", {"todos": [{"content": "Task A", "status": "in_progress"}]}),
        ("update", {"content": "Task A", "status": "done"}),
        ("update", {"content": "Task A", "notes": "from content-shape"}),
        ("write", {}),
    ]))
    add(("update_alias_task_todo_edits", 4, None, [
        ("write", {"todos": [{"content": "Task A", "status": "pending"}]}),
        ("update", {"task": "Task A", "status": "done"}),
        ("update", {"todo": "Task A", "status": "in_progress"}),
        ("update", {"edits": [{"title": "Task A", "status": "done"}]}),
    ]))

    # ---- todo_update: depth guard ----------------------------------------
    add(("update_depth_guard_layers_0", 0, None, [
        ("write", {"todos": [{"content": "A", "status": "pending"}]}),
        ("update", {"parent": "A", "title": "child"}),
    ]))
    add(("update_depth_guard_at_limit", 4, None, [
        ("write", {"todos": [_deep_chain("L", 4)]}),
        ("update", {"parent": "L4", "title": "leaf"}),
        ("write", {}),
    ]))

    # ---- notes rendering --------------------------------------------------
    add(("read_in_progress_notes", 4, None, [
        ("write", {"todos": [
            {"content": "Active task", "status": "in_progress", "notes": "Working on tests"},
            {"content": "Idle task", "status": "pending", "notes": "Not shown"},
        ]}),
        ("write", {}),
    ]))
    add(("write_summary_order", 4, None, [
        ("write", {"todos": [
            {"content": "First", "status": "pending"},
            {"content": "Second", "status": "in_progress"},
            {"content": "Third", "status": "pending"},
            {"content": "Fourth", "status": "done"},
            {"content": "Fifth", "status": "pending"},
        ]}),
    ]))

    # ---- tree rendering ---------------------------------------------------
    add(("read_tree_indent", 4, None, [
        ("write", {"todos": [
            {"content": "Parent", "status": "pending", "children": [
                {"content": "Child A", "status": "pending"},
                {"content": "Child B", "status": "done"},
            ]},
        ]}),
        ("write", {}),
    ]))
    add(("read_tree_grandchild", 4, None, [
        ("write", {"todos": [
            {"content": "P1", "status": "pending", "children": [
                {"content": "P2", "status": "pending", "children": [
                    {"content": "grandchild", "status": "pending"},
                ]},
            ]},
        ]}),
        ("write", {}),
    ]))

    # ---- second batch: aliases, limits, nesting, unicode ------------------

    # argument-name aliases (kosong _repair_dict_for_model / FIELD_ALIASES_TODO)
    add(("alias_items_for_todos", 4, None, [
        ("write", {"items": [{"content": "A", "status": "pending"}]}),
        ("write", {}),
    ]))
    add(("alias_list_tasks_entries", 4, None, [
        ("write", {"tasks": [{"content": "A", "status": "pending"}]}),
        ("write", {"entries": [{"content": "B", "status": "pending"}]}),
        ("write", {"list": [{"content": "C", "status": "pending"}]}),
        ("write", {"todo_list": [{"content": "D", "status": "pending"}]}),
        ("write", {"task_list": [{"content": "E", "status": "pending"}]}),
    ]))
    add(("alias_canonical_wins", 4, None, [
        ("write", {"todos": [{"content": "canonical", "status": "pending"}],
                   "items": [{"content": "alias", "status": "pending"}]}),
    ]))
    add(("mode_bool_keys", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "done"}]}),
        ("write", {"todos": [{"content": "B", "status": "done"}], "replace": True}),
    ]))
    add(("mode_uppercase", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "done"}]}),
        ("write", {"todos": [{"content": "B", "status": "done"}], "mode": "REPLACE"}),
    ]))
    add(("autofix_alias", 4, None, [
        ("write", {"todos": [
            {"content": "A", "status": "in_progress"},
            {"content": "B", "status": "in_progress"},
        ], "autofix": False}),
    ]))
    add(("auto_fix_false_with_force", 4, None, [
        ("write", {"todos": [
            {"content": "X", "status": "in_progress"},
            {"content": "Y", "status": "in_progress"},
            {"content": "Z", "status": "in_progress"},
        ], "mode": "replace", "force": True, "auto_fix": False}),
    ]))
    add(("auto_fix_false_replace_mode", 4, None, [
        ("write", {"todos": [{"content": "old", "status": "done"}]}),
        ("write", {"todos": [
            {"content": "X", "status": "in_progress"},
            {"content": "Y", "status": "in_progress"},
        ], "mode": "replace", "auto_fix": False}),
    ]))
    add(("title_whitespace_stripped", 4, None, [
        ("write", {"todos": [
            {"content": "  padded title  ", "status": "pending"},
            {"content": "\ttabbed\n", "status": "pending"},
        ]}),
        ("write", {}),
    ]))
    add(("single_object_with_children", 4, None, [
        ("write", {"todos": {"content": "root", "status": "in_progress",
                             "children": [{"content": "kid", "status": "pending"}]}}),
        ("write", {}),
    ]))
    add(("limit_4097_rejected", 4, None, [
        ("write", {"todos": [{"content": f"t{i}", "status": "pending"}
                             for i in range(4097)]}),
        ("write", {}),
    ]))

    # JSON-string forms that are NOT JSON (_looks_like_json == '[', '{')
    add(("updates_numeric_string", 4, None, [
        ("write", {"todos": [{"content": "123", "status": "pending"}]}),
        ("update", {"updates": "123"}),
    ]))
    add(("updates_whitespace_string", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "pending"}]}),
        ("update", {"updates": "   "}),
    ]))
    add(("updates_json_scalar_string", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "pending"}]}),
        ("update", {"updates": "[1, 2, 3]"}),
    ]))
    add(("todos_numeric_string", 4, None, [
        ("write", {"todos": "123"}),
    ]))

    # todo_update: empty batch / no-change batches
    add(("update_empty_batch", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "pending"}]}),
        ("update", {"updates": []}),
    ]))
    add(("update_no_change_batch", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "in_progress"}]}),
        ("update", {"updates": [{"title": "A"}]}),
    ]))

    # todo_update: nested rename / grandchild edits
    add(("update_rename_child_in_parent", 4, None, [
        ("write", {"todos": [
            {"content": "P", "status": "pending", "children": [
                {"content": "c1", "status": "pending"},
                {"content": "c2", "status": "pending"},
            ]},
        ]}),
        ("update", {"parent": "P", "title": "c1", "rename_to": "c3"}),
        ("write", {}),
    ]))
    add(("update_rename_child_collision", 4, None, [
        ("write", {"todos": [
            {"content": "P", "status": "pending", "children": [
                {"content": "c1", "status": "pending"},
                {"content": "c2", "status": "pending"},
            ]},
        ]}),
        ("update", {"parent": "P", "title": "c1", "rename_to": "c2"}),
        ("write", {}),
    ]))
    add(("update_rename_child_to_root_title_ok", 4, None, [
        ("write", {"todos": [
            {"content": "P", "status": "pending", "children": [
                {"content": "c1", "status": "pending"},
            ]},
            {"content": "other", "status": "pending"},
        ]}),
        ("update", {"parent": "P", "title": "c1", "rename_to": "other"}),
    ]))
    add(("update_grandchild_by_title", 4, None, [
        ("write", {"todos": [
            {"content": "A", "status": "pending", "children": [
                {"content": "B", "status": "pending", "children": [
                    {"content": "C", "status": "in_progress", "notes": "deep"},
                ]},
            ]},
        ]}),
        ("update", {"title": "C", "status": "done"}),
        ("write", {}),
    ]))
    add(("update_child_notes", 4, None, [
        ("write", {"todos": [{"content": "P", "status": "pending"}]}),
        ("update", {"parent": "P", "title": "c", "notes": "  spaced  "}),
        ("write", {}),
    ]))
    add(("update_complete_done_parent", 4, None, [
        ("write", {"todos": [
            {"content": "P", "status": "done", "children": [
                {"content": "c", "status": "pending"},
            ]},
        ]}),
        ("update", {"title": "P", "complete": True}),
        ("write", {}),
    ]))
    add(("update_complete_deep_tree", 4, None, [
        ("write", {"todos": [
            {"content": "A", "status": "pending", "children": [
                {"content": "B", "status": "pending", "children": [
                    {"content": "C", "status": "pending", "children": [
                        {"content": "D", "status": "pending"},
                    ]},
                ]},
            ]},
            {"content": "sibling", "status": "pending"},
        ]}),
        ("update", {"title": "A", "complete": True}),
        ("write", {}),
    ]))

    # archived + active list rendering
    add(("read_archived_with_active", 4, None, [
        ("write", {"todos": [
            {"content": "D1", "status": "done"},
            {"content": "keep", "status": "done"},
        ], "mode": "replace", "force": True}),
        ("write", {"todos": [{"content": "Active", "status": "pending"}],
                   "mode": "replace"}),
        ("write", {}),
    ]))

    # read truncation over a TREE (DFS line cap)
    add(("read_tree_truncates_120_lines", 4, None, [
        ("write", {"todos": [{"content": f"R{i:02d}", "status": "pending", "children": [
            {"content": f"C{i:02d}", "status": "pending"},
        ]} for i in range(60)], "mode": "replace", "force": True}),
        ("write", {}),
        ("update", {"title": "C59", "status": "done"}),
    ]))

    # notes with embedded newlines / update status synonyms / ambiguity
    add(("notes_multiline", 4, None, [
        ("write", {"todos": [
            {"content": "A", "status": "in_progress", "notes": "line1\nline2"},
            {"content": "B", "status": "pending", "notes": None},
        ]}),
        ("write", {}),
    ]))
    add(("update_status_synonyms", 4, None, [
        ("write", {"todos": [
            {"content": "A", "status": "pending"},
            {"content": "B", "status": "pending"},
            {"content": "C", "status": "pending"},
        ]}),
        ("update", {"title": "A", "status": "completed"}),
        ("update", {"title": "B", "status": "  DONE  "}),
        ("update", {"title": "C", "status": "in-progress"}),
        ("update", {"title": "B", "status": "in_progress", "fuzzy": True}),
        ("write", {}),
    ]))
    add(("update_fuzzy_ambiguous", 4, None, [
        ("write", {"todos": [
            {"content": "Fix the login bug", "status": "pending"},
            {"content": "Fix the logout bug", "status": "pending"},
        ]}),
        ("update", {"title": "Fix the login bugs", "status": "done"}),
    ]))
    add(("global_duplicate_title_first_wins", 4, None, [
        ("write", {"todos": [
            {"content": "A", "status": "pending", "children": [
                {"content": "A", "status": "pending"},
            ]},
        ]}),
        ("update", {"title": "A", "notes": "root"}),
        ("write", {}),
    ]))
    add(("update_fuzzy_parent_ambiguous", 4, None, [
        ("write", {"todos": [
            {"content": "Parent one", "status": "pending"},
            {"content": "Parent two", "status": "pending"},
        ]}),
        ("update", {"parent": "parent one", "title": "kid"}),
        ("write", {}),
    ]))

    # todos == null / absent -> read mode, whatever `mode` says
    add(("todos_null_reads", 4, None, [
        ("write", {"todos": [{"content": "A", "status": "pending"}]}),
        ("write", {"todos": None}),
        ("write", {"todos": None, "mode": "clear"}),
        ("write", {"mode": "clear"}),
        ("write", {"mode": "replace"}),
    ]))

    # unicode prompt truncation (code points, not bytes)
    add(("unicode_long_prompt", 4, "héllo wörld — 中文テスト " * 12, [
        ("write", {"todos": [{"content": "done item", "status": "done"}]}),
        ("write", {}),
    ]))

    # unicode titles (exact matching only; UTF-8 round-trip + rendering)
    add(("unicode_titles", 4, None, [
        ("write", {"todos": [
            {"content": "修复登录 bug", "status": "in_progress", "notes": "检查日志"},
            {"content": "Phase 1 — Baseline", "status": "done"},
            {"content": "café — “naïve”", "status": "pending"},
        ]}),
        ("write", {}),
        ("update", {"title": "修复登录 bug", "status": "done"}),
        ("write", {}),
    ]))
    add(("unicode_scope_duplicate_warning", 4, None, [
        ("update", {"parent": "", "title": "父任务"}),
        ("update", {"parent": "父任务", "title": "子任务"}),
        ("write", {"todos": [{"content": "子任务", "status": "done"}]}),
    ]))

    return C


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------


def _todo_display(result) -> list[dict]:
    """Flatten the TodoDisplayBlock items of a ToolReturnValue (other display
    blocks -- e.g. the ToolError brief -- have no C++ counterpart)."""
    ref = _import_reference()
    block_cls = ref["TodoDisplayBlock"]
    out: list[dict] = []
    for block in result.display:
        if isinstance(block, block_cls):
            for item in block.items:
                out.append({
                    "title": item.title,
                    "status": item.status,
                    "notes": item.notes,
                    "depth": item.depth,
                })
    return out


def _state_snapshot(load_session_state, directory: Path) -> dict:
    state = load_session_state(directory)
    return {
        "todos": [t.model_dump() for t in state.todos],
        "archived_todos": [t.model_dump() for t in state.archived_todos],
    }


async def _run_corpus() -> list[dict]:
    ref = _import_reference()
    TodoList = ref["TodoList"]
    todo_update_cls = ref["todo_update"]
    SessionState = ref["SessionState"]
    load_session_state = ref["load_session_state"]
    save_session_state = ref["save_session_state"]
    ToolValidateError = ref["ToolValidateError"]

    rows: list[dict] = []
    for name, max_layers, prompt, steps in corpus():
        with tempfile.TemporaryDirectory(prefix="todo_golden_") as td:
            session = _Session(SessionState, save_session_state, Path(td))
            runtime = _Runtime(session, max_layers, prompt)
            write_tool = TodoList(runtime)
            update_tool = todo_update_cls(runtime)
            for idx, (tool, args) in enumerate(steps):
                # Drive the *toolset* entry point (CallableTool2.call) so the
                # kosong argument repair (_repair_dict_for_model + per-tool
                # field_aliases) runs exactly as it does in production: the C++
                # port folds that repair into its own alias tables.
                impl = write_tool if tool == "write" else update_tool
                result = await impl.call(args)
                validate_error = isinstance(result, ToolValidateError)
                rows.append({
                    "case": name, "step": idx, "tool": tool,
                    "max_layers": max_layers, "args": args,
                    "prompt": prompt or "",
                    "validation_error": validate_error,
                    "is_error": bool(result.is_error),
                    "output": "" if validate_error else (
                        result.output if isinstance(result.output, str)
                        else json.dumps(result.output)),
                    "message": "" if validate_error else result.message,
                    "display": [] if validate_error else _todo_display(result),
                    "state": _state_snapshot(load_session_state, Path(td)),
                })
    return rows


# ---------------------------------------------------------------------------
# Emit
# ---------------------------------------------------------------------------


def c_literal(text: str, chunk: int = 1000) -> str:
    """Emit one or more adjacent C++ narrow string literals for UTF-8 `text`.

    Byte-exact and ASCII-safe (non-ASCII bytes become ``\\xNN`` escapes, split
    with ``""`` when the next character is a hex digit). The literal is broken
    into ``chunk``-sized pieces joined by a newline so no single literal
    exceeds MSVC's 16380-byte string-literal limit (the 550-item archive golden
    serializes well past it).
    """
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
    return '\n        '.join('"' + "".join(part) + '"' for part in chunks)


def _emit(rows: list[dict]) -> str:
    lines: list[str] = []
    lines.append("// GENERATED by scripts/gen_todo_goldens.py from the Python reference")
    lines.append("// implementation (kimi_cli/tools/todo/__init__.py, TodoList + todo_update)")
    lines.append("// run against a stub root runtime/session. Do not edit by hand -")
    lines.append("// regenerate with: python scripts/gen_todo_goldens.py")
    lines.append("//")
    lines.append("// Every row is one tool call. Rows are grouped by `case`; the runner")
    lines.append("// starts a fresh in-memory session whenever `case` changes.")
    lines.append("struct todo_golden_step {")
    lines.append("    const char *case_name;")
    lines.append("    int32_t step;")
    lines.append("    const char *tool; // \"write\" | \"update\"")
    lines.append("    int32_t max_layers;")
    lines.append("    const char *current_prompt; // Runtime.current_prompt (\"\" == None)")
    lines.append("    const char *args; // raw JSON object handed to the tool")
    lines.append("    bool validation_error; // Python returned a ToolValidateError")
    lines.append("    bool is_error;")
    lines.append("    const char *output;")
    lines.append("    const char *message;")
    lines.append("    const char *display; // JSON [{title,status,notes,depth}, ...]")
    lines.append("    const char *state;   // JSON {\"todos\":[...],\"archived_todos\":[...]}")
    lines.append("};")
    lines.append("")
    lines.append("const todo_golden_step kTodoGoldens[] = {")
    for row in rows:
        args_json = json.dumps(row["args"], ensure_ascii=False, separators=(",", ":"))
        display_json = json.dumps(row["display"], ensure_ascii=False, separators=(",", ":"))
        state_json = json.dumps(row["state"], ensure_ascii=False, separators=(",", ":"))
        lines.append("    {%s, %d, %s, %d, %s, %s, %s, %s, %s, %s, %s, %s}," % (
            c_literal(row["case"]),
            row["step"],
            c_literal(row["tool"]),
            row["max_layers"],
            c_literal(row["prompt"]),
            c_literal(args_json),
            "true" if row["validation_error"] else "false",
            "true" if row["is_error"] else "false",
            c_literal(row["output"]),
            c_literal(row["message"]),
            c_literal(display_json),
            c_literal(state_json),
        ))
    lines.append("};")
    lines.append("")
    lines.append("const size_t kTodoGoldenCount = sizeof(kTodoGoldens) / sizeof(kTodoGoldens[0]);")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="fail when the checked-in .inc differs from the generated one")
    parser.add_argument("--python", default=None,
                        help="interpreter able to import kimi_cli (default: kimi-agent venv)")
    parser.add_argument("--out", default=str(OUT_PATH), help="output .inc path")
    args = parser.parse_args()

    try:
        _import_reference()
    except Exception as exc:  # pragma: no cover - environment issue
        fallback = Path(args.python) if args.python else DEFAULT_AGENT_PYTHON
        if fallback.is_file() and Path(sys.executable) != fallback:
            print("re-running under %s (%s)" % (fallback, exc), file=sys.stderr)
            cmd = [str(fallback), str(Path(__file__).resolve()), *sys.argv[1:]]
            return subprocess.call(cmd)
        print("cannot import kimi_cli from %s: %s" % (KIMI_AGENT_ROOT, exc), file=sys.stderr)
        return 2

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
    print("wrote %s (%d steps, %d cases)" % (
        out_path, len(rows), len({r["case"] for r in rows})))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
