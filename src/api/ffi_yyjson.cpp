/*
 * ffi_yyjson.cpp -- implementation of the yyjson JSON C FFI (see ffi_yyjson.h).
 *
 * SHAPE OF EVERY BRIDGE
 * ---------------------
 * yyjson is a plain C library with no failure channel other than a return
 * value, so each entry point below is a three-step translation unit-local
 * function:
 *
 *   1. validate: reject NULL arguments with the documented neutral value
 *      (false / 0 / NULL).  yyjson's public accessors are themselves mostly
 *      NULL-tolerant, but they are `static inline` in the header: a future fork
 *      could call one of the `unsafe_yyjson_*` primitives directly, and a C ABI
 *      must not inherit its implementation's tolerance.  The guard is therefore
 *      written here, not trusted to the callee.
 *   2. forward ONE call, with `kimix::api::yyjson_mi_alc()` substituted for every
 *      `const yyjson_alc *` parameter the fork asks for.
 *   3. translate: copy yyjson's error POD field by field into the caller's
 *      `kimix_yyjson_read_err` / `kimix_yyjson_write_err` (never a cast between
 *      the two types) and hand back the value with the ownership the header
 *      documents.
 *
 * THE ALLOCATOR RULE (this is the whole point of the file)
 * --------------------------------------------------------
 * Only the `*_opts` / `*_new` / `*_copy` entry points that take an allocator are
 * used, and they are always given the mimalloc allocator of api/detail.h.  The
 * short forms `yyjson_read`, `yyjson_write`, `yyjson_val_write` and
 * `yyjson_mut_write` are `yyjson_api_inline` wrappers that pass `alc == NULL`,
 * which makes yyjson fall back to the C library's malloc/free.  A buffer written
 * that way and released here with mi_free() -- or a document allocated in the
 * caller's CRT heap and released in this DLL's mimalloc heap -- is a cross-heap
 * free, i.e. heap corruption.  None of those four forms is called below, and the
 * writers hand back mimalloc memory that only kimix_yyjson_str_free() (mi_free)
 * may release.  The two `*_write_buf` functions allocate nothing at all, so they
 * need no allocator and no free.
 *
 * No C++ exception can escape: the project is built with
 * kimix_enable_exception=false, yyjson throws nothing, mimalloc reports failure
 * by returning NULL, and nothing here uses the throwing parts of the STL.
 */
#include <api/detail.h>
#include <api/ffi_yyjson.h>

/* The vendored fork.  api/detail.h already includes it; the include is repeated
 * here because this file's use of it is the contract of the whole module. */
#include <yyjson.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

/* A uniquely named namespace, NOT the plain `namespace {}` of ffi_vec.cpp: the
 * api target is built with a unity batch (see _config_project({batch_size = 8})
 * in src/api/xmake.lua), so several of these translation units end up in one and
 * two anonymous namespaces with same-named helpers would collide. */
namespace kimix_ffi_yyjson_ns {

using kimix::api::yyjson_mi_alc;

/* The single allocator every JSON call below is made with.  Address-taking is
 * safe: yyjson_mi_alc() returns a reference to a function-local static, and
 * yyjson only ever copies the struct into the document. */
inline const yyjson_alc *mi_alc() noexcept {
    return &yyjson_mi_alc();
}

/* yyjson only writes an error struct on failure, so the local that is handed to
 * it starts out as "success" and the caller always observes a defined value. */
inline void reset_read_err(yyjson_read_err &e) noexcept {
    e.code = YYJSON_READ_SUCCESS;
    e.msg = nullptr;
    e.pos = 0;
}

inline void reset_write_err(yyjson_write_err &e) noexcept {
    e.code = YYJSON_WRITE_SUCCESS;
    e.msg = nullptr;
}

/* Field-by-field publication into the caller-side mirror struct.  A NULL `out`
 * simply means the caller did not ask for the reason. */
inline void publish_read_err(const yyjson_read_err &e, kimix_yyjson_read_err *out) noexcept {
    if (!out) {
        return;
    }
    out->code = static_cast<uint32_t>(e.code);
    out->msg = e.msg;
    out->position = e.pos;
}

inline void publish_write_err(const yyjson_write_err &e, kimix_yyjson_write_err *out) noexcept {
    if (!out) {
        return;
    }
    out->code = static_cast<uint32_t>(e.code);
    out->msg = e.msg;
}

/* The tail shared by the four allocating writers: report the length (0 unless a
 * buffer came back) and the diagnosis, and return the buffer to the caller as
 * TRANSFERRED memory. */
inline char *finish_write(char *s, const yyjson_write_err &e, size_t written, size_t *len, kimix_yyjson_write_err *out) noexcept {
    if (len) {
        *len = (s ? written : 0);
    }
    publish_write_err(e, out);
    return s;
}

/* The one reader implementation behind kimix_yyjson_read / _read_str. */
inline kimix_yyjson_doc *read_impl(char *dat, size_t len, uint32_t flags, kimix_yyjson_read_err *err) noexcept {
    yyjson_read_err e;
    reset_read_err(e);
    /* `dat` is only written when KIMIX_YYJSON_READ_INSITU is set; the constness
     * is cast away by the caller, exactly as yyjson's own documentation
     * describes, and the header states the padding requirement. */
    auto *doc = yyjson_read_opts(dat, len, static_cast<yyjson_read_flag>(flags), mi_alc(), &e);
    publish_read_err(e, err);
    return doc;
}

} // namespace kimix_ffi_yyjson_ns

/* ---------------------------------------------------------------------------
 * The frozen facts this bridge is written against.
 *
 * The published macros of ffi_yyjson.h are literals copied out of the vendored
 * header; these asserts are what makes that safe.  If the fork renames, renumbers
 * or re-values anything, the build breaks here rather than mis-parsing JSON in a
 * caller's process.  Same for the C typedefs naming the very same struct tags as
 * yyjson's own: the identity is what lets the pointers convert without a cast.
 * ------------------------------------------------------------------------- */
static_assert(std::is_same_v<kimix_yyjson_doc, yyjson_doc>, "kimix_yyjson_doc must BE yyjson_doc (no cast allowed)");
static_assert(std::is_same_v<kimix_yyjson_val, yyjson_val>, "kimix_yyjson_val must BE yyjson_val (no cast allowed)");
static_assert(std::is_same_v<kimix_yyjson_mut_doc, yyjson_mut_doc>, "kimix_yyjson_mut_doc must BE yyjson_mut_doc (no cast allowed)");
static_assert(std::is_same_v<kimix_yyjson_mut_val, yyjson_mut_val>, "kimix_yyjson_mut_val must BE yyjson_mut_val (no cast allowed)");

/* The int32_t narrowing of kimix_yyjson_get_int(): `int` is 32 bits on every
 * platform this project builds (x64 / arm64, MSVC / clang / gcc). */
static_assert(sizeof(int) == 4 && std::is_same_v<int, int32_t>, "yyjson's `int` is not a 32-bit int here");

