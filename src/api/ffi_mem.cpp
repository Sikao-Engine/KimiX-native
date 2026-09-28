/*
 * ffi_mem.cpp -- implementation of the kimix_mem_* C surface: the one
 * translation unit of the FFI allocation area that sees mimalloc.
 *
 * Structure (mirrors api/ffi_mem.h one-to-one):
 *   - <mimalloc.h> is included HERE and nowhere else in this area.  The mi_*
 *     functions are dllexport'd from this very image (MI_SHARED_LIB /
 *     MI_SHARED_LIB_EXPORT are propagated by the "mimalloc" target, see
 *     src/ext/xmake.lua), so re-declaring them in a public header would give
 *     consumers inconsistent DLL linkage (LNK4273 / dllimport clash) -- the
 *     header therefore stays pure C and only api/ffi_mem.cpp forwards.
 *   - Every exported definition is a thin, exception-free forward plus the
 *     NULL tolerance the header documents.  Nothing here throws, allocates
 *     C++ objects, or uses RTTI; mimalloc's C API is noexcept, and the file
 *     is compiled without exceptions, so no error path can unwind across the
 *     C boundary (contract rule 5 of api/ffi_common.h).  Failures are return
 *     values only: NULL from the allocation calls, kimix_status from
 *     kimix_mem_posix_memalign().
 *   - MI_XMALLOC=1 (same xmake.lua) makes mimalloc's default error handler
 *     abort on ENOMEM/EINVAL/EOVERFLOW, so on this build the NULL-on-failure
 *     contract is what remains observable only if the host replaced that
 *     handler (mi_register_error).  kimix_mem_posix_memalign validates its
 *     arguments BEFORE entering mimalloc so a malformed request can never
 *     reach an aborting path.
 *   - Unity-build safety (same rules as ffi_vec.cpp): every file-local helper
 *     lives in an anonymous namespace under a mem_-prefixed name, and the
 *     exported definitions sit between KIMIX_FFI_BEGIN / KIMIX_FFI_END, which
 *     gives them the C language linkage of the header declarations (the
 *     declarations themselves carry KIMIX_FFI; the definitions inherit the
 *     dllexport).
 *
 * Version identity: the vendored mimalloc.h provides ONLY the integer
 * MI_MALLOC_VERSION ("major + 2 digits minor + 2 digits patch", 30502 =
 * 3.5.2) -- it has no MI_MALLOC_VERSION_MAJOR/MINOR/PATCH macros -- so
 * kimix_mem_version_string() is built below by compile-time decimal
 * decomposition of that single macro.  No literal version can rot here:
 * bumping the vendored mimalloc changes the number and the string together.
 */
#include <api/ffi_mem.h>

#include <mimalloc.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>

namespace {

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
 * second half reduces to `>= sizeof(void*)`.  Checked here, before
 * mimalloc, exactly because MI_XMALLOC=1 turns mimalloc-internal EINVAL
 * reporting into a process abort. */
inline bool mem_valid_posix_alignment(std::size_t alignment) noexcept {
    return alignment >= sizeof(void *) && (alignment & (alignment - 1u)) == 0u;
}

} // namespace

KIMIX_FFI_BEGIN

// ===========================================================================
// 1. Core allocation (the malloc family)
// ===========================================================================

void *kimix_mem_malloc(size_t size) {
    return mi_malloc(size); // mi_malloc(size_t size)
}

void *kimix_mem_calloc(size_t count, size_t size) {
    return mi_calloc(count, size); // mi_calloc(size_t count, size_t size) -- count first
}

void *kimix_mem_zalloc(size_t size) {
    return mi_zalloc(size); // mi_zalloc(size_t size) -- zero-filled
}

void *kimix_mem_realloc(void *p, size_t newsize) {
    /* mi_realloc(void* p, size_t newsize): p NULL acts as mi_malloc(newsize),
     * newsize 0 shrinks to a zero-sized block (NOT free-and-return-NULL).
     * On failure mi_realloc leaves `p` alive and returns NULL. */
    return mi_realloc(p, newsize);
}

void *kimix_mem_expand(void *p, size_t newsize) {
    /* mi_expand(void* p, size_t newsize): in place or nothing; NULL return
     * means `p` is unchanged and still owned by the caller. */
    return mi_expand(p, newsize);
}

