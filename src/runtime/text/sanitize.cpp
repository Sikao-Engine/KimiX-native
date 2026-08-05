/*
 * sanitize.cpp — implementation of the text sanitizer kernels.
 *
 * Compiled into runtime.dll by the recursive glob in src/ext/xmake.lua.
 */

#include <runtime/text/sanitize.h>

#include <runtime/common/utf8.h>

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

// Encode one code point as UTF-8 (1..4 bytes).
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

// Steps 2–5, 6a, 6b: decode-filter-encode in one pass. `keep_newlines`
// controls the C0/C1 mask (sanitize always uses keep_newlines=true).
kimix::string filter_pass(kimix::string_view utf8, bool keep_newlines) {
    kimix::string out;
    const char* it = utf8.data();
    const char* end = it + utf8.size();
    while (it < end) {
        const uint32_t cp = common::decode_cp(it, end);
        if (is_surrogate(cp) || is_noncharacter(cp) || is_pua(cp) ||
            cp == 0xFFFDu || is_zero_width(cp) || is_control(cp, keep_newlines)) {
            continue;
        }
        append_cp(out, cp);
    }
    return out;
}

// Step 6d: strip() — remove leading/trailing Python-whitespace code points.
kimix::string strip_spaces(kimix::string_view utf8) {
    const char* begin = utf8.data();
    const char* end = begin + utf8.size();
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
        return kimix::string(); // all whitespace
    }
    return kimix::string(first, static_cast<size_t>(last - first));
}

// Step 7: dedupe repeats. Python `re.sub(r"(.)\1{max_repeat,}", m.group(1)*max_repeat)`
// collapses runs longer than max_repeat keeping the FIRST max_repeat copies;
// runs of length <= max_repeat pass through unchanged. max_repeat == 0 disables.
kimix::string dedupe_repeats(kimix::string_view utf8, uint32_t max_repeat) {
    if (max_repeat == 0u) {
        return kimix::string(utf8);
    }
    kimix::string out;
    const char* it = utf8.data();
    const char* end = it + utf8.size();
    bool has_run = false;
    uint32_t run_cp = 0;
    size_t run_len = 0; // in code points
    while (it < end) {
        const uint32_t cp = common::decode_cp(it, end);
        if (has_run && cp == run_cp) {
            ++run_len;
            continue;
        }
        // Flush previous run (keep min(run_len, max_repeat) copies).
        const size_t keep = run_len < max_repeat ? run_len : max_repeat;
        for (size_t k = 0; k < keep; ++k) {
            append_cp(out, run_cp);
        }
        run_cp = cp;
        run_len = 1;
        has_run = true;
    }
    const size_t keep = run_len < max_repeat ? run_len : max_repeat;
    for (size_t k = 0; k < keep; ++k) {
        append_cp(out, run_cp);
    }
    return out;
}

// Step 8: truncate to max_chars code points; reserve room for truncate_msg
// when len(msg) < max_chars (Python: text[:max_chars-len(msg)] + msg).
kimix::string truncate_cps(kimix::string_view utf8, uint32_t max_chars,
                           kimix::string_view msg, bool* truncated) {
    if (max_chars == 0u) {
        return kimix::string(utf8);
    }
    const size_t total = common::utf8_code_point_count(utf8);
    if (total <= max_chars) {
        return kimix::string(utf8);
    }
    if (truncated != nullptr) {
        *truncated = true;
    }
    const size_t msg_cps = common::utf8_code_point_count(msg);
    const bool keep_msg = !msg.empty() && msg_cps < max_chars;
    const size_t keep_cps = keep_msg ? max_chars - msg_cps : max_chars;

    const char* it = utf8.data();
    const char* end = it + utf8.size();
    size_t n = 0;
    while (it < end && n < keep_cps) {
        (void)common::decode_cp(it, end);
        ++n;
    }
    kimix::string out(utf8.data(), static_cast<size_t>(it - utf8.data()));
    if (keep_msg) {
        out.append(msg.data(), msg.size());
    }
    return out;
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
    // 6d strip
    kimix::string s = strip_spaces(utf8);
    // 7 dedupe repeats
    s = dedupe_repeats(s, opts.max_repeat);
    // 8 truncate (no final strip — reference has none)
    return truncate_cps(s, opts.max_chars, opts.truncate_msg, truncated);
}

kimix::string sanitize_for_tokenizer(kimix::string_view utf8,
                                     const sanitize_options& opts,
                                     bool* truncated) {
    if (truncated != nullptr) {
        *truncated = false;
    }
    kimix::string pre = sanitize_pre_nfc(utf8);
    return sanitize_post_nfc(pre, opts, truncated);
}

kimix::string clean_text(kimix::string_view utf8, bool keep_newlines) {
    // 6a zero-width + 6b controls + 6d strip (NFC applied by the shim hook).
    // NOTE: unlike sanitize_pre_nfc, clean_text does NOT remove surrogates /
    // noncharacters / PUA / U+FFFD — the Python clean_text only removes the
    // zero-width and control sets (verified against safety_check.py).
    kimix::string out;
    const char* it = utf8.data();
    const char* end = it + utf8.size();
    while (it < end) {
        const uint32_t cp = common::decode_cp(it, end);
        if (!is_zero_width(cp) && !is_control(cp, keep_newlines)) {
            append_cp(out, cp);
        }
    }
    return strip_spaces(out);
}

kimix::string strip_controls(kimix::string_view utf8, bool keep_newlines) {
    // 6b only — decode-filter-encode without the zero-width pass or strip.
    kimix::string out;
    const char* it = utf8.data();
    const char* end = it + utf8.size();
    while (it < end) {
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
