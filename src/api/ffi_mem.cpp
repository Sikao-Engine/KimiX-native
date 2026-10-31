/*
 * ffi_mem.cpp -- implementation of the kimix_mem_* C surface.
 *
 * Structure (mirrors api/ffi_mem.h one-to-one):
 *   - Every allocation entry point is a thin, exception-free forward to the
 *     kimix::allocate / kimix::deallocate gateway (src/core/memory.h,
 *     implemented in src/core/memory.cpp), which is the ONE place in project
 *     code that calls the mimalloc allocation family.  Nothing here throws,
 *     allocates C++ objects, or uses RTTI; mimalloc's C API is noexcept and this
 *     file is compiled without exceptions, so no error path can unwind across the
 *     C boundary (contract rule 5 of api/ffi_common.h).  Failures are return
 *     values only: NULL from the allocation calls, kimix_status from
 *     kimix_mem_posix_memalign().
 *   - <mimalloc.h> is still included HERE and nowhere else in this area, but now
 *     only for the NON-allocation parts of the surface that this header also
 *     re-exports: MI_MALLOC_VERSION decoding, mi_thread_init / mi_thread_done /
 *     mi_collect, mi_is_redirected.  The allocation calls go through the gateway,
 *     so the FFI heap and the container heap are one tracked call path: with
 *     KIMIX_MEMORY_STRICT every block handed out here is recorded by
 *     src/core/memory.cpp and reported at exit if it is never released.
 *   - Standalone static kimix-core carries mi_* into this image; the external
 *     leg imports the shared kimix-core/luisa-core allocation path instead.
 *     Public headers still expose only kimix_mem_* names, never mi_* symbols.
 *   - MI_XMALLOC=1 (same xmake.lua) makes mimalloc's default error handler abort
 *     on ENOMEM/EINVAL/EOVERFLOW, so on this build the NULL-on-failure contract
 *     is what remains observable only if the host replaced that handler
 *     (mi_register_error).  kimix_mem_posix_memalign validates its arguments
 *     BEFORE entering mimalloc so a malformed request can never reach an
 *     aborting path.
 *   - Unity-build safety (same rules as ffi_vec.cpp): every file-local helper
 *     lives in a named namespace under a mem_-prefixed name (AGENTS.md forbids
 *     anonymous namespaces), and the exported definitions sit between
 *     KIMIX_FFI_BEGIN / KIMIX_FFI_END, which gives them the C language linkage of
 *     the header declarations (the declarations themselves carry KIMIX_FFI; the
 *     definitions inherit the dllexport).
 *
 * Version identity: the vendored mimalloc.h provides ONLY the integer
 * MI_MALLOC_VERSION ("major + 2 digits minor + 2 digits patch", 30502 =
 * 3.5.2) -- it has no MI_MALLOC_VERSION_MAJOR/MINOR/PATCH macros -- so
 * kimix_mem_version_string() is built below by compile-time decimal
 * decomposition of that single macro.  No literal version can rot here:
 * bumping the vendored mimalloc changes the number and the string together.
 */
#include <api/ffi_mem.h>

#include <mimalloc.h> // MI_MALLOC_VERSION, mi_thread_*, mi_collect, mi_is_redirected

#include <core/memory.h> // the kimix::allocate / kimix::deallocate gateway

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring> // memcpy / memmove / memset / memcmp -- section 8 forwards

