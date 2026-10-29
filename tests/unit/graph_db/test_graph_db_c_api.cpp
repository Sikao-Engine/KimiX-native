// Test for the C ABI (src/graph_db/graph_c_api.h) — GDI §2.13.
//
// This suite includes ONLY the C header (like tests/unit/api/test_kimix_api.cpp):
// it is what a Pentest-style foreign-language binding sees, so nothing of the C++
// surface is allowed to leak into it.
//
// This test covers:
// - the ABI/layout contract: abi_version matches the macro, every struct size is
//   pinned, sizeof(size_t) is the pointer width;
// - the session lifecycle: open/close, a second close, NULL-argument rejection;
// - status names: every code round-trips, and an unknown one answers
//   "status_unknown" instead of NULL;
// - a full command walk: schema -> keyed vertices -> edges -> labels/properties ->
//   constraint-filtered paged reads through the TWO-PHASE buffer protocol (NULL
//   first to learn the size, then the real fetch, then a deliberately too-small
//   buffer that must report ERR_BUFFER with the needed size) -> a transaction
//   abort and commit -> persist -> reopen and read the same data back;
// - a create/free loop, to prove the session owns no per-call allocation it
//   forgets (the only owned object is the session itself).
#include "ut/ut.hpp"

#include "graph_db_test_util.h"

#include <core/kimix_core.h> // umbrella first: it fixes the winsock2.h order
#include <core/fiber.h>

// The ONLY include from the graph_db library: the C header.
#include <graph_db/graph_c_api.h>

#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;
namespace gtest = kimix::graph_db::test;

namespace tests_unit_graph_db_test_graph_db_c_api {

// Small helpers that make the C surface readable in a test.
kimix_gdb_bytes bytes_of(char const *text) {
    kimix_gdb_bytes out;
    out.data = reinterpret_cast<uint8_t const *>(text);
    out.len = std::strlen(text);
    return out;
}

kimix_gdb_property int_property(int64_t value) {
    kimix_gdb_property property{};
    property.kind = KIMIX_GDB_KIND_INT64;
    property.dtype = KIMIX_GDB_DTYPE_INT64;
    property.count = 1;
    property.i64 = value;
    return property;
}

kimix_gdb_property text_property(char const *text) {
kimix_gdb_property property{};
property.kind = KIMIX_GDB_KIND_STRING;
property.dtype = KIMIX_GDB_DTYPE_CHAR;
property.count = 1;
property.payload = bytes_of(text);
return property;
}

kimix_gdb_property blob_property(uint8_t const *value, size_t len) {
kimix_gdb_property property{};
property.kind = KIMIX_GDB_KIND_BLOB;
property.dtype = KIMIX_GDB_DTYPE_BYTE;
property.count = 1;
property.payload.data = value;
property.payload.len = len;
return property;
}

// One open in-memory session; the guard removes it even on an early return.
struct session_guard {
    kimix_gdb_session *session{nullptr};

    session_guard() {
        kimix_gdb_open_options options{};
        options.directory = nullptr; // in memory
        options.auto_create_schema = 1;
        options.allow_parallel_edges = 1;
        options.allow_self_edges = 1;
        options.max_journal_bytes = 64ull * 1024ull * 1024ull;
        expect(eq(kimix_gdb_open(&options, &session), KIMIX_GDB_OK));
    }
    session_guard(session_guard const &) = delete;
    session_guard &operator=(session_guard const &) = delete;
    ~session_guard() {
        if (session != nullptr) {
            kimix_gdb_close(session);
        }
    }

    // A registered label by name.
    uint32_t label(char const *name) {
        uint32_t id = 0;
        expect(eq(kimix_gdb_label_create(session, name, std::strlen(name), &id), KIMIX_GDB_OK));
        return id;
    }

    uint32_t property_type(char const *name, uint8_t dtype) {
        uint32_t id = 0;
        expect(eq(kimix_gdb_property_type_create(session, name, std::strlen(name),
                                                 KIMIX_GDB_CONTAINER_SINGLE, dtype, KIMIX_GDB_SIZE_FIXED, 1, &id),
                  KIMIX_GDB_OK));
        return id;
    }

    // An ARRAY declaration (container = array + the exact element count): the shape a
    // string_array or int64_array value must satisfy. One char_ declaration serves
    // String and StringArray, distinguished by the container (plan §1.1).
    uint32_t property_type_array(char const *name, uint8_t dtype, uint32_t count) {
        uint32_t id = 0;
        expect(eq(kimix_gdb_property_type_create(session, name, std::strlen(name),
                                                 KIMIX_GDB_CONTAINER_ARRAY, dtype, KIMIX_GDB_SIZE_FIXED, count, &id),
                  KIMIX_GDB_OK));
        return id;
    }

    // An ARRAY declaration with an explicit SIZE TYPE (ABI 8) — what a
    // variable-length data-array property needs (max / unlimited).
    uint32_t property_type_array_sized(char const *name, uint8_t dtype, uint8_t size_type,
                                       uint32_t count) {
        uint32_t id = 0;
        expect(eq(kimix_gdb_property_type_create(session, name, std::strlen(name),
                                                 KIMIX_GDB_CONTAINER_ARRAY, dtype, size_type,
                                                 count, &id),
                  KIMIX_GDB_OK));
        return id;
    }

    kimix_gdb_guid vertex(uint32_t label_id, char const *external) {
        kimix_gdb_vertex_spec spec{};
        spec.key.label = label_id;
        spec.key.external_id = bytes_of(external);
        kimix_gdb_guid uid{};
        uint8_t warning = 0;
        expect(eq(kimix_gdb_vertex_create(session, &spec, &uid, &warning), KIMIX_GDB_OK));
        return uid;
    }

    kimix_gdb_guid edge(kimix_gdb_guid origin, kimix_gdb_guid target) {
        kimix_gdb_guid id{};
        expect(eq(kimix_gdb_edge_create(session, origin, target, KIMIX_GDB_EDGE_DIRECTED, nullptr,
                                        0, nullptr, nullptr, 0, nullptr, &id),
                  KIMIX_GDB_OK));
        return id;
    }

    // One edge carrying a single registered label right at creation (what the
    // edge-filtered path cases assert on).
    kimix_gdb_guid edge_labeled(kimix_gdb_guid origin, kimix_gdb_guid target,
                                uint32_t label_id) {
        kimix_gdb_guid id{};
        expect(eq(kimix_gdb_edge_create(session, origin, target, KIMIX_GDB_EDGE_DIRECTED,
                                        &label_id, 1, nullptr, nullptr, 0, nullptr, &id),
                  KIMIX_GDB_OK));
        return id;
    }
};


// ---------------------------------------------------------------------------
// string-array fixtures (ABI 7, plan §1.1). This suite includes ONLY the C header,
// so the block is spelled and parsed byte by byte here: an independent test-side
// encoder/decoder is the point — a bug in the library's own block handling must show
// up as a failing round trip, not as a shared-assumption pass.
// ---------------------------------------------------------------------------
void push_u32(kimix::vector<uint8_t> &block, uint32_t value) {
    block.push_back(static_cast<uint8_t>(value & 0xffu));
    block.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
    block.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
    block.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
}

uint32_t pop_u32(uint8_t const *at) {
    return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) |
           (static_cast<uint32_t>(at[2]) << 16) | (static_cast<uint32_t>(at[3]) << 24);
}

// [u32 element_count]{ [u32 byte_len][UTF-8 bytes] }* — little-endian, no padding:
// the ONE layout the ABI payload and the record codec share.
kimix::vector<uint8_t> string_array_block(std::initializer_list<char const *> elements) {
    kimix::vector<uint8_t> block;
    push_u32(block, static_cast<uint32_t>(elements.size()));
    for (char const *text : elements) {
        uint32_t const len = static_cast<uint32_t>(std::strlen(text));
        push_u32(block, len);
        for (uint32_t i = 0; i < len; ++i) {
            block.push_back(static_cast<uint8_t>(text[i]));
        }
    }
    return block;
}

// The same layout with generated ids: the ~2,000-element ANY_IN set leg.
kimix::vector<uint8_t> string_array_block_sequence(uint32_t count) {
    kimix::vector<uint8_t> block;
    push_u32(block, count);
    for (uint32_t i = 0; i < count; ++i) {
        kimix::string const text = kimix::format("id-{:04}", i);
        push_u32(block, static_cast<uint32_t>(text.size()));
        for (char c : text) {
            block.push_back(static_cast<uint8_t>(c));
        }
    }
    return block;
}

// A KIND_STRING_ARRAY input whose payload borrows `block`. `count` is the EXACT
// element count (§1.4); a test that wants an inconsistent input passes a different
// number and expects the refusal.
kimix_gdb_property string_array_property(kimix::vector<uint8_t> const &block, uint32_t count) {
    kimix_gdb_property property{};
    property.kind = KIMIX_GDB_KIND_STRING_ARRAY;
    property.dtype = KIMIX_GDB_DTYPE_CHAR;
    property.count = count;
    property.payload.data = block.data();
    property.payload.len = block.size();
    return property;
}

// An int64_array value: little-endian 8-byte elements, count exact (§1.4).
kimix_gdb_property int64_array_property(kimix::vector<int64_t> const &values) {
    kimix_gdb_property property{};
    property.kind = KIMIX_GDB_KIND_INT64_ARRAY;
    property.dtype = KIMIX_GDB_DTYPE_INT64;
    property.count = static_cast<uint32_t>(values.size());
    property.payload.data = reinterpret_cast<uint8_t const *>(values.data());
    property.payload.len = values.size() * 8;
    return property;
}

// ---------------------------------------------------------------------------
// data-array fixtures (ABI 9). Same discipline as the string-array ones: this
// suite includes ONLY the C header, so the block is spelled byte by byte here and
// every round trip is checked against bytes this file computed itself.
// ---------------------------------------------------------------------------

// [u8 flag][u8 reserved[3]] [u32 count] — the 8-byte header, nothing else.
kimix::vector<uint8_t> data_array_header(uint8_t flag, uint32_t count) {
    kimix::vector<uint8_t> block;
    block.push_back(flag);
    block.push_back(0);
    block.push_back(0);
    block.push_back(0);
    push_u32(block, count);
    return block;
}

void push_u64(kimix::vector<uint8_t> &block, uint64_t value) {
    for (uint32_t shift = 0; shift < 64; shift += 8) {
        block.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
    }
}

kimix::vector<uint8_t> data_array_block_int64(std::initializer_list<int64_t> elements) {
    auto block = data_array_header(KIMIX_GDB_DATA_INT64, static_cast<uint32_t>(elements.size()));
    for (int64_t const element : elements) {
        push_u64(block, static_cast<uint64_t>(element));
    }
    return block;
}

kimix::vector<uint8_t> data_array_block_double(std::initializer_list<double> elements) {
    auto block = data_array_header(KIMIX_GDB_DATA_DOUBLE, static_cast<uint32_t>(elements.size()));
    for (double const element : elements) {
        uint64_t bits = 0;
        std::memcpy(&bits, &element, sizeof(bits));
        push_u64(block, bits);
    }
    return block;
}

kimix::vector<uint8_t> data_array_block_string(std::initializer_list<char const *> elements) {
    auto block = data_array_header(KIMIX_GDB_DATA_STRING, static_cast<uint32_t>(elements.size()));
    for (char const *text : elements) {
        uint32_t const len = static_cast<uint32_t>(std::strlen(text));
        push_u32(block, len);
        for (uint32_t i = 0; i < len; ++i) {
            block.push_back(static_cast<uint8_t>(text[i]));
        }
    }
    return block;
}

// A KIND_DATA_ARRAY input whose payload borrows `block`. `count` is the EXACT
// element count (D6); a test that wants an inconsistent input passes another number.
kimix_gdb_property data_array_property(kimix::vector<uint8_t> const &block, uint32_t count) {
    kimix_gdb_property property{};
    property.kind = KIMIX_GDB_KIND_DATA_ARRAY;
    property.dtype = KIMIX_GDB_DTYPE_DATA;
    property.count = count;
    property.payload.data = block.data();
    property.payload.len = block.size();
    return property;
}

// The elements a block carries, decoded independently of the library.
kimix::vector<kimix::string> string_array_elements(uint8_t const *data, size_t bytes) {
    kimix::vector<kimix::string> out;
    if (data == nullptr || bytes < 4) {
        return out;
    }
    uint32_t const count = pop_u32(data);
    size_t at = 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (at + 4 > bytes) {
            break; // a malformed tail: whatever decoded so far is the answer
        }
        uint32_t const len = pop_u32(data + at);
        at += 4;
        if (at + len > bytes) {
            break;
        }
        out.emplace_back(reinterpret_cast<char const *>(data + at), len);
        at += len;
    }
    return out;
}

// One two-phase property read, done the documented way: probe the sizes with NULL
// buffers, then fetch into caller storage.
struct property_read {
    kimix::vector<kimix_gdb_property_record> items;
    kimix::vector<uint8_t> scratch;
    size_t count{0};
    size_t scratch_bytes{0};

    [[nodiscard]] kimix_gdb_property_record const *find(uint32_t name) const {
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].name_id == name) {
                return &items[i];
            }
        }
        return nullptr;
    }
};

template <typename Read>
property_read read_properties(Read &&read) {
    property_read out;
    expect(eq(read(nullptr, 0, nullptr, 0, &out.count, &out.scratch_bytes), KIMIX_GDB_OK))
        << "the size probe succeeds with NULL buffers";
    out.items.resize(out.count);
    out.scratch.resize(out.scratch_bytes);
    size_t count = 0;
    size_t scratch_bytes = 0;
    expect(eq(read(out.items.data(), out.items.size(), out.scratch.data(), out.scratch.size(),
                   &count, &scratch_bytes),
              KIMIX_GDB_OK));
    expect(eq(count, out.count));
    expect(eq(scratch_bytes, out.scratch_bytes));
    return out;
}

// One data_array record, verified against the block it was written from: the kind,
// the datatype sentinel, the declared element count, a byte-equal payload. The
// record's `count` field reports 1 for an EMPTY array (fill_property_record), while
// the BLOCK's own u32 reports 0 — readers take the count from the block.
void expect_data_array_record(kimix_gdb_property_record const *record,
                              property_read const &page,
                              kimix::vector<uint8_t> const &block, uint32_t elements,
                              kimix::string_view what) {
    expect(record != nullptr) << what;
    if (record == nullptr) {
        return;
    }
    expect(record->kind == KIMIX_GDB_KIND_DATA_ARRAY) << what;
    expect(record->dtype == KIMIX_GDB_DTYPE_DATA) << what << ": the `data` sentinel dtype";
    expect(eq(record->count, elements == 0 ? uint32_t{1} : elements))
        << what << ": the record's count (1 for an empty array)";
    expect(eq(static_cast<size_t>(record->payload_len), block.size()))
        << what << ": the payload is the whole block";
    if (static_cast<size_t>(record->payload_len) != block.size() ||
        static_cast<size_t>(record->payload_offset) + record->payload_len >
            page.scratch.size()) {
        return; // the expectations above already failed; never read out of range
    }
    uint8_t const *const back = page.scratch.data() + record->payload_offset;
    expect(std::memcmp(back, block.data(), block.size()) == 0)
        << what << ": the payload is byte-equal to the input block";
    if (block.size() >= 8) {
        expect(eq(pop_u32(back + 4), elements))
            << what << ": the block's own count is the element count";
    }
}

// One string_array record, verified against the block it was written from: the kind,
// the EXACT element count (§1.4), a byte-equal payload, and the same elements in the
// same order.
void expect_string_array_record(kimix_gdb_property_record const *record,
                                property_read const &page,
                                kimix::vector<uint8_t> const &block, uint32_t elements,
                                kimix::string_view what) {
    expect(record != nullptr) << what;
    if (record == nullptr) {
        return;
    }
    expect(record->kind == KIMIX_GDB_KIND_STRING_ARRAY) << what;
    expect(record->dtype == KIMIX_GDB_DTYPE_CHAR) << what << ": char_ + array";
    expect(eq(record->count, elements)) << what << ": the exact element count (§1.4)";
    expect(eq(static_cast<size_t>(record->payload_len), block.size()))
        << what << ": the payload is the whole block";
    if (static_cast<size_t>(record->payload_len) != block.size() ||
        static_cast<size_t>(record->payload_offset) + record->payload_len >
            page.scratch.size()) {
        return; // the expectations above already failed; never read out of range
    }
    uint8_t const *const back = page.scratch.data() + record->payload_offset;
    expect(std::memcmp(back, block.data(), block.size()) == 0)
        << what << ": the payload is byte-equal to the input block";
    auto const got = string_array_elements(back, record->payload_len);
    auto const wanted = string_array_elements(block.data(), block.size());
    expect(eq(got.size(), wanted.size())) << what;
    for (size_t i = 0; i < got.size() && i < wanted.size(); ++i) {
        expect(got[i] == wanted[i]) << what << ": element " << i << " keeps its order";
    }
}

