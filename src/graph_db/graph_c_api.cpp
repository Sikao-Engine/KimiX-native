/*
 * graph_c_api.cpp — the C ABI over kimix::graph_db::Database (see graph_c_api.h
 * for the contract).
 *
 * STRUCTURE
 *   `struct kimix_gdb_session` wraps ONE Database plus the two pieces of state the
 *   C side cannot express: the constraint handles the caller refers to by id (a
 *   Constraint is a C++ value it cannot hold) and the session's open flag.
 *   Everything else is a direct forwarding call, with the three conversions the
 *   ABI needs:
 *
 *     guid        kimix_gdb_guid {lo, hi}   <->  kimix::Guid {data0, data1}
 *     bytes       (uint8_t*, len)           <->  kimix::span<const std::byte>
 *     property    kimix_gdb_property        <->  kimix::graph_db::PropertyValue
 *
 *   The two-phase read protocol is implemented by ONE helper (append into a
 *   caller scratch buffer) so "buffer too small" behaves identically everywhere:
 *   the call returns KIMIX_GDB_ERR_BUFFER and still reports the sizes needed.
 *
 *   PROPERTY VALUES (ABI 7, extended by ABI 9). to_value() is the ONE place a
 *   caller's kimix_gdb_property is decoded, and the ONE place a
 *   KIMIX_GDB_KIND_STRING_ARRAY / KIMIX_GDB_KIND_DATA_ARRAY payload is checked
 *   against its block layout (well-formedness plus the exact-count cross-check);
 *   every entry point that STORES a value — vertex/edge add/set/update, the create
 *   and bulk spec flows, a constraint's any_in set — refuses a bad block through
 *   malformed_status() and stores nothing: KIMIX_GDB_ERR_COUNT for a count-shaped
 *   problem, KIMIX_GDB_ERR_DATATYPE for an illegal data-array element flag.
 *   payload_of() hands the same block back verbatim, in both directions.
 *
 * NO EXCEPTIONS, NO RTTI, NO C++ TYPES AT THE BOUNDARY. A failure is a status
 * code and an out-parameter left untouched.
 */
#include "graph_c_api.h"

#include "database.h"
#include "graph_db.h"
#include "status.h"
#include "transaction.h"

#include <core/guid.h>
#include <core/md5.h>
#include <core/stl/map.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>

/*
 * A session-local constraint: the ABI hands out a dense id and keeps the
 * definition beside it, because a Constraint is a C++ value the caller cannot hold.
 */
struct ConstraintSlot {
    kimix::graph_db::ConstraintHandle handle;
    kimix::graph_db::Constraint definition;
};

/* The opaque session (the C side only ever holds a pointer to it). */
struct kimix_gdb_session {
    kimix::graph_db::Database database;
    kimix::vector<ConstraintSlot> constraints;
    uint32_t next_constraint{1};
    bool open{false};
    // The catalog name this session registered under ("" = anonymous; plan v3
    // step 3.7).  Copied at open; close() releases the catalog entry.
    kimix::string graph_name;
    // Plan O12: the edge-filtered shortest path's staging buffer, ONE per session.
    // kimix_gdb_shortest_path_edges used to build a fresh kimix::vector<PathStep>
    // per call and then copy it into the caller's array, so the two-phase protocol
    // (a count-only probe, then a fill) paid that allocation twice per path. The
    // walk fills `out` in place and keeps its capacity (plan O8), so keeping the
    // buffer here means the probe leaves it ready and the fill reuses it: from the
    // second call on, neither phase allocates for the path at all.
    // Session-local by the same rule as the constraint table above — the ABI's
    // threading contract is one session per worker, single-threaded per session
    // (graph_c_api.h, contract point 5), so no other thread can observe it
    // mid-call. Purely internal: the struct is opaque, so this adds nothing to the
    // ABI (KIMIX_GDB_ABI_VERSION is unchanged).
    kimix::vector<kimix::graph_db::Graph::PathStep> path_staging;
    // ABI 7 (plan O12 again, for the edge-filtered BFS): the SAME staging idiom for
    // kimix_gdb_traverse_bfs_edges' THREE outputs, ONE record per session. The walk
    // clears both vectors and refills them in place (Database::traverse_bfs_edges), so
    // the count-only probe leaves them sized for the neighbourhood it just measured and
    // the fill phase of the same two-phase call reuses that capacity instead of
    // allocating for it: from the second call on, neither phase allocates for the
    // result at all. Session-local by the same rule as `path_staging` above — one
    // session per worker, single-threaded per session (graph_c_api.h, contract point 5)
    // — and purely internal: the struct is opaque, so this adds nothing to the ABI.
      kimix::graph_db::Graph::BfsEdgesResult bfs_staging;
      // ABI 8: kimix_gdb_traverse_bfs_edges' start vertices, converted from the
      // caller's kimix_gdb_guid array into kimix::Guid here and kept for the same
      // reason as the two staging buffers above -- a repeated multi-start call then
      // converts into capacity it already has instead of allocating per phase.
      // Session-local by the same one-session-per-worker rule; the struct is opaque,
      // so this adds nothing to the ABI.
      kimix::vector<kimix::Guid> start_staging;
  };

namespace src_graph_db_graph_c_api {

// The named-graph catalog: the LIVE named graphs of this process, one entry
// per name (a second live open is ERR_NAME_EXISTS — the one-session-per-worker
// contract, named).  Anonymous sessions never appear.  Guarded by its own
// mutex: opens and closes may arrive from any thread.
std::mutex g_catalog_mutex;
kimix::map<kimix::string, kimix_gdb_session *> g_catalog;

using kimix::graph_db::AggregateResult;
using kimix::graph_db::AggregateSpec;
using kimix::graph_db::Constraint;
using kimix::graph_db::ConstraintHandle;
using kimix::graph_db::Database;
using kimix::graph_db::GdiAggregate;
using kimix::graph_db::EdgeDirectionType;
using kimix::graph_db::EdgeOrientation;
using kimix::graph_db::EdgeRecord;
using kimix::graph_db::GdiOp;
using kimix::graph_db::GdiStatus;
using kimix::graph_db::LabelId;
using kimix::graph_db::PropNameId;
using kimix::graph_db::PropertyKind;
using kimix::graph_db::PropertyValue;
using kimix::graph_db::VertexKeyView;


inline int32_t code_of(GdiStatus status) noexcept {
    return static_cast<int32_t>(status);
}

inline kimix::Guid to_guid(kimix_gdb_guid value) noexcept {
    return kimix::Guid{kimix::Guid::GuidData{value.lo, value.hi}};
}

  inline kimix_gdb_guid from_guid(kimix::Guid const &value) noexcept {
      kimix_gdb_guid out;
      out.lo = value.to_binary().data0;
      out.hi = value.to_binary().data1;
      return out;
  }

  // ABI 8: the "no uid chosen" spelling on the input side. An all-zero guid is
  // what a zero-initialized kimix_gdb_edge_spec carries, so treating it (like a
  // NULL pointer) as "generate one" keeps ABI-7 caller code working unchanged.
  inline bool is_zero_guid(kimix::Guid const &value) noexcept {
      auto const &words = value.to_binary();
      return words.data0 == 0 && words.data1 == 0;
  }

inline kimix::span<const std::byte> to_bytes(kimix_gdb_bytes value) noexcept {
    if (value.data == nullptr || value.len == 0) {
        return {};
    }
    return kimix::span<const std::byte>{reinterpret_cast<std::byte const *>(value.data),
                                        value.len};
}

// Why a property value could not be decoded from the ABI struct. ONE status per
// cause (no silent coercion): a malformed block or a `count` that disagrees with
// the block it describes is KIMIX_GDB_ERR_COUNT (the ABI-7 string-array rule), and
// an unsupported / reserved element flag is KIMIX_GDB_ERR_DATATYPE (ABI 9, D6).
enum class DecodeRefusal : uint8_t {
    ok = 0,
    bad_count = 1,
    bad_kind = 2,
};

// The single mapping from a decode refusal to the status a write path returns.
[[nodiscard]] inline int32_t refusal_status(DecodeRefusal refusal) noexcept {
    switch (refusal) {
    case DecodeRefusal::ok: return KIMIX_GDB_OK;
    case DecodeRefusal::bad_count: return KIMIX_GDB_ERR_COUNT;
    case DecodeRefusal::bad_kind: return KIMIX_GDB_ERR_DATATYPE;
    }
    return KIMIX_GDB_ERR_COUNT;
}

// One property value decoded from the ABI struct. The array kinds read their
// elements from `payload` (little-endian, 8 bytes each); KIND_STRING_ARRAY carries
// the §1.1 block verbatim and KIND_DATA_ARRAY its own self-describing block (ABI 9).
//
// `out_refusal` is the ONE write-path validation signal for the two block kinds:
// it is set when the payload is not a well-formed block, when the caller's `count`
// disagrees with the count the block itself declares (bad_count), or when a
// data-array block names an element kind that does not exist (bad_kind). A refused
// value decodes to `none` and every property WRITE entry point refuses it with the
// status refusal_status() maps that refusal to, storing nothing — so nothing
// reaches the Database. A caller that only reads (there is none today: every
// to_value use is a write) may pass nullptr and get the `none` value alone.
PropertyValue to_value(kimix_gdb_property const &input,
                       DecodeRefusal *out_refusal = nullptr) {
    if (out_refusal != nullptr) {
        *out_refusal = DecodeRefusal::ok;
    }
    switch (input.kind) {
    case KIMIX_GDB_KIND_INT64:
        return PropertyValue::make_int64(input.i64);
    case KIMIX_GDB_KIND_DOUBLE:
        return PropertyValue::make_double(input.f64);
    case KIMIX_GDB_KIND_STRING:
        return PropertyValue::make_string(kimix::string_view{
            reinterpret_cast<char const *>(input.payload.data), input.payload.len});
    case KIMIX_GDB_KIND_BLOB:
        return PropertyValue::make_blob(to_bytes(input.payload));
    case KIMIX_GDB_KIND_INT64_ARRAY: {
        kimix::vector<int64_t> values;
        size_t const count = input.payload.len / 8;
        values.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            int64_t element = 0;
            std::memcpy(&element, input.payload.data + i * 8, sizeof(element));
            values.push_back(element);
        }
        return PropertyValue::make_int64_array(
            kimix::span<const int64_t>{values.data(), values.size()});
    }
    case KIMIX_GDB_KIND_DOUBLE_ARRAY: {
        kimix::vector<double> values;
        size_t const count = input.payload.len / 8;
        values.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            double element = 0.0;
            std::memcpy(&element, input.payload.data + i * 8, sizeof(element));
            values.push_back(element);
        }
        return PropertyValue::make_double_array(
            kimix::span<const double>{values.data(), values.size()});
    }
    case KIMIX_GDB_KIND_STRING_ARRAY: {
        // The payload IS the wire block, so decoding is a validate-and-adopt, never
        // a re-framing (one encoder, one decoder, plan §1.1). string_array_block_validate
        // is that library-side gate; the count cross-check is the ABI-side half of the
        // same rule: `count` is the EXACT element count (§1.4), so a block that
        // declares a different number is inconsistent input and must not be stored.
        kimix::span<const std::byte> const block = to_bytes(input.payload);
        uint32_t block_count = 0;
        if (!kimix::graph_db::string_array_block_validate(block, block_count) ||
            input.count != block_count) {
            if (out_refusal != nullptr) {
                *out_refusal = DecodeRefusal::bad_count;
            }
            return PropertyValue{};
        }
        return PropertyValue::make_string_array_from_block(block);
    }
    case KIMIX_GDB_KIND_DATA_ARRAY: {
        // ABI 9: the same validate-and-adopt shape, with ONE extra cause. A block whose
        // element flag names nothing (0xFF included) is a DATATYPE refusal, while every
        // other malformation — and a `count` that disagrees with the block — stays a
        // COUNT refusal, so the managed side can report a useful error. The value is
        // adopted only when the block is well-formed AND the count agrees, so no
        // inconsistent data array is ever stored.
        kimix::span<const std::byte> const block = to_bytes(input.payload);
        PropertyKind element_kind = PropertyKind::none;
        uint32_t block_count = 0;
        if (!kimix::graph_db::data_array_block_validate(block, element_kind, block_count)) {
            bool const bad_flag =
                block.size() >= kimix::graph_db::kDataArrayHeaderSize &&
                !kimix::graph_db::is_data_array_element_kind(
                    static_cast<PropertyKind>(std::to_integer<uint8_t>(block[0])));
            if (out_refusal != nullptr) {
                *out_refusal = bad_flag ? DecodeRefusal::bad_kind : DecodeRefusal::bad_count;
            }
            return PropertyValue{};
        }
        if (input.count != block_count) {
            if (out_refusal != nullptr) {
                *out_refusal = DecodeRefusal::bad_count;
            }
            return PropertyValue{};
        }
        return PropertyValue::make_data_array_from_block(block);
    }
    default:
        return PropertyValue{};
    }
}

