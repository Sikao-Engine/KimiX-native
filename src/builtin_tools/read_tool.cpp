// read_tool.cpp - Pure-CPU kernels for the `read` built-in agent tool.
//
// Ports of the pure-text kernels listed in read_tool.h; see that header for
// the Python sources of truth and the exact line ranges. Highlights:
//   * render_forward / render_tail / render_result: read.py 1528-1697.
//     The tail path replaces Python's O(n^2) list.pop(0) with a bounded ring
//     buffer; every observable output (line numbers, budgets, messages) is
//     byte-identical to the reference.
//   * truncate_line_read: kimi_cli/tools/utils.py 113-126 (the "..." marker
//     variant read.py actually imports, NOT the output_utils variant).
//   * apply_char_window: read.py 349-385 (code-point window + NOTE text).
//   * compute_line_hashes: hash_line.py 55-109 (chained xxHash32 line ids).
//   * render_cpu_profile / render_sample_profile: read_profiles.py.
//   * markdown_to_text: read_markit.py 215-254 (nine regex passes, ported as
//     a deterministic scanner; underscore emphasis is word-boundary guarded).
//
// Kernels never throw: allocation is the only throwing operation and the
// tool boundary treats OOM as fatal anyway.

#include "builtin_tools/read_tool.h"

#include "builtin_tools/utf8_util.h"
#include "llm/yyjson_alc.h"

#include <yyjson.h>

#include <algorithm>
#include <cstring>

