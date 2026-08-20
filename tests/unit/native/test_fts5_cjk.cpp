// Test for src/native/fts5_cjk/fts5_cjk_core.h (cjk_unicode61 tokenizer core).
// This test covers:
// - is_cjk_cp: all 12 reference CJK ranges (boundaries + interiors), non-CJK
// - utf8_decode: ASCII / 2-byte / 3-byte / 4-byte code points, invalid bytes
// - emit_cjk_bigrams: pure-ASCII passthrough (offsets preserved), CJK runs
//   re-emitted as overlapping bigrams, mixed CJK/ASCII segmentation, lone CJK
//   char unigram, 2-char CJK single bigram, clamp of sub-token offsets
#include "ut/ut.hpp"
#include <native/fts5_cjk/fts5_cjk_core.h>

#include <string>
#include <utility>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::native::fts5_cjk;

namespace {

struct TokenRecorder {
    std::vector<std::string> tokens;
    std::vector<std::pair<int, int>> offsets;
    int rc = 0; // return value from the sink (0 = OK)
};

int record_token(void* ctx, int /*tflags*/, const char* pToken, int nToken,
                 int iStart, int iEnd) {
    auto* rec = static_cast<TokenRecorder*>(ctx);
    rec->tokens.emplace_back(pToken, static_cast<size_t>(nToken));
    rec->offsets.emplace_back(iStart, iEnd);
    return rec->rc;
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "is_cjk_cp_ranges"_test = [] {
        // Hangul syllables (AC00-D7A3)
        expect(is_cjk_cp(0xAC00u));
        expect(is_cjk_cp(0xD7A3u));
        // Hangul Jamo (1100-11FF)
        expect(is_cjk_cp(0x1100u));
        expect(is_cjk_cp(0x11FFu));
        // Hangul compat Jamo (3130-318F)
        expect(is_cjk_cp(0x3130u));
        expect(is_cjk_cp(0x318Fu));
        // Hangul Jamo ext-A (A960-A97F)
        expect(is_cjk_cp(0xA960u));
        expect(is_cjk_cp(0xA97Fu));
        // Hangul Jamo ext-B (D7B0-D7FF)
        expect(is_cjk_cp(0xD7B0u));
        expect(is_cjk_cp(0xD7FFu));
        // CJK unified (4E00-9FFF)
        expect(is_cjk_cp(0x4E00u));
        expect(is_cjk_cp(0x9FFFu));
        // CJK ext A (3400-4DBF)
        expect(is_cjk_cp(0x3400u));
        expect(is_cjk_cp(0x4DBFu));
        // CJK compat (F900-FAFF)
        expect(is_cjk_cp(0xF900u));
        expect(is_cjk_cp(0xFAFFu));
        // CJK ext B..F + compat supplement (20000-2FA1F)
        expect(is_cjk_cp(0x20000u));
        expect(is_cjk_cp(0x2FA1Fu));
        // Hiragana (3040-309F)
        expect(is_cjk_cp(0x3040u));
        expect(is_cjk_cp(0x309Fu));
        // Katakana (30A0-30FF)
        expect(is_cjk_cp(0x30A0u));
        expect(is_cjk_cp(0x30FFu));
        // Katakana phonetic ext (31F0-31FF)
        expect(is_cjk_cp(0x31F0u));
        expect(is_cjk_cp(0x31FFu));
        // Non-CJK: ASCII, Latin, PUA, fullwidth forms, emoji, boundaries
        expect(!is_cjk_cp('A'));
        expect(!is_cjk_cp(0xE9u));      // é
        expect(!is_cjk_cp(0xE000u));    // PUA
        expect(!is_cjk_cp(0xFF01u));    // fullwidth !
        expect(!is_cjk_cp(0x1F600u));   // emoji
        expect(!is_cjk_cp(0x2FA1Fu + 1));
        expect(!is_cjk_cp(0x4E00u - 1));
        expect(!is_cjk_cp(0xD7A3u + 1));
    };

    "utf8_decode_codepoints"_test = [] {
        uint32_t cp = 0;
        // ASCII
        expect(eq(utf8_decode(reinterpret_cast<const unsigned char*>("A"), 1, &cp), 1));
        expect(eq(cp, 0x41u));
        // 2-byte: é = U+00E9 -> C3 A9
        const unsigned char e2[] = {0xC3, 0xA9};
        expect(eq(utf8_decode(e2, 2, &cp), 2));
        expect(eq(cp, 0xE9u));
        // 3-byte: 中 = U+4E2D -> E4 B8 AD
        const unsigned char e3[] = {0xE4, 0xB8, 0xAD};
        expect(eq(utf8_decode(e3, 3, &cp), 3));
        expect(eq(cp, 0x4E2Du));
        // 4-byte: 𠀀 = U+20000 -> F0 A0 80 80
        const unsigned char e4[] = {0xF0, 0xA0, 0x80, 0x80};
        expect(eq(utf8_decode(e4, 4, &cp), 4));
        expect(eq(cp, 0x20000u));
        // Invalid lead byte decodes as itself (termination safety)
        const unsigned char bad[] = {0xFF};
        expect(eq(utf8_decode(bad, 1, &cp), 1));
        expect(eq(cp, 0xFFu));
    };

    "emit_passthrough_ascii"_test = [] {
        const std::string input = "hello world 123";
        TokenRecorder rec;
        const int rc = emit_cjk_bigrams(
            &rec, record_token, 0, input.data(), static_cast<int>(input.size()),
            10, 10 + static_cast<int>(input.size()));
        expect(eq(rc, 0));
        expect(eq(rec.tokens.size(), 1u));
        expect(eq(rec.tokens[0], input));
        expect(eq(rec.offsets[0].first, 10));
        expect(eq(rec.offsets[0].second, 10 + static_cast<int>(input.size())));
    };

    "emit_cjk_bigrams_korean"_test = [] {
        // 캘린더 = U+C728 U+B9B0 U+B354 (3 bytes each -> 9 bytes)
        const std::string input = "\xEC\xBA\x98\xEB\xA6\xB0\xEB\x8D\x94";
        TokenRecorder rec;
        const int rc = emit_cjk_bigrams(
            &rec, record_token, 0, input.data(), static_cast<int>(input.size()),
            0, static_cast<int>(input.size()));
        expect(eq(rc, 0));
        expect(eq(rec.tokens.size(), 2u));
        expect(eq(rec.tokens[0], std::string("\xEC\xBA\x98\xEB\xA6\xB0"))); // 캘린
        expect(eq(rec.tokens[1], std::string("\xEB\xA6\xB0\xEB\x8D\x94"))); // 린더
        expect(eq(rec.offsets[0].first, 0));
        expect(eq(rec.offsets[0].second, 6));
        expect(eq(rec.offsets[1].first, 3));
        expect(eq(rec.offsets[1].second, 9));
    };

    "emit_cjk_bigrams_three_char_cjk"_test = [] {
        // 日本語 = U+65E5 U+672C U+8A9E -> bigrams 日本, 本語
        const std::string input = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";
        TokenRecorder rec;
        const int rc = emit_cjk_bigrams(
            &rec, record_token, 0, input.data(), static_cast<int>(input.size()),
            5, 5 + static_cast<int>(input.size()));
        expect(eq(rc, 0));
        expect(eq(rec.tokens.size(), 2u));
        expect(eq(rec.tokens[0], std::string("\xE6\x97\xA5\xE6\x9C\xAC"))); // 日本
        expect(eq(rec.tokens[1], std::string("\xE6\x9C\xAC\xE8\xAA\x9E"))); // 本語
        // Offsets clamp to the outer [iStart, iEnd) window (5..14)
        expect(eq(rec.offsets[0].first, 5));
        expect(eq(rec.offsets[0].second, 11));
        expect(eq(rec.offsets[1].first, 8));
        expect(eq(rec.offsets[1].second, 14));
    };

    "emit_cjk_two_char_single_bigram"_test = [] {
        // 中文 = U+4E2D U+6587 -> exactly one bigram
        const std::string input = "\xE4\xB8\xAD\xE6\x96\x87";
        TokenRecorder rec;
        const int rc = emit_cjk_bigrams(
            &rec, record_token, 0, input.data(), static_cast<int>(input.size()),
            0, static_cast<int>(input.size()));
        expect(eq(rc, 0));
        expect(eq(rec.tokens.size(), 1u));
        expect(eq(rec.tokens[0], input));
        expect(eq(rec.offsets[0].first, 0));
        expect(eq(rec.offsets[0].second, 6));
    };

    "emit_cjk_lone_char_unigram"_test = [] {
        // 中 (single CJK char) -> unigram
        const std::string input = "\xE4\xB8\xAD";
        TokenRecorder rec;
        const int rc = emit_cjk_bigrams(
            &rec, record_token, 0, input.data(), static_cast<int>(input.size()),
            2, 5);
        expect(eq(rc, 0));
        expect(eq(rec.tokens.size(), 1u));
        expect(eq(rec.tokens[0], input));
        expect(eq(rec.offsets[0].first, 2));
        expect(eq(rec.offsets[0].second, 5));
    };

    "emit_cjk_mixed_segments"_test = [] {
        // a캘린b : 'a'(1) + 캘린(6) + 'b'(1)
        const std::string input = "a\xEC\xBA\x98\xEB\xA6\xB0" "b";
        TokenRecorder rec;
        const int rc = emit_cjk_bigrams(
            &rec, record_token, 0, input.data(), static_cast<int>(input.size()),
            0, static_cast<int>(input.size()));
        expect(eq(rc, 0));
        expect(eq(rec.tokens.size(), 3u));
        expect(eq(rec.tokens[0], std::string("a")));
        expect(eq(rec.tokens[1], std::string("\xEC\xBA\x98\xEB\xA6\xB0"))); // 캘린
        expect(eq(rec.tokens[2], std::string("b")));
        expect(eq(rec.offsets[0].first, 0));
        expect(eq(rec.offsets[0].second, 1));
        expect(eq(rec.offsets[1].first, 1));
        expect(eq(rec.offsets[1].second, 7));
        expect(eq(rec.offsets[2].first, 7));
        expect(eq(rec.offsets[2].second, 8));
    };

    "emit_sink_error_propagates"_test = [] {
        // Sink abort: the leading segment is delivered, then the non-zero
        // return value from the outer sink aborts tokenization immediately.
        const std::string input = "abc";
        TokenRecorder rec;
        rec.rc = 1; // simulate outer sink abort
        const int rc = emit_cjk_bigrams(
            &rec, record_token, 0, input.data(), static_cast<int>(input.size()),
            0, static_cast<int>(input.size()));
        expect(eq(rc, 1));
        expect(eq(rec.tokens.size(), 1u));
        expect(eq(rec.tokens[0], std::string("abc")));
        expect(eq(rec.offsets[0].first, 0));
        expect(eq(rec.offsets[0].second, 3));
    };

    "emit_empty_token"_test = [] {
        // Empty input: no CJK scan runs, so the token passes through untouched
        // (mirrors the reference wrapper's passthrough fast path).
        TokenRecorder rec;
        const int rc = emit_cjk_bigrams(&rec, record_token, 0, "", 0, 0, 0);
        expect(eq(rc, 0));
        expect(eq(rec.tokens.size(), 1u));
        expect(eq(rec.tokens[0], std::string("")));
    };
}
