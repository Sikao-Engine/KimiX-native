/*
 * ffi_common.h -- the C-FFI basement of the kimix_api library (src/api).
 *
 * SCOPE: this header is shared by every src/api public FFI header.  It provides
 * the linkage macro, the `extern "C"` plumbing, the fixed-width scalar includes
 * a C99/C11 compiler needs, the common status enum every entry point returns,
 * and the library/ABI query functions.  It declares no feature functions --
 * those live in ffi_mem.h / ffi_vec.h / ffi_map.h / ffi_parallel.h /
 * ffi_yyjson.h / ffi_repair.h, all of which are pulled in by the umbrella header
 * <api/kimix_api.h>.
 *
 * CONTRACT (read before editing anything under src/api):
 *   1. Every public header here must stay valid for BOTH a C99/C11 compiler and
 *      C++17-or-newer.  No templates, no namespaces, no `enum class`, no
 *      default arguments, no overloads, no C++ references, no attributes that
 *      C does not know.  Plain `typedef` + unscoped `enum` only.
 *   2. Every exported declaration is written as
 *         KIMIX_FFI <return type> kimix_<area>_<verb>(<args>);
 *      inside a KIMIX_FFI_BEGIN / KIMIX_FFI_END pair.  Names are lower
 *      snake_case with the `kimix_` prefix; there is exactly one prefix per
 *      feature area so a caller can grep its own bindings.
 *   3. Nothing that is not representable in C crosses the boundary: opaque
 *      pointers (`typedef struct kimix_x kimix_x;`), fixed-size POD structs the
 *      caller may embed, `size_t`, `<stdint.h>` scalars, `bool` (1 byte, C99
 *      `_Bool` / C++ `bool` -- never `int`), `const char *` for UTF-8 text, and
 *      the `kimix_status` code.  C++ classes (kimix::vector, std::string,
 *      std::string_view) and yyjson_alc NEVER appear here.
 *   4. One heap: all buffers handed to the caller come from mimalloc inside
 *      this library (see src/api/xmake.lua) and must be returned with the
 *      matching kimix_mem_* / kimix_vec_* / kimix_yyjson_*_free() call from the
 *      same library, never with the caller module's malloc/free.
 *   5. No exceptions and no RTTI cross the boundary (the project is built with
 *      kimix_enable_exception=false and kimix_rtti=false).  Every failure is a
 *      return value; out-parameters are only written on success.
 *   6. Threading: every function is re-entrant and thread-safe as long as the
 *      objects it is given are not shared concurrently (the same rule as plain
 *      malloc + STL containers).  mimalloc keeps a heap per thread; a caller
 *      thread that is foreign to the host process may call
 *      kimix_mem_thread_init() / kimix_mem_thread_done() around its life.
 *   7. x64 / arm64 only (the platforms this project builds): one calling
 *      convention, `size_t` = pointer width.  Callers that need to verify the
 *      layout of the running library call kimix_api_layout_info().
 *
 * The full documented index of the surface is docs/ffi.md.
 */
#pragma once
#ifndef KIMIX_API_FFI_COMMON_H
#define KIMIX_API_FFI_COMMON_H

/* The export/import macro families (KIMIX_CORE_API, KIMIX_API_API, ...).  This
 * is the "basement" the C FFI is built on: the linkage decision is made in one
 * place in the project, and a pure-C compiler only ever sees the plain macros
 * below.  Define KIMIX_API_STATIC (dlopen/LoadLibrary users, or a static merge)
 * for undecorated declarations, KIMIX_API_EXPORT_DLL when building kimix_api. */
#include <core/dll_export.h>

#include <stddef.h>  /* size_t, NULL */
#include <stdint.h>  /* uint8_t ... uint64_t, int64_t (C99 / C++11) */
#include <stdbool.h> /* bool: ONE byte on both sides of the boundary */

/* ---------------------------------------------------------------------------
 * Linkage + language plumbing
 * ------------------------------------------------------------------------- */

/* KIMIX_FFI marks an exported C-FFI entry point.  Inside the library build it
 * is dllexport / default visibility; a linked consumer gets dllimport; a
 * dlopen-based consumer who defines KIMIX_API_STATIC gets a plain declaration
 * that still resolves through the loaded image. */
#define KIMIX_FFI KIMIX_API_API

/* C++ needs the language linkage; C needs nothing. */
#if defined(__cplusplus)
    #define KIMIX_FFI_BEGIN extern "C" {
    #define KIMIX_FFI_END   }
#else
    #define KIMIX_FFI_BEGIN
    #define KIMIX_FFI_END
#endif

/* Documentation keywords only (they expand to nothing and exist so the
 * contract is readable in the declarations themselves):
 *   KIMIX_IN      caller-owned input, the callee does not keep the pointer
 *   KIMIX_IN_OUT  the object is read AND modified in place
 *   KIMIX_OUT     callee-written out-parameter, only written on success
 *   KIMIX_TRANSFER ownership of the object moves to the callee
 *   KIMIX_BORROWED the pointer stays valid until the owning call below
 *   KIMIX_NULLABLE the argument may be NULL
 *   KIMIX_NOTNULL  the argument must not be NULL (KIMIX_ERR_INVALID_ARG
 *                  otherwise) */
#define KIMIX_IN
#define KIMIX_IN_OUT
#define KIMIX_OUT
#define KIMIX_TRANSFER
#define KIMIX_BORROWED
#define KIMIX_NULLABLE
#define KIMIX_NOTNULL

/* ---------------------------------------------------------------------------
 * ABI identity
 * ------------------------------------------------------------------------- */

