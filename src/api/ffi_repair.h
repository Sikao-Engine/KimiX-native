/*
 * ffi_repair.h -- kimix::repair() (src/core/json_repair.h) exported to C.
 *
 * WHAT THIS MODULE IS FOR
 * -----------------------
 * Large language models do not emit syntactically valid JSON when they are asked
 * for a tool call: they leave a markdown fence around the object, quote keys
 * twice or not at all, write `True`/`None`/`NaN`/`0x1A`, drop commas and colons,
 * close a container that was never closed, cut the value off at the token limit,
 * put a raw newline inside a string, or wrap the whole thing in prose.  The
 * agent dispatch path of this project cannot feed such a string to a JSON parser
 * and give up, so `kimix::repair()` re-reads it with a tolerant, non-recursive
 * parser and re-serialises canonical JSON that the strict parser accepts.  This
 * header is that kernel behind a plain C ABI, so any host language can sanitise
 * the model output before it reaches its own deserializer.
 *
 * THE RESULT CONVENTION (the one thing to remember)
 * --------------------------------------------------
 * `kimix::repair()` returns a `kimix::vector<char>` and uses EMPTYNESS as its
 * status:
 *   - EMPTY result  -> the input was already strictly valid JSON, so there is
 *                      nothing to hand back (the caller should parse the bytes it
 *                      already has).  Also, for completeness: an input the
 *                      tolerant parser could not turn into valid JSON yields an
 *                      empty result too -- the library never emits invalid JSON.
 *   - NON-EMPTY     -> the repaired, strictly valid JSON.  The LAST element is
 *                      the NUL terminator '\0', so the buffer is a valid C
 *                      string; the JSON text itself is the first size() - 1
 *                      bytes (see kimix_repaired_view / kimix_repaired_cstr).
 * Every function below that delivers a result does it into a `kimix_vec`
 * placeholder (api/ffi_vec.h) -- the very same `kimix::vector<std::byte>` type,
 * so the whole api/ffi_vec.h contract (init/destroy pair, guard word, one
 * library heap, moves are the only cheap transfer) applies unchanged.
 *
 * OWNERSHIP AND COST
 * ------------------
 * * A repaired result is allocated inside this library (mimalloc) and is owned by
 *   the placeholder: release it with kimix_vec_destroy() (inline storage) or
 *   kimix_vec_free() (the heap placeholders of kimix_repair_new()).
 * * `kimix_repair()` MOVES the buffer into the caller's placeholder: no byte of
 *   the repaired text is copied across the boundary.
 * * `kimix_json_is_valid()` allocates nothing that the caller sees: it parses and
 *   releases the document before returning (see its own comment for the cost).
 * * The input is length-based (a `string_view` on the C++ side), so it does NOT
 *   have to be NUL-terminated; embedded NUL bytes are simply part of the text.
 * * Nothing here throws: the project is built without C++ exceptions, and every
 *   failure is a `kimix_status` (or NULL / false for the value-returning calls).
 *
 * Valid for a C99/C11 compiler and for C++17 or newer; see api/ffi_common.h for
 * the rules shared by the whole surface.  The index is docs/ffi.md.
 */
#pragma once
#ifndef KIMIX_API_FFI_REPAIR_H
#define KIMIX_API_FFI_REPAIR_H

#include <api/ffi_common.h>
#include <api/ffi_vec.h>

/* A borrowed {pointer, length} pair of UTF-8 text: the C shape of
 * kimix::string_view.  `data` is NOT NUL-terminated and `length` is in BYTES, so
 * a binding must copy it out (or use kimix_repaired_cstr()) before handing it to
 * a string API that stops at a terminator.  Both fields are 0/NULL for the empty
 * view. */
typedef struct kimix_str_view {
    const char *data;
    size_t length;
} kimix_str_view;

KIMIX_FFI_BEGIN

/* ===========================================================================
 * 1. Validity probe
 * ======================================================================== */

