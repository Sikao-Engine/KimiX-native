/*
 * query.h — constraint evaluation and the two-phase page contract (GDI §2.6).
 *
 * Two things live here, both pure functions over already-copied data (no locks,
 * no graph access — the Database does the locking):
 *
 *   matches(...)  the AND-ed condition evaluation of Semevia.Gdi's
 *                 InMemoryStore.Matches, over a label set and a property bag.
 *   Page<T>       the GDI page shape: `items` (at most `cap` rows) plus
 *                 `total_count` (the FULL match count). cap == 0 yields no items
 *                 and the real count — that is how CountNeighbors counts without
 *                 pulling a neighbourhood, and how the C ABI's two-phase buffer
 *                 protocol learns how big a buffer it needs.
 *
 * EVALUATION RULES (copied from the reference, not invented):
 *   - a LABEL condition tests membership; only equal/not_equal are meaningful and
 *     any other op is rejected when the condition is ADDED (Schema: error_op),
 *     because a filter that silently evaluated `greater` on a label would lie;
 *   - a PROPERTY condition needs the property to be present and then compares via
 *     ConstraintCondition::matches_value(): evaluate() for the ordering/equality ops
 *     (numeric cross-kind ordering, ordinal strings, equality-only for blob/arrays),
 *     and the condition's own sorted probe index for ANY_IN — built once when the
 *     condition was added, so a set membership never costs a scan per edge (plan
 *     §1.2). The missing-property short-circuit below stays FIRST: a property that
 *     is absent fails before any probe, indexed or not;
 *   - the conditions are AND-ed; an empty constraint matches everything.
 *
 * Example:
 *   kimix::graph_db::Constraint constraint = kimix::graph_db::Constraint{}.with(
 *       kimix::graph_db::ConstraintCondition::on_label(person, kimix::graph_db::GdiOp::equal));
 *   kimix::graph_db::EdgePage page;
 *   db.get_edges_of_vertex(uid, constraint, kimix::graph_db::EdgeOrientation::outgoing,
 *                          0, page);          // page.total_count is the match count
 */
#pragma once

#include "edge.h"
#include "graph_db_api.h"
#include "property.h"
#include "schema.h"

#include <core/guid.h>
#include <core/stl/vector.h>

#include <cstddef>
#include <cstdint>

namespace kimix::graph_db {

// One page of a capped read. `total_count` is always the FULL match count, so a
// caller can size its buffer from a cap-0 call and then fetch with cap ==
// total_count (GDI's own size-query/fetch idiom).
template<typename T>
struct Page {
    kimix::vector<T> items;
    size_t total_count{0};

    [[nodiscard]] bool empty() const noexcept { return items.empty(); }
};

using VertexPage = Page<kimix::Guid>;
using EdgePage = Page<EdgeRecord>;

/*
 * Fill one page from an already collected match list: `items` gets the first
 * min(cap, matches) entries, `total_count` the full size. `matches` is consumed
 * (moved from) so a large neighbourhood is not copied twice.
 */
template<typename T>
[[nodiscard]] Page<T> make_page(kimix::vector<T> &&matches, int64_t cap) {
    Page<T> page;
    page.total_count = matches.size();
    if (cap < 0) {
        return page; // the caller validates the cap; a negative one yields nothing
    }
    size_t const take = static_cast<size_t>(cap);
    if (take == 0) {
        return page; // count probe: no items, the real total
    }
    if (matches.size() <= take) {
        page.items = std::move(matches);
        return page;
    }
      page.items.reserve(take);
      for (size_t i = 0; i < take; ++i) {
          page.items.push_back(std::move(matches[i]));
      }
      return page;
  }

  // ---------------------------------------------------------------------------
  // Global aggregates (GDI G4)
  // ---------------------------------------------------------------------------
  // Which reduction Database::aggregate_vertices applies.
  enum class GdiAggregate : int32_t { count = 0, min = 1, max = 2, sum = 3, avg = 4 };
  // One aggregate request: the vertex set (constraint, empty = every vertex), the
  // property to reduce (ignored by count), and the reduction. The property must
  // be a REGISTERED property type for min/max/sum/avg: the declared dtype decides
  // which reductions are legal (sum/avg need int64_/double_, min/max also accept
  // char_ for the ordinal string order) — anything else is refused with error_op
  // BEFORE the scan, and an unknown name id with error_property_type.
  struct AggregateSpec {
      Constraint constraint;           // empty = every vertex
      PropNameId property{kNoPropName}; // ignored by count
      GdiAggregate function{GdiAggregate::count};
  };
  // The aggregate outcome. `matched` counts the vertices satisfying the
  // constraint; `valued` the subset of those CARRYING the property (a missing
  // property is skipped, like SQL's AVG ignoring NULLs). `has_value` is false
  // exactly when the reduction had no input rows (valued == 0, or matched == 0
  // for count), and `value` is the payload:
  //   count -> int64(matched)
  //   min/max -> the extremal value, kind preserved (strings ordinal)
  //   sum   -> int64 for an int64_ declaration, double for double_
  //   avg   -> always double ((sum of every contributing value) / valued)
  // `value` is none when !has_value; count's zero is read from `matched`.
  struct AggregateResult {
      uint64_t matched{0};
      uint64_t valued{0};
      bool has_value{false};
      PropertyValue value;
  };

  // Does an entity with these labels/properties satisfy the constraint?
// `properties` must be sorted by name for the binary search used below... it is
// not: the bag is small and a linear scan is what the reference does, so a linear
// scan it is (documented: a condition list is short, a property bag is short).
[[nodiscard]] KIMIX_GRAPH_DB_API bool
matches(Constraint const &constraint, kimix::span<const LabelId> labels,
        PropertyList const &properties) noexcept;

// The same test over a PropertyBag (the vertex-side storage form).
[[nodiscard]] KIMIX_GRAPH_DB_API bool
matches(Constraint const &constraint, kimix::span<const LabelId> labels,
        PropertyBag const &properties) noexcept;

// Convenience: does the label set contain `label`? (linear; a label set is tiny)
[[nodiscard]] KIMIX_GRAPH_DB_API bool
has_label(kimix::span<const LabelId> labels, LabelId label) noexcept;

// PLAN V3 §6.6 zero-copy form: `probe(name)` returns the BORROWED value or
// nullptr (Vertex::find_property supplies it). Identical semantics to the two
// owned-forms above - a condition is an AND of get-by-name probes - but hot
// loops (aggregate fan-out, filtered scans/traversals) evaluate a constraint
// per vertex without deep-copying the property payload.
template <typename Probe>
[[nodiscard]] bool matches_probe(Constraint const &constraint,
                                 kimix::span<const LabelId> labels,
                                 Probe &&probe) noexcept {
    for (auto const &condition : constraint.conditions) {
        if (condition.is_label()) {
            bool const present = has_label(labels, condition.label);
            if (condition.op == GdiOp::equal) {
                if (!present) {
                    return false;
                }
            } else if (condition.op == GdiOp::not_equal) {
                if (present) {
                    return false;
                }
            } else {
                return false;
            }
            continue;
        }
        PropertyValue const *actual = probe(condition.property);
        if (actual == nullptr) {
            return false;
        }
        if (!condition.matches_value(*actual)) {
            return false;
        }
    }
    return true;
}

} // namespace kimix::graph_db
