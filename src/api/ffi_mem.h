/*
 * ffi_mem.h -- the vendored mimalloc allocator (src/ext/mimalloc,
 * include/mimalloc.h, version 3.5.2) exported as a stable `kimix_mem_*` C surface.
 *
 * WHY THIS AREA EXISTS
 * --------------------
 * Contract rule 4 of api/ffi_common.h: ONE heap.  Every buffer the library hands
 * to a caller is a mimalloc block that lives INSIDE the kimix_api image -- the
 * JSON writer's output string (api/ffi_yyjson.h), the placeholder storage of
 * kimix_vec_new() (api/ffi_vec.h), and anything else any area allocates.  The
 * caller's own module cannot release those with its own malloc/free, so the
 * matching deallocator has to be part of this library: kimix_mem_free().  This
 * header therefore exposes mimalloc's whole plain-allocation vocabulary under
 * stable kimix_mem_* names, so a binding can allocate its own buffers on the
 * very same heap the library uses, hand them to the other areas, and free
 * everything through one family of entry points.
 *
 * HOW THE WIRING WORKS (and why no mi_* name appears in this header)
 * ------------------------------------------------------------------
 * The mimalloc objects are compiled into this very DLL: the "mimalloc" xmake
 * target propagates MI_SHARED_LIB / MI_SHARED_LIB_EXPORT PUBLICLY (see
 * src/ext/xmake.lua), so `mi_decl_export` resolves to __declspec(dllexport) on
 * MSVC/clang-cl and to default visibility on ELF, and the mi_* definitions are
 * part of kimix_api.  If a public header re-declared (or pulled in by including
 * <mimalloc.h>) those prototypes, every consumer would compile the same symbol
 * twice with mismatched DLL linkage -- the MSVC linker reports that as LNK4273
 * "inconsistent DLL linkage", and a dllimport-vs-dllexport clash is worse than
 * a warning: the call would jump through the wrong thunk.  Hence the rules:
 *   - this header is PURE C and declares only kimix_mem_* functions;
 *   - api/ffi_mem.cpp is the only translation unit of this area that includes
 *     <mimalloc.h> and forwards to the mi_* functions;
 *   - no other src/api header may ever name a mi_* symbol.
 *
 * FAILURE MODE -- READ THIS BEFORE TRUSTING A NULL CHECK
 * ------------------------------------------------------
 * Every allocation entry point below returns NULL on failure; that is the ABI
 * contract generated bindings rely on.  However this build also defines
 * MI_XMALLOC=1 (src/ext/xmake.lua): mimalloc's default error handler then maps
 * ENOMEM/EINVAL/EOVERFLOW to abort() (src/ext/mimalloc/src/options.c,
 * mi_error_default) instead of letting the allocation return NULL.  On the
 * shipped library a real out-of-memory therefore terminates the process rather
 * than producing a NULL -- unless the host replaced the handler through
 * mi_register_error(), or a future build drops MI_XMALLOC.  Check for NULL
 * anyway: it is cheap, it is the portable contract, and it is the only outcome
 * this header promises.
 *
 * SIZE 0
 * ------
 * mimalloc does NOT return NULL for a zero-sized request: mi_malloc(0) hands
 * out a fresh minimum-size block (never dereference it, but it must be freed),
 * and mi_realloc(p, 0) shrinks to such a zero-sized block instead of the C
 * runtime's "free(p) and return NULL".  Each entry point below documents the
 * exact zero-size behaviour of the mi_* function it maps to.
 *
 * THREADING
 * ---------
 * mimalloc attaches a heap to every thread that allocates through it; the
 * allocation itself needs no lock, which is why these calls are as re-entrant
 * as malloc (contract rule 6).  A block may be freed from ANY thread: the page
 * is abandoned and reclaimed by a collection pass.  A foreign thread that lives
 * longer than a few allocations should register and de-register itself -- see
 * section 6 -- and may ask for collection explicitly.
 */
#pragma once
#ifndef KIMIX_API_FFI_MEM_H
#define KIMIX_API_FFI_MEM_H

