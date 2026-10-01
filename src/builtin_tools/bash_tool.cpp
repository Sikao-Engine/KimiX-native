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

#include "builtin_tools/process_runner.h"
#include "builtin_tools/tool_registry.h"

#include "builtin_tools/pwsh_tool.h"
#include "builtin_tools/python_tool.h"
#include "builtin_tools/utf8_util.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
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

// Windows-relative tail of an ntpath-style path: the components of the
// (drive/root-free) relative part, minus empty components and "." entries.
// Mirrors CPython's PurePath._parse_path tail list:
//   path = path.replace(altsep, sep);  drv, root, rel = splitroot(path)
//   [x for x in rel.split(sep) if x and x != '.']
// ``splitroot``'s relative part is returned in ``rel``.  The tail is the last
// of those components (or "" when there is none), which is what PurePath.name
// returns.
kimix::string_view bash_path_tail(kimix::string_view path) {
    // altsep -> sep: pathlib replaces '/' with '\\' before parsing, so both
    // characters act as separators everywhere below.
    const auto is_sep = [](char c) noexcept { return c == '\\' || c == '/'; };
    const auto find_sep = [&](size_t from) noexcept {
        for (size_t i = from; i < path.size(); ++i) {
            if (is_sep(path[i])) {
                return i;
            }
        }
        return kimix::string_view::npos;
    };
    // ntpath.splitroot, reduced to the parts that influence `rel`:
    //   '\\\\a\\b\\rel' is a UNC/device drive (drive = '\\\\a\\b', rel = ...),
    //   '\\rel' is a rooted relative path (rel = ...),
    //   'X:\\rel' / 'X:rel' carry a drive letter, everything else has no drive.
    size_t start = 0;
    if (!path.empty() && is_sep(path[0])) {
        if (path.size() >= 2 && is_sep(path[1])) {
            // UNC / device drive: the share (or device name) is mandatory.
            size_t scan = 2;
            static constexpr kimix::string_view k_unc = "\\\\?\\UNC\\";
            if (path.size() >= k_unc.size()) {
                bool is_unc = true;
                for (size_t i = 0; i < k_unc.size(); ++i) {
                    const char a = path[i];
                    const char b = k_unc[i];
                    const char na = is_sep(a) ? '\\' : bash_lower_ascii(a);
                    if (na != bash_lower_ascii(b)) {
                        is_unc = false;
                        break;
                    }
                }
                if (is_unc) {
                    scan = k_unc.size();
                }
            }
            const size_t index = find_sep(scan);
            if (index == kimix::string_view::npos) {
                return kimix::string_view();  // not a drive -> rel == ""
            }
            const size_t index2 = find_sep(index + 1);
            if (index2 == kimix::string_view::npos) {
                return kimix::string_view();  // not a drive -> rel == ""
            }
            start = index2 + 1;
        } else {
            start = 1;  // '/' or '\' root, rel follows
        }
    } else if (path.size() >= 2 && path[1] == ':') {
        start = (path.size() >= 3 && is_sep(path[2])) ? 3 : 2;
    }
    const kimix::string_view rel = path.substr(start);
    // Last component that is neither empty nor ".".
    kimix::string_view tail;
    size_t i = 0;
    while (i <= rel.size()) {
        size_t j = i;
        while (j < rel.size() && !is_sep(rel[j])) {
            ++j;
        }
        const kimix::string_view part = rel.substr(i, j - i);
        if (!part.empty() && part != ".") {
            tail = part;
        }
        i = j + 1;
    }
    return tail;
}

