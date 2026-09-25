src/cli — skill discovery port (cli_skills) + the "Provider model:" debug print

Port of the skill pipeline the Python CLI assembles before creating a
session: kimix/base.py::COMMON_SKILL_DIRS / get_skill_dirs(),
kimix/utils/config.py::_load_skill_json() and init() step 4 (explicit
`skill_dir`), plus kimi_cli/skill/__init__.py::discover_skills /
parse_skill_text / discover_skills_from_roots / format_skills_for_prompt.
This report is the per-module traceability record of the change: which
behaviour each piece pins, where the expectations come from, and the exact
verification log.

Reference (read-only): D:/kimi-agent — src/kimix/base.py (COMMON_SKILL_DIRS,
get_skill_dirs), src/kimix/utils/config.py (_load_skill_json,
_load_and_set_provider, init), kimi-cli/src/kimi_cli/skill/__init__.py
(discover_skills, parse_skill_text, format_skills_for_prompt),
kimi-cli/src/kimi_cli/utils/frontmatter.py (parse/strip_frontmatter).

Nothing outside the listed deliverables was changed: src/agent/**,
src/cli/cli_commands.cpp, src/ext/**, src/core/**, the reference checkout and
no xmake build/test invocation was run (per the task constraints; syntax was
verified with scripts/check_cpp_syntax.py only).

1. Files

| file | change | content |
|---|---|---|
| src/cli/cli_skills.h | new | skill_info / skill_bundle, discover_skill_dirs, discover_skills, format_skills_for_prompt, init_skill_bundle (kimix::cli) |
| src/cli/cli_skills.cpp | new | the pipeline: COMMON_SKILL_DIRS + `*/skills` expansion, .kimix/skill.json (yyjson + kYYJsonAlcMi), explicit -s handling; the two discovery layouts + frontmatter parse; the scope-grouped renderer |
| src/cli/cli_config.cpp | +6 | `print_debug("Provider model: ...")` at the end of load_provider_config (the _load_and_set_provider print), "None" when the model key is absent; + cli_print.h include |
| src/cli/cli_app.h | +4 | `#include "cli/cli_skills.h"`, `skill_bundle skills;` on app_context |
| src/cli/cli_app.cpp | +5 | app_init: `app.skills = init_skill_bundle(opts.skill_dirs);` before the soul options are built |
| tests/unit/cli/test_cli_skills.cpp | new | 10 Boost.UT main-scope tests (see §3) |
| tests/xmake.lua | +5 | test_cli_skills target next to test_cli (same deps: kimix-llm, kimix-cli) |
| src/cli/reports/cli_skills.md | this file | |

No cli_print.h change was needed: print_debug already exists (bright cyan,
muted by quiet()) and carries every print this port needs.

2. Behaviour notes / documented deviations

* No caching: base.py memoises the auto-detected list in
  `_default_skill_dirs`; the port recomputes on every discover_skill_dirs()
  call (the native CLI initialises once per process).
* `*/skills` expansion: the reference stores the literal glob string
  `p/*/skills` and lets kaos expand it downstream. The native port expands
  eagerly: when `<p>/skills` is a directory, each existing
  `<p>/<sub>/skills` directory takes the pattern's place (no child with a
  `skills/` dir -> the pattern contributes nothing, matching an unmatched
  glob). The `skill dir: <path>` debug prints therefore show real paths.
* Order of the pipeline follows the task spec (auto-detect -> skill.json ->
  explicit -s), not the reference's implicit "any explicit dir suppresses
  auto-detection" side effect of the global cache.
* Scopes: kimi_cli has project/user/extra/builtin roots; the ported pipeline
  only ever produces CWD roots (project) and config/-s extras (extra).
  discover_skills classifies by location: under the process CWD -> project,
  otherwise -> extra. user/builtin stay renderable headings but are never
  produced by this pipeline.
* Frontmatter parser: simple line parser between the leading `---` fences
  (`name:` / `description:` keys, values trimmed, matching quotes stripped),
  plus the reference fallbacks: name -> directory name (subdir form) or file
  stem (flat form); description -> first non-empty body line (240-char cap +
  ellipsis) -> "No description provided.". A `---` opener without a closing
  fence is treated as body text, mirroring strip_frontmatter.
