/*
 * le_bytes.h - explicit little-endian byte access for the binary blobs the
 * runtime kernels read and write (kimix::runtime::common).
 *
 * The KMB2 / KMSH1 / KMEL1 / KMSS1 formats are little-endian and 4-byte
 * aligned, and their payloads arrive as Python `bytes`, whose buffer is not
 * guaranteed to be aligned for a cast. These helpers are therefore byte-wise:
 * a reader never dereferences a cast pointer, and a writer never relies on
 * native byte order.
 *
 * ONE copy of each helper lives here on purpose: the runtime_py target builds
 * with unity build (batch_size = 8), so two file-local copies of `put_u32` in
 * two sibling namespaces make every unqualified call ambiguous in the merged
 * TU (a hard error), and two copies in the SAME namespace are a redefinition.
 * A single definition imported with a using-declaration is unambiguous.
 *
 * Header-only, noexcept, no allocation.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace common {

inline void put_le8(char *p, uint8_t v) noexcept {
    p[0] = static_cast<char>(v);
}

inline void put_le16(char *p, uint16_t v) noexcept {
    p[0] = static_cast<char>(v & 0xFFu);
    p[1] = static_cast<char>((v >> 8) & 0xFFu);
}

inline void put_le32(char *p, uint32_t v) noexcept {
    p[0] = static_cast<char>(v & 0xFFu);
    p[1] = static_cast<char>((v >> 8) & 0xFFu);
    p[2] = static_cast<char>((v >> 16) & 0xFFu);
    p[3] = static_cast<char>((v >> 24) & 0xFFu);
}

inline void put_le_i32(char *p, int32_t v) noexcept {
    put_le32(p, static_cast<uint32_t>(v));
}

inline void put_le64(char *p, uint64_t v) noexcept {
    put_le32(p, static_cast<uint32_t>(v & 0xFFFFFFFFu));
    put_le32(p + 4, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu));
}

inline uint8_t read_le8(const char *p) noexcept {
    return static_cast<uint8_t>(p[0]);
}

inline uint16_t read_le16(const char *p) noexcept {
    return static_cast<uint16_t>(read_le8(p) |
                                 (static_cast<uint16_t>(read_le8(p + 1)) << 8));
}

inline uint32_t read_le32(const char *p) noexcept {
    return static_cast<uint32_t>(read_le8(p)) |
           (static_cast<uint32_t>(read_le8(p + 1)) << 8) |
           (static_cast<uint32_t>(read_le8(p + 2)) << 16) |
           (static_cast<uint32_t>(read_le8(p + 3)) << 24);
}

inline int32_t read_le_i32(const char *p) noexcept {
    return static_cast<int32_t>(read_le32(p));
}

inline uint64_t read_le64(const char *p) noexcept {
    return static_cast<uint64_t>(read_le32(p)) |
           (static_cast<uint64_t>(read_le32(p + 4)) << 32);
}

// (start, end) is a valid span of a buffer of `len` bytes.
inline bool le_span_ok(int32_t start, int32_t end, size_t len) noexcept {
    return start >= 0 && end >= start && static_cast<uint64_t>(end) <= len;
}

} // namespace common
} // namespace runtime
} // namespace kimix
