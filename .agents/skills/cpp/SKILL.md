---
name: cpp
description: KimixBase C++ core library usage guide. Use when writing or editing C++ code in this project — covers namespace kimix, STL wrappers, Vector/Matrix, hashing, memory/pool, JSON repair, threading, platform, and I/O.
---

# KimixBase Core Library (`src/core/`)

All symbols live in `namespace kimix`. Include individual headers as needed: `#include <core/basic_types.h>`, `#include <core/string_scratch.h>`, etc. (`src/` is on the include path, so `<core/...>` and `"core/..."` both resolve.)

**Umbrella**: `#include <core/kimix_core.h>` pulls in every core header plus `kimix_version.h`, which xmake generates from `version.txt` (`KIMIX_CORE_VERSION`, `kimix::version_string`) — never hard-code a version. It does **not** include `json_repair.h`, `memory.h` or `rbc_concurrent_queue.h`; include those directly.

## 1. STL Wrappers (`stl/`)

Every standard container is aliased under `kimix::` with the **mimalloc allocator**. Use `kimix::vector`, `kimix::string`, `kimix::map`, etc. — never `std::vector` or `std::string` in kimix APIs (signatures and members).

| kimix alias       | Underlying type                     |
|----------------|-------------------------------------|
| `kimix::vector<T>`| `std::vector<T, kimix::allocator<T>>`  |
| `kimix::string`   | `std::basic_string<char, ..., kimix::allocator<char>>` |
| `kimix::map<K,V>` | `std::map<K,V, Compare, kimix::allocator<...>>` |
| `kimix::unordered_map<K,V>` | `ankerl::unordered_dense::map<K,V, kimix::hash<K>, std::equal_to<>, kimix::allocator<...>, kimix::vector<...>>` |
| `kimix::deque<T>` | `std::deque<T, kimix::allocator<T>>`   |
| `kimix::list<T>`  | `std::list<T, kimix::allocator<T>>`    |
| `kimix::set<T>`   | `std::set<T, Compare, kimix::allocator<T>>` |
| `kimix::unordered_set<K>` | `ankerl::unordered_dense::set<K, kimix::hash<K>, std::equal_to<>, kimix::allocator<K>, kimix::vector<K>>` |
| `kimix::queue<T>` | `std::queue<T, kimix::deque<T>>`       |
| `kimix::stack<T>` | `std::stack<T, kimix::deque<T>>`       |
| `kimix::priority_queue<T>` | `std::priority_queue<T, kimix::vector<T>, Compare>` |

`stl/unordered_dense.h` is the vendored ankerl header (no `kimix::unordered_dense` alias). There is no `kimix::hash<kimix::string>` specialization, so pass the hasher explicitly: `kimix::unordered_map<kimix::string, V, kimix::string_hash>`.

**Smart pointers and utilities**: `kimix::unique_ptr<T>`, `kimix::shared_ptr<T>`, `kimix::weak_ptr<T>`, `kimix::span<T>`, `kimix::optional<T>`, `kimix::variant<Ts...>`, `kimix::function<Sig>`, `kimix::move_only_function<Sig>` (falls back to `std::function` when the standard library has no `std::move_only_function`).

**Format**: Use `kimix::format(fmt, args...)` (C++20 `std::format`). `FMT_STRING(x)` is a no-op passthrough.

**String views**: `kimix::string_view` (= `std::string_view`), `kimix::u8string_view`, `kimix::u16string_view`, etc.

**Sstreams**: `kimix::ostringstream`, `kimix::istringstream`, `kimix::stringstream`.

**Other aliases**: `kimix::bitvector` (= `std::vector<bool>`), `kimix::fixed_vector<T,N>` (= `kimix::vector<T>`, `N` is ignored), `kimix::bit_cast<To,From>` (= `std::bit_cast`), `kimix::pointer_hash<T>` (hash pointer addresses), `kimix::align(size, alignment)` (round up to a power-of-two alignment). `stl/algorithm.h`, `stl/iterator.h`, `stl/type_traits.h` and `stl/pdqsort.h` are convenience re-includes of the standard headers (no `kimix::` wrappers).

**Allocator helpers** (`stl/memory.h`):
```cpp
T* p = kimix::new_with_allocator<T>(args...);   // allocate + construct
kimix::delete_with_allocator(p);                 // destruct + deallocate
T* p = kimix::allocate_with_allocator<T>(n);    // raw allocation
kimix::deallocate_with_allocator(p);            // raw deallocation
```

