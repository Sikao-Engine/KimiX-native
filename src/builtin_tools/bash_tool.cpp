// bash_tool.cpp - Pure string kernels of the kimi-agent bash tool.
//
// Port target: plans/bash.md 3.1 (exit-code semantics), 3.3 (output truncation
// with error preservation), 3.4 (RTK rewrite scanner), plus the bounded-run
// capture/timeout/kill policy state machine (AGENT_TASK.md scope). The
// function-by-function mapping to the Python reference lives in
// src/builtin_tools/reports/bash.md. Two non-obvious parity arguments are
// recorded here:
//
// 1. \b word boundaries. _ERROR_PATTERN is `\b(?:kw1|kw2|...)\b` with
//    re.IGNORECASE over str lines. Python \b is the ASCII boundary between
//    \w = [A-Za-z0-9_] and non-\w when the text is ASCII, so the native
//    kernel replicates it with is_word_char ASCII tests on both sides of the
//    keyword run. The keyword table is ASCII-only and the shim routes
//    non-ASCII output to the Python mirror, so this is byte-exact.
//
// 2. splitlines() terminators. Python str.splitlines() accepts more
//    terminators than LF/CRLF/CR (e.g. \x0b, \x0c, \x85, U+2028), but the bash
//    output pipeline always runs filter_output first, which normalizes every
//    line ending to LF (CRLF/CR -> LF). The kernel therefore splits on LF,
//    CRLF and CR only - exactly the documented contract of _truncate_lines in
//    the plan's risk notes (8).
//
// Compiled into the kimix-llm static library with a unity (jumbo) batch, so
// everything lives inside kimix::builtin_tools::bash and internal helpers have
// internal linkage with bash-specific names.

#include "builtin_tools/bash_tool.h"

#include "builtin_tools/pwsh_tool.h"
#include "builtin_tools/python_tool.h"
#include "builtin_tools/utf8_util.h"

#include <algorithm>
#include <utility>

namespace kimix::builtin_tools::bash {

namespace {

// -- character predicates -----------------------------------------------------

bool bash_is_ascii(kimix::string_view s) noexcept {
    for (const char c : s) {
        if (static_cast<uint8_t>(c) >= 0x80u) {
            return false;
        }
    }
    return true;
}

char bash_lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

// Python str.isspace() on the ASCII subset: space, \t, \n, \r, \f, \v.
bool bash_is_space(char c) noexcept {
    switch (c) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case '\f':
    case '\v':
        return true;
    default:
        return false;
    }
}

bool bash_is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

bool bash_is_alpha(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Python \w for ASCII text: [A-Za-z0-9_].
bool bash_is_word_char(char c) noexcept {
    return bash_is_alpha(c) || bash_is_digit(c) || c == '_';
}

kimix::string_view bash_strip_view(kimix::string_view s) noexcept {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && bash_is_space(s[b])) {
        ++b;
    }
    while (e > b && bash_is_space(s[e - 1])) {
        --e;
    }
    return s.substr(b, e - b);
}

bool bash_starts_with(kimix::string_view s, kimix::string_view prefix) noexcept {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool bash_iequals(kimix::string_view a, kimix::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (bash_lower_ascii(a[i]) != bash_lower_ascii(b[i])) {
            return false;
        }
    }
    return true;
}

// Case-insensitive ASCII substring search (haystack folded on the fly).
// Returns npos when not found.
size_t bash_find_ci(kimix::string_view haystack, kimix::string_view needle) noexcept {
    if (needle.empty()) {
        return 0;
    }
    if (needle.size() > haystack.size()) {
        return kimix::string_view::npos;
    }
    const size_t last = haystack.size() - needle.size();
    for (size_t i = 0; i <= last; ++i) {
        size_t j = 0;
        while (j < needle.size() &&
               bash_lower_ascii(haystack[i + j]) == needle[j]) {
            ++j;
        }
        if (j == needle.size()) {
            return i;
        }
    }
    return kimix::string_view::npos;
}

// ---------------------------------------------------------------------------
// splitlines()-style line splitting on LF / CRLF / CR (see header comment).
// Python: "a\r\nb" -> ["a", "b"]; "a\r\n" -> ["a"] (no trailing empty).
// ---------------------------------------------------------------------------
struct bash_lines {
    kimix::vector<kimix::string_view> views;
};

bash_lines bash_split_lines(kimix::string_view output) {
    // Python str.splitlines() on the LF/CRLF/CR subset: each terminator ends
    // the current line; a trailing terminator does NOT add an empty line.
    bash_lines out;
    size_t i = 0;
    const size_t n = output.size();
    size_t start = 0;
    while (i < n) {
        const char c = output[i];
        if (c == '\r') {
            out.views.push_back(output.substr(start, i - start));
            if (i + 1 < n && output[i + 1] == '\n') {
                ++i; // CRLF counts as one terminator
            }
            ++i;
            start = i;
        } else if (c == '\n') {
            out.views.push_back(output.substr(start, i - start));
            ++i;
            start = i;
        } else {
            ++i;
        }
    }
    if (start < n) {
        out.views.push_back(output.substr(start, n - start));
    }
    return out;
}

} // namespace

// ===========================================================================
// Exit-code semantics (plans/bash.md 3.1)
// ===========================================================================