/* Value types (yyjson.h:604-618). */
static_assert(KIMIX_YYJSON_TYPE_NONE == YYJSON_TYPE_NONE, "KIMIX_YYJSON_TYPE_NONE drifted");
static_assert(KIMIX_YYJSON_TYPE_RAW == YYJSON_TYPE_RAW, "KIMIX_YYJSON_TYPE_RAW drifted");
static_assert(KIMIX_YYJSON_TYPE_NULL == YYJSON_TYPE_NULL, "KIMIX_YYJSON_TYPE_NULL drifted");
static_assert(KIMIX_YYJSON_TYPE_BOOL == YYJSON_TYPE_BOOL, "KIMIX_YYJSON_TYPE_BOOL drifted");
static_assert(KIMIX_YYJSON_TYPE_NUM == YYJSON_TYPE_NUM, "KIMIX_YYJSON_TYPE_NUM drifted");
static_assert(KIMIX_YYJSON_TYPE_STR == YYJSON_TYPE_STR, "KIMIX_YYJSON_TYPE_STR drifted");
static_assert(KIMIX_YYJSON_TYPE_ARR == YYJSON_TYPE_ARR, "KIMIX_YYJSON_TYPE_ARR drifted");
static_assert(KIMIX_YYJSON_TYPE_OBJ == YYJSON_TYPE_OBJ, "KIMIX_YYJSON_TYPE_OBJ drifted");

/* Read flags (yyjson.h:816-909) -- the fork's are `static const` objects, the
 * published ones are literals; the comparison is a constant expression because a
 * `static const` integral with a literal initializer is usable in one. */
static_assert(KIMIX_YYJSON_READ_NOFLAG == YYJSON_READ_NOFLAG, "READ_NOFLAG drifted");
static_assert(KIMIX_YYJSON_READ_INSITU == YYJSON_READ_INSITU, "READ_INSITU drifted");
static_assert(KIMIX_YYJSON_READ_STOP_WHEN_DONE == YYJSON_READ_STOP_WHEN_DONE, "READ_STOP_WHEN_DONE drifted");
static_assert(KIMIX_YYJSON_READ_ALLOW_TRAILING_COMMAS == YYJSON_READ_ALLOW_TRAILING_COMMAS, "READ_ALLOW_TRAILING_COMMAS drifted");
static_assert(KIMIX_YYJSON_READ_ALLOW_COMMENTS == YYJSON_READ_ALLOW_COMMENTS, "READ_ALLOW_COMMENTS drifted");
static_assert(KIMIX_YYJSON_READ_ALLOW_INF_AND_NAN == YYJSON_READ_ALLOW_INF_AND_NAN, "READ_ALLOW_INF_AND_NAN drifted");
static_assert(KIMIX_YYJSON_READ_NUMBER_AS_RAW == YYJSON_READ_NUMBER_AS_RAW, "READ_NUMBER_AS_RAW drifted");
static_assert(KIMIX_YYJSON_READ_ALLOW_INVALID_UNICODE == YYJSON_READ_ALLOW_INVALID_UNICODE, "READ_ALLOW_INVALID_UNICODE drifted");
static_assert(KIMIX_YYJSON_READ_BIGNUM_AS_RAW == YYJSON_READ_BIGNUM_AS_RAW, "READ_BIGNUM_AS_RAW drifted");
static_assert(KIMIX_YYJSON_READ_ALLOW_BOM == YYJSON_READ_ALLOW_BOM, "READ_ALLOW_BOM drifted");
static_assert(KIMIX_YYJSON_READ_ALLOW_EXT_NUMBER == YYJSON_READ_ALLOW_EXT_NUMBER, "READ_ALLOW_EXT_NUMBER drifted");
static_assert(KIMIX_YYJSON_READ_ALLOW_EXT_ESCAPE == YYJSON_READ_ALLOW_EXT_ESCAPE, "READ_ALLOW_EXT_ESCAPE drifted");
static_assert(KIMIX_YYJSON_READ_ALLOW_EXT_WHITESPACE == YYJSON_READ_ALLOW_EXT_WHITESPACE, "READ_ALLOW_EXT_WHITESPACE drifted");
static_assert(KIMIX_YYJSON_READ_ALLOW_SINGLE_QUOTED_STR == YYJSON_READ_ALLOW_SINGLE_QUOTED_STR, "READ_ALLOW_SINGLE_QUOTED_STR drifted");
static_assert(KIMIX_YYJSON_READ_ALLOW_UNQUOTED_KEY == YYJSON_READ_ALLOW_UNQUOTED_KEY, "READ_ALLOW_UNQUOTED_KEY drifted");
static_assert(KIMIX_YYJSON_READ_JSON5 == YYJSON_READ_JSON5, "READ_JSON5 drifted");

/* Write flags (yyjson.h:1252-1309). */
static_assert(KIMIX_YYJSON_WRITE_NOFLAG == YYJSON_WRITE_NOFLAG, "WRITE_NOFLAG drifted");
static_assert(KIMIX_YYJSON_WRITE_PRETTY == YYJSON_WRITE_PRETTY, "WRITE_PRETTY drifted");
static_assert(KIMIX_YYJSON_WRITE_ESCAPE_UNICODE == YYJSON_WRITE_ESCAPE_UNICODE, "WRITE_ESCAPE_UNICODE drifted");
static_assert(KIMIX_YYJSON_WRITE_ESCAPE_SLASHES == YYJSON_WRITE_ESCAPE_SLASHES, "WRITE_ESCAPE_SLASHES drifted");
static_assert(KIMIX_YYJSON_WRITE_ALLOW_INF_AND_NAN == YYJSON_WRITE_ALLOW_INF_AND_NAN, "WRITE_ALLOW_INF_AND_NAN drifted");
static_assert(KIMIX_YYJSON_WRITE_INF_AND_NAN_AS_NULL == YYJSON_WRITE_INF_AND_NAN_AS_NULL, "WRITE_INF_AND_NAN_AS_NULL drifted");
static_assert(KIMIX_YYJSON_WRITE_ALLOW_INVALID_UNICODE == YYJSON_WRITE_ALLOW_INVALID_UNICODE, "WRITE_ALLOW_INVALID_UNICODE drifted");
static_assert(KIMIX_YYJSON_WRITE_PRETTY_TWO_SPACES == YYJSON_WRITE_PRETTY_TWO_SPACES, "WRITE_PRETTY_TWO_SPACES drifted");
static_assert(KIMIX_YYJSON_WRITE_NEWLINE_AT_END == YYJSON_WRITE_NEWLINE_AT_END, "WRITE_NEWLINE_AT_END drifted");
static_assert(KIMIX_YYJSON_WRITE_LOWERCASE_HEX == YYJSON_WRITE_LOWERCASE_HEX, "WRITE_LOWERCASE_HEX drifted");
static_assert(KIMIX_YYJSON_WRITE_FP_FLAG_BITS == YYJSON_WRITE_FP_FLAG_BITS, "WRITE_FP_FLAG_BITS drifted");
static_assert(KIMIX_YYJSON_WRITE_FP_PREC_BITS == YYJSON_WRITE_FP_PREC_BITS, "WRITE_FP_PREC_BITS drifted");
static_assert(KIMIX_YYJSON_WRITE_FP_TO_FLOAT == static_cast<uint32_t>(YYJSON_WRITE_FP_TO_FLOAT), "WRITE_FP_TO_FLOAT drifted");
static_assert(KIMIX_YYJSON_WRITE_FP_TO_FIXED(15) == static_cast<uint32_t>(YYJSON_WRITE_FP_TO_FIXED(15)), "WRITE_FP_TO_FIXED drifted");
static_assert(KIMIX_YYJSON_WRITE_FP_TO_FIXED(0) == static_cast<uint32_t>(YYJSON_WRITE_FP_TO_FIXED(0)), "WRITE_FP_TO_FIXED(0) drifted");

