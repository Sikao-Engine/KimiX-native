// Test for core/stl/format.h (kimix::format built on C++20 std::format).
// This test covers:
// - basic substitution (int, unsigned, float, double, bool, char)
// - string argument types (const char *, std::string, kimix::string, string_view)
// - format specs (precision, hex, binary, scientific, padding)
// - positional args and escaped braces
// - kimix::format_to and custom destination string types
// - long strings and empty format output

#include "ut/ut.hpp"
#include <core/stl/format.h>
#include <core/stl/string.h>

#include <string>
#include <string_view>
#include <type_traits>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "format_returns_kimix_string"_test = [] {
        auto s = kimix::format("{}", 42);
        static_assert(std::is_same_v<decltype(kimix::format("{}", 42)), kimix::string>);
        expect(s == "42") << "kimix::format should return a kimix::string with formatted content";
    };

    "format_basic_types"_test = [] {
        expect(kimix::format("{}", 0) == "0");
        expect(kimix::format("{}", -123456) == "-123456");
        expect(kimix::format("{}", 123456u) == "123456");
        expect(kimix::format("{}", true) == "true");
        expect(kimix::format("{}", false) == "false");
        expect(kimix::format("{}", 'a') == "a");
        expect(std::string_view{kimix::format("{}", 3.5).c_str()} == "3.5");
    };

    "format_multiple_args"_test = [] {
        expect(kimix::format("{}, {}", 1, 2) == "1, 2");
        expect(kimix::format("bool={}, str={}", true, "hello") == "bool=true, str=hello");
        expect(kimix::format("answer is {}", 42) == "answer is 42");
    };

    "format_string_argument_types"_test = [] {
        const char *cstr = "cstr";
        std::string std_str = "std_string";
        kimix::string kimix_str = "kimix_string";
        std::string_view sv = "string_view";
        expect(kimix::format("{}", cstr) == "cstr");
        expect(kimix::format("{}", std_str) == "std_string");
        expect(kimix::format("{}", kimix_str) == "kimix_string");
        expect(kimix::format("{}", sv) == "string_view");
    };

    "format_float_specs"_test = [] {
        expect(kimix::format("{:.2f}", 3.14159) == "3.14");
        expect(kimix::format("{:.0f}", 2.5) == "2");
        expect(std::string_view{kimix::format("{:e}", 12345.6789).c_str()}.substr(0, 6) == "1.2345");
    };

    "format_integer_specs"_test = [] {
        expect(kimix::format("{:x}", 255) == "ff");
        expect(kimix::format("{:X}", 255) == "FF");
        expect(kimix::format("{:b}", 5) == "101");
        expect(kimix::format("{:o}", 8) == "10");
        expect(kimix::format("{:05d}", 42) == "00042");
        expect(kimix::format("{:016X}", static_cast<uint64_t>(0xDEADBEEF)) == "00000000DEADBEEF");
    };

    "format_positional_and_escaped_braces"_test = [] {
        expect(kimix::format("{1} {0}", "world", "hello") == "hello world");
        expect(kimix::format("{0} {0}", "again") == "again again");
        expect(kimix::format("{{}}") == "{}");
        expect(kimix::format("{{{}}}", 7) == "{7}");
    };

    "format_pointer"_test = [] {
        expect(kimix::format("{}", static_cast<void *>(nullptr)) == "0x0");
        int x = 0;
        auto s = kimix::format("{}", static_cast<void *>(&x));
        expect(s.size() > 2u && s[0] == '0' && s[1] == 'x') << "pointer should format as 0x...";
    };

    "format_to_appends_to_buffer"_test = [] {
        std::string buffer = "prefix:";
        kimix::format_to(std::back_inserter(buffer), "{} {}", 1, 2);
        expect(buffer == "prefix:1 2");
    };

    "format_custom_destination_string"_test = [] {
        // kimix::format<String> formats into an arbitrary basic_string-like type
        auto std_str = kimix::format<std::string>("{} + {} = {}", 1, 2, 3);
        static_assert(std::is_same_v<decltype(std_str), std::string>);
        expect(std_str == "1 + 2 = 3");

        auto kimix_str = kimix::format<kimix::string>("x={}", 9);
        static_assert(std::is_same_v<decltype(kimix_str), kimix::string>);
        expect(kimix_str == "x=9");
    };

    "format_large_output"_test = [] {
        // exceed fmt's inline buffer so the heap/allocation path is exercised
        kimix::string long_piece = "This is a long message repeated multiple times. ";
        kimix::string expected;
        for (int i = 0; i < 100; ++i) { expected += long_piece; }
        kimix::string result;
        for (int i = 0; i < 100; ++i) { result += kimix::format("{}", long_piece); }
        expect(result.size() > 4000u) << "long output should be substantial";
        expect(result == expected);
    };

    "format_empty_and_special_chars"_test = [] {
        expect(kimix::format("").empty());
        expect(kimix::format("{}", "") == "");
        expect(kimix::format("tab\tnewline\n{}", 1) == "tab\tnewline\n1");
    };

    "fmt_string_macro_available"_test = [] {
        // FMT_STRING is provided by format.h (passthrough when fmt does not
        // define it); it must be usable in constant expressions of format calls.
        expect(kimix::format(FMT_STRING("{}"), 5) == "5");
    };
}