`kimix::allocator<T>` is stateless, compares equal to every other instantiation, and supports alignment via `allocate(n, alignment, offset = 0)`. It **never throws**: the project is built without C++ exceptions, so an exhausted allocator reports through `kimix::allocation_failure()` and aborts.

**Helper functions**:
```cpp
kimix::enlarge_by(vec, n);           // vec.resize(vec.size() + n)
kimix::size_bytes(vec);              // vec.size() * sizeof(T)
kimix::vector_resize(vec, size);    // vec.resize(size)
```

**Size literals** (in `kimix::size_literals`):
```cpp
using namespace kimix::size_literals;
size_t buf = 64_k;   // 64 * 1024
size_t heap = 16_M;  // 16 * 1024 * 1024
size_t big  = 2_G;   // 2 * 1024 * 1024 * 1024
```

### Custom data structures

**`kimix::fixed_map<Key, Value, N>`** — fixed-capacity map backed by `std::array`. O(N) lookup, no heap allocation.
```cpp
kimix::fixed_map<int, kimix::string, 8> fm;
fm[42] = "answer";
auto it = fm.find(42);
```

**`kimix::vector_map<Key, Value>`** — flat map stored in a sorted `kimix::vector` of pairs. Insertion uses binary search + insert, O(N) insertion, O(log N) lookup.
```cpp
kimix::vector_map<int, kimix::string> vm;
vm[42] = "answer";
```

**`kimix::lru_cache<Key, Value>`** — LRU cache backed by `kimix::list` + `kimix::unordered_map`.
```cpp
kimix::lru_cache<int, kimix::string> cache(100);
cache.put(1, "one");
auto v = cache.get(1);  // std::optional<kimix::string>
```

**Filesystem**: `kimix::filesystem` is a namespace alias for `std::filesystem`. `kimix::to_string(const filesystem::path&)` converts a path to `kimix::string`; `bool kimix::path_from_narrow(string_view, filesystem::path&)` builds a path from narrow bytes without throwing (returns false on Windows when the bytes are not representable in the native code page — treat that as "path does not exist", not as a hard error).

**`kimix::ring_buffer<T>`** — circular buffer backed by `kimix::vector`.
```cpp
kimix::ring_buffer<int> rb(64);
rb.push(1);
auto v = rb.pop();  // std::optional<int>
```

## 2. StringScratch (`string_scratch.h`)

Efficient string builder backed by `kimix::string` with stream-style `operator<<`:

```cpp
kimix::StringScratch ss;
ss << "Hello " << name << ", count = " << n << ", pi = " << 3.14f;
const kimix::string& result = ss.string();
kimix::string_view sv = ss.string_view();
const char* cstr = ss.c_str();
ss.clear();  // reuse buffer
```

Also `ss.reserve(n)`, `ss.size()`, `ss.empty()`; default initial capacity 512 bytes (`StringScratch(size_t)` to override).

**Free-standing helpers**: `kimix::scratch_append_format(ss, "value: %d", v)` (printf-style), `kimix::scratch_append_hex(ss, u64)`, `kimix::scratch_append_ptr(ss, ptr)` are defined in `string_scratch.cpp` but **not declared in any header**, so they are not usable from other translation units — prefer `kimix::format` / `operator<<`.

## 3. Basic Types & Traits

### Type aliases (`basic_traits.h`)
```cpp
kimix::byte   = int8_t;      kimix::ubyte  = uint8_t;
kimix::ushort = uint16_t;    kimix::uint   = uint32_t;
kimix::slong  = long long;   kimix::ulong  = unsigned long long;
kimix::half   = float;       // placeholder
```

### Type traits
- `kimix::is_integral<T>`, `kimix::is_floating_point<T>`, `kimix::is_boolean<T>`, `kimix::is_arithmetic<T>`, `kimix::is_signed<T>`, `kimix::is_unsigned<T>` (with `_v` helpers)
- `kimix::is_vector<T>`, `kimix::is_matrix<T>` (with `_v` helpers) — detect `kimix::Vector`/`kimix::Matrix`
- `kimix::is_integral_or_vector<T>`, `kimix::is_floating_point_or_vector<T>`, etc.
- `kimix::vector_element_t<T>`, `kimix::vector_dimension_v<T>` — extract element type and N
- `kimix::always_false<T...>` / `kimix::always_true<T...>` — for `static_assert` with dependent types
- `kimix::to_underlying(e)` — `static_cast` to underlying enum type