bool has_top_level_pipe(kimix::string_view command) {
    // output_enhance.py _has_top_level_pipe (57-99): char walk tracking the
    // open quote (', ", `), backslash escapes, and the paren depth; a single
    // `|` at depth 0 that is not part of `||` returns true.
    if (command.empty()) {
        return false;
    }
    char quote = 0; // 0 == no open quote; else the opening character
    bool escaped = false;
    int64_t depth = 0;
    const size_t n = command.size();
    for (size_t i = 0; i < n; ++i) {
        const char ch = command[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (quote != 0) {
            if (ch == quote) {
                quote = 0;
            }
            continue;
        }
        if (ch == '\'' || ch == '"' || ch == '`') {
            quote = ch;
            continue;
        }
        if (ch == '(') {
            ++depth;
            continue;
        }
        if (ch == ')') {
            depth = depth > 0 ? depth - 1 : 0; // max(0, depth - 1)
            continue;
        }
        if (ch != '|' || depth != 0) {
            continue;
        }
        // A `||` logical-OR operator is not a pipeline: skip both pipes.
        if (i + 1 < n && command[i + 1] == '|') {
            continue;
        }
        if (i > 0 && command[i - 1] == '|') {
            continue;
        }
        return true;
    }
    return false;
}

kimix::string base_command_name(kimix::string_view command) {
    // output_enhance.py _base_command_name (41-54): strip, take the text after
    // the LAST && / || / | / ; separator, then the first word that is not a
    // VAR=... assignment; strip the directory part and a trailing ".exe".
    const auto split_last = [](kimix::string_view s,
                               kimix::string_view sep) -> kimix::string_view {
        const size_t p = s.rfind(sep);
        if (p == kimix::string_view::npos) {
            return s;
        }
        return s.substr(p + sep.size());
    };
    kimix::string_view seg = bash_strip_view(command);
    seg = split_last(seg, "&&");
    seg = split_last(seg, "||");
    seg = split_last(seg, "|");
    seg = split_last(seg, ";");
    seg = bash_strip_view(seg);
    size_t i = 0;
    while (i < seg.size()) {
        while (i < seg.size() && bash_is_space(seg[i])) {
            ++i;
        }
        if (i >= seg.size()) {
            break;
        }
        const size_t j = i;
        while (i < seg.size() && !bash_is_space(seg[i])) {
            ++i;
        }
        const kimix::string_view word = seg.substr(j, i - j);
        if (word.find('=') != kimix::string_view::npos && word[0] != '-') {
            continue; // FOO=1 assignment word
        }
        kimix::string_view stem = word;
        const size_t slash = word.rfind('/');
        if (slash != kimix::string_view::npos) {
            stem = word.substr(slash + 1);
        }
        // stem[:-4] when stem.lower().endswith(".exe")
        if (stem.size() >= 4 && bash_iequals(stem.substr(stem.size() - 4), ".exe")) {
            stem = stem.substr(0, stem.size() - 4);
        }
        return kimix::string(stem);
    }
    return kimix::string();
}

kimix::optional<kimix::string> interpret_exit_code(kimix::string_view command,
                                                   kimix::optional<int64_t> exit_code) {
    // output_enhance.py interpret_exit_code (119-157). The SIGPIPE rule is
    // checked FIRST (the reference comment explains: the older compiled kernel
    // predates it, so the shim decides it before the fast path to stay
    // identical under every execution mode).
    if (!exit_code.has_value() || *exit_code == 0) {
        return std::nullopt;
    }
    const int64_t code = *exit_code;
    if (code == 141 && has_top_level_pipe(command)) {
        return kimix::string("SIGPIPE: an upstream pipeline stage was truncated "
                             "(expected when piping to head/tail)");
    }
    const kimix::string name_raw = base_command_name(command);
    kimix::string name;
    name.reserve(name_raw.size());
    for (const char c : name_raw) {
        name.push_back(bash_lower_ascii(c));
    }
    if (code == 1) {
        if (name == "grep" || name == "egrep" || name == "fgrep" ||
            name == "rg" || name == "ag" || name == "ack") {
            return kimix::string("No matches found (not an error)");
        }
        if (name == "diff" || name == "colordiff") {
            return kimix::string("Files differ (expected, not an error)");
        }
        if (name == "find") {
            return kimix::string(
                "Some directories were inaccessible (partial results may still be valid)");
        }
        if (name == "test" || name == "[") {
            return kimix::string("Condition evaluated to false (expected, not an error)");
        }
    }
    if (name == "curl") {
        switch (code) {
        case 6:
            return kimix::string("Could not resolve host (DNS failure)");
        case 7:
            return kimix::string("Failed to connect to host");
        case 22:
            return kimix::string("HTTP error (server returned an error status)");
        case 28:
            return kimix::string("Connection timed out");
        default:
            break;
        }
    }
    if (name == "git" && code == 1) {
        // The em dash is the exact byte sequence of the reference message.
        return kimix::string(
            "Non-zero exit (often normal \xE2\x80\x94 e.g. 'git diff' returns 1 when files differ)");
    }
    return std::nullopt;
}

bool is_expected_exit(kimix::string_view command, kimix::optional<int64_t> exit_code) {
    // output_enhance.py is_expected_exit (160-176) delegates to
    // _is_expected_exit_py (102-116) when no native kernel answers; the kernel
    // implements the Python body directly.
    if (!exit_code.has_value() || *exit_code == 0) {
        return false;
    }
    const int64_t code = *exit_code;
    if (code == 141 && has_top_level_pipe(command)) {
        return true;
    }
    if (code != 1) {
        return false;
    }
    const kimix::string name_raw = base_command_name(command);
    kimix::string name;
    name.reserve(name_raw.size());
    for (const char c : name_raw) {
        name.push_back(bash_lower_ascii(c));
    }
    return name == "grep" || name == "egrep" || name == "fgrep" ||
           name == "rg" || name == "ag" || name == "ack" ||
           name == "diff" || name == "colordiff" || name == "test" ||
           name == "[" || name == "find";
}

// ===========================================================================
// Output truncation with error preservation (plans/bash.md 3.3)
// ===========================================================================

const kimix::string_view error_keywords[] = {
    "error",
    "exception",
    "traceback",
    "failed",
    "failure",
    "fatal",
    "panic",
    "abort",
    "assertion",
    "undefined",
    "syntaxerror",
    "typeerror",
    "valueerror",
    "keyerror",
    "importerror",
    "modulenotfounderror",
    "attributeerror",
    "nameerror",
    "runtimeerror",
    "oserror",
    "ioerror",
    "zerodivisionerror",
    "indexerror",
    "memoryerror",
    "recursionerror",
    "unboundlocalerror",
    "referenceerror",
    "permission denied",
    "access denied",
    "not found",
    "cannot find",
    "does not exist",
    "no such file",
    "connection refused",
    "timeout",
    "unhandled",
};
static_assert(sizeof(error_keywords) / sizeof(error_keywords[0]) == 36,
              "keyword table must keep the reference order and count");

namespace {

// One keyword search with ASCII \b boundaries, mirroring
// re.search(r'\b(?:kw)\b', line, re.IGNORECASE). The keyword may contain a
// space ("permission denied"); the boundary tests apply to the first and last
// character of the run only.
bool bash_keyword_at_boundary(kimix::string_view line, kimix::string_view keyword) noexcept {
    size_t from = 0;
    for (;;) {
        if (from > line.size()) {
            return false;
        }
        const size_t rel = bash_find_ci(line.substr(from), keyword);
        if (rel == kimix::string_view::npos) {
            return false;
        }
        const size_t pos = from + rel;
        const bool left_ok = pos == 0 || !bash_is_word_char(line[pos - 1]);
        const size_t after = pos + keyword.size();
        const bool right_ok =
            after >= line.size() || !bash_is_word_char(line[after]);
        if (left_ok && right_ok) {
            return true;
        }
        from = pos + 1; // resume after this (failed) occurrence
    }
}

bool bash_line_has_error(kimix::string_view line) noexcept {
    for (const kimix::string_view kw : error_keywords) {
        if (bash_keyword_at_boundary(line, kw)) {
            return true;
        }
    }
    return false;
}

} // namespace

kimix::optional<int64_t> find_error_line_index(kimix::string_view output) {
    // common.py _find_error_line_index (353-358): first line (1-based)
    // containing any keyword with \b word boundaries, else None.
    const bash_lines lines = bash_split_lines(output);
    for (size_t i = 0; i < lines.views.size(); ++i) {
        if (bash_line_has_error(lines.views[i])) {
            return static_cast<int64_t>(i) + 1;
        }
    }
    return std::nullopt;
}

kimix::string truncate_lines(kimix::string_view output, int64_t max_lines,
                             bool preserve_errors, int64_t error_context_lines) {
    // common.py _truncate_lines (1100-1164), byte-exact.
    if (output.empty() || max_lines <= 0) {
        return kimix::string(output);
    }
    const bash_lines split = bash_split_lines(output);
    const auto &lines = split.views;
    const int64_t n = static_cast<int64_t>(lines.size());
    if (n <= max_lines) {
        return kimix::string(output);
    }
    const int64_t head_n = max_lines / 2;
    const int64_t tail_n = max_lines - head_n - 1; // -1 reserves the fold marker line
    const int64_t omitted = n - head_n - tail_n;

    const auto join_range = [&lines](int64_t lo, int64_t hi) {
        kimix::string out;
        for (int64_t i = lo; i < hi; ++i) {
            if (i > lo) {
                out.push_back('\n');
            }
            out.append(lines[static_cast<size_t>(i)].data(),
                       lines[static_cast<size_t>(i)].size());
        }
        return out;
    };

    const kimix::string head = join_range(0, head_n);
    const kimix::string tail = tail_n > 0 ? join_range(n - tail_n, n) : kimix::string();

    kimix::vector<kimix::string_view> preserved;
    if (preserve_errors) {
        const kimix::optional<int64_t> err_idx = find_error_line_index(output); // 1-based
        if (err_idx.has_value()) {
            const int64_t e = *err_idx - 1; // 0-based
            const int64_t omitted_lo = head_n;
            const int64_t omitted_hi = n - tail_n; // exclusive
            if (omitted_lo <= e && e < omitted_hi) {
                const int64_t lo = std::max(omitted_lo, e - error_context_lines);
                const int64_t hi = std::min(omitted_hi, e + error_context_lines + 1);
                for (int64_t i = lo; i < hi; ++i) {
                    preserved.push_back(lines[static_cast<size_t>(i)]);
                }
            }
        }
    }

    kimix::string fold;
    fold += "\n\n[... ";
    fold += kimix::format("{}", omitted);
    fold += " lines omitted";
    if (!preserved.empty()) {
        fold += " (";
        fold += kimix::format("{}", preserved.size());
        fold += " error-context line(s) preserved)";
    }
    fold += " ...]\n\n";

    if (!preserved.empty()) {
        kimix::string out;
        out.reserve(head.size() + tail.size() + fold.size() + 64);
        out += head;
        out.push_back('\n');
        for (size_t i = 0; i < preserved.size(); ++i) {
            if (i > 0) {
                out.push_back('\n');
            }
            out.append(preserved[i].data(), preserved[i].size());
        }
        out += fold;
        out += tail;
        return out;
    }
    if (!tail.empty()) {
        kimix::string out;
        out.reserve(head.size() + fold.size() + tail.size());
        out += head;
        out += fold;
        out += tail;
        return out;
    }
    kimix::string out;
    out.reserve(head.size() + fold.size());
    out += head;
    out += fold;
    return out;
}

// ===========================================================================
// RTK command rewrite scanner (plans/bash.md 3.4)
// ===========================================================================

namespace {

// common.py _find_ansi_c_end (1167-1179): index AFTER the closing ' of $'...'
// or -1. Backslash escapes a single following character.
int64_t bash_find_ansi_c_end(kimix::string_view cmd, int64_t start) {
    int64_t i = start;
    const int64_t length = static_cast<int64_t>(cmd.size());
    while (i < length) {
        const char c = cmd[static_cast<size_t>(i)];
        if (c == '\\' && i + 1 < length) {
            i += 2;
        } else if (c == '\'') {
            return i + 1;
        } else {
            ++i;
        }
    }
    return -1;
}

// common.py _find_backtick_end (1182-1194): index AFTER the closing backtick
// of `...` or -1.
int64_t bash_find_backtick_end(kimix::string_view cmd, int64_t start) {
    int64_t i = start;
    const int64_t length = static_cast<int64_t>(cmd.size());
    while (i < length) {
        const char c = cmd[static_cast<size_t>(i)];
        if (c == '\\' && i + 1 < length) {
            i += 2;
        } else if (c == '`') {
            return i + 1;
        } else {
            ++i;
        }
    }
    return -1;
}

int64_t bash_find_matching_paren(kimix::string_view cmd, int64_t open_pos);

// common.py _find_dq_end (1197-1224): index AFTER the closing " of a
// double-quoted region or -1. Honours \", \\, \$, \` escapes and nested
// $(...), $'...' and `...` regions.
int64_t bash_find_dq_end(kimix::string_view cmd, int64_t start) {
    int64_t i = start;
    const int64_t length = static_cast<int64_t>(cmd.size());
    while (i < length) {
        const char c = cmd[static_cast<size_t>(i)];
        if (c == '\\' && i + 1 < length) {
            const char nx = cmd[static_cast<size_t>(i + 1)];
            if (nx == '"' || nx == '\\' || nx == '$' || nx == '`') {
                i += 2;
                continue;
            }
        }
        if (c == '"') {
            return i + 1;
        }
        if (c == '$' && i + 1 < length && cmd[static_cast<size_t>(i + 1)] == '(') {
            const int64_t end = bash_find_matching_paren(cmd, i + 1);
            if (end == -1) {
                return -1;
            }
            i = end + 1;
            continue;
        }
        if (c == '$' && i + 1 < length && cmd[static_cast<size_t>(i + 1)] == '\'') {
            const int64_t end = bash_find_ansi_c_end(cmd, i + 2);
            if (end == -1) {
                return -1;
            }
            i = end;
            continue;
        }
        if (c == '`') {
            const int64_t end = bash_find_backtick_end(cmd, i + 1);
            if (end == -1) {
                return -1;
            }
            i = end;
            continue;
        }
        ++i;
    }
    return -1;
}

// common.py _find_matching_paren (1227-1265): index of the ')' matching the
// '(' at cmd[open_pos], skipping quoted regions; -1 when unbalanced.
int64_t bash_find_matching_paren(kimix::string_view cmd, int64_t open_pos) {
    int64_t depth = 1;
    int64_t i = open_pos + 1;
    const int64_t length = static_cast<int64_t>(cmd.size());
    while (i < length) {
        const char c = cmd[static_cast<size_t>(i)];
        if (c == '\'') {
            const size_t end = cmd.find('\'', static_cast<size_t>(i) + 1);
            if (end == kimix::string_view::npos) {
                return -1;
            }
            i = static_cast<int64_t>(end) + 1;
        } else if (c == '"') {
            const int64_t end = bash_find_dq_end(cmd, i + 1);
            if (end == -1) {
                return -1;
            }
            i = end;
        } else if (c == '`') {
            const int64_t end = bash_find_backtick_end(cmd, i + 1);
            if (end == -1) {
                return -1;
            }
            i = end;
        } else if (c == '$' && i + 1 < length && cmd[static_cast<size_t>(i + 1)] == '\'') {
            const int64_t end = bash_find_ansi_c_end(cmd, i + 2);
            if (end == -1) {
                return -1;
            }
            i = end;
        } else if (c == '$' && i + 1 < length && cmd[static_cast<size_t>(i + 1)] == '(') {
            ++depth;
            i += 2;
        } else if (c == ')') {
            --depth;
            if (depth == 0) {
                return i;
            }
            ++i;
        } else {
            ++i;
        }
    }
    return -1;
}

// _PREFIX_SKIP (common.py 1420): modifiers skipped before the real executable.
constexpr kimix::string_view bash_prefix_skip[] = {"sudo", "time", "nohup", "nice"};

// _ASSIGNMENT_RE (common.py 1422): ^[A-Za-z_][A-Za-z0-9_]*=
bool bash_is_shell_assignment(kimix::string_view word) noexcept {
    if (word.empty()) {
        return false;
    }
    const char c0 = word[0];
    if (!(bash_is_alpha(c0) || c0 == '_')) {
        return false;
    }
    size_t i = 1;
    while (i < word.size()) {
        const char c = word[i];
        if (c == '=') {
            return true;
        }
        if (!(bash_is_alpha(c) || bash_is_digit(c) || c == '_')) {
            return false;
        }
        ++i;
    }
    return false;
}

// common.py _read_shell_word (1350-1415): read the next shell word starting at
// or after `i`; returns (word, word_start, next_index) with word_start == -1
// for the (None, None, None) sentinel.
struct bash_shell_word {
    kimix::string word;
    int64_t start = -1;
    int64_t next = -1;
};

bash_shell_word bash_read_shell_word(kimix::string_view cmd, int64_t i) {
    const int64_t n = static_cast<int64_t>(cmd.size());
    while (i < n && bash_is_space(cmd[static_cast<size_t>(i)])) {
        ++i;
    }
    bash_shell_word out;
    if (i >= n) {
        return out; // (None, None, None)
    }
    out.start = i;
    while (i < n) {
        const char c = cmd[static_cast<size_t>(i)];
        if (bash_is_space(c) || c == '|') {
            break; // unquoted | ends the word (leftmost-command detection)
        }
        if (c == '\'') {
            const size_t end = cmd.find('\'', static_cast<size_t>(i) + 1);
            if (end == kimix::string_view::npos) {
                out.word.append(cmd.data() + i, static_cast<size_t>(n - i));
                i = n;
                break;
            }
            out.word.append(cmd.data() + i, end + 1 - static_cast<size_t>(i));
            i = static_cast<int64_t>(end) + 1;
        } else if (c == '"') {
            const int64_t end = bash_find_dq_end(cmd, i + 1);
            if (end == -1) {
                out.word.append(cmd.data() + i, static_cast<size_t>(n - i));
                i = n;
                break;
            }
            out.word.append(cmd.data() + i, static_cast<size_t>(end - i));
            i = end;
        } else if (c == '$' && i + 1 < n && cmd[static_cast<size_t>(i + 1)] == '\'') {
            const int64_t end = bash_find_ansi_c_end(cmd, i + 2);
            if (end == -1) {
                out.word.append(cmd.data() + i, static_cast<size_t>(n - i));
                i = n;
                break;
            }
            out.word.append(cmd.data() + i, static_cast<size_t>(end - i));
            i = end;
        } else if (c == '$' && i + 1 < n && cmd[static_cast<size_t>(i + 1)] == '(') {
            const int64_t end = bash_find_matching_paren(cmd, i + 1);
            if (end == -1) {
                out.word.append(cmd.data() + i, static_cast<size_t>(n - i));
                i = n;
                break;
            }
            out.word.append(cmd.data() + i, static_cast<size_t>(end + 1 - i));
            i = end + 1;
        } else if (c == '`') {
            const int64_t end = bash_find_backtick_end(cmd, i + 1);
            if (end == -1) {
                out.word.append(cmd.data() + i, static_cast<size_t>(n - i));
                i = n;
                break;
            }
            out.word.append(cmd.data() + i, static_cast<size_t>(end - i));
            i = end;
        } else {
            out.word.push_back(c);
            ++i;
        }
    }
    out.next = i;
    return out;
}

// Path(token.strip("\"'")).stem with a ".exe" strip, matching
// _rewrite_shell_segment's `name` computation:
//   token.strip("\"'") removes leading/trailing " and ' only;
//   Path(...).stem drops the directory (last '/' or '\\') and the last
//   extension (only when one exists, e.g. "git.exe" -> "git", "git" -> "git").
kimix::string bash_token_stem(kimix::string_view token) {
    size_t b = 0;
    size_t e = token.size();
    while (b < e && (token[b] == '"' || token[b] == '\'')) {
        ++b;
    }
    while (e > b && (token[e - 1] == '"' || token[e - 1] == '\'')) {
        --e;
    }
    kimix::string_view s = token.substr(b, e - b);
    // Directory part: Python pathlib treats both '/' and '\\' as separators.
    size_t sep = kimix::string_view::npos;
    for (size_t i = s.size(); i-- > 0;) {
        if (s[i] == '/' || s[i] == '\\') {
            sep = i;
            break;
        }
    }
    if (sep != kimix::string_view::npos) {
        s = s.substr(sep + 1);
    }
    // Path.stem: name without the extension; "final_component.rpartition('.')[0]
    // or the whole name" — a leading dot does not start an extension.
    const size_t dot = s.rfind('.');
    if (dot != kimix::string_view::npos && dot > 0) {
        s = s.substr(0, dot);
    }
    return kimix::string(s);
}

} // namespace

void split_shell_segments(kimix::string_view command,
                          kimix::vector<shell_segment> &out) {
    // common.py _split_shell_segments (1268-1347).
    out.clear();
    kimix::string current;
    int64_t i = 0;
    const int64_t n = static_cast<int64_t>(command.size());
    // Reference behaviour on an unterminated region: append the rest of the
    // command to the current segment and stop scanning.
    const auto consume_rest = [&]() {
        current.append(command.data() + i, static_cast<size_t>(n - i));
        i = n;
    };
    while (i < n) {
        const char c = command[static_cast<size_t>(i)];
        if (c == '\'') {
            const size_t end = command.find('\'', static_cast<size_t>(i) + 1);
            if (end == kimix::string_view::npos) {
                consume_rest();
                continue;
            }
            current.append(command.data() + i, end + 1 - static_cast<size_t>(i));
            i = static_cast<int64_t>(end) + 1;
        } else if (c == '"') {
            const int64_t end = bash_find_dq_end(command, i + 1);
            if (end == -1) {
                consume_rest();
                continue;
            }
            current.append(command.data() + i, static_cast<size_t>(end - i));
            i = end;
        } else if (c == '$' && i + 1 < n && command[static_cast<size_t>(i + 1)] == '\'') {
            const int64_t end = bash_find_ansi_c_end(command, i + 2);
            if (end == -1) {
                consume_rest();
                continue;
            }
            current.append(command.data() + i, static_cast<size_t>(end - i));
            i = end;
        } else if (c == '$' && i + 1 < n && command[static_cast<size_t>(i + 1)] == '(') {
            const int64_t end = bash_find_matching_paren(command, i + 1);
            if (end == -1) {
                consume_rest();
                continue;
            }
            current.append(command.data() + i, static_cast<size_t>(end + 1 - i));
            i = end + 1;
        } else if (c == '`') {
            const int64_t end = bash_find_backtick_end(command, i + 1);
            if (end == -1) {
                consume_rest();
                continue;
            }
            current.append(command.data() + i, static_cast<size_t>(end - i));
            i = end;
        } else if (c == ';') {
            out.push_back(shell_segment{current, kimix::string(";")});
            current.clear();
            ++i;
        } else if (c == '|') {
            if (i + 1 < n && command[static_cast<size_t>(i + 1)] == '|') {
                out.push_back(shell_segment{current, kimix::string("||")});
                current.clear();
                i += 2;
            } else {
                // Single | stays inside the segment: the per-segment rewriter
                // only rewrites the leftmost command of the pipeline.
                current.push_back(c);
                ++i;
            }
        } else if (c == '&') {
            if (i + 1 < n && command[static_cast<size_t>(i + 1)] == '&') {
                out.push_back(shell_segment{current, kimix::string("&&")});
                current.clear();
                i += 2;
            } else {
                current.push_back(c);
                ++i;
            }
        } else {
            current.push_back(c);
            ++i;
        }
    }
    out.push_back(shell_segment{std::move(current), kimix::string()});
}

bool is_known_rtk_command(kimix::string_view name) {
    // common.py _RTK_KNOWN_COMMANDS (363-439) + _is_known_rtk_command (456-460):
    // strip a trailing ".exe" (case-insensitive), then lowercase lookup.
    static constexpr kimix::string_view table[] = {
        // File
        "ls", "tree", "read", "smart",
        // NOTE: `find` is intentionally NOT wrapped by rtk (see the reference
        // comment): rtk's find emulation is not a drop-in for find(1).
        "grep", "rg", "diff", "wc", "json", "log", "env", "deps",
        // Git
        "git",
        // Rust
        "cargo",
        // JS/TS
        "vitest", "jest", "tsc", "lint", "prettier", "format", "next",
        "prisma", "playwright", "npm", "npx", "pnpm",
        // Python
        "pytest", "ruff", "mypy", "pip", "uv",
        // Go
        "go", "golangci-lint",
        // Ruby
        "rspec", "rubocop", "rake",
        // .NET
        "dotnet",
        // Docker/K8s
        "docker", "kubectl", "oc",
        // Cloud/CLI
        "aws", "gh", "glab", "gt", "curl", "wget", "psql",
        // Other
        "php", "phpunit", "phpstan", "pest", "paratest", "ecs", "pint",
        "gradlew", "mvn",
    };
    kimix::string_view key = name;
    if (key.size() >= 4 && bash_iequals(key.substr(key.size() - 4), ".exe")) {
        key = key.substr(0, key.size() - 4);
    }
    for (const kimix::string_view entry : table) {
        if (bash_iequals(key, entry)) {
            return true;
        }
    }
    return false;
}

rewrite_result rewrite_shell_segment(kimix::string_view segment,
                                     bool exclude_read, bool pwsh) {
    // common.py _rewrite_shell_segment (1429-1466).
    rewrite_result res;
    res.segment = kimix::string(segment);
    res.changed = false;

    int64_t i = 0;
    int64_t token_start = -1;
    kimix::string token;
    for (;;) {
        const bash_shell_word w = bash_read_shell_word(segment, i);
        if (w.start < 0) {
            return res; // no word at all -> unchanged
        }
        if (w.word == "RTK_DISABLED=1") {
            return res;
        }
        if (bash_is_shell_assignment(w.word)) {
            i = w.next;
            continue;
        }
        bool is_prefix = false;
        for (const kimix::string_view p : bash_prefix_skip) {
            if (w.word == p) {
                is_prefix = true;
                break;
            }
        }
        if (is_prefix) {
            i = w.next;
            continue;
        }
        token = w.word;
        token_start = w.start;
        break;
    }

    // Strip surrounding quotes so quoted absolute paths still match by stem.
    const kimix::string name = bash_token_stem(token);
    kimix::string lowered;
    lowered.reserve(name.size());
    for (const char c : name) {
        lowered.push_back(bash_lower_ascii(c));
    }
    if (lowered == "rtk") {
        return res; // Path.stem already removed ".exe"
    }
    if (exclude_read && lowered == "read") {
        return res;
    }
    if (!is_known_rtk_command(name)) {
        return res;
    }

    // Use the bare `rtk` executable name; PowerShell needs the `&` call
    // operator to invoke a command by name.
    const char *prefix = pwsh ? "& rtk " : "rtk ";
    kimix::string out;
    out.reserve(segment.size() + 8);
    out.append(segment.data(), static_cast<size_t>(token_start));
    out += prefix;
    out.append(segment.data() + token_start,
               segment.size() - static_cast<size_t>(token_start));
    res.segment = std::move(out);
    res.changed = true;
    return res;
}

rewrite_result maybe_rewrite_shell_command_with_rtk(kimix::string_view command,
                                                    bool token_kill,
                                                    bool rtk_available,
                                                    kimix::string_view rtk_binary_path,
                                                    bool exclude_read, bool pwsh) {
    // common.py _maybe_rewrite_shell_command_with_rtk (1469-1538).
    rewrite_result res;
    res.segment = kimix::string(command);
    res.changed = false;

    if (!token_kill || !rtk_available) {
        return res;
    }
    // `not command or command.isspace()`
    if (command.empty()) {
        return res;
    }
    bool all_space = true;
    for (const char c : command) {
        if (!bash_is_space(c)) {
            all_space = false;
            break;
        }
    }
    if (all_space) {
        return res;
    }

    // lstrip() then the rtk-prefix fast paths.
    size_t lb = 0;
    while (lb < command.size() && bash_is_space(command[lb])) {
        ++lb;
    }
    const kimix::string_view stripped = command.substr(lb);
    if (bash_starts_with(stripped, "rtk ") || bash_starts_with(stripped, "rtk\t") ||
        stripped == "rtk" || bash_starts_with(stripped, "rtk.exe") ||
        bash_starts_with(stripped, "& rtk ") || stripped == "& rtk") {
        return res;
    }

    // Absolute rtk path fast path (rtk_path is None == empty here -> skipped).
    if (!rtk_binary_path.empty()) {
        kimix::string_view head = stripped;
        if (bash_starts_with(head, "& ")) {
            head = head.substr(2);
            while (!head.empty() && bash_is_space(head[0])) {
                head = head.substr(1);
            }
        }
        kimix::string quoted;
        quoted.reserve(rtk_binary_path.size() + 2);
        quoted.push_back('"');
        quoted.append(rtk_binary_path.data(), rtk_binary_path.size());
        quoted.push_back('"');
        if (bash_starts_with(head, rtk_binary_path) ||
            bash_starts_with(head, quoted)) {
            return res;
        }
    }

    kimix::vector<shell_segment> segments;
    split_shell_segments(command, segments);
    // Multi-segment commands skip rtk entirely: rtk cannot guarantee
    // newline-terminated output, so a wrapped segment would glue the next
    // command's text onto the same line (reference comment 1518-1525).
    if (segments.size() > 1) {
        return res;
    }

    bool changed = false;
    kimix::string rebuilt;
    rebuilt.reserve(command.size() + 8);
    for (const shell_segment &seg : segments) {
        const rewrite_result r = rewrite_shell_segment(seg.text, exclude_read, pwsh);
        rebuilt += r.segment;
        rebuilt += seg.sep;
        changed = changed || r.changed;
    }
    if (!changed) {
        return res;
    }
    res.segment = std::move(rebuilt);
    res.changed = true;
    return res;
}

// ===========================================================================
// Bounded-run capture/timeout/kill policy state machine
// ===========================================================================

kimix::string bounded_append_capture(kimix::string_view content,
                                     kimix::string_view text, int64_t cap,
                                     bool &truncated) {
    // background/utils.py bounded_append (42-76), character-based like the
    // Python reference (code points, matching len(str)/slicing).
    kimix::string full;
    full.reserve(content.size() + text.size());
    full.append(content.data(), content.size());
    full.append(text.data(), text.size());

    const int64_t n = static_cast<int64_t>(kimix::builtin_tools::utf8_code_point_count(full));
    if (n <= cap) {
        return full;
    }
    truncated = true;
    // int(cap * 0.4) -- Python float multiply + truncation toward zero.
    const int64_t head_len = static_cast<int64_t>(static_cast<double>(cap) * 0.4);
    const int64_t tail_len = cap - head_len;

    const size_t head_end =
        kimix::builtin_tools::utf8_byte_offset_of_code_point(full, static_cast<size_t>(head_len));
    const size_t tail_begin =
        kimix::builtin_tools::utf8_byte_offset_of_code_point(full, static_cast<size_t>(n - tail_len));

    kimix::string out;
    out.reserve(head_end + (full.size() - tail_begin) + 64);
    out.append(full.data(), head_end);
    out += "\n[... (output truncated, keeping first ";
    out += kimix::format("{}", head_len);
    out += " and last ";
    out += kimix::format("{}", tail_len);
    out += " chars)]\n";
    out.append(full.data() + tail_begin, full.size() - tail_begin);
    return out;
}

kimix::string process_exited_banner(int64_t exit_code,
                                    kimix::optional<int64_t> error_line) {
    // common.py ProcessStream (2118-2124): the banner the stream queues when a
    // foreground process exits non-zero.
    kimix::string out = "\n[Process exited with code ";
    out += kimix::format("{}", exit_code);
    if (error_line.has_value()) {
        out += ", error at line ";
        out += kimix::format("{}", *error_line);
    }
    out += "]";
    return out;
}

capture_machine::capture_machine() = default;
capture_machine::capture_machine(capture_config config) : config_(std::move(config)) {
    // background/utils.py wait_for_output (353): the inactivity bound runs
    // whenever inactivity_timeout > 0; the total timeout is checked FIRST on
    // every iteration, so the inactivity == timeout configuration resolves to
    // the timeout (matching bash_tool.__call__, which passes
    // min(DEFAULT_INACTIVITY_TIMEOUT, params.timeout)).
    inactivity_armed_ = config_.inactivity_timeout_ms > 0;
}

const kimix::string &capture_machine::output() const { return output_; }

kimix::optional<int64_t> capture_machine::exit_code() const { return exit_code_; }

bool capture_machine::matched() const { return matched_; }

bool capture_machine::truncated() const { return truncated_; }

bool capture_machine::finished() const { return finished_; }

int64_t capture_machine::last_output_elapsed_ms() const {
    return last_output_elapsed_ms_;
}

void capture_machine::bounded_append_chunk(kimix::string_view text) {
    bool trunc = false;
    output_ = bounded_append_capture(output_, text, config_.output_cap_chars, trunc);
    truncated_ = truncated_ || trunc;
}

bool capture_machine::pattern_matches() const {
    // The Python reference compiles wait_for_pattern with the `regex` module
    // and calls pattern.search(output). The policy machine compares a literal
    // substring (the common agent usage: "ready", "Listening on", prompt
    // markers); regex patterns are the caller's responsibility at the binding
    // layer, which can inject a pre-decided match via capture_config. Keeping
    // the kernel regex-free avoids std::regex/Python-regex semantic drift (see
    // plan 8 risks).
    if (config_.wait_pattern.empty()) {
        return false;
    }
    return output_.find(config_.wait_pattern) != kimix::string::npos;
}

capture_decision capture_machine::on_event(const capture_event &event) {
    capture_decision d;
    d.elapsed_ms = event.elapsed_ms;
    d.truncated = truncated_;

    // After a stop decision the machine is finished: replay the decision so
    // callers draining races see a stable answer.
    if (finished_) {
        // Exit-code bookkeeping keeps flowing (a late process_exited event).
        if (event.type == capture_event::kind::process_exited) {
            exit_code_ = event.exit_code;
        }
        last_stop_.elapsed_ms = event.elapsed_ms;
        last_stop_.truncated = truncated_;
        return last_stop_;
    }

    // 1. Drain: append the chunk payload / record the exit code.
    if (event.type == capture_event::kind::chunk) {
        if (!event.text.empty()) {
            bounded_append_chunk(event.text);
            last_output_elapsed_ms_ = event.elapsed_ms; // activity refresh
        }
    } else {
        exit_code_ = event.exit_code;
    }
    d.truncated = truncated_;

    // 2. Pattern check on the accumulated output (before the timeout check,
    //    exactly like wait_for_output 338-340).
    if (!config_.wait_pattern.empty() && pattern_matches()) {
        matched_ = true;
        finished_ = true;
        d.act = capture_decision::action::pattern_stop;
        d.matched = true;
        last_stop_ = d;
        return d;
    }

    // 3. Process exit: the final drain already happened above; the process
    //    ended on its own, so no kill is needed even when the total timeout
    //    was reached on the same event (the caller's thread_is_alive check
    //    takes the completion path - bash_tool.py 857/904).
    if (event.type == capture_event::kind::process_exited) {
        finished_ = true;
        d.act = capture_decision::action::complete_stop;
        last_stop_ = d;
        return d;
    }

    // 4. Total timeout. `timeout_ms <= 0` fires immediately (elapsed >= 0),
    //    mirroring `if timeout <= 0 or elapsed >= timeout` (wait_for_output
    //    341): the caller then kills the process tree (_stop_after_timeout).
    if (event.elapsed_ms >= config_.timeout_ms) {
        finished_ = true;
        d.act = capture_decision::action::timeout_kill;
        last_stop_ = d;
        return d;
    }

    // 5. Inactivity timeout: no output for at least inactivity_timeout_ms
    //    (wait_for_output 353-359). The timer starts at the run start
    //    (elapsed 0), like _last_output_time in the stream constructor. The
    //    `timeout_ms > 0` guard mirrors the reference loop: with a
    //    non-positive total timeout the loop breaks before the inactivity
    //    branch is reached.
    if (inactivity_armed_ && config_.timeout_ms > 0 &&
        event.elapsed_ms - last_output_elapsed_ms_ >= config_.inactivity_timeout_ms) {
        finished_ = true;
        d.act = capture_decision::action::inactivity_stop;
        last_stop_ = d;
        return d;
    }

    d.act = capture_decision::action::wait;
    return d;
}

// ===========================================================================
// Hardline safety floor (plans/bash.md 3.4.1)
// Source: D:/kimi-agent/src/kimix/tools/file/bash/safety.py 48-219.
// ===========================================================================

namespace {

// Whitespace-collapse helper matching " ".join(command.split()).
kimix::string bash_collapse_whitespace(kimix::string_view s) {
    kimix::string out;
    out.reserve(s.size());
    bool need_space = false;
    bool in_space = true;
    for (const char c : s) {
        if (bash_is_space(c)) {
            if (!in_space) {
                need_space = true;
            }
            in_space = true;
        } else {
            if (need_space && !out.empty()) {
                out.push_back(' ');
            }
            out.push_back(c);
            need_space = false;
            in_space = false;
        }
    }
    return out;
}

// Tokenize the tail of a collapsed command starting at `start`, stopping at the
// first shell separator (; && || | newline). Mirrors _segment_tokens.
kimix::vector<kimix::string_view>
bash_segment_tokens(kimix::string_view text, size_t start) {
    kimix::vector<kimix::string_view> tokens;
    if (start >= text.size()) {
        return tokens;
    }
    // Stop at the first separator.
    size_t limit = text.size();
    for (size_t i = start; i < text.size();) {
        if (text[i] == ';' || text[i] == '\n') {
            limit = i;
            break;
        }
        if (text[i] == '|') {
            // `||` and single `|` are segment separators.
            limit = i;
            break;
        }
        if (text[i] == '&') {
            // `&&` and single `&` are segment separators.
            limit = i;
            break;
        }
        ++i;
    }
    kimix::string_view tail = text.substr(start, limit - start);
    size_t i = 0;
    while (i < tail.size()) {
        while (i < tail.size() && bash_is_space(tail[i])) {
            ++i;
        }
        if (i >= tail.size()) {
            break;
        }
        size_t j = i;
        while (j < tail.size() && !bash_is_space(tail[j])) {
            ++j;
        }
        tokens.push_back(tail.substr(i, j - i));
        i = j;
    }
    return tokens;
}

// _looks_like_flag (84-91): -... or /alpha...
bool bash_looks_like_flag(kimix::string_view token) noexcept {
    if (token.size() > 1 && token[0] == '-') {
        return true;
    }
    if (token.size() > 1 && token[0] == '/' && bash_is_alpha(token[1])) {
        return true;
    }
    return false;
}

// _collect_flags (94-110): collect short/long flag letters r/f/s/q.
kimix::vector<char> bash_collect_flags(const kimix::vector<kimix::string_view> &tokens) {
    kimix::vector<char> flags;
    for (const auto &token : tokens) {
        if (!bash_looks_like_flag(token)) {
            continue;
        }
        kimix::string_view core = token;
        if (core.size() > 1 && (core[0] == '-' || core[0] == '/')) {
            core = core.substr(1);
        }
        if (core.empty()) {
            continue;
        }
        // Lowercase core for substring checks.
        kimix::string lowered;
        lowered.reserve(core.size());
        for (const char c : core) {
            lowered.push_back(bash_lower_ascii(c));
        }
        if (lowered.find("recursive") != kimix::string::npos) {
            flags.push_back('r');
        }
        if (lowered.find("force") != kimix::string::npos) {
            flags.push_back('f');
        }
        for (const char c : core) {
            const char lc = bash_lower_ascii(c);
            if (lc == 'r' || lc == 'f' || lc == 's' || lc == 'q') {
                flags.push_back(lc);
            }
        }
    }
    return flags;
}

bool bash_has_flag(const kimix::vector<char> &flags, char f) noexcept {
    for (const char c : flags) {
        if (c == f) {
            return true;
        }
    }
    return false;
}

// _rm_target_is_protected (113-131).
bool bash_rm_target_is_protected(kimix::string_view target) noexcept {
    // Strip surrounding quotes.
    size_t b = 0;
    size_t e = target.size();
    while (b < e && (target[b] == '"' || target[b] == '\'')) {
        ++b;
    }
    while (e > b && (target[e - 1] == '"' || target[e - 1] == '\'')) {
        --e;
    }
    kimix::string t(target.data() + b, e - b);
    // Replace ${home} -> $home
    for (size_t i = 0; i + 7 <= t.size();) {
        if (t[i] == '$' && t[i + 1] == '{' &&
            bash_lower_ascii(t[i + 2]) == 'h' && bash_lower_ascii(t[i + 3]) == 'o' &&
            bash_lower_ascii(t[i + 4]) == 'm' && bash_lower_ascii(t[i + 5]) == 'e' &&
            t[i + 6] == '}') {
            t.replace(i, 7, "$home");
            i += 5;
        } else {
            ++i;
        }
    }
    // Lowercase copy for comparisons.
    kimix::string lower;
    lower.reserve(t.size());
    for (const char c : t) {
        lower.push_back(bash_lower_ascii(c));
    }
    kimix::string_view lv(lower);
    // Trim trailing /\, but keep a lone root slash.
    while (lv.size() > 1 && (lv.back() == '/' || lv.back() == '\\')) {
        lv.remove_suffix(1);
    }
    if (lv == "~" || lv == "$home") {
        return true;
    }
    // Windows drive root with optional glob.
    if (lv.size() >= 2 && lv[1] == ':') {
        bool alpha0 = bash_is_alpha(lv[0]);
        bool rest_root = true;
        for (size_t i = 2; i < lv.size(); ++i) {
            if (lv[i] != '/' && lv[i] != '\\' && lv[i] != '*') {
                rest_root = false;
                break;
            }
        }
        if (alpha0 && rest_root) {
            return true;
        }
    }
    if (!lv.empty() && lv[0] == '/') {
        // Split path, dropping empty, ., .., and * only.
        kimix::vector<kimix::string_view> parts;
        size_t i = 1;
        while (i <= lv.size()) {
            size_t j = i;
            while (j < lv.size() && lv[j] != '/') {
                ++j;
            }
            kimix::string_view part = lv.substr(i, j - i);
            if (!part.empty() && part != "." && part != "..") {
                parts.push_back(part);
            }
            i = j + 1;
        }
        if (parts.empty()) {
            return true;
        }
        if (parts.size() == 1 && parts[0] == "*") {
            return true;
        }
    }
    return false;
}

// Find the next occurrence of a command word with optional .exe suffix.
// Returns position and matched word (without .exe).
struct bash_word_match {
    size_t pos = kimix::string_view::npos;
    kimix::string_view word;
};

bash_word_match bash_find_command_word(kimix::string_view text,
                                       kimix::string_view name) noexcept {
    bash_word_match m;
    size_t i = 0;
    while (i < text.size()) {
        // Look for a word boundary start.
        if (i > 0 && bash_is_word_char(text[i - 1])) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < text.size() && !bash_is_space(text[j]) && text[j] != ';' &&
               text[j] != '|' && text[j] != '&') {
            ++j;
        }
        kimix::string_view token = text.substr(i, j - i);
        // Strip .exe suffix for comparison.
        kimix::string_view stem = token;
        if (stem.size() > 4 &&
            bash_iequals(stem.substr(stem.size() - 4), ".exe")) {
            stem = stem.substr(0, stem.size() - 4);
        }
        if (bash_iequals(stem, name)) {
            m.pos = i;
            m.word = token;
            return m;
        }
        i = j + 1;
        if (j == i) {
            ++i;
        }
    }
    return m;
}

// _detect_recursive_delete (134-150).
kimix::optional<kimix::string>
bash_detect_recursive_delete(kimix::string_view text) {
    const char *names[] = {"rm", "rmdir", "del"};
    for (const char *name : names) {
        size_t pos = 0;
        for (;;) {
            bash_word_match m = bash_find_command_word(text.substr(pos), name);
            if (m.pos == kimix::string_view::npos) {
                break;
            }
            const size_t global_pos = pos + m.pos;
            const size_t word_end = global_pos + m.word.size();
            const auto tokens =
                bash_segment_tokens(text, word_end);
            const auto flags = bash_collect_flags(tokens);
            kimix::string_view command_word = m.word;
            kimix::string_view lowered_cmd = command_word;
            if (lowered_cmd.size() > 4 &&
                bash_iequals(lowered_cmd.substr(lowered_cmd.size() - 4), ".exe")) {
                lowered_cmd = lowered_cmd.substr(0, lowered_cmd.size() - 4);
            }
            kimix::string lowered;
            for (const char c : lowered_cmd) {
                lowered.push_back(bash_lower_ascii(c));
            }
            bool sufficient = false;
            if (lowered == "rm") {
                sufficient = bash_has_flag(flags, 'r') || bash_has_flag(flags, 'f');
            } else if (lowered == "rmdir") {
                sufficient = bash_has_flag(flags, 'r') || bash_has_flag(flags, 's');
            } else if (lowered == "del") {
                sufficient = bash_has_flag(flags, 'r') || bash_has_flag(flags, 'f') ||
                             bash_has_flag(flags, 's');
            }
            if (!sufficient) {
                pos = global_pos + 1;
                continue;
            }
            for (const auto &target : tokens) {
                if (!bash_looks_like_flag(target)) {
                    if (bash_rm_target_is_protected(target)) {
                        kimix::string desc = "Recursive delete of protected root/home (`";
                        desc.append(target.data(), target.size());
                        desc += "`)";
                        return desc;
                    }
                }
            }
            pos = global_pos + 1;
        }
    }
    return std::nullopt;
}

// Find any occurrence of a whole word (ASCII \w boundaries on both sides).
bool bash_has_word(kimix::string_view text, kimix::string_view word) noexcept {
    size_t from = 0;
    for (;;) {
        if (from > text.size()) {
            return false;
        }
        const size_t rel = bash_find_ci(text.substr(from), word);
        if (rel == kimix::string_view::npos) {
            return false;
        }
        const size_t pos = from + rel;
        const bool left_ok = pos == 0 || !bash_is_word_char(text[pos - 1]);
        const size_t after = pos + word.size();
        const bool right_ok = after >= text.size() || !bash_is_word_char(text[after]);
        if (left_ok && right_ok) {
            return true;
        }
        from = pos + 1;
    }
}

} // namespace

