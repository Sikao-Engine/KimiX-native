/*
 * guid.cpp — kimix::Guid implementation.
 *
 * Base64 and hex helpers are local (fixed 16-byte payload) because the project
 * has no StringUtil equivalent and no new dependency is wanted. GUID
 * generation uses ::CoCreateGuid on Windows and std::random_device elsewhere,
 * where RFC 4122 version-4 / variant bits are applied by hand.
 */
#include "guid.h"

#include <cstring>

#ifdef _WIN32
    #include <objbase.h>
#else
    #include <random>
#endif

namespace kimix {

Guid::Guid(bool generate) noexcept : data{0, 0} {
    if (generate) {
        remake();
    }
}

Guid::Guid(GuidData const &d) noexcept : data{d.data0, d.data1} {}

Guid::Guid(std::array<uint8_t, sizeof(GuidData)> const &data) noexcept : data{0, 0} {
    std::memcpy(&this->data, data.data(), sizeof(GuidData));
}

namespace detail {

uint8_t get_number(char c) noexcept {
    switch (c) {
        case '0': return 0;
        case '1': return 1;
        case '2': return 2;
        case '3': return 3;
        case '4': return 4;
        case '5': return 5;
        case '6': return 6;
        case '7': return 7;
        case '8': return 8;
        case '9': return 9;
        case 'a': return 10;
        case 'b': return 11;
        case 'c': return 12;
        case 'd': return 13;
        case 'e': return 14;
        case 'f': return 15;
        case 'A': return 10;
        case 'B': return 11;
        case 'C': return 12;
        case 'D': return 13;
        case 'E': return 14;
        case 'F': return 15;
        default: return 15;
    }
};

void parse_hex(string_view strv, Guid::GuidData &data) noexcept {
    char const *ptr = strv.data();
    span<uint8_t> dst{reinterpret_cast<uint8_t *>(&data), sizeof(Guid::GuidData)};
    for (auto &i : dst) {
        i = get_number(ptr[0]) << 4;
        i |= get_number(ptr[1]);
        ptr += 2;
    }
}

// RFC 4648 standard base64 (alphabet "A–Z a–z 0–9 + /"), specialised to the
// fixed 16-byte guid payload: encode writes 24 chars including the two '='
// padding chars, decode reads the first 22 payload chars (padding ignored) into
// exactly 16 bytes.
inline constexpr char const b64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Reverse of b64_alphabet; any character that is not in the alphabet maps to -1.
int8_t const *b64_inverse() noexcept {
    struct table_t {
        int8_t v[256];
        table_t() noexcept {
            for (int i = 0; i < 256; ++i) v[i] = -1;
            for (int8_t k = 0; k < 64; ++k) v[static_cast<unsigned char>(b64_alphabet[k])] = static_cast<int8_t>(k);
        }
    };
    static table_t const table;
    return table.v;
}

/* Standard RFC 4648 base64: one 3-byte group -> four alphabet chars, then a
   two-byte tail ("xx=") or a one-byte tail ("x=="). For size == 16 this
   produces 24 chars, of which the last two are '=' (to_base64() drops them). */
void encode_base64(uint8_t const *src, size_t size, char *dst) noexcept {
    auto const tab = b64_alphabet;
    char const *in = reinterpret_cast<char const *>(src);
    for (auto n = size / 3; n--;) {
        *dst++ = tab[(in[0] & 0xfc) >> 2];
        *dst++ = tab[((in[0] & 0x03) << 4) + ((in[1] & 0xf0) >> 4)];
        *dst++ = tab[((in[2] & 0xc0) >> 6) + ((in[1] & 0x0f) << 2)];
        *dst++ = tab[in[2] & 0x3f];
        in += 3;
    }
    switch (size - static_cast<size_t>(in - reinterpret_cast<char const *>(src))) {
        case 2:
            *dst++ = tab[(in[0] & 0xfc) >> 2];
            *dst++ = tab[((in[0] & 0x03) << 4) + ((in[1] & 0xf0) >> 4)];
            *dst++ = tab[(in[1] & 0x0f) << 2];
            *dst++ = '=';
            break;
        case 1:
            *dst++ = tab[(in[0] & 0xfc) >> 2];
            *dst++ = tab[((in[0] & 0x03) << 4)];
            *dst++ = '=';
            *dst++ = '=';
            break;
        case 0:
            break;
    }
}

/* Decode the payload characters of a base64 string into 'dst'. Padding and the
   trailing partial quantum are handled by byte count: exactly
   (num_chars_without_padding / 4) * 3 full bytes plus the remainder are
   written, so 22 payload chars give 16 bytes. Returns the number of decoded
   bytes, or 0 when an invalid character is hit before the output is filled. */
size_t decode_base64(char const *src, size_t num_chars, uint8_t *dst) noexcept {
    auto const inverse = b64_inverse();
    uint32_t acc = 0;
    int nbits = 0;
    size_t out = 0;
    size_t i = 0;
    for (; i < num_chars; ++i) {
        char const c = src[i];
        if (c == '=') break;
        int8_t const v = inverse[static_cast<unsigned char>(c)];
        if (v < 0) return 0;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            dst[out++] = static_cast<uint8_t>((acc >> nbits) & 0xffu);
        }
    }
    // Consume any remaining padding so callers can check for trailing junk.
    for (; i < num_chars; ++i) {
        if (src[i] != '=') return 0;
    }
    return out;
}

} // namespace detail

