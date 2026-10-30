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
// - the single-writer contract: one structural write at a time (overlap
//   refused), sequential cross-thread writes allowed, update_content exempt
// - has_path / shortest_path on a known DAG (BFS, shortest = fewest hops)
// - a dependency cycle does not hang either traversal
// - cache-line sanity: the per-vertex lock region is 64-byte separated
// - the dirty INDEX (VertexDirtySink): every entry point that can set a dirty flag
//   is cross-checked against the full O(V) scan, so no record can silently drop out
//   of the next incremental save
//
// Everything runs in main() scope per the project's Boost.UT convention.

#include "ut/ut.hpp"
#include "ut/ut.hpp"
#include "graph_db_test_util.h"
#include <graph_db/graph.h>
#include <graph_db/graph_db.h>
#include <graph_db/utils.h>
#include <graph_db/vertex.h>
#include <core/fiber.h>
#include <core/guid.h>
#include <core/kimix_core.h>
#include <core/md5.h>
#include <core/stl/unordered_map.h> // kimix::unordered_set: the dirty-index cross-check
#include <core/stl/vector.h>
#include <core/stl/vector.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using graph_db::GdiStatus;
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
    // FIRST, before any suite can run: bind a scheduler for the whole process,
    // so the write-lease fiber test below really executes graph writes on worker
    // fibers (and any future fiber use in the suites has a pool). Per the fiber
    // skill's root-main pattern the scheduler outlives every test.
    kimix::fiber::scheduler sched{4u};
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
        // The rule is enforced by Graph's write lease: ONE structural write at a
        // time — NOT one thread forever (the ABI-8 feedback, bug 3: managed async
        // code serializes every call but resumes on whichever pool thread is
        // free, so strictly sequential writes from different threads must be
        // legal). A write that OVERLAPS an in-flight write from another thread
        // is refused (error_state in release, an assert in debug); the refusal
        // cannot be exercised deterministically here, so this test proves the
        // allowed side: sequential cross-thread writes, same-thread reentrancy
        // (a public write composing another public write, e.g. free_vertex ->
        // remove_vertex), and the update_content exception.
        Graph graph;
        expect(graph.is_writer_thread()) << "no write in flight: any thread may write";
        Vertex *vertex = graph.create_vertex(gtest::to_bytes("v"));
        expect(vertex != nullptr);
        expect(graph.is_writer_thread()) << "this thread holds the right while writing";

        // A strictly sequential write from ANOTHER thread (the first call has
        // returned, nothing overlaps) is legal under the new contract.
        std::atomic<Vertex *> foreign{nullptr};
        std::thread other([&] { foreign.store(graph.create_vertex(gtest::to_bytes("w"))); });
        other.join();
        expect(foreign.load() != nullptr)
            << "a sequential structural write from another thread is allowed";
        expect(eq(graph.size(), size_t{2}));
        expect(graph.is_writer_thread())
            << "no write is in flight once the foreign call returned";

        // update_content is NOT a structural write: another thread may do it even
        // while this thread is the recorded writer.
        kimix::Guid guid = vertex->guid();
        std::atomic<bool> updated{false};
        std::thread updater([&] {
            auto bytes = gtest::to_bytes("from-another-thread");
            updated.store(graph.update_content(guid, bytes), std::memory_order_release);
        });
        updater.join();
        expect(updated.load()) << "a level-2 content update crosses threads freely";
        expect(gtest::blob_equal(vertex->content_copy(), "from-another-thread"));
        expect(graph.is_writer_thread()) << "the content update took no write right";

        // Same-thread reentrancy: free_vertex composes remove_vertex, each a
        // public structural write entry. Depth counting must let the inner call
        // through — a plain in-flight flag would refuse it.
        expect(eq(graph.free_vertex(vertex->guid(), /*guarded=*/false), GdiStatus::ok));
        expect(eq(graph.size(), size_t{1})) << "the composed cascade removed exactly v";

        // Retained for source compatibility: with the overlap-based rule there
        // is no recorded owner between calls, so the reset is a no-op.
        graph.reset_writer_thread();
        expect(graph.is_writer_thread());
    };

    "graph_refused_write_lease_is_a_clean_noop"_test = [] {
        // The refusal side of the single-writer contract (ABI-8 follow-up,
        // issue 1): while one execution flow holds a write lease, another
        // flow's attempt is refused. The refused lease owns NOTHING, so
        // destroying it must not touch the holder's bookkeeping — the old
        // ~WriteLease released unconditionally, which (1) dropped the holder's
        // depth to 0 while its write was still in flight, so a third flow
        // could take a lease and overlap it, and (2) wrapped the uint32_t
        // depth to UINT32_MAX when the holder's own lease ended, refusing
        // every future lease forever. The seam takes a lease exactly like a
        // structural entry, minus the debug print/assert (a deterministic
        // refusal is a test construction, not a caller bug).
#ifndef NDEBUG
        // The seam itself is DEBUG ONLY (graph.h): an NDEBUG library does not
        // contain test_write_lease(), so a release run has no way to hold the
        // write right across a deterministic refusal. The contract it pins is
        // still exercised below by the ordinary write/refuse paths.
        Graph graph;
        expect(graph.create_vertex(gtest::to_bytes("seed")) != nullptr);

        {
            auto held = graph.test_write_lease(/*assert_on_refusal=*/true);
            expect(bool(held)) << "the first flow takes the lease";

            // Second flow, overlapping: refused. Its lease is a clean no-op.
            std::atomic<bool> refused{false};
            std::thread other([&] {
                auto denied = graph.test_write_lease(/*assert_on_refusal=*/false);
                refused.store(!bool(denied), std::memory_order_release);
            });
            other.join();
            expect(refused.load()) << "an overlapping write from another flow is refused";

            // The refused lease going out of scope must NOT have released the
            // holder's right: a THIRD overlapping flow is still refused, and
            // no flow other than the holder may report itself as the writer.
            // (With the unconditional-release bug the refused lease already
            // dropped the depth to 0 here, so this flow got IN and any thread
            // reported writable.)
            std::atomic<bool> third_refused{false};
            std::atomic<bool> third_sees_free{false};
            std::thread third([&] {
                auto denied = graph.test_write_lease(/*assert_on_refusal=*/false);
                third_refused.store(!bool(denied), std::memory_order_release);
                third_sees_free.store(graph.is_writer_thread(), std::memory_order_release);
            });
            third.join();
            expect(third_refused.load())
                << "the holder's lease survives the refused lease's destruction";
            expect(!third_sees_free.load())
                << "no foreign flow sees the graph as writable mid-lease";
        }

        // Both leases have returned: a normal structural write succeeds. (With
        // the bug the depth wrapped to UINT32_MAX at the holder's release and
        // this write would have been refused.)
        expect(graph.is_writer_thread());
        expect(graph.create_vertex(gtest::to_bytes("after")) != nullptr)
            << "a write after both leases returned succeeds";
        expect(eq(graph.size(), size_t{2}));
#endif // NDEBUG
    };

    "graph_write_lease_is_fiber_safe"_test = [] {
        // The lease keys on the FIBER (kimix::fiber::current_execution_id()),
        // never on std::this_thread::get_id(): a fiber migrates across the pool's
        // worker OS threads at every yield, so an OS-thread owner key both refuses
        // the migrated holder's own inner call and lets a second flow on the former
        // thread compare "equal" and write concurrently. The observable contract
        // here: a structural write issued from INSIDE a worker fiber task is a
        // strictly sequential write — the previous write returned before the
        // fan-out started — and must be accepted. (A yield WITHIN one lease cannot
        // happen today — every structural path is lock-only — which is exactly why
        // the key must be the fiber: it stays correct the day one does yield.)
        expect(kimix::fiber::is_bound());
        expect(ge(kimix::fiber::worker_thread_count(), 2u))
            << "the fan-out must really run on worker fibers, not inline";

        Graph graph;
        expect(graph.create_vertex(gtest::to_bytes("from-the-main-fiber")) != nullptr);

        // Several sequential writes, each from a DIFFERENT worker fiber task.
        kimix::vector<Vertex *> made;
        made.reserve(8);
        for (uint32_t round = 0; round < 8; ++round) {
            std::atomic<Vertex *> created{nullptr};
            kimix::fiber::parallel(16u, [&](uint32_t id) noexcept {
                if (id == 0) {
                    created.store(graph.create_vertex(gtest::to_bytes("from-a-worker-fiber")),
                                  std::memory_order_release);
                }
            });
            Vertex *vertex = created.load(std::memory_order_acquire);
            expect(vertex != nullptr)
                << "a sequential structural write from a worker fiber is accepted (round "
                << round << ")";
            made.push_back(vertex);
        }
        expect(eq(graph.size(), size_t{9}));

        // The fiber-held lease composes with the public cascade exactly like a
        // thread-held one: free_vertex -> remove_vertex reentry at depth 2.
        expect(eq(graph.free_vertex(made.front()->guid(), /*guarded=*/false), GdiStatus::ok));
        expect(eq(graph.size(), size_t{8}));
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

    "graph_dirty_index_matches_scan"_test = [] {
        // The dirty INDEX (VertexDirtySink: vertex.h/graph.h) replaced the O(V) scan
        // dirty_vertices() used to run, so the one thing that must never happen is an
        // index entry going MISSING: the next incremental save would silently drop
        // that record. Every entry point that can set a dirty flag is exercised here
        // and followed by the cross-check "dirty_vertices().size() == a full scan".
        // A STALE entry is the harmless direction (the collect re-checks the flag and
        // skips it), which is why the cross-check is a size/scan equality and not an
        // exact guid list — a mutation that dirties a record through a path I did not
        // expect still fails here, it just fails with a count.
        //
        // The SCAN itself is a DEBUG-ONLY seam (NDEBUG builds do not contain it), so a
        // release run verifies the same thing through dirty_vertices() — the entry
        // point the save path consumes — and the seam only adds the reconciliation.
        Graph graph;
        auto check = [&graph](char const *where) {
            kimix::vector<graph_db::DirtyVertex> const dirty = graph.dirty_vertices();
#ifndef NDEBUG
            expect(eq(dirty.size(), graph.test_count_dirty_vertices_by_scan()))
                << where << ": the dirty index must hold every dirty record";
#endif
            kimix::unordered_set<kimix::Guid> seen;
            bool sound = true;
            for (auto const &item : dirty) {
                sound = seen.insert(item.guid).second && sound;
                sound = item.vertex != nullptr && item.vertex->is_dirty() && sound;
                sound = item.guid == item.vertex->guid() && sound;
            }
            expect(sound) << where
                          << ": each record is reported once, is still dirty, and "
                             "carries its own guid";
        };

        // --- creation: a fresh vertex starts dirty, so it must be indexed ---------
        Vertex *a = graph.create_vertex(one_byte(1));
        Vertex *b = graph.create_vertex(one_byte(2));
        Vertex *c = graph.create_vertex(one_byte(3));
        expect(a != nullptr && b != nullptr && c != nullptr);
        check("create_vertex");
        expect(eq(graph.dirty_vertices().size(), size_t{3}));

        // --- clear_dirty empties both the flags and the index --------------------
        graph.clear_dirty();
        check("clear_dirty");
        expect(graph.dirty_vertices().empty());
#ifndef NDEBUG
        expect(eq(graph.test_count_dirty_vertices_by_scan(), size_t{0}))
            << "the debug scan agrees: nothing is dirty";
#endif

        // --- level-2 content writes ---------------------------------------------
        expect(graph.update_content(a->guid(), one_byte(4)));
        check("Graph::update_content");
        expect(eq(graph.dirty_vertices().size(), size_t{1}))
            << "update_content dirties exactly the vertex it writes";
        graph.clear_dirty();

        b->set_content(gtest::to_bytes("direct"));
        check("Vertex::set_content");
        expect(eq(graph.dirty_vertices().size(), size_t{1}))
            << "a direct level-2 write reaches the index too";
        graph.clear_dirty();

        b->touch();
        check("Vertex::touch");
        graph.clear_dirty();

        // --- typed payload (vertex lock): add / set / put / remove --------------
        expect(b->add_property(graph_db::PropNameId{7}, graph_db::PropertyValue::make_int64(1)));
        check("add_property");
        expect(eq(graph.dirty_vertices().size(), size_t{1}));
        graph.clear_dirty();

        expect(b->set_property(graph_db::PropNameId{7}, graph_db::PropertyValue::make_int64(2)));
        check("set_property");
        graph.clear_dirty();

        expect(b->put_property(graph_db::PropNameId{7}, graph_db::PropertyValue::make_int64(3)));
        check("put_property (replace)");
        graph.clear_dirty();

        // put_property that INSERTS is the same write path (it returns false).
        expect(!b->put_property(graph_db::PropNameId{8}, graph_db::PropertyValue::make_int64(4)));
        check("put_property (insert)");
        graph.clear_dirty();

        graph_db::PropNameId const names[]{graph_db::PropNameId{7}, graph_db::PropNameId{8}};
        expect(eq(b->remove_properties(kimix::span<graph_db::PropNameId const>{names, 2}),
                  size_t{2}));
        check("remove_properties");
        graph.clear_dirty();

        expect(b->add_label(graph_db::LabelId{3}));
        check("add_label");
        graph.clear_dirty();

        expect(b->remove_label(graph_db::LabelId{3}));
        check("remove_label");
        graph.clear_dirty();

        // A REFUSED payload write changes nothing, so it must leave both the flag
        // and the index alone (removing an absent label is a no-op, not a write).
        expect(!b->remove_label(graph_db::LabelId{4}));
        check("remove_label (absent)");
        expect(graph.dirty_vertices().empty())
            << "a refused payload write must not dirty the record";

        // --- structural writes: the adjacency lives in the endpoint records ------
        expect(graph.connect(a->guid(), b->guid()));
        check("connect");
        // connect fills a's OUT list and b's IN list, and both lists are part of
        // their owner's serialized record, so BOTH endpoints are dirty.
        expect(eq(graph.dirty_vertices().size(), size_t{2}));
        graph.clear_dirty();

        graph_db::EdgeId ab;
        expect(eq(graph.edge_create(a->guid(), c->guid(), graph_db::EdgeDirectionType::directed,
                                    {}, nullptr, nullptr, ab),
                  GdiStatus::ok));
        check("edge_create");
        expect(eq(graph.dirty_vertices().size(), size_t{2}))
            << "edge_create dirties the two endpoints it links";
        graph.clear_dirty();

        // A direction change touches the edge RECORD only: no adjacency moved, so
        // no vertex is dirty and the index must stay empty.
        expect(eq(graph.edge_set_direction(ab, graph_db::EdgeDirectionType::undirected),
                  GdiStatus::ok));
        check("edge_set_direction");
        expect(graph.dirty_vertices().empty())
            << "an edge-record-only write must not dirty a vertex";

        // A payload write on an edge is the same: record only.
        expect(eq(graph.edge_add_label(ab, graph_db::LabelId{5}), GdiStatus::ok));
        check("edge_add_label");
        expect(graph.dirty_vertices().empty());
        expect(eq(graph.edge_add_property(ab, graph_db::PropNameId{9},
                                         graph_db::PropertyValue::make_int64(11)),
                  GdiStatus::ok));
        check("edge_add_property");
        expect(graph.dirty_vertices().empty());

        // An endpoint move re-links the adjacency. _move_edge_endpoint removes the
        // link from BOTH old endpoints and re-adds it to BOTH new ones (the
        // remove-by-id/re-add shape that keeps parallel edges and self loops
        // correct), so it dirties three records here: the old origin, the
        // unchanged-but-relinked target, and the new origin.
        expect(eq(graph.edge_set_origin(ab, b->guid()), GdiStatus::ok));
        check("edge_set_origin");
        expect(eq(graph.dirty_vertices().size(), size_t{3}))
            << "a move dirties both old endpoints and both new ones (3 distinct here)";
        graph.clear_dirty();

        Vertex *d = graph.create_vertex(one_byte(5));
        expect(d != nullptr);
        graph.clear_dirty();
        expect(eq(graph.edge_set_target(ab, d->guid()), GdiStatus::ok));
        check("edge_set_target");
        expect(eq(graph.dirty_vertices().size(), size_t{3}))
            << "same remove-then-re-add shape: both old endpoints plus the new target";
        graph.clear_dirty();

        // edge_free unlinks both endpoints (the b -> d edge is ab after the moves).
        expect(eq(graph.edge_free(ab), GdiStatus::ok));
        check("edge_free");
        expect(eq(graph.dirty_vertices().size(), size_t{2}))
            << "freeing an edge dirties the two endpoints that lose a link";
        graph.clear_dirty();

        // --- removal: the victim leaves the index, its neighbours become dirty ---
        kimix::Guid const c_guid = c->guid();
        expect(eq(graph.edge_create(a->guid(), c_guid, graph_db::EdgeDirectionType::directed,
                                    {}, nullptr, nullptr, ab),
                  GdiStatus::ok));
        graph.clear_dirty();
        kimix::unique_ptr<Vertex> victim = graph.remove_vertex(c_guid);
        expect(victim != nullptr);
        check("remove_vertex");
        expect(eq(graph.dirty_vertices().size(), size_t{1}))
            << "only the surviving neighbour that lost a link is dirty";
        // THE detach check: the removed vertex still holds a dirty flag, and a caller
        // may keep marking it dirty, but it must never re-enter this graph's index.
        victim->mark_dirty();
        check("mark_dirty on a removed vertex");
        expect(eq(graph.dirty_vertices().size(), size_t{1}))
            << "a detached vertex must not resurrect an index entry";
        expect(victim->is_dirty()) << "the caller's own flag still works";

        // Re-adoption (the transaction-undo shape) re-attaches the sink.
        expect(graph.add_vertex(std::move(victim)));
        check("re-adopted vertex");
        expect(eq(graph.dirty_vertices().size(), size_t{2}))
            << "a re-adopted dirty vertex is indexed again";
        graph.clear_dirty();

        // --- adoption / loader paths --------------------------------------------
        // Payload assigned BEFORE publication (assign_payload on a vertex no reader
        // can see), then adopted: the vertex starts dirty, so it must be indexed.
        kimix::unique_ptr<Vertex> built{new Vertex(kimix::Guid{true})};
        graph_db::LabelId const built_labels[]{graph_db::LabelId{6}};
        graph_db::PropertyList built_props;
        built_props.push_back(graph_db::PropertyEntry{graph_db::PropNameId{12},
                                                     graph_db::PropertyValue::make_int64(13)});
        built->assign_payload(graph_db::kNoLabel, kimix::span<const std::byte>{},
                              kimix::span<const graph_db::LabelId>{built_labels, 1},
                              built_props);
        kimix::Guid const built_guid = built->guid();
        expect(graph.add_vertex(std::move(built)));
        check("add_vertex (payload assigned before publication)");
        kimix::vector<graph_db::DirtyVertex> const after_add = graph.dirty_vertices();
        expect(eq(after_add.size(), size_t{1}));
        expect(after_add.front().guid == built_guid);
        expect(after_add.front().content_hash == kimix::MD5{})
            << "a vertex whose content was never written carries the empty digest";
        graph.clear_dirty();

        // The LOADER shape: a CLEAN adopted vertex must not enter the index at all
        // (an immediate save after a load writes nothing).
        kimix::unique_ptr<Vertex> clean{new Vertex(kimix::Guid{true})};
        clean->set_content(gtest::to_bytes("loaded"));
        clean->clear_dirty(); // exactly what the store holds
        expect(graph.add_vertex(std::move(clean)));
        check("add_vertex (clean)");
        expect(graph.dirty_vertices().empty())
            << "an adopted clean vertex is not in the write set";

        kimix::unique_ptr<Vertex> load_adopted{new Vertex(kimix::Guid{true})};
        kimix::Guid const load_guid = load_adopted->guid();
        expect(eq(graph.adopt_vertex(std::move(load_adopted)), GdiStatus::ok));
        check("adopt_vertex");
        expect(eq(graph.dirty_vertices().size(), size_t{1}))
            << "the loader's own settle step (clear_dirty) empties the entry";
        kimix::unique_ptr<Vertex> clean_load{new Vertex(kimix::Guid{true})};
        clean_load->clear_dirty();
        expect(eq(graph.adopt_vertex(std::move(clean_load)), GdiStatus::ok));
        check("adopt_vertex (clean)");
        expect(eq(graph.dirty_vertices().size(), size_t{1}));
        expect(graph.dirty_vertices().front().guid == load_guid);

        // --- whole-graph helpers ------------------------------------------------
        graph.mark_dirty();
        check("mark_dirty");
        expect(eq(graph.dirty_vertices().size(), graph.size()))
            << "mark_dirty puts every vertex back in the write set";
        graph.clear_dirty();
        check("clear_dirty (after mark_dirty)");
        expect(graph.dirty_vertices().empty());
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

    "graph_dirty_epoch_keeps_a_write_that_lands_during_a_save"_test = [] {
        // THE lost-update case the dirty EPOCH exists for, made DETERMINISTIC: a save
        // collects a record, a level-2 write lands while it is in flight, and the
        // save's clear must then REFUSE to mark the record clean. The test drives the
        // two save steps by hand (collect -> clear_vertex_dirty_if), which is exactly
        // what GraphSerializer::_write() does around its batch, so it needs no seam
        // and runs in release builds too.
        Graph graph;
        Vertex *vertex = graph.create_vertex(gtest::to_bytes("before"));
        expect(vertex != nullptr);

        kimix::vector<graph_db::DirtyVertex> collected = graph.dirty_vertices();
        expect(eq(collected.size(), size_t{1}));
        uint64_t const epoch = collected.front().dirty_epoch;
        expect(gt(epoch, uint64_t{0})) << "a collected record carries a live stamp";

        // The write that lands mid-save. The record was ALREADY dirty when the save
        // collected it — exactly the case a flag-only protocol loses.
        expect(vertex->add_property(graph_db::PropNameId{41},
                                    graph_db::PropertyValue::make_int64(7)));
        expect(gt(vertex->dirty_epoch(), epoch)) << "every write bumps the stamp";

        expect(!graph.clear_vertex_dirty_if(vertex, epoch))
            << "a record that changed after the collect must not be marked clean";
        expect(vertex->is_dirty()) << "its flag is still set";
        expect(eq(graph.dirty_vertices().size(), size_t{1}))
            << "and the next save still sees it";

        // The retry collects the NEW stamp and its clear goes through.
        collected = graph.dirty_vertices();
        expect(eq(collected.size(), size_t{1}));
        expect(graph.clear_vertex_dirty_if(collected.front().vertex,
                                           collected.front().dirty_epoch))
            << "with no write in between, the clear lands";
        expect(!vertex->is_dirty());
        expect(graph.dirty_vertices().empty()) << "and the record leaves the write set";

        // A record written between two collects is cleared only by the LATEST stamp:
        // a superseded one never wins.
        vertex->set_content(gtest::to_bytes("v2"));
        kimix::vector<graph_db::DirtyVertex> const stale = graph.dirty_vertices();
        vertex->set_content(gtest::to_bytes("v3"));
        kimix::vector<graph_db::DirtyVertex> const fresh = graph.dirty_vertices();
        expect(eq(stale.size(), size_t{1}));
        expect(eq(fresh.size(), size_t{1}));
        expect(!graph.clear_vertex_dirty_if(vertex, stale.front().dirty_epoch))
            << "the superseded stamp is refused";
        expect(graph.clear_vertex_dirty_if(vertex, fresh.front().dirty_epoch));
        expect(graph.dirty_vertices().empty());

        // The graph-wide helper still clears unconditionally (a full save / an
        // explicit reset by the caller), and the epoch keeps advancing.
        uint64_t const before_reset = vertex->dirty_epoch();
        vertex->set_content(gtest::to_bytes("v4"));
        graph.clear_dirty();
        expect(graph.dirty_vertices().empty());
        expect(ge(vertex->dirty_epoch(), before_reset))
            << "a reset clears the flag, it does not rewind the stamp";
    };
}