namespace src_api_ffi_mem {

/* -------------------------------------------------------------------------
 * Compile-time decomposition of MI_MALLOC_VERSION (section 7 of the header).
 * Encoding, from mimalloc.h itself:  major * 10000 + minor * 100 + patch.
 * ------------------------------------------------------------------------- */
constexpr std::uint32_t mem_mi_version = static_cast<std::uint32_t>(MI_MALLOC_VERSION);
constexpr std::uint32_t mem_mi_version_major = mem_mi_version / 10000u;
constexpr std::uint32_t mem_mi_version_minor = (mem_mi_version / 100u) % 100u;
constexpr std::uint32_t mem_mi_version_patch = mem_mi_version % 100u;

/* Documents (and checks that the macro is still a plain decomposable
 * integer under) the encoding above; the three parts are derived from the
 * macro by definition, so this asserts the encoding, not a transcription. */
static_assert(mem_mi_version_major * 10000u + mem_mi_version_minor * 100u + mem_mi_version_patch == MI_MALLOC_VERSION,
              "MI_MALLOC_VERSION no longer decodes as major*10000 + minor*100 + patch");

/* The "major.minor.patch" text (e.g. "3.5.2"), computed at compile time.
 * Minor and patch lose their encoding's padding zeros: that is how mimalloc
 * itself writes release versions. */
struct mem_version_text_t {
    std::array<char, 32> buf;
    std::size_t len; // visible characters; buf[len] == '\0'
};

constexpr mem_version_text_t mem_build_version_text() noexcept {
    mem_version_text_t text{};
    const std::uint32_t parts[3] = {mem_mi_version_major, mem_mi_version_minor, mem_mi_version_patch};
    for (std::size_t i = 0; i < 3; ++i) {
        if (i != 0) {
            text.buf[text.len++] = '.';
        }
        std::uint32_t value = parts[i];
        if (value == 0u) {
            text.buf[text.len++] = '0';
        } else {
            char digits[16];
            std::size_t n = 0;
            while (value != 0u) { // reversed decimal digits
                digits[n++] = static_cast<char>('0' + static_cast<std::uint32_t>(value % 10u));
                value /= 10u;
            }
            while (n != 0u) {
                text.buf[text.len++] = digits[--n];
            }
        }
    }
    text.buf[text.len] = '\0';
    return text;
}

constexpr mem_version_text_t mem_mi_version_text = mem_build_version_text();
static_assert(mem_mi_version_text.buf[mem_mi_version_text.len] == '\0',
              "the version text must be NUL-terminated at len");
static_assert(mem_mi_version_text.len >= 5 && mem_mi_version_text.len < mem_mi_version_text.buf.size(),
              "the compile-time version text does not fit or is malformed");

/* The posix_memalign rule (see the C standard / man page): alignment is a
 * power of two and a multiple of sizeof(void*) -- for powers of two the
 * second half reduces to `>= sizeof(void*)`.  Checked here, before mimalloc,
 * exactly because MI_XMALLOC=1 turns mimalloc-internal EINVAL reporting into a
 * process abort. */
inline bool mem_valid_posix_alignment(std::size_t alignment) noexcept {
    return alignment >= sizeof(void *) && (alignment & (alignment - 1u)) == 0u;
}

/* The leak tracker this area used to carry (a kimix::unordered_set<void*> of
 * every block handed out) is gone: tracking now lives in src/core/memory.cpp,
 * behind KIMIX_MEMORY_STRICT, and covers the FFI surface because every
 * allocation below goes through the gateway.  There is exactly one record per
 * block in the process, and it does not matter whether it was allocated by a
 * container, a JSON document or kimix_mem_malloc(). */

} // namespace src_api_ffi_mem
using namespace src_api_ffi_mem;

KIMIX_FFI_BEGIN

// ===========================================================================
// 1. Core allocation (the malloc family)
// ===========================================================================

void *kimix_mem_malloc(size_t size) {
    return kimix::allocate(size); // mi_malloc(size_t size)
}

void *kimix_mem_calloc(size_t count, size_t size) {
    return kimix::allocate_n(count, size); // mi_calloc(count, size) -- count first
}

void *kimix_mem_zalloc(size_t size) {
    return kimix::allocate_zeroed(size); // mi_zalloc(size_t size) -- zero-filled
}

void *kimix_mem_realloc(void *p, size_t newsize) {
    /* kimix::reallocate == mi_realloc(void* p, size_t newsize): p NULL acts as a
     * fresh allocation, newsize 0 shrinks to a zero-sized block (NOT
     * free-and-return-NULL).  On failure the original block stays alive and
     * NULL is returned. */
    return kimix::reallocate(p, newsize);
}

void *kimix_mem_expand(void *p, size_t newsize) {
    /* kimix::expand == mi_expand(void* p, size_t newsize): in place or nothing;
     * a NULL return means `p` is unchanged and still owned by the caller.  The
     * result is `p` itself on success, and the tracker's record is moved/updated
     * inside the gateway (never here). */
    return kimix::expand(p, newsize);
}

