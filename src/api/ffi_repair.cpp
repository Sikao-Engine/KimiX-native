/*
 * ffi_repair.cpp -- implementation of the JSON-repair C FFI (api/ffi_repair.h).
 *
 * Two jobs, both thin and exception-free:
 *
 *   1. call kimix::repair() / its strict validity probe (src/core/json_repair.*)
 *      with a `string_view` built from the caller's {pointer, length} pair, and
 *   2. deliver the resulting `kimix::vector<char>` into the caller's `kimix_vec`
 *      placeholder, which holds a `kimix::vector<std::byte>`.
 *
 * THE BRIDGE (why no byte is copied)
 * ----------------------------------
 * `kimix::vector<char>` and `kimix::vector<std::byte>` are the SAME
 * std::vector template over the SAME stateless mimalloc allocator (core/stl)
 * with element types of identical size and alignment (both 1), so the two
 * objects have the same size, the same alignment and the same three-pointer
 * representation (begin / end / capacity): the buffer a vector<char> owns may be
 * reinterpreted in place as a vector<std::byte> buffer.  The static asserts below
 * freeze exactly that assumption against the real build; if one of them ever
 * fires, the bridge is broken and the fix belongs in api/detail.h, not here.
 * Reinterpreting is only half of it: the pointer handed to the move constructor /
 * move assignment is derived from the storage of an object created as a
 * vector<char>, so the byte-flavoured glvalue is re-derived with std::launder
 * (the same discipline every other entry point of this library uses for the
 * placeholder storage).  The move then transfers the BUFFER POINTER -- vector move
 * construction/assignment never touches the elements, so the repaired JSON is
 * neither copied nor re-serialised; it stays in the one allocation mimalloc made
 * for it inside this library, which is why a single kimix_vec_destroy() /
 * kimix_vec_free() is enough to release it and why the source vector<char>
 * (moved-from, empty, owning nothing) frees nothing when it dies at the end of
 * the call.
 *
 * THE VALIDITY PROBE
 * ------------------
 * core's `is_valid_json()` is a file-local (anonymous-namespace) helper of
 * json_repair.cpp and cannot be reached, so repair_probe_is_valid() below
 * re-implements it with the identical call shape -- a YYJSON_READ_NOFLAG parse of
 * exactly these bytes -- over the library's mimalloc yyjson_alc (api/detail.h) so
 * that no allocator other than the library's is ever used, and frees the document
 * before returning.  Everything the probe allocates is released inside the call;
 * nothing escapes to the caller.
 *
 * WHAT core DOES WITH DEGENERATE INPUT (matched, not invented, here)
 * -----------------------------------------------------------------
 *   json_repair.cpp:864  bool is_valid_json(string_view s) {
 *   json_repair.cpp:865      yyjson_doc *doc = yyjson_read(s.data(), s.size(), YYJSON_READ_NOFLAG);
 *   json_repair.cpp:866      if (doc == nullptr) return false;
 *   yyjson refuses a NULL buffer and a 0 length (yyjson.c, yyjson_read_opts:
 *   `if (!dat) ...`, `if (!len) ...`), so an EMPTY input is invalid, and
 *   json_repair.cpp:873  vector<char> repair(string_view json) {
 *   json_repair.cpp:874      if (is_valid_json(json)) return {};
 *   ...
 *   json_repair.cpp:878      if (!is_valid_json(res)) return {}; // never emit invalid JSON
 *   json_repair.cpp:882      vector<char> out;  ... out.push_back('\0');
 *   sends "" through the tolerant parser, whose output is the literal "null"
 *   (`if (out.empty()) return "null";`, json_repair.cpp:859, pinned by
 *   tests/unit/core/test_json_repair.cpp: `check_repaired("", "null")`).  Hence:
 *   kimix_json_is_valid(x, 0) == false and kimix_repair(out, NULL, 0) succeeds
 *   with the NON-empty 5-byte result "null\0".
 *
 * OOM note (same as api/ffi_vec.h): the vector's allocator reports exhaustion by
 * aborting, so no repair call can return KIMIX_ERR_OUT_OF_MEMORY.  Only the heap
 * placeholder of kimix_repair_new() can genuinely fail to allocate, and it
 * reports that by returning NULL.
 *
 * All file-local names are prefixed with `repair` / `k_repair`: the build merges
 * translation units (unity / cxx batch), so a global or an anonymous-namespace
 * name here must not be one that ffi_vec.cpp / ffi_mem.cpp / ffi_yyjson.cpp could
 * also invent.
 */
#include <api/detail.h>
#include <api/ffi_repair.h>
#include <core/json_repair.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

