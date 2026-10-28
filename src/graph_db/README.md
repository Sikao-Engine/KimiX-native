# kimix-graph-db (`src/graph_db`)

The graph database layer of KimixBase: an in-memory vertex/edge container with a
five-level lock hierarchy, a GDI-shaped backend facade on top of it (typed
properties, labels, `(label, external_id)` keys, first-class edges, a schema,
constraints, filtered/paged reads, transactions and a C ABI), plus a parallel
incremental serializer to LMDB cold storage and a dual-stage,
cycle/corruption-tolerant loader.

Design authority: `graph_db_plan.md` (§3.1 data model, §3.2 concurrency, §3.3
serialization format & disk layout, §3.4 error handling, §6 tests). The product
requirement it implements is `graph_db.md`.

---

## 1. Scope

- **In scope**: the in-memory graph (`Graph`, `Vertex`, `Edge`, `KeyIndex`), the
  on-disk record format and key spaces, the LMDB cold store, incremental/full
  save, dual-stage load, checkpoint copies, and the GDI layer: `PropertyValue` /
  `PropertyBag`, labels, `(label, external_id)` keys with a uniqueness index, a
  schema (labels / property types / constraints), oriented + constraint-filtered +
  paged reads, transactions with an undo journal, and the C ABI
  (`graph_c_api.h`). See `reports/gdi_alignment.md` for the command-by-command
  alignment with `Semevia.GDI`.
- **Out of scope (deferred, P9 in the plan)**: global vertex scans, native
  multi-hop traversal, GDI-shaped shortest-path/BFS composites, global
  aggregates and a named multi-graph catalog. The capability advertisement keeps
  those flags honestly `false` (see `capability.h`).
- **Dependency rule**: `kimix-core` only. Third-party code (LMDB, mimalloc) is
  reached through it, never by an `add_deps()` on a `src/ext` target.

## 2. File map

