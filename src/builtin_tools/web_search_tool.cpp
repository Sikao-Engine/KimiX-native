// web_search_tool.cpp - Pure kernels for the web_search built-in agent tool.
//
// Exact ports of the Python reference (kimi-cli/src/kimi_cli/tools/web/):
//   content.py  convert_base64_images_to_links (38-65)
//   content.py  store_full_text (75-104)
//   content.py  truncate_with_footer (107-163)
//   content.py  get_extract_char_limit (166-186)
//   search.py   Params.limit (27-38) / SearchResult (41-49) /
//               SearchWeb.__call__ rendering (112-125)
//   providers.py _resolve (232-300)
//
// See web_search_tool.h for the plan/source-of-truth notes and the missing
// plan file (C:/dev/kimi-agent/plans/web_search.md) reconstruction.
//
// Unity-build safety: every helper is static inside an anonymous namespace in
// this TU (or in the tool namespace declared by the header); no file-scope
// `using namespace`.

#include "builtin_tools/web_search_tool.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <core/kimix_core.h>
#include <core/stl/filesystem.h>

#include "builtin_tools/utf8_util.h"

// Vendored xxHash — XXH64 (NOT kimix::hash64, which is XXH3; the plan flags
// the XXH3-vs-XXH64 mix-up as a silent cache-key break). The macro makes the
// header self-contained (the same pattern the xxhash unit test uses).
#define XXH_INLINE_ALL
#include "xxhash.h"

