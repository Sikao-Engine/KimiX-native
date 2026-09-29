# `kimix_api` — the C FFI shared library

`kimix_api` is a shared library (`src/api`) that puts a **plain C ABI** on top of
`kimix-core`, so a caller in C, C#, Zig, Rust, Python (ctypes/cffi), Julia,
Node `ffi-napi` or any other FFI host can use the kimix base facilities without
C++ or pybind11 in the picture. It exports four feature areas plus a small common
header: the version/ABI/status/layout queries of
`kimix_api_*` / `kimix_status_name`, the mimalloc allocator vocabulary
(`kimix_mem_*`), the byte-buffer container `kimix::vector<std::byte>` as an inline
caller-owned object (`kimix_vec_*`), the yyjson JSON read/build/write surface with
the mimalloc allocator baked in (`kimix_yyjson_*`), and `kimix::repair()` for
malformed LLM tool-call JSON (`kimix_repair*` / `kimix_json_*`). Every entry point
is declared in a pure-C header under `src/api`; this document is the searchable
index of all of them.

## Getting the library

| Item | Value |
|---|---|
| Build target | `kimix_api` (`set_kind("shared")` in `src/api/xmake.lua`) |
| Build command | `xmake build kimix_api` (or `python bootstrap.py`) |
| Output | `bin/<mode>/kimix_api.dll` + `kimix_api.lib` on Windows, `bin/<mode>/libkimix_api.so` on Linux (`<mode>` = `debug` / `release`) |
| Umbrella header | `#include <api/kimix_api.h>` — include dir is `src/` |
| Area headers | `<api/ffi_common.h>`, `<api/ffi_mem.h>`, `<api/ffi_vec.h>`, `<api/ffi_yyjson.h>`, `<api/ffi_repair.h>` |
| Dependencies | `kimix-core` only; mimalloc and yyjson arrive through it and are compiled **into** this image |

The headers are valid for a **C99/C11 compiler and for C++17 or newer**: opaque
pointer typedefs, fixed-size POD structs, `size_t`, `<stdint.h>` scalars, `bool`
and `const char *` — no templates, no namespaces, no references, no overloads.

Linkage is decided by `KIMIX_FFI` (= `KIMIX_API_API`, from
`<core/dll_export.h>`), which has three states:

| What you are doing | Macro to define | `KIMIX_FFI` expands to |
|---|---|---|
| Building `kimix_api` itself | `KIMIX_API_EXPORT_DLL` (the target does this in `on_load`) | `__declspec(dllexport)` / `visibility("default")` |
| Linking the import library / the `.so` | nothing | `__declspec(dllimport)` / nothing |
| `dlopen()` / `LoadLibrary()` and resolving symbols yourself, or merging the objects statically | `KIMIX_API_STATIC` | nothing (plain `extern` declaration) |

A `dlopen` user therefore compiles the headers with `-DKIMIX_API_STATIC` and
resolves the names against the loaded image; a link-time user needs no define at
all. The library also propagates `KIMIX_CORE_STATIC` (and `NOMINMAX`,
`_CRT_SECURE_NO_WARNINGS` on Windows) publicly, because the core code inside
this DLL is the static copy.