// The uids of the edges one filtered read returns (count probe, then the fetch).
kimix::vector<kimix_gdb_guid> edge_uids(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                        uint32_t constraint, uint8_t orientation,
                                        size_t &out_total) {
    size_t count = 0;
    size_t total = 0;
    expect(eq(kimix_gdb_edges_of_vertex(session, vertex, constraint, orientation, 0, nullptr, 0,
                                        &count, &total),
              KIMIX_GDB_OK));
    out_total = total;
    kimix::vector<kimix_gdb_edge_record> records(total);
    if (total != 0) {
        expect(eq(kimix_gdb_edges_of_vertex(session, vertex, constraint, orientation,
                                            static_cast<int64_t>(total), records.data(),
                                            records.size(), &count, &total),
                  KIMIX_GDB_OK));
        expect(eq(count, total));
    }
    kimix::vector<kimix_gdb_guid> uids;
    for (auto const &record : records) {
        uids.push_back(record.uid);
    }
    return uids;
}

} // namespace tests_unit_graph_db_test_graph_db_c_api
using namespace tests_unit_graph_db_test_graph_db_c_api;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    kimix::fiber::scheduler sched{4u};

    "cabi_version_and_layout"_test = [] {
        expect(eq(kimix_gdb_abi_version(), KIMIX_GDB_ABI_VERSION));
        expect(kimix_gdb_version_string() != nullptr);
        expect(std::strlen(kimix_gdb_version_string()) > 0);

        kimix_gdb_layout layout{};
        expect(eq(kimix_gdb_layout_info(&layout), KIMIX_GDB_OK));
        expect(eq(layout.abi_version, KIMIX_GDB_ABI_VERSION));
        expect(eq(layout.sizeof_property, static_cast<uint32_t>(sizeof(kimix_gdb_property))));
        expect(eq(layout.sizeof_property_record,
                  static_cast<uint32_t>(sizeof(kimix_gdb_property_record))));
        expect(eq(layout.sizeof_vertex_key, static_cast<uint32_t>(sizeof(kimix_gdb_vertex_key))));
        expect(eq(layout.sizeof_edge_record,
                  static_cast<uint32_t>(sizeof(kimix_gdb_edge_record))));
        expect(eq(layout.sizeof_property_type,
                  static_cast<uint32_t>(sizeof(kimix_gdb_property_type))));
        expect(eq(layout.sizeof_capability, static_cast<uint32_t>(sizeof(kimix_gdb_capability))));
        expect(eq(layout.sizeof_vertex_spec,
                  static_cast<uint32_t>(sizeof(kimix_gdb_vertex_spec))));
        expect(eq(layout.sizeof_size_t, static_cast<uint32_t>(sizeof(size_t))));
        expect(eq(layout.alignof_guid, static_cast<uint32_t>(alignof(kimix_gdb_guid))));
        expect(eq(layout.sizeof_size_t, uint32_t{8})) << "the ABI is 64-bit only";
 expect(eq(layout.sizeof_traversal_vertex,
 static_cast<uint32_t>(sizeof(kimix_gdb_traversal_vertex))));
 expect(eq(layout.sizeof_aggregate_result,
 static_cast<uint32_t>(sizeof(kimix_gdb_aggregate_result))));
        // ABI 7's two trailing layout fields: the edge-filtered BFS records. The
        // header is frozen (step 0), so both sizes are pinned against the library's
        // report AND the C struct — a reordering or a widened field fails here before
        // a binding can be built against a different layout. (The plan annotated
        // kimix_gdb_bfs_edge as 56 B; the frozen struct is {edge, origin, target} =
        // 3 x 16 B = 48 B — the header is the contract, so 48 is what is pinned.)
        static_assert(sizeof(kimix_gdb_bfs_vertex) == 40,
                      "kimix_gdb_bfs_vertex is {vertex, edge, depth, reserved} = 40 B");
        static_assert(sizeof(kimix_gdb_bfs_edge) == 48,
                      "kimix_gdb_bfs_edge is {edge, origin, target} = 48 B");
        expect(eq(layout.sizeof_bfs_vertex, uint32_t{40}));
        expect(eq(layout.sizeof_bfs_edge, uint32_t{48}));
        expect(eq(layout.sizeof_bfs_vertex,
                  static_cast<uint32_t>(sizeof(kimix_gdb_bfs_vertex))));
        expect(eq(layout.sizeof_bfs_edge, static_cast<uint32_t>(sizeof(kimix_gdb_bfs_edge))));
        // NULL out-parameters are argument errors, never crashes.
        expect(eq(kimix_gdb_layout_info(nullptr), KIMIX_GDB_ERR_ARGUMENT));
    };

    "cabi_status_names"_test = [] {
        // Every code the header defines round-trips through status_name.
        int32_t const codes[] = {KIMIX_GDB_OK,
                                 KIMIX_GDB_WARNING_NON_UNIQUE_ID,
                                 KIMIX_GDB_WARNING_OTHER,
                                 KIMIX_GDB_ERR_ARGUMENT,
                                 KIMIX_GDB_ERR_BUFFER,
                                 KIMIX_GDB_ERR_EDGE,
                                 KIMIX_GDB_ERR_EDGE_ORIENTATION,
                                 KIMIX_GDB_ERR_EMPTY_NAME,
                                 KIMIX_GDB_ERR_LABEL,
                                 KIMIX_GDB_ERR_NAME_EXISTS,
                                 KIMIX_GDB_ERR_NO_PROPERTY,
                                 KIMIX_GDB_ERR_NON_UNIQUE_ID,
                                 KIMIX_GDB_ERR_OP,
                                 KIMIX_GDB_ERR_PROPERTY_EXISTS,
                                 KIMIX_GDB_ERR_PROPERTY_TYPE,
                                 KIMIX_GDB_ERR_READ_ONLY_TRANSACTION,
                                 KIMIX_GDB_ERR_SIZE_LIMIT,
                                 KIMIX_GDB_ERR_TRANSACTION,
                                 KIMIX_GDB_ERR_UID,
                                 KIMIX_GDB_ERR_VERTEX,
                                 KIMIX_GDB_ERR_WRONG_TYPE,
                                 KIMIX_GDB_ERR_IO,
                                 KIMIX_GDB_ERR_UNKNOWN};
        for (auto code : codes) {
            char const *name = kimix_gdb_status_name(code);
            expect(name != nullptr);
            expect(std::strlen(name) > 0) << "code " << code;
        }
        expect(std::strcmp(kimix_gdb_status_name(KIMIX_GDB_OK), "ok") == 0);
        expect(std::strcmp(kimix_gdb_status_name(KIMIX_GDB_ERR_VERTEX), "error_vertex") == 0);
        expect(std::strcmp(kimix_gdb_status_name(999), "status_unknown") == 0);
    };

    "cabi_session_lifecycle"_test = [] {
        kimix_gdb_session *session = nullptr;
        kimix_gdb_open_options options{};
        options.max_journal_bytes = 1024 * 1024;
        expect(eq(kimix_gdb_open(&options, &session), KIMIX_GDB_OK));
        expect(session != nullptr);

        kimix_gdb_capability capability{};
        expect(eq(kimix_gdb_capabilities(session, &capability), KIMIX_GDB_OK));
        expect(capability.enforces_unique_external_ids == 1);
        expect(capability.supports_edge_by_stable_uid == 1);
        expect(capability.supports_edge_property_read == 1);
        expect(capability.supports_collective_transactions == 1);
 expect(capability.supports_global_scan == 1)
 << "G1: the global scan is implemented (plan v3 step 2.4)";
 expect(capability.supports_native_multi_hop == 1)
 << "G3: the native multi-hop traversal is implemented (ABI 2)";
 expect(capability.max_native_hop_depth ==
 static_cast<int32_t>(KIMIX_GDB_MAX_HOP_DEPTH));
 expect(capability.supports_global_aggregates == 1)
 << "G4: the global aggregates are implemented (plan v3 step 2.4)";

        kimix_gdb_close(session);
        // A NULL session is tolerated by close() and rejected by everything else.
        kimix_gdb_close(nullptr);
        kimix_gdb_guid uid{};
        expect(eq(kimix_gdb_vertex_free(nullptr, uid), KIMIX_GDB_ERR_ARGUMENT));
        expect(eq(kimix_gdb_capabilities(nullptr, &capability), KIMIX_GDB_ERR_ARGUMENT));
        expect(eq(kimix_gdb_open(nullptr, nullptr), KIMIX_GDB_ERR_ARGUMENT));
    };

    "cabi_command_walk"_test = [] {
        session_guard guard;
        kimix_gdb_session *session = guard.session;
        uint32_t const node = guard.label("node");
        uint32_t const link = guard.label("link");
        uint32_t const age = guard.property_type("age", KIMIX_GDB_DTYPE_INT64);

        kimix_gdb_guid const a = guard.vertex(node, "a");
        kimix_gdb_guid const b = guard.vertex(node, "b");
        kimix_gdb_guid const c = guard.vertex(node, "c");

        // Key -> uid translation.
        kimix_gdb_vertex_key key{};
        key.label = node;
        key.external_id = bytes_of("b");
        kimix_gdb_guid resolved{};
        uint8_t found = 0;
        expect(eq(kimix_gdb_vertex_translate(session, &key, &resolved, &found), KIMIX_GDB_OK));
        expect(found == 1);
        expect(resolved.lo == b.lo && resolved.hi == b.hi);
        key.external_id = bytes_of("nobody");
        expect(eq(kimix_gdb_vertex_translate(session, &key, &resolved, &found), KIMIX_GDB_OK));
        expect(found == 0) << "a miss is not an error";

        // Edges, a label and an int property.
        kimix_gdb_guid const ab = guard.edge(a, b);
        kimix_gdb_guid const ac = guard.edge(a, c);
        kimix_gdb_property const value = int_property(42);
        expect(eq(kimix_gdb_vertex_property_add(session, a, age, &value), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_vertex_property_add(session, a, age, &value),
                  KIMIX_GDB_ERR_PROPERTY_EXISTS));
        expect(eq(kimix_gdb_vertex_label_add(session, a, link), KIMIX_GDB_OK));

        // Property read-back, TWO-PHASE: the size probe first.
        size_t count = 0;
        size_t scratch_bytes = 0;
        expect(eq(kimix_gdb_vertex_properties(session, a, nullptr, 0, nullptr, 0, &count,
                                              &scratch_bytes),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{1}));
        kimix::vector<kimix_gdb_property_record> items(count + 1);
        kimix::vector<uint8_t> scratch(scratch_bytes + 8);
        expect(eq(kimix_gdb_vertex_properties(session, a, items.data(), items.size(),
                                              scratch.data(), scratch.size(), &count,
                                              &scratch_bytes),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{1}));
        expect(eq(items[0].name_id, age));
        expect(items[0].kind == KIMIX_GDB_KIND_INT64);
        expect(eq(items[0].i64, int64_t{42}));

        // A deliberately too-small item array reports the need, not a crash.
        expect(eq(kimix_gdb_vertex_properties(session, a, nullptr, 0, nullptr, 0, &count,
                                              &scratch_bytes),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{1}));

        // Labels: the two-phase shape, with the key label first.
        size_t label_count = 0;
        expect(eq(kimix_gdb_vertex_labels(session, a, nullptr, 0, &label_count), KIMIX_GDB_OK));
        expect(eq(label_count, size_t{2})) << "the key label `node` and the added `link`";
        kimix::vector<uint32_t> labels(label_count);
        expect(eq(kimix_gdb_vertex_labels(session, a, labels.data(), labels.size(), &label_count),
                  KIMIX_GDB_OK));
        expect(eq(labels[0], node)) << "the key label comes first";

        // Edge read: count probe, then a page of two.
        size_t total = 0;
        expect(eq(kimix_gdb_edges_of_vertex(session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 0,
                                            nullptr, 0, &count, &total),
                  KIMIX_GDB_OK));
        expect(eq(total, size_t{2}));
        kimix::vector<kimix_gdb_edge_record> edges(2);
        expect(eq(kimix_gdb_edges_of_vertex(session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 2,
                                            edges.data(), edges.size(), &count, &total),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{2}));
        expect(eq(total, size_t{2}));
        for (auto const &record : edges) {
            expect(record.origin.lo == a.lo && record.origin.hi == a.hi);
        }
        // cap 1 of 2: one item, still the full total.
        expect(eq(kimix_gdb_edges_of_vertex(session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 1,
                                            edges.data(), edges.size(), &count, &total),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{1}));
        expect(eq(total, size_t{2}));

        // A constraint that filters the NEIGHBOUR VERTICES by property.
        uint32_t constraint = 0;
        expect(eq(kimix_gdb_constraint_create(session, &constraint), KIMIX_GDB_OK));
        kimix_gdb_property const expected = int_property(42);
        expect(eq(kimix_gdb_constraint_add_property(session, constraint, age,
                                                    KIMIX_GDB_OP_EQUAL, &expected, 1),
                  KIMIX_GDB_OK));
        expect(eq(kimix_gdb_count_neighbors(session, a, constraint, KIMIX_GDB_ORIENTATION_OUTGOING,
                                            &count),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{0})) << "b and c carry no `age` property";
        uint8_t exists = 1;
        expect(eq(kimix_gdb_exists_neighbor(session, a, constraint,
                                            KIMIX_GDB_ORIENTATION_OUTGOING, &exists),
                  KIMIX_GDB_OK));
        expect(exists == 0);
        expect(eq(kimix_gdb_constraint_free(session, constraint), KIMIX_GDB_OK));

        // Endpoints of one edge.
        kimix_gdb_guid origin{};
        kimix_gdb_guid target{};
        expect(eq(kimix_gdb_edge_endpoints(session, ab, &origin, &target), KIMIX_GDB_OK));
        expect(origin.lo == a.lo);
        expect(target.lo == b.lo);
        expect(eq(kimix_gdb_edge_free(session, ac), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_edge_free(session, ac), KIMIX_GDB_ERR_UID));

      // Guarded delete: `a` still has the ab edge.
      expect(eq(kimix_gdb_vertex_free_guarded(session, a), KIMIX_GDB_ERR_EDGE));
      expect(eq(kimix_gdb_edge_free(session, ab), KIMIX_GDB_OK));
      expect(eq(kimix_gdb_vertex_free_guarded(session, a), KIMIX_GDB_OK));
  };

  // The G3 multi-hop surface (ABI 2): a full two-phase walk of both calls —
  // probe -> fetch -> ERR_BUFFER, the constraint-filtered shape, and the
  // error surface (unknown endpoint, out-of-range bound, NULL arguments).
  "cabi_traverse_walk"_test = [] {
      session_guard guard;
      kimix_gdb_session *session = guard.session;
      uint32_t const node = guard.label("node");
      uint32_t const age = guard.property_type("age", KIMIX_GDB_DTYPE_INT64);
      // a(50) -> b(70) -> c(70);  a -> d(20).  Shortest a->c is a,b,c.
      kimix_gdb_guid const a = guard.vertex(node, "a");
      kimix_gdb_guid const b = guard.vertex(node, "b");
      kimix_gdb_guid const c = guard.vertex(node, "c");
      kimix_gdb_guid const d = guard.vertex(node, "d");
      kimix_gdb_property const age50 = int_property(50);
      kimix_gdb_property const age70 = int_property(70);
      kimix_gdb_property const age20 = int_property(20);
      expect(eq(kimix_gdb_vertex_property_add(session, a, age, &age50), KIMIX_GDB_OK));
      expect(eq(kimix_gdb_vertex_property_add(session, b, age, &age70), KIMIX_GDB_OK));
      expect(eq(kimix_gdb_vertex_property_add(session, c, age, &age70), KIMIX_GDB_OK));
      expect(eq(kimix_gdb_vertex_property_add(session, d, age, &age20), KIMIX_GDB_OK));
      guard.edge(a, b);
      guard.edge(b, c);
      guard.edge(a, d);

      // Unfiltered traverse_bfs from a, depth 2, generous cap: a + b,d + c.
      size_t count = 0;
      expect(eq(kimix_gdb_traverse_bfs(session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 100,
                                       2, nullptr, 0, &count),
                KIMIX_GDB_OK));
      expect(eq(count, size_t{4}));
      kimix_gdb_traversal_vertex walk[8]{};
      expect(eq(kimix_gdb_traverse_bfs(session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 100,
                                       2, walk, 8, &count),
                KIMIX_GDB_OK));
      expect(eq(count, size_t{4}));
      expect(walk[0].vertex.lo == a.lo && walk[0].depth == 0)
          << "items[0] is the start at depth 0";
      expect(walk[1].depth == 1u && walk[2].depth == 1u);
      expect(walk[3].depth == 2u);
      expect(walk[3].parent.lo == b.lo) << "c is discovered through b";
      // The too-small buffer reports the count and refuses.
      expect(eq(kimix_gdb_traverse_bfs(session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 100,
                                       2, walk, 1, &count),
                KIMIX_GDB_ERR_BUFFER));
      expect(eq(count, size_t{4}));
      // A cap of 1 keeps exactly one vertex per level.
      expect(eq(kimix_gdb_traverse_bfs(session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 1, 2,
                                       nullptr, 0, &count),
                KIMIX_GDB_OK));
      expect(eq(count, size_t{3})) << "start + 1 per level";

      // Constraint-filtered traversal: age >= 60 drops d at level 1, so its
      // (absent) children are never visited; only a, b, c remain.
      uint32_t seniors = 0;
      expect(eq(kimix_gdb_constraint_create(session, &seniors), KIMIX_GDB_OK));
      kimix_gdb_property const threshold = int_property(60);
      expect(eq(kimix_gdb_constraint_add_property(session, seniors, age,
                                                  KIMIX_GDB_OP_GREATER_OR_EQUAL, &threshold, 1),
                KIMIX_GDB_OK));
      expect(eq(kimix_gdb_traverse_bfs(session, a, seniors,
                                       KIMIX_GDB_ORIENTATION_OUTGOING, 100, 2, nullptr, 0,
                                       &count),
                KIMIX_GDB_OK));
      expect(eq(count, size_t{3}));

      // shortest_path: two-phase walk and the reachable/unreachable split.
      expect(eq(kimix_gdb_shortest_path(session, a, c, KIMIX_GDB_MAX_HOP_DEPTH, nullptr, 0,
                                        &count),
                KIMIX_GDB_OK));
      expect(eq(count, size_t{3}));
      kimix_gdb_guid path[8]{};
      expect(eq(kimix_gdb_shortest_path(session, a, c, KIMIX_GDB_MAX_HOP_DEPTH, path, 8,
                                        &count),
                KIMIX_GDB_OK));
      expect(eq(count, size_t{3}));
      expect(path[0].lo == a.lo && path[2].lo == c.lo && path[1].lo == b.lo)
          << "the path is a, b, c";
      expect(eq(kimix_gdb_shortest_path(session, a, c, 1, nullptr, 0, &count),
                KIMIX_GDB_OK));
      expect(eq(count, size_t{0}))
          << "unreachable within the bound is an empty result, not an error";
      // Errors: unknown endpoints, bad bounds, NULL arguments.
      kimix_gdb_guid const unknown{0xdeadbeefull, 0xfeedfaceull};
      expect(eq(kimix_gdb_shortest_path(session, a, unknown, 4, nullptr, 0, &count),
                KIMIX_GDB_ERR_VERTEX));
      expect(eq(kimix_gdb_shortest_path(session, a, c, 0, nullptr, 0, &count),
                KIMIX_GDB_ERR_ARGUMENT));
      expect(eq(kimix_gdb_shortest_path(session, a, c, KIMIX_GDB_MAX_HOP_DEPTH + 1,
                                        nullptr, 0, &count),
                KIMIX_GDB_ERR_ARGUMENT));
      expect(eq(kimix_gdb_shortest_path(nullptr, a, c, 4, nullptr, 0, &count),
                KIMIX_GDB_ERR_ARGUMENT));
      expect(eq(kimix_gdb_shortest_path(session, a, c, 4, nullptr, 0, nullptr),
                KIMIX_GDB_ERR_ARGUMENT));
      expect(eq(kimix_gdb_traverse_bfs(session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 0, 2,
                                       nullptr, 0, &count),
                KIMIX_GDB_ERR_ARGUMENT))
          << "a cap below 1 is refused";
      expect(eq(kimix_gdb_traverse_bfs(session, unknown, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                       10, 2, nullptr, 0, &count),
                KIMIX_GDB_ERR_VERTEX));
      expect(eq(kimix_gdb_traverse_bfs(nullptr, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 10,
                                       2, nullptr, 0, &count),
                KIMIX_GDB_ERR_ARGUMENT));
  };

