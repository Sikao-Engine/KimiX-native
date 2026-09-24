// tool_types.cpp - Shared line-stream helpers for the built-in tool kernels.
//
// Ports of the pure helpers in
// C:/dev/kimi-agent/kimi-cli/src/kimi_cli/tools/file/output_utils.py
// (fold_lines, dedup_lines, truncate_line, parse_rtk_rg_output's marker text)
// plus the byte-capped join of grep_local.py 618-632 (glob.py 631-637 runs the
// same loop inline). Line lengths are measured in *code points* to match
// Python's len(str); byte slicing always happens on a UTF-8 boundary.
//
// Parity evidence: tests/unit/builtin_tools/tool_types_goldens.inc, generated
// by scripts/gen_tool_types_goldens.py from the reference implementation, and
// replayed by tests/unit/builtin_tools/test_tool_types.cpp.

#include "builtin_tools/tool_types.h"

#include "builtin_tools/utf8_util.h"

namespace kimix::builtin_tools {

void truncate_line(kimix::string_view text, size_t max_len, kimix::string &out) {
    const size_t len = utf8_code_point_count(text);
    if (len <= max_len) {
        out.assign(text.data(), text.size());
        return;
    }
    // marker = "… [+K chars]" where K is the number of *removed* characters.
    const size_t removed = len - max_len;
    kimix::string marker;
    {
        kimix::StringScratch ss;
        ss << "\xE2\x80\xA6 [+" << removed << " chars]"; // U+2026 horizontal ellipsis
        marker = std::move(ss.string());
    }
    const size_t marker_len = utf8_code_point_count(marker);
    if (marker_len >= max_len) {
        out.assign(text.data(), utf8_byte_offset_of_code_point(text, max_len));
        return;
    }
    out.assign(text.data(),
               utf8_byte_offset_of_code_point(text, max_len - marker_len));
    out += marker;
}

// Byte-budget join. The reference is grep_local._join_with_byte_limit
// (grep_local.py 618-632); glob.py 631-637 runs exactly the same loop inline,
// and the golden generator asserts the two agree over the whole corpus.
//
// Python appends the line FIRST and tests `n_bytes >= max_bytes` afterwards,
// so (a) the line that *reaches* the budget is kept, (b) `truncated` is set
// even when that line happened to be the last one, and (c) the separator
// depends on how many lines were collected (a leading empty line still gives
// the next line its '\n'). `omitted` is the native extension: the number of
// input lines after the one that crossed the budget.
//
// The caller must pass valid UTF-8: Python measures
// `len(line.encode("utf-8"))`, which raises UnicodeEncodeError otherwise.
// (grep::join_with_byte_limit is the variant that validates and can signal it.)
void join_with_byte_limit(kimix::span<const kimix::string> lines, size_t max_bytes,
                          kimix::string &out, bool &truncated, size_t &omitted) {
    out.clear();
    truncated = false;
    omitted = 0;
    // The full join is an upper bound for what can be appended (`total` already
    // counts every separator), so a single reservation is always enough.
    size_t total = 0;
    for (const auto &l : lines) {
        total += l.size() + 1u;
    }
    out.reserve(total);
    size_t n_bytes = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i != 0u) {
            out.push_back('\n');
            n_bytes += 1u;
        }
        out.append(lines[i].data(), lines[i].size());
        n_bytes += lines[i].size();
        if (n_bytes >= max_bytes) {
            truncated = true;
            omitted = lines.size() - (i + 1u);
            break;
        }
    }
}

void fold_lines(kimix::span<const kimix::string> lines, size_t max_lines,
                size_t head, size_t tail, kimix::vector<kimix::string> &out,
                size_t &omitted) {
    out.clear();
    omitted = 0;
    const size_t n = lines.size();
    if (max_lines == 0 || n <= max_lines) {
        out.assign(lines.begin(), lines.end());
        return;
    }
    size_t head_count = head;
    size_t tail_count = tail;
    if (head_count + tail_count > max_lines) {
        // Keep the caller-specified head, cap the tail to the budget.
        tail_count = max_lines > head_count ? max_lines - head_count : 0u;
    }
    const size_t kept = head_count + tail_count;
    if (kept >= n) {
        out.assign(lines.begin(), lines.end());
        return;
    }
    omitted = n - kept;
    out.reserve(kept + 1u);
    for (size_t i = 0; i < head_count; i++) {
        out.push_back(lines[i]);
    }
    {
        kimix::StringScratch ss;
        ss << "\xE2\x80\xA6 (" << omitted << " lines omitted) \xE2\x80\xA6";
        out.push_back(std::move(ss.string()));
    }
    for (size_t i = n - tail_count; i < n; i++) {
        out.push_back(lines[i]);
    }
}

void dedup_lines(kimix::span<const kimix::string> lines, size_t min_repeats,
                 kimix::vector<kimix::string> &out, size_t &saved) {
    out.clear();
    saved = 0;
    if (min_repeats < 2) {
        min_repeats = 2;
    }
    const size_t n = lines.size();
    if (n < 2) {
        out.assign(lines.begin(), lines.end());
        return;
    }
    out.reserve(n);
    size_t i = 0;
    while (i < n) {
        size_t j = i + 1;
        while (j < n && lines[j] == lines[i]) {
            j++;
        }
        const size_t run_len = j - i;
        if (run_len >= min_repeats) {
            kimix::StringScratch ss;
            ss << lines[i] << "  (" << (run_len - 1) << " repeats)";
            out.push_back(std::move(ss.string()));
            saved += run_len - 1;
        } else {
            for (size_t k = i; k < j; k++) {
                out.push_back(lines[k]);
            }
        }
        i = j;
    }
}

} // namespace kimix::builtin_tools
