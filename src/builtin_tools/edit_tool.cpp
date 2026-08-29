// edit_tool.cpp - Multi-mode edit kernels (kimix::builtin_tools::edit).
//
// Port of the kimi-agent `edit` tool kernels. See edit_tool.h for the Python
// source-of-truth references. All functions are pure CPU kernels returning
// data (tool_error); nothing throws across the tool boundary.
//
// Unity-build note: every helper lives in the `edit` nested namespace or in
// the TU-local `edit_detail` namespace; no generic file-scope names.
// Source is ASCII-only (non-ASCII payload bytes are written as \xNN escapes).

#include "builtin_tools/edit_tool.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include "builtin_tools/utf8_util.h"

namespace kimix::builtin_tools::edit {

// ===========================================================================
// TU-local helpers (edit_detail)
// ===========================================================================

namespace edit_detail {

// Python str.isspace() code points (exact, mirrors runtime/tools/line_hash).
bool is_py_space_cp(uint32_t cp) noexcept {
    switch (cp) {
    case 0x0009:
    case 0x000A:
    case 0x000B:
    case 0x000C:
    case 0x000D:
    case 0x0020:
    case 0x0085:
    case 0x00A0:
    case 0x1680:
        return true;
    default:
        return (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
               cp == 0x202F || cp == 0x205F || cp == 0x3000;
    }
}

// ---------------------------------------------------------------------------
// Unicode alphanumeric ranges (Python str.isalnum = categories L* and N*).
// Generated from Python unicodedata (same table as runtime/tools/line_hash).
// ---------------------------------------------------------------------------
constexpr uint32_t kAlnumRanges[][2] = {
    0x0030,
    0x0039,
    0x0041,
    0x005A,
    0x0061,
    0x007A,
    0x00AA,
    0x00AA,
    0x00B2,
    0x00B3,
    0x00B5,
    0x00B5,
    0x00B9,
    0x00BA,
    0x00BC,
    0x00BE,
    0x00C0,
    0x00D6,
    0x00D8,
    0x00F6,
    0x00F8,
    0x02C1,
    0x02C6,
    0x02D1,
    0x02E0,
    0x02E4,
    0x02EC,
    0x02EC,
    0x02EE,
    0x02EE,
    0x0370,
    0x0374,
    0x0376,
    0x0377,
    0x037A,
    0x037D,
    0x037F,
    0x037F,
    0x0386,
    0x0386,
    0x0388,
    0x038A,
    0x038C,
    0x038C,
    0x038E,
    0x03A1,
    0x03A3,
    0x03F5,
    0x03F7,
    0x0481,
    0x048A,
    0x052F,
    0x0531,
    0x0556,
    0x0559,
    0x0559,
    0x0560,
    0x0588,
    0x05D0,
    0x05EA,
    0x05EF,
    0x05F2,
    0x0620,
    0x064A,
    0x0660,
    0x0669,
    0x066E,
    0x06D3,
    0x06D5,
    0x06D5,
    0x06E5,
    0x06E6,
    0x06EE,
    0x06FC,
    0x06FF,
    0x06FF,
    0x0710,
    0x0710,
    0x0712,
    0x072F,
    0x074D,
    0x07A5,
    0x07B1,
    0x07B1,
    0x07C0,
    0x07EA,
    0x07F4,
    0x07F5,
    0x07FA,
    0x07FA,
    0x0800,
    0x0815,
    0x081A,
    0x0824,
    0x0828,
    0x0828,
    0x0840,
    0x0858,
    0x0860,
    0x086A,
    0x0870,
    0x0887,
    0x0889,
    0x088E,
    0x08A0,
    0x08C9,
    0x0904,
    0x0939,
    0x093D,
    0x093D,
    0x0950,
    0x0950,
    0x0958,
    0x0961,
    0x0966,
    0x096F,
    0x0971,
    0x0980,
    0x0985,
    0x098C,
    0x098F,
    0x0990,
    0x0993,
    0x09A8,
    0x09AA,
    0x09B0,
    0x09B2,
    0x09B2,
    0x09B6,
    0x09B9,
    0x09BD,
    0x09BD,
    0x09CE,
    0x09CE,
    0x09DC,
    0x09DD,
    0x09DF,
    0x09E1,
    0x09E6,
    0x09F1,
    0x09F4,
    0x09F9,
    0x09FC,
    0x09FC,
    0x0A01,
    0x0A03,
    0x0A05,
    0x0A0A,
    0x0A0F,
    0x0A10,
    0x0A13,
    0x0A28,
    0x0A2A,
    0x0A30,
    0x0A32,
    0x0A33,
    0x0A35,
    0x0A36,
    0x0A38,
    0x0A39,
    0x0A59,
    0x0A5C,
    0x0A5E,
    0x0A5E,
    0x0A66,
    0x0A6F,
    0x0A72,
    0x0A74,
    0x0A85,
    0x0A8D,
    0x0A8F,
    0x0A91,
    0x0A93,
    0x0AA8,
    0x0AAA,
    0x0AB0,
    0x0AB2,
    0x0AB3,
    0x0AB5,
    0x0AB9,
    0x0ABD,
    0x0ABD,
    0x0AD0,
    0x0AD0,
    0x0AE0,
    0x0AE1,
    0x0AE6,
    0x0AEF,
    0x0AF9,
    0x0AF9,
    0x0B01,
    0x0B03,
    0x0B05,
    0x0B0C,
    0x0B0F,
    0x0B10,
    0x0B13,
    0x0B28,
    0x0B2A,
    0x0B30,
    0x0B32,
    0x0B33,
    0x0B35,
    0x0B39,
    0x0B3D,
    0x0B3D,
    0x0B5C,
    0x0B5D,
    0x0B5F,
    0x0B61,
    0x0B66,
    0x0B6F,
    0x0B71,
    0x0B71,
    0x0B83,
    0x0B83,
    0x0B85,
    0x0B8A,
    0x0B8E,
    0x0B90,
    0x0B92,
    0x0B95,
    0x0B99,
    0x0B9A,
    0x0B9C,
    0x0B9C,
    0x0B9E,
    0x0B9F,
    0x0BA3,
    0x0BA4,
    0x0BA8,
    0x0BAA,
    0x0BAE,
    0x0BB9,
    0x0BBE,
    0x0BBF,
    0x0BC1,
    0x0BC2,
    0x0BC6,
    0x0BC8,
    0x0BCA,
    0x0BCF,
    0x0BD7,
    0x0BD7,
    0x0BE6,
    0x0BEF,
    0x0C00,
    0x0C04,
    0x0C05,
    0x0C0C,
    0x0C0E,
    0x0C10,
    0x0C12,
    0x0C28,
    0x0C2A,
    0x0C39,
    0x0C3D,
    0x0C3D,
    0x0C58,
    0x0C5A,
    0x0C60,
    0x0C61,
    0x0C66,
    0x0C6F,
    0x0C78,
    0x0C7E,
    0x0C80,
    0x0C80,
    0x0C85,
    0x0C8C,
    0x0C8E,
    0x0C90,
    0x0C92,
    0x0CA8,
    0x0CAA,
    0x0CB3,
    0x0CB5,
    0x0CB9,
    0x0CBD,
    0x0CBD,
    0x0CDE,
    0x0CDE,
    0x0CE0,
    0x0CE1,
    0x0CE6,
    0x0CEF,
    0x0CF1,
    0x0CF3,
    0x0D00,
    0x0D0C,
    0x0D0E,
    0x0D10,
    0x0D12,
    0x0D3A,
    0x0D3D,
    0x0D3D,
    0x0D4E,
    0x0D4E,
    0x0D54,
    0x0D56,
    0x0D5F,
    0x0D61,
    0x0D66,
    0x0D6F,
    0x0D7A,
    0x0D7F,
    0x0D85,
    0x0D96,
    0x0D9A,
    0x0DB1,
    0x0DB3,
    0x0DBB,
    0x0DBD,
    0x0DBD,
    0x0DC0,
    0x0DC6,
    0x0DCF,
    0x0DD1,
    0x0DD8,
    0x0DDF,
    0x0DE6,
    0x0DEF,
    0x0DF2,
    0x0DF4,
    0x0E01,
    0x0E30,
    0x0E32,
    0x0E33,
    0x0E40,
    0x0E46,
    0x0E50,
    0x0E59,
    0x0E81,
    0x0E82,
    0x0E84,
    0x0E84,
    0x0E86,
    0x0E8A,
    0x0E8C,
    0x0EA3,
    0x0EA5,
    0x0EA5,
    0x0EA7,
    0x0EB0,
    0x0EB2,
    0x0EB3,
    0x0EBD,
    0x0EBD,
    0x0EC0,
    0x0EC4,
    0x0EC6,
    0x0EC6,
    0x0ED0,
    0x0ED9,
    0x0EDC,
    0x0EDF,
    0x0F00,
    0x0F00,
    0x0F18,
    0x0F19,
    0x0F20,
    0x0F33,
    0x0F40,
    0x0F47,
    0x0F49,
    0x0F6C,
    0x0F88,
    0x0F8C,
    0x1000,
    0x102A,
    0x103F,
    0x103F,
    0x1050,
    0x1055,
    0x105A,
    0x105D,
    0x1061,
    0x1061,
    0x1065,
    0x1066,
    0x106E,
    0x1070,
    0x1075,
    0x1081,
    0x108E,
    0x108E,
    0x1090,
    0x1099,
    0x10A0,
    0x10C5,
    0x10C7,
    0x10C7,
    0x10CD,
    0x10CD,
    0x10D0,
    0x10FA,
    0x10FC,
    0x10FC,
    0x10FD,
    0x1248,
    0x124A,
    0x124D,
    0x1250,
    0x1256,
    0x1258,
    0x1258,
    0x125A,
    0x125D,
    0x1260,
    0x1288,
    0x128A,
    0x128D,
    0x1290,
    0x12B0,
    0x12B2,
    0x12B5,
    0x12B8,
    0x12BE,
    0x12C0,
    0x12C0,
    0x12C2,
    0x12C5,
    0x12C8,
    0x12D6,
    0x12D8,
    0x1310,
    0x1312,
    0x1315,
    0x1318,
    0x135A,
    0x1380,
    0x138F,
    0x13A0,
    0x13F5,
    0x13F8,
    0x13FD,
    0x1401,
    0x166C,
    0x166F,
    0x167F,
    0x1681,
    0x169A,
    0x16A0,
    0x16EA,
    0x16EE,
    0x16F8,
    0x1700,
    0x1711,
    0x171F,
    0x1731,
    0x1740,
    0x1751,
    0x1760,
    0x176C,
    0x176E,
    0x1770,
    0x1780,
    0x17B3,
    0x17D7,
    0x17D7,
    0x17DC,
    0x17DC,
    0x17E0,
    0x17E9,
    0x1810,
    0x1819,
    0x1820,
    0x1878,
    0x1880,
    0x1884,
    0x1887,
    0x18A8,
    0x18AA,
    0x18AA,
    0x18B0,
    0x18F5,
    0x1900,
    0x191E,
    0x1946,
    0x194F,
    0x1950,
    0x196D,
    0x1970,
    0x1974,
    0x1980,
    0x19AB,
    0x19B0,
    0x19C9,
    0x19D0,
    0x19DA,
    0x1A00,
    0x1A16,
    0x1A20,
    0x1A54,
    0x1A80,
    0x1A89,
    0x1A90,
    0x1A99,
    0x1AA7,
    0x1AA7,
    0x1B05,
    0x1B33,
    0x1B45,
    0x1B4C,
    0x1B50,
    0x1B59,
    0x1B83,
    0x1BA0,
    0x1BAE,
    0x1BAF,
    0x1BBA,
    0x1BE5,
    0x1C00,
    0x1C23,
    0x1C40,
    0x1C49,
    0x1C4D,
    0x1C7D,
    0x1C80,
    0x1C88,
    0x1C90,
    0x1CBA,
    0x1CBD,
    0x1CBF,
    0x1CE9,
    0x1CEC,
    0x1CEE,
    0x1CF3,
    0x1CF5,
    0x1CF7,
    0x1CFA,
    0x1CFA,
    0x1D00,
    0x1DBF,
    0x1E00,
    0x1F15,
    0x1F18,
    0x1F1D,
    0x1F20,
    0x1F45,
    0x1F48,
    0x1F4D,
    0x1F50,
    0x1F57,
    0x1F59,
    0x1F59,
    0x1F5B,
    0x1F5B,
    0x1F5D,
    0x1F5D,
    0x1F5F,
    0x1F7D,
    0x1F80,
    0x1FB4,
    0x1FB6,
    0x1FBC,
    0x1FBE,
    0x1FBE,
    0x1FC2,
    0x1FC4,
    0x1FC6,
    0x1FCC,
    0x1FD0,
    0x1FD3,
    0x1FD6,
    0x1FDB,
    0x1FE0,
    0x1FEC,
    0x1FF2,
    0x1FF4,
    0x1FF6,
    0x1FFC,
    0x2071,
    0x2071,
    0x207F,
    0x207F,
    0x2090,
    0x209C,
    0x2102,
    0x2102,
    0x2107,
    0x2107,
    0x210A,
    0x2113,
    0x2115,
    0x2115,
    0x2119,
    0x211D,
    0x2124,
    0x2124,
    0x2126,
    0x2126,
    0x2128,
    0x2128,
    0x212A,
    0x212D,
    0x212F,
    0x2139,
    0x213C,
    0x213F,
    0x2145,
    0x2149,
    0x214E,
    0x214E,
    0x2160,
    0x2188,
    0x24B6,
    0x24E9,
    0x2C00,
    0x2CE4,
    0x2CEB,
    0x2CEE,
    0x2CF2,
    0x2CF3,
    0x2D00,
    0x2D25,
    0x2D27,
    0x2D27,
    0x2D2D,
    0x2D2D,
    0x2D30,
    0x2D67,
    0x2D6F,
    0x2D6F,
    0x2D80,
    0x2D96,
    0x2DA0,
    0x2DA6,
    0x2DA8,
    0x2DAE,
    0x2DB0,
    0x2DB6,
    0x2DB8,
    0x2DBE,
    0x2DC0,
    0x2DC6,
    0x2DC8,
    0x2DCE,
    0x2DD0,
    0x2DD6,
    0x2DD8,
    0x2DDE,
    0x2E2F,
    0x2E2F,
    0x3005,
    0x3007,
    0x3021,
    0x3029,
    0x3031,
    0x3035,
    0x3038,
    0x303C,
    0x3041,
    0x3096,
    0x309D,
    0x309F,
    0x30A1,
    0x30FA,
    0x30FC,
    0x30FF,
    0x3105,
    0x312F,
    0x3131,
    0x318E,
    0x31A0,
    0x31BF,
    0x31F0,
    0x31FF,
    0x3400,
    0x4DBF,
    0x4E00,
    0xA48C,
    0xA4D0,
    0xA4FD,
    0xA500,
    0xA60C,
    0xA610,
    0xA61F,
    0xA62A,
    0xA62B,
    0xA640,
    0xA66E,
    0xA67F,
    0xA69D,
    0xA6A0,
    0xA6E5,
    0xA6F0,
    0xA6F1,
    0xA717,
    0xA71F,
    0xA722,
    0xA788,
    0xA78B,
    0xA7CA,
    0xA7D0,
    0xA7D1,
    0xA7D3,
    0xA7D3,
    0xA7D5,
    0xA7D9,
    0xA7F2,
    0xA801,
    0xA803,
    0xA805,
    0xA807,
    0xA80A,
    0xA80C,
    0xA822,
    0xA830,
    0xA835,
    0xA840,
    0xA873,
    0xA882,
    0xA8B3,
    0xA8D0,
    0xA8D9,
    0xA8F2,
    0xA8F7,
    0xA8FB,
    0xA8FB,
    0xA8FD,
    0xA8FE,
    0xA900,
    0xA925,
    0xA930,
    0xA946,
    0xA960,
    0xA97C,
    0xA984,
    0xA9B2,
    0xA9CF,
    0xA9CF,
    0xA9D0,
    0xA9D9,
    0xA9E0,
    0xA9E4,
    0xA9E6,
    0xA9EF,
    0xA9FA,
    0xA9FE,
    0xAA00,
    0xAA28,
    0xAA40,
    0xAA42,
    0xAA44,
    0xAA4B,
    0xAA50,
    0xAA59,
    0xAA60,
    0xAA76,
    0xAA7A,
    0xAA7A,
    0xAA7E,
    0xAAAF,
    0xAAB1,
    0xAAB1,
    0xAAB5,
    0xAAB6,
    0xAAB9,
    0xAABD,
    0xAAC0,
    0xAAC0,
    0xAAC2,
    0xAAC2,
    0xAADB,
    0xAADD,
    0xAAE0,
    0xAAEA,
    0xAAF2,
    0xAAF4,
    0xAB01,
    0xAB06,
    0xAB09,
    0xAB0E,
    0xAB11,
    0xAB16,
    0xAB20,
    0xAB26,
    0xAB28,
    0xAB2E,
    0xAB30,
    0xAB5A,
    0xAB5C,
    0xAB69,
    0xAB70,
    0xABE2,
    0xABF0,
    0xABF9,
    0xAC00,
    0xD7A3,
    0xD7B0,
    0xD7C6,
    0xD7CB,
    0xD7FB,
    0xF900,
    0xFA6D,
    0xFA70,
    0xFAD9,
    0xFB00,
    0xFB06,
    0xFB13,
    0xFB17,
    0xFB1D,
    0xFB1D,
    0xFB1F,
    0xFB28,
    0xFB2A,
    0xFB36,
    0xFB38,
    0xFB3C,
    0xFB3E,
    0xFB3E,
    0xFB40,
    0xFB41,
    0xFB43,
    0xFB44,
    0xFB46,
    0xFBB1,
    0xFBD3,
    0xFD3D,
    0xFD50,
    0xFD8F,
    0xFD92,
    0xFDC7,
    0xFDF0,
    0xFDFB,
    0xFE70,
    0xFE74,
    0xFE76,
    0xFEFC,
    0xFF10,
    0xFF19,
    0xFF21,
    0xFF3A,
    0xFF41,
    0xFF5A,
    0xFF66,
    0xFFBE,
    0xFFC2,
    0xFFC7,
    0xFFCA,
    0xFFCF,
    0xFFD2,
    0xFFD7,
    0xFFDA,
    0xFFDC,
    0x10000,
    0x1000B,
    0x1000D,
    0x10026,
    0x10028,
    0x1003A,
    0x1003C,
    0x1003D,
    0x1003F,
    0x1004D,
    0x10050,
    0x1005D,
    0x10080,
    0x100FA,
    0x10140,
    0x10174,
    0x10280,
    0x1029C,
    0x102A0,
    0x102D0,
    0x10300,
    0x1031F,
    0x1032D,
    0x1034A,
    0x10350,
    0x10375,
    0x10380,
    0x1039D,
    0x103A0,
    0x103C3,
    0x103C8,
    0x103CF,
    0x103D1,
    0x103D5,
    0x10400,
    0x1049D,
    0x104A0,
    0x104A9,
    0x104B0,
    0x104D3,
    0x104D8,
    0x104FB,
    0x10500,
    0x10527,
    0x10530,
    0x10563,
    0x10570,
    0x1057A,
    0x1057C,
    0x1058A,
    0x1058C,
    0x10592,
    0x10594,
    0x10595,
    0x10597,
    0x105A1,
    0x105A3,
    0x105B1,
    0x105B3,
    0x105B9,
    0x105BB,
    0x105BC,
    0x10600,
    0x10736,
    0x10740,
    0x10755,
    0x10760,
    0x10767,
    0x10780,
    0x10785,
    0x10787,
    0x107B0,
    0x107B2,
    0x107BA,
    0x10800,
    0x10805,
    0x10808,
    0x10808,
    0x1080A,
    0x10835,
    0x10837,
    0x10838,
    0x1083C,
    0x1083C,
    0x1083F,
    0x10855,
    0x10860,
    0x10876,
    0x10880,
    0x1089E,
    0x108E0,
    0x108F2,
    0x108F4,
    0x108F5,
    0x10900,
    0x10915,
    0x10920,
    0x10939,
    0x10980,
    0x109B7,
    0x109BE,
    0x109BF,
    0x10A00,
    0x10A00,
    0x10A10,
    0x10A13,
    0x10A15,
    0x10A17,
    0x10A19,
    0x10A35,
    0x10A60,
    0x10A7C,
    0x10A80,
    0x10A9C,
    0x10AC0,
    0x10AC7,
    0x10AC9,
    0x10AE4,
    0x10B00,
    0x10B35,
    0x10B40,
    0x10B55,
    0x10B60,
    0x10B72,
    0x10B80,
    0x10B91,
    0x10C00,
    0x10C48,
    0x10C80,
    0x10CB2,
    0x10CC0,
    0x10CF2,
    0x10D00,
    0x10D23,
    0x10D30,
    0x10D39,
    0x10E60,
    0x10E7E,
    0x10E80,
    0x10EA9,
    0x10EB0,
    0x10EB1,
    0x10F00,
    0x10F1C,
    0x10F27,
    0x10F27,
    0x10F30,
    0x10F45,
    0x10F70,
    0x10F81,
    0x10FB0,
    0x10FC4,
    0x10FE0,
    0x10FF6,
    0x11000,
    0x11000,
    0x11003,
    0x11037,
    0x11047,
    0x1104D,
    0x11066,
    0x1106F,
    0x11071,
    0x11072,
    0x11075,
    0x11075,
    0x11083,
    0x110AF,
    0x110D0,
    0x110E8,
    0x110F0,
    0x110F9,
    0x11103,
    0x11126,
    0x11136,
    0x1113F,
    0x11144,
    0x11144,
    0x11147,
    0x11147,
    0x11150,
    0x11172,
    0x11176,
    0x11176,
    0x11180,
    0x111B2,
    0x111C1,
    0x111C4,
    0x111DA,
    0x111DA,
    0x111DC,
    0x111DC,
    0x11200,
    0x11211,
    0x11213,
    0x1122B,
    0x11280,
    0x11286,
    0x11288,
    0x11288,
    0x1128A,
    0x1128D,
    0x1128F,
    0x1129D,
    0x1129F,
    0x112A8,
    0x112B0,
    0x112DE,
    0x11305,
    0x1130C,
    0x1130F,
    0x11310,
    0x11313,
    0x11328,
    0x1132A,
    0x11330,
    0x11332,
    0x11333,
    0x11335,
    0x11339,
    0x1133D,
    0x1133D,
    0x11350,
    0x11350,
    0x1135D,
    0x11361,
    0x11400,
    0x11434,
    0x11447,
    0x1144A,
    0x11450,
    0x11459,
    0x1145F,
    0x11461,
    0x11480,
    0x114AF,
    0x114C4,
    0x114C7,
    0x114D0,
    0x114D9,
    0x11580,
    0x115AE,
    0x115D8,
    0x115DB,
    0x11600,
    0x1162F,
    0x11644,
    0x11644,
    0x11650,
    0x11659,
    0x11680,
    0x116AA,
    0x116B0,
    0x116B5,
    0x116C0,
    0x116C9,
    0x11700,
    0x1171A,
    0x11730,
    0x1173F,
    0x11740,
    0x11746,
    0x11800,
    0x1182B,
    0x118A0,
    0x118E9,
    0x118FF,
    0x118FF,
    0x11900,
    0x11906,
    0x11909,
    0x11909,
    0x1190C,
    0x11913,
    0x11915,
    0x11916,
    0x11918,
    0x1192F,
    0x1193F,
    0x1193F,
    0x11941,
    0x11941,
    0x11950,
    0x11959,
    0x119A0,
    0x119A7,
    0x119AA,
    0x119D0,
    0x119E1,
    0x119E3,
    0x119E4,
    0x11A00,
    0x11A00,
    0x11A0B,
    0x11A32,
    0x11A3A,
    0x11A3A,
    0x11A50,
    0x11A50,
    0x11A5C,
    0x11A89,
    0x11A9D,
    0x11A9D,
    0x11AB0,
    0x11AF8,
    0x11C00,
    0x11C08,
    0x11C0A,
    0x11C2E,
    0x11C40,
    0x11C40,
    0x11C50,
    0x11C6C,
    0x11C72,
    0x11C8F,
    0x11D00,
    0x11D06,
    0x11D08,
    0x11D09,
    0x11D0B,
    0x11D30,
    0x11D46,
    0x11D46,
    0x11D50,
    0x11D59,
    0x11D60,
    0x11D65,
    0x11D67,
    0x11D68,
    0x11D6A,
    0x11D89,
    0x11D98,
    0x11D98,
    0x11DA0,
    0x11DA9,
    0x11EE0,
    0x11EF2,
    0x11EF5,
    0x11EF8,
    0x11F00,
    0x11F01,
    0x11F02,
    0x11F02,
    0x11F04,
    0x11F10,
    0x11F12,
    0x11F33,
    0x11F50,
    0x11F59,
    0x11FB0,
    0x11FB0,
    0x12000,
    0x12399,
    0x12400,
    0x1246E,
    0x12480,
    0x12543,
    0x12F90,
    0x12FF0,
    0x13000,
    0x1342F,
    0x13441,
    0x13446,
    0x14400,
    0x14646,
    0x16800,
    0x16A38,
    0x16A40,
    0x16A5E,
    0x16A60,
    0x16A69,
    0x16A70,
    0x16ABE,
    0x16AD0,
    0x16AED,
    0x16B00,
    0x16B2F,
    0x16B40,
    0x16B43,
    0x16B50,
    0x16B59,
    0x16B63,
    0x16B77,
    0x16B7D,
    0x16B8F,
    0x16E40,
    0x16E7F,
    0x16F00,
    0x16F4A,
    0x16F50,
    0x16F50,
    0x16F93,
    0x16F9F,
    0x16FE0,
    0x16FE1,
    0x16FE3,
    0x16FE3,
    0x17000,
    0x187F7,
    0x18800,
    0x18CD5,
    0x18D00,
    0x18D08,
    0x1AFF0,
    0x1AFF3,
    0x1AFF5,
    0x1AFFB,
    0x1AFFD,
    0x1AFFE,
    0x1B000,
    0x1B122,
    0x1B132,
    0x1B132,
    0x1B150,
    0x1B152,
    0x1B155,
    0x1B155,
    0x1B164,
    0x1B167,
    0x1B170,
    0x1B2FB,
    0x1BC00,
    0x1BC6A,
    0x1BC70,
    0x1BC7C,
    0x1BC80,
    0x1BC88,
    0x1BC90,
    0x1BC99,
    0x1BC9E,
    0x1BC9E,
    0x1D400,
    0x1D454,
    0x1D456,
    0x1D49C,
    0x1D49E,
    0x1D49F,
    0x1D4A2,
    0x1D4A2,
    0x1D4A5,
    0x1D4A6,
    0x1D4A9,
    0x1D4AC,
    0x1D4AE,
    0x1D4B9,
    0x1D4BB,
    0x1D4BB,
    0x1D4BD,
    0x1D4C3,
    0x1D4C5,
    0x1D505,
    0x1D507,
    0x1D50A,
    0x1D50D,
    0x1D514,
    0x1D516,
    0x1D51C,
    0x1D51E,
    0x1D539,
    0x1D53B,
    0x1D53E,
    0x1D540,
    0x1D544,
    0x1D546,
    0x1D546,
    0x1D54A,
    0x1D550,
    0x1D552,
    0x1D6A5,
    0x1D6A8,
    0x1D7CB,
    0x1D7CE,
    0x1D9FF,
    0x1DA00,
    0x1DA36,
    0x1DA3B,
    0x1DA6C,
    0x1DA75,
    0x1DA75,
    0x1DA84,
    0x1DA84,
    0x1DA9B,
    0x1DA9F,
    0x1DF00,
    0x1DF1E,
    0x1DF25,
    0x1DF2A,
    0x1E030,
    0x1E06D,
    0x1E100,
    0x1E12C,
    0x1E137,
    0x1E13D,
    0x1E14E,
    0x1E14E,
    0x1E290,
    0x1E2AD,
    0x1E2C0,
    0x1E2EB,
    0x1E4D0,
    0x1E4EA,
    0x1E4F0,
    0x1E4F9,
    0x1E7E0,
    0x1E7E6,
    0x1E7E8,
    0x1E7EB,
    0x1E7ED,
    0x1E7EE,
    0x1E7F0,
    0x1E7FE,
    0x1E800,
    0x1E8C4,
    0x1E8D0,
    0x1E8D6,
    0x1E900,
    0x1E943,
    0x1E950,
    0x1E959,
    0x1EE00,
    0x1EE03,
    0x1EE05,
    0x1EE1F,
    0x1EE21,
    0x1EE22,
    0x1EE24,
    0x1EE24,
    0x1EE27,
    0x1EE27,
    0x1EE29,
    0x1EE32,
    0x1EE34,
    0x1EE37,
    0x1EE39,
    0x1EE39,
    0x1EE3B,
    0x1EE3B,
    0x1EE42,
    0x1EE42,
    0x1EE47,
    0x1EE47,
    0x1EE49,
    0x1EE49,
    0x1EE4B,
    0x1EE4B,
    0x1EE4D,
    0x1EE4F,
    0x1EE51,
    0x1EE52,
    0x1EE54,
    0x1EE54,
    0x1EE57,
    0x1EE57,
    0x1EE59,
    0x1EE59,
    0x1EE5B,
    0x1EE5B,
    0x1EE5D,
    0x1EE5D,
    0x1EE5F,
    0x1EE5F,
    0x1EE61,
    0x1EE62,
    0x1EE64,
    0x1EE64,
    0x1EE67,
    0x1EE6A,
    0x1EE6C,
    0x1EE72,
    0x1EE74,
    0x1EE77,
    0x1EE79,
    0x1EE7C,
    0x1EE7E,
    0x1EE7E,
    0x1EE80,
    0x1EE89,
    0x1EE8B,
    0x1EE9B,
    0x1EEA1,
    0x1EEA3,
    0x1EEA5,
    0x1EEA9,
    0x1EEAB,
    0x1EEBB,
    0x20000,
    0x2A6DF,
    0x2A700,
    0x2B739,
    0x2B740,
    0x2B81D,
    0x2B820,
    0x2CEA1,
    0x2CEB0,
    0x2EBE0,
    0x2F800,
    0x2FA1D,
    0x30000,
    0x3134A,
    0x31350,
    0x323AF,
    0xE0100,
    0xE01EF,
};

bool is_alnum_cp(uint32_t cp) noexcept {
    if (cp < 0x80) {
        return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') ||
               (cp >= 'A' && cp <= 'Z');
    }
    size_t lo = 0;
    size_t hi = sizeof(kAlnumRanges) / sizeof(kAlnumRanges[0]);
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (cp < kAlnumRanges[mid][0]) {
            hi = mid;
        } else if (cp > kAlnumRanges[mid][1]) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// XXH32 (canonical xxHash 32-bit; public domain / BSD, Yann Collet)
// ---------------------------------------------------------------------------

constexpr uint32_t kPrime32_1 = 0x9E3779B1u;
constexpr uint32_t kPrime32_2 = 0x85EBCA77u;
constexpr uint32_t kPrime32_3 = 0xC2B2AE3Du;
constexpr uint32_t kPrime32_4 = 0x27D4EB2Fu;
constexpr uint32_t kPrime32_5 = 0x165667B1u;

inline uint32_t rotl32(uint32_t x, int r) noexcept {
    return (x << r) | (x >> (32 - r));
}

inline uint32_t read32_le(const uint8_t *p) noexcept {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

inline uint32_t round32(uint32_t acc, uint32_t input) noexcept {
    acc += input * kPrime32_2;
    acc = rotl32(acc, 13);
    acc *= kPrime32_1;
    return acc;
}

uint32_t xxh32(const void *input, size_t len, uint32_t seed) noexcept {
    const uint8_t *p = static_cast<const uint8_t *>(input);
    const uint8_t *const bEnd = p + len;
    uint32_t h32;
    if (len >= 16) {
        const uint8_t *const limit = bEnd - 16;
        uint32_t v1 = seed + kPrime32_1 + kPrime32_2;
        uint32_t v2 = seed + kPrime32_2;
        uint32_t v3 = seed;
        uint32_t v4 = seed - kPrime32_1;
        do {
            v1 = round32(v1, read32_le(p));
            p += 4;
            v2 = round32(v2, read32_le(p));
            p += 4;
            v3 = round32(v3, read32_le(p));
            p += 4;
            v4 = round32(v4, read32_le(p));
            p += 4;
        } while (p <= limit);
        h32 = rotl32(v1, 1) + rotl32(v2, 7) + rotl32(v3, 12) + rotl32(v4, 18);
    } else {
        h32 = seed + kPrime32_5;
    }
    h32 += static_cast<uint32_t>(len);
    while (p + 4 <= bEnd) {
        h32 += read32_le(p) * kPrime32_3;
        h32 = rotl32(h32, 17) * kPrime32_4;
        p += 4;
    }
    while (p < bEnd) {
        h32 += static_cast<uint32_t>(*p) * kPrime32_5;
        h32 = rotl32(h32, 11) * kPrime32_1;
        ++p;
    }
    h32 ^= h32 >> 15;
    h32 *= kPrime32_2;
    h32 ^= h32 >> 13;
    h32 *= kPrime32_3;
    h32 ^= h32 >> 16;
    return h32;
}

// NIBBLE_STR = "ZPMQVRWSNKTXJBYH" -> 2-char nibble string for a byte 0..255.
kimix::string nibble_str(uint32_t v) noexcept {
    static const char kNibble[16] = {'Z', 'P', 'M', 'Q', 'V', 'R', 'W', 'S',
                                     'N', 'K', 'T', 'X', 'J', 'B', 'Y', 'H'};
    kimix::string s;
    s.push_back(kNibble[(v >> 4) & 0x0F]);
    s.push_back(kNibble[v & 0x0F]);
    return s;
}

// Decode all code points of `s` into `out` (Python errors="replace" style).
void decode_code_points(kimix::string_view s, kimix::vector<uint32_t> &out) {
    out.clear();
    const char *it = s.data();
    const char *end = s.data() + s.size();
    while (it < end) {
        out.push_back(decode_code_point(it, end));
    }
}

// Parse an unsigned decimal from a digit run (std::stoll does not accept
// kimix::string, so parse by hand; callers only pass validated digit spans).
int64_t parse_decimal(kimix::string_view s) {
    int64_t v = 0;
    for (char c : s) {
        v = v * 10 + (c - '0');
    }
    return v;
}

// Indel distance (Levenshtein with substitution cost 2) over two arrays.
template <typename T>
uint64_t indel_distance(const T *a, size_t an, const T *b, size_t bn) {
    if (an == 0) {
        return bn;
    }
    if (bn == 0) {
        return an;
    }
    kimix::vector<uint64_t> prev(bn + 1, 0), cur(bn + 1, 0);
    for (size_t j = 0; j <= bn; ++j) {
        prev[j] = j;
    }
    for (size_t i = 1; i <= an; ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= bn; ++j) {
            const uint64_t sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 2);
            const uint64_t del = prev[j] + 1;
            const uint64_t ins = cur[j - 1] + 1;
            cur[j] = std::min(sub, std::min(del, ins));
        }
        std::swap(prev, cur);
    }
    return prev[bn];
}

// Shared scoring body after the length gates have passed. `full_len` is the
// original (untrimmed) code-point count sum of both strings.
double ratio_from_middle(const uint32_t *a, size_t an, const uint32_t *b,
                         size_t bn, size_t full_len) {
    size_t pre = 0;
    while (pre < an && pre < bn && a[pre] == b[pre]) {
        ++pre;
    }
    const size_t sa = an - pre;
    const size_t sb = bn - pre;
    size_t suf = 0;
    while (suf < sa && suf < sb && a[an - 1 - suf] == b[bn - 1 - suf]) {
        ++suf;
    }
    const size_t ma = sa - suf;
    const size_t mb = sb - suf;
    const uint64_t dist = indel_distance(a + pre, ma, b + pre, mb);
    if (full_len == 0) {
        return 100.0;
    }
    return 100.0 * (1.0 - static_cast<double>(dist) /
                              static_cast<double>(full_len));
}

size_t leading_space_len(kimix::string_view s) {
    const char *it = s.data();
    const char *end = s.data() + s.size();
    size_t count = 0;
    while (it < end) {
        const char *before = it;
        const uint32_t cp = decode_code_point(it, end);
        if (!is_py_space_cp(cp)) {
            break;
        }
        count += static_cast<size_t>(it - before);
    }
    return count;
}

size_t trailing_space_len(kimix::string_view s) {
    size_t i = s.size();
    while (i > 0) {
        size_t start = i - 1;
        while (start > 0 && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) {
            --start;
        }
        const char *it = s.data() + start;
        const char *end = s.data() + i;
        const uint32_t cp = decode_code_point(it, end);
        if (!is_py_space_cp(cp)) {
            break;
        }
        i = start;
    }
    return s.size() - i;
}

struct extract_one_result {
    bool found = false;
    size_t index = 0;
    double score = 0.0;
    tool_error error;
};

extract_one_result extract_one(kimix::string_view target,
                               kimix::span<const kimix::string> candidates) {
    extract_one_result r;
    bool first = true;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const fuzz_ratio_result fr = fuzz_ratio(target, candidates[i]);
        if (fr.status != tool_status::ok) {
            r.error.status = fr.status;
            r.error.message = "fuzz_ratio input exceeds the native length cap";
            return r;
        }
        if (first || fr.score > r.score) {
            r.score = fr.score;
            r.index = i;
            first = false;
        }
    }
    r.found = !first;
    return r;
}

// Replace the first occurrence of `old` in `s` with `new_text`.
kimix::string replace_first(kimix::string_view s, kimix::string_view old_text,
                            kimix::string_view new_text) {
    const size_t idx = s.find(old_text);
    if (idx == kimix::string_view::npos) {
        return kimix::string(s);
    }
    kimix::string out;
    out.reserve(s.size() + new_text.size());
    out.append(s.substr(0, idx));
    out.append(new_text);
    out.append(s.substr(idx + old_text.size()));
    return out;
}

// Python str.replace(old, new): replace every non-overlapping occurrence
// left-to-right.
kimix::string replace_all(kimix::string_view s, kimix::string_view old_text,
                          kimix::string_view new_text) {
    kimix::string out;
    out.reserve(s.size());
    size_t pos = 0;
    while (true) {
        const size_t idx = s.find(old_text, pos);
        if (idx == kimix::string_view::npos) {
            out.append(s.substr(pos));
            break;
        }
        out.append(s.substr(pos, idx - pos));
        out.append(new_text);
        pos = idx + old_text.size();
    }
    return out;
}

// Python str.count(non-overlapping occurrences).
size_t count_occurrences(kimix::string_view s, kimix::string_view needle) {
    size_t count = 0;
    size_t pos = 0;
    while (true) {
        const size_t idx = s.find(needle, pos);
        if (idx == kimix::string_view::npos) {
            break;
        }
        ++count;
        pos = idx + needle.size();
    }
    return count;
}

// First 80 code points (Python matched_text[:80]).
kimix::string_view first_80_cps(kimix::string_view s) {
    const size_t bytes = utf8_byte_offset_of_code_point(s, 80);
    return s.substr(0, bytes);
}

struct replace_all_out {
    kimix::string content;
    size_t count = 0;
    bool missed = false; // count == 0 -> caller computes the suggestion
};

replace_all_out apply_replace_all(kimix::string_view norm_content,
                                  kimix::string_view norm_old,
                                  kimix::string_view norm_new,
                                  const replace_edit_item &edit) {
    replace_all_out out;
    if (edit.max_replacements.has_value()) {
        size_t count = 0;
        kimix::string result(norm_content);
        while (count < *edit.max_replacements) {
            const size_t idx = result.find(norm_old);
            if (idx == kimix::string::npos) {
                break;
            }
            result.replace(idx, norm_old.size(), norm_new);
            ++count;
        }
        out.content = std::move(result);
        out.count = count;
        out.missed = (count == 0);
        return out;
    }
    const size_t count = count_occurrences(norm_content, norm_old);
    out.content = replace_all(norm_content, norm_old, norm_new);
    out.count = count;
    out.missed = (count == 0);
    return out;
}

replace_result apply_fuzzy_fallback(kimix::string_view content,
                                    kimix::string_view norm_content,
                                    kimix::string_view norm_old,
                                    kimix::string_view norm_new,
                                    const replace_edit_item &edit) {
    replace_result r;
    const optional_text_result strip_result =
        try_strip_match(content, edit.old_text, edit.new_text);
    if (strip_result.error.failed()) {
        r.error = strip_result.error;
        return r;
    }
    if (strip_result.text.has_value()) {
        r.content = *strip_result.text;
        r.replacements = 1;
        return r;
    }

    const fuzzy_match_result fuzzy = best_fuzzy_match(edit.old_text, content);
    if (fuzzy.error.failed()) {
        r.error = fuzzy.error;
        return r;
    }
    if (fuzzy.matched_original.has_value()) {
        const kimix::string matched = normalize_newlines(*fuzzy.matched_original);
        r.content = replace_first(norm_content, matched, norm_new);
        r.replacements = 1;
        const kimix::string_view shown =
            first_80_cps(kimix::string_view(*fuzzy.matched_original));
        r.suggestion = kimix::format("fuzzy-matched at {:.0f}%: '{}'",
                                     fuzzy.score, shown);
        return r;
    }

    const optional_text_result sim = find_similar(edit.old_text, content);
    if (sim.error.failed()) {
        r.error = sim.error;
        return r;
    }
    r.content = kimix::string(content);
    r.replacements = 0;
    r.suggestion = sim.text;
    return r;
}

kimix::vector<hunk_line> adjust_added_lines(
    const diff_hunk &hunk, kimix::span<const kimix::string> pattern_lines,
    kimix::span<const kimix::string> actual_lines) {
    int32_t delta = 0;
    kimix::string indent_char;
    infer_indent_adjustment(pattern_lines, actual_lines, delta, indent_char);
    if (delta == 0) {
        return hunk.lines;
    }
    kimix::vector<hunk_line> adjusted;
    adjusted.reserve(hunk.lines.size());
    for (const hunk_line &l : hunk.lines) {
        if (l.kind == hunk_line_kind::add) {
            adjusted.push_back(
                {hunk_line_kind::add, apply_indent(l.text, delta, indent_char)});
        } else {
            adjusted.push_back(l);
        }
    }
    return adjusted;
}

} // namespace edit_detail

// ===========================================================================
// 1. Common helpers
// ===========================================================================

kimix::string normalize_newlines(kimix::string_view text) {
    kimix::string out;
    out.reserve(text.size());
    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        if (i + 1 < n && text[i] == '\r' && text[i + 1] == '\n') {
            out.push_back('\n');
            i += 2;
        } else {
            out.push_back(text[i]);
            ++i;
        }
    }
    return out;
}