void command_detection_variants(kimix::string_view command,
                                kimix::vector<kimix::string> &out) {
    // safety.py command_detection_variants (48-70).
    out.clear();
    bool any_non_space = false;
    for (const char c : command) {
        if (!bash_is_space(c)) {
            any_non_space = true;
            break;
        }
    }
    if (!any_non_space) {
        return;
    }
    const kimix::string collapsed = bash_collapse_whitespace(command);
    kimix::string deobfuscated;
    deobfuscated.reserve(collapsed.size());
    for (const char c : collapsed) {
        if (c != '\\' && c != '\'' && c != '"') {
            deobfuscated.push_back(bash_lower_ascii(c));
        }
    }
    kimix::string lowered;
    lowered.reserve(collapsed.size());
    for (const char c : collapsed) {
        lowered.push_back(bash_lower_ascii(c));
    }
    auto add = [&](const kimix::string_view &v) {
        kimix::string s(v.data(), v.size());
        bool found = false;
        for (const auto &existing : out) {
            if (existing == s) {
                found = true;
                break;
            }
        }
        if (!found) {
            out.push_back(std::move(s));
        }
    };
    add(collapsed);
    add(deobfuscated);
    add(lowered);
}

hardline_result detect_hardline_command(kimix::string_view command) {
    // safety.py detect_hardline_command (153-203). ASCII-only: non-ASCII is
    // treated as safe so the shim's isascii() gate stays the authoritative
    // fallback switch.
    hardline_result res;
    bool any_non_space = false;
    for (const char c : command) {
        if (!bash_is_space(c)) {
            any_non_space = true;
            break;
        }
    }
    if (!any_non_space) {
        return res;
    }
    // Non-ASCII defensive gate: treat as not blocked.
    for (const char c : command) {
        if (static_cast<uint8_t>(c) >= 0x80u) {
            return res;
        }
    }
    const kimix::string text = [&] {
        kimix::string collapsed = bash_collapse_whitespace(command);
        for (char &c : collapsed) {
            c = bash_lower_ascii(c);
        }
        return collapsed;
    }();

    // 1. Recursive delete.
    const auto recursive_delete = bash_detect_recursive_delete(text);
    if (recursive_delete.has_value()) {
        res.blocked = true;
        res.description = *recursive_delete;
        return res;
    }

    // 2. Disk formatting (mkfs.*).
    if (bash_has_word(text, "mkfs")) {
        res.blocked = true;
        res.description = "Disk formatting command (`mkfs`) is blocked";
        return res;
    }

    // 3. dd writing to a raw device (matches disk, sd, nvme, rdisk prefixes).
    if (bash_has_word(text, "dd")) {
        size_t pos = 0;
        for (;;) {
            const size_t found = text.find("of=/dev/", pos);
            if (found == kimix::string::npos) {
                break;
            }
            pos = found + 8;
            const size_t prefix_len =
                (found + 8 + 4 <= text.size()) ? 4 : (text.size() - found - 8);
            const kimix::string_view prefix(text.data() + found + 8, prefix_len);
            if (prefix.size() >= 2) {
                const char c0 = bash_lower_ascii(prefix[0]);
                const char c1 = bash_lower_ascii(prefix[1]);
                if ((c0 == 's' && c1 == 'd') ||
                    (c0 == 'n' && c1 == 'v') ||
                    (c0 == 'h' && c1 == 'd') ||
                    (c0 == 'r' && c1 == 'd')) {
                    res.blocked = true;
                    res.description = "`dd` writing to a raw device is blocked";
                    return res;
                }
                if (prefix.size() >= 4 &&
                    bash_lower_ascii(prefix[0]) == 'd' &&
                    bash_lower_ascii(prefix[1]) == 'i' &&
                    bash_lower_ascii(prefix[2]) == 's' &&
                    bash_lower_ascii(prefix[3]) == 'k') {
                    res.blocked = true;
                    res.description = "`dd` writing to a raw device is blocked";
                    return res;
                }
            }
        }
    }

    // 4. System power commands as the first word.
    {
        kimix::vector<kimix::string_view> words;
        size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && bash_is_space(text[i])) {
                ++i;
            }
            if (i >= text.size()) {
                break;
            }
            size_t j = i;
            while (j < text.size() && !bash_is_space(text[j])) {
                ++j;
            }
            words.push_back(kimix::string_view(text.data() + i, j - i));
            i = j;
        }
        if (!words.empty()) {
            const kimix::string_view first = words[0];
            if (first == "shutdown" || first == "reboot" || first == "poweroff" ||
                first == "halt") {
                res.blocked = true;
                res.description = kimix::string("System `") +
                                  kimix::string(first.data(), first.size()) +
                                  "` command is blocked";
                return res;
            }
        }
    }

    // 5. Fork bomb pattern.
    if (text.find(":(){") != kimix::string::npos &&
        text.find(":|:") != kimix::string::npos &&
        text.find(":&") != kimix::string::npos) {
        res.blocked = true;
        res.description = "Fork bomb pattern detected";
        return res;
    }

    // 6. kill targeting PID 1 or $PPID.
    {
        size_t pos = 0;
        for (;;) {
            bash_word_match m = bash_find_command_word(text.substr(pos), "kill");
            if (m.pos == kimix::string_view::npos) {
                break;
            }
            const size_t global_pos = pos + m.pos;
            const size_t word_end = global_pos + m.word.size();
            const auto tokens = bash_segment_tokens(text, word_end);
            for (const auto &target : tokens) {
                if (!bash_looks_like_flag(target)) {
                    kimix::string lower;
                    for (const char c : target) {
                        lower.push_back(bash_lower_ascii(c));
                    }
                    if (lower == "1" || lower == "$ppid") {
                        res.blocked = true;
                        res.description = "`kill` targeting PID 1 (or `$PPID`) is blocked";
                        return res;
                    }
                }
            }
            pos = global_pos + 1;
        }
    }

    // 7. Windows format on a drive letter.
    {
        size_t pos = 0;
        for (;;) {
            bash_word_match m = bash_find_command_word(text.substr(pos), "format");
            if (m.pos == kimix::string_view::npos) {
                break;
            }
            const size_t global_pos = pos + m.pos;
            const size_t word_end = global_pos + m.word.size();
            const auto tokens = bash_segment_tokens(text, word_end);
            for (const auto &target : tokens) {
                if (!bash_looks_like_flag(target) && target.size() >= 2 &&
                    target[1] == ':' && bash_is_alpha(target[0])) {
                    bool rest_ok = true;
                    for (size_t i = 2; i < target.size(); ++i) {
                        if (target[i] != '/' && target[i] != '\\') {
                            rest_ok = false;
                            break;
                        }
                    }
                    if (rest_ok) {
                        res.blocked = true;
                        res.description = "Windows `format` on a drive is blocked";
                        return res;
                    }
                }
            }
            pos = global_pos + 1;
        }
    }

    return res;
}

