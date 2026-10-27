// Test for the graph_db memory budget (plan v3 Phase 5, step 5.1).
//
// The peak-working-set acceptance target (graphbench 50k persons / 300k edges,
// kimix backend: 425 MB -> <= 300 MB) is gated in step 5.6 by re-running
// graphbench. THIS suite is the per-PR regression guard that makes the budget
// executable between full benchmark runs:
//
// - sizeof probes pin the structural unit costs (EdgeLink, Edge, Vertex), so a
//   field added to any hot struct trips the suite instead of silently growing
//   every entity in the store. Values print on failure and in the log; the
//   ceilings are the step-5.1 freeze (plan_v3.md "Step 5.1").
// - A scaled store (10k keyed vertices + 60k edges, interleaved with an
//   unkeyed control, RSS read through mimalloc's process counters — the same
//   staged-attribution idiom as step 0.3) asserts the per-entity ceilings.
//   Floors catch a broken RSS probe (e.g. a mimalloc change) rather than
//   letting a nonsense zero pass.
//
// The ceilings guard against a STRUCTURAL regression (an extra container, a
// second adjacency copy, a wider node) and sit above the measured noise band of
// each platform rather than at freeze + 20 %: RSS deltas at 10k scale vary up to
// ~10 % run to run, and the derived `key` figure - the difference of two such
// deltas - up to 2.4x (see the band note on the constants). Phase-5 lever steps
// (5.2-5.4) must LOWER the freeze, and step 5.6 re-runs the graphbench gate; the
// freeze value stays the number the plan tracks, so re-tightening a ceiling
// still needs a plan-v3 epoch entry.
#include "ut/ut.hpp"
#include "graph_db_test_util.h"

#include <core/kimix_core.h> // umbrella first among the core headers (winsock2 order)
#include <graph_db/database.h>
#include <graph_db/edge.h>
#include <graph_db/graph_db.h>
#include <graph_db/vertex.h>

#include <core/stl/format.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <mimalloc.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;

