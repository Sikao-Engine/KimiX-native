# agent_*.json manifest coverage report

Date: 2026 (checkout `D:/kimi-agent`, current working tree).
Scope: the 5 kimi-agent role manifests under `src/kimix/` — `agent_worker.json`,
`agent_planner.json`, `agent_boss.json`, `agent_subagent.json`,
`agent_readonly.json` — checked against the `kimix::builtin_tools::ToolRegistry`
in this repository (post lowercase-rename keys + fuzzy `resolve()`).

## How the check works

New test: `tests/unit/builtin_tools/test_agent_manifests.cpp`
(registered as `test_agent_manifests` in `tests/xmake.lua`, same deps as
`test_builtin_tool`).

* Locates the kimi-agent checkout: env `KIMI_AGENT_ROOT`, else
  `C:/dev/kimi-agent`, else `D:/kimi-agent`; if none exists it prints a SKIP
  notice and passes (mirrors the `python/tests` parity-test convention).
* Reads each manifest's bytes and parses with `yyjson_read_opts` +
  `kimix::llm::kYYJsonAlcMi` (mimalloc allocator; docs freed with
  `yyjson_doc_free`).
* Extracts `agent.extend` (must be a JSON string) and iterates `agent.tools`.
* Each `"<module>:<attr>"` entry is split on the LAST `:` and the attr is
  resolved through `ToolRegistry::resolve()` (exact canonical →
  case-insensitive canonical → alias exact → alias folded), asserting the
  resolved `ToolMeta` exists and has a non-empty factory. The mechanism that
  accepted each attr is classified and printed.
* A factored helper `check_manifest_text()` is also unit-tested with inline
  JSON (valid manifest; empty attr after `:`; missing `:`; non-string entry;
  non-JSON garbage) so the malformed-input robustness coverage runs even
  without the checkout. Broken entries are reported as unresolved and never
  crash.

The builtin_tools tests link `kimix-llm` only (not the `cli` library), so the
test resolves the attr directly through `ToolRegistry::resolve` instead of
`kimix::cli::resolve_tool_path`.

## Per-manifest results (parsed from the actual files)

| Manifest | File | extend | Tools | All resolved | Mechanism breakdown |
|---|---|---|---|---|---|
| worker | `D:/kimi-agent/src/kimix/agent_worker.json` | `"default"` | 22 | **yes** | 21 exact canonical, 1 case-insensitive canonical (`Run` → `run`) |
| planner | `D:/kimi-agent/src/kimix/agent_planner.json` | `"default"` | 9 | **yes** | 6 exact canonical, 3 case-insensitive canonical (`WritePlan` → `writeplan`, `ReadPlan` → `readplan`, `EditPlan` → `editplan`) |
| boss | `D:/kimi-agent/src/kimix/agent_boss.json` | `"default"` | 18 | **yes** | 15 exact canonical, 3 case-insensitive canonical (the 3 plan tools) |
| subagent | `D:/kimi-agent/src/kimix/agent_subagent.json` | `"default"` | 18 | **yes** | 17 exact canonical, 1 case-insensitive canonical (`Run` → `run`) |
| readonly | `D:/kimi-agent/src/kimix/agent_readonly.json` | `"default"` | 8 | **yes** | 8 exact canonical |

Union across the 5 manifests: 25 distinct `"<module>:<attr>"` entries / 25
distinct attrs — matches the 25-tool static table in
`tests/unit/builtin_tools/test_tool.cpp` (`registry_covers_every_agent_json_tool`)
and `k_clit_agent_tools` in `src/cli/cli_tools.cpp`.

Resolution-mechanism notes:

* All 21 lowercase attrs (`bash`, `pwsh`, `python`, `job_output`, `todo_write`,
  `todo_update`, `retrieve`, `read`, `read_image`, `edit`, `write`, `subagent`,
  `send_message`, `list_agents`, `interrupt_agent`, `workflow`, `glob`, `grep`,
  `fetch_url`, `web_search`, `compact`) hit the registry's **exact canonical**
  leg (a) — the lowercase rename made every manifest attr a byte-exact key.
* `Run`, `WritePlan`, `ReadPlan`, `EditPlan` hit leg (b), the
  **case-insensitive canonical** pass (`writeplan` ≡ `WritePlan` ignoring
  case). They resolve *before* the alias legs are consulted, even though
  `"WritePlan"`/`"write_plan"` etc. are also declared aliases.
* No manifest entry needed the alias legs (c)/(d); the declared aliases
  (`Shell`, `str_replace`, `task_output`, `swarm`, …) remain as
  hallucination-tolerance extras, covered by
  `tool_registry_fuzzy_name_resolution` in `test_tool.cpp`.

## Verification steps run

1. Read all 5 manifest files at `D:/kimi-agent/src/kimix/agent_*.json` and
   extracted `agent.extend` / `agent.tools` (see table; all `extend` values
   are the string `"default"`).
2. Cross-checked the registered canonical keys and alias lists at every
   `KIMIX_REGISTER_TOOL_NAMED_ALIASED` site under `src/builtin_tools/`
   (25 registrations).
3. Simulated the exact `ToolRegistry::resolve()` order (and the test's
   `classify_resolution`) over the parsed manifests: 0 unresolved attrs.
4. `python scripts/check_cpp_syntax.py --project-root D:\KimiX-native
   tests/unit/builtin_tools/test_agent_manifests.cpp` → **OK, no issues**
   (xmake build/test execution intentionally not run per task constraints;
   the test binary is expected to be exercised by the normal
   `xmake run test_agent_manifests` flow).

## Conclusion

**All 5 agent types (worker, planner, boss, subagent, readonly) are fully
supported by the current registry.** Every tool entry in every manifest
resolves — 75 of 75 manifest entries across the 5 files, 25 distinct tools —
with no registry changes required. No unresolved tools, no bugs found.