void kimix_mem_free(void *p) {
    if (p != nullptr) { // mi_free(NULL) is a documented no-op; guard anyway
        mi_free(p);
    }
}

char *kimix_mem_strdup(const char *s) {
    return s != nullptr ? mi_strdup(s) : nullptr; // mi_strdup(const char* s), NULL -> NULL
}

char *kimix_mem_strndup(const char *s, size_t n) {
    return s != nullptr ? mi_strndup(s, n) : nullptr; // mi_strndup(const char* s, size_t n)
}

// ===========================================================================
// 2. Aligned allocation -- mimalloc's order is (size, alignment), NOT
//    (alignment, size) as in posix_memalign/aligned_alloc.
// ===========================================================================

void *kimix_mem_malloc_aligned(size_t size, size_t alignment) {
    return mi_malloc_aligned(size, alignment);
}

void *kimix_mem_malloc_aligned_at(size_t size, size_t alignment, size_t offset) {
    return mi_malloc_aligned_at(size, alignment, offset);
}

void *kimix_mem_zalloc_aligned(size_t size, size_t alignment) {
    return mi_zalloc_aligned(size, alignment);
}

void *kimix_mem_calloc_aligned(size_t count, size_t size, size_t alignment) {
    return mi_calloc_aligned(count, size, alignment);
}

void *kimix_mem_realloc_aligned(void *p, size_t newsize, size_t alignment) {
    return mi_realloc_aligned(p, newsize, alignment);
}

void kimix_mem_free_aligned(void *p, size_t alignment) {
    /* mi_free_aligned(void* p, size_t alignment): release builds ignore the
     * alignment (debug builds assert p % alignment == 0) and free like
     * mi_free; the argument is kept in the surface so caller code is
     * self-documenting and debug builds get the check. */
    if (p != nullptr) {
        mi_free_aligned(p, alignment);
    }
}

void kimix_mem_free_size(void *p, size_t size) {
    /* mi_free_size(void* p, size_t size): `size` is the original request; it
     * only routes to the small-block fast path, so it must be the size the
     * block was allocated with (the header says so; debug builds verify). */
    if (p != nullptr) {
        mi_free_size(p, size);
    }
}

// ===========================================================================
// 3. Size queries
// ===========================================================================

size_t kimix_mem_usable_size(const void *p) {
    return p != nullptr ? mi_usable_size(p) : 0; // mi_usable_size(const void* p)
}

size_t kimix_mem_good_size(size_t size) {
    return mi_good_size(size); // mi_good_size(size_t size)
}

// ===========================================================================
// 4. Zero-initialising re-allocation (valid only on blocks that were
//    originally allocated zero-initialised -- see the section 4 comment in
//    api/ffi_mem.h and mimalloc.h's "Zero initialized re-allocation").
// ===========================================================================

void *kimix_mem_rezalloc(void *p, size_t newsize) {
    return mi_rezalloc(p, newsize); // mi_rezalloc(void* p, size_t newsize)
}

void *kimix_mem_recalloc(void *p, size_t newcount, size_t size) {
    return mi_recalloc(p, newcount, size); // mi_recalloc(void* p, size_t newcount, size_t size)
}

// ===========================================================================
// 5. posix-shaped aligned allocation
// ===========================================================================

kimix_status kimix_mem_posix_memalign(void **p, size_t alignment, size_t size) {
    if (p == nullptr || !mem_valid_posix_alignment(alignment)) {
        return KIMIX_ERR_INVALID_ARG; // rejected before mimalloc is entered
    }
    /* mi_posix_memalign(void** p, size_t alignment, size_t size) -- posix
     * order.  It re-checks the arguments (EINVAL) and leaves *p untouched on
     * any error, but the pre-check above means only the allocation outcome
     * can still arrive here: 0 on success, ENOMEM on failure (with this
     * build's MI_XMALLOC=1 a true OOM aborts inside mimalloc before
     * returning, as documented in the header). */
    const int rc = mi_posix_memalign(p, alignment, size);
    if (rc == 0) {
        return KIMIX_OK;
    }
    if (rc == ENOMEM) {
        return KIMIX_ERR_OUT_OF_MEMORY;
    }
    return KIMIX_ERR_INVALID_ARG; // EINVAL: unreachable after the check, kept for safety
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

KIMIX_FFI_END
