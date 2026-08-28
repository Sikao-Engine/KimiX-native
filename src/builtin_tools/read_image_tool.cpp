// read_image_tool.cpp - Decision kernels of the read_image built-in tool.
//
// See read_image_tool.h for the plan / Python source-of-truth map. Every
// string literal below is a byte-exact port of the referenced Python code;
// changes MUST be cross-checked against the source of truth, not invented
// here.

#include "builtin_tools/read_image_tool.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace kimix::builtin_tools::read_image {

namespace {

// -------------------------------------------------------------------------
// Small byte/order helpers (all bounds-checked by callers)
// -------------------------------------------------------------------------

inline uint16_t be_u16(const uint8_t *p) noexcept { return uint16_t((p[0] << 8) | p[1]); }
inline uint16_t le_u16(const uint8_t *p) noexcept { return uint16_t(p[0] | (p[1] << 8)); }
inline int32_t le_i32(const uint8_t *p) noexcept {
    uint32_t v = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    int32_t s;
    std::memcpy(&s, &v, 4);
    return s;
}
inline uint32_t le_u32(const uint8_t *p) noexcept {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline bool starts_with(kimix::string_view data, const char *prefix, size_t prefix_len) noexcept {
    return data.size() >= prefix_len && std::memcmp(data.data(), prefix, prefix_len) == 0;
}

inline char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? char(c + ('a' - 'A')) : c;
}

// Case-insensitive substring search on raw bytes (mirrors Python's
// header.lower() + `in`).
bool ci_contains(kimix::string_view data, const char *needle, size_t needle_len) noexcept {
    if (data.size() < needle_len) return false;
    for (size_t i = 0; i + needle_len <= data.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < needle_len; ++j) {
            if (ascii_lower(data[i + j]) != ascii_lower(needle[j])) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

// -------------------------------------------------------------------------
// Env resolution (image_compress.py _positive_int_from_env 131-136)
// -------------------------------------------------------------------------

#if defined(_WIN32)
// GetEnvironmentVariableA (PEB-backed) rather than std::getenv: MSVC's
// getenv only sees the CRT's environment table after _putenv, not os.environ
// mutations made by an embedding Python; the PEB is the single source of
// truth (same rationale as src/runtime/py/module.cpp's env helper).
kimix::string read_env(const char *name) noexcept {
    const DWORD len = GetEnvironmentVariableA(name, nullptr, 0);
    if (len == 0) return kimix::string();
    kimix::string value(size_t(len - 1), '\0');
    if (GetEnvironmentVariableA(name, value.data(), len) == 0) return kimix::string();
    return value;
}
#else
kimix::string read_env(const char *name) noexcept {
    const char *v = std::getenv(name);
    return v ? kimix::string(v) : kimix::string();
}
#endif

kimix::string_view trimmed(kimix::string_view s) noexcept {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

// ^[0-9]+$ with trim, then a bounded integer parse (Python ints are
// arbitrary precision; overflow here falls back exactly like a non-numeric
// value, which is indistinguishable downstream).
kimix::optional<int64_t> positive_int_from(kimix::string_view raw) noexcept {
    raw = trimmed(raw);
    if (raw.empty()) return std::nullopt;
    int64_t value = 0;
    for (char c : raw) {
        if (c < '0' || c > '9') return std::nullopt;
        if (value > (INT64_MAX - (c - '0')) / 10) return std::nullopt;
        value = value * 10 + (c - '0');
    }
    if (value <= 0) return std::nullopt;
    return value;
}

// -------------------------------------------------------------------------
// Suffix / brand tables (tools/file/utils.py 33-174)
// -------------------------------------------------------------------------

struct suffix_entry {
    const char *suffix;
    const char *mime;
};

// _TEXT_MIME_BY_SUFFIX
constexpr suffix_entry k_text_suffixes[] = {
    {".svg", "image/svg+xml"},
};

// _IMAGE_MIME_BY_SUFFIX
constexpr suffix_entry k_image_suffixes[] = {
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".bmp", "image/bmp"},
    {".tif", "image/tiff"},
    {".tiff", "image/tiff"},
    {".webp", "image/webp"},
    {".ico", "image/x-icon"},
    {".heic", "image/heic"},
    {".heif", "image/heif"},
    {".avif", "image/avif"},
    {".svgz", "image/svg+xml"},
};

// _VIDEO_MIME_BY_SUFFIX
constexpr suffix_entry k_video_suffixes[] = {
    {".mp4", "video/mp4"},
    {".mkv", "video/x-matroska"},
    {".avi", "video/x-msvideo"},
    {".mov", "video/quicktime"},
    {".wmv", "video/x-ms-wmv"},
    {".webm", "video/webm"},
    {".m4v", "video/x-m4v"},
    {".flv", "video/x-flv"},
    {".3gp", "video/3gpp"},
    {".3g2", "video/3gpp2"},
};

// Curated stand-in for Python's mimetypes.guess_type fallback step (union of
// the stdlib map + _EXTRA_MIME_TYPES that yields image/* or video/* and is
// not already covered by the explicit maps above). Parity limitation
// documented in the plan (§8) and the implementation report.
constexpr suffix_entry k_mimetypes_fallback[] = {
    {".jfif", "image/jpeg"},
    {".pjpeg", "image/jpeg"},
    {".pjp", "image/jpeg"},
    {".jpe", "image/jpeg"},
    {".xbm", "image/x-xbitmap"},
    {".mng", "video/x-mng"},
    {".ts", "video/mp2t"},
    {".mts", "video/mp2t"},
    {".m2ts", "video/mp2t"},
    {".ogv", "video/ogg"},
    {".mpeg", "video/mpeg"},
    {".mpg", "video/mpeg"},
    {".mpe", "video/mpeg"},
    {".mpv", "video/mpeg"},
    {".mxu", "video/vnd.mpegurl"},
    {".m4u", "video/vnd.mpegurl"},
    {".viv", "video/vnd.vivo"},
    {".f4v", "video/x-f4v"},
    {".fli", "video/x-fli"},
    {".flc", "video/x-fli"},
    {".asf", "video/x-ms-asf"},
    {".asx", "video/x-ms-asf"},
    {".wm", "video/x-ms-wm"},
    {".wmx", "video/x-ms-wmx"},
    {".wvx", "video/x-ms-wvx"},
    {".movie", "video/x-sgi-movie"},
    {".uvv", "video/vnd.dece.video"},
    {".uvh", "video/vnd.dece.hd"},
    {".uvm", "video/vnd.dece.mobile"},
    {".uvp", "video/vnd.dece.pd"},
    {".uvs", "video/vnd.dece.sd"},
    {".uvu", "video/vnd.uvvu.mp4"},
    {".fvt", "video/vnd.fvt"},
    {".dvb", "video/vnd.dvb.file"},
    {".pyv", "video/vnd.ms-playready.media.pyv"},
};

// _NON_TEXT_SUFFIXES
constexpr const char *k_non_text_suffixes[] = {
    ".icns", ".psd", ".ai", ".eps",
    ".pdf", ".doc", ".docx", ".dot", ".dotx", ".rtf", ".odt",
    ".xls", ".xlsx", ".xlsm", ".xlt", ".xltx", ".xltm", ".ods",
    ".ppt", ".pptx", ".pptm", ".pps", ".ppsx", ".odp",
    ".pages", ".numbers", ".key",
    ".zip", ".rar", ".7z", ".tar", ".gz", ".tgz", ".bz2", ".xz", ".zst",
    ".lz", ".lz4", ".br", ".cab", ".ar", ".deb", ".rpm",
    ".mp3", ".wav", ".flac", ".ogg", ".oga", ".opus", ".aac", ".m4a", ".wma",
    ".ttf", ".otf", ".woff", ".woff2",
    ".exe", ".dll", ".so", ".dylib", ".bin", ".apk", ".ipa", ".jar",
    ".class", ".pyc", ".pyo", ".wasm",
    ".dmg", ".iso", ".img", ".sqlite", ".sqlite3", ".db", ".db3",
};

constexpr const char *k_asf_header =
    "\x30\x26\xb2\x75\x8e\x66\xcf\x11\xa6\xd9\x00\xaa\x00\x62\xce\x6c";

// PurePath(str(path)).suffix.lower(): last '.' in the file name (the part
// after the last separator); empty when the name starts with '.' or has no
// '.'.
kimix::string_view path_suffix_lower(kimix::string_view path) noexcept {
    size_t name_begin = 0;
    for (size_t i = path.size(); i-- > 0;) {
        if (path[i] == '/' || path[i] == '\\') {
            name_begin = i + 1;
            break;
        }
    }
    size_t dot = kimix::string_view::npos;
    for (size_t i = path.size(); i-- > name_begin;) {
        if (path[i] == '.') {
            dot = i;
            break;
        }
    }
    if (dot == kimix::string_view::npos || dot == name_begin) return kimix::string_view();
    kimix::string_view suffix = path.substr(dot);
    return suffix; // caller compares against lowercase keys via icmp
}

bool suffix_icmp(kimix::string_view suffix, const char *lower_key) noexcept {
    const size_t n = std::strlen(lower_key);
    if (suffix.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (ascii_lower(suffix[i]) != lower_key[i]) return false;
    }
    return true;
}

template <size_t N>
const char *lookup_suffix(kimix::string_view suffix, const suffix_entry (&table)[N]) noexcept {
    for (const auto &entry : table) {
        if (suffix_icmp(suffix, entry.suffix)) return entry.mime;
    }
    return nullptr;
}

template <size_t N>
bool in_non_text(kimix::string_view suffix, const char *const (&table)[N]) noexcept {
    for (const char *entry : table) {
        if (suffix_icmp(suffix, entry)) return true;
    }
    return false;
}

// _sniff_ftyp_brand: ISO-BMFF major brand at offset 8..12, lowercased and
// whitespace-stripped.
kimix::string sniff_ftyp_brand(kimix::string_view header) noexcept {
    if (header.size() < 12 || std::memcmp(header.data() + 4, "ftyp", 4) != 0) {
        return kimix::string();
    }
    kimix::string brand;
    brand.reserve(4);
    for (size_t i = 8; i < 12; ++i) {
        const char c = ascii_lower(header[i]);
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') brand.push_back(c);
    }
    return brand;
}

// -------------------------------------------------------------------------
// Format policy tables (image_format_policy.py 56-63)
// -------------------------------------------------------------------------

struct linux_decoder {
    const char *command;
    const char *package_name;
};

// Normalized MIME -> dedicated Linux decoder (nullptr == none named).
const linux_decoder *unsupported_format_decoder(kimix::string_view normalized_mime) noexcept {
    static const linux_decoder k_heif{"heif-convert", "libheif-examples"};
    if (normalized_mime == "image/avif") return nullptr;
    if (normalized_mime == "image/heic") return &k_heif;
    if (normalized_mime == "image/heif") return &k_heif;
    if (normalized_mime == "image/bmp") return nullptr;
    if (normalized_mime == "image/tiff") return nullptr;
    if (normalized_mime == "image/x-icon") return nullptr;
    return nullptr; // entry absent: still refused, just no tailored hint
}

// _TRAILING_EXTENSION_RE.sub("", path): strip ".<name-chars>" where
// name-chars exclude '.', '/' and '\'.
kimix::string strip_trailing_extension(kimix::string_view path) noexcept {
    for (size_t i = path.size(); i-- > 0;) {
        const char c = path[i];
        if (c == '/' || c == '\\') break;
        if (c == '.') return kimix::string(path.substr(0, i));
    }
    return kimix::string(path);
}

// -------------------------------------------------------------------------
// Local base64 encoder (payload_builder, plan §3.6 — padded standard
// alphabet, no line breaks; byte-identical to pybase64.b64encode)
// -------------------------------------------------------------------------

constexpr char k_base64_alpha[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void base64_encode(kimix::span<const uint8_t> in, kimix::string &out) noexcept {
    const size_t n = in.size();
    out.reserve(out.size() + ((n + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= n) {
        const uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | uint32_t(in[i + 2]);
        out.push_back(k_base64_alpha[(v >> 18) & 63]);
        out.push_back(k_base64_alpha[(v >> 12) & 63]);
        out.push_back(k_base64_alpha[(v >> 6) & 63]);
        out.push_back(k_base64_alpha[v & 63]);
        i += 3;
    }
    const size_t rem = n - i;
    if (rem == 1) {
        const uint32_t v = uint32_t(in[i]) << 16;
        out.push_back(k_base64_alpha[(v >> 18) & 63]);
        out.push_back(k_base64_alpha[(v >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        const uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8);
        out.push_back(k_base64_alpha[(v >> 18) & 63]);
        out.push_back(k_base64_alpha[(v >> 12) & 63]);
        out.push_back(k_base64_alpha[(v >> 6) & 63]);
        out.push_back('=');
    }
}

// HTML escape with quote=True: & < > " ' -> &amp; &lt; &gt; &quot; &#x27;
// (Python html.escape).
kimix::string html_escape_quoted(kimix::string_view value) noexcept {
    kimix::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&#x27;";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// image_sniff
// ---------------------------------------------------------------------------

kimix::optional<int32_t> read_exif_orientation(kimix::string_view data, size_t start, size_t end) noexcept {
    const size_t bounded_end = std::min(end, data.size());
    const uint8_t *const d = reinterpret_cast<const uint8_t *>(data.data());

    // 'Exif\0\0' preamble, then the TIFF header.
    if (start + 6 > bounded_end || std::memcmp(d + start, "Exif\x00\x00", 6) != 0) {
        return std::nullopt;
    }
    const size_t tiff = start + 6;
    if (tiff + 8 > bounded_end) return std::nullopt;

    const bool little_endian = data[tiff] == 'I' && data[tiff + 1] == 'I';
    const bool big_endian = data[tiff] == 'M' && data[tiff + 1] == 'M';
    if (!little_endian && !big_endian) return std::nullopt;

    const auto u16 = [&](size_t offset) -> uint16_t {
        return little_endian ? le_u16(d + offset) : be_u16(d + offset);
    };
    const auto u32 = [&](size_t offset) -> uint32_t {
        return little_endian ? le_u32(d + offset)
                             : (uint32_t(d[offset]) << 24) | (uint32_t(d[offset + 1]) << 16) |
                                   (uint32_t(d[offset + 2]) << 8) | uint32_t(d[offset + 3]);
    };

    if (u16(tiff + 2) != 42) return std::nullopt;
    const size_t ifd = tiff + u32(tiff + 4);
    if (ifd + 2 > bounded_end) return std::nullopt;
    const uint16_t entry_count = u16(ifd);
    for (uint16_t i = 0; i < entry_count; ++i) {
        const size_t entry = ifd + 2 + size_t(i) * 12;
        if (entry + 12 > bounded_end) return std::nullopt;
        if (u16(entry) == 0x0112) {
            // Type SHORT: the value sits in the first two bytes of the
            // 4-byte value field, in the TIFF byte order.
            const uint16_t value = u16(entry + 8);
            if (value >= 1 && value <= 8) return int32_t(value);
            return std::nullopt;
        }
    }
    return std::nullopt;
}

kimix::optional<image_dimensions> sniff_image_dimensions(kimix::string_view data) noexcept {
    const uint8_t *const d = reinterpret_cast<const uint8_t *>(data.data());
    const size_t n = data.size();

    // PNG — IHDR is the first chunk; width/height are big-endian uint32 at
    // offsets 16 and 20.
    if (starts_with(data, "\x89PNG\r\n\x1a\n", 8) && n >= 24) {
        return image_dimensions{int32_t((uint32_t(d[16]) << 24) | (uint32_t(d[17]) << 16) | (uint32_t(d[18]) << 8) | uint32_t(d[19])),
                                int32_t((uint32_t(d[20]) << 24) | (uint32_t(d[21]) << 16) | (uint32_t(d[22]) << 8) | uint32_t(d[23])),
                                false};
    }

    // GIF — logical-screen width/height are little-endian uint16 at 6 and 8.
    if ((starts_with(data, "GIF87a", 6) || starts_with(data, "GIF89a", 6)) && n >= 10) {
        return image_dimensions{int32_t(le_u16(d + 6)), int32_t(le_u16(d + 8)), false};
    }

    // BMP — DIB header width/height are little-endian int32 at 18 and 22
    // (height may be negative for top-down bitmaps).
    if (starts_with(data, "BM", 2) && n >= 26) {
        int32_t height = le_i32(d + 22);
        if (height == INT32_MIN) height = INT32_MAX; // abs() overflow guard
        else if (height < 0) height = -height;
        return image_dimensions{le_i32(d + 18), height, false};
    }

    // WebP — RIFF container; VP8/VP8L/VP8X each store dimensions
    // differently in the chunk that follows the 'WEBP' tag.
    if (starts_with(data, "RIFF", 4) && n >= 30) {
        if (std::memcmp(d + 12, "VP8 ", 4) == 0) {
            return image_dimensions{int32_t(le_u16(d + 26) & 0x3FFF),
                                    int32_t(le_u16(d + 28) & 0x3FFF), false};
        }
        if (std::memcmp(d + 12, "VP8L", 4) == 0 && n >= 25) {
            const uint32_t bits = le_u32(d + 21);
            return image_dimensions{int32_t((bits & 0x3FFF) + 1),
                                    int32_t(((bits >> 14) & 0x3FFF) + 1), false};
        }
        if (std::memcmp(d + 12, "VP8X", 4) == 0) {
            const int32_t width = 1 + int32_t(d[24] | (d[25] << 8) | (d[26] << 16));
            const int32_t height = 1 + int32_t(d[27] | (d[28] << 8) | (d[29] << 16));
            return image_dimensions{width, height, false};
        }
    }

    // JPEG — scan segment markers for a Start-Of-Frame (SOFn) marker, whose
    // payload carries height/width as big-endian uint16. An EXIF APP1
    // segment encountered on the way supplies the orientation.
    if (starts_with(data, "\xff\xd8", 2)) {
        kimix::optional<int32_t> orientation;
        size_t offset = 2;
        while (offset + 9 < n) {
            if (d[offset] != 0xFF) {
                offset += 1;
                continue;
            }
            const uint8_t marker = d[offset + 1];
            // SOFn markers carry frame dimensions; skip SOF4/SOF8/SOF12
            // (0xC4/0xC8/0xCC).
        if (marker >= 0xC0 && marker <= 0xCF &&
            marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                const int32_t height = int32_t(be_u16(d + offset + 5));
                const int32_t width = int32_t(be_u16(d + offset + 7));
                if (orientation.has_value() && *orientation >= 5) {
                    return image_dimensions{height, width, true};
                }
                return image_dimensions{width, height, false};
            }
            // Standalone markers (RSTn, SOI, EOI) carry no length field.
            if (marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7)) {
                offset += 2;
                continue;
            }
            const uint16_t segment_length = be_u16(d + offset + 2);
            if (segment_length < 2) break;
            if (marker == 0xE1 && !orientation.has_value()) {
                orientation = read_exif_orientation(data, offset + 4, offset + 2 + segment_length);
            }
            offset += 2 + segment_length;
        }
    }

    return std::nullopt;
}

bool is_animated_webp(kimix::string_view data) noexcept {
    return data.size() >= 21 &&
           std::memcmp(data.data(), "RIFF", 4) == 0 &&
           std::memcmp(data.data() + 8, "WEBP", 4) == 0 &&
           std::memcmp(data.data() + 12, "VP8X", 4) == 0 &&
           (uint8_t(data[20]) & 0x02) != 0;
}

// ---------------------------------------------------------------------------
// file_type
// ---------------------------------------------------------------------------

kimix::optional<file_type> sniff_media_from_magic(kimix::string_view data) noexcept {
    if (data.size() > k_media_sniff_bytes) data = data.substr(0, k_media_sniff_bytes);
    const uint8_t *const d = reinterpret_cast<const uint8_t *>(data.data());

    if (starts_with(data, "\x89PNG\r\n\x1a\n", 8)) return file_type{media_kind::image, "image/png"};
    if (starts_with(data, "\xff\xd8\xff", 3)) return file_type{media_kind::image, "image/jpeg"};
    if (starts_with(data, "GIF87a", 6) || starts_with(data, "GIF89a", 6)) return file_type{media_kind::image, "image/gif"};
    if (starts_with(data, "BM", 2)) return file_type{media_kind::image, "image/bmp"};
    if (starts_with(data, "II*\x00", 4) || starts_with(data, "MM\x00*", 4)) return file_type{media_kind::image, "image/tiff"};
    if (starts_with(data, "\x00\x00\x01\x00", 4)) return file_type{media_kind::image, "image/x-icon"};
    if (starts_with(data, "RIFF", 4) && data.size() >= 12) {
        if (std::memcmp(d + 8, "WEBP", 4) == 0) return file_type{media_kind::image, "image/webp"};
        if (std::memcmp(d + 8, "AVI ", 4) == 0) return file_type{media_kind::video, "video/x-msvideo"};
    }
    if (starts_with(data, "FLV", 3)) return file_type{media_kind::video, "video/x-flv"};
    if (data.size() >= 16 && std::memcmp(d, k_asf_header, 16) == 0) {
        return file_type{media_kind::video, "video/x-ms-wmv"};
    }
    if (starts_with(data, "\x1a\x45\xdf\xa3", 4)) {
        if (ci_contains(data, "webm", 4)) return file_type{media_kind::video, "video/webm"};
        if (ci_contains(data, "matroska", 8)) return file_type{media_kind::video, "video/x-matroska"};
    }
    const kimix::string brand = sniff_ftyp_brand(data);
    if (!brand.empty()) {
        // _FTYP_IMAGE_BRANDS
        if (brand == "avif" || brand == "avis") return file_type{media_kind::image, "image/avif"};
        if (brand == "heic" || brand == "hevc") return file_type{media_kind::image, "image/heic"};
        if (brand == "heif" || brand == "heix" || brand == "mif1" || brand == "msf1") {
            return file_type{media_kind::image, "image/heif"};
        }
        // _FTYP_VIDEO_BRANDS
        if (brand == "isom" || brand == "iso2" || brand == "iso5" || brand == "mp41" ||
            brand == "mp42" || brand == "avc1" || brand == "mp4v") {
            return file_type{media_kind::video, "video/mp4"};
        }
        if (brand == "m4v") return file_type{media_kind::video, "video/x-m4v"};
        if (brand == "qt") return file_type{media_kind::video, "video/quicktime"};
        if (brand == "3gp4" || brand == "3gp5" || brand == "3gp6" || brand == "3gp7") {
            return file_type{media_kind::video, "video/3gpp"};
        }
        if (brand == "3g2") return file_type{media_kind::video, "video/3gpp2"};
    }
    return std::nullopt;
}

file_type detect_file_type(kimix::string_view path, kimix::string_view header, bool has_header) noexcept {
    const kimix::string_view suffix = path_suffix_lower(path);

    kimix::optional<file_type> media_hint;
    if (const char *mime = lookup_suffix(suffix, k_text_suffixes)) {
        media_hint = file_type{media_kind::text, mime};
    } else if (const char *mime2 = lookup_suffix(suffix, k_image_suffixes)) {
        media_hint = file_type{media_kind::image, mime2};
    } else if (const char *mime3 = lookup_suffix(suffix, k_video_suffixes)) {
        media_hint = file_type{media_kind::video, mime3};
    } else if (const char *mime4 = lookup_suffix(suffix, k_mimetypes_fallback)) {
        // Curated mimetypes.guess_type stand-in (only image/* and video/*
        // outcomes matter here).
        const bool is_video = std::strncmp(mime4, "video/", 6) == 0;
        media_hint = file_type{is_video ? media_kind::video : media_kind::image, mime4};
    }

    if (media_hint && (media_hint->kind == media_kind::image || media_hint->kind == media_kind::video)) {
        return *media_hint;
    }

    if (has_header) {
        if (const auto sniffed = sniff_media_from_magic(header)) {
            if (media_hint && sniffed->kind != media_hint->kind) {
                return file_type{media_kind::unknown, kimix::string()};
            }
            return *sniffed;
        }
        // NUL bytes indicate binary content.
        if (header.find(char(0)) != kimix::string_view::npos) {
            return file_type{media_kind::unknown, kimix::string()};
        }
    }

    if (media_hint) return *media_hint;
    if (in_non_text(suffix, k_non_text_suffixes)) {
        return file_type{media_kind::unknown, kimix::string()};
    }
    return file_type{media_kind::text, "text/plain"};
}

// ---------------------------------------------------------------------------
// image_policy
// ---------------------------------------------------------------------------

kimix::string normalize_image_mime(kimix::string_view mime) noexcept {
    const size_t semi = mime.find(';');
    kimix::string base = kimix::string(trimmed(mime.substr(0, semi)));
    for (char &c : base) c = ascii_lower(c);
    if (base == "image/jpg") return kimix::string("image/jpeg");
    return base;
}

bool is_model_accepted_image_mime(kimix::string_view mime) noexcept {
    const kimix::string normalized = normalize_image_mime(mime);
    return normalized == "image/png" || normalized == "image/jpeg" ||
           normalized == "image/gif" || normalized == "image/webp";
}

kimix::string build_image_conversion_guidance(kimix::string_view path, kimix::string_view mime, kimix::string_view os_kind) noexcept {
    const kimix::string converted = strip_trailing_extension(path) + ".jpg";
    const kimix::string normalized = normalize_image_mime(mime);
    const linux_decoder *decoder = unsupported_format_decoder(normalized);
    const kimix::string magick = kimix::string("magick \"") + kimix::string(path) + "\" \"" + converted + "\"";

    kimix::string guidance;
    if (os_kind == "macOS") {
        guidance = kimix::string("On macOS: sips -s format jpeg \"") + kimix::string(path) +
                   "\" --out \"" + converted + "\"";
    } else if (os_kind == "Linux") {
        if (decoder == nullptr) {
            guidance = kimix::string("On Linux, with ImageMagick: ") + magick;
        } else {
            guidance = kimix::string("On Linux: ") + decoder->command + " \"" + kimix::string(path) +
                       "\" \"" + converted + "\" (package " + decoder->package_name +
                       "), or with ImageMagick: " + magick;
        }
    } else if (os_kind == "Windows") {
        guidance = kimix::string("On Windows, with ImageMagick: ") + magick +
                   " (install it first if missing: winget install ImageMagick.ImageMagick)";
    } else {
        guidance = kimix::string("Options: sips -s format jpeg \"") + kimix::string(path) +
                   "\" --out \"" + converted + "\" (macOS)";
        if (decoder != nullptr) {
            guidance += kimix::string(", ") + decoder->command + " \"" + kimix::string(path) +
                        "\" \"" + converted + "\" (Linux, package " + decoder->package_name + ")";
        }
        guidance += kimix::string(", or ") + magick + " (ImageMagick)";
    }

    return kimix::string("\"") + kimix::string(path) + "\" is an " + kimix::string(mime) +
           " image, which the provider does not accept. " +
           "Convert it to JPEG first, then read the converted file. " + guidance;
}

int32_t resolve_max_image_edge_px_from(kimix::string_view raw, int32_t fallback) noexcept {
    const auto parsed = positive_int_from(raw);
    if (!parsed.has_value() || *parsed > int64_t(INT32_MAX)) return fallback;
    return int32_t(*parsed);
}

int64_t resolve_read_image_byte_budget_from(kimix::string_view raw, int64_t fallback) noexcept {
    const auto parsed = positive_int_from(raw);
    return parsed.value_or(fallback);
}

int32_t resolve_max_image_edge_px() noexcept {
    return resolve_max_image_edge_px_from(read_env("KIMI_IMAGE_MAX_EDGE_PX"), k_max_image_edge_px);
}

int64_t resolve_read_image_byte_budget() noexcept {
    return resolve_read_image_byte_budget_from(read_env("KIMI_IMAGE_READ_BYTE_BUDGET"), k_read_image_byte_budget);
}

kimix::string format_byte_size(int64_t n) noexcept {
    if (n < 1024) return kimix::format("{} B", n);
    if (n < 1024 * 1024) {
        // JS Math.round semantics (round half up), matching the TS original:
        // floor(n / 1024 + 0.5) == floor((2n + 1024) / 2048) for n >= 0.
        const int64_t kb = (2 * n + 1024) / 2048;
        return kimix::format("{} KB", kb);
    }
    // f"{n / (1024 * 1024):.1f} MB": Python float formatting rounds
    // half-to-even at the digit level. Compute the exact decimal digits of
    // n/2^20 (binary fractions are exact in decimal) and round half-even.
    const int64_t mb_floor = n / (1024 * 1024);
    const int64_t rem = n % (1024 * 1024);
    const int64_t tenths = (rem * 10) / (1024 * 1024);
    const int64_t leftover20 = rem * 10 - tenths * (1024 * 1024); // in units of 1/2^20
    int64_t d1 = tenths;
    if (leftover20 * 2 > 1024 * 1024 ||
        (leftover20 * 2 == 1024 * 1024 && tenths % 2 == 1)) {
        ++d1;
    }
    int64_t whole = mb_floor;
    if (d1 == 10) {
        d1 = 0;
        ++whole;
    }
    return kimix::format("{}.{} MB", whole, d1);
}

kimix::optional<crop_region> parse_region_pct(kimix::string_view spec, int32_t orig_w, int32_t orig_h, bool *is_overflow) noexcept {
    if (is_overflow) *is_overflow = false;

    kimix::string_view fields[4];
    size_t field_count = 0;
    size_t start = 0;
    bool too_many = false;
    for (size_t i = 0; i <= spec.size(); ++i) {
        if (i == spec.size() || spec[i] == ',') {
            if (field_count == 4) {
                too_many = true;
                break;
            }
            fields[field_count++] = spec.substr(start, i - start);
            start = i + 1;
        }
    }
    if (too_many || field_count != 4) return std::nullopt;

    double values[4];
    for (size_t i = 0; i < 4; ++i) {
        const std::string field(fields[i]); // strtod needs NUL termination
        const char *begin = field.c_str();
        char *end = nullptr;
        const double value = std::strtod(begin, &end);
        if (end != begin + field.size()) return std::nullopt; // non-numeric / trailing junk
        if (value != value) return std::nullopt;               // NaN -> ValueError path
        if (value == HUGE_VAL || value == -HUGE_VAL) {
            // int(float('inf')) raises OverflowError in Python, which is NOT
            // caught by the tool's except clause -> generic failure (plan §8).
            if (is_overflow) *is_overflow = true;
            return std::nullopt;
        }
        values[i] = value;
    }

    const auto to_px = [](double dim, double pct) -> int64_t {
        const double v = dim * pct / 100.0;
        return v >= 0.0 ? int64_t(v) : -int64_t(-v); // int() truncation toward zero
    };

    crop_region region;
    region.x = int32_t(to_px(orig_w, values[0]));
    region.y = int32_t(to_px(orig_h, values[1]));
    region.width = int32_t(std::max<int64_t>(1, to_px(orig_w, values[2])));
    region.height = int32_t(std::max<int64_t>(1, to_px(orig_h, values[3])));
    return region;
}

// ---------------------------------------------------------------------------
// compress_ladder
// ---------------------------------------------------------------------------

kimix::optional<std::pair<int32_t, int32_t>> fit_dimensions(int32_t w, int32_t h, int32_t edge) noexcept {
    const int32_t longest = std::max(w, h);
    if (longest <= edge) return std::nullopt;
    const double factor = double(edge) / double(longest);
    const auto rounded = [factor](int32_t dim) -> int32_t {
        const double v = double(dim) * factor + 0.5;
        int64_t f = int64_t(v);
        if (v < 0.0 && double(f) != v) --f; // floor for completeness
        return int32_t(std::max<int64_t>(1, f));
    };
    return std::pair<int32_t, int32_t>{rounded(w), rounded(h)};
}

kimix::vector<ladder_rung> build_ladder_plan(bool prefer_lossless, int32_t current_w, int32_t current_h, int64_t byte_budget) noexcept {
    (void)byte_budget; // the stopping rule is applied by the executor using
                       // real encoded lengths; the plan is rung order only.

    kimix::vector<ladder_rung> rungs;
    int32_t w = current_w;
    int32_t h = current_h;

    const auto push_jpeg_ladder = [&rungs](int32_t edge) {
        for (const int32_t quality : k_jpeg_quality_steps) {
            rungs.push_back(ladder_rung{ladder_rung::encode_format::jpeg, edge, quality});
        }
    };

    if (prefer_lossless) {
        // Lossless PNG first: best for screenshots/UI (sharp text) and
        // keeps alpha.
        rungs.push_back(ladder_rung{ladder_rung::encode_format::png, 0, 0});

        // Over budget: progressively smaller PNGs (down to the floor) before
        // going lossy. _fit_within_edge returns the same object when the
        // level already fits -> that rung is skipped.
        for (const int32_t edge : k_fallback_edges_px) {
            if (edge < k_png_rescale_floor_px) break;
            if (const auto fitted = fit_dimensions(w, h, edge)) {
                rungs.push_back(ladder_rung{ladder_rung::encode_format::png, edge, 0});
            }
        }

        // Lossy JPEG ladder (drops transparency) at the floored size, then at
        // each sub-floor edge until the budget is met.
        push_jpeg_ladder(0);
        for (const int32_t edge : k_fallback_edges_px) {
            if (edge >= k_png_rescale_floor_px) continue;
            if (const auto fitted = fit_dimensions(w, h, edge)) {
                push_jpeg_ladder(edge);
            }
        }
        return rungs;
    }

    // JPEG source: quality ladder at the fitted size, then the full ladder
    // again at each fallback rescale.
    push_jpeg_ladder(0);
    for (const int32_t edge : k_fallback_edges_px) {
        if (const auto fitted = fit_dimensions(w, h, edge)) {
            push_jpeg_ladder(edge);
        }
    }
    return rungs;
}

kimix::vector<std::pair<int32_t, int32_t>> mipmap_level_dims(int32_t w, int32_t h) noexcept {
    kimix::vector<std::pair<int32_t, int32_t>> levels;
    levels.push_back({w, h});
    while (w > k_min_mipmap_edge_px && h > k_min_mipmap_edge_px) {
        const int32_t w2 = w / 2;
        const int32_t h2 = h / 2;
        if (w2 < k_min_mipmap_edge_px || h2 < k_min_mipmap_edge_px) break;
        levels.push_back({w2, h2});
        w = w2;
        h = h2;
    }
    return levels;
}

kimix::optional<size_t> first_mipmap_level_for_edge(kimix::span<const std::pair<int32_t, int32_t>> levels, int32_t max_edge) noexcept {
    if (levels.empty()) return std::nullopt;
    for (size_t i = 0; i < levels.size(); ++i) {
        if (std::max(levels[i].first, levels[i].second) <= max_edge) return i;
    }
    return levels.size() - 1;
}

void box_downsample_2x2(const uint8_t *src, int32_t w, int32_t h, int32_t channels, uint8_t *dst) noexcept {
    if (src == nullptr || dst == nullptr || (channels != 3 && channels != 4) || w < 2 || h < 2) {
        return;
    }
    const int32_t w2 = w / 2;
    const int32_t h2 = h / 2;
    const int32_t stride = w * channels;
    for (int32_t y = 0; y < h2; ++y) {
        const uint8_t *row0 = src + int64_t(y) * 2 * stride;
        const uint8_t *row1 = row0 + stride;
        uint8_t *out = dst + int64_t(y) * w2 * channels;
        for (int32_t x = 0; x < w2; ++x) {
            const uint8_t *p00 = row0 + int64_t(x) * 2 * channels;
            const uint8_t *p01 = p00 + channels;
            const uint8_t *p10 = row1 + int64_t(x) * 2 * channels;
            const uint8_t *p11 = p10 + channels;
            uint8_t *o = out + int64_t(x) * channels;
            for (int32_t c = 0; c < channels; ++c) {
                // numpy mean(...).astype(uint8) truncates; for uint8 inputs
                // the float mean truncation equals integer (a+b+c+d)/4.
                o[c] = uint8_t((uint32_t(p00[c]) + p01[c] + p10[c] + p11[c]) / 4);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// payload_builder
// ---------------------------------------------------------------------------

kimix::string to_data_url(kimix::string_view mime, kimix::span<const uint8_t> data) noexcept {
    kimix::string out;
    out.reserve(mime.size() + 22 + ((data.size() + 2) / 3) * 4);
    out += "data:";
    out += mime;
    out += ";base64,";
    base64_encode(data, out);
    return out;
}

kimix::string build_media_note(media_kind kind, kimix::string_view mime, int64_t byte_size, kimix::optional<image_dimensions> dims, kimix::optional<delivery_info> delivery) noexcept {
    const bool is_image = kind == media_kind::image;
    kimix::vector<kimix::string> parts;

    parts.push_back(kimix::format("Read {} file.", is_image ? "image" : "video"));
    parts.push_back(kimix::format("Mime type: {}.", mime));
    parts.push_back(kimix::format("Size: {} bytes.", byte_size));

    if (is_image && dims.has_value()) {
        parts.push_back(kimix::format("Original dimensions: {}x{} pixels.", dims->width, dims->height));
    }

    if (delivery.has_value() && delivery->kind == delivery_info::delivery_kind::downsampled) {
        parts.push_back(kimix::format(
            "The attached image was downsampled to {}x{} pixels ({}, {}) to fit model limits; fine detail may be lost.",
            delivery->width, delivery->height, delivery->mime_type, format_byte_size(delivery->byte_length)));
        parts.push_back(kimix::string(
            "To inspect fine detail, call read_image again with the region parameter "
            "(original-image pixel coordinates) to view a crop at full fidelity."));
        if (delivery->mipmap) {
            parts.push_back(kimix::string(
                "Warning: Mip-map downsampling (2x2 bilinear averaging) was used "
                "because standard compression could not meet the delivery limits; "
                "fine detail may be significantly reduced."));
        }
    } else if (delivery.has_value() && delivery->kind == delivery_info::delivery_kind::crop &&
               delivery->region.has_value()) {
        const crop_region &region = *delivery->region;
        const kimix::string how = delivery->resized
                                      ? kimix::format(", downsampled to {}x{} pixels", delivery->width, delivery->height)
                                      : kimix::string(" at native resolution");
        parts.push_back(kimix::format(
            "Showing region (x={}, y={}, width={}, height={}) of the original image{}.",
            region.x, region.y, region.width, region.height, how));
        parts.push_back(kimix::format(
            "To output coordinates in original-image pixels, locate them within this "
            "crop and add the region offset (x={}, y={}).",
            region.x, region.y));
    } else if (delivery.has_value() && delivery->kind == delivery_info::delivery_kind::full) {
        parts.push_back(kimix::string("Shown at native resolution; no downscaling applied."));
    }

    if (is_image && dims.has_value() &&
        (!delivery.has_value() || delivery->kind != delivery_info::delivery_kind::crop)) {
        parts.push_back(kimix::string(
            "If you need to output coordinates, output relative coordinates first "
            "and compute absolute coordinates using the original image size."));
    }

    parts.push_back(kimix::string(
        "If you generate or edit images or videos via commands or scripts, "
        "read the result back immediately before continuing."));

    kimix::string joined;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) joined.push_back(' ');
        joined += parts[i];
    }
    return kimix::string("<system>") + joined + "</system>";
}

kimix::string build_image_delivery_limit_error(int64_t final_bytes, int64_t read_byte_budget, int32_t max_edge) noexcept {
    return kimix::format(
        "Image is too large to send safely after compression ({} bytes; "
        "limit {} bytes and {}px on the longest edge). "
        "The original image was not sent to the model. Do not retry the same file unchanged. "
        "Use Bash or an available image-processing tool to create a smaller copy within both "
        "limits, then call read_image on the smaller copy.",
        final_bytes, read_byte_budget, max_edge);
}

kimix::string build_image_decode_limit_error(int64_t final_bytes) noexcept {
    return kimix::format(
        "Image is too large to process safely for region or full_resolution "
        "({} bytes; safe decode limit {} bytes). "
        "The original image was not sent to the model. Do not retry the same file unchanged. "
        "Use Bash or an available image-processing tool to create a smaller copy or crop the "
        "needed region into a separate image, then call read_image on the resulting file.",
        final_bytes, k_max_image_decode_bytes);
}

kimix::string build_full_resolution_limit_error(kimix::string_view path, int64_t final_bytes) noexcept {
    return kimix::format(
        "\"{}\" is {} bytes ({}), over the {}-byte ({}) per-image limit, "
        "so full_resolution cannot be honored. "
        "Use region to view a crop at full fidelity instead.",
        path, final_bytes, format_byte_size(final_bytes),
        k_image_byte_budget, format_byte_size(k_image_byte_budget));
}

kimix::string format_media_tag(kimix::string_view tag, kimix::span<const std::pair<kimix::string, kimix::string>> attrs) noexcept {
    if (attrs.empty()) return kimix::format("<{}>", tag);

    kimix::vector<std::pair<kimix::string, kimix::string>> sorted(attrs.begin(), attrs.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });

    kimix::vector<kimix::string> rendered;
    for (const auto &attr : sorted) {
        if (attr.second.empty()) continue; // skip falsy values
        rendered.push_back(attr.first + "=\"" + html_escape_quoted(attr.second) + "\"");
    }
    if (rendered.empty()) return kimix::format("<{}>", tag);

    kimix::string out = kimix::format("<{} ", tag);
    for (size_t i = 0; i < rendered.size(); ++i) {
        if (i != 0) out.push_back(' ');
        out += rendered[i];
    }
    out.push_back('>');
    return out;
}

kimix::string build_preview_line(kimix::string_view kind, int32_t width, int32_t height, int64_t byte_length) noexcept {
    return kimix::format("[Image: {}, {}x{}, {} bytes]\n", kind, width, height, byte_length);
}

kimix::string build_pdf_preview_line(kimix::string_view kind, int32_t width, int32_t height, int64_t byte_length) noexcept {
    return kimix::format("[PDF page image: {}, {}x{}, {} bytes]\n", kind, width, height, byte_length);
}

} // namespace kimix::builtin_tools::read_image
