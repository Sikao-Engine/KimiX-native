// grep_tool.cpp - Pure string kernels of the kimi-agent grep tool.
//
// Port target: D:/KimiX-native/plans/grep.md §3 kernels 1-6 (Phase A / Phase B-CPU scope).
// The function-by-function mapping to the Python reference lives in
// src/builtin_tools/reports/grep.md; header grep_tool.h names the source files
// and line ranges. Two non-obvious parity arguments are recorded here:
//
// 1. Lazy quantifiers. `^(.*?)([:\-])(\d+)\2` picks the LEFTMOST position whose
//    character is ':' or '-' that is followed by a decimal run closed by the
//    same delimiter. `(.*?)` is lazy and the digit run is greedy, but a shorter
//    digit run cannot rescue an earlier failure either: it would have to be
//    followed by a digit, and digits are never delimiters. One left-to-right
//    scan is therefore exact - no backtracking engine is required.
//
// 2. Unicode classes. Python's \d is category Nd and `\s`/str.strip() use a
//    29-code-point set. Kernels whose answer depends on \d inside a region with
//    non-ASCII bytes report tool_status::unsupported (the shim then calls the
//    pure-Python mirror); blank/whitespace tests use the exact code-point set
//    (is_space_cp) and never need gating. Content-line kernels apply the gate
//    per candidate delimiter, so non-ASCII text bodies - the common case - stay
//    native. Pure byte transforms are never gated.
//
// Compiled into the kimix-llm static library with a unity (jumbo) batch, so
// everything lives inside kimix::builtin_tools::grep and internal helpers have
// internal linkage with grep-specific names.

#include "builtin_tools/grep_tool.h"

#include "builtin_tools/utf8_util.h"

#include <algorithm>

namespace kimix::builtin_tools::grep {

namespace {

// -- character predicates ---------------------------------------------------

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool is_path_sep(char c) noexcept { return c == '/' || c == '\\'; }

// Python str.isspace() on the ASCII subset, EXCLUDING 0x1C-0x1F (which Python
// treats as non-whitespace even though re.\s matches them).
bool is_ws_ascii(char c) noexcept {
    if (c >= 0x09 && c <= 0x0d) {
        return true;
    }
    return c == ' ';
}

bool is_all_ascii(kimix::string_view s) noexcept {
    for (const char c : s) {
        if (static_cast<uint8_t>(c) >= 0x80u) {
            return false;
        }
    }
    return true;
}

// Python str.isspace() == regex \s code-point set MINUS 0x1C-0x1F (str.strip()
// does not remove them; the reference's selectors rely on strip() semantics).
bool is_space_cp(uint32_t cp) noexcept {
    if (cp >= 0x0009u && cp <= 0x000du) {
        return true;
    }
    switch (cp) {
    case 0x0020u:
    case 0x0085u:
    case 0x00a0u:
    case 0x1680u:
    case 0x2028u:
    case 0x2029u:
    case 0x202fu:
    case 0x205fu:
    case 0x3000u:
        return true;
    default:
        return cp >= 0x2000u && cp <= 0x200au; // U+2000..U+200A
    }
}

// Python `not text.strip()`. Invalid UTF-8 decodes to U+FFFD (not whitespace),
// so undecodable bytes keep the text non-blank.
bool is_blank_text(kimix::string_view s) noexcept {
    const char *it = s.data();
    const char *end = s.data() + s.size();
    while (it < end) {
        if (!is_space_cp(decode_code_point(it, end))) {
            return false;
        }
    }
    return true;
}

// `text.strip()` on the ASCII subset (the selector grammar is ASCII-gated).
kimix::string_view strip_ascii_ws(kimix::string_view s) noexcept {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && is_ws_ascii(s[b])) {
        b++;
    }
    while (e > b && is_ws_ascii(s[e - 1])) {
        e--;
    }
    return s.substr(b, e - b);
}

// `text.strip()` for arbitrary UTF-8 (returns a sub-view).
kimix::string_view strip_text(kimix::string_view s) noexcept {
    const char *begin = s.data();
    const char *end = s.data() + s.size();
    const char *head = begin;
    {
        const char *it = begin;
        bool found = false;
        while (it < end) {
            const char *before = it;
            if (!is_space_cp(decode_code_point(it, end))) {
                head = before;
                found = true;
                break;
            }
        }
        if (!found) {
            head = it; // all whitespace (or empty) -> empty result
        }
    }
    const char *tail = end;
    while (tail > head) {
        const char *probe = tail;
        do {
            --probe;
        } while (probe > head && (static_cast<uint8_t>(*probe) & 0xC0u) == 0x80u);
        const char *walk = probe;
        const uint32_t cp = decode_code_point(walk, tail);
        if (walk != tail || !is_space_cp(cp)) {
            break;
        }
        tail = probe;
    }
    return s.substr(static_cast<size_t>(head - begin), static_cast<size_t>(tail - head));
}

char lower_ascii(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// ASCII-case-insensitive equality (Python `re.IGNORECASE` over an ASCII pattern).
bool equal_ci(kimix::string_view a, kimix::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        if (lower_ascii(a[i]) != lower_ascii(b[i])) {
            return false;
        }
    }
    return true;
}

bool ends_with_ic(kimix::string_view s, kimix::string_view suffix) noexcept {
    if (s.size() < suffix.size()) {
        return false;
    }
    const size_t off = s.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); i++) {
        if (lower_ascii(s[off + i]) != lower_ascii(suffix[i])) {
            return false;
        }
    }
    return true;
}