namespace kimix::builtin_tools {
namespace read {

namespace {

// ── UTF-8 primitives ───────────────────────────────────────────────────────

// Decode one code point; invalid bytes decode to U+FFFD and advance one byte
// (Python errors="replace" semantics used by read_lines(errors="replace")).
uint32_t rd_decode(const char *&it, const char *end) noexcept {
    if (it >= end) {
        return 0xFFFD;
    }
    const uint8_t b0 = static_cast<uint8_t>(*it);
    if (b0 < 0x80) {
        ++it;
        return b0;
    }
    const size_t avail = static_cast<size_t>(end - it);
    uint32_t cp = 0;
    size_t extra = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        cp = b0 & 0x1Fu;
        extra = 1;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        cp = b0 & 0x0Fu;
        extra = 2;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        cp = b0 & 0x07u;
        extra = 3;
    } else {
        ++it;
        return 0xFFFD; // invalid start byte
    }
    if (avail <= extra) {
        ++it;
        return 0xFFFD; // truncated sequence
    }
    const char *saved = it;
    ++it;
    for (size_t i = 0; i < extra; i++) {
        const uint8_t b = static_cast<uint8_t>(*it);
        if ((b & 0xC0u) != 0x80u) {
            it = saved + 1;
            return 0xFFFD; // invalid continuation
        }
        cp = (cp << 6) | (b & 0x3Fu);
        ++it;
    }
    // Overlong / surrogate / out-of-range checks.
    if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
        (extra == 3 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) ||
        cp > 0x10FFFF) {
        it = saved + 1;
        return 0xFFFD;
    }
    return cp;
}

// Append one code point as UTF-8.
void rd_encode(uint32_t cp, kimix::string &out) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

// Python str.isspace() code-point set (exact, see plan 013 notes).
bool rd_is_space_cp(uint32_t cp) noexcept {
    switch (cp) {
    case 0x0009: case 0x000A: case 0x000B: case 0x000C: case 0x000D:
    case 0x0020: case 0x0085: case 0x00A0: case 0x1680:
        return true;
    default:
        return (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
               cp == 0x202F || cp == 0x205F || cp == 0x3000;
    }
}

// ── Unicode alphanumeric ranges (Python str.isalnum = categories L* and N*) ─
// Mirrors the generated table in runtime/tools/line_hash.cpp (plan 013).
// read_tool_alnum.inc - Unicode alphanumeric ranges (Python str.isalnum,
// categories L* and N*). Generated from Python unicodedata; identical to the
// table in src/runtime/tools/line_hash.cpp (plan 013). Re-embedded here
// because builtin_tools must not depend on the runtime_py target.
constexpr uint32_t rd_alnum_ranges[][2] = {
    0x0030, 0x0039, 0x0041, 0x005A, 0x0061, 0x007A, 0x00AA, 0x00AA, 0x00B2, 0x00B3, 0x00B5, 0x00B5,
    0x00B9, 0x00BA, 0x00BC, 0x00BE, 0x00C0, 0x00D6, 0x00D8, 0x00F6, 0x00F8, 0x02C1, 0x02C6, 0x02D1,
    0x02E0, 0x02E4, 0x02EC, 0x02EC, 0x02EE, 0x02EE, 0x0370, 0x0374, 0x0376, 0x0377, 0x037A, 0x037D,
    0x037F, 0x037F, 0x0386, 0x0386, 0x0388, 0x038A, 0x038C, 0x038C, 0x038E, 0x03A1, 0x03A3, 0x03F5,
    0x03F7, 0x0481, 0x048A, 0x052F, 0x0531, 0x0556, 0x0559, 0x0559, 0x0560, 0x0588, 0x05D0, 0x05EA,
    0x05EF, 0x05F2, 0x0620, 0x064A, 0x0660, 0x0669, 0x066E, 0x06D3, 0x06D5, 0x06D5, 0x06E5, 0x06E6,
    0x06EE, 0x06FC, 0x06FF, 0x06FF, 0x0710, 0x0710, 0x0712, 0x072F, 0x074D, 0x07A5, 0x07B1, 0x07B1,
    0x07C0, 0x07EA, 0x07F4, 0x07F5, 0x07FA, 0x07FA, 0x0800, 0x0815, 0x081A, 0x0824, 0x0828, 0x0828,
    0x0840, 0x0858, 0x0860, 0x086A, 0x0870, 0x0887, 0x0889, 0x088E, 0x08A0, 0x08C9, 0x0904, 0x0939,
    0x093D, 0x093D, 0x0950, 0x0950, 0x0958, 0x0961, 0x0966, 0x096F, 0x0971, 0x0980, 0x0985, 0x098C,
    0x098F, 0x0990, 0x0993, 0x09A8, 0x09AA, 0x09B0, 0x09B2, 0x09B2, 0x09B6, 0x09B9, 0x09BD, 0x09BD,
    0x09CE, 0x09CE, 0x09DC, 0x09DD, 0x09DF, 0x09E1, 0x09E6, 0x09F1, 0x09F4, 0x09F9, 0x09FC, 0x09FC,
    0x0A01, 0x0A03, 0x0A05, 0x0A0A, 0x0A0F, 0x0A10, 0x0A13, 0x0A28, 0x0A2A, 0x0A30, 0x0A32, 0x0A33,
    0x0A35, 0x0A36, 0x0A38, 0x0A39, 0x0A59, 0x0A5C, 0x0A5E, 0x0A5E, 0x0A66, 0x0A6F, 0x0A72, 0x0A74,
    0x0A85, 0x0A8D, 0x0A8F, 0x0A91, 0x0A93, 0x0AA8, 0x0AAA, 0x0AB0, 0x0AB2, 0x0AB3, 0x0AB5, 0x0AB9,
    0x0ABD, 0x0ABD, 0x0AD0, 0x0AD0, 0x0AE0, 0x0AE1, 0x0AE6, 0x0AEF, 0x0AF9, 0x0AF9, 0x0B01, 0x0B03,
    0x0B05, 0x0B0C, 0x0B0F, 0x0B10, 0x0B13, 0x0B28, 0x0B2A, 0x0B30, 0x0B32, 0x0B33, 0x0B35, 0x0B39,
    0x0B3D, 0x0B3D, 0x0B5C, 0x0B5D, 0x0B5F, 0x0B61, 0x0B66, 0x0B6F, 0x0B71, 0x0B71, 0x0B83, 0x0B83,
    0x0B85, 0x0B8A, 0x0B8E, 0x0B90, 0x0B92, 0x0B95, 0x0B99, 0x0B9A, 0x0B9C, 0x0B9C, 0x0B9E, 0x0B9F,
    0x0BA3, 0x0BA4, 0x0BA8, 0x0BAA, 0x0BAE, 0x0BB9, 0x0BBE, 0x0BBF, 0x0BC1, 0x0BC2, 0x0BC6, 0x0BC8,
    0x0BCA, 0x0BCF, 0x0BD7, 0x0BD7, 0x0BE6, 0x0BEF, 0x0C00, 0x0C04, 0x0C05, 0x0C0C, 0x0C0E, 0x0C10,
    0x0C12, 0x0C28, 0x0C2A, 0x0C39, 0x0C3D, 0x0C3D, 0x0C58, 0x0C5A, 0x0C60, 0x0C61, 0x0C66, 0x0C6F,
    0x0C78, 0x0C7E, 0x0C80, 0x0C80, 0x0C85, 0x0C8C, 0x0C8E, 0x0C90, 0x0C92, 0x0CA8, 0x0CAA, 0x0CB3,
    0x0CB5, 0x0CB9, 0x0CBD, 0x0CBD, 0x0CDE, 0x0CDE, 0x0CE0, 0x0CE1, 0x0CE6, 0x0CEF, 0x0CF1, 0x0CF3,
    0x0D00, 0x0D0C, 0x0D0E, 0x0D10, 0x0D12, 0x0D3A, 0x0D3D, 0x0D3D, 0x0D4E, 0x0D4E, 0x0D54, 0x0D56,
    0x0D5F, 0x0D61, 0x0D66, 0x0D6F, 0x0D7A, 0x0D7F, 0x0D85, 0x0D96, 0x0D9A, 0x0DB1, 0x0DB3, 0x0DBB,
    0x0DBD, 0x0DBD, 0x0DC0, 0x0DC6, 0x0DCF, 0x0DD1, 0x0DD8, 0x0DDF, 0x0DE6, 0x0DEF, 0x0DF2, 0x0DF4,
    0x0E01, 0x0E30, 0x0E32, 0x0E33, 0x0E40, 0x0E46, 0x0E50, 0x0E59, 0x0E81, 0x0E82, 0x0E84, 0x0E84,
    0x0E86, 0x0E8A, 0x0E8C, 0x0EA3, 0x0EA5, 0x0EA5, 0x0EA7, 0x0EB0, 0x0EB2, 0x0EB3, 0x0EBD, 0x0EBD,
    0x0EC0, 0x0EC4, 0x0EC6, 0x0EC6, 0x0ED0, 0x0ED9, 0x0EDC, 0x0EDF, 0x0F00, 0x0F00, 0x0F18, 0x0F19,
    0x0F20, 0x0F33, 0x0F40, 0x0F47, 0x0F49, 0x0F6C, 0x0F88, 0x0F8C, 0x1000, 0x102A, 0x103F, 0x103F,
    0x1050, 0x1055, 0x105A, 0x105D, 0x1061, 0x1061, 0x1065, 0x1066, 0x106E, 0x1070, 0x1075, 0x1081,
    0x108E, 0x108E, 0x1090, 0x1099, 0x10A0, 0x10C5, 0x10C7, 0x10C7, 0x10CD, 0x10CD, 0x10D0, 0x10FA,
    0x10FC, 0x10FC, 0x10FD, 0x1248, 0x124A, 0x124D, 0x1250, 0x1256, 0x1258, 0x1258, 0x125A, 0x125D,
    0x1260, 0x1288, 0x128A, 0x128D, 0x1290, 0x12B0, 0x12B2, 0x12B5, 0x12B8, 0x12BE, 0x12C0, 0x12C0,
    0x12C2, 0x12C5, 0x12C8, 0x12D6, 0x12D8, 0x1310, 0x1312, 0x1315, 0x1318, 0x135A, 0x1380, 0x138F,
    0x13A0, 0x13F5, 0x13F8, 0x13FD, 0x1401, 0x166C, 0x166F, 0x167F, 0x1681, 0x169A, 0x16A0, 0x16EA,
    0x16EE, 0x16F8, 0x1700, 0x1711, 0x171F, 0x1731, 0x1740, 0x1751, 0x1760, 0x176C, 0x176E, 0x1770,
    0x1780, 0x17B3, 0x17D7, 0x17D7, 0x17DC, 0x17DC, 0x17E0, 0x17E9, 0x1810, 0x1819, 0x1820, 0x1878,
    0x1880, 0x1884, 0x1887, 0x18A8, 0x18AA, 0x18AA, 0x18B0, 0x18F5, 0x1900, 0x191E, 0x1946, 0x194F,
    0x1950, 0x196D, 0x1970, 0x1974, 0x1980, 0x19AB, 0x19B0, 0x19C9, 0x19D0, 0x19DA, 0x1A00, 0x1A16,
    0x1A20, 0x1A54, 0x1A80, 0x1A89, 0x1A90, 0x1A99, 0x1AA7, 0x1AA7, 0x1B05, 0x1B33, 0x1B45, 0x1B4C,
    0x1B50, 0x1B59, 0x1B83, 0x1BA0, 0x1BAE, 0x1BAF, 0x1BBA, 0x1BE5, 0x1C00, 0x1C23, 0x1C40, 0x1C49,
    0x1C4D, 0x1C7D, 0x1C80, 0x1C88, 0x1C90, 0x1CBA, 0x1CBD, 0x1CBF, 0x1CE9, 0x1CEC, 0x1CEE, 0x1CF3,
    0x1CF5, 0x1CF7, 0x1CFA, 0x1CFA, 0x1D00, 0x1DBF, 0x1E00, 0x1F15, 0x1F18, 0x1F1D, 0x1F20, 0x1F45,
    0x1F48, 0x1F4D, 0x1F50, 0x1F57, 0x1F59, 0x1F59, 0x1F5B, 0x1F5B, 0x1F5D, 0x1F5D, 0x1F5F, 0x1F7D,
    0x1F80, 0x1FB4, 0x1FB6, 0x1FBC, 0x1FBE, 0x1FBE, 0x1FC2, 0x1FC4, 0x1FC6, 0x1FCC, 0x1FD0, 0x1FD3,
    0x1FD6, 0x1FDB, 0x1FE0, 0x1FEC, 0x1FF2, 0x1FF4, 0x1FF6, 0x1FFC, 0x2071, 0x2071, 0x207F, 0x207F,
    0x2090, 0x209C, 0x2102, 0x2102, 0x2107, 0x2107, 0x210A, 0x2113, 0x2115, 0x2115, 0x2119, 0x211D,
    0x2124, 0x2124, 0x2126, 0x2126, 0x2128, 0x2128, 0x212A, 0x212D, 0x212F, 0x2139, 0x213C, 0x213F,
    0x2145, 0x2149, 0x214E, 0x214E, 0x2160, 0x2188, 0x24B6, 0x24E9, 0x2C00, 0x2CE4, 0x2CEB, 0x2CEE,
    0x2CF2, 0x2CF3, 0x2D00, 0x2D25, 0x2D27, 0x2D27, 0x2D2D, 0x2D2D, 0x2D30, 0x2D67, 0x2D6F, 0x2D6F,
    0x2D80, 0x2D96, 0x2DA0, 0x2DA6, 0x2DA8, 0x2DAE, 0x2DB0, 0x2DB6, 0x2DB8, 0x2DBE, 0x2DC0, 0x2DC6,
    0x2DC8, 0x2DCE, 0x2DD0, 0x2DD6, 0x2DD8, 0x2DDE, 0x2E2F, 0x2E2F, 0x3005, 0x3007, 0x3021, 0x3029,
    0x3031, 0x3035, 0x3038, 0x303C, 0x3041, 0x3096, 0x309D, 0x309F, 0x30A1, 0x30FA, 0x30FC, 0x30FF,
    0x3105, 0x312F, 0x3131, 0x318E, 0x31A0, 0x31BF, 0x31F0, 0x31FF, 0x3400, 0x4DBF, 0x4E00, 0xA48C,
    0xA4D0, 0xA4FD, 0xA500, 0xA60C, 0xA610, 0xA61F, 0xA62A, 0xA62B, 0xA640, 0xA66E, 0xA67F, 0xA69D,
    0xA6A0, 0xA6E5, 0xA6F0, 0xA6F1, 0xA717, 0xA71F, 0xA722, 0xA788, 0xA78B, 0xA7CA, 0xA7D0, 0xA7D1,
    0xA7D3, 0xA7D3, 0xA7D5, 0xA7D9, 0xA7F2, 0xA801, 0xA803, 0xA805, 0xA807, 0xA80A, 0xA80C, 0xA822,
    0xA830, 0xA835, 0xA840, 0xA873, 0xA882, 0xA8B3, 0xA8D0, 0xA8D9, 0xA8F2, 0xA8F7, 0xA8FB, 0xA8FB,
    0xA8FD, 0xA8FE, 0xA900, 0xA925, 0xA930, 0xA946, 0xA960, 0xA97C, 0xA984, 0xA9B2, 0xA9CF, 0xA9CF,
    0xA9D0, 0xA9D9, 0xA9E0, 0xA9E4, 0xA9E6, 0xA9EF, 0xA9FA, 0xA9FE, 0xAA00, 0xAA28, 0xAA40, 0xAA42,
    0xAA44, 0xAA4B, 0xAA50, 0xAA59, 0xAA60, 0xAA76, 0xAA7A, 0xAA7A, 0xAA7E, 0xAAAF, 0xAAB1, 0xAAB1,
    0xAAB5, 0xAAB6, 0xAAB9, 0xAABD, 0xAAC0, 0xAAC0, 0xAAC2, 0xAAC2, 0xAADB, 0xAADD, 0xAAE0, 0xAAEA,
    0xAAF2, 0xAAF4, 0xAB01, 0xAB06, 0xAB09, 0xAB0E, 0xAB11, 0xAB16, 0xAB20, 0xAB26, 0xAB28, 0xAB2E,
    0xAB30, 0xAB5A, 0xAB5C, 0xAB69, 0xAB70, 0xABE2, 0xABF0, 0xABF9, 0xAC00, 0xD7A3, 0xD7B0, 0xD7C6,
    0xD7CB, 0xD7FB, 0xF900, 0xFA6D, 0xFA70, 0xFAD9, 0xFB00, 0xFB06, 0xFB13, 0xFB17, 0xFB1D, 0xFB1D,
    0xFB1F, 0xFB28, 0xFB2A, 0xFB36, 0xFB38, 0xFB3C, 0xFB3E, 0xFB3E, 0xFB40, 0xFB41, 0xFB43, 0xFB44,
    0xFB46, 0xFBB1, 0xFBD3, 0xFD3D, 0xFD50, 0xFD8F, 0xFD92, 0xFDC7, 0xFDF0, 0xFDFB, 0xFE70, 0xFE74,
    0xFE76, 0xFEFC, 0xFF10, 0xFF19, 0xFF21, 0xFF3A, 0xFF41, 0xFF5A, 0xFF66, 0xFFBE, 0xFFC2, 0xFFC7,
    0xFFCA, 0xFFCF, 0xFFD2, 0xFFD7, 0xFFDA, 0xFFDC, 0x10000, 0x1000B, 0x1000D, 0x10026, 0x10028,
    0x1003A, 0x1003C, 0x1003D, 0x1003F, 0x1004D, 0x10050, 0x1005D, 0x10080, 0x100FA, 0x10140,
    0x10174, 0x10280, 0x1029C, 0x102A0, 0x102D0, 0x10300, 0x1031F, 0x1032D, 0x1034A, 0x10350,
    0x10375, 0x10380, 0x1039D, 0x103A0, 0x103C3, 0x103C8, 0x103CF, 0x103D1, 0x103D5, 0x10400,
    0x1049D, 0x104A0, 0x104A9, 0x104B0, 0x104D3, 0x104D8, 0x104FB, 0x10500, 0x10527, 0x10530,
    0x10563, 0x10570, 0x1057A, 0x1057C, 0x1058A, 0x1058C, 0x10592, 0x10594, 0x10595, 0x10597,
    0x105A1, 0x105A3, 0x105B1, 0x105B3, 0x105B9, 0x105BB, 0x105BC, 0x10600, 0x10736, 0x10740,
    0x10755, 0x10760, 0x10767, 0x10780, 0x10785, 0x10787, 0x107B0, 0x107B2, 0x107BA, 0x10800,
    0x10805, 0x10808, 0x10808, 0x1080A, 0x10835, 0x10837, 0x10838, 0x1083C, 0x1083C, 0x1083F,
    0x10855, 0x10860, 0x10876, 0x10880, 0x1089E, 0x108E0, 0x108F2, 0x108F4, 0x108F5, 0x10900,
    0x10915, 0x10920, 0x10939, 0x10980, 0x109B7, 0x109BE, 0x109BF, 0x10A00, 0x10A00, 0x10A10,
    0x10A13, 0x10A15, 0x10A17, 0x10A19, 0x10A35, 0x10A60, 0x10A7C, 0x10A80, 0x10A9C, 0x10AC0,
    0x10AC7, 0x10AC9, 0x10AE4, 0x10B00, 0x10B35, 0x10B40, 0x10B55, 0x10B60, 0x10B72, 0x10B80,
    0x10B91, 0x10C00, 0x10C48, 0x10C80, 0x10CB2, 0x10CC0, 0x10CF2, 0x10D00, 0x10D23, 0x10D30,
    0x10D39, 0x10E60, 0x10E7E, 0x10E80, 0x10EA9, 0x10EB0, 0x10EB1, 0x10F00, 0x10F1C, 0x10F27,
    0x10F27, 0x10F30, 0x10F45, 0x10F70, 0x10F81, 0x10FB0, 0x10FC4, 0x10FE0, 0x10FF6, 0x11000,
    0x11000, 0x11003, 0x11037, 0x11047, 0x1104D, 0x11066, 0x1106F, 0x11071, 0x11072, 0x11075,
    0x11075, 0x11083, 0x110AF, 0x110D0, 0x110E8, 0x110F0, 0x110F9, 0x11103, 0x11126, 0x11136,
    0x1113F, 0x11144, 0x11144, 0x11147, 0x11147, 0x11150, 0x11172, 0x11176, 0x11176, 0x11180,
    0x111B2, 0x111C1, 0x111C4, 0x111DA, 0x111DA, 0x111DC, 0x111DC, 0x11200, 0x11211, 0x11213,
    0x1122B, 0x11280, 0x11286, 0x11288, 0x11288, 0x1128A, 0x1128D, 0x1128F, 0x1129D, 0x1129F,
    0x112A8, 0x112B0, 0x112DE, 0x11305, 0x1130C, 0x1130F, 0x11310, 0x11313, 0x11328, 0x1132A,
    0x11330, 0x11332, 0x11333, 0x11335, 0x11339, 0x1133D, 0x1133D, 0x11350, 0x11350, 0x1135D,
    0x11361, 0x11400, 0x11434, 0x11447, 0x1144A, 0x11450, 0x11459, 0x1145F, 0x11461, 0x11480,
    0x114AF, 0x114C4, 0x114C7, 0x114D0, 0x114D9, 0x11580, 0x115AE, 0x115D8, 0x115DB, 0x11600,
    0x1162F, 0x11644, 0x11644, 0x11650, 0x11659, 0x11680, 0x116AA, 0x116B0, 0x116B5, 0x116C0,
    0x116C9, 0x11700, 0x1171A, 0x11730, 0x1173F, 0x11740, 0x11746, 0x11800, 0x1182B, 0x118A0,
    0x118E9, 0x118FF, 0x118FF, 0x11900, 0x11906, 0x11909, 0x11909, 0x1190C, 0x11913, 0x11915,
    0x11916, 0x11918, 0x1192F, 0x1193F, 0x1193F, 0x11941, 0x11941, 0x11950, 0x11959, 0x119A0,
    0x119A7, 0x119AA, 0x119D0, 0x119E1, 0x119E3, 0x119E4, 0x11A00, 0x11A00, 0x11A0B, 0x11A32,
    0x11A3A, 0x11A3A, 0x11A50, 0x11A50, 0x11A5C, 0x11A89, 0x11A9D, 0x11A9D, 0x11AB0, 0x11AF8,
    0x11C00, 0x11C08, 0x11C0A, 0x11C2E, 0x11C40, 0x11C40, 0x11C50, 0x11C6C, 0x11C72, 0x11C8F,
    0x11D00, 0x11D06, 0x11D08, 0x11D09, 0x11D0B, 0x11D30, 0x11D46, 0x11D46, 0x11D50, 0x11D59,
    0x11D60, 0x11D65, 0x11D67, 0x11D68, 0x11D6A, 0x11D89, 0x11D98, 0x11D98, 0x11DA0, 0x11DA9,
    0x11EE0, 0x11EF2, 0x11EF5, 0x11EF8, 0x11F00, 0x11F01, 0x11F02, 0x11F02, 0x11F04, 0x11F10,
    0x11F12, 0x11F33, 0x11F50, 0x11F59, 0x11FB0, 0x11FB0, 0x12000, 0x12399, 0x12400, 0x1246E,
    0x12480, 0x12543, 0x12F90, 0x12FF0, 0x13000, 0x1342F, 0x13441, 0x13446, 0x14400, 0x14646,
    0x16800, 0x16A38, 0x16A40, 0x16A5E, 0x16A60, 0x16A69, 0x16A70, 0x16ABE, 0x16AD0, 0x16AED,
    0x16B00, 0x16B2F, 0x16B40, 0x16B43, 0x16B50, 0x16B59, 0x16B63, 0x16B77, 0x16B7D, 0x16B8F,
    0x16E40, 0x16E7F, 0x16F00, 0x16F4A, 0x16F50, 0x16F50, 0x16F93, 0x16F9F, 0x16FE0, 0x16FE1,
    0x16FE3, 0x16FE3, 0x17000, 0x187F7, 0x18800, 0x18CD5, 0x18D00, 0x18D08, 0x1AFF0, 0x1AFF3,
    0x1AFF5, 0x1AFFB, 0x1AFFD, 0x1AFFE, 0x1B000, 0x1B122, 0x1B132, 0x1B132, 0x1B150, 0x1B152,
    0x1B155, 0x1B155, 0x1B164, 0x1B167, 0x1B170, 0x1B2FB, 0x1BC00, 0x1BC6A, 0x1BC70, 0x1BC7C,
    0x1BC80, 0x1BC88, 0x1BC90, 0x1BC99, 0x1BC9E, 0x1BC9E, 0x1D400, 0x1D454, 0x1D456, 0x1D49C,
    0x1D49E, 0x1D49F, 0x1D4A2, 0x1D4A2, 0x1D4A5, 0x1D4A6, 0x1D4A9, 0x1D4AC, 0x1D4AE, 0x1D4B9,
    0x1D4BB, 0x1D4BB, 0x1D4BD, 0x1D4C3, 0x1D4C5, 0x1D505, 0x1D507, 0x1D50A, 0x1D50D, 0x1D514,
    0x1D516, 0x1D51C, 0x1D51E, 0x1D539, 0x1D53B, 0x1D53E, 0x1D540, 0x1D544, 0x1D546, 0x1D546,
    0x1D54A, 0x1D550, 0x1D552, 0x1D6A5, 0x1D6A8, 0x1D7CB, 0x1D7CE, 0x1D9FF, 0x1DA00, 0x1DA36,
    0x1DA3B, 0x1DA6C, 0x1DA75, 0x1DA75, 0x1DA84, 0x1DA84, 0x1DA9B, 0x1DA9F, 0x1DF00, 0x1DF1E,
    0x1DF25, 0x1DF2A, 0x1E030, 0x1E06D, 0x1E100, 0x1E12C, 0x1E137, 0x1E13D, 0x1E14E, 0x1E14E,
    0x1E290, 0x1E2AD, 0x1E2C0, 0x1E2EB, 0x1E4D0, 0x1E4EA, 0x1E4F0, 0x1E4F9, 0x1E7E0, 0x1E7E6,
    0x1E7E8, 0x1E7EB, 0x1E7ED, 0x1E7EE, 0x1E7F0, 0x1E7FE, 0x1E800, 0x1E8C4, 0x1E8D0, 0x1E8D6,
    0x1E900, 0x1E943, 0x1E950, 0x1E959, 0x1EE00, 0x1EE03, 0x1EE05, 0x1EE1F, 0x1EE21, 0x1EE22,
    0x1EE24, 0x1EE24, 0x1EE27, 0x1EE27, 0x1EE29, 0x1EE32, 0x1EE34, 0x1EE37, 0x1EE39, 0x1EE39,
    0x1EE3B, 0x1EE3B, 0x1EE42, 0x1EE42, 0x1EE47, 0x1EE47, 0x1EE49, 0x1EE49, 0x1EE4B, 0x1EE4B,
    0x1EE4D, 0x1EE4F, 0x1EE51, 0x1EE52, 0x1EE54, 0x1EE54, 0x1EE57, 0x1EE57, 0x1EE59, 0x1EE59,
    0x1EE5B, 0x1EE5B, 0x1EE5D, 0x1EE5D, 0x1EE5F, 0x1EE5F, 0x1EE61, 0x1EE62, 0x1EE64, 0x1EE64,
    0x1EE67, 0x1EE6A, 0x1EE6C, 0x1EE72, 0x1EE74, 0x1EE77, 0x1EE79, 0x1EE7C, 0x1EE7E, 0x1EE7E,
    0x1EE80, 0x1EE89, 0x1EE8B, 0x1EE9B, 0x1EEA1, 0x1EEA3, 0x1EEA5, 0x1EEA9, 0x1EEAB, 0x1EEBB,
    0x20000, 0x2A6DF, 0x2A700, 0x2B739, 0x2B740, 0x2B81D, 0x2B820, 0x2CEA1, 0x2CEB0, 0x2EBE0,
    0x2F800, 0x2FA1D, 0x30000, 0x3134A, 0x31350, 0x323AF, 0xE0100, 0xE01EF,
};

bool rd_is_alnum_cp(uint32_t cp) noexcept {
    if (cp < 0x80) {
        return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') ||
               (cp >= 'A' && cp <= 'Z');
    }
    size_t lo = 0;
    size_t hi = sizeof(rd_alnum_ranges) / sizeof(rd_alnum_ranges[0]);
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (cp < rd_alnum_ranges[mid][0]) {
            hi = mid;
        } else if (cp > rd_alnum_ranges[mid][1]) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

// Python regex \w under UNICODE: [a-zA-Z0-9_] + Unicode word characters
// (categories L*, N*, Pc + Mn/Mc marking characters). Approximated as
// alnum + underscore + connector punctuation (Pc). Mn/Mc is a superset of
// this but only affects exotic scripts; ASCII/CJK/European are exact.
bool rd_is_word_cp(uint32_t cp) noexcept {
    if (cp < 0x80) {
        return rd_is_alnum_cp(cp) || cp == '_';
    }
    if (cp == '_') {
        return true;
    }
    // Connector punctuation (Pc) outside ASCII.
    switch (cp) {
    case 0x203F: case 0x2040: case 0x2054: case 0xFE33: case 0xFE34:
    case 0xFE4D: case 0xFE4E: case 0xFE4F: case 0xFF3F:
        return true;
    default:
        break;
    }
    return rd_is_alnum_cp(cp);
}

// ── XXH32 (canonical xxHash 32-bit; matches the Python xxhash package) ─────
constexpr uint32_t rd_prime32_1 = 0x9E3779B1u;
constexpr uint32_t rd_prime32_2 = 0x85EBCA77u;
constexpr uint32_t rd_prime32_3 = 0xC2B2AE3Du;
constexpr uint32_t rd_prime32_4 = 0x27D4EB2Fu;
constexpr uint32_t rd_prime32_5 = 0x165667B1u;

inline uint32_t rd_rotl32(uint32_t x, int r) noexcept {
    return (x << r) | (x >> (32 - r));
}
inline uint32_t rd_read32_le(const uint8_t *p) noexcept {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
inline uint32_t rd_round32(uint32_t acc, uint32_t input) noexcept {
    acc += input * rd_prime32_2;
    acc = rd_rotl32(acc, 13);
    acc *= rd_prime32_1;
    return acc;
}
uint32_t rd_xxh32(const void *input, size_t len, uint32_t seed) noexcept {
    const uint8_t *p = static_cast<const uint8_t *>(input);
    const uint8_t *const bend = p + len;
    uint32_t h32;
    if (len >= 16) {
        const uint8_t *const limit = bend - 16;
        uint32_t v1 = seed + rd_prime32_1 + rd_prime32_2;
        uint32_t v2 = seed + rd_prime32_2;
        uint32_t v3 = seed;
        uint32_t v4 = seed - rd_prime32_1;
        do {
            v1 = rd_round32(v1, rd_read32_le(p));
            p += 4;
            v2 = rd_round32(v2, rd_read32_le(p));
            p += 4;
            v3 = rd_round32(v3, rd_read32_le(p));
            p += 4;
            v4 = rd_round32(v4, rd_read32_le(p));
            p += 4;
        } while (p <= limit);
        h32 = rd_rotl32(v1, 1) + rd_rotl32(v2, 7) + rd_rotl32(v3, 12) +
              rd_rotl32(v4, 18);
    } else {
        h32 = seed + rd_prime32_5;
    }
    h32 += static_cast<uint32_t>(len);
    while (p + 4 <= bend) {
        h32 += rd_read32_le(p) * rd_prime32_3;
        h32 = rd_rotl32(h32, 17) * rd_prime32_4;
        p += 4;
    }
    while (p < bend) {
        h32 += static_cast<uint32_t>(*p) * rd_prime32_5;
        h32 = rd_rotl32(h32, 11) * rd_prime32_1;
        ++p;
    }
    h32 ^= h32 >> 15;
    h32 *= rd_prime32_2;
    h32 ^= h32 >> 13;
    h32 *= rd_prime32_3;
    h32 ^= h32 >> 16;
    return h32;
}

// NIBBLE_STR = "ZPMQVRWSNKTXJBYH" (hash_line.py 39).
constexpr char rd_nibble_str[16] = {'Z', 'P', 'M', 'Q', 'V', 'R', 'W', 'S',
                                      'N', 'K', 'T', 'X', 'J', 'B', 'Y', 'H'};

// Format a float exactly like Python f"{v:.2f}" / f"{v:.1f}": round-half-even
// at the given precision (std::format uses the default round-to-nearest,
// ties-to-even rule — the same as Python).
kimix::string rd_fmt_f(double v, int precision) {
    char buf[64];
    const int n = std::snprintf(buf, sizeof(buf),
                                precision == 1 ? "%.1f" : "%.2f", v);
    return kimix::string(buf, static_cast<size_t>(n));
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Parameter validation (Params._validate_value, read.py 278-294)
// ═══════════════════════════════════════════════════════════════════════════

tool_error validate_int_option(kimix::string_view name, int64_t value) {
    tool_error err;
    if (name == "offset") {
        if (value == 0) {
            err.status = tool_status::invalid_input;
            err.message = "offset cannot be 0; use 1 for the first line or -1 "
                          "for the last line";
            return err;
        }
        if (value < -k_max_lines) {
            err.status = tool_status::invalid_input;
            kimix::StringScratch ss;
            ss << "offset cannot be less than -" << k_max_lines << ". "
                  "Use a positive offset with the total line count "
                  "to read from a specific position.";
            err.message = std::move(ss.string());
            return err;
        }
        return err; // ok
    }
    int64_t min_value = 0;
    if (name == "limit") {
        min_value = 1;
    } else if (name == "max_char" || name == "char_offset") {
        min_value = 0;
    } else {
        err.status = tool_status::invalid_input;
        err.message = kimix::string("unknown option: ") + kimix::string(name);
        return err;
    }
    if (value < min_value) {
        err.status = tool_status::invalid_input;
        kimix::StringScratch ss;
        ss << name << " must be >= " << min_value << ".";
        err.message = std::move(ss.string());
        return err;
    }
    return err; // ok
}

// ═══════════════════════════════════════════════════════════════════════════
// truncate_line_read (kimi_cli/tools/utils.py 113-126)
// ═══════════════════════════════════════════════════════════════════════════

void truncate_line_read(kimix::string_view text, int64_t max_len,
                        kimix::string &out) {
    const size_t cp_len = utf8_code_point_count(text);
    if (static_cast<int64_t>(cp_len) <= max_len) {
        out.assign(text.data(), text.size());
        return;
    }
    // Find the trailing line-break run: re.search(r"[\r\n]+$", line).
    size_t lb_bytes = 0;
    size_t lb_cp = 0;
    {
        size_t i = text.size();
        while (i > 0 && (text[i - 1] == '\r' || text[i - 1] == '\n')) {
            i--;
            lb_bytes++;
            lb_cp++;
        }
    }
    const char *marker = "...";
    const size_t marker_cp = 3;
    const size_t end_cp = marker_cp + lb_cp;
    // max_length = max(max_length, len(end))
    int64_t budget = max_len;
    if (budget < static_cast<int64_t>(end_cp)) {
        budget = static_cast<int64_t>(end_cp);
    }
    const size_t keep_cp = static_cast<size_t>(budget) - end_cp;
    const size_t keep_bytes = utf8_byte_offset_of_code_point(text, keep_cp);
    out.assign(text.data(), keep_bytes);
    out.append(marker, marker_cp);
    out.append(text.data() + text.size() - lb_bytes, lb_bytes);
}

// ═══════════════════════════════════════════════════════════════════════════
// Line splitting (Python text-mode / read_lines semantics)
// ═══════════════════════════════════════════════════════════════════════════

kimix::vector<kimix::string> split_lines(kimix::string_view bytes) {
    kimix::vector<kimix::string> lines;
    const size_t n = bytes.size();
    lines.reserve(n / 32 + 1);
    size_t start = 0;
    size_t i = 0;
    while (i < n) {
        const char c = bytes[i];
        if (c == '\n') {
            // Decode [start, i+1) with replacement, newline normalized to \n.
            kimix::string line;
            line.reserve(i + 1 - start);
            const char *it = bytes.data() + start;
            const char *end = bytes.data() + i;
            while (it < end) {
                rd_encode(rd_decode(it, end), line);
            }
            line.push_back('\n');
            lines.push_back(std::move(line));
            start = i + 1;
            i++;
        } else if (c == '\r') {
            // Lone CR or CRLF both become one line ending with '\n'.
            kimix::string line;
            line.reserve(i + 1 - start);
            const char *it = bytes.data() + start;
            const char *end = bytes.data() + i;
            while (it < end) {
                rd_encode(rd_decode(it, end), line);
            }
            line.push_back('\n');
            lines.push_back(std::move(line));
            start = i + 1;
            if (i + 1 < n && bytes[i + 1] == '\n') {
                start = i + 2;
                i += 2;
            } else {
                i++;
            }
        } else {
            i++;
        }
    }
    if (start < n) {
        kimix::string line;
        line.reserve(n - start);
        const char *it = bytes.data() + start;
        const char *end = bytes.data() + n;
        while (it < end) {
            rd_encode(rd_decode(it, end), line);
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

// ═══════════════════════════════════════════════════════════════════════════
// Render engine (_render_forward / _render_tail / _render_result)
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// _render_result (read.py 1650-1697).
void rd_render_result(kimix::span<const rendered_line> candidates,
                      kimix::string_view display_path, int64_t n_lines,
                      int64_t start_line, bool show_line_numbers,
                      int64_t total_lines /* -1 == None */,
                      bool max_lines_reached, bool max_bytes_reached,
                      bool end_of_file, kimix::string_view note,
                      render_result &result) {
    result.start_line = start_line;
    result.max_lines_reached = max_lines_reached;
    result.max_bytes_reached = max_bytes_reached;
    result.end_of_file = end_of_file;
    result.total_lines = total_lines;

    kimix::StringScratch out;
    out.reserve(k_max_bytes);
    for (const auto &entry : candidates) {
        if (entry.was_truncated) {
            result.truncated_line_numbers.push_back(entry.line_no);
        }
        if (show_line_numbers) {
            // f"{line_no:6d}\t{truncated}"
            char num[24];
            const int len = std::snprintf(num, sizeof(num), "%6lld\t",
                                          static_cast<long long>(entry.line_no));
            out << kimix::string_view(num, static_cast<size_t>(len));
        }
        out << entry.text;
    }
    result.output = std::move(out.string());

    kimix::StringScratch msg;
    const size_t line_count = candidates.size();
    if (line_count > 0) {
        msg << line_count << " lines read from file starting from line "
            << start_line << ".";
    } else {
        msg << "No lines read from file.";
    }
    if (total_lines >= 0) {
        msg << " Total lines in file: " << total_lines << ".";
    }
    if (max_lines_reached) {
        msg << " Max " << k_max_lines << " lines reached.";
    } else if (max_bytes_reached) {
        msg << " Max " << k_max_bytes << " bytes reached.";
    } else if (end_of_file) {
        msg << " End of file reached.";
    }
    if (!result.truncated_line_numbers.empty()) {
        // Python list repr: "[1, 3, 17]" (space after each comma).
        msg << " Lines [";
        for (size_t i = 0; i < result.truncated_line_numbers.size(); i++) {
            if (i != 0) {
                msg << ", ";
            }
            msg << result.truncated_line_numbers[i];
        }
        msg << "] were truncated.";
    }
    if (!note.empty()) {
        msg << note;
    }
    msg << " Path: " << display_path;
    result.message = std::move(msg.string());
}

// rstrip("\r\n") copy of a rendered line (raw_collected in Python).
kimix::string rd_rstrip_eol(kimix::string_view line) {
    size_t end = line.size();
    while (end > 0 && (line[end - 1] == '\r' || line[end - 1] == '\n')) {
        end--;
    }
    return kimix::string(line.data(), end);
}

} // namespace

render_result render_forward(kimix::span<const kimix::string> lines,
                             kimix::string_view display_path,
                             int64_t line_offset, int64_t n_lines,
                             bool show_line_numbers, kimix::string_view note) {
    render_result result;
    kimix::vector<rendered_line> entries;
    kimix::vector<kimix::string> raw_collected;
    uint64_t n_bytes = 0;
    bool max_lines_reached = false;
    bool max_bytes_reached = false;
    int64_t current_line_no = 0;
    const int64_t target_lines = std::min(n_lines, k_max_lines);
    bool eof_reached = true;

    kimix::string truncated_buf;
    for (const auto &line : lines) {
        current_line_no++;
        if (current_line_no < line_offset) {
            continue;
        }
        truncate_line_read(line, k_max_line_length, truncated_buf);
        const uint64_t b_len = truncated_buf.size(); // already UTF-8
        rendered_line entry;
        entry.line_no = current_line_no;
        entry.text = truncated_buf;
        entry.was_truncated = truncated_buf != line;
        entry.byte_len = b_len;
        entries.push_back(std::move(entry));
        raw_collected.push_back(rd_rstrip_eol(line));
        n_bytes += b_len;
        if (static_cast<int64_t>(entries.size()) >= target_lines) {
            max_lines_reached = target_lines >= k_max_lines;
            eof_reached = false;
            break;
        }
        if (n_bytes >= k_max_bytes) {
            max_bytes_reached = true;
            eof_reached = false;
            break;
        }
    }

    result.window_lines = std::move(raw_collected);
    rd_render_result(entries, display_path, n_lines, line_offset,
                     show_line_numbers, eof_reached ? current_line_no : -1,
                     max_lines_reached, max_bytes_reached,
                     static_cast<int64_t>(entries.size()) < n_lines, note,
                     result);
    return result;
}

render_result render_tail(kimix::span<const kimix::string> lines,
                          kimix::string_view display_path, int64_t line_offset,
                          int64_t n_lines, bool show_line_numbers,
                          kimix::string_view note) {
    render_result result;
    const size_t tail_count = static_cast<size_t>(
        line_offset < 0 ? -line_offset : line_offset);
    const int64_t line_limit = std::min(n_lines, k_max_lines);

    // Bounded window keeping the last `tail_count` lines. The Python
    // reference uses list.pop(0), which is O(n^2); kimix::deque pop_front is
    // O(1), keeping the whole pass O(n). (kimix::ring_buffer has no
    // iteration support, so a deque is the bounded-window container here.)
    kimix::deque<rendered_line> tail_buf;
    kimix::deque<std::pair<int64_t, kimix::string>> tail_raw;

    int64_t current_line_no = 0;
    kimix::string truncated_buf;
    for (const auto &line : lines) {
        current_line_no++;
        truncate_line_read(line, k_max_line_length, truncated_buf);
        rendered_line entry;
        entry.line_no = current_line_no;
        entry.text = truncated_buf;
        entry.was_truncated = truncated_buf != line;
        entry.byte_len = truncated_buf.size();
        tail_buf.push_back(std::move(entry));
        tail_raw.emplace_back(current_line_no, rd_rstrip_eol(line));
        if (tail_buf.size() > tail_count) {
            tail_buf.pop_front();
            tail_raw.pop_front();
        }
    }
    const int64_t total_lines = current_line_no;

    // Apply n_lines / MAX_LINES from the head of tail_buf.
    kimix::vector<rendered_line> candidates;
    {
        const size_t take = std::min<size_t>(tail_buf.size(),
                                             static_cast<size_t>(line_limit));
        candidates.reserve(take);
        for (size_t i = 0; i < take; i++) {
            candidates.push_back(tail_buf[i]);
        }
    }
    const bool max_lines_reached =
        tail_buf.size() > static_cast<size_t>(k_max_lines) &&
        candidates.size() == static_cast<size_t>(k_max_lines);

    // Apply the byte budget: reverse-scan to keep the newest lines that fit.
    bool max_bytes_reached = false;
    if (!candidates.empty()) {
        uint64_t total_candidate_bytes = 0;
        for (const auto &entry : candidates) {
            total_candidate_bytes += entry.byte_len;
        }
        if (total_candidate_bytes > k_max_bytes) {
            max_bytes_reached = true;
            size_t kept = 0;
            uint64_t n_bytes = 0;
            for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
                n_bytes += it->byte_len;
                if (n_bytes > k_max_bytes) {
                    break;
                }
                kept++;
            }
            if (kept < candidates.size()) {
                candidates.erase(candidates.begin(),
                                 candidates.begin() +
                                     static_cast<int64_t>(candidates.size() - kept));
            }
        }
    }

    const int64_t start_line =
        candidates.empty() ? total_lines + 1 : candidates.front().line_no;

    // window_lines for the kept candidates. tail_raw stays index-aligned with
    // tail_buf (same pushes/pops), and candidates is a contiguous suffix of
    // the (head-capped) window, so map by line number for robustness.
    for (const auto &entry : candidates) {
        for (const auto &raw : tail_raw) {
            if (raw.first == entry.line_no) {
                result.window_lines.push_back(raw.second);
                break;
            }
        }
    }

    rd_render_result(candidates, display_path, n_lines, start_line,
                     show_line_numbers, total_lines, max_lines_reached,
                     max_bytes_reached,
                     static_cast<int64_t>(candidates.size()) < n_lines, note,
                     result);
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Character window (_apply_char_window, read.py 349-385)
// ═══════════════════════════════════════════════════════════════════════════

char_window apply_char_window(kimix::string_view output, int64_t char_offset,
                              int64_t max_char) {
    char_window result;
    const size_t begin_byte =
        utf8_byte_offset_of_code_point(output, static_cast<size_t>(char_offset));
    const size_t end_byte = utf8_byte_offset_of_code_point(
        output, static_cast<size_t>(char_offset + max_char));
    result.output.assign(output.data() + begin_byte,
                         end_byte - begin_byte);

    const size_t total = utf8_code_point_count(output);
    const int64_t end = char_offset + max_char;
    if (end < static_cast<int64_t>(total) || char_offset > 0) {
        kimix::StringScratch msg;
        if (char_offset > 0 && end < static_cast<int64_t>(total)) {
            msg << " NOTE: output window shows middle chars " << char_offset
                << ".." << end << " of " << total
                << " (content before and after is hidden); max_char=" << max_char
                << ". Raise max_char / adjust char_offset to read the rest.";
        } else if (char_offset > 0) {
            msg << " NOTE: output window shows tail chars " << char_offset
                << ".." << total << " of " << total
                << " (content before is hidden); max_char=" << max_char
                << ". Raise max_char / adjust char_offset to read the rest.";
        } else {
            msg << " NOTE: output window shows head chars 0.." << end << " of "
                << total << " (content after is hidden); max_char=" << max_char
                << ". Raise max_char / adjust char_offset to read the rest.";
        }
        result.note = std::move(msg.string());
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Line-hash / repeated-line collapse (hash_line.py)
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// Core recipe for one line: strip a trailing '\r', keep only non-whitespace
// code points, detect alphanumeric content, then xxh32 & 0xFF with `seed`.
// `out_filtered` receives the filtered bytes; returns the hash.
uint32_t rd_hash_line_core(kimix::string_view line, uint32_t seed,
                           kimix::string *out_filtered = nullptr) {
    size_t len = line.size();
    if (len > 0 && line[len - 1] == '\r') {
        len -= 1;
        line = kimix::string_view(line.data(), len);
    }
    kimix::string filtered;
    filtered.reserve(len);
    const char *it = line.data();
    const char *end = line.data() + len;
    while (it < end) {
        const char *before = it;
        const uint32_t cp = rd_decode(it, end);
        if (!rd_is_space_cp(cp)) {
            filtered.append(before, static_cast<size_t>(it - before));
        }
    }
    const uint32_t h = rd_xxh32(filtered.data(), filtered.size(), seed) & 0xFFu;
    if (out_filtered != nullptr) {
        *out_filtered = std::move(filtered);
    }
    return h;
}

// True when the line contains at least one alphanumeric code point (after
// the trailing-CR strip).
bool rd_line_has_significant(kimix::string_view line) {
    size_t len = line.size();
    if (len > 0 && line[len - 1] == '\r') {
        len -= 1;
    }
    const char *it = line.data();
    const char *end = line.data() + len;
    while (it < end) {
        const uint32_t cp = rd_decode(it, end);
        if (rd_is_alnum_cp(cp)) {
            return true;
        }
    }
    return false;
}

} // namespace

kimix::vector<uint32_t> compute_line_hashes(kimix::string_view content) {
    kimix::vector<uint32_t> out;
    bool has_prev = false;
    uint32_t prev_hash = 0;
    uint32_t line_num = 0;
    size_t pos = 0;
    const size_t n = content.size();
    while (pos < n) {
        const size_t nl = content.find('\n', pos);
        const size_t line_end = (nl == kimix::string_view::npos) ? n : nl;
        const kimix::string_view line = content.substr(pos, line_end - pos);
        pos = (nl == kimix::string_view::npos) ? n : nl + 1;
        line_num++;
        uint32_t seed;
        if (has_prev) {
            // seed = ((prev chars folded) *256 + ord) & 0xFFFFFFFF
            const uint32_t c0 = static_cast<uint32_t>(rd_nibble_str[prev_hash >> 4]);
            const uint32_t c1 = static_cast<uint32_t>(rd_nibble_str[prev_hash & 0x0F]);
            seed = (c0 * 256 + c1) & 0xFFFFFFFFu;
        } else if (rd_line_has_significant(line)) {
            seed = 0; // HASH_SEED
        } else {
            seed = line_num;
        }
        const uint32_t h = rd_hash_line_core(line, seed);
        out.push_back(h);
        has_prev = true;
        prev_hash = h;
    }
    return out;
}

uint32_t line_hash_independent(kimix::string_view line) {
    // Non-chained variant: no previous hash, and non-significant lines use
    // line_num=1 so identical lines always hash identically regardless of
    // their position in the file.
    const uint32_t seed = rd_line_has_significant(line) ? 0u : 1u;
    return rd_hash_line_core(line, seed);
}

kimix::vector<kimix::string> compute_line_hash_strings(kimix::string_view content) {
    const kimix::vector<uint32_t> hashes = compute_line_hashes(content);
    kimix::vector<kimix::string> out;
    out.reserve(hashes.size());
    for (const uint32_t h : hashes) {
        kimix::string s;
        s.push_back(rd_nibble_str[h >> 4]);
        s.push_back(rd_nibble_str[h & 0x0F]);
        out.push_back(std::move(s));
    }
    return out;
}

void collapse_repeated_lines(kimix::span<const uint32_t> hashes,
                             kimix::span<const kimix::string> lines,
                             size_t min_repeats,
                             kimix::vector<kimix::string> &out, size_t &saved) {
    out.clear();
    saved = 0;
    if (min_repeats < 2) {
        min_repeats = 2;
    }
    const size_t n = std::min(hashes.size(), lines.size());
    if (n < 2) {
        out.assign(lines.begin(), lines.begin() + n);
        return;
    }
    out.reserve(n);
    size_t i = 0;
    while (i < n) {
        size_t j = i + 1;
        while (j < n && hashes[j] == hashes[i]) {
            j++;
        }
        const size_t run_len = j - i;
        if (run_len >= min_repeats) {
            kimix::StringScratch ss;
            ss << lines[i] << "  (" << (run_len - 1) << " repeats)";
            out.push_back(std::move(ss.string()));
            saved += run_len - 1;
        } else {
            for (size_t k = i; k < j; k++) {
                out.push_back(lines[k]);
            }
        }
        i = j;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// CPU profile summarizer (read_profiles.py)
// ═══════════════════════════════════════════════════════════════════════════

namespace {

struct rd_cpu_node {
    int64_t node_id = 0;
    int64_t parent_id = -1; // -1 == none
    kimix::string function_name;
    kimix::string url;
    int64_t line_number = -1;
    bool has_line = false;
    int64_t hit_count = 0;
    bool has_hit_count = false;
    int64_t self_micros = 0;
    int64_t total_micros = 0;
    kimix::vector<size_t> children; // indices into the node arena
};

// label(): name or "(anonymous)"; "name (url:line)" when url and line >= 0.
kimix::string rd_cpu_label(const rd_cpu_node &node) {
    const kimix::string name =
        node.function_name.empty() ? kimix::string("(anonymous)") : node.function_name;
    if (!node.url.empty() && node.has_line && node.line_number >= 0) {
        kimix::StringScratch ss;
        ss << name << " (" << node.url << ":" << node.line_number << ")";
        return std::move(ss.string());
    }
    return name;
}

} // namespace

bool render_cpu_profile(kimix::string_view json_text, kimix::string &out) {
    out.clear();
    yyjson_read_err jerr;
    yyjson_doc *doc = yyjson_read_opts(
        const_cast<char *>(json_text.data()), json_text.size(),
        YYJSON_READ_NOFLAG, &kimix::llm::kYYJsonAlcMi, &jerr);
    if (doc == nullptr) {
        return false;
    }
    // RAII-style cleanup via a small guard struct (no exceptions needed).
    struct doc_guard {
        yyjson_doc *d;
        ~doc_guard() {
            if (d) {
                yyjson_doc_free(d);
            }
        }
    } guard{doc};

    yyjson_val *data = yyjson_doc_get_root(doc);
    if (yyjson_get_type(data) != YYJSON_TYPE_OBJ) {
        return false;
    }
    // Chrome DevTools wraps the profile in {"profile": {...}}.
    yyjson_val *profile = yyjson_obj_get(data, "profile");
    if (profile == nullptr) {
        profile = data;
    }
    if (yyjson_get_type(profile) != YYJSON_TYPE_OBJ) {
        return false;
    }
    yyjson_val *nodes = yyjson_obj_get(profile, "nodes");
    yyjson_val *samples = yyjson_obj_get(profile, "samples");
    yyjson_val *time_deltas = yyjson_obj_get(profile, "timeDeltas");
    yyjson_val *start_time_val = yyjson_obj_get(profile, "startTime");
    yyjson_val *end_time_val = yyjson_obj_get(profile, "endTime");
    if (yyjson_get_type(nodes) != YYJSON_TYPE_ARR ||
        yyjson_get_type(samples) != YYJSON_TYPE_ARR) {
        return false;
    }

    // ── Build the node arena ────────────────────────────────────────────────
    kimix::vector<rd_cpu_node> arena;
    kimix::unordered_map<int64_t, size_t> id_to_idx;
    {
        size_t idx;
        yyjson_val *n;
        yyjson_arr_iter it;
        yyjson_arr_iter_init(nodes, &it);
        while ((n = yyjson_arr_iter_next(&it)) != nullptr) {
            if (yyjson_get_type(n) != YYJSON_TYPE_OBJ) {
                continue;
            }
            yyjson_val *id_val = yyjson_obj_get(n, "id");
            if (id_val == nullptr || !yyjson_is_int(id_val)) {
                continue;
            }
            rd_cpu_node node;
            node.node_id = yyjson_get_int(id_val);
            yyjson_val *parent_val = yyjson_obj_get(n, "parent");
            if (parent_val != nullptr && yyjson_is_int(parent_val)) {
                node.parent_id = yyjson_get_int(parent_val);
            }
            yyjson_val *cf = yyjson_obj_get(n, "callFrame");
            if (cf != nullptr && yyjson_get_type(cf) == YYJSON_TYPE_OBJ) {
                yyjson_val *fn = yyjson_obj_get(cf, "functionName");
                if (fn != nullptr && yyjson_is_str(fn)) {
                    node.function_name.assign(yyjson_get_str(fn),
                                              yyjson_get_len(fn));
                }
                yyjson_val *url = yyjson_obj_get(cf, "url");
                if (url != nullptr && yyjson_is_str(url)) {
                    node.url.assign(yyjson_get_str(url), yyjson_get_len(url));
                }
                yyjson_val *ln = yyjson_obj_get(cf, "lineNumber");
                if (ln != nullptr && yyjson_is_int(ln)) {
                    node.line_number = yyjson_get_int(ln);
                    node.has_line = true;
                }
            }
            yyjson_val *hit = yyjson_obj_get(n, "hitCount");
            if (hit != nullptr && yyjson_is_int(hit)) {
                node.hit_count = yyjson_get_int(hit);
                node.has_hit_count = true;
            }
            id_to_idx.emplace(node.node_id, arena.size());
            arena.push_back(std::move(node));
            (void)idx;
        }
    }
    if (arena.empty()) {
        return false;
    }
    // Link children via parent ids.
    for (size_t i = 0; i < arena.size(); i++) {
        const int64_t parent_id = arena[i].parent_id;
        if (parent_id >= 0) {
            const auto it = id_to_idx.find(parent_id);
            if (it != id_to_idx.end()) {
                arena[it->second].children.push_back(i);
            }
        }
    }

    const size_t sample_count_json = yyjson_arr_size(samples);

    // ── Self times (_compute_self_times) ────────────────────────────────────
    int64_t total_micros = 0;
    kimix::vector<int64_t> positive_deltas;
    const bool deltas_match =
        time_deltas != nullptr && yyjson_get_type(time_deltas) == YYJSON_TYPE_ARR &&
        yyjson_arr_size(time_deltas) == sample_count_json;
    if (deltas_match) {
        yyjson_val *d;
        yyjson_arr_iter it;
        yyjson_arr_iter_init(time_deltas, &it);
        while ((d = yyjson_arr_iter_next(&it)) != nullptr) {
            if (yyjson_is_num(d)) {
                const double dv = yyjson_get_num(d);
                if (dv > 0) {
                    positive_deltas.push_back(static_cast<int64_t>(dv));
                    total_micros += static_cast<int64_t>(dv);
                }
            }
        }
    } else {
        // Fall back to duration from start/end.
        int64_t start = 0;
        int64_t end = 0;
        if (start_time_val != nullptr && yyjson_is_num(start_time_val)) {
            start = static_cast<int64_t>(yyjson_get_num(start_time_val));
        }
        if (end_time_val != nullptr && yyjson_is_num(end_time_val)) {
            end = static_cast<int64_t>(yyjson_get_num(end_time_val));
        }
        const int64_t duration = std::max<int64_t>(0, end - start);
        total_micros = duration;
        const size_t n_samples = std::max<size_t>(1, sample_count_json);
        positive_deltas.assign(sample_count_json,
                               static_cast<int64_t>(duration / static_cast<int64_t>(n_samples)));
    }
    const size_t denom = !positive_deltas.empty() ? positive_deltas.size()
                                                  : sample_count_json;
    int64_t avg_interval = total_micros / static_cast<int64_t>(std::max<size_t>(1, denom));

    // hitCount fallback: nodes carry hitCount but no samples/timeDeltas.
    bool used_hit_count = false;
    if (sample_count_json == 0 && !arena.empty()) {
        bool all_have_hits = true;
        for (const auto &node : arena) {
            if (!node.has_hit_count) {
                all_have_hits = false;
                break;
            }
        }
        if (all_have_hits) {
            used_hit_count = true;
            for (auto &node : arena) {
                node.self_micros = node.hit_count * avg_interval;
                total_micros += node.self_micros;
            }
        }
    }
    if (!used_hit_count) {
        yyjson_val *s;
        yyjson_arr_iter it;
        yyjson_arr_iter_init(samples, &it);
        while ((s = yyjson_arr_iter_next(&it)) != nullptr) {
            if (yyjson_is_int(s)) {
                const auto found = id_to_idx.find(yyjson_get_int(s));
                if (found != id_to_idx.end()) {
                    arena[found->second].self_micros += avg_interval;
                    total_micros += avg_interval;
                }
            }
        }
    }

    // ── Aggregate totals (post-order over roots) ────────────────────────────
    {
        kimix::vector<bool> is_child(arena.size(), false);
        for (const auto &node : arena) {
            for (const size_t c : node.children) {
                is_child[c] = true;
            }
        }
        kimix::vector<size_t> roots;
        for (size_t i = 0; i < arena.size(); i++) {
            if (!is_child[i]) {
                roots.push_back(i);
            }
        }
        // Iterative post-order walk.
        for (const size_t root_idx : roots) {
            kimix::vector<std::pair<size_t, size_t>> stack; // (node, next child)
            stack.push_back({root_idx, 0});
            while (!stack.empty()) {
                auto &top = stack.back();
                const rd_cpu_node &node = arena[top.first];
                if (top.second < node.children.size()) {
                    const size_t child = node.children[top.second++];
                    stack.push_back({child, 0});
                } else {
                    int64_t total = node.self_micros;
                    for (const size_t c : node.children) {
                        total += arena[c].total_micros;
                    }
                    arena[top.first].total_micros = total;
                    stack.pop_back();
                }
            }
        }
    }

    // ── Header numbers ──────────────────────────────────────────────────────
    int64_t start = 0;
    int64_t end = 0;
    if (start_time_val != nullptr && yyjson_is_num(start_time_val)) {
        start = static_cast<int64_t>(yyjson_get_num(start_time_val));
    }
    if (end_time_val != nullptr && yyjson_is_num(end_time_val)) {
        end = static_cast<int64_t>(yyjson_get_num(end_time_val));
    }
    int64_t wall_micros = std::max<int64_t>(0, end - start);
    if (wall_micros == 0) {
        wall_micros = total_micros;
    }
    int64_t sample_count = static_cast<int64_t>(sample_count_json);
    if (sample_count == 0) {
        for (const auto &node : arena) {
            if (node.has_hit_count) {
                sample_count += node.hit_count;
            }
        }
    }
    avg_interval = total_micros / std::max<int64_t>(1, sample_count);
    const int64_t threshold =
        std::max<int64_t>(3 * avg_interval,
                          static_cast<int64_t>(static_cast<double>(total_micros) * 0.02));

    // ── Root promotion ──────────────────────────────────────────────────────
    size_t root_idx = SIZE_MAX;
    yyjson_val *root_val = yyjson_obj_get(profile, "root");
    if (root_val != nullptr && yyjson_is_int(root_val)) {
        const auto found = id_to_idx.find(yyjson_get_int(root_val));
        if (found != id_to_idx.end()) {
            root_idx = found->second;
        }
    } else {
        for (size_t i = 0; i < arena.size(); i++) {
            if (arena[i].function_name == "(root)") {
                root_idx = i;
                break;
            }
        }
        if (root_idx == SIZE_MAX && !arena.empty()) {
            root_idx = 0;
        }
    }

    // ── Top functions by self time (excluding "(idle)") ─────────────────────
    kimix::vector<size_t> self_nodes;
    for (size_t i = 0; i < arena.size(); i++) {
        if (arena[i].self_micros > 0 && arena[i].function_name != "(idle)") {
            self_nodes.push_back(i);
        }
    }
    std::sort(self_nodes.begin(), self_nodes.end(), [&arena](size_t a, size_t b) {
        if (arena[a].self_micros != arena[b].self_micros) {
            return arena[a].self_micros > arena[b].self_micros;
        }
        return a < b;
    });

    kimix::vector<kimix::string> lines;
    {
        kimix::StringScratch ss;
        ss << "V8 CPU profile: " << wall_micros
           << "\xCE\xBCs wall clock, " // μs
           << sample_count << " samples (avg interval " << avg_interval
           << "\xCE\xBCs)";
        lines.push_back(std::move(ss.string()));
    }
    lines.push_back("");
    lines.push_back("## Hot paths");

    // Recursive hot-tree pruning (_prune_hot_tree), iterative with an explicit
    // stack to avoid deep recursion on large profiles.
    if (root_idx != SIZE_MAX) {
        kimix::vector<size_t> child_order = arena[root_idx].children;
        std::sort(child_order.begin(), child_order.end(),
                  [&arena](size_t a, size_t b) {
                      if (arena[a].total_micros != arena[b].total_micros) {
                          return arena[a].total_micros > arena[b].total_micros;
                      }
                      return a < b;
                  });
        // Stack of (node_idx, depth, child-position list) — DFS, children are
        // pre-sorted so output order matches the reference.
        struct frame {
            size_t node_idx;
            int depth;
        };
        kimix::vector<frame> stack;
        for (auto it = child_order.rbegin(); it != child_order.rend(); ++it) {
            // The reference filters root children by threshold before recursing
            // (_prune_hot_tree is only entered for children >= threshold).
            if (arena[*it].total_micros >= threshold) {
                stack.push_back({*it, 0});
            }
        }
        while (!stack.empty()) {
            const frame f = stack.back();
            stack.pop_back();
            const rd_cpu_node &node = arena[f.node_idx];
            if (f.depth > 8) {
                continue;
            }
            if (node.total_micros < threshold && f.depth > 0) {
                continue;
            }
            kimix::string indent(static_cast<size_t>(f.depth) * 2, ' ');
            lines.push_back(indent + rd_cpu_label(node));
            kimix::vector<size_t> kids = node.children;
            std::sort(kids.begin(), kids.end(), [&arena](size_t a, size_t b) {
                if (arena[a].total_micros != arena[b].total_micros) {
                    return arena[a].total_micros > arena[b].total_micros;
                }
                return a < b;
            });
            for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
                if (arena[*it].total_micros >= threshold) {
                    stack.push_back({*it, f.depth + 1});
                }
            }
        }
    } else {
        // No root: top 20 by total time from self_nodes ordering is not what
        // the reference does — it re-sorts by total_micros.
        kimix::vector<size_t> by_total = self_nodes;
        std::sort(by_total.begin(), by_total.end(), [&arena](size_t a, size_t b) {
            if (arena[a].total_micros != arena[b].total_micros) {
                return arena[a].total_micros > arena[b].total_micros;
            }
            return a < b;
        });
        const size_t take = std::min<size_t>(20, by_total.size());
        for (size_t i = 0; i < take; i++) {
            lines.push_back(rd_cpu_label(arena[by_total[i]]));
        }
    }
    lines.push_back("");
    lines.push_back("## Top functions by self time");
    {
        const size_t take = std::min<size_t>(20, self_nodes.size());
        for (size_t i = 0; i < take; i++) {
            const rd_cpu_node &node = arena[self_nodes[i]];
            const double pct =
                static_cast<double>(node.self_micros) /
                static_cast<double>(std::max<int64_t>(1, total_micros)) * 100.0;
            kimix::StringScratch ss;
            ss << (i + 1) << ". " << rd_cpu_label(node) << " \xE2\x80\x94 "
               << node.self_micros << "\xCE\xBCs (" << rd_fmt_f(pct, 2) << "%)";
            lines.push_back(std::move(ss.string()));
        }
    }
    lines.push_back("");
    lines.push_back("[Summarized view of CPU profile. Use profile_raw=True to "
                    "read the original JSON.]");

    kimix::StringScratch joined;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i != 0) {
            joined << "\n";
        }
        joined << lines[i];
    }
    out = std::move(joined.string());
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// macOS sample profile parser (read_profiles.py 263-430)
// ═══════════════════════════════════════════════════════════════════════════

namespace {

constexpr const char *rd_wait_symbols[] = {
    "_pthread_cond_wait", "__psynch_cvwait", "__semwait_signal", "mach_msg_trap",
    "__workq_kernreturn", "__ulock_wait", "__recvfrom", "__select", "__poll",
    "kevent", "epoll_wait", "poll", "select", "nanosleep", "usleep", "sleep",
};

bool rd_is_wait_frame(kimix::string_view symbol) {
    for (const char *w : rd_wait_symbols) {
        if (symbol.find(w) != kimix::string_view::npos) {
            return true;
        }
    }
    return false;
}

// Parse "symbol (in module)" or "symbol + 123" (_parse_frame_text).
void rd_parse_frame_text(kimix::string_view text, kimix::string &symbol,
                         kimix::string &module) {
    // strip()
    size_t b = 0;
    size_t e = text.size();
    while (b < e && rd_is_space_cp(static_cast<uint8_t>(text[b])) &&
           static_cast<uint8_t>(text[b]) < 0x80) {
        b++;
    }
    while (e > b && rd_is_space_cp(static_cast<uint8_t>(text[e - 1])) &&
           static_cast<uint8_t>(text[e - 1]) < 0x80) {
        e--;
    }
    text = text.substr(b, e - b);
    // ^(.*?)\s+\(in\s+(.+)\)\s*(?:\+.*)?$
    const size_t in_pos = text.find(" (in ");
    if (in_pos != kimix::string_view::npos) {
        size_t close = text.find(')', in_pos + 5);
        if (close != kimix::string_view::npos) {
            // optional trailing "+..." after the closing paren — allow anything
            size_t rest = close + 1;
            while (rest < text.size() && text[rest] == ' ') {
                rest++;
            }
            const bool ok_tail = rest >= text.size() || text[rest] == '+';
            if (ok_tail) {
                kimix::string_view sym = text.substr(0, in_pos);
                kimix::string_view mod = text.substr(in_pos + 5, close - in_pos - 5);
                // strip both
                while (!sym.empty() && (sym.back() == ' ' || sym.back() == '\t')) {
                    sym.remove_suffix(1);
                }
                while (!mod.empty() && (mod.front() == ' ' || mod.front() == '\t')) {
                    mod.remove_prefix(1);
                }
                while (!mod.empty() && (mod.back() == ' ' || mod.back() == '\t')) {
                    mod.remove_suffix(1);
                }
                symbol.assign(sym.data(), sym.size());
                module.assign(mod.data(), mod.size());
                return;
            }
        }
    }
    // ^(.*?)\s*\+.*$
    const size_t plus = text.find('+');
    if (plus != kimix::string_view::npos) {
        kimix::string_view sym = text.substr(0, plus);
        while (!sym.empty() && (sym.back() == ' ' || sym.back() == '\t')) {
            sym.remove_suffix(1);
        }
        symbol.assign(sym.data(), sym.size());
        module.clear();
        return;
    }
    symbol.assign(text.data(), text.size());
    module.clear();
}

// Best-effort demangle (_demangle_symbol).
kimix::string rd_demangle_symbol(kimix::string_view symbol) {
    if (symbol.starts_with("_R") && symbol.size() > 2) {
        kimix::string s(symbol.substr(0, std::min<size_t>(10, symbol.size())));
        s += "...";
        return s;
    }
    if (symbol.starts_with("_ZN") && symbol.ends_with("E")) {
        const kimix::string_view inner = symbol.substr(3, symbol.size() - 4);
        kimix::vector<kimix::string> parts;
        size_t i = 0;
        while (i < inner.size()) {
            size_t j = i;
            while (j < inner.size() && inner[j] >= '0' && inner[j] <= '9') {
                j++;
            }
            if (j == i) {
                break;
            }
            size_t length = 0;
            for (size_t k = i; k < j; k++) {
                length = length * 10 + static_cast<size_t>(inner[k] - '0');
            }
            i = j;
            if (i + length > inner.size()) {
                length = inner.size() - i; // Python slice clamps
            }
            parts.emplace_back(inner.data() + i, length);
            i += length;
        }
        if (!parts.empty()) {
            kimix::StringScratch ss;
            for (size_t k = 0; k < parts.size(); k++) {
                if (k != 0) {
                    ss << "::";
                }
                ss << parts[k];
            }
            return std::move(ss.string());
        }
    }
    return kimix::string(symbol);
}

} // namespace

bool render_sample_profile(kimix::string_view text, kimix::string &out) {
    out.clear();
    const kimix::vector<kimix::string> lines = split_lines(text);
    if (lines.empty()) {
        return false;
    }
    // Recognize the macOS sample preamble (first 20 lines).
    bool preamble_match = false;
    bool call_graph_started = false;
    {
        const size_t probe = std::min<size_t>(20, lines.size());
        for (size_t i = 0; i < probe; i++) {
            if (lines[i].find("Sampling process") != kimix::string::npos ||
                lines[i].find("Analysis of sampling") != kimix::string::npos) {
                preamble_match = true;
            }
            if (lines[i].find("Call graph:") != kimix::string::npos) {
                call_graph_started = true;
            }
        }
    }
    if (!preamble_match && !call_graph_started) {
        return false;
    }

    // threads: name -> list of stacks (each stack = list of frame raw texts).
    kimix::vector<kimix::string> thread_names;
    kimix::vector<kimix::vector<kimix::vector<kimix::string>>> thread_stacks;
    auto thread_index = [&](kimix::string_view name) -> size_t {
        for (size_t i = 0; i < thread_names.size(); i++) {
            if (thread_names[i] == name) {
                return i;
            }
        }
        thread_names.emplace_back(name);
        thread_stacks.emplace_back();
        return thread_names.size() - 1;
    };

    bool have_thread = false;
    kimix::string current_thread;
    kimix::vector<kimix::string> current_stack;

    for (const auto &raw_line : lines) {
        // stripped = line.strip()
        kimix::string_view stripped = raw_line;
        while (!stripped.empty() &&
               (stripped.front() == ' ' || stripped.front() == '\t' ||
                stripped.front() == '\r' || stripped.front() == '\n')) {
            stripped.remove_prefix(1);
        }
        while (!stripped.empty() &&
               (stripped.back() == ' ' || stripped.back() == '\t' ||
                stripped.back() == '\r' || stripped.back() == '\n')) {
            stripped.remove_suffix(1);
        }
        if (stripped.starts_with("Thread_")) {
            if (have_thread && !current_stack.empty()) {
                thread_stacks[thread_index(current_thread)].push_back(current_stack);
            }
            // current_thread = stripped.split()[0]
            size_t sp = 0;
            while (sp < stripped.size() && stripped[sp] != ' ' &&
                   stripped[sp] != '\t') {
                sp++;
            }
            current_thread.assign(stripped.data(), sp);
            have_thread = true;
            current_stack.clear();
            continue;
        }
        if (stripped == "Call graph:") {
            continue;
        }
        // decorator regex ^(\s*)([+|!:])(\s*)(.*)$
        size_t i = 0;
        size_t indent = 0;
        while (i < raw_line.size() &&
               (raw_line[i] == ' ' || raw_line[i] == '\t')) {
            i++;
            indent++;
        }
        if (i >= raw_line.size() ||
            (raw_line[i] != '+' && raw_line[i] != '|' && raw_line[i] != '!' &&
             raw_line[i] != ':')) {
            continue;
        }
        i++; // skip decorator char
        while (i < raw_line.size() &&
               (raw_line[i] == ' ' || raw_line[i] == '\t')) {
            i++;
            indent++;
        }
        kimix::string_view frame_text(raw_line.data() + i, raw_line.size() - i);
        while (!frame_text.empty() &&
               (frame_text.back() == '\r' || frame_text.back() == '\n' ||
                frame_text.back() == ' ' || frame_text.back() == '\t')) {
            frame_text.remove_suffix(1);
        }
        if (frame_text.empty()) {
            continue;
        }
        const kimix::string frame(frame_text);
        const size_t depth = indent / 2;
        if (depth >= current_stack.size()) {
            current_stack.push_back(frame);
        } else {
            current_stack.resize(depth);
            current_stack.push_back(frame);
        }
        if (!have_thread) {
            current_thread = "Thread_0";
            have_thread = true;
        }
        thread_stacks[thread_index(current_thread)].push_back(current_stack);
    }
    if (have_thread && !current_stack.empty()) {
        thread_stacks[thread_index(current_thread)].push_back(current_stack);
    }

    // Self counts at each recorded stack's leaf.
    kimix::vector<kimix::string> symbols;
    kimix::vector<int64_t> counts;
    auto bump = [&](kimix::string_view symbol) {
        for (size_t i = 0; i < symbols.size(); i++) {
            if (symbols[i] == symbol) {
                counts[i]++;
                return;
            }
        }
        symbols.emplace_back(symbol);
        counts.push_back(1);
    };
    for (size_t t = 0; t < thread_stacks.size(); t++) {
        for (const auto &stack : thread_stacks[t]) {
            if (stack.empty()) {
                continue;
            }
            kimix::string symbol, module;
            rd_parse_frame_text(stack.back(), symbol, module);
            if (symbol.empty()) {
                symbol = stack.back();
            }
            bump(rd_demangle_symbol(symbol));
        }
    }
    int64_t total_samples = 0;
    for (const int64_t c : counts) {
        total_samples += c;
    }
    if (total_samples == 0) {
        return false;
    }

    // Exclude idle/wait frames from the top ranking (keep them for idle %).
    kimix::vector<size_t> active;
    for (size_t i = 0; i < symbols.size(); i++) {
        if (!rd_is_wait_frame(symbols[i])) {
            active.push_back(i);
        }
    }
    if (active.empty()) {
        for (size_t i = 0; i < symbols.size(); i++) {
            active.push_back(i);
        }
    }
    std::sort(active.begin(), active.end(), [&counts](size_t a, size_t b) {
        if (counts[a] != counts[b]) {
            return counts[a] > counts[b];
        }
        return a < b;
    });
    int64_t idle_total = 0;
    for (size_t i = 0; i < symbols.size(); i++) {
        if (rd_is_wait_frame(symbols[i])) {
            idle_total += counts[i];
        }
    }
    const double idle_pct =
        static_cast<double>(idle_total) / static_cast<double>(total_samples) * 100.0;

    kimix::vector<kimix::string> out_lines;
    {
        kimix::StringScratch ss;
        ss << "macOS sample profile: " << total_samples << " samples across "
           << thread_names.size() << " thread(s), " << rd_fmt_f(idle_pct, 1)
           << "% in wait/idle frames";
        out_lines.push_back(std::move(ss.string()));
    }
    out_lines.push_back("");
    out_lines.push_back("## Top functions by self samples");
    {
        const size_t take = std::min<size_t>(20, active.size());
        for (size_t i = 0; i < take; i++) {
            const size_t idx = active[i];
            const double pct = static_cast<double>(counts[idx]) /
                               static_cast<double>(total_samples) * 100.0;
            kimix::StringScratch ss;
            ss << (i + 1) << ". " << symbols[idx] << " \xE2\x80\x94 "
               << counts[idx] << " self samples (" << rd_fmt_f(pct, 2) << "%)";
            out_lines.push_back(std::move(ss.string()));
        }
    }
    out_lines.push_back("");
    out_lines.push_back("[Summarized view of sample profile. Use profile_raw=True "
                        "to read the original text.]");

    kimix::StringScratch joined;
    for (size_t i = 0; i < out_lines.size(); i++) {
        if (i != 0) {
            joined << "\n";
        }
        joined << out_lines[i];
    }
    out = std::move(joined.string());
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// markdown_to_text (read_markit.py 215-254)
// ═══════════════════════════════════════════════════════════════════════════

kimix::string markdown_to_text(kimix::string_view md) {
    kimix::string text(md.data(), md.size());

    // 1. Fenced code blocks -> "[code block: N lines]" where N is the number
    //    of '\n' chars inside the matched "```...```" region (non-greedy).
    {
        kimix::string out;
        out.reserve(text.size());
        size_t pos = 0;
        while (true) {
            const size_t open = text.find("```", pos);
            if (open == kimix::string::npos) {
                out.append(text.data() + pos, text.size() - pos);
                break;
            }
            const size_t close = text.find("```", open + 3);
            if (close == kimix::string::npos) {
                // No closing fence: regex [\s\S]*?``` needs a match, so the
                // remainder stays untouched.
                out.append(text.data() + pos, text.size() - pos);
                break;
            }
            const size_t match_end = close + 3;
            out.append(text.data() + pos, open - pos);
            size_t nl_count = 0;
            for (size_t i = open; i < match_end; i++) {
                if (text[i] == '\n') {
                    nl_count++;
                }
            }
            kimix::StringScratch ss;
            ss << "[code block: " << nl_count << " lines]";
            out.append(ss.string());
            pos = match_end;
        }
        text = std::move(out);
    }

    // 2. Inline code -> placeholders so later passes never rewrite code.
    kimix::vector<kimix::string> inline_code;
    {
        kimix::string out;
        out.reserve(text.size());
        size_t pos = 0;
        while (pos < text.size()) {
            const size_t open = text.find('`', pos);
            if (open == kimix::string::npos) {
                out.append(text.data() + pos, text.size() - pos);
                break;
            }
            size_t close = open + 1;
            while (close < text.size() && text[close] != '`') {
                close++;
            }
            if (close >= text.size() || close == open + 1) {
                // `([^`]+)` requires at least one non-backtick char.
                out.push_back(text[open]);
                pos = open + 1;
                continue;
            }
            out.append(text.data() + pos, open - pos);
            inline_code.emplace_back(text.data() + open + 1, close - open - 1);
            kimix::StringScratch ss;
            ss << "\x00CODE" << (inline_code.size() - 1) << "\x00";
            out.append(ss.string());
            pos = close + 1;
        }
        text = std::move(out);
    }

    // 3. Bold: \*\*([^*]+)\*\* -> \1
    {
        kimix::string out;
        out.reserve(text.size());
        size_t pos = 0;
        while (true) {
            const size_t open = text.find("**", pos);
            if (open == kimix::string::npos) {
                out.append(text.data() + pos, text.size() - pos);
                break;
            }
            size_t i = open + 2;
            const size_t content_start = i;
            while (i < text.size() && text[i] != '*') {
                i++;
            }
            if (i + 1 < text.size() && text[i + 1] == '*' && i > content_start) {
                out.append(text.data() + pos, open - pos);
                out.append(text.data() + content_start, i - content_start);
                pos = i + 2;
            } else {
                out.append(text.data() + pos, open + 2 - pos);
                pos = open + 2;
            }
        }
        text = std::move(out);
    }

    // 4. Italic: \*([^*]+)\* -> \1
    {
        kimix::string out;
        out.reserve(text.size());
        size_t pos = 0;
        while (true) {
            const size_t open = text.find('*', pos);
            if (open == kimix::string::npos) {
                out.append(text.data() + pos, text.size() - pos);
                break;
            }
            size_t i = open + 1;
            const size_t content_start = i;
            while (i < text.size() && text[i] != '*') {
                i++;
            }
            if (i < text.size() && i > content_start) {
                out.append(text.data() + pos, open - pos);
                out.append(text.data() + content_start, i - content_start);
                pos = i + 1;
            } else {
                out.append(text.data() + pos, open + 1 - pos);
                pos = open + 1;
            }
        }
        text = std::move(out);
    }

    // 5. Double-underscore bold: __([^_]+)__ -> \1
    {
        kimix::string out;
        out.reserve(text.size());
        size_t pos = 0;
        while (true) {
            const size_t open = text.find("__", pos);
            if (open == kimix::string::npos) {
                out.append(text.data() + pos, text.size() - pos);
                break;
            }
            size_t i = open + 2;
            const size_t content_start = i;
            while (i < text.size() && text[i] != '_') {
                i++;
            }
            if (i + 1 < text.size() && text[i + 1] == '_' && i > content_start) {
                out.append(text.data() + pos, open - pos);
                out.append(text.data() + content_start, i - content_start);
                pos = i + 2;
            } else {
                out.append(text.data() + pos, open + 2 - pos);
                pos = open + 2;
            }
        }
        text = std::move(out);
    }

    // 6. Underscore italic with word-boundary guards:
    //    (?<!\w)_([^_\n]+)_(?!\w) -> \1
    {
        kimix::string out;
        out.reserve(text.size());
        size_t pos = 0;
        // Code-point aware walk to evaluate \w lookarounds.
        auto cp_at_before = [&](size_t byte_pos) -> uint32_t {
            // decode the code point ending at byte_pos
            if (byte_pos == 0) {
                return 0xFFFFFFFFu; // "no char" -> \w false
            }
            // walk back up to 4 bytes to find the lead byte
            size_t start = byte_pos;
            for (size_t k = 0; k < 4 && start > 0; k++) {
                start--;
                const uint8_t b = static_cast<uint8_t>(text[start]);
                if ((b & 0xC0u) != 0x80u) {
                    break;
                }
            }
            const char *it = text.data() + start;
            return rd_decode(it, text.data() + byte_pos);
        };
        while (pos < text.size()) {
            const size_t open = text.find('_', pos);
            if (open == kimix::string::npos) {
                out.append(text.data() + pos, text.size() - pos);
                break;
            }
            const uint32_t before = cp_at_before(open);
            if (before != 0xFFFFFFFFu && rd_is_word_cp(before)) {
                out.append(text.data() + pos, open + 1 - pos);
                pos = open + 1;
                continue;
            }
            size_t i = open + 1;
            const size_t content_start = i;
            bool valid = false;
            while (i < text.size()) {
                if (text[i] == '_') {
                    valid = true;
                    break;
                }
                if (text[i] == '\n') {
                    break;
                }
                i++;
            }
            if (!valid || i == content_start) {
                out.append(text.data() + pos, open + 1 - pos);
                pos = open + 1;
                continue;
            }
            // (?!\w): decode the code point starting at i+1
            const char *it = text.data() + i + 1;
            const char *end_ptr = text.data() + text.size();
            const bool after_is_word = it < end_ptr && rd_is_word_cp(rd_decode(it, end_ptr));
            if (after_is_word) {
                out.append(text.data() + pos, open + 1 - pos);
                pos = open + 1;
                continue;
            }
            out.append(text.data() + pos, open - pos);
            out.append(text.data() + content_start, i - content_start);
            pos = i + 1;
        }
        text = std::move(out);
    }

    // 7. Links: \[([^\]]+)\]\(([^)]+)\) -> "\1 (\2)"
    //    (runs before the image pass would hide images; the reference applies
    //    the link regex first, which consumes image alt-text too — the image
    //    pass below only sees what remains, matching Python order.)
    {
        kimix::string out;
        out.reserve(text.size());
        size_t pos = 0;
        while (pos < text.size()) {
            const size_t open = text.find('[', pos);
            if (open == kimix::string::npos) {
                out.append(text.data() + pos, text.size() - pos);
                break;
            }
            size_t close = open + 1;
            while (close < text.size() && text[close] != ']') {
                close++;
            }
            if (close >= text.size() || close == open + 1 ||
                close + 1 >= text.size() || text[close + 1] != '(') {
                out.append(text.data() + pos, open + 1 - pos);
                pos = open + 1;
                continue;
            }
            size_t paren_end = close + 2;
            while (paren_end < text.size() && text[paren_end] != ')') {
                paren_end++;
            }
            if (paren_end >= text.size()) {
                out.append(text.data() + pos, open + 1 - pos);
                pos = open + 1;
                continue;
            }
            out.append(text.data() + pos, open - pos);
            out.append(text.data() + open + 1, close - open - 1);
            out.append(" (", 2);
            out.append(text.data() + close + 2, paren_end - close - 2);
            out.push_back(')');
            pos = paren_end + 1;
        }
        text = std::move(out);
    }

    // 8. Images: !\[([^\]]*)\]\(([^)]+)\) -> "[image: \2]"
    {
        kimix::string out;
        out.reserve(text.size());
        size_t pos = 0;
        while (pos < text.size()) {
            const size_t open = text.find("![", pos);
            if (open == kimix::string::npos) {
                out.append(text.data() + pos, text.size() - pos);
                break;
            }
            size_t close = open + 2;
            while (close < text.size() && text[close] != ']') {
                close++;
            }
            if (close >= text.size() || close + 1 >= text.size() ||
                text[close + 1] != '(') {
                out.append(text.data() + pos, open + 2 - pos);
                pos = open + 2;
                continue;
            }
            size_t paren_end = close + 2;
            while (paren_end < text.size() && text[paren_end] != ')') {
                paren_end++;
            }
            if (paren_end >= text.size()) {
                out.append(text.data() + pos, open + 2 - pos);
                pos = open + 2;
                continue;
            }
            out.append(text.data() + pos, open - pos);
            out.append("[image: ", 8);
            out.append(text.data() + close + 2, paren_end - close - 2);
            out.push_back(']');
            pos = paren_end + 1;
        }
        text = std::move(out);
    }

    // 9. Heading markers: ^#+\s*(.+)$  (MULTILINE) -> \1
    {
        kimix::string out;
        out.reserve(text.size());
        size_t pos = 0;
        const size_t n = text.size();
        while (pos < n) {
            const size_t line_end = text.find('\n', pos);
            const size_t end = line_end == kimix::string::npos ? n : line_end;
            size_t i = pos;
            while (i < end && text[i] == '#') {
                i++;
            }
            if (i > pos) {
                // \s* (ASCII whitespace in practice; use Python str.isspace)
                size_t j = i;
                while (j < end) {
                    const char *it = text.data() + j;
                    const char *ep = text.data() + end;
                    const char *before = it;
                    const uint32_t cp = rd_decode(it, ep);
                    if (!rd_is_space_cp(cp)) {
                        break;
                    }
                    j += static_cast<size_t>(it - before);
                }
                if (j < end) {
                    out.append(text.data() + j, end - j);
                }
                // else: line was only "#" + ws -> replaced with empty
            } else {
                out.append(text.data() + pos, end - pos);
            }
            if (line_end != kimix::string::npos) {
                out.push_back('\n');
                pos = line_end + 1;
            } else {
                pos = n;
            }
        }
        text = std::move(out);
    }

    // 10. Horizontal rules: ^---+$ (MULTILINE) -> ""
    {
        kimix::string out;
        out.reserve(text.size());
        size_t pos = 0;
        const size_t n = text.size();
        while (pos < n) {
            const size_t line_end = text.find('\n', pos);
            const size_t end = line_end == kimix::string::npos ? n : line_end;
            bool is_rule = end > pos;
            for (size_t i = pos; i < end; i++) {
                if (text[i] != '-') {
                    is_rule = false;
                    break;
                }
            }
            if (is_rule && end - pos >= 3) {
                // replaced with empty string; keep the newline
            } else {
                out.append(text.data() + pos, end - pos);
            }
            if (line_end != kimix::string::npos) {
                out.push_back('\n');
                pos = line_end + 1;
            } else {
                pos = n;
            }
        }
        text = std::move(out);
    }

    // 11. Restore inline code placeholders.
    for (size_t i = 0; i < inline_code.size(); i++) {
        kimix::StringScratch ss;
        ss << "\x00CODE" << i << "\x00";
        const kimix::string token = std::move(ss.string());
        size_t pos = 0;
        while ((pos = text.find(token, pos)) != kimix::string::npos) {
            text.replace(pos, token.size(), inline_code[i]);
            pos += inline_code[i].size();
        }
    }

    // 12. Collapse blank runs: \n{3,} -> \n\n
    {
        kimix::string out;
        out.reserve(text.size());
        size_t i = 0;
        const size_t n = text.size();
        while (i < n) {
            if (text[i] == '\n') {
                size_t run = 0;
                while (i < n && text[i] == '\n') {
                    run++;
                    i++;
                }
                out.push_back('\n');
                if (run >= 2) {
                    out.push_back('\n');
                }
            } else {
                out.push_back(text[i]);
                i++;
            }
        }
        text = std::move(out);
    }

    // 13. strip() both ends (Python str.strip: Unicode whitespace).
    {
        size_t b = 0;
        size_t e = text.size();
        while (b < e) {
            const char *it = text.data() + b;
            const char *before = it;
            const uint32_t cp = rd_decode(it, text.data() + e);
            if (!rd_is_space_cp(cp)) {
                break;
            }
            b += static_cast<size_t>(it - before);
        }
        // strip from end: walk backwards over whitespace code points
        while (e > b) {
            // find the start of the last code point
            size_t start = e;
            for (size_t k = 0; k < 4 && start > b; k++) {
                start--;
                const uint8_t byte = static_cast<uint8_t>(text[start]);
                if ((byte & 0xC0u) != 0x80u) {
                    break;
                }
            }
            const char *it = text.data() + start;
            const uint32_t cp = rd_decode(it, text.data() + e);
            if (!rd_is_space_cp(cp)) {
                break;
            }
            e = start;
        }
        return text.substr(b, e - b);
    }
}

namespace {

void rd_serialize_status(kimix::builtin_tools::ToolParams &result,
                         kimix::string_view status, kimix::string_view message,
                         kimix::vector<char> &out) {
    result.values["status"] = ValueElement::make_string(kimix::string(status));
    result.values["message"] = ValueElement::make_string(kimix::string(message));
    result.values["brief"] = ValueElement::make_string(kimix::string("Read file"));
    result.values["output"] = ValueElement::make_string(kimix::string());
    result.serialize(out);
}

} // namespace

Read::Read(kimix::builtin_tools::Session *session)
    : kimix::builtin_tools::Tool(session) {}

void Read::operator()(kimix::builtin_tools::ToolParams const *parameters) {
    _result.clear();
    kimix::builtin_tools::ToolParams result;
    if (parameters == nullptr) {
        rd_serialize_status(result, "invalid_input", "missing parameters", _result);
        return;
    }

    const ValueElement *content_el = parameters->get("content");
    if (content_el == nullptr || !content_el->is_string()) {
        rd_serialize_status(result, "invalid_input",
                            "missing required field: content", _result);
        return;
    }
    const kimix::string_view content = content_el->as_string();

    const ValueElement *display_el = parameters->get("display_path");
    if (display_el == nullptr || !display_el->is_string()) {
        rd_serialize_status(result, "invalid_input",
                            "missing required field: display_path", _result);
        return;
    }
    const kimix::string_view display_path = display_el->as_string();

    kimix::string mode = "text";
    if (const ValueElement *mode_el = parameters->get("mode");
        mode_el != nullptr && mode_el->is_string()) {
        mode = mode_el->as_string();
    }

    // Rich-format short-circuits: the Python binding pre-extracts bytes and
    // routes to the matching native kernel by setting mode.
    if (mode == "markdown") {
        kimix::string converted = markdown_to_text(content);
        result.values["status"] = ValueElement::make_string(kimix::string("ok"));
        result.values["output"] = ValueElement::make_string(std::move(converted));
        {
            kimix::StringScratch ss;
            ss << "Markdown converted to plain text. Path: " << display_path;
            result.values["message"] =
                ValueElement::make_string(std::move(ss.string()));
        }
        result.values["brief"] = ValueElement::make_string(kimix::string("Read file"));
        result.serialize(_result);
        return;
    }

    if (mode == "cpu_profile") {
        kimix::string summary;
        if (!render_cpu_profile(content, summary)) {
            rd_serialize_status(result, "unsupported",
                                "content is not a valid CPU profile", _result);
            return;
        }
        result.values["status"] = ValueElement::make_string(kimix::string("ok"));
        result.values["output"] = ValueElement::make_string(std::move(summary));
        {
            kimix::StringScratch ss;
            ss << "Profile summary. Path: " << display_path;
            result.values["message"] =
                ValueElement::make_string(std::move(ss.string()));
        }
        result.values["brief"] = ValueElement::make_string(kimix::string("Read file"));
        result.serialize(_result);
        return;
    }

    if (mode == "sample_profile") {
        kimix::string summary;
        if (!render_sample_profile(content, summary)) {
            rd_serialize_status(result, "unsupported",
                                "content is not a valid sample profile", _result);
            return;
        }
        result.values["status"] = ValueElement::make_string(kimix::string("ok"));
        result.values["output"] = ValueElement::make_string(std::move(summary));
        {
            kimix::StringScratch ss;
            ss << "Profile summary. Path: " << display_path;
            result.values["message"] =
                ValueElement::make_string(std::move(ss.string()));
        }
        result.values["brief"] = ValueElement::make_string(kimix::string("Read file"));
        result.serialize(_result);
        return;
    }

    // Default text mode: line-oriented read with budgets and char window.
    int64_t offset = 1;
    int64_t limit = 2000;
    int64_t max_char = 16000;
    int64_t char_offset = 0;
    bool show_line_numbers = true;
    kimix::string note;

    auto parse_int_opt = [&](kimix::string_view key, int64_t &out,
                             kimix::string_view err_msg) -> bool {
        const ValueElement *el = parameters->get(key);
        if (el == nullptr) {
            return true;
        }
        if (!el->is_int()) {
            rd_serialize_status(result, "invalid_input", err_msg, _result);
            return false;
        }
        out = el->as_int();
        return true;
    };

    if (!parse_int_opt("offset", offset, "offset must be an integer")) {
        return;
    }
    if (!parse_int_opt("limit", limit, "limit must be an integer")) {
        return;
    }
    if (!parse_int_opt("max_char", max_char, "max_char must be an integer")) {
        return;
    }
    if (!parse_int_opt("char_offset", char_offset,
                       "char_offset must be an integer")) {
        return;
    }

    const ValueElement *sn_el = parameters->get("show_line_numbers");
    if (sn_el != nullptr) {
        if (!sn_el->is_bool()) {
            rd_serialize_status(result, "invalid_input",
                              "show_line_numbers must be a bool", _result);
            return;
        }
        show_line_numbers = sn_el->as_bool();
    }

    const ValueElement *note_el = parameters->get("note");
    if (note_el != nullptr) {
        if (!note_el->is_string()) {
            rd_serialize_status(result, "invalid_input", "note must be a string",
                                _result);
            return;
        }
        note = note_el->as_string();
    }

    tool_error err = validate_int_option("offset", offset);
    if (err.failed()) {
        rd_serialize_status(result, "invalid_input", err.message, _result);
        return;
    }
    err = validate_int_option("limit", limit);
    if (err.failed()) {
        rd_serialize_status(result, "invalid_input", err.message, _result);
        return;
    }
    err = validate_int_option("max_char", max_char);
    if (err.failed()) {
        rd_serialize_status(result, "invalid_input", err.message, _result);
        return;
    }
    err = validate_int_option("char_offset", char_offset);
    if (err.failed()) {
        rd_serialize_status(result, "invalid_input", err.message, _result);
        return;
    }

    const kimix::vector<kimix::string> lines = split_lines(content);
    const render_result rr =
        (offset < 0)
            ? render_tail(lines, display_path, offset, limit, show_line_numbers, note)
            : render_forward(lines, display_path, offset, limit, show_line_numbers, note);

    const char_window cw = apply_char_window(rr.output, char_offset, max_char);

    result.values["status"] = ValueElement::make_string(kimix::string("ok"));
    result.values["output"] = ValueElement::make_string(cw.output);
    kimix::StringScratch msg;
    msg << rr.message << cw.note;
    result.values["message"] = ValueElement::make_string(std::move(msg.string()));
    result.values["brief"] = ValueElement::make_string(kimix::string("Read file"));
    result.values["start_line"] = ValueElement::make_int(rr.start_line);
    result.values["total_lines"] = ValueElement::make_int(rr.total_lines);
    result.values["max_lines_reached"] =
        ValueElement::make_bool(rr.max_lines_reached);
    result.values["max_bytes_reached"] =
        ValueElement::make_bool(rr.max_bytes_reached);
    result.values["end_of_file"] = ValueElement::make_bool(rr.end_of_file);
    ValueElement::Array trunc;
    trunc.reserve(rr.truncated_line_numbers.size());
    for (const int64_t ln : rr.truncated_line_numbers) {
        trunc.push_back(ValueElement::make_int(ln));
    }
    result.values["truncated_line_numbers"] =
        ValueElement::make_array(std::move(trunc));
    result.serialize(_result);
}

} // namespace read
} // namespace kimix::builtin_tools
