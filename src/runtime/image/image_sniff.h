/*
 * image_sniff.h — Header-only image dimension / orientation / animation sniffing.
 *
 * Native port of `kimi_cli/utils/image_compress.py` helpers:
 *   sniff_image_dimensions, _read_exif_orientation, _is_animated_webp,
 *   format_byte_size.
 *
 * Pure C++ kernel: no Python includes. The binding layer in src/runtime/py/
 * releases the GIL while these functions run.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace image {

/// Pixel dimensions returned by sniff_dimensions().
/// `transposed` is true when a JPEG EXIF orientation of 5-8 swapped width/height
/// into display space.
struct dimensions {
    int width = 0;
    int height = 0;
    bool transposed = false;
};

/// Best-effort pixel-dimension reader for PNG/GIF/BMP/WebP/JPEG.
/// Returns nullopt for unsupported or truncated input.
KIMIX_RUNTIME_API kimix::optional<dimensions> sniff_dimensions(
    kimix::string_view data) noexcept;

/// Read the JPEG EXIF Orientation tag (0x0112) from IFD0.
/// Returns 1-8, or nullopt when no EXIF APP1 segment is found or the tag is
/// missing/truncated.
KIMIX_RUNTIME_API kimix::optional<int> read_exif_orientation(
    kimix::string_view data) noexcept;

/// True when the payload is a RIFF WebP whose VP8X chunk carries the ANIM flag.
KIMIX_RUNTIME_API bool is_animated_webp(kimix::string_view data) noexcept;

/// Human-readable byte size matching `image_compress.py::format_byte_size`.
KIMIX_RUNTIME_API kimix::string format_byte_size(uint64_t n) noexcept;

} // namespace image
} // namespace runtime
} // namespace kimix
