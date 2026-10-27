/*
 * file_cold_store.cpp — the flat-file cold-store backend (see
 * file_cold_store.h for the layout, the contract and the limits).
 *
 * STRUCTURE
 *   open()        header validation (or creation); the frame geometry and the
 *                 meta record stay LAZY, because a store that is only ever
 *                 written never needs them.
 *   _ensure_geometry()  walks the 8-byte frame headers only — the cheapest way to
 *                 tell a complete segment from one with a torn tail, and the
 *                 append offset the next save needs.
 *   _scan()       the ONE record pass: windowed 1 MiB positioned reads, framing
 *                 validation, last-wins dedupe and the "meta/root" capture.
 *   _write()      the ONE write path: append (one positioned write, one sync) or
 *                 rebuild (header + frame, one sync).
 *   copy_to()     a sequential chunked copy of the complete frames.
 *
 * All I/O is plain positioned stdio (see FileReader / FileWriter below), one
 * open per pass. Every call is non-throwing and reports through bool; the caches
 * are `mutable` because the seam's operations are `const`, exactly like
 * ColdStorage::save_batch(), which mutates its LMDB environment through a const
 * method.
 */
#include "file_cold_store.h"

#include "binary_le.h"

#include <core/platform.h>
#include <core/stl/unordered_map.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <system_error>

#if defined(KIMIX_PLATFORM_WINDOWS)
#include <io.h> // _commit, _fileno
#else
#include <unistd.h> // fsync, fileno
#endif

namespace kimix::graph_db {

namespace src_graph_db_file_cold_store {

// One positioned read chunk = 1 MiB.
constexpr uint64_t kChunkSize = 1024ull * 1024ull;
// Chunks staged per pass. This window — not the file size — bounds the staging
// memory of a scan or a copy (8 MiB).
constexpr size_t kWindowChunks = 8;
// Framing sanity bounds. No writer produces a key longer than "meta/root" (9) or
// an 18-byte guid key, so the bound only has to catch nonsense; a key_len of 0 or
// one above this ends the frame as corrupt.
constexpr uint32_t kMaxKeyLength = 1024;
// u64 LE frame length in front of every frame payload.
constexpr size_t kFrameHeaderSize = 8;
// u32 LE key length + u64 LE value length around a record's bytes.
constexpr size_t kKeyLengthSize = 4;
constexpr size_t kValueLengthSize = 8;

// ---------------------------------------------------------------------------
// Private positioned file I/O (64-bit offsets, plain stdio)
//
// The cold store speaks the CRT directly. Offsets are 64-bit on purpose —
// _fseeki64 / fseeko — so there is no 2 GiB file-size wall and the chunk offsets
// stay exact. Neither helper throws and neither is exported: this is the store's
// own transport, not a core abstraction.
// ---------------------------------------------------------------------------

int seek_to(FILE *f, uint64_t offset) noexcept {
#if defined(KIMIX_PLATFORM_WINDOWS)
    return _fseeki64(f, static_cast<__int64>(offset), SEEK_SET);
#else
    return ::fseeko(f, static_cast<off_t>(offset), SEEK_SET);
#endif
}

class FileReader {
public:
    FileReader() noexcept = default;
    ~FileReader() { close(); }
    FileReader(FileReader const &) = delete;
    FileReader &operator=(FileReader const &) = delete;

    // fopen "rb"; a path that cannot be opened leaves the reader closed.
    bool open(kimix::string const &path) noexcept {
        close();
        _f = std::fopen(path.c_str(), "rb");
        return _f != nullptr;
    }

    // `size` bytes at `offset`; a short count only happens at EOF.
    size_t read_at(uint64_t offset, void *dst, size_t size) const noexcept {
        if (_f == nullptr || size == 0 || seek_to(_f, offset) != 0) {
            return 0;
        }
        return std::fread(dst, 1, size, _f);
    }