kimix::string normalize_breaks(kimix::string_view text) {
    const kimix::string first = normalize_newlines(text);
    kimix::string out;
    out.reserve(first.size());
    for (char c : first) {
        out.push_back(c == '\r' ? '\n' : c);
    }
    return out;
}

void split_lf(kimix::string_view text, kimix::vector<kimix::string> &out) {
    out.clear();
    size_t start = 0;
    const size_t n = text.size();
    for (size_t i = 0; i < n; ++i) {
        if (text[i] == '\n') {
            out.emplace_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    out.emplace_back(text.substr(start)); // final segment, possibly empty
}

void split_lines(kimix::string_view text, kimix::vector<kimix::string> &out) {
    out.clear();
    size_t start = 0;
    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t term = 0;
        if (c == '\n') {
            term = 1;
        } else if (c == '\r') {
            term = (i + 1 < n && text[i + 1] == '\n') ? 2 : 1;
        } else if (c == '\x0B' || c == '\x0C' || c == '\x1C' || c == '\x1D' ||
                   c == '\x1E') {
            term = 1;
        } else if (c == 0xC2 && i + 1 < n &&
                   static_cast<unsigned char>(text[i + 1]) == 0x85) {
            term = 2;
        } else if (c == 0xE2 && i + 2 < n &&
                   static_cast<unsigned char>(text[i + 1]) == 0x80 &&
                   (static_cast<unsigned char>(text[i + 2]) == 0xA8 ||
                    static_cast<unsigned char>(text[i + 2]) == 0xA9)) {
            term = 3;
        }
        if (term != 0) {
            out.emplace_back(text.substr(start, i - start));
            i += term;
            start = i;
        } else {
            ++i;
        }
    }
    if (start < n) {
        out.emplace_back(text.substr(start));
    }
}

void split_lines_keepends(kimix::string_view text,
                          kimix::vector<kimix::string> &out) {
    out.clear();
    size_t start = 0;
    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t term = 0;
        if (c == '\n') {
            term = 1;
        } else if (c == '\r') {
            term = (i + 1 < n && text[i + 1] == '\n') ? 2 : 1;
        } else if (c == '\x0B' || c == '\x0C' || c == '\x1C' || c == '\x1D' ||
                   c == '\x1E') {
            term = 1;
        } else if (c == 0xC2 && i + 1 < n &&
                   static_cast<unsigned char>(text[i + 1]) == 0x85) {
            term = 2;
        } else if (c == 0xE2 && i + 2 < n &&
                   static_cast<unsigned char>(text[i + 1]) == 0x80 &&
                   (static_cast<unsigned char>(text[i + 2]) == 0xA8 ||
                    static_cast<unsigned char>(text[i + 2]) == 0xA9)) {
            term = 3;
        }
        if (term != 0) {
            out.emplace_back(text.substr(start, i - start + term));
            i += term;
            start = i;
        } else {
            ++i;
        }
    }
    if (start < n) {
        out.emplace_back(text.substr(start));
    }
}

