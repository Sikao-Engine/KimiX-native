/*
 * utils.h — key scheme, vertex-record codec and cold storage for kimix-graph-db.
 *
 * Everything that is neither a Vertex nor the Graph container lives here: the
 * on-disk key/value layout, the byte-exact record codec, and a thin RAII
 * wrapper over <core/lmdb.h>.
 *
 * KEY SCHEME (LMDB database, one entry per vertex)
 *   "v/" + 16-byte binary GUID   -> vertex record   (kVertexKeySize = 18 bytes)
 *   "meta/root"                  -> reserved meta key (format version + the
 *                                   timestamp of the last full save). Nothing in
 *                                   this subtask writes it; the key is reserved
 *                                   so a later writer cannot collide with a
 *                                   vertex key.
 * The 16 GUID bytes are Guid::to_binary().data0 then .data1, each written
 * little-endian — the same byte order <core/md5.h> documents for a digest, so
 * a key is byte-identical on every host.
 *
 * VERTEX RECORD (all fields little-endian, no padding, no alignment)
 *   [ 4B magic "GDB1" ][ 16B guid ][ 16B content MD5 ]
 *   [ 4B n_depending  ][ 4B n_depended ]
 *   [ n_depending x 16B guid ][ n_depended x 16B guid ][ content bytes ]
 * kVertexRecordHeaderSize (44) covers magic + guid + MD5 + the two counts; the
 * dependency tables and the content follow with no separator. Decoding is
 * total: any truncated, mis-magicked or inconsistent buffer is reported as a
 * failure (return false) — a corrupt record never aborts and never reads out of
 * bounds.
 *
 * COLD STORAGE
 *   ColdStorage is a thin RAII front end over kimix::LMDB: open/close/valid,
 *   path(), one record read, a full scan, a batched transactional write, batch
 *   removal, per-vertex removal and copy_to() for checkpoints.
 *   NOTE: kimix::LMDB itself is fatal-on-error by design (it reports a failing
 *   mdb_* call to stderr and aborts, see <core/lmdb.h>); the bool returns below
 *   only report the recoverable conditions (storage not open, empty path).
 *
 * Example:
 *   kimix::graph_db::ColdStorage cold;
 *   if (cold.open(kimix::graph_db::default_cold_dir())) {
 *       kimix::graph_db::VertexRecord rec;      // guid/hash/dep guids/content
 *       rec.guid = kimix::Guid{true};
 *       kimix::vector<std::byte> blob;           // the opaque content
 *       blob.push_back(std::byte{'a'});
 *       rec.content = blob;
 *       rec.content_hash = kimix::graph_db::md5_of(rec.content);
 *       kimix::vector<kimix::graph_db::VertexRecord> batch;
 *       batch.push_back(std::move(rec));
 *       cold.save_records(std::move(batch));   // one LMDB transaction
 *
 *       kimix::vector<kimix::graph_db::VertexRecord> loaded;
 *       size_t skipped = 0;
 *       cold.load_vertex_records(loaded, &skipped); // corrupt values skipped
 *   }
 */
#pragma once

  #include "graph_db_api.h"
  #include "graph_db_options.h"
  #include "property.h"

#include <core/guid.h>
#include <core/lmdb.h>
#include <core/md5.h>
#include <core/memory.h>
#include <core/stl/filesystem.h>
#include <core/stl/memory.h>
#include <core/stl/optional.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace kimix::graph_db {

// ---------------------------------------------------------------------------
// Key scheme / format constants
// ---------------------------------------------------------------------------

