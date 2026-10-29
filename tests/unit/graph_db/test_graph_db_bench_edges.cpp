// Benchmark for the EDGE-filtered traversals of src/graph_db (kimix-graph-db) —
// optimize_plan.md phase E0 ("measure"), §3.
//
// This suite covers (the scenario matrix of §3.4, every cell x the fixtures of
// §3.3 that make it meaningful):
// - S1  filtered shortest_path, 1 hop (per-call walk state)
// - S2  filtered, 3 hops (the acceptance row; F1 label 100 % and F2 label 50 %)
// - S3  filtered, 8 hops (level width grows -> batching amortization)
// - S4  filtered, unreachable: F5 chain over the full 32 levels, and the wide
//       F1 variant (32 levels of ~100 reachable vertices per level = the peak)
// - S5  filtered has_path, true and false
// - S6  the UNFILTERED control: shortest_path / has_path on the SAME pairs as
//       S2/S5. This row is what keeps the README's "the unfiltered guid
//       projection fast path stays untouched" promise honest, so it must not
//       regress.
// - S7  orientation outgoing / incoming / both (oriented_links copy cost)
// - S8  selectivity sweep: label 100 % / 50 % / 5 % / 0 %, property equality,
//       two-condition AND
// - S9  contract early-outs (unknown endpoints, max_depth 0 and 33, a label
//       condition carrying an ordering op, and the empty graph) — these must be
//       one early return, ~0 cost
// - S10 the C ABI kimix_gdb_shortest_path_edges two-phase protocol (count-only
//       phase, then the fill phase)
// - S11 the two primitives alone: edge_matches (one edge-shard lock + hash find)
//       and oriented_links (one vertex-shard lock + link copy), so a top-down
//       row can be decomposed instead of guessed at
//
// HARNESS (optimize_plan.md §3.2): every time row goes through tests/unit/native/
// bench_util.h's kimix_bench::run -> "[bench] edges/<scenario>/<fixture> <ns/op>
// <ops/s> (<iters>)" on stderr, and every row additionally prints ONE memory row
// in the same key space -> "[mem] edges/<scenario>/<fixture> <allocs/op> <B/op>
// peak <B>", measured with mimalloc's per-thread counters (the process heap IS
// mimalloc; the probe self-checks on first use and never fails — a build without
// MI_STATS just prints "[bench] alloc probe unavailable" and keeps the time
// rows). Instrumented [ctr] rows are printed only in a build with xmake option
// graph_db_walk_counters=y.
//
// THERE IS NOT ONE TIMING ASSERTION IN THIS FILE, on purpose — the existing
// suite's rule: a benchmark that fails on a slow CI machine is a broken
// benchmark. The only expects() are (a) the brute-force parity gate, (b)
// methodology checks (the fixtures really have the shape, the workloads really
// did work, the pairs really are at the distance the row name claims, the
// early-out rows really return the documented status) and (c) — only in the
// instrumented build — counter RELATIONS, never absolute numbers (§3.5 item 2).
//
// CORRECTNESS GATE #1 (§3.5): before anything is timed, ~200 seeded pairs per
// fixture run through both the edge-filtered walk and a brute-force reference
// (the algorithm of test_graph_db_traverse.cpp's
// edge_filtered_shortest_path_matches_brute_force, adapted: it walks the PUBLIC
// edge reads only — incident_edges + edge_record + matches — so it shares no
// code with the engine's link-level walk), and the WHOLE PathStep sequence
// (vertex, edge, depth) must be equal. An optimization that changes an answer
// fails here, not in review.
//
// GATED: only runs when KIMIX_GRAPH_DB_BENCH=1 is set (same convention as
// KIMIX_PDQSORT_BENCH / KIMIX_GREP_BENCH / test_graph_db_bench). Any other run —
// a plain `xmake test`, a bare exe invocation — prints one skip line and exits
// immediately, so this target never slows the default test sweep down:
//   set KIMIX_GRAPH_DB_BENCH=1 && xmake run test_graph_db_bench_edges
//   (PowerShell) $env:KIMIX_GRAPH_DB_BENCH=1; xmake run test_graph_db_bench_edges
// Every published row and the whole E0 record are RELEASE numbers. The fixture
// sizes carry a `scaled()` debug variant (§3.3's "(20 k)" parentheticals), but no
// debug configuration was built or run for E0, so a debug run is NOT a validated
// mode — it exists only so a future debug build does not have to allocate a
// 600k-edge fixture.
//
// CALIBRATION — how a row is compared across runs and phases (a §3.6 addendum:
// this host's clock state is not stable, so raw ns/op is not a comparable unit).
// A [cal] row is ONE fixed, deterministic, PURE-CPU workload that touches NO
// graph_db code: kCalBufferBytes bytes hashed kCalRepeats times with
// kimix::hash64 (XXH3). kCalRepeats is chosen once, on the first [cal] row, so one
// measurement costs ~100 us, and the SAME count is used for every [cal] row of the
// run — so two [cal] rows measure the identical work, and any difference between
// them is the MACHINE (frequency/thermal state), never the code under test.
// A [cal] row reads "[cal] <label> <ns/op> (min .. median .. max .. of N)" — the
// recorded ns/op is the min of the row's samples — and the label is the POSITION in the
// run (start, after_fixtures, before_S1 ... before_S11, end). The run closes with
//   [bench] calibration min <ns> max <ns> drift <pct> %  run quality: RECORD-OK
// (RECORD-DEGRADED when (max-min)/min > 5 %). It is INFORMATIONAL ONLY: no
// expect(), no exit-code change — a benchmark that fails on a loaded CI box is a
// broken benchmark.
// To compare a row with another RUN or another PHASE, normalise it:
//     normalized_ns = row_ns / cal_ns(nearest preceding [cal] row)
// (or scale two runs onto one clock state with cal_ns_A(any row) /
// cal_ns_B(the same position)). Raw ns/op from two different runs must never be
// compared directly, and S6 (the unfiltered control) is measured immediately
// adjacent to the filtered row it polices, in the same interval, for that reason.
// HONEST LIMIT, measured: a ~100 us pure-CPU row is a COARSE state indicator. On
// this host the [cal] level moved up to 8x between sessions while the 120 ms row
// windows moved ~30%, i.e. the calibration is noisier than the rows it would
// normalise — so treat [cal] primarily as the RECORD-OK/RECORD-DEGRADED gate and
// as a gross state label, not as a precision scale factor. Measured on the three
// RECORD-OK runs of the E0 record (M, N, O), 39 rows each: O-vs-M agreed within 5 %
// on 27 rows, O-vs-N on 32, M-vs-N on 32 — and a wider sample (8 RECORD-OK runs)
// put 35 of 39 rows over 5 %. The rule this file is therefore graded by is:
// RECORD-OK decides whether a run may be QUOTED AT ALL, but a per-row number must be
// the MEDIAN OF THREE runs, never a single 120 ms window. Rows that still exceed
// 5 % are the allocation/page-bound ones (S2 on F1 by ~9 %, S3, the S4/S5-wide rows
// and S10 by up to ~34 %), whose variance is page and allocator state, not clock,
// and is therefore invisible to any CPU calibration: compare those on the [mem]
// axis or with the 3-run median, never across a single run.
//
// DETERMINISM — what is and is not reproducible:
// * TOPOLOGY is seeded and reproducible: every fixture is built from a fixed seed
//   with std::mt19937, with no clock and no address-dependent choice, and every
//   fixture is built ONCE, single-writer (the bench thread), before any
//   measurement, so no fixture cost can land inside a timed window. The endpoint
//   pairs are either derived from the construction (F2/F4/F5 have known
//   distances) or discovered by a bounded BFS over the reference implementation
//   (F1/F3).
// * IDENTITIES are NOT reproducible: Graph::create_vertex / edge_create mint every
//   guid with kimix::Guid{true} -> CoCreateGuid (src/graph_db/graph.cpp:140,
//   src/core/guid.cpp:211), so the same seeded topology gets different guids in
//   every process. The consequence is confined to shard ASSIGNMENT: visit counts,
//   candidate counts, allocation counts and every PathStep sequence are stable
//   (they depend on the topology, not on the guids), while the `shards` column of
//   the [ctr] rows — the number of distinct edge shards a walk touches — is a
// per-process random variable: measured 27..31 for S2_3hop_label100/F1 across
// sessions, and 59..64 for the shard-heavy rows (S7_in_3hop/F1,
// S2_3hop_label100/F3, the S4_unreachable_wide/F1 walk), which touch most of the
// 64 edge shards anyway. Nothing here asserts or compares `shards`, or any other
// ABSOLUTE counter value: the [ctr] relations are inequalities, plus the S9 rows'
// "all counters are zero".
// THREADING: deliberately none — every row here is single-threaded, and no fiber
// scheduler is bound, so the C ABI's collective paths run inline. This file
// measures the per-call cost of one walk, not scaling; that is
// test_graph_db_bench.cpp's job.

#include "ut/ut.hpp"
#include "unit/native/bench_util.h" // tests/ is the include root: shared bench harness
#include "graph_db_test_util.h"

#include <core/kimix_core.h> // umbrella first among the core headers (winsock2 order)
#include <core/clock.h>
#include <core/guid.h>
#include <core/platform.h>
#include <core/stl/string.h>
#include <core/stl/hash.h> // kimix::hash64 (the [cal] workload: pure CPU, no graph_db)
#include <core/stl/unordered_map.h> // kimix::unordered_map + kimix::unordered_set
#include <core/stl/vector.h>

#if defined(KIMIX_PLATFORM_WINDOWS)
  #ifndef NOMINMAX
    #define NOMINMAX 1
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN 1
  #endif
  #include <windows.h> // SetThreadAffinityMask / GetSystemInfo (see pin_measurement_thread)
#endif

// The ONLY graph_db C++ headers this suite needs, plus the C ABI for S10.
#include <graph_db/edge.h>
#include <graph_db/graph.h>
#include <graph_db/graph_c_api.h>
#include <graph_db/property.h>
#include <graph_db/query.h>
#include <graph_db/schema.h>
#include <graph_db/status.h>

// mimalloc's per-thread allocation counters (the [mem] rows). The heap IS
// mimalloc, so this counts the real allocator, not a proxy.
#include <mimalloc.h>
#include <mimalloc-stats.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib> // std::getenv (KIMIX_GRAPH_DB_BENCH gate)
#include <cstring> // std::strlen (the C ABI fixture's external ids)
#include <random>
#include <string_view>
#include <thread>
#include <utility>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;

using graph_db::Constraint;
using graph_db::ConstraintCondition;
using graph_db::EdgeDirectionType;
using graph_db::EdgeId;
using graph_db::EdgeLink;
using graph_db::EdgeOrientation;
using graph_db::GdiOp;
using graph_db::GdiStatus;
using graph_db::Graph;
using graph_db::LabelId;
using graph_db::PropNameId;
using graph_db::PropertyEntry;
using graph_db::PropertyList;
using graph_db::PropertyValue;
using graph_db::Vertex;
namespace gtest = kimix::graph_db::test;

