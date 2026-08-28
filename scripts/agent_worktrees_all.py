#!/usr/bin/env python3
"""Pre-create the per-tool agent worktrees (serially, to avoid git lock races).

    python scripts/agent_worktrees_all.py glob grep read ...
"""
from __future__ import annotations

import sys

sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent))
from agent_worktree import create  # noqa: E402

for name in sys.argv[1:]:
    print(">>", name)
    create(name)
print("ALL DONE")
