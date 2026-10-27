#!/usr/bin/env python3
"""detect_unity_build.py — detect unity-build (jumbo build) hazards in the sources.

Several targets merge translation units into one unity batch (e.g. kimix-graph-db,
kimix_api with batch_size = 8 — see src/api/xmake.lua and _config_project in
scripts/xmake_func.lua). In a merged translation unit, things that are harmless
in separate TUs become hard errors or ODR violations. This script scans the
first-party sources (src/ except src/ext, plus tests/) and reports:

ERROR
  anonymous-namespace  `namespace { ... }` (forbidden by AGENTS.md; collides
                       across merged TUs; in a header it is an ODR violation)
  dup-function         same qualified function name + same parameter list
                       defined in more than one file with different bodies
                       (or non-inline identical bodies => multiple definition)
  dup-variable         same namespace-scope variable name defined with external
                       linkage in more than one file
  dup-type             same struct/class/union/enum name or type alias defined
                       in more than one file (identical bodies still error once
                       merged into one TU)
  macro-redefinition   same macro name defined with different bodies in
                       different files (the merged TU sees both)
WARNING
  macro-leak           #define without a matching #undef in a .cpp/.inc
                       (leaks into the rest of the unity batch; include guards
                       are exempt)
  using-directive      `using namespace X;` at file scope (pollutes the merged TU)
  anonymous-namespace-block  anonymous namespace at block scope (legal and
                       collision-safe, but forbidden by the project rule)

INFO (shown only with --verbose)
  Findings inside tests/ (every test file is its own single-TU target, so
  nothing unity-merges there) and macros a header defines for its callers.

Static (internal-linkage) helpers are collision-safe and not reported, and
`main` is exempt (one per binary target; unity batches never span targets).
Findings whose files are all under tests/ are downgraded: every test file is
its own single-TU target there, so nothing ever unity-merges — they are
reported as WARNING/INFO instead of ERROR.

Vendored single-file libraries that live outside src/ext are skipped:
src/core/lmdb/ (LMDB), src/core/detail/concurrent_queue.h (moodycamel),
tests/ut/ut.hpp (Boost.UT).

Usage:
  python scripts/detect_unity_build.py [--root DIR] [--json OUT] [--quiet]
                                       [--verbose]

Flags:
  --root   project root to scan (default: the repo root, i.e. the parent of
           the scripts/ directory containing this file).
  --json   also write the findings as a JSON array to OUT.
  --quiet  print only the summary line (findings still set the exit code).
  --verbose  also print INFO findings (test-file idioms such as
           `using namespace boost::ut;`).

Exit codes: 0 = no ERROR findings, 1 = one or more ERROR findings,
2 = internal failure (bad --root, unreadable file, ...).
"""
import argparse
import io
import json
import os
import re
import sys

CPP_EXTS = (".cpp", ".cc", ".cxx", ".h", ".hpp", ".inc")
HEADER_EXTS = (".h", ".hpp")

# Vendored single-file libraries that do not live under src/ext.
VENDORED_PREFIXES = (
    "src/core/lmdb/",
    "src/core/detail/",
    "tests/ut/",
)


def is_vendored(rel):
    return rel.startswith(VENDORED_PREFIXES)

IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
STRING_PREFIXES = {"R", "L", "u8", "u", "U", "LR", "u8R", "uR", "UR"}
CHAR_PREFIXES = {"L", "u8", "u", "U"}

# tokens allowed between a function's ')' and its '{' (trailing return type,
# qualifiers); scanning back from '{' stops at the first token not in here or
# not covered by the generic rules below
FUNC_TAIL_IDS = {"const", "noexcept", "volatile", "override", "final",
                 "mutable", "requires"}
CONTROL_IDS = {"if", "for", "while", "switch", "catch", "return", "sizeof",
               "static_cast", "reinterpret_cast", "const_cast", "dynamic_cast",
               "new", "delete", "decltype", "alignof", "co_await", "co_return",
               "co_yield", "throw", "case", "and", "or", "not"}
TYPE_KEYWORDS = {"struct", "class", "union", "enum"}
STORAGE_INTERNAL = {"static", "const", "constexpr"}
ALIAS_KEYWORDS = {"typedef", "using"}


