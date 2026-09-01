/*
 * comment_scanner.cpp - Comment extraction for 7 languages (plan 011).
 *
 * Faithful ports of the kimi-agent reference parsers (the seven files in
 * src/kimix/parser).
 * Each scanner is a direct transliteration of the Python state machine; only
 * byte-offset spans are produced (no per-char output buffers, no line/column
 * counters - the shim derives those from the spans by code point).
 */

#include <runtime/parse/ascii_util.h>
#include <runtime/parse/comment_scanner.h>

namespace kimix {
namespace runtime {
namespace parse {

namespace {

// ASCII char-class helpers live in the shared ascii_util.h header
// (kimix::runtime::parse::detail). They were previously defined here AND in
// shell_scanner.cpp, which collided when a unity build merged both TUs.
using namespace detail;

// Newline index within [from, end) or `end` (memchr-backed).
inline size_t find_nl(kimix::string_view s, size_t from, size_t end) noexcept {
    return detail::scan_find_char(s.data(), end, from, '\n');
}

void emit(kimix::vector<comment_span>& out, uint32_t start, uint32_t end, uint32_t kind) {
    out.push_back(comment_span{start, end, kind});
}

// ===========================================================================
// C / C++ / Java / JS / TS / C# / Go / Rust (c_parser.py)
// ===========================================================================

bool regex_keyword(kimix::string_view word) noexcept {
    switch (word.size()) {
    case 2: return word == "in" || word == "of";
    case 3: return word == "new";
    case 4: return word == "case" || word == "void" || word == "else";
    case 5: return word == "throw" || word == "yield";
    case 6: return word == "delete" || word == "return" || word == "typeof";
    case 7: return word == "await";
    case 9: return word == "instanceof";
    default: return false;
    }
}

bool regex_preceding(char c) noexcept {
    switch (c) {
    case '=': case '(': case '[': case '!': case '&': case '|': case ',':
    case ';': case '{': case ':': case '?': case '~': case '^': case '*':
    case '-': case '+': case '%': case '<': case '>': case '/':
        return true;
    default:
        return false;
    }
}

void scan_c(kimix::string_view s, kimix::vector<comment_span>& out) {
    enum : uint8_t {
        CODE, LINE_COMMENT, BLOCK_COMMENT, DOC_COMMENT,
        STRING_DOUBLE, STRING_SINGLE, BACKTICK_STRING, REGEX_LITERAL
    };
    const size_t n = s.size();
    size_t i = 0;
    uint8_t state = CODE;
    uint32_t start = 0;
    bool in_raw_string = false;
    size_t raw_hash_count = 0;
    int32_t prev_non_whitespace = -1; // -1 == None
    kimix::string word_buffer;

    while (i < n) {
        if (state == CODE) {
            const char ch = s[i];
            const char next = (i + 1 < n) ? s[i + 1] : '\0';
            // Rust raw string r#"..."# (only when 'r' starts one)
            if (ch == 'r' && next == '#' && !in_raw_string) {
                size_t j = i + 1;
                size_t hashes = 0;
                while (j < n && s[j] == '#') {
                    ++hashes;
                    ++j;
                }
                if (j < n && s[j] == '"') {
                    in_raw_string = true;
                    raw_hash_count = hashes;
                    state = STRING_DOUBLE;
                    i = j + 1;
                    prev_non_whitespace = '"';
                    word_buffer.clear();
                    continue;
                }
                // fall through: 'r' is an ordinary code character
            }
            if (ch == '/' && next == '/') {
                start = static_cast<uint32_t>(i + 2);
                state = LINE_COMMENT;
                i += 2;
                continue;
            }
            if (ch == '/' && next == '*') {
                if (i + 2 < n && s[i + 2] == '*' &&
                    !(i + 3 < n && s[i + 3] == '/')) {
                    // /** ... */ doc comment (but not "/**/")
                    start = static_cast<uint32_t>(i + 3);
                    state = DOC_COMMENT;
                    i += 3;
                } else {
                    start = static_cast<uint32_t>(i + 2);
                    state = BLOCK_COMMENT;
                    i += 2;
                }
                continue;
            }
            if (ch == '"') {
                state = STRING_DOUBLE;
                prev_non_whitespace = '"';
                word_buffer.clear();
                i += 1;
                continue;
            }
            if (ch == '\'') {
                state = STRING_SINGLE;
                prev_non_whitespace = '\'';
                word_buffer.clear();
                i += 1;
                continue;
            }
            if (ch == '`') {
                state = BACKTICK_STRING;
                prev_non_whitespace = '`';
                word_buffer.clear();
                i += 1;
                continue;
            }
            // Regex literal heuristic (JavaScript/TypeScript)
            if (ch == '/') {
                bool regex_start = false;
                if (prev_non_whitespace < 0) {
                    regex_start = true;
                } else if (regex_keyword(word_buffer)) {
                    regex_start = true;
                } else if (regex_preceding(static_cast<char>(prev_non_whitespace))) {
                    regex_start = true;
                }
                if (regex_start) {
                    state = REGEX_LITERAL;
                    prev_non_whitespace = '/';
                    word_buffer.clear();
                    i += 1;
                    continue;
                }
            }
            // Regular code character
            if (!ascii_space(ch)) {
                prev_non_whitespace = static_cast<int32_t>(static_cast<unsigned char>(ch));
                if (ascii_alnum(ch) || ch == '_') {
                    word_buffer.push_back(ch);
                } else {
                    word_buffer.clear();
                }
            }
            i += 1;
        } else if (state == LINE_COMMENT) {
            const size_t nl = find_nl(s, i, n);
            emit(out, start, static_cast<uint32_t>(nl), 0);
            state = CODE;
            i = (nl < n) ? nl + 1 : n;
        } else if (state == BLOCK_COMMENT || state == DOC_COMMENT) {
            const size_t close = detail::scan_find_sub(s.data(), n, i, "*/", 2);
            if (close < n) {
                emit(out, start, static_cast<uint32_t>(close),
                     state == DOC_COMMENT ? 2u : 1u);
                state = CODE;
                i = close + 2;
            } else {
                // unclosed: state stays set so the EOF emission below runs
                i = n;
            }
        } else if (state == STRING_DOUBLE) {
            const size_t hit = detail::scan_find_any(s.data(), n, i, "\"\\\n", 3);
            if (hit >= n) {
                i = n;
            } else if (s[hit] == '\\') {
                i = (hit + 1 < n) ? hit + 2 : hit + 1;
            } else if (s[hit] == '\n') {
                // Unclosed string at end of line - handle gracefully
                in_raw_string = false;
                state = CODE;
                i = hit + 1;
            } else if (in_raw_string) {
                // Closing Rust raw string: " followed by #*raw_hash_count
                size_t j = hit + 1;
                size_t found = 0;
                while (j < n && s[j] == '#') {
                    ++found;
                    ++j;
                }
                if (found == raw_hash_count) {
                    in_raw_string = false;
                    state = CODE;
                    prev_non_whitespace = '"';
                    word_buffer.clear();
                    i = j;
                } else {
                    i = hit + 1;
                }
            } else {
                state = CODE;
                prev_non_whitespace = '"';
                word_buffer.clear();
                i = hit + 1;
            }
        } else if (state == STRING_SINGLE) {
            const size_t hit = detail::scan_find_any(s.data(), n, i, "'\\\n", 3);
            if (hit >= n) {
                i = n;
            } else if (s[hit] == '\\') {
                i = (hit + 1 < n) ? hit + 2 : hit + 1;
            } else if (s[hit] == '\'') {
                state = CODE;
                prev_non_whitespace = '\'';
                word_buffer.clear();
                i = hit + 1;
            } else { // '\n'
                state = CODE;
                i = hit + 1;
            }
        } else if (state == BACKTICK_STRING) {
            const size_t hit = detail::scan_find_any(s.data(), n, i, "`\\\n", 3);
            if (hit >= n) {
                i = n;
            } else if (s[hit] == '\\') {
                i = (hit + 1 < n) ? hit + 2 : hit + 1;
            } else if (s[hit] == '`') {
                state = CODE;
                prev_non_whitespace = '`';
                word_buffer.clear();
                i = hit + 1;
            } else { // '\n'
                state = CODE;
                i = hit + 1;
            }
        } else { // REGEX_LITERAL
            const size_t hit = detail::scan_find_any(s.data(), n, i, "/\\\n", 3);
            if (hit >= n) {
                i = n;
            } else if (s[hit] == '\\') {
                i = (hit + 1 < n) ? hit + 2 : hit + 1;
            } else if (s[hit] == '/') {
                state = CODE;
                prev_non_whitespace = '/';
                word_buffer.clear();
                i = hit + 1;
            } else { // '\n'
                state = CODE;
                i = hit + 1;
            }
        }
    }

    // Unclosed comments at end of file
    if (state == LINE_COMMENT) {
        emit(out, start, static_cast<uint32_t>(n), 0);
    } else if (state == BLOCK_COMMENT || state == DOC_COMMENT) {
        emit(out, start, static_cast<uint32_t>(n), state == DOC_COMMENT ? 2u : 1u);
    }
}

// ===========================================================================
// Python (py_parser.py)
// ===========================================================================

// Interesting bytes in the Python CODE state: quote starts and the prefixes
// that can begin a string literal (all other bytes are plain code).
constexpr auto kPyCode = detail::make_set_table(
    {'#', '\'', '"', 'r', 'R', 'b', 'B', 'f', 'F'});

// Interesting bytes inside an f-string expression (no active quote): quote
// starts, brace nesting, and string prefixes.
constexpr auto kPyFexpr = detail::make_set_table(
    {'\'', '"', '{', '}', 'r', 'R', 'b', 'B', 'f', 'F'});

void scan_python(kimix::string_view s, kimix::vector<comment_span>& out) {
    enum : uint8_t {
        CODE, LINE_COMMENT, STRING_SINGLE, STRING_DOUBLE,
        STRING_TRIPLE_SINGLE, STRING_TRIPLE_DOUBLE, FSTRING_EXPR
    };
    const size_t n = s.size();
    size_t i = 0;
    uint8_t state = CODE;

    uint32_t comment_start = 0;
    uint32_t string_start = 0;
    char string_quote = '"';
    bool string_is_fstring = false;
    bool string_has_prefix = false;
    bool escape = false;

    uint32_t fstring_depth = 0;
    char fexpr_string_quote = '\0';
    bool fexpr_string_escape = false;
    uint8_t fexpr_parent_state = CODE;

    auto prefix_char = [](char c) noexcept {
        return c == 'r' || c == 'R' || c == 'b' || c == 'B' ||
               c == 'f' || c == 'F';
    };

    while (i < n) {
        if (state == CODE) {
            // Bulk-skip plain code bytes; only quote/prefix starts can
            // transition, and there is no per-byte state bookkeeping here.
            const size_t hit = detail::scan_find_table(s.data(), n, i, kPyCode);
            i = hit;
            if (i >= n) {
                continue;
            }
            const char ch = s[i];
            const char next = (i + 1 < n) ? s[i + 1] : '\0';
            if (ch == '#') {
                state = LINE_COMMENT;
                comment_start = static_cast<uint32_t>(i);
                i += 1;
            } else if (prefix_char(ch)) {
                // Scan for the full prefix (e.g. "rb", "fr")
                size_t j = i + 1;
                while (j < n && prefix_char(s[j])) {
                    ++j;
                }
                if (j < n && (s[j] == '\'' || s[j] == '"')) {
                    const char quote = s[j];
                    const bool is_f = (ch == 'f' || ch == 'F'); // first char
                    bool is_triple = (j + 2 < n) && s[j] == quote &&
                                     s[j + 1] == quote && s[j + 2] == quote;
                    // NOTE: the reference computes is_f from the whole prefix
                    // (any 'f'/'F' in it); track it below.
                    (void)is_f;
                    string_is_fstring = false;
                    // full-prefix f check: scan the prefix range for f/F
                    for (size_t k = i; k < j; ++k) {
                        if (s[k] == 'f' || s[k] == 'F') {
                            string_is_fstring = true;
                            break;
                        }
                    }
                    if (is_triple) {
                        state = (quote == '\'') ? STRING_TRIPLE_SINGLE : STRING_TRIPLE_DOUBLE;
                        string_start = static_cast<uint32_t>(i);
                        string_quote = quote;
                        string_has_prefix = true;
                        i = j + 3;
                    } else {
                        state = (quote == '\'') ? STRING_SINGLE : STRING_DOUBLE;
                        string_start = static_cast<uint32_t>(i);
                        string_quote = quote;
                        string_has_prefix = true;
                        escape = false;
                        i = j + 1;
                    }
                } else {
                    // Not a string prefix; regular code
                    i += 1;
                }
            } else if (ch == '\'') {
                if (i + 2 < n && s[i + 1] == '\'' && s[i + 2] == '\'') {
                    state = STRING_TRIPLE_SINGLE;
                    string_start = static_cast<uint32_t>(i);
                    string_quote = '\'';
                    string_is_fstring = false;
                    string_has_prefix = false;
                    i += 3;
                } else {
                    state = STRING_SINGLE;
                    string_start = static_cast<uint32_t>(i);
                    string_quote = '\'';
                    string_is_fstring = false;
                    string_has_prefix = false;
                    escape = false;
                    i += 1;
                }
            } else if (ch == '"') {
                if (i + 2 < n && s[i + 1] == '"' && s[i + 2] == '"') {
                    state = STRING_TRIPLE_DOUBLE;
                    string_start = static_cast<uint32_t>(i);
                    string_quote = '"';
                    string_is_fstring = false;
                    string_has_prefix = false;
                    i += 3;
                } else {
                    state = STRING_DOUBLE;
                    string_start = static_cast<uint32_t>(i);
                    string_quote = '"';
                    string_is_fstring = false;
                    string_has_prefix = false;
                    escape = false;
                    i += 1;
                }
            } else {
                i += 1;
            }
        } else if (state == LINE_COMMENT) {
            const size_t nl = find_nl(s, i, n);
            emit(out, comment_start, static_cast<uint32_t>(nl), 0);
            state = CODE;
            i = (nl < n) ? nl + 1 : n;
        } else if (state == STRING_SINGLE || state == STRING_DOUBLE) {
            const char q = (state == STRING_SINGLE) ? '\'' : '"';
            const char needles[4] = {q, '\\', '\n', '{'};
            const size_t hit = detail::scan_find_any(s.data(), n, i, needles, 4);
            if (hit >= n) {
                i = n;
            } else if (s[hit] == '\\') {
                i = (hit + 1 < n) ? hit + 2 : hit + 1;
            } else if (s[hit] == q) {
                state = CODE;
                i = hit + 1;
            } else if (s[hit] == '\n') {
                state = CODE;
                i = hit + 1;
            } else if (s[hit] == '{' && string_is_fstring) {
                if (hit + 1 < n && s[hit + 1] == '{') {
                    i = hit + 2;
                } else {
                    const uint8_t parent = state;
                    state = FSTRING_EXPR;
                    fstring_depth = 0;
                    fexpr_string_quote = '\0';
                    fexpr_string_escape = false;
                    fexpr_parent_state = parent;
                    i = hit + 1;
                }
            } else {
                i = hit + 1;
            }
        } else if (state == STRING_TRIPLE_SINGLE || state == STRING_TRIPLE_DOUBLE) {
            const char q = (state == STRING_TRIPLE_SINGLE) ? '\'' : '"';
            const char needles[3] = {q, '\\', '{'};
            const size_t hit = detail::scan_find_any(s.data(), n, i, needles, 3);
            if (hit >= n) {
                i = n;
            } else if (s[hit] == '\\') {
                i = (hit + 1 < n) ? hit + 2 : hit + 1;
            } else if (s[hit] == '{' && string_is_fstring) {
                if (hit + 1 < n && s[hit + 1] == '{') {
                    i = hit + 2;
                } else {
                    const uint8_t parent = state;
                    state = FSTRING_EXPR;
                    fstring_depth = 0;
                    fexpr_string_quote = '\0';
                    fexpr_string_escape = false;
                    fexpr_parent_state = parent;
                    i = hit + 1;
                }
            } else if (s[hit] == q) {
                if (hit + 1 < n && hit + 2 < n && s[hit + 1] == q &&
                    s[hit + 2] == q) {
                    if (!string_has_prefix) {
                        emit(out, string_start, static_cast<uint32_t>(hit + 3), 2);
                    }
                    state = CODE;
                    i = hit + 3;
                } else {
                    i = hit + 1;
                }
            } else {
                i = hit + 1;
            }
        } else { // FSTRING_EXPR
            if (fexpr_string_escape) {
                fexpr_string_escape = false;
                i += 1;
                continue;
            }
            if (fexpr_string_quote != '\0') {
                // Inside a nested string within the f-string expression.
                const char needles[2] = {fexpr_string_quote, '\\'};
                const size_t hit = detail::scan_find_any(s.data(), n, i, needles, 2);
                if (hit >= n) {
                    i = n;
                } else if (s[hit] == '\\') {
                    i = (hit + 1 < n) ? hit + 2 : hit + 1;
                } else {
                    fexpr_string_quote = '\0';
                    i = hit + 1;
                }
                continue;
            }
            const size_t hit = detail::scan_find_table(s.data(), n, i, kPyFexpr);
            i = hit;
            if (i >= n) {
                continue;
            }
            const char ch = s[i];
            if (prefix_char(ch)) {
                size_t j = i + 1;
                while (j < n && prefix_char(s[j])) {
                    ++j;
                }
                if (j < n && (s[j] == '\'' || s[j] == '"')) {
                    const char quote = s[j];
                    if (j + 2 < n && s[j] == quote && s[j + 1] == quote &&
                        s[j + 2] == quote) {
                        i = j + 3;
                    } else {
                        fexpr_string_quote = quote;
                        i = j + 1;
                    }
                } else {
                    i += 1;
                }
            } else if (ch == '\'' || ch == '"') {
                if (i + 2 < n && s[i + 1] == ch && s[i + 2] == ch) {
                    i += 3;
                } else {
                    fexpr_string_quote = ch;
                    i += 1;
                }
            } else if (ch == '{') {
                fstring_depth += 1;
                i += 1;
            } else if (ch == '}') {
                if (fstring_depth == 0) {
                    state = fexpr_parent_state;
                } else {
                    fstring_depth -= 1;
                }
                i += 1;
            } else {
                i += 1; // '#' inside an f-string expression is NOT a comment
            }
        }
    }

    if (state == LINE_COMMENT) {
        emit(out, comment_start, static_cast<uint32_t>(n), 0);
    }
    if ((state == STRING_TRIPLE_SINGLE || state == STRING_TRIPLE_DOUBLE) &&
        !string_has_prefix) {
        emit(out, string_start, static_cast<uint32_t>(n), 2);
    }
}

// ===========================================================================
// Shell / Bash (shell_parser.py)
// ===========================================================================

// Interesting bytes in the shell CODE state ('\n' is included for line
// counting; nothing else is per-byte state), reused by the BACKTICK and
// DOLLAR_PAREN substitution states with their extra terminators.
constexpr auto kShellCode = detail::make_set_table(
    {'#', '\'', '"', '`', '$', '<', '\n'});
constexpr auto kShellBacktick = detail::make_set_table(
    {'\\', '`', '#', '\'', '"', '$', '<', '\n'});
constexpr auto kShellDollarParen = detail::make_set_table(
    {'(', ')', '#', '\'', '"', '`', '$', '<', '\n'});

void scan_shell(kimix::string_view s, kimix::vector<comment_span>& out) {
    enum : uint8_t {
        CODE, LINE_COMMENT, STRING_SINGLE, STRING_DOUBLE, HEREDOC, BACKTICK,
        DOLLAR_PAREN
    };
    const size_t n = s.size();
    size_t i = 0;
    uint8_t state = CODE;
    uint32_t line = 1;

    uint32_t comment_start = 0;
    uint32_t comment_kind = 0;

    bool string_escape = false;

    kimix::string heredoc_delimiter;
    bool heredoc_allow_tab = false;
    kimix::string heredoc_line;
    uint8_t heredoc_return_state = CODE;

    uint32_t dp_depth = 0;
    uint8_t dp_return_state = CODE;

    uint8_t bt_return_state = CODE;

    auto start_heredoc = [&](size_t j, uint8_t return_state) {
        // j points at the first char after "<<"
        size_t k = j;
        bool allow_tab = false;
        if (k < n && s[k] == '-') {
            allow_tab = true;
            ++k;
        }
        while (k < n && (s[k] == ' ' || s[k] == '\t')) {
            ++k;
        }
        kimix::string delimiter;
        if (k < n && (s[k] == '\'' || s[k] == '"')) {
            const char q = s[k];
            ++k;
            while (k < n && s[k] != q) {
                delimiter.push_back(s[k]);
                ++k;
            }
            if (k < n) {
                ++k;
            }
        } else {
            while (k < n && s[k] != ' ' && s[k] != '\t' && s[k] != '\n') {
                delimiter.push_back(s[k]);
                ++k;
            }
        }
        if (!delimiter.empty()) {
            state = HEREDOC;
            heredoc_delimiter = delimiter;
            heredoc_allow_tab = allow_tab;
            heredoc_line.clear();
            heredoc_return_state = return_state;
            i = k;
            return true;
        }
        return false;
    };

    while (i < n) {
        if (state == CODE) {
            // Bulk-skip plain code bytes; '#' / quotes / '$(' / '<<' / '\n'
            // are the only per-byte events and need no accumulation state.
            const size_t hit = detail::scan_find_table(s.data(), n, i, kShellCode);
            i = hit;
            if (i >= n) {
                continue;
            }
            const char ch = s[i];
            const char next = (i + 1 < n) ? s[i + 1] : '\0';
            if (ch == '#') {
                state = LINE_COMMENT;
                comment_start = static_cast<uint32_t>(i);
                comment_kind = (line == 1 && next == '!') ? 2u : 0u;
                i += 1;
            } else if (ch == '\'') {
                state = STRING_SINGLE;
                i += 1;
            } else if (ch == '"') {
                state = STRING_DOUBLE;
                string_escape = false;
                i += 1;
            } else if (ch == '`') {
                state = BACKTICK;
                bt_return_state = CODE;
                string_escape = false;
                i += 1;
            } else if (ch == '$' && next == '(') {
                state = DOLLAR_PAREN;
                dp_depth = 1;
                dp_return_state = CODE;
                i += 2;
            } else if (ch == '<' && next == '<') {
                if (i + 2 < n && s[i + 2] == '<') {
                    i += 3; // <<< here-string, not a heredoc
                } else {
                    if (!start_heredoc(i + 2, CODE)) {
                        i += 1;
                    }
                }
            } else {
                if (ch == '\n') {
                    line += 1;
                }
                i += 1;
            }
        } else if (state == LINE_COMMENT) {
            const size_t nl = find_nl(s, i, n);
            emit(out, comment_start, static_cast<uint32_t>(nl), comment_kind);
            state = CODE;
            if (nl < n) {
                line += 1;
                i = nl + 1;
            } else {
                i = n;
            }
        } else if (state == STRING_SINGLE) {
            const size_t hit = detail::scan_find_any(s.data(), n, i, "'\n", 2);
            if (hit >= n) {
                i = n;
            } else if (s[hit] == '\'') {
                state = CODE;
                i = hit + 1;
            } else { // '\n'
                state = CODE;
                line += 1;
                i = hit + 1;
            }
        } else if (state == STRING_DOUBLE) {
            const size_t hit = detail::scan_find_any(s.data(), n, i, "\"\\$\n`", 5);
            if (hit >= n) {
                i = n;
            } else if (s[hit] == '\\') {
                i = (hit + 1 < n) ? hit + 2 : hit + 1;
            } else if (s[hit] == '"') {
                state = CODE;
                i = hit + 1;
            } else if (s[hit] == '$' && hit + 1 < n && s[hit + 1] == '(') {
                state = DOLLAR_PAREN;
                dp_depth = 1;
                dp_return_state = STRING_DOUBLE;
                i = hit + 2;
            } else if (s[hit] == '`') {
                state = BACKTICK;
                bt_return_state = STRING_DOUBLE;
                string_escape = false;
                i = hit + 1;
            } else { // '\n'
                state = CODE;
                line += 1;
                i = hit + 1;
            }
        } else if (state == HEREDOC) {
            const size_t nl = find_nl(s, i, n);
            if (nl > i) {
                heredoc_line.append(s.data() + i, nl - i);
            }
            if (nl < n) {
                kimix::string_view stripped(heredoc_line);
                if (heredoc_allow_tab) {
                    size_t t = 0;
                    while (t < stripped.size() && stripped[t] == '\t') {
                        ++t;
                    }
                    stripped = stripped.substr(t);
                }
                if (stripped == heredoc_delimiter) {
                    state = heredoc_return_state;
                }
                heredoc_line.clear();
                line += 1;
                i = nl + 1;
            } else {
                // EOF inside a heredoc: the residue is data, no comment.
                i = n;
            }
        } else if (state == BACKTICK) {
            if (string_escape) {
                string_escape = false;
                i += 1;
                continue;
            }
            const size_t hit = detail::scan_find_table(s.data(), n, i, kShellBacktick);
            i = hit;
            if (i >= n) {
                continue;
            }
            const char ch = s[i];
            const char next = (i + 1 < n) ? s[i + 1] : '\0';
            if (ch == '\\') {
                i += (i + 1 < n) ? 2 : 1;
            } else if (ch == '`') {
                state = bt_return_state;
                i += 1;
            } else if (ch == '#') {
                state = LINE_COMMENT;
                comment_start = static_cast<uint32_t>(i);
                comment_kind = 0;
                i += 1;
            } else if (ch == '\'') {
                state = STRING_SINGLE;
                i += 1;
            } else if (ch == '"') {
                state = STRING_DOUBLE;
                string_escape = false;
                i += 1;
            } else if (ch == '$' && next == '(') {
                state = DOLLAR_PAREN;
                dp_depth = 1;
                dp_return_state = BACKTICK;
                i += 2;
            } else if (ch == '<' && next == '<') {
                if (i + 2 < n && s[i + 2] == '<') {
                    i += 3;
                } else {
                    if (!start_heredoc(i + 2, BACKTICK)) {
                        i += 1;
                    }
                }
            } else {
                if (ch == '\n') {
                    line += 1;
                }
                i += 1;
            }
        } else { // DOLLAR_PAREN
            const size_t hit = detail::scan_find_table(s.data(), n, i, kShellDollarParen);
            i = hit;
            if (i >= n) {
                continue;
            }
            const char ch = s[i];
            const char next = (i + 1 < n) ? s[i + 1] : '\0';
            if (ch == '(') {
                dp_depth += 1;
                i += 1;
            } else if (ch == ')') {
                dp_depth -= 1;
                if (dp_depth == 0) {
                    state = dp_return_state;
                }
                i += 1;
            } else if (ch == '#') {
                state = LINE_COMMENT;
                comment_start = static_cast<uint32_t>(i);
                comment_kind = 0;
                i += 1;
            } else if (ch == '\'') {
                state = STRING_SINGLE;
                i += 1;
            } else if (ch == '"') {
                state = STRING_DOUBLE;
                string_escape = false;
                i += 1;
            } else if (ch == '`') {
                state = BACKTICK;
                bt_return_state = DOLLAR_PAREN;
                string_escape = false;
                i += 1;
            } else if (ch == '$' && next == '(') {
                dp_depth += 1;
                i += 2;
            } else if (ch == '<' && next == '<') {
                if (i + 2 < n && s[i + 2] == '<') {
                    i += 3;
                } else {
                    if (!start_heredoc(i + 2, DOLLAR_PAREN)) {
                        i += 1;
                    }
                }
            } else {
                if (ch == '\n') {
                    line += 1;
                }
                i += 1;
            }
        }
    }

    if (state == LINE_COMMENT) {
        emit(out, comment_start, static_cast<uint32_t>(n), comment_kind);
    }
    // HEREDOC at EOF: the accumulated line may close the heredoc; no comment
    // is ever emitted for heredoc state, so nothing else to do.
}

// ===========================================================================
// SQL (sql_parser.py)
// ===========================================================================

// Interesting bytes in the SQL CODE state ('-' '-' only starts a comment when
// followed by whitespace; the other bytes start strings / block comments).
constexpr auto kSqlCode = detail::make_set_table(
    {'-', '#', '/', '\'', '"', '`'});

// Inside a SQL block comment only '/' and '*' can change the nesting depth.
constexpr auto kSqlBlock = detail::make_set_table({'/', '*'});

void scan_sql(kimix::string_view s, kimix::vector<comment_span>& out) {
    enum : uint8_t {
        CODE, LINE_COMMENT_DASH, LINE_COMMENT_HASH, BLOCK_COMMENT,
        STRING_SINGLE, ID_DOUBLE, ID_BACKTICK
    };
    const size_t n = s.size();
    size_t i = 0;
    uint8_t state = CODE;
    uint32_t start = 0;
    uint32_t block_depth = 0;

    while (i < n) {
        if (state == CODE) {
            // Bulk-skip plain code bytes; no accumulation state in CODE.
            const size_t hit = detail::scan_find_table(s.data(), n, i, kSqlCode);
            i = hit;
            if (i >= n) {
                continue;
            }
            const char ch = s[i];
            const char next = (i + 1 < n) ? s[i + 1] : '\0';
            // -- line comment (requires space/tab/newline/CR/EOF after --)
            if (ch == '-' && next == '-') {
                const char after = (i + 2 < n) ? s[i + 2] : '\0';
                if (after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
                    after == '\0') {
                    start = static_cast<uint32_t>(i + 2);
                    state = LINE_COMMENT_DASH;
                    i += 2;
                    continue;
                }
                // otherwise: two dashes in code
            }
            if (ch == '#') {
                start = static_cast<uint32_t>(i + 1);
                state = LINE_COMMENT_HASH;
                i += 1;
                continue;
            }
            if (ch == '/' && next == '*') {
                start = static_cast<uint32_t>(i + 2);
                block_depth = 1;
                state = BLOCK_COMMENT;
                i += 2;
                continue;
            }
            if (ch == '\'') {
                state = STRING_SINGLE;
                i += 1;
                continue;
            }
            if (ch == '"') {
                state = ID_DOUBLE;
                i += 1;
                continue;
            }
            if (ch == '`') {
                state = ID_BACKTICK;
                i += 1;
                continue;
            }
            i += 1;
        } else if (state == LINE_COMMENT_DASH || state == LINE_COMMENT_HASH) {
            const size_t nl = find_nl(s, i, n);
            emit(out, start, static_cast<uint32_t>(nl), 0);
            state = CODE;
            i = (nl < n) ? nl + 1 : n;
        } else if (state == BLOCK_COMMENT) {
            // Only '/' and '*' can open/close a nesting level.
            const size_t hit = detail::scan_find_table(s.data(), n, i, kSqlBlock);
            i = hit;
            if (i >= n) {
                continue;
            }
            if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
                block_depth += 1;
                i += 2;
                continue;
            }
            if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') {
                block_depth -= 1;
                if (block_depth == 0) {
                    emit(out, start, static_cast<uint32_t>(i), 1);
                    state = CODE;
                }
                i += 2;
                continue;
            }
            i += 1;
        } else if (state == STRING_SINGLE) {
            const size_t hit = detail::scan_find_any(s.data(), n, i, "'\\\n", 3);
            if (hit >= n) {
                i = n;
            } else if (s[hit] == '\'' && hit + 1 < n && s[hit + 1] == '\'') {
                i = hit + 2;
            } else if (s[hit] == '\\') {
                i = (hit + 1 < n) ? hit + 2 : hit + 1;
            } else if (s[hit] == '\'') {
                state = CODE;
                i = hit + 1;
            } else { // '\n'
                state = CODE;
                i = hit + 1;
            }
        } else if (state == ID_DOUBLE) {
            const size_t hit = detail::scan_find_any(s.data(), n, i, "\"\\\n", 3);
            if (hit >= n) {
                i = n;
            } else if (s[hit] == '"' && hit + 1 < n && s[hit + 1] == '"') {
                i = hit + 2;
            } else if (s[hit] == '\\') {
                i = (hit + 1 < n) ? hit + 2 : hit + 1;
            } else if (s[hit] == '"') {
                state = CODE;
                i = hit + 1;
            } else { // '\n'
                state = CODE;
                i = hit + 1;
            }
        } else { // ID_BACKTICK
            const size_t hit = detail::scan_find_any(s.data(), n, i, "`\\\n", 3);
            if (hit >= n) {
                i = n;
            } else if (s[hit] == '`' && hit + 1 < n && s[hit + 1] == '`') {
                i = hit + 2;
            } else if (s[hit] == '\\') {
                i = (hit + 1 < n) ? hit + 2 : hit + 1;
            } else if (s[hit] == '`') {
                state = CODE;
                i = hit + 1;
            } else { // '\n'
                state = CODE;
                i = hit + 1;
            }
        }
    }

