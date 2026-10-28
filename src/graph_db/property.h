/*
 * property.h — the typed property model of kimix-graph-db (GDI §2.1).
 *
 * A Vertex or an Edge carries a PropertyBag: an interned-name -> PropertyValue
 * map. It sits BESIDE the legacy opaque content blob (Vertex::content()), which
 * stays exactly as it was, so the lower-level consumers and their tests keep
 * working and a GDI adapter still has a place for a raw payload that has no
 * schema.
 *
 * NAMES ARE INTERNED. A PropertyBag is keyed by PropNameId (a 4-byte id the
 * Schema allocates), never by a string: that keeps a record small, makes
 * Schema::rename_property_type O(1) (entities store the id, only the registry's
 * name table changes) and makes a property lookup one integer probe. Both
 * PropertyTypeInfo and the read-back API carry the name text as well, so a
 * caller that has an id can always recover the name without a second round trip.
 *
 * KINDS. PropertyKind mirrors Semevia.Gdi.PropertyValue.ValueKind exactly
 * (None/Int64/Double/String/Blob/Int64Array/DoubleArray/StringArray — ABI 7
 * adds the last one). The enumerator `double_value` spells ValueKind.Double and
 * `int64_value` ValueKind.Int64 — `double`/`int64` are unavailable as
 * identifiers (a keyword and an MSVC predefined type), and the C ABI
 * (graph_c_api.h) uses the same spellings.
 *
 * STORAGE / TRADEOFF. Scalars are inline in a 24-byte tagged union; the
 * variable-size kinds (string, blob, and the three array kinds) own exactly ONE
 * heap block each, so a copy of a blob value copies the blob and a scalar costs
 * no allocation. That is the deliberate choice: it is value-semantics correct
 * and needs no arena lifetime rules — the record codec (utils.h) writes the
 * payload inline either way.
 *
 * STRING-ARRAY WIRE BLOCK (ABI 7, plan §1.1). A string_array value stores its
 * elements as ONE canonical block, and that same byte image is BOTH the C ABI
 * payload and the record codec's payload — one layout, one encoder, one
 * decoder:
 *
 *   [4B element count][4B byte length of element 0][utf-8 bytes]...
 *
 * little-endian, no padding, no terminator. The block is canonical (one byte
 * image per value), so comparing two string_array values is a memcmp, and
 * encoding one is a verbatim copy. The element count lives ONLY in the block's
 * leading u32 — the union member keeps {data, bytes} and never a second count,
 * so there is exactly one source of truth. string_array_block_validate() is the
 * single well-formedness check (record decoder and C ABI write path both use
 * it); StringArrayView walks a block without allocating.
 *
 * Example:
 *   kimix::graph_db::PropertyValue v = kimix::graph_db::PropertyValue::make_int64(42);
 *   kimix::graph_db::PropertyValue s = kimix::graph_db::PropertyValue::make_string("hello");
 *   if (kimix::graph_db::evaluate(kimix::graph_db::GdiOp::greater, v,
 *                                 kimix::graph_db::PropertyValue::make_int64(7))) { ... }
 */
#pragma once

#include "graph_db_api.h"

#include <core/stl/memory.h>
#include <core/stl/optional.h>
#include <core/stl/string.h>
#include <core/stl/unordered_map.h>
#include <core/stl/vector.h>

#include <cstddef>
#include <cstdint>

