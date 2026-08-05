"""Verify native and fallback workspace kernels produce identical output."""

from __future__ import annotations

import os
import sys
import tempfile
from pathlib import Path

# Ensure the build outputs and shims are importable. We force the freshly
# built runtime_py.pyd from D:\KimiX-native\bin\debug to take priority.
sys.path.insert(0, r"D:\KimiX-native\python")
sys.path.insert(0, r"D:\KimiX-native\bin\debug")
# Remove any agent bin directory that may contain a stale runtime_py.pyd.
agent_bin = r"D:\kimi-agent\bin"
if agent_bin in sys.path:
    sys.path.remove(agent_bin)

# Import the same module twice; use_native is checked at call time, so we can
# toggle per-call via the environment variable.
from kimix_native import workspace as ws_module


def native_call(name: str, *args, **kwargs):
    os.environ["KIMIX_NATIVE_WORKSPACE"] = "1"
    fn = getattr(ws_module, name)
    return fn(*args, **kwargs)


def fallback_call(name: str, *args, **kwargs):
    os.environ["KIMIX_NATIVE_WORKSPACE"] = "0"
    fn = getattr(ws_module, name)
    return fn(*args, **kwargs)


def assert_eq(label: str, native, fallback) -> None:
    if native != fallback:
        print(f"FAIL: {label}")
        print(f"  native:   {native!r}")
        print(f"  fallback: {fallback!r}")
        raise AssertionError(label)
    print(f"OK: {label}")


def test_snapshot() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "sub").mkdir()
        (root / "skip").mkdir()

        (root / "a.txt").write_bytes(b"hello\nworld\n")
        (root / "sub" / "b.txt").write_bytes(b"nested\n")
        (root / "skip" / "ignored.txt").write_bytes(b"ignored\n")
        (root / "exact.txt").write_bytes(b"x" * 10)
        (root / "big.bin").write_bytes(b"y" * 11)
        (root / "binary.bin").write_bytes(b"\x00\x01\x02")

        try:
            os.symlink(root / "a.txt", root / "link.txt")
        except OSError:
            pass  # symlinks may be unavailable

        kwargs = {"ignore_dirs": ["skip"], "max_file_bytes": 10}
        native = native_call("snapshot", str(root), **kwargs)
        fallback = fallback_call("snapshot", str(root), **kwargs)

        assert_eq("snapshot keys", sorted(native.keys()), sorted(fallback.keys()))
        for key in native:
            assert_eq(f"snapshot content {key}", native[key], fallback[key])


def test_diff_and_changed() -> None:
    before = {
        "a.txt": b"alpha\n",
        "b.txt": b"beta\n",
        "del.txt": b"deleted\n",
        "bin.bin": b"\x00\x01",
    }
    after = {
        "a.txt": b"alpha\nupdated\n",
        "b.txt": b"beta\n",
        "add.txt": b"added\n",
        "bin.bin": b"\x00\x02",
    }

    text_exts = {".bin"}
    diff_kwargs = {"text_extensions": text_exts, "context_lines": 3}

    native_diff = native_call("diff_snapshots", before, after, **diff_kwargs)
    fallback_diff = fallback_call("diff_snapshots", before, after, **diff_kwargs)
    assert_eq("diff_snapshots", native_diff, fallback_diff)

    native_files = native_call("changed_files", before, after)
    fallback_files = fallback_call("changed_files", before, after)
    assert_eq("changed_files", native_files, fallback_files)


def test_no_changes() -> None:
    snap = {"a.txt": b"same\n"}
    assert_eq("empty diff", native_call("diff_snapshots", snap, snap), fallback_call("diff_snapshots", snap, snap))
    assert_eq("empty changed", native_call("changed_files", snap, snap), fallback_call("changed_files", snap, snap))


def test_empty_files() -> None:
    before = {"empty.txt": b""}
    after = {"empty.txt": b"", "other_empty.txt": b""}
    assert_eq("empty file diff", native_call("diff_snapshots", before, after), fallback_call("diff_snapshots", before, after))
    assert_eq("empty file changed", native_call("changed_files", before, after), fallback_call("changed_files", before, after))


def main() -> int:
    test_snapshot()
    test_diff_and_changed()
    test_no_changes()
    test_empty_files()
    print("\nAll parity checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