| File | Contents |
|---|---|
| `graph_db_api.h` | `KIMIX_GRAPH_DB_API` linkage-macro family (three states, static today). |
| `graph_db.h` | Umbrella include (`graph.h`, `utils.h`, `vertex.h`) + `KIMIX_GRAPH_DB_VERSION` (generated from `version.txt`). |
| `utils.h` / `utils.cpp` | Key scheme (`"v/"` + 16 binary guid bytes, `meta/root` reserved), the byte-exact `VertexRecord` codec (`encode_vertex_record` / `decode_vertex_record`, all little-endian), `guid_key` / `guid_from_key` / `md5_of`, filesystem helpers, and `ColdStorage` — a thin RAII front end over `kimix::LMDB` with batched transactional writes, a full scan and `copy_to()`. |
| `vertex.h` / `vertex.cpp` | One vertex: guid, opaque content blob + cached MD5, dependency links in **both** forms (guid vectors = the serialized source of truth; `Vertex*` sets = the resolved in-memory form), an `alignas(64)` `kimix::shared_atomic_mutex` and the atomic dirty flag. Also the `VertexUniqueLock` / `VertexSharedLock` RAII adapters. |
| graph.h / graph.cpp | The vertex container: a 64-way SHARDED vertex map (kGraphShardCount, one std::shared_mutex + one bucketed kimix::HashMap per shard), structural CRUD with a cascading remove_vertex (one shard lock at a time), connect / disconnect (endpoint shards, ascending order), connect_bulk (epoch 3: one endpoint resolution, one ascending lock set and one writer check for a whole edge batch), reserve_links, a version-keyed cached roots(), neighbors / dependencies / dependents plus the non-allocating AdjacencyBuffer / caller-span forms, cycle-safe BFS has_path / shortest_path (one shard lock per visit), the EDGE-filtered forms of both (oriented_links / edge_matches, plus a bounded shortest_path that keeps only hops whose EDGE satisfies a constraint and reports each hop's edge as a PathStep), dirty bookkeeping (mark_dirty / clear_dirty / dirty_vertices), the single-writer contract check. graph.h carries the full concurrency contract, the deadlock-freedom argument and the epoch-1/2/3 results. |
| `serialize.h` / `serialize.cpp` | `GraphSerializer` (full + incremental save, `copy_to` checkpoint) and `GraphDeserializer` (dual-stage load), plus `GraphSerializeStats`. Details in §4 and §5. |
| cold_store.h | INTERNAL storage-backend seam (not in the umbrella, RTTI-free, non-virtual): `ColdStore`, a non-owning handle over exactly one of `ColdStorage` (LMDB) or `FileColdStore` (flat file), dispatching `valid` / `path` / `read_meta` / `save_batch` / `load_all_records` / `copy_to` through a `kimix::variant` of pointers. That is what lets the backend be selected without touching the serializer or the deserializer; `open()`/`close()` stay an owner concern (`Database`). |
| file_cold_store.h / file_cold_store.cpp | The opt-in flat-file backend: ONE append-only `<dir>\graph.xdb` (magic "KXDB0001" + 20-byte header + frames of length-prefixed records), one sync per save batch, one open + positioned sequential reads per scan, duplicate keys collapsed last-wins, tombstones as empty values, and a full save is a truncating `rebuild()`. The transport is plain positioned CRT I/O (64-bit offsets, `_fseeki64`/`fseeko`, `_commit`/`fsync`), one open per pass, no file-size wall. |
| `edge.h` / `edge.cpp` | First-class edges: `EdgeId`, `Edge`, `EdgeLink`, `EdgeRecord`, `EdgeDirectionType` / `EdgeOrientation`, the sharded `EdgeTable` and the `"GDE2"` edge-record codec. |
| `key.h` / `key.cpp` | `ExternalId` (16-byte SSO, arbitrary binary), `VertexKey` / `VertexKeyView` and the sharded `KeyIndex` (level 4). |
| `status.h` / `status.cpp` | `GdiStatus`: the 63 GDI status codes with their pinned numeric values, names and messages. |
| `property.h` / `property.cpp` | `PropertyKind` / `PropertyValue` / `PropertyBag`, the GDI type vocabulary and `evaluate()`, plus the record-level property codec. |
| `schema.h` / `schema.cpp` | Label / property-type / constraint registries, rename, free, the `"s/"` table codecs and their restore path. |
| `query.h` / `query.cpp` | `Page<T>` (the two-phase count/fetch shape) and the AND-ed constraint evaluation. |
| `transaction.h` / `transaction.cpp` | The undo journal (`JournalEntry`) and the transaction state machine. |
| `database.h` / `database.cpp` | `Database`: the GDI backend facade (one per session) — every command, its schema validation, its exact status code, the rollback replay and the save hooks. |
| `capability.h` / `capability.cpp` | `Capability` (the honest advertisement) and `capability_name_diff`. |
| `graph_db_options.h` | `GraphOptions` / `KeyMode`: key uniqueness policy, edge policy, journal cap, store map size, **`store_backend`** (`ColdStoreBackend::lmdb` default / `ColdStoreBackend::flat_file` opt-in — C++ only, no C-ABI field), `persist_on_commit`, graph name. |
| `graph_c_api.h` / `graph_c_api.cpp` | The C ABI (ASCII, C99-clean, no library allocation on read paths, two-phase buffers). |
| `binary_le.h` | INTERNAL little-endian codec helpers shared by the record encoders (not part of the umbrella). |
| `reports/gdi_alignment.md` | The `graph_report.md` §2.1–§2.16 checklist: what is closed, by which file, with which test. |
| `xmake.lua` | The `kimix-graph-db` static target and its public include dirs / defines. |
| `README.md` | This file (module overview + the benchmark table S3 fills in). |
| `tests/unit/graph_db/graph_db_test_util.h` | Shared fixtures: byte helpers, `temp_dir_guard`, the known 6-vertex DAG, `build_layered_dag`, record/store hand-crafting helpers, raw-store snapshots, guid/adjacency comparison helpers. |
| `tests/unit/graph_db/test_graph_db.cpp` | Container suite: CRUD, cascade remove, edge directions, roots, threaded content races, writer contract, traversals, record codec, cold-storage round trip, cache-line/alignment checks. |
| `tests/unit/graph_db/test_graph_db_serialize.cpp` | Serializer suite: round trip, incremental writes, cycle/dangling stores, determinism, corruption, merge policy, checkpoint. |
| `tests/unit/graph_db/test_graph_db_file_store.cpp` | Flat-file backend + seam suite: the `graph.xdb` layout, frame/torn-tail atomicity, last-wins duplicates, tombstones, `rebuild()` vs append, `copy_to()`, the windowed scan of a store past 4 MiB, determinism, and — the parity gate — the same persistence script replayed through `Database` on both backends (`GraphOptions::store_backend`) with byte-identical results. |
| `tests/unit/graph_db/test_graph_db_bench.cpp` | Read / write / serialization benchmarks (subtask S3). The serialization section runs **both cold-store backends** (lmdb, flat_file) at 100k and 1M, named `graphdb/serialize/<scale>/<backend>/<op>`; §6.1 holds the resulting table. |

## 3. Concurrency contract (summary — `graph.h` is authoritative)

Two independent lock levels, always acquired in the order **shard → vertex** and
never the other way round:

1. **Graph level** — `kGraphShardCount` (64) **shard locks**, each a
   `std::shared_mutex` on its own cache line guarding one bucketed
   `kimix::HashMap` of the vertex map (`shard = hash(guid) & 63`). Epoch 1
   replaced the single graph-wide mutex with them; §6 has the measured result.
   - **UNIQUE** for anything that changes the vertex set or the edge set:
     `create_vertex`, `add_vertex`, `remove_vertex`, `connect`, `disconnect` —
     one shard for the single-vertex writes, the endpoint shards in ascending
     order for `connect`/`disconnect`, and ONE shard at a time for
     `remove_vertex`'s cascade.
   - **SHARED** for pure reads and traversals: `find`, `contains`, `roots`,
     `neighbors`, `dependencies`, `dependents`, `has_path`, `shortest_path`,
     `dirty_vertices`, `mark_dirty`, `clear_dirty`; and for the guid→`Vertex*`
     resolution in `update_content` (released before the vertex lock is taken).
     `size()` takes no lock at all (an atomic count the writers maintain).
   - A traversal holds ONE shard lock per vertex visit: acquire, copy the
     adjacency into a reused `AdjacencyBuffer`, release.
2. **Vertex level** — one `kimix::shared_atomic_mutex` per vertex, `alignas(64)`
   so neighbouring vertices never share its cache line (no false sharing).
   - **SHARED** to read `content` / `content_hash`.
   - **UNIQUE** to write them (`set_content`, `touch`).

Write policy: **structural writes are single-threaded by contract.** The graph
records the id of the thread that performed the first structural write; a later
structural write from a different thread is refused (`false` / `nullptr`) and, in
debug builds only, also reported on stderr and asserted. Reads are unrestricted.
`update_content` is a level-2 operation and is explicitly exempt: N threads may
update N distinct vertices concurrently.

Deadlock freedom: shard locks are taken in ascending shard index whenever more
than one is needed, shard before vertex, and never the inverse. No path acquires a
shard lock while holding a vertex lock, so a wait-for cycle is impossible
(`graph.h` carries the full argument).

Read atomicity NARROWED in epoch 1: a single call is still self-consistent for its
subject vertex, but a multi-vertex traversal may now observe a structural mutation
on one visit and not the next — the epoch-0 whole-graph snapshot is what parallel
shard reads give up. Memory safety is unaffected (`remove_vertex` keeps the victim
alive for its whole cascade and returns it to the caller), and a caller that needs
a consistent whole-graph picture must keep the graph structurally quiescent,
exactly as the serializer's caller contract already required.

Serializer additions to the contract:

- Save may run concurrently with `update_content` (level-2 only). The record's
  content hash is recomputed from the bytes actually copied, so a racing update
  cannot produce a record whose stored MD5 disagrees with its own content, and a
  vertex whose hash changed mid-save is left dirty.
- Save must **not** overlap a structural write, and load performs structural
  writes, so both run on the graph's writer thread.
- `connect` / `disconnect` change both endpoints' serialized dependency lists but
  do not set their dirty flags; the caller marks the affected vertices (or calls
  `Graph::mark_dirty()` / `GraphSerializer::save()`, which marks everything).
- Removing a vertex does not delete its record; use
  `ColdStorage::remove_record()` or a checkpoint to drop it.

## 4. Save pipeline (`GraphSerializer`)

| Stage | Thread | Work |
|---|---|---|
| 1 collect | caller | `graph.dirty_vertices()` — graph SHARED lock for the map, each vertex's SHARED lock for the cached hash. This vector **is** the write set. |
| A encode | fiber fan-out | Per dirty vertex: `content_copy()` under the vertex SHARED lock, copy both guid lists, `encode_vertex_record` straight into its own `LMDBWriteCommand` slot. Jobs write disjoint slots, so no lock. |
| B write | **calling thread** | One `ColdStore::save_batch()` = ONE atomic unit of the backend (an LMDB transaction, or one appended frame) ⇒ the batch is atomic. Kept out of the fan-out: it is a blocking syscall batch and would park a pool worker. Which store it is, is the backend choice — see below. |
| settle | caller | Clear the dirty flag of each written vertex, but only when its cached hash still equals the written hash (a racing update stays dirty). |

- `save_incremental(graph)` — the primary path: only the dirty vertices are
  encoded and written. A clean vertex is never re-encoded and never rewritten, so
  its stored bytes stay bit-identical.
- `save(graph)` — full save: marks every vertex dirty first, then runs the same
  pipeline, so the whole graph is rewritten in one transaction.
- `copy_to(path)` — passthrough to `ColdStore::copy_to()` (compacted snapshot; a
  directory either way).

Which store stage B writes to is a BACKEND choice, and it is the only stage the
choice changes (the pipeline, the fan-out and the counters are shared):

| `GraphOptions::store_backend` | stage B | disk unit |
|---|---|---|
| `ColdStoreBackend::lmdb` (default) | one `save_batch()` = one LMDB write transaction | the memory-mapped `data.mdb`; on Windows LMDB opens it `FILE_FLAG_WRITE_THROUGH`, so the commit is a chain of synchronous page flushes |
| `ColdStoreBackend::flat_file` | one `save_batch()` = ONE appended frame of `<dir>\graph.xdb`, one open + one positioned write + **one `sync()`** + close | the append log; a FULL save replaces the file instead (`FileColdStore::rebuild()`, armed by `Database::persist(full=true)` because the seam's `save_batch()` cannot tell a full batch from an incremental one) |

`Database::persist()` is the only caller that arms the rebuild, and it clears the
arm right after the save, so an incremental save can never be mistaken for a full
one. The measured consequence of the two shapes is §6.1.

## 5. Load pipeline (`GraphDeserializer`) — the dual-stage guarantee

| Stage | Thread | Work |
|---|---|---|
| 1 collect | sequential scan | `ColdStore::load_all_records()` — ONE pass over the store: on lmdb one LMDB cursor over the memory-mapped map (the iterator is single-cursor), on flat_file one open + positioned sequential reads over the frames. Every key is copied out with its value — see the backend table after the one below. |
| 1' decode | fiber fan-out | `decode_vertex_record` per slot into (guid, hash, dependency guids, content, validity). No shared state. |
| 2 materialize | fiber fan-out | Bare `Vertex` per valid slot: guid + content, **no dependency list, no pointer, no edge**. |
| insert | caller | `Graph::add_vertex` per materialized vertex (a structural write, so single-threaded) + a staging guid→`Vertex*` index. |
| link | caller | Only after every vertex is in the graph AND after the reserve sweep: the whole recorded edge list goes to ONE `Graph::connect_bulk` call (epoch 3), which resolves each distinct endpoint once, takes each touched shard's unique lock once in ascending order, checks the single writer once, and fills both directions on both endpoints for every edge. |
| settle | caller | Loaded vertices are marked **clean** (they match the store), so an immediate incremental save writes nothing. |

**Why a cycle or a missing guid can never fail a load**: a vertex's existence
depends on nothing but its own record — no pointer is resolved and no edge is
created while vertices are being inserted, so a dependency problem cannot reject
a record, and a cycle is simply two resolvable edges once everything exists.
Unresolvable dependencies (missing target, self edge, refused edge) are skipped,
counted in `skipped_edge_count` and warned about on stderr. A corrupt record (bad
magic, truncated, impossible dependency counts, or an embedded guid that
disagrees with its key) is skipped, counted in `skipped_count` and warned about;
the rest of the store loads. Load is not a merge: a record whose guid already
exists in the target graph is skipped rather than overwriting the live vertex,
while an edge may still resolve to a vertex the graph already held.

Determinism: the fan-outs fill pre-sized slots by index, and the link pass walks
the slots in store key order, so loading the same store twice yields identical
guid sets and identical adjacency regardless of scheduling.

Stage 1 is the one stage the backend changes, and the two are the same contract
with two mechanics:

| backend | stage 1 |
|---|---|
| lmdb | `load_all_records()` = one LMDB cursor over the memory-mapped `data.mdb`; every key and value of the map comes out exactly once |
| flat_file | `load_all_records()` = ONE open, then a positioned sequential pass over the complete frames (a torn tail is ignored), collapsing duplicate keys **last-wins** while it scans — that is what gives the append log LMDB's map semantics |

A flat-file load therefore reads more bytes than the counters report: each record
carries its 18-byte key (`"v/"`/`"e/"` + 16) and a 12-byte length prefix next to
the value, whereas `bytes_read` counts values only (§6.1 has the measured file
sizes). The append log's duplicates never reach the loader — one file always scans
to the same record sequence, so the determinism argument above holds for it too.

6. Benchmarks

test_graph_db_bench.cpp (subtask S3) reports read, write and serialization
numbers to stderr via tests/unit/native/bench_util.h; there are no timing
asserts. Run it with `xmake run test_graph_db_bench` (release) — the whole suite
takes ~1 minute, and the 1M-vertex serialization pass is reduced to 200k in
debug builds so a debug run stays a quick smoke check.

Workload (all sizes are the constants at the top of the bench file):

- READ graph — 100k vertices, out-degree 4..8 acyclic by construction, 64-byte
  content. One "user op" = find + content read + dependencies + neighbors + a
  depth-3/node-64-bounded expansion + has_path + shortest_path on a pre-sampled
  1-2-hop pair. Reported single-threaded and with cores/2, cores, 2*cores plain
  std::threads (20 hardware threads here ⇒ 10/20/40), 1 s per configuration.
  Per-call baselines (find, content read, dependencies, neighbors, bounded reach,
  has_path, shortest_path) are printed too: they are the epoch-1 profile start.
- WRITE — (a) 200k create_vertex+connect on a fresh graph from ONE thread (the
  single-writer contract); (b) update_content from W threads on DISJOINT
  partitions of a prebuilt 40k pool, next to a `Vertex::set_content` probe that
  takes the vertex lock only (isolates the graph-lock tax from the level-2 path);
  (c) mixed: 1 structural writer + cores content updaters, plus the same
  structural loop alone as the starvation baseline.
- SERIALIZE — fresh 100k graph and a fresh 1M graph, EACH PASS REPEATED ON BOTH
  COLD-STORE BACKENDS (lmdb and flat_file, same seed, same constants): full save,
  dual-stage load and a 1 %-dirty incremental save (saved_count == N/100 is
  asserted), warmup pass then measured pass, timed by steady_clock and
  cross-checked against the serializer's own save_ms / load_ms.
  bytes_written / bytes_read are reported, and the file backend also reports the
  size of its `graph.xdb`. §6.1 holds the resulting backend table.

Table columns are fixed per epoch row so rows stay comparable:

- machine — CPU model / hardware threads / RAM of the host that ran the epoch.
- mode — xmake build mode (always release for a recorded epoch) + commit.
- epoch — 0 is the baseline; each later epoch is one optimization pass
  (graph_db_plan.md §7). Stop when a full epoch yields < 5 %.
- read 1T / read 10T/20T/40T — user-ops per second, aggregate over the threads.
- content w/s 1T, W=10/20/40 — update_content per second, aggregate; the
  `vertex-lock only` number in parentheses is the same workers calling
  Vertex::set_content, i.e. what the level-2 path would do without the graph lock.
- structural w/s — create+connect pairs per second, single thread.
- save/load/incremental — milliseconds (seconds for 1M).

| machine | mode | epoch | date | read 1T (ops/s) | read 10T/20T/40T (ops/s) | content w/s 1T | content w/s 10T/20T/40T | content w/s, vertex-lock only 10T/20T/40T | structural w/s 1T | mixed: structural vs content (w/s) | save 100k | load 100k | incr 1 % @100k (1000 rec) | save 1M | load 1M | incr 1 % @1M (10k rec) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Intel Core Ultra 7 265K, 20 hw threads, 64 GB | release | 0 | 2026-10-21 | 256 327 | 473 933 / 416 047 / 414 033 | 4 642 309 | 7 306 194 / 6 141 672 / 6 307 799 | 45 525 895 / 86 915 505 / 79 383 416 | 1 064 918 | 320 struct (1 365 960 alone, run-to-run 107-1024: the unique lock is unfair) vs 5 819 021 content | 1121 ms (30.0 MB) | 285 ms (30.0 MB, 105 MB/s) | 114 ms (744 KB) | 12 075 ms (300.1 MB) | 4 837 ms (300.1 MB, 62 MB/s) | 949 ms (7.4 MB) |
| Intel Core Ultra 7 265K, 20 hw threads, 64 GB | release | 1 | 2026-10-21 | 276 240 | 2 364 399 / 4 091 673 / 4 041 547 | 4 496 762 | 34 989 715 / 66 475 564 / 60 075 280 | 45 496 126 / 88 818 892 / 79 604 865 | 1 119 361 | 945 946 struct (1 363 556 alone) vs 56 952 424 content | 1145 ms (30.0 MB) | 308 ms (30.0 MB, 102 MB/s) | 100 ms (744 KB) | 12 403 ms (300.1 MB) | 4 911 ms (300.1 MB, 61 MB/s) | 861 ms (7.4 MB) |
| Intel Core Ultra 7 265K, 20 hw threads, 64 GB | release | 2 | 2026-10-21 | 302 046 | 2 556 243 / 4 455 914 / 4 026 305 | 4 639 496 | 35 226 859 / 67 249 889 / 56 567 417 | 45 757 777 / 88 481 725 / 83 060 833 | 1 149 725 | 920 702 struct (1 352 563 alone) vs 58 398 088 content | 774 ms (30.0 MB, 38.8 MB/s) | 232 ms (30.0 MB, 129 MB/s) | 74 ms (744 KB) | 8 124 ms (300.1 MB, 36.9 MB/s) | 3 842 ms (300.1 MB, 78 MB/s) | 737 ms (7.4 MB) |
| Intel Core Ultra 7 265K, 20 hw threads, 64 GB | release | 3 | 2026-10-21 | 280 503 | 2 562 777 / 4 467 412 / 4 105 975 | 4 562 709 | 35 195 843 / 67 095 474 / 61 565 193 | 45 722 950 / 89 113 384 / 81 686 037 | 1 123 154 | 1 057 298 struct (1 334 778 alone) vs 57 844 083 content | 786 ms (30.0 MB, 38.1 MB/s) | 168 ms (30.0 MB, 178 MB/s) | 78 ms (744 KB) | 8 576 ms (300.1 MB, 35.0 MB/s) | 3 099 ms (300.1 MB, 97 MB/s) | 776 ms (7.4 MB) |

Epoch-1 row caveat (read this before comparing the two rows): the host carried
unrelated desktop load (Windows reported 60-99 % CPU in OTHER processes) for the
whole epoch-1 session, and this CPU is hybrid (8 P-cores + 12 E-cores), so a
single run's ns/op swings 2-3x with which core a worker lands on — the untouched
`content_read_only` control moved 49.5 -> 64.4 ns between two idle-ish runs,
which is machine noise, not code. Every epoch-1 cell above is therefore the
MINIMUM ns/op observed for the COMMITTED code over the whole session (4 whole-suite
runs, including one interleaved with a control build), i.e. the uncontended
P-core number every configuration is equally entitled to. The epoch-0 row was a
single clean-machine run, which is why the two rows are close to comparable at
1T but NOT at the loaded 10/20/40T settings. The authoritative epoch-1 verdict is
the interleaved same-binary A/B below, not a row-to-row read.

Single-thread per-call breakdown from the same run (epoch-1 profile input):
find 14.9 ns, content read 49.5 ns, dependencies 67.8 ns, neighbors 369 ns,
has_path (1-2 hops) 405 ns, shortest_path (1-2 hops) 534 ns, and the
depth-3/node-64 bounded expansion 2795 ns — that expansion is ~72 % of the whole
user op. The find-only probe under threads: 67.1M ops/s at 1T vs 84.5M / 72.4M /
83.6M at 10T / 20T / 40T — only ~1.25x, with the per-call cost going from 14.9 ns
to 118-138 ns. That plateau is the read ceiling.
Epoch-0 findings (what epoch 1 should attack, in measured order):

1. READ does not scale: 256k user-ops/s on 1 thread, and 414-474k with 10/20/40
   threads — a ~1.8x ceiling, and adding threads does not reach it either (40T is
   no better than 20T). The find-only probe above isolates the cause: every
   structural read takes the one graph std::shared_mutex in SHARED mode, so the
   lock word is the bottleneck, not the container. Sharding that lock (or moving
   traversal to a per-vertex-lock + guid-snapshot scheme) is the single biggest
   win available.
2. The 1T user op is dominated by the traversal, not by lookup: find 14.9 ns,
   content read 49.5 ns, dependencies 67.8 ns, has_path 405 ns, shortest_path
   534 ns, but the depth-3/node-64 bounded expansion is 2795 ns — ~72 % of the
   whole op. Each `dependencies()` call pays a shared-lock acquire plus a full
   `kimix::vector` copy of the guid list; a ~30-vertex expansion issues ~30 of
   them. Epoch 1: an adjacency accessor that does not allocate, and a bounded
   BFS that walks its inner loop under one lock acquisition.
3. LEVEL-2 WRITES are the one thing that already scales and then some:
   update_content goes 4.64M -> 7.31M ops/s (1.57x at 10T), while
   `Vertex::set_content` on the same disjoint partitions goes 5.05M -> 86.9M
   ops/s (17.2x at 20T). The gap IS update_content's graph-SHARED-lock guid
   resolution; the per-vertex alignas(64) locks do not false-share at all (11.5
   ns/op at 20T is a lock-free-looking write). Removing the graph lock from the
   update path (epoch 1: resolve the guid once per partition, or a
   lock-free/per-shard guid index) is worth up to 17x on this bench.
4. The mixed phase is the contract's honest cost: 320 structural ops/s with 20
   updaters running vs 1 365 960/s alone, i.e. ~4000x writer starvation. This is
   the least reproducible number in the table by far — repeated runs produced 54,
   129, 161 and 514 creates in the same 1 s window, which is itself the finding:
   the unique graph lock is not fair, so a structural writer waiting behind a
   constant stream of shared-lock readers makes progress in bursts. Content updates
   meanwhile lose almost nothing (5.82M/s mixed vs 6.14M/s alone) — they are
   correctly on the other lock level. Epoch 2 candidate: a writer-preferring (or
   queued) graph lock so a structural writer cannot starve.
5. SERIALIZATION scales acceptably but is LMDB-bound, not CPU-bound: the save
   fan-out is capped at 8 workers and 1M vertices cost 12.1 s (24.9 MB/s), while
   the dual-stage load reaches 62 MB/s at 1M. The 1 % incremental save is 949 ms
   at 1M for 10 000 records (7.4 MB) — 12.7x cheaper than the full save, so the
   dirty tracking pays off. Epoch 1/2: batch-size and map-size tuning, sharded
   load cursors, and checking whether the encode fan-out or the single LMDB write
   transaction is the cost (stats.save_ms == wall time here, so stage B
   dominates: 8 workers of parallel encode finish inside a memory-bound ~1 s
   write transaction).

Epoch-1 notes (read scaling, non-allocating adjacency, sharded write path)

WHAT CHANGED (all in src/graph_db, gated by the unchanged tests):

1. The single graph-wide `std::shared_mutex` became 64 SHARD locks
   (`kGraphShardCount`, power of two; `shard = hash(guid) & 63`), each owning one
   bucketed `kimix::HashMap` on its own cache line. A reader of vertex V touches
   only V's shard; structural writes take the endpoint shard(s) UNIQUE in
   ascending shard order (connect/disconnect touch at most two; `remove_vertex`
   takes ONE shard at a time and releases it between neighbours, so it never
   holds two). Traversals copy one vertex's adjacency under one shard shared lock
   into a local buffer and release it before the next hop, so a BFS holds no lock
   between visits and parallelizes across shards. Lock order is written down in
   graph.h, with the deadlock-freedom argument and the narrowed read-atomicity
   guarantee (per-vertex atomic instead of whole-graph snapshot).
2. Non-allocating adjacency: `AdjacencyBuffer` (16 guids inline, one retained
   heap vector beyond) plus `size_t dependencies(guid, span<Guid>)` (snprintf
   convention: returns the TOTAL count). `dependencies`/`dependents`/`neighbors`
   all use that discipline internally; `has_path`/`shortest_path` reuse one buffer
   per traversal and replaced their per-hop hash-set membership test with a
   linear scan of the BFS queue up to 32 discovered vertices (the queue IS the
   visited set), falling back to a hash set above that.
3. The level-2 write path lost the global lock: `update_content` takes ONE shard
   SHARED lock for the guid -> Vertex* lookup and releases it before the vertex
   unique lock. This is the option the epoch-1 brief called the "cheaper correct
   step" — proof sketch in graph.h: the shard lock is held only across the map
   read, the returned pointer is stable because removal is a structural write
   excluded by the single-writer contract, and the vertex's own lock then
   excludes every other content access.
4. `neighbors()` dedups into a 16-entry stack array and materialises the result
   vector once (was: a `kimix::unordered_set` per call). The serializer now reads
   the dependency lists through the shard-locked Graph accessors instead of the
   raw `Vertex` accessors (the raw ones are no longer safe against a concurrent
   reader, and `dirty_vertices()`/save are documented as runnable next to
   level-2 updates).

MEASURED BEFORE/AFTER. Rows compare the committed epoch-0 code with the epoch-1
code; because the host was loaded, the authoritative comparison is the
interleaved same-session A/B between the epoch-1 build and the SAME code compiled
with `KIMIX_GRAPH_DB_SHARD_COUNT=1` (which reproduces the epoch-0 single-lock
structure exactly). min-of-N, whole suite, alternating runs:

| metric | 1-shard (== epoch 0) | 64-shard (epoch 1) | delta |
|---|---|---|---|
| read 1T user op | 3 502 ns/op | 3 516 ns/op | +0.4 % |
| read 10T user op | 2 503 ns/op | 448 ns/op | **-82 %** |
| read 20T user op | 2 876 ns/op | 241 ns/op | **-92 %** |
| read 40T user op | 2 830 ns/op | 253 ns/op | **-91 %** |
| find-only 10T | 147 ns/op | 9.7 ns/op | **-93 %** |
| find-only 20T | 159 ns/op | 6.0 ns/op | **-96 %** |
| find-only 40T | 126 ns/op | 7.3 ns/op | **-94 %** |
| update_content 10T | 143 ns/op | 29 ns/op | **-80 %** |
| update_content 20T | 167 ns/op | 15 ns/op | **-91 %** |
| update_content 40T | 159 ns/op | 17 ns/op | **-90 %** |
| Vertex::set_content 20T (control) | 11.4 ns/op | 11.6 ns/op | +2 % |
| structural 1T | 830 ns/op | 892 ns/op | +7 % |
| mixed structural (1 writer + 20 updaters) | 19.6 ms/op (51 creates/s) | 931 ns/op (1.07M/s) | **~20 000x** |
| save/load/incremental | — | — | within +/-8 % (LMDB-bound) |

Aggregate headline move against the recorded epoch-0 row: read
474k/416k/414k -> 2.36M/4.09M/4.04M ops/s (5.0x/9.8x/9.8x) and update_content
7.3M/6.1M/6.3M -> 35.0M/66.5M/60.1M ops/s (4.8x/10.8x/9.5x). The contended-find
plateau epoch 0 measured (7-8M ops/s per thread at 118-138 ns) is gone: 10/20/40
threads now reach 102M/168M/140M find ops/s aggregate, i.e. 9.8/6.0/7.1 ns per
call — the same as single-threaded (16.5 ns), so the read lock is no longer the
ceiling. Epoch-0's finding #1 ("read does not scale: a ~1.8x ceiling") is
answered: the ceiling is now the machine, not the lock.
The 1T op is unchanged by the sharding itself (the shard index and the extra
bounds check cancel against the cheaper lock), but its COMPONENTS moved:
`neighbors` 369 -> 186 ns (stack dedup) and the depth-3 bounded expansion
2 795 -> 3 108+ ns depending on machine state (the expansion now uses the
non-allocating form, but this host's load makes that comparison unsafe to
over-read; the allocation removal is visible in `dependencies` 67.8 -> 63.2 ns
for the buffer form).

Correctness was NOT traded for speed. The narrowed guarantee is read atomicity:
a multi-vertex traversal may now observe a structural mutation on one visit and
not the next (it can no longer see an inconsistent single vertex, ever). That is
inherent to letting readers of different shards run in parallel, it is documented
in graph.h, and it is what the serializer's "structurally quiescent" caller
contract already assumed. Memory safety is preserved: `remove_vertex` keeps the
victim alive for the whole cascade and returns it to the caller, so no reader can
dereference a freed Vertex. Verified by an 8-reader / 1-writer stress harness
(create + connect + cascade remove, 80 000 removals, readers resolving and
reading vertices mid-removal): zero invariant violations, zero dangling edges,
post-run sweep clean; ASan debug and release runs of both test binaries are
clean, and all 216 + 303 assertions pass with the tests UNMODIFIED.

EPOCH 3 SHOULD ATTACK, in measured order (rewritten by epoch 2, which answered
the previous list):

1. LMDB durability policy — THE finding of epoch 2, and the only remaining lever
   on save. LMDB on Windows opens its data file with FILE_FLAG_WRITE_THROUGH, so
   every 4 KiB page of a commit is a separate synchronously-completed
   write-through WriteFile: measured 105 us per 4 KiB page through an overlapped
   write-through handle against 5.7 us buffered, i.e. **~40 MB/s hard ceiling on
   the commit**, and that is what the save numbers track (30.0 MB in 774 ms = the
   serialized-page count, not the payload). `kimix::LMDB` passes only
   `MDB_NORDAHEAD` (src/core/lmdb.cpp), so MDB_NOSYNC / MDB_NOMETASYNC /
   MDB_WRITEMAP are unreachable from src/graph_db — a 100k save commit drops from
   712 ms to 38 ms with MDB_NOSYNC in the standalone probe. Making the sync policy
   a parameter of `kimix::LMDB` (and a ColdStorage knob) is a src/core change that
   this epoch was not allowed to make, and it is the highest-value next step.
   Epoch-2's sort already banked the part that needs no policy change.
2. Load's link pass is still ~80 % of a load (2.4 s of 3.8 s at 1M after the
   epoch-2 reservation): the remaining cost is Graph::connect per edge, i.e. two
   shard unique locks plus two vertex uniques plus the duplicate probe. A
   bulk-link entry point that takes the endpoint shards once for a whole
   adjacency list (still one edge at a time for correctness, still single-writer)
   is the next step, and it must not weaken the dual-stage guarantee.
3. Stage 1 of the load is now the second-largest piece at 1M (135-330 ms scan +
   19-31 ms slot build + 55-58 ms decode). Sharded cursors stay blocked on
   `kimix::LMDBIterator` (one cursor per iterator, no key-range seek in the
   wrapper), so the available win is avoiding the key+value copy in
   load_all_records, not concurrency.
4. Structural 1T and the 1T user op are both at parity with epoch 1 (867 vs 879
   ns/op; 3.31 vs 3.34 us). Epoch 2 measured the two candidates and neither pays:
   reserving the per-vertex edge containers at construction is a wash (955 vs 955
   ns/op), and the MD5 inside create_vertex is 180 ns of the 900 (20 %) but is
   required by the content-hash contract. Caching the thread-id hash is also a
   wash (927 vs 930). A real win there needs a different content-hash policy, not
   a code tweak.
5. roots() / dirty_vertices() are still O(n) with one lock acquisition per shard;
   the incremental serializer calls the latter on every save (1.0 ms at 100k).

Epoch 1 improved the headline read and level-2-write numbers by far more than the
5 % stop threshold (read 5.0x/9.8x/9.8x, update_content 4.8x/10.8x/9.5x, find-only
~13-25x), so the loop continues to epoch 2.

EPOCH-2 NOTES (serialization: order the save, size the load)

PROFILE FIRST — where the time actually went (temporary stderr probes, removed
from the committed code; all numbers release, 100k graph unless stated):

- SAVE was 96 % LMDB and 0.02 % the encode fan-out. `save_ms` decomposes as
  collect 9 ms, encode 0.2-27 ms, sort 14 ms, **one `write_all` transaction
  1139-1413 ms**. A standalone C probe against the vendored `mdb.c` (same TU
  flags, same volume) reproduced it and explained it: `mdb_txn_commit` is
  680-780 ms of the 1040 ms while `mdb_put` is 330-350 ms, and neither
  `map_size` (1 GiB vs 4 GiB vs 64 GiB: 848/1336/1120 ms) nor `MDB_NOMETASYNC`
  (1167 ms) moves it. `MDB_NOSYNC` moves it to 386 ms. Root cause read out of
  mdb.c: on Windows the data file is opened FILE_FLAG_WRITE_THROUGH and dirty
  pages are flushed individually through an overlapped write-through handle.
  Measured directly on this host: 105 us per 4 KiB write-through page against
  5.7 us buffered. That is the 4-8x "slower than load per byte" the brief asked
  about, and it is not the encoder, not the batch shape, and not `src/graph_db`.
- The 1 % incremental at 1M (10k records, 7.4 MB) taking 1.27 s was NOT a
  suspicious fixed overhead: 7.4 MB of payload is ~9.2 MB of fresh + COW pages,
  i.e. ~2300 write-through pages. The probe puts the same batch at 10.7 ms per
  1k records warm, and at 87 ms per 1k records when it is the first commit after
  a full save (the OS cache is still flushing the 300 MB that save just wrote).
  A 1-record transaction into a warm store commits in 0.09 ms and an empty one
  in ~0 ms, so there is no per-transaction fixed cost to remove in src/graph_db.
- LOAD was 87 % link pass, and stage 1 was never the problem: 100k decomposes as
  scan 12-31 ms, slot build 2-3 ms, decode 5-6 ms, materialize 4 ms, insert
  10-12 ms, **link 245-329 ms**. Inside the link pass, 600 054 edges cost
  connect 197-213 ms + a `Graph::find` per edge 77-80 ms + a made-a-copy
  `pending` list 10 ms; and inside connect, the four per-vertex edge containers'
  geometric growth was 170-302 ms of 600k calls. So: the pass's cost was
  per-VERTEX container growth, not the per-edge work.

WHAT CHANGED (src/graph_db only, tests unmodified):

1. `serialize.cpp` sorts the finished save batch into LMDB key order (stage A',
   after the encode fan-out, before the single transaction). The key is "v/" + 16
   raw guid bytes, i.e. already the bytes LMDB compares, so the comparator is a
   memcmp. This turns a random-insert B-tree build into a bulk-load one: 100k
   went from 11.9k to 9.2k dirty pages. Nothing else about the save changes — same
   records, same bytes per record, same ONE transaction (still atomic), same
   single-writer contract, and the dirty-flag settle pass still reads the
   collection-ordered `dirty[]`/`written_hashes[]`, which the sort does not touch.
2. `Graph::reserve_links(guid, outgoing, incoming)` (graph.h/.cpp) +
   `Vertex::reserve_links` (vertex.h/.cpp): reserve room for the four per-vertex
   edge containers. `reserve()` only ever RAISES capacity and never touches an
   element, so it cannot add, drop or reorder an edge. The deserializer's link
   pass now runs two sweeps: reserve every vertex's recorded degree (the record
   carries both counts), then create the edges. This is a structural write, so it
   takes that one shard's UNIQUE lock and obeys the single-writer contract like
   every other structural mutation; a no-op for an unknown guid.