bool starts_with(kimix::string_view s, kimix::string_view prefix) noexcept {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool contains(kimix::string_view hay, kimix::string_view needle) noexcept {
    return hay.find(needle) != kimix::string_view::npos;
}

// -- small string builders --------------------------------------------------

void str_assign(kimix::string &out, kimix::string_view v) { out.assign(v.data(), v.size()); }

void str_append(kimix::string &out, kimix::string_view v) { out.append(v.data(), v.size()); }

void append_num(kimix::string &out, uint64_t v) {
    const kimix::string text = kimix::format("{}", v);
    str_append(out, text);
}

void append_utf8_cp(kimix::string &out, uint32_t cp) {
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

// Python `s.replace("\\", "/")`.
kimix::string to_forward_slashes(kimix::string_view s) {
    kimix::string out;
    out.reserve(s.size());
    for (const char c : s) {
        out.push_back(c == '\\' ? '/' : c);
    }
    return out;
}

// Python `s.rstrip("/")`.
kimix::string_view rstrip_slash(kimix::string_view s) noexcept {
    size_t e = s.size();
    while (e > 0 && s[e - 1] == '/') {
        e--;
    }
    return s.substr(0, e);
}

// Python `int(digits)` for a decimal run, saturating at the uint32_t domain.
// Returns false on overflow (never a silently wrong number).
bool digits_to_u32(kimix::string_view digits, uint32_t &out) noexcept {
    size_t i = 0;
    while (i < digits.size() && digits[i] == '0') {
        i++;
    }
    if (digits.size() - i > 10u) {
        return false;
    }
    uint64_t value = 0;
    for (; i < digits.size(); i++) {
        value = value * 10u + static_cast<uint64_t>(digits[i] - '0');
        if (value > 0xFFFFFFFFull) {
            return false;
        }
    }
    out = static_cast<uint32_t>(value);
    return true;
}

// -- scanner primitives -----------------------------------------------------

// Consume `L?\d+` at `pos`; on success `digits` is the number run.
bool scan_l_digits(kimix::string_view s, size_t &pos, kimix::string_view &digits) noexcept {
    size_t p = pos;
    if (p < s.size() && (s[p] == 'L' || s[p] == 'l')) {
        p++;
    }
    const size_t begin = p;
    while (p < s.size() && is_digit(s[p])) {
        p++;
    }
    if (p == begin) {
        return false;
    }
    digits = s.substr(begin, p - begin);
    pos = p;
    return true;
}

// `^L?(\d+)(?:(\.\.|[-+])(?:L?(\d+))?)?$` with re.IGNORECASE, hand-rolled.
// `op` is 0 when absent, '-' (also used for the '..' alias) or '+'.
bool range_chunk_match(kimix::string_view s, kimix::string_view &digits1, int &op,
                       kimix::string_view &digits2) noexcept {
    size_t pos = 0;
    if (!scan_l_digits(s, pos, digits1)) {
        return false;
    }
    digits2 = {};
    if (pos == s.size()) {
        op = 0;
        return true;
    }
    if (s[pos] == '.' && pos + 1 < s.size() && s[pos + 1] == '.') {
        op = '-'; // Python handles '-' and '..' in the same branch
        pos += 2;
    } else if (s[pos] == '-' || s[pos] == '+') {
        op = s[pos];
        pos += 1;
    } else {
        return false;
    }
    if (pos == s.size()) {
        return true; // "N-", "N+", "N.." with no second number
    }
    if (!scan_l_digits(s, pos, digits2)) {
        return false;
    }
    return pos == s.size();
}

// `^(?:raw|conflicts|L?\d+(?:(?:\.\.|[-+])L?\d*)?)$` with re.IGNORECASE.
// The trailing `\d*` makes "5-", "5+" and "5.." shape-valid with the second
// number omitted (this is the loose guard; semantics come from parse_line_ranges).
bool selector_shape_chunk_match(kimix::string_view c) noexcept {
    if (equal_ci(c, "raw") || equal_ci(c, "conflicts")) {
        return true;
    }
    size_t pos = 0;
    if (pos < c.size() && (c[pos] == 'L' || c[pos] == 'l')) {
        pos++;
    }
    const size_t begin = pos;
    while (pos < c.size() && is_digit(c[pos])) {
        pos++;
    }
    if (pos == begin) {
        return false;
    }
    if (pos == c.size()) {
        return true;
    }
    if (c[pos] == '.' && pos + 1 < c.size() && c[pos + 1] == '.') {
        pos += 2;
    } else if (c[pos] == '-' || c[pos] == '+') {
        pos += 1;
    } else {
        return false;
    }
    // The second number may carry its own optional 'L' prefix (`L?\d*`).
    if (pos < c.size() && (c[pos] == 'L' || c[pos] == 'l')) {
        pos++;
    }
    while (pos < c.size() && is_digit(c[pos])) {
        pos++;
    }
    return pos == c.size();
}

// grep_selectors._is_selector_shape (206-215).
bool is_selector_shape(kimix::string_view tail) noexcept {
    const kimix::string_view stripped = strip_ascii_ws(tail);
    if (stripped.empty()) {
        return false;
    }
    size_t pos = 0;
    while (true) {
        const size_t comma = stripped.find(',', pos);
        const size_t stop = comma == kimix::string_view::npos ? stripped.size() : comma;
        const kimix::string_view chunk = strip_ascii_ws(stripped.substr(pos, stop - pos));
        if (chunk.empty() || !selector_shape_chunk_match(chunk)) {
            return false;
        }
        if (comma == kimix::string_view::npos) {
            return true;
        }
        pos = comma + 1;
    }
}

// `^[a-zA-Z][a-zA-Z0-9+.\-]*://[^/]*$` - scheme://authority without a path, so a
// trailing ":port" chunk is a port and never a selector.
bool scheme_authority_match(kimix::string_view s) noexcept {
    if (s.empty() || !is_alpha(s[0])) {
        return false;
    }
    size_t pos = 1;
    while (pos < s.size()) {
        const char c = s[pos];
        if (!is_alpha(c) && !is_digit(c) && c != '+' && c != '.' && c != '-') {
            break;
        }
        pos++;
    }
    if (!starts_with(s.substr(pos), "://")) {
        return false;
    }
    return s.find('/', pos + 3) == kimix::string_view::npos;
}

// ntpath.splitdrive, restricted to the two shapes split_path_and_sel's guard
// cares about: drive letters ("X:") and UNC roots ("\\server\share\...").
void splitdrive_nt(kimix::string_view p, kimix::string_view &drive, kimix::string_view &after) noexcept {
    drive = {};
    after = p;
    if (p.size() >= 2u && is_alpha(p[0]) && p[1] == ':') {
        drive = p.substr(0, 2);
        after = p.substr(2);
        return;
    }
    if (p.size() >= 2u && is_path_sep(p[0]) && is_path_sep(p[1])) {
        size_t pos = 2;
        const size_t server = pos;
        while (pos < p.size() && !is_path_sep(p[pos])) {
            pos++;
        }
        if (pos == server) {
            drive = p; // separators only
            after = {};
            return;
        }
        while (pos < p.size() && is_path_sep(p[pos])) {
            pos++;
        }
        const size_t share = pos;
        while (pos < p.size() && !is_path_sep(p[pos])) {
            pos++;
        }
        if (pos == p.size() || pos == share) {
            drive = p; // "\\server" or "\\server\share" - nothing left over
            after = {};
            return;
        }
        drive = p.substr(0, pos);
        after = p.substr(pos);
    }
}

// Result of the leftmost `([:\-])(\d+)\1` scan.
struct delim_scan {
    bool matched = false;
    bool unsupported = false; // Python's \d could pick a different candidate
    size_t path_len = 0;      // bytes before the opening delimiter
    int sep = 0;              // ':' or '-'
    kimix::string_view digits;
    size_t close_pos = 0; // index of the closing delimiter
};

// `^(.*?)([:\-])(\d+)\2` - shared by parse_content_line (re.DOTALL, so the scan
// crosses newlines freely) and _RG_LINE_RE (`no_newline_region`: `.` excludes
// '\n', so the delimiter must precede the first newline).
delim_scan scan_delimiters(kimix::string_view line, bool no_newline_region) noexcept {
    delim_scan res;
    bool suspicious = false;
    size_t p = 0;
    while (p < line.size()) {
        const char c = line[p];
        if (no_newline_region && c == '\n') {
            break; // `.` cannot span it, so no later delimiter can match
        }
        if (c != ':' && c != '-') {
            p++;
            continue;
        }
        size_t q = p + 1;
        while (q < line.size() && is_digit(line[q])) {
            q++;
        }
        if (q < line.size() && static_cast<uint8_t>(line[q]) >= 0x80u) {
            // Our ASCII digit run stopped at a non-ASCII byte: Python's \d (Nd)
            // may keep going and close the pair there, so this candidate - and
            // everything left of it - is unknowable without Unicode tables.
            suspicious = true;
        }
        if (q != p + 1 && q < line.size() && line[q] == c) {
            res.matched = true;
            res.path_len = p;
            res.sep = c;
            res.digits = line.substr(p + 1, q - (p + 1));
            res.close_pos = q;
            // This candidate is exact (its closing delimiter is ASCII, so
            // Python's greedy run stops at the same byte); only an earlier
            // suspicious candidate could have made Python match further left.
            res.unsupported = suspicious;
            return res;
        }
        p++;
    }
    res.unsupported = suspicious;
    return res;
}

// -- fnmatch (ASCII subset, both normcase flavours) -------------------------

// fnmatch.translate(pattern) + fullmatch semantics: '*', '?' and '[...]' are
// the metacharacters, everything else is literal. `fold_case` is the normcase
// effect (ntpath.normcase lower-cases, posixpath.normcase is the identity).
bool fnmatch_ascii(kimix::string_view name, kimix::string_view pattern, bool fold_case) noexcept {
    const auto eq = [fold_case](char a, char b) noexcept {
        return fold_case ? lower_ascii(a) == lower_ascii(b) : a == b;
    };
    size_t n = 0; // name cursor
    size_t p = 0; // pattern cursor
    size_t star_p = kimix::string_view::npos; // last '*' position
    size_t star_n = 0;                        // name index to retry from
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star_p = p;
            star_n = n + 1u;
            p++;
            continue;
        }
        bool consumed = false;
        if (p < pattern.size()) {
            const char pc = pattern[p];
            if (pc == '?') {
                n++;
                p++;
                consumed = true;
            } else if (pc == '[') {
                size_t i = p + 1;
                bool negate = false;
                if (i < pattern.size() && (pattern[i] == '!' || pattern[i] == '^')) {
                    negate = true;
                    i++;
                }
                const size_t items_begin = i;
                bool hit = false;
                bool closed = false;
                size_t close = 0;
                while (i < pattern.size()) {
                    if (pattern[i] == ']' && i != items_begin) {
                        closed = true;
                        close = i;
                        break;
                    }
                    if (pattern[i] == '-' && i != items_begin && i + 1 < pattern.size() &&
                        pattern[i + 1] != ']') {
                        const char lo = pattern[i - 1];
                        const char hi = pattern[i + 1];
                        const char cur = fold_case ? lower_ascii(name[n]) : name[n];
                        const char low_lo = fold_case ? lower_ascii(lo) : lo;
                        const char low_hi = fold_case ? lower_ascii(hi) : hi;
                        if (cur >= low_lo && cur <= low_hi) {
                            hit = true;
                        }
                        i += 2;
                        continue;
                    }
                    if (eq(name[n], pattern[i])) {
                        hit = true;
                    }
                    i++;
                }
                if (!closed) {
                    return false; // unterminated class never matches
                }
                if (hit != negate) {
                    n++;
                    p = close + 1u;
                    consumed = true;
                }
            } else if (eq(name[n], pc)) {
                n++;
                p++;
                consumed = true;
            }
        }
        if (consumed) {
            continue;
        }
        if (star_p == kimix::string_view::npos) {
            return false;
        }
        n = star_n;
        star_n++; // consume one more name byte under the '*'
        p = star_p + 1u;
    }
    while (p < pattern.size() && pattern[p] == '*') {
        p++;
    }
    return p == pattern.size();
}

// -- sensitive.py tables ----------------------------------------------------

// SENSITIVE_PATTERNS, in the reference's order (first match wins).
constexpr kimix::string_view sensitive_patterns[] = {
    ".env",
    ".env.*",
    "id_rsa",
    "id_ed25519",
    "id_ecdsa",
    ".aws/credentials",
    ".gcp/credentials",
    "credentials",
};

