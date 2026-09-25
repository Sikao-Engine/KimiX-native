#!/usr/bin/env python3
"""Regenerate tests/unit/cli/cli_config_goldens.inc.

Why
---
``src/cli/cli_config.cpp`` ports reference behaviour that is pure data plus a
pure algorithm, and both are easy to get subtly wrong:

* ``_MODEL_DEFAULTS`` - the 17 ``(keywords, max_context_size, max_tokens)`` rows,
* ``_tokenize_model_name`` / ``_keywords_match`` - tokenise on ``[^a-z0-9]+``,
  consume the keyword tokens from the model-token multiset (numeric tokens must
  match exactly, alphabetic tokens may fuzzy-match with a rapidfuzz
  ``fuzz.ratio`` >= 80), and
* ``_resolve_model_defaults`` - first matching row wins, ``None`` when unknown.

Instead of transcribing expectations by hand, this generator imports the
reference module **by path** (``<reference>/kimi-cli/src/kimi_cli/config.py``),
runs it over a deterministic corpus of model names and writes the results as C++
arrays.  It also records the tool list of the five KimiX role manifests
(``<reference>/src/kimix/agent_*.json``) so the C++ test can assert that every
distinct ``module:attr`` path they name resolves through
``kimix::cli::resolve_tool_path`` and is backed by a registered tool class.
(The ``module:attr`` -> registry-name mapping itself is not derivable from the
reference - the Python CLI resolves it dynamically with importlib - so only the
distinct path list is emitted.)

The reference checkout is opened read-only and is not needed at test run time:
``tests/unit/cli/cli_config_goldens.inc`` is committed generated data that
``tests/unit/cli/test_cli.cpp`` replays.  ``HELP_STR`` is *not* duplicated here;
``scripts/gen_cli_help.py`` already owns that text.

Usage::
    python scripts/gen_cli_config_goldens.py            # rewrite the .inc
    python scripts/gen_cli_config_goldens.py --check    # fail when out of date
    python scripts/gen_cli_config_goldens.py --reference D:/kimi-agent
    python scripts/gen_cli_config_goldens.py --out other.inc
    python scripts/gen_cli_config_goldens.py --python <python.exe>

When the running interpreter cannot import the reference module's dependencies
(orjson / regex / tomlkit / pydantic / rapidfuzz / kosong) the script re-runs
itself under the kimi-agent virtualenv interpreter
(``<KIMI_AGENT_ROOT>/.venv/Scripts/python.exe``) - the same fallback
``scripts/gen_tool_pairing_goldens.py`` uses.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import random
import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_REFERENCE = Path(os.environ.get("KIMI_AGENT_ROOT", r"C:/dev/kimi-agent"))
DEFAULT_REFERENCE_PYTHON = DEFAULT_REFERENCE / ".venv" / "Scripts" / "python.exe"
DEFAULT_OUT = PROJECT_ROOT / "tests" / "unit" / "cli" / "cli_config_goldens.inc"

#: The reference module, relative to the checkout root.
CONFIG_REL = Path("kimi-cli") / "src" / "kimi_cli" / "config.py"
#: The kimi_cli package root (needed on sys.path for config.py's own imports).
KIMI_CLI_SRC_REL = Path("kimi-cli") / "src"
#: The KimiX role manifests, relative to the checkout root.
MANIFEST_DIR_REL = Path("src") / "kimix"
MANIFEST_NAMES = (
    "agent_worker.json",
    "agent_boss.json",
    "agent_planner.json",
    "agent_readonly.json",
    "agent_subagent.json",
)
#: Module name the reference config.py is loaded under (never imported as
#: ``kimi_cli.config`` so the loaded module cannot shadow the real package).
REFERENCE_MODULE_NAME = "kimix_cli_config_reference"
#: Seed of the deterministic fuzz group.
FUZZ_SEED = 0x5D6C11
FUZZ_COUNT = 40

#: Model names that mix separators, case and surrounding whitespace.
MULTI_SEPARATOR_NAMES = (
    "  Claude   Opus   5  ",
    "claude.opus.5",
    "claude/opus/5",
    "claude::opus::5",
    "CLAUDE__OPUS__5",
    "gpt--5.4---mini",
    "gpt_5_4_mini",
    "Gemini-3.5.Flash",
    "\tdeepseek/v4/flash\n",
    "supergrok..heavy",
)

#: Names that must not match any row.
UNKNOWN_NAMES = (
    "llama-3-70b",
    "mistral-large",
    "qwen2.5-72b",
    "phi-4",
    "command-r-plus",
    "yi-34b",
    "gemma-2-9b",
    "o1-preview",
    "o3-mini",
    "sonnet",
    "opus",
    "sol",
    "grok-5",
    "supergrok",
)

#: Degenerate / boundary inputs.
EDGE_NAMES = (
    "",
    " ",
    "\t\n\r\v\f",
    "----",
    "///",
    "5.6",
    "0",
    "2",
    "claude",
    "gemini",
    "amazon",
    "mini",
    "flash",
    "claude\u00b7opus\u00b75",
    "gemini-\uff13.6",
    "gpt-5.4-gemini",
)


def _reference_config_path(reference: Path) -> Path:
    return reference / CONFIG_REL


def _ensure_reference_on_path(reference: Path) -> None:
    for rel in (KIMI_CLI_SRC_REL, Path("src")):
        candidate = reference / rel
        if candidate.is_dir() and str(candidate) not in sys.path:
            sys.path.insert(0, str(candidate))


def load_reference_module(reference: Path):
    """Import ``kimi_cli/config.py`` by path and return the module.

    The module is loaded under a private name (never as ``kimi_cli.config``) so
    the generator cannot accidentally depend on an installed kimi_cli.
    """
    path = _reference_config_path(reference)
    if not path.is_file():
        raise FileNotFoundError("reference config.py not found: %s" % path)
    _ensure_reference_on_path(reference)
    spec = importlib.util.spec_from_file_location(REFERENCE_MODULE_NAME, path)
    if spec is None or spec.loader is None:
        raise ImportError("cannot load %s" % path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    for attribute in ("_MODEL_DEFAULTS", "_tokenize_model_name", "_keywords_match",
                      "_resolve_model_defaults"):
        if not hasattr(module, attribute):
            raise AttributeError("%s has no %s" % (path, attribute))
    return module


def git_head(path: Path) -> str:
    """``git rev-parse HEAD`` (read-only) or ``"unknown"``."""
    try:
        done = subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"],
                              capture_output=True, text=True, check=False)
    except OSError:
        return "unknown"
    if done.returncode != 0:
        return "unknown"
    return done.stdout.strip() or "unknown"


# ---------------------------------------------------------------------------
# Model-name corpus
# ---------------------------------------------------------------------------

def _separator_variant(canonical: str) -> str:
    """``gpt-5.6-sol`` -> ``gpt__5.6__sol`` (same tokens, other separators)."""
    return canonical.replace("-", "__")


def _typo_variants(canonical: str) -> list[tuple[str, str]]:
    """Three deterministic typos of `canonical` (typos of alphabetic runs).

    * ``duplicate`` doubles the last alphabetic character,
    * ``drop`` removes the last alphabetic character,
    * ``swap`` transposes the first two alphabetic characters of the last run.
    """
    letters = [i for i, ch in enumerate(canonical) if ch.isalpha()]
    if not letters:
        return []
    last = letters[-1]
    out = [("duplicate", canonical[:last + 1] + canonical[last] + canonical[last + 1:])]
    if len(letters) > 1:
        out.append(("drop", canonical[:last] + canonical[last + 1:]))
    # First two characters of the *last* alphabetic run.
    run_start = last
    while run_start > 0 and canonical[run_start - 1].isalpha():
        run_start -= 1
    if last - run_start >= 1:
        swapped = list(canonical)
        swapped[run_start], swapped[run_start + 1] = swapped[run_start + 1], swapped[run_start]
        out.append(("swap", "".join(swapped)))
    return out


def _bump_version(canonical: str) -> str:
    """Increment the last numeric component, or append ``-2`` when there is none."""
    index = None
    for i in range(len(canonical) - 1, -1, -1):
        if canonical[i].isdigit():
            index = i
            break
    if index is None:
        return canonical + "-2"
    digits = canonical[index]
    return canonical[:index] + str((int(digits) + 1) % 10) + canonical[index + 1:]


def _fuzz_names(module, rows: list[dict], count: int) -> list[str]:
    """A small seeded fuzz set built from the reference vocabulary."""
    rnd = random.Random(FUZZ_SEED)
    pieces: list[str] = []
    for row in rows:
        pieces.append(row["canonical"])
        pieces.extend(row["keywords"])
        for phrase in row["keywords"]:
            pieces.extend(module._tokenize_model_name(phrase))
    pieces = sorted(set(pieces))
    alphabet = "abcdefghijklmnopqrstuvwxyz0123456789"
    separators = ("-", ".", "_", "/", " ", "::", "--", "__", ".")
    names: list[str] = []
    for _ in range(count):
        if rnd.random() < 0.75:
            parts = [rnd.choice(pieces) for _ in range(rnd.randint(1, 3))]
        else:
            parts = ["".join(rnd.choice(alphabet) for _ in range(rnd.randint(1, 5)))
                     for _ in range(rnd.randint(3, 8))]
        name = rnd.choice(separators).join(parts)
        if rnd.random() < 0.3:
            name = name.upper()
        names.append(name)
    return names


def collect_rows(module) -> list[dict]:
    rows: list[dict] = []
    for keywords, context, output in module._MODEL_DEFAULTS:
        keywords = tuple(keywords)
        canonical = "-".join(keywords)
        resolved = module._resolve_model_defaults(canonical)
        rows.append({
            "keywords": keywords,
            "canonical": canonical,
            "context": int(context),
            "output": -1 if output is None else int(output),
            "canonical_known": resolved is not None,
            "canonical_context": -1 if resolved is None else int(resolved[0]),
            "canonical_output": -1 if resolved is None or resolved[1] is None
            else int(resolved[1]),
        })
    return rows


def collect_cases(module, rows: list[dict]) -> list[dict]:
    wanted: list[tuple[str, str]] = []
    for row in rows:
        wanted.append(("canonical", row["canonical"]))
        wanted.append(("separator", _separator_variant(row["canonical"])))
    for row in rows:
        for _label, name in _typo_variants(row["canonical"]):
            wanted.append(("typo", name))
    for row in rows:
        wanted.append(("version", _bump_version(row["canonical"])))
    for name in MULTI_SEPARATOR_NAMES:
        wanted.append(("multi_sep", name))
    for name in UNKNOWN_NAMES:
        wanted.append(("unknown", name))
    for name in EDGE_NAMES:
        wanted.append(("edge", name))
    for name in _fuzz_names(module, rows, FUZZ_COUNT):
        wanted.append(("fuzz", name))

    cases: list[dict] = []
    seen: set[str] = set()
    for group, name in wanted:
        if name in seen:
            continue
        seen.add(name)
        tokens = list(module._tokenize_model_name(name))
        resolved = module._resolve_model_defaults(name)
        mask = 0
        for index, (keywords, _ctx, _out) in enumerate(module._MODEL_DEFAULTS):
            if module._keywords_match(tuple(keywords), tokens):
                mask |= 1 << index
        cases.append({
            "group": group,
            "name": name,
            "tokens": tokens,
            "known": resolved is not None,
            "context": -1 if resolved is None else int(resolved[0]),
            "output": -1 if resolved is None or resolved[1] is None else int(resolved[1]),
            "mask": mask,
        })
    return cases


def collect_manifests(reference: Path) -> list[dict]:
    manifests: list[dict] = []
    for name in MANIFEST_NAMES:
        path = reference / MANIFEST_DIR_REL / name
        if not path.is_file():
            manifests.append({"file": name, "tools": [], "present": False})
            continue
        data = json.loads(path.read_text(encoding="utf-8"))
        agent = data.get("agent") if isinstance(data, dict) else None
        if not isinstance(agent, dict):
            agent = data if isinstance(data, dict) else {}
        tools = agent.get("tools")
        if not isinstance(tools, list):
            tools = []
        manifests.append({
            "file": name,
            "tools": [tool for tool in tools if isinstance(tool, str)],
            "present": True,
        })
    return manifests


def distinct_tool_paths(manifests: list[dict]) -> list[str]:
    seen: list[str] = []
    for manifest in manifests:
        for tool in manifest["tools"]:
            if tool not in seen:
                seen.append(tool)
    return seen


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def c_literal(text: str) -> str:
    """Render `text` as a C++ string literal (LF-free, non-ASCII escaped)."""
    chunks = ['"']
    for ch in text:
        code = ord(ch)
        if ch == '"':
            chunks.append('\\"')
        elif ch == "\\":
            chunks.append("\\\\")
        elif ch == "\n":
            chunks.append("\\n")
        elif ch == "\t":
            chunks.append("\\t")
        elif ch == "\r":
            chunks.append("\\r")
        elif 0x20 <= code < 0x7F:
            chunks.append(ch)
        elif code < 0x80:
            chunks.append("\\x%02x" % code)
            chunks.append('""')  # a following hex digit would extend the escape
        else:
            for byte in ch.encode("utf-8"):
                chunks.append("\\x%02x" % byte)
                chunks.append('""')
    chunks.append('"')
    return "".join(chunks)


def joined(tokens: list[str], separator: str) -> str:
    return separator.join(tokens)


def render(rows: list[dict], cases: list[dict], manifests: list[dict],
           reference: Path, commits: dict, out_label: str) -> str:
    paths = distinct_tool_paths(manifests)
    groups: dict[str, int] = {}
    for case in cases:
        groups[case["group"]] = groups.get(case["group"], 0) + 1
    breakdown = ", ".join("%d %s" % (groups[group], group) for group in sorted(groups))

    lines: list[str] = []
    lines.append("// GENERATED by scripts/gen_cli_config_goldens.py - DO NOT EDIT BY HAND.")
    lines.append("//")
    lines.append("// Expectations derived from the kimi-agent reference implementation:")
    lines.append("//   * kimi-cli/src/kimi_cli/config.py: _MODEL_DEFAULTS /")
    lines.append("//     _tokenize_model_name / _keywords_match / _resolve_model_defaults")
    lines.append("//     (imported by path, never imported at test run time)")
    lines.append("//   * src/kimix/agent_*.json: the tool list of every KimiX role manifest")
    lines.append("//")
    lines.append("// Reference checkout: %s" % reference)
    lines.append("//   repo HEAD:     %s" % commits["repo"])
    lines.append("//   kimi-cli HEAD: %s" % commits["kimi_cli"])
    lines.append("//")
    lines.append("// Corpus: %d model names (%s)" % (len(cases), breakdown))
    lines.append("//         %d default rows, %d manifests, %d distinct tool paths"
                 % (len(rows), len(manifests), len(paths)))
    lines.append("// Replay: tests/unit/cli/test_cli.cpp; regenerate with")
    lines.append("//   python scripts/gen_cli_config_goldens.py")
    lines.append("//")
    lines.append("// %s" % out_label)
    lines.append("")
    lines.append("// _MODEL_DEFAULTS (reference order; first match wins).")
    lines.append("// `output` / `canonical_output` are -1 where the reference stores None.")
    lines.append("struct cli_golden_model_row {")
    lines.append("    const char *keywords;         // keyword phrases, ' '-separated")
    lines.append("    const char *canonical;        // the phrases joined with '-'")
    lines.append("    long long context;            // max_context_size")
    lines.append("    long long output;             // max_tokens (-1 == None)")
    lines.append("    bool canonical_known;         // _resolve_model_defaults(canonical)")
    lines.append("    long long canonical_context;  //   is None?")
    lines.append("    long long canonical_output;")
    lines.append("};")
    lines.append("static const cli_golden_model_row kCliGoldenModelRows[] = {")
    for row in rows:
        lines.append('    {%s, %s, %d, %d, %s, %d, %d},' % (
            c_literal(joined(list(row["keywords"]), " ")),
            c_literal(row["canonical"]),
            row["context"],
            row["output"],
            "true" if row["canonical_known"] else "false",
            row["canonical_context"],
            row["canonical_output"],
        ))
    lines.append("};")
    lines.append("static const size_t kCliGoldenModelRowCount =")
    lines.append("    sizeof(kCliGoldenModelRows) / sizeof(kCliGoldenModelRows[0]);")
    lines.append("")
    lines.append("// One model name: the reference's tokenisation, resolve result and the")
    lines.append("// per-row _keywords_match bitmask (bit r == row r matched).")
    lines.append("struct cli_golden_model_case {")
    lines.append("    const char *group;   // corpus group the name came from")
    lines.append("    const char *name;    // the model name as fed to the reference")
    lines.append("    const char *tokens;  // '|'-separated _tokenize_model_name(name)")
    lines.append("    bool known;          // _resolve_model_defaults(name) is not None")
    lines.append("    long long context;   // max_context_size (-1 == unknown)")
    lines.append("    long long output;    // max_tokens (-1 == None or unknown)")
    lines.append("    unsigned mask;       // per-row _keywords_match bitmask")
    lines.append("};")
    lines.append("static const cli_golden_model_case kCliGoldenModelCases[] = {")
    for case in cases:
        lines.append('    {%s, %s, %s, %s, %d, %d, 0x%04xu},' % (
            c_literal(case["group"]),
            c_literal(case["name"]),
            c_literal(joined(case["tokens"], "|")),
            "true" if case["known"] else "false",
            case["context"],
            case["output"],
            case["mask"],
        ))
    lines.append("};")
    lines.append("static const size_t kCliGoldenModelCaseCount =")
    lines.append("    sizeof(kCliGoldenModelCases) / sizeof(kCliGoldenModelCases[0]);")
    lines.append("")
    lines.append("// The KimiX role manifests: their tool paths as written (present == false")
    lines.append("// when the reference checkout does not carry the file).")
    lines.append("struct cli_golden_manifest {")
    lines.append("    const char *file;    // file name under <reference>/src/kimix/")
    lines.append("    const char *tools;   // '|'-separated tool paths, in file order")
    lines.append("    size_t count;")
    lines.append("    bool present;")
    lines.append("};")
    lines.append("static const cli_golden_manifest kCliGoldenManifests[] = {")
    for manifest in manifests:
        lines.append('    {%s, %s, %d, %s},' % (
            c_literal(manifest["file"]),
            c_literal(joined(manifest["tools"], "|")),
            len(manifest["tools"]),
            "true" if manifest["present"] else "false",
        ))
    lines.append("};")
    lines.append("static const size_t kCliGoldenManifestCount =")
    lines.append("    sizeof(kCliGoldenManifests) / sizeof(kCliGoldenManifests[0]);")
    lines.append("")
    lines.append("// Every distinct tool path of the union, first-appearance order")
    lines.append("// (agent_worker, agent_boss, agent_planner, agent_readonly, agent_subagent).")
    lines.append("static const char *const kCliGoldenDistinctToolPaths[] = {")
    for path in paths:
        lines.append("    %s," % c_literal(path))
    lines.append("};")
    lines.append("static const size_t kCliGoldenDistinctToolPathCount =")
    lines.append("    sizeof(kCliGoldenDistinctToolPaths) / sizeof(kCliGoldenDistinctToolPaths[0]);")
    lines.append("")
    return "\n".join(lines)


def out_label_for(path: Path) -> str:
    """``tests/unit/cli/cli_config_goldens.inc`` (relative) or the raw path."""
    try:
        return path.resolve().relative_to(PROJECT_ROOT).as_posix()
    except ValueError:
        return str(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, default=DEFAULT_REFERENCE,
                        help="kimi-agent checkout (default: %(default)s)")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT,
                        help="generated include path (default: %(default)s)")
    parser.add_argument("--python", default=None,
                        help="interpreter able to import the reference module "
                             "(default: the kimi-agent virtualenv)")
    parser.add_argument("--check", action="store_true",
                        help="exit 1 when the generated file is out of date")
    args = parser.parse_args()

    try:
        module = load_reference_module(args.reference)
    except Exception as exc:  # environment issue: retry under the reference venv
        fallback = Path(args.python) if args.python else DEFAULT_REFERENCE_PYTHON
        same = (Path(sys.executable).resolve() == fallback.resolve()
                if fallback.is_file() else True)
        if fallback.is_file() and not same:
            print("re-running under %s (%s)" % (fallback, exc), file=sys.stderr)
            return subprocess.call([str(fallback), str(Path(__file__).resolve()),
                                    *sys.argv[1:]])
        print("cannot import %s: %s" % (_reference_config_path(args.reference), exc),
              file=sys.stderr)
        return 2

    rows = collect_rows(module)
    cases = collect_cases(module, rows)
    manifests = collect_manifests(args.reference)
    commits = {
        "repo": git_head(args.reference),
        "kimi_cli": git_head(args.reference / "kimi-cli"),
    }
    text = render(rows, cases, manifests, args.reference, commits,
                  out_label_for(args.out))

    # The reference itself must be internally consistent: a name is resolvable
    # exactly when at least one row matches it.
    for case in cases:
        if case["known"] != (case["mask"] != 0):
            print("reference inconsistency for %r: known=%s mask=0x%x"
                  % (case["name"], case["known"], case["mask"]), file=sys.stderr)
            return 2
    for row in rows:
        if not row["canonical_known"]:
            print("reference row %r is unreachable via its canonical name"
                  % row["canonical"], file=sys.stderr)
            return 2

    paths = distinct_tool_paths(manifests)
    absent = [m["file"] for m in manifests if not m["present"]]

    if args.check:
        if not args.out.is_file():
            print("missing generated file: %s" % args.out, file=sys.stderr)
            return 1
        current = args.out.read_text(encoding="utf-8")
        if current != text:
            print("%s is out of date - regenerate with "
                  "python scripts/gen_cli_config_goldens.py" % args.out, file=sys.stderr)
            return 1
        print("in sync: %s (%d rows, %d model names, %d manifests, %d tool paths)"
              % (args.out, len(rows), len(cases), len(manifests), len(paths)))
        if absent:
            print("note: reference manifest(s) missing: %s" % ", ".join(absent))
        return 0

    args.out.parent.mkdir(parents=True, exist_ok=True)
    # newline="\n" keeps the generated file LF-only so --check compares like for like.
    args.out.write_text(text, encoding="utf-8", newline="\n")
    print("wrote %s (%d rows, %d model names, %d manifests, %d tool paths)"
          % (args.out, len(rows), len(cases), len(manifests), len(paths)))
    if absent:
        print("note: reference manifest(s) missing: %s" % ", ".join(absent))
    groups: dict[str, int] = {}
    for case in cases:
        groups[case["group"]] = groups.get(case["group"], 0) + 1
    print("corpus groups: %s"
          % ", ".join("%s=%d" % item for item in sorted(groups.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