namespace kimix::graph_db {

// Interned name ids: one dense id space each, allocated by the Schema and never
// recycled (a freed label/property-type leaves its id dead forever, so a stale
// id can never silently address a new name). 0 is "no id".
using LabelId = uint32_t;
using PropNameId = uint32_t;
inline constexpr LabelId kNoLabel = 0;
inline constexpr PropNameId kNoPropName = 0;

// ---------------------------------------------------------------------------
// GDI type vocabulary (values pinned to Semevia.Gdi)
// ---------------------------------------------------------------------------

// GDI_EDGE_DIRECTED-class datatypes (gdi_datatype.h). Only the ones the property
// model can actually carry are meaningful here; the rest exist so a declaration
// round-trips verbatim.
enum class GdiDataType : uint8_t {
    char_ = 100,
    int8_ = 101,
    uint8_ = 102,
    boolean = 103,
    byte_ = 104,
    int16_ = 105,
    uint16_ = 106,
    int32_ = 107,
    uint32_ = 108,
    float_ = 109,
    date = 110,
    time = 111,
    int64_ = 112,
    uint64_ = 113,
    double_ = 114,
    datetime = 115,
    decimal = 116,
};

// Property container type (GDI "etype"): one value, or a fixed-length array.
enum class GdiContainerType : uint8_t {
    single = 0,
    array = 1,
};

// Comparison operations of a constraint condition (gdi_operation.h:
// GDI_EQUAL=0 ... GDI_EQSMALLER=5, plus GDI_ANY_IN=6 added in ABI 7).
enum class GdiOp : uint8_t {
    equal = 0,
    not_equal = 1,
    greater = 2,
    greater_or_equal = 3,
    less = 4,
    less_or_equal = 5,
    // Member-of-set: is the property value — a scalar, or ANY element of an array
    // value — one of the set carried as the condition's value (a string_array or an
    // int64_array; see any_in_matches and plan §1.2). A membership test, never an
    // ordering, so it is not comparable and takes no part in compare_values().
    any_in = 6,
};

[[nodiscard]] KIMIX_GRAPH_DB_API char const *op_name(GdiOp op) noexcept;
[[nodiscard]] KIMIX_GRAPH_DB_API char const *data_type_name(GdiDataType type) noexcept;
[[nodiscard]] KIMIX_GRAPH_DB_API char const *container_type_name(GdiContainerType type) noexcept;

// ---------------------------------------------------------------------------
// PropertyValue
// ---------------------------------------------------------------------------

// Which payload a PropertyValue carries — Semevia.Gdi.PropertyValue.ValueKind.
enum class PropertyKind : uint8_t {
    none = 0,
    int64_value = 1,
    double_value = 2,
    string = 3,
    blob = 4,
    int64_array = 5,
    double_array = 6,
    string_array = 7,
};

[[nodiscard]] KIMIX_GRAPH_DB_API char const *property_kind_name(PropertyKind kind) noexcept;

// Field widths of the string_array wire block (see the file header): the
// leading element count and each element's byte length.
inline constexpr size_t kStringArrayCountSize = 4;
inline constexpr size_t kStringArrayLengthSize = 4;

/*
 * A non-owning cursor over a string_array value's elements — the §1.1 block read
 * back as string_views, with NO allocation and no copy. Every operation is
 * total: a default-constructed view (or one built from a malformed block) is
 * empty, and a walk stops at the block's own end, so a hostile block can never
 * be read past its last byte.
 *
 *   for (kimix::string_view element : value.as_string_array()) { ... }
 */
class KIMIX_GRAPH_DB_API StringArrayView {
public:
    // Forward-only input iterator over the elements, in order. `_cursor` is the
    // length-field position of the element `operator*` yields and `_next` the
    // following element's; end() is the block's byte end, so the walk yields the
    // LAST element before it stops, and a truncated tail stops there instead of
    // reading on.
    class KIMIX_GRAPH_DB_API iterator {
    public:
        iterator() noexcept = default;
        iterator(std::byte const *cursor, std::byte const *end) noexcept;

        [[nodiscard]] kimix::string_view operator*() const noexcept { return _current; }
        iterator &operator++() noexcept;
        [[nodiscard]] friend bool operator==(iterator const &lhs, iterator const &rhs) noexcept {
            return lhs._cursor == rhs._cursor;
        }
        [[nodiscard]] friend bool operator!=(iterator const &lhs, iterator const &rhs) noexcept {
            return lhs._cursor != rhs._cursor;
        }

    private:
        // Decodes the element at `_cursor` into `_current` and sets `_next` to the
        // following element's length field. A length field or payload that does not
        // fit parks the iterator at the block's end (the walk is over) with an empty
        // element — the one bounds rule of read_string_array_element().
        void _decode() noexcept;

        std::byte const *_cursor{nullptr};
        std::byte const *_next{nullptr};
        std::byte const *_end{nullptr};
        kimix::string_view _current;
    };

    StringArrayView() noexcept = default;
    // Borrows `bytes` of the §1.1 block at `data`. The block is NOT re-validated
    // here: PropertyValue hands out blocks it built (or accepted) itself, and
    // string_array_block_validate() is the check for a foreign block. Either way
    // accessors stay inside [data, data + bytes) and out-of-range reads are empty.
    StringArrayView(std::byte const *data, size_t bytes) noexcept;

