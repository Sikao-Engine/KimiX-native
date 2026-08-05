// Test for string_scratch.h (kimix::StringScratch).
// This test covers:
// - Basic << operators (int, float, string, bool, etc.)
// - clear()
// - string() and string_view()
// - c_str() returns null-terminated
// - pop_back

#include "ut/ut.hpp"
#include <core/kimix_core.h>

#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "scratch_int"_test = [] {
        kimix::StringScratch ss;
        ss << 42;
        expect(ss.string_view() == "42");
    };

    "scratch_negative_int"_test = [] {
        kimix::StringScratch ss;
        ss << -10;
        expect(ss.string_view() == "-10");
    };

    "scratch_unsigned"_test = [] {
        kimix::StringScratch ss;
        ss << 100u;
        expect(ss.string_view() == "100");
    };

    "scratch_long_long"_test = [] {
        kimix::StringScratch ss;
        ss << 9876543210ll;
        expect(ss.string_view() == "9876543210");
    };

    "scratch_float"_test = [] {
        kimix::StringScratch ss;
        ss << 3.14f;
        auto sv = ss.string_view();
        expect(sv.find("3.14") != kimix::string_view::npos) << "float output should contain '3.14'";
    };

    "scratch_double"_test = [] {
        kimix::StringScratch ss;
        ss << 2.718;
        auto sv = ss.string_view();
        expect(sv.find("2.718") != kimix::string_view::npos);
    };

    "scratch_string_view"_test = [] {
        kimix::StringScratch ss;
        kimix::string_view sv = "hello";
        ss << sv;
        expect(ss.string_view() == "hello");
    };

    "scratch_c_string"_test = [] {
        kimix::StringScratch ss;
        ss << "world";
        expect(ss.string_view() == "world");
    };

    "scratch_char"_test = [] {
        kimix::StringScratch ss;
        ss << 'X';
        expect(ss.string_view() == "X");
    };

    "scratch_bool_true"_test = [] {
        kimix::StringScratch ss;
        ss << true;
        expect(ss.string_view() == "true");
    };

    "scratch_bool_false"_test = [] {
        kimix::StringScratch ss;
        ss << false;
        expect(ss.string_view() == "false");
    };

    "scratch_size_t"_test = [] {
        kimix::StringScratch ss;
        ss << size_t{12345};
        expect(ss.string_view() == "12345");
    };

    "scratch_chain"_test = [] {
        kimix::StringScratch ss;
        ss << "hello " << "world " << 123;
        expect(ss.string_view() == "hello world 123");
    };

    "scratch_clear"_test = [] {
        kimix::StringScratch ss;
        ss << "some text";
        expect(!ss.empty());
        ss.clear();
        expect(ss.empty());
        expect(ss.string_view() == "");
    };

    "scratch_string"_test = [] {
        kimix::StringScratch ss;
        ss << "test";
        const kimix::string& s = ss.string();
        expect(s == "test");
    };

    "scratch_string_view"_test = [] {
        kimix::StringScratch ss;
        ss << "view test";
        kimix::string_view sv = ss.string_view();
        expect(sv == "view test");
    };

    "scratch_c_str"_test = [] {
        kimix::StringScratch ss;
        ss << "cstr";
        const char* c = ss.c_str();
        expect(std::strcmp(c, "cstr") == 0) << "c_str() should return null-terminated string";
    };

    "scratch_size"_test = [] {
        kimix::StringScratch ss;
        ss << "abcd";
        expect(eq(ss.size(), 4_u));
    };

    "scratch_empty"_test = [] {
        kimix::StringScratch ss;
        expect(ss.empty());
        ss << "x";
        expect(!ss.empty());
    };

    "scratch_pop_back"_test = [] {
        // StringScratch doesn't directly expose pop_back, but the underlying
        // string does. Test that pop_back on the string works as expected.
        kimix::StringScratch ss;
        ss << "hello";
        expect(ss.string_view() == "hello");

        // Access the underlying string and pop_back
        kimix::string& s = const_cast<kimix::string&>(ss.string());
        s.pop_back();
        expect(ss.string_view() == "hell");

        s.pop_back();
        s.pop_back();
        expect(ss.string_view() == "he");
    };

    "scratch_reserve"_test = [] {
        kimix::StringScratch ss(64);
        // Should not crash when growing within reserved size
        for (int i = 0; i < 10; ++i) {
            ss << "data";
        }
        expect(ss.size() > 0_u);
    };

}
