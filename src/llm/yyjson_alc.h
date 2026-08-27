// yyjson_alc.h - Shared mimalloc-backed allocator for yyjson consumers.
//
// yyjson copies the supplied yyjson_alc into doc->alc at
// yyjson_mut_doc_new(), so a single shared const instance is safe: mi_* are
// thread-safe and the struct is read-only after init. C++17 inline variable
// gives one definition across TUs (needed because the parser headers are
// header-only and unit tests link only kimix-core, which provides the mi_*
// symbols).

#pragma once

#include <mimalloc.h>

#include "yyjson.h"

namespace kimix::llm {

// Single mimalloc-backed yyjson_alc (type defined in third-party yyjson.h).
inline const yyjson_alc kYYJsonAlcMi{
    /*malloc  =*/ [](void *, size_t size) -> void * { return mi_malloc(size); },
    /*realloc =*/ [](void *, void *p, size_t /*old_size*/, size_t size) -> void * { return mi_realloc(p, size); },
    /*free    =*/ [](void *, void *p) { mi_free(p); },
    /*ctx     =*/ nullptr,
};

} // namespace kimix::llm
