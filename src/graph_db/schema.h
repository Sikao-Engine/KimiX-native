/*
 * schema.h — the schema registry of kimix-graph-db (GDI §2.5).
 *
 * One Schema per Database holds the three registries the GDI command layer
 * addresses by NAME and the entities address by ID:
 *
 *   labels         name <-> LabelId          (Create/Find/GetAll/Update/Free)
 *   property types name <-> PropNameId + the (container, dtype, count)
 *                  declaration                 (Create/Find/GetAll/Update/Free)
 *   constraints    ConstraintId -> AND-ed list of label/property conditions
 *
 * IDS ARE DENSE AND NEVER RECYCLED. Allocating an id is ++counter, so a freed
 * name leaves a dead id behind forever; that is what makes a stale id in an
 * entity's label set (or in a saved record) a detectable error instead of a
 * silent alias of a new name. Both counters are persisted in the meta record, so
 * a save/load cycle continues the space instead of restarting it.
 *
 * RENAME IS O(1) ON ENTITIES by construction: entities store ids, the registry
 * stores names. Renaming therefore rewrites one name table entry and touches no
 * vertex and no edge — the opposite of the C# reference backend, which has to
 * walk every entity because it stores strings. Freeing IS O(n) on entities and
 * deliberately so: the caller (Database) walks the graph and strips the id.
 *
 * CONCURRENCY. One kimix::shared_atomic_mutex guards all three registries. It is
 * the 4-byte shared/exclusive SPIN lock (core/shared_atomic_mutex.h), not
 * std::shared_mutex, because it passes that header's admission rule: every
 * holder's section is one of
 *   - a hash-map probe (find, resolve, has and the version-free reads): the probe
 *     key is built BEFORE the lock, so the section allocates nothing at all;
 *   - a bounded registry mutation (create/rename/free/update, add condition): one
 *     map op, one vector op and a relaxed version bump;
 *   - a copy of one registry entry or of one constraint (bounded by that entry).
 * The sections that are NOT short run in the loader's QUIESCENT phase:
 * snapshot() (a full copy of both registries) and restore()/restore_label()/
 * restore_property_type() (a full rebuild) — a load/save runs them while no other
 * thread touches this schema, and a spin lock nobody waits on is free. That is
 * the whole exception list; do not put a new long section on this lock without
 * re-reading the admission rule.
 * It is LEVEL 1 of the database's lock hierarchy (see graph.h / README.md): a
 * caller may hold no other database lock when it takes this one, and every method
 * below releases it before returning. No method here calls back into the graph,
 * which is what keeps that rule trivially true. _version is a relaxed atomic, so
 * version()/bump_version() take NO lock and add no acquisition to any path.
 *
 * SHARED RECURSION WARNING: the atomic type counts a second lock_shared() on the
 * same thread instead of refusing it (UB on std::shared_mutex). No method here
 * re-enters the shared lock — in particular resolve_*() releases it BEFORE it
 * calls create_*() — and a new one must keep doing that.
 *
 * Example:
 *   kimix::graph_db::Schema schema;
 *   kimix::graph_db::LabelId person{};
 *   schema.create_label("person", person);
 *   kimix::graph_db::PropertyTypeInfo info;
 *   info.name = "age";
 *   info.data = kimix::graph_db::GdiDataType::int64;
 *   kimix::graph_db::PropNameId age{};
 *   schema.create_property_type(info, age);
 */
#pragma once

#include "graph_db_api.h"
#include "property.h"
#include "status.h"

#include <core/guid.h>
#include <core/memory.h>
#include <core/shared_atomic_mutex.h> // kimix::shared_atomic_mutex + its RAII guards
#include <core/stl/memory.h>         // kimix::shared_ptr: the published registry view
#include <core/stl/string.h>
#include <core/stl/unordered_map.h>
#include <core/stl/vector.h>
#include <atomic> // std::atomic<uint32_t> _version/_generation (lock-free counters)
#include <cstddef>
#include <cstdint>
#include <cstdint>

