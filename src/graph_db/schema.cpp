/*
 * schema.cpp — label / property-type / constraint registries (see schema.h).
 *
 * Every method takes the one registry mutex and releases it before returning:
 * the schema never calls into the graph, so it can never be part of a wait-for
 * cycle (lock hierarchy level 1, see README.md).
 *
 * The two table codecs ("s/labels", "s/proptypes") are here rather than in
 * utils.cpp because they speak PropertyTypeInfo, which is property.h's type;
 * utils.h stays free of the schema vocabulary (documented deviation from the
 * plan's file map, same behaviour).
 */
#include "binary_le.h"
#include "schema.h"
#include "utils.h" // kSchemaTableMagic / kRecordMagicSize (the key space stays in utils.h)

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>

namespace kimix::graph_db {

namespace src_graph_db_schema {

// Header of a schema table record: magic + table kind + entry count.
constexpr size_t kSchemaTableHeaderSize = 4 + 4 + 4;
// One property-type entry: id + container + dtype + reserved + count + name len.
constexpr size_t kPropertyTypeEntryHeaderSize = 4 + 1 + 1 + 2 + 4 + 4;
// One label entry: id + name len.
constexpr size_t kLabelEntryHeaderSize = 4 + 4;

enum class TableKind : uint32_t {
    labels = 0,
    property_types = 1,
};

inline bool is_blank(kimix::string_view text) noexcept {
    for (char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            return false;
        }
    }
    return true;
}

// Text equality that does not depend on an implicit string <-> string_view
// conversion (basic_string has no implicit constructor from string_view, so
// `name == view` would not compile portably).
inline bool same_text(kimix::string const &lhs, kimix::string_view rhs) noexcept {
    return lhs.size() == rhs.size() &&
           (lhs.empty() || std::memcmp(lhs.data(), rhs.data(), lhs.size()) == 0);
}

} // namespace src_graph_db_schema

// ---------------------------------------------------------------------------
// Property-type declaration rules (ABI 8, plan §1.2) — the ONE place a
// declaration is judged, shared by create_property_type and
// update_property_type. The value-side rule lives in
// PropertyValue::matches_declaration; this is the declaration-side one, and
// Database::update_property_type calls it too (there is no second copy of the
// count rule anywhere). Declared in schema.h.
// ---------------------------------------------------------------------------

bool is_valid_size_type(GdiSizeType type) noexcept {
    return type == GdiSizeType::fixed || type == GdiSizeType::max ||
           type == GdiSizeType::unlimited;
}

GdiStatus validate_property_type_declaration(PropertyTypeInfo const &info) noexcept {
    if (!is_valid_size_type(info.size_type)) {
        return GdiStatus::error_argument;
    }
    if (info.container == GdiContainerType::single) {
        // ABI 9: `(single, data)` is refused. A self-describing array carries its
        // element kind in the value's own flag byte; a single value has no array to
        // describe, so the declaration would be unsatisfiable — refusing it here keeps
        // such a declaration from ever existing.
        if (info.data == GdiDataType::data) {
            return GdiStatus::error_argument;
        }
        // A single value has no length to bound: only `fixed` is meaningful, and
        // the schema refuses to store the other two.
        if (info.size_type != GdiSizeType::fixed) {
            return GdiStatus::error_argument;
        }
        return info.count < 1 ? GdiStatus::error_count : GdiStatus::ok;
    }
    // An ARRAY declaration of ANY datatype is legal, the `data` sentinel included:
    // `(array, data)` is exactly what a data_array value needs, and `(array, int64)`
    // written with a data_array value is refused on the VALUE side (error_wrong_type
    // in Database::_validate_property, through PropertyValue::data_type()).
    if (info.size_type == GdiSizeType::unlimited) {
        return GdiStatus::ok; // no bound, so the count field carries nothing
    }
    return info.count < 1 ? GdiStatus::error_count : GdiStatus::ok;
}