# ---------------------------------------------------------------------------
# Lexer
# ---------------------------------------------------------------------------

def scan_string(text, q, raw):
    """text[q] == '"'. Returns index past the closing quote."""
    n = len(text)
    if raw:
        p = q + 1
        while p < n and text[p] not in '("' and p - q <= 18:
            p += 1
        if p >= n or text[p] != "(":
            return q + 1
        delim = text[q + 1:p]
        close = ")" + delim + '"'
        j = text.find(close, p + 1)
        return n if j == -1 else j + len(close)
    p = q + 1
    while p < n:
        if text[p] == "\\":
            p += 2
            continue
        if text[p] == '"':
            return p + 1
        p += 1
    return n


def scan_char(text, q):
    """text[q] == "'". Returns index past the closing quote."""
    n = len(text)
    p = q + 1
    while p < n:
        if text[p] == "\\":
            p += 2
            continue
        if text[p] == "'":
            return p + 1
        if text[p] == "\n":
            return q + 1
        p += 1
    return n


def lex(text):
    """Tokenize into a list of (kind, start, end).

    kinds: 'id', 'num', 'punct', 'comment', 'string', 'preproc'.
    Whitespace is skipped; preprocessor lines (# ... through end of line,
    with line continuations) are returned as single 'preproc' tokens.
    """
    toks = []
    n = len(text)
    i = 0
    at_line_start = True
    while i < n:
        c = text[i]
        if c in " \t\r\n\f\v":
            if c == "\n":
                at_line_start = True
            i += 1
            continue
        # preprocessor line: '#' as first non-space char of a line
        if c == "#" and at_line_start:
            j = i
            while True:
                k = text.find("\n", j)
                if k == -1:
                    j = n
                    break
                # line continuation?
                back = 0
                p = k - 1
                while p >= 0 and text[p] == "\\":
                    back += 1
                    p -= 1
                if back % 2 == 1:
                    j = k + 1
                    continue
                j = k
                break
            toks.append(("preproc", i, j))
            i = j
            continue
        at_line_start = False
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j == -1 else j
            toks.append(("comment", i, j))
            i = j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j == -1 else j + 2
            toks.append(("comment", i, j))
            i = j
            continue
        if c.isalpha() or c == "_":
            m = IDENT_RE.match(text, i)
            word = m.group(0)
            end = m.end()
            if word in STRING_PREFIXES and end < n and text[end] == '"':
                j = scan_string(text, end, raw=word.endswith("R"))
                toks.append(("string", i, j))
                i = j
                continue
            if word in CHAR_PREFIXES and end < n and text[end] == "'":
                j = scan_char(text, end)
                toks.append(("string", i, j))
                i = j
                continue
            toks.append(("id", i, end))
            i = end
            continue
        if c.isdigit() or (c == "." and i + 1 < n and text[i + 1].isdigit()):
            m = re.compile(r"[0-9A-Za-z_.']+").match(text, i)
            toks.append(("num", i, m.end()))
            i = m.end()
            continue
        if c == "'":
            j = scan_char(text, i)
            toks.append(("string", i, j))
            i = j
            continue
        if c == '"':
            j = scan_string(text, i, raw=False)
            toks.append(("string", i, j))
            i = j
            continue
        toks.append(("punct", i, i + 1))
        i += 1
    return toks


# ---------------------------------------------------------------------------
# Per-file analysis
# ---------------------------------------------------------------------------

class FileFindings:
    def __init__(self, relpath, text, toks):
        self.relpath = relpath
        self.is_header = relpath.lower().endswith(HEADER_EXTS)
        self.text = text
        self.toks = toks
        self.line_starts = [0]
        for m in re.finditer("\n", text):
            self.line_starts.append(m.end())
        self.anon_ns = []        # (line, block_scope)
        self.functions = []      # dict(name, params, body, external, inline, line)
        self.variables = []      # dict(name, external, line)
        self.types = []          # dict(name, kind, line)
        self.macros = {}         # name -> (line, body)
        self.using_directives = []  # (line, namespace)

    def line_of(self, pos):
        lo, hi = 0, len(self.line_starts)
        while lo + 1 < hi:
            mid = (lo + hi) // 2
            if self.line_starts[mid] <= pos:
                lo = mid
            else:
                hi = mid
        return lo + 1

    def tok_text(self, idx):
        s, e = self.toks[idx][1], self.toks[idx][2]
        return self.text[s:e]


