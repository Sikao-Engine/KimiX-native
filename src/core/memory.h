/*
 * memory.h — the kimix::allocate / kimix::deallocate allocation gateway.
 *
 * Every heap allocation in project code goes through this header, never
 * through mi_* directly (the vendored mimalloc in src/ext and the FFI forwards
 * of src/api/ffi_mem.cpp are the only two exceptions, and both are documented
 * there). kimix::allocator<T> — and therefore every kimix:: container,
 * kimix::string, kimix::vector, yyjson_alc, lua_Alloc and IOperatorNewBase —
 * is a client of these functions. Each standalone final image has its own heap
 * and leak check; the external shared-core leg keeps both process-wide.
 *
 * The functions are thin, noexcept forwards to the mimalloc family and they
 * return nullptr on failure instead of aborting (kimix::allocator<T> keeps its
 * own kimix::allocation_failure() abort, see core/stl/memory.h):
 *
 *   allocate(size)                       mi_malloc(size)
 *   allocate_zeroed(size)                mi_zalloc(size)
 *   allocate_n(count, size)              mi_calloc(count, size)
 *   allocate_aligned(size, align)        mi_malloc_aligned
 *   allocate_aligned_at(size, align, off) mi_malloc_aligned_at (`(p+off)%align==0`)
 *   allocate_zeroed_aligned(...)         mi_zalloc_aligned
 *   allocate_n_aligned(count, size, a)   mi_calloc_aligned
 *   reallocate / reallocate_aligned      mi_realloc / mi_realloc_aligned
 *   reallocate_zeroed / reallocate_n     mi_rezalloc / mi_recalloc
 *   expand(p, size)                      mi_expand  (in place or nullptr)
 *   deallocate(p)                        mi_free    (NULL-safe)
 *   deallocate_aligned / deallocate_size mi_free_aligned / mi_free_size
 *   usable_size / good_size              mi_usable_size / mi_good_size
 *   duplicate_string / duplicate_string_n mi_strdup / mi_strndup
 *
 * OPTIONAL LEAK TRACKING (KIMIX_MEMORY_STRICT)
 *   Debug builds can record every live block in a mutex-guarded hash map and
 *   report the ones still live at process exit. It is OFF by default and off
 *   means COMPILED OUT — the tracker does not exist in the binary:
 *
 *     | build                     | KIMIX_MEMORY_STRICT | behaviour            |
 *     |---------------------------|---------------------|----------------------|
 *     | debug (no NDEBUG)         | off (default)       | thin forward only    |
 *     | debug (no NDEBUG)         | on                  | per-allocation track |
 *     | release / releasedbg      | either              | thin forward only    |
 *
 *   Enable it with `xmake f -m debug --kimix_memory_strict=true` (root
 *   xmake.lua option; the define is public on kimix-core, so tests see it too).
 *   Release builds ignore the option and src/core/memory.cpp emits a
 *   #pragma message saying so. See .agents/skills/test/SKILL.md.
 *
 * SYMMETRY INVARIANT (what makes the leak report meaningful):
 *   a block released through kimix::deallocate* MUST have been handed out by
 *   kimix::allocate*. Tracking is per-function, not per-heap: freeing a mi_
 *   block through the gateway (or a gateway block through mi_free) leaves a
 *   stale or missing record, i.e. a false leak or a missing one. That is why
 *   an allocation site and its release site are migrated together.
 *
 * Example:
 *   void *p = kimix::allocate(64);
 *   char *s = kimix::duplicate_string("abc");
 *   kimix::deallocate(s);
 *   kimix::deallocate(p);
 */
#pragma once

#include "dll_export.h"

#include <cstddef>
#include <cstdio>
#include <new>