3. Documentation only: the before/after numbers and the LMDB-durability finding
   are written into graph.h (EPOCH-2 RESULT), serialize.h (stage A', the sorted
   order, the two-sweep link pass) and this file.

MEASURED BEFORE/AFTER. All numbers are release, same machine, same session,
min-of-N whole-suite runs interleaved with the epoch-1 binary as the control
(3 epoch-2 runs, 2 epoch-1 runs; 100k/1M serialization also A/B'd 4x/2x on its
own because a whole suite is 55 s):

| metric | epoch 1 | epoch 2 | delta |
|---|---|---|---|
| save 100k | 1015 ms | 774 ms | **-23.7 %** |
| load 100k | 266 ms | 232 ms | **-12.7 %** |
| incremental 1 % @100k | 76 ms | 74 ms | -2.1 % (noise) |
| save 1M | 10 570 ms | 8 124 ms | **-23.1 %** |
| load 1M | 4 723 ms | 3 842 ms | **-18.6 %** |
| incremental 1 % @1M | 827 ms | 737 ms | -11.0 % |
| read 1T user op | 3338 ns | 3312 ns | -0.8 % (unchanged) |
| read 10/20/40T user op | 387/222/239 ns | 391/223/247 ns | +1.2/+0.5/+3.3 % (noise) |
| find-only 1T / 10/20/40T | 16.8 / 9.3/5.8/6.9 ns | 15.8 / 9.3/5.8/7.0 ns | flat |
| content read / deps / neighbors (controls) | 53.5 / 61.9 / 179.8 ns | 53.5 / 62.0 / 178.2 ns | flat |
| 1T bounded reach (3 hop / 64 nodes) | 2431 ns | 2316 ns | -4.7 % (noise) |
| structural 1T create+connect | 879 ns | 867 ns | -1.4 % (unchanged) |
| update_content 1T / 10/20/40T | 218 / 28.4/14.8/17.2 ns | 214 / 28.4/14.8/16.8 ns | flat |
| Vertex::set_content 20T (control) | 11.2 ns | 11.2 ns | flat |
| mixed: structural / content | 1030 / 17.0 ns | 1049 / 17.0 ns | flat |

