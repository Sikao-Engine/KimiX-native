"""Parity test: runtime_py.todo (native) vs kimix_native.todo (fallback)."""

from __future__ import annotations

import os
import sys
import tempfile

# Make the native extension and the pure-Python shim importable.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "bin", "debug"))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "python"))

# Force the shim to use its pure-Python implementation.
os.environ["KIMIX_NATIVE"] = "0"

import kimix_native.todo as fallback  # noqa: E402
import runtime_py as native  # noqa: E402


def _norm(value):
    """Recursively normalize pybind11 containers to plain Python objects."""
    if value is None:
        return None
    if isinstance(value, str):
        return value
    if isinstance(value, (list, tuple)):
        return [_norm(v) for v in value]
    if isinstance(value, dict):
        return {k: _norm(v) for k, v in value.items()}
    # pybind11 lists/dicts compare equal, but normalize for diff printing.
    return value


def _items_eq(a, b) -> bool:
    a = _norm(a)
    b = _norm(b)
    return a == b


def _check(name: str, native_val, fallback_val) -> None:
    if not _items_eq(native_val, fallback_val):
        print(f"FAIL: {name}")
        print(f"  native   = {_norm(native_val)!r}")
        print(f"  fallback = {_norm(fallback_val)!r}")
        raise AssertionError(f"mismatch in {name}")
    print(f"ok: {name}")


def _call_merge(old, new, mode, fuzzy=None):
    if fuzzy is None:
        fuzzy = {}
    return native.todo.merge(old, new, mode, fuzzy), fallback.merge(old, new, mode, fuzzy)


def _call_counts(items):
    return native.todo.status_counts(items), fallback.status_counts(items)


def _call_summary(items, max_items=50):
    return native.todo.format_summary(items, max_items), fallback.format_summary(items, max_items)


