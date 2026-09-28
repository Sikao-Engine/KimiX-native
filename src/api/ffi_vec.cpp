/*
 * ffi_vec.cpp -- implementation of the kimix::vector<std::byte> C FFI.
 *
 * Every entry point below is a thin, exception-free bridge: it validates the
 * caller's placeholder (NULL? live object? index in range?), re-derives the C++
 * object from the storage with kimix::api::vec_live() (std::launder, see
 * api/detail.h), forwards one member call, and maps the outcome onto
 * kimix_status / an out-parameter.  Construction and destruction are the only
 * places that create or end the object's lifetime:
 *
 *   kimix_vec_default_init / _init_from / _copy_init / _move_init
 *        -> placement new of byte_vector + live guard
 *   kimix_vec_destroy / kimix_vec_free
 *        -> explicit ~vector() + dead guard
 *
 * Nothing else may assume an object exists.  A placeholder whose guard word does
 * not say "live" is reported as KIMIX_ERR_INVALID_STATE, so a missing or double
 * init/destroy is a return value rather than memory corruption.
 *
 * OOM note: the calls below cannot report KIMIX_ERR_OUT_OF_MEMORY for the
 * vector's own buffer, because kimix::allocator reports exhaustion by calling
 * kimix::allocation_failure() and aborting (core/stl/memory.h) -- the project is
 * built without C++ exceptions, so std::bad_alloc never unwinds into this FFI.
 * The code is listed in the header for completeness and for the heap helpers
 * (kimix_vec_new*), which allocate the placeholder storage with mi_malloc and
 * can genuinely fail.
 */
#include <api/detail.h>
#include <api/ffi_vec.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

namespace {

using kimix::api::byte_vector;
using kimix::api::char_vector;
using kimix::api::k_vec_live_guard;

/* The value kimix_vec_size() / kimix_vec_capacity() return for an invalid
 * placeholder: an out-of-band size no buffer can ever have. */
constexpr std::size_t k_bad_size = static_cast<std::size_t>(-1);

inline const std::byte *as_bytes(const void *p) noexcept {
    return reinterpret_cast<const std::byte *>(p);
}

inline unsigned char *as_uchar(std::byte *p) noexcept {
    return reinterpret_cast<unsigned char *>(p);
}

/* A null `v` is an argument error; a dead placeholder is a state error.  Used by
 * the calls that return a kimix_status directly. */
inline kimix_status check(kimix_vec *v) noexcept {
    if (!v) {
        return KIMIX_ERR_INVALID_ARG;
    }
    return kimix::api::vec_read_guard(v) == k_vec_live_guard ? KIMIX_OK
                                                             : KIMIX_ERR_INVALID_STATE;
}

inline kimix_status check(const kimix_vec *v) noexcept {
    return check(const_cast<kimix_vec *>(v));
}

} // namespace

/* ---------------------------------------------------------------------------
 * The ABI of the placeholder, frozen against the real C++ type.
 *
 * The values below are what makes this FFI safe to use from another module: the
 * header's fixed KIMIX_VEC_BYTES / KIMIX_VEC_ALIGN must always be enough for the
 * object the library actually builds (which grows with the standard library's
 * debug proxy, e.g. 24 bytes for libstdc++ release, 40 for an MSVC
 * _ITERATOR_DEBUG_LEVEL=2 debug build).  If one of these asserts ever fires, the
 * fix is to raise KIMIX_VEC_BYTES (and KIMIX_API_ABI_VERSION), never to shrink
 * the object.
 * ------------------------------------------------------------------------- */
static_assert(sizeof(kimix_vec) == KIMIX_VEC_BYTES,
              "kimix_vec must be exactly KIMIX_VEC_BYTES wide");
static_assert(alignof(kimix_vec) == KIMIX_VEC_ALIGN,
              "kimix_vec must be exactly KIMIX_VEC_ALIGN aligned");
static_assert(sizeof(byte_vector) + sizeof(std::uint64_t) <= KIMIX_VEC_BYTES,
              "the vector object and the guard word do not fit in the placeholder");
static_assert(alignof(byte_vector) <= KIMIX_VEC_ALIGN,
              "the placeholder is not aligned enough for the vector object");
static_assert(kimix::api::k_vec_guard_offset >= sizeof(byte_vector),
              "the guard slot overlaps the vector object");
