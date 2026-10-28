/*
 * property.cpp — PropertyValue construction, access, equality and the GDI
 * comparison semantics (see property.h for the contract).
 *
 * Nothing here allocates on a failure path, nothing throws, and every accessor
 * is total: a kind mismatch yields a zeroed/empty value.
 */
#include "binary_le.h"
#include "property.h"

#include <core/stl/memory.h> // kimix::allocator (owned payload blocks)

#include <algorithm>
#include <cstring>
#include <utility>

namespace kimix::graph_db {

namespace src_graph_db_property {

// Byte view of a string value (the payload vector stores UTF-8 bytes verbatim).
inline kimix::span<const std::byte> bytes_of(kimix::string_view text) noexcept {
    return kimix::span<const std::byte>{
        reinterpret_cast<std::byte const *>(text.data()), text.size()};
}

inline kimix::string_view text_of(kimix::vector<std::byte> const &bytes) noexcept {
    if (bytes.empty()) {
        return {};
    }
    return kimix::string_view{reinterpret_cast<char const *>(bytes.data()), bytes.size()};
}

inline bool same_bytes(kimix::vector<std::byte> const &lhs,
                       kimix::vector<std::byte> const &rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    if (lhs.empty()) {
        return true;
    }
    return std::memcmp(lhs.data(), rhs.data(), lhs.size()) == 0;
}

template<typename T>
inline bool same_sequence(kimix::vector<T> const &lhs, kimix::vector<T> const &rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    return std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

// Decodes the element whose u32 length field starts at `at`, inside
// [bytes.data(), end): `out` is the element's text and `next` the position of the
// FOLLOWING element's length field. `at` itself is never advanced — the caller's
// cursor therefore always names the element it is holding, which is what makes an
// end-of-walk comparison exact (an iterator that advanced past the element it
// yielded would stop one element early). On ANY failure `out` is empty and `next`
// is `end`, so the caller stops. Never reads outside [at, end).
inline bool read_string_array_element(std::byte const *at, std::byte const *end,
                                      kimix::string_view &out,
                                      std::byte const *&next) noexcept {
    using namespace detail;
    out = {};
    next = end;
    if (at == nullptr || static_cast<size_t>(end - at) < kStringArrayLengthSize) {
        return false;
    }
    uint32_t const length = read_u32(at);
    std::byte const *const text = at + kStringArrayLengthSize;
    if (static_cast<uint64_t>(length) > static_cast<uint64_t>(end - text)) {
        return false;
    }
    out = kimix::string_view{reinterpret_cast<char const *>(text), length};
    next = text + length;
    return true;
}

// Membership of `text` in a string_array SET value: the linear walk that DEFINES
// the ANY_IN answer (property.h). The hot path never comes here — a constraint
// probes the sorted index ConstraintCondition builds once at add time.
inline bool set_contains_text(PropertyValue const &set_value,
                              kimix::string_view text) noexcept {
    for (auto const &element : set_value.as_string_array()) {
        if (element == text) {
            return true;
        }
    }
    return false;
}

// The int64_array twin of set_contains_text.
inline bool set_contains_int64(PropertyValue const &set_value, int64_t value) noexcept {
    for (auto const element : set_value.as_int64_array()) {
        if (element == value) {
            return true;
        }
    }
    return false;
}

} // namespace src_graph_db_property
using namespace src_graph_db_property;

// ---------------------------------------------------------------------------
// Type vocabulary names
// ---------------------------------------------------------------------------

char const *op_name(GdiOp op) noexcept {
    switch (op) {
    case GdiOp::equal: return "equal";
    case GdiOp::not_equal: return "not_equal";
    case GdiOp::greater: return "greater";
    case GdiOp::greater_or_equal: return "greater_or_equal";
    case GdiOp::less: return "less";
    case GdiOp::less_or_equal: return "less_or_equal";
    case GdiOp::any_in: return "any_in";
    }
    return "op_unknown";
}

char const *data_type_name(GdiDataType type) noexcept {
    switch (type) {
    case GdiDataType::char_: return "char";
    case GdiDataType::int8_: return "int8";
    case GdiDataType::uint8_: return "uint8";
    case GdiDataType::boolean: return "bool";
    case GdiDataType::byte_: return "byte";
    case GdiDataType::int16_: return "int16";
    case GdiDataType::uint16_: return "uint16";
    case GdiDataType::int32_: return "int32";
    case GdiDataType::uint32_: return "uint32";
    case GdiDataType::float_: return "float";
    case GdiDataType::date: return "date";
    case GdiDataType::time: return "time";
    case GdiDataType::int64_: return "int64";
    case GdiDataType::uint64_: return "uint64";
    case GdiDataType::double_: return "double";
    case GdiDataType::datetime: return "datetime";
    case GdiDataType::decimal: return "decimal";
    }
    return "datatype_unknown";
}

char const *container_type_name(GdiContainerType type) noexcept {
    switch (type) {
    case GdiContainerType::single: return "single";
    case GdiContainerType::array: return "array";
    }
    return "container_unknown";
}

char const *property_kind_name(PropertyKind kind) noexcept {
    switch (kind) {
    case PropertyKind::none: return "none";
    case PropertyKind::int64_value: return "int64";
    case PropertyKind::double_value: return "double";
    case PropertyKind::string: return "string";
    case PropertyKind::blob: return "blob";
    case PropertyKind::int64_array: return "int64_array";
    case PropertyKind::double_array: return "double_array";
    case PropertyKind::string_array: return "string_array";
    }
    return "kind_unknown";
}

// ---------------------------------------------------------------------------
// The string-array wire block (plan §1.1)
// ---------------------------------------------------------------------------

bool string_array_block_validate(kimix::span<const std::byte> block,
                                 uint32_t &out_count) noexcept {
    out_count = 0;
    using namespace detail;
    if (block.data() == nullptr || block.size() < kStringArrayCountSize) {
        return false;
    }
    uint32_t const count = read_u32(block.data());
    std::byte const *at = block.data() + kStringArrayCountSize;
    std::byte const *const end = block.data() + block.size();
    for (uint32_t i = 0; i < count; ++i) {
        kimix::string_view element;
        std::byte const *next = nullptr;
        if (!read_string_array_element(at, end, element, next)) {
            return false;
        }
        at = next;
    }
    // The walk must consume the block EXACTLY: trailing bytes mean the declared
    // count does not describe this block, and an empty array is the bare 4-byte
    // count and nothing else.
    if (at != end) {
        return false;
    }
    out_count = count;
    return true;
}

// ---------------------------------------------------------------------------
// StringArrayView
// ---------------------------------------------------------------------------

StringArrayView::StringArrayView(std::byte const *data, size_t bytes) noexcept
    : _data(data), _bytes(bytes) {}

uint32_t StringArrayView::element_count() const noexcept {
    if (_data == nullptr || _bytes < kStringArrayCountSize) {
        return 0;
    }
    return detail::read_u32(_data);
}

StringArrayView::iterator::iterator(std::byte const *cursor, std::byte const *end) noexcept
    : _cursor(cursor), _next(cursor), _end(end) {
    _decode();
}

void StringArrayView::iterator::_decode() noexcept {
    // `_cursor` is the length-field position of the element this iterator yields,
    // so the comparison against end() (whose cursor is the block's byte end) is
    // exact: the LAST element is yielded before the walk stops.
    if (_cursor == nullptr || _cursor >= _end) {
        _cursor = _end;
        _next = _end;
        _current = {};
        return;
    }
    if (!read_string_array_element(_cursor, _end, _current, _next)) {
        // A length field or payload that does not fit: stop at the end rather than
        // read on — a malformed tail terminates, it never runs off the block.
        _cursor = _end;
        _next = _end;
        _current = {};
    }
}

StringArrayView::iterator &StringArrayView::iterator::operator++() noexcept {
    _cursor = _next; // step onto the following element (== _end at the end)
    _decode();
    return *this;
}

kimix::string_view StringArrayView::element(uint32_t index) const noexcept {
    if (_data == nullptr || _bytes < kStringArrayCountSize) {
        return {};
    }
    uint32_t const count = detail::read_u32(_data);
    std::byte const *at = _data + kStringArrayCountSize;
    std::byte const *const end = _data + _bytes;
    for (uint32_t i = 0; i < count; ++i) {
        kimix::string_view current;
        std::byte const *next = nullptr;
        if (!read_string_array_element(at, end, current, next)) {
            return {}; // malformed tail: total, empty (the block cannot be walked)
        }
        if (i == index) {
            return current;
        }
        at = next;
    }
    return {}; // index >= count
}

StringArrayView::iterator StringArrayView::begin() const noexcept {
    std::byte const *const last = (_bytes == 0) ? _data : _data + _bytes;
    if (_data == nullptr || _bytes < kStringArrayCountSize) {
        return iterator{last, last}; // empty or too short to hold a count: begin == end
    }
    return iterator{_data + kStringArrayCountSize, last};
}

StringArrayView::iterator StringArrayView::end() const noexcept {
    std::byte const *const last = (_bytes == 0) ? _data : _data + _bytes;
    return iterator{last, last};
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PropertyValue PropertyValue::make_int64(int64_t value) noexcept {
    PropertyValue out;
    out._kind = PropertyKind::int64_value;
    out._payload.integer = value;
    return out;
}

PropertyValue::PropertyValue(PropertyValue const &other) {
    _clone_from(other);
}

PropertyValue &PropertyValue::operator=(PropertyValue const &other) {
    if (this != &other) {
        _release();
        _clone_from(other);
    }
    return *this;
}

PropertyValue::PropertyValue(PropertyValue &&other) noexcept
    : _kind(other._kind), _payload(other._payload) {
    other._kind = PropertyKind::none;
    other._payload.integer = 0;
}

PropertyValue &PropertyValue::operator=(PropertyValue &&other) noexcept {
    if (this != &other) {
        _release();
        _kind = other._kind;
        _payload = other._payload;
        other._kind = PropertyKind::none;
        other._payload.integer = 0;
    }
    return *this;
}

PropertyValue::~PropertyValue() {
    _release();
}

void PropertyValue::_release() noexcept {
    switch (_kind) {
    case PropertyKind::string:
    case PropertyKind::blob:
        if (_payload.bytes.data != nullptr) {
            kimix::allocator<std::byte>{}.deallocate(_payload.bytes.data,
                                                     _payload.bytes.count);
        }
        break;
    case PropertyKind::int64_array:
        if (_payload.int64s.data != nullptr) {
            kimix::allocator<int64_t>{}.deallocate(_payload.int64s.data,
                                                   _payload.int64s.count);
        }
        break;
    case PropertyKind::double_array:
        if (_payload.doubles.data != nullptr) {
            kimix::allocator<double>{}.deallocate(_payload.doubles.data,
                                                  _payload.doubles.count);
        }
        break;
    case PropertyKind::string_array:
        // The whole §1.1 block is one allocation (make_string_array and the record
        // decoder always own at least the 4-byte count field).
        if (_payload.strings.data != nullptr) {
            kimix::allocator<std::byte>{}.deallocate(_payload.strings.data,
                                                     _payload.strings.bytes);
        }
        break;
    default:
        break;
    }
    _kind = PropertyKind::none;
    _payload.integer = 0;
}

void PropertyValue::_clone_from(PropertyValue const &other) {
    _kind = other._kind;
    switch (_kind) {
    case PropertyKind::int64_value:
        _payload.integer = other._payload.integer;
        break;
    case PropertyKind::double_value:
        _payload.real = other._payload.real;
        break;
    case PropertyKind::string:
    case PropertyKind::blob: {
        _payload.bytes.count = other._payload.bytes.count;
        _payload.bytes.data = nullptr;
        if (_payload.bytes.count != 0) {
            kimix::allocator<std::byte> alloc{};
            _payload.bytes.data = alloc.allocate(_payload.bytes.count);
            std::memcpy(_payload.bytes.data, other._payload.bytes.data,
                        _payload.bytes.count);
        }
        break;
    }
    case PropertyKind::int64_array: {
        _payload.int64s.count = other._payload.int64s.count;
        _payload.int64s.data = nullptr;
        if (_payload.int64s.count != 0) {
            kimix::allocator<int64_t> alloc{};
            _payload.int64s.data = alloc.allocate(_payload.int64s.count);
            std::copy(other._payload.int64s.data,
                      other._payload.int64s.data + _payload.int64s.count,
                      _payload.int64s.data);
        }
        break;
    }
    case PropertyKind::double_array: {
        _payload.doubles.count = other._payload.doubles.count;
        _payload.doubles.data = nullptr;
        if (_payload.doubles.count != 0) {
            kimix::allocator<double> alloc{};
            _payload.doubles.data = alloc.allocate(_payload.doubles.count);
            std::copy(other._payload.doubles.data,
                      other._payload.doubles.data + _payload.doubles.count,
                      _payload.doubles.data);
        }
        break;
    }
    case PropertyKind::string_array: {
        // Deep copy of the ONE owned block, bytes verbatim: the copy's elements are
        // independent of the source (no borrowed element views survive an assignment).
        _payload.strings.bytes = other._payload.strings.bytes;
        _payload.strings.data = nullptr;
        if (_payload.strings.bytes != 0) {
            kimix::allocator<std::byte> alloc{};
            _payload.strings.data = alloc.allocate(_payload.strings.bytes);
            std::memcpy(_payload.strings.data, other._payload.strings.data,
                        _payload.strings.bytes);
        }
        break;
    }
    default:
        _payload.integer = 0;
        break;
    }
}

PropertyValue PropertyValue::make_double(double value) noexcept {
 PropertyValue out;
 out._kind = PropertyKind::double_value;
 out._payload.real = value;
 return out;
}

PropertyValue PropertyValue::make_string(kimix::string_view value) {
    PropertyValue out;
    out._kind = PropertyKind::string;
    auto const bytes = bytes_of(value);
    out._payload.bytes.count = bytes.size();
    out._payload.bytes.data = nullptr;
    if (!bytes.empty()) {
        kimix::allocator<std::byte> alloc{};
        out._payload.bytes.data = alloc.allocate(bytes.size());
        std::memcpy(out._payload.bytes.data, bytes.data(), bytes.size());
    }
    return out;
}

PropertyValue PropertyValue::make_blob(kimix::span<const std::byte> value) {
    PropertyValue out;
    out._kind = PropertyKind::blob;
    out._payload.bytes.count = value.size();
    out._payload.bytes.data = nullptr;
    if (!value.empty()) {
        kimix::allocator<std::byte> alloc{};
        out._payload.bytes.data = alloc.allocate(value.size());
        std::memcpy(out._payload.bytes.data, value.data(), value.size());
    }
    return out;
}

PropertyValue PropertyValue::make_int64_array(kimix::span<const int64_t> value) {
    PropertyValue out;
    out._kind = PropertyKind::int64_array;
    out._payload.int64s.count = value.size();
    out._payload.int64s.data = nullptr;
    if (!value.empty()) {
        kimix::allocator<int64_t> alloc{};
        out._payload.int64s.data = alloc.allocate(value.size());
        std::copy(value.begin(), value.end(), out._payload.int64s.data);
    }
    return out;
}

PropertyValue PropertyValue::make_double_array(kimix::span<const double> value) {
    PropertyValue out;
    out._kind = PropertyKind::double_array;
    out._payload.doubles.count = value.size();
    out._payload.doubles.data = nullptr;
    if (!value.empty()) {
        kimix::allocator<double> alloc{};
        out._payload.doubles.data = alloc.allocate(value.size());
        std::copy(value.begin(), value.end(), out._payload.doubles.data);
    }
    return out;
}

PropertyValue PropertyValue::make_string_array(
    kimix::span<const kimix::string_view> elements) {
    PropertyValue out;
    // The §1.1 block spells the count and every element length as a u32; input
    // that cannot fit is refused as a `none` value (never an abort, never a
    // silently truncated array — see the header).
    if (elements.size() > static_cast<size_t>(UINT32_MAX)) {
        return out;
    }
    uint64_t total = kStringArrayCountSize;
    for (auto const &element : elements) {
        if (element.size() > static_cast<uint64_t>(UINT32_MAX)) {
            return out;
        }
        total += kStringArrayLengthSize + static_cast<uint64_t>(element.size());
    }
    // An element that must be longer than UINT32_MAX bytes is refused above, so
    // `total` cannot overflow a size_t here (it would need > 4 GiB of elements).
    out._kind = PropertyKind::string_array;
    out._payload.strings.bytes = static_cast<size_t>(total);
    out._payload.strings.data = nullptr;
    kimix::allocator<std::byte> alloc{};
    out._payload.strings.data = alloc.allocate(out._payload.strings.bytes);
    using namespace detail;
    std::byte *cursor = out._payload.strings.data;
    write_u32(cursor, static_cast<uint32_t>(elements.size()));
    cursor += kStringArrayCountSize;
    for (auto const &element : elements) {
        write_u32(cursor, static_cast<uint32_t>(element.size()));
        cursor += kStringArrayLengthSize;
        if (!element.empty()) {
            std::memcpy(cursor, element.data(), element.size());
            cursor += element.size();
        }
    }
    return out;
}

PropertyValue PropertyValue::make_string_array_from_block(
    kimix::span<const std::byte> block) noexcept {
    PropertyValue out;
    uint32_t count = 0;
    // The one gate: a block the validator rejects never becomes a value, so every
    // string_array in the library owns a canonical, walkable block.
    if (!string_array_block_validate(block, count)) {
        return out;
    }
    out._kind = PropertyKind::string_array;
    out._payload.strings.bytes = block.size();
    out._payload.strings.data = nullptr;
    // A valid block is at least the 4-byte count field, so `data` is never null
    // for a string_array value (the invariant _release and as_string_array rely on).
    kimix::allocator<std::byte> alloc{};
    out._payload.strings.data = alloc.allocate(block.size());
    std::memcpy(out._payload.strings.data, block.data(), block.size());
    return out;
}

void PropertyValue::clear() noexcept {
    _release();
}

// ---------------------------------------------------------------------------
// Access
// ---------------------------------------------------------------------------

kimix::string_view PropertyValue::as_string() const noexcept {
    if (_kind != PropertyKind::string) {
        return {};
    }
    if (_payload.bytes.count == 0) {
        return {};
    }
    return kimix::string_view{
        reinterpret_cast<char const *>(_payload.bytes.data), _payload.bytes.count};
}

kimix::span<const std::byte> PropertyValue::as_blob() const noexcept {
    if (_kind != PropertyKind::blob || _payload.bytes.count == 0) {
        return {};
    }
    return kimix::span<const std::byte>{_payload.bytes.data, _payload.bytes.count};
}

kimix::span<const int64_t> PropertyValue::as_int64_array() const noexcept {
    if (_kind != PropertyKind::int64_array || _payload.int64s.count == 0) {
        return {};
    }
    return kimix::span<const int64_t>{_payload.int64s.data, _payload.int64s.count};
}

kimix::span<const double> PropertyValue::as_double_array() const noexcept {
    if (_kind != PropertyKind::double_array || _payload.doubles.count == 0) {
        return {};
    }
    return kimix::span<const double>{_payload.doubles.data, _payload.doubles.count};
}

StringArrayView PropertyValue::as_string_array() const noexcept {
    if (_kind != PropertyKind::string_array) {
        return {};
    }
    return StringArrayView{_payload.strings.data, _payload.strings.bytes};
}

kimix::span<const std::byte> PropertyValue::as_string_array_block() const noexcept {
    if (_kind != PropertyKind::string_array) {
        return {};
    }
    return kimix::span<const std::byte>{_payload.strings.data, _payload.strings.bytes};
}

size_t PropertyValue::element_count() const noexcept {
    switch (_kind) {
    case PropertyKind::int64_array: return _payload.int64s.count;
    case PropertyKind::double_array: return _payload.doubles.count;
    case PropertyKind::string_array:
        // ELEMENTS, not bytes: read from the block's own leading u32 — the union
        // keeps no second count, so the block stays the single source of truth.
        return as_string_array().element_count();
    case PropertyKind::none: return 0;
    default: return 0;
    }
}

bool PropertyValue::equals(PropertyValue const &other) const noexcept {
    if (_kind != other._kind) {
        return false;
    }    switch (_kind) {
    case PropertyKind::none: return true;
    case PropertyKind::int64_value: return _payload.integer == other._payload.integer;
    case PropertyKind::double_value: return _payload.real == other._payload.real;
    case PropertyKind::string:
    case PropertyKind::blob:
        return _payload.bytes.count == other._payload.bytes.count &&
               (_payload.bytes.count == 0 ||
                std::memcmp(_payload.bytes.data, other._payload.bytes.data,
                            _payload.bytes.count) == 0);
    case PropertyKind::int64_array:
        return _payload.int64s.count == other._payload.int64s.count &&
               (_payload.int64s.count == 0 ||
                std::equal(_payload.int64s.data, _payload.int64s.data + _payload.int64s.count,
                           other._payload.int64s.data));
    case PropertyKind::double_array:
        return _payload.doubles.count == other._payload.doubles.count &&
               (_payload.doubles.count == 0 ||
                std::equal(_payload.doubles.data,
                           _payload.doubles.data + _payload.doubles.count,
                           other._payload.doubles.data));
    case PropertyKind::string_array:
        // The block is CANONICAL (one byte image per value: count, then each
        // element's length and bytes, in order), so a byte compare of the two
        // blocks IS whole-array equality — same count, same order, same element
        // bytes — exactly like the C# reference's array comparison.
        return _payload.strings.bytes == other._payload.strings.bytes &&
               (_payload.strings.bytes == 0 ||
                std::memcmp(_payload.strings.data, other._payload.strings.data,
                            _payload.strings.bytes) == 0);
    }
    return false;
}

GdiDataType PropertyValue::data_type() const noexcept {
    switch (_kind) {
    case PropertyKind::int64_value:
    case PropertyKind::int64_array:
        return GdiDataType::int64_;
    case PropertyKind::double_value:
    case PropertyKind::double_array:
        return GdiDataType::double_;
    case PropertyKind::string:
    case PropertyKind::string_array:
        return GdiDataType::char_;
    case PropertyKind::blob:
        return GdiDataType::byte_;
    case PropertyKind::none:
        return GdiDataType::int64_; // unreachable: `none` never validates
    }
    return GdiDataType::int64_;
}

bool PropertyValue::matches_declaration(GdiContainerType container, GdiDataType data,
                                        uint32_t count) const noexcept {
    if (_kind == PropertyKind::none) {
        return false;
    }
    bool const is_array = _kind == PropertyKind::int64_array ||
                          _kind == PropertyKind::double_array ||
                          _kind == PropertyKind::string_array;
    if (is_array != (container == GdiContainerType::array)) {
        return false;
    }
    if (data_type() != data) {
        return false;
    }
    if (is_array) {
        return element_count() == static_cast<size_t>(count);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Comparison (Semevia.Gdi's InMemoryStore.Compare)
// ---------------------------------------------------------------------------

kimix::optional<int32_t> compare_values(PropertyValue const &lhs,
                                        PropertyValue const &rhs) noexcept {
    PropertyKind const left = lhs.kind();
    PropertyKind const right = rhs.kind();
    // Numeric cross-kind comparison: an int64 is widened to double, exactly like
    // the reference's `(double)actual.AsInt64`.
    if (left == PropertyKind::int64_value && right == PropertyKind::int64_value) {
        int64_t const a = lhs.as_int64();
        int64_t const b = rhs.as_int64();
        if (a < b) {
            return -1;
        }
        if (a > b) {
            return 1;
        }
        return 0;
    }
    if (left == PropertyKind::double_value && right == PropertyKind::double_value) {
        double const a = lhs.as_double();
        double const b = rhs.as_double();
        if (a < b) {
            return -1;
        }
        if (a > b) {
            return 1;
        }
        return 0;
    }
    if (left == PropertyKind::int64_value && right == PropertyKind::double_value) {
        double const a = static_cast<double>(lhs.as_int64());
        double const b = rhs.as_double();
        if (a < b) {
            return -1;
        }
        if (a > b) {
            return 1;
        }
        return 0;
    }
    if (left == PropertyKind::double_value && right == PropertyKind::int64_value) {
        double const a = lhs.as_double();
        double const b = static_cast<double>(rhs.as_int64());
        if (a < b) {
            return -1;
        }
        if (a > b) {
            return 1;
        }
        return 0;
    }
    if (left == PropertyKind::string && right == PropertyKind::string) {
        auto const a = lhs.as_string();
        auto const b = rhs.as_string();
        int const order = a.compare(b);
        if (order < 0) {
            return -1;
        }
        if (order > 0) {
            return 1;
        }
        return 0;
    }
    // Same-but-unorderable (blob, the three array kinds) is a defined equality;
    // mixed kinds are unequal. Neither supports an ordering, so compare_values
    // answers 0 for an equal string_array and nullopt otherwise — the record
    // codec's whole-array equality, nothing more.
    if (lhs.equals(rhs)) {
        return 0;
    }
    return std::nullopt;
}

bool any_in_matches(PropertyValue const &set_value, PropertyValue const &actual) noexcept {
    // The SET decides what can be probed at all (plan §1.2): only a string_array or
    // an int64_array value is a set; anything else — and an empty set — is never
    // satisfied. The walk is deliberately linear: this is the semantics definition,
    // not a hot path (see the header).
    switch (set_value.kind()) {
    case PropertyKind::string_array: {
        switch (actual.kind()) {
        case PropertyKind::string:
            return set_contains_text(set_value, actual.as_string());
        case PropertyKind::string_array:
            for (auto const &element : actual.as_string_array()) {
                if (set_contains_text(set_value, element)) {
                    return true;
                }
            }
            return false;
        default:
            return false; // an int64/double/blob is not a member of a string set
        }
    }
    case PropertyKind::int64_array: {
        switch (actual.kind()) {
        case PropertyKind::int64_value:
            return set_contains_int64(set_value, actual.as_int64());
        case PropertyKind::int64_array:
            for (auto const element : actual.as_int64_array()) {
                if (set_contains_int64(set_value, element)) {
                    return true;
                }
            }
            return false;
        default:
            return false; // a string/double/blob is not a member of an int64 set
        }
    }
    default:
        return false; // not a set: an empty condition value or a non-array kind
    }
}

bool evaluate(GdiOp op, PropertyValue const &actual, PropertyValue const &expected) noexcept {
    // ANY_IN is a membership test, not a comparison: `expected` is the SET and
    // `actual` the property value, so it answers before the ordering rules run and
    // never consults compare_values (a set is not orderable against a scalar).
    if (op == GdiOp::any_in) {
        return any_in_matches(expected, actual);
    }
    auto const order = compare_values(actual, expected);
    if (!order) {
        // Unorderable pair: only (in)equality is meaningful.
        bool const same = actual.equals(expected);
        if (op == GdiOp::equal) {
            return same;
        }
        if (op == GdiOp::not_equal) {
            return !same;
        }
        return false;
    }
    int32_t const value = *order;
    switch (op) {
    case GdiOp::equal: return value == 0;
    case GdiOp::not_equal: return value != 0;
    case GdiOp::greater: return value > 0;
    case GdiOp::greater_or_equal: return value >= 0;
    case GdiOp::less: return value < 0;
    case GdiOp::less_or_equal: return value <= 0;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Record-level property codec
// ---------------------------------------------------------------------------

namespace src_graph_db_property {

// Bytes of the value payload (the scalars are 8 fixed bytes, the fixed-width
// arrays 8 bytes per element, the byte kinds their own length, and a string_array
// is its whole §1.1 block).
inline size_t payload_size(PropertyValue const &value) noexcept {
    switch (value.kind()) {
    case PropertyKind::none: return 0;
    case PropertyKind::int64_value:
    case PropertyKind::double_value: return 8;
    case PropertyKind::string: return value.as_string().size();
    case PropertyKind::blob: return value.as_blob().size();
    case PropertyKind::int64_array: return value.as_int64_array().size() * 8;
    case PropertyKind::double_array: return value.as_double_array().size() * 8;
    case PropertyKind::string_array:
        // The §1.1 block verbatim — its own size, because the block already carries
        // its count and its per-element lengths (no extra framing on disk).
        return value.as_string_array_block().size();
    }
    return 0;
}

inline uint32_t payload_count(PropertyValue const &value) noexcept {
    switch (value.kind()) {
    case PropertyKind::int64_array:
    case PropertyKind::double_array:
    case PropertyKind::string_array:
        return static_cast<uint32_t>(value.element_count());
    default:
        return 1;
    }
}

inline uint64_t double_bits(double value) noexcept {
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline double bits_double(uint64_t bits) noexcept {
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

} // namespace src_graph_db_property
using namespace src_graph_db_property;

size_t encoded_property_entry_size(PropertyEntry const &entry) noexcept {
    // name + kind + dtype + reserved + count + payload length, then the payload.
    return 20 + payload_size(entry.value);
}

void encode_property_entry(std::byte *dst, PropertyEntry const &entry) noexcept {
    using namespace detail;
    write_u32(dst + 0, entry.name);
    write_u8(dst + 4, static_cast<uint8_t>(entry.value.kind()));
    write_u8(dst + 5, static_cast<uint8_t>(entry.value.data_type()));
    write_u16(dst + 6, 0); // reserved
    write_u32(dst + 8, payload_count(entry.value));
    write_u64(dst + 12, static_cast<uint64_t>(payload_size(entry.value)));
    std::byte *payload = dst + 20;
    switch (entry.value.kind()) {
    case PropertyKind::none:
        break;
    case PropertyKind::int64_value:
        write_u64(payload, static_cast<uint64_t>(entry.value.as_int64()));
        break;
    case PropertyKind::double_value:
        write_u64(payload, double_bits(entry.value.as_double()));
        break;
    case PropertyKind::string:
        write_bytes(payload, bytes_of(entry.value.as_string()));
        break;
    case PropertyKind::blob:
        write_bytes(payload, entry.value.as_blob());
        break;
    case PropertyKind::int64_array: {
        auto const values = entry.value.as_int64_array();
        for (size_t i = 0; i < values.size(); ++i) {
            write_u64(payload + i * 8, static_cast<uint64_t>(values[i]));
        }
        break;
    }
    case PropertyKind::double_array: {
        auto const values = entry.value.as_double_array();
        for (size_t i = 0; i < values.size(); ++i) {
            write_u64(payload + i * 8, double_bits(values[i]));
        }
        break;
    }
    case PropertyKind::string_array:
        // Encoding IS the block: the payload bytes are the value's own canonical
        // §1.1 image, copied verbatim (no element loop, no second encoder).
        write_bytes(payload, entry.value.as_string_array_block());
        break;
    }
}

bool decode_property_entry(std::byte const *&cursor, std::byte const *end,
                           PropertyEntry &out) noexcept {
    using namespace detail;
    out = PropertyEntry{};
    if (end - cursor < 20) {
        return false;
    }
    // The name is held back until the payload decoded: the contract is that a
    // `false` leaves `out` default-constructed, so no field of a refused entry
    // (name included) is ever written.
    PropNameId const name = read_u32(cursor + 0);
    auto const kind = static_cast<PropertyKind>(read_u8(cursor + 4));
    uint32_t const count = read_u32(cursor + 8);
    uint64_t const payload_len = read_u64(cursor + 12);
    cursor += 20;
    if (payload_len > static_cast<uint64_t>(end - cursor)) {
        return false;
    }
    std::byte const *payload = cursor;
    switch (kind) {
    case PropertyKind::none:
        out.value = PropertyValue{};
        break;
    case PropertyKind::int64_value:
        if (payload_len != 8) {
            return false;
        }
        out.value = PropertyValue::make_int64(static_cast<int64_t>(read_u64(payload)));
        break;
    case PropertyKind::double_value:
        if (payload_len != 8) {
            return false;
        }
        out.value = PropertyValue::make_double(bits_double(read_u64(payload)));
        break;
    case PropertyKind::string:
        out.value = PropertyValue::make_string(kimix::string_view{
            reinterpret_cast<char const *>(payload), static_cast<size_t>(payload_len)});
        break;
    case PropertyKind::blob:
        out.value = PropertyValue::make_blob(
            kimix::span<const std::byte>{payload, static_cast<size_t>(payload_len)});
        break;
    case PropertyKind::int64_array: {
        if (payload_len != static_cast<uint64_t>(count) * 8ull) {
            return false;
        }
        kimix::vector<int64_t> values;
        values.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            values.push_back(static_cast<int64_t>(read_u64(payload + i * 8)));
        }
        out.value = PropertyValue::make_int64_array(
            kimix::span<const int64_t>{values.data(), values.size()});
        break;
    }
    case PropertyKind::double_array: {
        if (payload_len != static_cast<uint64_t>(count) * 8ull) {
            return false;
        }
        kimix::vector<double> values;
        values.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            values.push_back(bits_double(read_u64(payload + i * 8)));
        }
        out.value = PropertyValue::make_double_array(
            kimix::span<const double>{values.data(), values.size()});
        break;
    }
    case PropertyKind::string_array: {
        // STRICT block decode, the same defensive posture as the fixed-width arms:
        // the validator walks the block with a bounds-checked cursor (never past
        // `end`, each element's u32 length must fit, the walk must consume the
        // payload exactly), and the record's own `count` field must agree with the
        // count the block declares. Any disagreement fails the whole record read.
        uint32_t block_count = 0;
        kimix::span<const std::byte> const block{payload, static_cast<size_t>(payload_len)};
        if (!string_array_block_validate(block, block_count) ||
            block_count != count || payload_len < kStringArrayCountSize) {
            return false;
        }
        out.value = PropertyValue::make_string_array_from_block(block);
        if (out.value.kind() != PropertyKind::string_array) {
            return false; // defensive: the validator already accepted this block
        }
        break;
    }
    default:
        return false;
    }
    out.name = name; // only now: the payload decoded, so the entry is complete
    cursor += static_cast<size_t>(payload_len);
    return true;
}

} // namespace kimix::graph_db