hardline_result check_hardline_blocked(kimix::string_view command) {
    // safety.py check_hardline_blocked (206-219).
    hardline_result res;
    kimix::vector<kimix::string> variants;
    command_detection_variants(command, variants);
    if (variants.empty()) {
        return res;
    }
    for (const auto &variant : variants) {
        res = detect_hardline_command(variant);
        if (res.blocked) {
            return res;
        }
    }
    return res;
}

// ===========================================================================
// Foreground / background guidance (plans/bash.md 3.4.2)
// Source: D:/kimi-agent/src/kimix/tools/file/bash/safety.py 227-269.
// ===========================================================================

namespace {

// _strip_quoted (248-251): replace single/double-quoted spans with spaces.
kimix::string bash_strip_quoted(kimix::string_view s) {
    kimix::string out;
    out.reserve(s.size());
    char quote = 0;
    for (const char c : s) {
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            }
            out.push_back(' ');
        } else if (c == '\'' || c == '"') {
            quote = c;
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    if (quote != 0) {
        // Unterminated quote: the rest was already replaced with spaces.
    }
    return out;
}

bool bash_is_long_running(const kimix::vector<kimix::string_view> &words) noexcept {
    // _LONG_RUNNING_PATTERNS (227-240), rewritten as token scans.
    const size_t n = words.size();
    for (size_t i = 0; i < n; ++i) {
        const kimix::string_view w = words[i];
        if (w == "vite" || w == "nodemon" || w == "uvicorn" || w == "gunicorn") {
            return true;
        }
        if (w == "next" && i + 1 < n && words[i + 1] == "dev") {
            return true;
        }
        if (w == "python" && i + 2 < n && words[i + 1] == "-m" &&
            words[i + 2] == "http.server") {
            return true;
        }
        if (w == "docker" && i + 2 < n && words[i + 1] == "compose" &&
            words[i + 2] == "up") {
            return true;
        }
        if (w == "docker-compose" && i + 1 < n && words[i + 1] == "up") {
            return true;
        }
        if (w == "npm" || w == "pnpm" || w == "yarn" || w == "bun") {
            size_t k = i + 1;
            if (k < n && words[k] == "run") {
                ++k;
            }
            if (k < n &&
                (words[k] == "dev" || words[k] == "start" ||
                 words[k] == "serve" || words[k] == "watch")) {
                return true;
            }
        }
        if (w == "nohup" || w == "setsid") {
            return true;
        }
    }
    // Trailing & operator.
    if (!words.empty()) {
        kimix::string_view last = words.back();
        if (!last.empty() && last.back() == '&') {
            return true;
        }
    }
    return false;
}

} // namespace

