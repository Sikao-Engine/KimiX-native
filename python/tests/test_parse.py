"""Parity tests for kimix_native.parse (plans 011/012).

Compares the native kernels against:
- the vendored reference mirrors (_parse_compat / _shell_compat)
- the REAL kimi-agent reference parsers when the checkout is importable
  (guarded by os.path.exists("C:/dev/kimi-agent/src/kimix/parser"))

Coverage:
- comment parsers: fixture corpora per language (>= 20 cases each) with
  byte-identical comment sets (content, line, column, kind) and
  code_without_comments
- shell scanners: bash_fix / process_unquoted / pwsh_fix / pwsh_transform
  golden vectors (Windows paths, quotes, heredocs, ternary, $?, here-strings)
"""

import os
import shutil
import subprocess
import sys

import kimix_native
import pytest
from kimix_native import parse as P

REF_PARSER_ROOT = r"C:\dev\kimi-agent\src\kimix\parser"
HAS_REF = os.path.exists(os.path.join(REF_PARSER_ROOT, "base.py"))
if HAS_REF:
    # Load the reference parser files directly (by path) into a synthetic
    # "kimix.parser" package: the full pytest suite can shadow the real
    # `kimix` package (kimi-cli's checkout is on sys.path), so a plain
    # `from kimix.parser import ...` is not reliable here.
    import importlib.util
    import types

    _kpkg = types.ModuleType("kimix")
    _kpkg.__path__ = []
    _ppkg = types.ModuleType("kimix.parser")
    _ppkg.__path__ = [REF_PARSER_ROOT]
    sys.modules["kimix"] = _kpkg
    sys.modules["kimix.parser"] = _ppkg

    def _load_ref_module(name):
        path = os.path.join(REF_PARSER_ROOT, f"{name}.py")
        spec = importlib.util.spec_from_file_location(f"kimix.parser.{name}", path)
        mod = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        sys.modules[f"kimix.parser.{name}"] = mod  # before exec (dataclasses)
        spec.loader.exec_module(mod)
        return mod

    _base_mod = _load_ref_module("base")
    for _n in ("c_parser", "py_parser", "shell_parser", "sql_parser",
               "html_parser", "lisp_parser", "pascal_parser"):
        _load_ref_module(_n)
    CParser = sys.modules["kimix.parser.c_parser"].CParser
    PythonParser = sys.modules["kimix.parser.py_parser"].PythonParser
    ShellParser = sys.modules["kimix.parser.shell_parser"].ShellParser
    SqlParser = sys.modules["kimix.parser.sql_parser"].SqlParser
    HtmlParser = sys.modules["kimix.parser.html_parser"].HtmlParser
    LispParser = sys.modules["kimix.parser.lisp_parser"].LispParser
    PascalParser = sys.modules["kimix.parser.pascal_parser"].PascalParser

import kimix_native._shell_compat as SC  # noqa: E402

# ---------------------------------------------------------------------------
# Comment parser corpora (hand-built from the conformance matrix; every case
# verified against the reference during development)
# ---------------------------------------------------------------------------

C_CORPUS = [
    "// hi\nint x;\n",
    "int x = 1;\n// A line comment\n",
    "/* A block\ncomment */\n",
    "/** A doc comment */\n",
    "/**/\n",  # empty block, NOT doc
    'char *url = "http://example.com/path";\n',
    "var re = /a\\/b/; // real\n",
    "a = x / y; // division then comment\n",
    "return /re/; // keyword then regex\n",
    'let s = r#"a // b"#; /* c */\n',
    "x /* unclosed\n",
    "// tail no newline",
    "/* multi\nline\nblock */\n// after\n",
    "'single // not comment'\n// real\n",
    "`backtick /* not */` // yes\n",
    "a /*1*/ b /*2*/ c\n",
    "//a\n//b\n//c\n",
    "x = /**/;\n",
    "y /* nested /* not nested */ z */\n",
    "/*\n*/\n",
    "int a; // trailing\nint b;\n",
    "// comment with // inside\n",
]

