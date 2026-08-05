/*
 * image_sniff.cpp — Native image header sniffing kernels.
 *
 * Mirrors `kimi_cli/utils/image_compress.py` exactly for all supported formats.
 * All functions are noexcept and return "empty" results for truncated or
 * malformed input.
 */

#include <runtime/image/image_sniff.h>

#include <cmath>
#include <cstring>

namespace kimix {
namespace runtime {
namespace image {
namespace {

// ---------------------------------------------------------------------------
// Little / big endian helpers (no unaligned-read assumptions).
// ---------------------------------------------------------------------------

inline uint16_t read_u16_le(const char* p) noexcept {
    return static_cast<uint8_t>(p[0]) |
           (static_cast<uint8_t>(p[1]) << 8);
}

inline uint32_t read_u32_le(const char* p) noexcept {
    return static_cast<uint8_t>(p[0]) |
           (static_cast<uint8_t>(p[1]) << 8) |
           (static_cast<uint8_t>(p[2]) << 16) |
           (static_cast<uint8_t>(p[3]) << 24);
}

inline uint16_t read_u16_be(const char* p) noexcept {
    return (static_cast<uint8_t>(p[0]) << 8) |
           static_cast<uint8_t>(p[1]);
}

inline uint32_t read_u32_be(const char* p) noexcept {
    return (static_cast<uint32_t>(static_cast<uint8_t>(p[0])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(p[3]));
}

inline int32_t read_i32_le(const char* p) noexcept {
    return static_cast<int32_t>(read_u32_le(p));
}

// ---------------------------------------------------------------------------
// EXIF/TIFF helper: parse IFD0 orientation tag (0x0112).
// `start`/`end` bound the APP1 payload (after the FF E1 length field).
// ---------------------------------------------------------------------------

kimix::optional<int> parse_exif_orientation(kimix::string_view data,
                                            size_t start,
                                            size_t end) noexcept {
    const size_t bounded_end = (end < data.size()) ? end : data.size();
    if (start + 6 > bounded_end ||
        std::memcmp(data.data() + start, "Exif\0\0", 6) != 0) {
        return std::nullopt;
    }

    const size_t tiff = start + 6;
    if (tiff + 8 > bounded_end) {
        return std::nullopt;
    }

    const char* p = data.data();
    bool little_endian = false;
    if (std::memcmp(p + tiff, "II", 2) == 0) {
        little_endian = true;
    } else if (std::memcmp(p + tiff, "MM", 2) != 0) {
        return std::nullopt;
    }

    const auto u16 = [&](size_t offset) -> uint16_t {
        return little_endian ? read_u16_le(p + offset) : read_u16_be(p + offset);
    };
    const auto u32 = [&](size_t offset) -> uint32_t {
        return little_endian ? read_u32_le(p + offset) : read_u32_be(p + offset);
    };

    if (u16(tiff + 2) != 42) {
        return std::nullopt;
    }

    const size_t ifd = tiff + u32(tiff + 4);
    if (ifd + 2 > bounded_end) {
        return std::nullopt;
    }

    const uint16_t entry_count = u16(ifd);
    for (uint16_t i = 0; i < entry_count; ++i) {
        const size_t entry = ifd + 2 + static_cast<size_t>(i) * 12;
        if (entry + 12 > bounded_end) {
            return std::nullopt;
        }
        if (u16(entry) == 0x0112) {
            const uint16_t value = u16(entry + 8);
            if (value >= 1 && value <= 8) {
                return static_cast<int>(value);
            }
            return std::nullopt;
        }
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// JPEG helpers.
// ---------------------------------------------------------------------------

kimix::optional<int> scan_jpeg_for_exif(kimix::string_view data) noexcept {
    if (data.size() < 2 || data[0] != '\xff' || data[1] != '\xd8') {
        return std::nullopt;
    }

    size_t offset = 2;
    while (offset + 4 < data.size()) {
        if (data[offset] != '\xff') {
            ++offset;
            continue;
        }

        const uint8_t marker = static_cast<uint8_t>(data[offset + 1]);

        // Standalone markers (RSTn, SOI, EOI) carry no length field.
        if (marker == 0xD8 || marker == 0xD9 ||
            (marker >= 0xD0 && marker <= 0xD7)) {
            offset += 2;
            continue;
        }

        const uint16_t segment_length = read_u16_be(data.data() + offset + 2);
        if (segment_length < 2) {
            break;
        }

        if (marker == 0xE1) {
            const size_t payload_start = offset + 4;
            const size_t payload_end = offset + 2 + segment_length;
            if (auto orient = parse_exif_orientation(data, payload_start, payload_end)) {
                return orient;
            }
        }

        offset += 2 + segment_length;
    }

    return std::nullopt;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

kimix::optional<dimensions> sniff_dimensions(kimix::string_view data) noexcept {
    // PNG — IHDR width/height are big-endian uint32 at offsets 16 and 20.
    if (data.size() >= 24 &&
        std::memcmp(data.data(), "\x89PNG\r\n\x1a\n", 8) == 0) {
        return dimensions{
            static_cast<int>(read_u32_be(data.data() + 16)),
            static_cast<int>(read_u32_be(data.data() + 20)),
            false,
        };
    }

    // GIF — logical-screen width/height are little-endian uint16 at offsets 6 and 8.
    if (data.size() >= 10 &&
        (std::memcmp(data.data(), "GIF87a", 6) == 0 ||
         std::memcmp(data.data(), "GIF89a", 6) == 0)) {
        return dimensions{
            static_cast<int>(read_u16_le(data.data() + 6)),
            static_cast<int>(read_u16_le(data.data() + 8)),
            false,
        };
    }

    // BMP — DIB header width/height are signed little-endian int32 at offsets 18 and 22.
    if (data.size() >= 26 && data[0] == 'B' && data[1] == 'M') {
        const int32_t width = read_i32_le(data.data() + 18);
        const int32_t height = read_i32_le(data.data() + 22);
        return dimensions{
            static_cast<int>(width),
            static_cast<int>(std::abs(height)),
            false,
        };
    }

    // WebP — RIFF container with VP8 / VP8L / VP8X chunks.
    if (data.size() >= 30 && std::memcmp(data.data(), "RIFF", 4) == 0) {
        const kimix::string_view four_cc(data.data() + 12, 4);

        if (four_cc == "VP8 ") {
            return dimensions{
                static_cast<int>(read_u16_le(data.data() + 26) & 0x3FFF),
                static_cast<int>(read_u16_le(data.data() + 28) & 0x3FFF),
                false,
            };
        }

        if (four_cc == "VP8L" && data.size() >= 25) {
            const uint32_t bits = read_u32_le(data.data() + 21);
            return dimensions{
                static_cast<int>((bits & 0x3FFF) + 1),
                static_cast<int>(((bits >> 14) & 0x3FFF) + 1),
                false,
            };
        }

        if (four_cc == "VP8X") {
            const uint32_t width = 1 + static_cast<uint32_t>(
                static_cast<uint8_t>(data[24]) |
                (static_cast<uint8_t>(data[25]) << 8) |
                (static_cast<uint8_t>(data[26]) << 16));
            const uint32_t height = 1 + static_cast<uint32_t>(
                static_cast<uint8_t>(data[27]) |
                (static_cast<uint8_t>(data[28]) << 8) |
                (static_cast<uint8_t>(data[29]) << 16));
            return dimensions{
                static_cast<int>(width),
                static_cast<int>(height),
                false,
            };
        }
    }

    // JPEG — scan SOFn markers for dimensions; APP1 supplies EXIF orientation.
    if (data.size() >= 2 && data[0] == '\xff' && data[1] == '\xd8') {
        kimix::optional<int> orientation = std::nullopt;
        size_t offset = 2;

        while (offset + 9 < data.size()) {
            if (data[offset] != '\xff') {
                ++offset;
                continue;
            }

            const uint8_t marker = static_cast<uint8_t>(data[offset + 1]);

            // SOFn markers carry frame dimensions; skip SOF4/SOF8/SOF12.
            if (marker >= 0xC0 && marker <= 0xCF &&
                marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                const int height = static_cast<int>(
                    read_u16_be(data.data() + offset + 5));
                const int width = static_cast<int>(
                    read_u16_be(data.data() + offset + 7));
                if (orientation && *orientation >= 5) {
                    return dimensions{height, width, true};
                }
                return dimensions{width, height, false};
            }

            // Standalone markers carry no length field.
            if (marker == 0xD8 || marker == 0xD9 ||
                (marker >= 0xD0 && marker <= 0xD7)) {
                offset += 2;
                continue;
            }

            const uint16_t segment_length = read_u16_be(data.data() + offset + 2);
            if (segment_length < 2) {
                break;
            }

            if (marker == 0xE1 && !orientation) {
                orientation = parse_exif_orientation(
                    data, offset + 4, offset + 2 + segment_length);
            }

            offset += 2 + segment_length;
        }
    }

    return std::nullopt;
}

kimix::optional<int> read_exif_orientation(kimix::string_view data) noexcept {
    return scan_jpeg_for_exif(data);
}

bool is_animated_webp(kimix::string_view data) noexcept {
    return data.size() >= 21 &&
           std::memcmp(data.data(), "RIFF", 4) == 0 &&
           std::memcmp(data.data() + 8, "WEBP", 4) == 0 &&
           std::memcmp(data.data() + 12, "VP8X", 4) == 0 &&
           (static_cast<uint8_t>(data[20]) & 0x02) != 0;
}

kimix::string format_byte_size(uint64_t n) noexcept {
    constexpr uint64_t kib = 1024;
    constexpr uint64_t mib = 1024 * 1024;

    if (n < kib) {
        return kimix::format("{} B", n);
    }
    if (n < mib) {
        // JS Math.round semantics (round half up), matching the TS original.
        const uint64_t kb = static_cast<uint64_t>(
            std::floor(static_cast<double>(n) / 1024.0 + 0.5));
        return kimix::format("{} KB", kb);
    }
    return kimix::format("{:.1f} MB", static_cast<double>(n) / static_cast<double>(mib));
}

} // namespace image
} // namespace runtime
} // namespace kimix
