// fetch_url_tool.cpp - fetch_url builtin agent tool kernels (C++ port).
//
// Source of truth (Python):
//   * C:/dev/kimi-agent/src/kimix/tools/web/web_fetcher/fetcher.py
//   * C:/dev/kimi-agent/kimi-cli/src/kimi_cli/tools/web/url_safety.py
//   * C:/dev/kimi-agent/kimi-cli/src/kimi_cli/tools/web/fetch.py
//   * bs4 4.15 html.parser builder + markdownify 1.2.3 (vendored in
//     C:/dev/kimi-agent/.venv) -- behavior captured via Python goldens.
//
// See fetch_url_tool.h for the API contract. Unity-safe: every helper below
// lives inside namespace kimix::builtin_tools::fetch_url and is static.

#include <cstdint>
#include <cstring>

#include <algorithm>

#include <core/kimix_core.h>

#include "builtin_tools/fetch_url_tool.h"
#include "builtin_tools/utf8_util.h"

namespace kimix::builtin_tools::fetch_url {

namespace {

// ---------------------------------------------------------------------------
// Small ASCII helpers
// ---------------------------------------------------------------------------

inline char ascii_lower_char(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool ascii_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

inline bool ascii_digit(char c) { return c >= '0' && c <= '9'; }

inline bool ascii_alnum(char c) { return ascii_alpha(c) || ascii_digit(c); }

bool ascii_iequals(kimix::string_view a, kimix::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower_char(a[i]) != ascii_lower_char(b[i])) return false;
    }
    return true;
}

void ascii_lower_into(kimix::string_view src, kimix::string &out) {
    out.clear();
    out.reserve(src.size());
    for (char c : src) out.push_back(ascii_lower_char(c));
}

// ---------------------------------------------------------------------------
// UTF-8 helpers
// ---------------------------------------------------------------------------

void append_utf8(kimix::string &out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Python str.isspace() code-point set (used for str.strip() approximation).
bool is_python_space_cp(uint32_t cp) {
    if (cp == 0x20 || cp == 0x09 || cp == 0x0A || cp == 0x0B || cp == 0x0C ||
        cp == 0x0D) {
        return true;
    }
    if (cp >= 0x1C && cp <= 0x1F) return true;
    if (cp == 0x85 || cp == 0xA0) return true;
    if (cp == 0x1680) return true;
    if (cp >= 0x2000 && cp <= 0x200A) return true;
    if (cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F) {
        return true;
    }
    return cp == 0x3000;
}

// Python str.strip(chars) semantics where chars is a UTF-8 string of ASCII
// characters: strip left/right occurrences of any of those bytes.
void strip_chars(kimix::string_view s, kimix::string_view chars, bool left,
                 bool right, kimix::string &out) {
    size_t begin = 0;
    size_t end = s.size();
    if (left) {
        while (begin < end && chars.find(s[begin]) != kimix::string_view::npos) {
            ++begin;
        }
    }
    if (right) {
        while (end > begin && chars.find(s[end - 1]) != kimix::string_view::npos) {
            --end;
        }
    }
    out.assign(s.substr(begin, end - begin));
}

// Python str.strip() (Unicode whitespace), code-point aware.
void strip_python_ws(kimix::string_view s, bool left, bool right,
                     kimix::string &out) {
    size_t begin = 0;
    size_t end = s.size();
    if (left) {
        while (begin < end) {
            const char *it = s.data() + begin;
            uint32_t cp = decode_code_point(it, s.data() + end);
            if (!is_python_space_cp(cp)) break;
            begin = static_cast<size_t>(it - s.data());
        }
    }
    if (right) {
        while (end > begin) {
            // Walk back to the start of the last code point (the first
            // non-continuation byte before `end`).
            size_t p = end - 1;
            while (p > begin &&
                   (static_cast<unsigned char>(s[p - 1]) & 0xC0) == 0x80) {
                --p;
            }
            const char *it = s.data() + p;
            uint32_t cp = decode_code_point(it, s.data() + end);
            if (!is_python_space_cp(cp)) break;
            end = p;
        }
    }
    out.assign(s.substr(begin, end - begin));
}

bool has_non_ws(kimix::string_view s) {
    size_t pos = 0;
    while (pos < s.size()) {
        const char *it = s.data() + pos;
        uint32_t cp = decode_code_point(it, s.data() + s.size());
        if (!is_python_space_cp(cp)) return true;
        pos = static_cast<size_t>(it - s.data());
    }
    return false;
}

// All bytes are ASCII whitespace as defined by bs4's ASCII_SPACES
// ("\x20\x0a\x09\x0c\x0d"); note \v is deliberately excluded (Python's
// `i not in ASCII_SPACES` keeps a segment containing \v as-is).
bool all_ascii_ws(kimix::string_view s) {
    for (char c : s) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') {
            return false;
        }
    }
    return true;
}

// re_all_whitespace = [\t \r\n]+ -> " "
void collapse_all_ws_to_space(kimix::string_view s, kimix::string &out) {
    out.clear();
    out.reserve(s.size());
    bool in_ws = false;
    for (char c : s) {
        if (c == '\t' || c == ' ' || c == '\r' || c == '\n') {
            in_ws = true;
        } else {
            if (in_ws) {
                out.push_back(' ');
                in_ws = false;
            }
            out.push_back(c);
        }
    }
    if (in_ws) out.push_back(' ');
}

// re_newline_whitespace = [\t \r\n]*[\r\n][\t \r\n]* -> "\n"
void collapse_newline_ws(kimix::string_view s, kimix::string &out) {
    out.clear();
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c == '\t' || c == ' ' || c == '\r' || c == '\n') {
            size_t j = i;
            bool has_nl = false;
            while (j < s.size() &&
                   (s[j] == '\t' || s[j] == ' ' || s[j] == '\r' || s[j] == '\n')) {
                if (s[j] == '\n') has_nl = true;
                ++j;
            }
            if (has_nl) {
                out.push_back('\n');
            } else {
                out.append(s.substr(i, j - i));
            }
            i = j;
        } else {
            out.push_back(c);
            ++i;
        }
    }
}

// re_whitespace = [\t ]+ -> " "
void collapse_space_tabs(kimix::string_view s, kimix::string &out) {
    out.clear();
    out.reserve(s.size());
    bool in_ws = false;
    for (char c : s) {
        if (c == '\t' || c == ' ') {
            in_ws = true;
        } else {
            if (in_ws) {
                out.push_back(' ');
                in_ws = false;
            }
            out.push_back(c);
        }
    }
    if (in_ws) out.push_back(' ');
}

// ---------------------------------------------------------------------------
// Named entity table (HTML5 table from bs4 dammit, sorted by name)
// ---------------------------------------------------------------------------
struct entity_pair {
    const char *name;
    const char *value; // UTF-8, NUL-terminated
};