namespace src_graph_db_schema {

// The canonical form of a declaration: `unlimited` carries NO bound, so its
// count is stored as 0 (a caller may pass anything; the schema normalizes it).
// Every other combination keeps its count verbatim. The stored/returned
// PropertyTypeInfo is therefore canonical, which is also what makes a
// round-tripped declaration compare equal to itself.
inline void canonicalize_property_type(PropertyTypeInfo &info) noexcept {
    if (info.size_type == GdiSizeType::unlimited) {
        info.count = 0;
    }
}

// True when `next` accepts a SUPERSET of the lengths `previous` accepted while
// keeping the same container and datatype — the only shape change a declaration
// with live values may still make, because every stored value stays valid
// without being re-examined (plan §1.2: fixed N -> max >= N -> unlimited).
inline bool is_declaration_relaxation(PropertyTypeInfo const &previous,
                                      PropertyTypeInfo const &next) noexcept {
    if (previous.container != next.container || previous.data != next.data) {
        return false;
    }
    if (next.size_type == GdiSizeType::unlimited) {
        return true; // accepts every length, so it accepts whatever was stored
    }
    if (previous.size_type == GdiSizeType::unlimited) {
        return false; // a bound cannot be put back on an unbounded declaration
    }
    if (next.size_type == GdiSizeType::max) {
        // max M accepts 0..M, and so does fixed N (the single length N), so an
        // upper bound that does not shrink is a superset in both directions.
        return next.count >= previous.count;
    }
    // A FIXED declaration accepts exactly one length: it is a superset of another
    // declaration only when that one accepted exactly the same length (in which
    // case the shape did not change at all).
    return previous.size_type == GdiSizeType::fixed && next.count == previous.count;
}

} // namespace src_graph_db_schema
using namespace src_graph_db_schema;

// ---------------------------------------------------------------------------
// ConstraintCondition / Constraint
// ---------------------------------------------------------------------------

ConstraintCondition ConstraintCondition::on_label(LabelId label, GdiOp op) noexcept {
    ConstraintCondition condition;
    condition.kind = Kind::label;
    condition.label = label;
    condition.op = op;
    return condition;
}

ConstraintCondition ConstraintCondition::on_property(PropNameId property, GdiOp op,
                                                     PropertyValue value,
                                                     uint32_t count) noexcept {
    ConstraintCondition condition;
    condition.kind = Kind::property;
    condition.property = property;
    condition.op = op;
    condition.value = std::move(value);
    // An any_in count is the SET's element count and 0 is meaningful (the empty set,
    // §1.2); every other op keeps the pre-ABI-7 normalisation, where a 0 can only
    // mean "the caller did not say" and the scalar reading (1) is the safe one.
    condition.count = count == 0 ? (op == GdiOp::any_in ? 0u : 1u) : count;
    // The index is built HERE, so every construction path gets it: Schema's
    // add_property_condition, Constraint::with, a test or a bench that spells a
    // condition by hand. One build per condition, never per edge.
    if (op == GdiOp::any_in) {
        condition.any_in_set = AnyInIndex::build(condition.value);
    }
    return condition;
}

// ---------------------------------------------------------------------------
// The ANY_IN probe index
// ---------------------------------------------------------------------------

AnyInIndex AnyInIndex::build(PropertyValue const &set_value) {
    AnyInIndex out;
    switch (set_value.kind()) {
    case PropertyKind::string_array: {
        out.shape = Shape::strings;
        auto const set = set_value.as_string_array();
        out.strings.reserve(set.element_count());
        for (auto const &element : set) {
            out.strings.emplace_back(element.data(), element.size());
        }
        // Ordinal byte order — the order evaluate() already uses for text, so the
        // index cannot disagree with the semantics it accelerates.
        std::sort(out.strings.begin(), out.strings.end());
        // Duplicates in a set are the caller's noise, not a different answer: one
        // copy each keeps a snapshot (and therefore every Constraint copy) small.
        out.strings.erase(std::unique(out.strings.begin(), out.strings.end()),
                          out.strings.end());
        break;
    }
    case PropertyKind::int64_array: {
        out.shape = Shape::integers;
        auto const set = set_value.as_int64_array();
        out.integers.assign(set.begin(), set.end());
        std::sort(out.integers.begin(), out.integers.end());
        out.integers.erase(std::unique(out.integers.begin(), out.integers.end()),
                           out.integers.end());
        break;
    }
    default:
        // Not a set: the condition stays well-formed and simply never matches
        // (Schema::add_property_condition refuses this combination up front; a
        // hand-built one falls back to the linear semantics in matches_value).
        out.shape = Shape::none;
        break;
    }
    return out;
}

bool AnyInIndex::contains(kimix::string_view text) const noexcept {
    if (shape != Shape::strings) {
        return false;
    }
    auto const it = std::lower_bound(
        strings.begin(), strings.end(), text,
        [](kimix::string const &element, kimix::string_view needle) {
            return kimix::string_view{element.data(), element.size()} < needle;
        });
    return it != strings.end() && kimix::string_view{it->data(), it->size()} == text;
}

bool AnyInIndex::contains(int64_t value) const noexcept {
    if (shape != Shape::integers) {
        return false;
    }
    auto const it = std::lower_bound(integers.begin(), integers.end(), value);
    return it != integers.end() && *it == value;
}