namespace kimix::graph_db {

// ---------------------------------------------------------------------------
// Property-type declaration rules (ABI 8, plan §1.2)
// ---------------------------------------------------------------------------

// True for the three wire values of GdiSizeType (fixed/max/unlimited). A C
// caller can pass any uint8_t, so an out-of-range cast must be caught, not
// stored.
[[nodiscard]] KIMIX_GRAPH_DB_API bool is_valid_size_type(GdiSizeType type) noexcept;

// The declaration-side rules, in the caller-visible error order:
// error_argument  an out-of-range size type, a `single` declaration whose
// size type is not fixed (a single value has no length to
// bound), or a `single` declaration of the ABI-9 `data` sentinel
// (a self-describing array needs an ARRAY to describe)
// error_count a fixed/max count < 1 (unlimited carries no bound, so its
// count is ignored and normalized to 0)
// ok otherwise — an ARRAY declaration of any datatype, `data` included
// create_property_type / update_property_type apply exactly this, and
// Database::update_property_type calls it to keep the error order of its own
// early checks identical — there is no second copy of the rule.
[[nodiscard]] KIMIX_GRAPH_DB_API GdiStatus
validate_property_type_declaration(PropertyTypeInfo const &info) noexcept;

// ---------------------------------------------------------------------------
// Constraints
// ---------------------------------------------------------------------------

/*
 * The probe index of an ANY_IN property condition (plan §1.2) — the SET the
 * condition tests membership against, prepared ONCE when the condition is built so
 * a filter never scans the raw set per edge (the review rule: no linear probe on
 * the walk's hot path).
 *
 * Two sorted, de-duplicated arrays and the discriminator saying which one is live:
 *   strings   — a string_array set, ORDINAL BYTE ORDER (kimix::string's operator<
 *               is memcmp-ordered, the same order evaluate() uses for text);
 *   integers — an int64_array set, ascending numeric order;
 *   none      — the condition value is not a set (a scalar, an empty value, or a
 *               hand-built condition that never went through on_property).
 *
 * An EMPTY set keeps its shape with no elements: the condition is legal and can
 * never match, which is exactly what a binary search in an empty array answers.
 *
 * VALUE SEMANTICS. Every member owns its bytes (kimix::vector of kimix::string /
 * int64_t), so the compiler-generated copy ctor and assignment of this struct — and
 * of ConstraintCondition and Constraint — are deep by construction. Constraint is
 * copied by Constraint::with and by the session registry, so that matters; a large
 * set therefore copies once per snapshot, never per probe.
 */
struct KIMIX_GRAPH_DB_API AnyInIndex {
    enum class Shape : uint8_t {
        none = 0,
        strings = 1,
        integers = 2,
    };

    Shape shape{Shape::none};
    kimix::vector<kimix::string> strings;
    kimix::vector<int64_t> integers;

    // Build from the condition's VALUE. Only the two array kinds become a set; any
    // other kind yields Shape::none (and the condition then answers false, matching
    // any_in_matches). Allocation failure aborts by design (kimix allocator).
    [[nodiscard]] static AnyInIndex build(PropertyValue const &set_value);

    // Binary-search probes. A probe against the WRONG shape is false — a string is
    // never a member of an int64 set and the inverse — so the shape check is part of
    // the answer, not an assertion.
    [[nodiscard]] bool contains(kimix::string_view text) const noexcept;
    [[nodiscard]] bool contains(int64_t value) const noexcept;
};

// One condition of a Constraint: a label membership test (Equal/NotEqual only,
// any other op is error_op) or a property comparison (any GdiOp, evaluated by
// property.h's evaluate() — or, for any_in, by the condition's own probe index;
// see matches_value).
struct KIMIX_GRAPH_DB_API ConstraintCondition {
    enum class Kind : uint8_t {
        label = 0,
        property = 1,
    };

    Kind kind{Kind::label};
    LabelId label{kNoLabel};
    PropNameId property{kNoPropName};
    GdiOp op{GdiOp::equal};
    PropertyValue value;
    // Array length a property condition applies to (GDI's count); 1 for a
    // scalar declaration. Participates in the comparison only for array types. For
    // an any_in condition it is the SET's exact element count (§1.4) — and there 0
    // is a real value: an empty set that can never match.
    uint32_t count{1};
    // The ANY_IN probe index, built with the condition (see AnyInIndex). Empty for
    // every other op, where it costs nothing and is never read.
    AnyInIndex any_in_set;

    [[nodiscard]] bool is_label() const noexcept { return kind == Kind::label; }
    [[nodiscard]] bool is_property() const noexcept { return kind == Kind::property; }