const entity_pair k_entities[] = {
    {"AElig", "\xC3\x86"},
    {"AMP", "\x26"},
    {"Aacute", "\xC3\x81"},
    {"Abreve", "\xC4\x82"},
    {"Acirc", "\xC3\x82"},
    {"Acy", "\xD0\x90"},
    {"Afr", "\xF0\x9D\x94\x84"},
    {"Agrave", "\xC3\x80"},
    {"Alpha", "\xCE\x91"},
    {"Amacr", "\xC4\x80"},
    {"And", "\xE2\xA9\x93"},
    {"Aogon", "\xC4\x84"},
    {"Aopf", "\xF0\x9D\x94\xB8"},
    {"ApplyFunction", "\xE2\x81\xA1"},
    {"Aring", "\xC3\x85"},
    {"Ascr", "\xF0\x9D\x92\x9C"},
    {"Assign", "\xE2\x89\x94"},
    {"Atilde", "\xC3\x83"},
    {"Auml", "\xC3\x84"},
    {"Backslash", "\xE2\x88\x96"},
    {"Barv", "\xE2\xAB\xA7"},
    {"Barwed", "\xE2\x8C\x86"},
    {"Bcy", "\xD0\x91"},
    {"Because", "\xE2\x88\xB5"},
    {"Bernoullis", "\xE2\x84\xAC"},
    {"Beta", "\xCE\x92"},
    {"Bfr", "\xF0\x9D\x94\x85"},
    {"Bopf", "\xF0\x9D\x94\xB9"},
    {"Breve", "\xCB\x98"},
    {"Bscr", "\xE2\x84\xAC"},
    {"Bumpeq", "\xE2\x89\x8E"},
    {"CHcy", "\xD0\xA7"},
    {"COPY", "\xC2\xA9"},
    {"Cacute", "\xC4\x86"},
    {"Cap", "\xE2\x8B\x92"},
    {"CapitalDifferentialD", "\xE2\x85\x85"},
    {"Cayleys", "\xE2\x84\xAD"},
    {"Ccaron", "\xC4\x8C"},
    {"Ccedil", "\xC3\x87"},
    {"Ccirc", "\xC4\x88"},
    {"Cconint", "\xE2\x88\xB0"},
    {"Cdot", "\xC4\x8A"},
    {"Cedilla", "\xC2\xB8"},
    {"CenterDot", "\xC2\xB7"},
    {"Cfr", "\xE2\x84\xAD"},
    {"Chi", "\xCE\xA7"},
    {"CircleDot", "\xE2\x8A\x99"},
    {"CircleMinus", "\xE2\x8A\x96"},
    {"CirclePlus", "\xE2\x8A\x95"},
    {"CircleTimes", "\xE2\x8A\x97"},
    {"ClockwiseContourIntegral", "\xE2\x88\xB2"},
    {"CloseCurlyDoubleQuote", "\xE2\x80\x9D"},
    {"CloseCurlyQuote", "\xE2\x80\x99"},
    {"Colon", "\xE2\x88\xB7"},
    {"Colone", "\xE2\xA9\xB4"},
    {"Congruent", "\xE2\x89\xA1"},
    {"Conint", "\xE2\x88\xAF"},
    {"ContourIntegral", "\xE2\x88\xAE"},
    {"Copf", "\xE2\x84\x82"},
    {"Coproduct", "\xE2\x88\x90"},
    {"CounterClockwiseContourIntegral", "\xE2\x88\xB3"},
    {"Cross", "\xE2\xA8\xAF"},
    {"Cscr", "\xF0\x9D\x92\x9E"},
    {"Cup", "\xE2\x8B\x93"},
    {"CupCap", "\xE2\x89\x8D"},
    {"DD", "\xE2\x85\x85"},
    {"DDotrahd", "\xE2\xA4\x91"},
    {"DJcy", "\xD0\x82"},
    {"DScy", "\xD0\x85"},
    {"DZcy", "\xD0\x8F"},
    {"Dagger", "\xE2\x80\xA1"},
    {"Darr", "\xE2\x86\xA1"},
    {"Dashv", "\xE2\xAB\xA4"},
    {"Dcaron", "\xC4\x8E"},
    {"Dcy", "\xD0\x94"},
    {"Del", "\xE2\x88\x87"},
    {"Delta", "\xCE\x94"},
    {"Dfr", "\xF0\x9D\x94\x87"},
    {"DiacriticalAcute", "\xC2\xB4"},
    {"DiacriticalDot", "\xCB\x99"},
    {"DiacriticalDoubleAcute", "\xCB\x9D"},
    {"DiacriticalGrave", "\x60"},
    {"DiacriticalTilde", "\xCB\x9C"},
    {"Diamond", "\xE2\x8B\x84"},
    {"DifferentialD", "\xE2\x85\x86"},
    {"Dopf", "\xF0\x9D\x94\xBB"},
    {"Dot", "\xC2\xA8"},
    {"DotDot", "\xE2\x83\x9C"},
    {"DotEqual", "\xE2\x89\x90"},
    {"DoubleContourIntegral", "\xE2\x88\xAF"},
    {"DoubleDot", "\xC2\xA8"},
    {"DoubleDownArrow", "\xE2\x87\x93"},
    {"DoubleLeftArrow", "\xE2\x87\x90"},
    {"DoubleLeftRightArrow", "\xE2\x87\x94"},
    {"DoubleLeftTee", "\xE2\xAB\xA4"},
    {"DoubleLongLeftArrow", "\xE2\x9F\xB8"},
    {"DoubleLongLeftRightArrow", "\xE2\x9F\xBA"},
    {"DoubleLongRightArrow", "\xE2\x9F\xB9"},
    {"DoubleRightArrow", "\xE2\x87\x92"},
    {"DoubleRightTee", "\xE2\x8A\xA8"},
    {"DoubleUpArrow", "\xE2\x87\x91"},
    {"DoubleUpDownArrow", "\xE2\x87\x95"},
    {"DoubleVerticalBar", "\xE2\x88\xA5"},
    {"DownArrow", "\xE2\x86\x93"},
    {"DownArrowBar", "\xE2\xA4\x93"},
    {"DownArrowUpArrow", "\xE2\x87\xB5"},
    {"DownBreve", "\xCC\x91"},
    {"DownLeftRightVector", "\xE2\xA5\x90"},
    {"DownLeftTeeVector", "\xE2\xA5\x9E"},
    {"DownLeftVector", "\xE2\x86\xBD"},
    {"DownLeftVectorBar", "\xE2\xA5\x96"},
    {"DownRightTeeVector", "\xE2\xA5\x9F"},
    {"DownRightVector", "\xE2\x87\x81"},
    {"DownRightVectorBar", "\xE2\xA5\x97"},
    {"DownTee", "\xE2\x8A\xA4"},
    {"DownTeeArrow", "\xE2\x86\xA7"},
    {"Downarrow", "\xE2\x87\x93"},
    {"Dscr", "\xF0\x9D\x92\x9F"},
    {"Dstrok", "\xC4\x90"},
    {"ENG", "\xC5\x8A"},
    {"ETH", "\xC3\x90"},
    {"Eacute", "\xC3\x89"},
    {"Ecaron", "\xC4\x9A"},
    {"Ecirc", "\xC3\x8A"},
    {"Ecy", "\xD0\xAD"},
    {"Edot", "\xC4\x96"},
    {"Efr", "\xF0\x9D\x94\x88"},
    {"Egrave", "\xC3\x88"},
    {"Element", "\xE2\x88\x88"},
    {"Emacr", "\xC4\x92"},
    {"EmptySmallSquare", "\xE2\x97\xBB"},
    {"EmptyVerySmallSquare", "\xE2\x96\xAB"},
    {"Eogon", "\xC4\x98"},
    {"Eopf", "\xF0\x9D\x94\xBC"},
    {"Epsilon", "\xCE\x95"},
    {"Equal", "\xE2\xA9\xB5"},
    {"EqualTilde", "\xE2\x89\x82"},
    {"Equilibrium", "\xE2\x87\x8C"},
    {"Escr", "\xE2\x84\xB0"},
    {"Esim", "\xE2\xA9\xB3"},
    {"Eta", "\xCE\x97"},
    {"Euml", "\xC3\x8B"},
    {"Exists", "\xE2\x88\x83"},
    {"ExponentialE", "\xE2\x85\x87"},
    {"Fcy", "\xD0\xA4"},
    {"Ffr", "\xF0\x9D\x94\x89"},
    {"FilledSmallSquare", "\xE2\x97\xBC"},
    {"FilledVerySmallSquare", "\xE2\x96\xAA"},
    {"Fopf", "\xF0\x9D\x94\xBD"},
    {"ForAll", "\xE2\x88\x80"},
    {"Fouriertrf", "\xE2\x84\xB1"},
    {"Fscr", "\xE2\x84\xB1"},
    {"GJcy", "\xD0\x83"},
    {"GT", "\x3E"},
    {"Gamma", "\xCE\x93"},
    {"Gammad", "\xCF\x9C"},
    {"Gbreve", "\xC4\x9E"},
    {"Gcedil", "\xC4\xA2"},
    {"Gcirc", "\xC4\x9C"},
    {"Gcy", "\xD0\x93"},
    {"Gdot", "\xC4\xA0"},
    {"Gfr", "\xF0\x9D\x94\x8A"},
    {"Gg", "\xE2\x8B\x99"},
    {"Gopf", "\xF0\x9D\x94\xBE"},
    {"GreaterEqual", "\xE2\x89\xA5"},
    {"GreaterEqualLess", "\xE2\x8B\x9B"},
    {"GreaterFullEqual", "\xE2\x89\xA7"},
    {"GreaterGreater", "\xE2\xAA\xA2"},
    {"GreaterLess", "\xE2\x89\xB7"},
    {"GreaterSlantEqual", "\xE2\xA9\xBE"},
    {"GreaterTilde", "\xE2\x89\xB3"},
    {"Gscr", "\xF0\x9D\x92\xA2"},
    {"Gt", "\xE2\x89\xAB"},
    {"HARDcy", "\xD0\xAA"},
    {"Hacek", "\xCB\x87"},
    {"Hat", "\x5E"},
    {"Hcirc", "\xC4\xA4"},
    {"Hfr", "\xE2\x84\x8C"},
    {"HilbertSpace", "\xE2\x84\x8B"},
    {"Hopf", "\xE2\x84\x8D"},
    {"HorizontalLine", "\xE2\x94\x80"},
    {"Hscr", "\xE2\x84\x8B"},
    {"Hstrok", "\xC4\xA6"},
    {"HumpDownHump", "\xE2\x89\x8E"},
    {"HumpEqual", "\xE2\x89\x8F"},
    {"IEcy", "\xD0\x95"},
    {"IJlig", "\xC4\xB2"},
    {"IOcy", "\xD0\x81"},
    {"Iacute", "\xC3\x8D"},
    {"Icirc", "\xC3\x8E"},
    {"Icy", "\xD0\x98"},
    {"Idot", "\xC4\xB0"},
    {"Ifr", "\xE2\x84\x91"},
    {"Igrave", "\xC3\x8C"},
    {"Im", "\xE2\x84\x91"},
    {"Imacr", "\xC4\xAA"},
    {"ImaginaryI", "\xE2\x85\x88"},
    {"Implies", "\xE2\x87\x92"},
    {"Int", "\xE2\x88\xAC"},
    {"Integral", "\xE2\x88\xAB"},
    {"Intersection", "\xE2\x8B\x82"},
    {"InvisibleComma", "\xE2\x81\xA3"},
    {"InvisibleTimes", "\xE2\x81\xA2"},
    {"Iogon", "\xC4\xAE"},
    {"Iopf", "\xF0\x9D\x95\x80"},
    {"Iota", "\xCE\x99"},
    {"Iscr", "\xE2\x84\x90"},
    {"Itilde", "\xC4\xA8"},
    {"Iukcy", "\xD0\x86"},
    {"Iuml", "\xC3\x8F"},
    {"Jcirc", "\xC4\xB4"},
    {"Jcy", "\xD0\x99"},
    {"Jfr", "\xF0\x9D\x94\x8D"},
    {"Jopf", "\xF0\x9D\x95\x81"},
    {"Jscr", "\xF0\x9D\x92\xA5"},
    {"Jsercy", "\xD0\x88"},
    {"Jukcy", "\xD0\x84"},
    {"KHcy", "\xD0\xA5"},
    {"KJcy", "\xD0\x8C"},
    {"Kappa", "\xCE\x9A"},
    {"Kcedil", "\xC4\xB6"},
    {"Kcy", "\xD0\x9A"},
    {"Kfr", "\xF0\x9D\x94\x8E"},
    {"Kopf", "\xF0\x9D\x95\x82"},
    {"Kscr", "\xF0\x9D\x92\xA6"},
    {"LJcy", "\xD0\x89"},
    {"LT", "\x3C"},
    {"Lacute", "\xC4\xB9"},
    {"Lambda", "\xCE\x9B"},
    {"Lang", "\xE2\x9F\xAA"},
    {"Laplacetrf", "\xE2\x84\x92"},
    {"Larr", "\xE2\x86\x9E"},
    {"Lcaron", "\xC4\xBD"},
    {"Lcedil", "\xC4\xBB"},
    {"Lcy", "\xD0\x9B"},
    {"LeftAngleBracket", "\xE2\x9F\xA8"},
    {"LeftArrow", "\xE2\x86\x90"},
    {"LeftArrowBar", "\xE2\x87\xA4"},
    {"LeftArrowRightArrow", "\xE2\x87\x86"},
    {"LeftCeiling", "\xE2\x8C\x88"},
    {"LeftDoubleBracket", "\xE2\x9F\xA6"},
    {"LeftDownTeeVector", "\xE2\xA5\xA1"},
    {"LeftDownVector", "\xE2\x87\x83"},
    {"LeftDownVectorBar", "\xE2\xA5\x99"},
    {"LeftFloor", "\xE2\x8C\x8A"},
    {"LeftRightArrow", "\xE2\x86\x94"},
    {"LeftRightVector", "\xE2\xA5\x8E"},
    {"LeftTee", "\xE2\x8A\xA3"},
    {"LeftTeeArrow", "\xE2\x86\xA4"},
    {"LeftTeeVector", "\xE2\xA5\x9A"},
    {"LeftTriangle", "\xE2\x8A\xB2"},
    {"LeftTriangleBar", "\xE2\xA7\x8F"},
    {"LeftTriangleEqual", "\xE2\x8A\xB4"},
    {"LeftUpDownVector", "\xE2\xA5\x91"},
    {"LeftUpTeeVector", "\xE2\xA5\xA0"},
    {"LeftUpVector", "\xE2\x86\xBF"},
    {"LeftUpVectorBar", "\xE2\xA5\x98"},
    {"LeftVector", "\xE2\x86\xBC"},
    {"LeftVectorBar", "\xE2\xA5\x92"},
    {"Leftarrow", "\xE2\x87\x90"},
    {"Leftrightarrow", "\xE2\x87\x94"},
    {"LessEqualGreater", "\xE2\x8B\x9A"},
    {"LessFullEqual", "\xE2\x89\xA6"},
    {"LessGreater", "\xE2\x89\xB6"},
    {"LessLess", "\xE2\xAA\xA1"},
    {"LessSlantEqual", "\xE2\xA9\xBD"},
    {"LessTilde", "\xE2\x89\xB2"},
    {"Lfr", "\xF0\x9D\x94\x8F"},
    {"Ll", "\xE2\x8B\x98"},
    {"Lleftarrow", "\xE2\x87\x9A"},
    {"Lmidot", "\xC4\xBF"},
    {"LongLeftArrow", "\xE2\x9F\xB5"},
    {"LongLeftRightArrow", "\xE2\x9F\xB7"},
    {"LongRightArrow", "\xE2\x9F\xB6"},
    {"Longleftarrow", "\xE2\x9F\xB8"},
    {"Longleftrightarrow", "\xE2\x9F\xBA"},
    {"Longrightarrow", "\xE2\x9F\xB9"},
    {"Lopf", "\xF0\x9D\x95\x83"},
    {"LowerLeftArrow", "\xE2\x86\x99"},
    {"LowerRightArrow", "\xE2\x86\x98"},
    {"Lscr", "\xE2\x84\x92"},
    {"Lsh", "\xE2\x86\xB0"},
    {"Lstrok", "\xC5\x81"},
    {"Lt", "\xE2\x89\xAA"},
    {"Map", "\xE2\xA4\x85"},
    {"Mcy", "\xD0\x9C"},
    {"MediumSpace", "\xE2\x81\x9F"},
    {"Mellintrf", "\xE2\x84\xB3"},
    {"Mfr", "\xF0\x9D\x94\x90"},
    {"MinusPlus", "\xE2\x88\x93"},
    {"Mopf", "\xF0\x9D\x95\x84"},
    {"Mscr", "\xE2\x84\xB3"},
    {"Mu", "\xCE\x9C"},
    {"NJcy", "\xD0\x8A"},
    {"Nacute", "\xC5\x83"},
    {"Ncaron", "\xC5\x87"},
    {"Ncedil", "\xC5\x85"},
    {"Ncy", "\xD0\x9D"},
    {"NegativeMediumSpace", "\xE2\x80\x8B"},
    {"NegativeThickSpace", "\xE2\x80\x8B"},
    {"NegativeThinSpace", "\xE2\x80\x8B"},
    {"NegativeVeryThinSpace", "\xE2\x80\x8B"},
    {"NestedGreaterGreater", "\xE2\x89\xAB"},
    {"NestedLessLess", "\xE2\x89\xAA"},
    {"NewLine", "\x0A"},
    {"Nfr", "\xF0\x9D\x94\x91"},
    {"NoBreak", "\xE2\x81\xA0"},
    {"NonBreakingSpace", "\xC2\xA0"},
    {"Nopf", "\xE2\x84\x95"},
    {"Not", "\xE2\xAB\xAC"},
    {"NotCongruent", "\xE2\x89\xA2"},
    {"NotCupCap", "\xE2\x89\xAD"},
    {"NotDoubleVerticalBar", "\xE2\x88\xA6"},
    {"NotElement", "\xE2\x88\x89"},
    {"NotEqual", "\xE2\x89\xA0"},
    {"NotEqualTilde", "\xE2\x89\x82\xCC\xB8"},
    {"NotExists", "\xE2\x88\x84"},
    {"NotGreater", "\xE2\x89\xAF"},
    {"NotGreaterEqual", "\xE2\x89\xB1"},
    {"NotGreaterFullEqual", "\xE2\x89\xA7\xCC\xB8"},
    {"NotGreaterGreater", "\xE2\x89\xAB\xCC\xB8"},
    {"NotGreaterLess", "\xE2\x89\xB9"},
    {"NotGreaterSlantEqual", "\xE2\xA9\xBE\xCC\xB8"},
    {"NotGreaterTilde", "\xE2\x89\xB5"},
    {"NotHumpDownHump", "\xE2\x89\x8E\xCC\xB8"},
    {"NotHumpEqual", "\xE2\x89\x8F\xCC\xB8"},
    {"NotLeftTriangle", "\xE2\x8B\xAA"},
    {"NotLeftTriangleBar", "\xE2\xA7\x8F\xCC\xB8"},
    {"NotLeftTriangleEqual", "\xE2\x8B\xAC"},
    {"NotLess", "\xE2\x89\xAE"},
    {"NotLessEqual", "\xE2\x89\xB0"},
    {"NotLessGreater", "\xE2\x89\xB8"},
    {"NotLessLess", "\xE2\x89\xAA\xCC\xB8"},
    {"NotLessSlantEqual", "\xE2\xA9\xBD\xCC\xB8"},
    {"NotLessTilde", "\xE2\x89\xB4"},
    {"NotNestedGreaterGreater", "\xE2\xAA\xA2\xCC\xB8"},
    {"NotNestedLessLess", "\xE2\xAA\xA1\xCC\xB8"},
    {"NotPrecedes", "\xE2\x8A\x80"},
    {"NotPrecedesEqual", "\xE2\xAA\xAF\xCC\xB8"},
    {"NotPrecedesSlantEqual", "\xE2\x8B\xA0"},
    {"NotReverseElement", "\xE2\x88\x8C"},
    {"NotRightTriangle", "\xE2\x8B\xAB"},
    {"NotRightTriangleBar", "\xE2\xA7\x90\xCC\xB8"},
    {"NotRightTriangleEqual", "\xE2\x8B\xAD"},
    {"NotSquareSubset", "\xE2\x8A\x8F\xCC\xB8"},
    {"NotSquareSubsetEqual", "\xE2\x8B\xA2"},
    {"NotSquareSuperset", "\xE2\x8A\x90\xCC\xB8"},
    {"NotSquareSupersetEqual", "\xE2\x8B\xA3"},
    {"NotSubset", "\xE2\x8A\x82\xE2\x83\x92"},
    {"NotSubsetEqual", "\xE2\x8A\x88"},
    {"NotSucceeds", "\xE2\x8A\x81"},
    {"NotSucceedsEqual", "\xE2\xAA\xB0\xCC\xB8"},
    {"NotSucceedsSlantEqual", "\xE2\x8B\xA1"},
    {"NotSucceedsTilde", "\xE2\x89\xBF\xCC\xB8"},
    {"NotSuperset", "\xE2\x8A\x83\xE2\x83\x92"},
    {"NotSupersetEqual", "\xE2\x8A\x89"},
    {"NotTilde", "\xE2\x89\x81"},
    {"NotTildeEqual", "\xE2\x89\x84"},
    {"NotTildeFullEqual", "\xE2\x89\x87"},
    {"NotTildeTilde", "\xE2\x89\x89"},
    {"NotVerticalBar", "\xE2\x88\xA4"},
    {"Nscr", "\xF0\x9D\x92\xA9"},
    {"Ntilde", "\xC3\x91"},
    {"Nu", "\xCE\x9D"},
    {"OElig", "\xC5\x92"},
    {"Oacute", "\xC3\x93"},
    {"Ocirc", "\xC3\x94"},
    {"Ocy", "\xD0\x9E"},
    {"Odblac", "\xC5\x90"},
    {"Ofr", "\xF0\x9D\x94\x92"},
    {"Ograve", "\xC3\x92"},
    {"Omacr", "\xC5\x8C"},
    {"Omega", "\xCE\xA9"},
    {"Omicron", "\xCE\x9F"},
    {"Oopf", "\xF0\x9D\x95\x86"},
    {"OpenCurlyDoubleQuote", "\xE2\x80\x9C"},
    {"OpenCurlyQuote", "\xE2\x80\x98"},
    {"Or", "\xE2\xA9\x94"},
    {"Oscr", "\xF0\x9D\x92\xAA"},
    {"Oslash", "\xC3\x98"},
    {"Otilde", "\xC3\x95"},
    {"Otimes", "\xE2\xA8\xB7"},
    {"Ouml", "\xC3\x96"},
    {"OverBar", "\xE2\x80\xBE"},
    {"OverBrace", "\xE2\x8F\x9E"},
    {"OverBracket", "\xE2\x8E\xB4"},
    {"OverParenthesis", "\xE2\x8F\x9C"},
    {"PartialD", "\xE2\x88\x82"},
    {"Pcy", "\xD0\x9F"},
    {"Pfr", "\xF0\x9D\x94\x93"},
    {"Phi", "\xCE\xA6"},
    {"Pi", "\xCE\xA0"},
    {"PlusMinus", "\xC2\xB1"},
    {"Poincareplane", "\xE2\x84\x8C"},
    {"Popf", "\xE2\x84\x99"},
    {"Pr", "\xE2\xAA\xBB"},
    {"Precedes", "\xE2\x89\xBA"},
    {"PrecedesEqual", "\xE2\xAA\xAF"},
    {"PrecedesSlantEqual", "\xE2\x89\xBC"},
    {"PrecedesTilde", "\xE2\x89\xBE"},
    {"Prime", "\xE2\x80\xB3"},
    {"Product", "\xE2\x88\x8F"},
    {"Proportion", "\xE2\x88\xB7"},
    {"Proportional", "\xE2\x88\x9D"},
    {"Pscr", "\xF0\x9D\x92\xAB"},
    {"Psi", "\xCE\xA8"},
    {"QUOT", "\x22"},
    {"Qfr", "\xF0\x9D\x94\x94"},
    {"Qopf", "\xE2\x84\x9A"},
    {"Qscr", "\xF0\x9D\x92\xAC"},
    {"RBarr", "\xE2\xA4\x90"},
    {"REG", "\xC2\xAE"},
    {"Racute", "\xC5\x94"},
    {"Rang", "\xE2\x9F\xAB"},
    {"Rarr", "\xE2\x86\xA0"},
    {"Rarrtl", "\xE2\xA4\x96"},
    {"Rcaron", "\xC5\x98"},
    {"Rcedil", "\xC5\x96"},
    {"Rcy", "\xD0\xA0"},
    {"Re", "\xE2\x84\x9C"},
    {"ReverseElement", "\xE2\x88\x8B"},
    {"ReverseEquilibrium", "\xE2\x87\x8B"},
    {"ReverseUpEquilibrium", "\xE2\xA5\xAF"},
    {"Rfr", "\xE2\x84\x9C"},
    {"Rho", "\xCE\xA1"},
    {"RightAngleBracket", "\xE2\x9F\xA9"},
    {"RightArrow", "\xE2\x86\x92"},
    {"RightArrowBar", "\xE2\x87\xA5"},
    {"RightArrowLeftArrow", "\xE2\x87\x84"},
    {"RightCeiling", "\xE2\x8C\x89"},
    {"RightDoubleBracket", "\xE2\x9F\xA7"},
    {"RightDownTeeVector", "\xE2\xA5\x9D"},
    {"RightDownVector", "\xE2\x87\x82"},
    {"RightDownVectorBar", "\xE2\xA5\x95"},
    {"RightFloor", "\xE2\x8C\x8B"},
    {"RightTee", "\xE2\x8A\xA2"},
    {"RightTeeArrow", "\xE2\x86\xA6"},
    {"RightTeeVector", "\xE2\xA5\x9B"},
    {"RightTriangle", "\xE2\x8A\xB3"},
    {"RightTriangleBar", "\xE2\xA7\x90"},
    {"RightTriangleEqual", "\xE2\x8A\xB5"},
    {"RightUpDownVector", "\xE2\xA5\x8F"},
    {"RightUpTeeVector", "\xE2\xA5\x9C"},
    {"RightUpVector", "\xE2\x86\xBE"},
    {"RightUpVectorBar", "\xE2\xA5\x94"},
    {"RightVector", "\xE2\x87\x80"},
    {"RightVectorBar", "\xE2\xA5\x93"},
    {"Rightarrow", "\xE2\x87\x92"},
    {"Ropf", "\xE2\x84\x9D"},
    {"RoundImplies", "\xE2\xA5\xB0"},
    {"Rrightarrow", "\xE2\x87\x9B"},
    {"Rscr", "\xE2\x84\x9B"},
    {"Rsh", "\xE2\x86\xB1"},
    {"RuleDelayed", "\xE2\xA7\xB4"},
    {"SHCHcy", "\xD0\xA9"},
    {"SHcy", "\xD0\xA8"},
    {"SOFTcy", "\xD0\xAC"},
    {"Sacute", "\xC5\x9A"},
    {"Sc", "\xE2\xAA\xBC"},
    {"Scaron", "\xC5\xA0"},
    {"Scedil", "\xC5\x9E"},
    {"Scirc", "\xC5\x9C"},
    {"Scy", "\xD0\xA1"},
    {"Sfr", "\xF0\x9D\x94\x96"},
    {"ShortDownArrow", "\xE2\x86\x93"},
    {"ShortLeftArrow", "\xE2\x86\x90"},
    {"ShortRightArrow", "\xE2\x86\x92"},
    {"ShortUpArrow", "\xE2\x86\x91"},
    {"Sigma", "\xCE\xA3"},
    {"SmallCircle", "\xE2\x88\x98"},
    {"Sopf", "\xF0\x9D\x95\x8A"},
    {"Sqrt", "\xE2\x88\x9A"},
    {"Square", "\xE2\x96\xA1"},
    {"SquareIntersection", "\xE2\x8A\x93"},
    {"SquareSubset", "\xE2\x8A\x8F"},
    {"SquareSubsetEqual", "\xE2\x8A\x91"},
    {"SquareSuperset", "\xE2\x8A\x90"},
    {"SquareSupersetEqual", "\xE2\x8A\x92"},
    {"SquareUnion", "\xE2\x8A\x94"},
    {"Sscr", "\xF0\x9D\x92\xAE"},
    {"Star", "\xE2\x8B\x86"},
    {"Sub", "\xE2\x8B\x90"},
    {"Subset", "\xE2\x8B\x90"},
    {"SubsetEqual", "\xE2\x8A\x86"},
    {"Succeeds", "\xE2\x89\xBB"},
    {"SucceedsEqual", "\xE2\xAA\xB0"},
    {"SucceedsSlantEqual", "\xE2\x89\xBD"},
    {"SucceedsTilde", "\xE2\x89\xBF"},
    {"SuchThat", "\xE2\x88\x8B"},
    {"Sum", "\xE2\x88\x91"},
    {"Sup", "\xE2\x8B\x91"},
    {"Superset", "\xE2\x8A\x83"},
    {"SupersetEqual", "\xE2\x8A\x87"},
    {"Supset", "\xE2\x8B\x91"},
    {"THORN", "\xC3\x9E"},
    {"TRADE", "\xE2\x84\xA2"},
    {"TSHcy", "\xD0\x8B"},
    {"TScy", "\xD0\xA6"},
    {"Tab", "\x09"},
    {"Tau", "\xCE\xA4"},
    {"Tcaron", "\xC5\xA4"},
    {"Tcedil", "\xC5\xA2"},
    {"Tcy", "\xD0\xA2"},
    {"Tfr", "\xF0\x9D\x94\x97"},
    {"Therefore", "\xE2\x88\xB4"},
    {"Theta", "\xCE\x98"},
    {"ThickSpace", "\xE2\x81\x9F\xE2\x80\x8A"},
    {"ThinSpace", "\xE2\x80\x89"},
    {"Tilde", "\xE2\x88\xBC"},
    {"TildeEqual", "\xE2\x89\x83"},
    {"TildeFullEqual", "\xE2\x89\x85"},
    {"TildeTilde", "\xE2\x89\x88"},
    {"Topf", "\xF0\x9D\x95\x8B"},
    {"TripleDot", "\xE2\x83\x9B"},
    {"Tscr", "\xF0\x9D\x92\xAF"},
    {"Tstrok", "\xC5\xA6"},
    {"Uacute", "\xC3\x9A"},
    {"Uarr", "\xE2\x86\x9F"},
    {"Uarrocir", "\xE2\xA5\x89"},
    {"Ubrcy", "\xD0\x8E"},
    {"Ubreve", "\xC5\xAC"},
    {"Ucirc", "\xC3\x9B"},
    {"Ucy", "\xD0\xA3"},
    {"Udblac", "\xC5\xB0"},
    {"Ufr", "\xF0\x9D\x94\x98"},
    {"Ugrave", "\xC3\x99"},
    {"Umacr", "\xC5\xAA"},
    {"UnderBar", "\x5F"},
    {"UnderBrace", "\xE2\x8F\x9F"},
    {"UnderBracket", "\xE2\x8E\xB5"},
    {"UnderParenthesis", "\xE2\x8F\x9D"},
    {"Union", "\xE2\x8B\x83"},
    {"UnionPlus", "\xE2\x8A\x8E"},
    {"Uogon", "\xC5\xB2"},
    {"Uopf", "\xF0\x9D\x95\x8C"},
    {"UpArrow", "\xE2\x86\x91"},
    {"UpArrowBar", "\xE2\xA4\x92"},
    {"UpArrowDownArrow", "\xE2\x87\x85"},
    {"UpDownArrow", "\xE2\x86\x95"},
    {"UpEquilibrium", "\xE2\xA5\xAE"},
    {"UpTee", "\xE2\x8A\xA5"},
    {"UpTeeArrow", "\xE2\x86\xA5"},
    {"Uparrow", "\xE2\x87\x91"},
    {"Updownarrow", "\xE2\x87\x95"},
    {"UpperLeftArrow", "\xE2\x86\x96"},
    {"UpperRightArrow", "\xE2\x86\x97"},
    {"Upsi", "\xCF\x92"},
    {"Upsilon", "\xCE\xA5"},
    {"Uring", "\xC5\xAE"},
    {"Uscr", "\xF0\x9D\x92\xB0"},
    {"Utilde", "\xC5\xA8"},
    {"Uuml", "\xC3\x9C"},
    {"VDash", "\xE2\x8A\xAB"},
    {"Vbar", "\xE2\xAB\xAB"},
    {"Vcy", "\xD0\x92"},
    {"Vdash", "\xE2\x8A\xA9"},
    {"Vdashl", "\xE2\xAB\xA6"},
    {"Vee", "\xE2\x8B\x81"},
    {"Verbar", "\xE2\x80\x96"},
    {"Vert", "\xE2\x80\x96"},
    {"VerticalBar", "\xE2\x88\xA3"},
    {"VerticalLine", "\x7C"},
    {"VerticalSeparator", "\xE2\x9D\x98"},
    {"VerticalTilde", "\xE2\x89\x80"},
    {"VeryThinSpace", "\xE2\x80\x8A"},
    {"Vfr", "\xF0\x9D\x94\x99"},
    {"Vopf", "\xF0\x9D\x95\x8D"},
    {"Vscr", "\xF0\x9D\x92\xB1"},
    {"Vvdash", "\xE2\x8A\xAA"},
    {"Wcirc", "\xC5\xB4"},
    {"Wedge", "\xE2\x8B\x80"},
    {"Wfr", "\xF0\x9D\x94\x9A"},
    {"Wopf", "\xF0\x9D\x95\x8E"},
    {"Wscr", "\xF0\x9D\x92\xB2"},
    {"Xfr", "\xF0\x9D\x94\x9B"},
    {"Xi", "\xCE\x9E"},
    {"Xopf", "\xF0\x9D\x95\x8F"},
    {"Xscr", "\xF0\x9D\x92\xB3"},
    {"YAcy", "\xD0\xAF"},
    {"YIcy", "\xD0\x87"},
    {"YUcy", "\xD0\xAE"},
    {"Yacute", "\xC3\x9D"},
    {"Ycirc", "\xC5\xB6"},
    {"Ycy", "\xD0\xAB"},
    {"Yfr", "\xF0\x9D\x94\x9C"},
    {"Yopf", "\xF0\x9D\x95\x90"},
    {"Yscr", "\xF0\x9D\x92\xB4"},
    {"Yuml", "\xC5\xB8"},
    {"ZHcy", "\xD0\x96"},
    {"Zacute", "\xC5\xB9"},
    {"Zcaron", "\xC5\xBD"},
    {"Zcy", "\xD0\x97"},
    {"Zdot", "\xC5\xBB"},
    {"ZeroWidthSpace", "\xE2\x80\x8B"},
    {"Zeta", "\xCE\x96"},
    {"Zfr", "\xE2\x84\xA8"},
    {"Zopf", "\xE2\x84\xA4"},
    {"Zscr", "\xF0\x9D\x92\xB5"},
    {"aacute", "\xC3\xA1"},
    {"abreve", "\xC4\x83"},
    {"ac", "\xE2\x88\xBE"},
    {"acE", "\xE2\x88\xBE\xCC\xB3"},
    {"acd", "\xE2\x88\xBF"},
    {"acirc", "\xC3\xA2"},
    {"acute", "\xC2\xB4"},
    {"acy", "\xD0\xB0"},
    {"aelig", "\xC3\xA6"},
    {"af", "\xE2\x81\xA1"},
    {"afr", "\xF0\x9D\x94\x9E"},
    {"agrave", "\xC3\xA0"},
    {"alefsym", "\xE2\x84\xB5"},
    {"aleph", "\xE2\x84\xB5"},
    {"alpha", "\xCE\xB1"},
    {"amacr", "\xC4\x81"},
    {"amalg", "\xE2\xA8\xBF"},
    {"amp", "\x26"},
    {"and", "\xE2\x88\xA7"},
    {"andand", "\xE2\xA9\x95"},
    {"andd", "\xE2\xA9\x9C"},
    {"andslope", "\xE2\xA9\x98"},
    {"andv", "\xE2\xA9\x9A"},
    {"ang", "\xE2\x88\xA0"},
    {"ange", "\xE2\xA6\xA4"},
    {"angle", "\xE2\x88\xA0"},
    {"angmsd", "\xE2\x88\xA1"},
    {"angmsdaa", "\xE2\xA6\xA8"},
    {"angmsdab", "\xE2\xA6\xA9"},
    {"angmsdac", "\xE2\xA6\xAA"},
    {"angmsdad", "\xE2\xA6\xAB"},
    {"angmsdae", "\xE2\xA6\xAC"},
    {"angmsdaf", "\xE2\xA6\xAD"},
    {"angmsdag", "\xE2\xA6\xAE"},
    {"angmsdah", "\xE2\xA6\xAF"},
    {"angrt", "\xE2\x88\x9F"},
    {"angrtvb", "\xE2\x8A\xBE"},
    {"angrtvbd", "\xE2\xA6\x9D"},
    {"angsph", "\xE2\x88\xA2"},
    {"angst", "\xC3\x85"},
    {"angzarr", "\xE2\x8D\xBC"},
    {"aogon", "\xC4\x85"},
    {"aopf", "\xF0\x9D\x95\x92"},
    {"ap", "\xE2\x89\x88"},
    {"apE", "\xE2\xA9\xB0"},
    {"apacir", "\xE2\xA9\xAF"},
    {"ape", "\xE2\x89\x8A"},
    {"apid", "\xE2\x89\x8B"},
    {"apos", "\x27"},
    {"approx", "\xE2\x89\x88"},
    {"approxeq", "\xE2\x89\x8A"},
    {"aring", "\xC3\xA5"},
    {"ascr", "\xF0\x9D\x92\xB6"},
    {"ast", "\x2A"},
    {"asymp", "\xE2\x89\x88"},
    {"asympeq", "\xE2\x89\x8D"},
    {"atilde", "\xC3\xA3"},
    {"auml", "\xC3\xA4"},
    {"awconint", "\xE2\x88\xB3"},
    {"awint", "\xE2\xA8\x91"},
    {"bNot", "\xE2\xAB\xAD"},
    {"backcong", "\xE2\x89\x8C"},
    {"backepsilon", "\xCF\xB6"},
    {"backprime", "\xE2\x80\xB5"},
    {"backsim", "\xE2\x88\xBD"},
    {"backsimeq", "\xE2\x8B\x8D"},
    {"barvee", "\xE2\x8A\xBD"},
    {"barwed", "\xE2\x8C\x85"},
    {"barwedge", "\xE2\x8C\x85"},
    {"bbrk", "\xE2\x8E\xB5"},
    {"bbrktbrk", "\xE2\x8E\xB6"},
    {"bcong", "\xE2\x89\x8C"},
    {"bcy", "\xD0\xB1"},
    {"bdquo", "\xE2\x80\x9E"},
    {"becaus", "\xE2\x88\xB5"},
    {"because", "\xE2\x88\xB5"},
    {"bemptyv", "\xE2\xA6\xB0"},
    {"bepsi", "\xCF\xB6"},
    {"bernou", "\xE2\x84\xAC"},
    {"beta", "\xCE\xB2"},
    {"beth", "\xE2\x84\xB6"},
    {"between", "\xE2\x89\xAC"},
    {"bfr", "\xF0\x9D\x94\x9F"},
    {"bigcap", "\xE2\x8B\x82"},
    {"bigcirc", "\xE2\x97\xAF"},
    {"bigcup", "\xE2\x8B\x83"},
    {"bigodot", "\xE2\xA8\x80"},
    {"bigoplus", "\xE2\xA8\x81"},
    {"bigotimes", "\xE2\xA8\x82"},
    {"bigsqcup", "\xE2\xA8\x86"},
    {"bigstar", "\xE2\x98\x85"},
    {"bigtriangledown", "\xE2\x96\xBD"},
    {"bigtriangleup", "\xE2\x96\xB3"},
    {"biguplus", "\xE2\xA8\x84"},
    {"bigvee", "\xE2\x8B\x81"},
    {"bigwedge", "\xE2\x8B\x80"},
    {"bkarow", "\xE2\xA4\x8D"},
    {"blacklozenge", "\xE2\xA7\xAB"},
    {"blacksquare", "\xE2\x96\xAA"},
    {"blacktriangle", "\xE2\x96\xB4"},
    {"blacktriangledown", "\xE2\x96\xBE"},
    {"blacktriangleleft", "\xE2\x97\x82"},
    {"blacktriangleright", "\xE2\x96\xB8"},
    {"blank", "\xE2\x90\xA3"},
    {"blk12", "\xE2\x96\x92"},
    {"blk14", "\xE2\x96\x91"},
    {"blk34", "\xE2\x96\x93"},
    {"block", "\xE2\x96\x88"},
    {"bne", "\x3D\xE2\x83\xA5"},
    {"bnequiv", "\xE2\x89\xA1\xE2\x83\xA5"},
    {"bnot", "\xE2\x8C\x90"},
    {"bopf", "\xF0\x9D\x95\x93"},
    {"bot", "\xE2\x8A\xA5"},
    {"bottom", "\xE2\x8A\xA5"},
    {"bowtie", "\xE2\x8B\x88"},
    {"boxDL", "\xE2\x95\x97"},
    {"boxDR", "\xE2\x95\x94"},
    {"boxDl", "\xE2\x95\x96"},
    {"boxDr", "\xE2\x95\x93"},
    {"boxH", "\xE2\x95\x90"},
    {"boxHD", "\xE2\x95\xA6"},
    {"boxHU", "\xE2\x95\xA9"},
    {"boxHd", "\xE2\x95\xA4"},
    {"boxHu", "\xE2\x95\xA7"},
    {"boxUL", "\xE2\x95\x9D"},
    {"boxUR", "\xE2\x95\x9A"},
    {"boxUl", "\xE2\x95\x9C"},
    {"boxUr", "\xE2\x95\x99"},
    {"boxV", "\xE2\x95\x91"},
    {"boxVH", "\xE2\x95\xAC"},
    {"boxVL", "\xE2\x95\xA3"},
    {"boxVR", "\xE2\x95\xA0"},
    {"boxVh", "\xE2\x95\xAB"},
    {"boxVl", "\xE2\x95\xA2"},
    {"boxVr", "\xE2\x95\x9F"},
    {"boxbox", "\xE2\xA7\x89"},
    {"boxdL", "\xE2\x95\x95"},
    {"boxdR", "\xE2\x95\x92"},
    {"boxdl", "\xE2\x94\x90"},
    {"boxdr", "\xE2\x94\x8C"},
    {"boxh", "\xE2\x94\x80"},
    {"boxhD", "\xE2\x95\xA5"},
    {"boxhU", "\xE2\x95\xA8"},
    {"boxhd", "\xE2\x94\xAC"},
    {"boxhu", "\xE2\x94\xB4"},
    {"boxminus", "\xE2\x8A\x9F"},
    {"boxplus", "\xE2\x8A\x9E"},
    {"boxtimes", "\xE2\x8A\xA0"},
    {"boxuL", "\xE2\x95\x9B"},
    {"boxuR", "\xE2\x95\x98"},
    {"boxul", "\xE2\x94\x98"},
    {"boxur", "\xE2\x94\x94"},
    {"boxv", "\xE2\x94\x82"},
    {"boxvH", "\xE2\x95\xAA"},
    {"boxvL", "\xE2\x95\xA1"},
    {"boxvR", "\xE2\x95\x9E"},
    {"boxvh", "\xE2\x94\xBC"},
    {"boxvl", "\xE2\x94\xA4"},
    {"boxvr", "\xE2\x94\x9C"},
    {"bprime", "\xE2\x80\xB5"},
    {"breve", "\xCB\x98"},
    {"brvbar", "\xC2\xA6"},
    {"bscr", "\xF0\x9D\x92\xB7"},
    {"bsemi", "\xE2\x81\x8F"},
    {"bsim", "\xE2\x88\xBD"},
    {"bsime", "\xE2\x8B\x8D"},
    {"bsol", "\x5C"},
    {"bsolb", "\xE2\xA7\x85"},
    {"bsolhsub", "\xE2\x9F\x88"},
    {"bull", "\xE2\x80\xA2"},
    {"bullet", "\xE2\x80\xA2"},
    {"bump", "\xE2\x89\x8E"},
    {"bumpE", "\xE2\xAA\xAE"},
    {"bumpe", "\xE2\x89\x8F"},
    {"bumpeq", "\xE2\x89\x8F"},
    {"cacute", "\xC4\x87"},
    {"cap", "\xE2\x88\xA9"},
    {"capand", "\xE2\xA9\x84"},
    {"capbrcup", "\xE2\xA9\x89"},
    {"capcap", "\xE2\xA9\x8B"},
    {"capcup", "\xE2\xA9\x87"},
    {"capdot", "\xE2\xA9\x80"},
    {"caps", "\xE2\x88\xA9\xEF\xB8\x80"},
    {"caret", "\xE2\x81\x81"},
    {"caron", "\xCB\x87"},
    {"ccaps", "\xE2\xA9\x8D"},
    {"ccaron", "\xC4\x8D"},
    {"ccedil", "\xC3\xA7"},
    {"ccirc", "\xC4\x89"},
    {"ccups", "\xE2\xA9\x8C"},
    {"ccupssm", "\xE2\xA9\x90"},
    {"cdot", "\xC4\x8B"},
    {"cedil", "\xC2\xB8"},
    {"cemptyv", "\xE2\xA6\xB2"},
    {"cent", "\xC2\xA2"},
    {"centerdot", "\xC2\xB7"},
    {"cfr", "\xF0\x9D\x94\xA0"},
    {"chcy", "\xD1\x87"},
    {"check", "\xE2\x9C\x93"},
    {"checkmark", "\xE2\x9C\x93"},
    {"chi", "\xCF\x87"},
    {"cir", "\xE2\x97\x8B"},
    {"cirE", "\xE2\xA7\x83"},
    {"circ", "\xCB\x86"},
    {"circeq", "\xE2\x89\x97"},
    {"circlearrowleft", "\xE2\x86\xBA"},
    {"circlearrowright", "\xE2\x86\xBB"},
    {"circledR", "\xC2\xAE"},
    {"circledS", "\xE2\x93\x88"},
    {"circledast", "\xE2\x8A\x9B"},
    {"circledcirc", "\xE2\x8A\x9A"},
    {"circleddash", "\xE2\x8A\x9D"},
    {"cire", "\xE2\x89\x97"},
    {"cirfnint", "\xE2\xA8\x90"},
    {"cirmid", "\xE2\xAB\xAF"},
    {"cirscir", "\xE2\xA7\x82"},
    {"clubs", "\xE2\x99\xA3"},
    {"clubsuit", "\xE2\x99\xA3"},
    {"colon", "\x3A"},
    {"colone", "\xE2\x89\x94"},
    {"coloneq", "\xE2\x89\x94"},
    {"comma", "\x2C"},
    {"commat", "\x40"},
    {"comp", "\xE2\x88\x81"},
    {"compfn", "\xE2\x88\x98"},
    {"complement", "\xE2\x88\x81"},
    {"complexes", "\xE2\x84\x82"},
    {"cong", "\xE2\x89\x85"},
    {"congdot", "\xE2\xA9\xAD"},
    {"conint", "\xE2\x88\xAE"},
    {"copf", "\xF0\x9D\x95\x94"},
    {"coprod", "\xE2\x88\x90"},
    {"copy", "\xC2\xA9"},
    {"copysr", "\xE2\x84\x97"},
    {"crarr", "\xE2\x86\xB5"},
    {"cross", "\xE2\x9C\x97"},
    {"cscr", "\xF0\x9D\x92\xB8"},
    {"csub", "\xE2\xAB\x8F"},
    {"csube", "\xE2\xAB\x91"},
    {"csup", "\xE2\xAB\x90"},
    {"csupe", "\xE2\xAB\x92"},
    {"ctdot", "\xE2\x8B\xAF"},
    {"cudarrl", "\xE2\xA4\xB8"},
    {"cudarrr", "\xE2\xA4\xB5"},
    {"cuepr", "\xE2\x8B\x9E"},
    {"cuesc", "\xE2\x8B\x9F"},
    {"cularr", "\xE2\x86\xB6"},
    {"cularrp", "\xE2\xA4\xBD"},
    {"cup", "\xE2\x88\xAA"},
    {"cupbrcap", "\xE2\xA9\x88"},
    {"cupcap", "\xE2\xA9\x86"},
    {"cupcup", "\xE2\xA9\x8A"},
    {"cupdot", "\xE2\x8A\x8D"},
    {"cupor", "\xE2\xA9\x85"},
    {"cups", "\xE2\x88\xAA\xEF\xB8\x80"},
    {"curarr", "\xE2\x86\xB7"},
    {"curarrm", "\xE2\xA4\xBC"},
    {"curlyeqprec", "\xE2\x8B\x9E"},
    {"curlyeqsucc", "\xE2\x8B\x9F"},
    {"curlyvee", "\xE2\x8B\x8E"},
    {"curlywedge", "\xE2\x8B\x8F"},
    {"curren", "\xC2\xA4"},
    {"curvearrowleft", "\xE2\x86\xB6"},
    {"curvearrowright", "\xE2\x86\xB7"},
    {"cuvee", "\xE2\x8B\x8E"},
    {"cuwed", "\xE2\x8B\x8F"},
    {"cwconint", "\xE2\x88\xB2"},
    {"cwint", "\xE2\x88\xB1"},
    {"cylcty", "\xE2\x8C\xAD"},
    {"dArr", "\xE2\x87\x93"},
    {"dHar", "\xE2\xA5\xA5"},
    {"dagger", "\xE2\x80\xA0"},
    {"daleth", "\xE2\x84\xB8"},
    {"darr", "\xE2\x86\x93"},
    {"dash", "\xE2\x80\x90"},
    {"dashv", "\xE2\x8A\xA3"},
    {"dbkarow", "\xE2\xA4\x8F"},
    {"dblac", "\xCB\x9D"},
    {"dcaron", "\xC4\x8F"},
    {"dcy", "\xD0\xB4"},
    {"dd", "\xE2\x85\x86"},
    {"ddagger", "\xE2\x80\xA1"},
    {"ddarr", "\xE2\x87\x8A"},
    {"ddotseq", "\xE2\xA9\xB7"},
    {"deg", "\xC2\xB0"},
    {"delta", "\xCE\xB4"},
    {"demptyv", "\xE2\xA6\xB1"},
    {"dfisht", "\xE2\xA5\xBF"},
    {"dfr", "\xF0\x9D\x94\xA1"},
    {"dharl", "\xE2\x87\x83"},
    {"dharr", "\xE2\x87\x82"},
    {"diam", "\xE2\x8B\x84"},
    {"diamond", "\xE2\x8B\x84"},
    {"diamondsuit", "\xE2\x99\xA6"},
    {"diams", "\xE2\x99\xA6"},
    {"die", "\xC2\xA8"},
    {"digamma", "\xCF\x9D"},
    {"disin", "\xE2\x8B\xB2"},
    {"div", "\xC3\xB7"},
    {"divide", "\xC3\xB7"},
    {"divideontimes", "\xE2\x8B\x87"},
    {"divonx", "\xE2\x8B\x87"},
    {"djcy", "\xD1\x92"},
    {"dlcorn", "\xE2\x8C\x9E"},
    {"dlcrop", "\xE2\x8C\x8D"},
    {"dollar", "\x24"},
    {"dopf", "\xF0\x9D\x95\x95"},
    {"dot", "\xCB\x99"},
    {"doteq", "\xE2\x89\x90"},
    {"doteqdot", "\xE2\x89\x91"},
    {"dotminus", "\xE2\x88\xB8"},
    {"dotplus", "\xE2\x88\x94"},
    {"dotsquare", "\xE2\x8A\xA1"},
    {"doublebarwedge", "\xE2\x8C\x86"},
    {"downarrow", "\xE2\x86\x93"},
    {"downdownarrows", "\xE2\x87\x8A"},
    {"downharpoonleft", "\xE2\x87\x83"},
    {"downharpoonright", "\xE2\x87\x82"},
    {"drbkarow", "\xE2\xA4\x90"},
    {"drcorn", "\xE2\x8C\x9F"},
    {"drcrop", "\xE2\x8C\x8C"},
    {"dscr", "\xF0\x9D\x92\xB9"},
    {"dscy", "\xD1\x95"},
    {"dsol", "\xE2\xA7\xB6"},
    {"dstrok", "\xC4\x91"},
    {"dtdot", "\xE2\x8B\xB1"},
    {"dtri", "\xE2\x96\xBF"},
    {"dtrif", "\xE2\x96\xBE"},
    {"duarr", "\xE2\x87\xB5"},
    {"duhar", "\xE2\xA5\xAF"},
    {"dwangle", "\xE2\xA6\xA6"},
    {"dzcy", "\xD1\x9F"},
    {"dzigrarr", "\xE2\x9F\xBF"},
    {"eDDot", "\xE2\xA9\xB7"},
    {"eDot", "\xE2\x89\x91"},
    {"eacute", "\xC3\xA9"},
    {"easter", "\xE2\xA9\xAE"},
    {"ecaron", "\xC4\x9B"},
    {"ecir", "\xE2\x89\x96"},
    {"ecirc", "\xC3\xAA"},
    {"ecolon", "\xE2\x89\x95"},
    {"ecy", "\xD1\x8D"},
    {"edot", "\xC4\x97"},
    {"ee", "\xE2\x85\x87"},
    {"efDot", "\xE2\x89\x92"},
    {"efr", "\xF0\x9D\x94\xA2"},
    {"eg", "\xE2\xAA\x9A"},
    {"egrave", "\xC3\xA8"},
    {"egs", "\xE2\xAA\x96"},
    {"egsdot", "\xE2\xAA\x98"},
    {"el", "\xE2\xAA\x99"},
    {"elinters", "\xE2\x8F\xA7"},
    {"ell", "\xE2\x84\x93"},
    {"els", "\xE2\xAA\x95"},
    {"elsdot", "\xE2\xAA\x97"},
    {"emacr", "\xC4\x93"},
    {"empty", "\xE2\x88\x85"},
    {"emptyset", "\xE2\x88\x85"},
    {"emptyv", "\xE2\x88\x85"},
    {"emsp", "\xE2\x80\x83"},
    {"emsp13", "\xE2\x80\x84"},
    {"emsp14", "\xE2\x80\x85"},
    {"eng", "\xC5\x8B"},
    {"ensp", "\xE2\x80\x82"},
    {"eogon", "\xC4\x99"},
    {"eopf", "\xF0\x9D\x95\x96"},
    {"epar", "\xE2\x8B\x95"},
    {"eparsl", "\xE2\xA7\xA3"},
    {"eplus", "\xE2\xA9\xB1"},
    {"epsi", "\xCE\xB5"},
    {"epsilon", "\xCE\xB5"},
    {"epsiv", "\xCF\xB5"},
    {"eqcirc", "\xE2\x89\x96"},
    {"eqcolon", "\xE2\x89\x95"},
    {"eqsim", "\xE2\x89\x82"},
    {"eqslantgtr", "\xE2\xAA\x96"},
    {"eqslantless", "\xE2\xAA\x95"},
    {"equals", "\x3D"},
    {"equest", "\xE2\x89\x9F"},
    {"equiv", "\xE2\x89\xA1"},
    {"equivDD", "\xE2\xA9\xB8"},
    {"eqvparsl", "\xE2\xA7\xA5"},
    {"erDot", "\xE2\x89\x93"},
    {"erarr", "\xE2\xA5\xB1"},
    {"escr", "\xE2\x84\xAF"},
    {"esdot", "\xE2\x89\x90"},
    {"esim", "\xE2\x89\x82"},
    {"eta", "\xCE\xB7"},
    {"eth", "\xC3\xB0"},
    {"euml", "\xC3\xAB"},
    {"euro", "\xE2\x82\xAC"},
    {"excl", "\x21"},
    {"exist", "\xE2\x88\x83"},
    {"expectation", "\xE2\x84\xB0"},
    {"exponentiale", "\xE2\x85\x87"},
    {"fallingdotseq", "\xE2\x89\x92"},
    {"fcy", "\xD1\x84"},
    {"female", "\xE2\x99\x80"},
    {"ffilig", "\xEF\xAC\x83"},
    {"fflig", "\xEF\xAC\x80"},
    {"ffllig", "\xEF\xAC\x84"},
    {"ffr", "\xF0\x9D\x94\xA3"},
    {"filig", "\xEF\xAC\x81"},
    {"fjlig", "\x66\x6A"},
    {"flat", "\xE2\x99\xAD"},
    {"fllig", "\xEF\xAC\x82"},
    {"fltns", "\xE2\x96\xB1"},
    {"fnof", "\xC6\x92"},
    {"fopf", "\xF0\x9D\x95\x97"},
    {"forall", "\xE2\x88\x80"},
    {"fork", "\xE2\x8B\x94"},
    {"forkv", "\xE2\xAB\x99"},
    {"fpartint", "\xE2\xA8\x8D"},
    {"frac12", "\xC2\xBD"},
    {"frac13", "\xE2\x85\x93"},
    {"frac14", "\xC2\xBC"},
    {"frac15", "\xE2\x85\x95"},
    {"frac16", "\xE2\x85\x99"},
    {"frac18", "\xE2\x85\x9B"},
    {"frac23", "\xE2\x85\x94"},
    {"frac25", "\xE2\x85\x96"},
    {"frac34", "\xC2\xBE"},
    {"frac35", "\xE2\x85\x97"},
    {"frac38", "\xE2\x85\x9C"},
    {"frac45", "\xE2\x85\x98"},
    {"frac56", "\xE2\x85\x9A"},
    {"frac58", "\xE2\x85\x9D"},
    {"frac78", "\xE2\x85\x9E"},
    {"frasl", "\xE2\x81\x84"},
    {"frown", "\xE2\x8C\xA2"},
    {"fscr", "\xF0\x9D\x92\xBB"},
    {"gE", "\xE2\x89\xA7"},
    {"gEl", "\xE2\xAA\x8C"},
    {"gacute", "\xC7\xB5"},
    {"gamma", "\xCE\xB3"},
    {"gammad", "\xCF\x9D"},
    {"gap", "\xE2\xAA\x86"},
    {"gbreve", "\xC4\x9F"},
    {"gcirc", "\xC4\x9D"},
    {"gcy", "\xD0\xB3"},
    {"gdot", "\xC4\xA1"},
    {"ge", "\xE2\x89\xA5"},
    {"gel", "\xE2\x8B\x9B"},
    {"geq", "\xE2\x89\xA5"},
    {"geqq", "\xE2\x89\xA7"},
    {"geqslant", "\xE2\xA9\xBE"},
    {"ges", "\xE2\xA9\xBE"},
    {"gescc", "\xE2\xAA\xA9"},
    {"gesdot", "\xE2\xAA\x80"},
    {"gesdoto", "\xE2\xAA\x82"},
    {"gesdotol", "\xE2\xAA\x84"},
    {"gesl", "\xE2\x8B\x9B\xEF\xB8\x80"},
    {"gesles", "\xE2\xAA\x94"},
    {"gfr", "\xF0\x9D\x94\xA4"},
    {"gg", "\xE2\x89\xAB"},
    {"ggg", "\xE2\x8B\x99"},
    {"gimel", "\xE2\x84\xB7"},
    {"gjcy", "\xD1\x93"},
    {"gl", "\xE2\x89\xB7"},
    {"glE", "\xE2\xAA\x92"},
    {"gla", "\xE2\xAA\xA5"},
    {"glj", "\xE2\xAA\xA4"},
    {"gnE", "\xE2\x89\xA9"},
    {"gnap", "\xE2\xAA\x8A"},
    {"gnapprox", "\xE2\xAA\x8A"},
    {"gne", "\xE2\xAA\x88"},
    {"gneq", "\xE2\xAA\x88"},
    {"gneqq", "\xE2\x89\xA9"},
    {"gnsim", "\xE2\x8B\xA7"},
    {"gopf", "\xF0\x9D\x95\x98"},
    {"grave", "\x60"},
    {"gscr", "\xE2\x84\x8A"},
    {"gsim", "\xE2\x89\xB3"},
    {"gsime", "\xE2\xAA\x8E"},
    {"gsiml", "\xE2\xAA\x90"},
    {"gt", "\x3E"},
    {"gtcc", "\xE2\xAA\xA7"},
    {"gtcir", "\xE2\xA9\xBA"},
    {"gtdot", "\xE2\x8B\x97"},
    {"gtlPar", "\xE2\xA6\x95"},
    {"gtquest", "\xE2\xA9\xBC"},
    {"gtrapprox", "\xE2\xAA\x86"},
    {"gtrarr", "\xE2\xA5\xB8"},
    {"gtrdot", "\xE2\x8B\x97"},
    {"gtreqless", "\xE2\x8B\x9B"},
    {"gtreqqless", "\xE2\xAA\x8C"},
    {"gtrless", "\xE2\x89\xB7"},
    {"gtrsim", "\xE2\x89\xB3"},
    {"gvertneqq", "\xE2\x89\xA9\xEF\xB8\x80"},
    {"gvnE", "\xE2\x89\xA9\xEF\xB8\x80"},
    {"hArr", "\xE2\x87\x94"},
    {"hairsp", "\xE2\x80\x8A"},
    {"half", "\xC2\xBD"},
    {"hamilt", "\xE2\x84\x8B"},
    {"hardcy", "\xD1\x8A"},
    {"harr", "\xE2\x86\x94"},
    {"harrcir", "\xE2\xA5\x88"},
    {"harrw", "\xE2\x86\xAD"},
    {"hbar", "\xE2\x84\x8F"},
    {"hcirc", "\xC4\xA5"},
    {"hearts", "\xE2\x99\xA5"},
    {"heartsuit", "\xE2\x99\xA5"},
    {"hellip", "\xE2\x80\xA6"},
    {"hercon", "\xE2\x8A\xB9"},
    {"hfr", "\xF0\x9D\x94\xA5"},
    {"hksearow", "\xE2\xA4\xA5"},
    {"hkswarow", "\xE2\xA4\xA6"},
    {"hoarr", "\xE2\x87\xBF"},
    {"homtht", "\xE2\x88\xBB"},
    {"hookleftarrow", "\xE2\x86\xA9"},
    {"hookrightarrow", "\xE2\x86\xAA"},
    {"hopf", "\xF0\x9D\x95\x99"},
    {"horbar", "\xE2\x80\x95"},
    {"hscr", "\xF0\x9D\x92\xBD"},
    {"hslash", "\xE2\x84\x8F"},
    {"hstrok", "\xC4\xA7"},
    {"hybull", "\xE2\x81\x83"},
    {"hyphen", "\xE2\x80\x90"},
    {"iacute", "\xC3\xAD"},
    {"ic", "\xE2\x81\xA3"},
    {"icirc", "\xC3\xAE"},
    {"icy", "\xD0\xB8"},
    {"iecy", "\xD0\xB5"},
    {"iexcl", "\xC2\xA1"},
    {"iff", "\xE2\x87\x94"},
    {"ifr", "\xF0\x9D\x94\xA6"},
    {"igrave", "\xC3\xAC"},
    {"ii", "\xE2\x85\x88"},
    {"iiiint", "\xE2\xA8\x8C"},
    {"iiint", "\xE2\x88\xAD"},
    {"iinfin", "\xE2\xA7\x9C"},
    {"iiota", "\xE2\x84\xA9"},
    {"ijlig", "\xC4\xB3"},
    {"imacr", "\xC4\xAB"},
    {"image", "\xE2\x84\x91"},
    {"imagline", "\xE2\x84\x90"},
    {"imagpart", "\xE2\x84\x91"},
    {"imath", "\xC4\xB1"},
    {"imof", "\xE2\x8A\xB7"},
    {"imped", "\xC6\xB5"},
    {"in", "\xE2\x88\x88"},
    {"incare", "\xE2\x84\x85"},
    {"infin", "\xE2\x88\x9E"},
    {"infintie", "\xE2\xA7\x9D"},
    {"inodot", "\xC4\xB1"},
    {"int", "\xE2\x88\xAB"},
    {"intcal", "\xE2\x8A\xBA"},
    {"integers", "\xE2\x84\xA4"},
    {"intercal", "\xE2\x8A\xBA"},
    {"intlarhk", "\xE2\xA8\x97"},
    {"intprod", "\xE2\xA8\xBC"},
    {"iocy", "\xD1\x91"},
    {"iogon", "\xC4\xAF"},
    {"iopf", "\xF0\x9D\x95\x9A"},
    {"iota", "\xCE\xB9"},
    {"iprod", "\xE2\xA8\xBC"},
    {"iquest", "\xC2\xBF"},
    {"iscr", "\xF0\x9D\x92\xBE"},
    {"isin", "\xE2\x88\x88"},
    {"isinE", "\xE2\x8B\xB9"},
    {"isindot", "\xE2\x8B\xB5"},
    {"isins", "\xE2\x8B\xB4"},
    {"isinsv", "\xE2\x8B\xB3"},
    {"isinv", "\xE2\x88\x88"},
    {"it", "\xE2\x81\xA2"},
    {"itilde", "\xC4\xA9"},
    {"iukcy", "\xD1\x96"},
    {"iuml", "\xC3\xAF"},
    {"jcirc", "\xC4\xB5"},
    {"jcy", "\xD0\xB9"},
    {"jfr", "\xF0\x9D\x94\xA7"},
    {"jmath", "\xC8\xB7"},
    {"jopf", "\xF0\x9D\x95\x9B"},
    {"jscr", "\xF0\x9D\x92\xBF"},
    {"jsercy", "\xD1\x98"},
    {"jukcy", "\xD1\x94"},
    {"kappa", "\xCE\xBA"},
    {"kappav", "\xCF\xB0"},
    {"kcedil", "\xC4\xB7"},
    {"kcy", "\xD0\xBA"},
    {"kfr", "\xF0\x9D\x94\xA8"},
    {"kgreen", "\xC4\xB8"},
    {"khcy", "\xD1\x85"},
    {"kjcy", "\xD1\x9C"},
    {"kopf", "\xF0\x9D\x95\x9C"},
    {"kscr", "\xF0\x9D\x93\x80"},
    {"lAarr", "\xE2\x87\x9A"},
    {"lArr", "\xE2\x87\x90"},
    {"lAtail", "\xE2\xA4\x9B"},
    {"lBarr", "\xE2\xA4\x8E"},
    {"lE", "\xE2\x89\xA6"},
    {"lEg", "\xE2\xAA\x8B"},
    {"lHar", "\xE2\xA5\xA2"},
    {"lacute", "\xC4\xBA"},
    {"laemptyv", "\xE2\xA6\xB4"},
    {"lagran", "\xE2\x84\x92"},
    {"lambda", "\xCE\xBB"},
    {"lang", "\xE2\x9F\xA8"},
    {"langd", "\xE2\xA6\x91"},
    {"langle", "\xE2\x9F\xA8"},
    {"lap", "\xE2\xAA\x85"},
    {"laquo", "\xC2\xAB"},
    {"larr", "\xE2\x86\x90"},
    {"larrb", "\xE2\x87\xA4"},
    {"larrbfs", "\xE2\xA4\x9F"},
    {"larrfs", "\xE2\xA4\x9D"},
    {"larrhk", "\xE2\x86\xA9"},
    {"larrlp", "\xE2\x86\xAB"},
    {"larrpl", "\xE2\xA4\xB9"},
    {"larrsim", "\xE2\xA5\xB3"},
    {"larrtl", "\xE2\x86\xA2"},
    {"lat", "\xE2\xAA\xAB"},
    {"latail", "\xE2\xA4\x99"},
    {"late", "\xE2\xAA\xAD"},
    {"lates", "\xE2\xAA\xAD\xEF\xB8\x80"},
    {"lbarr", "\xE2\xA4\x8C"},
    {"lbbrk", "\xE2\x9D\xB2"},
    {"lbrace", "\x7B"},
    {"lbrack", "\x5B"},
    {"lbrke", "\xE2\xA6\x8B"},
    {"lbrksld", "\xE2\xA6\x8F"},
    {"lbrkslu", "\xE2\xA6\x8D"},
    {"lcaron", "\xC4\xBE"},
    {"lcedil", "\xC4\xBC"},
    {"lceil", "\xE2\x8C\x88"},
    {"lcub", "\x7B"},
    {"lcy", "\xD0\xBB"},
    {"ldca", "\xE2\xA4\xB6"},
    {"ldquo", "\xE2\x80\x9C"},
    {"ldquor", "\xE2\x80\x9E"},
    {"ldrdhar", "\xE2\xA5\xA7"},
    {"ldrushar", "\xE2\xA5\x8B"},
    {"ldsh", "\xE2\x86\xB2"},
    {"le", "\xE2\x89\xA4"},
    {"leftarrow", "\xE2\x86\x90"},
    {"leftarrowtail", "\xE2\x86\xA2"},
    {"leftharpoondown", "\xE2\x86\xBD"},
    {"leftharpoonup", "\xE2\x86\xBC"},
    {"leftleftarrows", "\xE2\x87\x87"},
    {"leftrightarrow", "\xE2\x86\x94"},
    {"leftrightarrows", "\xE2\x87\x86"},
    {"leftrightharpoons", "\xE2\x87\x8B"},
    {"leftrightsquigarrow", "\xE2\x86\xAD"},
    {"leftthreetimes", "\xE2\x8B\x8B"},
    {"leg", "\xE2\x8B\x9A"},
    {"leq", "\xE2\x89\xA4"},
    {"leqq", "\xE2\x89\xA6"},
    {"leqslant", "\xE2\xA9\xBD"},
    {"les", "\xE2\xA9\xBD"},
    {"lescc", "\xE2\xAA\xA8"},
    {"lesdot", "\xE2\xA9\xBF"},
    {"lesdoto", "\xE2\xAA\x81"},
    {"lesdotor", "\xE2\xAA\x83"},
    {"lesg", "\xE2\x8B\x9A\xEF\xB8\x80"},
    {"lesges", "\xE2\xAA\x93"},
    {"lessapprox", "\xE2\xAA\x85"},
    {"lessdot", "\xE2\x8B\x96"},
    {"lesseqgtr", "\xE2\x8B\x9A"},
    {"lesseqqgtr", "\xE2\xAA\x8B"},
    {"lessgtr", "\xE2\x89\xB6"},
    {"lesssim", "\xE2\x89\xB2"},
    {"lfisht", "\xE2\xA5\xBC"},
    {"lfloor", "\xE2\x8C\x8A"},
    {"lfr", "\xF0\x9D\x94\xA9"},
    {"lg", "\xE2\x89\xB6"},
    {"lgE", "\xE2\xAA\x91"},
    {"lhard", "\xE2\x86\xBD"},
    {"lharu", "\xE2\x86\xBC"},
    {"lharul", "\xE2\xA5\xAA"},
    {"lhblk", "\xE2\x96\x84"},
    {"ljcy", "\xD1\x99"},
    {"ll", "\xE2\x89\xAA"},
    {"llarr", "\xE2\x87\x87"},
    {"llcorner", "\xE2\x8C\x9E"},
    {"llhard", "\xE2\xA5\xAB"},
    {"lltri", "\xE2\x97\xBA"},
    {"lmidot", "\xC5\x80"},
    {"lmoust", "\xE2\x8E\xB0"},
    {"lmoustache", "\xE2\x8E\xB0"},
    {"lnE", "\xE2\x89\xA8"},
    {"lnap", "\xE2\xAA\x89"},
    {"lnapprox", "\xE2\xAA\x89"},
    {"lne", "\xE2\xAA\x87"},
    {"lneq", "\xE2\xAA\x87"},
    {"lneqq", "\xE2\x89\xA8"},
    {"lnsim", "\xE2\x8B\xA6"},
    {"loang", "\xE2\x9F\xAC"},
    {"loarr", "\xE2\x87\xBD"},
    {"lobrk", "\xE2\x9F\xA6"},
    {"longleftarrow", "\xE2\x9F\xB5"},
    {"longleftrightarrow", "\xE2\x9F\xB7"},
    {"longmapsto", "\xE2\x9F\xBC"},
    {"longrightarrow", "\xE2\x9F\xB6"},
    {"looparrowleft", "\xE2\x86\xAB"},
    {"looparrowright", "\xE2\x86\xAC"},
    {"lopar", "\xE2\xA6\x85"},
    {"lopf", "\xF0\x9D\x95\x9D"},
    {"loplus", "\xE2\xA8\xAD"},
    {"lotimes", "\xE2\xA8\xB4"},
    {"lowast", "\xE2\x88\x97"},
    {"lowbar", "\x5F"},
    {"loz", "\xE2\x97\x8A"},
    {"lozenge", "\xE2\x97\x8A"},
    {"lozf", "\xE2\xA7\xAB"},
    {"lpar", "\x28"},
    {"lparlt", "\xE2\xA6\x93"},
    {"lrarr", "\xE2\x87\x86"},
    {"lrcorner", "\xE2\x8C\x9F"},
    {"lrhar", "\xE2\x87\x8B"},
    {"lrhard", "\xE2\xA5\xAD"},
    {"lrm", "\xE2\x80\x8E"},
    {"lrtri", "\xE2\x8A\xBF"},
    {"lsaquo", "\xE2\x80\xB9"},
    {"lscr", "\xF0\x9D\x93\x81"},
    {"lsh", "\xE2\x86\xB0"},
    {"lsim", "\xE2\x89\xB2"},
    {"lsime", "\xE2\xAA\x8D"},
    {"lsimg", "\xE2\xAA\x8F"},
    {"lsqb", "\x5B"},
    {"lsquo", "\xE2\x80\x98"},
    {"lsquor", "\xE2\x80\x9A"},
    {"lstrok", "\xC5\x82"},
    {"lt", "\x3C"},
    {"ltcc", "\xE2\xAA\xA6"},
    {"ltcir", "\xE2\xA9\xB9"},
    {"ltdot", "\xE2\x8B\x96"},
    {"lthree", "\xE2\x8B\x8B"},
    {"ltimes", "\xE2\x8B\x89"},
    {"ltlarr", "\xE2\xA5\xB6"},
    {"ltquest", "\xE2\xA9\xBB"},
    {"ltrPar", "\xE2\xA6\x96"},
    {"ltri", "\xE2\x97\x83"},
    {"ltrie", "\xE2\x8A\xB4"},
    {"ltrif", "\xE2\x97\x82"},
    {"lurdshar", "\xE2\xA5\x8A"},
    {"luruhar", "\xE2\xA5\xA6"},
    {"lvertneqq", "\xE2\x89\xA8\xEF\xB8\x80"},
    {"lvnE", "\xE2\x89\xA8\xEF\xB8\x80"},
    {"mDDot", "\xE2\x88\xBA"},
    {"macr", "\xC2\xAF"},
    {"male", "\xE2\x99\x82"},
    {"malt", "\xE2\x9C\xA0"},
    {"maltese", "\xE2\x9C\xA0"},
    {"map", "\xE2\x86\xA6"},
    {"mapsto", "\xE2\x86\xA6"},
    {"mapstodown", "\xE2\x86\xA7"},
    {"mapstoleft", "\xE2\x86\xA4"},
    {"mapstoup", "\xE2\x86\xA5"},
    {"marker", "\xE2\x96\xAE"},
    {"mcomma", "\xE2\xA8\xA9"},
    {"mcy", "\xD0\xBC"},
    {"mdash", "\xE2\x80\x94"},
    {"measuredangle", "\xE2\x88\xA1"},
    {"mfr", "\xF0\x9D\x94\xAA"},
    {"mho", "\xE2\x84\xA7"},
    {"micro", "\xC2\xB5"},
    {"mid", "\xE2\x88\xA3"},
    {"midast", "\x2A"},
    {"midcir", "\xE2\xAB\xB0"},
    {"middot", "\xC2\xB7"},
    {"minus", "\xE2\x88\x92"},
    {"minusb", "\xE2\x8A\x9F"},
    {"minusd", "\xE2\x88\xB8"},
    {"minusdu", "\xE2\xA8\xAA"},
    {"mlcp", "\xE2\xAB\x9B"},
    {"mldr", "\xE2\x80\xA6"},
    {"mnplus", "\xE2\x88\x93"},
    {"models", "\xE2\x8A\xA7"},
    {"mopf", "\xF0\x9D\x95\x9E"},
    {"mp", "\xE2\x88\x93"},
    {"mscr", "\xF0\x9D\x93\x82"},
    {"mstpos", "\xE2\x88\xBE"},
    {"mu", "\xCE\xBC"},
    {"multimap", "\xE2\x8A\xB8"},
    {"mumap", "\xE2\x8A\xB8"},
    {"nGg", "\xE2\x8B\x99\xCC\xB8"},
    {"nGt", "\xE2\x89\xAB\xE2\x83\x92"},
    {"nGtv", "\xE2\x89\xAB\xCC\xB8"},
    {"nLeftarrow", "\xE2\x87\x8D"},
    {"nLeftrightarrow", "\xE2\x87\x8E"},
    {"nLl", "\xE2\x8B\x98\xCC\xB8"},
    {"nLt", "\xE2\x89\xAA\xE2\x83\x92"},
    {"nLtv", "\xE2\x89\xAA\xCC\xB8"},
    {"nRightarrow", "\xE2\x87\x8F"},
    {"nVDash", "\xE2\x8A\xAF"},
    {"nVdash", "\xE2\x8A\xAE"},
    {"nabla", "\xE2\x88\x87"},
    {"nacute", "\xC5\x84"},
    {"nang", "\xE2\x88\xA0\xE2\x83\x92"},
    {"nap", "\xE2\x89\x89"},
    {"napE", "\xE2\xA9\xB0\xCC\xB8"},
    {"napid", "\xE2\x89\x8B\xCC\xB8"},
    {"napos", "\xC5\x89"},
    {"napprox", "\xE2\x89\x89"},
    {"natur", "\xE2\x99\xAE"},
    {"natural", "\xE2\x99\xAE"},
    {"naturals", "\xE2\x84\x95"},
    {"nbsp", "\xC2\xA0"},
    {"nbump", "\xE2\x89\x8E\xCC\xB8"},
    {"nbumpe", "\xE2\x89\x8F\xCC\xB8"},
    {"ncap", "\xE2\xA9\x83"},
    {"ncaron", "\xC5\x88"},
    {"ncedil", "\xC5\x86"},
    {"ncong", "\xE2\x89\x87"},
    {"ncongdot", "\xE2\xA9\xAD\xCC\xB8"},
    {"ncup", "\xE2\xA9\x82"},
    {"ncy", "\xD0\xBD"},
    {"ndash", "\xE2\x80\x93"},
    {"ne", "\xE2\x89\xA0"},
    {"neArr", "\xE2\x87\x97"},
    {"nearhk", "\xE2\xA4\xA4"},
    {"nearr", "\xE2\x86\x97"},
    {"nearrow", "\xE2\x86\x97"},
    {"nedot", "\xE2\x89\x90\xCC\xB8"},
    {"nequiv", "\xE2\x89\xA2"},
    {"nesear", "\xE2\xA4\xA8"},
    {"nesim", "\xE2\x89\x82\xCC\xB8"},
    {"nexist", "\xE2\x88\x84"},
    {"nexists", "\xE2\x88\x84"},
    {"nfr", "\xF0\x9D\x94\xAB"},
    {"ngE", "\xE2\x89\xA7\xCC\xB8"},
    {"nge", "\xE2\x89\xB1"},
    {"ngeq", "\xE2\x89\xB1"},
    {"ngeqq", "\xE2\x89\xA7\xCC\xB8"},
    {"ngeqslant", "\xE2\xA9\xBE\xCC\xB8"},
    {"nges", "\xE2\xA9\xBE\xCC\xB8"},
    {"ngsim", "\xE2\x89\xB5"},
    {"ngt", "\xE2\x89\xAF"},
    {"ngtr", "\xE2\x89\xAF"},
    {"nhArr", "\xE2\x87\x8E"},
    {"nharr", "\xE2\x86\xAE"},
    {"nhpar", "\xE2\xAB\xB2"},
    {"ni", "\xE2\x88\x8B"},
    {"nis", "\xE2\x8B\xBC"},
    {"nisd", "\xE2\x8B\xBA"},
    {"niv", "\xE2\x88\x8B"},
    {"njcy", "\xD1\x9A"},
    {"nlArr", "\xE2\x87\x8D"},
    {"nlE", "\xE2\x89\xA6\xCC\xB8"},
    {"nlarr", "\xE2\x86\x9A"},
    {"nldr", "\xE2\x80\xA5"},
    {"nle", "\xE2\x89\xB0"},
    {"nleftarrow", "\xE2\x86\x9A"},
    {"nleftrightarrow", "\xE2\x86\xAE"},
    {"nleq", "\xE2\x89\xB0"},
    {"nleqq", "\xE2\x89\xA6\xCC\xB8"},
    {"nleqslant", "\xE2\xA9\xBD\xCC\xB8"},
    {"nles", "\xE2\xA9\xBD\xCC\xB8"},
    {"nless", "\xE2\x89\xAE"},
    {"nlsim", "\xE2\x89\xB4"},
    {"nlt", "\xE2\x89\xAE"},
    {"nltri", "\xE2\x8B\xAA"},
    {"nltrie", "\xE2\x8B\xAC"},
    {"nmid", "\xE2\x88\xA4"},
    {"nopf", "\xF0\x9D\x95\x9F"},
    {"not", "\xC2\xAC"},
    {"notin", "\xE2\x88\x89"},
    {"notinE", "\xE2\x8B\xB9\xCC\xB8"},
    {"notindot", "\xE2\x8B\xB5\xCC\xB8"},
    {"notinva", "\xE2\x88\x89"},
    {"notinvb", "\xE2\x8B\xB7"},
    {"notinvc", "\xE2\x8B\xB6"},
    {"notni", "\xE2\x88\x8C"},
    {"notniva", "\xE2\x88\x8C"},
    {"notnivb", "\xE2\x8B\xBE"},
    {"notnivc", "\xE2\x8B\xBD"},
    {"npar", "\xE2\x88\xA6"},
    {"nparallel", "\xE2\x88\xA6"},
    {"nparsl", "\xE2\xAB\xBD\xE2\x83\xA5"},
    {"npart", "\xE2\x88\x82\xCC\xB8"},
    {"npolint", "\xE2\xA8\x94"},
    {"npr", "\xE2\x8A\x80"},
    {"nprcue", "\xE2\x8B\xA0"},
    {"npre", "\xE2\xAA\xAF\xCC\xB8"},
    {"nprec", "\xE2\x8A\x80"},
    {"npreceq", "\xE2\xAA\xAF\xCC\xB8"},
    {"nrArr", "\xE2\x87\x8F"},
    {"nrarr", "\xE2\x86\x9B"},
    {"nrarrc", "\xE2\xA4\xB3\xCC\xB8"},
    {"nrarrw", "\xE2\x86\x9D\xCC\xB8"},
    {"nrightarrow", "\xE2\x86\x9B"},
    {"nrtri", "\xE2\x8B\xAB"},
    {"nrtrie", "\xE2\x8B\xAD"},
    {"nsc", "\xE2\x8A\x81"},
    {"nsccue", "\xE2\x8B\xA1"},
    {"nsce", "\xE2\xAA\xB0\xCC\xB8"},
    {"nscr", "\xF0\x9D\x93\x83"},
    {"nshortmid", "\xE2\x88\xA4"},
    {"nshortparallel", "\xE2\x88\xA6"},
    {"nsim", "\xE2\x89\x81"},
    {"nsime", "\xE2\x89\x84"},
    {"nsimeq", "\xE2\x89\x84"},
    {"nsmid", "\xE2\x88\xA4"},
    {"nspar", "\xE2\x88\xA6"},
    {"nsqsube", "\xE2\x8B\xA2"},
    {"nsqsupe", "\xE2\x8B\xA3"},
    {"nsub", "\xE2\x8A\x84"},
    {"nsubE", "\xE2\xAB\x85\xCC\xB8"},
    {"nsube", "\xE2\x8A\x88"},
    {"nsubset", "\xE2\x8A\x82\xE2\x83\x92"},
    {"nsubseteq", "\xE2\x8A\x88"},
    {"nsubseteqq", "\xE2\xAB\x85\xCC\xB8"},
    {"nsucc", "\xE2\x8A\x81"},
    {"nsucceq", "\xE2\xAA\xB0\xCC\xB8"},
    {"nsup", "\xE2\x8A\x85"},
    {"nsupE", "\xE2\xAB\x86\xCC\xB8"},
    {"nsupe", "\xE2\x8A\x89"},
    {"nsupset", "\xE2\x8A\x83\xE2\x83\x92"},
    {"nsupseteq", "\xE2\x8A\x89"},
    {"nsupseteqq", "\xE2\xAB\x86\xCC\xB8"},
    {"ntgl", "\xE2\x89\xB9"},
    {"ntilde", "\xC3\xB1"},
    {"ntlg", "\xE2\x89\xB8"},
    {"ntriangleleft", "\xE2\x8B\xAA"},
    {"ntrianglelefteq", "\xE2\x8B\xAC"},
    {"ntriangleright", "\xE2\x8B\xAB"},
    {"ntrianglerighteq", "\xE2\x8B\xAD"},
    {"nu", "\xCE\xBD"},
    {"num", "\x23"},
    {"numero", "\xE2\x84\x96"},
    {"numsp", "\xE2\x80\x87"},
    {"nvDash", "\xE2\x8A\xAD"},
    {"nvHarr", "\xE2\xA4\x84"},
    {"nvap", "\xE2\x89\x8D\xE2\x83\x92"},
    {"nvdash", "\xE2\x8A\xAC"},
    {"nvge", "\xE2\x89\xA5\xE2\x83\x92"},
    {"nvgt", "\x3E\xE2\x83\x92"},
    {"nvinfin", "\xE2\xA7\x9E"},
    {"nvlArr", "\xE2\xA4\x82"},
    {"nvle", "\xE2\x89\xA4\xE2\x83\x92"},
    {"nvlt", "\x3C\xE2\x83\x92"},
    {"nvltrie", "\xE2\x8A\xB4\xE2\x83\x92"},
    {"nvrArr", "\xE2\xA4\x83"},
    {"nvrtrie", "\xE2\x8A\xB5\xE2\x83\x92"},
    {"nvsim", "\xE2\x88\xBC\xE2\x83\x92"},
    {"nwArr", "\xE2\x87\x96"},
    {"nwarhk", "\xE2\xA4\xA3"},
    {"nwarr", "\xE2\x86\x96"},
    {"nwarrow", "\xE2\x86\x96"},
    {"nwnear", "\xE2\xA4\xA7"},
    {"oS", "\xE2\x93\x88"},
    {"oacute", "\xC3\xB3"},
    {"oast", "\xE2\x8A\x9B"},
    {"ocir", "\xE2\x8A\x9A"},
    {"ocirc", "\xC3\xB4"},
    {"ocy", "\xD0\xBE"},
    {"odash", "\xE2\x8A\x9D"},
    {"odblac", "\xC5\x91"},
    {"odiv", "\xE2\xA8\xB8"},
    {"odot", "\xE2\x8A\x99"},
    {"odsold", "\xE2\xA6\xBC"},
    {"oelig", "\xC5\x93"},
    {"ofcir", "\xE2\xA6\xBF"},
    {"ofr", "\xF0\x9D\x94\xAC"},
    {"ogon", "\xCB\x9B"},
    {"ograve", "\xC3\xB2"},
    {"ogt", "\xE2\xA7\x81"},
    {"ohbar", "\xE2\xA6\xB5"},
    {"ohm", "\xCE\xA9"},
    {"oint", "\xE2\x88\xAE"},
    {"olarr", "\xE2\x86\xBA"},
    {"olcir", "\xE2\xA6\xBE"},
    {"olcross", "\xE2\xA6\xBB"},
    {"oline", "\xE2\x80\xBE"},
    {"olt", "\xE2\xA7\x80"},
    {"omacr", "\xC5\x8D"},
    {"omega", "\xCF\x89"},
    {"omicron", "\xCE\xBF"},
    {"omid", "\xE2\xA6\xB6"},
    {"ominus", "\xE2\x8A\x96"},
    {"oopf", "\xF0\x9D\x95\xA0"},
    {"opar", "\xE2\xA6\xB7"},
    {"operp", "\xE2\xA6\xB9"},
    {"oplus", "\xE2\x8A\x95"},
    {"or", "\xE2\x88\xA8"},
    {"orarr", "\xE2\x86\xBB"},
    {"ord", "\xE2\xA9\x9D"},
    {"order", "\xE2\x84\xB4"},
    {"orderof", "\xE2\x84\xB4"},
    {"ordf", "\xC2\xAA"},
    {"ordm", "\xC2\xBA"},
    {"origof", "\xE2\x8A\xB6"},
    {"oror", "\xE2\xA9\x96"},
    {"orslope", "\xE2\xA9\x97"},
    {"orv", "\xE2\xA9\x9B"},
    {"oscr", "\xE2\x84\xB4"},
    {"oslash", "\xC3\xB8"},
    {"osol", "\xE2\x8A\x98"},
    {"otilde", "\xC3\xB5"},
    {"otimes", "\xE2\x8A\x97"},
    {"otimesas", "\xE2\xA8\xB6"},
    {"ouml", "\xC3\xB6"},
    {"ovbar", "\xE2\x8C\xBD"},
    {"par", "\xE2\x88\xA5"},
    {"para", "\xC2\xB6"},
    {"parallel", "\xE2\x88\xA5"},
    {"parsim", "\xE2\xAB\xB3"},
    {"parsl", "\xE2\xAB\xBD"},
    {"part", "\xE2\x88\x82"},
    {"pcy", "\xD0\xBF"},
    {"percnt", "\x25"},
    {"period", "\x2E"},
    {"permil", "\xE2\x80\xB0"},
    {"perp", "\xE2\x8A\xA5"},
    {"pertenk", "\xE2\x80\xB1"},
    {"pfr", "\xF0\x9D\x94\xAD"},
    {"phi", "\xCF\x86"},
    {"phiv", "\xCF\x95"},
    {"phmmat", "\xE2\x84\xB3"},
    {"phone", "\xE2\x98\x8E"},
    {"pi", "\xCF\x80"},
    {"pitchfork", "\xE2\x8B\x94"},
    {"piv", "\xCF\x96"},
    {"planck", "\xE2\x84\x8F"},
    {"planckh", "\xE2\x84\x8E"},
    {"plankv", "\xE2\x84\x8F"},
    {"plus", "\x2B"},
    {"plusacir", "\xE2\xA8\xA3"},
    {"plusb", "\xE2\x8A\x9E"},
    {"pluscir", "\xE2\xA8\xA2"},
    {"plusdo", "\xE2\x88\x94"},
    {"plusdu", "\xE2\xA8\xA5"},
    {"pluse", "\xE2\xA9\xB2"},
    {"plusmn", "\xC2\xB1"},
    {"plussim", "\xE2\xA8\xA6"},
    {"plustwo", "\xE2\xA8\xA7"},
    {"pm", "\xC2\xB1"},
    {"pointint", "\xE2\xA8\x95"},
    {"popf", "\xF0\x9D\x95\xA1"},
    {"pound", "\xC2\xA3"},
    {"pr", "\xE2\x89\xBA"},
    {"prE", "\xE2\xAA\xB3"},
    {"prap", "\xE2\xAA\xB7"},
    {"prcue", "\xE2\x89\xBC"},
    {"pre", "\xE2\xAA\xAF"},
    {"prec", "\xE2\x89\xBA"},
    {"precapprox", "\xE2\xAA\xB7"},
    {"preccurlyeq", "\xE2\x89\xBC"},
    {"preceq", "\xE2\xAA\xAF"},
    {"precnapprox", "\xE2\xAA\xB9"},
    {"precneqq", "\xE2\xAA\xB5"},
    {"precnsim", "\xE2\x8B\xA8"},
    {"precsim", "\xE2\x89\xBE"},
    {"prime", "\xE2\x80\xB2"},
    {"primes", "\xE2\x84\x99"},
    {"prnE", "\xE2\xAA\xB5"},
    {"prnap", "\xE2\xAA\xB9"},
    {"prnsim", "\xE2\x8B\xA8"},
    {"prod", "\xE2\x88\x8F"},
    {"profalar", "\xE2\x8C\xAE"},
    {"profline", "\xE2\x8C\x92"},
    {"profsurf", "\xE2\x8C\x93"},
    {"prop", "\xE2\x88\x9D"},
    {"propto", "\xE2\x88\x9D"},
    {"prsim", "\xE2\x89\xBE"},
    {"prurel", "\xE2\x8A\xB0"},
    {"pscr", "\xF0\x9D\x93\x85"},
    {"psi", "\xCF\x88"},
    {"puncsp", "\xE2\x80\x88"},
    {"qfr", "\xF0\x9D\x94\xAE"},
    {"qint", "\xE2\xA8\x8C"},
    {"qopf", "\xF0\x9D\x95\xA2"},
    {"qprime", "\xE2\x81\x97"},
    {"qscr", "\xF0\x9D\x93\x86"},
    {"quaternions", "\xE2\x84\x8D"},
    {"quatint", "\xE2\xA8\x96"},
    {"quest", "\x3F"},
    {"questeq", "\xE2\x89\x9F"},
    {"quot", "\x22"},
    {"rAarr", "\xE2\x87\x9B"},
    {"rArr", "\xE2\x87\x92"},
    {"rAtail", "\xE2\xA4\x9C"},
    {"rBarr", "\xE2\xA4\x8F"},
    {"rHar", "\xE2\xA5\xA4"},
    {"race", "\xE2\x88\xBD\xCC\xB1"},
    {"racute", "\xC5\x95"},
    {"radic", "\xE2\x88\x9A"},
    {"raemptyv", "\xE2\xA6\xB3"},
    {"rang", "\xE2\x9F\xA9"},
    {"rangd", "\xE2\xA6\x92"},
    {"range", "\xE2\xA6\xA5"},
    {"rangle", "\xE2\x9F\xA9"},
    {"raquo", "\xC2\xBB"},
    {"rarr", "\xE2\x86\x92"},
    {"rarrap", "\xE2\xA5\xB5"},
    {"rarrb", "\xE2\x87\xA5"},
    {"rarrbfs", "\xE2\xA4\xA0"},
    {"rarrc", "\xE2\xA4\xB3"},
    {"rarrfs", "\xE2\xA4\x9E"},
    {"rarrhk", "\xE2\x86\xAA"},
    {"rarrlp", "\xE2\x86\xAC"},
    {"rarrpl", "\xE2\xA5\x85"},
    {"rarrsim", "\xE2\xA5\xB4"},
    {"rarrtl", "\xE2\x86\xA3"},
    {"rarrw", "\xE2\x86\x9D"},
    {"ratail", "\xE2\xA4\x9A"},
    {"ratio", "\xE2\x88\xB6"},
    {"rationals", "\xE2\x84\x9A"},
    {"rbarr", "\xE2\xA4\x8D"},
    {"rbbrk", "\xE2\x9D\xB3"},
    {"rbrace", "\x7D"},
    {"rbrack", "\x5D"},
    {"rbrke", "\xE2\xA6\x8C"},
    {"rbrksld", "\xE2\xA6\x8E"},
    {"rbrkslu", "\xE2\xA6\x90"},
    {"rcaron", "\xC5\x99"},
    {"rcedil", "\xC5\x97"},
    {"rceil", "\xE2\x8C\x89"},
    {"rcub", "\x7D"},
    {"rcy", "\xD1\x80"},
    {"rdca", "\xE2\xA4\xB7"},
    {"rdldhar", "\xE2\xA5\xA9"},
    {"rdquo", "\xE2\x80\x9D"},
    {"rdquor", "\xE2\x80\x9D"},
    {"rdsh", "\xE2\x86\xB3"},
    {"real", "\xE2\x84\x9C"},
    {"realine", "\xE2\x84\x9B"},
    {"realpart", "\xE2\x84\x9C"},
    {"reals", "\xE2\x84\x9D"},
    {"rect", "\xE2\x96\xAD"},
    {"reg", "\xC2\xAE"},
    {"rfisht", "\xE2\xA5\xBD"},
    {"rfloor", "\xE2\x8C\x8B"},
    {"rfr", "\xF0\x9D\x94\xAF"},
    {"rhard", "\xE2\x87\x81"},
    {"rharu", "\xE2\x87\x80"},
    {"rharul", "\xE2\xA5\xAC"},
    {"rho", "\xCF\x81"},
    {"rhov", "\xCF\xB1"},
    {"rightarrow", "\xE2\x86\x92"},
    {"rightarrowtail", "\xE2\x86\xA3"},
    {"rightharpoondown", "\xE2\x87\x81"},
    {"rightharpoonup", "\xE2\x87\x80"},
    {"rightleftarrows", "\xE2\x87\x84"},
    {"rightleftharpoons", "\xE2\x87\x8C"},
    {"rightrightarrows", "\xE2\x87\x89"},
    {"rightsquigarrow", "\xE2\x86\x9D"},
    {"rightthreetimes", "\xE2\x8B\x8C"},
    {"ring", "\xCB\x9A"},
    {"risingdotseq", "\xE2\x89\x93"},
    {"rlarr", "\xE2\x87\x84"},
    {"rlhar", "\xE2\x87\x8C"},
    {"rlm", "\xE2\x80\x8F"},
    {"rmoust", "\xE2\x8E\xB1"},
    {"rmoustache", "\xE2\x8E\xB1"},
    {"rnmid", "\xE2\xAB\xAE"},
    {"roang", "\xE2\x9F\xAD"},
    {"roarr", "\xE2\x87\xBE"},
    {"robrk", "\xE2\x9F\xA7"},
    {"ropar", "\xE2\xA6\x86"},
    {"ropf", "\xF0\x9D\x95\xA3"},
    {"roplus", "\xE2\xA8\xAE"},
    {"rotimes", "\xE2\xA8\xB5"},
    {"rpar", "\x29"},
    {"rpargt", "\xE2\xA6\x94"},
    {"rppolint", "\xE2\xA8\x92"},
    {"rrarr", "\xE2\x87\x89"},
    {"rsaquo", "\xE2\x80\xBA"},
    {"rscr", "\xF0\x9D\x93\x87"},
    {"rsh", "\xE2\x86\xB1"},
    {"rsqb", "\x5D"},
    {"rsquo", "\xE2\x80\x99"},
    {"rsquor", "\xE2\x80\x99"},
    {"rthree", "\xE2\x8B\x8C"},
    {"rtimes", "\xE2\x8B\x8A"},
    {"rtri", "\xE2\x96\xB9"},
    {"rtrie", "\xE2\x8A\xB5"},
    {"rtrif", "\xE2\x96\xB8"},
    {"rtriltri", "\xE2\xA7\x8E"},
    {"ruluhar", "\xE2\xA5\xA8"},
    {"rx", "\xE2\x84\x9E"},
    {"sacute", "\xC5\x9B"},
    {"sbquo", "\xE2\x80\x9A"},
    {"sc", "\xE2\x89\xBB"},
    {"scE", "\xE2\xAA\xB4"},
    {"scap", "\xE2\xAA\xB8"},
    {"scaron", "\xC5\xA1"},
    {"sccue", "\xE2\x89\xBD"},
    {"sce", "\xE2\xAA\xB0"},
    {"scedil", "\xC5\x9F"},
    {"scirc", "\xC5\x9D"},
    {"scnE", "\xE2\xAA\xB6"},
    {"scnap", "\xE2\xAA\xBA"},
    {"scnsim", "\xE2\x8B\xA9"},
    {"scpolint", "\xE2\xA8\x93"},
    {"scsim", "\xE2\x89\xBF"},
    {"scy", "\xD1\x81"},
    {"sdot", "\xE2\x8B\x85"},
    {"sdotb", "\xE2\x8A\xA1"},
    {"sdote", "\xE2\xA9\xA6"},
    {"seArr", "\xE2\x87\x98"},
    {"searhk", "\xE2\xA4\xA5"},
    {"searr", "\xE2\x86\x98"},
    {"searrow", "\xE2\x86\x98"},
    {"sect", "\xC2\xA7"},
    {"semi", "\x3B"},
    {"seswar", "\xE2\xA4\xA9"},
    {"setminus", "\xE2\x88\x96"},
    {"setmn", "\xE2\x88\x96"},
    {"sext", "\xE2\x9C\xB6"},
    {"sfr", "\xF0\x9D\x94\xB0"},
    {"sfrown", "\xE2\x8C\xA2"},
    {"sharp", "\xE2\x99\xAF"},
    {"shchcy", "\xD1\x89"},
    {"shcy", "\xD1\x88"},
    {"shortmid", "\xE2\x88\xA3"},
    {"shortparallel", "\xE2\x88\xA5"},
    {"shy", "\xC2\xAD"},
    {"sigma", "\xCF\x83"},
    {"sigmaf", "\xCF\x82"},
    {"sigmav", "\xCF\x82"},
    {"sim", "\xE2\x88\xBC"},
    {"simdot", "\xE2\xA9\xAA"},
    {"sime", "\xE2\x89\x83"},
    {"simeq", "\xE2\x89\x83"},
    {"simg", "\xE2\xAA\x9E"},
    {"simgE", "\xE2\xAA\xA0"},
    {"siml", "\xE2\xAA\x9D"},
    {"simlE", "\xE2\xAA\x9F"},
    {"simne", "\xE2\x89\x86"},
    {"simplus", "\xE2\xA8\xA4"},
    {"simrarr", "\xE2\xA5\xB2"},
    {"slarr", "\xE2\x86\x90"},
    {"smallsetminus", "\xE2\x88\x96"},
    {"smashp", "\xE2\xA8\xB3"},
    {"smeparsl", "\xE2\xA7\xA4"},
    {"smid", "\xE2\x88\xA3"},
    {"smile", "\xE2\x8C\xA3"},
    {"smt", "\xE2\xAA\xAA"},
    {"smte", "\xE2\xAA\xAC"},
    {"smtes", "\xE2\xAA\xAC\xEF\xB8\x80"},
    {"softcy", "\xD1\x8C"},
    {"sol", "\x2F"},
    {"solb", "\xE2\xA7\x84"},
    {"solbar", "\xE2\x8C\xBF"},
    {"sopf", "\xF0\x9D\x95\xA4"},
    {"spades", "\xE2\x99\xA0"},
    {"spadesuit", "\xE2\x99\xA0"},
    {"spar", "\xE2\x88\xA5"},
    {"sqcap", "\xE2\x8A\x93"},
    {"sqcaps", "\xE2\x8A\x93\xEF\xB8\x80"},
    {"sqcup", "\xE2\x8A\x94"},
    {"sqcups", "\xE2\x8A\x94\xEF\xB8\x80"},
    {"sqsub", "\xE2\x8A\x8F"},
    {"sqsube", "\xE2\x8A\x91"},
    {"sqsubset", "\xE2\x8A\x8F"},
    {"sqsubseteq", "\xE2\x8A\x91"},
    {"sqsup", "\xE2\x8A\x90"},
    {"sqsupe", "\xE2\x8A\x92"},
    {"sqsupset", "\xE2\x8A\x90"},
    {"sqsupseteq", "\xE2\x8A\x92"},
    {"squ", "\xE2\x96\xA1"},
    {"square", "\xE2\x96\xA1"},
    {"squarf", "\xE2\x96\xAA"},
    {"squf", "\xE2\x96\xAA"},
    {"srarr", "\xE2\x86\x92"},
    {"sscr", "\xF0\x9D\x93\x88"},
    {"ssetmn", "\xE2\x88\x96"},
    {"ssmile", "\xE2\x8C\xA3"},
    {"sstarf", "\xE2\x8B\x86"},
    {"star", "\xE2\x98\x86"},
    {"starf", "\xE2\x98\x85"},
    {"straightepsilon", "\xCF\xB5"},
    {"straightphi", "\xCF\x95"},
    {"strns", "\xC2\xAF"},
    {"sub", "\xE2\x8A\x82"},
    {"subE", "\xE2\xAB\x85"},
    {"subdot", "\xE2\xAA\xBD"},
    {"sube", "\xE2\x8A\x86"},
    {"subedot", "\xE2\xAB\x83"},
    {"submult", "\xE2\xAB\x81"},
    {"subnE", "\xE2\xAB\x8B"},
    {"subne", "\xE2\x8A\x8A"},
    {"subplus", "\xE2\xAA\xBF"},
    {"subrarr", "\xE2\xA5\xB9"},
    {"subset", "\xE2\x8A\x82"},
    {"subseteq", "\xE2\x8A\x86"},
    {"subseteqq", "\xE2\xAB\x85"},
    {"subsetneq", "\xE2\x8A\x8A"},
    {"subsetneqq", "\xE2\xAB\x8B"},
    {"subsim", "\xE2\xAB\x87"},
    {"subsub", "\xE2\xAB\x95"},
    {"subsup", "\xE2\xAB\x93"},
    {"succ", "\xE2\x89\xBB"},
    {"succapprox", "\xE2\xAA\xB8"},
    {"succcurlyeq", "\xE2\x89\xBD"},
    {"succeq", "\xE2\xAA\xB0"},
    {"succnapprox", "\xE2\xAA\xBA"},
    {"succneqq", "\xE2\xAA\xB6"},
    {"succnsim", "\xE2\x8B\xA9"},
    {"succsim", "\xE2\x89\xBF"},
    {"sum", "\xE2\x88\x91"},
    {"sung", "\xE2\x99\xAA"},
    {"sup", "\xE2\x8A\x83"},
    {"sup1", "\xC2\xB9"},
    {"sup2", "\xC2\xB2"},
    {"sup3", "\xC2\xB3"},
    {"supE", "\xE2\xAB\x86"},
    {"supdot", "\xE2\xAA\xBE"},
    {"supdsub", "\xE2\xAB\x98"},
    {"supe", "\xE2\x8A\x87"},
    {"supedot", "\xE2\xAB\x84"},
    {"suphsol", "\xE2\x9F\x89"},
    {"suphsub", "\xE2\xAB\x97"},
    {"suplarr", "\xE2\xA5\xBB"},
    {"supmult", "\xE2\xAB\x82"},
    {"supnE", "\xE2\xAB\x8C"},
    {"supne", "\xE2\x8A\x8B"},
    {"supplus", "\xE2\xAB\x80"},
    {"supset", "\xE2\x8A\x83"},
    {"supseteq", "\xE2\x8A\x87"},
    {"supseteqq", "\xE2\xAB\x86"},
    {"supsetneq", "\xE2\x8A\x8B"},
    {"supsetneqq", "\xE2\xAB\x8C"},
    {"supsim", "\xE2\xAB\x88"},
    {"supsub", "\xE2\xAB\x94"},
    {"supsup", "\xE2\xAB\x96"},
    {"swArr", "\xE2\x87\x99"},
    {"swarhk", "\xE2\xA4\xA6"},
    {"swarr", "\xE2\x86\x99"},
    {"swarrow", "\xE2\x86\x99"},
    {"swnwar", "\xE2\xA4\xAA"},
    {"szlig", "\xC3\x9F"},
    {"target", "\xE2\x8C\x96"},
    {"tau", "\xCF\x84"},
    {"tbrk", "\xE2\x8E\xB4"},
    {"tcaron", "\xC5\xA5"},
    {"tcedil", "\xC5\xA3"},
    {"tcy", "\xD1\x82"},
    {"tdot", "\xE2\x83\x9B"},
    {"telrec", "\xE2\x8C\x95"},
    {"tfr", "\xF0\x9D\x94\xB1"},
    {"there4", "\xE2\x88\xB4"},
    {"therefore", "\xE2\x88\xB4"},
    {"theta", "\xCE\xB8"},
    {"thetasym", "\xCF\x91"},
    {"thetav", "\xCF\x91"},
    {"thickapprox", "\xE2\x89\x88"},
    {"thicksim", "\xE2\x88\xBC"},
    {"thinsp", "\xE2\x80\x89"},
    {"thkap", "\xE2\x89\x88"},
    {"thksim", "\xE2\x88\xBC"},
    {"thorn", "\xC3\xBE"},
    {"tilde", "\xCB\x9C"},
    {"times", "\xC3\x97"},
    {"timesb", "\xE2\x8A\xA0"},
    {"timesbar", "\xE2\xA8\xB1"},
    {"timesd", "\xE2\xA8\xB0"},
    {"tint", "\xE2\x88\xAD"},
    {"toea", "\xE2\xA4\xA8"},
    {"top", "\xE2\x8A\xA4"},
    {"topbot", "\xE2\x8C\xB6"},
    {"topcir", "\xE2\xAB\xB1"},
    {"topf", "\xF0\x9D\x95\xA5"},
    {"topfork", "\xE2\xAB\x9A"},
    {"tosa", "\xE2\xA4\xA9"},
    {"tprime", "\xE2\x80\xB4"},
    {"trade", "\xE2\x84\xA2"},
    {"triangle", "\xE2\x96\xB5"},
    {"triangledown", "\xE2\x96\xBF"},
    {"triangleleft", "\xE2\x97\x83"},
    {"trianglelefteq", "\xE2\x8A\xB4"},
    {"triangleq", "\xE2\x89\x9C"},
    {"triangleright", "\xE2\x96\xB9"},
    {"trianglerighteq", "\xE2\x8A\xB5"},
    {"tridot", "\xE2\x97\xAC"},
    {"trie", "\xE2\x89\x9C"},
    {"triminus", "\xE2\xA8\xBA"},
    {"triplus", "\xE2\xA8\xB9"},
    {"trisb", "\xE2\xA7\x8D"},
    {"tritime", "\xE2\xA8\xBB"},
    {"trpezium", "\xE2\x8F\xA2"},
    {"tscr", "\xF0\x9D\x93\x89"},
    {"tscy", "\xD1\x86"},
    {"tshcy", "\xD1\x9B"},
    {"tstrok", "\xC5\xA7"},
    {"twixt", "\xE2\x89\xAC"},
    {"twoheadleftarrow", "\xE2\x86\x9E"},
    {"twoheadrightarrow", "\xE2\x86\xA0"},
    {"uArr", "\xE2\x87\x91"},
    {"uHar", "\xE2\xA5\xA3"},
    {"uacute", "\xC3\xBA"},
    {"uarr", "\xE2\x86\x91"},
    {"ubrcy", "\xD1\x9E"},
    {"ubreve", "\xC5\xAD"},
    {"ucirc", "\xC3\xBB"},
    {"ucy", "\xD1\x83"},
    {"udarr", "\xE2\x87\x85"},
    {"udblac", "\xC5\xB1"},
    {"udhar", "\xE2\xA5\xAE"},
    {"ufisht", "\xE2\xA5\xBE"},
    {"ufr", "\xF0\x9D\x94\xB2"},
    {"ugrave", "\xC3\xB9"},
    {"uharl", "\xE2\x86\xBF"},
    {"uharr", "\xE2\x86\xBE"},
    {"uhblk", "\xE2\x96\x80"},
    {"ulcorn", "\xE2\x8C\x9C"},
    {"ulcorner", "\xE2\x8C\x9C"},
    {"ulcrop", "\xE2\x8C\x8F"},
    {"ultri", "\xE2\x97\xB8"},
    {"umacr", "\xC5\xAB"},
    {"uml", "\xC2\xA8"},
    {"uogon", "\xC5\xB3"},
    {"uopf", "\xF0\x9D\x95\xA6"},
    {"uparrow", "\xE2\x86\x91"},
    {"updownarrow", "\xE2\x86\x95"},
    {"upharpoonleft", "\xE2\x86\xBF"},
    {"upharpoonright", "\xE2\x86\xBE"},
    {"uplus", "\xE2\x8A\x8E"},
    {"upsi", "\xCF\x85"},
    {"upsih", "\xCF\x92"},
    {"upsilon", "\xCF\x85"},
    {"upuparrows", "\xE2\x87\x88"},
    {"urcorn", "\xE2\x8C\x9D"},
    {"urcorner", "\xE2\x8C\x9D"},
    {"urcrop", "\xE2\x8C\x8E"},
    {"uring", "\xC5\xAF"},
    {"urtri", "\xE2\x97\xB9"},
    {"uscr", "\xF0\x9D\x93\x8A"},
    {"utdot", "\xE2\x8B\xB0"},
    {"utilde", "\xC5\xA9"},
    {"utri", "\xE2\x96\xB5"},
    {"utrif", "\xE2\x96\xB4"},
    {"uuarr", "\xE2\x87\x88"},
    {"uuml", "\xC3\xBC"},
    {"uwangle", "\xE2\xA6\xA7"},
    {"vArr", "\xE2\x87\x95"},
    {"vBar", "\xE2\xAB\xA8"},
    {"vBarv", "\xE2\xAB\xA9"},
    {"vDash", "\xE2\x8A\xA8"},
    {"vangrt", "\xE2\xA6\x9C"},
    {"varepsilon", "\xCF\xB5"},
    {"varkappa", "\xCF\xB0"},
    {"varnothing", "\xE2\x88\x85"},
    {"varphi", "\xCF\x95"},
    {"varpi", "\xCF\x96"},
    {"varpropto", "\xE2\x88\x9D"},
    {"varr", "\xE2\x86\x95"},
    {"varrho", "\xCF\xB1"},
    {"varsigma", "\xCF\x82"},
    {"varsubsetneq", "\xE2\x8A\x8A\xEF\xB8\x80"},
    {"varsubsetneqq", "\xE2\xAB\x8B\xEF\xB8\x80"},
    {"varsupsetneq", "\xE2\x8A\x8B\xEF\xB8\x80"},
    {"varsupsetneqq", "\xE2\xAB\x8C\xEF\xB8\x80"},
    {"vartheta", "\xCF\x91"},
    {"vartriangleleft", "\xE2\x8A\xB2"},
    {"vartriangleright", "\xE2\x8A\xB3"},
    {"vcy", "\xD0\xB2"},
    {"vdash", "\xE2\x8A\xA2"},
    {"vee", "\xE2\x88\xA8"},
    {"veebar", "\xE2\x8A\xBB"},
    {"veeeq", "\xE2\x89\x9A"},
    {"vellip", "\xE2\x8B\xAE"},
    {"verbar", "\x7C"},
    {"vert", "\x7C"},
    {"vfr", "\xF0\x9D\x94\xB3"},
    {"vltri", "\xE2\x8A\xB2"},
    {"vnsub", "\xE2\x8A\x82\xE2\x83\x92"},
    {"vnsup", "\xE2\x8A\x83\xE2\x83\x92"},
    {"vopf", "\xF0\x9D\x95\xA7"},
    {"vprop", "\xE2\x88\x9D"},
    {"vrtri", "\xE2\x8A\xB3"},
    {"vscr", "\xF0\x9D\x93\x8B"},
    {"vsubnE", "\xE2\xAB\x8B\xEF\xB8\x80"},
    {"vsubne", "\xE2\x8A\x8A\xEF\xB8\x80"},
    {"vsupnE", "\xE2\xAB\x8C\xEF\xB8\x80"},
    {"vsupne", "\xE2\x8A\x8B\xEF\xB8\x80"},
    {"vzigzag", "\xE2\xA6\x9A"},
    {"wcirc", "\xC5\xB5"},
    {"wedbar", "\xE2\xA9\x9F"},
    {"wedge", "\xE2\x88\xA7"},
    {"wedgeq", "\xE2\x89\x99"},
    {"weierp", "\xE2\x84\x98"},
    {"wfr", "\xF0\x9D\x94\xB4"},
    {"wopf", "\xF0\x9D\x95\xA8"},
    {"wp", "\xE2\x84\x98"},
    {"wr", "\xE2\x89\x80"},
    {"wreath", "\xE2\x89\x80"},
    {"wscr", "\xF0\x9D\x93\x8C"},
    {"xcap", "\xE2\x8B\x82"},
    {"xcirc", "\xE2\x97\xAF"},
    {"xcup", "\xE2\x8B\x83"},
    {"xdtri", "\xE2\x96\xBD"},
    {"xfr", "\xF0\x9D\x94\xB5"},
    {"xhArr", "\xE2\x9F\xBA"},
    {"xharr", "\xE2\x9F\xB7"},
    {"xi", "\xCE\xBE"},
    {"xlArr", "\xE2\x9F\xB8"},
    {"xlarr", "\xE2\x9F\xB5"},
    {"xmap", "\xE2\x9F\xBC"},
    {"xnis", "\xE2\x8B\xBB"},
    {"xodot", "\xE2\xA8\x80"},
    {"xopf", "\xF0\x9D\x95\xA9"},
    {"xoplus", "\xE2\xA8\x81"},
    {"xotime", "\xE2\xA8\x82"},
    {"xrArr", "\xE2\x9F\xB9"},
    {"xrarr", "\xE2\x9F\xB6"},
    {"xscr", "\xF0\x9D\x93\x8D"},
    {"xsqcup", "\xE2\xA8\x86"},
    {"xuplus", "\xE2\xA8\x84"},
    {"xutri", "\xE2\x96\xB3"},
    {"xvee", "\xE2\x8B\x81"},
    {"xwedge", "\xE2\x8B\x80"},
    {"yacute", "\xC3\xBD"},
    {"yacy", "\xD1\x8F"},
    {"ycirc", "\xC5\xB7"},
    {"ycy", "\xD1\x8B"},
    {"yen", "\xC2\xA5"},
    {"yfr", "\xF0\x9D\x94\xB6"},
    {"yicy", "\xD1\x97"},
    {"yopf", "\xF0\x9D\x95\xAA"},
    {"yscr", "\xF0\x9D\x93\x8E"},
    {"yucy", "\xD1\x8E"},
    {"yuml", "\xC3\xBF"},
    {"zacute", "\xC5\xBA"},
    {"zcaron", "\xC5\xBE"},
    {"zcy", "\xD0\xB7"},
    {"zdot", "\xC5\xBC"},
    {"zeetrf", "\xE2\x84\xA8"},
    {"zeta", "\xCE\xB6"},
    {"zfr", "\xF0\x9D\x94\xB7"},
    {"zhcy", "\xD0\xB6"},
    {"zigrarr", "\xE2\x87\x9D"},
    {"zopf", "\xF0\x9D\x95\xAB"},
    {"zscr", "\xF0\x9D\x93\x8F"},
    {"zwj", "\xE2\x80\x8D"},
    {"zwnj", "\xE2\x80\x8C"},
};
const size_t k_entity_count = 2125;

