#!/usr/bin/env python3
"""Pull main repo + all submodules to latest, with retry logic."""

import configparser
import subprocess
import sys
import time
from pathlib import Path

LOG = "\033[36m[..]\033[0m"
OK = "\033[32m[OK]\033[0m"
WARN = "\033[33m[!!]\033[0m"
ERR = "\033[31m[!!]\033[0m"

MAX_RETRIES = 3
RETRY_DELAY = 3


def log(msg: str) -> None:
    print(f"{LOG} {msg}")


def ok(msg: str) -> None:
    print(f"{OK} {msg}")


def warn(msg: str) -> None:
    print(f"{WARN} {msg}")


def err(msg: str) -> None:
    print(f"{ERR} {msg}")


def run(
    cmd: list[str],
    cwd: Path,
    *,
    retry: int = 1,
    check: bool = False,
) -> subprocess.CompletedProcess:
    """Run a command with optional retry on failure."""
    last_exc: Exception | None = None
    for attempt in range(retry):
        try:
            r = subprocess.run(
                cmd, cwd=str(cwd), capture_output=True, text=True, timeout=120,
            )
            if r.returncode == 0:
                return r
            last_exc = RuntimeError(
                f"exit {r.returncode}: {r.stderr.strip() or r.stdout.strip()}"
            )
        except FileNotFoundError as e:
            last_exc = e
            break  # no point retrying
        except Exception as e:
            last_exc = e

        if attempt + 1 < retry:
            wait = RETRY_DELAY * (attempt + 1)
            warn(f"Retry {attempt + 1}/{retry} after {wait}s: {' '.join(cmd)}")
            time.sleep(wait)

    if check:
        raise RuntimeError(
            f"Command failed after {retry} attempt(s): {' '.join(cmd)}\n{last_exc}"
        )
    # Return a dummy failure
    return subprocess.CompletedProcess(cmd, 1, "", str(last_exc))


def is_git_repo(path: Path) -> bool:
    """Check if path is a valid git repository."""
    r = subprocess.run(
        ["git", "rev-parse", "--git-dir"],
        cwd=str(path), capture_output=True, text=True, timeout=30,
    )
    return r.returncode == 0


def get_current_branch(path: Path) -> str | None:
    """Get the current branch name, or None if detached HEAD."""
    r = run(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=path)
    branch = r.stdout.strip()
    return None if branch == "HEAD" else branch


def pull_main(repo_root: Path) -> bool:
    """Pull the main repository."""
    log("Pulling main repository...")
    branch = get_current_branch(repo_root)
    if branch:
        r = run(
            ["git", "pull", "--rebase", "--autostash", "origin", branch],
            cwd=repo_root, retry=MAX_RETRIES,
        )
    else:
        r = run(["git", "pull", "--rebase", "--autostash"], cwd=repo_root, retry=MAX_RETRIES)
    if r.returncode != 0:
        err(f"Main repo pull failed: {r.stderr.strip() or r.stdout.strip()}")
        return False
    # Show latest commit
    r2 = run(["git", "log", "--oneline", "-1"], cwd=repo_root)
    ok(f"Main repo at {r2.stdout.strip()}")
    return True


def resolve_submodule_branch(sm_path: Path, sm_branch: str | None) -> str | None:
    """Determine target branch for a submodule. Returns branch name or None."""
    if sm_branch:
        return sm_branch

    # Try remote HEAD branch
    r = run(["git", "remote", "show", "origin"], cwd=sm_path, retry=2)
    if r.returncode == 0:
        for line in r.stdout.splitlines():
            line = line.strip()
            if line.startswith("HEAD branch:"):
                return line.split(":", 1)[1].strip()
    return None


def handle_submodule(
    sm_name: str, sm_path: Path, sm_url: str, sm_branch: str | None,
) -> bool:
    """Clone or pull a single submodule. Returns True on success."""
    if not sm_path.exists():
        return clone_submodule(sm_name, sm_path, sm_url, sm_branch)

    if not sm_path.is_dir():
        err(f"{sm_name}: path exists but is not a directory")
        return False

    # Path exists and is a directory
    if not is_git_repo(sm_path):
        # Non-empty non-git directory — remove and re-clone
        warn(f"{sm_name}: directory exists but is not a git repo; removing...")
        import shutil

        shutil.rmtree(sm_path, ignore_errors=True)
        return clone_submodule(sm_name, sm_path, sm_url, sm_branch)

    # Valid git repo — pull latest
    return pull_submodule(sm_name, sm_path, sm_branch)


