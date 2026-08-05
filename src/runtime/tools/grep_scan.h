/*
 * grep_scan.h - Single-pass line scanner with incremental byte offsets
 *               (kimix::runtime::tools).
 *
 * Plan 013: native replacement for the O(matches x n) counting pattern in
 * grep_local.py backup_grep (`content.count("\n", 0, m.start())` per match).
 * One pass over the content:
 *   - lines are split on '\n' (Python splitlines() semantics for LF input:
 *     a trailing terminator does not produce an extra empty line);
 *   - byte offsets are maintained incrementally (never counted per match);
 *   - the matcher predicate decides whether a line is included; it receives
 *     the line WITHOUT its terminator and the 0-based line index.
 *
 * grep_hit fields are 0-based: line_index, byte_offset (start of the line
 * within the content), line_len (line bytes, terminator excluded). The shim
 * converts to 1-based line numbers for the reference output format.
 *
 * The reference matcher is a compiled Python regex; the substring overload
 * in the bindings covers literal patterns (case-insensitive = ASCII fold,
 * non-ASCII routed to the Python fallback), and the per-line callback binding
 * (scan_lines_cb) keeps full regex semantics while computing offsets natively.
 *
 * Pure C++ kernel: no Python includes; GIL released in the binding layer.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace tools {

struct grep_hit {
    uint32_t line_index;  // 0-based
    uint32_t byte_offset; // 0-based start of the line in the content
    uint32_t line_len;    // line bytes (terminator excluded)
};

// `matcher` is invoked once per line (in order); when it returns true the
// line's hit is appended. `out` is cleared first.
KIMIX_RUNTIME_API void scan_lines(
    kimix::string_view content,
    kimix::function<bool(kimix::string_view line, uint32_t line_index)>&& matcher,
    kimix::vector<grep_hit>& out);

} // namespace tools
} // namespace runtime
} // namespace kimix
