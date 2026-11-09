/*
 * shell_scanner.cpp - Bash / PowerShell command scanners (plan 012).
 *
 * Faithful ports of the kimi-agent reference scanners:
 *   bash_fix.py::_Scanner        (BASH_FIX)
 *   bash_tool.py::_process_unquoted (BASH_PROCESS_UNQUOTED)
 *   pwsh_fix.py::_Scanner.fix    (PWSH_FIX)
 *   process_pwsh.py::pwsh_transform (PWSH_TRANSFORM)
 *
 * All scanners are depth-bounded (1024, the reference _MAX_NESTING_DEPTH).
 * Fallback-command edits (BASH_FIX) use a marker replacement "\x01<name>\x01"
 * that the Python shim expands to the wrapper runner (the fallback definition
 * data lives in the shim).  The BASH_FIX *name* tables (fallback names,
 * fallback command wrappers and the unsupported set) are generated from
 * kimi-agent's bin/kimix_native/_shell_compat.py - see the
 * GENERATED:BASH-FIX-PARSE-DATA region and scripts/gen_bash_fix_data.py
 * --tables-runtime.  ASCII-only character classes where the reference
 * uses str.isalnum()/isalpha() (PWSH_FIX token boundary); the shim routes
 * non-ASCII input to the _compat mirror for those dialects.
 */

#include <runtime/parse/ascii_util.h>
#include <runtime/parse/shell_scanner.h>

#include <cstring>
#include <utility>

namespace kimix {
namespace runtime {
namespace parse {

namespace src_runtime_parse_shell_scanner {

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

// ASCII char-class helpers live in the shared ascii_util.h header
// (kimix::runtime::parse::detail). They were previously defined here AND in
// comment_scanner.cpp, which collided when a unity build merged both TUs.
using namespace detail;

// Bash/PowerShell whitespace: space, tab, CR only (shell-specific: LF is a
// command terminator, not skipped whitespace).
inline bool is_ws(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r';
}

// Interesting bytes inside a read_word() word: word terminators plus the
// quoting/expansion starts that the per-char walk handles specially. '#'
// is handled before the loop (it only ends a word when it is the first byte).
constexpr auto kWordSpecial = detail::make_set_table(
    {' ', '\t', '\r', '\n', ';', '&', '|', '(', ')', '<', '>',
     '\\', '\'', '"', '`', '$'});

// Interesting bytes inside a double-quoted region: escape, close quote and
// the expansion starts that trigger nested scanning.
constexpr auto kDqSpecial = detail::make_set_table({'\\', '"', '`', '$'});

// Bytes that terminate or escape plain code regions inside $(...)-style
// substitution scanners (scan_expansions / scan_heredoc_expansions).
constexpr auto kExpSpecial = detail::make_set_table({'\\', '$', '\'', '"', '`'});

// Interesting bytes in the PWSH_FIX NORMAL mode: everything that does not hit
// the "plain code" else-branch. Skipping plain-code runs is exact because the
// only side effect of the else-branch is saw_code = true.
constexpr auto kPwshNormal = detail::make_set_table(
    {'"', '\'', '`', '#', '<', '@', '-', '$',
     ' ', '\t', '\n', '\r', '\v', '\f'});

// Interesting bytes inside $(...) / $((...)) subexpression scans of the
// PWSH_FIX scanner (quotes, escapes, nesting, comments).
constexpr auto kPwshSubexpr = detail::make_set_table(
    {'(', ')', '\'', '"', '`', '#', '<'});

void apply_edits(kimix::string_view cmd, const kimix::vector<edit>& edits,
                 kimix::string* out) {
    if (out == nullptr) {
        return;
    }
    out->clear();
    size_t previous = 0;
    for (const edit& e : edits) {
        if (e.start < previous) {
            continue; // defensive: overlapping edits (should not happen)
        }
        out->append(cmd.data() + previous, e.start - previous);
        out->append(e.replacement);
        previous = e.end;
    }
    out->append(cmd.data() + previous, cmd.size() - previous);
}

// ===========================================================================
// BASH_PROCESS_UNQUOTED - bash_tool.py::_process_unquoted
// ===========================================================================

bool bash_metachar(char c) noexcept {
    switch (c) {
    case '(': case ')': case '|': case ';': case '&': case '<': case '>':
    case '$': case '"': case '`': case '\'': case '*': case '?': case '[':
    case ']': case '{': case '}': case '~': case '!': case '#': case '=':
    case '%': case ' ': case '\t': case '\n': case '\r':
        return true;
    default:
        return false;
    }
}

bool dq_escaped(char c) noexcept { return c == '"' || c == '\\' || c == '$' || c == '`'; }

bool unquoted_special(char c) noexcept {
    return c == '\\' || c == '\'' || c == '"' || c == '$' || c == '`';
}

struct ProcessUnquoted {
    kimix::string_view cmd;
    size_t n = 0;
    kimix::vector<edit>* edits = nullptr;

    // index AFTER the closing quote of a $'...' region (or n when unterminated)
    size_t find_ansi_c_end(size_t start) const noexcept {
        size_t i = start;
        while (i < n) {
            const char c = cmd[i];
            if (c == '\\' && i + 1 < n) {
                i += 2;
            } else if (c == '\'') {
                return i + 1;
            } else {
                i += 1;
            }
        }
        return n + 1;
    }

    // index AFTER the closing backtick (reference bash_tool._find_backtick_end)
    // or n + 1 when unterminated (the reference returns -1; n + 1 keeps the
    // "terminated at the last char" case distinguishable from unterminated).
    size_t find_backtick_end(size_t start) const noexcept {
        size_t i = start;
        while (i < n) {
            const char c = cmd[i];
            if (c == '\\' && i + 1 < n) {
                i += 2;
            } else if (c == '`') {
                return i + 1;
            } else {
                i += 1;
            }
        }
        return n + 1;
    }

    // index of the ')' matching the '(' at open_pos (or n)
    size_t find_matching_paren(size_t open_pos) const noexcept {
        size_t depth = 1;
        size_t i = open_pos + 1;
        while (i < n) {
            const char c = cmd[i];
            if (c == '\'') {
                const size_t end = cmd.find('\'', i + 1);
                if (end == kimix::string_view::npos) {
                    return n + 1;
                }
                i = end + 1;
            } else if (c == '"') {
                const size_t end = find_dq_end(i + 1);
                if (end > n) {
                    return n + 1;
                }
                i = end;
            } else if (c == '`') {
                const size_t end = find_backtick_end(i + 1);
                if (end > n) {
                    return n;
                }
                i = end;
            } else if (c == '$' && i + 1 < n && cmd[i + 1] == '(') {
                depth += 1;
                i += 2;
            } else if (c == '$' && i + 1 < n && cmd[i + 1] == '\'') {
                const size_t end = find_ansi_c_end(i + 2);
                if (end > n) {
                    return n + 1;
                }
                i = end;
            } else if (c == ')') {
                depth -= 1;
                if (depth == 0) {
                    return i;
                }
                i += 1;
            } else {
                i += 1;
            }
        }
        return n + 1;
    }

    // index AFTER the closing " of a double-quoted region (or n + 1 when
    // unterminated, mirroring the reference's -1)
    size_t find_dq_end(size_t start) const noexcept {
        size_t i = start;
        while (i < n) {
            const char c = cmd[i];
            if (c == '\\' && i + 1 < n && dq_escaped(cmd[i + 1])) {
                i += 2;
            } else if (c == '"') {
                return i + 1;
            } else if (c == '$' && i + 1 < n && cmd[i + 1] == '(') {
                const size_t end = find_matching_paren(i + 1);
                if (end > n) {
                    return n + 1;
                }
                i = end + 1;
            } else if (c == '$' && i + 1 < n && cmd[i + 1] == '\'') {
                const size_t end = find_ansi_c_end(i + 2);
                if (end > n) {
                    return n + 1;
                }
                i = end;
            } else if (c == '`') {
                const size_t end = find_backtick_end(i + 1);
                if (end > n) {
                    return n;
                }
                i = end;
            } else {
                i += 1;
            }
        }
        return n + 1;
    }

    // Core walk over [0, n) mirroring _process_unquoted. `depth` bounds
    // recursion into $(...) / backtick substitutions (reference recursion
    // limit; on overflow the remainder is copied verbatim).
    void process(size_t depth) {
        size_t i = 0;
        while (i < n) {
            // find the next special character
            size_t nxt = i;
            while (nxt < n && !unquoted_special(cmd[nxt])) {
                ++nxt;
            }
            i = nxt;
            if (i >= n) {
                break;
            }
            const char ch = cmd[i];

            if (ch == '\'') {
                const size_t end = cmd.find('\'', i + 1);
                if (end == kimix::string_view::npos || end >= n) {
                    break; // unterminated: copy the rest verbatim
                }
                i = end + 1;
            } else if (ch == '"') {
                const size_t dq_end = find_dq_end(i + 1);
                if (dq_end > n) {
                    break; // unterminated: copy the rest verbatim
                }
                size_t j = i + 1;
                while (j < dq_end) {
                    size_t nxt2 = j;
                    while (nxt2 < dq_end && !unquoted_special(cmd[nxt2])) {
                        ++nxt2;
                    }
                    j = nxt2;
                    if (j >= dq_end) {
                        break;
                    }
                    const char c = cmd[j];
                    if (c == '\\' && j + 1 < dq_end && dq_escaped(cmd[j + 1])) {
                        j += 2;
                    } else if (c == '$' && j + 1 < dq_end && cmd[j + 1] == '(') {
                        const size_t paren_end = find_matching_paren(j + 1);
                        if (paren_end > n || paren_end >= dq_end) {
                            j = dq_end;
                            break;
                        }
                        if (depth < 1024) {
                            ProcessUnquoted sub{cmd, n, edits};
                            sub.process_range(j + 2, paren_end, depth + 1);
                        }
                        j = paren_end + 1;
                    } else if (c == '$' && j + 1 < dq_end && cmd[j + 1] == '\'') {
                        const size_t ac_end = find_ansi_c_end(j + 2);
                        if (ac_end > n || ac_end > dq_end) {
                            j = dq_end;
                            break;
                        }
                        j = ac_end;
                    } else if (c == '`') {
                        const size_t bt_end = find_backtick_end(j + 1);
                        if (bt_end > n || bt_end > dq_end) {
                            j = dq_end;
                            break;
                        }
                        if (depth < 1024) {
                            ProcessUnquoted sub{cmd, n, edits};
                            sub.process_range(j + 1, bt_end - 1, depth + 1);
                        }
                        j = bt_end;
                    } else {
                        j += 1;
                    }
                }
                i = dq_end;
            } else if (ch == '$' && i + 1 < n && cmd[i + 1] == '\'') {
                const size_t ac_end = find_ansi_c_end(i + 2);
                if (ac_end > n) {
                    break;
                }
                i = ac_end;
            } else if (ch == '`') {
                const size_t bt_end = find_backtick_end(i + 1);
                if (bt_end > n) {
                    break;
                }
                if (depth < 1024) {
                    ProcessUnquoted sub{cmd, n, edits};
                    sub.process_range(i + 1, bt_end - 1, depth + 1);
                }
                i = bt_end;
            } else if (ch == '\\') {
                if (i + 1 < n && bash_metachar(cmd[i + 1])) {
                    i += 2; // backslash escaping a metachar: preserve both
                } else {
                    edits->push_back(edit{static_cast<uint32_t>(i),
                                          static_cast<uint32_t>(i + 1),
                                          kimix::string("/")});
                    i += 1;
                }
            } else {
                i += 1;
            }
        }
    }

    // Walk [from, to) as the reference's recursive calls do (substring slice).
    // Edits produced inside the sub-range are offset back to the parent's
    // coordinate space before being merged.
    void process_range(size_t from, size_t to, size_t depth) {
        if (to <= from) {
            return;
        }
        kimix::vector<edit> sub;
        ProcessUnquoted sub_scanner{kimix::string_view(cmd.data() + from, to - from),
                                    to - from, &sub};
        sub_scanner.process(depth);
        for (edit& e : sub) {
            e.start += static_cast<uint32_t>(from);
            e.end += static_cast<uint32_t>(from);
            edits->push_back(e);
        }
    }
};

void scan_process_unquoted(kimix::string_view cmd, kimix::vector<edit>& edits,
                           kimix::string* transformed) {
    edits.clear();
    if (cmd.empty()) {
        if (transformed) {
            *transformed = kimix::string(cmd);
        }
        return;
    }
    ProcessUnquoted scanner{cmd, cmd.size(), &edits};
    scanner.process(0);
    if (transformed) {
        apply_edits(cmd, edits, transformed);
    }
}

// ===========================================================================
// PWSH_FIX - pwsh_fix.py::_Scanner.fix
// ===========================================================================

// warning codes (low 4 bits = kind, bit 4 = trailing continuation)
enum : int {
    PW_OK = 0,
    PW_UNCLOSED_DQ = 1,
    PW_UNCLOSED_SQ = 2,
    PW_UNCLOSED_HDQ = 3,
    PW_UNCLOSED_HSQ = 4,
    PW_UNCLOSED_BLOCK = 5,
    PW_TRAILING_COMMENT = 6,
    PW_STOP_PARSING = 7,
    PW_COMMENT_ONLY = 8,
    PW_TRAILING_CONT = 9,
    PW_CONT_FLAG = 0x10,
    PW_INVALID = -1
};

struct PwshFixScanner {
    kimix::string_view s;
    size_t n = 0;
    int warning = PW_OK;

    bool at_token_start(size_t i) const noexcept {
        if (i == 0) {
            return true;
        }
        const char prev = s[i - 1];
        return !(ascii_alnum(prev) || prev == '_');
    }

    size_t skip_sq(size_t start) const noexcept {
        size_t i = start + 1;
        for (;;) {
            const size_t pos = detail::scan_find_char(s.data(), n, i, '\'');
            if (pos >= n) {
                return n;
            }
            if (pos + 1 < n && s[pos + 1] == '\'') {
                i = pos + 2;
            } else {
                return pos + 1;
            }
        }
    }

    size_t skip_dq(size_t start) const noexcept {
        size_t i = start + 1;
        for (;;) {
            const size_t hit = detail::scan_find_any(s.data(), n, i, "\"`$", 3);
            if (hit >= n) {
                return n;
            }
            const char ch = s[hit];
            if (ch == '`') {
                i = (hit + 1 < n) ? hit + 2 : hit + 1;
            } else if (ch == '"') {
                if (hit + 1 < n && s[hit + 1] == '"') {
                    i = hit + 2;
                } else {
                    return hit + 1;
                }
            } else { // '$'
                if (hit + 1 < n && s[hit + 1] == '(') {
                    i = skip_subexpr(hit);
                } else {
                    i = hit + 1;
                }
            }
        }
    }

    size_t skip_block(size_t start) const noexcept {
        size_t i = start + 2;
        for (;;) {
            const size_t pos = detail::scan_find_char(s.data(), n, i, '#');
            if (pos >= n) {
                return n;
            }
            if (pos + 1 < n && s[pos + 1] == '>') {
                return pos + 2;
            }
            i = pos + 1;
        }
    }

    size_t skip_subexpr(size_t start) const noexcept {
        size_t i = start + 2;
        size_t depth = 1;
        while (i < n && depth) {
            const size_t hit = detail::scan_find_table(s.data(), n, i, kPwshSubexpr);
            i = hit;
            if (i >= n) {
                break;
            }
            const char ch = s[i];
            if (ch == '(') {
                depth += 1;
                i += 1;
            } else if (ch == ')') {
                depth -= 1;
                i += 1;
            } else if (ch == '\'') {
                i = skip_sq(i);
            } else if (ch == '"') {
                i = skip_dq(i);
            } else if (ch == '`') {
                i += (i + 1 < n) ? 2 : 1;
            } else if (ch == '#') {
                if (at_token_start(i)) {
                    i = detail::scan_find_char(s.data(), n, i, '\n');
                    if (i < n) {
                        i += 1;
                    }
                } else {
                    i += 1;
                }
            } else if (ch == '<' && i + 1 < n && s[i + 1] == '#') {
                i = skip_block(i);
            } else {
                i += 1;
            }
        }
        return i;
    }

    // Returns false when the command cannot be repaired (None).
    bool run(kimix::string_view input, kimix::vector<edit>& edits,
             kimix::string* transformed) {
        s = input;
        n = input.size();
        edits.clear();
        warning = PW_OK;

        if (input.empty() || input.find_first_not_of(" \t\r\n\v\f") == kimix::string_view::npos) {
            warning = PW_INVALID;
            if (transformed) {
                transformed->clear();
            }
            return false;
        }
        // Fast path: no quoting/comment/continuation/here-string characters.
        if (input.find('"') == kimix::string_view::npos &&
            input.find('\'') == kimix::string_view::npos &&
            input.find('#') == kimix::string_view::npos &&
            input.find('`') == kimix::string_view::npos &&
            input.find('@') == kimix::string_view::npos &&
            input.find("--%") == kimix::string_view::npos) {
            if (transformed) {
                *transformed = kimix::string(input);
            }
            return true;
        }

        enum : uint8_t {
            NORMAL, DQ, SQ, HDQ, HSQ, COMMENT, BLOCK
        };
        uint8_t mode = NORMAL;
        char here_quote = 0;
        size_t line_start = 0;
        bool saw_code = false;
        size_t last_cont_target = kimix::string_view::npos; // -1

        size_t i = 0;
        while (i < n) {
            const char ch = s[i];
            if (mode == NORMAL) {
                // Bulk-skip plain code bytes (the final else-branch only
                // sets saw_code), then handle the interesting bytes.
                const size_t hit = detail::scan_find_table(s.data(), n, i, kPwshNormal);
                if (hit > i) {
                    saw_code = true;
                    i = hit;
                }
                if (i >= n) {
                    continue;
                }
                const char nch = s[i];
                if (nch == '"') {
                    saw_code = true;
                    mode = DQ;
                    i += 1;
                } else if (nch == '\'') {
                    saw_code = true;
                    mode = SQ;
                    i += 1;
                } else if (nch == '`') {
                    if (i + 1 < n) {
                        saw_code = true;
                        if (s[i + 1] == '\n') {
                            last_cont_target = i + 2;
                        }
                        i += 2;
                    } else {
                        warning = PW_INVALID;
                        if (transformed) {
                            transformed->clear();
                        }
                        return false; // dangling continuation backtick
                    }
                } else if (nch == '#' && at_token_start(i)) {
                    mode = COMMENT;
                    i += 1;
                } else if (nch == '<' && i + 1 < n && s[i + 1] == '#') {
                    mode = BLOCK;
                    i += 2;
                } else if (nch == '@' && i + 1 < n &&
                           (s[i + 1] == '\'' || s[i + 1] == '"') &&
                           at_token_start(i)) {
                    size_t j = i + 2;
                    while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r')) {
                        ++j;
                    }
                    if (j == n || s[j] == '\n') {
                        saw_code = true;
                        here_quote = s[i + 1];
                        mode = (here_quote == '"') ? HDQ : HSQ;
                        line_start = (j == n) ? n : j + 1;
                        i = line_start;
                        continue;
                    }
                    saw_code = true;
                    i += 1;
                } else if (nch == '-' && i + 2 < n && s[i + 1] == '-' &&
                           s[i + 2] == '%' && at_token_start(i)) {
                    if (!saw_code) {
                        warning = PW_INVALID;
                        if (transformed) {
                            transformed->clear();
                        }
                        return false;
                    }
                    const size_t nl = s.find('\n', i);
                    if (nl == kimix::string_view::npos) {
                        warning = PW_STOP_PARSING;
                        if (transformed) {
                            *transformed = kimix::string(input);
                            transformed->push_back('\n');
                        }
                        edits.push_back(edit{static_cast<uint32_t>(n),
                                             static_cast<uint32_t>(n),
                                             kimix::string("\n")});
                        return true;
                    }
                    i = nl + 1;
                } else if (nch == '$' && i + 1 < n && s[i + 1] == '(') {
                    saw_code = true;
                    i = skip_subexpr(i);
                } else if (nch == ' ' || nch == '\t' || nch == '\n' || nch == '\r' ||
                           nch == '\v' || nch == '\f') {
                    // Skip a whole run of whitespace (no state changes).
                    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' ||
                                     s[i] == '\r' || s[i] == '\v' || s[i] == '\f')) {
                        i += 1;
                    }
                } else {
                    saw_code = true;
                    i += 1;
                }
            } else if (mode == DQ) {
                const size_t hit = detail::scan_find_any(s.data(), n, i, "\"`$", 3);
                if (hit >= n) {
                    i = n;
                } else if (s[hit] == '`') {
                    i = (hit + 1 < n) ? hit + 2 : hit + 1;
                } else if (s[hit] == '"') {
                    if (hit + 1 < n && s[hit + 1] == '"') {
                        i = hit + 2;
                    } else {
                        mode = NORMAL;
                        i = hit + 1;
                    }
                } else if (s[hit] == '$' && hit + 1 < n && s[hit + 1] == '(') {
                    i = skip_subexpr(hit);
                } else {
                    i = hit + 1;
                }
            } else if (mode == SQ) {
                const size_t pos = detail::scan_find_char(s.data(), n, i, '\'');
                if (pos >= n) {
                    i = n;
                } else if (pos + 1 < n && s[pos + 1] == '\'') {
                    i = pos + 2;
                } else {
                    mode = NORMAL;
                    i = pos + 1;
                }
            } else if (mode == HDQ || mode == HSQ) {
                const char nd[2] = {'\n', here_quote};
                const size_t hit = detail::scan_find_any(s.data(), n, i, nd, 2);
                if (hit >= n) {
                    i = n;
                } else if (s[hit] == '\n') {
                    line_start = hit + 1;
                    i = hit + 1;
                } else if (hit + 1 < n && s[hit + 1] == '@' &&
                           s.substr(line_start, hit - line_start)
                                   .find_first_not_of(" \t\r") ==
                               kimix::string_view::npos) {
                    mode = NORMAL;
                    i = hit + 2;
                } else {
                    i = hit + 1;
                }
            } else if (mode == COMMENT) {
                const size_t nl = detail::scan_find_char(s.data(), n, i, '\n');
                if (nl < n) {
                    mode = NORMAL;
                    i = nl + 1;
                } else {
                    // EOF: keep mode COMMENT so the end-of-input suffix
                    // logic below still sees a trailing comment.
                    i = n;
                }
            } else { // BLOCK
                const size_t pos = detail::scan_find_char(s.data(), n, i, '#');
                if (pos >= n) {
                    i = n;
                } else if (pos + 1 < n && s[pos + 1] == '>') {
                    mode = NORMAL;
                    i = pos + 2;
                } else {
                    i = pos + 1;
                }
            }
        }