def clone_submodule(
    sm_name: str, sm_path: Path, sm_url: str, sm_branch: str | None,
) -> bool:
    """Clone a submodule. Returns True on success."""
    log(f"{sm_name}: cloning...")
    cmd = ["git", "clone"]
    if sm_branch:
        cmd += ["--branch", sm_branch]
    cmd += [sm_url, str(sm_path)]

    r = run(cmd, cwd=sm_path.parent, retry=MAX_RETRIES)
    if r.returncode != 0:
        err(f"{sm_name}: clone failed: {r.stderr.strip() or r.stdout.strip()}")
        return False

    r2 = run(["git", "log", "--oneline", "-1"], cwd=sm_path)
    ok(f"{sm_name}: cloned at {r2.stdout.strip()}")
    return True


def pull_submodule(sm_name: str, sm_path: Path, sm_branch: str | None) -> bool:
    """Pull an existing submodule to latest. Returns True on success."""
    # Determine current branch
    r = run(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=sm_path)
    current_branch = r.stdout.strip()

    target_branch = current_branch
    detached = current_branch == "HEAD"

    if detached:
        target_branch = resolve_submodule_branch(sm_path, sm_branch)
        if not target_branch:
            err(f"{sm_name}: detached HEAD and cannot determine target branch")
            return False
        warn(f"{sm_name}: detached HEAD → checking out '{target_branch}'")
        r = run(
            ["git", "checkout", target_branch],
            cwd=sm_path, retry=2,
        )
        if r.returncode != 0:
            err(f"{sm_name}: checkout of '{target_branch}' failed")
            return False

    log(f"{sm_name}: fetching origin/{target_branch}...")
    r = run(
        ["git", "fetch", "origin", target_branch],
        cwd=sm_path, retry=MAX_RETRIES,
    )
    if r.returncode != 0:
        err(f"{sm_name}: fetch failed: {r.stderr.strip()}")
        return False

    log(f"{sm_name}: pulling...")
    r = run(
        ["git", "pull", "--rebase", "origin", target_branch],
        cwd=sm_path, retry=2,
    )
    if r.returncode != 0:
        # Fallback: reset --hard
        warn(f"{sm_name}: pull --rebase failed; reset --hard to origin/{target_branch}")
        r = run(
            ["git", "reset", "--hard", f"origin/{target_branch}"],
            cwd=sm_path, retry=2,
        )
        if r.returncode != 0:
            err(f"{sm_name}: reset also failed: {r.stderr.strip()}")
            return False

    r2 = run(["git", "log", "--oneline", "-1"], cwd=sm_path)
    ok(f"{sm_name}: at {r2.stdout.strip()}")
    return True


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    gitmodules = repo_root / ".gitmodules"

    # 1. Pull main repo
    if not pull_main(repo_root):
        return 1

    print()

    # 2. Parse .gitmodules
    if not gitmodules.exists():
        ok("No .gitmodules; nothing else to do.")
        return 0

    parser = configparser.ConfigParser()
    parser.read(str(gitmodules))

    submodules: list[dict] = []
    for section in parser.sections():
        if section.startswith("submodule "):
            name = section.split('"')[1] if '"' in section else section[len("submodule "):]
            submodules.append({
                "name": name,
                "path": parser.get(section, "path"),
                "url": parser.get(section, "url"),
                "branch": parser.get(section, "branch", fallback=None),
            })

    if not submodules:
        ok("No submodules; done.")
        return 0

    log(f"Updating {len(submodules)} submodule(s)...\n")

    failed: list[str] = []
    for i, sm in enumerate(submodules, 1):
        print(f"\033[36m[{i}/{len(submodules)}]\033[0m {sm['name']}")
        sm_path = repo_root / sm["path"]
        success = handle_submodule(
            sm["name"], sm_path, sm["url"], sm["branch"],
        )
        if not success:
            failed.append(sm["name"])
        print()

    if failed:
        err(f"Failed submodules: {', '.join(failed)}")
        return 1

    ok("All submodules up to date.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
