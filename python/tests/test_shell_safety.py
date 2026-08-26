"""Parity tests for kimix_native.tools shell-safety kernels (plan: commit
0582e09 "Study from hermes").

Compares the native kernels against the verbatim _compat mirrors over
adversarial ASCII corpora (native path) and non-ASCII commands (compat path,
equal by construction).  Gate toggled in-process via KIMIX_NATIVE_TOOLS=0.

Coverage:
- command_detection_variants: collapse/dedup/deobfuscate
- detect_hardline_command / check_hardline_blocked: all 7 rule types +
  obfuscation defeat + benign passthrough
- foreground_background_guidance: 12 patterns + quoted-span stripping
  - base_command_name: first non-assignment command word
  - interpret_exit_code / is_expected_exit: well-known exit codes and
    expected-exit classification (pure Python in the shim — no native kernel)
- annotate_failure: 4000-char sample cap, module-not-found capture
"""

import pytest

from kimix_native import tools as T


def _run_both(fn, *args, monkeypatch):
    monkeypatch.delenv("KIMIX_NATIVE_TOOLS", raising=False)
    native = fn(*args)
    monkeypatch.setenv("KIMIX_NATIVE_TOOLS", "0")
    compat = fn(*args)
    assert native == compat, f"native != compat for {args!r}:\n{native!r}\n{compat!r}"
    return native, compat


# ---------------------------------------------------------------------------
# command_detection_variants
# ---------------------------------------------------------------------------

VARIANT_CASES = [
    "rm -rf /",
    "  rm   -rf  /  ",
    r"r\m -rf /",
    "",
    "   ",
    '"rm" -rf /',
    "echo 'hi'",
    "rm -rf / && echo done",
    "RM -RF /",
    "s\u00fcdO rm -rf /",  # non-ASCII routes to compat
]


@pytest.mark.parametrize("command", VARIANT_CASES)
def test_variants_parity(command, monkeypatch):
    native, compat = _run_both(T.command_detection_variants, command, monkeypatch=monkeypatch)
    assert compat == T._compat_command_detection_variants(command)


def test_variants_goldens(monkeypatch):
    monkeypatch.delenv("KIMIX_NATIVE_TOOLS", raising=False)
    assert T.command_detection_variants("") == []
    assert T.command_detection_variants("   ") == []
    assert T.command_detection_variants("rm -rf /") == ["rm -rf /"]
    assert T.command_detection_variants(r"r\m -rf /") == [r"r\m -rf /", "rm -rf /"]
    assert T.command_detection_variants("RM -RF /") == ["RM -RF /", "rm -rf /"]


# ---------------------------------------------------------------------------
# detect_hardline_command / check_hardline_blocked
# ---------------------------------------------------------------------------

HARDLINE_CASES = [
    "",
    "   ",
    "ls -la",
    "echo hello",
    "rm -rf /",
    "rm -rf ~",
    "rm -rf $HOME",
    "rm -rf ${HOME}",
    "rm -rf /*",
    "rm -rf /.",
    "rm -r /",
    "rm -f /",
    "rm /",
    "rm -rf /tmp/build",
    "rmdir /",
    "rmdir -r /",
    "rmdir -s /",
    "rmdir -p /",
    "del /f /s /q C:\\*",
    "del /s /q C:\\",
    "del /q C:\\Windows",
    "rm -rf C:\\",
    "rm -rf c:/",
    "rm -rf C:\\*",
    r"r\m -rf /",
    "r'm' -rf /",
    "rm '-rf' /",
    "mkfs.ext4 /dev/sda1",
    "mkfs /dev/sda",
    "mkfs.fat -n USB /dev/sdb1",
    "mkfs.xfs /dev/sdc",
    "dd if=/dev/zero of=/dev/sda",
    "dd if=/dev/zero of=/dev/nvme0n1",
    "dd if=/dev/zero of=/dev/disk2",
    "dd of=/dev/rdisk1",
    "dd if=/dev/zero of=/tmp/out",
    "dd if=/dev/zero of=/dev/sd",
    "shutdown -h now",
    "reboot",
    "poweroff",
    "halt",
    "shutdown",
    ":(){ :|:& };:",
    ":(){ :& };:",
    "kill 1",
    "kill -9 1",
    "kill $PPID",
    "kill 123",
    "kill -9 123",
    "killall 1",
    "format C:",
    "format D:\\",
    "format C:\\Windows",
    "format c:",
    "sudo rm -rf /",
    "git rm -rf /",
    "echo rm -rf /",
    "rm -rf /\u00e9",  # non-ASCII routes to compat
]


@pytest.mark.parametrize("command", HARDLINE_CASES)
def test_detect_hardline_parity(command, monkeypatch):
    native, compat = _run_both(T.detect_hardline_command, command, monkeypatch=monkeypatch)
    assert compat == T._compat_detect_hardline_command(command)


@pytest.mark.parametrize("command", HARDLINE_CASES)
def test_check_hardline_parity(command, monkeypatch):
    native, compat = _run_both(T.check_hardline_blocked, command, monkeypatch=monkeypatch)
    assert compat == T._compat_check_hardline_blocked(command)


