// yyjson_alc.h - Shared kimix::allocate-backed allocator for yyjson consumers.
//
// yyjson copies the supplied yyjson_alc into doc->alc at
// yyjson_mut_doc_new(), so a single shared const instance is safe: the
// kimix::allocate / kimix::reallocate / kimix::deallocate gateway is
// thread-safe and the struct is read-only after init. C++17 inline variable
// gives one definition across TUs (needed because the parser headers are
// header-only and unit tests link only kimix-core, which provides the gateway
// symbols).
//
// Contract: every block yyjson takes from this alc is released through this
// same alc, and a writer buffer handed to the caller is released with
// kimix::deallocate -- never with the CRT free() and never with a bare mi_free
// (which would bypass the KIMIX_MEMORY_STRICT tracker and, on a different
// module, the heap that owns it).
#pragma once

#include <core/memory.h>
#include "yyjson.h"

namespace kimix::llm {

// Single gateway-backed yyjson_alc (type defined in third-party yyjson.h).
inline const yyjson_alc kYYJsonAlcMi{
    /*malloc  =*/ [](void *, size_t size) -> void * { return kimix::allocate(size); },
    /*realloc =*/ [](void *, void *p, size_t /*old_size*/, size_t size) -> void * { return kimix::reallocate(p, size); },
    /*free =*/ [](void *, void *p) { kimix::deallocate(p); },
    /*ctx =*/ nullptr,
};

} // namespace kimix::llm
