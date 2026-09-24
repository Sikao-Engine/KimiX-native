"""Differential parity tests for the read_image builtin tool (kimix-base <-> kimi-agent).

``src/builtin_tools/read_image_tool.{h,cpp}`` is the C++ port of kimi-agent's
read_image / ReadMediaFile tool.  This module compares every kernel the
``runtime_py`` extension exposes for it through
``runtime_py.builtin_tools.file`` against the *original* Python implementation
in the kimi-agent checkout (``C:/dev/kimi-agent``, override with
``KIMI_AGENT_ROOT``):

* ``sniff_image_dimensions`` (FOCUS 1) -- ``kimi_cli.utils.image_compress``
  ``sniff_image_dimensions`` / ``_read_exif_orientation`` (pure Python, no
  native gate).  Exercised with real codec output (PIL) for PNG + APNG, JPEG
  (baseline/progressive/restart markers/COM/EXIF orientations 1-8), GIF
  (87a/89a + comment extension), WebP (VP8 / VP8L / VP8X / animated), BMP,
  TIFF, ICO, PPM, plus synthetic and malformed headers, *every* truncation of
  each fixture, and a seeded fuzz corpus.
* ``detect_file_type`` (FOCUS 2) -- ``kimi_cli.tools.file.utils``
  ``detect_file_type`` / ``sniff_media_from_magic`` / ``_sniff_ftyp_brand``:
  the magic-byte table, the explicit suffix maps, the NUL/binary gate, the
  kind-conflict rule, and the ``mimetypes.guess_type`` fallback.
* ``is_model_accepted_image_mime`` -- ``kimi_cli.utils.image_format_policy``.

Two reference-provenance traps are handled explicitly:

1. Import order.  ``kimi_cli.native_loader`` puts ``<kimi-agent>/bin`` -- which
   holds a *staged, older* ``runtime_py.pyd`` -- first on ``sys.path``.
   ``runtime_py`` is therefore imported from this checkout's ``bin/<mode>``
   *before* anything kimi-agent, and its ``__file__`` is asserted.
2. Windows registry.  ``detect_file_type`` falls through to the stdlib
   ``mimetypes`` module, whose *module-level* tables CPython merges the
   ``HKEY_CLASSES_ROOT`` Content-Type values into on Windows.  That is not
   reproducible in a portable C++ port (and differs between machines), so the
   fallback table is compared against a *registry-free* ``mimetypes.MimeTypes()``
   instance (see ``portable_reference``) and a second test pins exactly which
   suffixes the live reference gets from the registry.
"""

from __future__ import annotations

import io
import os
import random
import sys
from contextlib import contextmanager
from pathlib import Path
from unittest import mock

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

_KIMIX_BASE_ROOT = Path(__file__).resolve().parents[2]


def _kimix_base_bin_dir():
    """The kimix-base build directory holding ``runtime_py`` (conftest's order)."""
    for mode in ("release", "releasedbg", "debug", "check"):
        cand = _KIMIX_BASE_ROOT / "bin" / mode
        if cand.is_dir() and any(cand.glob("runtime_py.*")):
            return cand
    for cand in sorted((_KIMIX_BASE_ROOT / "bin").glob("*")):
        if cand.is_dir() and any(cand.glob("runtime_py.*")):
            return cand
    return None


# IMPORT ORDER IS LOAD-BEARING: see the module docstring.
_BIN_DIR = _kimix_base_bin_dir()
if _BIN_DIR is not None:
    sys.path.insert(0, str(_BIN_DIR))
import runtime_py  # noqa: E402

if _BIN_DIR is None:  # pragma: no cover - defensive
    pytest.skip("no kimix-base runtime_py build found under bin/",
                allow_module_level=True)

_NATIVE_PATH = Path(runtime_py.__file__).resolve()
assert _BIN_DIR.resolve() in _NATIVE_PATH.parents, (
    f"runtime_py came from {_NATIVE_PATH}, not the kimix-base build "
    f"{_BIN_DIR} -- a staged copy shadowed it; parity results would be bogus")

from _parity_ref import ref, ref_available  # noqa: E402

pytestmark = pytest.mark.skipif(
    not ref_available(), reason="kimi-agent checkout not available")

FILE = runtime_py.builtin_tools.file

image_compress = ref("kimi_cli.utils.image_compress")
image_format_policy = ref("kimi_cli.utils.image_format_policy")
file_utils = ref("kimi_cli.tools.file.utils")

