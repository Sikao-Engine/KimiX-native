"""Parity test for kimix_native.diff native vs fallback.

Exercises unified_diff, diff_hunks, and inline_diff_ranges on a synthetic
corpus and asserts semantic equivalence (round-trip, parseability, counts).
"""

from __future__ import annotations

import os
import random
import sys

# Ensure we import from the working tree, not any installed package.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "python"))

import kimix_native.diff as diff_mod


def apply_hunks(old_text: bytes, hunks: list[dict]) -> str:
    """Apply diff_hunks result to old_text and return reconstructed text."""
    old_lines = old_text.decode("utf-8", "surrogatepass").splitlines()
    result: list[str] = []
    old_idx = 0

    for hunk in hunks:
        h_old_start = hunk["old_start"] - 1
        while old_idx < h_old_start:
            result.append(old_lines[old_idx])
            old_idx += 1

        oi = 0
        ni = 0
        while oi < len(hunk["old_lines"]) or ni < len(hunk["new_lines"]):
            if oi < len(hunk["old_lines"]) and ni < len(hunk["new_lines"]):
                if hunk["old_lines"][oi] == hunk["new_lines"][ni]:
                    result.append(hunk["old_lines"][oi])
                    old_idx += 1
                    oi += 1
                    ni += 1
                else:
                    old_idx += 1
                    oi += 1
                    result.append(hunk["new_lines"][ni])
                    ni += 1
            elif oi < len(hunk["old_lines"]):
                old_idx += 1
                oi += 1
            else:
                result.append(hunk["new_lines"][ni])
                ni += 1

    while old_idx < len(old_lines):
        result.append(old_lines[old_idx])
        old_idx += 1

    return "\n".join(result)


def parse_unified_counts(udiff: bytes) -> tuple[int, int]:
    """Return (added, deleted) line counts from a unified diff."""
    added = 0
    deleted = 0
    for raw in udiff.splitlines():
        line = raw.decode("utf-8", "surrogatepass")
        if line.startswith("+") and not line.startswith("+++"):
            added += 1
        elif line.startswith("-") and not line.startswith("---"):
            deleted += 1
    return added, deleted


def corpus() -> list[tuple[bytes, bytes, str]]:
    """Return (old_text, new_text, label) test cases."""
    cases: list[tuple[bytes, bytes, str]] = [
        (b"", b"", "empty both"),
        (b"hello", b"hello", "identical"),
        (b"hello", b"world", "single line change"),
        (b"a\nb\nc\n", b"a\nB\nc\n", "multi-line change"),
        (b"a\nb\nc", b"a\nB\nc", "no trailing newline"),
        (b"a\nb", b"a\nb\n", "trailing newline added"),
        (b"line1\nline2\nline3\n", b"line1\nline2\nline3\n", "identical multi"),
    ]

    # Multiple hunks.
    old_multi = "\n".join(f"line{i}" for i in range(30)) + "\n"
    new_multi = old_multi.replace("line5\n", "LINE5\n").replace("line25\n", "LINE25\n")
    cases.append((old_multi.encode(), new_multi.encode(), "multiple hunks"))

    # Code-like text.
    old_code = (
        "def foo():\n"
        "    x = 1\n"
        "    return x\n"
    )
    new_code = (
        "def foo():\n"
        "    x = 2\n"
        "    y = 3\n"
        "    return x + y\n"
    )
    cases.append((old_code.encode(), new_code.encode(), "code-like"))

    # Unicode text.
    old_uni = "こんにちは\n世界\n"
    new_uni = "こんにちは\n地球\n"
    cases.append((old_uni.encode(), new_uni.encode(), "unicode"))

    # Surrogatepass bytes (lone surrogate encoded as UTF-8).
    old_sur = b"hello \xed\xa0\x80world"
    new_sur = b"hello \xed\xa0\x80WORLD"
    cases.append((old_sur, new_sur, "surrogatepass"))

    # Random edit.
    random.seed(42)
    alphabet = "abcdefghijklmnopqrstuvwxyz\n"
    old_rand = "".join(random.choice(alphabet) for _ in range(200))
    new_rand = old_rand.replace("abc", "XYZ", 3)
    cases.append((old_rand.encode(), new_rand.encode(), "random"))

    return cases


