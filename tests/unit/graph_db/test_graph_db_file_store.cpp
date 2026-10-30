// Test for the two cold-store BACKENDS behind src/graph_db/cold_store.h: the LMDB
// ColdStorage of src/graph_db/utils.h and the second backend FileColdStore of
// src/graph_db/file_cold_store.h — an LMDB environment of its own at
// <dir>\graph.xdb, written through <core/lmdb.h>.
// This test covers:
// - THE PARITY MATRIX — every persistence scenario of test_graph_db_serialize.cpp
//   runs against BOTH backends and the outcomes are compared byte for byte (the
//   stored record bytes included; the "meta/root" record is excluded from the byte
//   comparison because it carries the store identity and the save timestamps, which
//   two stores are allowed to differ in):
//     * save/load round trip on an empty store, the 6-vertex DAG and a layered DAG
//     * incremental save writes only the dirty records; every untouched record stays
//       bit-identical and the two backends hold the same new bytes
//     * a full save after an incremental one (on the second backend that is the
//       replacing rebuild(), armed through set_rebuild_next_batch): the superseded
//       records are gone instead of being kept beside the new one
//     * the dual-stage load of a hand-crafted store holding a dependency cycle and a
//       dangling guid, with the cycle's edges materialized on both backends
//     * corrupt-record skip counts (garbage + truncated value under "v/" keys)
//     * the v1 ("GDB1") and v2 ("GDB2") record decode paths, including the edge
//       synthesized from a retired v1 dependency list
//     * the "meta/root" record read/write, byte for byte
//     * record removal — ColdStorage::remove_record/remove_keys against the second
//       backend's tombstone (an empty value), which is how a save spells "deleted"
//     * a copy_to() checkpoint that loads back identically (for the second backend
//       the checkpoint is a graph.xdb environment directory of its own)
//     * an immediate incremental save after a load writing nothing at all (the empty
//       batch leaves the store's data.mdb byte-identical)
// - THE SECOND BACKEND'S OWN CONTRACT (file_cold_store.h):
//     * open-failure degradation: a regular FILE where the environment directory
//       belongs, an empty path, a foreign data.mdb and a truncated one all leave the
//       store closed with every operation reporting false — and never abort, which is
//       why the backend validates the path before it hands it to LMDB
//     * the large-value path: six ~700 KiB records are stored and read back intact,
//       across a later save and a reopen of the store
//     * determinism: two full saves of the same graph produce identical record bytes,
//       and the on-disk layout really is an LMDB environment (graph.xdb is a
//       directory holding data.mdb + lock.mdb, and data.mdb carries LMDB's meta-page
//       magic — read from the bytes, not through the code under test)
//     * Database honors GraphOptions::store_backend: the same GDI script through
//       both backends persists and reloads the same state, and the second backend's
//       directory holds graph.xdb instead of an LMDB environment of its own
//
// EVERY fixture graph is built from DETERMINISTIC guids: a parity claim ("the two
// backends hold the same bytes") is only meaningful when the graphs they were saved
// from have the same guids, and Guid{true} is random.
//
// The fiber scheduler is declared and bound FIRST in main() (the serializer's
// fan-outs submit to it), per the fiber skill's root-main pattern. Assertions all run
// on the main thread.

#include "ut/ut.hpp"
#include "graph_db_test_util.h"

#include <core/kimix_core.h> // umbrella first: it fixes the winsock2.h order
#include <core/fiber.h>

#include <graph_db/binary_le.h>
#include <graph_db/cold_store.h>
#include <graph_db/database.h>
#include <graph_db/file_cold_store.h>
#include <graph_db/graph.h>
#include <graph_db/graph_db_options.h>
#include <graph_db/serialize.h>
#include <graph_db/status.h>
#include <graph_db/utils.h>
#include <graph_db/vertex.h>

#include <core/binary_file_stream.h>
#include <core/guid.h>
#include <core/lmdb.h>
#include <core/string_scratch.h>
#include <core/stl/filesystem.h>
#include <core/stl/format.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <system_error>
#include <utility>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using graph_db::ColdRecord;
using graph_db::ColdStorage;
using graph_db::ColdStore;
using graph_db::FileColdStore;
using graph_db::Graph;
using graph_db::GraphDeserializer;
using graph_db::GraphSerializeStats;
using graph_db::GraphSerializer;
using graph_db::MetaRecord;
using graph_db::Vertex;
using graph_db::VertexRecord;
namespace gtest = kimix::graph_db::test;