// Prefix of every vertex key: "v/" (2 bytes) + the 16 binary guid bytes.
inline constexpr kimix::string_view kVertexKeyPrefix = "v/";
// Total length of a vertex key: 2-byte prefix + 16-byte guid.
inline constexpr size_t kVertexKeySize = 18;
// Prefix of every edge key: "e/" + the 16 binary edge id bytes. Same length as a
// vertex key, so a key family is decided by its prefix byte alone.
inline constexpr kimix::string_view kEdgeKeyPrefix = "e/";
inline constexpr size_t kEdgeKeySize = 18;
// Prefix of the schema tables ("s/labels", "s/proptypes").
inline constexpr kimix::string_view kSchemaKeyPrefix = "s/";
// Reserved for future secondary indexes; NEVER written by this library.
inline constexpr kimix::string_view kIndexKeyPrefix = "i/";
// Graph-level metadata (never a vertex key: 'm' is no key family prefix).
inline constexpr kimix::string_view kMetaRootKey = "meta/root";
// Magic that starts every v1 (GDB1) vertex record value.
inline constexpr kimix::string_view kRecordMagic = "GDB1";
// Magic of the v2 vertex record. A store may mix GDB1 and GDB2 records: the
// decoder dispatches on the magic, so a v1 store loads unchanged and a v2 store
// keeps working after a partial downgrade is impossible (a newer FORMAT is
// refused through the meta record, see kFormatVersion).
inline constexpr kimix::string_view kRecordMagicV2 = "GDB2";
// Magic of an edge record ("GDE2" = graph database edge, format 2).
inline constexpr kimix::string_view kEdgeRecordMagic = "GDE2";
// Magic of the meta record and of the two schema tables.
inline constexpr kimix::string_view kMetaRecordMagic = "GDM2";
inline constexpr kimix::string_view kSchemaTableMagic = "GDS2";
inline constexpr size_t kRecordMagicSize = 4;
// Format version this build writes. 1 = the original layout (no meta record, no
// edge records); 2 = meta + schema + edge records + typed vertex records;
// 3 = a property-type declaration carries a size type (ABI 8: the first of the
// per-entry reserved bytes, 0 == fixed, so a v2 store loads unchanged and a v3
// store is refused by an older build through the meta record's range check).
inline constexpr uint32_t kFormatVersion = 3;
// magic + guid + md5 + n_depending + n_depended = 4 + 16 + 16 + 4 + 4.
inline constexpr size_t kVertexRecordHeaderSize = 44;
inline constexpr size_t kRecordGuidOffset = 4;
inline constexpr size_t kRecordHashOffset = 20;
inline constexpr size_t kRecordDependingCountOffset = 36;
inline constexpr size_t kRecordDependedCountOffset = 40;
// Default directory name of the cold store (relative to the working dir).
inline constexpr kimix::string_view kColdDirName = "graph_db.cold";

// ---------------------------------------------------------------------------
// Key helpers
// ---------------------------------------------------------------------------

// "v/" + the 16 little-endian guid bytes (Guid::to_binary().data0 then data1).
[[nodiscard]] KIMIX_GRAPH_DB_API std::array<std::byte, kVertexKeySize>
guid_key(kimix::Guid const &guid) noexcept;

// Same bytes, appended into a byte vector (the shape LMDBWriteCommand wants).
KIMIX_GRAPH_DB_API void guid_key(kimix::Guid const &guid,
                                 kimix::vector<std::byte> &out);

// Parse a key produced by guid_key(). Returns nullopt when the span is not
// exactly kVertexKeySize bytes or does not carry the "v/" prefix.
[[nodiscard]] KIMIX_GRAPH_DB_API kimix::optional<kimix::Guid>
guid_from_key(kimix::span<const std::byte> key) noexcept;

// "e/" + the 16 little-endian edge-id bytes, and its parser. The edge key space
// is a sibling of the vertex space, so one scan can be partitioned by prefix.
[[nodiscard]] KIMIX_GRAPH_DB_API std::array<std::byte, kEdgeKeySize>
edge_key(kimix::Guid const &edge_id) noexcept;
KIMIX_GRAPH_DB_API void edge_key(kimix::Guid const &edge_id,
                                 kimix::vector<std::byte> &out);
[[nodiscard]] KIMIX_GRAPH_DB_API kimix::optional<kimix::Guid>
edge_from_key(kimix::span<const std::byte> key) noexcept;

  // MD5 of an opaque byte blob (the content hash cached on a Vertex).
  [[nodiscard]] KIMIX_GRAPH_DB_API kimix::MD5
  md5_of(kimix::span<const std::byte> bytes) noexcept;

  // The policy-aware content digest (plan v3 §4.5): md5 for the historical
  // default, XXH3-128 behind GraphOptions::content_digest. Both are 128-bit
  // values carried in the same kimix::MD5 container, so callers (Vertex,
  // serializer) switch policy without a type change.
  [[nodiscard]] KIMIX_GRAPH_DB_API kimix::MD5
  content_digest_of(kimix::span<const std::byte> bytes, ContentDigest policy) noexcept;

