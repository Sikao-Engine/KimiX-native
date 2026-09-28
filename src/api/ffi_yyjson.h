/*
 * ffi_yyjson.h -- the vendored yyjson JSON library (src/ext/yyjson, fork of
 * yyjson 0.13.0) exported to plain C, with the allocator HIDDEN.
 *
 * WHY THE ALLOCATOR IS NOT PART OF THIS SURFACE
 * ---------------------------------------------
 * yyjson is allocator-parametric: almost every entry point of it takes a
 * `const yyjson_alc *`.  An allocator is a struct of three function pointers
 * plus a context pointer, and the identity of the heap those pointers reach is
 * decided by whoever fills them in.  Exporting it would let a caller install a
 * `malloc`-based allocator, and then a document's memory would be allocated in
 * the caller's module and freed in this one (or the other way round) -- a
 * cross-heap free, which is exactly the bug class this FFI exists to avoid.
 *
 * So `yyjson_alc` never appears here, and no exported function takes one.
 * Internally every call that needs an allocator is given
 * `kimix::api::yyjson_mi_alc()` (src/api/detail.h): the mimalloc heap this
 * library already owns.  Consequences a caller must know:
 *
 *   * A `kimix_yyjson_doc` / `kimix_yyjson_mut_doc` owns mimalloc memory and is
 *     released with kimix_yyjson_doc_free() / kimix_yyjson_mut_doc_free(), which
 *     route through the allocator recorded in the document.
 *   * The writers return a buffer allocated from that same mimalloc heap.  It
 *     must be released with kimix_yyjson_str_free() (mi_free), NEVER with the
 *     caller's free(), and never with a kimix_mem_free() resolved in another
 *     module.
 *   * For the same reason this FFI never calls the short yyjson convenience
 *     forms `yyjson_read` / `yyjson_write` / `yyjson_val_write` /
 *     `yyjson_mut_write`: they are `yyjson_api_inline` wrappers that pass
 *     `alc == NULL`, which makes yyjson use the libc allocator, and the result
 *     is the cross-heap bug above.  ffi_yyjson.cpp therefore only ever calls the
 *     `*_opts` variants with the mimalloc allocator.
 *
 * OPAQUE HANDLES
 * --------------
 * The four yyjson object types are published as new typedef names for the SAME
 * incomplete struct tags (`struct yyjson_doc`, ...).  No size, no field, and no
 * yyjson.h include crosses this boundary: a C compiler sees four forward
 * declarations and may only pass the pointers back in.  Inside the library the
 * two typedefs name one and the same type, so the pointers convert implicitly
 * and no cast is performed.
 *
 * WHAT IS DELIBERATELY NOT EXPORTED
 * ---------------------------------
 * * the allocator interface and the allocator helpers (`yyjson_alc_*`),
 * * the `*_file` and `*_fp` read/write functions: a `FILE *` is a CRT object
 *   whose buffer, locale and lock live in the module that opened it, so passing
 *   one across a Windows DLL boundary (two modules, two CRT heaps) is a
 *   memory-safety bug regardless of what the JSON on the other side looks like.
 *   Open the file in the caller and hand the bytes over with kimix_yyjson_read(),
 * * the `unsafe_yyjson_*` family: it dereferences its argument without checking,
 *   which this FFI cannot promise,
 * * the `yyjson_set_*` family: those "modify" an immutable value in place, which
 *   is only valid on memory the caller knows how to re-tag; build a mutable
 *   document instead,
 * * the JSON-Pointer (`*_ptr_*`) and Merge-Patch (`yyjson_*_patch`) utilities and
 *   the deprecated `*_get_pointer*` spellings,
 * * the iterators `yyjson_arr_iter` / `yyjson_obj_iter`: they are small POD
 *   structs whose layout would become ABI.  Use the index/key accessors below --
 *   note that `kimix_yyjson_arr_get` is O(1) only for a "flat" array (all
 *   elements inline in the parent's block); for an array whose elements were
 *   allocated separately it walks the element chain, i.e. O(idx).  Sequential
 *   traversal of such an array through the index accessor is therefore quadratic.
 * * the mutable-document pool tunables (`yyjson_mut_doc_set_str_pool_size`,
 *   `yyjson_mut_doc_set_val_pool_size`): pure performance hints, and a wrong
 *   value is a failed allocation rather than a wrong result.
 *
 * IMMUTABLE vs MUTABLE VALUES
 * ---------------------------
 * `kimix_yyjson_val` (from a parsed or `imut_copy`'d document) and
 * `kimix_yyjson_mut_val` (from a mutable document) are DIFFERENT types with
 * DIFFERENT layouts (16 vs 24 bytes).  Sections 2-5 take the immutable one,
 * sections 9-11 the mutable one; handing a mutable value to an immutable
 * accessor (or the reverse) is a type error the FFI cannot detect -- it would
 * read the wrong fields.  Convert with kimix_yyjson_doc_mut_copy() /
 * kimix_yyjson_mut_doc_imut_copy() instead.
 *
 * NULL SAFETY
 * -----------
 * yyjson's own public accessors are mostly NULL-tolerant, but the `unsafe_*`
 * primitives they sit on are not, and a caller of a C ABI must not have to read
 * the implementation to find out.  Every function below therefore checks its
 * pointer arguments first and returns the documented neutral value (false / 0 /
 * NULL) instead of dereferencing NULL.  The library is built without C++
 * exceptions, and nothing in yyjson throws one either: every failure here is a
 * return value.
 *
 * The full documented index of the surface is docs/ffi.md.
 */
#pragma once
#ifndef KIMIX_API_FFI_YYJSON_H
#define KIMIX_API_FFI_YYJSON_H

#include <api/ffi_common.h>

/* ---------------------------------------------------------------------------
 * The opaque handles
 * ------------------------------------------------------------------------- */

/* An immutable document: the result of parsing, or of converting a mutable
 * document.  Owns the memory of every value and string it contains; release the
 * whole thing with kimix_yyjson_doc_free(). */
typedef struct yyjson_doc kimix_yyjson_doc;

/* A value inside an immutable document.  KIMIX_BORROWED: its lifetime is exactly
 * the lifetime of its document, it cannot be freed on its own. */
typedef struct yyjson_val kimix_yyjson_val;

/* A mutable document: created empty with kimix_yyjson_mut_doc_new() and built up
 * with the creators/mutators of sections 8-11.  Release with
 * kimix_yyjson_mut_doc_free(). */
typedef struct yyjson_mut_doc kimix_yyjson_mut_doc;

/* A value inside a mutable document.  KIMIX_BORROWED from the document. */
typedef struct yyjson_mut_val kimix_yyjson_mut_val;

/* ---------------------------------------------------------------------------
 * JSON value types (yyjson's `yyjson_type`, src/ext/yyjson/src/yyjson.h
 * lines 604-618).  These literals are COPIES of the YYJSON_TYPE_* defines;
 * ffi_yyjson.cpp static_asserts every one of them against the real header, so a
 * fork that changes a value breaks the build instead of the caller.
 * A bool is TYPE_BOOL (use the is_true / is_false predicates, or
 * get_type_desc(), to tell the two subtypes apart); an integer is TYPE_NUM with
 * a uint / sint / real subtype.
 * ------------------------------------------------------------------------- */