// The write-path guard, implemented ONCE and used by every entry point that STORES
// a property value (vertex/edge add-set-update, the create and bulk spec flows, and
// a constraint's any_in set): a malformed block is refused before the Database sees
// it — KIMIX_GDB_ERR_COUNT for a count-shaped problem, KIMIX_GDB_ERR_DATATYPE for an
// illegal data-array element flag.
[[nodiscard]] inline int32_t malformed_status(DecodeRefusal refusal) noexcept {
    return refusal_status(refusal);
}

// A scratch writer: one caller-owned buffer, append-only, overflow flag instead of
// an exception. Every variable-length read uses it. `needed()` tallies EVERY
// append attempt, so a call that overflowed still reports the buffer size the
// caller must provide (the contract: ERR_BUFFER reports the required sizes).
class ScratchWriter {
public:
    ScratchWriter(uint8_t *buffer, size_t capacity) noexcept
        : _buffer(buffer), _capacity(capacity) {
    }

    bool append(kimix::span<const std::byte> bytes, uint32_t &offset, uint32_t &length) noexcept {
        _needed += bytes.size();
        if (_used + bytes.size() > _capacity) {
            _overflow = true;
            offset = 0;
            length = 0;
            return false;
        }
        offset = static_cast<uint32_t>(_used);
        length = static_cast<uint32_t>(bytes.size());
        if (!bytes.empty() && _buffer != nullptr) {
            std::memcpy(_buffer + _used, bytes.data(), bytes.size());
        }
        _used += bytes.size();
        return true;
    }

    bool append_text(kimix::string_view text, uint32_t &offset, uint32_t &length) noexcept {
        return append(kimix::span<const std::byte>{
                          reinterpret_cast<std::byte const *>(text.data()), text.size()},
                      offset, length);
    }

    [[nodiscard]] bool overflowed() const noexcept { return _overflow; }
    [[nodiscard]] size_t used() const noexcept { return _used; }
    // The buffer size that fits every append attempt, successful or not: what a
    // caller must allocate after an ERR_BUFFER, and what the probe phase reports.
    [[nodiscard]] size_t needed() const noexcept { return _needed; }

private:
    uint8_t *_buffer{nullptr};
    size_t _capacity{0};
    size_t _used{0};
    size_t _needed{0};
    bool _overflow{false};
};

// The payload bytes a read copies into the scratch buffer for one value.
kimix::span<const std::byte> payload_of(PropertyValue const &value) noexcept {
    switch (value.kind()) {
    case PropertyKind::string: {
        auto const text = value.as_string();
        return kimix::span<const std::byte>{
            reinterpret_cast<std::byte const *>(text.data()), text.size()};
    }
    case PropertyKind::blob:
        return value.as_blob();
    case PropertyKind::int64_array: {
        auto const ints = value.as_int64_array();
        return kimix::span<const std::byte>{
            reinterpret_cast<std::byte const *>(ints.data()), ints.size() * 8};
    }
    case PropertyKind::double_array: {
        auto const reals = value.as_double_array();
        return kimix::span<const std::byte>{
            reinterpret_cast<std::byte const *>(reals.data()), reals.size() * 8};
    }
    case PropertyKind::string_array:
        // The value's own §1.1 block, verbatim: the scratch carries the wire image,
        // so a reader gets back exactly the bytes a writer handed in (no per-element
        // re-encoding on the read path).
        return value.as_string_array_block();
    case PropertyKind::data_array:
        // ABI 9: the data-array block verbatim, for the same reason — the element flag,
        // the count and the elements are already the wire image, so the read path is a
        // memcpy of the block and needs no per-element kind switch.
        return value.as_data_array_block();
    default:
        return {};
    }
}

inline void fill_property_record(kimix_gdb_property_record &record, PropNameId name,
                                 PropertyValue const &value, uint32_t offset,
                                 uint32_t length) noexcept {
    record.name_id = name;
    record.kind = static_cast<uint8_t>(value.kind());
    record.dtype = static_cast<uint8_t>(value.data_type());
    size_t const elements = value.element_count();
    // NOTE (ABI 9): an EMPTY array — a data array with count 0 included — reports 1
    // here, the long-standing "a value exists" spelling. The element count of a block
    // kind is the BLOCK's own u32 (0 for an empty array), never this field.
    record.count = static_cast<uint32_t>(elements == 0 ? 1 : elements);
    record.i64 = value.as_int64();
    record.f64 = value.as_double();
    record.payload_offset = offset;
    record.payload_len = length;
}

// Write a (name, value) list into the caller's arrays. Reports the FULL count and
// the scratch bytes needed even when the buffers are too small.
GdiStatus write_properties(kimix::vector<std::pair<PropNameId, PropertyValue>> const &properties,
                           kimix_gdb_property_record *items, size_t capacity,
                           uint8_t *scratch_buffer, size_t scratch_capacity,
                           size_t *out_count, size_t *out_scratch) {
    // PHASE 1 (both buffers NULL): report the sizes and succeed, so a caller can
    // learn what it needs without a second guess.
    bool const probe = items == nullptr && scratch_buffer == nullptr;
    if (probe) {
        size_t needed = 0;
        for (auto const &property : properties) {
            needed += payload_of(property.second).size();
        }
        if (out_count != nullptr) {
            *out_count = properties.size();
        }
        if (out_scratch != nullptr) {
            *out_scratch = needed;
        }
        return GdiStatus::ok;
    }
    ScratchWriter scratch{scratch_buffer, scratch_capacity};
    for (size_t i = 0; i < properties.size(); ++i) {
        uint32_t offset = 0;
        uint32_t length = 0;
        bool const fits = scratch.append(payload_of(properties[i].second), offset, length);
        if (items != nullptr && i < capacity) {
            fill_property_record(items[i], properties[i].first, properties[i].second, offset,
                                length);
        }
        (void)fits;
    }
    if (out_count != nullptr) {
        *out_count = properties.size();
    }
      if (out_scratch != nullptr) {
          *out_scratch = scratch.needed();
      }
      if (scratch.overflowed()) {
          return GdiStatus::error_buffer;
      }
      if (properties.size() > capacity) {
          return GdiStatus::error_buffer;
      }
      return GdiStatus::ok;
  }

ConstraintSlot *find_slot(kimix_gdb_session *session, uint32_t id) {
    for (auto &slot : session->constraints) {
        if (slot.handle.id == id) {
            return &slot;
        }
    }
    return nullptr;
}

GdiStatus resolve_constraint(kimix_gdb_session *session, uint32_t id, Constraint const *&out) {
    // Plan O9: hand back the SESSION's definition BY REFERENCE instead of copying
    // it into a per-call Constraint. A handle was resolved once, at
    // kimix_gdb_constraint_create, so the ABI path has no constraint work left to
    // do and no longer allocates for it — which used to be paid on top of the walk
    // it feeds, twice per two-phase read. The rules are unchanged: id 0 is "no
    // constraint" (match everything), an unknown id is error_constraint, and `out`
    // is written only on success. The slot lives in the session's table and is not
    // mutated between this call and the end of the command (create/free only touch
    // it inside their own calls), so the pointer stays valid for the call; a C ABI
    // session is single-threaded by contract (graph_c_api.h, threading), so no
    // other thread can invalidate it either.
    static Constraint const kEmptyConstraint{};
    if (id == 0) {
        out = &kEmptyConstraint; // an id of 0 means "no constraint": match everything
        return GdiStatus::ok;
    }
    ConstraintSlot *slot = find_slot(session, id);
    if (slot == nullptr) {
        return GdiStatus::error_constraint;
    }
    out = &slot->definition;
    return GdiStatus::ok;
}

  // The property list of one vertex in the ABI's (name, value) shape.
  void collect_vertex_properties(Database &db, kimix::Guid const &uid,
                                 kimix::vector<std::pair<PropNameId, PropertyValue>> &out) {
      out.clear();
      kimix::graph_db::Vertex *vertex = db.graph().find(uid);
      if (vertex == nullptr) {
          return;
      }
      kimix::graph_db::PropertyList properties;
      vertex->properties(properties);
      out.reserve(properties.size());
      for (auto const &entry : properties) {
          out.push_back({entry.name, entry.value});
      }
  }

  // Plan v3 §4.3: the one-pass record+scratch fill for the VERTEX property
  // read. Walks the bag once under the leaf lock via visit_properties, writing
  // each record and appending each payload straight into the caller's buffers.
  // The collect_vertex_properties + write_properties pair this replaces copied
  // every PropertyValue twice on the way through (a string/blob property: two
  // heap allocations) on top of the final payload copy. Same two-phase
  // contract and error semantics as write_properties: probe (both buffers
  // NULL) reports the sizes and succeeds; a too-small scratch or item
  // capacity yields ERR_BUFFER with the sizes still reported.
  GdiStatus write_vertex_property_records(kimix::graph_db::Vertex &vertex,
                                          kimix_gdb_property_record *items, size_t capacity,
                                          uint8_t *scratch_buffer, size_t scratch_capacity,
                                          size_t *out_count, size_t *out_scratch) {
      bool const probe = items == nullptr && scratch_buffer == nullptr;
      ScratchWriter scratch{scratch_buffer, scratch_capacity};
      size_t count = 0;
      vertex.visit_properties([&](PropNameId name, PropertyValue const &value) {
          uint32_t offset = 0;
          uint32_t length = 0;
          // In probe mode the writer has a null buffer and only accumulates
          // the needed byte count (its overflow flag is ignored below).
          bool const fits = scratch.append(payload_of(value), offset, length);
          (void)fits;
          if (!probe && items != nullptr && count < capacity) {
              fill_property_record(items[count], name, value, offset, length);
          }
          ++count;
          return true;
      });
      if (out_count != nullptr) {
          *out_count = count;
      }
      if (out_scratch != nullptr) {
          *out_scratch = scratch.needed();
      }
      if (probe) {
          return GdiStatus::ok;
      }
      if (scratch.overflowed()) {
          return GdiStatus::error_buffer;
      }
      if (count > capacity) {
          return GdiStatus::error_buffer;
      }
      return GdiStatus::ok;
  }