bool ConstraintCondition::matches_value(PropertyValue const &actual) const noexcept {
    if (op != GdiOp::any_in) {
        return evaluate(op, actual, value);
    }
    // The indexed answer (plan §1.2), identical to any_in_matches but O(log n) per
    // probe. A shape that does not match the actual's kind is false, exactly like the
    // linear definition.
    switch (any_in_set.shape) {
    case AnyInIndex::Shape::strings:
        switch (actual.kind()) {
        case PropertyKind::string:
            return any_in_set.contains(actual.as_string());
        case PropertyKind::string_array:
            for (auto const &element : actual.as_string_array()) {
                if (any_in_set.contains(element)) {
                    return true;
                }
            }
            return false;
        default:
            return false;
        }
    case AnyInIndex::Shape::integers:
        switch (actual.kind()) {
        case PropertyKind::int64_value:
            return any_in_set.contains(actual.as_int64());
        case PropertyKind::int64_array:
            for (auto const element : actual.as_int64_array()) {
                if (any_in_set.contains(element)) {
                    return true;
                }
            }
            return false;
        default:
            return false;
        }
    case AnyInIndex::Shape::none:
        break;
    }
    // An index-less any_in condition: a value built as a set AFTER this condition was
    // constructed (nothing in the library does that today — grep `condition.value` —
    // but a direct aggregate-initialised ConstraintCondition can carry one). Stay
    // correct rather than silently false: answer from the semantics definition.
    return any_in_matches(value, actual);
}

Constraint Constraint::with(ConstraintCondition condition) const {
    Constraint out = *this;
    out.conditions.push_back(std::move(condition));
    return out;
}

// ---------------------------------------------------------------------------
// Table codecs
// ---------------------------------------------------------------------------

size_t encoded_label_table_size(kimix::vector<LabelTableEntry> const &entries) noexcept {
    size_t size = kSchemaTableHeaderSize;
    for (auto const &entry : entries) {
        size += kLabelEntryHeaderSize + entry.name.size();
    }
    return size;
}

void encode_label_table(kimix::vector<LabelTableEntry> const &entries,
                        kimix::vector<std::byte> &out) {
    using namespace detail;
    out.clear();
    out.resize(encoded_label_table_size(entries));
    std::byte *cursor = out.data();
    std::memcpy(cursor, kSchemaTableMagic.data(), kRecordMagicSize);
    cursor += kRecordMagicSize;
    write_u32(cursor, static_cast<uint32_t>(TableKind::labels));
    cursor += 4;
    write_u32(cursor, static_cast<uint32_t>(entries.size()));
    cursor += 4;
    for (auto const &entry : entries) {
        write_u32(cursor, entry.id);
        cursor += 4;
        cursor = write_blob(cursor, kimix::span<const std::byte>{
                                       reinterpret_cast<std::byte const *>(entry.name.data()),
                                       entry.name.size()});
    }
}

bool decode_label_table(kimix::span<const std::byte> bytes,
                        kimix::vector<LabelTableEntry> &out) noexcept {
    using namespace detail;
    out.clear();
    std::byte const *cursor = bytes.data();
    std::byte const *end = bytes.data() + bytes.size();
    if (bytes.size() < kSchemaTableHeaderSize) {
        return false;
    }
    if (std::memcmp(cursor, kSchemaTableMagic.data(), kRecordMagicSize) != 0) {
        return false;
    }
    cursor += kRecordMagicSize;
    if (read_u32(cursor) != static_cast<uint32_t>(TableKind::labels)) {
        return false;
    }
    cursor += 4;
    uint32_t const count = read_u32(cursor);
    cursor += 4;
    // Each entry costs at least kLabelEntryHeaderSize bytes: reject an
    // impossible count before it drives an allocation.
    if (static_cast<uint64_t>(count) * kLabelEntryHeaderSize >
        static_cast<uint64_t>(end - cursor)) {
        return false;
    }
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        LabelTableEntry entry;
        if (end - cursor < 4) {
            out.clear();
            return false;
        }
        entry.id = read_u32(cursor);
        cursor += 4;
        kimix::vector<std::byte> name;
        if (!read_blob(cursor, end, name)) {
            out.clear();
            return false;
        }
        entry.name.assign(reinterpret_cast<char const *>(name.data()), name.size());
        out.push_back(std::move(entry));
    }
    return true;
}

size_t encoded_property_type_table_size(PropertyTypeList const &entries) noexcept {
    size_t size = kSchemaTableHeaderSize;
    for (auto const &entry : entries) {
        size += kPropertyTypeEntryHeaderSize + entry.name.size();
    }
    return size;
}

