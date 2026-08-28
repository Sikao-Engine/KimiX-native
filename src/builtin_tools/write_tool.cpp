// write_tool.cpp - Built-in agent tool "write" kernels (see write_tool.h).
//
// Ports the CPU-bound guards of kimi-cli/src/kimi_cli/tools/file/write.py and
// its supporting modules (auto_generated.py, conflict_detect.py, check_fmt.py,
// utils/diff.py).  Byte-exact message text is pinned by
// tests/unit/builtin_tools/test_write_tool.cpp against Python goldens.

#include "builtin_tools/write_tool.h"

#include <yyjson.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <utility>

#include <core/kimix_core.h>

#include "builtin_tools/utf8_util.h"

namespace kimix {
namespace builtin_tools {
namespace write {

// ---------------------------------------------------------------------------
// Internal helpers (write namespace, unity-safe: no anonymous namespace).
// ---------------------------------------------------------------------------

bool is_space_ascii(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

char to_lower_ascii(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(static_cast<unsigned char>(c) - 'A' + 'a');
    }
    return c;
}

bool is_word_char(char c) noexcept {
    const unsigned char u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') ||
           u == '_';
}

// Name chars of marker pattern 2 ([a-z0-9_.-] with regex.I -> incl. A-Z).
bool is_name_char(char c) noexcept {
    const unsigned char u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') ||
           u == '_' || u == '.' || u == '-';
}

kimix::string lower_ascii(kimix::string_view s) noexcept {
    kimix::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(to_lower_ascii(c));
    }
    return out;
}

kimix::string trim_ascii(kimix::string_view s) noexcept {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && is_space_ascii(s[b])) {
        ++b;
    }
    while (e > b && is_space_ascii(s[e - 1])) {
        --e;
    }
    return kimix::string(s.substr(b, e - b));
}

kimix::string_view strip_trailing_cr(kimix::string_view line) noexcept {
    if (!line.empty() && line.back() == '\r') {
        return line.substr(0, line.size() - 1);
    }
    return line;
}

kimix::string_view basename_of(kimix::string_view file_path) noexcept {
    const size_t slash = file_path.find_last_of("/\\");
    return (slash == kimix::string_view::npos) ? file_path : file_path.substr(slash + 1);
}

// Python pathlib suffix: from the last '.' in the basename; "" for dotfiles.
kimix::string_view suffix_of(kimix::string_view name) noexcept {
    const size_t dot = name.rfind('.');
    if (dot == kimix::string_view::npos || dot == 0) {
        return kimix::string_view();
    }
    return name.substr(dot);
}

void replace_all(kimix::string_view input, kimix::string_view from, kimix::string_view to,
                 kimix::string &out) noexcept {
    out.clear();
    if (from.empty() || input.empty()) {
        out = kimix::string(input);
        return;
    }
    size_t pos = 0;
    while (true) {
        const size_t found = input.find(from, pos);
        if (found == kimix::string_view::npos) {
            out.append(input.data() + pos, input.size() - pos);
            break;
        }
        out.append(input.data() + pos, found - pos);
        out.append(to.data(), to.size());
        pos = found + from.size();
    }
}

kimix::string join(const kimix::vector<kimix::string> &parts,
                   kimix::string_view sep) noexcept {
    if (parts.empty()) {
        return kimix::string();
    }
    kimix::string out;
    size_t total = 0;
    for (const auto &p : parts) {
        total += p.size();
    }
    out.reserve(total + sep.size() * (parts.size() - 1));
    out += parts[0];
    for (size_t i = 1; i < parts.size(); ++i) {
        out.append(sep.data(), sep.size());
        out += parts[i];
    }
    return out;
}

// Python str.split("\n") (keeps the trailing empty element).
void split_lf(kimix::string_view text, kimix::vector<kimix::string> &out) noexcept {
    out.clear();
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            out.emplace_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    out.emplace_back(text.substr(start));
}

// Python str.splitlines() semantics: \n \r \r\n \v \f \x1c-\x1e \x85
// U+2028 U+2029 (terminators stripped; same scan as runtime/diff split_lines
// with keepends=false).
void split_py_lines(kimix::string_view text, kimix::vector<kimix::string> &out) noexcept {
    out.clear();
    const size_t n = text.size();
    size_t start = 0;
    size_t i = 0;
    while (i < n) {
        size_t boundary_len = 0;
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\n' || c == '\r' || c == '\v' || c == '\f' || (c >= 0x1cu && c <= 0x1eu)) {
            boundary_len = 1;
            if (c == '\r' && i + 1 < n && static_cast<unsigned char>(text[i + 1]) == '\n') {
                boundary_len = 2;
            }
        } else if (c == 0xc2u && i + 1 < n &&
                   static_cast<unsigned char>(text[i + 1]) == 0x85u) {
            boundary_len = 2; // U+0085 NEXT LINE
        } else if (c == 0xe2u && i + 2 < n &&
                   static_cast<unsigned char>(text[i + 1]) == 0x80u &&
                   (static_cast<unsigned char>(text[i + 2]) == 0xa8u ||
                    static_cast<unsigned char>(text[i + 2]) == 0xa9u)) {
            boundary_len = 3; // U+2028 / U+2029
        }
        if (boundary_len > 0) {
            out.emplace_back(text.substr(start, i - start));
            start = i + boundary_len;
            i = start;
        } else {
            ++i;
        }
    }
    if (start < n) {
        out.emplace_back(text.substr(start));
    }
}

bool has_style(const kimix::vector<comment_style> &styles, comment_style style) noexcept {
    for (comment_style s : styles) {
        if (s == style) {
            return true;
        }
    }
    return false;
}

// Case-insensitive substring search (ASCII only).
size_t ci_find(kimix::string_view hay, kimix::string_view needle, size_t from = 0) noexcept {
    if (needle.empty()) {
        return from;
    }
    for (size_t i = from; i + needle.size() <= hay.size(); ++i) {
        bool eq = true;
        for (size_t k = 0; k < needle.size(); ++k) {
            if (to_lower_ascii(hay[i + k]) != to_lower_ascii(needle[k])) {
                eq = false;
                break;
            }
        }
        if (eq) {
            return i;
        }
    }
    return kimix::string_view::npos;
}

bool ci_match_at(kimix::string_view hay, size_t pos, kimix::string_view needle) noexcept {
    if (pos + needle.size() > hay.size()) {
        return false;
    }
    for (size_t k = 0; k < needle.size(); ++k) {
        if (to_lower_ascii(hay[pos + k]) != to_lower_ascii(needle[k])) {
            return false;
        }
    }
    return true;
}

// Skip one-or-more ASCII whitespace starting at `pos`; returns the new index
// (== pos when there is no whitespace).
size_t skip_ws(kimix::string_view h, size_t pos) noexcept {
    while (pos < h.size() && is_space_ascii(h[pos])) {
        ++pos;
    }
    return pos;
}

bool match_known_generator(kimix::string_view h, size_t pos, size_t &end) noexcept {
    // protoc(?:-gen-[\w-]+)?  - try the -gen- suffix first (greedy).
    if (ci_match_at(h, pos, "protoc")) {
        const size_t after = pos + 6;
        if (ci_match_at(h, after, "-gen-")) {
            size_t j = after + 5;
            if (j < h.size() && (is_word_char(h[j]) || h[j] == '-')) {
                while (j < h.size() && (is_word_char(h[j]) || h[j] == '-')) {
                    ++j;
                }
                end = j;
                return true;
            }
        }
        end = after;
        return true;
    }
    // Remaining alternatives in Python alternation order.  Longer variants
    // are listed before their prefixes so the optional suffixes win.
    static constexpr kimix::string_view k_alts[] = {
        "sqlc",        "buf",           "swagger-codegen", "swagger",
        "openapi-generator", "openapi", "grpc-gateway",    "mockery",
        "stringer",    "easyjson",      "deepcopy-gen",    "defaulter-gen",
        "conversion-gen", "client-gen", "lister-gen",      "informer-gen",
        "kysely-codegen", "napi-rs",
    };
    for (kimix::string_view alt : k_alts) {
        if (ci_match_at(h, pos, alt)) {
            end = pos + alt.size();
            return true;
        }
    }
    return false;
}