// ---------------------------------------------------------------------------
// Vertex record
// ---------------------------------------------------------------------------

/*
 * The serialized form of one vertex: exactly what encode_vertex_record() writes
 * and decode_vertex_record() reads. It is deliberately independent of class
 * Vertex so the codec (and the later serializer) can be used without a live
 * graph — the dependency lists are GUIDs, never pointers.
 */
struct KIMIX_GRAPH_DB_API VertexRecord {
    kimix::Guid guid;
    kimix::MD5 content_hash;
    // Vertices this one depends on (outgoing edges).
    kimix::vector<kimix::Guid> depending_guids;
    // Vertices that depend on this one (reverse edges).
    kimix::vector<kimix::Guid> depended_guids;
    // Opaque content blob (see the TODO on Vertex::_content).
    kimix::vector<std::byte> content;
};

// Serialized size of `record` in bytes (header + deps + content).
[[nodiscard]] KIMIX_GRAPH_DB_API size_t
encoded_record_size(VertexRecord const &record) noexcept;

// Encode `record` into `out`. `out` is cleared first; allocate-on-demand, no
// exceptions (an exhausted allocator aborts, see <core/stl/memory.h>).
KIMIX_GRAPH_DB_API void encode_vertex_record(VertexRecord const &record,
                                            kimix::vector<std::byte> &out);

// Decode a vertex record — a "GDB1" one written by encode_vertex_record() OR a
// "GDB2" one, which lands in the same VertexRecord through its compatibility
// projection (guid, hash, both dependency tables, content; the GDI fields need
// decode_vertex_record_v2). Returns false — leaving `out` in an unspecified but
// destructible state — for a truncated buffer, a wrong magic, or a record whose
// counts do not fit. Never aborts, never reads past `bytes`.
[[nodiscard]] KIMIX_GRAPH_DB_API bool
decode_vertex_record(kimix::span<const std::byte> bytes,
                     VertexRecord &out) noexcept;

// ---------------------------------------------------------------------------
// Vertex record v2 ("GDB2")
// ---------------------------------------------------------------------------

/*
 * The v2 vertex record: everything a v1 record holds EXCEPT the dependency guid
 * lists, plus the GDI data model.
 *
 *   [4B "GDB2"][16B guid][16B content MD5]
 *   [4B key label][4B n_labels][4B n_props][4B n_dep][4B n_depended][4B ext len]
 *   [4B reserved]
 *   [ext id bytes][n_labels x 4B label id][n_props x property entry]
 *   [n_dep x 16B guid][n_depended x 16B guid][content bytes]
 *
 * THE EDGE RECORDS ARE AUTHORITATIVE, the two dependency tables are a
 * COMPATIBILITY PROJECTION of them (the same shape a v1 record carried). The
 * load adopts the "e/" records first and only synthesizes an edge from a link
 * that no edge record covers — which is exactly what a v1 store needs (it has no
 * edge records at all) and what keeps a hand-edited or partially written store
 * loadable. Writing both costs 32 bytes per edge on disk, once, which buys:
 *   - a v1 store and a v2 store have the same in-memory result,
 *   - `dependent`-only information (an incoming edge whose source record is
 *     missing) still survives,
 *   - an older reader's expectation that a vertex record describes its
 *     adjacency keeps holding for the tools that decode records directly.
 *
 * `key_label` == 0 means the vertex has no GDI key (a container-API vertex, or
 * one migrated from a v1 store).
 */
struct KIMIX_GRAPH_DB_API VertexRecordV2 {
    kimix::Guid guid;
    kimix::MD5 content_hash;
    LabelId key_label{kNoLabel};
    // Raw external-id bytes (an ExternalId's payload).
    kimix::vector<std::byte> external_id;
    kimix::vector<LabelId> labels;
    PropertyList properties;
    kimix::vector<std::byte> content;
    // Compatibility projection of the edge set (see the layout note above): the
    // outgoing and incoming neighbour guids. Filled by a save, verified by a load.
    kimix::vector<kimix::Guid> depending_guids;
    kimix::vector<kimix::Guid> depended_guids;
};