    // Does the property value `actual` satisfy this condition? For op == any_in this
    // probes `any_in_set` — O(log n) per scalar and per element of an array actual,
    // with the SAME semantics as property.h's any_in_matches (which is what it falls
    // back to when a hand-built condition carries a set but no index). Every other
    // op is evaluate(op, actual, value) unchanged. A missing property is NOT handled
    // here: the callers (query.h/query.cpp) keep the null-probe short-circuit, so an
    // absent property never reaches a probe of any kind.
    [[nodiscard]] bool matches_value(PropertyValue const &actual) const noexcept;

    // Both factories leave `any_in_set` consistent with `value` — on_property builds
    // the index whenever op == any_in, so a direct ConstraintCondition construction
    // (a test, a bench, Constraint::with) is covered exactly like the Schema path.
    static ConstraintCondition on_label(LabelId label, GdiOp op = GdiOp::equal) noexcept;
    static ConstraintCondition on_property(PropNameId property, GdiOp op, PropertyValue value,
                                           uint32_t count = 1) noexcept;
};

// An AND-ed list of conditions: the only structured filter the GDI read
// commands accept. An empty constraint matches everything.
struct KIMIX_GRAPH_DB_API Constraint {
    kimix::vector<ConstraintCondition> conditions;

    [[nodiscard]] bool empty() const noexcept { return conditions.empty(); }
    [[nodiscard]] size_t size() const noexcept { return conditions.size(); }
    [[nodiscard]] Constraint with(ConstraintCondition condition) const;
};

// Handle of a registered constraint. 0 is never a valid id.
using ConstraintId = uint32_t;
inline constexpr ConstraintId kNoConstraint = 0;

struct KIMIX_GRAPH_DB_API ConstraintHandle {
    ConstraintId id{kNoConstraint};

    [[nodiscard]] bool valid() const noexcept { return id != kNoConstraint; }
    [[nodiscard]] bool operator==(ConstraintHandle const &other) const noexcept {
        return id == other.id;
    }
    [[nodiscard]] bool operator!=(ConstraintHandle const &other) const noexcept {
        return id != other.id;
    }
};

// ---------------------------------------------------------------------------
// Registry persistence
// ---------------------------------------------------------------------------

// One label table entry ("s/labels"). Ids are written verbatim: the dense space
// must survive a save/load cycle, otherwise a stored entity's label id would
// address a different name after a reload.
struct KIMIX_GRAPH_DB_API LabelTableEntry {
    LabelId id{kNoLabel};
    kimix::string name;
};

// Codec of the "s/labels" record. Decoding is total (truncated/garbage buffers
// are a `false`, never a crash) and preserves entry order.
[[nodiscard]] KIMIX_GRAPH_DB_API size_t
encoded_label_table_size(kimix::vector<LabelTableEntry> const &entries) noexcept;
KIMIX_GRAPH_DB_API void encode_label_table(kimix::vector<LabelTableEntry> const &entries,
                                           kimix::vector<std::byte> &out);
[[nodiscard]] KIMIX_GRAPH_DB_API bool
decode_label_table(kimix::span<const std::byte> bytes,
                   kimix::vector<LabelTableEntry> &out) noexcept;

// Codec of the "s/proptypes" record (same shape, PropertyTypeInfo entries).
[[nodiscard]] KIMIX_GRAPH_DB_API size_t
encoded_property_type_table_size(PropertyTypeList const &entries) noexcept;
KIMIX_GRAPH_DB_API void encode_property_type_table(PropertyTypeList const &entries,
                                                   kimix::vector<std::byte> &out);
[[nodiscard]] KIMIX_GRAPH_DB_API bool
decode_property_type_table(kimix::span<const std::byte> bytes, PropertyTypeList &out) noexcept;

// ---------------------------------------------------------------------------
// Schema
// ---------------------------------------------------------------------------

class KIMIX_GRAPH_DB_API Schema : public kimix::IOperatorNewBase {
public:
    Schema() noexcept = default;
    Schema(Schema const &) = delete;
    Schema &operator=(Schema const &) = delete;
    Schema(Schema &&) = delete;
    Schema &operator=(Schema &&) = delete;
    ~Schema() noexcept = default;

    // --- labels -------------------------------------------------------------

