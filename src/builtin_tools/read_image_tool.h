// read_image_tool.h - Decision kernels of the read_image (ReadMediaFile)
// built-in agent tool: media-type sniffing, header-only dimension parsing,
// EXIF orientation, format policy/budgets, compression-ladder planning, the
// 2x2 box-downsample kernel, and payload builders (data URLs, notes, errors,
// tags).
//
// Plan: C:/dev/kimi-agent/plans/read_image.md (§3.1-3.6 design, §7 tests,
// §8 risks, §9 phase plan).
//
// Python source of truth (byte-exact parity target for every string and
// every decision):
//   - kimi-cli/src/kimi_cli/tools/file/utils.py
//       MEDIA_SNIFF_BYTES (13), suffix maps (33-89), _ASF_HEADER (64),
//       _FTYP_*_BRANDS (65-90), _NON_TEXT_SUFFIXES (92-174),
//       _sniff_ftyp_brand (183-187), sniff_media_from_magic (190-225),
//       detect_file_type (228-262)
//   - kimi-cli/src/kimi_cli/utils/image_compress.py
//       budgets/constants (48-127), env resolvers (131-156),
//       sniff_image_dimensions (164-261), _read_exif_orientation (264-308),
//       _is_animated_webp (311-322), _fit_within_edge (697-711),
//       _encode_within_budget rung policy (714-813), mipmap pyramid
//       policy (821-991)
//   - kimi-cli/src/kimi_cli/utils/image_format_policy.py
//       accepted-MIME set (34-39), normalize_image_mime (68-78),
//       is_model_accepted_image_mime (81-88),
//       build_image_conversion_guidance (91-114),
//       _image_conversion_command (117-146)
//   - kimi-cli/src/kimi_cli/tools/file/read_media_shared.py
//       to_data_url (45-47), build_image_delivery_limit_error (50-57),
//       build_media_note (60-119)
//   - kimi-cli/src/kimi_cli/tools/file/read_media.py
//       _build_image_decode_limit_error (84-91),
//       _build_full_resolution_limit_error (134-140), preview line (475-478)
//   - kimi-cli/src/kimi_cli/utils/media_tags.py
//       _format_tag (9-19)
//
// NOT implemented here (BLOCKED - see issue/read_image.md): the
// decode->resize->encode pipeline (phase 2 of the plan) needs image codecs
// (PNG/JPEG/WebP) plus a PDF page rasteriser; none are vendored in src/ext.
// Everything below is a pure decision/byte kernel that works on caller-
// supplied buffers.
//
// Design rules (mirrors tool_types.h header comment): namespace
// kimix::builtin_tools::read_image, kimix:: containers only, fixed-width
// ints, no RTTI, no exceptions across the tool boundary (kernels are
// noexcept where they cannot fail and return optional/string by value).

#pragma once

#include <cstddef>
#include <cstdint>

#include <core/kimix_core.h>

#include "builtin_tools/tool_types.h"

namespace kimix::builtin_tools::read_image {

// ---------------------------------------------------------------------------
// Shared POD types (media_types.h-equivalent, plan §3.1)
// ---------------------------------------------------------------------------

// Media kind as reported by detect_file_type. `unknown` mirrors Python's
// FileType(kind="unknown", mime_type="").
enum class media_kind : uint8_t {
    text,
    image,
    video,
    unknown,
};

// Port of kimi_cli.tools.file.utils.FileType.
struct file_type {
    media_kind kind = media_kind::unknown;
    kimix::string mime_type; // empty for kind == unknown

    bool operator==(const file_type &) const = default;
};

// Port of kimi_cli.utils.image_compress.ImageDimensions.
struct image_dimensions {
    int32_t width = 0;
    int32_t height = 0;
    // True when a JPEG EXIF orientation of 5-8 swapped the reported
    // width/height into display space.
    bool transposed = false;

    bool operator==(const image_dimensions &) const = default;
};

// Port of kimi_cli.utils.image_compress.CropRegion.
struct crop_region {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;

    bool operator==(const crop_region &) const = default;
};

// Port of kimi_cli.tools.file.read_media_shared.ImageDelivery.
struct delivery_info {
    enum class delivery_kind : uint8_t {
        untouched,
        downsampled,
        crop,
        full,
    };