// Marker pattern 2: \bcode\s+generated\s+by\s+[a-z0-9_.-]+  (case-insensitive)
bool scan_pattern_code_generated(kimix::string_view h, size_t &begin, size_t &end) noexcept {
    size_t pos = 0;
    while ((pos = ci_find(h, "code", pos)) != kimix::string_view::npos) {
        if (pos > 0 && is_word_char(h[pos - 1])) {
            pos += 4;
            continue;
        }
        const size_t after_code = pos + 4;
        size_t i = skip_ws(h, after_code);
        if (i == after_code) { // need at least one \s between words
            pos += 4;
            continue;
        }
        if (!ci_match_at(h, i, "generated")) {
            pos += 4;
            continue;
        }
        const size_t after_generated = i + 9;
        i = skip_ws(h, after_generated);
        if (i == after_generated || !ci_match_at(h, i, "by")) {
            pos += 4;
            continue;
        }
        const size_t after_by = i + 2;
        i = skip_ws(h, after_by);
        if (i == after_by || i >= h.size() || !is_name_char(h[i])) {
            pos += 4;
            continue;
        }
        size_t j = i;
        while (j < h.size() && is_name_char(h[j])) {
            ++j;
        }
        begin = pos;
        end = j;
        return true;
    }
    return false;
}

// Marker pattern 3: \bthis\s+file\s+was\s+automatically\s+generated\b
bool scan_pattern_this_file(kimix::string_view h, size_t &begin, size_t &end) noexcept {
    size_t pos = 0;
    while ((pos = ci_find(h, "this", pos)) != kimix::string_view::npos) {
        if (pos > 0 && is_word_char(h[pos - 1])) {
            pos += 4;
            continue;
        }
        const size_t after_this = pos + 4;
        size_t i = skip_ws(h, after_this);
        if (i == after_this || !ci_match_at(h, i, "file")) {
            pos += 4;
            continue;
        }
        const size_t after_file = i + 4;
        i = skip_ws(h, after_file);
        if (i == after_file || !ci_match_at(h, i, "was")) {
            pos += 4;
            continue;
        }
        const size_t after_was = i + 3;
        i = skip_ws(h, after_was);
        if (i == after_was || !ci_match_at(h, i, "automatically")) {
            pos += 4;
            continue;
        }
        const size_t after_auto = i + 13;
        i = skip_ws(h, after_auto);
        if (i == after_auto || !ci_match_at(h, i, "generated")) {
            pos += 4;
            continue;
        }
        const size_t after = i + 9;
        if (after < h.size() && is_word_char(h[after])) {
            pos += 4;
            continue;
        }
        begin = pos;
        end = after;
        return true;
    }
    return false;
}

// Marker pattern 4: \bgenerated\s+by\s+<KNOWN_GENERATOR_PATTERN>\b
bool scan_pattern_generated_by(kimix::string_view h, size_t &begin, size_t &end) noexcept {
    size_t pos = 0;
    while ((pos = ci_find(h, "generated", pos)) != kimix::string_view::npos) {
        if (pos > 0 && is_word_char(h[pos - 1])) {
            pos += 9;
            continue;
        }
        const size_t after_generated = pos + 9;
        size_t i = skip_ws(h, after_generated);
        if (i == after_generated || !ci_match_at(h, i, "by")) {
            pos += 9;
            continue;
        }
        const size_t after_by = i + 2;
        i = skip_ws(h, after_by);
        if (i == after_by) {
            pos += 9;
            continue;
        }
        size_t gen_end = 0;
        if (!match_known_generator(h, i, gen_end)) {
            pos += 9;
            continue;
        }
        if (gen_end < h.size() && is_word_char(h[gen_end])) {
            pos += 9;
            continue;
        }
        begin = pos;
        end = gen_end;
        return true;
    }
    return false;
}

bool scan_header_markers(kimix::string_view header, kimix::string &matched) noexcept {
    // Pattern 1: @generated\b
    const size_t at = ci_find(header, "@generated");
    if (at != kimix::string_view::npos) {
        const size_t after = at + 10;
        if (after >= header.size() || !is_word_char(header[after])) {
            matched = kimix::string(header.substr(at, 10));
            return true;
        }
    }
    size_t begin = 0;
    size_t end = 0;
    if (scan_pattern_code_generated(header, begin, end)) {
        matched = kimix::string(header.substr(begin, end - begin));
        return true;
    }
    if (scan_pattern_this_file(header, begin, end)) {
        matched = kimix::string(header.substr(begin, end - begin));
        return true;
    }
    if (scan_pattern_generated_by(header, begin, end)) {
        matched = kimix::string(header.substr(begin, end - begin));
        return true;
    }
    return false;
}

// Loose int() approximation: optional ASCII spaces, optional sign, digits,
// optional trailing ASCII spaces.
int64_t parse_int_loose(kimix::string_view s, bool &ok) noexcept {
    ok = false;
    size_t i = 0;
    while (i < s.size() && is_space_ascii(s[i])) {
        ++i;
    }
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
        neg = (s[i] == '-');
        ++i;
    }
    const size_t digits_start = i;
    uint64_t v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        if (v > (std::numeric_limits<uint64_t>::max() - 9u) / 10u) {
            return 0; // overflow -> invalid
        }
        v = v * 10u + static_cast<uint64_t>(s[i] - '0');
        ++i;
    }
    if (i == digits_start) {
        return 0;
    }
    while (i < s.size() && is_space_ascii(s[i])) {
        ++i;
    }
    if (i != s.size()) {
        return 0;
    }
    if (neg) {
        if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1u) {
            return 0;
        }
        ok = true;
        if (v == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1u) {
            return std::numeric_limits<int64_t>::min();
        }
        return -static_cast<int64_t>(v);
    }
    if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return 0;
    }
    ok = true;
    return static_cast<int64_t>(v);
}

// Conflict state machine over an already-split line vector
// (conflict_detect.scan_conflict_lines).
void scan_conflict_lines_impl(const kimix::vector<kimix::string> &lines,
                              int32_t first_line_number,
                              kimix::vector<conflict_block> &out) noexcept {
    enum class scan_state : uint8_t { idle, ours, base, theirs };
    scan_state state = scan_state::idle;
    int32_t start_line = 0;
    int32_t separator_line = 0;
    int32_t base_line = -1;
    kimix::optional<kimix::string> ours_label;
    kimix::optional<kimix::string> base_label;
    kimix::optional<kimix::string> theirs_label;
    kimix::vector<kimix::string> ours_buf;
    kimix::vector<kimix::string> base_buf;
    kimix::vector<kimix::string> theirs_buf;

    auto reset = [&]() {
        state = scan_state::idle;
        start_line = 0;
        separator_line = 0;
        base_line = -1;
        ours_label.reset();
        base_label.reset();
        theirs_label.reset();
        ours_buf.clear();
        base_buf.clear();
        theirs_buf.clear();
    };

    for (size_t idx = 0; idx < lines.size(); ++idx) {
        const int32_t line_no = first_line_number + static_cast<int32_t>(idx);
        const kimix::string_view line = strip_trailing_cr(lines[idx]);

        if (state == scan_state::idle) {
            auto label = match_marker(line, k_ours_prefix);
            if (label.has_value()) {
                state = scan_state::ours;
                start_line = line_no;
                ours_label = label->empty() ? kimix::optional<kimix::string>()
                                            : kimix::optional<kimix::string>(*label);
                ours_buf.clear();
                base_buf.clear();
                theirs_buf.clear();
                base_line = -1;
                separator_line = 0;
            }
            continue;
        }

        if (state == scan_state::ours) {
            auto label = match_marker(line, k_base_prefix);
            if (label.has_value()) {
                state = scan_state::base;
                base_line = line_no;
                base_label = label->empty() ? kimix::optional<kimix::string>()
                                            : kimix::optional<kimix::string>(*label);
                continue;
            }
            if (is_separator(line)) {
                state = scan_state::theirs;
                separator_line = line_no;
                continue;
            }
            auto nested = match_marker(line, k_ours_prefix);
            if (nested.has_value()) {
                // Nested opener: restart at this line.
                start_line = line_no;
                ours_label = nested->empty() ? kimix::optional<kimix::string>()
                                             : kimix::optional<kimix::string>(*nested);
                ours_buf.clear();
                base_buf.clear();
                theirs_buf.clear();
                base_line = -1;
                separator_line = 0;
                continue;
            }
            ours_buf.emplace_back(line);
            continue;
        }

        if (state == scan_state::base) {
            if (is_separator(line)) {
                state = scan_state::theirs;
                separator_line = line_no;
                continue;
            }
            if (match_marker(line, k_ours_prefix).has_value() ||
                match_marker(line, k_base_prefix).has_value()) {
                // Malformed: reset and re-process this line from idle.
                reset();
                auto label = match_marker(line, k_ours_prefix);
                if (label.has_value()) {
                    state = scan_state::ours;
                    start_line = line_no;
                    ours_label = label->empty() ? kimix::optional<kimix::string>()
                                                : kimix::optional<kimix::string>(*label);
                    ours_buf.clear();
                }
                continue;
            }
            base_buf.emplace_back(line);
            continue;
        }

        // state == theirs
        auto label = match_marker(line, k_theirs_prefix);
        if (label.has_value()) {
            theirs_label = label->empty() ? kimix::optional<kimix::string>()
                                          : kimix::optional<kimix::string>(*label);
            conflict_block b;
            b.start_line = start_line;
            b.separator_line = separator_line;
            b.end_line = line_no;
            b.base_line = base_line;
            b.ours_label = ours_label;
            b.base_label = base_label;
            b.theirs_label = theirs_label;
            b.ours_lines = ours_buf;
            b.base_lines = base_buf;
            b.theirs_lines = theirs_buf;
            out.push_back(std::move(b));
            reset();
            continue;
        }
        if (match_marker(line, k_ours_prefix).has_value()) {
            reset();
            auto nested = match_marker(line, k_ours_prefix);
            state = scan_state::ours;
            start_line = line_no;
            ours_label = nested->empty() ? kimix::optional<kimix::string>()
                                         : kimix::optional<kimix::string>(*nested);
            ours_buf.clear();
            continue;
        }
        theirs_buf.emplace_back(line);
    }
}