def norm_span(text):
    return " ".join(text.split())


def analyze_file(relpath, text):
    toks = lex(text)
    ff = FileFindings(relpath, text, toks)
    n = len(toks)
    code = [i for i in range(n) if toks[i][0] in ("id", "num", "punct")]

    def prev_code(ci):
        """code-list index of previous code token, or -1."""
        return ci - 1

    # bracket matching for (), [], {}
    paren_match = {}
    brackets = []
    for i in range(n):
        kind, s, e = toks[i]
        if kind != "punct":
            continue
        ch = text[s]
        if ch in "([{":
            brackets.append((ch, i))
        elif ch in ")]}":
            if not brackets:
                continue
            och, oi = brackets.pop()
            if (och, ch) in (("(", ")"), ("[", "]"), ("{", "}")):
                paren_match[oi] = i
                paren_match[i] = oi
    # fall back: ignore unbalanced tails
    code_set = set(code)

    # brace classification stack
    stack = []  # entries: dict(kind=..., name=...)
    brace_kind = {}

    def is_decl_level():
        return all(e["kind"] in ("ns", "linkage") for e in stack)

    def find_code_before(ci):
        j = ci - 1
        while j >= 0 and j not in code_set:
            j -= 1
        return j

    def find_code_after(ci):
        j = ci + 1
        while j < n and j not in code_set:
            j += 1
        return j

    # code-list based walk over braces
    brace_stack = []
    i_code = 0
    # We walk the token stream; maintain mapping token->code handled inline.

    ci = 0
    while ci < len(code):
        ti = code[ci]
        kind, s, e = toks[ti]
        ch = text[s]
        if kind == "punct" and ch == "{":
            # classify this brace
            pj = find_code_before(ti)
            pk = toks[pj][0] if pj >= 0 else None
            pt = ff.tok_text(pj) if pj >= 0 else ""
            entry = {"kind": "block", "name": None}
            # type-definition brace: the keyword (struct/class/union/enum) may
            # be several tokens back:  struct S {  /  class A : public B {
            #  /  enum class E : int {
            type_kw = None
            j = pj
            for _ in range(6):
                if j < 0:
                    break
                jk2 = toks[j][0]
                jt2 = ff.tok_text(j)
                if jk2 == "id" and jt2 in TYPE_KEYWORDS:
                    type_kw = jt2
                    break
                if jk2 == "punct" and jt2 in ";{}()=[]":
                    break
                j = find_code_before(j)
            if pk == "id" and pt == "namespace":
                nj = find_code_after(ti)
                name = None
                if nj < n and toks[nj][0] == "id":
                    name = ff.tok_text(nj)
                if is_decl_level():
                    ff.anon_ns.append((ff.line_of(s), False))
                else:
                    ff.anon_ns.append((ff.line_of(s), True))
                entry = {"kind": "ns", "name": name}
            elif pk == "id" and type_kw is not None:
                # named type definition: struct/class/union/enum Name { ... }
                # (forward declarations end in ';' and never reach this brace)
                pt = type_kw
                kw_ti = pj
                while kw_ti >= 0 and ff.tok_text(kw_ti) != pt:
                    kw_ti = find_code_before(kw_ti)
                name = None
                # the name sits between the keyword and the brace:
                #   struct SharedType {      enum class E : int {
                ids_between = []
                j = find_code_after(kw_ti)
                guard = 0
                while j < ti and guard < 8:
                    if toks[j][0] == "id":
                        ids_between.append(j)
                    j = find_code_after(j)
                    guard += 1
                if ids_between:
                    cand = 0
                    if pt == "enum" and ff.tok_text(ids_between[0]) in ("class", "struct"):
                        cand = 1
                    if cand < len(ids_between):
                        name = ff.tok_text(ids_between[cand])
                if name and is_decl_level():
                    ns_parts = [en["name"] for en in stack
                                if en["kind"] == "ns" and en["name"]]
                    qname = "::".join(ns_parts + [name]) if ns_parts else name
                    ff.types.append({"name": qname, "kind": pt,
                                     "line": ff.line_of(toks[ti][1])})
                entry = {"kind": "type", "name": name}
            elif pk == "id" and pt == "extern":
                # extern "C" { ... }
                entry = {"kind": "linkage", "name": None}
            elif pk == "punct" and pt == ":" and is_decl_level():
                # enum E : int { ... }
                entry = {"kind": "type", "name": None}
            elif is_decl_level():
                # maybe a function definition: scan back over the tail allowlist
                j = pj
                steps = 0
                while j >= 0 and steps < 24:
                    jk = toks[j][0]
                    jt = ff.tok_text(j)
                    if jk == "punct" and jt == ")":
                        break
                    if jk == "id" and jt in FUNC_TAIL_IDS:
                        j -= 1
                        steps += 1
                        continue
                    if jk == "punct" and jt in ("&", "->", "::", "<", ">"):
                        j -= 1
                        steps += 1
                        continue
                    if jk in ("id", "num"):
                        # trailing return type name e.g. `-> std::string`
                        j -= 1
                        steps += 1
                        continue
                    break
                if j >= 0 and toks[j][0] == "punct" and ff.tok_text(j) == ")":
                    oj = paren_match.get(j)
                    if oj is not None:
                        name_j = find_code_before(oj)
                        if name_j >= 0 and toks[name_j][0] == "id":
                            fname = ff.tok_text(name_j)
                            # qualified name:  A::B::fname
                            qj = name_j
                            parts = [fname]
                            while True:
                                c1 = find_code_before(qj)
                                if c1 >= 0 and toks[c1][0] == "punct" and ff.tok_text(c1) == "::":
                                    c2 = find_code_before(c1)
                                    if c2 >= 0 and toks[c2][0] == "id":
                                        parts.append(ff.tok_text(c2))
                                        qj = c2
                                        continue
                                break
                            qname = "::".join(reversed(parts))
                            # template-id name?  name<...>(
                            tj = find_code_before(qj)
                            if tj >= 0 and toks[tj][0] == "punct" and ff.tok_text(tj) == ">":
                                lt = paren_match.get(tj)
                                if lt is not None and find_code_before(lt) == qj:
                                    pass  # template-id; name already captured
                            # storage / inline specifiers before the name
                            ext = True
                            inline = False
                            sj = tj if tj >= 0 else find_code_before(qj)
                            scan = find_code_before(qj)
                            guard = 0
                            while scan >= 0 and guard < 16:
                                st = ff.tok_text(scan)
                                if st == "static":
                                    ext = False
                                elif st in ("inline", "constexpr", "friend"):
                                    inline = True
                                elif st in (";", "}", "{"):
                                    break
                                scan = find_code_before(scan)
                                guard += 1
                            if fname != "operator" and fname not in CONTROL_IDS \
                                    and qname != "main":
                                cclose = paren_match.get(ti)
                                body = ""
                                if cclose is not None:
                                    body = norm_span(text[s:toks[cclose][2]])
                                cj = paren_match.get(oj)
                                if cj is not None:
                                    params = norm_span(text[toks[oj][1]:toks[cj][2]])
                                else:
                                    params = ""
                                ff.functions.append({
                                    "name": qname, "params": params, "body": body,
                                    "external": ext, "inline": inline,
                                    "line": ff.line_of(s),
                                })
                            entry = {"kind": "func", "name": qname}
                else:
                    entry = {"kind": "init", "name": None}
            else:
                entry = {"kind": "block", "name": None}
            brace_stack.append((ti, entry))
            brace_kind[ti] = entry
            stack = [en for _, en in brace_stack]
        elif kind == "punct" and ch == "}":
            if brace_stack:
                brace_stack.pop()
            stack = [en for _, en in brace_stack]
        elif kind == "punct" and ch == ";":
            # statement boundary at declaration level: parse declarators
            if is_decl_level():
                parse_declaration(ff, ti, find_code_before, find_code_after,
                                  paren_match)
        ci += 1

    detect_macros(ff)
    return ff