void encode_property_type_table(PropertyTypeList const &entries,
                                kimix::vector<std::byte> &out) {
    using namespace detail;
    out.clear();
    out.resize(encoded_property_type_table_size(entries));
    std::byte *cursor = out.data();
    std::memcpy(cursor, kSchemaTableMagic.data(), kRecordMagicSize);
    cursor += kRecordMagicSize;
    write_u32(cursor, static_cast<uint32_t>(TableKind::property_types));
    cursor += 4;
    write_u32(cursor, static_cast<uint32_t>(entries.size()));
    cursor += 4;
    for (auto const &entry : entries) {
        write_u32(cursor, entry.name_id);
        cursor += 4;
        write_u8(cursor, static_cast<uint8_t>(entry.container));
        cursor += 1;
        write_u8(cursor, static_cast<uint8_t>(entry.data));
        cursor += 1;
        // ABI 8: the first of the two reserved bytes carries the size type; the
        // second stays zero. An ABI-7 image has 0 there, i.e. `fixed` — the only
        // declaration ABI 7 could express — so old stores load unchanged.
        write_u8(cursor, static_cast<uint8_t>(entry.size_type));
        cursor += 1;
        write_u8(cursor, 0); // reserved
        cursor += 1;
        write_u32(cursor, entry.count);
        cursor += 4;
        cursor = write_blob(cursor, kimix::span<const std::byte>{
                                       reinterpret_cast<std::byte const *>(entry.name.data()),
                                       entry.name.size()});
    }
}

bool decode_property_type_table(kimix::span<const std::byte> bytes,
                                PropertyTypeList &out) noexcept {
    using namespace detail;
    out.clear();
    std::byte const *cursor = bytes.data();
    std::byte const *end = bytes.data() + bytes.size();
    if (bytes.size() < kSchemaTableHeaderSize) {
        return false;
    }
    if (std::memcmp(cursor, kSchemaTableMagic.data(), kRecordMagicSize) != 0) {
        return false;
    }
    cursor += kRecordMagicSize;
    if (read_u32(cursor) != static_cast<uint32_t>(TableKind::property_types)) {
        return false;
    }
    cursor += 4;
    uint32_t const count = read_u32(cursor);
    cursor += 4;
    if (static_cast<uint64_t>(count) * kPropertyTypeEntryHeaderSize >
        static_cast<uint64_t>(end - cursor)) {
        return false;
    }
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (end - cursor < kPropertyTypeEntryHeaderSize) {
            out.clear();
            return false;
        }
        PropertyTypeInfo info;
        info.name_id = read_u32(cursor);
        cursor += 4;
        info.container = static_cast<GdiContainerType>(read_u8(cursor));
        cursor += 1;
        info.data = static_cast<GdiDataType>(read_u8(cursor));
        cursor += 1;
        // ABI 8: size type in the first reserved byte (zero — `fixed` — in an
        // ABI-7 image), one reserved byte after it. An out-of-range value fails
        // the whole table, so the load path reports a corrupt schema rather than
        // adopting a size type no writer could have produced.
        uint8_t const size_type = read_u8(cursor);
        cursor += 1;
        if (size_type > static_cast<uint8_t>(GdiSizeType::unlimited)) {
            out.clear();
            return false;
        }
        info.size_type = static_cast<GdiSizeType>(size_type);
        cursor += 1; // reserved
        info.count = read_u32(cursor);
        cursor += 4;
        kimix::vector<std::byte> name;
        if (!read_blob(cursor, end, name)) {
            out.clear();
            return false;
        }
        info.name.assign(reinterpret_cast<char const *>(name.data()), name.size());
        out.push_back(std::move(info));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Labels
// ---------------------------------------------------------------------------

LabelId Schema::_find_label_locked(kimix::string_view name) const {
    auto it = _label_by_name.find(kimix::string{name});
    return it == _label_by_name.end() ? kNoLabel : it->second;
}

GdiStatus Schema::create_label(kimix::string_view name, LabelId &out) {
    if (is_blank(name)) {
        return GdiStatus::error_empty_name;
    }
    std::unique_lock<std::shared_mutex> lock(_mutex);
    if (_find_label_locked(name) != kNoLabel) {
        return GdiStatus::error_name_exists;
    }
    LabelTableEntry entry;
    entry.id = _next_label_id++;
    entry.name.assign(name.data(), name.size());
    LabelId const id = entry.id;
    _labels.push_back(std::move(entry));
    _label_by_name.emplace(kimix::string{name}, id);
    ++_version;
    out = id;
    return GdiStatus::ok;
}

bool Schema::find_label(kimix::string_view name, LabelId &out) const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    LabelId const id = _find_label_locked(name);
    if (id == kNoLabel) {
        return false;
    }
    out = id;
    return true;
}

