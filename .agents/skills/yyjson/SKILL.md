---
name: yyjson
description: Guide for using the vendored yyjson JSON library in KimixBase. Use when writing or editing C++ code that parses, builds, or serializes JSON with yyjson, especially when choosing the mimalloc-backed allocator.
---

# yyjson

KimixBase vendors upstream yyjson under `src/ext/yyjson` — unmodified submodule `ibireme/yyjson`, tag **0.13.0** (`YYJSON_VERSION_STRING`, `yyjson.h:590`). It is wrapped by the `kimix-yyjson` xmake target (static lib, complete `yyjson.c`, **no** `YYJSON_DISABLE_*` defines — reader, writer, file and utils APIs all compiled) and linked through `kimix-core`.

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
- The short forms `yyjson_read` / `yyjson_write` / `yyjson_mut_write` are inline wrappers that pass `alc == NULL`, so yyjson falls back to the **libc** allocator. The kimix idiom is always the `*_opts` variant with `kYYJsonAlcMi` (`src/llm/common.cpp`, `src/runtime/codec/wire_envelope.cpp`).
- Exception: kimix-core code cannot include `llm/yyjson_alc.h` (wrong dependency direction), so `src/core/json_repair.cpp:865` uses plain `yyjson_read` — safe only because the doc is parsed, validated and freed there without memory crossing the pyd boundary (`yyjson_doc_free` returns it to libc via the recorded allocator).
- The C FFI (`src/api/ffi_yyjson.h`) exposes the same JSON surface as `kimix_yyjson_*` with the allocator *hidden*: every call bakes in `kimix::api::yyjson_mi_alc()` (`src/api/detail.h:175`), no `yyjson_alc` crosses the ABI, and writer buffers are released with `kimix_yyjson_str_free()` — never `free()`, never `mi_free()` resolved in another module.

## Immutable vs mutable documents

- Parse with `yyjson_read_opts(..., &kYYJsonAlcMi, ...)` → `yyjson_doc*` (immutable).
- Build with `yyjson_mut_doc_new(&kYYJsonAlcMi)` → `yyjson_mut_doc*` (mutable).
- Convert immutable → mutable with `yyjson_doc_mut_copy(doc, &kYYJsonAlcMi)`.
- Convert mutable → immutable with `yyjson_mut_doc_imut_copy(doc, &kYYJsonAlcMi)` (or `yyjson_mut_val_imut_copy` for a single value); both exist in the vendored 0.13.0 (`yyjson.h:2532,2541`). Free the result with `yyjson_doc_free`.

## Kimix glue (prefer these in llm/agent code)

`src/llm/common.h` wraps the recurring yyjson moves; all of them route through `kYYJsonAlcMi`:

- `add_json_str(doc, obj, key, sv)` — length-explicit string add; an embedded `'\0'` survives as `\u0000` (yyjson's own `add_str` helpers are `strlen`-based).
- `add_json_fragment(doc, obj, key, raw)` — parse-once-and-embed with UTF-8 sanitizing (same idea as the pattern below).
- `write_json_doc(doc, err)` — serialize a mut doc to `kimix::string`; `err` carries the `yyjson_write_err` reason.
- `sanitize_tool_arguments` truncates trailing garbage with `YYJSON_READ_STOP_WHEN_DONE` (`1 << 1`, `yyjson.h:829`).

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

The envelope itself no longer serializes through `yyjson_*_write_opts`: `write_json_into` (`src/runtime/codec/wire_envelope.cpp:66-74`) writes with the allocation-free `yyjson_mut_write_buf` / `yyjson_val_write_buf` (added in 0.13.0, `yyjson.h:1593,1728`) into a caller-owned `kimix::string`, sidestepping the output-buffer ownership question entirely.

## Testing

Unit tests that exercise yyjson live in `tests/unit/ext/test_yyjson.cpp`. They show reading, writing, error handling, and direct `mi_malloc`/`mi_free` checks.

## Things to avoid

- Do not mix allocator families: buffers written with `kYYJsonAlcMi` must be freed with `mi_free`, and documents created with `kYYJsonAlcMi` must be freed with `yyjson_doc_free`/`yyjson_mut_doc_free`.
- Do not expect a `yyjson_mut_read` document parser: it does not exist in yyjson 0.13.0 (the only `yyjson_mut_read_*` is `yyjson_mut_read_number`, an inline alias of `yyjson_read_number`, `yyjson.h:1228`). Parse immutable with `yyjson_read_opts`, then `yyjson_doc_mut_copy`.
- Keep the dependency graph clean: only `kimix-core` depends on `kimix-yyjson` and `mimalloc` directly.