# The reference modules must not have short-circuited to a native library on
# us; both are pure Python in the kimi-agent checkout.
assert "kimi-agent" in str(Path(image_compress.__file__).resolve())
assert "kimi-agent" in str(Path(file_utils.__file__).resolve())


# ---------------------------------------------------------------------------
# thin wrappers around the two implementations
# ---------------------------------------------------------------------------


def py_sniff(data: bytes):
    dims = image_compress.sniff_image_dimensions(data)
    return None if dims is None else (dims.width, dims.height, dims.transposed)


def cxx_sniff(data: bytes):
    dims = FILE.sniff_image_dimensions(data)
    return None if dims is None else tuple(dims)


def py_detect(path: str, header: bytes | None = None):
    ft = file_utils.detect_file_type(path, header)
    return (ft.kind, ft.mime_type)


def cxx_detect(path: str, header: bytes | None = None):
    return tuple(FILE.detect_file_type(path, header))


def assert_sniff_parity(data: bytes, label: str = "") -> None:
    expected = py_sniff(data)
    got = cxx_sniff(data)
    assert got == expected, (
        f"sniff_image_dimensions mismatch {label}: py={expected} cxx={got} "
        f"len={len(data)} head={data[:48].hex()}")


# ---------------------------------------------------------------------------
# image fixtures (real codec output, built deterministically in-process)
# ---------------------------------------------------------------------------

Image = pytest.importorskip("PIL.Image", reason="Pillow is required to build fixtures")


def _save(img, fmt: str, **kw) -> bytes:
    buf = io.BytesIO()
    img.save(buf, format=fmt, **kw)
    return buf.getvalue()


def _rgb(size, color=(51, 102, 204)):
    return Image.new("RGB", size, color)


def _bmp(width: int, height: int, tail: int = 40) -> bytes:
    """BITMAPFILEHEADER (14 B) + width/height at their real DIB offsets 18/22."""
    return (b"BM" + bytes(16) + width.to_bytes(4, "little", signed=True) +
            height.to_bytes(4, "little", signed=True) + bytes(tail))


def _mm_exif_jpeg(width: int, height: int, orientation: int) -> bytes:
    """A JPEG whose APP1 EXIF block is *big endian* ('MM'), like camera output."""
    payload = b"Exif\x00\x00" + b"MM" + b"\x00\x2a" + (8).to_bytes(4, "big")
    payload += (1).to_bytes(2, "big")                      # 1 IFD0 entry
    payload += (0x0112).to_bytes(2, "big")                 # Orientation
    payload += (3).to_bytes(2, "big")                      # SHORT
    payload += (1).to_bytes(4, "big")                      # count
    payload += orientation.to_bytes(2, "big") + bytes(2)   # value + pad
    seg_len = len(payload) + 2
    data = b"\xff\xd8" + b"\xff\xe1" + seg_len.to_bytes(2, "big") + payload
    data += b"\xff\xc0\x00\x08\x08" + height.to_bytes(2, "big") + width.to_bytes(2, "big") + b"\x00"
    return data