void collect_edge_properties(Database &db, kimix::Guid const &edge,
                             kimix::vector<std::pair<PropNameId, PropertyValue>> &out) {
    out.clear();
    kimix::graph_db::PropertyList properties;
    if (!db.graph().edge_properties(edge, properties)) {
        return;
    }
    out.reserve(properties.size());
    for (auto const &entry : properties) {
        out.push_back({entry.name, entry.value});
    }
}

// The (name, value) list of a vertex spec: `properties[i]` is named by
// `property_names[i]`. A missing names array is an argument error the callers
// report through the Database (an unnamed property is refused as
// error_property_type, which is the same status a caller would get for a bogus id).
// A NON-ZERO return is the refusal every property write shares (plan §1.1, ABI 9):
// one property carried a malformed block or an illegal data-array element flag, so
// `out` is left empty, the caller answers the mapped ERR_COUNT / ERR_DATATYPE, and
// nothing of the write is stored.
int32_t named_properties(kimix_gdb_vertex_spec const &spec,
                         kimix::graph_db::PropertyList &out) {
    out.clear();
    if (spec.properties == nullptr || spec.property_count == 0) {
        return KIMIX_GDB_OK;
    }
    out.reserve(spec.property_count);
    for (size_t i = 0; i < spec.property_count; ++i) {
        DecodeRefusal malformed = DecodeRefusal::ok;
        kimix::graph_db::PropertyEntry entry;
        entry.name = spec.property_names != nullptr ? spec.property_names[i] : 0;
        entry.value = to_value(spec.properties[i], &malformed);
        if (int32_t const refusal = malformed_status(malformed)) {
            out.clear(); // the whole write is refused: keep none of it
            return refusal;
        }
        out.push_back(std::move(entry));
    }
    return KIMIX_GDB_OK;
}

// The edge spec's twin, same contract.
int32_t named_properties(kimix_gdb_edge_spec const &spec,
                         kimix::graph_db::PropertyList &out) {
    out.clear();
    if (spec.properties == nullptr || spec.property_count == 0) {
        return KIMIX_GDB_OK;
    }
    out.reserve(spec.property_count);
    for (size_t i = 0; i < spec.property_count; ++i) {
        DecodeRefusal malformed = DecodeRefusal::ok;
        kimix::graph_db::PropertyEntry entry;
        entry.name = spec.property_names != nullptr ? spec.property_names[i] : 0;
        entry.value = to_value(spec.properties[i], &malformed);
        if (int32_t const refusal = malformed_status(malformed)) {
            out.clear(); // the whole write is refused: keep none of it
            return refusal;
        }
        out.push_back(std::move(entry));
    }
    return KIMIX_GDB_OK;
}

} // namespace src_graph_db_graph_c_api
using namespace src_graph_db_graph_c_api;

// ===========================================================================
// Library / ABI queries
// ===========================================================================

extern "C" {

uint32_t kimix_gdb_abi_version(void) {
    return KIMIX_GDB_ABI_VERSION;
}

const char *kimix_gdb_version_string(void) {
    return KIMIX_GRAPH_DB_VERSION;
}

int32_t kimix_gdb_layout_info(kimix_gdb_layout *out) {
    if (out == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    out->abi_version = KIMIX_GDB_ABI_VERSION;
    out->sizeof_property = static_cast<uint32_t>(sizeof(kimix_gdb_property));
    out->sizeof_property_record = static_cast<uint32_t>(sizeof(kimix_gdb_property_record));
    out->sizeof_vertex_key = static_cast<uint32_t>(sizeof(kimix_gdb_vertex_key));
    out->sizeof_edge_record = static_cast<uint32_t>(sizeof(kimix_gdb_edge_record));
    out->sizeof_property_type = static_cast<uint32_t>(sizeof(kimix_gdb_property_type));
    out->sizeof_capability = static_cast<uint32_t>(sizeof(kimix_gdb_capability));
    out->sizeof_vertex_spec = static_cast<uint32_t>(sizeof(kimix_gdb_vertex_spec));
    out->sizeof_size_t = static_cast<uint32_t>(sizeof(size_t));
    out->alignof_guid = static_cast<uint32_t>(alignof(kimix_gdb_guid));
    out->sizeof_traversal_vertex = static_cast<uint32_t>(sizeof(kimix_gdb_traversal_vertex));
    out->sizeof_aggregate_result = static_cast<uint32_t>(sizeof(kimix_gdb_aggregate_result));
    out->sizeof_bfs_vertex = static_cast<uint32_t>(sizeof(kimix_gdb_bfs_vertex));
    out->sizeof_bfs_edge = static_cast<uint32_t>(sizeof(kimix_gdb_bfs_edge));
    return KIMIX_GDB_OK;
}

const char *kimix_gdb_status_name(int32_t status) {
    return kimix::graph_db::status_name(static_cast<GdiStatus>(status));
}

// ===========================================================================
// Session lifecycle
// ===========================================================================

int32_t kimix_gdb_open(const kimix_gdb_open_options *options, kimix_gdb_session **out) {
    if (out == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix_gdb_open_options defaults{};
    defaults.auto_create_schema = 1;
    defaults.key_mode = 0;
    defaults.allow_parallel_edges = 1;
    defaults.allow_self_edges = 1;
    defaults.max_journal_bytes = 64ull * 1024ull * 1024ull;
    kimix_gdb_open_options const &request = options != nullptr ? *options : defaults;

    kimix::graph_db::GraphOptions graph_options;
    graph_options.auto_create_schema = request.auto_create_schema != 0;
    graph_options.key_mode = request.key_mode == 1 ? kimix::graph_db::KeyMode::report_warning
                                                   : kimix::graph_db::KeyMode::enforce_unique;
    graph_options.allow_parallel_edges = request.allow_parallel_edges != 0;
          graph_options.allow_self_edges = request.allow_self_edges != 0;
      // ABI 6 (plan v3 §4.5): 0 (or a pre-ABI-6 zeroed struct) = MD5, 1 = XXH3.
      graph_options.content_digest = request.content_digest == 1
                                         ? kimix::graph_db::ContentDigest::xxh3
                                         : kimix::graph_db::ContentDigest::md5;
    graph_options.persist_on_commit = request.persist_on_commit != 0;
    graph_options.max_journal_bytes =
        request.max_journal_bytes != 0 ? request.max_journal_bytes : 64ull * 1024ull * 1024ull;
    graph_options.cold_map_size = request.cold_map_size;

    kimix_gdb_session *session = new kimix_gdb_session();
    kimix::filesystem::path directory;
    if (request.directory != nullptr && request.directory[0] != '\0') {
        if (!kimix::path_from_narrow(request.directory, directory)) {
            delete session;
            return KIMIX_GDB_ERR_ARGUMENT;
        }
    }
    // The named-graph catalog name (ABI 5+, plan v3 step 3.7): an explicit
    // graph_name wins; without one the directory path remains the recorded
    // name (the pre-ABI-5 behavior, unchanged).  Only an EXPLICIT name enters
    // the live-graph catalog.
    kimix::string catalog_name;
    if (request.graph_name != nullptr && request.graph_name[0] != '\0') {
        catalog_name.assign(request.graph_name);
        graph_options.graph_name = catalog_name;
    } else if (request.directory != nullptr && request.directory[0] != '\0') {
        graph_options.graph_name = request.directory;
    }
    GdiStatus const status = session->database.open(directory, graph_options);
    if (is_error(status)) {
        delete session;
        return code_of(status);
    }
    if (!catalog_name.empty()) {
        // One live graph per name: a collision is a status code, not an
        // exception, and leaves the refused session completely unregistered.
        std::lock_guard<std::mutex> lock(g_catalog_mutex);
        if (g_catalog.find(catalog_name) != g_catalog.end()) {
            session->database.close();
            delete session;
            return KIMIX_GDB_ERR_NAME_EXISTS;
        }
        session->graph_name = catalog_name;
        g_catalog.emplace(session->graph_name, session);
    }
    session->open = true;
    *out = session;
    return KIMIX_GDB_OK;
}

void kimix_gdb_close(kimix_gdb_session *session) {
    if (session == nullptr) {
        return;
    }
    if (!session->graph_name.empty()) {
        std::lock_guard<std::mutex> lock(g_catalog_mutex);
        auto entry = g_catalog.find(session->graph_name);
        if (entry != g_catalog.end() && entry->second == session) {
            g_catalog.erase(entry);
        }
        session->graph_name.clear();
    }
    session->database.close();
    session->open = false;
    delete session;
}

int32_t kimix_gdb_capabilities(kimix_gdb_session *session, kimix_gdb_capability *out) {
    if (session == nullptr || out == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::graph_db::Capability const capability = session->database.capabilities();
    out->supports_global_scan = capability.supports_global_scan ? 1 : 0;
    out->supports_edge_by_stable_uid = capability.supports_edge_by_stable_uid ? 1 : 0;
    out->supports_edge_property_read = capability.supports_edge_property_read ? 1 : 0;
    out->supports_native_multi_hop = capability.supports_native_multi_hop ? 1 : 0;
    out->max_native_hop_depth = capability.max_native_hop_depth;
    out->supports_global_aggregates = capability.supports_global_aggregates ? 1 : 0;
    out->supports_collective_transactions = capability.supports_collective_transactions ? 1 : 0;
    out->enforces_unique_external_ids = capability.enforces_unique_external_ids ? 1 : 0;
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_catalog_contains(const char *name, size_t name_len, uint8_t *out_exists) {
    if (name == nullptr || name_len == 0 || out_exists == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::string key{name, name + name_len};
    std::lock_guard<std::mutex> lock(g_catalog_mutex);
    *out_exists = g_catalog.find(key) != g_catalog.end() ? 1 : 0;
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_catalog_count(size_t *out_count) {
    if (out_count == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(g_catalog_mutex);
    *out_count = g_catalog.size();
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_session_graph_name(kimix_gdb_session *session, char *buffer, size_t capacity,
                                     size_t *out_len) {
    if (session == nullptr || out_len == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    size_t const needed = session->graph_name.size();
    *out_len = needed;
    if (buffer == nullptr || capacity < needed) {
        return KIMIX_GDB_ERR_BUFFER;
    }
    std::memcpy(buffer, session->graph_name.data(), needed);
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_persist(kimix_gdb_session *session, uint8_t full) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.persist(full != 0));
}

int32_t kimix_gdb_load(kimix_gdb_session *session) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.load_store());
}

// ===========================================================================
// Vertices
// ===========================================================================

int32_t kimix_gdb_vertex_create(kimix_gdb_session *session, const kimix_gdb_vertex_spec *spec,
                                kimix_gdb_guid *out_uid, uint8_t *out_warning) {
    if (session == nullptr || spec == nullptr ||
        (spec->labels == nullptr && spec->label_count != 0) ||
        (spec->properties == nullptr && spec->property_count != 0) ||
        (spec->property_names == nullptr && spec->property_count != 0)) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::graph_db::PropertyList properties;
    if (int32_t const refusal = named_properties(*spec, properties)) {
        return refusal; // a bad block / element flag: refuse, store nothing
    }
    kimix::graph_db::VertexCreateResult created;
    GdiStatus const status = session->database.create_vertex(
        VertexKeyView{spec->key.label, to_bytes(spec->key.external_id)},
        kimix::span<const LabelId>{spec->labels, spec->label_count}, &properties, created);
    if (is_error(status)) {
        return code_of(status);
    }
    if (out_uid != nullptr) {
        *out_uid = from_guid(created.uid);
    }
    if (out_warning != nullptr) {
        *out_warning = created.non_unique_id_warning ? 1 : 0;
    }
    return code_of(status);
}

int32_t kimix_gdb_vertex_get_or_create(kimix_gdb_session *session,
                                       const kimix_gdb_vertex_spec *spec,
                                       kimix_gdb_guid *out_uid, uint8_t *out_created) {
    if (session == nullptr || spec == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::graph_db::PropertyList properties;
    if (int32_t const refusal = named_properties(*spec, properties)) {
        return refusal; // a bad block / element flag: refuse, store nothing
    }
    kimix::graph_db::VertexMergeResult merged;
    GdiStatus const status = session->database.get_or_create_vertex(
        VertexKeyView{spec->key.label, to_bytes(spec->key.external_id)},
        kimix::span<const LabelId>{spec->labels, spec->label_count}, &properties, merged);
    if (is_error(status)) {
        return code_of(status);
    }
    if (out_uid != nullptr) {
        *out_uid = from_guid(merged.uid);
    }
    if (out_created != nullptr) {
        *out_created = merged.created ? 1 : 0;
    }
    return code_of(status);
}

int32_t kimix_gdb_vertex_translate(kimix_gdb_session *session, const kimix_gdb_vertex_key *key,
                                   kimix_gdb_guid *out_uid, uint8_t *out_found) {
    if (session == nullptr || key == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::Guid resolved;
    bool found = false;
    GdiStatus const status = session->database.translate_vertex_id(
        VertexKeyView{key->label, to_bytes(key->external_id)}, resolved, found);
    if (is_error(status)) {
        return code_of(status);
    }
    if (out_found != nullptr) {
        *out_found = found ? 1 : 0;
    }
    if (found && out_uid != nullptr) {
        *out_uid = from_guid(resolved);
    }
    return code_of(status);
}

int32_t kimix_gdb_vertex_free(kimix_gdb_session *session, kimix_gdb_guid vertex) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.free_vertex(to_guid(vertex)));
}

int32_t kimix_gdb_vertex_free_guarded(kimix_gdb_session *session, kimix_gdb_guid vertex) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.free_vertex_guarded(to_guid(vertex)));
}

// ===========================================================================
// Labels
// ===========================================================================

int32_t kimix_gdb_vertex_label_add(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                   uint32_t label) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.add_label(to_guid(vertex), label));
}

int32_t kimix_gdb_vertex_label_remove(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                      uint32_t label) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.remove_label(to_guid(vertex), label));
}

int32_t kimix_gdb_vertex_labels(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                uint32_t *labels, size_t capacity, size_t *out_count) {
    if (session == nullptr || out_count == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::Guid const uid = to_guid(vertex);
    kimix::graph_db::Vertex *record = session->database.graph().find(uid);
    if (record == nullptr) {
        return KIMIX_GDB_ERR_VERTEX;
    }
    kimix::vector<LabelId> ids;
    record->labels(ids);
    // The fetch below writes the key label first and skips it inside `ids`, so
    // the probe must count exactly that.  Normally _labels does NOT carry the
    // key label (it lives in _key_label), but a create whose label list names
    // the key label stores it there too — count it once, not twice.
    bool const key_in_labels =
        record->key_label() != 0 &&
        std::find(ids.begin(), ids.end(), record->key_label()) != ids.end();
    size_t const total = ids.size() + (record->key_label() != 0 && !key_in_labels ? 1 : 0);
    *out_count = total;
    if (labels == nullptr) {
        return KIMIX_GDB_OK; // size query
    }
    if (capacity < total) {
        return KIMIX_GDB_ERR_BUFFER;
    }
    size_t cursor = 0;
    if (record->key_label() != 0) {
        labels[cursor++] = record->key_label(); // the key label comes FIRST
    }
    for (auto id : ids) {
        if (id == record->key_label()) {
            continue;
        }
        labels[cursor++] = id;
    }
    *out_count = cursor;
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_edge_label_add(kimix_gdb_session *session, kimix_gdb_guid edge,
                                 uint32_t label) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.add_edge_label(to_guid(edge), label));
}

int32_t kimix_gdb_edge_label_remove(kimix_gdb_session *session, kimix_gdb_guid edge,
                                    uint32_t label) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.remove_edge_label(to_guid(edge), label));
}

