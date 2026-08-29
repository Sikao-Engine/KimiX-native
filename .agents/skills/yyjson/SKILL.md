---
name: yyjson
description: Guide for using the vendored yyjson JSON library in KimixBase. Use when writing or editing C++ code that parses, builds, or serializes JSON with yyjson, especially when choosing the mimalloc-backed allocator.
---

# yyjson

KimixBase vendors a trimmed yyjson fork under `src/ext/yyjson`. It is wrapped by the `kimix-yyjson` xmake target and linked through `kimix-core`.

## When to use this skill

- Adding JSON parsing, mutation, or writing in C++.
- Deciding whether to pass a custom allocator.
- Debugging memory ownership with `yyjson_read_opts` / `yyjson_mut_write_opts` output buffers.

## Include and dependency

Source files use the vendored header directly:

```cpp
#include "yyjson.h"
```

The `kimix-core` xmake target already depends on `kimix-yyjson`, so any target that depends on `kimix-core` gets the include path automatically. Do **not** add a direct `add_deps("kimix-yyjson")` from other targets.

## Mimalloc-backed allocator

The project shares a single mimalloc heap across `runtime_py.pyd` and native C++ tests. yyjson allocations therefore go through mimalloc to avoid cross-heap frees.

Use the shared allocator defined in `src/llm/yyjson_alc.h`:

```cpp
#include <llm/yyjson_alc.h>

// Read
yyjson_doc* doc = yyjson_read_opts(
    (char*)data.data(), data.size(), 0,
    &kimix::llm::kYYJsonAlcMi, nullptr);

// Write
size_t len = 0;
char* text = yyjson_mut_write_opts(
    doc, 0, &kimix::llm::kYYJsonAlcMi, &len, nullptr);
if (text) {
    out.assign(text, len);
    mi_free(text);   // written with mimalloc allocator -> free with mi_free
}
```

`kYYJsonAlcMi` is an `inline const yyjson_alc` in namespace `kimix::llm`:

```cpp
inline const yyjson_alc kYYJsonAlcMi{
    /*malloc  =*/ [](void*, size_t size) -> void* { return mi_malloc(size); },
    /*realloc =*/ [](void*, void* p, size_t /*old_size*/, size_t size) -> void* { return mi_realloc(p, size); },
    /*free    =*/ [](void*, void* p) { mi_free(p); },
    /*ctx     =*/ nullptr,
};
```

Key points:

- The allocator is read-only after init; yyjson copies it into the document, so one global instance is safe.
- Any buffer returned by `yyjson_*_write_opts` with `kYYJsonAlcMi` must be freed with `mi_free`, not `free`.
- `yyjson_doc_free` / `yyjson_mut_doc_free` use the allocator stored in the document, so they route through mimalloc automatically.

## Immutable vs mutable documents

- Parse with `yyjson_read_opts(..., &kYYJsonAlcMi, ...)` → `yyjson_doc*` (immutable).
- Build with `yyjson_mut_doc_new(&kYYJsonAlcMi)` → `yyjson_mut_doc*` (mutable).
- Convert immutable → mutable with `yyjson_doc_mut_copy(doc, &kYYJsonAlcMi)`.
- Convert mutable → immutable with `yyjson_mut_doc_imut_copy` if available.

## Pattern: embed a pre-parsed payload without re-escaping

See `src/runtime/codec/wire_envelope.cpp`:

```cpp
yyjson_doc* parsed = yyjson_read_opts(payload_data, payload_len, 0, &kYYJsonAlcMi, nullptr);
if (parsed) {
    yyjson_mut_doc* payload_doc = yyjson_doc_mut_copy(parsed, &kYYJsonAlcMi);
    yyjson_doc_free(parsed);
    yyjson_mut_obj_add_val(doc, root, "payload", yyjson_mut_doc_get_root(payload_doc));
    // keep payload_doc alive until the envelope is written, then free it.
}
```

## Testing

Unit tests that exercise yyjson live in `tests/unit/ext/test_yyjson.cpp`. They show reading, writing, error handling, and direct `mi_malloc`/`mi_free` checks.

## Things to avoid

- Do not mix allocator families: buffers written with `kYYJsonAlcMi` must be freed with `mi_free`, and documents created with `kYYJsonAlcMi` must be freed with `yyjson_doc_free`/`yyjson_mut_doc_free`.
- Do not rely on `yyjson_mut_read`; the vendored fork does not provide it.
- Keep the dependency graph clean: only `kimix-core` depends on `kimix-yyjson` and `mimalloc` directly.