### Concepts (`concepts.h`)
C++20 concept wrappers: `kimix::arithmetic<T>`, `kimix::floating_point<T>`, `kimix::integral<T>`, `kimix::signed_integral<T>`, `kimix::unsigned_integral<T>`, `kimix::boolean<T>`, `kimix::enum_type<T>`, `kimix::pointer_type<T>`, `kimix::same_as<T,U>`, `kimix::derived_from<D,B>`, `kimix::convertible_to<F,T>`, `kimix::destructible<T>`, `kimix::constructible_from<T,Args...>`, `kimix::default_initializable<T>`, `kimix::move_constructible<T>`, `kimix::copy_constructible<T>`, `kimix::equality_comparable<T>`, `kimix::totally_ordered<T>`, `kimix::assignable_from<T,U>`.

## 4. Vector & Matrix (`basic_types.h`)

### Vector<T, N>
```cpp
kimix::float3 v{1.0f, 2.0f, 3.0f};
kimix::float4 u = kimix::float4(0.5f);     // all components = 0.5
float x = v.x();  // requires N>=1
float y = v.y();  // requires N>=2
float z = v.z();  // requires N>=3
float w = v.w();  // requires N>=4

auto sum = v + u;      // component-wise
auto diff = v - 0.5f;  // scalar broadcast
auto prod = 2.0f * v;
v += u;                 // compound assignment

auto cmp = v < u;        // returns kimix::bool3
bool any_true = kimix::any(cmp);
bool all_true = kimix::all(cmp);
bool none_true = kimix::none(cmp);
```

**Type aliases**: `kimix::bool2/3/4`, `kimix::float2/3/4`, `kimix::double2/3/4`, `kimix::int2/3/4` (`int32_t`), `kimix::uint2/3/4` (`uint32_t`), `kimix::short2/3/4` (`int16_t`), `kimix::ushort2/3/4`, `kimix::byte2/3/4` (`int8_t`), `kimix::ubyte2/3/4`, `kimix::long2/3/4` (`int64_t`), `kimix::ulong2/3/4` (`uint64_t`), `kimix::half2/3/4`. There is no `slong2/3/4` (the 64-bit signed alias is `long2/3/4`).

All arithmetic (`+`, `-`, `*`, `/`, `%`, `&`, `|`, `^`), compound (`+=`, `-=`, etc.) and comparison (`<`, `>`, `<=`, `>=`, `==`, `!=`) operators exist in vector-vector, vector-scalar and scalar-vector form (scalar broadcast, `requires std::is_convertible_v<U, T>`); comparisons yield `Vector<bool, N>`. Unary `+`, `-`, `~` (integral only) and `!` are members. `&&` / `||` are defined **only** between two `Vector<bool, N>` — no scalar broadcast. Hash specialization via `kimix::hash<kimix::Vector<T,N>>` (defined in `basic_types.h`, like the `Matrix` one).

### Matrix<T, N>
```cpp
kimix::Matrix<float, 4> m;              // zero matrix
kimix::Matrix<float, 4> identity(1.0f); // identity * 1.0
m[2][1] = 3.0f;  // col 2, row 1

auto m2 = m * identity;               // matrix multiply
auto v2 = m * kimix::float4{1,0,0,0};   // matrix * vector
auto sum = m + identity;
```

Square matrices only (`Matrix<T, N>` is `N×N`); type aliases `kimix::float2x2` / `float3x3` / `float4x4`. Members: `operator*` (matrix and vector), free `+` / `-`, `cols[N]`, `hash<Matrix<T,N>>`. There is no `Matrix * scalar`.

## 5. Mathematics (`mathematics.h`)

```cpp
kimix::clamp(value, lo, hi);          // clamp to [lo, hi]
kimix::lerp(a, b, t);                 // linear interpolation (floating_point only)
kimix::min(a, b); kimix::max(a, b);
kimix::sign(val);                     // -1, 0, or 1
kimix::saturate(x);                   // clamp to [0, 1] (floating_point)
kimix::smoothstep(edge0, edge1, x);   // Hermite interpolation
kimix::frac(value);                   // fractional part
kimix::next_pow2(v);                  // round up to next power of 2
kimix::is_pow2(v);                    // is power of 2?
kimix::radians(deg); kimix::degrees(rad);
kimix::floor_div(a, b); kimix::ceil_div(a, b);  // integer floor/ceil division
kimix::align_up(value, alignment); kimix::align_down(value, alignment);  // power-of-two alignment
```