@pytest.mark.parametrize(
    "command,expected",
    [
        ("rm -rf /", "Recursive delete of protected root/home (`/`)"),
        ("rm -rf $HOME", "Recursive delete of protected root/home (`$home`)"),
        ("rm -rf ${HOME}", "Recursive delete of protected root/home (`${home}`)"),
        ("del /f /s /q C:\\*", "Recursive delete of protected root/home (`c:\\*`)"),
        ("mkfs.ext4 /dev/sda1", "Disk formatting command (`mkfs`) is blocked"),
        ("dd if=/dev/zero of=/dev/sda", "`dd` writing to a raw device is blocked"),
        ("shutdown -h now", "System `shutdown` command is blocked"),
        (":(){ :|:& };:", "Fork bomb pattern detected"),
        ("kill 1", "`kill` targeting PID 1 (or `$PPID`) is blocked"),
        ("format C:", "Windows `format` on a drive is blocked"),
    ],
)
def test_detect_hardline_goldens(command, expected, monkeypatch):
    monkeypatch.delenv("KIMIX_NATIVE_TOOLS", raising=False)
    blocked, desc = T.detect_hardline_command(command)
    assert blocked and desc == expected


def test_check_hardline_obfuscation(monkeypatch):
    monkeypatch.delenv("KIMIX_NATIVE_TOOLS", raising=False)
    for cmd in (r"r\m -rf /", "r'm' -rf /", "rm '-rf' /", r"R\M -RF /"):
        blocked, desc = T.check_hardline_blocked(cmd)
        assert blocked, cmd
        assert desc == "Recursive delete of protected root/home (`/`)", cmd
    blocked, _ = T.check_hardline_blocked("echo hello")
    assert not blocked


# ---------------------------------------------------------------------------
# foreground_background_guidance
# ---------------------------------------------------------------------------

GUIDANCE_CASES = [
    "",
    "   ",
    "ls -la",
    "npm run dev",
    "npm run start",
    "npm run build",
    "pnpm run watch",
    "yarn run serve",
    "bun run dev",
    "next dev",
    "vite",
    "nodemon app.js",
    "uvicorn app:app",
    "gunicorn app:app",
    "python -m http.server",
    "docker compose up",
    "docker-compose up",
    "echo done &",
    "echo done & ",
    "nohup python app.py",
    "setsid bash",
    "echo 'npm run dev'",
    'echo "docker compose up"',
    "grep npm run dev x",
    "npmrun dev",
    "vite \u00e9",  # non-ASCII routes to compat
]


@pytest.mark.parametrize("command", GUIDANCE_CASES)
def test_guidance_parity(command, monkeypatch):
    native, compat = _run_both(T.foreground_background_guidance, command, monkeypatch=monkeypatch)
    assert compat == T._compat_foreground_background_guidance(command)


def test_guidance_goldens(monkeypatch):
    monkeypatch.delenv("KIMIX_NATIVE_TOOLS", raising=False)
    hint = (
        "Long-running process detected. Consider mode='send' (background) + "
        "TaskOutput to avoid blocking on timeout."
    )
    assert T.foreground_background_guidance("npm run dev") == hint
    assert T.foreground_background_guidance("docker-compose up") == hint
    assert T.foreground_background_guidance("echo done &") == hint
    assert T.foreground_background_guidance("npm run build") is None
    assert T.foreground_background_guidance("echo 'npm run dev'") is None
    assert T.foreground_background_guidance("") is None


# ---------------------------------------------------------------------------
# base_command_name / interpret_exit_code
# ---------------------------------------------------------------------------

BASE_NAME_CASES = [
    "/usr/bin/grep -r foo",
    "FOO=1 git diff",
    "python -m http.server",
    "app.exe --help",
    "cmd.exe",
    "echo hi && grep foo",
    "",
    "ls",
    "   ",
    "a=b c=d",
    "python3.11 -V",
    "/x/y/tool.EXE arg",
    "ls \u00e9",  # non-ASCII routes to compat
]


@pytest.mark.parametrize("command", BASE_NAME_CASES)
def test_base_name_parity(command, monkeypatch):
    native, compat = _run_both(T.base_command_name, command, monkeypatch=monkeypatch)
    assert compat == T._compat_base_command_name(command)


def test_base_name_goldens(monkeypatch):
    monkeypatch.delenv("KIMIX_NATIVE_TOOLS", raising=False)
    assert T.base_command_name("/usr/bin/grep -r foo") == "grep"
    assert T.base_command_name("FOO=1 git diff") == "git"
    assert T.base_command_name("cmd.exe") == "cmd"
    assert T.base_command_name("/x/y/tool.EXE arg") == "tool"
    assert T.base_command_name("a=b c=d") == ""
    assert T.base_command_name("") == ""