def run_case(label: str, old: bytes, new: bytes, path: str = "test.txt") -> None:
    print(f"  checking: {label}")

    native_udiff = diff_mod.unified_diff(old, new, path, True, "\n")
    fb_udiff = diff_mod._compat_unified_diff(old, new, path, True, "\n")
    assert native_udiff == fb_udiff, (
        f"[{label}] unified_diff mismatch\n"
        f"native:\n{native_udiff.decode('utf-8', 'surrogatepass')}\n"
        f"fallback:\n{fb_udiff.decode('utf-8', 'surrogatepass')}"
    )

    for ctx in (0, 1, 3, 10):
        native_hunks = diff_mod.diff_hunks(old, new, ctx)
        fb_hunks = diff_mod._compat_diff_hunks(old, new, ctx)
        assert len(native_hunks) == len(fb_hunks), (
            f"[{label}] hunk count mismatch with ctx={ctx}: "
            f"{len(native_hunks)} vs {len(fb_hunks)}"
        )
        for nh, fh in zip(native_hunks, fb_hunks):
            assert nh["old_start"] == fh["old_start"], f"[{label}] old_start mismatch"
            assert nh["new_start"] == fh["new_start"], f"[{label}] new_start mismatch"
            assert nh["old_lines"] == fh["old_lines"], (
                f"[{label}] old_lines mismatch with ctx={ctx}"
            )
            assert nh["new_lines"] == fh["new_lines"], (
                f"[{label}] new_lines mismatch with ctx={ctx}"
            )

    # Round-trip property using native hunks with default context.
    native_hunks = diff_mod.diff_hunks(old, new, 3)
    reconstructed = apply_hunks(old, native_hunks)
    expected = new.decode("utf-8", "surrogatepass").splitlines()
    assert reconstructed.splitlines() == expected, (
        f"[{label}] round-trip mismatch\n"
        f"reconstructed: {reconstructed!r}\n"
        f"expected:        {new!r}"
    )

    # Parseability and counts.
    added, deleted = parse_unified_counts(native_udiff)
    if native_udiff:
        assert b"@@" in native_udiff, f"[{label}] missing hunk header"
    fb_added, fb_deleted = parse_unified_counts(fb_udiff)
    assert added == fb_added, f"[{label}] added count mismatch"
    assert deleted == fb_deleted, f"[{label}] deleted count mismatch"


def run_inline_cases() -> None:
    pairs = [
        ("hello world", "hello WORLD", "simple"),
        ("a\tb", "a\tB", "tabs"),
        ("abc", "xyz", "dissimilar"),
        ("", "", "empty"),
        ("foo", "foobar", "insert only"),
        ("foobar", "foo", "delete only"),
        ("héllo", "héllo", "identical unicode"),
        ("héllo", "héllo world", "unicode insert"),
    ]

    for old_line, new_line, label in pairs:
        print(f"  checking inline: {label}")
        native_del, native_ins = diff_mod.inline_diff_ranges(old_line, new_line)
        fb_del, fb_ins = diff_mod._compat_inline_diff_ranges(old_line, new_line)
        assert native_del == fb_del, (
            f"[{label}] inline delete ranges mismatch: {native_del} vs {fb_del}"
        )
        assert native_ins == fb_ins, (
            f"[{label}] inline insert ranges mismatch: {native_ins} vs {fb_ins}"
        )


def main() -> int:
    print("Running diff parity tests...")
    for old, new, label in corpus():
        run_case(label, old, new)
    run_inline_cases()
    print("All parity tests passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