GdiStatus Schema::resolve_label(kimix::string_view name, bool auto_create, LabelId &out) {
    if (is_blank(name)) {
        return GdiStatus::error_empty_name;
    }
    {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        LabelId const id = _find_label_locked(name);
        if (id != kNoLabel) {
            out = id;
            return GdiStatus::ok;
        }
    }
    if (!auto_create) {
        return GdiStatus::error_label;
    }
    LabelId created = kNoLabel;
    GdiStatus const status = create_label(name, created);
    if (is_error(status)) {
        // A concurrent create won the race: that is success for a resolve.
        if (status == GdiStatus::error_name_exists) {
            std::shared_lock<std::shared_mutex> lock(_mutex);
            LabelId const id = _find_label_locked(name);
            if (id != kNoLabel) {
                out = id;
                return GdiStatus::ok;
            }
            return GdiStatus::error_state;
        }
        return status;
    }
    out = created;
    return GdiStatus::ok;
}

kimix::vector<LabelId> Schema::all_labels() const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    kimix::vector<LabelId> out;
    out.reserve(_labels.size());
    for (auto const &entry : _labels) {
        out.push_back(entry.id);
    }
    return out;
}

GdiStatus Schema::rename_label(LabelId id, kimix::string_view new_name) {
    if (is_blank(new_name)) {
        return GdiStatus::error_empty_name;
    }
    std::unique_lock<std::shared_mutex> lock(_mutex);
    auto slot = std::find_if(_labels.begin(), _labels.end(),
                             [id](LabelTableEntry const &entry) { return entry.id == id; });
    if (slot == _labels.end()) {
        return GdiStatus::error_label;
    }
    LabelId const clash = _find_label_locked(new_name);
    if (clash != kNoLabel && clash != id) {
        return GdiStatus::error_name_exists;
    }
    if (same_text(slot->name, new_name)) {
        return GdiStatus::ok;
    }
    _label_by_name.erase(slot->name);
    slot->name.assign(new_name.data(), new_name.size());
    _label_by_name.emplace(slot->name, id);
    ++_version;
    return GdiStatus::ok;
}

GdiStatus Schema::free_label(LabelId id) {
    std::unique_lock<std::shared_mutex> lock(_mutex);
    auto slot = std::find_if(_labels.begin(), _labels.end(),
                             [id](LabelTableEntry const &entry) { return entry.id == id; });
    if (slot == _labels.end()) {
        return GdiStatus::error_label;
    }
    _label_by_name.erase(slot->name);
    _labels.erase(slot);
    ++_version;
    return GdiStatus::ok;
}

kimix::string Schema::label_name(LabelId id) const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    for (auto const &entry : _labels) {
        if (entry.id == id) {
            return entry.name;
        }
    }
    return {};
}

bool Schema::has_label(LabelId id) const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    return std::any_of(_labels.begin(), _labels.end(),
                       [id](LabelTableEntry const &entry) { return entry.id == id; });
}

size_t Schema::label_count() const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    return _labels.size();
}

// ---------------------------------------------------------------------------
// Property types
// ---------------------------------------------------------------------------

PropNameId Schema::_find_property_type_locked(kimix::string_view name) const {
    auto it = _property_type_by_name.find(kimix::string{name});
    return it == _property_type_by_name.end() ? kNoPropName : it->second;
}

GdiStatus Schema::create_property_type(PropertyTypeInfo const &info, PropNameId &out) {
    if (is_blank(info.name)) {
        return GdiStatus::error_empty_name;
    }
    GdiStatus status = validate_property_type_declaration(info);
    if (is_error(status)) {
        return status;
    }
    std::unique_lock<std::shared_mutex> lock(_mutex);
    if (_find_property_type_locked(info.name) != kNoPropName) {
        return GdiStatus::error_name_exists;
    }
    PropertyTypeSlot slot;
    slot.id = _next_prop_name_id++;
    slot.info = info;
    canonicalize_property_type(slot.info);
    slot.info.name_id = slot.id;
    PropNameId const id = slot.id;
    _property_type_by_name.emplace(slot.info.name, id);
    _property_types.push_back(std::move(slot));
    ++_version;
    out = id;
    return GdiStatus::ok;
}

bool Schema::find_property_type(kimix::string_view name, PropNameId &out) const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    PropNameId const id = _find_property_type_locked(name);
    if (id == kNoPropName) {
        return false;
    }
    out = id;
    return true;
}