namespace tests_unit_graph_db_test_graph_db_bench_edges {

// ---------------------------------------------------------------------------
// Constants: fixture sizes and the fabricated label / property ids
// ---------------------------------------------------------------------------
// Release sizes are the record; the debug run only proves the suite passes, so
// the big fixtures shrink by 5x under !NDEBUG (the existing bench's convention).
#if defined(NDEBUG)
inline constexpr bool kReleaseSizes = true;
#else
inline constexpr bool kReleaseSizes = false;
#endif

inline constexpr size_t scaled(size_t release_size, size_t debug_size) {
    return kReleaseSizes ? release_size : debug_size;
}

// F1 — layered: out-degree 4..8, a uniform `R` label on every edge. Layer width
// 100 means a level saturates at 100 vertices from depth 3 on, which is the
// shape the cost model in optimize_plan §2 assumes (frontier 1 -> 6 -> 36).
inline constexpr size_t kF1Vertices = scaled(100'000, 20'000);
inline constexpr size_t kF1LayerWidth = 100;
inline constexpr size_t kF1DegreeMin = 4;
inline constexpr size_t kF1DegreeMax = 8;

// F2 — ladder: two rails of 5000 columns plus one rung per column; rails of row 0
// carry `A`, rails of row 1 carry `B`, rungs alternate A/B by column parity, so
// the split is exactly 50/50 (7499 / 7499). Every edge also carries the single
// i32 property `w` = (source column / 3) % 8, so a property-equality filter has a
// 12.5 % selectivity AND three consecutive matching hops exist (columns 9..11).
// `C` is added to the rails of both rows for columns < 375: 750 of 14998 edges =
// 5.0 %, and a 3-hop C-chain exists (the rails are consecutive). `D` is applied
// to no edge at all (0 %).
inline constexpr size_t kF2Columns = scaled(5'000, 1'000);
inline constexpr size_t kF2Vertices = kF2Columns * 2;
inline constexpr size_t kF2LabelCFirstColumn = 375; // ~5 % of the edges
inline constexpr size_t kF2ColumnsPerPropGroup = 3;
inline constexpr size_t kF2PropGroups = 8;

// F3 — dense: out-degree 16 with high clustering (every target is drawn from the
// previous 512 indices), which is what makes a level's neighbourhoods overlap and
// therefore makes the visited-check-first lever (O1) visible.
inline constexpr size_t kF3Vertices = scaled(20'000, 5'000);
inline constexpr size_t kF3Degree = 16;
inline constexpr size_t kF3Window = 512;

// F4 — hub: one hub with 5000 out-links to 5000 leaves; the first 1000 leaves
// carry 4 leaves of their own, so a 2-hop target exists behind the whole fan-out.
inline constexpr size_t kF4Children = scaled(5'000, 1'500);
inline constexpr size_t kF4GrandchildParents = scaled(1'000, 300);
inline constexpr size_t kF4Grandchildren = 4;

// F5 — chain: a path graph of degree 1, label `A` only. The max-depth walk and
// the has_path(false) transient peak live here.
inline constexpr size_t kF5Vertices = scaled(100'000, 20'000);

// F2c — the C ABI ladder for S10: two chains of 1000 columns, label A / B. Built
// through the C ABI (it measures the marshalling, so the fixture must be a real
// session), small enough that the build stays out of the report.
inline constexpr size_t kCabiColumns = 1'000;

// Fabricated ids: the Graph container has no schema to register them in (LabelId
// / PropNameId are plain uint32_t aliases, property.h), exactly like
// test_graph_db_traverse.cpp's edge-filter constants.
inline constexpr LabelId kLabelR = 1u; // F1 / F3 / F4: every edge
inline constexpr LabelId kLabelA = 2u; // F2 row 0 rails + even rungs; F5 every edge
inline constexpr LabelId kLabelB = 3u; // F2 row 1 rails + odd rungs
inline constexpr LabelId kLabelC = 4u; // F2: the first 375 columns of both rails (~5 %)
inline constexpr LabelId kLabelD = 5u; // F2: applied to NO edge (0 %)
inline constexpr PropNameId kPropW = 6u; // F2: one i32 property on every edge

inline constexpr uint32_t kSeed = 20240517u;

// The measured window of one [bench] row, seconds. The whole suite must stay
// well under ~120 s in release, which is why this is 0.12 s and not the
// harness's 0.25 s default: ~30 rows x (0.05 s calibration + 0.12 s measure).
inline constexpr double kWindow = 0.12;

// Seeded draws per fixture for the brute-force parity gate (§3.5 gate #1).
inline constexpr size_t kParityPairs = 200;

// ---------------------------------------------------------------------------
// Deterministic vertex CONTENT (seeded, no clock). The guid is minted by
// Graph::create_vertex with CoCreateGuid, so identities are per-process — see
// the DETERMINISM note in the file header.
// ---------------------------------------------------------------------------
inline kimix::vector<std::byte> vertex_content(size_t index) {
    kimix::vector<std::byte> out(16, std::byte{0});
    uint64_t x = (static_cast<uint64_t>(index) + 1ull) * 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < out.size(); ++i) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        out[i] = static_cast<std::byte>((x >> 33) & 0xFFu);
    }
    return out;
}

// ---------------------------------------------------------------------------
// The allocation probe (§3.2)
// ---------------------------------------------------------------------------
// One logical call's allocator footprint, exact and per-thread. mimalloc
// (>= 3.x) keeps the allocation counters PER PAGE and folds them into the
// thread's record (`mi_theap`) only on a page update, so a window has to force
// that fold: `mi_theap_collect(theap, false)` visits the thread's pages and
// updates their stats, and with the automatic merges disabled (see
// alloc_probe_available) the folded numbers then SURVIVE in `theap->stats` until
// the window clears them. Self-checks on first use; a zero count after a
// known-allocating control workload means this build has no usable counters, and
// the suite then prints "[bench] alloc probe unavailable" and keeps the time
// rows — it never fails, and it never asserts a time.
class AllocWindow {
  public:
    struct Result {
        int64_t allocations;
        int64_t requested_bytes;
        int64_t peak_bytes;
    };

    AllocWindow() : _theap(mi_theap_get_default()) {
        fold();                                      // pull pending page counters in
        mi_theap_stats_merge_to_heap(_theap);         // ... and clear the record
    }

    [[nodiscard]] Result close() {
        fold();
        mi_stats_t stats;
        mi_stats_init(&stats);
        if (_theap == nullptr || !mi_theap_stats_get(_theap, &stats)) {
            return Result{0, 0, 0};
        }
        return Result{stats.malloc_count.total, stats.malloc_requested.total,
                      stats.allocated.peak};
    }

  private:
    // Fold this thread's per-page counters into its thread record. force=false:
    // no purging, no page release — only the statistic update.
    void fold() {
        if (_theap != nullptr) {
            mi_theap_collect(_theap, false);
        }
    }

    mi_theap_t *_theap;
};

// True once the probe is known to work. The control workload allocates a known
// amount of SMALL blocks (a 64 KiB block would live on its own huge page, which
// a non-forcing collect does not visit), so a working build reports many
// allocations and >= 32 KiB of requested bytes; anything else is treated as "no
// usable counters in this build" and the [mem] axis is dropped.
inline bool alloc_probe_available() {
    static int state = -1; // -1 unknown, 0 unavailable, 1 available
    if (state < 0) {
        // Keep the thread record intact: without this, every `mi_theap_collect`
        // and every 512 KiB of churn would merge the record into the parent heap
        // (mimalloc's defaults), and the window would always read zero.
        mi_option_set(mi_option_stats_merge_on_collect, 0);
        mi_option_set(mi_option_stats_merge_threshold, 0);

        AllocWindow window;
        int64_t control_bytes = 0;
        {
            kimix::vector<kimix::vector<std::byte>> control;
            control.reserve(512);
            for (size_t i = 0; i < 512; ++i) {
                control.push_back(kimix::vector<std::byte>(64, std::byte{7}));
                control_bytes += 64;
            }
            kimix_bench::sink(control.back().size());
        }
        AllocWindow::Result const result = window.close();
        bool const available =
            result.allocations >= 512 && result.requested_bytes >= control_bytes / 2;
        state = available ? 1 : 0;
        std::fprintf(stderr,
                     "[bench] alloc probe self-check: control %lld B in 512 blocks -> %lld allocs, "
                     "%lld B requested -> %s\n",
                     static_cast<long long>(control_bytes),
                     static_cast<long long>(result.allocations),
                     static_cast<long long>(result.requested_bytes),
                     available ? "counters live" : "UNAVAILABLE");
        if (!available) {
            std::fprintf(stderr,
                         "[bench] alloc probe unavailable (this build keeps no usable mimalloc "
                         "counters); keeping the time rows\n");
        }
        kimix_bench::sink(available);
    }
    return state == 1;
}

// ---------------------------------------------------------------------------
// Row printers: [bench] (through kimix_bench::run) + [mem] + [ctr]
// ---------------------------------------------------------------------------
template <typename Fn> inline void mem_row(char const *name, Fn &&fn) {
    if (!alloc_probe_available()) {
        return;
    }
    AllocWindow window;
    fn();
    AllocWindow::Result const result = window.close();
    std::fprintf(stderr, "[mem]   %-44s %8.1f allocs/op %10.0f B/op  peak %10lld B\n", name,
                 static_cast<double>(result.allocations),
                 static_cast<double>(result.requested_bytes),
                 static_cast<long long>(result.peak_bytes));
}

#if defined(KIMIX_GRAPH_DB_WALK_COUNTERS)
// The relations §3.5 item 2 asserts — chosen to survive EVERY lever of the plan,
// so they are never "adjusted" by a later phase:
//   edge_tests   <= links_snapshotted  (a tested candidate came from a snapshot)
//   edge_locks   <= edge_tests         (one lock per test today, fewer after O2)
//   visited_checks <= links_snapshotted
//   shards       <= edge_locks         (distinct shards locked <= lock takes)
// COLUMN NAMES: the printed columns are named after what they CARRY, which is not
// the pre-O1 vocabulary — O1 made the per-candidate denominator visited_checks, so
// that column prints as "candidates" (it still equals E0's candidate count on every
// row) and the filter-evaluation count prints as "edge_tests". The relations above
// stay stated in the counters' own field names.
inline unsigned popcount64(uint64_t value) noexcept {
    unsigned count = 0;
    while (value != 0) {
        value &= value - 1;
        ++count;
    }
    return count;
}

template <typename Fn> inline void ctr_row(char const *name, Fn &&fn, bool walk_relations) {
    graph_db::reset_walk_counters();
    fn();
    graph_db::WalkCounters const &counters = graph_db::walk_counters();
    unsigned const shards = popcount64(counters.edge_shards);
    std::fprintf(stderr,
                 "[ctr]   %-44s visits %llu  edge_tests %llu  edge_locks %llu  shards %u  "
                 "links %llu  candidates %llu\n",
                 name, static_cast<unsigned long long>(counters.vertex_locks),
                 static_cast<unsigned long long>(counters.edge_tests),
                 static_cast<unsigned long long>(counters.edge_locks), shards,
                 static_cast<unsigned long long>(counters.links_snapshotted),
                 static_cast<unsigned long long>(counters.visited_checks));
    if (walk_relations) {
        expect(le(counters.edge_tests, counters.links_snapshotted))
            << name << ": every tested candidate came from a snapshot";
        expect(le(counters.edge_locks, counters.edge_tests))
            << name << ": one edge-shard lock per test today, at most";
        expect(le(counters.visited_checks, counters.links_snapshotted))
            << name << ": a visited probe follows a snapshot";
        expect(le(static_cast<uint64_t>(shards), counters.edge_locks))
            << name << ": distinct locked shards <= lock takes";
    }
}

// A row that must cost ~0 (S9): the early return happens before any lock, so the
// counters must stay at zero. That is the structural proof behind "~0 cost".
template <typename Fn> inline void ctr_row_early_out(char const *name, Fn &&fn) {
    graph_db::reset_walk_counters();
    fn();
    graph_db::WalkCounters const &counters = graph_db::walk_counters();
    std::fprintf(stderr,
                 "[ctr]   %-44s visits %llu  edge_tests %llu  edge_locks %llu  shards 0  links "
                 "%llu  candidates %llu\n",
                 name, static_cast<unsigned long long>(counters.vertex_locks),
                 static_cast<unsigned long long>(counters.edge_tests),
                 static_cast<unsigned long long>(counters.edge_locks),
                 static_cast<unsigned long long>(counters.links_snapshotted),
                 static_cast<unsigned long long>(counters.visited_checks));
    expect(eq(counters.vertex_locks, uint64_t{0})) << name << ": an early-out takes no vertex lock";
    expect(eq(counters.edge_locks, uint64_t{0})) << name << ": an early-out takes no edge lock";
    expect(eq(counters.edge_tests, uint64_t{0})) << name << ": an early-out tests no edge";
    expect(eq(counters.edge_shards, uint64_t{0})) << name << ": an early-out touches no shard";
}
#else
template <typename Fn> inline void ctr_row(char const *, Fn &&, bool) {}
template <typename Fn> inline void ctr_row_early_out(char const *, Fn &&) {}
#endif