kimix::optional<kimix::string> foreground_background_guidance(kimix::string_view command) {
    // safety.py foreground_background_guidance (254-269).
    bool any_non_space = false;
    for (const char c : command) {
        if (!bash_is_space(c)) {
            any_non_space = true;
            break;
        }
    }
    if (!any_non_space) {
        return std::nullopt;
    }
    // Non-ASCII: fall back to no hint (shim gates on isascii()).
    for (const char c : command) {
        if (static_cast<uint8_t>(c) >= 0x80u) {
            return std::nullopt;
        }
    }
    const kimix::string stripped = bash_strip_quoted(command);
    const kimix::string text = bash_collapse_whitespace(stripped);
    kimix::vector<kimix::string_view> words;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && bash_is_space(text[i])) {
            ++i;
        }
        if (i >= text.size()) {
            break;
        }
        size_t j = i;
        while (j < text.size() && !bash_is_space(text[j])) {
            ++j;
        }
        words.push_back(kimix::string_view(text.data() + i, j - i));
        i = j;
    }
    if (bash_is_long_running(words)) {
        return kimix::string(
            "Long-running process detected. Consider mode='send' (background) + "
            "TaskOutput to avoid blocking on timeout.");
    }
    return std::nullopt;
}

// ===========================================================================
// Failure annotation (plans/bash.md 3.4.3)
// Source: D:/kimi-agent/src/kimix/tools/file/bash/output_enhance.py 179-217.
// ===========================================================================

