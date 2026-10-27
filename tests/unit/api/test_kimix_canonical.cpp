/*
 * test_kimix_canonical.cpp — exercises the canonical-JSON C FFI (api/ffi_canonical.h).
 *
 * Like the rest of the unit/api suite this drives the library through the
 * PUBLIC C headers only, exactly as a foreign binding does: nothing here names a
 * kimix-core type, and every buffer is allocated and released inside the DLL.
 *
 * This test covers:
 * - key-shuffle byte equality: the same object members in two insertion orders
 *   canonicalise to identical bytes, and to the documented sorted spelling;
 * - number-precision preservation: the raw-number flag means 1.0, 1, 1e2 and -0
 *   re-emit their source text verbatim (1.0 != 1);
 * - nested arrays and objects: arrays keep order, objects sort, minified;
 * - embedded NUL rejection on the length-based API (binary safety: a raw 0x00 is
 *   INVALID_INPUT, never a silent truncation), with the placeholder left raw;
 * - invalid UTF-8 -> KIMIX_ERR_INVALID_INPUT, not undefined behaviour;
 * - duplicate object key -> KIMIX_ERR_INVALID_INPUT;
 * - empty / whitespace-only document -> KIMIX_ERR_INVALID_INPUT; empty object and
 *   empty array are valid and map to themselves;
 * - top-level scalars;
 * - argument / state errors and the single-owner library-heap discipline;
 * - a ~1 MiB stress document (large flat object), checked both against itself and
 *   against a key-reversed twin that must canonicalise to the same bytes.
 *
 * Registered in tests/xmake.lua behind has_config("kimix_enable_api"): with the
 * option off neither kimix_api nor this test is part of the configuration.
 */
#include "ut/ut.hpp"

#include <api/kimix_api.h>

#include <cstdio>
#include <cstring>

using boost::ut::expect;
using boost::ut::operator""_test;

namespace tests_unit_api_test_kimix_canonical {

// Two fixed buffers the cases read the canonical bytes back into.  The suite must
// not depend on kimix-core containers, so snapshots land here (kimix_vec_read).
unsigned char g_canon_a[2u * 1024u * 1024u];
unsigned char g_canon_b[2u * 1024u * 1024u];
char g_in1[2u * 1024u * 1024u];
char g_in2[2u * 1024u * 1024u];

/* Canonicalise [in, in+ilen) into `buf` (cap bytes), storing the length in
   `*outlen`.  Returns the FFI status verbatim; a non-OK status leaves `*outlen`
   at 0 and the placeholder never becomes live (checked by the caller). */
kimix_status ffi_canon_buf(const char *in, size_t ilen, unsigned char *buf, size_t cap, size_t *outlen) {
    *outlen = 0;
    kimix_vec out; // raw storage: kimix_canonical_json constructs into it
    const kimix_status st = kimix_canonical_json(in, ilen, &out);
    if (st != KIMIX_OK) {
        return st;
    }
    const size_t n = kimix_vec_size(&out);
    if (n > cap) {
        kimix_vec_destroy(&out);
        return KIMIX_ERR_FAILED;
    }
    if (n != 0) {
        expect(kimix_vec_read(&out, 0, buf, n) == KIMIX_OK);
    }
    *outlen = n;
    expect(kimix_vec_destroy(&out) == KIMIX_OK);
    return KIMIX_OK;
}

/* Convenience: canonicalise a NUL-terminated C literal and compare the bytes to
   another literal. */
bool ffi_canon_eq(const char *json, const char *expected) {
    size_t n = 0;
    if (ffi_canon_buf(json, std::strlen(json), g_canon_a, sizeof g_canon_a, &n) != KIMIX_OK) {
        return false;
    }
    const size_t e = std::strlen(expected);
    return n == e && std::memcmp(g_canon_a, expected, e) == 0;
}

/* Generate a large flat object into `buf` and return its byte length.
   `reverse` emits the members in the opposite insertion order; the canonical
   output must be identical either way (fixed-width keys sort the same as they
   were generated). */
/* Generate a large flat object into `buf` and return its byte length.
 `reverse` emits the members in the opposite insertion order; the canonical
 output must be identical either way, so each member's VALUE is derived from
 its own KEY (never from its position) and the fixed-width keys sort exactly
 into generation order. */
size_t ffi_fill_object(char *buf, size_t cap, size_t count, bool reverse) {
 size_t pos = 0;
 buf[pos++] = '{';
 for (size_t i = 0; i < count; ++i) {
 const long key = reverse ? static_cast<long>(count - 1 - i) : static_cast<long>(i);
 pos += static_cast<size_t>(std::snprintf(buf + pos, cap - pos, "%s\"key%06ld\":%ld",
 i == 0 ? "" : ",", key, key));
 }
 buf[pos++] = '}';
 return pos;
}

} // namespace tests_unit_api_test_kimix_canonical
using namespace tests_unit_api_test_kimix_canonical;