// ---------------------------------------------------------------------------
// Fixtures F1..F6 (§3.3)
// ---------------------------------------------------------------------------
struct bench_fixture {
    Graph graph;
    kimix::vector<kimix::Guid> vertices; // creation order (F1: layer order)
    kimix::vector<EdgeId> sample_edges;  // the first few created edge ids (S11)
    bool built{false};
};

inline kimix::Guid make_vertex(Graph &graph, size_t index) {
    kimix::vector<std::byte> content = vertex_content(index);
    Vertex *vertex =
        graph.create_vertex(kimix::span<const std::byte>{content.data(), content.size()});
    return vertex != nullptr ? vertex->guid() : kimix::Guid{};
}

inline bool make_edge(Graph &graph, kimix::Guid const &from, kimix::Guid const &to, LabelId label,
                      PropertyList const *properties, kimix::Guid &out_edge) {
    LabelId const labels[] = {label};
    return graph.edge_create(from, to, EdgeDirectionType::directed,
                             kimix::span<const LabelId>{labels, 1}, properties, nullptr,
                             out_edge) == GdiStatus::ok;
}

// F1 — layered DAG, every edge labelled `R`. Each vertex links 4..8 vertices of
// the NEXT layer only, so the hop distance is the layer distance and a level's
// width saturates at kF1LayerWidth.
inline void build_f1(bench_fixture &fixture) {
    fixture.vertices.reserve(kF1Vertices);
    for (size_t i = 0; i < kF1Vertices; ++i) {
        fixture.vertices.push_back(make_vertex(fixture.graph, i));
    }
    std::mt19937 rng(kSeed);
    std::uniform_int_distribution<size_t> degree_dist(kF1DegreeMin, kF1DegreeMax);
    std::uniform_int_distribution<size_t> pick(0, kF1LayerWidth - 1);
    size_t const layers = kF1Vertices / kF1LayerWidth;
    size_t edges = 0;
    for (size_t layer = 0; layer + 1 < layers; ++layer) {
        size_t const base = layer * kF1LayerWidth;
        size_t const next = base + kF1LayerWidth;
        for (size_t column = 0; column < kF1LayerWidth; ++column) {
            size_t const want = degree_dist(rng);
            size_t made = 0;
            size_t guard = 0;
            while (made < want && guard < want * 4) {
                ++guard;
                kimix::Guid edge{};
                if (make_edge(fixture.graph, fixture.vertices[base + column],
                              fixture.vertices[next + pick(rng)], kLabelR, nullptr, edge)) {
                    ++made;
                    ++edges;
                    if (fixture.sample_edges.size() < 8) {
                        fixture.sample_edges.push_back(edge);
                    }
                }
            }
        }
    }
    std::fprintf(stderr, "[bench] fixture F1: %zu vertices, %zu edges (label R, out-degree %zu..%zu)\n",
                 kF1Vertices, edges, kF1DegreeMin, kF1DegreeMax);
}

// F2 — ladder (see the constants above for the exact label arithmetic).
inline void build_f2(bench_fixture &fixture) {
    fixture.vertices.reserve(kF2Vertices);
    for (size_t i = 0; i < kF2Vertices; ++i) {
        fixture.vertices.push_back(make_vertex(fixture.graph, i));
    }
    PropertyList properties;
    size_t edges = 0;
    size_t labelled_c = 0;
    // Rails: row 0 -> A, row 1 -> B. Property w = (column / 3) % 8.
    for (size_t row = 0; row < 2; ++row) {
        for (size_t column = 0; column + 1 < kF2Columns; ++column) {
            properties.clear();
            properties.push_back(PropertyEntry{
                kPropW, PropertyValue::make_int64(
                            static_cast<int64_t>((column / kF2ColumnsPerPropGroup) % kF2PropGroups))});
            kimix::Guid edge{};
            LabelId const rail = row == 0 ? kLabelA : kLabelB;
            if (!make_edge(fixture.graph, fixture.vertices[row * kF2Columns + column],
                           fixture.vertices[row * kF2Columns + column + 1], rail, &properties,
                           edge)) {
                continue;
            }
            ++edges;
            if (fixture.sample_edges.size() < 8) {
                fixture.sample_edges.push_back(edge);
            }
            // C on the rails of BOTH rows for the first 375 columns: 5 % of the
            // edges, and a 3-hop C-chain exists (a refused edge is still skipped,
            // not discovered, which is what the sweep measures).
            if (column < kF2LabelCFirstColumn &&
                fixture.graph.edge_add_label(edge, kLabelC) == GdiStatus::ok) {
                ++labelled_c;
            }
        }
    }
    // Rungs: alternating A/B by column parity, keeping the 50/50 split exact.
    for (size_t column = 0; column < kF2Columns; ++column) {
        properties.clear();
        properties.push_back(PropertyEntry{
            kPropW, PropertyValue::make_int64(
                        static_cast<int64_t>((column / kF2ColumnsPerPropGroup) % kF2PropGroups))});
        kimix::Guid edge{};
        if (make_edge(fixture.graph, fixture.vertices[column],
                      fixture.vertices[kF2Columns + column],
                      (column % 2u) == 0u ? kLabelA : kLabelB, &properties, edge)) {
            ++edges;
            if (fixture.sample_edges.size() < 8) {
                fixture.sample_edges.push_back(edge);
            }
        }
    }
    std::fprintf(stderr,
                 "[bench] fixture F2: %zu vertices, %zu edges (A/B 50-50, C %zu = %.2f %%, "
                 "property w:i32 = (col/%zu)%%%zu)\n",
                 kF2Vertices, edges, labelled_c,
                 100.0 * static_cast<double>(labelled_c) / static_cast<double>(edges),
                 kF2ColumnsPerPropGroup, kF2PropGroups);
}

// F3 — dense, clustered, out-degree 16.
inline void build_f3(bench_fixture &fixture) {
    fixture.vertices.reserve(kF3Vertices);
    for (size_t i = 0; i < kF3Vertices; ++i) {
        fixture.vertices.push_back(make_vertex(fixture.graph, i));
    }
    std::mt19937 rng(kSeed + 3u);
    size_t edges = 0;
    for (size_t i = 1; i < kF3Vertices; ++i) {
        size_t const span = i < kF3Window ? i : kF3Window;
        std::uniform_int_distribution<size_t> pick(0, span - 1);
        size_t made = 0;
        size_t guard = 0;
        while (made < kF3Degree && guard < kF3Degree * 4) {
            ++guard;
            kimix::Guid edge{};
            if (make_edge(fixture.graph, fixture.vertices[i], fixture.vertices[i - 1 - pick(rng)],
                          kLabelR, nullptr, edge)) {
                ++made;
                ++edges;
                if (fixture.sample_edges.size() < 8) {
                    fixture.sample_edges.push_back(edge);
                }
            }
        }
    }
    std::fprintf(stderr,
                 "[bench] fixture F3: %zu vertices, %zu edges (label R, out-degree %zu, "
                 "clustered window %zu)\n",
                 kF3Vertices, edges, kF3Degree, kF3Window);
}

// F4 — hub: 1 hub -> kF4Children leaves; the first kF4GrandchildParents leaves
// carry kF4Grandchildren leaves each.
inline void build_f4(bench_fixture &fixture) {
    fixture.vertices.reserve(1 + kF4Children + kF4GrandchildParents * kF4Grandchildren);
    kimix::Guid const hub = make_vertex(fixture.graph, 0);
    fixture.vertices.push_back(hub);
    for (size_t child = 0; child < kF4Children; ++child) {
        kimix::Guid const guid = make_vertex(fixture.graph, 1 + child);
        fixture.vertices.push_back(guid);
        kimix::Guid edge{};
        if (make_edge(fixture.graph, hub, guid, kLabelR, nullptr, edge) &&
            fixture.sample_edges.size() < 8) {
            fixture.sample_edges.push_back(edge);
        }
    }
    for (size_t parent = 0; parent < kF4GrandchildParents; ++parent) {
        for (size_t grandchild = 0; grandchild < kF4Grandchildren; ++grandchild) {
            kimix::Guid const guid = make_vertex(fixture.graph, fixture.vertices.size());
            fixture.vertices.push_back(guid);
            kimix::Guid edge{};
            make_edge(fixture.graph, fixture.vertices[1 + parent], guid, kLabelR, nullptr, edge);
        }
    }
    std::fprintf(stderr,
                 "[bench] fixture F4: %zu vertices, hub out-degree %zu, %zu grandchildren under "
                 "%zu children (label R)\n",
                 fixture.vertices.size(), kF4Children,
                 kF4GrandchildParents * kF4Grandchildren, kF4GrandchildParents);
}

// F5 — chain, degree 1, label A only.
inline void build_f5(bench_fixture &fixture) {
    fixture.vertices.reserve(kF5Vertices);
    for (size_t i = 0; i < kF5Vertices; ++i) {
        fixture.vertices.push_back(make_vertex(fixture.graph, i));
    }
    size_t edges = 0;
    for (size_t i = 0; i + 1 < kF5Vertices; ++i) {
        kimix::Guid edge{};
        if (make_edge(fixture.graph, fixture.vertices[i], fixture.vertices[i + 1], kLabelA, nullptr,
                      edge)) {
            ++edges;
            if (fixture.sample_edges.size() < 8) {
                fixture.sample_edges.push_back(edge);
            }
        }
    }
    std::fprintf(stderr, "[bench] fixture F5: %zu vertices, %zu edges (label A, degree 1)\n",
                 kF5Vertices, edges);
}

// F6 — empty: the contract fast paths must cost ~0.
inline void build_f6(bench_fixture &fixture) {
    std::fprintf(stderr, "[bench] fixture F6: 0 vertices (empty graph)\n");
    kimix_bench::sink(fixture.vertices.size());
}

// The fixtures are built ONCE, lazily, on the bench thread, before the first
// timed row of the suite that needs them.
inline bench_fixture &fixture_f1() {
    static bench_fixture fixture;
    if (!fixture.built) {
        build_f1(fixture);
        fixture.built = true;
    }
    return fixture;
}
inline bench_fixture &fixture_f2() {
    static bench_fixture fixture;
    if (!fixture.built) {
        build_f2(fixture);
        fixture.built = true;
    }
    return fixture;
}
inline bench_fixture &fixture_f3() {
    static bench_fixture fixture;
    if (!fixture.built) {
        build_f3(fixture);
        fixture.built = true;
    }
    return fixture;
}
inline bench_fixture &fixture_f4() {
    static bench_fixture fixture;
    if (!fixture.built) {
        build_f4(fixture);
        fixture.built = true;
    }
    return fixture;
}
inline bench_fixture &fixture_f5() {
    static bench_fixture fixture;
    if (!fixture.built) {
        build_f5(fixture);
        fixture.built = true;
    }
    return fixture;
}
inline bench_fixture &fixture_f6() {
    static bench_fixture fixture;
    if (!fixture.built) {
        build_f6(fixture);
        fixture.built = true;
    }
    return fixture;
}

// ---------------------------------------------------------------------------
// The C ABI fixture for S10 (an in-memory session, built through the C ABI)
// ---------------------------------------------------------------------------
struct cabi_fixture {
    kimix_gdb_session *session{nullptr};
    uint32_t label_node{0};
    uint32_t label_a{0};
    uint32_t label_b{0};
    uint32_t constraint_a{0};
    kimix_gdb_guid from{};
    kimix_gdb_guid to{};
    bool built{false};

    cabi_fixture() = default;
    cabi_fixture(cabi_fixture const &) = delete;
    cabi_fixture &operator=(cabi_fixture const &) = delete;
    ~cabi_fixture() {
        if (session != nullptr) {
            kimix_gdb_close(session);
        }
    }
};