def _fixture_bytes() -> "dict[str, bytes]":
    fx: dict[str, bytes] = {}
    fx["png"] = _save(_rgb((3, 4)), "PNG")
    fx["png_large"] = _save(_rgb((2200, 1100)), "PNG")
    fx["png_big_w"] = b"\x89PNG\r\n\x1a\n" + bytes(8) + b"\xff\xff\xff\xff" + (1).to_bytes(4, "big") + bytes(16)
    fx["png_u31"] = b"\x89PNG\r\n\x1a\n" + bytes(8) + (0x80000000).to_bytes(4, "big") + (3).to_bytes(4, "big") + bytes(16)
    fx["gif87"] = _save(_rgb((5, 6)), "GIF")
    fx["gif89"] = _save(_rgb((7, 8)), "GIF")
    gif = fx["gif89"]
    fx["gif_comment"] = gif[:6] + gif[6:13] + b"\x21\xfe\x05hello\x00" + gif[13:]
    # Logical screen 10x20 with a 5x5 frame: the sniffer must report the
    # logical screen (the frame descriptor only sizes that frame).
    fx["gif_frame_rect"] = (b"GIF89a" + (10).to_bytes(2, "little") + (20).to_bytes(2, "little") +
                            bytes([0x00, 0x00, 0x00]) + b"\x2c" + bytes(4) +
                            (5).to_bytes(2, "little") + (5).to_bytes(2, "little") + bytes(10))
    fx["bmp"] = _save(_rgb((7, 8)), "BMP")
    fx["bmp_neg_height"] = _bmp(100, -200)
    fx["bmp_min_height"] = _bmp(100, -2147483648)
    fx["bmp_max_width"] = _bmp(2147483647, 5)
    fx["bmp_neg_width"] = _bmp(-5, 5)
    fx["bmp_os2"] = b"BM" + bytes(2) + (12).to_bytes(4, "little") + b"\x00" * 4 + \
        (12).to_bytes(4, "little") + (3).to_bytes(2, "little") + (4).to_bytes(2, "little") + bytes(40)
    fx["webp_lossy"] = _save(_rgb((9, 10)), "WEBP")
    fx["webp_lossless"] = _save(_rgb((11, 12), (1, 2, 3)), "WEBP", lossless=True)
    fx["webp_vp8x"] = _save(Image.new("RGBA", (21, 22), (1, 2, 3, 4)), "WEBP")
    frames = [_rgb((32, 16), (255, 0, 0)), _rgb((32, 16), (0, 255, 0))]
    fx["webp_anim"] = _save(frames[0], "WEBP", save_all=True, append_images=frames[1:],
                            duration=100, loop=0)
    fx["webp_vp8x_canvas"] = (b"RIFF" + bytes(4) + b"WEBPVP8X" + bytes(8) + bytes([0x02]) +
                             bytes(5) + (12).to_bytes(3, "little") + (16).to_bytes(3, "little"))
    fx["webp_vp8_masked"] = (b"RIFF" + bytes(4) + b"WEBPVP8 " + bytes(8) +
                             (0xE234).to_bytes(2, "little") + (0xD345).to_bytes(2, "little") + bytes(4))
    fx["webp_vp8l_packed"] = (b"RIFF" + bytes(4) + b"WEBPVP8L" + bytes(8) +
                              ((12) | (16 << 14)).to_bytes(4, "little") + bytes(4))
    fx["apng"] = _save(frames[0], "PNG", save_all=True,
                       append_images=[Image.new("RGBA", (32, 16), (0, 0, 255))], duration=100)
    fx["jpeg"] = _save(_rgb((60, 20)), "JPEG")
    fx["jpeg_progressive"] = _save(_rgb((60, 20), (1, 2, 3)), "JPEG", progressive=True)
    fx["jpeg_restart"] = _save(_rgb((300, 300), (7, 8, 9)), "JPEG", restart_marker_blocks=1)
    jpeg = fx["jpeg"]
    fx["jpeg_com"] = jpeg[:2] + b"\xff\xfe\x00\x08comment!" + jpeg[2:]
    fx["jpeg_pad"] = jpeg[:2] + b"\xff" * 5 + jpeg[2:]
    fx["jpeg_rst_marker"] = jpeg[:2] + b"\xff\xd0\xff\xd1" + jpeg[2:]
    for orientation in range(1, 9):
        exif = Image.Exif()
        exif[0x0112] = orientation
        fx[f"jpeg_exif{orientation}"] = _save(_rgb((60, 20), (9, 9, 9)), "JPEG", exif=exif)
        fx[f"jpeg_exif_mm{orientation}"] = _mm_exif_jpeg(60, 20, orientation)
    fx["jpeg_trailing_garbage"] = _save(_rgb((60, 20)), "JPEG") + b"garbage"
    fx["tiff"] = _save(_rgb((5, 5)), "TIFF")
    fx["ppm"] = _save(_rgb((4, 4)), "PPM")
    fx["ico"] = _save(_rgb((16, 16)), "ICO")
    ico_hdr = (b"\x00\x00\x01\x00" + (2).to_bytes(2, "little") + bytes([16, 16, 0, 0]) +
               (1).to_bytes(2, "little") + (32).to_bytes(2, "little") +
               (22).to_bytes(4, "little") + (100).to_bytes(4, "little"))
    fx["ico_multientry"] = ico_hdr + _save(_rgb((16, 16)), "PNG")
    fx["empty"] = b""
    fx["text"] = b"not an image"
    fx["png_magic_only"] = b"\x89PNG\r\n\x1a\n"
    fx["gif_short"] = b"GIF89a\x01"
    fx["bmp_short"] = b"BMxxxx"
    fx["riff_short"] = b"RIFF\x00\x00\x00\x00WEBP"
    fx["ff_d8_junk"] = b"\xff\xd8" + b"\x00" * 40
    fx["ff_d8_sof_no_exif"] = (b"\xff\xd8" + b"\xff\xc0\x00\x11\x08" + (17).to_bytes(2, "big") +
                               (13).to_bytes(2, "big") + b"\x03" + bytes(9))
    return fx