    // The block's own element count (0 when the block is absent or too short to
    // carry even the count field).
    [[nodiscard]] uint32_t element_count() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return element_count() == 0; }

    // The `index`-th element, or an empty view when `index` is out of range. This
    // is a LINEAR walk from the first element: use the range-for iterator to visit
    // every element once.
    [[nodiscard]] kimix::string_view element(uint32_t index) const noexcept;

    [[nodiscard]] iterator begin() const noexcept;
    [[nodiscard]] iterator end() const noexcept;

    // The borrowed block verbatim (empty for a default-constructed view). This is
    // the byte image a payload is encoded from.
    [[nodiscard]] kimix::span<const std::byte> block() const noexcept {
        return kimix::span<const std::byte>{_data, _bytes};
    }

private:
    std::byte const *_data{nullptr};
    size_t _bytes{0};
};

// True when `block` is a well-formed §1.1 string-array block, reporting its
// element count in `out_count`: the count field is present, every element's
// length field and bytes fit inside the block, and the walk consumes it exactly
// (an empty array is the 4-byte block [u32 0] and nothing else). The ONE
// validator of this layout — the record decoder and the C ABI write path both
// call it, so a malformed block is refused in exactly one place per side.
[[nodiscard]] KIMIX_GRAPH_DB_API bool
string_array_block_validate(kimix::span<const std::byte> block, uint32_t &out_count) noexcept;

/*
 * One typed value. Value semantics: a copy owns its payloads, so an entity may
 * hand out copies freely. Every accessor is total — asking for the wrong kind
 * returns a zeroed/empty value instead of asserting, because the C ABI must
 * never abort on a caller mistake (the Database validates kinds and returns
 * error_wrong_type long before an accessor is reached; a short-circuit `as_*`
 * read of a mismatched value is a bug in this library, not a trap for the
 * adapter).
 */
class KIMIX_GRAPH_DB_API PropertyValue {
public:
    PropertyValue() noexcept = default;

    static PropertyValue make_int64(int64_t value) noexcept;
    static PropertyValue make_double(double value) noexcept;
    static PropertyValue make_string(kimix::string_view value);
    static PropertyValue make_blob(kimix::span<const std::byte> value);
    static PropertyValue make_int64_array(kimix::span<const int64_t> value);
    static PropertyValue make_double_array(kimix::span<const double> value);

    // A string array: the elements are copied into ONE heap block in the §1.1
    // wire layout (count + per-element length + UTF-8 bytes), so the value's byte
    // image is its wire form and no per-element allocation happens on read.
    // `elements` may be empty (an empty array is a VALUE: a 4-byte block holding a
    // zero count). The input is taken as a span of borrowed views — the caller owns
    // the text, this copies it — which a brace-init list, a kimix::vector of
    // views, or a stack array all convert to.
    //
    // Returns a `none` value (never an abort) when the input cannot be spelled in
    // the §1.1 block: more than UINT32_MAX elements, or an element longer than
    // UINT32_MAX bytes. Callers that must distinguish "too big" from "empty" check
    // is_none() after the call.
    static PropertyValue make_string_array(kimix::span<const kimix::string_view> elements);

    // A string_array value adopted from a COPY of `block`, which must be a
    // well-formed §1.1 block: `string_array_block_validate()` decides, and a block
    // it rejects yields a `none` value. This is the decoder's entry point (the
    // record codec and the C ABI's read path).
    static PropertyValue make_string_array_from_block(kimix::span<const std::byte> block) noexcept;

    [[nodiscard]] PropertyKind kind() const noexcept { return _kind; }
    [[nodiscard]] bool is_none() const noexcept { return _kind == PropertyKind::none; }

