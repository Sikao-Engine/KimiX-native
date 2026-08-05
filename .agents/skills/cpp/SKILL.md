---
name: cpp
description: KimixBase C++ core library usage guide. Use when writing or editing C++ code in this project — covers namespace kimix, STL wrappers, Vector/Matrix, memory, platform, and I/O.
---

# KimixBase Core Library (`src/core/`)

All symbols live in `namespace kimix`. Include individual headers as needed: `#include <core/basic_types.h>`, `#include <core/string_scratch.h>`, etc.

## 1. STL Wrappers (`stl/`)

Every standard container is aliased under `kimix::` with the **mimalloc allocator**. Use `kimix::vector`, `kimix::string`, `kimix::map`, etc. — never `std::vector` or `std::string`.

| kimix alias       | Underlying type                     |
|----------------|-------------------------------------|
| `kimix::vector<T>`| `std::vector<T, kimix::allocator<T>>`  |
| `kimix::string`   | `std::basic_string<char, ..., kimix::allocator<char>>` |
| `kimix::map<K,V>` | `std::map<K,V, Compare, kimix::allocator<...>>` |
| `kimix::unordered_map<K,V>` | `std::unordered_map<K,V, kimix::hash<K>, ..., kimix::allocator<...>>` |
| `kimix::deque<T>` | `std::deque<T, kimix::allocator<T>>`   |
| `kimix::list<T>`  | `std::list<T, kimix::allocator<T>>`    |
| `kimix::set<T>`   | `std::set<T, Compare, kimix::allocator<T>>` |
| `kimix::unordered_set<T>` | `std::unordered_set<T, kimix::hash<T>, ..., kimix::allocator<T>>` |
| `kimix::queue<T>` | `std::queue<T, kimix::deque<T>>`       |
| `kimix::stack<T>` | `std::stack<T, kimix::deque<T>>`       |
| `kimix::priority_queue<T>` | `std::priority_queue<T, kimix::vector<T>>` |

**Smart pointers and utilities**: `kimix::unique_ptr<T>`, `kimix::shared_ptr<T>`, `kimix::weak_ptr<T>`, `kimix::span<T>`, `kimix::optional<T>`, `kimix::variant<Ts...>`, `kimix::function<Sig>`, `kimix::move_only_function<Sig>`.

**Format**: Use `kimix::format(fmt, args...)` (C++20 `std::format`). `FMT_STRING(x)` is a no-op passthrough.

**String views**: `kimix::string_view` (= `std::string_view`), `kimix::u8string_view`, `kimix::u16string_view`, etc.

**Sstreams**: `kimix::ostringstream`, `kimix::istringstream`, `kimix::stringstream`.

**Other aliases**: `kimix::bitvector` (= `std::vector<bool>`), `kimix::fixed_vector<T,N>` (= `kimix::vector<T>`), `kimix::unordered_dense<K,V>` (= `kimix::unordered_map<K,V>`), `kimix::bit_cast<To,From>` (= `std::bit_cast`), `kimix::pointer_hash<T>` (hash pointer addresses).

**Allocator helpers** (`stl/memory.h`):
```cpp
T* p = kimix::new_with_allocator<T>(args...);   // allocate + construct
kimix::delete_with_allocator(p);                 // destruct + deallocate
T* p = kimix::allocate_with_allocator<T>(n);    // raw allocation
kimix::deallocate_with_allocator(p);            // raw deallocation
```

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
kimix::fixed_map<int, std::string, 8> fm;
fm[42] = "answer";
auto it = fm.find(42);
```

**`kimix::vector_map<Key, Value>`** — flat map stored in a sorted `kimix::vector` of pairs. Insertion uses binary search + insert, O(N) insertion, O(log N) lookup.
```cpp
kimix::vector_map<int, std::string> vm;
vm[42] = "answer";
```

**`kimix::lru_cache<Key, Value>`** — LRU cache backed by `kimix::list` + `kimix::unordered_map`.
```cpp
kimix::lru_cache<int, std::string> cache(100);
cache.put(1, "one");
auto v = cache.get(1);  // std::optional<std::string>
```

**Filesystem**: `kimix::filesystem` is a namespace alias for `std::filesystem`. `kimix::to_string(const filesystem::path&)` converts a path to `kimix::string`.

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

**Free-standing helpers** (in `string_scratch.cpp`):
```cpp
kimix::scratch_append_format(ss, "value: %d (0x%x)", v, v);  // printf-style
kimix::scratch_append_hex(ss, 0xDEADBEEFull);
kimix::scratch_append_ptr(ss, pointer);
```

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

auto sum = v + u;      // component-wise
auto diff = v - 0.5f;  // scalar broadcast
auto prod = 2.0f * v;
v += u;                 // compound assignment

auto cmp = v < u;        // returns kimix::bool3
bool any_true = kimix::any(cmp);
bool all_true = kimix::all(cmp);
bool none_true = kimix::none(cmp);
```

**Type aliases**: `kimix::bool2/3/4`, `kimix::float2/3/4`, `kimix::int2/3/4`, `kimix::uint2/3/4`, `kimix::short2/3/4`, `kimix::ushort2/3/4`, `kimix::slong2/3/4`, `kimix::ulong2/3/4`, `kimix::half2/3/4`.