#define KIMIX_YYJSON_TYPE_NONE ((uint8_t)0) /* not a value: a NULL pointer was passed */
#define KIMIX_YYJSON_TYPE_RAW ((uint8_t)1)  /* a number kept as its text */
#define KIMIX_YYJSON_TYPE_NULL ((uint8_t)2) /* the null literal */
#define KIMIX_YYJSON_TYPE_BOOL ((uint8_t)3) /* true or false */
#define KIMIX_YYJSON_TYPE_NUM ((uint8_t)4)  /* uint, sint or real */
#define KIMIX_YYJSON_TYPE_STR ((uint8_t)5)  /* a string */
#define KIMIX_YYJSON_TYPE_ARR ((uint8_t)6)  /* an array */
#define KIMIX_YYJSON_TYPE_OBJ ((uint8_t)7)  /* an object */

/* ---------------------------------------------------------------------------
 * Read flags.  yyjson declares these as `static const yyjson_read_flag` OBJECTS
 * (lines 816-909), not as preprocessor constants, so they cannot be reused
 * across the boundary: the literals below are copied from that range and
 * static_assert'd against the objects in ffi_yyjson.cpp.  Combine with '|';
 * KIMIX_YYJSON_READ_NOFLAG (0) is strict RFC 8259.
 * ------------------------------------------------------------------------- */

#define KIMIX_YYJSON_READ_NOFLAG 0u                  /* strict RFC 8259 */
#define KIMIX_YYJSON_READ_INSITU 1u                  /* (1 << 0)  parse in place */
#define KIMIX_YYJSON_READ_STOP_WHEN_DONE 2u          /* (1 << 1)  tolerate content after the value */
#define KIMIX_YYJSON_READ_ALLOW_TRAILING_COMMAS 4u   /* (1 << 2)  [1,2,3,] */
#define KIMIX_YYJSON_READ_ALLOW_COMMENTS 8u          /* (1 << 3)  C and C++ style comments */
#define KIMIX_YYJSON_READ_ALLOW_INF_AND_NAN 16u      /* (1 << 4)  inf / NaN literals */
#define KIMIX_YYJSON_READ_NUMBER_AS_RAW 32u          /* (1 << 5)  every number becomes TYPE_RAW */
#define KIMIX_YYJSON_READ_ALLOW_INVALID_UNICODE 64u  /* (1 << 6)  tolerate bad UTF-8 */
#define KIMIX_YYJSON_READ_BIGNUM_AS_RAW 128u         /* (1 << 7)  unrepresentable numbers as TYPE_RAW */
#define KIMIX_YYJSON_READ_ALLOW_BOM 256u             /* (1 << 8)  skip a UTF-8 BOM */
#define KIMIX_YYJSON_READ_ALLOW_EXT_NUMBER 512u      /* (1 << 9)  0x7B, .1, 1., +1 */
#define KIMIX_YYJSON_READ_ALLOW_EXT_ESCAPE 1024u     /* (1 << 10) \a \e \v \' \? \0 \xNN */
#define KIMIX_YYJSON_READ_ALLOW_EXT_WHITESPACE 2048u /* (1 << 11) \v \f U+2028 NBSP ... */
#define KIMIX_YYJSON_READ_ALLOW_SINGLE_QUOTED_STR 4096u /* (1 << 12) 'ab' */
#define KIMIX_YYJSON_READ_ALLOW_UNQUOTED_KEY 8192u      /* (1 << 13) {a:1} */
/* The JSON5 bundle: trailing commas | comments | inf-and-nan | ext number | ext
 * escape | ext whitespace | single-quoted strings | unquoted keys (= 15900). */
#define KIMIX_YYJSON_READ_JSON5                                                                  \
    (KIMIX_YYJSON_READ_ALLOW_TRAILING_COMMAS | KIMIX_YYJSON_READ_ALLOW_COMMENTS |                \
     KIMIX_YYJSON_READ_ALLOW_INF_AND_NAN | KIMIX_YYJSON_READ_ALLOW_EXT_NUMBER |                  \
     KIMIX_YYJSON_READ_ALLOW_EXT_ESCAPE | KIMIX_YYJSON_READ_ALLOW_EXT_WHITESPACE |               \
     KIMIX_YYJSON_READ_ALLOW_SINGLE_QUOTED_STR | KIMIX_YYJSON_READ_ALLOW_UNQUOTED_KEY)

/* ---------------------------------------------------------------------------
 * Write flags (the `static const yyjson_write_flag` objects of lines 1252-1287
 * and the two FP macros of lines 1302-1309).  Combine with '|';
 * KIMIX_YYJSON_WRITE_NOFLAG (0) is minified and strict.
 * ------------------------------------------------------------------------- */

#define KIMIX_YYJSON_WRITE_NOFLAG 0u                 /* minify */
#define KIMIX_YYJSON_WRITE_PRETTY 1u                 /* (1 << 0)  indent with 4 spaces */
#define KIMIX_YYJSON_WRITE_ESCAPE_UNICODE 2u         /* (1 << 1)  \uXXXX, the output is ASCII only */
#define KIMIX_YYJSON_WRITE_ESCAPE_SLASHES 4u         /* (1 << 2)  escape the forward slash */
#define KIMIX_YYJSON_WRITE_ALLOW_INF_AND_NAN 8u      /* (1 << 3)  write Infinity / NaN */
#define KIMIX_YYJSON_WRITE_INF_AND_NAN_AS_NULL 16u   /* (1 << 4)  write null instead (wins over the above) */
#define KIMIX_YYJSON_WRITE_ALLOW_INVALID_UNICODE 32u /* (1 << 5)  copy bad UTF-8 bytes through */
#define KIMIX_YYJSON_WRITE_PRETTY_TWO_SPACES 64u     /* (1 << 6)  indent with 2 spaces (wins over PRETTY) */
#define KIMIX_YYJSON_WRITE_NEWLINE_AT_END 128u       /* (1 << 7)  append a '\n' (NDJSON friendly) */
#define KIMIX_YYJSON_WRITE_LOWERCASE_HEX 256u        /* (1 << 8)  \uffff instead of \uFFFF */
/* The top 8 bits of the flag word (and the top 4 of them, the precision) select
 * the floating-point output format; see yyjson.h:1291-1309. */
#define KIMIX_YYJSON_WRITE_FP_FLAG_BITS 8u
#define KIMIX_YYJSON_WRITE_FP_PREC_BITS 4u
/* Fixed-point number output: `prec` in 1..15, in the top 4 bits of the flag
 * (yyjson.h:1302).  Use 0 to leave the field empty. */
#define KIMIX_YYJSON_WRITE_FP_TO_FIXED(prec) ((uint32_t)((uint32_t)(prec) << 28u))
/* Round doubles to single precision before printing (yyjson.h:1309). */
#define KIMIX_YYJSON_WRITE_FP_TO_FLOAT ((uint32_t)1u << 27u)

/* ---------------------------------------------------------------------------
 * Error codes: the `static const yyjson_read_code` objects of lines 917-962 and
 * the `static const yyjson_write_code` objects of lines 1317-1341.
 * ------------------------------------------------------------------------- */