GdiStatus Schema::resolve_property_type(kimix::string_view name, bool auto_create,
                                        PropertyTypeInfo const &declaration,
                                        PropNameId &out) {
    if (is_blank(name)) {
        return GdiStatus::error_empty_name;
    }
    {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        PropNameId const id = _find_property_type_locked(name);
        if (id != kNoPropName) {
            out = id;
            return GdiStatus::ok;
        }
    }
    if (!auto_create) {
        return GdiStatus::error_property_type;
    }
    // Implicit declaration: the name comes from the call, the shape from the
    // value being written (container/dtype/count), so an auto-created type is
    // immediately usable for the value that created it. The size type is
    // PropertyTypeInfo's default `fixed` with the value's exact count — an
    // auto-created declaration is exact-length by construction (nothing else
    // could be inferred from a single value).
    //
    // The CALLER derives `declaration` from the value, and a data_array (ABI 9)
    // derives exactly the shape a string_array does: container = array,
    // data = GdiDataType::data (the self-describing sentinel), count =
    // value.element_count(), size_type = fixed. The element kind of that first value
    // is deliberately NOT part of the declaration — that freedom is the point of the
    // kind — so a second write with a different element kind is accepted only if the
    // size type allows its count (a `fixed` auto-created declaration pins the count,
    // so the caller relaxes it to max/unlimited to keep writing).
    PropertyTypeInfo created = declaration;
    created.name.assign(name.data(), name.size());
    created.size_type = GdiSizeType::fixed;
    PropNameId created_id = kNoPropName;
    GdiStatus const status = create_property_type(created, created_id);
    if (is_error(status)) {
        if (status == GdiStatus::error_name_exists) {
            std::shared_lock<std::shared_mutex> lock(_mutex);
            PropNameId const id = _find_property_type_locked(name);
            if (id != kNoPropName) {
                out = id;
                return GdiStatus::ok;
            }
            return GdiStatus::error_state;
        }
        return status;
    }
    out = created_id;
    return GdiStatus::ok;
}

PropertyTypeInfo Schema::property_type(PropNameId id) const {
    PropertyTypeInfo out;
    out.name_id = id;
    std::shared_lock<std::shared_mutex> lock(_mutex);
    for (auto const &slot : _property_types) {
        if (slot.id == id) {
            out = slot.info;
            return out;
        }
    }
    return out;
}

bool Schema::has_property_type(PropNameId id) const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    return std::any_of(_property_types.begin(), _property_types.end(),
                       [id](PropertyTypeSlot const &slot) { return slot.id == id; });
}

PropertyTypeList Schema::all_property_types() const {
    PropertyTypeList out;
    {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        out.reserve(_property_types.size());
        for (auto const &slot : _property_types) {
            out.push_back(slot.info);
        }
    }
    // Sorted by name: the C# reference sorts too (GetAllPropertyTypes), so the
    // order is part of the contract rather than an accident of insertion.
    std::sort(out.begin(), out.end(),
              [](PropertyTypeInfo const &lhs, PropertyTypeInfo const &rhs) {
                  return lhs.name < rhs.name;
              });
    return out;
}

GdiStatus Schema::update_property_type(PropNameId id, PropertyTypeInfo const &info,
                                       bool values_exist) {
    if (is_blank(info.name)) {
        return GdiStatus::error_empty_name;
    }
    GdiStatus status = validate_property_type_declaration(info);
    if (is_error(status)) {
        return status;
    }
    // The canonical form the slot will hold: `unlimited` drops the count.
    PropertyTypeInfo const next = [&info] {
        PropertyTypeInfo copy = info;
        canonicalize_property_type(copy);
        return copy;
    }();
    std::unique_lock<std::shared_mutex> lock(_mutex);
    auto slot = std::find_if(_property_types.begin(), _property_types.end(),
                             [id](PropertyTypeSlot const &entry) { return entry.id == id; });
    if (slot == _property_types.end()) {
        return GdiStatus::error_property_type;
    }
    PropNameId const clash = _find_property_type_locked(info.name);
    if (clash != kNoPropName && clash != id) {
        return GdiStatus::error_name_exists;
    }
    // A live value pins the shape: reinterpreting an int64 as a blob would make
    // every stored value unreadable, so the declaration is read-only while one
    // exists. The ONE exception is a RELAXATION (ABI 8, plan §1.2): a new
    // declaration that accepts a superset of the old one's lengths leaves every
    // stored value readable, so fixed N -> max >= N -> unlimited is allowed.
    bool const shape_changed = slot->info.container != next.container ||
                               slot->info.data != next.data ||
                               slot->info.size_type != next.size_type ||
                               slot->info.count != next.count;
    if (shape_changed && values_exist && !is_declaration_relaxation(slot->info, next)) {
        return GdiStatus::error_read_only_property_type;
    }
    if (!same_text(slot->info.name, kimix::string_view{info.name.data(), info.name.size()})) {
        _property_type_by_name.erase(slot->info.name);
        slot->info.name = info.name;
        _property_type_by_name.emplace(slot->info.name, id);
    }
    slot->info.container = next.container;
    slot->info.data = next.data;
    slot->info.count = next.count;
    slot->info.size_type = next.size_type;
    slot->info.name_id = id;
    ++_version;
    return GdiStatus::ok;
}

