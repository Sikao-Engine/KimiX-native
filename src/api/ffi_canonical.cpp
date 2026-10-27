/*
 * ffi_canonical.cpp -- implementation of the canonical-JSON C FFI
 * (api/ffi_canonical.h).
 *
 * One job, split in two, both exception-free:
 *
 *   1. parse the caller's bytes with yyjson in STRICT RFC 8259 mode plus the
 *      raw-number flag, over the library's own mimalloc yyjson_alc
 *      (api/detail.h) -- so a foreign allocator is never involved and a bad
 *      document is a NULL return, not UB; and
 *   2. walk the immutable tree and re-emit it, byte by byte, into a
 *      `kimix::vector<std::byte>` in the canonical grammar this header
 *      documents: minified, no BOM, object members key-sorted by decoded-UTF-8
 *      bytes, numbers copied verbatim from their source token, strings escaped
 *      with the fixed table.  The vector is then MOVED into the caller's
 *      placeholder (kimix::api::vec_construct_move) -- the one allocation the
 *      buffer made (on the library heap) is what kimix_vec_destroy releases.
 *
 * WHY A HAND-WRITTEN EMITTER AND NOT yyjson's writer
 * --------------------------------------------------
 * The acceptance rule is byte-identity with the managed Semevia.Core fallback.
 * A third-party writer's escaping choices (which control characters get a short
 * escape vs \uXXXX, the case of the hex digits, whether non-ASCII or '/' is
 * escaped) are exactly the kind of detail that must not drift between two
 * independent implementations, so it is pinned HERE, in code, rather than
 * inherited from yyjson's defaults.  yyjson is used only to parse and to hand
 * back the decoded value bytes (yyjson_get_str / _len) and the raw number text
 * (yyjson_get_raw / _len, present because of the raw-number flag); it never
 * writes.  The managed re-serializer follows the same table, and the FFI suite
 * asserts the two agree on every fixture.
 *
 * WHY THE RAW-NUMBER FLAG
 * -----------------------
 * Canonical identity here is byte-identity of the source number token (the plan
 * requires 1.0 != 1): YYJSON_READ_NUMBER_AS_RAW keeps every number as a TYPE_RAW
 * value whose text yyjson copied verbatim from the input.  Re-emitting that text
 * unchanged preserves precision and representation without any double
 * round-trip.  With this flag a number is TYPE_RAW, so the emitter's number case
 * is simply "copy the raw bytes"; a bare `yyjson_is_num` value can never occur on
 * this path and is reported as an input error rather than mis-emitted.
 *
 * WHAT THE STRICT PARSER ALREADY GUARANTEES (matched, not invented)
 * -----------------------------------------------------------------
 * The reader runs WITHOUT the ALLOW_INVALID_UNICODE, ALLOW_COMMENTS,
 * ALLOW_TRAILING_COMMAS, ALLOW_EXT_NUMBER / _SINGLE_QUOTED / _UNQUOTED_KEY (i.e.
 * JSON5) flags, and the fast string scanner treats every byte in [0x00..0x1F] as
 * a stop (yyjson.c, CHAR_TYPE_ASCII excludes 0x00-0x1F and 0x80-0xFF): so a raw
 * NUL or any other unescaped control byte, ill-formed UTF-8, or a malformed /
 * empty document makes yyjson_read_opts return NULL.  This file maps every NULL
 * to KIMIX_ERR_INVALID_INPUT -- the binary-safety guarantee of the length-based
 * API (an embedded NUL is REJECTED, never treated as the end of the text).  Only
 * after a document parses do the emitter's own two rules add to that set: a
 * duplicate object key (adjacent-equal bytes after the key sort) and a nesting
 * depth past k_canonical_max_depth (a stack bound, not corruption).
 *
 * All file-local names are prefixed with `canon` / `k_canon`: the build merges
 * translation units (unity / cxx batch), so a global or file-local namespace
 * name here must not be one another ffi_*.cpp could also invent.
 */
#include <api/detail.h>
#include <api/ffi_canonical.h>
#include <core/stl/algorithm.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

/* The result vector the emitter fills lives in the same placeholder the rest of
 * this library constructs, so restate the one layout fact that makes that
 * construction sound (ffi_vec.cpp asserts the full set; this is the one this
 * file depends on directly). */
static_assert(kimix::api::k_vec_guard_offset >= sizeof(kimix::api::byte_vector),
              "the guard slot would overlap the vector this file constructs");
static_assert(sizeof(std::byte) == 1, "the byte-domain canonical form assumes 1-byte elements");