#include <api/ffi_common.h>

KIMIX_FFI_BEGIN

/* ===========================================================================
 * 1. Core allocation (the malloc family)
 *
 * Ownership for the whole section: a returned block belongs to the library
 * heap and must be released with kimix_mem_free() (or kimix_mem_free_size()/
 * kimix_mem_free_aligned() below) -- never with the caller's own free().
 * Failure mode for the whole section: NULL (see the MI_XMALLOC note at the top
 * of this header for what "failure" means in this build).
 * ======================================================================== */

/* Allocates `size` uninitialised bytes.  Maps to mi_malloc(size).
 * Free the result with kimix_mem_free().  size 0: mimalloc returns a fresh
 * non-NULL minimum block (not dereferenceable, still freeable). */
KIMIX_FFI void *kimix_mem_malloc(size_t size);

/* Allocates a zero-filled array of `count` elements of `size` bytes.  Maps to
 * mi_calloc(count, size) -- SAME argument order, count first.  An overflow of
 * count * size fails like an out-of-memory (NULL, never a partial buffer).
 * Free with kimix_mem_free(). */
KIMIX_FFI void *kimix_mem_calloc(size_t count, size_t size);

/* Allocates `size` bytes filled with zero.  Maps to mi_zalloc(size).
 * Free with kimix_mem_free().  size 0 behaves like kimix_mem_malloc(0). */
KIMIX_FFI void *kimix_mem_zalloc(size_t size);

/* Re-sizes a block: maps to mi_realloc(p, newsize) -- the (p, size) order of
 * mimalloc/C.  `p` may be NULL (then it behaves like kimix_mem_malloc).  On
 * success the old block is consumed by the library heap and the returned
 * pointer (which may differ from `p`) owns the buffer; on failure `p` is
 * UNTOUCHED and still owned by the caller.  newsize 0 shrinks to a zero-sized
 * block and returns non-NULL -- unlike the C runtime, it does NOT free-and-
 * return-NULL.  Free the result with kimix_mem_free(). */
KIMIX_FFI void *kimix_mem_realloc(KIMIX_IN_OUT KIMIX_NULLABLE void *p, size_t newsize);

/* In-place resize: maps to mi_expand(p, newsize) -- the same (p, size) order.
 * Unlike kimix_mem_realloc this NEVER copies and NEVER fails softly into a new
 * block: it returns `p` when the existing block's usable size already covers
 * `newsize` (grow or shrink), and NULL when it does not -- with `p` left
 * completely intact and its old content still valid, so the caller can fall
   * back to kimix_mem_realloc.  `p` NULL returns NULL.  Ownership NEVER moves:
   * the same block belongs to the caller before and after the call -- whether the
   * request grew, shrank (it returned `p` with the usable size unchanged) or failed
   * (it returned NULL) -- and is still released with kimix_mem_free(). */
KIMIX_FFI void *kimix_mem_expand(KIMIX_IN_OUT KIMIX_NULLABLE void *p, size_t newsize);

/* Releases a block to the library heap.  Maps to mi_free(p).  Accepts any
 * pointer returned by this area or by another area of this library (JSON
 * writer strings, kimix_vec_new() storage, ...).  NULL is ignored (mimalloc's
 * own no-op, guarded here as well).  A wrong-heap pointer (the caller's own
 * malloc, or a block from another build of this library) is UB -- mimalloc
 * validates the page pointer and, in secure/debug builds, reports it; the
 * release build does not promise to catch it. */
KIMIX_FFI void kimix_mem_free(KIMIX_TRANSFER KIMIX_NULLABLE void *p);

/* Copies a NUL-terminated UTF-8 string onto the library heap.  Maps to
 * mi_strdup(s).  `s` NULL returns NULL (mimalloc does the same; guarded
 * explicitly).  The copy is owned by the caller and freed with
 * kimix_mem_free(); NULL on allocation failure. */
KIMIX_FFI char *kimix_mem_strdup(KIMIX_IN KIMIX_NULLABLE const char *s);