def parse_declaration(ff, semi_ti, find_code_before, find_code_after,
                      paren_match, max_lookback=48):
    """At a declaration-level ';', look back for variable / alias declarations."""
    toks = ff.toks
    text = ff.text
    # gather the statement's code tokens back to a boundary
    stmt = []
    j = find_code_before(semi_ti)
    guard = 0
    while j >= 0 and guard < max_lookback:
        jt = ff.tok_text(j)
        jk = toks[j][0]
        if jk == "punct" and jt in (";", "{", "}"):
            break
        stmt.append(j)
        j = find_code_before(j)
        guard += 1
    stmt.reverse()
    if not stmt:
        return
    first = ff.tok_text(stmt[0])
    if first in TYPE_KEYWORDS or first == "template":
        return  # handled at the brace site
    if first in ("using",):
        # using X = ...;  or  using enum X; / using X::y;
        if len(stmt) >= 2 and toks[stmt[1]][0] == "id" and first == "using":
            if ff.tok_text(stmt[1]) == "namespace":
                # using namespace X; or using namespace A::B;
                parts = [ff.tok_text(k) for k in stmt[2:]
                         if toks[k][0] == "id"]
                ff.using_directives.append((ff.line_of(toks[semi_ti][1]),
                                            "::".join(parts)))
                return
            if len(stmt) >= 3 and ff.tok_text(stmt[2]) == "=":
                ff.types.append({"name": ff.tok_text(stmt[1]), "kind": "alias",
                                 "line": ff.line_of(toks[stmt[0]][1])})
                return
            return  # using-declaration (imports an existing name) — safe
    if first == "typedef":
        # typedef <type...> Name;  (possibly pointers) — last id is the alias.
        # `typedef struct foo foo;` (opaque-handle forward declaration) is
        # not an alias hazard — the real definition carries the same name.
        ids = [k for k in stmt if toks[k][0] == "id"]
        if ids:
            k = ids[-1]
            name = ff.tok_text(k)
            self_alias = len(ids) >= 2 and ff.tok_text(ids[-2]) == name
            if not self_alias and name not in STORAGE_INTERNAL \
                    and name not in TYPE_KEYWORDS:
                ff.types.append({"name": name, "kind": "alias",
                                 "line": ff.line_of(toks[k][1])})
        return
    if first in ("return", "static_assert", "extern", "friend", "asm",
                 "co_return", "co_yield", "namespace"):
        # `namespace a = b;` aliases may be redeclared to the same target and
        # are legal in a merged TU; not a hazard.
        return
    # variable declaration(s): <type tokens...> declarator (, declarator)* ;
    # find storage specifiers
    is_static = any(ff.tok_text(k) == "static" for k in stmt)
    is_const = any(ff.tok_text(k) in ("const", "constexpr") for k in stmt)
    is_extern = any(ff.tok_text(k) == "extern" for k in stmt)
    external = not (is_static or is_const)
    # split top-level commas and extract declarator names
    depth = 0
    group = []
    groups = []
    for k in stmt:
        jt = ff.tok_text(k)
        jk = toks[k][0]
        if jk == "punct" and jt in "([<":
            depth += 1
        elif jk == "punct" and jt in ")]>":
            depth = max(0, depth - 1)
        if jk == "punct" and jt == "," and depth == 0:
            groups.append(group)
            group = []
            continue
        group.append(k)
    groups.append(group)
    for g in groups:
        # walk the group tracking bracket depth to find top-level structure
        depth = 0
        top_eq = None       # token index of a top-level '='
        first_paren = None  # token index of the first top-level '('
        fnptr_name = None   # token index of a function-pointer name:  (*name)
        for k in g:
            jt = ff.tok_text(k)
            jk = toks[k][0]
            if jk == "punct" and jt in "([<":
                if depth == 0:
                    if jt == "(" and first_paren is None:
                        first_paren = k
                        nk = find_code_after(k)
                        n2 = find_code_after(nk) if nk < len(toks) else -1
                        if nk < len(toks) and toks[nk][0] == "punct" \
                                and ff.tok_text(nk) == "*" \
                                and n2 < len(toks) and toks[n2][0] == "id":
                            fnptr_name = n2
                    if jt == "=":
                        top_eq = k
                depth += 1
            elif jk == "punct" and jt in ")]>":
                depth = max(0, depth - 1)
            elif depth == 0 and jk == "punct" and jt == "=":
                top_eq = k
        if first_paren is not None:
            if fnptr_name is not None:
                name_idx = fnptr_name  # function-pointer variable
            elif top_eq is not None:
                # variable initialized by a call/lambda:  int x = foo();
                pk = find_code_before(top_eq)
                while pk >= 0 and (toks[pk][0] != "id"
                                   or ff.tok_text(pk) in STORAGE_INTERNAL):
                    pk = find_code_before(pk)
                if pk < 0:
                    continue
                name_idx = pk
            else:
                continue  # function declaration — params are not variables
        else:
            ids = [k for k in g if toks[k][0] == "id"
                   and ff.tok_text(k) not in STORAGE_INTERNAL
                   and ff.tok_text(k) not in TYPE_KEYWORDS]
            if not ids:
                continue
            name_idx = ids[-1]
            if top_eq is not None and name_idx > top_eq:
                name_idx = None
                for k in reversed(ids):
                    if k < top_eq:
                        name_idx = k
                        break
                if name_idx is None:
                    continue
        name = ff.tok_text(name_idx)
        # lone ALL_CAPS identifier statement (e.g. KIMIX_FFI_BEGIN;) is a bare
        # macro invocation, not a variable
        if (first_paren is None and top_eq is None and len(g) == 1
                and name.upper() == name):
            continue
        ff.variables.append({"name": name, "external": external,
                             "line": ff.line_of(toks[name_idx][1])})