#define KIMIX_YYJSON_READ_SUCCESS 0u
#define KIMIX_YYJSON_READ_ERROR_INVALID_PARAMETER 1u
#define KIMIX_YYJSON_READ_ERROR_MEMORY_ALLOCATION 2u
#define KIMIX_YYJSON_READ_ERROR_EMPTY_CONTENT 3u
#define KIMIX_YYJSON_READ_ERROR_UNEXPECTED_CONTENT 4u
#define KIMIX_YYJSON_READ_ERROR_UNEXPECTED_END 5u
#define KIMIX_YYJSON_READ_ERROR_UNEXPECTED_CHARACTER 6u
#define KIMIX_YYJSON_READ_ERROR_JSON_STRUCTURE 7u
#define KIMIX_YYJSON_READ_ERROR_INVALID_COMMENT 8u /* deprecated: an unclosed comment now reports UNEXPECTED_END */
#define KIMIX_YYJSON_READ_ERROR_INVALID_NUMBER 9u
#define KIMIX_YYJSON_READ_ERROR_INVALID_STRING 10u
#define KIMIX_YYJSON_READ_ERROR_LITERAL 11u
#define KIMIX_YYJSON_READ_ERROR_FILE_OPEN 12u /* unreachable here: no *_file entry point is exported */
#define KIMIX_YYJSON_READ_ERROR_FILE_READ 13u /* unreachable here: no *_file entry point is exported */
#define KIMIX_YYJSON_READ_ERROR_MORE 14u      /* incremental parsing only; never produced by kimix_yyjson_read */
#define KIMIX_YYJSON_READ_ERROR_DEPTH 15u

#define KIMIX_YYJSON_WRITE_SUCCESS 0u
#define KIMIX_YYJSON_WRITE_ERROR_INVALID_PARAMETER 1u
#define KIMIX_YYJSON_WRITE_ERROR_MEMORY_ALLOCATION 2u
#define KIMIX_YYJSON_WRITE_ERROR_INVALID_VALUE_TYPE 3u
#define KIMIX_YYJSON_WRITE_ERROR_NAN_OR_INF 4u
#define KIMIX_YYJSON_WRITE_ERROR_FILE_OPEN 5u  /* unreachable here: no *_file entry point is exported */
#define KIMIX_YYJSON_WRITE_ERROR_FILE_WRITE 6u /* unreachable here: no *_file entry point is exported */
#define KIMIX_YYJSON_WRITE_ERROR_INVALID_STRING 7u
#define KIMIX_YYJSON_WRITE_ERROR_DEPTH 8u

/* ---------------------------------------------------------------------------
 * The error records.  These are the caller-side mirror of yyjson's
 * `yyjson_read_err` / `yyjson_write_err` PODs: same field order, same widths --
 * but the wrappers fill them FIELD BY FIELD and never reinterpret-cast them, so
 * the two types may evolve independently.  A NULL `err` argument means "I do not
 * want the reason"; the return value alone still tells success from failure.
 * ------------------------------------------------------------------------- */

/* Diagnosis of a failed kimix_yyjson_read*(). */
typedef struct kimix_yyjson_read_err {
    /* One of the KIMIX_YYJSON_READ_* codes above; 0 == success. */
    uint32_t code;
    /* A constant message owned by the library's static storage: KIMIX_BORROWED,
     * never free it, it stays valid for the process lifetime.  NULL when `code`
     * is KIMIX_YYJSON_READ_SUCCESS. */
    const char *msg;
    /* The byte position in the input the error was found at; 0 when unused. */
    size_t position;
} kimix_yyjson_read_err;

/* Diagnosis of a failed kimix_yyjson_write*() / ..._val_write*(). */
typedef struct kimix_yyjson_write_err {
    /* One of the KIMIX_YYJSON_WRITE_* codes above; 0 == success. */
    uint32_t code;
    /* A constant message owned by the library's static storage: KIMIX_BORROWED,
     * never free it.  NULL when `code` is KIMIX_YYJSON_WRITE_SUCCESS. */
    const char *msg;
} kimix_yyjson_write_err;

KIMIX_FFI_BEGIN

/* ===========================================================================
 * 0. Identity
 * ======================================================================== */

/* The yyjson version the library was built against, in hex
 * (major << 16 | minor << 8 | patch; 0.13.0 == 0x000D00).  Same value and
 * spelling as yyjson_version().  Never fails. */
KIMIX_FFI uint32_t kimix_yyjson_version(void);

/* ===========================================================================
 * 1. Immutable document: parse and lifecycle
 * ======================================================================== */

/* Parse JSON.  Wraps yyjson_read_opts() with the library's mimalloc allocator.
 *   - `data` is KIMIX_IN: the bytes are read (and possibly pointed into, see
 *     below), the pointer itself is not retained,
 *   - `len` is the byte length; a NUL terminator is NOT required,
 *   - `flags` is 0 or a combination of the KIMIX_YYJSON_READ_* macros,
 *   - `err` is KIMIX_NULLABLE and is ALWAYS written (success appears there as
 *     code == KIMIX_YYJSON_READ_SUCCESS).
 * String and raw values of the result point at the parsed bytes, and those are
 * the CALLER's buffer only when KIMIX_YYJSON_READ_INSITU was used: without the
 * flag the library copies the input into the document (mimalloc), so `data` may
 * be released as soon as this call returns.  With INSITU the caller must keep
 * `data` alive AND WRITABLE for the whole life of the document, padded by at
 * least 4 zero bytes past `len` (the fork's YYJSON_PADDING_SIZE); the pointer is
 * passed through as-is, which means INSITU on a truly const or unpadded buffer is
 * undefined behaviour -- the same caveat yyjson documents for its `char *dat`
 * parameter, and the reason this FFI hands out a `const char *`.
 * Returns a new document, or NULL on failure. */
KIMIX_FFI kimix_yyjson_doc *kimix_yyjson_read(KIMIX_IN const char *data,
                                             size_t len,
                                             uint32_t flags,
                                             KIMIX_OUT KIMIX_NULLABLE kimix_yyjson_read_err *err);

/* The same, with the input as a NUL-terminated string: the length is
 * strlen(str).  KIMIX_YYJSON_READ_INSITU is dropped here (a plain C string is
 * not guaranteed to be padded, and yyjson's own `yyjson_read` does the same),
 * everything else behaves as kimix_yyjson_read(). */
KIMIX_FFI kimix_yyjson_doc *kimix_yyjson_read_str(KIMIX_IN const char *str,
                                                  uint32_t flags,
                                                  KIMIX_OUT KIMIX_NULLABLE kimix_yyjson_read_err *err);

/* Release a document and every value it owns (wraps yyjson_doc_free(); the frees
 * go to the mimalloc heap recorded in the document).  All kimix_yyjson_val
 * pointers from it become dangling.  NULL is ignored. */
KIMIX_FFI void kimix_yyjson_doc_free(KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_doc *doc);

/* The root value (wraps yyjson_doc_get_root()).  KIMIX_BORROWED from `doc`;
 * NULL if `doc` is NULL.  Never free it on its own. */
KIMIX_FFI kimix_yyjson_val *kimix_yyjson_doc_root(KIMIX_BORROWED const kimix_yyjson_doc *doc);

/* The number of input bytes the document was read from
 * (wraps yyjson_doc_get_read_size()).  0 for a NULL document. */
KIMIX_FFI size_t kimix_yyjson_doc_read_size(KIMIX_IN const kimix_yyjson_doc *doc);

/* The number of values stored in the document
 * (wraps yyjson_doc_get_val_count()).  0 for a NULL document. */
KIMIX_FFI size_t kimix_yyjson_doc_val_count(KIMIX_IN const kimix_yyjson_doc *doc);

/* Deep-copy an immutable document into a mutable one
 * (wraps yyjson_doc_mut_copy() with the mimalloc allocator).  The strings are
 * copied into the new document's pool, so the source may be freed right
 * afterwards.
 * Returns a new document owned by the caller (free with
 * kimix_yyjson_mut_doc_free()), or NULL for a NULL input, a document without a
 * root, or an allocation failure. */