/* Copies at most `n` bytes of `s` and always NUL-terminates the copy -- maps
 * to mi_strndup(s, n) -- the C strndup semantics: with strlen(s) < n only
 * strlen(s) + 1 bytes are allocated.  `s` NULL returns NULL.  Free with
 * kimix_mem_free(); NULL on allocation failure or an over-long string. */
KIMIX_FFI char *kimix_mem_strndup(KIMIX_IN KIMIX_NULLABLE const char *s, size_t n);

/* ===========================================================================
 * 2. Aligned allocation
 *
 * NOTE THE ARGUMENT ORDER: mimalloc takes `size` FIRST and `alignment` after
 * ("alignment always follows `size` for consistency with the unaligned calls",
 * mimalloc.h) -- the opposite of posix_memalign/aligned_alloc.  The kimix_mem_*
 * signatures keep mimalloc's order so every mapping below is 1:1; the one
 * posix-shaped entry point of this area is kimix_mem_posix_memalign() in
 * section 5.  `alignment` must be a positive power of two; mimalloc's release
 * build does NOT diagnose a non-power-of-two alignment (its debug build
 * asserts), so validating it is the caller's job.
 *
 * Alignment is a property of the BLOCK, not of a different heap: all of these
 * are freed with kimix_mem_free() like any other block (kimix_mem_free_aligned
 * merely passes the alignment for debug validation).
 * ======================================================================== */

/* Aligned block: maps to mi_malloc_aligned(size, alignment).  The address
 * satisfies `alignment`.  Ownership/failure as in section 1; free with
 * kimix_mem_free(). */
KIMIX_FFI void *kimix_mem_malloc_aligned(size_t size, size_t alignment);

/* Offset-aligned block: maps to mi_malloc_aligned_at(size, alignment, offset).
 * The returned address `a` satisfies `a % alignment == offset % alignment`.
 * Free with kimix_mem_free(). */
KIMIX_FFI void *kimix_mem_malloc_aligned_at(size_t size, size_t alignment, size_t offset);

/* Aligned + zero-filled: maps to mi_zalloc_aligned(size, alignment). */
KIMIX_FFI void *kimix_mem_zalloc_aligned(size_t size, size_t alignment);

/* Aligned zero-filled array: maps to mi_calloc_aligned(count, size,
 * alignment) -- count, then element size, then alignment. */
KIMIX_FFI void *kimix_mem_calloc_aligned(size_t count, size_t size, size_t alignment);

/* Aligned resize: maps to mi_realloc_aligned(p, newsize, alignment).  `p` may
 * be NULL (behaves like kimix_mem_malloc_aligned); the old block is consumed
 * on success, left intact on failure (NULL returned).  Free the result with
 * kimix_mem_free(). */
KIMIX_FFI void *kimix_mem_realloc_aligned(KIMIX_IN_OUT KIMIX_NULLABLE void *p, size_t newsize, size_t alignment);

/* Aligned free: maps to mi_free_aligned(p, alignment) -- `alignment` must be
 * the one the block was allocated with; in mimalloc's release builds the
 * argument only feeds a debug assertion and the block is freed exactly like
 * kimix_mem_free() would, so this is documentation made executable.  NULL is
 * ignored (guarded; mi_free itself is a NULL no-op). */
KIMIX_FFI void kimix_mem_free_aligned(KIMIX_TRANSFER KIMIX_NULLABLE void *p, size_t alignment);

/* Size-aware free: maps to mi_free_size(p, size) -- `size` must be the size
 * originally requested from the allocating call; mimalloc uses it to pick the
 * small-block fast path (mi_free_small) instead of re-deriving the page, a
 * hint only: a wrong-but-plausible size does not corrupt anything in release
 * builds (the debug build reports mismatches).  NULL is ignored (guarded). */
KIMIX_FFI void kimix_mem_free_size(KIMIX_TRANSFER KIMIX_NULLABLE void *p, size_t size);