/* ---------------------------------------------------------------------------
 * The facts the bridge relies on, frozen against this build.
 *
 * sizeof / alignof equality of the two vector flavours is what makes the
 * reinterpret in kimix::api::vec_construct_move_from_char() sound: the object
 * laid down in the placeholder must be indistinguishable from the one the C++
 * function returned.  The element sizes are asserted too, because a
 * `string_view`-style byte index and a `size() - 1` text length are only
 * interchangeable while one element is one byte.
 * ------------------------------------------------------------------------- */
static_assert(sizeof(kimix::api::byte_vector) == sizeof(kimix::api::char_vector),
              "the vector<char> -> vector<std::byte> bridge needs equal object sizes");
static_assert(alignof(kimix::api::byte_vector) == alignof(kimix::api::char_vector),
              "the vector<char> -> vector<std::byte> bridge needs equal alignments");
static_assert(sizeof(std::byte) == 1 && sizeof(char) == 1,
              "the bridge, and the byte lengths of this ABI, assume 1-byte elements");
static_assert(std::is_same_v<decltype(kimix::repair(kimix::string_view{})), kimix::api::char_vector>,
              "kimix::repair() must return the very vector type the bridge moves");
/* The buffer itself is heap memory owned by the vector, so it never has to fit
 * in the placeholder -- but the object inside it must, and the guard word of
 * api/detail.h must not overlap it (checked in ffi_vec.cpp as well; restated for
 * this file's own use of the storage). */
static_assert(kimix::api::k_vec_guard_offset >= sizeof(kimix::api::byte_vector),
              "the guard slot would overlap the vector this file constructs");

namespace {

using kimix::api::byte_vector;
using kimix::api::char_vector;

/* The empty input as a `string_view` with a NON-null data pointer.  A NULL
 * `json` with `len == 0` is documented as "the empty input"; rather than build a
 * string_view from {nullptr, 0} (whose data() is then dereferenceable-free but
 * whose behaviour through an API that assumes a valid range is not what a C++
 * caller of repair("") would get), the call is made over this literal, which is
 * exactly what repair("") yields in C++: a non-null pointer and size 0.  Both
 * spellings take the same path through core, and yyjson refuses a 0 length
 * either way, so the observable result ("null\0") is identical. */
constexpr char k_repair_empty[] = "";

/* The caller's {pointer, length} pair as the C++ input of kimix::repair().
 * `json` is only read, never written, and the length is authoritative: the text
 * does not have to be NUL-terminated and may contain NUL bytes. */
inline kimix::string_view repair_input(const void *json, size_t len) noexcept {
    return len ? kimix::string_view{static_cast<const char *>(json), len}
               : kimix::string_view{k_repair_empty, 0};
}

/* The strict validity probe, mirroring the file-local `is_valid_json()` of
 * src/core/json_repair.cpp (line 864): same parser, same flags, same answer.
 * It uses the library's mimalloc yyjson_alc instead of yyjson's own libc default,
 * no foreign allocator is involved; the document it allocates is freed here,
 * before returning, and nothing is handed back.
 *
 * `json` must not be NULL and `len` must not be 0 for anything but a false
 * answer: yyjson_read_opts() rejects both explicitly, and core's own probe
 * therefore calls an empty input invalid.  The short-circuit keeps that decision
 * in one place, and keeps a NULL pointer out of the parser for good measure.
 *
 * yyjson_read_opts() takes a `char *` because its INSITU mode patches the input
 * in place; YYJSON_READ_NOFLAG is not INSITU, so the reader first copies the
 * bytes into its own padded buffer and the caller's memory is only ever read
 * (the same const_cast yyjson_read()'s own inline wrapper performs). */
inline bool repair_probe_is_valid(const void *json, size_t len) noexcept {
    if (!json || len == 0) {
        return false;
    }
    auto *dat = const_cast<char *>(static_cast<const char *>(json));
    yyjson_doc *doc = yyjson_read_opts(dat, len, YYJSON_READ_NOFLAG, &kimix::api::yyjson_mi_alc(), nullptr);
    if (!doc) {
        return false;
    }
    yyjson_doc_free(doc);
    return true;
}

/* The assignment counterpart of kimix::api::vec_construct_move_from_char(),
 * which only covers CONSTRUCTION (api/detail.h has no assign helper and must not
 * gain one here).  Same argument as the bridge above: `src` is a live
 * vector<char> whose representation is that of a vector<std::byte>, so the
 * byte-flavoured glvalue is laundered out of its address and the move assignment
 * steals the buffer pointer -- the old content of `dst` returns to the library
 * heap and `src` is left empty and owning nothing, so its own destructor frees
 * nothing.  `dst`'s live-object guard is untouched: the placeholder keeps holding
 * the same object. */
inline void repair_vec_assign_move_from_char(byte_vector *dst, char_vector &&src) noexcept {
    auto *bytes = std::launder(reinterpret_cast<byte_vector *>(std::addressof(src)));
    *dst = std::move(*bytes);
}

} // namespace