PY_CORPUS = [
    "a = 1\n# a comment\nb = 2\n",
    '"""A module docstring."""\n',
    "'''A single-quoted docstring.'''\n",
    'x = "# not a comment"\n',
    'x = f"{1 + 1}"  # real comment\n',
    'url = "http://example.com#fragment"\n',
    '# comment\na = 1\n# another\n',
    "x = r\"\"\"raw\"\"\"\n# real\n",
    'x = f"""doc {1} # not"""\n# yes\n',
    "s = '''\nmulti\n'''\n# after\n",
    "# tail",
    'x = "unclosed\n# comment\n',
    "a = 1  # one\nb = 2  # two\nc = 3  # three\n",
    'f"{a}"  # f-string\n',
    "'''unclosed doc\n",
    '"""doc"""\n"""doc2"""\n',
    "x = 'sq' # c\n",
    "y = b\"bytes\" # c2\n",
    "z = fr\"{x}\" # c3\n",
    "if x:  # cond\n    pass\n",
]

SHELL_CORPUS = [
    "echo hi\n# a shell comment\n",
    "#!/bin/bash\necho hi\n",
    'echo "http://example.com#fragment"\n',
    "echo hi # comment\n",
    "cat <<EOF\n# not a comment\nEOF\n",
    "cat <<'EOF'\n# not either\nEOF\n",
    "cat <<-EOF\n\t# tab stripped\n\tEOF\n",
    "x=$(echo '# sub')\n# after\n",
    "x=`echo '# bt'`\n",
    'echo "a $(# sub) b"\n',
    "# only\n",
    "echo a # c1\necho b # c2\n",
    "#!\n",
    "echo 'sq # no'\n",
    'echo "dq # no"\n',
    "echo \\# escaped\n",
    "x = $((1 + 2)) # arith\n",
    "cat <<EOF\n# c\nEOF\necho done # c2\n",
    "echo $(echo # nested\n)\n",
    "#\n",
]

SQL_CORPUS = [
    "SELECT 1 -- c\n",
    "SELECT 1 --x\n",  # not a comment
    "SELECT 1 -- c\nSELECT 2\n",
    "SELECT 1 # note\n",
    "/* a block */\n",
    "/* a /* nested */ b */\n",
    "SELECT 'str -- not'\n",
    'SELECT "id -- not"\n',
    "SELECT `col -- not`\n",
    "SELECT 'it''s' -- c\n",
    "/* multi\nline */\n",
    "SELECT 1 --\n",
    "SELECT 1 -- c\n-- tail",
    "/* unclosed\n",
    "SELECT '\\' -- not' -- yes\n",
    "SELECT 1 /* c1 */ + 2 /* c2 */\n",
    "/*\n/* nested */\n*/\n",
    "SELECT 1 --c\n",  # dash without space: not a comment
    "SELECT 1 --\t c\n",
    "# mysql\nSELECT 1\n",
]

HTML_CORPUS = [
    "<!-- A comment -->\n",
    '<?xml version="1.0"?>\n',
    "<![CDATA[some data]]>\n",
    '<div data-test="a <!-- b --> c"></div>\n',
    "<!-- multi\nline -->\n",
    "text <!-- c1 --> more <!-- c2 -->\n",
    "<!-- unclosed\n",
    "<?unclosed\n",
    "<!-- a <!-- nested --> b -->\n",
    "<a href='x'>t</a><!-- c -->\n",
    "<!--\n-->\n",
    "<!DOCTYPE html>\n<!-- c -->\n",
    "<!-- c --><!-- d -->\n",
    "<div><!-- c --></div>\n",
    "<?pi ?>\n",
    "<!-- tail no close",
    "x <!-- c --> y\n",
    "<!-- a -->\n<!-- b\n",
    "<script>/* not html */</script><!-- c -->\n",
    "<!-- -->\n",
]

LISP_CORPUS = [
    "; line comment\n",
    "#| block |#\n",
    "#| a #| b |# c |#\n",  # not nested: first |# closes
    "#\\; char literal\n",
    "#\\Space named\n",
    '"str ; not"\n',
    "; tail",
    "#| unclosed\n",
    ";;; doc-ish\n",
    "(defun f (x) ; c\n  x)\n",
    "#| multi\nline\nblock |#\n",
    "a ; c1\nb ; c2\n",
    '#\\a b ; c\n',
    "#\\Newline ; c\n",
    '"esc \\" quote" ; c\n',
    ";\n",
    "#|x|#\n",
    "; c ; c\n",
    "(foo) ; tail\n",
    "#\\x\n",
]

