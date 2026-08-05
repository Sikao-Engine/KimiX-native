// Test for stl/string.h (kimix::string).
// This test covers:
// - Default construction
// - Construction from literal
// - Copy/move
// - append, substr, find
// - Comparison operators
// - Conversion to/from std::string

#include "ut/ut.hpp"
#include <core/kimix_core.h>

#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "string_default_construction"_test = [] {
        kimix::string s;
        expect(s.empty()) << "default-constructed string should be empty";
        expect(eq(s.size(), 0_u)) << "default-constructed string size should be 0";
    };

    "string_construction_from_literal"_test = [] {
        kimix::string s = "hello";
        expect(eq(s.size(), 5_u));
        expect(s == "hello");
    };

    "string_construction_from_std_string"_test = [] {
        std::string std_s = "test";
        kimix::string s(std_s);
        expect(s == "test");
    };

    "string_copy_construction"_test = [] {
        kimix::string s1 = "original";
        kimix::string s2(s1);
        expect(s2 == "original");
        expect(eq(s1.size(), s2.size()));
    };

    "string_copy_assignment"_test = [] {
        kimix::string s1 = "first";
        kimix::string s2 = "second";
        s2 = s1;
        expect(s2 == "first");
    };

    "string_move_construction"_test = [] {
        kimix::string s1 = "move me";
        kimix::string s2(std::move(s1));
        expect(s2 == "move me");
        // s1 is in valid but unspecified state
    };

    "string_move_assignment"_test = [] {
        kimix::string s1 = "source";
        kimix::string s2 = "dest";
        s2 = std::move(s1);
        expect(s2 == "source");
    };

    "string_append"_test = [] {
        kimix::string s = "hello";
        s.append(" world");
        expect(s == "hello world");
        expect(eq(s.size(), 11_u));
    };

    "string_append_char"_test = [] {
        kimix::string s = "ab";
        s.push_back('c');
        expect(s == "abc");
    };

    "string_append_operator"_test = [] {
        kimix::string s = "hello";
        s += " world";
        expect(s == "hello world");
    };

    "string_substr"_test = [] {
        kimix::string s = "hello world";
        auto sub = s.substr(0, 5);
        expect(sub == "hello");
        auto sub2 = s.substr(6, 5);
        expect(sub2 == "world");
    };

    "string_find"_test = [] {
        kimix::string s = "hello world";
        auto pos = s.find("world");
        expect(eq(pos, 6_u)) << "'world' should be found at position 6";
        auto pos2 = s.find("xyz");
        expect(eq(pos2, kimix::string::npos)) << "'xyz' should not be found";
    };

    "string_comparison_operators"_test = [] {
        kimix::string a = "abc";
        kimix::string b = "abc";
        kimix::string c = "abd";
        kimix::string d = "ab";

        expect(a == b);
        expect(a != c);
        expect(a < c);
        expect(a > d);
        expect(a >= b);
        expect(a <= b);
    };

    "string_conversion_to_std_string"_test = [] {
        kimix::string ws = "convert me";
        std::string ss(ws);
        expect(ss == "convert me");
    };

    "string_from_std_string"_test = [] {
        std::string ss = "std string";
        kimix::string ws(ss);
        expect(ws == "std string");
    };

    "string_c_str"_test = [] {
        kimix::string s = "cstr test";
        const char* c = s.c_str();
        expect(c != nullptr);
        expect(c == std::string_view{"cstr test"});
    };

}