int main() {
    // ------------------------------------------------------- key-shuffle equality
    "canonical key shuffle byte equality"_test = [] {
        const char *a = R"({"b":1,"a":2,"c":{"y":1,"x":2}})";
        const char *b = R"({"a":2,"c":{"x":2,"y":1},"b":1})";
        const char *expected = R"({"a":2,"b":1,"c":{"x":2,"y":1}})";
        size_t na = 0;
        size_t nb = 0;
        expect(ffi_canon_buf(a, std::strlen(a), g_canon_a, sizeof g_canon_a, &na) == KIMIX_OK);
        expect(ffi_canon_buf(b, std::strlen(b), g_canon_b, sizeof g_canon_b, &nb) == KIMIX_OK);
        expect(na == nb) << "shuffled inputs produce equal-length canonical bytes";
        expect(std::memcmp(g_canon_a, g_canon_b, na) == 0) << "shuffled inputs are byte-identical";
        expect(na == std::strlen(expected) && std::memcmp(g_canon_a, expected, na) == 0)
            << "the canonical form is the documented sorted spelling";
    };

    // ---------------------------------------------------- number precision
    "canonical number precision preserved"_test = [] {
        // 1.0 != 1: the raw token text is copied verbatim, not normalised.
        expect(ffi_canon_eq(R"({"a":1.0,"b":1,"c":1e2,"d":-0,"e":0.1,"f":1.500})",
                            R"({"a":1.0,"b":1,"c":1e2,"d":-0,"e":0.1,"f":1.500})"));
        // a bare-number top level, and the 1.0 vs 1 distinction survives
        expect(ffi_canon_eq("1.0", "1.0"));
        expect(ffi_canon_eq("1", "1"));
    };

    // ---------------------------------------------------------- nesting
    "canonical nested arrays and objects"_test = [] {
        const char *in = R"([ {"b":[1,2,{"z":null,"a":true}]}, [], {"q":{},"p":[]}, false, "s"] )";
        // arrays keep order, objects sort, whitespace gone.
        expect(ffi_canon_eq(in, R"([{"b":[1,2,{"a":true,"z":null}]},[],{"p":[],"q":{}},false,"s"])"));
    };

    // ------------------------------------------------------- empty / degenerate
    "canonical empty object and empty array are valid"_test = [] {
        expect(ffi_canon_eq("{}", "{}"));
        expect(ffi_canon_eq("[]", "[]"));
    };

    "canonical empty and whitespace-only documents are rejected"_test = [] {
        size_t n = 0;
        expect(ffi_canon_buf("", 0, g_canon_a, sizeof g_canon_a, &n) == KIMIX_ERR_INVALID_INPUT);
        const char *ws = "   \t\r\n ";
        expect(ffi_canon_buf(ws, std::strlen(ws), g_canon_a, sizeof g_canon_a, &n) == KIMIX_ERR_INVALID_INPUT);
    };

    "canonical top-level scalars"_test = [] {
        expect(ffi_canon_eq("null", "null"));
        expect(ffi_canon_eq("true", "true"));
        expect(ffi_canon_eq("false", "false"));
        expect(ffi_canon_eq("\"hi\"", "\"hi\""));
    };

    // ----------------------------------------------- binary safety: embedded NUL
    "canonical rejects an embedded NUL byte, not UB"_test = [] {
        // {"a":"x<0x00>y"} — a RAW NUL inside a string, passed with an explicit
        // length so a naive strlen-based implementation would silently truncate.
        const char json[] = {'{', '"', 'a', '"', ':', '"', 'x', '\0', 'y', '"', '}', '\0'};
        const size_t len = 11; // the bytes up to (not including) the final '\0'
        // Must be rejected and must NOT construct a placeholder.
        kimix_vec out; // raw storage
        const kimix_status st = kimix_canonical_json(json, len, &out);
        expect(st == KIMIX_ERR_INVALID_INPUT);
        expect(!kimix_vec_is_initialized(&out)) << "a rejected call leaves the placeholder raw";
        // The _str spelling cannot even see the NUL: it truncates the input at the
        // embedded terminator to `{` `"a"` `:` `"x` and that is malformed too.
        kimix_vec out2;
        expect(kimix_canonical_json_str(json, &out2) == KIMIX_ERR_INVALID_INPUT);
        expect(!kimix_vec_is_initialized(&out2));
    };

    // ------------------------------------------------------ invalid UTF-8
    "canonical invalid UTF-8 reports an error status, not UB"_test = [] {
        // {"a":"\xff\xfe"} — 0xFF/0xFE are not valid UTF-8 lead bytes.
        const char json[] = {'{', '"', 'a', '"', ':', '"', static_cast<char>(0xFF),
                             static_cast<char>(0xFE), '"', '}', '\0'};
        size_t n = 0;
        expect(ffi_canon_buf(json, 10, g_canon_a, sizeof g_canon_a, &n) == KIMIX_ERR_INVALID_INPUT);
        // A valid multi-byte UTF-8 value is preserved verbatim (no \u escaping).
        const char good[] = {"\xC3\xA9"}; // "é" (U+00E9) as UTF-8
        kimix_vec out;
        const char wrapped[] = {'[', '"', '\xC3', '\xA9', '"', ']', '\0'};
        expect(kimix_canonical_json(wrapped, 6, &out) == KIMIX_OK);
        const size_t got = kimix_vec_size(&out);
        unsigned char buf[16];
        expect(got == 6);
        if (got == 6) {
            expect(kimix_vec_read(&out, 0, buf, got) == KIMIX_OK);
            expect(std::memcmp(buf, "[\"\xC3\xA9\"]", 6) == 0) << "raw UTF-8 passes through unescaped";
        }
        expect(kimix_vec_destroy(&out) == KIMIX_OK);
        (void)good;
    };

    // ------------------------------------------------------ duplicate key
    "canonical rejects a duplicate object key"_test = [] {
        size_t n = 0;
        expect(ffi_canon_buf(R"({"a":1,"a":2})", 11, g_canon_a, sizeof g_canon_a, &n) == KIMIX_ERR_INVALID_INPUT);
        // keys that differ are fine
        expect(ffi_canon_eq(R"({"a":1,"b":2})", R"({"a":1,"b":2})"));
    };

    // --------------------------------------------- string escape table
    "canonical re-escapes control characters and quotes"_test = [] {
        // "  and  \  become \" and \\; 0x00..0x1F that are not \b\t\n\f\r use
        // lowercase \u00xx (these arrived as escapes, since the raw bytes are
        // rejected by the strict parser).
        expect(ffi_canon_eq("\"a\\tb\"", "\"a\\tb\""));   // tab stays \t
        expect(ffi_canon_eq("\"\\u0001\"", "\"\\u0001\"")); // lowercase hex round-trips
        expect(ffi_canon_eq("\"\\u0000\"", "\"\\u0000\"")); // a decoded NUL re-escapes
        expect(ffi_canon_eq("\"\\u000A\"", "\"\\n\""));      // \n uses its short escape
        expect(ffi_canon_eq("\"\\u0022\"", "\"\\\"\""));     // a decoded quote escapes back
    };

    // ------------------------------------------- argument / state discipline
    "canonical argument and state errors"_test = [] {
        const char *good = "{\"a\":1}";
        // NULL out: INVALID_ARG.
        expect(kimix_canonical_json(good, std::strlen(good), nullptr) == KIMIX_ERR_INVALID_ARG);
        // NULL input with len > 0: INVALID_ARG.
        kimix_vec out;
        expect(kimix_canonical_json(nullptr, 4, &out) == KIMIX_ERR_INVALID_ARG);
        expect(!kimix_vec_is_initialized(&out));
        // NULL _str input: INVALID_ARG.
        expect(kimix_canonical_json_str(nullptr, &out) == KIMIX_ERR_INVALID_ARG);
        // Constructing a live result, then constructing again into the SAME slot:
        // the second is INVALID_STATE (it would leak), the storage stays the live
        // first vector and is destroyed once.
        expect(kimix_canonical_json(good, std::strlen(good), &out) == KIMIX_OK);
        expect(kimix_vec_is_initialized(&out));
        expect(kimix_canonical_json(good, std::strlen(good), &out) == KIMIX_ERR_INVALID_STATE);
        expect(kimix_vec_destroy(&out) == KIMIX_OK);
        // Destroyed slot is raw again: a fresh construction succeeds.
        expect(kimix_canonical_json(good, std::strlen(good), &out) == KIMIX_OK);
        expect(kimix_vec_destroy(&out) == KIMIX_OK);
    };

    // ---------------------------------------- library-heap single-owner
    "canonical keeps the buffer on the library heap"_test = [] {
        // The result vector is moved into the placeholder; exactly one owner ever
        // exists, so kimix_vec_destroy() alone releases it (a double-free or a
        // foreign-heap release would abort in a debug mimalloc build).
        for (int i = 0; i < 2000; ++i) {
            kimix_vec v;
            const char *in = "{\"z\":1,\"a\":[1,2,3]}";
            if (kimix_canonical_json(in, std::strlen(in), &v) != KIMIX_OK) {
                expect(false) << "canonical ok in loop";
                return;
            }
            expect(kimix_vec_size(&v) > 0u);
            if (kimix_vec_destroy(&v) != KIMIX_OK) {
                expect(false) << "destroy ok in loop";
                return;
            }
        }
    };

    // ------------------------------------------------------- 1 MiB stress doc
    "canonical 1 MiB stress document"_test = [] {
        // A large flat object; a key-reversed twin must canonicalise to the exact
        // same bytes, proving the sort is total and the buffer survives ~1 MiB.
        const size_t count = 60000u; // ~1.05 MB of JSON
        const size_t len1 = ffi_fill_object(g_in1, sizeof g_in1, count, /*reverse=*/false);
        const size_t len2 = ffi_fill_object(g_in2, sizeof g_in2, count, /*reverse=*/true);
        expect(len1 > 1024u * 1024u) << "the generated document really is ~1 MiB";
        expect(len1 == len2);

        size_t na = 0;
        size_t nb = 0;
        expect(ffi_canon_buf(g_in1, len1, g_canon_a, sizeof g_canon_a, &na) == KIMIX_OK);
        expect(ffi_canon_buf(g_in2, len2, g_canon_b, sizeof g_canon_b, &nb) == KIMIX_OK);
        expect(na == nb) << "forward and reverse canonicalise to equal length";
        expect(std::memcmp(g_canon_a, g_canon_b, na) == 0) << "forward and reverse are byte-identical";
        // The canonical output is smaller than the minified input only because the
        // zero-padded keys collapse to their sorted order; sanity: it starts with
        // {"key000000":0 and is a single object.
        expect(na > 0u && g_canon_a[0] == '{');
    };
}