inline kimix_gdb_bytes cabi_bytes(char const *text) {
    kimix_gdb_bytes out;
    out.data = reinterpret_cast<uint8_t const *>(text);
    out.len = std::strlen(text);
    return out;
}

inline cabi_fixture &fixture_cabi() {
    static cabi_fixture fixture;
    if (fixture.built) {
        return fixture;
    }
    kimix_gdb_open_options options{};
    options.directory = nullptr; // in memory: no LMDB files, no temp directory
    options.auto_create_schema = 1;
    options.allow_parallel_edges = 1;
    options.allow_self_edges = 0;
    options.max_journal_bytes = 64ull * 1024ull * 1024ull;
    expect(eq(kimix_gdb_open(&options, &fixture.session), KIMIX_GDB_OK))
        << "the C ABI session opens";
    expect(eq(kimix_gdb_label_create(fixture.session, "node", 4, &fixture.label_node),
              KIMIX_GDB_OK));
    expect(eq(kimix_gdb_label_create(fixture.session, "route_a", 7, &fixture.label_a),
              KIMIX_GDB_OK));
    expect(eq(kimix_gdb_label_create(fixture.session, "route_b", 7, &fixture.label_b),
              KIMIX_GDB_OK));
    expect(eq(kimix_gdb_constraint_create(fixture.session, &fixture.constraint_a), KIMIX_GDB_OK));
    expect(eq(kimix_gdb_constraint_add_label(fixture.session, fixture.constraint_a,
                                             fixture.label_a, KIMIX_GDB_OP_EQUAL),
              KIMIX_GDB_OK));

    // Two chains of kCabiColumns, row 0 labelled route_a, row 1 route_b.
    kimix::vector<kimix_gdb_guid> vertices;
    vertices.reserve(kCabiColumns * 2);
    char external[32];
    for (size_t i = 0; i < kCabiColumns * 2; ++i) {
        std::snprintf(external, sizeof(external), "v%zu", i);
        kimix_gdb_vertex_spec spec{};
        spec.key.label = fixture.label_node;
        spec.key.external_id = cabi_bytes(external);
        kimix_gdb_guid guid{};
        uint8_t warning = 0;
        if (kimix_gdb_vertex_create(fixture.session, &spec, &guid, &warning) != KIMIX_GDB_OK) {
            break;
        }
        vertices.push_back(guid);
    }
    expect(eq(vertices.size(), kCabiColumns * 2)) << "the C ABI fixture is complete";
    for (size_t row = 0; row < 2; ++row) {
        for (size_t column = 0; column + 1 < kCabiColumns; ++column) {
            uint32_t const label = row == 0 ? fixture.label_a : fixture.label_b;
            kimix_gdb_guid edge{};
            kimix_gdb_edge_create(fixture.session, vertices[row * kCabiColumns + column],
                                  vertices[row * kCabiColumns + column + 1],
                                  KIMIX_GDB_EDGE_DIRECTED, &label, 1, nullptr, nullptr, 0, nullptr,
                                  &edge);
        }
    }
    if (vertices.size() == kCabiColumns * 2) {
        fixture.from = vertices[0];                   // row 0, column 0
        fixture.to = vertices[3];                     // row 0, column 3: exactly 3 A-hops
    }
    std::fprintf(stderr,
                 "[bench] fixture F2c: %zu C ABI vertices, %zu edges (route_a / route_b)\n",
                 vertices.size(), (kCabiColumns - 1) * 2);
    fixture.built = true;
    return fixture;
}

// ---------------------------------------------------------------------------
// The brute-force reference (§3.5 gate #1), adapted from
// test_graph_db_traverse.cpp's ref_filtered_path: it walks the PUBLIC edge reads
// only (incident_edges + edge_record + matches), so it shares no code with the
// engine's link-level walk.
// ---------------------------------------------------------------------------
void ref_hops(Graph const &graph, kimix::Guid const &current, EdgeOrientation orientation,
              Constraint const &constraint,
              kimix::vector<std::pair<kimix::Guid, EdgeId>> &out) {
    out.clear();
    kimix::vector<EdgeId> ids;
    graph.incident_edges(current, orientation, ids);
    for (auto const &id : ids) {
        graph_db::EdgeRecord record;
        if (!graph.edge_record(id, record)) {
            continue;
        }
        if (!graph_db::matches(constraint, record.labels, record.properties)) {
            continue;
        }
        kimix::Guid const neighbour = record.origin == current ? record.target : record.origin;
        out.push_back({neighbour, id});
    }
}

struct RefPath {
    kimix::vector<kimix::Guid> vertices;
    kimix::vector<EdgeId> edges;
};

RefPath ref_filtered_path(Graph const &graph, kimix::Guid const &from, kimix::Guid const &to,
                          Constraint const &constraint, EdgeOrientation orientation,
                          uint32_t max_depth) {
    RefPath path;
    if (from == to) {
        path.vertices.push_back(from);
        path.edges.push_back(EdgeId{});
        return path;
    }
    kimix::unordered_map<kimix::Guid, kimix::Guid> parent;
    kimix::unordered_map<kimix::Guid, EdgeId> via;
    parent.emplace(from, from);
    via.emplace(from, EdgeId{});
    kimix::vector<kimix::Guid> frontier;
    kimix::vector<kimix::Guid> next_frontier;
    kimix::vector<std::pair<kimix::Guid, EdgeId>> hops;
    frontier.push_back(from);
    uint32_t depth = 0;
    bool found = false;
    while (!frontier.empty() && !found && depth < max_depth) {
        ++depth;
        next_frontier.clear();
        for (auto const &current : frontier) {
            ref_hops(graph, current, orientation, constraint, hops);
            for (auto const &hop : hops) {
                if (parent.find(hop.first) != parent.end()) {
                    continue;
                }
                parent.emplace(hop.first, current);
                via.emplace(hop.first, hop.second);
                if (hop.first == to) {
                    found = true;
                    break;
                }
                next_frontier.push_back(hop.first);
            }
            if (found) {
                break;
            }
        }
        frontier.swap(next_frontier);
    }
    if (!found) {
        return path;
    }
    kimix::vector<kimix::Guid> reversed;
    kimix::vector<EdgeId> reversed_edges;
    for (kimix::Guid cursor = to;; cursor = parent.find(cursor)->second) {
        reversed.push_back(cursor);
        reversed_edges.push_back(via.find(cursor)->second);
        if (cursor == from) {
            break;
        }
    }
    std::reverse(reversed.begin(), reversed.end());
    std::reverse(reversed_edges.begin(), reversed_edges.end());
    path.vertices = std::move(reversed);
    path.edges = std::move(reversed_edges);
    return path;
}

// The engine's path equals the reference: same vertices, same edges (so a
// parallel-edge choice is pinned too) and depth == index.
void expect_same_path(kimix::vector<Graph::PathStep> const &actual, RefPath const &expected,
                      char const *fixture_name) {
    bool const same_size = actual.size() == expected.vertices.size();
    expect(same_size) << fixture_name << ": the path length matches the brute force";
    if (!same_size) {
        return;
    }
    bool vertices_match = true;
    bool edges_match = true;
    bool depths_match = true;
    for (size_t i = 0; i < actual.size(); ++i) {
        vertices_match = vertices_match && actual[i].vertex == expected.vertices[i];
        edges_match = edges_match && actual[i].edge == expected.edges[i];
        depths_match = depths_match && actual[i].depth == static_cast<uint32_t>(i);
    }
    expect(vertices_match) << fixture_name << ": every PathStep vertex matches the brute force";
    expect(edges_match) << fixture_name << ": every PathStep edge matches the brute force";
    expect(depths_match) << fixture_name << ": every PathStep depth is its index";
}

// A vertex exactly `depth` hops away from `from` over MATCHING edges, discovered
// by the reference BFS (deterministic: incident_edges order). Used to place the
// sampled pairs of F1/F3 at the distance the row name claims.
bool find_at_depth(Graph const &graph, kimix::Guid const &from, Constraint const &constraint,
                   EdgeOrientation orientation, uint32_t depth, kimix::Guid &out) {
    if (depth == 0) {
        out = from;
        return true;
    }
    kimix::unordered_set<kimix::Guid> seen;
    kimix::vector<kimix::Guid> frontier;
    kimix::vector<kimix::Guid> next_frontier;
    kimix::vector<std::pair<kimix::Guid, EdgeId>> hops;
    seen.insert(from);
    frontier.push_back(from);
    for (uint32_t level = 1; level <= depth; ++level) {
        next_frontier.clear();
        for (auto const &current : frontier) {
            ref_hops(graph, current, orientation, constraint, hops);
            for (auto const &hop : hops) {
                if (!seen.insert(hop.first).second) {
                    continue;
                }
                next_frontier.push_back(hop.first);
            }
        }
        if (next_frontier.empty()) {
            return false;
        }
        if (level == depth) {
            out = next_frontier.front();
            return true;
        }
        frontier = next_frontier;
    }
    return false;
}

// Gate #1: `kParityPairs` seeded draws (from, to, constraint, orientation, depth)
// through the engine and the reference, exact equality of the whole sequence.
void parity_gate(char const *fixture_name, Graph const &graph,
                 kimix::vector<kimix::Guid> const &vertices) {
    if (vertices.empty()) {
        return;
    }
    Constraint const constraints[] = {
        Constraint{}, // no filter: every edge is a hop
        Constraint{}.with(ConstraintCondition::on_label(kLabelR, GdiOp::equal)),
        Constraint{}.with(ConstraintCondition::on_label(kLabelA, GdiOp::equal)),
        Constraint{}.with(ConstraintCondition::on_label(kLabelC, GdiOp::equal)),
        Constraint{}.with(ConstraintCondition::on_label(kLabelD, GdiOp::equal)),
        Constraint{}.with(
            ConstraintCondition::on_property(kPropW, GdiOp::equal, PropertyValue::make_int64(3))),
        Constraint{}
            .with(ConstraintCondition::on_label(kLabelA, GdiOp::equal))
            .with(ConstraintCondition::on_property(kPropW, GdiOp::equal,
                                                   PropertyValue::make_int64(3))),
    };
    std::mt19937 rng(kSeed + 991u);
    size_t compared = 0;
    size_t non_empty = 0;
    bool all_match = true;
    kimix::vector<Graph::PathStep> actual;
    for (size_t round = 0; round < kParityPairs; ++round) {
        kimix::Guid const from = vertices[rng() % vertices.size()];
        Constraint const &constraint = constraints[rng() % (sizeof(constraints) / sizeof(constraints[0]))];
        EdgeOrientation const orientation = static_cast<EdgeOrientation>(rng() % 3u);
        uint32_t const depth = 1u + static_cast<uint32_t>(rng() % 4u);
        // Three draws out of four put the target EXACTLY `depth` hops away along
        // the drawn filter (discovered by the reference BFS, so the draw itself is
        // engine-independent), because a uniformly random target is unreachable
        // from a 100k-vertex DAG in 4 hops — a gate that only ever compares empty
        // answers proves nothing. The fourth draw stays uniformly random, which is
        // what keeps the "no matching chain" answer in the matrix.
        kimix::Guid to{};
        if ((rng() % 4u) == 0u ||
            !find_at_depth(graph, from, constraint, orientation, depth, to)) {
            to = vertices[rng() % vertices.size()];
        }
        GdiStatus const status = graph.shortest_path(from, to, constraint, orientation, depth, actual);
        if (status != GdiStatus::ok) {
            all_match = false;
            continue;
        }
        expect_same_path(actual, ref_filtered_path(graph, from, to, constraint, orientation, depth),
                         fixture_name);
        if (!actual.empty()) {
            ++non_empty;
        }
        ++compared;
    }
    expect(all_match) << fixture_name << ": every parity draw answers ok";
    expect(eq(compared, kParityPairs)) << fixture_name << ": the parity matrix really ran";
    expect(gt(non_empty, size_t{0}))
        << fixture_name << ": the draws did produce reachable pairs (a gate that never finds a "
                            "path proves nothing)";
    std::fprintf(stderr,
                 "[bench] parity %s: %zu pairs compared to the brute force, %zu reachable\n",
                 fixture_name, compared, non_empty);
}