namespace kimix {

// ---------------------------------------------------------------------------
// Allocation (mimalloc family, noexcept, nullptr on failure)
// ---------------------------------------------------------------------------

KIMIX_CORE_API void *allocate(size_t size) noexcept;
KIMIX_CORE_API void *allocate_zeroed(size_t size) noexcept;
KIMIX_CORE_API void *allocate_n(size_t count, size_t size) noexcept;
KIMIX_CORE_API void *allocate_aligned(size_t size, size_t alignment) noexcept;
KIMIX_CORE_API void *allocate_aligned_at(size_t size, size_t alignment, size_t offset) noexcept;
KIMIX_CORE_API void *allocate_zeroed_aligned(size_t size, size_t alignment) noexcept;
KIMIX_CORE_API void *allocate_n_aligned(size_t count, size_t size, size_t alignment) noexcept;
KIMIX_CORE_API void *reallocate(void *p, size_t size) noexcept;
KIMIX_CORE_API void *reallocate_aligned(void *p, size_t size, size_t alignment) noexcept;
KIMIX_CORE_API void *reallocate_zeroed(void *p, size_t size) noexcept;
KIMIX_CORE_API void *reallocate_n(void *p, size_t count, size_t size) noexcept;
KIMIX_CORE_API void *expand(void *p, size_t size) noexcept;
KIMIX_CORE_API void deallocate(void *p) noexcept;
KIMIX_CORE_API void deallocate_aligned(void *p, size_t alignment) noexcept;
KIMIX_CORE_API void deallocate_size(void *p, size_t size) noexcept;
KIMIX_CORE_API size_t usable_size(const void *p) noexcept;
KIMIX_CORE_API size_t good_size(size_t size) noexcept;
KIMIX_CORE_API char *duplicate_string(const char *s) noexcept;
KIMIX_CORE_API char *duplicate_string_n(const char *s, size_t n) noexcept;

// ---------------------------------------------------------------------------
// Diagnostics of the optional tracker (declared in every build)
// ---------------------------------------------------------------------------
//
// Untracked builds answer "no" / 0 and report nothing, so a caller never needs
// an #ifdef — assert a DELTA around your own allocate/free pair instead of an
// absolute count (other threads and fibers share the map).

KIMIX_CORE_API bool memory_tracking_enabled() noexcept; // true iff debug + KIMIX_MEMORY_STRICT
KIMIX_CORE_API size_t live_allocation_count() noexcept; // 0 when untracked
KIMIX_CORE_API size_t live_allocation_bytes() noexcept; // 0 when untracked
KIMIX_CORE_API void memory_leak_report(std::FILE *out); // same text the exit report prints

// ---------------------------------------------------------------------------
// memory_mark_permanent — declare a block process-lifetime on purpose
// ---------------------------------------------------------------------------
//
// The exit report lists every block that is still recorded when the library
// unloads, and some blocks are meant to outlive everything: the marl scheduler
// pool (see core/fiber.cpp) is created on first use and deliberately never
// destroyed.  Such a block is not a leak, but a report that keeps listing it
// is noise, and noise is how a real leak gets ignored.
//
// Call this right after creating the block.  It stays tracked (a later
// deallocate is not reported as an untracked pointer, and the live counters
// keep counting it) but it is left out of the exit report and of
// memory_leak_report().  Only for singletons whose lifetime IS the process —
// a real leak hidden behind this call becomes invisible to the tracker.
KIMIX_CORE_API void memory_mark_permanent(void *p) noexcept;

// ---------------------------------------------------------------------------
// IOperatorNewBase — per-class new/delete through the gateway
// ---------------------------------------------------------------------------
//
// Stateless base: derived objects never touch the CRT heap, so they are freed
// by the same heap that handed them out and are visible to the leak tracker.
// The placement forms stay no-ops (they are what the compiler calls for
// `new (place) T` and for the exception-less new expression).

struct IOperatorNewBase {
    static void *operator new(size_t size) noexcept { return kimix::allocate(size); }
    static void *operator new(size_t, void *place) noexcept { return place; }
    static void *operator new[](size_t size) noexcept { return kimix::allocate(size); }
    static void *operator new(size_t size, const std::nothrow_t &) noexcept {
        return kimix::allocate(size);
    }
    static void *operator new(size_t, void *place, const std::nothrow_t &) noexcept {
        return place;
    }
    static void *operator new[](size_t size, const std::nothrow_t &) noexcept {
        return kimix::allocate(size);
    }
    static void operator delete(void *pdead) noexcept { kimix::deallocate(pdead); }
    static void operator delete(void *ptr, void *place) noexcept {
        // do nothing
    }
    static void operator delete[](void *pdead) noexcept { kimix::deallocate(pdead); }
    static void operator delete(void *pdead, size_t) noexcept { kimix::deallocate(pdead); }
    static void operator delete[](void *pdead, size_t) noexcept { kimix::deallocate(pdead); }
};

} // namespace kimix

// ---------------------------------------------------------------------------
// Global one-line forwarders (literal spelling of the requirement)
// ---------------------------------------------------------------------------
//
// kimix::allocate / kimix::deallocate are the primary names; these exist so a
// C-shaped call site can spell the same gateway without the namespace.

inline void *kimix_allocate(size_t size) noexcept { return kimix::allocate(size); }
inline void kimix_deallocate(void *p) noexcept { kimix::deallocate(p); }
