/*
 * utils.cpp — key scheme, vertex-record codec and cold storage.
 * See utils.h for the layout documented byte by byte; the encoders below are
 * the single place that layout is materialised, so the two must be kept in
 * sync.
 *
 * Portability notes:
 *  - Every multi-byte field is written/read byte by byte in little-endian
 *    order, so the record is identical on any host and no struct is ever
 *    memcpy'd to or from the disk.
 *  - Decoding is total: each read is bounds-checked against the buffer size
 *    before it happens, so a truncated or hostile record is a `false`, never a
 *    crash and never an abort.
 *  - No exceptions: kimix::LMDB reports fatal LMDB errors itself (documented in
 *    utils.h); the bool results here cover the recoverable conditions only.
 */
#include "utils.h"

#include "binary_le.h"

  #include <core/md5.h>
  #include <core/stl/hash.h>
  #include <core/stl/string.h>

#include <cstring>
#include <utility>

namespace kimix::graph_db {

namespace src_graph_db_utils {

// --- little-endian scalar helpers (no alignment assumptions) ----------------

inline void write_u32le(std::byte *dst, uint32_t value) noexcept {
    dst[0] = static_cast<std::byte>(value & 0xffu);
    dst[1] = static_cast<std::byte>((value >> 8) & 0xffu);
    dst[2] = static_cast<std::byte>((value >> 16) & 0xffu);
    dst[3] = static_cast<std::byte>((value >> 24) & 0xffu);
}

inline uint32_t read_u32le(std::byte const *src) noexcept {
    return static_cast<uint32_t>(std::to_integer<uint32_t>(src[0])) |
           (static_cast<uint32_t>(std::to_integer<uint32_t>(src[1])) << 8) |
           (static_cast<uint32_t>(std::to_integer<uint32_t>(src[2])) << 16) |
           (static_cast<uint32_t>(std::to_integer<uint32_t>(src[3])) << 24);
}

inline void write_u64le(std::byte *dst, uint64_t value) noexcept {
    write_u32le(dst + 0, static_cast<uint32_t>(value & 0xffffffffull));
    write_u32le(dst + 4, static_cast<uint32_t>((value >> 32) & 0xffffffffull));
}

inline uint64_t read_u64le(std::byte const *src) noexcept {
    return static_cast<uint64_t>(read_u32le(src + 0)) |
           (static_cast<uint64_t>(read_u32le(src + 4)) << 32);
}

// Appends `count` bytes of `text` and returns the advanced cursor.
inline std::byte *append_text(std::byte *dst, kimix::string_view text) noexcept {
    if (!text.empty()) {
        std::memcpy(dst, text.data(), text.size());
    }
    return dst + text.size();
}

// Appends a 128-bit value as two little-endian 64-bit words. kimix::Guid and
// kimix::MD5 share that layout (both are two uint64_t words), so one pair of
// helpers covers the guid fields and the content hash in the record.
inline void write_two_u64le(std::byte *dst, uint64_t data0, uint64_t data1) noexcept {
    write_u32le(dst + 0, static_cast<uint32_t>(data0 & 0xffffffffull));
    write_u32le(dst + 4, static_cast<uint32_t>((data0 >> 32) & 0xffffffffull));
    write_u32le(dst + 8, static_cast<uint32_t>(data1 & 0xffffffffull));
    write_u32le(dst + 12, static_cast<uint32_t>((data1 >> 32) & 0xffffffffull));
}

inline kimix::Guid::GuidData read_two_u64le(std::byte const *src) noexcept {
    kimix::Guid::GuidData words{};
    words.data0 = static_cast<uint64_t>(read_u32le(src + 0)) |
                  (static_cast<uint64_t>(read_u32le(src + 4)) << 32);
    words.data1 = static_cast<uint64_t>(read_u32le(src + 8)) |
                  (static_cast<uint64_t>(read_u32le(src + 12)) << 32);
    return words;
}

inline void write_guid_bytes(std::byte *dst, kimix::Guid const &guid) noexcept {
    auto const &words = guid.to_binary();
    write_two_u64le(dst, words.data0, words.data1);
}

inline kimix::Guid read_guid_bytes(std::byte const *src) noexcept {
    return kimix::Guid{read_two_u64le(src)};
}

// Append `count` guids at `dst`; returns the advanced pointer.
inline std::byte *write_guid_table(std::byte *dst,
                                   kimix::vector<kimix::Guid> const &guids) noexcept {
    for (auto const &guid : guids) {
        write_guid_bytes(dst, guid);
        dst += 16;
    }
    return dst;
}

} // namespace src_graph_db_utils
using namespace src_graph_db_utils;

// ---------------------------------------------------------------------------
// Key helpers
// ---------------------------------------------------------------------------

std::array<std::byte, kVertexKeySize>
guid_key(kimix::Guid const &guid) noexcept {
    std::array<std::byte, kVertexKeySize> key{};
    key[0] = static_cast<std::byte>(kVertexKeyPrefix[0]);
    key[1] = static_cast<std::byte>(kVertexKeyPrefix[1]);
    write_guid_bytes(key.data() + 2, guid);
    return key;
}

void guid_key(kimix::Guid const &guid, kimix::vector<std::byte> &out) {
    auto key = guid_key(guid);
    out.assign(key.begin(), key.end());
}

kimix::optional<kimix::Guid>
guid_from_key(kimix::span<const std::byte> key) noexcept {
    if (key.size() != kVertexKeySize) {
        return std::nullopt;
    }
    if (std::to_integer<char>(key[0]) != kVertexKeyPrefix[0] ||
        std::to_integer<char>(key[1]) != kVertexKeyPrefix[1]) {
        return std::nullopt;
    }
    return read_guid_bytes(key.data() + 2);
}

std::array<std::byte, kEdgeKeySize> edge_key(kimix::Guid const &edge_id) noexcept {
    std::array<std::byte, kEdgeKeySize> key{};
    key[0] = static_cast<std::byte>(kEdgeKeyPrefix[0]);
    key[1] = static_cast<std::byte>(kEdgeKeyPrefix[1]);
    write_guid_bytes(key.data() + 2, edge_id);
    return key;
}

void edge_key(kimix::Guid const &edge_id, kimix::vector<std::byte> &out) {
    auto key = edge_key(edge_id);
    out.assign(key.begin(), key.end());
}

kimix::optional<kimix::Guid> edge_from_key(kimix::span<const std::byte> key) noexcept {
    if (key.size() != kEdgeKeySize) {
        return std::nullopt;
    }
    if (std::to_integer<char>(key[0]) != kEdgeKeyPrefix[0] ||
        std::to_integer<char>(key[1]) != kEdgeKeyPrefix[1]) {
        return std::nullopt;
    }
    return read_guid_bytes(key.data() + 2);
}