// Path(token.strip("\"'")).stem with a ".exe" strip, matching
// _rewrite_shell_segment's `name` computation:
//   token.strip("\"'") removes leading/trailing " and ' only;
//   Path(...).name is the Windows tail of the path (see bash_path_tail);
//   Path(...).stem drops the last extension, unless the part before the last
//   "." is empty or all dots (CPython 3.14: "the stem must contain at least
//   one non-dot character").
// Parity detail: `Path` is `WindowsPath` on the reference host, so both '/'
// and '\\' separate components and trailing separators / "." components are
// dropped ("git/" -> "git", "dir/git/." -> "git", but "\\\\git" -> "" because
// that is an incomplete UNC drive).  A naive "text after the last separator"
// scan got all of those wrong.
kimix::string bash_token_stem(kimix::string_view token) {
    size_t b = 0;
    size_t e = token.size();
    while (b < e && (token[b] == '"' || token[b] == '\'')) {
        ++b;
    }
    while (e > b && (token[e - 1] == '"' || token[e - 1] == '\'')) {
        --e;
    }
    const kimix::string_view s = token.substr(b, e - b);
    const kimix::string_view name = bash_path_tail(s);
    const size_t dot = name.rfind('.');
    if (dot != kimix::string_view::npos) {
        const kimix::string_view stem = name.substr(0, dot);
        bool has_non_dot = false;
        for (const char c : stem) {
            if (c != '.') {
                has_non_dot = true;
                break;
            }
        }
        if (has_non_dot) {
            return kimix::string(stem);
        }
    }
    return kimix::string(name);
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
// first shell separator (; && || | newline). Mirrors _segment_tokens
// (``re.split(r";|\|\||&&|\||\n", tail, maxsplit=1)[0]``): a *single* ``&``
// (background operator) is NOT a separator -- it stays in the token list.
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
        if (text[i] == '&' && i + 1 < text.size() && text[i + 1] == '&') {
            // Only `&&` separates; a lone `&` is not in the reference pattern.
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

// _looks_like_flag (60-67): `-...` or `/all-alpha` (Windows switch).
// ``token[1:].isalpha()`` requires *every* remaining character to be a letter,
// so paths such as ``/dev/sda`` are operands, not switches (ASCII gate).
bool bash_looks_like_flag(kimix::string_view token) noexcept {
    if (token.size() > 1 && token[0] == '-') {
        return true;
    }
    if (token.size() > 1 && token[0] == '/') {
        for (size_t i = 1; i < token.size(); ++i) {
            if (!bash_is_alpha(token[i])) {
                return false;
            }
        }
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
    // ``t.rstrip("/\\")`` is used for the ``~``/``$home`` test ONLY (it does not
    // mutate `t` in the reference); every later check sees the full token.
    kimix::string_view trimmed(lower);
    while (trimmed.size() > 1 && (trimmed.back() == '/' || trimmed.back() == '\\')) {
        trimmed.remove_suffix(1);
    }
    if (trimmed == "~" || trimmed == "$home") {
        return true;
    }
    const kimix::string_view lv(lower);
    // Windows drive root, optionally with one trailing separator and/or a
    // trailing glob: ``^[a-z]:[\\/]?(?:[\\/]?\*)?$`` (so ``c:``, ``c:/``,
    // ``c:\``, ``c:*``, ``c:/*`` and ``c:\*`` are protected, but ``c:**``,
    // ``c:*/`` or ``c://`` are not).
    if (lv.size() >= 2 && lv[1] == ':') {
        const kimix::string_view rest = lv.substr(2);
        const bool ok = rest.empty() || rest == "/" || rest == "\\" ||
                        rest == "*" || rest == "/*" || rest == "\\*";
        if (bash_is_alpha(lv[0]) && ok) {
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

// Find the next match of safety.py's command-word regex
// ``\b(?:name0|name1|...)(?:\.exe)?\b`` at or after ``from``.
//
// The reference scans the *text* with real word boundaries
// (``re.finditer(r"\b(rm|rmdir|del)(?:\.exe)?\b", text)``,
// ``re.finditer(r"\bkill(?:\.exe)?\b", text)``,
// ``re.finditer(r"\bformat(?:\.exe)?\b", text)``), so the command word is
// found after any non-word character.  A token-based scan (read a whitespace
// word, compare it) missed ``./rm -rf /``, ``/bin/rm -rf /``,
// ``sh -c 'rm -rf /'`` and friends, i.e. it let obfuscated commands through
// the hardline floor the reference blocks.
//
// ``name`` is the matched alternative (group 1), ``end`` the end of the whole
// match -- including the greedy optional ``.exe`` when the trailing ``\b``
// allows it (the engine backtracks into that group otherwise).  Both the
// resume position and the operand-token start use ``end``, exactly like
// ``match.end()`` in the reference.
struct bash_command_word_match {
    kimix::string_view name;  // group(1): empty when there is no match
    size_t pos = kimix::string_view::npos;
    size_t end = kimix::string_view::npos;
};

bash_command_word_match bash_find_command_word(kimix::string_view text, size_t from,
                                               const kimix::string_view *names,
                                               size_t name_count) {
    bash_command_word_match m;
    const size_t n = text.size();
    for (size_t i = from; i < n; ++i) {
        if (!bash_is_word_char(text[i])) {
            continue;  // `\b` needs a word character here
        }
        if (i > 0 && bash_is_word_char(text[i - 1])) {
            continue;  // left `\b` fails
        }
        for (size_t k = 0; k < name_count; ++k) {
            const kimix::string_view name = names[k];
            if (i + name.size() > n || text.substr(i, name.size()) != name) {
                continue;
            }
            const size_t base = i + name.size();
            // Greedy `(?:\.exe)?` first, then the empty alternative.
            if (base + 4 <= n && text.substr(base, 4) == ".exe" &&
                (base + 4 == n || !bash_is_word_char(text[base + 4]))) {
                m.name = name;
                m.pos = i;
                m.end = base + 4;
                return m;
            }
            if (base == n || !bash_is_word_char(text[base])) {
                m.name = name;
                m.pos = i;
                m.end = base;
                return m;
            }
        }
    }
    return m;
}

// _detect_recursive_delete (110-126): one finditer over the alternation
// ``\b(rm|rmdir|del)(?:\.exe)?\b``; the matched alternative selects the flag
// requirement.  The scan resumes at ``match.end()`` after every match
// (including the ones whose flags are insufficient), and the operand tokens
// start at ``match.end()`` too.
kimix::optional<kimix::string>
bash_detect_recursive_delete(kimix::string_view text) {
    static constexpr kimix::string_view k_names[] = {"rm", "rmdir", "del"};
    size_t pos = 0;
    for (;;) {
        const bash_command_word_match m =
            bash_find_command_word(text, pos, k_names, 3);
        if (m.pos == kimix::string_view::npos) {
            break;
        }
        pos = m.end;  // finditer() resumes after the whole match
        const auto tokens = bash_segment_tokens(text, m.end);
        const auto flags = bash_collect_flags(tokens);
        bool sufficient = false;
        if (m.name == "rm") {
            sufficient = bash_has_flag(flags, 'r') || bash_has_flag(flags, 'f');
        } else if (m.name == "rmdir") {
            sufficient = bash_has_flag(flags, 'r') || bash_has_flag(flags, 's');
        } else {  // "del"
            sufficient = bash_has_flag(flags, 'r') || bash_has_flag(flags, 'f') ||
                         bash_has_flag(flags, 's');
        }
        if (!sufficient) {
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

// safety.py rule 3 second half: ``\bof=/dev/(?:sd|nvme|disk|rdisk)[a-z0-9]*``.
// The regex needs a word boundary before ``of=`` and one of exactly those four
// device prefixes; the trailing ``[a-z0-9]*`` has no boundary, so any suffix
// (or none) is accepted.  The previous prefix test accepted ``hd``/``nv``/``rd``
// (substring prefixes of the reference alternatives) and ignored the ``\b``,
// which blocked commands the reference runs.
bool bash_dd_writes_raw_device(kimix::string_view text) noexcept {
    static constexpr kimix::string_view k_devices[] = {"sd", "nvme", "disk",
                                                       "rdisk"};
    const kimix::string_view needle = "of=/dev/";
    size_t pos = 0;
    for (;;) {
        const size_t found = text.find(needle, pos);
        if (found == kimix::string_view::npos) {
            return false;
        }
        pos = found + 1;
        if (found > 0 && bash_is_word_char(text[found - 1])) {
            continue;  // left `\b` fails (e.g. "xof=/dev/sda")
        }
        const kimix::string_view rest = text.substr(found + needle.size());
        for (const kimix::string_view device : k_devices) {
            if (bash_starts_with(rest, device)) {
                return true;
            }
        }
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

    // 3. dd writing to a raw device:
    // ``\bdd\b`` and ``\bof=/dev/(?:sd|nvme|disk|rdisk)[a-z0-9]*``.
    if (bash_has_word(text, "dd") && bash_dd_writes_raw_device(text)) {
        res.blocked = true;
        res.description = "`dd` writing to a raw device is blocked";
        return res;
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

    // 5. Fork bomb: `:(){ :|:& };` (`:\(\)\{` and the literal `:|:&`).
    if (text.find(":(){") != kimix::string::npos &&
        text.find(":|:&") != kimix::string::npos) {
        res.blocked = true;
        res.description = "Fork bomb pattern detected";
        return res;
    }

    // 6. kill targeting PID 1 or $PPID.
    {
        static constexpr kimix::string_view k_names[] = {"kill"};
        size_t pos = 0;
        for (;;) {
            const bash_command_word_match m =
                bash_find_command_word(text, pos, k_names, 1);
            if (m.pos == kimix::string_view::npos) {
                break;
            }
            pos = m.end;
            const auto tokens = bash_segment_tokens(text, m.end);
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
        }
    }

    // 7. Windows format on a drive letter.
    {
        static constexpr kimix::string_view k_names[] = {"format"};
        size_t pos = 0;
        for (;;) {
            const bash_command_word_match m =
                bash_find_command_word(text, pos, k_names, 1);
            if (m.pos == kimix::string_view::npos) {
                break;
            }
            pos = m.end;
            const auto tokens = bash_segment_tokens(text, m.end);
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

// One occurrence of the literal *needle* in *text* with the reference's ASCII
// ``\b`` boundaries on both ends (``text`` is already whitespace-collapsed, so
// ``\s+`` inside a caller's pattern is exactly one space).
bool bash_find_bounded(kimix::string_view text, kimix::string_view needle) noexcept {
    size_t from = 0;
    for (;;) {
        const size_t at = text.find(needle, from);
        if (at == kimix::string_view::npos) {
            return false;
        }
        from = at + 1;
        if (at > 0 && bash_is_word_char(text[at - 1])) {
            continue;
        }
        const size_t after = at + needle.size();
        if (after >= text.size() || !bash_is_word_char(text[after])) {
            return true;
        }
    }
}

// _LONG_RUNNING_PATTERNS (safety.py 227-240), evaluated as the reference does:
// ``re.search(pattern, text)`` over the whitespace-collapsed, quote-stripped
// command.  Every pattern has real ``\b`` boundaries, so the keywords match
// inside longer tokens too (``vite.dev``, ``./node_modules/.bin/vite``), while
// ``yarn dev`` / ``npm start`` -- which the reference patterns, all requiring
// ``run``, do NOT match -- must not trigger the hint.
bool bash_is_long_running(kimix::string_view text) noexcept {
    // \b(?:npm|pnpm|yarn|bun)\s+run\s+(?:dev|start|serve|watch)\b
    static constexpr kimix::string_view k_runners[] = {"npm", "pnpm", "yarn", "bun"};
    static constexpr kimix::string_view k_verbs[] = {"dev", "start", "serve", "watch"};
    for (const kimix::string_view runner : k_runners) {
        for (const kimix::string_view verb : k_verbs) {
            kimix::string needle(runner);
            needle += " run ";
            needle += verb;
            const bool hit = bash_find_bounded(text, needle);
            if (hit) {
                return true;
            }
        }
    }
    // \bnext\s+dev\b
    if (bash_find_bounded(text, "next dev")) {
        return true;
    }
    // \bvite\b | \bnodemon\b | \buvicorn\b | \bgunicorn\b | \bnohup\b | \bsetsid\b
    static constexpr kimix::string_view k_words[] = {
        "vite", "nodemon", "uvicorn", "gunicorn", "nohup", "setsid"};
    for (const kimix::string_view word : k_words) {
        if (bash_find_bounded(text, word)) {
            return true;
        }
    }
    // \bpython\s+-m\s+http\.server\b
    if (bash_find_bounded(text, "python -m http.server")) {
        return true;
    }
    // \bdocker\s+compose\s+up\b | \bdocker-compose\s+up\b
    if (bash_find_bounded(text, "docker compose up") ||
        bash_find_bounded(text, "docker-compose up")) {
        return true;
    }
    // &\s*$ (the collapsed text has no trailing whitespace).
    if (!text.empty() && text.back() == '&') {
        return true;
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
    if (bash_is_long_running(text)) {
        return kimix::string(
            "Long-running command detected; use `job_output` to wait for it or to stop it.");
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
            "Verify the path with `glob`/`read`.");
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

// Fuzzy alias matching (tool.h): the alternate argument names the model may
// send instead of the documented one (`command` for `cmd`, ...). The canonical
// name always wins; `cmd` as well keeps its explicit fallback below.
static const kimix::builtin_tools::param_alias k_bash_aliases[] = {
    {"cmd", "command cmdline command_line shell_command cmd_string"},
    {"mode", "execution_mode run_mode"},
    {"timeout", "timeout_seconds timeout_sec"},
    {"task_id", "job_id job task"},
    {"wait_for_pattern", "wait_pattern pattern wait_for wait_until"},
    {"max_lines", "max_output_lines output_lines lines"},
    {"output_path", "output output_file save_path out_path"},
};

tool_error parse_bash_params(const kimix::builtin_tools::ToolParams *params,
                             bash_params &out) {
    using kimix::builtin_tools::ToolParams;
    using kimix::builtin_tools::ValueElement;
    // Fuzzy alias matching (tool.h): wrong-but-reasonable argument names
    // ("command" for "cmd") are accepted; the canonical name always wins.
    const ToolParams k_resolved = ToolParams::with_aliases(params, k_bash_aliases);
    if (params != nullptr) {
        params = &k_resolved;
    }
    out = bash_params{};
    if (params == nullptr) {
        return {tool_status::invalid_input, "missing parameters"};
    }
    // mode (optional, default "execute"). Parsed before cmd because cmd
    // presence depends on it (fresh interactive starts may omit cmd).
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

    // cmd (required, alias "command"). Reference parity
    // (prompt_common.shell_cmd_required_validator): cmd may be ABSENT only
    // for a FRESH interactive start (no task_id); execute mode and session
    // continuations still require it. An empty string is left to run()'s
    // existing "Empty command." guard for non-interactive modes.
    const ValueElement *cmd_elem = params->get("cmd");
    if (cmd_elem == nullptr) {
        cmd_elem = params->get("command");
    }
    const bool fresh_interactive =
        out.mode == "interactive" && params->get("task_id") == nullptr;
    if (cmd_elem == nullptr || !cmd_elem->is_string()) {
        if (fresh_interactive) {
            out.cmd.clear();
        } else {
            return {tool_status::invalid_input, "missing required string field 'cmd'"};
        }
    } else {
        out.cmd = cmd_elem->as_string();
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

    // output_path (optional string, execute-mode tee).
    const ValueElement *output_path_elem = params->get("output_path");
    if (output_path_elem != nullptr) {
        if (!output_path_elem->is_string()) {
            return {tool_status::invalid_input, "field 'output_path' must be a string"};
        }
        out.output_path = output_path_elem->as_string();
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

Bash::Bash(kimix::builtin_tools::Session *session) : Tool(session) {
    _cfg.bash_path = detect_bash_path();
    _cfg.self_kill_guard_enabled = false; // no Python-resolved pid identity
}

bool Bash::valid() const {
    // The registry constructor resolved `_cfg.bash_path`; a shim-supplied
    // config may leave it empty ("" == auto-detect), so probe again. This is a
    // pure existence check: no process is spawned. On Windows the probe only
    // accepts a real Git Bash / MSYS2 / Cygwin install, so a machine without
    // one gets an invalid bash tool and the soul routes the shell through
    // pwsh instead (see KimiSoul::effective_shell_tool).
    const bool shell_found =
        !_cfg.bash_path.empty() || !detect_bash_path().empty();
    return tool_valid("bash", shell_found);
}

kimix::string Bash::detect_bash_path() {
    namespace fs = kimix::filesystem;
#ifdef KIMIX_PLATFORM_WINDOWS
    // Git Bash first (the same policy as the Python reference), then MSYS2.
    static const char *kWindowsCandidates[] = {
        "C:/Program Files/Git/bin/bash.exe",
        "C:/Program Files (x86)/Git/bin/bash.exe",
        "C:/msys64/usr/bin/bash.exe",
        "C:/msys64/bin/bash.exe",
        "C:/cygwin64/bin/bash.exe",
    };
    for (const char *c : kWindowsCandidates) {
        std::error_code ec;
        if (fs::exists(fs::path(c), ec)) {
            return kimix::string(c);
        }
    }
    // LocalAppData Git install (winget/scoop layouts).
    if (const char *la = std::getenv("LOCALAPPDATA")) {
        std::error_code ec;
        fs::path p = fs::path(la) / "Programs/Git/bin/bash.exe";
        if (fs::exists(p, ec)) {
            return kimix::to_string(p);
        }
    }
    return {};
#else
    static const char *kPosixCandidates[] = {"/bin/bash", "/usr/bin/bash",
                                             "/usr/local/bin/bash"};
    for (const char *c : kPosixCandidates) {
        std::error_code ec;
        if (fs::exists(fs::path(c), ec)) {
            return kimix::string(c);
        }
    }
    if (const char *path_env = std::getenv("PATH")) {
        kimix::string_view rest(path_env);
        while (!rest.empty()) {
            const size_t colon = rest.find(':');
            const kimix::string_view dir =
                (colon == kimix::string_view::npos) ? rest : rest.substr(0, colon);
            if (!dir.empty()) {
                std::error_code ec;
                fs::path cand = fs::path(kimix::string(dir)) / "bash";
                if (fs::exists(cand, ec)) {
                    return kimix::to_string(cand);
                }
            }
            if (colon == kimix::string_view::npos) {
                break;
            }
            rest.remove_prefix(colon + 1);
        }
    }
    return {};
#endif
}

namespace {

// Resolve the working directory for a native spawn: session work_dir when set,
// else empty (inherit the parent process cwd).
kimix::string bash_native_cwd(const kimix::builtin_tools::Session *session) {
    if (session == nullptr) {
        return {};
    }
    return session->work_dir;
}

// Build the child environment deltas for a native bash spawn (mirrors
// _bash_subprocess_env: MSYS path-conversion opt-out on Windows). MSYSTEM is
// deliberately NOT neutralized here - the Git Bash launcher injects
// MSYSTEM=MINGW64 after the environment is applied and the MSYS2 runtime
// re-injects the variable into children when it is absent, so the reference
// neutralizes it through the command prefix instead (bash_spawn_script).
  kimix::vector<kimix::string> bash_native_env() {
      kimix::vector<kimix::string> env;
  #ifdef KIMIX_PLATFORM_WINDOWS
      env.push_back("MSYS_NO_PATHCONV=1");
      env.push_back("MSYS2_ARG_CONV_EXCL=*");
  #endif
      return env;
  }

  // The quiet-idle bound of an interactive REPL read (ms): output arrived
  // and then the child went silent for this long, so the command finished
  // printing. The fixed full-timeout wait made every send burn the whole
  // 30 s bound (bug_tool.md item 3: a 3-command session took ~90s).
  constexpr int64_t k_repl_quiet_ms = 1200;

    // Clean up the captured output of one interactive turn.
    //  - Drop the MSYS job-control banner (first turn only): the shell
    //    cannot set a terminal process group over pipes, and repeating that
    //    warning every turn is noise (bug_tool.md item 3's spurious banner).
    //  - Drop the echo of the command that was just sent: with no tty bash
    //    echoes each stdin line (with its prompt prefix) before running it,
    //    polluting the result with the command text (bug_tool.md minor (b)).
    // Only an echoed line whose text equals the sent command is dropped, so
    // real command output is never touched.
    void bash_repl_strip_turn_noise(const kimix::string &sent_cmd,
                                    bool first_turn, kimix::string &out) {
        static const kimix::string k_banner_a =
            "cannot set terminal process group";
        static const kimix::string k_banner_b = "no job control in this shell";
        kimix::string kept;
        size_t pos = 0;
        bool stripped_echo = sent_cmd.empty();
        while (pos <= out.size()) {
            const size_t nl = out.find('\n', pos);
            const size_t stop = nl == kimix::string::npos ? out.size() : nl;
            kimix::string_view line(out.data() + pos, stop - pos);
            if (!line.empty() && line.back() == '\r') {
                line = line.substr(0, line.size() - 1);
            }
            bool drop = false;
            if (first_turn &&
                (line.find(k_banner_a) != kimix::string_view::npos ||
                 line.find(k_banner_b) != kimix::string_view::npos)) {
                drop = true;
            } else if (!stripped_echo) {
                kimix::string_view payload = line;
                const size_t dollar = payload.rfind("$ ");
                const size_t hash = payload.rfind("# ");
                size_t cut = kimix::string_view::npos;
                if (dollar != kimix::string_view::npos &&
                    hash != kimix::string_view::npos) {
                    cut = dollar > hash ? dollar : hash;
                } else if (dollar != kimix::string_view::npos) {
                    cut = dollar;
                } else {
                    cut = hash;
                }
                if (cut != kimix::string_view::npos &&
                    cut + 2 <= payload.size()) {
                    payload = payload.substr(cut + 2);
                }
                if (payload == kimix::string_view(sent_cmd)) {
                    stripped_echo = true;
                    drop = true;
                }
            }
            if (!drop) {
                if (!kept.empty()) {
                    kept.push_back('\n');
                }
                kept.append(line.data(), line.size());
            }
            if (nl == kimix::string::npos) {
                break;
            }
            pos = nl + 1;
        }
        out = std::move(kept);
    }

  } // namespace

bool bash_is_git_bash_install(kimix::string_view bash_path) noexcept {
    // _is_git_bash_install (bash_tool.py:276-307). Windows-only: real MSYS2
    // installs have no <root>/cmd/git.exe marker, so neutralization stays
    // limited to Git Bash and never affects real MSYS2 shells.
#ifndef KIMIX_PLATFORM_WINDOWS
    // The reference's `sys.platform == "win32"` gate: no other host injects
    // MSYSTEM, so there is nothing to neutralize.
    (void)bash_path;
    return false;
#else
    namespace fs = kimix::filesystem;
    if (bash_path.empty()) {
        return false;
    }
    // ntpath.normpath: forward slashes to backslashes, then split the drive.
    kimix::string text(bash_path);
    for (char &c : text) {
        if (c == '/') {
            c = '\\';
        }
    }
    const size_t drive_end = text.find(':');
    if (drive_end == kimix::string::npos || drive_end + 1 >= text.size()) {
        return false;
    }
    kimix::string drive = text.substr(0, drive_end + 1); // "C:"
    kimix::string tail = text.substr(drive_end + 1);
    // parts = [p.lower() for p in tail.split("\\") if p]
    kimix::vector<kimix::string> parts;
    {
        size_t start = 0;
        while (start <= tail.size()) {
            size_t stop = tail.find('\\', start);
            if (stop == kimix::string::npos) {
                stop = tail.size();
            }
            if (stop > start) {
                kimix::string part = tail.substr(start, stop - start);
                for (char &c : part) {
                    c = bash_lower_ascii(c);
                }
                parts.push_back(std::move(part));
            }
            if (stop == tail.size()) {
                break;
            }
            start = stop + 1;
        }
    }
    // expect either ...\usr\bin\bash.exe or ...\bin\bash.exe
    if (parts.size() < 3 || parts[parts.size() - 1] != "bash.exe" ||
        parts[parts.size() - 2] != "bin") {
        return false;
    }
    const bool usr_layout = parts[parts.size() - 3] == "usr";
    const size_t root_parts = usr_layout ? parts.size() - 3 : parts.size() - 2;
    kimix::string root;
    for (size_t i = 0; i < root_parts; ++i) {
        root.push_back('\\');
        root.append(parts[i]);
    }
    // Anchor the drive: ntpath.join(drive, root, ...) would produce a
    // drive-relative path ("C:foo") that Windows resolves against the
    // per-drive current directory, making the marker lookup CWD-dependent.
      kimix::string marker = drive;
      marker.push_back('\\');
      marker.append(root);
      marker += "\\cmd\\git.exe";
      std::error_code ec;
      // No narrow path constructor (throws std::system_error on bytes the ANSI
      // code page cannot represent; fatal without exceptions).
      fs::path marker_path;
      if (!kimix::path_from_narrow(marker, marker_path)) {
          kimix::path_from_utf8(marker, marker_path);
      }
      return fs::is_regular_file(marker_path, ec);
#endif
}

kimix::string bash_spawn_script(kimix::string_view bash_path,
                                kimix::string_view command) {
    // _with_msystem_neutralized(_PIPEFAIL_PREFIX + cmd, bash_path)
    // (bash_tool.py:307-329, 886): the MSYSTEM statement only for a Git for
    // Windows install, pipefail everywhere, no stderr suppression (a shell
    // that cannot set pipefail must surface the failure).
    kimix::string script;
    if (bash_is_git_bash_install(bash_path)) {
        script = "export MSYSTEM=; ";
    }
    script += "set -o pipefail; ";
    script.append(command.data(), command.size());
    return script;
}

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
    // The command is handed to the shell as written - the Windows Git Bash
    // compatibility fix is gone, so no fallback definitions, path rewrites or
    // unsupported-command rejections are applied here (only the MSYSTEM
    // neutralization in bash_spawn_script survives). The prepared command is
    // returned in the message so the Python binding can hand it to the
    // subprocess.
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

void Bash::operator()(const kimix::builtin_tools::ToolParams *parameters,
                      kimix::string &display_str) {
    const kimix::builtin_tools::tool_display_scope k_display{
        *this, display_str};
    const kimix::builtin_tools::tool_output_spill_scope k_spill{*this, _result};
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

    // Native execution (reproc): when the session requests native_io and the
    // safety floors passed, `output_block` holds the prepared command; run it
    // for real through the async poll/drain process runner and rebuild the
    // output block from the captured stream.
    const bool native_io = (_session != nullptr && _session->native_io &&
                            _cfg.native_execute && err.status == tool_status::ok);
    if (native_io && params.mode == "execute") {
        const kimix::string bash_path =
            _cfg.bash_path.empty() ? detect_bash_path() : _cfg.bash_path;
        if (bash_path.empty()) {
            err = {tool_status::unsupported,
                   "no bash executable found on this system"};
            output_block =
                bash_build_blocked_block(params, "unsupported", err.message);
        } else {
            proc::run_options opts;
            opts.argv.push_back(bash_path);
            opts.argv.push_back("--noprofile");
            opts.argv.push_back("--norc");
            opts.argv.push_back("-c");
            // bash_tool.py:886: _with_msystem_neutralized(_PIPEFAIL_PREFIX +
            // rtk_cmd, self._bash) - MSYSTEM neutralized INSIDE the command on
            // a Git for Windows install (the child-env spelling does not
            // stick), pipefail with no stderr suppression.
            opts.argv.push_back(bash_spawn_script(bash_path, output_block));
            opts.working_directory = bash_native_cwd(_session);
            opts.extra_env = bash_native_env();
            opts.timeout_ms = params.timeout > 0 ? params.timeout * 1000 : 0;
            opts.output_cap_chars = 200000;
            if (params.wait_for_pattern.has_value()) {
                opts.wait_pattern = *params.wait_for_pattern;
            }
            const proc::run_result rr = proc::run_process(opts);
            if (!rr.spawn_error.empty()) {
                err = {tool_status::invalid_input, rr.spawn_error};
                output_block =
                    bash_build_blocked_block(params, "invalid_input", err.message);
              } else {
                  // F1b: flag the max_lines fold too (python parity) - the
                  // raw runner truncation (rr.truncated) and the line fold
                  // are separate limits and callers gate re-reads on this
                  // flag.
                  const int64_t fold_bound = params.max_lines.value_or(500);
                  bool max_lines_folded = false;
                  if (fold_bound > 0) {
                      int64_t n_lines = 0;
                      for (const char c : rr.output) {
                          n_lines += (c == '\n');
                      }
                      if (!rr.output.empty() && rr.output.back() != '\n') {
                          ++n_lines;
                      }
                      max_lines_folded = n_lines > fold_bound;
                  }
                  kimix::string out =
                      truncate_lines(rr.output, fold_bound, true, 2);
                  // F-new-10: honor output_path like the python tool does -
                  // the raw (pre-fold) output is teed to the file and the
                  // resolved path is echoed in the block. Before this the
                  // param was silently dropped and the envelope echoed
                  // output_path: null on every run.
                  kimix::string resolved_output_path;
                  bool output_outside_work_dir = false;
                  if (!params.output_path.empty()) {
                      namespace fs = kimix::filesystem;
                      // No narrow path constructor: it converts through the
                      // ANSI code page and THROWS std::system_error on bytes
                      // it cannot represent (fatal with C++ exceptions
                      // disabled). The argument arrives UTF-8, work_dir
                      // follows the CLI's ANSI/lossy convention; a failed
                      // conversion simply skips the tee.
                      fs::path op;
                      if (!kimix::path_from_utf8(params.output_path, op)) {
                          kimix::path_from_narrow(params.output_path, op);
                      }
                      if (op.is_relative() && _session != nullptr &&
                          !_session->work_dir.empty()) {
                          fs::path wd;
                          if (kimix::path_from_narrow(_session->work_dir, wd) ||
                              kimix::path_from_utf8(_session->work_dir, wd)) {
                              op = wd / op;
                          }
                      }
                      std::error_code ec;
                      const fs::path parent = op.parent_path();
                      if (!parent.empty()) {
                          fs::create_directories(parent, ec);
                      }
                      if (std::FILE *of =
                              std::fopen(kimix::to_string(op).c_str(), "wb");
                          of != nullptr) {
                          std::fwrite(rr.output.data(), 1, rr.output.size(), of);
                          std::fclose(of);
                          resolved_output_path = kimix::to_string(op);
                          // python parity (F-new-2): the write happens as
                          // requested, but a path escaping the work dir earns
                          // a warning in the success message.
                          if (_session != nullptr && !_session->work_dir.empty()) {
                              fs::path wd_raw;
                              if (!kimix::path_from_narrow(_session->work_dir,
                                                           wd_raw)) {
                                  kimix::path_from_utf8(_session->work_dir,
                                                        wd_raw);
                              }
                              const fs::path wd = wd_raw.lexically_normal();
                              const fs::path opn = op.lexically_normal();
                              std::error_code rec;
                              const fs::path rel = fs::relative(opn, wd, rec);
                              output_outside_work_dir =
                                  rec || rel.empty() || *rel.begin() == "..";
                          }
                      }
                  }
                kimix::string status_str = "completed";
                kimix::optional<kimix::string> meaning;
                kimix::optional<kimix::string> hint;
                if (rr.killed) {
                    status_str = "timeout";
                } else if (rr.exit_code.has_value() && *rr.exit_code != 0) {
                    status_str = "failed";
                    if (!is_expected_exit(output_block, rr.exit_code)) {
                        meaning = interpret_exit_code(output_block, rr.exit_code);
                        hint = annotate_failure(rr.output, output_block, rr.exit_code);
                        const kimix::optional<int64_t> eline =
                            find_error_line_index(rr.output);
                        out += process_exited_banner(*rr.exit_code, eline);
                    }
                }
                python::session_output_block block;
                block.task_id = params.task_id.value_or("bash");
                block.status = status_str;
                block.output = out;
                block.exit_code = rr.exit_code.has_value()
                                      ? std::optional<int32_t>(
                                            static_cast<int32_t>(*rr.exit_code))
                                      : std::nullopt;
                block.exit_code_meaning = meaning;
                block.failure_hint = hint;
                block.wait_matched =
                    rr.matched ? std::optional<bool>(true) : std::nullopt;
                block.elapsed_seconds =
                    static_cast<double>(rr.elapsed_ms) / 1000.0;
                                  block.output_truncated = rr.truncated || max_lines_folded;
                if (!resolved_output_path.empty()) {
                    block.output_path = resolved_output_path;
                    if (output_outside_work_dir) {
                        err.message +=
                            " Warning: output_path is outside the session work dir.";
                    }
                }
                output_block = python::build_session_output_block(block);
            }
        }
    } else if (native_io &&
               (params.mode == "interactive" || params.mode == "send")) {
        // Long-lived REPL task driven through the interactive task registry.
        if (params.mode == "interactive" && !params.task_id.has_value()) {
            const kimix::string bash_path =
                _cfg.bash_path.empty() ? detect_bash_path() : _cfg.bash_path;
            if (bash_path.empty()) {
                err = {tool_status::unsupported,
                       "no bash executable found on this system"};
                output_block =
                    bash_build_blocked_block(params, "unsupported", err.message);
            } else {
                proc::run_options opts;
                opts.argv.push_back(bash_path);
                opts.argv.push_back("--noprofile");
                opts.argv.push_back("--norc");
                opts.argv.push_back("-i");
                opts.working_directory = bash_native_cwd(_session);
                opts.extra_env = bash_native_env();
                opts.timeout_ms = 0;
                proc::task_handle handle;
                err = proc::start_task(opts, handle);
                if (err.failed()) {
                    output_block = bash_build_blocked_block(
                        params, "invalid_input", err.message);
                } else {
                    params.task_id = handle.task_id;
                      // (a) The startup command must actually RUN in the
                      // spawned shell and its output be captured
                      // (bug_tool.md item 3: a start command only ever
                      // showed "interactive bash started (pid ...)" - it
                      // was never executed, so its output was lost and its
                      // `export`s did not persist). Send it through the
                      // same stdin the later turns use, then capture the
                      // output with the quiet-idle bound instead of a
                      // fixed full-timeout wait.
                      kimix::string repl_out;
 std::optional<bool> start_wait_matched;
 std::optional<double> start_elapsed_s;
 if (!params.cmd.empty()) {
 if (!proc::send_task(handle.task_id, params.cmd,
 true).failed()) {
 // e2e pass 10 (P10-new-C): an explicit wait_for_pattern
 // must bound the START read too (reference bash_tool.py
 // start path waits with the pattern and reports
 // wait_matched), not only the send path. The pattern
 // opts into blocking until it appears, so quiet-idle
 // stays disabled, mirroring the send branch below.
 const bool want_pattern = params.wait_for_pattern.has_value();
 const int64_t wait_ms =
 params.timeout > 0 ? params.timeout * 1000
 : (want_pattern ? 30000 : 5000);
 const int64_t quiet_ms =
 want_pattern ? 0 : k_repl_quiet_ms;
 const proc::task_wait_result tw = proc::wait_task_quiet(
 handle.task_id, params.wait_for_pattern.value_or(""),
 wait_ms, quiet_ms);
 if (tw.matched) {
 start_wait_matched = true;
 }
 start_elapsed_s = static_cast<double>(tw.elapsed_ms) / 1000.0;
 kimix::string raw;
 proc::read_task(handle.task_id, raw);
 // First turn: drop the MSYS banner and the
 // echo of the startup command (minor (b)).
 bash_repl_strip_turn_noise(
 params.cmd, true, raw);
 repl_out = truncate_lines(raw, params.max_lines.value_or(500), true, 2);
 }
 }
 python::session_output_block block;
 block.task_id = handle.task_id;
 block.status = "running";
 block.wait_matched = start_wait_matched;
 block.elapsed_seconds = start_elapsed_s;
 kimix::string block_body = kimix::format(
 "interactive bash started (pid {})", handle.pid);
 if (!repl_out.empty()) {
 block_body += "\n";
 block_body += repl_out;
 }
block.output = block_body;
                      output_block = python::build_session_output_block(block);
                    // The task id must be VISIBLE to the model (bug_tool.md
                    // item 1: the interactive task was unmanageable because
                    // neither message nor output carried the task_id).
                    err.message = kimix::format(
                        "Interactive bash started. task_id: `{}`. Use task_id to "
                        "send commands and job_output to read results. Send 'exit' "
                        "to close the session.",
                        handle.task_id);
                }
            }
        } else if (params.task_id.has_value()) {
            const kimix::string &tid = *params.task_id;
            if (params.mode == "send" && !params.cmd.empty()) {
                err = proc::send_task(tid, params.cmd, true);
            }
            if (!err.failed()) {
                // (c) Return as soon as the command's output has flushed (quiet-idle)
 // instead of burning the full timeout every send (bug_tool.md item 3:
 // a 3-command session took ~90s because each send blocked the whole
 // 30s bound). The `timeout` param stays the worst-case bound.
 const int64_t wait_ms =
 params.timeout > 0 ? params.timeout * 1000
 : (params.wait_for_pattern.has_value() ? 30000 : 5000);
 // An explicit wait_for_pattern opts into blocking until the pattern
 // appears, so quiet-idle stays disabled there; a plain send returns as
 // soon as the command's output has flushed.
 const int64_t quiet_ms = params.wait_for_pattern.has_value()
 ? 0
 : k_repl_quiet_ms;
 const proc::task_wait_result tw = proc::wait_task_quiet(
 tid, params.wait_for_pattern.value_or(""), wait_ms, quiet_ms);
                kimix::string out;
                proc::read_task(tid, out);
                // Drop the echo of the command just sent (bug_tool.md
                // minor (b): the echoed command polluted every turn's log).
                if (params.mode == "send") {
                    bash_repl_strip_turn_noise(params.cmd, false, out);
                }
                // F1b: see the execute path - a max_lines fold must flip
                // output_truncated just like the python tool does.
                const int64_t fold_bound = params.max_lines.value_or(500);
                bool max_lines_folded = false;
                if (fold_bound > 0) {
                    int64_t n_lines = 0;
                    for (const char c : out) {
                        n_lines += (c == '\n');
                    }
                    if (!out.empty() && out.back() != '\n') {
                        ++n_lines;
                    }
                    max_lines_folded = n_lines > fold_bound;
                }
                out = truncate_lines(out, fold_bound, true, 2);
                const proc::task_status_info info = proc::query_task(tid);
                python::session_output_block block;
                block.task_id = tid;
                block.status = tw.exited ? "completed" : "running";
                block.output = out;
                if (tw.exited && info.exit_code.has_value()) {
                    block.exit_code = static_cast<int32_t>(*info.exit_code);
                }
                block.wait_matched =
                    tw.matched ? std::optional<bool>(true) : std::nullopt;
                block.elapsed_seconds =
                    static_cast<double>(tw.elapsed_ms) / 1000.0;
                block.output_truncated = max_lines_folded;
                output_block = python::build_session_output_block(block);
            } else {
                output_block =
                    bash_build_blocked_block(params, "invalid_input", err.message);
            }
        } else {
            err = {tool_status::invalid_input, "mode 'send' requires a task_id"};
            output_block =
                bash_build_blocked_block(params, "invalid_input", err.message);
        }
    }

    result.values["status"] =
        ValueElement::make_string(kimix::string(bash_status_string(err.status)));
    result.values["message"] = ValueElement::make_string(err.message);
    result.values["output_block"] = ValueElement::make_string(output_block);
    // The model-visible output channel (bug_tool.md item 1: the soul renders
    // only status/message/output, so the captured stdout / the task block
    // never reached the model and every execute call answered just
    // "Command ready for execution"). Surface the native session block under
    // "output" too; the legacy prepared-command path keeps "command".
    if (native_io) {
        result.values["output"] = ValueElement::make_string(output_block);
    }
    if (!native_io && err.status == tool_status::ok &&
        params.mode == "execute" && !output_block.empty()) {
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

    // CLI display line: what the shell call amounts to, never the captured
    // output (the terminal prints this line IN PLACE of the output block).
    {
        kimix::string line;
        if (err.status != tool_status::ok) {
            tool_display_append(line, bash_status_string(err.status));
        }
        tool_display_append(line, err.message);
        if (params.task_id.has_value()) {
            tool_display_append(line,
                                kimix::format("task_id: {}", *params.task_id));
        }
        tool_display_append(line, tool_display_size(output_block));
        tool_display_finish(line);
        if (!line.empty()) {
            display_str = std::move(line);
        }
    }
}

const kimix::vector<char> &Bash::serialized_result() const {
    return _result;
}


  // Static registration: the registry key is the lowercase "bash" (the
  // class-name spellings "Bash"/"shell"/"sh" are declared aliases); see
  // tool_registry.h. The schema mirrors the Python BashParams model.
KIMIX_REGISTER_TOOL_NAMED_ALIASED(
    Bash, "bash",
    "Execute a shell command with the system bash (native POSIX syntax). "
    "Modes: 'execute' (bounded foreground run), 'send' (write to a running "
    "interactive task), 'interactive' (start a persistent REPL task).",
    R"JSON({"type":"object","properties":{"cmd":{"type":"string","description":"Shell command to run (POSIX syntax)"},"mode":{"type":"string","enum":["execute","send","interactive"],"description":"execute: run now; send: write to task stdin; interactive: start persistent task"},"timeout":{"type":"integer","description":"Timeout in seconds (default 30)"},"task_id":{"type":"string","description":"Task id for send/interactive continuation"},"wait_for_pattern":{"type":"string","description":"Stop waiting when this literal appears in output"},"max_lines":{"type":"integer","description":"Max output lines to return"},"output_path":{"type":"string","description":"Save captured output to this file (execute mode)"}},"required":["cmd"]})JSON",
    "Bash shell Shell sh");

} // namespace kimix::builtin_tools::bash