// ---------------------------------------------------------------------------
// The endpoint pairs, derived (F2/F4/F5: known by construction) or discovered by
// a bounded reference BFS (F1/F3). Computed once, before any timed row.
// ---------------------------------------------------------------------------
struct bench_pairs {
    // F1
    kimix::Guid f1_start{};        // a mid-graph vertex (layer 50)
    kimix::Guid f1_1hop{};
    kimix::Guid f1_3hop{};
    kimix::Guid f1_8hop{};
    kimix::Guid f1_back_from{};    // layer 500 -> layer 0: unreachable forward
    kimix::Guid f1_back_to{};
    // F2 (index arithmetic on the known ladder)
    kimix::Guid f2_rail_start{};   // row 0, column 0
    kimix::Guid f2_rail_1hop{};    // row 0, column 1
    kimix::Guid f2_rail_3hop{};    // row 0, column 3
    kimix::Guid f2_prop_start{};   // row 0, column 9  (w == 3 for columns 9..11)
    kimix::Guid f2_prop_end{};     // row 0, column 12
    // F3
    kimix::Guid f3_start{};
    kimix::Guid f3_3hop{};
    // F4
    kimix::Guid f4_hub{};
    kimix::Guid f4_child{};        // a direct child of the hub
    kimix::Guid f4_grandchild{};   // a grandchild behind child 999 (visited last)
    // F5
    kimix::Guid f5_head{};         // the first chain vertex
    kimix::Guid f5_mid{};          // a mid-chain vertex
    kimix::Guid f5_tail{};         // the last chain vertex
    kimix::Guid f5_mid_3hop{};     // three hops ahead of f5_mid (inside the 32-level bound)
};

inline bench_pairs const &pairs() {
    static bench_pairs computed;
    static bool built = false;
    if (built) {
        return computed;
    }
    bench_fixture &f1 = fixture_f1();
    Constraint const any_label = Constraint{}.with(ConstraintCondition::on_label(kLabelR, GdiOp::equal));
    computed.f1_start = f1.vertices[(kF1Vertices / kF1LayerWidth / 2) * kF1LayerWidth];
    expect(find_at_depth(f1.graph, computed.f1_start, any_label, EdgeOrientation::outgoing, 1,
                         computed.f1_1hop))
        << "F1 has a 1-hop pair";
    expect(find_at_depth(f1.graph, computed.f1_start, any_label, EdgeOrientation::outgoing, 3,
                         computed.f1_3hop))
        << "F1 has a 3-hop pair";
    expect(find_at_depth(f1.graph, computed.f1_start, any_label, EdgeOrientation::outgoing, 8,
                         computed.f1_8hop))
        << "F1 has an 8-hop pair";
    computed.f1_back_from = f1.vertices[(kF1Vertices / kF1LayerWidth - 1) / 2 * kF1LayerWidth];
    computed.f1_back_to = f1.vertices[0];
    expect(!(computed.f1_back_from == computed.f1_back_to)) << "the unreachable pair has two ends";

    bench_fixture &f2 = fixture_f2();
    computed.f2_rail_start = f2.vertices[0];
    computed.f2_rail_1hop = f2.vertices[1];
    computed.f2_rail_3hop = f2.vertices[3];
    computed.f2_prop_start = f2.vertices[9];
    computed.f2_prop_end = f2.vertices[12];

    bench_fixture &f3 = fixture_f3();
    computed.f3_start = f3.vertices[kF3Vertices / 2];
    expect(find_at_depth(f3.graph, computed.f3_start, any_label, EdgeOrientation::outgoing, 3,
                         computed.f3_3hop))
        << "F3 has a 3-hop pair";

    bench_fixture &f4 = fixture_f4();
    computed.f4_hub = f4.vertices[0];
    computed.f4_child = f4.vertices[1 + 7];
    computed.f4_grandchild = f4.vertices[1 + kF4Children + (kF4GrandchildParents - 1) * kF4Grandchildren + 3];

    bench_fixture &f5 = fixture_f5();
    computed.f5_head = f5.vertices[0];
    computed.f5_mid = f5.vertices[kF5Vertices / 2];
    computed.f5_tail = f5.vertices[kF5Vertices - 1];
    computed.f5_mid_3hop = f5.vertices[kF5Vertices / 2 + 3];
    built = true;
    return computed;
}

// ---------------------------------------------------------------------------
// Row helpers — one scenario = [bench] ns/op + [mem] allocs/op + [ctr] counters
// ---------------------------------------------------------------------------
// One edge-filtered shortest_path row. `out_size` (optional) reports the step
// count of the LAST measured call, so the caller can assert the workload really
// produced the expected path (a methodology check, not a timing check).
inline void filtered_row(char const *name, Graph const &graph, kimix::Guid const &from,
                         kimix::Guid const &to, Constraint const &constraint,
                         EdgeOrientation orientation, uint32_t depth,
                         size_t *out_size = nullptr) {
    kimix::vector<Graph::PathStep> out;
    auto run = [&] {
        GdiStatus const status = graph.shortest_path(from, to, constraint, orientation, depth, out);
        kimix_bench::sink(static_cast<int32_t>(status));
    };
    kimix_bench::run(name, run, 1, 0.0, kWindow);
    if (out_size != nullptr) {
        *out_size = out.size();
    }
    mem_row(name, run);
    ctr_row(name, run, true);
}

// One filtered has_path row (true or false, the caller says which).
inline void filtered_has_path_row(char const *name, Graph const &graph, kimix::Guid const &from,
                                 kimix::Guid const &to, Constraint const &constraint,
                                 EdgeOrientation orientation, bool expect_true) {
    bool answer = false;
    auto run = [&] {
        answer = graph.has_path(from, to, constraint, orientation);
        kimix_bench::sink(answer);
    };
    kimix_bench::run(name, run, 1, 0.0, kWindow);
    expect(eq(answer, expect_true)) << name << ": the reachability answer is the documented one";
    mem_row(name, run);
    ctr_row(name, run, true);
}

// S6: the UNFILTERED control rows, on the same pairs as the filtered rows.
inline void unfiltered_path_row(char const *name, Graph const &graph, kimix::Guid const &from,
                               kimix::Guid const &to, uint32_t depth, size_t *out_size = nullptr) {
    kimix::vector<kimix::Guid> out;
    auto run = [&] {
        GdiStatus const status = graph.shortest_path(from, to, depth, out);
        kimix_bench::sink(static_cast<int32_t>(status));
    };
    kimix_bench::run(name, run, 1, 0.0, kWindow);
    if (out_size != nullptr) {
        *out_size = out.size();
    }
    mem_row(name, run);
}

inline void unfiltered_has_path_row(char const *name, Graph const &graph, kimix::Guid const &from,
                                    kimix::Guid const &to, bool expect_true) {
    bool answer = false;
    auto run = [&] {
        answer = graph.has_path(from, to);
        kimix_bench::sink(answer);
    };
    kimix_bench::run(name, run, 1, 0.0, kWindow);
    expect(eq(answer, expect_true)) << name << ": the unfiltered answer is the documented one";
    mem_row(name, run);
}

// S11: the two primitives alone.
inline void edge_matches_row(char const *name, Graph const &graph, EdgeId const &edge,
                             Constraint const &constraint) {
    bool answer = false;
    auto run = [&] {
        answer = graph.edge_matches(edge, constraint);
        kimix_bench::sink(answer);
    };
    kimix_bench::run(name, run, 1, 0.0, kWindow);
    expect(answer) << name << ": the probe edge satisfies its own constraint";
    mem_row(name, run);
    ctr_row(name, run, false); // a micro-baseline: no walk, so no walk relations
}

inline void oriented_links_row(char const *name, Graph const &graph, kimix::Guid const &guid,
                               EdgeOrientation orientation) {
    kimix::vector<EdgeLink> links;
    auto run = [&] {
        bool const found = graph.oriented_links(guid, orientation, links);
        kimix_bench::sink(found);
    };
    kimix_bench::run(name, run, 1, 0.0, kWindow);
    expect(gt(links.size(), size_t{0})) << name << ": the snapshot is not empty";
    mem_row(name, run);
    ctr_row(name, run, false);
}

// S10: one C ABI row. `fill` false = the count-only phase (items == nullptr),
// true = the fill phase, which is what a binder does after the probe.
inline void cabi_row(char const *name, cabi_fixture &fixture, uint32_t constraint, uint8_t orientation,
                     uint32_t depth, bool fill) {
    kimix_gdb_path_step steps[16]{};
    size_t count = 0;
    auto run = [&] {
        int32_t const status = kimix_gdb_shortest_path_edges(
            fixture.session, fixture.from, fixture.to, constraint, orientation, depth,
            fill ? steps : nullptr, fill ? 16 : 0, &count);
        kimix_bench::sink(status);
    };
    kimix_bench::run(name, run, 1, 0.0, kWindow);
    if (fill) {
        expect(eq(count, size_t{4})) << name << ": 3 A-hops is a 4-step path";
    } else {
        expect(eq(count, size_t{4})) << name << ": the count-only phase reports 4 steps";
    }
    mem_row(name, run);
}

// S9: one early-out row. `expected` is the status the contract documents; the
// cost must be one early return (and, in the instrumented build, no counter at
// all — the check is inside ctr_row_early_out).
inline void early_out_row(char const *name, Graph const &graph, kimix::Guid const &from,
                          kimix::Guid const &to, Constraint const &constraint,
                          EdgeOrientation orientation, uint32_t depth, GdiStatus expected) {
    kimix::vector<Graph::PathStep> out;
    GdiStatus status = GdiStatus::ok;
    auto run = [&] {
        status = graph.shortest_path(from, to, constraint, orientation, depth, out);
        kimix_bench::sink(static_cast<int32_t>(status));
    };
    kimix_bench::run(name, run, 1, 0.0, kWindow);
    expect(eq(status, expected)) << name << ": the documented early-out status";
    mem_row(name, run);
    ctr_row_early_out(name, run);
}

inline size_t hardware_threads() noexcept {
    unsigned const reported = std::thread::hardware_concurrency();
    return reported > 0 ? static_cast<size_t>(reported) : size_t{4};
}

// Best-effort: pin the measuring thread to ONE logical CPU for the whole run.
// On a hybrid CPU (P-cores + E-cores) Windows migrates a busy thread between core
// types, and that migration changes this host's measured per-op cost by up to 1.35x
// WITHIN a run — a measurement artefact, not a property of the code under test
// (observed: the [cal] row jumping between a ~27 us and a ~34 us plateau while the
// same walk row moves in step). Affinity is advisory: if the call fails, or the
// platform is not Windows, the run continues unpinned and the [cal] rows remain the
// authoritative clock-state signal. Returns the CPU index actually pinned to, or -1.
// This is benchmark methodology only — it changes what is MEASURED, never what the
// measured code does.
inline int pin_measurement_thread() noexcept {
#if defined(KIMIX_PLATFORM_WINDOWS)
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    DWORD const cpus = info.dwNumberOfProcessors;
    if (cpus < 2) {
        return -1;
    }
    // Prefer a core well away from CPU 0 (the one the OS and the timer interrupts
    // favour); on a hybrid part the OS enumerates P-core threads first, so a low
    // index is a P-core, and on a non-hybrid part every index is equivalent.
    DWORD const target = cpus >= 16 ? 4u : 0u;
    if (SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1) << target) == 0) {
        return -1;
    }
    return static_cast<int>(target);
#else
    return -1;
#endif
}