/* Wraps the strict validity check that kimix::repair() performs as its very
 * first step (the `is_valid_json()` helper of src/core/json_repair.cpp: a
 * YYJSON_READ_NOFLAG read of exactly these bytes): "would kimix_repair() return
 * an empty result for this input because it needs no repair?".
 *
 * CHOICE OF PROBE AND ITS COST: the check is re-implemented here with
 * yyjson_read_opts() over the library's own mimalloc yyjson_alc (api/detail.h),
 * because core's helper lives in an anonymous namespace and is not exported.
 * It is a full parse, not a peek: the non-INSITU reader copies the input into a
 * padded buffer and allocates the document's value/string pools on the library
 * heap, then this call frees everything before returning.  Nothing is handed to
 * the caller, no kimix_vec is built, and no repaired text is produced -- one
 * parse and one allocation round-trip, both released internally.
 *
 * LENGTHS ARE IN BYTES and `json` need not be NUL-terminated.  An EMPTY input is
 * NOT valid: len == 0 (as well as NULL `json`) returns false, which is what the
 * probe would answer anyway (yyjson rejects an empty document, and
 * kimix::repair("") repairs it to the text "null").  A NULL `json` with len == 0
 * is therefore false, not an error -- this call reports nothing but its answer.
 *
 * Consistency with kimix_repair(): true => the result vector is EMPTY.  false
 * does NOT guarantee a non-empty result, because an input that cannot be turned
 * into valid JSON also yields an empty vector ("never emit invalid JSON").
 * Returns: true / false only; it cannot fail. */
KIMIX_FFI bool kimix_json_is_valid(KIMIX_IN const void *json, size_t len);

/* The NUL-terminated spelling of the same probe: the text length is
 * strlen(nul_terminated_json), so embedded NULs are not representable here and
 * the bytes must be valid UTF-8 for the parser to accept them at all.  NULL is
 * reported as false (an empty input is not valid, and it is not an error). */
KIMIX_FFI bool kimix_json_is_valid_str(KIMIX_IN const char *nul_terminated_json);

/* ===========================================================================
 * 2. Repair into a caller-owned placeholder
 * ======================================================================== */

/* Wraps `kimix::vector<char> kimix::repair(kimix::string_view json)` and MOVES
 * the returned buffer into the raw storage of `out` -- the same way
 * kimix_vec_init_from() constructs its vector, so `out` needs no prior init and
 * does not have to be destroyed and re-created between calls (use
 * kimix_repair_assign() for that instead).  No byte of the repaired JSON is
 * copied: the vector<char> the C++ function returned is reinterpreted as the
 * vector<std::byte> that lives in the placeholder and move-constructed there
 * (kimix::api::vec_construct_move_from_char, see api/detail.h).
 *
 * Result convention (the whole point of the call):
 *   - the input was malformed -> `out` holds the repaired, strictly valid JSON,
 *     with a trailing '\0' as its LAST byte (so kimix_vec_size() is
 *     text length + 1);
 *   - the input was already valid -> `out` holds an EMPTY vector and the call
 *     still returns KIMIX_OK ("nothing to repair" is a success, not an error).
 *
 * `json` is caller-owned input read during the call; `len` is in BYTES and the
 * text need not be NUL-terminated.  A NULL `json` with len == 0 is the empty
 * input, which core repairs to the text "null" -- so the result is NON-empty.
 *
 * Returns KIMIX_ERR_INVALID_ARG for a NULL `out` or a NULL `json` with len > 0,
 * KIMIX_ERR_INVALID_STATE when `out` already holds a live vector (constructing
 * into it would leak that buffer), KIMIX_OK otherwise.  Never
 * KIMIX_ERR_OUT_OF_MEMORY: the vector's allocator aborts on exhaustion instead
 * of throwing (see api/ffi_vec.h).  On any error `out` is left untouched. */
KIMIX_FFI kimix_status kimix_repair(KIMIX_OUT kimix_vec *out,
                                    KIMIX_IN const void *json,
                                    size_t len);

/* The NUL-terminated spelling of kimix_repair(): the input length is
 * strlen(nul_terminated_json).  A NULL string is KIMIX_ERR_INVALID_ARG (there is
 * no length to fall back on); to repair the empty input pass "" or
 * kimix_repair(out, NULL, 0).  Same result convention, same ownership, same
 * error codes as kimix_repair(). */
