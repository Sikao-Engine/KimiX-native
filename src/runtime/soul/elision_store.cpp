/*
 * elision_store.cpp - content-addressed blob store for pruned message text.
 * See elision_store.h for the design and the retained-bytes contract.
 *
 * Pure C++ kernel: no Python includes, no RTTI, no exceptions. Slabs are 1 MiB
 * kimix::string blocks so growth never reallocates the whole store, and the
 * ref -> entry index is an open-addressing table (power-of-two capacity, 50 %
 * load factor) so a 126-entry store never pays a std::map.
 */

#include <core/kimix_core.h>

#include <runtime/common/le_bytes.h>
#include <runtime/soul/elision_store.h>

#include <cstring>

namespace kimix {
namespace runtime {
namespace soul {

namespace src_runtime_soul_elision_store {

using kimix::runtime::common::put_le32;
using kimix::runtime::common::put_le64;
using kimix::runtime::common::read_le32;
using kimix::runtime::common::read_le64;

constexpr char kMagic[8] = {'K', 'M', 'E', 'L', '1', 0, 0, 0};
constexpr size_t kHeaderBytes = 16;

// Content-addressed ref: xxh3-64 of the bytes. Deterministic across runs and
// across stores, so a persisted ref still resolves after load().
inline uint64_t content_ref(kimix::string_view text) noexcept {
    return kimix::hash64(text.data(), text.size());
}

} // namespace src_runtime_soul_elision_store
using namespace src_runtime_soul_elision_store;

// ── ref table ────────────────────────────────────────────────────────────────

uint32_t ElisionStore::find_slot(uint64_t ref) const noexcept {
    if (_slot.empty()) {
        return 0;
    }
    uint32_t idx = static_cast<uint32_t>(ref) & _slot_mask;
    for (uint32_t probe = 0; probe <= _slot_mask; ++probe) {
        const uint32_t v = _slot[idx];
        if (v == 0) {
            return idx; // empty slot: insertion point (and not found)
        }
        if (_entries[v - 1].ref == ref) {
            return idx;
        }
        idx = (idx + 1) & _slot_mask;
    }
    return 0;
}

void ElisionStore::insert_slot(uint64_t ref, uint32_t entry_index) noexcept {
    if (_slot.empty()) {
        return;
    }
    uint32_t idx = static_cast<uint32_t>(ref) & _slot_mask;
    while (_slot[idx] != 0) {
        idx = (idx + 1) & _slot_mask;
    }
    _slot[idx] = entry_index + 1;
}

void ElisionStore::rehash(uint32_t capacity) noexcept {
    kimix::vector<uint32_t> fresh;
    if (capacity == 0) {
        capacity = 16;
    }
    fresh.assign(capacity, 0);
    _slot = std::move(fresh);
    _slot_mask = capacity - 1;
    for (uint32_t i = 0; i < _entries.size(); ++i) {
        insert_slot(_entries[i].ref, i);
    }
}

const ElisionStore::entry *ElisionStore::find(uint64_t ref) const noexcept {
    if (_entries.empty() || _slot.empty()) {
        return nullptr;
    }
    const uint32_t idx = find_slot(ref);
    if (_slot[idx] == 0) {
        return nullptr;
    }
    return &_entries[_slot[idx] - 1];
}

// ── store ────────────────────────────────────────────────────────────────────

uint64_t ElisionStore::put(kimix::string_view text) noexcept {
    if (text.data() == nullptr) {
        text = kimix::string_view("", 0);
    }
    uint64_t ref = content_ref(text);
    if (const entry *e = find(ref)) {
        if (e->length == text.size()) {
            const kimix::string &slab = _slabs[e->slab];
            if (text.empty() ||
                memcmp(slab.data() + e->offset, text.data(), text.size()) == 0) {
                ++_dedup_hits;
                return e->ref;
            }
        }
        // Hash collision with different content: probe for the next free ref so
        // both payloads stay addressable.
        while (find(ref) != nullptr) {
            ++ref;
        }
    }

    if (text.size() > static_cast<size_t>(UINT32_MAX)) {
        return 0; // a single elision larger than 4 GiB is not a real case
    }
    if (_slabs.empty() || _slabs.back().size() + text.size() > kElisionSlabBytes) {
        kimix::string slab;
        if (text.size() > kElisionSlabBytes) {
            slab.reserve(text.size());
        } else {
            slab.reserve(kElisionSlabBytes);
        }
        _slabs.push_back(std::move(slab));
    }
    entry e;
    e.ref = ref;
    e.slab = static_cast<uint32_t>(_slabs.size() - 1);
    e.offset = static_cast<uint32_t>(_slabs.back().size());
    e.length = static_cast<uint32_t>(text.size());
    if (!text.empty()) {
        _slabs.back().append(text.data(), text.size());
    }

    _entries.push_back(e);
    // Keep the open-addressing table at or below a 50 % load factor.
    if (_slot.empty() || static_cast<uint64_t>(_entries.size()) * 2 > _slot.size()) {
        rehash(static_cast<uint32_t>(_slot.empty() ? 16 : _slot.size() * 2));
    } else {
        insert_slot(ref, static_cast<uint32_t>(_entries.size() - 1));
    }
    ++_count;
    _live_bytes += e.length;
    return ref;
}

bool ElisionStore::get(uint64_t ref, kimix::string_view &out) const noexcept {
    const entry *e = find(ref);
    if (e == nullptr) {
        return false;
    }
    out = kimix::string_view(_slabs[e->slab].data() + e->offset, e->length);
    return true;
}

bool ElisionStore::has(uint64_t ref) const noexcept {
    return find(ref) != nullptr;
}

uint64_t ElisionStore::length(uint64_t ref) const noexcept {
    const entry *e = find(ref);
    return e == nullptr ? 0 : e->length;
}

bool ElisionStore::copy(uint64_t ref, kimix::string &out) const noexcept {
    const entry *e = find(ref);
    if (e == nullptr) {
        return false;
    }
    out.assign(_slabs[e->slab].data() + e->offset, e->length);
    return true;
}

void ElisionStore::clear() noexcept {
    _entries.clear();
    _slabs.clear();
    _slot.clear();
    _slot_mask = 0;
    _count = 0;
    _dedup_hits = 0;
    _live_bytes = 0;
}

uint64_t ElisionStore::bytes_owned() const noexcept {
    uint64_t total = static_cast<uint64_t>(_entries.capacity()) * sizeof(entry);
    total += static_cast<uint64_t>(_slot.capacity()) * sizeof(uint32_t);
    total += static_cast<uint64_t>(_slabs.capacity()) * sizeof(kimix::string);
    for (const kimix::string &slab : _slabs) {
        total += static_cast<uint64_t>(slab.capacity());
    }
    return total;
}

// ── persistence ──────────────────────────────────────────────────────────────

void ElisionStore::save(kimix::string &blob) const noexcept {
    size_t total = kHeaderBytes;
    for (const entry &e : _entries) {
        total += 12 + e.length;
    }
    blob.resize(total);
    char *p = blob.data();
    for (size_t i = 0; i < 8; ++i) {
        p[i] = kMagic[i];
    }
    put_le32(p + 8, static_cast<uint32_t>(_entries.size()));
    put_le32(p + 12, 0);
    p += kHeaderBytes;
    for (const entry &e : _entries) {
        put_le64(p, e.ref);
        put_le32(p + 8, e.length);
        p += 12;
        if (e.length != 0) {
            memcpy(p, _slabs[e.slab].data() + e.offset, e.length);
        }
        p += e.length;
    }
}

bool ElisionStore::load(kimix::string_view blob) noexcept {
    if (blob.size() < kHeaderBytes) {
        return false;
    }
    const char *h = blob.data();
    for (size_t i = 0; i < 8; ++i) {
        if (h[i] != kMagic[i]) {
            return false;
        }
    }
    const uint32_t count = read_le32(h + 8);

    kimix::vector<entry> entries;
    kimix::vector<kimix::string> slabs;
    entries.reserve(count);

    const char *p = h + kHeaderBytes;
    const char *end = h + blob.size();
    for (uint32_t i = 0; i < count; ++i) {
        if (static_cast<size_t>(end - p) < 12) {
            return false;
        }
        entry e;
        e.ref = read_le64(p);
        e.length = read_le32(p + 8);
        p += 12;
        if (static_cast<size_t>(end - p) < e.length) {
            return false;
        }
        if (slabs.empty() || slabs.back().size() + e.length > kElisionSlabBytes) {
            kimix::string slab;
            if (e.length > kElisionSlabBytes) {
                slab.reserve(e.length);
            } else {
                slab.reserve(kElisionSlabBytes);
            }
            slabs.push_back(std::move(slab));
        }
        e.slab = static_cast<uint32_t>(slabs.size() - 1);
        e.offset = static_cast<uint32_t>(slabs.back().size());
        if (e.length != 0) {
            slabs.back().append(p, e.length);
        }
        p += e.length;
        entries.push_back(e);
    }

    if (_entries.empty() && _slabs.empty()) {
        _entries = std::move(entries);
        _slabs = std::move(slabs);
        _slot.clear();
        _slot_mask = 0;
        _count = _entries.size();
        _live_bytes = 0;
        for (const entry &e : _entries) {
            _live_bytes += e.length;
        }
        rehash(16);
    } else {
        // Merge into a non-empty store: keep this store's refs authoritative on
        // a collision (the sequential ref probe makes that deterministic).
        for (const entry &e : entries) {
            kimix::string_view text(slabs[e.slab].data() + e.offset, e.length);
            if (!has(e.ref)) {
                put(text);
            }
        }
    }
    return true;
}

} // namespace soul
} // namespace runtime
} // namespace kimix