int32_t kimix_gdb_edge_labels(kimix_gdb_session *session, kimix_gdb_guid edge,
                              uint32_t *labels, size_t capacity, size_t *out_count) {
    if (session == nullptr || out_count == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::vector<LabelId> ids;
    if (!session->database.graph().edge_labels(to_guid(edge), ids)) {
        return KIMIX_GDB_ERR_UID;
    }
    *out_count = ids.size();
    if (labels == nullptr) {
        return KIMIX_GDB_OK;
    }
    if (capacity < ids.size()) {
        return KIMIX_GDB_ERR_BUFFER;
    }
    for (size_t i = 0; i < ids.size(); ++i) {
        labels[i] = ids[i];
    }
    return KIMIX_GDB_OK;
}

// ===========================================================================
// Properties
// ===========================================================================

} // extern "C"

namespace src_graph_db_graph_c_api {

int32_t vertex_property_write(kimix_gdb_session *session, kimix_gdb_guid vertex,
                              uint32_t name, kimix_gdb_property const *value, int mode) {
    if (session == nullptr || value == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::Guid const uid = to_guid(vertex);
    DecodeRefusal malformed = DecodeRefusal::ok;
    PropertyValue const decoded = to_value(*value, &malformed);
    // The block write-path guard, ONE implementation for every property write
    // (malformed_status): a bad block-kind payload answers ERR_COUNT or ERR_DATATYPE
    // and the Database never sees the value.
    if (int32_t const refusal = malformed_status(malformed)) {
        return refusal;
    }
    GdiStatus status = GdiStatus::ok;
    switch (mode) {
    case 0: status = session->database.add_property(uid, name, decoded); break;
    case 1: status = session->database.set_property(uid, name, decoded); break;
    default: status = session->database.update_property(uid, name, decoded); break;
    }
    return code_of(status);
}

int32_t vertex_properties_read(kimix_gdb_session *session, kimix_gdb_guid vertex,
                               void *raw_items, size_t capacity, uint8_t *scratch,
                               size_t scratch_capacity, size_t *out_count, size_t *out_scratch,
                               int what) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
      kimix::Guid const uid = to_guid(vertex);
        if (what == 0) {
            // Plan v3 §4.3: one pass straight from the bag into the caller's
            // record + scratch buffers (no PropertyList / (id, value) copies).
            // The single find below doubles as the existence check (no separate
            // contains() hash lookup).
            kimix::graph_db::Vertex *vertex = session->database.graph().find(uid);
            if (vertex == nullptr) {
                return KIMIX_GDB_ERR_VERTEX;
            }
          return code_of(write_vertex_property_records(
              *vertex, static_cast<kimix_gdb_property_record *>(raw_items), capacity, scratch,
              scratch_capacity, out_count, out_scratch));
      }
      auto *items = static_cast<kimix_gdb_property_type *>(raw_items);
      // Property TYPES: the declaration of every property the vertex carries.
      kimix::graph_db::PropertyTypeList types;
      (void)session->database.get_property_types(uid, types);
      // PHASE 1 (both buffers NULL): sizes only, success — the documented probe.
      if (items == nullptr && scratch == nullptr) {
          size_t needed = 0;
          for (auto const &type : types) {
              needed += type.name.size();
          }
          if (out_count != nullptr) {
              *out_count = types.size();
          }
          if (out_scratch != nullptr) {
              *out_scratch = needed;
          }
          return KIMIX_GDB_OK;
      }
      ScratchWriter writer{scratch, scratch_capacity};
      if (out_count != nullptr) {
          *out_count = types.size();
    }
    GdiStatus status = GdiStatus::ok;
    for (size_t i = 0; i < types.size(); ++i) {
        uint32_t offset = 0;
        uint32_t length = 0;
        if (!writer.append_text(types[i].name, offset, length)) {
            status = GdiStatus::error_buffer;
        }
        if (items != nullptr && i < capacity) {
            items[i].id = types[i].name_id;
            items[i].container = static_cast<uint8_t>(types[i].container);
            items[i].dtype = static_cast<uint8_t>(types[i].data);
            // ABI 8: the size type occupies the byte an ABI-7 image left zero.
            items[i].size_type = static_cast<uint8_t>(types[i].size_type);
            for (size_t r = 0; r < sizeof(items[i].reserved); ++r) {
                items[i].reserved[r] = 0;
            }
            items[i].count = types[i].count;
            items[i].name_offset = offset;
            items[i].name_len = length;
        }
    }
    if (out_scratch != nullptr) {
        *out_scratch = writer.needed();
    }
    if (status == GdiStatus::ok && raw_items != nullptr && types.size() > capacity) {
        status = GdiStatus::error_buffer;
    }
    return code_of(status);
}

int32_t edge_property_write(kimix_gdb_session *session, kimix_gdb_guid edge, uint32_t name,
                            kimix_gdb_property const *value, int mode) {
    if (session == nullptr || value == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::Guid const id = to_guid(edge);
    DecodeRefusal malformed = DecodeRefusal::ok;
    PropertyValue const decoded = to_value(*value, &malformed);
    // The block write-path guard, ONE implementation for every property write
    // (malformed_status): a bad block-kind payload answers ERR_COUNT or ERR_DATATYPE
    // and the Database never sees the value.
    if (int32_t const refusal = malformed_status(malformed)) {
        return refusal;
    }
    GdiStatus status = GdiStatus::ok;
    switch (mode) {
    case 0: status = session->database.add_edge_property(id, name, decoded); break;
    case 1: status = session->database.set_edge_property(id, name, decoded); break;
    default: status = session->database.update_edge_property(id, name, decoded); break;
    }
    return code_of(status);
}

int32_t edge_properties_read(kimix_gdb_session *session, kimix_gdb_guid edge,
                             void *raw_items, size_t capacity, uint8_t *scratch,
                             size_t scratch_capacity, size_t *out_count, size_t *out_scratch,
                             int what) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::Guid const id = to_guid(edge);
    if (!session->database.graph().edges().contains(id)) {
        return KIMIX_GDB_ERR_UID;
    }
    if (what == 0) {
        kimix::vector<std::pair<PropNameId, PropertyValue>> properties;
        collect_edge_properties(session->database, id, properties);
        return code_of(write_properties(
            properties, static_cast<kimix_gdb_property_record *>(raw_items), capacity, scratch,
            scratch_capacity, out_count, out_scratch));
    }
    auto *items = static_cast<kimix_gdb_property_type *>(raw_items);
    kimix::graph_db::PropertyTypeList types;
    (void)session->database.get_edge_property_types(id, types);
    // PHASE 1 (both buffers NULL): sizes only, success — the documented probe.
    if (items == nullptr && scratch == nullptr) {
        size_t needed = 0;
        for (auto const &type : types) {
            needed += type.name.size();
        }
        if (out_count != nullptr) {
            *out_count = types.size();
        }
        if (out_scratch != nullptr) {
            *out_scratch = needed;
        }
        return KIMIX_GDB_OK;
    }
    ScratchWriter writer{scratch, scratch_capacity};
    if (out_count != nullptr) {
        *out_count = types.size();
    }
    GdiStatus status = GdiStatus::ok;
    for (size_t i = 0; i < types.size(); ++i) {
        uint32_t offset = 0;
        uint32_t length = 0;
        if (!writer.append_text(types[i].name, offset, length)) {
            status = GdiStatus::error_buffer;
        }
        if (items != nullptr && i < capacity) {
            items[i].id = types[i].name_id;
            items[i].container = static_cast<uint8_t>(types[i].container);
            items[i].dtype = static_cast<uint8_t>(types[i].data);
            // ABI 8: the size type occupies the byte an ABI-7 image left zero.
            items[i].size_type = static_cast<uint8_t>(types[i].size_type);
            for (size_t r = 0; r < sizeof(items[i].reserved); ++r) {
                items[i].reserved[r] = 0;
            }
            items[i].count = types[i].count;
            items[i].name_offset = offset;
            items[i].name_len = length;
        }
    }
    if (out_scratch != nullptr) {
        *out_scratch = writer.needed();
    }
    if (status == GdiStatus::ok && raw_items != nullptr && types.size() > capacity) {
        status = GdiStatus::error_buffer;
    }
    return code_of(status);
}

} // namespace src_graph_db_graph_c_api
using namespace src_graph_db_graph_c_api;