KIMIX_FFI kimix_yyjson_mut_doc *kimix_yyjson_doc_mut_copy(KIMIX_IN const kimix_yyjson_doc *doc);

/* ===========================================================================
 * 2. Immutable value: type checks
 * ======================================================================== */

/* Every predicate below wraps its same-named yyjson_is_*() function, only reads
 * `v`, and returns false when `v` is NULL or the value has another type. */

KIMIX_FFI bool kimix_yyjson_is_raw(KIMIX_IN const kimix_yyjson_val *v);
KIMIX_FFI bool kimix_yyjson_is_null(KIMIX_IN const kimix_yyjson_val *v);
KIMIX_FFI bool kimix_yyjson_is_true(KIMIX_IN const kimix_yyjson_val *v);
KIMIX_FFI bool kimix_yyjson_is_false(KIMIX_IN const kimix_yyjson_val *v);
KIMIX_FFI bool kimix_yyjson_is_bool(KIMIX_IN const kimix_yyjson_val *v);
KIMIX_FFI bool kimix_yyjson_is_uint(KIMIX_IN const kimix_yyjson_val *v);
KIMIX_FFI bool kimix_yyjson_is_sint(KIMIX_IN const kimix_yyjson_val *v);
/* True for uint OR sint (the fork's yyjson_is_int() is the integer test). */
KIMIX_FFI bool kimix_yyjson_is_int(KIMIX_IN const kimix_yyjson_val *v);
KIMIX_FFI bool kimix_yyjson_is_real(KIMIX_IN const kimix_yyjson_val *v);
KIMIX_FFI bool kimix_yyjson_is_num(KIMIX_IN const kimix_yyjson_val *v);
KIMIX_FFI bool kimix_yyjson_is_str(KIMIX_IN const kimix_yyjson_val *v);
KIMIX_FFI bool kimix_yyjson_is_arr(KIMIX_IN const kimix_yyjson_val *v);
KIMIX_FFI bool kimix_yyjson_is_obj(KIMIX_IN const kimix_yyjson_val *v);
/* Array or object (wraps yyjson_is_ctn()). */
KIMIX_FFI bool kimix_yyjson_is_ctn(KIMIX_IN const kimix_yyjson_val *v);

/* The value's type as one of the KIMIX_YYJSON_TYPE_* macros (wraps
 * yyjson_get_type(), the 3 type bits of the tag).  KIMIX_YYJSON_TYPE_NONE (0)
 * for a NULL `v`. */
KIMIX_FFI uint8_t kimix_yyjson_get_type(KIMIX_IN const kimix_yyjson_val *v);

/* A static description: "raw", "null", "string", "array", "object", "true",
 * "false", "uint", "sint", "real" or "unknown" (wraps yyjson_get_type_desc()).
 * KIMIX_BORROWED from the library's static storage -- never free it; unlike the
 * other borrowed pointers it outlives the document. */
KIMIX_FFI const char *kimix_yyjson_get_type_desc(KIMIX_IN const kimix_yyjson_val *v);

/* ===========================================================================
 * 3. Immutable value: content getters
 * ======================================================================== */

/* All read-only, all KIMIX_BORROWED, all NULL-safe.  A getter called on a value
 * of the wrong type returns the neutral value written below and does NOT
 * convert: check with the section 2 predicates first. */

/* true / false only (wraps yyjson_get_bool()).  false for NULL or a non-bool. */
KIMIX_FFI bool kimix_yyjson_get_bool(KIMIX_IN const kimix_yyjson_val *v);

/* The value as an unsigned integer (wraps yyjson_get_uint()).
 * 0 for NULL or a non-uint. */
KIMIX_FFI uint64_t kimix_yyjson_get_uint(KIMIX_IN const kimix_yyjson_val *v);

/* The value as a signed integer (wraps yyjson_get_sint()).
 * 0 for NULL or a non-sint. */
KIMIX_FFI int64_t kimix_yyjson_get_sint(KIMIX_IN const kimix_yyjson_val *v);

/* The value as an integer truncated to 32 bits (wraps yyjson_get_int(), which
 * the fork declares as plain `int`; `int` is 32 bits on every platform this
 * project builds -- x64 and arm64 under MSVC, clang or gcc -- which
 * ffi_yyjson.cpp static_asserts, so the cast does not change the width).
 * 0 for NULL or a non-integer. */
KIMIX_FFI int32_t kimix_yyjson_get_int(KIMIX_IN const kimix_yyjson_val *v);

/* The value as a double, only when it really is a real (wraps
 * yyjson_get_real()).  0.0 for NULL or a non-real. */
KIMIX_FFI double kimix_yyjson_get_real(KIMIX_IN const kimix_yyjson_val *v);

/* Any number as a double (wraps yyjson_get_num(): uint, sint and real are all
 * converted).  0.0 for NULL or a non-number; a uint64 too large for a double
 * loses precision, exactly as in yyjson. */
KIMIX_FFI double kimix_yyjson_get_num(KIMIX_IN const kimix_yyjson_val *v);

/* The string content (wraps yyjson_get_str()).  NUL-terminated UTF-8,
 * KIMIX_BORROWED from the document: valid until kimix_yyjson_doc_free() and no
 * longer than kimix_yyjson_get_len() says.  NULL for NULL or a non-string.  An
 * embedded NUL truncates C-string handling: use get_len / equals_strn for data
 * that may contain one. */
KIMIX_FFI const char *kimix_yyjson_get_str(KIMIX_BORROWED const kimix_yyjson_val *v);

/* The byte length of a string, raw or container value (wraps
 * yyjson_get_len()).  0 for NULL or a type without a length. */
KIMIX_FFI size_t kimix_yyjson_get_len(KIMIX_IN const kimix_yyjson_val *v);

/* The literal text of a TYPE_RAW value (wraps yyjson_get_raw()).
 * NUL-terminated, KIMIX_BORROWED from the document (it usually points into the
 * input buffer of kimix_yyjson_read).  NULL for NULL or a non-raw value. */
KIMIX_FFI const char *kimix_yyjson_get_raw(KIMIX_BORROWED const kimix_yyjson_val *v);

/* Is the value a string equal to the NUL-terminated `str`
 * (wraps yyjson_equals_str())?  false for a NULL argument. */
KIMIX_FFI bool kimix_yyjson_equals_str(KIMIX_IN const kimix_yyjson_val *v, KIMIX_IN const char *str);

/* Is the value a string equal to the `str` / `len` bytes
 * (wraps yyjson_equals_strn())?  false for a NULL value or a NULL `str`. */
KIMIX_FFI bool kimix_yyjson_equals_strn(KIMIX_IN const kimix_yyjson_val *v,
                                        KIMIX_IN const char *str,
                                        size_t len);

/* ===========================================================================
 * 4. Immutable value: array access
 * ======================================================================== */

/* The number of elements (wraps yyjson_arr_size()).  0 for NULL or a non-array. */
KIMIX_FFI size_t kimix_yyjson_arr_size(KIMIX_IN const kimix_yyjson_val *arr);

/* The element at `idx` (wraps yyjson_arr_get()).  KIMIX_BORROWED from the
 * document; NULL when `idx` is out of range, `arr` is NULL, or `arr` is not an
 * array.  O(1) for a flat array, O(idx) otherwise -- see the note in the file
 * header. */
KIMIX_FFI kimix_yyjson_val *kimix_yyjson_arr_get(KIMIX_BORROWED const kimix_yyjson_val *arr, size_t idx);