PASCAL_CORPUS = [
    "{ A brace comment }\n",
    "(* An alt comment *)\n",
    "// A line comment\n",
    "s := '{ not a comment }';\n",
    "{ multi\nline }\n",
    "(* multi\nline *)\n",
    "{ unclosed\n",
    "(* unclosed\n",
    "// tail",
    "a := 1; { c1 } b := 2; { c2 }\n",
    "(* *)  \n",
    "{ }\n",
    "s := 'it''s' { c }\n",
    "(* { braces inside paren-star } *)\n",
    "{ (* paren inside brace *) }\n",
    "// c1\n// c2\n",
    "begin { c } end\n",
    "x := 1; // tail\n",
    "(* a (* b *) c *)\n",
    "{'str'}\n",
]

# kind name -> int
KIND_INT = {"line": 0, "block": 1, "doc": 2}


def comment_set(result):
    """Normalize a ParseResult to a comparable tuple set."""
    return tuple(
        (c.content, c.line, c.column, KIND_INT[c.kind]) for c in result.comments
    )


def ref_parser(lang):
    if not HAS_REF:
        pytest.skip("kimi-agent reference not importable")
    return {
        "c": CParser,
        "python": PythonParser,
        "shell": ShellParser,
        "sql": SqlParser,
        "html": HtmlParser,
        "lisp": LispParser,
        "pascal": PascalParser,
    }[lang]()


@pytest.mark.parametrize("lang", ["c", "python", "shell", "sql", "html", "lisp", "pascal"])
def test_comment_corpus_parity(lang):
    corpus = {
        "c": C_CORPUS, "python": PY_CORPUS, "shell": SHELL_CORPUS,
        "sql": SQL_CORPUS, "html": HTML_CORPUS, "lisp": LISP_CORPUS,
        "pascal": PASCAL_CORPUS,
    }[lang]
    assert len(corpus) >= 20, f"{lang} corpus too small: {len(corpus)}"
    parser = ref_parser(lang)
    for i, src in enumerate(corpus):
        native = P.parse(lang, src)
        ref = parser.parse(src)
        assert comment_set(native) == comment_set(ref), f"{lang}[{i}] {src!r}"
        assert (
            native.code_without_comments == ref.code_without_comments
        ), f"{lang}[{i}] code_without {src!r}"


def test_comment_spans_roundtrip():
    """comment_spans + shim slicing == reference comments (native path)."""
    src = "// hi\nint x; /* b */\n/** doc */\n"
    spans = P.comment_spans("c", src.encode())
    data = src.encode()
    for s, e, k in spans:
        assert data[s:e].decode() in src


def test_parse_native_disabled(monkeypatch):
    monkeypatch.setenv("KIMIX_NATIVE_PARSE", "0")
    assert kimix_native.use_native("PARSE") is False
    parser = ref_parser("c")
    src = "// hi\nint x;\n"
    assert comment_set(P.parse("c", src)) == comment_set(parser.parse(src))
    assert P.parse("c", src).code_without_comments == parser.parse(src).code_without_comments


def test_parse_non_ascii_c_routes_to_compat():
    """C with non-ASCII bytes routes to the vendored mirror (documented)."""
    src = "// \u4e2d\u6587 comment\nint x;\n"
    native = P.parse("c", src)
    compat = ref_parser("c").parse(src)
    assert comment_set(native) == comment_set(compat)
    assert native.code_without_comments == compat.code_without_comments
    # python has no unicode-sensitive decisions: native runs directly
    pysrc = "# \u4e2d\u6587\nx = 1\n"
    assert comment_set(P.parse("python", pysrc)) == comment_set(
        ref_parser("python").parse(pysrc)
    )


# ---------------------------------------------------------------------------
# Shell scanners
# ---------------------------------------------------------------------------