    void close() noexcept {
        if (_f != nullptr) {
            std::fclose(_f);
            _f = nullptr;
        }
    }

private:
    FILE *_f{nullptr};
};

class FileWriter {
public:
    FileWriter() noexcept = default;
    ~FileWriter() { close(); }
    FileWriter(FileWriter const &) = delete;
    FileWriter &operator=(FileWriter const &) = delete;

    // truncate=true creates/empties the file; truncate=false keeps its content so
    // positioned writes overwrite or extend it.
    bool open(kimix::string const &path, bool truncate) noexcept {
        close();
        _f = std::fopen(path.c_str(), truncate ? "wb" : "r+b");
        if (_f == nullptr && !truncate) {
            // "r+b" needs an existing file; a missing store file is created.
            _f = std::fopen(path.c_str(), "wb");
        }
        return _f != nullptr;
    }

    // `size` bytes at `offset`; returns the count that landed (all-or-nothing in
    // every caller below, which compares it against `size`).
    size_t write_at(uint64_t offset, void const *src, size_t size) noexcept {
        if (_f == nullptr || size == 0 || seek_to(_f, offset) != 0) {
            return 0;
        }
        return std::fwrite(src, 1, size, _f);
    }

    // fflush then _commit/fsync: the bytes are on stable storage when this returns.
    bool sync() noexcept {
        if (_f == nullptr || std::fflush(_f) != 0) {
            return false;
        }
#if defined(KIMIX_PLATFORM_WINDOWS)
        return ::_commit(::_fileno(_f)) == 0;
#else
        return ::fsync(::fileno(_f)) == 0;
#endif
    }