void find_dangling_openers_impl(const kimix::vector<kimix::string> &lines,
                                kimix::vector<dangling_opener> &out) noexcept {
    out.clear();
    enum class dstate : uint8_t { idle, ours };
    dstate state = dstate::idle;
    int32_t open_line = 0;
    kimix::string open_text;
    for (size_t i = 0; i < lines.size(); ++i) {
        const kimix::string_view line = strip_trailing_cr(lines[i]);
        const int32_t line_no = static_cast<int32_t>(i) + 1;
        if (state == dstate::idle) {
            if (match_marker(line, k_ours_prefix).has_value()) {
                state = dstate::ours;
                open_line = line_no;
                open_text = kimix::string(line);
            }
            continue;
        }
        if (state == dstate::ours) {
            if (match_marker(line, k_theirs_prefix).has_value()) {
                state = dstate::idle;
            } else if (match_marker(line, k_ours_prefix).has_value()) {
                open_line = line_no;
                open_text = kimix::string(line);
            }
        }
    }
    if (state == dstate::ours) {
        dangling_opener d;
        d.line = open_line;
        d.marker_line = std::move(open_text);
        out.push_back(std::move(d));
    }
}

kimix::vector<kimix::string> block_signature(const conflict_block &e) {
    kimix::vector<kimix::string> sig;
    sig.reserve(1 + e.ours_lines.size() +
                (e.base_line >= 0 ? 1 + e.base_lines.size() : 0) + 1 +
                e.theirs_lines.size() + 1);
    sig.emplace_back(k_ours_prefix);
    sig.insert(sig.end(), e.ours_lines.begin(), e.ours_lines.end());
    if (e.base_line >= 0) {
        sig.emplace_back(k_base_prefix);
        sig.insert(sig.end(), e.base_lines.begin(), e.base_lines.end());
    }
    sig.emplace_back(k_separator);
    sig.insert(sig.end(), e.theirs_lines.begin(), e.theirs_lines.end());
    sig.emplace_back(k_theirs_prefix);
    return sig;
}

kimix::string render_marker(kimix::string_view prefix,
                            const kimix::optional<kimix::string> &label) noexcept {
    if (label.has_value() && !label->empty()) {
        return kimix::string(prefix) + " " + *label;
    }
    return kimix::string(prefix);
}

kimix::vector<kimix::string> region_lines(const conflict_block &e) {
    kimix::vector<kimix::string> out;
    out.reserve(1 + e.ours_lines.size() +
                (e.base_line >= 0 ? 1 + e.base_lines.size() : 0) + 1 +
                e.theirs_lines.size() + 1);
    out.push_back(render_marker(k_ours_prefix, e.ours_label));
    out.insert(out.end(), e.ours_lines.begin(), e.ours_lines.end());
    if (e.base_line >= 0) {
        out.push_back(render_marker(k_base_prefix, e.base_label));
        out.insert(out.end(), e.base_lines.begin(), e.base_lines.end());
    }
    out.push_back(kimix::string(k_separator));
    out.insert(out.end(), e.theirs_lines.begin(), e.theirs_lines.end());
    out.push_back(render_marker(k_theirs_prefix, e.theirs_label));
    return out;
}