KIMIX_FFI kimix_status kimix_repair_str(KIMIX_OUT kimix_vec *out,
                                        KIMIX_IN const char *nul_terminated_json);

/* Same as kimix_repair(), but ASSIGNs into a placeholder that already holds a
 * live vector (`*out = std::move(repaired)`): the previous content is released on
 * the library heap and the repaired bytes take its place, so `out` stays
 * initialised and keeps being the object its owner destroys exactly once.  Use
 * this to repair into a long-lived buffer in a loop.  Empty result means the
 * input was already valid, exactly as above (the previous content is still
 * replaced -- by nothing).
 *
 * Returns KIMIX_ERR_INVALID_ARG for a NULL `out` or a NULL `json` with len > 0,
 * KIMIX_ERR_INVALID_STATE when `out` is NOT initialised (nothing to assign
 * into), KIMIX_OK otherwise; on error the vector keeps its old content. */
KIMIX_FFI kimix_status kimix_repair_assign(KIMIX_IN_OUT kimix_vec *out,
                                           KIMIX_IN const void *json,
                                           size_t len);

/* Heap convenience for bindings that would rather hold a pointer than embed a
 * placeholder: allocate one `kimix_vec` on the library heap (kimix_vec_new
 * semantics) and fill it with the repair of [json, json + len) -- the result
 * convention is the one above, so a caller that must distinguish "was already
 * valid" checks the vector's size (kimix_vec_size() == 0).  The caller owns the
 * placeholder and MUST release it with kimix_vec_free(), never with its own
 * free().  Returns NULL on an allocation failure or a NULL `json` with len > 0
 * (nothing is leaked: the placeholder is freed again before NULL is returned). */
KIMIX_FFI kimix_vec *kimix_repair_new(KIMIX_IN const void *json, size_t len);

/* ===========================================================================
 * 3. Reading a result
 * ======================================================================== */

/* Mirrors `kimix::repaired_view(const vector<char>&)` of src/core/json_repair.h:
 * the repaired text WITHOUT its trailing '\0', i.e. the first size() - 1 bytes of
 * `result`.  The placeholder is validated through the public accessor semantics
 * (the guard word of api/detail.h must say "live") before a single byte is read.
 *
 *   - live and EMPTY result   -> `*out_view` = { NULL, 0 } and KIMIX_OK: the
 *     input was already valid JSON, so there is no repaired text to show.
 *   - live and non-empty      -> `*out_view` = { data(), size() - 1 }, KIMIX_OK.
 *   - dead / NULL placeholder -> KIMIX_ERR_INVALID_STATE / KIMIX_ERR_INVALID_ARG
 *     and nothing usable in `*out_view` (it is neutralised to { NULL, 0 } first).
 *   - NULL `out_view`         -> KIMIX_ERR_INVALID_ARG.
 *
 * The view is BORROWED from `result` (see the KIMIX_BORROWED tag on the owning
 * argument): it dies with any call that resizes, assigns, moves-from or destroys
 * the vector, and with kimix_vec_destroy() / kimix_vec_free().  Call it on an
 * unmodified result: the helper drops the LAST byte unconditionally, exactly like
 * the C++ one, so a buffer the caller mutated through api/ffi_vec.h loses its
 * real last byte. */
KIMIX_FFI kimix_status kimix_repaired_view(KIMIX_BORROWED const kimix_vec *result,
                                           KIMIX_OUT kimix_str_view *out_view);

/* The same text as a NUL-terminated C string.  A non-empty result already ends
 * with '\0' inside its buffer (that is core's convention), so this simply
 * returns data() -- no copy, no allocation.  Returns NULL for an empty result
 * (the input was already valid: there is no text to point at), for a dead
 * placeholder and for a NULL `result`.
 *
 * BORROWED from `result`: invalid until any mutation of the vector
 * (resize / reserve / assign / append / swap / move-from) and by
 * kimix_vec_destroy() / kimix_vec_free(). */
KIMIX_FFI const char *kimix_repaired_cstr(KIMIX_BORROWED const kimix_vec *result);

KIMIX_FFI_END

#endif /* KIMIX_API_FFI_REPAIR_H */