kimix::string join_lf(kimix::span<const kimix::string> lines) {
    size_t total = 0;
    for (const kimix::string &s : lines) {
        total += s.size() + 1;
    }
    kimix::string out;
    out.reserve(total);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            out.push_back('\n');
        }
        out.append(lines[i]);
    }
    return out;
}

kimix::string detect_line_ending(kimix::string_view content) {
    return content.find("\r\n") != kimix::string_view::npos ? kimix::string("\r\n")
                                                            : kimix::string("\n");
}

void restore_trailing_newline(kimix::string &result, bool ended_with_newline) {
    if (ended_with_newline && !result.ends_with("\n")) {
        result.push_back('\n');
    }
}

void restore_trailing_newline_nonempty(kimix::string &result,
                                       bool ended_with_newline) {
    if (ended_with_newline && !result.empty() && !result.ends_with("\n")) {
        result.push_back('\n');
    }
}

kimix::string py_strip(kimix::string_view text) {
    const size_t lead = edit_detail::leading_space_len(text);
    const size_t trail = edit_detail::trailing_space_len(text);
    if (lead + trail >= text.size()) {
        return kimix::string();
    }
    return kimix::string(text.substr(lead, text.size() - lead - trail));
}

kimix::string py_repr(kimix::string_view text) {
    const bool has_single = text.find('\'') != kimix::string_view::npos;
    const bool has_double = text.find('"') != kimix::string_view::npos;
    const bool use_double = has_single && !has_double;
    kimix::string out;
    out.push_back(use_double ? '"' : '\'');
    for (unsigned char c : text) {
        if (c == '\\') {
            out.append("\\\\");
        } else if (c == '\n') {
            out.append("\\n");
        } else if (c == '\r') {
            out.append("\\r");
        } else if (c == '\t') {
            out.append("\\t");
        } else if (c == '\'' && !use_double) {
            out.append("\\'");
        } else if (c < 0x20 || c == 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", c);
            out.append(buf);
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    out.push_back(use_double ? '"' : '\'');
    return out;
}

// ===========================================================================
// 2. fuzz_ratio
// ===========================================================================

fuzz_ratio_result fuzz_ratio(kimix::string_view a, kimix::string_view b) {
    fuzz_ratio_result r;
    if (a.empty() || b.empty()) {
        r.score = (a == b) ? 100.0 : 0.0;
        return r;
    }
    if (a == b) {
        r.score = 100.0;
        return r;
    }
    if (is_ascii(a) && is_ascii(b)) {
        const size_t la = a.size();
        const size_t lb = b.size();
        if (la > k_fuzz_max_len || lb > k_fuzz_max_len ||
            la * lb > k_fuzz_max_cells) {
            r.status = tool_status::too_large;
            return r;
        }
        // prefix/suffix trim (distance-invariant), denominator = original lens
        size_t pre = 0;
        while (pre < la && pre < lb && a[pre] == b[pre]) {
            ++pre;
        }
        const size_t sa = la - pre;
        const size_t sb = lb - pre;
        size_t suf = 0;
        while (suf < sa && suf < sb && a[la - 1 - suf] == b[lb - 1 - suf]) {
            ++suf;
        }
        const size_t ma = sa - suf;
        const size_t mb = sb - suf;
        const uint64_t dist = edit_detail::indel_distance(
            reinterpret_cast<const uint8_t *>(a.data() + pre), ma,
            reinterpret_cast<const uint8_t *>(b.data() + pre), mb);
        r.score =
            100.0 * (1.0 - static_cast<double>(dist) /
                               static_cast<double>(la + lb));
        return r;
    }
    // Non-ASCII: code-point DP.
    kimix::vector<uint32_t> ca, cb;
    edit_detail::decode_code_points(a, ca);
    edit_detail::decode_code_points(b, cb);
    const size_t la = ca.size();
    const size_t lb = cb.size();
    if (la > k_fuzz_max_len || lb > k_fuzz_max_len ||
        la * lb > k_fuzz_max_cells) {
        r.status = tool_status::too_large;
        return r;
    }
    r.score = edit_detail::ratio_from_middle(ca.data(), la, cb.data(), lb,
                                             la + lb);
    return r;
}

// ===========================================================================
// 3. Replace kernels
// ===========================================================================

optional_text_result find_similar(kimix::string_view target,
                                  kimix::string_view content,
                                  double cutoff) {
    optional_text_result r;
    const kimix::string norm_target = normalize_newlines(target);
    const kimix::string norm_content = normalize_newlines(content);

    kimix::vector<kimix::string> lines;
    split_lines(norm_content, lines);
    if (lines.empty()) {
        return r;
    }

    edit_detail::extract_one_result best =
        edit_detail::extract_one(norm_target, lines);
    if (best.error.failed()) {
        r.error = best.error;
        return r;
    }
    if (best.found && best.score >= cutoff) {
        r.text = lines[best.index];
        return r;
    }

    kimix::vector<kimix::string> target_lines;
    split_lines(norm_target, target_lines);
    const size_t tlc = target_lines.size();
    if (tlc > 1 && lines.size() >= tlc) {
        kimix::vector<kimix::string> windows;
        windows.reserve(lines.size() - tlc + 1);
        for (size_t i = 0; i + tlc <= lines.size(); ++i) {
            windows.push_back(
                join_lf(kimix::span<const kimix::string>(lines.data() + i, tlc)));
        }
        best = edit_detail::extract_one(norm_target, windows);
        if (best.error.failed()) {
            r.error = best.error;
            return r;
        }
        if (best.found && best.score >= cutoff) {
            r.text = windows[best.index];
            return r;
        }
    }
    return r;
}

optional_text_result try_strip_match(kimix::string_view content,
                                     kimix::string_view old_text,
                                     kimix::string_view new_text) {
    optional_text_result r;
    const kimix::string old_stripped = py_strip(old_text);
    if (old_stripped.empty()) {
        return r;
    }

    kimix::vector<kimix::string> lines;
    split_lines_keepends(content, lines);
    for (const kimix::string &line : lines) {
        size_t len = line.size();
        if (len > 0 && line[len - 1] == '\n') {
            --len;
        }
        if (len > 0 && line[len - 1] == '\r') {
            --len;
        }
        const kimix::string_view core(line.data(), len);
        const size_t idx = core.find(old_stripped);
        if (idx == kimix::string_view::npos) {
            continue;
        }
        const kimix::string_view prefix = core.substr(0, idx);
        const kimix::string_view suffix = core.substr(idx + old_stripped.size());
        kimix::string ending;
        if (line.ends_with("\r\n")) {
            ending = "\r\n";
        } else if (line.ends_with("\n")) {
            ending = "\n";
        } else if (line.ends_with("\r")) {
            ending = "\r";
        }
        kimix::string new_line;
        new_line.reserve(prefix.size() + new_text.size() + suffix.size() +
                         ending.size());
        new_line.append(prefix);
        new_line.append(new_text);
        new_line.append(suffix);
        new_line.append(ending);
        r.text = edit_detail::replace_first(content, line, new_line);
        return r;
    }
    return r;
}

fuzzy_match_result best_fuzzy_match(kimix::string_view target,
                                    kimix::string_view content,
                                    double cutoff) {
    fuzzy_match_result r;
    const kimix::string norm_target = normalize_newlines(target);
    const kimix::string norm_content = normalize_newlines(content);

    kimix::vector<kimix::string> target_lines;
    split_lines(norm_target, target_lines);
    const size_t tlc = target_lines.size();

    kimix::vector<kimix::string> original_lines;
    split_lines(content, original_lines);
    kimix::vector<kimix::string> norm_lines;
    split_lines(norm_content, norm_lines);

    double best_score = 0.0;
    if (tlc == 1) {
        const size_t n = std::min(original_lines.size(), norm_lines.size());
        for (size_t i = 0; i < n; ++i) {
            const fuzz_ratio_result fr = fuzz_ratio(norm_target, norm_lines[i]);
            if (fr.status != tool_status::ok) {
                r.error.status = fr.status;
                r.error.message = "fuzz_ratio input exceeds the native length cap";
                return r;
            }
            if (fr.score > best_score) {
                best_score = fr.score;
                r.matched_original = original_lines[i];
            }
        }
    } else if (norm_lines.size() >= tlc) {
        for (size_t i = 0; i + tlc <= norm_lines.size(); ++i) {
            // original_lines mirrors norm_lines in length for LF/CRLF content
            // (Python zip(..., strict=False) truncates); guard the slice.
            if (i + tlc > original_lines.size()) {
                break;
            }
            const kimix::string window = join_lf(
                kimix::span<const kimix::string>(norm_lines.data() + i, tlc));
            const fuzz_ratio_result fr = fuzz_ratio(norm_target, window);
            if (fr.status != tool_status::ok) {
                r.error.status = fr.status;
                r.error.message = "fuzz_ratio input exceeds the native length cap";
                return r;
            }
            if (fr.score > best_score) {
                best_score = fr.score;
                r.matched_original = join_lf(kimix::span<const kimix::string>(
                    original_lines.data() + i, tlc));
            }
        }
    }
    if (best_score >= cutoff) {
        r.score = best_score;
    }
    return r;
}

replace_result apply_edit(kimix::string_view content,
                          const replace_edit_item &edit) {
    replace_result r;
    if (edit.old_text.empty() || edit.old_text == edit.new_text) {
        r.content = kimix::string(content);
        r.replacements = 0;
        return r;
    }

    const kimix::string norm_content = normalize_newlines(content);
    const kimix::string norm_old = normalize_newlines(edit.old_text);
    const kimix::string norm_new = normalize_newlines(edit.new_text);

    if (edit.replace_all) {
        const edit_detail::replace_all_out all =
            edit_detail::apply_replace_all(norm_content, norm_old, norm_new, edit);
        if (all.missed) {
            const optional_text_result sim = find_similar(edit.old_text, content);
            if (sim.error.failed()) {
                r.error = sim.error;
                return r;
            }
            r.content = kimix::string(content);
            r.replacements = 0;
            r.suggestion = sim.text;
            return r;
        }
        r.content = std::move(all.content);
        r.replacements = all.count;
        return r;
    }

    const size_t idx = norm_content.find(norm_old);
    if (idx != kimix::string::npos) {
        kimix::string result = norm_content;
        result.replace(idx, norm_old.size(), norm_new);
        r.content = std::move(result);
        r.replacements = 1;
        return r;
    }

    if (edit.match_mode == "exact") {
        const optional_text_result sim = find_similar(edit.old_text, content);
        if (sim.error.failed()) {
            r.error = sim.error;
            return r;
        }
        r.content = kimix::string(content);
        r.replacements = 0;
        r.suggestion = sim.text;
        return r;
    }

    return edit_detail::apply_fuzzy_fallback(content, norm_content, norm_old,
                                             norm_new, edit);
}

// ===========================================================================
// 4. Unified-diff kernels
// ===========================================================================

kimix::string normalize_diff(kimix::string_view diff) {
    const kimix::string text = normalize_breaks(diff);
    kimix::vector<kimix::string> lines;
    {
        kimix::vector<kimix::string> raw;
        split_lf(text, raw);
        for (const kimix::string &line : raw) {
            if (line.starts_with("\\ No newline at end of file")) {
                continue;
            }
            if (line.starts_with("diff --git") || line.starts_with("index ")) {
                continue;
            }
            if (line.starts_with("--- ") || line.starts_with("+++ ")) {
                continue;
            }
            if (line.starts_with("*** End of File")) {
                continue;
            }
            lines.push_back(line);
        }
    }
    while (!lines.empty() && py_strip(lines.front()).empty()) {
        lines.erase(lines.begin());
    }
    while (!lines.empty() && py_strip(lines.back()).empty()) {
        lines.pop_back();
    }
    return join_lf(lines);
}

kimix::string normalize_create_content(kimix::string_view diff) {
    const kimix::string text = normalize_breaks(diff);
    kimix::vector<kimix::string> lines;
    {
        kimix::vector<kimix::string> raw;
        split_lf(text, raw);
        for (const kimix::string &line : raw) {
            if (line.starts_with("\\ No newline at end of file")) {
                continue;
            }
            if (line.starts_with("+")) {
                lines.push_back(kimix::string(line.substr(1)));
            } else if (line.starts_with(" ")) {
                lines.push_back(kimix::string(line.substr(1)));
            } else {
                lines.push_back(line);
            }
        }
    }
    return join_lf(lines);
}

namespace edit_detail {

// _HEADER_RE equivalent:
//   ^@@\s+-(\d+)(?:,\d+)?\s+\+(\d+)(?:,\d+)?\s+@@(.*)$
bool match_diff_header(kimix::string_view stripped, int32_t &old_start,
                       int32_t &new_start, kimix::string &context) {
    if (!stripped.starts_with("@@")) {
        return false;
    }
    size_t i = 2;
    const auto skip_ws = [&]() {
        while (i < stripped.size() &&
               (stripped[i] == ' ' || stripped[i] == '\t')) {
            ++i;
        }
    };
    skip_ws();
    if (i >= stripped.size() || stripped[i] != '-') {
        return false;
    }
    ++i;
    const size_t old_digits = i;
    while (i < stripped.size() && stripped[i] >= '0' && stripped[i] <= '9') {
        ++i;
    }
    if (i == old_digits) {
        return false;
    }
    old_start = static_cast<int32_t>(
        parse_decimal(stripped.substr(old_digits, i - old_digits)));
    // optional ,count on the old side
    if (i < stripped.size() && stripped[i] == ',') {
        ++i;
        while (i < stripped.size() && stripped[i] >= '0' && stripped[i] <= '9') {
            ++i;
        }
    }
    skip_ws();
    if (i >= stripped.size() || stripped[i] != '+') {
        return false;
    }
    ++i;
    const size_t new_digits = i;
    while (i < stripped.size() && stripped[i] >= '0' && stripped[i] <= '9') {
        ++i;
    }
    if (i == new_digits) {
        return false;
    }
    new_start = static_cast<int32_t>(
        parse_decimal(stripped.substr(new_digits, i - new_digits)));
    // optional ,count on the new side (Python (?:,\d+)? before @@)
    if (i < stripped.size() && stripped[i] == ',') {
        ++i;
        while (i < stripped.size() && stripped[i] >= '0' && stripped[i] <= '9') {
            ++i;
        }
    }
    skip_ws();
    if (i + 1 >= stripped.size() || stripped[i] != '@' ||
        stripped[i + 1] != '@') {
        return false;
    }
    i += 2;
    context = py_strip(stripped.substr(i));
    return true;
}

} // namespace edit_detail

hunks_result parse_diff_hunks(kimix::string_view diff) {
    hunks_result r;
    const kimix::string text = normalize_diff(diff);
    if (py_strip(text).empty()) {
        return r;
    }

    kimix::vector<kimix::string> raw_lines;
    split_lf(text, raw_lines);

    struct builder {
        kimix::optional<int32_t> start_line;
        kimix::optional<kimix::string> change_context;
        kimix::vector<hunk_line> lines;
    };
    kimix::vector<diff_hunk> hunks;
    kimix::optional<builder> current;

    const auto flush = [&]() {
        if (current.has_value()) {
            bool has_change = false;
            for (const hunk_line &l : current->lines) {
                if (l.kind == hunk_line_kind::add ||
                    l.kind == hunk_line_kind::deleted) {
                    has_change = true;
                    break;
                }
            }
            if (has_change) {
                diff_hunk h;
                h.start_line = current->start_line;
                h.change_context = current->change_context;
                h.lines = std::move(current->lines);
                hunks.push_back(std::move(h));
            }
            current.reset();
        }
    };

    for (const kimix::string &raw_line : raw_lines) {
        const kimix::string_view line = raw_line;
        const kimix::string stripped = py_strip(line);

        if (stripped.starts_with("@@")) {
            flush();
            int32_t old_start = 0;
            int32_t new_start = 0;
            kimix::string header_context;
            builder b;
            if (edit_detail::match_diff_header(stripped, old_start, new_start,
                                               header_context)) {
                b.start_line = old_start;
                b.change_context =
                    header_context.empty()
                        ? kimix::optional<kimix::string>()
                        : kimix::optional<kimix::string>(header_context);
                if (new_start != 0 && old_start == 0) {
                    b.start_line = new_start;
                }
            } else {
                // Bare @@ or anchor-only header: capture text after @@ as
                // context, optionally with a leading line number.
                kimix::string after =
                    py_strip(kimix::string_view(stripped).substr(2));
                if (after.starts_with("@@")) {
                    after = py_strip(kimix::string_view(after).substr(2));
                }
                if (!after.empty()) {
                    size_t digits = 0;
                    while (digits < after.size() && after[digits] >= '0' &&
                           after[digits] <= '9') {
                        ++digits;
                    }
                    const bool word_boundary =
                        digits == after.size() ||
                        !((after[digits] >= '0' && after[digits] <= '9') ||
                          (after[digits] >= 'a' && after[digits] <= 'z') ||
                          (after[digits] >= 'A' && after[digits] <= 'Z') ||
                          after[digits] == '_');
                    if (digits > 0 && word_boundary) {
                        b.start_line = static_cast<int32_t>(edit_detail::parse_decimal(
                            kimix::string_view(after).substr(0, digits)));
                        const kimix::string rest =
                            py_strip(kimix::string_view(after).substr(digits));
                        if (!rest.empty()) {
                            b.change_context = rest;
                        }
                    } else {
                        b.change_context = after;
                    }
                }
            }
            current = std::move(b);
            continue;
        }

        if (!current.has_value()) {
            if (stripped.empty()) {
                continue;
            }
            if (stripped.starts_with("---") || stripped.starts_with("+++")) {
                continue;
            }
            r.error.status = tool_status::invalid_input;
            r.error.message = kimix::format(
                "Unexpected diff content outside a hunk: {}",
                kimix::string_view(
                    py_repr(kimix::string_view(line).substr(0, 80))));
            return r;
        }

        if (line.starts_with("+")) {
            current->lines.push_back(
                {hunk_line_kind::add, kimix::string(line.substr(1))});
        } else if (line.starts_with("-")) {
            current->lines.push_back(
                {hunk_line_kind::deleted, kimix::string(line.substr(1))});
        } else if (line.starts_with(" ")) {
            current->lines.push_back(
                {hunk_line_kind::context, kimix::string(line.substr(1))});
        } else if (stripped.empty()) {
            current->lines.push_back({hunk_line_kind::context, kimix::string()});
        } else {
            flush();
        }
    }
    flush();

    if (hunks.empty()) {
        // No hunk markers but +/- body lines: fall back to one single hunk
        // (unreachable for bodies with content because the scan above raises,
        // but kept for exact mirror fidelity).
        kimix::vector<hunk_line> body;
        for (const kimix::string &line : raw_lines) {
            if (line.starts_with("+")) {
                body.push_back({hunk_line_kind::add, kimix::string(line.substr(1))});
            } else if (line.starts_with("-")) {
                body.push_back(
                    {hunk_line_kind::deleted, kimix::string(line.substr(1))});
            } else if (line.starts_with(" ")) {
                body.push_back(
                    {hunk_line_kind::context, kimix::string(line.substr(1))});
            } else {
                if (!body.empty()) {
                    break;
                }
            }
        }
        bool has_change = false;
        for (const hunk_line &l : body) {
            if (l.kind == hunk_line_kind::add ||
                l.kind == hunk_line_kind::deleted) {
                has_change = true;
                break;
            }
        }
        if (!body.empty() && has_change) {
            diff_hunk h;
            h.start_line = std::nullopt;
            h.change_context = std::nullopt;
            h.lines = std::move(body);
            hunks.push_back(std::move(h));
        }
    }

    r.hunks = std::move(hunks);
    return r;
}

void hunk_pattern(const diff_hunk &hunk, kimix::vector<kimix::string> &out) {
    out.clear();
    for (const hunk_line &l : hunk.lines) {
        if (l.kind != hunk_line_kind::add) {
            out.push_back(l.text);
        }
    }
}

void find_exact_matches(kimix::span<const kimix::string> original_lines,
                        kimix::span<const kimix::string> pattern,
                        kimix::vector<int32_t> &out) {
    out.clear();
    if (pattern.empty()) {
        out.push_back(0);
        return;
    }
    if (pattern.size() > original_lines.size()) {
        return;
    }
    for (size_t i = 0; i + pattern.size() <= original_lines.size(); ++i) {
        bool eq = true;
        for (size_t k = 0; k < pattern.size(); ++k) {
            if (original_lines[i + k] != pattern[k]) {
                eq = false;
                break;
            }
        }
        if (eq) {
            out.push_back(static_cast<int32_t>(i));
        }
    }
}

fuzzy_location_result find_fuzzy_match(
    kimix::span<const kimix::string> original_lines,
    kimix::span<const kimix::string> pattern, double threshold) {
    fuzzy_location_result r;
    if (pattern.empty()) {
        r.index = 0;
        return r;
    }
    const size_t window_size = pattern.size();
    if (window_size > original_lines.size()) {
        return r;
    }
    const kimix::string needle = join_lf(pattern);
    kimix::vector<std::pair<size_t, double>> candidates;
    for (size_t i = 0; i + window_size <= original_lines.size(); ++i) {
        const kimix::string window = join_lf(kimix::span<const kimix::string>(
            original_lines.data() + i, window_size));
        const fuzz_ratio_result fr = fuzz_ratio(window, needle);
        if (fr.status != tool_status::ok) {
            r.error.status = fr.status;
            r.error.message = "fuzz_ratio input exceeds the native length cap";
            return r;
        }
        const double score = fr.score / 100.0;
        if (score >= threshold) {
            candidates.emplace_back(i, score);
        }
    }
    if (candidates.empty()) {
        return r;
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const std::pair<size_t, double> &x,
                 const std::pair<size_t, double> &y) { return x.second > y.second; });
    if (candidates.size() == 1) {
        r.index = static_cast<int32_t>(candidates[0].first);
        return r;
    }
    if (candidates[0].second - candidates[1].second >= 0.05) {
        r.index = static_cast<int32_t>(candidates[0].first);
    }
    return r;
}

void count_leading_whitespace(kimix::string_view line, int32_t &spaces,
                              int32_t &tabs) {
    spaces = 0;
    tabs = 0;
    for (char ch : line) {
        if (ch == ' ') {
            ++spaces;
        } else if (ch == '\t') {
            ++tabs;
        } else {
            break;
        }
    }
}

namespace edit_detail {

// CPython set-iteration order for the tiny int/char sets used here: small
// ints hash to themselves and a fresh 8-slot table scans slots 0..7, so the
// order is ascending by (hash & 7). Mirrors max(set, key=list.count) tie
// breaking for the deltas/indent-chars seen in practice.
size_t set_slot_key(int64_t v) {
    const uint64_t h = static_cast<uint64_t>(v);
    return static_cast<size_t>(h & 7u);
}

} // namespace edit_detail

void infer_indent_adjustment(kimix::span<const kimix::string> pattern_lines,
                             kimix::span<const kimix::string> actual_lines,
                             int32_t &delta, kimix::string &indent_char) {
    kimix::vector<int32_t> deltas;
    kimix::vector<char> indent_chars;
    const size_t n = std::min(pattern_lines.size(), actual_lines.size());
    for (size_t k = 0; k < n; ++k) {
        const kimix::string &p = pattern_lines[k];
        const kimix::string &a = actual_lines[k];
        if (py_strip(p).empty() || py_strip(a).empty()) {
            continue;
        }
        int32_t p_spaces = 0, p_tabs = 0, a_spaces = 0, a_tabs = 0;
        count_leading_whitespace(p, p_spaces, p_tabs);
        count_leading_whitespace(a, a_spaces, a_tabs);
        if (p_tabs != 0 && a_tabs == 0 && a_spaces != 0) {
            if (a_spaces % p_tabs == 0) {
                deltas.push_back(a_spaces - p_tabs * (a_spaces / p_tabs));
            } else {
                deltas.push_back(a_spaces - p_spaces);
            }
            indent_chars.push_back(' ');
        } else if (p_spaces != 0 && p_spaces == 0 && a_tabs != 0) {
            // Dead branch in the reference (p_spaces != 0 && p_spaces == 0),
            // kept for fidelity.
            deltas.push_back(a_tabs - p_spaces);
            indent_chars.push_back('\t');
        } else {
            deltas.push_back(a_spaces - p_spaces);
            indent_chars.push_back(' ');
        }
    }
    if (deltas.empty()) {
        delta = 0;
        indent_char = " ";
        return;
    }
    // max(set(deltas), key=deltas.count): first element in CPython set order
    // that reaches the maximum count.
    int32_t max_count = 0;
    int32_t best_delta = 0;
    size_t best_key = std::numeric_limits<size_t>::max();
    for (int32_t d : deltas) {
        const int32_t c =
            static_cast<int32_t>(std::count(deltas.begin(), deltas.end(), d));
        if (c > max_count ||
            (c == max_count && edit_detail::set_slot_key(d) < best_key)) {
            max_count = c;
            best_delta = d;
            best_key = edit_detail::set_slot_key(d);
        }
    }
    delta = best_delta;

    int32_t max_char_count = 0;
    char best_char = ' ';
    size_t best_char_key = std::numeric_limits<size_t>::max();
    for (char ch : indent_chars) {
        const int32_t c = static_cast<int32_t>(
            std::count(indent_chars.begin(), indent_chars.end(), ch));
        if (c > max_char_count ||
            (c == max_char_count &&
             edit_detail::set_slot_key(ch) < best_char_key)) {
            max_char_count = c;
            best_char = ch;
            best_char_key = edit_detail::set_slot_key(ch);
        }
    }
    indent_char = kimix::string(1, best_char);
}

kimix::string apply_indent(kimix::string_view line, int32_t delta,
                           kimix::string_view indent_char) {
    if (py_strip(line).empty()) {
        return kimix::string(line);
    }
    int32_t spaces = 0, tabs = 0;
    count_leading_whitespace(line, spaces, tabs);
    const int32_t current = spaces + tabs;
    const int32_t new_indent = std::max<int32_t>(0, current + delta);
    kimix::string out;
    out.reserve(static_cast<size_t>(new_indent) + line.size());
    for (int32_t i = 0; i < new_indent; ++i) {
        out.append(indent_char);
    }
    const size_t lead = edit_detail::leading_space_len(line);
    out.append(line.substr(lead));
    return out;
}

diff_apply_result apply_diff_hunks(kimix::span<const diff_hunk> hunks,
                                   kimix::string_view content,
                                   bool allow_fuzzy, double threshold) {
    diff_apply_result r;
    kimix::vector<kimix::string> original_lines;
    split_lf(content, original_lines);
    const bool ended_with_newline = content.ends_with("\n");

    kimix::vector<diff_hunk> sorted(hunks.begin(), hunks.end());
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const diff_hunk &x, const diff_hunk &y) {
                         const int32_t kx = x.start_line.value_or(0);
                         const int32_t ky = y.start_line.value_or(0);
                         return kx > ky;
                     });

    kimix::vector<kimix::string> working = original_lines;
    kimix::optional<int32_t> first_changed;

    for (const diff_hunk &hunk : sorted) {
        kimix::vector<kimix::string> pattern;
        hunk_pattern(hunk, pattern);

        kimix::vector<int32_t> matches;
        find_exact_matches(working, pattern, matches);
        if (matches.size() > 1) {
            r.error.status = tool_status::invalid_input;
            const kimix::string line_desc =
                hunk.start_line.has_value()
                    ? kimix::format("{}", *hunk.start_line)
                    : kimix::string("?");
            r.error.message = kimix::format(
                "Found multiple matches for hunk at line {}; "
                "add more context lines to disambiguate.",
                kimix::string_view(line_desc));
            return r;
        }

        kimix::optional<int32_t> match_index;
        if (!matches.empty()) {
            match_index = matches[0];
        }
        if (!match_index.has_value() && allow_fuzzy) {
            const fuzzy_location_result fz =
                find_fuzzy_match(working, pattern, threshold);
            if (fz.error.failed()) {
                r.error = fz.error;
                return r;
            }
            match_index = fz.index;
        }

        if (!match_index.has_value()) {
            kimix::string context;
            if (hunk.change_context.has_value()) {
                context = *hunk.change_context;
            } else {
                context = "line ";
                context += hunk.start_line.has_value()
                               ? kimix::format("{}", *hunk.start_line)
                               : kimix::string("?");
            }
            r.error.status = tool_status::invalid_input;
            r.error.message = kimix::format(
                "No match found for hunk anchored at {}.",
                kimix::string_view(context));
            return r;
        }

        const size_t mi = static_cast<size_t>(*match_index);
        const kimix::span<const kimix::string> actual(working.data() + mi,
                                                      pattern.size());
        const kimix::vector<hunk_line> adjusted =
            edit_detail::adjust_added_lines(hunk, pattern, actual);

        kimix::vector<kimix::string> replacement;
        replacement.reserve(adjusted.size());
        size_t actual_idx = 0;
        bool changed = false;
        for (const hunk_line &line : adjusted) {
            if (line.kind == hunk_line_kind::context) {
                replacement.push_back(actual[actual_idx]);
                ++actual_idx;
            } else if (line.kind == hunk_line_kind::deleted) {
                ++actual_idx;
                changed = true;
            } else {
                replacement.push_back(line.text);
                changed = true;
            }
        }

        if (changed) {
            const int32_t line_num = static_cast<int32_t>(mi + 1);
            if (!first_changed.has_value() || line_num < *first_changed) {
                first_changed = line_num;
            }
        }

        kimix::vector<kimix::string> next;
        next.reserve(working.size() - pattern.size() + replacement.size());
        next.insert(next.end(), working.begin(),
                    working.begin() + static_cast<ptrdiff_t>(mi));
        next.insert(next.end(), replacement.begin(), replacement.end());
        next.insert(next.end(),
                    working.begin() +
                        static_cast<ptrdiff_t>(mi + pattern.size()),
                    working.end());
        working.swap(next);
    }

    r.content = join_lf(working);
    restore_trailing_newline(r.content, ended_with_newline);
    r.first_changed_line = first_changed;
    return r;
}