        // End of input.
        const bool needs_cont_nl =
            last_cont_target != kimix::string_view::npos &&
            s.rfind('\n') < last_cont_target;
        // suffix = the complete text appended to the input (one edit), so the
        // transformed output is exactly input + suffix.
        kimix::string suffix;
        if (mode == NORMAL) {
            if (saw_code) {
                if (needs_cont_nl) {
                    suffix = "\n";
                    warning = PW_TRAILING_CONT;
                } else {
                    warning = PW_OK;
                }
            } else {
                suffix = "\n$null";
                warning = PW_COMMENT_ONLY;
            }
        } else if (mode == DQ) {
            size_t trailing = 0;
            size_t t = n;
            while (t > 0 && s[t - 1] == '`') {
                ++trailing;
                --t;
            }
            suffix = (trailing % 2 == 1) ? "\"\"" : "\"";
            warning = PW_UNCLOSED_DQ;
            if (needs_cont_nl) {
                suffix.push_back('\n');
                warning |= PW_CONT_FLAG;
            }
        } else if (mode == SQ) {
            suffix = "'";
            warning = PW_UNCLOSED_SQ;
            if (needs_cont_nl) {
                suffix.push_back('\n');
                warning |= PW_CONT_FLAG;
            }
        } else if (mode == HDQ) {
            suffix = "\n\"@";
            warning = PW_UNCLOSED_HDQ;
            if (needs_cont_nl) {
                suffix.push_back('\n');
                warning |= PW_CONT_FLAG;
            }
        } else if (mode == HSQ) {
            suffix = "\n'@";
            warning = PW_UNCLOSED_HSQ;
            if (needs_cont_nl) {
                suffix.push_back('\n');
                warning |= PW_CONT_FLAG;
            }
        } else if (mode == COMMENT) {
            if (saw_code) {
                suffix = "\n";
                warning = PW_TRAILING_COMMENT;
            } else {
                suffix = "\n$null";
                warning = PW_COMMENT_ONLY;
            }
        } else { // BLOCK
            if (saw_code) {
                suffix = "#>";
                warning = PW_UNCLOSED_BLOCK;
            } else {
                suffix = "#>\n$null";
                warning = PW_COMMENT_ONLY;
            }
            if (needs_cont_nl) {
                suffix.push_back('\n');
                warning |= PW_CONT_FLAG;
            }
        }
        if (!suffix.empty()) {
            edits.push_back(edit{static_cast<uint32_t>(n), static_cast<uint32_t>(n), suffix});
        }
        if (transformed) {
            *transformed = kimix::string(input);
            transformed->append(suffix);
        }
        return true;
    }
};

void scan_pwsh_fix(kimix::string_view cmd, kimix::vector<edit>& edits,
                   kimix::string* transformed, int* warning_code) {
    PwshFixScanner scanner;
    scanner.run(cmd, edits, transformed);
    if (warning_code) {
        *warning_code = scanner.warning;
    }
}

// ===========================================================================
// BASH_FIX - bash_fix.py::_Scanner
// ===========================================================================

// _FALLBACK_BODIES / _FALLBACK_COMMAND_WRAPPERS / _UNSUPPORTED_BODIES keys.
// GENERATED, do not edit: python scripts/gen_bash_fix_data.py --tables-runtime
// rewrites everything between the markers below from kimi-agent's
// bin/kimix_native/_shell_compat.py.
// >>> GENERATED:BASH-FIX-PARSE-DATA >>>
// GENERATED by scripts/gen_bash_fix_data.py from kimi-agent's
// bin/kimix_native/_shell_compat.py - DO NOT EDIT BY HAND.
//
// The BASH_FIX scanner's name tables, keyed by the reference's own data:
//
//   kFallbackNames    _FALLBACK_BODIES keys (88 names, reference order).
//                     A word at an executable command position whose
//                     quote-removal value is one of these has its fallback
//                     definition recorded (and is swapped for the standalone
//                     runner when an exec-ing wrapper consumes it as its
//                     command operand).
//   kFallbackWrapperNames  _FALLBACK_COMMAND_WRAPPERS: names that are
//                     fallback names AND command wrappers.  `kind` is the
//                     reference wrapper kind; this kernel implements the
//                     "sudo" kind (WrapperKind::SUDO) and only records the
//                     name for the others - the "timeout"/"watch" wrapper
//                     semantics (option tables, the mandatory DURATION
//                     operand, quoted `watch` scripts) are not ported here,
//                     and the Python shim routes those commands to the
//                     pure-Python reference.
//   kUnsupportedNames _UNSUPPORTED_BODIES keys (4 name(s)): commands with no
//                     faithful Windows Git Bash equivalent.  The scanner
//                     records them in its `unsupported` output and leaves the
//                     command text byte-for-byte, so the caller can refuse to
//                     run the command with the reason instead of letting Bash
//                     fail with "command not found".
struct fallback_wrapper_entry {
    const char *name;
    const char *kind;
};

const char* const kFallbackNames[] = {
    "gtimeout", "rev", "xdg-open", "open", "pbcopy", "pbpaste", "wget", "xclip",
    "xsel", "wl-copy", "wl-paste", "zip", "nc", "pgrep", "pkill", "traceroute",
    "tree", "say", "python3", "pip3", "copy", "move", "del", "erase",
    "ren", "rename", "rd", "md", "chdir", "cls", "xcopy", "mklink",
    "findstr", "fc", "where", "tasklist", "taskkill", "systeminfo", "watch", "killall",
    "pidof", "column", "free", "uptime", "top", "ss", "ip", "man",
    "systemctl", "sudo", "gawk", "gcat", "gcomm", "gcp", "gcut", "gdate",
    "gdf", "gdu", "gegrep", "gfgrep", "gfind", "ggrep", "ghead", "gjoin",
    "gln", "gls", "gmake", "gmkdir", "gmv", "gpaste", "greadlink", "grealpath",
    "grm", "grmdir", "gsed", "gseq", "gshuf", "gsort", "gsplit", "gstat",
    "gtail", "gtar", "gtr", "guniq", "gwc", "gxargs", "netcat", "htop",
};

const fallback_wrapper_entry kFallbackWrapperNames[] = {
    {"gtimeout", "timeout"},
    {"watch", "watch"},
    {"sudo", "sudo"},
};

const char* const kUnsupportedNames[] = {
    "journalctl",
    "goto",
    "call :label",
    "exit /b",
};

constexpr size_t kFallbackNamesCount =
    sizeof(kFallbackNames) / sizeof(kFallbackNames[0]);
constexpr size_t kUnsupportedNamesCount =
    sizeof(kUnsupportedNames) / sizeof(kUnsupportedNames[0]);
// <<< GENERATED:BASH-FIX-PARSE-DATA <<<

bool bash_fix_name_in_table(kimix::string_view name, const char* const* table,
                   size_t count) noexcept {
    for (size_t i = 0; i < count; ++i) {
        if (name == kimix::string_view(table[i])) {
            return true;
        }
    }
    return false;
}

bool is_fallback_name(kimix::string_view name) noexcept {
    return bash_fix_name_in_table(name, kFallbackNames, kFallbackNamesCount);
}

// _UNSUPPORTED_BODIES keys: commands with no faithful Windows Git Bash
// equivalent.  The scanner records the name (leaving the command text
// byte-for-byte) so the caller can refuse to run it with the reference's
// reason instead of a bare "command not found".
bool is_unsupported_name(kimix::string_view name) noexcept {
    return bash_fix_name_in_table(name, kUnsupportedNames, kUnsupportedNamesCount);
}

// cmd_translator._GLUED_LABEL_RE: ^:[A-Za-z_][A-Za-z0-9_]*$ -- a batch label
// GLUED to the command position (``:label`` on its own line).
bool glued_label_re(kimix::string_view name) noexcept {
    if (name.size() < 2 || name[0] != ':') {
        return false;
    }
    const char c1 = name[1];
    if (!(ascii_alpha(c1) || c1 == '_')) {
        return false;
    }
    for (size_t i = 2; i < name.size(); ++i) {
        if (!(ascii_alnum(name[i]) || name[i] == '_')) {
            return false;
        }
    }
    return true;
}

// cmd_translator._CALL_BATCH_RE: \.(?:bat|cmd)$ (re.IGNORECASE) -- a child
// script target after ``call``.
bool call_batch_suffix(kimix::string_view word) noexcept {
    if (word.size() < 4 || word[word.size() - 4] != '.') {
        return false;
    }
    auto lower = [](char c) noexcept {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    const char a = lower(word[word.size() - 3]);
    const char b = lower(word[word.size() - 2]);
    const char c = lower(word[word.size() - 1]);
    return (a == 'b' && b == 'a' && c == 't') ||
           (a == 'c' && b == 'm' && c == 'd');
}

// _FALLBACK_COMMAND_WRAPPERS membership: the wrapper kind of a name that is
// BOTH a fallback name and a command wrapper ("timeout" / "watch" / "sudo"),
// or an empty view.  The reference looks the RAW word up (no quote removal),
// so ``'sudo'`` is a plain literal fallback word instead.
kimix::string_view fallback_wrapper_kind(kimix::string_view raw) noexcept {
    for (const fallback_wrapper_entry& entry : kFallbackWrapperNames) {
        if (raw == kimix::string_view(entry.name)) {
            return kimix::string_view(entry.kind);
        }
    }
    return kimix::string_view();
}

// "\x01<name>\x01" marker for fallback-command edits; the shim expands it.
kimix::string fallback_marker(kimix::string_view name) {
    kimix::string m;
    m.push_back('\x01');
    m.append(name.data(), name.size());
    m.push_back('\x01');
    return m;
}

bool bash_fix_contains_name(const kimix::vector<kimix::string>& names,
                   kimix::string_view name) noexcept {
    for (const kimix::string& existing : names) {
        if (kimix::string_view(existing) == name) {
            return true;
        }
    }
    return false;
}

bool assignment_re(kimix::string_view raw) noexcept {
    // ^[A-Za-z_][A-Za-z0-9_]*(?:\+)?=
    if (raw.empty()) {
        return false;
    }
    const char c0 = raw[0];
    if (!(ascii_alpha(c0) || c0 == '_')) {
        return false;
    }
    size_t i = 1;
    while (i < raw.size() && (ascii_alnum(raw[i]) || raw[i] == '_')) {
        ++i;
    }
    if (i < raw.size() && raw[i] == '+') {
        ++i;
    }
    return i < raw.size() && raw[i] == '=';
}

bool name_re(kimix::string_view raw) noexcept {
    // ^[A-Za-z_][A-Za-z0-9_]*$
    if (raw.empty()) {
        return false;
    }
    const char c0 = raw[0];
    if (!(ascii_alpha(c0) || c0 == '_')) {
        return false;
    }
    for (size_t i = 1; i < raw.size(); ++i) {
        if (!(ascii_alnum(raw[i]) || raw[i] == '_')) {
            return false;
        }
    }
    return true;
}

bool path_drive_re(kimix::string_view raw) noexcept {
    // ^[A-Za-z]:\\.*$  (fullmatch)
    if (raw.size() < 3) {
        return false;
    }
    if (!ascii_alpha(raw[0]) || raw[1] != ':' || raw[2] != '\\') {
        return false;
    }
    return true;
}

bool path_segment_re(kimix::string_view raw) noexcept {
    // ^[A-Za-z0-9_.~\\-]+$  (fullmatch)
    if (raw.empty()) {
        return false;
    }
    for (char c : raw) {
        const bool ok = ascii_alnum(c) || c == '_' || c == '.' || c == '~' ||
                        c == '\\' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool path_safe_char(char c) noexcept {
    if (ascii_alnum(c)) {
        return true;
    }
    switch (c) {
    case '_': case '.': case '/': case ':': case '~': case '@': case '%':
    case '+': case '=': case '-': case '#': case ',': case '[': case ']':
    case '*': case '?':
        return true;
    default:
        return false;
    }
}

bool escaped_literal_char(char c) noexcept {
    switch (c) {
    case ' ': case '\t': case '&': case ';': case '|': case '(': case ')':
    case '<': case '>': case '#': case '\'': case '"': case '$': case '`':
    case '{': case '}': case '!':
        return true;
    default:
        return false;
    }
}

bool word_end_char(char c) noexcept {
    switch (c) {
    case ';': case '&': case '|': case '(': case ')': case '<': case '>':
    case '\n': case ' ': case '\t': case '\r':
        return true;
    default:
        return false;
    }
}

bool redirection_start(char c) noexcept { return c == '<' || c == '>'; }

bool operator_char(char c) noexcept {
    switch (c) {
    case ';': case '&': case '|': case '(': case ')': case '<': case '>':
    case '\n':
        return true;
    default:
        return false;
    }
}

enum class WrapperKind : uint8_t {
    COMMAND, COPROC, ENV, EXEC, NOHUP, SUDO, TIME
};

struct Wrapper {
    WrapperKind kind = WrapperKind::COMMAND;
    bool skip_next = false;
    bool opaque = false;
    bool path_value = false;
};

bool wrapper_kind_of(kimix::string_view name, WrapperKind& out) noexcept {
    if (name == "command") { out = WrapperKind::COMMAND; return true; }
    if (name == "coproc") { out = WrapperKind::COPROC; return true; }
    if (name == "env") { out = WrapperKind::ENV; return true; }
    if (name == "exec") { out = WrapperKind::EXEC; return true; }
    if (name == "nohup") { out = WrapperKind::NOHUP; return true; }
    if (name == "sudo") { out = WrapperKind::SUDO; return true; }
    if (name == "time") { out = WrapperKind::TIME; return true; }
    return false;
}

struct HereDoc {
    kimix::string delimiter; // empty == unmatchable
    bool strip_tabs = false;
    bool expands = false;
};

struct BashFixScanner {
    kimix::string_view s;
    size_t n = 0;
    kimix::vector<edit>* edits = nullptr;
    kimix::vector<kimix::string>* names = nullptr;
    kimix::vector<kimix::string>* notes = nullptr;
    kimix::vector<kimix::string>* nul_notes = nullptr;
    // _UNSUPPORTED_BODIES names seen at a command position, in source order,
    // de-duplicated (the reference records them in ``BashFix.unsupported``).
    kimix::vector<kimix::string>* unsupported = nullptr;
    kimix::vector<std::pair<size_t, size_t>> heredoc_events;
    size_t nest_depth = 0;
    bool aborted = false;
    // Depth bound for the scanner recursion. The reference uses 1024
    // (_MAX_NESTING_DEPTH) but Python's interpreter limit (~1000) fires
    // first in practice; the C++ frames are much larger than Python frames,
    // so 1024 levels would overflow a 1 MiB thread stack when the kernel is
    // called from Python. The bound must also hold for unoptimized builds: in
    // a debug build one `$(` level costs several KB of stack (find_matching /
    // read_word / scan_range_inner keep their vectors and temporaries in
    // separate slots), so 256 levels exhausted the 1 MiB stack and crashed the
    // process (test_native_shell_scanner "bash_fix_deep_nesting_abort") before
    // the guard could fire. 64 levels stays well inside 1 MiB even then, and is
    // far beyond any real command; deeper input takes the reference's
    // RecursionError path (command returned unchanged).
    static constexpr size_t MAX_DEPTH = 64;


    // Bounded newline/char search (reference s.find(c, from, end) semantics).
    size_t find_nl_bounded(size_t from, size_t end) const noexcept {
        if (from >= end) {
            return kimix::string_view::npos;
        }
        const size_t r = s.find('\n', from);
        return (r == kimix::string_view::npos || r >= end) ? kimix::string_view::npos : r;
    }

    size_t find_char_bounded(char c, size_t from, size_t end) const noexcept {
        if (from >= end) {
            return kimix::string_view::npos;
        }
        const size_t r = s.find(c, from);
        return (r == kimix::string_view::npos || r >= end) ? kimix::string_view::npos : r;
    }

    bool starts_with(size_t i, const char* str) const noexcept {
        const size_t len = std::strlen(str);
        return i + len <= n && std::memcmp(s.data() + i, str, len) == 0;
    }

    bool starts_raw(kimix::string_view raw, size_t i, const char* str) const noexcept {
        const size_t len = std::strlen(str);
        return i + len <= raw.size() &&
               std::memcmp(raw.data() + i, str, len) == 0;
    }

    bool is_command_start_keyword(kimix::string_view raw) const noexcept {
        return raw == "!" || raw == "{" || raw == "if" || raw == "then" ||
               raw == "elif" || raw == "else" || raw == "while" ||
               raw == "until" || raw == "do";
    }

    bool is_command_end_keyword(kimix::string_view raw) const noexcept {
        return raw == "fi" || raw == "done" || raw == "esac";
    }

    bool is_list_keyword(kimix::string_view raw) const noexcept {
        return raw == "for" || raw == "select" || raw == "case";
    }

    // -- literal command name (quote removal only) --------------------------
    // Reference _BashFixScanner._literal_word_value: the word value produced
    // solely by Bash quote removal, or false when the word contains an
    // expansion/glob/brace form (which quote removal alone cannot resolve).
    bool literal_word_value(kimix::string_view raw, kimix::string& value) const {
        value.clear();
        size_t i = 0;
        const size_t len = raw.size();
        while (i < len) {
            const char ch = raw[i];
            if (ch == '\\') {
                if (i + 1 >= len) {
                    return false;
                }
                if (raw[i + 1] == '\n') {
                    i += 2;
                    continue;
                }
                value.push_back(raw[i + 1]);
                i += 2;
                continue;
            }
            if (ch == '\'') {
                const size_t close = raw.find('\'', i + 1);
                if (close == kimix::string_view::npos) {
                    return false;
                }
                value.append(raw.data() + i + 1, close - i - 1);
                i = close + 1;
                continue;
            }
            if (ch == '"') {
                i += 1;
                while (i < len && raw[i] != '"') {
                    const char inner = raw[i];
                    if (inner == '$' || inner == '`') {
                        return false;
                    }
                    if (inner == '\\' && i + 1 < len) {
                        const char escaped = raw[i + 1];
                        if (escaped == '$' || escaped == '`' || escaped == '"' ||
                            escaped == '\\' || escaped == '\n') {
                            if (escaped != '\n') {
                                value.push_back(escaped);
                            }
                            i += 2;
                            continue;
                        }
                    }
                    value.push_back(inner);
                    i += 1;
                }
                if (i >= len) {
                    return false;
                }
                i += 1;
                continue;
            }
            if (ch == '$' || ch == '`' || ch == '*' || ch == '?' || ch == '[' ||
                ch == '{' || ch == '~') {
                return false;
            }
            value.push_back(ch);
            i += 1;
        }
        return true;
    }

    // _literal_command_name: a word that quote-resolves to a fallback name.
    bool literal_command_name(kimix::string_view raw, kimix::string& name) const {
        if (!literal_word_value(raw, name)) {
            return false;
        }
        if (is_fallback_name(name)) {
            return true;
        }
        name.clear();
        return false;
    }

    // _literal_unsupported_name: a word that quote-resolves to a name with no
    // faithful Windows Git Bash equivalent (_UNSUPPORTED_BODIES).
    bool literal_unsupported_name(kimix::string_view raw,
                                  kimix::string& name) const {
        if (!literal_word_value(raw, name)) {
            return false;
        }
        if (is_unsupported_name(name)) {
            return true;
        }
        name.clear();
        return false;
    }

    // _peek_literal_word (cmd_translator.peek_literal_word): the quote-removed
    // value of the word at `pos`, read WITHOUT recording substitution edits.
    // Horizontal whitespace only; false on no word or a non-decodable
    // (runtime-expansion) word.
    bool peek_literal_word(size_t pos, size_t end, kimix::string& value) {
        size_t q = pos;
        while (q < end && is_ws(s[q])) {
            ++q;
        }
        const size_t word_end = read_word(q, end, false);
        return literal_word_value(s.substr(q, word_end - q), value);
    }

    // _record_cmd_jump / cmd_translator.cmd_jump_record: the refusal key for an
    // untranslatable cmd.exe batch jump at a command position, or an empty
    // string.  ``goto`` and a glued ``:label`` line have no bash equivalent,
    // ``call`` into a ``:label`` / ``*.bat`` / ``*.cmd`` target neither, and a
    // translated ``exit /b`` would kill the persistent session -- all three are
    // refused through the same ``unsupported`` channel, so the command text
    // stays byte-for-byte.  Only the operand word is peeked; nothing is edited.
    kimix::string cmd_jump_key(kimix::string_view raw, size_t word_end,
                               size_t end) {
        kimix::string name;
        if (!literal_word_value(raw, name)) {
            name.assign(raw.data(), raw.size());
        }
        if (name == "goto" || glued_label_re(name)) {
            return kimix::string("goto");
        }
        if (name == "call") {
            kimix::string next;
            if (peek_literal_word(word_end, end, next) &&
                ((!next.empty() && next[0] == ':') || call_batch_suffix(next))) {
                return kimix::string("call :label");
            }
        }
        if (name == "exit") {
            kimix::string next;
            if (peek_literal_word(word_end, end, next) &&
                (next == "/b" || next == "/B")) {
                return kimix::string("exit /b");
            }
        }
        return kimix::string();
    }

    // -- word / region skips ------------------------------------------------
    size_t skip_single_quote(size_t i, size_t end) const noexcept {
        const size_t close = find_char_bounded('\'', i, end);
        return (close == kimix::string_view::npos) ? end : close + 1;
    }

    size_t skip_ansi_quote(size_t i, size_t end) const noexcept {
        for (;;) {
            const size_t hit = detail::scan_find_any(s.data(), end, i, "\\'", 2);
            if (hit >= end) {
                return end;
            }
            if (s[hit] == '\\') {
                i = (hit + 1 < end) ? hit + 2 : hit + 1;
            } else {
                return hit + 1;
            }
        }
    }

    size_t skip_double_quote(size_t i, size_t end) {
        for (;;) {
            const size_t hit = detail::scan_find_table(s.data(), end, i, kDqSpecial);
            i = hit;
            if (i >= end) {
                return end;
            }
            const char ch = s[i];
            if (ch == '\\' && i + 1 < end &&
                (s[i + 1] == '$' || s[i + 1] == '`' || s[i + 1] == '"' ||
                 s[i + 1] == '\\' || s[i + 1] == '\n')) {
                i += 2;
            } else if (ch == '"') {
                return i + 1;
            } else if (ch == '`') {
                const size_t close = find_backtick_end(i + 1, end);
                scan_range(i + 1, close);
                i = (close < end) ? close + 1 : end;
            } else if (ch == '$' && starts_with(i, "$(") && !starts_with(i, "$((")) {
                const size_t close = find_matching(i + 2, end, ')');
                scan_range(i + 2, close < end ? close : end);
                i = (close < end) ? close + 1 : end;
            } else if (ch == '$' && starts_with(i, "${")) {
                i = skip_parameter(i + 2, end);
            } else {
                i += 1;
            }
        }
    }

    size_t skip_double_quote_for_matching(size_t i, size_t end) {
        for (;;) {
            const size_t hit = detail::scan_find_table(s.data(), end, i, kDqSpecial);
            i = hit;
            if (i >= end) {
                return end;
            }
            const char ch = s[i];
            if (ch == '\\' && i + 1 < end &&
                (s[i + 1] == '$' || s[i + 1] == '`' || s[i + 1] == '"' ||
                 s[i + 1] == '\\' || s[i + 1] == '\n')) {
                i += 2;
            } else if (ch == '"') {
                return i + 1;
            } else if (ch == '`') {
                const size_t close = find_backtick_end(i + 1, end);
                i = (close < end) ? close + 1 : end;
            } else if (ch == '$' && starts_with(i, "$(") && !starts_with(i, "$((")) {
                const size_t close = find_matching(i + 2, end, ')');
                i = (close < end) ? close + 1 : end;
            } else {
                i += 1;
            }
        }
    }

    size_t find_backtick_end(size_t i, size_t end) const noexcept {
        for (;;) {
            const size_t hit = detail::scan_find_any(s.data(), end, i, "\\`", 2);
            if (hit >= end) {
                return end;
            }
            if (s[hit] == '\\') {
                i = (hit + 1 < end) ? hit + 2 : hit + 1;
            } else {
                return hit;
            }
        }
    }

    size_t skip_arithmetic(size_t i, size_t end) {
        size_t depth = 1;
        while (i < end) {
            const char ch = s[i];
            if (ch == '$' && starts_with(i, "$(") && !starts_with(i, "$((")) {
                const size_t close = find_matching(i + 2, end, ')');
                scan_range(i + 2, close < end ? close : end);
                i = (close < end) ? close + 1 : end;
            } else if (ch == '(' && starts_with(i, "((")) {
                depth += 1;
                i += 2;
            } else if (ch == ')' && starts_with(i, "))")) {
                depth -= 1;
                i += 2;
                if (depth == 0) {
                    return i;
                }
            } else if (ch == '\\') {
                i += (i + 1 < end) ? 2 : 1;
            } else if (ch == '\'') {
                i = skip_single_quote(i + 1, end);
            } else if (ch == '"') {
                i = skip_double_quote(i + 1, end);
            } else {
                i += 1;
            }
        }
        return end;
    }

    size_t skip_parameter_literal(size_t i, size_t end) {
        size_t depth = 1;
        while (i < end) {
            if (s[i] == '\\') {
                i += (i + 1 < end) ? 2 : 1;
            } else if (s[i] == '\'') {
                i = skip_single_quote(i + 1, end);
            } else if (s[i] == '"') {
                i = skip_double_quote_for_matching(i + 1, end);
            } else if (s[i] == '{') {
                depth += 1;
                i += 1;
            } else if (s[i] == '}') {
                depth -= 1;
                i += 1;
                if (depth == 0) {
                    return i;
                }
            } else {
                i += 1;
            }
        }
        return end;
    }

    size_t skip_parameter(size_t i, size_t end) {
        size_t depth = 1;
        while (i < end) {
            const char ch = s[i];
            if (ch == '\\') {
                i += (i + 1 < end) ? 2 : 1;
            } else if (ch == '$' && starts_with(i, "$(") && !starts_with(i, "$((")) {
                const size_t close = find_matching(i + 2, end, ')');
                scan_range(i + 2, close < end ? close : end);
                i = (close < end) ? close + 1 : end;
            } else if (ch == '\'') {
                i = skip_single_quote(i + 1, end);
            } else if (ch == '"') {
                i = skip_double_quote(i + 1, end);
            } else if (ch == '{') {
                depth += 1;
                i += 1;
            } else if (ch == '}') {
                depth -= 1;
                i += 1;
                if (depth == 0) {
                    return i;
                }
            } else {
                i += 1;
            }
        }
        return end;
    }

    size_t skip_conditional(size_t i, size_t end) {
        while (i < end) {
            const char ch = s[i];
            if (ch == ']' && starts_with(i, "]]")) {
                return i + 2;
            }
            if (ch == '\\') {
                i += (i + 1 < end) ? 2 : 1;
            } else if (ch == '$' && starts_with(i, "$'")) {
                i = skip_ansi_quote(i + 2, end);
            } else if (ch == '\'') {
                i = skip_single_quote(i + 1, end);
            } else if (ch == '"') {
                i = skip_double_quote(i + 1, end);
            } else if (ch == '`') {
                const size_t close = find_backtick_end(i + 1, end);
                scan_range(i + 1, close);
                i = (close < end) ? close + 1 : end;
            } else if (ch == '$' && starts_with(i, "$(") && !starts_with(i, "$((")) {
                const size_t close = find_matching(i + 2, end, ')');
                scan_range(i + 2, close < end ? close : end);
                i = (close < end) ? close + 1 : end;
            } else if (ch == '$' && starts_with(i, "$((")) {
                i = skip_arithmetic(i + 3, end);
            } else {
                i += 1;
            }
        }
        return end;
    }

    size_t find_matching(size_t i, size_t end, char closing) {
        if (nest_depth >= MAX_DEPTH) {
            aborted = true;
            return end;
        }
        ++nest_depth;
        const size_t r = find_matching_inner(i, end, closing);
        --nest_depth;
        return r;
    }

    size_t find_matching_inner(size_t i, size_t end, char closing) {
        size_t depth = 0;
        kimix::vector<HereDoc> pending_heredocs;
        kimix::vector<uint8_t> case_stack; // 0 word, 1 await-in, 2 patterns, 3 body
        while (i < end) {
            const char ch = s[i];
            if (ch == '\\') {
                i += (i + 1 < end) ? 2 : 1;
            } else if (ch == '\n') {
                i += 1;
                if (!pending_heredocs.empty()) {
                    i = skip_heredoc_bodies(i, end, pending_heredocs, false);
                    pending_heredocs.clear();
                }
            } else if (ch == '$' && starts_with(i, "$((")) {
                i = skip_arithmetic(i + 3, end);
                if (!case_stack.empty() && case_stack.back() == 0) {
                    case_stack.back() = 1;
                }
            } else if (ch == '<' && starts_with(i, "<<") && !starts_with(i, "<<<")) {
                const bool strip_tabs = starts_with(i, "<<-");
                size_t delimiter_start = i + (strip_tabs ? 3 : 2);
                while (delimiter_start < end && is_ws(s[delimiter_start])) {
                    ++delimiter_start;
                }
                const size_t delimiter_end =
                    read_word(delimiter_start, end, false);
                kimix::string delimiter;
                bool expands = false;
                if (heredoc_delimiter(
                        s.substr(delimiter_start, delimiter_end - delimiter_start),
                        delimiter, expands)) {
                    pending_heredocs.push_back(HereDoc{delimiter, strip_tabs, expands});
                }
                i = (delimiter_end > delimiter_start) ? delimiter_end : delimiter_start;
            } else if (ch == '\'') {
                i = skip_single_quote(i + 1, end);
            } else if (ch == '"') {
                i = skip_double_quote_for_matching(i + 1, end);
            } else if (ch == '`') {
                const size_t close = find_backtick_end(i + 1, end);
                i = (close < end) ? close + 1 : end;
                if (!case_stack.empty() && case_stack.back() == 0) {
                    case_stack.back() = 1;
                }
            } else if (ch == '#' && comment_starts(i, 0)) {
                const size_t newline = find_nl_bounded(i + 1, end);
                i = (newline == kimix::string_view::npos) ? end : newline;
            } else if (ch == ';' && starts_with(i, ";;&")) {
                if (!case_stack.empty()) {
                    case_stack.back() = 2;
                }
                i += 3;
            } else if (ch == ';' && (starts_with(i, ";;") || starts_with(i, ";&"))) {
                if (!case_stack.empty()) {
                    case_stack.back() = 2;
                }
                i += 2;
            } else if (!word_end_char(ch)) {
                const size_t word_end = read_word(i, end, false);
                if (word_end <= i) {
                    i += 1;
                    continue;
                }
                const kimix::string_view word = s.substr(i, word_end - i);
                if (word == "case") {
                    case_stack.push_back(0);
                } else if (!case_stack.empty() &&
                           (case_stack.back() == 2 || case_stack.back() == 3) &&
                           word == "esac") {
                    case_stack.pop_back();
                } else if (!case_stack.empty() && case_stack.back() == 0) {
                    case_stack.back() = 1;
                } else if (!case_stack.empty() && case_stack.back() == 1 &&
                           word == "in") {
                    case_stack.back() = 2;
                }
                i = word_end;
            } else if (ch == '(') {
                depth += 1;
                i += 1;
            } else if (ch == closing) {
                if (!case_stack.empty() && case_stack.back() == 2) {
                    case_stack.back() = 3;
                    i += 1;
                } else if (depth == 0) {
                    return i;
                } else {
                    depth -= 1;
                    i += 1;
                }
            } else {
                i += 1;
            }
        }
        return end;
    }

    size_t read_word(size_t start, size_t end, bool scan_substitutions) {
        size_t i = start;
        if (i < end && s[i] == '#') {
            return i; // '#' at word start ends the word (matches the walk)
        }
        for (;;) {
            // Bulk-skip plain word bytes; only terminators and quoting/
            // expansion starts need the per-char walk.
            const size_t hit = detail::scan_find_table(s.data(), end, i, kWordSpecial);
            i = hit;
            if (i >= end) {
                break;
            }
            const char ch = s[i];
            if (word_end_char(ch)) {
                break;
            }
            if (ch == '\\') {
                i += (i + 1 < end) ? 2 : 1;
                continue;
            }
            if (ch == '\'') {
                i = skip_single_quote(i + 1, end);
                continue;
            }
            if (ch == '"') {
                i = scan_substitutions ? skip_double_quote(i + 1, end)
                                       : skip_double_quote_for_matching(i + 1, end);
                continue;
            }
            if (ch == '`') {
                const size_t close = find_backtick_end(i + 1, end);
                if (scan_substitutions) {
                    scan_range(i + 1, close);
                }
                i = (close < end) ? close + 1 : end;
                continue;
            }
            if (ch == '$') {
                if (starts_with(i, "$((")) {
                    i = skip_arithmetic(i + 3, end);
                    continue;
                }
                if (starts_with(i, "$(")) {
                    const size_t close = find_matching(i + 2, end, ')');
                    if (scan_substitutions) {
                        scan_range(i + 2, close < end ? close : end);
                    }
                    i = (close < end) ? close + 1 : end;
                    continue;
                }
                if (starts_with(i, "${")) {
                    i = scan_substitutions ? skip_parameter(i + 2, end)
                                           : skip_parameter_literal(i + 2, end);
                    continue;
                }
                if (starts_with(i, "$'")) {
                    i = skip_ansi_quote(i + 2, end);
                    continue;
                }
            }
            i += 1;
        }
        return i;
    }

    // -- heredocs -----------------------------------------------------------
    void read_ansi_c_delimiter(kimix::string_view raw, size_t i, kimix::string& result,
                               size_t& out_i, bool& valid) const {
        static const char simple_chars[] = "abefnrtvE\\'\"?";
        static const char simple_vals[] = {'\a', '\b', '\x1b', '\f', '\n',
                                           '\r', '\t', '\v', '\x1b', '\\',
                                           '\'', '\"', '?'};
        const size_t len = raw.size();
        while (i < len) {
            if (raw[i] == '\'') {
                out_i = i + 1;
                return;
            }
            if (raw[i] != '\\' || i + 1 >= len) {
                result.push_back(raw[i]);
                i += 1;
                continue;
            }
            const char escape = raw[i + 1];
            const char* pos = std::strchr(simple_chars, escape);
            if (pos != nullptr) {
                result.push_back(simple_vals[pos - simple_chars]);
                i += 2;
                continue;
            }
            if (escape >= '0' && escape <= '7') {
                size_t j = i + 1;
                while (j < len && j < i + 4 && raw[j] >= '0' && raw[j] <= '7') {
                    ++j;
                }
                int value = 0;
                for (size_t k = i + 1; k < j; ++k) {
                    value = value * 8 + (raw[k] - '0');
                }
                result.push_back(static_cast<char>(value & 0xFF));
                i = j;
                continue;
            }
            if (escape == 'x' || escape == 'X' || escape == 'u' || escape == 'U') {
                const size_t widths = (escape == 'x' || escape == 'X') ? 2
                                        : (escape == 'u' ? 4 : 8);
                size_t j = i + 2;
                const size_t limit = (len < j + widths) ? len : j + widths;
                while (j < limit && ((raw[j] >= '0' && raw[j] <= '9') ||
                                     (raw[j] >= 'a' && raw[j] <= 'f') ||
                                     (raw[j] >= 'A' && raw[j] <= 'F'))) {
                    ++j;
                }
                if (j > i + 2) {
                    uint32_t value = 0;
                    for (size_t k = i + 2; k < j; ++k) {
                        char c = raw[k];
                        value = value * 16 + (c <= '9' ? c - '0'
                                                : (c <= 'F' ? c - 'A' + 10
                                                            : c - 'a' + 10));
                    }
                    if (value <= 0x10FFFF && !(0xD800 <= value && value <= 0xDFFF)) {
                        // UTF-8 encode the code point
                        if (value < 0x80) {
                            result.push_back(static_cast<char>(value));
                        } else if (value < 0x800) {
                            result.push_back(static_cast<char>(0xC0 | (value >> 6)));
                            result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
                        } else if (value < 0x10000) {
                            result.push_back(static_cast<char>(0xE0 | (value >> 12)));
                            result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
                            result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
                        } else {
                            result.push_back(static_cast<char>(0xF0 | (value >> 18)));
                            result.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
                            result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
                            result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
                        }
                    } else {
                        valid = false;
                        result.append(raw.data() + i, j - i);
                    }
                    i = j;
                    continue;
                }
            }
            result.push_back('\\');
            result.push_back(escape);
            i += 2;
        }
        out_i = i;
    }

    bool heredoc_delimiter(kimix::string_view raw, kimix::string& out_delimiter,
                           bool& out_expands) const {
        if (raw.empty()) {
            return false;
        }
        kimix::string result;
        bool quoted = false;
        bool matchable = true;
        size_t i = 0;
        const size_t len = raw.size();
        while (i < len) {
            const char ch = raw[i];
            if (starts_raw(raw, i, "$'")) {
                quoted = true;
                size_t j = 0;
                bool valid = true;
                read_ansi_c_delimiter(raw, i + 2, result, j, valid);
                i = j;
                matchable = matchable && valid;
            } else if (ch == '\'') {
                quoted = true;
                const size_t close = raw.find('\'', i + 1);
                if (close == kimix::string_view::npos) {
                    result.append(raw.data() + i + 1, len - i - 1);
                    i = len;
                } else {
                    result.append(raw.data() + i + 1, close - i - 1);
                    i = close + 1;
                }
            } else if (ch == '"') {
                quoted = true;
                i += 1;
                while (i < len && raw[i] != '"') {
                    if (raw[i] == '\\' && i + 1 < len) {
                        const char escaped = raw[i + 1];
                        if (escaped == '$' || escaped == '`' || escaped == '"' ||
                            escaped == '\\' || escaped == '\n') {
                            if (escaped != '\n') {
                                result.push_back(escaped);
                            }
                            i += 2;
                            continue;
                        }
                    }
                    result.push_back(raw[i]);
                    i += 1;
                }
                if (i < len) {
                    i += 1;
                }
            } else if (ch == '\\' && i + 1 < len) {
                quoted = true;
                result.push_back(raw[i + 1]);
                i += 2;
            } else {
                result.push_back(ch);
                i += 1;
            }
        }
        if (!matchable) {
            // Python reference (_Scanner._heredoc_delimiter): an unmatchable
            // delimiter (out-of-range/surrogate ANSI-C escape) is still a
            // heredoc whose body is conservatively kept to EOF. Empty
            // delimiter == unmatchable (see HereDoc). Do NOT return false,
            // which would let the following lines be scanned as commands.
            out_delimiter.clear();
            out_expands = !quoted;
            return true;
        }
        out_delimiter = result;
        out_expands = !quoted;
        return true;
    }

    static bool heredoc_line_continues(kimix::string_view line) noexcept {
        size_t trailing = 0;
        size_t t = line.size();
        while (t > 0 && line[t - 1] == '\\') {
            ++trailing;
            --t;
        }
        return trailing % 2 == 1;
    }

    void scan_heredoc_expansions(size_t i, size_t end) {
        for (;;) {
            const size_t hit = detail::scan_find_table(s.data(), end, i, kExpSpecial);
            i = hit;
            if (i >= end) {
                break;
            }
            const char ch = s[i];
            if (ch == '\\') {
                i += (i + 1 < end) ? 2 : 1;
            } else if (ch == '`') {
                const size_t close = find_backtick_end(i + 1, end);
                scan_range(i + 1, close);
                i = (close < end) ? close + 1 : end;
            } else if (ch == '$' && starts_with(i, "$(") && !starts_with(i, "$((")) {
                const size_t close = find_matching(i + 2, end, ')');
                scan_range(i + 2, close < end ? close : end);
                i = (close < end) ? close + 1 : end;
            } else if (ch == '$' && starts_with(i, "$((")) {
                i = skip_arithmetic(i + 3, end);
            } else if (ch == '$' && starts_with(i, "${")) {
                i = skip_parameter(i + 2, end);
            } else {
                i += 1;
            }
        }
    }

    size_t skip_heredoc_bodies(size_t i, size_t end,
                               kimix::vector<HereDoc>& documents,
                               bool scan_expansions) {
        size_t redir_line_end = kimix::string::npos;
        for (const HereDoc& document : documents) {
            const size_t body_start = i;
            if (redir_line_end == kimix::string::npos) {
                redir_line_end = (body_start > 0) ? body_start - 1 : kimix::string::npos;
            }
            kimix::string logical_line;
            size_t logical_start = i;
            bool broke = false;
            while (i < end) {
                const size_t newline = find_nl_bounded(i, end);
                const size_t line_end = (newline == kimix::string_view::npos) ? end : newline;
                const kimix::string_view line = s.substr(i, line_end - i);
                kimix::string_view compare = line;
                if (document.strip_tabs) {
                    size_t t = 0;
                    while (t < compare.size() && compare[t] == '\t') {
                        ++t;
                    }
                    compare = compare.substr(t);
                }
                if (logical_line.empty()) {
                    logical_start = i;
                }
                if (document.expands && heredoc_line_continues(compare)) {
                    logical_line.append(compare.data(), compare.size() - 1);
                    i = (newline == kimix::string_view::npos) ? end : newline + 1;
                    continue;
                }
                logical_line.append(compare.data(), compare.size());
                if (!document.delimiter.empty() && logical_line == document.delimiter) {
                    if (scan_expansions && document.expands) {
                        scan_heredoc_expansions(body_start, logical_start);
                    }
                    i = (newline == kimix::string_view::npos) ? end : newline + 1;
                    broke = true;
                    break;
                }
                logical_line.clear();
                i = (newline == kimix::string_view::npos) ? end : newline + 1;
            }
            if (!broke && scan_expansions && document.expands) {
                scan_heredoc_expansions(body_start, end);
            }
        }
        if (redir_line_end != kimix::string::npos) {
            heredoc_events.emplace_back(redir_line_end, i);
        }
        return i;
    }

    // -- control operators / redirections -----------------------------------
    void read_control_operator(size_t i, size_t end, kimix::string_view& op,
                               size_t& op_end) const noexcept {
        op_end = i;
        const char ch = s[i];
        if (ch == ';') {
            if (starts_with(i, ";;&")) { op = kimix::string_view(";;&", 3); op_end = i + 3; }
            else if (starts_with(i, ";;")) { op = kimix::string_view(";;", 2); op_end = i + 2; }
            else if (starts_with(i, ";&")) { op = kimix::string_view(";&", 2); op_end = i + 2; }
            else { op = kimix::string_view(";", 1); op_end = i + 1; }
            return;
        }
        if (ch == '&') {
            if (starts_with(i, "&&")) { op = kimix::string_view("&&", 2); op_end = i + 2; }
            else { op = kimix::string_view("&", 1); op_end = i + 1; }
            return;
        }
        if (ch == '|') {
            if (starts_with(i, "||")) { op = kimix::string_view("||", 2); op_end = i + 2; }
            else if (starts_with(i, "|&")) { op = kimix::string_view("|&", 2); op_end = i + 2; }
            else { op = kimix::string_view("|", 1); op_end = i + 1; }
            return;
        }
        if (ch == '(' || ch == ')') {
            op = kimix::string_view(&ch, 1);
            op_end = i + 1;
            return;
        }
        op = kimix::string_view();
    }

    void read_redirection(size_t i, size_t end, kimix::string_view& op,
                          size_t& op_end) const noexcept {
        static const char* kOps[] = {"&>>", "&>", "<<<", "<<-", "<<", ">>",
                                     "<>", ">|", "<&", ">&", "<", ">"};
        for (const char* o : kOps) {
            if (starts_with(i, o)) {
                op = kimix::string_view(o, std::strlen(o));
                op_end = i + std::strlen(o);
                return;
            }
        }
        op = kimix::string_view();
        op_end = i;
    }

    bool redirection_after_fd(size_t i, size_t end) const noexcept {
        while (i < end && ascii_digit(s[i])) {
            ++i;
        }
        return i < end && redirection_start(s[i]);
    }

    bool comment_starts(size_t i, size_t range_start) const noexcept {
        if (i <= range_start) {
            return true;
        }
        return s[i - 1] == ' ' || s[i - 1] == '\t' || s[i - 1] == '\r' ||
               s[i - 1] == '\n' || s[i - 1] == ';' || s[i - 1] == '&' ||
               s[i - 1] == '|' || s[i - 1] == '(' || s[i - 1] == ')' ||
               s[i - 1] == '<' || s[i - 1] == '>';
    }

    // -- function / wrapper helpers -----------------------------------------
    bool empty_parentheses_end(size_t i, size_t end, size_t& out) const noexcept {
        while (i < end && is_ws(s[i])) {
            ++i;
        }
        if (i >= end || s[i] != '(') {
            return false;
        }
        i += 1;
        while (i < end && is_ws(s[i])) {
            ++i;
        }
        if (i < end && s[i] == ')') {
            out = i + 1;
            return true;
        }
        return false;
    }

    bool function_declaration_end(kimix::string_view raw, size_t i, size_t end,
                                  size_t& out) const noexcept {
        if (!name_re(raw)) {
            return false;
        }
        return empty_parentheses_end(i, end, out);
    }

    enum class WrapperAction : uint8_t { SKIP, INSPECT, COMMAND };

    WrapperAction consume_wrapper_word(Wrapper& wrapper, kimix::string_view raw) {
        if (wrapper.skip_next) {
            wrapper.skip_next = false;
            return wrapper.opaque ? WrapperAction::INSPECT : WrapperAction::SKIP;
        }
        if (wrapper.opaque) {
            return WrapperAction::INSPECT;
        }
        if (wrapper.kind == WrapperKind::COMMAND && (raw == "-v" || raw == "-V")) {
            return WrapperAction::INSPECT;
        }
        if (wrapper.kind == WrapperKind::COMMAND &&
            (raw == "-p" ||
             (raw.size() > 1 && raw[0] == '-' && !(raw.size() >= 2 && raw[1] == '-') &&
              raw.find('p', 1) != kimix::string_view::npos))) {
            wrapper.opaque = true;
            return WrapperAction::SKIP;
        }
        if (wrapper.kind == WrapperKind::ENV &&
            (raw == "-S" || raw == "--split-string")) {
            wrapper.opaque = true;
            wrapper.skip_next = true;
            return WrapperAction::SKIP;
        }
        if (wrapper.kind == WrapperKind::ENV &&
            (starts_raw(raw, 0, "--split-string=") ||
             (raw.size() > 2 && starts_raw(raw, 0, "-S") && raw != "-S"))) {
            return WrapperAction::INSPECT;
        }
        if (raw == "--") {
            return WrapperAction::SKIP;
        }
        if (wrapper_option_with_value(wrapper.kind, raw)) {
            wrapper.skip_next = true;
            if (wrapper_path_option(wrapper.kind, raw)) {
                wrapper.path_value = true;
            }
            return WrapperAction::SKIP;
        }
        if (!raw.empty() && raw[0] == '-') {
            return WrapperAction::SKIP;
        }
        if (wrapper.kind == WrapperKind::ENV && assignment_re(raw)) {
            return WrapperAction::SKIP;
        }
        return WrapperAction::COMMAND;
    }

    bool wrapper_option_with_value(WrapperKind kind, kimix::string_view raw) const noexcept {
        switch (kind) {
        case WrapperKind::ENV:
            return raw == "-u" || raw == "--unset" || raw == "-C" ||
                   raw == "--chdir" || raw == "-S" || raw == "--split-string";
        case WrapperKind::EXEC:
            return raw == "-a";
        case WrapperKind::SUDO:
            return raw == "-C" || raw == "--close-from" || raw == "-D" ||
                   raw == "--chdir" || raw == "-g" || raw == "--group" ||
                   raw == "-h" || raw == "--host" || raw == "-p" ||
                   raw == "--prompt" || raw == "-R" || raw == "--chroot" ||
                   raw == "-r" || raw == "--role" || raw == "-t" ||
                   raw == "--type" || raw == "-T" || raw == "--command-timeout" ||
                   raw == "-u" || raw == "--user";
        case WrapperKind::TIME:
            return raw == "-f" || raw == "--format" || raw == "-o" ||
                   raw == "--output";
        default:
            return false;
        }
    }

    bool wrapper_path_option(WrapperKind kind, kimix::string_view raw) const noexcept {
        switch (kind) {
        case WrapperKind::ENV:
            return raw == "-C" || raw == "--chdir";
        case WrapperKind::SUDO:
            return raw == "-D" || raw == "--chdir";
        case WrapperKind::TIME:
            return raw == "-o" || raw == "--output";
        default:
            return false;
        }
    }

    bool coproc_name_before_compound(kimix::string_view raw, size_t i, size_t end) const {
        if (!name_re(raw)) {
            return false;
        }
        while (i < end && is_ws(s[i])) {
            ++i;
        }
        if (i >= end) {
            return false;
        }
        if (starts_with(i, "{") || starts_with(i, "(") || starts_with(i, "[[") ||
            starts_with(i, "((")) {
            return true;
        }
        static const char* kKeywords[] = {"case", "for", "if", "select", "until", "while"};
        for (const char* kw : kKeywords) {
            const size_t kw_len = std::strlen(kw);
            if (starts_with(i, kw)) {
                const size_t keyword_end = i + kw_len;
                if (keyword_end >= end ||
                    s[keyword_end] == ' ' || s[keyword_end] == '\t' ||
                    s[keyword_end] == '\r' || s[keyword_end] == '\n' ||
                    s[keyword_end] == ';' || s[keyword_end] == '&' ||
                    s[keyword_end] == '|' || s[keyword_end] == '(' ||
                    s[keyword_end] == ')' || s[keyword_end] == '<' ||
                    s[keyword_end] == '>' || s[keyword_end] == '{' ||
                    s[keyword_end] == '}') {
                    return true;
                }
            }
        }
        return false;
    }
    // -- Windows path normalization for Git Bash -----------------------------

    // True only when *raw* is an UNQUOTED literal ASCII ``nul``/``NUL`` word.
    // A quoted ``'nul'``/``"nul"`` is an intentional filename, and escapes /
    // expansions ($, `, \, quotes) disqualify the word for the same reason as
    // the reference (unquoted literal target only).
    bool is_nul_word(kimix::string_view raw) const noexcept {
        if (raw.size() != 3) {
            return false;
        }
        for (size_t k = 0; k < 3; ++k) {
            const char c = raw[k];
            if (c != 'n' && c != 'N' && c != 'u' && c != 'U' && c != 'l' &&
                c != 'L') {
                return false;
            }
            const char low = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
            if (k == 0 && low != 'n') {
                return false;
            }
            if (k == 1 && low != 'u') {
                return false;
            }
            if (k == 2 && low != 'l') {
                return false;
            }
        }
        return true;
    }

    void drop_cmd_cd_flag(size_t i, size_t end) {
        size_t j = i;
        while (j < end && is_ws(s[j])) {
            ++j;
        }
        if (j >= end) {
            return;
        }
        const size_t flag_end = read_word(j, end, false);
        const kimix::string_view flag = s.substr(j, flag_end - j);
        if (flag_end <= j || !(flag == "/d" || flag == "/D")) {
            return;
        }
        size_t k = flag_end;
        while (k < end && is_ws(s[k])) {
            ++k;
        }
        if (k >= end || operator_char(s[k]) || s[k] == '#') {
            return;
        }
        edits->push_back(edit{static_cast<uint32_t>(j), static_cast<uint32_t>(flag_end),
                              kimix::string()});
        if (notes) {
            notes->push_back(kimix::string("cd /d"));
        }
    }

    bool plausible_path_segments(kimix::string_view raw) const noexcept {
        size_t seg_start = 0;
        for (size_t i = 0; i <= raw.size(); ++i) {
            if (i == raw.size() || raw[i] == '\\') {
                const size_t seg_len = i - seg_start;
                if (seg_len >= 2 && ascii_alpha(raw[seg_start])) {
                    return true;
                }
                seg_start = i + 1;
            }
        }
        return false;
    }

    kimix::string decode_unquoted_word(kimix::string_view raw) const noexcept {
        kimix::string value;
        size_t i = 0;
        while (i < raw.size()) {
            const char ch = raw[i];
            if (ch == '\\' && i + 1 < raw.size()) {
                value.push_back(raw[i + 1]);
                i += 2;
            } else {
                value.push_back(ch);
                i += 1;
            }
        }
        return value;
    }

    kimix::string normalize_windows_path(kimix::string_view raw) const {
        kimix::string out;
        size_t i = 0;
        const size_t nraw = raw.size();
        if (nraw >= 2 && starts_raw(raw, 0, "\\\\")) {
            out.append("//");
            i = 2;
        }
        while (i < nraw) {
            const char ch = raw[i];
            if (ch == '\\' && i + 1 < nraw) {
                const char nxt = raw[i + 1];
                if (nxt == '\\') {
                    out.push_back('/');
                } else if (escaped_literal_char(nxt)) {
                    out.push_back(nxt);
                } else {
                    out.push_back('/');
                    out.push_back(nxt);
                }
                i += 2;
            } else if (ch == '\\') {
                out.push_back('/');
                i += 1;
            } else {
                out.push_back(ch);
                i += 1;
            }
        }
        return out;
    }

    kimix::string quote_path_word(kimix::string_view normalized) const {
        bool all_safe = true;
        for (char c : normalized) {
            if (!path_safe_char(c)) {
                all_safe = false;
                break;
            }
        }
        if (all_safe) {
            return kimix::string(normalized);
        }
        if (!normalized.empty() && normalized[0] == '~') {
            kimix::string r;
            r.push_back('~');
            r.append(quote_path_word(normalized.substr(1)));
            return r;
        }
        kimix::string escaped;
        for (char c : normalized) {
            if (c == '\\') {
                escaped.append("\\\\");
            } else if (c == '"') {
                escaped.append("\\\"");
            } else if (c == '$') {
                escaped.append("\\$");
            } else if (c == '`') {
                escaped.append("\\`");
            } else {
                escaped.push_back(c);
            }
        }
        kimix::string r;
        r.push_back('"');
        r.append(escaped);
        r.push_back('"');
        return r;
    }

    bool windows_path_replacement(kimix::string_view raw, kimix::string& replacement) const {
        if (raw.empty() || raw.find('\\') == kimix::string_view::npos) {
            return false;
        }
        size_t backslashes = 0;
        for (char ch : raw) {
            if (ch == '\\') {
                ++backslashes;
            } else if (ch == '\'' || ch == '"' || ch == '`' || ch == '$' ||
                       ch == '\n' || ch == '\r') {
                return false;
            }
        }
        if (path_drive_re(raw)) {
            // pass: drive-absolute
        } else if (raw.size() > 2 && starts_raw(raw, 0, "\\\\")) {
            // pass: UNC share
        } else if (raw.size() >= 2 && raw[0] == '\\' && raw[1] != '\\' &&
                   backslashes >= 2) {
            if (!plausible_path_segments(raw)) {
                return false;
            }
        } else if (starts_raw(raw, 0, "~\\")) {
            // pass
        } else if (starts_raw(raw, 0, ".\\") || starts_raw(raw, 0, "..\\")) {
            // pass
        } else if (backslashes >= 2) {
            const kimix::string decoded = decode_unquoted_word(raw);
            bool has_alnum = false;
            for (char c : decoded) {
                if (ascii_alnum(c)) {
                    has_alnum = true;
                    break;
                }
            }
            if (decoded.size() < 2 || !has_alnum ||
                !path_segment_re(decoded) || !plausible_path_segments(raw)) {
                return false;
            }
        } else {
            return false;
        }
        replacement = quote_path_word(normalize_windows_path(raw));
        return true;
    }

    // -- main scan ----------------------------------------------------------
    void scan_range(size_t start, size_t end) {
        if (nest_depth >= MAX_DEPTH) {
            aborted = true;
            return;
        }
        ++nest_depth;
        scan_range_inner(start, end);
        --nest_depth;
    }

    void scan_expansions(size_t i, size_t end) {
        for (;;) {
            // Bulk-skip plain code; only quoting/expansion starts matter.
            const size_t hit = detail::scan_find_table(s.data(), end, i, kExpSpecial);
            i = hit;
            if (i >= end) {
                break;
            }
            const char ch = s[i];
            if (ch == '\\') {
                i += (i + 1 < end) ? 2 : 1;
            } else if (ch == '$' && starts_with(i, "$'")) {
                i = skip_ansi_quote(i + 2, end);
            } else if (ch == '\'') {
                i = skip_single_quote(i + 1, end);
            } else if (ch == '"') {
                i = skip_double_quote(i + 1, end);
            } else if (ch == '`') {
                const size_t close = find_backtick_end(i + 1, end);
                scan_range(i + 1, close);
                i = (close < end) ? close + 1 : end;
            } else if (ch == '$' && starts_with(i, "$(") && !starts_with(i, "$((")) {
                const size_t close = find_matching(i + 2, end, ')');
                scan_range(i + 2, close < end ? close : end);
                i = (close < end) ? close + 1 : end;
            } else if (ch == '$' && starts_with(i, "$((")) {
                i = skip_arithmetic(i + 3, end);
            } else if (ch == '$' && starts_with(i, "${")) {
                i = skip_parameter(i + 2, end);
            } else {
                i += 1;
            }
        }
    }

    void scan_array_words(size_t i, size_t end) {
        while (i < end) {
            const char ch = s[i];
            if (is_ws(ch) || ch == '\n') {
                i += 1;
                continue;
            }
            if (ch == '\\' && i + 1 < end && s[i + 1] == '\n') {
                i += 2;
                continue;
            }
            if (ch == '#' && comment_starts(i, 0)) {
                const size_t newline = find_nl_bounded(i + 1, end);
                i = (newline == kimix::string_view::npos) ? end : newline;
                continue;
            }
            const size_t word_end = read_word(i, end, true);
            if (word_end <= i) {
                i += 1;
                continue;
            }
            const kimix::string_view raw = s.substr(i, word_end - i);
            kimix::string replacement;
            if (windows_path_replacement(raw, replacement)) {
                edits->push_back(edit{static_cast<uint32_t>(i),
                                      static_cast<uint32_t>(word_end), replacement});
                if (notes) {
                    notes->push_back(kimix::string(raw));
                }
            }
            i = word_end;
        }
    }

    void scan_range_inner(size_t start, size_t end) {
        size_t i = start;
        bool command_expected = true;
        bool redirect_expected = false;
        bool redirect_resume = true;
        kimix::string_view redirect_op;
        Wrapper wrapper_obj;
        bool wrapper_active = false;
        kimix::string heredoc_operator;
        bool herestring_flag = false;
        kimix::vector<HereDoc> pending_heredocs;
        kimix::vector<uint8_t> case_stack; // 0 word, 1 await-in, 2 patterns, 3 body
        bool function_name_expected = false;
        bool function_body_expected = false;

        auto clear_wrapper = [&]() { wrapper_active = false; };
        auto wrapper = [&]() -> Wrapper* {
            return wrapper_active ? &wrapper_obj : nullptr;
        };

        while (i < end) {
            const char ch = s[i];

            if (is_ws(ch)) {
                i += 1;
                continue;
            }
            if (ch == '\\' && i + 1 < end && s[i + 1] == '\n') {
                i += 2;
                continue;
            }
            if (ch == '\n') {
                i += 1;
                if (!pending_heredocs.empty()) {
                    i = skip_heredoc_bodies(i, end, pending_heredocs, true);
                    pending_heredocs.clear();
                }
                command_expected = true;
                redirect_expected = false;
                redirect_op = kimix::string_view();
                heredoc_operator.clear();
                herestring_flag = false;
                clear_wrapper();
                continue;
            }
            if (ch == '#' && comment_starts(i, start)) {
                const size_t newline = find_nl_bounded(i + 1, end);
                i = (newline == kimix::string_view::npos) ? end : newline;
                continue;
            }

            const bool process_substitution =
                redirection_start(ch) && (starts_with(i, "<(") || starts_with(i, ">("));
            if (!process_substitution &&
                (redirection_start(ch) ||
                 (ch == '&' && starts_with(i, "&>")) ||
                 (ascii_digit(ch) && redirection_after_fd(i, end)))) {
                size_t op_start = i;
                if (ascii_digit(ch)) {
                    while (i < end && ascii_digit(s[i])) {
                        ++i;
                    }
                }
                kimix::string_view op;
                read_redirection(i, end, op, i);
                if (!op.empty()) {
                    redirect_resume = command_expected;
                    redirect_expected = true;
                    redirect_op = op;
                    herestring_flag = (op == "<<<");
                    if (op == "<<" || op == "<<-") {
                        heredoc_operator = kimix::string(op);
                    }
                    continue;
                }
                i = op_start;
            }

            if (redirect_expected) {
                size_t word_end;
                if (starts_with(i, "<(") || starts_with(i, ">(")) {
                    const size_t close = find_matching(i + 2, end, ')');
                    scan_range(i + 2, close < end ? close : end);
                    word_end = (close < end) ? close + 1 : end;
                } else {
                    const bool scan_substitutions = !(heredoc_operator == "<<" ||
                                                      heredoc_operator == "<<-");
                    word_end = read_word(i, end, scan_substitutions);
                }
                if (word_end <= i) {
                    i += 1;
                    continue;
                }
                if (heredoc_operator == "<<" || heredoc_operator == "<<-") {
                    kimix::string delimiter;
                    bool expands = false;
                    if (heredoc_delimiter(s.substr(i, word_end - i), delimiter, expands)) {
                        pending_heredocs.push_back(
                            HereDoc{delimiter, heredoc_operator == "<<-", expands});
                    }
                } else if (!herestring_flag) {
                    const kimix::string_view raw_word = s.substr(i, word_end - i);
                    // ``> nul`` / ``2> NUL`` / ``&> nul`` / ``>> nul`` would
                    // silently create a file literally named ``nul`` in Git
                    // Bash.  Only an *unquoted* literal ``nul`` target is
                    // rewritten (quoted ``'nul'`` is an intentional filename);
                    // input redirections (``< nul``) never create a file.
                    if (!redirect_op.empty() && redirect_op[0] != '<' &&
                        is_nul_word(raw_word)) {
                        edits->push_back(edit{static_cast<uint32_t>(i),
                                              static_cast<uint32_t>(word_end),
                                              kimix::string("/dev/null")});
                        if (nul_notes) {
                            nul_notes->push_back(kimix::string(raw_word));
                        }
                    } else {
                        kimix::string replacement;
                        if (windows_path_replacement(raw_word, replacement)) {
                            edits->push_back(edit{static_cast<uint32_t>(i),
                                                  static_cast<uint32_t>(word_end),
                                                  replacement});
                            if (notes) {
                                notes->push_back(kimix::string(raw_word));
                            }
                        }
                    }
                }
                i = word_end;
                command_expected = redirect_resume;
                redirect_expected = false;
                redirect_op = kimix::string_view();
                heredoc_operator.clear();
                continue;
            }

            if (starts_with(i, "[[") && ch == '[') {
                function_body_expected = false;
                i = skip_conditional(i + 2, end);
                command_expected = false;
                continue;
            }
            if (starts_with(i, "((") && ch == '(') {
                function_body_expected = false;
                i = skip_arithmetic(i + 2, end);
                command_expected = false;
                continue;
            }
            if (ch == '$' && starts_with(i, "$(") && !starts_with(i, "$((")) {
                const size_t close = find_matching(i + 2, end, ')');
                const size_t inner_end = (close < end) ? close : end;
                scan_range(i + 2, inner_end);
                i = (close < end) ? close + 1 : end;
                if (!case_stack.empty() && case_stack.back() == 0) {
                    case_stack.back() = 1;
                }
                command_expected = false;
                continue;
            }
            if (ch == '`') {
                const size_t close = find_backtick_end(i + 1, end);
                scan_range(i + 1, close);
                i = (close < end) ? close + 1 : end;
                if (!case_stack.empty() && case_stack.back() == 0) {
                    case_stack.back() = 1;
                }
                command_expected = false;
                continue;
            }
            if (redirection_start(ch) &&
                (starts_with(i, "<(") || starts_with(i, ">("))) {
                const size_t close = find_matching(i + 2, end, ')');
                scan_range(i + 2, close < end ? close : end);
                i = (close < end) ? close + 1 : end;
                if (!case_stack.empty() && case_stack.back() == 0) {
                    case_stack.back() = 1;
                }
                command_expected = false;
                continue;
            }

            kimix::string_view op;
            size_t op_end = i;
            read_control_operator(i, end, op, op_end);
            if (!op.empty()) {
                i = op_end;
                if (op == "(" && function_body_expected) {
                    function_body_expected = false;
                    command_expected = true;
                } else if (op == "(") {
                    command_expected = true;
                } else if (op == ")") {
                    if (!case_stack.empty() && case_stack.back() == 2) {
                        case_stack.back() = 3;
                        command_expected = true;
                    } else {
                        command_expected = false;
                    }
                } else if (op == ";;" || op == ";&" || op == ";;&") {
                    if (!case_stack.empty()) {
                        case_stack.back() = 2;
                        command_expected = false;
                    } else {
                        command_expected = true;
                    }
                } else {
                    command_expected = true;
                }
                redirect_expected = false;
                heredoc_operator.clear();
                clear_wrapper();
                continue;
            }

            const size_t word_start = i;
            const bool scan_substitutions = !(heredoc_operator == "<<" ||
                                              heredoc_operator == "<<-");
            const size_t word_end = read_word(i, end, scan_substitutions);
            if (word_end <= i) {
                i += 1;
                continue;
            }
            const kimix::string_view raw = s.substr(word_start, word_end - word_start);
            i = word_end;

            if (function_name_expected) {
                function_name_expected = false;
                function_body_expected = true;
                command_expected = false;
                size_t declaration_end = 0;
                if (empty_parentheses_end(i, end, declaration_end)) {
                    i = declaration_end;
                }
                continue;
            }

            if (function_body_expected) {
                function_body_expected = false;
                if (raw == "{") {
                    command_expected = true;
                    continue;
                }
            }

            if (!case_stack.empty() && case_stack.back() == 0) {
                case_stack.back() = 1;
                command_expected = false;
                continue;
            }
            if (!case_stack.empty() && case_stack.back() == 1 && raw == "in") {
                case_stack.back() = 2;
                command_expected = false;
                continue;
            }
            if (!case_stack.empty() && case_stack.back() == 2) {
                if (raw == "esac") {
                    case_stack.pop_back();
                }
                command_expected = false;
                continue;
            }

            if (!command_expected) {
                if (raw == "then" || raw == "do" || raw == "else" || raw == "elif") {
                    command_expected = true;
                } else if (raw == "esac" && !case_stack.empty()) {
                    case_stack.pop_back();
                } else {
                    kimix::string replacement;
                    if (windows_path_replacement(raw, replacement)) {
                        edits->push_back(edit{static_cast<uint32_t>(word_start),
                                              static_cast<uint32_t>(word_end),
                                              replacement});
                        if (notes) {
                            notes->push_back(kimix::string(raw));
                        }
                    }
                    if (assignment_re(raw) && i < end && s[i] == '(') {
                        const size_t close = find_matching(i + 1, end, ')');
                        scan_array_words(i + 1, close < end ? close : end);
                        i = (close < end) ? close + 1 : end;
                    }
                }
                continue;
            }

            if (raw == "function") {
                function_name_expected = true;
                command_expected = true;
                continue;
            }
            size_t declaration_end = 0;
            if (function_declaration_end(raw, i, end, declaration_end)) {
                i = declaration_end;
                function_body_expected = true;
                command_expected = false;
                continue;
            }
            if (is_command_start_keyword(raw)) {
                command_expected = true;
                continue;
            }
            if (is_command_end_keyword(raw)) {
                if (raw == "esac" && !case_stack.empty()) {
                    case_stack.pop_back();
                }
                command_expected = false;
                continue;
            }
            if (is_list_keyword(raw)) {
                if (raw == "case") {
                    case_stack.push_back(0);
                }
                command_expected = false;
                continue;
            }
            if (assignment_re(raw)) {
                if (i < end && s[i] == '(') {
                    const size_t close = find_matching(i + 1, end, ')');
                    scan_array_words(i + 1, close < end ? close : end);
                    i = (close < end) ? close + 1 : end;
                }
                command_expected = true;
                continue;
            }

            if (raw == "cd") {
                drop_cmd_cd_flag(i, end);
            }

            const bool executable_wrapper =
                wrapper_active && wrapper_obj.kind != WrapperKind::COPROC &&
                wrapper_obj.kind != WrapperKind::TIME;
            if (wrapper_active && wrapper_obj.kind == WrapperKind::COPROC) {
                if (coproc_name_before_compound(raw, i, end)) {
                    clear_wrapper();
                    command_expected = true;
                    continue;
                }
            }
            bool inline_consumed = false;
            if (wrapper_active &&
                (wrapper_obj.kind == WrapperKind::ENV ||
                 wrapper_obj.kind == WrapperKind::SUDO ||
                 wrapper_obj.kind == WrapperKind::TIME)) {
                static const char* kLong[] = {"--chdir", "--output"};
                for (const char* option : kLong) {
                    const size_t olen = std::strlen(option);
                    if (raw.size() > olen && starts_raw(raw, 0, option) &&
                        raw[olen] == '=') {
                        const kimix::string_view value =
                            raw.substr(olen + 1, raw.size() - olen - 1);
                        kimix::string replacement;
                        if (windows_path_replacement(value, replacement)) {
                            edits->push_back(edit{
                                static_cast<uint32_t>(word_start),
                                static_cast<uint32_t>(word_end),
                                kimix::string(option) + "=" + replacement});
                            if (notes) {
                                notes->push_back(kimix::string(raw));
                            }
                        }
                        wrapper_obj.skip_next = false;
                        wrapper_obj.path_value = false;
                        command_expected = true;
                        inline_consumed = true;
                        break;
                    }
                }
            }
            if (inline_consumed) {
                continue;
            }
            if (wrapper_active) {
                const bool path_option_value =
                    wrapper_obj.path_value && wrapper_obj.skip_next;
                const WrapperAction action =
                    consume_wrapper_word(wrapper_obj, raw);
                if (action == WrapperAction::SKIP) {
                    if (path_option_value) {
                        kimix::string replacement;
                        if (windows_path_replacement(raw, replacement)) {
                            edits->push_back(edit{static_cast<uint32_t>(word_start),
                                                  static_cast<uint32_t>(word_end),
                                                  replacement});
                            if (notes) {
                                notes->push_back(kimix::string(raw));
                            }
                        }
                    }
                    command_expected = true;
                    continue;
                }
                if (action == WrapperAction::INSPECT) {
                    command_expected = false;
                    clear_wrapper();
                    continue;
                }
            }

            // Fallback command wrappers (_FALLBACK_COMMAND_WRAPPERS) are
            // checked before the plain wrapper table: ``sudo`` is both a plain
            // command wrapper (operand semantics, option tables) and a
            // fallback name whose definition must be recorded for hosts
            // without ``sudo.exe``.  The reference appends the RAW word (the
            // lookup does no quote removal), so ``'sudo'`` stays a plain
            // literal fallback word and is handled by the branch below.
            const kimix::string_view fallback_kind = fallback_wrapper_kind(raw);
            if (!fallback_kind.empty()) {
                if (names) {
                    names->push_back(kimix::string(raw));
                }
                if (executable_wrapper) {
                    // The wrapping executable (``env sudo ...``) cannot invoke
                    // shell functions: swap the word for the standalone
                    // runner, exactly like an ordinary fallback name.
                    edits->push_back(edit{static_cast<uint32_t>(word_start),
                                          static_cast<uint32_t>(word_end),
                                          fallback_marker(raw)});
                }
                if (fallback_kind == "sudo") {
                    wrapper_obj = Wrapper{WrapperKind::SUDO};
                    wrapper_active = true;
                } else {
                    // "timeout" / "watch" wrapper semantics (option tables,
                    // the mandatory DURATION operand, quoted ``watch``
                    // scripts) are not ported in this kernel; the name is
                    // still recorded so the fallback definition matches the
                    // reference.  The shim routes those commands to the
                    // pure-Python reference.
                    clear_wrapper();
                }
                command_expected = true;
                continue;
            }

            WrapperKind kind;
            if (wrapper_kind_of(raw, kind)) {
                wrapper_obj = Wrapper{kind};
                wrapper_active = true;
                command_expected = true;
                continue;
            }

            kimix::string fallback_name;
            if (literal_command_name(raw, fallback_name)) {
                if (names) {
                    names->push_back(fallback_name);
                }
                if (executable_wrapper) {
                    edits->push_back(edit{static_cast<uint32_t>(word_start),
                                          static_cast<uint32_t>(word_end),
                                          fallback_marker(fallback_name)});
                }
            } else {
                // _record_cmd_jump comes before _record_unsupported in the
                // reference: an untranslatable cmd.exe batch jump records its
                // refusal key in the same `unsupported` channel and leaves the
                // text byte-for-byte (no path rewrite either).
                kimix::string jump_key = cmd_jump_key(raw, word_end, end);
                kimix::string unsupported_name;
                if (!jump_key.empty()) {
                    if (unsupported &&
                        !bash_fix_contains_name(*unsupported, jump_key)) {
                        unsupported->push_back(jump_key);
                    }
                } else if (literal_unsupported_name(raw, unsupported_name)) {
                    // _UNSUPPORTED_BODIES: no faithful Git Bash equivalent.
                    // The name is recorded (each name once, like the
                    // reference's `if name not in self.unsupported`) and the
                    // command text is left byte-for-byte -- in particular the
                    // word is NOT treated as a Windows executable path.
                    if (unsupported && !bash_fix_contains_name(*unsupported,
                                                      unsupported_name)) {
                        unsupported->push_back(unsupported_name);
                    }
                } else {
                    kimix::string replacement;
                    if (windows_path_replacement(raw, replacement)) {
                        edits->push_back(
                            edit{static_cast<uint32_t>(word_start),
                                 static_cast<uint32_t>(word_end), replacement});
                        if (notes) {
                            notes->push_back(kimix::string(raw));
                        }
                    }
                }
            }
            command_expected = false;
            clear_wrapper();
        }
    }
};

namespace src_runtime_parse_shell_scanner {

bool is_heredoc_trailing_operator(kimix::string_view op) noexcept {
    return op == "&&" || op == "||" || op == "|" || op == "|&" || op == ";" ||
           op == "&";
}

void read_control_operator_for_heredoc(kimix::string_view s, size_t i, size_t n,
                                       kimix::string_view& op,
                                       size_t& op_end) noexcept {
    op_end = i;
    if (i >= n) {
        op = kimix::string_view();
        return;
    }
    const char ch = s[i];
    if (ch == ';') {
        if (i + 3 <= n && s.substr(i, 3) == ";;&") {
            op = kimix::string_view(";;&", 3);
            op_end = i + 3;
        } else if (i + 2 <= n && s.substr(i, 2) == ";;") {
            op = kimix::string_view(";;", 2);
            op_end = i + 2;
        } else if (i + 2 <= n && s.substr(i, 2) == ";&") {
            op = kimix::string_view(";&", 2);
            op_end = i + 2;
        } else {
            op = kimix::string_view(";", 1);
            op_end = i + 1;
        }
        return;
    }
    if (ch == '&') {
        if (i + 2 <= n && s.substr(i, 2) == "&&") {
            op = kimix::string_view("&&", 2);
            op_end = i + 2;
        } else {
            op = kimix::string_view("&", 1);
            op_end = i + 1;
        }
        return;
    }
    if (ch == '|') {
        if (i + 2 <= n && s.substr(i, 2) == "||") {
            op = kimix::string_view("||", 2);
            op_end = i + 2;
        } else if (i + 2 <= n && s.substr(i, 2) == "|&") {
            op = kimix::string_view("|&", 2);
            op_end = i + 2;
        } else {
            op = kimix::string_view("|", 1);
            op_end = i + 1;
        }
        return;
    }
    op = kimix::string_view();
}

size_t skip_blank_and_comments(kimix::string_view s, size_t i, size_t n) noexcept {
    while (i < n) {
        const char ch = s[i];
        if (ch == ' ' || ch == '\t' || ch == '\r') {
            ++i;
            continue;
        }
        if (ch == '\n') {
            ++i;
            continue;
        }
        if (ch == '#') {
            const size_t nl = s.find('\n', i);
            i = (nl == kimix::string_view::npos) ? n : nl + 1;
            continue;
        }
        break;
    }
    return i;
}

bool apply_heredoc_operator_move(kimix::string& source,
                                 size_t redir_line_end,
                                 size_t terminator_end) {
    const size_t n = source.size();
    if (redir_line_end >= n || source[redir_line_end] != '\n') {
        return false;
    }
    if (terminator_end > n) {
        return false;
    }
    size_t i = skip_blank_and_comments(source, terminator_end, n);
    if (i >= n) {
        return false;
    }
    kimix::string_view op;
    size_t op_end = i;
    read_control_operator_for_heredoc(source, i, n, op, op_end);
    if (op.empty() || !is_heredoc_trailing_operator(op)) {
        return false;
    }
    // ``&>`` / ``&>>`` are redirections, not list terminators.
    if (op == "&" && op_end < n && source[op_end] == '>') {
        return false;
    }
    const size_t move_start = i;
    const size_t first_line_end = source.find('\n', i);
    size_t line_end = (first_line_end == kimix::string::npos) ? n : first_line_end;
    size_t move_end = (first_line_end == kimix::string::npos) ? n : first_line_end + 1;

    size_t rest_start = op_end;
    while (rest_start < line_end &&
           (source[rest_start] == ' ' || source[rest_start] == '\t' ||
            source[rest_start] == '\r')) {
        ++rest_start;
    }
    const kimix::string_view rest = source.substr(rest_start, line_end - rest_start);
    if (rest.empty() || rest.starts_with('#')) {
        const size_t k = skip_blank_and_comments(source, move_end, n);
        if (k >= n) {
            return false;
        }
        const size_t next_line_end = source.find('\n', k);
        move_end = (next_line_end == kimix::string::npos) ? n : next_line_end + 1;
        line_end = move_end;
    }

    kimix::string moved;
    moved.append(source, move_start, move_end - move_start);
    while (!moved.empty() && moved.back() == '\n') {
        moved.pop_back();
    }
    kimix::string joined;
    size_t a = 0;
    while (a < moved.size()) {
        const size_t nl = moved.find('\n', a);
        const kimix::string_view part_view =
            (nl == kimix::string::npos)
                ? kimix::string_view(moved.data() + a, moved.size() - a)
                : kimix::string_view(moved.data() + a, nl - a);
        kimix::string_view part = part_view;
        if (a == 0) {
            part = part.substr(op.size());
        }
        size_t p = 0;
        size_t q = part.size();
        while (p < q && (part[p] == ' ' || part[p] == '\t' || part[p] == '\r')) {
            ++p;
        }
        while (q > p &&
               (part[q - 1] == ' ' || part[q - 1] == '\t' || part[q - 1] == '\r')) {
            --q;
        }
        part = part.substr(p, q - p);
        if (part.starts_with('#')) {
            a = (nl == kimix::string::npos) ? moved.size() : nl + 1;
            continue;
        }
        if (!joined.empty()) {
            joined.push_back(' ');
        }
        joined.append(part.data(), part.size());
        a = (nl == kimix::string::npos) ? moved.size() : nl + 1;
    }

    kimix::string replacement;
    replacement.reserve(op.size() + 1 + joined.size() + 1);
    replacement.append(op.data(), op.size());
    replacement.push_back(' ');
    replacement.append(joined);
    replacement.push_back('\n');

    kimix::string out;
    out.reserve(source.size() + replacement.size());
    out.append(source, 0, redir_line_end);
    out.push_back(' ');
    out.append(replacement);
    if (move_start > redir_line_end + 1) {
        out.append(source, redir_line_end + 1, move_start - (redir_line_end + 1));
    }
    if (move_end < n) {
        out.append(source, move_end, n - move_end);
    }
    source = std::move(out);
    return true;
}

bool fix_heredoc_trailing_operators(kimix::string& source) {
    BashFixScanner scanner;
    scanner.s = source;
    scanner.n = source.size();
    kimix::vector<edit> dummy_edits;
    kimix::vector<kimix::string> dummy_names;
    kimix::vector<kimix::string> dummy_notes;
    scanner.edits = &dummy_edits;
    scanner.names = &dummy_names;
    scanner.notes = &dummy_notes;
    scanner.scan_range(0, source.size());
    if (scanner.aborted || scanner.heredoc_events.empty()) {
        return false;
    }
    bool changed = false;
    for (auto it = scanner.heredoc_events.rbegin();
         it != scanner.heredoc_events.rend(); ++it) {
        if (apply_heredoc_operator_move(source, it->first, it->second)) {
            changed = true;
        }
    }
    return changed;
}

} // namespace src_runtime_parse_shell_scanner
using namespace src_runtime_parse_shell_scanner;

void scan_bash_fix(kimix::string_view cmd, kimix::vector<edit>& edits,
                   kimix::string* transformed,
                   kimix::vector<kimix::string>* names,
                   kimix::vector<kimix::string>* notes,
                   kimix::vector<kimix::string>* nul_notes,
                   kimix::vector<kimix::string>* unsupported) {
    edits.clear();
    if (names) {
        names->clear();
    }
    if (notes) {
        notes->clear();
    }
    if (nul_notes) {
        nul_notes->clear();
    }
    if (unsupported) {
        unsupported->clear();
    }
    if (transformed) {
        transformed->clear();
    }
    if (cmd.empty()) {
        return;
    }
    BashFixScanner scanner;
    scanner.s = cmd;
    scanner.n = cmd.size();
    scanner.edits = &edits;
    scanner.names = names;
    scanner.notes = notes;
    scanner.nul_notes = nul_notes;
    scanner.unsupported = unsupported;
    scanner.scan_range(0, cmd.size());
    if (scanner.aborted) {
        // Reference behavior: RecursionError -> BashFix(cmd) unchanged.
        edits.clear();
        if (names) {
            names->clear();
        }
        if (notes) {
            notes->clear();
        }
        if (nul_notes) {
            nul_notes->clear();
        }
        if (unsupported) {
            unsupported->clear();
        }
        return;
    }
    // The heredoc-trailing-operator repair rescans the edited source to find
    // <op> placements after heredoc terminators. When the primary scan never
    // consumed a heredoc body (heredoc_events is empty), the edited source
    // has no heredoc structure either (edits only rewrite path words, never
    // insert '<<'), so the repair pass is a guaranteed no-op — skip it.
    kimix::string source;
    if (!scanner.heredoc_events.empty()) {
        apply_edits(cmd, edits, &source);
        if (fix_heredoc_trailing_operators(source)) {
            edits.clear();
            edits.push_back(edit{0, static_cast<uint32_t>(cmd.size()), source});
        }
    }
}

// ===========================================================================
// PWSH_TRANSFORM - process_pwsh.py::pwsh_transform
// ===========================================================================

inline bool py_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
           c == '\f';
}

bool is_ps_keyword(kimix::string_view word) noexcept {
    static const char* kKeywords[] = {
        "begin", "break", "catch", "class", "continue", "data", "define",
        "do", "dynamicparam", "else", "elseif", "end", "enum", "exit",
        "filter", "finally", "for", "foreach", "from", "function",
        "hidden", "if", "in", "param", "process", "return", "static",
        "switch", "throw", "trap", "try", "until", "using", "var",
        "while",
    };
    for (const char* kw : kKeywords) {
        if (word == kimix::string_view(kw)) {
            return true;
        }
    }
    return false;
}

// -- region mask builders ---------------------------------------------------

size_t skip_subexpression(kimix::string_view code, size_t start) noexcept;

size_t scan_single_quoted(kimix::string_view code, size_t i) noexcept {
    i += 1;
    const size_t n = code.size();
    for (;;) {
        const size_t pos = detail::scan_find_char(code.data(), n, i, '\'');
        if (pos >= n) {
            return n;
        }
        if (pos + 1 < n && code[pos + 1] == '\'') {
            i = pos + 2;
        } else {
            return pos + 1;
        }
    }
}

size_t scan_double_quoted(kimix::string_view code, size_t i) noexcept {
    i += 1;
    const size_t n = code.size();
    for (;;) {
        const size_t hit = detail::scan_find_any(code.data(), n, i, "\"`$", 3);
        if (hit >= n) {
            return n;
        }
        const char ch = code[hit];
        if (ch == '`' && hit + 1 < n) {
            i = hit + 2;
        } else if (ch == '"') {
            return hit + 1;
        } else if (ch == '$' && hit + 1 < n && code[hit + 1] == '(') {
            i = skip_subexpression(code, hit);
        } else {
            i = hit + 1;
        }
    }
}

size_t scan_block_comment(kimix::string_view code, size_t i) noexcept {
    // NESTED <# ... #> (process_pwsh semantics; differs from pwsh_fix)
    size_t depth = 1;
    i += 2;
    const size_t n = code.size();
    while (depth) {
        const size_t hit = detail::scan_find_any(code.data(), n, i, "<#", 2);
        if (hit >= n) {
            return n;
        }
        if (code[hit] == '<' && hit + 1 < n && code[hit + 1] == '#') {
            depth += 1;
            i = hit + 2;
        } else if (code[hit] == '#' && hit + 1 < n && code[hit + 1] == '>') {
            depth -= 1;
            i = hit + 2;
            if (depth == 0) {
                return i;
            }
        } else {
            i = hit + 1;
        }
    }
    return i;
}

size_t skip_subexpression(kimix::string_view code, size_t start) noexcept {
    size_t i = start + 2;
    size_t depth = 1;
    const size_t n = code.size();
    while (i < n && depth) {
        const char c = code[i];
        if (c == '(') {
            depth += 1;
            i += 1;
        } else if (c == ')') {
            depth -= 1;
            i += 1;
        } else if (c == '\'') {
            i = scan_single_quoted(code, i);
        } else if (c == '"') {
            i = scan_double_quoted(code, i);
        } else if (c == '$' && i + 1 < n && code[i + 1] == '(') {
            i = skip_subexpression(code, i);
        } else {
            i += 1;
        }
    }
    return i;
}

size_t scan_here_string(kimix::string_view code, size_t start) noexcept {
    const size_t n = code.size();
    const char quote = code[start + 1];
    size_t i = start + 2;
    size_t line_begin = start + 2;
    const char needles[2] = {'\n', quote};
    for (;;) {
        const size_t hit = detail::scan_find_any(code.data(), n, i, needles, 2);
        if (hit >= n) {
            return n;
        }
        if (code[hit] == '\n') {
            line_begin = hit + 1;
            i = hit + 1;
        } else if (hit + 1 < n && code[hit + 1] == '@' &&
                   code.substr(line_begin, hit - line_begin)
                           .find_first_not_of(" \t\r\n\v\f") ==
                       kimix::string_view::npos) {
            return hit + 2;
        } else {
            i = hit + 1;
        }
    }
}

inline bool buffer_is_word_char(char c) noexcept {
    return ascii_alnum(c) || c == '_';
}

// mask: all positions initially code; mark(start,end) clears them.
void build_region_mask(kimix::string_view code, bool here_strings,
                       RegionMask& mask, bool hash_token_boundary = false) {
    const size_t n = code.size();
    size_t i = 0;
    while (i < n) {
        const char c = code[i];
        if (c == '<' && i + 1 < n && code[i + 1] == '#') {
            const size_t start = i;
            i = scan_block_comment(code, i);
            mask.mark(static_cast<uint32_t>(start), static_cast<uint32_t>(i));
        } else if (c == '#') {
            // ``#`` opens a comment only at a token boundary: PowerShell treats
            // ``foo#c`` (a hash glued to an identifier character) as one
            // bareword token, so the rest of the line stays code (reference
            // ``_mask_code_span``; without this the mask swallows any
            // ``> nul`` / ``??`` / ``?.`` behind a glued hash).
            if (hash_token_boundary && i > 0 &&
                buffer_is_word_char(code[i - 1])) {
                i += 1;
                continue;
            }
            const size_t start = i;
            i = detail::scan_find_char(code.data(), n, i, '\n');
            mask.mark(static_cast<uint32_t>(start), static_cast<uint32_t>(i));
        } else if (here_strings && c == '@' && i + 1 < n &&
                   (code[i + 1] == '\'' || code[i + 1] == '"')) {
            size_t j = i + 2;
            while (j < n && (code[j] == ' ' || code[j] == '\t' || code[j] == '\r')) {
                ++j;
            }
            if (j < n && code[j] != '\n') {
                i += 1;
                continue;
            }
            const size_t start = i;
            i = scan_here_string(code, i);
            mask.mark(static_cast<uint32_t>(start), static_cast<uint32_t>(i));
        } else if (c == '\'') {
            const size_t start = i;
            i = scan_single_quoted(code, i);
            mask.mark(static_cast<uint32_t>(start), static_cast<uint32_t>(i));
        } else if (c == '"') {
            const size_t start = i;
            i = scan_double_quoted(code, i);
            mask.mark(static_cast<uint32_t>(start), static_cast<uint32_t>(i));
        } else {
            i += 1;
        }
    }
}

// -- PS7 -> PS5.1 transform: three-state region mask ------------------------
//
// _build_region_mask / _mask_code_span / _mask_double_quoted: byte 1 marks
// ordinary code, byte 2 marks live code inside a ``$( ... )`` interpolation of
// a double-quoted string, byte 0 marks strings / comments / here-strings.  The
// transforms treat both 1 and 2 as code; only the chain pass distinguishes them
// (phase 1 rewrites plain code, phase 2 the interpolated kind).
using pt_mask = kimix::vector<uint8_t>;

constexpr size_t pt_npos = kimix::string_view::npos;

inline char pt_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool pt_code(const pt_mask& mask, size_t i) noexcept {
    return i < mask.size() && mask[i] != 0;
}

void pt_mask_fill(pt_mask& mask, size_t start, size_t end, uint8_t value) {
    const size_t n = std::min(end, mask.size());
    for (size_t k = start; k < n; ++k) {
        mask[k] = value;
    }
}

inline bool pt_starts_with(kimix::string_view s,
                           kimix::string_view prefix) noexcept {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

inline bool pt_ends_with(kimix::string_view s,
                         kimix::string_view suffix) noexcept {
    return s.size() >= suffix.size() &&
           s.substr(s.size() - suffix.size()) == suffix;
}

bool expr_stop(char c) noexcept {
    return c == '=' || c == ';' || c == '|' || c == '&' || c == ',';
}

bool depth_open(char c) noexcept { return c == '(' || c == '[' || c == '{'; }
bool depth_close(char c) noexcept { return c == ')' || c == ']' || c == '}'; }

kimix::string lstrip_py(kimix::string_view s) {
    size_t i = 0;
    while (i < s.size() && py_space(s[i])) {
        ++i;
    }
    return kimix::string(s.substr(i));
}

kimix::string rstrip_py(kimix::string_view s) {
    size_t end = s.size();
    while (end > 0 && py_space(s[end - 1])) {
        --end;
    }
    return kimix::string(s.substr(0, end));
}

kimix::string strip_py(kimix::string_view s) {
    return lstrip_py(rstrip_py(s));
}

// _scan_block_comment: PowerShell block comments do NOT nest in the transformer
// (the comment ends at the first ``#>``), unlike the pwsh_fix scanner above.
size_t pt_scan_block_comment(kimix::string_view code, size_t i) noexcept {
    i += 2;
    const size_t n = code.size();
    while (i < n) {
        if (code[i] == '#' && i + 1 < n && code[i + 1] == '>') {
            return i + 2;
        }
        ++i;
    }
    return i;
}

// _scan_here_string: the closing delimiter must start its own line and a
// newline must have been seen after the opener.
size_t pt_scan_here_string(kimix::string_view code, size_t start) noexcept {
    const size_t n = code.size();
    const char quote = code[start + 1];
    size_t i = start + 2;
    bool seen_newline = false;
    size_t line_begin = start + 2;
    while (i < n) {
        if (code[i] == '\n') {
            seen_newline = true;
            line_begin = i + 1;
        } else if (code[i] == quote && i + 1 < n && code[i + 1] == '@' &&
                   seen_newline) {
            if (code.substr(line_begin, i - line_begin)
                    .find_first_not_of(" \t\r\n\v\f") == pt_npos) {
                return i + 2;
            }
        }
        ++i;
    }
    return i;
}

size_t pt_mask_double_quoted(kimix::string_view code, pt_mask& mask,
                             size_t start, size_t hi, bool here_strings);

// _mask_code_span: mask region (0) characters within the code span [lo, hi).
void pt_mask_code_span(kimix::string_view code, pt_mask& mask, size_t lo,
                       size_t hi, bool here_strings) {
    size_t i = lo;
    while (i < hi) {
        size_t hit = pt_npos;
        for (size_t k = i; k < hi; ++k) {
            const char c = code[k];
            if (c == '#' || c == '<' || c == '@' || c == '\'' || c == '"') {
                hit = k;
                break;
            }
        }
        if (hit == pt_npos) {
            break;
        }
        i = hit;
        const char c = code[i];
        if (c == '<' && i + 1 < hi && code[i + 1] == '#') {
            const size_t start = i;
            i = std::min(pt_scan_block_comment(code, i), hi);
            pt_mask_fill(mask, start, i, 0);
        } else if (c == '#') {
            // ``#`` opens a comment only at a token boundary.
            if (i > lo && (ascii_alnum(code[i - 1]) || code[i - 1] == '_')) {
                i += 1;
                continue;
            }
            const size_t start = i;
            const size_t nl = code.find('\n', i);
            i = (nl == pt_npos || nl >= hi) ? hi : nl;
            pt_mask_fill(mask, start, i, 0);
        } else if (c == '@' && i + 1 < hi &&
                   (code[i + 1] == '\'' || code[i + 1] == '"')) {
            if (!here_strings) {
                i += 1;
                continue;
            }
            size_t j = i + 2;
            while (j < hi && (code[j] == ' ' || code[j] == '\t' ||
                              code[j] == '\r')) {
                ++j;
            }
            if (j >= hi || code[j] != '\n') {
                i += 1;
                continue;
            }
            const size_t start = i;
            i = std::min(pt_scan_here_string(code, i), hi);
            pt_mask_fill(mask, start, i, 0);
        } else if (c == '\'') {
            const size_t start = i;
            i = std::min(scan_single_quoted(code, i), hi);
            pt_mask_fill(mask, start, i, 0);
        } else if (c == '"') {
            i = pt_mask_double_quoted(code, mask, i, hi, here_strings);
        } else {
            i += 1;
        }
    }
}

// _mask_double_quoted: string text is masked 0, ``$( ... )`` interpolations are
// live code (2) with nested strings/comments masked 0 again.
size_t pt_mask_double_quoted(kimix::string_view code, pt_mask& mask,
                             size_t start, size_t hi, bool here_strings) {
    const size_t n = std::min(code.size(), hi);
    size_t i = start + 1;
    mask[start] = 0;
    while (i < n) {
        const char ch = code[i];
        if (ch == '`' && i + 1 < n) {
            mask[i] = 0;
            mask[i + 1] = 0;
            i += 2;
        } else if (ch == '"') {
            mask[i] = 0;
            return i + 1;
        } else if (ch == '$' && i + 1 < n && code[i + 1] == '(') {
            const size_t sub_end = std::min(skip_subexpression(code, i), n);
            mask[i] = 2;
            mask[i + 1] = 2;
            if (sub_end > i + 2) {
                mask[sub_end - 1] = 2;
                pt_mask_code_span(code, mask, i + 2, sub_end - 1, here_strings);
                for (size_t k = i + 2; k < sub_end - 1; ++k) {
                    if (mask[k] == 1) {
                        mask[k] = 2;
                    }
                }
            }
            i = sub_end;
        } else {
            mask[i] = 0;
            ++i;
        }
    }
    return i;
}

// _build_region_mask: one pass over the whole (or a single line of) text.
pt_mask pt_build_region_mask(kimix::string_view code, bool here_strings) {
    pt_mask mask(code.size(), static_cast<uint8_t>(1));
    pt_mask_code_span(code, mask, 0, code.size(), here_strings);
    return mask;
}

// _line_mask: here-strings disabled (a single line cannot open one reliably).
pt_mask pt_line_mask(kimix::string_view line) {
    return pt_build_region_mask(line, false);
}

// _compute_depths: nesting depth of () {} [] before each character.
void pt_compute_depths(kimix::string_view line, const pt_mask& mask,
                       kimix::vector<int32_t>& depths) {
    depths.clear();
    depths.reserve(line.size() + 1);
    int32_t depth = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        depths.push_back(depth);
        if (pt_code(mask, i)) {
            if (depth_open(line[i])) {
                depth += 1;
            } else if (depth_close(line[i])) {
                depth -= 1;
            }
        }
    }
    depths.push_back(depth);
}

// _brace_open_depths: number of unclosed ``{`` before each character.
void pt_brace_open_depths(kimix::string_view line, const pt_mask& mask,
                          kimix::vector<int32_t>& depths) {
    depths.assign(line.size(), 0);
    int32_t bc = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        depths[i] = bc;
        if (pt_code(mask, i)) {
            if (line[i] == '{') {
                bc += 1;
            } else if (line[i] == '}') {
                bc -= 1;
            }
        }
    }
}

// -- pre-processing: backtick / operator line continuations ------------------

// _join_backtick_continuations
kimix::string pt_join_backtick_continuations(kimix::string_view code) {
    if (code.find('`') == pt_npos) {
        return kimix::string(code);
    }
    const pt_mask mask = pt_build_region_mask(code, true);
    const size_t n = code.size();
    kimix::string result;
    result.reserve(n);
    size_t i = 0;
    size_t previous = 0;
    while (true) {
        size_t idx = code.find('`', i);
        while (idx != pt_npos && !pt_code(mask, idx)) {
            idx = code.find('`', idx + 1);
        }
        if (idx == pt_npos) {
            break;
        }
        i = idx;
        size_t j = i + 1;
        while (j < n && (code[j] == ' ' || code[j] == '\t' || code[j] == '\r')) {
            ++j;
        }
        if (j < n && code[j] == '\n') {
            j += 1;
            while (j < n &&
                   (code[j] == ' ' || code[j] == '\t' || code[j] == '\r')) {
                ++j;
            }
            result.append(code.substr(previous, i - previous));
            result.push_back(' ');
            previous = j;
            i = j;
            continue;
        }
        if (j == i + 1 && j < n && code[j] == '`') {
            // Escaped backtick: a literal character, not a continuation.
            i = j + 1;
            continue;
        }
        i += 1;
    }
    result.append(code.substr(previous));
    return result;
}

// _join_operator_continuations: join a physical line ending with a binary
// operator (``&&`` / ``||`` / ``??`` / ``??=``) onto its operand.
kimix::string pt_join_operator_continuations(kimix::string_view code) {
    const pt_mask mask = pt_build_region_mask(code, true);
    const size_t n = code.size();
    kimix::string result;
    result.reserve(n);
    size_t pos = 0;
    while (pos < n) {
        const size_t nl = code.find('\n', pos);
        if (nl == pt_npos) {
            result.append(code.substr(pos));
            break;
        }
        const kimix::string_view line = code.substr(pos, nl - pos);
        size_t slen = line.size();
        while (slen > 0 && (line[slen - 1] == ' ' || line[slen - 1] == '\t' ||
                            line[slen - 1] == '\r')) {
            --slen;
        }
        bool join = false;
        if (slen > 0) {
            const kimix::string_view stripped = line.substr(0, slen);
            const size_t tail_idx = nl - slen;
            if (pt_ends_with(stripped, "??=")) {
                join = pt_code(mask, tail_idx + slen - 3);
            } else if (pt_ends_with(stripped, "&&") ||
                       pt_ends_with(stripped, "||") ||
                       pt_ends_with(stripped, "??")) {
                join = pt_code(mask, tail_idx + slen - 2);
            }
        }
        if (!join) {
            result.append(code.substr(pos, nl + 1 - pos));
            pos = nl + 1;
            continue;
        }
        // Skip blank / comment lines to find the continuation operand.
        size_t j = nl + 1;
        while (j < n) {
            size_t nxt_nl = code.find('\n', j);
            if (nxt_nl == pt_npos) {
                nxt_nl = n;
            }
            // nxt = code[j:nxt_nl].strip(" \t\r")
            const kimix::string_view raw = code.substr(j, nxt_nl - j);
            size_t nb = 0;
            while (nb < raw.size() && (raw[nb] == ' ' || raw[nb] == '\t' ||
                                       raw[nb] == '\r')) {
                ++nb;
            }
            size_t ne = raw.size();
            while (ne > nb && (raw[ne - 1] == ' ' || raw[ne - 1] == '\t' ||
                               raw[ne - 1] == '\r')) {
                --ne;
            }
            const kimix::string_view nxt = raw.substr(nb, ne - nb);
            if (nxt.empty() || nxt[0] == '#') {
                j = (nxt_nl < n) ? nxt_nl + 1 : n;
                continue;
            }
            break;
        }
        if (j >= n) {
            result.append(code.substr(pos, nl + 1 - pos));
            pos = nl + 1;
            continue;
        }
        result.append(line.substr(0, slen));
        result.push_back(' ');
        pos = j;
    }
    return result;
}

// _join_continuation_lines
kimix::string join_continuation_lines(kimix::string_view code) {
    bool has_op_cont = code.find("&&") != pt_npos || code.find("||") != pt_npos ||
                       code.find("??") != pt_npos;
    kimix::string current(code);
    if (current.find('`') == pt_npos && !has_op_cont) {
        return current;
    }
    if (current.find('`') != pt_npos) {
        current = pt_join_backtick_continuations(current);
    }
    if (has_op_cont) {
        current = pt_join_operator_continuations(current);
    }
    return current;
}

// -- expression helpers -----------------------------------------------------

// _match_assignment: _ASSIGN_RE = (.*?)(\$\w+(?::\w+)?(?:\.\w+)*)\s*=\s*$ over
// rstrip(before).
bool match_assignment(kimix::string_view before, kimix::string_view& group1,
                      kimix::string_view& group2) {
    size_t end = before.size();
    while (end > 0 && py_space(before[end - 1])) {
        --end;
    }
    const kimix::string_view t = before.substr(0, end);
    const size_t n = t.size();
    auto word_char = [](char c) noexcept { return ascii_alnum(c) || c == '_'; };
    for (size_t p = 0; p + 2 <= n; ++p) {
        if (t[p] != '$' || !word_char(t[p + 1])) {
            continue;
        }
        size_t i = p + 1;
        while (i < n && word_char(t[i])) {
            ++i;
        }
        if (i < n && t[i] == ':' && i + 1 < n && word_char(t[i + 1])) {
            i += 2;
            while (i < n && word_char(t[i])) {
                ++i;
            }
        }
        while (i < n && t[i] == '.' && i + 1 < n && word_char(t[i + 1])) {
            i += 2;
            while (i < n && word_char(t[i])) {
                ++i;
            }
        }
        const size_t var_end = i;
        while (i < n && py_space(t[i])) {
            ++i;
        }
        if (i >= n || t[i] != '=') {
            continue;
        }
        i += 1;
        while (i < n && py_space(t[i])) {
            ++i;
        }
        if (i != n) {
            continue;
        }
        group1 = t.substr(0, p);
        group2 = t.substr(p, var_end - p);
        return true;
    }
    return false;
}

// _build_replacement: preserve an assignment prefix, if any.
kimix::string build_replacement(kimix::string_view prefix,
                                kimix::string_view inner) {
    kimix::string_view g1, g2;
    kimix::string out;
    if (match_assignment(prefix, g1, g2)) {
        out.append(g1.data(), g1.size());
        out.append(g2.data(), g2.size());
        out.append(" = ");
        out.append(inner.data(), inner.size());
        return out;
    }
    out.append(prefix.data(), prefix.size());
    out.append(inner.data(), inner.size());
    return out;
}

// _emit_replacement: guard keyword glue (``return$(...)`` is a command name).
kimix::string emit_replacement(kimix::string_view prefix,
                               kimix::string_view inner) {
    if (!prefix.empty()) {
        const char last = prefix[prefix.size() - 1];
        kimix::string_view g1, g2;
        if ((ascii_alnum(last) || last == '_') &&
            !match_assignment(prefix, g1, g2)) {
            kimix::string out;
            out.append(prefix.data(), prefix.size());
            out.push_back(' ');
            out.append(inner.data(), inner.size());
            return out;
        }
    }
    return build_replacement(prefix, inner);
}

bool command_prefix_match(kimix::string_view expr, size_t& cmd_end) {
    // ^[A-Za-z][A-Za-z0-9_-]*\s+
    if (expr.empty() || !ascii_alpha(expr[0])) {
        return false;
    }
    size_t i = 1;
    while (i < expr.size() &&
           (ascii_alnum(expr[i]) || expr[i] == '_' || expr[i] == '-')) {
        ++i;
    }
    if (i >= expr.size() || !py_space(expr[i])) {
        return false;
    }
    cmd_end = i;
    while (cmd_end < expr.size() && py_space(expr[cmd_end])) {
        ++cmd_end;
    }
    return true;
}

// _strip_command_prefix / _strip_keyword_operand
void strip_command_prefix(kimix::string_view& expr, size_t& start,
                          bool check_keywords) {
    size_t cmd_end = 0;
    if (!command_prefix_match(expr, cmd_end)) {
        return;
    }
    size_t first_ws = 0;
    while (first_ws < cmd_end && !py_space(expr[first_ws])) {
        ++first_ws;
    }
    kimix::string cmd_lower;
    cmd_lower.reserve(first_ws);
    for (size_t k = 0; k < first_ws; ++k) {
        char c = expr[k];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        cmd_lower.push_back(c);
    }
    if (!check_keywords || !is_ps_keyword(cmd_lower)) {
        const kimix::string_view expr_part = expr.substr(cmd_end);
        if (!expr_part.empty()) {
            const char c0 = expr_part[0];
            if (c0 == '$' || c0 == '(' || c0 == '[' || c0 == '"' ||
                c0 == '\'' || c0 == '@' || (c0 >= '0' && c0 <= '9')) {
                start += cmd_end;
                expr = expr_part;
            }
        }
    }
}

bool pt_keyword_at(kimix::string_view s, kimix::string_view keyword,
                   size_t& keyword_end) noexcept {
    if (s.size() < keyword.size()) {
        return false;
    }
    for (size_t i = 0; i < keyword.size(); ++i) {
        if (pt_lower(s[i]) != keyword[i]) {
            return false;
        }
    }
    if (s.size() <= keyword.size() || !py_space(s[keyword.size()])) {
        return false;
    }
    keyword_end = keyword.size();
    return true;
}

void strip_keyword_operand(kimix::string_view& expr, size_t& start) {
    size_t kw_end = 0;
    if (!pt_keyword_at(expr, kimix::string_view("return", 6), kw_end) &&
        !pt_keyword_at(expr, kimix::string_view("throw", 5), kw_end)) {
        return;
    }
    size_t i = kw_end;
    while (i < expr.size() && py_space(expr[i])) {
        ++i;
    }
    start += i;
    expr = expr.substr(i);
}

// _lift_region_placeholders: __kimixrgn<line>x<run>__ words spliced onto lines
// whose multi-line region opens/closes mid-line are opaque tokens.
bool pt_placeholder_at(kimix::string_view s, size_t i, size_t& end) noexcept {
    static constexpr char kPrefix[] = "__kimixrgn";
    constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (s.size() < i + kPrefixLen || s.substr(i, kPrefixLen) != kPrefix) {
        return false;
    }
    size_t k = i + kPrefixLen;
    const size_t digits0 = k;
    while (k < s.size() && ascii_digit(s[k])) {
        ++k;
    }
    if (k == digits0 || k >= s.size() || s[k] != 'x') {
        return false;
    }
    ++k;
    const size_t digits1 = k;
    while (k < s.size() && ascii_digit(s[k])) {
        ++k;
    }
    if (k == digits1) {
        return false;
    }
    if (k + 2 > s.size() || s.substr(k, 2) != "__") {
        return false;
    }
    end = k + 2;
    return true;
}

void lift_region_placeholders(kimix::string& expr,
                              kimix::vector<kimix::string>& found) {
    kimix::vector<kimix::string> hits;
    kimix::string cleaned;
    size_t i = 0;
    size_t previous = 0;
    while (i < expr.size()) {
        size_t e = 0;
        if (pt_placeholder_at(expr, i, e)) {
            hits.push_back(strip_py(expr.substr(i, e - i)));
            cleaned.append(expr.substr(previous, i - previous));
            cleaned.push_back(' ');
            previous = e;
            i = e;
            continue;
        }
        ++i;
    }
    if (hits.empty()) {
        return;
    }
    cleaned.append(expr.substr(previous));
    kimix::string stripped = strip_py(cleaned);
    if (stripped.empty()) {
        return;
    }
    expr = stripped;
    for (kimix::string& hit : hits) {
        found.push_back(hit);
    }
}

kimix::string keeps_prefix(const kimix::vector<kimix::string>& keeps) {
    if (keeps.empty()) {
        return kimix::string();
    }
    kimix::string out;
    out.push_back(' ');
    for (size_t i = 0; i < keeps.size(); ++i) {
        if (i > 0) {
            out.push_back(' ');
        }
        out.append(keeps[i]);
    }
    out.push_back(' ');
    return out;
}

// re.match(r"(?i)^(?:return|throw)[ \t]", text.lstrip())
bool return_throw_operand_prefix(kimix::string_view s) noexcept {
    size_t i = 0;
    while (i < s.size() && py_space(s[i])) {
        ++i;
    }
    const kimix::string_view rest = s.substr(i);
    static constexpr char kKeywords[2][7] = {"return", "throw"};
    for (const auto& kw : kKeywords) {
        const kimix::string_view word(kw);
        if (rest.size() <= word.size()) {
            continue;
        }
        bool same = true;
        for (size_t k = 0; k < word.size(); ++k) {
            if (pt_lower(rest[k]) != word[k]) {
                same = false;
                break;
            }
        }
        if (!same) {
            continue;
        }
        const char after = rest[word.size()];
        if (after == ' ' || after == '\t') {
            return true;
        }
    }
    return false;
}

// -- shared context guards --------------------------------------------------

bool after_dollar_question(kimix::string_view line, size_t op_idx) noexcept {
    return op_idx > 0 && line[op_idx - 1] == '$' &&
           !(op_idx > 1 && line[op_idx - 2] == '$');
}

bool is_scope_colon(kimix::string_view line, size_t i) noexcept {
    size_t j = i;
    while (j > 0 && (ascii_alnum(line[j - 1]) || line[j - 1] == '_')) {
        --j;
    }
    return j > 0 && line[j - 1] == '$';
}

// _is_drive_colon: a single letter before the colon followed by \ / or an
// expression terminator is a drive reference, not a ternary separator.
bool is_drive_colon(kimix::string_view line, size_t i) noexcept {
    if (i < 1 || !ascii_alpha(line[i - 1])) {
        return false;
    }
    if (i >= 2) {
        const char p = line[i - 2];
        if (ascii_alnum(p) || p == '_' || p == '$') {
            return false;
        }
    }
    if (i + 1 >= line.size()) {
        return true;
    }
    const char c = line[i + 1];
    return c == '\\' || c == '/' || c == ' ' || c == '\t' || c == '\'' ||
           c == ')' || c == '}' || c == ',' || c == ';' || c == '|' || c == '&';
}

bool is_null_conditional_qmark(kimix::string_view line, size_t i) noexcept {
    return i + 1 < line.size() && (line[i + 1] == '.' || line[i + 1] == '[');
}

bool is_double_colon(kimix::string_view line, size_t i) noexcept {
    return (i + 1 < line.size() && line[i + 1] == ':') ||
           (i > 0 && line[i - 1] == ':');
}

// _VALUE_EXPR_TAIL_RE: does *expr* end like a value expression?
bool pt_postfix_tail(kimix::string_view s, size_t p) {
    // (?:\.\w+|\[[^\]]*\]|--|\+\+)*$  -- backtracking search over postfix units.
    if (p == s.size()) {
        return true;
    }
    if (s[p] == '.' && p + 1 < s.size() &&
        (ascii_alnum(s[p + 1]) || s[p + 1] == '_')) {
        size_t k = p + 1;
        while (k < s.size() && (ascii_alnum(s[k]) || s[k] == '_')) {
            ++k;
        }
        if (pt_postfix_tail(s, k)) {
            return true;
        }
    }
    if (s[p] == '[') {
        const size_t close = s.find(']', p + 1);
        if (close != pt_npos && s.substr(p + 1, close - p - 1).find(']') ==
                                    pt_npos) {
            if (pt_postfix_tail(s, close + 1)) {
                return true;
            }
        }
    }
    if (p + 1 < s.size() && ((s[p] == '-' && s[p + 1] == '-') ||
                             (s[p] == '+' && s[p + 1] == '+'))) {
        if (pt_postfix_tail(s, p + 2)) {
            return true;
        }
    }
    return false;
}

bool pt_value_head_at(kimix::string_view s, size_t p) {
    // \$[A-Za-z_][\w:]* | \$\? | \$\$ | \$\^ | \$\{[^}]+\} | @[A-Za-z_][\w:]*
    // | [)\]}] | '[^']*' | "[^"]*" | [0-9][0-9.]* | \[[\w.]+\]::\w+
    if (p >= s.size()) {
        return false;
    }
    const char c = s[p];
    if (c == '$') {
        if (p + 1 >= s.size()) {
            return false;
        }
        const char c1 = s[p + 1];
        if (c1 == '?' || c1 == '$' || c1 == '^') {
            return pt_postfix_tail(s, p + 2);
        }
        if (c1 == '{') {
            const size_t close = s.find('}', p + 2);
            if (close == pt_npos || close == p + 2) {
                return false;
            }
            return pt_postfix_tail(s, close + 1);
        }
        if (ascii_alpha(c1) || c1 == '_') {
            size_t k = p + 2;
            while (k < s.size() && (ascii_alnum(s[k]) || s[k] == '_' ||
                                    s[k] == ':')) {
                ++k;
            }
            return pt_postfix_tail(s, k);
        }
        return false;
    }
    if (c == '@') {
        if (p + 1 >= s.size() ||
            !(ascii_alpha(s[p + 1]) || s[p + 1] == '_')) {
            return false;
        }
        size_t k = p + 2;
        while (k < s.size() && (ascii_alnum(s[k]) || s[k] == '_' ||
                                s[k] == ':')) {
            ++k;
        }
        return pt_postfix_tail(s, k);
    }
    if (c == ')' || c == ']' || c == '}') {
        return pt_postfix_tail(s, p + 1);
    }
    if (c == '\'') {
        size_t k = p + 1;
        while (k < s.size() && s[k] != '\'') {
            ++k;
        }
        if (k >= s.size()) {
            return false;
        }
        return pt_postfix_tail(s, k + 1);
    }
    if (c == '"') {
        size_t k = p + 1;
        while (k < s.size() && s[k] != '"') {
            ++k;
        }
        if (k >= s.size()) {
            return false;
        }
        return pt_postfix_tail(s, k + 1);
    }
    if (ascii_digit(c)) {
        size_t k = p + 1;
        while (k < s.size() && (ascii_digit(s[k]) || s[k] == '.')) {
            ++k;
        }
        // [0-9][0-9.]* is greedy; the regex engine backtracks so a trailing
        // postfix chain (``1.5.foo``) can still match.
        for (size_t cut = k; cut > p; --cut) {
            if (pt_postfix_tail(s, cut)) {
                return true;
            }
        }
        return false;
    }
    if (c == '[') {
        // \[[\w.]+\]::\w+
        size_t k = p + 1;
        while (k < s.size() && (ascii_alnum(s[k]) || s[k] == '_' ||
                                s[k] == '.')) {
            ++k;
        }
        if (k == p + 1 || k + 1 >= s.size() || s[k] != ']' ||
            s[k + 1] != ':') {
            return false;
        }
        k += 2;
        if (k >= s.size() || s[k] != ':') {
            return false;
        }
        ++k;
        const size_t word0 = k;
        while (k < s.size() && (ascii_alnum(s[k]) || s[k] == '_')) {
            ++k;
        }
        if (k == word0) {
            return false;
        }
        return pt_postfix_tail(s, k);
    }
    return false;
}

bool looks_like_value_expr(kimix::string_view expr) {
    size_t end = expr.size();
    while (end > 0 && py_space(expr[end - 1])) {
        --end;
    }
    const kimix::string_view s = expr.substr(0, end);
    if (s.empty()) {
        return false;
    }
    const char last = s[s.size() - 1];
    if (last == ')' || last == ']' || last == '}') {
        return true;
    }
    for (size_t p = 0; p < s.size(); ++p) {
        if (pt_value_head_at(s, p)) {
            return true;
        }
    }
    return false;
}

// _find_expr_start
size_t find_expr_start(kimix::string_view line, size_t end, const pt_mask& mask,
                       kimix::string_view extra_stop) {
    int64_t depth = 0;
    for (int64_t i = static_cast<int64_t>(end) - 1; i >= 0; --i) {
        if (!pt_code(mask, static_cast<size_t>(i))) {
            continue;
        }
        const char c = line[static_cast<size_t>(i)];
        if (depth_close(c)) {
            depth += 1;
        } else if (depth_open(c)) {
            depth -= 1;
            if (depth < 0) {
                return static_cast<size_t>(i) + 1;
            }
        } else if (depth == 0 &&
                   (expr_stop(c) || extra_stop.find(c) != pt_npos)) {
            if (c == '?') {
                if (is_null_conditional_qmark(line, static_cast<size_t>(i)) ||
                    after_dollar_question(line, static_cast<size_t>(i))) {
                    continue;
                }
            } else if (c == ':') {
                if (is_double_colon(line, static_cast<size_t>(i)) ||
                    is_scope_colon(line, static_cast<size_t>(i))) {
                    continue;
                }
            }
            return static_cast<size_t>(i) + 1;
        }
    }
    return 0;
}

// _find_expr_end
size_t find_expr_end(kimix::string_view line, size_t start, const pt_mask& mask,
                     kimix::string_view extra_stop) {
    int64_t depth = 0;
    bool prev_mask = (start == 0) ? true : pt_code(mask, start - 1);
    for (size_t i = start; i < line.size(); ++i) {
        const char c = line[i];
        const bool cur_mask = pt_code(mask, i);
        if (!cur_mask) {
            if (prev_mask && c == '#') {
                return i;
            }
            prev_mask = cur_mask;
            continue;
        }
        prev_mask = cur_mask;
        if (depth_open(c)) {
            depth += 1;
        } else if (depth_close(c)) {
            depth -= 1;
            if (depth < 0) {
                return i;
            }
        } else if (depth == 0 &&
                   (expr_stop(c) || extra_stop.find(c) != pt_npos)) {
            if (c == '?') {
                if (is_null_conditional_qmark(line, i)) {
                    continue;
                }
                if (i > 0 && line[i - 1] == '$') {
                    continue;
                }
            } else if (c == ':') {
                if (is_double_colon(line, i) || is_scope_colon(line, i) ||
                    is_drive_colon(line, i)) {
                    continue;
                }
            }
            return i;
        }
    }
    return line.size();
}

void expr_left(kimix::string_view line, size_t pos, const pt_mask& mask,
               kimix::string_view extra_stop, size_t& out_start,
               size_t& out_end) {
    size_t end = pos;
    while (end > 0 && line[end - 1] == ' ') {
        --end;
    }
    out_start = find_expr_start(line, end, mask, extra_stop);
    out_end = end;
}

void expr_right(kimix::string_view line, size_t pos, const pt_mask& mask,
                kimix::string_view extra_stop, size_t& out_start,
                size_t& out_end) {
    size_t start = pos;
    while (start < line.size() && line[start] == ' ') {
        ++start;
    }
    out_start = start;
    out_end = find_expr_end(line, start, mask, extra_stop);
}

// _swallow_comma_operands: comma binds tighter than ``??`` in PowerShell 7.
size_t swallow_comma_operands(kimix::string_view line, size_t end,
                              const pt_mask& mask) {
    kimix::vector<int32_t> depth_arr;
    pt_compute_depths(line, mask, depth_arr);
    const size_t n = line.size();
    size_t j = end;
    while (true) {
        int64_t p = static_cast<int64_t>(j) - 1;
        while (p >= 0 && (line[static_cast<size_t>(p)] == ' ' ||
                          line[static_cast<size_t>(p)] == '\t')) {
            --p;
        }
        size_t k = j;
        while (k < n && (line[k] == ' ' || line[k] == '\t')) {
            ++k;
        }
        if (p < 0 || k >= n || line[k] != ',' || !pt_code(mask, k) ||
            depth_arr[k] != depth_arr[static_cast<size_t>(p)]) {
            return j;
        }
        const int32_t d = depth_arr[k];
        if (d > 0) {
            int64_t o = static_cast<int64_t>(k) - 1;
            int64_t nested = 0;
            char opener = 0;
            int64_t opener_pos = -1;
            while (o >= 0) {
                if (pt_code(mask, static_cast<size_t>(o))) {
                    const char ch = line[static_cast<size_t>(o)];
                    if (ch == ')' || ch == ']' || ch == '}') {
                        nested += 1;
                    } else if (ch == '(' || ch == '[' || ch == '{') {
                        if (nested) {
                            nested -= 1;
                        } else if (depth_arr[static_cast<size_t>(o)] == d - 1) {
                            opener = ch;
                            opener_pos = o;
                            break;
                        }
                    }
                }
                --o;
            }
            if (opener != 0 && opener_pos > 0) {
                const char before = line[static_cast<size_t>(opener_pos) - 1];
                if (ascii_alnum(before) || before == '_' || before == ')' ||
                    before == '}' || before == ']') {
                    return j;
                }
            }
        }
        size_t nstart = k + 1;
        while (nstart < n && (line[nstart] == ' ' || line[nstart] == '\t')) {
            ++nstart;
        }
        if (nstart >= n) {
            return j;
        }
        const size_t nend = find_expr_end(line, nstart, mask, kimix::string_view());
        if (nend <= nstart) {
            return j;
        }
        j = nend;
    }
}

// _find_next_op: next occurrence of *op* outside regions (forward only).
size_t find_next_op(kimix::string_view line, kimix::string_view op,
                    const pt_mask& mask, bool skip_dollar_q, size_t start,
                    bool plain_code_only) {
    size_t idx = line.find(op, start);
    while (idx != pt_npos) {
        if (pt_code(mask, idx) && (!plain_code_only || mask[idx] == 1) &&
            (!skip_dollar_q || !after_dollar_question(line, idx))) {
            return idx;
        }
        idx = line.find(op, idx + 1);
    }
    return pt_npos;
}

// -- warning formatting -----------------------------------------------------

kimix::string format_transform_warning(uint32_t line, kimix::string_view body) {
    kimix::string out = "Line ";
    out += kimix::format("{}", line);
    out += ": ";
    out.append(body.data(), body.size());
    return out;
}

// -- transform: null-coalescing assignment  (??=) ---------------------------

void transform_nca_line(kimix::string& line,
                        kimix::vector<kimix::string>* warnings);

void transform_nc_line(kimix::string& line,
                       kimix::vector<kimix::string>* warnings);

void transform_ternary_line(kimix::string& line,
                            kimix::vector<kimix::string>* warnings);

void transform_chain_line(kimix::string& line,
                          kimix::vector<kimix::string>* warnings);

void transform_nca_line(kimix::string& line,
                        kimix::vector<kimix::string>* warnings) {
    if (line.find("??=") == pt_npos) {
        return;
    }
    size_t search = 0;
    while (true) {
        const pt_mask mask = pt_line_mask(line);
        const size_t idx =
            find_next_op(line, kimix::string_view("??=", 3), mask, true, search,
                         false);
        if (idx == pt_npos) {
            break;
        }
        size_t left_start = 0, left_end = 0;
        expr_left(line, idx, mask, kimix::string_view(), left_start, left_end);
        kimix::string var = strip_py(line.substr(left_start, left_end - left_start));
        if (var.empty()) {
            search = idx + 1;
            continue;
        }
        size_t val_start = 0, val_end = 0;
        expr_right(line, idx + 3, mask, kimix::string_view(), val_start, val_end);
        kimix::string value =
            strip_py(line.substr(val_start, val_end - val_start));
        // A chained coalesce in the value is right-associative in PS 7.
        while (pt_ends_with(value, "??") && val_end < line.size() &&
               line[val_end] == '=') {
            val_end = find_expr_end(line, val_end + 1, mask, kimix::string_view());
            value = strip_py(line.substr(val_start, val_end - val_start));
        }
        kimix::string yield_suffix;
        kimix::string new_inner;
        if (value.find("??") != pt_npos) {
            const pt_mask vmask = pt_line_mask(value);
            const size_t inner_idx = find_next_op(
                value, kimix::string_view("??=", 3), vmask, true, 0, false);
            if (inner_idx != pt_npos) {
                size_t is_ = 0, ie_ = 0;
                expr_left(value, inner_idx, vmask, kimix::string_view(), is_,
                          ie_);
                const kimix::string yield_var = strip_py(value.substr(is_, ie_ - is_));
                transform_nca_line(value, warnings);
                yield_suffix = "; ";
                yield_suffix += yield_var;
            }
            transform_nc_line(value, warnings);
            new_inner = "if ($null -eq ";
            new_inner += var;
            new_inner += ") { ";
            new_inner += var;
            new_inner += " = $(";
            new_inner += value;
            new_inner += yield_suffix;
            new_inner += ") }";
        } else {
            new_inner = "if ($null -eq ";
            new_inner += var;
            new_inner += ") { ";
            new_inner += var;
            new_inner += " = ";
            new_inner += value;
            new_inner += " }";
        }
        if (warnings != nullptr) {
            kimix::string body = "null-coalescing assignment `";
            body += var;
            body += " ??= ...` rewritten to `";
            body += new_inner;
            body += "`";
            warnings->push_back(body);
        }
        const kimix::string prefix = rstrip_py(line.substr(0, left_start));
        kimix::string new_line = emit_replacement(prefix, new_inner);
        new_line.append(line.data() + val_end, line.size() - val_end);
        line = std::move(new_line);
        search = left_start;
    }
}

// -- transform: null-coalescing  (??) ---------------------------------------

void transform_nc_line(kimix::string& line,
                       kimix::vector<kimix::string>* warnings) {
    if (line.find("??") == pt_npos) {
        return;
    }
    size_t search = 0;
    while (true) {
        const pt_mask mask = pt_line_mask(line);
        const size_t idx =
            find_next_op(line, kimix::string_view("??", 2), mask, true, search,
                         false);
        if (idx == pt_npos) {
            break;
        }
        // left operand (operand_extra_stop = "?:")
        size_t left_start = 0, left_end = 0;
        expr_left(line, idx, mask, kimix::string_view("?:", 2), left_start,
                  left_end);
        kimix::string left =
            strip_py(line.substr(left_start, left_end - left_start));
        {
            kimix::string_view lv(left);
            strip_command_prefix(lv, left_start, true);
            strip_keyword_operand(lv, left_start);
            left.assign(lv.data(), lv.size());
        }
        // right operand, swallowing same-depth comma continuations
        size_t right_start = 0, right_end = 0;
        expr_right(line, idx + 2, mask, kimix::string_view("?:", 2), right_start,
                   right_end);
        right_end = swallow_comma_operands(line, right_end, mask);
        kimix::string right =
            strip_py(line.substr(right_start, right_end - right_start));
        // Region placeholders are opaque tokens.
        kimix::vector<kimix::string> keeps;
        lift_region_placeholders(left, keeps);
        lift_region_placeholders(right, keeps);
        // Parenthesized ternaries inside an operand would be copied verbatim
        // into the generated braces (where the ternary brace guard cannot
        // reach them) — pre-transform them here.
        if (left.find('?') != pt_npos) {
            transform_ternary_line(left, warnings);
        }
        if (right.find('?') != pt_npos) {
            transform_ternary_line(right, warnings);
        }
        if (left.empty() || right.empty()) {
            search = idx + 1;
            continue;
        }
        if (!looks_like_value_expr(left)) {
            search = idx + 1;
            continue;
        }
        kimix::string inner = "if ($null -ne ";
        inner += left;
        inner += ") { ";
        inner += left;
        inner += " } else { ";
        inner += right;
        inner += " }";
        // A bare ``if`` statement is only legal at statement position; lift it
        // into a value everywhere (identical semantics, valid in expression
        // position too).
        inner = "$(" + inner + ")";
        if (warnings != nullptr) {
            kimix::string body = "?? operator `";
            body += left;
            body += " ?? ";
            body += right;
            body += "` rewritten to `";
            body += inner;
            body += "`";
            warnings->push_back(body);
        }
        kimix::string prefix = line.substr(0, left_start);
        prefix += keeps_prefix(keeps);
        kimix::string new_line = emit_replacement(prefix, inner);
        new_line.append(line.data() + right_end, line.size() - right_end);
        line = std::move(new_line);
        search = left_start;
    }
}

// -- transform: ternary  (? :) ----------------------------------------------

size_t find_matching_colon(kimix::string_view line, size_t start,
                           const pt_mask& mask,
                           const kimix::vector<int32_t>& depth_arr,
                           int32_t ref_depth) {
    for (size_t i = start; i < line.size(); ++i) {
        if (!pt_code(mask, i)) {
            continue;
        }
        if (depth_arr[i] < ref_depth) {
            break;
        }
        if (line[i] != ':' || depth_arr[i] != ref_depth) {
            continue;
        }
        if (is_double_colon(line, i) || is_scope_colon(line, i) ||
            is_drive_colon(line, i)) {
            continue;
        }
        return i;
    }
    return pt_npos;
}

void transform_ternary_line(kimix::string& line,
                            kimix::vector<kimix::string>* warnings) {
    if (line.find('?') == pt_npos) {
        return;
    }
    kimix::vector<uint8_t> rejected;
    while (true) {
        const pt_mask mask = pt_line_mask(line);
        kimix::vector<int32_t> depth_arr;
        kimix::vector<int32_t> brace_open;
        pt_compute_depths(line, mask, depth_arr);
        pt_brace_open_depths(line, mask, brace_open);
        // Rewrite the RIGHTMOST eligible ternary first (inner-to-outer keeps
        // nested ``? :`` out of the emitted arm blocks).
        int64_t pos = static_cast<int64_t>(line.size()) - 1;
        int64_t colon_pos = -1;
        while (pos >= 0) {
            const size_t upos = static_cast<size_t>(pos);
            if (upos < rejected.size() && rejected[upos] != 0) {
                --pos;
                continue;
            }
            if (line[upos] == '?' && pt_code(mask, upos) &&
                !after_dollar_question(line, upos)) {
                // ``?`` in command position (the Where-Object alias) is not a
                // ternary.
                int64_t k = pos - 1;
                while (k >= 0 && line[static_cast<size_t>(k)] == ' ') {
                    --k;
                }
                if (k < 0) {
                    --pos;
                    continue;
                }
                const char kc = line[static_cast<size_t>(k)];
                if (kc == '|' || kc == '&' || kc == ';' || kc == ',') {
                    --pos;
                    continue;
                }
                // Inside an open ``{`` the colon may belong to hashtable /
                // switch / script-block syntax: skip unless the ternary is
                // nested in ``(...)`` or sits in a return/throw operand.
                if (brace_open[upos] > 0 &&
                    depth_arr[upos] - brace_open[upos] == 0) {
                    size_t cs = 0, ce = 0;
                    expr_left(line, upos, mask, kimix::string_view(), cs, ce);
                    if (!return_throw_operand_prefix(line.substr(cs, ce - cs))) {
                        --pos;
                        continue;
                    }
                }
                const size_t found = find_matching_colon(
                    line, upos + 1, mask, depth_arr, depth_arr[upos]);
                if (found == pt_npos) {
                    --pos;
                    continue;
                }
                colon_pos = static_cast<int64_t>(found);
                break;
            }
            --pos;
        }
        if (pos < 0) {
            break;
        }
        const size_t upos = static_cast<size_t>(pos);
        const size_t ucolon = static_cast<size_t>(colon_pos);
        size_t cond_start = 0, cond_end = 0;
        expr_left(line, upos, mask, kimix::string_view(":", 1), cond_start,
                  cond_end);
        kimix::string condition =
            strip_py(line.substr(cond_start, cond_end - cond_start));
        kimix::string true_expr =
            strip_py(line.substr(upos + 1, ucolon - upos - 1));
        size_t false_start = 0, false_end = 0;
        expr_right(line, ucolon + 1, mask, kimix::string_view(), false_start,
                   false_end);
        // The comma operator binds tighter than the ternary: the false branch
        // swallows same-depth comma-separated values.
        const int32_t ref_depth = depth_arr[ucolon];
        size_t j = false_end;
        while (j < line.size()) {
            size_t k = j;
            while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) {
                ++k;
            }
            if (!(k < line.size() && line[k] == ',' && pt_code(mask, k) &&
                  depth_arr[k] == ref_depth)) {
                break;
            }
            size_t nstart = k + 1;
            while (nstart < line.size() &&
                   (line[nstart] == ' ' || line[nstart] == '\t')) {
                ++nstart;
            }
            if (nstart >= line.size()) {
                break;
            }
            const size_t nend =
                find_expr_end(line, nstart, mask, kimix::string_view());
            if (nend <= nstart) {
                break;
            }
            j = nend;
        }
        false_end = j;
        kimix::string false_expr =
            strip_py(line.substr(false_start, false_end - false_start));
        kimix::string suffix(line.substr(false_end));
        {
            kimix::string_view g1, g2;
            if (!match_assignment(line.substr(0, cond_start), g1, g2)) {
                kimix::string_view cv(condition);
                strip_command_prefix(cv, cond_start, false);
                strip_keyword_operand(cv, cond_start);
                condition.assign(cv.data(), cv.size());
            } else {
                kimix::string_view cv(condition);
                strip_keyword_operand(cv, cond_start);
                condition.assign(cv.data(), cv.size());
            }
        }
        kimix::vector<kimix::string> keeps;
        lift_region_placeholders(condition, keeps);
        lift_region_placeholders(true_expr, keeps);
        lift_region_placeholders(false_expr, keeps);
        if (condition.empty() || true_expr.empty() || false_expr.empty()) {
            if (rejected.size() <= upos) {
                rejected.resize(upos + 1, 0);
            }
            rejected[upos] = 1;
            continue;
        }
        kimix::string inner = "if (";
        inner += condition;
        inner += ") { ";
        inner += true_expr;
        inner += " } else { ";
        inner += false_expr;
        inner += " }";
        {
            size_t s0 = 0;
            while (s0 < suffix.size() && py_space(suffix[s0])) {
                ++s0;
            }
            const char first = (s0 < suffix.size()) ? suffix[s0] : 0;
            if (depth_arr[upos] > 0 || first == '|' || first == '&' ||
                first == '>') {
                inner = "$(" + inner + ")";
            }
        }
        if (warnings != nullptr) {
            kimix::string body = "ternary operator `";
            body += condition;
            body += " ? ";
            body += true_expr;
            body += " : ";
            body += false_expr;
            body += "` rewritten to `";
            body += inner;
            body += "`";
            warnings->push_back(body);
        }
        kimix::string prefix = line.substr(0, cond_start);
        prefix += keeps_prefix(keeps);
        kimix::string new_line = emit_replacement(prefix, inner);
        new_line += suffix;
        line = std::move(new_line);
        rejected.clear();
    }
}

// -- transform: pipeline chain operators  (&& / ||) -------------------------

char enclosing_group_opener(kimix::string_view line, size_t pos,
                            const pt_mask& mask,
                            const kimix::vector<int32_t>& depths, int32_t depth) {
    if (depth <= 0) {
        return 0;
    }
    int64_t j = static_cast<int64_t>(pos) - 1;
    int64_t nested = 0;
    while (j >= 0) {
        if (pt_code(mask, static_cast<size_t>(j))) {
            const char ch = line[static_cast<size_t>(j)];
            if (ch == ')' || ch == ']' || ch == '}') {
                nested += 1;
            } else if (ch == '(' || ch == '[' || ch == '{') {
                if (nested) {
                    nested -= 1;
                } else if (depths[static_cast<size_t>(j)] == depth - 1) {
                    return ch;
                }
            }
        }
        --j;
    }
    return 0;
}

size_t find_interp_op(kimix::string_view line, kimix::string_view op,
                      const pt_mask& mask) {
    size_t idx = line.find(op);
    while (idx != pt_npos) {
        if (idx < mask.size() && mask[idx] == 2) {
            return idx;
        }
        idx = line.find(op, idx + 1);
    }
    return pt_npos;
}

void mask0_span(kimix::string_view line, const pt_mask& mask, size_t k,
                size_t& out_start, size_t& out_end) {
    size_t s = k;
    while (s > 0 && !pt_code(mask, s - 1)) {
        --s;
    }
    size_t e = k + 1;
    while (e < line.size() && !pt_code(mask, e)) {
        ++e;
    }
    out_start = s;
    out_end = e;
}

// _interp_edge_or_literal: a mask-0 character inside an interpolated region is
// either a nested string literal (transparent) or the region edge.
bool interp_is_literal(kimix::string_view line, const pt_mask& mask, size_t k) {
    size_t s = 0, e = 0;
    mask0_span(line, mask, k, s, e);
    return s < line.size() && (line[s] == '\'' || line[s] == '"');
}

// _CHAIN_ASSIGN_RE: ^(\s*(?:\$\{[^}]+\}|\$\w+(?::\w+)?(?:\.\w+|\[[^\]]*\])*)\s*
//                      (\+=|-=|\*=|/=|%=|=(?![=~])))\s*(.*)$
bool match_chain_assign(kimix::string_view s, kimix::string_view& group1,
                        kimix::string_view& group3) {
    const size_t n = s.size();
    size_t i = 0;
    while (i < n && py_space(s[i])) {
        ++i;
    }
    if (i >= n || s[i] != '$') {
        return false;
    }
    ++i;
    if (i >= n) {
        return false;
    }
    if (s[i] == '{') {
        const size_t close = s.find('}', i + 1);
        if (close == pt_npos || close == i + 1) {
            return false;
        }
        i = close + 1;
    } else {
        if (!(ascii_alnum(s[i]) || s[i] == '_')) {
            return false;
        }
        while (i < n && (ascii_alnum(s[i]) || s[i] == '_')) {
            ++i;
        }
        if (i + 1 < n && s[i] == ':' && (ascii_alnum(s[i + 1]) || s[i + 1] == '_')) {
            i += 2;
            while (i < n && (ascii_alnum(s[i]) || s[i] == '_')) {
                ++i;
            }
        }
        while (i < n) {
            if (s[i] == '.' && i + 1 < n &&
                (ascii_alnum(s[i + 1]) || s[i + 1] == '_')) {
                i += 2;
                while (i < n && (ascii_alnum(s[i]) || s[i] == '_')) {
                    ++i;
                }
                continue;
            }
            if (s[i] == '[') {
                const size_t close = s.find(']', i + 1);
                if (close == pt_npos) {
                    return false;
                }
                i = close + 1;
                continue;
            }
            break;
        }
    }
    while (i < n && py_space(s[i])) {
        ++i;
    }
    // operator: += -= *= /= %= or '=' not followed by '=' / '~'
    size_t op_end = 0;
    if (i + 2 <= n) {
        const char c0 = s[i];
        const char c1 = (i + 1 < n) ? s[i + 1] : 0;
        if (c1 == '=' && (c0 == '+' || c0 == '-' || c0 == '*' || c0 == '/' ||
                          c0 == '%')) {
            op_end = i + 2;
        } else if (c0 == '=' && c1 != '=' && c1 != '~') {
            op_end = i + 1;
        }
    } else if (i < n && s[i] == '=') {
        // '=' at the very end of the string: the negative lookahead is
        // vacuously satisfied.
        op_end = i + 1;
    }
    if (op_end == 0) {
        return false;
    }
    group1 = s.substr(0, op_end);
    size_t rest = op_end;
    while (rest < n && py_space(s[rest])) {
        ++rest;
    }
    group3 = s.substr(rest);
    return true;
}

kimix::string chain_emit(kimix::string_view prefix, kimix::string_view left_seg,
                        kimix::string_view right_seg, kimix::string_view best_op,
                        bool wrap_value, kimix::string_view suffix,
                        const kimix::string_view* assign_group1,
                        const kimix::string_view* assign_rest) {
    kimix::string head_prefix(prefix);
    if (!head_prefix.empty()) {
        const char last = head_prefix[head_prefix.size() - 1];
        if (!py_space(last)) {
            head_prefix.push_back(' ');
        }
    }
    const kimix::string_view condition =
        (best_op == "&&") ? kimix::string_view("$?", 2)
                          : kimix::string_view("-not $?", 7);
    kimix::string out;
    if (assign_group1 != nullptr) {
        // ``$x = A && B`` -> ``$x = $(A; if ($?) { B })``: the variable
        // receives the combined output of the executed segments.
        out.append(head_prefix);
        out.append(*assign_group1);
        out += " $(";
        if (!assign_rest->empty()) {
            out.append(*assign_rest);
            out += "; ";
        } else {
            out += "; ";
        }
        out += "if (";
        out.append(condition);
        out += ") { ";
        out.append(right_seg);
        out += " })";
        out.append(suffix);
        return out;
    }
    kimix::string inner;
    if (!left_seg.empty()) {
        inner.append(left_seg);
        inner += "; ";
    } else {
        inner += "; ";
    }
    inner += "if (";
    inner.append(condition);
    inner += ") { ";
    inner.append(right_seg);
    inner += " }";
    // The statement sequence is lifted into a value only where an expression
    // is required; the surrounding prefix/suffix stay outside the ``$( )``.
    if (wrap_value) {
        inner = "$(" + inner + ")";
    }
    out.append(head_prefix);
    out += inner;
    out.append(suffix);
    return out;
}

void transform_chain_line(kimix::string& line,
                          kimix::vector<kimix::string>* warnings) {
    if (line.find("&&") == pt_npos && line.find("||") == pt_npos) {
        return;
    }
    // Phase 1: chains in ordinary code (mask 1); region characters (mask 0)
    // are transparent for the boundary scans.
    while (true) {
        const pt_mask mask = pt_line_mask(line);
        const size_t and_pos =
            find_next_op(line, kimix::string_view("&&", 2), mask, false, 0, true);
        const size_t or_pos =
            find_next_op(line, kimix::string_view("||", 2), mask, false, 0, true);
        if (and_pos == pt_npos && or_pos == pt_npos) {
            break;
        }
        size_t best_pos = 0;
        kimix::string_view best_op;
        if (and_pos == pt_npos) {
            best_pos = or_pos;
            best_op = kimix::string_view("||", 2);
        } else if (or_pos == pt_npos) {
            best_pos = and_pos;
            best_op = kimix::string_view("&&", 2);
        } else if (and_pos < or_pos) {
            best_pos = and_pos;
            best_op = kimix::string_view("&&", 2);
        } else {
            best_pos = or_pos;
            best_op = kimix::string_view("||", 2);
        }
        kimix::vector<int32_t> depths;
        pt_compute_depths(line, mask, depths);
        const int32_t depth = depths[best_pos];
        // Left boundary.
        size_t lo = best_pos;
        while (lo > 0) {
            const size_t k = lo - 1;
            if (pt_code(mask, k)) {
                if (depths[k] < depth) {
                    break;
                }
                if (depths[k] == depth && line[k] == ';') {
                    break;
                }
            }
            lo = k;
        }
        // Right boundary.
        size_t hi = best_pos + 2;
        bool prev_code = true;
        while (hi < line.size()) {
            const char c = line[hi];
            const bool cur = pt_code(mask, hi);
            if (!cur) {
                if (prev_code && c == '#') {
                    break;
                }
                prev_code = false;
                ++hi;
                continue;
            }
            prev_code = true;
            if (depths[hi] < depth) {
                break;
            }
            if (depths[hi] == depth) {
                if (c == ';' || c == ')' || c == ']' || c == '}') {
                    break;
                }
                if (pt_starts_with(line.substr(hi), "&&") ||
                    pt_starts_with(line.substr(hi), "||")) {
                    break;
                }
            }
            ++hi;
        }
        kimix::string left_seg = strip_py(line.substr(lo, best_pos - lo));
        kimix::string right_seg = strip_py(line.substr(best_pos + 2, hi - best_pos - 2));
        // A segment may still contain a chain operator at a DEEPER nesting
        // level; rewrite it recursively so no raw operator survives.
        if (left_seg.find("&&") != pt_npos || left_seg.find("||") != pt_npos) {
            transform_chain_line(left_seg, warnings);
        }
        if (right_seg.find("&&") != pt_npos || right_seg.find("||") != pt_npos) {
            transform_chain_line(right_seg, warnings);
        }
        const char opener =
            enclosing_group_opener(line, best_pos, mask, depths, depth);
        const bool wrap_value =
            (opener == '(' || opener == '[');
        kimix::string_view a1, a3;
        bool has_assign = false;
        if (opener == 0 && depth == 0) {
            if (match_chain_assign(left_seg, a1, a3) && !strip_py(a3).empty()) {
                has_assign = true;
            }
        }
        kimix::string_view prefix = kimix::string_view(line).substr(0, lo);
        kimix::string_view suffix = kimix::string_view(line).substr(hi);
        kimix::string new_line;
        if (has_assign) {
            // The regex's group(3) is stripped before use.
            kimix::string rest = strip_py(a3);
            kimix::string_view rest_v(rest);
            new_line = chain_emit(prefix, left_seg, right_seg, best_op,
                                  wrap_value, suffix, &a1, &rest_v);
        } else {
            new_line = chain_emit(prefix, left_seg, right_seg, best_op,
                                  wrap_value, suffix, nullptr, nullptr);
        }
        if (warnings != nullptr) {
            kimix::string body = "pipeline chain `";
            body += left_seg;
            body.push_back(' ');
            body.append(best_op.data(), best_op.size());
            body.push_back(' ');
            body += right_seg;
            body += "` rewritten to `";
            body += new_line;
            body += "`";
            warnings->push_back(body);
        }
        line = std::move(new_line);
    }
    // Phase 2: chains inside interpolated sub-expressions (mask 2).  Region
    // edges are hard boundaries here.
    while (true) {
        const pt_mask mask = pt_line_mask(line);
        const size_t and_pos = find_interp_op(line, kimix::string_view("&&", 2), mask);
        const size_t or_pos = find_interp_op(line, kimix::string_view("||", 2), mask);
        if (and_pos == pt_npos && or_pos == pt_npos) {
            break;
        }
        size_t best_pos = 0;
        kimix::string_view best_op;
        if (and_pos == pt_npos) {
            best_pos = or_pos;
            best_op = kimix::string_view("||", 2);
        } else if (or_pos == pt_npos) {
            best_pos = and_pos;
            best_op = kimix::string_view("&&", 2);
        } else if (and_pos < or_pos) {
            best_pos = and_pos;
            best_op = kimix::string_view("&&", 2);
        } else {
            best_pos = or_pos;
            best_op = kimix::string_view("||", 2);
        }
        kimix::vector<int32_t> depths;
        pt_compute_depths(line, mask, depths);
        const int32_t depth = depths[best_pos];
        size_t lo = best_pos;
        while (lo > 0) {
            const size_t k = lo - 1;
            if (!pt_code(mask, k)) {
                if (interp_is_literal(line, mask, k)) {
                    size_t s = 0, e = 0;
                    mask0_span(line, mask, k, s, e);
                    lo = s;
                    continue;
                }
                break;
            }
            if (depths[k] < depth) {
                break;
            }
            if (depths[k] == depth && line[k] == ';') {
                break;
            }
            lo = k;
        }
        size_t hi = best_pos + 2;
        while (hi < line.size()) {
            if (!pt_code(mask, hi)) {
                if (interp_is_literal(line, mask, hi)) {
                    size_t s = 0, e = 0;
                    mask0_span(line, mask, hi, s, e);
                    hi = e;
                    continue;
                }
                break;
            }
            if (depths[hi] < depth) {
                break;
            }
            if (depths[hi] == depth) {
                const char c = line[hi];
                if (c == ';' || c == ')' || c == ']' || c == '}') {
                    break;
                }
                if (pt_starts_with(line.substr(hi), "&&") ||
                    pt_starts_with(line.substr(hi), "||")) {
                    break;
                }
            }
            ++hi;
        }
        kimix::string left_seg = strip_py(line.substr(lo, best_pos - lo));
        kimix::string right_seg = strip_py(line.substr(best_pos + 2, hi - best_pos - 2));
        if (left_seg.find("&&") != pt_npos || left_seg.find("||") != pt_npos) {
            transform_chain_line(left_seg, warnings);
        }
        if (right_seg.find("&&") != pt_npos || right_seg.find("||") != pt_npos) {
            transform_chain_line(right_seg, warnings);
        }
        const char opener =
            enclosing_group_opener(line, best_pos, mask, depths, depth);
        const bool wrap_value = (opener == '(' || opener == '[');
        kimix::string_view prefix = kimix::string_view(line).substr(0, lo);
        kimix::string_view suffix = kimix::string_view(line).substr(hi);
        kimix::string new_line = chain_emit(prefix, left_seg, right_seg, best_op,
                                            wrap_value, suffix, nullptr, nullptr);
        if (warnings != nullptr) {
            kimix::string body = "pipeline chain `";
            body += left_seg;
            body.push_back(' ');
            body.append(best_op.data(), best_op.size());
            body.push_back(' ');
            body += right_seg;
            body += "` rewritten to `";
            body += new_line;
            body += "`";
            warnings->push_back(body);
        }
        line = std::move(new_line);
    }
}

// -- transform: null-conditional  (?. and ?[) -------------------------------

size_t scan_member_name(kimix::string_view line, size_t ms,
                        const pt_mask& mask) {
    const size_t n = line.size();
    if (ms >= n) {
        return ms;
    }
    const char c0 = line[ms];
    if (c0 == '\'') {
        return scan_single_quoted(line, ms);
    }
    if (c0 == '"') {
        return scan_double_quoted(line, ms);
    }
    if (c0 != '$') {
        size_t me = ms;
        while (me < n && (ascii_alnum(line[me]) || line[me] == '_')) {
            ++me;
        }
        return me;
    }
    // Variable member ($var, ${var}, $? etc.)
    size_t me = ms + 1;
    if (me >= n) {
        return me;
    }
    const char ch = line[me];
    if (ch == '{') {
        size_t bd = 1;
        me += 1;
        while (me < n && bd > 0) {
            if (line[me] == '{') {
                bd += 1;
            } else if (line[me] == '}') {
                bd -= 1;
            }
            ++me;
        }
    } else if (ch == '?' || ch == '$' || ch == '^') {
        me += 1;
    } else {
        while (me < n &&
               (ascii_alnum(line[me]) || line[me] == '_' || line[me] == ':')) {
            ++me;
        }
    }
    return me;
}

void scan_method_args(kimix::string_view line, size_t start,
                      const pt_mask& mask, kimix::string_view& args,
                      size_t& after) {
    size_t j = start;
    const size_t n = line.size();
    while (j < n && line[j] == ' ') {
        ++j;
    }
    if (j >= n || line[j] != '(') {
        args = kimix::string_view();
        after = start;
        return;
    }
    size_t d = 1;
    size_t k = j + 1;
    while (k < n && d > 0) {
        if (pt_code(mask, k)) {
            if (line[k] == '(') {
                d += 1;
            } else if (line[k] == ')') {
                d -= 1;
            }
        }
        ++k;
    }
    args = line.substr(j, k - j);
    after = k;
}

void transform_null_conditional_line(kimix::string& line, kimix::string_view op,
                                     kimix::vector<kimix::string>* warnings) {
    const bool is_dot = (op == "?.");
    const size_t op_len = op.size();
    if (line.find(op) == pt_npos) {
        return;
    }
    size_t search = 0;
    while (true) {
        const pt_mask mask = pt_line_mask(line);
        const size_t idx = find_next_op(line, op, mask, true, search, false);
        if (idx == pt_npos) {
            return;
        }
        size_t expr_start = 0, expr_end = 0;
        expr_left(line, idx, mask, kimix::string_view("?:", 2), expr_start,
                  expr_end);
        kimix::string base = strip_py(line.substr(expr_start, expr_end - expr_start));
        {
            kimix::string_view bv(base);
            strip_command_prefix(bv, expr_start, true);
            strip_keyword_operand(bv, expr_start);
            base.assign(bv.data(), bv.size());
        }
        kimix::vector<kimix::string> keeps;
        lift_region_placeholders(base, keeps);
        if (base.empty() || !looks_like_value_expr(base)) {
            // A bareword "base" is 5.1 command text whose ``?`` is a filename
            // wildcard (``rm f?.bak``), not a null-conditional.
            search = idx + op_len;
            continue;
        }
        if (is_dot && base.find("?[") != pt_npos) {
            // Mixed chain (``$a?[$i]?.b``): transform the index form first.
            transform_null_conditional_line(base, kimix::string_view("?[", 2),
                                            warnings);
            if (base.empty()) {
                search = idx + op_len;
                continue;
            }
        }
        kimix::string inner;
        size_t end_pos = 0;
        kimix::string orig_expr;
        if (is_dot) {
            kimix::vector<kimix::string> segments;  // ".member(args)"
            kimix::vector<size_t> seg_ends;
            kimix::vector<kimix::string> prefixes;
            prefixes.push_back(base);
            size_t cur = idx;
            while (cur + 1 < line.size() && line[cur] == '?' &&
                   line[cur + 1] == '.') {
                size_t ms = cur + 2;
                while (ms < line.size() && line[ms] == ' ') {
                    ++ms;
                }
                const size_t me = scan_member_name(line, ms, mask);
                if (me == ms) {
                    break;
                }
                const kimix::string_view mem = kimix::string_view(line).substr(ms, me - ms);
                kimix::string_view args;
                size_t after = 0;
                scan_method_args(line, me, mask, args, after);
                kimix::string seg = ".";
                seg.append(mem.data(), mem.size());
                seg.append(args.data(), args.size());
                segments.push_back(seg);
                seg_ends.push_back(after);
                kimix::string next_prefix = prefixes.back();
                next_prefix += segments.back();
                prefixes.push_back(std::move(next_prefix));
                cur = after;
            }
            if (segments.empty()) {
                search = idx + op_len;
                continue;
            }
            kimix::string full_expr = prefixes.back();
            for (size_t k = prefixes.size() - 1; k > 0; --k) {
                kimix::string wrapped = "if ($null -ne ";
                wrapped += prefixes[k - 1];
                wrapped += ") { ";
                wrapped += full_expr;
                wrapped += " }";
                full_expr = std::move(wrapped);
            }
            inner = "$(";
            inner += full_expr;
            inner += ")";
            end_pos = seg_ends.back();
            orig_expr = base;
            orig_expr += "?.";
            for (size_t k = 0; k < segments.size(); ++k) {
                if (k > 0) {
                    orig_expr += "?.";
                }
                orig_expr.append(segments[k].data() + 1, segments[k].size() - 1);
            }
        } else {
            size_t bracket_depth = 1;
            size_t bracket_end = idx + 2;
            while (bracket_end < line.size() && bracket_depth > 0) {
                const char c = line[bracket_end];
                if (pt_code(mask, bracket_end)) {
                    if (c == '[') {
                        bracket_depth += 1;
                    } else if (c == ']') {
                        bracket_depth -= 1;
                    }
                }
                ++bracket_end;
            }
            const kimix::string_view index_expr =
                kimix::string_view(line).substr(idx + 2, bracket_end - 1 - (idx + 2));
            inner = "$(if ($null -ne ";
            inner += base;
            inner += ") { ";
            inner += base;
            inner += "[";
            inner.append(index_expr.data(), index_expr.size());
            inner += "] })";
            end_pos = bracket_end;
            orig_expr = base;
            orig_expr += "?[";
            orig_expr.append(index_expr.data(), index_expr.size());
            orig_expr += "]";
        }
        if (warnings != nullptr) {
            kimix::string body = "null-conditional ";
            body += is_dot ? "member access `" : "index `";
            body += orig_expr;
            body += "` rewritten to `";
            body += inner;
            body += "`";
            warnings->push_back(body);
        }
        kimix::string prefix = line.substr(0, expr_start);
        prefix += keeps_prefix(keeps);
        kimix::string new_line = emit_replacement(prefix, inner);
        new_line.append(line.data() + end_pos, line.size() - end_pos);
        line = std::move(new_line);
        search = 0;
    }
}

// -- whole-code transform ---------------------------------------------------

// _find_multiline_regions: line indices touched by a region spanning lines.
void find_multiline_regions(kimix::string_view code, const pt_mask& mask,
                            kimix::vector<uint8_t>& multi) {
    const size_t n = code.size();
    size_t i = 0;
    size_t line_idx = 0;
    while (i < n) {
        if (!pt_code(mask, i)) {
            const size_t start_line = line_idx;
            while (i < n && !pt_code(mask, i)) {
                if (code[i] == '\n') {
                    ++line_idx;
                }
                ++i;
            }
            if (line_idx > start_line) {
                for (size_t li = start_line; li <= line_idx; ++li) {
                    if (multi.size() <= li) {
                        multi.resize(li + 1, 0);
                    }
                    multi[li] = 1;
                }
            }
        } else {
            if (code[i] == '\n') {
                ++line_idx;
            }
            ++i;
        }
    }
}

// _find_multiline_region_open_lines: the line a multi-line region starts on.
void find_multiline_region_open_lines(kimix::string_view code,
                                      const pt_mask& mask,
                                      kimix::vector<uint8_t>& opens) {
    const size_t n = code.size();
    size_t i = 0;
    size_t line_idx = 0;
    while (i < n) {
        if (!pt_code(mask, i)) {
            const size_t start_line = line_idx;
            while (i < n && !pt_code(mask, i)) {
                if (code[i] == '\n') {
                    ++line_idx;
                }
                ++i;
            }
            if (line_idx > start_line) {
                if (opens.size() <= start_line) {
                    opens.resize(start_line + 1, 0);
                }
                opens[start_line] = 1;
            }
        } else {
            if (code[i] == '\n') {
                ++line_idx;
            }
            ++i;
        }
    }
}

void scan_pwsh_transform(kimix::string_view cmd, kimix::string* transformed,
                         kimix::vector<kimix::string>* warnings) {
    if (transformed == nullptr) {
        return;
    }
    const kimix::string code = join_continuation_lines(cmd);
    // lines = code.split("\n") (a trailing empty segment is kept)
    kimix::vector<kimix::string> lines;
    {
        size_t start = 0;
        for (size_t i = 0; i <= code.size(); ++i) {
            if (i == code.size() || code[i] == '\n') {
                lines.push_back(code.substr(start, i - start));
                start = i + 1;
            }
        }
    }
    const pt_mask mask = pt_build_region_mask(code, true);
    kimix::vector<uint8_t> multi;
    kimix::vector<uint8_t> opens;
    find_multiline_regions(code, mask, multi);
    find_multiline_region_open_lines(code, mask, opens);
    kimix::vector<size_t> line_starts;
    line_starts.reserve(lines.size());
    {
        size_t off = 0;
        for (const kimix::string& line : lines) {
            line_starts.push_back(off);
            off += line.size() + 1;
        }
    }
    kimix::string result;
    result.reserve(code.size());
    kimix::vector<kimix::string> line_warnings;
    auto run_transforms = [&](kimix::string& line) {
        line_warnings.clear();
        transform_nca_line(line, &line_warnings);
        transform_null_conditional_line(line, kimix::string_view("?.", 2),
                                        &line_warnings);
        transform_null_conditional_line(line, kimix::string_view("?[", 2),
                                        &line_warnings);
        transform_nc_line(line, &line_warnings);
        transform_ternary_line(line, &line_warnings);
        transform_chain_line(line, &line_warnings);
    };
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            result.push_back('\n');
        }
        kimix::string line = lines[i];
        if (i < multi.size() && multi[i] != 0) {
            // The line touches a multi-line string/comment/here-string region.
            if (i < opens.size() && opens[i] != 0) {
                // This line *opens* the region: its header must not be folded
                // into a generated construct.
                result += line;
                continue;
            }
            const size_t base = line_starts[i];
            const size_t nline = line.size();
            kimix::vector<kimix::string> orig_texts;
            kimix::vector<kimix::string> placeholders;
            bool whole_region = false;
            {
                size_t j = 0;
                while (j < nline) {
                    if (!pt_code(mask, base + j)) {
                        const size_t k0 = j;
                        while (j < nline && !pt_code(mask, base + j)) {
                            ++j;
                        }
                        if (k0 == 0 && j == nline) {
                            whole_region = true;
                        }
                    } else {
                        ++j;
                    }
                }
            }
            if (whole_region) {
                // Wholly inside the region — nothing live on this line.
                result += line;
                continue;
            }
            kimix::string pline;
            pline.reserve(nline + 32);
            {
                size_t j = 0;
                size_t prev = 0;
                size_t ridx = 0;
                while (j < nline) {
                    if (!pt_code(mask, base + j)) {
                        const size_t s = j;
                        while (j < nline && !pt_code(mask, base + j)) {
                            ++j;
                        }
                        kimix::string ph = "__kimixrgn";
                        ph += kimix::format("{}", i);
                        ph += "x";
                        ph += kimix::format("{}", ridx);
                        ph += "__";
                        placeholders.push_back(ph);
                        orig_texts.push_back(line.substr(s, j - s));
                        pline.append(line.substr(prev, s - prev));
                        pline += ph;
                        prev = j;
                        ++ridx;
                    } else {
                        ++j;
                    }
                }
                pline.append(line.substr(prev));
            }
            run_transforms(pline);
            if (warnings != nullptr) {
                for (const kimix::string& msg : line_warnings) {
                    warnings->push_back(
                        format_transform_warning(static_cast<uint32_t>(i + 1),
                                                 msg));
                }
            }
            for (size_t r = 0; r < placeholders.size(); ++r) {
                const kimix::string& ph = placeholders[r];
                const kimix::string& orig = orig_texts[r];
                kimix::string replaced;
                replaced.reserve(pline.size());
                size_t pos = 0;
                while (true) {
                    const size_t hit = pline.find(ph, pos);
                    if (hit == pt_npos) {
                        replaced.append(pline.substr(pos));
                        break;
                    }
                    replaced.append(pline.substr(pos, hit - pos));
                    replaced += orig;
                    pos = hit + ph.size();
                }
                pline = std::move(replaced);
            }
            result += pline;
            continue;
        }
        run_transforms(line);
        if (warnings != nullptr) {
            for (const kimix::string& msg : line_warnings) {
                warnings->push_back(format_transform_warning(
                    static_cast<uint32_t>(i + 1), msg));
            }
        }
        result += line;
    }
    *transformed = std::move(result);
}

} // namespace src_runtime_parse_shell_scanner
using namespace src_runtime_parse_shell_scanner;

