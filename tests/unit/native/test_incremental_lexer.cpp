// Test for src/runtime/json/incremental_lexer.h (plan 010).
// This test covers:
// - split-point property: feed byte-by-byte == 4KB chunks == one-shot
// - completeness: every proper prefix not complete; full document complete
// - value spans for top-level keys + key order (document order)
// - relaxed parsing: trailing commas, // and /* */ comments
// - errors: {"a": }, unmatched close, bad escape, invalid number/literal,
//   second top-level value after completion
// - string escapes incl. \uXXXX + surrogate pair split mid-escape
// - reset()

#include "ut/ut.hpp"
#include <runtime/json/incremental_lexer.h>

#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::json;

namespace {

// Feed `doc` in the given split pattern; returns the lexer state.
void feed_split(IncrementalJsonLexer& lex, kimix::string_view doc,
                size_t chunk_size) {
    for (size_t i = 0; i < doc.size(); i += chunk_size) {
        const size_t n = (chunk_size == 0)
                             ? 1
                             : (chunk_size < doc.size() - i ? chunk_size
                                                            : doc.size() - i);
        lex.feed(doc.substr(i, n));
    }
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "lexer_split_point_property"_test = [] {
        const char* docs[] = {
            R"({"a": 1, "b": [1, 2, 3], "c": {"d": "x"}, "e": true})",
            R"([{"k": "v"}, [1, 2], {"n": null}])",
            R"({"s": "a string with \"escapes\" and \u4e16\u754c"})",
            R"({"a": {"b": {"c": [1, [2, [3]]]}}})",
            R"(true)",
            R"("just a string")",
        };
        for (const char* doc : docs) {
            // one-shot
            IncrementalJsonLexer once;
            once.feed(doc);
            // byte-by-byte
            IncrementalJsonLexer b2b;
            feed_split(b2b, doc, 1);
            // 4KB chunks (one chunk for these short docs)
            IncrementalJsonLexer big;
            feed_split(big, doc, 4096);
            expect(eq(once.is_complete(), b2b.is_complete())) << doc;
            expect(eq(once.has_error(), b2b.has_error())) << doc;
            expect(eq(once.is_complete(), big.is_complete())) << doc;
            auto once_keys = once.top_level_keys();
            auto b2b_keys = b2b.top_level_keys();
            expect(eq(once_keys.size(), b2b_keys.size())) << doc;
            if (once_keys.size() == b2b_keys.size()) {
                bool keys_equal = true;
                for (size_t i = 0; i < once_keys.size(); ++i) {
                    if (once_keys[i] != b2b_keys[i]) {
                        keys_equal = false;
                    }
                }
                expect(keys_equal) << "keys must match: " << doc;
            }
            expect(eq(once.spans().size(), b2b.spans().size())) << doc;
            if (once.spans().size() == b2b.spans().size()) {
                for (size_t i = 0; i < once.spans().size(); ++i) {
                    expect(eq(once.spans()[i].start, b2b.spans()[i].start)) << doc;
                    expect(eq(once.spans()[i].end, b2b.spans()[i].end)) << doc;
                }
            }
            expect(once.is_complete()) << "full document must be complete: " << doc;
            expect(!once.has_error()) << doc;
        }
    };

    "lexer_bare_number_streaming_documented"_test = [] {
        // Numbers terminate ONLY on a delimiter (never at a feed boundary),
        // so chunked feeding is byte-identical to one-shot feeding. A bare
        // top-level number therefore stays incomplete until a delimiter
        // arrives; `42 ` (with trailing whitespace) is complete.
        IncrementalJsonLexer once;
        once.feed("42");
        expect(!once.is_complete()) << "bare number without delimiter is pending";
        expect(!once.has_error());
        once.feed(" ");
        expect(once.is_complete());
        expect(!once.has_error());

        // Byte-by-byte matches one-shot exactly (no divergence).
        IncrementalJsonLexer b2b;
        b2b.feed("4");
        expect(!b2b.is_complete());
        b2b.feed("2");
        expect(!b2b.is_complete());
        b2b.feed(" ");
        expect(b2b.is_complete());
        expect(!b2b.has_error());

        // A number inside an object completes at the closing brace.
        IncrementalJsonLexer obj;
        obj.feed(R"({"a": 3.5})");
        expect(obj.is_complete());
        expect(!obj.has_error());
    };

    "lexer_incomplete_at_every_prefix"_test = [] {
        const char* doc = R"({"a": 1, "b": [1, 2]})";
        IncrementalJsonLexer lex;
        const size_t n = std::strlen(doc);
        for (size_t i = 0; i < n - 1; ++i) {
            lex.feed(kimix::string_view(doc + i, 1));
            expect(!lex.is_complete())
                << "prefix of length " << (i + 1) << " must be incomplete";
            expect(!lex.has_error()) << "prefix of length " << (i + 1);
        }
        lex.feed(kimix::string_view(doc + n - 1, 1));
        expect(lex.is_complete());
        expect(!lex.has_error());
    };

    "lexer_value_spans_and_keys"_test = [] {
        IncrementalJsonLexer lex;
        lex.feed(R"({"alpha": 1, "beta": [10, 20], "gamma": {"deep": true}})");
        expect(lex.is_complete());
        expect(!lex.has_error());
        auto keys = lex.top_level_keys();
        expect(eq(keys.size(), 3u));
        expect(eq(keys[0], kimix::string("alpha")));
        expect(eq(keys[1], kimix::string("beta")));
        expect(eq(keys[2], kimix::string("gamma")));

        size_t s = 0, e = 0;
        expect(lex.value_span("alpha", s, e));
        // The span points at the raw value bytes in the buffer.
        expect(e > s);
        expect(lex.value_span("beta", s, e));
        expect(e > s);
        expect(lex.value_span("gamma", s, e));
        expect(e > s);
        // Unknown key -> false.
        expect(!lex.value_span("nope", s, e));
    };

    "lexer_relaxed_trailing_commas"_test = [] {
        IncrementalJsonLexer lex;
        lex.feed(R"([1, 2, 3,])");
        expect(lex.is_complete());
        expect(!lex.has_error());
        lex.reset();

        lex.feed(R"({"a": 1, "b": 2,})");
        expect(lex.is_complete());
        expect(!lex.has_error());
        lex.reset();

        // Nested trailing comma.
        lex.feed(R"({"a": [1, 2,], "b": {"c": 3,},})");
        expect(lex.is_complete());
        expect(!lex.has_error());
        lex.reset();

        // Comma without a value is still an error.
        lex.feed(R"([,])");
        expect(lex.has_error());
        lex.reset();
        lex.feed(R"({"a":,})");
        expect(lex.has_error());
    };

    "lexer_relaxed_comments"_test = [] {
        IncrementalJsonLexer lex;
        lex.feed(R"({"a": 1, // line comment
                     "b": 2})");
        expect(lex.is_complete());
        expect(!lex.has_error());
        lex.reset();

        lex.feed(R"({"a": /* block */ 1})");
        expect(lex.is_complete());
        expect(!lex.has_error());
        lex.reset();

        // Comment before the value.
        lex.feed(R"({"a": /*x*/ 1, "b": //y
                    2})");
        expect(lex.is_complete());
        expect(!lex.has_error());
        lex.reset();

        // '//' text inside a string is NOT a comment.
        lex.feed(R"({"url": "http://example.com"})");
        expect(lex.is_complete());
        expect(!lex.has_error());
    };

    "lexer_errors"_test = [] {
        // Truly malformed docs (error).
        const char* bad[] = {
            R"({"a": })",    // missing value
            R"({"a" 1})",    // value where a colon was expected
            R"({"a":1",})",  // string where a comma was expected
            R"([1 2])",       // value without comma
            R"({"a":1 "b":2})",
            R"({"a":1,}{"b":2})", // second top-level value
            R"("a\x")",      // bad escape inside a string
            R"(truee)",       // literal then extra char
            R"(})",           // stray close -> error (no open)
        };
        // Docs that are INCOMPLETE at EOF (valid prefixes of longer docs).
        const char* incomplete[] = {
            R"({"a": )", R"("unterminated)", R"(tru)", R"({)", R"([)",
            R"({"a":1)", R"(42.)", R"(1e+)",
        };
        for (const char* doc : incomplete) {
            IncrementalJsonLexer lex;
            lex.feed(doc);
            expect(!lex.has_error()) << doc << " (incomplete)";
            expect(!lex.is_complete()) << doc << " (incomplete)";
        }
        // Truly malformed docs.
        for (const char* doc : bad) {
            IncrementalJsonLexer lex;
            lex.feed(doc);
            expect(lex.has_error()) << doc;
        }
        // Error is sticky: further feeds change nothing.
        IncrementalJsonLexer lex;
        lex.feed(R"({"a": })");
        expect(lex.has_error());
        lex.feed(R"({"a": 1})");
        expect(lex.has_error());
        expect(!lex.is_complete());
    };

    "lexer_string_escapes_and_surrogate_split"_test = [] {
        // All standard escapes in one string.
        IncrementalJsonLexer lex;
        lex.feed(R"({"s": "\"\\\/\b\f\n\r\t\u4e16\u754c"})");
        expect(lex.is_complete());
        expect(!lex.has_error());
        lex.reset();

        // Surrogate pair \uD83D\uDE00 split across feeds (mid-escape).
        const char* doc = R"({"emoji": "\uD83D\uDE00"})";
        IncrementalJsonLexer one;
        one.feed(doc);
        expect(one.is_complete());
        expect(!one.has_error());

        IncrementalJsonLexer split;
        for (size_t i = 0; i < std::strlen(doc); ++i) {
            split.feed(kimix::string_view(doc + i, 1));
        }
        expect(!split.has_error()) << "split surrogate feeds must not error";
        expect(split.is_complete());
        expect(eq(one.spans().size(), split.spans().size()));
        expect(eq(one.spans()[0].start, split.spans()[0].start));

        // Bad escape -> error.
        IncrementalJsonLexer bad;
        bad.feed(R"({"a": "\x"})");
        expect(bad.has_error());
    };

    "lexer_scalars_and_whitespace"_test = [] {
        IncrementalJsonLexer lex;
        lex.feed("  \t\r\n 42 ");
        expect(lex.is_complete());
        expect(!lex.has_error());
        lex.reset();

        lex.feed("null");
        expect(lex.is_complete());
        lex.reset();

        lex.feed("-12.5e3 ");
        expect(lex.is_complete());
        lex.reset();

        // Negative exponent form (terminated by the closing brace).
        lex.feed(R"({"x": -1.5E-10})");
        expect(lex.is_complete());
        lex.reset();

        // Whitespace + comment after a complete value is fine.
        lex.feed(R"({"a":1} // done
)");
        expect(lex.is_complete());
        expect(!lex.has_error());
    };

    "lexer_reset"_test = [] {
        IncrementalJsonLexer lex;
        lex.feed(R"({"a": 1})");
        expect(lex.is_complete());
        lex.reset();
        expect(!lex.is_complete());
        expect(!lex.has_error());
        expect(lex.top_level_keys().empty());
        size_t s = 0, e = 0;
        expect(!lex.value_span("a", s, e));
        // Can parse a different document after reset.
        lex.feed("[1, 2]");
        expect(lex.is_complete());
        expect(!lex.has_error());
        expect(lex.top_level_keys().empty());
    };

    "lexer_top_level_keys_of_non_object"_test = [] {
        IncrementalJsonLexer lex;
        lex.feed("[1, 2, 3]");
        expect(lex.is_complete());
        expect(lex.top_level_keys().empty());
        size_t s = 0, e = 0;
        expect(!lex.value_span("anything", s, e));
    };

    return 0;
}
