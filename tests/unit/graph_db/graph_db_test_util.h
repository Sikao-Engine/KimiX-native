// Shared fixture helpers for the graph_db test suites
// (test_graph_db.cpp, test_graph_db_serialize.cpp, test_graph_db_bench.cpp).
//
// Everything here is `inline` (the test targets are built with the unity rule,
// so several TUs may end up in one batch — no non-inline definitions allowed).
//
// Contents:
//   to_bytes(text)              — a little byte blob from a literal
//   blob_equal(vector, literal) — compare a byte vector against a literal
//   temp_dir_guard              — RAII temp directory (removed on scope exit)
//   small_dag()                 — the known 6-vertex DAG the graph tests use
//   build_dag(graph, count)     — a synthetic layered DAG for the later tests
//
// Serializer fixtures (used by test_graph_db_serialize.cpp):
//   make_record(...)            — a VertexRecord with a correct content hash
//   write_record(cold, record)  — encode + write one record (one transaction)
//   write_raw_value(cold, guid, bytes)
//                               — store arbitrary bytes under a "v/" key, i.e.
//                                 hand-craft a corrupt record or a cycle store
//   raw_snapshot + snapshot_store(cold)
//                               — the raw stored values keyed by guid, sorted, so
//                                 an incremental save can be proved to leave the
//                                 untouched records bit-identical
//   sorted_guid_text(guids)     — guid texts, sorted, for order-free comparison
//   adjacency_lines(graph, vertices)
//                               — one "guid|deps|dependents" line per vertex,
//                                 sorted, for exact adjacency comparison
//
// Note: Graph exposes no vertex iteration (the map is private), so every helper
// that needs the vertex set takes the list the caller already has — a source
// graph's builder output, or the guide set a hand-crafted store was built from.
//
// The known DAG (edges mean "A depends on B", i.e. A -> B):
//
//        a —> b —> d
//         \        ^
//          \-> c --/
//        e (isolated)      f —> d
//
// guid order is the vector's, so a test can name vertices positionally.
#pragma once

#include <graph_db/graph.h>
#include <graph_db/property.h>
#include <graph_db/status.h>
#include <graph_db/utils.h>
#include <graph_db/vertex.h>

#include <core/guid.h>
#include <core/string_scratch.h>
#include <core/stl/filesystem.h>
#include <core/stl/format.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ostream>
#include <random>
#include <system_error>
#include <utility>

// ---------------------------------------------------------------------------
// Printers for the scoped enums
// ---------------------------------------------------------------------------
// Boost.UT formats both operands of a failed `eq(...)`, which needs an
// operator<< in the enum's own namespace (ADL). Declaring them here — once, for
// every graph_db suite — keeps the assertions readable as `eq(status, ...)`
// instead of a cast soup. Test-only: the library never prints an enum.
namespace kimix::graph_db {

inline std::ostream &operator<<(std::ostream &os, GdiStatus status) {
    return os << status_name(status) << " (" << static_cast<int32_t>(status) << ")";
}

inline std::ostream &operator<<(std::ostream &os, GdiOp op) {
    return os << op_name(op) << " (" << static_cast<int>(op) << ")";
}

inline std::ostream &operator<<(std::ostream &os, PropertyKind kind) {
    return os << property_kind_name(kind) << " (" << static_cast<int>(kind) << ")";
}

inline std::ostream &operator<<(std::ostream &os, GdiDataType type) {
    return os << data_type_name(type) << " (" << static_cast<int>(type) << ")";
}

inline std::ostream &operator<<(std::ostream &os, GdiContainerType type) {
    return os << container_type_name(type) << " (" << static_cast<int>(type) << ")";
}

inline std::ostream &operator<<(std::ostream &os, GdiSizeType type) {
    return os << size_type_name(type) << " (" << static_cast<int>(type) << ")";
}

} // namespace kimix::graph_db

