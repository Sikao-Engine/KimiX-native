// Test for src/runtime/index/ngram_tokenizer.h (plan 004).
// This test covers:
// - normalize: ASCII lowercasing, non-ASCII passthrough (kernel contract)
// - detect_n: empty/ASCII/CJK-dense/mixed thresholds, default_n handling
// - tokenize: overlapping n-grams over code points, len<n single-token rule,
//   empty input, n=1..5, CJK multi-byte text
// - is_cjk_cp: all 16 retrieval.py ranges + non-CJK code points

#include "ut/ut.hpp"
#include "bench_util.h"
#include <runtime/index/ngram_tokenizer.h>
#include <runtime/common/utf8.h>

#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::index;

// UTF-8 literal helpers (same as test_token_count.cpp).
static kimix::string utf8_of(const std::initializer_list<uint32_t>& cps) {
    kimix::string out;
    for (uint32_t cp : cps) {
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
    return out;
}

static kimix::vector<kimix::string> to_strings(const kimix::vector<kimix::string_view>& views) {
    kimix::vector<kimix::string> out;
    out.reserve(views.size());
    for (auto v : views) {
        out.emplace_back(v);
    }
    return out;
}

// ---- benchmark helpers ----
// Deterministic pseudo-random stream (xorshift; same shape as the other
// native test files).
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<uint32_t>(state);
    }
    uint32_t bounded(uint32_t n) { return n == 0 ? 0 : next() % n; }
};