// The EDGE-filtered shortest path (an additive surface after ABI 6): the
// two-phase walk of kimix_gdb_shortest_path_edges, the reported hop edges, the
// rerouting a label filter causes, the orientation knob and the error surface.
"cabi_shortest_path_edges"_test = [] {
    session_guard guard;
    kimix_gdb_session *session = guard.session;
    uint32_t const node = guard.label("node");
    uint32_t const open = guard.label("open_route");
    uint32_t const blocked = guard.label("blocked_route");
    // a -> b -> c is the short chain (b -> c blocked); a -> d -> c is the long
    // one (both open).  A filter that refuses `blocked` must pick a, d, c.
    kimix_gdb_guid const a = guard.vertex(node, "a");
    kimix_gdb_guid const b = guard.vertex(node, "b");
    kimix_gdb_guid const c = guard.vertex(node, "c");
    kimix_gdb_guid const d = guard.vertex(node, "d");
    kimix_gdb_guid const ab = guard.edge(a, b);
    kimix_gdb_guid const bc = guard.edge_labeled(b, c, blocked);
    kimix_gdb_guid const ad = guard.edge_labeled(a, d, open);
    kimix_gdb_guid const dc = guard.edge_labeled(d, c, open);

    // Probe phase: the unfiltered walk is the short chain a, b, c.
    size_t count = 0;
    expect(eq(kimix_gdb_shortest_path_edges(session, a, c, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                            KIMIX_GDB_MAX_HOP_DEPTH, nullptr, 0, &count),
              KIMIX_GDB_OK));
    expect(eq(count, size_t{3}));
    kimix_gdb_path_step path[8]{};
    expect(eq(kimix_gdb_shortest_path_edges(session, a, c, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                            KIMIX_GDB_MAX_HOP_DEPTH, path, 8, &count),
              KIMIX_GDB_OK));
    expect(eq(count, size_t{3}));
    expect(path[0].vertex.lo == a.lo && path[1].vertex.lo == b.lo && path[2].vertex.lo == c.lo)
        << "unfiltered: a, b, c";
    expect(path[0].edge.lo == 0ull && path[0].edge.hi == 0ull)
        << "the start is reached by no edge";
    expect(path[1].edge.lo == ab.lo && path[2].edge.lo == bc.lo)
        << "each step reports the edge it traversed";
    expect(path[1].depth == 1u && path[2].depth == 2u);

    // A blocking label filter reroutes the SAME endpoints onto the long chain.
    uint32_t blocked_only = 0;
    expect(eq(kimix_gdb_constraint_create(session, &blocked_only), KIMIX_GDB_OK));
    expect(eq(kimix_gdb_constraint_add_label(session, blocked_only, open, KIMIX_GDB_OP_EQUAL),
              KIMIX_GDB_OK));
    expect(eq(kimix_gdb_shortest_path_edges(session, a, c, blocked_only,
                                            KIMIX_GDB_ORIENTATION_OUTGOING,
                                            KIMIX_GDB_MAX_HOP_DEPTH, path, 8, &count),
              KIMIX_GDB_OK));
    expect(eq(count, size_t{3})) << "open edges only: a -> d -> c is still 2 hops";
    expect(path[1].vertex.lo == d.lo && path[2].vertex.lo == c.lo) << "the filter chose the route";
    expect(path[1].edge.lo == ad.lo && path[2].edge.lo == dc.lo);
    // Filtering on the OTHER label keeps the short chain and reports its edge.
    uint32_t blocked_routes = 0;
    expect(eq(kimix_gdb_constraint_create(session, &blocked_routes), KIMIX_GDB_OK));
    expect(eq(kimix_gdb_constraint_add_label(session, blocked_routes, blocked, KIMIX_GDB_OP_EQUAL),
              KIMIX_GDB_OK));
    expect(eq(kimix_gdb_shortest_path_edges(session, a, c, blocked_routes,
                                            KIMIX_GDB_ORIENTATION_OUTGOING,
                                            KIMIX_GDB_MAX_HOP_DEPTH, nullptr, 0, &count),
              KIMIX_GDB_OK));
    expect(eq(count, size_t{0}))
        << "a -> b carries no label, so the blocked-only walk never leaves a";

    // Orientation: walking BACKWARDS from c over `open` edges reaches d and a.
    expect(eq(kimix_gdb_shortest_path_edges(session, c, a, blocked_only,
                                            KIMIX_GDB_ORIENTATION_INCOMING,
                                            KIMIX_GDB_MAX_HOP_DEPTH, path, 8, &count),
              KIMIX_GDB_OK));
    expect(eq(count, size_t{3}));
    expect(path[1].vertex.lo == d.lo && path[2].vertex.lo == a.lo)
        << "incoming: c <- d <- a";
    expect(path[1].edge.lo == dc.lo && path[2].edge.lo == ad.lo);

    // Two-phase contract: a too-small buffer reports the count and refuses, and
    // the count probe (items == NULL) never needs storage.
    expect(eq(kimix_gdb_shortest_path_edges(session, a, c, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                            KIMIX_GDB_MAX_HOP_DEPTH, path, 2, &count),
              KIMIX_GDB_ERR_BUFFER));
    expect(eq(count, size_t{3})) << "the required size is still reported";
    // Unreachable within the bound, and unreachable over the matched edges, are
    // both OK with a zero count.
    expect(eq(kimix_gdb_shortest_path_edges(session, a, c, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 1,
                                            nullptr, 0, &count),
              KIMIX_GDB_OK));
    expect(eq(count, size_t{0}));

    // Error surface: NULL session / NULL out_count, unknown endpoints, bounds,
    // unknown constraint id, and a label condition with an ordering op.
    kimix_gdb_guid const unknown{0xdeadbeefull, 0xfeedfaceull};
    expect(eq(kimix_gdb_shortest_path_edges(nullptr, a, c, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                            KIMIX_GDB_MAX_HOP_DEPTH, nullptr, 0, &count),
              KIMIX_GDB_ERR_ARGUMENT));
    expect(eq(kimix_gdb_shortest_path_edges(session, a, c, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                            KIMIX_GDB_MAX_HOP_DEPTH, nullptr, 0, nullptr),
              KIMIX_GDB_ERR_ARGUMENT));
    expect(eq(kimix_gdb_shortest_path_edges(session, a, unknown, 0,
                                            KIMIX_GDB_ORIENTATION_OUTGOING,
                                            KIMIX_GDB_MAX_HOP_DEPTH, nullptr, 0, &count),
              KIMIX_GDB_ERR_VERTEX));
    expect(eq(kimix_gdb_shortest_path_edges(session, a, c, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 0,
                                            nullptr, 0, &count),
              KIMIX_GDB_ERR_ARGUMENT));
    expect(eq(kimix_gdb_shortest_path_edges(session, a, c, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                            KIMIX_GDB_MAX_HOP_DEPTH + 1, nullptr, 0, &count),
              KIMIX_GDB_ERR_ARGUMENT));
    expect(eq(kimix_gdb_shortest_path_edges(session, a, c, 424242u,
                                            KIMIX_GDB_ORIENTATION_OUTGOING,
                                            KIMIX_GDB_MAX_HOP_DEPTH, nullptr, 0, &count),
              KIMIX_GDB_ERR_CONSTRAINT));
    // A label condition can only test membership. This surface refuses an
    // ordering op when the condition is ADDED (the same rule the walk re-checks),
    // so the refusal never even reaches the walk through the C ABI.
    uint32_t ordering = 0;
    expect(eq(kimix_gdb_constraint_create(session, &ordering), KIMIX_GDB_OK));
    expect(eq(kimix_gdb_constraint_add_label(session, ordering, open, KIMIX_GDB_OP_GREATER),
              KIMIX_GDB_ERR_OP))
        << "a label condition can only test membership";
};

