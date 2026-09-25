// cli/cli_skills.h - Skill directory discovery + skill scanning for the native
// CLI's system prompt.
//
// Port of two reference pieces:
// * kimix/base.py::COMMON_SKILL_DIRS / get_skill_dirs() and
//   kimix/utils/config.py::_load_skill_json() + init() step 4 (explicit
//   `skill_dir` handling) -> discover_skill_dirs(): the full directory
//   pipeline (auto-detected COMMON_SKILL_DIRS under the CWD with the
//   `*/skills` expansion, then `.kimix/skill.json`'s "skill_dir" entries,
//   then the explicit -s/--skill-dir arguments), with the reference's debug /
//   warning prints.
// * kimi_cli/skill/__init__.py::discover_skills / parse_skill_text /
//   discover_skills_from_roots / format_skills_for_prompt -> discover_skills()
//   + format_skills_for_prompt(): the two on-disk layouts (subdirectory
//   `<root>/<name>/SKILL.md` first, flat `<root>/<name>.md` second), the
//   frontmatter name/description parse with the reference's fallbacks, the
//   lowercased-name dedupe (first occurrence wins) and the scope-grouped
//   prompt rendering.
//
// Deviations from the reference (documented):
// * base.py caches the auto-detected list in a module global; this port
//   recomputes on every call (per PLAN.md the CLI is a one-shot init).
// * The reference carries the literal `p/*/skills` glob string downstream
//   (kaos expands it); here the glob is expanded eagerly to every existing
//   `<p>/<sub>/skills` directory, so the returned list holds real paths only.
// * Scopes: the ported pipeline has no user-home or bundled roots, so each
//   root is classified "project" when it lives under the process CWD and
//   "extra" otherwise ("builtin"/"user" remain renderable but unused).
//
// Rules (see src/cli/PLAN.md and .agents/skills/cpp): namespace kimix::cli,
// kimix:: containers in every public API, no RTTI, no exceptions (filesystem
// errors are swallowed exactly like the reference's OSError handlers), K&R
// braces, unity build with TU-local helpers in an anonymous namespace under
// the `clisk_` prefix.

#pragma once

#include <core/kimix_core.h>

namespace kimix::cli {

// One discovered skill (kimi_cli Skill's name/description/skill_md_file/scope).
struct skill_info {
    kimix::string name;
    kimix::string description;
    kimix::string md_file; // SKILL.md path (subdir form) or the .md itself (flat)
    kimix::string scope;   // "project" | "extra" (see the header note)
};

// The startup discovery result handed to the agent layer: the resolved skill
// roots plus the rendered system-prompt block.
struct skill_bundle {
    kimix::vector<kimix::string> dirs;
    kimix::string prompt_text;
};

// The full directory pipeline.  Auto-detects COMMON_SKILL_DIRS under the
// process CWD (printing `skill dir: <path>` per auto-detected dir), appends
// `.kimix/skill.json`'s "skill_dir" entries (`Skill dir from config:` /
// `Skill dir from config not found:`), then the explicit `-s` dirs
// (`Skill dir added:` / `Skill dir not found:`).  Relative inputs resolve
// against the CWD; non-directories are skipped with a warning.  Duplicates
// collapse to their first occurrence.
kimix::vector<kimix::string> discover_skill_dirs(const kimix::vector<kimix::string> &explicit_dirs);

// Scan every root for skills (subdirectory layout first, flat .md second),
// parsing the YAML frontmatter `name:` / `description:` lines plus the
// reference fallbacks (dir/file-stem name; first body line, else "No
// description provided.").  Deduped by lowercased name (first occurrence
// wins) and sorted by name.
kimix::vector<skill_info> discover_skills(const kimix::vector<kimix::string> &roots);

// The scope-grouped rendering for the system prompt: "### Project" /
// "### User" / "### Extra" / "### Built-in" sections of
// "- <name>\n  - Path: <md>\n  - Description: <desc>" blocks, sections
// separated by a blank line; "No skills found." when empty.
kimix::string format_skills_for_prompt(const kimix::vector<skill_info> &skills);

// discover_skill_dirs + discover_skills + format_skills_for_prompt in one
// call; what app_init stores on app_context for the soul's system prompt.
skill_bundle init_skill_bundle(const kimix::vector<kimix::string> &explicit_dirs);

} // namespace kimix::cli