const entity_pair *find_entity(kimix::string_view name) {
    // The table is sorted by name; use a binary search over the array.
    extern const entity_pair k_entities[];
    extern const size_t k_entity_count;
    size_t lo = 0;
    size_t hi = k_entity_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = name.compare(k_entities[mid].name);
        if (cmp == 0) return &k_entities[mid];
        if (cmp < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Numeric character references (bs4 UnicodeDammit.numeric_character_reference)
// ---------------------------------------------------------------------------

// Windows-1252 -> Unicode for C1 controls 0x80..0x9F (bs4 table; missing
// entries 0x81/0x8D/0x8F/0x90/0x9D resolve as-is).
uint32_t windows1252_map(uint32_t cp) {
    switch (cp) {
    case 0x80: return 0x20AC; // ?
    case 0x82: return 0x201A; // ?
    case 0x83: return 0x0192; // ?
    case 0x84: return 0x201E; // ?
    case 0x85: return 0x2026; // ...
    case 0x86: return 0x2020; // ?
    case 0x87: return 0x2021; // ?
    case 0x88: return 0x02C6; // ?
    case 0x89: return 0x2030; // ?
    case 0x8A: return 0x0160; // ?
    case 0x8B: return 0x2039; // ?
    case 0x8C: return 0x0152; // ?
    case 0x8E: return 0x017D; // ?
    case 0x91: return 0x2018; // ?
    case 0x92: return 0x2019; // ?
    case 0x93: return 0x201C; // ?
    case 0x94: return 0x201D; // ?
    case 0x95: return 0x2022; // ?
    case 0x96: return 0x2013; // -
    case 0x97: return 0x2014; // --
    case 0x98: return 0x02DC; // ?
    case 0x99: return 0x2122; // ?
    case 0x9A: return 0x0161; // ?
    case 0x9B: return 0x203A; // ?
    case 0x9C: return 0x0153; // ?
    case 0x9E: return 0x017E; // ?
    case 0x9F: return 0x0178; // ?
    default: return cp;
    }
}

// Append the decoded character for a numeric reference; returns true when the
// reference was replaced/dropped (bs4 replacement_added). The installed bs4
// runtime (4.15.0 pyc) returns the EMPTY string for null / out-of-range /
// surrogate references, so nothing is appended for those (mirrors the
// reference's observable behaviour; the source file would return U+FFFD).
bool numeric_reference(uint32_t numeric, kimix::string &out) {
    if (numeric == 0x00 || numeric > 0x10FFFF ||
        (numeric >= 0xD800 && numeric <= 0xDFFF)) {
        return true;
    }
    if (numeric >= 0x80 && numeric <= 0x9F) {
        uint32_t mapped = windows1252_map(numeric);
        if (mapped != numeric) {
            append_utf8(out, mapped);
            return false;
        }
    }
    append_utf8(out, numeric);
    return false;
}

// bs4 _dereference_numeric_character_reference: decode `name` (text after
// "&#"), extracting a numeric prefix when the whole name is not numeric.
void dereference_numeric(kimix::string_view name, kimix::string &out) {
    bool hex = false;
    kimix::string_view digits = name;
    if (!name.empty() && (name[0] == 'x' || name[0] == 'X')) {
        hex = true;
        digits = name.substr(1);
    }
    uint64_t value = 0;
    bool numeric_ok = !digits.empty();
    for (char c : digits) {
        int d = -1;
        if (ascii_digit(c)) d = c - '0';
        else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else {
            numeric_ok = false;
            break;
        }
        if (value > (UINT64_MAX - static_cast<uint64_t>(d)) / 16) {
            numeric_ok = false;
            break;
        }
        value = value * (hex ? 16 : 10) + static_cast<uint64_t>(d);
    }
    if (numeric_ok && !digits.empty()) {
        kimix::string tmp;
        numeric_reference(static_cast<uint32_t>(value), tmp);
        out += tmp;
        return;
    }
    // int(name, base) failed -> extract the numeric prefix, keep the rest.
    // bs4 uses _DECIMAL_REFERENCE_WITH_FOLLOWING_DATA / _HEX... = ^(digits)(.*)
    size_t prefix_len = 0;
    uint64_t prefix_value = 0;
    bool prefix_ok = false;
    for (size_t i = 0; i < digits.size(); ++i) {
        char c = digits[i];
        int d = -1;
        if (ascii_digit(c)) d = c - '0';
        else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        prefix_value = prefix_value * (hex ? 16 : 10) + static_cast<uint64_t>(d);
        prefix_ok = true;
        ++prefix_len;
    }
    if (prefix_ok && prefix_len > 0) {
        kimix::string tmp;
        numeric_reference(static_cast<uint32_t>(prefix_value), tmp);
        out += tmp;
        out.append(digits.substr(prefix_len));
        return;
    }
    out.append(name);
}

// ---------------------------------------------------------------------------
// Entity decoding in text data
// ---------------------------------------------------------------------------

// Decode a named entity reference body; when unknown, emit "&name" (bs4).
void decode_named_entity(kimix::string_view name, kimix::string &out) {
    const entity_pair *e = find_entity(name);
    if (e != nullptr) {
        out += e->value;
    } else {
        out.push_back('&');
        out.append(name);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// html_dom parsing (BeautifulSoup html.parser subset)
// ---------------------------------------------------------------------------

namespace {

bool is_void_tag(kimix::string_view t) {
    static const char *kVoid[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr",
    };
    for (const char *v : kVoid) {
        if (t == v) return true;
    }
    return false;
}

bool is_rawtext_tag(kimix::string_view t) {
    static const char *kRaw[] = {"script", "style", "xmp", "iframe",
                                 "noembed", "noframes"};
    for (const char *v : kRaw) {
        if (t == v) return true;
    }
    return false;
}

bool is_rcdata_tag(kimix::string_view t) {
    return t == "textarea" || t == "title";
}

uint32_t make_node(html_dom &dom, node_kind kind) {
    if (dom.nodes.size() >= k_max_dom_nodes) return k_invalid_node;
    dom_node nd;
    nd.kind = kind;
    dom.nodes.push_back(std::move(nd));
    return static_cast<uint32_t>(dom.nodes.size() - 1);
}

void append_child(html_dom &dom, uint32_t parent, uint32_t child) {
    dom_node &p = dom.nodes[parent];
    if (p.children.size() >= k_max_dom_nodes) return;
    dom.nodes[child].parent = parent;
    p.children.push_back(child);
}

// True when `name` (lowercase) matches the tag name list.
bool in_tag_list(kimix::string_view name,
                 kimix::span<const kimix::string_view> tags) {
    for (kimix::string_view t : tags) {
        if (name == t) return true;
    }
    return false;
}

// Decode entities inside text, appending to `out` and advancing `pos` to the
// first unconsumed byte. `end` is the scan limit (normally the next '<');
// `buffer_end` is the true end of the whole input. Mirrors html.parser +
// bs4: on an *incomplete* charref (decimal digits followed by a hex digit, or
// digits at the scan limit) the parser's close() path consumes the ENTIRE
// remaining buffer as one character reference, so the caller must not parse
// tags after it.
void decode_text_entities(kimix::string_view buffer, size_t &pos, size_t end,
                          size_t buffer_end, bool decode,
                          kimix::string &out) {
    if (!decode) {
        out.append(buffer.substr(pos, end - pos));
        pos = end;
        return;
    }
    size_t i = pos;
    const size_t n = end;
    out.reserve(out.size() + (n - i));
    while (i < n) {
        char c = buffer[i];
        if (c != '&') {
            out.push_back(c);
            ++i;
            continue;
        }
        // charref: &#(?:[0-9]+|[xX][0-9a-fA-F]+)[^0-9a-fA-F]
        if (i + 2 <= n && buffer[i + 1] == '#') {
            size_t p = i + 2;
            bool hex = false;
            size_t digit_start = p;
            if (p < n && (buffer[p] == 'x' || buffer[p] == 'X')) {
                hex = true;
                ++p;
                digit_start = p;
            }
            size_t ndigits = 0;
            while (p < n) {
                char d = buffer[p];
                if (ascii_digit(d) ||
                    (hex && ((d >= 'a' && d <= 'f') || (d >= 'A' && d <= 'F')))) {
                    ++ndigits;
                    ++p;
                } else {
                    break;
                }
            }
            if (ndigits > 0 && p < n) {
                char delim = buffer[p];
                bool is_hex_digit =
                    ascii_digit(delim) ||
                    (delim >= 'a' && delim <= 'f') || (delim >= 'A' && delim <= 'F');
                if (!is_hex_digit) {
                    // Hex names keep the leading 'x'/'X' so dereference_numeric
                    // decodes them as base 16 (bs4 handle_charref('x42')).
                    size_t name_start = hex ? digit_start - 1 : digit_start;
                    size_t name_len = hex ? ndigits + 1 : ndigits;
                    kimix::string name(buffer.substr(name_start, name_len));
                    kimix::string tmp;
                    dereference_numeric(name, tmp);
                    out += tmp;
                    // Delimiter ';' is consumed; anything else is rewound.
                    if (delim == ';') {
                        i = p + 1;
                    } else {
                        i = p;
                    }
                    continue;
                }
                // Digits followed by a hex digit: charref regex fails ->
                // incomplete charref; close() swallows the whole remainder.
                kimix::string name(buffer.substr(digit_start));
                kimix::string tmp;
                dereference_numeric(name, tmp);
                out += tmp;
                pos = buffer_end;
                return;
            }
            if (ndigits > 0 && p == n) {
                // Digits ran to the scan limit. When the limit is a '<' (or
                // "</" in RCDATA) the regex treats it as a non-hex delimiter
                // and continues; only a true end-of-buffer is incomplete.
                size_t name_start = hex ? digit_start - 1 : digit_start;
                size_t name_len = hex ? ndigits + 1 : ndigits;
                kimix::string name(buffer.substr(name_start, name_len));
                kimix::string tmp;
                dereference_numeric(name, tmp);
                out += tmp;
                if (n == buffer_end) {
                    pos = buffer_end;
                } else {
                    pos = p; // rewind so the '<' is processed next
                }
                continue;
            }
            // Neither charref nor incomplete: literal "&#".
            out.append("&#");
            i += 2;
            continue;
        }
        // entityref: &([a-zA-Z][-.a-zA-Z0-9]*)[^a-zA-Z0-9]
        if (i + 1 < n && ascii_alpha(buffer[i + 1])) {
            size_t p = i + 1;
            size_t name_start = p;
            while (p < n) {
                char d = buffer[p];
                if (ascii_alnum(d) || d == '-' || d == '.') {
                    ++p;
                } else {
                    break;
                }
            }
            if (p < n && !ascii_alnum(buffer[p])) {
                kimix::string name(buffer.substr(name_start, p - name_start));
                decode_named_entity(name, out);
                if (buffer[p] == ';') {
                    i = p + 1;
                } else {
                    i = p; // rewind delimiter (reprocessed as text)
                }
                continue;
            }
            // Incomplete entity at the scan limit: when the limit is a '<'
            // the regex matches it as a delimiter and the tag is parsed
            // next; only a true end-of-buffer consumes the remainder.
            if (p == n) {
                kimix::string name(buffer.substr(name_start));
                decode_named_entity(name, out);
                if (n == buffer_end) {
                    pos = buffer_end;
                } else {
                    pos = p; // rewind so the '<' is processed next
                }
                continue;
            }
        }
        // Bare '&' (e.g. "a & b") or "&" + non-letter.
        out.push_back('&');
        ++i;
    }
    pos = i;
}

// Decode entities inside an attribute value (html.parser _unescape_attrvalue:
// numeric refs always, named refs when exact + not followed by '=').
void decode_attr_entities(kimix::string_view value, kimix::string &out) {
    out.clear();
    out.reserve(value.size());
    size_t i = 0;
    const size_t n = value.size();
    while (i < n) {
        if (value[i] != '&') {
            out.push_back(value[i]);
            ++i;
            continue;
        }
        // Numeric refs always unescaped.
        if (i + 2 <= n && value[i + 1] == '#') {
            size_t p = i + 2;
            bool hex = false;
            if (p < n && (value[p] == 'x' || value[p] == 'X')) {
                hex = true;
                ++p;
            }
            size_t digit_start = p;
            size_t ndigits = 0;
            while (p < n) {
                char d = value[p];
                if (ascii_digit(d) ||
                    (hex && ((d >= 'a' && d <= 'f') || (d >= 'A' && d <= 'F')))) {
                    ++ndigits;
                    ++p;
                } else {
                    break;
                }
            }
            if (ndigits > 0 && p < n && value[p] == ';') {
                size_t name_start = hex ? digit_start - 1 : digit_start;
                size_t name_len = hex ? ndigits + 1 : ndigits;
                kimix::string digits(value.substr(name_start, name_len));
                kimix::string tmp;
                dereference_numeric(digits, tmp);
                out += tmp;
                i = p + 1;
                continue;
            }
            // keep as-is otherwise
            out.push_back('&');
            ++i;
            continue;
        }
        // Named ref: exact match, not followed by '='.
        if (i + 1 < n && ascii_alpha(value[i + 1])) {
            size_t p = i + 1;
            size_t name_start = p;
            while (p < n &&
                   (ascii_alnum(value[p]) || value[p] == '-' || value[p] == '.')) {
                ++p;
            }
            kimix::string name(value.substr(name_start, p - name_start));
            bool followed_by_eq = (p < n && value[p] == '=');
            const entity_pair *e = find_entity(name);
            if (e != nullptr && !followed_by_eq) {
                out += e->value;
                i = p;
                if (i < n && value[i] == ';') ++i;
                continue;
            }
        }
        out.push_back('&');
        ++i;
    }
}

struct tokenizer {
    kimix::string_view html;
    size_t pos = 0;
    html_dom &dom;
    kimix::vector<uint32_t> stack;
    kimix::string pending;
    bool failed = false;
    tool_status fail_status = tool_status::ok;

    explicit tokenizer(html_dom &d) : dom(d) {}

    bool budget_ok() {
        if (stack.size() > k_max_dom_depth) {
            fail_status = tool_status::unsupported;
            failed = true;
            return false;
        }
        if (dom.nodes.size() >= k_max_dom_nodes) {
            fail_status = tool_status::unsupported;
            failed = true;
            return false;
        }
        return true;
    }

    uint32_t current_top() const { return stack.back(); }

    // bs4 endData: whitespace-only data segments collapse to " " or "\n".
    void flush_text() {
        if (pending.empty()) return;
        kimix::string final_text;
        if (all_ascii_ws(pending)) {
            final_text = pending.find('\n') != kimix::string::npos ? "\n" : " ";
        } else {
            final_text = std::move(pending);
        }
        if (!final_text.empty()) {
            uint32_t parent = current_top();
            uint32_t node = make_node(dom, node_kind::text);
            if (node != k_invalid_node) {
                dom.nodes[node].text = std::move(final_text);
                append_child(dom, parent, node);
            }
        }
        pending.clear();
    }

    void push_element(kimix::string tag_name,
                      kimix::vector<named_value> attrs) {
        uint32_t parent = current_top();
        uint32_t node = make_node(dom, node_kind::element);
        if (node == k_invalid_node) {
            failed = true;
            fail_status = tool_status::unsupported;
            return;
        }
        dom.nodes[node].tag_name = std::move(tag_name);
        dom.nodes[node].attrs = std::move(attrs);
        append_child(dom, parent, node);
        if (dom.nodes[node].parent == k_invalid_node) return;
        stack.push_back(node);
        budget_ok();
    }

    void pop_to(kimix::string_view name) {
        for (size_t i = stack.size(); i-- > 1;) {
            const dom_node &nd = dom.nodes[stack[i]];
            if (nd.kind == node_kind::element && nd.tag_name == name) {
                stack.resize(i); // pop everything up to and incl. the match
                return;
            }
        }
        // No matching open tag -> nothing popped.
    }

    void handle_starttag(kimix::string tag, kimix::vector<named_value> attrs,
                         bool self_closing) {
        flush_text();
        bool void_tag = is_void_tag(tag);
        if (void_tag || self_closing) {
            uint32_t parent = current_top();
            uint32_t node = make_node(dom, node_kind::element);
            if (node == k_invalid_node) {
                failed = true;
                fail_status = tool_status::unsupported;
                return;
            }
            dom.nodes[node].tag_name = std::move(tag);
            dom.nodes[node].attrs = std::move(attrs);
            append_child(dom, parent, node);
            return;
        }
        push_element(std::move(tag), std::move(attrs));
    }

    // RAWTEXT (script/style/...) or RCDATA (textarea/title) content scan.
    // Returns the position of the matching end tag's '<' (the caller parses
    // the end tag from there), or npos when no end tag exists (content runs
    // to EOF).
    size_t scan_cdata_end(kimix::string_view tag, bool rcdata) {
        kimix::string close_marker("</");
        close_marker += tag;
        size_t i = pos;
        while (i < html.size()) {
            size_t j = html.find(close_marker, i);
            if (j == kimix::string_view::npos) {
                // Content to EOF; no closing tag.
                decode_text_entities(html, pos, html.size(), html.size(),
                                     rcdata, pending);
                return kimix::string_view::npos;
            }
            // Must be followed by whitespace, '/' or '>'.
            size_t after = j + close_marker.size();
            if (after >= html.size()) {
                // "end of buffer" -- html.parser treats </script at EOF as an
                // end tag (end=1).
                decode_text_entities(html, pos, j, html.size(), rcdata, pending);
                pos = j;
                return j; // caller parses the end tag at EOF
            }
            char nxt = html[after];
            if (nxt == ' ' || nxt == '\t' || nxt == '\n' || nxt == '\r' ||
                nxt == '\f' || nxt == '/' || nxt == '>') {
                decode_text_entities(html, pos, j, html.size(), rcdata, pending);
                if (pos != j) {
                    return kimix::string_view::npos; // entity swallowed the rest
                }
                pos = j;
                return j;
            }
            i = after;
        }
        return kimix::string_view::npos;
    }
};

kimix::string_view trim_ws_view(kimix::string_view s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' ||
                     s[b] == '\r' || s[b] == '\f')) {
        ++b;
    }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' ||
                     s[e - 1] == '\r' || s[e - 1] == '\f')) {
        --e;
    }
    return s.substr(b, e - b);
}

} // namespace

