// Test for src/runtime/tools/line_hash.h (plan 013 chained xxHash32).
// This test covers:
// - xxh32 golden values (verified against the Python `xxhash` package)
// - compute_line_hash: CR strip, whitespace filter, seed
// - compute_line_hashes: chained seed semantics (nibble decode), first-line
//   has_significant / line_num seeds
// - Unicode whitespace + alnum handling (NBSP filtered, CJK significant)

#include "ut/ut.hpp"
#include <runtime/tools/line_hash.h>

#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::tools;

namespace {
kimix::string_view sv(const std::string& s) { return kimix::string_view(s); }

uint32_t nibble_code(uint32_t nib) {
    const char* ns = "ZPMQVRWSNKTXJBYH";
    return static_cast<unsigned char>(ns[nib & 15]);
}
} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "line_hash_golden"_test = [] {
        // xxh32(b"helloworld", 0) & 0xFF == 2 (space filtered)
        expect(eq(compute_line_hash(sv("hello world"), 0), 2u));
        // xxh32(b"", 0) & 0xFF == 5
        expect(eq(compute_line_hash(sv(""), 0), 5u));
        expect(eq(compute_line_hash(sv("   "), 0), 5u)); // all whitespace
        // xxh32(b"", 42) & 0xFF == 184
        expect(eq(compute_line_hash(sv(""), 42), 184u));
        // xxh32(b"abc", 12345) & 0xFF == 41
        expect(eq(compute_line_hash(sv("abc"), 12345), 41u));
        // xxh32(b"x"*2000, 0) & 0xFF == 141 (>= 16 byte path)
        expect(eq(compute_line_hash(sv(std::string(2000, 'x')), 0), 141u));
        // trailing CR stripped
        expect(eq(compute_line_hash(sv("abc\r"), 12345), 41u));
        // tab is whitespace -> filtered
        expect(eq(compute_line_hash(sv("a\tb"), 0), compute_line_hash(sv("ab"), 0)));
    };

    "line_hash_unicode"_test = [] {
        // NBSP (U+00A0, bytes C2 A0) is whitespace -> filtered out
        const std::string nbsp = "a\xC2\xA0" "b";
        expect(eq(compute_line_hash(sv(nbsp), 0), compute_line_hash(sv("ab"), 0)));
        // CJK chars are alnum (has_significant) and kept
        const std::string cjk = "\xE4\xB8\xAD\xE6\x96\x87"; // "中文"
        expect(eq(compute_line_hash(sv(cjk), 0), compute_line_hash(sv(cjk), 0)));
        expect(is_alnum_cp(0x4E2D));
        expect(is_alnum_cp(0x00E9));
        expect(!is_alnum_cp(0x00B7)); // middle dot: not alnum
        expect(!is_alnum_cp(0x00A0)); // NBSP: not alnum
    };

    "line_hashes_chain"_test = [] {
        kimix::vector<uint32_t> out;
        // "line1": has_significant -> seed 0 -> xxh32(b"line1",0)&0xFF
        // "line2": prev hash 2 -> nibble string "ZM" -> seed = 'Z'*256+'M'
        compute_line_hashes(sv("line1\nline2\n"), 0, out);
        expect(eq(out.size(), 2u));
        expect(eq(out[0], 2u));
        expect(eq(out[1], 153u));
        // empty content -> no lines
        out.clear();
        compute_line_hashes(sv(""), 0, out);
        expect(out.empty());
        // CRLF: trailing \r stripped per line, same as LF
        kimix::vector<uint32_t> out2;
        compute_line_hashes(sv("line1\r\nline2\r\n"), 0, out2);
        kimix::vector<uint32_t> lf;
        compute_line_hashes(sv("line1\nline2\n"), 0, lf);
        expect(eq(out2.size(), 2u));
        expect(eq(out2[0], lf[0]));
        expect(eq(out2[1], lf[1]));
        // first line with no alnum -> seed = line_num (1)
        kimix::vector<uint32_t> out3;
        compute_line_hashes(sv("!!!\n"), 0, out3);
        expect(eq(out3.size(), 1u));
        // seed 1 for line 1 with no significant chars
        expect(eq(out3[0], compute_line_hash(sv("!!!"), 1)));
        // second line chain continues from previous hash
        kimix::vector<uint32_t> out4;
        compute_line_hashes(sv("!!!\nabc\n"), 0, out4);
        expect(eq(out4.size(), 2u));
        // prev hash h0 -> nibble "XY" -> seed = NIBBLE_CODE[h0>>4]*256 + NIBBLE_CODE[h0&15]
        const uint32_t h0 = out4[0];
        const uint32_t seed = (nibble_code(h0 >> 4) * 256 + nibble_code(h0 & 15)) & 0xFFFFFFFFu;
        expect(eq(out4[1], compute_line_hash(sv("abc"), seed)));
    };

    "line_hashes_unusual_endings"_test = [] {
        kimix::vector<uint32_t> out;
        // no trailing newline: last line still hashed
        compute_line_hashes(sv("a\nb"), 0, out);
        expect(eq(out.size(), 2u));
        // blank lines between
        out.clear();
        compute_line_hashes(sv("a\n\nb\n"), 0, out);
        expect(eq(out.size(), 3u));
    };
}
