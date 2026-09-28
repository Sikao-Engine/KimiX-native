/*
 * kimix_api.h -- umbrella header of the kimix_api C FFI library (src/api).
 *
 * Include THIS header (or one individual area header) from C or C++ and link
 * against kimix_api.dll / libkimix_api.so, or resolve the symbols at run time
 * after dlopen()/LoadLibrary() (define KIMIX_API_STATIC for the second case).
 *
 *     #include <api/kimix_api.h>
 *     ...
 *     kimix_vec out;
 *     if (kimix_vec_default_init(&out) == KIMIX_OK) {
 *         kimix_vec_append(&out, "hi", 2);
 *         size_t n = kimix_vec_size(&out);        // 2
 *         kimix_vec_destroy(&out);                // mandatory
 *     }
 *
 * The areas (each one is a self-contained pair of <header, translation unit>):
 *   <api/ffi_common.h>   linkage plumbing, ABI identity, kimix_status,
 *                        version/layout queries          -> kimix_api.cpp
 *   <api/ffi_mem.h>      mimalloc allocation (one heap for the whole library)
 *                                                       -> ffi_mem.cpp
 *   <api/ffi_vec.h>      kimix::vector<std::byte> in caller-owned storage
 *                                                       -> ffi_vec.cpp
 *   <api/ffi_yyjson.h>   yyjson parse / build / write with the mimalloc
 *                        allocator baked in (no yyjson_alc in the surface)
 *                                                      -> ffi_yyjson.cpp
 *   <api/ffi_repair.h>   kimix::repair(): fix malformed LLM JSON, results
 *                        delivered into a kimix_vec     -> ffi_repair.cpp
 *
 * Rules that apply to the whole surface (see the file headers for the details):
 *   - One heap.  Anything the library allocates is mimalloc memory owned by
 *     this library and must be released with the matching free/destroy call of
 *     this library, never with the caller's own malloc/free.
 *   - Caller-owned inline objects (kimix_vec) must be initialised exactly once
 *     and destroyed exactly once; a violation is reported as
 *     KIMIX_ERR_INVALID_STATE rather than corrupting memory.
 *   - No exceptions and no RTTI cross the boundary: every fallible call returns
 *     kimix_status and reports details through out-parameters.
 *   - C strings are NUL-terminated UTF-8; lengths are in BYTES, never in code
 *     points.  A `char *`/`size_t` pair may be replaced by a plain NUL-terminated
 *     string where the function offers a `_str` / `_n` spelling.
 *   - Pointers returned by the library are BORROWED (valid until the owning
 *     object is freed or mutated) unless the documentation of the function says
 *     the caller takes ownership.
 *
 * The searchable index of every function, with signatures and ownership notes,
 * is docs/ffi.md.
 */
#pragma once
#ifndef KIMIX_API_KIMIX_API_H
#define KIMIX_API_KIMIX_API_H

#include <api/ffi_common.h>
#include <api/ffi_mem.h>
#include <api/ffi_vec.h>
#include <api/ffi_repair.h>
#include <api/ffi_yyjson.h>

#endif /* KIMIX_API_KIMIX_API_H */