/* The first element (wraps yyjson_arr_get_first()).  NULL for an empty array, a
 * NULL `arr` or a non-array. */
KIMIX_FFI kimix_yyjson_val *kimix_yyjson_arr_get_first(KIMIX_BORROWED const kimix_yyjson_val *arr);

/* The last element (wraps yyjson_arr_get_last()).  NULL for an empty array, a
 * NULL `arr` or a non-array. */
KIMIX_FFI kimix_yyjson_val *kimix_yyjson_arr_get_last(KIMIX_BORROWED const kimix_yyjson_val *arr);

/* ===========================================================================
 * 5. Immutable value: object access
 * ======================================================================== */

/* The number of key-value pairs (wraps yyjson_obj_size()).  0 for NULL or a
 * non-object. */
KIMIX_FFI size_t kimix_yyjson_obj_size(KIMIX_IN const kimix_yyjson_val *obj);

/* The value stored under the NUL-terminated `key` (wraps yyjson_obj_get()).
 * KIMIX_BORROWED from the document; NULL when the key is missing, `obj` is NULL
 * or `obj` is not an object.  A hash lookup. */
KIMIX_FFI kimix_yyjson_val *kimix_yyjson_obj_get(KIMIX_BORROWED const kimix_yyjson_val *obj,
                                                 KIMIX_IN const char *key);

/* The value stored under the exact key `key` / `key_len` (no NUL terminator
 * required; wraps yyjson_obj_getn()).  Same rules as kimix_yyjson_obj_get(). */
KIMIX_FFI kimix_yyjson_val *kimix_yyjson_obj_getn(KIMIX_BORROWED const kimix_yyjson_val *obj,
                                                  KIMIX_IN const char *key,
                                                  size_t key_len);

/* ===========================================================================
 * 6. Writing an immutable document / value
 * ======================================================================== */

/* Serialize a whole document into a freshly allocated, NUL-terminated UTF-8
 * string.  Wraps yyjson_write_opts() with the library's mimalloc allocator --
 * NOT the yyjson_write() convenience form, which allocates with libc malloc and
 * would then be freed in the wrong heap.
 *   - `flags`: 0 or a combination of the KIMIX_YYJSON_WRITE_* macros,
 *   - `len` KIMIX_NULLABLE: set to 0, then written with the byte length
 *     excluding the NUL terminator on success,
 *   - `err` KIMIX_NULLABLE: always written (code == KIMIX_YYJSON_WRITE_SUCCESS
 *     on success).
 * The result is KIMIX_TRANSFER: the caller MUST release it with
 * kimix_yyjson_str_free().  NULL when `doc` is NULL, has no root, or the write
 * failed (an unserializable value type, a NaN/Infinity without the matching
 * flag, an allocation failure). */
KIMIX_FFI char *kimix_yyjson_write(KIMIX_IN const kimix_yyjson_doc *doc,
                                   uint32_t flags,
                                   KIMIX_OUT KIMIX_NULLABLE size_t *len,
                                   KIMIX_OUT KIMIX_NULLABLE kimix_yyjson_write_err *err);

/* Serialize ONE value of an immutable document (wraps yyjson_val_write_opts()).
 * Same allocator rule and same KIMIX_TRANSFER ownership as
 * kimix_yyjson_write(): free the result with kimix_yyjson_str_free().
 * NULL for a NULL `v`. */
KIMIX_FFI char *kimix_yyjson_val_write(KIMIX_IN const kimix_yyjson_val *v,
                                       uint32_t flags,
                                       KIMIX_OUT KIMIX_NULLABLE size_t *len,
                                       KIMIX_OUT KIMIX_NULLABLE kimix_yyjson_write_err *err);

/* Serialize a whole document into the caller's own buffer: no allocation at all,
 * nothing to free.  Wraps yyjson_write_buf().
 *   - `buf` / `buf_len`: the output window; a successful call NUL-terminates the
 *     result,
 *   - the buffer must be LARGER than the final JSON size: yyjson reserves
 *     temporary space while writing (see the fork's API.md),
 *   - `len` and `err` behave as in kimix_yyjson_write().
 * Returns the number of bytes written (excluding the NUL terminator), or 0 when
 * `buf` or `doc` is NULL, the buffer is too small, or the document cannot be
 * serialized.  On failure nothing in `buf` is meaningful. */
KIMIX_FFI size_t kimix_yyjson_write_buf(KIMIX_OUT char *buf,
                                        size_t buf_len,
                                        KIMIX_IN const kimix_yyjson_doc *doc,
                                        uint32_t flags,
                                        KIMIX_OUT KIMIX_NULLABLE kimix_yyjson_write_err *err);

/* Release a string returned by kimix_yyjson_write(), kimix_yyjson_val_write(),
 * kimix_yyjson_mut_write() or kimix_yyjson_mut_val_write().  Such a buffer is
 * mimalloc memory of THIS library's heap, so it must not be released with the
 * caller's own free() nor through a kimix_mem_free() resolved in another module.
 * (The body is mi_free().)  NULL is ignored. */
KIMIX_FFI void kimix_yyjson_str_free(KIMIX_TRANSFER KIMIX_NULLABLE char *s);

/* ===========================================================================
 * 7. Mutable document: lifecycle
 * ======================================================================== */

/* Create an empty mutable document (wraps yyjson_mut_doc_new() with the
 * library's mimalloc allocator; there is no root yet).  The result is
 * KIMIX_TRANSFER -- release with kimix_yyjson_mut_doc_free() -- or NULL when the
 * allocation failed.  Every creator and mutator of sections 8-11 needs this
 * pointer: the values it makes belong to the document, never to the caller. */
KIMIX_FFI kimix_yyjson_mut_doc *kimix_yyjson_mut_doc_new(void);

/* Release a mutable document and all of its values
 * (wraps yyjson_mut_doc_free()).  Every kimix_yyjson_mut_val from it becomes
 * dangling.  NULL is ignored. */
KIMIX_FFI void kimix_yyjson_mut_doc_free(KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_doc *doc);

/* The root value (wraps yyjson_mut_doc_get_root()).  KIMIX_BORROWED from `doc`;
 * NULL when `doc` is NULL or has no root yet.  `doc` is only READ here (the
 * fork's getter takes a non-const pointer because the same type is used for the
 * mutating setters); the annotation says so instead of implying a write. */
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_doc_root(KIMIX_IN KIMIX_NULLABLE kimix_yyjson_mut_doc *doc);

/* Set -- or with a NULL `root` clear -- the root (wraps
 * yyjson_mut_doc_set_root()).  `root` must be a value made by the SAME document
 * and not yet attached anywhere else; yyjson cannot check that and the call
 * cannot fail, so a foreign value corrupts the document exactly as it would in
 * yyjson.  A NULL `doc` does nothing. */
KIMIX_FFI void kimix_yyjson_mut_doc_set_root(KIMIX_IN_OUT KIMIX_NULLABLE kimix_yyjson_mut_doc *doc,
                                            KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *root);

/* Deep-copy a mutable document into a new immutable one
 * (wraps yyjson_mut_doc_imut_copy() with the mimalloc allocator): the usual last
 * step before handing a built document to the section 2-6 accessors.  Recursive,
 * so a very deep tree can exhaust the stack, exactly as in yyjson.  KIMIX_TRANSFER
 * -- release with kimix_yyjson_doc_free(); NULL for a NULL input, a document with
 * no root, or an allocation failure. */
