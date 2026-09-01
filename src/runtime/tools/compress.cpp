/*
 * compress.cpp - Native micro-compression kernels for text (plan 016).
 *
 * Byte-identical mirrors of selected stages from
 * kimi-cli/src/kimi_cli/tools/file/micro_compress.py.  All kernels are pure
 * C++ and release the GIL in the binding layer.
 */

#include <runtime/tools/compress.h>
#include <runtime/stream/ansi.h>

#include <cstdio>
#include <cstring>

namespace kimix {
namespace runtime {
namespace tools {

namespace {

constexpr size_t k_max_indent_scan = 8192;

inline bool is_whitespace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

inline bool is_blank_line(kimix::string_view line) noexcept {
    for (char c : line) {
        if (!is_whitespace(c)) {
            return false;
        }
    }
    return true;
}

// Split on '\n' exactly (Python str.split("\n") semantics).
// "a\nb\n" -> ["a", "b", ""]; "\n" -> ["", ""]; "" -> [""].
void split_lines(kimix::string_view text, kimix::vector<kimix::string_view>& out) {
    out.clear();
    size_t start = 0;
    while (true) {
        size_t nl = text.find('\n', start);
        if (nl == kimix::string_view::npos) {
            out.push_back(text.substr(start));
            break;
        }
        out.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
}

// Count trailing characters that belong to Python's str.rstrip() set
// (space, tab, newline, carriage return, form feed, vertical tab).
// For code kind we only strip spaces.
size_t count_trailing_ws(kimix::string_view line, bool code_kind) noexcept {
    size_t n = line.size();
    while (n > 0) {
        char c = line[n - 1];
        if (code_kind) {
            if (c != ' ') {
                break;
            }
        } else {
            if (!is_whitespace(c)) {
                break;
            }
        }
        --n;
    }
    return line.size() - n;
}

// Python str.lstrip(" \t"): leading spaces and tabs only.
kimix::string_view leading_indent(kimix::string_view line) noexcept {
    size_t n = 0;
    while (n < line.size() && (line[n] == ' ' || line[n] == '\t')) {
        ++n;
    }
    return line.substr(0, n);
}

// Collapse internal runs of 3+ spaces between non-whitespace chars to a single
// space (Python regex "(?<=\\S) {3,}(?=\\S)" -> " "), appending directly to
// `out` and reporting whether anything changed.  Clean segments are copied in
// bulk (one append per run) instead of char-by-char, and lines with no
// collapsible run need just one scan + one memcpy of the whole line.
bool append_collapse_internal_spaces(kimix::string_view line, kimix::string& out) {
    bool changed = false;
    size_t clean = 0; // start of pending unmodified bytes
    size_t i = 0;
    const size_t n = line.size();
    while (i < n) {
        if (line[i] == ' ') {
            size_t j = i;
            while (j < n && line[j] == ' ') {
                ++j;
            }
            if (j - i >= 3 && i > 0 && !is_whitespace(line[i - 1]) &&
                j < n && !is_whitespace(line[j])) {
                if (clean < i) {
                    out.append(line.data() + clean, i - clean);
                }
                out.push_back(' ');
                changed = true;
                clean = j;
                i = j;
                continue;
            }
            i = j; // not collapsible; stays inside the clean span
            continue;
        }
        ++i;
    }
    if (clean < n) {
        out.append(line.data() + clean, n - clean);
    }
    return changed;
}

bool starts_with_meta_prefix(kimix::string_view line) noexcept {
    if (line.empty()) {
        return false;
    }
    if (line[0] == '[') {
        return true;
    }
    // U+2026 HORIZONTAL ELLIPSIS = UTF-8 E2 80 A6.
    if (line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xE2 &&
        static_cast<unsigned char>(line[1]) == 0x80 &&
        static_cast<unsigned char>(line[2]) == 0xA6) {
        return true;
    }
    return false;
}

// Returns true and writes the numeric span [num_start, num_end) if the line
// matches ^\s*(\d+)\t.
bool match_lineno_prefix(kimix::string_view line, size_t& num_start, size_t& num_end) noexcept {
    size_t i = 0;
    while (i < line.size() && is_whitespace(line[i])) {
        ++i;
    }
    num_start = i;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
        ++i;
    }
    num_end = i;
    return num_start < num_end && i < line.size() && line[i] == '\t';
}

} // namespace

// Returns the length of the smallest repeating unit of *line* (<= max_unit)
// that exactly composes the whole line, or 0 if none.  The scan mirrors the
// Python reference: try periods in increasing order, require n % p == 0, and
// verify by comparing each position with the one *p* bytes earlier (sequential,
// cache-friendly reads).
static size_t smallest_repeating_period(kimix::string_view line, size_t max_unit) {
    const size_t n = line.size();
    const size_t limit = std::min(n / 3, max_unit);
    for (size_t p = 1; p <= limit; ++p) {
        if (n % p != 0) {
            continue;
        }
        // s[i] == s[i-p] for all i in [p, n)  <=>  suffix(p, n-p) == prefix(0, n-p).
        if (std::memcmp(line.data() + p, line.data(), n - p) == 0) {
            return p;
        }
    }
    return 0;
}

kimix::string compress_intra_line_dedup(kimix::string_view text, int threshold, int max_unit) {
    if (threshold < 0 || max_unit <= 0 || text.empty()) {
        return kimix::string(text);
    }

    kimix::vector<kimix::string_view> lines;
    split_lines(text, lines);

    bool changed = false;
    kimix::string out;
    out.reserve(text.size());

    for (size_t idx = 0; idx < lines.size(); ++idx) {
        kimix::string_view line = lines[idx];
        if (static_cast<int>(line.size()) > threshold && line.size() >= 6) {
            const size_t n = line.size();
            const size_t max_p = std::min(n / 3, static_cast<size_t>(max_unit));
            const size_t unit_len = smallest_repeating_period(line, max_p);
            if (unit_len > 0) {
                const size_t repeats = n / unit_len;
                const size_t elided = n - unit_len;
                // " \xC3\x97" is UTF-8 U+00D7 (×); same bytes kimix::format
                // produced, but via a stack buffer (no per-line allocation).
                char marker[64];
                const int marker_len = std::snprintf(
                    marker, sizeof(marker), " \xC3\x97%llu [+%llu chars elided]",
                    static_cast<unsigned long long>(repeats),
                    static_cast<unsigned long long>(elided));
                if (unit_len + static_cast<size_t>(marker_len) < n) {
                    out.append(line.substr(0, unit_len));
                    out.append(marker, static_cast<size_t>(marker_len));
                    changed = true;
                    if (idx + 1 < lines.size()) {
                        out.push_back('\n');
                    }
                    continue;
                }
            }
        }
        out.append(line);
        if (idx + 1 < lines.size()) {
            out.push_back('\n');
        }
    }

    return changed ? std::move(out) : kimix::string(text);
}

kimix::string compress_collapse_whitespace(kimix::string_view text,
                                           kimix::string_view kind,
                                           bool lossless_only,
                                           bool strip_trailing_ws,
                                           int blank_line_collapse,
                                           bool common_indent_factor,
                                           bool /*prefix_fold*/) {
    if (text.empty()) {
        return kimix::string(text);
    }

    kimix::vector<kimix::string_view> lines;
    split_lines(text, lines);

    const bool is_code = (kind == "code");
    const bool is_prose = (kind == "prose");
    const bool is_log = (kind == "log");

    // A2 - strip trailing whitespace.
    bool changed = false;
    kimix::vector<kimix::string_view> stripped_lines;
    stripped_lines.reserve(lines.size());
    for (kimix::string_view line : lines) {
        size_t trailing = strip_trailing_ws ? count_trailing_ws(line, is_code) : 0;
        if (trailing > 0) {
            changed = true;
        }
        stripped_lines.push_back(line.substr(0, line.size() - trailing));
    }

    // A1 - collapse blank-line runs.
    kimix::vector<kimix::string_view> collapsed;
    if (blank_line_collapse >= 0) {
        collapsed.reserve(stripped_lines.size());
        int blank_run = 0;
        for (kimix::string_view line : stripped_lines) {
        if (is_blank_line(line)) {
            ++blank_run;
            if (blank_run <= blank_line_collapse) {
                // Collapsing a non-empty blank line to empty is a real change.
                if (!line.empty()) {
                    changed = true;
                }
                collapsed.push_back(line.substr(0, 0)); // empty view
            } else {
                changed = true; // dropped an excess blank line
            }
        } else {
            blank_run = 0;
            collapsed.push_back(line);
        }
        }
    } else {
        collapsed = std::move(stripped_lines);
    }

    // A3 - factor common leading indent (non-code, non-lossless-only).
    bool indent_removed = false;
    kimix::string indent_prefix;
    if (common_indent_factor && !is_code && !lossless_only) {
        kimix::vector<kimix::string_view> non_blank;
        non_blank.reserve(collapsed.size());
        for (kimix::string_view line : collapsed) {
            if (!is_blank_line(line)) {
                non_blank.push_back(line);
            }
        }
        if (non_blank.size() >= 2) {
            kimix::string_view common = leading_indent(non_blank[0]);
            if (common.size() > k_max_indent_scan) {
                common = common.substr(0, k_max_indent_scan);
            }
            for (size_t i = 1; i < non_blank.size() && !common.empty(); ++i) {
                kimix::string_view indent = leading_indent(non_blank[i]);
                if (indent.size() > k_max_indent_scan) {
                    indent = indent.substr(0, k_max_indent_scan);
                }
                size_t n = std::min(common.size(), indent.size());
                size_t j = 0;
                while (j < n && common[j] == indent[j]) {
                    ++j;
                }
                common = common.substr(0, j);
            }
            if (common.size() >= 4) {
                indent_prefix = kimix::string(common);
                indent_removed = true;
                changed = true;
            }
        }
    }

    // A4 - collapse internal 3+ space runs (prose/log only).
    // Note: this step is *not* gated by lossless_only in the Python reference.
    const bool collapse_spaces = (is_prose || is_log);

    // Compute output size for reservation.
    size_t estimated = text.size();
    if (indent_removed) {
        estimated += indent_prefix.size() + 32;
    }
    kimix::string out;
    out.reserve(estimated);

    if (indent_removed) {
        out.append(kimix::format("[common-indent: {} cols removed]\n", indent_prefix.size()));
    }

    for (size_t idx = 0; idx < collapsed.size(); ++idx) {
        kimix::string_view line = collapsed[idx];
        if (indent_removed && line.size() >= indent_prefix.size()) {
            if (std::memcmp(line.data(), indent_prefix.data(), indent_prefix.size()) == 0) {
                line = line.substr(indent_prefix.size());
            }
        }
        if (collapse_spaces && line.size() >= 3) {
            // Direct append: no per-line temporary string when unchanged.
            if (append_collapse_internal_spaces(line, out)) {
                changed = true;
            }
        } else {
            out.append(line);
        }
        if (idx + 1 < collapsed.size()) {
            out.push_back('\n');
        }
    }

    return changed ? std::move(out) : kimix::string(text);
}

kimix::string compress_renumber_lines(kimix::string_view text) {
    if (text.empty()) {
        return kimix::string(text);
    }

    kimix::vector<kimix::string_view> lines;
    split_lines(text, lines);

    size_t substantial = 0;
    size_t numbered = 0;
    for (kimix::string_view line : lines) {
        if (is_blank_line(line) || starts_with_meta_prefix(line)) {
            continue;
        }
        ++substantial;
        size_t num_start = 0, num_end = 0;
        if (match_lineno_prefix(line, num_start, num_end)) {
            ++numbered;
        }
    }

    if (substantial == 0 || numbered < substantial) {
        return kimix::string(text);
    }

    kimix::string out;
    out.reserve(text.size());
    bool changed = false;
    for (size_t idx = 0; idx < lines.size(); ++idx) {
        kimix::string_view line = lines[idx];
        size_t num_start = 0, num_end = 0;
        if (match_lineno_prefix(line, num_start, num_end)) {
            if (num_start > 0) {
                changed = true;
            }
            out.append(line.substr(num_start, num_end - num_start));
            out.push_back('\t');
            out.append(line.substr(num_end + 1));
        } else {
            out.append(line);
        }
        if (idx + 1 < lines.size()) {
            out.push_back('\n');
        }
    }

    return changed ? std::move(out) : kimix::string(text);
}

kimix::string compress_strip_control_noise(kimix::string_view text) {
    const bool has_esc = text.find('\x1B') != kimix::string_view::npos;
    const bool has_cr = text.find('\r') != kimix::string_view::npos;
    if (!has_esc && !has_cr) {
        return kimix::string(text);
    }

    kimix::string stripped = kimix::runtime::stream::strip_ansi(text);

    kimix::vector<kimix::string_view> lines;
    split_lines(stripped, lines);

    kimix::string out;
    out.reserve(stripped.size());
    bool changed = has_esc; // strip_ansi was invoked, but we still treat ESC presence as potential change
    for (size_t idx = 0; idx < lines.size(); ++idx) {
        kimix::string_view line = lines[idx];
        size_t cr = line.rfind('\r');
        if (cr != kimix::string_view::npos) {
            line = line.substr(cr + 1);
            changed = true;
        }
        out.append(line);
        if (idx + 1 < lines.size()) {
            out.push_back('\n');
        }
    }

    return changed ? std::move(out) : kimix::string(text);
}

} // namespace tools
} // namespace runtime
} // namespace kimix