FIXTURES = _fixture_bytes()


# ---------------------------------------------------------------------------
# FOCUS 1 -- sniff_image_dimensions
# ---------------------------------------------------------------------------


def test_sniff_matches_reference_on_real_images():
    """Every fixture, from real encoders and hand-built headers."""
    for name, data in sorted(FIXTURES.items()):
        assert_sniff_parity(data, name)


def test_sniff_matches_reference_on_every_truncation():
    """A parser that reads past the buffer or drops a bounds check shows up here."""
    for name, data in sorted(FIXTURES.items()):
        limit = min(len(data), 256)
        for cut in range(limit + 1):
            assert_sniff_parity(data[:cut], f"{name}@{cut}")


def test_sniff_large_dimensions_are_not_truncated():
    """PNG stores uint32 dimensions; BMP takes abs() of a signed int32.

    The Python sniffer returns arbitrary-precision ints, so a poisoned header
    can legitimately report 4294967295 x 1 (PNG) or height 2147483648 (BMP,
    abs(int32 min)).  A 32-bit C++ field made those negative.
    """
    cases = {
        "png_max_u32": b"\x89PNG\r\n\x1a\n" + bytes(8) +
                       (0xFFFFFFFF).to_bytes(4, "big") + (0xFFFFFFFF).to_bytes(4, "big") + bytes(16),
        "png_2_31": b"\x89PNG\r\n\x1a\n" + bytes(8) +
                    (0x80000000).to_bytes(4, "big") + (0x80000001).to_bytes(4, "big") + bytes(16),
        "bmp_min_height": _bmp(100, -2147483648),
    }
    for label, data in cases.items():
        expected = py_sniff(data)
        got = cxx_sniff(data)
        assert got == expected, f"{label}: py={expected} cxx={got}"
    # and the values really are outside the int32 range (guards the regression)
    assert cxx_sniff(cases["png_max_u32"]) == (4294967295, 4294967295, False)
    assert cxx_sniff(cases["bmp_min_height"]) == (100, 2147483648, False)


def test_sniff_fuzz_matches_reference():
    """Seeded fuzz over random bytes, magic-prefixed buffers and mutated fixtures."""
    rng = random.Random(0x5EED_1A6E)
    magics = [b"\x89PNG\r\n\x1a\n", b"GIF89a", b"GIF87a", b"BM", b"RIFF", b"\xff\xd8",
              b"\xff\xd8\xff\xe0", b"II*\x00", b"MM\x00*", b"\x00\x00\x01\x00", b"", b"RIFF\x00\x00\x00\x00WEBP"]
    bases = [v for v in FIXTURES.values() if v]
    mismatches = []
    for i in range(20000):
        mode = i % 4
        if mode == 0:
            data = rng.randbytes(rng.randint(0, 60))
        elif mode == 1:
            buf = bytearray(rng.randbytes(rng.randint(2, 64)))
            magic = rng.choice(magics)
            buf[0:len(magic)] = magic
            data = bytes(buf)
        elif mode == 2:
            base = rng.choice(bases)
            data = bytearray(base[:rng.randint(0, min(len(base), 300))])
            for _ in range(rng.randint(0, 6)):
                if data:
                    data[rng.randrange(len(data))] = rng.randrange(256)
            data = bytes(data)
        else:
            base = rng.choice(bases)
            data = bytearray(base[:rng.randint(0, min(len(base), 120))])
            for _ in range(rng.randint(0, 12)):
                if data:
                    data[rng.randrange(len(data))] = rng.randrange(256)
            data = bytes(data)
        expected, got = py_sniff(data), cxx_sniff(data)
        if got != expected:
            mismatches.append((i, expected, got, data[:48].hex()))
            if len(mismatches) >= 5:
                break
    assert not mismatches, f"{len(mismatches)} sniff mismatches, first: {mismatches[0]}"


