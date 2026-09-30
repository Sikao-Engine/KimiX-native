// Test for stl/filesystem.h (kimix path <-> narrow-string conversions).
//
// to_string() must never throw or terminate: the library is compiled
// without C++ exceptions, and the std::filesystem::path::string<char>()
// it replaces threw std::system_error ("No mapping for the Unicode
// character exists in the target multi-byte code page") on any name
// containing characters unrepresentable in the ANSI code page - which
// terminated the whole CLI (__fastfail 0xC0000409) when a directory
// walk hit such a name. Unrepresentable names are now returned lossily.
// path_from_narrow() reports unrepresentable input through its return
// value instead of throwing.
#include "ut/ut.hpp"
#include <core/stl/filesystem.h>
#include <string>
#if defined(_WIN32) || defined(_WIN64)
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>
#endif
using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "fs_to_string_roundtrips_representable_path"_test = [] {
        kimix::filesystem::path p;
        expect(kimix::path_from_narrow("some/dir/file.txt", p));
        expect(kimix::to_string(p) == "some/dir/file.txt");
        kimix::filesystem::path back;
        expect(kimix::path_from_narrow(kimix::to_string(p), back));
        expect(back == p);
    };

    "fs_path_from_utf8_roundtrips_ascii"_test = [] {
        kimix::filesystem::path p;
        expect(kimix::path_from_utf8("some/dir/file.txt", p));
        expect(kimix::to_string(p) == "some/dir/file.txt");
    };

    "fs_path_from_utf8_empty_is_empty_path"_test = [] {
        kimix::filesystem::path p;
        expect(kimix::path_from_utf8("", p));
        expect(p.empty());
    };

    "fs_to_string_empty_path_is_empty_string"_test = [] {
        expect(kimix::to_string(kimix::filesystem::path()).empty());
    };

#if defined(_WIN32) || defined(_WIN64)
    // A UTF-8 name that the ANSI code page cannot represent must still build
    // a correct wide path: the narrow path construction it replaces decoded
    // through the ACP and threw std::system_error on exactly these bytes,
    // terminating the CLI (__fastfail 0xC0000409) when a glob walk hit such
    // a directory name on a GBK machine.
    "fs_path_from_utf8_keeps_names_outside_the_acp"_test = [] {
        kimix::filesystem::path p;
        expect(kimix::path_from_utf8("sub\xEF\x80\xBA\xEF\x81\x9C", p));
        expect(p.native() == std::wstring(L"sub\xF03A\xF05C"));
    };

    // Bytes that are not valid UTF-8 are reported through the return value
    // (out stays cleared) instead of being decoded lossily to a different
    // name - e.g. GBK-encoded text is NOT UTF-8 and must not be misread.
    "fs_path_from_utf8_rejects_non_utf8_bytes"_test = [] {
        kimix::filesystem::path p;
        expect(!kimix::path_from_utf8("bad\xFF\xFEname", p));
        expect(p.empty());
        expect(!kimix::path_from_utf8("\xD6\xD0", p)); // '中' in GBK
        expect(p.empty());
    };
#endif

#if defined(_WIN32) || defined(_WIN64)
    // 'D<U+F03A><U+F05C>proj' is a real directory name an agent run created;
    // both code points are unmapped in the GBK code page and the conversion
    // used to throw/terminate. Now it must degrade lossily to the default
    // replacement character (unless the ACP is UTF-8, which can represent
    // every valid scalar).
    "fs_to_string_lossy_on_unconvertible_names"_test = [] {
        const kimix::filesystem::path p(std::wstring(L"D\xF03A\xF05Cproj"));
        const kimix::string s = kimix::to_string(p);
        expect(!s.empty());
        if (::GetACP() != CP_UTF8) {
            expect(s == "D??proj");
        }
    };

    // Lone surrogates are invalid scalar values: even the lossy code-page
    // pass rejects them, so the UTF-8 fallback sanitizes them to '?'.
    "fs_to_string_sanitizes_lone_surrogates"_test = [] {
        const kimix::filesystem::path p(std::wstring(L"sur\xD800rogate"));
        expect(kimix::to_string(p) == "sur?rogate");
    };
#endif

    return 0;
}