namespace tests_unit_graph_db_test_graph_db_memory {

// Step 5.1 freeze — structural unit costs (bytes). EdgeLink is pinned exactly:
// it is the per-adjacency-entry cost and the 5.2 lever's whole subject.
constexpr size_t kFreezeEdgeLinkBytes = 32;
constexpr size_t kFreezeEdgeStructBytes = 256;
// Step 5.2 (adjacency de-dup) shrank Vertex from 512 to 320 by dropping the
// guid-projection vectors and the Vertex* sets — one adjacency representation.
constexpr size_t kFreezeVertexStructBytes = 320;

// Scaled-fixture shape: one fifth of the graphbench dataset.
constexpr size_t kBudgetVertices = 10'000;
constexpr size_t kBudgetEdgesPerVertex = 6;
constexpr size_t kBudgetBatch = 500;

// Per-entity RSS ceilings. The 5.4 freeze (keyed 1085.8 / unkeyed 968.3 /
// key 117.6 / edge 489.1 B at the 10k measurement; 5.2 was 1371.3 / 1129.5 /
// 251.7 / 517.3; 5.1 was 1579.0 / 1292.4 / 286.6 / 607.0) is the number the
// plan tracks; the constants below are deliberately LOOSER than freeze + 20 %
// because the measurement itself is quantized - see the band note.
//
// One freeze, two measurement bands. The attribution is a DELTA over a
// baseline, so it moves with the STL, the allocator AND the allocator's own
// bookkeeping. Repeated runs of the same binary on the same host (22 on MSVC,
// 4 on libstdc++) span:
//   MSVC      keyed 1144-1432 / unkeyed 904-1173 / key 137-388 / edge 485-558
//             / bare edge 319-333 B
//   libstdc++ keyed 1140-1160 / unkeyed  819- 839 / key 301-341 / edge 343-344
//             / bare edge 156-162 B
// i.e. up to ~10 % peak-to-peak on a direct delta and ~2.8x on the derived
// `key` figure (which is the difference of two independently quantized deltas,
// so the two errors add). The ceilings are therefore the observed maximum of
// each platform plus ~15 %, not freeze + 20 %: they catch a STRUCTURAL
// regression (a field added to Vertex or Edge, a second adjacency copy, a
// per-key container) - which is what the sizeof probes above cannot see - and
// not a few percent of allocator slack. Re-tightening needs a stabilizer this
// suite cannot use: RSS only ever grows, so a min-over-passes estimator drifts
// toward the already-committed pages of the previous pass and would trip the
// floors instead.
#ifdef KIMIX_PLATFORM_WINDOWS
constexpr double kCeilVertexKeyed = 1650.0;   // band max 1431.6
constexpr double kCeilVertexUnkeyed = 1350.0; // band max 1172.7
constexpr double kCeilKeyEntry = 450.0;       // band max  387.9 (derived, see above)
constexpr double kCeilEdge = 640.0;           // band max  557.7
// Bare-edge floor: 60 % of the lowest 319.2 B MSVC measurement.
constexpr double kFloorBareEdge = 200.0;
#else
constexpr double kCeilVertexKeyed = 1350.0;   // band max 1160.0
constexpr double kCeilVertexUnkeyed = 1000.0; // band max  838.9
constexpr double kCeilKeyEntry = 420.0;       // band max  340.8 (derived)
constexpr double kCeilEdge = 420.0;           // band max  344.1
// Bare-edge floor: 77 % of the lowest 156.2 B libstdc++ measurement.
constexpr double kFloorBareEdge = 120.0;
#endif
// Floors: 55 % of the freeze — the measurement must see real bytes.
constexpr double kFloorFactor = 0.55;

inline size_t mi_current_rss() noexcept {
    size_t elapsed = 0, user = 0, system = 0, current = 0, peak = 0;
    size_t commit = 0, peak_commit = 0, faults = 0;
    mi_process_info(&elapsed, &user, &system, &current, &peak, &commit, &peak_commit,
                    &faults);
    return current;
}

inline graph_db::PropertyTypeInfo budget_type(kimix::string_view name,
                                              graph_db::GdiDataType data) {
    graph_db::PropertyTypeInfo info;
    info.name = kimix::string{name};
    info.data = data;
    info.count = 1;
    info.container = graph_db::GdiContainerType::single;
    return info;
}

inline bool budget_schema(graph_db::Database &db, graph_db::LabelId &person,
                          graph_db::LabelId &link, graph_db::PropNameId &name_id,
                          graph_db::PropNameId &age_id, graph_db::PropNameId &bio_id,
                          graph_db::PropNameId &weight_id) {
    return db.create_label("Person", person) == graph_db::GdiStatus::ok &&
           db.create_label("LINK", link) == graph_db::GdiStatus::ok &&
           db.create_property_type(budget_type("name", graph_db::GdiDataType::char_),
                                   name_id) == graph_db::GdiStatus::ok &&
           db.create_property_type(budget_type("age", graph_db::GdiDataType::int64_),
                                   age_id) == graph_db::GdiStatus::ok &&
           db.create_property_type(budget_type("bio", graph_db::GdiDataType::char_),
                                   bio_id) == graph_db::GdiStatus::ok &&
           db.create_property_type(budget_type("weight", graph_db::GdiDataType::int64_),
                                   weight_id) == graph_db::GdiStatus::ok;
}

inline kimix::string const &budget_bio() {
    static kimix::string bio = [] {
        kimix::string text;
        while (text.size() < 256) {
            text += "semivia-graph-bench-bio-";
        }
        text.resize(256);
        return text;
    }();
    return bio;
}

inline kimix::vector<kimix::Guid> budget_create_vertices(
    graph_db::Database &db, graph_db::LabelId person, graph_db::PropNameId name_id,
    graph_db::PropNameId age_id, graph_db::PropNameId bio_id, size_t count, bool keyed,
    size_t first_index) {
    kimix::vector<graph_db::LabelId> labels{person};
    kimix::vector<std::byte> key_bytes;
    graph_db::PropertyList props;
    props.push_back({name_id, graph_db::PropertyValue{}});
    props.push_back({age_id, graph_db::PropertyValue{}});
    props.push_back({bio_id, graph_db::PropertyValue{}});
    kimix::vector<kimix::Guid> out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        size_t const index = first_index + i;
        kimix::string const name = kimix::format("person {}", index);
        props[0].value = graph_db::PropertyValue::make_string(name);
        props[1].value =
            graph_db::PropertyValue::make_int64(static_cast<int64_t>(index % 90));
        props[2].value = graph_db::PropertyValue::make_string(budget_bio());
        graph_db::VertexKeyView key{};
        if (keyed) {
            key_bytes = graph_db::test::to_bytes(kimix::format("person-{}", index));
            key = graph_db::VertexKeyView{
                person, kimix::span<const std::byte>{key_bytes.data(), key_bytes.size()}};
        }
        graph_db::VertexCreateResult created;
        if (db.create_vertex(key, kimix::span<const graph_db::LabelId>{labels.data(),
                                                                       labels.size()},
                             &props, created) != graph_db::GdiStatus::ok) {
            break;
        }
        out.push_back(created.uid);
    }
    return out;
}

} // namespace tests_unit_graph_db_test_graph_db_memory
using namespace tests_unit_graph_db_test_graph_db_memory;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));
    // Structural probes: the per-entity freeze in one place. Printed so the
    // graphbench-scale attribution and this suite cannot drift apart silently.
    "sizeof_probes"_test = [] {
        std::fprintf(stderr, "[mem] sizeof(EdgeLink)=%zu sizeof(Edge)=%zu sizeof(Vertex)=%zu\n",
                     sizeof(graph_db::EdgeLink), sizeof(graph_db::Edge),
                     sizeof(graph_db::Vertex));
        expect(eq(sizeof(graph_db::EdgeLink), kFreezeEdgeLinkBytes))
            << "EdgeLink is the per-adjacency-entry cost; step 5.2 lowers it "
               "deliberately, so this exact pin must move WITH that epoch";
#if defined(NDEBUG) // debug builds inflate the STL members (debug iterators)
        expect(le(sizeof(graph_db::Edge), kFreezeEdgeStructBytes))
            << "Edge struct grew past the step-5.1 freeze";
        expect(le(sizeof(graph_db::Vertex), kFreezeVertexStructBytes))
            << "Vertex struct grew past the step-5.1 freeze";
#endif
    };

    // Scaled-store per-entity ceilings (see the file header for the freeze