namespace tests_unit_graph_db_test_graph_db_file_store {

// ---------------------------------------------------------------------------
// Deterministic graph fixtures
// ---------------------------------------------------------------------------

// A guid that depends only on `index`, so two independently built graphs can carry
// the same guid bytes — the precondition of every byte-level parity assertion.
kimix::Guid fixed_guid(uint64_t index) noexcept {
    return kimix::Guid{
        kimix::Guid::GuidData{0x5eed000000000000ull + index, 0xbeef000000000000ull + index}};
}

// Add one vertex with a fixed guid and `content` bytes; nullptr when the graph
// refuses it (the caller's expect() reports that).
Vertex *add_fixed_vertex(Graph &graph, uint64_t index, kimix::string_view content) {
    kimix::unique_ptr<Vertex> vertex{new Vertex(fixed_guid(index), gtest::to_bytes(content))};
    Vertex *raw = vertex.get();
    if (!graph.add_vertex(std::move(vertex))) {
        return nullptr;
    }
    return raw;
}

// The known 6-vertex DAG of graph_db_test_util.h (a -> b, a -> c, b -> d, c -> d,
// f -> d, e isolated) with deterministic guids 1..6 and contents "a".."f".
struct fixed_dag {
    kimix::vector<Vertex *> vertices;
    Vertex *a{nullptr};
    Vertex *b{nullptr};
    Vertex *c{nullptr};
    Vertex *d{nullptr};
    Vertex *e{nullptr};
    Vertex *f{nullptr};
};

fixed_dag fixed_small_dag(Graph &graph) {
    fixed_dag dag;
    dag.a = add_fixed_vertex(graph, 1, "a");
    dag.b = add_fixed_vertex(graph, 2, "b");
    dag.c = add_fixed_vertex(graph, 3, "c");
    dag.d = add_fixed_vertex(graph, 4, "d");
    dag.e = add_fixed_vertex(graph, 5, "e");
    dag.f = add_fixed_vertex(graph, 6, "f");
    dag.vertices = {dag.a, dag.b, dag.c, dag.d, dag.e, dag.f};
    for (auto *vertex : dag.vertices) {
        if (vertex == nullptr) {
            return dag; // the caller asserts on the graph size
        }
    }
    graph.connect(dag.a->guid(), dag.b->guid());
    graph.connect(dag.a->guid(), dag.c->guid());
    graph.connect(dag.b->guid(), dag.d->guid());
    graph.connect(dag.c->guid(), dag.d->guid());
    graph.connect(dag.f->guid(), dag.d->guid());
    return dag;
}

// A synthetic layered DAG with deterministic guids (1000 + i): vertex i depends on
// i - 1 and, every third one, on i / 2 — acyclic by construction, so the graph is
// reproducible byte for byte.
kimix::vector<Vertex *> fixed_layered_dag(Graph &graph, size_t count) {
    kimix::vector<Vertex *> out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        out.push_back(add_fixed_vertex(graph, 1000 + i, kimix::format("v{}", i)));
    }
    for (size_t i = 1; i < count; ++i) {
        if (out[i] == nullptr || out[i - 1] == nullptr) {
            return out;
        }
        graph.connect(out[i]->guid(), out[i - 1]->guid());
        if (i > 2 && (i % 3) == 0 && out[i / 2] != nullptr) {
            graph.connect(out[i]->guid(), out[i / 2]->guid());
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Backend fixture: one store per backend, side by side
// ---------------------------------------------------------------------------

enum class backend_kind : uint8_t { lmdb, flat_file };

[[nodiscard]] kimix::string_view backend_name(backend_kind kind) {
    return kind == backend_kind::lmdb ? kimix::string_view{"lmdb"}
                                      : kimix::string_view{"flat_file"};
}

// Both backends over their own directory, so a scenario can be written ONCE and run
// twice. The seam handle (cold_store.h) is what the serializer takes, so the same
// scenario code drives either backend.
class backend_pair {
public:
    backend_pair(kimix::filesystem::path lmdb_dir, kimix::filesystem::path file_dir)
        : _lmdb_dir(std::move(lmdb_dir)), _file_dir(std::move(file_dir)) {
    }

    [[nodiscard]] bool open() {
        if (!_lmdb.open(_lmdb_dir)) {
            return false;
        }
        if (!_file.open(_file_dir)) {
            return false;
        }
        _is_open = true;
        return true;
    }

    [[nodiscard]] ColdStore seam(backend_kind kind) {
        return kind == backend_kind::lmdb ? ColdStore{_lmdb} : ColdStore{_file};
    }
    [[nodiscard]] ColdStorage &lmdb() { return _lmdb; }
    [[nodiscard]] FileColdStore &file() { return _file; }
    [[nodiscard]] kimix::filesystem::path dir(backend_kind kind) const {
        return kind == backend_kind::lmdb ? _lmdb_dir : _file_dir;
    }
    [[nodiscard]] bool is_open() const noexcept { return _is_open; }

    [[nodiscard]] static backend_kind side(size_t index) noexcept {
        return index == 0 ? backend_kind::lmdb : backend_kind::flat_file;
    }

private:
    kimix::filesystem::path _lmdb_dir;
    kimix::filesystem::path _file_dir;
    ColdStorage _lmdb;
    FileColdStore _file;
    bool _is_open{false};
};

// The second backend's store inside a store directory: <dir>\graph.xdb, an LMDB
// environment DIRECTORY of its own.
[[nodiscard]] kimix::filesystem::path store_file(kimix::filesystem::path const &dir) {
    return dir / kimix::filesystem::path{graph_db::kColdFileName};
}

// The store's data file — the environment's only payload.
[[nodiscard]] kimix::filesystem::path store_data_file(kimix::filesystem::path const &dir) {
    return store_file(dir) / kimix::filesystem::path{FileColdStore::kDataFileName};
}

// Size of a regular file; 0 for a directory or an absent path (every caller here
// asks about a data file, never about the environment directory).
[[nodiscard]] uintmax_t file_bytes(kimix::filesystem::path const &file) {
    std::error_code ec;
    if (!kimix::filesystem::is_regular_file(file, ec) || ec) {
        return 0;
    }
    uintmax_t const size = kimix::filesystem::file_size(file, ec);
    return ec ? 0 : size;
}

// Bytes of the second backend's data.mdb (0 when the store was never written).
[[nodiscard]] uintmax_t store_bytes(kimix::filesystem::path const &dir) {
    return file_bytes(store_data_file(dir));
}

// ---------------------------------------------------------------------------
// Byte helpers (both backends; the store-taking ones go through the seam)
// ---------------------------------------------------------------------------

[[nodiscard]] bool same_bytes(kimix::span<const std::byte> lhs,
                              kimix::span<const std::byte> rhs) noexcept {
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

[[nodiscard]] bool key_is(kimix::span<const std::byte> key, kimix::string_view expected) noexcept {
    return same_bytes(key, kimix::span<const std::byte>{
                               reinterpret_cast<std::byte const *>(expected.data()),
                               expected.size()});
}

[[nodiscard]] bool key_starts_with(kimix::span<const std::byte> key,
                                   kimix::string_view prefix) noexcept {
    return key.size() >= prefix.size() &&
           std::memcmp(key.data(), prefix.data(), prefix.size()) == 0;
}

[[nodiscard]] kimix::string hex_of(kimix::span<const std::byte> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    kimix::string out;
    out.reserve(bytes.size() * 2 + 1);
    for (auto byte : bytes) {
        auto const value = std::to_integer<uint32_t>(byte);
        out.push_back(digits[(value >> 4) & 0x0fu]);
        out.push_back(digits[value & 0x0fu]);
    }
    return out;
}

// One store key as bytes ("v/" + guid, "meta/root", "s/labels", …).
[[nodiscard]] kimix::vector<std::byte> key_bytes(kimix::string_view text) {
    auto const *const first = reinterpret_cast<std::byte const *>(text.data());
    return kimix::vector<std::byte>{first, first + text.size()};
}

// A "v/"+guid key as bytes (what ColdStorage::remove_keys takes).
[[nodiscard]] kimix::vector<std::byte> guid_key_bytes(kimix::Guid const &guid) {
    auto const key = graph_db::guid_key(guid);
    return kimix::vector<std::byte>{key.begin(), key.end()};
}

// One write command for a "v/" key.
[[nodiscard]] kimix::vector<kimix::LMDBWriteCommand>
one_command(kimix::Guid const &guid, kimix::span<const std::byte> value) {
    kimix::LMDBWriteCommand command;
    graph_db::guid_key(guid, command.key);
    command.value.assign(value.begin(), value.end());
    kimix::vector<kimix::LMDBWriteCommand> batch;
    batch.push_back(std::move(command));
    return batch;
}

// Encode + write ONE record (both backends have save_batch through the seam).
[[nodiscard]] bool write_record(ColdStore cold, VertexRecord const &record) {
    kimix::LMDBWriteCommand command;
    graph_db::guid_key(record.guid, command.key);
    graph_db::encode_vertex_record(record, command.value);
    kimix::vector<kimix::LMDBWriteCommand> batch;
    batch.push_back(std::move(command));
    return cold.save_batch(std::move(batch));
}

// Store arbitrary bytes under `guid`'s "v/" key: a hand-crafted corrupt record, a
// record whose embedded guid disagrees with its key, or a tombstone (empty value).
[[nodiscard]] bool write_raw_value(ColdStore cold, kimix::Guid const &guid,
                                   kimix::span<const std::byte> bytes) {
    return cold.save_batch(one_command(guid, bytes));
}

// Store arbitrary bytes under an arbitrary key (a hand-crafted "meta/root", …).
[[nodiscard]] bool write_raw_key(ColdStore cold, kimix::string_view key,
                                 kimix::span<const std::byte> bytes) {
    kimix::LMDBWriteCommand command;
    command.key = key_bytes(key);
    command.value.assign(bytes.begin(), bytes.end());
    kimix::vector<kimix::LMDBWriteCommand> batch;
    batch.push_back(std::move(command));
    return cold.save_batch(std::move(batch));
}

[[nodiscard]] bool write_meta(ColdStore cold, MetaRecord const &meta) {
    kimix::vector<std::byte> encoded;
    graph_db::encode_meta_record(meta, encoded);
    return write_raw_key(cold, graph_db::kMetaRootKey,
                         kimix::span<const std::byte>{encoded.data(), encoded.size()});
}

// ---------------------------------------------------------------------------
// Store snapshots
// ---------------------------------------------------------------------------

// One stored record as raw bytes.
struct raw_record {
    kimix::vector<std::byte> key;
    kimix::vector<std::byte> value;
};

[[nodiscard]] bool same_records(kimix::vector<raw_record> const &lhs,
                                kimix::vector<raw_record> const &rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (!same_bytes(lhs[i].key, rhs[i].key) || !same_bytes(lhs[i].value, rhs[i].value)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool raw_record_less(raw_record const &lhs, raw_record const &rhs) {
    return std::lexicographical_compare(lhs.key.begin(), lhs.key.end(), rhs.key.begin(),
                                        rhs.key.end());
}

// Every record the store holds, sorted by key bytes, with "meta/root" dropped: the
// meta carries the store identity and the two save timestamps, which two stores may
// legitimately differ in (amendment A6). Everything else — vertex, edge and schema
// records, tombstones included — is compared byte for byte.
[[nodiscard]] kimix::vector<raw_record> record_bytes(ColdStore cold) {
    kimix::vector<raw_record> out;
    kimix::vector<ColdRecord> scanned;
    if (!cold.load_all_records(scanned)) {
        return out;
    }
    out.reserve(scanned.size());
    for (auto &entry : scanned) {
        if (key_is(entry.key, graph_db::kMetaRootKey)) {
            continue;
        }
        raw_record record;
        record.key = std::move(entry.key);
        record.value = std::move(entry.value);
        out.push_back(std::move(record));
    }
    std::sort(out.begin(), out.end(), raw_record_less);
    return out;
}

// The raw stored value of one key, in store key bytes (empty when absent).
[[nodiscard]] kimix::vector<std::byte> stored_value(ColdStore cold,
                                                    kimix::span<const std::byte> key) {
    kimix::vector<ColdRecord> scanned;
    if (!cold.load_all_records(scanned)) {
        return {};
    }
    for (auto const &entry : scanned) {
        if (same_bytes(entry.key, key)) {
            return entry.value;
        }
    }
    return {};
}

// The raw stored value of a vertex record (empty when absent).
[[nodiscard]] kimix::vector<std::byte> stored_vertex_value(ColdStore cold,
                                                           kimix::Guid const &guid) {
    auto const key = graph_db::guid_key(guid);
    return stored_value(cold, kimix::span<const std::byte>{key.data(), key.size()});
}

// True when the store holds an "e/" record for the edge `origin` -> `target`. A v2
// vertex record projects no adjacency (utils.h, VertexRecordV2), so this — not the
// vertex record — is where "the edge was saved" is observable, on either backend.
[[nodiscard]] bool has_edge_record(ColdStore cold, kimix::Guid const &origin,
                                   kimix::Guid const &target) {
    kimix::vector<ColdRecord> scanned;
    if (!cold.load_all_records(scanned)) {
        return false;
    }
    for (auto const &entry : scanned) {
        auto const id = graph_db::edge_from_key(
            kimix::span<const std::byte>{entry.key.data(), entry.key.size()});
        if (!id || entry.value.empty()) {
            continue;
        }
        graph_db::EdgeRecord record;
        if (!graph_db::decode_edge_record(
                kimix::span<const std::byte>{entry.value.data(), entry.value.size()}, record)) {
            continue;
        }
        if (record.origin == origin && record.target == target) {
            return true;
        }
    }
    return false;
}

// "v/" keys with a non-empty value: the live vertex records of the store (a tombstone
// is stored but not live).
[[nodiscard]] size_t live_vertex_records(ColdStore cold) {
    kimix::vector<ColdRecord> scanned;
    if (!cold.load_all_records(scanned)) {
        return 0;
    }
    size_t count = 0;
    for (auto const &entry : scanned) {
        if (key_starts_with(entry.key, graph_db::kVertexKeyPrefix) && !entry.value.empty()) {
            ++count;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// The parity currency: what ONE backend produced for a scenario
// ---------------------------------------------------------------------------

struct scenario_outcome {
    // save side
    size_t saved_count{0};
    size_t saved_edge_count{0};
    size_t deleted_vertex_count{0};
    size_t bytes_written{0};
    size_t stored_records{0}; // every record the store holds (meta excluded)
    size_t live_records{0};   // "v/" keys with a non-empty value
    // load side
    size_t loaded_count{0};
    size_t skipped_count{0};
    size_t skipped_edge_count{0};
    size_t loaded_edge_count{0};
    size_t restored_label_count{0};
    size_t restored_property_count{0};
    uint32_t format_version{0};
    bool migrated_from_v1{false};
    // what the load materialized
    size_t vertex_count{0};
    kimix::vector<kimix::string> vertex_lines; // "guid|content-hex|hash", sorted
    kimix::vector<kimix::string> adjacency;    // one line per vertex, sorted
    // the store's own bytes (meta excluded)
    kimix::vector<raw_record> records;
};

void fill_save_stats(GraphSerializeStats const &stats, scenario_outcome &out) {
    out.saved_count = stats.saved_count;
    out.saved_edge_count = stats.saved_edge_count;
    out.deleted_vertex_count = stats.deleted_vertex_count;
    out.bytes_written = stats.bytes_written;
}

void fill_load_stats(GraphSerializeStats const &stats, scenario_outcome &out) {
    out.loaded_count = stats.loaded_count;
    out.skipped_count = stats.skipped_count;
    out.skipped_edge_count = stats.skipped_edge_count;
    out.loaded_edge_count = stats.loaded_edge_count;
    out.restored_label_count = stats.restored_label_count;
    out.restored_property_count = stats.restored_property_type_count;
    out.format_version = stats.format_version;
    out.migrated_from_v1 = stats.migrated_from_v1;
}

void fill_store_snapshot(ColdStore cold, scenario_outcome &out) {
    out.records = record_bytes(cold);
    out.stored_records = out.records.size();
    out.live_records = live_vertex_records(cold);
}

// A canonical, order-free description of a graph: one line per vertex
// (guid | content bytes | content hash) plus the sorted adjacency lines. It takes no
// vertex list from the caller, so it also describes a graph the test only knows
// through the store — and it covers the two properties a load must reproduce.
void fill_graph_snapshot(Graph const &graph, scenario_outcome &out) {
    kimix::vector<kimix::Guid> guids;
    kimix::vector<kimix::string> lines;
    graph.for_each_vertex([&guids, &lines](Vertex const &vertex) {
        guids.push_back(vertex.guid());
        auto const content = vertex.content_copy();
        kimix::string line = vertex.guid().to_string();
        line += '|';
        line += hex_of(kimix::span<const std::byte>{content.data(), content.size()});
        line += '|';
        line += vertex.content_hash().to_string();
        lines.push_back(std::move(line));
        return true;
    });
    std::sort(lines.begin(), lines.end());
    kimix::vector<Vertex *> resolved;
    resolved.reserve(guids.size());
    for (auto const &guid : guids) {
        resolved.push_back(graph.find(guid));
    }
    out.vertex_count = guids.size();
    out.vertex_lines = std::move(lines);
    out.adjacency = gtest::adjacency_lines(graph, resolved);
}

// Field-by-field comparison of two backends' outcomes. An EMPTY result means they are
// identical; otherwise the message names every difference, so a parity failure says
// what diverged instead of "not equal".
//
// `tombstone_count` is deliberately NOT compared here: a key that was removed has no
// record at all in LMDB, while the log-structured file backend can only store an empty
// value under it (file_cold_store.h A4). The LIVE state — what a caller observes — is
// compared, and the removal scenario asserts the mechanism difference explicitly.
[[nodiscard]] kimix::string outcome_diff(scenario_outcome const &lmdb,
                                         scenario_outcome const &file) {
    kimix::StringScratch diff;
    auto note = [&diff](kimix::string_view field, size_t lhs, size_t rhs) {
        if (lhs != rhs) {
            diff << field << ": lmdb=" << lhs << " flat_file=" << rhs << "; ";
        }
    };
    note("saved_count", lmdb.saved_count, file.saved_count);
    note("saved_edge_count", lmdb.saved_edge_count, file.saved_edge_count);
    note("deleted_vertex_count", lmdb.deleted_vertex_count, file.deleted_vertex_count);
    note("bytes_written", lmdb.bytes_written, file.bytes_written);
    note("stored_records", lmdb.stored_records, file.stored_records);
    note("live_records", lmdb.live_records, file.live_records);
    note("loaded_count", lmdb.loaded_count, file.loaded_count);
    note("skipped_count", lmdb.skipped_count, file.skipped_count);
    note("skipped_edge_count", lmdb.skipped_edge_count, file.skipped_edge_count);
    note("loaded_edge_count", lmdb.loaded_edge_count, file.loaded_edge_count);
    note("restored_label_count", lmdb.restored_label_count, file.restored_label_count);
    note("restored_property_count", lmdb.restored_property_count, file.restored_property_count);
    note("vertex_count", lmdb.vertex_count, file.vertex_count);
    note("format_version", static_cast<size_t>(lmdb.format_version),
         static_cast<size_t>(file.format_version));
    if (lmdb.migrated_from_v1 != file.migrated_from_v1) {
        diff << "migrated_from_v1 differs; ";
    }
    if (lmdb.vertex_lines != file.vertex_lines) {
        diff << "the loaded vertices (content + hash) differ; ";
    }
    if (lmdb.adjacency != file.adjacency) {
        diff << "the loaded adjacency differs; ";
    }
    if (!same_records(lmdb.records, file.records)) {
        diff << "the stored record bytes differ; ";
    }
    return kimix::string{diff.string_view()};
}

// ---------------------------------------------------------------------------
// The second backend's on-disk layout, read from the BYTES
// ---------------------------------------------------------------------------

[[nodiscard]] kimix::vector<std::byte> read_file_bytes(kimix::filesystem::path const &file) {
    kimix::vector<std::byte> out;
    kimix::BinaryFileStream stream{kimix::to_string(file)};
    if (!stream) {
        return out;
    }
    kimix::vector<kimix::byte> const raw = stream.read_all();
    out.reserve(raw.size());
    for (auto byte : raw) {
        out.push_back(static_cast<std::byte>(byte));
    }
    return out;
}

// MDB_page_header size on this host: pgno_t + txnid_t (LMDB 1.0 writes the txnid into
// every page header) + mh_pad + mh_flags + mh_lower/mh_upper. MDB_meta follows it, so
// mm_magic/mm_version sit right behind that header.
constexpr size_t kMetaPageHeaderSize = sizeof(uintptr_t) + 16;
// MDB_MAGIC: the stamp that identifies an LMDB data file.
constexpr uint32_t kLmdbMagic = 0xBEEFC0DEu;
// MDB_DATA_VERSION of the vendored LMDB 1.x on-disk format (0.9 wrote 1).
constexpr uint32_t kLmdbDataVersion = 3u;

// <dir>\graph.xdb as file_cold_store.h documents it: an LMDB environment DIRECTORY
// holding the database in data.mdb (page 0 is a meta page) and LMDB's reader/writer
// table in lock.mdb. Read from the bytes themselves, never through FileColdStore, so
// these assertions are not a tautology.
struct env_layout {
    bool is_directory{false};
    bool has_data{false};
    bool has_lock{false};
    uint32_t meta_magic{0};
    uint32_t meta_version{0};
    uint64_t data_bytes{0};
};

[[nodiscard]] env_layout read_store_env(kimix::filesystem::path const &dir) {
    env_layout layout;
    std::error_code ec;
    kimix::filesystem::path const env = store_file(dir);
    layout.is_directory = kimix::filesystem::is_directory(env, ec) && !ec;
    if (!layout.is_directory) {
        return layout;
    }
    kimix::filesystem::path const data = store_data_file(dir);
    layout.has_data = kimix::filesystem::is_regular_file(data, ec) && !ec;
    layout.has_lock = kimix::filesystem::is_regular_file(
                          env / kimix::filesystem::path{FileColdStore::kLockFileName}, ec) &&
                      !ec;
    if (!layout.has_data) {
        return layout;
    }
    layout.data_bytes = static_cast<uint64_t>(file_bytes(data));
    kimix::vector<std::byte> const bytes = read_file_bytes(data);
    if (bytes.size() >= kMetaPageHeaderSize + 8) {
        layout.meta_magic = graph_db::detail::read_u32(bytes.data() + kMetaPageHeaderSize);
        layout.meta_version = graph_db::detail::read_u32(bytes.data() + kMetaPageHeaderSize + 4);
    }
    return layout;
}

[[nodiscard]] size_t count_keys(kimix::vector<raw_record> const &records,
                                kimix::string_view prefix) {
    size_t count = 0;
    for (auto const &record : records) {
        if (key_starts_with(record.key, prefix)) {
            ++count;
        }
    }
    return count;
}

} // namespace tests_unit_graph_db_test_graph_db_file_store
using namespace tests_unit_graph_db_test_graph_db_file_store;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    // FIRST, before any suite can run: bind a scheduler for the whole process. The
    // serializer's fan-outs submit to it; declared here so it outlives every suite
    // Boost.UT runs from the _test destructors at the end of main().
    kimix::fiber::scheduler sched{4u};

    "file_store_pair_precondition"_test = [] {
        // The precondition every parity test below relies on: both backends open and the
        // seam reports the same directory, so a scenario really does run twice.
        gtest::temp_dir_guard lmdb_dir;
        gtest::temp_dir_guard file_dir;
        expect(lmdb_dir.ok() && file_dir.ok()) << "temp directories must be creatable";
        if (!lmdb_dir.ok() || !file_dir.ok()) {
            return;
        }
        backend_pair stores{lmdb_dir.path(), file_dir.path()};
        expect(stores.open()) << "both backends must open";
        if (!stores.is_open()) {
            return;
        }
        for (size_t side = 0; side < 2; ++side) {
            backend_kind const kind = backend_pair::side(side);
            ColdStore const cold = stores.seam(kind);
            expect(cold.valid()) << "the seam forwards valid(): " << backend_name(kind);
            expect(cold.path() == stores.dir(kind))
                << "both backends speak DIRECTORIES: " << backend_name(kind);
            kimix::vector<ColdRecord> records;
            expect(cold.load_all_records(records)) << "an empty store scans to nothing";
            expect(records.empty());
            MetaRecord meta;
            expect(!cold.read_meta(meta)) << "a fresh store carries no meta record (v1)";
        }
    };

    "file_store_parity_full_round_trip"_test = [] {
        // Save/load round trip on the three shapes the serialize suite uses: an empty
        // store, the 6-vertex DAG and a 48-vertex layered DAG. The SAME source graph is
        // saved into both backends, so the stored bytes must be identical.
        enum class shape : uint8_t { empty, small_dag, layered };
        struct shape_case {
            shape which;
            size_t expected_vertices;
        };
        shape_case const cases[] = {shape_case{shape::empty, 0},
                                    shape_case{shape::small_dag, 6},
                                    shape_case{shape::layered, 48}};
        for (shape_case const test_case : cases) {
            gtest::temp_dir_guard lmdb_dir;
            gtest::temp_dir_guard file_dir;
            expect(lmdb_dir.ok() && file_dir.ok()) << "temp directories must be creatable";
            if (!lmdb_dir.ok() || !file_dir.ok()) {
                return;
            }

            Graph source;
            if (test_case.which == shape::small_dag) {
                fixed_small_dag(source);
            } else if (test_case.which == shape::layered) {
                fixed_layered_dag(source, 48);
            }
            expect(eq(source.size(), test_case.expected_vertices));

            scenario_outcome source_outcome;
            fill_graph_snapshot(source, source_outcome);

            backend_pair stores{lmdb_dir.path(), file_dir.path()};
            expect(stores.open()) << "both backends must open";
            if (!stores.is_open()) {
                return;
            }

            scenario_outcome outcomes[2];
            for (size_t side = 0; side < 2; ++side) {
                backend_kind const kind = backend_pair::side(side);
                ColdStore const cold = stores.seam(kind);
                GraphSerializer serializer{cold};
                GraphSerializeStats const saved = serializer.save(source);
                fill_save_stats(saved, outcomes[side]);
                fill_store_snapshot(cold, outcomes[side]);

                Graph restored;
                GraphDeserializer deserializer;
                GraphSerializeStats const loaded = deserializer.load(cold, restored);
                fill_load_stats(loaded, outcomes[side]);
                fill_graph_snapshot(restored, outcomes[side]);

                expect(eq(saved.saved_count, test_case.expected_vertices))
                    << "a full save writes every vertex: " << backend_name(kind);
                expect(eq(loaded.loaded_count, test_case.expected_vertices));
                expect(eq(loaded.skipped_count, size_t{0}));
                expect(eq(loaded.skipped_edge_count, size_t{0}))
                    << "a self-consistent store resolves every edge: " << backend_name(kind);
                expect(eq(outcomes[side].live_records, test_case.expected_vertices));
                // The load reproduces the source graph exactly — content, hashes and both
                // adjacency directions.
                expect(outcomes[side].vertex_lines == source_outcome.vertex_lines)
                    << "the loaded vertices equal the source's: " << backend_name(kind);
                expect(outcomes[side].adjacency == source_outcome.adjacency)
                    << "the loaded adjacency equals the source's: " << backend_name(kind);
            }

            kimix::string const diff = outcome_diff(outcomes[0], outcomes[1]);
            expect(diff.empty()) << "the two backends must agree byte for byte: " << diff;
            if (test_case.expected_vertices == 0) {
                expect(eq(outcomes[0].bytes_written, size_t{0}));
                expect(eq(outcomes[1].bytes_written, size_t{0}));
                expect(eq(outcomes[0].live_records, size_t{0}));
                expect(eq(outcomes[1].live_records, size_t{0}));
            }
        }
    };

    "file_store_parity_incremental_writes_only_dirty"_test = [] {
        // An incremental save writes exactly the dirty records: every untouched record
        // stays bit-identical (proved against a "before" snapshot per backend) and the
        // two backends hold the same new bytes.
        gtest::temp_dir_guard lmdb_dir;
        gtest::temp_dir_guard file_dir;
        expect(lmdb_dir.ok() && file_dir.ok()) << "temp directories must be creatable";
        if (!lmdb_dir.ok() || !file_dir.ok()) {
            return;
        }

        Graph graph;
        kimix::vector<Vertex *> vertices = fixed_layered_dag(graph, 24);
        expect(eq(graph.size(), size_t{24}));

        backend_pair stores{lmdb_dir.path(), file_dir.path()};
        expect(stores.open()) << "both backends must open";
        if (!stores.is_open()) {
            return;
        }

        // FIRST ROUND: a full save on both backends, then each store's byte image.
        kimix::vector<raw_record> before[2];
        for (size_t side = 0; side < 2; ++side) {
            ColdStore const cold = stores.seam(backend_pair::side(side));
            GraphSerializer serializer{cold};
            expect(eq(serializer.save(graph).saved_count, size_t{24}))
                << "the first save writes the whole graph";
            before[side] = record_bytes(cold);
            expect(eq(count_keys(before[side], graph_db::kVertexKeyPrefix), size_t{24}))
                << "one vertex record per vertex";
            expect(eq(count_keys(before[side], graph_db::kEdgeKeyPrefix), size_t{30}))
                << "one edge record per edge";
        }
        expect(same_records(before[0], before[1]))
            << "both backends hold the same first-save bytes";

        // One content update, one new vertex and one new edge — applied ONCE, to the
        // graph both backends save.
        Vertex *changed = vertices[7];
        Vertex *edge_source = vertices[3];
        Vertex *edge_target = vertices[19];
        expect(changed != nullptr && edge_source != nullptr && edge_target != nullptr);
        if (changed == nullptr || edge_source == nullptr || edge_target == nullptr) {
            return;
        }
        expect(graph.update_content(changed->guid(), gtest::to_bytes("changed-content")));
        Vertex *added = add_fixed_vertex(graph, 9000, "added-later");
        expect(added != nullptr) << "the extra vertex must be adopted";
        if (added == nullptr) {
            return;
        }
        expect(graph.connect(added->guid(), edge_target->guid()));
        expect(graph.connect(edge_source->guid(), added->guid()));
        // A structural write dirties the endpoints whose adjacency it changed (the
        // ABI-8 feedback fix), so no manual mark_dirty() is needed: changed (the
        // content update), added (fresh) and both connect endpoints are dirty.
        expect(eq(graph.dirty_vertices().size(), size_t{4}))
            << "changed, added and both connect endpoints are dirty before the second save";
        // The two new edges are dirty records of their own, and a save clears the flag of
        // every record it wrote — so they are re-marked for the second backend below.
        kimix::vector<graph_db::EdgeRecord> const new_edges = graph.dirty_edges();
        expect(eq(new_edges.size(), size_t{2})) << "the two new edges are dirty records";

        // SECOND ROUND: the incremental save. It clears the dirty flags of what it wrote,
        // so the four vertices are marked dirty again for the second backend — the
        // records that produces are identical either way.
        scenario_outcome outcomes[2];
        for (size_t side = 0; side < 2; ++side) {
            if (side != 0) {
                changed->mark_dirty();
                edge_source->mark_dirty();
                edge_target->mark_dirty();
                added->mark_dirty();
                for (auto const &edge : new_edges) {
                    graph_db::Edge *raw = graph.edges().find(edge.id);
                    if (raw != nullptr) {
                        raw->mark_dirty();
                    }
                }
            }
            backend_kind const kind = backend_pair::side(side);
            ColdStore const cold = stores.seam(kind);
            GraphSerializer serializer{cold};
            GraphSerializeStats const stats = serializer.save_incremental(graph);
            fill_save_stats(stats, outcomes[side]);
            expect(eq(stats.saved_count, size_t{4}))
                << "an incremental save writes exactly the dirty vertices: "
                << backend_name(kind);
            expect(eq(stats.skipped_count, size_t{0}));
            fill_store_snapshot(cold, outcomes[side]);
            expect(eq(outcomes[side].live_records, size_t{25}))
                << "the new vertex got a record too";

            // Every record that was NOT one of the dirty vertices is still bit-identical
            // to the first-save snapshot.
            auto const after = record_bytes(cold);
            size_t compared = 0;
            for (auto const &entry : before[side]) {
                bool touched = false;
                for (auto const *vertex : {changed, edge_source, edge_target}) {
                    auto const key = graph_db::guid_key(vertex->guid());
                    if (same_bytes(entry.key,
                                   kimix::span<const std::byte>{key.data(), key.size()})) {
                        touched = true;
                    }
                }
                if (touched) {
                    continue;
                }
                auto const found = std::find_if(
                    after.begin(), after.end(), [&entry](raw_record const &candidate) {
                        return same_bytes(candidate.key, entry.key);
                    });
                expect(found != after.end()) << "every untouched record still exists";
                if (found == after.end()) {
                    continue;
                }
                expect(same_bytes(found->value, entry.value))
                    << "an untouched record is bit-identical after an incremental save";
                ++compared;
            }
            expect(eq(compared, size_t{51}))
                << "every record except the three touched vertices is untouched";

            // The rewritten records really changed and carry the new data.
            VertexRecord decoded;
            expect(graph_db::decode_vertex_record(stored_vertex_value(cold, changed->guid()),
                                                  decoded));
            expect(gtest::blob_equal(decoded.content, "changed-content"))
                << "the rewritten record holds the new content";
            expect(decoded.content_hash == graph_db::md5_of(decoded.content))
                << "the stored hash matches the stored content";
            VertexRecord added_record;
            expect(graph_db::decode_vertex_record(stored_vertex_value(cold, added->guid()),
                                                  added_record));
            expect(gtest::blob_equal(added_record.content, "added-later"));
            // A v2 record is written with both legacy projection lists EMPTY: for a
            // v2 store the "e/" records are the authority (utils.h, VertexRecordV2),
            // so "the new outgoing edge was saved" means the save wrote an edge
            // record for it — not that the vertex record lists the guid. The two
            // backends must agree on that, hence the assertion is inside the loop.
            expect(added_record.depending_guids.empty())
                << "a v2 vertex record carries no adjacency projection: "
                << backend_name(kind);
            expect(added_record.depended_guids.empty()) << backend_name(kind);
            expect(has_edge_record(cold, added->guid(), edge_target->guid()))
                << "the new outgoing edge is a separate e/ record the save writes: "
                << backend_name(kind);
        }

        kimix::string const diff = outcome_diff(outcomes[0], outcomes[1]);
        expect(diff.empty()) << "the two backends must agree byte for byte: " << diff;
    };

    "file_store_parity_full_save_after_incremental"_test = [] {
        // A full save after an incremental one replaces everything. On the second backend
        // that is rebuild(), armed through set_rebuild_next_batch — the handoff a DIRECT
        // GraphSerializer::save() does not do by itself.
        gtest::temp_dir_guard lmdb_dir;
        gtest::temp_dir_guard file_dir;
        expect(lmdb_dir.ok() && file_dir.ok()) << "temp directories must be creatable";
        if (!lmdb_dir.ok() || !file_dir.ok()) {
            return;
        }

        Graph graph;
        kimix::vector<Vertex *> vertices = fixed_layered_dag(graph, 24);
        expect(eq(graph.size(), size_t{24}));
        if (graph.size() != 24) {
            return;
        }

        backend_pair stores{lmdb_dir.path(), file_dir.path()};
        expect(stores.open()) << "both backends must open";
        if (!stores.is_open()) {
            return;
        }

        scenario_outcome outcomes[2];
        for (size_t side = 0; side < 2; ++side) {
            backend_kind const kind = backend_pair::side(side);
            ColdStore const cold = stores.seam(kind);
            GraphSerializer serializer{cold};
            expect(eq(serializer.save(graph).saved_count, size_t{24}));

            // One dirty vertex, written incrementally: on both backends that is ONE
            // transaction that replaces that one record in place (the store keeps exactly
            // one record per key, so nothing of the earlier round survives beside it).
            expect(graph.update_content(vertices[5]->guid(), gtest::to_bytes("v5-again")));
            if (side != 0) {
                vertices[5]->mark_dirty();
            }
            size_t const records_before_incremental = record_bytes(cold).size();
            expect(eq(serializer.save_incremental(graph).saved_count, size_t{1}));
            expect(eq(record_bytes(cold).size(), records_before_incremental))
                << "the incremental save REPLACED a record instead of adding one: "
                << backend_name(kind);

            // FULL SAVE. On the second backend the handoff arms the rebuild; a plain
            // save_batch() would only put the dirty records and leave the rest untouched.
            if (kind == backend_kind::flat_file) {
                stores.file().set_rebuild_next_batch(true);
            }
            GraphSerializeStats const full = serializer.save(graph);
            fill_save_stats(full, outcomes[side]);
            expect(eq(full.saved_count, size_t{24})) << "a full save writes every vertex";
            fill_store_snapshot(cold, outcomes[side]);
            expect(eq(outcomes[side].live_records, size_t{24}))
                << "the rebuild drops the superseded records instead of keeping both";
            // A rebuild is a complete snapshot: one record per vertex, none left over.
            expect(eq(count_keys(outcomes[side].records, graph_db::kVertexKeyPrefix), size_t{24}))
                << "the rebuilt store holds exactly one record per vertex";
            if (kind == backend_kind::flat_file) {
                env_layout const rebuilt = read_store_env(stores.dir(kind));
                expect(rebuilt.is_directory && rebuilt.has_data)
                    << "the store is an LMDB environment directory";
            }

            Graph restored;
            GraphDeserializer deserializer;
            fill_load_stats(deserializer.load(cold, restored), outcomes[side]);
            fill_graph_snapshot(restored, outcomes[side]);
            expect(eq(outcomes[side].loaded_count, size_t{24}));
            expect(eq(outcomes[side].skipped_edge_count, size_t{0}));
        }

        kimix::string const diff = outcome_diff(outcomes[0], outcomes[1]);
        expect(diff.empty()) << "the two backends must agree byte for byte: " << diff;
        // The full save really was a full save: the rewritten record carries the new
        // content on both backends.
        for (size_t side = 0; side < 2; ++side) {
            VertexRecord decoded;
            expect(graph_db::decode_vertex_record(
                stored_vertex_value(stores.seam(backend_pair::side(side)), vertices[5]->guid()),
                decoded));
            expect(gtest::blob_equal(decoded.content, "v5-again"));
        }
    };

    "file_store_parity_dual_stage_cycle_and_dangling"_test = [] {
        // A store no Graph::connect could have produced: A <-> B is a dependency cycle and
        // C depends on a guid that has no record at all. The dual-stage load must
        // materialize all three, keep both cycle edges real and count exactly one
        // unresolvable edge — on both backends.
        gtest::temp_dir_guard lmdb_dir;
        gtest::temp_dir_guard file_dir;
        expect(lmdb_dir.ok() && file_dir.ok()) << "temp directories must be creatable";
        if (!lmdb_dir.ok() || !file_dir.ok()) {
            return;
        }

        kimix::Guid const a = fixed_guid(11);
        kimix::Guid const b = fixed_guid(12);
        kimix::Guid const c = fixed_guid(13);
        kimix::Guid const missing = fixed_guid(14);

        backend_pair stores{lmdb_dir.path(), file_dir.path()};
        expect(stores.open()) << "both backends must open";
        if (!stores.is_open()) {
            return;
        }

        scenario_outcome outcomes[2];
        for (size_t side = 0; side < 2; ++side) {
            backend_kind const kind = backend_pair::side(side);
            ColdStore const cold = stores.seam(kind);
            expect(write_record(cold, gtest::make_record(a, gtest::to_bytes("A"), {b}, {})));
            expect(write_record(cold, gtest::make_record(b, gtest::to_bytes("B"), {a}, {})));
            expect(write_record(cold,
                                gtest::make_record(c, gtest::to_bytes("C"), {missing, a}, {})));
            expect(eq(live_vertex_records(cold), size_t{3}));

            Graph restored;
            GraphDeserializer deserializer;
            GraphSerializeStats const stats = deserializer.load(cold, restored);
            fill_load_stats(stats, outcomes[side]);
            fill_graph_snapshot(restored, outcomes[side]);

            expect(eq(stats.loaded_count, size_t{3}))
                << "a dependency cycle or a dangling guid never costs a vertex";
            expect(eq(stats.skipped_count, size_t{0})) << "every record itself was valid";
            expect(eq(stats.skipped_edge_count, size_t{1}))
                << "exactly the C -> missing edge is unresolvable";
            expect(!restored.contains(missing)) << "the dangling guid never materializes";
            expect(restored.has_path(a, b) && restored.has_path(b, a))
                << "a cycle is a valid dependency structure";
            expect(eq(restored.dependencies(c).size(), size_t{1}));
            expect(eq(restored.dependents(a).size(), size_t{2}))
                << "a is depended on by both the cycle edge and C";
            Vertex *restored_a = restored.find(a);
            expect(restored_a != nullptr);
            if (restored_a != nullptr) {
                expect(gtest::blob_equal(restored_a->content_copy(), "A"));
            }

            // A satisfied load leaves the graph clean and immediately re-savable.
            expect(restored.dirty_vertices().empty());
            GraphSerializer serializer{cold};
            expect(eq(serializer.save_incremental(restored).saved_count, size_t{0}));
        }

        kimix::string const diff = outcome_diff(outcomes[0], outcomes[1]);
        expect(diff.empty()) << "the two backends must agree byte for byte: " << diff;
    };

    "file_store_parity_corrupt_record_skip_counts"_test = [] {
        // Garbage and a truncated value under valid "v/" keys: the vertices are skipped,
        // the load completes, the good vertices are intact and the edges that lost an
        // endpoint are counted — identically on both backends.
        gtest::temp_dir_guard lmdb_dir;
        gtest::temp_dir_guard file_dir;
        expect(lmdb_dir.ok() && file_dir.ok()) << "temp directories must be creatable";
        if (!lmdb_dir.ok() || !file_dir.ok()) {
            return;
        }

        Graph source;
        fixed_dag dag = fixed_small_dag(source);
        expect(dag.c != nullptr);
        if (dag.c == nullptr) {
            return;
        }
        kimix::Guid const junk = fixed_guid(77);

        backend_pair stores{lmdb_dir.path(), file_dir.path()};
        expect(stores.open()) << "both backends must open";
        if (!stores.is_open()) {
            return;
        }

        scenario_outcome outcomes[2];
        for (size_t side = 0; side < 2; ++side) {
            backend_kind const kind = backend_pair::side(side);
            ColdStore const cold = stores.seam(kind);
            GraphSerializer serializer{cold};
            expect(eq(serializer.save(source).saved_count, size_t{6}));
            // Overwrite one good record with bytes that are not a record at all, and add a
            // second "v/" key whose value is too short to be one.
            expect(write_raw_value(cold, dag.c->guid(),
                                   gtest::to_bytes("this-is-not-a-vertex-record")));
            expect(write_raw_value(cold, junk, gtest::to_bytes("GDBshort")));
            expect(eq(live_vertex_records(cold), size_t{7}));

            Graph restored;
            GraphDeserializer deserializer;
            GraphSerializeStats const stats = deserializer.load(cold, restored);
            fill_load_stats(stats, outcomes[side]);
            fill_graph_snapshot(restored, outcomes[side]);

            expect(eq(stats.loaded_count, size_t{5}))
                << "the two corrupt records are dropped, the five good ones load";
            expect(eq(stats.skipped_count, size_t{2})) << "both corrupt records are counted";
            expect(eq(stats.skipped_edge_count, size_t{2}))
                << "a -> c and c -> d lost their endpoint";
            expect(!restored.contains(dag.c->guid())) << "the corrupted vertex is gone";
            expect(!restored.contains(junk));
            expect(eq(restored.dependencies(dag.a->guid()).size(), size_t{1}))
                << "a keeps only its edge to b";
            // A corrupt record never fails the load: the result is re-savable.
            expect(restored.dirty_vertices().empty());
            GraphSerializer resaver{cold};
            expect(eq(resaver.save_incremental(restored).saved_count, size_t{0}));
        }

        kimix::string const diff = outcome_diff(outcomes[0], outcomes[1]);
        expect(diff.empty()) << "the two backends must agree byte for byte: " << diff;
    };

    "file_store_parity_v1_and_v2_record_paths"_test = [] {
        // A store mixing a v1 ("GDB1") record and a v2 ("GDB2") record — the decoder
        // dispatches on the magic, and a store without a meta record is format 1. Both
        // backends must land on the same graph, with the v1 dependency list turned into a
        // real edge (synthesized_edge_count).
        gtest::temp_dir_guard lmdb_dir;
        gtest::temp_dir_guard file_dir;
        expect(lmdb_dir.ok() && file_dir.ok()) << "temp directories must be creatable";
        if (!lmdb_dir.ok() || !file_dir.ok()) {
            return;
        }

        kimix::Guid const v1_guid = fixed_guid(21);
        kimix::Guid const v2_guid = fixed_guid(22);

        // The v2 record: everything the GDB2 layout carries except the GDI key/labels, so
        // the loader needs no schema table to make sense of it.
        graph_db::VertexRecordV2 v2;
        v2.guid = v2_guid;
        v2.content = gtest::to_bytes("B");
        v2.content_hash = graph_db::md5_of(v2.content);
        v2.external_id = gtest::to_bytes("ext-b");
        kimix::vector<std::byte> v2_encoded;
        graph_db::encode_vertex_record_v2(v2, v2_encoded);
        expect(eq(v2_encoded.size(), graph_db::encoded_vertex_record_v2_size(v2)));
        // A v1 record whose dependency list names the v2 vertex.
        VertexRecord const v1 = gtest::make_record(v1_guid, gtest::to_bytes("A"), {v2_guid}, {});

        backend_pair stores{lmdb_dir.path(), file_dir.path()};
        expect(stores.open()) << "both backends must open";
        if (!stores.is_open()) {
            return;
        }

        scenario_outcome outcomes[2];
        for (size_t side = 0; side < 2; ++side) {
            backend_kind const kind = backend_pair::side(side);
            ColdStore const cold = stores.seam(kind);
            expect(write_record(cold, v1)) << "the GDB1 record is stored";
            expect(write_raw_value(cold,
                                   v2_guid,
                                   kimix::span<const std::byte>{v2_encoded.data(),
                                                                v2_encoded.size()}))
                << "the GDB2 record is stored";

            // Both values are what the codecs expect, on either backend.
            VertexRecord v1_back;
            expect(graph_db::decode_vertex_record(stored_vertex_value(cold, v1_guid), v1_back));
            expect(gtest::blob_equal(v1_back.content, "A"));
            graph_db::VertexRecordV2 v2_back;
            expect(graph_db::decode_vertex_record_v2(stored_vertex_value(cold, v2_guid), v2_back));
            expect(gtest::blob_equal(v2_back.content, "B"));
            expect(gtest::blob_equal(v2_back.external_id, "ext-b"))
                << "the v2 external id survives the store round trip";

            Graph graph;
            GraphDeserializer deserializer;
            GraphSerializeStats const stats = deserializer.load(cold, graph);
            fill_load_stats(stats, outcomes[side]);
            fill_graph_snapshot(graph, outcomes[side]);

            expect(eq(stats.loaded_count, size_t{2})) << "both record versions decode";
            expect(eq(stats.skipped_count, size_t{0}));
            expect(eq(stats.format_version, uint32_t{1}))
                << "no meta record means format 1, on both backends";
            expect(stats.migrated_from_v1);
            expect(eq(stats.skipped_edge_count, size_t{0}));
            expect(eq(stats.synthesized_edge_count, size_t{1}))
                << "the v1 dependency list became a real edge";
            expect(eq(graph.dependencies(v1_guid).size(), size_t{1}));
            expect(graph.dependencies(v1_guid).front() == v2_guid);
            expect(eq(graph.dependents(v2_guid).size(), size_t{1}));
        }

        kimix::string const diff = outcome_diff(outcomes[0], outcomes[1]);
        expect(diff.empty()) << "the two backends must agree byte for byte: " << diff;
    };

    "file_store_parity_meta_record"_test = [] {
        // The "meta/root" record: absent on a fresh store, written through the seam
        // (save_batch, exactly what the serializer does), read back field by field and
        // held as identical BYTES by both backends; then stamped by a serialized save.
        gtest::temp_dir_guard lmdb_dir;
        gtest::temp_dir_guard file_dir;
        expect(lmdb_dir.ok() && file_dir.ok()) << "temp directories must be creatable";
        if (!lmdb_dir.ok() || !file_dir.ok()) {
            return;
        }

        backend_pair stores{lmdb_dir.path(), file_dir.path()};
        expect(stores.open()) << "both backends must open";
        if (!stores.is_open()) {
            return;
        }

        MetaRecord meta;
        meta.format_version = graph_db::kFormatVersion;
        meta.store_uuid = fixed_guid(31);
        meta.last_full_save_ms = 1234567890123ull;
        meta.last_incr_save_ms = 1234567890124ull;
        meta.schema_version = 7;
        meta.next_label_id = 11;
        meta.next_prop_name_id = 13;
        meta.graph_name = "parity";

        kimix::vector<std::byte> meta_bytes[2];
        for (size_t side = 0; side < 2; ++side) {
            backend_kind const kind = backend_pair::side(side);
            ColdStore const cold = stores.seam(kind);

            // Absent first (a v1 store), then written by hand.
            MetaRecord read_back;
            expect(!cold.read_meta(read_back)) << "a fresh store has no meta";
            expect(write_meta(cold, meta)) << "the meta record is writable through the seam";
            expect(cold.read_meta(read_back)) << "…and readable back";
            expect(eq(read_back.format_version, meta.format_version));
            expect(read_back.store_uuid == meta.store_uuid);
            expect(eq(read_back.last_full_save_ms, meta.last_full_save_ms));
            expect(eq(read_back.last_incr_save_ms, meta.last_incr_save_ms));
            expect(eq(read_back.schema_version, meta.schema_version));
            expect(eq(read_back.next_label_id, meta.next_label_id));
            expect(eq(read_back.next_prop_name_id, meta.next_prop_name_id));
            expect(read_back.graph_name == meta.graph_name);

            // The stored value is the codec's own output — no second copy of the format.
            kimix::vector<std::byte> expected_bytes;
            graph_db::encode_meta_record(meta, expected_bytes);
            meta_bytes[side] = stored_value(cold, key_bytes(graph_db::kMetaRootKey));
            expect(same_bytes(meta_bytes[side],
                              kimix::span<const std::byte>{expected_bytes.data(),
                                                           expected_bytes.size()}))
                << "the meta is stored as one ordinary record: " << backend_name(kind);

            // A serialized save stamps the store with the meta the caller handed over.
            Graph graph;
            fixed_small_dag(graph);
            GraphSerializer serializer{cold};
            MetaRecord handed = meta;
            handed.graph_name = "parity-save";
            serializer.set_meta(handed);
            expect(eq(serializer.save(graph).saved_count, size_t{6}));
            MetaRecord stamped;
            expect(cold.read_meta(stamped));
            expect(eq(stamped.format_version, graph_db::kFormatVersion));
            expect(stamped.graph_name == handed.graph_name);
            expect(eq(stamped.schema_version, handed.schema_version))
                << "a save keeps the schema version the caller handed over";
            expect(stamped.store_uuid == handed.store_uuid)
                << "a save keeps the store identity the caller handed over";
        }
        expect(same_bytes(meta_bytes[0], meta_bytes[1]))
            << "both backends store the same meta bytes";
    };

    "file_store_parity_remove_record_and_keys"_test = [] {
        // Removal: ColdStorage deletes the key (remove_record / remove_keys), while the
        // second backend's only removal vocabulary is the TOMBSTONE a save writes (an
        // empty value under the key), because the seam has no delete operation. The live
        // state a caller observes must be identical; the mechanism difference is asserted
        // explicitly.
        //
        // The store byte images are deliberately NOT compared here (they differ by
        // construction: 4 records vs 6 with two tombstones) — the load outcome is.
        gtest::temp_dir_guard lmdb_dir;
        gtest::temp_dir_guard file_dir;
        expect(lmdb_dir.ok() && file_dir.ok()) << "temp directories must be creatable";
        if (!lmdb_dir.ok() || !file_dir.ok()) {
            return;
        }

        Graph source;
        fixed_dag dag = fixed_small_dag(source);
        expect(dag.b != nullptr && dag.c != nullptr);
        if (dag.b == nullptr || dag.c == nullptr) {
            return;
        }

        backend_pair stores{lmdb_dir.path(), file_dir.path()};
        expect(stores.open()) << "both backends must open";
        if (!stores.is_open()) {
            return;
        }

        scenario_outcome outcomes[2];
        size_t stored_records[2] = {0, 0};
        size_t tombstones[2] = {0, 0};
        for (size_t side = 0; side < 2; ++side) {
            backend_kind const kind = backend_pair::side(side);
            ColdStore const cold = stores.seam(kind);
            GraphSerializer serializer{cold};
            expect(eq(serializer.save(source).saved_count, size_t{6}));
            expect(eq(live_vertex_records(cold), size_t{6}));

            if (kind == backend_kind::lmdb) {
                // The LMDB backend deletes: one record by guid, one batch of raw keys.
                expect(stores.lmdb().remove_record(dag.c->guid()))
                    << "ColdStorage::remove_record drops the record";
                kimix::vector<kimix::vector<std::byte>> keys;
                keys.push_back(guid_key_bytes(dag.b->guid()));
                expect(stores.lmdb().remove_keys(std::move(keys)))
                    << "ColdStorage::remove_keys drops a batch of raw keys";
            } else {
                // The second backend's seam has no removal op: an empty value is how a
                // save says "gone", and the loader reads it as a deletion.
                expect(write_raw_value(cold, dag.c->guid(), kimix::span<const std::byte>{}))
                    << "the file backend stores a tombstone for c";
                expect(write_raw_value(cold, dag.b->guid(), kimix::span<const std::byte>{}))
                    << "…and one for b";
            }

            // Live records: the four remaining vertices on both backends.
            expect(eq(live_vertex_records(cold), size_t{4}));
            stored_records[side] = record_bytes(cold).size();

            Graph restored;
            GraphDeserializer deserializer;
            GraphSerializeStats const stats = deserializer.load(cold, restored);
            fill_load_stats(stats, outcomes[side]);
            fill_graph_snapshot(restored, outcomes[side]);
            tombstones[side] = stats.tombstone_count;

            expect(eq(stats.loaded_count, size_t{4})) << "the removed vertices are gone";
            expect(eq(stats.skipped_count, size_t{0}))
                << "a tombstone is a deletion, not a corrupt record";
            expect(!restored.contains(dag.c->guid()));
            expect(!restored.contains(dag.b->guid()));
            expect(eq(stats.skipped_edge_count, size_t{4}))
                << "a -> b, a -> c, b -> d and c -> d lost an endpoint";
        }

        expect(eq(stored_records[0], size_t{9}))
            << "LMDB drops the removed keys: 4 vertex + 5 edge records remain";
        expect(eq(stored_records[1], size_t{11}))
            << "the second backend keeps the keys and their two tombstones";
        expect(eq(tombstones[0], size_t{0})) << "LMDB has no tombstone to read";
        expect(eq(tombstones[1], size_t{2}))
            << "the second backend's two deletions are read back as tombstones";

        kimix::string const diff = outcome_diff(outcomes[0], outcomes[1]);
        expect(diff.empty()) << "the two backends must agree on the live state: " << diff;
    };

    "file_store_parity_copy_to_checkpoint"_test = [] {
        // A checkpoint must load back identically. For the second backend copy_to() is
        // LMDB's own compacting copy of its environment, so the checkpoint holds a
        // graph.xdb environment directory of its own.
        gtest::temp_dir_guard lmdb_dir;
        gtest::temp_dir_guard file_dir;
        gtest::temp_dir_guard lmdb_checkpoint;
        gtest::temp_dir_guard file_checkpoint;
        expect(lmdb_dir.ok() && file_dir.ok() && lmdb_checkpoint.ok() && file_checkpoint.ok())
            << "temp directories must be creatable";
        if (!lmdb_dir.ok() || !file_dir.ok() || !lmdb_checkpoint.ok() ||
            !file_checkpoint.ok()) {
            return;
        }

        Graph source;
        fixed_small_dag(source);
        expect(eq(source.size(), size_t{6}));
        scenario_outcome source_outcome;
        fill_graph_snapshot(source, source_outcome);

        backend_pair stores{lmdb_dir.path(), file_dir.path()};
        expect(stores.open()) << "both backends must open";
        if (!stores.is_open()) {
            return;
        }

        scenario_outcome outcomes[2];
        for (size_t side = 0; side < 2; ++side) {
            backend_kind const kind = backend_pair::side(side);
            ColdStore const cold = stores.seam(kind);
            GraphSerializer serializer{cold};
            expect(eq(serializer.save(source).saved_count, size_t{6}));
            kimix::filesystem::path const target =
                kind == backend_kind::lmdb ? lmdb_checkpoint.path() : file_checkpoint.path();
            expect(serializer.copy_to(target)) << "copy_to checkpoints the store";

            // The checkpoint is a usable store of the SAME backend.
            ColdStorage lmdb_copy;
            FileColdStore file_copy;
            bool opened = false;
            if (kind == backend_kind::lmdb) {
                opened = lmdb_copy.open(target);
            } else {
                opened = file_copy.open(target);
            }
            expect(opened) << "the checkpoint opens as a store";
            if (!opened) {
                continue;
            }
            ColdStore const copy =
                kind == backend_kind::lmdb ? ColdStore{lmdb_copy} : ColdStore{file_copy};
            Graph restored;
            GraphDeserializer deserializer;
            fill_load_stats(deserializer.load(copy, restored), outcomes[side]);
            fill_graph_snapshot(restored, outcomes[side]);
            expect(eq(outcomes[side].loaded_count, size_t{6}));
            expect(eq(outcomes[side].skipped_edge_count, size_t{0}));
            expect(outcomes[side].adjacency == source_outcome.adjacency)
                << "the checkpoint reproduces the whole graph, edges included";
            if (kind == backend_kind::flat_file) {
                // A compacted environment copy under <target>\graph.xdb, closed before
                // returning — so the checkpoint directory is immediately deletable on
                // Windows.
                env_layout const live = read_store_env(stores.dir(kind));
                env_layout const copied = read_store_env(target);
                expect(live.is_directory && live.has_data) << "the live store is an environment";
                expect(copied.is_directory && copied.has_data)
                    << "the checkpoint is an environment of its own";
                expect(gt(copied.data_bytes, uint64_t{0}));
                // The copy is COMPACTED, so it is never larger than the live file.
                expect(le(copied.data_bytes, live.data_bytes))
                    << "the checkpoint is a compacted copy of the store";
                expect(same_records(record_bytes(copy), record_bytes(cold)))
                    << "the checkpoint and the live store hold the same records";
            }
        }

        kimix::string const diff = outcome_diff(outcomes[0], outcomes[1]);
        expect(diff.empty()) << "the two backends must agree byte for byte: " << diff;
        // The checkpoint really was a checkpoint: the source store is untouched, so it
        // still loads its six vertices.
        for (size_t side = 0; side < 2; ++side) {
            expect(eq(live_vertex_records(stores.seam(backend_pair::side(side))), size_t{6}));
        }
    };

    "file_store_parity_incremental_after_load_writes_nothing"_test = [] {
        // A load leaves the vertices clean, so an immediate incremental save is an EMPTY
        // batch: no transaction is started at all, so the store's data file stays
        // byte-identical (nothing is written, nothing is padded, nothing is truncated).
        gtest::temp_dir_guard lmdb_dir;
        gtest::temp_dir_guard file_dir;
        expect(lmdb_dir.ok() && file_dir.ok()) << "temp directories must be creatable";
        if (!lmdb_dir.ok() || !file_dir.ok()) {
            return;
        }

        Graph source;
        fixed_small_dag(source);

        backend_pair stores{lmdb_dir.path(), file_dir.path()};
        expect(stores.open()) << "both backends must open";
        if (!stores.is_open()) {
            return;
        }

        scenario_outcome outcomes[2];
        for (size_t side = 0; side < 2; ++side) {
            backend_kind const kind = backend_pair::side(side);
            ColdStore const cold = stores.seam(kind);
            GraphSerializer serializer{cold};
            expect(eq(serializer.save(source).saved_count, size_t{6}));

            Graph restored;
            GraphDeserializer deserializer;
            expect(eq(deserializer.load(cold, restored).loaded_count, size_t{6}));
            expect(restored.dirty_vertices().empty())
                << "loaded vertices are clean, so nothing is dirty";

            kimix::filesystem::path const file = store_data_file(stores.dir(kind));
            uintmax_t const size_before = file_bytes(file);
            kimix::vector<std::byte> const bytes_before = read_file_bytes(file);
            auto const records_before = record_bytes(cold);

            GraphSerializeStats const noop = serializer.save_incremental(restored);
            fill_save_stats(noop, outcomes[side]);
            expect(eq(noop.saved_count, size_t{0})) << "nothing to write after a load";
            expect(eq(noop.bytes_written, size_t{0}));
            expect(eq(noop.skipped_count, size_t{0}));
            expect(same_records(record_bytes(cold), records_before))
                << "an empty batch leaves every stored byte alone";
            expect(eq(file_bytes(file), size_before))
                << "an empty batch does not grow the store file";
            expect(same_bytes(read_file_bytes(file), bytes_before))
                << "an empty batch leaves the store file byte-identical";
            fill_store_snapshot(cold, outcomes[side]);
        }
        kimix::string const diff = outcome_diff(outcomes[0], outcomes[1]);
        expect(diff.empty()) << "the two backends must agree byte for byte: " << diff;
    };

    "file_store_copy_to_refuses_the_live_environment"_test = [] {
        // copy_to() clears its destination before LMDB writes the copy into it, so a
        // destination that IS the live environment would delete the store it is reading.
        // The guard has to compare the two paths the way the filesystem does: the SAME
        // directory spelled with a redundant "." segment is the same environment, and a
        // textual compare would wave it through.
        gtest::temp_dir_guard dir;
        expect(dir.ok()) << "a temp directory must be creatable";
        if (!dir.ok()) {
            return;
        }
        FileColdStore store;
        expect(store.open(dir.path())) << "the second backend opens its own environment";
        if (!store.valid()) {
            return;
        }
        ColdStore const cold{store};
        kimix::vector<std::byte> const content = gtest::to_bytes("payload");
        expect(write_raw_value(cold, fixed_guid(1), kimix::span<const std::byte>{content.data(), content.size()}));
        expect(eq(live_vertex_records(cold), size_t{1}));

        kimix::filesystem::path const live = store.path();
        kimix::filesystem::path const spelled_with_a_dot = live / kimix::filesystem::path{"."};
        expect(spelled_with_a_dot.string() != live.string())
            << "the two spellings differ, so a textual compare would not catch it";
        expect(!store.copy_to(live)) << "a checkpoint onto the live store is refused";
        expect(!store.copy_to(spelled_with_a_dot))
            << "and so is the same directory spelled with a dot segment";
        // The live store survived both refusals: its record is still there and it still
        // answers a checkpoint into a DIFFERENT directory.
        expect(eq(live_vertex_records(cold), size_t{1})) << "the store was not deleted";
        gtest::temp_dir_guard checkpoint;
        if (checkpoint.ok()) {
            expect(store.copy_to(checkpoint.path())) << "a real checkpoint still works";
            expect(eq(store_bytes(checkpoint.path()) > 0, true));
        }
    };

    "file_store_rebuild_of_an_empty_batch_is_refused"_test = [] {
        // A rebuild() batch IS the whole store, so an empty one cannot be told from a
        // caller bug. It answers false and leaves the store untouched instead of emptying
        // a store the caller never meant to clear; an empty INCREMENTAL batch stays a
        // success, because "nothing to write" is what an incremental save of a clean
        // graph is.
        gtest::temp_dir_guard dir;
        expect(dir.ok()) << "a temp directory must be creatable";
        if (!dir.ok()) {
            return;
        }
        FileColdStore store;
        expect(store.open(dir.path()));
        if (!store.valid()) {
            return;
        }
        ColdStore const cold{store};
        kimix::vector<std::byte> const content = gtest::to_bytes("payload");
        expect(write_raw_value(cold, fixed_guid(7), kimix::span<const std::byte>{content.data(), content.size()}));
        expect(eq(live_vertex_records(cold), size_t{1}));

        expect(store.save_batch({})) << "an empty incremental batch is a no-op success";
        expect(!store.rebuild({})) << "an empty full-save batch writes nothing and says so";
        expect(eq(live_vertex_records(cold), size_t{1}))
            << "and it does NOT empty the store it could not describe";

        // A non-empty rebuild really does replace the content.
        kimix::vector<std::byte> const other = gtest::to_bytes("other");
        expect(store.rebuild(one_command(
                fixed_guid(9), kimix::span<const std::byte>{other.data(), other.size()})));
        expect(eq(live_vertex_records(cold), size_t{1})) << "the old record is gone";
        kimix::vector<ColdRecord> records;
        expect(cold.load_all_records(records));
        expect(eq(records.size(), size_t{1})) << "a rebuild replaces the whole store";
        if (records.size() == 1) {
            expect(same_bytes(kimix::span<const std::byte>{records[0].value.data(), records[0].value.size()},
                                kimix::span<const std::byte>{other.data(), other.size()}))
                << "and it holds exactly the batch it was given";
        }
    };

    "file_store_stale_rebuild_arm_cannot_wipe_the_store"_test = [] {
        // The "next batch is a complete snapshot" arm is consumed by the next save_batch()
        // that REACHES the store — so a save that writes nothing leaves it armed. The
        // serializer must drop it for an incremental save, or the NEXT (dirty-only) batch
        // would be written through rebuild(), i.e. with mdb_drop first, wiping every record
        // that batch does not carry.
        gtest::temp_dir_guard dir;
        expect(dir.ok()) << "a temp directory must be creatable";
        if (!dir.ok()) {
            return;
        }
        FileColdStore store;
        expect(store.open(dir.path()));
        if (!store.valid()) {
            return;
        }
        ColdStore const cold{store};

        Graph graph;
        fixed_dag const dag = fixed_small_dag(graph);
        expect(eq(graph.size(), size_t{6})) << "six vertices";
        if (graph.size() != 6) {
            return;
        }

        // A FULL save first: 6 vertex records and one per edge (the meta record aside).
        GraphSerializer serializer{cold};
        GraphSerializeStats const full = serializer.save(graph);
        expect(eq(full.status, graph_db::GdiStatus::ok));
        expect(eq(full.saved_count, size_t{6}));
        expect(eq(full.saved_edge_count, size_t{5}));
        kimix::vector<raw_record> const before = record_bytes(cold);
        expect(eq(count_keys(before, graph_db::kVertexKeyPrefix), size_t{6}));
        expect(eq(count_keys(before, graph_db::kEdgeKeyPrefix), size_t{5}));
        expect(eq(before.size(), size_t{11})) << "11 records: 6 vertices + 5 edges";

        // ARM the rebuild, then save the CLEAN graph incrementally: it writes no byte at
        // all, so it never reaches save_batch() and cannot consume the arm.
        store.set_rebuild_next_batch(true);
        GraphSerializeStats const clean = serializer.save_incremental(graph);
        expect(eq(clean.status, graph_db::GdiStatus::ok));
        expect(eq(clean.bytes_written, size_t{0})) << "a clean incremental save writes nothing";
        expect(eq(clean.saved_count, size_t{0}));
        expect(eq(clean.saved_edge_count, size_t{0}));
        expect(same_records(record_bytes(cold), before))
            << "and the store is untouched, so the arm is still pending";

        // ONE content change, then an incremental save: the armed store would empty the
        // database before putting this single record.
        expect(graph.update_content(dag.b->guid(), gtest::to_bytes("b-changed")));
        GraphSerializeStats const incremental = serializer.save_incremental(graph);
        expect(eq(incremental.status, graph_db::GdiStatus::ok));
        expect(eq(incremental.saved_count, size_t{1})) << "one dirty vertex";
        expect(eq(incremental.saved_edge_count, size_t{0})) << "and no dirty edge";

        kimix::vector<raw_record> const after = record_bytes(cold);
        expect(eq(after.size(), size_t{11})) << "the store still holds every record";
        expect(eq(count_keys(after, graph_db::kVertexKeyPrefix), size_t{6}))
            << "a wiped store would hold a single vertex record";
        expect(eq(count_keys(after, graph_db::kEdgeKeyPrefix), size_t{5}))
            << "and no edge record at all";
        // Exactly one record was rewritten: the changed vertex's value differs, every
        // other record is still bit-identical.
        auto const changed_key = guid_key_bytes(dag.b->guid());
        size_t untouched = 0;
        for (auto const &entry : before) {
            auto const found =
                std::find_if(after.begin(), after.end(), [&entry](raw_record const &candidate) {
                    return same_bytes(candidate.key, entry.key);
                });
            expect(found != after.end()) << "a record the save did not rewrite is still there";
            if (found == after.end()) {
                continue;
            }
            if (same_bytes(entry.key,
                           kimix::span<const std::byte>{changed_key.data(), changed_key.size()})) {
                expect(!same_bytes(found->value, entry.value))
                    << "the changed vertex's record was rewritten";
            } else {
                ++untouched;
                expect(same_bytes(found->value, entry.value))
                    << "every record the save did not touch is bit-identical";
            }
        }
        expect(eq(untouched, size_t{10})) << "10 of the 11 records were left alone";

        // Reload into a FRESH graph: every vertex and every edge comes back.
        Graph restored;
        GraphDeserializer deserializer;
        GraphSerializeStats const loaded = deserializer.load(cold, restored);
        expect(eq(loaded.status, graph_db::GdiStatus::ok));
        expect(eq(loaded.loaded_count, size_t{6})) << "every vertex is back";
        expect(eq(loaded.loaded_edge_count, size_t{5})) << "every edge is back";
        expect(eq(loaded.skipped_count, size_t{0}));
        kimix::vector<Vertex *> restored_vertices;
        for (uint64_t index = 1; index <= 6; ++index) {
            restored_vertices.push_back(restored.find(fixed_guid(index)));
            expect(restored_vertices.back() != nullptr) << "vertex " << index << " is back";
        }
        expect(gtest::adjacency_lines(restored, restored_vertices) ==
               gtest::adjacency_lines(graph, dag.vertices))
            << "the reloaded adjacency is the graph's adjacency";
        Vertex *restored_b = restored.find(fixed_guid(2));
        expect(restored_b != nullptr);
        if (restored_b != nullptr) {
            auto const content = restored_b->content_copy();
            expect(gtest::blob_equal(
                      kimix::span<const std::byte>{content.data(), content.size()}, "b-changed"))
                << "the changed content survived the reload";
        }
    };

    "file_store_refuses_an_unstorable_key_and_writes_nothing"_test = [] {
        // An empty key and one longer than LMDB's MDB_MAXKEYSIZE cannot be stored: handing
        // either to mdb_put would abort the process, so the backend refuses the WHOLE batch
        // before it writes anything. A refused batch leaves the store exactly as it was and
        // the store stays usable for the next, well-formed one.
        gtest::temp_dir_guard dir;
        expect(dir.ok()) << "a temp directory must be creatable";
        if (!dir.ok()) {
            return;
        }
        FileColdStore store;
        expect(store.open(dir.path()));
        if (!store.valid()) {
            return;
        }
        ColdStore const cold{store};

        kimix::vector<std::byte> const value = gtest::to_bytes("payload");
        expect(write_raw_value(cold, fixed_guid(1),
                               kimix::span<const std::byte>{value.data(), value.size()}));
        kimix::vector<raw_record> const before = record_bytes(cold);
        expect(eq(before.size(), size_t{1}));

        // (1) a key longer than FileColdStore::kMaxKeyLength (511 == MDB_MAXKEYSIZE). The
        // batch also holds a WELL-FORMED command first, so a partial write would be visible
        // as an extra record.
        kimix::vector<kimix::LMDBWriteCommand> too_long = one_command(
            fixed_guid(2), kimix::span<const std::byte>{value.data(), value.size()});
        kimix::LMDBWriteCommand long_key;
        long_key.key.assign(size_t{600}, std::byte{'k'});
        long_key.value.assign(value.begin(), value.end());
        expect(gt(long_key.key.size(), FileColdStore::kMaxKeyLength));
        too_long.push_back(std::move(long_key));
        expect(!store.save_batch(std::move(too_long)))
            << "a 600-byte key is not a store key, so the batch is refused";
        expect(same_records(record_bytes(cold), before))
            << "a refused batch writes nothing — not even its valid command";
        expect(store.valid()) << "and it leaves the store open";

        // (2) an EMPTY key is refused the same way.
        kimix::vector<kimix::LMDBWriteCommand> empty_key;
        kimix::LMDBWriteCommand no_key;
        no_key.value.assign(value.begin(), value.end());
        expect(no_key.key.empty());
        empty_key.push_back(std::move(no_key));
        expect(!store.save_batch(std::move(empty_key))) << "an empty key is refused";
        expect(same_records(record_bytes(cold), before)) << "and it writes nothing either";

        // A following well-formed batch still writes: the refusals left nothing behind.
        expect(write_raw_value(cold, fixed_guid(3),
                               kimix::span<const std::byte>{value.data(), value.size()}))
            << "the store still accepts a storable batch";
        expect(eq(record_bytes(cold).size(), before.size() + 1));
    };

    "file_store_open_failure_degrades"_test = [] {
        // A store that cannot be opened reports exactly the state a closed ColdStorage
        // reports: valid() false, every operation false, no crash and no half-open store.
        // The last point is what makes this case worth its own test: kimix::LMDB ABORTS on
        // a bad environment, so FileColdStore has to recognise "not a store this build
        // wrote" BEFORE it hands the path to LMDB.
          gtest::temp_dir_guard occupied;   // a regular FILE where the env directory belongs
          gtest::temp_dir_guard bad_data;   // an env directory whose data.mdb is foreign
          gtest::temp_dir_guard short_data; // ... whose data.mdb is truncated
          gtest::temp_dir_guard old_format; // ... whose data.mdb is a 0.9-format LMDB store
          expect(occupied.ok() && bad_data.ok() && short_data.ok() && old_format.ok())
              << "temp directories must be creatable";
          if (!occupied.ok() || !bad_data.ok() || !short_data.ok() || !old_format.ok()) {
              return;
          }

        // A main()-scope CRT writer: the store files are written by the test itself, so a
        // failure is visible here instead of inside the code under test.
        auto const write_bytes = [](kimix::filesystem::path const &file,
                                    kimix::span<const std::byte> bytes) {
            kimix::string const name = kimix::to_string(file);
            std::FILE *stream = std::fopen(name.c_str(), "wb");
            if (stream == nullptr) {
                return false;
            }
            size_t const written = bytes.empty()
                                        ? size_t{0}
                                        : std::fwrite(bytes.data(), 1, bytes.size(), stream);
            std::fclose(stream);
            return written == bytes.size();
        };

        // (1) a regular FILE where the environment DIRECTORY belongs: refused, instead of
        // letting mdb_env_open find out and abort the process.
        std::byte junk[64];
        std::memcpy(junk, "NOTANENV", 8);
        expect(write_bytes(store_file(occupied.path()),
                           kimix::span<const std::byte>{junk, sizeof(junk)}));
        FileColdStore store;
        expect(!store.open(occupied.path())) << "a regular file is not a store environment";
        expect(!store.valid());
        expect(store.path().empty()) << "a failed open leaves no store behind";
        MetaRecord meta;
        expect(!store.read_meta(meta));
        kimix::vector<ColdRecord> records;
        expect(!store.load_all_records(records));
        expect(records.empty());
        expect(!store.save_batch(one_command(fixed_guid(51), gtest::to_bytes("x"))));
        expect(!store.rebuild(one_command(fixed_guid(52), gtest::to_bytes("x"))));
        expect(!store.copy_to(short_data.path()));

        // (2) an empty path is refused the same way.
        FileColdStore empty_path;
        expect(!empty_path.open(kimix::filesystem::path{}));
        expect(!empty_path.valid());
        expect(!empty_path.load_all_records(records));

        // (3) an environment directory whose data.mdb is foreign, and one whose data.mdb
        // is too short to hold LMDB's meta page: neither is a store this build can open,
        // and neither may reach mdb_env_open (which would abort).
        std::error_code ec;
        expect(kimix::filesystem::create_directories(store_file(bad_data.path()), ec));
        expect(!ec);
        std::byte foreign[64]{};
        std::memcpy(foreign, "NOTLMDB!", 8);
        expect(write_bytes(store_data_file(bad_data.path()),
                           kimix::span<const std::byte>{foreign, sizeof(foreign)}));
        FileColdStore bad_magic_store;
        expect(!bad_magic_store.open(bad_data.path())) << "a foreign data.mdb is refused";
        expect(!bad_magic_store.valid());

        expect(kimix::filesystem::create_directories(store_file(short_data.path()), ec));
        expect(!ec);
        std::byte truncated[8]{};
        std::memcpy(truncated, "LMDB", 4);
        expect(write_bytes(store_data_file(short_data.path()),
                           kimix::span<const std::byte>{truncated, sizeof(truncated)}));
        FileColdStore short_store;
        expect(!short_store.open(short_data.path()))
            << "a data.mdb truncated inside its meta page is refused";
        expect(!short_store.valid());
        expect(!short_store.load_all_records(records));

        // (3b) a data.mdb written in LMDB 0.9's on-disk format: MDB_MAGIC is the same,
        // but the meta page header is 8 bytes shorter (1.x stores a txnid in every page
        // header) and mm_version is 1 instead of kLmdbDataVersion. A store left behind by
        // a kimix build that vendored the 0.9 line lands here, so this case pins the
        // upgrade boundary: refused as a plain false, never handed to mdb_env_open.
        expect(kimix::filesystem::create_directories(store_file(old_format.path()), ec));
        expect(!ec);
        kimix::vector<std::byte> legacy(4096, std::byte{0}); // one 4 KiB page, 0.9 meta page
        graph_db::detail::write_u32(legacy.data() + 16, kLmdbMagic);
        graph_db::detail::write_u32(legacy.data() + 20, uint32_t{1});
        expect(write_bytes(store_data_file(old_format.path()),
                           kimix::span<const std::byte>{legacy.data(), legacy.size()}));
        FileColdStore legacy_store;
        expect(!legacy_store.open(old_format.path()))
            << "a 0.9-format data.mdb is refused instead of aborting inside LMDB";
        expect(!legacy_store.valid());
        expect(!legacy_store.load_all_records(records));

        // (4) a refused open writes nothing into the directory it was given.
        std::error_code untouched_ec;
        expect(kimix::filesystem::is_regular_file(store_file(occupied.path()), untouched_ec))
            << "the blocking file is still there, untouched";
        expect(!untouched_ec);
        expect(!kimix::filesystem::exists(store_data_file(occupied.path()), untouched_ec))
            << "a refused open creates no environment";
    };

    "file_store_large_values_round_trip"_test = [] {
        // The large-value path: six ~700 KiB records (4+ MiB of payload) go in through ONE
        // save batch, are read back intact, survive a later incremental save and a full
        // close/reopen of the store. A value that does not fit a page lives in LMDB
        // OVERFLOW pages, so this is the shape that breaks first on a wrong page
        // accounting — and it is also the size at which a copy of the value (instead of a
        // pointer into the map) is what keeps the records valid.
        gtest::temp_dir_guard dir;
        expect(dir.ok()) << "a temp directory must be creatable";
        if (!dir.ok()) {
            return;
        }

        constexpr size_t payload_bytes = 700 * 1024;
        kimix::vector<std::byte> const content(payload_bytes, std::byte{'z'});
        kimix::vector<kimix::LMDBWriteCommand> batch;
        for (uint64_t i = 0; i < 6; ++i) {
            VertexRecord const record =
                gtest::make_record(fixed_guid(100 + i),
                                   kimix::span<const std::byte>{content.data(), content.size()},
                                   {}, {});
            kimix::LMDBWriteCommand command;
            graph_db::guid_key(record.guid, command.key);
            graph_db::encode_vertex_record(record, command.value);
            batch.push_back(std::move(command));
        }

        {
            FileColdStore store;
            expect(store.open(dir.path()));
            ColdStore const cold = ColdStore{store};
            expect(cold.save_batch(std::move(batch))) << "one batch holds the whole payload";
            expect(gt(store_bytes(dir.path()), static_cast<uintmax_t>(4ull * 1024ull * 1024ull)))
                << "the payload really reached the store's data file (past 4 MiB)";
            expect(read_store_env(dir.path()).has_data);

            kimix::vector<ColdRecord> first;
            expect(cold.load_all_records(first)) << "the scan succeeds";
            expect(eq(first.size(), size_t{6}));
            expect(eq(live_vertex_records(cold), size_t{6}));

            // A save in between: the incremental path writes its own transaction, so the
            // memory of the scan above is long gone by the next scan.
            expect(cold.save_batch(one_command(fixed_guid(200), gtest::to_bytes("late"))));

            kimix::vector<ColdRecord> second;
            expect(cold.load_all_records(second));
            expect(eq(second.size(), size_t{7}));
            // Every record loaded BEFORE the save is intact and byte-identical.
            size_t matched = 0;
            for (auto const &before : first) {
                auto const found =
                    std::find_if(second.begin(), second.end(), [&before](ColdRecord const &candidate) {
                        return same_bytes(candidate.key, before.key);
                    });
                expect(found != second.end()) << "a record loaded earlier still exists";
                if (found == second.end()) {
                    continue;
                }
                expect(same_bytes(found->value, before.value))
                    << "a record survives a later save intact";
                ++matched;
            }
            expect(eq(matched, size_t{6}));

            // ... and they are the bytes that were written, so nothing was re-pointed at a
            // recycled buffer or truncated at a page boundary.
            for (uint64_t i = 0; i < 6; ++i) {
                VertexRecord const record =
                    gtest::make_record(fixed_guid(100 + i),
                                       kimix::span<const std::byte>{content.data(), content.size()},
                                       {}, {});
                kimix::vector<std::byte> encoded;
                graph_db::encode_vertex_record(record, encoded);
                expect(same_bytes(stored_vertex_value(cold, record.guid), encoded))
                    << "the stored value is the encoded record";
            }
        }

        // A full close/reopen: a committed transaction is durable without a further save.
        FileColdStore reopened;
        expect(reopened.open(dir.path())) << "the store reopens";
        ColdStore const cold = ColdStore{reopened};
        expect(eq(reopened.path(), dir.path()));
        expect(eq(live_vertex_records(cold), size_t{7}));
        kimix::vector<ColdRecord> third;
        expect(cold.load_all_records(third));
        expect(eq(third.size(), size_t{7}));
    };

    "file_store_layout_and_determinism"_test = [] {
        // Two full saves of the SAME graph must produce identical record bytes, and the
        // on-disk layout must be the one file_cold_store.h documents: an LMDB environment
        // DIRECTORY at <dir>\\graph.xdb holding data.mdb (a real LMDB file — its page 0 is
        // a meta page carrying MDB_MAGIC) and lock.mdb. Both are checked from the bytes,
        // not through FileColdStore.
        gtest::temp_dir_guard dir_a;
        gtest::temp_dir_guard dir_b;
        expect(dir_a.ok() && dir_b.ok()) << "temp directories must be creatable";
        if (!dir_a.ok() || !dir_b.ok()) {
            return;
        }

        // ONE graph, saved into two stores: the record bytes can only be compared across
        // stores when they came from the same graph (edge ids are generated per edge, so
        // two separately built graphs differ in their "e/" keys, and their vertex records
        // are identical by construction).
        Graph graph;
        fixed_layered_dag(graph, 32);
        expect(eq(graph.size(), size_t{32}));

        FileColdStore store_a;
        FileColdStore store_b;
        expect(store_a.open(dir_a.path()));
        expect(store_b.open(dir_b.path()));
        {
            GraphSerializer serializer{ColdStore{store_a}};
            expect(eq(serializer.save(graph).saved_count, size_t{32}));
        }
        {
            GraphSerializer serializer{ColdStore{store_b}};
            expect(eq(serializer.save(graph).saved_count, size_t{32}));
        }
        // The second full save of the same graph into store A goes through the REBUILD
        // path: a plain put would leave the superseded records beside the new ones, so the
        // replacing transaction is the documented way to write a full snapshot again.
        {
            GraphSerializer serializer{ColdStore{store_a}};
            store_a.set_rebuild_next_batch(true);
            expect(eq(serializer.save(graph).saved_count, size_t{32}));
        }

        // --- the documented layout -------------------------------------------
        env_layout const layout_a = read_store_env(dir_a.path());
        env_layout const layout_b = read_store_env(dir_b.path());
        expect(eq(FileColdStore::kDataFileName, kimix::string_view{"data.mdb"}));
        expect(eq(FileColdStore::kLockFileName, kimix::string_view{"lock.mdb"}));
        expect(layout_a.is_directory && layout_b.is_directory)
            << "graph.xdb is an LMDB environment DIRECTORY, not a flat file";
        expect(layout_a.has_data && layout_b.has_data) << "the environment holds data.mdb";
        expect(layout_a.has_lock && layout_b.has_lock) << "…and LMDB's lock.mdb beside it";
        expect(gt(layout_a.data_bytes, uint64_t{0}));
        expect(eq(layout_a.meta_magic, kLmdbMagic))
            << "data.mdb page 0 carries LMDB's meta page magic";
        expect(eq(layout_b.meta_magic, kLmdbMagic));
        expect(eq(layout_a.meta_version, kLmdbDataVersion)) << "LMDB's data format version";
        expect(eq(layout_b.meta_version, kLmdbDataVersion));
        // The store lives INSIDE the store directory (the lmdb ColdStorage puts its
        // data.mdb directly there), so the two backends never share a path.
        std::error_code layout_ec;
        expect(!kimix::filesystem::exists(
            dir_a.path() / kimix::filesystem::path{FileColdStore::kDataFileName}, layout_ec))
            << "the environment is not the store directory itself";
        expect(!layout_ec);

        // --- the records ------------------------------------------------------
        auto const records_a = record_bytes(ColdStore{store_a});
        auto const records_b = record_bytes(ColdStore{store_b});
        expect(eq(count_keys(records_a, graph_db::kVertexKeyPrefix), size_t{32}))
            << "one record per vertex after the rebuild";
        expect(gt(count_keys(records_a, graph_db::kEdgeKeyPrefix), size_t{0}))
            << "the edge records ride in the same environment";
        // A save-then-rebuild store and a single-save store hold the SAME records: a full
        // save is a complete snapshot either way, and the rebuild dropped the superseded
        // copies instead of keeping both.
        expect(same_records(records_a, records_b))
            << "two full saves of the same graph produce the same record bytes";
    };

    "file_store_database_backend_parity"_test = [] {
        // Database is what SELECTS a backend at open. The same GDI script through both
        // backends must persist and reload the same state, and the second backend's
        // directory must hold its own graph.xdb environment instead of the LMDB files
        // ColdStorage writes directly into the store directory.
        struct db_outcome {
            size_t vertices{0};
            size_t reload_vertices{0};
            size_t keyed_lookups{0};
            size_t content_matches{0};
            size_t persist_saved{0};
            size_t clean_saved{0};
            uint32_t format_version{0};
            bool graph_file{false};
        };
        auto const run = [](backend_kind kind, kimix::filesystem::path const &dir) {
            db_outcome out;
            graph_db::GraphOptions options;
            options.store_backend = kind == backend_kind::lmdb
                                        ? graph_db::ColdStoreBackend::lmdb
                                        : graph_db::ColdStoreBackend::flat_file;
            graph_db::LabelId node{};
            kimix::vector<kimix::string> const keys{"one", "two", "three"};
            {
                graph_db::Database db;
                if (db.open(dir, options) != graph_db::GdiStatus::ok) {
                    return out;
                }
                if (db.schema().create_label("node", node) != graph_db::GdiStatus::ok) {
                    return out;
                }
                for (auto const &text : keys) {
                    auto const bytes = gtest::to_bytes(text);
                    graph_db::VertexCreateResult created;
                    auto const key = graph_db::VertexKeyView{
                        node, kimix::span<const std::byte>{bytes.data(), bytes.size()}};
                    if (db.create_vertex(key, {}, nullptr, created) != graph_db::GdiStatus::ok) {
                        return out;
                    }
                    if (!db.graph().update_content(created.uid,
                                                   kimix::span<const std::byte>{bytes.data(),
                                                                                bytes.size()})) {
                        return out;
                    }
                }
                graph_db::GraphSerializeStats stats;
                if (db.persist(/*full=*/true, &stats) != graph_db::GdiStatus::ok) {
                    return out;
                }
                out.persist_saved = stats.saved_count;
                out.vertices = db.graph().size();
                out.format_version = stats.format_version;
                out.graph_file = kimix::filesystem::is_directory(store_file(dir));
            }
            {
                graph_db::Database db;
                if (db.open(dir, options) != graph_db::GdiStatus::ok) {
                    return out;
                }
                out.reload_vertices = db.graph().size();
                for (auto const &text : keys) {
                    auto const bytes = gtest::to_bytes(text);
                    auto const key = graph_db::VertexKeyView{
                        node, kimix::span<const std::byte>{bytes.data(), bytes.size()}};
                    kimix::Guid uid;
                    bool found = false;
                    if (db.translate_vertex_id(key, uid, found) != graph_db::GdiStatus::ok ||
                        !found) {
                        continue;
                    }
                    ++out.keyed_lookups;
                    Vertex *vertex = db.graph().find(uid);
                    if (vertex == nullptr) {
                        continue;
                    }
                    auto const content = vertex->content_copy();
                    if (same_bytes(kimix::span<const std::byte>{content.data(), content.size()},
                                   kimix::span<const std::byte>{bytes.data(), bytes.size()})) {
                        ++out.content_matches;
                    }
                }
                graph_db::GraphSerializeStats stats;
                if (db.persist(/*full=*/false, &stats) != graph_db::GdiStatus::ok) {
                    return out;
                }
                out.clean_saved = stats.saved_count;
            }
            return out;
        };

        gtest::temp_dir_guard lmdb_dir;
        gtest::temp_dir_guard file_dir;
        expect(lmdb_dir.ok() && file_dir.ok()) << "temp directories must be creatable";
        if (!lmdb_dir.ok() || !file_dir.ok()) {
            return;
        }
        db_outcome const lmdb = run(backend_kind::lmdb, lmdb_dir.path());
        db_outcome const file = run(backend_kind::flat_file, file_dir.path());

        expect(eq(lmdb.vertices, size_t{3})) << "the lmdb backend holds three vertices";
        expect(eq(file.vertices, size_t{3})) << "the flat_file backend holds three vertices";
        expect(eq(lmdb.persist_saved, size_t{3})) << "a full save writes every vertex";
        expect(eq(file.persist_saved, size_t{3}));
        expect(eq(lmdb.format_version, graph_db::kFormatVersion));
        expect(eq(file.format_version, graph_db::kFormatVersion));
        expect(eq(lmdb.reload_vertices, size_t{3})) << "the reopen loads the store";
        expect(eq(file.reload_vertices, size_t{3})) << "the file backend reloads too";
        expect(eq(lmdb.keyed_lookups, size_t{3})) << "the key index survives the round trip";
        expect(eq(file.keyed_lookups, size_t{3}));
        expect(eq(lmdb.content_matches, size_t{3}));
        expect(eq(file.content_matches, size_t{3}));
        expect(eq(lmdb.clean_saved, size_t{0})) << "a clean reopen writes nothing";
        expect(eq(file.clean_saved, size_t{0}));
        expect(!lmdb.graph_file) << "the lmdb backend knows no graph.xdb directory";
        expect(file.graph_file) << "the second backend writes <dir>\graph.xdb";
    };
    // The cold-store scope (plan T1) - the single test that pins the whole
    // refactor: the environment (and its data.mdb/lock.mdb handles) must not
    // outlive one cold operation. On Windows an OPEN LMDB handle is exactly what
    // makes the store directory unrenameable and undeletable, so the renames below
    // are a platform-visible proof, not a bookkeeping detail.
    "file_store_directory_is_free_between_operations"_test = [] {
        gtest::temp_dir_guard dir;
        gtest::temp_dir_guard parked;
        expect(dir.ok() && parked.ok()) << "temp directories must be creatable";
        if (!dir.ok() || !parked.ok()) {
            return;
        }
        graph_db::Database db;
        expect(eq(db.open(dir.path()), graph_db::GdiStatus::ok));
        graph_db::LabelId node{};
        expect(eq(db.schema().create_label("node", node), graph_db::GdiStatus::ok));
        for (auto const text : {"one", "two", "three"}) {
            auto const bytes = gtest::to_bytes(text);
            graph_db::VertexCreateResult created;
            auto const key = graph_db::VertexKeyView{
                node, kimix::span<const std::byte>{bytes.data(), bytes.size()}};
            expect(eq(db.create_vertex(key, {}, nullptr, created), graph_db::GdiStatus::ok));
        }
        expect(eq(db.persist(/*full=*/true, nullptr), graph_db::GdiStatus::ok));
        expect(db.is_open()) << "the store stays configured even though nothing is mapped";
        // 1. The directory is renameable away and back while no operation runs.
        kimix::filesystem::path const moved =
            parked.path() / kimix::filesystem::path{"moved-store"};
        std::error_code ec;
        kimix::filesystem::rename(dir.path(), moved, ec);
        expect(!ec) << "the store directory must be renameable between operations: "
                    << ec.message();
        kimix::filesystem::rename(moved, dir.path(), ec);
        expect(!ec) << "and renameable back: " << ec.message();
        expect(eq(db.load_store(), graph_db::GdiStatus::ok));
        expect(eq(db.graph().size(), size_t{3})) << "the restored store still loads";
        // 2. Deletable, too. An INCREMENTAL save of a clean graph then writes no
        //    record, so the recreated store is EMPTY: the documented behaviour, not
        //    a silent resurrection of the deleted content.
        kimix::filesystem::remove_all(dir.path(), ec);
        expect(!ec) << "the store directory must be deletable between operations: "
                    << ec.message();
        expect(eq(db.persist(/*full=*/false, nullptr), graph_db::GdiStatus::ok));
        {
            graph_db::Database fresh;
            expect(eq(fresh.open(dir.path()), graph_db::GdiStatus::ok));
            expect(eq(fresh.graph().size(), size_t{0}))
                << "a store deleted from outside comes back empty";
        }
        // 3. A FULL save repopulates it from the in-memory graph (the graph, not
        //    the store, is the authority).
        expect(eq(db.persist(/*full=*/true, nullptr), graph_db::GdiStatus::ok));
        {
            graph_db::Database fresh;
            expect(eq(fresh.open(dir.path()), graph_db::GdiStatus::ok));
            expect(eq(fresh.graph().size(), size_t{3})) << "the full save repopulated the store";
        }
        db.close();
        expect(!db.is_open()) << "close() forgets the configuration";
        expect(!kimix::filesystem::exists(dir.path(), ec) || !ec);
    };

    // The cold-store scope (plan T7): two sessions on ONE directory, interleaved.
    // Each operation takes its own environment, so session B sees A's commit on its
    // next load and vice versa - which is also what makes "another session may open
    // the directory between operations" a documented property instead of an
    // accident of LMDB's reader table.
    "file_store_two_databases_share_a_directory"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok()) << "a temp directory must be creatable";
        if (!dir.ok()) {
            return;
        }
        auto const add_one = [](graph_db::Database &db, graph_db::LabelId label,
                                kimix::string_view text) {
            auto const bytes = gtest::to_bytes(text);
            graph_db::VertexCreateResult created;
            auto const key = graph_db::VertexKeyView{
                label, kimix::span<const std::byte>{bytes.data(), bytes.size()}};
            return db.create_vertex(key, {}, nullptr, created);
        };
        graph_db::Database a;
        expect(eq(a.open(dir.path()), graph_db::GdiStatus::ok));
        graph_db::LabelId node{};
        expect(eq(a.schema().create_label("node", node), graph_db::GdiStatus::ok));
        expect(eq(add_one(a, node, "a1"), graph_db::GdiStatus::ok));
        expect(eq(a.persist(/*full=*/true, nullptr), graph_db::GdiStatus::ok));
        graph_db::Database b;
        expect(eq(b.open(dir.path()), graph_db::GdiStatus::ok));
        expect(eq(b.graph().size(), size_t{1})) << "the second session loads the first one's commit";
        // A commits again; B sees it on its next load, not only on its next open().
        expect(eq(add_one(a, node, "a2"), graph_db::GdiStatus::ok));
        expect(eq(a.persist(/*full=*/false, nullptr), graph_db::GdiStatus::ok));
        expect(eq(b.load_store(), graph_db::GdiStatus::ok));
        expect(eq(b.graph().size(), size_t{2})) << "a load sees the other session's commit";
        // ...and back the other way round.
        expect(eq(add_one(b, node, "b1"), graph_db::GdiStatus::ok));
        expect(eq(b.persist(/*full=*/false, nullptr), graph_db::GdiStatus::ok));
        expect(eq(a.load_store(), graph_db::GdiStatus::ok));
        expect(eq(a.graph().size(), size_t{3})) << "the first session sees the second one's commit";
    };

}