namespace src_api_ffi_canonical {

using kimix::api::byte_vector;

/* Nesting bound for the recursive emitter.  The yyjson reader itself is
 * iterative (no depth limit compiled in -- YYJSON_READER_DEPTH_LIMIT is 0 here),
 * so a pathological document could otherwise drive this recursion past the
 * caller's stack.  A value past the bound is an input error, reported as
 * KIMIX_ERR_INVALID_INPUT, never a crash.  512 keeps the frame count small
 * enough for a default 1 MiB stack on every supported toolchain. */
constexpr std::size_t k_canonical_max_depth = 512;

/* The hex digits used for \u00xx control escapes (LOWERCASE -- part of the byte
 * table this header pins). */
constexpr char k_canonical_hex[] = "0123456789abcdef";

enum class canon_result {
    k_ok,
    k_invalid, // malformed / empty / raw NUL / bad UTF-8 / duplicate key
    k_too_deep,
};

/* std::byte from an ASCII char without the narrowing pitfalls of brace-init
 * under a signed `char`. */
inline std::byte canon_byte(char c) noexcept {
    return static_cast<std::byte>(static_cast<unsigned char>(c));
}

inline void canon_push(byte_vector &out, char c) noexcept {
    out.push_back(canon_byte(c));
}

/* Append the n bytes [s, s + n) verbatim (used for number tokens copied from the
 * source). */
inline void canon_append(byte_vector &out, const char *s, std::size_t n) noexcept {
    const std::byte *b = reinterpret_cast<const std::byte *>(s);
    out.insert(out.end(), b, b + n);
}

/* One object member: a borrowed pointer at its key's decoded UTF-8 bytes (valid
 * for the whole call because the immutable document stays alive until the
 * emitter returns) and a borrowed pointer at its value. */
struct canon_member {
    const char *key;
    std::size_t key_len;
    const yyjson_val *val;
};

/* Ordinal BYTE order of the two keys' UTF-8 bytes: compare the common prefix,
 * then the shorter key wins when one is a prefix of the other. */
inline bool canon_key_less(const canon_member &a, const canon_member &b) noexcept {
    std::size_t n = a.key_len < b.key_len ? a.key_len : b.key_len;
    int c = n ? std::memcmp(a.key, b.key, n) : 0;
    if (c != 0) return c < 0;
    return a.key_len < b.key_len;
}

/* Exact byte equality of two keys (the duplicate test after the sort). */
inline bool canon_key_equal(const canon_member &a, const canon_member &b) noexcept {
    return a.key_len == b.key_len && (a.key_len == 0 || std::memcmp(a.key, b.key, a.key_len) == 0);
}

/* Write a string (key or value): the leading/trailing quote plus the escape
 * table of ffi_canonical.h.  `len` bytes are the DECODED UTF-8 of the value, so
 * a byte below 0x20 that reaches here arrived through an escape and is
 * re-escaped back; every byte >= 0x20 (including all UTF-8 continuation and lead
 * bytes) is copied through unchanged -- non-ASCII is never \u-escaped. */
void canon_write_string(byte_vector &out, const char *data, std::size_t len) noexcept {
    canon_push(out, '"');
    for (std::size_t i = 0; i < len; ++i) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        switch (c) {
            case '"': canon_push(out, '\\'); canon_push(out, '"'); break;
            case '\\': canon_push(out, '\\'); canon_push(out, '\\'); break;
            case '\b': canon_push(out, '\\'); canon_push(out, 'b'); break;
            case '\t': canon_push(out, '\\'); canon_push(out, 't'); break;
            case '\n': canon_push(out, '\\'); canon_push(out, 'n'); break;
            case '\f': canon_push(out, '\\'); canon_push(out, 'f'); break;
            case '\r': canon_push(out, '\\'); canon_push(out, 'r'); break;
            default:
                if (c < 0x20u) {
                    canon_push(out, '\\');
                    canon_push(out, 'u');
                    canon_push(out, '0');
                    canon_push(out, '0');
                    canon_push(out, k_canonical_hex[c >> 4]);
                    canon_push(out, k_canonical_hex[c & 0xF]);
                } else {
                    canon_push(out, static_cast<char>(c));
                }
        }
    }
    canon_push(out, '"');
}

/* Emit one value into `out`.  The immutable `v` and the whole document it comes
 * from are alive for the call, so every borrowed pointer stays valid. */
