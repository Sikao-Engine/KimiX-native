/*
 * elision_store.h - content-addressed blob store for pruned message text
 * (runtime_py.soul).
 *
 * Report candidate A2. The prune pass keeps a second copy of 50.1 % of the
 * history text: 126 ElidedRecord objects holding 392 040 B of text for a
 * 782 332 B history, with `elided_original_text_same_object_hits = 0` - the
 * sharing fast path never fires, so every record is a genuine copy. The record
 * objects themselves add another 428-443 KB.
 *
 * Here the payload lives in 1 MiB slabs and Python keeps only
 * (index, role, kind, summary, ref, len): `original_text` becomes a lazy
 * property that faults the blob only when the retrieval index actually wants
 * it. `put()` is content-addressed, so a repeated elision returns the existing
 * ref instead of a second copy (0.5x retained instead of 1.0x per duplicate).
 *
 * ref stability: the ref of a given content is deterministic for the life of a
 * store (xxh3-64 of the bytes, linearly probed on a hash collision), so a ref
 * saved in a session file still resolves after load().
 *
 * Example:
 *   kimix::runtime::soul::ElisionStore store;
 *   const uint64_t ref = store.put("... 40 KB of elided tool output ...");
 *   kimix::string_view text;
 *   if (store.get(ref, text)) { ... }
 *
 * Pure C++ kernel: no Python includes, no RTTI, no exceptions, kimix
 * containers, KIMIX_RUNTIME_API exports. Not thread-safe by design (one store
 * per prune pass); the binding layer drives it with the GIL held only for the
 * argument extraction.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace soul {

inline constexpr size_t kElisionSlabBytes = 1u << 20; // 1 MiB

class KIMIX_RUNTIME_API ElisionStore {
public:
    ElisionStore() = default;
    ~ElisionStore() = default;
    ElisionStore(const ElisionStore &) = delete;
    ElisionStore &operator=(const ElisionStore &) = delete;

    // Store `text` and return its stable ref; a byte-identical entry already in
    // the store is reused (dedup_hits() += 1) instead of copied.
    uint64_t put(kimix::string_view text) noexcept;

    // Borrowed view of the stored bytes. False when `ref` is unknown.
    bool get(uint64_t ref, kimix::string_view &out) const noexcept;
    bool has(uint64_t ref) const noexcept;
    // Length of the stored blob (0 when unknown).
    uint64_t length(uint64_t ref) const noexcept;
    // Copy the stored bytes out (used by the binding's py::bytes result).
    bool copy(uint64_t ref, kimix::string &out) const noexcept;

    uint64_t count() const noexcept { return _count; }
    uint64_t dedup_hits() const noexcept { return _dedup_hits; }
    uint64_t bytes_owned() const noexcept;
    uint64_t bytes_live() const noexcept { return _live_bytes; }
    void clear() noexcept;

    // "KMEL1": magic | u32 count | u32 reserved | per entry (u64 ref, u32 len,
    // bytes). Refs are preserved verbatim across load().
    void save(kimix::string &blob) const noexcept;
    bool load(kimix::string_view blob) noexcept;

private:
    struct entry {
        uint64_t ref = 0;
        uint32_t slab = 0;
        uint32_t offset = 0;
        uint32_t length = 0;
    };

    kimix::vector<entry> _entries; // insertion order (a flat, cache-friendly list)
    kimix::vector<kimix::string> _slabs;
    kimix::vector<uint32_t> _slot; // ref -> entry index + 1 (0 = empty), open addressing
    uint32_t _slot_mask = 0;
    uint64_t _count = 0;
    uint64_t _dedup_hits = 0;
    uint64_t _live_bytes = 0;

    void rehash(uint32_t capacity) noexcept;
    uint32_t find_slot(uint64_t ref) const noexcept;
    void insert_slot(uint64_t ref, uint32_t entry_index) noexcept;
    const entry *find(uint64_t ref) const noexcept;
};

} // namespace soul
} // namespace runtime
} // namespace kimix
