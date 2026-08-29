# read_image — C++ implementation report

Plan: `C:/dev/kimi-agent/plans/read_image.md`. This report maps the shipped
kernels to the Python source of truth, lists what stayed in Python (and why),
and records every deviation.

## What shipped

Two files under src/builtin_tools/ (namespace kimix::builtin_tools::read_image),
compiled into the kimix-llm static library via the existing
add_files("builtin_tools/*.cpp") glob — no build-file change was needed. A
concrete ReadImage subclass of kimix::builtin_tools::Tool is now declared in
the header and implemented in the .cpp, giving the binding layer a standard
CallableTool2-style entry point.

All kernels are pure decision/byte parsers: no codec, no file-system, no
exceptions across the tool boundary (failures return `std::nullopt` /
structured data).

### 1. Shared POD types (plan §3.1, one header, no extra files)

| C++ type | Python source of truth |
|---|---|
| `media_kind` enum | `FileType.kind` literal set (`utils.py:177-180`) |
| `file_type` | `FileType` dataclass (`utils.py:177-180`) |
| `image_dimensions` | `ImageDimensions` (`image_compress.py:164-170`) |
| `crop_region` | `CropRegion` (`image_compress.py:469-477`) |
| `delivery_info` | `ImageDelivery` (`read_media_shared.py:25-42`) |
| `ladder_rung` | the per-rung (format, edge, quality) tuple of `_encode_within_budget` (`image_compress.py:714-813`) |

Budget/limit constants are byte-exact ports of `image_compress.py:48-127`:
`k_max_image_edge_px=2000`, `k_image_byte_budget=3932160`,
`k_read_image_byte_budget=262144`, `k_max_decode_pixels=100_000_000`,
`k_max_image_decode_bytes=64 MiB`, `k_max_mipmap_decode_bytes=128 MiB`,
`k_min_mipmap_edge_px=2`, `k_png_rescale_floor_px=1000`,
`k_jpeg_quality_steps={80,60,40,20}`,
`k_fallback_edges_px={2000,1000,768,512,384,256}`, `k_media_sniff_bytes=512`,
`k_max_media_megabytes=100`, and the PDF DPI fallback ladder
`k_pdf_dpi_sequence={150,96,72}` (`read_pdf_pages.py:33`).

### 2. Dimension sniffing (plan §3.2)

| C++ | Python reference |
|---|---|
| `sniff_image_dimensions` | `image_compress.py:173-261` |
| `read_exif_orientation` | `image_compress.py:264-308` (`_read_exif_orientation`) |
| `is_animated_webp` | `image_compress.py:311-322` (`_is_animated_webp`) |

PNG IHDR (BE u32 @16/20), GIF LSD (LE u16 @6/8), BMP DIB (LE i32 @18/22,
`abs()` height), WebP VP8 (`&0x3FFF` @26/28) / VP8L (24-bit packed @21-25) /
VP8X (24-bit LE @24-30, +1), JPEG SOFn marker walk (0xC0-0xCF minus
0xC4/0xC8/0xCC; standalone markers skip 2; length-prefixed segments skip
2+len; APP1 feeds the EXIF reader). EXIF orientation ≥ 5 swaps width/height
and sets `transposed` — byte-exact to Python, verified against PIL-encoded
fixtures (see Tests).

### 3. Media detection (plan §3.3)

| C++ | Python reference |
|---|---|
| `sniff_media_from_magic` | `utils.py:190-225` (+ `_sniff_ftyp_brand` 183-187) |
| `detect_file_type` | `utils.py:228-262` |

All magic branches ported: PNG/JPEG/GIF/BMP/TIFF/ICO/WebP/AVI/FLV/ASF/EBML
(case-insensitive `webm`/`matroska` substring) and the full ISO-BMFF
`_FTYP_IMAGE_BRANDS` / `_FTYP_VIDEO_BRANDS` tables. The accepted-MIME
session-poisoning gate lives in `is_model_accepted_image_mime` (below) and is
the kernel the tool calls before ever sending bytes to the model.

### 4. Format policy (plan §3.4)

| C++ | Python reference |
|---|---|
| `normalize_image_mime` | `image_format_policy.py:68-78` |
| `is_model_accepted_image_mime` | `image_format_policy.py:81-88` |
| `build_image_conversion_guidance` | `image_format_policy.py:91-146` (+ `_UNSUPPORTED_IMAGE_FORMATS` 56-63) |
| `resolve_max_image_edge_px` / `resolve_read_image_byte_budget` (+ `_from` variants) | `image_compress.py:131-146` |
| `format_byte_size` | `image_compress.py:149-156` |
| `parse_region_pct` | `read_media.py:300-319` (region_pct→pixel conversion) |

