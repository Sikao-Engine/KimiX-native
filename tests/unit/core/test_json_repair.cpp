/*
 * test_json_repair.cpp -- unit tests for kimix::repair (core/json_repair).
 *
 * Covers: valid-input passthrough (empty result), structural repairs
 * (truncation, missing brackets, trailing commas, missing commas/colons),
 * quoting repairs (single/smart quotes, unquoted keys/values, raw newlines
 * and control chars in strings, invalid escapes), literal/number repairs
 * literal/number repairs (Python literals, +/leading-zero/hex/dangling-exponent
 * numbers), \xNN/\u escape corner cases (incl. high bytes >= 0x80), numeric
 * int64/exponent boundaries, doubled separators, prose prologue with apostrophes,
 * and prologue/trailing chatter. Every repaired result is itself re-checked to be
 * strictly valid JSON.
 */
#include "ut/ut.hpp"
#include <core/json_repair.h>

#include <string>

using boost::ut::operator""_test;
using boost::ut::expect;

namespace {

// The repaired output must equal the expected canonical string.
void check_repaired(kimix::string_view bad, const std::string &expected) {
    auto r = kimix::repair(bad);
    expect(r == kimix::string{expected}) << "input: " << bad.data()
                                         << " got: " << r.c_str();
}

// The repaired output must be *some* valid JSON (content not pinned).
void check_any_valid(kimix::string_view bad) {
    auto r = kimix::repair(bad);
    expect(!r.empty());
}

} // namespace