    // Deliberately does NOT sync: callers sync explicitly when they need durability
    // (and Windows cannot delete the file while a writer still holds it open).
    void close() noexcept {
        if (_f != nullptr) {
            std::fclose(_f);
            _f = nullptr;
        }
    }

private:
    FILE *_f{nullptr};
};

[[nodiscard]] bool key_is(kimix::string_view expected, std::byte const *key,
                          size_t key_len) noexcept {
    return key_len == expected.size() &&
           std::memcmp(key, expected.data(), expected.size()) == 0;
}

// The stored "meta/root" value, or empty when this batch does not touch the meta.
[[nodiscard]] bool batch_meta_value(kimix::vector<kimix::LMDBWriteCommand> const &commands,
                                    kimix::vector<std::byte> &out) noexcept {
    bool seen = false;
    for (auto const &command : commands) {
        if (key_is(kMetaRootKey, command.key.data(), command.key.size())) {
            // LAST ONE WINS, like the scan: a batch that rewrites the meta twice
            // means the second record is the store's state.
            out = command.value;
            seen = true;
        }
    }
    return seen;
}

} // namespace src_graph_db_file_cold_store
using namespace src_graph_db_file_cold_store;

// ---------------------------------------------------------------------------
// Construction / store lifecycle
// ---------------------------------------------------------------------------

FileColdStore::FileColdStore(kimix::filesystem::path const &dir,
                             size_t map_size) noexcept {
    (void)open(dir, map_size);
}

void FileColdStore::_encode_header(std::byte *out) noexcept {
    std::memcpy(out, kFileMagic.data(), kFileMagic.size());
    detail::write_u32(out + 8, static_cast<uint32_t>(kHeaderSize));
    detail::write_u32(out + 12, kHeaderFlags);
    // A5: the store's format version, mirroring MetaRecord::format_version. The
    // meta/root record stays the authority (this field is informational), and a
    // newer value here must not turn a load's error_bad_file into an open failure.
    detail::write_u32(out + 16, kFormatVersion);
}

bool FileColdStore::_check_or_create_file() noexcept {
    std::error_code ec;
    bool const exists = kimix::filesystem::exists(_file, ec);
    if (ec) {
        return false;
    }
    if (!exists) {
        // Create the empty store: header + sync + close, so a reader that opens it
        // later finds a complete (empty) store instead of a zero-length file.
        FileWriter writer;
        if (!writer.open(_file_name, /*truncate=*/true)) {
            return false;
        }
        std::byte header[kHeaderSize];
        _encode_header(header);
        bool const written = writer.write_at(0, header, kHeaderSize) == kHeaderSize;
        bool const synced = written && writer.sync();
        writer.close();
        return synced;
    }
    uint64_t size = 0;
    {
        std::error_code size_ec;
        uintmax_t const raw = kimix::filesystem::file_size(_file, size_ec);
        if (size_ec) {
            return false;
        }
        size = static_cast<uint64_t>(raw);
    }
    if (size == 0) {
        // A zero-length file (an interrupted creation): stamp the header instead of
        // refusing the store.
        FileWriter writer;
        if (!writer.open(_file_name, /*truncate=*/false)) {
            return false;
        }
        std::byte header[kHeaderSize];
        _encode_header(header);
        bool const written = writer.write_at(0, header, kHeaderSize) == kHeaderSize;
        bool const synced = written && writer.sync();
        writer.close();
        return synced;
    }
    if (size < kHeaderSize) {
        return false; // truncated header: not a store this build can repair
    }
    FileReader reader;
    if (!reader.open(_file_name)) {
        return false;
    }
    std::byte header[kHeaderSize];
    size_t const got = reader.read_at(0, header, kHeaderSize);
    reader.close();
    if (got != kHeaderSize) {
        return false;
    }
    if (!key_is(kFileMagic, header, kFileMagic.size())) {
        return false;
    }
    uint32_t const header_size = detail::read_u32(header + 8);
    uint32_t const flags = detail::read_u32(header + 12);
    // A header that grew additively is readable (the segment starts where the file
    // says); one that shrank below this build's header, or that declares flags this
    // build does not know, is a different layout.
    if (header_size < kHeaderSize || flags != kHeaderFlags) {
        return false;
    }
    _segment_start = header_size;
    return true;
}

bool FileColdStore::open(kimix::filesystem::path const &dir, size_t map_size) noexcept {
    // Accepted and ignored: ColdStorage's LMDB map size — this backend has no map
    // size of its own, the file simply grows with the graph.
    (void)map_size;
    close();
    if (dir.empty()) {
        return false;
    }
    if (!ensure_dir(dir)) {
        return false;
    }
    kimix::filesystem::path const file = dir / kimix::filesystem::path{kColdFileName};
    kimix::string const file_name = kimix::to_string(file);
    if (file_name.empty()) {
        return false;
    }
    _path = dir;
    _file = file;
    _file_name = file_name;
    _segment_start = kHeaderSize;
    if (!_check_or_create_file()) {
        _forget();
        _path.clear();
        _file.clear();
        _file_name.clear();
        return false;
    }
    _is_open = true;
    return true;
}

void FileColdStore::close() noexcept {
    _is_open = false;
    _forget();
    _path.clear();
    _file.clear();
    _file_name.clear();
}

void FileColdStore::_forget() const noexcept {
    _geometry_known = false;
    _file_size = 0;
    _frames_end = 0;
    _meta_known = false;
    _meta_present = false;
    _meta = MetaRecord{};
    _rebuild_next = false;
}

// ---------------------------------------------------------------------------
// Frame geometry
// ---------------------------------------------------------------------------

bool FileColdStore::_ensure_geometry() const noexcept {
    if (_geometry_known) {
        return true;
    }
    if (!_is_open) {
        return false;
    }
    std::error_code ec;
    uintmax_t const raw = kimix::filesystem::file_size(_file, ec);
    if (ec) {
        return false;
    }
    _file_size = static_cast<uint64_t>(raw);
    if (_file_size < _segment_start) {
        return false;
    }
    // Walk the FRAME headers only (8 bytes each): one positioned read per save
    // batch, not per record. A frame whose declared length runs past the end of the
    // file is a torn tail (amendment A2) — it and everything after it are ignored.
    uint64_t pos = _segment_start;
    if (pos < _file_size) {
        FileReader reader;
        if (!reader.open(_file_name)) {
            return false;
        }
        std::byte raw_len[kFrameHeaderSize];
        while (pos + kFrameHeaderSize <= _file_size) {
            if (reader.read_at(pos, raw_len, kFrameHeaderSize) != kFrameHeaderSize) {
                break;
            }
            uint64_t const frame_len = detail::read_u64(raw_len);
            if (frame_len > _file_size - pos - kFrameHeaderSize) {
                break;
            }
            pos += kFrameHeaderSize + frame_len;
        }
        reader.close();
    }
    _frames_end = pos;
    _geometry_known = true;
    return true;
}

// ---------------------------------------------------------------------------
// Read side: the one scan
// ---------------------------------------------------------------------------

bool FileColdStore::_scan(kimix::vector<ColdRecord> *out) const noexcept {
    if (!_ensure_geometry()) {
        return false;
    }
    if (out != nullptr) {
        out->clear();
    }
    bool const collect = out != nullptr;

    // Last-wins dedupe index (A3): key bytes -> the slot it already occupies in
    // `out`. Only built when the caller wants the records; a meta-only pass walks
    // the framing without keeping anything.
    kimix::unordered_map<kimix::string, uint32_t, kimix::string_hash> index;

    // --- parser state -------------------------------------------------------
    // `pending` holds the bytes of the window last read plus whatever tail did not
    // complete a record yet; `cursor` is how far it has been parsed. A record (or
    // even one value) may straddle a window boundary, so the tail is kept and
    // compacted once per window — the parse is a stream, not a per-chunk parse.
    kimix::vector<std::byte> pending;
    size_t cursor = 0;
    uint64_t remaining = 0; // bytes left in the current frame; 0 = at a frame boundary
    bool stopped = false;   // a framing error inside a complete frame ends the pass
    bool saw_meta = false;
    kimix::vector<std::byte> meta_value;

    auto emit = [&](std::byte const *key, size_t key_len, std::byte const *value,
                    size_t value_len) {
        if (key_is(kMetaRootKey, key, key_len)) {
            // The meta is a normal record (A1); the LAST one is the store's state,
            // and an empty value is a tombstone, i.e. "no meta" (A4).
            meta_value.assign(value, value + value_len);
            saw_meta = true;
        }
        if (!collect) {
            return;
        }
        kimix::string key_text{reinterpret_cast<char const *>(key), key_len};
        auto const found = index.find(key_text);
        if (found != index.end()) {
            // LMDB is a map: the newest record of a key replaces the older one, and
            // it keeps the slot's position so a given file always scans the same way.
            (*out)[found->second].value.assign(value, value + value_len);
            return;
        }
        ColdRecord record;
        record.key.assign(key, key + key_len);
        record.value.assign(value, value + value_len);
        index.emplace(std::move(key_text), static_cast<uint32_t>(out->size()));
        out->push_back(std::move(record));
    };

    auto consume = [&](std::byte const *data, size_t size) {
        if (stopped || size == 0) {
            return;
        }
        pending.insert(pending.end(), data, data + size);
        std::byte const *base = pending.data();
        while (!stopped) {
            size_t const avail = pending.size() - cursor;
            if (remaining == 0) {
                if (avail < kFrameHeaderSize) {
                    break; // the frame length itself is not staged yet
                }
                uint64_t const frame_len = detail::read_u64(base + cursor);
                cursor += kFrameHeaderSize;
                if (frame_len > _frames_end) {
                    stopped = true; // not a frame this file can contain
                    break;
                }
                remaining = frame_len;
                continue;
            }
            // Inside a frame payload: u32 key_len | key | u64 value_len | value.
            if (remaining < kKeyLengthSize + kValueLengthSize) {
                stopped = true; // no room for even an empty record
                break;
            }
            if (avail < kKeyLengthSize) {
                break;
            }
            uint32_t const key_len = detail::read_u32(base + cursor);
            if (key_len == 0 || key_len > kMaxKeyLength) {
                stopped = true;
                break;
            }
            size_t const header = kKeyLengthSize + static_cast<size_t>(key_len) + kValueLengthSize;
            if (remaining < header) {
                stopped = true;
                break;
            }
            if (avail < header) {
                break;
            }
            uint64_t const value_len = detail::read_u64(base + cursor + kKeyLengthSize + key_len);
            if (value_len > remaining - header) {
                stopped = true;
                break;
            }
            size_t const total = header + static_cast<size_t>(value_len);
            if (avail < total) {
                break; // the value is not staged yet
            }
            emit(base + cursor + kKeyLengthSize, key_len, base + cursor + header,
                 static_cast<size_t>(value_len));
            cursor += total;
            remaining -= total;
        }
        // Compact the unparsed tail to the front (bounded by one record's size).
        if (cursor != 0) {
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(cursor));
            cursor = 0;
        }
    };

