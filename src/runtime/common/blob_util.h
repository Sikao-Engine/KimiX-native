/*
 * blob_util.h - shared capacity accounting for the kernel handle stores
 * (kimix::runtime::common).
 *
 * Every native handle exposes `bytes_owned()` so mem_bench can report the
 * native bytes next to the traced Python bytes (report §7: without it a win
 * looks like a traced-memory drop with an unchanged RSS). That number is
 * "capacity of every container the handle owns", which is the same arithmetic
 * in every store, so it lives here once - two file-local copies in sibling
 * namespaces would make the unqualified call ambiguous in a unity-built TU.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace common {

// Bytes reserved by one container (capacity, not size).
template <typename T>
inline uint64_t blob_capacity_bytes(const T &c) noexcept {
    return static_cast<uint64_t>(c.capacity()) * sizeof(typename T::value_type);
}

} // namespace common
} // namespace runtime
} // namespace kimix
