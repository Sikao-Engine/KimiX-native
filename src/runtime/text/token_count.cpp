/*
 * token_count.cpp — implementation of the heuristic token-count kernels.
 *
 * Compiled into runtime.dll by the recursive glob in src/ext/xmake.lua
 * (add_files("../runtime/**.cpp") minus "../runtime/py/**.cpp").
 */

#include <runtime/text/token_count.h>

#include <runtime/common/utf8.h>

namespace kimix {
namespace runtime {
namespace text {

namespace {

// Extended scan used by the estimate: code points + ASCII count + CJK count
// in a single pass over the bytes (plan: "One pass over the UTF-8 bytes").
struct scan_full_stats {
    uint32_t code_points = 0;
    uint32_t ascii = 0;
    uint32_t cjk = 0;
};

scan_full_stats scan_full(kimix::string_view bytes) noexcept {
    scan_full_stats st;
    const char* it = bytes.data();
    const char* end = it + bytes.size();

    while (it < end) {
        if (static_cast<unsigned char>(*it) < 0x80u) {
            // ASCII fast path: count a whole run in one pass.
            const char* run = it;
            while (run < end && static_cast<unsigned char>(*run) < 0x80u) {
                ++run;
            }
            const size_t n = static_cast<size_t>(run - it);
            st.code_points += static_cast<uint32_t>(n);
            st.ascii += static_cast<uint32_t>(n);
            it = run;
            continue;
        }

        // One code point (decode_cp advances past the sequence, or past a
        // single invalid byte which counts as one code point).
        const uint32_t cp = common::decode_cp(it, end);
        ++st.code_points;
        if (is_cjk_cp(cp)) {
            ++st.cjk;
        }
    }
    return st;
}

} // namespace

count_stats scan_utf8(kimix::string_view bytes) noexcept {
    count_stats st;
    const char* it = bytes.data();
    const char* end = it + bytes.size();

    while (it < end) {
        if (static_cast<unsigned char>(*it) < 0x80u) {
            const char* run = it;
            while (run < end && static_cast<unsigned char>(*run) < 0x80u) {
                ++run;
            }
            const size_t n = static_cast<size_t>(run - it);
            st.code_points += static_cast<uint32_t>(n);
            st.ascii += static_cast<uint32_t>(n);
            it = run;
            continue;
        }
        (void)common::decode_cp(it, end);
        ++st.code_points;
    }
    return st;
}

bool is_cjk_cp(uint32_t cp) noexcept {
    return (cp >= 0x4E00u && cp <= 0x9FFFu)      // \u4e00-\u9fff
        || (cp >= 0x3400u && cp <= 0x4DBFu)      // \u3400-\u4dbf
        || (cp >= 0x20000u && cp <= 0x2EBEFu)    // \U00020000-\U0002ebef
        || (cp >= 0xAC00u && cp <= 0xD7AFu)      // \uac00-\ud7af
        || (cp >= 0x3040u && cp <= 0x309Fu)      // \u3040-\u309f
        || (cp >= 0x30A0u && cp <= 0x30FFu)      // \u30a0-\u30ff
        || (cp >= 0xFF00u && cp <= 0xFFEFu);     // \uff00-\uffef
}

bool is_cjk_text(kimix::string_view utf8, double threshold) noexcept {
    if (utf8.empty()) {
        return false;
    }
    const scan_full_stats st = scan_full(utf8);
    // cjk_count / len(text) > threshold — strict, IEEE-754 double division.
    return static_cast<double>(st.cjk) / static_cast<double>(st.code_points) > threshold;
}

int estimate_chars_tokens(kimix::string_view utf8) noexcept {
    if (utf8.empty()) {
        return 0;
    }
    const scan_full_stats st = scan_full(utf8);
    const uint32_t total = st.code_points;

    const double ascii_ratio = static_cast<double>(st.ascii) / static_cast<double>(total);
    if (ascii_ratio > 0.95) {
        // max(1, total // 4) — floor division.
        const uint32_t v = total / 4u;
        return v > 1u ? static_cast<int>(v) : 1;
    }
    if (static_cast<double>(st.cjk) / static_cast<double>(total) > 0.15) {
        // max(1, total // 3) — floor division.
        const uint32_t v = total / 3u;
        return v > 1u ? static_cast<int>(v) : 1;
    }
    // max(1, int(total / 3.5)) — float division then truncation.
    const int64_t v = static_cast<int64_t>(static_cast<double>(total) / 3.5);
    return v > 1 ? static_cast<int>(v) : 1;
}

} // namespace text
} // namespace runtime
} // namespace kimix