/* Error codes (yyjson.h:917-962 and 1317-1341). */
static_assert(KIMIX_YYJSON_READ_SUCCESS == YYJSON_READ_SUCCESS, "READ_SUCCESS drifted");
static_assert(KIMIX_YYJSON_READ_ERROR_INVALID_PARAMETER == YYJSON_READ_ERROR_INVALID_PARAMETER, "READ_ERROR_INVALID_PARAMETER drifted");
static_assert(KIMIX_YYJSON_READ_ERROR_MEMORY_ALLOCATION == YYJSON_READ_ERROR_MEMORY_ALLOCATION, "READ_ERROR_MEMORY_ALLOCATION drifted");
static_assert(KIMIX_YYJSON_READ_ERROR_EMPTY_CONTENT == YYJSON_READ_ERROR_EMPTY_CONTENT, "READ_ERROR_EMPTY_CONTENT drifted");
static_assert(KIMIX_YYJSON_READ_ERROR_UNEXPECTED_CONTENT == YYJSON_READ_ERROR_UNEXPECTED_CONTENT, "READ_ERROR_UNEXPECTED_CONTENT drifted");
static_assert(KIMIX_YYJSON_READ_ERROR_UNEXPECTED_END == YYJSON_READ_ERROR_UNEXPECTED_END, "READ_ERROR_UNEXPECTED_END drifted");
static_assert(KIMIX_YYJSON_READ_ERROR_UNEXPECTED_CHARACTER == YYJSON_READ_ERROR_UNEXPECTED_CHARACTER, "READ_ERROR_UNEXPECTED_CHARACTER drifted");
static_assert(KIMIX_YYJSON_READ_ERROR_JSON_STRUCTURE == YYJSON_READ_ERROR_JSON_STRUCTURE, "READ_ERROR_JSON_STRUCTURE drifted");
static_assert(KIMIX_YYJSON_READ_ERROR_INVALID_COMMENT == YYJSON_READ_ERROR_INVALID_COMMENT, "READ_ERROR_INVALID_COMMENT drifted");
static_assert(KIMIX_YYJSON_READ_ERROR_INVALID_NUMBER == YYJSON_READ_ERROR_INVALID_NUMBER, "READ_ERROR_INVALID_NUMBER drifted");
static_assert(KIMIX_YYJSON_READ_ERROR_INVALID_STRING == YYJSON_READ_ERROR_INVALID_STRING, "READ_ERROR_INVALID_STRING drifted");
static_assert(KIMIX_YYJSON_READ_ERROR_LITERAL == YYJSON_READ_ERROR_LITERAL, "READ_ERROR_LITERAL drifted");
static_assert(KIMIX_YYJSON_READ_ERROR_FILE_OPEN == YYJSON_READ_ERROR_FILE_OPEN, "READ_ERROR_FILE_OPEN drifted");
static_assert(KIMIX_YYJSON_READ_ERROR_FILE_READ == YYJSON_READ_ERROR_FILE_READ, "READ_ERROR_FILE_READ drifted");
static_assert(KIMIX_YYJSON_READ_ERROR_MORE == YYJSON_READ_ERROR_MORE, "READ_ERROR_MORE drifted");
static_assert(KIMIX_YYJSON_READ_ERROR_DEPTH == YYJSON_READ_ERROR_DEPTH, "READ_ERROR_DEPTH drifted");

static_assert(KIMIX_YYJSON_WRITE_SUCCESS == YYJSON_WRITE_SUCCESS, "WRITE_SUCCESS drifted");
static_assert(KIMIX_YYJSON_WRITE_ERROR_INVALID_PARAMETER == YYJSON_WRITE_ERROR_INVALID_PARAMETER, "WRITE_ERROR_INVALID_PARAMETER drifted");
static_assert(KIMIX_YYJSON_WRITE_ERROR_MEMORY_ALLOCATION == YYJSON_WRITE_ERROR_MEMORY_ALLOCATION, "WRITE_ERROR_MEMORY_ALLOCATION drifted");
static_assert(KIMIX_YYJSON_WRITE_ERROR_INVALID_VALUE_TYPE == YYJSON_WRITE_ERROR_INVALID_VALUE_TYPE, "WRITE_ERROR_INVALID_VALUE_TYPE drifted");
static_assert(KIMIX_YYJSON_WRITE_ERROR_NAN_OR_INF == YYJSON_WRITE_ERROR_NAN_OR_INF, "WRITE_ERROR_NAN_OR_INF drifted");
static_assert(KIMIX_YYJSON_WRITE_ERROR_FILE_OPEN == YYJSON_WRITE_ERROR_FILE_OPEN, "WRITE_ERROR_FILE_OPEN drifted");
static_assert(KIMIX_YYJSON_WRITE_ERROR_FILE_WRITE == YYJSON_WRITE_ERROR_FILE_WRITE, "WRITE_ERROR_FILE_WRITE drifted");
static_assert(KIMIX_YYJSON_WRITE_ERROR_INVALID_STRING == YYJSON_WRITE_ERROR_INVALID_STRING, "WRITE_ERROR_INVALID_STRING drifted");
static_assert(KIMIX_YYJSON_WRITE_ERROR_DEPTH == YYJSON_WRITE_ERROR_DEPTH, "WRITE_ERROR_DEPTH drifted");

/* The error records are mirrors, not aliases: they are filled field by field and
 * a size match is documentation, not a licence to reinterpret.  They catch a
 * fork that changes the widths the published struct promises. */
static_assert(sizeof(uint32_t) == sizeof(yyjson_read_code), "yyjson_read_code is not 32-bit");
static_assert(sizeof(uint32_t) == sizeof(yyjson_write_code), "yyjson_write_code is not 32-bit");
static_assert(sizeof(kimix_yyjson_read_err) == sizeof(yyjson_read_err), "the read_err mirror no longer matches the fork's layout");
static_assert(sizeof(kimix_yyjson_write_err) == sizeof(yyjson_write_err), "the write_err mirror no longer matches the fork's layout");

/* The in-situ padding the header documents for KIMIX_YYJSON_READ_INSITU. */
static_assert(YYJSON_PADDING_SIZE == 4, "the documented INSITU padding is no longer 4 bytes");

KIMIX_FFI_BEGIN

using namespace kimix_ffi_yyjson_ns;

// ===========================================================================
// 0. Identity
// ===========================================================================

uint32_t kimix_yyjson_version(void) {
    /* yyjson.h:593 -- the fork's own version query, a plain uint32 hex number. */
    return yyjson_version();
}

// ===========================================================================
// 1. Immutable document: parse and lifecycle
// ===========================================================================

kimix_yyjson_doc *kimix_yyjson_read(const char *data, size_t len, uint32_t flags, kimix_yyjson_read_err *err) {
    /* yyjson_read_opts (yyjson.h:1001), always with the mimalloc allocator.  A
     * NULL `data` or a zero `len` is reported by yyjson itself as
     * YYJSON_READ_ERROR_INVALID_PARAMETER; the cast below is the documented way
     * to feed a read-only buffer, and it is inert unless INSITU is set. */
    return read_impl(const_cast<char *>(data), len, flags, err);
}

kimix_yyjson_doc *kimix_yyjson_read_str(const char *str, uint32_t flags, kimix_yyjson_read_err *err) {
    if (!str) {
        yyjson_read_err e;
        reset_read_err(e);
        e.code = YYJSON_READ_ERROR_INVALID_PARAMETER;
        e.msg = "input string is null";
        publish_read_err(e, err);
        return nullptr;
    }
    /* INSITU needs bytes past the terminator; a C string does not have them, so
     * the flag is cleared -- the same thing yyjson's own `yyjson_read` inline
     * does (yyjson.h:1073), except that we do it for a documented reason. */
    return read_impl(const_cast<char *>(str), std::strlen(str), flags & ~static_cast<uint32_t>(KIMIX_YYJSON_READ_INSITU), err);
}

