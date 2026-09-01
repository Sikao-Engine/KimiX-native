/*
 * text_util.h - Shared UTF-8 text helpers (kimix::runtime::common).
 *
 * Inline-only helpers used by the soul / json / tools kernels (payload
 * builder, prune scanner, export builder, JSON pretty printer). Marked
 * `inline` so the unity build (which merges several runtime .cpp files into
 * one TU per batch) never hits redefinition collisions -- each TU gets its
 * own copy, exactly like the vendored single-header libraries.
 *
 * Pure C++ kernel helpers: never include Python / pybind11 headers.
 */

#pragma once

#include <core/kimix_core.h>
#include <runtime/common/utf8.h>

#include <cstring>

namespace kimix {
namespace runtime {
namespace common {

// True for the code points Python's str.isspace() accepts (Unicode
// White_Space plus U+001C..U+001F).
inline bool py_isspace_cp(uint32_t cp) noexcept {
    if (cp >= 0x0009 && cp <= 0x000D) {
        return true; // \t \n \v \f \r
    }
    if (cp >= 0x001C && cp <= 0x001F) {
        return true;
    }
    switch (cp) {
    case 0x0020: // space
    case 0x0085:
    case 0x00A0:
    case 0x1680:
    case 0x2028:
    case 0x2029:
    case 0x202F:
    case 0x205F:
    case 0x3000:
        return true;
    default:
        return cp >= 0x2000 && cp <= 0x200A;
    }
}

// Encode one code point as UTF-8 and append it to `out`.
inline void append_utf8(kimix::string& out, uint32_t cp) noexcept {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

namespace text_util_detail {

// Skip a run of pure ASCII spaces (byte 0x20) 8 bytes at a time. Every 0x20
// byte decodes to the space code point, which py_isspace_cp accepts, so this
// is exactly equivalent to per-code-point decoding for these bytes; any
// non-0x20 byte falls back to decode_cp.
inline void skip_py_ws_front(const char*& it, const char* end) noexcept {
    while (static_cast<size_t>(end - it) >= sizeof(uint64_t)) {
        uint64_t w;
        std::memcpy(&w, it, sizeof(w));
        if (w != 0x2020202020202020ull) {
            break;
        }
        it += sizeof(uint64_t);
    }
}

} // namespace text_util_detail

// Trim Python-whitespace code points from both ends of `s`. Invalid UTF-8
// bytes are treated as non-whitespace (kept), mirroring decode_cp semantics
// for malformed input.
inline kimix::string_view trim_py_ws(kimix::string_view s) noexcept {
    const char* begin = s.data();
    const char* end = s.data() + s.size();
    const char* it = begin;
    for (;;) {
        text_util_detail::skip_py_ws_front(it, end);
        if (it >= end) {
            break;
        }
        const char* before = it;
        const uint32_t cp = decode_cp(it, end);
        if (!py_isspace_cp(cp)) {
            it = before;
            break;
        }
    }
    const char* new_begin = it;
    it = end;
    while (it > new_begin) {
        const char* before = it;
        --it;
        // Walk back to the lead byte.
        while (it > new_begin && (static_cast<unsigned char>(*it) & 0xC0) == 0x80) {
            --it;
        }
        const char* cp_start = it;
        const char* walk = cp_start;
        const uint32_t cp = decode_cp(walk, end);
        if (walk != before || !py_isspace_cp(cp)) {
            it = before; // not (a trailing whitespace cp) -- restore
            break;
        }
    }
    return kimix::string_view(new_begin, static_cast<size_t>(it - new_begin));
}

// Python str.lstrip(): drop leading whitespace code points only.
inline kimix::string_view ltrim_py_ws(kimix::string_view s) noexcept {
    const char* it = s.data();
    const char* end = s.data() + s.size();
    for (;;) {
        text_util_detail::skip_py_ws_front(it, end);
        if (it >= end) {
            break;
        }
        const char* before = it;
        const uint32_t cp = decode_cp(it, end);
        if (!py_isspace_cp(cp)) {
            it = before;
            break;
        }
    }
    return kimix::string_view(it, static_cast<size_t>(end - it));
}

// True when `s` is empty or contains only Python-whitespace code points
// (equivalent to Python `s.strip() == ""`).
inline bool empty_after_trim(kimix::string_view s) noexcept {
    return trim_py_ws(s).empty();
}

// True when the trimmed text starts with `prefix` (Python
// `text.strip().startswith(prefix)`).
inline bool trimmed_starts_with(kimix::string_view s, kimix::string_view prefix) noexcept {
    return trim_py_ws(s).substr(0, prefix.size()) == prefix;
}

// Append `s` with A-Z lowercased (bytes outside A-Z pass through unchanged).
inline void append_lower_ascii(kimix::string& out, kimix::string_view s) noexcept {
    out.reserve(out.size() + s.size());
    for (char c : s) {
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
    }
}

// Python kimi_cli/utils/string.py::shorten(text, width, placeholder="..."):
// normalize whitespace (" ".join(text.split())), then truncate preferring a
// word boundary near the cut point, appending the placeholder. All length
// arithmetic is on CODE POINTS (Python str length).
inline kimix::string shorten_utf8(kimix::string_view text, size_t width) noexcept {
    // Normalize: drop whitespace runs, join with single ASCII spaces.
    kimix::string norm;
    norm.reserve(text.size()); // hint only; normalization can only shrink
    bool pending_space = false;
    const char* it = text.data();
    const char* end = text.data() + text.size();
    while (it < end) {
        const uint32_t cp = decode_cp(it, end);
        if (py_isspace_cp(cp)) {
            pending_space = !norm.empty();
        } else {
            if (pending_space) {
                norm.push_back(' ');
                pending_space = false;
            }
            append_utf8(norm, cp);
        }
    }
    const size_t cp_len = utf8_code_point_count(norm);
    if (cp_len <= width) {
        return norm;
    }
    const size_t placeholder_len = 1; // U+2026 is one code point
    size_t cut = (width >= placeholder_len) ? (width - placeholder_len) : 0;
    if (cut == 0) {
        // text[:width] by code points
        kimix::string out;
        size_t count = 0;
        const char* p = norm.data();
        const char* pe = norm.data() + norm.size();
        while (p < pe && count < width) {
            const char* start = p;
            (void)decode_cp(p, pe);
            out.append(start, static_cast<size_t>(p - start));
            ++count;
        }
        return out;
    }
    // text.rfind(" ", 0, cut + 1): last space code point at position <= cut.
    size_t space_pos = SIZE_MAX;
    {
        size_t pos = 0;
        const char* p = norm.data();
        const char* pe = norm.data() + norm.size();
        while (p < pe && pos <= cut) {
            const uint32_t cp = decode_cp(p, pe);
            if (cp == ' ' && pos > 0) {
                space_pos = pos;
            }
            ++pos;
        }
    }
    if (space_pos != SIZE_MAX) {
        cut = space_pos;
    }
    // text[:cut].rstrip() + placeholder (rstrip is a no-op here because the
    // normalized text has no trailing whitespace at the cut unless cut lands
    // on a space; keep it for exactness).
    kimix::string out;
    {
        size_t count = 0;
        const char* p = norm.data();
        const char* pe = norm.data() + norm.size();
        while (p < pe && count < cut) {
            const char* start = p;
            (void)decode_cp(p, pe);
            out.append(start, static_cast<size_t>(p - start));
            ++count;
        }
    }
    while (!out.empty()) {
        // strip trailing Python-whitespace code points
        const char* p = out.data() + out.size();
        const char* begin = out.data();
        while (p > begin && (static_cast<unsigned char>(p[-1]) & 0xC0) == 0x80) {
            --p;
        }
        if (p <= begin) {
            break;
        }
        const char* cp_start = p - 1;
        while (cp_start > begin &&
               (static_cast<unsigned char>(cp_start[-1]) & 0xC0) == 0x80) {
            --cp_start;
        }
        const char* walk = cp_start;
        const uint32_t cp = decode_cp(walk, p);
        if (walk != p || !py_isspace_cp(cp)) {
            break;
        }
        out.resize(static_cast<size_t>(cp_start - begin));
    }
    append_utf8(out, 0x2026); // "..."
    return out;
}

// Append `s` JSON-escaped with orjson / json.dumps(ensure_ascii=False)
// semantics: " \\ \b \f \n \r \t short escapes, other control bytes (<0x20)
// as \u00XX lowercase hex, everything else raw (UTF-8 passes through).
inline void append_json_escaped(kimix::string& out, kimix::string_view s) noexcept {
    // Kept as a plain inlined per-byte switch: measurements showed word-level
    // scanning (SWAR) wins on sparse-escape strings but regresses escape-dense
    // strings and short JSON keys by 10-20% (see .kimix_cache/bench_reports/
    // common.md), which json_pretty and the export builder depend on.
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0x0F]);
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
}

} // namespace common
} // namespace runtime
} // namespace kimix