extern "C" {

int32_t kimix_gdb_vertex_property_add(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                      uint32_t name, const kimix_gdb_property *value) {
    return vertex_property_write(session, vertex, name, value, 0);
}

int32_t kimix_gdb_vertex_property_set(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                      uint32_t name, const kimix_gdb_property *value) {
    return vertex_property_write(session, vertex, name, value, 1);
}

int32_t kimix_gdb_vertex_property_update(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                         uint32_t name, const kimix_gdb_property *value) {
    return vertex_property_write(session, vertex, name, value, 2);
}

int32_t kimix_gdb_vertex_properties_remove(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                           const uint32_t *names, size_t name_count) {
    if (session == nullptr || (names == nullptr && name_count != 0)) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.remove_properties(
        to_guid(vertex), kimix::span<const PropNameId>{names, name_count}));
}

int32_t kimix_gdb_vertex_properties(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                    kimix_gdb_property_record *items, size_t capacity,
                                    uint8_t *scratch, size_t scratch_capacity,
                                    size_t *out_count, size_t *out_scratch_bytes) {
    return vertex_properties_read(session, vertex, items, capacity, scratch, scratch_capacity,
                                  out_count, out_scratch_bytes, 0);
}

int32_t kimix_gdb_vertex_property_types(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                        kimix_gdb_property_type *items, size_t capacity,
                                        uint8_t *scratch, size_t scratch_capacity,
                                        size_t *out_count, size_t *out_scratch_bytes) {
    return vertex_properties_read(session, vertex, items, capacity, scratch, scratch_capacity,
                                  out_count, out_scratch_bytes, 1);
}

int32_t kimix_gdb_edge_property_add(kimix_gdb_session *session, kimix_gdb_guid edge,
                                    uint32_t name, const kimix_gdb_property *value) {
    return edge_property_write(session, edge, name, value, 0);
}

int32_t kimix_gdb_edge_property_set(kimix_gdb_session *session, kimix_gdb_guid edge,
                                    uint32_t name, const kimix_gdb_property *value) {
    return edge_property_write(session, edge, name, value, 1);
}

int32_t kimix_gdb_edge_property_update(kimix_gdb_session *session, kimix_gdb_guid edge,
                                       uint32_t name, const kimix_gdb_property *value) {
    return edge_property_write(session, edge, name, value, 2);
}

int32_t kimix_gdb_edge_properties_remove(kimix_gdb_session *session, kimix_gdb_guid edge,
                                         const uint32_t *names, size_t name_count) {
    if (session == nullptr || (names == nullptr && name_count != 0)) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.remove_edge_properties(
        to_guid(edge), kimix::span<const PropNameId>{names, name_count}));
}

int32_t kimix_gdb_edge_properties(kimix_gdb_session *session, kimix_gdb_guid edge,
                                  kimix_gdb_property_record *items, size_t capacity,
                                  uint8_t *scratch, size_t scratch_capacity,
                                  size_t *out_count, size_t *out_scratch_bytes) {
    return edge_properties_read(session, edge, items, capacity, scratch, scratch_capacity,
                                out_count, out_scratch_bytes, 0);
}

int32_t kimix_gdb_edge_property_types(kimix_gdb_session *session, kimix_gdb_guid edge,
                                      kimix_gdb_property_type *items, size_t capacity,
                                      uint8_t *scratch, size_t scratch_capacity,
                                      size_t *out_count, size_t *out_scratch_bytes) {
    return edge_properties_read(session, edge, items, capacity, scratch, scratch_capacity,
                                out_count, out_scratch_bytes, 1);
}

// ===========================================================================
// Edges
// ===========================================================================

int32_t kimix_gdb_edge_create(kimix_gdb_session *session, kimix_gdb_guid origin,
kimix_gdb_guid target, uint8_t direction, const uint32_t *labels,
size_t label_count, const uint32_t *property_names,
const kimix_gdb_property *properties, size_t property_count,
const kimix_gdb_guid *uid, kimix_gdb_guid *out_edge) {
 if (session == nullptr || (labels == nullptr && label_count != 0) ||
     (property_names == nullptr && property_count != 0) ||
     (properties == nullptr && property_count != 0)) {
 return KIMIX_GDB_ERR_ARGUMENT;
 }
 kimix_gdb_edge_spec spec{};
 spec.origin = origin;
 spec.target = target;
 spec.direction = direction;
 spec.labels = labels;
 spec.label_count = label_count;
 spec.properties = properties;
 spec.property_names = property_names;
 spec.property_count = property_count;
 kimix::graph_db::PropertyList list;
 if (int32_t const refusal = named_properties(spec, list)) {
     return refusal; // a bad block / element flag: refuse, store nothing
 }
 // ABI 8: a NULL uid (or an all-zero one) mints the uid; anything else is the
 // caller's choice, and the live-duplicate check is the Database's.
 kimix::Guid const chosen = uid != nullptr ? to_guid(*uid) : kimix::Guid{};
 bool const has_uid = uid != nullptr && !is_zero_guid(chosen);
 kimix::Guid edge;
 GdiStatus const status = session->database.edge_create(
        to_guid(origin), to_guid(target),
        direction == KIMIX_GDB_EDGE_UNDIRECTED ? EdgeDirectionType::undirected
                                               : EdgeDirectionType::directed,
        kimix::span<const LabelId>{labels, label_count}, &list,
        has_uid ? &chosen : nullptr, edge);
    if (is_error(status)) {
        return code_of(status);
    }
    if (out_edge != nullptr) {
        *out_edge = from_guid(edge);
    }
    return code_of(status);
}

int32_t kimix_gdb_edge_free(kimix_gdb_session *session, kimix_gdb_guid edge) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.edge_free(to_guid(edge)));
}

int32_t kimix_gdb_edge_set_origin(kimix_gdb_session *session, kimix_gdb_guid edge,
                                  kimix_gdb_guid origin) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.set_edge_origin(to_guid(edge), to_guid(origin)));
}

int32_t kimix_gdb_edge_set_target(kimix_gdb_session *session, kimix_gdb_guid edge,
                                  kimix_gdb_guid target) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.set_edge_target(to_guid(edge), to_guid(target)));
}

int32_t kimix_gdb_edge_set_direction(kimix_gdb_session *session, kimix_gdb_guid edge,
                                     uint8_t direction) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.set_edge_direction(
        to_guid(edge), direction == KIMIX_GDB_EDGE_UNDIRECTED ? EdgeDirectionType::undirected
                                                             : EdgeDirectionType::directed));
}