/* ===========================================================================
 * 3. Size queries
 * ======================================================================== */

/* The real usable size of a block: maps to mi_usable_size(p).  This is the
 * number of bytes that may be written to `p` (>= the requested size); the
 * standard way to discover what mi_expand()/kimix_mem_expand() can grow into.
 * `p` NULL returns 0 (guarded here; mimalloc treats NULL as 0 as well).  A
 * pointer that is not a live block of this heap is UB. */
KIMIX_FFI size_t kimix_mem_usable_size(KIMIX_IN KIMIX_NULLABLE const void *p);

/* The block size mimalloc would hand out for `size`: maps to
 * mi_good_size(size) -- round a request up to a size class to allocate in
 * batches without per-block padding surprises.  Never fails. */
KIMIX_FFI size_t kimix_mem_good_size(size_t size);

/* ===========================================================================
 * 4. Zero-initialising re-allocation
 *
 * From mimalloc.h's "Zero initialized re-allocation" section: these are only
 * valid on blocks that were ORIGINALLY allocated zero-initialised (any of
 * calloc / zalloc / *_aligned zero variants above, and results of these calls
 * themselves).  Resizing memory that was never zero-initialised is a
 * programming error; mimalloc does not diagnose it in release builds.
 * ======================================================================== */

/* Rezalloc: maps to mi_rezalloc(p, newsize) -- like kimix_mem_realloc, but the
 * newly exposed tail bytes are zeroed (and the whole block stays zero-valid).
 * `p` may be NULL (acts as zalloc).  Old block consumed on success, kept on
 * failure (NULL).  Free with kimix_mem_free(). */
KIMIX_FFI void *kimix_mem_rezalloc(KIMIX_IN_OUT KIMIX_NULLABLE void *p, size_t newsize);

/* Recalloc: maps to mi_recalloc(p, newcount, size) -- the calloc-shaped
 * rezalloc: resize the array to `newcount` elements of `size` bytes, zeroing
 * the grown tail.  Argument order: p, newcount, size.  Same rules and same
 * free as kimix_mem_rezalloc. */
KIMIX_FFI void *kimix_mem_recalloc(KIMIX_IN_OUT KIMIX_NULLABLE void *p, size_t newcount, size_t size);

/* ===========================================================================
 * 5. posix-shaped aligned allocation with kimix_status reporting
 * ======================================================================== */

/* posix_memalign with this library's error vocabulary.  Argument checks FIRST
 * (mimalloc's own mi_posix_memalign, mimalloc.h "posix" section, would return
 * EINVAL, and under this build's MI_XMALLOC=1 any error that reaches
 * mimalloc's error handler aborts the process -- so a bad request is rejected
 * here before mimalloc is entered):
 *   - KIMIX_ERR_INVALID_ARG  when `p` is NULL, or `alignment` is not a power
 *     of two that is >= sizeof(void*) (the posix rule);
 *   - KIMIX_ERR_OUT_OF_MEMORY when the aligned allocation fails -- `*p` is
 *     left exactly untouched (the posix contract, and mimalloc follows it);
 *   - KIMIX_OK               writes the aligned block into `*p`.
 * The block is owned by the caller and freed with kimix_mem_free().
 * Forwarding target: mi_posix_memalign(p, alignment, size) -- note the posix
 * order (p, alignment, size), the ONLY call in this header whose argument
 * order does NOT follow mimalloc's size-first rule, because it mirrors the
 * posix prototype 1:1. */
KIMIX_FFI kimix_status kimix_mem_posix_memalign(KIMIX_OUT void **p, size_t alignment, size_t size);