void kimix_mem_free(void *p) {
    kimix::deallocate(p); // mi_free(NULL) is a documented no-op; the gateway keeps it
}

char *kimix_mem_strdup(const char *s) {
    return kimix::duplicate_string(s); // mi_strdup(const char* s), NULL -> NULL
}

char *kimix_mem_strndup(const char *s, size_t n) {
    return kimix::duplicate_string_n(s, n); // mi_strndup(const char* s, size_t n)
}

// ===========================================================================
// 2. Aligned allocation -- mimalloc's order is (size, alignment), NOT
//    (alignment, size) as in posix_memalign/aligned_alloc.
// ===========================================================================

void *kimix_mem_malloc_aligned(size_t size, size_t alignment) {
    return kimix::allocate_aligned(size, alignment);
}

void *kimix_mem_malloc_aligned_at(size_t size, size_t alignment, size_t offset) {
    return kimix::allocate_aligned_at(size, alignment, offset);
}

void *kimix_mem_zalloc_aligned(size_t size, size_t alignment) {
    return kimix::allocate_zeroed_aligned(size, alignment);
}

void *kimix_mem_calloc_aligned(size_t count, size_t size, size_t alignment) {
    return kimix::allocate_n_aligned(count, size, alignment);
}

void *kimix_mem_realloc_aligned(void *p, size_t newsize, size_t alignment) {
    return kimix::reallocate_aligned(p, newsize, alignment);
}

void kimix_mem_free_aligned(void *p, size_t alignment) {
    /* mi_free_aligned(void* p, size_t alignment): release builds ignore the
     * alignment (debug builds assert p % alignment == 0) and free like mi_free;
     * the argument is kept in the surface so caller code is self-documenting and
     * debug builds get the check. */
    kimix::deallocate_aligned(p, alignment);
}

void kimix_mem_free_size(void *p, size_t size) {
    /* mi_free_size(void* p, size_t size): `size` is the original request; it
     * only routes to the small-block fast path, so it must be the size the
     * block was allocated with (the header says so; debug builds verify). */
    kimix::deallocate_size(p, size);
}

// ===========================================================================
// 3. Size queries
// ===========================================================================

size_t kimix_mem_usable_size(const void *p) {
    return kimix::usable_size(p); // mi_usable_size(const void* p); NULL -> 0 inside
}

size_t kimix_mem_good_size(size_t size) {
    return kimix::good_size(size); // mi_good_size(size_t size)
}

// ===========================================================================
// 4. Zero-initialising re-allocation (valid only on blocks that were
//    originally allocated zero-initialised -- see the section 4 comment in
//    api/ffi_mem.h and mimalloc.h's "Zero initialized re-allocation").
// ===========================================================================

void *kimix_mem_rezalloc(void *p, size_t newsize) {
    return kimix::reallocate_zeroed(p, newsize); // mi_rezalloc(void* p, size_t newsize)
}

void *kimix_mem_recalloc(void *p, size_t newcount, size_t size) {
    return kimix::reallocate_n(p, newcount, size); // mi_recalloc(void* p, size_t newcount, size_t size)
}

// ===========================================================================
// 5. posix-shaped aligned allocation
// ===========================================================================

kimix_status kimix_mem_posix_memalign(void **p, size_t alignment, size_t size) {
    if (p == nullptr || !mem_valid_posix_alignment(alignment)) {
        return KIMIX_ERR_INVALID_ARG; // rejected before mimalloc is entered
    }
    /* The posix contract: on any error `*p` is left exactly untouched, on
     * success it receives the block.  The argument validation above is what the
     * posix rule requires (a power of two >= sizeof(void*)); what is left is the
     * allocation outcome.  mimalloc's size-first order is used here because the
     * posix (alignment-first) order is this function's own signature. */
    void *const block = kimix::allocate_aligned(size, alignment);
    if (block == nullptr) {
        return KIMIX_ERR_OUT_OF_MEMORY; // with this build's MI_XMALLOC=1 a true OOM aborts inside mimalloc instead
    }
    *p = block;
    return KIMIX_OK;
}