tool_error parse_html(kimix::string_view html, html_dom &out_dom) {
    tool_error err;
    out_dom.nodes.clear();
    out_dom.root = k_invalid_node;
    uint32_t root = make_node(out_dom, node_kind::root);
    if (root == k_invalid_node) {
        err.status = tool_status::unsupported;
        err.message = "fetch_url: HTML document exceeds the node budget";
        return err;
    }
    out_dom.nodes[root].tag_name = "[document]";
    out_dom.root = root;

    tokenizer tz(out_dom);
    tz.html = html;
    tz.stack.push_back(root);

    const size_t n = html.size();
    while (tz.pos < n && !tz.failed) {
        // Find the next '<' or '&'.
        size_t amp = html.find('&', tz.pos);
        size_t lt = html.find('<', tz.pos);
        size_t next = kimix::string_view::npos;
        if (amp != kimix::string_view::npos && lt != kimix::string_view::npos) {
            next = amp < lt ? amp : lt;
        } else if (amp != kimix::string_view::npos) {
            next = amp;
        } else {
            next = lt;
        }
        if (next == kimix::string_view::npos) {
            decode_text_entities(html, tz.pos, n, n, true, tz.pending);
            tz.pos = n;
            break;
        }
        if (next > tz.pos) {
            decode_text_entities(html, tz.pos, next, n, true, tz.pending);
        }
        const size_t i = tz.pos;
        if (html[i] == '&') {
            // Decode from i through the next '<' (entities cannot span a tag
            // boundary because '<' terminates them in html.parser). An
            // incomplete charref may swallow the whole remaining buffer.
            size_t j = html.find('<', i + 1);
            size_t seg_end = (j == kimix::string_view::npos) ? n : j;
            decode_text_entities(html, tz.pos, seg_end, n, true, tz.pending);
            if (tz.pos >= n) break;
            continue;
        }
        // '<' handling.
        if (html.compare(i, 4, "<!--") == 0) {
            tz.flush_text();
            size_t close = html.find("-->", i + 4);
            if (close == kimix::string_view::npos) {
                close = html.find("--!>", i + 4);
            }
            if (close == kimix::string_view::npos) {
                close = n;
            }
            uint32_t node = make_node(out_dom, node_kind::comment);
            if (node != k_invalid_node) {
                dom_node &nd = out_dom.nodes[node];
                nd.text.assign(html.substr(i + 4, close - (i + 4)));
                append_child(out_dom, tz.current_top(), node);
            }
            tz.pos = (close == n) ? n : close + 3;
            continue;
        }
        if (html.compare(i, 2, "<?") == 0) {
            tz.flush_text();
            size_t close = html.find('>', i + 2);
            if (close == kimix::string_view::npos) close = n;
            uint32_t node = make_node(out_dom, node_kind::comment);
            if (node != k_invalid_node) {
                out_dom.nodes[node].text.assign(
                    html.substr(i + 2, close - (i + 2)));
                append_child(out_dom, tz.current_top(), node);
            }
            tz.pos = (close == n) ? n : close + 1;
            continue;
        }
        if (html.compare(i, 9, "<![CDATA[") == 0) {
            tz.flush_text();
            size_t close = html.find("]]>", i + 9);
            if (close == kimix::string_view::npos) close = n;
            uint32_t node = make_node(out_dom, node_kind::text);
            if (node != k_invalid_node) {
                out_dom.nodes[node].text.assign(
                    html.substr(i + 9, close - (i + 9)));
                append_child(out_dom, tz.current_top(), node);
            }
            tz.pos = (close == n) ? n : close + 3;
            continue;
        }
        if (tz.pos + 9 <= n && ascii_iequals(html.substr(i, 9), "<!doctype")) {
            tz.flush_text();
            size_t close = html.find('>', i + 9);
            if (close == kimix::string_view::npos) close = n;
            kimix::string decl(html.substr(i + 2, close - (i + 2)));
            // bs4 handle_decl strips the literal "DOCTYPE " prefix.
            if (decl.rfind("DOCTYPE ", 0) == 0) {
                decl.erase(0, 8);
            } else if (decl.rfind("doctype ", 0) == 0) {
                decl.erase(0, 8);
            }
            uint32_t node = make_node(out_dom, node_kind::doctype);
            if (node != k_invalid_node) {
                out_dom.nodes[node].text = std::move(decl);
                append_child(out_dom, tz.current_top(), node);
            }
            tz.pos = (close == n) ? n : close + 1;
            continue;
        }
        if (html.compare(i, 2, "<!") == 0) {
            // Bogus declaration -> comment (html.parser parse_bogus_comment).
            tz.flush_text();
            size_t close = html.find('>', i + 2);
            if (close == kimix::string_view::npos) close = n;
            uint32_t node = make_node(out_dom, node_kind::comment);
            if (node != k_invalid_node) {
                out_dom.nodes[node].text.assign(
                    html.substr(i + 2, close - (i + 2)));
                append_child(out_dom, tz.current_top(), node);
            }
            tz.pos = (close == n) ? n : close + 1;
            continue;
        }
        if (html.compare(i, 2, "</") == 0 && i + 2 < n && ascii_alpha(html[i + 2])) {
            tz.flush_text();
            // Parse end tag name: </\s*([a-zA-Z][-.a-zA-Z0-9:_]*)\s*>
            size_t p = i + 2;
            while (p < n && (html[p] == ' ' || html[p] == '\t' || html[p] == '\n' ||
                             html[p] == '\r' || html[p] == '\f')) {
                ++p;
            }
            size_t name_start = p;
            if (p < n && ascii_alpha(html[p])) {
                ++p;
                while (p < n) {
                    char c = html[p];
                    if (ascii_alnum(c) || c == '-' || c == '.' || c == ':' ||
                        c == '_') {
                        ++p;
                    } else {
                        break;
                    }
                }
            }
            kimix::string name;
            ascii_lower_into(html.substr(name_start, p - name_start), name);
            while (p < n && (html[p] == ' ' || html[p] == '\t' || html[p] == '\n' ||
                             html[p] == '\r' || html[p] == '\f')) {
                ++p;
            }
            if (p < n && html[p] == '>') {
                tz.pop_to(name);
                tz.pos = p + 1;
            } else {
                // Malformed end tag: treat as literal text "<" and continue.
                tz.pending.push_back('<');
                tz.pos = i + 1;
            }
            continue;
        }
        if (ascii_alpha(html[i + (i + 1 < n ? 1 : 0)]) && i + 1 < n) {
            // Start tag.
            tz.flush_text();
            size_t p = i + 1;
            size_t name_start = p;
            while (p < n) {
                char c = html[p];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
                    c == '/' || c == '>') {
                    break;
                }
                ++p;
            }
            kimix::string tag;
            ascii_lower_into(html.substr(name_start, p - name_start), tag);
            kimix::vector<named_value> attrs;
            bool self_closing = false;
            bool ok = true;
            // Attribute scan.
            while (ok) {
                while (p < n && (html[p] == ' ' || html[p] == '\t' ||
                                 html[p] == '\n' || html[p] == '\r' ||
                                 html[p] == '\f')) {
                    ++p;
                }
                if (p >= n) {
                    ok = false; // incomplete tag at EOF
                    break;
                }
                char c = html[p];
                if (c == '>') {
                    ++p;
                    break;
                }
                if (c == '/' && p + 1 < n && html[p + 1] == '>') {
                    self_closing = true;
                    p += 2;
                    break;
                }
                if (c == '/') {
                    ++p; // '/' separator not followed by '>' is ignored
                    continue;
                }
                // Attribute name.
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
                    c == '/' || c == '>' || c == '=') {
                    ok = false;
                    break;
                }
                size_t attr_start = p;
                ++p;
                while (p < n) {
                    char d = html[p];
                    if (d == ' ' || d == '\t' || d == '\n' || d == '\r' ||
                        d == '\f' || d == '/' || d == '=' || d == '>') {
                        break;
                    }
                    ++p;
                }
                kimix::string attr_name;
                ascii_lower_into(html.substr(attr_start, p - attr_start),
                                 attr_name);
                while (p < n && (html[p] == ' ' || html[p] == '\t' ||
                                 html[p] == '\n' || html[p] == '\r' ||
                                 html[p] == '\f')) {
                    ++p;
                }
                kimix::string attr_value;
                if (p < n && html[p] == '=') {
                    ++p;
                    while (p < n && (html[p] == ' ' || html[p] == '\t' ||
                                     html[p] == '\n' || html[p] == '\r' ||
                                     html[p] == '\f')) {
                        ++p;
                    }
                    if (p < n && html[p] == '\'') {
                        ++p;
                        size_t vs = p;
                        while (p < n && html[p] != '\'') ++p;
                        decode_attr_entities(html.substr(vs, p - vs), attr_value);
                        if (p < n) ++p;
                    } else if (p < n && html[p] == '"') {
                        ++p;
                        size_t vs = p;
                        while (p < n && html[p] != '"') ++p;
                        decode_attr_entities(html.substr(vs, p - vs), attr_value);
                        if (p < n) ++p;
                    } else {
                        size_t vs = p;
                        while (p < n && html[p] != ' ' && html[p] != '\t' &&
                               html[p] != '\n' && html[p] != '\r' &&
                               html[p] != '\f' && html[p] != '>') {
                            ++p;
                        }
                        decode_attr_entities(html.substr(vs, p - vs), attr_value);
                    }
                }
                // Duplicate attributes: later value wins (bs4 REPLACE).
                bool replaced = false;
                for (named_value &av : attrs) {
                    if (av.name == attr_name) {
                        av.value = attr_value;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) {
                    attrs.push_back(named_value{std::move(attr_name),
                                                std::move(attr_value)});
                }
            }
            if (!ok && p >= n) {
                // Incomplete start tag at EOF: html.parser treats the "<"
                // as data in this edge case; keep it simple and faithful for
                // well-formed documents.
                tz.pending.push_back('<');
                tz.pos = i + 1;
                continue;
            }
            // RAWTEXT / RCDATA content scan. `p` is just past the tag's '>';
            // the content scan must start there, not at the '<'.
            if (is_rawtext_tag(tag) || is_rcdata_tag(tag)) {
                bool rcdata = is_rcdata_tag(tag);
                tz.pos = p;
                uint32_t parent = tz.current_top();
                uint32_t node = make_node(out_dom, node_kind::element);
                if (node == k_invalid_node) {
                    tz.failed = true;
                    tz.fail_status = tool_status::unsupported;
                    continue;
                }
                out_dom.nodes[node].tag_name = tag;
                out_dom.nodes[node].attrs = std::move(attrs);
                append_child(out_dom, parent, node);
                // Push so flush_text() targets the RAWTEXT/RCDATA element.
                tz.stack.push_back(node);
                size_t after = tz.scan_cdata_end(tag, rcdata);
                if (after == kimix::string_view::npos) {
                    // No closing tag; content consumed to EOF.
                    tz.flush_text();
                    tz.stack.pop_back();
                    tz.pos = n;
                    break;
                }
                tz.flush_text();
                tz.stack.pop_back();
                // tz.pos is at the end tag; parse it (</tag ...>).
                size_t p2 = tz.pos;
                while (p2 < n && (html[p2] == ' ' || html[p2] == '\t' ||
                                  html[p2] == '\n' || html[p2] == '\r' ||
                                  html[p2] == '\f')) {
                    ++p2;
                }
                while (p2 < n && ascii_alnum(html[p2])) ++p2;
                while (p2 < n && (html[p2] == ' ' || html[p2] == '\t' ||
                                  html[p2] == '\n' || html[p2] == '\r' ||
                                  html[p2] == '\f')) {
                    ++p2;
                }
                if (p2 < n && html[p2] == '>') {
                    tz.pos = p2 + 1;
                } else {
                    tz.pos = p2;
                }
                continue;
            }
            tz.handle_starttag(std::move(tag), std::move(attrs), self_closing);
            tz.pos = p;
            continue;
        }
        // "<" followed by something else -> literal text.
        tz.pending.push_back('<');
        tz.pos = i + 1;
    }

    tz.flush_text();
    if (tz.failed) {
        out_dom.nodes.clear();
        out_dom.root = k_invalid_node;
        err.status = tz.fail_status;
        err.message = "fetch_url: HTML document exceeds the parser budget "
                      "(depth/node count)";
        return err;
    }
    return err;
}