KIMIX_FFI kimix_yyjson_doc *kimix_yyjson_mut_doc_imut_copy(KIMIX_IN const kimix_yyjson_mut_doc *doc);

/* Deep-copy a mutable document into a new mutable one
 * (wraps yyjson_mut_doc_mut_copy() with the mimalloc allocator).  A source with
 * no root yields a NEW EMPTY mutable document, not NULL (that is the fork's
 * behaviour).  KIMIX_TRANSFER -- release with kimix_yyjson_mut_doc_free(); NULL
 * for a NULL input, a value too deep for the recursive copy, or an allocation
 * failure. */
KIMIX_FFI kimix_yyjson_mut_doc *kimix_yyjson_mut_doc_mut_copy(KIMIX_IN const kimix_yyjson_mut_doc *doc);

/* ===========================================================================
 * 8. Mutable value creators
 * ======================================================================== */

/* Each creator wraps its same-named yyjson_mut_*() function, takes one node out
 * of `doc`'s pools and returns it KIMIX_BORROWED from `doc` -- never free a value
 * by itself, it disappears with kimix_yyjson_mut_doc_free().  Attach it with
 * sections 10-11 or make it the root with kimix_yyjson_mut_doc_set_root().
 * A NULL `doc`, or an allocation that could not be satisfied, yields NULL.  The
 * document passed in MUST be the one the value ends up in. */

KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_null(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc);
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_true(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc);
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_false(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc);
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_bool(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, bool val);
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_uint(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, uint64_t num);
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_sint(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, int64_t num);
/* The signed integer creator taking a wide argument (wraps yyjson_mut_int(), an
 * alias of yyjson_mut_sint()); reading it back with
 * kimix_yyjson_mut_get_int() truncates to 32 bits. */
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_int(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, int64_t num);
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_real(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, double num);

/* A string value that REFERENCES `str`: yyjson_mut_str() stores the pointer and
 * its strlen and copies NOTHING.  The caller must keep `str` alive and unmodified
 * for the whole remaining life of `doc`, and `str` must be NUL-terminated.  NULL
 * `str` yields NULL.  Use kimix_yyjson_mut_strcpy() when the text is temporary. */
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_str(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN const char *str);

/* The no-copy form with an explicit byte length (wraps yyjson_mut_strn()): a NUL
 * terminator is not required, but the bytes must stay alive and unmodified. */
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_strn(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                                     KIMIX_IN const char *str,
                                                     size_t len);

/* The copying forms (wrap yyjson_mut_strcpy() / yyjson_mut_strncpy()): the bytes
 * are duplicated into the document's string pool, so the caller's buffer may be
 * reused or released immediately after the call.  `_cpy` needs a NUL terminator,
 * `_ncpy` does not. */
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_strcpy(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN const char *str);
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_strncpy(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                                        KIMIX_IN const char *str,
                                                        size_t len);

/* An empty array / empty object, ready for section 10 / section 11
 * (wraps yyjson_mut_arr() / yyjson_mut_obj()). */
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_arr(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc);
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_obj(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc);

/* A raw value: `str` is emitted verbatim as JSON text.  Nothing is copied -- keep
 * it alive for the life of `doc` -- and it must already be valid JSON, because
 * the writer does not check it (wraps yyjson_mut_raw()). */
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_raw(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN const char *str);

/* ===========================================================================
 * 9. Mutable value: type checks, content getters and lookup
 * ======================================================================== */

/* The sections 2-5 surface mirrored for MUTABLE values (every one wraps the
 * yyjson_mut_* accessor of the same name).  They accept a mutable value and ONLY
 * a mutable value: handing a `kimix_yyjson_mut_val` to a section 2-5 function,
 * or an immutable `kimix_yyjson_val` to one of these, makes yyjson read the wrong
 * fields of a differently shaped node (24 vs 16 bytes).  That is a caller bug the
 * FFI cannot detect.  Convert with kimix_yyjson_mut_doc_imut_copy() /
 * kimix_yyjson_doc_mut_copy() instead. */

KIMIX_FFI bool kimix_yyjson_mut_is_raw(KIMIX_IN const kimix_yyjson_mut_val *v);
KIMIX_FFI bool kimix_yyjson_mut_is_null(KIMIX_IN const kimix_yyjson_mut_val *v);
KIMIX_FFI bool kimix_yyjson_mut_is_true(KIMIX_IN const kimix_yyjson_mut_val *v);
KIMIX_FFI bool kimix_yyjson_mut_is_false(KIMIX_IN const kimix_yyjson_mut_val *v);
KIMIX_FFI bool kimix_yyjson_mut_is_bool(KIMIX_IN const kimix_yyjson_mut_val *v);
KIMIX_FFI bool kimix_yyjson_mut_is_uint(KIMIX_IN const kimix_yyjson_mut_val *v);
KIMIX_FFI bool kimix_yyjson_mut_is_sint(KIMIX_IN const kimix_yyjson_mut_val *v);
KIMIX_FFI bool kimix_yyjson_mut_is_int(KIMIX_IN const kimix_yyjson_mut_val *v); /* uint OR sint */
KIMIX_FFI bool kimix_yyjson_mut_is_real(KIMIX_IN const kimix_yyjson_mut_val *v);
KIMIX_FFI bool kimix_yyjson_mut_is_num(KIMIX_IN const kimix_yyjson_mut_val *v);
KIMIX_FFI bool kimix_yyjson_mut_is_str(KIMIX_IN const kimix_yyjson_mut_val *v);
KIMIX_FFI bool kimix_yyjson_mut_is_arr(KIMIX_IN const kimix_yyjson_mut_val *v);
KIMIX_FFI bool kimix_yyjson_mut_is_obj(KIMIX_IN const kimix_yyjson_mut_val *v);
KIMIX_FFI bool kimix_yyjson_mut_is_ctn(KIMIX_IN const kimix_yyjson_mut_val *v);

/* KIMIX_YYJSON_TYPE_*, KIMIX_YYJSON_TYPE_NONE (0) for NULL (wraps
 * yyjson_mut_get_type()). */
KIMIX_FFI uint8_t kimix_yyjson_mut_get_type(KIMIX_IN const kimix_yyjson_mut_val *v);
/* A static description string, never free it (wraps yyjson_mut_get_type_desc()). */
KIMIX_FFI const char *kimix_yyjson_mut_get_type_desc(KIMIX_IN const kimix_yyjson_mut_val *v);

KIMIX_FFI bool kimix_yyjson_mut_get_bool(KIMIX_IN const kimix_yyjson_mut_val *v);
KIMIX_FFI uint64_t kimix_yyjson_mut_get_uint(KIMIX_IN const kimix_yyjson_mut_val *v);
KIMIX_FFI int64_t kimix_yyjson_mut_get_sint(KIMIX_IN const kimix_yyjson_mut_val *v);
KIMIX_FFI int32_t kimix_yyjson_mut_get_int(KIMIX_IN const kimix_yyjson_mut_val *v);
KIMIX_FFI double kimix_yyjson_mut_get_real(KIMIX_IN const kimix_yyjson_mut_val *v);
KIMIX_FFI double kimix_yyjson_mut_get_num(KIMIX_IN const kimix_yyjson_mut_val *v);
/* KIMIX_BORROWED from the document.  With the no-copy creators (mut_str /
 * mut_strn / mut_raw) those bytes are the CALLER's, so the pointer is only as
 * valid as the buffer handed to the creator.  NULL for NULL or a non-string. */
