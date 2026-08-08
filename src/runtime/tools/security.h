/*
 * security.h - Security kernels: output redaction, env scrubbing, workdir
 * validation, bounded output append (kimix::runtime::tools).
 *
 * Plan: native port of kimi-agent commit 0582e09 "Study from hermes"
 * (src/kimix/tools/security.py, src/kimix/tools/background/utils.py).
 *
 * The redaction kernel implements the reference's 10 chained `regex`
 * substitutions as hand-rolled ASCII scanners. Callers MUST route non-ASCII
 * input to the pure-Python mirror (see python/kimix_native/tools.py): the
 * scanners use ASCII-only \s = [ \t\n\r\f\v], \w = [A-Za-z0-9_],
 * \b = ASCII word boundary, which is bit-exact with the `regex` module on
 * pure-ASCII input.  All regex semantics are preserved: leftmost match,
 * ordered alternation, greedy runs, lazy .*? (first valid END after BEGIN),
 * non-overlapping replacement (scan resumes after each match end).
 *
 * validate_workdir is pure string math and accepts any UTF-8 input; the
 * rejection message replicates Python's `{char!r}` exactly (printable chars
 * verbatim, escapes \x/\u/\U otherwise) via a generated non-printable
 * codepoint table (Python 3.14 / Unicode 16.0).
 *
 * Pure C++ kernel: no Python includes; GIL released in the binding layer.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace tools {

// One name/value pair of a child environment (insertion order matters).
struct env_entry {
    kimix::string name;
    kimix::string value;
};

// Result of the bounded output append: the new buffer content and whether a
// truncation happened (mirrors background/utils.py bounded_append).
struct bounded_result {
    kimix::string content;
    bool truncated = false;
};

// The reference's 10 chained redactions applied in order
// (URL userinfo -> JWT -> PEM -> github_pat -> gh[pousr]_ -> glpat -> AKIA ->
// auth header -> password/secret assignment -> bare bearer).
// ASCII input only (callers route non-ASCII to the Python mirror).
KIMIX_RUNTIME_API kimix::string redact_sensitive_output(kimix::string_view output);

// Keep entries whose uppercased name starts with a safe prefix; drop entries
// whose uppercased name contains a secret substring; insertion order is
// preserved. Names are ASCII (callers route non-ASCII keys to the mirror).
KIMIX_RUNTIME_API void scrub_child_env(const kimix::vector<env_entry>& env,
                                       kimix::vector<env_entry>& out);

// Returns an error message (exact reference wording incl. Python repr of the
// first offending character) or nullopt when the workdir is safe.
// Accepts any UTF-8 input.
KIMIX_RUNTIME_API kimix::optional<kimix::string> validate_workdir(kimix::string_view workdir);

// bounded_append(content, text, cap): returns (new_content, truncated).
// Replicates int(cap * 0.4) head/tail split with the reference marker line.
KIMIX_RUNTIME_API bounded_result bounded_append(kimix::string_view content,
                                                kimix::string_view text,
                                                int64_t cap);

} // namespace tools
} // namespace runtime
} // namespace kimix