[[nodiscard]] KIMIX_GRAPH_DB_API size_t
encoded_vertex_record_v2_size(VertexRecordV2 const &record) noexcept;
KIMIX_GRAPH_DB_API void encode_vertex_record_v2(VertexRecordV2 const &record,
                                                kimix::vector<std::byte> &out);
// Requires the "GDB2" magic.
[[nodiscard]] KIMIX_GRAPH_DB_API bool
decode_vertex_record_v2(kimix::span<const std::byte> bytes, VertexRecordV2 &out) noexcept;
// Dispatches on the magic: a "GDB2" record fills the whole struct, a "GDB1"
// record fills guid/content/hash plus the two dependency tables and leaves the
// GDI fields (key, labels, properties) empty; `was_v1` says which. A buffer with
// neither magic fails.
[[nodiscard]] KIMIX_GRAPH_DB_API bool
decode_any_vertex_record(kimix::span<const std::byte> bytes, VertexRecordV2 &out,
                         bool &was_v1) noexcept;

// ---------------------------------------------------------------------------
// Meta record ("meta/root") — format version negotiation
// ---------------------------------------------------------------------------

/*
 * What a store says about itself. Written on every full save (and on an
 * incremental save only when it changed), read by every load.
 *
 *   absent key            -> format 1 (a v1 store: no metadata at all)
 *   format_version <= 2   -> loadable; a record whose version is 1 but whose
 *                            meta is present is treated as format 1
 *   format_version > 2    -> REFUSED with error_bad_file, never partially loaded
 *
 * `store_uuid` identifies this store instance (a copy_to() checkpoint keeps it),
 * `schema_version` is bumped by every schema mutation so a load can tell whether
 * the schema tables must be re-read, and the two next_*_id counters continue the
 * registries' dense id allocation across a save/load cycle.
 */
struct KIMIX_GRAPH_DB_API MetaRecord {
    uint32_t format_version{kFormatVersion};
    kimix::Guid store_uuid;
    uint64_t last_full_save_ms{0};
    uint64_t last_incr_save_ms{0};
    uint32_t schema_version{0};
    uint32_t next_label_id{1};
    uint32_t next_prop_name_id{1};
    kimix::string graph_name;
};

// True when this build can load a store whose meta record says `format_version`.
[[nodiscard]] KIMIX_GRAPH_DB_API bool
format_version_supported(uint32_t format_version) noexcept;

[[nodiscard]] KIMIX_GRAPH_DB_API size_t
encoded_meta_size(MetaRecord const &record) noexcept;
KIMIX_GRAPH_DB_API void encode_meta_record(MetaRecord const &record,
                                           kimix::vector<std::byte> &out);
[[nodiscard]] KIMIX_GRAPH_DB_API bool
decode_meta_record(kimix::span<const std::byte> bytes, MetaRecord &out) noexcept;

// ---------------------------------------------------------------------------
// Filesystem helpers
// ---------------------------------------------------------------------------

// Create `dir` (and its parents) when missing. Returns false only when the
// filesystem reports an error; an existing directory counts as success.
[[nodiscard]] KIMIX_GRAPH_DB_API bool
ensure_dir(kimix::filesystem::path const &dir) noexcept;

// Default cold-storage directory: kColdDirName in the working directory.
[[nodiscard]] KIMIX_GRAPH_DB_API kimix::filesystem::path default_cold_dir();

// ---------------------------------------------------------------------------
// Cold storage
// ---------------------------------------------------------------------------

// One raw key/value pair copied out of a cold-storage scan. The iterator of
// kimix::LMDB yields spans into the memory-mapped file that die on the next
// ++, so a scan that needs to outlive the cursor must copy (this type).
struct KIMIX_GRAPH_DB_API ColdRecord {
    kimix::vector<std::byte> key;
    kimix::vector<std::byte> value;
};

/*
 * Thin RAII wrapper over one kimix::LMDB environment. Move-only; a
 * default-constructed or closed instance reports false from valid() and every
 * operation returns false without touching the disk.
 *
 * This is one of the two backends behind the internal cold_store.h seam
 * (kimix::graph_db::ColdStore): GraphSerializer and GraphDeserializer take their
 * store through that handle, and Database is where the handle is built. This
 * class stays a complete, standalone type — the seam borrows it, it does not
 * wrap or hide it — because the test suites construct and drive a ColdStorage
 * directly.
 */