bool matches_at(const kimix::vector<kimix::string> &lines, int64_t idx,
                const kimix::vector<kimix::string> &signature) noexcept {
    if (idx < 0 || static_cast<size_t>(idx) + signature.size() > lines.size()) {
        return false;
    }
    for (size_t offset = 0; offset < signature.size(); ++offset) {
        const kimix::string_view candidate =
            strip_trailing_cr(lines[static_cast<size_t>(idx) + offset]);
        const kimix::string_view expected = signature[offset];
        if (candidate == expected) {
            continue;
        }
        if (expected == k_ours_prefix || expected == k_base_prefix ||
            expected == k_separator || expected == k_theirs_prefix) {
            if (!match_marker(candidate, expected).has_value()) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

int32_t delimiter_balance(kimix::string_view line) noexcept {
    int32_t bal = 0;
    for (char c : line) {
        if (c == '(') {
            ++bal;
        } else if (c == ')') {
            --bal;
        } else if (c == '[') {
            ++bal;
        } else if (c == ']') {
            --bal;
        }
    }
    return bal;
}

int32_t echo_prefix_len(const kimix::vector<kimix::string> &replacement,
                        const kimix::vector<kimix::string> &context) noexcept {
    int64_t limit = k_echo_trim_limit;
    limit = std::min(limit, static_cast<int64_t>(replacement.size()) - 1);
    limit = std::min(limit, static_cast<int64_t>(context.size()));
    int32_t best = 0;
    for (int64_t k = 1; k <= limit; ++k) {
        bool eq = true;
        for (int64_t t = 0; t < k; ++t) {
            if (replacement[static_cast<size_t>(t)] !=
                context[static_cast<size_t>(context.size() - static_cast<size_t>(k) +
                                            static_cast<size_t>(t))]) {
                eq = false;
                break;
            }
        }
        if (eq) {
            best = static_cast<int32_t>(k);
        }
    }
    return best;
}

int32_t echo_suffix_len(const kimix::vector<kimix::string> &replacement,
                        const kimix::vector<kimix::string> &context) noexcept {
    int64_t limit = k_echo_trim_limit;
    limit = std::min(limit, static_cast<int64_t>(replacement.size()) - 1);
    limit = std::min(limit, static_cast<int64_t>(context.size()));
    int32_t best = 0;
    for (int64_t k = 1; k <= limit; ++k) {
        bool eq = true;
        for (int64_t t = 0; t < k; ++t) {
            if (replacement[static_cast<size_t>(replacement.size() -
                                                static_cast<size_t>(k) +
                                                static_cast<size_t>(t))] !=
                context[static_cast<size_t>(t)]) {
                eq = false;
                break;
            }
        }
        if (eq) {
            best = static_cast<int32_t>(k);
        }
    }
    return best;
}

void trim_echo(const kimix::vector<kimix::string> &preceding,
               const kimix::vector<kimix::string> &following,
               kimix::vector<kimix::string> &replacement, int32_t &trimmed_leading,
               int32_t &trimmed_trailing) noexcept {
    trimmed_leading = 0;
    trimmed_trailing = 0;
    if (replacement.size() <= 1) {
        return;
    }
    int32_t k = echo_prefix_len(replacement, preceding);
    if (k >= 2 || (k == 1 && delimiter_balance(replacement[0]) == 0)) {
        replacement.erase(replacement.begin(), replacement.begin() + k);
        trimmed_leading = k;
    }
    k = echo_suffix_len(replacement, following);
    if (k >= 2 ||
        (k == 1 && delimiter_balance(replacement[replacement.size() - 1]) == 0)) {
        replacement.resize(replacement.size() - static_cast<size_t>(k));
        trimmed_trailing = k;
    }
}

void collect_labels(const kimix::vector<conflict_entry> &entries,
                    kimix::vector<kimix::string> &ours,
                    kimix::vector<kimix::string> &theirs) noexcept {
    ours.clear();
    theirs.clear();
    for (const auto &e : entries) {
        if (e.ours_label.has_value() && !e.ours_label->empty()) {
            bool seen = false;
            for (const auto &l : ours) {
                if (l == *e.ours_label) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                ours.push_back(*e.ours_label);
            }
        }
        if (e.theirs_label.has_value() && !e.theirs_label->empty()) {
            bool seen = false;
            for (const auto &l : theirs) {
                if (l == *e.theirs_label) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                theirs.push_back(*e.theirs_label);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Minimal deterministic unified-diff engine (port of runtime/diff/diff_engine
// semantics -> difflib.SequenceMatcher(autojunk=False) + unified_diff).
// ---------------------------------------------------------------------------

struct write_diff_match {
    size_t i = 0;
    size_t j = 0;
    size_t n = 0;
};

struct write_diff_opcode {
    kimix::string tag;
    size_t old_start = 0;
    size_t old_end = 0;
    size_t new_start = 0;
    size_t new_end = 0;
};

struct write_string_hash {
    size_t operator()(const kimix::string &s) const noexcept {
        return kimix::hash64(s.data(), s.size());
    }
};

template <typename T, typename Hash>
void diff_find_longest_match(const kimix::vector<T> &a, size_t alo, size_t ahi,
                             const kimix::vector<T> &b, size_t blo, size_t bhi,
                             write_diff_match &out) noexcept {
    kimix::unordered_map<T, kimix::vector<size_t>, Hash> b2j;
    for (size_t j = blo; j < bhi; ++j) {
        b2j[b[j]].push_back(j);
    }
    kimix::unordered_map<size_t, size_t> j2len_prev;
    kimix::unordered_map<size_t, size_t> j2len_curr;
    size_t best_i = alo;
    size_t best_j = blo;
    size_t best_n = 0;
    for (size_t i = alo; i < ahi; ++i) {
        j2len_curr.clear();
        auto it = b2j.find(a[i]);
        if (it == b2j.end()) {
            j2len_prev.swap(j2len_curr);
            continue;
        }
        for (size_t j : it->second) {
            if (j < blo || j >= bhi) {
                continue;
            }
            size_t k = 0;
            auto prev = j2len_prev.find(j - 1);
            if (prev != j2len_prev.end()) {
                k = prev->second;
            }
            k += 1;
            j2len_curr[j] = k;
            if (k > best_n) {
                best_i = i + 1 - k;
                best_j = j + 1 - k;
                best_n = k;
            }
        }
        j2len_prev.swap(j2len_curr);
    }
    out = {best_i, best_j, best_n};
}

template <typename T, typename Hash>
void diff_find_matching_blocks(const kimix::vector<T> &a, size_t alo, size_t ahi,
                               const kimix::vector<T> &b, size_t blo, size_t bhi,
                               kimix::vector<write_diff_match> &out) noexcept {
    write_diff_match m;
    diff_find_longest_match<T, Hash>(a, alo, ahi, b, blo, bhi, m);
    if (m.n == 0) {
        return;
    }
    if (alo < m.i && blo < m.j) {
        diff_find_matching_blocks<T, Hash>(a, alo, m.i, b, blo, m.j, out);
    }
    out.push_back(m);
    if (m.i + m.n < ahi && m.j + m.n < bhi) {
        diff_find_matching_blocks<T, Hash>(a, m.i + m.n, ahi, b, m.j + m.n, bhi, out);
    }
}

void diff_compute_opcodes(const kimix::vector<kimix::string> &a,
                          const kimix::vector<kimix::string> &b,
                          kimix::vector<write_diff_opcode> &out) noexcept {
    out.clear();
    kimix::vector<write_diff_match> matches;
    diff_find_matching_blocks<kimix::string, write_string_hash>(a, 0, a.size(), b, 0,
                                                                b.size(), matches);
    matches.push_back({a.size(), b.size(), 0}); // sentinel

    size_t i1 = 0;
    size_t j1 = 0;
    for (const auto &m : matches) {
        const size_t i2 = m.i;
        const size_t j2 = m.j;
        if (i1 < i2 || j1 < j2) {
            const char *tag = (i1 < i2 && j1 < j2) ? "replace" : (i1 < i2) ? "delete"
                                                                           : "insert";
            out.push_back({kimix::string(tag), i1, i2, j1, j2});
        }
        if (m.n > 0) {
            out.push_back({"equal", i2, i2 + m.n, j2, j2 + m.n});
        }
        i1 = i2 + m.n;
        j1 = j2 + m.n;
    }
}

void diff_group_opcodes(const kimix::vector<write_diff_opcode> &opcodes, size_t n,
                        kimix::vector<kimix::vector<write_diff_opcode>> &out) noexcept {
    out.clear();
    kimix::vector<write_diff_opcode> codes = opcodes;

    if (codes.empty()) {
        codes.push_back({"equal", 0, 1, 0, 1});
    }

    if (!codes.empty() && codes.front().tag == "equal") {
        write_diff_opcode &first = codes.front();
        const size_t old_len = first.old_end - first.old_start;
        const size_t new_len = first.new_end - first.new_start;
        first.old_start = (old_len > n) ? (first.old_end - n) : first.old_start;
        first.new_start = (new_len > n) ? (first.new_end - n) : first.new_start;
    }

    if (!codes.empty() && codes.back().tag == "equal") {
        write_diff_opcode &last = codes.back();
        last.old_end = std::min(last.old_end, last.old_start + n);
        last.new_end = std::min(last.new_end, last.new_start + n);
    }

    const size_t nn = n + n;
    kimix::vector<write_diff_opcode> group;

    for (write_diff_opcode &op : codes) {
        if (op.tag == "equal" && (op.old_end - op.old_start) > nn) {
            write_diff_opcode tail = op;
            tail.old_end = op.old_start + n;
            tail.new_end = op.new_start + n;
            group.push_back(tail);
            out.push_back(std::move(group));
            group.clear();
            op.old_start = op.old_end - n;
            op.new_start = op.new_end - n;
        }
        group.push_back(op);
    }

    if (!group.empty() && !(group.size() == 1 && group.front().tag == "equal")) {
        out.push_back(std::move(group));
    }
}

kimix::string diff_format_range(size_t start, size_t len) noexcept {
    if (len == 1) {
        return kimix::format("{}", start);
    }
    if (len == 0) {
        return kimix::format("{},0", start - 1);
    }
    return kimix::format("{},{}", start, len);
}

void split_lines_keepends(kimix::string_view text, kimix::vector<kimix::string> &out) noexcept {
    out.clear();
    const size_t n = text.size();
    size_t start = 0;
    size_t i = 0;
    while (i < n) {
        size_t boundary_len = 0;
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\n' || c == '\r' || c == '\v' || c == '\f' || (c >= 0x1cu && c <= 0x1eu)) {
            boundary_len = 1;
            if (c == '\r' && i + 1 < n && static_cast<unsigned char>(text[i + 1]) == '\n') {
                boundary_len = 2;
            }
        } else if (c == 0xc2u && i + 1 < n &&
                   static_cast<unsigned char>(text[i + 1]) == 0x85u) {
            boundary_len = 2;
        } else if (c == 0xe2u && i + 2 < n &&
                   static_cast<unsigned char>(text[i + 1]) == 0x80u &&
                   (static_cast<unsigned char>(text[i + 2]) == 0xa8u ||
                    static_cast<unsigned char>(text[i + 2]) == 0xa9u)) {
            boundary_len = 3;
        }
        if (boundary_len > 0) {
            out.emplace_back(text.substr(start, i + boundary_len - start));
            start = i + boundary_len;
            i = start;
        } else {
            ++i;
        }
    }
    if (start < n) {
        out.emplace_back(text.substr(start));
    }
}

bool ends_with_newline(const kimix::string &s) noexcept {
    return !s.empty() && s.back() == '\n';
}

// ---------------------------------------------------------------------------
// 1. UTF-8 strict validation wrapper
// ---------------------------------------------------------------------------

kimix::optional<kimix::string> utf8_decode_error(kimix::string_view bytes) noexcept {
    size_t bad = 0;
    kimix::string reason;
    if (utf8_strict_error(bytes, bad, reason)) {
        return kimix::optional<kimix::string>();
    }
    return kimix::string("utf-8 decoding error: ") + reason;
}

kimix::optional<uint64_t> expected_write_size(bool append, kimix::string_view old_text,
                                              kimix::string_view content,
                                              kimix::string_view new_text) noexcept {
    if (append) {
        if (!utf8_validate(old_text) || !utf8_validate(content)) {
            return kimix::optional<uint64_t>();
        }
        return static_cast<uint64_t>(old_text.size()) +
               static_cast<uint64_t>(content.size());
    }
    if (!utf8_validate(new_text)) {
        return kimix::optional<uint64_t>();
    }
    return static_cast<uint64_t>(new_text.size());
}

// ---------------------------------------------------------------------------
// 2. Auto-generated-file guard
// ---------------------------------------------------------------------------

bool is_auto_generated_file_name(kimix::string_view file_path) noexcept {
    const kimix::string_view name = basename_of(file_path);
    if (name.starts_with("zz_generated.")) {
        return true;
    }
    if (name.ends_with(".pb.go") || name.ends_with(".pb.cc") || name.ends_with(".pb.h") ||
        name.ends_with(".pb.c") || name.ends_with(".pb.js") || name.ends_with(".pb.ts")) {
        return true;
    }
    if (name.ends_with("_pb2.py") || name.ends_with("_pb2_grpc.py")) {
        return true;
    }
    if (name.ends_with(".gen.go") || name.ends_with(".gen.ts") || name.ends_with(".gen.js") ||
        name.ends_with(".gen.py")) {
        return true;
    }
    if (name == "generated.go" || name == "generated.ts" || name == "generated.js" ||
        name == "generated.py") {
        return true;
    }
    if (name.ends_with(".swagger.json") || name.ends_with(".openapi.json")) {
        return true;
    }
    // \.mock\.(?:go|ts)$  and  \.mocks?\.(?:go|ts|js)$
    if (name.ends_with(".mock.go") || name.ends_with(".mock.ts")) {
        return true;
    }
    if (name.ends_with(".mock.js") || name.ends_with(".mocks.go") ||
        name.ends_with(".mocks.ts") || name.ends_with(".mocks.js")) {
        return true;
    }
    return false;
}

void get_comment_styles_for_path(kimix::string_view file_path,
                                 kimix::vector<comment_style> &out) noexcept {
    out.clear();
    const kimix::string name = lower_ascii(basename_of(file_path));
    if (name == "dockerfile" || name == "makefile" || name == "justfile") {
        out.push_back(comment_style::hash);
        return;
    }
    const kimix::string ext = lower_ascii(suffix_of(name));
    if (ext == ".py" || ext == ".rb" || ext == ".sh" || ext == ".bash" || ext == ".zsh" ||
        ext == ".yml" || ext == ".yaml" || ext == ".toml" || ext == ".ini" ||
        ext == ".cfg" || ext == ".conf" || ext == ".env" || ext == ".pl" || ext == ".r") {
        out.push_back(comment_style::hash);
        return;
    }
    if (ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cs" || ext == ".dart" ||
        ext == ".go" || ext == ".h" || ext == ".hpp" || ext == ".java" || ext == ".js" ||
        ext == ".jsx" || ext == ".kt" || ext == ".kts" || ext == ".mjs" ||
        ext == ".cjs" || ext == ".php" || ext == ".rs" || ext == ".scala" ||
        ext == ".swift" || ext == ".ts" || ext == ".tsx") {
        out.push_back(comment_style::slash);
        return;
    }
    if (ext == ".sql") {
        out.push_back(comment_style::sql);
        return;
    }
    if (ext == ".html" || ext == ".htm" || ext == ".xml" || ext == ".svg" ||
        ext == ".xhtml") {
        out.push_back(comment_style::html);
        return;
    }
    out = {comment_style::slash, comment_style::hash, comment_style::sql,
           comment_style::html};
}

void extract_leading_header_comment_text(kimix::string_view content,
                                         const kimix::vector<comment_style> &styles,
                                         kimix::string &out) noexcept {
    out.clear();
    // Strip a leading UTF-8 BOM (EF BB BF).
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEFu &&
        static_cast<unsigned char>(content[1]) == 0xBBu &&
        static_cast<unsigned char>(content[2]) == 0xBFu) {
        content.remove_prefix(3);
    }
    kimix::vector<kimix::string> lines;
    split_py_lines(content, lines);
    kimix::vector<kimix::string> collected;
    bool started = false;
    int in_block = 0; // 0 none, 1 slash, 2 html
    const size_t limit = std::min(lines.size(), k_header_line_limit);
    for (size_t i = 0; i < limit; ++i) {
        const kimix::string stripped = trim_ascii(lines[i]);
        // Shebang is only allowed on the very first line.
        if (i == 0 && stripped.starts_with("#!")) {
            continue;
        }
        if (in_block != 0) {
            collected.push_back(stripped);
            if (in_block == 1 && stripped.find("*/") != kimix::string::npos) {
                in_block = 0;
            } else if (in_block == 2 && stripped.find("-->") != kimix::string::npos) {
                in_block = 0;
            }
            started = true;
            continue;
        }
        if (stripped.empty()) {
            if (started) {
                collected.push_back("");
            }
            continue;
        }
        bool is_comment = false;
        if (has_style(styles, comment_style::slash)) {
            if (stripped.starts_with("//")) {
                is_comment = true;
            } else if (stripped.starts_with("/*")) {
                is_comment = true;
                if (stripped.find("*/") == kimix::string::npos) {
                    in_block = 1;
                }
            }
        }
        if (!is_comment && has_style(styles, comment_style::hash) &&
            stripped.starts_with("#")) {
            is_comment = true;
        }
        if (!is_comment && has_style(styles, comment_style::sql) &&
            stripped.starts_with("--")) {
            is_comment = true;
        }
        if (!is_comment && has_style(styles, comment_style::html) &&
            stripped.starts_with("<!--")) {
            is_comment = true;
            if (stripped.find("-->") == kimix::string::npos) {
                in_block = 2;
            }
        }
        if (is_comment) {
            collected.push_back(stripped);
            started = true;
        } else {
            break;
        }
    }
    out = join(collected, "\n");
}

kimix::optional<kimix::string> detect_auto_generated_marker(
    kimix::string_view content, kimix::string_view file_path) noexcept {
    if (is_auto_generated_file_name(file_path)) {
        return kimix::string(basename_of(file_path));
    }
    kimix::string_view prefix = content.substr(0, std::min(content.size(), k_check_byte_count));
    // Never split a multi-byte sequence at the 1 KiB cut (Python slices chars).
    prefix = prefix.substr(0, utf8_floor_boundary(prefix, prefix.size()));

    kimix::vector<comment_style> styles;
    get_comment_styles_for_path(file_path, styles);
    kimix::string header;
    extract_leading_header_comment_text(prefix, styles, header);
    if (header.empty()) {
        return kimix::optional<kimix::string>();
    }
    kimix::string matched;
    if (scan_header_markers(header, matched)) {
        return matched;
    }
    return kimix::optional<kimix::string>();
}

kimix::string build_auto_generated_error(kimix::string_view display_path,
                                         kimix::string_view detected) noexcept {
    kimix::string m =
        kimix::format("Cannot modify auto-generated file: {}\n\n", display_path);
    m += kimix::format(
        "This file appears to be automatically generated (detected marker: \"{}\"). "
        "Changes will be overwritten the next time the code is regenerated. "
        "Edit the source (schema, template, or generator input) and regenerate instead, "
        "or pass allow_auto_generated=true to override.",
        detected);
    return m;
}

// ---------------------------------------------------------------------------
// 3. Conflict-marker scan + splice
// ---------------------------------------------------------------------------

kimix::optional<kimix::string> match_marker(kimix::string_view line,
                                            kimix::string_view prefix) noexcept {
    line = strip_trailing_cr(line);
    if (!line.starts_with(prefix)) {
        return kimix::optional<kimix::string>();
    }
    const kimix::string_view rest = line.substr(prefix.size());
    if (rest.empty()) {
        return kimix::string();
    }
    if (rest[0] == ' ') {
        const kimix::string_view label = rest.substr(1);
        // A label is required after the space and must not itself start with a
        // space ("<<<<<<<  two" never matches).
        if (!label.empty() && label[0] != ' ') {
            return kimix::string(label);
        }
    }
    return kimix::optional<kimix::string>();
}

bool is_separator(kimix::string_view line) noexcept {
    return strip_trailing_cr(line) == k_separator;
}

void scan_conflict_blocks(kimix::string_view content,
                          kimix::vector<conflict_block> &out) noexcept {
    out.clear();
    kimix::vector<kimix::string> lines;
    split_lf(content, lines);
    scan_conflict_lines_impl(lines, 1, out);
}

void find_dangling_openers(kimix::string_view content,
                           kimix::vector<dangling_opener> &out) noexcept {
    out.clear();
    kimix::string norm;
    replace_all(content, "\r\n", "\n", norm);
    kimix::vector<kimix::string> lines;
    split_py_lines(norm, lines);
    find_dangling_openers_impl(lines, out);
}

bool splice_conflict(kimix::string_view original_text, const conflict_entry &entry,
                     kimix::string_view replacement, conflict_splice_result &out,
                     kimix::string &error) noexcept {
    const bool uses_crlf = original_text.find("\r\n") != kimix::string_view::npos;
    kimix::string text_lf;
    replace_all(original_text, "\r\n", "\n", text_lf);
    kimix::vector<kimix::string> lines;
    split_lf(text_lf, lines);
    const kimix::vector<kimix::string> signature = block_signature(entry);

    const int64_t anchor = static_cast<int64_t>(entry.start_line) - 1;
    int64_t located = -1;
    if (matches_at(lines, anchor, signature)) {
        located = anchor;
    } else {
        int64_t best_distance = -1;
        for (size_t idx = 0; idx < lines.size(); ++idx) {
            if (matches_at(lines, static_cast<int64_t>(idx), signature)) {
                const int64_t distance =
                    std::llabs(static_cast<int64_t>(idx) - anchor);
                if (best_distance < 0 || distance < best_distance) {
                    best_distance = distance;
                    located = static_cast<int64_t>(idx);
                }
            }
        }
    }
    if (located < 0) {
        error = kimix::format(
            "Conflict #{} no longer matches the recorded block at {}:{}. "
            "Re-read the file to get a current conflict id.",
            entry.id, entry.display_path, entry.start_line);
        return false;
    }

    const size_t region_len = signature.size();

    // replacement.split("\n") with trailing \r stripped; only "" yields no lines.
    kimix::vector<kimix::string> replacement_lines;
    if (!replacement.empty()) {
        split_lf(replacement, replacement_lines);
        for (auto &ln : replacement_lines) {
            ln = kimix::string(strip_trailing_cr(ln));
        }
    }

    kimix::vector<kimix::string> preceding;
    const int64_t pre_lo = std::max<int64_t>(0, located - k_echo_trim_limit);
    for (int64_t i = pre_lo; i < located; ++i) {
        preceding.push_back(lines[static_cast<size_t>(i)]);
    }
    kimix::vector<kimix::string> following;
    const int64_t fol_hi = std::min<int64_t>(
        static_cast<int64_t>(lines.size()),
        located + static_cast<int64_t>(region_len) + k_echo_trim_limit);
    for (int64_t i = located + static_cast<int64_t>(region_len); i < fol_hi; ++i) {
        following.push_back(lines[static_cast<size_t>(i)]);
    }

    int32_t trimmed_leading = 0;
    int32_t trimmed_trailing = 0;
    trim_echo(preceding, following, replacement_lines, trimmed_leading,
              trimmed_trailing);

    kimix::vector<kimix::string> new_lines;
    new_lines.reserve(lines.size() - region_len + replacement_lines.size());
    new_lines.insert(new_lines.end(), lines.begin(),
                     lines.begin() + static_cast<ptrdiff_t>(located));
    new_lines.insert(new_lines.end(), replacement_lines.begin(),
                     replacement_lines.end());
    new_lines.insert(new_lines.end(),
                     lines.begin() +
                         static_cast<ptrdiff_t>(located + static_cast<int64_t>(region_len)),
                     lines.end());

    kimix::string text = join(new_lines, "\n");
    if (uses_crlf) {
        kimix::string crlf_text;
        replace_all(text, "\n", "\r\n", crlf_text);
        text = std::move(crlf_text);
    }

    out.text = std::move(text);
    out.trimmed_leading = trimmed_leading;
    out.trimmed_trailing = trimmed_trailing;
    return true;
}

kimix::optional<kimix::string> expand_content_tokens(
    kimix::string_view content, const conflict_entry &entry,
    kimix::string &out) noexcept {
    kimix::vector<kimix::string> lines;
    split_lf(content, lines);
    kimix::vector<kimix::string> result;
    result.reserve(lines.size());
    for (const auto &raw : lines) {
        const kimix::string stripped = trim_ascii(raw);
        if (stripped == "@ours") {
            result.insert(result.end(), entry.ours_lines.begin(),
                          entry.ours_lines.end());
        } else if (stripped == "@theirs") {
            result.insert(result.end(), entry.theirs_lines.begin(),
                          entry.theirs_lines.end());
        } else if (stripped == "@base") {
            if (entry.base_line < 0) {
                kimix::string m = "@base is not available for conflict #";
                m += kimix::format("{}", entry.id);
                m += " \xE2\x80\x94 it is a 2-way conflict (no ||||||| base section).";
                return m;
            }
            result.insert(result.end(), entry.base_lines.begin(),
                          entry.base_lines.end());
        } else if (stripped == "@both") {
            result.insert(result.end(), entry.ours_lines.begin(),
                          entry.ours_lines.end());
            result.insert(result.end(), entry.theirs_lines.begin(),
                          entry.theirs_lines.end());
        } else {
            result.push_back(raw);
        }
    }
    out = join(result, "\n");
    return kimix::optional<kimix::string>();
}

bool conflict_regions_equal(const conflict_block &a, const conflict_block &b) noexcept {
    if (a.start_line != b.start_line || a.end_line != b.end_line) {
        return false;
    }
    return join(region_lines(a), "\n") == join(region_lines(b), "\n");
}

bool conflict_region_present(kimix::string_view content,
                             const conflict_block &entry) noexcept {
    kimix::string text_lf;
    replace_all(content, "\r\n", "\n", text_lf);
    const kimix::string region = join(region_lines(entry), "\n");
    return text_lf.find(region) != kimix::string::npos;
}

kimix::optional<kimix::string> render_conflict_region(
    const conflict_entry &entry, kimix::string_view scope,
    kimix::vector<kimix::string> &out_lines, int32_t &start_line) noexcept {
    if (scope.empty()) {
        out_lines = region_lines(entry);
        start_line = entry.start_line;
        return kimix::optional<kimix::string>();
    }
    if (scope == "ours") {
        out_lines = entry.ours_lines;
        start_line = entry.start_line + 1;
        return kimix::optional<kimix::string>();
    }
    if (scope == "theirs") {
        out_lines = entry.theirs_lines;
        start_line = entry.separator_line + 1;
        return kimix::optional<kimix::string>();
    }
    if (scope == "base") {
        if (entry.base_line < 0) {
            kimix::string m = kimix::format("Conflict #{} is a 2-way conflict ", entry.id);
            m += "\xE2\x80\x94 no base section. Use /ours or /theirs.";
            return m;
        }
        out_lines = entry.base_lines;
        start_line = entry.base_line + 1;
        return kimix::optional<kimix::string>();
    }
    return kimix::format("Unknown conflict scope '{}'.", scope);
}

kimix::string format_conflict_summary(const kimix::vector<conflict_entry> &entries,
                                      kimix::string_view display_path,
                                      bool scan_truncated) noexcept {
    if (entries.empty()) {
        return kimix::format("No unresolved git merge conflicts in {}.", display_path);
    }
    const size_t count = entries.size();
    kimix::string lines = "\xE2\x9A\xA0 ";
    lines += kimix::format("{} unresolved {} in {}", count,
                           count == 1 ? "conflict" : "conflicts", display_path);
    kimix::vector<kimix::string> ours_labels;
    kimix::vector<kimix::string> theirs_labels;
    collect_labels(entries, ours_labels, theirs_labels);
    if (!ours_labels.empty()) {
        lines += "\n- ours = " + join(ours_labels, ", ");
    }
    if (!theirs_labels.empty()) {
        lines += "\n- theirs = " + join(theirs_labels, ", ");
    }
    if (scan_truncated) {
        lines += "\n- note: file scan hit the byte cap; additional conflicts may exist "
                 "beyond the scanned prefix.";
    }
    lines += "\nNOTICE: Bulk-resolve with `write({ path: \"conflict://*\", content })`, "
             "or address a single block with `write({ path: \"conflict://<N>\", content })`. "
             "A line exactly `@ours` / `@theirs` / `@base` / `@both` expands to that "
             "recorded section; non-token lines pass through verbatim.";
    for (const auto &e : entries) {
        const kimix::string suffix = e.base_line >= 0 ? "  (3-way)" : "";
        lines += kimix::format("\n#{}  L{}-{}{}", e.id, e.start_line, e.end_line, suffix);
    }
    return lines;
}

kimix::optional<kimix::string> parse_conflict_uri(kimix::string_view raw,
                                                  parsed_conflict_uri &out) noexcept {
    constexpr kimix::string_view k_scheme = "conflict://";
    int64_t scheme_pos = -1;
    for (size_t i = 0; i + k_scheme.size() <= raw.size(); ++i) {
        if (raw.substr(i, k_scheme.size()) == k_scheme && (i == 0 || raw[i - 1] == ':')) {
            scheme_pos = static_cast<int64_t>(i);
        }
    }
    if (scheme_pos < 0) {
        return kimix::optional<kimix::string>();
    }
    const kimix::string_view body =
        raw.substr(static_cast<size_t>(scheme_pos) + k_scheme.size());
    if (body.empty()) {
        // regex requires a non-empty (.+) body
        return kimix::optional<kimix::string>();
    }
    kimix::optional<kimix::string> prefix;
    if (scheme_pos > 0) {
        if (scheme_pos == 1) {
            return kimix::optional<kimix::string>(); // "(?:(.+):)?" needs >=1 prefix char
        }
        prefix = kimix::string(raw.substr(0, static_cast<size_t>(scheme_pos) - 1));
    }

    kimix::string_view id_part = body;
    kimix::string scope;
    const size_t slash = body.find('/');
    if (slash != kimix::string_view::npos) {
        id_part = body.substr(0, slash);
        const kimix::string_view scope_part = body.substr(slash + 1);
        if (scope_part == "ours" || scope_part == "theirs" || scope_part == "base") {
            scope = kimix::string(scope_part);
        } else {
            return kimix::format("Invalid conflict scope '{}'. Valid scopes: ours, "
                                 "theirs, base.",
                                 scope_part);
        }
    }
    if (id_part == "*") {
        if (!scope.empty()) {
            kimix::string m = "conflict://* does not accept a scope ";
            m += "\xE2\x80\x94 it resolves every registered conflict.";
            return m;
        }
        out = parsed_conflict_uri();
        out.is_star = true;
        out.recovered_prefix = prefix;
        return kimix::optional<kimix::string>();
    }

    bool ok = false;
    const int64_t id = parse_int_loose(id_part, ok);
    if (!ok) {
        return kimix::format("Invalid conflict id '{}' in '{}'. Expected conflict://<N> "
                             "or conflict://<N>/<ours|theirs|base>.",
                             id_part, raw);
    }
    if (id <= 0) {
        kimix::string m = kimix::format("Invalid conflict id '{}' ", id_part);
        m += "\xE2\x80\x94 ids start at 1.";
        return m;
    }
    out = parsed_conflict_uri();
    out.id = id;
    out.scope = std::move(scope);
    out.recovered_prefix = prefix;
    return kimix::optional<kimix::string>();
}

bool parse_bulk_directives(
    kimix::string_view content,
    kimix::vector<std::pair<int32_t, kimix::string>> &out) noexcept {
    out.clear();
    kimix::vector<kimix::string> lines;
    split_lf(content, lines);
    bool saw_any = false;
    for (const auto &raw_line : lines) {
        const kimix::string line = trim_ascii(raw_line);
        if (line.empty()) {
            continue;
        }
        saw_any = true;
        // ^\s*(\d+)\s*:\s*@(ours|theirs|base|both)\s*$
        size_t i = 0;
        while (i < raw_line.size() && is_space_ascii(raw_line[i])) {
            ++i;
        }
        uint64_t id = 0;
        bool any_digit = false;
        while (i < raw_line.size() && raw_line[i] >= '0' && raw_line[i] <= '9') {
            if (id > (static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) - 9u) / 10u) {
                return false; // out of int32 range -> not a directive
            }
            id = id * 10u + static_cast<uint64_t>(raw_line[i] - '0');
            any_digit = true;
            ++i;
        }
        if (!any_digit) {
            return false;
        }
        while (i < raw_line.size() && is_space_ascii(raw_line[i])) {
            ++i;
        }
        if (i >= raw_line.size() || raw_line[i] != ':') {
            return false;
        }
        ++i;
        while (i < raw_line.size() && is_space_ascii(raw_line[i])) {
            ++i;
        }
        if (i >= raw_line.size() || raw_line[i] != '@') {
            return false;
        }
        ++i;
        const size_t word_start = i;
        while (i < raw_line.size() && raw_line[i] >= 'a' && raw_line[i] <= 'z') {
            ++i;
        }
        const kimix::string_view side(raw_line.data() + word_start, i - word_start);
        while (i < raw_line.size() && is_space_ascii(raw_line[i])) {
            ++i;
        }
        if (i != raw_line.size()) {
            return false;
        }
        if (side != "ours" && side != "theirs" && side != "base" && side != "both") {
            return false;
        }
        out.emplace_back(static_cast<int32_t>(id), kimix::string(side));
    }
    return saw_any;
}

conflict_guard_result run_conflict_guard(kimix::string_view display_path,
                                         kimix::string_view old_text,
                                         kimix::string_view new_content, bool append,
                                         bool file_existed,
                                         bool allow_conflicts) noexcept {
    conflict_guard_result result;

    // write.py: old_lines = old_text.replace("\r\n","\n").splitlines() if old_text else []
    kimix::vector<kimix::string> old_lines;
    if (!old_text.empty()) {
        kimix::string norm;
        replace_all(old_text, "\r\n", "\n", norm);
        split_py_lines(norm, old_lines);
    }
    kimix::vector<conflict_block> old_blocks;
    scan_conflict_lines_impl(old_lines, 1, old_blocks);
    result.old_had_blocks = !old_blocks.empty();

    kimix::string norm_new;
    replace_all(new_content, "\r\n", "\n", norm_new);
    kimix::vector<kimix::string> new_lines;
    split_py_lines(norm_new, new_lines);
    kimix::vector<conflict_block> new_blocks;
    scan_conflict_lines_impl(new_lines, 1, new_blocks);

    if (append && file_existed) {
        kimix::vector<dangling_opener> dangling;
        find_dangling_openers_impl(old_lines, dangling);
        if (!dangling.empty() && !allow_conflicts) {
            result.error = build_dangling_opener_error(display_path, dangling);
            return result;
        }
        if (!new_blocks.empty() && !allow_conflicts) {
            result.error = build_conflict_markers_error(display_path, new_blocks);
            return result;
        }
        if (!new_blocks.empty()) {
            result.note = kimix::format(
                " Warning: appended content still contains {} unresolved conflict "
                "marker block(s) (allow_conflicts=True).",
                new_blocks.size());
        } else if (result.old_had_blocks) {
            result.note = kimix::format(
                " Note: `{}` still contains {} unresolved conflict marker block(s); "
                "the appended text was clean. Resolve them via `read <path>:conflicts` + "
                "`write({{ path: \"conflict://<N>\", content }})`.",
                display_path, old_blocks.size());
        }
        return result;
    }

    // Overwrite mode (also covers brand-new files).
    if (!new_blocks.empty() && !allow_conflicts) {
        result.error = build_conflict_markers_error(display_path, new_blocks);
        return result;
    }
    if (!new_blocks.empty()) {
        result.note = kimix::format(
            " Warning: written content still contains {} unresolved conflict "
            "marker block(s) (allow_conflicts=True).",
            new_blocks.size());
    }
    return result;
}

kimix::string build_conflict_markers_error(
    kimix::string_view display_path,
    const kimix::vector<conflict_block> &blocks) noexcept {
    kimix::vector<kimix::string> found;
    found.reserve(blocks.size() * 2);
    for (const auto &b : blocks) {
        found.push_back(kimix::format("  line {}: <<<<<<< marker block start", b.start_line));
        found.push_back(kimix::format("  line {}: >>>>>>> marker block end", b.end_line));
    }
    kimix::string m =
        kimix::format("Conflict markers detected in `{}`; refusing to write.\n",
                      display_path);
    m += join(found, "\n");
    m += kimix::format(
        "\nThe content still contains {} unresolved conflict marker block(s). "
        "Resolve them first (e.g. read `{}:conflicts`, then "
        "`write({{ path: \"conflict://<N>\", content }})`), or set "
        "allow_conflicts=True to write anyway.",
        blocks.size(), display_path);
    return m;
}

kimix::string build_dangling_opener_error(
    kimix::string_view display_path,
    const kimix::vector<dangling_opener> &dangling) noexcept {
    kimix::vector<kimix::string> found;
    found.reserve(dangling.size());
    for (const auto &d : dangling) {
        found.push_back(kimix::format("  line {}: {}", d.line, d.marker_line));
    }
    kimix::string m = kimix::format(
        "Conflict markers detected in `{}`; refusing to append: the file ends "
        "inside an unclosed conflict block.\n",
        display_path);
    m += join(found, "\n");
    m += "\nResolve the conflict first, or set allow_conflicts=True to append anyway.";
    return m;
}

// ---------------------------------------------------------------------------
// 4. JSON format validation (vendored yyjson)
// ---------------------------------------------------------------------------

kimix::optional<kimix::string> check_json_format(kimix::string_view text) noexcept {
    if (text.empty()) {
        // orjson uses this exact wording for a zero-length document; yyjson
        // reports "input length is 0" - special-cased for message parity.
        return kimix::string(
            "JSON decode error at line 1, column 1: Input is a zero-length, empty "
            "document");
    }
    yyjson_read_err err{};
    yyjson_doc *doc = yyjson_read_opts(const_cast<char *>(text.data()), text.size(),
                                       0 /* no flags: strict, stop-on-error */,
                                       nullptr, &err);
    if (doc != nullptr) {
        yyjson_doc_free(doc);
        return kimix::optional<kimix::string>();
    }
    size_t pos = err.pos;
    if (pos > text.size()) {
        pos = text.size();
    }
    // 1-based line, 1-based column counting code points (orjson colno).
    size_t line = 1;
    size_t line_start = 0;
    for (size_t i = 0; i < pos; ++i) {
        if (text[i] == '\n') {
            ++line;
            line_start = i + 1;
        }
    }
    const size_t col = 1 + utf8_code_point_count(text.substr(line_start, pos - line_start));
    const char *msg = (err.msg != nullptr) ? err.msg : "invalid JSON";
    return kimix::format("JSON decode error at line {}, column {}: {}", line, col,
                         kimix::string_view(msg));
}

tool_status validate_format_by_path(kimix::string_view file_path,
                                    kimix::string_view text,
                                    kimix::string &fmt_error) noexcept {
    fmt_error.clear();
    const kimix::string lower = lower_ascii(file_path);
    if (lower.ends_with(".json")) {
        auto err = check_json_format(text);
        if (err.has_value()) {
            fmt_error = std::move(*err);
        }
        return tool_status::ok;
    }
    if (lower.ends_with(".yaml") || lower.ends_with(".yml") || lower.ends_with(".toml") ||
        lower.ends_with(".xml")) {
        // No vendored YAML/TOML/XML parsers: validation stays Python-side
    // (plan section 3.4 / section 3.6).  The caller must fall back to the mirror.
        return tool_status::unsupported;
    }
    return tool_status::ok;
}

// ---------------------------------------------------------------------------
// 5. mkdir / diff / post-write verification kernels
// ---------------------------------------------------------------------------

parent_dir_decision decide_parent_dir(bool parent_exists, bool mkdir,
                                      kimix::string_view display_path,
                                      kimix::string_view parent_path,
                                      kimix::optional<kimix::string> create_error) noexcept {
    parent_dir_decision d;
    if (parent_exists) {
        return d;
    }
    if (!mkdir) {
        d.status = tool_status::not_found;
        d.message = kimix::format("Parent directory does not exist: {}. Set mkdir=True "
                                  "to create it.",
                                  parent_path);
        return d;
    }
    if (create_error.has_value()) {
        d.status = tool_status::invalid_input;
        d.message = kimix::format("Failed to create parent directory for {}: {}",
                                  display_path, *create_error);
        return d;
    }
    return d; // mkdir requested and creation succeeded -> proceed
}

kimix::string build_unified_diff(kimix::string_view old_text, kimix::string_view new_text,
                                 kimix::string_view path,
                                 bool include_file_header) noexcept {
    kimix::vector<kimix::string> old_lines;
    kimix::vector<kimix::string> new_lines;
    split_lines_keepends(old_text, old_lines);
    split_lines_keepends(new_text, new_lines);

    if (!old_lines.empty() && !ends_with_newline(old_lines.back())) {
        old_lines.back().push_back('\n');
    }
    if (!new_lines.empty() && !ends_with_newline(new_lines.back())) {
        new_lines.back().push_back('\n');
    }

    kimix::vector<write_diff_opcode> opcodes;
    diff_compute_opcodes(old_lines, new_lines, opcodes);

    kimix::vector<kimix::vector<write_diff_opcode>> groups;
    diff_group_opcodes(opcodes, 3, groups);

    if (groups.empty()) {
        return kimix::string();
    }

    kimix::string result;
    const kimix::string fromfile = path.empty() ? "a/file" : ("a/" + kimix::string(path));
    const kimix::string tofile = path.empty() ? "b/file" : ("b/" + kimix::string(path));
    if (include_file_header) {
        result += "--- " + fromfile + "\n";
        result += "+++ " + tofile + "\n";
    }

    for (const auto &group : groups) {
        const write_diff_opcode &first = group.front();
        const write_diff_opcode &last = group.back();
        const size_t old_start = first.old_start + 1;
        const size_t new_start = first.new_start + 1;
        const size_t old_len = last.old_end - first.old_start;
        const size_t new_len = last.new_end - first.new_start;

        result += "@@ -";
        result += diff_format_range(old_start, old_len);
        result += " +";
        result += diff_format_range(new_start, new_len);
        result += " @@\n";

        for (const write_diff_opcode &op : group) {
            if (op.tag == "equal") {
                for (size_t i = op.old_start; i < op.old_end; ++i) {
                    result += ' ';
                    result += old_lines[i];
                }
            } else if (op.tag == "delete") {
                for (size_t i = op.old_start; i < op.old_end; ++i) {
                    result += '-';
                    result += old_lines[i];
                }
            } else if (op.tag == "insert") {
                for (size_t j = op.new_start; j < op.new_end; ++j) {
                    result += '+';
                    result += new_lines[j];
                }
            } else if (op.tag == "replace") {
                for (size_t i = op.old_start; i < op.old_end; ++i) {
                    result += '-';
                    result += old_lines[i];
                }
                for (size_t j = op.new_start; j < op.new_end; ++j) {
                    result += '+';
                    result += new_lines[j];
                }
            }
        }
    }
    return result;
}

kimix::string verification_failed_error(kimix::string_view display_path,
                                        kimix::string_view reason,
                                        bool outside) noexcept {
    return kimix::format("{}Write verification failed for {}: {}.",
                         outside ? "[out of work-dir] " : "", display_path, reason);
}

kimix::string size_mismatch_error(kimix::string_view display_path, uint64_t expected,
                                  uint64_t actual, bool outside) noexcept {
    return kimix::format("{}Write verification failed (size mismatch): expected {} "
                         "bytes, got {} bytes. Path: {}",
                         outside ? "[out of work-dir] " : "", expected, actual,
                         display_path);
}

kimix::string success_message(kimix::string_view display_path, uint64_t size,
                              kimix::string_view action_desc,
                              kimix::string_view conflict_note,
                              kimix::string_view drift_note) noexcept {
    kimix::string m = kimix::format("File successfully {}. Current size: {} bytes. "
                                    "Path: {}",
                                    action_desc, size, display_path);
    m += " Verified: size matches.";
    if (!conflict_note.empty()) {
        m += kimix::string(conflict_note);
    }
    if (!drift_note.empty()) {
        m += " " + kimix::string(drift_note);
    }
    return m;
}

kimix::string conflict_resolved_message(int32_t id, int32_t start_line, int32_t end_line,
                                        kimix::string_view display_path,
                                        int32_t trimmed_total) noexcept {
    kimix::string m = kimix::format("Resolved conflict #{} at line(s) L{}-{} in {}.",
                                    id, start_line, end_line, display_path);
    if (trimmed_total > 0) {
        m += kimix::format(
            " Note: dropped {} content line(s) that duplicated the code adjacent to "
            "the conflict region (boundary-echo repair).",
            trimmed_total);
    }
    return m;
}

} // namespace write
} // namespace builtin_tools
} // namespace kimix
