/*
 * sanitize.h — UTF-8 text sanitizer (kimix::runtime::text).
 *
 * Plan 002: native port of `kimi_cli/safety_check.py::sanitize_for_tokenizer`
 * and `clean_text`. Called for every tool output and every Text/Think part,
 * every step — the second highest-frequency kernel in the report.
 *
 * Pipeline (verified against safety_check.py — implement exactly):
 *   1. Coerce to str                                   (shim, Python side)
 *   2. Remove surrogates        U+D800–U+DFFF
 *   3. Remove noncharacters     U+FDD0–U+FDEF, (cp & 0xFFFF) in (0xFFFE, 0xFFFF)
 *   4. Remove PUA               U+E000–U+F8FF, U+F0000–U+FFFFD, U+100000–U+10FFFD
 *   5. Remove replacement chars U+FFFD
 *   6. clean_text(keep_newlines=True):
 *      a. remove zero-width / format: \u200b \u200c \u200d \u2060 \u00ad
 *         \ufeff \u200e \u200f \u202a-\u202e \u2066-\u2069
 *      b. remove C0/C1 controls except \n \r \t
 *      c. NFC normalize                                 (SHIM HOOK, not C++)
 *      d. strip() — Python's Unicode whitespace set
 *   7. Dedupe repeats: collapse runs longer than max_repeat to max_repeat
 *   8. Truncate to max_chars (code points); truncate_msg kept only when
 *      len(msg) < max_chars; reservation: text[:max_chars-len(msg)] + msg
 *
 * NFC (step 6c) is NOT implemented in C++ (v1): the shim splits the pipeline
 * into sanitize_pre_nfc (steps 2–5, 6a, 6b) and sanitize_post_nfc (6d, 7, 8)
 * and runs `unicodedata.normalize("NFC", …)` between them only when
 * non-ASCII survived (ASCII fast path: NFC is identity, single native call
 * `sanitize_for_tokenizer`). There is NO final strip after truncation — the
 * reference implementation's "9. Final strip" comment is not followed by any
 * strip call; the only strip happens inside clean_text (step 6d).
 *
 * strip() whitespace set (Python str.strip()/isspace, verified empirically):
 *   ASCII: 0x09-0x0D, 0x1C-0x1F, 0x20
 *   Non-ASCII: 0x85, 0xA0, 0x1680, 0x2000-0x200A, 0x2028, 0x2029,
 *              0x202F, 0x205F, 0x3000
 *
 * Pure C++ kernel: no Python includes; GIL is released in the binding layer.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace text {

// Options for sanitize_for_tokenizer / sanitize_post_nfc.
struct sanitize_options {
    uint32_t max_chars = 0;        // 0 = truncation disabled
    uint32_t max_repeat = 100;     // max consecutive identical code points; 0 = disabled
    kimix::string_view truncate_msg; // UTF-8 suffix appended when truncation occurs
};

// Steps 2–5 + 6a + 6b of the pipeline (everything before NFC). The shim runs
// NFC on the result when non-ASCII survived, then calls sanitize_post_nfc.
KIMIX_RUNTIME_API kimix::string sanitize_pre_nfc(kimix::string_view utf8);

// Steps 6d (strip) + 7 (dedupe repeats) + 8 (truncate). `truncated` (optional)
// is set to true iff truncation actually occurred (code points > max_chars).
KIMIX_RUNTIME_API kimix::string sanitize_post_nfc(kimix::string_view utf8,
                                                  const sanitize_options& opts,
                                                  bool* truncated = nullptr);

// Full pipeline sans NFC: sanitize_pre_nfc + sanitize_post_nfc. Exact for
// pure-ASCII input (NFC is identity); the shim uses the pre/post split for
// inputs that contain non-ASCII. Returns sanitized UTF-8.
KIMIX_RUNTIME_API kimix::string sanitize_for_tokenizer(kimix::string_view utf8,
                                                       const sanitize_options& opts,
                                                       bool* truncated = nullptr);

// Mirror of clean_text(text, keep_newlines=True): steps 6a + 6b + 6d (strip).
// NFC is applied by the shim after this call when non-ASCII survived.
KIMIX_RUNTIME_API kimix::string clean_text(kimix::string_view utf8,
                                           bool keep_newlines = true);

// Step 6b only: remove C0/C1 control characters (keep \n \r \t when
// keep_newlines is true).
KIMIX_RUNTIME_API kimix::string strip_controls(kimix::string_view utf8,
                                               bool keep_newlines);

// True when `cp` is whitespace per Python's str.strip() set (see file comment).
KIMIX_RUNTIME_API bool is_unicode_space(uint32_t cp) noexcept;

} // namespace text
} // namespace runtime
} // namespace kimix
