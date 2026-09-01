// Test for src/runtime/text/token_count.h (heuristic UTF-8 token counting).
// This test covers:
// - scan_utf8: code-point + ASCII counts (ASCII runs, CJK, 4-byte, empty)
// - is_cjk_cp: all 7 ranges + non-CJK code points
// - is_cjk_text: threshold boundary (strict >), empty input
// - estimate_chars_tokens: empty, //4 branch, //3 branch, /3.5 branch and its
//   rounding boundaries (total 7..11), golden vectors from kimi-agent tests

#include "ut/ut.hpp"
#include "bench_util.h"
#include <runtime/text/token_count.h>
#include <runtime/common/utf8.h>

#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::text;

// UTF-8 literal helpers: build std::string from code points.
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

static kimix::string repeat(kimix::string s, size_t n) {
    kimix::string out;
    out.reserve(s.size() * n);
    for (size_t i = 0; i < n; ++i) {
        out += s;
    }
    return out;
}

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "scan_utf8_basics"_test = [] {
        // empty
        {
            const auto st = scan_utf8(kimix::string_view());
            expect(eq(st.code_points, 0u));
            expect(eq(st.ascii, 0u));
        }
        // pure ASCII
        {
            const auto st = scan_utf8(kimix::string_view("hello world", 11));
            expect(eq(st.code_points, 11u));
            expect(eq(st.ascii, 11u));
        }
        // mixed: "héllo中" = h é l l o 中 (6 cps; ascii = h l l o = 4)
        {
            const kimix::string s = utf8_of({'h', 0xE9, 'l', 'l', 'o', 0x4E2D});
            const auto st = scan_utf8(kimix::string_view(s));
            expect(eq(st.code_points, 6u));
            expect(eq(st.ascii, 4u));
        }
        // 4-byte CJK Ext B + emoji: U+20000, U+1F600 (2 cps, 0 ascii)
        {
            const kimix::string s = utf8_of({0x20000, 0x1F600});
            const auto st = scan_utf8(kimix::string_view(s));
            expect(eq(st.code_points, 2u));
            expect(eq(st.ascii, 0u));
        }
    };

    "is_cjk_cp_ranges"_test = [] {
        // Each of the 7 ranges: boundary + interior.
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
        expect(is_cjk_cp(0xFF00u));
        expect(is_cjk_cp(0xFFEFu));
        // Non-CJK: ASCII, emoji, Latin, PUA.
        expect(!is_cjk_cp('A'));
        expect(!is_cjk_cp(0x1F600u));
        expect(!is_cjk_cp(0xE9u));      // é
        expect(!is_cjk_cp(0xE000u));    // PUA
        expect(!is_cjk_cp(0x3000u));    // ideographic space (NOT in CJK ranges)
        expect(!is_cjk_cp(0x9FFFu + 1));
        expect(!is_cjk_cp(0x2EBEFu + 1));
    };

    "is_cjk_text_threshold"_test = [] {
        expect(!is_cjk_text(kimix::string_view()));
        expect(!is_cjk_text(kimix::string_view("Hello world", 11)));
        // "Hello world 世" — 1 CJK / 13 chars ≈ 0.077 → false
        {
            const kimix::string s = "Hello world " + utf8_of({0x4E16});
            expect(eq(s.size(), size_t(15))); // 12 ASCII bytes + 3-byte CJK
            expect(!is_cjk_text(kimix::string_view(s)));
        }
        // "Hello世界" — 2 CJK / 7 = 0.2857 > 0.15 → true
        {
            const kimix::string s = "Hello" + utf8_of({0x4E16, 0x754C});
            expect(is_cjk_text(kimix::string_view(s)));
        }
        // pure CJK
        {
            const kimix::string s = utf8_of({0x4F60, 0x597D, 0x4E16, 0x754C});
            expect(is_cjk_text(kimix::string_view(s)));
        }
        // Korean + Japanese
        {
            const kimix::string ko = utf8_of({0xC548, 0xB155, 0xD558, 0xC138, 0xC694});
            expect(is_cjk_text(kimix::string_view(ko)));
            const kimix::string ja = utf8_of({0x3053, 0x3093, 0x306B, 0x3061, 0x306F});
            expect(is_cjk_text(kimix::string_view(ja)));
        }
        // exactly at 0.15 → NOT > → false: 3 CJK of 20 chars (3/20 == 0.15)
        {
            const kimix::string s = repeat("a", 17) + utf8_of({0x4E00, 0x4E01, 0x4E02});
            expect(eq(s.size(), size_t(26))); // 17 + 3*3 bytes
            expect(!is_cjk_text(kimix::string_view(s), 0.15));
        }
        // just above: 4 CJK of 20 = 0.2 → true
        {
            const kimix::string s = repeat("a", 16) + utf8_of({0x4E00, 0x4E01, 0x4E02, 0x4E03});
            expect(is_cjk_text(kimix::string_view(s), 0.15));
        }
    };

    "estimate_branches"_test = [] {
        expect(eq(estimate_chars_tokens(kimix::string_view()), 0));

        // English / mostly-ASCII → total // 4
        {
            const kimix::string s(400, 'a');
            expect(eq(estimate_chars_tokens(kimix::string_view(s)), 100));
        }
        // >95% ASCII: "a"*96 + "你"*4 = 100 cps, ratio 0.96 → 100 // 4 = 25
        {
            const kimix::string s = repeat("a", 96) + utf8_of({0x4F60, 0x4F60, 0x4F60, 0x4F60});
            expect(eq(estimate_chars_tokens(kimix::string_view(s)), 25));
        }
        // exactly 95% ASCII: "a"*95 + "你"*5 → ratio 0.95 NOT > 0.95,
        // CJK 5/100 = 0.05 < 0.15 → int(100/3.5) = 28
        {
            const kimix::string s = repeat("a", 95) + repeat(utf8_of({0x4F60}), 5);
            expect(eq(estimate_chars_tokens(kimix::string_view(s)), 28));
        }
        // CJK-detected → total // 3: "你"*300 → 100
        {
            const kimix::string s = repeat(utf8_of({0x4F60}), 300);
            expect(eq(estimate_chars_tokens(kimix::string_view(s)), 100));
        }
        // "你"*6 → 6 // 3 = 2
        {
            const kimix::string s = repeat(utf8_of({0x4F60}), 6);
            expect(eq(estimate_chars_tokens(kimix::string_view(s)), 2));
        }
        // max(1, total // 3): 1 CJK char → 0 → 1
        {
            const kimix::string s = utf8_of({0x4F60});
            expect(eq(estimate_chars_tokens(kimix::string_view(s)), 1));
        }
    };

    "estimate_rounding_3_5"_test = [] {
        // Mixed / code branch: int(total / 3.5). Use "é" + (n-1) ASCII chars
        // (é is not CJK). ascii_ratio (n-1)/n stays <= 0.95 for n <= 20, so
        // the /3.5 branch applies for the exact plan vectors below:
        // int(total/3.5) with max(1, ...): 1..6 → 1, 7→2, 8→2, 9→2, 10→2,
        // 11→3, 12→3, 13→3, 14→4, 15→4, 16→4, 17→4, 18→5, 19→5, 20→5.
        const int expected[] = {0, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3,
                                4, 4, 4, 4, 5, 5, 5};
        for (int n = 1; n <= 20; ++n) {
            kimix::string s = utf8_of({0xE9}); // é
            s += std::string(static_cast<size_t>(n - 1), 'a');
            const int got = estimate_chars_tokens(kimix::string_view(s));
            expect(eq(got, expected[n])) << "total=" << n << " got=" << got;
        }
        // Verify against the Python formula independently for larger totals.
        // Use 100 non-ASCII chars so ascii_ratio (n-100)/n <= 0.95 for n <= 2000.
        const kimix::string e100 = repeat(utf8_of({0xE9}), 100);
        for (int n = 100; n <= 2000; ++n) {
            kimix::string s = e100;
            s += std::string(static_cast<size_t>(n - 100), 'a');
            const double v = static_cast<double>(n) / 3.5;
            const int py = v > 1.0 ? static_cast<int>(v) : 1;
            expect(eq(estimate_chars_tokens(kimix::string_view(s)), py)) << "n=" << n;
        }
    };

    "golden_vectors_kimi_agent"_test = [] {
        // Harvested from kimi-cli/tests/utils/test_tokens.py.
        // _estimate_chars_tokens("def foo():\n    return '你好'") == int(26/3.5) == 7
        {
            const kimix::string s = "def foo():\n    return '" + utf8_of({0x4F60, 0x597D}) + "'";
            expect(eq(estimate_chars_tokens(kimix::string_view(s)), 7));
        }
        // "The quick brown fox jumps over the lazy dog. " * 20 → 900 cps → 225
        {
            std::string s;
            for (int i = 0; i < 20; ++i) {
                s += "The quick brown fox jumps over the lazy dog. ";
            }
            expect(eq(s.size(), size_t(900)));
            expect(eq(estimate_chars_tokens(kimix::string_view(s)), 225));
        }
        // "Hello world 世" not CJK; "Hello世界" is.
        {
            const kimix::string a = "Hello world " + utf8_of({0x4E16});
            expect(!is_cjk_text(kimix::string_view(a)));
            const kimix::string b = "Hello" + utf8_of({0x4E16, 0x754C});
            expect(is_cjk_text(kimix::string_view(b)));
        }
    };

    // ---------------------------------------------------------------------------
    // Benchmarks — heuristic token counting (kimix_bench contract, bench_util.h).
    // Production shape: counting runs on every message append / prune /
    // compaction, from short messages to ~1 MB buffers. Every case asserts a
    // known-good reference so we never time a broken path, and sinks the
    // measured value so the loop cannot be optimized away.
    // ---------------------------------------------------------------------------

    "bench_scan_utf8_ascii_1mb"_test = [] {
        const std::string data(1 << 20, 'a');
        count_stats st;
        kimix_bench::run("token/scan_utf8_ascii_1mb",
                         [&] { st = scan_utf8(kimix::string_view(data)); }, 1,
                         static_cast<double>(data.size()));
        expect(eq(st.code_points, static_cast<uint32_t>(data.size())));
        expect(eq(st.ascii, static_cast<uint32_t>(data.size())));
        kimix_bench::sink(st);
    };

    "bench_scan_utf8_cjk_500kb"_test = [] {
        // "\xE4\xBD\xA0" = U+4F60 ("you"), 3 bytes per code point.
        const kimix::string data = repeat(kimix::string("\xE4\xBD\xA0", 3), 174762);
        expect(eq(data.size(), size_t(174762) * 3)); // ~512 KiB
        count_stats st;
        kimix_bench::run("token/scan_utf8_cjk_500kb",
                         [&] { st = scan_utf8(kimix::string_view(data)); }, 1,
                         static_cast<double>(data.size()));
        expect(eq(st.code_points, static_cast<uint32_t>(data.size()) / 3u));
        expect(eq(st.ascii, 0u));
        kimix_bench::sink(st);
    };

    "bench_scan_utf8_mixed_256kb"_test = [] {
        // ASCII + CJK + emoji + Latin-1 segments; no adjacent repeats.
        const kimix::string seg = "chunk " + utf8_of({0x4E00, 0x1F600, 0xE9}) + " ;";
        const std::string data(repeat(seg, 15400));
        count_stats st;
        kimix_bench::run("token/scan_utf8_mixed_256kb",
                         [&] { st = scan_utf8(kimix::string_view(data)); }, 1,
                         static_cast<double>(data.size()));
        // Independent reference walk over the same bytes (decode_cp based).
        count_stats ref;
        {
            kimix::string_view v(data);
            const char* it = v.data();
            const char* end = it + v.size();
            while (it < end) {
                const uint32_t cp = kimix::runtime::common::decode_cp(it, end);
                ++ref.code_points;
                if (cp < 0x80u) {
                    ++ref.ascii;
                }
            }
        }
        expect(eq(st.code_points, ref.code_points));
        expect(eq(st.ascii, ref.ascii));
        kimix_bench::sink(st);
    };

    "bench_estimate_ascii_1mb"_test = [] {
        const std::string data(1 << 20, 'a');
        int r = 0;
        kimix_bench::run("token/estimate_ascii_1mb",
                         [&] { r = estimate_chars_tokens(kimix::string_view(data)); },
                         1, static_cast<double>(data.size()));
        expect(eq(r, (1 << 20) / 4)); // 100% ASCII -> total // 4
        kimix_bench::sink(r);
    };

    "bench_estimate_cjk_500kb"_test = [] {
        // total = 174762 code points; 100% CJK -> total // 3.
        const kimix::string data = repeat(kimix::string("\xE4\xBD\xA0", 3), 174762);
        int r = 0;
        kimix_bench::run("token/estimate_cjk_500kb",
                         [&] { r = estimate_chars_tokens(kimix::string_view(data)); },
                         1, static_cast<double>(data.size()));
        expect(eq(r, 174762 / 3));
        kimix_bench::sink(r);
    };

    "bench_estimate_emoji_256kb"_test = [] {
        // 4-byte code points, non-CJK -> the /3.5 branch, exercising all 7
        // CJK range misses per code point in the counting pass.
        const kimix::string data = repeat(kimix::string("\xF0\x9F\x98\x80", 4), 65536);
        expect(eq(data.size(), size_t(65536) * 4)); // 256 KiB
        int r = 0;
        kimix_bench::run("token/estimate_emoji_256kb",
                         [&] { r = estimate_chars_tokens(kimix::string_view(data)); },
                         1, static_cast<double>(data.size()));
        expect(eq(r, static_cast<int>(65536.0 / 3.5)));
        kimix_bench::sink(r);
    };

    "bench_estimate_mixed_256kb"_test = [] {
        const kimix::string seg = "chunk " + utf8_of({0x4E00, 0x1F600, 0xE9}) + " ;";
        const std::string data(repeat(seg, 15400));
        int r = 0;
        kimix_bench::run("token/estimate_mixed_256kb",
                         [&] { r = estimate_chars_tokens(kimix::string_view(data)); },
                         1, static_cast<double>(data.size()));
        // Independent reference: count cps/ascii/cjk via decode_cp, apply the
        // exact Python formula (_estimate_chars_tokens).
        size_t total = 0, ascii = 0, cjk = 0;
        {
            kimix::string_view v(data);
            const char* it = v.data();
            const char* end = it + v.size();
            while (it < end) {
                const uint32_t cp = kimix::runtime::common::decode_cp(it, end);
                ++total;
                if (cp < 0x80u) {
                    ++ascii;
                }
                if (is_cjk_cp(cp)) {
                    ++cjk;
                }
            }
        }
        int expected = 0;
        const double ar = static_cast<double>(ascii) / static_cast<double>(total);
        if (ar > 0.95) {
            expected = static_cast<int>(total) / 4;
        } else if (static_cast<double>(cjk) / static_cast<double>(total) > 0.15) {
            expected = static_cast<int>(total) / 3;
        } else {
            expected = static_cast<int>(static_cast<double>(total) / 3.5);
        }
        if (expected < 1) {
            expected = 1;
        }
        expect(eq(r, expected));
        kimix_bench::sink(r);
    };

    "bench_estimate_short_msgs"_test = [] {
        // Realistic per-message counting: short ASCII body and short CJK body.
        const std::string msg(160, 'x');
        int r = 0;
        kimix_bench::time_op("token/estimate_short_ascii_160b",
                             [&] { r = estimate_chars_tokens(kimix::string_view(msg)); });
        expect(eq(r, 160 / 4));
        const kimix::string cjk = repeat(kimix::string("\xE4\xBD\xA0", 3), 30);
        int c = 0;
        kimix_bench::time_op("token/estimate_short_cjk_90b",
                             [&] { c = estimate_chars_tokens(kimix::string_view(cjk)); });
        expect(eq(c, 10)); // 30 cps of CJK -> total // 3
        kimix_bench::sink(r + c);
    };
}