canon_result canon_emit(byte_vector &out, const yyjson_val *v, std::size_t depth) {
    if (depth > k_canonical_max_depth) {
        return canon_result::k_too_deep;
    }

    // null / true / false.
    if (yyjson_is_null(v)) {
        canon_append(out, "null", 4);
        return canon_result::k_ok;
    }
    if (yyjson_is_true(v)) {
        canon_append(out, "true", 4);
        return canon_result::k_ok;
    }
    if (yyjson_is_false(v)) {
        canon_append(out, "false", 5);
        return canon_result::k_ok;
    }

    // number: the raw-number flag makes every number TYPE_RAW -- copy its
    // source text verbatim (precision preserved; 1.0 stays 1.0).
    if (yyjson_is_raw(v)) {
        const char *raw = yyjson_get_raw(v);
        std::size_t len = yyjson_get_len(v);
        if (raw == nullptr) {
            return canon_result::k_invalid;
        }
        canon_append(out, raw, len);
        return canon_result::k_ok;
    }
    // A real (non-raw) number would mean the flag was not honoured -- refuse
    // rather than emit a lossy double rendering that the managed side cannot
    // reproduce byte for byte.
    if (yyjson_is_num(v)) {
        return canon_result::k_invalid;
    }

    // string: emit with the fixed escape table.
    if (yyjson_is_str(v)) {
        canon_write_string(out, yyjson_get_str(v), yyjson_get_len(v));
        return canon_result::k_ok;
    }

    // array: order preserved, comma separated, no whitespace.
    if (yyjson_is_arr(v)) {
        canon_push(out, '[');
        std::size_t idx = 0;
        std::size_t max = 0;
        yyjson_val *item = nullptr;
        bool first = true;
        yyjson_arr_foreach(v, idx, max, item) {
            if (!first) canon_push(out, ',');
            first = false;
            canon_result rc = canon_emit(out, item, depth + 1);
            if (rc != canon_result::k_ok) return rc;
        }
        canon_push(out, ']');
        return canon_result::k_ok;
    }

    // object: collect members, sort by key, reject duplicates, emit sorted.
    if (yyjson_is_obj(v)) {
        kimix::vector<canon_member> members;
        members.reserve(yyjson_obj_size(v));
        std::size_t idx = 0;
        std::size_t max = 0;
        yyjson_val *key = nullptr;
        yyjson_val *val = nullptr;
        yyjson_obj_foreach(v, idx, max, key, val) {
            // Keys are strings by the grammar; a non-string key cannot occur in
            // a document the strict parser accepted.
            if (!yyjson_is_str(key)) {
                return canon_result::k_invalid;
            }
            members.push_back(canon_member{yyjson_get_str(key), yyjson_get_len(key), val});
        }

        kimix::sort(members.begin(), members.end(), canon_key_less);
        for (std::size_t i = 1; i < members.size(); ++i) {
            if (canon_key_equal(members[i - 1], members[i])) {
                return canon_result::k_invalid; // duplicate key: see header
            }
        }

        canon_push(out, '{');
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (i != 0) canon_push(out, ',');
            canon_write_string(out, members[i].key, members[i].key_len);
            canon_push(out, ':');
            canon_result rc = canon_emit(out, members[i].val, depth + 1);
            if (rc != canon_result::k_ok) return rc;
        }
        canon_push(out, '}');
        return canon_result::k_ok;
    }

    return canon_result::k_invalid;
}

} // namespace src_api_ffi_canonical
using namespace src_api_ffi_canonical;

KIMIX_FFI_BEGIN

kimix_status kimix_canonical_json(const char *utf8, size_t len, kimix_vec *out) {
    if (!out) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (!utf8 && len) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (!kimix::api::vec_is_raw_storage(out)) {
        return KIMIX_ERR_INVALID_STATE; // constructing would leak the live vector
    }

    /* Parse strict + raw numbers over the library's mimalloc allocator.  The
     * non-INSITU reader only ever READS the buffer (it copies it into the
     * document first), so the const-cast yyjson's own inline wrapper performs is
     * safe here.  A NULL document is the parser's answer to empty / malformed /
     * control-byte / ill-formed-UTF-8 input; map it to the documented error. */
    auto *dat = const_cast<char *>(utf8);
    yyjson_doc *doc = yyjson_read_opts(dat, len,
                                       YYJSON_READ_NUMBER_AS_RAW,
                                       &kimix::api::yyjson_mi_alc(), nullptr);
    if (!doc) {
        return KIMIX_ERR_INVALID_INPUT;
    }

    const yyjson_val *root = yyjson_doc_get_root(doc);
    byte_vector result;
    result.reserve(len); // canonical length is within a small factor of the input
    canon_result rc = root ? canon_emit(result, root, 0) : canon_result::k_invalid;
    yyjson_doc_free(doc);

    if (rc != canon_result::k_ok) {
        return KIMIX_ERR_INVALID_INPUT; // invalid input or nesting past the bound
    }

    /* Move the emitter's buffer into the placeholder -- the single owner of the
     * bytes is now the vector constructed in `out`, released by one
     * kimix_vec_destroy(). */
    kimix::api::vec_construct_move(out, std::move(result));
    return KIMIX_OK;
}

kimix_status kimix_canonical_json_str(const char *nul_terminated, kimix_vec *out) {
    if (!nul_terminated) {
        return KIMIX_ERR_INVALID_ARG; // no length to fall back on (out is checked below)
    }
    return kimix_canonical_json(nul_terminated, std::strlen(nul_terminated), out);
}

KIMIX_FFI_END
