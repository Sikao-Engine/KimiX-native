/*
 * ngram_tokenizer.cpp — implementation of NgramTokenizer (see header).
 */

#include <runtime/index/ngram_tokenizer.h>

#include <runtime/common/utf8.h>

namespace kimix {
namespace runtime {
namespace index {

// The 16 CJK ranges of retrieval.py::NgramTokenizer._is_cjk (lines 43-60).
// Stored sorted by (lo, hi) so is_cjk_cp can binary-search instead of a
// 16-entry linear scan on every code point of every CJK-dense document.
namespace {

struct cp_range {
    uint32_t lo;
    uint32_t hi;
};

constexpr cp_range kCjkRanges[] = {
    {0x1100, 0x11FF},   // Hangul Jamo
    {0x3040, 0x309F},   // Hiragana
    {0x30A0, 0x30FF},   // Katakana
    {0x31C0, 0x31EF},   // CJK Strokes
    {0x3200, 0x32FF},   // Enclosed CJK Letters and Months
    {0x3400, 0x4DBF},   // Extension A
    {0x4E00, 0x9FFF},   // CJK Unified Ideographs
    {0xA960, 0xA97F},   // Hangul Jamo Extended-A
    {0xAC00, 0xD7AF},   // Hangul Syllables
    {0xD7B0, 0xD7FF},   // Hangul Jamo Extended-B
    {0xF900, 0xFAFF},   // CJK Compatibility Ideographs
    {0x20000, 0x2EBEF}, // Extensions B-F
    {0x2EBF0, 0x2EE5F}, // Extension I
    {0x2F800, 0x2FA1F}, // CJK Compatibility Ideographs Supplement
    {0x30000, 0x3134F}, // Extension G
    {0x31350, 0x323AF}, // Extension H
};

} // namespace

bool is_cjk_cp(uint32_t cp) noexcept {
    // Binary search over the sorted range table (16 entries -> <= 4 probes;
    // the ranges are disjoint, so range membership is an interval decision).
    size_t lo = 0;
    size_t hi = sizeof(kCjkRanges) / sizeof(kCjkRanges[0]);
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const cp_range& r = kCjkRanges[mid];
        if (cp < r.lo) {
            hi = mid;
        } else if (cp > r.hi) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

NgramTokenizer::NgramTokenizer(uint32_t default_n) noexcept : _default_n(default_n) {}

kimix::string NgramTokenizer::normalize(kimix::string_view text) const {
    // ASCII fast path (also covers the whole input when pure ASCII).
    // Python's lower() maps 'A'-'Z' to 'a'-'z' identically, so for pure
    // ASCII input the kernel output IS the reference output.
    kimix::string out;
    out.reserve(text.size());
    for (char c : text) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc >= 'A' && uc <= 'Z') {
            out.push_back(static_cast<char>(uc + ('a' - 'A')));
        } else {
            out.push_back(c);
        }
    }
    return out;
}

uint32_t NgramTokenizer::detect_n(kimix::string_view normalized) const noexcept {
    // Mirrors _detect_n(text): empty -> self.n.
    if (normalized.empty()) {
        return _default_n;
    }
    const uint32_t floor_n = _default_n < 3 ? 3u : _default_n;
    // Fast path: ASCII text cannot contain CJK.
    if (common::is_ascii(normalized)) {
        return floor_n;
    }
    // threshold = len * 3 // 10 with len in CODE POINTS (Python str length).
    // Single UTF-8 pass counts code points and CJK code points together
    // (decode_cp's invalid-byte handling matches utf8_code_point_count, so the
    // counts are identical to the old two-pass version). The reference
    // early-exits once cjk > threshold with a threshold fixed from the total;
    // cjk_count only grows during the scan, so comparing the final count
    // against the fixed threshold yields the identical decision.
    const char* it = normalized.data();
    const char* end = it + normalized.size();
    size_t cp_total = 0;
    size_t cjk_count = 0;
    while (it < end) {
        const uint32_t cp = common::decode_cp(it, end);
        ++cp_total;
        if (is_cjk_cp(cp)) {
            ++cjk_count;
        }
    }
    return cjk_count > (cp_total * 3u) / 10u ? 2u : floor_n;
}

void NgramTokenizer::tokenize(kimix::string_view normalized_text, uint32_t n,
                              kimix::vector<kimix::string_view>& out) const {
    // Python: `if len(text) < n: return (text,)` — len in code points; the
    // caller has already normalized+stripped and handled the empty case.
    if (normalized_text.empty()) {
        return;
    }
    if (n == 0) {
        // Degenerate: every n-gram of size 0 would be empty. Python would
        // raise on `n=0` slicing, so treat as "no tokens" (documented).
        return;
    }

    // ASCII fast path: bytes == code points.
    if (common::is_ascii(normalized_text)) {
        const size_t len = normalized_text.size();
        if (len < n) {
            out.push_back(normalized_text);
            return;
        }
        const size_t count = len - n + 1;
        out.reserve(out.size() + count);
        for (size_t i = 0; i < count; ++i) {
            out.push_back(normalized_text.substr(i, n));
        }
        return;
    }

    // Non-ASCII: walk code points recording byte offsets, then slice.
    kimix::vector<uint32_t> offsets;
    offsets.reserve(normalized_text.size() / 2 + 2);
    const char* it = normalized_text.data();
    const char* end = it + normalized_text.size();
    offsets.push_back(0);
    while (it < end) {
        common::decode_cp(it, end);
        offsets.push_back(static_cast<uint32_t>(it - normalized_text.data()));
    }
    const size_t cp_count = offsets.size() - 1;
    if (cp_count < n) {
        out.push_back(normalized_text);
        return;
    }
    const size_t count = cp_count - n + 1;
    out.reserve(out.size() + count);
    for (size_t i = 0; i < count; ++i) {
        const size_t begin = offsets[i];
        const size_t end_off = offsets[i + n];
        out.push_back(kimix::string_view(normalized_text.data() + begin, end_off - begin));
    }
}

} // namespace index
} // namespace runtime
} // namespace kimix