// SENSITIVE_EXEMPTIONS (case-sensitive membership test in the reference).
constexpr kimix::string_view sensitive_exemptions[] = {
    ".env.example",
    ".env.sample",
    ".env.template",
};

tool_status unsupported() noexcept { return tool_status::unsupported; }

} // namespace

// ===========================================================================
// 1. Selector grammar (grep_selectors.py)
// ===========================================================================

selector_result parse_line_range_chunk(kimix::string_view chunk) {
    selector_result res;
    if (!is_all_ascii(chunk)) {
        res.status = unsupported();
        return res;
    }
    const kimix::string_view s = strip_ascii_ws(chunk);
    kimix::string_view digits1;
    kimix::string_view digits2;
    int op = 0;
    if (s.empty() || !range_chunk_match(s, digits1, op, digits2)) {
        res.no_value = true; // `if m is None: return None`
        return res;
    }
    uint32_t start = 0;
    if (!digits_to_u32(digits1, start)) {
        res.status = unsupported();
        return res;
    }
    if (start < 1u) {
        res.status = tool_status::invalid_input;
        res.message =
            kimix::format("Line selector {} is invalid; lines are 1-indexed. Use :1.", start);
        return res;
    }
    const auto emit = [&](uint32_t end_line, bool open_ended) {
        res.range = open_ended ? line_range{start, kimix::optional<uint32_t>{}}
                               : line_range{start, end_line};
        res.ranges.push_back(res.range);
    };

    if (op == '-' || op == 0) {
        if (digits2.empty()) {
            emit(0u, true); // "N", "N-", "N.." -> open-ended
            return res;
        }
        uint32_t end = 0;
        if (!digits_to_u32(digits2, end)) {
            res.status = unsupported();
            return res;
        }
        if (end < start) {
            res.status = tool_status::invalid_input;
            res.message = kimix::format("Invalid range {}-{}: end must be >= start.", start, end);
            return res;
        }
        emit(end, false);
        return res;
    }

    // op == '+'
    if (digits2.empty()) {
        emit(0u, true); // "N+" behaves like the open-ended "N-"
        return res;
    }
    uint32_t count = 0;
    if (!digits_to_u32(digits2, count)) {
        res.status = unsupported();
        return res;
    }
    if (count < 1u) {
        res.status = tool_status::invalid_input;
        res.message = kimix::format("Invalid range {}+{}: count must be >= 1.", start, count);
        return res;
    }
    if (static_cast<uint64_t>(start) + count - 1ull > 0xFFFFFFFFull) {
        res.status = unsupported();
        return res;
    }
    emit(start + count - 1u, false);
    return res;
}