// ---------------------------------------------------------------------------
// DOM navigation
// ---------------------------------------------------------------------------

namespace {

uint32_t previous_sibling(const html_dom &dom, uint32_t node) {
    const dom_node *nd = dom.at(node);
    if (nd == nullptr || nd->parent == k_invalid_node) return k_invalid_node;
    const dom_node &parent = dom.nodes[nd->parent];
    for (size_t i = 0; i < parent.children.size(); ++i) {
        if (parent.children[i] == node) {
            return i > 0 ? parent.children[i - 1] : k_invalid_node;
        }
    }
    return k_invalid_node;
}

uint32_t next_sibling(const html_dom &dom, uint32_t node) {
    const dom_node *nd = dom.at(node);
    if (nd == nullptr || nd->parent == k_invalid_node) return k_invalid_node;
    const dom_node &parent = dom.nodes[nd->parent];
    for (size_t i = 0; i < parent.children.size(); ++i) {
        if (parent.children[i] == node) {
            return i + 1 < parent.children.size() ? parent.children[i + 1]
                                                  : k_invalid_node;
        }
    }
    return k_invalid_node;
}

uint32_t parent_of(const html_dom &dom, uint32_t node) {
    const dom_node *nd = dom.at(node);
    return nd ? nd->parent : k_invalid_node;
}

bool has_ancestor(const html_dom &dom, uint32_t node,
                  kimix::string_view tag) {
    uint32_t cur = parent_of(dom, node);
    while (cur != k_invalid_node) {
        const dom_node *nd = dom.at(cur);
        if (nd == nullptr) return false;
        if (nd->kind == node_kind::element && nd->tag_name == tag) return true;
        cur = nd->parent;
    }
    return false;
}

// First previous sibling that is an element (bs4 no-arg find_previous_sibling
// returns Tags only).
uint32_t find_previous_sibling_tag(const html_dom &dom, uint32_t node) {
    uint32_t cur = node;
    while ((cur = previous_sibling(dom, cur)) != k_invalid_node) {
        const dom_node *nd = dom.at(cur);
        if (nd != nullptr && nd->kind == node_kind::element) return cur;
    }
    return k_invalid_node;
}

void collect_descendants(const html_dom &dom, uint32_t node,
                         kimix::vector<uint32_t> &out) {
    const dom_node *nd = dom.at(node);
    if (nd == nullptr) return;
    for (uint32_t c : nd->children) {
        out.push_back(c);
        collect_descendants(dom, c, out);
    }
}

void collect_descendants_tagged(const html_dom &dom, uint32_t node,
                                kimix::span<const kimix::string_view> names,
                                kimix::vector<uint32_t> &out) {
    const dom_node *nd = dom.at(node);
    if (nd == nullptr) return;
    for (uint32_t c : nd->children) {
        const dom_node *cn = dom.at(c);
        if (cn != nullptr && cn->kind == node_kind::element &&
            in_tag_list(cn->tag_name, names)) {
            out.push_back(c);
        }
        collect_descendants_tagged(dom, c, names, out);
    }
}

size_t count_prev_li_siblings(const html_dom &dom, uint32_t node) {
    size_t count = 0;
    uint32_t cur = node;
    while ((cur = previous_sibling(dom, cur)) != k_invalid_node) {
        const dom_node *nd = dom.at(cur);
        if (nd != nullptr && nd->kind == node_kind::element &&
            nd->tag_name == "li") {
            ++count;
        }
    }
    return count;
}

} // namespace

