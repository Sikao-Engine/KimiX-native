/*
 * id_gen.h -- Thread-safe monotonic ID generator (kimix::runtime::concurrency).
 *
 * Plan 008: the hot chunk loop only needs monotonic uniqueness; the Python
 * per-event lock + xxhash (`_gen_part_id`) is replaced by an atomic counter:
 *
 *   - next(): one atomic fetch_add (post-increment) -- no lock, no hash.
 *   - reserve(n): n consecutive ids from a single atomic RMW.
 *
 * Ids are unique across threads by construction (each thread gets a
 * disjoint slice of the counter).
 */

#pragma once

#include <core/kimix_core.h>

#include <atomic>

namespace kimix {
namespace runtime {
namespace concurrency {

class IdGenerator { // header-only kernel: all methods inline, no DLL export needed
public:
    explicit IdGenerator(uint64_t seed = 0) noexcept : _counter(seed) {}

    // Next id (starts at seed, then seed+1, ...). Thread-safe.
    uint64_t next() noexcept { return _counter.fetch_add(1, std::memory_order_relaxed); }

    // Reserve n consecutive ids in one RMW: out[i] = start + i.
    // Thread-safe; concurrent reserves get disjoint contiguous ranges.
    void reserve(uint64_t n, kimix::vector<uint64_t>& out) noexcept {
        const uint64_t start = _counter.fetch_add(n, std::memory_order_relaxed);
        out.clear();
        out.reserve(static_cast<size_t>(n));
        for (uint64_t i = 0; i < n; ++i) {
            out.push_back(start + i);
        }
    }

private:
    std::atomic<uint64_t> _counter;
};

} // namespace concurrency
} // namespace runtime
} // namespace kimix