namespace kimix {
namespace builtin_tools {
namespace web_search {

namespace {

// ---- ASCII helpers ---------------------------------------------------------

// The two whitespace classes of the reference, pinned empirically against the
// kimi-agent reference (Python 3.14 / `regex` module):
//
// * ``regex`` ``\s`` (used by the payload classes and the optional space after
//   '(' / ']('): 25 code points — U+0009..U+000D, U+0020, U+0085, U+00A0,
//   U+1680, U+2000..U+200A, U+2028, U+2029, U+202F, U+205F, U+3000.
//   NOTE: \x1c-\x1f are NOT whitespace for the `regex` module.
// * ``str.strip()`` on the markdown alt text (``str.isspace()``): the same 25
//   plus U+001C..U+001F (29 code points).
enum class ws_space_class : uint8_t {
    regex_s,   // `regex` \s — payload class / prefix whitespace
    str_strip, // str.isspace() — markdown alt .strip()
};

bool ws_is_space_code_point(uint32_t cp, ws_space_class cls) noexcept {
    switch (cp) {
    case 0x09u: case 0x0Au: case 0x0Bu: case 0x0Cu: case 0x0Du: case 0x20u:
    case 0x85u: case 0xA0u: case 0x1680u: case 0x2028u: case 0x2029u:
    case 0x202Fu: case 0x205Fu: case 0x3000u:
        return true;
    case 0x1Cu: case 0x1Du: case 0x1Eu: case 0x1Fu:
        return cls == ws_space_class::str_strip;
    default:
        return cp >= 0x2000u && cp <= 0x200Au;
    }
}

// Byte width of the whitespace code point at `i`, or 0 when `text[i]` does not
// start a whitespace character of `cls`.
size_t ws_space_width(kimix::string_view text, size_t i,
                      ws_space_class cls) noexcept {
    if (i >= text.size()) {
        return 0u;
    }
    const unsigned char b = static_cast<unsigned char>(text[i]);
    if (b < 0x80u) {
        return ws_is_space_code_point(b, cls) ? 1u : 0u;
    }
    const char *it = text.data() + i;
    const char *end = text.data() + text.size();
    // The scanner only needs the code point value; the decode also yields how
    // many bytes it consumed, so re-decode to get the width.
    const char *probe = it;
    const uint32_t cp = decode_code_point(probe, end);
    if (!ws_is_space_code_point(cp, cls)) {
        return 0u;
    }
    return static_cast<size_t>(probe - it);
}

size_t ws_skip_spaces(kimix::string_view text, size_t i,
                      ws_space_class cls) noexcept {
    for (;;) {
        const size_t w = ws_space_width(text, i, cls);
        if (w == 0u) {
            return i;
        }
        i += w;
    }
}

bool ws_is_base64_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
}

bool ws_has_at(kimix::string_view text, size_t pos, kimix::string_view lit) noexcept {
    return pos + lit.size() <= text.size() &&
           text.substr(pos, lit.size()) == lit;
}

// Python-style thousands separator ("{n:,}").
kimix::string ws_thousands(int64_t n) {
    kimix::StringScratch ss;
    ss << static_cast<long long>(n);
    kimix::string digits = std::move(ss.string());
    kimix::string out;
    out.reserve(digits.size() + digits.size() / 3u + 1u);
    for (size_t i = 0; i < digits.size(); ++i) {
        out.push_back(digits[i]);
        const size_t remaining = digits.size() - i - 1u;
        if (remaining > 0u && remaining % 3u == 0u) {
            out.push_back(',');
        }
    }
    return out;
}

size_t ws_count_newlines(kimix::string_view s) noexcept {
    size_t n = 0;
    for (const char c : s) {
        if (c == '\n') {
            ++n;
        }
    }
    return n;
}

// ---- convert_base64_images_to_links scanner --------------------------------
//
// One left-to-right pass that, at each position, tries the three reference
// `regex` patterns in order:
//   1. !\[(?P<alt>[^\]]*)\]\(\s*data:image/[^;]+;base64,[A-Za-z0-9+/=\s]+\)
//   2. \(\s*data:image/[^;]+;base64,[A-Za-z0-9+/=\s]+\)
//   3. data:image/[^;]+;base64,[A-Za-z0-9+/=]+
// The first chars are disjoint ('!', '(', 'd'), so at most one matches per
// position and the result is identical to the three sequential re.sub passes
// of the reference (verified against the Python goldens).

struct ws_match {
    bool is_md = false;
    size_t begin = 0; // == scan position
    size_t end = 0;   // exclusive
    size_t alt_begin = 0;
    size_t alt_end = 0;
    size_t payload_begin = 0;
    size_t payload_end = 0;
};

// Shared blob matcher: text[start..] must begin with "data:image/". Mirrors
// `[^;]+` (one+ chars up to the first ';'), the literal ";base64,", and the
// payload class `[A-Za-z0-9+/=]` (+ `\s` when allow_space). When
// require_close_paren, the char immediately after the payload run must be ')'
// (the payload class never contains ')', so greedy + backtrack collapses to
// "run followed by ')'").
bool ws_match_data_blob(kimix::string_view text, size_t start,
                        bool require_close_paren, bool allow_space,
                        size_t &payload_begin, size_t &payload_end,
                        size_t &match_end) noexcept {
    size_t i = start + 11u; // "data:image/" (11 bytes)
    const size_t type_start = i;
    while (i < text.size() && text[i] != ';') {
        ++i;
    }
    if (i == type_start) {
        return false; // [^;]+ needs at least one char ("data:image/;base64," fails)
    }
    if (!ws_has_at(text, i, ";base64,")) {
        return false;
    }
    i += 8u; // ";base64,"
    payload_begin = i;
    while (i < text.size()) {
        if (ws_is_base64_char(text[i])) {
            ++i;
            continue;
        }
        if (allow_space) {
            const size_t w = ws_space_width(text, i, ws_space_class::regex_s);
            if (w != 0u) {
                i += w;
                continue;
            }
        }
        break;
    }
    if (i == payload_begin) {
        return false; // payload class is '+' — must be non-empty
    }
    if (require_close_paren) {
        if (i >= text.size() || text[i] != ')') {
            return false;
        }
        match_end = i + 1u;
    } else {
        match_end = i;
    }
    payload_end = i;
    return true;
}

bool ws_find_md_base64(kimix::string_view text, size_t from, ws_match &m) noexcept {
    if (from + 2u > text.size() || text[from] != '!' || text[from + 1u] != '[') {
        return false;
    }
    const size_t alt_begin = from + 2u;
    size_t alt_end = alt_begin;
    while (alt_end < text.size() && text[alt_end] != ']') {
        ++alt_end;
    }
    if (alt_end >= text.size()) {
        return false; // unterminated alt
    }
    if (alt_end + 1u >= text.size() || text[alt_end] != ']' ||
        text[alt_end + 1u] != '(') {
        return false;
    }
    size_t i = alt_end + 2u;
    i = ws_skip_spaces(text, i, ws_space_class::regex_s);
    if (!ws_has_at(text, i, "data:image/")) {
        return false;
    }
    size_t payload_begin = 0;
    size_t payload_end = 0;
    size_t match_end = 0;
    if (!ws_match_data_blob(text, i, /*require_close_paren=*/true,
                            /*allow_space=*/true, payload_begin, payload_end,
                            match_end)) {
        return false;
    }
    m.is_md = true;
    m.begin = from;
    m.end = match_end;
    m.alt_begin = alt_begin;
    m.alt_end = alt_end;
    m.payload_begin = payload_begin;
    m.payload_end = payload_end;
    return true;
}

bool ws_find_paren_base64(kimix::string_view text, size_t from, ws_match &m) noexcept {
    if (text[from] != '(') {
        return false;
    }
    size_t i = from + 1u;
    i = ws_skip_spaces(text, i, ws_space_class::regex_s);
    if (!ws_has_at(text, i, "data:image/")) {
        return false;
    }
    size_t payload_begin = 0;
    size_t payload_end = 0;
    size_t match_end = 0;
    if (!ws_match_data_blob(text, i, /*require_close_paren=*/true,
                            /*allow_space=*/true, payload_begin, payload_end,
                            match_end)) {
        return false;
    }
    m.is_md = false;
    m.begin = from;
    m.end = match_end;
    m.payload_begin = payload_begin;
    m.payload_end = payload_end;
    return true;
}

bool ws_find_bare_base64(kimix::string_view text, size_t from, ws_match &m) noexcept {
    if (!ws_has_at(text, from, "data:image/")) {
        return false;
    }
    size_t payload_begin = 0;
    size_t payload_end = 0;
    size_t match_end = 0;
    if (!ws_match_data_blob(text, from, /*require_close_paren=*/false,
                            /*allow_space=*/false, payload_begin, payload_end,
                            match_end)) {
        return false;
    }
    m.is_md = false;
    m.begin = from;
    m.end = match_end;
    m.payload_begin = payload_begin;
    m.payload_end = payload_end;
    return true;
}

kimix::string ws_replacement(kimix::string_view text, const ws_match &m) {
    if (!m.is_md) {
        return "[IMAGE]";
    }
    // Python `(m.group("alt") or "").strip()` — str.strip() removes
    // str.isspace() characters (which includes \x1c-\x1f, unlike `regex` \s).
    size_t b = m.alt_begin;
    size_t e = m.alt_end;
    while (b < e) {
        const size_t w = ws_space_width(text, b, ws_space_class::str_strip);
        if (w == 0u || b + w > e) {
            break;
        }
        b += w;
    }
    while (e > b) {
        // Walk back to the start of the last code point before `e`.
        size_t start = e - 1u;
        while (start > b && (static_cast<unsigned char>(text[start]) & 0xC0u) == 0x80u) {
            --start;
        }
        const size_t w = ws_space_width(text, start, ws_space_class::str_strip);
        if (w == 0u || start + w != e) {
            break;
        }
        e = start;
    }
    if (b == e) {
        return "[IMAGE]";
    }
    kimix::string out = "[IMAGE: ";
    out.append(text.data() + b, e - b);
    out += "]";
    return out;
}

kimix::string ws_scan_base64(kimix::string_view text,
                            kimix::vector<kimix::string> *payloads) {
    kimix::string out;
    out.reserve(text.size());
    size_t pos = 0;
    while (pos < text.size()) {
        ws_match m;
        bool matched = false;
        if (text[pos] == '!') {
            matched = ws_find_md_base64(text, pos, m);
        } else if (text[pos] == '(') {
            matched = ws_find_paren_base64(text, pos, m);
        } else {
            matched = ws_find_bare_base64(text, pos, m);
        }
        if (matched) {
            out += ws_replacement(text, m);
            if (payloads != nullptr) {
                payloads->emplace_back(text.data() + m.payload_begin,
                                       m.payload_end - m.payload_begin);
            }
            pos = m.end;
        } else {
            out.push_back(text[pos]);
            ++pos;
        }
    }
    return out;
}

// ---- URL hostname (content.py store_full_text host extraction) -------------

// Python's SplitResult.hostname is lower-cased (``hostname.lower()``), so
// "https://EXAMPLE.com/x" caches as "example.com-<digest>.md". ASCII folding
// covers every host that can actually appear (IDNA/registered names are ASCII
// or punycode); Python's Unicode .lower() can *expand* a code point
// (U+0130 -> "i\u0307"), which this cannot — recorded as a residual deviation.
void ws_ascii_lower_in_place(kimix::string &s) noexcept {
    for (auto &c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
}

kimix::string ws_url_hostname(kimix::string_view url) {
    const size_t scheme = url.find("://");
    if (scheme == kimix::string_view::npos) {
        return {}; // urlparse: no scheme -> hostname None -> "page"
    }
    size_t start = scheme + 3u;
    size_t end = start;
    while (end < url.size() && url[end] != '/' && url[end] != '?' &&
           url[end] != '#') {
        ++end;
    }
    kimix::string_view netloc = url.substr(start, end - start);
    const size_t at = netloc.rfind('@');
    if (at != kimix::string_view::npos) {
        netloc = netloc.substr(at + 1u);
    }
    if (!netloc.empty() && netloc[0] == '[') {
        const size_t rb = netloc.find(']');
        if (rb == kimix::string_view::npos) {
            return {};
        }
        kimix::string host(netloc.substr(1u, rb - 1u));
        ws_ascii_lower_in_place(host);
        return host;
    }
    const size_t colon = netloc.find(':');
    if (colon != kimix::string_view::npos) {
        netloc = netloc.substr(0u, colon);
    }
    kimix::string host(netloc);
    ws_ascii_lower_in_place(host);
    return host;
}

// ---- ToolResultBuilder(max_line_length=None) emulation ---------------------
//
// search.py renders through ``ToolResultBuilder(max_line_length=None)``
// (tools/utils.py). Reproducing its cap byte-for-byte needs three pieces of
// Python behaviour:
//
//   1. ``str.splitlines(keepends=True)`` boundaries: \n, \r, \r\n (one line),
//      \v, \f, \x1c, \x1d, \x1e, \x85, U+2028, U+2029.
//   2. ``truncate_line(line, remaining, "[...truncated]")``: a line longer than
//      the remaining character budget keeps its trailing [\r\n]+ run and is cut
//      to ``remaining`` code points ending in marker + that run (the marker
//      wins when the budget is smaller than the marker).
//   3. ``write()`` stops as soon as the buffer holds ``max_chars`` code points,
//      dropping every later line/chunk without shortening it.
//
// Everything is measured in code points (Python ``len(str)``); byte offsets go
// through utf8_util so a cut never lands inside a sequence.

bool ws_is_splitlines_boundary(uint32_t cp) noexcept {
    switch (cp) {
    case 0x0Au: // \n
    case 0x0Bu: // \v
    case 0x0Cu: // \f
    case 0x1Cu:
    case 0x1Du:
    case 0x1Eu:
    case 0x85u: // U+0085 NEL
    case 0x2028u:
    case 0x2029u:
        return true;
    default:
        return false;
    }
}

// End offset (exclusive) of the splitlines() line starting at `i`, terminator
// included. Returns `text.size()` for an unterminated final line.
size_t ws_splitline_end(kimix::string_view text, size_t i) noexcept {
    while (i < text.size()) {
        const char *p = text.data() + i;
        const char *end = text.data() + text.size();
        const char *probe = p;
        const uint32_t cp = decode_code_point(probe, end);
        const size_t w = static_cast<size_t>(probe - p);
        if (cp == 0x0Du) { // \r or \r\n
            i += w;
            if (i < text.size() && text[i] == '\n') {
                ++i;
            }
            return i;
        }
        i += w;
        if (ws_is_splitlines_boundary(cp)) {
            return i;
        }
    }
    return i;
}

// tools/utils.py truncate_line(line, max_length, marker).
void ws_truncate_line(kimix::string_view line, size_t max_code_points,
                      kimix::string &out, bool &changed) {
    const size_t line_cp = utf8_code_point_count(line);
    if (line_cp <= max_code_points) {
        out.assign(line.data(), line.size());
        return;
    }
    size_t brk_begin = line.size();
    while (brk_begin > 0u &&
           (line[brk_begin - 1u] == '\n' || line[brk_begin - 1u] == '\r')) {
        --brk_begin;
    }
    const kimix::string_view linebreak = line.substr(brk_begin);
    // end = marker + linebreak; max_length = max(max_length, len(end))
    const size_t end_cp = k_tool_result_truncation_marker.size() +
                          utf8_code_point_count(linebreak);
    const size_t keep_cp = max_code_points > end_cp ? max_code_points : end_cp;
    out.assign(
        line.data(),
        utf8_byte_offset_of_code_point(line, keep_cp - end_cp));
    out.append(k_tool_result_truncation_marker.data(),
               k_tool_result_truncation_marker.size());
    out.append(linebreak.data(), linebreak.size());
    changed = true;
}

struct ws_tool_result_builder {
    size_t max_chars = 0u; // 0 = unlimited
    kimix::string out;
    size_t n_chars = 0u;
    bool truncated = false;

    bool is_full() const noexcept {
        return max_chars != 0u && n_chars >= max_chars;
    }

    void write(kimix::string_view text) {
        if (is_full() || text.empty()) {
            return;
        }
        size_t i = 0u;
        while (i < text.size()) {
            if (is_full()) {
                break;
            }
            const size_t line_end = ws_splitline_end(text, i);
            const size_t remaining =
                max_chars == 0u ? static_cast<size_t>(-1) : max_chars - n_chars;
            kimix::string piece;
            bool changed = false;
            ws_truncate_line(text.substr(i, line_end - i), remaining, piece,
                             changed);
            if (changed) {
                truncated = true;
            }
            out += piece;
            n_chars += utf8_code_point_count(piece);
            i = line_end;
        }
    }
};

} // namespace

// ---------------------------------------------------------------------------
// search.py Params.limit + content.py get_extract_char_limit
// ---------------------------------------------------------------------------

int32_t clamp_search_limit(int64_t limit) {
    if (limit < k_min_search_limit) {
        return k_min_search_limit;
    }
    if (limit > k_max_search_limit) {
        return k_max_search_limit;
    }
    return static_cast<int32_t>(limit);
}

int64_t clamp_extract_char_limit(int64_t char_limit) {
    if (char_limit < k_min_extract_char_limit) {
        return k_min_extract_char_limit;
    }
    if (char_limit > k_max_extract_char_limit) {
        return k_max_extract_char_limit;
    }
    return char_limit;
}

// ---------------------------------------------------------------------------
// providers.py _resolve engine routing decision table
// ---------------------------------------------------------------------------

kimix::optional<kimix::string>
resolve_active_provider(kimix::string_view configured,
                        search_capability capability,
                        kimix::span<const web_provider_info> providers) {
    const auto capable = [capability](const web_provider_info &p) {
        return capability == search_capability::search ? p.supports_search
                                                       : p.supports_extract;
    };

    // Rule 1: explicit config wins (registered + capable, availability ignored).
    if (!configured.empty()) {
        for (const auto &p : providers) {
            if (p.name == configured && capable(p)) {
                return p.name;
            }
        }
    }

    // Rule 2: exactly one eligible provider.
    const web_provider_info *single = nullptr;
    size_t single_count = 0;
    for (const auto &p : providers) {
        if (capable(p) && p.available) {
            single = &p;
            ++single_count;
        }
    }
    if (single_count == 1u) {
        return single->name;
    }

    // Rule 3: legacy preference walk (providers.py _SEARCH_LEGACY_PREFERENCE /
    // _EXTRACT_LEGACY_PREFERENCE — the full reference order, not a shortened
    // subset: firecrawl/parallel/tavily/exa/searxng/brave-free/xai all come
    // before ddgs/local).
    static constexpr kimix::string_view k_search_pref[] = {
        "kimi", "firecrawl", "parallel", "tavily", "exa",
        "searxng", "brave-free", "xai", "ddgs", "local"};
    static constexpr kimix::string_view k_extract_pref[] = {
        "local", "kimi", "firecrawl", "parallel", "tavily", "exa"};
    const kimix::span<const kimix::string_view> pref =
        capability == search_capability::search
            ? kimix::span<const kimix::string_view>(k_search_pref, 10u)
            : kimix::span<const kimix::string_view>(k_extract_pref, 6u);
    for (const kimix::string_view name : pref) {
        for (const auto &p : providers) {
            if (p.name == name && capable(p) && p.available) {
                return p.name;
            }
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// content.py convert_base64_images_to_links (38-65)
// ---------------------------------------------------------------------------

kimix::string convert_base64_images_to_links(kimix::string_view text) {
    return ws_scan_base64(text, nullptr);
}

kimix::string convert_base64_images_to_links(
    kimix::string_view text, kimix::vector<kimix::string> &payloads) {
    return ws_scan_base64(text, &payloads);
}

// ---------------------------------------------------------------------------
// content.py store_full_text (75-104) — cache file naming + write
// ---------------------------------------------------------------------------

kimix::string make_cache_slug(kimix::string_view url) {
    const uint64_t h = XXH64(url.data(), url.size(), 0);
    char buf[16];
    for (int i = 15; i >= 0; --i) {
        const unsigned nib = static_cast<unsigned>((h >> (4u * static_cast<unsigned>(i))) & 0xFu);
        buf[15 - i] = nib < 10u ? static_cast<char>('0' + nib)
                                : static_cast<char>('a' + nib - 10u);
    }
    // xxhash.xxh64(...).hexdigest()[:10]
    return kimix::string(buf, 10u);
}

kimix::string make_cache_file_name(kimix::string_view url) {
    kimix::string host = ws_url_hostname(url);
    // content.py: host = (urlparse(url).hostname or "page").replace(":", "_")
    for (auto &c : host) {
        if (c == ':') {
            c = '_';
        }
    }
    // re.sub(r"[^A-Za-z0-9._-]", "-", host)[:60] — the reference substitutes per
    // *code point* and truncates to 60 code points (Python ``str`` slicing), so
    // a non-ASCII host becomes one '-' per character, not per UTF-8 byte
    // (reference: "k\u00f6ln.example" -> "k-ln.example", not "k--ln.example").
    kimix::string slug;
    slug.reserve(60u);
    {
        const char *it = host.data();
        const char *end = host.data() + host.size();
        size_t code_points = 0u;
        while (it < end && code_points < 60u) {
            const char *probe = it;
            const uint32_t cp = decode_code_point(probe, end);
            const size_t width = static_cast<size_t>(probe - it);
            bool ok = false;
            if (cp < 0x80u) {
                const char c = static_cast<char>(cp);
                ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
            }
            if (ok) {
                slug.push_back(static_cast<char>(cp));
            } else {
                slug.push_back('-');
            }
            ++code_points;
            it += width;
        }
    }
    // .strip("-")
    size_t b = 0;
    size_t e = slug.size();
    while (b < e && slug[b] == '-') {
        ++b;
    }
    while (e > b && slug[e - 1u] == '-') {
        --e;
    }
    kimix::string out;
    if (b == e) {
        out = "page";
    } else {
        out.assign(slug.data() + b, e - b);
    }
    out += "-";
    out += make_cache_slug(url);
    out += ".md";
    return out;
}

bool store_full_text(kimix::string_view url, kimix::string_view content,
                     kimix::string_view cache_dir, kimix::string &out_path) {
    out_path.clear();
    // No exceptions (kimix_enable_exception=false): the former
    // `try { ... } catch (...) { return false; }` best-effort guard (matching
    // Python's try/except -> None) is replaced by the non-throwing
    // kimix::path_from_narrow() helper. Both path components are validated
    // before use, so every later native/narrow conversion is representable and
    // cannot fail either; a cache dir or URL we cannot represent is simply not
    // stored.
    kimix::filesystem::path dir;
    if (!kimix::path_from_narrow(cache_dir, dir)) {
        return false;
    }
    kimix::filesystem::path file_name;
    if (!kimix::path_from_narrow(make_cache_file_name(url), file_name)) {
        return false;
    }
    std::error_code ec;
    kimix::filesystem::create_directories(dir, ec);
    const kimix::filesystem::path file_path = dir / file_name;

    kimix::string stored;
    const size_t total_cp = utf8_code_point_count(content);
    if (total_cp > k_max_stored_text_chars) {
        stored.assign(
            content.data(),
            utf8_byte_offset_of_code_point(content, k_max_stored_text_chars));
        kimix::StringScratch ss;
        ss << "\n\n[... stored copy truncated at "
           << ws_thousands(static_cast<int64_t>(k_max_stored_text_chars))
           << " chars of " << ws_thousands(static_cast<int64_t>(total_cp))
           << "; re-extract a more specific URL for the rest ...]";
        stored += ss.string();
    } else {
        stored.assign(content.data(), content.size());
    }

    FILE *file = fopen(file_path.string().c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const size_t written = fwrite(stored.data(), 1u, stored.size(), file);
    fclose(file);
    if (written != stored.size()) {
        return false;
    }
    out_path = kimix::to_string(file_path);
    return true;
}

// ---------------------------------------------------------------------------
// content.py truncate_with_footer (107-163)
// ---------------------------------------------------------------------------

truncate_with_footer_result truncate_with_footer(
    kimix::string_view content, kimix::string_view url, int64_t char_limit,
    const store_full_text_fn &store) {
    truncate_with_footer_result r;
    const size_t total_cp = utf8_code_point_count(content);
    if (total_cp <= static_cast<size_t>(char_limit)) {
        r.text.assign(content.data(), content.size());
        r.was_truncated = false;
        return r;
    }
    r.was_truncated = true;

    // int(char_limit * 0.75); 0.75 is exact in binary so integer arithmetic
    // matches Python for the clamped range.
    const int64_t head_budget = char_limit * 3 / 4;
    const int64_t tail_budget = char_limit - head_budget;

    const size_t head_cp = static_cast<size_t>(head_budget);
    // Python `content[-tail_budget:]`; note content[-0:] == content[0:] (whole).
    const size_t tail_start_cp =
        (tail_budget <= 0) ? 0u : total_cp - static_cast<size_t>(tail_budget);

    kimix::string head(content.data(),
                       utf8_byte_offset_of_code_point(content, head_cp));
    const size_t tail_byte_begin =
        utf8_byte_offset_of_code_point(content, tail_start_cp);
    kimix::string tail(content.data() + tail_byte_begin,
                       content.size() - tail_byte_begin);

    // Snap the head cut back to the last newline (code-point index semantics:
    // Python head.rfind("\n") then head[:nl]).
    const size_t head_nl_byte = head.rfind('\n');
    if (head_nl_byte != kimix::string::npos) {
        const int64_t head_nl = static_cast<int64_t>(utf8_code_point_count(
            kimix::string_view(head).substr(0u, head_nl_byte)));
        if (head_nl * 2 > head_budget) {
            head.assign(head.data(), head_nl_byte);
        }
    }

    // Snap the tail cut forward to the next newline.
    const size_t tail_nl_byte = tail.find('\n');
    if (tail_nl_byte != kimix::string::npos) {
        const int64_t tail_nl = static_cast<int64_t>(utf8_code_point_count(
            kimix::string_view(tail).substr(0u, tail_nl_byte)));
        if (tail_nl * 2 < tail_budget) {
            tail.assign(tail.data() + tail_nl_byte + 1u,
                        tail.size() - tail_nl_byte - 1u);
        }
    }

    const int64_t total = static_cast<int64_t>(total_cp);
    const kimix::optional<kimix::string> stored_path = store(url, content);

    kimix::vector<kimix::string> footer_lines;
    footer_lines.emplace_back(); // leading empty line
    footer_lines.push_back(
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80 [TRUNCATED] "
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80");
    {
        kimix::StringScratch ss;
        ss << "Showing "
           << ws_thousands(static_cast<int64_t>(utf8_code_point_count(head)))
           << " chars (head) + "
           << ws_thousands(static_cast<int64_t>(utf8_code_point_count(tail)))
           << " chars (tail) of " << ws_thousands(total)
           << " total clean characters.";
        footer_lines.push_back(std::move(ss.string()));
    }
    if (stored_path) {
        kimix::StringScratch ss;
        ss << "Full text saved to: " << *stored_path;
        footer_lines.push_back(std::move(ss.string()));
        kimix::StringScratch ss2;
        ss2 << "To read the omitted middle: read_file path=\"" << *stored_path
            << "\" offset=" << static_cast<unsigned long long>(
                   ws_count_newlines(head) + 2u)
            << " limit=200  (the file is the complete page; raise/lower "
               "offset to page through it).";
        footer_lines.push_back(std::move(ss2.string()));
    } else {
        footer_lines.push_back(
            "Full text could not be stored; re-run web_extract on a more "
            "specific URL for the complete page.");
    }
    footer_lines.push_back(
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80");

    kimix::string model_text;
    model_text.reserve(head.size() + tail.size() + 256u);
    model_text += head;
    // "\n\n[... middle omitted — see footer ...]\n\n" (U+2014 em dash)
    model_text += "\n\n[... middle omitted \xE2\x80\x94 see footer ...]\n\n";
    model_text += tail;
    model_text += "\n";
    for (size_t i = 0; i < footer_lines.size(); ++i) {
        if (i > 0u) {
            model_text += "\n";
        }
        model_text += footer_lines[i];
    }
    r.text = std::move(model_text);
    return r;
}

// ---------------------------------------------------------------------------
// search.py SearchWeb.__call__ rendering (112-125) — pure renderer
// ---------------------------------------------------------------------------

build_search_output_result
build_search_output(kimix::span<const web_item> items,
                    const build_search_output_options &opts) {
    build_search_output_result r;

    // The reference renders through ToolResultBuilder (see the emulation
    // above); the cap is measured in code points, not bytes.
    ws_tool_result_builder builder;
    builder.max_chars = opts.max_output_chars;

    if (opts.summary && !opts.summary->empty()) {
        kimix::string summary = *opts.summary;
        summary += "\n\n";
        builder.write(summary);
    }

    kimix::unordered_set<kimix::string_view> seen;
    bool first = true;
    for (const auto &it : items) {
        if (opts.dedup_urls) {
            // Extension (OFF by default): search.py renders every item.
            const kimix::string_view url(it.url.data(), it.url.size());
            if (!seen.insert(url).second) {
                ++r.omitted_items;
                continue;
            }
        }
        if (!first) {
            builder.write("---\n\n");
        }
        first = false;
        const size_t before = builder.n_chars;
        // "Title: <title>\nDate: <date>\nURL: <url>\nSummary: <desc>\n\n"
        kimix::string block;
        block.reserve(it.title.size() + it.date.size() + it.url.size() +
                      it.snippet.size() + 32u);
        block += "Title: ";
        block += it.title;
        block += "\nDate: ";
        block += it.date;
        block += "\nURL: ";
        block += it.url;
        block += "\nSummary: ";
        block += it.snippet;
        block += "\n\n";
        builder.write(block);
        // search.py prints the content block whenever the item carries one —
        // there is no include_content gate in the renderer (the flag only asks
        // the provider for content in the first place).
        if (!it.content.empty()) {
            kimix::string_view content(it.content.data(), it.content.size());
            if (opts.max_content_chars > 0u) {
                content = content.substr(
                    0u, utf8_byte_offset_of_code_point(content,
                                                       opts.max_content_chars));
            }
            kimix::string content_block;
            content_block.reserve(content.size() + 2u);
            content_block.append(content.data(), content.size());
            content_block += "\n\n";
            builder.write(content_block);
        }
        if (builder.n_chars == before) {
            ++r.omitted_items; // cap was already full: nothing of this item fit
        }
    }

    r.truncated = builder.truncated;
    r.text = std::move(builder.out);
    return r;
}

// ---------------------------------------------------------------------------
// Tool class wrapper
// ---------------------------------------------------------------------------

namespace {

kimix::string ws_object_string(const ToolParams *obj, kimix::string_view key,
                               kimix::string_view default_value = {}) {
    if (obj == nullptr) {
        return kimix::string(default_value);
    }
    const auto *el = obj->get(key);
    if (el != nullptr && el->is_string()) {
        return el->as_string();
    }
    return kimix::string(default_value);
}

bool ws_object_bool(const ToolParams *obj, kimix::string_view key,
                    bool default_value) {
    if (obj == nullptr) {
        return default_value;
    }
    const auto *el = obj->get(key);
    if (el != nullptr && el->is_bool()) {
        return el->as_bool();
    }
    return default_value;
}

int64_t ws_object_int64(const ToolParams *obj, kimix::string_view key,
                        int64_t default_value) {
    if (obj == nullptr) {
        return default_value;
    }
    const auto *el = obj->get(key);
    if (el != nullptr && el->is_int()) {
        return el->as_int();
    }
    if (el != nullptr && el->is_uint()) {
        return static_cast<int64_t>(el->as_uint());
    }
    return default_value;
}

web_item ws_parse_web_item(const ToolParams *obj) {
    web_item item;
    if (obj == nullptr) {
        return item;
    }
    item.site_name = ws_object_string(obj, "site_name");
    item.title = ws_object_string(obj, "title");
    item.url = ws_object_string(obj, "url");
    // search.py renders the provider's "description" value; `snippet` is this
    // project's name for the same field (SearchResult.snippet), so accept both.
    item.snippet = ws_object_string(obj, "snippet");
    if (item.snippet.empty()) {
        item.snippet = ws_object_string(obj, "description");
    }
    item.content = ws_object_string(obj, "content");
    item.date = ws_object_string(obj, "date");
    item.icon = ws_object_string(obj, "icon");
    item.mime = ws_object_string(obj, "mime");
    return item;
}

} // namespace

WebSearch::WebSearch(kimix::builtin_tools::Session *session)
    : kimix::builtin_tools::Tool(session) {}

static const kimix::builtin_tools::param_alias k_web_search_aliases[] = {
    {"items", "results items_list search_results"},
    {"summary", "answer summary_text abstract"},
    {"include_content", "content include_full_content full_content with_content"},
    {"max_content_chars", "max_content_length content_max_chars"},
    {"max_output_chars", "max_chars output_chars max_output_size output_limit"},
    {"dedup_urls", "dedup deduplicate_urls unique_urls"},
    {"query", "q search search_query query_string"},
    {"limit", "max_results num_results result_count top_k"},
};

void WebSearch::operator()(kimix::builtin_tools::ToolParams const *parameters) {
    // Fuzzy alias matching (tool.h): wrong-but-reasonable argument names
    // ("command" for "cmd") are accepted; the canonical name always wins.
    const kimix::builtin_tools::ToolParams k_resolved =
        kimix::builtin_tools::ToolParams::with_aliases(parameters, k_web_search_aliases);
    if (parameters != nullptr) {
        parameters = &k_resolved;
    }
    using namespace kimix::builtin_tools;

    _last_result.clear();
    ToolParams result;

    auto set_error = [&result](kimix::string_view message) {
        result.values["ok"] = ValueElement::make_bool(false);
        result.values["status"] = ValueElement::make_string("invalid_input");
        result.values["error"] =
            ValueElement::make_string(kimix::string(message));
    };

    if (parameters == nullptr) {
        set_error("missing parameters");
        result.serialize(_last_result);
        return;
    }

    const auto *items_el = parameters->get("items");
    if (items_el == nullptr || !items_el->is_array()) {
        set_error("missing or invalid 'items' array");
        result.serialize(_last_result);
        return;
    }

    kimix::vector<web_item> items;
    items.reserve(items_el->as_array().size());
    for (const auto &el : items_el->as_array()) {
        if (!el.is_object()) {
            set_error("every item in 'items' must be an object");
            result.serialize(_last_result);
            return;
        }
        items.push_back(ws_parse_web_item(el.as_object()));
    }

    build_search_output_options opts;
    // `include_content` is accepted for callers that mirror search.py's Params,
    // but it does not gate the rendered content: the reference renderer prints
    // any non-empty item content (the flag only asks the *provider* for it).
    const auto *summary_el = parameters->get("summary");
    if (summary_el != nullptr && summary_el->is_string()) {
        opts.summary = summary_el->as_string();
    }
    const int64_t max_content_chars =
        ws_object_int64(parameters, "max_content_chars", 0);
    opts.max_content_chars =
        (max_content_chars > 0) ? static_cast<size_t>(max_content_chars) : 0u;
    opts.dedup_urls = ws_object_bool(parameters, "dedup_urls", false);
    const int64_t max_output_chars = ws_object_int64(
        parameters, "max_output_chars",
        static_cast<int64_t>(k_tool_result_max_chars));
    // 0 (or a negative value) restores the reference ToolResultBuilder cap.
    opts.max_output_chars =
        (max_output_chars > 0) ? static_cast<size_t>(max_output_chars)
                               : k_tool_result_max_chars;

    const auto r = build_search_output(items, opts);

    result.values["ok"] = ValueElement::make_bool(true);
    result.values["status"] = ValueElement::make_string("ok");
    result.values["text"] = ValueElement::make_string(r.text);
    result.values["truncated"] = ValueElement::make_bool(r.truncated);
    result.values["omitted_items"] =
        ValueElement::make_int(static_cast<int64_t>(r.omitted_items));
    result.serialize(_last_result);
}

} // namespace web_search
} // namespace builtin_tools
} // namespace kimix