KIMIX_FFI const char *kimix_yyjson_mut_get_str(KIMIX_BORROWED const kimix_yyjson_mut_val *v);
KIMIX_FFI size_t kimix_yyjson_mut_get_len(KIMIX_IN const kimix_yyjson_mut_val *v);
KIMIX_FFI const char *kimix_yyjson_mut_get_raw(KIMIX_BORROWED const kimix_yyjson_mut_val *v);
KIMIX_FFI bool kimix_yyjson_mut_equals_str(KIMIX_IN const kimix_yyjson_mut_val *v, KIMIX_IN const char *str);
KIMIX_FFI bool kimix_yyjson_mut_equals_strn(KIMIX_IN const kimix_yyjson_mut_val *v,
                                            KIMIX_IN const char *str,
                                            size_t len);

/* Array lookup on a mutable value (wraps yyjson_mut_arr_size / _get /
 * _get_first / _get_last).  A mutable array is a linked chain, so every index
 * access walks it: O(idx) with no flat fast path (unlike the immutable
 * accessor). */
KIMIX_FFI size_t kimix_yyjson_mut_arr_size(KIMIX_IN const kimix_yyjson_mut_val *arr);
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_arr_get(KIMIX_BORROWED const kimix_yyjson_mut_val *arr, size_t idx);
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_arr_get_first(KIMIX_BORROWED const kimix_yyjson_mut_val *arr);
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_arr_get_last(KIMIX_BORROWED const kimix_yyjson_mut_val *arr);

/* Object lookup on a mutable value (wraps yyjson_mut_obj_size / _get / _getn).
 * A mutable object is an unordered bucket list: a lookup is a linear scan,
 * O(obj_size), and a duplicate key (possible with
 * kimix_yyjson_mut_obj_add()) returns the first match. */
KIMIX_FFI size_t kimix_yyjson_mut_obj_size(KIMIX_IN const kimix_yyjson_mut_val *obj);
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_obj_get(KIMIX_BORROWED const kimix_yyjson_mut_val *obj,
                                                         KIMIX_IN const char *key);
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_obj_getn(KIMIX_BORROWED const kimix_yyjson_mut_val *obj,
                                                          KIMIX_IN const char *key,
                                                          size_t key_len);

/* ===========================================================================
 * 10. Mutable array mutation
 * ======================================================================== */

/* The plain mutators take the element `val` as KIMIX_TRANSFER: on success the
 * array owns it and it must not be freed or attached a second time; on failure
 * (a NULL argument, `arr` not an array, an out-of-range index) nothing is
 * inserted and the caller still owns it.  The `add_*` sugar takes `doc` because
 * it creates the element for you; it must be the document that owns `arr`.
 * Every one of these wraps its same-named yyjson_mut_arr_*() function. */

KIMIX_FFI bool kimix_yyjson_mut_arr_append(KIMIX_IN_OUT kimix_yyjson_mut_val *arr,
                                          KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *val);
KIMIX_FFI bool kimix_yyjson_mut_arr_prepend(KIMIX_IN_OUT kimix_yyjson_mut_val *arr,
                                           KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *val);
/* `val` at position `idx`; a valid idx is 0..size (size == append). */
KIMIX_FFI bool kimix_yyjson_mut_arr_insert(KIMIX_IN_OUT kimix_yyjson_mut_val *arr,
                                          KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *val,
                                          size_t idx);
/* Take one element out.  The fork's yyjson_mut_arr_remove*() return the node
 * that left the array, not a bool, so these do too: the result is
 * KIMIX_BORROWED from the document, it may be re-inserted elsewhere or ignored,
 * and it is never freed alone.  NULL when nothing was removed. */
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_arr_remove(KIMIX_IN_OUT kimix_yyjson_mut_val *arr, size_t idx);
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_arr_remove_first(KIMIX_IN_OUT kimix_yyjson_mut_val *arr);
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_arr_remove_last(KIMIX_IN_OUT kimix_yyjson_mut_val *arr);
/* Put `val` at `idx` and return the element it displaced (NULL for a NULL
 * argument, a non-array or an out-of-range index -- the array is then unchanged
 * and `val` is still owned by the caller). */
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_arr_replace(KIMIX_IN_OUT kimix_yyjson_mut_val *arr,
                                                            size_t idx,
                                                            KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *val);
/* Drop all elements (the nodes stay in the document's pool and get reused). */
KIMIX_FFI bool kimix_yyjson_mut_arr_clear(KIMIX_IN_OUT kimix_yyjson_mut_val *arr);

/* Append a freshly created element.  false means nothing was appended (bad
 * arguments or an allocation failure). */
KIMIX_FFI bool kimix_yyjson_mut_arr_add_null(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                            KIMIX_IN_OUT kimix_yyjson_mut_val *arr);
KIMIX_FFI bool kimix_yyjson_mut_arr_add_bool(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                            KIMIX_IN_OUT kimix_yyjson_mut_val *arr,
                                            bool val);
KIMIX_FFI bool kimix_yyjson_mut_arr_add_uint(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                            KIMIX_IN_OUT kimix_yyjson_mut_val *arr,
                                            uint64_t num);
KIMIX_FFI bool kimix_yyjson_mut_arr_add_sint(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                            KIMIX_IN_OUT kimix_yyjson_mut_val *arr,
                                            int64_t num);
KIMIX_FFI bool kimix_yyjson_mut_arr_add_real(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                            KIMIX_IN_OUT kimix_yyjson_mut_val *arr,
                                            double num);
/* Append a string WITHOUT copying it: the bytes must stay alive and unmodified
 * for the life of `doc` (wraps yyjson_mut_arr_add_str()). */
KIMIX_FFI bool kimix_yyjson_mut_arr_add_str(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                           KIMIX_IN_OUT kimix_yyjson_mut_val *arr,
                                           KIMIX_IN const char *str);
KIMIX_FFI bool kimix_yyjson_mut_arr_add_strn(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                            KIMIX_IN_OUT kimix_yyjson_mut_val *arr,
                                            KIMIX_IN const char *str,
                                            size_t len);
/* Append a string and COPY it into the document's string pool
 * (wraps yyjson_mut_arr_add_strcpy()). */
KIMIX_FFI bool kimix_yyjson_mut_arr_add_strcpy(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                              KIMIX_IN_OUT kimix_yyjson_mut_val *arr,
                                              KIMIX_IN const char *str);
/* Append an existing value; the array takes ownership (wraps
 * yyjson_mut_arr_add_val(), an alias of yyjson_mut_arr_append()). */
KIMIX_FFI bool kimix_yyjson_mut_arr_add_val(KIMIX_IN_OUT kimix_yyjson_mut_val *arr,
                                           KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *val);
/* Append and return a NEW empty array / object, so a nested container can be
 * filled without a create-then-append pair (wraps yyjson_mut_arr_add_arr() /
 * yyjson_mut_arr_add_obj()).  KIMIX_BORROWED from `doc`; NULL on failure. */
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_arr_add_arr(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                                            KIMIX_IN_OUT kimix_yyjson_mut_val *arr);
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_arr_add_obj(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                                            KIMIX_IN_OUT kimix_yyjson_mut_val *arr);

/* ===========================================================================
 * 11. Mutable object mutation
 * ======================================================================== */

/* In the two-value form `key` must be a string value made by THIS document's
 * creators (section 8); `val` becomes owned by the document on success. */

/* Append a key-value pair WITHOUT de-duplicating: the object may then hold two
 * pairs with the same key and a lookup returns the first one (wraps
 * yyjson_mut_obj_add()).  false when `obj` is not an object, `key` is not a
 * string value, `val` is NULL, or the document could not allocate. */