// ---------------------------------------------------------------------------
// The calibration row ([cal]) — pure CPU, no graph_db code
// ---------------------------------------------------------------------------
// The unit problem this solves: this host's clock state is not stable, so a raw
// ns/op from one run is not comparable with the same row from another run (or
// another phase). A [cal] row measures ONE fixed, deterministic workload that
// touches NO graph_db code, repeated at the head of every test block, so
//   normalized_ns = row_ns / cal_ns(nearest preceding [cal] row)
// is a clock-state-free unit. That quotient is what across-phase comparisons use;
// see the file header for the full rule.
//
// The workload: kCalBufferBytes bytes hashed with kimix::hash64 (XXH3) — pure
// arithmetic on a fixed buffer, no allocation, no lock, no graph. kCalRepeats is
// chosen once, on the first call, so one measurement costs ~kCalTargetNs, and it
// is then FROZEN for the whole run: two [cal] rows always measure identical work,
// so a difference between them is the machine and nothing else. Each row takes
// kCalSamples measurements and RECORDS THE MINIMUM (on a contended host every
// disturbance only adds time, so the minimum is the least-disturbed estimate of
// what the machine could do); the median and max are printed beside it so the
// contention that hit the row stays visible.
inline constexpr size_t kCalBufferBytes = 4096;
inline constexpr double kCalTargetNs = 100'000.0; // ~100 us per measurement
inline constexpr size_t kCalSamples = 21;
inline constexpr size_t kCalMaxRows = 32;

struct cal_state {
    struct row {
        char const *label;
        double ns;
    };
    kimix::vector<std::byte> buffer;
    size_t repeats{0};
    row rows[kCalMaxRows]{};
    size_t count{0};
};

inline cal_state &cal() {
    static cal_state state;
    return state;
}

// Choose the repeat count once, so that ONE measurement costs ~kCalTargetNs on
// this machine, and freeze it for the whole run. Two steps are needed because the
// first measurement in a process is cold (page faults, cold caches, a frequency
// ramp): a short probe would freeze a wrong count and every [cal] row would then
// measure a different amount of work in every run — the exact opposite of a
// reference. So: a warmup, a long probe, then one verification pass that rescales
// the count if the measured duration missed the target by more than 25 %.
inline void cal_init() {
    cal_state &state = cal();
    if (state.repeats != 0) {
        return;
    }
    state.buffer.resize(kCalBufferBytes);
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (auto &byte : state.buffer) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        byte = static_cast<std::byte>((x >> 33) & 0xFFu);
    }
    auto hash_once = [&state] {
        kimix_bench::sink(kimix::hash64(state.buffer.data(), state.buffer.size()));
    };
    auto measure = [&hash_once](size_t count) {
        kimix::Clock clock;
        for (size_t i = 0; i < count; ++i) {
            hash_once();
        }
        return clock.toc() * 1e6; // ns
    };
    for (size_t i = 0; i < 256; ++i) { // warmup
        hash_once();
    }
    size_t const probe = 1024;
    double const one_ns = measure(probe) / static_cast<double>(probe);
    size_t repeats = one_ns > 0.0 ? static_cast<size_t>(kCalTargetNs / one_ns + 0.5)
                                  : size_t{1};
    repeats = repeats < 1 ? 1 : (repeats > (1u << 22) ? (1u << 22) : repeats);
    // One verification pass: rescale if the frozen count is off the target by > 25 %.
    double const first = measure(repeats);
    if (first > 0.0 && (first < kCalTargetNs * 0.75 || first > kCalTargetNs * 1.25)) {
        double const scaled = static_cast<double>(repeats) * (kCalTargetNs / first);
        size_t const next = scaled < 1.0 ? size_t{1}
                                         : (scaled > static_cast<double>(1u << 22)
                                                ? size_t{1u << 22}
                                                : static_cast<size_t>(scaled + 0.5));
        repeats = next;
    }
    state.repeats = repeats;
    std::fprintf(stderr,
                 "[cal]   config %zu B hashed x %zu repeats per measurement (~%.1f us, "
                 "probe %.1f ns/hash)\n",
                 kCalBufferBytes, state.repeats, measure(repeats) / 1000.0, one_ns);
}

// One [cal] row: kCalSamples measurements of the frozen workload. The RECORDED
// value is the MINIMUM (the least-disturbed estimate — on a contended host every
// disturbance only ADDS time, so the minimum is the closest thing to "the
// machine's capability right now"), and the median/max are printed next to it so a
// reader can see how much contention the row carried. Informational: it prints and
// records, it never asserts and never changes the exit code.
inline double cal_row(char const *label) {
    cal_init();
    cal_state &state = cal();
    double samples[kCalSamples]{};
    for (size_t s = 0; s < kCalSamples; ++s) {
        kimix::Clock clock;
        for (size_t i = 0; i < state.repeats; ++i) {
            kimix_bench::sink(kimix::hash64(state.buffer.data(), state.buffer.size()));
        }
        samples[s] = clock.toc() * 1e6; // ns
    }
    for (size_t i = 1; i < kCalSamples; ++i) { // insertion sort; kCalSamples is small
        double const value = samples[i];
        size_t j = i;
        while (j > 0 && samples[j - 1] > value) {
            samples[j] = samples[j - 1];
            --j;
        }
        samples[j] = value;
    }
    double const minimum = samples[0];
    double const median = samples[kCalSamples / 2];
    // The spread INSIDE one [cal] row tells the reader which kind of noise the run
    // has: a wide min..max inside the row is short-term jitter, a tight spread with
    // a different level between rows is a sustained clock-state change.
    std::fprintf(stderr, "[cal]   %-14s %12.1f ns/op  (min %.1f median %.1f max %.1f of %zu)\n",
                 label, minimum, minimum, median, samples[kCalSamples - 1],
                 kCalSamples);
    if (state.count < kCalMaxRows) {
        state.rows[state.count++] = cal_state::row{label, minimum};
    }
    return minimum;
}