### Constants (`constants.h`)
```cpp
kimix::pi, kimix::inv_pi, kimix::pi_over_two, kimix::pi_over_four, kimix::two_pi, kimix::inv_two_pi
kimix::sqrt2, kimix::inv_sqrt2, kimix::e, kimix::log2e, kimix::log10e, kimix::ln2, kimix::ln10
// float versions exist only for the pi family: kimix::pi_f, inv_pi_f, pi_over_two_f, pi_over_four_f, two_pi_f, inv_two_pi_f
```

## 6. Hashing (`stl/hash.h`)

Uses **XXH3** (xxHash). Default seed: `kimix::hash64_default_seed` = `2^61 - 1`.

```cpp
uint64_t h = kimix::hash64(data_ptr, size);         // raw bytes
uint64_t h = kimix::hash64(data_ptr, size, seed);
uint64_t h = kimix::hash_value(obj);                // sizeof(T) bytes
uint64_t h = kimix::hash_value(obj, seed);

// Combine multiple hashes
uint64_t combined = kimix::hash_combine({h1, h2, h3});

// 128-bit hash
kimix::Hash128 h128 = kimix::hash128(data, size);
kimix::string hex = h128.to_string();            // 32-char hex string
```

`hash64`, `hash64_default_seed` and the primary `hash<T>` template live in `stl/hash_fwd.h` (implemented in `hash.cpp` with `XXH3_64bits_withSeed` / `XXH3_128bits_withSeed`).

**`kimix::hash<T>`** is specialized for: arithmetic types, pointers, enums, C-strings, `std::string`, `std::string_view`, `kimix::Vector`, `kimix::Matrix`, and any type with a `.hash()` method returning `uint64_t`.

**String hashing** (`stl/string.h`): `kimix::string_hash`, `kimix::wstring_hash`, `kimix::u8string_hash`, etc. — typed hash functors for each string type.

**Character traits**: `kimix::is_char<T>`, `kimix::is_char_v<T>` — true for `char`, `wchar_t`, `char8_t`, `char16_t`, `char32_t`.

## 7. Clock (`clock.h`)

```cpp
kimix::Clock clock;                   // starts on construction
double ms = clock.toc();           // elapsed ms (does not reset)
double sec = clock.toc_seconds();  // elapsed seconds
double ms2 = clock.toc_reset();    // elapsed ms + reset
clock.reset();                     // manual reset
double now = kimix::Clock::now_ms();  // steady_clock ms since its epoch (static)
```

## 8. Thread Safety

### `kimix::spin_mutex`
Lightweight atomic spinlock with CPU pause hint:
```cpp
kimix::spin_mutex mtx;
std::lock_guard<kimix::spin_mutex> lock(mtx);
```

### `kimix::conditional_mutex_t<bool ThreadSafe, typename Mutex>`
Real mutex when `ThreadSafe=true`, no-op when `false`. Useful for optionally-thread-safe data structures.

### `kimix::thread_safety<Mutex>`
Mutex mix-in providing the `with_lock(f)` pattern (`f()` runs under a `std::lock_guard<Mutex>`; a `const` overload exists, the mutex is `mutable`). Also `mutex()` for direct access:
```cpp
class MyClass : public kimix::thread_safety<std::mutex> {
    void do_work() {
        with_lock([&] { /* critical section */ });
    }
};
```

### `rbc::ConcurrentQueue<T>` (`rbc_concurrent_queue.h`)
Lock-free multi-producer/multi-consumer queue (vendored moodycamel implementation in `core/detail/concurrent_queue.h`) with `rbc::RBCConcurrentQueueTraits`, whose `malloc`/`free` go to mimalloc. `rbc::ProducerToken` / `rbc::ConsumerToken` are re-exported. **Not** in `namespace kimix` and **not** in the umbrella header — include `core/rbc_concurrent_queue.h` explicitly.

## 9. JSON Repair (`json_repair.h`)

