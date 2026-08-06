"""Parity tests for kimix_native.image (image header sniffing).

Compares the native kernels against the pure-Python ``_compat`` mirrors on:
- sniff_dimensions: PNG / GIF / BMP / WebP (VP8, VP8L, VP8X) / JPEG (with and
  without EXIF orientation), truncated and malformed headers
- read_exif_orientation: II/MM byte order, orientation values 1..8, missing /
  out-of-range values
- is_animated_webp: VP8X animation flag
- format_byte_size: B / KB / MB formatting
- the KIMIX_NATIVE_IMAGE=0 fallback toggle
"""
import os
import random
import struct
import subprocess
import sys

from kimix_native import image


def _png(w: int, h: int) -> bytes:
    return b"\x89PNG\r\n\x1a\n" + b"\x00" * 8 + struct.pack(">II", w, h)


def _gif(w: int, h: int) -> bytes:
    return b"GIF89a" + struct.pack("<HH", w, h)


def _bmp(w: int, h: int) -> bytes:
    return b"BM" + b"\x00" * 16 + struct.pack("<ii", w, h) + b"\x00\x00\x00\x00"


def _webp_lossy(w: int, h: int) -> bytes:
    return b"RIFF" + b"\x00" * 4 + b"WEBP" + b"VP8 " + b"\x00" * 10 + struct.pack(
        "<HH", w, h
    )


def _webp_lossless(w: int, h: int) -> bytes:
    bits = ((w - 1) & 0x3FFF) | (((h - 1) & 0x3FFF) << 14)
    return b"RIFF" + b"\x00" * 4 + b"WEBP" + b"VP8L" + b"\x00" * 5 + struct.pack(
        "<I", bits
    ) + b"\x00" * 5


def _webp_extended(w: int, h: int) -> bytes:
    return (
        b"RIFF" + b"\x00" * 4 + b"WEBP" + b"VP8X" + b"\x00" * 8
        + (w - 1).to_bytes(3, "little") + (h - 1).to_bytes(3, "little")
    )


def _jpeg(sos_segments: bytes = b"", width: int = 320, height: int = 240) -> bytes:
    # SOF0 marker + a couple of segments; EXIF optional.
    sof = b"\xff\xc0\x00\x11\x08" + struct.pack(">HH", height, width) + b"\x03\x01\x22\x00\x02\x11\x01\x03\x11\x01"
    return b"\xff\xd8" + sos_segments + sof + b"\xff\xd9"


def _exif_jpeg(orientation: int, byte_order: bytes = b"II") -> bytes:
    order = "<" if byte_order == b"II" else ">"
    # IFD0 with one SHORT entry; the 4-byte value field holds the orientation
    # in its first two bytes (TIFF SHORT semantics), padded for 4-byte align.
    ifd_data = struct.pack(order + "H", 1) + struct.pack(order + "HHI", 0x0112, 3, 1) \
        + struct.pack(order + "H", orientation) + b"\x00\x00"
    tiff = byte_order + struct.pack(order + "HI", 42, 8) + ifd_data + b"\x00" * 2
    app1 = b"\xff\xe1" + struct.pack(">H", len(tiff) + 6) + b"Exif\x00\x00" + tiff
    return b"\xff\xd8" + app1 + _jpeg()[2:]


SNIFF_CASES = [
    (_png(100, 50), (100, 50, False)),
    (_gif(16, 9), (16, 9, False)),
    (_bmp(33, -22), (33, 22, False)),
    (_webp_lossy(80, 60), (80, 60, False)),
    (_webp_lossless(90, 70), (90, 70, False)),
    (_webp_extended(120, 100), (120, 100, False)),
    (_jpeg(width=320, height=240), (320, 240, False)),
    (b"", None),
    (b"garbage", None),
    (b"\x89PNG\r\n\x1a\n" + b"\x00" * 8 + struct.pack(">I", 100), None),  # truncated
]


