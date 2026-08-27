#!/usr/bin/env python3
"""Create an isolated build worktree of kimix-base for one agent task.

Each parallel implementer works in its own git worktree + its own xmake
build dir, so nobody races on `.xmake/`, `build/` or `bin/`. The vendored
submodules under src/ext are plain directories in this repo's working tree
(they are populated, not per-worktree git clones), so we mirror them into
the new worktree with a file copy.

Usage:
    python scripts/agent_worktree.py create <name>     # -> C:/dev/kimix_wt/<name>
    python scripts/agent_worktree.py remove <name>
    python scripts/agent_worktree.py list
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WT_BASE = ROOT.parent / "kimix_wt"
EXT_DIRS = [
    p.name for p in (ROOT / "src" / "ext").iterdir() if p.is_dir()
]


def run(*args: str, cwd: Path | None = None) -> str:
    res = subprocess.run(
        list(args), cwd=str(cwd or ROOT), capture_output=True, text=True
    )
    if res.returncode != 0:
        sys.stderr.write(f"failed: {' '.join(args)}\n{res.stdout}\n{res.stderr}\n")
        raise SystemExit(1)
    return res.stdout


def create(name: str) -> Path:
    wt = WT_BASE / name
    if wt.exists():
        print(f"[agent_worktree] reusing {wt}")
    else:
        wt.parent.mkdir(parents=True, exist_ok=True)
        run("git", "worktree", "add", "-B", f"agent/{name}", str(wt))
    # Mirror vendored third-party trees (plain dirs in the main worktree).
    for d in EXT_DIRS:
        src = ROOT / "src" / "ext" / d
        dst = wt / "src" / "ext" / d
        if any(dst.iterdir()) if dst.exists() else False:
            continue
        dst.mkdir(parents=True, exist_ok=True)
        shutil.copytree(src, dst, dirs_exist_ok=True, symlinks=True)
    print(wt.as_posix())
    return wt


def remove(name: str) -> None:
    wt = WT_BASE / name
    if wt.exists():
        run("git", "worktree", "remove", "--force", str(wt))
    run("git", "worktree", "prune")
    print(f"removed {name}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["create", "remove", "list"])
    ap.add_argument("name", nargs="?", default="")
    a = ap.parse_args()
    if a.action == "create":
        create(a.name)
    elif a.action == "remove":
        remove(a.name)
    else:
        print(run("git", "worktree", "list"))


if __name__ == "__main__":
    main()
