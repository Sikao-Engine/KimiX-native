// Test for the in-memory graph database (src/graph_db: graph.h, vertex.h).
// This test covers:
// - create / find / remove vertex, size and contains
// - cascade remove: the removed vertex is unlinked from every neighbour's
//   pointer sets and guid lists (no dangling edge survives)
// - connect / disconnect updates BOTH directions and is idempotent-safe
// - roots(): a root is a vertex nothing depends on (empty _depended), so the
//   sinks in dependency order — the plan's §9 definition
// - content update while N reader threads hold the vertex SHARED lock
//   (threaded smoke test: no crash, every read is one of the written values)
// - the single-writer debug contract (debug-only writer-thread-id check)
// - has_path / shortest_path on a known DAG (BFS, shortest = fewest hops)
// - a dependency cycle does not hang either traversal
// - cache-line sanity: the per-vertex lock region is 64-byte separated
//
// Everything runs in main() scope per the project's Boost.UT convention.

#include "ut/ut.hpp"
#include "graph_db_test_util.h"

#include <graph_db/graph.h>
#include <graph_db/graph_db.h>
#include <graph_db/utils.h>
#include <graph_db/vertex.h>

#include <core/guid.h>
#include <core/kimix_core.h>
#include <core/md5.h>
#include <core/stl/vector.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using graph_db::Graph;
using graph_db::Vertex;
namespace gtest = kimix::graph_db::test;