namespace kimix::graph_db::test {

// ---------------------------------------------------------------------------
// Byte helpers
// ---------------------------------------------------------------------------

// A byte blob holding the bytes of `text` (no NUL terminator).
inline kimix::vector<std::byte> to_bytes(kimix::string_view text) {
    kimix::vector<std::byte> out;
    out.reserve(text.size());
    for (char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

// True when `bytes` spells exactly `text`.
inline bool blob_equal(kimix::span<const std::byte> bytes,
                       kimix::string_view text) noexcept {
    if (bytes.size() != text.size()) {
        return false;
    }
    for (size_t i = 0; i < text.size(); ++i) {
        if (std::to_integer<char>(bytes[i]) != text[i]) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Temp directory guard
// ---------------------------------------------------------------------------

// Creates a unique directory under std::filesystem::temp_directory_path() and
// removes it (recursively) when the guard leaves scope. Move-only; a failed
// creation leaves path() empty and ok() false.
class temp_dir_guard {
public:
    explicit temp_dir_guard(kimix::string_view tag = "graph_db") {
        static std::atomic<uint64_t> counter{0};
        std::error_code ec;
        kimix::filesystem::path base = kimix::filesystem::temp_directory_path(ec);
        if (ec) {
            return;
        }
        auto stamp = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        kimix::StringScratch name;
        name << "kimix_" << kimix::string_view{tag.data(), tag.size()} << "_"
             << stamp << "_" << counter.fetch_add(1);
        kimix::filesystem::path target = base / kimix::filesystem::path(name.c_str());
        kimix::filesystem::create_directories(target, ec);
        if (ec) {
            return;
        }
        _path = target;
    }
    temp_dir_guard(temp_dir_guard const &) = delete;
    temp_dir_guard &operator=(temp_dir_guard const &) = delete;
    temp_dir_guard(temp_dir_guard &&other) noexcept : _path(std::move(other._path)) {
        other._path.clear();
    }
    temp_dir_guard &operator=(temp_dir_guard &&) = delete;
    ~temp_dir_guard() {
        if (_path.empty()) {
            return;
        }
        std::error_code ec;
        kimix::filesystem::remove_all(_path, ec);
    }

    [[nodiscard]] bool ok() const noexcept { return !_path.empty(); }
    [[nodiscard]] kimix::filesystem::path const &path() const noexcept { return _path; }

private:
    kimix::filesystem::path _path;
};

// A one-vertex start span: the ABI-7 single-start request written as the ABI-8
// multi-start argument, so a leg that pins a single-start answer reads exactly as
// it did before the bump. Points at the caller's object, which outlives the call.
inline kimix::span<const kimix::Guid> one_start(kimix::Guid const &start) noexcept {
    return kimix::span<const kimix::Guid>{&start, 1};
}

// ---------------------------------------------------------------------------
// The known small DAG
// ---------------------------------------------------------------------------

struct known_dag {
    kimix::vector<Vertex *> vertices; // a, b, c, d, e, f — see the header sketch
    Vertex *a{nullptr};
    Vertex *b{nullptr};
    Vertex *c{nullptr};
    Vertex *d{nullptr};
    Vertex *e{nullptr};
    Vertex *f{nullptr};
};

// Build the DAG described at the top of this file inside `graph` (which must be
// empty). Contents are "a".."f" so the tests can assert on them too.
inline known_dag small_dag(Graph &graph) {
    known_dag dag;
    dag.a = graph.create_vertex(to_bytes("a"));
    dag.b = graph.create_vertex(to_bytes("b"));
    dag.c = graph.create_vertex(to_bytes("c"));
    dag.d = graph.create_vertex(to_bytes("d"));
    dag.e = graph.create_vertex(to_bytes("e"));
    dag.f = graph.create_vertex(to_bytes("f"));
    dag.vertices = {dag.a, dag.b, dag.c, dag.d, dag.e, dag.f};

    graph.connect(dag.a->guid(), dag.b->guid());
    graph.connect(dag.a->guid(), dag.c->guid());
    graph.connect(dag.b->guid(), dag.d->guid());
    graph.connect(dag.c->guid(), dag.d->guid());
    graph.connect(dag.f->guid(), dag.d->guid());
    return dag;
}

// A synthetic layered DAG: `count` vertices, each depending on up to two
// vertices with a lower index (so the graph is acyclic by construction). Used
// by the later serialize/bench suites; deterministic for a fixed seed.
inline kimix::vector<Vertex *> build_layered_dag(Graph &graph, size_t count,
                                                 uint32_t seed = 12345u) {
    kimix::vector<Vertex *> out;
    out.reserve(count);
    std::mt19937 rng(seed);
    for (size_t i = 0; i < count; ++i) {
        out.push_back(graph.create_vertex(to_bytes(kimix::format("v{}", i))));
    }
    for (size_t i = 1; i < count; ++i) {
        std::uniform_int_distribution<size_t> dist(0, i - 1);
        graph.connect(out[i]->guid(), out[dist(rng)]->guid());
        if (i > 3 && (rng() % 3u) == 0u) {
            size_t other = dist(rng);
            if (other != i - 1) {
                graph.connect(out[i]->guid(), out[other]->guid());
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Serializer fixtures
// ---------------------------------------------------------------------------

// Order guids by their binary words, so every "sorted" helper below is stable
// and independent of the map/insertion order the graph happens to have.
inline bool guid_less(kimix::Guid const &a, kimix::Guid const &b) noexcept {
    auto const &lhs = a.to_binary();
    auto const &rhs = b.to_binary();
    if (lhs.data0 != rhs.data0) {
        return lhs.data0 < rhs.data0;
    }
    return lhs.data1 < rhs.data1;
}

// A record with a correct content hash and the given dependency lists. `content`
// is copied; the hash is computed from it, never passed in.
inline VertexRecord make_record(kimix::Guid const &guid,
                               kimix::span<const std::byte> content,
                               kimix::vector<kimix::Guid> depending = {},
                               kimix::vector<kimix::Guid> depended = {}) {
    VertexRecord record;
    record.guid = guid;
    record.content.assign(content.begin(), content.end());
    record.content_hash = md5_of(record.content);
    record.depending_guids = std::move(depending);
    record.depended_guids = std::move(depended);
    return record;
}

// Encode + write ONE record (a single LMDB transaction).
inline bool write_record(ColdStorage &cold, VertexRecord const &record) noexcept {
    kimix::vector<VertexRecord> batch;
    batch.push_back(record);
    return cold.save_records(std::move(batch));
}

// Store arbitrary bytes under `guid`'s "v/" key — i.e. hand-craft a corrupt
// record, or a record whose bytes are not what a save would produce.
inline bool write_raw_value(ColdStorage &cold, kimix::Guid const &guid,
                            kimix::span<const std::byte> bytes) noexcept {
    kimix::LMDBWriteCommand command;
    guid_key(guid, command.key);
    command.value.assign(bytes.begin(), bytes.end());
    kimix::vector<kimix::LMDBWriteCommand> batch;
    batch.push_back(std::move(command));
    return cold.save_batch(std::move(batch));
}

// One raw stored value of a "v/" key.
struct raw_store_entry {
    kimix::Guid guid;
    kimix::vector<std::byte> value;
};

// Every stored vertex record as raw bytes, sorted by guid: the snapshot the
// incremental test keeps in memory between two saves, so "the untouched record
// is bit-identical" can be compared byte for byte. Non-"v/" keys are ignored.
inline kimix::vector<raw_store_entry> snapshot_store(ColdStorage const &cold) {
    kimix::vector<raw_store_entry> out;
    kimix::vector<ColdRecord> all;
    if (!cold.load_all_records(all)) {
        return out;
    }
    out.reserve(all.size());
    for (auto &entry : all) {
        auto guid = guid_from_key(kimix::span<const std::byte>{entry.key.data(), entry.key.size()});
        if (!guid) {
            continue;
        }
        raw_store_entry item;
        item.guid = *guid;
        item.value = std::move(entry.value);
        out.push_back(std::move(item));
    }
    std::sort(out.begin(), out.end(), [](raw_store_entry const &a, raw_store_entry const &b) {
        return guid_less(a.guid, b.guid);
    });
    return out;
}

// The raw value stored for `guid` (empty vector when absent).
inline kimix::vector<std::byte> stored_value(ColdStorage const &cold, kimix::Guid const &guid) {
    kimix::vector<ColdRecord> all;
    if (!cold.load_all_records(all)) {
        return {};
    }
    auto const key = guid_key(guid);
    for (auto const &entry : all) {
        if (entry.key.size() != key.size()) {
            continue;
        }
        if (std::equal(entry.key.begin(), entry.key.end(), key.begin())) {
            return entry.value;
        }
    }
    return {};
}

// Guid texts of `vertices`, sorted — an order-free identity check.
// (Graph exposes no vertex iteration, so every helper here takes the vertex or
// guid list the caller already knows: a source graph's list, or the guid set a
// hand-crafted store was built from.)
inline kimix::vector<kimix::string> sorted_guid_text(kimix::vector<Vertex *> const &vertices) {
    kimix::vector<kimix::string> out;
    out.reserve(vertices.size());
    for (auto *vertex : vertices) {
        if (vertex != nullptr) {
            out.push_back(vertex->guid().to_string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

inline kimix::vector<kimix::string> sorted_guid_text(kimix::vector<kimix::Guid> const &guids) {
    kimix::vector<kimix::string> out;
    out.reserve(guids.size());
    for (auto const &guid : guids) {
        out.push_back(guid.to_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// One line per vertex: "<guid>|<sorted outgoing guids>|<sorted incoming guids>",
// lines sorted by guid. Two graphs with equal adjacency produce identical
// vectors, so a determinism check is an exact comparison of this snapshot.
inline kimix::vector<kimix::string> adjacency_lines(Graph const &graph,
                                                   kimix::vector<Vertex *> const &vertices) {
    kimix::vector<kimix::string> out;
    out.reserve(vertices.size());
    for (auto *vertex : vertices) {
        if (vertex == nullptr) {
            continue;
        }
        auto outgoing = sorted_guid_text(graph.dependencies(vertex->guid()));
        auto incoming = sorted_guid_text(graph.dependents(vertex->guid()));
        kimix::StringScratch line;
        line << vertex->guid().to_string() << "|";
        for (size_t i = 0; i < outgoing.size(); ++i) {
            line << (i == 0 ? "" : ",") << outgoing[i];
        }
        line << "|";
        for (size_t i = 0; i < incoming.size(); ++i) {
            line << (i == 0 ? "" : ",") << incoming[i];
        }
        out.push_back(kimix::string{line.string_view()});
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace kimix::graph_db::test