kimix::optional<kimix::string> annotate_failure(kimix::string_view output,
                                                kimix::string_view command,
                                                kimix::optional<int64_t> exit_code) {
    // output_enhance.py annotate_failure (179-217). `command` and `exit_code`
    // are accepted for signature compatibility only.
    (void)command;
    (void)exit_code;
    if (output.empty()) {
        return std::nullopt;
    }
    // Non-ASCII: fall back to no hint.
    for (const char c : output) {
        if (static_cast<uint8_t>(c) >= 0x80u) {
            return std::nullopt;
        }
    }
    const size_t limit = output.size() < 4000 ? output.size() : 4000;
    const kimix::string_view sample = output.substr(0, limit);
    kimix::string lowered;
    lowered.reserve(sample.size());
    for (const char c : sample) {
        lowered.push_back(bash_lower_ascii(c));
    }

    if (lowered.find("command not found") != kimix::string::npos ||
        lowered.find("not recognized as an internal or external command") !=
            kimix::string::npos) {
        return kimix::string(
            "The command was not found. Check it is installed and on PATH "
            "(use `which <cmd>` / `Get-Command <cmd>`).");
    }
    if (lowered.find("no such file or directory") != kimix::string::npos) {
        return kimix::string(
            "A file or directory referenced by the command does not exist. "
            "Verify the path with `Glob`/ReadFile.");
    }
    // re.search(r"modulenotfounderror:\s*no module named ['\"]([^'\"]+)['\"]", ...)
    static constexpr kimix::string_view k_marker =
        "modulenotfounderror:";
    static constexpr kimix::string_view k_no_module_named =
        "no module named";
    size_t pos = lowered.find(k_marker);
    while (pos != kimix::string_view::npos) {
        size_t q = pos + k_marker.size();
        while (q < lowered.size() && bash_is_space(lowered[q])) {
            ++q;
        }
        if (q + k_no_module_named.size() <= lowered.size() &&
            lowered.compare(q, k_no_module_named.size(),
                            k_no_module_named.data(),
                            k_no_module_named.size()) == 0) {
            q += k_no_module_named.size();
            while (q < lowered.size() && bash_is_space(lowered[q])) {
                ++q;
            }
            if (q < lowered.size() && (lowered[q] == '\'' || lowered[q] == '"')) {
                const char quote = lowered[q];
                size_t end = q + 1;
                while (end < lowered.size() && lowered[end] != quote) {
                    ++end;
                }
                if (end < lowered.size()) {
                    // Extract the module name from the ORIGINAL output so its
                    // case is preserved (matches the Python reference).
                    kimix::string_view module_name(output.data() + q + 1,
                                                    end - q - 1);
                    if (!module_name.empty()) {
                        kimix::StringScratch s;
                        s << "Python module " << module_name
                          << " is missing. Install it (e.g. `pip install "
                          << module_name << "`) or check the environment.";
                        return kimix::string(s.string());
                    }
                }
            }
        }
        pos = lowered.find(k_marker, pos + 1);
    }
    if (lowered.find("permission denied") != kimix::string::npos) {
        return kimix::string(
            "Permission denied. Check file permissions (ls -la) or ownership.");
    }
    return std::nullopt;
}