void kimix_yyjson_doc_free(kimix_yyjson_doc *doc) {
    /* yyjson_doc_free (yyjson.h:1938): frees through the allocator stored in the
     * document, which is the mimalloc one this file installed. */
    if (doc) {
        yyjson_doc_free(doc);
    }
}

kimix_yyjson_val *kimix_yyjson_doc_root(const kimix_yyjson_doc *doc) {
    return doc ? yyjson_doc_get_root(doc) : nullptr; /* yyjson_doc_get_root, yyjson.h:1923 */
}

size_t kimix_yyjson_doc_read_size(const kimix_yyjson_doc *doc) {
    return doc ? yyjson_doc_get_read_size(doc) : 0; /* yyjson.h:1928 */
}

size_t kimix_yyjson_doc_val_count(const kimix_yyjson_doc *doc) {
    return doc ? yyjson_doc_get_val_count(doc) : 0; /* yyjson.h:1933 */
}

kimix_yyjson_mut_doc *kimix_yyjson_doc_mut_copy(const kimix_yyjson_doc *doc) {
    /* yyjson_doc_mut_copy (yyjson.h:2500) takes an allocator: it is the mimalloc
     * one, so the copy and its source live in the same heap. */
    if (!doc) {
        return nullptr;
    }
    return yyjson_doc_mut_copy(doc, mi_alc());
}

// ===========================================================================
// 2. Immutable value: type checks
// ===========================================================================

bool kimix_yyjson_is_raw(const kimix_yyjson_val *v) { return v && yyjson_is_raw(v); }           /* yyjson.h:1948 */
bool kimix_yyjson_is_null(const kimix_yyjson_val *v) { return v && yyjson_is_null(v); }         /* yyjson.h:1952 */
bool kimix_yyjson_is_true(const kimix_yyjson_val *v) { return v && yyjson_is_true(v); }         /* yyjson.h:1956 */
bool kimix_yyjson_is_false(const kimix_yyjson_val *v) { return v && yyjson_is_false(v); }       /* yyjson.h:1960 */
bool kimix_yyjson_is_bool(const kimix_yyjson_val *v) { return v && yyjson_is_bool(v); }         /* yyjson.h:1964 */
bool kimix_yyjson_is_uint(const kimix_yyjson_val *v) { return v && yyjson_is_uint(v); }         /* yyjson.h:1968 */
bool kimix_yyjson_is_sint(const kimix_yyjson_val *v) { return v && yyjson_is_sint(v); }         /* yyjson.h:1972 */
bool kimix_yyjson_is_int(const kimix_yyjson_val *v) { return v && yyjson_is_int(v); }           /* yyjson.h:1976 */
bool kimix_yyjson_is_real(const kimix_yyjson_val *v) { return v && yyjson_is_real(v); }         /* yyjson.h:1980 */
bool kimix_yyjson_is_num(const kimix_yyjson_val *v) { return v && yyjson_is_num(v); }           /* yyjson.h:1984 */
bool kimix_yyjson_is_str(const kimix_yyjson_val *v) { return v && yyjson_is_str(v); }           /* yyjson.h:1988 */
bool kimix_yyjson_is_arr(const kimix_yyjson_val *v) { return v && yyjson_is_arr(v); }           /* yyjson.h:1992 */
bool kimix_yyjson_is_obj(const kimix_yyjson_val *v) { return v && yyjson_is_obj(v); }           /* yyjson.h:1996 */
bool kimix_yyjson_is_ctn(const kimix_yyjson_val *v) { return v && yyjson_is_ctn(v); }           /* yyjson.h:2000 */

uint8_t kimix_yyjson_get_type(const kimix_yyjson_val *v) {
    /* yyjson_get_type (yyjson.h:2010) already maps NULL to YYJSON_TYPE_NONE; the
     * guard keeps this true even if the fork changes that. */
    return v ? static_cast<uint8_t>(yyjson_get_type(v)) : static_cast<uint8_t>(KIMIX_YYJSON_TYPE_NONE);
}

const char *kimix_yyjson_get_type_desc(const kimix_yyjson_val *v) {
    /* yyjson_get_type_desc (yyjson.h:2023) returns a string literal from the
     * library's static storage, "unknown" for a NULL value. */
    return v ? yyjson_get_type_desc(v) : "unknown";
}

// ===========================================================================
// 3. Immutable value: content getters
// ===========================================================================

bool kimix_yyjson_get_bool(const kimix_yyjson_val *v) { return v ? yyjson_get_bool(v) : false; }             /* yyjson.h:2031 */
uint64_t kimix_yyjson_get_uint(const kimix_yyjson_val *v) { return v ? yyjson_get_uint(v) : 0; }             /* yyjson.h:2035 */
int64_t kimix_yyjson_get_sint(const kimix_yyjson_val *v) { return v ? yyjson_get_sint(v) : 0; }              /* yyjson.h:2039 */

int32_t kimix_yyjson_get_int(const kimix_yyjson_val *v) {
    /* yyjson_get_int (yyjson.h:2043) returns a plain `int`; the static assert at
     * the top of this file pins `int` to 32 bits on every supported platform, so
     * this cast preserves the exact type the fork produces. */
    return v ? static_cast<int32_t>(yyjson_get_int(v)) : static_cast<int32_t>(0);
}

double kimix_yyjson_get_real(const kimix_yyjson_val *v) { return v ? yyjson_get_real(v) : 0.0; } /* yyjson.h:2047 */
double kimix_yyjson_get_num(const kimix_yyjson_val *v) { return v ? yyjson_get_num(v) : 0.0; }   /* yyjson.h:2051 */

const char *kimix_yyjson_get_str(const kimix_yyjson_val *v) {
    /* yyjson_get_str (yyjson.h:2055): the bytes belong to the document (or, for
     * a non-copied read, to the caller's input buffer).  BORROWED. */
    return v ? yyjson_get_str(v) : nullptr;
}

size_t kimix_yyjson_get_len(const kimix_yyjson_val *v) { return v ? yyjson_get_len(v) : 0; } /* yyjson.h:2060 */

const char *kimix_yyjson_get_raw(const kimix_yyjson_val *v) { return v ? yyjson_get_raw(v) : nullptr; } /* yyjson.h:2027 */

bool kimix_yyjson_equals_str(const kimix_yyjson_val *v, const char *str) {
    /* yyjson_equals_str (yyjson.h:2064) would strlen() a NULL `str`. */
    return v && str && yyjson_equals_str(v, str);
}

bool kimix_yyjson_equals_strn(const kimix_yyjson_val *v, const char *str, size_t len) {
    /* yyjson_equals_strn (yyjson.h:2070): `len` may be 0, but a NULL `str` with a
     * non-zero length is a memory error either way. */
    return v && str && yyjson_equals_strn(v, str, len);
}

// ===========================================================================
// 4. Immutable value: array access
// ===========================================================================

size_t kimix_yyjson_arr_size(const kimix_yyjson_val *arr) { return arr ? yyjson_arr_size(arr) : 0; } /* yyjson.h:2166 */

kimix_yyjson_val *kimix_yyjson_arr_get(const kimix_yyjson_val *arr, size_t idx) {
    return arr ? yyjson_arr_get(arr, idx) : nullptr; /* yyjson.h:2172, O(idx) if not flat */
}

kimix_yyjson_val *kimix_yyjson_arr_get_first(const kimix_yyjson_val *arr) {
    return arr ? yyjson_arr_get_first(arr) : nullptr; /* yyjson.h:2176 */
}