// ===========================================================================
// 5. Hashline kernels
// ===========================================================================

kimix::string compute_line_hash(int32_t line_num, kimix::string_view line,
                                kimix::optional<kimix::string_view> prev_hash) {
    // 1. strip one trailing '\r'
    size_t len = line.size();
    if (len > 0 && line[len - 1] == '\r') {
        --len;
    }
    // 2. collect non-whitespace code points + has_significant
    kimix::string filtered;
    filtered.reserve(len);
    bool has_significant = false;
    const char *it = line.data();
    const char *end = line.data() + len;
    while (it < end) {
        const char *before = it;
        const uint32_t cp = decode_code_point(it, end);
        if (!edit_detail::is_py_space_cp(cp)) {
            filtered.append(before, static_cast<size_t>(it - before));
            if (!has_significant && edit_detail::is_alnum_cp(cp)) {
                has_significant = true;
            }
        }
    }
    // 3. seed
    uint32_t seed = 0;
    if (prev_hash.has_value()) {
        seed = 0;
        for (char c : *prev_hash) {
            seed = ((seed * 256) +
                    static_cast<uint32_t>(static_cast<unsigned char>(c))) &
                   0xFFFFFFFFu;
        }
    } else if (has_significant) {
        seed = 0; // HASH_SEED
    } else {
        seed = static_cast<uint32_t>(line_num);
    }
    // 4. xxh32 & 0xFF -> 2-char nibble string
    const uint32_t hv =
        edit_detail::xxh32(filtered.data(), filtered.size(), seed) & 0xFFu;
    return edit_detail::nibble_str(hv);
}