optional<Guid> Guid::try_parse(string_view strv) noexcept {
    using namespace detail;
    switch (strv.size()) {
        case 22: {
            Guid opt{};
            if (decode_base64(strv.data(), strv.size(), reinterpret_cast<uint8_t *>(&opt.data)) != sizeof(GuidData)) {
                return {};
            }
            return optional<Guid>{opt};
        }
        case 32: {
            Guid opt{};
            parse_hex(strv, opt.data);
            return optional<Guid>{opt};
        }
    }
    return {};
}

Guid::Guid(string_view strv) noexcept : data{0, 0} {
    using namespace detail;
    switch (strv.size()) {
        case 22:
            // Invalid input leaves the guid zeroed (no exit/log in this project).
            if (decode_base64(strv.data(), strv.size(), reinterpret_cast<uint8_t *>(&data)) != sizeof(GuidData)) {
                reset();
            }
            break;
        case 32:
            parse_hex(strv, data);
            break;
        default:
            reset();
            break;
    }
}

Guid::Guid(span<uint8_t const> data) noexcept : data{0, 0} {
    // Wrong size leaves the guid zeroed (no exit/log in this project).
    if (data.size() == sizeof(GuidData)) {
        std::memcpy(&this->data, data.data(), sizeof(GuidData));
    }
}

string Guid::to_base64() const {
    string result;
    result.resize(24);
    detail::encode_base64(reinterpret_cast<uint8_t const *>(&data), sizeof(data), result.data());
    result.resize(22);// drop the "==" padding
    return result;
}

void Guid::to_base64(char *result) const noexcept {
    detail::encode_base64(reinterpret_cast<uint8_t const *>(&data), sizeof(data), result);
}

void Guid::remake() noexcept {
#ifdef _WIN32
    static_assert(sizeof(data) == sizeof(_GUID), "Size mismatch");
    ::CoCreateGuid(reinterpret_cast<_GUID *>(&data));
#else
    /* No libuuid / CFUUID dependency: 16 random bytes turned into a valid
       RFC 4122 version-4 guid by setting the version and variant fields. */
    uint8_t bytes[sizeof(data)];
    std::random_device rd;
    for (size_t i = 0; i < sizeof(bytes); i += 4) {
        uint32_t const r = rd();
        std::memcpy(bytes + i, &r, 4);
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fu) | 0x40u);// version = 4
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fu) | 0x80u);// variant = 0b10
    std::memcpy(&data, bytes, sizeof(data));
#endif
}

std::array<uint8_t, sizeof(Guid::GuidData)> Guid::to_array() const noexcept {
    std::array<uint8_t, sizeof(GuidData)> arr{};
    std::memcpy(arr.data(), &data, sizeof(GuidData));
    return arr;
}

string Guid::to_string(bool upper) const {
    static constexpr char const *hex_digits[2] = {"0123456789abcdef", "0123456789ABCDEF"};
    char const *const digits = hex_digits[upper ? 1 : 0];
    auto const bytes = reinterpret_cast<uint8_t const *>(&data);
    string s;
    s.resize(sizeof(data) * 2);
    char *out = s.data();
    for (size_t i = 0; i < sizeof(data); ++i) {
        out[2 * i] = digits[(bytes[i] >> 4) & 15];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    return s;
}

void Guid::to_string(char *result, bool upper) const noexcept {
    static constexpr char const *hex_digits[2] = {"0123456789abcdef", "0123456789ABCDEF"};
    char const *const digits = hex_digits[upper ? 1 : 0];
    auto const bytes = reinterpret_cast<uint8_t const *>(&data);
    for (size_t i = 0; i < sizeof(data); ++i) {
        result[2 * i] = digits[(bytes[i] >> 4) & 15];
        result[2 * i + 1] = digits[bytes[i] & 15];
    }
}

} // namespace kimix