selector_result parse_line_ranges(kimix::string_view sel) {
    selector_result res;
    if (!is_all_ascii(sel)) {
        res.status = unsupported();
        return res;
    }
    kimix::vector<line_range> ranges;
    bool saw_any = false;
    size_t pos = 0;
    while (true) {
        const size_t comma = sel.find(',', pos);
        const size_t stop = comma == kimix::string_view::npos ? sel.size() : comma;
        const kimix::string_view chunk = strip_ascii_ws(sel.substr(pos, stop - pos));
        if (!chunk.empty()) {
            selector_result one = parse_line_range_chunk(chunk);
            if (one.failed()) {
                return one;
            }
            if (!one.no_value) {
                saw_any = true;
                ranges.push_back(one.range);
            }
        }
        if (comma == kimix::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
    if (!saw_any) {
        res.no_value = true;
        return res;
    }
    std::stable_sort(ranges.begin(), ranges.end(),
                     [](const line_range &a, const line_range &b) noexcept {
                         return a.start_line < b.start_line;
                     });
    kimix::vector<line_range> merged;
    merged.reserve(ranges.size());
    for (const line_range &r : ranges) {
        if (!merged.empty()) {
            line_range &last = merged.back();
            if (!last.end_line.has_value()) {
                continue; // an open-ended range absorbs everything after it
            }
            if (r.start_line <= *last.end_line + 1u) {
                if (r.end_line.has_value()) {
                    last.end_line = std::max(*last.end_line, *r.end_line);
                } else {
                    last.end_line.reset();
                }
                continue;
            }
        }
        merged.push_back(r);
    }
    res.ranges = std::move(merged);
    return res;
}

bool is_line_in_ranges(uint32_t line_number, kimix::span<const line_range> ranges) noexcept {
    if (ranges.empty()) {
        return true; // Python's None (unfiltered); the binding maps None -> empty
    }
    for (const line_range &r : ranges) {
        if (line_number < r.start_line) {
            continue;
        }
        if (!r.end_line.has_value() || line_number <= *r.end_line) {
            return true;
        }
    }
    return false;
}

selector_result selector_line_ranges(kimix::string_view sel) {
    selector_result res;
    if (sel.empty()) {
        res.no_value = true; // `if not sel: return None`
        return res;
    }
    if (!is_all_ascii(sel)) {
        res.status = unsupported();
        return res;
    }
    size_t pos = 0;
    while (true) {
        const size_t colon = sel.find(':', pos);
        const size_t stop = colon == kimix::string_view::npos ? sel.size() : colon;
        const kimix::string_view chunk = strip_ascii_ws(sel.substr(pos, stop - pos));
        if (!chunk.empty() && !equal_ci(chunk, "raw") && !equal_ci(chunk, "conflicts")) {
            selector_result parsed = parse_line_ranges(chunk);
            if (parsed.failed() || !parsed.no_value) {
                return parsed;
            }
        }
        if (colon == kimix::string_view::npos) {
            break;
        }
        pos = colon + 1;
    }
    res.no_value = true;
    return res;
}

tool_status split_path_and_sel(kimix::string_view raw_path,
                               const kimix::function<bool(kimix::string_view)> &probe_exists,
                               path_selector &out) {
    out = path_selector{};
    if (!is_all_ascii(raw_path)) {
        return unsupported();
    }
    if (raw_path.empty() || raw_path.find(':') == kimix::string_view::npos) {
        str_assign(out.path, raw_path);
        return tool_status::ok;
    }
    // A real filesystem entry with the raw name outranks the selector reading.
    if (probe_exists && probe_exists(raw_path)) {
        str_assign(out.path, raw_path);
        return tool_status::ok;
    }
    if (scheme_authority_match(raw_path)) {
        str_assign(out.path, raw_path);
        return tool_status::ok;
    }
    kimix::string_view rest = raw_path;
    kimix::vector<kimix::string_view> peeled;
    for (int attempt = 0; attempt < 2; attempt++) {
        const size_t found = rest.rfind(':');
        if (found == kimix::string_view::npos || found == 0u) {
            break; // `if idx <= 0: break`
        }
        const kimix::string_view head = rest.substr(0, found);
        const kimix::string_view tail = rest.substr(found + 1);
        if (!is_selector_shape(tail)) {
            break;
        }
        kimix::string_view drive;
        kimix::string_view after;
        splitdrive_nt(head, drive, after);
        if (!drive.empty() && after.empty()) {
            break; // a bare drive letter ("C:") must never be left behind
        }
        peeled.insert(peeled.begin(), tail);
        rest = head;
    }
    if (peeled.empty()) {
        str_assign(out.path, raw_path);
        return tool_status::ok;
    }
    str_assign(out.path, rest);
    for (size_t i = 0; i < peeled.size(); i++) {
        if (i > 0) {
            out.selector.push_back(':');
        }
        str_append(out.selector, peeled[i]);
    }
    out.has_selector = true;
    return tool_status::ok;
}

// -- JSON array scan (orjson parity for the shapes _maybe_json_array accepts)

namespace {

bool json_ws(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

uint32_t json_hex4(kimix::string_view s, size_t pos) noexcept {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        const char c = s[pos + i];
        uint32_t d = 0;
        if (c >= '0' && c <= '9') {
            d = static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = static_cast<uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = static_cast<uint32_t>(c - 'A' + 10);
        } else {
            return 0xFFFFFFFFu;
        }
        v = v * 16u + d;
    }
    return v;
}

// Parse one JSON string literal (strict: control bytes must be escaped, lone
// surrogates are rejected - orjson raises there and the caller falls back).
bool json_string(kimix::string_view s, size_t &pos, kimix::string &value) noexcept {
    if (pos >= s.size() || s[pos] != '"') {
        return false;
    }
    pos++;
    value.clear();
    while (pos < s.size()) {
        const char c = s[pos];
        if (c == '"') {
            pos++;
            return true;
        }
        if (static_cast<uint8_t>(c) < 0x20u) {
            return false;
        }
        if (c != '\\') {
            value.push_back(c);
            pos++;
            continue;
        }
        if (pos + 1 >= s.size()) {
            return false;
        }
        const char e = s[pos + 1];
        pos += 2;
        switch (e) {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        case 'u': {
            if (pos + 4 > s.size()) {
                return false;
            }
            uint32_t cp = json_hex4(s, pos);
            if (cp == 0xFFFFFFFFu) {
                return false;
            }
            pos += 4;
            if (cp >= 0xD800u && cp <= 0xDBFFu) {
                if (pos + 6 > s.size() || s[pos] != '\\' || s[pos + 1] != 'u') {
                    return false; // lone high surrogate
                }
                const uint32_t low = json_hex4(s, pos + 2);
                if (low < 0xDC00u || low > 0xDFFFu) {
                    return false;
                }
                pos += 6;
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
            } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                return false; // lone low surrogate
            }
            append_utf8_cp(value, cp);
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

// grep_selectors._maybe_json_array (266-278): a top-level array whose items are
// all strings. Anything else - malformed JSON, trailing comma, trailing
// content, a non-array, a non-string item - makes Python return None and the
// caller falls back to the ';' split, so a bool is enough here.
bool json_string_array(kimix::string_view s, kimix::vector<kimix::string> &items) {
    if (s.empty() || s[0] != '[') {
        return false;
    }
    size_t pos = 1;
    while (pos < s.size() && json_ws(s[pos])) {
        pos++;
    }
    if (pos < s.size() && s[pos] == ']') {
        pos++;
        while (pos < s.size() && json_ws(s[pos])) {
            pos++;
        }
        return pos == s.size();
    }
    while (true) {
        while (pos < s.size() && json_ws(s[pos])) {
            pos++; // JSON allows whitespace before every item
        }
        kimix::string value;
        if (pos >= s.size() || s[pos] != '"') {
            return false; // non-string item (or premature end)
        }
        if (!json_string(s, pos, value)) {
            return false;
        }
        items.push_back(std::move(value));
        while (pos < s.size() && json_ws(s[pos])) {
            pos++;
        }
        if (pos >= s.size()) {
            return false;
        }
        if (s[pos] == ',') {
            pos++;
            continue;
        }
        if (s[pos] == ']') {
            pos++;
            while (pos < s.size() && json_ws(s[pos])) {
                pos++;
            }
            return pos == s.size();
        }
        return false;
    }
}

// expand_path_entries' final pass: first occurrence wins.
void dedupe(kimix::vector<kimix::string> &entries) {
    kimix::vector<kimix::string> out;
    out.reserve(entries.size());
    for (kimix::string &e : entries) {
        bool dup = false;
        for (const kimix::string &o : out) {
            if (o == e) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            out.push_back(std::move(e));
        }
    }
    entries = std::move(out);
}

void add_stripped(kimix::vector<kimix::string> &entries, kimix::string_view item) {
    const kimix::string_view t = strip_text(item);
    if (!t.empty()) {
        entries.emplace_back(t.data(), t.size());
    }
}

} // namespace

tool_status expand_path_entries(kimix::string_view raw, kimix::vector<kimix::string> &out) {
    out.clear();
    const kimix::string_view s = strip_text(raw);
    if (s.empty()) {
        return tool_status::ok;
    }
    if (s[0] == '[' && is_all_ascii(s)) {
        kimix::vector<kimix::string> parsed;
        if (json_string_array(s, parsed)) {
            for (const kimix::string &item : parsed) {
                add_stripped(out, item);
            }
            dedupe(out);
            return tool_status::ok;
        }
    }
    kimix::vector<kimix::string> entries;
    size_t pos = 0;
    while (true) {
        const size_t at = s.find(';', pos);
        const size_t stop = at == kimix::string_view::npos ? s.size() : at;
        add_stripped(entries, s.substr(pos, stop - pos));
        if (at == kimix::string_view::npos) {
            break;
        }
        pos = at + 1;
    }
    dedupe(entries);
    out = std::move(entries);
    return tool_status::ok;
}

tool_status expand_path_entries(kimix::span<const kimix::string> raw, kimix::vector<kimix::string> &out) {
    out.clear();
    for (const kimix::string &item : raw) {
        if (!is_all_ascii(item)) {
            out.clear();
            return unsupported(); // Python keeps unicode entries; we do not guess
        }
        add_stripped(out, item);
    }
    dedupe(out);
    return tool_status::ok;
}

void merge_ranges_into(ranges_map &map, kimix::string_view abs_key, kimix::span<const line_range> ranges) {
    if (ranges.empty()) {
        return; // `if not ranges: return`
    }
    for (path_ranges &bucket : map) {
        if (bucket.path == abs_key) {
            bucket.ranges.insert(bucket.ranges.end(), ranges.begin(), ranges.end());
            return;
        }
    }
    path_ranges created;
    str_assign(created.path, abs_key);
    created.ranges.assign(ranges.begin(), ranges.end());
    map.push_back(std::move(created));
}

const kimix::vector<line_range> *ranges_map_find(const ranges_map &map, kimix::string_view key) noexcept {
    for (const path_ranges &bucket : map) {
        if (bucket.path == key) {
            return &bucket.ranges;
        }
    }
    return nullptr;
}

bool entries_are_rich(kimix::span<const kimix::string> entries,
                      const kimix::function<bool(kimix::string_view)> &probe_exists) {
    if (entries.size() != 1u) {
        return true;
    }
    path_selector ps;
    if (split_path_and_sel(entries[0], probe_exists, ps) == tool_status::unsupported) {
        return true; // conservative: the Python rich pipeline handles it
    }
    if (ps.has_selector) {
        return true;
    }
    kimix::vector<archive_candidate> candidates;
    parse_archive_path_candidates(ps.path, candidates);
    return !candidates.empty();
}

// ===========================================================================
// 2. Archive paths (grep_archive.py + read_archive.py)
// ===========================================================================

kimix::span<const kimix::string_view> archive_extensions() noexcept {
    // ARCHIVE_EXTENSIONS in the reference's order: longest first so a
    // ".tar.gz" name matches ".tar.gz" before ".gz".
    static constexpr kimix::string_view table[] = {
        ".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tgz",  ".tbz2", ".tbz",
        ".txz",    ".zip",     ".jar",    ".war",     ".ear",  ".apk",  ".whl",
        ".xpi",    ".vsix",    ".nupkg",  ".cbz",     ".tar",  ".gz",   ".bz2",
        ".xz",     ".zst",
    };
    return {table, sizeof(table) / sizeof(table[0])};
}

bool is_archive_path(kimix::string_view path) noexcept {
    for (const kimix::string_view ext : archive_extensions()) {
        if (ends_with_ic(path, ext)) {
            return true;
        }
    }
    return false;
}

void parse_archive_path_candidates(kimix::string_view entry, kimix::vector<archive_candidate> &out) {
    out.clear();
    kimix::string_view rest = entry;
    while (true) {
        const size_t found = rest.rfind(':');
        if (found == kimix::string_view::npos || found == 0u) {
            break; // `if idx <= 0: break`
        }
        const kimix::string_view left = rest.substr(0, found);
        const kimix::string_view member = rest.substr(found + 1);
        if (!member.empty() && is_archive_path(left)) {
            archive_candidate candidate;
            str_assign(candidate.archive, left);
            str_assign(candidate.member, member);
            out.push_back(std::move(candidate));
            rest = left; // nested archives: keep splitting, rightmost-first
            continue;
        }
        break; // not an archive on the left; no deeper candidate can qualify
    }
}

namespace {

// Python `\w` on the ASCII subset (str.isalnum() for ASCII == [A-Za-z0-9]).
bool is_word_ascii(char c) noexcept { return is_alpha(c) || is_digit(c) || c == '_'; }

bool is_scratch_kept(char c) noexcept { return is_word_ascii(c) || c == '.' || c == '-'; }

} // namespace

tool_status safe_scratch_name(kimix::string_view member, kimix::string &out) {
    out.clear();
    if (!is_all_ascii(member)) {
        return unsupported(); // \w is Unicode-aware in the reference
    }
    // os.path.basename(member.replace("\\", "/")): everything after the last
    // separator (either flavour counts as one after the rewrite), else the
    // whole - possibly empty - string.
    const size_t cut = member.find_last_of("/\\");
    kimix::string_view base =
        cut == kimix::string_view::npos ? member : member.substr(cut + 1);
    if (base.empty()) {
        base = "member"; // os.path.basename("") == "" -> `or "member"`
    }
    // re.sub(r"[^\w.-]+", "_", base): each maximal run collapses to one '_'.
    size_t i = 0;
    while (i < base.size()) {
        if (is_scratch_kept(base[i])) {
            out.push_back(base[i]);
            i++;
            continue;
        }
        while (i < base.size() && !is_scratch_kept(base[i])) {
            i++;
        }
        out.push_back('_');
    }
    if (out.empty()) {
        out = "member";
    }
    return tool_status::ok;
}

void remap_display(kimix::span<const kimix::string> lines, const display_map &map,
                   kimix::vector<kimix::string> &out) {
    out.clear();
    out.reserve(lines.size());
    if (map.empty()) {
        out.assign(lines.begin(), lines.end()); // `if not display_map: return lines`
        return;
    }
    kimix::vector<display_entry> pairs;
    pairs.reserve(map.size());
    for (const display_entry &entry : map) {
        display_entry fwd;
        fwd.scratch = to_forward_slashes(entry.scratch);
        fwd.display = entry.display;
        pairs.push_back(std::move(fwd));
    }
    for (const kimix::string &line : lines) {
        const kimix::string norm = to_forward_slashes(line);
        bool replaced = false;
        for (const display_entry &pair : pairs) {
            if (starts_with(norm, pair.scratch)) {
                kimix::string rewritten = pair.display;
                str_append(rewritten, kimix::string_view(norm).substr(pair.scratch.size()));
                out.push_back(std::move(rewritten));
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            out.push_back(line); // the ORIGINAL line, not the normalized one
        }
    }
}

void strip_key_for(kimix::string_view path_arg, kimix::string_view prefix_base, kimix::string &out) {
    const kimix::string norm = to_forward_slashes(path_arg);
    const kimix::string pb = to_forward_slashes(rstrip_slash(prefix_base));
    out.clear();
    if (!pb.empty() && norm.size() > pb.size() && starts_with(norm, pb) && norm[pb.size()] == '/') {
        str_append(out, kimix::string_view(norm).substr(pb.size() + 1));
        return;
    }
    str_append(out, norm);
}

// ===========================================================================
// 3. Output rendering (grep_output.py + grep_local.py)
// ===========================================================================

tool_status parse_content_line(kimix::string_view line, content_line &out, bool &no_match) {
    out = content_line{};
    no_match = true;
    if (line == "--") {
        return tool_status::ok; // the reference's explicit separator check
    }
    const delim_scan scan = scan_delimiters(line, false);
    if (scan.unsupported) {
        return unsupported();
    }
    if (!scan.matched) {
        return tool_status::ok;
    }
    uint32_t line_no = 0;
    if (!digits_to_u32(scan.digits, line_no)) {
        return unsupported();
    }
    const kimix::string_view path = line.substr(0, scan.path_len);
    if (path.empty()) {
        return tool_status::ok; // `if not path: return None`
    }
    str_assign(out.path, path);
    out.line_no = line_no;
    str_assign(out.text, line.substr(scan.close_pos + 1));
    out.is_match = scan.sep == ':';
    no_match = false;
    return tool_status::ok;
}

tool_status line_path_shape(kimix::string_view line, size_t &path_len, bool &no_match) noexcept {
    no_match = true;
    path_len = 0;
    const delim_scan scan = scan_delimiters(line, true); // _RG_LINE_RE: no DOTALL
    if (scan.unsupported) {
        return unsupported();
    }
    if (!scan.matched) {
        return tool_status::ok;
    }
    path_len = scan.path_len; // may be 0: the caller tests `if file_path`
    no_match = false;
    return tool_status::ok;
}

void format_match_line(uint32_t line_number, kimix::string_view text, bool is_match,
                       kimix::string &out) {
    out.clear();
    out.push_back(is_match ? '*' : ' ');
    append_num(out, line_number);
    out.push_back('|');
    str_append(out, text);
}

tool_status group_lines_by_file(kimix::span<const kimix::string> lines, kimix::vector<file_group> &out) {
    out.clear();
    bool have_current = false;
    kimix::string current_path;
    for (const kimix::string &line : lines) {
        content_line parsed;
        bool no_match = false;
        const tool_status status = parse_content_line(line, parsed, no_match);
        if (status == tool_status::unsupported) {
            out.clear();
            return unsupported();
        }
        if (no_match) {
            if (have_current && !is_blank_text(line)) {
                // Keep "--" separators and gap markers inside the current group.
                out.back().body.push_back(
                    grouped_entry{0u, kimix::string(line.data(), line.size()), false});
            }
            continue;
        }
        if (!have_current || current_path != parsed.path) {
            have_current = true;
            current_path = parsed.path;
            file_group group;
            str_assign(group.path, parsed.path);
            out.push_back(std::move(group));
        }
        out.back().body.push_back(
            grouped_entry{parsed.line_no, std::move(parsed.text), parsed.is_match});
    }
    return tool_status::ok;
}

void format_grouped_output(const kimix::vector<file_group> &groups, kimix::vector<kimix::string> &out) {
    out.clear();
    for (size_t i = 0; i < groups.size(); i++) {
        if (i > 0) {
            out.emplace_back(); // blank separator line, never before the first
        }
        kimix::string header = "# ";
        str_append(header, groups[i].path);
        out.push_back(std::move(header));
        for (const grouped_entry &entry : groups[i].body) {
            if (entry.line_no == 0u) {
                out.push_back(entry.text); // verbatim (separators, gap markers)
                continue;
            }
            kimix::string rendered;
            format_match_line(entry.line_no, entry.text, entry.is_match, rendered);
            out.push_back(std::move(rendered));
        }
    }
}

void group_line_indices_by_blank(kimix::span<const kimix::string> raw_lines,
                                 kimix::vector<kimix::vector<uint32_t>> &out) {
    out.clear();
    kimix::vector<uint32_t> current;
    for (size_t i = 0; i < raw_lines.size(); i++) {
        if (is_blank_text(raw_lines[i])) {
            if (!current.empty()) {
                out.push_back(std::move(current));
                current.clear();
            }
            continue;
        }
        current.push_back(static_cast<uint32_t>(i));
    }
    if (!current.empty()) {
        out.push_back(std::move(current));
    }
}

bool should_group(bool grouped, bool grouped_is_set, bool has_rich_entries) noexcept {
    return grouped_is_set ? grouped : has_rich_entries;
}

tool_status range_filter_lines(kimix::span<const kimix::string> lines, const ranges_map &map,
                               kimix::vector<kimix::string> &out) {
    out.clear();
    out.reserve(lines.size());
    if (map.empty()) {
        out.assign(lines.begin(), lines.end()); // `if not ranges_map: return lines`
        return tool_status::ok;
    }
    for (const kimix::string &line : lines) {
        content_line parsed;
        bool no_match = false;
        const tool_status status = parse_content_line(line, parsed, no_match);
        if (status == tool_status::unsupported) {
            out.clear();
            return unsupported();
        }
        if (no_match) {
            out.push_back(line);
            continue;
        }
        const kimix::vector<line_range> *spec = ranges_map_find(map, parsed.path);
        if (spec != nullptr && !is_line_in_ranges(parsed.line_no, *spec)) {
            continue;
        }
        out.push_back(line);
    }
    kimix::vector<kimix::string> swept;
    swept.reserve(out.size());
    for (kimix::string &line : out) {
        if (line == "--" && (swept.empty() || swept.back() == "--")) {
            continue; // drop leading and doubled separators
        }
        swept.push_back(std::move(line));
    }
    while (!swept.empty() && swept.back() == "--") {
        swept.pop_back(); // drop trailing separators
    }
    out = std::move(swept);
    return tool_status::ok;
}

tool_status reattach_single_file_prefix(kimix::span<const kimix::string> lines,
                                        kimix::string_view prefix,
                                        kimix::vector<kimix::string> &out) {
    out.clear();
    out.reserve(lines.size());
    if (prefix.empty()) {
        out.assign(lines.begin(), lines.end()); // `if not prefix: return lines`
        return tool_status::ok;
    }
    for (const kimix::string &line : lines) {
        // _BARE_CONTENT_RE: ^(\d+)([:\-])
        size_t pos = 0;
        while (pos < line.size() && is_digit(line[pos])) {
            pos++;
        }
        const bool bare = pos > 0u && pos < line.size() && (line[pos] == ':' || line[pos] == '-');
        if (!bare) {
            // The run stopped either at a non-digit or at the end. If the
            // stopping byte is non-ASCII, Python's \d (Nd) may keep going and
            // reach a delimiter there - unknowable without Unicode tables.
            if (pos < line.size() && static_cast<uint8_t>(line[pos]) >= 0x80u) {
                out.clear();
                return unsupported();
            }
            out.push_back(line);
            continue;
        }
        // `f"{prefix}{sep}{line}"`: the matched separator comes after the
        // prefix and the whole original line follows it.
        kimix::string attached;
        attached.reserve(prefix.size() + line.size() + 1);
        str_append(attached, prefix);
        attached.push_back(line[pos]);
        str_append(attached, line);
        out.push_back(std::move(attached));
    }
    return tool_status::ok;
}

void strip_path_prefix(kimix::span<const kimix::string> lines, kimix::string_view search_base,
                       kimix::vector<kimix::string> &out) {
    out.clear();
    out.reserve(lines.size());
    // Python: replace FIRST, then rstrip("/") - "C:\w\" must strip to "C:/w".
    kimix::string prefix = to_forward_slashes(search_base);
    while (!prefix.empty() && prefix.back() == '/') {
        prefix.pop_back();
    }
    const kimix::string prefix_slash = prefix + "/";
    for (const kimix::string &line : lines) {
        if (starts_with(to_forward_slashes(line), prefix_slash)) {
            // Python slices the ORIGINAL string: line[len(prefix_slash):].
            out.emplace_back(line.data() + prefix_slash.size(), line.size() - prefix_slash.size());
        } else {
            out.push_back(line);
        }
    }
}

tool_status normalize_slashes_content(kimix::span<const kimix::string> lines,
                                      kimix::string_view output_mode, bool on_windows,
                                      kimix::vector<kimix::string> &out) {
    out.clear();
    out.reserve(lines.size());
    if (!on_windows) {
        out.assign(lines.begin(), lines.end()); // POSIX passthrough (os.sep != "\\")
        return tool_status::ok;
    }
    const bool non_content = output_mode != "content";
    for (const kimix::string &line : lines) {
        content_line parsed;
        bool no_match = false;
        const tool_status status = parse_content_line(line, parsed, no_match);
        if (status == tool_status::unsupported) {
            out.clear();
            return unsupported();
        }
        if (no_match) {
            if (non_content) {
                out.push_back(to_forward_slashes(line)); // line.replace("\\", "/")
            } else {
                out.push_back(line);
            }
            continue;
        }
        kimix::string rebuilt;
        rebuilt.reserve(line.size() + 8);
        str_append(rebuilt, to_forward_slashes(parsed.path));
        const char sep = parsed.is_match ? ':' : '-';
        rebuilt.push_back(sep);
        append_num(rebuilt, parsed.line_no);
        rebuilt.push_back(sep);
        str_append(rebuilt, parsed.text);
        out.push_back(std::move(rebuilt));
    }
    return tool_status::ok;
}

tool_status collect_record_files(kimix::span<const kimix::string> lines, kimix::string_view output_mode,
                                 kimix::vector<kimix::string> &out) {
    out.clear();
    const auto push_unique = [&](kimix::string_view path) {
        for (const kimix::string &existing : out) {
            if (existing == path) {
                return;
            }
        }
        out.emplace_back(path.data(), path.size());
    };
    if (output_mode == "content") {
        for (const kimix::string &line : lines) {
            content_line parsed;
            bool no_match = false;
            const tool_status status = parse_content_line(line, parsed, no_match);
            if (status == tool_status::unsupported) {
                out.clear();
                return unsupported();
            }
            if (!no_match) {
                push_unique(parsed.path);
            }
        }
        return tool_status::ok;
    }
    for (const kimix::string &line : lines) {
        if (output_mode == "count_matches") {
            const size_t idx = line.rfind(':');
            push_unique(idx != kimix::string_view::npos && idx > 0u
                            ? kimix::string_view(line).substr(0, idx)
                            : kimix::string_view(line));
        } else {
            push_unique(line);
        }
    }
    return tool_status::ok;
}

// ===========================================================================
// 4. rtk protocol (output_utils.py + grep_local._rtk_fold_note)
// ===========================================================================

namespace {

// `^(\d+) matches in (\d+) files:$`
bool rtk_header_match(kimix::string_view line, uint32_t &matches, uint32_t &files) noexcept {
    size_t pos = 0;
    while (pos < line.size() && is_digit(line[pos])) {
        pos++;
    }
    if (pos == 0u) {
        return false;
    }
    const kimix::string_view m_digits = line.substr(0, pos);
    constexpr kimix::string_view mid = " matches in ";
    if (!starts_with(line.substr(pos), mid)) {
        return false;
    }
    pos += mid.size();
    const size_t f_begin = pos;
    while (pos < line.size() && is_digit(line[pos])) {
        pos++;
    }
    if (pos == f_begin) {
        return false;
    }
    const kimix::string_view f_digits = line.substr(f_begin, pos - f_begin);
    if (line.substr(pos) != " files:") {
        return false;
    }
    return digits_to_u32(m_digits, matches) && digits_to_u32(f_digits, files);
}

// `^\s*\+(\d+) more files \[see remaining: (.*)\]$` - the greedy payload takes
// everything up to the LAST ']' of the line.
bool rtk_files_fold_match(kimix::string_view line, uint32_t &count,
                          kimix::string_view &hint) noexcept {
    size_t pos = 0;
    while (pos < line.size() && is_ws_ascii(line[pos])) {
        pos++;
    }
    if (pos >= line.size() || line[pos] != '+') {
        return false;
    }
    pos++;
    const size_t d_begin = pos;
    while (pos < line.size() && is_digit(line[pos])) {
        pos++;
    }
    if (pos == d_begin) {
        return false;
    }
    const kimix::string_view digits = line.substr(d_begin, pos - d_begin);
    constexpr kimix::string_view mid = " more files [see remaining: ";
    if (!starts_with(line.substr(pos), mid)) {
        return false;
    }
    const kimix::string_view rest = line.substr(pos + mid.size());
    if (rest.empty() || rest.back() != ']') {
        return false;
    }
    hint = rest.substr(0, rest.size() - 1);
    return digits_to_u32(digits, count);
}

// `^\s*\+(\d+) more in (.+?) \[see remaining: (.*)\]$`. The path group is lazy,
// so the FIRST " [see remaining: " whose payload ends the line with ']' wins;
// the payload group is greedy and takes everything up to that final ']'.
bool rtk_per_file_fold_match(kimix::string_view line, uint32_t &count, kimix::string_view &path,
                             kimix::string_view &hint) noexcept {
    size_t pos = 0;
    while (pos < line.size() && is_ws_ascii(line[pos])) {
        pos++;
    }
    if (pos >= line.size() || line[pos] != '+') {
        return false;
    }
    pos++;
    const size_t d_begin = pos;
    while (pos < line.size() && is_digit(line[pos])) {
        pos++;
    }
    if (pos == d_begin) {
        return false;
    }
    const kimix::string_view digits = line.substr(d_begin, pos - d_begin);
    constexpr kimix::string_view mid = " more in ";
    if (!starts_with(line.substr(pos), mid)) {
        return false;
    }
    pos += mid.size();
    const size_t path_begin = pos;
    constexpr kimix::string_view tail = " [see remaining: ";
    for (size_t p = path_begin; p + tail.size() <= line.size(); p++) {
        if (!starts_with(line.substr(p), tail)) {
            continue;
        }
        const kimix::string_view rest = line.substr(p + tail.size());
        if (rest.empty() || rest.back() != ']') {
            continue;
        }
        path = line.substr(path_begin, p - path_begin);
        hint = rest.substr(0, rest.size() - 1);
        return !path.empty() && digits_to_u32(digits, count);
    }
    return false;
}

// `_parse_tail_hint`: `^tail -n \+(\d+)\s+(\S+)$` on the stripped hint.
// Returns true with the parsed (start_line, log); false means "log = the
// stripped hint text itself, no start line" (Python's fallback branch).
bool parse_tail_hint(kimix::string_view hint, uint32_t &start_line, kimix::string_view &log) noexcept {
    const kimix::string_view t = strip_ascii_ws(hint);
    constexpr kimix::string_view prefix = "tail -n +";
    if (!starts_with(t, prefix)) {
        return false;
    }
    size_t pos = prefix.size();
    const size_t d_begin = pos;
    while (pos < t.size() && is_digit(t[pos])) {
        pos++;
    }
    if (pos == d_begin) {
        return false;
    }
    const kimix::string_view digits = t.substr(d_begin, pos - d_begin);
    if (pos >= t.size() || !is_ws_ascii(t[pos])) {
        return false;
    }
    pos++;
    while (pos < t.size() && is_ws_ascii(t[pos])) {
        pos++;
    }
    const kimix::string_view rest = t.substr(pos);
    // \S+ is fully anchored: no whitespace anywhere in the remainder.
    if (rest.empty() || contains(rest, " ") || contains(rest, "\t")) {
        return false;
    }
    if (!digits_to_u32(digits, start_line)) {
        return false;
    }
    log = rest;
    return true;
}

// Every rtk marker contains one of these literals; a line without any of them
// cannot be a protocol line, so it passes through even when non-ASCII.
bool rtk_marker_candidate(kimix::string_view line) noexcept {
    return contains(line, " matches in ") || contains(line, " more in ") ||
           contains(line, " more files ") || contains(line, "tail -n +");
}

} // namespace

tool_status parse_rtk_rg_output(kimix::span<const kimix::string> lines,
                                kimix::vector<kimix::string> &cleaned, rtk_meta &meta) {
    cleaned.clear();
    cleaned.reserve(lines.size());
    meta = rtk_meta{};
    const size_t n = lines.size();
    for (size_t i = 0; i < n; i++) {
        const kimix::string_view line = lines[i];
        if (!is_all_ascii(line) && rtk_marker_candidate(line)) {
            cleaned.clear();
            meta = rtk_meta{};
            return unsupported();
        }
        uint32_t matches = 0;
        uint32_t files = 0;
        if (rtk_header_match(line, matches, files)) {
            meta.total_matches = matches;
            meta.total_files = files;
            // The header is followed by a blank separator line - drop it too.
            if (i + 1 < n && is_blank_text(lines[i + 1])) {
                i++;
            }
            continue;
        }
        uint32_t count = 0;
        kimix::string_view path;
        kimix::string_view hint;
        if (rtk_per_file_fold_match(line, count, path, hint)) {
            rtk_folded_file folded;
            folded.count = count;
            str_assign(folded.path, path);
            const kimix::string_view trimmed = strip_ascii_ws(hint);
            str_assign(folded.log, trimmed); // `content.strip() or None`
            folded.has_log = !trimmed.empty();
            uint32_t start_line = 0;
            kimix::string_view log;
            if (parse_tail_hint(hint, start_line, log)) {
                folded.start_line = start_line;
                str_assign(folded.log, log);
            }
            meta.folded_files.push_back(std::move(folded));
            continue;
        }
        if (rtk_files_fold_match(line, count, hint)) {
            meta.skipped_files = count;
            // `_parse_tail_hint`: a recognized `tail -n +K <log>` keeps only
            // the log path (start_line is discarded here); anything else keeps
            // the stripped payload text. Empty -> Python None.
            uint32_t start_line = 0;
            kimix::string_view log;
            if (parse_tail_hint(hint, start_line, log)) {
                str_assign(meta.skipped_log, log);
                meta.has_skipped_log = true;
            } else {
                const kimix::string_view trimmed = strip_ascii_ws(hint);
                str_assign(meta.skipped_log, trimmed);
                meta.has_skipped_log = !trimmed.empty();
            }
            continue;
        }
        cleaned.emplace_back(line.data(), line.size());
    }
    return tool_status::ok;
}

void rtk_fold_note(const rtk_meta &meta, kimix::string_view original_path, kimix::string &out) {
    out.clear();
    kimix::vector<kimix::string> parts;
    for (const rtk_folded_file &entry : meta.folded_files) {
        kimix::string part;
        append_num(part, entry.count);
        str_append(part, " more lines in ");
        str_append(part, entry.path);
        parts.push_back(std::move(part));
    }
    if (meta.skipped_files.has_value()) {
        kimix::string part;
        append_num(part, *meta.skipped_files);
        str_append(part, " more files");
        parts.push_back(std::move(part));
    }
    if (parts.empty()) {
        return; // Python returns None
    }
    kimix::string note = "rtk folded output: ";
    for (size_t i = 0; i < parts.size(); i++) {
        if (i > 0) {
            str_append(note, "; ");
        }
        str_append(note, parts[i]);
    }
    str_append(note, ".");

    kimix::string log;
    bool has_log = false;
    if (!meta.folded_files.empty() && meta.folded_files.back().has_log) {
        const rtk_folded_file &last = meta.folded_files.back();
        if (last.start_line.has_value()) {
            str_append(log, "tail -n +");
            append_num(log, *last.start_line);
            log.push_back(' ');
            str_append(log, last.log);
        } else {
            str_append(log, last.log);
        }
        has_log = true;
    } else if (meta.has_skipped_log) {
        str_append(log, meta.skipped_log);
        has_log = true;
    }
    if (has_log) {
        str_append(note, " Full log: ");
        str_append(note, log);
    }
    if (!original_path.empty()) {
        str_append(note, " Original output: ");
        str_append(note, to_forward_slashes(original_path));
    }
    out = std::move(note);
}

// ===========================================================================
// 5. Recorder (grep_recorder.py)
// ===========================================================================

void recorder_record(kimix::vector<kimix::string> &existing, kimix::string_view path) {
    if (path.empty()) {
        return; // `if not relative_path: return`
    }
    for (const kimix::string &e : existing) {
        if (e == path) {
            return; // `relative_path in self._seen`
        }
    }
    existing.emplace_back(path.data(), path.size());
}

void recorder_merge(kimix::span<const kimix::string> existing, kimix::span<const kimix::string> fresh,
                    size_t cap, kimix::vector<kimix::string> &out) {
    out.clear();
    out.reserve(existing.size() + fresh.size());
    const auto add = [&](const kimix::string &item) {
        if (item.empty()) {
            return; // Python skips falsy entries
        }
        for (const kimix::string &e : out) {
            if (e == item) {
                return;
            }
        }
        out.push_back(item);
    };
    for (const kimix::string &item : existing) {
        add(item);
    }
    for (const kimix::string &item : fresh) {
        add(item);
    }
    if (out.size() > cap) {
        // `merged[-RECORDER_CAP:]`: keep the tail, drop from the front.
        out.erase(out.begin(), out.begin() + static_cast<ptrdiff_t>(out.size() - cap));
    }
}

// ===========================================================================
// 6. Sensitive files (utils/sensitive.py)
// ===========================================================================

void posix_basename(kimix::string_view path, kimix::string &out) {
    out.clear();
    // PurePosixPath drops empty and "." components; the name is the last one
    // ("./config/.env" -> ".env").
    size_t pos = 0;
    kimix::string_view last;
    bool any = false;
    while (true) {
        const size_t at = path.find('/', pos);
        const size_t stop = at == kimix::string_view::npos ? path.size() : at;
        const kimix::string_view part = path.substr(pos, stop - pos);
        if (!part.empty() && part != ".") {
            last = part;
            any = true;
        }
        if (at == kimix::string_view::npos) {
            break;
        }
        pos = at + 1;
    }
    if (any) {
        str_assign(out, last);
    }
}

void windows_basename(kimix::string_view path, kimix::string &out) {
    out.clear();
    // Mirrors pathlib's Windows parser: an optional drive ("X:" or
    // "\\server\share"), an optional root separator, then the tail components.
    // The name is the last tail component that is neither empty nor ".".
    const size_t n = path.size();
    size_t pos = 0;
    if (n >= 2u && is_alpha(path[0]) && path[1] == ':') {
        pos = 2; // drive-absolute ("C:/x") or drive-relative ("C:x/y")
    } else if (n >= 2u && is_path_sep(path[0]) && is_path_sep(path[1])) {
        // UNC: the drive is "\\server" plus an optional "\share".
        pos = 2;
        while (pos < n && !is_path_sep(path[pos])) {
            pos++;
        }
        while (pos < n && is_path_sep(path[pos])) {
            pos++;
        }
        const size_t share = pos;
        while (pos < n && !is_path_sep(path[pos])) {
            pos++;
        }
        if (pos == n || pos == share) {
            return; // "\\server", "\\server\" or "\\server\share[\]": no name
        }
        pos++; // the root separator that closes the UNC share
    } else if (n >= 1u && is_path_sep(path[0])) {
        pos = 1; // rooted without a drive
    }
    const kimix::string_view rest = path.substr(pos);
    size_t end = rest.size();
    while (true) {
        while (end > 0u && is_path_sep(rest[end - 1])) {
            end--; // trailing separators belong to no component
        }
        size_t begin = end;
        while (begin > 0u && !is_path_sep(rest[begin - 1])) {
            begin--;
        }
        const kimix::string_view part = rest.substr(begin, end - begin);
        if (!part.empty() && part != ".") {
            str_assign(out, part);
            return;
        }
        if (begin == 0u) {
            return;
        }
        end = begin - 1u; // step over the separator and keep looking
    }
}

tool_status is_sensitive_path(kimix::string_view path, bool on_windows, bool &out) noexcept {
    out = false;
    if (!is_all_ascii(path)) {
        return unsupported();
    }
    kimix::string name_storage;
    if (on_windows) {
        windows_basename(path, name_storage);
    } else {
        posix_basename(path, name_storage);
    }
    const kimix::string_view name = name_storage;
    for (const kimix::string_view exemption : sensitive_exemptions) {
        if (name == exemption) {
            return tool_status::ok; // exempt: never sensitive
        }
    }
    for (const kimix::string_view pattern : sensitive_patterns) {
        if (contains(pattern, "/")) {
            if (path.size() >= pattern.size() &&
                path.compare(path.size() - pattern.size(), pattern.size(), pattern) == 0) {
                out = true; // path.endswith(pattern)
                return tool_status::ok;
            }
            kimix::string needle = "/";
            str_append(needle, pattern);
            if (contains(path, needle)) {
                out = true; // ("/" + pattern) in path
                return tool_status::ok;
            }
            continue;
        }
        if (fnmatch_ascii(name, pattern, on_windows)) {
            out = true;
            return tool_status::ok;
        }
    }
    return tool_status::ok;
}

tool_status sensitive_file_warning(kimix::span<const kimix::string> paths, bool on_windows,
                                   kimix::string &out) {
    for (const kimix::string &p : paths) {
        if (!is_all_ascii(p)) {
            return unsupported();
        }
    }
    kimix::vector<kimix::string> names;
    for (const kimix::string &p : paths) {
        kimix::string name;
        if (on_windows) {
            windows_basename(p, name);
        } else {
            posix_basename(p, name);
        }
        bool dup = false;
        for (const kimix::string &e : names) {
            if (e == name) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            names.push_back(std::move(name));
        }
    }
    std::sort(names.begin(), names.end());
    kimix::string file_list;
    for (size_t i = 0; i < names.size() && i < 5u; i++) {
        if (i > 0) {
            str_append(file_list, ", ");
        }
        str_append(file_list, names[i]);
    }
    if (names.size() > 5u) {
        str_append(file_list, kimix::format(", ... ({} files total)", names.size()));
    }
    out = kimix::format(
        "Skipped {} sensitive file(s) ({}) to protect secrets. These files may contain credentials or "
        "private keys.",
        paths.size(), file_list);
    return tool_status::ok;
}

// ===========================================================================
// 7. Pattern kernels (grep_local.py 106-138)
// ===========================================================================
// The runtime_py target already carries this port (src/runtime/tools/
// grep_pattern.cpp), but kimix-llm does not link runtime_py, so the pipeline
// re-declares the same two functions inside this namespace.

namespace {

// `(?<!\\)(?:\\\\)*\\n`: a run of an ODD number of backslashes immediately
// followed by 'n' (runs are maximal, so the lookbehind is automatic).
bool newline_escape_present(kimix::string_view p) noexcept {
    size_t i = 0;
    while (i < p.size()) {
        if (p[i] != '\\') {
            i++;
            continue;
        }
        size_t j = i;
        while (j < p.size() && p[j] == '\\') {
            j++;
        }
        if (j < p.size() && p[j] == 'n' && ((j - i) % 2u) == 1u) {
            return true;
        }
        i = j; // the 'n' cannot start a match itself
    }
    return false;
}

bool next_newline_escape(kimix::string_view p, size_t from, size_t &start, size_t &end) noexcept {
    size_t i = from;
    while (i < p.size()) {
        if (p[i] != '\\') {
            i++;
            continue;
        }
        size_t j = i;
        while (j < p.size() && p[j] == '\\') {
            j++;
        }
        if (j < p.size() && p[j] == 'n' && ((j - i) % 2u) == 1u) {
            start = i;
            end = j + 1;
            return true;
        }
        i = j;
    }
    return false;
}

} // namespace

tool_status pattern_has_regex_newline(kimix::string_view pattern, bool &out) noexcept {
    out = false;
    if (!is_all_ascii(pattern)) {
        return unsupported(); // the shim routes unicode patterns to Python
    }
    out = pattern.find('\n') != kimix::string_view::npos || newline_escape_present(pattern);
    return tool_status::ok;
}

tool_status multiline_pattern(kimix::string_view pattern, kimix::string &out) {
    out.clear();
    if (!is_all_ascii(pattern)) {
        return unsupported();
    }
    if (pattern.find('\n') == kimix::string_view::npos && !newline_escape_present(pattern)) {
        str_assign(out, pattern);
        return tool_status::ok;
    }
    // 1. pattern.replace("\r\n", "\n")
    kimix::string step1;
    step1.reserve(pattern.size());
    for (size_t i = 0; i < pattern.size(); i++) {
        if (pattern[i] == '\r' && i + 1 < pattern.size() && pattern[i + 1] == '\n') {
            step1.push_back('\n');
            i++;
            continue;
        }
        step1.push_back(pattern[i]);
    }
    // 2. every regex \n escape -> the literal 5 bytes "\r?\n"
    kimix::string step2;
    step2.reserve(step1.size() + 16);
    size_t pos = 0;
    size_t s = 0;
    size_t e = 0;
    while (next_newline_escape(step1, pos, s, e)) {
        str_append(step2, kimix::string_view(step1).substr(pos, s - pos));
        str_append(step2, "\\r?\\n");
        pos = e;
    }
    str_append(step2, kimix::string_view(step1).substr(pos));
    // 3. real newlines -> the literal "\r?\n"
    out.reserve(step2.size() + 16);
    pos = 0;
    for (size_t i = 0; i < step2.size(); i++) {
        if (step2[i] != '\n') {
            continue;
        }
        str_append(out, kimix::string_view(step2).substr(pos, i - pos));
        str_append(out, "\\r?\\n");
        pos = i + 1;
    }
    str_append(out, kimix::string_view(step2).substr(pos));
    return tool_status::ok;
}

// ===========================================================================
// 8. Byte-budget join (grep_local._join_with_byte_limit)
// ===========================================================================

bool join_with_byte_limit(kimix::span<const kimix::string> lines, size_t max_bytes, kimix::string &out,
                          bool &truncated, size_t &omitted) {
    truncated = false;
    omitted = 0;
    // Python measures len(line.encode("utf-8")); for valid UTF-8 bytes that is
    // simply the byte length. Invalid UTF-8 would raise UnicodeEncodeError
    // there, so the shim keeps that case.
    for (const kimix::string &line : lines) {
        if (!utf8_validate(line)) {
            out.clear();
            return false;
        }
    }
    size_t total = 0;
    for (const kimix::string &line : lines) {
        total += line.size() + 1u;
    }
    out.clear();
    out.reserve(total);
    for (size_t i = 0; i < lines.size(); i++) {
        if (!out.empty()) {
            out.push_back('\n'); // separator_bytes = 1 if result_lines else 0
        }
        str_append(out, lines[i]);
        if (out.size() >= max_bytes) {
            truncated = true;
            omitted = lines.size() - (i + 1u);
            break;
        }
    }
    return true;
}

// ===========================================================================
// 9. Regex line search (BLOCKED - see issue/grep.md)
// ===========================================================================

tool_status grep_search_lines(kimix::string_view content, kimix::string_view pattern,
                              bool ignore_case, bool dotall, kimix::vector<grep_hit> &out) {
    (void)content;
    (void)pattern;
    (void)ignore_case;
    (void)dotall;
    out.clear();
    // PCRE2 is not vendored in src/ext and adding a new ext library is
    // forbidden; std::regex and RE2 were rejected by plans/grep.md §3 for
    // parity reasons. The shim must keep the Python `regex` matcher.
    return tool_status::unsupported;
}

// ===========================================================================
// 10. Tool class wrapper (CallableTool2-style binding entry point)
// ===========================================================================

Grep::Grep(kimix::builtin_tools::Session *session) : kimix::builtin_tools::Tool(session) {}

namespace {

// Append a string status + message into `result`, then serialize it.
void grep_serialize_status(kimix::builtin_tools::ToolParams &result,
                           kimix::string_view status, kimix::string_view message,
                           kimix::vector<char> &out) {
    result.values["status"] = ValueElement::make_string(kimix::string(status));
    result.values["message"] = ValueElement::make_string(kimix::string(message));
    result.serialize(out);
}

} // namespace

void Grep::operator()(kimix::builtin_tools::ToolParams const *parameters) {
    _result.clear();
    kimix::builtin_tools::ToolParams result;
    if (parameters == nullptr) {
        grep_serialize_status(result, "invalid_input", "missing parameters", _result);
        return;
    }

    const ValueElement *pattern_el = parameters->get("pattern");
    if (pattern_el == nullptr || !pattern_el->is_string()) {
        grep_serialize_status(result, "invalid_input", "missing required field: pattern", _result);
        return;
    }
    const kimix::string_view pattern = pattern_el->as_string();
    if (pattern.empty()) {
        grep_serialize_status(result, "invalid_input", "pattern must be non-empty", _result);
        return;
    }

    const ValueElement *paths_el = parameters->get("paths");
    if (paths_el == nullptr) {
        grep_serialize_status(result, "invalid_input", "missing required field: paths", _result);
        return;
    }
    kimix::vector<kimix::string> paths_input;
    if (paths_el->is_string()) {
        paths_input.push_back(paths_el->as_string());
    } else if (paths_el->is_array()) {
        for (const ValueElement &item : paths_el->as_array()) {
            if (item.is_string()) {
                paths_input.push_back(item.as_string());
            }
        }
    } else {
        grep_serialize_status(result, "invalid_input",
                              "paths must be a string or an array of strings", _result);
        return;
    }
    if (paths_input.empty()) {
        grep_serialize_status(result, "invalid_input", "paths must not be empty", _result);
        return;
    }

    bool has_regex_newline = false;
    tool_status newline_status = pattern_has_regex_newline(pattern, has_regex_newline);
    if (newline_status != tool_status::ok) {
        grep_serialize_status(result, "unsupported",
                              "pattern contains non-ASCII constructs not supported by the native kernel",
                              _result);
        return;
    }

    kimix::string multiline_pattern_out;
    tool_status ml_status = multiline_pattern(pattern, multiline_pattern_out);
    if (ml_status != tool_status::ok) {
        grep_serialize_status(result, "unsupported",
                              "multiline_pattern preprocessing failed", _result);
        return;
    }

    kimix::vector<kimix::string> expanded_paths;
    for (const kimix::string &p : paths_input) {
        kimix::vector<kimix::string> chunk;
        tool_status exp_status = expand_path_entries(p, chunk);
        if (exp_status != tool_status::ok) {
            grep_serialize_status(result, "unsupported",
                                  "path expansion failed", _result);
            return;
        }
        expanded_paths.insert(expanded_paths.end(), chunk.begin(), chunk.end());
    }
    dedupe(expanded_paths);

    result.values["status"] = ValueElement::make_string(kimix::string("unsupported"));
    result.values["message"] =
        ValueElement::make_string(kimix::string(
            "native grep tool is a kernel library; full invocation requires Python-side orchestration"));
    result.values["pattern"] = ValueElement::make_string(kimix::string(pattern));
    result.values["multiline_pattern"] = ValueElement::make_string(multiline_pattern_out);
    result.values["has_regex_newline"] = ValueElement::make_bool(has_regex_newline);

    ValueElement::Array paths_arr;
    paths_arr.reserve(expanded_paths.size());
    for (const kimix::string &p : expanded_paths) {
        paths_arr.push_back(ValueElement::make_string(p));
    }
    result.values["paths"] = ValueElement::make_array(std::move(paths_arr));
    result.serialize(_result);
}

} // namespace kimix::builtin_tools::grep