    // Register `name`. error_empty_name for a blank name, error_name_exists when
    // the name is taken. On success `out` holds the new id.
    [[nodiscard]] GdiStatus create_label(kimix::string_view name, LabelId &out);
    // Look up a name without creating it. False when unknown.
    [[nodiscard]] bool find_label(kimix::string_view name, LabelId &out) const;
    // Resolve a name, creating it when `auto_create` is set. This is the
    // GraphOptions::auto_create_schema path used by every write that takes a
    // label name.
    [[nodiscard]] GdiStatus resolve_label(kimix::string_view name, bool auto_create,
                                          LabelId &out);
    // Every registered label, ordered by id (= registration order: stable and
    // deterministic, and the order the C# backend's GetAllLabels produces).
    [[nodiscard]] kimix::vector<LabelId> all_labels() const;
    // Rename, keeping the id (and therefore every entity) untouched.
    [[nodiscard]] GdiStatus rename_label(LabelId id, kimix::string_view new_name);
    // Unregister. The caller MUST strip the id from every entity (Database does).
    // Refuses (error_label) an unknown id and (error_state) a label that is still
    // the key label of a live vertex — the Database checks that before calling.
    [[nodiscard]] GdiStatus free_label(LabelId id);
    // Name of a label, copied. Empty when the id is unknown/dead.
    [[nodiscard]] kimix::string label_name(LabelId id) const;
    [[nodiscard]] bool has_label(LabelId id) const;
    [[nodiscard]] size_t label_count() const;

    // --- property types -----------------------------------------------------

    [[nodiscard]] GdiStatus create_property_type(PropertyTypeInfo const &info,
                                                 PropNameId &out);
    [[nodiscard]] bool find_property_type(kimix::string_view name, PropNameId &out) const;
    [[nodiscard]] GdiStatus resolve_property_type(kimix::string_view name, bool auto_create,
                                                  PropertyTypeInfo const &declaration,
                                                  PropNameId &out);
    // Declaration of an id. name_id/name are always filled; the other fields are
    // the registered declaration (single/int64/fixed/1 for an unknown id).
    [[nodiscard]] PropertyTypeInfo property_type(PropNameId id) const;
    [[nodiscard]] bool has_property_type(PropNameId id) const;
    // Every declaration, SORTED BY NAME (deterministic; the C# side sorts too).
    [[nodiscard]] PropertyTypeList all_property_types() const;
    // Update a declaration. Renaming is free; changing container/dtype is
    // refused with error_read_only_property_type when `values_exist` says a live
    // entity still carries a value of this type (the Database decides that). A
    // count/size-type change is refused the same way UNLESS the new declaration
    // is a relaxation (it accepts a superset of the old lengths: fixed N ->
    // max >= N -> unlimited), which leaves every stored value readable.
    [[nodiscard]] GdiStatus update_property_type(PropNameId id, PropertyTypeInfo const &info,
                                                 bool values_exist);
    [[nodiscard]] GdiStatus free_property_type(PropNameId id);
    [[nodiscard]] size_t property_type_count() const;

    // --- constraints --------------------------------------------------------

    [[nodiscard]] GdiStatus create_constraint(Constraint const &definition,
                                              ConstraintHandle &out);
    [[nodiscard]] GdiStatus add_label_condition(ConstraintHandle handle, LabelId label,
                                                GdiOp op);
    [[nodiscard]] GdiStatus add_property_condition(ConstraintHandle handle, PropNameId property,
                                                   GdiOp op, PropertyValue value,
                                                   uint32_t count = 1);
    [[nodiscard]] GdiStatus free_constraint(ConstraintHandle handle);
    // Definition of a handle, COPIED into `out` (a pointer into the registry
    // could not outlive the lock the schema holds while answering). False when
    // the handle is unknown/foreign.
    [[nodiscard]] bool constraint(ConstraintHandle handle, Constraint &out) const;
    [[nodiscard]] size_t constraint_count() const;

    // --- persistence / versioning -------------------------------------------

    // Monotone counter bumped by every registry mutation (create/rename/free/
    // update). The meta record stores it so a load can tell whether the stored
    // tables are stale relative to the meta record.
    [[nodiscard]] uint32_t version() const noexcept;
    // Explicit bump (a constraint edit does not change what a store must write,
    // but a caller that mutates through a non-schema path can force one).
    void bump_version() noexcept;

