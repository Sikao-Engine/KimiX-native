/*
 * sanitize.cpp — implementation of the text sanitizer kernels.
 *
 * Compiled into runtime.dll by the recursive glob in src/ext/xmake.lua.
 */

#include <runtime/text/sanitize.h>

#include <runtime/common/utf8.h>

#include <cstring>

namespace kimix {
namespace runtime {
namespace text {

namespace {

// ---------------------------------------------------------------------------
// Code point predicates (exact mirrors of the Python strip passes)
// ---------------------------------------------------------------------------

// Step 2: U+D800–U+DFFF (lone surrogates — invalid Unicode scalars).
inline bool is_surrogate(uint32_t cp) noexcept {
    return cp >= 0xD800u && cp <= 0xDFFFu;
}

// Step 3: U+FDD0–U+FDEF and any cp whose low 16 bits are 0xFFFE/0xFFFF.
inline bool is_noncharacter(uint32_t cp) noexcept {
    if (cp >= 0xFDD0u && cp <= 0xFDEFu) {
        return true;
    }
    const uint32_t low = cp & 0xFFFFu;
    return low == 0xFFFEu || low == 0xFFFFu;
}

// Step 4: Private Use Area planes.
inline bool is_pua(uint32_t cp) noexcept {
    return (cp >= 0xE000u && cp <= 0xF8FFu)      // BMP PUA
        || (cp >= 0xF0000u && cp <= 0xFFFFDu)    // Plane 15 PUA
        || (cp >= 0x100000u && cp <= 0x10FFFDu); // Plane 16 PUA
}

// Step 6a: zero-width / invisible format characters removed by clean_text.
inline bool is_zero_width(uint32_t cp) noexcept {
    return cp == 0x200Bu   // ZWSP
        || cp == 0x200Cu   // ZWNJ
        || cp == 0x200Du   // ZWJ
        || cp == 0x2060u   // word joiner
        || cp == 0x00ADu   // soft hyphen
        || cp == 0xFEFFu   // BOM / ZWNBSP
        || cp == 0x200Eu   // LRM
        || cp == 0x200Fu   // RLM
        || (cp >= 0x202Au && cp <= 0x202Eu)      // LR/RL/PDF/LRE/RLE embeddings
        || (cp >= 0x2066u && cp <= 0x2069u);     // LRI/RLI/FSI/PDI isolates
}

// Step 6b: C0/C1 control characters. keep_newlines keeps \n (0x0A), \r (0x0D),
// \t (0x09) — exactly the Python regexes:
//   keep:    [\x00-\x08\x0b\x0c\x0e-\x1f\x7f-\x9f]
//   strip:   [\x00-\x1f\x7f-\x9f]
inline bool is_control(uint32_t cp, bool keep_newlines) noexcept {
    if (keep_newlines) {
        return cp <= 0x08u || cp == 0x0Bu || cp == 0x0Cu
            || (cp >= 0x0Eu && cp <= 0x1Fu)
            || (cp >= 0x7Fu && cp <= 0x9Fu);
    }
    return cp <= 0x1Fu || (cp >= 0x7Fu && cp <= 0x9Fu);
}

// Encode one code point as UTF-8 (1..4 bytes) into a raw buffer; returns the
// advanced write pointer. Used by the in-place dedupe pass.
inline char* encode_cp(char* w, uint32_t cp) {
    if (cp < 0x80u) {
        *w++ = static_cast<char>(cp);
    } else if (cp < 0x800u) {
        *w++ = static_cast<char>(0xC0u | (cp >> 6));
        *w++ = static_cast<char>(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000u) {
        *w++ = static_cast<char>(0xE0u | (cp >> 12));
        *w++ = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        *w++ = static_cast<char>(0x80u | (cp & 0x3Fu));
    } else {
        *w++ = static_cast<char>(0xF0u | (cp >> 18));
        *w++ = static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
        *w++ = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        *w++ = static_cast<char>(0x80u | (cp & 0x3Fu));
    }
    return w;
}

// Append one code point to the end of `out` (used by the decode-filter-encode
// passes; the buffer is pre-reserved so no reallocation occurs).
inline void append_cp(kimix::string& out, uint32_t cp) {
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

// ASCII-only control predicates (bytes are known < 0x80 inside the run), as
// constant bitmask lookups instead of a 5-branch chain per byte:
//   keep_newlines: drop 0x00-0x08, 0x0B, 0x0C, 0x0E-0x1F, 0x7F
//   strip:         drop 0x00-0x1F, 0x7F
inline bool ascii_ctl_keep(unsigned char c) noexcept {
    constexpr uint64_t kLo = 0xFFFFD9FFull;         // bytes 0x00-0x3F: bits 0-8, 11, 12, 14-31
    constexpr uint64_t kHi = 0x8000000000000000ull; // byte 0x7F
    return ((c & 0x40u) != 0u ? (kHi >> (c & 0x3Fu)) : (kLo >> c)) & 1u;
}

inline bool ascii_ctl_strip(unsigned char c) noexcept {
    constexpr uint64_t kLo = 0xFFFFFFFFull;         // bytes 0x00-0x1F
    constexpr uint64_t kHi = 0x8000000000000000ull; // byte 0x7F
    return ((c & 0x40u) != 0u ? (kHi >> (c & 0x3Fu)) : (kLo >> c)) & 1u;
}

// Copy the ASCII bytes of [it, end) to `out`, dropping C0/C1 controls per
// `keep_newlines` (the only sanitize classes that can occur below 0x80).
// Stops at the first non-ASCII byte. Returns the first byte not consumed.
// Single pass: run-end detection and control filtering are fused, so a
// control-free ASCII run costs one branch per byte plus one bulk append.
inline const char* append_ascii_run(kimix::string& out, const char* it,
                                    const char* end, bool keep_newlines) {
    const char* w = it;
    while (it < end) {
        const unsigned char c = static_cast<unsigned char>(*it);
        if (c >= 0x80u) {
            break;
        }
        const bool ctl = keep_newlines ? ascii_ctl_keep(c) : ascii_ctl_strip(c);
        if (ctl) {
            const size_t seg = static_cast<size_t>(it - w);
            if (seg > 0) {
                out.append(w, seg);
            }
            ++it;
            w = it;
        } else {
            ++it;
        }
    }
    const size_t tail = static_cast<size_t>(it - w);
    if (tail > 0) {
        out.append(w, tail);
    }
    return it;
}

kimix::string filter_pass(kimix::string_view utf8, bool keep_newlines) {
    kimix::string out;
    out.reserve(utf8.size()); // output never exceeds input
    const char* it = utf8.data();
    const char* end = it + utf8.size();
    while (it < end) {
        const unsigned char b = static_cast<unsigned char>(*it);
        if (b < 0x80u) {
            // ASCII fast path: bulk-copy runs of kept bytes instead of one
            // decode_cp + append_cp call pair per byte.
            it = append_ascii_run(out, it, end, keep_newlines);
            if (it >= end) {
                break;
            }
        }
        const uint32_t cp = common::decode_cp(it, end);
        if (is_surrogate(cp) || is_noncharacter(cp) || is_pua(cp) ||
            cp == 0xFFFDu || is_zero_width(cp) || is_control(cp, keep_newlines)) {
            continue;
        }
        append_cp(out, cp);
    }
    return out;
}

// Step 6d: strip() — remove leading/trailing Python-whitespace code points
// from an owned string in place (memmove + resize, no allocation).
static void strip_inplace(kimix::string& s) {
    const char* const begin = s.data();
    const char* const end = begin + s.size();
    const char* first = end;   // byte offset of the first non-space cp
    const char* last = begin;  // one past the last non-space cp
    const char* it = begin;
    while (it < end) {
        const char* start = it;
        const uint32_t cp = common::decode_cp(it, end);
        if (!is_unicode_space(cp)) {
            if (first == end) {
                first = start;
            }
            last = it;
        }
    }
    if (first == end) {
        s.clear(); // all whitespace
        return;
    }
    const size_t head = static_cast<size_t>(first - begin);
    const size_t keep = static_cast<size_t>(last - first);
    if (head != 0) {
        std::memmove(const_cast<char*>(begin), first, keep);
    }
    s.resize(keep);
}

// Step 6d + 7 merged into a single in-place pass over an owned string:
// strip leading/trailing Python-whitespace code points AND collapse runs of
// identical code points longer than max_repeat (max_repeat == 0 disables
// dedupe). The two transformations commute — strip only removes code points
// at the two ends, dedupe only collapses interior runs of identical cps — so
// one forward decode pass with a rewind over the final whitespace run is
// byte-exact vs. the reference (strip then dedupe). No allocation.
static void strip_dedupe_inplace(kimix::string& s, uint32_t max_repeat) {
    const char* const begin = s.data();
    const char* const end = begin + s.size();
    if (begin == end) {
        return;
    }

    // Skip leading whitespace code points; all-whitespace input empties out.
    const char* r = begin;
    {
        const char* it = begin;
        for (;;) {
            if (it >= end) {
                s.clear();
                return;
            }
            const char* start = it;
            const uint32_t cp = common::decode_cp(it, end);
            if (!is_unicode_space(cp)) {
                r = start; // first non-space code point
                break;
            }
        }
    }
    // Phase 2: dedupe over [r, end), writing compacted bytes at w (w never
    // overtakes the read position: the write position trails by design).
    const char* const out_base = r; // first output byte (after leading skip)
    char* w = const_cast<char*>(out_base);
    const bool dedupe_on = max_repeat != 0u;
    bool has_run = false;
    uint32_t run_cp = 0;
    size_t run_len = 0;
    size_t last_non_ws = 0; // output bytes after the last flushed non-space run
    bool last_run_is_ws = false;

    auto flush = [&]() {
        const size_t keep = dedupe_on && run_len > max_repeat ? max_repeat : run_len;
        const bool ws = is_unicode_space(run_cp);
        last_run_is_ws = ws;
        if (run_cp < 0x80u) {
            for (size_t k = 0; k < keep; ++k) {
                *w++ = static_cast<char>(run_cp);
            }
        } else {
            for (size_t k = 0; k < keep; ++k) {
                w = encode_cp(w, run_cp);
            }
        }
        if (!ws) {
            last_non_ws = static_cast<size_t>(w - out_base);
        }
    };

    while (r < end) {
        const unsigned char b = static_cast<unsigned char>(*r);
        uint32_t cp;
        if (b < 0x80u) {
            cp = static_cast<uint32_t>(b); // ASCII: byte == code point
            ++r;
        } else {
            cp = common::decode_cp(r, end);
        }
        if (has_run && cp == run_cp) {
            ++run_len;
            continue;
        }
        if (has_run) {
            flush();
        }
        run_cp = cp;
        run_len = 1;
        has_run = true;
    }
    if (has_run) {
        flush();
    }
    // The compacted output lives in [out_base, w); shift it down to the front
    // of the buffer when leading whitespace was skipped, then truncate.
    const size_t written = last_run_is_ws ? last_non_ws
                                          : static_cast<size_t>(w - out_base);
    if (out_base != begin) {
        std::memmove(const_cast<char*>(begin), out_base, written);
    }
    s.resize(written);
}

// Step 8: truncate to max_chars code points in place; reserve room for
// truncate_msg when len(msg) < max_chars (Python: text[:max_chars-len(msg)]
// + msg, applied only when truncation actually occurs). Walks at most
// max_chars + 1 code points — no full-buffer pass, no intermediate copy.
static void truncate_inplace(kimix::string& s, uint32_t max_chars,
                             kimix::string_view msg, bool* truncated) {
    if (max_chars == 0u) {
        return;
    }
    const bool has_msg = !msg.empty();
    size_t msg_cps = 0;
    if (has_msg) {
        msg_cps = common::utf8_code_point_count(msg);
    }
    const bool keep_msg = has_msg && msg_cps < max_chars;
    const size_t keep_cps = keep_msg ? static_cast<size_t>(max_chars) - msg_cps
                                     : static_cast<size_t>(max_chars);

    // Walk at most max_chars code points; reaching the end means the input
    // fits (total <= max_chars) and nothing is truncated.
    const char* const begin = s.data();
    const char* const end = begin + s.size();
    const char* it = begin;
    size_t n = 0;
    while (it < end && n < max_chars) {
        (void)common::decode_cp(it, end);
        ++n;
    }
    if (it >= end) {
        return;
    }
    if (truncated != nullptr) {
        *truncated = true;
    }
    if (keep_cps == max_chars) {
        s.resize(static_cast<size_t>(it - begin));
        return;
    }
    // Reserve room for msg: keep only keep_cps (< max_chars) code points
    // (guaranteed present since total > max_chars).
    it = begin;
    n = 0;
    while (n < keep_cps) {
        (void)common::decode_cp(it, end);
        ++n;
    }
    s.resize(static_cast<size_t>(it - begin));
    s.append(msg.data(), msg.size());
}

} // namespace

bool is_unicode_space(uint32_t cp) noexcept {
    return cp == 0x20u
        || (cp >= 0x09u && cp <= 0x0Du)
        || (cp >= 0x1Cu && cp <= 0x1Fu)
        || cp == 0x85u        // NEL
        || cp == 0xA0u        // NBSP
        || cp == 0x1680u      // Ogham space mark
        || (cp >= 0x2000u && cp <= 0x200Au)
        || cp == 0x2028u      // line separator
        || cp == 0x2029u      // paragraph separator
        || cp == 0x202Fu      // narrow NBSP
        || cp == 0x205Fu      // medium mathematical space
        || cp == 0x3000u;     // ideographic space
}

kimix::string sanitize_pre_nfc(kimix::string_view utf8) {
    return filter_pass(utf8, /*keep_newlines=*/true);
}

kimix::string sanitize_post_nfc(kimix::string_view utf8,
                                const sanitize_options& opts,
                                bool* truncated) {
    if (truncated != nullptr) {
        *truncated = false;
    }
    // Own the input once, then strip (6d) + dedupe (7) + truncate (8) all in
    // place: one allocation total, no full-buffer intermediate copies.
    kimix::string s(utf8);
    strip_dedupe_inplace(s, opts.max_repeat);
    truncate_inplace(s, opts.max_chars, opts.truncate_msg, truncated);
    return s;
}

kimix::string sanitize_for_tokenizer(kimix::string_view utf8,
                                     const sanitize_options& opts,
                                     bool* truncated) {
    if (truncated != nullptr) {
        *truncated = false;
    }
    // Full pipeline sans NFC: filter (2-5, 6a, 6b) writes once into an owned
    // buffer; strip + dedupe + truncate then operate in place — the entire
    // pipeline performs a single allocation and a single filter pass.
    kimix::string s = filter_pass(utf8, /*keep_newlines=*/true);
    strip_dedupe_inplace(s, opts.max_repeat);
    truncate_inplace(s, opts.max_chars, opts.truncate_msg, truncated);
    return s;
}

kimix::string clean_text(kimix::string_view utf8, bool keep_newlines) {
    // 6a zero-width + 6b controls + 6d strip (NFC applied by the shim hook).
    // NOTE: unlike sanitize_pre_nfc, clean_text does NOT remove surrogates /
    // noncharacters / PUA / U+FFFD — the Python clean_text only removes the
    // zero-width and control sets (verified against safety_check.py).
    kimix::string out;
    out.reserve(utf8.size()); // output never exceeds input
    const char* it = utf8.data();
    const char* end = it + utf8.size();
    while (it < end) {
        const unsigned char b = static_cast<unsigned char>(*it);
        if (b < 0x80u) {
            it = append_ascii_run(out, it, end, keep_newlines);
            if (it >= end) {
                break;
            }
        }
        const uint32_t cp = common::decode_cp(it, end);
        if (!is_zero_width(cp) && !is_control(cp, keep_newlines)) {
            append_cp(out, cp);
        }
    }
    strip_inplace(out);
    return out;
}

kimix::string strip_controls(kimix::string_view utf8, bool keep_newlines) {
    // 6b only — decode-filter-encode without the zero-width pass or strip.
    kimix::string out;
    out.reserve(utf8.size()); // output never exceeds input
    const char* it = utf8.data();
    const char* end = it + utf8.size();
    while (it < end) {
        const unsigned char b = static_cast<unsigned char>(*it);
        if (b < 0x80u) {
            it = append_ascii_run(out, it, end, keep_newlines);
            if (it >= end) {
                break;
            }
        }
        const uint32_t cp = common::decode_cp(it, end);
        if (!is_control(cp, keep_newlines)) {
            append_cp(out, cp);
        }
    }
    return out;
}

} // namespace text
} // namespace runtime
} // namespace kimix