    // --- the reader ---------------------------------------------------------
    uint64_t const begin = _segment_start;
    uint64_t const end = _frames_end;
    if (end > begin) {
        // 1 MiB positioned reads staged in 8 MiB windows, one reader open for the
        // whole pass. Chunks are 1 MiB-aligned, so the first one may start before
        // the segment and carry a few header bytes that are skipped below.
        FileReader reader;
        if (!reader.open(_file_name)) {
            return false;
        }
        kimix::vector<kimix::vector<std::byte>> staging(kWindowChunks);
        for (auto &buffer : staging) {
            buffer.resize(kChunkSize);
        }
        bool read_ok = true;
        uint64_t pos = begin - (begin % kChunkSize);
        while (pos < end && !stopped) {
            uint64_t const span = end - pos;
            size_t const chunks = static_cast<size_t>(
                std::min<uint64_t>(static_cast<uint64_t>(kWindowChunks),
                                   (span + kChunkSize - 1) / kChunkSize));
            for (size_t i = 0; i < chunks; ++i) {
                uint64_t const offset = pos + static_cast<uint64_t>(i) * kChunkSize;
                uint64_t const bytes = std::min<uint64_t>(kChunkSize, end - offset);
                if (reader.read_at(offset, staging[i].data(), static_cast<size_t>(bytes)) !=
                    bytes) {
                    read_ok = false; // the store became unreadable mid-pass
                    break;
                }
            }
            if (!read_ok) {
                break;
            }
            for (size_t i = 0; i < chunks && !stopped; ++i) {
                uint64_t const offset = pos + static_cast<uint64_t>(i) * kChunkSize;
                uint64_t const bytes = std::min<uint64_t>(kChunkSize, end - offset);
                size_t skip = 0;
                if (offset < begin) {
                    skip = static_cast<size_t>(std::min<uint64_t>(begin - offset, bytes));
                }
                consume(staging[i].data() + skip, static_cast<size_t>(bytes) - skip);
            }
            pos += static_cast<uint64_t>(chunks) * kChunkSize;
        }
        reader.close();
        if (!read_ok) {
            return false;
        }
    }