    delivery_kind kind = delivery_kind::untouched;
    // Pixel size of the payload actually sent; 0 when unknown.
    int32_t width = 0;
    int32_t height = 0;
    int64_t byte_length = 0;
    kimix::string mime_type;
    // The crop actually applied (clamped), for kind == crop.
    kimix::optional<crop_region> region;
    // For kind == crop: the crop was additionally downscaled to fit budgets.
    bool resized = false;
    // True when mip-map (2x2 box averaging) was used.
    bool mipmap = false;
};

// One rung of the compression ladder (plan §3.5).
struct ladder_rung {
    enum class encode_format : uint8_t {
        png,
        jpeg,
    };

    encode_format format = encode_format::png;
    // Target longest-edge cap for this rung in px; 0 == "current size".
    int32_t edge = 0;
    // JPEG quality (1-100); 0 for PNG rungs.
    int32_t quality = 0;

    bool operator==(const ladder_rung &) const = default;
};

// ---------------------------------------------------------------------------
// Budgets and limits (image_compress.py 48-127)
// ---------------------------------------------------------------------------

// Built-in longest-edge ceiling (px); the env var
// KIMI_IMAGE_MAX_EDGE_PX overrides it when it holds a positive integer.
inline constexpr int32_t k_max_image_edge_px = 2000;
// Raw-byte budget for a single image: 3.75 MiB stays under a 5 MB
// base64-encoded ceiling (int(3.75 * 1024 * 1024)).
inline constexpr int64_t k_image_byte_budget = 3932160;
// Built-in raw-byte budget for images the model reads for itself
// (read_image default path): 256 KiB.
inline constexpr int64_t k_read_image_byte_budget = 262144;
// Pixel-count ceiling above which compression is skipped entirely
// (decompression-bomb guard).
inline constexpr int64_t k_max_decode_pixels = 100'000'000;
// Raw-byte ceiling above which compression is skipped rather than decoded.
inline constexpr int64_t k_max_image_decode_bytes = 67108864; // 64 MiB
// Raw-byte ceiling for mipmap decode attempts.
inline constexpr int64_t k_max_mipmap_decode_bytes = 134217728; // 128 MiB
// Minimum dimension for mipmap levels.
inline constexpr int32_t k_min_mipmap_edge_px = 2;
// PNG rescales stop at this edge; below it the ladder goes lossy.
inline constexpr int32_t k_png_rescale_floor_px = 1000;
// Progressively lower JPEG quality until the payload fits the byte budget.
inline constexpr int32_t k_jpeg_quality_steps[4] = {80, 60, 40, 20};
// Longest-edge step-downs tried when the budget cannot be met at the fitted
// size.
inline constexpr int32_t k_fallback_edges_px[6] = {2000, 1000, 768, 512, 384, 256};
// Number of leading header bytes sniffed for magic detection.
inline constexpr size_t k_media_sniff_bytes = 512;
// read_media.py MAX_MEDIA_MEGABYTES (100 MB ceiling for media files).
inline constexpr int32_t k_max_media_megabytes = 100;
// DPI fallback sequence for over-budget PDF page screenshots
// (read_pdf_pages.py _DPI_SEQUENCE).
inline constexpr int32_t k_pdf_dpi_sequence[3] = {150, 96, 72};

// ---------------------------------------------------------------------------
// image_sniff (image_compress.py 164-322)
// ---------------------------------------------------------------------------

// Best-effort pixel-dimension reader for PNG/GIF/BMP/WebP/JPEG. Inspects only
// the fixed region near the start of the file where each format records its
// dimensions. Returns nullopt for formats whose dimensions are not locatable
// from that region, or when the supplied buffer is too short. JPEG dimensions
// are reported in DISPLAY space: an EXIF Orientation of 5-8 swaps width and
// height and sets transposed.
kimix::optional<image_dimensions> sniff_image_dimensions(kimix::string_view data) noexcept;

// Read the Orientation tag (0x0112) out of a JPEG APP1 payload: returns 1-8,
// or nullopt when the payload is not EXIF, is truncated, or carries no valid
// orientation. Only IFD0 is examined. `start` is the offset just after the
// APP1 marker length field ('Exif\0\0' preamble expected there); `end` is the
// segment end and is clamped to data.size() internally (mirrors Python's
// min(end, len(data))).
kimix::optional<int32_t> read_exif_orientation(kimix::string_view data, size_t start, size_t end) noexcept;

// True when the payload is a WebP whose VP8X container header carries the
// ANIM flag (bit 0x02 at offset 20).
bool is_animated_webp(kimix::string_view data) noexcept;

// ---------------------------------------------------------------------------
// file_type (tools/file/utils.py 183-262)
// ---------------------------------------------------------------------------

// Magic-byte media sniff over the first min(data.size(), 512) bytes:
// PNG/JPEG/GIF/BMP/TIFF/ICO/WebP/AVI/FLV/ASF/EBML(webm,matroska)/ISO-BMFF
// ftyp brands. Returns nullopt when no magic matches.
kimix::optional<file_type> sniff_media_from_magic(kimix::string_view data) noexcept;

// Full file-type decision: explicit suffix maps win for image/video, then
// header sniffing (kind-conflict with the suffix -> unknown), then a NUL-byte
// binary gate, then the text-map suffix (.svg), then _NON_TEXT_SUFFIXES ->
// unknown, else text/plain. `has_header` mirrors Python's header=None check
// (sniffing and the NUL gate only run when a header was read); an empty
// header with has_header=true is a real (empty) file header, exactly like
// Python's b"". A curated image/video suffix fallback table stands in for
// Python's mimetypes.guess_type (parity limitation, plan §8).
file_type detect_file_type(kimix::string_view path, kimix::string_view header, bool has_header = true) noexcept;

// ---------------------------------------------------------------------------
// image_policy (image_format_policy.py + image_compress.py)
// ---------------------------------------------------------------------------

// Lowercase, drop MIME parameters (strip at ';'), trim, apply the
// image/jpg -> image/jpeg alias.
kimix::string normalize_image_mime(kimix::string_view mime) noexcept;

// Session-poisoning gate: only the closed accepted set
// {image/png, image/jpeg, image/gif, image/webp} (after normalization) may
// ever be sent to the model.
bool is_model_accepted_image_mime(kimix::string_view mime) noexcept;

// Refusal text for an unsupported image format, with a per-OS conversion
// command. `os_kind` is "macOS", "Linux", "Windows", or anything else.
kimix::string build_image_conversion_guidance(kimix::string_view path, kimix::string_view mime, kimix::string_view os_kind) noexcept;

// Longest-edge ceiling (px): env KIMI_IMAGE_MAX_EDGE_PX (positive integer
// only) > built-in k_max_image_edge_px.
int32_t resolve_max_image_edge_px() noexcept;

// Read-image byte budget: env KIMI_IMAGE_READ_BYTE_BUDGET (positive integer
// only) > built-in k_read_image_byte_budget.
int64_t resolve_read_image_byte_budget() noexcept;

// Overridable variants for deterministic tests: when `raw` holds a positive
// integer (digits only), it is returned; otherwise `fallback`.
int32_t resolve_max_image_edge_px_from(kimix::string_view raw, int32_t fallback) noexcept;
int64_t resolve_read_image_byte_budget_from(kimix::string_view raw, int64_t fallback) noexcept;

// Human-readable byte size: "640 B" / "128 KB" (JS Math.round half-up) /
// "3.8 MB" (Python f"{x:.1f}" round-half-even parity).
kimix::string format_byte_size(int64_t n) noexcept;

// Parse "x,y,width,height" percentages into an original-image pixel region:
// x/y = int(dim * pct / 100.0), w/h = max(1, int(dim * pct / 100.0)).
// Exactly 4 comma-separated float fields; anything else (bad arity, non-
// numeric, NaN) returns nullopt. Python raises OverflowError on +-inf and
// the outer handler turns it into a generic failure — pass is_overflow out
// so callers can distinguish that path (see plan §8).
kimix::optional<crop_region> parse_region_pct(kimix::string_view spec, int32_t orig_w, int32_t orig_h, bool *is_overflow = nullptr) noexcept;

// ---------------------------------------------------------------------------
// compress_ladder (image_compress.py 697-991, plan §3.5)
// ---------------------------------------------------------------------------

// Scale so the longest edge is at most `edge`, preserving aspect ratio
// (new = max(1, floor(dim * edge/longest + 0.5))). Returns nullopt when the
// image already fits (never enlarges) — mirrors Python returning the same
// object.
kimix::optional<std::pair<int32_t, int32_t>> fit_dimensions(int32_t w, int32_t h, int32_t edge) noexcept;

// The rung-order policy of _encode_within_budget (no codec involved: the
// caller executes each rung with a real encoder and stops at the first rung
// meeting the byte budget, else uses the smallest buffer produced).
// prefer_lossless: true for PNG/WebP sources (PNG-first ladder), false for
// JPEG sources (JPEG ladder at every size).
kimix::vector<ladder_rung> build_ladder_plan(bool prefer_lossless, int32_t current_w, int32_t current_h, int64_t byte_budget) noexcept;

// Mip-map level dimension sequence: (w, h), then repeated 2x2 halving until
// a level is below k_min_mipmap_edge_px or the next level would be.
kimix::vector<std::pair<int32_t, int32_t>> mipmap_level_dims(int32_t w, int32_t h) noexcept;

// First level whose longest edge <= max_edge, else the last (smallest)
// level — mirrors mipmap_downsample's selection.
kimix::optional<size_t> first_mipmap_level_for_edge(kimix::span<const std::pair<int32_t, int32_t>> levels, int32_t max_edge) noexcept;

// Non-overlapping 2x2 box average on raw interleaved pixels: input
// (w, h, channels), output (w/2, h/2, channels); odd edges are floored
// (right/bottom dropped). channels must be 3 or 4, src/dst must be valid for
// the implied sizes and must not alias. Parity: numpy mean(axis=(1,3))
// .astype(uint8) truncates, which equals integer (a+b+c+d)/4 for uint8.
void box_downsample_2x2(const uint8_t *src, int32_t w, int32_t h, int32_t channels, uint8_t *dst) noexcept;

// ---------------------------------------------------------------------------
// payload_builder (read_media_shared.py, read_media.py, media_tags.py)
// ---------------------------------------------------------------------------

// "data:{mime};base64,{payload}" with standard padded base64 (no line
// breaks) — byte-identical to pybase64.b64encode.
kimix::string to_data_url(kimix::string_view mime, kimix::span<const uint8_t> data) noexcept;

// The model-facing <system> media note (build_media_note): reports MIME,
// byte size, original dimensions and exactly how the image was delivered
// (untouched/downsampled+mipmap/crop/full), plus coordinate guidance and the
// re-read reminder.
kimix::string build_media_note(media_kind kind, kimix::string_view mime, int64_t byte_size, kimix::optional<image_dimensions> dims, kimix::optional<delivery_info> delivery) noexcept;

// "Image is too large to send safely after compression ..."
kimix::string build_image_delivery_limit_error(int64_t final_bytes, int64_t read_byte_budget, int32_t max_edge) noexcept;

// "Image is too large to process safely for region or full_resolution ..."
kimix::string build_image_decode_limit_error(int64_t final_bytes) noexcept;

// '"{path}" is {final_bytes} bytes ..., over the IMAGE_BYTE_BUDGET ...'
kimix::string build_full_resolution_limit_error(kimix::string_view path, int64_t final_bytes) noexcept;

// Port of media_tags._format_tag: attrs sorted by key, falsy (empty) values
// skipped, values HTML-escaped with quote=True (& < > " ' -> &amp; &lt; &gt;
// &quot; &#x27;). Empty attr set (or all skipped) -> "<tag>".
kimix::string format_media_tag(kimix::string_view tag, kimix::span<const std::pair<kimix::string, kimix::string>> attrs) noexcept;

// "[Image: {kind}, {width}x{height}, {byte_length} bytes]\n"
kimix::string build_preview_line(kimix::string_view kind, int32_t width, int32_t height, int64_t byte_length) noexcept;

// "[PDF page image: {kind}, {width}x{height}, {byte_length} bytes]\n"
// (read_pdf_pages.py _build_pdf_delivery_preview, plan §3.6).
kimix::string build_pdf_preview_line(kimix::string_view kind, int32_t width, int32_t height, int64_t byte_length) noexcept;

} // namespace kimix::builtin_tools::read_image