void compute_line_hashes(kimix::span<const kimix::string> lines,
                         kimix::vector<kimix::string> &out) {
    out.clear();
    out.reserve(lines.size());
    kimix::optional<kimix::string> prev;
    for (size_t i = 0; i < lines.size(); ++i) {
        const kimix::string h =
            compute_line_hash(static_cast<int32_t>(i + 1), lines[i], prev);
        out.push_back(h);
        prev = h;
    }
}

namespace edit_detail {

// _HEADER_RE: ^\[(?P<path>[^\]\n#]+)#(?P<tag>[^\]\n]+)\]\s*$
bool match_hashline_header(kimix::string_view stripped, kimix::string &path,
                           kimix::string &tag) {
    if (stripped.size() < 4 || stripped.front() != '[') {
        return false;
    }
    const size_t hash_pos = stripped.find('#');
    if (hash_pos == kimix::string_view::npos || hash_pos < 2) {
        return false;
    }
    if (stripped.find(']', 1) != kimix::string_view::npos &&
        stripped.find(']', 1) < hash_pos) {
        return false; // ']' inside the path part
    }
    const kimix::string_view rest = stripped.substr(hash_pos + 1);
    if (rest.size() < 2 || rest.back() != ']') {
        return false; // tag must be at least one char before ']'
    }
    if (rest.find(']') != rest.size() - 1) {
        return false; // another ']' inside the tag part
    }
    path = py_strip(stripped.substr(1, hash_pos - 1));
    tag = py_strip(rest.substr(0, rest.size() - 1));
    return true;
}

// Shared range grammar for PUT / PUT-paste / CUT:
//   \d+(?:\.=|\.)(\d+|\$) | <\s*(\d+|\$) | >\s*(\d+|\$)
struct put_range {
    bool ok = false;
    bool had_end = false; // dot form parsed an end token (digits or '$')
    kimix::optional<int32_t> start;
    kimix::optional<int32_t> end; // set when the .range form was used
    bool before = false;
    bool after = false;
};

bool parse_digits_or_dollar(kimix::string_view s, size_t &i,
                            kimix::optional<int32_t> &out) {
    if (i >= s.size()) {
        return false;
    }
    if (s[i] == '$') {
        out = std::nullopt; // $ sentinel; caller interprets
        ++i;
        return true;
    }
    const size_t start = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        ++i;
    }
    if (i == start) {
        return false;
    }
    out = static_cast<int32_t>(parse_decimal(s.substr(start, i - start)));
    return true;
}

void skip_ws(kimix::string_view s, size_t &i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
}

bool parse_put_range(kimix::string_view s, size_t &i, put_range &r) {
    if (i >= s.size()) {
        return false;
    }
    if (s[i] == '<' || s[i] == '>') {
        const bool is_before = (s[i] == '<');
        ++i;
        skip_ws(s, i);
        kimix::optional<int32_t> val;
        if (!parse_digits_or_dollar(s, i, val)) {
            return false;
        }
        r.before = is_before;
        r.after = !is_before;
        if (is_before) {
            r.start = val.has_value() ? val : kimix::optional<int32_t>(1);
        } else {
            r.start = val; // $ -> nullopt
        }
        r.ok = true;
        return true;
    }
    // digits (\.=|\.) end
    const size_t start = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        ++i;
    }
    if (i == start) {
        return false;
    }
    r.start = static_cast<int32_t>(parse_decimal(s.substr(start, i - start)));
    if (i + 1 >= s.size() || s[i] != '.') {
        return false;
    }
    if (s[i + 1] == '=') {
        i += 2;
    } else {
        i += 1;
    }
    kimix::optional<int32_t> end_val;
    if (!parse_digits_or_dollar(s, i, end_val)) {
        return false;
    }
    r.end = end_val;
    r.had_end = true;
    r.ok = true;
    return true;
}

bool match_register(kimix::string_view s, size_t &i,
                    kimix::optional<kimix::string> &reg) {
    // (?:\s*@\s*([A-Za-z_]\w*))?
    const size_t save = i;
    skip_ws(s, i);
    if (i < s.size() && s[i] == '@') {
        ++i;
        skip_ws(s, i);
        if (i < s.size() &&
            ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') ||
             s[i] == '_')) {
            const size_t start = i;
            ++i;
            while (i < s.size() &&
                   ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') ||
                    (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) {
                ++i;
            }
            reg = kimix::string(s.substr(start, i - start));
            return true;
        }
    }
    i = save;
    return false;
}

// Returns the op kind for a PUT-ish line, or false. Fills the shared fields.
struct put_parse {
    bool ok = false;
    bool paste = false;
    put_range range;
    kimix::optional<kimix::string> reg;
    size_t after_colon = 0; // index past ':' for the non-paste form
};

bool match_put(kimix::string_view s, put_parse &p) {
    // ^PUT\s+ then the range.
    if (!s.starts_with("PUT")) {
        return false;
    }
    size_t i = 3;
    skip_ws(s, i);
    if (i >= s.size() || i == 3) {
        return false; // \s+ requires at least one whitespace
    }
    put_range r;
    if (!parse_put_range(s, i, r)) {
        return false;
    }
    // _PUT_RE first: (?:\s*@\s*reg)?\s*:   (prefix match, colon required)
    put_parse try_put;
    size_t i2 = i;
    kimix::optional<kimix::string> reg2;
    match_register(s, i2, reg2);
    skip_ws(s, i2);
    if (i2 < s.size() && s[i2] == ':') {
        try_put.ok = true;
        try_put.paste = false;
        try_put.range = r;
        try_put.reg = reg2;
        try_put.after_colon = i2 + 1;
        p = try_put;
        return true;
    }
    // _PUT_PASTE_RE: \s+@\s*reg\s*$   (requires whitespace before @)
    size_t i3 = i;
    if (i3 >= s.size() || !(s[i3] == ' ' || s[i3] == '\t')) {
        return false;
    }
    kimix::optional<kimix::string> reg3;
    match_register(s, i3, reg3);
    if (!reg3.has_value()) {
        return false;
    }
    skip_ws(s, i3);
    if (i3 != s.size()) {
        return false;
    }
    put_parse try_paste;
    try_paste.ok = true;
    try_paste.paste = true;
    try_paste.range = r;
    try_paste.reg = reg3;
    try_paste.after_colon = i3;
    p = try_paste;
    return true;
}

