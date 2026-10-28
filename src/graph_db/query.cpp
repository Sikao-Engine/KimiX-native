/*
 * query.cpp — constraint evaluation (see query.h for the rules and query.h's
 * header comment for why they are what they are).
 *
 * Nothing here locks, allocates on a failure path, or knows about the graph: the
 * caller (Database) collects the candidate entities and their payload under the
 * right lock, then asks these functions.
 */
#include "query.h"

#include <algorithm>

namespace kimix::graph_db {

namespace src_graph_db_query {

// A property condition looks its name up in the entity's bag. Both storage forms
// are searched linearly: a condition list is short and so is a property bag, so a
// hash probe per condition would be more code for the same answer.
[[nodiscard]] PropertyValue const *find_property(PropertyList const &properties,
                                                 PropNameId name) noexcept {
    for (auto const &entry : properties) {
        if (entry.name == name) {
            return &entry.value;
        }
    }
    return nullptr;
}

[[nodiscard]] PropertyValue const *find_property(PropertyBag const &properties,
                                                 PropNameId name) noexcept {
    auto it = properties.find(name);
    if (it == properties.end()) {
        return nullptr;
    }
    return &it->second;
}

template<typename Properties>
[[nodiscard]] bool matches_impl(Constraint const &constraint,
                                kimix::span<const LabelId> labels,
                                Properties const &properties) noexcept {
    for (auto const &condition : constraint.conditions) {
        if (condition.is_label()) {
            bool const present = has_label(labels, condition.label);
            // Only membership semantics exist for a label (Schema rejects any
            // other op at add time); `greater` on a label is false rather than an
            // error here, because this path cannot report one.
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
        // A property condition needs the property to be present, exactly like the
        // reference backend's `properties.TryGetValue` early-out.
        PropertyValue const *actual = find_property(properties, condition.property);
        if (actual == nullptr) {
            return false;
        }
        // The same rule as matches_probe: matches_value for everything (the ordering
        // ops through evaluate, any_in through the condition's probe index), and only
        // after the missing-property short-circuit above.
        if (!condition.matches_value(*actual)) {
            return false;
        }
    }
    return true;
}

} // namespace src_graph_db_query
using namespace src_graph_db_query;

bool has_label(kimix::span<const LabelId> labels, LabelId label) noexcept {
    if (label == kNoLabel) {
        return false;
    }
    return std::find(labels.begin(), labels.end(), label) != labels.end();
}

bool matches(Constraint const &constraint, kimix::span<const LabelId> labels,
             PropertyList const &properties) noexcept {
    if (constraint.conditions.empty()) {
        return true;
    }
    return matches_impl(constraint, labels, properties);
}

bool matches(Constraint const &constraint, kimix::span<const LabelId> labels,
             PropertyBag const &properties) noexcept {
    if (constraint.conditions.empty()) {
        return true;
    }
    return matches_impl(constraint, labels, properties);
}

} // namespace kimix::graph_db