kimix_yyjson_val *kimix_yyjson_arr_get_last(const kimix_yyjson_val *arr) {
    return arr ? yyjson_arr_get_last(arr) : nullptr; /* yyjson.h:2182 */
}

// ===========================================================================
// 5. Immutable value: object access
// ===========================================================================

size_t kimix_yyjson_obj_size(const kimix_yyjson_val *obj) { return obj ? yyjson_obj_size(obj) : 0; } /* yyjson.h:2274 */

kimix_yyjson_val *kimix_yyjson_obj_get(const kimix_yyjson_val *obj, const char *key) {
    return (obj && key) ? yyjson_obj_get(obj, key) : nullptr; /* yyjson.h:2283 */
}

kimix_yyjson_val *kimix_yyjson_obj_getn(const kimix_yyjson_val *obj, const char *key, size_t key_len) {
    return (obj && key) ? yyjson_obj_getn(obj, key, key_len) : nullptr; /* yyjson.h:2294 */
}

// ===========================================================================
// 6. Writing an immutable document / value
// ===========================================================================

char *kimix_yyjson_write(const kimix_yyjson_doc *doc, uint32_t flags, size_t *len, kimix_yyjson_write_err *err) {
    /* yyjson_write_opts (yyjson.h:1379) with the mimalloc allocator -- NEVER
     * yyjson_write (yyjson.h:1481), which passes alc == NULL.  The result is
     * mi_malloc'd; the caller must pair it with kimix_yyjson_str_free(). */
    yyjson_write_err e;
    reset_write_err(e);
    size_t written = 0;
    auto *s = yyjson_write_opts(doc, static_cast<yyjson_write_flag>(flags), mi_alc(), &written, &e);
    return finish_write(s, e, written, len, err);
}

char *kimix_yyjson_val_write(const kimix_yyjson_val *v, uint32_t flags, size_t *len, kimix_yyjson_write_err *err) {
    /* yyjson_val_write_opts (yyjson.h:1646); yyjson_val_write (yyjson.h:1748) is
     * the alc-less form this file must not call. */
    if (!v) {
        if (len) {
            *len = 0;
        }
        yyjson_write_err e;
        reset_write_err(e);
        e.code = YYJSON_WRITE_ERROR_INVALID_PARAMETER;
        e.msg = "value is null";
        publish_write_err(e, err);
        return nullptr;
    }
    yyjson_write_err e;
    reset_write_err(e);
    size_t written = 0;
    auto *s = yyjson_val_write_opts(v, static_cast<yyjson_write_flag>(flags), mi_alc(), &written, &e);
    return finish_write(s, e, written, len, err);
}

size_t kimix_yyjson_write_buf(char *buf, size_t buf_len, const kimix_yyjson_doc *doc, uint32_t flags, kimix_yyjson_write_err *err) {
    /* yyjson_write_buf (yyjson.h:1461) takes NO allocator: it allocates nothing,
     * so there is nothing to free and no heap to cross. */
    if (!buf || !doc || buf_len == 0) {
        if (err) {
            err->code = KIMIX_YYJSON_WRITE_ERROR_INVALID_PARAMETER;
            err->msg = "buffer or document is null";
        }
        return 0;
    }
    yyjson_write_err e;
    reset_write_err(e);
    const size_t written = yyjson_write_buf(buf, buf_len, doc, static_cast<yyjson_write_flag>(flags), &e);
    publish_write_err(e, err);
    return written;
}

void kimix_yyjson_str_free(char *s) {
    /* The counterpart of the four allocating writers: their buffers came from
     * mi_malloc through the mimalloc allocator, so they go back with mi_free --
     * never with the caller's free(), and never with a different module's
     * kimix_mem_free(). */
    if (s) {
        mi_free(s);
    }
}

// ===========================================================================
// 7. Mutable document: lifecycle
// ===========================================================================

kimix_yyjson_mut_doc *kimix_yyjson_mut_doc_new(void) {
    /* yyjson_mut_doc_new (yyjson.h:2494) REQUIRES an allocator argument; the
     * mimalloc one is baked in so the document is born in this heap. */
    return yyjson_mut_doc_new(mi_alc());
}

void kimix_yyjson_mut_doc_free(kimix_yyjson_mut_doc *doc) {
    if (doc) {
        yyjson_mut_doc_free(doc); /* yyjson.h:2490 */
    }
}

kimix_yyjson_mut_val *kimix_yyjson_mut_doc_root(kimix_yyjson_mut_doc *doc) {
    return doc ? yyjson_mut_doc_get_root(doc) : nullptr; /* yyjson.h:2448 */
}

void kimix_yyjson_mut_doc_set_root(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *root) {
    /* yyjson_mut_doc_set_root (yyjson.h:2452) is a void setter: it cannot fail
     * and cannot validate that `root` belongs to `doc`. */
    if (doc) {
        yyjson_mut_doc_set_root(doc, root);
    }
}

kimix_yyjson_doc *kimix_yyjson_mut_doc_imut_copy(const kimix_yyjson_mut_doc *doc) {
    /* yyjson_mut_doc_imut_copy (yyjson.h:2532) takes an allocator. */
    if (!doc) {
        return nullptr;
    }
    return yyjson_mut_doc_imut_copy(doc, mi_alc());
}

kimix_yyjson_mut_doc *kimix_yyjson_mut_doc_mut_copy(const kimix_yyjson_mut_doc *doc) {
    /* yyjson_mut_doc_mut_copy (yyjson.h:2507) takes an allocator. */
    if (!doc) {
        return nullptr;
    }
    return yyjson_mut_doc_mut_copy(doc, mi_alc());
}

// ===========================================================================
// 8. Mutable value creators
// ===========================================================================

/* All of them are `yyjson_api_inline` (static inline) in the fork: there is no
 * linkable symbol, so the code is instantiated in THIS translation unit and the
 * returned node is carved out of `doc`'s mimalloc pools.  A creator must be
 * given the document the value will live in; the value is never freed alone. */

kimix_yyjson_mut_val *kimix_yyjson_mut_null(kimix_yyjson_mut_doc *doc) { return doc ? yyjson_mut_null(doc) : nullptr; }   /* yyjson.h:2815 */
kimix_yyjson_mut_val *kimix_yyjson_mut_true(kimix_yyjson_mut_doc *doc) { return doc ? yyjson_mut_true(doc) : nullptr; }    /* yyjson.h:2818 */
kimix_yyjson_mut_val *kimix_yyjson_mut_false(kimix_yyjson_mut_doc *doc) { return doc ? yyjson_mut_false(doc) : nullptr; }  /* yyjson.h:2821 */
kimix_yyjson_mut_val *kimix_yyjson_mut_bool(kimix_yyjson_mut_doc *doc, bool val) { return doc ? yyjson_mut_bool(doc, val) : nullptr; } /* yyjson.h:2824 */
kimix_yyjson_mut_val *kimix_yyjson_mut_uint(kimix_yyjson_mut_doc *doc, uint64_t num) { return doc ? yyjson_mut_uint(doc, num) : nullptr; } /* yyjson.h:2828 */
kimix_yyjson_mut_val *kimix_yyjson_mut_sint(kimix_yyjson_mut_doc *doc, int64_t num) { return doc ? yyjson_mut_sint(doc, num) : nullptr; }  /* yyjson.h:2832 */
kimix_yyjson_mut_val *kimix_yyjson_mut_int(kimix_yyjson_mut_doc *doc, int64_t num) { return doc ? yyjson_mut_int(doc, num) : nullptr; }    /* yyjson.h:2836 */
kimix_yyjson_mut_val *kimix_yyjson_mut_real(kimix_yyjson_mut_doc *doc, double num) { return doc ? yyjson_mut_real(doc, num) : nullptr; }    /* yyjson.h:2848 */