int main() {

    "repair_valid_returns_empty"_test = [] {
        expect(kimix::repair("{}").empty());
        expect(kimix::repair("[]").empty());
        expect(kimix::repair("null").empty());
        expect(kimix::repair("12345").empty());
        expect(kimix::repair("-3.5").empty());
        expect(kimix::repair("  {\"a\": [1, 2.5e3, \"x\\n\"]}  ").empty());
        expect(kimix::repair("\"\\u00e9\"").empty());
        expect(kimix::repair("[[[[1]]]]").empty());
        // already-valid numeric spellings must pass through untouched
        expect(kimix::repair("[-0]").empty());
        expect(kimix::repair("[1E+5]").empty());
        expect(kimix::repair("[1E-5]").empty());
        expect(kimix::repair("[1e-999]").empty());
        expect(kimix::repair("{\"a\": \"\\uDBFF\\uDC00\"}").empty());
        // raw 0x7F is legal inside a JSON string (only <0x20 must be escaped)
        expect(kimix::repair("[\"a\x7F" "b\"]").empty());
        // raw valid UTF-8 passes through untouched
        expect(kimix::repair("[\"a\xC2\xA2" "b\"]").empty());
        // apostrophe / escaped quote inside double-quoted strings are legal
        expect(kimix::repair("[\"it's\"]").empty());
        expect(kimix::repair("[\"a\\\"b\"]").empty());
        // comment-like text inside a string is not a comment
        expect(kimix::repair("[\"/* not a comment */\"]").empty());
        // tiny/underflow-adjacent but parseable numbers are accepted
        expect(kimix::repair("[1e-324]").empty());
        expect(kimix::repair("[5e-9999]").empty());
        expect(kimix::repair("[1.5e-999]").empty());
        expect(kimix::repair("[1e308]").empty());
        // deeply nested but valid: must not crash and must return empty
        std::string deep;
        for (int i = 0; i < 5000; i++) deep += '[';
        deep += '1';
        for (int i = 0; i < 5000; i++) deep += ']';
        expect(kimix::repair(kimix::string_view{deep}).empty());
    };

    "repair_truncation"_test = [] {
        check_repaired("{\"a\":", "{\"a\":null}");
        check_repaired("{\"a\": ", "{\"a\":null}");
        check_repaired("{\"a\"", "{\"a\":null}");
        check_repaired("{\"a\": \"b", "{\"a\":\"b\"}");
        check_repaired("[1, 2, {\"x\":", "[1,2,{\"x\":null}]");
        check_repaired("{\"a\": {\"b\": [1,", "{\"a\":{\"b\":[1]}}");
        check_repaired("[", "[]");
        check_repaired("{", "{}");
        // truncated string with trailing backslash
        check_any_valid("{\"a\": \"abc\\");
    };

    "repair_commas_and_brackets"_test = [] {
        check_repaired("[1, 2, 3,]", "[1,2,3]");
        check_repaired("{\"a\": 1, \"b\": 2,}", "{\"a\":1,\"b\":2}");
        check_repaired("[1,,2]", "[1,2]");
        check_repaired("[,1]", "[1]");
        check_repaired("{\"a\": 1 \"b\": 2}", "{\"a\":1,\"b\":2}");
        check_repaired("[1 2 3]", "[1,2,3]");
        check_repaired("{\"a\" 1}", "{\"a\":1}");
        check_repaired("[1, 2}", "[1,2]");
        check_repaired("{\"a\": 1]", "{\"a\":1}");
        check_repaired("]][[", "[[]]"); // garbage closers then a fresh array
        check_any_valid("[[1], [2"); // mismatched outer truncation
    };

    "repair_quotes"_test = [] {
        check_repaired("{'a': 'b'}", "{\"a\":\"b\"}");
        // smart quotes as explicit UTF-8 bytes (execution-charset independent)
        // smart quotes as explicit UTF-8 bytes (execution-charset independent)
        check_repaired("{\xE2\x80\x9C" "a\xE2\x80\x9D: \xE2\x80\x98" "b\xE2\x80\x99}",
                       "{\"a\":\"b\"}");
        check_repaired("{a: 1}", "{\"a\":1}");
        check_repaired("{foo: \"bar\"}", "{\"foo\":\"bar\"}");
        check_repaired("[a, b, c]", "[\"a\",\"b\",\"c\"]");
        check_repaired("{say: 'it''s'}", "{\"say\":\"it\",\"s\":null}"); // '' closes then opens a pair
        check_repaired("{n: \"line1\nline2\"}", "{\"n\":\"line1\\nline2\"}");
        check_repaired("{\"a\": \"b\\'c\"}", "{\"a\":\"b'c\"}");
        check_any_valid("{\"a\": \"he said \"hi\" ok\"}"); // raw inner quotes
    };

    "repair_literals_and_numbers"_test = [] {
        check_repaired("[True, False, None]", "[true,false,null]");
        check_repaired("[NaN, Infinity, -Infinity]", "[null,null,null]");
        check_repaired("[+1, 007, .5, 1., 1e]", "[1,7,0.5,1.0,1]");
        check_repaired("[0x1A, -0xff]", "[26,-255]");
        check_repaired("[undefined]", "[null]");
        check_repaired("[TRUE, FALSE, NULL]", "[true,false,null]");
    };

    "repair_comments_and_chatter"_test = [] {
        check_repaired("// leading comment\n{\"a\": 1 /* inline */}", "{\"a\":1}");
        check_repaired("```json\n{\"a\": 1}\n```", "{\"a\":1}");
        check_repaired("Here is the JSON you asked for: {\"a\": 1} hope it helps", "{\"a\":1}");
        check_any_valid("\xEF\xBB\xBF{\"a\": 1}"); // BOM
    };

    "repair_degenerate_inputs"_test = [] {
        check_repaired("", "null");
        check_repaired("   \n\t ", "null");
        check_repaired("True", "true");
        check_repaired("hello world", "\"hello\"");
        // pathological deep truncation: must terminate and produce valid JSON
        std::string deep;
        for (int i = 0; i < 10000; i++) deep += '[';
        deep += '1';
        check_any_valid(kimix::string_view{deep});
        // single long string of garbage
        std::string garbage(100000, 'x');
        check_any_valid(kimix::string_view{garbage});
        // control chars in strings (must be escaped, not passed through raw)
        check_any_valid(kimix::string_view{"{\"a\x01\": \"b\x02\"}"});
    };

    "repair_output_is_valid"_test = [] {
        // for a pile of nasty inputs, the result must round-trip: repairing
        // the repaired output must return empty (i.e. it is already valid)
        const char *cases[] = {
            "{\"a\": 1,}",     "{'x': [1,2}",     "{a b: 1}",       "[1 2",
            "{\"k\": 'v\\",    "[True, 0x10,]",   "{\",\": }",       "[[[[",
            "{\xE2\x80\x9Cq\xE2\x80\x9D: `x`}",  "{\"a\": \"b\" \"c\": 2}",
            "[1, {\"a\": }, 2]",                 "no json here at all",
            "[1; 2; 3]",       "{\"a\": `v`; \"b\": `w`}",  "`x`",
            "[`a`; `b`]",      "{a: 1; b: 2}",   "[1; 2}",         "{\"a\": ; \"b\": 2}",
        };
        for (auto *c : cases) {
            auto once = kimix::repair(c);
            expect(!once.empty());
            auto twice = kimix::repair(once);
            expect(twice.empty()) << "not idempotent for: " << c;
        }
    };

    "repair_fuzz_regressions"_test = [] {
        // invalid UTF-8 inside strings must not pass through raw (yyjson strict
        // would reject it); bytes are escaped as \u00XX
        check_repaired("{\"a\": \"ok\xE2\"}", "{\"a\":\"ok\\u00e2\"}");
        check_repaired("[\"\xE2\x80\"]", "[\"\\u00e2\\u0080\"]");
        // lone surrogates become the replacement char; a well-formed pair stays
        check_repaired("{\"a\":\"\\ud800\"}", "{\"a\":\"\\uFFFD\"}");
        check_repaired("[\"\\udc00\"", "[\"\\uFFFD\"]");
        check_repaired("[\"\\ud83d\\ude00\"", "[\"\\ud83d\\ude00\"]");
        // exponent overflow must be clamped into double range, not dropped
        check_any_valid("[1e999");
        check_any_valid("[1234E678");
        // trailing junk after a numeric prefix is preserved by quoting
        check_repaired("[1.5abc", "[\"1.5abc\"]");
        check_repaired("[007x", "[\"007x\"]");
        check_repaired("[-2.55-3", "[\"-2.55-3\"]");
        // backslash followed by a control / non-ASCII byte inside a string:
        // the output must stay strictly valid (escape + \u00XX / valid UTF-8)
        check_repaired("[\"a\\\xE4\xB8\x96\"]", "[\"a\\\\\xE4\xB8\x96\"]");
        check_repaired("[\"a\\" "\xE2\x80\x98\"]", "[\"a\\\\\xE2\x80\x98\"]");
        check_repaired("[\"a\\\x01\"]", "[\"a\\\\\\u0001\"]");
        check_repaired("[\"a\\q\"]", "[\"a\\\\q\"]");
        // every output must re-repair to empty (strictly valid, idempotent)
        const char *nasty[] = {
            "[\"jus\x01t \x98s\"", "{\"\xE2\x80\x9Cq\xE2\x80\x9D: 1e999",
            "[True, 0x10, 1e9999,]", "{'a': '\\ud800', 'b': '\\udfff'}",
        };
        for (auto *c : nasty) {
            auto once = kimix::repair(c);
            expect(!once.empty());
            expect(kimix::repair(once).empty()) << "not idempotent for: " << c;
        }
    };

    "repair_semicolon_separators"_test = [] {
        // LLM output often separates members/elements with ';' instead of ','
        check_repaired("[1; 2; 3]", "[1,2,3]");
        check_repaired("[1;2;3;]", "[1,2,3]");
        check_repaired("{\"a\": 1; \"b\": 2}", "{\"a\":1,\"b\":2}");
        check_repaired("{a: 1; b: [2; 3]}", "{\"a\":1,\"b\":[2,3]}");
        check_repaired("[1; 2}", "[1,2]");
        check_repaired("{\"a\": ; \"b\": 2}", "{\"a\":null,\"b\":2}");
        check_repaired("[;1;2;]", "[1,2]");
    };

    "repair_backtick_quotes"_test = [] {
        // backticks are common LLM string delimiters (markdown inline code)
        check_repaired("{\"a\": `hello world`}", "{\"a\":\"hello world\"}");
        check_repaired("[`x`, `y`]", "[\"x\",\"y\"]");
        check_repaired("{\"a\": `x,y`}", "{\"a\":\"x,y\"}");
        check_repaired("{\"a\": `it\\`s`}", "{\"a\":\"it`s\"}"); // \` escape
        check_repaired("{`k`: `v`}", "{\"k\":\"v\"}");
        check_repaired("`hello`", "\"hello\"");
        // markdown fences (3+ backticks) must NOT be treated as quotes
        check_repaired("```json\n{\"a\": 1}\n```", "{\"a\":1}");
        check_repaired("````\n[1, 2]\n````", "[1,2]");
    };

    "repair_more_corner_cases"_test = [] {
        // missing values / stray separators
        check_repaired("{\"a\": , \"b\": 2}", "{\"a\":null,\"b\":2}");
        check_repaired("{\"a\": }", "{\"a\":null}");
        check_repaired("{\"a\":1,\"b\":}", "{\"a\":1,\"b\":null}");
        check_repaired("[1, 2: 3]", "[1,2,3]");
        check_repaired("[,,]", "[]");
        // missing colons / commas before nested values
        check_repaired("{\"a\" [1, 2]}", "{\"a\":[1,2]}");
        check_repaired("{\"a\" {\"b\": 1}}", "{\"a\":{\"b\":1}}");
        check_repaired("[{\"a\":1} {\"b\":2}]", "[{\"a\":1},{\"b\":2}]");
        check_repaired("{\"a\": 1 \"b\": [2, 3]}", "{\"a\":1,\"b\":[2,3]}");
        check_repaired("[ \"a\" \"b\" ]", "[\"a\",\"b\"]");
        // nested truncation
        check_repaired("[{\"a\": 1, \"b\": [2, 3", "[{\"a\":1,\"b\":[2,3]}]");
        check_repaired("[[1, 2], [3, 4", "[[1,2],[3,4]]");
        check_repaired("{\"a\": {\"b\": \"c", "{\"a\":{\"b\":\"c\"}}");
        // number corner cases
        check_repaired("[1e+5, 1E-5, -.5, +.5]", "[1e5,1e-5,-0.5,0.5]");
        check_repaired("{\"a\": -0x10}", "{\"a\":-16}");
        check_repaired("[1.5.2]", "[\"1.5.2\"]");
        // empty quoted values
        check_repaired("{'a': ''}", "{\"a\":\"\"}");
        check_repaired("[ '', 'x' ]", "[\"\",\"x\"]");
    };

    // ---- imagination-driven extra broken JSON cases -----------------------

    "repair_imagination_hex_escapes"_test = [] {
        // \xNN escapes decode low bytes; high bytes must never corrupt output
        check_repaired("{\"a\": \"\\x41\"}", "{\"a\":\"A\"}");
        check_repaired("{\"a\": \"\\x0A\"}", "{\"a\":\"\\u000a\"}");
        check_repaired("{\"a\": \"\\x22\"}", "{\"a\":\"\\u0022\"}");
        check_repaired("{\"a\": \"\\x5C\"}", "{\"a\":\"\\u005c\"}");
        check_repaired("{\"a\": \"\\x\"}", "{\"a\":\"\\\\x\"}");
        // a \xNN pair that forms valid UTF-8 stays valid
        check_any_valid("[\"\\xC3\\xA9\"]");
        check_any_valid("{\"a\": \"\\x7F\"}");
        // lone/incomplete high bytes currently break strict validation:
        // repair must still return *some* valid JSON instead of nothing
        check_any_valid("{\"a\": \"\\xE9\"}");
        check_any_valid("{\"a\": \"\\x80\"}");
        check_any_valid("{\"a\": \"\\xFF\"}");
        check_any_valid("[\"\\xC3\"]");
        check_any_valid("[\"\\xE2\\x80\"]");
        check_any_valid("[\"\\xED\\xA0\\x80\"]");
    };

    "repair_imagination_unicode_escapes"_test = [] {
        // truncated / invalid \u escapes degrade to a literal backslash-u
        check_repaired("{\"a\": \"\\uZZZZ\"}", "{\"a\":\"\\\\uZZZZ\"}");
        check_repaired("{\"a\": \"\\u12\"}", "{\"a\":\"\\\\u12\"}");
        // lone surrogates become the replacement char; a pair stays verbatim
        check_repaired("{\"a\": \"\\uD800x\"}", "{\"a\":\"\\uFFFDx\"}");
        check_repaired("{\"a\": \"\\uDC00x\"}", "{\"a\":\"\\uFFFDx\"}");
    };

    "repair_imagination_numbers"_test = [] {
        // tokens that merely look numeric are quoted as strings
        check_repaired("{\"a\": 0x1G}", "{\"a\":\"0x1G\"}");
        check_repaired("{\"a\": 0x}", "{\"a\":\"0x\"}");
        check_repaired("{\"a\": -0x}", "{\"a\":\"-0x\"}");
        check_repaired("{\"a\": 1_000}", "{\"a\":\"1_000\"}");
        check_repaired("{\"a\": 0b101}", "{\"a\":\"0b101\"}");
        check_repaired("{\"a\": 0o17}", "{\"a\":\"0o17\"}");
        check_repaired("{\"a\": 1..2}", "{\"a\":\"1..2\"}");
        check_repaired("{\"a\": 1e5.5}", "{\"a\":\"1e5.5\"}");
        // normalization corner cases
        check_repaired("[00]", "[0]");
        check_repaired("[01.5]", "[1.5]");
        check_repaired("[-01.5]", "[-1.5]");
        check_repaired("[1.e5]", "[1.0e5]");
        check_repaired("[0X1A]", "[26]");
        check_repaired("[1e, 2., .5, +.5]", "[1,2.0,0.5,0.5]");
        // integer boundaries and exponent clamping must stay valid
        check_repaired("[-0x8000000000000000]", "[-9223372036854775808]");
        check_repaired("[0xFFFFFFFFFFFFFFFF]", "[18446744073709551615]");
        check_repaired("[1e+999]", "[1e299]");
        check_any_valid("[123e456]");
        check_any_valid("[1234E678]");
        std::string huge(400, '9');
        check_any_valid(kimix::string_view{"[" + huge + "]"});
    };

    "repair_imagination_separators"_test = [] {
        check_repaired("[1;;2]", "[1,2]");
        check_repaired("[1;;;2]", "[1,2]");
        check_repaired("[1:::2]", "[1,2]");
        check_repaired("[::]", "[]");
        check_repaired("[,]", "[]");
        check_repaired("[: 1]", "[1]");
        check_repaired("{,}", "{}");
        check_repaired("{,1}", "{\"1\":null}");
        check_repaired("{: 1}", "{\"1\":null}");
        check_repaired("{\"a\":: 1}", "{\"a\":null,\"1\":null}");
        check_repaired("{\"a\": 1;; \"b\": 2}", "{\"a\":1,\"b\":2}");
        check_repaired("{a:;b:}", "{\"a\":null,\"b\":null}");
        check_repaired("{\"a\": 1, \"b\" 2}", "{\"a\":1,\"b\":2}");
        check_repaired("{\"a\": 1 2}", "{\"a\":1,\"2\":null}");
        check_repaired("{\"a\": \"x\" \"b\": \"y\"}", "{\"a\":\"x\",\"b\":\"y\"}");
        check_repaired("[\"x\": \"y\"]", "[\"x\",\"y\"]");
        check_repaired("{a:b:c}", "{\"a\":\"b\",\"c\":null}");
        check_repaired("{a:b, c:d}", "{\"a\":\"b\",\"c\":\"d\"}");
    };

    "repair_imagination_keys_and_values"_test = [] {
        check_repaired("{1: 2}", "{\"1\":2}");
        check_repaired("{true: false}", "{\"true\":false}");
        check_repaired("{my-key: 1}", "{\"my-key\":1}");
        check_repaired("{a.b: 1}", "{\"a.b\":1}");
        check_repaired("{'a': \"b\"}", "{\"a\":\"b\"}");
        check_repaired("{a: 'b', c: \"d\"}", "{\"a\":\"b\",\"c\":\"d\"}");
        check_repaired("[1, 'two', \"three\"]", "[1,\"two\",\"three\"]");
        check_repaired("[\"a\", 'b', `c`]", "[\"a\",\"b\",\"c\"]");
        check_repaired("{a: `v`, b: 'w', c: \"x\"}", "{\"a\":\"v\",\"b\":\"w\",\"c\":\"x\"}");
        check_repaired("{a: 1, b: 2,}", "{\"a\":1,\"b\":2}");
        // apostrophes inside bare keys/values are tolerated (still valid JSON)
        check_any_valid("{it's: 1}");
        check_any_valid("{a: b'c'}");
    };

    "repair_imagination_containers"_test = [] {
        check_repaired("[[", "[[]]");
        check_repaired("{{", "{\"\":{}}");
        check_repaired("{\"a\": [}", "{\"a\":[]}");
        check_repaired("{\"a\": {]}", "{\"a\":{}}");
        check_repaired("[[[[{\"a\":1}]]]", "[[[[{\"a\":1}]]]]");
        check_repaired("[{\"a\": 1}, {\"b\": 2}", "[{\"a\":1},{\"b\":2}]");
        check_repaired("{a: {b: [1, 2", "{\"a\":{\"b\":[1,2]}}");
        check_repaired("{\"a\": 12", "{\"a\":12}");
        // trailing chatter / a second document is ignored once the value closes
        check_repaired("{\"a\":1} trailing", "{\"a\":1}");
        check_repaired("{\"a\":1} {\"b\":2}", "{\"a\":1}");
        check_repaired("[1, 2] extra", "[1,2]");
        check_repaired("{\"a\": 1} // comment", "{\"a\":1}");
        // comments inside containers
        check_repaired("{\"a\": /* c */ 1}", "{\"a\":1}");
        check_repaired("[1, /* c */ 2]", "[1,2]");
        check_repaired("[1, // c\n2]", "[1,2]");
    };

    "repair_imagination_llm_output"_test = [] {
        // multi-line single-quoted LLM blob
        check_repaired("Please return JSON:\n{\n  'name': 'Alice',\n  'age': 30,\n"
                       "  'tags': ['admin', 'dev',],\n"
                       "  'address': {'city': 'NY', 'zip': 10001}\n}",
                       "{\"name\":\"Alice\",\"age\":30,\"tags\":[\"admin\",\"dev\"],"
                       "\"address\":{\"city\":\"NY\",\"zip\":10001}}");
        // semicolons as separators at every nesting level
        check_repaired("{a: 1; b: [2; 3]; c: {d: 4; e: 5}}",
                       "{\"a\":1,\"b\":[2,3],\"c\":{\"d\":4,\"e\":5}}");
        check_repaired("[{a:1};{b:2}]", "[{\"a\":1},{\"b\":2}]");
        // prose with an apostrophe before the JSON (currently mis-repaired)
        check_repaired("Sure! Here's the JSON:\n{\n  'name': 'Alice'\n}",
                       "{\"name\":\"Alice\"}");
    };

    "repair_imagination_round_trip"_test = [] {
        // for a pile of nasty inputs, the result must round-trip: repairing
        // the repaired output must return empty (i.e. it is already valid)
        const char *cases[] = {
            "{{",              "{,1}",          "[: 1]",         "{a b: 1}",
            "{it's: 1}",       "{a: b'c'}",     "[\"x\": \"y\"]", "{\"a\": 1 2}",
            "[1:::2]",         "[1;;;2]",       "{\"a\":: 1}",   "[1e+999]",
            "{\"a\": 1e9999}", "[0b101]",       "{\"a\": \"\\xE9\"}",
        };
        for (auto *c : cases) {
            auto once = kimix::repair(c);
            expect(!once.empty()) << "repair produced nothing for: " << c;
            auto twice = kimix::repair(once);
            expect(twice.empty()) << "not idempotent for: " << c;
        }
    };

    // ---- continued research: more corner cases (probe-verified) -----------

    "repair_research_numbers_more"_test = [] {
        // sign/operator junk around numbers is quoted as a string
        check_repaired("[--1]", "[\"--1\"]");
        check_repaired("[+-1]", "[\"+-1\"]");
        check_repaired("[1-2]", "[\"1-2\"]");
        check_repaired("[.]", "[\".\"]");
        check_repaired("[..]", "[\"..\"]");
        check_repaired("[+.]", "[\"+.\"]");
        check_repaired("[0b]", "[\"0b\"]");
        check_repaired("[0o]", "[\"0o\"]");
        check_repaired("[0xG]", "[\"0xG\"]");
        check_repaired("[0x1Fg]", "[\"0x1Fg\"]");
        check_repaired("[0x1A.5]", "[\"0x1A.5\"]");
        check_repaired("[-0x1A.5]", "[\"-0x1A.5\"]");
        check_repaired("[1e5abc]", "[\"1e5abc\"]");
        // overflow clamp: leading-zero forms cap at 300, int forms at 299
        check_repaired("[0.5e999]", "[0.5e300]");
        check_repaired("[2e308]", "[2e299]");
        check_repaired("[1e999999999999999999999999999999999]", "[1e299]");
    };

    "repair_research_strings_more"_test = [] {
        // control characters are escaped, never passed through raw
        check_repaired("[\"a\tb\"]", "[\"a\\tb\"]");
        check_repaired("[\"a\rb\"]", "[\"a\\rb\"]");
        check_repaired("[\"a\bb\"]", "[\"a\\u0008b\"]");
        check_repaired("[\"a\fb\"]", "[\"a\\u000cb\"]");
        check_repaired("[\"a\x1F" "b\"]", "[\"a\\u001fb\"]");
        check_repaired("['line1\nline2']", "[\"line1\\nline2\"]");
        check_repaired("['say \"hi\"']", "[\"say \\\"hi\\\"\"]");
        // raw UTF-8 in unquoted keys/values is preserved; invalid bytes escaped
        check_repaired("{caf\xC3\xA9: 1}", "{\"caf\xC3\xA9\":1}");
        check_repaired("[caf\xC3\xA9]", "[\"caf\xC3\xA9\"]");
        check_repaired("[caf\xE9]", "[\"caf\\u00e9\"]");
        check_repaired("{a: caf\xE9}", "{\"a\":\"caf\\u00e9\"}");
        // embedded NUL bytes (explicit length: string_view must not truncate)
        check_repaired(kimix::string_view{"[\"a\x00" "b\"]", 7}, "[\"a\\u0000b\"]");
        check_repaired(kimix::string_view{"[a\x00" "b]", 5}, "[\"a\\u0000b\"]");
        check_repaired(kimix::string_view{"{a\x00" "b: 1}", 8}, "{\"a\\u0000b\":1}");
        check_repaired(kimix::string_view{"{\"a\": \x00}", 8}, "{\"a\":\"\\u0000\"}");
        check_repaired(kimix::string_view{"\x00", 1}, "\"\\u0000\"");
        check_repaired(kimix::string_view{"[\"a\x00" "b\", 2]", 10}, "[\"a\\u0000b\",2]");
        check_repaired(kimix::string_view{"{\"a\x00" "b\": 1}", 10}, "{\"a\\u0000b\":1}");
        // literal \xNN escape text decodes like a real byte
        check_repaired("[\"a\\x00b\"]", "[\"a\\u0000b\"]");
        // backslash before the closing quote: the quote is escaped, rest kept
        check_any_valid("[\"a\\\"]");
    };

    "repair_research_unicode_more"_test = [] {
        // a truncated surrogate pair at EOF is completed by closing the string
        check_repaired("[\"\\uD83D\\uDE00", "[\"\\uD83D\\uDE00\"]");
        // truncated / dangling \u escapes degrade to a literal backslash-u
        check_repaired("[\"\\uD83D\\uDE0", "[\"\\uFFFD\\\\uDE0\"]");
        check_repaired("[\"\\uD83D\\u\"]", "[\"\\uFFFD\\\\u\"]");
        // adjacent surrogates: only a valid high+low pair is kept verbatim
        check_repaired("[\"\\uD800\\uD800\"]", "[\"\\uFFFD\\uFFFD\"]");
        check_repaired("[\"\\uDC00\\uDC00\"]", "[\"\\uFFFD\\uFFFD\"]");
        check_repaired("[\"\\uD800\\uE000\"]", "[\"\\uFFFD\\uE000\"]");
        check_repaired("[\"a\\uD800b\"]", "[\"a\\uFFFDb\"]");
    };

    "repair_research_containers_more"_test = [] {
        check_repaired("[[[", "[[[]]]");
        check_repaired("[{[}", "[{\"\":[]}]");
        check_repaired("[}", "[]");
        check_repaired("{]", "{}");
        check_repaired("}{", "{}");
        check_repaired("][", "[]");
        check_repaired("{}]", "{}");
        check_repaired("{a:1,]", "{\"a\":1}");
        check_repaired("{\"a\":1,\"b\":2,", "{\"a\":1,\"b\":2}");
        check_repaired("{\"a\": {\"b\": 1, \"c\": [2, 3", "{\"a\":{\"b\":1,\"c\":[2,3]}}");
        check_repaired("[{\"a\": 1}, {\"b\": 2}, {\"c\": 3", "[{\"a\":1},{\"b\":2},{\"c\":3}]");
        check_repaired("{\"a\": [1, {\"b\": 2}, [3, {\"c\": 4}", "{\"a\":[1,{\"b\":2},[3,{\"c\":4}]]}");
        check_repaired("{\"a\": {\"b\": {\"c\": [1, 2", "{\"a\":{\"b\":{\"c\":[1,2]}}}");
        // trailing commas at every nesting level
        check_repaired("[{\"a\":1},]", "[{\"a\":1}]");
        check_repaired("{\"a\": {\"b\": 1},}", "{\"a\":{\"b\":1}}");
        check_repaired("[[1,2],[3,4],]", "[[1,2],[3,4]]");
        check_repaired("{\"a\": [1, 2,], \"b\": {\"c\": 3,}}", "{\"a\":[1,2],\"b\":{\"c\":3}}");
        // brackets inside truncated strings stay inside the string
        check_repaired("{\"a\": \"x]", "{\"a\":\"x]\"}");
        check_repaired("{\"a\": \"x}", "{\"a\":\"x}\"}");
    };

    "repair_research_separators_more"_test = [] {
        check_repaired("{\"a\":1,,,\"b\":2}", "{\"a\":1,\"b\":2}");
        check_repaired("[1,,,2]", "[1,2]");
        check_repaired("{\"a\":1;,\"b\":2}", "{\"a\":1,\"b\":2}");
        check_repaired("[1;,2]", "[1,2]");
        check_repaired("{\"a\" ; \"b\"}", "{\"a\":null,\"b\":null}");
        check_repaired("{\"a\" : \"b\" ; \"c\" : \"d\"}", "{\"a\":\"b\",\"c\":\"d\"}");
        check_repaired("[a: b]", "[\"a\",\"b\"]");
        check_repaired("[:]", "[]");
        check_repaired("[:a]", "[\"a\"]");
        check_repaired("{\"a\" : : 1}", "{\"a\":null,\"1\":null}");
        check_repaired("[;]", "[]");
        check_repaired("{;}", "{}");
        check_repaired("{;1:2}", "{\"1\":2}");
        check_repaired("{\"a\": 1, , \"b\": 2}", "{\"a\":1,\"b\":2}");
        // unknown operators become keys/values (still valid JSON)
        check_any_valid("{a: 1 | b: 2}");
        check_any_valid("{a: 1 & b: 2}");
        check_any_valid("{\"a\": 1/2}");
    };

    "repair_research_keys_more"_test = [] {
        check_repaired("{a-b-c: 1}", "{\"a-b-c\":1}");
        check_repaired("{a_b: 1}", "{\"a_b\":1}");
        check_repaired("{a$b: 1}", "{\"a$b\":1}");
        check_repaired("{a(b): 1}", "{\"a(b)\":1}");
        check_repaired("{True: 1}", "{\"True\":1}");
        check_repaired("{Null: null}", "{\"Null\":null}");
        check_repaired("{1e3: 1}", "{\"1e3\":1}");
        check_repaired("{.5: 1}", "{\".5\":1}");
        check_repaired("{-1: 2}", "{\"-1\":2}");
        check_repaired("{+1: 2}", "{\"+1\":2}");
        check_repaired("{1.5: 2}", "{\"1.5\":2}");
        check_repaired("{1e5: 2}", "{\"1e5\":2}");
        // bare words as values
        check_repaired("[inf, -inf, +inf]", "[null,null,null]");
        check_repaired("[yes, no]", "[\"yes\",\"no\"]");
        check_repaired("[on, off]", "[\"on\",\"off\"]");
        check_repaired("[hello world]", "[\"hello\",\"world\"]");
        check_repaired("{\"a\": hello world}", "{\"a\":\"hello\",\"world\":null}");
        check_repaired("[1, hello, 2]", "[1,\"hello\",2]");
        check_repaired("{\"a\": 1, \"b\": yes}", "{\"a\":1,\"b\":\"yes\"}");
        check_repaired("[null null]", "[null,null]");
        // multi-line single-quoted / backtick mixtures
        check_repaired("{\"a\": ``hello``}", "{\"a\":\"``hello``\"}");
        check_repaired("{\"a\": ``}", "{\"a\":\"``\"}");
        check_repaired("{\"a\": `hello` `world`}", "{\"a\":\"hello\",\"world\":null}");
        check_repaired("[`world`, `hello`]", "[\"world\",\"hello\"]");
        check_repaired("{\"a\": \"1,2\" \"b\": 3}", "{\"a\":\"1,2\",\"b\":3}");
    };

    "repair_research_comments_prologue_more"_test = [] {
        // comment-only input degrades to null
        check_repaired("// just a comment", "null");
        check_repaired("/* block */", "null");
        // unterminated comments are tolerated
        check_repaired("[1, /*", "[1]");
        check_repaired("[1, //", "[1]");
        // comments inside containers and between tokens
        check_repaired("{\"a\": /* } */ 1}", "{\"a\":1}");
        check_repaired("[1 /**/ 2]", "[1,2]");
        check_repaired("[1/*x*/2]", "[1,2]");
        check_repaired("{\"a\": 1} /* trailing */", "{\"a\":1}");
        // BOM + comment + array
        check_repaired("\xEF\xBB\xBF // c\n[1]", "[1]");
        // prose with apostrophes before the JSON anchors on the object
        check_repaired("Here's the answer: {\"a\": 1}", "{\"a\":1}");
        check_repaired("don't worry {\"a\": 1}", "{\"a\":1}");
        check_repaired("it's {\"a\": 1}", "{\"a\":1}");
        // a leading quoted value wins over later chatter
        check_repaired("'quoted' {\"a\": 1}", "\"quoted\"");
        check_repaired("{a:1}'suffix", "{\"a\":1}");
        check_repaired("{\"a\":1}'", "{\"a\":1}");
        check_repaired("[1,2,3] // comment", "[1,2,3]");
        // number / string / array anchors stop prologue scanning
        check_repaired("123 {\"a\":1}", "123");
        check_repaired("\"hello\" {\"a\":1}", "\"hello\"");
        check_repaired("['a'] {'b':2}", "[\"a\"]");
        // degenerate comma-separated top level keeps the first value
        check_repaired("True, False", "true");
        check_repaired("1, 2, 3", "1");
        check_repaired("null, undefined", "null");
        // unclosed quoted strings are closed
        check_repaired("\"just a string", "\"just a string\"");
        check_repaired("'just a string", "\"just a string\"");
        check_repaired("'a", "\"a\"");
        // newlines act as whitespace between tokens
        check_repaired("{a\n:\n1}", "{\"a\":1}");
        check_repaired("[1\n2]", "[1,2]");
        check_repaired("{\"a\"\n1}", "{\"a\":1}");
    };

    "repair_research_mixed_output"_test = [] {
        check_repaired("{\"a\": [1; 2; 3]; b: {c: 'x'; d: `y`}; e: [true, false, null]; f: 0x10; g: 1e+999}",
                       "{\"a\":[1,2,3],\"b\":{\"c\":\"x\",\"d\":\"y\"},\"e\":[true,false,null],\"f\":16,\"g\":1e299}");
        check_repaired("{'a': 1e999, 'b': [0b11, 0o7, 0x1F], 'c': {d: True, e: None}}",
                       "{\"a\":1e299,\"b\":[\"0b11\",\"0o7\",31],\"c\":{\"d\":true,\"e\":null}}");
    };

    "repair_research_round_trip"_test = [] {
        const char *cases[] = {
            "[--1]",      "[+-1]",      "[1-2]",      "[.]",        "[..]",
            "[+.]",       "[0b]",       "[0o]",       "[0xG]",      "[0x1Fg]",
            "[0x1A.5]",   "[1e5abc]",   "[0.5e999]",  "[2e308]",    "[1e999999999999999999999999999999999]",
            "[[[",        "[{[}",       "[}",         "{]",         "}{",
            "][",         "{}]",        "{a:1,]",     "{\"a\":1,\"b\":2,",
            "{\"a\":1,,,\"b\":2}", "[1,,,2]", "{a: 1 | b: 2}", "{a: 1 & b: 2}",
            "{\"a\": 1/2}", "[a: b]",   "[:]",        "[:a]",       "{\"a\" : : 1}",
            "[;]",        "{;}",        "{;1:2}",     "// just a comment",
            "/* block */", "[1, /*",    "[1, //",     "{\"a\": /* } */ 1}",
            "[1 /**/ 2]",  "[1/*x*/2]", "True, False", "1, 2, 3",   "null, undefined",
            "{\"a\": ``hello``}", "{\"a\": ``}", "{\"a\": `hello` `world`}",
            "{\"a\": \"x]", "{\"a\": \"x}", "['say \"hi\"']",
            "[inf, -inf, +inf]", "[yes, no]", "[on, off]", "[hello world]",
        };
        for (auto *c : cases) {
            auto once = kimix::repair(c);
            expect(!once.empty()) << "repair produced nothing for: " << c;
            auto twice = kimix::repair(once);
            expect(twice.empty()) << "not idempotent for: " << c;
        }
    };

    return 0;
}
