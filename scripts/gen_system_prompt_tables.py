"""Regenerate the prompt template table in src/agent/system_prompt.cpp.

Derives every embedded string from kimi-agent's
src/kimix/utils/system_prompt.py (_TEMPLATES + _WORKER_OPTIONAL_CLAUSES) by
parsing the module AST (no import, no execution), so the C++ literals stay
byte-identical to the Python reference. Run from the project root:

    python scripts/gen_system_prompt_tables.py --reference D:/kimi-agent
"""

from __future__ import annotations

import argparse
import ast
import json
import sys
from pathlib import Path

CPP_FILE = "src/agent/system_prompt.cpp"
BEGIN = "// TEMPLATE-TABLE-BEGIN"
END = "// TEMPLATE-TABLE-END"


def extract_dicts(reference: Path) -> tuple[dict[str, str], dict[str, str]]:
    tree = ast.parse(
        (reference / "src" / "kimix" / "utils" / "system_prompt.py").read_text(
            encoding="utf-8"),
        filename="system_prompt.py")
    templates: dict[str, str] | None = None
    clauses: dict[str, str] | None = None
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1:
            target = node.targets[0]
            value = node.value
        elif isinstance(node, ast.AnnAssign):
            target = node.target
            value = node.value
        else:
            continue
        if not isinstance(target, ast.Name):
            continue
        if target.id == "_TEMPLATES":
            templates = ast.literal_eval(node.value)
        elif target.id == "_WORKER_OPTIONAL_CLAUSES":
            clauses = ast.literal_eval(node.value)
    if templates is None or clauses is None:
        raise SystemExit("could not locate _TEMPLATES / _WORKER_OPTIONAL_CLAUSES")
    return templates, clauses


def cpp_literal(name: str, value: str) -> str:
    # json.dumps escaping is valid for C++ string literals; keep non-ASCII
    # (em-dashes) as raw UTF-8 bytes like the Python source.
    return f'constexpr const char *k_{name} = {json.dumps(value, ensure_ascii=False)};'


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--reference", default="D:/kimi-agent",
                    help="path to the kimi-agent checkout")
    args = ap.parse_args()

    templates, clauses = extract_dicts(Path(args.reference))

    lines = []
    for key, value in templates.items():
        lines.append(cpp_literal(key.lower(), value))
    for key, value in clauses.items():
        lines.append(cpp_literal(f"clause_{key.lower()}", value))
    block = "\n".join(lines)

    cpp = Path(CPP_FILE)
    text = cpp.read_text(encoding="utf-8")
    begin = text.index(BEGIN) + len(BEGIN)
    end = text.index(END)
    new = text[:begin] + "\n" + block + "\n" + text[end:]
    cpp.write_text(new, encoding="utf-8")
    print(f"regenerated {CPP_FILE}: {len(templates)} templates, "
          f"{len(clauses)} clauses")
    return 0


if __name__ == "__main__":
    sys.exit(main())