`kimix::repair(kimix::string_view json) -> kimix::vector<char>` — tolerant re-parse of malformed LLM tool-call JSON into strictly valid JSON (validated with yyjson before it is returned, so invalid output is never emitted):

```cpp
#include <core/json_repair.h>          // not part of kimix_core.h
auto repaired = kimix::repair(text);   // empty => text is already valid JSON
if (!repaired.empty()) {
    kimix::string_view json = kimix::repaired_view(repaired);  // drops the trailing '\0'
}
```

The returned buffer is **NUL-terminated** (last element `'\0'`, so `data()` feeds C-string APIs; the text is `size() - 1` bytes) — `repaired_view()` gives the view without the terminator. Handled malformations include BOM/comments/markdown fences and prose chatter, single/smart/backtick quotes, unquoted keys and values, Python literals (`True/False/None`, `NaN`, `Infinity`), odd number forms (leading `+`, `0x..`, `.5`, `1.`, dangling exponent, thousands separators), missing/duplicate colons and commas (`;`, `=>`, `->` as separators/aliases), full-width CJK punctuation (`：`, `，`) and invisible whitespace (`12 345` → `12345`), truncated containers, raw control characters/newlines in strings and bad escapes. Empty/whitespace-only/garbage input repairs to `"null"`.

## 10. Memory Management

### `kimix::Pool<T, ThreadSafe=true>`
Object pool with 64-element blocks (`block_size = 64`), LIFO free list, `T` must be at least `sizeof(void*)`. Thread-safe by default (`Pool<T, false>` drops the spinlock). Also `allocate()` / `deallocate(ptr)` for raw (unconstructed) memory. Neither copyable nor movable.
```cpp
kimix::Pool<MyObject> pool;
MyObject* obj = pool.create(args...);  // allocate + construct
pool.destroy(obj);                      // destruct + deallocate
size_t n = pool.allocated_count();
```

### `kimix::FirstFit`
First-fit / best-fit allocator over a contiguous region.
```cpp
kimix::FirstFit allocator(1_M);         // 1 MiB region
auto* node = allocator.allocate(128); // first-fit, 16-byte aligned
node = allocator.allocate_best_fit(256);
allocator.free(node);
allocator.dump_free_list();          // debug dump to stderr

// Iteration over free list:
for (auto& node : allocator) { /* node.offset, node.size */ }
```

Move-only (copy deleted, move defined). `allocate()` rounds the size up to 16 bytes and returns `nullptr` when the region is exhausted; `free()` coalesces adjacent nodes. Nodes carry `offset` / `size` / `next` / `prev`. A `kimix::string`-returning `dump_free_list(const FirstFit&)` exists in `first_fit.cpp` but is not declared in any header, so the member `dump_free_list()` (stderr) is the only usable dump.

### Aligned allocation (`platform.h`)
```cpp
void* p = kimix::aligned_alloc(alignment, size);
kimix::aligned_free(p);
size_t ps = kimix::pagesize();
```

### `kimix::IOperatorNewBase` (`memory.h`)
Stateless base struct that overrides `operator new` / `new[]` / `delete` to route through mimalloc (`mi_malloc` / `mi_free`), so derived objects never touch the CRT heap. Not part of `kimix_core.h`; include `core/memory.h`.

## 11. File I/O

### `kimix::BinaryFileStream` (read)
```cpp
kimix::BinaryFileStream stream("path/to/file.bin");
if (stream) {
    auto data = stream.read_all();           // kimix::vector<kimix::byte>
    // or incremental:
    kimix::byte buf[1024];
    size_t n = stream.read(buf);
    stream.set_pos(0);                       // seek
}
```

Also `length()`, `position()`, `is_open()`, `close()`, `explicit operator bool()`; constructible from an existing `FILE*`; move-only. Opening a missing file leaves the stream closed (`!stream`) instead of throwing.

### `kimix::BinaryFileWriteStream` (write) — not header-visible
`write(const void*, size_t)` / `write(std::span<const kimix::byte>)` / `close()` / `is_open()` are defined **only inside `binary_file_stream.cpp`** and declared in no header, so they are not usable from other translation units. Use `BinaryFileStream` + `FILE*` (`fopen`), `std::ofstream`, or `kimix::DefaultBinaryIO` for writing.

### `kimix::BinaryIO` / `kimix::DefaultBinaryIO`
Abstract interface for shader source/cache I/O:
```cpp
kimix::DefaultBinaryIO io(".cache");
kimix::string src;
io.read_shader_source("shader.glsl", src);
io.write_shader_cache("shader.bin", byte_span);
```