DEFINE_RE = re.compile(r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)(.*)$")
UNDEF_RE = re.compile(r"^\s*#\s*undef\s+([A-Za-z_][A-Za-z0-9_]*)")
IFNDEF_RE = re.compile(r"^\s*#\s*ifndef\s+([A-Za-z_][A-Za-z0-9_]*)")


def detect_macros(ff):
    """Record #define ... without a matching #undef (include guards exempt)."""
    lines = ff.text.split("\n")
    ifndefs = set()
    for ln in lines:
        m = IFNDEF_RE.match(ln)
        if m:
            ifndefs.add(m.group(1))
    undefs = set()
    for ln in lines:
        m = UNDEF_RE.match(ln)
        if m:
            undefs.add(m.group(1))
    for idx, ln in enumerate(lines):
        m = DEFINE_RE.match(ln)
        if not m:
            continue
        name, body = m.group(1), m.group(2).strip()
        if name in ifndefs:
            continue  # include guard
        if name in undefs:
            continue
        if name.startswith("_") and name.upper() == name:
            continue  # reserved/guard-style macro
        ff.macros.setdefault(name, (idx + 1, body))


# ---------------------------------------------------------------------------
# Cross-file aggregation
# ---------------------------------------------------------------------------

FINDING_ORDER = ["anonymous-namespace", "dup-function", "dup-variable",
                 "dup-type", "macro-redefinition", "macro-leak",
                 "using-directive", "anonymous-namespace-block"]


