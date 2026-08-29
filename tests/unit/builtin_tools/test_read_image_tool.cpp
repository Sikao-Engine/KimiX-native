// Test for the read_image built-in tool decision kernels
// (builtin_tools/read_image_tool.h/.cpp) — C++ port of the kimi-agent
// read_media / image_compress / image_format_policy Python reference. This
// test covers:
// - sniff_image_dimensions: PNG/GIF/BMP/WebP(VP8/VP8L/VP8X) synthetic
//   headers, JPEG SOFn marker walk, EXIF orientation transpose (both
//   endians, values 1-8, malformed payloads), standalone-marker skipping,
//   truncated-header nullopt cases
// - is_animated_webp: ANIM flag bit at offset 20
// - sniff_media_from_magic: every magic branch incl. ftyp brand tables and
//   EBML webm/matroska substring detection
// - detect_file_type: suffix precedence, kind-conflict -> unknown, NUL-byte
//   gate, .svg text map, non-text suffixes, default text
// - image_policy: normalize_image_mime, the accepted-MIME session-poisoning
//   gate, conversion guidance for all four OS branches, env-resolution
//   parsing rules, format_byte_size (JS round half-up + Python .1f parity),
//   parse_region_pct (valid/invalid/NaN/inf/truncation semantics)
// - compress_ladder: fit_dimensions JS-round, ladder rung order for both
//   source kinds, mipmap level policy, box_downsample_2x2 golden vectors
//   (RGB + RGBA, odd-dimension flooring)
// - payload_builder: to_data_url (0/1/2/3-byte padding vectors),
//   build_media_note all delivery branches, all three error builders,
//   format_media_tag sorting/escaping/falsy-skip, preview lines
// - ReadImage Tool wrapper: null/missing parameters, PNG header dispatch,
//   accepted-MIME gate blocking, info_only short-circuit, region_pct
//   serialization, data_b64 -> data_url

#include "ut/ut.hpp"

#include "builtin_tools/read_image_tool.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace ri = kimix::builtin_tools::read_image;
using kimix::builtin_tools::ValueElement;