// ===========================================================================
// Public entry point
// ===========================================================================

void scan_shell(shell_dialect dialect, kimix::string_view cmd,
                kimix::vector<edit>& edits, kimix::string* transformed,
                kimix::vector<kimix::string>* names,
                kimix::vector<kimix::string>* notes, int* warning_code,
                kimix::vector<kimix::string>* warnings,
                kimix::vector<kimix::string>* nul_notes,
                kimix::vector<kimix::string>* unsupported) {
    switch (dialect) {
    case shell_dialect::BASH_FIX:
        scan_bash_fix(cmd, edits, transformed, names, notes, nul_notes,
                      unsupported);
        break;
    case shell_dialect::BASH_PROCESS_UNQUOTED:
        scan_process_unquoted(cmd, edits, transformed);
        break;
    case shell_dialect::PWSH_FIX: {
        int warning = PW_OK;
        scan_pwsh_fix(cmd, edits, transformed, &warning);
        if (warning_code) {
            *warning_code = warning;
        }
        break;
    }
    case shell_dialect::PWSH_TRANSFORM:
        scan_pwsh_transform(cmd, transformed, warnings);
        break;
    }
}

// -- RegionMask -------------------------------------------------------------

RegionMask::RegionMask(uint32_t n) : size_(n), bits_((n + 63) / 64, ~uint64_t{0}) {}