Every header is pure 7-bit ASCII with no BOM, so any C or C++ compiler can read
it with its default settings - no `/utf-8`, no execution-charset flag, no
`C4819`. (The library's own `.c`/`.cpp` sources are also ASCII.)

The `KIMIX_IN` / `KIMIX_IN_OUT` / `KIMIX_OUT` / `KIMIX_TRANSFER` /
`KIMIX_BORROWED` / `KIMIX_NULLABLE` / `KIMIX_NOTNULL` tokens in front of
parameters **expand to nothing**: they are contract keywords that stay readable
in the declarations. They are reproduced in the signatures below for that
reason; a binding ignores them and reads the `Ownership:` line instead.

## Rules that apply everywhere

**One heap, and the matching free.** All memory a caller receives comes from the
single mimalloc heap inside this library. Freeing it with the caller module's
`malloc`/`free` (or with a `kimix_mem_free` resolved out of a *different* copy of
this library) is undefined behaviour. The pairing is fixed:

| What you got | From | Release it with |
|---|---|---|
| a raw block | `kimix_mem_malloc` / `_calloc` / `_zalloc` / `_realloc` / `_expand` / `*_aligned` / `_rezalloc` / `_recalloc` / `_strdup` / `_strndup` / `_posix_memalign` | `kimix_mem_free` (or `kimix_mem_free_size` / `kimix_mem_free_aligned` as documented) |
| any memory, in place | `kimix_mem_copy` / `_move` / `_swap` / `_set` / `_zero` / `_compare` / `_equals` / `_find_byte` / `_count_byte` | nothing — these operate in place, allocate nothing, transfer nothing |
| a `kimix_vec` you embedded/stack-allocated | `kimix_vec_default_init` / `_init_from` / `_copy_init` / `_move_init` | `kimix_vec_destroy` |
| a `kimix_vec *` handle | `kimix_vec_new` / `kimix_vec_new_from` / `kimix_repair_new` | `kimix_vec_free` |
| a JSON output string | `kimix_yyjson_write` / `_val_write` / `_mut_write` / `_mut_val_write` | `kimix_yyjson_str_free` |
| an immutable document | `kimix_yyjson_read` / `_read_str` / `_mut_doc_imut_copy` | `kimix_yyjson_doc_free` |
| a mutable document | `kimix_yyjson_mut_doc_new` / `kimix_yyjson_doc_mut_copy` / `kimix_yyjson_mut_doc_mut_copy` | `kimix_yyjson_mut_doc_free` |
| a JSON value (`val` / `mut_val`) | created by or looked up in a document | never on its own — it dies with its document |
| a `const char *` from a `_version_string` / `_build_info` / `_status_name` / `_type_desc` / `_get_str` call | static storage or a borrowed document buffer | never — do not free |

**No exceptions, no RTTI across the boundary.** The project is built with
`kimix_enable_exception=false` and `kimix_rtti=false`; every failure is a return
value and out-parameters are written only on success.

**Status codes.** Every fallible call returns `kimix_status` (a plain C `enum`,
so `int` wide), with `KIMIX_OK == 0` so `if (kimix_...(...))` reads as a failure.
`kimix_status_name()` turns a code into its textual name.

| Code | Value | Meaning the headers give it |
|---|---|---|
| `KIMIX_OK` | 0 | Success. |
| `KIMIX_ERR_INVALID_ARG` | 1 | A pointer argument was NULL where `KIMIX_NOTNULL` is required, or a scalar argument was outside its accepted range (e.g. a length that overflows). |
| `KIMIX_ERR_OUT_OF_MEMORY` | 2 | mimalloc could not satisfy the request; the object is left exactly as it was before the call. |
| `KIMIX_ERR_OUT_OF_RANGE` | 3 | An index / range argument is outside `[0, size())`. |
| `KIMIX_ERR_INVALID_STATE` | 4 | The object is not usable: a caller-owned placeholder never initialised (or already destroyed), or an operation needing a non-empty buffer. Detected through the guard word, so use-after-destroy is reported instead of applied to dead memory. |
| `KIMIX_ERR_INVALID_INPUT` | 5 | Reserved - the bytes handed to a parser/decoder were not acceptable. No current entry point produces it: the JSON and repair areas report a bad input through their own out-parameters (a failed read, an empty repair result). |
| `KIMIX_ERR_NOT_FOUND` | 6 | Reserved - a lookup did not hit. The lookup entry points return the neutral value (NULL / 0 / false) instead. |
| `KIMIX_ERR_FAILED` | 7 | Reserved - the library could not comply for another reason. No current entry point produces it. |

Note that several areas return the *neutral value* instead of a status:
allocation functions return `NULL`, `kimix_vec_size()`/`kimix_vec_capacity()`
return `(size_t)-1`, and the JSON accessors return `false` / `0` / `NULL` for a
NULL or wrong-typed argument. A binding's error mapping should therefore test the
pointer/flag it got back first and the status second. As currently implemented,
`KIMIX_ERR_INVALID_ARG` and `KIMIX_ERR_INVALID_STATE` are what the status-returning
`kimix_vec_*`, `kimix_repair*` and `kimix_mem_posix_memalign` calls report,
`KIMIX_ERR_OUT_OF_RANGE` only comes back from the range-checked `kimix_vec_*`
accessors, and `KIMIX_ERR_OUT_OF_MEMORY` only from `kimix_mem_posix_memalign` (and
only if the host replaced mimalloc's aborting error handler — see the top of
`api/ffi_mem.h`), while `KIMIX_ERR_INVALID_INPUT`, `KIMIX_ERR_NOT_FOUND` and
`KIMIX_ERR_FAILED` are defined by the contract but returned by no entry point yet:
treat them as reserved.

**`bool` is one byte** (C99 `_Bool` / C++ `bool`, never `int`) on both sides of
the boundary; `sizeof(bool)` is also reported by `kimix_api_layout_info()`.

**Lengths are BYTES of UTF-8**, never code points. C strings are NUL-terminated
UTF-8; wherever a function has a `_str` / `_n` spelling, `_str` takes a
NUL-terminated string and the plain spelling takes `(ptr, len)`.

**Borrowed vs transferred.** `KIMIX_BORROWED` pointers stay valid only until the
owning object is freed or mutated; `KIMIX_TRANSFER` hands ownership to the callee
(or to the caller, for a returned pointer). The rules section above and each
entry's `Ownership:` line say which is which.

**Platforms.** x64 and arm64 only (the platforms this project builds): one
calling convention per OS, `size_t` == pointer width.

**Thread-safety (contract rule 6 of `ffi_common.h`).** Every function is
re-entrant and thread-safe as long as the objects it is given are not shared
concurrently — the same rule as plain `malloc` plus STL containers. mimalloc
keeps a heap per thread; a thread this library did not create may bracket its
use with `kimix_mem_thread_init()` / `kimix_mem_thread_done()`. Individual
entries below only add a note where they deviate from that general rule.

**The ABI guard.** `KIMIX_API_ABI_VERSION` (currently `1u`) is bumped whenever
the exported surface changes in a way a compiled binding must know about. A
binding should assert once at load time:

```c
#include <api/kimix_api.h>
#include <stdlib.h>          /* abort() */

static void assert_kimix_abi(void) {
    if (kimix_api_abi_version() != KIMIX_API_ABI_VERSION) abort();
    kimix_layout_info li;
    if (kimix_api_layout_info(&li) != KIMIX_OK) abort();
    if (li.size_of_size_t != sizeof(size_t) || li.size_of_pointer != sizeof(void *) ||
        li.size_of_bool != 1 || li.vec_abi_bytes != KIMIX_VEC_BYTES ||
        li.vec_abi_align != KIMIX_VEC_ALIGN) abort();
}
```

A binding that never embeds `kimix_vec` only needs the version check; one that
does embed it should check `vec_abi_bytes` / `vec_abi_align` as above and compare
`vec_inner_bytes` against its own expectation once, at load time.

`kimix_api_layout_info()` reports the layout the *running library* actually has,
including `sizeof` / `alignof` of the C++ object inside a `kimix_vec`, so a
binding that embeds the placeholder never trusts the header alone.

---

## Library and ABI queries — `<api/ffi_common.h>`

The basement: who is loaded, which ABI it speaks, and what a status code means.
Five functions; you need them at binding-initialisation time, and
`kimix_status_name()` whenever you surface an error to your own user.

### Version, status names and layout

#### `uint32_t kimix_api_abi_version(void)`
The ABI version this build of the library was compiled with — the value of `KIMIX_API_ABI_VERSION`. Compare it against the constant your binding was generated against.
- Wraps: the `KIMIX_API_ABI_VERSION` macro, returned as compiled into the image.
- Ownership: nothing to free.
- Errors: never fails.

#### `const char *kimix_api_version_string(void)`
The `kimix-core` version string this library was built against, e.g. `"kimix 1.2.3"`. NUL-terminated UTF-8.
- Ownership: static storage owned by the library — never free it; valid for the process lifetime.
- Errors: never fails.

#### `const char *kimix_api_build_info(void)`
A human-readable build stamp for bug reports: platform, architecture, build mode, compiler (`"windows x64 release msvc"` shape; the ABI number is `kimix_api_abi_version()`, not part of this string).
- Ownership: static storage — never free it.
- Errors: never fails.

#### `const char *kimix_status_name(KIMIX_IN kimix_status code)`
The textual name of a status code (`"KIMIX_OK"`, `"KIMIX_ERR_OUT_OF_MEMORY"`, …).
- Ownership: static storage — never free it.
- Errors: returns `NULL` for a value that is not one of the eight defined codes.

#### `kimix_status kimix_api_layout_info(KIMIX_OUT kimix_layout_info *out)`
Fills the POD `kimix_layout_info` (`abi_version`, `_reserved`, `size_of_size_t`, `size_of_pointer`, `size_of_bool`, `vec_abi_bytes`, `vec_abi_align`, `vec_inner_bytes`, `vec_inner_align`) with the layout facts of the running library.
- Ownership: `out` is caller-owned storage; every field is written on success. `_reserved` is always 0 — do not rely on it.
- Errors: `KIMIX_ERR_INVALID_ARG` when `out` is NULL.
- Notes: a binding that hard-codes a struct size should check this once at load time instead of trusting the header.

---

## Allocation — `<api/ffi_mem.h>` (`kimix_mem_*`)

The vendored mimalloc (3.5.2) behind stable names, so a binding can allocate its
own buffers on the very heap the library uses, hand them to the other areas, and
free everything through one family. `ffi_mem.cpp` is the only translation unit
that sees `<mimalloc.h>`; no `mi_*` name appears in the public header.

Side note for binding authors: because the vendored mimalloc objects are compiled
into this image with `MI_SHARED_LIB` + `MI_SHARED_LIB_EXPORT` (propagated by the
`mimalloc` target, see `src/ext/xmake.lua`), the DLL *also* exports the plain
`mi_malloc` / `mi_free` / ... names, plus the decorated `kimix::` core symbols.
They resolve if you need them, but they are an implementation detail: the stable,
documented surface is the `kimix_mem_*` family, and only that is covered by
`KIMIX_API_ABI_VERSION`.
Two global caveats from the header: **failures return `NULL`**, but this build
sets `MI_XMALLOC=1`, so a real exhaustion aborts the process unless the host
replaced mimalloc's error handler — check for NULL anyway, it is the promised
contract. And **size 0 is not an error**: `mi_malloc(0)` hands back a fresh
minimum-size block (never dereference it, still free it), and `mi_realloc(p, 0)`
shrinks to such a block instead of the C runtime's free-and-return-NULL.
Aligned calls take **`size` first, `alignment` second** (mimalloc's order, the
opposite of `posix_memalign`); only `kimix_mem_posix_memalign` keeps the posix
order. Alignment is a property of the block, not of a second heap: every block
here is freed with `kimix_mem_free`.

### Core allocation (the malloc family)

Returned blocks belong to the library heap and are released with
`kimix_mem_free()` — never with the caller's own `free()`.

#### `void *kimix_mem_malloc(size_t size)`
Allocates `size` uninitialised bytes.
- Wraps: `mi_malloc(size)`
- Ownership: caller owns the block; free with `kimix_mem_free`.
- Errors: `NULL` on failure. `size == 0` still returns a fresh non-NULL minimum block (not dereferenceable, still freeable).

#### `void *kimix_mem_calloc(size_t count, size_t size)`
Allocates a zero-filled array of `count` elements of `size` bytes.
- Wraps: `mi_calloc(count, size)` — same argument order, `count` first.
- Ownership: caller owns the block; free with `kimix_mem_free`.
- Errors: `NULL` on failure; an overflow of `count * size` fails like an out-of-memory (NULL, never a partial buffer).

#### `void *kimix_mem_zalloc(size_t size)`
Allocates `size` bytes filled with zero.
- Wraps: `mi_zalloc(size)`
- Ownership: caller owns the block; free with `kimix_mem_free`.
- Errors: `NULL` on failure; `size == 0` behaves like `kimix_mem_malloc(0)`.

#### `void *kimix_mem_realloc(void *p, size_t newsize)`
Re-sizes a block, possibly moving it.
- Wraps: `mi_realloc(p, newsize)` — the `(p, size)` order of mimalloc/C.
- Ownership: on success the old block is consumed by the library heap and the returned pointer (which may differ from `p`) owns the buffer; on failure `p` is untouched and still owned by the caller. `p == NULL` behaves like `kimix_mem_malloc`. Free the result with `kimix_mem_free`.
- Errors: `NULL` on failure. `newsize == 0` shrinks to a zero-sized block and returns non-NULL — unlike the C runtime, it does not free-and-return-NULL.

#### `void *kimix_mem_expand(void *p, size_t newsize)`
Tries to resize in place; never copies and never silently falls back to a new block.
- Wraps: `mi_expand(p, newsize)`
- Ownership: ownership never moves unless the call returned `p` for a shrink — then the caller owns the smaller view of the same block and frees it with `kimix_mem_free`.
- Errors: returns `p` when the block's usable size already covers `newsize` (grow or shrink), `NULL` when it does not, with `p` left completely intact and its content still valid, so the caller can fall back to `kimix_mem_realloc`. `p == NULL` returns `NULL`.
- Notes: discover the room available with `kimix_mem_usable_size`.

#### `void kimix_mem_free(void *p)`
Releases a block back to the library heap.
- Wraps: `mi_free(p)`
- Ownership: accepts any pointer returned by this area **or by another area of this library** (JSON writer strings, `kimix_vec_new()` storage, …). `p == NULL` is ignored.
- Errors: none (no return value).
- Notes: a wrong-heap pointer (the caller's own `malloc`, or a block from another build of this library) is UB; mimalloc validates the page pointer and reports it in secure/debug builds, the release build does not promise to catch it.

#### `char *kimix_mem_strdup(const char *s)`
Copies a NUL-terminated UTF-8 string onto the library heap.
- Wraps: `mi_strdup(s)`
- Ownership: the copy is caller-owned; free with `kimix_mem_free`.
- Errors: `NULL` for a NULL `s` (guarded explicitly) or on allocation failure.

#### `char *kimix_mem_strndup(const char *s, size_t n)`
Copies at most `n` bytes of `s` and always NUL-terminates the copy.
- Wraps: `mi_strndup(s, n)` — C `strndup` semantics: when `strlen(s) < n` only `strlen(s) + 1` bytes are allocated.
- Ownership: the copy is caller-owned; free with `kimix_mem_free`.
- Errors: `NULL` for a NULL `s`, on allocation failure, or for an over-long string.

### Aligned allocation

`alignment` must be a positive power of two; mimalloc's release build does **not**
diagnose a non-power-of-two alignment, so validating it is the caller's job. All
of these blocks are freed with `kimix_mem_free()`.

#### `void *kimix_mem_malloc_aligned(size_t size, size_t alignment)`
Allocates `size` bytes at an address that satisfies `alignment`.
- Wraps: `mi_malloc_aligned(size, alignment)`
- Ownership: caller owns the block; free with `kimix_mem_free` (or `kimix_mem_free_aligned`).
- Errors: `NULL` on failure.

#### `void *kimix_mem_malloc_aligned_at(size_t size, size_t alignment, size_t offset)`
Allocates an offset-aligned block whose address `a` satisfies `a % alignment == offset % alignment`.
- Wraps: `mi_malloc_aligned_at(size, alignment, offset)`
- Ownership: caller owns the block; free with `kimix_mem_free`.
- Errors: `NULL` on failure.

#### `void *kimix_mem_zalloc_aligned(size_t size, size_t alignment)`
Allocates `size` zero-filled bytes at an aligned address.
- Wraps: `mi_zalloc_aligned(size, alignment)`
- Ownership: caller owns the block; free with `kimix_mem_free`.
- Errors: `NULL` on failure.

#### `void *kimix_mem_calloc_aligned(size_t count, size_t size, size_t alignment)`
Allocates an aligned, zero-filled array of `count` elements of `size` bytes.
- Wraps: `mi_calloc_aligned(count, size, alignment)` — count, then element size, then alignment.
- Ownership: caller owns the block; free with `kimix_mem_free`.
- Errors: `NULL` on failure.

#### `void *kimix_mem_realloc_aligned(void *p, size_t newsize, size_t alignment)`
Re-sizes an aligned block, keeping the same alignment.
- Wraps: `mi_realloc_aligned(p, newsize, alignment)`
- Ownership: the old block is consumed on success and left intact on failure; `p == NULL` behaves like `kimix_mem_malloc_aligned`. Free the result with `kimix_mem_free`.
- Errors: `NULL` on failure.

#### `void kimix_mem_free_aligned(void *p, size_t alignment)`
Frees an aligned block, passing the alignment it was allocated with.
- Wraps: `mi_free_aligned(p, alignment)`
- Ownership: `p == NULL` is ignored.
- Errors: none.
- Notes: in mimalloc's release builds the `alignment` argument only feeds a debug assertion and the block is freed exactly like `kimix_mem_free()` would — this is documentation made executable.

#### `void kimix_mem_free_size(void *p, size_t size)`
Frees a block, passing the size originally requested.
- Wraps: `mi_free_size(p, size)`
- Ownership: `p == NULL` is ignored.
- Errors: none.
- Notes: `size` is a hint that selects the small-block fast path (`mi_free_small`) instead of re-deriving the page; a wrong-but-plausible size does not corrupt anything in release builds (the debug build reports mismatches).

### Size queries

#### `size_t kimix_mem_usable_size(const void *p)`
The real usable size of a block — the number of bytes that may be written to `p` (always >= the requested size).
- Wraps: `mi_usable_size(p)`
- Ownership: nothing to free.
- Errors: `0` for `p == NULL` (guarded here; mimalloc treats NULL as 0 as well).
- Notes: the standard way to find out what `kimix_mem_expand()` can grow into. A pointer that is not a live block of this heap is UB.

#### `size_t kimix_mem_good_size(size_t size)`
The block size mimalloc would hand out for `size` (rounded up to a size class).
- Wraps: `mi_good_size(size)`
- Ownership: nothing to free.
- Errors: never fails.
- Notes: use it to allocate in batches without per-block padding surprises.

### Zero-initialising re-allocation

Valid **only** on blocks that were originally allocated zero-initialised (`kimix_mem_calloc`, `kimix_mem_zalloc`, the aligned zero variants, or results of these two calls). Resizing memory that was never zero-initialised is a programming error mimalloc does not diagnose in release builds.

#### `void *kimix_mem_rezalloc(void *p, size_t newsize)`
Like `kimix_mem_realloc`, but the newly exposed tail bytes are zeroed.
- Wraps: `mi_rezalloc(p, newsize)`
- Ownership: old block consumed on success, kept on failure; `p == NULL` acts as a zalloc. Free the result with `kimix_mem_free`.
- Errors: `NULL` on failure.

#### `void *kimix_mem_recalloc(void *p, size_t newcount, size_t size)`
The calloc-shaped `rezalloc`: resize the array to `newcount` elements of `size` bytes, zeroing the grown tail.
- Wraps: `mi_recalloc(p, newcount, size)` — argument order `p`, `newcount`, `size`.
- Ownership: as `kimix_mem_rezalloc`; free with `kimix_mem_free`.
- Errors: `NULL` on failure.

### posix-shaped aligned allocation

#### `kimix_status kimix_mem_posix_memalign(void **p, size_t alignment, size_t size)`
`posix_memalign` with this library's error vocabulary.
- Wraps: `mi_posix_memalign(p, alignment, size)` — note the posix order `(p, alignment, size)`; the only call in this area whose argument order does not follow mimalloc's size-first rule, because it mirrors the posix prototype 1:1.
- Ownership: the aligned block is written into `*p` on success and is caller-owned; free with `kimix_mem_free`.
- Errors: `KIMIX_ERR_INVALID_ARG` when `p` is NULL or `alignment` is not a power of two >= `sizeof(void *)` (the posix rule); `KIMIX_ERR_OUT_OF_MEMORY` when the allocation fails, leaving `*p` exactly untouched; `KIMIX_OK` otherwise.
- Notes: arguments are validated **before** entering mimalloc, so a malformed request cannot reach the MI_XMALLOC aborting error handler.

### Thread and heap management for foreign threads

mimalloc attaches a heap to every thread that allocates through it. A binding
driving foreign threads (a host thread pool, a scripting runtime, GC threads)
should bracket each thread's use: `kimix_mem_thread_init()` before the first
allocation, `kimix_mem_thread_done()` before the thread exits. Blocks still owned
by that heap are **not** freed by `thread_done`: they stay valid and freeable from
any thread until collected.

#### `void kimix_mem_thread_init(void)`
Registers a mimalloc heap for the calling thread.
- Wraps: `mi_thread_init()`
- Ownership: nothing to free.
- Errors: never fails; calling it twice on one thread is harmless.
- Notes: mimalloc calls it implicitly on first use — pre-register only to avoid a first-allocation hiccup or to re-register after `kimix_mem_thread_done()`.

#### `void kimix_mem_thread_done(void)`
Releases the calling thread's heap registration.
- Wraps: `mi_thread_done()`
- Ownership: still-live blocks remain valid and may be freed from any thread until the pages are collected.
- Errors: never fails.
- Notes: pair with `kimix_mem_thread_init()` before a foreign thread exits; the thread may keep using the library afterwards (the next allocation re-initialises implicitly, explicit re-init is the clean way).

#### `void kimix_mem_collect(bool force)`
Asks mimalloc to return cached/abandoned memory to the OS now.
- Wraps: `mi_collect(force)`
- Ownership: nothing to free.
- Errors: never fails.
- Notes: `true` performs a full (slower) collection including the thread's heap, `false` a cheap opportunistic pass. Safe from any thread; useful after a large burst of frees from foreign threads that never ran `kimix_mem_thread_done()`.

### Identity

#### `uint32_t kimix_mem_version(void)`
The mimalloc version number this library was built with, encoding `major * 10000 + minor * 100 + patch` (two decimal digits each) — `30502` is mimalloc 3.5.2.
- Wraps: the compile-time `MI_MALLOC_VERSION` macro of the vendored `mimalloc.h`, returned unchanged.
- Ownership: nothing to free.
- Errors: never fails.
- Notes: the `mi_*` code is compiled **into** this DLL, so there is no version skew — this is the version of the exact heap the caller's buffers live in. Assert on it once at load time; `kimix_mem_version_string()` gives the human form.

#### `const char *kimix_mem_version_string(void)`
The same version as text: `major.minor.patch` without leading zeros (`"3.5.2"` for `30502`).
- Wraps: compile-time decimal decomposition of `MI_MALLOC_VERSION` (the vendored header provides no `..._MAJOR/MINOR/PATCH` macros).
- Ownership: static storage owned by the library, valid for the process lifetime — never free it.
- Errors: never `NULL`, never fails.

#### `bool kimix_mem_is_redirected(void)`
True when mimalloc has taken over the process-wide allocator, i.e. the host's libc `malloc`/`free` (or the Windows heap) is redirected into this same heap.
- Wraps: `mi_is_redirected()`
- Ownership: nothing to free.
- Errors: never fails.
- Notes: this library is built with `MI_WIN_NOREDIRECT` on Windows, so the normal answer there is `false` — only memory obtained through `kimix_mem_*` or the library itself is mimalloc. Where an override is compiled in it may return `true`, in which case even the caller's own malloc traffic shares this heap.

### Raw block operations (copy / move / swap / fill / compare / search)

The C `<string.h>` vocabulary (`memcpy` / `memmove` / `memset` / `memcmp` / `memchr`)
applied to any memory — a library-heap block or the caller's own buffer.
Nothing here allocates, frees, or transfers ownership, and none of these
touch the debug-build leak tracker. All ranges are byte counts. Every entry
is NULL-safe only for `n == 0` (the C contract: when nothing is read or
written, any pointer value is acceptable); a NULL pointer with `n > 0` is UB,
deliberately not turned into an error return — exactly as with the C
functions these map to. `kimix_mem_copy` requires disjoint ranges;
`kimix_mem_move` is overlap-safe in either direction; `kimix_mem_swap`
requires disjoint ranges (`a == b` is a no-op).

#### `void kimix_mem_copy(void *dst, const void *src, size_t n)`
Copies `n` bytes from `src` to `dst`.
- Wraps: `memcpy` semantics — the ranges must NOT overlap (use `kimix_mem_move` when they might).
- Ownership: both ranges stay with the caller; nothing is allocated.
- Errors: none (void return). `n == 0` is a no-op for any pointer values.

#### `void kimix_mem_move(void *dst, const void *src, size_t n)`
Copies `n` bytes from `src` to `dst`, overlap-safe in either direction.
- Wraps: `memmove` semantics.
- Ownership: both ranges stay with the caller; nothing is allocated.
- Errors: none (void return). `n == 0` is a no-op for any pointer values.

#### `void kimix_mem_swap(void *a, void *b, size_t n)`
Exchanges the `n` bytes at `a` and `b` in place, element by element — no scratch block, so it allocates nothing and is safe even from a foreign thread that never registered with the heap.
- Ownership: both ranges stay with the caller.
- Errors: none (void return). `a == b` is a no-op; overlapping-but-unequal ranges are UB (same contract as a hand-rolled exchange loop).

#### `void kimix_mem_set(void *dst, int byte, size_t n)`
Sets `n` bytes at `dst` to `(unsigned char)byte`.
- Wraps: `memset` semantics.
- Ownership: the range stays with the caller; nothing is allocated.
- Errors: none (void return). `n == 0` is a no-op for any pointer value.

#### `void kimix_mem_zero(void *dst, size_t n)`
Zeros `n` bytes at `dst` — the common `kimix_mem_set(dst, 0, n)` case spelled out.
- Wraps: `memset(dst, 0, n)`.
- Ownership: the range stays with the caller; nothing is allocated.
- Errors: none (void return). `n == 0` is a no-op for any pointer value.

#### `int kimix_mem_compare(const void *a, const void *b, size_t n)`
Lexicographic byte comparison of `a` and `b` over `n` bytes.
- Wraps: `memcmp` semantics, normalised to a strict `-1` / `0` / `+1` so a binding sees exactly one of three values instead of `memcmp`'s arbitrary sign and magnitude.
- Ownership: both ranges stay with the caller; nothing is allocated.
- Errors: none (int return). `n == 0` compares equal (`0`) without touching the pointers.

#### `bool kimix_mem_equals(const void *a, const void *b, size_t n)`
True when all `n` bytes are equal — the `kimix_mem_compare(a, b, n) == 0` common case spelled out.
- Ownership: both ranges stay with the caller; nothing is allocated.
- Errors: never fails. `n == 0` returns true without touching the pointers.

#### `size_t kimix_mem_find_byte(const void *p, size_t n, unsigned char byte)`
Offset of the first occurrence of `byte` within the first `n` bytes of `p`.
- Wraps: `memchr` with an offset result: returns the byte offset on a hit, `(size_t)-1` when the byte is absent — so "not found" can never be confused with "found at offset 0" the way a NULL return could.
- Ownership: the range stays with the caller; nothing is allocated.
- Errors: never fails; `(size_t)-1` means "not in the first `n` bytes". `n == 0` returns `(size_t)-1` without touching the pointer.

#### `size_t kimix_mem_count_byte(const void *p, size_t n, unsigned char byte)`
The number of occurrences of `byte` within the first `n` bytes of `p`.
- Ownership: the range stays with the caller; nothing is allocated.
- Errors: never fails; `0` means "none". `n == 0` returns `0` without touching the pointer.


---

## Byte vectors — `<api/ffi_vec.h>` (`kimix_vec_*`)

`kimix::vector<std::byte>` — the mimalloc-backed growable byte buffer of
`src/core/stl/vector.h` — exported as an **inline placeholder object** the caller
stores in its own memory (a struct field, an array element, a stack slot) instead
of a heap handle plus a dereference in every call. `sizeof` and `alignof` of that
C++ object are compile-time constants of the library build, which is what makes
embedding legal. The placeholder is 64 bytes wide and 16-byte aligned:

```c
#define KIMIX_VEC_BYTES 64u
#define KIMIX_VEC_ALIGN 16u
typedef struct kimix_vec {
    KIMIX_ALIGNAS(KIMIX_VEC_ALIGN) unsigned char _storage[KIMIX_VEC_BYTES];
} kimix_vec;
```

Inside the library, `api/detail.h` + `ffi_vec.cpp` do the object surgery: each
entry point reads an 8-byte **guard word** at the tail of `_storage`
(`k_vec_live_guard == "KIX_VECT"`, `k_vec_dead_guard == 0`) through
`std::memcpy`, and only when it says "live" does it re-derive the C++ object from
the raw storage bytes with `std::launder(reinterpret_cast<vector<std::byte> *>(_storage))`.
Construction is placement `new` into the storage, destruction is the explicit
`~vector()` plus clearing the guard. Consequences for a binding:

* the RAII pair is **mandatory**: `init` … use … `destroy`, exactly once each;
* touching a never-initialised or already-destroyed placeholder returns
  `KIMIX_ERR_INVALID_STATE` (or the neutral value) instead of corrupting memory —
  a memset-to-zero placeholder is "raw storage", stack garbage is "raw storage";
* `ffi_vec.cpp` `static_assert`s freeze the ABI: `sizeof(kimix_vec) == KIMIX_VEC_BYTES`,
  `alignof(kimix_vec) == KIMIX_VEC_ALIGN`, `sizeof(vector) + 8 <= KIMIX_VEC_BYTES`,
  `alignof(vector) <= KIMIX_VEC_ALIGN`, the guard slot not overlapping the object,
  `sizeof/alignof(vector<char>) == sizeof/alignof(vector<std::byte>)` (the
  `ffi_repair` bridge), `sizeof(std::byte) == sizeof(char) == 1`, and
  `KIMIX_VEC_BYTES == 64u && KIMIX_VEC_ALIGN == 16u`. If one ever fires, the fix
  is to raise `KIMIX_VEC_BYTES` (and `KIMIX_API_ABI_VERSION`), never to shrink the
  object — which is why the numbers are generous, not `sizeof` exactly: an MSVC
  `_ITERATOR_DEBUG_LEVEL=2` build stores a debug proxy pointer (40 bytes) where
  libstdc++ release needs 24.
* **moves are the only cheap transfer**: a moved-from placeholder stays
  initialised and valid but empty, and still has to be destroyed exactly once.
  Copying the raw bytes of a placeholder is *not* a copy of the vector — use
  `kimix_vec_copy_init()`.
* these calls never report `KIMIX_ERR_OUT_OF_MEMORY` for the vector's own buffer:
  `kimix::allocator` reports exhaustion by calling `kimix::allocation_failure()`
  and aborting (no exceptions in this build, so `std::bad_alloc` never unwinds into
  this FFI). The code is still listed because it does apply to the heap helpers
  (`kimix_vec_new*`), which allocate the placeholder storage with `mi_malloc` and
  can genuinely fail.

Callers may embed the placeholder in any struct, array or stack slot and may
`memset` it to zero to get raw storage, but must **never read `_storage`** and
must not treat a raw byte copy as a copy of the vector. `KIMIX_ALIGNAS(N)` maps
`alignas` / `_Alignas` / `__declspec(align(N))` / `__attribute__((aligned(N)))` to
the right spelling per compiler, and `KIMIX_VEC_SIZEOF` is
`((size_t)sizeof(kimix_vec))` as the caller sees it.

### ABI facts

#### `size_t kimix_vec_abi_bytes(void)`
The fixed inline-storage size a caller must reserve (`KIMIX_VEC_BYTES`, 64).
- Ownership: nothing to free.
- Errors: never fails.

#### `size_t kimix_vec_abi_align(void)`
The fixed alignment a caller must give the placeholder (`KIMIX_VEC_ALIGN`, 16).
- Ownership: nothing to free.
- Errors: never fails.

#### `size_t kimix_vec_inner_bytes(void)`
`sizeof` of the C++ `kimix::vector<std::byte>` the library actually constructs inside the storage.
- Ownership: nothing to free.
- Errors: never fails.
- Notes: must always be `<= kimix_vec_abi_bytes()`; a change here means the STL or the debug-iterator level changed under you.

#### `size_t kimix_vec_inner_align(void)`
`alignof` of that C++ object.
- Ownership: nothing to free.
- Errors: never fails.
- Notes: must always be `<= kimix_vec_abi_align()`.

#### `bool kimix_vec_is_initialized(KIMIX_IN const kimix_vec *v)`
True when `v` currently holds a live C++ vector (initialised and not yet destroyed).
- Ownership: `v` stays caller-owned; nothing is read out of it but the guard word.
- Errors: `false` for a NULL `v`.
- Notes: every other `kimix_vec_*` call performs this check itself, so this exists for your own teardown assertions.

### Construction and destruction (the RAII pair)

#### `kimix_status kimix_vec_default_init(KIMIX_OUT kimix_vec *v)`
Constructs an EMPTY vector in `v`.
- Wraps: `new (storage) std::vector<std::byte>()`
- Ownership: `v` must be raw storage — not yet initialised, or memset to zero. On success `v` holds a live vector that must be destroyed exactly once with `kimix_vec_destroy`.
- Errors: `KIMIX_ERR_INVALID_ARG` for a NULL `v`; `KIMIX_ERR_INVALID_STATE` when `v` already holds a live vector (constructing again would leak its buffer).

#### `kimix_status kimix_vec_init_from(KIMIX_OUT kimix_vec *v, KIMIX_IN const void *data, size_t len)`
Constructs `v` as a copy of the bytes `[data, data + len)`.
- Wraps: `new (storage) std::vector<std::byte>(first, last)`
- Ownership: `data` is read during the call only; the buffer the vector allocates belongs to the library heap. Same pre-conditions as `kimix_vec_default_init`.
- Errors: `KIMIX_ERR_INVALID_ARG` for a NULL `v` or a NULL `data` with `len > 0`; `KIMIX_ERR_INVALID_STATE` when `v` already holds a live vector.

#### `kimix_status kimix_vec_copy_init(KIMIX_OUT kimix_vec *dst, KIMIX_IN const kimix_vec *src)`
Constructs `dst` (raw storage) as a deep **copy** of the live vector `src`.
- Wraps: `new (storage) std::vector<std::byte>(*src)`
- Ownership: allocates `len(src)` bytes on the library heap; `dst` must be destroyed once. `src` is unchanged.
- Errors: `KIMIX_ERR_INVALID_ARG` for a NULL argument; `KIMIX_ERR_INVALID_STATE` when `src` is not live or `dst` is not raw storage.

#### `kimix_status kimix_vec_move_init(KIMIX_OUT kimix_vec *dst, KIMIX_TRANSFER kimix_vec *src)`
Constructs `dst` (raw storage) by **moving** the live vector `src`.
- Wraps: `new (storage) std::vector<std::byte>(std::move(*src))`
- Ownership: the buffer pointer travels with the move, so it stays owned by the heap that made it. After success `src` is still initialised and valid but empty, and must still be destroyed exactly once.
- Errors: `KIMIX_ERR_INVALID_ARG` for a NULL argument, for `dst == src` (moving an object onto itself), or when `src` is not live; `KIMIX_ERR_INVALID_STATE` when `dst` is not raw storage.

#### `kimix_status kimix_vec_destroy(KIMIX_IN_OUT kimix_vec *v)`
Destroys the vector in `v` and releases its buffer to the library heap.
- Wraps: `v->~vector()` plus clearing the guard word.
- Ownership: after this the placeholder is raw storage again and may be re-initialised.
- Errors: `KIMIX_ERR_INVALID_ARG` for a NULL `v`; `KIMIX_ERR_INVALID_STATE` — touching nothing — when `v` was never initialised or already destroyed.

### Assignment (both sides must already hold a live vector)

#### `kimix_status kimix_vec_assign(KIMIX_IN_OUT kimix_vec *dst, KIMIX_IN const kimix_vec *src)`
`dst = *src` — a deep copy.
- Wraps: `std::vector::operator=` (copy)
- Ownership: `dst`'s old content is released on the library heap; both placeholders stay live and each is still destroyed once.
- Errors: `KIMIX_ERR_INVALID_ARG` for a NULL argument; `KIMIX_ERR_INVALID_STATE` when either side is not a live vector.

#### `kimix_status kimix_vec_move_assign(KIMIX_IN_OUT kimix_vec *dst, KIMIX_TRANSFER kimix_vec *src)`
`dst = std::move(*src)`.
- Wraps: `std::vector::operator=` (move)
- Ownership: `dst`'s old content is freed, `src` becomes empty but stays initialised (and destroyable).
- Errors: `KIMIX_ERR_INVALID_ARG` for a NULL argument or `dst == src`; `KIMIX_ERR_INVALID_STATE` when either side is not a live vector.

#### `kimix_status kimix_vec_assign_bytes(KIMIX_IN_OUT kimix_vec *v, KIMIX_IN const void *data, size_t len)`
Replaces the content with the bytes `[data, data + len)`.
- Wraps: `std::vector::assign(first, last)`
- Ownership: `data` is read during the call; the old buffer returns to the library heap.
- Errors: `KIMIX_ERR_INVALID_ARG` for a NULL `data` with `len > 0`; `KIMIX_ERR_INVALID_STATE` for a dead placeholder.

### Observation

#### `size_t kimix_vec_size(KIMIX_IN const kimix_vec *v)`
The number of bytes stored (0 for an empty vector).
- Wraps: `std::vector::size()`
- Ownership: nothing to free.
- Errors: `(size_t)-1` when `v` is NULL or not initialised — an out-of-band size no buffer can ever have.

#### `size_t kimix_vec_capacity(KIMIX_IN const kimix_vec *v)`
The allocated capacity in bytes.
- Wraps: `std::vector::capacity()`
- Ownership: nothing to free.
- Errors: `(size_t)-1` on an invalid `v`.

#### `bool kimix_vec_empty(KIMIX_IN const kimix_vec *v)`
True when `size() == 0`.
- Wraps: `std::vector::empty()`
- Ownership: nothing to free.
- Errors: `false` on an invalid `v` (so a dead placeholder also reads as "empty" — use `kimix_vec_is_initialized` to tell).

#### `unsigned char *kimix_vec_data(KIMIX_BORROWED kimix_vec *v)`
The pointer to the contiguous bytes.
- Wraps: `std::vector::data()`
- Ownership: BORROWED from the vector: valid until any call that resizes, reserves, assigns or destroys it. NULL for an invalid `v`, and — as with `std::vector` — also for an empty vector, whose pointer must not be dereferenced.
- Errors: no status; NULL is the only failure signal.

#### `kimix_status kimix_vec_at(KIMIX_BORROWED kimix_vec *v, size_t index, KIMIX_OUT unsigned char **out)`
Bounds-checked element address: `*out == &data()[index]`.
- Wraps: range check plus `data() + index`
- Ownership: `*out` is BORROWED from `v`, with the same invalidation rule as `kimix_vec_data`. `out` is written only on success (`*out` is neutralised to NULL first).
- Errors: `KIMIX_ERR_INVALID_ARG` for a NULL `out`; `KIMIX_ERR_OUT_OF_RANGE` when `index >= size()`; `KIMIX_ERR_INVALID_ARG` / `KIMIX_ERR_INVALID_STATE` on a NULL / dead `v`.

#### `kimix_status kimix_vec_read(KIMIX_IN const kimix_vec *v, size_t offset, KIMIX_OUT void *dst, size_t len)`
Copies `len` bytes **out of** the vector starting at `offset`.
- Wraps: `std::memcpy` from `data() + offset`
- Ownership: `dst` is caller-owned memory; the vector is not modified.
- Errors: `KIMIX_ERR_INVALID_ARG` for a NULL `dst` with `len > 0`; `KIMIX_ERR_OUT_OF_RANGE` unless the whole `[offset, offset + len)` lies inside the vector (resize/reserve first); `KIMIX_ERR_INVALID_ARG` / `KIMIX_ERR_INVALID_STATE` on a NULL / dead `v`.

#### `kimix_status kimix_vec_write(KIMIX_IN_OUT kimix_vec *v, size_t offset, KIMIX_IN const void *src, size_t len)`
Copies `len` bytes **into** the vector starting at `offset` — it never grows the vector.
- Wraps: `std::memcpy` into `data() + offset`
- Ownership: `src` is read during the call; `v` keeps its own buffer.
- Errors: `KIMIX_ERR_INVALID_ARG` for a NULL `src` with `len > 0`; `KIMIX_ERR_OUT_OF_RANGE` unless the whole range lies inside the vector; `KIMIX_ERR_INVALID_ARG` / `KIMIX_ERR_INVALID_STATE` on a NULL / dead `v`.

#### `kimix_status kimix_vec_equals(KIMIX_IN const kimix_vec *a, KIMIX_IN const kimix_vec *b, KIMIX_OUT bool *out_equal)`
Element-wise equality of two vectors (size and content).
- Wraps: `std::vector::operator==`
- Ownership: both operands are read only. `*out_equal` is written (1/0) on success.
- Errors: `KIMIX_ERR_INVALID_ARG` for a NULL `out_equal` (it is pre-set to `false`); `KIMIX_ERR_INVALID_ARG` / `KIMIX_ERR_INVALID_STATE` for a NULL / dead `a` or `b`.

### Modification

Every one of these may reallocate and therefore invalidates a previously returned
`kimix_vec_data()` / `kimix_vec_at()` pointer.

#### `kimix_status kimix_vec_resize(KIMIX_IN_OUT kimix_vec *v, size_t n)`
`v.resize(n)` — grows with zero bytes or shrinks.
- Wraps: `std::vector::resize(n)`
- Ownership: size becomes `n`; capacity may grow.
- Errors: `KIMIX_ERR_INVALID_ARG` / `KIMIX_ERR_INVALID_STATE` on a NULL / dead `v`.

#### `kimix_status kimix_vec_resize_fill(KIMIX_IN_OUT kimix_vec *v, size_t n, unsigned char fill)`
`v.resize(n, fill)` — grows with `fill` bytes instead of zeros.
- Wraps: `std::vector::resize(n, value)`
- Ownership: as `kimix_vec_resize`.
- Errors: `KIMIX_ERR_INVALID_ARG` / `KIMIX_ERR_INVALID_STATE` on a NULL / dead `v`.

#### `kimix_status kimix_vec_reserve(KIMIX_IN_OUT kimix_vec *v, size_t n)`
`v.reserve(n)` — capacity only, size unchanged; may reallocate.
- Wraps: `std::vector::reserve(n)`
- Ownership: as `kimix_vec_resize`.
- Errors: `KIMIX_ERR_INVALID_ARG` / `KIMIX_ERR_INVALID_STATE` on a NULL / dead `v`.

#### `kimix_status kimix_vec_shrink_to_fit(KIMIX_IN_OUT kimix_vec *v)`
`v.shrink_to_fit()` — release unused capacity.
- Wraps: `std::vector::shrink_to_fit()`
- Ownership: may move the buffer; the old one returns to the library heap.
- Errors: `KIMIX_ERR_INVALID_ARG` / `KIMIX_ERR_INVALID_STATE` on a NULL / dead `v`.

#### `kimix_status kimix_vec_clear(KIMIX_IN_OUT kimix_vec *v)`
`v.clear()` — size becomes 0, capacity is kept.
- Wraps: `std::vector::clear()`
- Ownership: the buffer stays allocated in the vector.
- Errors: `KIMIX_ERR_INVALID_ARG` / `KIMIX_ERR_INVALID_STATE` on a NULL / dead `v`.

#### `kimix_status kimix_vec_push_back(KIMIX_IN_OUT kimix_vec *v, unsigned char b)`
`v.push_back(b)` — append one byte.
- Wraps: `std::vector::push_back(value)`
- Ownership: as above.
- Errors: `KIMIX_ERR_INVALID_ARG` / `KIMIX_ERR_INVALID_STATE` on a NULL / dead `v`.

#### `kimix_status kimix_vec_pop_back(KIMIX_IN_OUT kimix_vec *v)`
`v.pop_back()` — remove the last byte.
- Wraps: `std::vector::pop_back()`
- Ownership: size decreases by one.
- Errors: `KIMIX_ERR_OUT_OF_RANGE` when the vector is empty; `KIMIX_ERR_INVALID_ARG` / `KIMIX_ERR_INVALID_STATE` on a NULL / dead `v`.

#### `kimix_status kimix_vec_append(KIMIX_IN_OUT kimix_vec *v, KIMIX_IN const void *data, size_t len)`
Appends the bytes `[data, data + len)` at the end.
- Wraps: `std::vector::insert(end(), first, last)`
- Ownership: `data` is read during the call only.
- Errors: `KIMIX_ERR_INVALID_ARG` for a NULL `data` with `len > 0`; `KIMIX_ERR_INVALID_ARG` / `KIMIX_ERR_INVALID_STATE` on a NULL / dead `v`. `len == 0` succeeds without touching anything.

#### `kimix_status kimix_vec_insert(KIMIX_IN_OUT kimix_vec *v, size_t index, KIMIX_IN const void *data, size_t len)`
Inserts the bytes `[data, data + len)` **before** `index`.
- Wraps: `std::vector::insert(begin() + index, first, last)`
- Ownership: `data` is read during the call only.
- Errors: `KIMIX_ERR_INVALID_ARG` for a NULL `data` with `len > 0`; `KIMIX_ERR_OUT_OF_RANGE` when `index > size()` (so `index == size()` appends); `KIMIX_ERR_INVALID_ARG` / `KIMIX_ERR_INVALID_STATE` on a NULL / dead `v`.

#### `kimix_status kimix_vec_erase(KIMIX_IN_OUT kimix_vec *v, size_t index, size_t len)`
Removes the byte range `[index, index + len)`.
- Wraps: `std::vector::erase(begin() + index, begin() + index + len)`
- Ownership: size shrinks by `len`; capacity is kept.
- Errors: `KIMIX_ERR_OUT_OF_RANGE` when the range is not inside the vector (`len` may be 0); `KIMIX_ERR_INVALID_ARG` / `KIMIX_ERR_INVALID_STATE` on a NULL / dead `v`.

#### `kimix_status kimix_vec_swap(KIMIX_IN_OUT kimix_vec *a, KIMIX_IN_OUT kimix_vec *b)`
Swaps the two vectors' buffers in O(1).
- Wraps: `std::vector::swap(other)`
- Ownership: both placeholders stay initialised, neither is copied and no allocation happens; each keeps being destroyed exactly once by its own owner.
- Errors: `KIMIX_ERR_INVALID_ARG` / `KIMIX_ERR_INVALID_STATE` on a NULL / dead `a` or `b`.

### Heap convenience (a handle instead of inline storage)

#### `kimix_vec *kimix_vec_new(void)`
Allocates one placeholder on the library heap and default-constructs an empty vector in it.
- Wraps: `mi_malloc_aligned(KIMIX_VEC_BYTES, KIMIX_VEC_ALIGN)` (the header describes it as `kimix_mem_malloc_aligned()`) plus the placement construction of `kimix_vec_default_init`.
- Ownership: the caller owns the handle and must release it with `kimix_vec_free()`.
- Errors: `NULL` on allocation failure.
- Notes: the storage is allocated with the placeholder's 16-byte alignment because it holds an over-aligned C++ object — a plain `kimix_mem_malloc` block is not a valid substitute.

#### `kimix_vec *kimix_vec_new_from(KIMIX_IN const void *data, size_t len)`
Same, but initialised with the bytes `[data, data + len)`.
- Wraps: `kimix_vec_new()` plus `std::vector::assign(first, last)` on the fresh object.
- Ownership: release with `kimix_vec_free()`.
- Errors: `NULL` on allocation failure or a NULL `data` with `len > 0`.

#### `void kimix_vec_free(KIMIX_TRANSFER kimix_vec *v)`
Destroys the vector and releases the placeholder storage.
- Wraps: the explicit `~vector()` of `kimix_vec_destroy` plus `mi_free(v)`.
- Ownership: `v` must have come from `kimix_vec_new()` / `kimix_vec_new_from()` (or `kimix_repair_new()`); a stack-allocated placeholder must be released with `kimix_vec_destroy()`. NULL is ignored.
- Errors: none (no return value); a placeholder whose guard already says "dead" simply is not destroyed twice, and its storage is still returned to the heap.

---

## JSON — `<api/ffi_yyjson.h>` (`kimix_yyjson_*`)

The vendored yyjson (a 0.13.0 fork) read / build / write surface, exported with
the **allocator hidden**: yyjson is allocator-parametric (`yyjson_alc` is three
function pointers plus a context), and letting a caller install a libc allocator
would produce exactly the cross-heap free this FFI exists to prevent. So no
exported function takes an `yyjson_alc`; internally every call gets
`kimix::api::yyjson_mi_alc()`, the library's mimalloc heap. The corollaries a
binding must internalise: documents are released with `kimix_yyjson_doc_free()` /
`kimix_yyjson_mut_doc_free()`; writer output is released with
`kimix_yyjson_str_free()` (its body is `mi_free`) and with nothing else; and the
short libc-allocating convenience forms `yyjson_read` / `yyjson_write` /
`yyjson_val_write` / `yyjson_mut_write` are never called here — only the
`*_opts` variants.

Four types cross the boundary, all **opaque** (forward declarations of the same
struct tags; no size, no field, no `yyjson.h` include): `kimix_yyjson_doc`
(immutable document), `kimix_yyjson_val` (a value in it, BORROWED),
`kimix_yyjson_mut_doc` (mutable document), `kimix_yyjson_mut_val` (a value in it,
BORROWED). Immutable and mutable values have **different layouts** (16 vs 24
bytes): sections 2–5 take `kimix_yyjson_val`, sections 9–11 take
`kimix_yyjson_mut_val`, and mixing them up reads the wrong fields — the FFI
cannot detect it. Convert with `kimix_yyjson_doc_mut_copy()` /
`kimix_yyjson_mut_doc_imut_copy()`.

**NULL safety:** every function below checks its pointer arguments and returns the
documented neutral value (`false` / `0` / `NULL`) instead of dereferencing NULL.
Nothing throws; every failure is a return value.

**Deliberately not exported:** the allocator interface and `yyjson_alc_*` helpers;
the `*_file` / `*_fp` entry points (a `FILE *` is a CRT object — open the file in
the caller and hand the bytes to `kimix_yyjson_read()`); the `unsafe_yyjson_*`
family (unchecked dereferences); the `yyjson_set_*` family (in-place retagging of
immutable values — build a mutable document instead); JSON-Pointer (`*_ptr_*`),
Merge-Patch (`yyjson_*_patch`) and the deprecated `*_get_pointer*` spellings; the
iterators `yyjson_arr_iter` / `yyjson_obj_iter` (POD structs whose layout would
become ABI — use the index/key accessors); and the mutable-document pool tunables
(`yyjson_mut_doc_set_str_pool_size` / `_set_val_pool_size`).

**Constants are macros, not objects.** yyjson declares its `yyjson_read_flag`,
`yyjson_write_flag`, `yyjson_type` and error-code values as `static const`
objects, which cannot cross a boundary; the `KIMIX_YYJSON_*` macros below are
copies of those literals and `ffi_yyjson.cpp` `static_assert`s every one against
the real header, so a fork that changes a value breaks the build instead of the
caller. Value types are `KIMIX_YYJSON_TYPE_NONE` (0, "a NULL pointer was passed"),
`_RAW` 1, `_NULL` 2, `_BOOL` 3, `_NUM` 4, `_STR` 5, `_ARR` 6, `_OBJ` 7. Read
flags: `KIMIX_YYJSON_READ_NOFLAG` (0, strict RFC 8259), `_INSITU` 1,
`_STOP_WHEN_DONE` 2, `_ALLOW_TRAILING_COMMAS` 4, `_ALLOW_COMMENTS` 8,
`_ALLOW_INF_AND_NAN` 16, `_NUMBER_AS_RAW` 32, `_ALLOW_INVALID_UNICODE` 64,
`_BIGNUM_AS_RAW` 128, `_ALLOW_BOM` 256, `_ALLOW_EXT_NUMBER` 512,
`_ALLOW_EXT_ESCAPE` 1024, `_ALLOW_EXT_WHITESPACE` 2048,
`_ALLOW_SINGLE_QUOTED_STR` 4096, `_ALLOW_UNQUOTED_KEY` 8192, combined as
`KIMIX_YYJSON_READ_JSON5` (= 15900). Write flags: `_NOFLAG` 0 (minify), `_PRETTY`
1, `_ESCAPE_UNICODE` 2, `_ESCAPE_SLASHES` 4, `_ALLOW_INF_AND_NAN` 8,
`_INF_AND_NAN_AS_NULL` 16, `_ALLOW_INVALID_UNICODE` 32, `_PRETTY_TWO_SPACES` 64,
`_NEWLINE_AT_END` 128, `_LOWERCASE_HEX` 256, plus the format helpers
`KIMIX_YYJSON_WRITE_FP_TO_FIXED(prec)` (`prec` in 1..15, in the top 4 bits),
`KIMIX_YYJSON_WRITE_FP_TO_FLOAT` (`1u << 27`) and the field-width macros
`KIMIX_YYJSON_WRITE_FP_FLAG_BITS` / `_FP_PREC_BITS`. Read error codes are
`KIMIX_YYJSON_READ_SUCCESS` (0) through `KIMIX_YYJSON_READ_ERROR_DEPTH` (15),
write codes `KIMIX_YYJSON_WRITE_SUCCESS` (0) through
`KIMIX_YYJSON_WRITE_ERROR_DEPTH` (8); the `_FILE_OPEN` / `_FILE_READ` codes and
`KIMIX_YYJSON_READ_ERROR_MORE` are unreachable through this surface.

The two error records are caller-side mirrors of yyjson's PODs (same field order
and widths, but filled field by field, never reinterpret-cast):
`kimix_yyjson_read_err { uint32_t code; const char *msg; size_t position; }` and
`kimix_yyjson_write_err { uint32_t code; const char *msg; }`. `msg` is a constant
owned by the library's static storage — BORROWED, never free it, and `NULL` on
success. A NULL `err` argument means "I do not want the reason"; the return value
alone still tells success from failure.

### 0. Identity

#### `uint32_t kimix_yyjson_version(void)`
The yyjson version the library was built against, as a hex number.
- Wraps: `yyjson_version()`
- Ownership: nothing to free.
- Errors: never fails.
- Notes: encoded `major << 16 | minor << 8 | patch`, so 0.13.0 is `0x000D00`.

### 1. Immutable document: parse and lifecycle

#### `kimix_yyjson_doc *kimix_yyjson_read(KIMIX_IN const char *data, size_t len, uint32_t flags, KIMIX_OUT KIMIX_NULLABLE kimix_yyjson_read_err *err)`
Parses JSON into a new immutable document.
- Wraps: `yyjson_read_opts()` with the library's mimalloc allocator.
- Ownership: the document is TRANSFERred to the caller — release with `kimix_yyjson_doc_free()`. `data` is read during the call; without `KIMIX_YYJSON_READ_INSITU` the input is copied into the document, so `data` may be released as soon as the call returns. **With INSITU** the caller must keep `data` alive *and writable* for the whole life of the document, padded by at least 4 zero bytes past `len` (the fork's `YYJSON_PADDING_SIZE`): INSITU on a truly const or unpadded buffer is UB, which is why this FFI hands out a `const char *`. `len` is bytes; no NUL terminator required. `err` is always written (success appears there as `code == KIMIX_YYJSON_READ_SUCCESS`).
- Errors: `NULL` on failure; the reason is in `err->code` / `err->msg` / `err->position`.

#### `kimix_yyjson_doc *kimix_yyjson_read_str(KIMIX_IN const char *str, uint32_t flags, KIMIX_OUT KIMIX_NULLABLE kimix_yyjson_read_err *err)`
The NUL-terminated spelling: the input length is `strlen(str)`.
- Wraps: `yyjson_read_opts()` on the `strlen`-derived range.
- Ownership: as `kimix_yyjson_read`; free the document with `kimix_yyjson_doc_free()`.
- Errors: `NULL` on failure.
- Notes: `KIMIX_YYJSON_READ_INSITU` is dropped here (a plain C string is not guaranteed to be padded — yyjson's own `yyjson_read` does the same).

#### `void kimix_yyjson_doc_free(KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_doc *doc)`
Releases a document and every value it owns.
- Wraps: `yyjson_doc_free()` — the frees go to the mimalloc heap recorded in the document.
- Ownership: after this every `kimix_yyjson_val *` from it is dangling. NULL is ignored.
- Errors: none.

#### `kimix_yyjson_val *kimix_yyjson_doc_root(KIMIX_BORROWED const kimix_yyjson_doc *doc)`
The root value of a parsed document.
- Wraps: `yyjson_doc_get_root()`
- Ownership: BORROWED from `doc`; never free it on its own. NULL if `doc` is NULL.
- Errors: none.

#### `size_t kimix_yyjson_doc_read_size(KIMIX_IN const kimix_yyjson_doc *doc)`
The number of input bytes the document was read from.
- Wraps: `yyjson_doc_get_read_size()`
- Ownership: nothing to free.
- Errors: `0` for a NULL document.

#### `size_t kimix_yyjson_doc_val_count(KIMIX_IN const kimix_yyjson_doc *doc)`
The number of values stored in the document.
- Wraps: `yyjson_doc_get_val_count()`
- Ownership: nothing to free.
- Errors: `0` for a NULL document.

#### `kimix_yyjson_mut_doc *kimix_yyjson_doc_mut_copy(KIMIX_IN const kimix_yyjson_doc *doc)`
Deep-copies an immutable document into a mutable one.
- Wraps: `yyjson_doc_mut_copy()` with the mimalloc allocator.
- Ownership: a new document owned by the caller — free with `kimix_yyjson_mut_doc_free()`. Strings are copied into the new document's pool, so the source may be freed right afterwards.
- Errors: `NULL` for a NULL input, a document without a root, or an allocation failure.

### 2. Immutable value: type checks

Read-only predicates; each returns `false` when `v` is NULL or the value has
another type, and none of them converts.

#### `bool kimix_yyjson_is_raw(KIMIX_IN const kimix_yyjson_val *v)`
True for a `KIMIX_YYJSON_TYPE_RAW` value (a number kept as its text).
- Wraps: `yyjson_is_raw()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_is_null(KIMIX_IN const kimix_yyjson_val *v)`
True for the `null` literal.
- Wraps: `yyjson_is_null()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_is_true(KIMIX_IN const kimix_yyjson_val *v)`
True for the `true` literal (a bool subtype test).
- Wraps: `yyjson_is_true()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_is_false(KIMIX_IN const kimix_yyjson_val *v)`
True for the `false` literal (the other bool subtype test).
- Wraps: `yyjson_is_false()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_is_bool(KIMIX_IN const kimix_yyjson_val *v)`
True for either bool.
- Wraps: `yyjson_is_bool()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_is_uint(KIMIX_IN const kimix_yyjson_val *v)`
True for a non-negative integer stored as `uint64_t`.
- Wraps: `yyjson_is_uint()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_is_sint(KIMIX_IN const kimix_yyjson_val *v)`
True for a negative integer stored as `int64_t`.
- Wraps: `yyjson_is_sint()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_is_int(KIMIX_IN const kimix_yyjson_val *v)`
True for uint **or** sint — the fork's integer test.
- Wraps: `yyjson_is_int()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_is_real(KIMIX_IN const kimix_yyjson_val *v)`
True for a floating-point number.
- Wraps: `yyjson_is_real()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_is_num(KIMIX_IN const kimix_yyjson_val *v)`
True for any number subtype (uint, sint or real).
- Wraps: `yyjson_is_num()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_is_str(KIMIX_IN const kimix_yyjson_val *v)`
True for a string.
- Wraps: `yyjson_is_str()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_is_arr(KIMIX_IN const kimix_yyjson_val *v)`
True for an array.
- Wraps: `yyjson_is_arr()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_is_obj(KIMIX_IN const kimix_yyjson_val *v)`
True for an object.
- Wraps: `yyjson_is_obj()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_is_ctn(KIMIX_IN const kimix_yyjson_val *v)`
True for a container: array or object.
- Wraps: `yyjson_is_ctn()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `uint8_t kimix_yyjson_get_type(KIMIX_IN const kimix_yyjson_val *v)`
The value's type as one of the `KIMIX_YYJSON_TYPE_*` macros (the 3 type bits of the tag).
- Wraps: `yyjson_get_type()`
- Ownership: nothing to free.
- Errors: `KIMIX_YYJSON_TYPE_NONE` (0) for a NULL `v` — "not a value".

#### `const char *kimix_yyjson_get_type_desc(KIMIX_IN const kimix_yyjson_val *v)`
A static description: `"raw"`, `"null"`, `"string"`, `"array"`, `"object"`, `"true"`, `"false"`, `"uint"`, `"sint"`, `"real"` or `"unknown"`.
- Wraps: `yyjson_get_type_desc()`
- Ownership: BORROWED from the library's static storage — never free it; unlike the other borrowed pointers it outlives the document.
- Errors: `NULL` for a NULL `v`.

### 3. Immutable value: content getters

All read-only, all NULL-safe, and **none converts**: check with the section 2
predicate first, otherwise you get the neutral value below.

#### `bool kimix_yyjson_get_bool(KIMIX_IN const kimix_yyjson_val *v)`
The value as true/false.
- Wraps: `yyjson_get_bool()`
- Ownership: nothing to free.
- Errors: `false` for NULL or a non-bool (so `false` does not prove the value was `false` — use `kimix_yyjson_is_false`).

#### `uint64_t kimix_yyjson_get_uint(KIMIX_IN const kimix_yyjson_val *v)`
The value as an unsigned integer.
- Wraps: `yyjson_get_uint()`
- Ownership: nothing to free.
- Errors: `0` for NULL or a non-uint.

#### `int64_t kimix_yyjson_get_sint(KIMIX_IN const kimix_yyjson_val *v)`
The value as a signed integer.
- Wraps: `yyjson_get_sint()`
- Ownership: nothing to free.
- Errors: `0` for NULL or a non-sint.

#### `int32_t kimix_yyjson_get_int(KIMIX_IN const kimix_yyjson_val *v)`
The value as an integer truncated to 32 bits.
- Wraps: `yyjson_get_int()` — which the fork declares as plain `int`; `ffi_yyjson.cpp` static_asserts `int` is 32 bits on every supported platform, so the cast does not change the width.
- Ownership: nothing to free.
- Errors: `0` for NULL or a non-integer.

#### `double kimix_yyjson_get_real(KIMIX_IN const kimix_yyjson_val *v)`
The value as a double, only when it really is a real.
- Wraps: `yyjson_get_real()`
- Ownership: nothing to free.
- Errors: `0.0` for NULL or a non-real.

#### `double kimix_yyjson_get_num(KIMIX_IN const kimix_yyjson_val *v)`
Any number as a double (uint, sint and real are all converted).
- Wraps: `yyjson_get_num()`
- Ownership: nothing to free.
- Errors: `0.0` for NULL or a non-number.
- Notes: a `uint64` too large for a double loses precision, exactly as in yyjson.

#### `const char *kimix_yyjson_get_str(KIMIX_BORROWED const kimix_yyjson_val *v)`
The string content as NUL-terminated UTF-8.
- Wraps: `yyjson_get_str()`
- Ownership: BORROWED from the document — valid until `kimix_yyjson_doc_free()`, and no longer than `kimix_yyjson_get_len()` says.
- Errors: `NULL` for NULL or a non-string.
- Notes: an embedded NUL truncates C-string handling; use `kimix_yyjson_get_len` / `kimix_yyjson_equals_strn` for data that may contain one.

#### `size_t kimix_yyjson_get_len(KIMIX_IN const kimix_yyjson_val *v)`
The byte length of a string, raw or container value.
- Wraps: `yyjson_get_len()`
- Ownership: nothing to free.
- Errors: `0` for NULL or a type without a length.

#### `const char *kimix_yyjson_get_raw(KIMIX_BORROWED const kimix_yyjson_val *v)`
The literal text of a `KIMIX_YYJSON_TYPE_RAW` value.
- Wraps: `yyjson_get_raw()`
- Ownership: NUL-terminated, BORROWED from the document (it usually points into the input buffer of `kimix_yyjson_read`).
- Errors: `NULL` for NULL or a non-raw value.

#### `bool kimix_yyjson_equals_str(KIMIX_IN const kimix_yyjson_val *v, KIMIX_IN const char *str)`
Is the value a string equal to the NUL-terminated `str`?
- Wraps: `yyjson_equals_str()`
- Ownership: both arguments are read only.
- Errors: `false` for a NULL argument.

#### `bool kimix_yyjson_equals_strn(KIMIX_IN const kimix_yyjson_val *v, KIMIX_IN const char *str, size_t len)`
Is the value a string equal to the `str` / `len` bytes (no terminator required)?
- Wraps: `yyjson_equals_strn()`
- Ownership: both arguments are read only.
- Errors: `false` for a NULL value or a NULL `str`.

### 4. Immutable value: array access

#### `size_t kimix_yyjson_arr_size(KIMIX_IN const kimix_yyjson_val *arr)`
The number of elements.
- Wraps: `yyjson_arr_size()`
- Ownership: nothing to free.
- Errors: `0` for NULL or a non-array.

#### `kimix_yyjson_val *kimix_yyjson_arr_get(KIMIX_BORROWED const kimix_yyjson_val *arr, size_t idx)`
The element at `idx`.
- Wraps: `yyjson_arr_get()`
- Ownership: BORROWED from the document.
- Errors: `NULL` when `idx` is out of range, `arr` is NULL, or `arr` is not an array.
- Notes: O(1) for a "flat" array (all elements inline in the parent's block), O(idx) otherwise — sequential traversal through this accessor is quadratic for such arrays.

#### `kimix_yyjson_val *kimix_yyjson_arr_get_first(KIMIX_BORROWED const kimix_yyjson_val *arr)`
The first element.
- Wraps: `yyjson_arr_get_first()`
- Ownership: BORROWED from the document.
- Errors: `NULL` for an empty array, a NULL `arr` or a non-array.

#### `kimix_yyjson_val *kimix_yyjson_arr_get_last(KIMIX_BORROWED const kimix_yyjson_val *arr)`
The last element.
- Wraps: `yyjson_arr_get_last()`
- Ownership: BORROWED from the document.
- Errors: `NULL` for an empty array, a NULL `arr` or a non-array.

### 5. Immutable value: object access

#### `size_t kimix_yyjson_obj_size(KIMIX_IN const kimix_yyjson_val *obj)`
The number of key-value pairs.
- Wraps: `yyjson_obj_size()`
- Ownership: nothing to free.
- Errors: `0` for NULL or a non-object.

#### `kimix_yyjson_val *kimix_yyjson_obj_get(KIMIX_BORROWED const kimix_yyjson_val *obj, KIMIX_IN const char *key)`
The value stored under the NUL-terminated `key`.
- Wraps: `yyjson_obj_get()` — a hash lookup.
- Ownership: BORROWED from the document.
- Errors: `NULL` when the key is missing, `obj` is NULL or `obj` is not an object.

#### `kimix_yyjson_val *kimix_yyjson_obj_getn(KIMIX_BORROWED const kimix_yyjson_val *obj, KIMIX_IN const char *key, size_t key_len)`
The value stored under the exact key `key` / `key_len` (no NUL terminator required).
- Wraps: `yyjson_obj_getn()`
- Ownership: BORROWED from the document.
- Errors: as `kimix_yyjson_obj_get`.

### 6. Writing an immutable document / value

#### `char *kimix_yyjson_write(KIMIX_IN const kimix_yyjson_doc *doc, uint32_t flags, KIMIX_OUT KIMIX_NULLABLE size_t *len, KIMIX_OUT KIMIX_NULLABLE kimix_yyjson_write_err *err)`
Serialises a whole document into a freshly allocated, NUL-terminated UTF-8 string.
- Wraps: `yyjson_write_opts()` with the library's mimalloc allocator — deliberately not the `yyjson_write()` convenience form, which allocates with libc malloc and would be freed in the wrong heap.
- Ownership: the string is TRANSFERred to the caller and **must** be released with `kimix_yyjson_str_free()`. `len` (nullable) is set to 0, then written with the byte length excluding the NUL on success; `err` (nullable) is always written.
- Errors: `NULL` when `doc` is NULL, has no root, or the write failed (unserializable value type, NaN/Infinity without the matching flag, allocation failure) — see `err->code`.
- Notes: `flags` is 0 or a combination of the `KIMIX_YYJSON_WRITE_*` macros.

#### `char *kimix_yyjson_val_write(KIMIX_IN const kimix_yyjson_val *v, uint32_t flags, KIMIX_OUT KIMIX_NULLABLE size_t *len, KIMIX_OUT KIMIX_NULLABLE kimix_yyjson_write_err *err)`
Serialises ONE value of an immutable document.
- Wraps: `yyjson_val_write_opts()` with the mimalloc allocator.
- Ownership: same TRANSFER rule — free the result with `kimix_yyjson_str_free()`.
- Errors: `NULL` for a NULL `v`, or on a write failure (`err->code`).

#### `size_t kimix_yyjson_write_buf(KIMIX_OUT char *buf, size_t buf_len, KIMIX_IN const kimix_yyjson_doc *doc, uint32_t flags, KIMIX_OUT KIMIX_NULLABLE kimix_yyjson_write_err *err)`
Serialises a document into the caller's own buffer: no allocation at all, nothing to free.
- Wraps: `yyjson_write_buf()`
- Ownership: `buf` / `buf_len` is the caller's output window; a successful call NUL-terminates the result.
- Errors: returns the number of bytes written (excluding the NUL), or `0` when `buf` or `doc` is NULL, the buffer is too small, or the document cannot be serialized — on failure nothing in `buf` is meaningful.
- Notes: the buffer must be LARGER than the final JSON size, because yyjson reserves temporary space while writing.

#### `void kimix_yyjson_str_free(KIMIX_TRANSFER KIMIX_NULLABLE char *s)`
Releases a string returned by `kimix_yyjson_write()`, `kimix_yyjson_val_write()`, `kimix_yyjson_mut_write()` or `kimix_yyjson_mut_val_write()`.
- Wraps: `mi_free(s)`
- Ownership: such a buffer is mimalloc memory of this library's heap: it must not be released with the caller's `free()` nor through a `kimix_mem_free()` resolved in another module. NULL is ignored.
- Errors: none.

### 7. Mutable document: lifecycle

#### `kimix_yyjson_mut_doc *kimix_yyjson_mut_doc_new(void)`
Creates an empty mutable document (no root yet).
- Wraps: `yyjson_mut_doc_new()` with the library's mimalloc allocator.
- Ownership: TRANSFERred to the caller — release with `kimix_yyjson_mut_doc_free()`. Every creator and mutator of sections 8–11 needs this pointer: the values it makes belong to the document, never to the caller.
- Errors: `NULL` when the allocation failed.

#### `void kimix_yyjson_mut_doc_free(KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_doc *doc)`
Releases a mutable document and all of its values.
- Wraps: `yyjson_mut_doc_free()`
- Ownership: every `kimix_yyjson_mut_val *` from it becomes dangling. NULL is ignored.
- Errors: none.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_doc_root(KIMIX_IN_OUT KIMIX_NULLABLE kimix_yyjson_mut_doc *doc)`
The root value of a mutable document.
- Wraps: `yyjson_mut_doc_get_root()`
- Ownership: BORROWED from `doc`.
- Errors: `NULL` when `doc` is NULL or has no root yet.

#### `void kimix_yyjson_mut_doc_set_root(KIMIX_IN_OUT KIMIX_NULLABLE kimix_yyjson_mut_doc *doc, KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *root)`
Sets — or with a NULL `root` clears — the document's root.
- Wraps: `yyjson_mut_doc_set_root()`
- Ownership: `root` must be a value made by the SAME document and not yet attached anywhere else; on success the document owns it. A NULL `doc` does nothing.
- Errors: none — the call cannot fail, so a foreign value corrupts the document exactly as it would in yyjson (yyjson cannot check it either).

#### `kimix_yyjson_doc *kimix_yyjson_mut_doc_imut_copy(KIMIX_IN const kimix_yyjson_mut_doc *doc)`
Deep-copies a mutable document into a new immutable one — the usual last step before using the section 2–6 accessors.
- Wraps: `yyjson_mut_doc_imut_copy()` with the mimalloc allocator.
- Ownership: TRANSFERred — release with `kimix_yyjson_doc_free()`.
- Errors: `NULL` for a NULL input, a document with no root, or an allocation failure.
- Notes: recursive, so a very deep tree can exhaust the stack, exactly as in yyjson.

#### `kimix_yyjson_mut_doc *kimix_yyjson_mut_doc_mut_copy(KIMIX_IN const kimix_yyjson_mut_doc *doc)`
Deep-copies a mutable document into a new mutable one.
- Wraps: `yyjson_mut_doc_mut_copy()` with the mimalloc allocator.
- Ownership: TRANSFERred — release with `kimix_yyjson_mut_doc_free()`.
- Errors: `NULL` for a NULL input, a value too deep for the recursive copy, or an allocation failure. A source with no root yields a NEW EMPTY mutable document, not NULL (the fork's behaviour).

### 8. Mutable value creators

Each takes one node out of `doc`'s pools and returns it **BORROWED** from `doc`:
never free a value by itself — it disappears with `kimix_yyjson_mut_doc_free()`.
Attach it with sections 10–11 or make it the root. A NULL `doc`, or an allocation
that could not be satisfied, yields `NULL`. The document passed in MUST be the one
the value ends up in.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_null(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc)`
Creates a `null` value.
- Wraps: `yyjson_mut_null()`
- Ownership: BORROWED from `doc`.
- Errors: `NULL` for a NULL `doc` or an allocation failure.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_true(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc)`
Creates the `true` literal.
- Wraps: `yyjson_mut_true()`
- Ownership: BORROWED from `doc`.
- Errors: `NULL` for a NULL `doc` or an allocation failure.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_false(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc)`
Creates the `false` literal.
- Wraps: `yyjson_mut_false()`
- Ownership: BORROWED from `doc`.
- Errors: `NULL` for a NULL `doc` or an allocation failure.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_bool(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, bool val)`
Creates `true` or `false` from a 1-byte `bool`.
- Wraps: `yyjson_mut_bool()`
- Ownership: BORROWED from `doc`.
- Errors: `NULL` for a NULL `doc` or an allocation failure.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_uint(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, uint64_t num)`
Creates an unsigned integer value.
- Wraps: `yyjson_mut_uint()`
- Ownership: BORROWED from `doc`.
- Errors: `NULL` for a NULL `doc` or an allocation failure.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_sint(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, int64_t num)`
Creates a signed integer value.
- Wraps: `yyjson_mut_sint()`
- Ownership: BORROWED from `doc`.
- Errors: `NULL` for a NULL `doc` or an allocation failure.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_int(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, int64_t num)`
The signed integer creator taking a wide argument.
- Wraps: `yyjson_mut_int()` — an alias of `yyjson_mut_sint()`.
- Ownership: BORROWED from `doc`.
- Errors: `NULL` for a NULL `doc` or an allocation failure.
- Notes: reading it back with `kimix_yyjson_mut_get_int()` truncates to 32 bits.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_real(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, double num)`
Creates a floating-point value.
- Wraps: `yyjson_mut_real()`
- Ownership: BORROWED from `doc`.
- Errors: `NULL` for a NULL `doc` or an allocation failure.
- Notes: writing a NaN/Infinity needs `KIMIX_YYJSON_WRITE_ALLOW_INF_AND_NAN` (or `..._AS_NULL`).

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_str(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN const char *str)`
Creates a string value that **references** `str`: the pointer and its `strlen` are stored, nothing is copied.
- Wraps: `yyjson_mut_str()`
- Ownership: BORROWED from `doc`, but the bytes stay the CALLER's — keep `str` alive and unmodified for the whole remaining life of `doc`. `str` must be NUL-terminated.
- Errors: `NULL` for a NULL `str` (or NULL `doc` / allocation failure).
- Notes: use `kimix_yyjson_mut_strcpy()` when the text is temporary.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_strn(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN const char *str, size_t len)`
The no-copy form with an explicit byte length.
- Wraps: `yyjson_mut_strn()`
- Ownership: as `kimix_yyjson_mut_str` — the bytes must stay alive and unmodified.
- Errors: `NULL` for a NULL `str` / `doc`, or an allocation failure.
- Notes: a NUL terminator is not required.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_strcpy(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN const char *str)`
Creates a string value and COPIES the text into the document's string pool.
- Wraps: `yyjson_mut_strcpy()`
- Ownership: BORROWED from `doc`; the caller's buffer may be reused or released immediately after the call. `str` must be NUL-terminated.
- Errors: `NULL` for a NULL argument or an allocation failure.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_strncpy(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN const char *str, size_t len)`
The copying form with an explicit byte length (no NUL terminator required).
- Wraps: `yyjson_mut_strncpy()`
- Ownership: BORROWED from `doc`; the bytes are duplicated into the pool.
- Errors: `NULL` for a NULL argument or an allocation failure.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_arr(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc)`
Creates an EMPTY array, ready for section 10.
- Wraps: `yyjson_mut_arr()`
- Ownership: BORROWED from `doc`.
- Errors: `NULL` for a NULL `doc` or an allocation failure.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_obj(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc)`
Creates an EMPTY object, ready for section 11.
- Wraps: `yyjson_mut_obj()`
- Ownership: BORROWED from `doc`.
- Errors: `NULL` for a NULL `doc` or an allocation failure.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_raw(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN const char *str)`
Creates a raw value: `str` is emitted verbatim as JSON text.
- Wraps: `yyjson_mut_raw()`
- Ownership: nothing is copied — the value is BORROWED from `doc` but the text is the caller's, so keep `str` alive for the life of `doc`.
- Errors: `NULL` for a NULL argument or an allocation failure.
- Notes: `str` must already be valid JSON, because the writer does not check it.

### 9. Mutable value: type checks, content getters and lookup

The sections 2–5 surface mirrored for **mutable** values; each wraps the
`yyjson_mut_*` accessor of the same name. Handing a `kimix_yyjson_mut_val` to a
section 2–5 function (or an immutable `kimix_yyjson_val` to one of these) makes
yyjson read the wrong fields of a differently shaped node — a caller bug the FFI
cannot detect. All of them are NULL-safe and return the neutral value.

#### `bool kimix_yyjson_mut_is_raw(KIMIX_IN const kimix_yyjson_mut_val *v)`
True for a raw value.
- Wraps: `yyjson_mut_is_raw()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_mut_is_null(KIMIX_IN const kimix_yyjson_mut_val *v)`
True for `null`.
- Wraps: `yyjson_mut_is_null()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_mut_is_true(KIMIX_IN const kimix_yyjson_mut_val *v)`
True for the `true` literal.
- Wraps: `yyjson_mut_is_true()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_mut_is_false(KIMIX_IN const kimix_yyjson_mut_val *v)`
True for the `false` literal.
- Wraps: `yyjson_mut_is_false()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_mut_is_bool(KIMIX_IN const kimix_yyjson_mut_val *v)`
True for either bool.
- Wraps: `yyjson_mut_is_bool()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_mut_is_uint(KIMIX_IN const kimix_yyjson_mut_val *v)`
True for a uint-subtype number.
- Wraps: `yyjson_mut_is_uint()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_mut_is_sint(KIMIX_IN const kimix_yyjson_mut_val *v)`
True for a sint-subtype number.
- Wraps: `yyjson_mut_is_sint()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_mut_is_int(KIMIX_IN const kimix_yyjson_mut_val *v)`
True for uint **or** sint.
- Wraps: `yyjson_mut_is_int()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_mut_is_real(KIMIX_IN const kimix_yyjson_mut_val *v)`
True for a real.
- Wraps: `yyjson_mut_is_real()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_mut_is_num(KIMIX_IN const kimix_yyjson_mut_val *v)`
True for any number subtype.
- Wraps: `yyjson_mut_is_num()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_mut_is_str(KIMIX_IN const kimix_yyjson_mut_val *v)`
True for a string.
- Wraps: `yyjson_mut_is_str()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_mut_is_arr(KIMIX_IN const kimix_yyjson_mut_val *v)`
True for an array.
- Wraps: `yyjson_mut_is_arr()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_mut_is_obj(KIMIX_IN const kimix_yyjson_mut_val *v)`
True for an object.
- Wraps: `yyjson_mut_is_obj()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `bool kimix_yyjson_mut_is_ctn(KIMIX_IN const kimix_yyjson_mut_val *v)`
True for a container (array or object).
- Wraps: `yyjson_mut_is_ctn()`
- Ownership: reads only.
- Errors: `false` for NULL or a wrong type.

#### `uint8_t kimix_yyjson_mut_get_type(KIMIX_IN const kimix_yyjson_mut_val *v)`
The value's type as a `KIMIX_YYJSON_TYPE_*` macro.
- Wraps: `yyjson_mut_get_type()`
- Ownership: nothing to free.
- Errors: `KIMIX_YYJSON_TYPE_NONE` (0) for NULL.

#### `const char *kimix_yyjson_mut_get_type_desc(KIMIX_IN const kimix_yyjson_mut_val *v)`
A static description string of the type.
- Wraps: `yyjson_mut_get_type_desc()`
- Ownership: static storage — never free it.
- Errors: `NULL` for a NULL `v`.

#### `bool kimix_yyjson_mut_get_bool(KIMIX_IN const kimix_yyjson_mut_val *v)`
The value as true/false.
- Wraps: `yyjson_mut_get_bool()`
- Ownership: nothing to free.
- Errors: `false` for NULL or a non-bool.

#### `uint64_t kimix_yyjson_mut_get_uint(KIMIX_IN const kimix_yyjson_mut_val *v)`
The value as an unsigned integer.
- Wraps: `yyjson_mut_get_uint()`
- Ownership: nothing to free.
- Errors: `0` for NULL or a non-uint.

#### `int64_t kimix_yyjson_mut_get_sint(KIMIX_IN const kimix_yyjson_mut_val *v)`
The value as a signed integer.
- Wraps: `yyjson_mut_get_sint()`
- Ownership: nothing to free.
- Errors: `0` for NULL or a non-sint.

#### `int32_t kimix_yyjson_mut_get_int(KIMIX_IN const kimix_yyjson_mut_val *v)`
The value truncated to 32 bits.
- Wraps: `yyjson_mut_get_int()`
- Ownership: nothing to free.
- Errors: `0` for NULL or a non-integer.

#### `double kimix_yyjson_mut_get_real(KIMIX_IN const kimix_yyjson_mut_val *v)`
The value as a double, only for a real.
- Wraps: `yyjson_mut_get_real()`
- Ownership: nothing to free.
- Errors: `0.0` for NULL or a non-real.

#### `double kimix_yyjson_mut_get_num(KIMIX_IN const kimix_yyjson_mut_val *v)`
Any number as a double.
- Wraps: `yyjson_mut_get_num()`
- Ownership: nothing to free.
- Errors: `0.0` for NULL or a non-number.

#### `const char *kimix_yyjson_mut_get_str(KIMIX_BORROWED const kimix_yyjson_mut_val *v)`
The string content.
- Wraps: `yyjson_mut_get_str()`
- Ownership: BORROWED from the document — and with the no-copy creators (`kimix_yyjson_mut_str` / `_strn` / `kimix_yyjson_mut_raw`) those bytes are the CALLER's, so the pointer is only as valid as the buffer handed to the creator.
- Errors: `NULL` for NULL or a non-string.

#### `size_t kimix_yyjson_mut_get_len(KIMIX_IN const kimix_yyjson_mut_val *v)`
The byte length of a string, raw or container value.
- Wraps: `yyjson_mut_get_len()`
- Ownership: nothing to free.
- Errors: `0` for NULL or a type without a length.

#### `const char *kimix_yyjson_mut_get_raw(KIMIX_BORROWED const kimix_yyjson_mut_val *v)`
The literal text of a raw value.
- Wraps: `yyjson_mut_get_raw()`
- Ownership: BORROWED (caller-owned bytes when created by `kimix_yyjson_mut_raw`).
- Errors: `NULL` for NULL or a non-raw value.

#### `bool kimix_yyjson_mut_equals_str(KIMIX_IN const kimix_yyjson_mut_val *v, KIMIX_IN const char *str)`
Is the value a string equal to the NUL-terminated `str`?
- Wraps: `yyjson_mut_equals_str()`
- Ownership: both arguments read only.
- Errors: `false` for a NULL argument.

#### `bool kimix_yyjson_mut_equals_strn(KIMIX_IN const kimix_yyjson_mut_val *v, KIMIX_IN const char *str, size_t len)`
Is the value a string equal to the `str` / `len` bytes?
- Wraps: `yyjson_mut_equals_strn()`
- Ownership: both arguments read only.
- Errors: `false` for a NULL value or a NULL `str`.

#### `size_t kimix_yyjson_mut_arr_size(KIMIX_IN const kimix_yyjson_mut_val *arr)`
The number of elements of a mutable array.
- Wraps: `yyjson_mut_arr_size()`
- Ownership: nothing to free.
- Errors: `0` for NULL or a non-array.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_arr_get(KIMIX_BORROWED const kimix_yyjson_mut_val *arr, size_t idx)`
The element at `idx` of a mutable array.
- Wraps: `yyjson_mut_arr_get()`
- Ownership: BORROWED from the document.
- Errors: `NULL` out of range, for NULL, or for a non-array.
- Notes: a mutable array is a linked chain, so this walks it — O(idx) with no flat fast path (unlike the immutable accessor).

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_arr_get_first(KIMIX_BORROWED const kimix_yyjson_mut_val *arr)`
The first element of a mutable array.
- Wraps: `yyjson_mut_arr_get_first()`
- Ownership: BORROWED from the document.
- Errors: `NULL` for an empty array, NULL, or a non-array.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_arr_get_last(KIMIX_BORROWED const kimix_yyjson_mut_val *arr)`
The last element of a mutable array.
- Wraps: `yyjson_mut_arr_get_last()`
- Ownership: BORROWED from the document.
- Errors: `NULL` for an empty array, NULL, or a non-array.

#### `size_t kimix_yyjson_mut_obj_size(KIMIX_IN const kimix_yyjson_mut_val *obj)`
The number of pairs of a mutable object.
- Wraps: `yyjson_mut_obj_size()`
- Ownership: nothing to free.
- Errors: `0` for NULL or a non-object.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_obj_get(KIMIX_BORROWED const kimix_yyjson_mut_val *obj, KIMIX_IN const char *key)`
The value under the NUL-terminated `key` in a mutable object.
- Wraps: `yyjson_mut_obj_get()`
- Ownership: BORROWED from the document.
- Errors: `NULL` when the key is missing, for NULL, or for a non-object.
- Notes: a mutable object is an unordered bucket list — the lookup is a linear scan, O(obj_size), and a duplicate key (possible with `kimix_yyjson_mut_obj_add()`) returns the first match.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_obj_getn(KIMIX_BORROWED const kimix_yyjson_mut_val *obj, KIMIX_IN const char *key, size_t key_len)`
The value under the exact key `key` / `key_len` (no NUL terminator required).
- Wraps: `yyjson_mut_obj_getn()`
- Ownership: BORROWED from the document.
- Errors: as `kimix_yyjson_mut_obj_get`.

### 10. Mutable array mutation

The plain mutators take `val` as TRANSFER: on success the array owns it and it
must not be freed or attached a second time; on failure (NULL argument, `arr` not
an array, out-of-range index) nothing is inserted and the caller still owns it.
The `add_*` sugar takes `doc` because it creates the element for you — it must be
the document that owns `arr`. Each wraps its same-named `yyjson_mut_arr_*()`.

#### `bool kimix_yyjson_mut_arr_append(KIMIX_IN_OUT kimix_yyjson_mut_val *arr, KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *val)`
Appends `val` at the end of the array.
- Wraps: `yyjson_mut_arr_append()`
- Ownership: on success the array owns `val`.
- Errors: `false` when nothing was inserted (NULL arguments, `arr` not an array); `val` stays the caller's.

#### `bool kimix_yyjson_mut_arr_prepend(KIMIX_IN_OUT kimix_yyjson_mut_val *arr, KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *val)`
Inserts `val` at the front of the array.
- Wraps: `yyjson_mut_arr_prepend()`
- Ownership: on success the array owns `val`.
- Errors: `false` as above.

#### `bool kimix_yyjson_mut_arr_insert(KIMIX_IN_OUT kimix_yyjson_mut_val *arr, KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *val, size_t idx)`
Inserts `val` at position `idx`.
- Wraps: `yyjson_mut_arr_insert()`
- Ownership: on success the array owns `val`.
- Errors: `false` for a NULL argument, a non-array or an out-of-range index.
- Notes: a valid `idx` is `0..size` (`idx == size` appends).

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_arr_remove(KIMIX_IN_OUT kimix_yyjson_mut_val *arr, size_t idx)`
Takes the element at `idx` out of the array.
- Wraps: `yyjson_mut_arr_remove()` — the fork returns the node that left the array, not a bool, so this does too.
- Ownership: the result is BORROWED from the document: it may be re-inserted elsewhere or ignored, and it is never freed alone.
- Errors: `NULL` when nothing was removed.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_arr_remove_first(KIMIX_IN_OUT kimix_yyjson_mut_val *arr)`
Removes and returns the first element.
- Wraps: `yyjson_mut_arr_remove_first()`
- Ownership: BORROWED from the document, as `kimix_yyjson_mut_arr_remove`.
- Errors: `NULL` on an empty array or bad arguments.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_arr_remove_last(KIMIX_IN_OUT kimix_yyjson_mut_val *arr)`
Removes and returns the last element.
- Wraps: `yyjson_mut_arr_remove_last()`
- Ownership: BORROWED from the document.
- Errors: `NULL` on an empty array or bad arguments.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_arr_replace(KIMIX_IN_OUT kimix_yyjson_mut_val *arr, size_t idx, KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *val)`
Puts `val` at `idx` and returns the element it displaced.
- Wraps: `yyjson_mut_arr_replace()`
- Ownership: on success the array owns `val`; the returned node is BORROWED from the document.
- Errors: `NULL` for a NULL argument, a non-array or an out-of-range index — the array is then unchanged and `val` is still owned by the caller.

#### `bool kimix_yyjson_mut_arr_clear(KIMIX_IN_OUT kimix_yyjson_mut_val *arr)`
Drops all elements of the array.
- Wraps: `yyjson_mut_arr_clear()`
- Ownership: the nodes stay in the document's pool and get reused.
- Errors: `false` for a NULL `arr` or a non-array.

#### `bool kimix_yyjson_mut_arr_add_null(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *arr)`
Creates a `null` and appends it.
- Wraps: `yyjson_mut_arr_add_null()`
- Ownership: `doc` (which must be the owner of `arr`) provides the node.
- Errors: `false` when nothing was appended (bad arguments or an allocation failure).

#### `bool kimix_yyjson_mut_arr_add_bool(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *arr, bool val)`
Creates a bool from `val` and appends it.
- Wraps: `yyjson_mut_arr_add_bool()`
- Ownership: the node comes from `doc`.
- Errors: `false` on bad arguments or allocation failure.

#### `bool kimix_yyjson_mut_arr_add_uint(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *arr, uint64_t num)`
Creates an unsigned integer and appends it.
- Wraps: `yyjson_mut_arr_add_uint()`
- Ownership: the node comes from `doc`.
- Errors: `false` on bad arguments or allocation failure.

#### `bool kimix_yyjson_mut_arr_add_sint(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *arr, int64_t num)`
Creates a signed integer and appends it.
- Wraps: `yyjson_mut_arr_add_sint()`
- Ownership: the node comes from `doc`.
- Errors: `false` on bad arguments or allocation failure.

#### `bool kimix_yyjson_mut_arr_add_real(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *arr, double num)`
Creates a real and appends it.
- Wraps: `yyjson_mut_arr_add_real()`
- Ownership: the node comes from `doc`.
- Errors: `false` on bad arguments or allocation failure.

#### `bool kimix_yyjson_mut_arr_add_str(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *arr, KIMIX_IN const char *str)`
Appends a string **without copying it**.
- Wraps: `yyjson_mut_arr_add_str()`
- Ownership: the bytes must stay alive and unmodified for the life of `doc`.
- Errors: `false` on bad arguments or allocation failure.

#### `bool kimix_yyjson_mut_arr_add_strn(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *arr, KIMIX_IN const char *str, size_t len)`
Appends a string of an explicit byte length, still without copying.
- Wraps: `yyjson_mut_arr_add_strn()`
- Ownership: the bytes must stay alive and unmodified for the life of `doc`.
- Errors: `false` on bad arguments or allocation failure.

#### `bool kimix_yyjson_mut_arr_add_strcpy(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *arr, KIMIX_IN const char *str)`
Appends a string and COPIES it into the document's string pool.
- Wraps: `yyjson_mut_arr_add_strcpy()`
- Ownership: the caller's buffer may be reused or released right after the call.
- Errors: `false` on bad arguments or allocation failure.

#### `bool kimix_yyjson_mut_arr_add_val(KIMIX_IN_OUT kimix_yyjson_mut_val *arr, KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *val)`
Appends an existing value.
- Wraps: `yyjson_mut_arr_add_val()` — an alias of `yyjson_mut_arr_append()`.
- Ownership: the array takes ownership of `val` on success.
- Errors: `false` on bad arguments; `val` stays the caller's.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_arr_add_arr(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *arr)`
Appends a NEW empty array and returns it, so a nested container can be filled without a create-then-append pair.
- Wraps: `yyjson_mut_arr_add_arr()`
- Ownership: BORROWED from `doc`.
- Errors: `NULL` on failure.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_arr_add_obj(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *arr)`
Appends a NEW empty object under `arr` and returns it.
- Wraps: `yyjson_mut_arr_add_obj()`
- Ownership: BORROWED from `doc`.
- Errors: `NULL` on failure.

### 11. Mutable object mutation

In the two-value form `key` must be a string value made by THIS document's
creators (section 8); `val` becomes owned by the document on success. Each wraps
its same-named `yyjson_mut_obj_*()`.

#### `bool kimix_yyjson_mut_obj_add(KIMIX_IN_OUT kimix_yyjson_mut_val *obj, KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *key, KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *val)`
Appends a key-value pair **without de-duplicating**.
- Wraps: `yyjson_mut_obj_add()`
- Ownership: on success the object owns `key` and `val`.
- Errors: `false` when `obj` is not an object, `key` is not a string value, `val` is NULL, or the document could not allocate.
- Notes: the object may then hold two pairs with the same key; a lookup returns the first one. Use `kimix_yyjson_mut_obj_put()` for unique keys.

#### `bool kimix_yyjson_mut_obj_put(KIMIX_IN_OUT kimix_yyjson_mut_val *obj, KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *key, KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *val)`
Sets a key-value pair, removing every existing pair with the same key first.
- Wraps: `yyjson_mut_obj_put()`
- Ownership: on success the object owns `key` and `val`.
- Errors: `false` on bad arguments or allocation failure. A NULL `val` deletes the key, like `kimix_yyjson_mut_obj_remove()`.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_obj_remove(KIMIX_IN_OUT kimix_yyjson_mut_val *obj, KIMIX_IN const char *key)`
Removes every pair with the NUL-terminated `key` and returns the FIRST value removed.
- Wraps: `yyjson_mut_obj_remove_key()` — note: NOT the fork's `yyjson_mut_obj_remove()`, which takes a key *value* instead of a C string.
- Ownership: the returned value is BORROWED from the document; NULL means the key was not there.
- Errors: `NULL` when nothing was removed (missing key, NULL arguments, non-object).
- Notes: a linear scan.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_obj_remove_keyn(KIMIX_IN_OUT kimix_yyjson_mut_val *obj, KIMIX_IN const char *key, size_t key_len)`
The same with an explicit key length (no NUL terminator required).
- Wraps: `yyjson_mut_obj_remove_keyn()`
- Ownership: as `kimix_yyjson_mut_obj_remove`.
- Errors: `NULL` when nothing was removed.

#### `bool kimix_yyjson_mut_obj_clear(KIMIX_IN_OUT kimix_yyjson_mut_val *obj)`
Drops all pairs of the object.
- Wraps: `yyjson_mut_obj_clear()`
- Ownership: the nodes stay in the document's pool.
- Errors: `false` for a NULL `obj` or a non-object.

#### `bool kimix_yyjson_mut_obj_rename_key(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *obj, KIMIX_IN const char *key, KIMIX_IN const char *new_key)`
Renames an existing key in place: the pair keeps its position, only the key's text changes.
- Wraps: `yyjson_mut_obj_rename_key()`
- Ownership: `new_key` is COPIED into the document's string pool.
- Errors: `false` when `obj` is not an object, `key` is missing, or the copy failed.

#### `bool kimix_yyjson_mut_obj_add_null(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *obj, KIMIX_IN const char *key)`
Adds a freshly created `null` pair under `key`.
- Wraps: `yyjson_mut_obj_add_null()`
- Ownership: `doc` must own `obj`; the key is created by yyjson from `key` and is NOT copied, so `key` must stay alive for the life of `doc`.
- Errors: `false` on bad arguments or allocation failure.

#### `bool kimix_yyjson_mut_obj_add_bool(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *obj, KIMIX_IN const char *key, bool val)`
Adds a freshly created bool pair.
- Wraps: `yyjson_mut_obj_add_bool()`
- Ownership: the key is not copied — keep `key` alive for the life of `doc`.
- Errors: `false` on bad arguments or allocation failure.

#### `bool kimix_yyjson_mut_obj_add_uint(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *obj, KIMIX_IN const char *key, uint64_t val)`
Adds an unsigned integer pair.
- Wraps: `yyjson_mut_obj_add_uint()`
- Ownership: the key is not copied.
- Errors: `false` on bad arguments or allocation failure.

#### `bool kimix_yyjson_mut_obj_add_sint(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *obj, KIMIX_IN const char *key, int64_t val)`
Adds a signed integer pair.
- Wraps: `yyjson_mut_obj_add_sint()`
- Ownership: the key is not copied.
- Errors: `false` on bad arguments or allocation failure.

#### `bool kimix_yyjson_mut_obj_add_real(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *obj, KIMIX_IN const char *key, double val)`
Adds a real pair.
- Wraps: `yyjson_mut_obj_add_real()`
- Ownership: the key is not copied.
- Errors: `false` on bad arguments or allocation failure.

#### `bool kimix_yyjson_mut_obj_add_str(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *obj, KIMIX_IN const char *key, KIMIX_IN const char *val)`
Adds a string pair with the value bytes NOT copied.
- Wraps: `yyjson_mut_obj_add_str()`
- Ownership: both `key` and `val` must stay alive for the life of `doc`.
- Errors: `false` on bad arguments or allocation failure.

#### `bool kimix_yyjson_mut_obj_add_strn(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *obj, KIMIX_IN const char *key, KIMIX_IN const char *val, size_t len)`
The same with an explicit value length, still no copy.
- Wraps: `yyjson_mut_obj_add_strn()`
- Ownership: `key` and `val` must stay alive for the life of `doc`.
- Errors: `false` on bad arguments or allocation failure.

#### `bool kimix_yyjson_mut_obj_add_strcpy(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *obj, KIMIX_IN const char *key, KIMIX_IN const char *val)`
Adds a string pair, COPYING the value into the document's pool.
- Wraps: `yyjson_mut_obj_add_strcpy()`
- Ownership: the key is still not copied; the value buffer may be reused immediately.
- Errors: `false` on bad arguments or allocation failure.

#### `bool kimix_yyjson_mut_obj_add_val(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *obj, KIMIX_IN const char *key, KIMIX_TRANSFER KIMIX_NULLABLE kimix_yyjson_mut_val *val)`
Attaches an existing value under `key`.
- Wraps: `yyjson_mut_obj_add_val()`
- Ownership: the key is created by yyjson (not copied) and `val` becomes owned by `doc` on success.
- Errors: `false` on bad arguments or allocation failure.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_obj_add_arr(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *obj, KIMIX_IN const char *key)`
Adds a NEW empty array under `key` and returns it.
- Wraps: `yyjson_mut_obj_add_arr()`
- Ownership: BORROWED from `doc`; the key is not copied.
- Errors: `NULL` on failure.

#### `kimix_yyjson_mut_val *kimix_yyjson_mut_obj_add_obj(KIMIX_IN_OUT kimix_yyjson_mut_doc *doc, KIMIX_IN_OUT kimix_yyjson_mut_val *obj, KIMIX_IN const char *key)`
Adds a NEW empty object under `key` and returns it.
- Wraps: `yyjson_mut_obj_add_obj()`
- Ownership: BORROWED from `doc`; the key is not copied.
- Errors: `NULL` on failure.

### 12. Writing a mutable document / value

#### `char *kimix_yyjson_mut_write(KIMIX_IN const kimix_yyjson_mut_doc *doc, uint32_t flags, KIMIX_OUT KIMIX_NULLABLE size_t *len, KIMIX_OUT KIMIX_NULLABLE kimix_yyjson_write_err *err)`
Serialises a mutable document into a freshly allocated string.
- Wraps: `yyjson_mut_write_opts()` with the library's mimalloc allocator — never the libc-allocating `yyjson_mut_write()` convenience form.
- Ownership: exactly as `kimix_yyjson_write()`: the string is TRANSFERred and must be released with `kimix_yyjson_str_free()`.
- Errors: `NULL` on failure (no root, unserializable value, NaN/Infinity without the flag, allocation failure); `err->code` says which.

#### `size_t kimix_yyjson_mut_write_buf(KIMIX_OUT char *buf, size_t buf_len, KIMIX_IN const kimix_yyjson_mut_doc *doc, uint32_t flags, KIMIX_OUT KIMIX_NULLABLE kimix_yyjson_write_err *err)`
The allocation-free variant into the caller's buffer.
- Wraps: `yyjson_mut_write_buf()`
- Ownership: nothing to free; `buf` is the caller's.
- Errors: returns the bytes written, or `0` on any failure.
- Notes: `buf` must be larger than the final JSON size — see `kimix_yyjson_write_buf()`.

#### `char *kimix_yyjson_mut_val_write(KIMIX_IN const kimix_yyjson_mut_val *v, uint32_t flags, KIMIX_OUT KIMIX_NULLABLE size_t *len, KIMIX_OUT KIMIX_NULLABLE kimix_yyjson_write_err *err)`
Serialises one value of a mutable document.
- Wraps: `yyjson_mut_val_write_opts()` with the mimalloc allocator.
- Ownership: TRANSFERred — free the result with `kimix_yyjson_str_free()`.
- Errors: `NULL` for a NULL `v` or on failure.

---

## JSON repair — `<api/ffi_repair.h>` (`kimix_repair*`, `kimix_json_*`)

`kimix::repair()` (`src/core/json_repair.h`) behind C: a tolerant, non-recursive
re-parse of the malformed JSON LLMs emit for tool calls — markdown fences,
double-quoted or unquoted keys, `True`/`None`/`NaN`/`0x1A`, dropped commas and
colons, unclosed containers, values cut off at the token limit, raw newlines in
strings, prose wrapped around the object — re-serialised as canonical JSON a
strict parser accepts. Results are delivered into a `kimix_vec`, so the whole
`ffi_vec.h` contract (init/destroy pair, guard word, one heap, moves are the only
cheap transfer) applies unchanged.

**The result convention** — the one thing to remember: `kimix::repair()` uses
EMPTYNESS as its status. An EMPTY result means the input was already strictly
valid JSON (nothing to hand back; parse the bytes you already have) — and, for
completeness, an input the tolerant parser could not turn into valid JSON is also
empty, because the library never emits invalid JSON. A NON-EMPTY result is the
repaired, strictly valid JSON whose LAST byte is the NUL terminator, so
`kimix_vec_size()` is text length + 1 and the JSON text is the first `size() - 1`
bytes. Nothing here throws; failures are a `kimix_status`, NULL or `false`.

The borrowed result pair is the POD `kimix_str_view { const char *data; size_t
length; }` — the C shape of `kimix::string_view`: `data` is NOT NUL-terminated
and `length` is in BYTES (both 0/NULL for the empty view).

### Validity probe

#### `bool kimix_json_is_valid(KIMIX_IN const void *json, size_t len)`
"Would `kimix_repair()` return an empty result for these bytes because they need no repair?" — the strict validity check `kimix::repair()` performs as its very first step.
- Wraps: re-implements the `is_valid_json()` helper of `src/core/json_repair.cpp` (anonymous namespace, not exported) with `yyjson_read_opts()` over the library's mimalloc `yyjson_alc` — a full parse, not a peek: the non-INSITU reader copies the input into a padded buffer and allocates the value/string pools, then this call frees everything before returning.
- Ownership: nothing is handed to the caller: no `kimix_vec`, no repaired text, one parse and one allocation round-trip, both released internally. `json` need not be NUL-terminated; `len` is bytes.
- Errors: returns `true`/`false` only — it cannot fail. NULL `json` with `len == 0` is `false`, not an error.
- Notes: an EMPTY input is NOT valid (`len == 0` yields `false`, which is what the probe answers anyway: yyjson rejects an empty document and `kimix::repair("")` produces the text `"null"`). Consistency: `true` ⇒ the result vector is EMPTY; `false` does NOT guarantee a non-empty result.

#### `bool kimix_json_is_valid_str(KIMIX_IN const char *nul_terminated_json)`
The NUL-terminated spelling of the same probe; the length is `strlen(nul_terminated_json)`.
- Wraps: `yyjson_read_opts()` on the `strlen`-derived range.
- Ownership: nothing to free.
- Errors: `false` for NULL (an empty input is not valid, and it is not an error).
- Notes: embedded NULs are not representable here, and the bytes must be valid UTF-8 for the parser to accept them at all.

### Repairing into a placeholder

#### `kimix_status kimix_repair(KIMIX_OUT kimix_vec *out, KIMIX_IN const void *json, size_t len)`
Repairs `[json, json + len)` and MOVES the result into the raw storage of `out`.
- Wraps: `kimix::vector<char> kimix::repair(kimix::string_view json)`, moved through `kimix::api::vec_construct_move_from_char()` — the same way `kimix_vec_init_from()` constructs its vector, so no byte of the repaired text is copied across the boundary.
- Ownership: `out` needs no prior init and does not have to be destroyed and re-created between calls (use `kimix_repair_assign()` for that); on success it holds a live vector its owner destroys with `kimix_vec_destroy()`. `json` is caller-owned input read during the call; `len` is bytes, no terminator required.
- Errors: `KIMIX_ERR_INVALID_ARG` for a NULL `out` or a NULL `json` with `len > 0`; `KIMIX_ERR_INVALID_STATE` when `out` already holds a live vector; `KIMIX_OK` otherwise — including when the input was already valid, which yields an EMPTY vector ("nothing to repair" is a success). Never `KIMIX_ERR_OUT_OF_MEMORY` (the vector's allocator aborts on exhaustion). On any error `out` is left untouched.
- Notes: a NULL `json` with `len == 0` is the empty input, which core repairs to the text `"null"` — so the result is NON-empty. A non-empty result ends with `'\0'` as its last byte.

#### `kimix_status kimix_repair_str(KIMIX_OUT kimix_vec *out, KIMIX_IN const char *nul_terminated_json)`
The NUL-terminated spelling of `kimix_repair()`; the input length is `strlen(nul_terminated_json)`.
- Wraps: `kimix::repair()` on the `strlen`-derived view, moved into `out`.
- Ownership: same result convention and same ownership as `kimix_repair()`.
- Errors: `KIMIX_ERR_INVALID_ARG` for a NULL `nul_terminated_json` (there is no length to fall back on — pass `""` or `kimix_repair(out, NULL, 0)` to repair the empty input), `KIMIX_ERR_INVALID_STATE` when `out` already holds a live vector, `KIMIX_OK` otherwise.

#### `kimix_status kimix_repair_assign(KIMIX_IN_OUT kimix_vec *out, KIMIX_IN const void *json, size_t len)`
Same as `kimix_repair()`, but ASSIGNs into a placeholder that already holds a live vector (`*out = std::move(repaired)`).
- Wraps: `kimix::repair()` plus `std::vector::operator=` (move).
- Ownership: the previous content is released on the library heap and the repaired bytes take its place; `out` stays initialised and keeps being the object its owner destroys exactly once. This is the call to use to repair into a long-lived buffer in a loop.
- Errors: `KIMIX_ERR_INVALID_ARG` for a NULL `out` or a NULL `json` with `len > 0`; `KIMIX_ERR_INVALID_STATE` when `out` is NOT initialised (nothing to assign into); `KIMIX_OK` otherwise, and on error the vector keeps its old content.
- Notes: an empty result still replaces the previous content — by nothing.

#### `kimix_vec *kimix_repair_new(KIMIX_IN const void *json, size_t len)`
Heap convenience for bindings that prefer a pointer over embedding a placeholder: allocates one `kimix_vec` on the library heap and fills it with the repair of `[json, json + len)`.
- Wraps: `kimix_vec_new()` semantics + `kimix::repair()`
- Ownership: the caller owns the placeholder and MUST release it with `kimix_vec_free()`, never with its own `free()`.
- Errors: `NULL` on an allocation failure or a NULL `json` with `len > 0` (nothing leaks — the placeholder is freed again before NULL is returned).
- Notes: to distinguish "was already valid", check `kimix_vec_size() == 0`.

### Reading a result

#### `kimix_status kimix_repaired_view(KIMIX_BORROWED const kimix_vec *result, KIMIX_OUT kimix_str_view *out_view)`
The repaired text WITHOUT its trailing `'\0'` — the first `size() - 1` bytes of `result`.
- Wraps: `kimix::repaired_view(const vector<char>&)` of `src/core/json_repair.h`; the placeholder is validated through the public accessor semantics (the guard word of `api/detail.h`) before a single byte is read.
- Ownership: the view is BORROWED from `result`: it dies with any call that resizes, assigns, moves-from or destroys the vector, and with `kimix_vec_destroy()` / `kimix_vec_free()`. `*out_view` is `{ NULL, 0 }` plus `KIMIX_OK` for a live but EMPTY result (the input was already valid), `{ data(), size() - 1 }` for a live non-empty one.
- Errors: `KIMIX_ERR_INVALID_ARG` for a NULL `out_view` or NULL `result`; `KIMIX_ERR_INVALID_STATE` for a dead placeholder — `*out_view` is neutralised to `{ NULL, 0 }` first.
- Notes: the helper drops the LAST byte unconditionally, exactly like the C++ one, so call it on an unmodified result: a buffer you mutated through `ffi_vec.h` loses its real last byte.

#### `const char *kimix_repaired_cstr(KIMIX_BORROWED const kimix_vec *result)`
The same text as a NUL-terminated C string — a non-empty result already ends with `'\0'` inside its buffer, so this simply returns `data()`: no copy, no allocation.
- Wraps: the vector's `data()` under core's trailing-NUL convention.
- Ownership: BORROWED from `result` — invalid after any mutation of the vector (resize / reserve / assign / append / swap / move-from) and after `kimix_vec_destroy()` / `kimix_vec_free()`. Never free it.
- Errors: `NULL` for an empty result (the input was already valid: there is no text to point at), for a dead placeholder and for a NULL `result`.

---

## Quick start

Three complete, self-contained programs. Every call, argument order and release
below is taken from the headers, and all three were compiled and run against the
built library:

```console
# clang / gcc
clang -std=c11 -Wall -Wextra -I src ex1.c -L bin/debug -lkimix_api -o ex1
# MSVC (the headers are pure ASCII: no /utf-8 needed, it is harmless either way)
cl /nologo /utf-8 /W4 /I src ex1.c bin/debug/kimix_api.lib /Fe:ex1.exe
```

### (a) Allocate, inspect, free with `kimix_mem_*`

```c
/* ffi_mem area: one heap, one matching free. */
#include <api/kimix_api.h>   /* KIMIX_API_STATIC for a dlopen build */
#include <stdio.h>
#include <string.h>

int main(void) {
    /* Assert the ABI once, before trusting any layout. */
    if (kimix_api_abi_version() != KIMIX_API_ABI_VERSION) return 1;
    kimix_layout_info li;
    if (kimix_api_layout_info(&li) != KIMIX_OK) return 1;
    if (li.size_of_size_t != sizeof(size_t) || li.size_of_bool != 1) return 1;

    void *buf = kimix_mem_malloc(64);            /* library heap, NOT the caller's */
    if (!buf) return 1;                          /* see the MI_XMALLOC caveat */
    size_t usable = kimix_mem_usable_size(buf);  /* >= 64 */
    printf("%s: usable %zu bytes\n", kimix_api_build_info(), usable);

    /* Grow: the old block is consumed on success, kept on failure. */
    void *big = kimix_mem_realloc(buf, 4096);
    if (!big) { kimix_mem_free(buf); return 1; }

    /* A zero-filled aligned array, posix-shaped error reporting. */
    void *tab = NULL;
    if (kimix_mem_posix_memalign(&tab, 64, 128) != KIMIX_OK) { kimix_mem_free(big); return 1; }
    memset(tab, 0, kimix_mem_usable_size(tab));

    char *dup = kimix_mem_strdup("hello");       /* copy on the library heap */
    char *head = kimix_mem_strndup("hello world", 5);

    kimix_mem_free(head);
    kimix_mem_free(dup);
    kimix_mem_free(tab);                          /* aligned: plain free is fine */
    kimix_mem_free(big);
    printf("ok: %s\n", kimix_status_name(KIMIX_OK));
    return 0;
}
```

### (b) Build a `kimix_vec`, move it, destroy both

```c
/* ffi_vec area: init ... use ... destroy, exactly once each. */
#include <api/kimix_api.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    kimix_vec a, b;                    /* inline storage: stack, struct field, anywhere */
    memset(&a, 0, sizeof a);           /* raw storage: memset 0 is legal and enough */
    memset(&b, 0, sizeof b);

    if (kimix_vec_init_from(&a, "abc", 3) != KIMIX_OK) return 1;
    if (kimix_vec_append(&a, "def", 3) != KIMIX_OK) { kimix_vec_destroy(&a); return 1; }
    if (kimix_vec_push_back(&a, '!') != KIMIX_OK) { kimix_vec_destroy(&a); return 1; }

    size_t n = kimix_vec_size(&a);     /* 7 — (size_t)-1 would mean a dead placeholder */
    unsigned char *q = NULL;
    if (kimix_vec_at(&a, n - 1, &q) != KIMIX_OK) { kimix_vec_destroy(&a); return 1; }
    unsigned char last = *q;                          /* '!' */
    /* kimix_vec_data(&a) would give the same base pointer, but it is BORROWED and
       is invalidated by any resize / reserve / assign / move. */

    /* Move a into b: b must be RAW storage, a stays initialised but empty. */
    if (kimix_vec_move_init(&b, &a) != KIMIX_OK) { kimix_vec_destroy(&a); return 1; }
    if (kimix_vec_size(&a) != 0) return 1;       /* moved-from, still valid */

    char out[16];
    if (kimix_vec_read(&b, 0, out, 7) != KIMIX_OK) return 1;
    out[7] = '\0';
    printf("b holds: %s (%zu bytes, capacity %zu, last byte 0x%02x)\n",
           out, kimix_vec_size(&b), kimix_vec_capacity(&b), last);

    kimix_vec_destroy(&a);             /* both placeholders are destroyed once */
    kimix_vec_destroy(&b);

    /* The heap-handle flavour, for comparison: */
    kimix_vec *h = kimix_vec_new_from("raw", 3);
    if (h) kimix_vec_free(h);         /* free() of your own module would be UB */
    return 0;
}
```

> `kimix_vec_at` hands you a bounds-checked address, `kimix_vec_read` hands you a
> copy, and `kimix_vec_data` the raw base pointer. All three are BORROWED: any
> resize, `kimix_vec_reserve`, assignment, move or destroy invalidates them.

### (c) Parse JSON, read a nested field, build and write a modified document

```c
/* ffi_yyjson area: immutable read surface + mutable build surface. */
#include <api/kimix_api.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const char *text = "{\"tool\":{\"name\":\"bash\",\"args\":{\"cmd\":\"ls\"}}}";

    /* 1. parse (length-based; no NUL terminator required) */
    kimix_yyjson_read_err rerr;
    kimix_yyjson_doc *doc = kimix_yyjson_read(text, strlen(text),
                                              KIMIX_YYJSON_READ_ALLOW_TRAILING_COMMAS,
                                              &rerr);
    if (!doc) {
        fprintf(stderr, "read: code %u at %zu: %s\n",
                rerr.code, rerr.position, rerr.msg ? rerr.msg : "?");
        return 1;                       /* nothing allocated: doc == NULL */
    }

    /* 2. read a nested field — every val is BORROWED from doc */
    kimix_yyjson_val *root = kimix_yyjson_doc_root(doc);
    kimix_yyjson_val *tool = kimix_yyjson_obj_get(root, "tool");
    kimix_yyjson_val *args = kimix_yyjson_obj_get(tool, "args");
    kimix_yyjson_val *cmd  = kimix_yyjson_obj_get(args, "cmd");
    const char *cmd_text = kimix_yyjson_get_str(cmd);      /* "ls" */
    size_t cmd_len = kimix_yyjson_get_len(cmd);            /* 2 */
    printf("cmd = %.*s (type %s)\n", (int)cmd_len,
           cmd_text ? cmd_text : "", kimix_yyjson_get_type_desc(cmd));

    /* 3. build a modified copy on the MUTABLE surface */
    kimix_yyjson_mut_doc *mdoc = kimix_yyjson_doc_mut_copy(doc);
    if (!mdoc) { kimix_yyjson_doc_free(doc); return 1; }
    kimix_yyjson_mut_val *mroot = kimix_yyjson_mut_doc_root(mdoc);
    kimix_yyjson_mut_val *margs = kimix_yyjson_mut_obj_get(
                        kimix_yyjson_mut_obj_get(mroot, "tool"), "args");
    /* put() replaces the pair with the same key; the key and the value are both
       copied into the document's pool, so the literals may be temporary. */
    if (!kimix_yyjson_mut_obj_put(margs,
            kimix_yyjson_mut_strcpy(mdoc, "cmd"),
            kimix_yyjson_mut_strcpy(mdoc, "ls -la"))) {
        kimix_yyjson_mut_doc_free(mdoc); kimix_yyjson_doc_free(doc); return 1;
    }
    kimix_yyjson_mut_obj_add_strcpy(mdoc, margs, "cwd", "/tmp");
    kimix_yyjson_mut_obj_add_arr(mdoc, margs, "history");   /* nested, fill later */

    /* 4. write: the string is mimalloc memory of THIS library */
    size_t out_len = 0;
    kimix_yyjson_write_err werr;
    char *out = kimix_yyjson_mut_write(mdoc, KIMIX_YYJSON_WRITE_PRETTY, &out_len, &werr);
    if (!out) {
        fprintf(stderr, "write: code %u: %s\n", werr.code, werr.msg ? werr.msg : "?");
    } else {
        fwrite(out, 1, out_len, stdout);
        kimix_yyjson_str_free(out);        /* not free(), not another module's kimix_mem_free */
    }

    /* 5. release in the reverse order of creation */
    kimix_yyjson_mut_doc_free(mdoc);
    kimix_yyjson_doc_free(doc);            /* every kimix_yyjson_val above is now dangling */
    return 0;
}
```

For the LLM-JSON case, feed the model's raw output through the repair area first
and parse the result: `kimix_repair(&vec, text, len)` → `kimix_repaired_cstr(&vec)`
(empty vector ⇒ the text was already valid) → `kimix_yyjson_read_str(...)` →
`kimix_vec_destroy(&vec)`.

---

## Feature switch / build

`kimix_api` is switched by the `kimix_enable_api` option (default `true`) declared
in the root `xmake.lua`. It uses the **file-level** mechanism rather than the
`kimix_feature_gate` rule: `src/xmake.lua` only does `includes("api")` when the
option is on, so with it off `src/api/xmake.lua` is not even read.

```console
# turn the whole area off (file-level skip: the target, its headers and
# test_kimix_api all leave the configuration)
xmake f --kimix_enable_api=false -c

# back on
xmake f --kimix_enable_api=true -c

# build just the FFI library
xmake build kimix_api            # -> bin/<mode>/kimix_api.dll (+ .lib/.exp)
                                 # -> bin/<mode>/libkimix_api.so on Linux

# run the test that covers it, or the whole suite
xmake run test_kimix_api
xmake test
```

Details worth knowing:

* The option is independent of `kimix_enable_llm` / `kimix_enable_cli` /
  `kimix_enable_runtime`: `kimix_api` depends on `kimix-core` only, and mimalloc /
  yyjson arrive through it — the target never `add_deps` a `src/ext` target
  directly.
* `kimix_api` is additionally listed in the target→feature map in
  `scripts/xmake_func.lua`, so a future target that links it is dropped with the
  same option and the switch can never leave a dangling `add_deps("kimix_api")`.
* `test_kimix_api` (`tests/unit/api/test_kimix_api.cpp`) is the Boost.UT target
  that links this library; its registration in `tests/xmake.lua` repeats the same
  `kimix_enable_api` guard (and it also needs `kimix_enable_tests`). It includes
  only the public C headers, so it exercises the FFI exactly like a foreign
  binding does: the placeholder lifecycles and their guard-word error paths, JSON
  read / build / write, repair, and the allocation family. If `xmake run
  test_kimix_api` says the target does not exist, one of the two options is off —
  `xmake test` then still runs everything else that is configured.
* With the option off, `xmake build kimix_api` is a silent no-op — the target is
  simply not in the configuration.
* The library propagates `KIMIX_CORE_STATIC` (plus `NOMINMAX` and
  `_CRT_SECURE_NO_WARNINGS` on Windows) publicly, and is compiled with `-fPIC` on
  Linux because the static archives it links live inside a shared image.
* Full bootstrap: `python bootstrap.py` (add `--debug`, `--toolchain <name>`,
  `--test`, `--clean`, `--jobs N`); `python publish.py` packages the release
  artifacts.

---

## Index — every exported function, alphabetical


One row per exported function, sorted by name. **The headers under
`src/api` are the authoritative, machine-readable list** — this table is a
search index into them, and the area column names the header (and the JSON
sub-section) the declaration lives in.

| function | area | purpose |
|---|---|---|
| `kimix_api_abi_version`                  | ABI      | The ABI version this build of the library was compiled with — the value of `KIMIX_API_ABI_VERSION`. Compare it… |
| `kimix_api_build_info`                   | ABI      | A human-readable build stamp for bug reports: platform, architecture, build mode… |
| `kimix_api_layout_info`                  | ABI      | Fills the POD `kimix_layout_info` (`abi_version`, `_reserved`, `size_of_size_t`, `size_of_pointer`… |
| `kimix_api_version_string`               | ABI      | The `kimix-core` version string this library was built against, e.g. `"kimix 1.2.3"`. NUL-terminated UTF-8. |
| `kimix_json_is_valid`                    | repair   | "Would `kimix_repair()` return an empty result for these bytes because they need no repair?"… |
| `kimix_json_is_valid_str`                | repair   | The NUL-terminated spelling of the same probe; the length is `strlen(nul_terminated_json)`. |
| `kimix_mem_calloc`                       | mem      | Allocates a zero-filled array of `count` elements of `size` bytes. |
| `kimix_mem_calloc_aligned`               | mem      | Allocates an aligned, zero-filled array of `count` elements of `size` bytes. |
| `kimix_mem_collect`                      | mem      | Asks mimalloc to return cached/abandoned memory to the OS now. |
| `kimix_mem_expand`                       | mem      | Tries to resize in place; never copies and never silently falls back to a new block. |
| `kimix_mem_free`                         | mem      | Releases a block back to the library heap. |
| `kimix_mem_free_aligned`                 | mem      | Frees an aligned block, passing the alignment it was allocated with. |
| `kimix_mem_free_size`                    | mem      | Frees a block, passing the size originally requested. |
| `kimix_mem_good_size`                    | mem      | The block size mimalloc would hand out for `size` (rounded up to a size class). |
| `kimix_mem_is_redirected`                | mem      | True when mimalloc has taken over the process-wide allocator, i.e. the host's libc `malloc`/`free`… |
| `kimix_mem_malloc`                       | mem      | Allocates `size` uninitialised bytes. |
| `kimix_mem_malloc_aligned`               | mem      | Allocates `size` bytes at an address that satisfies `alignment`. |
| `kimix_mem_malloc_aligned_at`            | mem      | Allocates an offset-aligned block whose address `a` satisfies `a % alignment == offset % alignment`. |
| `kimix_mem_posix_memalign`               | mem      | `posix_memalign` with this library's error vocabulary. |
| `kimix_mem_realloc`                      | mem      | Re-sizes a block, possibly moving it. |
| `kimix_mem_realloc_aligned`              | mem      | Re-sizes an aligned block, keeping the same alignment. |
| `kimix_mem_recalloc`                     | mem      | The calloc-shaped `rezalloc`: resize the array to `newcount` elements of `size` bytes, zeroing the grown tail. |
| `kimix_mem_rezalloc`                     | mem      | Like `kimix_mem_realloc`, but the newly exposed tail bytes are zeroed. |
| `kimix_mem_strdup`                       | mem      | Copies a NUL-terminated UTF-8 string onto the library heap. |
| `kimix_mem_strndup`                      | mem      | Copies at most `n` bytes of `s` and always NUL-terminates the copy. |
| `kimix_mem_thread_done`                  | mem      | Releases the calling thread's heap registration. |
| `kimix_mem_thread_init`                  | mem      | Registers a mimalloc heap for the calling thread. |
| `kimix_mem_usable_size`                  | mem      | The real usable size of a block — the number of bytes that may be written to `p` (always >= the requested size). |
| `kimix_mem_version`                      | mem      | The mimalloc version number this library was built with, encoding `major * 10000 + minor * 100 + patch`… |
| `kimix_mem_version_string`               | mem      | The same version as text: `major.minor.patch` without leading zeros (`"3.5.2"` for `30502`). |
| `kimix_mem_zalloc`                       | mem      | Allocates `size` bytes filled with zero. |
| `kimix_mem_zalloc_aligned`               | mem      | Allocates `size` zero-filled bytes at an aligned address. |
| `kimix_repair`                           | repair   | Repairs `[json, json + len)` and MOVES the result into the raw storage of `out`. |
| `kimix_repair_assign`                    | repair   | Same as `kimix_repair()`, but ASSIGNs into a placeholder that already holds a live vector… |
| `kimix_repair_new`                       | repair   | Heap convenience for bindings that prefer a pointer over embedding a placeholder… |
| `kimix_repair_str`                       | repair   | The NUL-terminated spelling of `kimix_repair()`; the input length is `strlen(nul_terminated_json)`. |
| `kimix_repaired_cstr`                    | repair   | The same text as a NUL-terminated C string — a non-empty result already ends with `'\0'` inside its buffer… |
| `kimix_repaired_view`                    | repair   | The repaired text WITHOUT its trailing `'\0'` — the first `size() - 1` bytes of `result`. |
| `kimix_status_name`                      | ABI      | The textual name of a status code (`"KIMIX_OK"`, `"KIMIX_ERR_OUT_OF_MEMORY"`, …). |
| `kimix_vec_abi_align`                    | vec      | The fixed alignment a caller must give the placeholder (`KIMIX_VEC_ALIGN`, 16). |
| `kimix_vec_abi_bytes`                    | vec      | The fixed inline-storage size a caller must reserve (`KIMIX_VEC_BYTES`, 64). |
| `kimix_vec_append`                       | vec      | Appends the bytes `[data, data + len)` at the end. |
| `kimix_vec_assign`                       | vec      | `dst = *src` — a deep copy. |
| `kimix_vec_assign_bytes`                 | vec      | Replaces the content with the bytes `[data, data + len)`. |
| `kimix_vec_at`                           | vec      | Bounds-checked element address: `*out == &data()[index]`. |
| `kimix_vec_capacity`                     | vec      | The allocated capacity in bytes. |
| `kimix_vec_clear`                        | vec      | `v.clear()` — size becomes 0, capacity is kept. |
| `kimix_vec_copy_init`                    | vec      | Constructs `dst` (raw storage) as a deep **copy** of the live vector `src`. |
| `kimix_vec_data`                         | vec      | The pointer to the contiguous bytes. |
| `kimix_vec_default_init`                 | vec      | Constructs an EMPTY vector in `v`. |
| `kimix_vec_destroy`                      | vec      | Destroys the vector in `v` and releases its buffer to the library heap. |
| `kimix_vec_empty`                        | vec      | True when `size() == 0`. |
| `kimix_vec_equals`                       | vec      | Element-wise equality of two vectors (size and content). |
| `kimix_vec_erase`                        | vec      | Removes the byte range `[index, index + len)`. |
| `kimix_vec_free`                         | vec      | Destroys the vector and releases the placeholder storage. |
| `kimix_vec_init_from`                    | vec      | Constructs `v` as a copy of the bytes `[data, data + len)`. |
| `kimix_vec_inner_align`                  | vec      | `alignof` of that C++ object. |
| `kimix_vec_inner_bytes`                  | vec      | `sizeof` of the C++ `kimix::vector<std::byte>` the library actually constructs inside the storage. |
| `kimix_vec_insert`                       | vec      | Inserts the bytes `[data, data + len)` **before** `index`. |
| `kimix_vec_is_initialized`               | vec      | True when `v` currently holds a live C++ vector (initialised and not yet destroyed). |
| `kimix_vec_move_assign`                  | vec      | `dst = std::move(*src)`. |
| `kimix_vec_move_init`                    | vec      | Constructs `dst` (raw storage) by **moving** the live vector `src`. |
| `kimix_vec_new`                          | vec      | Allocates one placeholder on the library heap and default-constructs an empty vector in it. |
| `kimix_vec_new_from`                     | vec      | Same, but initialised with the bytes `[data, data + len)`. |
| `kimix_vec_pop_back`                     | vec      | `v.pop_back()` — remove the last byte. |
| `kimix_vec_push_back`                    | vec      | `v.push_back(b)` — append one byte. |
| `kimix_vec_read`                         | vec      | Copies `len` bytes **out of** the vector starting at `offset`. |
| `kimix_vec_reserve`                      | vec      | `v.reserve(n)` — capacity only, size unchanged; may reallocate. |
| `kimix_vec_resize`                       | vec      | `v.resize(n)` — grows with zero bytes or shrinks. |
| `kimix_vec_resize_fill`                  | vec      | `v.resize(n, fill)` — grows with `fill` bytes instead of zeros. |
| `kimix_vec_shrink_to_fit`                | vec      | `v.shrink_to_fit()` — release unused capacity. |
| `kimix_vec_size`                         | vec      | The number of bytes stored (0 for an empty vector). |
| `kimix_vec_swap`                         | vec      | Swaps the two vectors' buffers in O(1). |
| `kimix_vec_write`                        | vec      | Copies `len` bytes **into** the vector starting at `offset` — it never grows the vector. |
| `kimix_yyjson_arr_get`                   | json §4  | The element at `idx`. |
| `kimix_yyjson_arr_get_first`             | json §4  | The first element. |
| `kimix_yyjson_arr_get_last`              | json §4  | The last element. |
| `kimix_yyjson_arr_size`                  | json §4  | The number of elements. |
| `kimix_yyjson_doc_free`                  | json §1  | Releases a document and every value it owns. |
| `kimix_yyjson_doc_mut_copy`              | json §1  | Deep-copies an immutable document into a mutable one. |
| `kimix_yyjson_doc_read_size`             | json §1  | The number of input bytes the document was read from. |
| `kimix_yyjson_doc_root`                  | json §1  | The root value of a parsed document. |
| `kimix_yyjson_doc_val_count`             | json §1  | The number of values stored in the document. |
| `kimix_yyjson_equals_str`                | json §3  | Is the value a string equal to the NUL-terminated `str`? |
| `kimix_yyjson_equals_strn`               | json §3  | Is the value a string equal to the `str` / `len` bytes (no terminator required)? |
| `kimix_yyjson_get_bool`                  | json §3  | The value as true/false. |
| `kimix_yyjson_get_int`                   | json §3  | The value as an integer truncated to 32 bits. |
| `kimix_yyjson_get_len`                   | json §3  | The byte length of a string, raw or container value. |
| `kimix_yyjson_get_num`                   | json §3  | Any number as a double (uint, sint and real are all converted). |
| `kimix_yyjson_get_raw`                   | json §3  | The literal text of a `KIMIX_YYJSON_TYPE_RAW` value. |
| `kimix_yyjson_get_real`                  | json §3  | The value as a double, only when it really is a real. |
| `kimix_yyjson_get_sint`                  | json §3  | The value as a signed integer. |
| `kimix_yyjson_get_str`                   | json §3  | The string content as NUL-terminated UTF-8. |
| `kimix_yyjson_get_type`                  | json §2  | The value's type as one of the `KIMIX_YYJSON_TYPE_*` macros (the 3 type bits of the tag). |
| `kimix_yyjson_get_type_desc`             | json §2  | A static description: `"raw"`, `"null"`, `"string"`, `"array"`, `"object"`, `"true"`, `"false"`, `"uint"`… |
| `kimix_yyjson_get_uint`                  | json §3  | The value as an unsigned integer. |
| `kimix_yyjson_is_arr`                    | json §2  | True for an array. |
| `kimix_yyjson_is_bool`                   | json §2  | True for either bool. |
| `kimix_yyjson_is_ctn`                    | json §2  | True for a container: array or object. |
| `kimix_yyjson_is_false`                  | json §2  | True for the `false` literal (the other bool subtype test). |
| `kimix_yyjson_is_int`                    | json §2  | True for uint **or** sint — the fork's integer test. |
| `kimix_yyjson_is_null`                   | json §2  | True for the `null` literal. |
| `kimix_yyjson_is_num`                    | json §2  | True for any number subtype (uint, sint or real). |
| `kimix_yyjson_is_obj`                    | json §2  | True for an object. |
| `kimix_yyjson_is_raw`                    | json §2  | True for a `KIMIX_YYJSON_TYPE_RAW` value (a number kept as its text). |
| `kimix_yyjson_is_real`                   | json §2  | True for a floating-point number. |
| `kimix_yyjson_is_sint`                   | json §2  | True for a negative integer stored as `int64_t`. |
| `kimix_yyjson_is_str`                    | json §2  | True for a string. |
| `kimix_yyjson_is_true`                   | json §2  | True for the `true` literal (a bool subtype test). |
| `kimix_yyjson_is_uint`                   | json §2  | True for a non-negative integer stored as `uint64_t`. |
| `kimix_yyjson_mut_arr`                   | json §8  | Creates an EMPTY array, ready for section 10. |
| `kimix_yyjson_mut_arr_add_arr`           | json §10 | Appends a NEW empty array and returns it, so a nested container can be filled without a create-then-append pair. |
| `kimix_yyjson_mut_arr_add_bool`          | json §10 | Creates a bool from `val` and appends it. |
| `kimix_yyjson_mut_arr_add_null`          | json §10 | Creates a `null` and appends it. |
| `kimix_yyjson_mut_arr_add_obj`           | json §10 | Appends a NEW empty object under `arr` and returns it. |
| `kimix_yyjson_mut_arr_add_real`          | json §10 | Creates a real and appends it. |
| `kimix_yyjson_mut_arr_add_sint`          | json §10 | Creates a signed integer and appends it. |
| `kimix_yyjson_mut_arr_add_str`           | json §10 | Appends a string **without copying it**. |
| `kimix_yyjson_mut_arr_add_strcpy`        | json §10 | Appends a string and COPIES it into the document's string pool. |
| `kimix_yyjson_mut_arr_add_strn`          | json §10 | Appends a string of an explicit byte length, still without copying. |
| `kimix_yyjson_mut_arr_add_uint`          | json §10 | Creates an unsigned integer and appends it. |
| `kimix_yyjson_mut_arr_add_val`           | json §10 | Appends an existing value. |
| `kimix_yyjson_mut_arr_append`            | json §10 | Appends `val` at the end of the array. |
| `kimix_yyjson_mut_arr_clear`             | json §10 | Drops all elements of the array. |
| `kimix_yyjson_mut_arr_get`               | json §9  | The element at `idx` of a mutable array. |
| `kimix_yyjson_mut_arr_get_first`         | json §9  | The first element of a mutable array. |
| `kimix_yyjson_mut_arr_get_last`          | json §9  | The last element of a mutable array. |
| `kimix_yyjson_mut_arr_insert`            | json §10 | Inserts `val` at position `idx`. |
| `kimix_yyjson_mut_arr_prepend`           | json §10 | Inserts `val` at the front of the array. |
| `kimix_yyjson_mut_arr_remove`            | json §10 | Takes the element at `idx` out of the array. |
| `kimix_yyjson_mut_arr_remove_first`      | json §10 | Removes and returns the first element. |
| `kimix_yyjson_mut_arr_remove_last`       | json §10 | Removes and returns the last element. |
| `kimix_yyjson_mut_arr_replace`           | json §10 | Puts `val` at `idx` and returns the element it displaced. |
| `kimix_yyjson_mut_arr_size`              | json §9  | The number of elements of a mutable array. |
| `kimix_yyjson_mut_bool`                  | json §8  | Creates `true` or `false` from a 1-byte `bool`. |
| `kimix_yyjson_mut_doc_free`              | json §7  | Releases a mutable document and all of its values. |
| `kimix_yyjson_mut_doc_imut_copy`         | json §7  | Deep-copies a mutable document into a new immutable one — the usual last step before using the section 2–6… |
| `kimix_yyjson_mut_doc_mut_copy`          | json §7  | Deep-copies a mutable document into a new mutable one. |
| `kimix_yyjson_mut_doc_new`               | json §7  | Creates an empty mutable document (no root yet). |
| `kimix_yyjson_mut_doc_root`              | json §7  | The root value of a mutable document. |
| `kimix_yyjson_mut_doc_set_root`          | json §7  | Sets — or with a NULL `root` clears — the document's root. |
| `kimix_yyjson_mut_equals_str`            | json §9  | Is the value a string equal to the NUL-terminated `str`? |
| `kimix_yyjson_mut_equals_strn`           | json §9  | Is the value a string equal to the `str` / `len` bytes? |
| `kimix_yyjson_mut_false`                 | json §8  | Creates the `false` literal. |
| `kimix_yyjson_mut_get_bool`              | json §9  | The value as true/false. |
| `kimix_yyjson_mut_get_int`               | json §9  | The value truncated to 32 bits. |
| `kimix_yyjson_mut_get_len`               | json §9  | The byte length of a string, raw or container value. |
| `kimix_yyjson_mut_get_num`               | json §9  | Any number as a double. |
| `kimix_yyjson_mut_get_raw`               | json §9  | The literal text of a raw value. |
| `kimix_yyjson_mut_get_real`              | json §9  | The value as a double, only for a real. |
| `kimix_yyjson_mut_get_sint`              | json §9  | The value as a signed integer. |
| `kimix_yyjson_mut_get_str`               | json §9  | The string content. |
| `kimix_yyjson_mut_get_type`              | json §9  | The value's type as a `KIMIX_YYJSON_TYPE_*` macro. |
| `kimix_yyjson_mut_get_type_desc`         | json §9  | A static description string of the type. |
| `kimix_yyjson_mut_get_uint`              | json §9  | The value as an unsigned integer. |
| `kimix_yyjson_mut_int`                   | json §8  | The signed integer creator taking a wide argument. |
| `kimix_yyjson_mut_is_arr`                | json §9  | True for an array. |
| `kimix_yyjson_mut_is_bool`               | json §9  | True for either bool. |
| `kimix_yyjson_mut_is_ctn`                | json §9  | True for a container (array or object). |
| `kimix_yyjson_mut_is_false`              | json §9  | True for the `false` literal. |
| `kimix_yyjson_mut_is_int`                | json §9  | True for uint **or** sint. |
| `kimix_yyjson_mut_is_null`               | json §9  | True for `null`. |
| `kimix_yyjson_mut_is_num`                | json §9  | True for any number subtype. |
| `kimix_yyjson_mut_is_obj`                | json §9  | True for an object. |
| `kimix_yyjson_mut_is_raw`                | json §9  | True for a raw value. |
| `kimix_yyjson_mut_is_real`               | json §9  | True for a real. |
| `kimix_yyjson_mut_is_sint`               | json §9  | True for a sint-subtype number. |
| `kimix_yyjson_mut_is_str`                | json §9  | True for a string. |
| `kimix_yyjson_mut_is_true`               | json §9  | True for the `true` literal. |
| `kimix_yyjson_mut_is_uint`               | json §9  | True for a uint-subtype number. |
| `kimix_yyjson_mut_null`                  | json §8  | Creates a `null` value. |
| `kimix_yyjson_mut_obj`                   | json §8  | Creates an EMPTY object, ready for section 11. |
| `kimix_yyjson_mut_obj_add`               | json §11 | Appends a key-value pair **without de-duplicating**. |
| `kimix_yyjson_mut_obj_add_arr`           | json §11 | Adds a NEW empty array under `key` and returns it. |
| `kimix_yyjson_mut_obj_add_bool`          | json §11 | Adds a freshly created bool pair. |
| `kimix_yyjson_mut_obj_add_null`          | json §11 | Adds a freshly created `null` pair under `key`. |
| `kimix_yyjson_mut_obj_add_obj`           | json §11 | Adds a NEW empty object under `key` and returns it. |
| `kimix_yyjson_mut_obj_add_real`          | json §11 | Adds a real pair. |
| `kimix_yyjson_mut_obj_add_sint`          | json §11 | Adds a signed integer pair. |
| `kimix_yyjson_mut_obj_add_str`           | json §11 | Adds a string pair with the value bytes NOT copied. |
| `kimix_yyjson_mut_obj_add_strcpy`        | json §11 | Adds a string pair, COPYING the value into the document's pool. |
| `kimix_yyjson_mut_obj_add_strn`          | json §11 | The same with an explicit value length, still no copy. |
| `kimix_yyjson_mut_obj_add_uint`          | json §11 | Adds an unsigned integer pair. |
| `kimix_yyjson_mut_obj_add_val`           | json §11 | Attaches an existing value under `key`. |
| `kimix_yyjson_mut_obj_clear`             | json §11 | Drops all pairs of the object. |
| `kimix_yyjson_mut_obj_get`               | json §9  | The value under the NUL-terminated `key` in a mutable object. |
| `kimix_yyjson_mut_obj_getn`              | json §9  | The value under the exact key `key` / `key_len` (no NUL terminator required). |
| `kimix_yyjson_mut_obj_put`               | json §11 | Sets a key-value pair, removing every existing pair with the same key first. |
| `kimix_yyjson_mut_obj_remove`            | json §11 | Removes every pair with the NUL-terminated `key` and returns the FIRST value removed. |
| `kimix_yyjson_mut_obj_remove_keyn`       | json §11 | The same with an explicit key length (no NUL terminator required). |
| `kimix_yyjson_mut_obj_rename_key`        | json §11 | Renames an existing key in place: the pair keeps its position, only the key's text changes. |
| `kimix_yyjson_mut_obj_size`              | json §9  | The number of pairs of a mutable object. |
| `kimix_yyjson_mut_raw`                   | json §8  | Creates a raw value: `str` is emitted verbatim as JSON text. |
| `kimix_yyjson_mut_real`                  | json §8  | Creates a floating-point value. |
| `kimix_yyjson_mut_sint`                  | json §8  | Creates a signed integer value. |
| `kimix_yyjson_mut_str`                   | json §8  | Creates a string value that **references** `str`: the pointer and its `strlen` are stored, nothing is copied. |
| `kimix_yyjson_mut_strcpy`                | json §8  | Creates a string value and COPIES the text into the document's string pool. |
| `kimix_yyjson_mut_strn`                  | json §8  | The no-copy form with an explicit byte length. |
| `kimix_yyjson_mut_strncpy`               | json §8  | The copying form with an explicit byte length (no NUL terminator required). |
| `kimix_yyjson_mut_true`                  | json §8  | Creates the `true` literal. |
| `kimix_yyjson_mut_uint`                  | json §8  | Creates an unsigned integer value. |
| `kimix_yyjson_mut_val_write`             | json §12 | Serialises one value of a mutable document. |
| `kimix_yyjson_mut_write`                 | json §12 | Serialises a mutable document into a freshly allocated string. |
| `kimix_yyjson_mut_write_buf`             | json §12 | The allocation-free variant into the caller's buffer. |
| `kimix_yyjson_obj_get`                   | json §5  | The value stored under the NUL-terminated `key`. |
| `kimix_yyjson_obj_getn`                  | json §5  | The value stored under the exact key `key` / `key_len` (no NUL terminator required). |
| `kimix_yyjson_obj_size`                  | json §5  | The number of key-value pairs. |
| `kimix_yyjson_read`                      | json §1  | Parses JSON into a new immutable document. |
| `kimix_yyjson_read_str`                  | json §1  | The NUL-terminated spelling: the input length is `strlen(str)`. |
| `kimix_yyjson_str_free`                  | json §6  | Releases a string returned by `kimix_yyjson_write()`, `kimix_yyjson_val_write()`… |
| `kimix_yyjson_val_write`                 | json §6  | Serialises ONE value of an immutable document. |
| `kimix_yyjson_version`                   | json §0  | The yyjson version the library was built against, as a hex number. |
| `kimix_yyjson_write`                     | json §6  | Serialises a whole document into a freshly allocated, NUL-terminated UTF-8 string. |
| `kimix_yyjson_write_buf`                 | json §6  | Serialises a document into the caller's own buffer: no allocation at all, nothing to free. |

Total: **214 functions** — ABI 5, `kimix_mem_*` 26, `kimix_vec_*` 35, `kimix_yyjson_*` 140, repair 8. Any of these is a valid Ctrl-F target for the section above.