int32_t kimix_gdb_edge_endpoints(kimix_gdb_session *session, kimix_gdb_guid edge,
                                 kimix_gdb_guid *out_origin, kimix_gdb_guid *out_target) {
    if (session == nullptr || out_origin == nullptr || out_target == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::Guid origin;
    kimix::Guid target;
    GdiStatus const status = session->database.get_edge_endpoints(to_guid(edge), origin, target);
    if (is_error(status)) {
        return code_of(status);
    }
    *out_origin = from_guid(origin);
    *out_target = from_guid(target);
    return KIMIX_GDB_OK;
}

// ===========================================================================
// Filtered, oriented, paged reads
// ===========================================================================

int32_t kimix_gdb_edges_of_vertex(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                  uint32_t constraint, uint8_t orientation, int64_t cap,
                                  kimix_gdb_edge_record *items, size_t capacity,
                                  size_t *out_count, size_t *out_total) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    Constraint const *resolved = nullptr;
    GdiStatus status = resolve_constraint(session, constraint, resolved);
    if (is_error(status)) {
        return code_of(status);
    }
    EdgeOrientation const direction = orientation == KIMIX_GDB_ORIENTATION_OUTGOING
                                          ? EdgeOrientation::outgoing
                                      : orientation == KIMIX_GDB_ORIENTATION_INCOMING
                                          ? EdgeOrientation::incoming
                                          : EdgeOrientation::both;
    kimix::graph_db::EdgePage page;
    // Phase 1 (items == NULL) always asks for the count, so the caller learns the
    // size even when it passed no cap.
    int64_t const effective_cap = items == nullptr ? 0 : cap;
    status = session->database.get_edges_of_vertex(to_guid(vertex), *resolved, direction,
                                                  effective_cap, page);
    if (is_error(status)) {
        return code_of(status);
    }
    if (out_total != nullptr) {
        *out_total = page.total_count;
    }
    if (out_count != nullptr) {
        *out_count = page.items.size();
    }
    if (items == nullptr) {
        return KIMIX_GDB_OK;
    }
    if (page.items.size() > capacity) {
        return KIMIX_GDB_ERR_BUFFER;
    }
    for (size_t i = 0; i < page.items.size(); ++i) {
        items[i].uid = from_guid(page.items[i].id);
        items[i].origin = from_guid(page.items[i].origin);
        items[i].target = from_guid(page.items[i].target);
        items[i].direction = page.items[i].direction == EdgeDirectionType::undirected
                                 ? KIMIX_GDB_EDGE_UNDIRECTED
                                 : KIMIX_GDB_EDGE_DIRECTED;
    }
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_neighbors_of_vertex(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                      uint32_t constraint, uint8_t orientation, int64_t cap,
                                      kimix_gdb_guid *items, size_t capacity, size_t *out_count,
                                      size_t *out_total) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    Constraint const *resolved = nullptr;
    GdiStatus status = resolve_constraint(session, constraint, resolved);
    if (is_error(status)) {
        return code_of(status);
    }
    EdgeOrientation const direction = orientation == KIMIX_GDB_ORIENTATION_OUTGOING
                                          ? EdgeOrientation::outgoing
                                      : orientation == KIMIX_GDB_ORIENTATION_INCOMING
                                          ? EdgeOrientation::incoming
                                          : EdgeOrientation::both;
    kimix::graph_db::VertexPage page;
    int64_t const effective_cap = items == nullptr ? 0 : cap;
    status = session->database.get_neighbor_vertices(to_guid(vertex), *resolved, direction,
                                                    effective_cap, page);
    if (is_error(status)) {
        return code_of(status);
    }
    if (out_total != nullptr) {
        *out_total = page.total_count;
    }
    if (out_count != nullptr) {
        *out_count = page.items.size();
    }
    if (items == nullptr) {
        return KIMIX_GDB_OK;
    }
    if (page.items.size() > capacity) {
        return KIMIX_GDB_ERR_BUFFER;
    }
    for (size_t i = 0; i < page.items.size(); ++i) {
        items[i] = from_guid(page.items[i]);
    }
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_count_neighbors(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                  uint32_t constraint, uint8_t orientation, size_t *out_count) {
    if (session == nullptr || out_count == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    Constraint const *resolved = nullptr;
    GdiStatus status = resolve_constraint(session, constraint, resolved);
    if (is_error(status)) {
        return code_of(status);
    }
    EdgeOrientation const direction = orientation == KIMIX_GDB_ORIENTATION_OUTGOING
                                          ? EdgeOrientation::outgoing
                                      : orientation == KIMIX_GDB_ORIENTATION_INCOMING
                                          ? EdgeOrientation::incoming
                                          : EdgeOrientation::both;
    return code_of(
        session->database.count_neighbors(to_guid(vertex), *resolved, direction, *out_count));
}

int32_t kimix_gdb_exists_neighbor(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                  uint32_t constraint, uint8_t orientation,
                                  uint8_t *out_exists) {
    if (session == nullptr || out_exists == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    Constraint const *resolved = nullptr;
    GdiStatus status = resolve_constraint(session, constraint, resolved);
    if (is_error(status)) {
        return code_of(status);
    }
    EdgeOrientation const direction = orientation == KIMIX_GDB_ORIENTATION_OUTGOING
                                          ? EdgeOrientation::outgoing
                                      : orientation == KIMIX_GDB_ORIENTATION_INCOMING
                                          ? EdgeOrientation::incoming
                                          : EdgeOrientation::both;
    bool exists = false;
    status = session->database.exists_neighbor(to_guid(vertex), *resolved, direction, exists);
    if (is_error(status)) {
        return code_of(status);
    }
    *out_exists = exists ? 1 : 0;
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_traverse_bfs(kimix_gdb_session *session, kimix_gdb_guid start,
                               uint32_t constraint, uint8_t orientation, int64_t cap_per_level,
                               uint32_t max_depth, kimix_gdb_traversal_vertex *items,
                               size_t capacity, size_t *out_count) {
    if (session == nullptr || out_count == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    if (cap_per_level < 1 || max_depth < 1 ||
        max_depth > kimix::graph_db::Graph::kMaxNativeHopDepth) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::graph_db::Constraint const *resolved = nullptr;
    GdiStatus status = resolve_constraint(session, constraint, resolved);
    if (is_error(status)) {
        return code_of(status);
    }
    EdgeOrientation const direction = orientation == KIMIX_GDB_ORIENTATION_OUTGOING
                                          ? EdgeOrientation::outgoing
                                      : orientation == KIMIX_GDB_ORIENTATION_INCOMING
                                          ? EdgeOrientation::incoming
                                          : EdgeOrientation::both;
    kimix::vector<kimix::graph_db::Graph::TraversalVertex> walk;
    status = session->database.traverse_bfs(to_guid(start), *resolved, direction, cap_per_level,
                                            max_depth, walk);
    if (is_error(status)) {
        return code_of(status);
    }
    *out_count = walk.size();
    if (items == nullptr) {
        return KIMIX_GDB_OK;
    }
    if (walk.size() > capacity) {
        return KIMIX_GDB_ERR_BUFFER;
    }
    for (size_t i = 0; i < walk.size(); ++i) {
        items[i].vertex = from_guid(walk[i].guid);
        items[i].parent = from_guid(walk[i].parent);
        items[i].depth = walk[i].depth;
        items[i].reserved = 0;
    }
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_shortest_path(kimix_gdb_session *session, kimix_gdb_guid from,
                                kimix_gdb_guid to, uint32_t max_depth, kimix_gdb_guid *items,
                                size_t capacity, size_t *out_count) {
    if (session == nullptr || out_count == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    if (max_depth < 1 || max_depth > kimix::graph_db::Graph::kMaxNativeHopDepth) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::vector<kimix::Guid> path;
    GdiStatus const status =
        session->database.shortest_path(to_guid(from), to_guid(to), max_depth, path);
    if (is_error(status)) {
        return code_of(status);
    }
    *out_count = path.size();
    if (items == nullptr) {
        return KIMIX_GDB_OK;
    }
    if (path.size() > capacity) {
        return KIMIX_GDB_ERR_BUFFER;
    }
    for (size_t i = 0; i < path.size(); ++i) {
        items[i] = from_guid(path[i]);
    }
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_shortest_path_edges(kimix_gdb_session *session, kimix_gdb_guid from,
                                      kimix_gdb_guid to, uint32_t edge_constraint,
                                      uint8_t orientation, uint32_t max_depth,
                                      kimix_gdb_path_step *items, size_t capacity,
                                      size_t *out_count) {
    if (session == nullptr || out_count == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    if (max_depth < 1 || max_depth > kimix::graph_db::Graph::kMaxNativeHopDepth) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    Constraint const *resolved = nullptr;
    GdiStatus status = resolve_constraint(session, edge_constraint, resolved);
    if (is_error(status)) {
        return code_of(status);
    }
    EdgeOrientation const direction = orientation == KIMIX_GDB_ORIENTATION_OUTGOING
                                          ? EdgeOrientation::outgoing
                                      : orientation == KIMIX_GDB_ORIENTATION_INCOMING
                                          ? EdgeOrientation::incoming
                                          : EdgeOrientation::both;
    // Plan O12: fill the SESSION's staging buffer, not a fresh local vector — the
    // count-only phase and the fill phase then share one buffer whose capacity
    // survives between calls (the walk fills it in place, plan O8). `path` is only
    // read below, and the walk clears it first (Database::shortest_path), so a
    // stale path from the previous call can never leak into this one.
    kimix::vector<kimix::graph_db::Graph::PathStep> &path = session->path_staging;
    status = session->database.shortest_path(to_guid(from), to_guid(to), *resolved, direction,
                                             max_depth, path);
    if (is_error(status)) {
        return code_of(status);
    }
    *out_count = path.size();
    if (items == nullptr) {
        return KIMIX_GDB_OK;
    }
    if (path.size() > capacity) {
        return KIMIX_GDB_ERR_BUFFER;
    }
    for (size_t i = 0; i < path.size(); ++i) {
        items[i].vertex = from_guid(path[i].vertex);
        items[i].edge = from_guid(path[i].edge);
        items[i].depth = path[i].depth;
        items[i].reserved = 0;
    }
    return KIMIX_GDB_OK;
}

// ABI 7: the edge-filtered BFS. The symbol and its contract landed in the ABI-7
// skeleton commit so binders and the export audit saw the frozen shape; this is the
// implementation.
//
// The two-phase protocol is the one kimix_gdb_shortest_path_edges above uses — probe
// with both caller arrays NULL, then fill — with THREE outputs instead of one: the
// discovered vertices, every constraint-satisfying edge inside that set, and the
// truncation flag. The walk writes the SESSION's staging record
// (session->bfs_staging), so the probe sizes the buffers the fill reuses and neither
// phase allocates for the result on a warm session (contract point 3: no library
// allocation on the read path).
//
// Validation order mirrors shortest_path_edges exactly: the NULL guards and the bounds
// come FIRST (they are argument errors, and an argument error outranks a constraint
// error), then the constraint handle resolves, and only then does the walk run — which
// is what makes an unknown start answer ERR_VERTEX and a label condition carrying an
// ordering op answer ERR_OP from the Database/Graph layer rather than inventing a code
// here. ERR_BUFFER leaves the three outputs WRITTEN (the counts the caller must
// allocate for and the flag the same walk computed), so a caller that has to retry can
// retry with the shape it already learned.
int32_t kimix_gdb_traverse_bfs_edges(kimix_gdb_session *session,
                                     const kimix_gdb_guid *starts, size_t start_count,
                                     uint32_t edge_constraint, uint8_t orientation,
                                     uint32_t max_depth, uint32_t max_vertices,
                                     kimix_gdb_bfs_vertex *items, size_t vertex_capacity,
                                     kimix_gdb_bfs_edge *edges, size_t edge_capacity,
                                     size_t *out_vertex_count, size_t *out_edge_count,
                                     uint8_t *out_truncated) {
    if (session == nullptr || out_vertex_count == nullptr || out_edge_count == nullptr ||
        out_truncated == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    if (max_depth < 1 || max_depth > kimix::graph_db::Graph::kMaxNativeHopDepth) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    if (max_vertices < 1) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    // ABI 8: at least one start vertex. Checked with the other argument rules, i.e.
    // BEFORE the constraint is resolved, so the error order stays "argument first".
    if (starts == nullptr || start_count == 0) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    Constraint const *resolved = nullptr;
    GdiStatus status = resolve_constraint(session, edge_constraint, resolved);
    if (is_error(status)) {
        return code_of(status);
    }
    EdgeOrientation const direction = orientation == KIMIX_GDB_ORIENTATION_OUTGOING
                                          ? EdgeOrientation::outgoing
                                      : orientation == KIMIX_GDB_ORIENTATION_INCOMING
                                          ? EdgeOrientation::incoming
                                          : EdgeOrientation::both;
    // The caller's guids, converted into the session's staging: `start_count` is
    // bounded by the caller's own array, and the Database validates every one of them
    // before the walk writes anything.
    kimix::vector<kimix::Guid> &start_guids = session->start_staging;
    start_guids.clear();
    start_guids.reserve(start_count);
    for (size_t i = 0; i < start_count; ++i) {
        start_guids.push_back(to_guid(starts[i]));
    }
    // The walk fills the session's staging in place and clears it first
    // (Database::traverse_bfs_edges), so a stale neighbourhood from the previous call
    // — probe or fill, same session or a different one — cannot leak into this answer.
    kimix::graph_db::Graph::BfsEdgesResult &walk = session->bfs_staging;
    status = session->database.traverse_bfs_edges(
        kimix::span<const kimix::Guid>{start_guids.data(), start_guids.size()}, *resolved,
        direction, max_depth, max_vertices, walk);
    if (is_error(status)) {
        return code_of(status);
    }
    *out_vertex_count = walk.vertices.size();
    *out_edge_count = walk.edges.size();
    *out_truncated = walk.truncated ? 1 : 0;
    if (items == nullptr && edges == nullptr) {
        return KIMIX_GDB_OK; // PROBE: the walk ran, the three sizes are reported
    }
    // FILL: BOTH arrays are required (the contract's fill call hands both), and a NULL
    // one is the worst possible capacity — the vertex array can never be empty (items[0]
    // is the start), so a fill with items == NULL cannot be satisfied and answers
    // ERR_BUFFER rather than writing nothing and claiming OK.
    bool const vertex_short =
        items == nullptr ? !walk.vertices.empty() : walk.vertices.size() > vertex_capacity;
    bool const edge_short =
        edges == nullptr ? !walk.edges.empty() : walk.edges.size() > edge_capacity;
    if (vertex_short || edge_short) {
        return KIMIX_GDB_ERR_BUFFER; // the sizes and the flag above still stand
    }
    for (size_t i = 0; i < walk.vertices.size(); ++i) {
        items[i].vertex = from_guid(walk.vertices[i].vertex);
        items[i].edge = from_guid(walk.vertices[i].edge);
        items[i].depth = walk.vertices[i].depth;
        items[i].reserved = 0;
    }
    for (size_t i = 0; i < walk.edges.size(); ++i) {
        edges[i].edge = from_guid(walk.edges[i].edge);
        edges[i].origin = from_guid(walk.edges[i].origin);
        edges[i].target = from_guid(walk.edges[i].target);
    }
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_scan_vertices(kimix_gdb_session *session, uint32_t constraint, int64_t cap,
                                kimix_gdb_guid *items, size_t capacity, size_t *out_count,
                                size_t *out_total) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    Constraint const *resolved = nullptr;
    GdiStatus status = resolve_constraint(session, constraint, resolved);
    if (is_error(status)) {
        return code_of(status);
    }
    kimix::graph_db::VertexPage page;
    int64_t const effective_cap = items == nullptr ? 0 : cap;
    status = session->database.scan_vertices(*resolved, effective_cap, page);
    if (is_error(status)) {
        return code_of(status);
    }
    if (out_total != nullptr) {
        *out_total = page.total_count;
    }
    if (out_count != nullptr) {
        *out_count = page.items.size();
    }
    if (items == nullptr) {
        return KIMIX_GDB_OK;
    }
    if (page.items.size() > capacity) {
        return KIMIX_GDB_ERR_BUFFER;
    }
    for (size_t i = 0; i < page.items.size(); ++i) {
        items[i] = from_guid(page.items[i]);
    }
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_aggregate(kimix_gdb_session *session, uint32_t constraint, uint32_t property,
                            int32_t function, uint8_t *scratch, size_t scratch_capacity,
                            kimix_gdb_aggregate_result *out) {
    if (session == nullptr || out == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    Constraint const *resolved = nullptr;
    GdiStatus status = resolve_constraint(session, constraint, resolved);
    if (is_error(status)) {
        return code_of(status);
    }
    AggregateSpec spec;
    spec.constraint = *resolved; // AggregateSpec holds the constraint by value
    spec.property = PropNameId{property};
    spec.function = static_cast<GdiAggregate>(function);
    AggregateResult result;
    status = session->database.aggregate_vertices(spec, result);
    if (is_error(status)) {
        return code_of(status);
    }
    out->matched = result.matched;
    out->valued = result.valued;
    out->has_value = result.has_value ? 1 : 0;
    out->int64_value = 0;
    out->double_value = 0.0;
    out->string_offset = 0;
    out->string_len = 0;
    for (size_t i = 0; i < sizeof(out->reserved); ++i) {
        out->reserved[i] = 0;
    }
    switch (result.value.kind()) {
    case PropertyKind::int64_value:
        out->value_kind = KIMIX_GDB_VALUE_INT64;
        out->int64_value = result.value.as_int64();
        break;
    case PropertyKind::double_value:
        out->value_kind = KIMIX_GDB_VALUE_DOUBLE;
        out->double_value = result.value.as_double();
        break;
    case PropertyKind::string: {
        kimix::string_view const text = result.value.as_string();
        out->value_kind = KIMIX_GDB_VALUE_STRING;
        if (scratch == nullptr || text.size() > scratch_capacity) {
            out->string_len = static_cast<uint32_t>(text.size());
            return KIMIX_GDB_ERR_BUFFER;
        }
        std::memcpy(scratch, text.data(), text.size());
        out->string_offset = 0;
        out->string_len = static_cast<uint32_t>(text.size());
        break;
    }
    default:
        out->value_kind = KIMIX_GDB_VALUE_NONE;
        break;
    }
    return KIMIX_GDB_OK;
}

// ===========================================================================
// Transactions
// ===========================================================================

int32_t kimix_gdb_tx_begin(kimix_gdb_session *session) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.begin_transaction());
}

int32_t kimix_gdb_tx_commit(kimix_gdb_session *session) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.commit());
}

int32_t kimix_gdb_tx_abort(kimix_gdb_session *session) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.abort());
}

int32_t kimix_gdb_tx_begin_collective(kimix_gdb_session *session) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.begin_collective());
}

int32_t kimix_gdb_tx_close_collective(kimix_gdb_session *session, uint8_t commit_it) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.close_collective(commit_it != 0));
}

// ===========================================================================
// Schema
// ===========================================================================

int32_t kimix_gdb_label_create(kimix_gdb_session *session, const char *name, size_t name_len,
                               uint32_t *out_label) {
    if (session == nullptr || name == nullptr || out_label == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    LabelId id = 0;
    GdiStatus const status = session->database.create_label(
        kimix::string_view{name, name_len}, id);
    if (is_error(status)) {
        return code_of(status);
    }
    *out_label = id;
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_label_find(kimix_gdb_session *session, const char *name, size_t name_len,
                             uint32_t *out_label, uint8_t *out_found) {
    if (session == nullptr || name == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    LabelId id = 0;
    bool found = false;
    GdiStatus const status = session->database.get_label_by_name(
        kimix::string_view{name, name_len}, id, found);
    if (is_error(status)) {
        return code_of(status);
    }
    if (out_found != nullptr) {
        *out_found = found ? 1 : 0;
    }
    if (found && out_label != nullptr) {
        *out_label = id;
    }
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_label_get_all(kimix_gdb_session *session, kimix_gdb_label *items,
                                size_t capacity, uint8_t *scratch, size_t scratch_capacity,
                                size_t *out_count, size_t *out_scratch_bytes) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
      kimix::graph_db::NamedLabelList labels;
      GdiStatus status = session->database.get_all_labels(labels);
      if (is_error(status)) {
          return code_of(status);
      }
      // PHASE 1 (both buffers NULL): sizes only, success — the documented probe.
      if (items == nullptr && scratch == nullptr) {
          size_t needed = 0;
          for (auto const &label : labels) {
              needed += label.name.size();
          }
          if (out_count != nullptr) {
              *out_count = labels.size();
          }
          if (out_scratch_bytes != nullptr) {
              *out_scratch_bytes = needed;
          }
          return KIMIX_GDB_OK;
      }
      ScratchWriter writer{scratch, scratch_capacity};
      if (out_count != nullptr) {
          *out_count = labels.size();
      }
      for (size_t i = 0; i < labels.size(); ++i) {
          uint32_t offset = 0;
          uint32_t length = 0;
          if (!writer.append_text(labels[i].name, offset, length)) {
              status = GdiStatus::error_buffer;
          }
          if (items != nullptr && i < capacity) {
              items[i].id = labels[i].id;
              items[i].name_offset = offset;
              items[i].name_len = length;
          }
      }
    if (out_scratch_bytes != nullptr) {
        *out_scratch_bytes = writer.needed();
    }
    if (status == GdiStatus::ok && items != nullptr && labels.size() > capacity) {
        status = GdiStatus::error_buffer;
    }
    return code_of(status);
}

int32_t kimix_gdb_label_update(kimix_gdb_session *session, uint32_t label, const char *name,
                               size_t name_len) {
    if (session == nullptr || name == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.update_label(label, kimix::string_view{name, name_len}));
}

int32_t kimix_gdb_label_free(kimix_gdb_session *session, uint32_t label) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.free_label(label));
}

int32_t kimix_gdb_label_name(kimix_gdb_session *session, uint32_t label, char *out,
                             size_t capacity, size_t *out_required) {
    if (session == nullptr || out_required == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::string const name = session->database.schema().label_name(label);
    *out_required = name.size();
    if (out == nullptr || capacity < name.size()) {
        return KIMIX_GDB_ERR_BUFFER;
    }
    if (!name.empty()) {
        std::memcpy(out, name.data(), name.size());
    }
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_property_type_create(kimix_gdb_session *session, const char *name,
                                       size_t name_len, uint8_t container, uint8_t dtype,
                                       uint8_t size_type, uint32_t count, uint32_t *out_type) {
    if (session == nullptr || name == nullptr || out_type == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::graph_db::PropertyTypeInfo info;
    info.name = kimix::string{kimix::string_view{name, name_len}};
    info.container = container == KIMIX_GDB_CONTAINER_ARRAY
                         ? kimix::graph_db::GdiContainerType::array
                         : kimix::graph_db::GdiContainerType::single;
    info.data = static_cast<kimix::graph_db::GdiDataType>(dtype);
    // ABI 8: the size type is an input. An unknown value is refused here, before the
    // schema sees it, so the error is the ABI's argument code either way (the schema
    // applies the same rule for a caller that builds a PropertyTypeInfo directly).
    info.size_type = static_cast<kimix::graph_db::GdiSizeType>(size_type);
    if (!kimix::graph_db::is_valid_size_type(info.size_type)) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    info.count = count;
    PropNameId id = 0;
    GdiStatus const status = session->database.create_property_type(info, id);
    if (is_error(status)) {
        return code_of(status);
    }
    *out_type = id;
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_property_type_find(kimix_gdb_session *session, const char *name,
                                     size_t name_len, uint32_t *out_type, uint8_t *out_found) {
    if (session == nullptr || name == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    PropNameId id = 0;
    bool found = false;
    GdiStatus const status = session->database.get_property_type_by_name(
        kimix::string_view{name, name_len}, id, found);
    if (is_error(status)) {
        return code_of(status);
    }
    if (out_found != nullptr) {
        *out_found = found ? 1 : 0;
    }
    if (found && out_type != nullptr) {
        *out_type = id;
    }
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_property_type_get_all(kimix_gdb_session *session,
                                        kimix_gdb_property_type *items, size_t capacity,
                                        uint8_t *scratch, size_t scratch_capacity,
                                        size_t *out_count, size_t *out_scratch_bytes) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
      kimix::graph_db::PropertyTypeList types;
      GdiStatus status = session->database.get_all_property_types(types);
      if (is_error(status)) {
          return code_of(status);
      }
      // PHASE 1 (both buffers NULL): sizes only, success — the documented probe.
      if (items == nullptr && scratch == nullptr) {
          size_t needed = 0;
          for (auto const &type : types) {
              needed += type.name.size();
          }
          if (out_count != nullptr) {
              *out_count = types.size();
          }
          if (out_scratch_bytes != nullptr) {
              *out_scratch_bytes = needed;
          }
          return KIMIX_GDB_OK;
      }
      ScratchWriter writer{scratch, scratch_capacity};
      if (out_count != nullptr) {
          *out_count = types.size();
      }
      for (size_t i = 0; i < types.size(); ++i) {
          uint32_t offset = 0;
          uint32_t length = 0;
          if (!writer.append_text(types[i].name, offset, length)) {
              status = GdiStatus::error_buffer;
          }
          if (items != nullptr && i < capacity) {
              items[i].id = types[i].name_id;
              items[i].container = static_cast<uint8_t>(types[i].container);
              items[i].dtype = static_cast<uint8_t>(types[i].data);
              // ABI 8: the size type occupies the byte an ABI-7 image left zero.
              items[i].size_type = static_cast<uint8_t>(types[i].size_type);
              for (size_t r = 0; r < sizeof(items[i].reserved); ++r) {
                  items[i].reserved[r] = 0;
              }
              items[i].count = types[i].count;
              items[i].name_offset = offset;
              items[i].name_len = length;
          }
      }
      if (out_scratch_bytes != nullptr) {
          *out_scratch_bytes = writer.needed();
      }
      if (status == GdiStatus::ok && items != nullptr && types.size() > capacity) {
          status = GdiStatus::error_buffer;
      }
      return code_of(status);
  }

int32_t kimix_gdb_property_type_update(kimix_gdb_session *session, uint32_t type,
                                       const char *name, size_t name_len, uint8_t container,
                                       uint8_t dtype, uint8_t size_type, uint32_t count) {
    if (session == nullptr || name == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::graph_db::PropertyTypeInfo info;
    info.name = kimix::string{kimix::string_view{name, name_len}};
    info.container = container == KIMIX_GDB_CONTAINER_ARRAY
                         ? kimix::graph_db::GdiContainerType::array
                         : kimix::graph_db::GdiContainerType::single;
    info.data = static_cast<kimix::graph_db::GdiDataType>(dtype);
    info.size_type = static_cast<kimix::graph_db::GdiSizeType>(size_type);
    if (!kimix::graph_db::is_valid_size_type(info.size_type)) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    info.count = count;
    info.name_id = type;
    return code_of(session->database.update_property_type(type, info));
}

int32_t kimix_gdb_property_type_free(kimix_gdb_session *session, uint32_t type) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.free_property_type(type));
}

// ===========================================================================
// Constraints
// ===========================================================================

int32_t kimix_gdb_constraint_create(kimix_gdb_session *session, uint32_t *out_constraint) {
    if (session == nullptr || out_constraint == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    if (session->constraints.size() >= 256) {
        return KIMIX_GDB_ERR_SIZE_LIMIT; // a bounded registry, by design
    }
    ConstraintHandle handle;
    GdiStatus const status = session->database.create_constraint(Constraint{}, handle);
    if (is_error(status)) {
        return code_of(status);
    }
    ConstraintSlot slot;
    slot.handle = handle;
    slot.definition = Constraint{};
    session->constraints.push_back(std::move(slot));
    *out_constraint = handle.id;
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_constraint_add_label(kimix_gdb_session *session, uint32_t constraint,
                                       uint32_t label, uint8_t op) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    ConstraintSlot *slot = find_slot(session, constraint);
    if (slot == nullptr) {
        return KIMIX_GDB_ERR_CONSTRAINT;
    }
    GdiStatus const status =
        session->database.add_label_condition(slot->handle, label, static_cast<GdiOp>(op));
    if (is_error(status)) {
        return code_of(status);
    }
    // Keep the session's copy in step: the read commands resolve the DEFINITION
    // (the Database owns the registry, the ABI owns the mapping to its ids).
    Constraint current;
    if (session->database.schema().constraint(slot->handle, current)) {
        slot->definition = current;
    }
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_constraint_add_property(kimix_gdb_session *session, uint32_t constraint,
                                          uint32_t property, uint8_t op,
                                          const kimix_gdb_property *value, uint32_t count) {
    if (session == nullptr || value == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    ConstraintSlot *slot = find_slot(session, constraint);
    if (slot == nullptr) {
        return KIMIX_GDB_ERR_CONSTRAINT;
    }
    // The set an ANY_IN condition carries is a property value like any other, so it
    // goes through the same decode and the same block guard; `count` is the caller's
    // element count (exact, uint32, §1.4) and Schema::add_property_condition validates
    // the op/value combination (a non-set value is error_op_datatype_mismatch).
    DecodeRefusal malformed = DecodeRefusal::ok;
    PropertyValue set = to_value(*value, &malformed);
    if (int32_t const refusal = malformed_status(malformed)) {
        return refusal;
    }
    GdiStatus const status = session->database.add_property_condition(
        slot->handle, property, static_cast<GdiOp>(op), std::move(set), count);
    if (is_error(status)) {
        return code_of(status);
    }
    Constraint current;
    if (session->database.schema().constraint(slot->handle, current)) {
        slot->definition = current;
    }
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_constraint_free(kimix_gdb_session *session, uint32_t constraint) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    for (size_t i = 0; i < session->constraints.size(); ++i) {
        if (session->constraints[i].handle.id != constraint) {
            continue;
        }
        GdiStatus const status = session->database.free_constraint(session->constraints[i].handle);
        session->constraints.erase(session->constraints.begin() + static_cast<ptrdiff_t>(i));
        return code_of(status);
    }
    return KIMIX_GDB_ERR_CONSTRAINT;
}

// ===========================================================================
// Bulk
// ===========================================================================

int32_t kimix_gdb_vertices_create_bulk(kimix_gdb_session *session,
                                       const kimix_gdb_vertex_spec *specs, size_t count,
                                       uint8_t continue_on_error, kimix_gdb_guid *out_uids,
                                       int32_t *out_status, uint8_t *out_warning) {
    if (session == nullptr || (specs == nullptr && count != 0)) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::vector<kimix::graph_db::VertexBulkRow> rows;
    rows.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        kimix::graph_db::VertexBulkRow row;
        row.key.label = specs[i].key.label;
        row.key.external_id =
            kimix::graph_db::ExternalId::from_bytes(to_bytes(specs[i].key.external_id));
        for (size_t k = 0; k < specs[i].label_count; ++k) {
            row.labels.push_back(specs[i].labels[k]);
        }
        if (int32_t const refusal = named_properties(specs[i], row.properties)) {
      // A bad block anywhere in the batch refuses the WHOLE call
      // (plan §1.1: ERR_COUNT / ERR_DATATYPE, nothing stored): the rows are still being
            // assembled, so no entity has reached the Database and the batch stays
            // exactly as untouched as a refused single write.
            return refusal;
        }
        row.content = to_bytes(specs[i].content);
        rows.push_back(std::move(row));
    }
    kimix::vector<kimix::graph_db::VertexCreateResult> results;
    kimix::vector<GdiStatus> statuses;
    kimix::graph_db::BulkStats stats;
    GdiStatus const status = session->database.create_vertices_bulk(
        kimix::span<kimix::graph_db::VertexBulkRow const>{rows.data(), rows.size()},
        continue_on_error != 0, results, &statuses, stats);
    for (size_t i = 0; i < results.size() && i < count; ++i) {
        if (out_uids != nullptr) {
            out_uids[i] = from_guid(results[i].uid);
        }
        if (out_status != nullptr && i < statuses.size()) {
            out_status[i] = code_of(statuses[i]);
        }
        if (out_warning != nullptr) {
            out_warning[i] = results[i].non_unique_id_warning ? 1 : 0;
        }
    }
    return is_error(status) ? code_of(status) : KIMIX_GDB_OK;
}

int32_t kimix_gdb_edges_create_bulk(kimix_gdb_session *session,
                                    const kimix_gdb_edge_spec *specs, size_t count,
                                    uint8_t continue_on_error, kimix_gdb_guid *out_uids,
                                    int32_t *out_status) {
    if (session == nullptr || (specs == nullptr && count != 0)) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::vector<kimix::graph_db::EdgeBulkRow> rows;
    rows.reserve(count);
 for (size_t i = 0; i < count; ++i) {
 kimix::graph_db::EdgeBulkRow row;
 row.origin = to_guid(specs[i].origin);
 row.target = to_guid(specs[i].target);
 row.direction = specs[i].direction == KIMIX_GDB_EDGE_UNDIRECTED
 ? EdgeDirectionType::undirected
 : EdgeDirectionType::directed;
 // ABI 8: an all-zero uid is the "generate one" spelling (what a zero-initialized
 // spec carries), anything else is the caller's chosen uid for that row.
 row.uid = to_guid(specs[i].uid);
 row.has_uid = !is_zero_guid(row.uid);
 for (size_t k = 0; k < specs[i].label_count; ++k) {
 row.labels.push_back(specs[i].labels[k]);
 }
        if (int32_t const refusal = named_properties(specs[i], row.properties)) {
            return refusal; // the vertex bulk's rule, applied unchanged
        }
        rows.push_back(std::move(row));
    }
    kimix::vector<kimix::Guid> edge_ids;
    kimix::vector<GdiStatus> statuses;
    kimix::graph_db::BulkStats stats;
    GdiStatus const status = session->database.create_edges_bulk(
        kimix::span<kimix::graph_db::EdgeBulkRow const>{rows.data(), rows.size()},
        continue_on_error != 0, edge_ids, &statuses, stats);
    for (size_t i = 0; i < edge_ids.size() && i < count; ++i) {
        if (out_uids != nullptr) {
            out_uids[i] = from_guid(edge_ids[i]);
        }
        if (out_status != nullptr && i < statuses.size()) {
            out_status[i] = code_of(statuses[i]);
        }
    }
    return is_error(status) ? code_of(status) : KIMIX_GDB_OK;
}

} // extern "C"