kimix_yyjson_mut_val *kimix_yyjson_mut_str(kimix_yyjson_mut_doc *doc, const char *str) {
    /* yyjson_mut_str (yyjson.h:2855) stores the pointer and its strlen WITHOUT
     * COPYING: `str` must stay alive and unmodified for the life of `doc`.  The
     * guard matters -- a NULL would reach strlen(). */
    if (!doc || !str) {
        return nullptr;
    }
    return yyjson_mut_str(doc, str);
}

kimix_yyjson_mut_val *kimix_yyjson_mut_strn(kimix_yyjson_mut_doc *doc, const char *str, size_t len) {
    /* yyjson_mut_strn (yyjson.h:2862): also no copy; `len` bytes must stay valid. */
    if (!doc || !str) {
        return nullptr;
    }
    return yyjson_mut_strn(doc, str, len);
}

kimix_yyjson_mut_val *kimix_yyjson_mut_strcpy(kimix_yyjson_mut_doc *doc, const char *str) {
    /* yyjson_mut_strcpy (yyjson.h:2869) duplicates the bytes into the document's
     * string pool, so the caller's buffer is free to go afterwards. */
    if (!doc || !str) {
        return nullptr;
    }
    return yyjson_mut_strcpy(doc, str);
}

kimix_yyjson_mut_val *kimix_yyjson_mut_strncpy(kimix_yyjson_mut_doc *doc, const char *str, size_t len) {
    /* yyjson_mut_strncpy (yyjson.h:2875): copies `len` bytes. */
    if (!doc || !str) {
        return nullptr;
    }
    return yyjson_mut_strncpy(doc, str, len);
}

kimix_yyjson_mut_val *kimix_yyjson_mut_arr(kimix_yyjson_mut_doc *doc) { return doc ? yyjson_mut_arr(doc) : nullptr; } /* yyjson.h:3018 */
kimix_yyjson_mut_val *kimix_yyjson_mut_obj(kimix_yyjson_mut_doc *doc) { return doc ? yyjson_mut_obj(doc) : nullptr; } /* yyjson.h:3888 */

kimix_yyjson_mut_val *kimix_yyjson_mut_raw(kimix_yyjson_mut_doc *doc, const char *str) {
    /* yyjson_mut_raw (yyjson.h:2789): verbatim JSON text, not copied, not
     * validated -- the caller's buffer must outlive the document. */
    if (!doc || !str) {
        return nullptr;
    }
    return yyjson_mut_raw(doc, str);
}

// ===========================================================================
// 9. Mutable value: type checks, content getters and lookup
// ===========================================================================

bool kimix_yyjson_mut_is_raw(const kimix_yyjson_mut_val *v) { return v && yyjson_mut_is_raw(v); }         /* yyjson.h:2552 */
bool kimix_yyjson_mut_is_null(const kimix_yyjson_mut_val *v) { return v && yyjson_mut_is_null(v); }       /* yyjson.h:2556 */
bool kimix_yyjson_mut_is_true(const kimix_yyjson_mut_val *v) { return v && yyjson_mut_is_true(v); }       /* yyjson.h:2560 */
bool kimix_yyjson_mut_is_false(const kimix_yyjson_mut_val *v) { return v && yyjson_mut_is_false(v); }     /* yyjson.h:2564 */
bool kimix_yyjson_mut_is_bool(const kimix_yyjson_mut_val *v) { return v && yyjson_mut_is_bool(v); }       /* yyjson.h:2568 */
bool kimix_yyjson_mut_is_uint(const kimix_yyjson_mut_val *v) { return v && yyjson_mut_is_uint(v); }       /* yyjson.h:2572 */
bool kimix_yyjson_mut_is_sint(const kimix_yyjson_mut_val *v) { return v && yyjson_mut_is_sint(v); }       /* yyjson.h:2576 */
bool kimix_yyjson_mut_is_int(const kimix_yyjson_mut_val *v) { return v && yyjson_mut_is_int(v); }         /* yyjson.h:2580 */
bool kimix_yyjson_mut_is_real(const kimix_yyjson_mut_val *v) { return v && yyjson_mut_is_real(v); }       /* yyjson.h:2584 */
bool kimix_yyjson_mut_is_num(const kimix_yyjson_mut_val *v) { return v && yyjson_mut_is_num(v); }         /* yyjson.h:2588 */
bool kimix_yyjson_mut_is_str(const kimix_yyjson_mut_val *v) { return v && yyjson_mut_is_str(v); }         /* yyjson.h:2592 */
bool kimix_yyjson_mut_is_arr(const kimix_yyjson_mut_val *v) { return v && yyjson_mut_is_arr(v); }         /* yyjson.h:2596 */
bool kimix_yyjson_mut_is_obj(const kimix_yyjson_mut_val *v) { return v && yyjson_mut_is_obj(v); }         /* yyjson.h:2600 */
bool kimix_yyjson_mut_is_ctn(const kimix_yyjson_mut_val *v) { return v && yyjson_mut_is_ctn(v); }         /* yyjson.h:2604 */

uint8_t kimix_yyjson_mut_get_type(const kimix_yyjson_mut_val *v) {
    return v ? static_cast<uint8_t>(yyjson_mut_get_type(v)) : static_cast<uint8_t>(KIMIX_YYJSON_TYPE_NONE); /* yyjson.h:2614 */
}

const char *kimix_yyjson_mut_get_type_desc(const kimix_yyjson_mut_val *v) {
    return v ? yyjson_mut_get_type_desc(v) : "unknown"; /* yyjson.h:2628 */
}

bool kimix_yyjson_mut_get_bool(const kimix_yyjson_mut_val *v) { return v ? yyjson_mut_get_bool(v) : false; } /* yyjson.h:2637 */
uint64_t kimix_yyjson_mut_get_uint(const kimix_yyjson_mut_val *v) { return v ? yyjson_mut_get_uint(v) : 0; } /* yyjson.h:2641 */
int64_t kimix_yyjson_mut_get_sint(const kimix_yyjson_mut_val *v) { return v ? yyjson_mut_get_sint(v) : 0; }  /* yyjson.h:2645 */
int32_t kimix_yyjson_mut_get_int(const kimix_yyjson_mut_val *v) { return v ? static_cast<int32_t>(yyjson_mut_get_int(v)) : 0; } /* yyjson.h:2649 */
double kimix_yyjson_mut_get_real(const kimix_yyjson_mut_val *v) { return v ? yyjson_mut_get_real(v) : 0.0; } /* yyjson.h:2653 */
double kimix_yyjson_mut_get_num(const kimix_yyjson_mut_val *v) { return v ? yyjson_mut_get_num(v) : 0.0; }   /* yyjson.h:2657 */

const char *kimix_yyjson_mut_get_str(const kimix_yyjson_mut_val *v) {
    /* yyjson_mut_get_str (yyjson.h:2661).  For a value made by mut_str / mut_strn
     * the bytes are the CALLER's, not a copy. */
    return v ? yyjson_mut_get_str(v) : nullptr;
}

size_t kimix_yyjson_mut_get_len(const kimix_yyjson_mut_val *v) { return v ? yyjson_mut_get_len(v) : 0; }           /* yyjson.h:2666 */
const char *kimix_yyjson_mut_get_raw(const kimix_yyjson_mut_val *v) { return v ? yyjson_mut_get_raw(v) : nullptr; } /* yyjson.h:2633 */