// _CUT_RE: ^CUT\s+(\d+)(?:\.=|\.)(\d+|\$)(?:\s*@\s*reg)?\s*$
bool match_cut(kimix::string_view s, put_range &r,
               kimix::optional<kimix::string> &reg) {
    if (!s.starts_with("CUT")) {
        return false;
    }
    size_t i = 3;
    skip_ws(s, i);
    if (i >= s.size() || i == 3) {
        return false;
    }
    if (!parse_put_range(s, i, r)) {
        return false;
    }
    if (r.before || r.after || !r.had_end) {
        return false; // CUT requires the dot range form
    }
    match_register(s, i, reg);
    skip_ws(s, i);
    return i == s.size();
}

// _MV_RE: ^MV\s+(.+?)\s*$
bool match_mv(kimix::string_view s, kimix::string &dest) {
    if (!s.starts_with("MV")) {
        return false;
    }
    size_t i = 2;
    skip_ws(s, i);
    if (i >= s.size() || i == 2) {
        return false;
    }
    dest = py_strip(s.substr(i));
    return !dest.empty();
}

kimix::string edit_key(const hashline_edit &e) {
    // Matches _deduplicate_edits (hash_line.py:309-335) run AFTER
    // _normalize_edit, so delete edits are already replace-with-empty here.
    kimix::string line_key;
    if (e.op == "replace") {
        if (e.end.has_value()) {
            line_key = kimix::format("r:{}:{}", e.pos->line, e.end->line);
        } else {
            line_key = kimix::format("s:{}", e.pos->line);
        }
    } else if (e.op == "append") {
        line_key = e.pos.has_value() ? kimix::format("i:{}", e.pos->line)
                                     : kimix::string("ieof");
    } else if (e.op == "prepend") {
        line_key = e.pos.has_value() ? kimix::format("ib:{}", e.pos->line)
                                     : kimix::string("ibef");
    } else {
        line_key = kimix::format("unknown:{}",
                                 e.pos.has_value() ? e.pos->line : 0);
    }
    kimix::string body = join_lf(e.lines);
    return line_key + ":" + body;
}

bool edit_ranges_overlap(const hashline_edit &a, const hashline_edit &b,
                         size_t file_len) {
    auto range_of = [&](const hashline_edit &e)
        -> kimix::optional<std::pair<int32_t, int32_t>> {
        if (e.op == "replace") {
            const int32_t end_line = e.end.has_value() ? e.end->line : e.pos->line;
            return std::make_pair(e.pos->line, end_line);
        }
        if (e.op == "append") {
            if (e.lines.empty()) {
                return std::nullopt;
            }
            const int32_t ref =
                e.pos.has_value() ? e.pos->line : static_cast<int32_t>(file_len);
            return std::make_pair(ref + 1,
                                  ref + static_cast<int32_t>(e.lines.size()));
        }
        if (e.op == "prepend") {
            if (e.lines.empty()) {
                return std::nullopt;
            }
            const int32_t ref = e.pos.has_value() ? e.pos->line : 1;
            return std::make_pair(ref,
                                  ref + static_cast<int32_t>(e.lines.size()) - 1);
        }
        return std::nullopt;
    };
    const auto ra = range_of(a);
    const auto rb = range_of(b);
    if (!ra.has_value() || !rb.has_value()) {
        return false;
    }
    const bool intervals_overlap =
        !(ra->second < rb->first || rb->second < ra->first);
    bool same_ref_line = false;
    if ((a.op == "append" && b.op == "prepend") ||
        (a.op == "prepend" && b.op == "append")) {
        const hashline_edit *ap = (a.op == "append") ? &a : &b;
        const hashline_edit *pp = (a.op == "prepend") ? &a : &b;
        const int32_t ref_ap =
            ap->pos.has_value() ? ap->pos->line : static_cast<int32_t>(file_len);
        const int32_t ref_pp = pp->pos.has_value() ? pp->pos->line : 1;
        same_ref_line =
            ref_ap == ref_pp && ap->pos.has_value() && pp->pos.has_value();
    }
    return intervals_overlap || same_ref_line;
}

kimix::string op_name(const hashline_edit &e) {
    if (e.op == "replace") {
        return "replace";
    }
    if (e.op == "append") {
        return "append";
    }
    return "prepend";
}

void validate_anchor_ref(
    const anchor_ref &anchor, const kimix::vector<kimix::string> &file_lines,
    kimix::vector<hash_mismatch> &mismatches,
    kimix::vector<kimix::string> &validation_errors,
    const kimix::vector<kimix::string> &cumulative_hashes,
    const kimix::optional<kimix::vector<kimix::string>> &fuzzy_hashes) {
    if (anchor.line < 1) {
        validation_errors.push_back(
            kimix::format("Line {} must be >= 1", anchor.line));
        return;
    }
    if (anchor.line > static_cast<int32_t>(file_lines.size())) {
        validation_errors.push_back(kimix::format(
            "Line {} does not exist (file has {} lines)", anchor.line,
            file_lines.size()));
        return;
    }
    const kimix::string &actual_hash = cumulative_hashes[anchor.line - 1];
    if (actual_hash != anchor.hash) {
        bool fuzzy_match = false;
        if (fuzzy_hashes.has_value() &&
            (*fuzzy_hashes)[anchor.line - 1] == anchor.hash) {
            fuzzy_match = true;
        }
        if (!fuzzy_match) {
            mismatches.push_back(
                hash_mismatch{anchor.line, anchor.hash, actual_hash});
        }
    }
}

} // namespace edit_detail

parse_hashline_result parse_hashline_input(kimix::string_view input) {
    parse_hashline_result r;
    const kimix::string norm = normalize_breaks(input);
    kimix::vector<kimix::string> lines;
    split_lf(norm, lines);
    while (!lines.empty() && py_strip(lines.front()).starts_with("***")) {
        lines.erase(lines.begin());
    }
    while (!lines.empty() && py_strip(lines.back()).starts_with("***")) {
        lines.pop_back();
    }

    kimix::vector<hashline_section> sections;
    kimix::optional<hashline_section> current_section;
    kimix::vector<std::pair<int32_t, kimix::string>> current_body;

    const auto parse_body =
        [](const kimix::vector<std::pair<int32_t, kimix::string>> &body,
           kimix::vector<hashline_op> &ops, tool_error &err) {
            kimix::optional<hashline_op> pending;
            const auto flush = [&]() {
                if (pending.has_value()) {
                    while (!pending->body.empty() && pending->body.back().empty()) {
                        pending->body.pop_back();
                    }
                    ops.push_back(std::move(*pending));
                    pending.reset();
                }
            };
            for (const auto &item : body) {
                const kimix::string &raw = item.second;
                kimix::string line = raw;
                while (!line.empty() &&
                       (line.back() == '\r' || line.back() == '\n')) {
                    line.pop_back();
                }
                const kimix::string stripped = py_strip(line);

                if (stripped.empty()) {
                    if (pending.has_value()) {
                        pending->body.push_back("");
                    }
                    continue;
                }
                if (stripped.front() == '[' && stripped.back() == ']') {
                    flush();
                    continue;
                }

                edit_detail::put_parse pp;
                if (edit_detail::match_put(stripped, pp)) {
                    flush();
                    hashline_op op;
                    op.kind = hashline_op_kind::put;
                    op.line_text = stripped;
                    op.insert_where_ = insert_where::replace;
                    if (pp.range.before) {
                        op.start = pp.range.start;
                        op.insert_where_ = insert_where::before;
                    } else if (pp.range.after) {
                        op.start = pp.range.start;
                        op.insert_where_ = insert_where::after;
                    } else {
                        op.start = pp.range.start;
                        if (pp.range.end.has_value()) {
                            op.end = pp.range.end;
                        } else {
                            op.end = op.start; // '$' -> same as start
                        }
                    }
                    op.register_ = pp.reg;
                    pending = std::move(op);
                    if (pp.paste) {
                        flush();
                    }
                    continue;
                }

                edit_detail::put_range cut_range;
                kimix::optional<kimix::string> cut_reg;
                if (edit_detail::match_cut(stripped, cut_range, cut_reg)) {
                    flush();
                    hashline_op op;
                    op.kind = hashline_op_kind::cut;
                    op.line_text = stripped;
                    op.start = cut_range.start;
                    op.end = cut_range.end.has_value() ? cut_range.end : op.start;
                    op.register_ = cut_reg;
                    ops.push_back(std::move(op));
                    continue;
                }

                if (stripped == "REM") {
                    flush();
                    hashline_op op;
                    op.kind = hashline_op_kind::rem;
                    op.line_text = stripped;
                    ops.push_back(std::move(op));
                    continue;
                }

                kimix::string mv_dest;
                if (edit_detail::match_mv(stripped, mv_dest)) {
                    flush();
                    hashline_op op;
                    op.kind = hashline_op_kind::mv;
                    op.line_text = stripped;
                    op.dest = mv_dest;
                    ops.push_back(std::move(op));
                    continue;
                }

                if (pending.has_value()) {
                    if (stripped.starts_with("+")) {
                        pending->body.push_back(
                            line.size() > 1 ? line.substr(1) : kimix::string());
                    } else if (stripped.starts_with("-")) {
                        err.status = tool_status::invalid_input;
                        err.message = kimix::format(
                            "Body rows under `{}` must start with `+`; "
                            "rejecting `-` row: {}",
                            kimix::string_view(pending->line_text),
                            kimix::string_view(py_repr(line)));
                        return;
                    } else {
                        err.status = tool_status::invalid_input;
                        err.message = kimix::format(
                            "Body rows under `{}` must start with `+`; got: {}",
                            kimix::string_view(pending->line_text),
                            kimix::string_view(py_repr(line)));
                        return;
                    }
                    continue;
                }

                err.status = tool_status::invalid_input;
                err.message = kimix::format(
                    "Unexpected hashline line: {}. "
                    "Expected a section header `[path#tag]`, a `PUT ...:`, "
                    "`CUT ...`, `REM`, or `MV ...`.",
                    kimix::string_view(py_repr(line)));
                return;
            }
            flush();
        };

    int32_t line_num = 0;
    for (const kimix::string &raw : lines) {
        ++line_num;
        kimix::string line = raw;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        const kimix::string stripped = py_strip(line);
        if (stripped.empty()) {
            continue;
        }
        kimix::string path, tag;
        if (edit_detail::match_hashline_header(stripped, path, tag)) {
            if (current_section.has_value()) {
                tool_error body_err;
                kimix::vector<hashline_op> ops;
                parse_body(current_body, ops, body_err);
                if (body_err.failed()) {
                    r.error = body_err;
                    return r;
                }
                current_section->ops = std::move(ops);
                sections.push_back(std::move(*current_section));
            }
            current_section = hashline_section{path, tag, {}};
            current_body.clear();
            continue;
        }
        current_body.emplace_back(line_num, line);
    }
    if (current_section.has_value()) {
        tool_error body_err;
        kimix::vector<hashline_op> ops;
        parse_body(current_body, ops, body_err);
        if (body_err.failed()) {
            r.error = body_err;
            return r;
        }
        current_section->ops = std::move(ops);
        sections.push_back(std::move(*current_section));
    }

    if (sections.empty()) {
        r.error.status = tool_status::invalid_input;
        r.error.message = "No `[path#tag]` hashline sections found in input.";
        return r;
    }
    r.sections = std::move(sections);
    return r;
}

apply_hashline_result apply_hashline_edits(
    kimix::string_view content, kimix::span<const hashline_edit> edits) {
    apply_hashline_result r;
    if (edits.empty()) {
        r.content = kimix::string(content);
        return r;
    }

    // Normalize CRLF for fuzzy CRLF/LF matching.
    const kimix::string norm = normalize_newlines(content);
    const bool ends_with_newline = norm.ends_with("\n");
    kimix::vector<kimix::string> file_lines;
    split_lines(norm, file_lines);

    // Normalize delete edits to replace with empty lines.
    kimix::vector<hashline_edit> normalized;
    normalized.reserve(edits.size());
    for (const hashline_edit &e : edits) {
        if (e.op == "delete") {
            hashline_edit rep;
            rep.op = "replace";
            rep.pos = e.pos;
            rep.end = std::nullopt;
            rep.lines.clear();
            normalized.push_back(std::move(rep));
        } else {
            normalized.push_back(e);
        }
    }

    // Precompute cumulative hashes + CR-stripped fuzzy hashes.
    kimix::vector<kimix::string> file_hashes;
    compute_line_hashes(file_lines, file_hashes);
    kimix::optional<kimix::vector<kimix::string>> fuzzy_hashes;
    bool any_cr = false;
    for (const kimix::string &line : file_lines) {
        if (line.find('\r') != kimix::string::npos) {
            any_cr = true;
            break;
        }
    }
    if (any_cr) {
        kimix::vector<kimix::string> fuzzy_lines = file_lines;
        for (kimix::string &line : fuzzy_lines) {
            kimix::string stripped;
            stripped.reserve(line.size());
            for (char c : line) {
                if (c != '\r') {
                    stripped.push_back(c);
                }
            }
            line = std::move(stripped);
        }
        fuzzy_hashes = kimix::vector<kimix::string>();
        compute_line_hashes(fuzzy_lines, *fuzzy_hashes);
    }

    // Pre-validate.
    kimix::vector<hash_mismatch> mismatches;
    kimix::vector<kimix::string> validation_errors;
    for (const hashline_edit &e : normalized) {
        if (e.op == "replace") {
            if (e.end.has_value() && e.pos.has_value() &&
                e.pos->line > e.end->line) {
                validation_errors.push_back(kimix::format(
                    "Range start line {} must be <= end line {}", e.pos->line,
                    e.end->line));
            }
            if (e.pos.has_value()) {
                edit_detail::validate_anchor_ref(*e.pos, file_lines, mismatches,
                                                 validation_errors, file_hashes,
                                                 fuzzy_hashes);
            }
            if (e.end.has_value()) {
                edit_detail::validate_anchor_ref(*e.end, file_lines, mismatches,
                                                 validation_errors, file_hashes,
                                                 fuzzy_hashes);
            }
        } else if (e.op == "append" || e.op == "prepend") {
            if (e.pos.has_value()) {
                edit_detail::validate_anchor_ref(*e.pos, file_lines, mismatches,
                                                 validation_errors, file_hashes,
                                                 fuzzy_hashes);
            }
        }
    }
    if (!validation_errors.empty()) {
        r.error.status = tool_status::invalid_input;
        r.error.message = join_lf(validation_errors);
        return r;
    }
    if (!mismatches.empty()) {
        r.mismatches = mismatches;
        r.file_lines = file_lines;
        r.error.status = tool_status::invalid_input;
        r.error.message = hashline_mismatch_message(mismatches, file_lines);
        return r;
    }

    // Deduplicate identical edits (first occurrence wins).
    kimix::vector<hashline_edit> deduped;
    kimix::vector<kimix::string> seen_keys;
    for (const hashline_edit &e : normalized) {
        const kimix::string key = edit_detail::edit_key(e);
        bool found = false;
        for (const kimix::string &s : seen_keys) {
            if (s == key) {
                found = true;
                break;
            }
        }
        if (!found) {
            seen_keys.push_back(key);
            deduped.push_back(e);
        }
    }

    // Overlap detection.
    const size_t file_len = file_lines.size();
    kimix::vector<kimix::string> overlapping;
    for (size_t i = 0; i < deduped.size(); ++i) {
        for (size_t j = i + 1; j < deduped.size(); ++j) {
            if (!edit_detail::edit_ranges_overlap(deduped[i], deduped[j],
                                                  file_len)) {
                continue;
            }
            auto range_of = [&](const hashline_edit &e)
                -> std::pair<int32_t, int32_t> {
                if (e.op == "replace") {
                    const int32_t end_line =
                        e.end.has_value() ? e.end->line : e.pos->line;
                    return std::make_pair(e.pos->line, end_line);
                }
                if (e.op == "append") {
                    const int32_t ref =
                        e.pos.has_value() ? e.pos->line
                                          : static_cast<int32_t>(file_len);
                    return std::make_pair(
                        ref + 1, ref + static_cast<int32_t>(e.lines.size()));
                }
                const int32_t ref = e.pos.has_value() ? e.pos->line : 1;
                return std::make_pair(
                    ref, ref + static_cast<int32_t>(e.lines.size()) - 1);
            };
            const auto ri = range_of(deduped[i]);
            const auto rj = range_of(deduped[j]);
            overlapping.push_back(kimix::format(
                "  - {} at lines {}-{} overlaps with {} at lines {}-{}",
                kimix::string_view(edit_detail::op_name(deduped[i])),
                ri.first, ri.second,
                kimix::string_view(edit_detail::op_name(deduped[j])),
                rj.first, rj.second));
        }
    }
    if (!overlapping.empty()) {
        kimix::string msg = "Overlapping edits detected. "
                            "Combine overlapping edits into a single operation:\n";
        msg += join_lf(overlapping);
        r.error.status = tool_status::invalid_input;
        r.error.message = std::move(msg);
        return r;
    }

    // Sort bottom-up: (-sort_line, -original_index).
    kimix::vector<size_t> order(deduped.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    kimix::vector<int32_t> sort_line(deduped.size(), 0);
    for (size_t i = 0; i < deduped.size(); ++i) {
        const hashline_edit &e = deduped[i];
        if (e.op == "replace") {
            sort_line[i] = e.end.has_value() ? e.end->line : e.pos->line;
        } else if (e.op == "append") {
            sort_line[i] = e.pos.has_value() ? e.pos->line
                                             : static_cast<int32_t>(file_len);
        } else if (e.op == "prepend") {
            sort_line[i] = e.pos.has_value() ? e.pos->line : 0;
        }
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t x, size_t y) {
                         if (sort_line[x] != sort_line[y]) {
                             return sort_line[x] > sort_line[y];
                         }
                         return x > y;
                     });

    kimix::optional<int32_t> first_changed;
    const auto track = [&](int32_t line) {
        if (!first_changed.has_value() || line < *first_changed) {
            first_changed = line;
        }
    };

    for (size_t idx : order) {
        const hashline_edit &e = deduped[idx];
        if (e.op == "replace") {
            const int32_t count =
                e.end.has_value() ? (e.end->line - e.pos->line + 1) : 1;
            const int32_t start_idx = e.pos->line - 1;
            file_lines.erase(file_lines.begin() + start_idx,
                             file_lines.begin() + start_idx + count);
            file_lines.insert(file_lines.begin() + start_idx, e.lines.begin(),
                              e.lines.end());
            track(e.pos->line);
        } else if (e.op == "append") {
            if (e.lines.empty()) {
                continue;
            }
            if (e.pos.has_value()) {
                file_lines.insert(file_lines.begin() + e.pos->line,
                                  e.lines.begin(), e.lines.end());
                track(e.pos->line + 1);
            } else {
                if (file_lines.size() == 1 && file_lines[0].empty()) {
                    file_lines.clear();
                }
                const int32_t start_idx = static_cast<int32_t>(file_lines.size());
                file_lines.insert(file_lines.end(), e.lines.begin(), e.lines.end());
                track(start_idx + 1);
            }
        } else if (e.op == "prepend") {
            if (e.lines.empty()) {
                continue;
            }
            if (e.pos.has_value()) {
                file_lines.insert(file_lines.begin() + (e.pos->line - 1),
                                  e.lines.begin(), e.lines.end());
                track(e.pos->line);
            } else {
                if (file_lines.size() == 1 && file_lines[0].empty()) {
                    file_lines.clear();
                }
                file_lines.insert(file_lines.begin(), e.lines.begin(),
                                  e.lines.end());
                track(1);
            }
        }
    }

    r.content = join_lf(file_lines);
    restore_trailing_newline_nonempty(r.content, ends_with_newline);
    r.first_changed_line = first_changed;
    return r;
}