def main() -> int:
    failures = 0

    def run(name, fn):
        nonlocal failures
        try:
            fn()
            print(f"PASS: {name}")
        except AssertionError as exc:
            failures += 1
            print(f"FAIL: {name}: {exc}")

    # ------------------------------------------------------------------
    # merge() matrix
    # ------------------------------------------------------------------
    def test_append_update_and_new():
        old = [{"title": "A", "status": "pending"}, {"title": "B", "status": "done"}]
        new = [{"title": "A", "status": "in_progress"}, {"title": "C", "status": "pending"}]
        nr, fr = _call_merge(old, new, "append")
        _check("append items", nr["items"], fr["items"])
        _check("append warnings", nr["warnings"], fr["warnings"])
        _check("append error", nr["error"], fr["error"])
        _check("append regressed", nr["regressed"], fr["regressed"])
        _check("append archived", nr["archived"], fr["archived"])

    def test_append_preserve_notes_code():
        old = [{"title": "A", "status": "pending", "notes": "old notes", "code": "old code"}]
        new = [{"title": "A", "status": "in_progress"}]
        nr, fr = _call_merge(old, new, "append")
        _check("preserve notes/code", nr["items"], fr["items"])

    def test_append_clear_done():
        old = [{"title": "A", "status": "done"}, {"title": "B", "status": "done"}]
        new = []
        nr, fr = _call_merge(old, new, "append")
        _check("clear done items", nr["items"], fr["items"])
        _check("clear done archived", nr["archived"], fr["archived"])

    def test_append_clear_blocked():
        old = [{"title": "A", "status": "done"}, {"title": "B", "status": "pending"}]
        new = []
        nr, fr = _call_merge(old, new, "append")
        _check("clear blocked error", nr["error"], fr["error"])

    def test_overwrite_blocked():
        old = [{"title": "A", "status": "pending"}]
        new = [{"title": "B", "status": "done"}]
        nr, fr = _call_merge(old, new, "overwrite")
        _check("overwrite blocked error", nr["error"], fr["error"])

    def test_overwrite_ok():
        old = [{"title": "A", "status": "done"}, {"title": "B", "status": "done"}]
        new = [{"title": "C", "status": "pending"}]
        nr, fr = _call_merge(old, new, "overwrite")
        _check("overwrite ok items", nr["items"], fr["items"])
        _check("overwrite ok archived", nr["archived"], fr["archived"])

    def test_force_overwrite():
        old = [{"title": "A", "status": "done"}, {"title": "B", "status": "pending"}]
        new = [{"title": "C", "status": "in_progress"}]
        nr, fr = _call_merge(old, new, "force_overwrite")
        _check("force items", nr["items"], fr["items"])
        _check("force archived", nr["archived"], fr["archived"])
        _check("force regressed", nr["regressed"], fr["regressed"])

    def test_empty_old():
        old = []
        new = [{"title": "A", "status": "pending"}]
        nr, fr = _call_merge(old, new, "append")
        _check("empty old items", nr["items"], fr["items"])

    def test_duplicate_new_titles():
        new = [{"title": "A", "status": "pending"}, {"title": "A", "status": "done"}]
        nr, fr = _call_merge([], new, "append")
        _check("duplicate error", nr["error"], fr["error"])

    def test_invalid_status():
        new = [{"title": "A", "status": "unknown"}]
        nr, fr = _call_merge([], new, "append")
        _check("invalid status error", nr["error"], fr["error"])

    def test_status_canonicalization():
        new = [{"title": "A", "status": "In-Progress"}]
        nr, fr = _call_merge([], new, "append")
        _check("canonicalized status", nr["items"], fr["items"])

    def test_regression():
        old = [{"title": "A", "status": "done"}]
        new = [{"title": "A", "status": "pending"}]
        nr, fr = _call_merge(old, new, "append")
        _check("regression items", nr["items"], fr["items"])
        _check("regression list", nr["regressed"], fr["regressed"])

    def test_fuzzy_warnings():
        old = [{"title": "A", "status": "pending"}]
        new = [{"title": "B", "status": "pending"}, {"title": "C", "status": "pending"}]
        fuzzy = {"B": ["B warning"], "C": ["C1", "C2"]}
        nr, fr = _call_merge(old, new, "append", fuzzy)
        _check("fuzzy warnings", nr["warnings"], fr["warnings"])

    def test_autofix_in_progress():
        new = [
            {"title": "A", "status": "in_progress"},
            {"title": "B", "status": "in_progress"},
            {"title": "C", "status": "pending"},
        ]
        nr, fr = _call_merge([], new, "append")
        _check("autofix items", nr["items"], fr["items"])
        _check("autofix warnings", nr["warnings"], fr["warnings"])

    def test_invalid_mode():
        nr, fr = _call_merge([], [{"title": "A", "status": "pending"}], "bad_mode")
        _check("invalid mode error", nr["error"], fr["error"])

    # ------------------------------------------------------------------
    # status_counts() and format_summary()
    # ------------------------------------------------------------------
    def test_status_counts():
        items = [
            {"title": "A", "status": "pending"},
            {"title": "B", "status": "in_progress"},
            {"title": "C", "status": "done"},
            {"title": "D", "status": "done"},
        ]
        nc, fc = _call_counts(items)
        _check("status counts", nc, fc)

    def test_format_summary():
        items = [
            {"title": "A", "status": "pending", "code": "!pytest -x"},
            {"title": "B", "status": "in_progress", "notes": "some notes"},
            {"title": "C", "status": "done"},
        ]
        ns, fs = _call_summary(items)
        _check("format summary", ns, fs)

    def test_format_summary_max_items():
        items = [{"title": f"{i}", "status": "pending"} for i in range(10)]
        ns, fs = _call_summary(items, 3)
        _check("format summary max_items", ns, fs)

    def test_format_summary_file_kind():
        with tempfile.NamedTemporaryFile(mode="w", suffix=".py", delete=False) as f:
            f.write("# dummy\n")
            path = f.name
        try:
            items = [{"title": "A", "status": "pending", "code": path}]
            ns, fs = _call_summary(items)
            _check("format summary file kind", ns, fs)
        finally:
            os.unlink(path)

    tests = [
        test_append_update_and_new,
        test_append_preserve_notes_code,
        test_append_clear_done,
        test_append_clear_blocked,
        test_overwrite_blocked,
        test_overwrite_ok,
        test_force_overwrite,
        test_empty_old,
        test_duplicate_new_titles,
        test_invalid_status,
        test_status_canonicalization,
        test_regression,
        test_fuzzy_warnings,
        test_autofix_in_progress,
        test_invalid_mode,
        test_status_counts,
        test_format_summary,
        test_format_summary_max_items,
        test_format_summary_file_kind,
    ]

    for t in tests:
        run(t.__name__, t)

    print()
    if failures:
        print(f"FAILED: {failures} parity test(s)")
        return 1
    print("All parity tests passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