/* The vector<char> <-> vector<std::byte> bridge of ffi_repair.cpp. */
static_assert(sizeof(byte_vector) == sizeof(char_vector),
              "vector<char> and vector<std::byte> must have the same size");
static_assert(alignof(byte_vector) == alignof(char_vector),
              "vector<char> and vector<std::byte> must have the same alignment");
static_assert(sizeof(std::byte) == sizeof(char) == 1, "unexpected element size");
static_assert(KIMIX_VEC_BYTES == 64u && KIMIX_VEC_ALIGN == 16u,
              "KIMIX_VEC_BYTES/ALIGN are part of the published ABI");

KIMIX_FFI_BEGIN

// ===========================================================================
// 1. ABI facts
// ===========================================================================

size_t kimix_vec_abi_bytes(void) {
    return KIMIX_VEC_BYTES;
}

size_t kimix_vec_abi_align(void) {
    return KIMIX_VEC_ALIGN;
}

size_t kimix_vec_inner_bytes(void) {
    return sizeof(byte_vector);
}

size_t kimix_vec_inner_align(void) {
    return alignof(byte_vector);
}

bool kimix_vec_is_initialized(const kimix_vec *v) {
    return v && kimix::api::vec_read_guard(v) == k_vec_live_guard;
}

// ===========================================================================
// 2. Construction and destruction
// ===========================================================================

kimix_status kimix_vec_default_init(kimix_vec *v) {
    if (!v) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (!kimix::api::vec_is_raw_storage(v)) {
        return KIMIX_ERR_INVALID_STATE; // would leak the live vector's buffer
    }
    kimix::api::vec_construct(v);
    return KIMIX_OK;
}

kimix_status kimix_vec_init_from(kimix_vec *v, const void *data, size_t len) {
    if (!v) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (!data && len) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (!kimix::api::vec_is_raw_storage(v)) {
        return KIMIX_ERR_INVALID_STATE;
    }
    const auto *first = as_bytes(data);
    kimix::api::vec_construct(v, first, first + len);
    return KIMIX_OK;
}

kimix_status kimix_vec_copy_init(kimix_vec *dst, const kimix_vec *src) {
    if (!dst || !src) {
        return KIMIX_ERR_INVALID_ARG;
    }
    const auto *from = kimix::api::vec_live(src);
    if (!from) {
        return check(src);
    }
    if (!kimix::api::vec_is_raw_storage(dst)) {
        return KIMIX_ERR_INVALID_STATE;
    }
    kimix::api::vec_construct(dst, *from);
    return KIMIX_OK;
}

kimix_status kimix_vec_move_init(kimix_vec *dst, kimix_vec *src) {
    if (!dst || !src) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (dst == src) {
        return KIMIX_ERR_INVALID_ARG; // moving an object onto itself
    }
    auto *from = kimix::api::vec_live(src);
    if (!from) {
        return check(src);
    }
    if (!kimix::api::vec_is_raw_storage(dst)) {
        return KIMIX_ERR_INVALID_STATE;
    }
    /* new (dst) vector<byte>(std::move(*src)): the buffer pointer travels, so
     * the allocation stays owned by the heap that made it, and src becomes a
     * valid empty vector that its owner still has to destroy once. */
    kimix::api::vec_construct_move(dst, std::move(*from));
    return KIMIX_OK;
}

kimix_status kimix_vec_destroy(kimix_vec *v) {
    if (!v) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (!kimix::api::vec_live(v)) {
        return KIMIX_ERR_INVALID_STATE;
    }
    kimix::api::vec_destruct(v);
    return KIMIX_OK;
}

// ===========================================================================
// 3. Assignment
// ===========================================================================

kimix_status kimix_vec_assign(kimix_vec *dst, const kimix_vec *src) {
    if (!dst || !src) {
        return KIMIX_ERR_INVALID_ARG;
    }
    const auto *from = kimix::api::vec_live(src);
    if (!from) {
        return check(src);
    }
    auto *to = kimix::api::vec_live(dst);
    if (!to) {
        return check(dst);
    }
    *to = *from; // deep copy; dst's old buffer returns to the library heap
    return KIMIX_OK;
}