// The G1/G4 global surface (ABI 3): a full two-phase walk of both calls —
// scan probe -> fetch -> ERR_BUFFER -> paged caps, the constraint-filtered
// scan, the aggregate matrix (count/min/max/sum/avg, the string payload
// through scratch with its ERR_BUFFER), the legality errors, and the
// NULL-argument guards. The capability flags flipped in step 2.4 are
// mutation-checked above: a silent regression to false fails this suite.
"cabi_scan_and_aggregate_walk"_test = [] {
 session_guard guard;
 kimix_gdb_session *session = guard.session;
 uint32_t const node = guard.label("node");
 uint32_t const warm = guard.label("warm");
 uint32_t const age = guard.property_type("age", KIMIX_GDB_DTYPE_INT64);
 uint32_t const tag = guard.property_type("tag", KIMIX_GDB_DTYPE_CHAR);
 uint32_t const blob = guard.property_type("blob", KIMIX_GDB_DTYPE_BYTE);
 // n0..n9: age = i, tag = "t{i%3}", blob = {i%2, 0x7f}; even ones are warm.
 kimix_gdb_guid made[10]{};
 for (int i = 0; i < 10; ++i) {
 char ext[3] = {'n', static_cast<char>('0' + i), '\0'};
 made[i] = guard.vertex(node, ext);
 kimix_gdb_property const years = int_property(i);
 expect(eq(kimix_gdb_vertex_property_add(session, made[i], age, &years), KIMIX_GDB_OK));
 char tag_text[3] = {'t', static_cast<char>('0' + (i % 3)), '\0'};
 kimix_gdb_property const tag_value = text_property(tag_text);
 expect(eq(kimix_gdb_vertex_property_add(session, made[i], tag, &tag_value), KIMIX_GDB_OK));
 uint8_t const raw[2] = {static_cast<uint8_t>(i % 2), 0x7f};
 kimix_gdb_property const blob_value = blob_property(raw, 2);
 expect(eq(kimix_gdb_vertex_property_add(session, made[i], blob, &blob_value), KIMIX_GDB_OK));
 if (i % 2 == 0) {
 expect(eq(kimix_gdb_vertex_label_add(session, made[i], warm), KIMIX_GDB_OK));
 }
 }
 // Scan: the probe reports the full count, the fetch returns the guids, a
 // too-small buffer is ERR_BUFFER with the counts still reported, and a cap
 // pages the page (out_count follows the cap, out_total stays the full set).
 size_t count = 0;
 size_t total = 0;
 expect(eq(kimix_gdb_scan_vertices(session, 0, 0, nullptr, 0, &count, &total),
 KIMIX_GDB_OK));
 expect(eq(count, size_t{0})) << "the probe carries no items";
 expect(eq(total, size_t{10}));
 kimix_gdb_guid fetched[10]{};
 expect(eq(kimix_gdb_scan_vertices(session, 0, 0x7fffffff, fetched, 10, &count, &total),
 KIMIX_GDB_OK));
 expect(eq(count, size_t{10}));
 expect(eq(total, size_t{10}));
 for (auto const &uid : made) {
 bool found = false;
 for (auto const &got : fetched) {
 found = found || (got.lo == uid.lo && got.hi == uid.hi);
 }
 expect(found) << "every created vertex is in the scan";
 }
 expect(eq(kimix_gdb_scan_vertices(session, 0, 0x7fffffff, fetched, 4, &count, &total),
 KIMIX_GDB_ERR_BUFFER));
 expect(eq(count, size_t{10})) << "the count is reported either way";
 expect(eq(total, size_t{10}));
 expect(eq(kimix_gdb_scan_vertices(session, 0, 3, fetched, 10, &count, &total),
 KIMIX_GDB_OK));
 expect(eq(count, size_t{3})) << "a cap pages the page";
 expect(eq(total, size_t{10}));
 expect(eq(kimix_gdb_scan_vertices(session, 0, -1, fetched, 10, &count, &total),
 KIMIX_GDB_ERR_ARGUMENT));
 expect(eq(kimix_gdb_scan_vertices(nullptr, 0, 0, nullptr, 0, &count, &total),
 KIMIX_GDB_ERR_ARGUMENT));
 // Constraint-filtered scan: the warm vertices only.
 uint32_t warm_only = 0;
 expect(eq(kimix_gdb_constraint_create(session, &warm_only), KIMIX_GDB_OK));
 expect(eq(kimix_gdb_constraint_add_label(session, warm_only, warm, KIMIX_GDB_OP_EQUAL),
 KIMIX_GDB_OK));
 expect(eq(kimix_gdb_scan_vertices(session, warm_only, 0, nullptr, 0, &count, &total),
 KIMIX_GDB_OK));
 expect(eq(total, size_t{5}));
 expect(eq(kimix_gdb_scan_vertices(session, warm_only, 0x7fffffff, fetched, 10, &count,
 &total),
 KIMIX_GDB_OK));
 expect(eq(count, size_t{5}));
 for (size_t i = 0; i < count; ++i) {
 bool expected_warm = false;
 for (int k = 0; k < 10; k += 2) {
 expected_warm = expected_warm ||
 (fetched[i].lo == made[k].lo && fetched[i].hi == made[k].hi);
 }
 expect(expected_warm) << "the filtered scan returns exactly the warm vertices";
 }
 // Aggregates over the same store.
 kimix_gdb_aggregate_result result{};
 expect(eq(kimix_gdb_aggregate(session, 0, 0, KIMIX_GDB_AGGREGATE_COUNT, nullptr, 0,
 &result),
 KIMIX_GDB_OK));
 expect(eq(result.matched, uint64_t{10}));
 expect(eq(result.has_value, uint8_t{1}));
 expect(eq(result.value_kind, KIMIX_GDB_VALUE_INT64));
 expect(eq(result.int64_value, int64_t{10}));
 expect(eq(kimix_gdb_aggregate(session, warm_only, 0, KIMIX_GDB_AGGREGATE_COUNT, nullptr,
 0, &result),
 KIMIX_GDB_OK));
 expect(eq(result.matched, uint64_t{5}));
 expect(eq(result.int64_value, int64_t{5}));
 // min/max/sum/avg over age = 0..9.
 expect(eq(kimix_gdb_aggregate(session, 0, age, KIMIX_GDB_AGGREGATE_MIN, nullptr, 0,
 &result),
 KIMIX_GDB_OK));
 expect(eq(result.valued, uint64_t{10}));
 expect(eq(result.int64_value, int64_t{0}));
 expect(eq(kimix_gdb_aggregate(session, 0, age, KIMIX_GDB_AGGREGATE_MAX, nullptr, 0,
 &result),
 KIMIX_GDB_OK));
 expect(eq(result.int64_value, int64_t{9}));
 expect(eq(kimix_gdb_aggregate(session, 0, age, KIMIX_GDB_AGGREGATE_SUM, nullptr, 0,
 &result),
 KIMIX_GDB_OK));
 expect(eq(result.int64_value, int64_t{45}));
 expect(eq(kimix_gdb_aggregate(session, 0, age, KIMIX_GDB_AGGREGATE_AVG, nullptr, 0,
 &result),
 KIMIX_GDB_OK));
 expect(eq(result.value_kind, KIMIX_GDB_VALUE_DOUBLE));
 expect(eq(result.double_value, 4.5));
 // The string payload goes through scratch, with ERR_BUFFER when too small.
 uint8_t scratch[8]{};
 expect(eq(kimix_gdb_aggregate(session, 0, tag, KIMIX_GDB_AGGREGATE_MIN, scratch,
 sizeof(scratch), &result),
 KIMIX_GDB_OK));
 expect(eq(result.value_kind, KIMIX_GDB_VALUE_STRING));
 expect(eq(result.string_len, uint32_t{2}));
 expect(std::memcmp(scratch, "t0", 2) == 0);
 expect(eq(kimix_gdb_aggregate(session, 0, tag, KIMIX_GDB_AGGREGATE_MIN, scratch, 1,
 &result),
 KIMIX_GDB_ERR_BUFFER));
 expect(eq(result.string_len, uint32_t{2})) << "the length is reported either way";
 // Legality: decided up front from the registered declaration.
 expect(eq(kimix_gdb_aggregate(session, 0, tag, KIMIX_GDB_AGGREGATE_SUM, nullptr, 0,
 &result),
 KIMIX_GDB_ERR_OP));
 expect(eq(kimix_gdb_aggregate(session, 0, blob, KIMIX_GDB_AGGREGATE_SUM, nullptr, 0,
 &result),
 KIMIX_GDB_ERR_OP));
 expect(eq(kimix_gdb_aggregate(session, 0, blob, KIMIX_GDB_AGGREGATE_MIN, nullptr, 0,
 &result),
 KIMIX_GDB_ERR_OP));
 expect(eq(kimix_gdb_aggregate(session, 0, 999, KIMIX_GDB_AGGREGATE_SUM, nullptr, 0,
 &result),
 KIMIX_GDB_ERR_PROPERTY_TYPE));
 expect(eq(kimix_gdb_aggregate(session, 0, age, 99, nullptr, 0, &result),
 KIMIX_GDB_ERR_ARGUMENT));
 expect(eq(kimix_gdb_aggregate(session, 0, 0, KIMIX_GDB_AGGREGATE_AVG, nullptr, 0,
 &result),
 KIMIX_GDB_ERR_ARGUMENT)) << "a property reduction needs a property";
 // NULL guards.
 expect(eq(kimix_gdb_aggregate(nullptr, 0, age, KIMIX_GDB_AGGREGATE_COUNT, nullptr, 0,
 &result),
 KIMIX_GDB_ERR_ARGUMENT));
 expect(eq(kimix_gdb_aggregate(session, 0, age, KIMIX_GDB_AGGREGATE_COUNT, nullptr, 0,
 nullptr),
 KIMIX_GDB_ERR_ARGUMENT));
 };

  "cabi_transaction_and_persistence"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }
        kimix::string const path{dir.path().string().c_str()};
        kimix_gdb_guid kept{};
        kimix_gdb_guid dropped{};
        {
            kimix_gdb_session *session = nullptr;
            kimix_gdb_open_options options{};
            options.directory = path.c_str();
            options.max_journal_bytes = 1024 * 1024;
            expect(eq(kimix_gdb_open(&options, &session), KIMIX_GDB_OK));
            uint32_t label = 0;
            expect(eq(kimix_gdb_label_create(session, "node", 4, &label), KIMIX_GDB_OK));

            // An aborted transaction leaves nothing behind...
            expect(eq(kimix_gdb_tx_begin(session), KIMIX_GDB_OK));
            kimix_gdb_vertex_spec spec{};
            spec.key.label = label;
            spec.key.external_id = bytes_of("dropped");
            expect(eq(kimix_gdb_vertex_create(session, &spec, &dropped, nullptr), KIMIX_GDB_OK));
            expect(eq(kimix_gdb_tx_abort(session), KIMIX_GDB_OK));
            kimix_gdb_vertex_key key{};
            key.label = label;
            key.external_id = bytes_of("dropped");
            uint8_t found = 1;
            expect(eq(kimix_gdb_vertex_translate(session, &key, &dropped, &found), KIMIX_GDB_OK));
            expect(found == 0) << "the aborted create is gone";

            // ...and a committed one survives a save.
            expect(eq(kimix_gdb_tx_begin(session), KIMIX_GDB_OK));
            spec.key.external_id = bytes_of("kept");
            expect(eq(kimix_gdb_vertex_create(session, &spec, &kept, nullptr), KIMIX_GDB_OK));
            expect(eq(kimix_gdb_tx_commit(session), KIMIX_GDB_OK));
            expect(eq(kimix_gdb_persist(session, 1), KIMIX_GDB_OK));
            kimix_gdb_close(session);
        }
        // Reopen: the committed vertex is in the store, the aborted one is not.
        kimix_gdb_session *session = nullptr;
        kimix_gdb_open_options options{};
        options.directory = path.c_str();
        expect(eq(kimix_gdb_open(&options, &session), KIMIX_GDB_OK));
        uint32_t label = 0;
        uint8_t found = 0;
        expect(eq(kimix_gdb_label_find(session, "node", 4, &label, &found), KIMIX_GDB_OK));
        expect(found == 1) << "the schema survived the reopen";
        kimix_gdb_vertex_key key{};
        key.label = label;
        key.external_id = bytes_of("kept");
        kimix_gdb_guid resolved{};
        expect(eq(kimix_gdb_vertex_translate(session, &key, &resolved, &found), KIMIX_GDB_OK));
        expect(found == 1);
        expect(resolved.lo == kept.lo && resolved.hi == kept.hi);
        key.external_id = bytes_of("dropped");
        expect(eq(kimix_gdb_vertex_translate(session, &key, &resolved, &found), KIMIX_GDB_OK));
        expect(found == 0);
        kimix_gdb_close(session);
    };

    "cabi_churn_leaves_no_state"_test = [] {
        session_guard guard;
        kimix_gdb_session *session = guard.session;
        uint32_t const node = guard.label("node");
        for (int i = 0; i < 200; ++i) {
            kimix::string const name{kimix::format("churn{}", i)};
            kimix_gdb_vertex_spec spec{};
            spec.key.label = node;
            spec.key.external_id = kimix_gdb_bytes{
                reinterpret_cast<uint8_t const *>(name.data()), name.size()};
            kimix_gdb_guid uid{};
            expect(eq(kimix_gdb_vertex_create(session, &spec, &uid, nullptr), KIMIX_GDB_OK));
            kimix_gdb_guid const target = guard.vertex(node, kimix::format("fixed{}", i).c_str());
            kimix_gdb_guid edge{};
            expect(eq(kimix_gdb_edge_create(session, uid, target, KIMIX_GDB_EDGE_DIRECTED, nullptr,
                                            0, nullptr, nullptr, 0, nullptr, &edge),
                      KIMIX_GDB_OK));
            expect(eq(kimix_gdb_edge_free(session, edge), KIMIX_GDB_OK));
            expect(eq(kimix_gdb_vertex_free(session, uid), KIMIX_GDB_OK));
            expect(eq(kimix_gdb_vertex_free(session, target), KIMIX_GDB_OK));
        }
        // Everything is gone; the session is still usable and the close in the
          // guard is the only owned object's release.
          size_t count = 0;
          kimix_gdb_guid nobody{};
          expect(eq(kimix_gdb_vertex_free(session, nobody), KIMIX_GDB_ERR_VERTEX));
          expect(eq(kimix_gdb_vertex_labels(session, nobody, nullptr, 0, &count),
                    KIMIX_GDB_ERR_VERTEX));
      };

// The ABI 5 named-graph catalog (plan v3 step 3.7): a named open registers,
// a second LIVE open of the same name is ERR_NAME_EXISTS (status-coded, the
// refused session left completely unregistered), close releases the entry,
// anonymous sessions never appear, and the name round-trips through the
// session getter.
  "cabi_content_digest_option"_test = [] {
      // Plan v3 §4.5 (ABI 6): open_options.content_digest selects the vertex
      // content digest. 0 (or a zeroed struct) keeps MD5; 1 selects XXH3-128.
      // The digest is not exposed through the C API, so this walk verifies the
      // option is accepted and the session is fully usable under it (vertex
      // create with content, property write, read-back); the digest VALUES are
      // equivalence-tested at the C++ level (test_graph_db_serialize).
      for (uint8_t const flag : {uint8_t{0}, uint8_t{1}}) {
          kimix_gdb_open_options options{};
          options.directory = nullptr; // in memory
          options.auto_create_schema = 1;
          options.allow_parallel_edges = 1;
          options.allow_self_edges = 1;
          options.content_digest = flag;
          options.max_journal_bytes = 64ull * 1024ull * 1024ull;
          kimix_gdb_session *session = nullptr;
          expect(eq(kimix_gdb_open(&options, &session), KIMIX_GDB_OK));
          uint32_t const person = [&]() {
              uint32_t id = 0;
              expect(eq(kimix_gdb_label_create(session, "person", 6, &id), KIMIX_GDB_OK));
              return id;
          }();
          uint32_t const age = [&]() {
              uint32_t id = 0;
              expect(eq(kimix_gdb_property_type_create(session, "age", 3, KIMIX_GDB_CONTAINER_SINGLE,
                                                       KIMIX_GDB_DTYPE_INT64, KIMIX_GDB_SIZE_FIXED, 1, &id),
                        KIMIX_GDB_OK));
              return id;
          }();
          // A vertex WITH content (the field the digest caches).
          kimix_gdb_vertex_spec spec{};
          spec.key.label = person;
          static uint8_t const key_bytes[] = {'d', 'i', 'g', 'e', 's', 't'};
          spec.key.external_id.data = key_bytes;
          spec.key.external_id.len = sizeof(key_bytes);
          static uint8_t const content_bytes[] = {'d', 'i', 'g'};
          spec.content.data = content_bytes;
          spec.content.len = sizeof(content_bytes);
          kimix_gdb_guid uid{};
          expect(eq(kimix_gdb_vertex_create(session, &spec, &uid, nullptr), KIMIX_GDB_OK));
          // Usability under the policy: a property write + read-back round trip.
          kimix_gdb_property value{};
          value.kind = KIMIX_GDB_KIND_INT64;
          value.dtype = KIMIX_GDB_DTYPE_INT64;
          value.count = 1;
          value.i64 = 41;
          expect(eq(kimix_gdb_vertex_property_add(session, uid, age, &value), KIMIX_GDB_OK));
          kimix_gdb_property_record records[1]{};
          size_t fetched = 0;
          size_t used = 0;
          expect(eq(kimix_gdb_vertex_properties(session, uid, records, 1, nullptr, 0, &fetched, &used),
                    KIMIX_GDB_OK));
          expect(eq(fetched, size_t{1}));
          expect(eq(records[0].name_id, age));
          expect(eq(records[0].i64, 41));
          kimix_gdb_close(session);
      }
  };

  "cabi_named_graph_catalog"_test = [] {
 kimix_gdb_open_options options{};
 options.directory = nullptr;
 options.graph_name = "catalog-walk";
 size_t count = 0;
 expect(eq(kimix_gdb_catalog_count(&count), KIMIX_GDB_OK));
 size_t const baseline = count;
 uint8_t exists = 0xFF;
 expect(eq(kimix_gdb_catalog_contains("catalog-walk", 12, &exists), KIMIX_GDB_OK));
 expect(eq(exists, uint8_t{0})) << "nothing registered yet";
 kimix_gdb_session *first = nullptr;
 expect(eq(kimix_gdb_open(&options, &first), KIMIX_GDB_OK));
 expect(eq(kimix_gdb_catalog_count(&count), KIMIX_GDB_OK));
 expect(eq(count, baseline + 1));
 exists = 0;
 expect(eq(kimix_gdb_catalog_contains("catalog-walk", 12, &exists), KIMIX_GDB_OK));
 expect(eq(exists, uint8_t{1}));
 char name[32]{};
 size_t len = 99;
 expect(eq(kimix_gdb_session_graph_name(first, name, sizeof(name), &len), KIMIX_GDB_OK));
 expect(eq(len, size_t{12}));
 expect(std::memcmp(name, "catalog-walk", 12) == 0);
 // The collision: a second live open is refused, the first unaffected.
 kimix_gdb_session *second = nullptr;
 expect(eq(kimix_gdb_open(&options, &second), KIMIX_GDB_ERR_NAME_EXISTS));
 expect(second == nullptr);
 expect(eq(kimix_gdb_catalog_count(&count), KIMIX_GDB_OK));
 expect(eq(count, baseline + 1));
 // Anonymous opens never touch the catalog and carry no name.
 kimix_gdb_open_options anonymous{};
 anonymous.directory = nullptr;
 kimix_gdb_session *third = nullptr;
 expect(eq(kimix_gdb_open(&anonymous, &third), KIMIX_GDB_OK));
 expect(eq(kimix_gdb_catalog_count(&count), KIMIX_GDB_OK));
 expect(eq(count, baseline + 1));
 expect(eq(kimix_gdb_session_graph_name(third, name, sizeof(name), &len), KIMIX_GDB_OK));
 expect(eq(len, size_t{0})) << "the anonymous session has no name";
 kimix_gdb_close(third);
 // Closing the named session releases the entry; the name is reusable.
 kimix_gdb_close(first);
 expect(eq(kimix_gdb_catalog_count(&count), KIMIX_GDB_OK));
 expect(eq(count, baseline));
 exists = 1;
 expect(eq(kimix_gdb_catalog_contains("catalog-walk", 12, &exists), KIMIX_GDB_OK));
 expect(eq(exists, uint8_t{0}));
 kimix_gdb_session *reopened = nullptr;
 expect(eq(kimix_gdb_open(&options, &reopened), KIMIX_GDB_OK));
 kimix_gdb_close(reopened);
 // NULL guards.
 expect(eq(kimix_gdb_catalog_contains(nullptr, 3, &exists), KIMIX_GDB_ERR_ARGUMENT));
 expect(eq(kimix_gdb_catalog_contains("catalog-walk", 12, nullptr), KIMIX_GDB_ERR_ARGUMENT));
 expect(eq(kimix_gdb_catalog_count(nullptr), KIMIX_GDB_ERR_ARGUMENT));
 expect(eq(kimix_gdb_session_graph_name(nullptr, name, sizeof(name), &len),
 KIMIX_GDB_ERR_ARGUMENT));
 expect(eq(kimix_gdb_session_graph_name(reopened, name, sizeof(name), nullptr),
 KIMIX_GDB_ERR_ARGUMENT));
 };

