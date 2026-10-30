/*
 * memory.cpp — implementation of the kimix::allocate / kimix::deallocate
 * gateway declared in core/memory.h, plus the optional leak tracker it can
 * carry.
 *
 * Scope: this is the ONLY translation unit outside src/ext and
 * api/ffi_mem.cpp that is allowed to call the mimalloc allocation family
 * (mi_malloc, mi_free, ...) — everything else reaches it through the gateway.
 * The functions themselves are thin forwards (one call each) and keep
 * mimalloc's own semantics, including "returns nullptr on failure": on this
 * build MI_XMALLOC=1 turns a true OOM into an abort inside mimalloc, so the
 * nullptr path is what a replaced error handler (mi_register_error) can still
 * observe (documented in api/ffi_mem.h).
 *
 * STRICT TRACKING (KIMIX_MEMORY_STRICT, debug builds only)
 *
 *   | build               | KIMIX_MEMORY_STRICT | what the gateway does      |
 *   |---------------------|---------------------|----------------------------|
 *   | debug (no NDEBUG)   | off (default)       | forward only, tracker gone |
 *   | debug (no NDEBUG)   | on                  | forward + per-block record |
 *   | release/releasedbg  | either              | forward only (+#pragma)    |
 *
 *   The tracker is a namespace-scope static, so its constructor runs at
 *   library load and its destructor at unload: the destructor reports the
 *   blocks that are still recorded to stderr (advisory, never fatal — a test
 *   binary's exit code is unaffected).
 *
 *   Why the map is NOT a kimix:: container: kimix::unordered_map / kimix::string
 *   / kimix::vector are allocator clients of the very function being tracked,
 *   so recording a block through them would re-enter track_alloc() forever.
 *   std::unordered_map with the default CRT allocator is used instead; mimalloc
 *   is MI_WIN_NOREDIRECT (src/ext/xmake.lua), so the CRT path never comes back
 *   into kimix::allocate.
 *
 *   Why std::mutex and not kimix::spin_mutex: the cpp skill's admission test
 *   for the atomic/spin locks excludes a section that allocates, and a hash-map
 *   insert allocates whenever it rehashes.
 *
 *   Static-initialization order: track_* early-return while `ready` is false,
 *   which covers allocations from another TU's static initializer (before this
 *   file's constructor ran) and frees from a destructor that runs after this
 *   file's destructor cleared it.
 *
 *   What the report means: the blocks listed are still LIVE when the library
 *   unloads, which is not the same as leaked -- a process-lifetime fixture, a
 *   lazily built singleton or a still-scoped large index is listed too. A
 *   small, stable count whose sizes never change is almost always a fixture;
 *   a count that grows with a new code path is the leak being hunted. The
 *   line that indicates a genuinely broken alloc/free pair is the
 *   "deallocate of an untracked or already-freed pointer" one, not this report.
 *
 *   No stack capture: kimix::backtrace() is an unimplemented stub, so a
 *   recorded return address would be a number nobody can symbolize. A record
 *   holds the pointer and the requested size.
 */
#include "memory.h"

#include <mimalloc.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

/* The option has no effect in a release-family build (NDEBUG is defined there).
 * The #pragma is the documented notice; note that this project compiles with
 * /w, which makes MSVC swallow "#pragma message" output -- so the reliable
 * proof of the guard is symbol absence: bin/<mode>/kimix-core.lib in a release
 * build contains neither MemoryTracker nor "leaked block", while the
 * memory_tracking_* functions stay (they answer false / 0 in every build). */
#if defined(KIMIX_MEMORY_STRICT) && defined(NDEBUG)
#pragma message("kimix: KIMIX_MEMORY_STRICT has no effect when NDEBUG is defined (release/releasedbg)")
#endif

namespace src_core_memory {

/* -------------------------------------------------------------------------
 * Leak tracker (debug + KIMIX_MEMORY_STRICT only).
 *
 * Flip the two conditions below from
 *   #if !defined(NDEBUG) && defined(KIMIX_MEMORY_STRICT)
 * to
 *   #if !defined(NDEBUG)
 * if the debug leak check should ever be on without the option; nothing else
 * changes, and memory_tracking_enabled() follows automatically.
 * ------------------------------------------------------------------------- */
#if !defined(NDEBUG) && defined(KIMIX_MEMORY_STRICT)

/* One live block. The requested size is recorded, not mi_usable_size(): the
 * report answers "who asked for what", and usable_size would inflate it. */
struct AllocationRecord {
    size_t size = 0;
};

/* At most this many leaked blocks are listed, then a "... N more" line: a
 * broken loop can leak a million blocks and a report has to stay readable. */
constexpr size_t kMaxReportedLeaks = 64;

struct MemoryTracker {
    std::mutex mutex;
    std::unordered_map<void *, AllocationRecord> live; // CRT allocator — see the header comment
    std::atomic<bool> ready{false};

