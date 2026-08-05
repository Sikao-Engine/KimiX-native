"""Parity test: native runtime_py.image vs pure-Python kimix_native.image fallback."""

from __future__ import annotations

import os
import struct
import sys

# Make the compiled extension and the Python shim importable.
NATIVE_BIN = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "bin", "debug"))
PYTHON_SHIM_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "python"))

sys.path.insert(0, NATIVE_BIN)
sys.path.insert(0, PYTHON_SHIM_DIR)

# Import the native extension directly.
import runtime_py as native  # noqa: E402

# Import the fallback with native disabled for the IMAGE kernel.
os.environ["KIMIX_NATIVE_IMAGE"] = "0"
import kimix_native.image as fallback  # noqa: E402


def pack_u16_be(v: int) -> bytes:
    return struct.pack(">H", v)


def pack_u32_be(v: int) -> bytes:
    return struct.pack(">I", v)


def pack_u16_le(v: int) -> bytes:
    return struct.pack("<H", v)


def pack_u32_le(v: int) -> bytes:
    return struct.pack("<I", v)


def pack_i32_le(v: int) -> bytes:
    return struct.pack("<i", v)


def make_png(width: int, height: int) -> bytes:
    b = b"\x89PNG\r\n\x1a\n"
    b += pack_u32_be(13)  # IHDR length
    b += b"IHDR"
    b += pack_u32_be(width)
    b += pack_u32_be(height)
    b += b"\x08\x02\x00\x00\x00"  # bit depth, RGB, compression, filter, interlace
    b += pack_u32_be(0)  # CRC (ignored)
    return b


def make_gif(width: int, height: int) -> bytes:
    return (
        b"GIF89a"
        + pack_u16_le(width)
        + pack_u16_le(height)
        + b"\x00\x00\x00"  # packed field, bg index, aspect ratio
    )


def make_bmp(width: int, height: int) -> bytes:
    return (
        b"BM"
        + pack_u32_le(0)  # file size
        + pack_u16_le(0)
        + pack_u16_le(0)  # reserved
        + pack_u32_le(0)  # pixel offset
        + pack_u32_le(40)  # DIB header size
        + pack_i32_le(width)
        + pack_i32_le(height)
    )


def make_webp_vp8(width: int, height: int) -> bytes:
    return (
        b"RIFF"
        + pack_u32_le(0)
        + b"WEBP"
        + b"VP8 "
        + pack_u32_le(0)
        + b"\x00" * 6  # frame tag / start-code prefix
        + pack_u16_le(width & 0x3FFF)
        + pack_u16_le(height & 0x3FFF)
    )


def make_webp_vp8l(width: int, height: int) -> bytes:
    w = width - 1
    h = height - 1
    bits = w | (h << 14)
    payload = (
        b"RIFF"
        + pack_u32_le(0)
        + b"WEBP"
        + b"VP8L"
        + pack_u32_le(0)
        + b"\x2f"
        + pack_u32_le(bits)
    )
    if len(payload) < 30:
        payload += b"\x00" * (30 - len(payload))
    return payload


def make_webp_vp8x(width: int, height: int, flags: int) -> bytes:
    return (
        b"RIFF"
        + pack_u32_le(0)
        + b"WEBP"
        + b"VP8X"
        + pack_u32_le(10)
        + bytes([flags])
        + b"\x00\x00\x00"  # reserved
        + bytes([(width - 1) & 0xFF, ((width - 1) >> 8) & 0xFF, ((width - 1) >> 16) & 0xFF])
        + bytes([(height - 1) & 0xFF, ((height - 1) >> 8) & 0xFF, ((height - 1) >> 16) & 0xFF])
    )


def make_jpeg_sof(sof_marker: int, width: int, height: int) -> bytes:
    return (
        b"\xff\xd8"
        + bytes([0xFF, sof_marker])
        + pack_u16_be(11)
        + b"\x08"
        + pack_u16_be(height)
        + pack_u16_be(width)
        + b"\x03"
        + bytes([1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1])
    )


def make_exif_app1(orientation: int, little_endian: bool) -> bytes:
    payload = b"Exif\x00\x00"
    payload += b"II" if little_endian else b"MM"
    payload += pack_u16(42, little_endian)
    payload += pack_u32(8, little_endian)  # IFD offset
    payload += pack_u16(1, little_endian)  # entry count
    payload += pack_u16(0x0112, little_endian)
    payload += pack_u16(3, little_endian)  # SHORT
    payload += pack_u32(1, little_endian)  # count
    payload += pack_u16(orientation & 0xFFFF, little_endian)
    payload += pack_u16(0, little_endian)  # padding
    payload += pack_u32(0, little_endian)  # next IFD
    return bytes([0xFF, 0xE1]) + pack_u16_be(2 + len(payload)) + payload


def pack_u16(v: int, little_endian: bool) -> bytes:
    return struct.pack("<H" if little_endian else ">H", v)


def pack_u32(v: int, little_endian: bool) -> bytes:
    return struct.pack("<I" if little_endian else ">I", v)


def make_jpeg_with_exif(orientation: int, little_endian: bool) -> bytes:
    header = b"\xff\xd8"
    header += make_exif_app1(orientation, little_endian)
    header += b"\xff\xc0"
    header += pack_u16_be(11)
    header += b"\x08"
    header += pack_u16_be(1080)  # height
    header += pack_u16_be(1920)  # width
    header += b"\x03"
    header += bytes([1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1])
    return header


def main() -> int:
    samples: list[tuple[str, bytes]] = [
        ("png_1920x1080", make_png(1920, 1080)),
        ("gif_640x480", make_gif(640, 480)),
        ("bmp_800x600", make_bmp(800, 600)),
        ("bmp_topdown_800x600", make_bmp(800, -600)),
        ("webp_vp8_1280x720", make_webp_vp8(1280, 720)),
        ("webp_vp8l_512x512", make_webp_vp8l(512, 512)),
        ("webp_vp8x_2048x1536", make_webp_vp8x(2048, 1536, 0)),
        ("webp_vp8x_anim_256x256", make_webp_vp8x(256, 256, 0x02)),
        ("jpeg_sof0_1920x1080", make_jpeg_sof(0xC0, 1920, 1080)),
        ("jpeg_sof2_800x600", make_jpeg_sof(0xC2, 800, 600)),
        ("unknown", b"not an image"),
        ("empty", b""),
    ]

    for orient in range(1, 9):
        for le in (True, False):
            name = f"jpeg_exif_{orient}_{'le' if le else 'be'}"
            samples.append((name, make_jpeg_with_exif(orient, le)))

    failures = 0

    def check(name: str, got, expected) -> None:
        nonlocal failures
        if got != expected:
            print(f"MISMATCH {name}: got={got!r} expected={expected!r}")
            failures += 1

    for name, data in samples:
        check(f"{name}/sniff_dimensions", fallback.sniff_dimensions(data), native.image.sniff_dimensions(data))
        check(f"{name}/read_exif_orientation", fallback.read_exif_orientation(data), native.image.read_exif_orientation(data))
        check(f"{name}/is_animated_webp", fallback.is_animated_webp(data), native.image.is_animated_webp(data))

    byte_sizes = [
        0, 1023, 1024, 1536, 1024 * 1024 - 1, 1024 * 1024,
        1024 * 1024 * 2, 1024 * 1024 * 3 // 2, 1024 * 1024 * 1024,
    ]
    for n in byte_sizes:
        check(f"format_byte_size({n})", fallback.format_byte_size(n), native.image.format_byte_size(n))

    if failures:
        print(f"\n{failures} mismatch(es) found.")
        return 1
    print("All parity checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