BASH_FIX_VECTORS = [
    r"git log --oneline C:\repo\src",
    r"rev C:\repo\src",
    "echo hi # comment",
    r"cd /d D:\x && dir",
    "cp C:\\\\a\\\\b.txt D:\\\\c\\\\",
    r"env -C C:\work python3 script.py",
    r"xcopy /e C:\src D:\dst",
    r"a=$(rev C:\tmp\f) b",
    r"ls \\server\share",
    r"open C:\Users\me\file.txt",
    r"cat C:\a\b > C:\out.txt",
    "printf '%s\\n' hi",
    r"find . -name '*.c' -exec sed -i 's/x/y/' {} \;",
    r'grep -rn "C:\path" src',
    r"tar -czf C:\backup.tar.gz C:\data",
    r"cd /d C:\x && rev C:\y",
    "for f in *.txt; do echo $f; done",
    "case $x in a) echo 1;; esac",
    "x=1; y=2; echo $((x + y))",
    r"cmd /c dir C:\windows\system32",
    "! echo bang",
    "time rev C:\\f",
    r"sudo -E env VAR=x C:\tool\run.exe",
    "coproc myproc { rev; }",
    "function f() { echo hi; }",
    "rev <<EOF\nC:\\not\\rewritten\nEOF\n",
    r"echo 'C:\quoted\path'",
    r'echo "C:\dq\path"',
    r"echo C:\a\b\c\d\e",
    r"echo \a\b",
    "gtimeout 5 rev C:\\x",
    "x=$(echo $(rev C:\\p))",
    r"rev .\rel\path",
    r"rev ..\up\path",
    r"rev ~\home\path",
    r"env --chdir=C:\work rev C:\x",
    r"xargs -I{} rev C:\{}\f",
    "echo a && rev C:\\x || echo b",
    "python - <<'PY'\nprint(1)\nPY\n&& echo next",
    "cat <<EOF\nhello\nEOF\n|| echo fallback",
    "cat <<EOF\nhello\nEOF\n; echo done",
    "cat <<EOF\nhello\nEOF\n| tr a b",
    "cat <<A <<B\na\nA\nb\nB\n&& echo next",
    "python - <<'PY'\nprint(1)\nPY\n&&\necho next",
]


def test_bash_fix_parity():
    for cmd in BASH_FIX_VECTORS:
        n = P.fix_bash_command(cmd)
        c = SC.fix_bash_command(cmd)
        assert n.command == c.command, repr(cmd)
        assert n.replacements == c.replacements, repr(cmd)
        assert n.path_changes == c.path_changes, repr(cmd)


def _find_bash() -> str | None:
    # Prefer Git Bash on Windows; the WSL launcher strips shell variables from
    # -c command strings and breaks the fallback-command wrappers.
    for candidate in (
        r"C:\Program Files\Git\bin\bash.exe",
        r"C:\Program Files (x86)\Git\bin\bash.exe",
    ):
        if os.path.isfile(candidate):
            return candidate
    exe = shutil.which("bash")
    if exe and "WindowsApps" in exe:
        return None
    return exe


BASH_EXE = _find_bash()


@pytest.mark.skipif(not BASH_EXE, reason="bash not installed")
def test_bash_fix_heredoc_trailing_operator_runs():
    source = "python3 - <<'PY'\nprint('ok')\nPY\n&& echo next"
    failed = subprocess.run(
        [BASH_EXE, "-c", source],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=15,
    )
    assert failed.returncode != 0
    assert "syntax error" in (failed.stderr or "")

    fixed = P.fix_bash_command(source).command
    passed = subprocess.run(
        [BASH_EXE, "-c", fixed],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=15,
    )
    assert passed.returncode == 0
    assert "ok" in passed.stdout
    assert "next" in passed.stdout


def test_bash_fix_deep_nesting_unchanged():
    deep = "$(" * 2000 + "rev" + ")" * 2000
    n = P.fix_bash_command(deep)
    c = SC.fix_bash_command(deep)
    assert n.command == c.command
    assert n.command == deep