    MemoryTracker() noexcept {
        ready.store(true, std::memory_order_release); // last: everything above is constructed
    }

    ~MemoryTracker() {
        ready.store(false, std::memory_order_release); // first: no new records from here on
        std::lock_guard<std::mutex> lock(mutex);       // a straggler mid-free must finish first
        report_locked(stderr);
    }

    /* Caller holds `mutex`. Only prints while non-empty, so a clean process
     * stays silent and a debug build's normal exit prints nothing. */
    void report_locked(std::FILE *out) const noexcept {
        if (live.empty()) {
            return;
        }
        size_t bytes = 0;
        for (const auto &entry : live) {
            bytes += entry.second.size;
        }
        std::fprintf(out,
                     "[kimix][error] memory: leak check: %zu allocation(s), %zu byte(s) never freed\n",
                     live.size(), bytes);
        size_t shown = 0;
        for (const auto &entry : live) {
            if (shown++ == kMaxReportedLeaks) {
                std::fprintf(out, "[kimix][error] memory:   ... %zu more\n", live.size() - kMaxReportedLeaks);
                break;
            }
            /* 0x%llx, not %p: %p has no "0x" prefix on MSVC, and the report is
             * documented as "leaked block 0x<ptr> size=<n>" (skills + tests). */
            std::fprintf(out, "[kimix][error] memory:   leaked block 0x%llx size=%zu\n",
                         static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(entry.first)),
                         entry.second.size);
        }
    }
};

/* Static storage: constructed at library load, destroyed at unload (that is
 * where the report happens). */
static MemoryTracker memory_tracker;

/* Record a block handed out by the gateway. A nullptr result is not a block. */
void track_alloc(void *p, size_t size) noexcept {
    if (p == nullptr || !memory_tracker.ready.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(memory_tracker.mutex);
    memory_tracker.live.insert_or_assign(p, AllocationRecord{size}); // reassign: a reused address replaces the stale record
}

/* Forget a block the caller is releasing. A pointer that was never recorded
 * (or was already released) is still handed to mi_free — behaviour is
 * preserved — but it is reported: it means one side of an alloc/free pair
 * bypassed the gateway, which is exactly the invariant the tracker relies on. */
void track_free(void *p) noexcept {
    if (p == nullptr || !memory_tracker.ready.load(std::memory_order_acquire)) {
        return;
    }
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(memory_tracker.mutex);
        found = memory_tracker.live.erase(p) != 0;
    }
    if (!found) {
        std::fprintf(stderr,
                     "[kimix][error] memory: deallocate of an untracked or already-freed pointer 0x%llx\n",
                     static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p)));
    }
}

/* Re-allocation: `old_p` (when there was one) is consumed and `new_p` (when
 * non-null) becomes the live block. A nullptr result from the realloc family
 * leaves the original block alive, so its record must survive untouched. The
 * in-place case (new_p == old_p) is a plain size update. */
void *track_realloc(void *old_p, void *new_p, size_t size) noexcept {
    if (new_p == nullptr || !memory_tracker.ready.load(std::memory_order_acquire)) {
        return new_p;
    }
    std::lock_guard<std::mutex> lock(memory_tracker.mutex);
    if (old_p != nullptr) {
        memory_tracker.live.erase(old_p);
    }
    memory_tracker.live.insert_or_assign(new_p, AllocationRecord{size});
    return new_p;
}

size_t live_count() noexcept {
    if (!memory_tracker.ready.load(std::memory_order_acquire)) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(memory_tracker.mutex);
    return memory_tracker.live.size();
}

size_t live_bytes() noexcept {
    if (!memory_tracker.ready.load(std::memory_order_acquire)) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(memory_tracker.mutex);
    size_t bytes = 0;
    for (const auto &entry : memory_tracker.live) {
        bytes += entry.second.size;
    }
    return bytes;
}

void leak_report(std::FILE *out) noexcept {
    if (out == nullptr || !memory_tracker.ready.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(memory_tracker.mutex);
    memory_tracker.report_locked(out);
}

#else // NDEBUG or no KIMIX_MEMORY_STRICT — the tracker does not exist at all.

/* Same shape, zero cost: every gateway function below reads like the tracked
 * build without a single #if inside it. */
inline void track_alloc(void *, size_t) noexcept {}
inline void track_free(void *) noexcept {}
inline void *track_realloc(void *, void *new_p, size_t) noexcept {
    return new_p;
}
inline size_t live_count() noexcept {
    return 0;
}
inline size_t live_bytes() noexcept {
    return 0;
}
inline void leak_report(std::FILE *) noexcept {}

#endif

} // namespace src_core_memory
using namespace src_core_memory;