namespace tests_unit_graph_db_test_graph_db {

// Builds a byte vector with a single distinct byte, used as a vertex content.
kimix::vector<std::byte> one_byte(uint8_t value) {
    kimix::vector<std::byte> out;
    out.push_back(static_cast<std::byte>(value));
    return out;
}

} // namespace tests_unit_graph_db_test_graph_db
using namespace tests_unit_graph_db_test_graph_db;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "graph_create_find_remove"_test = [] {
        Graph graph;
        expect(graph.empty());
        expect(eq(graph.size(), size_t{0}));

        auto content = gtest::to_bytes("hello");
        Vertex *vertex = graph.create_vertex(content);
        expect(vertex != nullptr);
        expect(eq(graph.size(), size_t{1}));
        expect(!graph.empty());

        kimix::Guid guid = vertex->guid();
        expect(graph.contains(guid));
        expect(graph.find(guid) == vertex) << "find must return the stored pointer";
        expect(graph.find(kimix::Guid{true}) == nullptr) << "unknown guid => nullptr";

        auto read_back = vertex->content_copy();
        expect(gtest::blob_equal(read_back, "hello"));

        auto removed = graph.remove_vertex(guid);
        expect(removed != nullptr) << "remove_vertex returns the removed vertex";
        expect(removed->guid() == guid);
        expect(eq(graph.size(), size_t{0}));
        expect(!graph.contains(guid));
        expect(graph.find(guid) == nullptr);
        expect(graph.remove_vertex(guid) == nullptr) << "double remove is a no-op";
    };

    "graph_add_vertex_adopts_unique_ptr"_test = [] {
        Graph graph;
        kimix::unique_ptr<Vertex> vertex{new Vertex()};
        kimix::Guid guid = vertex->guid();
        expect(graph.add_vertex(std::move(vertex)));
        expect(eq(graph.size(), size_t{1}));
        expect(graph.find(guid) != nullptr);
        expect(!graph.add_vertex(nullptr)) << "a null vertex is rejected";
    };

    "graph_cascade_remove_unlinks_neighbors"_test = [] {
        Graph graph;
        gtest::known_dag dag = gtest::small_dag(graph);
        expect(eq(graph.size(), size_t{6}));

        // b sits between a (a -> b) and d (b -> d): removing it must clean up


        // both neighbours' link lists (the single adjacency representation,


        // plan v3 §5.2).
        kimix::Guid b_guid = dag.b->guid();
        auto removed = graph.remove_vertex(b_guid);
        expect(removed != nullptr);
        expect(eq(graph.size(), size_t{5}));

        expect(eq(dag.a->depending_count(), size_t{1}))
            << "a keeps only its edge to c";
        expect(!dag.a->depends_on(b_guid)) << "a's link list drops b";

        for (auto const &link : dag.a->out_links()) {

        expect(link.neighbour != b_guid) << "no stale edge to b may survive";

        }

        expect(eq(dag.d->depended_count(), size_t{2}))
            << "d keeps c and f as dependents";
        expect(!dag.d->depends_on(b_guid))
            << "d never depended on b, so its own outgoing list is unaffected";
        for (auto const &link : dag.d->in_links()) {

        expect(link.neighbour != b_guid) << "no stale edge from b may survive";

        }

        

        // The count accessors and the link lists are one view now (§5.2).

        expect(eq(dag.a->depending_count(), dag.a->out_links().size()));

        expect(eq(dag.d->depended_count(), dag.d->in_links().size()));

        // Traversals see the new shape: a no longer reaches d through b (its
        // only remaining edge is a -> c -> d, which still reaches it), so use
        // the isolated vertex for the negative case.
        expect(!graph.has_path(dag.e->guid(), dag.d->guid()))
            << "the isolated vertex reaches nothing";
    };

    "graph_connect_disconnect_both_directions"_test = [] {
        Graph graph;
        Vertex *from = graph.create_vertex(gtest::to_bytes("from"));
        Vertex *to = graph.create_vertex(gtest::to_bytes("to"));

        expect(graph.connect(from->guid(), to->guid()));
        expect(eq(from->depending_count(), size_t{1}));
        expect(eq(to->depended_count(), size_t{1}));
        expect(from->depends_on(to->guid()));
        expect(eq(from->out_links().size(), size_t{1}));

        expect(eq(to->in_links().size(), size_t{1}));

        expect(from->out_links().front().neighbour == to->guid());

        expect(to->in_links().front().neighbour == from->guid());

        expect(!graph.connect(from->guid(), to->guid()))
            << "a duplicate edge is rejected";
        expect(!graph.connect(from->guid(), from->guid()))
            << "a self edge is rejected";
        expect(!graph.connect(from->guid(), kimix::Guid{true}))
            << "an unknown endpoint is rejected";
        expect(eq(from->depending_count(), size_t{1})) << "rejections change nothing";

        // Direction: dependencies() is outgoing, dependents() incoming,
        // neighbors() is the undirected union.
        auto deps = graph.dependencies(from->guid());
        expect(eq(deps.size(), size_t{1}));
        expect(deps.front() == to->guid());
        auto dependents = graph.dependents(to->guid());
        expect(eq(dependents.size(), size_t{1}));
        expect(dependents.front() == from->guid());
        auto neighbors = graph.neighbors(from->guid());
        expect(eq(neighbors.size(), size_t{1}));
        auto to_neighbors = graph.neighbors(to->guid());
        expect(eq(to_neighbors.size(), size_t{1}));

        expect(graph.disconnect(from->guid(), to->guid()));
        expect(eq(from->depending_count(), size_t{0}));
        expect(eq(to->depended_count(), size_t{0}));expect(from->out_links().empty());

        expect(to->in_links().empty());
        expect(!graph.disconnect(from->guid(), to->guid()))
            << "a second disconnect is a no-op";
    };

    "graph_roots_are_vertices_nothing_depends_on"_test = [] {
        Graph graph;
        gtest::known_dag dag = gtest::small_dag(graph);

        // In the known DAG a -> {b, c}, {b, c} -> d, f -> d, e isolated.
        // Nothing depends on a, e or f, so those are the roots.
        auto roots = graph.roots();
        expect(eq(roots.size(), size_t{3}));
        kimix::unordered_set<kimix::Guid> root_guids;
        for (auto *vertex : roots) {
            root_guids.insert(vertex->guid());
        }
        expect(root_guids.count(dag.a->guid()) == 1) << "a has no incoming edge";
        expect(root_guids.count(dag.e->guid()) == 1) << "e is isolated";
        expect(root_guids.count(dag.f->guid()) == 1) << "f has no incoming edge";
        expect(root_guids.count(dag.d->guid()) == 0)
            << "d is depended on by b, c and f, so it is not a root";
        expect(root_guids.count(dag.b->guid()) == 0);
        expect(root_guids.count(dag.c->guid()) == 0);

        // Adding an incoming edge removes a vertex from the root set; removing
        // it puts the vertex back.
        expect(graph.connect(dag.d->guid(), dag.a->guid()));
        auto after_connect = graph.roots();
        expect(eq(after_connect.size(), size_t{2}));
        expect(graph.disconnect(dag.d->guid(), dag.a->guid()));
        expect(eq(graph.roots().size(), size_t{3}));

        Graph empty_graph;
        expect(empty_graph.roots().empty());
    };

    "graph_has_path_and_shortest_path_on_dag"_test = [] {
        Graph graph;
        gtest::known_dag dag = gtest::small_dag(graph);

        // a reaches b, c and d (through either of them); f reaches d only.
        expect(graph.has_path(dag.a->guid(), dag.d->guid()));
        expect(graph.has_path(dag.a->guid(), dag.b->guid()));
        expect(graph.has_path(dag.f->guid(), dag.d->guid()));
        expect(!graph.has_path(dag.d->guid(), dag.a->guid()))
            << "edges are directed: d reaches nothing";
        expect(!graph.has_path(dag.e->guid(), dag.d->guid()));
        expect(graph.has_path(dag.a->guid(), dag.a->guid()))
            << "a vertex reaches itself (zero-length path)";
        expect(!graph.has_path(dag.a->guid(), kimix::Guid{true}))
            << "an unknown endpoint is not reachable";

        // shortest_path is the fewest-hop route and includes both endpoints.
        auto path = graph.shortest_path(dag.a->guid(), dag.d->guid());
        expect(eq(path.size(), size_t{3})) << "a -> b -> d (or a -> c -> d)";
        expect(path.front() == dag.a->guid());
        expect(path.back() == dag.d->guid());
        expect(path[1] == dag.b->guid() || path[1] == dag.c->guid());

        auto direct = graph.shortest_path(dag.a->guid(), dag.b->guid());
        expect(eq(direct.size(), size_t{2}));
        expect(direct.front() == dag.a->guid());
        expect(direct.back() == dag.b->guid());

        auto self = graph.shortest_path(dag.a->guid(), dag.a->guid());
        expect(eq(self.size(), size_t{1}));
        expect(self.front() == dag.a->guid());

        auto none = graph.shortest_path(dag.d->guid(), dag.a->guid());
        expect(none.empty()) << "an unreachable target yields an empty path";
        auto missing = graph.shortest_path(dag.a->guid(), kimix::Guid{true});
        expect(missing.empty());
    };

    "graph_cycle_does_not_hang_traversal"_test = [] {
        Graph graph;
        Vertex *a = graph.create_vertex(gtest::to_bytes("a"));
        Vertex *b = graph.create_vertex(gtest::to_bytes("b"));
        Vertex *c = graph.create_vertex(gtest::to_bytes("c"));

        // a -> b -> c -> a: a full dependency cycle plus an extra shortcut.
        expect(graph.connect(a->guid(), b->guid()));
        expect(graph.connect(b->guid(), c->guid()));
        expect(graph.connect(c->guid(), a->guid()));
        expect(graph.connect(a->guid(), c->guid()));

        // Without a visited set both traversals would spin forever.
        expect(graph.has_path(a->guid(), c->guid()));
        expect(graph.has_path(c->guid(), b->guid()));
        expect(graph.has_path(b->guid(), a->guid()));

        auto path = graph.shortest_path(b->guid(), c->guid());
        expect(eq(path.size(), size_t{2})) << "b -> c is the direct hop";
        auto long_path = graph.shortest_path(c->guid(), b->guid());
        expect(eq(long_path.size(), size_t{3})) << "c -> a -> b";
        expect(long_path.front() == c->guid());
        expect(long_path.back() == b->guid());

        // A cycle must not become its own root set: each vertex is depended on.
        expect(graph.roots().empty()) << "every vertex of a cycle has an incoming edge";
    };

    "graph_update_content_readers_shared_writer_unique"_test = [] {
        Graph graph;
        // One hot vertex: the writer flips its content between two values while
        // N readers hold the SHARED vertex lock and must never observe a torn or
        // empty value.
        Vertex *hot = graph.create_vertex(one_byte(0));
        kimix::Guid hot_guid = hot->guid();

        // A second, untouched vertex keeps the readers busy on a different line
        // (that is the false-sharing property the design claims).
        Vertex *cold = graph.create_vertex(one_byte(0x7f));
        kimix::Guid cold_guid = cold->guid();

        constexpr int reader_count = 4;
        std::atomic<bool> stop{false};
        std::atomic<int> started{0};
        std::atomic<uint64_t> reads{0};
        std::atomic<uint64_t> bad{0};
        kimix::vector<std::thread> readers;
        for (int i = 0; i < reader_count; ++i) {
            readers.emplace_back([&] {
                started.fetch_add(1, std::memory_order_release);
                while (!stop.load(std::memory_order_acquire)) {
                    kimix::vector<std::byte> bytes;
                    hot->content(bytes); // vertex SHARED lock
                    if (bytes.size() != 1) {
                        bad.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        auto value = std::to_integer<uint8_t>(bytes.front());
                        if (value != 0 && value != 0xff) {
                            bad.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    // The untouched vertex must keep reading its own value.
                    kimix::vector<std::byte> other;
                    cold->content(other);
                    if (other.size() != 1 ||
                        std::to_integer<uint8_t>(other.front()) != 0x7f) {
                        bad.fetch_add(1, std::memory_order_relaxed);
                    }
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        // Wait until every reader is inside its loop before writing, so the
        // smoke test measures real overlap, not thread-start latency.
        while (started.load(std::memory_order_acquire) < reader_count) {
            std::this_thread::yield();
        }

        bool all_updates_ok = true;
        for (int i = 0; i < 2000; ++i) {
            uint8_t value = (i % 2 == 0) ? 0xff : 0x00;
            auto bytes = one_byte(value);
            if (!graph.update_content(hot_guid, bytes)) {
                all_updates_ok = false;
            }
        }
        expect(all_updates_ok) << "every content update of an existing vertex succeeds";
        expect(!graph.update_content(kimix::Guid{true}, one_byte(1)))
            << "an unknown guid is reported, not crashed on";

        stop.store(true, std::memory_order_release);
        for (auto &thread : readers) {
            thread.join();
        }
        expect(eq(bad.load(), uint64_t{0})) << "no reader saw a torn value";
        expect(reads.load() > 0) << "the readers really ran";
        kimix::vector<std::byte> cold_bytes = graph.find(cold_guid)->content_copy();
        expect(eq(cold_bytes.size(), size_t{1}));
        expect(eq(std::to_integer<uint8_t>(cold_bytes.front()), uint8_t{0x7f}))
            << "the untouched vertex kept its value";
    };

    "graph_single_writer_contract"_test = [] {
        // The contract is enforced by Graph::_check_writer(): the first
        // structural write records its thread, a later structural write from a
        // different thread is REFUSED (false / nullptr) and, in -m debug, also
        // hits an assert. That assert terminates the process by design, so this
        // test verifies the refusal indirectly — through is_writer_thread() from
        // a foreign thread — instead of making the debug assert the test result.
        // The "return false" half is what a release build observes.
        Graph graph;
        expect(graph.is_writer_thread()) << "no writer recorded yet";
        Vertex *vertex = graph.create_vertex(gtest::to_bytes("v"));
        expect(graph.is_writer_thread()) << "this thread is now the writer";

        std::atomic<bool> other_thread_is_writer{true};
        std::thread other([&] {
            other_thread_is_writer.store(graph.is_writer_thread(),
                                         std::memory_order_release);
        });
        other.join();
        expect(!other_thread_is_writer.load())
            << "a foreign thread is not the recorded structural writer";

        // update_content is NOT a structural write: another thread may do it.
        kimix::Guid guid = vertex->guid();
        std::atomic<bool> updated{false};
        std::thread updater([&] {
            auto bytes = gtest::to_bytes("from-another-thread");
            updated.store(graph.update_content(guid, bytes), std::memory_order_release);
        });
        updater.join();
        expect(updated.load()) << "a level-2 content update crosses threads freely";
        expect(gtest::blob_equal(vertex->content_copy(), "from-another-thread"));
        expect(graph.is_writer_thread()) << "the content update did not change the writer";

        graph.reset_writer_thread();
        expect(graph.is_writer_thread()) << "after the reset any thread may write";
    };

    "graph_dirty_tracking"_test = [] {
        Graph graph;
        gtest::known_dag dag = gtest::small_dag(graph);
        expect(eq(graph.dirty_vertices().size(), size_t{6}))
            << "fresh vertices are dirty";

        graph.clear_dirty();
        expect(graph.dirty_vertices().empty());

        auto bytes = gtest::to_bytes("changed");
        expect(graph.update_content(dag.c->guid(), bytes));
        auto dirty = graph.dirty_vertices();
        expect(eq(dirty.size(), size_t{1}));
        expect(dirty.front().guid == dag.c->guid());
        expect(dirty.front().content_hash == kimix::MD5{kimix::string_view{"changed"}})
            << "the cached hash is recomputed on every write";

        graph.mark_dirty();
        expect(eq(graph.dirty_vertices().size(), size_t{6}));
    };

    "graph_vertex_cache_line_separation"_test = [] {
        // alignas(64) on the vertex mutex: the lock bitfield must sit in a cache
        // line of its own, so two vertices never share it.
        expect(eq(alignof(Vertex), size_t{graph_db::kCacheLineSize}))
            << "Vertex is cache-line aligned";
        expect(eq(graph_db::kCacheLineSize, size_t{64}));

        Graph graph;
        // Several vertices: every pointer is 64-byte aligned, so no two vertex
        // header regions share a cache line.
        kimix::vector<Vertex *> vertices;
        for (int i = 0; i < 8; ++i) {
            vertices.push_back(graph.create_vertex(one_byte(static_cast<uint8_t>(i))));
        }
        for (auto *vertex : vertices) {
            auto address = reinterpret_cast<uintptr_t>(vertex);
            expect(eq(address % graph_db::kCacheLineSize, uintptr_t{0}))
                << "every Vertex allocation is cache-line aligned";
        }
        // The mutex itself is inside the object and the object is a multiple of
        // the cache line, so consecutive objects cannot straddle a line.
        expect(eq(sizeof(Vertex) % graph_db::kCacheLineSize, size_t{0}))
            << "sizeof(Vertex) is a whole number of cache lines";
        uintptr_t const lock0 = reinterpret_cast<uintptr_t>(&vertices[0]->mutex());
        uintptr_t const lock1 = reinterpret_cast<uintptr_t>(&vertices[1]->mutex());
        expect(lock0 != lock1);
        expect((lock1 > lock0 ? lock1 - lock0 : lock0 - lock1) >= graph_db::kCacheLineSize)
            << "two vertices' locks are at least a cache line apart";
    };

    "graph_record_codec_round_trip"_test = [] {
        // utils.h: the little-endian record the cold store writes.
        graph_db::VertexRecord record;
        record.guid = kimix::Guid{true};
        record.content = gtest::to_bytes("payload");
        record.depending_guids = {kimix::Guid{true}, kimix::Guid{true}};
        record.depended_guids = {kimix::Guid{true}};
        record.content_hash = kimix::MD5{kimix::string_view{"payload"}};

        kimix::vector<std::byte> encoded;
        graph_db::encode_vertex_record(record, encoded);
        expect(eq(encoded.size(), graph_db::encoded_record_size(record)));
        expect(encoded.size() >= graph_db::kVertexRecordHeaderSize);
        expect(std::to_integer<char>(encoded[0]) == 'G');
        expect(std::to_integer<char>(encoded[1]) == 'D');
        expect(std::to_integer<char>(encoded[2]) == 'B');
        expect(std::to_integer<char>(encoded[3]) == '1');

        graph_db::VertexRecord decoded;
        expect(graph_db::decode_vertex_record(encoded, decoded));
        expect(decoded.guid == record.guid);
        expect(decoded.content_hash == record.content_hash);
        expect(eq(decoded.depending_guids.size(), size_t{2}));
        expect(eq(decoded.depended_guids.size(), size_t{1}));
        expect(decoded.depending_guids[0] == record.depending_guids[0]);
        expect(decoded.depended_guids[0] == record.depended_guids[0]);
        expect(gtest::blob_equal(decoded.content, "payload"));

        // Corrupt input is reported, never fatal.
        kimix::vector<std::byte> truncated(encoded.begin(),
                                          encoded.begin() + graph_db::kVertexRecordHeaderSize - 1);
        graph_db::VertexRecord scratch;
        expect(!graph_db::decode_vertex_record(truncated, scratch))
            << "a buffer shorter than the header is rejected";

        kimix::vector<std::byte> bad_magic = encoded;
        bad_magic[0] = std::byte{'X'};
        expect(!graph_db::decode_vertex_record(bad_magic, scratch));

        kimix::vector<std::byte> bad_count(encoded.begin(), encoded.end());
        // The n_depending field lives at offset 36; set it to 0xffffffff.
        bad_count[36] = std::byte{0xff};
        bad_count[37] = std::byte{0xff};
        bad_count[38] = std::byte{0xff};
        bad_count[39] = std::byte{0xff};
        expect(!graph_db::decode_vertex_record(bad_count, scratch))
            << "an impossible dependency count is rejected, not allocated";
    };

    "graph_key_scheme"_test = [] {
        kimix::Guid guid{true};
        auto key = graph_db::guid_key(guid);
        expect(eq(key.size(), graph_db::kVertexKeySize));
        expect(std::to_integer<char>(key[0]) == 'v');
        expect(std::to_integer<char>(key[1]) == '/');

        auto parsed = graph_db::guid_from_key(kimix::span<const std::byte>{key.data(), key.size()});
        expect(parsed.has_value());
        expect(*parsed == guid) << "the key round-trips the guid";

        // The little-endian spelling of the guid words, byte for byte.
        auto const &words = guid.to_binary();
        uint8_t expected = static_cast<uint8_t>(words.data0 & 0xffu);
        expect(std::to_integer<uint8_t>(key[2]) == expected);

        expect(!graph_db::guid_from_key({}).has_value());
        kimix::vector<std::byte> wrong_prefix(key.begin(), key.end());
        wrong_prefix[0] = std::byte{'x'};
        expect(!graph_db::guid_from_key(
                    kimix::span<const std::byte>{wrong_prefix.data(), wrong_prefix.size()})
                    .has_value());
        expect(graph_db::kMetaRootKey[0] != 'v')
            << "the reserved meta key never collides with a vertex key";
    };

    "graph_cold_storage_round_trip"_test = [] {
        // ColdStorage is the LMDB glue the later serializer drives. Here it only
        // proves the wrapper works: write a batch, read it back, delete it.
        gtest::temp_dir_guard dir;
        expect(dir.ok()) << "a temp directory must be creatable";
        if (!dir.ok()) {
            return;
        }
        expect(graph_db::ensure_dir(dir.path())) << "ensure_dir is idempotent";

        graph_db::ColdStorage cold;
        expect(!cold.valid()) << "a default-constructed store is not open";
        expect(cold.open(dir.path()));
        expect(cold.valid());
        expect(cold.path() == dir.path());

        graph_db::VertexRecord record;
        record.guid = kimix::Guid{true};
        record.content = gtest::to_bytes("cold");
        record.content_hash = kimix::MD5{kimix::string_view{"cold"}};
        kimix::vector<graph_db::VertexRecord> batch;
        batch.push_back(record);
        expect(cold.save_records(std::move(batch)));

        graph_db::VertexRecord read_back;
        expect(cold.read_record(record.guid, read_back));
        expect(read_back.guid == record.guid);
        expect(gtest::blob_equal(read_back.content, "cold"));
        expect(!cold.read_record(kimix::Guid{true}, read_back))
            << "an unknown guid is a miss, not an abort";

        kimix::vector<graph_db::ColdRecord> all;
        expect(cold.load_all_records(all));
        expect(eq(all.size(), size_t{1}));
        expect(eq(all.front().key.size(), graph_db::kVertexKeySize));

        kimix::vector<graph_db::VertexRecord> decoded;
        size_t skipped = 99;
        expect(cold.load_vertex_records(decoded, &skipped));
        expect(eq(decoded.size(), size_t{1}));
        expect(eq(skipped, size_t{0}));

        expect(cold.remove_record(record.guid));
        kimix::vector<graph_db::VertexRecord> empty;
        expect(cold.load_vertex_records(empty));
        expect(empty.empty());

        cold.close();
        expect(!cold.valid());
        expect(!cold.load_all_records(all)) << "a closed store reports false";
    };

    "graph_vertex_alignment_and_new"_test = [] {
        // The over-aligned operator new/delete round-trips through mimalloc.
        for (int i = 0; i < 16; ++i) {
            auto *vertex = new Vertex(kimix::Guid{true}, gtest::to_bytes("x"));
            expect(reinterpret_cast<uintptr_t>(vertex) % graph_db::kCacheLineSize == 0);
            expect(vertex->content_size() == 1);
            delete vertex;
        }
        auto *empty = new Vertex();
        expect(bool{empty->guid()}) << "the default ctor generates a guid";
        expect(empty->content_size() == 0);
        delete empty;
    };
}