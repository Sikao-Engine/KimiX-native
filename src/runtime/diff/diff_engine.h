/*
 * diff_engine.h — Line-level diff, hunk extraction, and inline diff ranges.
 *
 * Kernels in this file are pure C++ (no Python dependency) and are compiled
 * into the runtime shared library. They mirror the behavior of
 * difflib.SequenceMatcher with autojunk=False, difflib.unified_diff, and the
 * inline-diff helpers in kimi_cli/utils/rich/diff_render.py.
 */

#pragma once

#include <core/kimix_core.h>

#include <tuple>

namespace kimix {
namespace runtime {
namespace diff {

// One line-level opcode: tag is "equal", "delete", "insert", or "replace".
// Ranges are [start, end) indices into the old/new line arrays.
struct opcode {
    kimix::string tag;
    size_t old_start = 0;
    size_t old_end = 0;
    size_t new_start = 0;
    size_t new_end = 0;
};

// A hunk produced by diff_hunks.
struct hunk {
    size_t old_start = 1; // 1-based line number
    size_t new_start = 1; // 1-based line number
    kimix::vector<kimix::string> old_lines;
    kimix::vector<kimix::string> new_lines;
};

// A pair of integer offsets [start, end).
struct offset_range {
    size_t start = 0;
    size_t end = 0;
};

// Split *text* into lines using Python str.splitlines() semantics.
// When keepends is true each line retains its terminator (the full boundary
// sequence: \n, \r, \r\n, \v, \f, \x1c-\x1e, \u2028, \u2029). When false the
// terminators are stripped.
KIMIX_RUNTIME_API kimix::vector<kimix::string> split_lines(kimix::string_view text, bool keepends);

// Compute opcodes equivalent to SequenceMatcher.get_opcodes(autojunk=False).
KIMIX_RUNTIME_API void compute_opcodes(const kimix::vector<kimix::string>& old_lines,
                                       const kimix::vector<kimix::string>& new_lines,
                                       kimix::vector<opcode>& out);

// Group opcodes with *n* context lines, equivalent to
// SequenceMatcher.get_grouped_opcodes(n).
KIMIX_RUNTIME_API void group_opcodes(const kimix::vector<opcode>& opcodes,
                                     size_t n,
                                     kimix::vector<kimix::vector<opcode>>& out);

// Produce a unified diff string.
// Inputs are UTF-8 byte strings (surrogatepass compatible). Lines are split
// with splitlines(keepends=true), the last line is forced to end with '\n',
// then headers and context/+/− lines are joined using *lineterm* for headers
// and preserving content line terminators.
KIMIX_RUNTIME_API kimix::string unified_diff(kimix::string_view old_text,
                                             kimix::string_view new_text,
                                             kimix::string_view path,
                                             bool include_file_header,
                                             kimix::string_view lineterm);

// Produce structured hunks with configurable context lines.
// Lines are split with splitlines(keepends=false).
KIMIX_RUNTIME_API kimix::vector<hunk> diff_hunks(kimix::string_view old_text,
                                                 kimix::string_view new_text,
                                                 size_t context_lines);

// Compute inline delete/insert highlight ranges for a pair of lines.
// Tabs are expanded to tab_size (default 4) using Python expandtabs rules.
// Returns (delete_ranges, insert_ranges) as codepoint offsets into the
// tab-expanded strings. Ranges are empty when the similarity ratio is below
// min_ratio.
KIMIX_RUNTIME_API std::tuple<kimix::vector<offset_range>, kimix::vector<offset_range>>
inline_diff_ranges(kimix::string_view old_line,
                   kimix::string_view new_line,
                   double min_ratio,
                   size_t tab_size);

} // namespace diff
} // namespace runtime
} // namespace kimix