GdiStatus Schema::free_property_type(PropNameId id) {
    std::unique_lock<std::shared_mutex> lock(_mutex);
    auto slot = std::find_if(_property_types.begin(), _property_types.end(),
                             [id](PropertyTypeSlot const &entry) { return entry.id == id; });
    if (slot == _property_types.end()) {
        return GdiStatus::error_property_type;
    }
    _property_type_by_name.erase(slot->info.name);
    _property_types.erase(slot);
    ++_version;
    return GdiStatus::ok;
}

size_t Schema::property_type_count() const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    return _property_types.size();
}

// ---------------------------------------------------------------------------
// Constraints
// ---------------------------------------------------------------------------

GdiStatus Schema::create_constraint(Constraint const &definition, ConstraintHandle &out) {
    std::unique_lock<std::shared_mutex> lock(_mutex);
    ConstraintId const id = _next_constraint_id++;
    _constraints.emplace(id, definition);
    out.id = id;
    return GdiStatus::ok;
}

GdiStatus Schema::add_label_condition(ConstraintHandle handle, LabelId label, GdiOp op) {
    // GDI only defines membership semantics for a label condition; an ordering op
    // is a caller error, and the reference backend raises ErrorOp for it.
    if (op != GdiOp::equal && op != GdiOp::not_equal) {
        return GdiStatus::error_op;
    }
    if (label == kNoLabel) {
        return GdiStatus::error_label;
    }
    std::unique_lock<std::shared_mutex> lock(_mutex);
    auto it = _constraints.find(handle.id);
    if (it == _constraints.end()) {
        return GdiStatus::error_constraint;
    }
    it->second.conditions.push_back(ConstraintCondition::on_label(label, op));
    return GdiStatus::ok;
}

GdiStatus Schema::add_property_condition(ConstraintHandle handle, PropNameId property,
                                         GdiOp op, PropertyValue value, uint32_t count) {
    if (property == kNoPropName) {
        return GdiStatus::error_property_type;
    }
    if (op == GdiOp::any_in) {
        // Plan §1.2: for an any_in condition the VALUE IS THE SET, so the value's
        // kind — not the caller's count — decides what is legal here. Only the two
        // array kinds can hold a set; anything else is a datatype mismatch, refused
        // before the condition exists. An EMPTY set is legal and never matches; the
        // size is bounded only by the block's own well-formedness (u32 range, §1.4),
        // which the ABI write path already checked — there is no extra cap here.
        if (value.kind() != PropertyKind::string_array &&
            value.kind() != PropertyKind::int64_array) {
            return GdiStatus::error_op_datatype_mismatch;
        }
        // The stored count is the set's exact element count (§1.4): the caller's
        // number is informational, and a disagreement must not become a lie in the
        // condition (0 included — the empty set keeps 0 here, unlike other ops).
        count = static_cast<uint32_t>(value.element_count());
    } else if (count < 1) {
        return GdiStatus::error_count;
    }
    std::unique_lock<std::shared_mutex> lock(_mutex);
    auto it = _constraints.find(handle.id);
    if (it == _constraints.end()) {
        return GdiStatus::error_constraint;
    }
    it->second.conditions.push_back(
        ConstraintCondition::on_property(property, op, std::move(value), count));
    return GdiStatus::ok;
}

GdiStatus Schema::free_constraint(ConstraintHandle handle) {
    std::unique_lock<std::shared_mutex> lock(_mutex);
    auto it = _constraints.find(handle.id);
    if (it == _constraints.end()) {
        return GdiStatus::error_constraint;
    }
    _constraints.erase(it);
    return GdiStatus::ok;
}

bool Schema::constraint(ConstraintHandle handle, Constraint &out) const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    auto it = _constraints.find(handle.id);
    if (it == _constraints.end()) {
        return false;
    }
    out = it->second;
    return true;
}

size_t Schema::constraint_count() const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    return _constraints.size();
}