// Append one code point as UTF-8 (matches utf8_of encoding; 1..4 bytes).
static void append_cp(kimix::string& s, uint32_t cp) {
    if (cp < 0x80) {
        s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// ~2 KB document: `ascii_chars` lowercase letters + `cjk_cps` CJK ideographs
// (3-byte U+4E00..U+9FFF). Interleaves so both fast and slow paths run.
static kimix::string make_mixed_doc(Rng& rng, size_t ascii_chars, size_t cjk_cps) {
    kimix::string s;
    s.reserve(ascii_chars + cjk_cps * 3);
    size_t a = 0, c = 0;
    const size_t total = ascii_chars + cjk_cps;
    for (size_t i = 0; i < total; ++i) {
        if ((i % 3) == 0 && c < cjk_cps) {
            append_cp(s, 0x4E00u + (rng.next() % 4096u));
            ++c;
        } else if (a < ascii_chars) {
            s.push_back(static_cast<char>('a' + rng.bounded(26)));
            ++a;
        } else {
            append_cp(s, 0x4E00u + (rng.next() % 4096u));
            ++c;
        }
    }
    return s;
}

// Pure-ASCII ~2 KB document (lowercase letters + digits).
static kimix::string make_ascii_doc(Rng& rng, size_t chars) {
    kimix::string s;
    s.reserve(chars);
    for (size_t i = 0; i < chars; ++i) {
        const uint32_t r = rng.bounded(36);
        s.push_back(static_cast<char>(r < 26 ? 'a' + r : '0' + (r - 26)));
    }
    return s;
}

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "is_cjk_cp_ranges"_test = [] {
        // Spot checks across the 16 retrieval.py ranges (boundary + interior).
        expect(is_cjk_cp(0x4E00u));
        expect(is_cjk_cp(0x9FFFu));
        expect(is_cjk_cp(0x3400u));
        expect(is_cjk_cp(0x4DBFu));
        expect(is_cjk_cp(0x20000u));
        expect(is_cjk_cp(0x2EBEFu));
        expect(is_cjk_cp(0xAC00u));
        expect(is_cjk_cp(0xD7AFu));
        expect(is_cjk_cp(0x3040u));
        expect(is_cjk_cp(0x309Fu));
        expect(is_cjk_cp(0x30A0u));
        expect(is_cjk_cp(0x30FFu));
        expect(is_cjk_cp(0xF900u));
        expect(is_cjk_cp(0xFAFFu));
        expect(is_cjk_cp(0x2F800u));
        expect(is_cjk_cp(0x2FA1Fu));
        expect(is_cjk_cp(0x30000u));
        expect(is_cjk_cp(0x3134Fu));
        expect(is_cjk_cp(0x31350u));
        expect(is_cjk_cp(0x323AFu));
        expect(is_cjk_cp(0x2EBF0u));
        expect(is_cjk_cp(0x2EE5Fu));
        expect(is_cjk_cp(0x1100u));
        expect(is_cjk_cp(0x11FFu));
        expect(is_cjk_cp(0xA960u));
        expect(is_cjk_cp(0xA97Fu));
        expect(is_cjk_cp(0xD7B0u));
        expect(is_cjk_cp(0xD7FFu));
        expect(is_cjk_cp(0x31C0u));
        expect(is_cjk_cp(0x31EFu));
        expect(is_cjk_cp(0x3200u));
        expect(is_cjk_cp(0x32FFu));
        // Non-CJK: ASCII, Latin, PUA, fullwidth forms (NOT in this list —
        // 0xFF00-0xFFEF belongs to plan 001's text::is_cjk_cp, not this one).
        expect(!is_cjk_cp('A'));
        expect(!is_cjk_cp(0xE9u));        // é
        expect(!is_cjk_cp(0xE000u));      // PUA
        expect(!is_cjk_cp(0xFF01u));      // fullwidth !
        expect(!is_cjk_cp(0x1F600u));     // emoji
        expect(!is_cjk_cp(0x4E00u - 1));
        expect(!is_cjk_cp(0x9FFFu + 1));
        expect(!is_cjk_cp(0x2EE5Fu + 1));
    };

    "normalize_ascii"_test = [] {
        NgramTokenizer tok;
        expect(eq(tok.normalize("MiXeD Case 123"), kimix::string("mixed case 123")));
        expect(eq(tok.normalize("ALL_CAPS"), kimix::string("all_caps")));
        expect(eq(tok.normalize("already lower"), kimix::string("already lower")));
        expect(eq(tok.normalize(""), kimix::string("")));
        expect(eq(tok.normalize("123!@#"), kimix::string("123!@#")));
    };

    "normalize_non_ascii_passthrough"_test = [] {
        // Kernel contract: ASCII bytes are lowercased; non-ASCII bytes are
        // returned unchanged (the shim composes .lower() + NFKC on top).
        NgramTokenizer tok;
        const kimix::string s = "H\xc3\x89LLO"; // "HÉLLO" — É untouched
        expect(eq(tok.normalize(s), kimix::string("h\xc3\x89llo")));
    };

    "detect_n_basic"_test = [] {
        NgramTokenizer tok2; // default 2
        expect(eq(tok2.detect_n(""), 2u));
        expect(eq(tok2.detect_n("hello world"), 3u));          // ASCII -> 3
        expect(eq(tok2.detect_n("hello world, this is code"), 3u));
        // CJK-dense -> 2
        {
            const kimix::string s = utf8_of({0x4F60, 0x597D, 0x4E16, 0x754C}); // 你好世界
            expect(eq(tok2.detect_n(s), 2u));
        }
        NgramTokenizer tok4(4); // default 4
        expect(eq(tok4.detect_n("hello world"), 4u));          // ASCII -> max(n,3)
        expect(eq(tok4.detect_n(""), 4u));
        {
            const kimix::string s = utf8_of({0x4F60, 0x597D, 0x4E16, 0x754C});
            expect(eq(tok4.detect_n(s), 2u)); // CJK-dense still 2
        }
    };

    "detect_n_threshold_boundary"_test = [] {
        // threshold = len * 3 // 10 (code points). 10 chars, 3 CJK:
        // 10*3//10 = 3; cjk 3 is NOT > 3 -> no early 2 -> ASCII branch gives 3.
        NgramTokenizer tok2;
        {
            kimix::string s = "aaaaaaa"; // 7 ASCII
            s += utf8_of({0x4E00, 0x4E01, 0x4E02}); // 3 CJK -> 10 cps total
            expect(eq(tok2.detect_n(s), 3u));
        }
        // 4 CJK of 10 -> 4 > 3 -> 2.
        {
            kimix::string s = "aaaaaa"; // 6 ASCII
            s += utf8_of({0x4E00, 0x4E01, 0x4E02, 0x4E03}); // 4 CJK -> 10 cps
            expect(eq(tok2.detect_n(s), 2u));
        }
        // 2 CJK of 10 (threshold 3): not CJK-dense, not ASCII -> 3.
        {
            kimix::string s = "aaaaaaaa";
            s += utf8_of({0x4E00, 0x4E01});
            expect(eq(tok2.detect_n(s), 3u));
        }
    };

    "tokenize_ascii"_test = [] {
        NgramTokenizer tok;
        kimix::vector<kimix::string_view> out;
        tok.tokenize("hello", 2, out);
        const auto grams = to_strings(out);
        expect(eq(grams.size(), 4u));
        expect(eq(grams[0], kimix::string("he")));
        expect(eq(grams[1], kimix::string("el")));
        expect(eq(grams[2], kimix::string("ll")));
        expect(eq(grams[3], kimix::string("lo")));

        out.clear();
        tok.tokenize("hello", 3, out);
        expect(eq(to_strings(out).size(), 3u));

        out.clear();
        tok.tokenize("hello", 1, out);
        expect(eq(to_strings(out).size(), 5u));

        out.clear();
        tok.tokenize("hello", 5, out);
        expect(eq(to_strings(out).size(), 1u));
        expect(eq(out[0], kimix::string_view("hello")));
    };

    "tokenize_len_lt_n_single_token"_test = [] {
        // Python: len(text) < n -> (text,) — the WHOLE text as one token.
        NgramTokenizer tok;
        kimix::vector<kimix::string_view> out;
        tok.tokenize("ab", 3, out);
        expect(eq(out.size(), 1u));
        expect(eq(out[0], kimix::string_view("ab")));
        // len == n -> normal n-grams: exactly one gram == whole text.
        out.clear();
        tok.tokenize("abc", 3, out);
        expect(eq(out.size(), 1u));
        expect(eq(out[0], kimix::string_view("abc")));
    };

    "tokenize_empty"_test = [] {
        NgramTokenizer tok;
        kimix::vector<kimix::string_view> out;
        tok.tokenize("", 2, out);
        expect(out.empty());
    };

    "tokenize_cjk_code_points"_test = [] {
        // "你好世界" (4 code points, 12 bytes) with n=2:
        // grams = 你+好, 好+世, 世+界 — code-point slicing, NOT byte slicing.
        NgramTokenizer tok;
        const kimix::string s = utf8_of({0x4F60, 0x597D, 0x4E16, 0x754C});
        kimix::vector<kimix::string_view> out;
        tok.tokenize(s, 2, out);
        expect(eq(out.size(), 3u));
        expect(eq(out[0], utf8_of({0x4F60, 0x597D})));
        expect(eq(out[1], utf8_of({0x597D, 0x4E16})));
        expect(eq(out[2], utf8_of({0x4E16, 0x754C})));
    };

    "tokenize_mixed_ascii_cjk"_test = [] {
        // "A你B" (3 code points, 1+3+1 bytes) with n=2:
        // grams = A+你, 你+B (byte offsets must align to code points).
        NgramTokenizer tok;
        const kimix::string s = kimix::string("A") + utf8_of({0x4F60}) + "B";
        kimix::vector<kimix::string_view> out;
        tok.tokenize(s, 2, out);
        expect(eq(out.size(), 2u));
        expect(eq(out[0], kimix::string("A") + utf8_of({0x4F60})));
        expect(eq(out[1], utf8_of({0x4F60}) + kimix::string("B")));
    };

    "tokenize_len_lt_n_cjk"_test = [] {
        // 2 CJK code points with n=3 -> single token (whole text).
        NgramTokenizer tok;
        const kimix::string s = utf8_of({0x4F60, 0x597D});
        kimix::vector<kimix::string_view> out;
        tok.tokenize(s, 3, out);
        expect(eq(out.size(), 1u));
        expect(eq(out[0], s));
    };

    // ---------------------------------------------------------------------
    // Benchmarks: production-sized tokenization (retrieval over conversation
    // history: 10k docs x ~2 KB, mixed ASCII + CJK, and CJK-dense docs).
    // Each case validates per-doc gram counts against detect_n before any
    // number is reported, so a broken path is never timed.
    // ---------------------------------------------------------------------

    "bench_tokenize_mixed_2kb"_test = [] {
        constexpr size_t kDocs = 10000;
        constexpr size_t kAscii = 700; // 700 B
        constexpr size_t kCjk = 450;   // 1350 B -> ~2050 B/doc
        Rng rng(0xABC02Bu);
        NgramTokenizer tok;
        kimix::vector<kimix::string> docs;
        docs.reserve(kDocs);
        kimix::vector<uint32_t> expected_grams;
        expected_grams.reserve(kDocs);
        for (size_t i = 0; i < kDocs; ++i) {
            docs.push_back(make_mixed_doc(rng, kAscii, kCjk));
            const uint32_t n = tok.detect_n(docs.back());
            const size_t cps = kimix::runtime::common::utf8_code_point_count(docs.back());
            expected_grams.push_back(static_cast<uint32_t>(cps) - n + 1);
        }
        size_t seen = 0;
        size_t expected_total = 0;
        for (auto g : expected_grams) {
            expected_total += g;
        }
        kimix_bench::run("ngram/mixed_2kb_pipeline", [&] {
            size_t acc = 0;
            for (size_t i = 0; i < kDocs; ++i) {
                const kimix::string norm = tok.normalize(docs[i]);
                const uint32_t n = tok.detect_n(norm);
                kimix::vector<kimix::string_view> grams;
                tok.tokenize(norm, n, grams);
                expect(eq(grams.size(), expected_grams[i])) << "doc " << i;
                acc += grams.size();
            }
            seen = acc;
        }, kDocs, static_cast<double>(kDocs) * static_cast<double>(kAscii + kCjk * 3));
        expect(eq(seen, expected_total));
        kimix_bench::sink(seen);
    };

    "bench_tokenize_cjk_dense_2kb"_test = [] {
        constexpr size_t kDocs = 10000;
        constexpr size_t kAscii = 200; // 200 B
        constexpr size_t kCjk = 600;   // 1800 B -> 2000 B/doc, CJK-dense -> n=2
        Rng rng(0xC0DE5u);
        NgramTokenizer tok;
        kimix::vector<kimix::string> docs;
        kimix::vector<uint32_t> expected_grams;
        docs.reserve(kDocs);
        expected_grams.reserve(kDocs);
        for (size_t i = 0; i < kDocs; ++i) {
            docs.push_back(make_mixed_doc(rng, kAscii, kCjk));
            const uint32_t n = tok.detect_n(docs.back());
            expect(eq(n, 2u)); // CJK-dense must pick bigrams
            const size_t cps = kimix::runtime::common::utf8_code_point_count(docs.back());
            expected_grams.push_back(static_cast<uint32_t>(cps) - n + 1);
        }
        size_t seen = 0;
        size_t expected_total = 0;
        for (auto g : expected_grams) {
            expected_total += g;
        }
        kimix_bench::run("ngram/cjk_dense_2kb_pipeline", [&] {
            size_t acc = 0;
            for (size_t i = 0; i < kDocs; ++i) {
                const kimix::string norm = tok.normalize(docs[i]);
                const uint32_t n = tok.detect_n(norm);
                kimix::vector<kimix::string_view> grams;
                tok.tokenize(norm, n, grams);
                expect(eq(grams.size(), expected_grams[i])) << "doc " << i;
                acc += grams.size();
            }
            seen = acc;
        }, kDocs, static_cast<double>(kDocs) * static_cast<double>(kAscii + kCjk * 3));
        expect(eq(seen, expected_total));
        kimix_bench::sink(seen);
    };

    "bench_tokenize_ascii_2kb"_test = [] {
        constexpr size_t kDocs = 10000;
        constexpr size_t kChars = 2048; // ~2 KB pure ASCII -> n=3 byte fast path
        Rng rng(0xA5C11u);
        NgramTokenizer tok;
        kimix::vector<kimix::string> docs;
        docs.reserve(kDocs);
        for (size_t i = 0; i < kDocs; ++i) {
            docs.push_back(make_ascii_doc(rng, kChars));
        }
        size_t seen = 0;
        kimix_bench::run("ngram/ascii_2kb_pipeline", [&] {
            size_t acc = 0;
            for (size_t i = 0; i < kDocs; ++i) {
                const kimix::string norm = tok.normalize(docs[i]);
                const uint32_t n = tok.detect_n(norm);
                expect(eq(n, 3u));
                kimix::vector<kimix::string_view> grams;
                tok.tokenize(norm, n, grams);
                expect(eq(grams.size(), kChars - n + 1)) << "doc " << i;
                acc += grams.size();
            }
            seen = acc;
        }, kDocs, static_cast<double>(kDocs) * static_cast<double>(kChars));
        expect(eq(seen, kDocs * (kChars - 3 + 1)));
        kimix_bench::sink(seen);
    };

    "bench_detect_n_cjk_dense"_test = [] {
        // detect_n alone on CJK-dense docs (the code-point walk + CJK scan).
        constexpr size_t kDocs = 10000;
        constexpr size_t kAscii = 200;
        constexpr size_t kCjk = 600;
        Rng rng(0xD37EC7u);
        NgramTokenizer tok;
        kimix::vector<kimix::string> docs;
        docs.reserve(kDocs);
        for (size_t i = 0; i < kDocs; ++i) {
            docs.push_back(make_mixed_doc(rng, kAscii, kCjk));
        }
        uint32_t seen = 0;
        kimix_bench::run("ngram/detect_n_cjk_dense", [&] {
            uint32_t acc = 0;
            for (const auto& d : docs) {
                const uint32_t n = tok.detect_n(d);
                expect(eq(n, 2u));
                acc += n;
            }
            seen = acc;
        }, kDocs, static_cast<double>(kDocs) * static_cast<double>(kAscii + kCjk * 3));
        expect(eq(seen, kDocs * 2u));
        kimix_bench::sink(seen);
    };
}