class KIMIX_GRAPH_DB_API ColdStorage : public kimix::IOperatorNewBase {
public:
    // 64 GiB default map size, the same default kimix::LMDB uses.
    static constexpr size_t default_map_size = 1024ull * 1024ull * 1024ull * 64ull;

    ColdStorage() noexcept = default;
    // Opens (and creates) the environment in `dir`.
    explicit ColdStorage(kimix::filesystem::path const &dir,
                         size_t map_size = default_map_size) noexcept;
    ColdStorage(ColdStorage const &) = delete;
    ColdStorage &operator=(ColdStorage const &) = delete;
    ColdStorage(ColdStorage &&) noexcept = default;
    ColdStorage &operator=(ColdStorage &&) noexcept = default;
    ~ColdStorage() noexcept = default;

    // Open (and create) the environment in `dir`. Returns false for an empty
    // path; an LMDB-level failure inside kimix::LMDB is fatal by design.
    bool open(kimix::filesystem::path const &dir,
              size_t map_size = default_map_size) noexcept;
    void close() noexcept;
    [[nodiscard]] bool valid() const noexcept;
    explicit operator bool() const noexcept { return valid(); }
    [[nodiscard]] kimix::filesystem::path path() const noexcept;

    // Read the record of `guid`. Returns false when the key is absent or the
    // stored value does not decode (corrupt record — callers skip the vertex).
    [[nodiscard]] bool read_record(kimix::Guid const &guid,
                                   VertexRecord &out) const noexcept;

    // Full scan: every key/value pair in the store, copied out of the mapped
    // region (the LMDB iterator is holding one cursor, so this is sequential).
    [[nodiscard]] bool
    load_all_records(kimix::vector<ColdRecord> &out) const noexcept;

    // Scan only the pairs whose key starts with `prefix` — how the load path
    // separates the vertex ("v/"), edge ("e/") and schema ("s/") families of ONE
    // LMDB database (kimix::LMDB has a single DBI and one cursor, so the
    // partition is by prefix and this filter is the only thing that keeps the
    // loader from treating an edge record as a vertex).
    [[nodiscard]] bool
    scan_prefix(kimix::string_view prefix,
                kimix::vector<ColdRecord> &out) const noexcept;

    // The store's meta record. read_meta() returns false when the key is absent
    // (a v1 store) or the value does not decode; write_meta() writes/replaces it
    // in one transaction.
    [[nodiscard]] bool read_meta(MetaRecord &out) const noexcept;
    [[nodiscard]] bool write_meta(MetaRecord const &record) const noexcept;

    // Scan + decode in one call: every "v/" key that decodes goes into `out`,
    // corrupt/foreign records are counted in `skipped` (may be null) and
    // skipped. Returns false only when the store is not open.
    [[nodiscard]] bool
    load_vertex_records(kimix::vector<VertexRecord> &out,
                        size_t *skipped = nullptr) const noexcept;

    // Apply a batch of write commands in ONE transaction (atomic incremental
    // save). The command keys must already be guid_key() spellings.
    [[nodiscard]] bool
    save_batch(kimix::vector<kimix::LMDBWriteCommand> &&commands) const noexcept;

    // Encode each record (parallel-friendly in the later serializer: the
    // encoding itself is pure) and write them in one transaction.
    [[nodiscard]] bool
    save_records(kimix::vector<VertexRecord> &&records) const noexcept;

    // Delete a batch of raw keys in one transaction.
    [[nodiscard]] bool
    remove_keys(kimix::vector<kimix::vector<std::byte>> &&keys) const noexcept;

    // Delete the record of `guid` (no-op when absent).
    [[nodiscard]] bool remove_record(kimix::Guid const &guid) const noexcept;

    // Compacted snapshot of the whole store into `target`.
    [[nodiscard]] bool
    copy_to(kimix::filesystem::path const &target) const noexcept;

private:
    kimix::LMDB _db;
    kimix::filesystem::path _path;
};

} // namespace kimix::graph_db