## 12. Dynamic Module Loading

```cpp
kimix::DynamicModule mod;
// load() hands the string straight to LoadLibraryA / dlopen: it must be a real
// file name or path, extension included. It does NOT append ".dll"/".so" and it
// does NOT consult the search paths.
if (mod.load("myplugin.dll") || mod.load("C:/plugins/myplugin.dll")) {
    auto fn = mod.function<void(int)>("my_function");  // kimix::function<sig>, null if missing
    if (fn) fn(42);
    mod.unload();
}

// kimix::dynamic_module_name("myplugin") -> "myplugin.dll" (Windows) / "libmyplugin.so" (Unix).
// The static path list (add_search_path / remove_search_path / reset_search_paths /
// get_search_paths) is bookkeeping only — load() does not use it.
kimix::DynamicModule::add_search_path("C:/plugins");
```

## 13. Platform Utilities

```cpp
kimix::string cpu = kimix::cpu_name();
kimix::string exe = kimix::current_executable_path();
char sep = kimix::env_separator();  // ';' on Windows, ':' on Unix
kimix::debug_break();               // __debugbreak / __builtin_trap

// Stack trace (returns empty vector — TODO)
auto trace = kimix::backtrace();  // kimix::vector<kimix::TraceItem>
```

Platform macros: `KIMIX_PLATFORM_WINDOWS`, `KIMIX_PLATFORM_APPLE`, `KIMIX_PLATFORM_UNIX` — `platform.h` also derives them from `_WIN32` / `__APPLE__` / `__linux__`, and the `kimix-core` target defines the right one publicly for every consumer. Also available: `kimix::dynamic_module_load/destroy/find_symbol` and `kimix::dynamic_module_name` (module loading, §12).

## 14. Linkage Macros (`dll_export.h`)

Four independent three-state families, all with the same pattern: `*_STATIC` → expands to nothing, `*_EXPORT_DLL` → `__declspec(dllexport)` / `__attribute__((visibility("default")))`, neither → `__declspec(dllimport)` on Windows, nothing elsewhere.

| Macro | Library | Static / export defines |
|---|---|---|
| `KIMIX_CORE_API` | `kimix-core` (static) | `KIMIX_CORE_STATIC` / `KIMIX_CORE_EXPORT_DLL` |
| `KIMIX_LLM_API` | `kimix-llm` (static) | `KIMIX_LLM_STATIC` / `KIMIX_LLM_EXPORT_DLL` |
| `KIMIX_RUNTIME_API` | `runtime_py` (shared `.pyd`) | `KIMIX_RUNTIME_STATIC` / `KIMIX_RUNTIME_EXPORT_DLL` |
| `KIMIX_API_API` | `kimix_api` (the plain-C FFI in `src/api`, docs in `docs/ffi.md`) | `KIMIX_API_STATIC` / `KIMIX_API_EXPORT_DLL` |

Tag every exported declaration with the family of the library it belongs to. `kimix-core` itself is compiled with `KIMIX_CORE_EXPORT_DLL` (so `runtime_py` re-exports e.g. `hash64`), while every consumer — `kimix-llm`, `kimix-cli`, `kimix-test`, `test_proj`, `runtime_py`, `kimix_api` — defines `KIMIX_CORE_STATIC`; the xmake targets propagate the right one, so never add these defines by hand.

## Naming

- **Classes / structs / enums**: `CamelCase` (`MyClass`, `ToolRegistry`)
- **Functions & public vars**: `snake_case` (`get_value`, `process_data`)
- **Private/protected members & functions**: `_snake_case` (`_private_var`, `_internal_helper()`)
- **Constants**: `kCamelCase` or `UPPER_SNAKE_CASE` for macros
- **Template params**: `CamelCase`
- **Namespaces**: `kimix`, `kimix::runtime`, `kimix::llm`, `kimix::agent`, `kimix::builtin_tools`, `kimix::cli` (plus `rbc` for the vendored queue). Keep compact.

## Syntax Check

Use the project C++ syntax checker:

```bash
python scripts/check_cpp_syntax.py <file>.cpp
```