def test_sniff_jpeg_display_space_transpose():
    """EXIF orientation 5-8 swaps into display space; 1-4 must not.

    Both TIFF byte orders are covered (PIL writes little-endian 'II'; the
    ``_mm`` fixtures are hand-built big-endian 'MM' APP1 blocks).
    """
    for prefix in ("jpeg_exif", "jpeg_exif_mm"):
        for orientation in range(1, 9):
            data = FIXTURES[f"{prefix}{orientation}"]
            expected = py_sniff(data)
            assert expected is not None and expected[2] == (orientation >= 5), prefix
            assert cxx_sniff(data) == expected, prefix
    # the MM case really is 20x60 transposed, not a coincidental 60x20
    assert cxx_sniff(FIXTURES["jpeg_exif_mm6"]) == (20, 60, True)
    assert cxx_sniff(FIXTURES["jpeg_exif_mm1"]) == (60, 20, False)


# ---------------------------------------------------------------------------
# FOCUS 2 -- sniff_media_from_magic / detect_file_type
# ---------------------------------------------------------------------------


def _magic_cases():
    asf = bytes.fromhex("3026b2758e66cf11a6d900aa0062ce6c")
    brands = {
        "avif": ("image", "image/avif"), "avis": ("image", "image/avif"),
        "heic": ("image", "image/heic"), "hevc": ("image", "image/heic"),
        "heif": ("image", "image/heif"), "heix": ("image", "image/heif"),
        "mif1": ("image", "image/heif"), "msf1": ("image", "image/heif"),
        "isom": ("video", "video/mp4"), "iso2": ("video", "video/mp4"),
        "iso5": ("video", "video/mp4"), "mp41": ("video", "video/mp4"),
        "mp42": ("video", "video/mp4"), "avc1": ("video", "video/mp4"),
        "mp4v": ("video", "video/mp4"), "m4v": ("video", "video/x-m4v"),
        "qt": ("video", "video/quicktime"), "3gp4": ("video", "video/3gpp"),
        "3gp5": ("video", "video/3gpp"), "3gp6": ("video", "video/3gpp"),
        "3gp7": ("video", "video/3gpp"), "3g2": ("video", "video/3gpp2"),
    }
    cases = [
        ("png", b"\x89PNG\r\n\x1a\n" + bytes(40)),
        ("jpeg", b"\xff\xd8\xff\xe0" + bytes(40)),
        ("gif87", b"GIF87a" + bytes(40)),
        ("gif89", b"GIF89a" + bytes(40)),
        ("bmp", b"BM" + bytes(40)),
        ("tiff_le", b"II*\x00" + bytes(40)),
        ("tiff_be", b"MM\x00*" + bytes(40)),
        ("ico", b"\x00\x00\x01\x00" + bytes(40)),
        ("webp", b"RIFF" + bytes(4) + b"WEBP" + bytes(40)),
        ("avi", b"RIFF" + bytes(4) + b"AVI " + bytes(40)),
        ("riff_other", b"RIFF" + bytes(4) + b"WAVE" + bytes(40)),
        ("riff_short", b"RIFF"),
        ("flv", b"FLV" + bytes(40)),
        ("asf", asf + bytes(40)),
        ("asf_short", asf[:8]),
        ("ebml_webm", b"\x1a\x45\xdf\xa3" + b"....webm...." + bytes(20)),
        ("ebml_webm_upper", b"\x1a\x45\xdf\xa3" + b"....WEBM...." + bytes(20)),
        ("ebml_matroska", b"\x1a\x45\xdf\xa3" + b"matroska" + bytes(20)),
        ("ebml_other", b"\x1a\x45\xdf\xa3" + b"other" + bytes(20)),
        ("none", bytes(64)),
        ("short", b"\x89PNG"),
    ]
    for brand, expected in brands.items():
        payload = ((18).to_bytes(4, "big") + b"ftyp" + brand.encode().ljust(4, b" ") +
                   bytes(4) + brand.encode() + b"isom")
        cases.append((f"ftyp_{brand}", payload, expected))
    # `header[8:12].decode("ascii", errors="ignore").lower().strip()`
    for pad in (b" ", b"\t", b"\n", b"\r", b"\x0b", b"\x0c", b"\x1c", b"\x1d", b"\x1e", b"\x1f"):
        cases.append((f"ftyp_m4v_trailing_{pad!r}",
                      (18).to_bytes(4, "big") + b"ftyp" + b"m4v" + pad + bytes(8),
                      ("video", "video/x-m4v")))
        cases.append((f"ftyp_m4v_leading_{pad!r}",
                      (18).to_bytes(4, "big") + b"ftyp" + pad + b"m4v" + bytes(8),
                      ("video", "video/x-m4v")))
    cases.append(("ftyp_qt_padded_whitespace",
                  (18).to_bytes(4, "big") + b"ftyp" + b"qt\x0c\x0c" + bytes(8),
                  ("video", "video/quicktime")))
    cases.append(("ftyp_m4v_nonascii_pad",
                  (18).to_bytes(4, "big") + b"ftyp" + b"m4v\x85" + bytes(8),
                  ("video", "video/x-m4v")))
    cases.append(("ftyp_nul_pad_is_not_stripped",
                  (18).to_bytes(4, "big") + b"ftyp" + b"m4v\x00" + bytes(8)))
    cases.append(("ftyp_unknown_brand", (18).to_bytes(4, "big") + b"ftypZZZZ" + bytes(8), None))
    cases.append(("ftyp_short", (18).to_bytes(4, "big") + b"ftyp" + b"mp4"))
    cases.append(("ftyp_padded_brand", (18).to_bytes(4, "big") + b"ftyp" + b"mp42" + bytes(8),
                  ("video", "video/mp4")))
    return cases