kimix_status kimix_vec_move_assign(kimix_vec *dst, kimix_vec *src) {
    if (!dst || !src) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (dst == src) {
        return KIMIX_ERR_INVALID_ARG;
    }
    auto *from = kimix::api::vec_live(src);
    if (!from) {
        return check(src);
    }
    auto *to = kimix::api::vec_live(dst);
    if (!to) {
        return check(dst);
    }
    *to = std::move(*from);
    return KIMIX_OK;
}

kimix_status kimix_vec_assign_bytes(kimix_vec *v, const void *data, size_t len) {
    if (!data && len) {
        return KIMIX_ERR_INVALID_ARG;
    }
    auto *self = kimix::api::vec_live(v);
    if (!self) {
        return check(v);
    }
    const auto *first = as_bytes(data);
    self->assign(first, first + len);
    return KIMIX_OK;
}

// ===========================================================================
// 4. Observation
// ===========================================================================

size_t kimix_vec_size(const kimix_vec *v) {
    const auto *self = kimix::api::vec_live(v);
    return self ? self->size() : k_bad_size;
}

size_t kimix_vec_capacity(const kimix_vec *v) {
    const auto *self = kimix::api::vec_live(v);
    return self ? self->capacity() : k_bad_size;
}

bool kimix_vec_empty(const kimix_vec *v) {
    const auto *self = kimix::api::vec_live(v);
    return self ? self->empty() : false;
}

unsigned char *kimix_vec_data(kimix_vec *v) {
    auto *self = kimix::api::vec_live(v);
    return self ? as_uchar(self->data()) : nullptr;
}

kimix_status kimix_vec_at(kimix_vec *v, size_t index, unsigned char **out) {
    if (!out) {
        return KIMIX_ERR_INVALID_ARG;
    }
    *out = nullptr;
    auto *self = kimix::api::vec_live(v);
    if (!self) {
        return check(v);
    }
    if (index >= self->size()) {
        return KIMIX_ERR_OUT_OF_RANGE;
    }
    *out = as_uchar(self->data() + index);
    return KIMIX_OK;
}

kimix_status kimix_vec_read(const kimix_vec *v, size_t offset, void *dst, size_t len) {
    if (!dst && len) {
        return KIMIX_ERR_INVALID_ARG;
    }
    const auto *self = kimix::api::vec_live(v);
    if (!self) {
        return check(v);
    }
    if (offset > self->size() || len > self->size() - offset) {
        return KIMIX_ERR_OUT_OF_RANGE;
    }
    if (len) {
        std::memcpy(dst, self->data() + offset, len);
    }
    return KIMIX_OK;
}

kimix_status kimix_vec_write(kimix_vec *v, size_t offset, const void *src, size_t len) {
    if (!src && len) {
        return KIMIX_ERR_INVALID_ARG;
    }
    auto *self = kimix::api::vec_live(v);
    if (!self) {
        return check(v);
    }
    if (offset > self->size() || len > self->size() - offset) {
        return KIMIX_ERR_OUT_OF_RANGE;
    }
    if (len) {
        std::memcpy(self->data() + offset, as_bytes(src), len);
    }
    return KIMIX_OK;
}

kimix_status kimix_vec_equals(const kimix_vec *a, const kimix_vec *b, bool *out_equal) {
    if (!out_equal) {
        return KIMIX_ERR_INVALID_ARG;
    }
    *out_equal = false;
    const auto *left = kimix::api::vec_live(a);
    if (!left) {
        return check(a);
    }
    const auto *right = kimix::api::vec_live(b);
    if (!right) {
        return check(b);
    }
    *out_equal = (*left == *right);
    return KIMIX_OK;
}

// ===========================================================================
// 5. Modification
// ===========================================================================

kimix_status kimix_vec_resize(kimix_vec *v, size_t n) {
    auto *self = kimix::api::vec_live(v);
    if (!self) {
        return check(v);
    }
    self->resize(n);
    return KIMIX_OK;
}

kimix_status kimix_vec_resize_fill(kimix_vec *v, size_t n, unsigned char fill) {
    auto *self = kimix::api::vec_live(v);
    if (!self) {
        return check(v);
    }
    self->resize(n, static_cast<std::byte>(fill));
    return KIMIX_OK;
}