// ===========================================================================
// Parameter parsing (plans/bash.md 3.4.5)
// Source: D:/kimi-agent/src/kimix/tools/file/bash/bash_tool.py BashParams 565-587.
// ===========================================================================

tool_error parse_bash_params(const kimix::builtin_tools::ToolParams *params,
                             bash_params &out) {
    using kimix::builtin_tools::ToolParams;
    using kimix::builtin_tools::ValueElement;
    out = bash_params{};
    if (params == nullptr) {
        return {tool_status::invalid_input, "missing parameters"};
    }
    // cmd (required, alias "command").
    const ValueElement *cmd_elem = params->get("cmd");
    if (cmd_elem == nullptr) {
        cmd_elem = params->get("command");
    }
    if (cmd_elem == nullptr || !cmd_elem->is_string()) {
        return {tool_status::invalid_input, "missing required string field 'cmd'"};
    }
    out.cmd = cmd_elem->as_string();

    // mode (optional, default "execute").
    const ValueElement *mode_elem = params->get("mode");
    if (mode_elem != nullptr) {
        if (!mode_elem->is_string()) {
            return {tool_status::invalid_input, "field 'mode' must be a string"};
        }
        out.mode = mode_elem->as_string();
    }
    if (out.mode != "execute" && out.mode != "send" && out.mode != "interactive") {
        return {tool_status::invalid_input, "field 'mode' must be 'execute', 'send' or 'interactive'"};
    }

    // timeout (optional, default 30).
    const ValueElement *timeout_elem = params->get("timeout");
    if (timeout_elem != nullptr) {
        if (timeout_elem->is_int()) {
            out.timeout = timeout_elem->as_int();
        } else if (timeout_elem->is_uint()) {
            out.timeout = static_cast<int64_t>(timeout_elem->as_uint());
        } else if (timeout_elem->is_real()) {
            out.timeout = static_cast<int64_t>(timeout_elem->as_real());
        } else {
            return {tool_status::invalid_input, "field 'timeout' must be a number"};
        }
    }

    // task_id (optional string).
    const ValueElement *task_id_elem = params->get("task_id");
    if (task_id_elem != nullptr) {
        if (!task_id_elem->is_string()) {
            return {tool_status::invalid_input, "field 'task_id' must be a string"};
        }
        out.task_id = task_id_elem->as_string();
    }

    // wait_for_pattern (optional string).
    const ValueElement *pattern_elem = params->get("wait_for_pattern");
    if (pattern_elem != nullptr) {
        if (!pattern_elem->is_string()) {
            return {tool_status::invalid_input, "field 'wait_for_pattern' must be a string"};
        }
        out.wait_for_pattern = pattern_elem->as_string();
    }

    // max_lines (optional int).
    const ValueElement *max_lines_elem = params->get("max_lines");
    if (max_lines_elem != nullptr) {
        if (max_lines_elem->is_int()) {
            out.max_lines = max_lines_elem->as_int();
        } else if (max_lines_elem->is_uint()) {
            out.max_lines = static_cast<int64_t>(max_lines_elem->as_uint());
        } else if (max_lines_elem->is_real()) {
            out.max_lines = static_cast<int64_t>(max_lines_elem->as_real());
        } else {
            return {tool_status::invalid_input, "field 'max_lines' must be a number"};
        }
    }

    return {tool_status::ok, {}};
}

