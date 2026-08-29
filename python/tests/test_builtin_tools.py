"""Tests for runtime_py.builtin_tools bindings.

Exercises the C++ built-in tool kernels exposed under
`runtime_py.builtin_tools.shell`, `.file`, and `.web` and compares a few
key results against the pure-Python references in the kimi-agent repo.
"""

import pytest

import kimix_native
from kimix_native import tools as T


# ---------------------------------------------------------------------------
# shell
# ---------------------------------------------------------------------------


def test_interpret_exit_code_grep_no_matches():
    assert T.interpret_exit_code("grep foo", 1) == "No matches found (not an error)"


def test_interpret_exit_code_diff():
    assert T.interpret_exit_code("diff a b", 1) == "Files differ (expected, not an error)"


def test_interpret_exit_code_unknown():
    assert T.interpret_exit_code("unknown", 123) is None
    assert T.interpret_exit_code("grep foo", 0) is None
    assert T.interpret_exit_code("grep foo", None) is None


def test_is_expected_exit_grep():
    assert T.is_expected_exit("grep foo", 1) is True
    assert T.is_expected_exit("grep foo", 0) is False
    assert T.is_expected_exit("grep foo", None) is False
    assert T.is_expected_exit("unknown", 1) is False


def test_annotate_failure_command_not_found():
    hint = T.annotate_failure("bash: foo: command not found", "foo", 127)
    assert hint is not None
    assert "not found" in hint.lower()


def test_annotate_failure_no_such_file():
    hint = T.annotate_failure("No such file or directory", "cat", 1)
    assert hint is not None
    assert "does not exist" in hint.lower()


def test_annotate_failure_no_hit():
    assert T.annotate_failure("random text", "cmd", 1) is None


def test_check_hardline_blocked_rm_rf_root():
    blocked, desc = T.check_hardline_blocked("rm -rf /")
    assert blocked is True
    assert "recursive delete" in desc.lower()


def test_check_hardline_blocked_safe():
    blocked, desc = T.check_hardline_blocked("ls -la")
    assert blocked is False
    assert desc is None


def test_foreground_background_guidance_server():
    hint = T.foreground_background_guidance("python -m http.server")
    assert hint is not None
    assert "background" in hint.lower()


def test_foreground_background_guidance_normal():
    assert T.foreground_background_guidance("ls") is None


def test_truncate_lines_short():
    text = "a\nb\nc\n"
    assert T.truncate_lines(text, 10) == text


def test_truncate_lines_fold():
    lines = [f"line{i}" for i in range(20)]
    text = "\n".join(lines) + "\n"
    folded = T.truncate_lines(text, 6)
    assert "omitted" in folded.lower()


def test_command_detection_variants():
    variants = T.command_detection_variants("r\\m -rf /")
    assert len(variants) >= 2
    assert any("rm -rf /" in v for v in variants)


# ---------------------------------------------------------------------------
# file / glob
# ---------------------------------------------------------------------------


def test_fnmatch_match_basics():
    assert T.fnmatch_match("*.py", "test.py", True) is True
    assert T.fnmatch_match("*.py", "test.cpp", True) is False
    assert T.fnmatch_match("[abc].py", "a.py", True) is True
    assert T.fnmatch_match("[abc].py", "d.py", True) is False


def test_match_path_pattern_basics():
    assert T.match_path_pattern("*.py", "foo.py", True) is True
    assert T.match_path_pattern("src/*.py", "src/foo.py", True) is True
    assert T.match_path_pattern("src/*.py", "foo.py", True) is False


def test_is_unsafe_recursive_pattern():
    assert T.is_unsafe_recursive_pattern("**") is True
    assert T.is_unsafe_recursive_pattern("src/**/*.py") is False


# ---------------------------------------------------------------------------
# web / compact
# ---------------------------------------------------------------------------


def test_extract_text_simple():
    msg = {
        "role": "user",
        "content": [{"type": "text", "text": "hello world"}],
    }
    assert T._native.builtin_tools.web.extract_text(msg, " ") == "hello world"


def test_has_think_part():
    think = {
        "role": "assistant",
        "content": [{"type": "think", "think": "reasoning"}],
    }
    text_only = {
        "role": "assistant",
        "content": [{"type": "text", "text": "hi"}],
    }
    assert T._native.builtin_tools.web.has_think_part(think) is True
    assert T._native.builtin_tools.web.has_think_part(text_only) is False