def derive_name(rel):
    base = os.path.splitext(rel)[0]
    name = re.sub(r"[^0-9A-Za-z_]", "_", base)
    if name and name[0].isdigit():
        name = "ns_" + name
    return name


def in_tests(rel):
    return rel.startswith("tests/")


def aggregate(file_findings):
    findings = []  # dict(severity, category, message, file, line)

    def add(sev, cat, message, file, line):
        findings.append({"severity": sev, "category": cat, "message": message,
                         "file": file, "line": line})

    # --- anonymous namespaces ---
    for ff in file_findings:
        for line, block in ff.anon_ns:
            if block:
                add("WARNING", "anonymous-namespace-block",
                    "anonymous namespace at block scope (legal, but forbidden "
                    "by the project rule)", ff.relpath, line)
            elif ff.is_header:
                add("ERROR", "anonymous-namespace",
                    "anonymous namespace in a header (ODR violation; use a "
                    "named namespace)", ff.relpath, line)
            else:
                add("ERROR", "anonymous-namespace",
                    "anonymous namespace (collides when TUs are unity-merged; "
                    "use a file-unique named namespace, see AGENTS.md)",
                    ff.relpath, line)

    # --- duplicate functions ---
    by_key = {}
    for ff in file_findings:
        for fn in ff.functions:
            by_key.setdefault((fn["name"], fn["params"]), []).append((ff, fn))
    for (name, params), defs in sorted(by_key.items()):
        # internal-linkage (static) definitions never collide across files;
        # only external-linkage duplicates are a hazard
        sub = [(ff, fn) for ff, fn in defs if fn["external"]]
        files = {ff.relpath for ff, _ in sub}
        if len(files) < 2:
            continue
        sev = "ERROR"
        if all(in_tests(ff.relpath) for ff, _ in sub):
            sev = "WARNING"  # every test file is its own single-TU target
        bodies = {fn["body"] for _, fn in sub}
        any_inline = any(fn["inline"] for _, fn in sub)
        loc = "; ".join("%s:%d" % (ff.relpath, fn["line"])
                        for ff, fn in sub[:4])
        if len(bodies) > 1:
            add(sev, "dup-function",
                "function '%s%s' defined with different bodies in %d "
                "files (%s) — redefinition/ODR error in a unity batch"
                % (name, params, len(files), loc),
                sub[0][0].relpath, sub[0][1]["line"])
        elif not any_inline:
            add(sev, "dup-function",
                "external function '%s%s' defined in %d files (%s) — "
                "multiple definition even without unity"
                % (name, params, len(files), loc),
                sub[0][0].relpath, sub[0][1]["line"])
        # identical bodies + inline: legal ODR, not reported

    # --- duplicate variables (external linkage) ---
    by_name = {}
    for ff in file_findings:
        for var in ff.variables:
            if var["external"]:
                by_name.setdefault(var["name"], []).append((ff, var))
    for name, defs in sorted(by_name.items()):
        files = {ff.relpath for ff, _ in defs}
        if len(files) < 2:
            continue
        sev = "ERROR"
        if all(in_tests(ff.relpath) for ff, _ in defs):
            sev = "WARNING"
        loc = "; ".join("%s:%d" % (ff.relpath, var["line"]) for ff, var in defs[:4])
        add(sev, "dup-variable",
            "variable '%s' defined with external linkage in %d files (%s) — "
            "multiple definition once merged" % (name, len(files), loc),
            defs[0][0].relpath, defs[0][1]["line"])

    # --- duplicate types / aliases ---
    by_name = {}
    for ff in file_findings:
        for tp in ff.types:
            by_name.setdefault(tp["name"], []).append((ff, tp))
    for name, defs in sorted(by_name.items()):
        files = {ff.relpath for ff, _ in defs}
        if len(files) < 2:
            continue
        sev = "ERROR"
        if all(in_tests(ff.relpath) for ff, _ in defs):
            sev = "WARNING"
        kinds = {tp["kind"] for _, tp in defs}
        loc = "; ".join("%s:%d" % (ff.relpath, tp["line"]) for ff, tp in defs[:4])
        add(sev, "dup-type",
            "%s '%s' defined in %d files (%s) — redefinition error in a "
            "unity batch" % ("/".join(sorted(kinds)), name, len(files), loc),
            defs[0][0].relpath, defs[0][1]["line"])

    # --- macros ---
    by_name = {}
    for ff in file_findings:
        for name, (line, body) in ff.macros.items():
            by_name.setdefault(name, []).append((ff, line, body))
    for name, defs in sorted(by_name.items()):
        bodies = {body for _, _, body in defs}
        files = {ff.relpath for ff, _, _ in defs}
        if len(files) < 2:
            continue
        if len(bodies) > 1:
            loc = "; ".join("%s:%d" % (ff.relpath, line) for ff, line, _ in defs[:4])
            add("ERROR", "macro-redefinition",
                "macro '%s' defined with different bodies in %d files (%s)"
                % (name, len(files), loc),
                defs[0][0].relpath, defs[0][1])
        else:
            all_files = {x.relpath for x, _, _ in defs}
            for ff, line, _ in defs:
                sev = "WARNING" if not ff.is_header else "INFO"
                if in_tests(ff.relpath):
                    sev = "INFO"
                add(sev, "macro-leak",
                    "macro '%s' defined here and also in %s without #undef — "
                    "leaks across the unity batch"
                    % (name, ", ".join(sorted(all_files - {ff.relpath}))),
                    ff.relpath, line)

    # --- macros defined without #undef in a single file ---
    for ff in file_findings:
        for name, (line, body) in sorted(ff.macros.items()):
            everywhere = by_name.get(name, [])
            other_files = {f.relpath for f, _, _ in everywhere} - {ff.relpath}
            if not other_files:
                # headers/.inc are included, not unity-merged: their macros are
                # visible by design, so this is informational there; a macro
                # defined in a .cpp really does leak into the batch it joins.
                sev = "WARNING" if not ff.is_header else "INFO"
                if in_tests(ff.relpath):
                    sev = "INFO"
                add(sev, "macro-leak",
                    "macro '%s' defined without #undef — leaks into later TUs "
                    "of the unity batch" % name, ff.relpath, line)

    # --- using directives at file scope ---
    for ff in file_findings:
        own = derive_name(ff.relpath)
        for line, ns in ff.using_directives:
            if ns == own:
                continue  # the file's own converted namespace (AGENTS.md pattern)
            where = "header" if ff.is_header else "file"
            sev = "INFO" if in_tests(ff.relpath) else "WARNING"
            add(sev, "using-directive",
                "`using namespace %s;` at %s scope pollutes a merged TU"
                % (ns, where), ff.relpath, line)

    order = {cat: i for i, cat in enumerate(FINDING_ORDER)}
    findings.sort(key=lambda f: (0 if f["severity"] == "ERROR" else 1,
                                 order.get(f["category"], 99),
                                 f["file"], f["line"]))
    return findings


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def collect_files(root):
    out = []
    for base in ("src", "tests"):
        base_dir = os.path.join(root, base)
        for dirpath, dirnames, filenames in os.walk(base_dir):
            dirnames[:] = [d for d in dirnames
                           if not (base == "src" and d == "ext")]
            for fn in sorted(filenames):
                if fn.endswith(CPP_EXTS):
                    rel = os.path.relpath(os.path.join(dirpath, fn), root)
                    rel = rel.replace(os.sep, "/")
                    if is_vendored(rel):
                        continue
                    out.append(rel)
    return sorted(out)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Detect unity-build hazards (anonymous namespaces, "
                    "duplicate cross-TU definitions, leaky macros).")
    ap.add_argument("--root", default=None,
                    help="project root (default: repo root, the parent of "
                         "scripts/)")
    ap.add_argument("--json", dest="json_path", default=None,
                    help="also write findings as JSON to this path")
    ap.add_argument("--quiet", action="store_true",
                    help="print only the summary line")
    ap.add_argument("--verbose", action="store_true",
                    help="also print INFO findings")
    args = ap.parse_args(argv)

    # messages contain non-ASCII punctuation; on a legacy console code page
    # (e.g. cp936) printing must not crash the run
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass  # Python < 3.7 or a redirected stream

    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    root = os.path.abspath(root)
    if not os.path.isdir(os.path.join(root, "src")):
        print("error: --root %s does not look like the repo root (no src/)" % root,
              file=sys.stderr)
        return 2

    files = collect_files(root)
    file_findings = []
    for rel in files:
        path = os.path.join(root, rel)
        try:
            with io.open(path, "r", encoding="utf-8", errors="replace",
                         newline="") as f:
                text = f.read()
        except OSError as ex:
            print("error: cannot read %s: %s" % (rel, ex), file=sys.stderr)
            return 2
        file_findings.append(analyze_file(rel.replace(os.sep, "/"), text))

    findings = aggregate(file_findings)

    errors = sum(1 for f in findings if f["severity"] == "ERROR")
    warnings = sum(1 for f in findings if f["severity"] == "WARNING")
    infos = sum(1 for f in findings if f["severity"] == "INFO")

    if not args.quiet:
        for f in findings:
            if f["severity"] == "INFO" and not args.verbose:
                continue
            print("[%s] %s: %s:%d: %s" % (f["severity"], f["category"],
                                          f["file"], f["line"], f["message"]))
    print("== %d file(s) scanned: %d error(s), %d warning(s), %d info =="
          % (len(files), errors, warnings, infos))

    if args.json_path:
        try:
            with io.open(args.json_path, "w", encoding="utf-8") as f:
                json.dump(findings, f, indent=2, sort_keys=True)
                f.write("\n")
        except OSError as ex:
            print("error: cannot write %s: %s" % (args.json_path, ex),
                  file=sys.stderr)
            return 2

    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