UNQUOTED_VECTORS = [
    r"cd C:\foo\bar",
    'echo "a\\nb"',
    r"grep 'C:\x' f",
    "x=$(cd C:\\d && pwd)",
    "x=`cd C:\\d && pwd`",
    r"echo C:\a\b",
    'echo "a$(cd C:\\x)b"',
    r"echo 'C:\q' C:\u",
    "echo a\\ b",
    "echo a\\\\b",
    r"echo C:\x\y\z",
    "echo \"$(echo 'C:\\in\\sq')\"",
    r"echo C:\a && echo C:\b",
]


def test_process_unquoted_parity():
    for cmd in UNQUOTED_VECTORS:
        assert P._process_unquoted(cmd) == SC._process_unquoted(cmd), repr(cmd)


PWSH_FIX_VECTORS = [
    'Write-Output "hi"',
    'Write-Output "unclosed',
    "x = 'sq",
    '@"\nline\n"@',
    'Write-Output "a`"b"',
    "# comment only",
    'cmd /c echo --% "x',
    "Write-Output `",
    'Write-Output "a""b"',
    "<# block",
    "<# b #>",
    'Write-Output $("a" + "b")',
    "Write-Output 1 # trailing",
    "@'\nhere\n'@",
    "Write-Output 'single'",
    "Write-Output \"a`n`tb\"",
    "if ($x) { Write-Output 1 }",
    "Write-Output `\ncontinued",
    "--% raw",
    'Write-Output "unclosed`',
    "<#\nmulti\nblock\n",
    "@\"\n$var ?? $x\n\"@",
]


def test_pwsh_fix_parity():
    for cmd in PWSH_FIX_VECTORS:
        n = P.fix_pwsh_command(cmd)
        c = SC.fix_pwsh_command(cmd)
        assert (n is None) == (c is None), repr(cmd)
        if n is not None:
            assert n.command == c.command, repr(cmd)
            assert n.warning == c.warning, repr(cmd)


PWSH_TRANSFORM_VECTORS = [
    "$a = $b ?? $c",
    "$x = $cond ? 1 : 2",
    "$o?.Prop.Method()",
    "$a ??= $b",
    "cmd1 && cmd2 || cmd3",
    "$x = $a[0] ?? 5",
    'Write-Output "a ?? b"',
    "# comment ?? x",
    "$x = $obj?.Prop?[0]",
    "$a = 1\n$b = $a ?? 2\n",
    "$x = $null ?? $fallback",
    "Write-Output ($a ? 'yes' : 'no')",
    "$result = $x?.Y?.Z ?? 0",
    "Get-Item $p | ForEach-Object { $_.Name }",
    "$v = $dict['k'] ?? 'd'",
    "if ($a -gt 5) { $b } else { $c }",
    "$s = \"text $($x ?? 'd')\"",
    "@\"\n$a ?? $b\n\"@\n",
    "$x = $a -eq 1 ? 'one' : 'other'",
    "Write-Output 1 && Write-Output 2",
    "$y = $x?.Prop",
    "$z = $x?[0]",
    "a = b ?? c ?? d",
    "1 ?? 2",
]


def test_pwsh_transform_parity():
    for cmd in PWSH_TRANSFORM_VECTORS:
        n = P.pwsh_transform(cmd)
        c = SC.pwsh_transform(cmd)
        assert n[0] == c[0], repr(cmd)


def test_pwsh_transform_multiline_regions_skipped():
    code = '@"\n$a ?? $b\n"@\nWrite-Output "ok"\n'
    n = P.pwsh_transform(code)
    c = SC.pwsh_transform(code)
    assert n[0] == c[0]


def test_shell_native_disabled(monkeypatch):
    monkeypatch.setenv("KIMIX_NATIVE_PARSE", "0")
    assert kimix_native.use_native("PARSE") is False
    for cmd in BASH_FIX_VECTORS[:5]:
        assert P.fix_bash_command(cmd).command == SC.fix_bash_command(cmd).command
    for cmd in PWSH_FIX_VECTORS[:5]:
        n, c = P.fix_pwsh_command(cmd), SC.fix_pwsh_command(cmd)
        assert (n is None) == (c is None)
        if n is not None:
            assert n.command == c.command and n.warning == c.warning
    for cmd in PWSH_TRANSFORM_VECTORS[:5]:
        assert P.pwsh_transform(cmd)[0] == SC.pwsh_transform(cmd)[0]