    // Snapshot / restore for the save and load paths. restore() REPLACES the
    // whole registry state and the two id counters.
    //
    // THREAD SAFETY OF snapshot(): it is O(1) in the lock. Both registries are
    // published as an IMMUTABLE view (an internally cached copy, rebuilt lazily the
    // first time it is asked for after a mutation), so the callers' copy — which
    // allocates one string per entry — runs with NO lock held at all. The old form
    // copied both registries inside the SHARED spin lock: on the SAVE path (which
    // calls this twice per save and is allowed to run beside level-2 writers) that
    // made every schema reader/writer spin for the length of a full copy, and a
    // pending writer blocked all new readers meanwhile. See Schema::snapshot() in
    // schema.cpp for the exact protocol.
    void snapshot(kimix::vector<LabelTableEntry> &labels, PropertyTypeList &types,
                  uint32_t &next_label_id, uint32_t &next_prop_name_id) const;
    // Returns false (and leaves the registry untouched) when a table carries an
    // impossible id (0, or a duplicate), which is what a corrupt "s/" record
    // looks like.
    // Re-register one label / property type with its EXACT id — the transaction
    // rollback path (a free must be undoable to the same id, or every entity that
    // referenced it would dangle). error_name_exists when the id or the name is
    // taken again, error_argument for a zero id.
    [[nodiscard]] GdiStatus restore_label(LabelTableEntry const &entry);
    [[nodiscard]] GdiStatus restore_property_type(PropertyTypeInfo const &info);

    [[nodiscard]] bool restore(kimix::vector<LabelTableEntry> const &labels,
                               PropertyTypeList const &types,
                               uint32_t next_label_id, uint32_t next_prop_name_id,
                               uint32_t schema_version);

private:
    struct PropertyTypeSlot {
        PropNameId id{kNoPropName};
        PropertyTypeInfo info;
    };

    // The published, IMMUTABLE view of both registries that snapshot() serves its
    // callers from. Once _published points at one of these it never changes again —
    // a mutation only ever publishes a NEW view — so a caller may hold it and copy
    // out of it with no lock held.
    struct RegistryView {
        kimix::vector<LabelTableEntry> labels;   // dense, id order
        PropertyTypeList types;                  // dense, id order
        uint32_t next_label_id{1};
        uint32_t next_prop_name_id{1};
        // The generation this view was built at (see _generation): the ONE test for
        // "is this view still current".
        uint32_t generation{0};
    };
    using RegistryViewPtr = kimix::shared_ptr<RegistryView const>;

    // The current view, or an empty pointer when it is stale/absent. ONE atomic
    // reference-count bump under the SHARED lock, no allocation.
    [[nodiscard]] RegistryViewPtr _current_view() const;
    // Build + publish a view for the current generation. Takes the UNIQUE lock, i.e.
    // this is the one long section left, and it runs at most ONCE per registry
    // generation (a save's two snapshot() calls share it). Returns the published
    // view; concurrent callers that lose the race get the winner's view.
    [[nodiscard]] RegistryViewPtr _publish_view() const;

    // The probe is built by the CALLER, before the lock: a lookup must not allocate
    // inside its critical section (an external name longer than the SSO buffer would
    // otherwise heap-allocate there). Passing the owning string by const& also lets
    // a mutation path (create/rename/update) reuse the very string it already holds.
    [[nodiscard]] LabelId _find_label_locked(kimix::string const &probe) const;
    [[nodiscard]] PropNameId _find_property_type_locked(kimix::string const &probe) const;

    mutable kimix::shared_atomic_mutex _mutex;
    kimix::vector<LabelTableEntry> _labels;                    // dense, id order
    kimix::unordered_map<kimix::string, LabelId, kimix::string_hash> _label_by_name;
    kimix::vector<PropertyTypeSlot> _property_types;           // dense, id order
    kimix::unordered_map<kimix::string, PropNameId, kimix::string_hash> _property_type_by_name;
    kimix::unordered_map<ConstraintId, Constraint> _constraints;
    LabelId _next_label_id{1};
    PropNameId _next_prop_name_id{1};
    ConstraintId _next_constraint_id{1};
    // Staleness counter, bumped by every registry mutation. Atomic so version() /
    // bump_version() are lock-free (they were 2 of the 28 lock sites here): the value
    // is only ever read and compared against a stored one, so a relaxed read of a
    // bump that races a mutation is exactly as useful as it was under the lock.
    mutable std::atomic<uint32_t> _version{0};
    // Internally monotone mutation counter for the published view (see
    // _current_view()): bumped by bump_version() AND by restore(), which sets
    // _version to the STORE's value and would otherwise be able to alias a view built
    // at the same number.
    mutable std::atomic<uint32_t> _generation{1};
    // Guarded by _mutex (written under the UNIQUE lock, read under either).
    mutable RegistryViewPtr _published;
};

} // namespace kimix::graph_db