    // Resolve the meta cache only when the pass really reached the end of the
    // segment: a corrupt frame leaves the answer unknown, and read_meta() should
    // fail (the caller sees "no meta" only when that is what the file says).
    if (!stopped) {
        _meta_known = true;
        _meta_present = false;
        if (saw_meta && !meta_value.empty()) {
            MetaRecord decoded;
            if (decode_meta_record(kimix::span<const std::byte>{meta_value.data(),
                                                               meta_value.size()},
                                   decoded)) {
                _meta = decoded;
                _meta_present = true;
            }
        }
    }
    return true;
}

bool FileColdStore::read_meta(MetaRecord &out) const noexcept {
    if (!_is_open) {
        return false;
    }
    if (!_meta_known) {
        // One pass per open resolves the meta for the whole save/load cycle: the
        // serializer alone reads it twice per save, and the loader twice per load.
        if (!_scan(nullptr)) {
            return false;
        }
    }
    if (!_meta_present) {
        return false;
    }
    out = _meta;
    return true;
}

bool FileColdStore::load_all_records(kimix::vector<ColdRecord> &out) const noexcept {
    if (!_is_open) {
        out.clear();
        return false;
    }
    return _scan(&out);
}

// ---------------------------------------------------------------------------
// Write side
// ---------------------------------------------------------------------------