// ===========================================================================
// Bash tool class (plans/bash.md 3.5)
// ===========================================================================

namespace {

const char *bash_status_string(tool_status s) noexcept {
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
    return "unknown";
}

// Build a session output block for an error/blocked result.
kimix::string bash_build_blocked_block(const bash_params &params,
                                       const kimix::string &status,
                                       const kimix::string &message) {
    python::session_output_block block;
    block.task_id = params.task_id.value_or("");
    block.status = status;
    block.output = message;
    block.exit_code = std::nullopt;
    block.exit_code_meaning = std::nullopt;
    block.failure_hint = std::nullopt;
    block.wait_matched = std::nullopt;
    block.elapsed_seconds = std::nullopt;
    block.output_path = std::nullopt;
    block.output_truncated = false;
    block.original_path = std::nullopt;
    return python::build_session_output_block(block);
}

} // namespace

Bash::Bash(kimix::builtin_tools::Session *session, config cfg)
    : Tool(session), _cfg(std::move(cfg)) {}

tool_error Bash::run(const bash_params &params, kimix::string &output_block) {
    output_block.clear();

    // Empty command check (matches bash_tool.py 735-740).
    if (params.mode != "interactive" && params.cmd.empty()) {
        output_block = bash_build_blocked_block(params, "invalid_input",
                                                "Empty command.");
        return {tool_status::invalid_input, "No command specified."};
    }

    const kimix::string_view cmd_view = params.cmd;

    // 1. Hardline safety floor.
    if (_cfg.hardline_enabled) {
        const hardline_result hr = check_hardline_blocked(cmd_view);
        if (hr.blocked && hr.description.has_value()) {
            output_block = bash_build_blocked_block(params, "blocked", *hr.description);
            return {tool_status::blocked, *hr.description};
        }
    }

    // 2. Self-kill guard (owned by pwsh).
    if (_cfg.self_kill_guard_enabled) {
        tool_status sk_status = tool_status::ok;
        const kimix::optional<kimix::string> sk_hint =
            kimix::builtin_tools::pwsh::self_kill_hint(
                cmd_view, _cfg.protected_pids, _cfg.image_names, _cfg.cmdline,
                _cfg.agent_pid, sk_status);
        if (sk_status == tool_status::unsupported) {
            output_block = bash_build_blocked_block(
                params, "unsupported",
                "Self-kill guard requires the Python mirror for this input.");
            return {tool_status::unsupported,
                    "Self-kill guard unsupported for non-ASCII or regex-metachar input."};
        }
        if (sk_hint.has_value()) {
            output_block = bash_build_blocked_block(params, "blocked", *sk_hint);
            return {tool_status::blocked, *sk_hint};
        }
    }

    // 3. Forbidden-keyword policy.
    if (!_cfg.forbidden_keywords.empty()) {
        kimix::string collapsed = bash_collapse_whitespace(cmd_view);
        for (char &c : collapsed) {
            c = bash_lower_ascii(c);
        }
        for (const auto &kw : _cfg.forbidden_keywords) {
            kimix::string lower;
            lower.reserve(kw.size());
            for (const char c : kw) {
                lower.push_back(bash_lower_ascii(c));
            }
            if (collapsed.find(lower) != kimix::string::npos) {
                kimix::string msg = "Forbidden keyword detected: `";
                msg.append(kw.data(), kw.size());
                msg += "`";
                output_block = bash_build_blocked_block(params, "blocked", msg);
                return {tool_status::blocked, msg};
            }
        }
    }

    // For send/interactive modes no further synchronous work is done; the
    // Python side owns the subprocess lifecycle.
    if (params.mode == "send" || params.mode == "interactive") {
        return {tool_status::ok, {}};
    }

    // 4. Execute-mode preflight: shell preparation and RTK rewrite callbacks.
    // The prepared command is returned in the message so the Python binding can
    // hand it to the subprocess.
    kimix::string prepared = params.cmd;
    if (_cfg.prepare_command) {
        prepared = _cfg.prepare_command(prepared);
    }
    kimix::string rtk_cmd = prepared;
    bool rtk_rewritten = false;
    if (_cfg.run_rtk_check) {
        const kimix::optional<kimix::string> check = _cfg.run_rtk_check(rtk_cmd);
        if (check.has_value()) {
            rtk_cmd = *check;
            rtk_rewritten = true;
        }
    }

    // Re-run safety floors on the prepared/rewritten command.
    if (_cfg.hardline_enabled) {
        const hardline_result hr = check_hardline_blocked(rtk_cmd);
        if (hr.blocked && hr.description.has_value()) {
            output_block = bash_build_blocked_block(params, "blocked", *hr.description);
            return {tool_status::blocked, *hr.description};
        }
    }
    if (_cfg.self_kill_guard_enabled) {
        tool_status sk_status = tool_status::ok;
        const kimix::optional<kimix::string> sk_hint =
            kimix::builtin_tools::pwsh::self_kill_hint(
                rtk_cmd, _cfg.protected_pids, _cfg.image_names, _cfg.cmdline,
                _cfg.agent_pid, sk_status);
        if (sk_status == tool_status::unsupported) {
            output_block = bash_build_blocked_block(
                params, "unsupported",
                "Self-kill guard requires the Python mirror for this input.");
            return {tool_status::unsupported,
                    "Self-kill guard unsupported for non-ASCII or regex-metachar input."};
        }
        if (sk_hint.has_value()) {
            output_block = bash_build_blocked_block(params, "blocked", *sk_hint);
            return {tool_status::blocked, *sk_hint};
        }
    }

    // The prepared command is returned via output_block so the Python binding
    // can hand it to the subprocess; the human message is a ready marker.
    output_block = std::move(rtk_cmd);
    return {tool_status::ok, "Command ready for execution"};
}

void Bash::operator()(const kimix::builtin_tools::ToolParams *parameters) {
    _result.clear();
    bash_params params;
    tool_error err = parse_bash_params(parameters, params);
    kimix::string output_block;
    if (err.status == tool_status::ok) {
        err = run(params, output_block);
    } else {
        output_block = bash_build_blocked_block(params, "invalid_input", err.message);
    }

    kimix::builtin_tools::ToolParams result;
    result.values["status"] =
        ValueElement::make_string(kimix::string(bash_status_string(err.status)));
    result.values["message"] = ValueElement::make_string(err.message);
    result.values["output_block"] = ValueElement::make_string(output_block);
    if (err.status == tool_status::ok && params.mode == "execute" &&
        !output_block.empty()) {
        result.values["command"] = ValueElement::make_string(output_block);
    }
    if (params.mode == "send" || params.mode == "interactive" ||
        params.mode == "execute") {
        result.values["mode"] = ValueElement::make_string(params.mode);
    }
    if (params.task_id.has_value()) {
        result.values["task_id"] = ValueElement::make_string(*params.task_id);
    }
    result.serialize(_result);
}

const kimix::vector<char> &Bash::serialized_result() const {
    return _result;
}

} // namespace kimix::builtin_tools::bash