def test_detect_file_type_magic_table_matches_reference():
    for case in _magic_cases():
        name, payload = case[0], case[1]
        expected = py_detect("no_suffix_here", payload)
        got = cxx_detect("no_suffix_here", payload)
        assert got == expected, f"{name}: py={expected} cxx={got} header={payload[:24].hex()}"
        if len(case) == 3 and case[2] is not None:
            assert expected == case[2], f"{name}: reference changed -> {expected}"


def test_detect_file_type_null_byte_gate_matches_reference():
    binary = b"\x00\x00binary"
    for path in ("notes.txt", "Makefile", ".env", "x.unknown", "x"):
        assert cxx_detect(path, binary) == py_detect(path, binary) == ("unknown", "")
    # a known text suffix plus a non-binary header stays text
    assert cxx_detect("notes.txt", b"hello") == py_detect("notes.txt", b"hello") == ("text", "text/plain")
    # empty header is a real header (Python header=b"")
    assert cxx_detect("x.zzz", b"") == py_detect("x.zzz", b"")


def test_detect_file_type_kind_conflict_matches_reference():
    png = FIXTURES["png"]
    bmp = FIXTURES["bmp"]
    mp4 = (18).to_bytes(4, "big") + b"ftypmp42" + bytes(8)
    cases = [
        ("sample.png", mp4),     # image suffix wins outright
        ("sample.bmp", png),     # image suffix wins outright
        ("sample.txt", png),     # text suffix + image magic -> conflict -> unknown
        ("sample.svg", png),     # text-kind suffix (.svg) + image magic -> unknown
        ("sample", png),
        ("sample.unknown", png),
        ("sample.unknown", bmp),
        ("sample.unknown", mp4),
        ("sample.unknown", b"\x00\x01\x02"),
    ]
    for path, header in cases:
        expected, got = py_detect(path, header), cxx_detect(path, header)
        assert got == expected, f"{path} hdr={header[:12].hex()}: py={expected} cxx={got}"


# --- the mimetypes.guess_type fallback -------------------------------------


@contextmanager
def portable_reference():
    """``file_utils`` with a registry-free ``mimetypes`` database.

    CPython's ``mimetypes`` merges ``HKEY_CLASSES_ROOT`` Content Types into its
    module-level tables on Windows (e.g. ``.3fr -> image/3FR``), which the
    portable C++ port cannot and must not reproduce.  A fresh ``MimeTypes()``
    is built from the stdlib defaults only, which is what the C++ table targets.
    """
    db = __import__("mimetypes").MimeTypes()
    for suffix, mime in file_utils._EXTRA_MIME_TYPES.items():
        db.add_type(mime, suffix)
    with mock.patch.object(file_utils, "mimetypes", db):
        yield db


