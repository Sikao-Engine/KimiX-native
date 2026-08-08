"""Parity tests for kimix_native.tools security kernels (plan: commit
0582e09 "Study from hermes").

Compares the native kernels against the verbatim _compat mirrors over
adversarial ASCII corpora (which run natively) and non-ASCII cases (which
route to the mirrors and must be equal by construction).  The gate is toggled
in-process via KIMIX_NATIVE_TOOLS=0 (the shim's use_native reads the env per
call).

Coverage:
- redact_sensitive_output: 10 chained rules, order/leftmost/non-overlap,
  short-value passthrough, empty input, non-ASCII
- scrub_child_env: safe-prefix keep / secret-substring drop / order,
  ASCII + non-ASCII keys
- validate_workdir: allowed set, Python-repr error messages (ASCII control,
  non-ASCII printable + non-printable), None/empty
- bounded_append: head 40% / tail 60% + marker, cap 0, no-truncation path
"""

import pytest

from kimix_native import tools as T


def _run_both(fn, *args, monkeypatch):
    """Run *fn* with the TOOLS gate on and off; returns (native, compat)."""
    monkeypatch.delenv("KIMIX_NATIVE_TOOLS", raising=False)
    native = fn(*args)
    monkeypatch.setenv("KIMIX_NATIVE_TOOLS", "0")
    compat = fn(*args)
    assert native == compat, f"native != compat for {args!r}:\n{native!r}\n{compat!r}"
    return native, compat


# ---------------------------------------------------------------------------
# redact_sensitive_output
# ---------------------------------------------------------------------------

REDACT_CORPUS = [
    "",
    "hello world",
    "no secrets here",
    "https://user:pass@example.com/path",
    "http://user@host:8080/path",
    "http://a:b@c/",
    "prefix https://u:p@h suffix",
    "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0."
    "SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c",
    "token: eyJh.eyJ.SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c",
    "-----BEGIN RSA PRIVATE KEY-----\nMIIEowIBAAKCAQEA...\n"
    "-----END RSA PRIVATE KEY-----\n",
    "-----BEGIN PRIVATE KEY-----abc-----END PRIVATE KEY-----",
    "-----BEGIN OPENSSH PRIVATE KEY-----x-----END OPENSSH PRIVATE KEY-----",
    "-----BEGIN EC PRIVATE KEY-----x-----END EC PRIVATE KEY-----",
    "-----BEGIN DSA PRIVATE KEY-----x-----END DSA PRIVATE KEY-----",
    "-----BEGIN ENCRYPTED PRIVATE KEY-----x-----END ENCRYPTED PRIVATE KEY-----",
    "no end marker -----BEGIN PRIVATE KEY-----",
    "ghp_abcdefghijklmnopqrstuvwxyz",
    "gho_abcdefghijklmnopqrstuvwxyz!",
    "ghu_abcdefghijklmnopqrstuvwxyz",
    "ghp_short",
    "github_pat_abcdefghijklmnopqrstuvwxyz",
    "github_pat_short",
    "glpat-abcdefghijklmnopqrstuv",
    "glpat-short",
    "AKIAIOSFODNN7EXAMPLE",
    "AKIA0123456789ABCDEF",
    "AKIASHORT",
    "Authorization: Bearer abc123",
    "authorization: abc123",
    "Authorization: bearer abc123",
    "x-api-key: k1234567",
    "x-api-key:secret",
    "apikey=k1234567",
    "Proxy-Authorization: xyz",
    "Authorization:bearer abc",
    "Authorization: bearerX",
    "password=supersecret",
    "password='secret123'",
    'password="secret456"',
    "password=x",
    "passwd='abcdef12'",
    "secret: abcdef123456",
    "token: abcdefghij",
    "api_key=abcdef123456",
    "api-key=abcdef123456",
    "apikey=abcdef123456",
    "access_key=abcdef123456",
    "access-key=abcdef123456",
    "accesskey=abcdef123456",
    "PASSWORD = 'abcdef12'",
    "Bearer abcdefghijklmnopqrstuvwxyz",
    "bearer  abcdefghijklmnopqrstuvwxyz",
    "Bearer short",
    "mypassword=abcdef123",
    "passwordx=abcdef123",
    "api key=abcdef123456",
    "url https://u:p@h; Authorization: Bearer abc",
    "combined: eyJh.eyJ.SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c "
    "and ghp_abcdefghijklmnopqrstuvwxyz",
    # non-ASCII: routes to _compat (must be equal by construction)
    "caf\u00e9 password=motdepasse123",
    "\u4e2d\u6587 https://user:pass@host/path",
    "token: \u00e9\u00e8\u00ea123456",
    "emoji \U0001f600 password=secret123",
]


@pytest.mark.parametrize("case", REDACT_CORPUS)
def test_redact_parity(case, monkeypatch):
    native, compat = _run_both(T.redact_sensitive_output, case, monkeypatch=monkeypatch)
    assert compat == T._compat_redact_sensitive_output(case)


@pytest.mark.parametrize(
    "case,expected",
    [
        ("", ""),
        ("hello world", "hello world"),
        ("password=x", "password=x"),  # short values untouched
        ("https://user:pass@example.com/path", "https://[REDACTED]@example.com/path"),
        ("Authorization: Bearer abc123", "[REDACTED]"),
        ("ghp_abcdefghijklmnopqrstuvwxyz", "[REDACTED]"),
        ("AKIAIOSFODNN7EXAMPLE", "[REDACTED]"),
    ],
)
def test_redact_goldens(case, expected, monkeypatch):
    monkeypatch.delenv("KIMIX_NATIVE_TOOLS", raising=False)
    assert T.redact_sensitive_output(case) == expected


