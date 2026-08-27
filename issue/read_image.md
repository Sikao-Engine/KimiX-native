# read_image — blocker report: image codecs are not vendored

## Summary

The `read_image` tool's **decode → resize → encode** pipeline (phase 2 of
`C:/dev/kimi-agent/plans/read_image.md`) cannot be ported to C++ with the
libraries currently vendored under `src/ext`. Per the shared brief
(`src/builtin_tools/README.md`), a tool that needs a library not in `src/ext`
must NOT vendor it and must instead write this report while still delivering
everything implementable. All decision kernels that do not require a codec
HAVE been ported and are tested (see `src/builtin_tools/reports/read_image.md`).

## The blocker

The following Python machinery is codec-bound and therefore stays in Python:

| Python function | Why it needs a codec |
|---|---|
| `_decode_image` (`image_compress.py:661-679`) | Pillow decodes PNG/JPEG/WebP/GIF and applies EXIF transpose |
| `_encode_png` (`image_compress.py:682-685`) | Pillow PNG encoder (libpng/zlib) |
| `_encode_jpeg` (`image_compress.py:688-694`) | Pillow JPEG encoder (libjpeg) |
| `compress_image_for_model` (`image_compress.py:352-461`) | wraps decode + `_encode_within_budget` |
| `crop_image_for_model` (`image_compress.py:510-645`) | wraps decode + crop + encode |
| `mipmap_downsample` (`image_compress.py:843-991`) | decode + numpy pyramid + `_encode_within_budget` per level |
| `render_pdf_page` (`read_pdf_pages.py:37+`) | PyMuPDF rasterises a PDF page to PNG |
| `auto_convert` branch (`read_media.py:551-575`) | Pillow re-encodes unsupported formats to PNG |

None of these can be expressed over raw bytes without an actual image
decoder/encoder and (for PDF) a page rasteriser.

## What is NOT in `src/ext`

The vendored tree currently contains only:

```
cpp-httplib  mbedtls  mimalloc  pybind11  reproc  xxHash  yyjson
```

- No PNG codec (libpng + zlib) and no `stb_image`/`stb_image_write`.
- No JPEG codec (libjpeg-turbo / mozjpeg).
- No WebP codec (libwebp).
- No PDF page rasteriser (mupdf / poppler).

`stb_image` is specifically called out in the plan (§6: "If phase 2 vendors
stb_image/stb_image_write ... deferred") and is NOT present. Adding any of
these is forbidden by the task brief ("Never vendor a new library, never edit
`src/ext/**`").

## Candidate libraries (for the future phase-2 decision)

| Library | Purpose | Approx. size | Licence |
|---|---|---|---|
| `stb_image` + `stb_image_write` | single-header PNG/JPEG/BMP/GIF/TGA decode + PNG/JPEG/BMP encode | ~300 KB (2 headers) | Public domain / MIT |
| libpng + zlib | reference PNG codec | ~1.2 MB | libpng / zlib |
| libjpeg-turbo | fast JPEG codec | ~3 MB | BSD-style (IJG) |
| libwebp | WebP codec (VP8/VP8L/VP8X, animation) | ~2 MB | BSD-3 |
| mupdf | PDF page rasteriser | ~40 MB (trimmed) | AGPL / commercial |
| poppler | PDF rasteriser (cairo backend) | ~20 MB | GPL-2 (cairo) |

The plan's own phase-2 preference is `stb_image` + `stb_image_write` (single
headers, minimal footprint) or linking libpng/libjpeg-turbo/libwebp when
available. The parity contract would change from byte-identical to
"delivery-equivalent" (≤ budget, same dims/format), per plan §8.

## What ships anyway

Everything that is implementable without a codec is delivered and green:

1. **Media/dimension sniffers** — PNG/GIF/BMP/WebP(VP8/VP8L/VP8X)/JPEG SOFn
   + EXIF orientation (IFD walk, both endians), `is_animated_webp`,
   `sniff_media_from_magic` (incl. ftyp brands, EBML, ASF, ICO, TIFF),
   `detect_file_type` (suffix precedence, kind-conflict gate, NUL gate).
2. **Session-poisoning gate** — `normalize_image_mime`,
   `is_model_accepted_image_mime`, `build_image_conversion_guidance`.
3. **Format policy / budgets** — constants, env resolvers, `format_byte_size`,
   `parse_region_pct` (with the NaN/inf distinction).
4. **Compression ladder** — `fit_dimensions`, `build_ladder_plan`,
   `mipmap_level_dims`, `first_mipmap_level_for_edge`, and the
   `box_downsample_2x2` kernel on raw RGBA/RGB (pure arithmetic, codec-free).
5. **Payload builder** — `to_data_url` (local base64), `build_media_note`,
   all three error builders, `format_media_tag`, preview lines.

Test result: **27 tests / 309 asserts, all passing**
(`test_builtin_read_image.exe` → `Suite 'global': all tests passed`).

The decision logic (which ladder rung, which mip level, which budget) is now
a single native source of truth; only the byte-level codec calls remain in
Pillow until a codec is vendored in a follow-up phase.