* Flat-layout rules kept verbatim: subdirectory form wins on a lowercased
  name clash, a bare `SKILL.md` at the root is a stray marker, non-`.md`
  files ignored, skills sorted by name (ASCII/code-point order), dedupe is
  first-occurrence-wins across roots.
* .kimix/skill.json tolerance: missing file -> nothing; malformed JSON ->
  print_warning (the reference prints the decode error, the port a fixed
  message); non-string list entries skipped; non-absolute entries resolve
  against the CWD; entries that are not existing directories warn
  ("Skill dir from config not found: ...") and are skipped.
* discover_skill_dirs dedupes exact resolved paths (first occurrence wins);
  kimi_cli dedupes canonical paths — equivalent here because every entry is
  absolute and lexically normalised.
* "Provider model: <model>" prints through print_debug in
  load_provider_config right after a successful parse, before the env
  entries are applied; wording matches _load_and_set_provider, with "None"
  for a missing model (unreachable today — load_provider_config rejects a
  model-less config — but kept for parity).

3. Test coverage (tests/unit/cli/test_cli_skills.cpp, all filesystem work in
   unique <temp>/kimix_cli_sk_* workspaces, cwd parked with a restoring
   guard, debug prints muted via set_quiet(true))

| behaviour | test |
|---|---|
| COMMON_SKILL_DIRS detection against a parked cwd (existing dirs kept in reference order, a *file* at a common path counts via .exists(), missing dirs skipped) | skill_dirs_common_detection |
| `*/skills` expansion (both child expansions found, child without skills/ skipped, empty expansion contributes nothing, no-`skills`-subdir root used as-is) | skill_dirs_glob_expansion |
| .kimix/skill.json string + list forms, cwd-relative resolution, absolute entries as-is, non-string entries skipped, missing dirs warned + skipped | skill_dirs_skill_json_forms |
| bad JSON tolerance, missing skill.json, empty result without crash | skill_dirs_skill_json_tolerance |
| explicit -s dirs (relative resolution, missing warned + skipped), exact-path dedupe against auto-detected dirs | skill_dirs_explicit_and_dedupe |
| both layouts, quoted frontmatter values, dir/file-stem name fallbacks, first-body-line description, subdir-over-flat shadowing, stray root SKILL.md ignored, .txt ignored, ASCII sort | discover_skills_layouts_and_parse |
| scope classification: root under the parked cwd -> "project" | discover_skills_project_scope |
| description fallbacks: "No description provided.", plain-body first line, 240-char truncation + ellipsis | discover_skills_description_fallbacks |
| format_skills_for_prompt golden output (Project + Extra sections, blank-line separation, empty-scope omission, "No skills found.") | skills_format_golden |
| within-scope name sorting in the rendered output | skills_format_sorts_within_scope |
| init_skill_bundle end-to-end: dirs + full prompt_text golden | skill_bundle_end_to_end |

4. Verification log

    python scripts/check_cpp_syntax.py --project-root D:/KimiX-native <file>

| file | result |
|---|---|
| src/cli/cli_skills.cpp | [OK] No issues found! |
| src/cli/cli_skills.h | (header; checked through the includers) |
| src/cli/cli_config.cpp | [OK] No issues found! |
| src/cli/cli_app.cpp | [OK] (1 pre-existing unused-include warning for <system_error> at line 27, untouched) |
| src/cli/cli_app.h | (header; checked through the includers) |
| tests/unit/cli/test_cli_skills.cpp | [OK] No issues found! |

Per the task constraints no xmake build or test binary was run; the tests are
registered as test_cli_skills in tests/xmake.lua and await the parent's build
wave. One ordering expectation was corrected during authoring: name sorting is
raw ASCII (uppercase before lowercase, "Zebra" < "mango"), which matches
Python's code-point ordering.

5. Left for the parent

* Building test_cli_skills (tests/xmake.lua) and running it in the build wave.
* Feeding app_context::skills into the soul: cliapp_soul_options /
  app_rebind_session / app_run_isolated currently do not consume the bundle;
  wire prompt_text (and dirs if the soul wants roots for Glob access) into
  KimiSoul::options wherever the parent's soul integration lands.
* The reference's user-home roots (~/.kimi/skills, ~/.agents/skills, ...) and
  bundled builtin skills are out of scope for this port; if the soul later
  needs them, extend discover_skill_dirs rather than the renderer.
