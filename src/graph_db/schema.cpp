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
    condition.count = count == 0 ? 1 : count;
    return condition;
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
        write_u16(cursor, 0); // reserved
        cursor += 2;
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
        cursor += 2; // reserved
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
    if (info.count < 1) {
        return GdiStatus::error_count;
    }
    std::unique_lock<std::shared_mutex> lock(_mutex);
    if (_find_property_type_locked(info.name) != kNoPropName) {
        return GdiStatus::error_name_exists;
    }
    PropertyTypeSlot slot;
    slot.id = _next_prop_name_id++;
    slot.info = info;
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
    // immediately usable for the value that created it.
    PropertyTypeInfo created = declaration;
    created.name.assign(name.data(), name.size());
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
    if (info.count < 1) {
        return GdiStatus::error_count;
    }
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
    // exists.
    bool const shape_changed = slot->info.container != info.container ||
                               slot->info.data != info.data || slot->info.count != info.count;
    if (shape_changed && values_exist) {
        return GdiStatus::error_read_only_property_type;
    }
    if (!same_text(slot->info.name, kimix::string_view{info.name.data(), info.name.size()})) {
        _property_type_by_name.erase(slot->info.name);
        slot->info.name = info.name;
        _property_type_by_name.emplace(slot->info.name, id);
    }
    slot->info.container = info.container;
    slot->info.data = info.data;
    slot->info.count = info.count;
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
    if (count < 1) {
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
