# Blocker: `grep_regex` (Phase B) requires PCRE2 — not vendored, cannot add

Tool: `grep` · worktree `C:/dev/kimix_wt/grep` · branch `agent/grep`

## What is blocked

`plans/grep.md` §3 kernel 7 (`grep_regex.h/.cpp`, Phase B): the native regex
line-search kernel for the pure-Python fallback path (`backup_grep` /
`_search_content_single`). It needs a regex engine whose semantics are
byte-exact with the Python `regex` module for the patterns the tool accepts.

The tool entry point for regex matching returns `tool_status::unsupported` so
the Python shim keeps the exact Python matcher — the same ASCII-gate routing
convention used by `grep_pattern.*` and `security.*`. No silent `std::regex`
substitution anywhere.

## Why the task cannot finish without PCRE2

Python's `regex` module (what `backup_grep` compiles with, IGNORECASE/DOTALL)
supports, in real agent traffic:

- lookaround (lookahead **and** variable-width lookbehind),
- backreferences (`\1`, `\g<name>`),
- atomic groups `(?>…)` and possessive quantifiers `*+`, `++`,
- Unicode properties `\p{Lu}`, `\p{Script=Cyrillic}`, category-aware `\d`/`\w`/`\s`,
- recursive patterns, `\X` grapheme clusters, `\R` line breaks,
- Python-specific corners (`re`-compatible `\b` semantics, flags like DOTALL).

Any engine lacking these returns **different match sets** for patterns the tool
legally accepts, silently changing grep output. That breaks the plan's central
requirement (§10: byte-exact parity with the Python path).

## Alternatives evaluated and rejected (per plan §3 kernel 7)

| Candidate | Rejected because |
|---|---|
| `std::regex` | ECMAScript subset only — no lookbehind, no backref parity guarantees, historically catastrophic performance on some inputs; cannot reach byte-exact parity with Python `regex` for the accepted pattern space. |
| RE2 | Linear-time guarantee, but deliberately **no lookaround and no backreferences** → breaks parity for patterns backup_grep accepts today. |
| Hand-rolled NFA | Same feature gaps, plus an unbounded maintenance/correctness surface; plan explicitly vendors instead. |

## What PCRE2 would buy

`PCRE2` (10.x, BSD-licensed, C, single vendored static lib — consistent with
kimix-base's vendored-ext philosophy of yyjson/xxhash/mbedtls/pybind11):
Perl-compatible semantics closest to Python `regex` among the candidates —
lookaround, backrefs, atomic groups, possessive quantifiers, `\p{...}` with
`PCRE2_UTF`/`PCRE2_UCP`, JIT optional. This is the plan's explicit
recommendation (§3 kernel 7).

## Required conformance gate (before any native flip)

Even with PCRE2, bit-exact parity is impossible for fuzzy quantifiers
(`{e<=n}`), some `\p` corner cases, and some `\X`/`\R`/`\b` Unicode edges.
The plan mandates a **conformance gate**: explicit feature scan at pattern
compile time; patterns using `regex`-only features are routed to the Python
mirror through the shim. Ship only when the golden-vector conformance suite is
green.

## What ships meanwhile (this commit)

- **Phase A complete**: all pure-CPU string kernels (selectors grammar with
  byte-exact ValueError texts, archive path parsing, content-line parse +
  grouped rendering + range filter + prefix re-attach/strip, rtk protocol
  parser + fold note, recorder merge, sensitive-path filter, pattern newline
  detection, multiline rewrite, byte-limit join) — see
  `src/builtin_tools/grep_tool.{h,cpp}` and
  `src/builtin_tools/reports/grep.md`.
- The **line-offset scanner contract** ships instead of the regex matcher: the
  C++ side takes caller-supplied match offsets (scan_lines-style), keeping the
  Python `regex` matcher authoritative for hit semantics (plan §3 kernel 7
  fallback: "Until PCRE2 lands, Phase A keeps the Python regex matcher per
  line").

## Unblocking request

Vendor PCRE2 as a new `src/ext` target (plan §6: `kimix-pcre2`, static,
`PCRE2_CODE_UNIT_WIDTH=8`, UTF+UCP defines), then implement kernel 7 behind
the conformance gate.