// The reference's three-state mask (_shell_compat.py / pwsh_transform.py
// ``_mask_double_quoted``): the interior of a ``$( ... )`` interpolation inside
// a double-quoted string is *live code* (mask value 2), with nested
// strings/comments masked 0 again.  The PS7->PS5.1 transformer keeps the
// two-state mask, so only the pwsh_fix mask exposes the distinction -- that is
// what lets ``"$( > nul"`` get its null-device target rewritten after the quote
// repair appends the closing ``"`` (the second rewrite pass sees the target as
// code).
void mask_interpolation_code(kimix::string_view code, size_t lo, size_t hi,
                             RegionMask& mask) {
    size_t i = lo;
    while (i < hi) {
        const char c = code[i];
        if (c == '`' && i + 1 < hi) {
            i += 2;
            continue;
        }
        if (c == '\'') {
            i = std::min(scan_single_quoted(code, i), hi);
            continue;
        }
        if (c != '"') {
            i += 1;
            continue;
        }
        size_t j = i + 1;
        while (j < hi) {
            const char ch = code[j];
            if (ch == '`' && j + 1 < hi) {
                j += 2;
                continue;
            }
            if (ch == '"') {
                j += 1;
                break;
            }
            if (ch != '$' || j + 1 >= hi || code[j + 1] != '(') {
                j += 1;
                continue;
            }
            const size_t sub_end = std::min(skip_subexpression(code, j), hi);
            const size_t inner_lo = std::min(j + 2, sub_end);
            const size_t inner_hi = sub_end;
            if (inner_hi > inner_lo) {
                mask.set(static_cast<uint32_t>(inner_lo),
                         static_cast<uint32_t>(inner_hi));
                // Nested strings / comments / here-strings stay regions.
                RegionMask sub(static_cast<uint32_t>(inner_hi - inner_lo));
                build_region_mask(code.substr(inner_lo, inner_hi - inner_lo),
                                  true, sub, true);
                for (size_t k = 0; k < inner_hi - inner_lo; ++k) {
                    if (!sub.is_code(static_cast<uint32_t>(k))) {
                        mask.mark(static_cast<uint32_t>(inner_lo + k),
                                  static_cast<uint32_t>(inner_lo + k + 1));
                    }
                }
                // Deeper interpolations: set their interiors too.
                mask_interpolation_code(code, inner_lo, inner_hi, mask);
            }
            j = sub_end > j ? sub_end : j + 2;
        }
        i = j;
    }
}