KIMIX_FFI_BEGIN

// ===========================================================================
// 1. Validity probe
// ===========================================================================

bool kimix_json_is_valid(const void *json, size_t len) {
    return repair_probe_is_valid(json, len);
}

bool kimix_json_is_valid_str(const char *nul_terminated_json) {
    if (!nul_terminated_json) {
        return false; // no length to take: the empty input, which is not valid
    }
    return repair_probe_is_valid(nul_terminated_json, std::strlen(nul_terminated_json));
}

// ===========================================================================
// 2. Repair into a caller-owned placeholder
// ===========================================================================

kimix_status kimix_repair(kimix_vec *out, const void *json, size_t len) {
    if (!out) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (!json && len) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (!kimix::api::vec_is_raw_storage(out)) {
        return KIMIX_ERR_INVALID_STATE; // would overwrite, and leak, a live vector
    }
    /* The one call into core; it returns by value, so this is a move, not a copy. */
    char_vector repaired = kimix::repair(repair_input(json, len));
    /* new (storage) vector<byte>(std::move(reinterpret_cast<vector<byte>&&>(repaired)))
     * -- buffer pointer transferred, no element copied. */
    kimix::api::vec_construct_move_from_char(out, std::move(repaired));
    return KIMIX_OK;
}

kimix_status kimix_repair_str(kimix_vec *out, const char *nul_terminated_json) {
    if (!nul_terminated_json) {
        return KIMIX_ERR_INVALID_ARG;
    }
    return kimix_repair(out, nul_terminated_json, std::strlen(nul_terminated_json));
}

kimix_status kimix_repair_assign(kimix_vec *out, const void *json, size_t len) {
    if (!json && len) {
        return KIMIX_ERR_INVALID_ARG;
    }
    byte_vector *self = kimix::api::vec_live(out);
    if (!self) {
        return out ? KIMIX_ERR_INVALID_STATE : KIMIX_ERR_INVALID_ARG;
    }
    char_vector repaired = kimix::repair(repair_input(json, len));
    repair_vec_assign_move_from_char(self, std::move(repaired));
    return KIMIX_OK;
}

kimix_vec *kimix_repair_new(const void *json, size_t len) {
    if (!json && len) {
        return nullptr;
    }
    /* The placeholder comes from the library heap, aligned for the over-aligned
     * object, and is default-constructed empty by kimix_vec_new() -- so the result
     * is ASSIGNED into it (the vector is live), which frees the empty vector's
     * nothing and steals the repaired buffer in one move. */
    kimix_vec *out = kimix_vec_new();
    if (!out) {
        return nullptr;
    }
    if (kimix_repair_assign(out, json, len) != KIMIX_OK) {
        kimix_vec_free(out); // unreachable (the arguments were just checked), but nothing leaks
        return nullptr;
    }
    return out;
}

// ===========================================================================
// 3. Reading a result
// ===========================================================================

kimix_status kimix_repaired_view(const kimix_vec *result, kimix_str_view *out_view) {
    if (!out_view) {
        return KIMIX_ERR_INVALID_ARG;
    }
    /* Neutralise first (the style of kimix_vec_at / kimix_vec_equals): a failure
     * return must never leave a stale or uninitialised-looking view. */
    out_view->data = nullptr;
    out_view->length = 0;
    const byte_vector *self = kimix::api::vec_live(result);
    if (!self) {
        return result ? KIMIX_ERR_INVALID_STATE : KIMIX_ERR_INVALID_ARG;
    }
    if (self->empty()) {
        return KIMIX_OK; // the input was already valid: there is no repaired text
    }
    /* kimix::repaired_view(): drop the trailing '\0' that repair() appended. */
    out_view->data = reinterpret_cast<const char *>(self->data());
    out_view->length = self->size() - 1;
    return KIMIX_OK;
}

const char *kimix_repaired_cstr(const kimix_vec *result) {
    const byte_vector *self = kimix::api::vec_live(result);
    if (!self || self->empty()) {
        return nullptr;
    }
    /* data()[size() - 1] is '\0' by core's convention, so the buffer is already a
     * C string; this is a pointer cast, not a copy. */
    return reinterpret_cast<const char *>(self->data());
}

KIMIX_FFI_END
