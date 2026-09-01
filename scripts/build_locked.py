"""Serialized xmake/build runner for parallel agent work.

Multiple agents (or xmake processes) may try to build the same project at the
same time; xmake has no safe guard for concurrent invocations sharing the same
object/cache directories.  This script wraps an arbitrary command line with a
cross-platform advisory lock (atomic lock-file creation + retry), so concurrent
builds of *different* targets queue up instead of racing on shared artifacts
(kimix-core.lib, runtime_py.pyd, .xmake cache, ...).

Usage:
    python scripts/build_locked.py -- <command...>
    python scripts/build_locked.py --timeout 1800 -- xmake build test_native_ansi
    python scripts/build_locked.py -- xmake run test_native_ansi

Exit code is the wrapped command's exit code. The lock is released on exit
(even on Ctrl+C / exceptions).  A stale lock (dead PID) is reclaimed after a
short grace period.
"""

import os
import subprocess
import sys
import time

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCK_PATH = os.path.join(PROJECT_ROOT, ".kimix_cache", "build.lock")


def _pid_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def acquire_lock(timeout: float) -> None:
    os.makedirs(os.path.dirname(LOCK_PATH), exist_ok=True)
    deadline = time.monotonic() + timeout
    while True:
        try:
            # 'x' is O_CREAT|O_EXCL: atomic on all supported platforms.
            with open(LOCK_PATH, "x", encoding="utf-8") as f:
                f.write(str(os.getpid()))
                f.flush()
                os.fsync(f.fileno())
            return
        except FileExistsError:
            # Reclaim stale locks (dead PID).
            try:
                with open(LOCK_PATH, "r", encoding="utf-8") as f:
                    raw = f.read().strip()
                pid = int(raw) if raw else -1
            except (OSError, ValueError):
                pid = -1
            if pid > 0 and not _pid_alive(pid) and time.monotonic() - os.path.getmtime(LOCK_PATH) > 30.0:
                try:
                    os.remove(LOCK_PATH)
                    continue
                except OSError:
                    pass
            if time.monotonic() >= deadline:
                raise TimeoutError(
                    f"timed out after {timeout:.0f}s waiting for build lock "
                    f"({LOCK_PATH}, owner pid={pid})")
            time.sleep(1.0)


def release_lock() -> None:
    try:
        with open(LOCK_PATH, "r", encoding="utf-8") as f:
            raw = f.read().strip()
        if raw == str(os.getpid()):
            os.remove(LOCK_PATH)
    except (OSError, ValueError):
        pass


def main(argv) -> int:
    if "--timeout" in argv:
        i = argv.index("--timeout")
        timeout = float(argv[i + 1])
        del argv[i:i + 2]
    else:
        timeout = 3600.0
    if not argv or argv[0] == "--":
        argv = argv[1:] if argv and argv[0] == "--" else argv
    if not argv:
        print("usage: build_locked.py [--timeout N] -- <command...>", file=sys.stderr)
        return 2

    acquired = False
    try:
        acquire_lock(timeout)
        acquired = True
        proc = subprocess.run(argv, cwd=PROJECT_ROOT)
        return proc.returncode
    except KeyboardInterrupt:
        return 130
    except TimeoutError as exc:
        print(f"build_locked: {exc}", file=sys.stderr)
        return 125
    finally:
        if acquired:
            release_lock()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))