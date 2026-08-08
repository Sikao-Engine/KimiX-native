/*
 * grep_pattern.h - Newline-aware grep pattern kernels (kimix::runtime::tools).
 *
 * Plan: native port of kimi-agent commit 0582e09 "Study from hermes"
 * (kimi-cli/src/kimi_cli/tools/file/grep_local.py):
 *   _pattern_has_regex_newline (81-95) and _multiline_pattern (98-114).
 *
 * The reference regex (?<!\\)(?:\\\\)*\\n matches a literal backslash+n iff
 * an ODD number of consecutive backslashes immediately precede the 'n'
 * (even backslashes mean a literal backslash+n search). The scanners below
 * replicate that exactly with a single pass over the pattern, plus the
 * real-newline check. ASCII-only input (callers route non-ASCII patterns to
 * the pure-Python mirror).
 *
 * Pure C++ kernel: no Python includes; GIL released in the binding layer.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace tools {

// True when the pattern contains a literal newline OR a regex ``\n`` escape
// (odd number of backslashes before 'n').
KIMIX_RUNTIME_API bool pattern_has_regex_newline(kimix::string_view pattern);

// Rewrite newline constructs so they also match CRLF: normalize explicit
// CRLF, replace every regex ``\n`` escape with the literal 5-char ``\r?\n``,
// then every real newline with the same literal.
KIMIX_RUNTIME_API kimix::string multiline_pattern(kimix::string_view pattern);

} // namespace tools
} // namespace runtime
} // namespace kimix