bool kimix_yyjson_mut_equals_str(const kimix_yyjson_mut_val *v, const char *str) {
    return v && str && yyjson_mut_equals_str(v, str); /* yyjson.h:2671 */
}

bool kimix_yyjson_mut_equals_strn(const kimix_yyjson_mut_val *v, const char *str, size_t len) {
    return v && str && yyjson_mut_equals_strn(v, str, len); /* yyjson.h:2677 */
}

size_t kimix_yyjson_mut_arr_size(const kimix_yyjson_mut_val *arr) { return arr ? yyjson_mut_arr_size(arr) : 0; } /* yyjson.h:2887 */

kimix_yyjson_mut_val *kimix_yyjson_mut_arr_get(const kimix_yyjson_mut_val *arr, size_t idx) {
    return arr ? yyjson_mut_arr_get(arr, idx) : nullptr; /* yyjson.h:2892, always O(idx) */
}

kimix_yyjson_mut_val *kimix_yyjson_mut_arr_get_first(const kimix_yyjson_mut_val *arr) {
    return arr ? yyjson_mut_arr_get_first(arr) : nullptr; /* yyjson.h:2897 */
}

kimix_yyjson_mut_val *kimix_yyjson_mut_arr_get_last(const kimix_yyjson_mut_val *arr) {
    return arr ? yyjson_mut_arr_get_last(arr) : nullptr; /* yyjson.h:2902 */
}

size_t kimix_yyjson_mut_obj_size(const kimix_yyjson_mut_val *obj) { return obj ? yyjson_mut_obj_size(obj) : 0; } /* yyjson.h:3695 */

kimix_yyjson_mut_val *kimix_yyjson_mut_obj_get(const kimix_yyjson_mut_val *obj, const char *key) {
    return (obj && key) ? yyjson_mut_obj_get(obj, key) : nullptr; /* yyjson.h:3704, linear scan */
}

kimix_yyjson_mut_val *kimix_yyjson_mut_obj_getn(const kimix_yyjson_mut_val *obj, const char *key, size_t key_len) {
    return (obj && key) ? yyjson_mut_obj_getn(obj, key, key_len) : nullptr; /* yyjson.h:3715 */
}

// ===========================================================================
// 10. Mutable array mutation
// ===========================================================================

bool kimix_yyjson_mut_arr_append(kimix_yyjson_mut_val *arr, kimix_yyjson_mut_val *val) {
    return arr && val && yyjson_mut_arr_append(arr, val); /* yyjson.h:3394, O(1) */
}

bool kimix_yyjson_mut_arr_prepend(kimix_yyjson_mut_val *arr, kimix_yyjson_mut_val *val) {
    return arr && val && yyjson_mut_arr_prepend(arr, val); /* yyjson.h:3404, O(1) */
}

bool kimix_yyjson_mut_arr_insert(kimix_yyjson_mut_val *arr, kimix_yyjson_mut_val *val, size_t idx) {
    return arr && val && yyjson_mut_arr_insert(arr, val, idx); /* yyjson.h:3384, O(idx) */
}

kimix_yyjson_mut_val *kimix_yyjson_mut_arr_remove(kimix_yyjson_mut_val *arr, size_t idx) {
    /* yyjson_mut_arr_remove (yyjson.h:3430) returns the removed node -- still
     * owned by the document, never freed on its own. */
    return arr ? yyjson_mut_arr_remove(arr, idx) : nullptr;
}

kimix_yyjson_mut_val *kimix_yyjson_mut_arr_remove_first(kimix_yyjson_mut_val *arr) {
    return arr ? yyjson_mut_arr_remove_first(arr) : nullptr; /* yyjson.h:3439 */
}

kimix_yyjson_mut_val *kimix_yyjson_mut_arr_remove_last(kimix_yyjson_mut_val *arr) {
    return arr ? yyjson_mut_arr_remove_last(arr) : nullptr; /* yyjson.h:3448 */
}

kimix_yyjson_mut_val *kimix_yyjson_mut_arr_replace(kimix_yyjson_mut_val *arr, size_t idx, kimix_yyjson_mut_val *val) {
    /* yyjson_mut_arr_replace (yyjson.h:3417) returns the displaced node. */
    return (arr && val) ? yyjson_mut_arr_replace(arr, idx, val) : nullptr;
}

bool kimix_yyjson_mut_arr_clear(kimix_yyjson_mut_val *arr) {
    return arr && yyjson_mut_arr_clear(arr); /* yyjson.h:3469 */
}

bool kimix_yyjson_mut_arr_add_null(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *arr) {
    return doc && arr && yyjson_mut_arr_add_null(doc, arr); /* yyjson.h:3504 */
}

bool kimix_yyjson_mut_arr_add_bool(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *arr, bool val) {
    return doc && arr && yyjson_mut_arr_add_bool(doc, arr, val); /* yyjson.h:3535 */
}

bool kimix_yyjson_mut_arr_add_uint(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *arr, uint64_t num) {
    return doc && arr && yyjson_mut_arr_add_uint(doc, arr, num); /* yyjson.h:3547 */
}

bool kimix_yyjson_mut_arr_add_sint(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *arr, int64_t num) {
    return doc && arr && yyjson_mut_arr_add_sint(doc, arr, num); /* yyjson.h:3559 */
}

bool kimix_yyjson_mut_arr_add_real(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *arr, double num) {
    return doc && arr && yyjson_mut_arr_add_real(doc, arr, num); /* yyjson.h:3607 */
}

bool kimix_yyjson_mut_arr_add_str(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *arr, const char *str) {
    /* yyjson_mut_arr_add_str (yyjson.h:3621): the string is NOT copied. */
    return doc && arr && str && yyjson_mut_arr_add_str(doc, arr, str);
}

bool kimix_yyjson_mut_arr_add_strn(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *arr, const char *str, size_t len) {
    return doc && arr && str && yyjson_mut_arr_add_strn(doc, arr, str, len); /* yyjson.h:3636, not copied */
}

bool kimix_yyjson_mut_arr_add_strcpy(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *arr, const char *str) {
    return doc && arr && str && yyjson_mut_arr_add_strcpy(doc, arr, str); /* yyjson.h:3649, copied */
}

bool kimix_yyjson_mut_arr_add_val(kimix_yyjson_mut_val *arr, kimix_yyjson_mut_val *val) {
    /* yyjson_mut_arr_add_val (yyjson.h:3494) is an alias of append. */
    return arr && val && yyjson_mut_arr_add_val(arr, val);
}

kimix_yyjson_mut_val *kimix_yyjson_mut_arr_add_arr(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *arr) {
    /* yyjson_mut_arr_add_arr (yyjson.h:3674): create-and-append, returns the NEW
     * sub-array borrowed from `doc`. */
    return (doc && arr) ? yyjson_mut_arr_add_arr(doc, arr) : nullptr;
}

kimix_yyjson_mut_val *kimix_yyjson_mut_arr_add_obj(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *arr) {
    return (doc && arr) ? yyjson_mut_arr_add_obj(doc, arr) : nullptr; /* yyjson.h:3684 */
}

// ===========================================================================
// 11. Mutable object mutation
// ===========================================================================

bool kimix_yyjson_mut_obj_add(kimix_yyjson_mut_val *obj, kimix_yyjson_mut_val *key, kimix_yyjson_mut_val *val) {
    /* yyjson_mut_obj_add (yyjson.h:3943): key must be a string value of the same
     * document; duplicates are allowed and become visible in the output. */
    if (!obj || !key || !val) {
        return false;
    }
    return yyjson_mut_obj_add(obj, key, val);
}