// ---------------------------------------------------------------------------
// Versioning / persistence
// ---------------------------------------------------------------------------

uint32_t Schema::version() const noexcept {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    return _version;
}

void Schema::bump_version() noexcept {
    std::unique_lock<std::shared_mutex> lock(_mutex);
    ++_version;
}

GdiStatus Schema::restore_label(LabelTableEntry const &entry) {
    if (entry.id == kNoLabel) {
        return GdiStatus::error_argument;
    }
    if (is_blank(entry.name)) {
        return GdiStatus::error_empty_name;
    }
    std::unique_lock<std::shared_mutex> lock(_mutex);
    for (auto const &existing : _labels) {
        if (existing.id == entry.id) {
            return GdiStatus::error_name_exists;
        }
    }
    if (_find_label_locked(entry.name) != kNoLabel) {
        return GdiStatus::error_name_exists;
    }
    _labels.push_back(entry);
    _label_by_name.emplace(entry.name, entry.id);
    if (entry.id >= _next_label_id) {
        _next_label_id = entry.id + 1;
    }
    ++_version;
    return GdiStatus::ok;
}

GdiStatus Schema::restore_property_type(PropertyTypeInfo const &info) {
    if (info.name_id == kNoPropName) {
        return GdiStatus::error_argument;
    }
    if (is_blank(info.name)) {
        return GdiStatus::error_empty_name;
    }
    std::unique_lock<std::shared_mutex> lock(_mutex);
    for (auto const &slot : _property_types) {
        if (slot.id == info.name_id) {
            return GdiStatus::error_name_exists;
        }
    }
    if (_find_property_type_locked(info.name) != kNoPropName) {
        return GdiStatus::error_name_exists;
    }
    PropertyTypeSlot slot;
    slot.id = info.name_id;
    slot.info = info;
    _property_type_by_name.emplace(info.name, info.name_id);
    _property_types.push_back(std::move(slot));
    if (info.name_id >= _next_prop_name_id) {
        _next_prop_name_id = info.name_id + 1;
    }
    ++_version;
    return GdiStatus::ok;
}

void Schema::snapshot(kimix::vector<LabelTableEntry> &labels, PropertyTypeList &types,
                      uint32_t &next_label_id, uint32_t &next_prop_name_id) const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    labels.clear();
    labels.reserve(_labels.size());
    for (auto const &entry : _labels) {
        labels.push_back(entry);
    }
    types.clear();
    types.reserve(_property_types.size());
    for (auto const &slot : _property_types) {
        types.push_back(slot.info);
    }
    next_label_id = _next_label_id;
    next_prop_name_id = _next_prop_name_id;
}

bool Schema::restore(kimix::vector<LabelTableEntry> const &labels,
                     PropertyTypeList const &types, uint32_t next_label_id,
                     uint32_t next_prop_name_id, uint32_t schema_version) {
    // Validate before mutating: a corrupt table must leave the schema usable.
    kimix::unordered_map<LabelId, uint8_t> seen_labels;
    LabelId max_label = 0;
    for (auto const &entry : labels) {
        if (entry.id == kNoLabel) {
            return false;
        }
        if (!seen_labels.emplace(entry.id, 1).second) {
            return false;
        }
        if (entry.id > max_label) {
            max_label = entry.id;
        }
    }
    kimix::unordered_map<PropNameId, uint8_t> seen_types;
    PropNameId max_type = 0;
    for (auto const &entry : types) {
        if (entry.name_id == kNoPropName) {
            return false;
        }
        if (!seen_types.emplace(entry.name_id, 1).second) {
            return false;
        }
        if (entry.name_id > max_type) {
            max_type = entry.name_id;
        }
    }

    std::unique_lock<std::shared_mutex> lock(_mutex);
    _labels.clear();
    _label_by_name.clear();
    for (auto const &entry : labels) {
        _label_by_name.emplace(entry.name, entry.id);
        _labels.push_back(entry);
    }
    _property_types.clear();
    _property_type_by_name.clear();
    for (auto const &entry : types) {
        PropertyTypeSlot slot;
        slot.id = entry.name_id;
        slot.info = entry;
        _property_type_by_name.emplace(entry.name, entry.name_id);
        _property_types.push_back(std::move(slot));
    }
    // The counters must stay ahead of every restored id: ids are never recycled,
    // and a stale counter would hand out a live id.
    _next_label_id = next_label_id > max_label ? next_label_id : max_label + 1;
    _next_prop_name_id = next_prop_name_id > max_type ? next_prop_name_id : max_type + 1;
    _version = schema_version;
    return true;
}

} // namespace kimix::graph_db
