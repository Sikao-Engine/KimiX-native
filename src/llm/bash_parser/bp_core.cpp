/* bp_core.cpp — boundary oracles, word decoders, constant tables.
 * Line-for-line ports of kimi-agent shell_compat/bash_core.py module-level
 * helpers and frozenset tables (see bp_core.h for the contracts).
 */
#include "llm/bash_parser/bp_core.h"

#include <cstdlib>

namespace kimix::llm::bash_parser {

namespace src_llm_bash_parser_bp_core {

// Membership helper over a static sorted-or-unsorted C array.
template <size_t N>
inline bool in_table(const char *const (&table)[N], kimix::string_view w) {
    for (size_t i = 0; i < N; ++i) {
        if (w == table[i]) {
            return true;
        }
    }
    return false;
}


inline char at(kimix::string_view s, size_t i) {
    return i < s.size() ? s[i] : '\0';
}

inline bool is_py_digit(char ch) { return ch >= '0' && ch <= '9'; }

} // namespace src_llm_bash_parser_bp_core
using namespace src_llm_bash_parser_bp_core;

// ── boundary oracles ─────────────────────────────────────────────────────────

int64_t find_ansi_c_end(kimix::string_view cmd, int64_t start) {
    int64_t i = start;
    const int64_t length = static_cast<int64_t>(cmd.size());
    while (i < length) {
        const char c = cmd[static_cast<size_t>(i)];
        if (c == '\\' && i + 1 < length) {
            i += 2;
        } else if (c == '\'') {
            return i + 1;
        } else {
            i += 1;
        }
    }
    return -1;
}

int64_t find_backtick_end(kimix::string_view cmd, int64_t start) {
    int64_t i = start;
    const int64_t length = static_cast<int64_t>(cmd.size());
    while (i < length) {
        const char c = cmd[static_cast<size_t>(i)];
        if (c == '\\' && i + 1 < length) {
            i += 2; // skip escaped char (including \`)
        } else if (c == '`') {
            return i + 1;
        } else {
            i += 1;
        }
    }
    return -1;
}

int64_t find_dq_end(kimix::string_view cmd, int64_t start) {
    int64_t i = start;
    const int64_t length = static_cast<int64_t>(cmd.size());
    while (i < length) {
        const char c = cmd[static_cast<size_t>(i)];
        if (c == '\\' && i + 1 < length && in_dq_escaped(cmd[static_cast<size_t>(i + 1)])) {
            i += 2; // skip \X (X is escaped: ", \, $, `)
        } else if (c == '"') {
            return i + 1;
        } else if (c == '$' && i + 1 < length && cmd[static_cast<size_t>(i + 1)] == '(') {
            const int64_t end = find_matching_paren(cmd, i + 1);
            if (end == -1) {
                return -1;
            }
            i = end + 1;
        } else if (c == '$' && i + 1 < length && cmd[static_cast<size_t>(i + 1)] == '\'') {
            // Inside "..." bash does NOT start an ANSI-C region at $': both
            // characters are ordinary literals here (documented legacy
            // behaviour).
            i += 1;
        } else if (c == '`') {
            const int64_t end = find_backtick_end(cmd, i + 1);
            if (end == -1) {
                return -1;
            }
            i = end;
        } else {
            i += 1;
        }
    }
    return -1;
}

int64_t find_matching_paren(kimix::string_view cmd, int64_t open_pos) {
    if (open_pos < 0 || static_cast<size_t>(open_pos) >= cmd.size() || cmd[static_cast<size_t>(open_pos)] != '(') {
        return -1; // the reference asserts cmd[open_pos] == "("; decline defensively
    }
    int64_t depth = 1;
    int64_t i = open_pos + 1;
    const int64_t length = static_cast<int64_t>(cmd.size());
    while (i < length) {
        const char c = cmd[static_cast<size_t>(i)];
        if (c == '\'') {
            const size_t found = cmd.find('\'', static_cast<size_t>(i + 1));
            if (found == kimix::string_view::npos) {
                return -1;
            }
            i = static_cast<int64_t>(found) + 1;
        } else if (c == '"') {
            i = find_dq_end(cmd, i + 1);
            if (i == -1) {
                return -1;
            }
        } else if (c == '`') {
            i = find_backtick_end(cmd, i + 1);
            if (i == -1) {
                return -1;
            }
        } else if (c == '$' && i + 1 < length && cmd[static_cast<size_t>(i + 1)] == '(') {
            depth += 1;
            i += 2;
        } else if (c == '$' && i + 1 < length && cmd[static_cast<size_t>(i + 1)] == '\'') {
            const int64_t end = find_ansi_c_end(cmd, i + 2);
            if (end == -1) {
                return -1;
            }
            i = end;
        } else if (c == '(') {
            // A plain parenthesis opens a subshell group: it must be balanced
            // by its own ) before the substitution can close.
            depth += 1;
            i += 1;
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
    return -1;
}

kimix::optional<size_t> match_ansi_c_quote_span(kimix::string_view text, size_t i) {
    const size_t n = text.size();
    while (i < n) {
        const char ch = text[i];
        if (ch == '\\') {
            i += 2;
        } else if (ch == '\'') {
            return i + 1;
        } else {
            i += 1;
        }
    }
    return std::nullopt;
}

kimix::optional<size_t> match_dquote_span(kimix::string_view text, size_t i) {
    const size_t n = text.size();
    while (i < n) {
        const char ch = text[i];
        if (ch == '\\') {
            i += 2;
        } else if (ch == '"') {
            return i + 1;
        } else {
            i += 1;
        }
    }
    return std::nullopt;
}

kimix::optional<size_t> match_parameter_braces(kimix::string_view text, size_t i) {
    int32_t depth = 1;
    const size_t n = text.size();
    while (i < n) {
        const char ch = text[i];
        if (ch == '\\') {
            i += 2;
        } else if (ch == '\'') {
            const size_t j = text.find('\'', i + 1);
            if (j == kimix::string_view::npos) {
                return std::nullopt;
            }
            i = j + 1;
        } else if (ch == '"') {
            const auto j = match_dquote_span(text, i + 1);
            if (!j.has_value()) {
                return std::nullopt;
            }
            i = *j;
        } else if (ch == '`' || (ch == '$' && i + 1 < n && text[i + 1] == '(')) {
            return std::nullopt;
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
    return std::nullopt;
}

kimix::optional<size_t> match_arithmetic_span(kimix::string_view text, size_t i) {
    int32_t depth = 1;
    const size_t n = text.size();
    while (i < n) {
        const char ch = text[i];
        if (ch == '$' && i + 1 < n && text[i + 1] == '(') {
            return std::nullopt;
        }
        if (ch == '(' && bp_starts_with(text.substr(i), "((")) {
            depth += 1;
            i += 2;
        } else if (ch == ')' && bp_starts_with(text.substr(i), "))")) {
            depth -= 1;
            i += 2;
            if (depth == 0) {
                return i;
            }
        } else if (ch == '\\') {
            i += 2;
        } else if (ch == '\'') {
            const size_t j = text.find('\'', i + 1);
            if (j == kimix::string_view::npos) {
                return std::nullopt;
            }
            i = j + 1;
        } else if (ch == '"') {
            const auto j = match_dquote_span(text, i + 1);
            if (!j.has_value()) {
                return std::nullopt;
            }
            i = *j;
        } else {
            i += 1;
        }
    }
    return std::nullopt;
}

// ── word decoding ────────────────────────────────────────────────────────────

namespace src_llm_bash_parser_bp_core {

// `_LITERAL_SPECIAL_RE` = [\\'"$`*?[{~] — position of the next special char
// at or after `i`, or npos.
size_t find_literal_special(kimix::string_view raw, size_t i) {
    for (size_t j = i; j < raw.size(); ++j) {
        const char ch = raw[j];
        if (ch == '\\' || ch == '\'' || ch == '"' || ch == '$' || ch == '`' || ch == '*' || ch == '?' || ch == '[' || ch == '{' || ch == '~') {
            return j;
        }
    }
    return kimix::string_view::npos;
}

} // namespace src_llm_bash_parser_bp_core
using namespace src_llm_bash_parser_bp_core;

kimix::optional<AnsiCLiteralSpan> ansi_c_literal_span(kimix::string_view raw, size_t i) {
    if (!bp_starts_with(raw.substr(i), "$'")) {
        return std::nullopt;
    }
    size_t j = i + 2;
    kimix::string out;
    // Bash strings are C strings: the VALUE truncates at the first NUL.  The
    // scan still runs to the closing quote so the returned index is correct.
    bool nul_seen = false;
    while (j < raw.size()) {
        const char ch = raw[j];
        if (ch == '\'') {
            return AnsiCLiteralSpan{out, j + 1};
        }
        if (ch != '\\') {
            if (!nul_seen) {
                out.push_back(ch);
            }
            j += 1;
            continue;
        }
        j += 1;
        if (j >= raw.size()) {
            return std::nullopt;
        }
        const char esc = raw[j];
        char simple = 0;
        bool has_simple = true;
        switch (esc) {
        case 'a': simple = '\a'; break;
        case 'b': simple = '\b'; break;
        case 'e':
        case 'E': simple = '\x1b'; break;
        case 'f': simple = '\f'; break;
        case 'n': simple = '\n'; break;
        case 'r': simple = '\r'; break;
        case 't': simple = '\t'; break;
        case 'v': simple = '\v'; break;
        case '\\': simple = '\\'; break;
        case '\'': simple = '\''; break;
        case '"': simple = '"'; break;
        case '?': simple = '?'; break;
        default: has_simple = false; break;
        }
        if (has_simple) {
            if (!nul_seen) {
                out.push_back(simple);
            }
            j += 1;
            continue;
        }
        if (esc == 'c') {
            if (j + 1 >= raw.size()) {
                return std::nullopt;
            }
            const char ctrl = raw[j + 1];
            char upper = ctrl;
            if (upper >= 'a' && upper <= 'z') {
                upper = static_cast<char>(upper - 'a' + 'A');
            }
            const int32_t code = static_cast<int32_t>(static_cast<unsigned char>(upper)) & 0x1F;
            if (code == 0) {
                nul_seen = true;
            } else if (!nul_seen) {
                out.push_back(static_cast<char>(code));
            }
            j += 2;
            continue;
        }
        if (esc >= '0' && esc <= '7') {
            kimix::string digits;
            size_t k = j;
            while (k < raw.size() && digits.size() < 3 && raw[k] >= '0' && raw[k] <= '7') {
                digits.push_back(raw[k]);
                k += 1;
            }
            int32_t code = 0;
            for (const char d : digits) {
                code = code * 8 + (d - '0');
            }
            if (code > 0x7F) {
                return std::nullopt; // multibyte octal: unsupported, be safe
            }
            if (code == 0) {
                nul_seen = true;
            } else if (!nul_seen) {
                out.push_back(static_cast<char>(code));
            }
            j = k;
            continue;
        }
        if (esc == 'x') {
            kimix::string digits;
            size_t k = j + 1;
            while (k < raw.size() && digits.size() < 2) {
                const char d = raw[k];
                const bool hex = (d >= '0' && d <= '9') || (d >= 'a' && d <= 'f') || (d >= 'A' && d <= 'F');
                if (!hex) {
                    break;
                }
                digits.push_back(d);
                k += 1;
            }
            if (digits.empty()) {
                return std::nullopt;
            }
            int32_t code = 0;
            for (const char d : digits) {
                int32_t v = 0;
                if (d >= '0' && d <= '9') {
                    v = d - '0';
                } else if (d >= 'a' && d <= 'f') {
                    v = d - 'a' + 10;
                } else {
                    v = d - 'A' + 10;
                }
                code = code * 16 + v;
            }
            if (code > 0x7F) {
                return std::nullopt;
            }
            if (code == 0) {
                nul_seen = true;
            } else if (!nul_seen) {
                out.push_back(static_cast<char>(code));
            }
            j = k;
            continue;
        }
        if (esc == 'u' || esc == 'U') {
            return std::nullopt; // unicode escapes: not worth approximating
        }
        return std::nullopt; // unknown escape: stay conservative
    }
    return std::nullopt; // unterminated
}

kimix::optional<kimix::string> literal_word_value(kimix::string_view raw) {
    // Fast path: no quoting/expansion characters — quote removal is identity.
    if (find_literal_special(raw, 0) == kimix::string_view::npos) {
        return kimix::string(raw);
    }
    kimix::string value;
    size_t i = 0;
    const size_t n = raw.size();
    while (i < n) {
        const size_t m = find_literal_special(raw, i);
        if (m == kimix::string_view::npos) {
            value.append(raw.substr(i));
            break;
        }
        if (m > i) {
            value.append(raw.substr(i, m - i));
            i = m;
        }
        const char ch = raw[i];
        if (ch == '\\') {
            if (i + 1 >= n) {
                return std::nullopt; // trailing lone backslash
            }
            if (raw[i + 1] == '\n') { // line continuation joins the value
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
                return std::nullopt; // unterminated single quote
            }
            value.append(raw.substr(i + 1, close - i - 1));
            i = close + 1;
            continue;
        }
        if (ch == '"') {
            i += 1;
            while (i < n && raw[i] != '"') {
                const char inner = raw[i];
                if (inner == '$' || inner == '`') {
                    return std::nullopt; // runtime expansion
                }
                if (inner == '\\' && i + 1 < n) {
                    const char escaped = raw[i + 1];
                    if (escaped == '$' || escaped == '`' || escaped == '"' || escaped == '\\' || escaped == '\n') {
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
            if (i >= n) {
                return std::nullopt; // unterminated double quote
            }
            i += 1;
            continue;
        }
        if (ch == '$' && bp_starts_with(raw.substr(i), "$'")) {
            const auto literal = ansi_c_literal_span(raw, i);
            if (!literal.has_value()) {
                return std::nullopt;
            }
            value.append(literal->value);
            i = literal->index_after;
            continue;
        }
        if (ch == '$' && bp_starts_with(raw.substr(i), "$\"")) {
            // $"..." locale translation: on Git Bash the value is the plain
            // double-quoted body.
            size_t close = i + 2;
            while (close < n) {
                if (raw[close] == '\\' && close + 1 < n) {
                    close += 2;
                    continue;
                }
                if (raw[close] == '"') {
                    break;
                }
                close += 1;
            }
            if (close >= n) {
                return std::nullopt;
            }
            const auto inner_literal = literal_word_value(raw.substr(i + 1, close + 1 - (i + 1)));
            if (!inner_literal.has_value()) {
                return std::nullopt;
            }
            value.append(*inner_literal);
            i = close + 1;
            continue;
        }
        if (ch == '$' || ch == '`' || ch == '*' || ch == '?' || ch == '[' || ch == '{' || ch == '~') {
            return std::nullopt;
        }
        value.push_back(ch);
        i += 1;
    }
    return value;
}

// ── predicate ports ──────────────────────────────────────────────────────────

bool is_assignment_word(kimix::string_view raw) {
    // ^[A-Za-z_][A-Za-z0-9_]*(\+)?=
    size_t i = 0;
    if (raw.empty() || !(is_path_alpha(raw[0]) || raw[0] == '_')) {
        return false;
    }
    i = 1;
    while (i < raw.size() && (is_path_alnum(raw[i]) || raw[i] == '_')) {
        i += 1;
    }
    if (i < raw.size() && raw[i] == '+') {
        i += 1;
    }
    return i < raw.size() && raw[i] == '=';
}

bool is_name_word(kimix::string_view raw) {
    if (raw.empty() || !(is_path_alpha(raw[0]) || raw[0] == '_')) {
        return false;
    }
    for (size_t i = 1; i < raw.size(); ++i) {
        if (!(is_path_alnum(raw[i]) || raw[i] == '_')) {
            return false;
        }
    }
    return true;
}

bool is_glued_label(kimix::string_view raw) {
    // ^:[A-Za-z_][A-Za-z0-9_]*$
    if (raw.size() < 2 || raw[0] != ':' || !(is_path_alpha(raw[1]) || raw[1] == '_')) {
        return false;
    }
    for (size_t i = 2; i < raw.size(); ++i) {
        if (!(is_path_alnum(raw[i]) || raw[i] == '_')) {
            return false;
        }
    }
    return true;
}

bool is_call_batch_target(kimix::string_view raw) {
    // \.(bat|cmd)$ case-insensitive
    auto tail_ci = [&](kimix::string_view tail) {
        if (raw.size() < tail.size()) {
            return false;
        }
        const size_t off = raw.size() - tail.size();
        for (size_t i = 0; i < tail.size(); ++i) {
            char c = raw[off + i];
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
            if (c != tail[i]) {
                return false;
            }
        }
        return true;
    };
    return tail_ci(".bat") || tail_ci(".cmd");
}

kimix::optional<kimix::string> for_loop_token_name(kimix::string_view raw) {
    // ^(%%?)([A-Za-z_][A-Za-z0-9_]*)$
    size_t i = 0;
    if (raw.empty() || raw[0] != '%') {
        return std::nullopt;
    }
    i = 1;
    if (i < raw.size() && raw[i] == '%') {
        i += 1;
    }
    if (i >= raw.size() || !(is_path_alpha(raw[i]) || raw[i] == '_')) {
        return std::nullopt;
    }
    kimix::string name(1, raw[i]);
    i += 1;
    while (i < raw.size()) {
        if (!(is_path_alnum(raw[i]) || raw[i] == '_')) {
            return std::nullopt;
        }
        name.push_back(raw[i]);
        i += 1;
    }
    return name;
}

bool is_shell_c_option(kimix::string_view word) {
    return word == "-c" || word == "-lc" || word == "-cl";
}

bool is_shell_c_script_option(kimix::string_view word) {
    // ^-[A-Za-z]*c$
    if (word.size() < 2 || word[0] != '-' || word.back() != 'c') {
        return false;
    }
    for (size_t i = 1; i + 1 < word.size(); ++i) {
        if (!is_path_alpha(word[i])) {
            return false;
        }
    }
    return true;
}

bool is_errorlevel_operand(kimix::string_view word) {
    // ^[0-9]{1,9}$
    if (word.empty() || word.size() > 9) {
        return false;
    }
    for (const char ch : word) {
        if (!is_py_digit(ch)) {
            return false;
        }
    }
    return true;
}

bool is_int_operand(kimix::string_view word) {
    // ^-?[0-9]+$
    size_t i = 0;
    if (!word.empty() && word[0] == '-') {
        i = 1;
    }
    if (i >= word.size()) {
        return false;
    }
    for (; i < word.size(); ++i) {
        if (!is_py_digit(word[i])) {
            return false;
        }
    }
    return true;
}

// ── constant tables ──────────────────────────────────────────────────────────

namespace src_llm_bash_parser_bp_core {

constexpr const char *kCommandStartKeywords[] = {"!", "{", "if", "then", "elif", "else", "while", "until", "do"};
constexpr const char *kCommandEndKeywords[] = {"fi", "done", "esac"};
constexpr const char *kListKeywords[] = {"for", "select", "case"};
constexpr const char *kShellKeywords[] = {"!", "{", "}", "if", "then", "elif", "else", "while", "until", "do", "fi", "done", "esac", "for", "select", "case", "time", "coproc", "in", "function"};
constexpr const char *kCommandWrappers[] = {"builtin", "command", "coproc", "env", "exec", "nohup", "sudo", "time", "timeout", "stdbuf", "nice", "xargs"};
constexpr const char *kSameShellWrappers[] = {"builtin", "coproc", "time", "watch"};
constexpr const char *kShellWrappers[] = {"bash", "sh", "dash", "ash"};
constexpr const char *kExecOperandCommands[] = {"find", "xargs", "eval", "ssh", "watch"};
constexpr const char *kPatternProgramCommands[] = {"grep", "fgrep", "egrep", "rg", "sed", "awk", "gawk", "mawk"};
constexpr const char *kGrepPatternFamily[] = {"grep", "fgrep", "egrep", "rg"};
constexpr const char *kGrepValueOptions[] = {"-A", "-B", "-C", "-m", "-d", "-D", "--after-context", "--before-context", "--context", "--max-count", "--directories", "--devices"};
constexpr const char *kShellValueOptions[] = {"-o", "+o", "-O", "+O", "--rcfile", "--init-file"};

struct WrapperOptionsEntry {
    const char *kind;
    const kimix::vector<kimix::string> *options;
};

// _WRAPPER_OPTIONS_WITH_VALUE / _WRAPPER_PATH_OPTIONS as static tables
// (function-local statics: no dynamic-init-order dependency across TUs).
const kimix::vector<std::pair<kimix::string, kimix::vector<kimix::string>>> &wrapper_options_with_value() {
    static const kimix::vector<std::pair<kimix::string, kimix::vector<kimix::string>>> table = {
        {"env", {"-u", "--unset", "-C", "--chdir", "-S", "--split-string"}},
        {"exec", {"-a"}},
        {"sudo", {"-C", "--close-from", "-D", "--chdir", "-g", "--group", "-h", "--host", "-p", "--prompt", "-R", "--chroot", "-r", "--role", "-t", "--type", "-T", "--command-timeout", "-u", "--user"}},
        {"time", {"-f", "--format", "-o", "--output"}},
        {"timeout", {"-k", "--kill-after", "-s", "--signal"}},
        {"stdbuf", {"-o", "-e", "-i", "--output", "--error", "--input"}},
        {"nice", {"-n", "--adjustment"}},
        {"xargs", {"-I", "-n", "-L", "-P", "-s", "-S", "-a", "-d", "-E", "--arg-file", "--max-args", "--max-chars", "--max-procs", "--max-lines", "--replace", "--eof", "--delimiter"}},
        {"watch", {"-n", "--interval"}},
    };
    return table;
}

const kimix::vector<std::pair<kimix::string, kimix::vector<kimix::string>>> &wrapper_path_options() {
    static const kimix::vector<std::pair<kimix::string, kimix::vector<kimix::string>>> table = {
        {"env", {"-C", "--chdir"}},
        {"sudo", {"-D", "--chdir"}},
        {"time", {"-o", "--output"}},
        {"xargs", {"-a", "--arg-file"}},
    };
    return table;
}

constexpr const char *kWrapperPathOptionLong[] = {"--chdir", "--output", "--arg-file"};

// _PATH_SAFE_CHARS:
// "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_./:~@%+=-#,[]*?{}"
constexpr const char *kPathSafeChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_./:~@%+=-#,[]*?{}";
// _ESCAPED_LITERAL_CHARS = " \t&;|()<>'\"$`{}!"
constexpr const char *kEscapedLiteralChars = " \t&;|()<>'\"$`{}!";
// _BASH_METACHARACTERS = "()|;&<>$\"`'\"*?[]{}~!#=% \t\n\r"
constexpr const char *kBashMetacharacters = "()|;&<>$\"`'*?[]{}~!#=% \t\n\r";

} // namespace src_llm_bash_parser_bp_core
using namespace src_llm_bash_parser_bp_core;

bool in_command_start_keywords(kimix::string_view w) { return in_table(kCommandStartKeywords, w); }
bool in_command_end_keywords(kimix::string_view w) { return in_table(kCommandEndKeywords, w); }
bool in_list_keywords(kimix::string_view w) { return in_table(kListKeywords, w); }
bool in_shell_keywords(kimix::string_view w) { return in_table(kShellKeywords, w); }
bool in_command_wrappers(kimix::string_view w) { return in_table(kCommandWrappers, w); }
bool in_same_shell_wrappers(kimix::string_view w) { return in_table(kSameShellWrappers, w); }
bool in_shell_wrappers(kimix::string_view w) { return in_table(kShellWrappers, w); }
bool in_exec_operand_commands(kimix::string_view w) { return in_table(kExecOperandCommands, w); }
bool in_pattern_program_commands(kimix::string_view w) { return in_table(kPatternProgramCommands, w); }
bool in_grep_pattern_family(kimix::string_view w) { return in_table(kGrepPatternFamily, w); }
bool in_grep_value_options(kimix::string_view w) { return in_table(kGrepValueOptions, w); }
bool in_shell_value_options(kimix::string_view w) { return in_table(kShellValueOptions, w); }
bool wrapper_path_option_long(kimix::string_view w) { return in_table(kWrapperPathOptionLong, w); }

int32_t wrapper_operand_count(kimix::string_view kind) {
    return kind == "timeout" ? 1 : 0; // _WRAPPER_OPERAND_COUNTS = {"timeout": 1}
}

kimix::optional<int32_t> reparse_builtin_count(kimix::string_view word) {
    // _REPARSE_BUILTINS = {"eval": -1, "trap": 1}
    if (word == "eval") {
        return -1;
    }
    if (word == "trap") {
        return 1;
    }
    return std::nullopt;
}

bool wrapper_option_with_value(kimix::string_view kind, kimix::string_view word) {
    for (const auto &entry : wrapper_options_with_value()) {
        if (entry.first == kind) {
            for (const auto &option : entry.second) {
                if (option == word) {
                    return true;
                }
            }
            return false;
        }
    }
    return false;
}

bool wrapper_path_option(kimix::string_view kind, kimix::string_view word) {
    for (const auto &entry : wrapper_path_options()) {
        if (entry.first == kind) {
            for (const auto &option : entry.second) {
                if (option == word) {
                    return true;
                }
            }
            return false;
        }
    }
    return false;
}

bool in_path_safe_chars(char ch) {
    for (const char *p = kPathSafeChars; *p; ++p) {
        if (*p == ch) {
            return true;
        }
    }
    return false;
}

bool in_path_glob_chars(char ch) {
    return ch == '*' || ch == '?' || ch == '[' || ch == ']' || ch == '{' || ch == '}';
}

bool in_escaped_literal_chars(char ch) {
    for (const char *p = kEscapedLiteralChars; *p; ++p) {
        if (*p == ch) {
            return true;
        }
    }
    return false;
}

bool in_operator_chars(char ch) {
    return ch == ';' || ch == '&' || ch == '|' || ch == '(' || ch == ')' || ch == '<' || ch == '>' || ch == '\n';
}

bool in_word_end_chars(char ch) {
    return in_operator_chars(ch) || ch == ' ' || ch == '\t' || ch == '\r';
}

bool in_redirection_start(char ch) { return ch == '<' || ch == '>'; }

bool in_control_op_starts(char ch) {
    return ch == ';' || ch == '&' || ch == '|' || ch == '(' || ch == ')';
}

bool in_bash_metacharacters(char ch) {
    for (const char *p = kBashMetacharacters; *p; ++p) {
        if (*p == ch) {
            return true;
        }
    }
    return false;
}

bool in_dq_escaped(char ch) {
    return ch == '"' || ch == '\\' || ch == '$' || ch == '`';
}

bool fullmatch_path_drive(kimix::string_view raw) {
    // [A-Za-z]:[\\/].*
    if (raw.size() < 3 || !is_path_alpha(raw[0]) || raw[1] != ':') {
        return false;
    }
    return raw[2] == '\\' || raw[2] == '/';
}

bool fullmatch_path_segment(kimix::string_view value) {
    // [A-Za-z0-9_.~\\-]+
    if (value.empty()) {
        return false;
    }
    for (const char ch : value) {
        const bool ok = is_path_alnum(ch) || ch == '_' || ch == '.' || ch == '~' || ch == '\\' || ch == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// ── operator readers ─────────────────────────────────────────────────────────

size_t read_shell_control_operator(kimix::string_view s, size_t i, size_t n, kimix::string_view *op_out) {
    // bash_core._read_shell_control_operator (no parens).
    *op_out = {};
    if (i >= n) {
        return i;
    }
    const char ch = s[i];
    if (ch == ';') {
        if (bp_starts_with(s.substr(i, n - i), ";;&")) {
            *op_out = ";;&";
            return i + 3;
        }
        if (bp_starts_with(s.substr(i, n - i), ";;")) {
            *op_out = ";;";
            return i + 2;
        }
        if (bp_starts_with(s.substr(i, n - i), ";&")) {
            *op_out = ";&";
            return i + 2;
        }
        *op_out = ";";
        return i + 1;
    }
    if (ch == '&') {
        if (bp_starts_with(s.substr(i, n - i), "&&")) {
            *op_out = "&&";
            return i + 2;
        }
        *op_out = "&";
        return i + 1;
    }
    if (ch == '|') {
        if (bp_starts_with(s.substr(i, n - i), "||")) {
            *op_out = "||";
            return i + 2;
        }
        if (bp_starts_with(s.substr(i, n - i), "|&")) {
            *op_out = "|&";
            return i + 2;
        }
        *op_out = "|";
        return i + 1;
    }
    return i;
}

size_t read_control_operator(kimix::string_view s, size_t i, size_t n, kimix::string_view *op_out) {
    // The scanner's `_read_control_operator` / lexer.read_control_operator:
    // the shell table extended with the subshell parentheses.
    const size_t after = read_shell_control_operator(s, i, n, op_out);
    if (!op_out->empty() || i >= n) {
        return after;
    }
    const char ch = s[i];
    if (ch == '(' || ch == ')') {
        *op_out = kimix::string_view(s.data() + i, 1);
        return i + 1;
    }
    return i;
}

size_t read_redirection_operator(kimix::string_view s, size_t i, size_t n, kimix::string_view *op_out) {
    // _REDIRECTION_RE = &>>|&>|<<<|<<-|<<|>>|<>|>\||<&|>&|<|>  (longest first)
    *op_out = {};
    static const char *const kOperators[] = {"&>>", "&>", "<<<", "<<-", "<<", ">>", "<>", ">|", "<&", ">&", "<", ">"};
    const kimix::string_view rest = s.substr(i, n > i ? n - i : 0);
    for (const char *const op : kOperators) {
        if (bp_starts_with(rest, op)) {
            *op_out = op;
            return i + kimix::string_view(op).size();
        }
    }
    return i;
}

bool redirection_after_fd(kimix::string_view s, size_t i, size_t n) {
    while (i < n && is_py_digit(s[i])) {
        i += 1;
    }
    return i < n && in_redirection_start(s[i]);
}

bool comment_starts(kimix::string_view s, size_t i, size_t range_start) {
    if (i <= range_start) {
        return true;
    }
    const char prev = s[i - 1];
    return prev == ' ' || prev == '\t' || prev == '\r' || prev == '\n' || prev == ';' || prev == '&' || prev == '|' || prev == '(' || prev == ')' || prev == '<' || prev == '>';
}

kimix::string windows_temp_dir() {
    // tempfile.gettempdir() equivalent: the first of %TEMP%/%TMP% that names a
    // usable directory (else C:\Temp as Python's last candidate), backslashes
    // normalized to '/', trailing separator stripped, non-throwing, never
    // empty.  Exact directory-existence probing is best-effort; the value is
    // only used for the `/tmp` virtual-path rewrite.
    const char *candidates[] = {std::getenv("TEMP"), std::getenv("TMP"), nullptr, "C:\\Temp"};
    kimix::string dir;
    for (const char *env : candidates) {
        if (env != nullptr && *env != '\0') {
            dir = env;
            break;
        }
    }
    for (char &ch : dir) {
        if (ch == '\\') {
            ch = '/';
        }
    }
    // Python's gettempdir returns the directory without a trailing separator.
    while (dir.size() > 1 && dir.back() == '/') {
        dir.pop_back();
    }
    if (dir.empty()) {
        dir = "C:/Temp";
    }
    return dir;
}

kimix::string single_quote(kimix::string_view text) {
    // "'" + text.replace("'", "'\"'\"'") + "'"
    kimix::string out;
    out.reserve(text.size() + 2);
    out.push_back('\'');
    for (const char ch : text) {
        if (ch == '\'') {
            out.append("'\"'\"'");
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('\'');
    return out;
}

} // namespace kimix::llm::bash_parser
