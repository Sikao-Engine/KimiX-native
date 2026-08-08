/*
 * shell_safety.h - Shell-safety kernels: hardline command detection, deobfuscation
 * variants, foreground/background guidance, exit-code interpretation and failure
 * annotation (kimix::runtime::tools).
 *
 * Plan: native port of kimi-agent commit 0582e09 "Study from hermes":
 *   src/kimix/tools/file/bash/safety.py         (command_detection_variants,
 *                                                detect_hardline_command,
 *                                                check_hardline_blocked,
 *                                                foreground_background_guidance)
 *   src/kimix/tools/file/bash/output_enhance.py (_base_command_name,
 *                                                interpret_exit_code,
 *                                                annotate_failure)
 *
 * All command/text inputs are ASCII-only (callers route non-ASCII input to
 * the pure-Python mirror): `.lower()` / `.isalpha()` / `\b` / `\s` are
 * Unicode-aware in Python, so the native ASCII implementations are exact only
 * for pure-ASCII input.  Regex semantics preserved: leftmost match, ordered
 * alternation, greedy runs, finditer resumes after each match end.
 *
 * Pure C++ kernel: no Python includes; GIL released in the binding layer.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace tools {

// Result of the hardline detectors: (blocked, description).
struct hardline_result {
    bool blocked = false;
    kimix::optional<kimix::string> description; // set when blocked
};

// safety.py command_detection_variants (24-46): deduped deobfuscation
// variants of *command* (whitespace-collapsed original, dequoted+lowered,
// lowered-collapsed; at most 3 entries). Empty/whitespace-only -> [].
KIMIX_RUNTIME_API void command_detection_variants(kimix::string_view command,
                                                  kimix::vector<kimix::string>& out);

// safety.py detect_hardline_command (129-179): 7 ordered checks on the
// whitespace-collapsed lowercased command.
KIMIX_RUNTIME_API hardline_result detect_hardline_command(kimix::string_view command);

// safety.py check_hardline_blocked (182-193): run the detector over every
// deobfuscation variant; (True, desc) when any variant matches.
KIMIX_RUNTIME_API hardline_result check_hardline_blocked(kimix::string_view command);

// safety.py foreground_background_guidance (228-241): hint for long-lived
// commands (quoted spans ignored), else None.
KIMIX_RUNTIME_API kimix::optional<kimix::string>
foreground_background_guidance(kimix::string_view command);

// output_enhance.py _base_command_name (28-41): first non-assignment command
// word, directory-stripped, ".exe" suffix removed.
KIMIX_RUNTIME_API kimix::string base_command_name(kimix::string_view command);

// output_enhance.py interpret_exit_code (44-75): explanation for well-known
// non-zero exit codes, else None.
KIMIX_RUNTIME_API kimix::optional<kimix::string>
interpret_exit_code(kimix::string_view command, kimix::optional<int64_t> exit_code);

// output_enhance.py annotate_failure (78-114): single actionable hint for
// common failure signatures in the first min(len, 4000) chars, else None.
// `command` is accepted for signature compatibility but not inspected.
KIMIX_RUNTIME_API kimix::optional<kimix::string>
annotate_failure(kimix::string_view output, kimix::string_view command,
                 kimix::optional<int64_t> exit_code);

} // namespace tools
} // namespace runtime
} // namespace kimix