kimix::string hashline_mismatch_message(
    kimix::span<const hash_mismatch> mismatches,
    kimix::span<const kimix::string> file_lines) {
    if (mismatches.empty()) {
        return "";
    }
    const kimix::string lines_word = mismatches.size() > 1 ? "lines" : "line";
    kimix::string msg = kimix::format(
        "{} {} have changed since last read. "
        "Use the updated LINE#ID references shown below "
        "(>>> marks changed lines).",
        mismatches.size(), kimix::string_view(lines_word));
    msg.push_back('\n');
    msg.push_back('\n');

    kimix::vector<int32_t> mismatch_lines;
    for (const hash_mismatch &m : mismatches) {
        mismatch_lines.push_back(m.line);
    }
    kimix::vector<int32_t> display_lines;
    for (const hash_mismatch &m : mismatches) {
        const int32_t lo = std::max<int32_t>(m.line - 2, 1);
        const int32_t hi = std::min<int32_t>(m.line + 2,
                                             static_cast<int32_t>(file_lines.size()));
        for (int32_t i = lo; i <= hi; ++i) {
            display_lines.push_back(i);
        }
    }
    std::sort(display_lines.begin(), display_lines.end());
    display_lines.erase(std::unique(display_lines.begin(), display_lines.end()),
                        display_lines.end());

    kimix::vector<kimix::string> hashes;
    compute_line_hashes(file_lines, hashes);

    int32_t prev_line = 0;
    for (int32_t line_num : display_lines) {
        if (prev_line != 0 && line_num > prev_line + 1) {
            msg.append("    ...\n");
        }
        prev_line = line_num;
        const bool is_mismatch =
            std::find(mismatch_lines.begin(), mismatch_lines.end(), line_num) !=
            mismatch_lines.end();
        const kimix::string &text = file_lines[line_num - 1];
        const kimix::string &hash_str = hashes[line_num - 1];
        if (is_mismatch) {
            msg += kimix::format(">>> {}#{}:{}\n", line_num,
                                 kimix::string_view(hash_str),
                                 kimix::string_view(text));
        } else {
            msg += kimix::format("    {}#{}:{}\n", line_num,
                                 kimix::string_view(hash_str),
                                 kimix::string_view(text));
        }
    }
    // Drop the final newline (Python joins parts with "\n").
    if (!msg.empty() && msg.back() == '\n') {
        msg.pop_back();
    }
    return msg;
}

// ===========================================================================
// 6. Sloppy kernels
// ===========================================================================

parse_sloppy_result parse_sloppy_input(kimix::string_view input) {
    parse_sloppy_result r;
    const kimix::string norm = normalize_breaks(input);
    kimix::vector<kimix::string> lines;
    split_lines(norm, lines);

    kimix::vector<kimix::vector<kimix::string>> sections;
    kimix::optional<kimix::vector<kimix::string>> current;
    for (const kimix::string &line : lines) {
        const kimix::string stripped = py_strip(line);
        if (stripped.starts_with("\xC2\xA7")) {
            if (current.has_value()) {
                sections.push_back(*current);
            }
            current = kimix::vector<kimix::string>{line};
        } else if (current.has_value()) {
            current->push_back(line);
        }
    }
    if (current.has_value()) {
        sections.push_back(*current);
    }

    if (sections.empty()) {
        r.error.status = tool_status::invalid_input;
        r.error.message =
            "No sloppy operations found. Input must start with `\xC2\xA7path`.";
        return r;
    }

    kimix::vector<sloppy_op> ops;
    kimix::optional<kimix::string> current_path;
    for (const kimix::vector<kimix::string> &section : sections) {
        const kimix::string header = py_strip(section.front());
        if (!header.starts_with("\xC2\xA7")) {
            r.error.status = tool_status::invalid_input;
            r.error.message =
                "No sloppy operations found. Input must start with `\xC2\xA7path`.";
            return r;
        }
        sloppy_op op;
        size_t i = 2; // skip the section sign (2-byte UTF-8 sequence)
        op.all_match = (i < header.size() && header[i] == '*');
        if (op.all_match) {
            ++i;
        }
        kimix::string path = py_strip(header.substr(i));
        if (path.empty()) {
            path = current_path.value_or(kimix::string());
        }
        op.path = std::move(path);
        if (op.path.empty()) {
            r.error.status = tool_status::invalid_input;
            r.error.message =
                "Bare `\xC2\xA7` requires a previous section with a path.";
            return r;
        }

        kimix::vector<kimix::string> body;
        body.reserve(section.size() - 1);
        for (size_t k = 1; k < section.size(); ++k) {
            kimix::string line = section[k];
            while (!line.empty() &&
                   (line.back() == '\r' || line.back() == '\n')) {
                line.pop_back();
            }
            body.push_back(std::move(line));
        }

        // Block rewrite separator ">>" (U+00BB) on its own line.
        size_t sep_idx = body.size();
        for (size_t k = 0; k < body.size(); ++k) {
            if (py_strip(body[k]) == "\xC2\xBB") {
                sep_idx = k;
                break;
            }
        }
        if (sep_idx != body.size()) {
            op.match_lines.assign(body.begin(), body.begin() + sep_idx);
            op.rewrite_lines = kimix::vector<kimix::string>();
            op.rewrite_lines->assign(body.begin() + sep_idx + 1, body.end());
            current_path = op.path;
            ops.push_back(std::move(op));
        } else {
            // Inline operation.
            for (const kimix::string &line : body) {
                if (py_strip(line).empty()) {
                    continue;
                }
                sloppy_inline_line entry;
                entry.line = line;
                kimix::string remaining = line;
                while (true) {
                    const size_t open_pos = remaining.find("\xE2\x9F\xAA");
                    if (open_pos == kimix::string::npos) {
                        break;
                    }
                    // old: [^|>>\n]+ up to the first | or >>
                    size_t p = open_pos + 3;
                    size_t bar_pos = kimix::string::npos;
                    size_t close_pos = kimix::string::npos;
                    while (p < remaining.size()) {
                        if (static_cast<unsigned char>(remaining[p]) == 0xE2 && p + 2 < remaining.size() &&
                            static_cast<unsigned char>(remaining[p + 1]) == 0x94 &&
                            static_cast<unsigned char>(remaining[p + 2]) == 0x82) {
                            bar_pos = p;
                            break;
                        }
                        if (static_cast<unsigned char>(remaining[p]) == 0xE2 && p + 2 < remaining.size() &&
                            static_cast<unsigned char>(remaining[p + 1]) == 0x9F &&
                            static_cast<unsigned char>(remaining[p + 2]) == 0xAB) {
                            close_pos = p;
                            break;
                        }
                        ++p;
                    }
                    if (bar_pos == kimix::string::npos ||
                        bar_pos == open_pos + 3) {
                        // no '|' or empty old: no match at this <<; keep scanning
                        remaining.erase(0, open_pos + 3);
                        continue;
                    }
                    // new: [^>>\n]* up to the first >>
                    const size_t close = remaining.find("\xE2\x9F\xAB",
                                                        bar_pos + 3);
                    if (close == kimix::string::npos) {
                        break;
                    }
                    const kimix::string old_text =
                        remaining.substr(open_pos + 3, bar_pos - open_pos - 3);
                    const kimix::string new_text =
                        remaining.substr(bar_pos + 3, close - bar_pos - 3);
                    entry.selections.push_back(sloppy_inline{old_text, new_text});
                    // remove the whole <<...>> span and rescan
                    remaining.erase(open_pos, close + 3 - open_pos);
                }
                op.inline_lines.push_back(std::move(entry));
            }
            current_path = op.path;
            ops.push_back(std::move(op));
        }
    }

    r.ops = std::move(ops);
    return r;
}

kimix::optional<byte_range> find_exact_block(
    kimix::string_view content, kimix::span<const kimix::string> block_lines) {
    if (block_lines.empty()) {
        return std::nullopt;
    }
    const kimix::string needle = join_lf(block_lines);
    const size_t start = content.find(needle);
    if (start == kimix::string_view::npos) {
        return std::nullopt;
    }
    return byte_range{static_cast<uint64_t>(start),
                      static_cast<uint64_t>(start + needle.size())};
}