All arithmetic (`+`, `-`, `*`, `/`, `%`, `&`, `|`, `^`), compound (`+=`, `-=`, etc.), comparison (`<`, `>`, `<=`, `>=`, `==`, `!=`), and logical (`&&`, `||`) operators are defined with scalar broadcast. Hash specialization via `kimix::hash<kimix::Vector<T,N>>`.

### Matrix<T, N>
```cpp
kimix::Matrix<float, 4> m;              // zero matrix
kimix::Matrix<float, 4> identity(1.0f); // identity * 1.0
m[2][1] = 3.0f;  // col 2, row 1

auto m2 = m * identity;               // matrix multiply
auto v2 = m * kimix::float4{1,0,0,0};   // matrix * vector
auto sum = m + identity;
```

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
// float versions: kimix::pi_f, kimix::inv_pi_f, etc.
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
std::string hex = h128.to_string();              // 32-char hex string
```

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
double now = kimix::Clock::now_ms();  // ms since epoch (static)
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
CRTP mixin providing `with_lock(f)` pattern:
```cpp
class MyClass : public kimix::thread_safety<std::mutex> {
    void do_work() {
        with_lock([&] { /* critical section */ });
    }
};
```

## 10. Memory Management

### `kimix::Pool<T, ThreadSafe=true>`
Object pool with 64-element blocks. Thread-safe by default.
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

Move-only, movable. Also see `kimix::dump_free_list(allocator)` for a `kimix::string`-based dump.

### Aligned allocation (`platform.h`)
```cpp
void* p = kimix::aligned_alloc(alignment, size);
kimix::aligned_free(p);
size_t ps = kimix::pagesize();
```

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

### `kimix::BinaryFileWriteStream` (write)
```cpp
kimix::BinaryFileWriteStream out("output.bin");
out.write(data_ptr, size);
out.write(span_of_bytes);
```

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
if (mod.load("myplugin")) {           // name -> "myplugin.dll" / "libmyplugin.so"
    auto fn = mod.function<void(int)>("my_function");
    if (fn) fn(42);
    mod.unload();
}

// Static search paths:
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

Platform macros: `KIMIX_PLATFORM_WINDOWS`, `KIMIX_PLATFORM_APPLE`, `KIMIX_PLATFORM_UNIX`.

## 14. DLL Export

```cpp
#define KIMIX_CORE_API   // __declspec(dllexport/dllimport) or visibility("default")
```
Define `KIMIX_CORE_EXPORT_DLL` when building core as a shared library. Client code leaves it un

## Naming

- **Classes / structs / enums**: `CamelCase` (`MyClass`, `RenderPipeline`)
- **Functions & public vars**: `snake_case` (`get_value`, `process_data`)
- **Private/protected members & functions**: `_snake_case` (`_private_var`, `_internal_helper()`)
- **Constants**: `kCamelCase` or `UPPER_SNAKE_CASE` for macros
- **Template params**: `CamelCase`
- **Namespaces**: `luisa`, `luisa::compute`, `vstd`, etc. Keep compact.

## Syntax Check

Use the project C++ syntax checker:

```bash
python scripts/check_cpp_syntax.py <file>.cpp
```

It runs `clangd` with the project's `compile_commands.json` and `.clang-tidy`. Skip files not in `compile_commands.json`.

## Formatting

Format with the project `.clang-format` (bundled in this skill; the project root copy is authoritative).

Base: **LLVM style**. Key overrides:

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
- **Macros**: control-flow-like (`$if`, `$elif`, `$else`, `$for`, `$while`, `$loop`, `$switch`, `$case`, `$default`) get space before `(`. Function-like macros don't. Special lists:
  - `ForEachMacros`: `LUISA_STRUCT`, `LUISA_BINDING_GROUP`, `LUISA_BINDING_GROUP_TEMPLATE`
  - `IfMacros`: `$if`, `$elif`, `$else`, `$for`, `$while`, `$loop`, `$switch`, `$case`, `$default`
  - `StatementMacros`: `LUISA_MAP`

## Static Analysis

Run `.clang-tidy` (bundled in this skill; the project root copy is authoritative).

All checks disabled (`-*`), then enabled by category:

- **bugprone-***
- **cert-***
- **cppcoreguidelines-***
- **google-*** (default-arguments, explicit-constructor, runtime-operator)
- **hicpp-***
- **misc-***
- **modernize-***
- **mpi-***, **openmp-***
- **performance-***
- **portability-***
- **readability-***

See the bundled `.clang-tidy` for the exact check list.

## No RTTI

RTTI is disabled for project code. Do **not** use:

- `dynamic_cast` — use `static_cast` when type is known
- `typeid`
- `std::type_info`

Prefer virtual dispatch or explicit type tags for type-safe downcasting. Third-party code under `src/ext` is exempt.

## Integer Types

Prefer fixed-width integer types:

- Use: `int32_t`, `uint32_t`, `int64_t`, `uint64_t`, `int16_t`, `uint16_t`, `int8_t`, `uint8_t`
- `size_t` is acceptable for sizes/indices per STL convention.
- Prefer `std::byte` for raw byte data.
- Avoid `unsigned int`, `long long`, `unsigned long`, `short`, and `char` for arithmetic. Some platform/system headers may define aliases such as `uint`; avoid introducing new uses in project code.
