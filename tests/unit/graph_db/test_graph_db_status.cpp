// Test for the graph_db status surface (src/graph_db/status.*) and the
// capability advertisement (src/graph_db/capability.*).
//
// This test covers:
// - EVERY numeric value of GdiStatus, pinned against Semevia.Gdi.GdiError: the
//   managed adapter switches on those integers, so a renumbering here would be a
//   silent ABI break. The table below is the contract, in one place.
// - status_name() round-trip for all 63 codes and a defined answer for a value
//   outside the table (never nullptr).
// - status_message() non-empty for every enumerator, and the exact text of a few
//   codes the adapter displays verbatim (success, the unique-id warning, the
//   errors the graph layer actually produces).
// - the is_warning / is_error / is_success boundaries (0 = success, 1..2 =
//   warnings, >= 3 = errors).
// - capability advertisement: enforces_unique_external_ids and the two edge
//   flags are on, global scan / multi-hop / aggregates are honestly off, and
//   capability_name_diff() names exactly the flags that differ.
//
// No scheduler is needed: nothing here touches fibers.
#include "ut/ut.hpp"

#include "graph_db_test_util.h"

#include <core/kimix_core.h> // umbrella first: it fixes the winsock2.h order

#include <graph_db/capability.h>
#include <graph_db/graph_db.h>
#include <graph_db/property.h>
#include <graph_db/status.h>

#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <cstdint>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using graph_db::Capability;
using graph_db::GdiOp;
using graph_db::GdiStatus;
using graph_db::PropertyValue;

