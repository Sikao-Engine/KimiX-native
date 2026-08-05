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
 * data lives in the shim). ASCII-only character classes where the reference
 * uses str.isalnum()/isalpha() (PWSH_FIX token boundary); the shim routes
 * non-ASCII input to the _compat mirror for those dialects.
 */

#include <runtime/parse/ascii_util.h>
#include <runtime/parse/shell_scanner.h>

#include <cstring>

namespace kimix {
namespace runtime {
namespace parse {

namespace {

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
        while (i < n) {
            if (s[i] == '\'') {
                if (i + 1 < n && s[i + 1] == '\'') {
                    i += 2;
                } else {
                    return i + 1;
                }
            } else {
                i += 1;
            }
        }
        return i;
    }

    size_t skip_dq(size_t start) const noexcept {
        size_t i = start + 1;
        while (i < n) {
            const char ch = s[i];
            if (ch == '`') {
                i += (i + 1 < n) ? 2 : 1;
            } else if (ch == '"') {
                if (i + 1 < n && s[i + 1] == '"') {
                    i += 2;
                } else {
                    return i + 1;
                }
            } else if (ch == '$' && i + 1 < n && s[i + 1] == '(') {
                i = skip_subexpr(i);
            } else {
                i += 1;
            }
        }
        return i;
    }

    size_t skip_block(size_t start) const noexcept {
        size_t i = start + 2;
        while (i < n) {
            if (s[i] == '#' && i + 1 < n && s[i + 1] == '>') {
                return i + 2;
            }
            i += 1;
        }
        return i;
    }