bool kimix_yyjson_mut_obj_put(kimix_yyjson_mut_val *obj, kimix_yyjson_mut_val *key, kimix_yyjson_mut_val *val) {
    /* yyjson_mut_obj_put (yyjson.h:3956) removes every existing pair with the
     * same key first, and a NULL `val` is its documented delete form -- so `val`
     * is deliberately NOT null-checked here. */
    if (!obj || !key) {
        return false;
    }
    return yyjson_mut_obj_put(obj, key, val);
}

kimix_yyjson_mut_val *kimix_yyjson_mut_obj_remove(kimix_yyjson_mut_val *obj, const char *key) {
    /* Maps to yyjson_mut_obj_remove_key (yyjson.h:3992): the fork's
     * yyjson_mut_obj_remove takes a key VALUE, not a C string.  Returns the first
     * value that was unlinked, borrowed from the document. */
    if (!obj || !key) {
        return nullptr;
    }
    return yyjson_mut_obj_remove_key(obj, key);
}

kimix_yyjson_mut_val *kimix_yyjson_mut_obj_remove_keyn(kimix_yyjson_mut_val *obj, const char *key, size_t key_len) {
    if (!obj || !key) {
        return nullptr;
    }
    return yyjson_mut_obj_remove_keyn(obj, key, key_len); /* yyjson.h:4003 */
}

bool kimix_yyjson_mut_obj_clear(kimix_yyjson_mut_val *obj) {
    return obj && yyjson_mut_obj_clear(obj); /* yyjson.h:4011 */
}

bool kimix_yyjson_mut_obj_rename_key(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *obj, const char *key, const char *new_key) {
    /* yyjson_mut_obj_rename_key (yyjson.h:4256): the new key text is COPIED into
     * the document's string pool; the old key node is overwritten in place. */
    if (!doc || !obj || !key || !new_key) {
        return false;
    }
    return yyjson_mut_obj_rename_key(doc, obj, key, new_key);
}

bool kimix_yyjson_mut_obj_add_null(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *obj, const char *key) {
    /* The yyjson_mut_obj_add_* sugar builds the key WITHOUT copying it
     * (yyjson.h:4050): `key` must stay alive for the life of `doc`. */
    return doc && obj && key && yyjson_mut_obj_add_null(doc, obj, key);
}

bool kimix_yyjson_mut_obj_add_bool(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *obj, const char *key, bool val) {
    return doc && obj && key && yyjson_mut_obj_add_bool(doc, obj, key, val); /* yyjson.h:4080 */
}

bool kimix_yyjson_mut_obj_add_uint(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *obj, const char *key, uint64_t val) {
    return doc && obj && key && yyjson_mut_obj_add_uint(doc, obj, key, val); /* yyjson.h:4090 */
}

bool kimix_yyjson_mut_obj_add_sint(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *obj, const char *key, int64_t val) {
    return doc && obj && key && yyjson_mut_obj_add_sint(doc, obj, key, val); /* yyjson.h:4100 */
}

bool kimix_yyjson_mut_obj_add_real(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *obj, const char *key, double val) {
    return doc && obj && key && yyjson_mut_obj_add_real(doc, obj, key, val); /* yyjson.h:4140 */
}

bool kimix_yyjson_mut_obj_add_str(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *obj, const char *key, const char *val) {
    /* yyjson.h:4150 -- neither the key nor the value is copied. */
    return doc && obj && key && val && yyjson_mut_obj_add_str(doc, obj, key, val);
}

bool kimix_yyjson_mut_obj_add_strn(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *obj, const char *key, const char *val, size_t len) {
    return doc && obj && key && val && yyjson_mut_obj_add_strn(doc, obj, key, val, len); /* yyjson.h:4162, not copied */
}

bool kimix_yyjson_mut_obj_add_strcpy(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *obj, const char *key, const char *val) {
    /* yyjson.h:4174: the KEY is still not copied, only the value is. */
    return doc && obj && key && val && yyjson_mut_obj_add_strcpy(doc, obj, key, val);
}

bool kimix_yyjson_mut_obj_add_val(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *obj, const char *key, kimix_yyjson_mut_val *val) {
    /* yyjson_mut_obj_add_val (yyjson.h:4224): attaches an existing value under a
     * key the fork creates from `key` (not copied). */
    return doc && obj && key && val && yyjson_mut_obj_add_val(doc, obj, key, val);
}

kimix_yyjson_mut_val *kimix_yyjson_mut_obj_add_arr(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *obj, const char *key) {
    /* yyjson_mut_obj_add_arr (yyjson.h:4201): create-and-add, returns the new
     * sub-array borrowed from `doc`; `key` is not copied. */
    return (doc && obj && key) ? yyjson_mut_obj_add_arr(doc, obj, key) : nullptr;
}

kimix_yyjson_mut_val *kimix_yyjson_mut_obj_add_obj(kimix_yyjson_mut_doc *doc, kimix_yyjson_mut_val *obj, const char *key) {
    return (doc && obj && key) ? yyjson_mut_obj_add_obj(doc, obj, key) : nullptr; /* yyjson.h:4214 */
}

// ===========================================================================
// 12. Writing a mutable document / value
// ===========================================================================

char *kimix_yyjson_mut_write(const kimix_yyjson_mut_doc *doc, uint32_t flags, size_t *len, kimix_yyjson_write_err *err) {
    /* yyjson_mut_write_opts (yyjson.h:1510) with the mimalloc allocator -- NEVER
     * yyjson_mut_write (yyjson.h:1614), the alc-less inline.  TRANSFERRED result;
     * release it with kimix_yyjson_str_free(). */
    yyjson_write_err e;
    reset_write_err(e);
    size_t written = 0;
    auto *s = yyjson_mut_write_opts(doc, static_cast<yyjson_write_flag>(flags), mi_alc(), &written, &e);
    return finish_write(s, e, written, len, err);
}

size_t kimix_yyjson_mut_write_buf(char *buf, size_t buf_len, const kimix_yyjson_mut_doc *doc, uint32_t flags, kimix_yyjson_write_err *err) {
    /* yyjson_mut_write_buf (yyjson.h:1593): no allocator, no allocation. */
    if (!buf || !doc || buf_len == 0) {
        if (err) {
            err->code = KIMIX_YYJSON_WRITE_ERROR_INVALID_PARAMETER;
            err->msg = "buffer or document is null";
        }
        return 0;
    }
    yyjson_write_err e;
    reset_write_err(e);
    const size_t written = yyjson_mut_write_buf(buf, buf_len, doc, static_cast<yyjson_write_flag>(flags), &e);
    publish_write_err(e, err);
    return written;
}

char *kimix_yyjson_mut_val_write(const kimix_yyjson_mut_val *v, uint32_t flags, size_t *len, kimix_yyjson_write_err *err) {
    /* yyjson_mut_val_write_opts (yyjson.h:1775); the alc-less yyjson_mut_val_write
     * (yyjson.h:1879) is never called.  TRANSFERRED result, freed with
     * kimix_yyjson_str_free(). */
    if (!v) {
        if (len) {
            *len = 0;
        }
        yyjson_write_err e;
        reset_write_err(e);
        e.code = YYJSON_WRITE_ERROR_INVALID_PARAMETER;
        e.msg = "value is null";
        publish_write_err(e, err);
        return nullptr;
    }
    yyjson_write_err e;
    reset_write_err(e);
    size_t written = 0;
    auto *s = yyjson_mut_val_write_opts(v, static_cast<yyjson_write_flag>(flags), mi_alc(), &written, &e);
    return finish_write(s, e, written, len, err);
}

KIMIX_FFI_END