namespace tests_unit_graph_db_test_graph_db_status {

// The pinned table: (code, name, value). Every enumerator appears exactly once,
// so a missing or renumbered code fails the walk below as well as the table.
struct pinned_status {
    GdiStatus status;
    char const *name;
    int32_t value;
};

kimix::vector<pinned_status> pinned_table() {
    return {
        {GdiStatus::ok, "ok", 0},
        {GdiStatus::warning_non_unique_id, "warning_non_unique_id", 1},
        {GdiStatus::warning_other, "warning_other", 2},
        {GdiStatus::error_access, "error_access", 3},
        {GdiStatus::error_argument, "error_argument", 4},
        {GdiStatus::error_bad_file, "error_bad_file", 5},
        {GdiStatus::error_buffer, "error_buffer", 6},
        {GdiStatus::error_constraint, "error_constraint", 7},
        {GdiStatus::error_conversion, "error_conversion", 8},
        {GdiStatus::error_count, "error_count", 9},
        {GdiStatus::error_database, "error_database", 10},
        {GdiStatus::error_datatype, "error_datatype", 11},
        {GdiStatus::error_date, "error_date", 12},
        {GdiStatus::error_datetime, "error_datetime", 13},
        {GdiStatus::error_decimal, "error_decimal", 14},
        {GdiStatus::error_edge, "error_edge", 15},
        {GdiStatus::error_edge_orientation, "error_edge_orientation", 16},
        {GdiStatus::error_empty_name, "error_empty_name", 17},
        {GdiStatus::error_error_code, "error_error_code", 18},
        {GdiStatus::error_file_exists, "error_file_exists", 19},
        {GdiStatus::error_file_in_use, "error_file_in_use", 20},
        {GdiStatus::error_incompatible_transactions, "error_incompatible_transactions", 21},
        {GdiStatus::error_index, "error_index", 22},
        {GdiStatus::error_label, "error_label", 23},
        {GdiStatus::error_name_exists, "error_name_exists", 24},
        {GdiStatus::error_no_memory, "error_no_memory", 25},
        {GdiStatus::error_no_property, "error_no_property", 26},
        {GdiStatus::error_no_space, "error_no_space", 27},
        {GdiStatus::error_no_such_file, "error_no_such_file", 28},
        {GdiStatus::error_non_unique_id, "error_non_unique_id", 29},
        {GdiStatus::error_not_same, "error_not_same", 30},
        {GdiStatus::error_object_mismatch, "error_object_mismatch", 31},
        {GdiStatus::error_op, "error_op", 32},
        {GdiStatus::error_op_datatype_mismatch, "error_op_datatype_mismatch", 33},
        {GdiStatus::error_property_exists, "error_property_exists", 34},
        {GdiStatus::error_property_type, "error_property_type", 35},
        {GdiStatus::error_property_type_exists, "error_property_type_exists", 36},
        {GdiStatus::error_range, "error_range", 37},
        {GdiStatus::error_read_only_file, "error_read_only_file", 38},
        {GdiStatus::error_read_only_property_type, "error_read_only_property_type", 39},
        {GdiStatus::error_read_only_transaction, "error_read_only_transaction", 40},
        {GdiStatus::error_resource, "error_resource", 41},
        {GdiStatus::error_size, "error_size", 42},
        {GdiStatus::error_size_limit, "error_size_limit", 43},
        {GdiStatus::error_stale, "error_stale", 44},
        {GdiStatus::error_state, "error_state", 45},
        {GdiStatus::error_subconstraint, "error_subconstraint", 46},
        {GdiStatus::error_time, "error_time", 47},
        {GdiStatus::error_transaction, "error_transaction", 48},
        {GdiStatus::error_uid, "error_uid", 49},
        {GdiStatus::error_vertex, "error_vertex", 50},
        {GdiStatus::error_wrong_type, "error_wrong_type", 51},
        {GdiStatus::error_invalid_date, "error_invalid_date", 52},
        {GdiStatus::error_communicator, "error_communicator", 53},
        {GdiStatus::error_block_size, "error_block_size", 54},
        {GdiStatus::error_quota, "error_quota", 55},
        {GdiStatus::error_truncate, "error_truncate", 56},
        {GdiStatus::error_transaction_critical, "error_transaction_critical", 57},
        {GdiStatus::error_transaction_commit_fail, "error_transaction_commit_fail", 58},
        {GdiStatus::error_intern, "error_intern", 59},
        {GdiStatus::error_io, "error_io", 60},
        {GdiStatus::error_other, "error_other", 61},
        {GdiStatus::error_unknown, "error_unknown", 62},
    };
}

bool same_text(kimix::string const &lhs, char const *rhs) {
    return lhs == kimix::string_view{rhs};
}

} // namespace tests_unit_graph_db_test_graph_db_status
using namespace tests_unit_graph_db_test_graph_db_status;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    "status_numeric_values_are_pinned"_test = [] {
        auto table = pinned_table();
        expect(eq(table.size(), size_t{63})) << "0..62, no gaps, no duplicates";
        for (size_t i = 0; i < table.size(); ++i) {
            expect(eq(static_cast<int32_t>(table[i].status), table[i].value))
                << "status " << table[i].name << " must keep its GDI value";
            expect(eq(static_cast<size_t>(table[i].value), i)) << "the table is dense";
        }
        expect(eq(static_cast<int32_t>(graph_db::kLastStatusValue), int32_t{62}));
    };

    "status_name_round_trip"_test = [] {
        auto table = pinned_table();
        for (auto const &entry : table) {
            expect(same_text(kimix::string{graph_db::status_name(entry.status)}, entry.name))
                << "name of value " << entry.value;
        }
        // Out-of-table values answer with a stable placeholder, never nullptr.
        expect(graph_db::status_name(static_cast<GdiStatus>(-1)) != nullptr);
        expect(graph_db::status_name(static_cast<GdiStatus>(99)) != nullptr);
        expect(same_text(kimix::string{graph_db::status_name(static_cast<GdiStatus>(99))},
                         "status_unknown"));
    };

    "status_message_is_never_empty"_test = [] {
        auto table = pinned_table();
        for (auto const &entry : table) {
            auto const message = graph_db::status_message(entry.status);
            expect(ge(message.size(), size_t{3})) << "message of " << entry.name;
        }
        // A few messages the adapter displays verbatim (GdiErrorTable parity).
        expect(same_text(graph_db::status_message(GdiStatus::ok), "Success."));
        expect(same_text(graph_db::status_message(GdiStatus::error_vertex), "Invalid vertex."));
        expect(same_text(graph_db::status_message(GdiStatus::error_edge), "Invalid edge."));
        expect(same_text(graph_db::status_message(GdiStatus::error_no_property),
                         "No such property."));
        expect(same_text(graph_db::status_message(GdiStatus::error_property_exists),
                         "Property already exists."));
        expect(same_text(graph_db::status_message(GdiStatus::error_read_only_transaction),
                         "Transaction is read-only."));
        expect(same_text(graph_db::status_message(GdiStatus::error_size_limit),
                         "Size limit exceeded."));
        // An out-of-table value still yields text.
        expect(!graph_db::status_message(static_cast<GdiStatus>(1234)).empty());
    };

    "status_severity_boundaries"_test = [] {
        expect(!graph_db::is_warning(GdiStatus::ok));
        expect(!graph_db::is_error(GdiStatus::ok));
        expect(graph_db::is_success(GdiStatus::ok));

        expect(graph_db::is_warning(GdiStatus::warning_non_unique_id));
        expect(graph_db::is_warning(GdiStatus::warning_other));
        expect(!graph_db::is_error(GdiStatus::warning_other))
            << "a warning is not an error: the call still succeeded";
        expect(graph_db::is_success(GdiStatus::warning_other));

        expect(!graph_db::is_warning(GdiStatus::error_access))
            << "3 is the first error, not a warning";
        expect(graph_db::is_error(GdiStatus::error_access));
        expect(graph_db::is_error(GdiStatus::error_unknown));
        expect(!graph_db::is_success(GdiStatus::error_unknown));
        expect(!graph_db::is_warning(GdiStatus::error_unknown));
    };

    "capability_is_honest"_test = [] {
        Capability const cap = graph_db::current_capability();
        expect(cap.enforces_unique_external_ids)
            << "the (label, external_id) KeyIndex refuses a duplicate";
        expect(cap.supports_edge_by_stable_uid) << "edges carry a stable uid across save/load";
        expect(cap.supports_edge_property_read) << "edges carry properties";
      expect(cap.supports_collective_transactions) << "a read-only scope is supported";
      // G3 (plan v3 step 1.6): native multi-hop traversal is implemented
      // (Graph::traverse_bfs + the bounded shortest_path, through Database,
      // the C ABI and the kimix_graphdb_* re-exports); the advertised depth is
      // the real bound the implementation validates against.
      expect(cap.supports_native_multi_hop);
      expect(eq(cap.max_native_hop_depth, int32_t{32}));
        // G1/G4 (plan v3 Phase 2): the global scan (Graph::for_each_vertex,
        // Database::scan_vertices) and the fiber-fan-out aggregates are
        // implemented through the C ABI and the C# adapter; the flags are on
        // and the mutation checks below keep them honest.
        expect(cap.supports_global_scan);
        expect(cap.supports_global_aggregates);
    };
  "capability_multi_hop_cannot_silently_regress"_test = [] {
      // Mutation check: flip the G3 flag (or the advertised depth) on a copy
      // and assert the name diff catches exactly that — the same diff the C#
      // Capability.Diff mirror computes, so a silent regression fails loudly
      // on both sides of the seam.
      Capability const live = graph_db::current_capability();
      expect(live.supports_native_multi_hop) << "the suite starts from the advertised true";
      Capability regressed = live;
      regressed.supports_native_multi_hop = false;
      auto diff = graph_db::capability_name_diff(live, regressed);
      expect(eq(diff.size(), size_t{1}));
      expect(same_text(diff[0], "supports_native_multi_hop"));
      Capability shallow = live;
      shallow.max_native_hop_depth = 0;
      diff = graph_db::capability_name_diff(live, shallow);
      expect(eq(diff.size(), size_t{1}));
      expect(same_text(diff[0], "max_native_hop_depth"));
  };

    "capability_diff_names_every_difference"_test = [] {
        Capability base = graph_db::current_capability();
        expect(graph_db::capability_name_diff(base, base).empty())
            << "a capability never differs from itself";
        expect(base == graph_db::current_capability());

        Capability other = base;
        other.supports_global_scan = !base.supports_global_scan;
        other.supports_global_aggregates = !base.supports_global_aggregates;
        other.max_native_hop_depth = base.max_native_hop_depth + 4;
        auto diff = graph_db::capability_name_diff(base, other);
        expect(eq(diff.size(), size_t{3}));
        bool saw_scan = false;
        bool saw_aggregates = false;
        bool saw_depth = false;
        for (auto const &name : diff) {
            saw_scan = saw_scan || same_text(name, "supports_global_scan");
            saw_aggregates = saw_aggregates || same_text(name, "supports_global_aggregates");
            saw_depth = saw_depth || same_text(name, "max_native_hop_depth");
        }
        expect(saw_scan);
        expect(saw_aggregates);
        expect(saw_depth);
        expect(base != other);
    };

    "property_value_kinds_and_ordering"_test = [] {
        // The status suite also pins property.h's GDI vocabulary, because the C
        // ABI numbers these enums directly (graph_c_api.h).
        expect(eq(static_cast<int>(graph_db::GdiOp::equal), 0));
        expect(eq(static_cast<int>(graph_db::GdiOp::not_equal), 1));
        expect(eq(static_cast<int>(graph_db::GdiOp::greater), 2));
        expect(eq(static_cast<int>(graph_db::GdiOp::greater_or_equal), 3));
        expect(eq(static_cast<int>(graph_db::GdiOp::less), 4));
        expect(eq(static_cast<int>(graph_db::GdiOp::less_or_equal), 5));
        expect(eq(static_cast<int>(graph_db::GdiDataType::char_), 100));
        expect(eq(static_cast<int>(graph_db::GdiDataType::decimal), 116));
        expect(eq(static_cast<int>(graph_db::GdiContainerType::single), 0));
        expect(eq(static_cast<int>(graph_db::GdiContainerType::array), 1));

        auto const seven = PropertyValue::make_int64(7);
        auto const nine = PropertyValue::make_double(9.0);
        expect(graph_db::evaluate(GdiOp::less, seven, nine))
            << "int64/double compare across kinds";
        expect(graph_db::evaluate(GdiOp::greater, nine, seven));
        expect(!graph_db::evaluate(GdiOp::greater, seven, seven));
        expect(graph_db::evaluate(GdiOp::equal, seven, PropertyValue::make_int64(7)));
        expect(!graph_db::evaluate(GdiOp::not_equal, seven, PropertyValue::make_int64(7)));

        auto const alpha = PropertyValue::make_string("alpha");
        auto const beta = PropertyValue::make_string("beta");
        expect(graph_db::evaluate(GdiOp::less, alpha, beta)) << "ordinal string compare";
        expect(!graph_db::evaluate(GdiOp::less, beta, alpha));
        expect(graph_db::evaluate(GdiOp::not_equal, alpha, beta));

        std::byte raw[]{std::byte{1}, std::byte{2}};
        auto const blob = PropertyValue::make_blob(kimix::span<const std::byte>{raw, 2});
        expect(graph_db::evaluate(GdiOp::equal, blob, blob));
        // Ordering on an unorderable pair is FALSE, not an error.
        expect(!graph_db::evaluate(GdiOp::greater, blob, blob));
        expect(!graph_db::evaluate(GdiOp::less, blob, seven));
    };
}