# ---------------------------------------------------------------------------
# scrub_child_env
# ---------------------------------------------------------------------------

SCRUB_ENVS = [
    {},
    {"PATH": "/usr/bin", "HOME": "/home/u"},
    {
        "PATH": "/usr/bin",
        "AWS_SECRET_ACCESS_KEY": "x",
        "DATABASE_URL": "postgres://u:p@h/db",
        "SSH_AUTH_SOCK": "/tmp/ssh",
        "MY_TOKEN": "t",
        "KIMIX_API_KEY": "k",
        "USER": "u",
        "AWS_ACCESS_KEY_ID": "ak",
        "GIT_ASKPASS": "g",
        "LC_ALL": "C",
        "PWD": "/x",
        "SHLVL": "1",
        "_": "x",
        "api_secret_key": "x",
        "TOKENIZER_MODEL": "m",
        "DATABASE_PASSWORD": "p",
        "WEBHOOK_URL": "w",
        "CREDENTIALS": "c",
        "BEARER_TOKEN": "b",
        "MY_DSN": "d",
        "my_api_key": "k",
    },
    {"a": "1", "b": "2", "c": "3"},
    {"\u00e9\u00e8key": "non-ascii-key", "PATH": "/x"},  # non-ASCII key
]


@pytest.mark.parametrize("env", SCRUB_ENVS)
def test_scrub_parity(env, monkeypatch):
    native, compat = _run_both(T.scrub_child_env, dict(env), monkeypatch=monkeypatch)
    assert compat == T._compat_scrub_child_env(dict(env))
    # insertion order preserved
    assert list(native) == list(compat)


def test_scrub_goldens(monkeypatch):
    monkeypatch.delenv("KIMIX_NATIVE_TOOLS", raising=False)
    out = T.scrub_child_env({"PATH": "/usr/bin", "MY_TOKEN": "t", "KIMIX_API_KEY": "k"})
    assert list(out) == ["PATH", "KIMIX_API_KEY"]
    assert T.scrub_child_env({}) == {}


# ---------------------------------------------------------------------------
# validate_workdir (shim-only)
# ---------------------------------------------------------------------------

WORKDIR_CASES = [
    None,
    "",
    "C:\\Users\\me",
    "/tmp/x",
    "a b-c.d~",
    "C:\\Windows\\System32",
    "a;b",
    "a\nb",
    "a\tb",
    "a\rb",
    "a$b",
    "x!y",
    "a|b",
    "a&b",
    "a<b",
    "a>b",
    "a`b",
    "a(b)",
    "a{b}",
    "a*b",
    "a?b",
    "'quote'",
    '"dq"',
    "\x01x",
    "\x7fx",
    "caf\u00e9",
    "a\u00a0b",
    "a\u200bb",
    "a\u2028b",
    "\u4e2d\u6587",
    "emoji \U0001f600",
]


@pytest.mark.parametrize("workdir", WORKDIR_CASES)
def test_validate_workdir_parity(workdir, monkeypatch):
    native, compat = _run_both(T.validate_workdir, workdir, monkeypatch=monkeypatch)
    assert compat == T._compat_validate_workdir(workdir)


def test_validate_workdir_goldens(monkeypatch):
    monkeypatch.delenv("KIMIX_NATIVE_TOOLS", raising=False)
    assert T.validate_workdir(None) is None
    assert T.validate_workdir("") is None
    assert T.validate_workdir("/tmp/x") is None
    assert T.validate_workdir("a;b") == "Invalid workdir: character ';' is not allowed."
    assert T.validate_workdir("a\nb") == "Invalid workdir: character '\\n' is not allowed."
    assert T.validate_workdir("\x01x") == "Invalid workdir: character '\\x01' is not allowed."
    assert T.validate_workdir("caf\u00e9") == "Invalid workdir: character '\u00e9' is not allowed."
    assert T.validate_workdir("a\u00a0b") == "Invalid workdir: character '\\xa0' is not allowed."
    assert T.validate_workdir("a\u200bb") == "Invalid workdir: character '\\u200b' is not allowed."


# ---------------------------------------------------------------------------
# bounded_append
# ---------------------------------------------------------------------------

BOUNDED_CASES = [
    ("", "hello", 100),
    ("a" * 90, "b" * 20, 100),
    ("", "x", 0),
    ("hello", "", 100),
    ("abcdefghijklmnopqrstuvwxyz", "0123456789", 20),
    ("x" * 5, "y" * 5, 3),
    ("", "", 0),
    ("\u00e9" * 50, "x" * 50, 40),  # non-ASCII content
    ("a" * 100, "b" * 100, 200),
    ("a" * 100, "b" * 100, 199),
    ("", "x" * 1000, 500),
]


@pytest.mark.parametrize("content,text,cap", BOUNDED_CASES)
def test_bounded_append_parity(content, text, cap, monkeypatch):
    native, compat = _run_both(T.bounded_append, content, text, cap, monkeypatch=monkeypatch)
    assert compat == T._compat_bounded_append(content, text, cap)


def test_bounded_append_goldens(monkeypatch):
    monkeypatch.delenv("KIMIX_NATIVE_TOOLS", raising=False)
    content, truncated = T.bounded_append("", "hello", 100)
    assert content == "hello" and truncated is False
    content, truncated = T.bounded_append("abcdefghijklmnopqrstuvwxyz", "0123456789", 20)
    assert truncated is True
    assert content == (
        "abcdefgh\n[... (output truncated, keeping first 8 and last 12 chars)]\n"
        "yz0123456789"
    )