kimix_status kimix_vec_reserve(kimix_vec *v, size_t n) {
    auto *self = kimix::api::vec_live(v);
    if (!self) {
        return check(v);
    }
    self->reserve(n);
    return KIMIX_OK;
}

kimix_status kimix_vec_shrink_to_fit(kimix_vec *v) {
    auto *self = kimix::api::vec_live(v);
    if (!self) {
        return check(v);
    }
    self->shrink_to_fit();
    return KIMIX_OK;
}

kimix_status kimix_vec_clear(kimix_vec *v) {
    auto *self = kimix::api::vec_live(v);
    if (!self) {
        return check(v);
    }
    self->clear();
    return KIMIX_OK;
}

kimix_status kimix_vec_push_back(kimix_vec *v, unsigned char b) {
    auto *self = kimix::api::vec_live(v);
    if (!self) {
        return check(v);
    }
    self->push_back(static_cast<std::byte>(b));
    return KIMIX_OK;
}

kimix_status kimix_vec_pop_back(kimix_vec *v) {
    auto *self = kimix::api::vec_live(v);
    if (!self) {
        return check(v);
    }
    if (self->empty()) {
        return KIMIX_ERR_OUT_OF_RANGE;
    }
    self->pop_back();
    return KIMIX_OK;
}

kimix_status kimix_vec_append(kimix_vec *v, const void *data, size_t len) {
    if (!data && len) {
        return KIMIX_ERR_INVALID_ARG;
    }
    auto *self = kimix::api::vec_live(v);
    if (!self) {
        return check(v);
    }
    if (!len) {
        return KIMIX_OK;
    }
    const auto *first = as_bytes(data);
    self->insert(self->end(), first, first + len);
    return KIMIX_OK;
}

kimix_status kimix_vec_insert(kimix_vec *v, size_t index, const void *data, size_t len) {
    if (!data && len) {
        return KIMIX_ERR_INVALID_ARG;
    }
    auto *self = kimix::api::vec_live(v);
    if (!self) {
        return check(v);
    }
    if (index > self->size()) {
        return KIMIX_ERR_OUT_OF_RANGE;
    }
    if (!len) {
        return KIMIX_OK;
    }
    const auto *first = as_bytes(data);
    self->insert(self->begin() + index, first, first + len);
    return KIMIX_OK;
}

kimix_status kimix_vec_erase(kimix_vec *v, size_t index, size_t len) {
    auto *self = kimix::api::vec_live(v);
    if (!self) {
        return check(v);
    }
    if (index > self->size() || len > self->size() - index) {
        return KIMIX_ERR_OUT_OF_RANGE;
    }
    if (len) {
        self->erase(self->begin() + index, self->begin() + index + len);
    }
    return KIMIX_OK;
}

kimix_status kimix_vec_swap(kimix_vec *a, kimix_vec *b) {
    if (!a || !b) {
        return KIMIX_ERR_INVALID_ARG;
    }
    auto *left = kimix::api::vec_live(a);
    if (!left) {
        return check(a);
    }
    auto *right = kimix::api::vec_live(b);
    if (!right) {
        return check(b);
    }
    left->swap(*right);
    return KIMIX_OK;
}

// ===========================================================================
// 6. Heap convenience
// ===========================================================================

kimix_vec *kimix_vec_new(void) {
    /* Aligned allocation: the placeholder carries an over-aligned C++ object,
     * and mi_malloc only guarantees word alignment for small blocks. */
    auto *v = static_cast<kimix_vec *>(mi_malloc_aligned(KIMIX_VEC_BYTES, KIMIX_VEC_ALIGN));
    if (!v) {
        return nullptr;
    }
    /* The guard says "no live object" until vec_construct flips it. */
    kimix::api::vec_write_guard(v, 0);
    kimix::api::vec_construct(v);
    return v;
}

kimix_vec *kimix_vec_new_from(const void *data, size_t len) {
    if (!data && len) {
        return nullptr;
    }
    auto *v = kimix_vec_new();
    if (!v) {
        return nullptr;
    }
    const auto *first = as_bytes(data);
    kimix::api::vec_object(v)->assign(first, first + len);
    return v;
}

void kimix_vec_free(kimix_vec *v) {
    if (!v) {
        return;
    }
    kimix::api::vec_destruct(v);
    mi_free(v);
}

KIMIX_FFI_END
