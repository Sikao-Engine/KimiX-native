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

} // namespace kimix::builtin_tools::bash