_LEGACY_AND_EXTRA_SUFFIXES = [
    # the previous hand-curated C++ table (kept as a regression corpus)
    ".jfif", ".pjpeg", ".pjp", ".jpe", ".xbm", ".mng", ".ts", ".mts", ".m2ts", ".ogv",
    ".mpeg", ".mpg", ".mpe", ".mpv", ".mxu", ".m4u", ".viv", ".f4v", ".fli", ".flc",
    ".asf", ".asx", ".wm", ".wmx", ".wvx", ".movie", ".uvv", ".uvh", ".uvm", ".uvp",
    ".uvs", ".uvu", ".fvt", ".dvb", ".pyv",
    # plausible agent-workspace suffixes that must stay text
    ".py", ".md", ".tsx", ".cts", ".json", ".yaml", ".cpp", ".h", ".hpp", ".txt",
    ".log", ".csv", ".html", ".css", ".js", ".rs", ".go", ".sh", ".ps1", ".env",
    ".lock", ".toml", ".ini", ".cfg", ".xml", ".sql", ".bat", ".cmd", ".c", ".cc",
    # stdlib image/video suffixes reachable only through the suffix/encoding fixups
    ".svgz", ".tgz", ".taz", ".tz", ".tbz2", ".txz", ".gz", ".bz2", ".xz", ".br", ".Z",
    ".3gp", ".3g2", ".avif", ".heic", ".heif", ".mkv", ".m4v",
]


def _suffix_corpus():
    import mimetypes as _m
    suffixes = set(_m.types_map) | set(_m.common_types) | set(_m.MimeTypes().types_map[True])
    suffixes |= set(file_utils._TEXT_MIME_BY_SUFFIX) | set(file_utils._IMAGE_MIME_BY_SUFFIX)
    suffixes |= set(file_utils._VIDEO_MIME_BY_SUFFIX) | set(file_utils._NON_TEXT_SUFFIXES)
    suffixes |= set(file_utils._EXTRA_MIME_TYPES)
    suffixes |= set(_LEGACY_AND_EXTRA_SUFFIXES)
    return sorted(s for s in suffixes if s.startswith(".") and len(s) > 1)


def test_detect_file_type_suffix_corpus_matches_portable_reference():
    """Full suffix corpus against the registry-free reference."""
    corpus = _suffix_corpus()
    assert len(corpus) > 400, f"corpus unexpectedly small: {len(corpus)}"
    mismatches = []
    with portable_reference():
        for suffix in corpus:
            path = "file" + suffix
            expected, got = py_detect(path), cxx_detect(path)
            if got != expected:
                mismatches.append((path, expected, got))
    assert not mismatches, f"{len(mismatches)} mismatches, first 10: {mismatches[:10]}"


def test_detect_file_type_matches_live_reference_except_registry():
    """C++ must equal the *live* reference wherever the registry is not involved.

    Every remaining difference is proved to be a Windows-registry entry by
    checking the module-level strict map against the registry-free one.
    """
    import mimetypes as _m
    portable_map = _m.MimeTypes().types_map[True]
    live_map = _m.types_map
    registry_only = []
    mismatches = []
    for suffix in _suffix_corpus():
        path = "file" + suffix
        expected, got = py_detect(path), cxx_detect(path)
        if got == expected:
            continue
        if live_map.get(suffix) != portable_map.get(suffix):
            registry_only.append((suffix, live_map.get(suffix), portable_map.get(suffix)))
            continue
        mismatches.append((path, expected, got))
    assert not mismatches, (
        f"{len(mismatches)} non-registry mismatches: {mismatches[:10]}")
    # Every registry-only entry must really be a suffix whose module-level
    # mapping differs from the stdlib default; the set stays bounded.
    assert len(registry_only) < 90, registry_only
    for suffix, live, portable in registry_only:
        assert live != portable, suffix


def test_detect_file_type_encoding_suffixes_match_live_reference():
    """mimetypes strips one encoding suffix before the lookup (shot.png.gz)."""
    for path in ["a.png.gz", "a.jpeg.bz2", "a.svg.gz", "a.gif.gz", "a.webp.gz",
                 "a.tar.gz", "a.jpg.xz", "a.avif.br", "a.txt.gz", "a.png.zst",
                 "a.PNG.GZ", "dir.d/a.png.gz", "C:\\tmp\\a.png.gz"]:
        expected, got = py_detect(path), cxx_detect(path)
        assert got == expected, f"{path}: py={expected} cxx={got}"