EXIT_CODE_CASES = [
    ("grep foo", 1),
    ("grep foo", 2),
    ("grep foo", 0),
    ("grep foo", None),
    ("diff a b", 1),
    ("find . -name x", 1),
    ("test -f x", 1),
    ("[ -f x ]", 1),
    ("curl https://x", 6),
    ("curl https://x", 7),
    ("curl https://x", 22),
    ("curl https://x", 28),
    ("curl https://x", 99),
    ("git diff", 1),
    ("git diff", 2),
    ("ls", 1),
    ("", 1),
    ("   ", 1),
    ("egrep foo", 1),
    ("rg foo", 1),
    ("colordiff a b", 1),
    ("C:\\tools\\grep.exe foo", 1),
    ("python -m pytest", 5),
    ("grep \u00e9", 1),  # non-ASCII routes to compat
]


@pytest.mark.parametrize("command,code", EXIT_CODE_CASES)
def test_exit_code_python(command, code):
    # interpret_exit_code is pure Python in the shim (no native kernel), so it
    # must simply delegate to the compat mirror.
    assert T.interpret_exit_code(command, code) == T._compat_interpret_exit_code(command, code)


def test_exit_code_goldens():
    assert T.interpret_exit_code("grep foo", 1) == "No matches found (not an error)"
    assert T.interpret_exit_code("curl https://x", 6) == "Could not resolve host (DNS failure)"
    assert T.interpret_exit_code("git diff", 1).startswith("Non-zero exit")
    assert T.interpret_exit_code("grep foo", 0) is None
    assert T.interpret_exit_code("grep foo", None) is None
    # SIGPIPE inside a pipeline is the normal truncation meaning.
    assert T.interpret_exit_code("yes | head", 141) == (
        "SIGPIPE: an upstream pipeline stage was truncated "
        "(expected when piping to head/tail)"
    )


# ---------------------------------------------------------------------------
# is_expected_exit (pure Python in the shim)
# ---------------------------------------------------------------------------

EXPECTED_CASES = [
    ("grep foo", 1, True),
    ("egrep foo", 1, True),
    ("rg foo", 1, True),
    ("diff a b", 1, True),
    ("colordiff a b", 1, True),
    ("test -f x", 1, True),
    ("[ -f x ]", 1, True),
    ("find . -name x", 1, True),
    ("ls", 1, False),
    ("git merge", 1, False),
    ("grep foo", 2, False),
    ("grep foo", 0, False),
    ("grep foo", None, False),
    ("", 1, False),
    # SIGPIPE: expected only when a top-level pipeline exists.
    ("yes | head", 141, True),
    ("producer --lines | tail -n 5", 141, True),
    ("yes", 141, False),
    ("echo 'a | b'", 141, False),  # pipe inside quotes is not a pipeline
    ("a || b", 141, False),  # logical-OR is not a pipeline
]


@pytest.mark.parametrize("command,code,expected", EXPECTED_CASES)
def test_is_expected_exit(command, code, expected):
    assert T.is_expected_exit(command, code) is expected
    assert T.is_expected_exit(command, code) == T._compat_is_expected_exit(command, code)


# ---------------------------------------------------------------------------
# annotate_failure
# ---------------------------------------------------------------------------

ANNOTATE_CASES = [
    ("", "x", 1),
    ("bash: foo: command not found", "foo", 127),
    ("'foo' is not recognized as an internal or external command", "foo", 1),
    ("ls: cannot access 'x': No such file or directory", "ls", 2),
    ("ModuleNotFoundError: No module named 'requests'", "python", 1),
    ("Traceback (most recent call last):\nModuleNotFoundError: No module named 'pandas'",
     "python", 1),
    ("MODULENOTFOUNDERROR: No Module Named 'FooBar'", "python", 1),
    ("Modulenotfounderror: no module named 'abc'", "python", 1),
    ("Permission denied", "cat", 1),
    ("everything fine", "ls", 0),
    ("x" * 5000 + "command not found", "x", 1),
    ("no such file or directory", "ls", 1),
    ("caf\u00e9 command not found", "x", 1),  # non-ASCII output -> compat
]


@pytest.mark.parametrize("output,command,code", ANNOTATE_CASES)
def test_annotate_parity(output, command, code, monkeypatch):
    native, compat = _run_both(T.annotate_failure, output, command, code, monkeypatch=monkeypatch)
    assert compat == T._compat_annotate_failure(output, command, code)


def test_annotate_goldens(monkeypatch):
    monkeypatch.delenv("KIMIX_NATIVE_TOOLS", raising=False)
    r = T.annotate_failure("ModuleNotFoundError: No module named 'requests'", "python", 1)
    assert r == ("Python module requests is missing. Install it "
                 "(e.g. `pip install requests`) or check the environment.")
    r = T.annotate_failure("MODULENOTFOUNDERROR: No Module Named 'FooBar'", "python", 1)
    assert r == ("Python module FooBar is missing. Install it "
                 "(e.g. `pip install FooBar`) or check the environment.")
    assert T.annotate_failure("", "x", 1) is None
    assert T.annotate_failure("everything fine", "ls", 0) is None