namespace {

kimix::string_view sv(const std::string &s) { return kimix::string_view(s.data(), s.size()); }

// Small standard base64 encoder for feeding binary headers to ReadImage.
std::string b64_encode(const std::string &data) {
    static const char *alpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= data.size()) {
        const uint32_t v = (uint32_t(static_cast<uint8_t>(data[i])) << 16) |
                           (uint32_t(static_cast<uint8_t>(data[i + 1])) << 8) |
                           uint32_t(static_cast<uint8_t>(data[i + 2]));
        out.push_back(alpha[(v >> 18) & 63]);
        out.push_back(alpha[(v >> 12) & 63]);
        out.push_back(alpha[(v >> 6) & 63]);
        out.push_back(alpha[v & 63]);
        i += 3;
    }
    const size_t rem = data.size() - i;
    if (rem == 1) {
        const uint32_t v = uint32_t(static_cast<uint8_t>(data[i])) << 16;
        out.push_back(alpha[(v >> 18) & 63]);
        out.push_back(alpha[(v >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        const uint32_t v = (uint32_t(static_cast<uint8_t>(data[i])) << 16) |
                           (uint32_t(static_cast<uint8_t>(data[i + 1])) << 8);
        out.push_back(alpha[(v >> 18) & 63]);
        out.push_back(alpha[(v >> 12) & 63]);
        out.push_back(alpha[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

// Deserialize the JSON buffer produced by ReadImage::operator().
kimix::builtin_tools::ToolParams parse_result(const ri::ReadImage &tool) {
    kimix::builtin_tools::ToolParams result;
    const auto &buf = tool.last_result();
    kimix::span<char const> span(buf.data(), buf.size());
    result.deserialize(span);
    return result;
}

// Build a minimal synthetic PNG header: magic + IHDR with big-endian dims.
std::string make_png(int32_t w, int32_t h) {
    std::string d(33, '\0');
    const uint8_t magic[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    d.replace(0, 8, reinterpret_cast<const char *>(magic), 8);
    const auto be32 = [](int32_t v) {
        return std::string{char((v >> 24) & 0xFF), char((v >> 16) & 0xFF),
                           char((v >> 8) & 0xFF), char(v & 0xFF)};
    };
    d.replace(16, 4, be32(w));
    d.replace(20, 4, be32(h));
    return d;
}

// Synthetic GIF: GIF89a + little-endian u16 dims at 6/8.
std::string make_gif(int32_t w, int32_t h) {
    std::string d(13, '\0');
    d.replace(0, 6, "GIF89a");
    d[6] = char(w & 0xFF);
    d[7] = char((w >> 8) & 0xFF);
    d[8] = char(h & 0xFF);
    d[9] = char((h >> 8) & 0xFF);
    return d;
}

// Synthetic BMP: BM + little-endian i32 dims at 18/22.
std::string make_bmp(int32_t w, int32_t h) {
    std::string d(30, '\0');
    d[0] = 'B';
    d[1] = 'M';
    const auto le32 = [](int32_t v) {
        return std::string{char(v & 0xFF), char((v >> 8) & 0xFF),
                           char((v >> 16) & 0xFF), char((v >> 24) & 0xFF)};
    };
    d.replace(18, 4, le32(w));
    d.replace(22, 4, le32(h));
    return d;
}

// Synthetic WebP: RIFF + size + WEBP + chunk fourcc with dims per variant.
std::string make_webp_vp8(int32_t w, int32_t h) {
    std::string d(30, '\0');
    d.replace(0, 4, "RIFF");
    d.replace(8, 4, "WEBP");
    d.replace(12, 4, "VP8 ");
    d[26] = char(w & 0x3FFF & 0xFF);
    d[27] = char(((w & 0x3FFF) >> 8) & 0xFF);
    d[28] = char(h & 0x3FFF & 0xFF);
    d[29] = char(((h & 0x3FFF) >> 8) & 0xFF);
    return d;
}

std::string make_webp_vp8l(int32_t w, int32_t h) {
    std::string d(30, '\0'); // outer RIFF sniff needs >= 30 bytes (Python parity)
    d.replace(0, 4, "RIFF");
    d.replace(8, 4, "WEBP");
    d.replace(12, 4, "VP8L");
    d[20] = char(0x2F); // VP8L signature byte
    const uint32_t bits = (uint32_t(w - 1) & 0x3FFF) | ((uint32_t(h - 1) & 0x3FFF) << 14);
    d[21] = char(bits & 0xFF);
    d[22] = char((bits >> 8) & 0xFF);
    d[23] = char((bits >> 16) & 0xFF);
    d[24] = char((bits >> 24) & 0xFF);
    return d;
}

std::string make_webp_vp8x(int32_t w, int32_t h, bool animated = false) {
    std::string d(30, '\0');
    d.replace(0, 4, "RIFF");
    d.replace(8, 4, "WEBP");
    d.replace(12, 4, "VP8X");
    d[20] = animated ? char(0x02) : char(0x00);
    const auto le24 = [](int32_t v) {
        return std::string{char(v & 0xFF), char((v >> 8) & 0xFF), char((v >> 16) & 0xFF)};
    };
    d.replace(24, 3, le24(w - 1));
    d.replace(27, 3, le24(h - 1));
    return d;
}

// Synthetic JPEG: SOI, an optional APP1 EXIF segment carrying an orientation,
// and a SOF0 segment with big-endian height/width.
std::string make_jpeg(int32_t w, int32_t h, int32_t orientation = 0) {
    std::string d;
    d += std::string("\xff\xd8", 2);
    if (orientation != 0) {
        // APP1 payload: "Exif\0\0" + TIFF header (little endian) + IFD0 with
        // one entry: tag 0x0112, type SHORT(3), count 1, value.
        std::string payload;
        payload += std::string("Exif\x00\x00", 6);
        payload += "II";                          // byte order
        payload += std::string("\x2a\x00", 2);   // magic 42 LE
        payload += std::string("\x08\x00\x00\x00", 4); // IFD0 offset = 8
        payload += std::string("\x01\x00", 2);   // 1 entry
        payload += std::string("\x12\x01", 2);   // tag 0x0112 (LE)
        payload += std::string("\x03\x00", 2);   // type SHORT
        payload += std::string("\x01\x00\x00\x00", 4); // count 1
        payload += char(orientation & 0xFF);
        payload += char((orientation >> 8) & 0xFF);
        payload += std::string("\x00\x00", 2);
        const uint16_t seg_len = uint16_t(payload.size() + 2);
        d += std::string("\xff\xe1", 2);
        d += char((seg_len >> 8) & 0xFF);
        d += char(seg_len & 0xFF);
        d += payload;
    }
    // SOF0: length 8 + precision + height + width (big-endian).
    d += std::string("\xff\xc0", 2);
    d += std::string("\x00\x08", 2);
    d += char(0x08);
    d += char((h >> 8) & 0xFF);
    d += char(h & 0xFF);
    d += char((w >> 8) & 0xFF);
    d += char(w & 0xFF);
    d += '\x00'; // filler so the Python-parity loop condition (offset+9 < len) holds
    return d;
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    "sniff_png_dims"_test = [] {
        const auto data = make_png(3, 4);
        const auto dims = ri::sniff_image_dimensions(sv(data));
        expect(dims.has_value());
        expect(eq(dims->width, 3));
        expect(eq(dims->height, 4));
        expect(!dims->transposed);
        // 4000x3000 big-endian wide values
        const auto big = make_png(4000, 3000);
        const auto dims2 = ri::sniff_image_dimensions(sv(big));
        expect(dims2.has_value());
        expect(eq(dims2->width, 4000) and eq(dims2->height, 3000));
    };

    "sniff_gif_bmp_dims"_test = [] {
        const auto gif = make_gif(5, 6);
        const auto g = ri::sniff_image_dimensions(sv(gif));
        expect(g.has_value() and g->width == 5 and g->height == 6 and !g->transposed);

        const auto bmp = make_bmp(7, 8);
        const auto b = ri::sniff_image_dimensions(sv(bmp));
        expect(b.has_value() and b->width == 7 and b->height == 8);

        // BMP top-down: negative height is abs()-ed.
        const auto bmp_neg = make_bmp(7, -8);
        const auto bn = ri::sniff_image_dimensions(sv(bmp_neg));
        expect(bn.has_value() and bn->width == 7 and bn->height == 8);

        // GIF87a variant.
        std::string gif87 = make_gif(2, 3);
        gif87.replace(3, 3, "87a");
        const auto g87 = ri::sniff_image_dimensions(sv(gif87));
        expect(g87.has_value() and g87->width == 2 and g87->height == 3);
    };

    "sniff_webp_dims"_test = [] {
        const auto vp8 = make_webp_vp8(9, 10);
        const auto v = ri::sniff_image_dimensions(sv(vp8));
        expect(v.has_value() and v->width == 9 and v->height == 10 and !v->transposed);

        const auto vp8l = make_webp_vp8l(11, 12);
        const auto vl = ri::sniff_image_dimensions(sv(vp8l));
        expect(vl.has_value() and vl->width == 11 and vl->height == 12);

        // Golden vector from kimi-cli test_image_compress.py:
        // 13x17 stored minus one, little-endian 24-bit.
        const auto vp8x = make_webp_vp8x(13, 17);
        const auto vx = ri::sniff_image_dimensions(sv(vp8x));
        expect(vx.has_value() and vx->width == 13 and vx->height == 17);

        // VP8 chunk must mask the scale bits: set the 0x4000 bit in the
        // stored width; the sniffer masks it off.
        std::string masked = make_webp_vp8(9, 10);
        masked[27] = char(masked[27] | 0x40);
        const auto m = ri::sniff_image_dimensions(sv(masked));
        expect(m.has_value() and m->width == 9 and m->height == 10);
    };

    "sniff_jpeg_dims"_test = [] {
        const auto jpg = make_jpeg(60, 20);
        const auto d = ri::sniff_image_dimensions(sv(jpg));
        expect(d.has_value());
        expect(eq(d->width, 60));
        expect(eq(d->height, 20));
        expect(!d->transposed);
    };

    "sniff_jpeg_exif_transpose"_test = [] {
        // Orientations 1-4: no transpose, dims unchanged.
        for (int32_t o = 1; o <= 4; ++o) {
            const auto jpg = make_jpeg(60, 20, o);
            const auto d = ri::sniff_image_dimensions(sv(jpg));
            expect(d.has_value()) << "orientation" << o;
            expect(d->width == 60 and d->height == 20 and !d->transposed)
                << "orientation" << o;
        }
        // Orientations 5-8: swapped + transposed.
        for (int32_t o = 5; o <= 8; ++o) {
            const auto jpg = make_jpeg(60, 20, o);
            const auto d = ri::sniff_image_dimensions(sv(jpg));
            expect(d.has_value()) << "orientation" << o;
            expect(d->width == 20 and d->height == 60 and d->transposed)
                << "orientation" << o;
        }
    };

    "sniff_jpeg_standalone_markers"_test = [] {
        // SOI, standalone RST marker, then SOF0: the walk must skip RST0
        // (2 bytes, no length) and still find the SOF.
        std::string jpg;
        jpg += std::string("\xff\xd8", 2);
        jpg += std::string("\xff\xd0", 2); // RST0 standalone
        jpg += std::string("\xff\xc0", 2);
        jpg += std::string("\x00\x08", 2);
        jpg += char(0x08);
        jpg += std::string("\x00\x28", 2); // height 40
        jpg += std::string("\x00\x64", 2); // width 100
        jpg += '\x00';                      // loop-condition filler
        const auto d = ri::sniff_image_dimensions(sv(jpg));
        expect(d.has_value() and d->width == 100 and d->height == 40);

        // Non-FF filler bytes are skipped one at a time.
        std::string junk = std::string("\xff\xd8", 2);
        junk += "\x00\x00\x00";
        junk += std::string("\xff\xc2", 2); // SOF2
        junk += std::string("\x00\x08", 2);
        junk += char(0x08);
        junk += std::string("\x00\x0a", 2); // height 10
        junk += std::string("\x00\x14", 2); // width 20
        junk += '\x00';                      // loop-condition filler
        const auto d2 = ri::sniff_image_dimensions(sv(junk));
        expect(d2.has_value() and d2->width == 20 and d2->height == 10);
    };

    "sniff_truncated_and_unknown"_test = [] {
        expect(!ri::sniff_image_dimensions(std::string_view("\x89PNG\r\n\x1a\n", 8)).has_value());
        expect(!ri::sniff_image_dimensions("").has_value());
        expect(!ri::sniff_image_dimensions("not an image").has_value());
        // GIF magic but truncated before the dims.
        expect(!ri::sniff_image_dimensions("GIF89a\x01").has_value());
        // BMP magic but truncated.
        expect(!ri::sniff_image_dimensions("BMxxxx").has_value());
    };

    "exif_orientation_reader"_test = [] {
        // Big-endian (MM) TIFF variant inside a JPEG.
        std::string jpg;
        jpg += std::string("\xff\xd8", 2);
        std::string payload;
        payload += std::string("Exif\x00\x00", 6);
        payload += "MM";
        payload += std::string("\x00\x2a", 2);       // magic 42 BE
        payload += std::string("\x00\x00\x00\x08", 4); // IFD0 offset 8 BE
        payload += std::string("\x00\x01", 2);       // 1 entry
        payload += std::string("\x01\x12", 2);       // tag 0x0112 BE
        payload += std::string("\x00\x03", 2);       // SHORT
        payload += std::string("\x00\x00\x00\x01", 4); // count 1
        payload += std::string("\x00\x08", 2);       // value 8 BE
        payload += std::string("\x00\x00", 2);
        const uint16_t seg_len = uint16_t(payload.size() + 2);
        jpg += std::string("\xff\xe1", 2);
        jpg += char((seg_len >> 8) & 0xFF);
        jpg += char(seg_len & 0xFF);
        jpg += payload;
        jpg += std::string("\xff\xc0", 2);
        jpg += std::string("\x00\x08", 2);
        jpg += char(0x08);
        jpg += std::string("\x00\x14", 2); // h=20
        jpg += std::string("\x00\x3c", 2); // w=60
        jpg += '\x00';                     // loop-condition filler
        const auto d = ri::sniff_image_dimensions(sv(jpg));
        expect(d.has_value() and d->width == 20 and d->height == 60 and d->transposed)
            << "big-endian EXIF orientation 8 transposes";

        // Direct reader: malformed preamble -> nullopt.
        expect(!ri::read_exif_orientation("NOTEXIF", 0, 7).has_value());
        // Value outside 1..8 -> nullopt.
        const auto jpg9 = make_jpeg(10, 10, 9);
        const auto d9 = ri::sniff_image_dimensions(sv(jpg9));
        expect(d9.has_value() and d9->width == 10 and d9->height == 10 and !d9->transposed)
            << "orientation 9 falls back to raw SOF dims";
    };

    "animated_webp_flag"_test = [] {
        const auto animated = make_webp_vp8x(100, 100, true);
        expect(ri::is_animated_webp(sv(animated)));
        const auto still = make_webp_vp8x(100, 100, false);
        expect(!ri::is_animated_webp(sv(still)));
        // VP8 (lossy) is never animated per the kernel.
        const auto vp8 = make_webp_vp8(9, 10);
        expect(!ri::is_animated_webp(sv(vp8)));
        expect(!ri::is_animated_webp("RIFF"));
        expect(!ri::is_animated_webp(""));
    };

    "sniff_media_magic_branches"_test = [] {
        using ri::media_kind;
        const auto check = [](const std::string &data, media_kind kind, const char *mime) {
            const auto ft = ri::sniff_media_from_magic(sv(data));
            expect(ft.has_value()) << mime;
            if (!ft.has_value()) return;
            expect(ft->kind == kind) << mime;
            expect(ft->mime_type == kimix::string(mime)) << mime;
        };
        check(make_png(1, 1), media_kind::image, "image/png");
        check(std::string("\xff\xd8\xff\x00", 4), media_kind::image, "image/jpeg");
        check(make_gif(1, 1), media_kind::image, "image/gif");
        check(make_bmp(1, 1), media_kind::image, "image/bmp");
        check(std::string("II*\x00\x00", 5), media_kind::image, "image/tiff");
        check(std::string("MM\x00*\x00", 5), media_kind::image, "image/tiff");
        check(std::string("\x00\x00\x01\x00\x02\x00", 6), media_kind::image, "image/x-icon");
        check(make_webp_vp8(1, 1), media_kind::image, "image/webp");
        {
            std::string avi = "RIFF";
            avi += std::string(4, '\0');
            avi += "AVI ";
            check(avi, media_kind::video, "video/x-msvideo");
        }
        check(std::string("FLV\x01", 4), media_kind::video, "video/x-flv");
        check(std::string("\x30\x26\xb2\x75\x8e\x66\xcf\x11\xa6\xd9\x00\xaa\x00\x62\xce\x6c", 16),
              media_kind::video, "video/x-ms-wmv");
        // EBML: case-insensitive substring.
        check(std::string("\x1a\x45\xdf\xa3") + "xxxxWEBMxxxx", media_kind::video, "video/webm");
        check(std::string("\x1a\x45\xdf\xa3") + "xxMatroska", media_kind::video, "video/x-matroska");

        // ftyp brands.
        const auto ftyp = [](const char *brand) {
            std::string d(16, '\0');
            d.replace(4, 4, "ftyp");
            d.replace(8, 4, brand);
            return d;
        };
        check(ftyp("avif"), media_kind::image, "image/avif");
        check(ftyp("heic"), media_kind::image, "image/heic");
        check(ftyp("mif1"), media_kind::image, "image/heif");
        check(ftyp("heix"), media_kind::image, "image/heif");
        check(ftyp("hevc"), media_kind::image, "image/heic");
        check(ftyp("msf1"), media_kind::image, "image/heif");
        check(ftyp("isom"), media_kind::video, "video/mp4");
        check(ftyp("mp41"), media_kind::video, "video/mp4");
        check(ftyp("qt  "), media_kind::video, "video/quicktime");
        check(ftyp("3gp4"), media_kind::video, "video/3gpp");
        check(ftyp("3g2 "), media_kind::video, "video/3gpp2");
        check(ftyp("m4v "), media_kind::video, "video/x-m4v");

        // No magic -> nullopt.
        expect(!ri::sniff_media_from_magic("hello world").has_value());
        expect(!ri::sniff_media_from_magic("").has_value());
    };

    "detect_file_type_suffix_precedence"_test = [] {
        // Suffix image/video wins even without a header.
        auto ft = ri::detect_file_type("/tmp/photo.PNG", "");
        expect(ft.kind == ri::media_kind::image and ft.mime_type == kimix::string("image/png"));
        ft = ri::detect_file_type("clip.Mp4", "");
        expect(ft.kind == ri::media_kind::video and ft.mime_type == kimix::string("video/mp4"));
        // .svg routes to the text map.
        ft = ri::detect_file_type("logo.svg", "");
        expect(ft.kind == ri::media_kind::text and ft.mime_type == kimix::string("image/svg+xml"));
        // Header-sniffed media for an unmapped suffix.
        ft = ri::detect_file_type("data.binx", make_png(2, 2));
        expect(ft.kind == ri::media_kind::image and ft.mime_type == kimix::string("image/png"));
        // Kind conflict (text-map suffix vs image magic) -> unknown.
        ft = ri::detect_file_type("logo.svg", make_png(2, 2));
        expect(ft.kind == ri::media_kind::unknown and ft.mime_type.empty());
        // NUL bytes in an unmapped header -> unknown.
        ft = ri::detect_file_type("notes.txt", std::string("ab\x00" "cd", 5));
        expect(ft.kind == ri::media_kind::unknown);
        // Non-text suffix without magic -> unknown.
        ft = ri::detect_file_type("doc.pdf", "plain ascii");
        expect(ft.kind == ri::media_kind::unknown);
        // No header read (has_header=false): suffix text map, else text default.
        ft = ri::detect_file_type("data.binx", "", false);
        expect(ft.kind == ri::media_kind::text and ft.mime_type == kimix::string("text/plain"));
        ft = ri::detect_file_type("notes.pdf", "", false);
        expect(ft.kind == ri::media_kind::unknown) << "non-text suffix without header";
        // Windows separators + dotfile handling (leading dot = no suffix).
        ft = ri::detect_file_type("C:\\dir\\.hidden", std::string("a\x00", 2));
        expect(ft.kind == ri::media_kind::unknown) << "dotfile has no suffix; NUL gate applies";
        ft = ri::detect_file_type("dir\\photo.JPG", "");
        expect(ft.kind == ri::media_kind::image and ft.mime_type == kimix::string("image/jpeg"));
    };

    "normalize_mime_and_gate"_test = [] {
        expect(ri::normalize_image_mime("IMAGE/PNG") == kimix::string("image/png"));
        expect(ri::normalize_image_mime(" image/jpeg ; charset=utf-8") == kimix::string("image/jpeg"));
        expect(ri::normalize_image_mime("image/jpg") == kimix::string("image/jpeg"));
        expect(ri::normalize_image_mime("image/bmp") == kimix::string("image/bmp"));

        expect(ri::is_model_accepted_image_mime("image/png"));
        expect(ri::is_model_accepted_image_mime("IMAGE/JPEG"));
        expect(ri::is_model_accepted_image_mime("image/gif"));
        expect(ri::is_model_accepted_image_mime("image/webp"));
        expect(ri::is_model_accepted_image_mime("image/jpg")) << "alias accepted after normalize";
        expect(!ri::is_model_accepted_image_mime("image/bmp"));
        expect(!ri::is_model_accepted_image_mime("image/avif"));
        expect(!ri::is_model_accepted_image_mime("image/heic"));
        expect(!ri::is_model_accepted_image_mime("image/tiff"));
        expect(!ri::is_model_accepted_image_mime("image/x-icon"));
        expect(!ri::is_model_accepted_image_mime("text/plain"));
    };

    "conversion_guidance"_test = [] {
        const kimix::string path = "photo.avif";
        // macOS: sips.
        auto g = ri::build_image_conversion_guidance(path, "image/avif", "macOS");
        expect(g == kimix::string(
                         "\"photo.avif\" is an image/avif image, which the provider does not accept. "
                         "Convert it to JPEG first, then read the converted file. "
                         "On macOS: sips -s format jpeg \"photo.avif\" --out \"photo.jpg\""))
            << g;
        // Linux without a dedicated decoder.
        g = ri::build_image_conversion_guidance(path, "image/avif", "Linux");
        expect(g == kimix::string(
                         "\"photo.avif\" is an image/avif image, which the provider does not accept. "
                         "Convert it to JPEG first, then read the converted file. "
                         "On Linux, with ImageMagick: magick \"photo.avif\" \"photo.jpg\""))
            << g;
        // Linux with heif-convert (HEIC).
        g = ri::build_image_conversion_guidance("a/b.heic", "image/heic", "Linux");
        expect(g == kimix::string(
                         "\"a/b.heic\" is an image/heic image, which the provider does not accept. "
                         "Convert it to JPEG first, then read the converted file. "
                         "On Linux: heif-convert \"a/b.heic\" \"a/b.jpg\" (package libheif-examples), "
                         "or with ImageMagick: magick \"a/b.heic\" \"a/b.jpg\""))
            << g;
        // Windows.
        g = ri::build_image_conversion_guidance("pic.tiff", "image/tiff", "Windows");
        expect(g == kimix::string(
                         "\"pic.tiff\" is an image/tiff image, which the provider does not accept. "
                         "Convert it to JPEG first, then read the converted file. "
                         "On Windows, with ImageMagick: magick \"pic.tiff\" \"pic.jpg\" "
                         "(install it first if missing: winget install ImageMagick.ImageMagick)"))
            << g;
        // Unknown OS: options list (no decoder for bmp).
        g = ri::build_image_conversion_guidance("x.bmp", "image/bmp", "FreeBSD");
        expect(g == kimix::string(
                         "\"x.bmp\" is an image/bmp image, which the provider does not accept. "
                         "Convert it to JPEG first, then read the converted file. "
                         "Options: sips -s format jpeg \"x.bmp\" --out \"x.jpg\" (macOS), "
                         "or magick \"x.bmp\" \"x.jpg\" (ImageMagick)"))
            << g;
        // Unknown OS with a decoder (heif): heif-convert appears in options.
        g = ri::build_image_conversion_guidance("y.heif", "image/heif", "Solaris");
        expect(g.find("heif-convert \"y.heif\" \"y.jpg\" (Linux, package libheif-examples)") !=
               kimix::string::npos)
            << g;
        // Extension with multiple dots: only the trailing component is stripped.
        g = ri::build_image_conversion_guidance("archive.tar.heic", "image/heic", "macOS");
        expect(g.find("--out \"archive.tar.jpg\"") != kimix::string::npos) << g;
    };

    "env_resolution_parsing"_test = [] {
        expect(eq(ri::resolve_max_image_edge_px_from("", 2000), 2000));
        expect(eq(ri::resolve_max_image_edge_px_from("abc", 2000), 2000));
        expect(eq(ri::resolve_max_image_edge_px_from("0", 2000), 2000));
        expect(eq(ri::resolve_max_image_edge_px_from("-5", 2000), 2000));
        expect(eq(ri::resolve_max_image_edge_px_from("2.5", 2000), 2000));
        expect(eq(ri::resolve_max_image_edge_px_from("12px", 2000), 2000));
        expect(eq(ri::resolve_max_image_edge_px_from("4000", 2000), 4000));
        expect(eq(ri::resolve_max_image_edge_px_from(" 4000 ", 2000), 4000));
        expect(eq(ri::resolve_read_image_byte_budget_from("1048576", 262144), 1048576));
        expect(eq(ri::resolve_read_image_byte_budget_from("nope", 262144), 262144));
        // The live-env resolvers return the built-in defaults when the env
        // vars are unset (the normal test environment).
        expect(eq(ri::resolve_max_image_edge_px(), ri::k_max_image_edge_px));
        expect(eq(ri::resolve_read_image_byte_budget(), ri::k_read_image_byte_budget));
    };

    "format_byte_size_golden"_test = [] {
        expect(ri::format_byte_size(640) == kimix::string("640 B"));
        expect(ri::format_byte_size(1023) == kimix::string("1023 B"));
        expect(ri::format_byte_size(1024) == kimix::string("1 KB"));
        expect(ri::format_byte_size(128 * 1024) == kimix::string("128 KB"));
        // JS Math.round semantics: half rounds up.
        expect(ri::format_byte_size(1536) == kimix::string("2 KB"));
        expect(ri::format_byte_size(2560) == kimix::string("3 KB"));
        // Python float formatting parity.
        expect(ri::format_byte_size(ri::k_image_byte_budget) == kimix::string("3.8 MB"));
        expect(ri::format_byte_size(4 * 1024 * 1024) == kimix::string("4.0 MB"));
        expect(ri::format_byte_size(1024 * 1024) == kimix::string("1.0 MB"));
        // Round-half-even probe: 1.25 MB -> "1.2 MB" (Python "%.1f").
        expect(ri::format_byte_size(1310720) == kimix::string("1.2 MB"));
        // Carry case: just over 1.95 MiB rounds up to 2.0 MB.
        expect(ri::format_byte_size(2044723) == kimix::string("1.9 MB"));
        expect(ri::format_byte_size(2044724) == kimix::string("2.0 MB"));
        expect(ri::format_byte_size(2097151) == kimix::string("2.0 MB"));
        expect(ri::format_byte_size(0) == kimix::string("0 B"));
    };

    "parse_region_pct"_test = [] {
        const auto r = ri::parse_region_pct("10,10,50,50", 1000, 800);
        expect(r.has_value());
        expect(r->x == 100 and r->y == 80 and r->width == 500 and r->height == 400);

        // Truncation toward zero + max(1, ...) floor.
        const auto r2 = ri::parse_region_pct("0.1,0.1,0.1,0.1", 999, 3);
        expect(r2.has_value());
        expect(r2->x == 0 and r2->y == 0 and r2->width == 1 and r2->height == 1);

        // Bad arity.
        expect(!ri::parse_region_pct("1,2,3", 100, 100).has_value());
        expect(!ri::parse_region_pct("1,2,3,4,5", 100, 100).has_value());
        // Non-numeric.
        expect(!ri::parse_region_pct("a,b,c,d", 100, 100).has_value());
        // NaN -> invalid (Python ValueError path).
        expect(!ri::parse_region_pct("nan,0,50,50", 100, 100).has_value());
        // inf -> overflow flag (Python OverflowError path, plan §8).
        bool overflow = false;
        expect(!ri::parse_region_pct("inf,0,50,50", 100, 100, &overflow).has_value());
        expect(overflow);
        // Trailing junk after a valid float is rejected.
        expect(!ri::parse_region_pct("10x,0,50,50", 100, 100).has_value());
    };

    "fit_dimensions"_test = [] {
        // Already fits -> nullopt (Python returns the same object).
        expect(!ri::fit_dimensions(1000, 500, 2000).has_value());
        expect(!ri::fit_dimensions(2000, 2000, 2000).has_value());
        // JS-round: 2001x1000 @2000 -> 2000x1000 (1000*2000/2001+0.5 = 1000.0...).
        const auto f = ri::fit_dimensions(2001, 1000, 2000);
        expect(f.has_value() and f->first == 2000 and f->second == 1000)
            << f->first << "x" << f->second;
        // Half-up rounding: 3x1 @2 -> factor 2/3: w=2, h=max(1,floor(0.666+0.5))=1.
        const auto f2 = ri::fit_dimensions(3, 1, 2);
        expect(f2.has_value() and f2->first == 2 and f2->second == 1);
        // Never enlarges: small image under the edge stays untouched.
        expect(!ri::fit_dimensions(10, 10, 256).has_value());
        // Round-half-up at exactly .5: 1500x1000 @1000 -> 1000x667.
        const auto f3 = ri::fit_dimensions(1500, 1000, 1000);
        expect(f3.has_value() and f3->first == 1000 and f3->second == 667)
            << f3->first << "x" << f3->second;
    };

    "ladder_plan_png_first"_test = [] {
        // 4000x2000: fitted already (edge irrelevant to the plan),
        // PNG-first order with rescale rungs + JPEG ladders.
        const auto rungs = ri::build_ladder_plan(true, 4000, 2000, 262144);
        std::vector<std::string> order;
        for (const auto &r : rungs) {
            order.push_back(std::string(r.format == ri::ladder_rung::encode_format::png ? "png" : "jpeg") +
                            "@" + std::to_string(r.edge) + "q" + std::to_string(r.quality));
        }
        expect(order.size() >= size_t(4));
        expect(order[0] == std::string("png@0q0"));
        expect(order[1] == std::string("png@2000q0"));
        expect(order[2] == std::string("png@1000q0"));
        expect(order[3] == std::string("jpeg@0q80"));
        expect(order[4] == std::string("jpeg@0q60"));
        expect(order[5] == std::string("jpeg@0q40"));
        expect(order[6] == std::string("jpeg@0q20"));
        expect(order[7] == std::string("jpeg@768q80"));
        // Small image: no PNG rescale rungs above the floor, but the
        // sub-floor edge 256 still rescales 300x200 -> extra JPEG ladder.
        const auto small_plan = ri::build_ladder_plan(true, 300, 200, 262144);
        expect(small_plan.size() == size_t(9)) << small_plan.size();
        expect(small_plan[0].format == ri::ladder_rung::encode_format::png and small_plan[0].edge == 0);
        expect(small_plan[1].format == ri::ladder_rung::encode_format::jpeg and small_plan[1].quality == 80 and small_plan[1].edge == 0);
        expect(small_plan[5].format == ri::ladder_rung::encode_format::jpeg and small_plan[5].edge == 256);
    };

    "ladder_plan_jpeg_first"_test = [] {
        const auto rungs = ri::build_ladder_plan(false, 4000, 2000, 262144);
        expect(rungs.size() >= size_t(8));
        expect(rungs[0].format == ri::ladder_rung::encode_format::jpeg and rungs[0].edge == 0 and rungs[0].quality == 80);
        expect(rungs[1].quality == 60);
        expect(rungs[2].quality == 40);
        expect(rungs[3].quality == 20);
        // Fallback rescales: 4000x2000 -> edges 2000,1000,768,512,384,256.
        expect(rungs[4].edge == 2000 and rungs[4].quality == 80);
        expect(rungs[8].edge == 1000 and rungs[8].quality == 80);
        expect(rungs[12].edge == 768);
        expect(rungs[16].edge == 512);
        expect(rungs[20].edge == 384);
        expect(rungs[24].edge == 256);
        expect(rungs.size() == size_t(28)) << rungs.size();
        // No PNG rung on the JPEG-first path.
        for (const auto &r : rungs) {
            expect(r.format == ri::ladder_rung::encode_format::jpeg);
        }
    };

    "mipmap_levels"_test = [] {
        // 8x8 pyramid stops at 2x2.
        const auto levels = ri::mipmap_level_dims(8, 8);
        expect(levels.size() == size_t(3));
        expect(levels[0].first == 8 and levels[0].second == 8);
        expect(levels[1].first == 4 and levels[1].second == 4);
        expect(levels[2].first == 2 and levels[2].second == 2);
        // Odd dims: 5x4 -> next level 2x2 (floor), then stop.
        const auto odd = ri::mipmap_level_dims(5, 4);
        expect(odd.size() == size_t(2));
        expect(odd[1].first == 2 and odd[1].second == 2);
        // Tiny input: single level only.
        expect(ri::mipmap_level_dims(2, 2).size() == size_t(1));
        expect(ri::mipmap_level_dims(3, 3).size() == size_t(1))
            << "next level 1x1 is below MIN_MIPMAP_EDGE_PX";

        // Level selection. Explicit span element type: CTAD from (ptr, count)
        // trips a GCC 13 span deduction-guide bug (extent SIZE_MAX).
        const auto levels_span =
            kimix::span<const std::pair<int32_t, int32_t>>(levels.data(), levels.size());
        const auto sel = ri::first_mipmap_level_for_edge(levels_span, 4);
        expect(sel.has_value() and *sel == size_t(1));
        const auto sel2 = ri::first_mipmap_level_for_edge(levels_span, 100);
        expect(sel2.has_value() and *sel2 == size_t(0));
        // Edge cap below every level -> smallest (last) level.
        const auto sel3 = ri::first_mipmap_level_for_edge(levels_span, 1);
        expect(sel3.has_value() and *sel3 == size_t(2));
    };

    "box_downsample_golden"_test = [] {
        // 2x2 RGB -> 1x1: per-channel integer average (truncating).
        {
            const uint8_t src[12] = {
                10, 20, 30, 40, 50, 60,
                70, 80, 90, 100, 110, 120};
            uint8_t dst[3] = {0};
            ri::box_downsample_2x2(src, 2, 2, 3, dst);
            expect(dst[0] == 55 and dst[1] == 65 and dst[2] == 75)
                << int(dst[0]) << int(dst[1]) << int(dst[2]);
        }
        // 4x4 RGBA -> 2x2 with truncation: (1+2+2+3)/4 = 2.0; (1+2+2+2)/4=1.75->1.
        {
            std::vector<uint8_t> src(4 * 4 * 4, 0);
            auto px = [&src](int x, int y, uint8_t v) {
                src[(y * 4 + x) * 4 + 0] = v;
                src[(y * 4 + x) * 4 + 3] = 255;
            };
            px(0, 0, 1); px(1, 0, 2); px(0, 1, 2); px(1, 1, 3);
            std::vector<uint8_t> dst(2 * 2 * 4, 0);
            ri::box_downsample_2x2(src.data(), 4, 4, 4, dst.data());
            expect(dst[0] == 2) << "rounds down: 8/4=2";
            expect(dst[3] == 255);
        }
        // Odd dimensions floor: 5x4 -> 2x2 output (right column dropped).
        {
            std::vector<uint8_t> src(5 * 4 * 3, 0);
            for (size_t i = 0; i < src.size(); ++i) src[i] = uint8_t(i % 256);
            std::vector<uint8_t> dst(2 * 2 * 3, 0);
            ri::box_downsample_2x2(src.data(), 5, 4, 3, dst.data());
            // top-left block covers src pixels (0,0),(1,0),(0,1),(1,1).
            const auto avg = [&](int x, int y, int c) {
                const int stride = 5 * 3;
                const uint32_t s = src[y * stride + x * 3 + c] + src[y * stride + (x + 1) * 3 + c] +
                                   src[(y + 1) * stride + x * 3 + c] + src[(y + 1) * stride + (x + 1) * 3 + c];
                return uint8_t(s / 4);
            };
            expect(dst[0] == avg(0, 0, 0));
            expect(dst[1] == avg(0, 0, 1));
            expect(dst[2] == avg(0, 0, 2));
            expect(dst[3] == avg(2, 0, 0));
            expect(dst[6] == avg(0, 2, 0));
            expect(dst[9] == avg(2, 2, 0));
        }
        // 3x3 RGB -> 1x1 (odd floor on both axes).
        {
            std::vector<uint8_t> src(3 * 3 * 3, 9);
            uint8_t dst[3] = {0};
            ri::box_downsample_2x2(src.data(), 3, 3, 3, dst);
            expect(dst[0] == 9 and dst[1] == 9 and dst[2] == 9);
        }
        // Truncation parity with numpy float-mean: 7/4 = 1.75 -> 1.
        {
            const uint8_t src[12] = {1, 0, 0, 2, 0, 0, 2, 0, 0, 2, 0, 0};
            uint8_t dst[3] = {9, 9, 9};
            ri::box_downsample_2x2(src, 2, 2, 3, dst);
            expect(dst[0] == 1) << "7/4 truncates to 1";
        }
    };

    "to_data_url_golden"_test = [] {
        expect(ri::to_data_url("image/png", {}) == kimix::string("data:image/png;base64,"));
        // Note: explicit span element type — `kimix::span(arr, n)` CTAD from a
        // raw array + count trips a GCC 13 deduction-guide bug (extent SIZE_MAX).
        const uint8_t one[1] = {'f'};
        expect(ri::to_data_url("image/png", kimix::span<const uint8_t>(one, 1)) == kimix::string("data:image/png;base64,Zg=="));
        const uint8_t two[2] = {'f', 'o'};
        expect(ri::to_data_url("image/jpeg", kimix::span<const uint8_t>(two, 2)) == kimix::string("data:image/jpeg;base64,Zm8="));
        const uint8_t three[3] = {'f', 'o', 'o'};
        expect(ri::to_data_url("image/gif", kimix::span<const uint8_t>(three, 3)) == kimix::string("data:image/gif;base64,Zm9v"));
        const uint8_t four[4] = {'f', 'o', 'o', 'b'};
        expect(ri::to_data_url("image/webp", kimix::span<const uint8_t>(four, 4)) == kimix::string("data:image/webp;base64,Zm9vYg=="));
        // Full alphabet probe.
        const uint8_t bytes[6] = {0xFB, 0xEF, 0xFF, 0x00, 0x10, 0x83};
        expect(ri::to_data_url("application/octet-stream", kimix::span<const uint8_t>(bytes, 6)) ==
               kimix::string("data:application/octet-stream;base64,++//ABCD"))
            << ri::to_data_url("application/octet-stream", kimix::span<const uint8_t>(bytes, 6));    };

    "media_note_branches"_test = [] {
        const ri::image_dimensions dims{800, 600, false};

        // Untouched delivery.
        {
            ri::delivery_info d;
            d.kind = ri::delivery_info::delivery_kind::untouched;
            const auto note = ri::build_media_note(ri::media_kind::image, "image/png", 1234, dims, d);
            expect(note == kimix::string(
                               "<system>Read image file. Mime type: image/png. Size: 1234 bytes. "
                               "Original dimensions: 800x600 pixels. "
                               "If you need to output coordinates, output relative coordinates first "
                               "and compute absolute coordinates using the original image size. "
                               "If you generate or edit images or videos via commands or scripts, "
                               "read the result back immediately before continuing.</system>"))
                << note;
        }
        // Downsampled delivery with mipmap warning.
        {
            ri::delivery_info d;
            d.kind = ri::delivery_info::delivery_kind::downsampled;
            d.width = 1000;
            d.height = 750;
            d.byte_length = 131072;
            d.mime_type = "image/jpeg";
            d.mipmap = true;
            const auto note = ri::build_media_note(ri::media_kind::image, "image/png", 5000000, dims, d);
            expect(note == kimix::string(
                               "<system>Read image file. Mime type: image/png. Size: 5000000 bytes. "
                               "Original dimensions: 800x600 pixels. "
                               "The attached image was downsampled to 1000x750 pixels (image/jpeg, 128 KB) "
                               "to fit model limits; fine detail may be lost. "
                               "To inspect fine detail, call read_image again with the region parameter "
                               "(original-image pixel coordinates) to view a crop at full fidelity. "
                               "Warning: Mip-map downsampling (2x2 bilinear averaging) was used "
                               "because standard compression could not meet the delivery limits; "
                               "fine detail may be significantly reduced. "
                               "If you need to output coordinates, output relative coordinates first "
                               "and compute absolute coordinates using the original image size. "
                               "If you generate or edit images or videos via commands or scripts, "
                               "read the result back immediately before continuing.</system>"))
                << note;
        }
        // Crop delivery at native resolution (no coordinate-guidance part).
        {
            ri::delivery_info d;
            d.kind = ri::delivery_info::delivery_kind::crop;
            d.width = 100;
            d.height = 50;
            d.byte_length = 4096;
            d.mime_type = "image/png";
            d.region = ri::crop_region{10, 20, 100, 50};
            d.resized = false;
            const auto note = ri::build_media_note(ri::media_kind::image, "image/png", 999, dims, d);
            expect(note == kimix::string(
                               "<system>Read image file. Mime type: image/png. Size: 999 bytes. "
                               "Original dimensions: 800x600 pixels. "
                               "Showing region (x=10, y=20, width=100, height=50) of the original image at native resolution. "
                               "To output coordinates in original-image pixels, locate them within this "
                               "crop and add the region offset (x=10, y=20). "
                               "If you generate or edit images or videos via commands or scripts, "
                               "read the result back immediately before continuing.</system>"))
                << note;
        }
        // Crop delivery resized.
        {
            ri::delivery_info d;
            d.kind = ri::delivery_info::delivery_kind::crop;
            d.width = 500;
            d.height = 250;
            d.byte_length = 2048;
            d.mime_type = "image/jpeg";
            d.region = ri::crop_region{0, 0, 1000, 500};
            d.resized = true;
            const auto note = ri::build_media_note(ri::media_kind::image, "image/jpeg", 4096, dims, d);
            expect(note.find("Showing region (x=0, y=0, width=1000, height=500) of the original image, "
                             "downsampled to 500x250 pixels.") != kimix::string::npos)
                << note;
        }
        // Full resolution delivery.
        {
            ri::delivery_info d;
            d.kind = ri::delivery_info::delivery_kind::full;
            const auto note = ri::build_media_note(ri::media_kind::image, "image/webp", 100, dims, d);
            expect(note.find("Shown at native resolution; no downscaling applied.") != kimix::string::npos) << note;
        }
        // Video note: no dimensions, no coordinate guidance.
        {
            const auto note = ri::build_media_note(ri::media_kind::video, "video/mp4", 42, std::nullopt, std::nullopt);
            expect(note == kimix::string(
                               "<system>Read video file. Mime type: video/mp4. Size: 42 bytes. "
                               "If you generate or edit images or videos via commands or scripts, "
                               "read the result back immediately before continuing.</system>"))
                << note;
        }
    };

    "error_builders"_test = [] {
        expect(ri::build_image_delivery_limit_error(300000, 262144, 2000) ==
               kimix::string(
                   "Image is too large to send safely after compression (300000 bytes; "
                   "limit 262144 bytes and 2000px on the longest edge). "
                   "The original image was not sent to the model. Do not retry the same file unchanged. "
                   "Use Bash or an available image-processing tool to create a smaller copy within both "
                   "limits, then call read_image on the smaller copy."));
        expect(ri::build_image_decode_limit_error(70000000) ==
               kimix::string(
                   "Image is too large to process safely for region or full_resolution "
                   "(70000000 bytes; safe decode limit 67108864 bytes). "
                   "The original image was not sent to the model. Do not retry the same file unchanged. "
                   "Use Bash or an available image-processing tool to create a smaller copy or crop the "
                   "needed region into a separate image, then call read_image on the resulting file."));
        expect(ri::build_full_resolution_limit_error("big.png", 5000000) ==
               kimix::string(
                   "\"big.png\" is 5000000 bytes (4.8 MB), over the 3932160-byte (3.8 MB) "
                   "per-image limit, so full_resolution cannot be honored. "
                   "Use region to view a crop at full fidelity instead."))
            << ri::build_full_resolution_limit_error("big.png", 5000000);
    };

    "format_media_tag"_test = [] {
        // No attrs -> bare tag.
        expect(ri::format_media_tag("image", {}) == kimix::string("<image>"));
        // Sorted keys + escaping.
        {
            const kimix::vector<std::pair<kimix::string, kimix::string>> attrs = {
                {"path", "a&b\"c'<d>"},
                {"alt", "x"},
            };
            // Explicit span element type: CTAD from (ptr, count) trips a GCC 13
            // span deduction-guide bug (extent SIZE_MAX) for this element type.
            const auto attrs_span =
                kimix::span<const std::pair<kimix::string, kimix::string>>(attrs.data(), attrs.size());
            expect(ri::format_media_tag("image", attrs_span) ==
                   kimix::string("<image alt=\"x\" path=\"a&amp;b&quot;c&#x27;&lt;d&gt;\">"))
                << ri::format_media_tag("image", attrs_span);
        }
        // Falsy (empty) values are skipped; all skipped -> bare tag.
        {
            const kimix::vector<std::pair<kimix::string, kimix::string>> attrs = {
                {"path", ""},
            };
            const auto attrs_span =
                kimix::span<const std::pair<kimix::string, kimix::string>>(attrs.data(), attrs.size());
            expect(ri::format_media_tag("video", attrs_span) ==
                   kimix::string("<video>"));
        }
    };

    "preview_lines"_test = [] {
        expect(ri::build_preview_line("downsampled", 1000, 563, 123456) ==
               kimix::string("[Image: downsampled, 1000x563, 123456 bytes]\n"));
        expect(ri::build_pdf_preview_line("downsampled", 1240, 1754, 200000) ==
               kimix::string("[PDF page image: downsampled, 1240x1754, 200000 bytes]\n"));
    };

    "constants"_test = [] {
        expect(eq(ri::k_image_byte_budget, int64_t(3932160)));
        expect(eq(ri::k_max_image_edge_px, int32_t(2000)));
        expect(eq(ri::k_read_image_byte_budget, int64_t(262144)));
        expect(eq(ri::k_max_decode_pixels, int64_t(100000000)));
        expect(eq(ri::k_max_image_decode_bytes, int64_t(64 * 1024 * 1024)));
        expect(eq(ri::k_max_mipmap_decode_bytes, int64_t(128 * 1024 * 1024)));
        expect(eq(ri::k_min_mipmap_edge_px, int32_t(2)));
        expect(eq(ri::k_png_rescale_floor_px, int32_t(1000)));
        expect(eq(ri::k_media_sniff_bytes, size_t(512)));
        expect(eq(ri::k_max_media_megabytes, int32_t(100)));
        expect(ri::k_pdf_dpi_sequence[0] == 150 and ri::k_pdf_dpi_sequence[1] == 96 and ri::k_pdf_dpi_sequence[2] == 72);
        expect(ri::k_jpeg_quality_steps[0] == 80 and ri::k_jpeg_quality_steps[3] == 20);
        expect(ri::k_fallback_edges_px[0] == 2000 and ri::k_fallback_edges_px[5] == 256);
    };

    // ------------------------------------------------------------------
    // ReadImage Tool class wrapper
    // ------------------------------------------------------------------

    "read_image_null_params"_test = [] {
        ri::ReadImage tool(nullptr);
        tool(nullptr);
        const auto result = parse_result(tool);
        const auto *ok = result.get("ok");
        expect(ok != nullptr && ok->is_bool() && !ok->as_bool());
        const auto *status = result.get("status");
        expect(status != nullptr && status->is_string() && status->as_string() == kimix::string("invalid_input"));
    };

    "read_image_missing_path"_test = [] {
        ri::ReadImage tool(nullptr);
        kimix::builtin_tools::ToolParams params;
        tool(&params);
        const auto result = parse_result(tool);
        const auto *ok = result.get("ok");
        expect(ok != nullptr && ok->is_bool() && !ok->as_bool());
        const auto *status = result.get("status");
        expect(status != nullptr && status->as_string() == kimix::string("invalid_input"));
    };

    "read_image_png_header"_test = [] {
        const auto header = make_png(80, 60);
        kimix::builtin_tools::ToolParams params;
        params.values["path"] = ValueElement::make_string(kimix::string("photo.png"));
        params.values["header_b64"] = ValueElement::make_string(kimix::string(b64_encode(header)));
        params.values["file_size"] = ValueElement::make_int(1234);

        ri::ReadImage tool(nullptr);
        tool(&params);
        const auto result = parse_result(tool);

        const auto *ok = result.get("ok");
        expect(ok != nullptr && ok->is_bool() && ok->as_bool()) << "ok";
        const auto *kind = result.get("kind");
        expect(kind != nullptr && kind->is_string() && kind->as_string() == kimix::string("image"));
        const auto *mime = result.get("mime_type");
        expect(mime != nullptr && mime->as_string() == kimix::string("image/png"));
        const auto *accepted = result.get("accepted");
        expect(accepted != nullptr && accepted->is_bool() && accepted->as_bool());
        const auto *width = result.get("width");
        expect(width != nullptr && width->is_int() && width->as_int() == 80);
        const auto *height = result.get("height");
        expect(height != nullptr && height->is_int() && height->as_int() == 60);
        const auto *ladder = result.get("ladder");
        expect(ladder != nullptr && ladder->is_array() && !ladder->as_array().empty());
        const auto *mip = result.get("mipmap_levels");
        expect(mip != nullptr && mip->is_array() && !mip->as_array().empty());
        const auto *note = result.get("media_note");
        expect(note != nullptr && note->is_string());
        const auto *preview = result.get("preview_line");
        expect(preview != nullptr && preview->is_string());
    };

    "read_image_unsupported_format"_test = [] {
        kimix::builtin_tools::ToolParams params;
        params.values["path"] = ValueElement::make_string(kimix::string("photo.heic"));
        params.values["header_b64"] = ValueElement::make_string(kimix::string(""));

        ri::ReadImage tool(nullptr);
        tool(&params);
        const auto result = parse_result(tool);

        const auto *ok = result.get("ok");
        expect(ok != nullptr && ok->is_bool() && !ok->as_bool());
        const auto *status = result.get("status");
        expect(status != nullptr && status->as_string() == kimix::string("blocked"));
        const auto *accepted = result.get("accepted");
        expect(accepted != nullptr && accepted->is_bool() && !accepted->as_bool());
        const auto *guidance = result.get("conversion_guidance");
        expect(guidance != nullptr && guidance->is_string());
    };

    "read_image_info_only"_test = [] {
        const auto header = make_jpeg(100, 80);
        kimix::builtin_tools::ToolParams params;
        params.values["path"] = ValueElement::make_string(kimix::string("pic.jpg"));
        params.values["header_b64"] = ValueElement::make_string(kimix::string(b64_encode(header)));
        params.values["info_only"] = ValueElement::make_bool(true);

        ri::ReadImage tool(nullptr);
        tool(&params);
        const auto result = parse_result(tool);

        const auto *ok = result.get("ok");
        expect(ok != nullptr && ok->as_bool());
        const auto *width = result.get("width");
        expect(width != nullptr && width->as_int() == 100);
        // Ladder should not be planned in info_only mode.
        const auto *ladder = result.get("ladder");
        expect(ladder == nullptr);
    };

    "read_image_region_pct"_test = [] {
        const auto header = make_png(1000, 800);
        kimix::builtin_tools::ToolParams params;
        params.values["path"] = ValueElement::make_string(kimix::string("photo.png"));
        params.values["header_b64"] = ValueElement::make_string(kimix::string(b64_encode(header)));
        params.values["region_pct"] = ValueElement::make_string(kimix::string("10,10,50,50"));

        ri::ReadImage tool(nullptr);
        tool(&params);
        const auto result = parse_result(tool);

        const auto *region = result.get("region");
        expect(region != nullptr && region->is_object());
        const auto *region_obj = region->as_object();
        expect(region_obj != nullptr);
        const auto *x = region_obj->get("x");
        const auto *y = region_obj->get("y");
        const auto *w = region_obj->get("width");
        const auto *h = region_obj->get("height");
        expect(x != nullptr && x->is_int() && x->as_int() == 100);
        expect(y != nullptr && y->is_int() && y->as_int() == 80);
        expect(w != nullptr && w->is_int() && w->as_int() == 500);
        expect(h != nullptr && h->is_int() && h->as_int() == 400);
    };

    "read_image_data_url"_test = [] {
        const std::string payload = "foob";
        kimix::builtin_tools::ToolParams params;
        params.values["path"] = ValueElement::make_string(kimix::string("dot.png"));
        params.values["mime_type"] = ValueElement::make_string(kimix::string("image/png"));
        params.values["data_b64"] = ValueElement::make_string(kimix::string(b64_encode(payload)));

        ri::ReadImage tool(nullptr);
        tool(&params);
        const auto result = parse_result(tool);

        const auto *data_url = result.get("data_url");
        expect(data_url != nullptr && data_url->is_string());
        expect(data_url->as_string() == kimix::string("data:image/png;base64,Zm9vYg=="));
    };
}