bool FileColdStore::_write(kimix::vector<kimix::LMDBWriteCommand> &&commands,
                           bool rebuild_requested) const noexcept {
    if (!_is_open) {
        return false;
    }
    if (commands.empty()) {
        return true; // nothing to write is not a failure (and never opens the file)
    }
    if (!_ensure_geometry()) {
        return false;
    }
    // --- frame payload ------------------------------------------------------
    size_t payload = 0;
    for (auto const &command : commands) {
        if (command.key.empty() || command.key.size() > kMaxKeyLength) {
            return false; // not a store key: refuse the whole batch, append nothing
        }
        payload += kKeyLengthSize + command.key.size() + kValueLengthSize + command.value.size();
    }
    size_t const frame_bytes = kFrameHeaderSize + payload;
    // The store's meta, as this batch leaves it (A1: the meta is a normal record at
    // the end of the batch, so it rides in the same frame with zero extra I/O).
    kimix::vector<std::byte> batch_meta;
    bool const batch_has_meta = batch_meta_value(commands, batch_meta);

    kimix::vector<std::byte> frame(frame_bytes);
    detail::write_u64(frame.data(), static_cast<uint64_t>(payload));
    std::byte *cursor = frame.data() + kFrameHeaderSize;
    for (auto const &command : commands) {
        detail::write_u32(cursor, static_cast<uint32_t>(command.key.size()));
        cursor += kKeyLengthSize;
        std::memcpy(cursor, command.key.data(), command.key.size());
        cursor += command.key.size();
        detail::write_u64(cursor, static_cast<uint64_t>(command.value.size()));
        cursor += kValueLengthSize;
        if (!command.value.empty()) {
            std::memcpy(cursor, command.value.data(), command.value.size());
            cursor += command.value.size();
        }
    }

    if (rebuild_requested) {
        // FULL save: header + one frame, truncating. Two writes, ONE sync, then the
        // handle is closed before returning (Windows cannot delete an open file).
        FileWriter writer;
        if (!writer.open(_file_name, /*truncate=*/true)) {
            return false;
        }
        std::byte header[kHeaderSize];
        _encode_header(header);
        bool ok = writer.write_at(0, header, kHeaderSize) == kHeaderSize &&
                  writer.write_at(kHeaderSize, frame.data(), frame.size()) == frame.size();
        if (ok) {
            ok = writer.sync();
        }
        writer.close();
        if (!ok) {
            _forget(); // the file's length is unknown now
            return false;
        }
        _file_size = kHeaderSize + frame_bytes;
        _frames_end = _file_size;
        _geometry_known = true;
    } else {
        // INCREMENTAL save: repair a torn tail, then append one frame.
        if (_file_size > _frames_end) {
            // A crash left an incomplete frame behind. Truncate it away first: the
            // next frame must start exactly at the end of the last complete one, or
            // the leftover bytes would be read as that frame's payload.
            std::error_code ec;
            kimix::filesystem::resize_file(_file, static_cast<uintmax_t>(_frames_end), ec);
            if (ec) {
                return false;
            }
            _file_size = _frames_end;
        }
        // ONE open, one positioned write at the end of the segment, ONE sync, close.
        // A short write or a failed sync is a failed save (never a success).
        FileWriter writer;
        if (!writer.open(_file_name, /*truncate=*/false)) {
            _forget();
            return false;
        }
        bool ok = writer.write_at(_frames_end, frame.data(), frame.size()) == frame.size();
        if (ok) {
            ok = writer.sync();
        }
        writer.close();
        if (!ok) {
            _forget();
            return false;
        }
        _frames_end += frame_bytes;
        _file_size = _frames_end;
    }

    // --- meta cache ---------------------------------------------------------
    if (batch_has_meta) {
        _meta_known = true;
        _meta_present = false;
        if (!batch_meta.empty()) {
            MetaRecord decoded;
            if (decode_meta_record(kimix::span<const std::byte>{batch_meta.data(),
                                                               batch_meta.size()},
                                   decoded)) {
                _meta = decoded;
                _meta_present = true;
            }
        }
    } else if (rebuild_requested) {
        // A rebuilt file holds exactly `commands`: no meta record was written, so
        // the store has none (the serializer always writes one, so this only
        // happens for a hand-built batch).
        _meta_known = true;
        _meta_present = false;
    }
    return true;
}