    // Unclosed comments at end of file
    if (state == LINE_COMMENT_DASH || state == LINE_COMMENT_HASH) {
        emit(out, start, static_cast<uint32_t>(n), 0);
    } else if (state == BLOCK_COMMENT) {
        emit(out, start, static_cast<uint32_t>(n), 1);
    }
}

// ===========================================================================
// HTML / XML (html_parser.py)
// ===========================================================================

// Interesting bytes in the HTML CODE state: tag/comment starts and attribute
// quotes (all other bytes are plain markup text).
constexpr auto kHtmlCode = detail::make_set_table({'<', '"', '\''});

void scan_html(kimix::string_view s, kimix::vector<comment_span>& out) {
    enum : uint8_t { CODE, COMMENT, PI, CDATA, ATTR_DOUBLE, ATTR_SINGLE };
    const size_t n = s.size();
    size_t i = 0;
    uint8_t state = CODE;
    uint32_t start = 0;

    while (i < n) {
        if (state == CODE) {
            // Bulk-skip plain markup; '<' is the only construct starter.
            const size_t hit = detail::scan_find_table(s.data(), n, i, kHtmlCode);
            i = hit;
            if (i >= n) {
                continue;
            }
            const char ch = s[i];
            if (ch == '<' && i + 3 < n && s[i] == '<' && s[i + 1] == '!' &&
                s[i + 2] == '-' && s[i + 3] == '-') {
                state = COMMENT;
                start = static_cast<uint32_t>(i + 4);
                i += 4;
                continue;
            }
            if (ch == '<' && i + 1 < n && s[i + 1] == '?') {
                state = PI;
                start = static_cast<uint32_t>(i + 2);
                i += 2;
                continue;
            }
            if (ch == '<' && i + 8 < n && s[i + 1] == '!' && s[i + 2] == '[' &&
                s[i + 3] == 'C' && s[i + 4] == 'D' && s[i + 5] == 'A' &&
                s[i + 6] == 'T' && s[i + 7] == 'A' && s[i + 8] == '[') {
                state = CDATA;
                i += 9;
                continue;
            }
            if (ch == '"') {
                state = ATTR_DOUBLE;
                i += 1;
                continue;
            }
            if (ch == '\'') {
                state = ATTR_SINGLE;
                i += 1;
                continue;
            }
            i += 1;
        } else if (state == COMMENT) {
            const size_t close = detail::scan_find_sub(s.data(), n, i, "-->", 3);
            if (close < n) {
                emit(out, start, static_cast<uint32_t>(close), 1);
                state = CODE;
                i = close + 3;
            } else {
                i = n;
            }
        } else if (state == PI) {
            const size_t close = detail::scan_find_sub(s.data(), n, i, "?>", 2);
            if (close < n) {
                emit(out, start, static_cast<uint32_t>(close), 2);
                state = CODE;
                i = close + 2;
            } else {
                i = n;
            }
        } else if (state == CDATA) {
            const size_t close = detail::scan_find_sub(s.data(), n, i, "]]>", 3);
            if (close < n) {
                state = CODE;
                i = close + 3;
            } else {
                i = n;
            }
        } else if (state == ATTR_DOUBLE) {
            const size_t close = detail::scan_find_char(s.data(), n, i, '"');
            state = CODE;
            i = close + 1;
        } else { // ATTR_SINGLE
            const size_t close = detail::scan_find_char(s.data(), n, i, '\'');
            state = CODE;
            i = close + 1;
        }
    }
    // HTML: unclosed <!-- or <? at EOF emits NO comment (reference behavior)
}

// ===========================================================================
// Lisp / Assembly (lisp_parser.py)
// ===========================================================================

// Interesting bytes in the Lisp CODE state: '#' starts character literals /
// block comments, ';' line comments, '"' strings.
constexpr auto kLispCode = detail::make_set_table({'#', ';', '"'});

void scan_lisp(kimix::string_view s, kimix::vector<comment_span>& out) {
    enum : uint8_t { CODE, LINE_COMMENT, BLOCK_COMMENT, STRING };
    const size_t n = s.size();
    size_t i = 0;
    uint8_t state = CODE;
    uint32_t start = 0;
    bool string_escape = false;

    while (i < n) {
        if (state == CODE) {
            // Bulk-skip plain code bytes; no per-byte bookkeeping in CODE.
            const size_t hit = detail::scan_find_table(s.data(), n, i, kLispCode);
            i = hit;
            if (i >= n) {
                continue;
            }
            const char ch = s[i];
            const char next = (i + 1 < n) ? s[i + 1] : '\0';
            // Character literal #\X (e.g. #\; is NOT a comment start)
            if (ch == '#' && next == '\\') {
                i += 2;
                if (i < n) {
                    const char c0 = s[i];
                    i += 1;
                    if (ascii_alpha(c0)) {
                        while (i < n && ascii_alpha(s[i])) {
                            ++i;
                        }
                    }
                }
                continue;
            }
            // Block comment #| ... |# (content includes both markers)
            if (ch == '#' && next == '|') {
                start = static_cast<uint32_t>(i);
                state = BLOCK_COMMENT;
                i += 2;
                continue;
            }
            // Line comment ;
            if (ch == ';') {
                start = static_cast<uint32_t>(i);
                state = LINE_COMMENT;
                i += 1;
                continue;
            }
            if (ch == '"') {
                state = STRING;
                string_escape = false;
                i += 1;
                continue;
            }
            i += 1;
        } else if (state == LINE_COMMENT) {
            const size_t nl = find_nl(s, i, n);
            emit(out, start, static_cast<uint32_t>(nl), 0);
            state = CODE;
            i = (nl < n) ? nl + 1 : n;
        } else if (state == BLOCK_COMMENT) {
            const size_t close = detail::scan_find_sub(s.data(), n, i, "|#", 2);
            if (close < n) {
                emit(out, start, static_cast<uint32_t>(close + 2), 1);
                state = CODE;
                i = close + 2;
            } else {
                i = n;
            }
        } else { // STRING
            if (string_escape) {
                string_escape = false;
                i += 1;
                continue;
            }
            const size_t hit = detail::scan_find_any(s.data(), n, i, "\"\\\n", 3);
            if (hit >= n) {
                i = n;
            } else if (s[hit] == '\\') {
                i = (hit + 1 < n) ? hit + 2 : hit + 1;
            } else { // '"' or '\n' both return to CODE
                state = CODE;
                i = hit + 1;
            }
        }
    }

    if (state == LINE_COMMENT) {
        emit(out, start, static_cast<uint32_t>(n), 0);
    } else if (state == BLOCK_COMMENT) {
        emit(out, start, static_cast<uint32_t>(n), 1);
    }
}

// ===========================================================================
// Pascal / Delphi (pascal_parser.py)
// ===========================================================================

// Interesting bytes in the Pascal CODE state: string quotes and the three
// comment openers.
constexpr auto kPascalCode = detail::make_set_table({'\'', '/', '(', '{'});

void scan_pascal(kimix::string_view s, kimix::vector<comment_span>& out) {
    enum : uint8_t { CODE, BRACE_COMMENT, PAREN_STAR_COMMENT, LINE_COMMENT, STRING };
    const size_t n = s.size();
    size_t i = 0;
    uint8_t state = CODE;
    uint32_t start = 0;

    while (i < n) {
        if (state == CODE) {
            // Bulk-skip plain code bytes; no per-byte bookkeeping in CODE.
            const size_t hit = detail::scan_find_table(s.data(), n, i, kPascalCode);
            i = hit;
            if (i >= n) {
                continue;
            }
            const char ch = s[i];
            const char next = (i + 1 < n) ? s[i + 1] : '\0';
            if (ch == '\'') {
                state = STRING;
                i += 1;
                continue;
            }
            if (ch == '/' && next == '/') {
                start = static_cast<uint32_t>(i + 2);
                state = LINE_COMMENT;
                i += 2;
                continue;
            }
            if (ch == '(' && next == '*') {
                start = static_cast<uint32_t>(i + 2);
                state = PAREN_STAR_COMMENT;
                i += 2;
                continue;
            }
            if (ch == '{') {
                start = static_cast<uint32_t>(i + 1);
                state = BRACE_COMMENT;
                i += 1;
                continue;
            }
            i += 1;
        } else if (state == BRACE_COMMENT) {
            const size_t close = detail::scan_find_char(s.data(), n, i, '}');
            emit(out, start, static_cast<uint32_t>(close), 1);
            state = CODE;
            i = (close < n) ? close + 1 : n;
        } else if (state == PAREN_STAR_COMMENT) {
            const size_t close = detail::scan_find_sub(s.data(), n, i, "*)", 2);
            if (close < n) {
                emit(out, start, static_cast<uint32_t>(close), 1);
                state = CODE;
                i = close + 2;
            } else {
                i = n;
            }
        } else if (state == LINE_COMMENT) {
            const size_t nl = find_nl(s, i, n);
            emit(out, start, static_cast<uint32_t>(nl), 0);
            state = CODE;
            i = (nl < n) ? nl + 1 : n;
        } else { // STRING
            const size_t hit = detail::scan_find_any(s.data(), n, i, "'\n", 2);
            if (hit >= n) {
                i = n;
            } else if (s[hit] == '\'' && hit + 1 < n && s[hit + 1] == '\'') {
                i = hit + 2;
            } else { // '\'' or '\n' both return to CODE
                state = CODE;
                i = hit + 1;
            }
        }
    }

    if (state == BRACE_COMMENT || state == PAREN_STAR_COMMENT) {
        emit(out, start, static_cast<uint32_t>(n), 1);
    } else if (state == LINE_COMMENT) {
        emit(out, start, static_cast<uint32_t>(n), 0);
    }
}

} // namespace

lang_rules rules_for(lang_kind lang) noexcept {
    switch (lang) {
    case lang_kind::C:
        return lang_rules{true, true, true, true, true};
    case lang_kind::PYTHON:
        return lang_rules{true, false, true, true, true};
    case lang_kind::SHELL:
        return lang_rules{true, false, true, false, true};
    case lang_kind::SQL:
        return lang_rules{true, true, true, false, true};
    case lang_kind::HTML:
        return lang_rules{false, true, true, false, true};
    case lang_kind::LISP:
        return lang_rules{true, true, true, false, false};
    case lang_kind::PASCAL_LANG:
        return lang_rules{true, true, true, false, false};
    }
    return lang_rules{false, false, false, false, false};
}

void scan_comments(lang_kind lang, kimix::string_view input,
                   kimix::vector<comment_span>& out) {
    out.clear();
    // Heuristic pre-reserve so dense-comment inputs do not grow the span
    // vector one doubling at a time (capacity is reused across calls).
    out.reserve(input.size() / 4 + 16);
    switch (lang) {
    case lang_kind::C: scan_c(input, out); break;
    case lang_kind::PYTHON: scan_python(input, out); break;
    case lang_kind::SHELL: scan_shell(input, out); break;
    case lang_kind::SQL: scan_sql(input, out); break;
    case lang_kind::HTML: scan_html(input, out); break;
    case lang_kind::LISP: scan_lisp(input, out); break;
    case lang_kind::PASCAL_LANG: scan_pascal(input, out); break;
    }
}

} // namespace parse
} // namespace runtime
} // namespace kimix
