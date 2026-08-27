// tool_types.cpp - Shared line-stream helpers for the built-in tool kernels.
//
// Ports of the pure helpers in
// C:/dev/kimi-agent/kimi-cli/src/kimi_cli/tools/file/output_utils.py
// (fold_lines, dedup_lines, truncate_line) plus the byte-capped join used by
// every tool result builder. Line lengths are measured in *code points* to
// match Python's len(str); byte slicing always happens on a UTF-8 boundary.

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

void join_with_byte_limit(kimix::span<const kimix::string> lines, size_t max_bytes,
                          kimix::string &out, bool &truncated, size_t &omitted) {
    out.clear();
    truncated = false;
    omitted = 0;
    // Reserve the budget once: worst case the whole payload plus separators.
    size_t total = 0;
    for (const auto &l : lines) {
        total += l.size() + 1u;
    }
    out.reserve(total < max_bytes ? total : max_bytes);
    for (size_t i = 0; i < lines.size(); i++) {
        const size_t add = lines[i].size() + (out.empty() ? 0u : 1u);
        if (!out.empty() && out.size() + add > max_bytes) {
            truncated = true;
            omitted = lines.size() - i;
            break;
        }
        if (!out.empty()) {
            out.push_back('\n');
        }
        out.append(lines[i].data(), lines[i].size());
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
