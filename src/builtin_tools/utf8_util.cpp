// utf8_util.cpp - Minimal UTF-8 helpers for src/builtin_tools (see utf8_util.h).

#include "builtin_tools/utf8_util.h"

#include <cstring>

namespace kimix::builtin_tools {

bool is_ascii(kimix::string_view bytes) noexcept {
    for (char c : bytes) {
        if ((static_cast<uint8_t>(c) & 0x80u) != 0u) {
            return false;
        }
    }
    return true;
}

uint32_t decode_code_point(const char *&it, const char *end) noexcept {
    if (it >= end) {
        return 0xFFFDu;
    }
    const auto b0 = static_cast<uint8_t>(*it);
    if (b0 < 0x80u) {
        ++it;
        return b0;
    }

    // Number of continuation bytes implied by the lead byte, plus the mask for
    // the payload bits. Invalid lead bytes (0x80-0xC1, 0xF5-0xFF) decode to
    // U+FFFD and consume one byte.
    size_t extra = 0;
    uint32_t cp = 0;
    if (b0 >= 0xC2u && b0 <= 0xDFu) {
        extra = 1;
        cp = b0 & 0x1Fu;
    } else if (b0 >= 0xE0u && b0 <= 0xEFu) {
        extra = 2;
        cp = b0 & 0x0Fu;
    } else if (b0 >= 0xF0u && b0 <= 0xF4u) {
        extra = 3;
        cp = b0 & 0x07u;
    } else {
        ++it;
        return 0xFFFDu;
    }

    const char *p = it + 1;
    const char *seq_end = it + 1 + extra;
    if (seq_end > end) {
        ++it;
        return 0xFFFDu;
    }
    for (size_t i = 0; i < extra; i++) {
        const auto b = static_cast<uint8_t>(p[i]);
        if ((b & 0xC0u) != 0x80u) {
            ++it;
            return 0xFFFDu;
        }
        cp = (cp << 6) | (b & 0x3Fu);
    }
    // Reject surrogates (EPA0-EFBF) and anything above U+10FFFF.
    if ((cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu) {
        ++it;
        return 0xFFFDu;
    }
    it = seq_end;
    return cp;
}

size_t utf8_code_point_count(kimix::string_view bytes) noexcept {
    if (is_ascii(bytes)) {
        return bytes.size();
    }
    const char *it = bytes.data();
    const char *end = it + bytes.size();
    size_t n = 0;
    while (it < end) {
        decode_code_point(it, end);
        n++;
    }
    return n;
}

size_t utf8_byte_offset_of_code_point(kimix::string_view bytes,
                                      size_t code_points) noexcept {
    if (code_points == 0) {
        return 0;
    }
    if (is_ascii(bytes)) {
        return code_points < bytes.size() ? code_points : bytes.size();
    }
    const char *it = bytes.data();
    const char *end = it + bytes.size();
    size_t seen = 0;
    while (it < end && seen < code_points) {
        decode_code_point(it, end);
        seen++;
    }
    return static_cast<size_t>(it - bytes.data());
}

size_t utf8_floor_boundary(kimix::string_view bytes, size_t byte_pos) noexcept {
    if (byte_pos >= bytes.size()) {
        return bytes.size();
    }
    // Walk back out of continuation bytes (10xxxxxx): the result is the start
    // of the sequence containing byte_pos, i.e. the largest code-point boundary
    // that is <= byte_pos.
    size_t p = byte_pos;
    while (p > 0 && (static_cast<uint8_t>(bytes[p]) & 0xC0u) == 0x80u) {
        p--;
    }
    return p;
}

bool utf8_validate(kimix::string_view bytes) noexcept {
    size_t bad = 0;
    kimix::string reason;
    return utf8_strict_error(bytes, bad, reason);
}

// Strict UTF-8 validation mirroring CPython's utf-8 codec: the exact
// `reason` strings ("invalid start byte" / "invalid continuation byte" /
// "unexpected end of data" / "surrogates not allowed") and the 0-based
// `bad_offset` of the first offending byte (the codec reports
// (start, end, reason) and the reference tools print `start`).
//
// Sequence tables (Python 3.12 `unicodeobject.h` semantics):
//   0x00-0x7F          1 byte
//   0xC2-0xDF          2 bytes, cont 0x80-0xBF
//   0xE0               3 bytes, cont 0xA0-0xBF then 0x80-0xBF
//   0xE1-0xEC          3 bytes, cont 0x80-0xBF, 0x80-0xBF
//   0xED               3 bytes, cont 0x80-0x9F then 0x80-0xBF (no surrogates)
//   0xEE-0xEF          3 bytes, cont 0x80-0xBF, 0x80-0xBF
//   0xF0               4 bytes, cont 0x90-0xBF, 0x80-0xBF, 0x80-0xBF
//   0xF1-0xF3          4 bytes, cont 0x80-0xBF x3
//   0xF4               4 bytes, cont 0x80-0x8F, 0x80-0xBF, 0x80-0xBF
bool utf8_strict_error(kimix::string_view bytes, size_t &bad_offset,
                       kimix::string &reason) noexcept {
    const auto *p = reinterpret_cast<const uint8_t *>(bytes.data());
    const size_t n = bytes.size();
    size_t i = 0;
    while (i < n) {
        const uint8_t b = p[i];
        if (b < 0x80u) {
            i++;
            continue;
        }

        size_t len = 0;
        uint8_t lo = 0x80u, hi = 0xBFu;
        if (b >= 0xC2u && b <= 0xDFu) {
            len = 2;
        } else if (b >= 0xE0u && b <= 0xEFu) {
            len = 3;
            if (b == 0xE0u) {
                lo = 0xA0u;
            } else if (b == 0xEDu) {
                hi = 0x9Fu;
            }
        } else if (b >= 0xF0u && b <= 0xF4u) {
            len = 4;
            if (b == 0xF0u) {
                lo = 0x90u;
            } else if (b == 0xF4u) {
                hi = 0x8Fu;
            }
        } else {
            // 0x80-0xC1 and 0xF5-0xFF can never start a sequence.
            bad_offset = i;
            reason = "invalid start byte";
            return false;
        }

        if (i + len > n) {
            bad_offset = i;
            reason = "unexpected end of data";
            return false;
        }
        // CPython reports the error range as (lead_index, lead_index + 1) for
        // a bad continuation byte (verified against `bytes.decode("utf-8")`:
        // b"//xed//xa0//x80" -> "invalid continuation byte" at start=0). Surrogate
        // sequences (ED A0..BF) fall out of the 0xED upper bound below and get
        // the same wording ("surrogates not allowed" is an *encoding*-side
        // error in CPython, never a decoding one).
        for (size_t k = 1; k < len; k++) {
            const uint8_t c = p[i + k];
            const uint8_t c_lo = (k == 1) ? lo : 0x80u;
            const uint8_t c_hi = (k == 1) ? hi : 0xBFu;
            if (c < c_lo || c > c_hi) {
                bad_offset = i;
                reason = "invalid continuation byte";
                return false;
            }
        }
        i += len;
    }
    return true;
}

} // namespace kimix::builtin_tools