void decompose(html_dom &dom, kimix::span<const kimix::string_view> tag_names) {
    for (dom_node &nd : dom.nodes) {
        if (nd.kind == node_kind::element && in_tag_list(nd.tag_name, tag_names)) {
            nd.removed = true;
        }
    }
    for (dom_node &nd : dom.nodes) {
        if (nd.children.empty()) continue;
        kimix::vector<uint32_t> keep;
        keep.reserve(nd.children.size());
        for (uint32_t c : nd.children) {
            if (c < dom.nodes.size() && !dom.nodes[c].removed) keep.push_back(c);
        }
        nd.children = std::move(keep);
    }
}

uint32_t find_tag(const html_dom &dom, uint32_t root, kimix::string_view tag) {
    const dom_node *nd = dom.at(root);
    if (nd == nullptr) return k_invalid_node;
    for (uint32_t c : nd->children) {
        const dom_node *cn = dom.at(c);
        if (cn != nullptr && cn->kind == node_kind::element &&
            cn->tag_name == tag) {
            return c;
        }
        uint32_t deeper = find_tag(dom, c, tag);
        if (deeper != k_invalid_node) return deeper;
    }
    return k_invalid_node;
}

uint32_t find_attr(const html_dom &dom, uint32_t root,
                   kimix::string_view attr_name, kimix::string_view attr_value) {
    const dom_node *nd = dom.at(root);
    if (nd == nullptr) return k_invalid_node;
    for (uint32_t c : nd->children) {
        const dom_node *cn = dom.at(c);
        if (cn != nullptr && cn->kind == node_kind::element) {
            for (const named_value &av : cn->attrs) {
                if (av.name == attr_name && av.value == attr_value) return c;
            }
        }
        uint32_t deeper = find_attr(dom, c, attr_name, attr_value);
        if (deeper != k_invalid_node) return deeper;
    }
    return k_invalid_node;
}

size_t text_len_stripped(const html_dom &dom, uint32_t node) {
    const dom_node *nd = dom.at(node);
    if (nd == nullptr) return 0;
    size_t total = 0;
    for (uint32_t c : nd->children) {
        const dom_node *cn = dom.at(c);
        if (cn == nullptr) continue;
        if (cn->kind == node_kind::text) {
            kimix::string stripped;
            strip_python_ws(cn->text, true, true, stripped);
            total += utf8_code_point_count(stripped);
        } else if (cn->kind == node_kind::element) {
            total += text_len_stripped(dom, c);
        }
    }
    return total;
}

// ---------------------------------------------------------------------------
// Serialization (bs4 minimal formatter)
// ---------------------------------------------------------------------------

namespace {

bool is_heading_tag(kimix::string_view t) {
    return t.size() == 2 && t[0] == 'h' && t[1] >= '1' && t[1] <= '6';
}

void escape_minimal(kimix::string_view s, kimix::string &out) {
    for (char c : s) {
        if (c == '&') {
            out += "&amp;";
        } else if (c == '<') {
            out += "&lt;";
        } else if (c == '>') {
            out += "&gt;";
        } else {
            out.push_back(c);
        }
    }
}

void serialize_attr_value(kimix::string_view value, kimix::string &out) {
    bool has_dq = value.find('"') != kimix::string_view::npos;
    bool has_sq = value.find('\'') != kimix::string_view::npos;
    if (has_dq && !has_sq) {
        out.push_back('\'');
        for (char c : value) {
            if (c == '&') out += "&amp;";
            else out.push_back(c);
        }
        out.push_back('\'');
        return;
    }
    out.push_back('"');
    for (char c : value) {
        if (c == '&') out += "&amp;";
        else if (c == '"') out += "&quot;";
        else out.push_back(c);
    }
    out.push_back('"');
}

} // namespace

kimix::string serialize_node(const html_dom &dom, uint32_t node) {
    kimix::string out;
    const dom_node *nd = dom.at(node);
    if (nd == nullptr) return out;
    switch (nd->kind) {
    case node_kind::text: {
        // Text inside script/style is raw (Script/Stylesheet classes).
        uint32_t par = nd->parent;
        bool raw = false;
        if (par != k_invalid_node) {
            const dom_node *pn = dom.at(par);
            if (pn != nullptr && pn->kind == node_kind::element &&
                (pn->tag_name == "script" || pn->tag_name == "style")) {
                raw = true;
            }
        }
        if (raw) {
            out += nd->text;
        } else {
            escape_minimal(nd->text, out);
        }
        return out;
    }
    case node_kind::comment:
        out += "<!--";
        out += nd->text;
        out += "-->";
        return out;
    case node_kind::doctype:
        out += "<!DOCTYPE ";
        out += nd->text;
        out += ">\n";
        return out;
    case node_kind::root:
        for (uint32_t c : nd->children) out += serialize_node(dom, c);
        return out;
    case node_kind::element: {
        out.push_back('<');
        out += nd->tag_name;
        // Attributes sorted by name (bs4 html.parser serializes sorted).
        kimix::vector<size_t> order;
        order.reserve(nd->attrs.size());
        for (size_t i = 0; i < nd->attrs.size(); ++i) order.push_back(i);
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return nd->attrs[a].name < nd->attrs[b].name;
        });
        for (size_t idx : order) {
            const named_value &av = nd->attrs[idx];
            out.push_back(' ');
            out += av.name;
            out.push_back('=');
            serialize_attr_value(av.value, out);
        }
        bool void_tag = is_void_tag(nd->tag_name);
        if (void_tag) {
            out += "/>";
            return out;
        }
        out.push_back('>');
        for (uint32_t c : nd->children) out += serialize_node(dom, c);
        out += "</";
        out += nd->tag_name;
        out.push_back('>');
        return out;
    }
    }
    return out;
}

// ---------------------------------------------------------------------------
// markdownify (ATX) port
// ---------------------------------------------------------------------------

namespace {

struct tag_set {
    kimix::vector<kimix::string> items;

    bool contains(kimix::string_view t) const {
        for (const kimix::string &s : items) {
            if (s == t) return true;
        }
        return false;
    }
    void add(kimix::string_view t) { items.emplace_back(t); }
};

bool is_block_tag(kimix::string_view t) {
    static const char *kBlock[] = {
        "p",       "blockquote", "article", "div",   "section",
        "ol",      "ul",         "li",      "dl",    "dt",
        "dd",      "table",      "thead",   "tbody", "tfoot",
        "tr",      "td",         "th",
    };
    for (const char *b : kBlock) {
        if (t == b) return true;
    }
    return false;
}

struct markdown_converter {
    const html_dom &dom;
    kimix::string_view bullets = "*+-";

    bool should_remove_whitespace_inside(uint32_t node) const {
        const dom_node *nd = dom.at(node);
        if (nd == nullptr || nd->kind != node_kind::element) return false;
        if (is_heading_tag(nd->tag_name)) return true;
        return is_block_tag(nd->tag_name);
    }

    bool should_remove_whitespace_outside(uint32_t node) const {
        const dom_node *nd = dom.at(node);
        if (nd == nullptr) return false;
        if (should_remove_whitespace_inside(node)) return true;
        return nd->kind == node_kind::element && nd->tag_name == "pre";
    }

    bool is_block_content_element(uint32_t node) const {
        const dom_node *nd = dom.at(node);
        if (nd == nullptr) return false;
        if (nd->kind == node_kind::element) return true;
        if (nd->kind == node_kind::comment || nd->kind == node_kind::doctype) {
            return false;
        }
        if (nd->kind == node_kind::text) return has_non_ws(nd->text);
        return false;
    }

    uint32_t prev_block_content_sibling(uint32_t node) const {
        uint32_t cur = node;
        while ((cur = previous_sibling(dom, cur)) != k_invalid_node) {
            if (is_block_content_element(cur)) return cur;
        }
        return k_invalid_node;
    }

    uint32_t next_block_content_sibling(uint32_t node) const {
        uint32_t cur = node;
        while ((cur = next_sibling(dom, cur)) != k_invalid_node) {
            if (is_block_content_element(cur)) return cur;
        }
        return k_invalid_node;
    }

    // r'^(\n*)((?:.*[^\n])?)(\n*)$' -- leading/content/trailing newline runs.
    void extract_newlines(kimix::string_view s, kimix::string_view &leading,
                          kimix::string_view &content,
                          kimix::string_view &trailing) const {
        size_t lead = 0;
        while (lead < s.size() && s[lead] == '\n') ++lead;
        size_t trail = 0;
        while (trail < s.size() - lead && s[s.size() - 1 - trail] == '\n') {
            ++trail;
        }
        leading = s.substr(0, lead);
        content = s.substr(lead, s.size() - lead - trail);
        trailing = s.substr(s.size() - trail, trail);
    }

    bool can_ignore(uint32_t el, bool remove_inside) const {
        const dom_node *nd = dom.at(el);
        if (nd == nullptr) return true;
        if (nd->kind == node_kind::element) return false;
        if (nd->kind == node_kind::comment || nd->kind == node_kind::doctype) {
            return true;
        }
        if (nd->kind == node_kind::text) {
            if (has_non_ws(nd->text)) return false;
            uint32_t prev = previous_sibling(dom, el);
            uint32_t next = next_sibling(dom, el);
            if (remove_inside && (prev == k_invalid_node || next == k_invalid_node)) {
                return true;
            }
            if (should_remove_whitespace_outside(prev) ||
                should_remove_whitespace_outside(next)) {
                return true;
            }
            return false;
        }
        return true;
    }

    void escape_text(kimix::string_view text, kimix::string &out) const {
        for (char c : text) {
            if (c == '*') {
                out += "\\*";
            } else if (c == '_') {
                out += "\\_";
            } else {
                out.push_back(c);
            }
        }
    }

    // chomp: keep leading/trailing single space, strip the rest.
    void chomp(kimix::string_view text, kimix::string &prefix,
               kimix::string &suffix, kimix::string &core) const {
        prefix.clear();
        suffix.clear();
        if (!text.empty() && text[0] == ' ') prefix.push_back(' ');
        if (!text.empty() && text.back() == ' ') suffix.push_back(' ');
        strip_python_ws(text, true, true, core);
    }

    kimix::string process_text(uint32_t node, const tag_set &parent_tags) const {
        const dom_node *nd = dom.at(node);
        if (nd == nullptr) return kimix::string();
        kimix::string text = nd->text;
        if (!parent_tags.contains("pre")) {
            kimix::string tmp;
            collapse_newline_ws(text, tmp);
            collapse_space_tabs(tmp, text);
        }
        if (!parent_tags.contains("_noformat")) {
            kimix::string tmp;
            escape_text(text, tmp);
            text = std::move(tmp);
        }
        uint32_t prev = previous_sibling(dom, node);
        uint32_t next = next_sibling(dom, node);
        uint32_t par = parent_of(dom, node);
        if (should_remove_whitespace_outside(prev) ||
            (should_remove_whitespace_inside(par) && prev == k_invalid_node)) {
            kimix::string tmp;
            strip_chars(text, " \t\r\n", true, false, tmp);
            text = std::move(tmp);
        }
        if (should_remove_whitespace_outside(next) ||
            (should_remove_whitespace_inside(par) && next == k_invalid_node)) {
            kimix::string tmp;
            strip_python_ws(text, false, true, tmp);
            text = std::move(tmp);
        }
        return text;
    }

    // re_line_with_content indentation: each line (including the final empty
    // line after a trailing newline) is mapped by fn(content).
    template <typename Fn>
    void indent_lines(kimix::string_view text, Fn &&fn, kimix::string &out) const {
        out.clear();
        size_t start = 0;
        bool first = true;
        while (start <= text.size()) {
            size_t nl = text.find('\n', start);
            kimix::string_view line =
                (nl == kimix::string_view::npos)
                    ? text.substr(start)
                    : text.substr(start, nl - start);
            kimix::string mapped;
            fn(line, mapped);
            if (!first) out.push_back('\n');
            out += mapped;
            first = false;
            if (nl == kimix::string_view::npos) break;
            start = nl + 1;
        }
    }

    kimix::string convert_document(uint32_t node, const kimix::string &text,
                                   const tag_set &) const {
        (void)node;
        // strip_document = STRIP -> text.strip('\n')
        size_t b = 0;
        size_t e = text.size();
        while (b < e && text[b] == '\n') ++b;
        while (e > b && text[e - 1] == '\n') --e;
        return text.substr(b, e - b);
    }

    kimix::string abstract_inline(kimix::string_view markup,
                                  const kimix::string &text) const {
        kimix::string prefix, suffix, core;
        chomp(text, prefix, suffix, core);
        if (core.empty()) return kimix::string();
        kimix::string out = prefix;
        out += markup;
        out += core;
        out += markup;
        out += suffix;
        return out;
    }

    kimix::string convert_a(uint32_t node, const kimix::string &text,
                            const tag_set &parent_tags) const {
        if (parent_tags.contains("_noformat")) return text;
        kimix::string prefix, suffix, core;
        chomp(text, prefix, suffix, core);
        if (core.empty()) return kimix::string();
        const dom_node *nd = dom.at(node);
        kimix::string_view href;
        kimix::string_view title;
        for (const named_value &av : nd->attrs) {
            if (av.name == "href") href = av.value;
            else if (av.name == "title") title = av.value;
        }
        // autolinks: text.replace(r'\_', '_') == href and no title.
        kimix::string autolink_cmp;
        for (size_t i = 0; i < core.size();) {
            if (core.compare(i, 2, "\\_") == 0) {
                autolink_cmp.push_back('_');
                i += 2;
            } else {
                autolink_cmp.push_back(core[i]);
                ++i;
            }
        }
        if (autolink_cmp == href && title.empty()) {
            kimix::string out;
            out.push_back('<');
            out += href;
            out.push_back('>');
            return out;
        }
        kimix::string title_part;
        if (!title.empty()) {
            title_part = " \"";
            for (char c : title) {
                if (c == '"') title_part += "\\\"";
                else title_part.push_back(c);
            }
            title_part.push_back('"');
        }
        if (href.empty()) return core;
        kimix::string out = prefix;
        out.push_back('[');
        out += core;
        out.push_back(']');
        out.push_back('(');
        out += href;
        out += title_part;
        out.push_back(')');
        out += suffix;
        return out;
    }

    kimix::string convert_blockquote(const kimix::string &text,
                                     const tag_set &parent_tags) const {
        kimix::string stripped;
        strip_chars(text, " \t\r\n", true, true, stripped);
        if (parent_tags.contains("_inline")) {
            kimix::string out = " ";
            out += stripped;
            out += " ";
            return out;
        }
        if (stripped.empty()) return kimix::string("\n");
        kimix::string indented;
        indent_lines(stripped, [](kimix::string_view line, kimix::string &m) {
            if (line.empty()) {
                m = ">";
            } else {
                m = "> ";
                m += line;
            }
        }, indented);
        kimix::string out = "\n";
        out += indented;
        out += "\n\n";
        return out;
    }

    kimix::string convert_br(uint32_t, const kimix::string &text,
                             const tag_set &parent_tags) const {
        if (parent_tags.contains("_inline")) {
            return text.empty() ? kimix::string(" ") : text + " ";
        }
        // newline_style = SPACES -> "  \n"
        kimix::string out = "  \n";
        out += text;
        return out;
    }

    kimix::string convert_code(const kimix::string &text,
                               const tag_set &parent_tags) const {
        if (parent_tags.contains("_noformat")) return text;
        kimix::string prefix, suffix, core;
        chomp(text, prefix, suffix, core);
        if (core.empty()) return kimix::string();
        size_t max_backticks = 0;
        size_t run = 0;
        for (char c : core) {
            if (c == '`') {
                ++run;
                if (run > max_backticks) max_backticks = run;
            } else {
                run = 0;
            }
        }
        kimix::string delim(max_backticks + 1, '`');
        kimix::string inner = core;
        if (max_backticks > 0) {
            inner = " " + inner + " ";
        }
        kimix::string out = prefix;
        out += delim;
        out += inner;
        out += delim;
        out += suffix;
        return out;
    }

    kimix::string convert_div(const kimix::string &text,
                              const tag_set &parent_tags) const {
        if (parent_tags.contains("_inline")) {
            kimix::string stripped;
            strip_python_ws(text, true, true, stripped);
            kimix::string out = " ";
            out += stripped;
            out += " ";
            return out;
        }
        kimix::string stripped;
        strip_python_ws(text, true, true, stripped);
        if (stripped.empty()) return kimix::string();
        kimix::string out = "\n\n";
        out += stripped;
        out += "\n\n";
        return out;
    }

    kimix::string convert_dd(const kimix::string &text,
                             const tag_set &parent_tags) const {
        kimix::string stripped;
        strip_chars(text, " \t\r\n", true, true, stripped);
        if (parent_tags.contains("_inline")) {
            kimix::string out = " ";
            out += stripped;
            out += " ";
            return out;
        }
        if (stripped.empty()) return kimix::string("\n");
        kimix::string indented;
        indent_lines(stripped, [](kimix::string_view line, kimix::string &m) {
            if (line.empty()) {
                m.clear();
            } else {
                m = " ";
                m += line;
            }
        }, indented);
        // Installed markdownify runtime prefixes ':' to the already-indented
        // first line (": Definition"), unlike the published source which
        // replaces the first indent char.
        kimix::string out = ":";
        out += indented;
        out.push_back('\n');
        return out;
    }

    kimix::string convert_dt(const kimix::string &text,
                             const tag_set &parent_tags) const {
        kimix::string stripped;
        strip_chars(text, " \t\r\n", true, true, stripped);
        kimix::string flat;
        collapse_all_ws_to_space(stripped, flat);
        if (parent_tags.contains("_inline")) {
            kimix::string out = " ";
            out += flat;
            out += " ";
            return out;
        }
        if (flat.empty()) return kimix::string("\n");
        kimix::string out = "\n\n";
        out += flat;
        out.push_back('\n');
        return out;
    }

    kimix::string convert_hN(int n, const kimix::string &text,
                             const tag_set &parent_tags) const {
        if (parent_tags.contains("_inline")) return text;
        n = std::max(1, std::min(6, n));
        kimix::string stripped;
        strip_python_ws(text, true, true, stripped);
        kimix::string flat;
        collapse_all_ws_to_space(stripped, flat);
        kimix::string out = "\n\n";
        out.append(static_cast<size_t>(n), '#');
        out.push_back(' ');
        out += flat;
        out += "\n\n";
        return out;
    }

    kimix::string convert_hr() const { return kimix::string("\n\n---\n\n"); }

    kimix::string convert_img(uint32_t node, const kimix::string &,
                              const tag_set &parent_tags) const {
        const dom_node *nd = dom.at(node);
        kimix::string_view alt, src, title;
        for (const named_value &av : nd->attrs) {
            if (av.name == "alt") alt = av.value;
            else if (av.name == "src") src = av.value;
            else if (av.name == "title") title = av.value;
        }
        kimix::string title_part;
        if (!title.empty()) {
            title_part = " \"";
            for (char c : title) {
                if (c == '"') title_part += "\\\"";
                else title_part.push_back(c);
            }
            title_part.push_back('"');
        }
        uint32_t par = parent_of(dom, node);
        bool inline_parent = parent_tags.contains("_inline");
        bool keep_inline = false;
        if (par != k_invalid_node) {
            const dom_node *pn = dom.at(par);
            if (pn != nullptr && pn->kind == node_kind::element) {
                // keep_inline_images_in is empty by default.
                keep_inline = pn->tag_name.empty() ? false : false;
            }
        }
        if (inline_parent && !keep_inline) return kimix::string(alt);
        kimix::string out = "![";
        out += alt;
        out += "](";
        out += src;
        out += title_part;
        out.push_back(')');
        return out;
    }

    kimix::string convert_video(uint32_t node, const kimix::string &text,
                                const tag_set &parent_tags) const {
        const dom_node *nd = dom.at(node);
        kimix::string_view src, poster;
        for (const named_value &av : nd->attrs) {
            if (av.name == "src") src = av.value;
            else if (av.name == "poster") poster = av.value;
        }
        if (parent_tags.contains("_inline")) return text;
        if (src.empty()) {
            // First <source src="..."> descendant.
            kimix::string_view src_tag("source");
            kimix::vector<uint32_t> sources;
            collect_descendants_tagged(dom, node,
                                       kimix::span<const kimix::string_view>(
                                           &src_tag, 1),
                                       sources);
            if (!sources.empty()) {
                const dom_node *sn = dom.at(sources[0]);
                for (const named_value &av : sn->attrs) {
                    if (av.name == "src") {
                        src = av.value;
                        break;
                    }
                }
            }
        }
        if (!src.empty() && !poster.empty()) {
            kimix::string out = "[![";
            out += text;
            out += "](";
            out += poster;
            out += "](";
            out += src;
            out += ")";
            return out;
        }        if (!src.empty()) {
            kimix::string out = "[";
            out += text;
            out += "](";
            out += src;
            out.push_back(')');
            return out;
        }
        if (!poster.empty()) {
            kimix::string out = "![";
            out += text;
            out += "](";
            out += poster;
            out.push_back(')');
            return out;
        }
        return text;
    }

    kimix::string convert_list(uint32_t node, const kimix::string &text,
                               const tag_set &parent_tags) const {
        uint32_t next = next_block_content_sibling(node);
        bool before_paragraph = false;
        if (next != k_invalid_node) {
            const dom_node *nn = dom.at(next);
            if (nn != nullptr && nn->kind == node_kind::element &&
                nn->tag_name != "ul" && nn->tag_name != "ol") {
                before_paragraph = true;
            }
        }
        if (parent_tags.contains("li")) {
            kimix::string out = "\n";
            size_t end = text.size();
            while (end > 0 && text[end - 1] == '\n') --end;
            out.append(text.substr(0, end));
            return out;
        }
        kimix::string out = "\n\n";
        out += text;
        if (before_paragraph) out.push_back('\n');
        return out;
    }