void build_pwsh_region_mask(kimix::string_view code, RegionMask& mask) {
    // ``hash_token_boundary``: the reference only opens a comment when ``#``
    // starts a token, so ``foo#c`` keeps the rest of the line as code.
    build_region_mask(code, true, mask, true);
    mask_interpolation_code(code, 0, code.size(), mask);
}

void RegionMask::mark(uint32_t start, uint32_t end) {
    if (start >= end) {
        return;
    }
    if (end > size_) {
        end = size_;
    }
    if (start >= end) {
        return;
    }
    // Clear [start, end) with whole-word updates instead of one bit per byte.
    const uint32_t first_word = start >> 6;
    const uint32_t last_word = (end - 1) >> 6;
    const uint64_t first_mask = ~uint64_t{0} << (start & 63);
    const uint64_t last_mask = ~uint64_t{0} >> (63 - ((end - 1) & 63));
    if (first_word == last_word) {
        bits_[first_word] &= ~(first_mask & last_mask);
    } else {
        bits_[first_word] &= ~first_mask;
        for (uint32_t w = first_word + 1; w < last_word; ++w) {
            bits_[w] = 0;
        }
        bits_[last_word] &= ~last_mask;
    }
}

void RegionMask::set(uint32_t start, uint32_t end) {
    if (start >= end) {
        return;
    }
    if (end > size_) {
        end = size_;
    }
    if (start >= end) {
        return;
    }
    const uint32_t first_word = start >> 6;
    const uint32_t last_word = (end - 1) >> 6;
    const uint64_t first_mask = ~uint64_t{0} << (start & 63);
    const uint64_t last_mask = ~uint64_t{0} >> (63 - ((end - 1) & 63));
    if (first_word == last_word) {
        bits_[first_word] |= (first_mask & last_mask);
    } else {
        bits_[first_word] |= first_mask;
        for (uint32_t w = first_word + 1; w < last_word; ++w) {
            bits_[w] = ~uint64_t{0};
        }
        bits_[last_word] |= last_mask;
    }
}

bool RegionMask::is_code(uint32_t i) const noexcept {
    if (i >= size_) {
        return false;
    }
    return (bits_[i >> 6] >> (i & 63)) & 1u;
}

} // namespace parse
} // namespace runtime
} // namespace kimix