bool FileColdStore::save_batch(kimix::vector<kimix::LMDBWriteCommand> &&commands) const noexcept {
    // The seam's batch carries no "full save" flag (serialize.cpp writes both kinds
    // through this one call), so Database::persist(full=true) arms the rebuild
    // explicitly. The flag is consumed here whether or not the write succeeds.
    bool const rebuild_requested = _rebuild_next;
    _rebuild_next = false;
    if (rebuild_requested) {
        return _write(std::move(commands), true);
    }
    return _write(std::move(commands), false);
}

bool FileColdStore::rebuild(kimix::vector<kimix::LMDBWriteCommand> &&commands) const noexcept {
    return _write(std::move(commands), true);
}

// ---------------------------------------------------------------------------
// Checkpoint
// ---------------------------------------------------------------------------

bool FileColdStore::copy_to(kimix::filesystem::path const &target) const noexcept {
    if (!_is_open || target.empty()) {
        return false;
    }
    if (!_ensure_geometry()) {
        return false;
    }
    if (!ensure_dir(target)) {
        return false;
    }
    kimix::filesystem::path const destination = target / kimix::filesystem::path{kColdFileName};
    kimix::string const destination_name = kimix::to_string(destination);
    if (destination_name.empty()) {
        return false;
    }
    // Truncating writer: a checkpoint REPLACES a previous one in that directory.
    FileWriter writer;
    if (!writer.open(destination_name, /*truncate=*/true)) {
        return false;
    }
    bool ok = true;
    if (_frames_end > 0) {
        // Sequential 1 MiB-chunk copy of the complete frames: ONE reader open on the
        // source, one writer open on the target, 8 MiB of staging.
        FileReader reader;
        if (!reader.open(_file_name)) {
            writer.close();
            return false;
        }
        kimix::vector<kimix::vector<std::byte>> staging(kWindowChunks);
        for (auto &buffer : staging) {
            buffer.resize(kChunkSize);
        }
        uint64_t const end = _frames_end;
        for (uint64_t pos = 0; pos < end && ok;) {
            uint64_t const span = end - pos;
            size_t const chunks = static_cast<size_t>(
                std::min<uint64_t>(static_cast<uint64_t>(kWindowChunks),
                                   (span + kChunkSize - 1) / kChunkSize));
            for (size_t i = 0; i < chunks && ok; ++i) {
                uint64_t const offset = pos + static_cast<uint64_t>(i) * kChunkSize;
                size_t const bytes =
                    static_cast<size_t>(std::min<uint64_t>(kChunkSize, end - offset));
                ok = reader.read_at(offset, staging[i].data(), bytes) == bytes;
            }
            for (size_t i = 0; i < chunks && ok; ++i) {
                uint64_t const offset = pos + static_cast<uint64_t>(i) * kChunkSize;
                size_t const bytes =
                    static_cast<size_t>(std::min<uint64_t>(kChunkSize, end - offset));
                ok = writer.write_at(offset, staging[i].data(), bytes) == bytes;
            }
            pos += static_cast<uint64_t>(chunks) * kChunkSize;
        }
        reader.close();
    }
    if (ok) {
        ok = writer.sync();
    }
    // CLOSE before returning: on Windows an open handle keeps the target file — and
    // its directory — undeletable.
    writer.close();
    return ok;
}

} // namespace kimix::graph_db