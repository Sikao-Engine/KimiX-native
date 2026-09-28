/*
 * json_repair.cpp -- repair malformed JSON into strictly valid JSON.
 *
 * kimix::repair(json):
 * - Returns an empty vector when the input is already valid JSON
 * (strict check via yyjson, the same parser the rest of the project uses).
 * - Otherwise re-parses the input with a tolerant, iterative (stack-based,
 * non-recursive) parser that re-serializes canonical JSON as it goes. The
 * repaired buffer is NUL-terminated: the last element of the returned
 * kimix::vector<char> is '\0' and the JSON text is size() - 1 bytes.
 *
 * Malformed input handled (best effort, LLM-output oriented):
 *   - UTF-8 BOM, // and block comments, markdown fences / prose prologue and
 *     trailing chatter around the actual JSON value
 *   - Single-quoted, "smart" (curly) quoted and backtick-quoted strings
 *     (a run of 2+ backticks stays opaque: markdown fences are chatter)
 *   - Unquoted object keys and unquoted string values
 *   - Python literals: True/False/None, NaN/Infinity (any case, +/-Infinity)
 *   - Numbers: leading '+', leading zeros, ".5", "1.", hex 0x1A, dangling
 *     exponent ("1e"); garbage tokens that merely start like a number are
 *     quoted as strings
 * - Missing colons, missing commas between members/elements; ';' is also
 * accepted as a member/element separator (common LLM output); '=>', '->' and
 * a bare '=' are accepted as colon aliases
 * - Multi-word unquoted string values after a colon when the run ends at a
 * separator or a quoted next member (`"k": some words here, ...`)
 * - Full-width CJK punctuation: '：' as a colon alias, '，' and '：' as
 * member/element separators; thousands-separated integers in value position
 * (`1,234,567` -> `1234567`, refused for non-plain-digit runs)
 *   - Trailing / leading / doubled commas; stray colons; mismatched or extra
 *     closing brackets; missing closing brackets (truncated containers)
 *   - Raw control characters and raw newlines inside strings, invalid escape
 *     sequences (\q -> \\q), \xNN escapes, truncated strings, trailing
 *     backslash at EOF
 *   - Empty / whitespace-only / garbage input -> "null"
 *
 * The repaired output is validated with the strict parser before it is
 * returned, so the function never emits invalid JSON.
 */
#include <core/json_repair.h>
#include <core/stl/format.h>