// values and why the ceilings sit ~20 % above them). The freeze is a
// RELEASE measurement (the 5.6 gate is a release graphbench run); debug
// builds inflate the containers (debug iterators), so the ceiling body
// is release-only. The sizeof probes above still pin debug builds.
#if defined(NDEBUG)
"scaled_store_ceiling"_test = [] {
        graph_db::Database db_keyed;
        graph_db::LabelId person{}, link{};
        graph_db::PropNameId name_id{}, age_id{}, bio_id{}, weight_id{};
        expect(budget_schema(db_keyed, person, link, name_id, age_id, bio_id, weight_id))
            << "the keyed store's schema is created";

        graph_db::Database db_unkeyed;
        graph_db::LabelId person2{}, link2{};
        graph_db::PropNameId name2{}, age2{}, bio2{}, weight2{};
        expect(budget_schema(db_unkeyed, person2, link2, name2, age2, bio2, weight2))
            << "the control store's schema is created";

        kimix::vector<kimix::Guid> keyed;
        keyed.reserve(kBudgetVertices);
        size_t keyed_acc = 0;
        size_t unkeyed_acc = 0;
        for (size_t base = 0; base < kBudgetVertices; base += kBudgetBatch) {
            size_t const rss0 = mi_current_rss();
            auto keyed_batch = budget_create_vertices(db_keyed, person, name_id, age_id,
                                                      bio_id, kBudgetBatch, true, base);
            size_t const rss1 = mi_current_rss();
            auto unkeyed_batch = budget_create_vertices(db_unkeyed, person2, name2,
                                                        age2, bio2, kBudgetBatch, false, base);
            size_t const rss2 = mi_current_rss();
            keyed_acc += rss1 - rss0;
            unkeyed_acc += rss2 - rss1;
            keyed.insert(keyed.end(), keyed_batch.begin(), keyed_batch.end());
            expect(eq(keyed_batch.size(), kBudgetBatch)) << "the keyed batch is complete";
            expect(eq(unkeyed_batch.size(), kBudgetBatch))
                << "the control batch is complete";
        }
        expect(eq(keyed.size(), kBudgetVertices)) << "every keyed vertex was created";
        size_t const rss_after_vertices = mi_current_rss();

        {
            kimix::vector<graph_db::LabelId> edge_labels{link};
            graph_db::PropertyList edge_props;
            edge_props.push_back({weight_id, graph_db::PropertyValue{}});
            size_t edges = 0;
            for (size_t i = 0; i < keyed.size(); ++i) {
                edge_props[0].value =
                    graph_db::PropertyValue::make_int64(static_cast<int64_t>(i % 1000));
                for (size_t k = 0; k < kBudgetEdgesPerVertex; ++k) {
                    size_t const target = (i + 1 + k * 7) % keyed.size();
                    kimix::Guid edge;
                    if (db_keyed.edge_create(keyed[i], keyed[target],
                                             graph_db::EdgeDirectionType::directed,
                                             kimix::span<const graph_db::LabelId>{
                                                 edge_labels.data(), edge_labels.size()},
                                             &edge_props, edge) == graph_db::GdiStatus::ok) {
                        ++edges;
                    }
                }
            }
            expect(eq(edges, kBudgetVertices * kBudgetEdgesPerVertex))
                << "every staged edge was created";
        }
        size_t const after_edges = mi_current_rss();

        double const keyed_bytes = static_cast<double>(keyed_acc);
        double const unkeyed_bytes = static_cast<double>(unkeyed_acc);
        double const key_bytes = keyed_bytes - unkeyed_bytes;
        double const n = static_cast<double>(kBudgetVertices);

        double const per_keyed = keyed_bytes / n;
        double const per_unkeyed = unkeyed_bytes / n;
        double const per_key = key_bytes / n;
        // Edge delta: RSS after the edge stage minus RSS right before it —
        // schema and both stores' vertices are already committed, so the
        // difference attributes to exactly the 60k edges on the keyed store.
        double const edge_total =
            static_cast<double>(after_edges - rss_after_vertices);
        double const per_edge =
            edge_total / (n * static_cast<double>(kBudgetEdgesPerVertex));

        std::fprintf(stderr,
                     "[mem] per-entity: keyed=%.1f B unkeyed=%.1f B key=%.1f B edge=%.1f B\n",
                     per_keyed, per_unkeyed, per_key, per_edge);

        expect(le(per_keyed, kCeilVertexKeyed))
            << "keyed vertex bytes over the step-5.1 ceiling";
        expect(ge(per_keyed, 1085.8 * kFloorFactor))
            << "keyed vertex measurement looks broken (floor)";
        expect(le(per_unkeyed, kCeilVertexUnkeyed))
            << "unkeyed vertex bytes over the step-5.1 ceiling";
        expect(ge(per_unkeyed, 968.3 * kFloorFactor))
            << "unkeyed vertex measurement looks broken (floor)";
        expect(le(per_key, kCeilKeyEntry)) << "key entry bytes over the step-5.1 ceiling";
        // Key entries amortize sub-linearly (the index hash table grows in
        // jumps: 133.5 B/key at this 10k scale vs 286.6 B at the 50k freeze),
        // so the floor is a fixed small value: it exists to catch a broken
        // RSS probe, not to model the table's growth curve.
        expect(ge(per_key, 60.0)) << "key measurement looks broken (floor)";
        expect(le(per_edge, kCeilEdge)) << "edge bytes over the step-5.1 ceiling";
        expect(ge(per_edge, 489.1 * kFloorFactor)) << "edge measurement looks broken (floor)";

        // Bare-edge attribution (plan v3 §5.4): the same edge count with NO
        // label and NO property isolates the payload containers — the delta
        // against per_edge is what the label + weight cost on the full edge.
        // Two fresh vertices carry all 60k parallel edges, so the link-vector
        // slack profile differs from the 6-per-vertex build; the delta is
        // still the honest per-edge payload-container cost.
        graph_db::Database db_bare;
        graph_db::LabelId bare_person{}, bare_link{};
        graph_db::PropNameId bn{}, ba{}, bb{}, bw{};
        expect(budget_schema(db_bare, bare_person, bare_link, bn, ba, bb, bw))
            << "the bare store's schema is created";
        kimix::Guid bare_a;
        kimix::Guid bare_b;
        {
            kimix::vector<graph_db::LabelId> labels{bare_person};
            graph_db::VertexCreateResult created;
            expect(eq(db_bare.create_vertex(graph_db::VertexKeyView{},
                                            kimix::span<const graph_db::LabelId>{
                                                labels.data(), labels.size()},
                                            nullptr, created),
                      graph_db::GdiStatus::ok));
            bare_a = created.uid;
            expect(eq(db_bare.create_vertex(graph_db::VertexKeyView{},
                                            kimix::span<const graph_db::LabelId>{
                                                labels.data(), labels.size()},
                                            nullptr, created),
                      graph_db::GdiStatus::ok));
            bare_b = created.uid;
        }
        size_t const rss_before_bare = mi_current_rss();
        {
            size_t bare_edges = 0;
            for (size_t i = 0; i < kBudgetVertices * kBudgetEdgesPerVertex; ++i) {
                kimix::Guid edge;
                if (db_bare.edge_create(bare_a, bare_b,
                                        graph_db::EdgeDirectionType::directed, {}, nullptr,
                                        edge) == graph_db::GdiStatus::ok) {
                    ++bare_edges;
                }
            }
            expect(eq(bare_edges, kBudgetVertices * kBudgetEdgesPerVertex))
                << "every bare edge was created";
        }
        size_t const rss_after_bare = mi_current_rss();
        double const per_edge_bare =
            static_cast<double>(rss_after_bare - rss_before_bare) /
            (static_cast<double>(kBudgetVertices) *
             static_cast<double>(kBudgetEdgesPerVertex));
        std::fprintf(stderr, "[mem] per-edge bare=%.1f B payload_containers=%.1f B\n",
                     per_edge_bare, per_edge - per_edge_bare);
        expect(ge(per_edge_bare, kFloorBareEdge))
            << "bare edge measurement looks broken (floor)";
    };
#endif // defined(NDEBUG)
}
