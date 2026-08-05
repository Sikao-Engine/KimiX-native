/*
 * prompt_texts.h - embedded compaction prompt texts (plan 016).
 *
 * Verbatim content of kimi_cli/prompts/compact.md and compact_cascade.md
 * (universal-newline normalized, exactly what prompts.py read_text
 * produces) plus the default (balanced) mode guidance of
 * soul/compaction.py::_MODE_GUIDANCE. Non-ASCII bytes are \xNN
 * escapes so the runtime target (no /utf-8 flag) compiles cleanly.
 */
#pragma once

namespace kimix {
namespace runtime {
namespace soul {
namespace {

// prompts.COMPACT (compact.md).
constexpr const char* kCompactPromptText =
    "---\n\nCompact the above agent conversation context. Very detailed, comprehensive.\n\n**What to "
    "keep (ordered by priority):**\n1. **Current Task State** \xE2\x80\x94 what is being worked on ri"
    "ght now, plus any user-supplied custom instructions, preferences, or constraints for future turn"
    "s.\n2. **Errors & Solutions** \xE2\x80\x94 preserve the full error message and the final working"
    " solution. For multi-turn debugging, summarize intermediate steps as a brief narrative (1-2 line"
    "s).\n3. **Code State** \xE2\x80\x94 final working versions only (drop intermediate attempts).\n4"
    ". **Design Decisions** \xE2\x80\x94 architectural choices and rationale.\n5. **Environment** "
    "\xE2\x80\x94 OS, work directory, Python version, key dependencies, and other relevant setup.\n6."
    " **TODO Items** \xE2\x80\x94 unfinished tasks and known issues.\n7. **Project Overview** \xE2"
    "\x80\x94 purpose, scope, tech stack.\n8. **Key Decisions** \xE2\x80\x94 critical choices, ration"
    "ale, rejected alternatives.\n9. **Current State** \xE2\x80\x94 what works, merged/verified, acti"
    "ve branch, test results.\n10. **Important Files** \xE2\x80\x94 key paths and their roles (add, m"
    "odify, delete).\n11. **Architecture / Data Flow** \xE2\x80\x94 major components, interfaces, sch"
    "ema changes.\n12. **Dependencies** \xE2\x80\x94 added, removed, upgraded packages or services.\n"
    "13. **Risks / Rollback** \xE2\x80\x94 breaking changes, migration steps, revert strategy.\n14. *"
    "*Technical Notes** \xE2\x80\x94 patterns, constraints, APIs, env setup, performance or security "
    "considerations.\n\n**What to remove or condense:**\n- **Drop:** redundant explanations, failed i"
    "ntermediate attempts (retain lessons learned), verbose comments, conversational filler.\n- **Mer"
    "ge:** similar discussions into single summary points.\n- **Condense code:** \n  - Keep full vers"
    "ion if \xE2\x89\xA4 20 lines.\n  - For longer code, keep signature + **key logic** only.\n  \n  "
    "**Key logic** means:\n  - The core algorithm or business logic (not boilerplate/imports)\n  - Cr"
    "itical control flow (conditionals, loops, error handling)\n  - Non-obvious transformations or si"
    "de effects\n  - Exclude: imports, logging, type annotations, docstrings, setup/teardown boilerpl"
    "ate\n\n**Special Handling:**\n- **Code:** keep full version if < 20 lines; otherwise keep signat"
    "ure + key logic\n- **Errors:** keep full error message + final solution\n- **Discussions:** extr"
    "act decisions and action items only\n\n**Length:** Aim to reduce the context to approximately 20"
    "-30% of the original length while preserving all essential information. Err on the side of brevi"
    "ty for aggressive mode and completeness for retentive mode.\n\n**User Instructions:** Preserve a"
    "ny explicit user preferences, constraints, or custom compaction instructions for future turns.\n"
    "\n**Output Structure:**\n\n```xml\n<current_focus>\n[What we're working on now]\n</current_focus"
    ">\n\n<environment>\n- OS: [os]\n- Work dir: [path]\n- Key deps: [packages]\n- [Other relevant se"
    "tup]\n</environment>\n\n<completed_tasks>\n- [Task]: [Brief outcome]\n</completed_tasks>\n\n<act"
    "ive_issues>\n- [Issue]: [Status/Next steps]\n</active_issues>\n\n<todo>\n- [ ] [Unfinished task]"
    "\n</todo>\n\n<code_state>\n<file name=\"path/to/file.py\">\n<summary>What this file does</summar"
    "y>\n<key_elements>\n- FunctionA: does X\n- ClassB: handles Y\n</key_elements>\n<latest_version>"
    "\n[Critical code snippets]\n</latest_version>\n</file>\n</code_state>\n\n<decisions>\n- [Decisio"
    "n]: [Rationale]\n</decisions>\n\n<key_decisions>\n- [Decision]: [Rationale, rejected alternative"
    "s]\n</key_decisions>\n\n<project_overview>\n- Purpose: [project purpose]\n- Scope: [project scop"
    "e]\n- Tech stack: [tech stack]\n</project_overview>\n\n<current_state>\n- What works: [summary]"
    "\n- Merged/Verified: [status]\n- Active branch: [branch]\n- Test results: [summary]\n</current_s"
    "tate>\n\n<important_files>\n- [path/to/file.py]: [role]\n</important_files>\n\n<architecture>\n-"
    " Major components: [list]\n- Interfaces: [summary]\n- Data flow: [summary]\n- Schema changes: [s"
    "ummary]\n</architecture>\n\n<dependencies>\n- Added: [packages]\n- Removed: [packages]\n- Upgrad"
    "ed: [packages]\n</dependencies>\n\n<risks_rollback>\n- Breaking changes: [details]\n- Migration "
    "steps: [details]\n- Revert strategy: [details]\n</risks_rollback>\n\n<technical_notes>\n- Patter"
    "ns: [details]\n- Constraints: [details]\n- APIs: [details]\n- Env setup: [details]\n- Performanc"
    "e/Security: [details]\n</technical_notes>\n\n<important_context>\n- [Crucial information not cov"
    "ered above]\n</important_context>\n```\n";

// prompts.COMPACT_CASCADE (compact_cascade.md).
constexpr const char* kCompactCascadePromptText =
    "---\n\nThe above context contains **multiple previous compaction summaries** that have been recu"
    "rsively summarized. Your task is to extract a **flat, deduplicated list of key facts** from the "
    "entire history.\nVery detailed, comprehensive.\n\n**Rules:**\n- Output a bulleted list of non-re"
    "dundant facts, decisions, and current file states.\n- **De-duplicate:** If the same fact appears"
    " across multiple previous summaries, include it only once.\n- **Discard** narrative flow, transi"
    "tional language, and meta-commentary about the compaction process itself.\n- **Preserve:** error"
    " messages, final solutions, tool output results, architectural decisions, design rationale, and "
    "current task state.\n- **Keep:** project overview (purpose, scope, tech stack), key decisions wi"
    "th rejected alternatives, current state (what works, merged/verified, active branch, test result"
    "s), important files with roles, architecture/data flow, dependencies (added/removed/upgraded), r"
    "isks/rollback strategy, technical notes (patterns, constraints, APIs, env setup, performance/sec"
    "urity).\n- **Condense:** long code blocks \xE2\x86\x92 signatures + key logic only (keep full ve"
    "rsion if < 20 lines).\n- **Discussions:** extract decisions and action items only.\n\n**Length:*"
    "* Aim to reduce the context to a compact fact list while preserving all essential information.\n"
    "\n**Output Structure:**\n\n```xml\n<current_focus>\n[What we're working on now]\n</current_focus"
    ">\n\n<environment>\n- OS: [os]\n- Work dir: [path]\n- Key deps: [packages]\n- [Other relevant se"
    "tup]\n</environment>\n\n<code_state>\n[Critical file states \xE2\x80\x94 signatures + key change"
    "s]\n</code_state>\n\n<facts>\n- [Decision] [Decision description and rationale]\n- [Code] [File "
    "path / function / key logic]\n- [Env] [Environment detail]\n- [Error] [Error message and resolut"
    "ion]\n</facts>\n\n<active_issues>\n- [Issue]: [Status/Next steps]\n</active_issues>\n\n<project_"
    "overview>\n- Purpose: [project purpose]\n- Scope: [project scope]\n- Tech stack: [tech stack]\n<"
    "/project_overview>\n\n<key_decisions>\n- [Decision]: [Rationale, rejected alternatives]\n</key_d"
    "ecisions>\n\n<current_state>\n- What works: [summary]\n- Merged/Verified: [status]\n- Active bra"
    "nch: [branch]\n- Test results: [summary]\n</current_state>\n\n<important_files>\n- [path/to/file"
    ".py]: [role]\n</important_files>\n\n<architecture>\n- Major components: [list]\n- Interfaces: [s"
    "ummary]\n- Data flow: [summary]\n- Schema changes: [summary]\n</architecture>\n\n<dependencies>"
    "\n- Added: [packages]\n- Removed: [packages]\n- Upgraded: [packages]\n</dependencies>\n\n<risks_"
    "rollback>\n- Breaking changes: [details]\n- Migration steps: [details]\n- Revert strategy: [deta"
    "ils]\n</risks_rollback>\n\n<technical_notes>\n- Patterns: [details]\n- Constraints: [details]\n-"
    " APIs: [details]\n- Env setup: [details]\n- Performance/Security: [details]\n</technical_notes>"
    "\n\n<important_context>\n- [Crucial information not covered above]\n</important_context>\n```\n";

// _MODE_GUIDANCE[CompactMode.BALANCED] (compaction.py:32).
constexpr const char* kBalancedModeGuidance =
    "**Compaction Style Guidance:** Be balanced. Preserve essential context while condensing redundan"
    "t information. Keep current task state, errors and solutions, code state, design decisions, and "
    "TODO items."
;

// _DECISION_SECTION_GUIDANCE (compaction.py:220) -- disabled by
// default (SimpleCompaction.decision_section_enabled=False).
constexpr const char* kDecisionSectionGuidance =
    "\n\n**Required Summary Sections:**\nYour summary MUST include these two sections with exact head"
    "ings:\n## Decisions & Conclusions\n- Decisions already made and their rationale; approaches alre"
    "ady evaluated and rejected (with the rejection reason); assumptions currently treated as valid."
    "\n## Verification Status\n- What has been verified to work (and how it was verified); what remai"
    "ns unverified."
;

} // namespace
} // namespace soul
} // namespace runtime
} // namespace kimix