/* Bumped whenever the exported surface changes in a way a compiled binding must
 * know about (a function removed, a POD struct field added, a signature
 * changed).  Additions are allowed at the maintainer's discretion, but
 * kimix_api_abi_version() is the value a binding should assert on at load time:
 *      if (kimix_api_abi_version() != KIMIX_API_ABI_VERSION) refuse_to_load();
 * Keep it in sync with the header -- the library returns its own compiled value,
 * so a mismatch is always reported instead of silently mis-parsing a struct. */
#define KIMIX_API_ABI_VERSION 1u

/* ---------------------------------------------------------------------------
 * Status codes
 * ------------------------------------------------------------------------- */

/* Every fallible entry point returns kimix_status.  KIMIX_OK is 0 so that
 * `if (kimix_...(...))` reads as a failure.  Plain C enum: the object size is
 * `int` on both sides of the boundary.
 *
 * The codes are the whole vocabulary of the library, including the ones the
 * current surface does not produce yet: a binding should be written to switch on
 * all eight, because an area may start returning a code that is already declared
 * here without that being an ABI break (the ones marked "reserved" are a
 * deliberately reserved range, not dead entries). */
typedef enum kimix_status {
    /* Success. */
    KIMIX_OK = 0,
    /* A pointer argument was NULL where KIMIX_NOTNULL is required, or a scalar
     * argument was out of the accepted range (e.g. a length that overflows). */
    KIMIX_ERR_INVALID_ARG = 1,
    /* mimalloc could not satisfy the request.  The object is left exactly as it
     * was before the call.  Produced today only by kimix_mem_posix_memalign():
     * the other allocation entry points follow mimalloc's MI_XMALLOC build (see
     * ffi_mem.h), and kimix's own container allocator reports exhaustion by
     * aborting (core/stl/memory.h), so no container call can return this code. */
    KIMIX_ERR_OUT_OF_MEMORY = 2,
    /* An index / range argument is outside [0, size()). */
    KIMIX_ERR_OUT_OF_RANGE = 3,
    /* The object is not usable: a caller-owned placeholder that was never
     * initialised (or already destroyed), or an operation that needs a non-empty
     * buffer.  Detected through the guard word the FFI keeps in the placeholder
     * storage, so use-after-destroy is reported instead of being silently applied
     * to dead memory. */
    KIMIX_ERR_INVALID_STATE = 4,
    /* The bytes handed to a parser/decoder are not acceptable input (malformed
     * JSON, invalid UTF-8 or an unpaired UTF-16 surrogate, a failing repair).
     * Produced by kimix_vec_append_utf16(); the JSON and repair areas report a bad
     * input through their own out-parameters instead (a failed read, an empty
     * repair result). */
    KIMIX_ERR_INVALID_INPUT = 5,
    /* A requested lookup did not hit (missing key, empty object).  Reserved for
     * the same reason: the lookup entry points return the neutral value (NULL /
     * 0 / false), which a C binding can test directly. */
    KIMIX_ERR_NOT_FOUND = 6,
    /* The library could not do what was asked for a reason that is not one of
     * the codes above; read the message out-parameter if the call has one.
     * Reserved: no current entry point produces it. */
    KIMIX_ERR_FAILED = 7
} kimix_status;

/* ---------------------------------------------------------------------------
 * Library / ABI queries -- ffi_common
 * ------------------------------------------------------------------------- */

KIMIX_FFI_BEGIN

/* The ABI version this build of the library was compiled with
 * (KIMIX_API_ABI_VERSION).  Never fails. */
KIMIX_FFI uint32_t kimix_api_abi_version(void);

/* The kimix-core version string this library was built against, e.g.
 * "kimix 1.2.3".  NUL-terminated UTF-8, static storage owned by the library:
 * never free it.  Never fails. */
KIMIX_FFI const char *kimix_api_version_string(void);

/* A human-readable build stamp for bug reports: platform, architecture, build
 * mode and the compiler, e.g. "windows x64 debug msvc" or
 * "linux x64 release gcc".  Static storage; never free it.  Never fails. */
KIMIX_FFI const char *kimix_api_build_info(void);

/* The textual name of a status code ("KIMIX_OK", "KIMIX_ERR_OUT_OF_MEMORY",
 * ...).  Static storage; NULL for a value that is not a defined code. */
KIMIX_FFI const char *kimix_status_name(KIMIX_IN kimix_status code);

/* Layout facts of the running library.  A binding that hard-codes a struct
 * size should check it once at load time instead of trusting this header.
 *   - sizeof(size_t) and sizeof(void *) must match the caller's own types,
 *   - sizeof(bool) is 1 on every supported platform,
 *   - sizeof(kimix_vec) and _Alignof(kimix_vec) are returned so a caller that
 *     embeds the placeholder in its own struct can assert against it.
 * `out` must not be NULL; the struct is fully written on success. */
typedef struct kimix_layout_info {
    uint32_t abi_version;      /* == kimix_api_abi_version() */
    uint32_t _reserved;        /* always 0, do not rely on it */
    size_t size_of_size_t;     /* 4 or 8 */
    size_t size_of_pointer;    /* 4 or 8 */
    size_t size_of_bool;       /* 1 */
    size_t vec_abi_bytes;      /* KIMIX_VEC_BYTES: placeholder size a caller embeds */
    size_t vec_abi_align;      /* KIMIX_VEC_ALIGN: placeholder alignment */
    size_t vec_inner_bytes;    /* sizeof(kimix::vector<std::byte>) inside the library */
    size_t vec_inner_align;    /* its alignment */
} kimix_layout_info;

/* KIMIX_OK, or KIMIX_ERR_INVALID_ARG when `out` is NULL. */
KIMIX_FFI kimix_status kimix_api_layout_info(KIMIX_OUT kimix_layout_info *out);

KIMIX_FFI_END

#endif /* KIMIX_API_FFI_COMMON_H */