fuzzy_block_result find_fuzzy_block(
    kimix::string_view content, kimix::span<const kimix::string> block_lines,
    double threshold) {
    fuzzy_block_result r;
    if (block_lines.empty()) {
        return r;
    }
    kimix::vector<kimix::string> content_lines;
    split_lines(content, content_lines);
    const size_t window_size = block_lines.size();
    if (window_size > content_lines.size()) {
        return r;
    }
    const kimix::string needle = join_lf(block_lines);
    double best_score = 0.0;
    size_t best_idx = 0;
    for (size_t i = 0; i + window_size <= content_lines.size(); ++i) {
        const kimix::string window = join_lf(kimix::span<const kimix::string>(
            content_lines.data() + i, window_size));
        const fuzz_ratio_result fr = fuzz_ratio(window, needle);
        if (fr.status != tool_status::ok) {
            r.error.status = fr.status;
            r.error.message = "fuzz_ratio input exceeds the native length cap";
            return r;
        }
        const double score = fr.score / 100.0;
        if (score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }
    if (best_score < threshold) {
        return r;
    }
    size_t start_char = 0;
    for (size_t i = 0; i < best_idx; ++i) {
        start_char += content_lines[i].size() + 1;
    }
    size_t end_char = start_char;
    for (size_t i = best_idx; i < best_idx + window_size; ++i) {
        end_char += content_lines[i].size() + 1;
    }
    end_char -= 1;
    r.range = byte_range{static_cast<uint64_t>(start_char),
                         static_cast<uint64_t>(end_char)};
    return r;
}

sloppy_apply_result apply_block_op(kimix::string_view content,
                                   const sloppy_op &op) {
    sloppy_apply_result r;
    if (op.match_lines.empty()) {
        r.error.status = tool_status::invalid_input;
        r.error.message = "MATCH block is empty.";
        return r;
    }

    const kimix::string replacement = join_lf(*op.rewrite_lines);
    kimix::optional<byte_range> found =
        find_exact_block(content, op.match_lines);
    if (!found.has_value()) {
        const fuzzy_block_result fz =
            find_fuzzy_block(content, op.match_lines, 0.75);
        if (fz.error.failed()) {
            r.error = fz.error;
            return r;
        }
        found = fz.range;
    }
    if (!found.has_value()) {
        kimix::string msg = "Could not locate MATCH block:\n";
        msg += join_lf(op.match_lines);
        r.error.status = tool_status::invalid_input;
        r.error.message = std::move(msg);
        return r;
    }

    size_t start = static_cast<size_t>(found->begin);
    size_t end = static_cast<size_t>(found->end);
    if (replacement.empty() && end < content.size() && content[end] == '\n') {
        ++end;
    }

    if (op.all_match) {
        const kimix::string needle = join_lf(op.match_lines);
        kimix::string result;
        size_t prev = 0;
        while (true) {
            const size_t pos = content.find(needle, prev);
            if (pos == kimix::string_view::npos) {
                break;
            }
            size_t end_pos = pos + needle.size();
            if (replacement.empty() && end_pos < content.size() &&
                content[end_pos] == '\n') {
                ++end_pos;
            }
            result.append(content.substr(prev, pos - prev));
            result.append(replacement);
            prev = end_pos;
        }
        result.append(content.substr(prev));
        r.content = std::move(result);
        return r;
    }

    r.content = kimix::string(content.substr(0, start)) + replacement +
                kimix::string(content.substr(end));
    return r;
}

sloppy_apply_result apply_inline_op(kimix::string_view content,
                                    const sloppy_op &op) {
    sloppy_apply_result r;
    if (op.inline_lines.empty()) {
        r.content = kimix::string(content);
        return r;
    }
    kimix::vector<sloppy_inline> selections;
    for (const sloppy_inline_line &entry : op.inline_lines) {
        selections.insert(selections.end(), entry.selections.begin(),
                          entry.selections.end());
    }
    if (selections.empty()) {
        r.content = kimix::string(content);
        return r;
    }

    if (op.all_match) {
        kimix::string result(content);
        for (const sloppy_inline &sel : selections) {
            result = edit_detail::replace_all(result, sel.old_text, sel.new_text);
        }
        r.content = std::move(result);
        return r;
    }

    kimix::string result(content);
    for (const sloppy_inline &sel : selections) {
        if (result.find(sel.old_text) == kimix::string::npos) {
            r.error.status = tool_status::invalid_input;
            r.error.message = kimix::format(
                "Could not locate inline selection: {}",
                kimix::string_view(py_repr(sel.old_text)));
            return r;
        }
        result = edit_detail::replace_first(result, sel.old_text, sel.new_text);
    }
    r.content = std::move(result);
    return r;
}

sloppy_apply_result apply_sloppy_op(kimix::string_view content,
                                    const sloppy_op &op) {
    if (op.rewrite_lines.has_value()) {
        return apply_block_op(content, op);
    }
    return apply_inline_op(content, op);
}

// ===========================================================================
// 7. Tool class and standard integration
// ===========================================================================

namespace edit_detail {

const char *status_name(tool_status s) {
    switch (s) {
    case tool_status::ok:
        return "ok";
    case tool_status::invalid_input:
        return "invalid_input";
    case tool_status::not_found:
        return "not_found";
    case tool_status::no_change:
        return "no_change";
    case tool_status::ambiguous:
        return "ambiguous";
    case tool_status::blocked:
        return "blocked";
    case tool_status::too_large:
        return "too_large";
    case tool_status::unsupported:
        return "unsupported";
    case tool_status::external_library:
        return "external_library";
    }
    return "unsupported";
}

ValueElement make_null_or_string(const kimix::string &s) {
    if (s.empty()) {
        return ValueElement::make_null();
    }
    return ValueElement::make_string(s);
}

ValueElement make_null_or_string(const kimix::optional<kimix::string> &s) {
    if (!s.has_value()) {
        return ValueElement::make_null();
    }
    return ValueElement::make_string(*s);
}

bool get_string(const ToolParams *params, kimix::string_view key,
                kimix::string &out) {
    const ValueElement *e = params->get(key);
    if (e == nullptr || e->is_null()) {
        return false;
    }
    if (!e->is_string()) {
        return false;
    }
    out = e->as_string();
    return true;
}

bool require_string(const ToolParams *params, kimix::string_view key,
                    kimix::string &out, tool_error &err) {
    if (!get_string(params, key, out)) {
        err.status = tool_status::invalid_input;
        err.message = kimix::format("Missing or invalid required parameter: {}", key);
        return false;
    }
    return true;
}

bool get_bool(const ToolParams *params, kimix::string_view key, bool &out) {
    const ValueElement *e = params->get(key);
    if (e == nullptr || e->is_null()) {
        return false;
    }
    if (!e->is_bool()) {
        return false;
    }
    out = e->as_bool();
    return true;
}

bool get_int(const ToolParams *params, kimix::string_view key, int64_t &out) {
    const ValueElement *e = params->get(key);
    if (e == nullptr || e->is_null()) {
        return false;
    }
    if (e->is_int()) {
        out = e->as_int();
        return true;
    }
    if (e->is_uint()) {
        out = static_cast<int64_t>(e->as_uint());
        return true;
    }
    return false;
}

bool get_real(const ToolParams *params, kimix::string_view key, double &out) {
    const ValueElement *e = params->get(key);
    if (e == nullptr || e->is_null()) {
        return false;
    }
    if (e->is_real()) {
        out = e->as_real();
        return true;
    }
    if (e->is_int()) {
        out = static_cast<double>(e->as_int());
        return true;
    }
    if (e->is_uint()) {
        out = static_cast<double>(e->as_uint());
        return true;
    }
    return false;
}

bool get_array(const ToolParams *params, kimix::string_view key,
               const ValueElement::Array *&out) {
    const ValueElement *e = params->get(key);
    if (e == nullptr || e->is_null()) {
        return false;
    }
    if (!e->is_array()) {
        return false;
    }
    out = &e->as_array();
    return true;
}

bool parse_replace_edit_item(const ValueElement &elem, replace_edit_item &out,
                             tool_error &err) {
    if (!elem.is_object()) {
        err.status = tool_status::invalid_input;
        err.message = "Each edit item must be a JSON object.";
        return false;
    }
    const ToolParams *obj = elem.as_object();
    if (obj == nullptr) {
        err.status = tool_status::invalid_input;
        err.message = "Each edit item must be a JSON object.";
        return false;
    }
    if (!get_string(obj, "old_string", out.old_text)) {
        if (!get_string(obj, "old", out.old_text)) {
            err.status = tool_status::invalid_input;
            err.message = "Replace edit item missing 'old_string'.";
            return false;
        }
    }
    if (!get_string(obj, "new_string", out.new_text)) {
        if (!get_string(obj, "new", out.new_text)) {
            err.status = tool_status::invalid_input;
            err.message = "Replace edit item missing 'new_string'.";
            return false;
        }
    }
    bool unused = false;
    get_bool(obj, "replace_all", out.replace_all);
    int64_t max_repl = 0;
    if (get_int(obj, "max_replacements", max_repl) && max_repl > 0) {
        out.max_replacements = static_cast<size_t>(max_repl);
    }
    kimix::string mode;
    if (get_string(obj, "match_mode", mode)) {
        if (mode == "exact" || mode == "fuzzy") {
            out.match_mode = std::move(mode);
        }
    }
    return true;
}

bool parse_replace_edits(const ToolParams *params,
                         kimix::vector<replace_edit_item> &out,
                         tool_error &err) {
    const ValueElement::Array *arr = nullptr;
    if (get_array(params, "edits", arr) || get_array(params, "edit", arr)) {
        out.reserve(arr->size());
        for (const ValueElement &elem : *arr) {
            replace_edit_item item;
            if (!parse_replace_edit_item(elem, item, err)) {
                return false;
            }
            out.push_back(std::move(item));
        }
        return true;
    }

    replace_edit_item item;
    if (!get_string(params, "old_string", item.old_text)) {
        if (!get_string(params, "old", item.old_text)) {
            err.status = tool_status::invalid_input;
            err.message = "Replace mode requires 'edits', 'edit', 'old_string', or 'old'.";
            return false;
        }
    }
    if (!get_string(params, "new_string", item.new_text)) {
        if (!get_string(params, "new", item.new_text)) {
            err.status = tool_status::invalid_input;
            err.message = "Replace mode requires 'new_string' or 'new'.";
            return false;
        }
    }
    bool unused = false;
    get_bool(params, "replace_all", item.replace_all);
    int64_t max_repl = 0;
    if (get_int(params, "max_replacements", max_repl) && max_repl > 0) {
        item.max_replacements = static_cast<size_t>(max_repl);
    }
    kimix::string mode;
    if (get_string(params, "match_mode", mode)) {
        if (mode == "exact" || mode == "fuzzy") {
            item.match_mode = std::move(mode);
        }
    }
    out.push_back(std::move(item));
    return true;
}

bool parse_anchor_ref(const ValueElement &elem, anchor_ref &out, tool_error &err) {
    if (!elem.is_object()) {
        err.status = tool_status::invalid_input;
        err.message = "Anchor reference must be a JSON object.";
        return false;
    }
    const ToolParams *obj = elem.as_object();
    if (obj == nullptr) {
        err.status = tool_status::invalid_input;
        err.message = "Anchor reference must be a JSON object.";
        return false;
    }
    int64_t line = 0;
    if (!get_int(obj, "line", line) || line < 1) {
        err.status = tool_status::invalid_input;
        err.message = "Anchor reference missing positive 'line'.";
        return false;
    }
    out.line = static_cast<int32_t>(line);
    if (!get_string(obj, "hash", out.hash)) {
        err.status = tool_status::invalid_input;
        err.message = "Anchor reference missing 'hash'.";
        return false;
    }
    return true;
}

bool parse_hashline_edit(const ValueElement &elem, hashline_edit &out,
                         tool_error &err) {
    if (!elem.is_object()) {
        err.status = tool_status::invalid_input;
        err.message = "Each hashline edit item must be a JSON object.";
        return false;
    }
    const ToolParams *obj = elem.as_object();
    if (obj == nullptr) {
        err.status = tool_status::invalid_input;
        err.message = "Each hashline edit item must be a JSON object.";
        return false;
    }
    kimix::string op;
    if (!get_string(obj, "op", op)) {
        err.status = tool_status::invalid_input;
        err.message = "Hashline edit item missing 'op'.";
        return false;
    }
    out.op = std::move(op);

    const ValueElement *pos_elem = obj->get("pos");
    if (pos_elem != nullptr && !pos_elem->is_null()) {
        out.pos = anchor_ref();
        if (!parse_anchor_ref(*pos_elem, *out.pos, err)) {
            return false;
        }
    }

    const ValueElement *end_elem = obj->get("end");
    if (end_elem != nullptr && !end_elem->is_null()) {
        out.end = anchor_ref();
        if (!parse_anchor_ref(*end_elem, *out.end, err)) {
            return false;
        }
    }

    const ValueElement::Array *arr = nullptr;
    if (get_array(obj, "lines", arr)) {
        out.lines.reserve(arr->size());
        for (const ValueElement &line_elem : *arr) {
            if (!line_elem.is_string()) {
                err.status = tool_status::invalid_input;
                err.message = "Hashline edit 'lines' must be an array of strings.";
                return false;
            }
            out.lines.push_back(line_elem.as_string());
        }
    }
    return true;
}

bool parse_hashline_edits(const ToolParams *params,
                          kimix::vector<hashline_edit> &out, tool_error &err) {
    const ValueElement::Array *arr = nullptr;
    if (!get_array(params, "edits", arr) && !get_array(params, "edit", arr)) {
        err.status = tool_status::invalid_input;
        err.message = "Hashline mode requires 'edits' or 'edit' array.";
        return false;
    }
    out.reserve(arr->size());
    for (const ValueElement &elem : *arr) {
        hashline_edit item;
        if (!parse_hashline_edit(elem, item, err)) {
            return false;
        }
        out.push_back(std::move(item));
    }
    return true;
}

void set_error(ToolParams &result, const tool_error &err) {
    result.values["status"] =
        ValueElement::make_string(kimix::string(status_name(err.status)));
    result.values["message"] = ValueElement::make_string(err.message);
}

void set_replace_result(ToolParams &result, const replace_result &r) {
    result.values["content"] = ValueElement::make_string(r.content);
    result.values["replacements"] = ValueElement::make_int(
        static_cast<int64_t>(r.replacements));
    result.values["suggestion"] = make_null_or_string(r.suggestion);
}

void set_diff_result(ToolParams &result, const diff_apply_result &r) {
    result.values["content"] = ValueElement::make_string(r.content);
    if (r.first_changed_line.has_value()) {
        result.values["first_changed_line"] = ValueElement::make_int(
            static_cast<int64_t>(*r.first_changed_line));
    } else {
        result.values["first_changed_line"] = ValueElement::make_null();
    }
}

void set_hashline_result(ToolParams &result, const apply_hashline_result &r) {
    result.values["content"] = ValueElement::make_string(r.content);
    if (r.first_changed_line.has_value()) {
        result.values["first_changed_line"] = ValueElement::make_int(
            static_cast<int64_t>(*r.first_changed_line));
    } else {
        result.values["first_changed_line"] = ValueElement::make_null();
    }
}

void set_sloppy_result(ToolParams &result, const sloppy_apply_result &r) {
    result.values["content"] = ValueElement::make_string(r.content);
}

void clear_result(ToolParams &result) {
    result.values.clear();
    result.values["status"] = ValueElement::make_string(kimix::string("ok"));
    result.values["message"] = ValueElement::make_string(kimix::string());
    result.values["content"] = ValueElement::make_string(kimix::string());
}

} // namespace edit_detail

Edit::Edit(kimix::builtin_tools::Session *session)
    : kimix::builtin_tools::Tool(session) {}

void Edit::operator()(kimix::builtin_tools::ToolParams const *parameters) {
    try {
        edit_detail::clear_result(_result);
        if (parameters == nullptr) {
            _result.values["status"] =
                ValueElement::make_string(kimix::string("invalid_input"));
            _result.values["message"] =
                ValueElement::make_string(kimix::string("Parameters are null."));
            return;
        }

        kimix::string mode;
        tool_error mode_err;
        if (!edit_detail::require_string(parameters, "mode", mode, mode_err)) {
            edit_detail::set_error(_result, mode_err);
            return;
        }

        kimix::string content;
        tool_error content_err;
        if (!edit_detail::require_string(parameters, "content", content,
                                         content_err)) {
            edit_detail::set_error(_result, content_err);
            return;
        }

        if (mode == "replace") {
        kimix::vector<replace_edit_item> edits;
        tool_error err;
        if (!edit_detail::parse_replace_edits(parameters, edits, err)) {
            edit_detail::set_error(_result, err);
            _result.values["content"] = ValueElement::make_string(content);
            _result.values["replacements"] = ValueElement::make_int(0);
            _result.values["suggestion"] = ValueElement::make_null();
            return;
        }
        kimix::string text = content;
        size_t total = 0;
        kimix::optional<kimix::string> last_suggestion;
        for (const replace_edit_item &edit : edits) {
            replace_result rr = apply_edit(text, edit);
            if (rr.error.failed()) {
                edit_detail::set_error(_result, rr.error);
                _result.values["content"] = ValueElement::make_string(text);
                _result.values["replacements"] =
                    ValueElement::make_int(static_cast<int64_t>(total));
                _result.values["suggestion"] =
                    edit_detail::make_null_or_string(last_suggestion);
                return;
            }
            text = std::move(rr.content);
            total += rr.replacements;
            if (rr.suggestion.has_value()) {
                last_suggestion = std::move(*rr.suggestion);
            }
        }
        replace_result combined;
        combined.content = std::move(text);
        combined.replacements = total;
        combined.suggestion = std::move(last_suggestion);
        edit_detail::set_replace_result(_result, combined);
        return;
    }

    if (mode == "patch") {
        kimix::string diff_text;
        tool_error err;
        if (!edit_detail::require_string(parameters, "diff", diff_text, err)) {
            edit_detail::set_error(_result, err);
            _result.values["content"] = ValueElement::make_string(content);
            _result.values["first_changed_line"] = ValueElement::make_null();
            return;
        }
        hunks_result parsed = parse_diff_hunks(diff_text);
        if (parsed.error.failed()) {
            edit_detail::set_error(_result, parsed.error);
            _result.values["content"] = ValueElement::make_string(content);
            _result.values["first_changed_line"] = ValueElement::make_null();
            return;
        }
        bool allow_fuzzy = true;
        double threshold = 0.75;
        bool unused = false;
        if (edit_detail::get_bool(parameters, "allow_fuzzy", unused)) {
            allow_fuzzy = unused;
        }
        if (!edit_detail::get_real(parameters, "threshold", threshold)) {
            int64_t threshold_int = 0;
            if (edit_detail::get_int(parameters, "threshold", threshold_int)) {
                threshold = static_cast<double>(threshold_int);
            }
        }
        const diff_apply_result applied =
            apply_diff_hunks(parsed.hunks, content, allow_fuzzy, threshold);
        if (applied.error.failed()) {
            edit_detail::set_error(_result, applied.error);
        } else {
            edit_detail::set_diff_result(_result, applied);
        }
        return;
    }

    if (mode == "hashline") {
        kimix::vector<hashline_edit> edits;
        tool_error err;
        if (!edit_detail::parse_hashline_edits(parameters, edits, err)) {
            edit_detail::set_error(_result, err);
            _result.values["content"] = ValueElement::make_string(content);
            _result.values["first_changed_line"] = ValueElement::make_null();
            return;
        }
        apply_hashline_result applied = apply_hashline_edits(content, edits);
        if (applied.error.failed()) {
            edit_detail::set_error(_result, applied.error);
            _result.values["content"] = ValueElement::make_string(applied.content);
        } else {
            edit_detail::set_hashline_result(_result, applied);
        }
        return;
    }

    if (mode == "sloppy") {
        kimix::string input;
        tool_error err;
        if (!edit_detail::require_string(parameters, "input", input, err)) {
            edit_detail::set_error(_result, err);
            _result.values["content"] = ValueElement::make_string(content);
            return;
        }
        parse_sloppy_result parsed = parse_sloppy_input(input);
        if (parsed.error.failed()) {
            edit_detail::set_error(_result, parsed.error);
            _result.values["content"] = ValueElement::make_string(content);
            return;
        }
        kimix::string text = content;
        for (const sloppy_op &op : parsed.ops) {
            sloppy_apply_result applied = apply_sloppy_op(text, op);
            if (applied.error.failed()) {
                edit_detail::set_error(_result, applied.error);
                _result.values["content"] = ValueElement::make_string(text);
                return;
            }
            text = std::move(applied.content);
        }
        sloppy_apply_result combined;
        combined.content = std::move(text);
        edit_detail::set_sloppy_result(_result, combined);
        return;
    }

    tool_error err;
    err.status = tool_status::invalid_input;
    err.message = kimix::format("Unsupported edit mode: {}", mode);
    edit_detail::set_error(_result, err);
    _result.values["content"] = ValueElement::make_string(content);
    } catch (const std::exception &e) {
        tool_error err;
        err.status = tool_status::unsupported;
        err.message = kimix::format("Edit tool error: {}", kimix::string_view(e.what()));
        edit_detail::set_error(_result, err);
    }
}

kimix::builtin_tools::ToolParams const &Edit::last_result() const noexcept {
    return _result;
}

} // namespace kimix::builtin_tools::edit