Windows env reads use `GetEnvironmentVariableA` (PEB-backed), mirroring the
rationale documented in `src/runtime/py/module.cpp`; `resolve_*_from`
overloads take the raw env string directly so tests are deterministic.

### 5. Compression ladder (plan §3.5)

| C++ | Python reference |
|---|---|
| `fit_dimensions` | `_fit_within_edge` (`image_compress.py:697-711`) |
| `build_ladder_plan` | rung order of `_encode_within_budget` (`image_compress.py:714-813`) |
| `mipmap_level_dims` | pyramid build of `mipmap_downsample` (`image_compress.py:910-922`) |
| `first_mipmap_level_for_edge` | level selection (`image_compress.py:926-934`) |
| `box_downsample_2x2` | `_mipmap_one_level` (`image_compress.py:821-830`) |

`build_ladder_plan` returns the rung ORDER (PNG-first vs JPEG-first, edges,
qualities); the actual stopping rule ("first rung meeting the byte budget,
else smallest buffer") is applied by the executor with real encoded lengths,
exactly as in Python — decision parity is testable, byte parity is
codec-dependent (plan §8). The 2×2 box kernel is pure arithmetic:
`(a+b+c+d)/4` matches numpy `mean(axis=(1,3)).astype(uint8)` truncation for
uint8 inputs (golden vectors pin it).

6. Payload builder (plan §3.6)

| C++ | Python reference |
|---|---|
| to_data_url (+ file-local static base64 encoder) | read_media_shared.py:45-47 |
| build_media_note | read_media_shared.py:60-119 |
| build_image_delivery_limit_error | read_media_shared.py:50-57 |
| build_image_decode_limit_error | read_media.py:84-91 |
| build_full_resolution_limit_error | read_media.py:134-140 |
| format_media_tag | media_tags.py:9-19 (_format_tag) |
| build_preview_line | read_media.py:475-478 |
| build_pdf_preview_line | read_pdf_pages.py:53-56 (_build_pdf_delivery_preview) |

The base64 encoder is a small local static function (standard alphabet,
padded, no line breaks — byte-identical to pybase64.b64encode). A matching
base64 decoder was added so the Tool wrapper can accept header_b64 / data_b64
parameters from the Python binding.

7. Tool class wrapper (plan §3.5)

| C++ | Role |
|---|---|
| ReadImage : public kimix::builtin_tools::Tool | CallableTool2 binding entry point |

ReadImage::operator() parses JSON parameters (path, header_b64, data_b64,
mime_type, region_pct, full_resolution, info_only, max_megabytes,
max_edge_px, byte_budget, file_size), dispatches to the pure kernels, and
serializes a structured result with ok/status/error, kind/mime_type,
accepted, width/height/transposed/animated, region, ladder, mipmap_levels,
selected_mip_level, media_note, preview_line, data_url and
conversion_guidance. It never touches the filesystem or a codec; decode and
encode remain in Python.

## What stayed in Python, and why (quoted from the plan)

The plan itself phases the codec work out of phase 1:

> "Codec encode/decode stays in Pillow until/unless a follow-up phase vendors
> a codec (see §8/§9); the decision logic is still moved natively so the
> ladder policy is a single source of truth in C++." (§1)

and §8:

> "Codec byte parity impossible in phase 1 — Pillow's PNG/JPEG/WebP encoders
> are C; re-encoding in a new C++ codec would never match byte-for-byte. Keep
> encode/decode in Pillow; parity targets are decisions (ladder rungs, mip
> levels) and strings (notes/URLs/errors), not encoded bytes."

and §9 phase 8:

> "(Optional phase 2, separate plan) vendor stb_image/stb_image_write (or
> link libpng/libjpeg-turbo/libwebp if available) to move
> decode→ladder→encode fully into C++."

Therefore NOT ported: `_decode_image` / `_encode_png` / `_encode_jpeg` /
`compress_image_for_model` / `crop_image_for_model` / `mipmap_downsample`
(the codec-bound wrappers), the Pillow `info_only` dimension read, the
`auto_convert` PNG re-encode, PyMuPDF `render_pdf_page`, and the provider
video upload. All of their DECISION inputs (sniffed dims, budgets, ladder
rungs, mip levels, box kernel, notes, errors, URLs) are now native kernels.
The concrete blocker and candidate libraries are in `issue/read_image.md`.