// ===========================================================================
// 6. Thread and heap management for foreign threads
// ===========================================================================

void kimix_mem_thread_init(void) {
    mi_thread_init(); // mi_thread_init(void) -- per-thread heap registration
}

void kimix_mem_thread_done(void) {
    mi_thread_done(); // mi_thread_done(void) -- eager release of this thread's heap
}

void kimix_mem_collect(bool force) {
    mi_collect(force); // mi_collect(bool force) -- safe from any thread
}

// ===========================================================================
// 7. Identity
// ===========================================================================

uint32_t kimix_mem_version(void) {
    return mem_mi_version; // == MI_MALLOC_VERSION, the compile-time mimalloc number (e.g. 30502)
}

const char *kimix_mem_version_string(void) {
    return mem_mi_version_text.buf.data(); // compile-time "3.5.2"-style text; static storage
}

bool kimix_mem_is_redirected(void) {
    return mi_is_redirected(); // mi_is_redirected(void) -- bool is 1 byte on both sides
}

// ===========================================================================
// 8. Raw block operations (the <string.h> vocabulary)
//
// Pure in-place forwards to the C runtime's block primitives -- nothing here
// allocates or frees, so the leak tracker is untouched.  The n == 0 guards
// implement the header's NULL contract: with nothing to read or write, any
// pointer value (including NULL) is acceptable, exactly as with the C
// functions; a NULL pointer with n > 0 stays UB by design.
// ===========================================================================

void kimix_mem_copy(void *dst, const void *src, size_t n) {
    if (n != 0u) { // disjoint ranges only (memcpy semantics) -- see the header
        std::memcpy(dst, src, n);
    }
}

void kimix_mem_move(void *dst, const void *src, size_t n) {
    if (n != 0u) { // overlap-safe in either direction (memmove semantics)
        std::memmove(dst, src, n);
    }
}

void kimix_mem_swap(void *a, void *b, size_t n) {
    if (a == b) {
        return; // same range: nothing to exchange
    }
    /* Elementwise exchange -- no scratch block, so this allocates nothing and
     * stays correct even on a foreign thread that never registered with the
     * heap.  Disjoint ranges only (per the header contract). */
    unsigned char *pa = static_cast<unsigned char *>(a);
    unsigned char *pb = static_cast<unsigned char *>(b);
    for (size_t i = 0; i < n; ++i) {
        const unsigned char tmp = pa[i];
        pa[i] = pb[i];
        pb[i] = tmp;
    }
}

void kimix_mem_set(void *dst, int byte, size_t n) {
    if (n != 0u) {
        std::memset(dst, byte, n);
    }
}

void kimix_mem_zero(void *dst, size_t n) {
    if (n != 0u) {
        std::memset(dst, 0, n);
    }
}

int kimix_mem_compare(const void *a, const void *b, size_t n) {
    if (n == 0u) {
        return 0; // nothing to compare: equal, without touching the pointers
    }
    /* Normalise memcmp's arbitrary sign/magnitude to a strict -1 / 0 / +1 so
     * a binding observes exactly three values across platforms. */
    const int rc = std::memcmp(a, b, n);
    return (rc < 0) ? -1 : (rc > 0) ? 1 : 0;
}

bool kimix_mem_equals(const void *a, const void *b, size_t n) {
    return kimix_mem_compare(a, b, n) == 0;
}

size_t kimix_mem_find_byte(const void *p, size_t n, unsigned char byte) {
    if (n == 0u) {
        return static_cast<size_t>(-1); // empty range: not found, pointer untouched
    }
    const void *const hit = std::memchr(p, byte, n);
    return hit != nullptr ? static_cast<size_t>(static_cast<const unsigned char *>(hit) -
                                                static_cast<const unsigned char *>(p))
                          : static_cast<size_t>(-1);
}

size_t kimix_mem_count_byte(const void *p, size_t n, unsigned char byte) {
    size_t count = 0;
    const unsigned char *cur = static_cast<const unsigned char *>(p);
    for (size_t i = 0; i < n; ++i) { // memchr-chasing loop, plain and branchy-light
        count += (cur[i] == byte) ? 1u : 0u;
    }
    return count;
}

KIMIX_FFI_END
