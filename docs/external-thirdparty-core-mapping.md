Core mapping review — what kimix-native/src/core reuses from LuisaCompute, and what it keeps

Scope: the files of src/core that have a close counterpart in LuisaCompute
(include/luisa/core/**, include/luisa/vstl/**, LC's bundled EASTL / marl /
BTree / yyjson / spdlog) after the external third-party leg
(xmake f --kimix_external_thirdparty=true) was made to build the whole tree.

This file is the decision record for that work: what is now an alias of LC, what
stays a Kimix implementation, and why. The inventory it was derived from (the
per-facility audit of src/core against its LC counterpart) lived in the
workspace-level audit notes that have since been deleted; the mapping tables
below are the surviving, self-contained record, so nothing here depends on a
document outside this repository. The rule applied throughout:

> Map a facility onto luisa:: when the upper-level behaviour is identical.
> Keep the Kimix implementation when the two differ observably — a mapping
> table that "looks complete" is not worth a silent behaviour change, a broken
> public API or a broken ABI.

Two things this file is NOT about, stated up front:

* Deduplication of the core third-party facilities — "one mimalloc / one yyjson /
  one marl / one STL per process" — is the whole of the external leg, and the leg
  is exactly lc-core plus its four dependencies (EASTL with its bundled mimalloc,
  spdlog, btree, lc-yyjson) and lc-check-winsdk. That minimal graph is what
  scripts/native_dual_config_gate.py asserts; nothing below adds a target to it.
* Building LuisaCompute's embedding example is a SEPARATE, OPT-IN switch
  (xmake f --kimix_luisa_embedding=true), and it only means anything together
  with --kimix_external_thirdparty=true. It is not part of the core mapping or of
  the deduplication above: with it off (the default) the external leg is the
  minimal graph, no backend / example / tutorial target exists, and none of the
  embedding's inputs (src/runtime, src/dsl, src/tile, src/backends, LMDB, volk,
  glslang, SPIRV-Tools, examples/embedding, LC's SDK helper) is even probed. See
  the header of scripts/xmake_luisa.lua for the four option combinations.

The options do not change the public core contract. Leg-only implementation
lines are inside #if defined(KIMIX_EXTERNAL_THIRDPARTY); the standalone branch
keeps its original std/STL and marl implementations. A few leg-neutral fixes are
intentionally shared by both builds: exact-capacity graph payload reserves, the
hash-line dangling-view fix, and kimix:: optional/variant spellings. pybind11
also moved from kimix-core to its actual runtime/test consumers. Thus neither
the source diff nor the dependency diff is claimed to be byte-for-byte empty;
the full dual-configuration suite is the behavior gate.

## 1. Mapped onto LC (identical behaviour)

| Facility | File | LC counterpart | Why the behaviour is the same |
|---|---|---|---|
| `vector`, `string`, `map`/`set`, `unordered_map`/`unordered_set`, `deque`, `list`, `queue`, `stack`, `priority_queue`, `sstream`, `filesystem`, `optional`, `variant`, `algorithm`, `iterator`, `type_traits`, `pdqsort`, `logging`/`magic_enum` doors | `src/core/stl/*.h`, `src/core/logging.h`, `src/core/magic_enum.h` | `luisa/core/stl/*`, `luisa/core/logging.h`, `luisa/core/magic_enum.h` | phase 2A; one STL/one allocator per process. `map`/`set`/`unordered_map`/`unordered_set` needed Kimix wrappers — see §3. |
| `fiber::scheduler` | `src/core/fiber.h` | `luisa::fiber::scheduler` | same contract: binds in the constructor, unbinds in the destructor, non-copyable/non-movable, `internal_t = marl::Scheduler`, same worker-count configuration. |
| `fiber::counter` / `lock` / `mutex` / `condition_variable` | `src/core/fiber.h` | `luisa::fiber::counter` / `lock` / `mutex` / `condition_variable` | plain aliases of the same `marl` types in both trees. |
| `fiber::event` | `src/core/fiber.h` | `luisa::fiber::event` | same struct, same members: `Mode`, `signal`, `clear`, `wait`, `test`, `is_signalled`. |
| `fiber::Future<T>` | `src/core/fiber_future.h` | `luisa::fiber::Future<T>` + `is_signalled()` | same handle semantics (shared state, `signal`/`clear`/`wait`/`test`); kimix keeps only the snake_case `is_signalled()` alias for `isSignalled()`. |
| `SharedFunction<Sig>` | `src/core/shared_function.h` | `luisa::SharedFunction<Sig>` | allocation-once + atomic ref-count + copy-by-refcount, i.e. move-only captures compile in both legs. The wrapper holds LC's object and adds kimix's `explicit operator bool()` and the report-and-abort path for an EMPTY wrapper (LC only has `LUISA_ASSUME`). It *holds* rather than *derives*: LC's converting constructor excludes only `luisa::SharedFunction` itself, so a derived type re-wraps on every copy and recurses (measured: instant stack overflow). |
| `fiber::detail::NonMovableAtomic<T>` | `src/core/fiber.h` | `luisa::fiber::detail::NonMovableAtomic<T>` | same name, same copy-deleted/move-allowed contract, same load-on-move. |
| `fiber::current_execution_id()` | `src/core/fiber.h` | `luisa::fiber::get_id()` | identical identity rule (marl fiber pointer tagged with the top bit, else the masked `std::hash<std::thread::id>`). |

Kimix-only fibre surface kept on top of that marl (LC has no counterpart, or
LC's version changes behaviour): `is_bound()`, the process-lifetime
`shared_scheduler()`, `schedule_background()` (submits to the shared pool from
an unbound thread via `Scheduler::enqueue`), the report-and-abort submission
check, `worker_thread_count()` returning 1 when unbound (LC dereferences the
scheduler), `sleep_for()`, `blocking_call()`, and the whole
`parallel()`/`async_parallel()` family with chunked claims, `task_limit`, the
inline `inplace_batch_threshold` and the unbound-inline fallback (LC's
`worker_thread_count()`-based split never degrades and ignores `task_limit`).

## 2. Kimix-only — nothing to map onto

`memory.h`/`memory.cpp` (gateway + `KIMIX_MEMORY_STRICT` tracker + the exported
`kimix_allocate`/`kimix_deallocate`), `json_repair.{h,cpp}`,
`rbc_concurrent_queue.h` + `detail/concurrent_queue.h`, `shared_atomic_mutex.{h,cpp}`,
`win_message_box_suppression.h`, `kimix_core.{h,cpp}`, `dll_export.h` (the
`KIMIX_*_API` tri-states are this project's own link contract),
`hash_map.h` (the vstl `HashMap` port under kimix names), `compare.h`,
`pch.h`.

## 3. Kept as Kimix implementations, deliberately

Each row is a case where a mapping exists on paper but would change behaviour,
the public API or the ABI, or would need LC code that does not exist.

| File | LC counterpart | Why it stays Kimix |
|---|---|---|
| `basic_traits.h` (`kimix::half`) | `luisa::half` = `half_float::half` | kimix's `half` is a `float` placeholder: different size, alignment, arithmetic and bit pattern. Not shimmable. |
| `basic_types.h` (`Vector`/`Matrix`) | `luisa/core/basic_types.h` | Storage and defaults differ: kimix has one `VectorStorage` holding `T _[N]` with `x()/y()/z()/w()` *functions* and no swizzles; LC has per-N named members + generated swizzles. `Matrix` default-constructs to zeros here and to the identity there, and `eye()`/`fill()` exist only in LC. Both are the numeric foundation of the whole tree — a mapping is a silent numeric change. |
| `stl/hash.h`, `Hash128` | `luisa/core/stl/hash.h` | `hash_combine` differs (chained per-element XXH3 here, one XXH3 over the block with a seed there) and `Hash128(string_view)` memcpys here but parses hex there — any persisted digest or cache key would change. `kimix::hash` also stays the default Hash template argument of the containers, so a container built here and one built there hash identically. |
| `dynamic_module.h` | `luisa/core/dynamic_module.h` | Different API (`bool load(...)/unload()/function()` vs the move-only `load()/load_exact()/address()/invoke()/apply()`), and kimix's search-path bookkeeping is not what LC's `load()` uses. The public kimix API is used by `kimix-llm`/`kimix-script` payload loading. |
| `binary_io.h`, `binary_file_stream.h` | `luisa/core/binary_io.h`, `binary_file_stream.h` | Different interfaces (4 pure virtuals with `bool` + out-parameters here, 8 returning `unique_ptr<BinaryStream>`/`filesystem::path` there; no `BinaryBlob`/`BinaryStream` base here). The whole `src/api` FFI reads through this API. |
| `memory.h` (top-level gateway) | `luisa/core/stl/memory.h` + `src/core/stl/memory.cpp` | Keep-alive of the kimix allocation gateway, the `KIMIX_MEMORY_STRICT` tracker, `IOperatorNewBase` and the advanced helpers (`allocate_aligned_at`, `memory_mark_permanent`, tracker scopes). `kimix::allocator<T>` keeps its never-throwing, always-equal semantics; owning `unique_ptr`/`shared_ptr` stay `std` in both legs because kimix allocates some pointees with plain `new` (EASTL's deleter frees through mimalloc — measured abort in `mi_free`). |
| `platform.h`/`.cpp` | `luisa/core/platform.h`/`.cpp` | Near 1:1 but every entry point is a kimix export with kimix's own symbol names and behaviour for `dynamic_module_path`, guid/serial generation and the message-box suppression. Mapping renames the exported symbol set. |
| `clock.h` | `luisa/core/clock.h` | Different API (`reset`/`toc`/`toc_seconds`/`toc_reset`/`now_ms` on `steady_clock` vs `tic()`/`toc()` on `high_resolution_clock`). Call sites depend on the ms/seconds forms. |
| `spin_mutex.h` | `luisa/core/spin_mutex.h` | Same algorithm, different pause/yield back-off policy — a lock, so a policy change is a performance/contention change, not a rename. |
| `thread_safety.h` | `luisa/core/thread_safety.h` | kimix defines its own `conditional_mutex_t` class and alias family; LC's is an alias layer with `thread_safety<void>`. Mapping changes the lock trait types used by `pool.h`. |
| `pool.h`, `first_fit.h` | `luisa/core/pool.h`, `first_fit.h` | Forked (kimix's pool is thread-safety-templated and its `first_fit` uses kimix's node layout); LC's `FirstFit` is tied to its own allocator/`ManagedPtr` plumbing. |
| `string_scratch.h` | `luisa/core/string_scratch.h` | kimix's is header-inline over `kimix::string` with its own `operator<<` surface; LC's is an exported out-of-line class. |
| `concepts.h` | `luisa/core/concepts.h` | Disjoint naming (`kimix::arithmetic`/`integral`/… flat, mirroring `std`, vs `luisa::concepts::*`). Nothing imports kimix's concepts across the boundary, so the mapping would only rename call sites. |
| `constants.h` | `luisa/core/constants.h` | kimix's constants are `double` (plus `_f` variants), LC's are `float` — a mapping silently narrows every constant. |
| `mathematics.h` | `luisa/core/mathematics.h` | kimix has the scalar subset; LC's adds the vector overloads over its own `Vector` types (which kimix does not use, see above). |
| `intrin.h` | `luisa/core/intrin.h` | `KIMIX_*` is a 1:1 rename of `LUISA_*` and is header-only macro text — there is no second implementation to eliminate, and the macros are used by name throughout `src/core`. |
| `guid.h`, `md5.h`, `lmdb.h` + `lmdb/*.c` | `luisa/vstl/{v_guid,md5,lmdb}` | Faithful ports already (vstd → kimix, snake_case). Mapping would rename `kimix::Guid`/`kimix::md5`/the lmdb wrappers, i.e. change public types for no behavioural gain. |
| `stl/fixed_map.h`, `stl/vector_map.h`, `stl/lru_cache.h`, `stl/ring_buffer.h` | `luisa/core/stl/*` | Phase 2A kept these as Kimix compatibility classes (LC's are `eastl::fixed_*` aliases / different `lru_cache` and `ring_buffer` APIs); their storage is leg-aware where it matters. |
| `stl/functional.h`, `stl/format.h` | `luisa/core/stl/functional.h`, `format.h` | `kimix::function`/`move_only_function` and `std::format` are kept (one formatting backend; `std::format_string` parameter types are what the call sites use). |
| `fiber.h` `parallel()`/`async_parallel()`/`sleep_for`/`blocking_call`/`schedule*` | `luisa/core/fiber.h` | See §1's last paragraph: `task_limit`, chunked claims, the inline threshold and the unbound-inline fallback are Kimix behaviour LC does not have. |

## 4. Consequences for the two legs

* One marl backend: LC's marl is compiled into `luisa-core.dll` (`MARL_DLL`,
  EASTL-backed) and `kimix-marl` is not declared in the external leg — no
  `MARL_USE_SYSTEM_STL` anywhere in that leg, so kimix's inline marl-using code
  and the scheduler in the DLL agree on every type.

 Mimalloc and core-state ownership differ by leg. Standalone static kimix-core
  is linked into each final executable or shared image. The external leg keeps
  BOTH kimix-core and luisa-core shared: cross-image C++ objects use one heap,
  and kimix-core's scheduler, strict allocation tracker and other mutable static
  state have exactly one process-wide instance.

  | leg | who compiles mimalloc | ownership / linkage |
  |---|---|---|
  | standalone (option off) | the vendored src/ext/mimalloc object target | one private allocator per final image; no MI_SHARED_LIB or MI_SHARED_LIB_EXPORT, so import libraries cannot collide when images link each other |
  | external (option on) | EASTL's bundled mimalloc, declared by scripts/xmake_luisa.lua | the objects stay in shared luisa-core; shared kimix-core and its consumers import that allocator |

  The supported allocation door remains kimix::allocate / kimix::deallocate;
  project code never calls mi_ outside src/ext and src/api/ffi_mem.cpp. Owning
  C++ objects must not be transferred between standalone module heaps; C-FFI
  buffers are released through the same kimix_api image that allocated them.


* `kimix::unordered_map`/`unordered_set` are wrapper classes in the external leg,
  not bare aliases: LC's ankerl instantiation (EASTL value container) has an
  unusable `swap()` (ADL finds both `eastl::swap` and `std::swap` → C2668) and an
  unusable `insert(std::initializer_list)` / `insert(first, last)` (the
  hint+value overload is an exact match for raw-pointer iterators because
  `luisa::is_constructible_v` is EASTL's, which says true for a scalar
  `value_type`). The wrappers keep the LC table as storage and restore the
  std-shaped `swap`/`insert`/constructor sets.

* `kimix::map`/`kimix::set` are wrapper classes too: frozenca's B+tree is
  MOVE-ONLY (copy ctor = delete), while kimix code copies sets and maps.
  Copyability is restored with a deep copy over the LC container. Both wrappers
  re-expose the std-shaped member types (`key_type`, `mapped_type`, `value_type`,
  `size_type`, `iterator`, `const_iterator`), which is what lets a call site that
  names them compile in both legs.

* The comparator restriction of those wrappers: frozenca's B+tree does not store
  a comparator — it default-constructs `Compare{}` at every use — so a stateful
  functor would lose its state silently in the external leg while still working
  standalone. `kimix::map`/`kimix::set` therefore `static_assert` that the
  comparator is empty in that leg. The DEFAULT comparator is unchanged
  (`std::less<Key>` / `std::less<T>`), so nothing in the tree is affected today;
  a future custom order has to be an explicit key type, never a stateful
  comparator that silently does nothing.

* `kimix::erase_if(container, pred)` exists in BOTH legs (`stl/map.h` and
  `stl/unordered_map.h`) and is the one spelling kimix code uses. `std::erase_if`
  is not leg-neutral: template deduction does not look through the external
  wrappers, and in the standalone leg the std overload is only reachable through
  ADL (where ankerl also injects its own `std::erase_if` extension for the dense
  tables, with a wrapped-around return value). `kimix::erase_if` returns the
  number of erased elements like the std one, and each leg implements it with its
  own facility (`std::erase_if` / frozenca's `erase_if()` member / ankerl's
  `erase(iterator)`). Always qualify the call: unqualified `erase_if(m, pred)`
  also finds the std overload by ADL in the standalone leg and would be
  ambiguous there.