## Deviations

1. `kimix::nullopt` is not defined by kimix-core (`kimix::optional` is an
   alias of `std::optional` without a companion `nullopt` alias), so the
   kernels use `std::nullopt`. No API/behavior impact.
2. `detect_file_type` takes an explicit `has_header` bool (default `true`)
   because C++ `string_view` cannot express Python's `header=None`. Empty
   header + `has_header=true` is a real (empty) header, exactly like
   Python's `b""`; `has_header=false` mirrors `header=None`. The tool
   binding passes `true` with the 512-byte read, matching today's call sites.
3. `parse_region_pct` reports the `±inf` case through an out-parameter
   (`is_overflow`) instead of raising: Python raises `OverflowError` which
   the outer handler turns into a generic failure; C++ has no exception
   channel here (plan §8). NaN returns `nullopt` exactly like Python's
   `ValueError` path.
4. The Python `suffix.lower()` suffix tables were extended into a curated
   `k_mimetypes_fallback` table standing in for `mimetypes.guess_type`
   (plan §8 "detect_file_type mimetypes gap"). Every explicit map
   (`_IMAGE_MIME_BY_SUFFIX`, `_VIDEO_MIME_BY_SUFFIX`, `_TEXT_MIME_BY_SUFFIX`)
   is ported exactly; only rare fallback suffixes may differ.
5. `build_pdf_preview_line` takes a `kind` argument to match the Python
   reference `read_pdf_pages.py:53-56` (`[PDF page image: {kind}, ...]`);
   the plan's §3.6 sketch omitted the kind field — the Python reference
   wins per AGENT_TASK.md step 5.
6. format_byte_size MB branch implements exact-decimal rounding of
   n/2^20 with round-half-even (Python f"{x:.1f}" semantics). The KB
   branch uses integer (2n+1024)/2048 == floor(n/1024+0.5) (JS
   Math.round half-up). Golden vectors in the test pin both.
7. ReadImage::operator() accepts binary payloads as base64 strings
   (header_b64 / data_b64) because JSON cannot carry raw bytes and the C++
   side performs no filesystem I/O. The exact parameter/result schema is a
   native binding convenience and has no direct Python equivalent; the
   Python side remains authoritative for file reads and codec execution.
8. When no accepted image MIME is detected the wrapper returns
   tool_status::blocked and conversion_guidance using the "Linux" OS branch
   as the default. Other OS branches can be obtained by calling
   build_image_conversion_guidance directly.

## Tests

`tests/unit/builtin_tools/test_read_image_tool.cpp` — Boost.UT, main-scope
`_test` lambdas, synthetic byte-array headers for every sniffer:

- 33 tests, ~337 asserts (Suite 'global': all tests passed).
- Sniffers: PNG/GIF/BMP/WebP(VP8/VP8L/VP8X)/JPEG golden dims, EXIF
  orientation 1-4 vs 5-8 transpose (little + big endian), standalone-marker
  skipping, truncated/unknown → nullopt.
- Magic sniff: every branch incl. ftyp brand tables, EBML substring.
- detect_file_type: suffix precedence, kind-conflict → unknown, NUL gate,
  .svg text map, non-text suffixes, dotfile/Windows-separator handling.
- Policy: normalization/accepted-set, conversion guidance all four OS
  branches, env parsing rules, format_byte_size goldens, parse_region_pct
  (valid/arity/NaN/inf/truncation).
- Ladder: fit_dimensions JS-round, PNG-first and JPEG-first rung orders,
  mipmap level build/selection, box_downsample_2x2 goldens (RGB+RGBA, odd
  floors, truncation parity).
- Payload: data-URL padding vectors (0/1/2/3 bytes), all media-note
  branches, all three error builders, tag sorting/escaping/falsy-skip,
  preview lines, constant values.
- Tool wrapper (ReadImage): null/missing parameters, PNG header dispatch,
  accepted-MIME gate blocking, info_only short-circuit, region_pct
  serialization, data_b64 → data_url.

Parity was additionally verified byte-for-byte against the live Python
reference (`read_media_shared.build_media_note`, `build_image_delivery_limit_error`,
`build_image_conversion_guidance`, `_format_tag`, `format_byte_size`) and
against `sniff_image_dimensions` on PIL-encoded PNG/GIF/BMP/WebP/JPEG
fixtures (including an EXIF orientation-6 JPEG).

Local test registration (NOT committed — the integrator collects these):

```lua
builtin_tools_test("test_builtin_read_image", "unit/builtin_tools/test_read_image_tool.cpp")
```