def test_sniff_dimensions_parity():
    for data, expected in SNIFF_CASES:
        native = image.sniff_dimensions(data)
        compat = image._compat_sniff_dimensions(data)
        assert native == compat == expected, data[:20]


def test_sniff_dimensions_random_truncation():
    rng = random.Random(5)
    base = [_png(100, 50), _gif(16, 9), _bmp(33, 22), _webp_lossy(8, 8),
            _webp_lossless(9, 7), _webp_extended(12, 10), _jpeg()]
    for blob in base:
        for cut in rng.sample(range(len(blob)), min(4, len(blob))):
            data = blob[:cut]
            n = image.sniff_dimensions(data)
            c = image._compat_sniff_dimensions(data)
            assert n == c, (cut, n, c)


def test_exif_orientation_parity():
    for orientation in (1, 2, 3, 4, 5, 6, 7, 8):
        for order in (b"II", b"MM"):
            data = _exif_jpeg(orientation, order)
            native = image.read_exif_orientation(data)
            compat = image._compat_read_exif_orientation_from_jpeg(data)
            assert native == compat == orientation, (orientation, order)


def test_exif_orientation_invalid():
    cases = [
        b"",  # not jpeg
        b"\xff\xd8\xff\xd9",  # no segments
        _jpeg(),  # no exif
        _exif_jpeg(0),  # out of range
        _exif_jpeg(9),  # out of range
        b"\xff\xd8" + b"\xff\xe1\x00\x05Exif\x00\x00" + b"\xff\xd9",  # malformed
    ]
    for data in cases:
        native = image.read_exif_orientation(data)
        compat = image._compat_read_exif_orientation_from_jpeg(data)
        assert native == compat, data[:30]


def test_is_animated_webp_parity():
    anim = b"RIFF" + b"\x00" * 4 + b"WEBP" + b"VP8X" + b"\x00" * 4 + b"\x02\x00\x00\x00" + b"\x00"
    still = b"RIFF" + b"\x00" * 4 + b"WEBP" + b"VP8X" + b"\x00" * 4 + b"\x00\x00\x00\x00" + b"\x00"
    for data, expected in [
        (anim, True),
        (still, False),
        (b"", False),
        (b"RIFF", False),
        (_png(1, 1), False),
    ]:
        assert image.is_animated_webp(data) == expected
        assert image._compat_is_animated_webp(data) == expected


def test_format_byte_size_parity():
    for n in (0, 1, 1023, 1024, 1536, 2048, 5 * 1024, 10 * 1024 * 1024, 1048576):
        native = image.format_byte_size(n)
        compat = image._compat_format_byte_size(n)
        assert native == compat, n
    assert image.format_byte_size(0) == "0 B"
    assert image.format_byte_size(2048) == "2 KB"
    assert image.format_byte_size(1048576) == "1.0 MB"


def test_native_disabled_fallback():
    """With KIMIX_NATIVE_IMAGE=0 the shim must behave identically."""
    env = dict(os.environ, KIMIX_NATIVE_IMAGE="0")
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    code = (
        "import sys; sys.path.insert(0, %r); sys.path.insert(0, %r);\n"
        "from kimix_native import image\n"
        "assert image.use_native('IMAGE') is False\n"
        "png = b'\\x89PNG\\r\\n\\x1a\\n' + b'\\x00'*8 + (100).to_bytes(4,'big') + (50).to_bytes(4,'big')\n"
        "assert image.sniff_dimensions(png) == (100, 50, False)\n"
        "assert image.format_byte_size(2048) == '2 KB'\n"
        "assert image.is_animated_webp(b'') is False\n"
        "print('FALLBACK_OK')\n"
    ) % (os.path.join(root, "python"), os.path.join(root, "bin", "release"))
    r = subprocess.run([sys.executable, "-c", code], capture_output=True,
                       text=True, env=env)
    assert r.returncode == 0, r.stderr
    assert "FALLBACK_OK" in r.stdout