It starts a real `clangd` LSP server (`--clang-tidy=true`) and prints diagnostics for that one file. Flags: `--project-root`, `--clangd`, `--compile-commands-dir`, `-v`. `compile_commands.json` is generated into `.vscode/` by `xmake project -k compile_commands --lsp=clangd .vscode` (the script looks in `.vscode/`, then `build/`, and fails if neither exists); files missing from that database get clangd's fallback flags, so trust its diagnostics only for listed files. See also `scripts/check_all_cpp_syntax.py` (parallel check of every file in the database).

## Formatting

There is **no `.clang-format` in the repository** (and none bundled with this skill), so formatting is by convention. The style the existing code follows (LLVM base + these overrides):

- **Indent**: 4 spaces, no tabs. Continuation indent 4. Case labels indented. Preprocessor indent 2.
- **Braces**: K&R (attach). No break before braces. Indent braces off.
- **Line width**: unlimited (`ColumnLimit: 0`).
- **Pointers/refs**: right-aligned (`int *p`, `int &r`).
- **Access modifiers**: indent offset `-4` (flush with `class`). Empty lines before/after left as-is.
- **Short constructs**: allow single-line for short blocks, functions, ifs, loops, lambdas, enums, case labels.
- **Constructor init**: not forced one-per-line; no break before comma.
- **Templates / concepts**: break declarations only when multiline; indent requires clause.
- **Spaces**: before `=`, ctor-initializer `:`, inheritance `:`, range-for `:`. No space after C-style casts, `!`, `template` keyword, before braced lists. No space in empty parens or before trailing comments.
- **Alignment**: after open brackets & operands; don't align consecutive assignments.
- **Includes/using**: never auto-sort.
- **Namespaces**: compact single-line when short; no indentation inside (`ShortNamespaceLines: 0`).
- **Strings/comments**: break string literals; don't reflow comments.
- **File-header comment**: most core headers/sources open with a `/* name.h — API summary + example */` block (e.g. `stl/memory.h`, `hash.h`, `pool.h`, `json_repair.h`); read it before editing and keep it in sync with the API it documents — but trust the code over it.

## Static Analysis

There is **no `.clang-tidy` in the repository** either. `scripts/check_cpp_syntax.py` starts clangd with `--clang-tidy=true`, so you see clangd's built-in default checks and nothing else; adding a `.clang-tidy` at the project root would narrow or widen that set.

## No RTTI, No Exceptions

RTTI is disabled for project code (`kimix_rtti=false` → `/GR-` on MSVC, `-fno-rtti` on GCC/Clang). Do **not** use:

- `dynamic_cast` — use `static_cast` when type is known
- `typeid`
- `std::type_info`

Prefer virtual dispatch or explicit type tags for type-safe downcasting. Third-party code under `src/ext` is exempt.

C++ exceptions are disabled too (`kimix_enable_exception=false`, `KIMIX_NO_EXCEPTIONS` defined on every library target). Do **not** use:

- `throw` / `try` / `catch` — report failures through return values (`bool` + out-parameter, `std::optional`, error enums, `kimix_status` at the C boundary)
- throwing standard-library calls (`std::stoi`, `.at()`, `std::filesystem` throwing overloads, `path` construction from unrepresentable bytes — use `kimix::path_from_narrow`); format errors abort inside the `noexcept` `kimix::format`

The only targets that keep exceptions are those listed in `kimix_exceptions_targets` (`runtime_py`, `test_pybind11` — the pybind11 layer needs them) plus the Boost.UT test binaries, where `test_proj` in `tests/xmake.lua` sets `kimix_enable_exception` per target. Third-party code under `src/ext` is exempt.

## Integer Types

Prefer fixed-width integer types:

- Use: `int32_t`, `uint32_t`, `int64_t`, `uint64_t`, `int16_t`, `uint16_t`, `int8_t`, `uint8_t`
- `size_t` is acceptable for sizes/indices per STL convention.
- Prefer `std::byte` for raw byte data (`kimix::vector<std::byte>` is the runtime/FFI convention, e.g. `src/api/detail.h`). Note core file I/O (`BinaryFileStream`, `BinaryIO`) still speaks `kimix::byte` (= `int8_t`).
- Avoid `unsigned int`, `long long`, `unsigned long`, `short`, and `char` for arithmetic. `kimix::byte/ubyte/ushort/uint/slong/ulong` (and `half`) exist in `basic_traits.h` but are essentially unused in `src/` — prefer the fixed-width `std::` types in new code.