/* ===========================================================================
 * 6. Thread and heap management for foreign threads
 *
 * WHY an FFI caller needs these: mimalloc keeps a heap per thread.  A thread
 * that allocates through this library gets its heap registered in the process
 * list automatically, but the registration is only released when the thread's
 * life ends through mimalloc's own TLS/destructor path.  A binding driving
 * FOREIGN threads (a host threadpool, a scripting runtime, GC threads) that
 * the library never created can -- and should -- bracket each thread's use of
 * the heap explicitly:
 *     kimix_mem_thread_init();   // once, before the first allocation
 *     ... allocate/free via this area ...
 *     kimix_mem_thread_done();   // once, before the thread exits
 * so the thread's heap is returned eagerly instead of being abandoned for a
 * later collection.  Blocks still owned by that heap are NOT freed by
 * thread_done: they stay valid and freeable from any thread until collected.
 * Freeing a block FROM another thread remains legal at all times.
 * ======================================================================== */

/* Registers a mimalloc heap for the calling thread.  Maps to
 * mi_thread_init().  mimalloc calls it implicitly on first use, so this is
 * only needed to pre-register (e.g. to avoid a first-allocation hiccup) or to
 * re-register after a kimix_mem_thread_done() on the same thread.  Never
 * fails; calling it twice on one thread is harmless. */
KIMIX_FFI void kimix_mem_thread_init(void);

/* Releases the calling thread's heap registration.  Maps to
 * mi_thread_done().  Pair it with kimix_mem_thread_init() before a foreign
 * thread exits.  Still-live blocks remain valid and may be freed from any
 * thread until the pages are collected; the thread may keep using this
 * library afterwards (its next allocation re-initialises implicitly, but
 * explicit re-init is the clean way).  Never fails. */
KIMIX_FFI void kimix_mem_thread_done(void);

/* Asks mimalloc to return cached/abandoned memory to the OS now.  Maps to
 * mi_collect(force).  `force == true` performs a full (slower) collection
 * including the thread's heap; false is a cheap opportunistic pass.  Safe
 * from any thread; never fails.  Useful after freeing a large burst of
 * buffers from foreign threads that never ran kimix_mem_thread_done(). */
KIMIX_FFI void kimix_mem_collect(bool force);

/* ===========================================================================
 * 7. Identity
 * ======================================================================== */

/* The mimalloc feature/version number this library was built with: the
 * compile-time MI_MALLOC_VERSION macro of the vendored mimalloc.h, returned
 * unchanged.  Encoding (mimalloc's own comment): major * 10000 + minor * 100
 * + patch -- two decimal digits for minor and patch each -- e.g. 30502 is
 * mimalloc 3.5.2.  Because the mi_* code is compiled INTO this DLL there is
 * no version skew: this is the version of the exact heap the caller's buffers
 * live in.  Use it to assert once at load time; kimix_mem_version_string()
 * gives the human form.  Never fails. */
KIMIX_FFI uint32_t kimix_mem_version(void);

/* The same version as text: major.minor.patch WITHOUT leading zeros --
 * "3.5.2" for 30502 -- built at compile time in ffi_mem.cpp from the decimal
 * decomposition of MI_MALLOC_VERSION (the vendored header provides no
 * MI_MALLOC_VERSION_MAJOR/MINOR/PATCH macros, so the components are computed
 * from the single integer; nothing is hard-coded that could rot).  The pointer
 * is to static storage owned by the library, valid for the life of the
 * process: KIMIX_BORROWED, never free it.  Never NULL, never fails. */
KIMIX_FFI const char *kimix_mem_version_string(void);

/* True when mimalloc has taken over the process-wide allocator -- i.e. the
 * libc malloc/free (or the Windows heap) in the host process are redirected
 * into the same heap as this area.  Maps to mi_is_redirected().  On Windows
 * this library is built with MI_WIN_NOREDIRECT (src/ext/xmake.lua), so the
 * normal answer is FALSE: the host's C runtime keeps its allocator and only
 * memory obtained through kimix_mem_* / the library itself is mimalloc.  On
 * platforms/links where the override is compiled in, it may return true, in
 * which case even the caller's own malloc traffic shares this heap.  Never
 * fails; `bool` is one byte on both sides of the boundary (contract rule 3). */
KIMIX_FFI bool kimix_mem_is_redirected(void);

KIMIX_FFI_END

#endif /* KIMIX_API_FFI_MEM_H */