// The ABI 4 create-time edge payload (plan v3 step 3.1/3.2): ONE call with
// property_names + properties, two-phase read-back, and the refusal matrix
// proving a refused create leaves nothing behind.
"cabi_edge_create_with_properties"_test = [] {
 session_guard guard;
 kimix_gdb_session *session = guard.session;
 uint32_t const node = guard.label("node");
 uint32_t const link = guard.label("link");
 uint32_t const age = guard.property_type("age", KIMIX_GDB_DTYPE_INT64);
 uint32_t const tag = guard.property_type("tag", KIMIX_GDB_DTYPE_CHAR);
 kimix_gdb_guid const a = guard.vertex(node, "a");
 kimix_gdb_guid const b = guard.vertex(node, "b");
 uint32_t const link_only[1]{link};
 uint32_t const names[2]{age, tag};
 kimix_gdb_property const values[2]{int_property(42), text_property("hello")};
 kimix_gdb_guid edge{};
 expect(eq(kimix_gdb_edge_create(session, a, b, KIMIX_GDB_EDGE_DIRECTED, link_only, 1,
 names, values, 2, nullptr, &edge),
 KIMIX_GDB_OK));
 // The payload comes back in one two-phase read: both name ids, both values.
 size_t count = 0;
 size_t scratch_bytes = 0;
 expect(eq(kimix_gdb_edge_properties(session, edge, nullptr, 0, nullptr, 0, &count,
 &scratch_bytes),
 KIMIX_GDB_OK));
 expect(eq(count, size_t{2}));
 kimix::vector<kimix_gdb_property_record> items(count);
 kimix::vector<uint8_t> scratch(scratch_bytes + 8);
 expect(eq(kimix_gdb_edge_properties(session, edge, items.data(), items.size(),
 scratch.data(), scratch.size(), &count, &scratch_bytes),
 KIMIX_GDB_OK));
 expect(eq(count, size_t{2}));
 bool saw_age = false;
 bool saw_tag = false;
 for (size_t i = 0; i < count; ++i) {
 saw_age = saw_age || (items[i].name_id == age && items[i].kind == KIMIX_GDB_KIND_INT64 &&
 items[i].i64 == 42);
 saw_tag = saw_tag || (items[i].name_id == tag && items[i].kind == KIMIX_GDB_KIND_STRING &&
 items[i].payload_len == 5);
 }
 expect(saw_age) << "the create-time int64 survived";
 expect(saw_tag) << "the create-time string survived (5 bytes in scratch)";
 // The refusal matrix.
 expect(eq(kimix_gdb_edge_create(session, a, b, KIMIX_GDB_EDGE_DIRECTED, nullptr, 0,
 nullptr, values, 1, nullptr, &edge),
 KIMIX_GDB_ERR_ARGUMENT))
 << "property_count > 0 with null names is refused up front";
 expect(eq(kimix_gdb_edge_create(session, a, b, KIMIX_GDB_EDGE_DIRECTED, nullptr, 0,
 names, nullptr, 1, nullptr, &edge),
 KIMIX_GDB_ERR_ARGUMENT));
 uint32_t const bad_names[1]{9999};
 expect(eq(kimix_gdb_edge_create(session, a, b, KIMIX_GDB_EDGE_DIRECTED, nullptr, 0,
 bad_names, values, 1, nullptr, &edge),
 KIMIX_GDB_ERR_PROPERTY_TYPE));
 size_t seen = 0;
 size_t total = 0;
 expect(eq(kimix_gdb_edges_of_vertex(session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 0,
 nullptr, 0, &seen, &total),
 KIMIX_GDB_OK));
 expect(eq(total, size_t{1})) << "the one good edge is the only edge";
 };

    // The string-array kind across the C ABI (ABI 7, plan §1.1 / §1.4): a char_ +
    // ARRAY declaration, one write, and the byte-exact read-back on both entity forms
    // (vertex, edge), including the create-time payload and a later set.
    "cabi_string_array_round_trip"_test = [] {
        session_guard guard;
        uint32_t const node = guard.label("node");
        uint32_t const docs = guard.property_type_array("docs", KIMIX_GDB_DTYPE_CHAR, 3);
        uint32_t const notes = guard.property_type_array("notes", KIMIX_GDB_DTYPE_CHAR, 1);

        // ---- vertex: add, then the two-phase read ----------------------------
        kimix_gdb_guid const a = guard.vertex(node, "a");
        auto const block = string_array_block({"alpha", "beta", "gamma"});
        auto const value = string_array_property(block, 3);
        expect(eq(kimix_gdb_vertex_property_add(guard.session, a, docs, &value), KIMIX_GDB_OK));
        property_read page = read_properties([&](kimix_gdb_property_record *items, size_t cap,
                                                 uint8_t *scratch, size_t scratch_cap,
                                                 size_t *out_count, size_t *out_scratch) {
            return kimix_gdb_vertex_properties(guard.session, a, items, cap, scratch,
                                               scratch_cap, out_count, out_scratch);
        });
        expect_string_array_record(page.find(docs), page, block, 3, "vertex docs");
        // one property, so the scratch IS the block: the ABI hands it out verbatim
        expect(eq(page.scratch_bytes, block.size())) << "scratch carries the block verbatim";

        // ---- a one-element array through its own array declaration ----------
        auto const single = string_array_block({"only-one"});
        auto const single_value = string_array_property(single, 1);
        expect(eq(kimix_gdb_vertex_property_add(guard.session, a, notes, &single_value),
                  KIMIX_GDB_OK));
        property_read page2 = read_properties([&](kimix_gdb_property_record *items, size_t cap,
                                                  uint8_t *scratch, size_t scratch_cap,
                                                  size_t *out_count, size_t *out_scratch) {
            return kimix_gdb_vertex_properties(guard.session, a, items, cap, scratch,
                                               scratch_cap, out_count, out_scratch);
        });
        expect_string_array_record(page2.find(notes), page2, single, 1, "vertex notes");
        expect_string_array_record(page2.find(docs), page2, block, 3, "vertex docs again");

        // ---- declaration round trip: char_ + array + the exact count ---------
        kimix_gdb_property_type types[4]{};
        size_t type_count = 0;
        size_t type_scratch = 0;
        expect(eq(kimix_gdb_vertex_property_types(guard.session, a, nullptr, 0, nullptr, 0,
                                                  &type_count, &type_scratch),
                  KIMIX_GDB_OK));
        kimix::vector<uint8_t> name_scratch(type_scratch == 0 ? 1 : type_scratch);
        expect(eq(kimix_gdb_vertex_property_types(guard.session, a, types, 4,
                                                  name_scratch.data(), name_scratch.size(),
                                                  &type_count, &type_scratch),
                  KIMIX_GDB_OK));
        expect(eq(type_count, size_t{2}));
        bool saw_declaration = false;
        for (size_t i = 0; i < type_count; ++i) {
            if (types[i].id != docs) {
                continue;
            }
            saw_declaration = true;
            expect(types[i].container == KIMIX_GDB_CONTAINER_ARRAY)
                << "one char_ declaration serves StringArray by its container";
            expect(types[i].dtype == KIMIX_GDB_DTYPE_CHAR);
            expect(eq(types[i].count, uint32_t{3})) << "the declared array length is exact";
        }
        expect(saw_declaration);

        // ---- the edge twins: the create-time payload, then a set ------------
        kimix_gdb_guid const b = guard.vertex(node, "b");
        uint32_t const names[1]{docs};
        const kimix_gdb_property create_values[1]{value};
        kimix_gdb_guid edge{};
        expect(eq(kimix_gdb_edge_create(guard.session, a, b, KIMIX_GDB_EDGE_DIRECTED, nullptr, 0,
                                        names, create_values, 1, nullptr, &edge),
                  KIMIX_GDB_OK));
        property_read edge_page = read_properties([&](kimix_gdb_property_record *items,
                                                      size_t cap, uint8_t *scratch,
                                                      size_t scratch_cap, size_t *out_count,
                                                      size_t *out_scratch) {
            return kimix_gdb_edge_properties(guard.session, edge, items, cap, scratch,
                                             scratch_cap, out_count, out_scratch);
        });
        expect_string_array_record(edge_page.find(docs), edge_page, block, 3,
                                   "edge create-time payload");

        // A later SET on the same edge property: the new bytes are what a read gives.
        auto const replacement = string_array_block({"X", "", "third-element"});
        auto const replacement_value = string_array_property(replacement, 3);
        expect(eq(kimix_gdb_edge_property_set(guard.session, edge, docs, &replacement_value),
                  KIMIX_GDB_OK));
        property_read set_page = read_properties([&](kimix_gdb_property_record *items,
                                                     size_t cap, uint8_t *scratch,
                                                     size_t scratch_cap, size_t *out_count,
                                                     size_t *out_scratch) {
            return kimix_gdb_edge_properties(guard.session, edge, items, cap, scratch,
                                             scratch_cap, out_count, out_scratch);
        });
        auto const *const record = set_page.find(docs);
        expect_string_array_record(record, set_page, replacement, 3, "edge set property");
        if (record != nullptr) {
            // an empty element is a VALUE, not a separator: "" round-trips in place
            auto const elements = string_array_elements(
                set_page.scratch.data() + record->payload_offset, record->payload_len);
            expect(eq(elements.size(), size_t{3}));
            expect(elements[1].empty()) << "the empty string element survives the round trip";
            expect(elements[0] == kimix::string{"X"}) << "the set replaced the old block";
            expect(elements[2] == kimix::string{"third-element"});
        }
    };

    // A malformed §1.1 block never becomes a value (plan §1.1): the write path answers
    // KIMIX_GDB_ERR_COUNT and stores nothing. Every shape the validator and the
    // exact-count rule are documented to refuse, on both entity forms, on a create-time
    // payload, and on a constraint's set.
    "cabi_string_array_malformed_is_refused"_test = [] {
        session_guard guard;
        uint32_t const node = guard.label("node");
        uint32_t const docs = guard.property_type_array("docs", KIMIX_GDB_DTYPE_CHAR, 3);
        kimix_gdb_guid const a = guard.vertex(node, "a");
        kimix_gdb_guid const b = guard.vertex(node, "b");

        // (a) a truncated element: the count says 2, the block stops inside element 1.
        kimix::vector<uint8_t> truncated;
        push_u32(truncated, 2);
        push_u32(truncated, 3);
        truncated.push_back('a');
        truncated.push_back('b');
        truncated.push_back('c');
        // (b) a length that runs past the end of the payload.
        kimix::vector<uint8_t> overrun;
        push_u32(overrun, 1);
        push_u32(overrun, 99);
        overrun.push_back('a');
        // (c) a well-formed block with trailing junk (the walk must consume it exactly).
        auto trailing = string_array_block({"ok"});
        trailing.push_back(0);
        // (d) a well-formed block whose count field disagrees with the block.
        auto const good = string_array_block({"one"});
        // (e) no block at all: a NULL payload is not an empty array (which is the
        // 4-byte [u32 0] block).
        kimix_gdb_property empty_payload{};
        empty_payload.kind = KIMIX_GDB_KIND_STRING_ARRAY;
        empty_payload.dtype = KIMIX_GDB_DTYPE_CHAR;
        empty_payload.count = 0;

        // The refusals below each need an ADDRESSABLE input (a C++ temporary has no
        // address), so every malformed block gets one named property here.
        auto const bad_truncated = string_array_property(truncated, 2);
        auto const bad_overrun = string_array_property(overrun, 1);
        auto const bad_trailing = string_array_property(trailing, 1);
        auto const bad_count = string_array_property(good, 2);

        expect(eq(kimix_gdb_vertex_property_add(guard.session, a, docs,
                                                &bad_truncated),
                  KIMIX_GDB_ERR_COUNT));
        expect(eq(kimix_gdb_vertex_property_add(guard.session, a, docs,
                                                &bad_overrun),
                  KIMIX_GDB_ERR_COUNT));
        expect(eq(kimix_gdb_vertex_property_add(guard.session, a, docs,
                                                &bad_trailing),
                  KIMIX_GDB_ERR_COUNT));
        expect(eq(kimix_gdb_vertex_property_add(guard.session, a, docs,
                                                &bad_count),
                  KIMIX_GDB_ERR_COUNT))
            << "an inexact count is inconsistent input, not a truncation hint (§1.4)";
        expect(eq(kimix_gdb_vertex_property_add(guard.session, a, docs, &empty_payload),
                  KIMIX_GDB_ERR_COUNT));
        expect(eq(kimix_gdb_vertex_property_set(guard.session, a, docs,
                                                &bad_truncated),
                  KIMIX_GDB_ERR_COUNT))
            << "the guard sits in all three write modes, not only add";
        expect(eq(kimix_gdb_vertex_property_update(guard.session, a, docs,
                                                  &bad_overrun),
                  KIMIX_GDB_ERR_COUNT));

        // Nothing was stored by ANY of the refusals.
        size_t count = 0;
        size_t scratch_bytes = 0;
        expect(eq(kimix_gdb_vertex_properties(guard.session, a, nullptr, 0, nullptr, 0, &count,
                                              &scratch_bytes),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{0})) << "a refused write leaves the bag empty";

        // The same guard on the edge writes.
        kimix_gdb_guid const edge = guard.edge(a, b);
        expect(eq(kimix_gdb_edge_property_add(guard.session, edge, docs,
                                              &bad_overrun),
                  KIMIX_GDB_ERR_COUNT));
        expect(eq(kimix_gdb_edge_property_set(guard.session, edge, docs,
                                              &bad_truncated),
                  KIMIX_GDB_ERR_COUNT));
        expect(eq(kimix_gdb_edge_property_update(guard.session, edge, docs,
                                                 &bad_trailing),
                  KIMIX_GDB_ERR_COUNT));
        count = 0;
        expect(eq(kimix_gdb_edge_properties(guard.session, edge, nullptr, 0, nullptr, 0, &count,
                                            &scratch_bytes),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{0}));

        // A malformed value in a vertex create refuses the whole create.
        kimix_gdb_vertex_spec spec{};
        spec.key.label = node;
        spec.key.external_id = bytes_of("newcomer");
        uint32_t const names[1]{docs};
        const kimix_gdb_property values[1]{bad_overrun};
        spec.property_names = names;
        spec.properties = values;
        spec.property_count = 1;
        kimix_gdb_guid created{};
        uint8_t warning = 0;
        expect(eq(kimix_gdb_vertex_create(guard.session, &spec, &created, &warning),
                  KIMIX_GDB_ERR_COUNT));
        kimix_gdb_vertex_key key{};
        key.label = node;
        key.external_id = bytes_of("newcomer");
        kimix_gdb_guid resolved{};
        uint8_t found = 1;
        expect(eq(kimix_gdb_vertex_translate(guard.session, &key, &resolved, &found),
                  KIMIX_GDB_OK));
        expect(found == 0) << "the refused create stored no vertex";

        // And a constraint's ANY_IN set is a value like any other: the same malformed
        // block answers ERR_COUNT instead of building a never-matching condition.
        uint32_t constraint = 0;
        expect(eq(kimix_gdb_constraint_create(guard.session, &constraint), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_constraint_add_property(guard.session, constraint, docs,
                                                    KIMIX_GDB_OP_ANY_IN,
                                                    &bad_overrun, 1),
                  KIMIX_GDB_ERR_COUNT));

        // The empty array IS a value: the 4-byte [u32 0] block with count 0. It cannot
        // be STORED as an entity property (GDI requires an array declaration of at
        // least one element), but it is a legal — never-matching — ANY_IN set (§1.2).
        auto const empty_block = string_array_block({});
        expect(eq(empty_block.size(), size_t{4}));
        auto const empty_value = string_array_property(empty_block, 0);
        expect(eq(kimix_gdb_constraint_add_property(guard.session, constraint, docs,
                                                    KIMIX_GDB_OP_ANY_IN, &empty_value, 0),
                  KIMIX_GDB_OK))
            << "the empty block validates as a well-formed (never-matching) set";

        // The good block still writes and reads back after all those refusals.
        auto const three = string_array_block({"a", "b", "c"});
        auto const three_value = string_array_property(three, 3);
        expect(eq(kimix_gdb_vertex_property_add(guard.session, a, docs, &three_value),
                  KIMIX_GDB_OK));
        property_read page = read_properties([&](kimix_gdb_property_record *items, size_t cap,
                                                 uint8_t *scratch, size_t scratch_cap,
                                                 size_t *out_count, size_t *out_scratch) {
            return kimix_gdb_vertex_properties(guard.session, a, items, cap, scratch,
                                               scratch_cap, out_count, out_scratch);
        });
        expect_string_array_record(page.find(docs), page, three, 3, "after the refusals");
    };

    // ANY_IN (ABI 7, plan §1.2) through the C ABI: the SET is the condition's value,
    // membership holds for a scalar actual and for ANY element of an array actual, a
    // missing property never matches, an empty set matches nothing, a 2,000-id set
    // behaves, and add-time validation refuses whatever is not a set. Every read that
    // evaluates a constraint inherits the answer — edges_of_vertex, neighbors_of_vertex,
    // count/exists_neighbor, scan_vertices, the edge-filtered shortest path.
    "cabi_any_in"_test = [] {
        session_guard guard;
        kimix_gdb_session *session = guard.session;
        uint32_t const node = guard.label("node");
        uint32_t const docs = guard.property_type_array("docs", KIMIX_GDB_DTYPE_CHAR, 3);
        uint32_t const state = guard.property_type("state", KIMIX_GDB_DTYPE_CHAR);
        uint32_t const codes = guard.property_type_array("codes", KIMIX_GDB_DTYPE_INT64, 2);
        uint32_t const prio = guard.property_type("prio", KIMIX_GDB_DTYPE_INT64);

        // v1 -> v2 (docs d-open), v1 -> v3 (docs d-closed), v1 -> v4 (NO docs).
        kimix_gdb_guid const v1 = guard.vertex(node, "v1");
        kimix_gdb_guid const v2 = guard.vertex(node, "v2");
        kimix_gdb_guid const v3 = guard.vertex(node, "v3");
        kimix_gdb_guid const v4 = guard.vertex(node, "v4");
        auto const open_block = string_array_block({"d-open", "junk", ""});
        auto const closed_block = string_array_block({"d-closed", "junk", ""});
        auto const open_value = string_array_property(open_block, 3);
        auto const closed_value = string_array_property(closed_block, 3);
        uint32_t const names[1]{docs};
        {
            const kimix_gdb_property values[1]{open_value};
            kimix_gdb_guid edge{};
            expect(eq(kimix_gdb_edge_create(session, v1, v2, KIMIX_GDB_EDGE_DIRECTED, nullptr, 0,
                                            names, values, 1, nullptr, &edge),
                      KIMIX_GDB_OK));
        }
        {
            const kimix_gdb_property values[1]{closed_value};
            kimix_gdb_guid edge{};
            expect(eq(kimix_gdb_edge_create(session, v1, v3, KIMIX_GDB_EDGE_DIRECTED, nullptr, 0,
                                            names, values, 1, nullptr, &edge),
                      KIMIX_GDB_OK));
        }
        kimix_gdb_guid const e14 = guard.edge(v1, v4); // carries no docs at all
        expect(e14.lo != 0ull || e14.hi != 0ull);

        // The scalar twin of the membership test, on the vertices' own property.
        kimix_gdb_property const state_open = text_property("open");
        kimix_gdb_property const state_closed = text_property("closed");
        expect(eq(kimix_gdb_vertex_property_add(session, v2, state, &state_open), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_vertex_property_add(session, v3, state, &state_closed), KIMIX_GDB_OK));

        size_t total = 0;

        // -- a one-element string set: the HIT edge only ----------------------
        auto const set_open = string_array_block({"d-open"});
        auto const set_open_value = string_array_property(set_open, 1);
        uint32_t readable = 0;
        expect(eq(kimix_gdb_constraint_create(session, &readable), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_constraint_add_property(session, readable, docs, KIMIX_GDB_OP_ANY_IN,
                                                    &set_open_value, 1),
                  KIMIX_GDB_OK));
        (void)edge_uids(session, v1, readable, KIMIX_GDB_ORIENTATION_OUTGOING, total);
        expect(eq(total, size_t{1})) << "exactly the edge whose docs carry the member";

        // -- a MISS: a set with no member at all ------------------------------
        auto const set_none = string_array_block({"nope", "also-nope"});
        auto const set_none_value = string_array_property(set_none, 2);
        uint32_t unreadable = 0;
        expect(eq(kimix_gdb_constraint_create(session, &unreadable), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_constraint_add_property(session, unreadable, docs,
                                                    KIMIX_GDB_OP_ANY_IN, &set_none_value, 2),
                  KIMIX_GDB_OK));
        (void)edge_uids(session, v1, unreadable, KIMIX_GDB_ORIENTATION_OUTGOING, total);
        expect(eq(total, size_t{0})) << "no member, no edge";
        size_t count = 0;
        expect(eq(kimix_gdb_count_neighbors(session, v1, unreadable,
                                            KIMIX_GDB_ORIENTATION_OUTGOING, &count),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{0}));

        // -- ANY element of an array actual: the member is the SECOND element --
        auto const set_junk = string_array_block({"junk"});
        auto const set_junk_value = string_array_property(set_junk, 1);
        uint32_t second_element = 0;
        expect(eq(kimix_gdb_constraint_create(session, &second_element), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_constraint_add_property(session, second_element, docs,
                                                    KIMIX_GDB_OP_ANY_IN, &set_junk_value, 1),
                  KIMIX_GDB_OK));
        (void)edge_uids(session, v1, second_element, KIMIX_GDB_ORIENTATION_OUTGOING, total);
        expect(eq(total, size_t{2}))
            << "both docs-carrying edges hold `junk` at index 1 — any element, not the first";

        // -- a scalar actual: "open" and "closed" are members of the set ------
        // neighbors_of_vertex filters the NEIGHBOUR VERTEX (GDI §5.10), so this leg
        // also proves the missing-property short-circuit: v4 has no `state` at all.
        auto const set_states = string_array_block({"open", "closed"});
        auto const set_states_value = string_array_property(set_states, 2);
        uint32_t any_state = 0;
        expect(eq(kimix_gdb_constraint_create(session, &any_state), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_constraint_add_property(session, any_state, state,
                                                    KIMIX_GDB_OP_ANY_IN, &set_states_value, 2),
                  KIMIX_GDB_OK));
        size_t neighbour_count = 0;
        size_t neighbour_total = 0;
        expect(eq(kimix_gdb_neighbors_of_vertex(session, v1, any_state,
                                                KIMIX_GDB_ORIENTATION_OUTGOING, 0, nullptr, 0,
                                                &neighbour_count, &neighbour_total),
                  KIMIX_GDB_OK));
        expect(eq(neighbour_total, size_t{2})) << "two neighbours carry a member state";
        kimix_gdb_guid neighbours[4]{};
        expect(eq(kimix_gdb_neighbors_of_vertex(session, v1, any_state,
                                                KIMIX_GDB_ORIENTATION_OUTGOING, 4, neighbours, 4,
                                                &neighbour_count, &neighbour_total),
                  KIMIX_GDB_OK));
        bool saw_v2 = false;
        bool saw_v3 = false;
        bool saw_v4 = false;
        for (size_t i = 0; i < neighbour_count; ++i) {
            saw_v2 = saw_v2 || (neighbours[i].lo == v2.lo && neighbours[i].hi == v2.hi);
            saw_v3 = saw_v3 || (neighbours[i].lo == v3.lo && neighbours[i].hi == v3.hi);
            saw_v4 = saw_v4 || (neighbours[i].lo == v4.lo && neighbours[i].hi == v4.hi);
        }
        expect(saw_v2 && saw_v3 && !saw_v4) << "a missing property never satisfies any_in";

        // -- the EMPTY set: legal, and it can never match (§1.2) --------------
        auto const set_empty = string_array_block({});
        auto const set_empty_value = string_array_property(set_empty, 0);
        uint32_t empty_set = 0;
        expect(eq(kimix_gdb_constraint_create(session, &empty_set), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_constraint_add_property(session, empty_set, docs, KIMIX_GDB_OP_ANY_IN,
                                                    &set_empty_value, 0),
                  KIMIX_GDB_OK))
            << "an empty set is a legal condition with count 0 (§1.4)";
        (void)edge_uids(session, v1, empty_set, KIMIX_GDB_ORIENTATION_OUTGOING, total);
        expect(eq(total, size_t{0})) << "nothing is a member of an empty set";
        uint8_t exists_flag = 1;
        expect(eq(kimix_gdb_exists_neighbor(session, v1, empty_set, KIMIX_GDB_ORIENTATION_OUTGOING,
                                            &exists_flag),
                  KIMIX_GDB_OK));
        expect(exists_flag == 0);

        // -- a ~2,000-element set: the index answers, hits and misses alike ----
        auto const big_block = string_array_block_sequence(2000);
        auto const big_value = string_array_property(big_block, 2000);
        uint32_t big_set = 0;
        expect(eq(kimix_gdb_constraint_create(session, &big_set), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_constraint_add_property(session, big_set, docs, KIMIX_GDB_OP_ANY_IN,
                                                    &big_value, 2000),
                  KIMIX_GDB_OK))
            << "§1.4: no 65,535 ceiling survives — only the u32 range and the block";
        kimix_gdb_guid const v5 = guard.vertex(node, "v5");
        kimix_gdb_guid const v6 = guard.vertex(node, "v6");
        {
            auto const block = string_array_block({"id-0777", "zz-1", "zz-2"});
            auto const property = string_array_property(block, 3);
            const kimix_gdb_property values[1]{property};
            kimix_gdb_guid edge{};
            expect(eq(kimix_gdb_edge_create(session, v1, v5, KIMIX_GDB_EDGE_DIRECTED, nullptr, 0,
                                            names, values, 1, nullptr, &edge),
                      KIMIX_GDB_OK));
        }
        {
            auto const block = string_array_block({"zz-1", "zz-2", "zz-3"});
            auto const property = string_array_property(block, 3);
            const kimix_gdb_property values[1]{property};
            kimix_gdb_guid edge{};
            expect(eq(kimix_gdb_edge_create(session, v1, v6, KIMIX_GDB_EDGE_DIRECTED, nullptr, 0,
                                            names, values, 1, nullptr, &edge),
                      KIMIX_GDB_OK));
        }
        // id-0777 is the only member in the neighbourhood: the generated set holds no
        // d-open / d-closed / junk, and the edge to v6 carries none of it.
        (void)edge_uids(session, v1, big_set, KIMIX_GDB_ORIENTATION_OUTGOING, total);
        expect(eq(total, size_t{1})) << "exactly the edge carrying id-0777";

        // -- the int64_array set (the bonus kind): array and scalar actual ----
        kimix::vector<int64_t> const set_ints{9, 11};
        auto const ints_value = int64_array_property(set_ints);
        uint32_t int_set = 0;
        expect(eq(kimix_gdb_constraint_create(session, &int_set), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_constraint_add_property(session, int_set, codes, KIMIX_GDB_OP_ANY_IN,
                                                    &ints_value, 2),
                  KIMIX_GDB_OK));
        kimix_gdb_guid const v7 = guard.vertex(node, "v7");
        kimix_gdb_guid const v8 = guard.vertex(node, "v8");
        kimix_gdb_guid const v9 = guard.vertex(node, "v9");
        uint32_t const code_names[1]{codes};
        {
            kimix::vector<int64_t> const edge_codes{7, 9}; // 9 is a member, at index 1
            auto const property = int64_array_property(edge_codes);
            const kimix_gdb_property values[1]{property};
            kimix_gdb_guid edge{};
            expect(eq(kimix_gdb_edge_create(session, v1, v7, KIMIX_GDB_EDGE_DIRECTED, nullptr, 0,
                                            code_names, values, 1, nullptr, &edge),
                      KIMIX_GDB_OK));
        }
        {
            kimix::vector<int64_t> const edge_codes{5, 6}; // no member
            auto const property = int64_array_property(edge_codes);
            const kimix_gdb_property values[1]{property};
            kimix_gdb_guid edge{};
            expect(eq(kimix_gdb_edge_create(session, v1, v8, KIMIX_GDB_EDGE_DIRECTED, nullptr, 0,
                                            code_names, values, 1, nullptr, &edge),
                      KIMIX_GDB_OK));
        }
        (void)edge_uids(session, v1, int_set, KIMIX_GDB_ORIENTATION_OUTGOING, total);
        expect(eq(total, size_t{1})) << "the edge whose codes array carries a member";

        // A scalar int64 actual against an int set: v9.prio == 3 is in {3,4}, but v9 is
        // not a neighbour of v1, so the EDGE read must find nothing — and the NEIGHBOUR
        // read (which filters vertices) finds v9 only through an edge, of which there is
        // none. Both answers are 0, for different reasons the comments name.
        kimix_gdb_property const prio3 = int_property(3);
        expect(eq(kimix_gdb_vertex_property_add(session, v9, prio, &prio3), KIMIX_GDB_OK));
        kimix::vector<int64_t> const prio_set{3, 4};
        auto const prio_value = int64_array_property(prio_set);
        uint32_t prio_any = 0;
        expect(eq(kimix_gdb_constraint_create(session, &prio_any), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_constraint_add_property(session, prio_any, prio, KIMIX_GDB_OP_ANY_IN,
                                                    &prio_value, 2),
                  KIMIX_GDB_OK));
        (void)edge_uids(session, v1, prio_any, KIMIX_GDB_ORIENTATION_OUTGOING, total);
        expect(eq(total, size_t{0})) << "no edge carries `prio` at all";
        expect(eq(kimix_gdb_count_neighbors(session, v1, prio_any, KIMIX_GDB_ORIENTATION_OUTGOING,
                                            &count),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{0})) << "no neighbour of v1 carries `prio` either";
        // ... and where the scalar IS the neighbour's property, it matches: v9 belongs
        // to no neighbourhood, so give v7 the property and read v1's neighbours.
        expect(eq(kimix_gdb_vertex_property_add(session, v7, prio, &prio3), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_count_neighbors(session, v1, prio_any, KIMIX_GDB_ORIENTATION_OUTGOING,
                                            &count),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{1})) << "v7.prio == 3 is a member of {3,4}";

        // -- the walk inherits the rule: the edge-filtered shortest path ------
        // v10 --d-open--> v11 --d-open--> v12: every hop satisfies the readable set.
        kimix_gdb_guid const v10 = guard.vertex(node, "v10");
        kimix_gdb_guid const v11 = guard.vertex(node, "v11");
        kimix_gdb_guid const v12 = guard.vertex(node, "v12");
        {
            const kimix_gdb_property values[1]{open_value};
            kimix_gdb_guid edge{};
            expect(eq(kimix_gdb_edge_create(session, v10, v11, KIMIX_GDB_EDGE_DIRECTED, nullptr,
                                            0, names, values, 1, nullptr, &edge),
                      KIMIX_GDB_OK));
            expect(eq(kimix_gdb_edge_create(session, v11, v12, KIMIX_GDB_EDGE_DIRECTED, nullptr,
                                            0, names, values, 1, nullptr, &edge),
                      KIMIX_GDB_OK));
        }
        size_t path_count = 0;
        kimix_gdb_path_step path[8]{};
        expect(eq(kimix_gdb_shortest_path_edges(session, v10, v12, readable,
                                                KIMIX_GDB_ORIENTATION_OUTGOING,
                                                KIMIX_GDB_MAX_HOP_DEPTH, nullptr, 0, &path_count),
                  KIMIX_GDB_OK));
        expect(eq(path_count, size_t{3})) << "both hops satisfy the ANY_IN set";
        expect(eq(kimix_gdb_shortest_path_edges(session, v10, v12, readable,
                                                KIMIX_GDB_ORIENTATION_OUTGOING,
                                                KIMIX_GDB_MAX_HOP_DEPTH, path, 8, &path_count),
                  KIMIX_GDB_OK));
        expect(path[2].vertex.lo == v12.lo && path[2].vertex.hi == v12.hi);
        expect(eq(kimix_gdb_shortest_path_edges(session, v10, v12, unreadable,
                                                KIMIX_GDB_ORIENTATION_OUTGOING,
                                                KIMIX_GDB_MAX_HOP_DEPTH, nullptr, 0, &path_count),
                  KIMIX_GDB_OK));
        expect(eq(path_count, size_t{0})) << "a filtered-out hop is no path, not an error";
        expect(eq(kimix_gdb_shortest_path_edges(session, v10, v12, empty_set,
                                                KIMIX_GDB_ORIENTATION_OUTGOING,
                                                KIMIX_GDB_MAX_HOP_DEPTH, nullptr, 0, &path_count),
                  KIMIX_GDB_OK));
        expect(eq(path_count, size_t{0})) << "the empty set filters every hop";

        // -- §1.2 add-time validation: the value must BE a set ----------------
        uint32_t bad = 0;
        expect(eq(kimix_gdb_constraint_create(session, &bad), KIMIX_GDB_OK));
        kimix_gdb_property const scalar = text_property("open");
        expect(eq(kimix_gdb_constraint_add_property(session, bad, docs, KIMIX_GDB_OP_ANY_IN,
                                                    &scalar, 1),
                  KIMIX_GDB_ERR_OP_DATATYPE_MISMATCH))
            << "a scalar value cannot be a set";
        kimix_gdb_property const number = int_property(7);
        expect(eq(kimix_gdb_constraint_add_property(session, bad, docs, KIMIX_GDB_OP_ANY_IN,
                                                    &number, 1),
                  KIMIX_GDB_ERR_OP_DATATYPE_MISMATCH));
        kimix::vector<uint8_t> const blob_bytes(8, uint8_t{1});
        kimix_gdb_property const blob = blob_property(blob_bytes.data(), blob_bytes.size());
        expect(eq(kimix_gdb_constraint_add_property(session, bad, docs, KIMIX_GDB_OP_ANY_IN,
                                                    &blob, 1),
                  KIMIX_GDB_ERR_OP_DATATYPE_MISMATCH));
        kimix::vector<double> const reals{1.0, 2.0};
        kimix_gdb_property double_array{};
        double_array.kind = KIMIX_GDB_KIND_DOUBLE_ARRAY;
        double_array.dtype = KIMIX_GDB_DTYPE_DOUBLE;
        double_array.count = 2;
        double_array.payload.data = reinterpret_cast<uint8_t const *>(reals.data());
        double_array.payload.len = reals.size() * 8;
        expect(eq(kimix_gdb_constraint_add_property(session, bad, docs, KIMIX_GDB_OP_ANY_IN,
                                                    &double_array, 2),
                  KIMIX_GDB_ERR_OP_DATATYPE_MISMATCH))
            << "a double_array is neither orderable nor a membership set";
        expect(eq(kimix_gdb_constraint_add_property(session, bad, docs, KIMIX_GDB_OP_EQUAL,
                                                    &scalar, 1),
                  KIMIX_GDB_OK))
            << "the refusals above were about the OP, not about the property";

        // A label condition keeps only membership: any_in (and every ordering op) on a
        // LABEL is refused with ERR_OP at ADD time, so the walk never sees one.
        uint32_t label_leg = 0;
        expect(eq(kimix_gdb_constraint_create(session, &label_leg), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_constraint_add_label(session, label_leg, node, KIMIX_GDB_OP_ANY_IN),
                  KIMIX_GDB_ERR_OP))
            << "a label condition tests membership only";
        expect(eq(kimix_gdb_constraint_add_label(session, label_leg, node, KIMIX_GDB_OP_GREATER),
                  KIMIX_GDB_ERR_OP));
        // Both refusals left the constraint EMPTY — and an empty constraint matches
        // everything, which is how this leg proves nothing half-built was stored.
        (void)edge_uids(session, v1, label_leg, KIMIX_GDB_ORIENTATION_OUTGOING, total);
        expect(eq(total, size_t{7}))
            << "v1 has seven outgoing edges, all matched by the empty constraint";
    };

    // The W007 / S3 leak scenario, edge-filter half: two entities are each visible on
    // their OWN evidence, the link between them is classified confidential by its
    // evidence, and a reader whose document set omits that evidence must not see the
    // link — not as an edge record, not through a neighbour that only it connects, and
    // not as a path over it. (The traverse_bfs_edges half belongs to the walk's owner.)
    "cabi_any_in_w007_s3_edge_filter"_test = [] {
        session_guard guard;
        kimix_gdb_session *session = guard.session;
        uint32_t const node = guard.label("node");
        uint32_t const docs = guard.property_type_array("docs", KIMIX_GDB_DTYPE_CHAR, 3);

        // alice and w007 carry public evidence of their own; `secret` is reachable only
        // over a confidential link and only has confidential evidence.
        kimix_gdb_guid const alice = guard.vertex(node, "alice");
        kimix_gdb_guid const w007 = guard.vertex(node, "w007");
        kimix_gdb_guid const secret = guard.vertex(node, "secret");
        auto const alice_block = string_array_block({"D-PUB-1", "D-PUB-2", ""});
        auto const w007_block = string_array_block({"D-PUB-3", "", ""});
        auto const secret_block = string_array_block({"W007", "S3", ""});
        auto const confidential_block = string_array_block({"W007", "S3", "E-OTHER"});
        auto const alice_value = string_array_property(alice_block, 3);
        auto const w007_value = string_array_property(w007_block, 3);
        auto const secret_value = string_array_property(secret_block, 3);
        auto const confidential_value = string_array_property(confidential_block, 3);
        expect(eq(kimix_gdb_vertex_property_add(session, alice, docs, &alice_value), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_vertex_property_add(session, w007, docs, &w007_value), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_vertex_property_add(session, secret, docs, &secret_value), KIMIX_GDB_OK));

        uint32_t const names[1]{docs};
        const kimix_gdb_property values[1]{confidential_value};
        kimix_gdb_guid const alice_to_w007 = [&] {
            kimix_gdb_guid edge{};
            expect(eq(kimix_gdb_edge_create(session, alice, w007, KIMIX_GDB_EDGE_DIRECTED, nullptr,
                                            0, names, values, 1, nullptr, &edge),
                      KIMIX_GDB_OK));
            return edge;
        }();
        kimix_gdb_guid const alice_to_secret = [&] {
            kimix_gdb_guid edge{};
            expect(eq(kimix_gdb_edge_create(session, alice, secret, KIMIX_GDB_EDGE_DIRECTED, nullptr,
                                            0, names, values, 1, nullptr, &edge),
                      KIMIX_GDB_OK));
            return edge;
        }();

        // The reader's constraint: docs ANY_IN the readable set. The SAME condition
        // filters EDGES in the edge reads (§2.6) and VERTICES in the neighbour/scan
        // reads (§5.10) — that is what makes this one condition the whole scenario.
        auto const readable_block = string_array_block({"D-PUB-1", "D-PUB-2", "D-PUB-3"});
        auto const readable_value = string_array_property(readable_block, 3);
        uint32_t reader = 0;
        expect(eq(kimix_gdb_constraint_create(session, &reader), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_constraint_add_property(session, reader, docs, KIMIX_GDB_OP_ANY_IN,
                                                    &readable_value, 3),
                  KIMIX_GDB_OK));

        // Both entities are visible on their own evidence; `secret` is not.
        size_t scan_count = 0;
        size_t scan_total = 0;
        expect(eq(kimix_gdb_scan_vertices(session, reader, 0, nullptr, 0, &scan_count,
                                          &scan_total),
                  KIMIX_GDB_OK));
        expect(eq(scan_total, size_t{2})) << "alice and w007 prove themselves; secret does not";
        kimix_gdb_guid visible[4]{};
        expect(eq(kimix_gdb_scan_vertices(session, reader, 4, visible, 4, &scan_count,
                                          &scan_total),
                  KIMIX_GDB_OK));
        bool saw_alice = false;
        bool saw_w007 = false;
        bool saw_secret = false;
        for (size_t i = 0; i < scan_count; ++i) {
            saw_alice = saw_alice || (visible[i].lo == alice.lo && visible[i].hi == alice.hi);
            saw_w007 = saw_w007 || (visible[i].lo == w007.lo && visible[i].hi == w007.hi);
            saw_secret = saw_secret || (visible[i].lo == secret.lo && visible[i].hi == secret.hi);
        }
        expect(saw_alice && saw_w007 && !saw_secret) << "the two vertices are visible, S3 is not";

        // The unfiltered baseline: BOTH confidential links exist, so what follows is
        // the filter's doing, not a missing edge.
        size_t total = 0;
        (void)edge_uids(session, alice, 0, KIMIX_GDB_ORIENTATION_OUTGOING, total);
        expect(eq(total, size_t{2})) << "two outgoing links exist";

        // Under the reader constraint the confidential edge is NOT returned...
        auto const filtered = edge_uids(session, alice, reader, KIMIX_GDB_ORIENTATION_OUTGOING,
                                        total);
        expect(filtered.empty()) << "the evidence filter hides the link";
        bool leaked = false;
        for (auto const &uid : filtered) {
            leaked = leaked || (uid.lo == alice_to_w007.lo && uid.hi == alice_to_w007.hi) ||
                     (uid.lo == alice_to_secret.lo && uid.hi == alice_to_secret.hi);
        }
        expect(!leaked);

        // ...and the neighbour read shows only the vertex that stands on its own
        // evidence: the neighbour reachable over the confidential link alone is gone.
        size_t neighbour_count = 0;
        size_t neighbour_total = 0;
        expect(eq(kimix_gdb_neighbors_of_vertex(session, alice, reader,
                                                KIMIX_GDB_ORIENTATION_OUTGOING, 0, nullptr, 0,
                                                &neighbour_count, &neighbour_total),
                  KIMIX_GDB_OK));
        expect(eq(neighbour_total, size_t{1})) << "w007 stands on its own evidence";
        kimix_gdb_guid neighbours[4]{};
        expect(eq(kimix_gdb_neighbors_of_vertex(session, alice, reader,
                                                KIMIX_GDB_ORIENTATION_OUTGOING, 4, neighbours, 4,
                                                &neighbour_count, &neighbour_total),
                  KIMIX_GDB_OK));
        expect(eq(neighbour_count, size_t{1}));
        expect(neighbours[0].lo == w007.lo && neighbours[0].hi == w007.hi);

        // The edge-filtered walk finds nothing over filtered edges: no path to w007
        // (the LINK is hidden even though the vertex is readable) and none to secret.
        size_t path_count = 0;
        expect(eq(kimix_gdb_shortest_path_edges(session, alice, w007, reader,
                                                KIMIX_GDB_ORIENTATION_OUTGOING,
                                                KIMIX_GDB_MAX_HOP_DEPTH, nullptr, 0, &path_count),
                  KIMIX_GDB_OK));
        expect(eq(path_count, size_t{0})) << "no path over a filtered-out edge";
        expect(eq(kimix_gdb_shortest_path_edges(session, alice, secret, reader,
                                                KIMIX_GDB_ORIENTATION_OUTGOING,
                                                KIMIX_GDB_MAX_HOP_DEPTH, nullptr, 0, &path_count),
                  KIMIX_GDB_OK));
        expect(eq(path_count, size_t{0}));
        // The same endpoints WITHOUT the filter do connect: the walk's answer changes
        // with the reader's evidence set, exactly as the scenario requires.
        expect(eq(kimix_gdb_shortest_path_edges(session, alice, w007, 0,
                                                KIMIX_GDB_ORIENTATION_OUTGOING,
                                                KIMIX_GDB_MAX_HOP_DEPTH, nullptr, 0, &path_count),
                  KIMIX_GDB_OK));
        expect(eq(path_count, size_t{2})) << "alice, w007";
        uint8_t exists = 0;
        expect(eq(kimix_gdb_exists_neighbor(session, alice, reader, KIMIX_GDB_ORIENTATION_BOTH,
                                            &exists),
                  KIMIX_GDB_OK));
        expect(exists == 1) << "w007 remains a readable neighbour; only the edge is hidden";
    };
 
    // -----------------------------------------------------------------------
    // ABI 8 (plan §4): the version, the property-type size type, the multi-start
    // BFS and the caller-chosen edge uid, driven through the C surface only.
    // ABI 9 adds the data-array kind and moves NO struct, so these pins survive it.
    // -----------------------------------------------------------------------
    "cabi_abi8_surface"_test = [] {
        expect(eq(KIMIX_GDB_ABI_VERSION, uint32_t{9}))
            << "a binding built for ABI 8 must refuse this library (D10)";
        expect(eq(kimix_gdb_abi_version(), uint32_t{9}));
        // The property-type record kept its SIZE: the ABI-7 uint32_t reserved split
        // into size_type + 3 reserved bytes, so no offset moved.
        static_assert(sizeof(kimix_gdb_property_type) == 24,
                      "kimix_gdb_property_type is 24 B before and after ABI 8");
        // The edge spec grew by one guid (it is a caller-built INPUT, never
        // persisted, and kimix_gdb_layout_info() does not report it — so it is pinned
        // here): 2 guids + uid + direction/reserved + 5 pointer/size fields.
        static_assert(sizeof(kimix_gdb_edge_spec) == 96,
                      "kimix_gdb_edge_spec is 96 B in ABI 8 (was 64 in ABI 7)");
        kimix_gdb_layout layout{};
        expect(eq(kimix_gdb_layout_info(&layout), KIMIX_GDB_OK));
        expect(eq(layout.abi_version, uint32_t{9}));
        expect(eq(layout.sizeof_property_type, uint32_t{24}));
    };

    "cabi_property_type_size_type_round_trip"_test = [] {
        session_guard guard;
        // Create with each size type; the unlimited count is normalized to 0.
        uint32_t fixed_id = 0;
        uint32_t max_id = 0;
        uint32_t unlimited_id = 0;
        expect(eq(kimix_gdb_property_type_create(guard.session, "fixed_docs", 10,
                                                 KIMIX_GDB_CONTAINER_ARRAY, KIMIX_GDB_DTYPE_CHAR,
                                                 KIMIX_GDB_SIZE_FIXED, 3, &fixed_id),
                  KIMIX_GDB_OK));
        expect(eq(kimix_gdb_property_type_create(guard.session, "max_docs", 8,
                                                 KIMIX_GDB_CONTAINER_ARRAY, KIMIX_GDB_DTYPE_CHAR,
                                                 KIMIX_GDB_SIZE_MAX, 4, &max_id),
                  KIMIX_GDB_OK));
        expect(eq(kimix_gdb_property_type_create(guard.session, "unlimited_docs", 14,
                                                 KIMIX_GDB_CONTAINER_ARRAY, KIMIX_GDB_DTYPE_CHAR,
                                                 KIMIX_GDB_SIZE_UNLIMITED, 77, &unlimited_id),
                  KIMIX_GDB_OK));
        // An unknown size type, a non-fixed single and a zero fixed/max count.
        uint32_t refused = 0;
        expect(eq(kimix_gdb_property_type_create(guard.session, "bad_size", 8,
                                                 KIMIX_GDB_CONTAINER_ARRAY, KIMIX_GDB_DTYPE_CHAR,
                                                 static_cast<uint8_t>(7), 3, &refused),
                  KIMIX_GDB_ERR_ARGUMENT));
        expect(eq(kimix_gdb_property_type_create(guard.session, "bad_single", 10,
                                                 KIMIX_GDB_CONTAINER_SINGLE, KIMIX_GDB_DTYPE_CHAR,
                                                 KIMIX_GDB_SIZE_MAX, 1, &refused),
                  KIMIX_GDB_ERR_ARGUMENT));
        expect(eq(kimix_gdb_property_type_create(guard.session, "bad_count", 9,
                                                 KIMIX_GDB_CONTAINER_ARRAY, KIMIX_GDB_DTYPE_CHAR,
                                                 KIMIX_GDB_SIZE_FIXED, 0, &refused),
                  KIMIX_GDB_ERR_COUNT));

        // get_all reports the size type, and the reserved bytes stay zero.
        size_t count = 0;
        size_t scratch_bytes = 0;
        expect(eq(kimix_gdb_property_type_get_all(guard.session, nullptr, 0, nullptr, 0, &count,
                                                  &scratch_bytes),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{3}));
        kimix::vector<kimix_gdb_property_type> items(count);
        kimix::vector<uint8_t> scratch(scratch_bytes);
        expect(eq(kimix_gdb_property_type_get_all(guard.session, items.data(), items.size(),
                                                  scratch.data(), scratch.size(), &count,
                                                  &scratch_bytes),
                  KIMIX_GDB_OK));
        expect(eq(items[0].size_type, static_cast<uint8_t>(KIMIX_GDB_SIZE_FIXED)));
        expect(eq(items[0].count, uint32_t{3}));
        expect(eq(items[1].size_type, static_cast<uint8_t>(KIMIX_GDB_SIZE_MAX)));
        expect(eq(items[1].count, uint32_t{4}));
        expect(eq(items[2].size_type, static_cast<uint8_t>(KIMIX_GDB_SIZE_UNLIMITED)));
        expect(eq(items[2].count, uint32_t{0})) << "unlimited is stored without a bound";
        for (auto const &item : items) {
            for (size_t r = 0; r < sizeof(item.reserved); ++r) {
                expect(eq(item.reserved[r], static_cast<uint8_t>(0)))
                    << "the reserved bytes stay zero-filled";
            }
        }
        // update carries it too, and a narrowing is refused while values exist.
        expect(eq(kimix_gdb_property_type_update(guard.session, fixed_id, "fixed_docs", 10,
                                                 KIMIX_GDB_CONTAINER_ARRAY,
                                                 KIMIX_GDB_DTYPE_CHAR, KIMIX_GDB_SIZE_MAX, 5),
                  KIMIX_GDB_OK));
        uint32_t const person = guard.label("person");
        auto const uid = guard.vertex(person, "sized");
        kimix::vector<uint8_t> const block = string_array_block({"reader-1", "reader-2"});
        kimix_gdb_property value{};
        value.kind = KIMIX_GDB_KIND_STRING_ARRAY;
        value.dtype = KIMIX_GDB_DTYPE_CHAR;
        value.count = 2;
        value.payload.data = block.data();
        value.payload.len = block.size();
        expect(eq(kimix_gdb_vertex_property_add(guard.session, uid, fixed_id, &value),
                  KIMIX_GDB_OK))
            << "the relaxed declaration accepts the 2-element value it now allows";
        expect(eq(kimix_gdb_property_type_update(guard.session, fixed_id, "fixed_docs", 10,
                                                 KIMIX_GDB_CONTAINER_ARRAY,
                                                 KIMIX_GDB_DTYPE_CHAR, KIMIX_GDB_SIZE_FIXED, 2),
                  KIMIX_GDB_ERR_READ_ONLY_PROPERTY_TYPE))
            << "a live value pins the declaration against a narrowing";
    };

    "cabi_edge_create_with_a_chosen_uid"_test = [] {
        session_guard guard;
        uint32_t const person = guard.label("person");
        auto const a = guard.vertex(person, "a");
        auto const b = guard.vertex(person, "b");
        auto const c = guard.vertex(person, "c");
        // A chosen uid is used verbatim.
        kimix_gdb_guid const chosen{0x1122334455667788ull, 0x99aabbccddeeff00ull};
        kimix_gdb_guid created{};
        expect(eq(kimix_gdb_edge_create(guard.session, a, b, KIMIX_GDB_EDGE_DIRECTED, nullptr, 0,
                                        nullptr, nullptr, 0, &chosen, &created),
                  KIMIX_GDB_OK));
        expect(created.lo == chosen.lo && created.hi == chosen.hi);
        kimix_gdb_guid origin{};
        kimix_gdb_guid target{};
        expect(eq(kimix_gdb_edge_endpoints(guard.session, chosen, &origin, &target), KIMIX_GDB_OK))
            << "the edge is resolvable under the caller's uid";
        expect(origin.lo == a.lo && origin.hi == a.hi);
        expect(target.lo == b.lo && target.hi == b.hi);
        // A live duplicate is the duplicate-id answer.
        kimix_gdb_guid refused{};
        expect(eq(kimix_gdb_edge_create(guard.session, a, c, KIMIX_GDB_EDGE_DIRECTED, nullptr, 0,
                                        nullptr, nullptr, 0, &chosen, &refused),
                  KIMIX_GDB_ERR_NON_UNIQUE_ID));
        // NULL mints one, and so does an ALL-ZERO guid (the zero-initialized spelling
        // an ABI-7 caller hands in).
        kimix_gdb_guid minted{};
        expect(eq(kimix_gdb_edge_create(guard.session, a, c, KIMIX_GDB_EDGE_DIRECTED, nullptr, 0,
                                        nullptr, nullptr, 0, nullptr, &minted),
                  KIMIX_GDB_OK));
        expect(!(minted.lo == 0 && minted.hi == 0));
        kimix_gdb_guid const zero{0, 0};
        kimix_gdb_guid minted_from_zero{};
        expect(eq(kimix_gdb_edge_create(guard.session, b, c, KIMIX_GDB_EDGE_DIRECTED, nullptr, 0,
                                        nullptr, nullptr, 0, &zero, &minted_from_zero),
                  KIMIX_GDB_OK));
        expect(!(minted_from_zero.lo == 0 && minted_from_zero.hi == 0))
            << "an all-zero uid means 'generate one', not 'use zero'";
        // After a free, the same uid is available again.
        expect(eq(kimix_gdb_edge_free(guard.session, chosen), KIMIX_GDB_OK));
        kimix_gdb_guid recreated{};
        expect(eq(kimix_gdb_edge_create(guard.session, a, b, KIMIX_GDB_EDGE_DIRECTED, nullptr, 0,
                                        nullptr, nullptr, 0, &chosen, &recreated),
                  KIMIX_GDB_OK));
        expect(recreated.lo == chosen.lo && recreated.hi == chosen.hi);
    };

    "cabi_edges_create_bulk_with_chosen_uids"_test = [] {
        session_guard guard;
        uint32_t const person = guard.label("person");
        auto const a = guard.vertex(person, "a");
        auto const b = guard.vertex(person, "b");
        auto const c = guard.vertex(person, "c");
        kimix_gdb_guid const first{0x1000ull, 0x2000ull};
        kimix_gdb_guid zero{0, 0};
        kimix_gdb_edge_spec specs[3]{};
        specs[0].origin = a;
        specs[0].target = b;
        specs[0].direction = KIMIX_GDB_EDGE_DIRECTED;
        specs[0].uid = first;
        specs[1].origin = b;
        specs[1].target = c;
        specs[1].direction = KIMIX_GDB_EDGE_DIRECTED;
        specs[1].uid = first; // a live duplicate within the same batch
        specs[2].origin = a;
        specs[2].target = c;
        specs[2].direction = KIMIX_GDB_EDGE_DIRECTED;
        specs[2].uid = zero; // all-zero = generate
        kimix_gdb_guid uids[3]{};
        int32_t statuses[3]{};
        expect(eq(kimix_gdb_edges_create_bulk(guard.session, specs, 3, /*continue_on_error=*/1,
                                              uids, statuses),
                  KIMIX_GDB_OK))
            << "continue_on_error isolates the duplicate row";
        expect(eq(statuses[0], KIMIX_GDB_OK));
        expect(uids[0].lo == first.lo && uids[0].hi == first.hi);
        expect(eq(statuses[1], KIMIX_GDB_ERR_NON_UNIQUE_ID))
            << "a duplicate uid fails exactly that row";
        expect(eq(statuses[2], KIMIX_GDB_OK));
        expect(!(uids[2].lo == 0 && uids[2].hi == 0)) << "the zero uid generated one";
        // The first row really is under the chosen uid.
        kimix_gdb_guid origin{};
        kimix_gdb_guid target{};
        expect(eq(kimix_gdb_edge_endpoints(guard.session, first, &origin, &target), KIMIX_GDB_OK));
          expect(origin.lo == a.lo && origin.hi == a.hi);
          expect(target.lo == b.lo && target.hi == b.hi);
      };

    // -----------------------------------------------------------------------
    // ABI 9: the data-array kind through the C surface only — the constants, a
    // write/read round trip per element flag (vertex AND edge, empty array
    // included), and every documented refusal.
    // -----------------------------------------------------------------------
    "cabi_data_array_round_trip"_test = [] {
        session_guard guard;
        uint32_t const node = guard.label("node");
        // fixed 3 (int64/double/string of exactly 3) and an unbounded one (empty,
        // and a 2-element blob) — one declaration per shape under test.
        uint32_t const payloads = guard.property_type_array("payloads", KIMIX_GDB_DTYPE_DATA, 3);
        uint32_t const any_payload =
            guard.property_type_array_sized("any_payload", KIMIX_GDB_DTYPE_DATA,
                                            KIMIX_GDB_SIZE_UNLIMITED, 0);
        // The constants are the ABI: the kind, the sentinel dtype, and the element
        // flags sharing the scalar kind numbering.
        expect(eq(static_cast<int>(KIMIX_GDB_KIND_DATA_ARRAY), 8));
        expect(eq(static_cast<int>(KIMIX_GDB_DTYPE_DATA), 200));
        expect(eq(static_cast<int>(KIMIX_GDB_DATA_INT64),
                  static_cast<int>(KIMIX_GDB_KIND_INT64)));
        expect(eq(static_cast<int>(KIMIX_GDB_DATA_DOUBLE),
                  static_cast<int>(KIMIX_GDB_KIND_DOUBLE)));
        expect(eq(static_cast<int>(KIMIX_GDB_DATA_STRING),
                  static_cast<int>(KIMIX_GDB_KIND_STRING)));
        expect(eq(static_cast<int>(KIMIX_GDB_DATA_BLOB), static_cast<int>(KIMIX_GDB_KIND_BLOB)));

        // ---- vertex: one element flag at a time, same declaration -------------
        kimix_gdb_guid const a = guard.vertex(node, "a");
        auto const ints = data_array_block_int64({1, -2, 3});
        auto const real = data_array_block_double({1.5, -0.25, 8.0});
        auto const texts = data_array_block_string({"alpha", "beta", "gamma"});
        auto const blobs = [&] {
            // A 2-element blob array, spelled by hand.
            auto block = data_array_header(KIMIX_GDB_DATA_BLOB, 2);
            push_u32(block, 2);
            block.push_back(0xde);
            block.push_back(0xad);
            push_u32(block, 1);
            block.push_back(0x7f);
            return block;
        }();

        struct Case {
            kimix::vector<uint8_t> const *block;
            kimix::string_view what;
        };
        kimix::vector<Case> const cases{
            {&ints, "int64 elements"},
            {&real, "double elements"},
            {&texts, "string elements"},
        };
        bool first_write = true;
        for (auto const &item : cases) {
            auto const value = data_array_property(*item.block, 3);
            // `add` creates the property, `set` replaces it: GDI's documented split
            // (Set -> ErrorNoProperty when absent), so the first kind uses add.
            int32_t const written =
                first_write ? kimix_gdb_vertex_property_add(guard.session, a, payloads, &value)
                            : kimix_gdb_vertex_property_set(guard.session, a, payloads, &value);
            first_write = false;
            expect(eq(written, KIMIX_GDB_OK)) << item.what;
            property_read page = read_properties([&](kimix_gdb_property_record *items, size_t cap,
                                                     uint8_t *scratch, size_t scratch_cap,
                                                     size_t *out_count, size_t *out_scratch) {
                return kimix_gdb_vertex_properties(guard.session, a, items, cap, scratch,
                                                   scratch_cap, out_count, out_scratch);
            });
            expect_data_array_record(page.find(payloads), page, *item.block, 3, item.what);
            expect(eq(page.scratch_bytes, item.block->size()))
                << "one property, so the scratch IS the block: handed out verbatim";
        }

        // A blob array on the unbounded declaration (a different element COUNT too,
        // which is exactly the freedom the kind buys).
        auto const blob_value = data_array_property(blobs, 2);
        expect(eq(kimix_gdb_vertex_property_add(guard.session, a, any_payload, &blob_value),
                  KIMIX_GDB_OK));
        property_read blob_page = read_properties([&](kimix_gdb_property_record *items,
                                                      size_t cap, uint8_t *scratch,
                                                      size_t scratch_cap, size_t *out_count,
                                                      size_t *out_scratch) {
            return kimix_gdb_vertex_properties(guard.session, a, items, cap, scratch,
                                               scratch_cap, out_count, out_scratch);
        });
        expect_data_array_record(blob_page.find(any_payload), blob_page, blobs, 2, "blob elements");

        // An EMPTY array is a VALUE: the 8-byte header with count 0.
        auto const empty_block = data_array_header(KIMIX_GDB_DATA_STRING, 0);
        auto const empty_value = data_array_property(empty_block, 0);
        expect(eq(kimix_gdb_vertex_property_set(guard.session, a, any_payload, &empty_value),
                  KIMIX_GDB_OK));
        property_read empty_page = read_properties([&](kimix_gdb_property_record *items,
                                                       size_t cap, uint8_t *scratch,
                                                       size_t scratch_cap, size_t *out_count,
                                                       size_t *out_scratch) {
            return kimix_gdb_vertex_properties(guard.session, a, items, cap, scratch,
                                               scratch_cap, out_count, out_scratch);
        });
        expect_data_array_record(empty_page.find(any_payload), empty_page, empty_block, 0,
                                 "an empty data array");

        // ---- the edge twins: create-time payload, then a set to another kind ---
        kimix_gdb_guid const b = guard.vertex(node, "b");
        uint32_t const names[1]{payloads};
        auto const create_value = data_array_property(ints, 3);
        kimix_gdb_property const create_values[1]{create_value};
        kimix_gdb_guid edge{};
        expect(eq(kimix_gdb_edge_create(guard.session, a, b, KIMIX_GDB_EDGE_DIRECTED, nullptr, 0,
                                        names, create_values, 1, nullptr, &edge),
                  KIMIX_GDB_OK));
        property_read edge_page = read_properties([&](kimix_gdb_property_record *items,
                                                      size_t cap, uint8_t *scratch,
                                                      size_t scratch_cap, size_t *out_count,
                                                      size_t *out_scratch) {
            return kimix_gdb_edge_properties(guard.session, edge, items, cap, scratch,
                                             scratch_cap, out_count, out_scratch);
        });
        expect_data_array_record(edge_page.find(payloads), edge_page, ints, 3,
                                 "edge create-time payload");

        auto const set_value = data_array_property(texts, 3);
        expect(eq(kimix_gdb_edge_property_set(guard.session, edge, payloads, &set_value),
                  KIMIX_GDB_OK));
        property_read set_page = read_properties([&](kimix_gdb_property_record *items,
                                                     size_t cap, uint8_t *scratch,
                                                     size_t scratch_cap, size_t *out_count,
                                                     size_t *out_scratch) {
            return kimix_gdb_edge_properties(guard.session, edge, items, cap, scratch,
                                             scratch_cap, out_count, out_scratch);
        });
        expect_data_array_record(set_page.find(payloads), set_page, texts, 3,
                                 "edge set property (a different element kind)");

        // ---- the declaration round-trips as (array, data (200)) ---------------
        kimix_gdb_property_type types[4]{};
        size_t type_count = 0;
        size_t type_scratch = 0;
        expect(eq(kimix_gdb_vertex_property_types(guard.session, a, nullptr, 0, nullptr, 0,
                                                  &type_count, &type_scratch),
                  KIMIX_GDB_OK));
        kimix::vector<uint8_t> name_scratch(type_scratch == 0 ? 1 : type_scratch);
        expect(eq(kimix_gdb_vertex_property_types(guard.session, a, types, 4,
                                                  name_scratch.data(), name_scratch.size(),
                                                  &type_count, &type_scratch),
                  KIMIX_GDB_OK));
        bool saw_declaration = false;
        for (size_t i = 0; i < type_count; ++i) {
            if (types[i].id != any_payload) {
                continue;
            }
            saw_declaration = true;
            expect(types[i].container == KIMIX_GDB_CONTAINER_ARRAY);
            expect(types[i].dtype == KIMIX_GDB_DTYPE_DATA);
            expect(types[i].size_type == KIMIX_GDB_SIZE_UNLIMITED);
            expect(eq(types[i].count, uint32_t{0})) << "unlimited carries no bound";
        }
        expect(saw_declaration);
    };

    "cabi_data_array_refusals"_test = [] {
        session_guard guard;
        uint32_t const node = guard.label("node");
        uint32_t const payloads = guard.property_type_array("payloads", KIMIX_GDB_DTYPE_DATA, 3);
        uint32_t const ints = guard.property_type_array("ints", KIMIX_GDB_DTYPE_INT64, 3);
        kimix_gdb_guid const a = guard.vertex(node, "a");

        auto const good = data_array_block_int64({1, 2, 3});
        // (a) an illegal element flag — including the reserved 0xFF — is ERR_DATATYPE,
        //     distinct from every count-shaped refusal (D6).
        for (uint8_t const flag : {uint8_t{0}, uint8_t{5}, uint8_t{8}, uint8_t{0xff}}) {
            auto block = data_array_header(flag, 0);
            auto const value = data_array_property(block, 0);
            expect(eq(kimix_gdb_vertex_property_add(guard.session, a, payloads, &value),
                      KIMIX_GDB_ERR_DATATYPE))
                << "element flag " << static_cast<int>(flag);
        }
        // (b) a non-zero reserved byte is a malformed block: ERR_COUNT.
        auto reserved = good;
        reserved[2] = 1;
        auto const bad_reserved = data_array_property(reserved, 3);
        expect(eq(kimix_gdb_vertex_property_add(guard.session, a, payloads, &bad_reserved),
                  KIMIX_GDB_ERR_COUNT));
        // (c) an exact-count disagreement: ERR_COUNT.
        auto const bad_count = data_array_property(good, 2);
        expect(eq(kimix_gdb_vertex_property_add(guard.session, a, payloads, &bad_count),
                  KIMIX_GDB_ERR_COUNT))
            << "count is the EXACT element count, not a truncation hint";
        // (d) a truncated block / no block at all: ERR_COUNT.
        auto truncated = good;
        truncated.resize(good.size() - 1);
        auto const bad_truncated = data_array_property(truncated, 3);
        expect(eq(kimix_gdb_vertex_property_add(guard.session, a, payloads, &bad_truncated),
                  KIMIX_GDB_ERR_COUNT));
        kimix_gdb_property null_payload{};
        null_payload.kind = KIMIX_GDB_KIND_DATA_ARRAY;
        null_payload.dtype = KIMIX_GDB_DTYPE_DATA;
        expect(eq(kimix_gdb_vertex_property_add(guard.session, a, payloads, &null_payload),
                  KIMIX_GDB_ERR_COUNT));
        // (e) the guard sits in all three write modes, on both entity forms.
        expect(eq(kimix_gdb_vertex_property_set(guard.session, a, payloads, &bad_count),
                  KIMIX_GDB_ERR_COUNT));
        expect(eq(kimix_gdb_vertex_property_update(guard.session, a, payloads, &bad_count),
                  KIMIX_GDB_ERR_COUNT));
        kimix_gdb_guid const b = guard.vertex(node, "b");
        kimix_gdb_guid const edge = guard.edge(a, b);
        expect(eq(kimix_gdb_edge_property_add(guard.session, edge, payloads, &bad_count),
                  KIMIX_GDB_ERR_COUNT));
        // ... and a create-time payload refuses the WHOLE vertex.
        uint32_t const create_names[1]{payloads};
        kimix_gdb_property const create_values[1]{bad_count};
        kimix_gdb_vertex_spec spec{};
        spec.key.label = node;
        kimix_gdb_bytes const external{reinterpret_cast<uint8_t const *>("c"), 1};
        spec.key.external_id = external;
        spec.property_names = create_names;
        spec.properties = create_values;
        spec.property_count = 1;
        kimix_gdb_guid refused_uid{};
        uint8_t warning = 0;
        expect(eq(kimix_gdb_vertex_create(guard.session, &spec, &refused_uid, &warning),
                  KIMIX_GDB_ERR_COUNT));
        // Nothing was stored by ANY of the refusals.
        size_t count = 0;
        size_t scratch_bytes = 0;
        expect(eq(kimix_gdb_vertex_properties(guard.session, a, nullptr, 0, nullptr, 0, &count,
                                              &scratch_bytes),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{0})) << "a refused write leaves the bag empty";

        // (f) the value against a non-`data` declaration is ERR_WRONG_TYPE, in both
        //     directions (the value's data_type() is the sentinel).
        auto const good_value = data_array_property(good, 3);
        expect(eq(kimix_gdb_vertex_property_add(guard.session, a, ints, &good_value),
                  KIMIX_GDB_ERR_WRONG_TYPE));
        auto const int_array = int64_array_property(kimix::vector<int64_t>{1, 2, 3});
        expect(eq(kimix_gdb_vertex_property_add(guard.session, a, payloads, &int_array),
                  KIMIX_GDB_ERR_WRONG_TYPE));
        // ... and (single, data) cannot even be declared: there is no array to describe.
        uint32_t lone_id = 12345;
        expect(eq(kimix_gdb_property_type_create(guard.session, "lone", 4,
                                                 KIMIX_GDB_CONTAINER_SINGLE,
                                                 KIMIX_GDB_DTYPE_DATA, KIMIX_GDB_SIZE_FIXED, 1,
                                                 &lone_id),
                  KIMIX_GDB_ERR_ARGUMENT));

        // (g) a data array is never an aggregate operand (array => not scalar => ERR_OP).
        expect(eq(kimix_gdb_vertex_property_add(guard.session, a, payloads, &good_value),
                  KIMIX_GDB_OK));
        kimix_gdb_aggregate_result result{};
        expect(eq(kimix_gdb_aggregate(guard.session, 0, payloads, KIMIX_GDB_AGGREGATE_SUM,
                                      nullptr, 0, &result),
                  KIMIX_GDB_ERR_OP));
        expect(eq(kimix_gdb_aggregate(guard.session, 0, payloads, KIMIX_GDB_AGGREGATE_MIN,
                                      nullptr, 0, &result),
                  KIMIX_GDB_ERR_OP));

        // (h) a data array is never an ANY_IN SET (V1 non-goal): ERR_OP_DATATYPE_MISMATCH.
        uint32_t constraint = 0;
        expect(eq(kimix_gdb_constraint_create(guard.session, &constraint), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_constraint_add_property(guard.session, constraint, payloads,
                                                    KIMIX_GDB_OP_ANY_IN, &good_value, 3),
                  KIMIX_GDB_ERR_OP_DATATYPE_MISMATCH));
        expect(eq(kimix_gdb_constraint_free(guard.session, constraint), KIMIX_GDB_OK));
    };
}
