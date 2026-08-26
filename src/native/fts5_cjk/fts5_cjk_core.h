/*
 * fts5_cjk_core.h — cjk_unicode61 FTS5 tokenizer core (CJK character bigrams).
 *
 * Why: SQLite's unicode61 tokenizer treats a CJK run as ONE token
 * ("캘린더일본" indexes as a single 6-char token), so a 2-char Korean/Chinese/
 * Japanese query can never match inside it. The stock trigram tokenizer fixes
 * substring search but needs >=3 chars per query term — 2-char words (일본,
 * 구글, 우리, 中文, ...) fall through to a full-table LIKE scan.
 *
 * What: wrap unicode61. Every token it emits is re-examined; maximal CJK
 * runs inside the token are re-emitted as overlapping character BIGRAMS
 * (Lucene CJKAnalyzer semantics), non-CJK segments pass through unchanged.
 * A lone CJK char (run length 1) is emitted as a unigram. Because FTS5
 * turns consecutive tokens emitted from one query term into a phrase,
 * a query word like 캘린더 → [캘린][린더] gets exact substring semantics
 * with index-speed lookups, down to 2-char terms.
 *
 * This header is the pure, SQLite-free core (no RTTI, no exceptions, no
 * heap allocation). The loadable SQLite extension wrapper lives in
 * fts5_cjk.cpp; the Boost.UT test exercises this core directly.
 *
 * Ported from Hermes-CN-Core native/fts5_cjk/fts5_cjk.c (PR #65544,
 * contributed by Soju06) and adapted to kimix C++ conventions.
 */
#pragma once

#include <cstdint>

namespace kimix {
namespace native {
namespace fts5_cjk {

// FTS5 xToken-compatible sink. Returns SQLITE_OK (0) on success; any
// non-zero value aborts tokenization.
using TokenSink = int (*)(void* ctx, int tflags, const char* pToken,
                          int nToken, int iStart, int iEnd);

// True when `cp` is a CJK code point per the reference cjk_is_cjk ranges:
// Hangul syllables/Jamo (incl. ext A/B), CJK unified + extensions A..F,
// compatibility ideographs (+ supplement), Hiragana, Katakana (+ phonetic).
inline bool is_cjk_cp(uint32_t cp) noexcept {
    return (cp >= 0xAC00u && cp <= 0xD7A3u)   /* Hangul syllables */
        || (cp >= 0x1100u && cp <= 0x11FFu)   /* Hangul Jamo */
        || (cp >= 0x3130u && cp <= 0x318Fu)   /* Hangul compat Jamo */
        || (cp >= 0xA960u && cp <= 0xA97Fu)   /* Hangul Jamo ext-A */
        || (cp >= 0xD7B0u && cp <= 0xD7FFu)   /* Hangul Jamo ext-B */
        || (cp >= 0x4E00u && cp <= 0x9FFFu)   /* CJK unified ideographs */
        || (cp >= 0x3400u && cp <= 0x4DBFu)   /* CJK ext A */
        || (cp >= 0xF900u && cp <= 0xFAFFu)   /* CJK compat ideographs */
        || (cp >= 0x20000u && cp <= 0x2FA1Fu) /* CJK ext B..F, compat sup */
        || (cp >= 0x3040u && cp <= 0x309Fu)   /* Hiragana */
        || (cp >= 0x30A0u && cp <= 0x30FFu)   /* Katakana */
        || (cp >= 0x31F0u && cp <= 0x31FFu);  /* Katakana phonetic ext */
}

// Decode one UTF-8 code point at p (n bytes available). Returns the number
// of bytes consumed (>= 1) and stores the code point in *out. Invalid bytes
// decode as themselves so segmentation still terminates.
inline int utf8_decode(const unsigned char* p, int n, uint32_t* out) noexcept {
    const uint32_t c = p[0];
    if (c < 0x80u) {
        *out = c;
        return 1;
    }
    if ((c & 0xE0u) == 0xC0u && n >= 2) {
        *out = ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu);
        return 2;
    }
    if ((c & 0xF0u) == 0xE0u && n >= 3) {
        *out = ((c & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
        return 3;
    }
    if ((c & 0xF8u) == 0xF0u && n >= 4) {
        *out = ((c & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12)
             | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
        return 4;
    }
    *out = c;
    return 1;
}

// Re-emit one unicode61 token, splitting CJK runs into overlapping bigrams.
//
// Offsets: unicode61 reports [iStart, iEnd) into the ORIGINAL text. For CJK
// bytes unicode61's folding is the identity, and ASCII case folding preserves
// byte length, so mapping sub-token offsets by byte position within the token
// is exact for CJK and correct-length for ASCII. For rare length-changing
// folds (accented latin) the highlight offsets can drift by a few bytes inside
// that token; matching is unaffected. Every emitted offset is clamped to
// [iStart, iEnd).
inline int emit_cjk_bigrams(void* ctx, TokenSink xToken, int tflags,
                            const char* pToken, int nToken,
                            int iStart, int iEnd) noexcept {
    const unsigned char* z = reinterpret_cast<const unsigned char*>(pToken);
    int i = 0;
    int rc = 0;

    // Fast path: no CJK anywhere -> pass through untouched.
    bool hasCjk = false;
    while (i < nToken) {
        uint32_t cp;
        i += utf8_decode(z + i, nToken - i, &cp);
        if (is_cjk_cp(cp)) {
            hasCjk = true;
            break;
        }
    }
    if (!hasCjk) {
        return xToken(ctx, tflags, pToken, nToken, iStart, iEnd);
    }

#define CJK_CLAMP_END(v) ((iStart + (v)) > iEnd ? iEnd : (iStart + (v)))

    i = 0;
    while (i < nToken && rc == 0) {
        uint32_t cp;
        const int segStart = i;
        const int len = utf8_decode(z + i, nToken - i, &cp);
        if (!is_cjk_cp(cp)) {
            // Non-CJK segment: extend to the next CJK char (or end).
            i += len;
            while (i < nToken) {
                const int l2 = utf8_decode(z + i, nToken - i, &cp);
                if (is_cjk_cp(cp)) {
                    break;
                }
                i += l2;
            }
            rc = xToken(ctx, tflags, pToken + segStart, i - segStart,
                        CJK_CLAMP_END(segStart), CJK_CLAMP_END(i));
        } else {
            // CJK run: collect char byte-boundaries, emit bigrams.
            int bounds[3]; // rolling window: start, mid, end
            bounds[0] = segStart;
            bounds[1] = segStart + len;
            i += len;
            int nChars = 1;
            while (i < nToken) {
                const int l2 = utf8_decode(z + i, nToken - i, &cp);
                if (!is_cjk_cp(cp)) {
                    break;
                }
                i += l2;
                nChars++;
                if (nChars >= 2) {
                    bounds[2] = i;
                    rc = xToken(ctx, tflags,
                                pToken + bounds[0], bounds[2] - bounds[0],
                                CJK_CLAMP_END(bounds[0]), CJK_CLAMP_END(bounds[2]));
                    if (rc != 0) {
                        break;
                    }
                    bounds[0] = bounds[1];
                    bounds[1] = bounds[2];
                }
            }
            if (rc == 0 && nChars == 1) {
                // Lone CJK char: emit as unigram.
                rc = xToken(ctx, tflags,
                            pToken + segStart, bounds[1] - segStart,
                            CJK_CLAMP_END(segStart), CJK_CLAMP_END(bounds[1]));
            }
        }
    }

#undef CJK_CLAMP_END
    return rc;
}

} // namespace fts5_cjk
} // namespace native
} // namespace kimix
