/*
 * utf8.cpp — UTF-8 decoding utilities implementation.
 *
 * Compiled into runtime.dll by the recursive glob in src/ext/xmake.lua
 * (add_files("../runtime/**.cpp") minus "../runtime/py/**.cpp").
 *
 * Decoding follows RFC 3629 / WHATWG-compatible lenient behavior for the
 * kernel contract: malformed input never crashes and never loops; each
 * offending byte yields U+FFFD (decode_cp) or counts as one code point
 * (utf8_code_point_count).
 */

#include <runtime/common/utf8.h>

namespace kimix {
namespace runtime {
namespace common {

bool is_ascii(kimix::string_view bytes) noexcept {
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t n = bytes.size();
    for (size_t i = 0; i < n; ++i) {
        if (p[i] >= 0x80u) {
            return false;
        }
    }
    return true;
}

uint32_t decode_cp(const char*& it, const char* end) noexcept {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(it);
    const unsigned char* e = reinterpret_cast<const unsigned char*>(end);
    const unsigned char b0 = p[0];

    // 1-byte ASCII.
    if (b0 < 0x80u) {
        it += 1;
        return static_cast<uint32_t>(b0);
    }

    // Determine the sequence length from the lead byte.
    int len = 0;
    uint32_t cp = 0;
    uint32_t min_cp = 0;
    if ((b0 & 0xE0u) == 0xC0u) {           // 2-byte sequence
        len = 2;
        cp = b0 & 0x1Fu;
        min_cp = 0x80u;
    } else if ((b0 & 0xF0u) == 0xE0u) {    // 3-byte sequence
        len = 3;
        cp = b0 & 0x0Fu;
        min_cp = 0x800u;
    } else if ((b0 & 0xF8u) == 0xF0u) {    // 4-byte sequence
        len = 4;
        cp = b0 & 0x07u;
        min_cp = 0x10000u;
    } else {
        // Invalid lead byte (lone continuation or 0xF8..0xFF).
        it += 1;
        return 0xFFFDu;
    }

    // Truncated sequence (fewer continuation bytes than required remain).
    if (static_cast<size_t>(e - p) < static_cast<size_t>(len)) {
        it += 1;
        return 0xFFFDu;
    }

    // Validate continuation bytes.
    for (int i = 1; i < len; ++i) {
        const unsigned char b = p[i];
        if ((b & 0xC0u) != 0x80u) {
            it += 1;
            return 0xFFFDu;
        }
        cp = (cp << 6) | (b & 0x3Fu);
    }

    // Overlong encoding or beyond U+10FFFF is invalid.
    if (cp < min_cp || cp > 0x10FFFFu) {
        it += 1;
        return 0xFFFDu;
    }

    it += len;
    return cp;
}

size_t utf8_code_point_count(kimix::string_view bytes) noexcept {
    const char* it = bytes.data();
    const char* end = it + bytes.size();
    size_t count = 0;

    while (it < end) {
        if (static_cast<unsigned char>(*it) < 0x80u) {
            // ASCII fast path: count a whole run in one pass.
            const char* run = it;
            while (run < end && static_cast<unsigned char>(*run) < 0x80u) {
                ++run;
            }
            count += static_cast<size_t>(run - it);
            it = run;
            continue;
        }

        // One code point (decode_cp advances past the sequence, or past a
        // single invalid byte). The count is exactly the number of code
        // points a decode_cp walk would produce.
        (void)decode_cp(it, end);
        ++count;
    }
    return count;
}

size_t utf8_byte_length(uint32_t cp) noexcept {
    if (cp < 0x80u) {
        return 1;
    }
    if (cp < 0x800u) {
        return 2;
    }
    if (cp < 0x10000u) {
        return 3;
    }
    return 4;
}

} // namespace common
} // namespace runtime
} // namespace kimix