Isolating the two changes inside ONE binary (temporary env knobs, removed from
the committed code) so host drift cannot be blamed: 100k save with the sort off
1138/1168/1183/1193 ms vs on 844/892/854/865 ms (**-26 %**, i.e. the whole save
win is the sort — the sort itself costs 14-16 ms of that); 100k load with the
reservation off 262/268/294/302/328 ms vs on 241/237/248/250/253 ms (**-10 %**,
5 interleaved reps, and the two effects are independent). At 1M the same two
changes give save 10.75 -> 8.03 s and load 4.70 -> 3.92 s.

HONEST LIMITS OF THIS EPOCH. The headline read/write/traversal numbers did not
move, and they were not supposed to: epoch 2 changed the serializer and added one
structural hook that only the loader calls, so every other path had to measure
flat — and it did, within +/-3 % run-to-run noise on this hybrid host. The
remaining save time is now 95 % one LMDB commit whose cost is the Windows
write-through page policy inside src/core/lmdb.cpp (LMDB itself, not the
wrapper's misuse of it); the only ways past it are an env flag this library
cannot reach (MDB_NOSYNC: 38 ms for the same 100k commit in the probe) or
splitting the atomic transaction, which is a contract change. Epoch 2 therefore
lands at -23.7 % save / -12.7 % load at 100k and -23.1 % / -18.6 % at 1M, i.e.
above the 5 % stop threshold on the targeted paths and exactly 0 % everywhere
else — so the loop continues, but the NEXT epoch's honest expectation is that the
serialization win must come from src/core (a sync-policy knob) or from the load's
remaining link-pass cost, not from more work inside src/graph_db.

CORRECTNESS ARGUMENT (nothing was traded). The dual-stage load guarantee is
untouched: no pointer is resolved at insert time, `reserve_links` runs in a
separate sweep AFTER every vertex is in the graph and before any edge exists, and
it only raises capacity. Edge creation still goes through `Graph::connect`, in the
same LMDB-key order and the same per-record list order, so the resulting guid
sets, adjacency and a subsequent save's bytes are bit-identical — verified by the
existing round-trip, determinism and incremental-byte-identity tests, all
unmodified. The incremental-save byte-identity property survives: the sort
reorders the WRITE BATCH, never the set of records, and a clean vertex is still
never re-encoded or rewritten. The single-writer contract is preserved
(`reserve_links` calls `_check_writer()`), lock ordering is preserved (shard
unique, never two shards at once, no vertex lock held while a shard lock is
taken), and the dual-stage/incrementality tests assert exactly the same counts as
before. Verified: debug (216 + 303 asserts + the bench's 43, green), ASan debug
(both suites green, no reports), release (bench + both suites green); no changes
outside src/graph_db.

Rows are appended, never rewritten: a later epoch that trades correctness for
speed is rejected, and every number must come from the same machine and mode as
the row above it.

EPOCH-3 NOTES (bulk edge creation + a cached roots())

PROFILE FIRST — where the time actually went (temporary stderr probes in
serialize.cpp, removed from the committed code; release, 100k store, the load's
own stage marks):

- The load decomposed as scan 8-22 ms, slot build 2 ms, decode 4 ms,
  materialize 6 ms, insert 10 ms, reserve sweep 16-28 ms, EDGE SWEEP 183-283 ms,
  settle 0.8 ms — i.e. the edge sweep was 78-85 % of the whole load, exactly what
  epoch 2 predicted.
- Inside the edge sweep, instrumenting the two calls per edge put Graph::find at
  77-80 ms and Graph::connect at 106-180 ms of a 283 ms sweep (the probe itself
  added ~100 ms of clock reads, which is why the sweep inflated from 183 to 283 ms
  while the breakdown was taken). So the find elimination was worth roughly a
  quarter of the pass and the lock choreography the rest.
- roots(): 0.593 ms at 100k vertices (14268 roots), one shared lock acquisition
  per shard. Nothing in the bench suite hammered it, but it is O(n) on demand and
  the incremental serializer's dirty_vertices() neighbour, so it was the second
  item.

WHAT CHANGED (src/graph_db only, tests unmodified):

1. Graph::connect_bulk(span<EdgeRequest const>, span<EdgeOutcome>) (graph.h/.cpp)
   + EdgeRequest / EdgeOutcome, and the deserializer's link pass now builds ONE
   edge list and hands it to ONE bulk call instead of N per-edge Graph::connect
   calls. Three things are hoisted out of the per-edge loop:
   (a) ENDPOINT RESOLUTION — every DISTINCT endpoint is resolved once into a
   staging guid -> endpoint-id index (one shard SHARED lock per unique endpoint,
   i.e. ~100k lookups instead of ~600k Graph::find calls at 100k vertices);
   (b) SHARD LOCKING — the set of touched shards is collected and then taken
   UNIQUE in ascending index order, once each, for the whole batch, so no edge
   pays an acquire/release pair and a same-shard edge needs no ordering decision;
   (c) THE WRITER CHECK — one _check_writer() per batch instead of per edge.
   The edge-creation body itself is shared, not copied: connect() and
   connect_bulk() both call _connect_resolved_locked, so the duplicate probe, the
   two vertex UNIQUE locks and the two-sided _add_depending/_add_depended
   bookkeeping are literally the same code. Vertex::_add_depending_if_absent is
   new and collapses the duplicate probe and the insert into one hash operation
   (measured separately: 118-134 ms -> 108-122 ms on the edge phase).
2. roots() is now a version-keyed cache (graph.h/.cpp): a `std::atomic<uint64_t>
   _structure_version` bumped by ONE relaxed increment inside each structural
   writer (create/add/remove vertex, connect/disconnect, connect_bulk — all
   already holding their shard lock), plus a dedicated `std::shared_mutex
   _roots_mutex` guarding the cached vector. A warm call is a shared-lock version
   compare plus one vector copy; a stale one takes the mutex unique and reruns the
   old O(n) scan (_compute_roots). Level-2 content writes and reserve_links do
   NOT bump: they cannot change the root set, which is defined by INCOMING EDGES.
3. Load stage 1 (item 3 of the brief) was measured and LEFT ALONE: at 8-22 ms of
   a 166 ms load it is now ~5-13 %, and item 1 cut the denominator.
4. Documentation only: the before/after numbers, the equivalence argument and the
   lock-order/versioning rules are written into graph.h (EPOCH-3 RESULT, LOCK
   ORDER item 3), serialize.h (the link pass) and this file.

MEASURED BEFORE/AFTER. Release, same machine, same session, min-of-N interleaved
A/B against the epoch-2 binary (kept as a copy in .kimix_cache/epoch3/, 6-12
whole-suite or single-suite runs alternating new/old):

| metric | epoch 2 | epoch 3 | delta |
|---|---|---|---|
| load 100k | 229.2 ms | 160.8 ms | **-29.9 %** |
| load 1M | 3892.6 ms | 3058.0 ms | **-21.4 %** |
| save 100k | 738.6 ms | 751.8 ms | +1.8 % (noise; untouched, LMDB-bound) |
| save 1M | 8276 ms | 7950 ms | -3.9 % (noise; per-run spread 7 950-9 433 ms) |
| incremental 1 % @100k | 69.2 ms | 68.5 ms | -1.1 % (noise) |
| incremental 1 % @1M | 753.4 ms | 729.0 ms | -3.2 % (noise) |
| roots() @100k, warm | 0.593 ms | 0.003 ms | **-99.4 % (152-172x)** |
| structural 1T create+connect | 881.7 ns | 881.2 ns | -0.06 % (unchanged) |
| read 1T / 10T / 20T / 40T user op | 3333.7 / 390.6 / 222.4 / 245.0 ns | 3434.0 / 388.7 / 222.8 / 249.3 ns | +3.0/-0.5/+0.2/+1.8 % (noise) |
| find-only 1T / 10T / 20T / 40T | 15.7 / 9.3 / 5.8 / 7.1 ns | 15.8 / 9.3 / 5.7 / 6.9 ns | flat |
| content read / deps / deps-noalloc / neighbors (controls) | 54.2 / 69.9 / 60.7 / 176.8 ns | 53.0 / 69.8 / 64.0 / 179.5 ns | flat |
| update_content 1T / 10T / 20T / 40T | 219.4 / 28.4 / 14.9 / 19.1 ns | 220.0 / 28.4 / 14.9 / 18.8 ns | flat |
| Vertex::set_content 1T / 10T / 20T / 40T (control) | 192.6 / 21.8 / 11.2 / 11.9 ns | 195.2 / 21.8 / 11.2 / 12.1 ns | flat |

Isolating each change inside ONE binary so host drift cannot be blamed:
- bulk edge creation, the only two temp knobs that shipped into the measurement
  build — single-insert off 133.8/120.4/126.7/120.4/123.0 ms vs on
  111.8/107.9/109.0/109.8/110.2 ms on the edge phase, so the one-hash-operation
  insert is ~-9 % of that phase and was kept;
- roots() bump cost, interleaved same-binary A/B with the increment compiled out:
  bump on 897.3 ms median 910.1 vs off 897.8 / 910.0 ns — -0.06 % / +0.02 %,
  i.e. a relaxed atomic increment inside a lock the writer already holds is free.
- A permanent-cost check that mattered: the FIRST version of connect_bulk stored
  Vertex* into the staging kimix::unordered_map (ankerl::unordered_dense), whose
  values MOVE on rehash; a 1M probe segfaulted. The committed version stores
  endpoint IDS into a stable kimix::vector and a guid -> id map, so a rehash only
  moves the ids.

CORRECTNESS ARGUMENT (nothing was traded). The bulk-edge equivalence was proven
mechanically rather than argued: a temporary env-gated harness (KIMIX_GDB_VERIFY_
LINK, removed from the committed code) re-ran the SAME decoded slots through the
OLD per-edge Graph::find + Graph::connect loop into a second Graph, then compared
the two graphs with the public accessors — every vertex's sorted outgoing guid list
and sorted incoming guid list — plus the vertex count and the skipped-edge count.
Result over the whole serialize suite: 11 loads compared, 0 adjacency mismatches,
skipped counts equal in every case (13/13 EQUIVALENT; the one "merge into a
non-empty graph" case is reported SKIPPED because the harness's second graph is
rebuilt from store slots only and legitimately lacks the pre-existing vertices the
bulk path links to). At scale on a 1M store: 1000000 vertices, 6 001 843 edges
created, 0 skipped, 0 adjacency mismatches. Because both paths call the same
_connect_resolved_locked body, the guid lists and the pointer sets stay in
lockstep and the reverse edges are filled identically.
The dual-stage guarantee is untouched: reserve_links still runs in a separate
sweep AFTER every vertex is in the graph and BEFORE any edge exists, and no
pointer is resolved at insert time. connect_bulk is called from the same
single-threaded link pass, so the single-writer contract holds — and it now takes
ONE writer check for the batch, which is strictly weaker than the per-edge version
it replaced (same thread, same phase). Lock ordering is preserved and extended:
the batch collects its touched shards, then acquires them once each in ASCENDING
index order, and the vertex locks are still taken shard-first and one edge at a
time. The roots() cache lock (_roots_mutex) is outside both levels and is never
taken by a writer, so it cannot join a shard-level cycle; a stale recompute reads
the version BEFORE its scan, so a structural write landing mid-scan leaves the
cache stale rather than caching a half-updated answer (verified: 13 loads × the
double-roots() probe, 0 mismatches, and every stale-recompute matched a fresh
scan). Skipped-edge semantics are unchanged: missing target, self edge and
refused edge each increment skipped_edge_count exactly once, from the same
EdgeOutcome that replaced connect's bool, never fatal.
The incremental-save byte-identity property is untouched (no save-side change).
Existing tests are UNMODIFIED. Verified: debug (216 + 303 asserts, plus the
bench's 43, green), ASan debug (both suites green, no reports), release (bench and
both suites green); `git status` shows only src/graph_db/** touched.

Epoch-3 row caveat: the row is ONE whole-suite release run (the step-3
verification run of the epoch-3 session), so its cell-to-cell noise is the
machine's — especially on the 10/20/40T cells and on the mixed-write and save
cells that epoch 0/1 already flagged as the least reproducible in the table (the
save cells here are the clearest example: 786 ms and 8 576 ms in this run vs
738/751 ms and 8 276/7 950 ms in the A/B runs minutes earlier, on code that
cannot differ because epoch 3 changed nothing on the save path). The
authoritative epoch-3 verdict is the interleaved min-of-N A/B in the epoch-3
notes, not a row-to-row read; the row is here so the columns stay comparable, and
its load cells (168 ms / 3 099 ms) agree with the A/B minima (160.8 ms /
3 058 ms) to within 5 %.

STOP RULE (graph_db_plan.md §7, "stop when a full epoch yields < 5 %"). The
headline targeted path moved -29.9 % at 100k and -21.4 % at 1M, i.e. the epoch is
well above the 5 % threshold, so the rule does not fire on this epoch's own
result. What epoch 4 would attack, in measured order:

1. The SAVE, and only through src/core. 95 % of a save is one LMDB commit (774 ms
   of 813 ms at 100k in epoch 2, and still 8.0 s of 8.1 s at 1M here – epoch 3
   changed nothing on the save path). The standalone probe already put the same
   100k commit at 38 ms with MDB_NOSYNC against 712 ms today, so making the sync
   policy a parameter of kimix::LMDB plus a ColdStorage knob is a ~20x save win
   sitting behind a file this loop is forbidden to touch. Nothing else in
   src/graph_db is worth 5 % on save.
2. Load stage 1, 8-22 ms of the remaining 166 ms (5-13 %): a sharded cursor is
   blocked on kimix::LMDBIterator (one cursor per iterator, no key-range seek in
   the wrapper), so the available win is avoiding the key+value copy in
   load_all_records — again a src/core change.
3. The load's edge phase is now ~110 ms of 166 ms, spread over 6 M container
   inserts at 1M; the measured sub-costs are the two vertex UNIQUE locks per edge
   (~8 % of the phase) and the hash inserts themselves. Dropping the vertex locks
   would be a documented invariant change for <5 % of the headline, so it is
   rejected on the plan's own rule ("reject epochs that trade correctness for
   speed").
None of those is a ≥5 % headline inside src/graph_db, so per the plan's stop rule
the optimization loop is STOPPED at epoch 3: this is the final epoch. The next
real step is a src/core sync-policy decision, not another src/graph_db epoch./n

### 6.1 Backend dimension — `lmdb` vs `flat_file` (plan 05eff1f, Phase 4)

`GraphOptions::store_backend` selects the cold store and nothing else changes: the
same serializer, the same deserializer, the same workload (§4/§5). The bench now
runs that workload on BOTH backends at BOTH scales, each with a fresh graph from
the same seed and a fresh store, so every cell below is the same operation over the
same graph bytes — the only difference is the store (and, on the file side, the
extra key + frame bytes it writes, see "disk" below).

Host/mode: AMD Ryzen 9 7950X3D (16C/32T, 64 GB), Windows 11, `mode=release` MSVC,
2026-10-25 — the plan_v3 host, so these cells are NOT comparable with the
Intel-host epoch rows above except as same-session ratios. Sources: one whole-suite
run plus three filtered repeats (`test_graph_db_bench.exe bench_serialize_100k`,
`... bench_serialize_large`); each cell is the MINIMUM of the four, as every row of
this file is. Raw lines: the store benchmark report, §8.3.
These cells were measured while the file backend still read and wrote through
the vendored native block-device classes. That library is gone: the path is now plain
positioned CRT I/O (`fopen` + `_fseeki64`/`fseeko` + `_commit`/`fsync`), one open
per pass, 64-bit offsets, no async queue and no NVMe device behind it. The I/O
the rows describe has the same shape (one appended frame per save batch, one
positioned sequential pass per load), so the numbers remain the right order of
magnitude, but they are a historical measurement of this workload rather than a
re-runnable claim about the current transport.

| scale | row | lmdb | flat_file | delta (file vs lmdb) | graph.xdb |
|---|---|---|---:|---:|---|
| 100k | full save | 3 488.6 ms (20.2 MB/s) | **746.1 ms (94.4 MB/s)** | **-78.6 % (4.7x faster)** | 92.2 MB |
| 100k | dual-stage load | **352.1 ms (199.9 MB/s)** | 395.2 ms (178.2 MB/s) | +12.2 % slower | — |
| 100k | incremental 1 % (1 000 rec, 763 808 B) | 120.4 ms (6.3 MB/s) | **7.8 ms (98.3 MB/s)** | **-93.5 % (15.5x faster)** | — |
| 1M | full save | 28 553.2 ms (24.7 MB/s) | **10 389.8 ms (67.8 MB/s)** | **-63.6 % (2.75x faster)** | 922.2 MB |
| 1M | dual-stage load | **6 161.9 ms (114.3 MB/s)** | 7 897.0 ms (89.2 MB/s) | +28.2 % slower | — |
| 1M | incremental 1 % (10 000 rec, 7 629 808 B) | 1 103.2 ms (6.9 MB/s) | **69.6 ms (109.6 MB/s)** | **-93.7 % (15.9x faster)** | — |

MB/s is `bytes / wall_ms` for the value bytes the operator wrote or read (the
bench's own convention, unchanged from §3 of the report); the file backend's
on-disk footprint is larger because a record in the log carries its 18-byte key
plus a 12-byte length prefix next to the value — 92.2 MB vs 70.4 MB of values at
100k, 922.2 MB vs 704.2 MB at 1M.

VERDICT (honest, and it does not go the way the pre-measurement expectation was
hedged):

- **The save wins, big and reproducibly.** 4.7x at 100k and 2.75x at 1M on the
  full save, ~15x on the 1 % incremental. The cause is the sync policy, not the
  storage medium: LMDB on Windows opens `data.mdb` `FILE_FLAG_WRITE_THROUGH`, so
  each commit is a chain of synchronous page flushes (epoch 0-3 already showed the
  commit is 95 % of a save), while the file backend writes ONE appended frame and
  pays ONE `sync()` per batch. The incremental row is the purest form of that
  (7.6 MB written in 69.6 ms vs 1 103 ms) — same bytes, one flush instead of a
  write-through commit.
- **The load does not win, and that is expected.** Per byte read the two backends
  are a tie: at 100k 178 MB/s (file) vs 200 MB/s (lmdb), at 1M 117 MB/s (file, of
  the 922 MB it actually reads) vs 114 MB/s (lmdb, of the 704 MB of values). Both
  sides read the same page-cached file with `fread`-class calls, so there is no
  slower read to blame. The wall-clock LOSS (+12 % at 100k, +28 % at 1M) has two
  causes, neither of which is a slower read: (a) the log carries 18 B key + 12 B
  frame lengths next to every value, so a 1M store is 922 MB to read instead of
  704 MB, and (b) the log owes its map semantics a last-wins dedupe, so its scan
  builds a ~7 M-entry key index that a B-tree gives LMDB for free in key order (it
  is only built for a load, not for the meta pass).
- **What the backend choice is for, therefore:** workloads dominated by saves —
  checkpoint-heavy or incremental-persist-per-commit workloads, where 15x on the
  incremental path changes what is affordable. It is NOT a load optimization and it
  is NOT free on disk.
- **The default does NOT change.** `ColdStoreBackend::lmdb` stays the default.
  The save win is real, but flipping the default would (a) trade LMDB's 64 GiB map
  for a single segment that grows with every incremental save, (b) silently produce
  an EMPTY graph for an existing LMDB store — a directory holding `data.mdb` opened
  with `store_backend = flat_file` gets a fresh `graph.xdb` beside it and an empty
  load, because there is no cross-backend detection or migration, and (c) change the
  C-ABI behavior, which has no field for the option.
  Flipping it is a planner decision with a migration story, not a benchmark
  result; this phase records the numbers and leaves the switch opt-in.

Stop rule (graph_db_plan.md §7 / §6 above, "stop when a full epoch yields < 5 %"):
the backend work is not an epoch over existing code — the lmdb path is unchanged,
cell for cell, so the loop stays STOPPED at epoch 3 for it. Read as a change, its
headline move is -78.6 % / -63.6 % on save, far above the 5 % threshold, so the
rule does not fire; the load cell is a 12-28 % REGRESSION and is reported as such,
not as a tie.
---

### Plan v3 GDI epoch log (time-cost epochs, GDI-shaped rows)

Plan v3 (`reports/plan_v3.md`) re-opened the epoch loop for the GDI command
scenarios. Same discipline: release mode, min-of-N interleaved runs, a <5 %
full re-run closes the epoch. Rows here, detail there.

| machine | mode | epoch | metric | before | after | delta |
|---|---|---|---|---:|---:|---:|
| AMD Ryzen 9 7950X3D, 16C/32T, 64 GB | release | v3-4.1 | graphdb/gdi/degree_probe (native) | 3565.5 ns | 280.8 ns | 12.7x |
| " | " | v3-4.1 | graphdb/gdi/neighbors_page100 (native) | 2916.2 ns | 277.4 ns | 10.5x |
| " | " | v3-4.1 | BDN DegreeProbe (adapter, managed) | 2.31 us | 196.5 ns (0 B) | 11.8x |
| " | " | v3-4.1 | BDN NeighborsPage100 (adapter) | 3.84 us | 433.8 ns | 8.9x |
| " | " | v3-4.1 | read_degree_count (graphbench e2e) | 177.7k ops/s | 533,368 ops/s | 3.0x (target >=300k MET) |
| " | " | v3-4.1 | read_neighbors_page (graphbench e2e) | 140.0k ops/s | 392,319 ops/s | 2.8x (target >=275k MET) |
| " | " | v3-4.3 | BDN GetProperties (adapter, managed) | 623.6 ns | 300.3 ns (1008 B) | 2.08x |
| " | " | v3-4.3 | read_properties (graphbench e2e) | 270.7k ops/s | 303,557 ops/s | +12 % (target >=340k residual owned by step 4.8: engine per-command overhead) |
| " | " | v3-4.4 | BDN InsertVerticesBulk (adapter) | 3.08 us/row (2.3 KB) | 1.358 us/row (1010 B) | 2.27x |
| " | " | v3-4.4 | insert_vertices_bulk (graphbench e2e) | 139.7k ops/s | 180,478 ops/s | 1.29x (target >=180k MET; inmemory parity 93.8 %) |
| " | " | v3-4.5 | BDN InsertVerticesBulk XXH3 vs MD5 (A/B) | 1.358 us/row | 1.309 us/row | +3.6 % (below the 5 % stop rule; flag kept for content-heavy consumers, ABI 6) |
| " | " | v3-4.6 | BDN InsertEdgesRowwise (adapter) | 1.32 us (448 B) | 1.202 us (168 B) | -9 % time, -62 % alloc |
| " | " | v3-4.6 | BDN EdgeCrudCycle (adapter) | 1.58 us (744 B) | 1.419 us (464 B) | -10 % time, -38 % alloc |
| " | " | v3-4.6 | insert_edges_rowwise (graphbench e2e) | 162.6k ops/s | 168.3k ops/s | +3.6 % (target >=220k residual owned by step 4.8: engine per-command overhead is 78 % of e2e) |
| " | " | v3-4.6 | edge_crud_cycle (graphbench e2e) | 115.3k ops/s | 121.7k ops/s | +5.4 % (target >=165k residual owned by step 4.8) |
| " | " | v3-4.8 | BDN engine reads (100-cmd buffer) | 88.9 KB | 62.4 KB | -30 % alloc (SingleOutputMap + per-batch CommandContext) |
| " | " | v3-4.8 | BDN engine row-wise edges (100-cmd buffer) | 277.9 KB | 251.5 KB | -9.5 % alloc |
| " | " | v3-4.8 | read_properties (graphbench e2e) | 270.7k ops/s | 338.1k ops/s | parity 98 % vs same-session inmemory (target >=340k referenced stale machine state) |
| " | " | v3-4.8 | insert_edges_rowwise (graphbench e2e) | 162.6k ops/s | 175.4k ops/s | parity 84-94 %; residual = native write premium, lever recorded (journal slimming) |
| " | " | v3-4.8 | edge_crud_cycle (graphbench e2e) | 115.3k ops/s | 124.9k ops/s | parity (reference swings 119-188k; residual = native write premium) |
| " | " | v3-5.2 | native store, projected 50k/300k (staged RSS) | 261.2 MB | 223.9 MB | -37.3 MB: one adjacency representation (EdgeLink lists only; guid projections + Vertex* sets deleted) |
| " | " | v3-5.2 | edge bytes (record+links+label+weight) | 607.0 B | 517.3 B | -14.8 % |
| " | " | v3-5.2 | keyed vertex bytes | 1,579.0 B | 1,371.3 B | -13.2 %; sizeof(Vertex) 512 -> 320 |
| " | " | v3-5.2 | edge_create_rowwise (native bench) | 935.7 ns | 533.3 ns | -43 % (less per-edge maintenance) |
| " | " | v3-5.3 | graphbench peak working set (kimix 50k/300k) | 424.5 MB | 381.4 MB | slim transaction journal (Fat behind unique_ptr; creates alloc-free); peak-setting phase transient -40 MB |
| " | " | v3-5.4 | graphbench peak working set (kimix 50k/300k) | 381.4 MB | 341-348 MB | PropertyValue 96 -> 24 B tagged union + exact link reservation in edge bulk |
| " | " | v3-5.6 | graphbench peak working set (kimix 50k/300k) | 424.5 MB | 339.5 MB (median of 3) | GATE <=300 MISSED by 13 %: native store 261->187 MB delivered; remainder is .NET GC segments + engine transients (~113 MB), engine-side future work |
| " | " | v3-6 | native store, projected 50k/300k (final, staged RSS) | 261.2 MB | 187.7 MB | -28 % across 5.2-5.4; edge 433.0 B, vertex keyed 1,153 B |
| " | " | v3-6 | edge_create_rowwise (native bench, final) | 935.7 ns | 495.3 ns | -47 % (5.2 de-dup + 5.3 slim journal); reads flat (degree 281.7 ns, page 278.9 ns, 2hop 255.7 ns) |
| " | " | v3-6 | verification sweep | — | 15 suites x (release + debug + ASan/UBSan) | all green; C# 57/57 + 44/44; FFI AOT 151/151 |
| " | " | v3-6.6 | read_aggregate_avg (graphbench e2e) | 129 ops/s | 1,634 ops/s | 12.7x: zero-copy constraint probes (Vertex::find_property + matches_probe), no per-vertex property copies |
| " | " | v3-6.6 | read_filtered_neighbors (graphbench e2e) | 93k ops/s | 208.5k ops/s | 2.2x (158 % parity): one-lock adjacency snapshot + probe filter, no per-edge endpoint lookups |

v3-4.1 change: the UNFILTERED neighbourhood read (empty constraint, any cap,
the cap-0 degree probe included) answers from ONE shard SHARED lock snapshot
of the anchor's adjacency projection (`Graph::oriented_adjacency`) instead of
per-edge endpoint round-trips plus per-neighbour label/property resolution.
Distinct-neighbour semantics unchanged (parallel edges / both-halves /
self loops collapse via the same seen dedup); neighbours are reported as
recorded at the anchor snapshot and not re-validated (the traverse_bfs
contract). A per-guid counter cache was considered and rejected: the bench
probes random vertices over the full 100k set, so it would never warm.


## 5b. GDI layer: lock hierarchy v2 (authoritative copy in `graph.h` + `database.h`)

Epoch 4 added two shard families and a request-level schema, so the levels are now
five, always acquired lowest-numbered first, and inside one level in ascending
shard/index order:

| Level | Lock | Guards | Taken by |
|---|---|---|---|
| 1 | `Schema::_mutex` (`shared_mutex`) | label / property-type / constraint registries | `Database` schema commands, `Schema::resolve_*` on a write |
| 2 | vertex shards (`Graph::_shards[i]`) | the vertex map, `_out_links` / `_in_links`, the neighbour projections, the key fields | structural vertex/edge writes (unique), reads and traversals (shared) |
| 3 | edge shards (`EdgeTable::_shards[i]`) | the edge map **and every edge's endpoints, direction, labels and properties** | edge CRUD and edge payload access |
| 4 | key-index shards (`KeyIndex::_shards[i]`) | `(label, external_id) -> uid` | vertex create / free, `translate_vertex_id` |
| 5 | leaf: `Vertex::_mutex` | content, labels, properties of one vertex | payload reads (shared) and writes (unique) |

Rules that keep it deadlock-free, and that the code actually follows:

- a structural write takes 2 -> 3 (or 2 -> 4) and never a level-5 lock while a
  shard lock is held;
- a payload write takes level 2 **shared only to resolve the guid, then releases
  it**, and takes the leaf lock afterwards (`Database::add_property` and friends);
- an edge payload write takes exactly one level-3 lock (an edge record is small, so
  it is guarded by its shard rather than by a per-edge mutex — that is the memory
  tradeoff documented in `edge.h`);
- endpoint mutation is the one place that needs both: it takes the involved vertex
  shards in ascending order (level 2) and then the edge's shard (level 3), so a
  reader can never observe the record and the adjacency disagree;
- `EdgeTable` and `KeyIndex` shard counts are `static_assert`ed equal to
  `kGraphShardCount`, because one index order is easier to reason about than three.

Because every structural write is single-writer by contract (unchanged), the
multi-lock paths have at most one participant; the ascending order makes them safe
anyway, and a 4-reader / 1-writer stress test (in `test_graph_db_read.cpp`) runs
the filtered-read path against a live writer.

## 5c. Key spaces and record formats (format 2)

One LMDB database, partitioned by key prefix (`utils.h`). The loader makes ONE
sequential scan and dispatches on the prefix, so an edge or schema record can
never be mistaken for a vertex.

| Key | Value |
|---|---|
| `"v/"` + 16B vertex guid | vertex record v2 (`"GDB2"`) |
| `"e/"` + 16B edge uid | edge record (`"GDE2"`) |
| `"s/labels"` | label table (`"GDS2"`) |
| `"s/proptypes"` | property-type table (`"GDS2"`) |
| `"meta/root"` | meta record (`"GDM2"`: format version, store uuid, both save timestamps, schema version, the two id counters, graph name) |
| `"i/"` | reserved for future secondary indexes — never written |

Vertex record v2 (`utils.h` has the byte layout):

```
[4B "GDB2"][16B guid][16B content MD5]
[4B key label][4B n_labels][4B n_props][4B n_dep][4B n_depended][4B ext len][4B reserved]
[ext id][n_labels x 4B][n_props x property entry]
[n_dep x 16B][n_depended x 16B][content]
```

The two dependency tables are a **compatibility projection** of the edge set: the
`"e/"` records are authoritative, and the loader only synthesizes an edge from a
link that no edge record covers (which is exactly what a v1 store needs, and what
keeps a hand-written store loadable). Properties are interned
(`{4B name id, 1B kind, 1B dtype, 2B reserved, 4B count, 8B payload length,
payload}`), so a rename is O(1) and a record stays small.

**Deletions are tombstones.** A save is one LMDB write transaction and a write
transaction cannot remove a key, so a deleted vertex or edge is stored as an EMPTY
value under its key (counted in `deleted_vertex_count` / `deleted_edge_count`) and
the loader treats an empty value as "gone" — silently, never as a corrupt record
(`tombstone_count`). Without this a reload would resurrect everything deleted since
the last save. The tombstone keys are never reclaimed; a `copy_to()` checkpoint of
a heavily churned store keeps them, and compacting them is a later epoch.

**Migration**: a store without `meta/root` is format 1. Its `"GDB1"` records load
unchanged (no key, no labels, no properties; content preserved) and each
dependency guid becomes a directed edge whose id is `MD5(origin ‖ target)` — so a
v1 store loads to the SAME graph twice and a later save writes stable ids. A store
that announces a format NEWER than `KIMIX_GRAPH_DB_FORMAT_VERSION` is refused with
`error_bad_file` before anything is materialized.

## 5d. Capability matrix (honest, per phase)

| Flag | value | why |
|---|---|---|
| `supports_global_scan` | **true** | G1 landed (plan v3 step 2): `Graph::for_each_vertex` (shard-ascending, one SHARED lock at a time) + `Database::scan_vertices`, exposed through the C ABI (`kimix_gdb_scan_vertices`, ABI 3) |
| `supports_edge_by_stable_uid` | **true** | edge uids are records and survive save/load |
| `supports_edge_property_read` | **true** | edges carry labels and properties |
| supports_native_multi_hop | true (depth 32) | G3 landed (plan v3 step 1): Graph::traverse_bfs + the bounded shortest_path, exposed through Database, the C ABI (kimix_gdb_traverse_bfs / _shortest_path, ABI 2) and the kimix_graphdb_* re-exports. The same family also answers the EDGE half of the section 5.10 filter (kimix_gdb_shortest_path_edges, section 5h; the whole-neighbourhood form kimix_gdb_traverse_bfs_edges, section 5i — no new capability bit: ABI 7 makes the symbol and its layout fields mandatory, so the advertisement stays honest unchanged) |
| `supports_global_aggregates` | **true** | G4 landed (plan v3 step 2): `Database::aggregate_vertices` (count/min/max/sum/avg, per-shard partials merged in shard order over a fiber fan-out), exposed through the C ABI (`kimix_gdb_aggregate`, ABI 3) |
| `supports_collective_transactions` | **true** | read-only scope; **no snapshot isolation** (single-writer store), documented in `transaction.h` |
| `enforces_unique_external_ids` | **true** | the `KeyIndex` refuses a duplicate (`KeyMode::report_warning` is the warning path) |

## 5f. G3 (plan v3 phase 1) — native multi-hop traversal, measured

One native call now does the level-by-level BFS the engine used to compose
from per-hop neighbour reads: `Graph::traverse_bfs` (per-level cap, bounded
depth, §5.10 constraint filter gating ENQUEUE) and a bounded
`Graph::shortest_path`, one shard SHARED lock per visited vertex, the
`VisitedSet` linear-first hybrid of `has_path`, deterministic discovery order.
`Database` forwards them with `_resolve_constraint`; the C ABI and the
kimix_api re-exports carry them under ABI 2 (`kimix_gdb_traversal_vertex`,
`KIMIX_GDB_MAX_HOP_DEPTH = 32` = `Graph::kMaxNativeHopDepth`).  Measured on the
graphbench 50k/300k dataset shape (`test_graph_db_bench` release, this host,
plan v3 step 1.12):

| shape | ops/s |
|---|---|
| engine-composite 2-hop (5,576 single-hop reads) | 222,643 reads/s |
| native `traverse_bfs` per anchor (cap 100/level, depth 2) | **434,047 walks/s** (2,304 ns each) |
| graphbench `read_2hop_bfs` end-to-end over kimix (native command path) | **65,639 ops/s** (was 18,891 composite) — rma parity target was ≥ 46k |

## 5g. G1/G4 (plan v3 phase 2) — global scan + global aggregates, measured

Machine/mode as §5f (Ryzen 9 7950X3D, release, 2026-10-25).  The
graphbench rows run ONE command per op through the engine (the engine worker
has no fiber scheduler bound, so the aggregate takes its documented serial
inline path); the native rows call the C ABI directly.

| scenario | kimix | in-memory | note |
|---|---:|---:|---|
| `read_scan_page` (ScanVerticesCommand cap=100) | **2,065 ops/s** (484 us/op) | 942 ops/s | a full 50k-vertex shard walk per op, page capped at 100 |
| `read_aggregate_avg` (AggregateNeighborsCommand Avg(age)) | 183 ops/s (5.5 ms/op) | 745 ops/s | O(vertices) serial walk; the in-memory store's plain dictionary pass is cheaper per vertex — the fiber fan-out needs a bound scheduler (the test suite's determinism suite runs it with 4 workers). No acceptance target exists for this new row; recorded as the phase-2 baseline |

## 5h. Edge filtering — the EDGE half of the traversal filter (edge searching)

GDI section 5.10 filters VERTICES: a traversal constraint gates the discovered
vertex, which is what `traverse_bfs` and the neighbour reads implement. Nothing in
that family could express the other half — "walk only over relations of this
kind" — because every walk enumerates a vertex's neighbour-guid projection and
never looks at the edge it steps over. Since the edge epoch the container, the
facade and the C ABI all answer it:

| level | entry point | shape |
|---|---|---|
| container | `Graph::oriented_links(guid, orientation, vector<EdgeLink>&)` | the oriented (edge uid, neighbour uid) link list from ONE shard SHARED lock |
| container | `Graph::edge_matches(edge, constraint)` | does THIS edge's own payload satisfy a constraint (ONE edge-shard SHARED lock, zero-copy probe — no labels/properties copy) |
| container | `Graph::shortest_path(from, to, constraint, orientation, max_depth, vector<PathStep>&)` | bounded, oriented, EDGE-filtered shortest path; `PathStep{vertex, edge, depth}` reports which edge carried each hop (zeroed at the start) |
| container | `Graph::has_path(from, to, constraint, orientation)` | the reachability probe over matching edges (bounded by `kMaxNativeHopDepth`) |
| facade | `Database::shortest_path(..., constraint, orientation, ...)` / `Database::has_path(..., bool&)` | the same two after `_resolve_constraint`, so a registered handle and the label-op rule behave exactly like the read commands |
| C ABI | `kimix_gdb_shortest_path_edges` (`kimix_gdb_path_step`) | two-phase buffer protocol, `0` = every edge, `ORIENTATION_*` = which way the walk goes |

Semantics, all inherited from the read commands rather than invented:

- an EDGE filter is the section 2.6 rule (the edge's labels AND-ed with its
  properties), evaluated by the same `matches`/`matches_probe` code, so a
  constraint means the same thing whether it filters a read or a hop;
- a refused edge is not a hop: its target is not discovered, so the walk routes
  around it (a refused VERTEX is never expanded — the vertex-only mirror);
- parallel edges: the link list holds one entry per edge and the first matching
  one wins, so `PathStep::edge` is the only place the choice is observable;
- `from == to` is the zero-length path and traverses no edge, so no edge
  constraint can exclude it;
- unknown endpoints / bounds / label ordering ops keep the vertex-only
  contract (`error_vertex` / `error_argument` / `error_op`); unreachable within
  the bound over matching edges is an EMPTY path with status ok;
- read atomicity is per visit for a vertex's adjacency and PER VISIT BATCH for
  that visit's edge tests: one vertex shard SHARED lock copies the oriented link
  list and is released before the next visit, then -- for a visit wide enough to
  pay for it -- the visit's candidate edges are tested together under ONE
  edge-shard SHARED lock per TOUCHED shard, in ascending index. No lock is held
  between visits, and no vertex-shard lock is ever held while an edge-shard lock
  is taken (the vertex shard is level 1 and the edge shard level 3 in the lock
  order of `graph.h`; only the ABSENCE of a level 3 -> level 1 acquisition keeps
  the wait-for graph acyclic -- a level 1 -> level 3 one is the direction the
  hierarchy permits, and the structural write paths take it too). A concurrent
  writer may therefore be observed on one visit and not the next. The batch
  changes WHEN an edge of that visit is tested and which state the test sees: **in
  a structurally quiescent graph** it never changes which vertices the walk
  discovers, because the candidates are filtered out of order but consumed in
  ORIGINAL link order; in a graph a writer is mutating concurrently it can, because
  a wide visit's tests span the per-link head plus up to seven drains (a hub visit
  drains 64 + 256 + 1 024 x 4 + 520), so a writer change landing between two drains
  may flip a verdict and with it a discovery. That is the SAME anomaly class as the
  inter-visit skew above and it trades nothing in memory safety or quiescent
  determinism.

Cost, measured (`tests/unit/graph_db/test_graph_db_bench_edges.cpp` -- its `[bench]`,
`[mem]` and `[ctr]` rows -- release MSVC x64 build, one pinned core, median of three
RECORD-OK runs; "E0" is the same file's pre-optimization row, so every ratio below is
a before/after of the SAME measurement, not a claim against another machine):

| row | E0 ns/op | now ns/op | ratio | `[mem]` allocs/op E0 -> now |
|---|---|---|---|---|
| **S6 unfiltered 3-hop / F1 (CONTROL)** | 856.8 | 841.4 | 1.02x | 18 (untouched) |
| S1 1-hop, label 100 %, F1 | 150.4 | 96.1 | **1.57x** | 8 -> **0** |
| S1 1-hop, hub F4 (early hit) | 2 742.7 | 2 606.3 | 1.05x | 9 -> 1 |
| S2 3-hop, label 100 %, F1 | 1 919.2 | 1 330.2 | **1.44x** | 27 -> **0** |
| S2 3-hop, label 50 %, F2 | 385.1 | 281.8 | 1.37x | 12 -> **0** |
| S2 3-hop, label 100 %, F3 | 14 373.2 | 9 126.9 | 1.57x | 48 -> 9 |
| S2 hub F4 | 697 342.2 | 447 296.7 | 1.56x | 100 -> 47 |
| S3 8-hop, label 100 %, F1 | 115 391.6 | 37 247.0 | **3.10x** | 61 -> 14 |
| S4 unreachable, F5 | 2 039.4 | 1 561.0 | 1.31x | 9 -> **0** |
| S4 unreachable wide, F1 | 1 167 468.0 | 541 521.9 | **2.16x** | 70 -> 19 |
| S5 `has_path` false, F5 | 2 047.6 | 1 400.9 | 1.46x | 9 -> **0** |
| S5 `has_path` false, F1 | 1 297 831.2 | 476 595.7 | **2.72x** | 70 -> 19 (peak 196 608 -> **65 536 B**) |
| S7 `both` 2-hop, F4 | 736 766.8 | 450 872.3 | 1.63x | 100 -> 47 |
| S10 C ABI fill 3-hop, F2c | 313.0 | 199.7 | 1.57x | 13 -> **0** |

26 of the 39 `[mem]` rows are exactly 0 allocs / 0 B / peak 0, and no row's peak
regressed. The control row is what keeps the "unfiltered fast paths untouched"
promise honest: -1.8 %, i.e. inside its own spread.

What each lever is, and what it is not:

- the batch is a **LOCK-COUNT** lever, not a time lever. `edge_matches` measures
  20.4 ns and only ~5-7 ns of that is the edge-shard lock (S11 micro-row), so
  halving the hub visit's lock count (`[ctr] edge_locks` 9 000 -> about 4 4xx; the
  exact point value scatters 4 384-4 489 per process because touched shards depend
  on guid hashing -- never quote an absolute) left that row inside its own spread.
  What the batch costs when it misfires is real, so it is applied only to a visit
  whose link list is at least `kEdgeFilterMinLinks` (128) wide: every narrow visit
  of every fixture stays per-link (S1/F1 1 probe, S2/F1 37, S2/F3 232, S3 515,
  S4-wide 3 006).
- a wide visit batches its TAIL only: the first `kEdgeFilterHeadLinks` (64) links
  take the per-link path, the rest is drained in ESCALATING chunks (64 -> 256 ->
  1 024, then 1 024 steady). The head is what preserves the early exit -- a drain
  stages before it consumes, so without the head a 1-hop hub lookup probed all
  5 000 links and took 320 edge locks where 8 probes and 8 locks answer it:
  `edges/S1_1hop_label100/F4` measured 96 888 ns (33x) before the split and
  2 709 ns after. The escalation bounds the same effect just past the head: a hit
  inside a drain is only noticed when that drain is consumed, so an early hit costs
  the current drain -- ~1.3 us right after the head, ~5 us in the 256 drain, ~21 us
  on the steady-state 1 024 chunk. A bound, not a per-link guarantee.
- the chunk (`kEdgeFilterChunk` = 1 024 candidates per drain) is a DELIBERATE
  peak-for-time trade: the staging arrays stay at "order 4 B + match 1 B per
  candidate" (~5 KB) however wide the visit is instead of growing with it.
- a LEVEL-wide batch is worse and is not what ships: with the visited check ahead
  of the filter, 79-83 % of a level's links point at vertices the SAME level
  discovers, so deferring discovery re-probes them (`[ctr] edge_tests` 515 -> 2 460
  on S3, exactly `links_snapshotted`). In the intermediate build that implemented
  it, draining a whole level measured ~2.4-2.5x slower on S3 (139-149 us against
  51-58 us); that build was not retained, so the number is quoted as a band and the
  mechanism above is what makes the deviation sound.
- neither constant is a fitted knee: 128 sits above the width where a level drain
  measured the regression above and below the hub rows that lose ~50 % of their
  edge locks; 64 covers the measured early-hit row by 8x. Both are conservative
  choices, recorded as such in `graph.cpp`.
- the visited check runs BEFORE the edge filter, which is what removed most of the
  filter work on the deep rows: `[ctr] edge_tests` fell from 17 460 to 3 006 on
  S4-wide (-83 %) and from 2 460 to 515 on S3 (-79 %) while `visits` and
  `links_snapshotted` stayed identical, i.e. discovery order, level widths and
  snapshot volume are untouched. A NEWLY discovered neighbour still takes the
  filter in link order, so the first matching parallel edge still wins for
  `PathStep::edge`.
- the walk's per-call state is stack-resident: the node array, both level frontiers
  and the link snapshot keep 64 / 64 / 16 entries inline and grow to the heap only
  past that, so a short filtered walk allocates NOTHING. The state is a plain local
  of 4.6 KB of buffer in one stack frame (0.44 % of a 1 MiB marl fiber), with no
  recursion and nothing that grows with the graph; its buffers are raw storage
  written by placement new, so they cost stack space and no stores. The only
  `{}`-initialised part is the level-3 scratch, and only once a wide visit ENGAGES
  it (its shard tables must start zeroed): +1 280 B, so 0.56 % (~5.9 KB) then.
  Where a row still allocates, it is that inline-budget growth and nothing else:
  S2/F3 9, S3/F1 14, S4-wide 19, S5_haspfalse/F1 19 (peak 65 536 B), the hub and
  S7`both`/F4 47 (peak 655 360 B = E0's figure, 160 000 B of it the hub's one
  5 000-link snapshot). Nothing allocates per candidate or per visit.
- the visited set of THIS walk is its own exact table (open addressing over node
  INDICES, keyed by a Fibonacci mix of the guid's two words, collisions resolved by
  a full 16-byte guid compare, 512 slots inline): the ankerl set keyed on the
  exported XXH3 `Guid` hash was 2 460 / 17 460 / 9 000 membership probes on the
  deep rows, i.e. the dominant remaining term, and replacing it is what moved the
  wide rows on top of the levers above (S3 1.51x, S4-wide 1.20x, S5/F1 1.32x, hub
  1.54x on top of their earlier state). It is exact: a collision always forces the
  guid compare, so it can never report a member that is not one.
- `has_path` is a mode of the SAME walk, not a second BFS: its node record is the
  16-byte guid alone, and the parent/depth/edge payload plus the whole `PathStep`
  assembly (including the caller's buffer) do not exist in that instantiation.
  That is the memory lever: peak 196 608 -> 65 536 B on the wide `has_path(false)`
  row (-67 %) and 1 -> 0 allocations on the `has_path(true)` rows.
- the discovered nodes keep their parent INDICES instead of a
  guid-to-predecessor map, `nodes` IS the visited container (there is no second
  discovery mirror), and a reused `out` keeps its capacity (the path is filled in
  place, no `std::reverse` pass).
- the facade no longer copies the `Constraint` per call, and the C ABI reuses one
  per-session staging buffer instead of building a fresh `vector<PathStep>` per
  call, so the two-phase (count-only, then fill) protocol costs no allocation at
  all: S10 3 -> 0 allocs/op. `KIMIX_GDB_ABI_VERSION` stays 6 and `graph_c_api.h`
  is unchanged; the session struct gained one private member and is opaque to C
  consumers, so its single allocation is per session, not per call.
- the UNFILTERED fast paths (has_path, the vertex-only shortest_path and
  traverse_bfs, i.e. the S6 control rows) keep their guid-projection algorithms and
  never touch the batch, the reachability mode or the walk-local visited table; the
  only change they see is the allocation-free visited set (one fewer allocation per
  call: S6 `has_path`-false `[mem]` 51 -> 49 allocs/op).

Known limitation, recorded rather than hidden: the duplicate-neighbour *re-enqueue*
inside one drain is only observable in a `KIMIX_GRAPH_DB_WALK_COUNTERS` build --
removing the consume-time visited check does not change any answer (the duplicate is
an extra node record, not a different `PathStep` sequence), so the default gate does
not catch it. The mechanical counters-off gate for a future change is a fixture whose
discovery count sits exactly at the 64-node inline budget with one duplicate link:
its `[mem]` row then reads 0.0 allocs/op when correct and 1.0 when mutated.

The C ABI addition is purely additive: no existing struct changed, so
`KIMIX_GDB_ABI_VERSION` stays 6 and a binder may treat a missing symbol as a
pre-edge-epoch image.

Coverage: `test_graph_db_traverse.cpp` (18 058 asserts; equivalence against a brute
force over the constraint x orientation x depth x endpoint matrix, the parallel-edge
choice, `oriented_links` / `edge_matches`, the facade and its handle path, plus the
three cases this cost work added: a 200-link visit with a parallel pair at index 130
-- inside a drained chunk, so the batch's first-match-wins is pinned rather than
assumed -- a duplicate neighbour inside one chunk with a rival, and a randomized
`has_path == !shortest_path.empty()` equivalence over 120 draws x 4 constraints x 3
orientations), `test_graph_db_c_api.cpp` (`cabi_shortest_path_edges`: two-phase, the
reroute a label filter causes, the orientation knob, the error surface),
`tests/unit/api/test_kimix_api.cpp` (the re-export exists) and the gated benchmark
`test_graph_db_bench_edges.cpp` (the `[bench]` / `[mem]` / `[ctr]` rows quoted above,
with a brute-force `PathStep` parity gate before any timing and no timing assertion
at all).

## 5e. Epoch 4 — first-class edges + the GDI layer

What changed, and what it cost:

- **Adjacency is per EDGE, not per neighbour**: `Vertex::_out_links` /
  `_in_links` hold `EdgeLink{edge uid, neighbour uid}` (32 B) instead of a 16 B guid,
  so a vertex's adjacency costs 2x the memory and the guid lists
  (`_depending_guids` / `_depended_guids`) are kept as MAINTAINED projections
  (updated in place on every link change) because `dependencies()` returns a const
  reference and is called once per visited vertex by every traversal. Rebuilding a
  projection lazily was rejected on that measurement.
- **`connect()` / `connect_bulk()` now go through the edge table** (`_create_edge_locked`),
  so the legacy contract (one directed edge per ordered pair, no self loops, the
  same `EdgeOutcome` vocabulary) and the GDI model cannot drift. `test_graph_db.cpp`
  passes UNMODIFIED as the regression gate.
- **A vertex record no longer carries the adjacency authoritatively**, so an
  edge-only change writes ONE edge record (`saved_edge_count`) and no vertex
  record at all.
- **One measured change to an existing suite**: `serialize_corrupt_record_is_skipped`
  now expects TWO unresolvable edges instead of one, because an edge record
  survives the loss of its endpoints' vertex records and is reported on its own
  (the v1 model could not even see the second one). The v1 record format itself is
  unchanged and still decodes.
- **The cost, measured in release on this host** (same 100k / 1M stores as
  epoch 3, so the numbers are comparable row by row):

  | Row | epoch 3 | epoch 4 | delta | why |
  |---|---|---|---|---|
  | structural 1T create+connect | 881 ns | 1327 ns | **+51 %** | an edge is now an OBJECT: one `EdgeTable` shard insert, one link pair, plus the guid projections. The old path wrote two guid vectors and two pointer sets. |
  | read 1T user op / find-only 1T | 3434 / 15.8 ns | 3197 / 17.6 ns | -7 % / +11 % (noise band) | the read path is unchanged: `dependencies()` still reads the maintained 16-byte projection. |
  | content_update 32T | 14.9 ns | 10.7 ns | -28 % (favourable noise) | level-2 path untouched. |
  | save 100k | 752 ms | 3236 ms | **+330 %** | two record families instead of one, and a v2 vertex record carries the key, the label/property tables AND the dependency projection (the compatibility promise documented in §5c). The 100k store is now 70.4 MB instead of ~? MB. |
  | load 100k | 161 ms | 623 ms | **+287 %** | same cause on the read side, plus the per-record property/label decode. |
  | incremental 1 % @100k | 68.5 ms | 100.5 ms | **+47 %** | same records, but now a save also sweeps the edge family's dirty set. |
  | save/load 1M | 7950 / 3058 ms | 26305 / 9155 ms | same shape | linear in the record size, i.e. no accidental superlinearity. |

  The honest conclusion: **the GDI data model costs roughly 3x on the bulk
  save/load paths and 1.5x on the structural write path**, and it costs nothing on
  reads. The two obvious levers for the next epoch, both requiring a format
  decision rather than a code change, are (a) drop the dependency projection from
  v2 records (the edge records are authoritative; the projection only earns its
  bytes for v1 migration and for a hand-edited store) and (b) a compact property
  encoding (varint payload lengths instead of 8-byte ones). Neither is done here
  because both change the on-disk format that the migration tests pin.


5i. ABI 7 — document-permission filtering: string arrays, ANY_IN, the edge-filtered BFS

The relation-graph permission use case (change plan `kimix_graph.md`, @CCShark
2026-10-28): every edge carries its own list of evidence-document ids, and a reader
only sees edges whose evidence intersects the set of documents they can read. Three
additions landed together under `KIMIX_GDB_ABI_VERSION 7`:

| addition | where | shape |
|---|---|---|
| string-array property kind | `PropertyKind::string_array = 7` (`KIMIX_GDB_KIND_STRING_ARRAY`), vertices and edges | ONE owned heap block per value, in the same layout on the C ABI and in the record codec: `[u32 element_count] { [u32 byte_len][UTF-8 bytes] }*`, little-endian, no padding. `count` is the exact element count (uint32). Whole-array equality (order-sensitive), equality-only comparison, declaration = container `array` + dtype `char_` — one `char_` declaration serves String and StringArray, distinguished by the container, exactly like Int64 vs Int64Array. NOTE: an array declaration is EXACT-length — a `(array, char_, 3)` docs property accepts exactly 3 elements, so a variable-length evidence list needs one declaration per length |
| ANY_IN comparison | `GdiOp::any_in = 6` (`KIMIX_GDB_OP_ANY_IN`), property conditions only | the condition VALUE is the set (string_array primary, int64_array bonus). Scalar actual: satisfied iff the scalar is in the set; array actual: iff ANY element is; missing property or empty set: false; other actual kinds: false. Add-time validation in `Schema::add_property_condition`: a non-array set value is `error_op_datatype_mismatch` (33), an empty set is legal (never matches), no size ceiling beyond the u32 range. The set index (sorted vector + binary search, built ONCE at condition-add time inside `ConstraintCondition::matches_value`) is what the hot paths (`matches_probe`, `matches`, `edge_matches`, both walks) probe — no per-edge scan of the raw set |
| edge-filtered BFS | `Graph::traverse_bfs_edges` + `Database::traverse_bfs_edges` + C ABI `kimix_gdb_traverse_bfs_edges` (`kimix_gdb_bfs_vertex` 40 B / `kimix_gdb_bfs_edge` 48 B) | the readable neighbourhood in ONE call: (1) discovered vertices in BFS order — `(vertex, depth, discovering edge)`; (2) EVERY constraint-satisfying edge with both endpoints discovered (a second sweep, structurally deduped by uid — parallel edges between one pair are separate records, a spanning tree would drop them); (3) a truncation flag. The second sweep costs O(V·degree), bounded by `max_vertices` (300 in the stated use) — recorded, not optimized |

`traverse_bfs_edges` semantics, inherited from the §5h walk: an edge failing the
constraint is never traversed (visited-check-then-filter); `items[0]` is the start at
depth 0 with a zeroed discovering edge; expansion stops once the vertex count reaches
`max_vertices` and `*out_truncated = 1` means "something readable was dropped" (an
exact fit is NOT truncated; a depth-bound stop is not flagged). The edges output is
orientation-independent as a set — records always carry the edge's own
`(origin, target)`. Two-phase like §5h: the probe call (`items == NULL && edges ==
NULL`) reports `*out_vertex_count / *out_edge_count / *out_truncated`; the fill
re-runs the identical walk into session staging (`session->bfs_staging`, the
`path_staging` precedent) and answers `error_buffer` (6) with the sizes still
reported when a capacity is short. Error surface mirrors §5h: `error_vertex` (50) /
`error_constraint` (7) / `error_argument` (4, bad depth or `max_vertices < 1`) /
`error_op` (32, a label ordering op is refused at constraint-creation time).

ABI-7 also WIDENED the property-surface count fields: `kimix_gdb_property.count`,
`kimix_gdb_property_record.count`, `kimix_gdb_property_type.reserved` and
`kimix_gdb_constraint_spec.reserved2` are uint32_t now (`kimix_gdb_property` grew
32 -> 40 B). The ON-DISK record codec is UNCHANGED — it already wrote count as a
4-byte little-endian u32, so a store written under ABI 6 loads byte-identically
under ABI 7 (no migration; the property/serialize suites pin the record header
bytes). No 65,535-element ceiling exists anywhere on this surface anymore; the only
limits are the u32 range and payload sanity checks. `traverse_bfs` (the old
vertex-filtered BFS) is untouched, and the two-phase protocol table of §5h applies
verbatim to the new call.

Caller-visible subtlety worth one line: the SAME constraint judges different
entities per entry point — `edges_of_vertex` and both edge-filtered walks gate the
EDGE's payload, while `neighbors_of_vertex` gates the NEIGHBOUR VERTEX (GDI §5.10).
A permission filter built as `docs ANY_IN readableSet` therefore needs vertices to
carry their own evidence docs for the vertex reads, and edges to carry theirs for
the edge reads; kimix never cross-checks the two.