namespace kimix {

// ---------------------------------------------------------------------------
// Plain allocation
// ---------------------------------------------------------------------------

void *allocate(size_t size) noexcept {
    void *const p = mi_malloc(size);
    track_alloc(p, size);
    return p;
}

void *allocate_zeroed(size_t size) noexcept {
    void *const p = mi_zalloc(size);
    track_alloc(p, size);
    return p;
}

void *allocate_n(size_t count, size_t size) noexcept {
    void *const p = mi_calloc(count, size); // count first, like calloc
    track_alloc(p, count * size);
    return p;
}

// ---------------------------------------------------------------------------
// Aligned allocation — mimalloc's argument order is (size, alignment), NOT the
// (alignment, size) of posix_memalign / aligned_alloc.
// ---------------------------------------------------------------------------

void *allocate_aligned(size_t size, size_t alignment) noexcept {
    void *const p = mi_malloc_aligned(size, alignment);
    track_alloc(p, size);
    return p;
}

void *allocate_aligned_at(size_t size, size_t alignment, size_t offset) noexcept {
    void *const p = mi_malloc_aligned_at(size, alignment, offset);
    track_alloc(p, size);
    return p;
}

void *allocate_zeroed_aligned(size_t size, size_t alignment) noexcept {
    void *const p = mi_zalloc_aligned(size, alignment);
    track_alloc(p, size);
    return p;
}

void *allocate_n_aligned(size_t count, size_t size, size_t alignment) noexcept {
    void *const p = mi_calloc_aligned(count, size, alignment);
    track_alloc(p, count * size);
    return p;
}

// ---------------------------------------------------------------------------
// Re-allocation. nullptr return == "the original block is still yours".
// ---------------------------------------------------------------------------

void *reallocate(void *p, size_t size) noexcept {
    return track_realloc(p, mi_realloc(p, size), size);
}

void *reallocate_aligned(void *p, size_t size, size_t alignment) noexcept {
    return track_realloc(p, mi_realloc_aligned(p, size, alignment), size);
}

void *reallocate_zeroed(void *p, size_t size) noexcept {
    return track_realloc(p, mi_rezalloc(p, size), size);
}

void *reallocate_n(void *p, size_t count, size_t size) noexcept {
    return track_realloc(p, mi_recalloc(p, count, size), count * size);
}

void *expand(void *p, size_t size) noexcept {
    /* mi_expand is "in place or nothing": on success it returns `p` itself, on
     * failure nullptr with `p` untouched. The difference is only checked so a
     * future mimalloc that ever moved the block would keep the record correct. */
    void *const r = mi_expand(p, size);
    if (r == nullptr) {
        return nullptr; // `p` and its record are untouched
    }
    return track_realloc(p, r, size); // same block (size update) or a moved one
}

// ---------------------------------------------------------------------------
// Release — deallocate(nullptr) is a documented no-op in every form.
// ---------------------------------------------------------------------------

void deallocate(void *p) noexcept {
    if (p != nullptr) {
        track_free(p);
        mi_free(p);
    }
}

void deallocate_aligned(void *p, size_t alignment) noexcept {
    /* The alignment is the one the block was allocated with: release builds
     * ignore it (debug builds assert it), so it is kept for self-documentation
     * and for that check. */
    if (p != nullptr) {
        track_free(p);
        mi_free_aligned(p, alignment);
    }
}

void deallocate_size(void *p, size_t size) noexcept {
    /* `size` is the original request; it only routes to the small-block fast
     * path, so it must be the size the block was allocated with. */
    if (p != nullptr) {
        track_free(p);
        mi_free_size(p, size);
    }
}

// ---------------------------------------------------------------------------
// Size queries
// ---------------------------------------------------------------------------

size_t usable_size(const void *p) noexcept {
    return p != nullptr ? mi_usable_size(p) : 0;
}

size_t good_size(size_t size) noexcept {
    return mi_good_size(size);
}

// ---------------------------------------------------------------------------
// String duplication (C-string copies on the gateway heap)
// ---------------------------------------------------------------------------

char *duplicate_string(const char *s) noexcept {
    if (s == nullptr) {
        return nullptr;
    }
    char *const p = mi_strdup(s);
    track_alloc(p, std::strlen(s) + 1);
    return p;
}

char *duplicate_string_n(const char *s, size_t n) noexcept {
    if (s == nullptr) {
        return nullptr;
    }
    char *const p = mi_strndup(s, n);
    track_alloc(p, n + 1); // mi_strndup always NUL-terminates
    return p;
}

// ---------------------------------------------------------------------------
// Tracker diagnostics (declared in every build; no-ops when untracked)
// ---------------------------------------------------------------------------

bool memory_tracking_enabled() noexcept {
#if !defined(NDEBUG) && defined(KIMIX_MEMORY_STRICT)
    return true;
#else
    return false;
#endif
}

size_t live_allocation_count() noexcept {
    return live_count();
}

size_t live_allocation_bytes() noexcept {
    return live_bytes();
}

void memory_leak_report(std::FILE *out) {
    leak_report(out);
}

} // namespace kimix