    size_t skip_subexpr(size_t start) const noexcept {
        size_t i = start + 2;
        size_t depth = 1;
        while (i < n && depth) {
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
                    while (i < n && s[i] != '\n') {
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
                if (ch == '"') {
                    saw_code = true;
                    mode = DQ;
                    i += 1;
                } else if (ch == '\'') {
                    saw_code = true;
                    mode = SQ;
                    i += 1;
                } else if (ch == '`') {
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
                } else if (ch == '#' && at_token_start(i)) {
                    mode = COMMENT;
                    i += 1;
                } else if (ch == '<' && i + 1 < n && s[i + 1] == '#') {
                    mode = BLOCK;
                    i += 2;
                } else if (ch == '@' && i + 1 < n &&
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
                } else if (ch == '-' && i + 2 < n && s[i + 1] == '-' &&
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
                } else if (ch == '$' && i + 1 < n && s[i + 1] == '(') {
                    saw_code = true;
                    i = skip_subexpr(i);
                } else if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
                           ch == '\v' || ch == '\f') {
                    i += 1;
                } else {
                    saw_code = true;
                    i += 1;
                }
            } else if (mode == DQ) {
                if (ch == '`') {
                    i += (i + 1 < n) ? 2 : 1;
                } else if (ch == '"') {
                    if (i + 1 < n && s[i + 1] == '"') {
                        i += 2;
                    } else {
                        mode = NORMAL;
                        i += 1;
                    }
                } else if (ch == '$' && i + 1 < n && s[i + 1] == '(') {
                    i = skip_subexpr(i);
                } else {
                    i += 1;
                }
            } else if (mode == SQ) {
                if (ch == '\'') {
                    if (i + 1 < n && s[i + 1] == '\'') {
                        i += 2;
                    } else {
                        mode = NORMAL;
                        i += 1;
                    }
                } else {
                    i += 1;
                }
            } else if (mode == HDQ || mode == HSQ) {
                if (ch == '\n') {
                    line_start = i + 1;
                    i += 1;
                } else if (ch == here_quote && i + 1 < n && s[i + 1] == '@' &&
                           s.substr(line_start, i - line_start).find_first_not_of(" \t\r") ==
                               kimix::string_view::npos) {
                    mode = NORMAL;
                    i += 2;
                } else {
                    i += 1;
                }
            } else if (mode == COMMENT) {
                if (ch == '\n') {
                    mode = NORMAL;
                    i += 1;
                } else {
                    i += 1;
                }
            } else { // BLOCK
                if (ch == '#' && i + 1 < n && s[i + 1] == '>') {
                    mode = NORMAL;
                    i += 2;
                } else {
                    i += 1;
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

// _FALLBACK_BODIES keys: 19 explicit + 36 g-prefixed GNU names (no plain
// GNU names; verified against bash_fix.py lines 130-344).
const char* const kFallbackNames[] = {
    "gawk", "gcat", "gcomm", "gcp", "gcut", "gdate", "gdf", "gdu",
    "gegrep", "gfgrep", "gfind", "ggrep", "ghead", "gjoin", "gln",
    "gls", "gmake", "gmkdir", "gmv", "gpaste", "greadlink", "grealpath",
    "grm", "grmdir", "gsed", "gseq", "gshuf", "gsort", "gsplit",
    "gstat", "gtail", "gtar", "gtimeout", "gtr", "guniq", "gwc",
    "gxargs", "nc", "open", "pbcopy", "pbpaste", "pgrep", "pip3",
    "pkill", "python3", "rev", "say", "tree", "wget", "wl-copy",
    "wl-paste", "xclip", "xdg-open", "xsel", "traceroute", "zip",
};

bool is_fallback_name(kimix::string_view name) noexcept {
    for (const char* n : kFallbackNames) {
        if (name == kimix::string_view(n)) {
            return true;
        }
    }
    return false;
}

// "\x01<name>\x01" marker for fallback-command edits; the shim expands it.
kimix::string fallback_marker(kimix::string_view name) {
    kimix::string m;
    m.push_back('\x01');
    m.append(name.data(), name.size());
    m.push_back('\x01');
    return m;
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
    size_t nest_depth = 0;
    bool aborted = false;
    // Depth bound for the scanner recursion. The reference uses 1024
    // (_MAX_NESTING_DEPTH) but Python's interpreter limit (~1000) fires
    // first in practice; the C++ frames are much larger than Python frames,
    // so 1024 levels would overflow a 1 MiB thread stack when the kernel is
    // called from Python. 256 levels (about 5 frames per level) stays well
    // inside 1 MiB and is far beyond any real command; deeper input takes
    // the reference's RecursionError path (command returned unchanged).
    static constexpr size_t MAX_DEPTH = 256;


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
    bool literal_command_name(kimix::string_view raw, kimix::string& name) const {
        kimix::string value;
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
        if (is_fallback_name(value)) {
            name = value;
            return true;
        }
        return false;
    }

    // -- word / region skips ------------------------------------------------
    size_t skip_single_quote(size_t i, size_t end) const noexcept {
        const size_t close = find_char_bounded('\'', i, end);
        return (close == kimix::string_view::npos) ? end : close + 1;
    }

    size_t skip_ansi_quote(size_t i, size_t end) const noexcept {
        while (i < end) {
            if (s[i] == '\\') {
                i += (i + 1 < end) ? 2 : 1;
            } else if (s[i] == '\'') {
                return i + 1;
            } else {
                i += 1;
            }
        }
        return end;
    }

    size_t skip_double_quote(size_t i, size_t end) {
        while (i < end) {
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
        return end;
    }

    size_t skip_double_quote_for_matching(size_t i, size_t end) {
        while (i < end) {
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
        return end;
    }

    size_t find_backtick_end(size_t i, size_t end) const noexcept {
        while (i < end) {
            if (s[i] == '\\') {
                i += (i + 1 < end) ? 2 : 1;
            } else if (s[i] == '`') {
                return i;
            } else {
                i += 1;
            }
        }
        return end;
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
        while (i < end) {
            const char ch = s[i];
            if (word_end_char(ch)) {
                break;
            }
            if (ch == '#' && i == start) {
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
        while (i < end) {
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
        for (const HereDoc& document : documents) {
            const size_t body_start = i;
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

    // -- Windows path rewrite -----------------------------------------------
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
        while (i < end) {
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
                i = word_end;
                command_expected = redirect_resume;
                redirect_expected = false;
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
            command_expected = false;
            clear_wrapper();
        }
    }
};

void scan_bash_fix(kimix::string_view cmd, kimix::vector<edit>& edits,
                   kimix::string* transformed,
                   kimix::vector<kimix::string>* names,
                   kimix::vector<kimix::string>* notes) {
    edits.clear();
    if (names) {
        names->clear();
    }
    if (notes) {
        notes->clear();
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
    while (i < n) {
        if (code[i] == '\'') {
            if (i + 1 < n && code[i + 1] == '\'') {
                i += 2;
            } else {
                return i + 1;
            }
        } else {
            i += 1;
        }
    }
    return i;
}

size_t scan_double_quoted(kimix::string_view code, size_t i) noexcept {
    i += 1;
    const size_t n = code.size();
    while (i < n) {
        const char ch = code[i];
        if (ch == '`' && i + 1 < n) {
            i += 2;
        } else if (ch == '"') {
            return i + 1;
        } else if (ch == '$' && i + 1 < n && code[i + 1] == '(') {
            i = skip_subexpression(code, i);
        } else {
            i += 1;
        }
    }
    return i;
}

size_t scan_block_comment(kimix::string_view code, size_t i) noexcept {
    // NESTED <# ... #> (process_pwsh semantics; differs from pwsh_fix)
    size_t depth = 1;
    i += 2;
    const size_t n = code.size();
    while (i < n && depth) {
        if (code[i] == '<' && i + 1 < n && code[i + 1] == '#') {
            depth += 1;
            i += 2;
        } else if (code[i] == '#' && i + 1 < n && code[i + 1] == '>') {
            depth -= 1;
            i += 2;
        } else {
            i += 1;
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
    while (i < n) {
        if (code[i] == '\n') {
            line_begin = i + 1;
        } else if (code[i] == quote && i + 1 < n && code[i + 1] == '@' &&
                   code.substr(line_begin, i - line_begin)
                           .find_first_not_of(" \t\r\n\v\f") ==
                       kimix::string_view::npos) {
            return i + 2;
        }
        i += 1;
    }
    return i;
}

// mask: all positions initially code; mark(start,end) clears them.
void build_region_mask(kimix::string_view code, bool here_strings,
                       RegionMask& mask) {
    const size_t n = code.size();
    size_t i = 0;
    while (i < n) {
        const char c = code[i];
        if (c == '<' && i + 1 < n && code[i + 1] == '#') {
            const size_t start = i;
            i = scan_block_comment(code, i);
            mask.mark(static_cast<uint32_t>(start), static_cast<uint32_t>(i));
        } else if (c == '#') {
            const size_t start = i;
            while (i < n && code[i] != '\n') {
                ++i;
            }
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

kimix::string join_continuation_lines(kimix::string_view code) {
    RegionMask mask(static_cast<uint32_t>(code.size()));
    build_region_mask(code, true, mask);
    const size_t n = code.size();
    kimix::string result;
    result.reserve(n);
    size_t i = 0;
    while (i < n) {
        if (code[i] == '`' && mask.is_code(static_cast<uint32_t>(i))) {
            size_t j = i + 1;
            while (j < n && (code[j] == ' ' || code[j] == '\t' || code[j] == '\r')) {
                ++j;
            }
            if (j < n && code[j] == '\n') {
                j += 1;
                while (j < n && (code[j] == ' ' || code[j] == '\t' || code[j] == '\r')) {
                    ++j;
                }
                result.push_back(' ');
                i = j;
                continue;
            }
        }
        result.push_back(code[i]);
        i += 1;
    }
    return result;
}

// -- expression helpers -----------------------------------------------------

bool match_assignment(kimix::string_view before, kimix::string_view& group1,
                      kimix::string_view& group2) {
    // _ASSIGN_RE = (.*?)(\$\w+(?::\w+)?(?:\.\w+)*)\s*=\s*$  over rstrip
    size_t end = before.size();
    while (end > 0 && py_space(before[end - 1])) {
        --end;
    }
    const kimix::string_view t = before.substr(0, end);
    const size_t n = t.size();
    auto word_char = [](char c) noexcept {
        return ascii_alnum(c) || c == '_';
    };
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

kimix::string build_replacement(kimix::string_view prefix, kimix::string_view inner) {
    kimix::string_view g1, g2;
    if (match_assignment(prefix, g1, g2)) {
        kimix::string out;
        out.append(g1.data(), g1.size());
        out.append(g2.data(), g2.size());
        out.append(" = ");
        out.append(inner.data(), inner.size());
        return out;
    }
    kimix::string out;
    out.append(prefix.data(), prefix.size());
    out.append(inner.data(), inner.size());
    return out;
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

// strip a leading command name; returns the (possibly) stripped expression
// and the adjusted start offset.
void strip_command_prefix(kimix::string_view& expr, size_t& start,
                          bool check_keywords) {
    size_t cmd_end = 0;
    if (!command_prefix_match(expr, cmd_end)) {
        return;
    }
    // command word = expr[0 : first whitespace], lowercased
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
            if (c0 == '$' || c0 == '(' || c0 == '[' || c0 == '"' || c0 == '\'' ||
                c0 == '@' || (c0 >= '0' && c0 <= '9')) {
                expr = expr_part;
                start += cmd_end;
            }
        }
    }
}

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

bool is_null_conditional_qmark(kimix::string_view line, size_t i) noexcept {
    return i + 1 < line.size() && line[i + 1] == '.';
}

bool is_double_colon(kimix::string_view line, size_t i) noexcept {
    return (i + 1 < line.size() && line[i + 1] == ':') ||
           (i > 0 && line[i - 1] == ':');
}

bool expr_stop(char c) noexcept { return c == '=' || c == ';' || c == '|' || c == '&' || c == ','; }
bool depth_open(char c) noexcept { return c == '(' || c == '[' || c == '{'; }
bool depth_close(char c) noexcept { return c == ')' || c == ']' || c == '}'; }

size_t find_expr_start(kimix::string_view line, size_t end, const RegionMask& mask,
                       kimix::string_view extra_stop) {
    size_t depth = 0;
    for (size_t i = end; i > 0;) {
        --i;
        if (!mask.is_code(static_cast<uint32_t>(i))) {
            continue;
        }
        const char c = line[i];
        if (depth_close(c)) {
            depth += 1;
        } else if (depth_open(c)) {
            if (depth == 0) {
                return i + 1;
            }
            depth -= 1;
        } else if (depth == 0 && (expr_stop(c) ||
                                  extra_stop.find(c) != kimix::string_view::npos)) {
            if (c == '?') {
                if (is_null_conditional_qmark(line, i) || after_dollar_question(line, i)) {
                    continue;
                }
            } else if (c == ':') {
                if (is_double_colon(line, i) || is_scope_colon(line, i)) {
                    continue;
                }
            }
            return i + 1;
        }
    }
    return 0;
}

size_t find_expr_end(kimix::string_view line, size_t start, const RegionMask& mask) {
    size_t depth = 0;
    bool prev_mask = (start == 0) ? true : mask.is_code(static_cast<uint32_t>(start - 1));
    for (size_t i = start; i < line.size(); ++i) {
        const char c = line[i];
        const bool cur_mask = mask.is_code(static_cast<uint32_t>(i));
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
            if (depth == 0) {
                return i;
            }
            depth -= 1;
        } else if (depth == 0 && expr_stop(c)) {
            return i;
        }
    }
    return line.size();
}

void expr_left(kimix::string_view line, size_t pos, const RegionMask& mask,
               kimix::string_view extra_stop, size_t& out_start, size_t& out_end) {
    size_t end = pos;
    while (end > 0 && line[end - 1] == ' ') {
        --end;
    }
    out_start = find_expr_start(line, end, mask, extra_stop);
    out_end = end;
}

void expr_right(kimix::string_view line, size_t pos, const RegionMask& mask,
                size_t& out_start, size_t& out_end) {
    size_t start = pos;
    while (start < line.size() && line[start] == ' ') {
        ++start;
    }
    out_start = start;
    out_end = find_expr_end(line, start, mask);
}

size_t find_next_op(kimix::string_view line, kimix::string_view op,
                    const RegionMask& mask, bool skip_dollar_q, size_t start,
                    bool reverse) {
    if (reverse) {
        size_t idx = line.rfind(op);
        while (idx != kimix::string_view::npos) {
            if (mask.is_code(static_cast<uint32_t>(idx)) &&
                (!skip_dollar_q || !after_dollar_question(line, idx))) {
                return idx;
            }
            idx = (idx == 0) ? kimix::string_view::npos : line.rfind(op, idx - 1);
        }
        return kimix::string_view::npos;
    }
    size_t idx = line.find(op, start);
    while (idx != kimix::string_view::npos) {
        if (mask.is_code(static_cast<uint32_t>(idx)) &&
            (!skip_dollar_q || !after_dollar_question(line, idx))) {
            return idx;
        }
        idx = line.find(op, idx + 1);
    }
    return kimix::string_view::npos;
}

void compute_depths(kimix::string_view line, const RegionMask& mask,
                    kimix::vector<size_t>& depths) {
    depths.clear();
    depths.reserve(line.size() + 1);
    size_t depth = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        depths.push_back(depth);
        if (mask.is_code(static_cast<uint32_t>(i))) {
            if (depth_open(line[i])) {
                depth += 1;
            } else if (depth_close(line[i])) {
                depth -= 1;
            }
        }
    }
    depths.push_back(depth);
}

// -- line transforms --------------------------------------------------------

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

kimix::string strip_py(kimix::string_view s) { return lstrip_py(rstrip_py(s)); }

void separate_trailing_comment(kimix::string_view line, size_t start,
                               const RegionMask& mask, size_t& content_end,
                               kimix::string_view& comment) {
    const size_t n = line.size();
    const size_t begin = (start > 1) ? start : 1;
    for (size_t ri = begin; ri < n; ++ri) {
        if (!mask.is_code(static_cast<uint32_t>(ri)) &&
            mask.is_code(static_cast<uint32_t>(ri - 1)) && line[ri] == '#') {
            content_end = ri;
            comment = line.substr(ri);
            return;
        }
    }
    content_end = n;
    comment = kimix::string_view();
}

size_t scan_member_name(kimix::string_view line, size_t ms, const RegionMask& mask) {
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
        while (me < n && (ascii_alnum(line[me]) || line[me] == '_' ||
                          line[me] == ':')) {
            ++me;
        }
    }
    return me;
}

void scan_method_args(kimix::string_view line, size_t start, const RegionMask& mask,
                      kimix::string_view& args, size_t& after) {
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
        if (mask.is_code(static_cast<uint32_t>(k))) {
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

// -- transform: ??= ---------------------------------------------------------

void transform_nca_line(kimix::string& line) {
    size_t search = 0;
    while (true) {
        RegionMask mask(static_cast<uint32_t>(line.size()));
        build_region_mask(line, false, mask);
        const size_t idx = find_next_op(line, kimix::string_view("??=", 3), mask,
                                        true, search, false);
        if (idx == kimix::string_view::npos) {
            break;
        }
        size_t left_start = 0, left_end = 0;
        expr_left(line, idx, mask, kimix::string_view(), left_start, left_end);
        const kimix::string var = strip_py(line.substr(left_start, left_end - left_start));
        if (var.empty()) {
            search = idx + 1;
            continue;
        }
        size_t val_start = 0, val_end = 0;
        expr_right(line, idx + 3, mask, val_start, val_end);
        const kimix::string value = strip_py(line.substr(val_start, val_end - val_start));
        kimix::string new_inner = "if ($null -eq ";
        new_inner += var;
        new_inner += ") { ";
        new_inner += var;
        new_inner += " = ";
        new_inner += value;
        new_inner += " }";
        const kimix::string prefix = rstrip_py(line.substr(0, left_start));
        kimix::string new_line = build_replacement(prefix, new_inner);
        new_line.append(line.data() + val_end, line.size() - val_end);
        line = new_line;
        search = left_start;
    }
}

// -- transform: ??  (via the generic operator engine) -----------------------

void transform_operator(kimix::string& line, kimix::string_view op) {
    const size_t op_len = op.size();
    size_t search = 0;
    while (true) {
        RegionMask mask(static_cast<uint32_t>(line.size()));
        build_region_mask(line, false, mask);
        const size_t idx = find_next_op(line, op, mask, true, search, false);
        if (idx == kimix::string_view::npos) {
            break;
        }
        size_t left_start = 0, left_end = 0;
        expr_left(line, idx, mask, kimix::string_view(), left_start, left_end);
        kimix::string left_expr = strip_py(line.substr(left_start, left_end - left_start));
        size_t adj_start = left_start;
        kimix::string_view left_view(left_expr);
        strip_command_prefix(left_view, adj_start, true);
        const kimix::string left_final(left_view);
        size_t right_start = 0, right_end = 0;
        expr_right(line, idx + op_len, mask, right_start, right_end);
        const kimix::string right_expr = strip_py(line.substr(right_start, right_end - right_start));
        if (left_final.empty() || right_expr.empty()) {
            search = idx + 1;
            continue;
        }
        // builder: if ($null -ne {left}) { {left} } else { {right} }
        kimix::string inner = "if ($null -ne ";
        inner += left_final;
        inner += ") { ";
        inner += left_final;
        inner += " } else { ";
        inner += right_expr;
        inner += " }";
        kimix::string new_line = build_replacement(line.substr(0, left_start), inner);
        new_line.append(line.data() + right_end, line.size() - right_end);
        line = new_line;
        search = left_start;
    }
}

void transform_nc_line(kimix::string& line) {
    transform_operator(line, kimix::string_view("??", 2));
}

// -- transform: ternary -----------------------------------------------------

size_t find_matching_colon(kimix::string_view line, size_t start, const RegionMask& mask,
                           const kimix::vector<size_t>& depth_arr) {
    for (size_t i = start; i < line.size(); ++i) {
        if (line[i] != ':' || depth_arr[i] != 0 ||
            !mask.is_code(static_cast<uint32_t>(i))) {
            continue;
        }
        if (is_double_colon(line, i) || is_scope_colon(line, i)) {
            continue;
        }
        return i;
    }
    return kimix::string_view::npos;
}

void transform_ternary_line(kimix::string& line) {
    RegionMask mask(static_cast<uint32_t>(line.size()));
    build_region_mask(line, false, mask);
    kimix::vector<size_t> depth_arr;
    compute_depths(line, mask, depth_arr);
    size_t pos = 0;
    while (pos < line.size()) {
        if (line[pos] == '?' && mask.is_code(static_cast<uint32_t>(pos)) &&
            !after_dollar_question(line, pos)) {
            const size_t colon_pos = find_matching_colon(line, pos + 1, mask, depth_arr);
            if (colon_pos != kimix::string_view::npos) {
                size_t cond_start = 0, cond_end = 0;
                expr_left(line, pos, mask, kimix::string_view(), cond_start, cond_end);
                const kimix::string condition =
                    strip_py(line.substr(cond_start, cond_end - cond_start));
                const kimix::string true_expr =
                    strip_py(line.substr(pos + 1, colon_pos - pos - 1));
                size_t false_start = 0, false_end = 0;
                expr_right(line, colon_pos + 1, mask, false_start, false_end);
                const kimix::string false_expr =
                    strip_py(line.substr(false_start, false_end - false_start));
                kimix::string_view cond_view(condition);
                size_t adj_start = cond_start;
                {
                    kimix::string_view g1, g2;
                    const bool is_assign =
                        match_assignment(line.substr(0, cond_start), g1, g2);
                    if (!is_assign) {
                        strip_command_prefix(cond_view, adj_start, false);
                    }
                }
                const kimix::string cond_final(cond_view);
                kimix::string inner = "if (";
                inner += cond_final;
                inner += ") { ";
                inner += true_expr;
                inner += " } else { ";
                inner += false_expr;
                inner += " }";
                const size_t suffix_start = false_end;
                const size_t suffix_len = line.size() - suffix_start;
                kimix::string new_line =
                    build_replacement(line.substr(0, cond_start), inner);
                new_line.append(line.data() + suffix_start, line.size() - suffix_start);
                line = new_line;
                mask = RegionMask(static_cast<uint32_t>(line.size()));
                build_region_mask(line, false, mask);
                compute_depths(line, mask, depth_arr);
                pos = line.size() - suffix_len;
                continue;
            }
        }
        ++pos;
    }
}

// -- transform: && / || -----------------------------------------------------

void transform_chain_line(kimix::string& line) {
    while (true) {
        RegionMask mask(static_cast<uint32_t>(line.size()));
        build_region_mask(line, false, mask);
        const size_t ap = find_next_op(line, kimix::string_view("&&", 2), mask,
                                       false, 0, true);
        const size_t op = find_next_op(line, kimix::string_view("||", 2), mask,
                                       false, 0, true);
        // -1 semantics (Python rfind returns -1; npos == SIZE_MAX would
        // break the > comparison below).
        const int64_t and_pos = (ap == kimix::string_view::npos) ? -1 : static_cast<int64_t>(ap);
        const int64_t or_pos = (op == kimix::string_view::npos) ? -1 : static_cast<int64_t>(op);
        if (and_pos == -1 && or_pos == -1) {
            break;
        }
        int64_t best_pos = 0;
        kimix::string_view best_op;
        if (and_pos > or_pos) {
            best_pos = and_pos;
            best_op = kimix::string_view("&&", 2);
        } else {
            best_pos = or_pos;
            best_op = kimix::string_view("||", 2);
        }
        const size_t bpos = static_cast<size_t>(best_pos);
        const kimix::string condition =
            (best_op == "&&") ? kimix::string("$?") : kimix::string("-not $?");
        const kimix::string left = strip_py(line.substr(0, bpos));
        const size_t right_start = bpos + 2;
        size_t right_raw_end = 0;
        kimix::string_view comment;
        separate_trailing_comment(line, right_start, mask, right_raw_end, comment);
        const kimix::string right =
            strip_py(line.substr(right_start, right_raw_end - right_start));
        kimix::string new_line = left;
        new_line += "; if (";
        new_line += condition;
        new_line += ") { ";
        new_line += right;
        new_line += " }";
        new_line.append(comment.data(), comment.size());
        line = new_line;
    }
}

// -- transform: null-conditional --------------------------------------------

void transform_null_conditional_line(kimix::string& line, kimix::string_view op) {
    const bool is_dot = (op == "?.");
    const size_t op_len = op.size();
    size_t search = 0;
    while (true) {
        RegionMask mask(static_cast<uint32_t>(line.size()));
        build_region_mask(line, false, mask);
        const size_t idx = find_next_op(line, op, mask, true, search, false);
        if (idx == kimix::string_view::npos) {
            return;
        }
        size_t expr_start = 0, expr_end = 0;
        expr_left(line, idx, mask, kimix::string_view("?:", 2), expr_start, expr_end);
        kimix::string base = strip_py(line.substr(expr_start, expr_end - expr_start));
        size_t adj_start = expr_start;
        kimix::string_view base_v(base);
        strip_command_prefix(base_v, adj_start, true);
        base = kimix::string(base_v);
        if (base.empty()) {
            search = idx + op_len;
            continue;
        }
        kimix::string inner;
        size_t end_pos = 0;
        if (is_dot) {
            struct Seg {
                kimix::string text;
                size_t end;
            };
            kimix::vector<Seg> segments;
            kimix::vector<kimix::string> prefixes;
            prefixes.push_back(base);
            size_t cur = idx;
            while (cur + 1 < line.size() && line[cur] == '?' && line[cur + 1] == '.') {
                size_t ms = cur + 2;
                while (ms < line.size() && line[ms] == ' ') {
                    ++ms;
                }
                const size_t me = scan_member_name(line, ms, mask);
                if (me == ms) {
                    break;
                }
                const kimix::string_view line_v(line);
                const kimix::string_view mem = line_v.substr(ms, me - ms);
                kimix::string_view args;
                size_t me2 = 0;
                scan_method_args(line, me, mask, args, me2);
                kimix::string seg = ".";
                seg.append(mem.data(), mem.size());
                seg.append(args.data(), args.size());
                segments.push_back(Seg{seg, me2});
                prefixes.push_back(prefixes.back() + segments.back().text);
                cur = me2;
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
                full_expr = wrapped;
            }
            inner = "$(";
            inner += full_expr;
            inner += ")";
            end_pos = segments.back().end;
        } else {
            size_t bracket_depth = 1;
            size_t bracket_end = idx + 2;
            while (bracket_end < line.size() && bracket_depth > 0) {
                const char c = line[bracket_end];
                if (mask.is_code(static_cast<uint32_t>(bracket_end))) {
                    if (c == '[') {
                        bracket_depth += 1;
                    } else if (c == ']') {
                        bracket_depth -= 1;
                    }
                }
                ++bracket_end;
            }
            const kimix::string_view line_v2(line);
            const kimix::string_view index_expr =
                line_v2.substr(idx + 2, bracket_end - 1 - (idx + 2));
            inner = "$(if ($null -ne ";
            inner += base;
            inner += ") { ";
            inner += base;
            inner += "[";
            inner.append(index_expr.data(), index_expr.size());
            inner += "] })";
            end_pos = bracket_end;
        }
        kimix::string new_line = build_replacement(line.substr(0, expr_start), inner);
        new_line.append(line.data() + end_pos, line.size() - end_pos);
        line = new_line;
        search = 0;
    }
}

// -- whole-code transform ---------------------------------------------------

void find_multiline_regions(kimix::string_view code, const RegionMask& mask,
                            kimix::vector<uint8_t>& multi) {
    const size_t n = code.size();
    size_t i = 0;
    size_t line_idx = 0;
    while (i < n) {
        if (!mask.is_code(static_cast<uint32_t>(i))) {
            const size_t start_line = line_idx;
            while (i < n && !mask.is_code(static_cast<uint32_t>(i))) {
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

void scan_pwsh_transform(kimix::string_view cmd, kimix::string* transformed) {
    if (transformed == nullptr) {
        return;
    }
    const kimix::string joined = join_continuation_lines(cmd);
    // split on '\n' (Python split semantics: trailing empty segment kept)
    kimix::vector<kimix::string> lines;
    {
        size_t start = 0;
        for (size_t i = 0; i <= joined.size(); ++i) {
            if (i == joined.size() || joined[i] == '\n') {
                lines.push_back(joined.substr(start, i - start));
                start = i + 1;
            }
        }
    }
    RegionMask mask(static_cast<uint32_t>(joined.size()));
    build_region_mask(joined, true, mask);
    kimix::vector<uint8_t> multi;
    find_multiline_regions(joined, mask, multi);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i < multi.size() && multi[i]) {
            continue;
        }
        transform_nca_line(lines[i]);
        transform_null_conditional_line(lines[i], kimix::string_view("?.", 2));
        transform_null_conditional_line(lines[i], kimix::string_view("?[", 2));
        transform_nc_line(lines[i]);
        transform_ternary_line(lines[i]);
        transform_chain_line(lines[i]);
    }
    kimix::string result;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            result.push_back('\n');
        }
        result.append(lines[i]);
    }
    *transformed = result;
}

} // namespace

// ===========================================================================
// Public entry point
// ===========================================================================

void scan_shell(shell_dialect dialect, kimix::string_view cmd,
                kimix::vector<edit>& edits, kimix::string* transformed,
                kimix::vector<kimix::string>* names,
                kimix::vector<kimix::string>* notes, int* warning_code) {
    switch (dialect) {
    case shell_dialect::BASH_FIX:
        scan_bash_fix(cmd, edits, transformed, names, notes);
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
        scan_pwsh_transform(cmd, transformed);
        break;
    }
}

// -- RegionMask -------------------------------------------------------------

RegionMask::RegionMask(uint32_t n) : size_(n), bits_((n + 63) / 64, ~uint64_t{0}) {}

void RegionMask::mark(uint32_t start, uint32_t end) {
    if (start > end) {
        return;
    }
    if (end > size_) {
        end = size_;
    }
    for (uint32_t i = start; i < end; ++i) {
        bits_[i >> 6] &= ~(uint64_t{1} << (i & 63));
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
