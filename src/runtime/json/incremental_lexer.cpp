/*
 * incremental_lexer.cpp -- see incremental_lexer.h (plan 010).
 *
 * The scanner is a per-char state machine. State is kept in members so it
 * survives feed boundaries (a token split across two feeds is identical to
 * one feed). Relaxed rules (trailing commas, // and block comments) follow
 * the documented streamingjson loads_relaxed scope; raw newlines inside
 * strings are accepted (LLM streams emit them unescaped).
 */

#include <runtime/json/incremental_lexer.h>

namespace kimix {
namespace runtime {
namespace json {

namespace {

// Frame substates (object).
constexpr uint8_t kObjKeyExpected = 0;
constexpr uint8_t kObjColonExpected = 1;
constexpr uint8_t kObjValueExpected = 2;
constexpr uint8_t kObjValueDone = 3;
// Frame substates (array).
constexpr uint8_t kArrValueExpected = 0;
constexpr uint8_t kArrValueDone = 1;

constexpr uint8_t kFrameObj = 0;
constexpr uint8_t kFrameArr = 1;

// Scanner states.
constexpr uint8_t kScanNormal = 0;
constexpr uint8_t kScanString = 1;
constexpr uint8_t kScanStringEsc = 2;
constexpr uint8_t kScanUnicode = 3;
constexpr uint8_t kScanLineComment = 4;
constexpr uint8_t kScanBlockComment = 5;
constexpr uint8_t kScanBlockCommentStar = 6;
constexpr uint8_t kScanNumber = 7;
constexpr uint8_t kScanLiteral = 8;
constexpr uint8_t kScanSlashPending = 9;

// Number sub-states.
constexpr uint8_t kNumLeadingSign = 0;
constexpr uint8_t kNumInt = 1;
constexpr uint8_t kNumFracOrExp = 2;
constexpr uint8_t kNumFrac = 3;
constexpr uint8_t kNumExpSign = 4;
constexpr uint8_t kNumExpSigned = 5;
constexpr uint8_t kNumExp = 6;

// Literals.
constexpr uint8_t kLitTrue = 0;
constexpr uint8_t kLitFalse = 1;
constexpr uint8_t kLitNull = 2;
constexpr const char* kLiterals[3] = {"true", "false", "null"};

bool is_ws(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// A number may end at this char (delimiter follows the number token).
bool is_number_delim(char c) noexcept {
    return is_ws(c) || c == ',' || c == '}' || c == ']' || c == '/';
}

} // namespace

void IncrementalJsonLexer::feed(kimix::string_view chunk) noexcept {
    if (chunk.empty() || _error) {
        return;
    }
    _buf.append(chunk.data(), chunk.size());
    scan(_processed);
    _processed = _buf.size();
}

void IncrementalJsonLexer::append_utf8(uint32_t cp) noexcept {
    if (cp < 0x80) {
        _str_buf.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        _str_buf.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        _str_buf.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        _str_buf.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        _str_buf.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        _str_buf.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        _str_buf.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        _str_buf.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        _str_buf.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        _str_buf.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Record a completed top-level key -> value span (end = one past the last
// byte of the value). The value start was pinned by value_started().
void IncrementalJsonLexer::record_span(size_t end) noexcept {
    top_level_span s;
    s.key = _cur_key;
    s.start = _value_start;
    s.end = end;
    _spans.push_back(std::move(s));
}

bool IncrementalJsonLexer::value_span(kimix::string_view key, size_t& start,
                                      size_t& end) const noexcept {
    for (size_t i = _spans.size(); i > 0; --i) {
        const top_level_span& s = _spans[i - 1];
        if (s.key == key) {
            start = s.start;
            end = s.end;
            return true;
        }
    }
    return false;
}

kimix::vector<kimix::string> IncrementalJsonLexer::top_level_keys() const {
    kimix::vector<kimix::string> out;
    out.reserve(_spans.size());
    for (const auto& s : _spans) {
        out.push_back(s.key);
    }
    return out;
}

void IncrementalJsonLexer::reset() noexcept {
    _buf.clear();
    _processed = 0;
    _complete = false;
    _error = false;
    _root_done = false;
    _stack.clear();
    _scan = kScanNormal;
    _number_sub = 0;
    _literal_kind = 0;
    _literal_pos = 0;
    _unicode_need = 0;
    _unicode_val = 0;
    _comment_star = false;
    _in_key = false;
    _str_buf.clear();
    _pending_high = false;
    _pending_high_val = 0;
    _have_key = false;
    _cur_key.clear();
    _want_span = false;
    _value_start = 0;
    _spans.clear();
}

// ---------------------------------------------------------------------------
// The scanner. `pos` is the byte index of the char being processed.
// ---------------------------------------------------------------------------
void IncrementalJsonLexer::scan(size_t from) {
    const size_t size = _buf.size();

    // Local helpers as lambdas over *this.
    auto value_started = [&](size_t pos) {
        // A value token begins at `pos`. If it is the value of a top-level
        // key (':' consumed, start not yet pinned), record the start.
        if (_stack.size() == 1 && _want_span) {
            _value_start = pos;
            _want_span = false;
        }
    };
    auto scalar_complete = [&](size_t end) {
        // A scalar value (string/number/literal) ended at `end`.
        if (_stack.empty()) {
            // Root-level scalar -> the document is complete.
            _complete = true;
            _root_done = true;
            return;
        }
        Frame& f = _stack.back();
        if (f.kind == kFrameObj) {
            if (f.sub != kObjValueExpected) {
                // A scalar can only complete while a value is expected.
                set_error();
                return;
            }
            f.sub = kObjValueDone;
        } else {
            if (f.sub != kArrValueExpected) {
                set_error();
                return;
            }
            f.sub = kArrValueDone;
        }
        // Top-level key's scalar value completed.
        if (_stack.size() == 1 && _have_key && !_want_span) {
            record_span(end);
            _have_key = false;
            _want_span = false;
            _value_start = 0;
            _cur_key.clear();
        }
    };
    auto open_container = [&](uint8_t kind) {
        // A container opened; the current frame's substate becomes
        // VALUE_DONE only when the container closes (handled by the close
        // path below).
        Frame f;
        f.kind = kind;
        f.sub = (kind == kFrameObj) ? kObjKeyExpected : kArrValueExpected;
        _stack.push_back(f);
    };
    auto close_container = [&](size_t pos) {
        const size_t end = pos + 1; // one past the closing bracket
        if (_stack.empty()) {
            set_error();
            return;
        }
        Frame& f = _stack.back();
        if (f.kind == kFrameObj) {
            if (f.sub != kObjKeyExpected && f.sub != kObjValueDone) {
                set_error(); // `{"a": }`, `{"a":` + `}` etc.
                return;
            }
        } else {
            if (f.sub != kArrValueExpected && f.sub != kArrValueDone) {
                set_error();
                return;
            }
        }
        _stack.pop_back();
        if (_stack.empty()) {
            // Root value (container) closed -> complete.
            _complete = true;
            _root_done = true;
            return;
        }
        // Parent frame: the value it was expecting just completed.
        Frame& parent = _stack.back();
        if (parent.kind == kFrameObj) {
            parent.sub = (parent.sub == kObjValueExpected) ? kObjValueDone
                                                           : parent.sub;
        } else {
            parent.sub = (parent.sub == kArrValueExpected) ? kArrValueDone
                                                           : parent.sub;
        }
        // A top-level key's container value completed.
        if (_stack.size() == 1 && _have_key && !_want_span) {
            record_span(end);
            _have_key = false;
            _want_span = false;
            _value_start = 0;
            _cur_key.clear();
        }
    };
    // True when the current position may start a VALUE token (as opposed to
    // a key, a comma, a closing bracket, ...).
    auto value_position_ok = [&]() -> bool {
        if (_stack.empty()) {
            return !_root_done;
        }
        const Frame& f = _stack.back();
        if (f.kind == kFrameObj) {
            return f.sub == kObjValueExpected;
        }
        return f.sub == kArrValueExpected;
    };
    auto finish_key = [&]() {
        // A key string closed. `_str_buf` holds the decoded key bytes.
        if (_pending_high) {
            // Unpaired high surrogate at end of key string: emit it alone
            // (3-byte CESU-8 style, mirroring Python surrogatepass).
            append_utf8(_pending_high_val);
            _pending_high = false;
        }
        if (_stack.size() == 1) {
            _cur_key = _str_buf;
            _have_key = true;
        }
        _str_buf.clear();
        _in_key = false;
        Frame& f = _stack.back();
        if (f.kind == kFrameObj && f.sub == kObjKeyExpected) {
            f.sub = kObjColonExpected;
        } else {
            set_error();
        }
    };
    auto start_value_token = [&](size_t pos) {
        // A key's value begins here (scalar or container start). The caller
        // has already validated value_position_ok(); only record the span.
        value_started(pos);
    };

    for (size_t i = from; i < size; ++i) {
        const char c = _buf[i];
        if (_error) {
            return;
        }
        switch (_scan) {
        case kScanNormal: {
            if (is_ws(c)) {
                break;
            }
            if (c == '{') {
                if (!value_position_ok()) {
                    set_error();
                } else {
                    start_value_token(i);
                    open_container(kFrameObj);
                }
            } else if (c == '[') {
                if (!value_position_ok()) {
                    set_error();
                } else {
                    start_value_token(i);
                    open_container(kFrameArr);
                }
            } else if (c == '}') {
                if (_stack.empty() || _stack.back().kind != kFrameObj) {
                    set_error();
                } else {
                    close_container(i);
                }
            } else if (c == ']') {
                if (_stack.empty() || _stack.back().kind != kFrameArr) {
                    set_error();
                } else {
                    close_container(i);
                }
            } else if (c == ':') {
                if (_stack.empty() || _stack.back().kind != kFrameObj ||
                    _stack.back().sub != kObjColonExpected) {
                    set_error();
                } else {
                    _stack.back().sub = kObjValueExpected;
                    if (_stack.size() == 1 && _have_key) {
                        _want_span = true;
                        _value_start = 0;
                    }
                }
            } else if (c == ',') {
                if (_stack.empty()) {
                    set_error();
                } else {
                    Frame& f = _stack.back();
                    if (f.kind == kFrameObj) {
                        if (f.sub == kObjValueDone) {
                            f.sub = kObjKeyExpected;
                        } else {
                            set_error(); // `{,}`, `{"a":,}` etc.
                        }
                    } else {
                        if (f.sub == kArrValueDone) {
                            f.sub = kArrValueExpected;
                        } else {
                            set_error(); // `[,]`
                        }
                    }
                }
            } else if (c == '"') {
                const bool key_pos = !_stack.empty() &&
                                     _stack.back().kind == kFrameObj &&
                                     _stack.back().sub == kObjKeyExpected;
                if (!key_pos && !value_position_ok()) {
                    set_error();
                } else {
                    start_value_token(i);
                    if (key_pos) {
                        _in_key = true;
                        _str_buf.clear();
                    }
                    _scan = kScanString;
                }
            } else if (c == '-' || is_digit(c)) {
                if (!value_position_ok()) {
                    set_error();
                } else {
                    start_value_token(i);
                    _scan = kScanNumber;
                    _number_sub = (c == '-') ? kNumLeadingSign : kNumInt;
                }
            } else if (c == 't' || c == 'f' || c == 'n') {
                if (!value_position_ok()) {
                    set_error();
                } else {
                    start_value_token(i);
                    _scan = kScanLiteral;
                    _literal_kind = (c == 't') ? kLitTrue
                                  : (c == 'f') ? kLitFalse
                                               : kLitNull;
                    _literal_pos = 1;
                }
            } else if (c == '/') {
                // '/' must start a comment; the decision needs one more char.
                if (i + 1 < size) {
                    const char n = _buf[i + 1];
                    if (n == '/') {
                        _scan = kScanLineComment;
                        ++i; // consume both chars
                    } else if (n == '*') {
                        _scan = kScanBlockComment;
                        ++i;
                    } else {
                        set_error();
                    }
                } else {
                    _scan = kScanSlashPending;
                }
            } else if (_root_done) {
                // Anything except whitespace/comments after a complete
                // root value is a second top-level value -> error.
                set_error();
            } else {
                set_error();
            }
            break;
        }
        case kScanString: {
            if (c == '"') {
                if (_in_key) {
                    finish_key();
                } else {
                    scalar_complete(i + 1);
                }
                _scan = kScanNormal;
            } else if (c == '\\') {
                _scan = kScanStringEsc;
            } else if (_in_key) {
                _str_buf.push_back(c);
            }
            // Any other byte is allowed inside strings (relaxed: raw
            // control chars / newlines accepted, LLM streams emit them).
            break;
        }
        case kScanStringEsc: {
            switch (c) {
            case 'u':
                _scan = kScanUnicode;
                _unicode_need = 4;
                _unicode_val = 0;
                break;
            case '"':
                if (_in_key) _str_buf.push_back('"');
                _scan = kScanString;
                break;
            case '\\':
                if (_in_key) _str_buf.push_back('\\');
                _scan = kScanString;
                break;
            case '/':
                if (_in_key) _str_buf.push_back('/');
                _scan = kScanString;
                break;
            case 'b':
                if (_in_key) _str_buf.push_back('\b');
                _scan = kScanString;
                break;
            case 'f':
                if (_in_key) _str_buf.push_back('\f');
                _scan = kScanString;
                break;
            case 'n':
                if (_in_key) _str_buf.push_back('\n');
                _scan = kScanString;
                break;
            case 'r':
                if (_in_key) _str_buf.push_back('\r');
                _scan = kScanString;
                break;
            case 't':
                if (_in_key) _str_buf.push_back('\t');
                _scan = kScanString;
                break;
            default:
                set_error(); // unknown escape
                break;
            }
            break;
        }
        case kScanUnicode: {
            const int h = hex_value(c);
            if (h < 0) {
                set_error();
                break;
            }
            _unicode_val = (_unicode_val << 4) | static_cast<uint32_t>(h);
            if (--_unicode_need == 0) {
                if (_in_key) {
                    const uint32_t cp = _unicode_val;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // High surrogate: hold for a possible low pair.
                        if (_pending_high) {
                            append_utf8(_pending_high_val); // lone high
                        }
                        _pending_high = true;
                        _pending_high_val = cp;
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        if (_pending_high) {
                            const uint32_t combined =
                                0x10000 + ((_pending_high_val - 0xD800) << 10) +
                                (cp - 0xDC00);
                            append_utf8(combined);
                            _pending_high = false;
                        } else {
                            append_utf8(cp); // lone low surrogate
                        }
                    } else {
                        if (_pending_high) {
                            append_utf8(_pending_high_val);
                            _pending_high = false;
                        }
                        append_utf8(cp);
                    }
                }
                _scan = kScanString;
            }
            break;
        }
        case kScanLineComment: {
            if (c == '\n') {
                _scan = kScanNormal;
            }
            break;
        }
        case kScanBlockComment: {
            if (c == '*') {
                _scan = kScanBlockCommentStar;
            }
            break;
        }
        case kScanBlockCommentStar: {
            if (c == '/') {
                _scan = kScanNormal;
            } else if (c != '*') {
                _scan = kScanBlockComment;
            }
            break;
        }
        case kScanSlashPending: {
            // The '/' that started this state was consumed by the previous
            // feed's tail; `c` decides the comment kind.
            if (c == '/') {
                _scan = kScanLineComment;
            } else if (c == '*') {
                _scan = kScanBlockComment;
            } else {
                set_error();
            }
            break;
        }
        case kScanNumber: {
            bool done = false;
            switch (_number_sub) {
            case kNumLeadingSign:
                if (is_digit(c)) {
                    _number_sub = kNumInt;
                } else if (is_number_delim(c)) {
                    set_error(); // '-' alone
                } else {
                    set_error();
                }
                break;
            case kNumInt:
                if (is_digit(c)) {
                    // stay
                } else if (c == '.') {
                    _number_sub = kNumFracOrExp;
                } else if (c == 'e' || c == 'E') {
                    _number_sub = kNumExpSign;
                } else if (is_number_delim(c)) {
                    done = true;
                } else {
                    set_error();
                }
                break;
            case kNumFracOrExp:
                if (is_digit(c)) {
                    _number_sub = kNumFrac;
                } else if (is_number_delim(c)) {
                    set_error(); // '.' with no fraction digits
                } else {
                    set_error();
                }
                break;
            case kNumFrac:
                if (is_digit(c)) {
                    // stay
                } else if (c == 'e' || c == 'E') {
                    _number_sub = kNumExpSign;
                } else if (is_number_delim(c)) {
                    done = true;
                } else {
                    set_error();
                }
                break;
            case kNumExpSign:
                if (is_digit(c)) {
                    _number_sub = kNumExp;
                } else if (c == '+' || c == '-') {
                    _number_sub = kNumExpSigned;
                } else if (is_number_delim(c)) {
                    set_error(); // 'e' with no exponent digits
                } else {
                    set_error();
                }
                break;
            case kNumExpSigned:
                if (is_digit(c)) {
                    _number_sub = kNumExp;
                } else if (is_number_delim(c)) {
                    set_error(); // 'e+' with no digits
                } else {
                    set_error();
                }
                break;
            case kNumExp:
                if (is_digit(c)) {
                    // stay
                } else if (is_number_delim(c)) {
                    done = true;
                } else {
                    set_error();
                }
                break;
            }
            if (done) {
                _scan = kScanNormal;
                scalar_complete(i); // number ends before this delim char
                --i;                // reprocess the delimiter
            }
            break;
        }
        case kScanLiteral: {
            const char* lit = kLiterals[_literal_kind];
            if (c == lit[_literal_pos]) {
                ++_literal_pos;
                if (lit[_literal_pos] == '\0') {
                    _scan = kScanNormal;
                    scalar_complete(i + 1);
                }
            } else {
                set_error();
            }
            break;
        }
        } // switch (_scan)
    }     // for

    // NOTE: a number token is NEVER completed at a feed boundary. Numbers
    // terminate only when a delimiter (whitespace, ',', '}', ']', comment
    // start) arrives, so chunked feeding is byte-identical to one-shot
    // feeding (e.g. `3.5` split as "3" + ".5" must not complete `3` first).
    // A bare top-level number therefore stays incomplete until a delimiter
    // follows (streaming semantics; tool-call args are objects anyway).
}

} // namespace json
} // namespace runtime
} // namespace kimix