// The closing summary: min/max over every [cal] row of the run and the drift
// between them. INFORMATIONAL ONLY (no expect, no exit code): a benchmark that
// fails on a loaded CI box is a broken benchmark. RECORD-DEGRADED is a label for
// the reader, not a verdict the harness enforces.
inline void cal_summary() {
    cal_state &state = cal();
    if (state.count == 0) {
        return;
    }
    double minimum = state.rows[0].ns;
    double maximum = state.rows[0].ns;
    char const * min_label = state.rows[0].label;
    char const * max_label = state.rows[0].label;
    for (size_t i = 1; i < state.count; ++i) {
        if (state.rows[i].ns < minimum) {
            minimum = state.rows[i].ns;
            min_label = state.rows[i].label;
        }
        if (state.rows[i].ns > maximum) {
            maximum = state.rows[i].ns;
            max_label = state.rows[i].label;
        }
    }
    double const drift = minimum > 0.0 ? (maximum - minimum) / minimum * 100.0 : 0.0;
    std::fprintf(stderr,
                 "[bench] calibration min %.1f (%s) max %.1f (%s) drift %.2f %%  run quality: %s\n",
                 minimum, min_label, maximum, max_label, drift,
                 drift > 5.0 ? "RECORD-DEGRADED" : "RECORD-OK");
    std::fprintf(stderr,
                 "[bench] calibration normalisation: compare rows as row_ns / cal_ns of the "
                 "nearest preceding [cal] row; %zu [cal] rows recorded\n",
                 state.count);
}

} // namespace tests_unit_graph_db_test_graph_db_bench_edges
using namespace tests_unit_graph_db_test_graph_db_bench_edges;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    // Gated exactly like the other benches (KIMIX_PDQSORT_BENCH, KIMIX_GREP_BENCH,
    // KIMIX_GRAPH_DB_BENCH in test_graph_db_bench.cpp): a plain `xmake test` run —
    // or a bare exe invocation — must do ZERO work, so nothing is registered and
    // this target never slows the default sweep down.
    const char *gate = std::getenv("KIMIX_GRAPH_DB_BENCH");
    if (gate == nullptr || gate[0] == '0') {
        std::fprintf(stderr,
                     "[bench] KIMIX_GRAPH_DB_BENCH not set; skipping the edge-filtered traversal "
                     "benchmark (set KIMIX_GRAPH_DB_BENCH=1 to run it).\n");
        return 0;
    }

    // FIRST thing after the gate: pin the measuring thread (best effort, see
    // pin_measurement_thread). The banner reports what actually happened.
    int const pinned_cpu = pin_measurement_thread();
    size_t const cores = hardware_threads();

    // -----------------------------------------------------------------------
    // 0. Environment banner, once: every recorded number names the platform,
    //    the build mode and the CPU (optimize_plan §3.6 item 4).
    // -----------------------------------------------------------------------
    "bench_edges_env_banner"_test = [cores, pinned_cpu] {
        kimix::string cpu = kimix::cpu_name();
        // The brand string comes straight from CPUID: keep only printable ASCII.
        for (auto &ch : cpu) {
            if (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) > 0x7E) {
                ch = ' ';
            }
        }
        while (!cpu.empty() && cpu.back() == ' ') {
            cpu.pop_back();
        }
        std::fprintf(stderr,
                     "[bench] graph_db bench_edges: mode=%s platform=%s cpu=\"%s\" hw_threads=%zu "
                     "cpu_pin=%d "
                     "graph_shards=%zu edge_shards=%zu walk_counters=%s\n",
#if defined(NDEBUG)
                     "release",
#else
                     "debug",
#endif
#if defined(KIMIX_PLATFORM_WINDOWS)
                     "windows",
#elif defined(KIMIX_PLATFORM_APPLE)
                     "macos",
#else
                     "unix",
#endif
                     cpu.c_str(), cores, pinned_cpu, graph_db::kGraphShardCount,
                     graph_db::kEdgeShardCount,
#if defined(KIMIX_GRAPH_DB_WALK_COUNTERS)
                     "on ([ctr] rows printed; NOT the timing record)"
#else
                     "off (the timing record)"
#endif
        );
        std::fprintf(stderr,
                     "[bench] sizes: F1=%zu vertices  F2=%zu  F3=%zu  F4=%zu  F5=%zu  F6=0  "
                     "C-ABI=%zu  window=%.2f s  parity pairs=%zu\n",
                     kF1Vertices, kF2Vertices, kF3Vertices,
                     1 + kF4Children + kF4GrandchildParents * kF4Grandchildren, kF5Vertices,
                     kCabiColumns * 2, kWindow, kParityPairs);
        // The opening [cal] row: this run's clock-state baseline. Every later row
        // is normalised by the nearest preceding [cal] row (see the file header).
        cal_row("start");
        kimix_bench::sink(cpu.size());
    };

    // -----------------------------------------------------------------------
    // 1. Fixtures + correctness gate #1 (brute-force parity), BEFORE any timing.
    //    Registered first on purpose: Boost.UT runs these in registration order,
    //    so the fixture build cost can never land inside a measured window.
    // -----------------------------------------------------------------------
    "bench_edges_fixtures_and_parity"_test = [] {
        bench_fixture &f1 = fixture_f1();
        bench_fixture &f2 = fixture_f2();
        bench_fixture &f3 = fixture_f3();
        bench_fixture &f4 = fixture_f4();
        bench_fixture &f5 = fixture_f5();
        bench_fixture &f6 = fixture_f6();
        cabi_fixture &cabi = fixture_cabi();
        expect(cabi.session != nullptr) << "the C ABI session exists";

        // Shape checks: the fixtures are what §3.3 says they are.
        expect(eq(f1.vertices.size(), kF1Vertices));
        expect(eq(f2.vertices.size(), kF2Vertices));
        expect(eq(f3.vertices.size(), kF3Vertices));
        expect(eq(f4.vertices.size(),
                  1 + kF4Children + kF4GrandchildParents * kF4Grandchildren));
        expect(eq(f5.vertices.size(), kF5Vertices));
        expect(eq(f6.vertices.size(), size_t{0})) << "F6 is empty";
        expect(gt(f1.sample_edges.size(), size_t{0})) << "F1 produced sample edge ids";

        // Gate #1 on every fixture that has vertices (F6 has nothing to walk).
        parity_gate("F1", f1.graph, f1.vertices);
        parity_gate("F2", f2.graph, f2.vertices);
        parity_gate("F3", f3.graph, f3.vertices);
        parity_gate("F4", f4.graph, f4.vertices);
        parity_gate("F5", f5.graph, f5.vertices);

        // The named pairs really are at the distance the row names claim, and
        // the unreachable pairs really are unreachable. This is the methodology
        // check that keeps S1/S2/S3/S4/S6 honest; it is not a timing assertion.
        bench_pairs const &p = pairs();
        Constraint const any_red =
            Constraint{}.with(ConstraintCondition::on_label(kLabelR, GdiOp::equal));
        Constraint const label_a =
            Constraint{}.with(ConstraintCondition::on_label(kLabelA, GdiOp::equal));
        Constraint const label_c =
            Constraint{}.with(ConstraintCondition::on_label(kLabelC, GdiOp::equal));
        Constraint const label_d =
            Constraint{}.with(ConstraintCondition::on_label(kLabelD, GdiOp::equal));
        Constraint const prop_w3 = Constraint{}.with(ConstraintCondition::on_property(
            kPropW, GdiOp::equal, PropertyValue::make_int64(3)));
        Constraint const and_a_w3 =
            Constraint{}
                .with(ConstraintCondition::on_label(kLabelA, GdiOp::equal))
                .with(ConstraintCondition::on_property(kPropW, GdiOp::equal,
                                                       PropertyValue::make_int64(3)));
        kimix::vector<Graph::PathStep> out;
        // The F5 unreachable pair really does have to expand: the chain walk from
        // its middle towards its head has no matching hop at any of the 32 levels.
        Constraint const label_a_full =
            Constraint{}.with(ConstraintCondition::on_label(kLabelA, GdiOp::equal));
        expect(eq(f5.graph.shortest_path(p.f5_mid, p.f5_head, label_a_full,
                                         EdgeOrientation::outgoing, Graph::kMaxNativeHopDepth,
                                         out),
                  GdiStatus::ok));
        expect(out.empty()) << "S4: the F5 chain pair is unreachable along outgoing edges";

        expect(eq(f1.graph.shortest_path(p.f1_start, p.f1_1hop, any_red,
                                         EdgeOrientation::outgoing, 1, out),
                  GdiStatus::ok));
        expect(eq(out.size(), size_t{2})) << "S1: the F1 pair is exactly 1 hop";
        expect(eq(f1.graph.shortest_path(p.f1_start, p.f1_3hop, any_red,
                                         EdgeOrientation::outgoing, 3, out),
                  GdiStatus::ok));
        expect(eq(out.size(), size_t{4})) << "S2: the F1 pair is exactly 3 hops";
        expect(eq(f1.graph.shortest_path(p.f1_start, p.f1_8hop, any_red,
                                         EdgeOrientation::outgoing, 8, out),
                  GdiStatus::ok));
        expect(eq(out.size(), size_t{9})) << "S3: the F1 pair is exactly 8 hops";
        expect(eq(f1.graph.shortest_path(p.f1_back_from, p.f1_back_to, any_red,
                                         EdgeOrientation::outgoing, Graph::kMaxNativeHopDepth, out),
                  GdiStatus::ok));
        expect(out.empty()) << "S4: the wide F1 pair is unreachable along outgoing edges";

        expect(eq(f2.graph.shortest_path(p.f2_rail_start, p.f2_rail_3hop, label_a,
                                         EdgeOrientation::outgoing, 3, out),
                  GdiStatus::ok));
        expect(eq(out.size(), size_t{4})) << "S2: the F2 A-chain pair is exactly 3 hops";
        expect(eq(f2.graph.shortest_path(p.f2_rail_start, p.f2_rail_3hop, label_c,
                                         EdgeOrientation::outgoing, 3, out),
                  GdiStatus::ok));
        expect(eq(out.size(), size_t{4})) << "S8 label 5 %: a 3-hop C-chain exists";
        expect(eq(f2.graph.shortest_path(p.f2_rail_start, p.f2_rail_3hop, label_d,
                                         EdgeOrientation::outgoing, 3, out),
                  GdiStatus::ok));
        expect(out.empty()) << "S8 label 0 %: no edge carries label D";
        expect(eq(f2.graph.shortest_path(p.f2_prop_start, p.f2_prop_end, prop_w3,
                                         EdgeOrientation::outgoing, 3, out),
                  GdiStatus::ok));
        expect(eq(out.size(), size_t{4})) << "S8 property equality: a 3-hop w==3 chain exists";
        expect(eq(f2.graph.shortest_path(p.f2_prop_start, p.f2_prop_end, and_a_w3,
                                         EdgeOrientation::outgoing, 3, out),
                  GdiStatus::ok));
        expect(eq(out.size(), size_t{4})) << "S8 two-condition AND: the same chain is A and w==3";

        expect(eq(f3.graph.shortest_path(p.f3_start, p.f3_3hop, any_red,
                                         EdgeOrientation::outgoing, 3, out),
                  GdiStatus::ok));
        expect(eq(out.size(), size_t{4})) << "S2: the F3 pair is exactly 3 hops";

        expect(eq(f4.graph.shortest_path(p.f4_hub, p.f4_child, any_red,
                                         EdgeOrientation::outgoing, 1, out),
                  GdiStatus::ok));
        expect(eq(out.size(), size_t{2})) << "S1 hub: the child is exactly 1 hop away";
        expect(eq(f4.graph.shortest_path(p.f4_hub, p.f4_grandchild, any_red,
                                         EdgeOrientation::outgoing, 2, out),
                  GdiStatus::ok));
        expect(eq(out.size(), size_t{3})) << "S2 hub: the grandchild is exactly 2 hops away";

        // S5: has_path true on F1/F2, false on the unreachable pairs. The F5 pair
        // walks from the middle of the chain BACKWARDS, so it really does expand
        // the full 32 levels (starting at the tail would end after one level: the
        // tail has no outgoing link at all).
        expect(f1.graph.has_path(p.f1_start, p.f1_3hop, any_red, EdgeOrientation::outgoing))
            << "S5: the filtered has_path answers true on the F1 3-hop pair";
        expect(!f1.graph.has_path(p.f1_back_from, p.f1_back_to, any_red,
                                  EdgeOrientation::outgoing))
            << "S5: the filtered has_path answers false on the unreachable F1 pair";
        Constraint const label_a_chain =
            Constraint{}.with(ConstraintCondition::on_label(kLabelA, GdiOp::equal));
        expect(!f5.graph.has_path(p.f5_mid, p.f5_head, label_a_chain, EdgeOrientation::outgoing))
            << "S5: F5 walked backwards over a chain is unreachable (the full 32 levels)";
        expect(f5.graph.has_path(p.f5_mid, p.f5_mid_3hop, label_a_chain,
                                 EdgeOrientation::outgoing))
            << "S5: the same chain walked forwards over the SAME filter reaches 3 hops out";

        // S6 control: the unfiltered forms must answer the same pairs (they read
        // the same graph, they just never look at the edge).
        kimix::vector<kimix::Guid> plain;
        expect(eq(f1.graph.shortest_path(p.f1_start, p.f1_3hop, 3, plain), GdiStatus::ok));
        expect(eq(plain.size(), size_t{4})) << "S6: the unfiltered 3-hop pair answers 4 steps";
        expect(f1.graph.has_path(p.f1_start, p.f1_3hop)) << "S6: the unfiltered has_path agrees";

        // S9: the early-outs return the documented status.
        kimix::Guid const unknown{};
        expect(!f1.graph.contains(unknown)) << "the zero guid is not a vertex of F1";
        expect(eq(f1.graph.shortest_path(unknown, p.f1_1hop, any_red,
                                         EdgeOrientation::outgoing, 1, out),
                  GdiStatus::error_vertex));
        expect(eq(f1.graph.shortest_path(p.f1_start, p.f1_1hop, any_red,
                                         EdgeOrientation::outgoing, 0, out),
                  GdiStatus::error_argument));
        expect(eq(f1.graph.shortest_path(p.f1_start, p.f1_1hop, any_red,
                                         EdgeOrientation::outgoing,
                                         Graph::kMaxNativeHopDepth + 1, out),
                  GdiStatus::error_argument));
        Constraint const ordering_op =
            Constraint{}.with(ConstraintCondition::on_label(kLabelR, GdiOp::less));
        expect(eq(f1.graph.shortest_path(p.f1_start, p.f1_1hop, ordering_op,
                                         EdgeOrientation::outgoing, 1, out),
                  GdiStatus::error_op));

        // Everything the measured rows need is built; close the fixture step with a
        // [cal] row so the parity gate's own cost is bracketed as well. The core is
        // NOT pre-heated with graph work on purpose: a warmup injects an unmeasured
        // clock-state change, which is exactly what the [cal] rows expose instead.
        cal_row("after_fixtures");
    };

    // -----------------------------------------------------------------------
    // 2. S1 / S2 / S3 — one, three and eight hops, per call state / crossover /
    //    batching amortization.
    // -----------------------------------------------------------------------
    "bench_edges_s1_s2_s3"_test = [] {
        bench_pairs const &p = pairs();
        Graph &f1 = fixture_f1().graph;
        Graph &f2 = fixture_f2().graph;
        Graph &f3 = fixture_f3().graph;
        Graph &f4 = fixture_f4().graph;
        Constraint const any_red =
            Constraint{}.with(ConstraintCondition::on_label(kLabelR, GdiOp::equal));
        Constraint const label_a =
            Constraint{}.with(ConstraintCondition::on_label(kLabelA, GdiOp::equal));

        size_t steps = 0;
        cal_row("before_S1");
        filtered_row("edges/S1_1hop_label100/F1", f1, p.f1_start, p.f1_1hop, any_red,
                     EdgeOrientation::outgoing, 1, &steps);
        expect(eq(steps, size_t{2})) << "S1/F1 produced a 2-step path";
        filtered_row("edges/S1_1hop_label100/F4", f4, p.f4_hub, p.f4_child, any_red,
                     EdgeOrientation::outgoing, 1, &steps);
        expect(eq(steps, size_t{2})) << "S1/F4 produced a 2-step path";

        filtered_row("edges/S2_3hop_label100/F1", f1, p.f1_start, p.f1_3hop, any_red,
                     EdgeOrientation::outgoing, 3, &steps);
        expect(eq(steps, size_t{4})) << "S2/F1 produced a 4-step path";
        // S6 control IMMEDIATELY after the filtered row it polices: same pair, no
        // filter, same [cal] interval — so "S6 must not regress >5 %" is a
        // same-interval comparison and never crosses the run's drift window.
        unfiltered_path_row("edges/S6_unfiltered_3hop/F1", f1, p.f1_start, p.f1_3hop, 3, &steps);
        expect(eq(steps, size_t{4})) << "S6/F1 answers the same 4-step path as S2";
        filtered_row("edges/S2_3hop_label50/F2", f2, p.f2_rail_start, p.f2_rail_3hop, label_a,
                     EdgeOrientation::outgoing, 3, &steps);
        expect(eq(steps, size_t{4})) << "S2/F2 produced a 4-step path";
        // The S6 control for the F2 cell, same interval, same pair.
        unfiltered_path_row("edges/S6_unfiltered_3hop/F2", f2, p.f2_rail_start, p.f2_rail_3hop, 3,
                            &steps);
        expect(eq(steps, size_t{4})) << "S6/F2 answers the same 4-step path as S2";
        filtered_row("edges/S2_3hop_label100/F3", f3, p.f3_start, p.f3_3hop, any_red,
                     EdgeOrientation::outgoing, 3, &steps);
        expect(eq(steps, size_t{4})) << "S2/F3 produced a 4-step path";
        filtered_row("edges/S2_3hop_hub/F4", f4, p.f4_hub, p.f4_grandchild, any_red,
                     EdgeOrientation::outgoing, 2, &steps);
        expect(eq(steps, size_t{3})) << "S2/F4 produced a 3-step path";

        filtered_row("edges/S3_8hop_label100/F1", f1, p.f1_start, p.f1_8hop, any_red,
                     EdgeOrientation::outgoing, 8, &steps);
        expect(eq(steps, size_t{9})) << "S3/F1 produced a 9-step path";
    };

    // -----------------------------------------------------------------------
    // 3. S4 / S5 — the unreachable worst case (full 32 levels) and has_path.
    // -----------------------------------------------------------------------
    "bench_edges_s4_s5"_test = [] {
        cal_row("before_S4");
        bench_pairs const &p = pairs();
        Graph &f1 = fixture_f1().graph;
        Graph &f2 = fixture_f2().graph;
        Graph &f5 = fixture_f5().graph;
        Constraint const any_red =
            Constraint{}.with(ConstraintCondition::on_label(kLabelR, GdiOp::equal));
        Constraint const label_a =
            Constraint{}.with(ConstraintCondition::on_label(kLabelA, GdiOp::equal));

        size_t steps = 0;
        // F5: backwards along a 100k chain, the full kMaxNativeHopDepth — the
        // has_path(false) pathology row (§2: it materializes the whole reachable
        // set only to discard it) and the §5.8 item 4 peak.
        // From the MIDDLE of the chain (the tail has no outgoing link, so it would
        // end after one level and not measure the full-depth walk at all).
        filtered_row("edges/S4_unreachable/F5", f5, p.f5_mid, p.f5_head, label_a,
                     EdgeOrientation::outgoing, Graph::kMaxNativeHopDepth, &steps);
        expect(eq(steps, size_t{0})) << "S4/F5 is unreachable";
        // F1: the same depth over a WIDE graph — 32 levels of ~100 vertices each
        // is the honest transient-peak row (the chain only reaches 32 vertices).
        filtered_row("edges/S4_unreachable_wide/F1", f1, p.f1_back_from, p.f1_back_to, any_red,
                     EdgeOrientation::outgoing, Graph::kMaxNativeHopDepth, &steps);
        expect(eq(steps, size_t{0})) << "S4/F1 is unreachable";

        filtered_has_path_row("edges/S5_hastrue/F1", f1, p.f1_start, p.f1_3hop, any_red,
                              EdgeOrientation::outgoing, true);
        // S6 control for has_path(true), same pair, same interval.
        unfiltered_has_path_row("edges/S6_unfiltered_hastrue/F1", f1, p.f1_start, p.f1_3hop, true);
        filtered_has_path_row("edges/S5_hastrue/F2", f2, p.f2_rail_start, p.f2_rail_3hop, label_a,
                              EdgeOrientation::outgoing, true);
        filtered_has_path_row("edges/S5_haspfalse/F1", f1, p.f1_back_from, p.f1_back_to, any_red,
                              EdgeOrientation::outgoing, false);
        // S6 control for has_path(false), same pair, same interval.
        unfiltered_has_path_row("edges/S6_unfiltered_haspfalse/F1", f1, p.f1_back_from,
                                p.f1_back_to, false);
        filtered_has_path_row("edges/S5_haspfalse/F5", f5, p.f5_mid, p.f5_head, label_a,
                              EdgeOrientation::outgoing, false);
    };

    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // 5. S7 — orientation: outgoing / incoming / both (the oriented_links copy).
    // -----------------------------------------------------------------------
    "bench_edges_s7_orientation"_test = [] {
        cal_row("before_S7");
        bench_pairs const &p = pairs();
        Graph &f1 = fixture_f1().graph;
        Graph &f4 = fixture_f4().graph;
        Constraint const any_red =
            Constraint{}.with(ConstraintCondition::on_label(kLabelR, GdiOp::equal));

        size_t steps = 0;
        filtered_row("edges/S7_out_3hop/F1", f1, p.f1_start, p.f1_3hop, any_red,
                     EdgeOrientation::outgoing, 3, &steps);
        expect(eq(steps, size_t{4})) << "S7/out walks the 3-hop pair";
        // The same pair walked BACKWARDS: incoming edges from the far end return
        // to the start, because F1's edges are directed layer -> next layer.
        filtered_row("edges/S7_in_3hop/F1", f1, p.f1_3hop, p.f1_start, any_red,
                     EdgeOrientation::incoming, 3, &steps);
        expect(eq(steps, size_t{4})) << "S7/in walks the same pair in reverse";
        filtered_row("edges/S7_both_3hop/F1", f1, p.f1_start, p.f1_3hop, any_red,
                     EdgeOrientation::both, 3, &steps);
        expect(eq(steps, size_t{4})) << "S7/both still finds the 3-hop pair";
        filtered_row("edges/S7_both_2hop/F4", f4, p.f4_hub, p.f4_grandchild, any_red,
                     EdgeOrientation::both, 2, &steps);
        expect(eq(steps, size_t{3})) << "S7/both finds the hub's 2-hop pair";
    };

    // -----------------------------------------------------------------------
    // 6. S8 — the selectivity sweep and the filter classes.
    // -----------------------------------------------------------------------
    "bench_edges_s8_selectivity"_test = [] {
        cal_row("before_S8");
        bench_pairs const &p = pairs();
        Graph &f1 = fixture_f1().graph;
        Graph &f2 = fixture_f2().graph;
        Constraint const any_red =
            Constraint{}.with(ConstraintCondition::on_label(kLabelR, GdiOp::equal));
        Constraint const label_a =
            Constraint{}.with(ConstraintCondition::on_label(kLabelA, GdiOp::equal));
        Constraint const label_c =
            Constraint{}.with(ConstraintCondition::on_label(kLabelC, GdiOp::equal));
        Constraint const label_d =
            Constraint{}.with(ConstraintCondition::on_label(kLabelD, GdiOp::equal));
        Constraint const prop_w3 = Constraint{}.with(ConstraintCondition::on_property(
            kPropW, GdiOp::equal, PropertyValue::make_int64(3)));
        Constraint const and_a_w3 =
            Constraint{}
                .with(ConstraintCondition::on_label(kLabelA, GdiOp::equal))
                .with(ConstraintCondition::on_property(kPropW, GdiOp::equal,
                                                       PropertyValue::make_int64(3)));

        size_t steps = 0;
        filtered_row("edges/S8_label100/F1", f1, p.f1_start, p.f1_3hop, any_red,
                     EdgeOrientation::outgoing, 3, &steps);
        expect(eq(steps, size_t{4})) << "S8 label 100 %";
        filtered_row("edges/S8_label50/F2", f2, p.f2_rail_start, p.f2_rail_3hop, label_a,
                     EdgeOrientation::outgoing, 3, &steps);
        expect(eq(steps, size_t{4})) << "S8 label 50 %";
        filtered_row("edges/S8_label5/F2", f2, p.f2_rail_start, p.f2_rail_3hop, label_c,
                     EdgeOrientation::outgoing, 3, &steps);
        expect(eq(steps, size_t{4})) << "S8 label 5 %";
        filtered_row("edges/S8_label0/F2", f2, p.f2_rail_start, p.f2_rail_3hop, label_d,
                     EdgeOrientation::outgoing, 3, &steps);
        expect(eq(steps, size_t{0})) << "S8 label 0 %: a refused edge is skipped, never discovered";
        filtered_row("edges/S8_prop_eq/F2", f2, p.f2_prop_start, p.f2_prop_end, prop_w3,
                     EdgeOrientation::outgoing, 3, &steps);
        expect(eq(steps, size_t{4})) << "S8 property equality";
        filtered_row("edges/S8_and2/F2", f2, p.f2_prop_start, p.f2_prop_end, and_a_w3,
                     EdgeOrientation::outgoing, 3, &steps);
        expect(eq(steps, size_t{4})) << "S8 two-condition AND";
    };

    // -----------------------------------------------------------------------
    // 7. S9 — the contract early-outs. One early return each, ~0 cost.
    // -----------------------------------------------------------------------
    "bench_edges_s9_early_outs"_test = [] {
        cal_row("before_S9");
        bench_pairs const &p = pairs();
        Graph &f1 = fixture_f1().graph;
        Graph &f6 = fixture_f6().graph;
        Constraint const any_red =
            Constraint{}.with(ConstraintCondition::on_label(kLabelR, GdiOp::equal));
        Constraint const ordering_op =
            Constraint{}.with(ConstraintCondition::on_label(kLabelR, GdiOp::less));
        kimix::Guid const unknown{};

        early_out_row("edges/S9_unknown_endpoint/F1", f1, unknown, p.f1_1hop, any_red,
                      EdgeOrientation::outgoing, 1, GdiStatus::error_vertex);
        early_out_row("edges/S9_depth0/F1", f1, p.f1_start, p.f1_1hop, any_red,
                      EdgeOrientation::outgoing, 0, GdiStatus::error_argument);
        early_out_row("edges/S9_depth33/F1", f1, p.f1_start, p.f1_1hop, any_red,
                      EdgeOrientation::outgoing, Graph::kMaxNativeHopDepth + 1,
                      GdiStatus::error_argument);
        early_out_row("edges/S9_label_op/F1", f1, p.f1_start, p.f1_1hop, ordering_op,
                      EdgeOrientation::outgoing, 1, GdiStatus::error_op);
        // The empty graph: unknown endpoints on a graph with no vertex at all.
        early_out_row("edges/S9_empty_graph/F6", f6, unknown, unknown, any_red,
                      EdgeOrientation::outgoing, 1, GdiStatus::error_vertex);
        expect(!f6.contains(unknown)) << "F6 really is empty";
    };

    // -----------------------------------------------------------------------
    // 8. S10 — the C ABI two-phase protocol (the marshalling overhead: a fresh
    //    vector<PathStep> per call, then a copy into the caller's buffer).
    // -----------------------------------------------------------------------
    "bench_edges_s10_c_abi"_test = [] {
        cal_row("before_S10");
        cabi_fixture &cabi = fixture_cabi();
        cabi_row("edges/S10_cabi_count3hop/F2c", cabi, cabi.constraint_a,
                 KIMIX_GDB_ORIENTATION_OUTGOING, 3, false);
        cabi_row("edges/S10_cabi_fill3hop/F2c", cabi, cabi.constraint_a,
                 KIMIX_GDB_ORIENTATION_OUTGOING, 3, true);
        // The unfiltered C ABI call on the same pair, so the edge-filtered row's
        // extra cost is attributable rather than assumed.
        cabi_row("edges/S10_cabi_fill3hop_nofilter/F2c", cabi, 0u,
                 KIMIX_GDB_ORIENTATION_OUTGOING, 3, true);
    };

    // -----------------------------------------------------------------------
    // 9. S11 — the two primitives alone (the micro-baselines a top-down row is
    //    decomposed against).
    // -----------------------------------------------------------------------
    "bench_edges_s11_primitives"_test = [] {
        cal_row("before_S11");
        bench_pairs const &p = pairs();
        bench_fixture &f1 = fixture_f1();
        bench_fixture &f4 = fixture_f4();
        Constraint const any_red =
            Constraint{}.with(ConstraintCondition::on_label(kLabelR, GdiOp::equal));

        expect(!f1.sample_edges.empty());
        edge_matches_row("edges/S11_edge_matches/F1", f1.graph, f1.sample_edges[0], any_red);
        if (!f4.sample_edges.empty()) {
            edge_matches_row("edges/S11_edge_matches/F4", f4.graph, f4.sample_edges[0], any_red);
        }
        oriented_links_row("edges/S11_oriented_links/F1", f1.graph, p.f1_start,
                           EdgeOrientation::outgoing);
        oriented_links_row("edges/S11_oriented_links/F4", f4.graph, p.f4_hub,
                           EdgeOrientation::outgoing);
        // The closing [cal] row plus the run summary: the same pure-CPU workload as
        // the head of every block, so the whole run is bracketed. Informational only
        // — no expect() and no exit-code change.
        cal_row("end");
        cal_summary();
    };

    return 0;
}