    kimix::string convert_li(uint32_t node, const kimix::string &text,
                             const tag_set &) const {
        kimix::string stripped;
        strip_chars(text, " \t\r\n", true, true, stripped);
        if (stripped.empty()) return kimix::string("\n");
        const dom_node *nd = dom.at(node);
        uint32_t par = parent_of(dom, node);
        kimix::string bullet;
        if (par != k_invalid_node) {
            const dom_node *pn = dom.at(par);
            if (pn != nullptr && pn->kind == node_kind::element &&
                pn->tag_name == "ol") {
                int start = 1;
                for (const named_value &av : pn->attrs) {
                    if (av.name == "start" && !av.value.empty()) {
                        bool numeric = true;
                        for (char c : av.value) {
                            if (!ascii_digit(c)) {
                                numeric = false;
                                break;
                            }
                        }
                        if (numeric) {
                            start = 0;
                            for (char c : av.value) {
                                start = start * 10 + (c - '0');
                            }
                        }
                        break;
                    }
                }
                size_t prev_li = count_prev_li_siblings(dom, node);
                bullet.append(
                    std::to_string(start + static_cast<int>(prev_li)));
                bullet.push_back('.');
            }
        }
        if (bullet.empty()) {
            int depth = -1;
            uint32_t cur = node;
            while (cur != k_invalid_node) {
                const dom_node *cn = dom.at(cur);
                if (cn != nullptr && cn->kind == node_kind::element &&
                    cn->tag_name == "ul") {
                    ++depth;
                }
                cur = (cn != nullptr) ? cn->parent : k_invalid_node;
            }
            size_t bullets_len = 3; // "*+-"
            char b = bullets[depth >= 0 ? (static_cast<size_t>(depth) % bullets_len)
                                        : 0];
            bullet.push_back(b);
        }
        bullet.push_back(' ');
        size_t bullet_width = bullet.size();
        kimix::string indent(bullet_width, ' ');
        kimix::string indented;
        indent_lines(stripped, [&](kimix::string_view line, kimix::string &m) {
            if (line.empty()) {
                m.clear();
            } else {
                m = indent;
                m += line;
            }
        }, indented);
        // Insert bullet into the first line's indent whitespace.
        kimix::string out = bullet;
        out += indented.substr(bullet_width);
        out.push_back('\n');
        return out;
    }

    kimix::string convert_p(const kimix::string &text,
                            const tag_set &parent_tags) const {
        if (parent_tags.contains("_inline")) {
            kimix::string stripped;
            strip_chars(text, " \t\r\n", true, true, stripped);
            kimix::string out = " ";
            out += stripped;
            out += " ";
            return out;
        }
        kimix::string stripped;
        strip_chars(text, " \t\r\n", true, true, stripped);
        if (stripped.empty()) return kimix::string();
        kimix::string out = "\n\n";
        out += stripped;
        out += "\n\n";
        return out;
    }

    // strip_pre = STRIP: remove leading [ \n]*\n and trailing [ \n]*.
    void strip_pre(kimix::string_view text, kimix::string &out) const {
        size_t b = 0;
        // ^[ \n]*\n : consume a leading run of spaces/newlines up to and
        // including the first newline (if the run contains one).
        size_t first_nl = text.find('\n');
        if (first_nl != kimix::string_view::npos) {
            bool all_ws = true;
            for (size_t i = 0; i < first_nl; ++i) {
                if (text[i] != ' ' && text[i] != '\n') {
                    all_ws = false;
                    break;
                }
            }
            if (all_ws) b = first_nl + 1;
        }
        size_t e = text.size();
        while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\n')) --e;
        out.assign(text.substr(b, e - b));
    }

    kimix::string convert_pre(const kimix::string &text) const {
        if (text.empty()) return kimix::string();
        kimix::string stripped;
        strip_pre(text, stripped);
        kimix::string out = "\n\n```\n";
        out += stripped;
        out += "\n```\n\n";
        return out;
    }

    kimix::string convert_table(const kimix::string &text) const {
        kimix::string stripped;
        strip_python_ws(text, true, true, stripped);
        kimix::string out = "\n\n";
        out += stripped;
        out += "\n\n";
        return out;
    }

    kimix::string convert_caption(const kimix::string &text) const {
        kimix::string stripped;
        strip_python_ws(text, true, true, stripped);
        kimix::string out = stripped;
        out += "\n\n";
        return out;
    }

    kimix::string convert_figcaption(const kimix::string &text) const {
        kimix::string stripped;
        strip_python_ws(text, true, true, stripped);
        kimix::string out = "\n\n";
        out += stripped;
        out += "\n\n";
        return out;
    }

    int cell_colspan(uint32_t node) const {
        const dom_node *nd = dom.at(node);
        for (const named_value &av : nd->attrs) {
            if (av.name == "colspan") {
                bool numeric = !av.value.empty();
                for (char c : av.value) {
                    if (!ascii_digit(c)) {
                        numeric = false;
                        break;
                    }
                }
                if (numeric) {
                    int v = 0;
                    for (char c : av.value) v = v * 10 + (c - '0');
                    return std::max(1, std::min(1000, v));
                }
            }
        }
        return 1;
    }

    kimix::string convert_td_th(uint32_t node, const kimix::string &text) const {
        kimix::string stripped;
        strip_python_ws(text, true, true, stripped);
        kimix::string flat;
        for (char c : stripped) {
            if (c == '\n') flat.push_back(' ');
            else flat.push_back(c);
        }
        int colspan = cell_colspan(node);
        // Python: ' ' + flat + ' |' * colspan  (space before each pipe)
        kimix::string out = " ";
        out += flat;
        for (int i = 0; i < colspan; ++i) out += " |";
        return out;
    }

    kimix::string convert_tr(uint32_t node, const kimix::string &text) const {
        const dom_node *nd = dom.at(node);
        static const kimix::string_view kCells[] = {"td", "th"};
        kimix::vector<uint32_t> cells;
        collect_descendants_tagged(
            dom, node, kimix::span<const kimix::string_view>(kCells, 2), cells);
        bool is_first_row = find_previous_sibling_tag(dom, node) == k_invalid_node;
        bool all_th = !cells.empty();
        for (uint32_t c : cells) {
            const dom_node *cn = dom.at(c);
            if (cn == nullptr || cn->tag_name != "th") {
                all_th = false;
                break;
            }
        }
        uint32_t par = parent_of(dom, node);
        const dom_node *pn = dom.at(par);
        bool is_headrow = all_th;
        if (pn != nullptr && pn->kind == node_kind::element &&
            pn->tag_name == "thead") {
            static const kimix::string_view kTr("tr");
            kimix::vector<uint32_t> trs;
            collect_descendants_tagged(
                dom, par, kimix::span<const kimix::string_view>(&kTr, 1), trs);
            if (trs.size() == 1) is_headrow = true;
        }
        bool is_head_row_missing = false;
        if (is_first_row && pn != nullptr && pn->kind == node_kind::element &&
            pn->tag_name != "tbody") {
            is_head_row_missing = true;
        } else if (is_first_row && pn != nullptr && pn->kind == node_kind::element &&
                   pn->tag_name == "tbody") {
            uint32_t gp = parent_of(dom, par);
            static const kimix::string_view kThead("thead");
            kimix::vector<uint32_t> theads;
            collect_descendants_tagged(
                dom, gp, kimix::span<const kimix::string_view>(&kThead, 1),
                theads);
            if (theads.empty()) is_head_row_missing = true;
        }
        int full_colspan = 0;
        for (uint32_t c : cells) full_colspan += cell_colspan(c);
        kimix::string overline;
        kimix::string underline;
        if ((is_headrow) && is_first_row) {
            underline = "| ";
            for (int i = 0; i < full_colspan; ++i) {
                if (i) underline += " | ";
                underline += "---";
            }
            underline += " |\n";
        } else if (is_head_row_missing || (is_first_row && pn != nullptr &&
                                           pn->kind == node_kind::element &&
                                           pn->tag_name == "table")) {
            overline = "| ";
            for (int i = 0; i < full_colspan; ++i) {
                if (i) overline += " | ";
            }
            overline += " |\n";
            overline += "| ";
            for (int i = 0; i < full_colspan; ++i) {
                if (i) overline += " | ";
                overline += "---";
            }
            overline += " |\n";
        }
        kimix::string out = overline;
        out.push_back('|');
        out += text;
        out.push_back('\n');
        out += underline;
        return out;
    }

    kimix::string convert_tag(uint32_t node, const kimix::string &text,
                              const tag_set &parent_tags) const {
        const dom_node *nd = dom.at(node);
        kimix::string_view name = nd->tag_name;
        if (name == "[document]") return convert_document(node, text, parent_tags);
        if (is_heading_tag(name)) {
            return convert_hN(name[1] - '0', text, parent_tags);
        }
        if (name == "a") return convert_a(node, text, parent_tags);
        if (name == "b" || name == "strong") return abstract_inline("**", text);
        if (name == "blockquote") return convert_blockquote(text, parent_tags);
        if (name == "br") return convert_br(node, text, parent_tags);
        if (name == "code" || name == "kbd" || name == "samp") {
            return convert_code(text, parent_tags);
        }
        if (name == "del" || name == "s") return abstract_inline("~~", text);
        if (name == "div" || name == "article" || name == "section" ||
            name == "dl") {
            return convert_div(text, parent_tags);
        }
        if (name == "dd") return convert_dd(text, parent_tags);
        if (name == "dt") return convert_dt(text, parent_tags);
        if (name == "em" || name == "i") return abstract_inline("*", text);
        if (name == "hr") return convert_hr();
        if (name == "img") return convert_img(node, text, parent_tags);
        if (name == "video") return convert_video(node, text, parent_tags);
        if (name == "ul" || name == "ol") return convert_list(node, text, parent_tags);
        if (name == "li") return convert_li(node, text, parent_tags);
        if (name == "p") return convert_p(text, parent_tags);
        if (name == "pre") return convert_pre(text);
        if (name == "q") {
            kimix::string out = "\"";
            out += text;
            out.push_back('"');
            return out;
        }
        if (name == "script" || name == "style") return kimix::string();
        if (name == "sub" || name == "sup") {
            // sub_symbol / sup_symbol default to ''.
            return abstract_inline("", text);
        }
        if (name == "table") return convert_table(text);
        if (name == "caption") return convert_caption(text);
        if (name == "figcaption") return convert_figcaption(text);
        if (name == "td" || name == "th") return convert_td_th(node, text);
        if (name == "tr") return convert_tr(node, text);
        // Unknown tags: no conversion function -> pass text through.
        return text;
    }

    kimix::string process_tag(uint32_t node, tag_set parent_tags) const {
        const dom_node *nd = dom.at(node);
        bool remove_inside = should_remove_whitespace_inside(node);

        kimix::vector<uint32_t> children;
        for (uint32_t c : nd->children) {
            if (!can_ignore(c, remove_inside)) children.push_back(c);
        }

        tag_set child_tags = parent_tags;
        if (nd->kind == node_kind::element) {
            child_tags.add(nd->tag_name);
            if (is_heading_tag(nd->tag_name) || nd->tag_name == "td" ||
                nd->tag_name == "th") {
                child_tags.add("_inline");
            }
            if (nd->tag_name == "pre" || nd->tag_name == "code" ||
                nd->tag_name == "kbd" || nd->tag_name == "samp") {
                child_tags.add("_noformat");
            }
        }

        kimix::vector<kimix::string> child_strings;
        child_strings.reserve(children.size());
        for (uint32_t c : children) {
            kimix::string s = process_element(c, child_tags);
            if (!s.empty()) child_strings.push_back(std::move(s));
        }

        bool in_pre = (nd->kind == node_kind::element && nd->tag_name == "pre") ||
                      has_ancestor(dom, node, "pre");
        if (!in_pre) {
            kimix::vector<kimix::string> updated;
            updated.push_back(kimix::string());
            for (const kimix::string &child_string : child_strings) {
                kimix::string_view leading, content, trailing;
                extract_newlines(child_string, leading, content, trailing);
                if (!updated.back().empty() && !leading.empty()) {
                    size_t prev_trailing_len = updated.back().size();
                    updated.pop_back();
                    size_t num = std::min<size_t>(
                        2, std::max(prev_trailing_len, leading.size()));
                    leading = kimix::string_view();
                    kimix::string collapsed;
                    collapsed.append(num, '\n');
                    updated.push_back(std::move(collapsed));
                    // Re-append content/trailing of this child below.
                    updated.push_back(kimix::string(content));
                    updated.push_back(kimix::string(trailing));
                } else {
                    updated.push_back(kimix::string(leading));
                    updated.push_back(kimix::string(content));
                    updated.push_back(kimix::string(trailing));
                }
            }
            child_strings = std::move(updated);
        }

        kimix::string text;
        size_t total = 0;
        for (const kimix::string &s : child_strings) total += s.size();
        text.reserve(total);
        for (const kimix::string &s : child_strings) text += s;

        if (nd->kind == node_kind::element) {
            return convert_tag(node, text, parent_tags);
        }
        // root node
        return convert_document(node, text, parent_tags);
    }

    kimix::string process_element(uint32_t node, tag_set parent_tags) const {
        const dom_node *nd = dom.at(node);
        if (nd == nullptr) return kimix::string();
        if (nd->kind == node_kind::text) {
            return process_text(node, parent_tags);
        }
        return process_tag(node, std::move(parent_tags));
    }
};

} // namespace

tool_error markdownify_atx(const html_dom &dom, uint32_t root,
                           kimix::string &out_markdown) {
    tool_error err;
    const dom_node *nd = dom.at(root);
    if (nd == nullptr) {
        err.status = tool_status::invalid_input;
        err.message = "fetch_url: markdownify_atx node index out of range";
        return err;
    }
    markdown_converter conv{dom};
    tag_set tags;
    out_markdown = conv.process_element(root, std::move(tags));
    return err;
}

tool_error html_to_markdown(kimix::string_view html,
                            kimix::string &out_markdown, bool extract) {
    tool_error err;
    html_dom dom;
    err = parse_html(html, dom);
    if (err.failed()) return err;

    uint32_t target = dom.root;
    if (extract) {
        static const kimix::string_view kDecompose[] = {
            "script",  "style",   "noscript", "img",   "video", "audio",
            "source",  "track",   "iframe",   "embed", "object", "canvas",
            "svg",     "picture", "figure",   "nav",   "aside", "footer",
            "header",
        };
        decompose(dom, kimix::span<const kimix::string_view>(kDecompose, 19));

        uint32_t main_node = find_tag(dom, dom.root, "main");
        if (main_node == k_invalid_node) {
            main_node = find_attr(dom, dom.root, "role", "main");
        }
        uint32_t body_node = find_tag(dom, dom.root, "body");
        if (main_node != k_invalid_node &&
            text_len_stripped(dom, main_node) >= 500) {
            target = main_node;
        } else if (body_node != k_invalid_node) {
            target = body_node;
        }
    }

    err = markdownify_atx(dom, target, out_markdown);
    if (err.failed()) return err;

    // re.sub(r"\n{3,}", "\n\n", markdown)
    kimix::string collapsed;
    collapsed.reserve(out_markdown.size());
    size_t i = 0;
    while (i < out_markdown.size()) {
        if (out_markdown[i] == '\n') {
            size_t j = i;
            while (j < out_markdown.size() && out_markdown[j] == '\n') ++j;
            size_t run = j - i;
            collapsed.append(std::min<size_t>(2, run), '\n');
            i = j;
        } else {
            collapsed.push_back(out_markdown[i]);
            ++i;
        }
    }
    // .strip()
    size_t b = 0;
    size_t e = collapsed.size();
    while (b < e) {
        const char *it = collapsed.data() + b;
        uint32_t cp = decode_code_point(it, collapsed.data() + e);
        if (!is_python_space_cp(cp)) break;
        b = static_cast<size_t>(it - collapsed.data());
    }
    while (e > b) {
        size_t p = e - 1;
        while (p > b &&
               (static_cast<unsigned char>(collapsed[p - 1]) & 0xC0) == 0x80) {
            --p;
        }
        const char *it = collapsed.data() + p;
        uint32_t cp = decode_code_point(it, collapsed.data() + e);
        if (!is_python_space_cp(cp)) break;
        e = p;
    }
    out_markdown.assign(collapsed.substr(b, e - b));
    return err;
}

// ---------------------------------------------------------------------------
// Text statistics
// ---------------------------------------------------------------------------

namespace {

struct login_wall_pattern {
    const char *ascii_text; // nullptr for UTF-8-only patterns
    const char *utf8_bytes;
    size_t len;
};

// fetcher._LOGIN_PATTERNS (fixed literals; re.IGNORECASE).
const login_wall_pattern kLoginWallPatterns[] = {
    {nullptr, "\xE7\x99\xBB\xE5\xBD\x95", 6},                    // denglu(login)
    {nullptr, "\xE5\xAF\x86\xE7\xA0\x81\xE7\x99\xBB\xE5\xBD\x95", 12}, // ??denglu(login)
    {nullptr, "\xE9\xAA\x8C\xE8\xAF\x81\xE7\xA0\x81\xE7\x99\xBB\xE5\xBD\x95", 15}, // ???denglu(login)
    {nullptr, "\xE6\xB3\xA8\xE5\x86\x8C", 6},                    // register
    {"sign in", nullptr, 7},
    {"log in", nullptr, 6},
    {"login", nullptr, 5},
    {"verification code", nullptr, 17},
    {nullptr, "\xE7\x9F\xAD\xE4\xBF\xA1\xE9\xAA\x8C\xE8\xAF\x81\xE7\xA0\x81", 15}, // sms-verification-code
};

} // namespace

size_t len_without_ws(kimix::string_view text) {
    size_t count = 0;
    const char *it = text.data();
    const char *end = it + text.size();
    while (it < end) {
        uint32_t cp = decode_code_point(it, end);
        if (cp != 0x20 && cp != 0x0A) ++count;
    }
    return count;
}