  kimix::MD5 md5_of(kimix::span<const std::byte> bytes) noexcept {
      return kimix::MD5{kimix::span<uint8_t const>{
          reinterpret_cast<uint8_t const *>(bytes.data()), bytes.size()}};
  }

  kimix::MD5 content_digest_of(kimix::span<const std::byte> bytes, ContentDigest policy) noexcept {
      if (policy == ContentDigest::xxh3) {
          // XXH3-128 into the same 16-byte digest container, little-endian so
          // the digest bytes are host-independent exactly like an MD5 digest.
          // The hash is a change detector (compared against the value recorded
          // at save time), not a signature, so the switch from MD5 loses
          // nothing.
          kimix::Hash128 const digest = kimix::hash128(bytes.data(), bytes.size());
          return kimix::MD5{kimix::MD5::MD5Data{
              detail::read_u64(reinterpret_cast<std::byte const *>(digest.data())),
              detail::read_u64(reinterpret_cast<std::byte const *>(digest.data()) + 8)}};
      }
      return md5_of(bytes);
  }

// ---------------------------------------------------------------------------
// Vertex record codec
// ---------------------------------------------------------------------------

size_t encoded_record_size(VertexRecord const &record) noexcept {
    return kVertexRecordHeaderSize + (record.depending_guids.size() +
                                      record.depended_guids.size()) * 16 +
           record.content.size();
}

void encode_vertex_record(VertexRecord const &record,
                          kimix::vector<std::byte> &out) {
    out.clear();
    out.resize(encoded_record_size(record));

    std::byte *cursor = out.data();
    std::memcpy(cursor, kRecordMagic.data(), kRecordMagicSize);
    write_guid_bytes(cursor + kRecordGuidOffset, record.guid);
    // The cached MD5 travels as its 16 RFC-1321 bytes, i.e. data0 then data1.
    auto const &hash_words = record.content_hash.to_binary();
    write_two_u64le(cursor + kRecordHashOffset, hash_words.data0, hash_words.data1);
    write_u32le(cursor + kRecordDependingCountOffset,
                static_cast<uint32_t>(record.depending_guids.size()));
    write_u32le(cursor + kRecordDependedCountOffset,
                static_cast<uint32_t>(record.depended_guids.size()));

    cursor += kVertexRecordHeaderSize;
    cursor = write_guid_table(cursor, record.depending_guids);
    cursor = write_guid_table(cursor, record.depended_guids);
    if (!record.content.empty()) {
        std::memcpy(cursor, record.content.data(), record.content.size());
    }
}

namespace src_graph_db_utils {

// The v1-only decoder behind both public decoders (a "GDB1" buffer).
[[nodiscard]] bool decode_vertex_record_v1(kimix::span<const std::byte> bytes,
                                           VertexRecord &out) noexcept {
    if (bytes.size() < kVertexRecordHeaderSize) {
        return false;
    }
    if (std::memcmp(bytes.data(), kRecordMagic.data(), kRecordMagicSize) != 0) {
        return false;
    }

    uint32_t const n_depending = read_u32le(bytes.data() + kRecordDependingCountOffset);
    uint32_t const n_depended = read_u32le(bytes.data() + kRecordDependedCountOffset);
    // Guard against a bogus count before it drives any allocation: each entry
    // costs 16 bytes and must fit in what is left after the header.
    size_t const available = bytes.size() - kVertexRecordHeaderSize;
    uint64_t const deps_bytes =
        (static_cast<uint64_t>(n_depending) + static_cast<uint64_t>(n_depended)) * 16ull;
    if (deps_bytes > available) {
        return false;
    }

    out.guid = read_guid_bytes(bytes.data() + kRecordGuidOffset);
    // The 16 hash bytes are the two uint64 words of the MD5 (identical layout
    // to GuidData, see <core/md5.h>), so the same reader recovers both fields.
    auto const hash_words = read_two_u64le(bytes.data() + kRecordHashOffset);
    out.content_hash =
        kimix::MD5{kimix::MD5::MD5Data{hash_words.data0, hash_words.data1}};

    std::byte const *cursor = bytes.data() + kVertexRecordHeaderSize;
    out.depending_guids.clear();
    out.depending_guids.reserve(n_depending);
    for (uint32_t i = 0; i < n_depending; ++i, cursor += 16) {
        out.depending_guids.push_back(read_guid_bytes(cursor));
    }
    out.depended_guids.clear();
    out.depended_guids.reserve(n_depended);
    for (uint32_t i = 0; i < n_depended; ++i, cursor += 16) {
        out.depended_guids.push_back(read_guid_bytes(cursor));
    }

    size_t const content_size =
        bytes.size() - kVertexRecordHeaderSize - static_cast<size_t>(deps_bytes);
    out.content.assign(cursor, cursor + content_size);
    return true;
}

} // namespace src_graph_db_utils
using namespace src_graph_db_utils;

bool decode_vertex_record(kimix::span<const std::byte> bytes,
                          VertexRecord &out) noexcept {
    // BOTH formats decode into a VertexRecord: the v1 layout directly, and a
    // "GDB2" record through its compatibility projection (guid, hash, the two
    // dependency tables, content). A caller that only needs the v1 shape therefore
    // keeps working across a format bump, and a v2 store is still readable by the
    // record-level tools — the GDI fields (key/labels/properties) need
    // decode_vertex_record_v2().
    if (bytes.size() < kRecordMagicSize) {
        return false;
    }
    if (std::memcmp(bytes.data(), kRecordMagicV2.data(), kRecordMagicSize) != 0) {
        return decode_vertex_record_v1(bytes, out);
    }
    VertexRecordV2 record;
    if (!decode_vertex_record_v2(bytes, record)) {
        return false;
    }
    out.guid = record.guid;
    out.content_hash = record.content_hash;
    out.depending_guids = std::move(record.depending_guids);
    out.depended_guids = std::move(record.depended_guids);
    out.content = std::move(record.content);
    return true;
}

// ---------------------------------------------------------------------------
// Meta record ("meta/root")
// ---------------------------------------------------------------------------

namespace src_graph_db_utils {

// magic + format + uuid + 2 timestamps + schema version + 2 id counters + name len
constexpr size_t kMetaHeaderSize = kRecordMagicSize + 4 + 16 + 8 + 8 + 4 + 4 + 4 + 4;

} // namespace src_graph_db_utils
using namespace src_graph_db_utils;

bool format_version_supported(uint32_t format_version) noexcept {
    return format_version >= 1 && format_version <= kFormatVersion;
}

size_t encoded_meta_size(MetaRecord const &record) noexcept {
    return kMetaHeaderSize + record.graph_name.size();
}

void encode_meta_record(MetaRecord const &record, kimix::vector<std::byte> &out) {
    out.clear();
    out.resize(encoded_meta_size(record));
    std::byte *cursor = out.data();
    std::memcpy(cursor, kMetaRecordMagic.data(), kRecordMagicSize);
    cursor += kRecordMagicSize;
    write_u32le(cursor, record.format_version);
    cursor += 4;
    write_guid_bytes(cursor, record.store_uuid);
    cursor += 16;
    write_u64le(cursor, record.last_full_save_ms);
    cursor += 8;
    write_u64le(cursor, record.last_incr_save_ms);
    cursor += 8;
    write_u32le(cursor, record.schema_version);
    cursor += 4;
    write_u32le(cursor, record.next_label_id);
    cursor += 4;
    write_u32le(cursor, record.next_prop_name_id);
    cursor += 4;
    write_u32le(cursor, static_cast<uint32_t>(record.graph_name.size()));
    cursor += 4;
    append_text(cursor, record.graph_name);
}

bool decode_meta_record(kimix::span<const std::byte> bytes, MetaRecord &out) noexcept {
    if (bytes.size() < kMetaHeaderSize) {
        return false;
    }
    if (std::memcmp(bytes.data(), kMetaRecordMagic.data(), kRecordMagicSize) != 0) {
        return false;
    }
    std::byte const *cursor = bytes.data() + kRecordMagicSize;
    out.format_version = read_u32le(cursor);
    cursor += 4;
    out.store_uuid = read_guid_bytes(cursor);
    cursor += 16;
    out.last_full_save_ms = read_u64le(cursor);
    cursor += 8;
    out.last_incr_save_ms = read_u64le(cursor);
    cursor += 8;
    out.schema_version = read_u32le(cursor);
    cursor += 4;
    out.next_label_id = read_u32le(cursor);
    cursor += 4;
    out.next_prop_name_id = read_u32le(cursor);
    cursor += 4;
    uint32_t const name_len = read_u32le(cursor);
    cursor += 4;
    if (static_cast<uint64_t>(name_len) > bytes.size() - kMetaHeaderSize) {
        return false;
    }
    out.graph_name.assign(reinterpret_cast<char const *>(cursor), name_len);
    return true;
}

// ---------------------------------------------------------------------------
// Vertex record v2 ("GDB2")
// ---------------------------------------------------------------------------

namespace src_graph_db_utils {

// magic + guid + md5 + key label + n_labels + n_props + n_dep + n_depended +
// ext len + reserved
constexpr size_t kVertexRecordV2HeaderSize = 4 + 16 + 16 + 4 + 4 + 4 + 4 + 4 + 4 + 4;

[[nodiscard]] size_t v2_payload_size(VertexRecordV2 const &record) noexcept {
    size_t size = record.external_id.size() + record.labels.size() * 4;
    for (auto const &entry : record.properties) {
        size += encoded_property_entry_size(entry);
    }
    size += (record.depending_guids.size() + record.depended_guids.size()) * 16;
    return size;
}

} // namespace src_graph_db_utils
using namespace src_graph_db_utils;

size_t encoded_vertex_record_v2_size(VertexRecordV2 const &record) noexcept {
    return kVertexRecordV2HeaderSize + v2_payload_size(record) + record.content.size();
}

void encode_vertex_record_v2(VertexRecordV2 const &record,
                             kimix::vector<std::byte> &out) {
    out.clear();
    out.resize(encoded_vertex_record_v2_size(record));
    std::byte *cursor = out.data();
    std::memcpy(cursor, kRecordMagicV2.data(), kRecordMagicSize);
    cursor += kRecordMagicSize;
    write_guid_bytes(cursor, record.guid);
    cursor += 16;
    // The cached MD5 travels as its 16 bytes (data0 then data1), like v1.
    auto const &hash_words = record.content_hash.to_binary();
    write_two_u64le(cursor, hash_words.data0, hash_words.data1);
    cursor += 16;
    write_u32le(cursor, record.key_label);
    cursor += 4;
    write_u32le(cursor, static_cast<uint32_t>(record.labels.size()));
    cursor += 4;
    write_u32le(cursor, static_cast<uint32_t>(record.properties.size()));
    cursor += 4;
    write_u32le(cursor, static_cast<uint32_t>(record.depending_guids.size()));
    cursor += 4;
    write_u32le(cursor, static_cast<uint32_t>(record.depended_guids.size()));
    cursor += 4;
    write_u32le(cursor, static_cast<uint32_t>(record.external_id.size()));
    cursor += 4;
    write_u32le(cursor, 0); // reserved
    cursor += 4;
    if (!record.external_id.empty()) {
        std::memcpy(cursor, record.external_id.data(), record.external_id.size());
        cursor += record.external_id.size();
    }
    for (auto label : record.labels) {
        write_u32le(cursor, label);
        cursor += 4;
    }
    for (auto const &entry : record.properties) {
        encode_property_entry(cursor, entry);
        cursor += encoded_property_entry_size(entry);
    }
    cursor = write_guid_table(cursor, record.depending_guids);
    cursor = write_guid_table(cursor, record.depended_guids);
    if (!record.content.empty()) {
        std::memcpy(cursor, record.content.data(), record.content.size());
    }
}

bool decode_vertex_record_v2(kimix::span<const std::byte> bytes,
                             VertexRecordV2 &out) noexcept {
    out = VertexRecordV2{};
    if (bytes.size() < kVertexRecordV2HeaderSize) {
        return false;
    }
    if (std::memcmp(bytes.data(), kRecordMagicV2.data(), kRecordMagicSize) != 0) {
        return false;
    }
    std::byte const *cursor = bytes.data() + kRecordMagicSize;
    std::byte const *end = bytes.data() + bytes.size();
    out.guid = read_guid_bytes(cursor);
    cursor += 16;
    auto const hash_words = read_two_u64le(cursor);
    out.content_hash =
        kimix::MD5{kimix::MD5::MD5Data{hash_words.data0, hash_words.data1}};
    cursor += 16;
    out.key_label = read_u32le(cursor);
    cursor += 4;
    uint32_t const label_count = read_u32le(cursor);
    cursor += 4;
    uint32_t const property_count = read_u32le(cursor);
    cursor += 4;
    uint32_t const dep_count = read_u32le(cursor);
    cursor += 4;
    uint32_t const depended_count = read_u32le(cursor);
    cursor += 4;
    uint32_t const ext_len = read_u32le(cursor);
    cursor += 4;
    cursor += 4; // reserved
    // Every count is checked against what is actually left BEFORE it drives an
    // allocation, so a truncated or hostile record is a `false`.
    if (static_cast<uint64_t>(ext_len) > static_cast<uint64_t>(end - cursor)) {
        return false;
    }
    out.external_id.assign(cursor, cursor + ext_len);
    cursor += ext_len;
    if (static_cast<uint64_t>(label_count) * 4ull > static_cast<uint64_t>(end - cursor)) {
        return false;
    }
    out.labels.reserve(label_count);
    for (uint32_t i = 0; i < label_count; ++i) {
        out.labels.push_back(read_u32le(cursor));
        cursor += 4;
    }
    // An entry is at least kPropertyEntryHeaderSize bytes, so a larger count than
    // what is left cannot be a valid record: reject it BEFORE the reserve() below
    // can drive a huge allocation (a corrupt store must be a `false`, never an
    // abort — the property loop itself is safe, its reserve is not).
    if (static_cast<uint64_t>(property_count) * kPropertyEntryHeaderSize >
        static_cast<uint64_t>(end - cursor)) {
        out = VertexRecordV2{};
        return false;
    }
    out.properties.reserve(property_count);
    for (uint32_t i = 0; i < property_count; ++i) {
        PropertyEntry entry;
        if (!decode_property_entry(cursor, end, entry)) {
            out = VertexRecordV2{};
            return false;
        }
        out.properties.push_back(std::move(entry));
    }
    // Both counts widened to 64 bits FIRST: `dep_count + depended_count` in 32-bit
    // arithmetic wraps (0x80000000 + 0x80000000 == 0), which would wave a hostile
    // pair through this check and into the two reserve() calls below.
    if ((static_cast<uint64_t>(dep_count) + static_cast<uint64_t>(depended_count)) * 16ull >
        static_cast<uint64_t>(end - cursor)) {
        out = VertexRecordV2{};
        return false;
    }
    out.depending_guids.reserve(dep_count);
    for (uint32_t i = 0; i < dep_count; ++i) {
        out.depending_guids.push_back(read_guid_bytes(cursor));
        cursor += 16;
    }
    out.depended_guids.reserve(depended_count);
    for (uint32_t i = 0; i < depended_count; ++i) {
        out.depended_guids.push_back(read_guid_bytes(cursor));
        cursor += 16;
    }
    out.content.assign(cursor, end);
    return true;
}

bool decode_any_vertex_record(kimix::span<const std::byte> bytes,
                              VertexRecordV2 &out, bool &was_v1) noexcept {
    was_v1 = false;
    if (bytes.size() < kRecordMagicSize) {
        return false;
    }
    if (std::memcmp(bytes.data(), kRecordMagicV2.data(), kRecordMagicSize) == 0) {
        return decode_vertex_record_v2(bytes, out);
    }
    if (std::memcmp(bytes.data(), kRecordMagic.data(), kRecordMagicSize) == 0) {
        // A v1 record: decode it with the v1 codec and lift what it carries.
        VertexRecord legacy;
        if (!decode_vertex_record_v1(bytes, legacy)) {
            return false;
        }
        out = VertexRecordV2{};
        out.guid = legacy.guid;
        out.content_hash = legacy.content_hash;
        out.content = std::move(legacy.content);
        out.depending_guids = std::move(legacy.depending_guids);
        out.depended_guids = std::move(legacy.depended_guids);
        was_v1 = true;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Filesystem helpers
// ---------------------------------------------------------------------------

bool ensure_dir(kimix::filesystem::path const &dir) noexcept {
    std::error_code ec;
    if (kimix::filesystem::exists(dir, ec)) {
        return !ec && kimix::filesystem::is_directory(dir, ec);
    }
    if (ec) {
        return false;
    }
    kimix::filesystem::create_directories(dir, ec);
    return !ec;
}

kimix::filesystem::path default_cold_dir() {
    kimix::filesystem::path dir;
    if (!kimix::path_from_utf8(kColdDirName, dir)) {
        return {};
    }
    return dir;
}

// ---------------------------------------------------------------------------
// ColdStorage
// ---------------------------------------------------------------------------

ColdStorage::ColdStorage(kimix::filesystem::path const &dir,
                         size_t map_size) noexcept
    : _db(dir, 126ull, map_size),
      _path(dir) {
}

bool ColdStorage::open(kimix::filesystem::path const &dir,
                       size_t map_size) noexcept {
    if (dir.empty()) {
        return false;
    }
    close();
    _db = kimix::LMDB(dir, 126ull, map_size);
    _path = dir;
    return valid();
}

void ColdStorage::close() noexcept {
    _db = kimix::LMDB{};
    _path.clear();
}

bool ColdStorage::valid() const noexcept {
    return static_cast<bool>(_db);
}

kimix::filesystem::path ColdStorage::path() const noexcept {
    return _path;
}

bool ColdStorage::read_record(kimix::Guid const &guid,
                              VertexRecord &out) const noexcept {
    if (!valid()) {
        return false;
    }
    auto key = guid_key(guid);
    // read_into, not read: the bytes are decoded right here, and a span into the
    // mapping stops being valid the moment its read transaction is gone.
    kimix::vector<std::byte> value;
    if (!_db.read_into(kimix::span<const std::byte>{key.data(), key.size()}, value)) {
        return false;
    }
    return decode_vertex_record(value, out);
}

bool ColdStorage::load_all_records(kimix::vector<ColdRecord> &out) const noexcept {
    if (!valid()) {
        return false;
    }
    out.clear();
    for (auto it = _db.begin(); it != _db.end(); ++it) {
        auto const &value = *it;
        ColdRecord record;
        record.key.assign(value.key.begin(), value.key.end());
        record.value.assign(value.value.begin(), value.value.end());
        out.push_back(std::move(record));
    }
    return true;
}

bool ColdStorage::scan_prefix(kimix::string_view prefix,
                              kimix::vector<ColdRecord> &out) const noexcept {
    if (!valid()) {
        return false;
    }
    out.clear();
    for (auto it = _db.begin(); it != _db.end(); ++it) {
        auto const &value = *it;
        if (value.key.size() < prefix.size()) {
            continue;
        }
        if (prefix.size() != 0 &&
            std::memcmp(value.key.data(), prefix.data(), prefix.size()) != 0) {
            continue;
        }
        ColdRecord record;
        record.key.assign(value.key.begin(), value.key.end());
        record.value.assign(value.value.begin(), value.value.end());
        out.push_back(std::move(record));
    }
    return true;
}

bool ColdStorage::read_meta(MetaRecord &out) const noexcept {
    if (!valid()) {
        return false;
    }
    kimix::vector<std::byte> value;
    if (!_db.read_into(kMetaRootKey, value)) {
        return false;
    }
    return decode_meta_record(value, out);
}

bool ColdStorage::write_meta(MetaRecord const &record) const noexcept {
    if (!valid()) {
        return false;
    }
    kimix::vector<kimix::LMDBWriteCommand> commands;
    commands.resize(1);
    commands[0].key.assign(reinterpret_cast<std::byte const *>(kMetaRootKey.data()),
                           reinterpret_cast<std::byte const *>(kMetaRootKey.data()) +
                               kMetaRootKey.size());
    encode_meta_record(record, commands[0].value);
    _db.write_all(std::move(commands));
    return true;
}

bool ColdStorage::load_vertex_records(kimix::vector<VertexRecord> &out,
                                      size_t *skipped) const noexcept {    if (!valid()) {
        return false;
    }
    out.clear();
    size_t skipped_count = 0;
    for (auto it = _db.begin(); it != _db.end(); ++it) {
        auto const &value = *it;
        // Only "v/" keys are vertex records; "meta/root" and any future key
        // family are simply not vertices.
        if (!guid_from_key(value.key)) {
            continue;
        }
        VertexRecord record;
        if (!decode_vertex_record(value.value, record)) {
            ++skipped_count;
            continue;
        }
        out.push_back(std::move(record));
    }
    if (skipped) {
        *skipped = skipped_count;
    }
    return true;
}

bool ColdStorage::save_batch(kimix::vector<kimix::LMDBWriteCommand> &&commands) const noexcept {
    if (!valid()) {
        return false;
    }
    _db.write_all(std::move(commands));
    return true;
}

bool ColdStorage::save_records(kimix::vector<VertexRecord> &&records) const noexcept {
    if (!valid()) {
        return false;
    }
    kimix::vector<kimix::LMDBWriteCommand> commands;
    commands.reserve(records.size());
    for (auto const &record : records) {
        kimix::LMDBWriteCommand command;
        guid_key(record.guid, command.key);
        encode_vertex_record(record, command.value);
        commands.push_back(std::move(command));
    }
    _db.write_all(std::move(commands));
    return true;
}

bool ColdStorage::remove_keys(kimix::vector<kimix::vector<std::byte>> &&keys) const noexcept {
    if (!valid()) {
        return false;
    }
    _db.remove_all(std::move(keys));
    return true;
}

bool ColdStorage::remove_record(kimix::Guid const &guid) const noexcept {
    if (!valid()) {
        return false;
    }
    auto key = guid_key(guid);
    _db.remove(kimix::span<const std::byte>{key.data(), key.size()});
    return true;
}

bool ColdStorage::copy_to(kimix::filesystem::path const &target) const noexcept {
    if (!valid() || target.empty()) {
        return false;
    }
    _db.copy_to(target);
    return true;
}

} // namespace kimix::graph_db