  [[nodiscard]] int64_t as_int64() const noexcept {
      return _kind == PropertyKind::int64_value ? _payload.integer : 0;
  }
  [[nodiscard]] double as_double() const noexcept {
      return _kind == PropertyKind::double_value ? _payload.real : 0.0;
  }
    // UTF-8 text of a string value (empty for any other kind). Valid until this
    // value is modified or destroyed.
    [[nodiscard]] kimix::string_view as_string() const noexcept;
    [[nodiscard]] kimix::span<const std::byte> as_blob() const noexcept;
    [[nodiscard]] kimix::span<const int64_t> as_int64_array() const noexcept;
    [[nodiscard]] kimix::span<const double> as_double_array() const noexcept;
    // The elements of a string_array value as a zero-allocation cursor view; an
    // EMPTY view for any other kind. Valid until this value is modified or
    // destroyed.
    [[nodiscard]] StringArrayView as_string_array() const noexcept;
    // The string_array value's §1.1 block verbatim (empty for any other kind) —
    // what the record codec and the C ABI hand out as the payload.
    [[nodiscard]] kimix::span<const std::byte> as_string_array_block() const noexcept;

    // Number of elements of an array value (a string_array's own count field, not
    // its byte total), 0 for a scalar.
    [[nodiscard]] size_t element_count() const noexcept;

    // Value equality — the C# reference's Equals, including the array kinds.
    [[nodiscard]] bool equals(PropertyValue const &other) const noexcept;    // Rule of five (plan v3 §5.4): the value owns its variable-size payload
    // through the tagged union below, so copying and destruction manage the
    // heap block explicitly.
    PropertyValue(PropertyValue const &other);
    PropertyValue &operator=(PropertyValue const &other);
    PropertyValue(PropertyValue &&other) noexcept;
    PropertyValue &operator=(PropertyValue &&other) noexcept;
    ~PropertyValue();

    // The GDI datatype this value's payload corresponds to (int64 -> Int64,
    // double_value -> Double, string -> Char, blob -> Byte,
    // int64_array -> Int64, double_array -> Double, string_array -> Char). Used
    // by the schema's declaration check (error_wrong_type): one char_ declaration
    // serves String and StringArray, distinguished by `container` — exactly how
    // Int64 serves int64_value and int64_array.
    [[nodiscard]] GdiDataType data_type() const noexcept;

    // True when this value satisfies a PropertyTypeInfo's container/dtype/count
    // declaration. A string_array needs container == array, data == char_ and a
    // count equal to its element count — the int64_array arm exactly, with the
    // element count (not the byte total) as the only array measure.
    [[nodiscard]] bool matches_declaration(GdiContainerType container, GdiDataType data,
                                           uint32_t count) const noexcept;    void clear() noexcept;
private:
    // Frees any owned payload block and resets to `none`.
    void _release() noexcept;
    // Deep-copies `other`'s payload into this (already released) value.
    void _clone_from(PropertyValue const &other);    // PLAN V3 §5.4: the payload is ONE tagged union (24 B total) instead of
    // three kimix::vectors (~96 B, nearly all of it dead weight for the scalar
    // kinds that dominate real bags). The variable-size kinds own a single
    // heap block; `count` is in ELEMENTS, not bytes.
    PropertyKind _kind{PropertyKind::none};
    union Payload {
        int64_t integer;
        double real;
        struct {
            std::byte *data;
            size_t count;
        } bytes; // string (UTF-8, no NUL) or blob; `_kind` disambiguates
        struct {
            int64_t *data;
            size_t count;
        } int64s;
        struct {
            double *data;
            size_t count;
        } doubles;
        struct {
            std::byte *data;
            size_t bytes; // whole §1.1 block; the element count is read FROM the block
        } strings; // string_array — one source of truth: there is no second count
        Payload() noexcept : integer(0) {}
        ~Payload() {}
    } _payload;
};

// Result of compare_values(): negative / zero / positive ordering.
[[nodiscard]] KIMIX_GRAPH_DB_API kimix::optional<int32_t>
compare_values(PropertyValue const &lhs, PropertyValue const &rhs) noexcept;

// The ANY_IN membership semantics (plan §1.2), spelled once:
//   - `set_value` is the SET: a string_array or an int64_array value. Any other
//     kind — and an empty set — can never be satisfied, and is FALSE, not an error;
//   - a string `actual` is satisfied iff its bytes are one of the set's elements,
//     an int64 `actual` iff its value is one of the set's elements (the set's kind
//     decides which of the two is probeable at all);
//   - a string_array / int64_array `actual` is satisfied iff ANY of its elements is
//     in the set;
//   - every other `actual` kind (double, blob, double_array, none) is false.
// This is the DEFINITION of the answer, computed with a linear walk of the set. The
// per-edge hot path does not use it: ConstraintCondition (schema.h) builds a sorted
// probe index from the same rule once at condition-add time and binary-searches it,
// so a filter never pays an O(set) scan per edge. The two must agree exactly —
// ConstraintCondition::matches_value falls back HERE when it holds no index.
[[nodiscard]] KIMIX_GRAPH_DB_API bool
any_in_matches(PropertyValue const &set_value, PropertyValue const &actual) noexcept;

