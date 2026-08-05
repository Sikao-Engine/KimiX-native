// Test for src/runtime/image/image_sniff.h (image header sniffing kernels).
// This test covers:
// - PNG, GIF, BMP dimension sniffing
// - WebP VP8 / VP8L / VP8X (animated and non-animated) sniffing + animation flag
// - JPEG baseline / progressive sniffing
// - JPEG EXIF orientations 1-8 (transpose behavior on 5-8)
// - Truncated / malformed headers returning empty results
// - format_byte_size boundaries

#include "ut/ut.hpp"
#include <runtime/image/image_sniff.h>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::image;

namespace {

// Append a big-endian uint16/uint32 into a byte buffer.
void push_u16_be(std::vector<char>& b, uint16_t v) {
    b.push_back(static_cast<char>((v >> 8) & 0xFF));
    b.push_back(static_cast<char>(v & 0xFF));
}

void push_u32_be(std::vector<char>& b, uint32_t v) {
    b.push_back(static_cast<char>((v >> 24) & 0xFF));
    b.push_back(static_cast<char>((v >> 16) & 0xFF));
    b.push_back(static_cast<char>((v >> 8) & 0xFF));
    b.push_back(static_cast<char>(v & 0xFF));
}

void push_u16_le(std::vector<char>& b, uint16_t v) {
    b.push_back(static_cast<char>(v & 0xFF));
    b.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void push_u32_le(std::vector<char>& b, uint32_t v) {
    b.push_back(static_cast<char>(v & 0xFF));
    b.push_back(static_cast<char>((v >> 8) & 0xFF));
    b.push_back(static_cast<char>((v >> 16) & 0xFF));
    b.push_back(static_cast<char>((v >> 24) & 0xFF));
}

void push_i32_le(std::vector<char>& b, int32_t v) {
    push_u32_le(b, static_cast<uint32_t>(v));
}

kimix::string_view view(const std::vector<char>& b) {
    return kimix::string_view(b.data(), b.size());
}

// PNG IHDR with arbitrary width/height.
std::vector<char> make_png(uint32_t width, uint32_t height) {
    std::vector<char> b;
    b.insert(b.end(), {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'});
    push_u32_be(b, 13);          // IHDR length
    b.insert(b.end(), {'I', 'H', 'D', 'R'});
    push_u32_be(b, width);
    push_u32_be(b, height);
    b.insert(b.end(), {8, 2, 0, 0, 0}); // bit depth, RGB, compression, filter, interlace
    push_u32_be(b, 0);           // CRC (ignored)
    return b;
}

// GIF89a logical screen.
std::vector<char> make_gif(uint16_t width, uint16_t height) {
    std::vector<char> b;
    b.insert(b.end(), {'G', 'I', 'F', '8', '9', 'a'});
    push_u16_le(b, width);
    push_u16_le(b, height);
    b.insert(b.end(), {0, 0, 0}); // packed field, bg index, aspect ratio
    return b;
}

// BITMAPINFOHEADER BMP.
std::vector<char> make_bmp(int32_t width, int32_t height) {
    std::vector<char> b;
    b.insert(b.end(), {'B', 'M'});
    push_u32_le(b, 0); // file size (ignored)
    push_u16_le(b, 0); // reserved
    push_u16_le(b, 0); // reserved
    push_u32_le(b, 0); // pixel offset (ignored)
    push_u32_le(b, 40); // DIB header size
    push_i32_le(b, width);
    push_i32_le(b, height);
    return b;
}

// WebP VP8 chunk. width/height use the low 14 bits of the 16-bit fields.
std::vector<char> make_webp_vp8(uint16_t width, uint16_t height) {
    std::vector<char> b;
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    push_u32_le(b, 0);
    b.insert(b.end(), {'W', 'E', 'B', 'P'});
    b.insert(b.end(), {'V', 'P', '8', ' '});
    push_u32_le(b, 0);
    b.insert(b.end(), 6, '\0'); // frame tag / start-code prefix
    push_u16_le(b, width & 0x3FFF);
    push_u16_le(b, height & 0x3FFF);
    return b;
}

// WebP VP8L chunk. The reference parser requires len >= 30 for any RIFF/WebP
// header and len >= 25 for VP8L, so pad to 30 bytes.
std::vector<char> make_webp_vp8l(uint16_t width, uint16_t height) {
    std::vector<char> b;
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    push_u32_le(b, 0);
    b.insert(b.end(), {'W', 'E', 'B', 'P'});
    b.insert(b.end(), {'V', 'P', '8', 'L'});
    push_u32_le(b, 0);
    b.push_back('\x2f'); // VP8L signature
    const uint32_t w = static_cast<uint32_t>(width - 1);
    const uint32_t h = static_cast<uint32_t>(height - 1);
    const uint32_t bits = w | (h << 14);
    push_u32_le(b, bits);
    if (b.size() < 30) {
        b.insert(b.end(), 30 - b.size(), '\0');
    }
    return b;
}

// WebP VP8X chunk.
std::vector<char> make_webp_vp8x(uint32_t width, uint32_t height, uint8_t flags) {
    std::vector<char> b;
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    push_u32_le(b, 0);
    b.insert(b.end(), {'W', 'E', 'B', 'P'});
    b.insert(b.end(), {'V', 'P', '8', 'X'});
    push_u32_le(b, 10); // chunk size
    b.push_back(static_cast<char>(flags));
    b.insert(b.end(), 3, '\0'); // reserved
    b.push_back(static_cast<char>((width - 1) & 0xFF));
    b.push_back(static_cast<char>(((width - 1) >> 8) & 0xFF));
    b.push_back(static_cast<char>(((width - 1) >> 16) & 0xFF));
    b.push_back(static_cast<char>((height - 1) & 0xFF));
    b.push_back(static_cast<char>(((height - 1) >> 8) & 0xFF));
    b.push_back(static_cast<char>(((height - 1) >> 16) & 0xFF));
    return b;
}

// Minimal JPEG SOF0 baseline segment. APP0 is omitted for brevity.
std::vector<char> make_jpeg_sof(uint8_t sof_marker, uint16_t width, uint16_t height) {
    std::vector<char> b;
    b.insert(b.end(), {'\xff', '\xd8'});
    b.insert(b.end(), {'\xff', static_cast<char>(sof_marker)});
    push_u16_be(b, 11); // segment length
    b.push_back('\x08'); // precision
    push_u16_be(b, height);
    push_u16_be(b, width);
    b.push_back('\x03'); // components
    b.insert(b.end(), {1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1}); // component descriptors
    return b;
}

void push_u16(std::vector<char>& b, uint16_t v, bool little_endian) {
    if (little_endian) {
        b.push_back(static_cast<char>(v & 0xFF));
        b.push_back(static_cast<char>((v >> 8) & 0xFF));
    } else {
        b.push_back(static_cast<char>((v >> 8) & 0xFF));
        b.push_back(static_cast<char>(v & 0xFF));
    }
}

void push_u32(std::vector<char>& b, uint32_t v, bool little_endian) {
    if (little_endian) {
        b.push_back(static_cast<char>(v & 0xFF));
        b.push_back(static_cast<char>((v >> 8) & 0xFF));
        b.push_back(static_cast<char>((v >> 16) & 0xFF));
        b.push_back(static_cast<char>((v >> 24) & 0xFF));
    } else {
        b.push_back(static_cast<char>((v >> 24) & 0xFF));
        b.push_back(static_cast<char>((v >> 16) & 0xFF));
        b.push_back(static_cast<char>((v >> 8) & 0xFF));
        b.push_back(static_cast<char>(v & 0xFF));
    }
}

// Minimal EXIF APP1 segment with a single Orientation tag.
std::vector<char> make_exif_app1(int orientation, bool little_endian) {
    std::vector<char> payload;
    payload.insert(payload.end(), {'E', 'x', 'i', 'f', '\0', '\0'});

    if (little_endian) {
        payload.insert(payload.end(), {'I', 'I'});
    } else {
        payload.insert(payload.end(), {'M', 'M'});
    }
    push_u16(payload, 42, little_endian);
    push_u32(payload, 8, little_endian); // IFD offset from TIFF start

    push_u16(payload, 1, little_endian); // entry count

    // Tag 0x0112, type SHORT (3), count 1, value = orientation.
    push_u16(payload, 0x0112, little_endian);
    push_u16(payload, 3, little_endian);
    push_u32(payload, 1, little_endian);
    push_u16(payload, orientation & 0xFFFF, little_endian);
    push_u16(payload, 0, little_endian); // padding

    push_u32(payload, 0, little_endian); // next IFD pointer

    std::vector<char> b;
    b.insert(b.end(), {'\xff', '\xe1'});
    push_u16_be(b, static_cast<uint16_t>(2 + payload.size()));
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    auto expect_dims = [](const kimix::optional<dimensions>& dims, int w, int h,
                          bool transposed) {
        expect(dims.has_value());
        if (dims) {
            expect(eq(dims->width, w));
            expect(eq(dims->height, h));
            expect(eq(dims->transposed, transposed));
        }
    };

    "png_dimensions"_test = [&] {
        const auto header = make_png(1920, 1080);
        expect_dims(sniff_dimensions(view(header)), 1920, 1080, false);
    };

    "gif_dimensions"_test = [&] {
        const auto header = make_gif(640, 480);
        expect_dims(sniff_dimensions(view(header)), 640, 480, false);
    };

    "bmp_dimensions"_test = [&] {
        const auto header = make_bmp(800, 600);
        expect_dims(sniff_dimensions(view(header)), 800, 600, false);
    };

    "bmp_top_down_negative_height"_test = [&] {
        const auto header = make_bmp(800, -600);
        expect_dims(sniff_dimensions(view(header)), 800, 600, false);
    };

    "webp_vp8_dimensions"_test = [&] {
        const auto header = make_webp_vp8(1280, 720);
        expect_dims(sniff_dimensions(view(header)), 1280, 720, false);
        expect(!is_animated_webp(view(header)));
    };

    "webp_vp8l_dimensions"_test = [&] {
        const auto header = make_webp_vp8l(512, 512);
        expect_dims(sniff_dimensions(view(header)), 512, 512, false);
        expect(!is_animated_webp(view(header)));
    };

    "webp_vp8x_dimensions"_test = [&] {
        const auto header = make_webp_vp8x(2048, 1536, 0);
        expect_dims(sniff_dimensions(view(header)), 2048, 1536, false);
        expect(!is_animated_webp(view(header)));
    };

    "webp_vp8x_animated"_test = [&] {
        const auto header = make_webp_vp8x(256, 256, 0x02);
        expect(is_animated_webp(view(header)));
        expect_dims(sniff_dimensions(view(header)), 256, 256, false);
    };

    "jpeg_baseline_sof0"_test = [&] {
        const auto header = make_jpeg_sof(0xC0, 1920, 1080);
        expect_dims(sniff_dimensions(view(header)), 1920, 1080, false);
    };

    "jpeg_progressive_sof2"_test = [&] {
        const auto header = make_jpeg_sof(0xC2, 800, 600);
        expect_dims(sniff_dimensions(view(header)), 800, 600, false);
    };

    "jpeg_exif_orientations"_test = [&] {
        for (int orient = 1; orient <= 8; ++orient) {
            for (bool le : {true, false}) {
                std::vector<char> header;
                header.insert(header.end(), {'\xff', '\xd8'});

                // APP1 EXIF before SOF.
                const auto app1 = make_exif_app1(orient, le);
                header.insert(header.end(), app1.begin(), app1.end());

                // SOF0.
                header.insert(header.end(), {'\xff', '\xc0'});
                push_u16_be(header, 11);
                header.push_back('\x08');
                push_u16_be(header, 1080); // height
                push_u16_be(header, 1920); // width
                header.push_back('\x03');
                header.insert(header.end(), {1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1});

                const auto dims = sniff_dimensions(view(header));
                expect(dims.has_value()) << "orientation=" << orient;
                if (dims) {
                    if (orient >= 5) {
                        expect(eq(dims->width, 1080));
                        expect(eq(dims->height, 1920));
                        expect(dims->transposed);
                    } else {
                        expect(eq(dims->width, 1920));
                        expect(eq(dims->height, 1080));
                        expect(!dims->transposed);
                    }
                }

                const auto orient_read = read_exif_orientation(view(header));
                expect(orient_read.has_value());
                if (orient_read) {
                    expect(eq(*orient_read, orient));
                }
            }
        }
    };

    "jpeg_exif_after_sof_ignored"_test = [&] {
        std::vector<char> header = make_jpeg_sof(0xC0, 100, 200);
        const auto app1 = make_exif_app1(6, true);
        header.insert(header.end(), app1.begin(), app1.end());
        expect_dims(sniff_dimensions(view(header)), 100, 200, false);
    };

    "truncated_headers"_test = [] {
        // PNG truncated before IHDR height.
        {
            std::vector<char> b = make_png(100, 200);
            b.resize(22);
            expect(!sniff_dimensions(view(b)).has_value());
        }
        // GIF truncated before height.
        {
            std::vector<char> b = make_gif(100, 200);
            b.resize(9);
            expect(!sniff_dimensions(view(b)).has_value());
        }
        // BMP truncated before height.
        {
            std::vector<char> b = make_bmp(100, 200);
            b.resize(25);
            expect(!sniff_dimensions(view(b)).has_value());
        }
        // WebP VP8 truncated before height.
        {
            std::vector<char> b = make_webp_vp8(100, 200);
            b.resize(29);
            expect(!sniff_dimensions(view(b)).has_value());
        }
        // WebP VP8L truncated before bitstream end.
        {
            std::vector<char> b = make_webp_vp8l(100, 200);
            b.resize(24);
            expect(!sniff_dimensions(view(b)).has_value());
        }
        // JPEG truncated before SOF dimensions.
        {
            std::vector<char> b = make_jpeg_sof(0xC0, 100, 200);
            b.resize(8);
            expect(!sniff_dimensions(view(b)).has_value());
        }
    };

    "unknown_malformed"_test = [] {
        expect(!sniff_dimensions(kimix::string_view()).has_value());
        expect(!sniff_dimensions(kimix::string_view("not an image", 12)).has_value());
        expect(!read_exif_orientation(kimix::string_view()).has_value());
        expect(!read_exif_orientation(kimix::string_view("no exif", 7)).has_value());
        expect(!is_animated_webp(kimix::string_view()));
        expect(!is_animated_webp(kimix::string_view("RIFF----WEBPVP8 ", 16)));
    };

    "format_byte_size_boundaries"_test = [] {
        expect(format_byte_size(0) == "0 B");
        expect(format_byte_size(1023) == "1023 B");
        expect(format_byte_size(1024) == "1 KB");
        expect(format_byte_size(1536) == "2 KB"); // floor(1.5 + 0.5) = 2
        expect(format_byte_size(1024 * 1024 - 1) == "1024 KB");
        expect(format_byte_size(1024 * 1024) == "1.0 MB");
        expect(format_byte_size(1024 * 1024 * 2) == "2.0 MB");
        expect(format_byte_size(1024 * 1024 * 3 / 2) == "1.5 MB");
        expect(format_byte_size(1024ULL * 1024 * 1024) == "1024.0 MB");
    };
}
