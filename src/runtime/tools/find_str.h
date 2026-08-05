/*
 * find_str.h - Case-insensitive substring search per file line
 *              (kimix::runtime::tools).
 *
 * Plan 013: native port of kimi-agent find_str.py::find_in_file (98-133).
 * Semantics (verified against the reference):
 *   - the file is read with readlines() (terminators KEPT): the search runs
 *     on each line INCLUDING its trailing '\n', so a needle ending in '\n'
 *     can match at a line end (reference quirk, replicated);
 *   - all occurrences per line, OVERLAPPING (search resumes at idx + 1);
 *   - case-insensitive folding is full Unicode str.lower() in the reference;
 *     the kernel folds ASCII A-Z only and the shim routes input containing
 *     any non-ASCII byte to the _compat mirror (documented deviation);
 *   - empty needle: matches at every position 0..len inclusive per line
 *     (reference find("") semantics; len+1 matches per line);
 *   - result fields are 0-based in the kernel: line_index (0-based), col
 *     (byte offset), length (byte length of the needle). The shim adds 1 to
 *     line/col to build the reference's 1-based {file, line, column, content}
 *     records (content = line.rstrip("\n\r")).
 *
 * Search algorithm: Boyer-Moore-Horspool over the folded needle with on-the-
 * fly ASCII folding (no per-line lowercase copies).
 *
 * Pure C++ kernel: no Python includes; GIL released in the binding layer.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace tools {

struct find_match {
    uint32_t line_index; // 0-based
    uint32_t col;        // 0-based byte offset within the line
    uint32_t length;     // byte length of the needle
};

// All occurrences of `needle` in `content`, per line (readlines semantics).
// `out` is cleared first.
KIMIX_RUNTIME_API void find_in_file(kimix::string_view content,
                                    kimix::string_view needle,
                                    bool case_insensitive,
                                    kimix::vector<find_match>& out);

} // namespace tools
} // namespace runtime
} // namespace kimix