// Evaluates one property condition: `actual <op> expected`, reproducing
// Semevia.Gdi's InMemoryStore.Compare to the letter:
//   - numeric kinds compare across int64/double (an int64 is widened);
//   - two strings compare ordinally (byte-wise here, which is the same order for
//     every UTF-8 sequence, and therefore for every ASCII name);
//   - everything else (blob, arrays, mixed kinds) supports equality ONLY, and
//     any ordering op on them is false rather than an error;
//   - `any_in` is the membership test above: `expected` is the SET and `actual` the
//     property value, so it answers any_in_matches(expected, actual) and takes no
//     part in the ordering rules.
[[nodiscard]] KIMIX_GRAPH_DB_API bool
evaluate(GdiOp op, PropertyValue const &actual, PropertyValue const &expected) noexcept;

// ---------------------------------------------------------------------------
// Bags and lists
// ---------------------------------------------------------------------------

// Interned-name -> value. kimix::unordered_map = ankerl::unordered_dense, so a
// small bag is one flat array, and iteration order is insertion order (the tests
// sort before comparing, but an unfiltered read-back therefore stays stable).
using PropertyBag = kimix::unordered_map<PropNameId, PropertyValue>;

// One (id, value) pair of a read-back. The Database turns these into
// name/value pairs through the Schema.
struct KIMIX_GRAPH_DB_API PropertyEntry {
    PropNameId name{kNoPropName};
    PropertyValue value;
};
using PropertyList = kimix::vector<PropertyEntry>;

// A property type's declaration: GDI_CreatePropertyType's name/etype/dtype/count
// triple. `count` is the array length for an array declaration and 1 for a
// single value (GDI requires >= 1).
struct KIMIX_GRAPH_DB_API PropertyTypeInfo {
    PropNameId name_id{kNoPropName};
    kimix::string name;
    GdiContainerType container{GdiContainerType::single};
    GdiDataType data{GdiDataType::int64_};
    uint32_t count{1};
};
using PropertyTypeList = kimix::vector<PropertyTypeInfo>;

// ---------------------------------------------------------------------------
// Record-level property codec (shared by the vertex and edge records)
// ---------------------------------------------------------------------------

/*
 * One encoded property entry, little-endian with no padding:
 *
 *   [4B name id][1B kind][1B dtype][2B reserved][4B count][8B payload bytes][payload]
 *
 * `count` is the element count of an array value and 1 for every scalar (a
 * string's BYTE length is the payload length, not the count), so a decoder can
 * size the payload before reading it. `dtype` is the value's GDI datatype, kept
 * so a record read back without a schema still describes itself.
 *
 * The payload of a string_array is its §1.1 wire block verbatim — no second
 * framing, no re-encoding — with `count` equal to the block's own leading u32.
 * The record layout itself is ABI-INDEPENDENT: ABI 7 changed no field width and
 * no order here (the count field has always been a 4-byte u32), so an ABI-6
 * record reads identically under ABI 7 and the new kind simply occupies a new
 * `kind` value.
 *
 * encoded_property_entry_size() is what a record encoder sizes its buffer with,
 * so the layout has exactly one implementation.
 */
[[nodiscard]] KIMIX_GRAPH_DB_API size_t
encoded_property_entry_size(PropertyEntry const &entry) noexcept;
KIMIX_GRAPH_DB_API void encode_property_entry(std::byte *dst,
                                              PropertyEntry const &entry) noexcept;
// Partial decode: returns false (leaving `out` default-constructed) when the
// entry does not fit, and never reads outside [cursor, end). A string_array
// payload is additionally validated with string_array_block_validate() and its
// count cross-checked against the record's own `count` field, so a corrupt block
// fails the record read (the caller's error_bad_file path) instead of adopting a
// value that would read past its payload.
[[nodiscard]] KIMIX_GRAPH_DB_API bool
decode_property_entry(std::byte const *&cursor, std::byte const *end,
                      PropertyEntry &out) noexcept;

} // namespace kimix::graph_db