KIMIX_FFI bool kimix_yyjson_mut_obj_add(KIMIX_IN_OUT kimix_yyjson_mut_val *obj,
                                        KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *key,
                                        KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *val);

/* Set a key-value pair, removing every existing pair with the same key first
 * (wraps yyjson_mut_obj_put()).  A NULL `val` deletes the key, like
 * kimix_yyjson_mut_obj_remove(). */
KIMIX_FFI bool kimix_yyjson_mut_obj_put(KIMIX_IN_OUT kimix_yyjson_mut_val *obj,
                                       KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *key,
                                       KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *val);

/* Remove every pair with the NUL-terminated `key` and return the FIRST value
 * that was removed (KIMIX_BORROWED from the document; NULL when the key was not
 * there).  A linear scan.  NOTE: this wraps yyjson_mut_obj_remove_key(), because
 * the fork's yyjson_mut_obj_remove() takes a key VALUE instead of a C string. */
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_obj_remove(KIMIX_IN_OUT kimix_yyjson_mut_val *obj,
                                                           KIMIX_IN const char *key);

/* The same with an explicit key length, no NUL terminator required (wraps
 * yyjson_mut_obj_remove_keyn()). */
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_obj_remove_keyn(KIMIX_IN_OUT kimix_yyjson_mut_val *obj,
                                                                KIMIX_IN const char *key,
                                                                size_t key_len);

/* Drop all pairs (wraps yyjson_mut_obj_clear()). */
KIMIX_FFI bool kimix_yyjson_mut_obj_clear(KIMIX_IN_OUT kimix_yyjson_mut_val *obj);

/* Rename an existing key in place (wraps yyjson_mut_obj_rename_key()): the pair
 * keeps its position, only the key's text is replaced.  `new_key` is COPIED into
 * the document's string pool.  false when `obj` is not an object, `key` is
 * missing, or the copy failed. */
KIMIX_FFI bool kimix_yyjson_mut_obj_rename_key(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                              KIMIX_IN_OUT kimix_yyjson_mut_val *obj,
                                              KIMIX_IN const char *key,
                                              KIMIX_IN const char *new_key);

/* Add a freshly created pair.  `doc` must own `obj`; the key string is created
 * by yyjson from `key` and is NOT copied for the `_str` spelling, so `key` must
 * stay alive for the life of `doc` (the `_cpy` variant of the key is available
 * by combining kimix_yyjson_mut_strcpy + kimix_yyjson_mut_obj_put).  Likewise
 * the `_str` / `_strn` VALUE forms do not copy their bytes. */
KIMIX_FFI bool kimix_yyjson_mut_obj_add_null(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                            KIMIX_IN_OUT kimix_yyjson_mut_val *obj,
                                            KIMIX_IN const char *key);
KIMIX_FFI bool kimix_yyjson_mut_obj_add_bool(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                            KIMIX_IN_OUT kimix_yyjson_mut_val *obj,
                                            KIMIX_IN const char *key,
                                            bool val);
KIMIX_FFI bool kimix_yyjson_mut_obj_add_uint(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                            KIMIX_IN_OUT kimix_yyjson_mut_val *obj,
                                            KIMIX_IN const char *key,
                                            uint64_t val);
KIMIX_FFI bool kimix_yyjson_mut_obj_add_sint(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                            KIMIX_IN_OUT kimix_yyjson_mut_val *obj,
                                            KIMIX_IN const char *key,
                                            int64_t val);
KIMIX_FFI bool kimix_yyjson_mut_obj_add_real(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                            KIMIX_IN_OUT kimix_yyjson_mut_val *obj,
                                            KIMIX_IN const char *key,
                                            double val);
KIMIX_FFI bool kimix_yyjson_mut_obj_add_str(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                           KIMIX_IN_OUT kimix_yyjson_mut_val *obj,
                                           KIMIX_IN const char *key,
                                           KIMIX_IN const char *val);
KIMIX_FFI bool kimix_yyjson_mut_obj_add_strn(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                            KIMIX_IN_OUT kimix_yyjson_mut_val *obj,
                                            KIMIX_IN const char *key,
                                            KIMIX_IN const char *val,
                                            size_t len);
/* The copying VALUE form (wraps yyjson_mut_obj_add_strcpy()). */
KIMIX_FFI bool kimix_yyjson_mut_obj_add_strcpy(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                              KIMIX_IN_OUT kimix_yyjson_mut_val *obj,
                                              KIMIX_IN const char *key,
                                              KIMIX_IN const char *val);
/* Attach an existing value under `key`; the key is created by yyjson (not
 * copied) and `val` becomes owned by `doc`. */
KIMIX_FFI bool kimix_yyjson_mut_obj_add_val(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                           KIMIX_IN_OUT kimix_yyjson_mut_val *obj,
                                           KIMIX_IN const char *key,
                                           KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *val);
/* Add and return a NEW empty array / object under `key` (wraps
 * yyjson_mut_obj_add_arr() / yyjson_mut_obj_add_obj()).  KIMIX_BORROWED from
 * `doc`; NULL on failure. */
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_obj_add_arr(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                                            KIMIX_IN_OUT kimix_yyjson_mut_val *obj,
                                                            KIMIX_IN const char *key);
KIMIX_FFI kimix_yyjson_mut_val *kimix_yyjson_mut_obj_add_obj(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc,
                                                            KIMIX_IN_OUT kimix_yyjson_mut_val *obj,
                                                            KIMIX_IN const char *key);

/* ===========================================================================
 * 12. Writing a mutable document / value
 * ======================================================================== */

/* Serialize a mutable document (wraps yyjson_mut_write_opts() with the library's
 * mimalloc allocator -- never the libc-allocating yyjson_mut_write() convenience
 * form).  Ownership and failure behaviour are exactly as in
 * kimix_yyjson_write(): the returned string is KIMIX_TRANSFER and must be
 * released with kimix_yyjson_str_free(). */
KIMIX_FFI char *kimix_yyjson_mut_write(KIMIX_IN const kimix_yyjson_mut_doc *doc,
                                       uint32_t flags,
                                       KIMIX_OUT KIMIX_NULLABLE size_t *len,
                                       KIMIX_OUT KIMIX_NULLABLE kimix_yyjson_write_err *err);

/* The allocation-free variant (wraps yyjson_mut_write_buf()): returns the number
 * of bytes written into `buf`, or 0 on any failure; `buf` must be larger than
 * the final JSON size, see kimix_yyjson_write_buf(). */
KIMIX_FFI size_t kimix_yyjson_mut_write_buf(KIMIX_OUT char *buf,
                                            size_t buf_len,
                                            KIMIX_IN const kimix_yyjson_mut_doc *doc,
                                            uint32_t flags,
                                            KIMIX_OUT KIMIX_NULLABLE kimix_yyjson_write_err *err);

/* Serialize one value of a mutable document (wraps
 * yyjson_mut_val_write_opts()).  Same KIMIX_TRANSFER rule: free the result with
 * kimix_yyjson_str_free().  NULL for a NULL `v`. */
KIMIX_FFI char *kimix_yyjson_mut_val_write(KIMIX_IN const kimix_yyjson_mut_val *v,
                                          uint32_t flags,
                                          KIMIX_OUT KIMIX_NULLABLE size_t *len,
                                          KIMIX_OUT KIMIX_NULLABLE kimix_yyjson_write_err *err);

KIMIX_FFI_END

#endif /* KIMIX_API_FFI_YYJSON_H */