def test_detect_file_type_kimi_agent_own_test_cases():
    """The assertions from kimi-agent's tests/utils/test_file_utils.py.

    ``app.ts``/``component.tsx``/``module.mts``/``common.cts`` are an explicit
    regression test there ("TypeScript files should not be misidentified as
    MPEG Transport Stream (video/mp2t)").
    """
    assert cxx_detect("image.PNG")[0] == "image"
    assert cxx_detect("clip.mp4")[0] == "video"
    assert cxx_detect("notes.txt")[0] == "text"
    assert cxx_detect("Makefile")[0] == "text"
    assert cxx_detect(".env")[0] == "text"
    assert cxx_detect("icon.svg")[0] == "text"
    assert cxx_detect("archive.tar.gz")[0] == "unknown"
    assert cxx_detect("my file.pdf")[0] == "unknown"
    assert cxx_detect("app.ts")[0] == "text"
    assert cxx_detect("component.tsx")[0] == "text"
    assert cxx_detect("module.mts")[0] == "text"
    assert cxx_detect("common.cts")[0] == "text"

    png_header = b"\x89PNG\r\n\x1a\n" + b"pngdata"
    mp4_header = b"\x00\x00\x00\x18ftypmp42\x00\x00\x00\x00mp42isom"
    iso5_header = b"\x00\x00\x00\x18ftypiso5\x00\x00\x00\x00iso5isom"
    binary_header = b"\x00\x00binary"
    assert cxx_detect("sample", png_header)[0] == "image"
    assert cxx_detect("sample.bin", png_header)[1] == "image/png"
    assert cxx_detect("sample", mp4_header)[0] == "video"
    assert cxx_detect("sample", iso5_header)[0] == "video"
    assert cxx_detect("sample.png", mp4_header)[0] == "image"
    assert cxx_detect("notes.txt", binary_header)[0] == "unknown"
    # ... and the port agrees with the reference on all of them
    for path, header in [("app.ts", None), ("module.mts", None), ("archive.tar.gz", None),
                         ("sample", png_header), ("sample.png", mp4_header)]:
        assert cxx_detect(path, header) == py_detect(path, header), path


def test_detect_file_type_path_shapes_match_reference():
    """Windows separators, dotfiles, trailing dots, missing header."""
    for path in ["a.txt", "A.TXT", "a/P/b.PNG", "a\\b\\c.png", ".hidden", ".hidden.png",
                 "..", "..\\a.txt", "C:\\dir\\file.PNG", "a.", "a..b.txt", "no_ext",
                 "x.tar.gz", "x.svgz", "x.tgz", "a b/a b.png", "", "."]:
        for header in (None, b""):
            expected, got = py_detect(path, header), cxx_detect(path, header)
            assert got == expected, f"path={path!r} header={header!r}: py={expected} cxx={got}"


# ---------------------------------------------------------------------------
# FOCUS 2 -- is_model_accepted_image_mime / normalize_image_mime
# ---------------------------------------------------------------------------


def test_is_model_accepted_image_mime_matches_reference():
    corpus = [
        "image/png", "image/jpeg", "image/jpg", "image/gif", "image/webp",
        "IMAGE/PNG", "Image/Jpeg", " image/png ", "image/jpeg; charset=utf-8",
        "image/jpeg;charset=utf-8", "image/jpg; q=1", "image/png;base64",
        "image/avif", "image/heic", "image/heif", "image/bmp", "image/tiff",
        "image/x-icon", "image/svg+xml", "image/x-portable-bitmap", "image/emf",
        "image/webp2", "image/pngx", "text/plain", "video/mp4", "", ";",
        "image/JPG", "image/Png;CAR=1", "  ", "image/png;", "image/png;;",
        "application/octet-stream", "image/png\r\n",
    ]
    for mime in corpus:
        expected = image_format_policy.is_model_accepted_image_mime(mime)
        got = FILE.is_model_accepted_image_mime(mime)
        assert got == expected, f"{mime!r}: py={expected} cxx={got}"


def test_accepted_gate_matches_reference_over_detected_mimes():
    """Whatever detect_file_type returns, the acceptance gate must agree."""
    kinds = ["file" + s for s in _suffix_corpus()]
    with portable_reference():
        for path in kinds:
            kind, mime = py_detect(path)
            if kind != "image":
                continue
            expected = image_format_policy.is_model_accepted_image_mime(mime)
            assert FILE.is_model_accepted_image_mime(mime) == expected, mime
