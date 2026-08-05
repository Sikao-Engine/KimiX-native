/*
 * grep_scan.cpp - Single-pass line scanner with incremental byte offsets
 *                 (plan 013).
 *
 * Line splitting matches Python splitlines() for LF-only input: a trailing
 * '\n' does not create an extra empty line ("a\n" -> ["a"]; "\n" -> [""];
 * "" -> []). CRLF input yields a trailing '\r' on each line (the reference
 * splitlines() would strip it; the shim's matcher operates on the raw line
 * bytes and the parity tests use LF/CRLF-normalized corpora -- see the
 * python/tests/test_tools.py note).
 */

#include <runtime/tools/grep_scan.h>

namespace kimix {
namespace runtime {
namespace tools {

void scan_lines(kimix::string_view content,
                kimix::function<bool(kimix::string_view line, uint32_t line_index)>&& matcher,
                kimix::vector<grep_hit>& out) {
    out.clear();
    const size_t n = content.size();
    size_t line_start = 0;
    uint32_t line_index = 0;
    while (line_start < n) {
        const size_t nl = content.find('\n', line_start);
        const size_t line_end = (nl == kimix::string_view::npos) ? n : nl;
        const kimix::string_view line =
            content.substr(line_start, line_end - line_start);
        if (matcher && matcher(line, line_index)) {
            out.push_back(grep_hit{line_index, static_cast<uint32_t>(line_start),
                                   static_cast<uint32_t>(line.size())});
        }
        line_start = (nl == kimix::string_view::npos) ? n : nl + 1;
        ++line_index;
    }
}

} // namespace tools
} // namespace runtime
} // namespace kimix