#include "yyjson.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace kimix {
namespace {

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex_digit(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
int hex_val(char c) {
    if (is_digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool ieq(string_view a, const char *b) {
    size_t n = 0;
    while (n < a.size() && b[n] != '\0' && lower(a[n]) == b[n]) n++;
    return n == a.size() && b[n] == '\0';
}

bool starts_value(char c) {
    return c == '{' || c == '[' || is_digit(c) || c == '+' || c == '-' || c == '.';
}

// Length of the valid UTF-8 sequence at in[i] (0 if the byte cannot start or
// continue a well-formed code point; rejects overlong forms, UTF-8-encoded
// surrogates and code points above U+10FFFF, matching strict JSON).
size_t utf8_valid_len(string_view in, size_t i) {
    auto c = static_cast<unsigned char>(in[i]);
    size_t n;
    uint32_t cp;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07u; }
    else return 0;
    if (i + n > in.size()) return 0;
    for (size_t k = 1; k < n; k++) {
        auto t = static_cast<unsigned char>(in[i + k]);
        if ((t & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (t & 0x3Fu);
    }
    if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000)) return 0;
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    if (cp > 0x10FFFF) return 0;
    return n;
}

// Append the raw bytes at in[i] (one code point). Invalid UTF-8 bytes are
// emitted as \u00XX escapes so the output always stays strictly valid.
void emit_raw_utf8(string &out, string_view in, size_t &i) {
    if (static_cast<unsigned char>(in[i]) < 0x80) {
        out += in[i];
        i++;
        return;
    }
    size_t n = utf8_valid_len(in, i);
    if (n > 0) {
        out.append(in.substr(i, n));
        i += n;
    } else {
        out += format("\\u{:04x}", static_cast<unsigned char>(in[i]));
        i++;
    }
}

// True when the byte at `pos` opens a quote-like delimiter; fills `kind`
// ('d' = double style, 's' = single style) and `len` (3 for UTF-8 curly
// quotes, 1 for ASCII quotes).
bool at_quote(string_view in, size_t pos, char &kind, size_t &len) {
    if (pos >= in.size()) return false;
    unsigned char c = static_cast<unsigned char>(in[pos]);
    if (c == '"') { kind = 'd'; len = 1; return true; }
    if (c == '\'') { kind = 's'; len = 1; return true; }
    if (c == '`') {
        // A backtick is a string delimiter only when it is a lone backtick;
        // a run of 2+ is a markdown fence and must stay opaque (the prologue
        // skips fences as chatter instead of parsing them as quotes).
        if ((pos > 0 && in[pos - 1] == '`') ||
            (pos + 1 < in.size() && in[pos + 1] == '`'))
            return false;
        kind = 't'; len = 1; return true;
    }
    if (c == 0xE2 && pos + 2 < in.size() &&
        static_cast<unsigned char>(in[pos + 1]) == 0x80) {
        unsigned char t = static_cast<unsigned char>(in[pos + 2]);
        if (t == 0x9C || t == 0x9D) { kind = 'd'; len = 3; return true; } // curly double quotes
        if (t == 0x98 || t == 0x99) { kind = 's'; len = 3; return true; } // curly single quotes
    }
    return false;
}

// True when the 3-byte UTF-8 sequence at `pos` is full-width CJK punctuation
// U+FFxx: ',' third byte 0x8C, ':' third byte 0x9A. CJK-language models
// often emit these where an ASCII ',' or ':' belongs.
bool at_fw_punct(string_view in, size_t pos, unsigned char third) {
    return pos + 2 < in.size() && static_cast<unsigned char>(in[pos]) == 0xEF &&
           static_cast<unsigned char>(in[pos + 1]) == 0xBC &&
           static_cast<unsigned char>(in[pos + 2]) == third;
}

// True when the sequence at `pos` is a full-width separator ('，' or '：').
bool at_fw_sep(string_view in, size_t pos) {
    return at_fw_punct(in, pos, 0x8C) || at_fw_punct(in, pos, 0x9A);
}

// Emit `raw` as a double-quoted JSON string, escaping as needed.
void emit_escaped(string &out, string_view raw) {
    out += '"';
    size_t i = 0;
    while (i < raw.size()) {
        char c = raw[i];
        switch (c) {
            case '"': out += "\\\""; i++; break;
            case '\\': out += "\\\\"; i++; break;
            case '\n': out += "\\n"; i++; break;
            case '\t': out += "\\t"; i++; break;
            case '\r': out += "\\r"; i++; break;
            case '\b': out += "\\b"; i++; break;
            case '\f': out += "\\f"; i++; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += format("\\u{:04x}", static_cast<unsigned char>(c));
                    i++;
                } else {
                    emit_raw_utf8(out, raw, i);
                }
        }
    }
    out += '"';
}

// Normalize a bare token that looks like a number into a valid JSON number.
// Returns false when the token is not a repairable number.
bool normalize_number(string_view t, string &out) {
    size_t i = 0;
    bool neg = false;
    if (i < t.size() && t[i] == '+') i++;
    else if (i < t.size() && t[i] == '-') { neg = true; i++; }
    string_view r = t.substr(i);
    if (r.empty()) return false;

    // Hex integer (0x1A / -0xFF)
    if (r.size() > 2 && r[0] == '0' && (r[1] == 'x' || r[1] == 'X')) {
        uint64_t v = 0;
        size_t j = 2;
        for (; j < r.size() && is_hex_digit(r[j]); j++) v = v * 16u + static_cast<uint64_t>(hex_val(r[j]));
        if (j != r.size()) return false;
        if (neg) out += '-';
        out += format("{}", v);
        return true;
    }

    string norm;
    if (neg) norm += '-';
    size_t j = 0;
    size_t int_digits = 0;
    size_t frac_digits = 0;
    if (j < r.size() && r[j] == '.') {
        // ".5" style
        if (j + 1 >= r.size() || !is_digit(r[j + 1])) return false;
        norm += "0.";
        j++;
        while (j < r.size() && is_digit(r[j])) { norm += r[j++]; frac_digits++; }
    } else if (is_digit(r[j])) {
        size_t zeros = j;
        while (j < r.size() && r[j] == '0') j++;
        size_t sig = j;
        while (j < r.size() && is_digit(r[j])) j++;
        int_digits = j - sig;
        if (int_digits == 0) norm += '0'; // only zeros ("000" -> "0", "-00.5" -> "-0.5")
        else norm.append(r.substr(sig, int_digits));
    } else {
        return false;
    }
    if (j < r.size() && r[j] == '.') {
        j++;
        size_t d0 = j;
        while (j < r.size() && is_digit(r[j])) j++;
        frac_digits += j - d0;
        if (j == d0) {
            if (j == r.size() || r[j] == 'e' || r[j] == 'E') norm += ".0"; // "1." / "1.e5"
            else return false;                                             // "1.x"
        } else {
            norm += '.';
            norm.append(r.substr(d0, j - d0));
        }
    }
    if (j < r.size() && (r[j] == 'e' || r[j] == 'E')) {
        size_t k = j + 1;
        bool esign = false;
        if (k < r.size() && (r[k] == '+' || r[k] == '-')) { esign = r[k] == '-'; k++; }
        size_t e0 = k;
        long long e = 0;
        while (k < r.size() && is_digit(r[k])) {
            if (e < 1000000) e = e * 10 + (r[k] - '0');
            k++;
        }
        if (esign) e = -e;
        if (k == e0) {
            if (k != r.size()) return false; // "1ex"
            // dangling exponent ("1e") -> drop it
        } else if (k != r.size()) {
            return false; // "1e5x"
        } else {
            // clamp the exponent so the value stays inside the double range
            // (yyjson's strict reader rejects out-of-range numbers)
            long long cap = 300 - static_cast<long long>(int_digits);
            if (cap < 0) cap = 0;
            long long floor_ = static_cast<long long>(frac_digits) - 300;
            if (e > cap) e = cap;
            if (e < floor_) e = floor_;
            if (e != 0) {
                norm += 'e';
                if (e < 0) { norm += '-'; e = -e; }
                norm += format("{}", static_cast<uint64_t>(e));
            }
        }
        j = k;
    }
    if (j != r.size()) return false; // trailing junk -> caller quotes the token
    // final safety: the normalized number must be a finite double
    errno = 0;
    char *endp = nullptr;
    double v = std::strtod(norm.c_str(), &endp);
    if (endp == nullptr || *endp != '\0' || !std::isfinite(v) ||
        (v == 0.0 && errno == ERANGE))
        return false; // e.g. a 400-digit integer -> quote as string instead
    out += norm;
    return true;
}

// Classify and emit a bare (unquoted) token as a JSON value.
void emit_bare_value(string &out, string_view tok) {
    if (ieq(tok, "true")) { out += "true"; return; }
    if (ieq(tok, "false")) { out += "false"; return; }
    if (ieq(tok, "null") || ieq(tok, "none") || ieq(tok, "nan") ||
        ieq(tok, "undefined")) {
        out += "null";
        return;
    }
    if (!tok.empty() && (tok[0] == '+' || tok[0] == '-')) {
        if (ieq(tok.substr(1), "infinity") || ieq(tok.substr(1), "inf")) {
            out += "null";
            return;
        }
    } else if (ieq(tok, "infinity") || ieq(tok, "inf")) {
        out += "null";
        return;
    }
    if (!tok.empty() && (is_digit(tok[0]) || tok[0] == '+' || tok[0] == '-' || tok[0] == '.')) {
        if (normalize_number(tok, out)) return;
    }
    emit_escaped(out, tok); // unquoted word value -> string
}

// Tolerant re-serializing parser. Iterative: nesting is an explicit stack, so
// pathological deep input cannot overflow the call stack.
struct Repairer {
    string_view in;
    size_t pos{0};
    string out;

    struct Frame {
        bool obj;     // true = object, false = array
        bool has_prev;// a member/element has been emitted already
    };
    kimix::vector<Frame> stk;

    enum class St { Value, Key, Colon } st{St::Value};
    bool value_required{false}; // Value state reached after a colon
    bool done{false};

    bool eof() const { return pos >= in.size(); }
    char peek() const { return eof() ? '\0' : in[pos]; }

    void skip_ws() {
        while (!eof()) {
            char c = in[pos];
            if (is_ws(c)) { pos++; continue; }
            if (c == '/' && pos + 1 < in.size() && in[pos + 1] == '/') {
                pos += 2;
                while (!eof() && in[pos] != '\n') pos++;
                continue;
            }
            if (c == '/' && pos + 1 < in.size() && in[pos + 1] == '*') {
                pos += 2;
                while (!eof() && !(in[pos] == '*' && pos + 1 < in.size() && in[pos + 1] == '/')) pos++;
                if (!eof()) pos += 2;
                continue;
            }
            break;
        }
    }

    // Scan a quoted string starting at `pos` (any quote-like delimiter) and
    // emit it as a properly escaped double-quoted JSON string.
    void scan_quoted() {
        char kind = 'd';
        size_t len = 0;
        at_quote(in, pos, kind, len); // caller guarantees this succeeds
        pos += len;
        out += '"';
        while (!eof()) {
            char c = in[pos];
            char qk;
            size_t ql;
            // any quote of the same style closes the string (curly quotes
            // accepted as delimiters are also accepted as closers)
            if (at_quote(in, pos, qk, ql) && qk == kind) {
                pos += ql;
                break;
            }
            if (c == '\\') {
                pos++;
                if (eof()) break; // trailing backslash -> drop
                char e = in[pos++];
                switch (e) {
                    case 'n': out += "\\n"; break;
                    case 't': out += "\\t"; break;
                    case 'r': out += "\\r"; break;
                    case 'b': out += "\\b"; break;
                    case 'f': out += "\\f"; break;
                    case '"': out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '/': out += "\\/"; break;
                    case '\'': out += '\''; break;
                    case '`': out += '`'; break; // \` escape in backtick strings
                    case 'u':
                        if (pos + 4 <= in.size() && is_hex_digit(in[pos]) &&
                            is_hex_digit(in[pos + 1]) && is_hex_digit(in[pos + 2]) &&
                            is_hex_digit(in[pos + 3])) {
                            auto cp = static_cast<uint32_t>(hex_val(in[pos]) * 4096 +
                                                           hex_val(in[pos + 1]) * 256 +
                                                           hex_val(in[pos + 2]) * 16 +
                                                           hex_val(in[pos + 3]));
                            bool high = cp >= 0xD800 && cp <= 0xDBFF;
                            bool low = cp >= 0xDC00 && cp <= 0xDFFF;
                            // a well-formed \uD8xx\uDCxx pair is valid verbatim
                            if (high && pos + 10 <= in.size() && in[pos + 4] == '\\' &&
                                in[pos + 5] == 'u' && is_hex_digit(in[pos + 6]) &&
                                is_hex_digit(in[pos + 7]) && is_hex_digit(in[pos + 8]) &&
                                is_hex_digit(in[pos + 9])) {
                                auto lo = static_cast<uint32_t>(hex_val(in[pos + 6]) * 4096 +
                                                               hex_val(in[pos + 7]) * 256 +
                                                               hex_val(in[pos + 8]) * 16 +
                                                               hex_val(in[pos + 9]));
                                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                    out += "\\u";
                                    out.append(in.substr(pos, 4));
                                    out += "\\u";
                                    out.append(in.substr(pos + 6, 4));
                                    pos += 10;
                                    break;
                                }
                            }
                            if (high || low) {
                                out += "\\uFFFD"; // lone surrogate -> replacement char
                                pos += 4;
                            } else {
                                out += "\\u";
                                out.append(in.substr(pos, 4));
                                pos += 4;
                            }
                        } else {
                            out += "\\\\u"; // lone \u -> literal "\u"
                        }
                        break;
                    case 'x':
                        if (pos + 2 <= in.size() && is_hex_digit(in[pos]) && is_hex_digit(in[pos + 1])) {
                            auto v = static_cast<unsigned char>(hex_val(in[pos]) * 16 + hex_val(in[pos + 1]));
                            pos += 2;
                            // Bytes >= 0x80 must not be emitted raw: a lone
                            // continuation/lead byte (or an incomplete UTF-8
                            // sequence) would make the output invalid UTF-8 and
                            // the strict final check would reject the whole
                            // repair. Escape them as \u00XX instead.
                            if (v < 0x20 || v >= 0x80 || v == '"' || v == '\\') {
                                out += format("\\u{:04x}", v);
                            } else {
                                out += static_cast<char>(v);
                            }
                        } else {
                            out += "\\\\x";
                        }
                        break;
                    default:
                        out += "\\\\";
                        if (static_cast<unsigned char>(e) < 0x20) {
                            out += format("\\u{:04x}", static_cast<unsigned char>(e));
                        } else {
                            pos--; // e may start a multi-byte UTF-8 sequence
                            emit_raw_utf8(out, in, pos);
                        }
                        break; // unknown escape -> keep both chars
                }
                continue;
            }
            if (c == '"') { // raw double quote inside a single-quoted string
                out += "\\\"";
                pos++;
                continue;
            }
            if (c == '\'') { // raw apostrophe inside a double-quoted string
                out += '\'';
                pos++;
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                switch (c) {
                    case '\n': out += "\\n"; break;
                    case '\t': out += "\\t"; break;
                    case '\r': out += "\\r"; break;
                    default: out += format("\\u{:04x}", static_cast<unsigned char>(c)); break;
                }
                pos++;
                continue;
            }
            if (static_cast<unsigned char>(c) >= 0x80) {
                emit_raw_utf8(out, in, pos); // invalid UTF-8 bytes become \u00XX
                continue;
            }
            out += c;
            pos++;
        }
        out += '"'; // also closes truncated (EOF) strings
    }

    // Terminator set for bare (unquoted) tokens.
    static bool bare_terminator(char c) {
        return is_ws(c) || c == ',' || c == ':' || c == ';' || c == '{' || c == '}' ||
               c == '[' || c == ']' || c == '/';
    }

    // ---- container bookkeeping -------------------------------------------

    // Emit the closer for the top frame, pop it, and mark the parent as
    // having received a value. Returns false when there was no open frame.
    bool close_top() {
        if (stk.empty()) return false;
        out += stk.back().obj ? '}' : ']';
        stk.pop_back();
        finish_value();
        return true;
    }

    void finish_value() {
        if (stk.empty()) {
            done = true;
            return;
        }
        stk.back().has_prev = true;
        st = stk.back().obj ? St::Key : St::Value;
        value_required = false;
    }

    // Insert the comma that separates this element from the previous one.
    void maybe_comma() {
        if (!stk.empty() && stk.back().has_prev) out += ',';
    }

    // ---- state machine ----------------------------------------------------

    string run() {
        if (in.size() >= 3 && static_cast<unsigned char>(in[0]) == 0xEF &&
            static_cast<unsigned char>(in[1]) == 0xBB &&
            static_cast<unsigned char>(in[2]) == 0xBF)
            pos = 3;
        skip_ws();
        // Prologue chatter (markdown fences, "here is the json:"): skip to the
        // first plausible value start.
        if (!eof() && !starts_value(peek())) {
            size_t start = pos;
            // A quote only anchors the value when it is not glued to a
            // preceding identifier character: in prose like "Here's the
            // JSON:", the apostrophe is part of the word, not a string
            // delimiter, so it must be skipped. A quote at position 0 is
            // always an anchor.
            auto quote_is_anchor = [&]() {
                char k;
                size_t l;
                if (!at_quote(in, pos, k, l)) return false;
                if (pos == 0) return true;
                char prev = in[pos - 1];
                return !(prev == '_' || (prev >= 'a' && prev <= 'z') ||
                         (prev >= 'A' && prev <= 'Z') || (prev >= '0' && prev <= '9'));
            };
            while (!eof() && peek() != '{' && peek() != '[' && !quote_is_anchor() &&
                   !starts_value(peek()))
                pos++;
            skip_ws();
            if (eof()) pos = start; // no JSON anchor: parse from the beginning
        }

        while (!done) {
            switch (st) {
                case St::Value: {
                    skip_ws();
                    if (eof()) {
                        if (value_required) { out += "null"; value_required = false; }
                        while (close_top()) {}
                        done = true; // top-level end of input
                        break;
                    }
                    char c = peek();
                    char k;
                    size_t l;
                    if (c == '{') {
                        bool elem = !stk.empty() && !stk.back().obj && !value_required;
                        if (elem) maybe_comma();
                        pos++;
                        out += '{';
                        stk.push_back({true, false});
                        st = St::Key;
                        continue;
                    }
                    if (c == '[') {
                        bool elem = !stk.empty() && !stk.back().obj && !value_required;
                        if (elem) maybe_comma();
                        pos++;
                        out += '[';
                        stk.push_back({false, false});
                        st = St::Value;
                        value_required = false;
                        continue;
                    }
                    if (at_quote(in, pos, k, l)) {
                        bool elem = !stk.empty() && !stk.back().obj && !value_required;
                        if (elem) maybe_comma();
                        scan_quoted();
                        finish_value();
                        continue;
                    }
                if (c == ',' || c == ':' || c == ';' || at_fw_sep(in, pos)) { // stray separator
                    if (value_required) {
                        out += "null";
                        value_required = false;
                        finish_value();
                    }
                    pos += at_fw_sep(in, pos) ? 3 : 1;
                    continue;
                }
                    if (c == '}' || c == ']') { // mismatched / immediate closer
                        if (value_required) {
                            out += "null";
                            value_required = false;
                        }
                        if (stk.empty()) { pos++; continue; } // extra closer at top level
                        pos++;
                        // close whichever container is actually open
                        out += stk.back().obj ? '}' : ']';
                        stk.pop_back();
                        finish_value();
                        continue;
                    }
                    if (c == '/') { pos++; continue; } // lone slash
                    // bare token
                    {
                        size_t b = pos;
                while (!eof() && !bare_terminator(peek())) {
                    char kk;
                    size_t ll;
                    if (at_quote(in, pos, kk, ll)) break;
                    if (at_fw_sep(in, pos)) break; // full-width ',' / ':' end the token
                    pos++;
                }
                if (pos == b) { pos++; continue; } // guarantee progress
                        bool elem = !stk.empty() && !stk.back().obj && !value_required;
                        if (elem) maybe_comma();
                string_view tok = in.substr(b, pos - b);
                // Thousands-separated integers: 1,234,567 / 93,000 (common in
                // human-readable LLM reports). Applies only right after a
                // colon, only when the token is 1-3 plain digits and every
                // ',' is glued to exactly three following digits. A malformed
                // group just ends the run there (that comma stays a member
                // separator); a run followed by '.', 'e' or 'E' is refused so
                // the old garbage path keeps quoting it as a string.
                if (value_required && !tok.empty() && tok.size() <= 3 && is_digit(tok[0])) {
                    bool pure = true;
                    for (char ch : tok) { if (!is_digit(ch)) { pure = false; break; } }
                    if (pure) {
                        string digits{tok};
                        size_t la = pos, end = pos;
                        while (la < in.size() && in[la] == ',') {
                            size_t g = la + 1, d = 0;
                            while (d < 3 && g < in.size() && is_digit(in[g])) { digits += in[g]; g++; d++; }
                            if (d != 3 || (g < in.size() && is_digit(in[g]))) break;
                            la = g;
                            end = g;
                        }
                        if (end > pos &&
                            !(end < in.size() && (in[end] == '.' || in[end] == 'e' || in[end] == 'E'))) {
                            pos = end;
                            normalize_number(string_view{digits}, out);
                            finish_value();
                            continue;
                        }
                    }
                }
                // Multi-word unquoted string values: right after a
                        // colon a bare word that is not a literal or number
                        // may continue across plain spaces, but only when
                        // the run ends at a sibling separator (`,`/`;`) or
                        // just before the next quoted member
                        // (`"author": Jane Q. Public, ...`). Ending at a
                        // closer / EOF / comment keeps the old reading (a
                        // word like `{a: hello world}` is still parsed as
                        // key `world`), and a bare word followed by ':' or
                        // '=' is always the next member key, so
                        // `{a: foo b: 1}` still yields two members; array
                        // elements are never joined.
                        string words;
                        bool extended = false;
                        if (value_required && !tok.empty() &&
                            !(is_digit(tok[0]) || tok[0] == '+' || tok[0] == '-' ||
                              tok[0] == '.') &&
                            !ieq(tok, "true") && !ieq(tok, "false") &&
                            !ieq(tok, "null") && !ieq(tok, "none") &&
                            !ieq(tok, "nan") && !ieq(tok, "undefined") &&
                            !ieq(tok, "inf") && !ieq(tok, "infinity")) {
                            string cand{tok};
                            size_t la = pos; // lookahead; only committed on merge
                            size_t pieces = 0;
                            int stop = 0; // 1 = sep, 2 = quoted next, 3 = refuse
                            while (stop == 0) {
                                size_t q = la;
                                while (q < in.size() && (in[q] == ' ' || in[q] == '\t'))
                                    q++;
                                if (q >= in.size()) { stop = 3; break; } // EOF
                                if (q == la) { // run ended on the last terminator
                                    char t = in[la];
                                    stop = (t == ',' || t == ';') ? 1 : 3;
                                    break;
                                }
                                char kk2;
                                size_t ll2;
                                if (at_quote(in, q, kk2, ll2)) { stop = 2; break; }
                                if (in[q] == '/' && q + 1 < in.size() &&
                                    (in[q + 1] == '/' || in[q + 1] == '*')) {
                                    stop = 3; // comment: not part of a value
                                    break;
                                }
                                size_t w = q;
                                while (w < in.size() && !bare_terminator(in[w])) {
                                    char kk3;
                                    size_t ll3;
                                    if (at_quote(in, w, kk3, ll3)) break;
                                    w++;
                                }
                                if (w == q) { stop = 3; break; }
                                if (w >= in.size()) { stop = 3; break; } // word at EOF
                                if (in[w] == ':' || in[w] == '=') { stop = 3; break; }
                                cand += ' ';
                                cand.append(in.substr(q, w - q));
                                pieces++;
                                la = w;
                            }
                            if ((stop == 1 || stop == 2) && pieces > 0) {
                                words = cand;
                                pos = la;
                                extended = true;
                            }
                        }
                        if (extended) emit_escaped(out, words);
                        else emit_bare_value(out, tok);
                        finish_value();
                        continue;
                    }
                }
                case St::Key: {
                    skip_ws();
                    if (eof()) {
                        close_top();
                        continue;
                    }
                    char c = peek();
                    char k;
                    size_t l;
                    if (c == '}' || c == ']') { // ']' closes an object too
                        pos++;
                        close_top();
                        continue;
                    }
                    if (c == ',' || c == ':' || c == ';') { pos++; continue; } // stray separators
                    if (at_fw_sep(in, pos)) { pos += 3; continue; } // full-width '，' / '：'
                    maybe_comma(); // missing comma between members
                    if (at_quote(in, pos, k, l)) {
                        scan_quoted();
                    } else {
                        size_t b = pos;
                while (!eof() && !bare_terminator(peek())) {
                    char kk;
                    size_t ll;
                    if (at_quote(in, pos, kk, ll)) break;
                    if (at_fw_sep(in, pos)) break; // full-width '，' / '：' end the key
                    pos++;
                }
                emit_escaped(out, in.substr(b, pos - b)); // unquoted key
                    }
                    st = St::Colon;
                    continue;
                }
            case St::Colon: {
                skip_ws();
                if (eof()) { // truncated right after the key
                    out += ':';
                    out += "null";
                    close_top();
                    continue;
                }
    out += ':';
    if (peek() == ':') pos++; // missing colon is inserted
    else if (at_fw_punct(in, pos, 0x9A)) pos += 3; // full-width colon '：'
                // Colon aliases: "k" => v, "k" -> v, "k" = v. LLMs mixing
                // Python / pseudo-code into JSON commonly write fat-arrows or
                // a bare '=' where a ':' belongs.
                skip_ws();
                if (peek() == '=') {
                    pos++;
                    if (peek() == '>') pos++;
                } else if (peek() == '-' && pos + 1 < in.size() && in[pos + 1] == '>') {
                    pos += 2;
                }
                st = St::Value;
                value_required = true;
                continue;
            }
            }
        }
        if (out.empty()) return "null";
        return out;
    }
};

bool is_valid_json(string_view s) {
    yyjson_doc *doc = yyjson_read(s.data(), s.size(), YYJSON_READ_NOFLAG);
    if (doc == nullptr) return false;
    yyjson_doc_free(doc);
    return true;
}

} // namespace

vector<char> repair(string_view json) {
    if (is_valid_json(json)) return {};
    Repairer r;
    r.in = json;
    string res = r.run();
    if (!is_valid_json(res)) return {}; // never emit invalid JSON
    // Hand the canonical text out as a NUL-terminated byte buffer: the JSON
    // occupies the first size() - 1 elements and back() is '\0', so callers
    // may pass data() straight to C-string APIs (e.g. yyjson).
    vector<char> out;
    out.reserve(res.size() + 1);
    out.insert(out.end(), res.begin(), res.end());
    out.push_back('\0');
    return out;
}

} // namespace kimix
