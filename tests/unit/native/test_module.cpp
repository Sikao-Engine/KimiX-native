// Test for the kimix runtime native module scaffold (runtime.dll + runtime_py).
// This test covers:
// - C-FFI version entry points (kimix_runtime_version / core_version)
// - UTF-8 kernels: decode_cp (1..4 byte + invalid input), code-point count,
//   is_ascii fast path, utf8_byte_length
//
// Links the runtime shared library (runtime.dll); the Python-side parity
// checks live in python/tests/test_scaffold.py.

#include "ut/ut.hpp"
#include <runtime/runtime.h>
#include <runtime/common/utf8.h>

#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::common;

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "c_ffi_version"_test = [] {
        const char* rv = kimix_runtime_version();
        const char* cv = kimix::runtime::core_version();
        const char* crv = kimix_runtime_core_version();
        expect(rv != nullptr && std::strncmp(rv, "kimix", 5) == 0) << "runtime version starts with kimix";
        expect(cv != nullptr && std::strncmp(cv, "kimix", 5) == 0) << "core version starts with kimix";
        expect(crv != nullptr && std::strncmp(crv, "kimix", 5) == 0) << "C-FFI core version starts with kimix";
        expect(std::strcmp(rv, "kimix-runtime 0.1.0") == 0);
        expect(std::strcmp(crv, cv) == 0);
    };

    "utf8_decode"_test = [] {
        // 1-byte ASCII: 'A'
        const char a[] = {'A'};
        const char* it = a;
        const char* end = a + 1;
        expect(eq(decode_cp(it, end), uint32_t('A')));
        expect(it == end);

        // 2-byte: é = U+00E9 (0xC3 0xA9)
        const char e[] = {char(0xC3), char(0xA9)};
        const char* it2 = e;
        const char* end2 = e + 2;
        expect(eq(decode_cp(it2, end2), uint32_t(0xE9u)));
        expect(it2 == end2);

        // 4-byte: U+1F600 GRINNING FACE (F0 9F 98 80)
        const char em[] = {char(0xF0), char(0x9F), char(0x98), char(0x80)};
        const char* it3 = em;
        const char* end3 = em + 4;
        expect(eq(decode_cp(it3, end3), uint32_t(0x1F600u)));
        expect(it3 == end3);

        // 4-byte: U+20000 CJK EXT B (F0 A0 80 80)
        const char cjk[] = {char(0xF0), char(0xA0), char(0x80), char(0x80)};
        const char* it4 = cjk;
        const char* end4 = cjk + 4;
        expect(eq(decode_cp(it4, end4), uint32_t(0x20000u)));
        expect(it4 == end4);

        // invalid: lone continuation byte 0x80 -> U+FFFD, advance exactly 1
        const char cont[] = {char(0x80)};
        const char* it5 = cont;
        const char* end5 = cont + 1;
        expect(eq(decode_cp(it5, end5), uint32_t(0xFFFDu)));
        expect(it5 == end5);

        // invalid: truncated 3-byte at end (0xE4 0xB8, missing 3rd byte)
        const char tr[] = {char(0xE4), char(0xB8)};
        const char* it6 = tr;
        const char* end6 = tr + 2;
        expect(eq(decode_cp(it6, end6), uint32_t(0xFFFDu)));
        expect(it6 == tr + 1);

        // invalid: overlong 2-byte encoding of NUL (0xC0 0x80)
        const char ov[] = {char(0xC0), char(0x80)};
        const char* it7 = ov;
        const char* end7 = ov + 2;
        expect(eq(decode_cp(it7, end7), uint32_t(0xFFFDu)));
        expect(it7 == ov + 1);

        // invalid lead byte 0xFF
        const char ff[] = {char(0xFF)};
        const char* it8 = ff;
        const char* end8 = ff + 1;
        expect(eq(decode_cp(it8, end8), uint32_t(0xFFFDu)));
        expect(it8 == end8);
    };

    "utf8_count"_test = [] {
        // "héllo😀中🙂" = h é l l o 😀 中 🙂 -> 8 code points, 17 bytes
        const char mixed[] = {'h', char(0xC3), char(0xA9), 'l', 'l', 'o',
                              char(0xF0), char(0x9F), char(0x98), char(0x80),
                              char(0xE4), char(0xB8), char(0xAD),
                              char(0xF0), char(0x9F), char(0x99), char(0x82)};
        expect(eq(utf8_code_point_count(kimix::string_view(mixed, 17)), size_t(8)));

        expect(eq(utf8_code_point_count(kimix::string_view("", 0)), size_t(0)));
        expect(eq(utf8_code_point_count(kimix::string_view("hello", 5)), size_t(5)));

        // Invalid bytes each count as one code point (truncated 3-byte).
        const char bad[] = {char(0xE4), char(0xB8)};
        expect(eq(utf8_code_point_count(kimix::string_view(bad, 2)), size_t(2)));
    };

    "utf8_ascii_fastpath"_test = [] {
        expect(is_ascii(kimix::string_view("", 0)));
        expect(is_ascii(kimix::string_view("hello world", 11)));
        expect(!is_ascii(kimix::string_view("caf\xC3\xA9", 5)));
        expect(!is_ascii(kimix::string_view("ok\x01\x80", 4)));
    };

    "utf8_byte_length"_test = [] {
        expect(eq(utf8_byte_length(0x41u), size_t(1)));       // 'A'
        expect(eq(utf8_byte_length(0xE9u), size_t(2)));       // é
        expect(eq(utf8_byte_length(0x4E2Du), size_t(3)));     // 中
        expect(eq(utf8_byte_length(0x1F600u), size_t(4)));    // 😀
        expect(eq(utf8_byte_length(0x20000u), size_t(4)));    // CJK ext B
        expect(eq(utf8_byte_length(0x10FFFFu), size_t(4)));   // max code point
    };
}