bool has_login_wall(kimix::string_view text) {
    // fetcher._LOGIN_PATTERNS with re.IGNORECASE; every alternative is a
    // fixed literal, so substring search is equivalent.
    for (const login_wall_pattern &p : kLoginWallPatterns) {
        if (p.ascii_text != nullptr) {
            // Case-insensitive ASCII substring search.
            if (text.size() < p.len) continue;
            for (size_t i = 0; i + p.len <= text.size(); ++i) {
                size_t k = 0;
                for (; k < p.len; ++k) {
                    if (ascii_lower_char(text[i + k]) != p.ascii_text[k]) break;
                }
                if (k == p.len) return true;
            }
        } else {
            if (text.find(kimix::string_view(p.utf8_bytes, p.len)) !=
                kimix::string_view::npos) {
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// URL normalization (urllib.parse port)
// ---------------------------------------------------------------------------

namespace {

bool is_unreserved(char c) {
    return ascii_alnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

// urllib quote(): percent-encode everything except unreserved + `safe`.
void quote_url(kimix::string_view s, kimix::string_view safe,
               kimix::string &out) {
    static const char kHex[] = "0123456789ABCDEF";
    out.clear();
    out.reserve(s.size());
    const char *it = s.data();
    const char *end = it + s.size();
    while (it < end) {
        unsigned char c = static_cast<unsigned char>(*it);
        if (is_unreserved(static_cast<char>(c)) ||
            safe.find(static_cast<char>(c)) != kimix::string_view::npos) {
            out.push_back(static_cast<char>(c));
            ++it;
        } else if (c < 0x80) {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0xF]);
            ++it;
        } else {
            // Multi-byte UTF-8: encode each byte.
            size_t remaining = static_cast<size_t>(end - it);
            size_t take = 1;
            if ((c & 0xE0) == 0xC0) take = 2;
            else if ((c & 0xF0) == 0xE0) take = 3;
            else if ((c & 0xF8) == 0xF0) take = 4;
            if (take > remaining) take = remaining;
            for (size_t k = 0; k < take; ++k) {
                unsigned char b = static_cast<unsigned char>(it[k]);
                out.push_back('%');
                out.push_back(kHex[b >> 4]);
                out.push_back(kHex[b & 0xF]);
            }
            it += take;
        }
    }
}

// unquote(): replace %XX (uppercase hex) with the byte; invalid escapes kept.
void unquote_url(kimix::string_view s, kimix::string &out) {
    out.clear();
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char a = s[i + 1];
            char b = s[i + 2];
            auto hexval = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int ha = hexval(a);
            int hb = hexval(b);
            if (ha >= 0 && hb >= 0) {
                out.push_back(static_cast<char>((ha << 4) | hb));
                i += 3;
                continue;
            }
        }
        out.push_back(s[i]);
        ++i;
    }
}

// unquote_plus: unquote + '+' -> ' '.
void unquote_plus(kimix::string_view s, kimix::string &out) {
    kimix::string tmp;
    tmp.reserve(s.size());
    for (char c : s) {
        if (c == '+') {
            tmp.push_back(' ');
        } else {
            tmp.push_back(c);
        }
    }
    unquote_url(tmp, out);
}

struct url_split {
    kimix::string scheme;
    kimix::string netloc;
    kimix::string path;
    kimix::string query;
    kimix::string fragment;
    bool valid = false;
};

// Port of urllib.parse.urlsplit (http/https URL subset). On malformed input
// (e.g. unmatched '[' in host) `valid` stays false.
url_split split_url(kimix::string_view url) {
    url_split r;
    // scheme
    size_t i = 0;
    while (i < url.size() && ascii_alpha(url[i])) ++i;
    while (i < url.size() &&
           (ascii_alnum(url[i]) || url[i] == '+' || url[i] == '-' ||
            url[i] == '.')) {
        ++i;
    }
    if (i < url.size() && url[i] == ':' && i > 0) {
        r.scheme.assign(url.substr(0, i));
        ++i;
    } else {
        i = 0;
    }
    // netloc
    if (i + 1 < url.size() && url[i] == '/' && url[i + 1] == '/') {
        i += 2;
        size_t start = i;
        while (i < url.size() && url[i] != '/' && url[i] != '?' && url[i] != '#') {
            ++i;
        }
        r.netloc.assign(url.substr(start, i - start));
    }
    // path
    size_t start = i;
    while (i < url.size() && url[i] != '?' && url[i] != '#') ++i;
    r.path.assign(url.substr(start, i - start));
    // query
    if (i < url.size() && url[i] == '?') {
        ++i;
        start = i;
        while (i < url.size() && url[i] != '#') ++i;
        r.query.assign(url.substr(start, i - start));
    }
    // fragment
    if (i < url.size() && url[i] == '#') {
        ++i;
        r.fragment.assign(url.substr(i));
    }
    r.valid = true;
    return r;
}

// netloc -> (host, port, userinfo, brackets). Handles [v6] literals.
bool parse_netloc_host(kimix::string_view netloc, kimix::string &host,
                       kimix::string_view &port) {
    // Strip userinfo (everything up to the last '@').
    size_t at = netloc.rfind('@');
    kimix::string_view authority =
        (at == kimix::string_view::npos) ? netloc : netloc.substr(at + 1);
    host.clear();
    port = kimix::string_view();
    if (authority.empty()) return false;
    if (authority[0] == '[') {
        size_t close = authority.find(']');
        if (close == kimix::string_view::npos) return false;
        host.assign(authority.substr(1, close - 1));
        if (close + 1 < authority.size() && authority[close + 1] == ':') {
            port = authority.substr(close + 2);
        }
        return true;
    }
    size_t colon = authority.rfind(':');
    if (colon == kimix::string_view::npos) {
        host.assign(authority);
        return true;
    }
    // Check for IPv6 without brackets (multiple colons) -> whole thing host.
    size_t colon2 = authority.find(':', colon + 1);
    if (colon2 != kimix::string_view::npos) {
        host.assign(authority);
        return true;
    }
    host.assign(authority.substr(0, colon));
    port = authority.substr(colon + 1);
    return true;
}

} // namespace

kimix::string normalize_url_for_request(kimix::string_view url) {
    // url.strip()
    kimix::string raw;
    strip_python_ws(url, true, true, raw);
    if (raw.empty()) return raw;

    // re.sub(r"^([A-Za-z][A-Za-z0-9+.-]*://)\s+", r"\1", raw)
    {
        size_t i = 0;
        if (i < raw.size() && ascii_alpha(raw[i])) {
            size_t j = i;
            ++j;
            while (j < raw.size() && (ascii_alnum(raw[j]) || raw[j] == '+' ||
                                      raw[j] == '-' || raw[j] == '.')) {
                ++j;
            }
            if (j + 2 < raw.size() && raw.compare(j, 3, "://") == 0) {
                size_t k = j + 3;
                while (k < raw.size() && (raw[k] == ' ' || raw[k] == '\t')) ++k;
                if (k > j + 3) {
                    kimix::string fixed;
                    fixed.reserve(raw.size());
                    fixed.append(raw.substr(0, j + 3));
                    fixed.append(raw.substr(k));
                    raw = std::move(fixed);
                }
            }
        }
    }

    url_split parsed = split_url(raw);
    if (!parsed.valid) return raw;
    kimix::string scheme_lower;
    ascii_lower_into(parsed.scheme, scheme_lower);
    if (scheme_lower != "http" && scheme_lower != "https") return raw;

    kimix::string netloc = parsed.netloc;
    kimix::string host;
    kimix::string_view port;
    if (parse_netloc_host(netloc, host, port)) {
        kimix::string ascii_host;
        bool ok = idna_encode_host(host, ascii_host);
        if (ok && ascii_host != host) {
            // netloc.replace(host, ascii_host, 1) -- only the first occurrence
            // in the host portion (after userinfo). Simplify: rebuild netloc
            // preserving userinfo and port.
            size_t at = netloc.rfind('@');
            kimix::string rebuilt;
            if (at != kimix::string_view::npos) {
                rebuilt.append(netloc.substr(0, at + 1));
            }
            bool bracketed = !netloc.empty() && netloc[0] == '[';
            // Rebuild using the original authority structure when possible.
            if (host.find(':') != kimix::string_view::npos && !bracketed) {
                // IPv6 without brackets: not handled by Python either; keep.
                rebuilt += netloc.substr(at == kimix::string_view::npos ? 0 : at + 1);
            } else {
                rebuilt += ascii_host;
                if (!port.empty()) {
                    rebuilt.push_back(':');
                    rebuilt.append(port);
                }
            }
            netloc = std::move(rebuilt);
        }
    }

    kimix::string path;
    quote_url(parsed.path, "/%:@!$&'()*+,;=", path);
    kimix::string query;
    quote_url(parsed.query, "/%:@!$&'()*+,;=?", query);
    kimix::string fragment;
    quote_url(parsed.fragment, "/%:@!$&'()*+,;=?", fragment);

    // urlunsplit((scheme, netloc, path, query, fragment))
    kimix::string out;
    out += parsed.scheme;
    out += "://";
    out += netloc;
    out += path;
    if (!query.empty()) {
        out.push_back('?');
        out += query;
    }
    if (!fragment.empty()) {
        out.push_back('#');
        out += fragment;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Sensitive query params + credential prefixes
// ---------------------------------------------------------------------------

namespace {

bool is_sensitive_param(kimix::string_view name) {
    static const char *kSensitive[] = {
        "access_token",    "api_key",       "apikey",          "auth_token",
        "authorization",   "awsaccesskeyid", "client_secret",  "credential",
        "credentials",     "jwt",           "password",        "passwd",
        "secret",          "session_id",    "signature",       "token",
        "x_amz_security_token", "x_amz_signature", "x-amz-security-token",
        "x-amz-signature",
    };
    for (const char *s : kSensitive) {
        if (name == s) return true;
    }
    return false;
}

} // namespace

kimix::optional<kimix::string> sensitive_query_param_name(
    kimix::string_view url) {
    if (url.find('?') == kimix::string_view::npos) return std::nullopt;
    kimix::string stripped;
    strip_python_ws(url, true, true, stripped);
    url_split parsed = split_url(stripped);
    if (!parsed.valid) return std::nullopt;
    kimix::string scheme_lower;
    ascii_lower_into(parsed.scheme, scheme_lower);
    if ((scheme_lower != "http" && scheme_lower != "https") ||
        parsed.query.empty()) {
        return std::nullopt;
    }
    // parse_qsl(parsed.query, keep_blank_values=True) over '&'-separated pairs.
    size_t start = 0;
    while (start <= parsed.query.size()) {
        size_t amp = parsed.query.find('&', start);
        kimix::string_view pair =
            (amp == kimix::string_view::npos)
                ? kimix::string_view(parsed.query).substr(start)
                : kimix::string_view(parsed.query).substr(start, amp - start);
        size_t eq = pair.find('=');
        kimix::string_view key_view = (eq == kimix::string_view::npos) ? pair
                                                                       : pair.substr(0, eq);
        kimix::string_view val_view =
            (eq == kimix::string_view::npos) ? kimix::string_view()
                                             : pair.substr(eq + 1);
        kimix::string key;
        unquote_plus(key_view, key);
        kimix::string val;
        unquote_plus(val_view, val);
        // The sensitivity check lowercases the unquoted key, but the returned
        // name keeps the original (unquoted) case — parse_qsl semantics.
        kimix::string key_lower;
        ascii_lower_into(key, key_lower);
        if (!val.empty() && is_sensitive_param(key_lower)) return key;
        if (amp == kimix::string_view::npos) break;
        start = amp + 1;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Credential prefix patterns (url_contains_secret)
// ---------------------------------------------------------------------------

namespace {

struct prefix_pattern {
    const char *prefix; // literal prefix, must match at current position
    char tail_class;    // 'w' alnum+underscore+dash, 'a' alnum, 'd' digits,
                        // 'h' alnum+dash+underscore+dots
    int min_tail;       // minimum tail length; -1 == exact fixed length
    int exact_tail;     // when min_tail == -1, the exact required length
};

const prefix_pattern kPrefixPatterns[] = {
    {"sk-", 'w', 10, -1},              // OpenAI / OpenRouter / Anthropic
    {"ghp_", 'a', 10, -1},             // GitHub PAT classic
    {"github_pat_", 'w', 10, -1},      // GitHub PAT fine-grained
    {"gho_", 'a', 10, -1},             // GitHub OAuth
    {"ghu_", 'a', 10, -1},             // GitHub user-to-server
    {"ghs_", 'a', 10, -1},             // GitHub server-to-server
    {"ghr_", 'a', 10, -1},             // GitHub refresh
    {"xapp-", 'd', 0, -1},             // xapp-<digits>-<alnum-dash>
    {"xox", 'b', 10, -1},              // xox[baprs]-... (prefix "xox" then tail)
    {"AIza", 'w', 30, -1},             // Google API keys
    {"pplx-", 'a', 10, -1},            // Perplexity
    {"fal_", 'w', 10, -1},             // Fal.ai
    {"fc-", 'a', 10, -1},              // Firecrawl
    {"bb_live_", 'w', 10, -1},         // BrowserBase
    {"gAAAA", 'w', 20, -1},            // Codex encrypted tokens
    {"AKIA", 'd', 0, 16},              // AWS Access Key ID (16 alnum upper)
    {"sk_live_", 'a', 10, -1},         // Stripe secret live
    {"sk_test_", 'a', 10, -1},         // Stripe secret test
    {"rk_live_", 'a', 10, -1},         // Stripe restricted
    {"SG.", 'w', 10, -1},              // SendGrid
    {"hf_", 'a', 10, -1},              // HuggingFace
    {"r8_", 'a', 10, -1},              // Replicate
    {"npm_", 'a', 10, -1},             // npm
    {"pypi-", 'w', 10, -1},            // PyPI
    {"dop_v1_", 'a', 10, -1},          // DigitalOcean PAT
    {"doo_v1_", 'a', 10, -1},          // DigitalOcean OAuth
    {"am_", 'w', 10, -1},              // AgentMail
    {"sk_", 'w', 10, -1},              // ElevenLabs (sk_ underscore)
    {"tvly-", 'a', 10, -1},            // Tavily
    {"exa_", 'a', 10, -1},             // Exa
    {"gsk_", 'a', 10, -1},             // Groq
    {"syt_", 'a', 10, -1},             // Matrix
    {"retaindb_", 'a', 10, -1},        // RetainDB
    {"hsk-", 'a', 10, -1},             // Hindsight
    {"mem0_", 'a', 10, -1},            // Mem0
    {"brv_", 'a', 10, -1},             // ByteRover
    {"xai-", 'a', 30, -1},             // xAI (Grok)
    {"ntn_", 'a', 10, -1},             // Notion
    {"fw-", 'a', 30, -1},              // Fireworks AI
    {"fw_", 'a', 30, -1},              // Fireworks AI
    {"fpk_", 'a', 30, -1},             // Fireworks project
    {"glpat-", 'h', 10, -1},           // GitLab personal access token
    {"gloas-", 'h', 10, -1},           // GitLab OAuth application secret
    {"gldt-", 'h', 10, -1},            // GitLab deploy token
    {"glrt-", 'h', 10, -1},            // GitLab runner authentication token
    {"glrtr-", 'h', 10, -1},           // GitLab runner registration token
    {"glcbt-", 'h', 10, -1},           // GitLab CI/CD job token
    {"glptt-", 'h', 10, -1},           // GitLab pipeline trigger token
    {"glft-", 'h', 10, -1},            // GitLab feed token
    {"glimt-", 'h', 10, -1},           // GitLab incoming mail token
    {"glagent-", 'h', 10, -1},         // GitLab agent (KAS) token
    {"glsoat-", 'h', 10, -1},          // GitLab service-account access token
    {"glffct-", 'h', 10, -1},          // GitLab feature-flags client token
    {"glwt-", 'h', 10, -1},            // GitLab workspace token
    {"GR1348941", 'h', 10, -1},        // GitLab legacy runner registration token
};

bool tail_char_ok(char c, char cls) {
    switch (cls) {
    case 'w': return ascii_alnum(c) || c == '_' || c == '-';
    case 'a': return ascii_alnum(c);
    case 'd': return ascii_digit(c);
    case 'h': return ascii_alnum(c) || c == '_' || c == '-' || c == '.';
    default: return false;
    }
}

// Returns the end offset of the full match (prefix + tail) or npos.
size_t match_prefix_end(kimix::string_view s, size_t pos,
                        const prefix_pattern &p) {
    size_t prefix_len = std::strlen(p.prefix);
    if (pos + prefix_len > s.size()) return kimix::string_view::npos;
    if (s.substr(pos, prefix_len) != p.prefix) return kimix::string_view::npos;
    size_t tail_start = pos + prefix_len;
    size_t tail_len = 0;
    // xapp-\d+-... and AKIA[0-9]{16} need special handling:
    if (kimix::string_view(p.prefix) == "xapp-") {
        // xapp-\d+-[A-Za-z0-9-]{10,}
        size_t q = tail_start;
        while (q < s.size() && ascii_digit(s[q])) ++q;
        if (q == tail_start || q >= s.size() || s[q] != '-') return kimix::string_view::npos;
        ++q;
        tail_start = q;
        tail_len = 0;
        while (q < s.size() && (ascii_alnum(s[q]) || s[q] == '-')) {
            ++tail_len;
            ++q;
        }
        return tail_len >= 10 ? q : kimix::string_view::npos;
    }
    if (kimix::string_view(p.prefix) == "xox") {
        // xox[baprs]-[A-Za-z0-9-]{10,}
        if (tail_start >= s.size()) return kimix::string_view::npos;
        char c = s[tail_start];
        if (c != 'b' && c != 'a' && c != 'p' && c != 'r' && c != 's') return kimix::string_view::npos;
        if (tail_start + 1 >= s.size() || s[tail_start + 1] != '-') return kimix::string_view::npos;
        size_t q = tail_start + 2;
        while (q < s.size() && (ascii_alnum(s[q]) || s[q] == '-')) {
            ++tail_len;
            ++q;
        }
        return tail_len >= 10 ? q : kimix::string_view::npos;
    }
    if (kimix::string_view(p.prefix) == "AKIA") {
        // AKIA[A-Z0-9]{16} -- exact 16 of A-Z0-9
        size_t q = tail_start;
        while (q < s.size() &&
               ((s[q] >= 'A' && s[q] <= 'Z') || ascii_digit(s[q]))) {
            ++tail_len;
            ++q;
        }
        return tail_len == 16 ? q : kimix::string_view::npos;
    }
    while (tail_start + tail_len < s.size() &&
           tail_char_ok(s[tail_start + tail_len], p.tail_class)) {
        ++tail_len;
    }
    if (p.min_tail == -1) {
        return tail_len == static_cast<size_t>(p.exact_tail)
                   ? tail_start + tail_len
                   : kimix::string_view::npos;
    }
    return tail_len >= static_cast<size_t>(p.min_tail) ? tail_start + tail_len
                                                       : kimix::string_view::npos;
}

bool prefix_re_search(kimix::string_view s) {
    for (size_t pos = 0; pos < s.size(); ++pos) {
        // Lookbehind (?<![A-Za-z0-9_-])
        if (pos > 0) {
            char prev = s[pos - 1];
            if (ascii_alnum(prev) || prev == '_' || prev == '-') continue;
        }
        for (const prefix_pattern &p : kPrefixPatterns) {
            size_t end_pos = match_prefix_end(s, pos, p);
            if (end_pos == kimix::string_view::npos) continue;
            // Lookahead (?![A-Za-z0-9_-]) -- after the full match.
            if (end_pos < s.size()) {
                char nxt = s[end_pos];
                if (ascii_alnum(nxt) || nxt == '_' || nxt == '-') continue;
            }
            return true;
        }
    }
    return false;
}

} // namespace

bool url_contains_secret(kimix::string_view url) {
    if (url.empty()) return false;
    if (prefix_re_search(url)) return true;
    kimix::string unquoted;
    unquote_url(url, unquoted);
    if (prefix_re_search(unquoted)) return true;
    kimix::string normalized = normalize_url_for_request(url);
    if (prefix_re_search(normalized)) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Hostname / IP classification (SSRF)
// ---------------------------------------------------------------------------

bool is_blocked_hostname(kimix::string_view hostname) {
    kimix::string h;
    ascii_lower_into(hostname, h);
    while (!h.empty() && h.back() == '.') h.pop_back();
    return h == "metadata.google.internal" || h == "metadata.goog";
}

namespace {

struct ipv4_addr {
    uint32_t value = 0;
    bool valid = false;
};

ipv4_addr parse_ipv4(kimix::string_view s) {
    ipv4_addr r;
    size_t i = 0;
    uint32_t result = 0;
    for (int part = 0; part < 4; ++part) {
        if (part > 0) {
            if (i >= s.size() || s[i] != '.') return r;
            ++i;
        }
        if (i >= s.size() || !ascii_digit(s[i])) return r;
        uint32_t v = 0;
        size_t digits = 0;
        while (i < s.size() && ascii_digit(s[i])) {
            v = v * 10 + static_cast<uint32_t>(s[i] - '0');
            if (v > 255) return r;
            ++digits;
            ++i;
        }
        if (digits == 0 || digits > 3) return r;
        result = (result << 8) | v;
    }
    if (i != s.size()) return r;
    r.value = result;
    r.valid = true;
    return r;
}

// IPv6 parser (pure, no DNS): returns the 16-byte address; supports the
// standard compressed forms plus IPv4-embedded tail (::ffff:1.2.3.4).
struct ipv6_parse_result {
    uint8_t bytes[16];
    bool valid = false;
    bool ipv4_mapped = false;
    uint32_t ipv4_value = 0;
};

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

ipv6_parse_result parse_ipv6(kimix::string_view s) {
    ipv6_parse_result r;
    std::memset(r.bytes, 0, sizeof(r.bytes));
    uint16_t groups[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    size_t n = 0;
    int compress = -1; // group index where '::' occurred
    size_t i = 0;
    bool last_was_colon = false;

    if (s.empty()) return r;
    if (s[0] == ':') {
        if (s.size() < 2 || s[1] != ':') return r;
        compress = 0;
        i = 2;
        last_was_colon = true;
    }
    while (i < s.size()) {
        if (s[i] == ':') {
            if (last_was_colon) {
                if (compress != -1) return r; // multiple '::'
                compress = static_cast<int>(n);
            } else if (n >= 8) {
                return r;
            }
            last_was_colon = true;
            ++i;
            continue;
        }
        // Try IPv4 tail (only allowed in the last group).
        size_t save_i = i;
        size_t save_n = n;
        bool save_last = last_was_colon;
        size_t dots = 0;
        size_t q = i;
        while (q < s.size()) {
            if (ascii_digit(s[q])) {
                ++q;
            } else if (s[q] == '.' && q + 1 < s.size() && ascii_digit(s[q + 1])) {
                ++dots;
                ++q;
            } else {
                break;
            }
        }
        if (dots > 0) {
            ipv4_addr v4 = parse_ipv4(s.substr(i, q - i));
            if (!v4.valid) return r;
            if (n >= 7) return r;
            groups[n++] = static_cast<uint16_t>(v4.value >> 16);
            groups[n++] = static_cast<uint16_t>(v4.value & 0xFFFF);
            r.ipv4_mapped = (n == 2 && groups[0] == 0 && groups[1] == 0xFFFF) ||
                            (compress == 0 && n == 2);
            // More precisely: IPv4-mapped when the first 5 groups are 0 and
            // group 5 is 0xFFFF -- handled below by the caller inspecting bytes.
            i = q;
            last_was_colon = false;
            if (i != s.size()) return r;
            break;
        }
        // Hex group.
        i = save_i;
        n = save_n;
        last_was_colon = save_last;
        if (n >= 8) return r;
        uint32_t v = 0;
        size_t ndig = 0;
        while (i < s.size() && ndig < 4 && hex_val(s[i]) >= 0) {
            v = v * 16 + static_cast<uint32_t>(hex_val(s[i]));
            ++ndig;
            ++i;
        }
        if (ndig == 0) return r;
        if (v > 0xFFFF) return r;
        groups[n++] = static_cast<uint16_t>(v);
        last_was_colon = false;
    }
    if (n == 0 && compress == -1) return r; // "::" alone is valid (n==0)
    if (compress == -1 && n != 8) return r;
    if (compress != -1 && n >= 8) return r;
    // Expand.
    size_t head = (compress == -1) ? n : static_cast<size_t>(compress);
    size_t tail = (compress == -1) ? 0 : n - static_cast<size_t>(compress);
    for (size_t k = 0; k < head; ++k) {
        uint16_t g = groups[k];
        r.bytes[k * 2] = static_cast<uint8_t>(g >> 8);
        r.bytes[k * 2 + 1] = static_cast<uint8_t>(g & 0xFF);
    }
    size_t tail_start = 8 - tail;
    for (size_t k = 0; k < tail; ++k) {
        uint16_t g = groups[head + k];
        r.bytes[(tail_start + k) * 2] = static_cast<uint8_t>(g >> 8);
        r.bytes[(tail_start + k) * 2 + 1] = static_cast<uint8_t>(g & 0xFF);
    }
    r.valid = true;
    return r;
}

bool ipv4_in_network(uint32_t ip, uint32_t net, uint32_t mask) {
    return (ip & mask) == net;
}

} // namespace

addr_class classify_resolved_address(kimix::string_view ip) {
    // Strip a %scope suffix (socket scope IDs).
    size_t pct = ip.find('%');
    if (pct != kimix::string_view::npos) ip = ip.substr(0, pct);

    ipv4_addr v4 = parse_ipv4(ip);
    if (v4.valid) {
        uint32_t a = v4.value;
        if (ipv4_in_network(a, 0x7F000000u, 0xFF000000u)) return addr_class::loopback; // 127/8
        if (ipv4_in_network(a, 0x0A000000u, 0xFF000000u)) return addr_class::private_addr; // 10/8
        if (ipv4_in_network(a, 0xAC100000u, 0xFFF00000u)) return addr_class::private_addr; // 172.16/12
        if (ipv4_in_network(a, 0xC0A80000u, 0xFFFF0000u)) return addr_class::private_addr; // 192.168/16
        if (ipv4_in_network(a, 0xA9FE0000u, 0xFFFF0000u)) return addr_class::link_local; // 169.254/16
        if (ipv4_in_network(a, 0x64400000u, 0xFFC00000u)) return addr_class::cgnat; // 100.64/10
        if (ipv4_in_network(a, 0xE0000000u, 0xF0000000u)) return addr_class::multicast; // 224/4
        if (a == 0) return addr_class::unspecified; // 0.0.0.0
        // ipaddress.is_reserved for IPv4: 240.0.0.0/4 (class E) and 0.0.0.0/8
        if (ipv4_in_network(a, 0xF0000000u, 0xF0000000u)) return addr_class::reserved; // 240/4
        if (ipv4_in_network(a, 0x00000000u, 0xFF000000u)) return addr_class::reserved; // 0/8 (except 0.0.0.0 above)
        return addr_class::public_addr;
    }

    ipv6_parse_result v6 = parse_ipv6(ip);
    if (v6.valid) {
        // IPv4-mapped IPv6: check embedded IPv4 (Python ip.ipv4_mapped).
        bool mapped = true;
        for (int k = 0; k < 10; ++k) {
            if (v6.bytes[k] != 0) {
                mapped = false;
                break;
            }
        }
        if (mapped && v6.bytes[10] == 0xFF && v6.bytes[11] == 0xFF) {
            uint32_t v4mapped = (static_cast<uint32_t>(v6.bytes[12]) << 24) |
                                (static_cast<uint32_t>(v6.bytes[13]) << 16) |
                                (static_cast<uint32_t>(v6.bytes[14]) << 8) |
                                static_cast<uint32_t>(v6.bytes[15]);
            kimix::string v4s;
            v4s += std::to_string((v4mapped >> 24) & 0xFF);
            v4s.push_back('.');
            v4s += std::to_string((v4mapped >> 16) & 0xFF);
            v4s.push_back('.');
            v4s += std::to_string((v4mapped >> 8) & 0xFF);
            v4s.push_back('.');
            v4s += std::to_string(v4mapped & 0xFF);
            addr_class cls = classify_resolved_address(v4s);
            if (cls == addr_class::public_addr) return addr_class::public_addr;
            if (cls == addr_class::invalid) return addr_class::invalid;
            return cls;
        }
        if (v6.bytes[0] == 0xFF) return addr_class::multicast; // ff00::/8
        if (v6.bytes[0] == 0xFE && (v6.bytes[1] & 0xC0) == 0x80) {
            return addr_class::link_local; // fe80::/10
        }
        bool all_zero = true;
        for (int k = 0; k < 16; ++k) {
            if (v6.bytes[k] != 0) {
                all_zero = false;
                break;
            }
        }
        if (all_zero) return addr_class::unspecified; // ::
        if (v6.bytes[0] == 0 && v6.bytes[1] == 0 && v6.bytes[2] == 0 &&
            v6.bytes[3] == 0 && v6.bytes[4] == 0 && v6.bytes[5] == 0 &&
            v6.bytes[6] == 0 && v6.bytes[7] == 0 && v6.bytes[8] == 0 &&
            v6.bytes[9] == 0 && v6.bytes[10] == 0 && v6.bytes[11] == 0 &&
            v6.bytes[12] == 0 && v6.bytes[13] == 0 && v6.bytes[14] == 0 &&
            v6.bytes[15] == 1) {
            return addr_class::loopback; // ::1
        }
        // Unique-local fc00::/7 (Python is_private).
        if ((v6.bytes[0] & 0xFE) == 0xFC) return addr_class::private_addr;
        // ipaddress.is_reserved IPv6: ::/128 handled above, ::1 above,
        // ::ffff:0:0/96 (mapped, handled), 64:ff9b::/96, 100::/64 etc.
        // Python is_reserved for IPv6 covers 100::/64, 2001:db8::/32 (doc).
        if (v6.bytes[0] == 0x20 && v6.bytes[1] == 0x01 && v6.bytes[2] == 0x0D &&
            v6.bytes[3] == 0xB8) {
            return addr_class::reserved; // 2001:db8::/32 documentation
        }
        if (v6.bytes[0] == 0x01 && v6.bytes[1] == 0x00) {
            return addr_class::reserved; // 100::/64 discard-only
        }
        return addr_class::public_addr;
    }
    return addr_class::invalid;
}

bool is_always_blocked_address(kimix::string_view ip) {
    // Metadata endpoints + link-local ranges, always blocked (even with the
    // private-urls override).
    ipv4_addr v4 = parse_ipv4(ip);
    if (v4.valid) {
        if (ipv4_in_network(v4.value, 0xA9FE0000u, 0xFFFF0000u)) return true; // 169.254/16
        if (v4.value == 0x646464C8u) return true; // 100.100.100.200 Alibaba
        return false;
    }
    ipv6_parse_result v6 = parse_ipv6(ip);
    if (!v6.valid) return false;
    // ::ffff:169.254.0.0/112 -- check the embedded IPv4.
    bool mapped = true;
    for (int k = 0; k < 10; ++k) {
        if (v6.bytes[k] != 0) {
            mapped = false;
            break;
        }
    }
    if (mapped && v6.bytes[10] == 0xFF && v6.bytes[11] == 0xFF) {
        uint32_t v4mapped = (static_cast<uint32_t>(v6.bytes[12]) << 24) |
                            (static_cast<uint32_t>(v6.bytes[13]) << 16) |
                            (static_cast<uint32_t>(v6.bytes[14]) << 8) |
                            static_cast<uint32_t>(v6.bytes[15]);
        if (ipv4_in_network(v4mapped, 0xA9FE0000u, 0xFFFF0000u)) return true;
        return false;
    }
    // fd00:ec2::254 (AWS IPv6 metadata) — groups fd00, 0ec2, 0254
    static const uint8_t kAws6[16] = {0xFD, 0x00, 0x0E, 0xC2, 0,    0, 0, 0,
                                      0,    0,    0,    0,    0,    0, 0x02, 0x54};
    // Compare first 4 bytes + the last two bytes.
    if (v6.bytes[0] == kAws6[0] && v6.bytes[1] == kAws6[1] &&
        v6.bytes[2] == kAws6[2] && v6.bytes[3] == kAws6[3] &&
        v6.bytes[14] == kAws6[14] && v6.bytes[15] == kAws6[15]) {
        return true;
    }
    return false;
}

bool is_safe_url_decision(kimix::string_view url, bool allow_all_private,
                          bool proxy_configured,
                          const resolve_outcome &resolved) {
    kimix::string stripped;
    strip_python_ws(url, true, true, stripped);
    url_split parsed = split_url(stripped);
    if (!parsed.valid) return false;
    kimix::string scheme_lower;
    ascii_lower_into(parsed.scheme, scheme_lower);
    if (scheme_lower != "http" && scheme_lower != "https") return false;
    kimix::string host;
    kimix::string_view port;
    if (!parse_netloc_host(parsed.netloc, host, port)) return false;
    if (host.empty()) return false;

    kimix::string host_clean;
    ascii_lower_into(host, host_clean);
    while (!host_clean.empty() && host_clean.back() == '.') host_clean.pop_back();
    if (is_blocked_hostname(host_clean)) return false;

    if (resolved.dns_failed) {
        // Literal IPs need no DNS -- they stay on the fail-closed path.
        if (classify_resolved_address(host) != addr_class::invalid) return false;
        if (proxy_configured) return true;
        return false;
    }
    for (const kimix::string &addr : resolved.addresses) {
        if (is_always_blocked_address(addr)) return false;
        if (!allow_all_private) {
            addr_class cls = classify_resolved_address(addr);
            if (cls == addr_class::private_addr || cls == addr_class::loopback ||
                cls == addr_class::link_local || cls == addr_class::reserved ||
                cls == addr_class::multicast || cls == addr_class::unspecified ||
                cls == addr_class::cgnat) {
                return false;
            }
            if (cls == addr_class::invalid) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// IDNA host encoding (stdlib `idna` codec semantics + RFC-3492 punycode)
// ---------------------------------------------------------------------------

namespace {

// Simple lowercase for code points: ASCII A-Z plus a small set of common
// Latin-1 uppercase letters (matches the builtin idna codec's Nameprep case
// folding for the common cases used in tests).
uint32_t simple_lower_cp(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + ('a' - 'A');
    // Latin-1 uppercase -> lowercase (U+00C0..U+00D6, U+00D8..U+00DE)
    if (cp >= 0xC0 && cp <= 0xD6) return cp + 0x20;
    if (cp >= 0xD8 && cp <= 0xDE) return cp + 0x20;
    return cp;
}

// RFC-3492 bootstring constants.
constexpr uint32_t kPunyBase = 36;
constexpr uint32_t kPunyTMin = 1;
constexpr uint32_t kPunyTMax = 26;
constexpr uint32_t kPunySkew = 38;
constexpr uint32_t kPunyDamp = 700;
constexpr uint32_t kPunyInitialBias = 72;
constexpr uint32_t kPunyInitialN = 128;
constexpr char kPunyDelimiter = '-';

char puny_digit(uint32_t d) {
    if (d < 26) return static_cast<char>('a' + d);
    return static_cast<char>('0' + (d - 26));
}

uint32_t puny_adapt(uint32_t delta, uint32_t numpoints, bool firsttime) {
    delta = firsttime ? delta / kPunyDamp : delta / 2;
    delta += delta / numpoints;
    uint32_t k = 0;
    while (delta > ((kPunyBase - kPunyTMin) * kPunyTMax) / 2) {
        delta /= (kPunyBase - kPunyTMin);
        k += kPunyBase;
    }
    return k + (((kPunyBase - kPunyTMin + 1) * delta) / (delta + kPunySkew));
}

// Encode one label (sequence of code points) with punycode; returns false on
// overflow (label too long / delta overflow) mirroring UnicodeError.
bool punycode_encode(kimix::vector<uint32_t> &cps, kimix::string &out) {
    out.clear();
    size_t b = 0;
    for (uint32_t cp : cps) {
        if (cp < 0x80) ++b;
    }
    size_t h = b;
    // Copy basic code points.
    kimix::string basic;
    for (uint32_t cp : cps) {
        if (cp < 0x80) basic.push_back(static_cast<char>(cp));
    }
    out += basic;
    if (b > 0 && cps.size() > b) out.push_back(kPunyDelimiter);
    if (cps.size() == b) return true; // ASCII label
    uint32_t n = kPunyInitialN;
    uint32_t delta = 0;
    uint32_t bias = kPunyInitialBias;
    while (h < cps.size()) {
        // m = min code point >= n
        uint32_t m = 0xFFFFFFFF;
        for (uint32_t cp : cps) {
            if (cp >= n && cp < m) m = cp;
        }
        if (m == 0xFFFFFFFF) return false;
        uint64_t new_delta = static_cast<uint64_t>(delta) +
                             static_cast<uint64_t>(m - n) * (h + 1);
        if (new_delta > 0xFFFFFFFFu) return false;
        delta = static_cast<uint32_t>(new_delta);
        n = m;
        for (uint32_t cp : cps) {
            if (cp < n) {
                if (delta == 0xFFFFFFFFu) return false;
                ++delta;
            }
            if (cp == n) {
                uint32_t q = delta;
                for (uint32_t k = kPunyBase;; k += kPunyBase) {
                    uint32_t t = k <= bias
                                     ? kPunyTMin
                                     : (k >= bias + kPunyTMax ? kPunyTMax
                                                              : k - bias);
                    if (q < t) break;
                    out.push_back(puny_digit(t + (q - t) % (kPunyBase - t)));
                    q = (q - t) / (kPunyBase - t);
                }
                out.push_back(puny_digit(q));
                bias = puny_adapt(delta, static_cast<uint32_t>(h + 1), h == b);
                delta = 0;
                ++h;
            }
        }
        ++delta;
        ++n;
    }
    return true;
}

} // namespace

bool idna_encode_host(kimix::string_view host, kimix::string &out) {
    out.clear();
    // Split into labels on '.'.
    size_t start = 0;
    bool first_label = true;
    while (start <= host.size()) {
        size_t dot = host.find('.', start);
        kimix::string_view label =
            (dot == kimix::string_view::npos)
                ? host.substr(start)
                : host.substr(start, dot - start);
        if (!first_label) out.push_back('.');
        first_label = false;

        // Decode label code points.
        kimix::vector<uint32_t> cps;
        const char *it = label.data();
        const char *end = it + label.size();
        bool non_ascii = false;
        while (it < end) {
            uint32_t cp = decode_code_point(it, end);
            cps.push_back(cp);
            if (cp >= 0x80) non_ascii = true;
        }

        if (!non_ascii) {
            // ASCII labels pass through unchanged (builtin idna codec keeps
            // case, e.g. "EXAMPLE.com" -> "EXAMPLE.com").
            out.append(label);
            if (dot == kimix::string_view::npos) break;
            start = dot + 1;
            continue;
        }
        // Non-ASCII label: lowercase (simple mapping) + punycode.
        for (uint32_t &cp : cps) cp = simple_lower_cp(cp);
        if (cps.size() > 63) return false; // label length limit (UnicodeError)
        kimix::string encoded;
        if (!punycode_encode(cps, encoded)) return false;
        out += "xn--";
        out += encoded;
        if (dot == kimix::string_view::npos) break;
        start = dot + 1;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Charset decision helper
// ---------------------------------------------------------------------------

namespace {

void ascii_lower_trim(kimix::string_view s, kimix::string &out) {
    kimix::string t;
    strip_chars(s, " \t\r\n\"'", true, true, t);
    ascii_lower_into(t, out);
}

bool known_encoding(kimix::string_view name) {
    static const char *kKnown[] = {
        "utf-8", "utf8", "ascii", "us-ascii", "latin-1", "iso-8859-1",
        "windows-1252", "gbk", "gb2312", "big5", "shift-jis", "euc-jp",
        "euc-kr", "utf-16", "utf-16le", "utf-16be",
    };
    for (const char *k : kKnown) {
        if (name == k) return true;
    }
    return false;
}

// Extract charset from a Content-Type header value ("text/html; charset=utf-8").
bool charset_from_content_type(kimix::string_view ct, kimix::string &out) {
    size_t i = 0;
    while (i < ct.size()) {
        size_t semi = ct.find(';', i);
        kimix::string_view part =
            (semi == kimix::string_view::npos) ? ct.substr(i)
                                               : ct.substr(i, semi - i);
        kimix::string p;
        strip_chars(part, " \t\r\n", true, true, p);
        if (p.rfind("charset", 0) == 0) {
            size_t eq = p.find('=');
            if (eq != kimix::string_view::npos) {
                ascii_lower_trim(kimix::string_view(p).substr(eq + 1), out);
                if (!out.empty()) return true;
            }
        }
        if (semi == kimix::string_view::npos) break;
        i = semi + 1;
    }
    return false;
}

} // namespace

kimix::string pick_encoding(
    kimix::string_view content_type,
    kimix::span<const kimix::string_view> meta_candidates) {
    kimix::string enc;
    if (charset_from_content_type(content_type, enc) && known_encoding(enc)) {
        return enc;
    }
    for (kimix::string_view cand : meta_candidates) {
        // A candidate may be a bare name ("utf-8"), a meta charset value, or
        // an http-equiv content string ("text/html; charset=gbk").
        kimix::string from_meta;
        if (charset_from_content_type(cand, from_meta)) {
            if (known_encoding(from_meta)) return from_meta;
        } else {
            ascii_lower_trim(cand, from_meta);
            if (known_encoding(from_meta)) return from_meta;
        }
    }
    // Default: UTF-8.
    return kimix::string("utf-8");
}

// ---------------------------------------------------------------------------
// Tool class wrapper
// ---------------------------------------------------------------------------

FetchUrl::FetchUrl(Session *session) : Tool(session) {}

void FetchUrl::operator()(ToolParams const *parameters) {
    _last_result.clear();
    ToolParams result;

    auto set_error = [&result](kimix::string_view message) {
        result.values["ok"] = ValueElement::make_bool(false);
        result.values["error"] = ValueElement::make_string(kimix::string(message));
    };

    if (parameters == nullptr) {
        set_error("missing parameters");
        result.serialize(_last_result);
        return;
    }

    auto const html_el = parameters->get("html");
    if (html_el == nullptr || !html_el->is_string()) {
        set_error("missing or invalid html parameter");
        result.serialize(_last_result);
        return;
    }

    bool extract = true;
    auto const extract_el = parameters->get("extract");
    if (extract_el != nullptr && extract_el->is_bool()) {
        extract = extract_el->as_bool();
    }

    int64_t max_length = 0;
    auto const max_length_el = parameters->get("max_length");
    if (max_length_el != nullptr) {
        if (max_length_el->is_int()) {
            max_length = max_length_el->as_int();
        } else if (max_length_el->is_uint()) {
            max_length = static_cast<int64_t>(max_length_el->as_uint());
        }
    }
    if (max_length < 0) {
        max_length = 0;
    }

    kimix::string markdown;
    tool_error err = html_to_markdown(html_el->as_string(), markdown, extract);
    if (err.failed()) {
        set_error(err.message.empty() ? kimix::string_view("html_to_markdown failed")
                                      : kimix::string_view(err.message));
        result.serialize(_last_result);
        return;
    }

    if (max_length > 0) {
        kimix::string truncated;
        truncate_line(markdown, static_cast<size_t>(max_length), truncated);
        markdown = std::move(truncated);
    }

    result.values["ok"] = ValueElement::make_bool(true);
    result.values["markdown"] = ValueElement::make_string(std::move(markdown));
    result.serialize(_last_result);
}

} // namespace kimix::builtin_tools::fetch_url
