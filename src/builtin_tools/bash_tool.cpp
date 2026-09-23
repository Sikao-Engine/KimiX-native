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
// Windows Git Bash compatibility fix (bash_fix.py / _shell_compat.py)
// ===========================================================================
// Port of the kimi-agent BashFix scanner (_shell_compat.py, the canonical
// pure-Python reference mirrored by bash_fix.py). It rewrites a native POSIX
// bash command line so it also runs under Git for Windows:
//
//   * native-command fallbacks   ``rev``/``tree``/``wget``/``free``/grep for
//                                the headless POSIX userland -> a shell
//                                function definition prefix (exported with
//                                ``export -f``) plus a standalone
//                                ``/usr/bin/bash -c`` runner when the command
//                                word is an operand of an exec-ing wrapper
//                                (``timeout 5 rev``, ``xargs rev``).
//   * Windows paths              ``D:\x\y`` -> ``D:/x/y``, ``\\srv\sh`` ->
//                                ``//srv/sh``, cmd.exe ``cd /d`` flag dropped.
//   * Git Bash virtual paths     ``/tmp/x`` -> the real Windows temp dir,
//                                ``/c/x`` -> ``C:/x``.
//   * null-device redirection    unquoted ``> nul`` -> ``>/dev/null``.
//   * redundant shell wrappers   ``bash cd /c/x && ...`` -> ``cd C:/x && ...``
//                                and ``bash -c '<script>'`` -> ``<script>``
//                                (the script is rescanned in place).
//   * unsupported commands       commands with no faithful Git Bash
//                                equivalent (``journalctl``) are recorded so
//                                the tool refuses to run them and reports the
//                                reason instead of Bash's "command not found".
//
// Faithfulness notes (everything below mirrors the reference line by line):
//
// 1. ASCII gate. The reference operates on Python ``str``; the native kernel
//    implements the identical algorithm for the ASCII subset (its
//    ``.lower()``/``.casefold()``/``.isalpha()``/``.isdigit()`` calls reduce to
//    the ASCII predicates for ASCII text, and the ``_PATH_*`` classes only
//    match ASCII). Non-ASCII input is reported through
//    ``tool_status::unsupported`` so the caller routes it to the Python mirror,
//    matching the project-wide ASCII-gate convention.
// 2. Nesting bound (documented deviation). The reference bounds
//    ``_scan_range``/``_find_matching`` at ``_MAX_NESTING_DEPTH`` (1024) and
//    returns the command unchanged when Python's own recursion limit fires
//    (``RecursionError``). The port keeps the same 1024 depth bound but
//    additionally abandons the scan as soon as the recursion has consumed a
//    fixed stack budget (== the ``RecursionError`` outcome: the command is
//    returned byte-for-byte), because a C++ frame chain costs orders of
//    magnitude more stack than Python's heap-allocated frames (measured: ~5 KiB
//    per ``$( )`` level in an unoptimized MSVC build). The budget covers ~75
//    nested substitutions unoptimized (several hundred optimized), so the
//    reference's deeper nesting cases (its suite exercises 250 levels) are left
//    byte-for-byte for Bash instead of being repaired. Adversarial nesting can
//    never overflow the caller's stack.
// 3. Temp directory. ``_windows_temp_dir()`` probes ``TMPDIR``/``TEMP``/``TMP``
//    and falls back to the Win32 temp path (``/tmp`` on POSIX); the reference
//    additionally validates candidate writability. Callers may inject the
//    directory (the golden tests do) so vectors stay machine independent.
//
// Generated data tables (``_FALLBACK_BODIES`` / ``_UNSUPPORTED_BODIES``) are
// spliced in between the GENERATED markers below; see scripts/gen_bash_fix_data.py.

namespace {

// ---------------------------------------------------------------------------
// Fallback bodies and unsupported-command reasons
// ---------------------------------------------------------------------------

struct bash_fix_fallback_body {
    const char *name;
    const char *body;
};

struct bash_fix_unsupported_reason {
    const char *name;
    const char *reason;
};

// >>> GENERATED:BASH-FIX-DATA >>>
// GENERATED by scripts/gen_bash_fix_data.py from kimi-agent's
// bin/kimix_native/_shell_compat.py - DO NOT EDIT BY HAND.
//
// _FALLBACK_BODIES (88 entries: reference insertion order is preserved
// because _FALLBACKS - and therefore the exported definitions prefix and
// bash_compatibility_prelude() - iterate in that order) and
// _UNSUPPORTED_BODIES (commands with no faithful Git Bash equivalent).
const bash_fix_fallback_body k_bash_fix_fallback_bodies[] = {
    {"gtimeout", "timeout \"$@\""},
    {"rev", "local __kimix_zero=0; while (( $# )); do case $1 in -0|--zero) __kimix_zero=1; shift;; --) shift; break;; -*) printf '%s\\n' \"rev: unsupported option: $1\" >&2; return 1;; *) break;; esac; done; perl '-Mopen=:std,:encoding(UTF-8)' -e 'my $zero = shift @ARGV; my $failed = 0; sub reverse_fh { my ($fh, $zero) = @_; local $/ = $zero ? qq(\\0) : qq(\\n); while (my $record = <$fh>) { my $ended = $zero ? $record =~ s/\\0\\z// : $record =~ s/\\r?\\n\\z//; print scalar reverse($record); print($zero ? qq(\\0) : qq(\\n)) if $ended } } if (@ARGV) { for my $file (@ARGV) { if (open my $fh, q(<:encoding(UTF-8)), $file) { reverse_fh($fh, $zero); close $fh } else { warn qq(rev: $file: $!\\n); $failed = 1 } } } else { reverse_fh(*STDIN, $zero) } exit $failed' -- \"$__kimix_zero\" \"$@\""},
    {"xdg-open", "start \"$@\""},
    {"open", "start \"$@\""},
    {"pbcopy", "clip.exe \"$@\""},
    {"pbpaste", "powershell.exe -NoProfile -NonInteractive -Command '[Console]::OutputEncoding=[System.Text.Encoding]::UTF8;[Console]::Out.Write((Get-Clipboard -Raw))' \"$@\""},
    {"wget", "local __kimix_url='' __kimix_out='' __kimix_stdout=0; local -a __kimix_args=(); while (( $# )); do case $1 in -O|--output-document) __kimix_out=$2; shift 2;; -O?*) __kimix_out=${1#-O}; shift;; --output-document=*) __kimix_out=${1#*=}; shift;; -q|--quiet) __kimix_args+=(-s); shift;; -c|--continue) __kimix_args+=(-C -); shift;; --no-check-certificate) __kimix_args+=(-k); shift;; -T|--timeout) __kimix_args+=(--max-time \"$2\"); shift 2;; --timeout=*) __kimix_args+=(--max-time \"${1#*=}\"); shift;; -*) printf '%s\\n' \"wget: unsupported option for curl fallback: $1\" >&2; return 1;; *) __kimix_url=$1; shift;; esac; done; if [[ -z $__kimix_url ]]; then printf '%s\\n' 'wget: missing URL' >&2; return 1; fi; if [[ $__kimix_out == '-' ]]; then __kimix_stdout=1; fi; if [[ -z $__kimix_out && $__kimix_stdout -eq 0 ]]; then __kimix_out=${__kimix_url##*/}; [[ -n $__kimix_out ]] || __kimix_out=index.html; fi; if (( __kimix_stdout )); then curl -fSL \"${__kimix_args[@]}\" -- \"$__kimix_url\"; else curl -fSL \"${__kimix_args[@]}\" -o \"$__kimix_out\" -- \"$__kimix_url\"; fi"},
    {"xclip", "local __kimix_out=0; while (( $# )); do case $1 in -o|-out) __kimix_out=1; shift;; -i|-in) shift;; -selection|-d|-display) shift 2;; -selection*|-display*) shift;; -*) printf '%s\\n' \"xclip: unsupported option for clipboard fallback: $1\" >&2; return 1;; *) shift;; esac; done; if (( __kimix_out )); then powershell.exe -NoProfile -NonInteractive -Command '[Console]::OutputEncoding=[System.Text.Encoding]::UTF8;[Console]::Out.Write((Get-Clipboard -Raw))'; else clip.exe; fi"},
    {"xsel", "local __kimix_out=0; while (( $# )); do case $1 in --output) __kimix_out=1; shift;; --input|--clipboard|--primary|--secondary) shift;; --*) printf '%s\\n' \"xsel: unsupported option for clipboard fallback: $1\" >&2; return 1;; -*) case $1 in *o*) __kimix_out=1;; esac; shift;; *) shift;; esac; done; if (( __kimix_out )); then powershell.exe -NoProfile -NonInteractive -Command '[Console]::OutputEncoding=[System.Text.Encoding]::UTF8;[Console]::Out.Write((Get-Clipboard -Raw))'; else clip.exe; fi"},
    {"wl-copy", "while (( $# )); do case $1 in -*) printf '%s\\n' \"wl-copy: unsupported option for clipboard fallback: $1\" >&2; return 1;; *) shift;; esac; done; clip.exe"},
    {"wl-paste", "while (( $# )); do case $1 in -n|--no-newline) shift;; -*) printf '%s\\n' \"wl-paste: unsupported option for clipboard fallback: $1\" >&2; return 1;; *) shift;; esac; done; powershell.exe -NoProfile -NonInteractive -Command '[Console]::OutputEncoding=[System.Text.Encoding]::UTF8;[Console]::Out.Write((Get-Clipboard -Raw))'"},
    {"zip", "local __kimix_archive='' __kimix_level=Optimal __kimix_p='' __kimix_combo='' __kimix_i=0; local -a __kimix_paths=() __kimix_wpaths=() __kimix_split=(); while (( $# )); do if [[ $1 == -[!-]* && ${#1} -gt 2 ]]; then __kimix_combo=${1#-}; __kimix_split=(); shift; for (( __kimix_i=0; __kimix_i<${#__kimix_combo}; __kimix_i++ )); do __kimix_split+=(-${__kimix_combo:__kimix_i:1}); done; set -- \"${__kimix_split[@]}\" \"$@\"; continue; fi; case $1 in -r|-R|--recurse-paths|-q|--quiet) shift;; -0) __kimix_level=NoCompression; shift;; -1) __kimix_level=Fastest; shift;; -[2-9]) shift;; -*) printf '%s\\n' \"zip: unsupported option for Compress-Archive fallback: $1\" >&2; return 1;; *) if [[ -z $__kimix_archive ]]; then __kimix_archive=$1; else __kimix_paths+=(\"$1\"); fi; shift;; esac; done; if [[ -z $__kimix_archive || ${#__kimix_paths[@]} -eq 0 ]]; then printf '%s\\n' 'zip: missing archive name or input paths' >&2; return 1; fi; for __kimix_p in \"${__kimix_paths[@]}\"; do __kimix_wpaths+=(\"$(cygpath -w -- \"$__kimix_p\")\"); done; __kimix_archive=$(cygpath -w -- \"$__kimix_archive\"); __KIMIX_ZIP_LEVEL=$__kimix_level __KIMIX_ZIP_DEST=$__kimix_archive __KIMIX_ZIP_PATHS=$(printf '%s\\n' \"${__kimix_wpaths[@]}\") powershell.exe -NoProfile -NonInteractive -Command 'Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem; $level = [System.IO.Compression.CompressionLevel]$env:__KIMIX_ZIP_LEVEL; $dest = $env:__KIMIX_ZIP_DEST; if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Force }; $zip = [System.IO.Compression.ZipFile]::Open($dest, [System.IO.Compression.ZipArchiveMode]::Create); foreach ($p in ($env:__KIMIX_ZIP_PATHS -split \"`n\")) { $item = Get-Item -LiteralPath $p; $base = $item.Name; if ($item.PSIsContainer) { $root = $item.FullName; Get-ChildItem -LiteralPath $root -Recurse -Force | ForEach-Object { $rel = $_.FullName.Substring($root.Length).TrimStart(\"\\\") -replace \"\\\\\", \"/\"; if ($_.PSIsContainer) { $zip.CreateEntry($base + \"/\" + $rel + \"/\") | Out-Null } else { [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $_.FullName, $base + \"/\" + $rel, $level) | Out-Null } } } else { [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $item.FullName, $base, $level) | Out-Null } }; $zip.Dispose(); if (Test-Path -LiteralPath $dest) { exit 0 } else { exit 1 }'"},
    {"nc", "local __kimix_z=0 __kimix_v=0 __kimix_w='' __kimix_host='' __kimix_port=''; while (( $# )); do case $1 in -z) __kimix_z=1; shift;; -v) __kimix_v=1; shift;; -zv|-vz) __kimix_z=1; __kimix_v=1; shift;; -w) __kimix_w=$2; shift 2;; -w?*) __kimix_w=${1#-w}; shift;; -*) printf '%s\\n' \"nc: unsupported option for /dev/tcp fallback: $1\" >&2; return 1;; *) if [[ -z $__kimix_host ]]; then __kimix_host=$1; elif [[ -z $__kimix_port ]]; then __kimix_port=$1; else printf '%s\\n' 'nc: too many arguments' >&2; return 1; fi; shift;; esac; done; if (( ! __kimix_z )); then printf '%s\\n' 'nc: only -z (zero-I/O scan) mode is supported by this fallback' >&2; return 1; fi; if [[ -z $__kimix_host || -z $__kimix_port ]]; then printf '%s\\n' 'nc: missing host or port' >&2; return 1; fi; if [[ -n $__kimix_w ]]; then timeout \"$__kimix_w\" bash -c 'exec 3<>/dev/tcp/$1/$2' _ \"$__kimix_host\" \"$__kimix_port\" 2>/dev/null; else (exec 3<>/dev/tcp/\"$__kimix_host\"/\"$__kimix_port\") 2>/dev/null; fi; local __kimix_rc=$?; (( __kimix_rc != 0 )) && __kimix_rc=1; if (( __kimix_rc == 0 )); then (( __kimix_v )) && printf '%s\\n' \"Connection to $__kimix_host $__kimix_port port [tcp/*] succeeded!\" >&2; else (( __kimix_v )) && printf '%s\\n' \"nc: connect to $__kimix_host port $__kimix_port (tcp) failed\" >&2; fi; return $__kimix_rc"},
    {"pgrep", "local __kimix_list=0 __kimix_full=0 __kimix_pat=''; while (( $# )); do case $1 in -l) __kimix_list=1; shift;; -f) __kimix_full=1; shift;; -lf|-fl) __kimix_list=1; __kimix_full=1; shift;; --) shift; break;; -*) printf '%s\\n' \"pgrep: unsupported option for Get-Process fallback: $1\" >&2; return 1;; *) __kimix_pat=$1; shift;; esac; done; if [[ -z $__kimix_pat ]]; then printf '%s\\n' 'pgrep: missing pattern' >&2; return 1; fi; if (( __kimix_full )); then __KIMIX_PAT=$__kimix_pat __KIMIX_LIST=$__kimix_list powershell.exe -NoProfile -NonInteractive -Command '$m = Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match $env:__KIMIX_PAT }; if ($m) { $m | ForEach-Object { if ($env:__KIMIX_LIST -eq \"1\") { \"$($_.ProcessId) $($_.Name)\" } else { $_.ProcessId } }; exit 0 } else { exit 1 }'; else __KIMIX_PAT=$__kimix_pat __KIMIX_LIST=$__kimix_list powershell.exe -NoProfile -NonInteractive -Command '$m = Get-Process | Where-Object { $_.Name -match $env:__KIMIX_PAT }; if ($m) { $m | ForEach-Object { if ($env:__KIMIX_LIST -eq \"1\") { \"$($_.Id) $($_.Name)\" } else { $_.Id } }; exit 0 } else { exit 1 }'; fi"},
    {"pkill", "local __kimix_full=0 __kimix_pat=''; while (( $# )); do case $1 in -f) __kimix_full=1; shift;; --) shift; break;; -*) printf '%s\\n' \"pkill: unsupported option for Stop-Process fallback: $1\" >&2; return 1;; *) __kimix_pat=$1; shift;; esac; done; if [[ -z $__kimix_pat ]]; then printf '%s\\n' 'pkill: missing pattern' >&2; return 1; fi; if (( __kimix_full )); then __KIMIX_PAT=$__kimix_pat powershell.exe -NoProfile -NonInteractive -Command '$m = Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match $env:__KIMIX_PAT }; if ($m) { $m | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }; exit 0 } else { exit 1 }'; else __KIMIX_PAT=$__kimix_pat powershell.exe -NoProfile -NonInteractive -Command '$m = Get-Process | Where-Object { $_.Name -match $env:__KIMIX_PAT }; if ($m) { $m | Stop-Process -Force; exit 0 } else { exit 1 }'; fi"},
    {"traceroute", "local -a __kimix_args=(); while (( $# )); do case $1 in -n) __kimix_args+=(-d); shift;; -m) __kimix_args+=(-h \"$2\"); shift 2;; -m?*) __kimix_args+=(-h \"${1#-m}\"); shift;; --max-hop=*) __kimix_args+=(-h \"${1#*=}\"); shift;; -w) __kimix_args+=(-w \"$(( $2 * 1000 ))\"); shift 2;; -w?*) __kimix_args+=(-w \"$(( ${1#-w} * 1000 ))\"); shift;; -*) printf '%s\\n' \"traceroute: unsupported option for tracert fallback: $1\" >&2; return 1;; *) __kimix_args+=(\"$1\"); shift;; esac; done; tracert \"${__kimix_args[@]}\""},
    {"tree", "local __kimix_depth=0 __kimix_all=0 __kimix_dirs=0 __kimix_noreport=0 __kimix_dir=''; while (( $# )); do case $1 in -L) __kimix_depth=$2; shift 2;; -L?*) __kimix_depth=${1#-L}; shift;; -a) __kimix_all=1; shift;; -d) __kimix_dirs=1; shift;; --noreport) __kimix_noreport=1; shift;; --) shift; break;; -*) printf '%s\\n' \"tree: unsupported option for perl fallback: $1\" >&2; return 1;; *) __kimix_dir=$1; shift;; esac; done; [[ -n $__kimix_dir ]] || __kimix_dir=.; perl -e 'my ($maxdepth,$showall,$dirsonly,$noreport,$top)=@ARGV; print qq($top\\n); my ($ndirs,$nfiles)=(0,0); sub walk { my ($path,$prefix,$depth)=@_; return if $maxdepth && $depth>$maxdepth; opendir(my $dh,$path) or return; my @e = grep { ! /^[.][.]?$/ } readdir($dh); closedir($dh); @e = grep { $showall || ! /^[.]/ } @e; @e = grep { ! $dirsonly || -d qq($path/$_) } @e; @e = sort { lc($a) cmp lc($b) } @e; my $n=@e; my $i=0; for my $e (@e) { $i++; my $last = $i==$n; my $full = qq($path/$e); my $isdir = -d $full; if ($isdir) { $ndirs++ } else { $nfiles++ } print $prefix, ($last ? qq(`-- ) : qq(|-- )), $e, qq(\\n); walk($full, $prefix . ($last ? qq(    ) : qq(|   )), $depth+1) if $isdir && ! -l $full } } walk($top,q(),1); my $dw = $ndirs==1 ? q(directory) : q(directories); my $fw = $nfiles==1 ? q(file) : q(files); print qq(\\n$ndirs $dw, $nfiles $fw\\n) unless $noreport' -- \"$__kimix_depth\" \"$__kimix_all\" \"$__kimix_dirs\" \"$__kimix_noreport\" \"$__kimix_dir\""},
    {"say", "while (( $# )); do case $1 in -*) printf '%s\\n' \"say: unsupported option for SAPI fallback: $1\" >&2; return 1;; *) shift;; esac; done; __KIMIX_SAY_TEXT=$* powershell.exe -NoProfile -NonInteractive -Command 'Add-Type -AssemblyName System.Speech; (New-Object System.Speech.Synthesis.SpeechSynthesizer).Speak($env:__KIMIX_SAY_TEXT)'"},
    {"python3", "python \"$@\""},
    {"pip3", "pip \"$@\""},
    {"copy", "if [[ $# -lt 2 ]]; then printf '%s\\n' 'copy: missing source or destination' >&2; return 1; fi; cp -R -- \"$@\""},
    {"move", "if [[ $# -lt 2 ]]; then printf '%s\\n' 'move: missing source or destination' >&2; return 1; fi; mv -- \"$@\""},
    {"del", "rm -- \"$@\""},
    {"erase", "rm -- \"$@\""},
    {"ren", "if [[ $# -ne 2 ]]; then printf '%s\\n' 'ren: exactly two arguments required' >&2; return 1; fi; mv -- \"$1\" \"$2\""},
    {"rename", "if [[ $# -ne 2 ]]; then printf '%s\\n' 'rename: exactly two arguments required' >&2; return 1; fi; mv -- \"$1\" \"$2\""},
    {"rd", "rmdir -- \"$@\""},
    {"md", "mkdir -p -- \"$@\""},
    {"chdir", "cd -- \"$@\""},
    {"cls", "clear"},
    {"xcopy", "cp -r -- \"$@\""},
    {"mklink", "local __kimix_hard=0 __kimix_link='' __kimix_target=''; while (( $# )); do case $1 in /D|/d|/J|/j) shift;; /H|/h) __kimix_hard=1; shift;; *) if [[ -z $__kimix_link ]]; then __kimix_link=$1; elif [[ -z $__kimix_target ]]; then __kimix_target=$1; else printf '%s\\n' 'mklink: too many arguments' >&2; return 1; fi; shift;; esac; done; if [[ -z $__kimix_link || -z $__kimix_target ]]; then printf '%s\\n' 'mklink: missing link name or target' >&2; return 1; fi; if (( __kimix_hard )); then ln -f -- \"$__kimix_target\" \"$__kimix_link\"; else ln -s -- \"$__kimix_target\" \"$__kimix_link\"; fi"},
    {"findstr", "grep \"$@\""},
    {"fc", "diff \"$@\""},
    {"where", "which \"$@\""},
    {"tasklist", "powershell.exe -NoProfile -NonInteractive -Command 'Get-Process | Select-Object Name, Id, CPU, WorkingSet | Format-Table -AutoSize'"},
    {"taskkill", "local __kimix_force=0 __kimix_pid='' __kimix_im=''; while (( $# )); do case $1 in /F|/f) __kimix_force=1; shift;; /IM|/im) __kimix_im=$2; shift 2;; /PID|/pid) __kimix_pid=$2; shift 2;; /*) printf '%s\\n' \"taskkill: unsupported option: $1\" >&2; return 1;; *) printf '%s\\n' \"taskkill: unsupported argument: $1\" >&2; return 1;; esac; done; if [[ -n $__kimix_pid ]]; then __KIMIX_FORCE=$__kimix_force __KIMIX_PID=$__kimix_pid powershell.exe -NoProfile -NonInteractive -Command '$force = $env:__KIMIX_FORCE -eq '1'; if ($env:__KIMIX_PID) { Stop-Process -Id $env:__KIMIX_PID -Force:$force; exit 0 } $procs = Get-Process | Where-Object { $_.Name -eq $env:__KIMIX_IM }; if ($procs) { $procs | Stop-Process -Force:$force; exit 0 } else { exit 1 }'; elif [[ -n $__kimix_im ]]; then __KIMIX_FORCE=$__kimix_force __KIMIX_IM=$__kimix_im powershell.exe -NoProfile -NonInteractive -Command '$force = $env:__KIMIX_FORCE -eq '1'; if ($env:__KIMIX_PID) { Stop-Process -Id $env:__KIMIX_PID -Force:$force; exit 0 } $procs = Get-Process | Where-Object { $_.Name -eq $env:__KIMIX_IM }; if ($procs) { $procs | Stop-Process -Force:$force; exit 0 } else { exit 1 }'; else printf '%s\\n' 'taskkill: missing /PID or /IM' >&2; return 1; fi"},
    {"systeminfo", "powershell.exe -NoProfile -NonInteractive -Command 'Get-ComputerInfo | Format-List'"},
    {"watch", "local __kimix_interval=2; while (( $# )); do case $1 in -n) __kimix_interval=$2; shift 2;; -n?*) __kimix_interval=${1#-n}; shift;; -t|-d|--no-title|--color) shift;; --) shift; break;; -*) printf '%s\\n' \"watch: unsupported option: $1\" >&2; return 1;; *) break;; esac; done; if [[ $# -eq 0 ]]; then printf '%s\\n' 'watch: missing command' >&2; return 1; fi; while true; do clear; eval \"$*\"; sleep \"$__kimix_interval\"; done"},
    {"killall", "if [[ $# -eq 0 ]]; then printf '%s\\n' 'killall: missing process name' >&2; return 1; fi; __KIMIX_NAME=$1 powershell.exe -NoProfile -NonInteractive -Command '$procs = Get-Process | Where-Object { $_.Name -eq $env:__KIMIX_NAME }; if ($procs) { $procs | Stop-Process -Force; exit 0 } else { exit 1 }'"},
    {"pidof", "if [[ $# -eq 0 ]]; then printf '%s\\n' 'pidof: missing process name' >&2; return 1; fi; __KIMIX_NAME=$1 powershell.exe -NoProfile -NonInteractive -Command '$ids = (Get-Process | Where-Object { $_.Name -eq $env:__KIMIX_NAME }).Id; if ($ids) { $ids -join \" \"; exit 0 } else { exit 1 }'"},
    {"column", "local __kimix_sep='DEFAULT'; while (( $# )); do case $1 in -t) shift;; -s) __kimix_sep=$2; shift 2;; -s?*) __kimix_sep=${1#-s}; shift;; -*) printf '%s\012' \"column: unsupported option for perl fallback: $1\" >&2; return 1;; *) break;; esac; done; perl -e 'my $sep = shift @ARGV; $sep = qr/\\s+/ if $sep eq \"DEFAULT\"; my @rows; my @max; while (<>) { chomp; my @c = split $sep; push @rows, \\@c; for my $i (0..$#c) { $max[$i] = length($c[$i]) if !defined $max[$i] || length($c[$i]) > $max[$i]; } } for my $r (@rows) { print join(\"  \", map { sprintf(\"%-*s\", $max[$_]//0, $r->[$_]) } 0..$#$r), \"\\n\"; }' \"$__kimix_sep\" \"$@\""},
    {"free", "local __kimix_unit=K; while (( $# )); do case $1 in -b|--bytes) __kimix_unit=B; shift;; -k|--kibi|--kilo) __kimix_unit=K; shift;; -m|--mebi|--mega) __kimix_unit=M; shift;; -g|--gibi|--giga) __kimix_unit=G; shift;; -h|--human) __kimix_unit=H; shift;; --help) printf '%s\012' 'free: report memory usage (Windows: Win32_OperatingSystem; no swap row)'; return 0;; -*) printf '%s\012' \"free: unsupported option: $1\" >&2; return 1;; *) shift;; esac; done; local -a __kimix_m=(); mapfile -t __kimix_m < <(powershell.exe -NoProfile -NonInteractive -Command '$o=Get-CimInstance Win32_OperatingSystem; Write-Output $o.TotalVisibleMemorySize; Write-Output $o.FreePhysicalMemory' 2>/dev/null | tr -d '\\r'); if [[ ${#__kimix_m[@]} -lt 2 ]]; then printf '%s\012' 'free: failed to query memory information' >&2; return 1; fi; local __kimix_total=${__kimix_m[0]} __kimix_free=${__kimix_m[1]}; local __kimix_used=$(( __kimix_total - __kimix_free )); printf '%s\012' '               total        used        free'; case $__kimix_unit in B) printf 'Mem: %12d %11d %11d\012' $(( __kimix_total * 1024 )) $(( __kimix_used * 1024 )) $(( __kimix_free * 1024 ));; M) printf 'Mem: %12d %11d %11d\012' $(( __kimix_total / 1024 )) $(( __kimix_used / 1024 )) $(( __kimix_free / 1024 ));; G) printf 'Mem: %12d %11d %11d\012' $(( __kimix_total / 1024 / 1024 )) $(( __kimix_used / 1024 / 1024 )) $(( __kimix_free / 1024 / 1024 ));; H) awk -v t=\"$__kimix_total\" -v f=\"$__kimix_free\" 'function h(x,  i){i=1; while(x>=10240&&i<4){x=x/1024;i++} return sprintf(\"%.1f%s\", x, substr(\"KMGT\", i, 1))} BEGIN{printf \"Mem: %11s %10s %10s\\n\", h(t), h(t-f), h(f)}';; *) printf 'Mem: %12d %11d %11d\012' \"$__kimix_total\" \"$__kimix_used\" \"$__kimix_free\";; esac"},
    {"uptime", "local __kimix_since=0; while (( $# )); do case $1 in -s|--since) __kimix_since=1; shift;; --help) printf '%s\012' 'uptime: tell how long the system has been running (Windows approximation; no user count)'; return 0;; -*) printf '%s\012' \"uptime: unsupported option: $1\" >&2; return 1;; *) printf '%s\012' \"uptime: unsupported argument: $1\" >&2; return 1;; esac; done; local -a __kimix_u=(); mapfile -t __kimix_u < <(powershell.exe -NoProfile -NonInteractive -Command '$b=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime; Write-Output $b.ToString(\"yyyy-MM-dd HH:mm:ss\"); $up=New-TimeSpan -Start $b -End (Get-Date); Write-Output $up.Days; Write-Output $up.Hours; Write-Output $up.Minutes' 2>/dev/null | tr -d '\\r'); if [[ ${#__kimix_u[@]} -lt 4 ]]; then printf '%s\012' 'uptime: failed to query boot time' >&2; return 1; fi; if (( __kimix_since )); then printf '%s\012' \"${__kimix_u[0]}\"; return 0; fi; local __kimix_days=${__kimix_u[1]} __kimix_hours=${__kimix_u[2]} __kimix_mins=${__kimix_u[3]}; printf -v __kimix_mins '%02d' \"$__kimix_mins\"; local __kimix_up; if (( __kimix_days > 0 )); then printf -v __kimix_up '%d day(s), %d:%s' \"$__kimix_days\" \"$__kimix_hours\" \"$__kimix_mins\"; else printf -v __kimix_up '%d:%s' \"$__kimix_hours\" \"$__kimix_mins\"; fi; printf '%s up %s, load average: n/a (not reported on Windows)\012' \"$(date '+%H:%M:%S')\" \"$__kimix_up\""},
    {"top", "local __kimix_delay=3 __kimix_iters=0 __kimix_batch=0 __kimix_count=0; while (( $# )); do case $1 in -b) __kimix_batch=1; shift;; -d) __kimix_delay=$2; shift 2;; -d?*) __kimix_delay=${1#-d}; shift;; -n) __kimix_iters=$2; shift 2;; -n?*) __kimix_iters=${1#-n}; shift;; -h|--help) printf '%s\012' 'top: display processes (Windows approximation via Get-Process; Ctrl+C quits)'; return 0;; -*) printf '%s\012' \"${FUNCNAME[0]}: unsupported option: $1\" >&2; return 1;; *) shift;; esac; done; __kimix_snapshot() { powershell.exe -NoProfile -NonInteractive -Command 'Get-Process | Sort-Object -Property CPU -Descending | Select-Object -First 25 Id,ProcessName,CPU,WorkingSet | Format-Table -AutoSize'; }; if (( __kimix_batch )); then local __kimix_n=$__kimix_iters; (( __kimix_n > 0 )) || __kimix_n=1; while (( __kimix_count < __kimix_n )); do __kimix_snapshot; __kimix_count=$(( __kimix_count + 1 )); done; return 0; fi; while (( __kimix_iters == 0 || __kimix_count < __kimix_iters )); do clear; __kimix_snapshot; __kimix_count=$(( __kimix_count + 1 )); (( __kimix_iters > 0 && __kimix_count >= __kimix_iters )) && break; sleep \"$__kimix_delay\"; done"},
    {"ss", "local __kimix_stats=0; local -a __kimix_split=(); local __kimix_combo='' __kimix_i=0; while (( $# )); do if [[ $1 == -[!-]* && ${#1} -gt 2 ]]; then __kimix_combo=${1#-}; __kimix_split=(); shift; for (( __kimix_i=0; __kimix_i<${#__kimix_combo}; __kimix_i++ )); do __kimix_split+=(-${__kimix_combo:__kimix_i:1}); done; set -- \"${__kimix_split[@]}\" \"$@\"; continue; fi; case $1 in -s|--summary) __kimix_stats=1; shift;; -t|-u|-l|-n|-a|-p|-e|-m|-r|-i|-x|-4|-6|-T|--tcp|--udp|--listening|--numeric|--all|--process|--extended|--memory|--resolve|--inet|--inet4|--inet6) shift;; --*) shift;; -*) printf '%s\012' \"ss: unsupported option: $1\" >&2; return 1;; *) shift;; esac; done; if (( __kimix_stats )); then netstat -s; else netstat -ano; fi"},
    {"ip", "local __kimix_sub=''; while (( $# )); do case $1 in -4|-6|-br|--brief|-details|-s|-human|-iec|-o|-oneline|-c|--color|-color) shift;; -h|--help) __kimix_sub=help; shift;; -*) printf '%s\012' \"ip: unsupported option: $1\" >&2; return 1;; *) if [[ -z $__kimix_sub ]]; then __kimix_sub=$1; fi; shift;; esac; done; case $__kimix_sub in help) printf '%s\012' 'Usage: ip [addr|link|route|neigh] (Windows equivalents: ipconfig / Get-NetAdapter / route print / arp -a)'; return 0;; '') printf '%s\012' 'Usage: ip [addr|link|route|neigh] (Windows equivalents: ipconfig / Get-NetAdapter / route print / arp -a)' >&2; return 1;; addr|address|a) ipconfig;; link|l) powershell.exe -NoProfile -NonInteractive -Command 'Get-NetAdapter | ForEach-Object { \"$($_.Name) $($_.Status) $($_.LinkSpeed) $($_.MacAddress)\" }';; route|r) route print;; neigh|n) arp -a;; *) printf '%s\012' \"ip: unsupported object: $__kimix_sub (supported: addr link route neigh)\" >&2; return 1;; esac"},
    {"man", "local __kimix_rc=0 __kimix_seen=0; while (( $# )); do case $1 in --help) printf '%s\012' 'man: show command help (fallback prints <command> --help)'; return 0;; --) shift; break;; -*) printf '%s\012' \"man: unsupported option for --help fallback: $1\" >&2; return 1;; *) break;; esac; done; while (( $# )); do if [[ $1 =~ ^[0-9]+$ ]]; then shift; continue; fi; __kimix_seen=1; \"$1\" --help || __kimix_rc=1; shift; done; if (( ! __kimix_seen )); then printf '%s\012' 'man: missing command name' >&2; return 1; fi; return $__kimix_rc"},
    {"systemctl", "local __kimix_cmd=''; local -a __kimix_names=(); while (( $# )); do case $1 in -q|--quiet|--no-pager|--plain|--full|-l|--no-legend|--no-ask-password|--user|--system|--global) shift;; --type=*) shift;; --) shift; break;; -*) printf '%s\012' \"systemctl: unsupported option: $1\" >&2; return 1;; *) if [[ -z $__kimix_cmd ]]; then __kimix_cmd=$1; else __kimix_names+=(\"$1\"); fi; shift;; esac; done; if [[ -z $__kimix_cmd ]]; then printf '%s\012' 'systemctl: missing subcommand (Windows service equivalents; supported: status start stop restart reload enable disable is-active is-enabled list-units list-unit-files)' >&2; return 1; fi; case $__kimix_cmd in status) (( ${#__kimix_names[@]} )) || { printf '%s\012' 'systemctl: missing unit name' >&2; return 1; }; local __kimix_csv=$(IFS=,; echo \"${__kimix_names[*]}\"); powershell.exe -NoProfile -NonInteractive -Command \"Get-Service -Name '$__kimix_csv' | Format-List Name,DisplayName,Status,StartType\";; start|stop|restart|reload) (( ${#__kimix_names[@]} )) || { printf '%s\012' 'systemctl: missing unit name' >&2; return 1; }; local __kimix_ps='' __kimix_n; for __kimix_n in \"${__kimix_names[@]}\"; do if [[ $__kimix_cmd == stop || $__kimix_cmd == restart || $__kimix_cmd == reload ]]; then __kimix_ps+=\"Stop-Service -Name '$__kimix_n' -ErrorAction Stop; \"; fi; if [[ $__kimix_cmd == start || $__kimix_cmd == restart || $__kimix_cmd == reload ]]; then __kimix_ps+=\"Start-Service -Name '$__kimix_n' -ErrorAction Stop; \"; fi; done; powershell.exe -NoProfile -NonInteractive -Command \"$__kimix_ps\";; enable|disable) (( ${#__kimix_names[@]} )) || { printf '%s\012' 'systemctl: missing unit name' >&2; return 1; }; local __kimix_start=demand; [[ $__kimix_cmd == enable ]] && __kimix_start=auto; local __kimix_rc=0 __kimix_n; for __kimix_n in \"${__kimix_names[@]}\"; do sc.exe config \"$__kimix_n\" start= \"$__kimix_start\" >/dev/null || __kimix_rc=1; done; return $__kimix_rc;; is-active) (( ${#__kimix_names[@]} )) || { printf '%s\012' 'systemctl: missing unit name' >&2; return 1; }; local __kimix_rc=0 __kimix_n; for __kimix_n in \"${__kimix_names[@]}\"; do if sc.exe query \"$__kimix_n\" 2>/dev/null | grep -q 'RUNNING'; then printf '%s\012' active; else printf '%s\012' inactive; __kimix_rc=3; fi; done; return $__kimix_rc;; is-enabled) (( ${#__kimix_names[@]} )) || { printf '%s\012' 'systemctl: missing unit name' >&2; return 1; }; local __kimix_rc=0 __kimix_n; for __kimix_n in \"${__kimix_names[@]}\"; do if sc.exe qc \"$__kimix_n\" 2>/dev/null | grep -q 'DISABLED'; then printf '%s\012' disabled; __kimix_rc=1; else printf '%s\012' enabled; fi; done; return $__kimix_rc;; list-units|list-unit-files) powershell.exe -NoProfile -NonInteractive -Command 'Get-Service | ForEach-Object { \"$($_.Status) $($_.Name) $($_.DisplayName)\" }';; *) printf '%s\012' \"systemctl: unsupported subcommand: $__kimix_cmd (supported: status start stop restart reload enable disable is-active is-enabled list-units list-unit-files)\" >&2; return 1;; esac"},
    {"sudo", "if (( $# == 0 )); then printf '%s\012' 'sudo: missing command' >&2; return 1; fi; local __kimix_bash; __kimix_bash=$(type -P bash) || { printf '%s\012' 'sudo: bash not found' >&2; return 1; }; local __kimix_wbash; __kimix_wbash=$(cygpath -w -- \"$__kimix_bash\") || return 1; printf '%s\012' 'sudo: elevating via UAC in a separate window; output is not captured here' >&2; __KIMIX_SUDO_CMD=\"$*\" powershell.exe -NoProfile -NonInteractive -Command \"Start-Process -Verb RunAs -Wait -FilePath '$__kimix_wbash' -ArgumentList '-c', \\$env:__KIMIX_SUDO_CMD\"; local __kimix_rc=$?; return $__kimix_rc"},
    {"gawk", "awk \"$@\""},
    {"gcat", "cat \"$@\""},
    {"gcomm", "comm \"$@\""},
    {"gcp", "cp \"$@\""},
    {"gcut", "cut \"$@\""},
    {"gdate", "date \"$@\""},
    {"gdf", "df \"$@\""},
    {"gdu", "du \"$@\""},
    {"gegrep", "egrep \"$@\""},
    {"gfgrep", "fgrep \"$@\""},
    {"gfind", "find \"$@\""},
    {"ggrep", "grep \"$@\""},
    {"ghead", "head \"$@\""},
    {"gjoin", "join \"$@\""},
    {"gln", "ln \"$@\""},
    {"gls", "ls \"$@\""},
    {"gmake", "make \"$@\""},
    {"gmkdir", "mkdir \"$@\""},
    {"gmv", "mv \"$@\""},
    {"gpaste", "paste \"$@\""},
    {"greadlink", "readlink \"$@\""},
    {"grealpath", "realpath \"$@\""},
    {"grm", "rm \"$@\""},
    {"grmdir", "rmdir \"$@\""},
    {"gsed", "sed \"$@\""},
    {"gseq", "seq \"$@\""},
    {"gshuf", "shuf \"$@\""},
    {"gsort", "sort \"$@\""},
    {"gsplit", "split \"$@\""},
    {"gstat", "stat \"$@\""},
    {"gtail", "tail \"$@\""},
    {"gtar", "tar \"$@\""},
    {"gtr", "tr \"$@\""},
    {"guniq", "uniq \"$@\""},
    {"gwc", "wc \"$@\""},
    {"gxargs", "xargs \"$@\""},
    {"netcat", "local __kimix_z=0 __kimix_v=0 __kimix_w='' __kimix_host='' __kimix_port=''; while (( $# )); do case $1 in -z) __kimix_z=1; shift;; -v) __kimix_v=1; shift;; -zv|-vz) __kimix_z=1; __kimix_v=1; shift;; -w) __kimix_w=$2; shift 2;; -w?*) __kimix_w=${1#-w}; shift;; -*) printf '%s\\n' \"nc: unsupported option for /dev/tcp fallback: $1\" >&2; return 1;; *) if [[ -z $__kimix_host ]]; then __kimix_host=$1; elif [[ -z $__kimix_port ]]; then __kimix_port=$1; else printf '%s\\n' 'nc: too many arguments' >&2; return 1; fi; shift;; esac; done; if (( ! __kimix_z )); then printf '%s\\n' 'nc: only -z (zero-I/O scan) mode is supported by this fallback' >&2; return 1; fi; if [[ -z $__kimix_host || -z $__kimix_port ]]; then printf '%s\\n' 'nc: missing host or port' >&2; return 1; fi; if [[ -n $__kimix_w ]]; then timeout \"$__kimix_w\" bash -c 'exec 3<>/dev/tcp/$1/$2' _ \"$__kimix_host\" \"$__kimix_port\" 2>/dev/null; else (exec 3<>/dev/tcp/\"$__kimix_host\"/\"$__kimix_port\") 2>/dev/null; fi; local __kimix_rc=$?; (( __kimix_rc != 0 )) && __kimix_rc=1; if (( __kimix_rc == 0 )); then (( __kimix_v )) && printf '%s\\n' \"Connection to $__kimix_host $__kimix_port port [tcp/*] succeeded!\" >&2; else (( __kimix_v )) && printf '%s\\n' \"nc: connect to $__kimix_host port $__kimix_port (tcp) failed\" >&2; fi; return $__kimix_rc"},
    {"htop", "local __kimix_delay=3 __kimix_iters=0 __kimix_batch=0 __kimix_count=0; while (( $# )); do case $1 in -b) __kimix_batch=1; shift;; -d) __kimix_delay=$2; shift 2;; -d?*) __kimix_delay=${1#-d}; shift;; -n) __kimix_iters=$2; shift 2;; -n?*) __kimix_iters=${1#-n}; shift;; -h|--help) printf '%s\012' 'top: display processes (Windows approximation via Get-Process; Ctrl+C quits)'; return 0;; -*) printf '%s\012' \"${FUNCNAME[0]}: unsupported option: $1\" >&2; return 1;; *) shift;; esac; done; __kimix_snapshot() { powershell.exe -NoProfile -NonInteractive -Command 'Get-Process | Sort-Object -Property CPU -Descending | Select-Object -First 25 Id,ProcessName,CPU,WorkingSet | Format-Table -AutoSize'; }; if (( __kimix_batch )); then local __kimix_n=$__kimix_iters; (( __kimix_n > 0 )) || __kimix_n=1; while (( __kimix_count < __kimix_n )); do __kimix_snapshot; __kimix_count=$(( __kimix_count + 1 )); done; return 0; fi; while (( __kimix_iters == 0 || __kimix_count < __kimix_iters )); do clear; __kimix_snapshot; __kimix_count=$(( __kimix_count + 1 )); (( __kimix_iters > 0 && __kimix_count >= __kimix_iters )) && break; sleep \"$__kimix_delay\"; done"},
};

const bash_fix_unsupported_reason k_bash_fix_unsupported_reasons[] = {
    {"journalctl", "systemd's journal does not exist on Windows and Git Bash has no journal daemon, so there is no faithful equivalent. Read the application's own log file(s), or query the Windows Event Log instead: powershell.exe Get-WinEvent -LogName Application -MaxEvents 50"},
};
// <<< GENERATED:BASH-FIX-DATA <<<

const char *bash_fix_body_of(kimix::string_view name) noexcept {
    for (const bash_fix_fallback_body &entry : k_bash_fix_fallback_bodies) {
        if (name == kimix::string_view(entry.name)) {
            return entry.body;
        }
    }
    return nullptr;
}

const char *bash_fix_reason_of(kimix::string_view name) noexcept {
    for (const bash_fix_unsupported_reason &entry : k_bash_fix_unsupported_reasons) {
        if (name == kimix::string_view(entry.name)) {
            return entry.reason;
        }
    }
    return nullptr;
}

bool bash_fix_is_fallback_name(kimix::string_view name) noexcept {
    return bash_fix_body_of(name) != nullptr;
}

// _fallback_definition: the exported shell function installed for *name*.
kimix::string bash_fix_definition(kimix::string_view name) {
    const char *body = bash_fix_body_of(name);
    kimix::string out;
    if (body == nullptr) {
        return out;
    }
    // _STUB_AWARE_FALLBACKS: the Microsoft Store App Execution Alias stubs in
    // WindowsApps satisfy ``command -v`` but print an install prompt instead of
    // running the tool, so these names get a stub-aware guard and delegate.
    const bool stub_aware = (name == "pip3" || name == "python3");
    out += "if ! command -v ";
    out.append(name.data(), name.size());
    if (stub_aware) {
        out += " >/dev/null 2>&1 || [[ $(type -P ";
        out.append(name.data(), name.size());
        out += ") == *WindowsApps* ]]; then ";
    } else {
        out += " >/dev/null 2>&1; then ";
    }
    out.append(name.data(), name.size());
    out += "() { local __kimix_native=''; __kimix_native=$(type -P ";
    out.append(name.data(), name.size());
    if (stub_aware) {
        out += ") || :; if [[ -n $__kimix_native && $__kimix_native != "
               "*WindowsApps* ]]; then \"$__kimix_native\" \"$@\"; return; fi; ";
    } else {
        out += ") || :; if [[ -n $__kimix_native ]]; then "
               "\"$__kimix_native\" \"$@\"; return; fi; ";
    }
    out += body;
    out += "; }; fi";
    return out;
}

// _single_quote: quote *command* as one literal Bash word.
kimix::string bash_fix_single_quote(kimix::string_view command) {
    kimix::string out;
    out.push_back('\'');
    for (const char c : command) {
        if (c == '\'') {
            out.append("'\"'\"'", 5);
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

// _wrapper_runner: an executable command for wrappers that cannot invoke
// shell functions (``xargs rev``, ``timeout 5 rev``).
kimix::string bash_fix_wrapper_runner(kimix::string_view name) {
    kimix::string script = bash_fix_definition(name);
    script += "; ";
    script.append(name.data(), name.size());
    script += " \"$@\"";
    kimix::string out = "/usr/bin/bash -c ";
    out += bash_fix_single_quote(script);
    out += " --";
    return out;
}

// ---------------------------------------------------------------------------
// Reference tables (mirrored literally from _shell_compat.py)
// ---------------------------------------------------------------------------

// _COMMAND_START_KEYWORDS / _COMMAND_END_KEYWORDS / _LIST_KEYWORDS
bool bash_fix_command_start_keyword(kimix::string_view w) noexcept {
    return w == "!" || w == "{" || w == "if" || w == "then" || w == "elif" ||
           w == "else" || w == "while" || w == "until" || w == "do";
}

bool bash_fix_command_end_keyword(kimix::string_view w) noexcept {
    return w == "fi" || w == "done" || w == "esac";
}

bool bash_fix_list_keyword(kimix::string_view w) noexcept {
    return w == "for" || w == "select" || w == "case";
}

// _SHELL_WRAPPERS (only shells whose syntax is a subset of bash).
bool bash_fix_shell_wrapper_name_of(kimix::string_view w) noexcept {
    return w == "bash" || w == "sh" || w == "dash" || w == "ash";
}

// _SHELL_C_OPTIONS = ^-c$|^-lc$|^-cl$ (a fullmatch on the option word).
bool bash_fix_shell_c_option(kimix::string_view opt) noexcept {
    return opt == "-c" || opt == "-lc" || opt == "-cl";
}

// _COMMAND_WRAPPERS: wrappers whose operand is itself a command.
enum class bash_fix_wrapper_kind : uint8_t {
    none = 0,
    command,
    coproc,
    env,
    exec,
    nohup,
    sudo,
    time,
    timeout,
    stdbuf,
    nice,
    xargs,
    watch
};

bool bash_fix_wrapper_kind_of(kimix::string_view w,
                              bash_fix_wrapper_kind &out) noexcept {
    if (w == "command") {
        out = bash_fix_wrapper_kind::command;
    } else if (w == "coproc") {
        out = bash_fix_wrapper_kind::coproc;
    } else if (w == "env") {
        out = bash_fix_wrapper_kind::env;
    } else if (w == "exec") {
        out = bash_fix_wrapper_kind::exec;
    } else if (w == "nohup") {
        out = bash_fix_wrapper_kind::nohup;
    } else if (w == "sudo") {
        out = bash_fix_wrapper_kind::sudo;
    } else if (w == "time") {
        out = bash_fix_wrapper_kind::time;
    } else if (w == "timeout") {
        out = bash_fix_wrapper_kind::timeout;
    } else if (w == "stdbuf") {
        out = bash_fix_wrapper_kind::stdbuf;
    } else if (w == "nice") {
        out = bash_fix_wrapper_kind::nice;
    } else if (w == "xargs") {
        out = bash_fix_wrapper_kind::xargs;
    } else {
        return false;
    }
    return true;
}

// _FALLBACK_COMMAND_WRAPPERS = {"gtimeout": "timeout", "watch": "watch",
// "sudo": "sudo"} - the word is a fallback name AND a wrapper.
bool bash_fix_fallback_wrapper_kind_of(kimix::string_view w,
                                       bash_fix_wrapper_kind &out) noexcept {
    if (w == "gtimeout") {
        out = bash_fix_wrapper_kind::timeout;
        return true;
    }
    if (w == "watch") {
        out = bash_fix_wrapper_kind::watch;
        return true;
    }
    if (w == "sudo") {
        out = bash_fix_wrapper_kind::sudo;
        return true;
    }
    return false;
}

// _SAME_SHELL_WRAPPERS = {coproc, time, watch}.
bool bash_fix_same_shell_wrapper(bash_fix_wrapper_kind k) noexcept {
    return k == bash_fix_wrapper_kind::coproc || k == bash_fix_wrapper_kind::time ||
           k == bash_fix_wrapper_kind::watch;
}

// _WRAPPER_OPERAND_COUNTS = {"timeout": 1}.
int64_t bash_fix_wrapper_operand_count(bash_fix_wrapper_kind k) noexcept {
    return k == bash_fix_wrapper_kind::timeout ? 1 : 0;
}

// _WRAPPER_OPTIONS_WITH_VALUE[wrapper.kind] membership.
bool bash_fix_wrapper_option_takes_value(bash_fix_wrapper_kind k,
                                         kimix::string_view opt) noexcept {
    switch (k) {
    case bash_fix_wrapper_kind::env:
        return opt == "-u" || opt == "--unset" || opt == "-C" || opt == "--chdir" ||
               opt == "-S" || opt == "--split-string";
    case bash_fix_wrapper_kind::exec:
        return opt == "-a";
    case bash_fix_wrapper_kind::sudo:
        return opt == "-C" || opt == "--close-from" || opt == "-D" || opt == "--chdir" ||
               opt == "-g" || opt == "--group" || opt == "-h" || opt == "--host" ||
               opt == "-p" || opt == "--prompt" || opt == "-R" || opt == "--chroot" ||
               opt == "-r" || opt == "--role" || opt == "-t" || opt == "--type" ||
               opt == "-T" || opt == "--command-timeout" || opt == "-u" || opt == "--user";
    case bash_fix_wrapper_kind::time:
        return opt == "-f" || opt == "--format" || opt == "-o" || opt == "--output";
    case bash_fix_wrapper_kind::timeout:
        return opt == "-k" || opt == "--kill-after" || opt == "-s" || opt == "--signal";
    case bash_fix_wrapper_kind::stdbuf:
        return opt == "-o" || opt == "-e" || opt == "-i" || opt == "--output" ||
               opt == "--error" || opt == "--input";
    case bash_fix_wrapper_kind::nice:
        return opt == "-n" || opt == "--adjustment";
    case bash_fix_wrapper_kind::xargs:
        return opt == "-I" || opt == "-n" || opt == "-L" || opt == "-s" || opt == "-P" ||
               opt == "-a" || opt == "-E" || opt == "-d" || opt == "--arg-file" ||
               opt == "--max-args" || opt == "--max-chars" || opt == "--max-procs" ||
               opt == "--max-lines" || opt == "--replace" || opt == "--eof" ||
               opt == "--delimiter";
    case bash_fix_wrapper_kind::watch:
        return opt == "-n" || opt == "--interval";
    default:
        return false;
    }
}

// _WRAPPER_PATH_OPTIONS[wrapper.kind]: the option value is a filesystem path
// (env: -C/--chdir, sudo: -D/--chdir, time: -o/--output, xargs: -a/--arg-file).
bool bash_fix_wrapper_path_option(bash_fix_wrapper_kind k,
                                  kimix::string_view opt) noexcept {
    switch (k) {
    case bash_fix_wrapper_kind::env:
        return opt == "-C" || opt == "--chdir";
    case bash_fix_wrapper_kind::sudo:
        return opt == "-D" || opt == "--chdir";
    case bash_fix_wrapper_kind::time:
        return opt == "-o" || opt == "--output";
    case bash_fix_wrapper_kind::xargs:
        return opt == "-a" || opt == "--arg-file";
    default:
        return false;
    }
}

// _WRAPPER_PATH_OPTIONS keys: only these wrappers inspect path-valued options.
bool bash_fix_wrapper_has_path_options(bash_fix_wrapper_kind k) noexcept {
    return k == bash_fix_wrapper_kind::env || k == bash_fix_wrapper_kind::sudo ||
           k == bash_fix_wrapper_kind::time || k == bash_fix_wrapper_kind::xargs;
}

// ---------------------------------------------------------------------------
// Character classes (_OPERATOR_CHARS / _WORD_END_CHARS / _PATH_*)
// ---------------------------------------------------------------------------

bool bash_fix_operator_char(char c) noexcept {
    switch (c) {
    case ';':
    case '&':
    case '|':
    case '(':
    case ')':
    case '<':
    case '>':
    case '\n':
        return true;
    default:
        return false;
    }
}

// _WORD_END_CHARS = _OPERATOR_CHARS | {" ", "\t", "\r"}
bool bash_fix_word_end_char(char c) noexcept {
    return bash_fix_operator_char(c) || c == ' ' || c == '\t' || c == '\r';
}

bool bash_fix_redirection_start(char c) noexcept { return c == '<' || c == '>'; }

bool bash_is_hex_digit(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

bool bash_fix_path_safe_char(char c) noexcept {
    if (bash_is_alpha(c) || bash_is_digit(c)) {
        return true;
    }
    switch (c) {
    case '_':
    case '.':
    case '/':
    case ':':
    case '~':
    case '@':
    case '%':
    case '+':
    case '=':
    case '-':
    case '#':
    case ',':
    case '[':
    case ']':
    case '*':
    case '?':
        return true;
    default:
        return false;
    }
}

// _ESCAPED_LITERAL_CHARS: chars whose backslash form is a pure Bash escape.
bool bash_fix_escaped_literal_char(char c) noexcept {
    switch (c) {
    case ' ':
    case '\t':
    case '&':
    case ';':
    case '|':
    case '(':
    case ')':
    case '<':
    case '>':
    case '#':
    case '\'':
    case '"':
    case '$':
    case '`':
    case '{':
    case '}':
    case '!':
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Small string helpers
// ---------------------------------------------------------------------------

kimix::string_view bash_fix_lstrip_tabs(kimix::string_view line) noexcept {
    size_t i = 0;
    while (i < line.size() && line[i] == '\t') {
        ++i;
    }
    return line.substr(i);
}

kimix::string bash_fix_join(const kimix::vector<kimix::string> &parts,
                            kimix::string_view sep) {
    kimix::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out.append(sep.data(), sep.size());
        }
        out.append(parts[i].data(), parts[i].size());
    }
    return out;
}

char bash_upper_ascii(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
}

// Deduplicate while preserving first-seen order (dict.fromkeys).
void bash_fix_unique_in_order(const kimix::vector<kimix::string> &in,
                              kimix::vector<kimix::string> &out) {
    out.clear();
    for (const kimix::string &value : in) {
        bool seen = false;
        for (const kimix::string &existing : out) {
            if (existing == value) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            out.push_back(value);
        }
    }
}

// _fix_heredoc_trailing_operators is defined after the scanner (it runs its
// own scan); forward-declared here because the scanner's shell-wrapper repair
// rescans inline scripts with it.
bool bash_fix_fix_heredoc_trailing_operators(kimix::string &source,
                                             kimix::string_view temp_dir);


// ---------------------------------------------------------------------------
// The scanner (_BashFixScanner)
// ---------------------------------------------------------------------------

struct bash_fix_edit {
    size_t start = 0;
    size_t end = 0;
    kimix::string replacement;
};

struct bash_fix_wrapper {
    bash_fix_wrapper_kind kind = bash_fix_wrapper_kind::none;
    bool skip_next = false;
    bool opaque = false;
    bool path_value = false;
    int64_t operands = 0;
};

struct bash_fix_heredoc {
    kimix::optional<kimix::string> delimiter; // nullopt == unmatchable delimiter
    bool strip_tabs = false;
    bool expands = false;
};

enum class bash_fix_case_state : uint8_t { word, await_in, patterns, body };

enum class bash_fix_wrapper_action : uint8_t { skip, inspect, command };

struct bash_fix_scanner {
    static constexpr size_t npos = kimix::string_view::npos;
    // _MAX_NESTING_DEPTH (see the section header note 2).
    static constexpr size_t kMaxDepth = 1024;
    // Stack budget for the recursion; exceeding it takes the reference's
    // RecursionError path (the command is returned byte-for-byte). The value
    // stays well below the smallest stack the scanner can run on (1 MiB is the
    // Windows default for the tests and the agent binaries) while covering
    // every realistic command: an unoptimized C++ frame chain costs ~5 KiB per
    // ``$( )`` level, so 384 KiB already allows ~75 nested substitutions
    // (optimized builds reach several hundred). The reference's own
    // _MAX_NESTING_DEPTH bound (1024 levels) is kept as the upper limit.
    static constexpr ptrdiff_t kStackBudget = 384 << 10;

    kimix::string_view s;
    size_t n = 0;
    kimix::vector<bash_fix_edit> edits;
    kimix::vector<kimix::string> names;
    kimix::vector<kimix::string> path_notes;
    kimix::vector<kimix::string> shell_notes;
    kimix::vector<std::pair<size_t, size_t>> heredoc_events;
    size_t nest_depth = 0;
    kimix::vector<kimix::string> nul_fixes;
    kimix::vector<kimix::string> unsupported;
    bool aborted = false;
    const char *stack_base = nullptr;
    // git bash virtual mount resolution: injected by the caller, resolved from
    // the environment when empty.
    kimix::string temp_dir;

    // -- primitives ---------------------------------------------------------

    bool starts_at(size_t i, kimix::string_view lit) const noexcept {
        return i <= n && lit.size() <= n - i &&
               std::memcmp(s.data() + i, lit.data(), lit.size()) == 0;
    }

    bool raw_starts_at(kimix::string_view raw, size_t i, kimix::string_view lit) const noexcept {
        return i <= raw.size() && lit.size() <= raw.size() - i &&
               std::memcmp(raw.data() + i, lit.data(), lit.size()) == 0;
    }

    // Python s.find(c, from, end): bounded search returning npos when absent.
    size_t find_char(char c, size_t from, size_t end) const noexcept {
        if (from >= end) {
            return npos;
        }
        const size_t r = s.find(c, from);
        return (r == npos || r >= end) ? npos : r;
    }

    bool stack_exhausted() const noexcept {
        if (stack_base == nullptr) {
            return false;
        }
        char probe = 0;
        const ptrdiff_t used = stack_base - &probe;
        return (used < 0 ? -used : used) > kStackBudget;
    }

    // -- literal word values (Bash quote removal only) -----------------------

    // _literal_word_value: the word value produced solely by Bash quote
    // removal; nullopt when the word needs an expansion or is malformed.
    kimix::optional<kimix::string> literal_word_value(kimix::string_view raw) const {
        kimix::string value;
        size_t i = 0;
        const size_t len = raw.size();
        while (i < len) {
            const char ch = raw[i];
            if (ch == '\\') {
                if (i + 1 >= len) {
                    return std::nullopt;
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
                if (close == npos) {
                    return std::nullopt;
                }
                value.append(raw.data() + i + 1, close - i - 1);
                i = close + 1;
                continue;
            }
            if (ch == '"') {
                ++i;
                while (i < len && raw[i] != '"') {
                    const char inner = raw[i];
                    if (inner == '$' || inner == '`') {
                        return std::nullopt;
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
                    ++i;
                }
                if (i >= len) {
                    return std::nullopt;
                }
                ++i;
                continue;
            }
            if (ch == '$' || ch == '`' || ch == '*' || ch == '?' || ch == '[' ||
                ch == '{' || ch == '~') {
                return std::nullopt;
            }
            value.push_back(ch);
            ++i;
        }
        return value;
    }

    // _literal_command_name / _literal_unsupported_name / _shell_wrapper_name.
    bool literal_command_name(kimix::string_view raw, kimix::string &name) const {
        const kimix::optional<kimix::string> value = literal_word_value(raw);
        if (!value.has_value() || !bash_fix_is_fallback_name(*value)) {
            return false;
        }
        name = *value;
        return true;
    }

    bool literal_unsupported_name(kimix::string_view raw, kimix::string &name) const {
        const kimix::optional<kimix::string> value = literal_word_value(raw);
        if (!value.has_value() || bash_fix_reason_of(*value) == nullptr) {
            return false;
        }
        name = *value;
        return true;
    }

    bool literal_shell_wrapper_name(kimix::string_view raw,
                                    kimix::string &name) const {
        const kimix::optional<kimix::string> value = literal_word_value(raw);
        if (!value.has_value() || !bash_fix_shell_wrapper_name_of(*value)) {
            return false;
        }
        name = *value;
        return true;
    }

    // _plausible_script_file: a script path, not a redundant command word.
    static bool plausible_script_file(kimix::string_view raw) {
        if (raw.size() >= 2 && raw[0] == '.' && raw[1] == '/') {
            return true;
        }
        if (raw.size() >= 3 && raw[0] == '.' && raw[1] == '.' && raw[2] == '/') {
            return true;
        }
        if (raw.size() >= 2 && raw[0] == '.' && raw[1] == '\\') {
            return true;
        }
        if (raw.size() >= 3 && raw[0] == '.' && raw[1] == '.' && raw[2] == '\\') {
            return true;
        }
        if (raw.find('/') != npos || raw.find('\\') != npos) {
            return true;
        }
        static const char *const kExtensions[] = {".sh",  ".bash", ".zsh", ".ksh",
                                                 ".dash", ".ash",  ".bats"};
        const size_t len = raw.size();
        for (const char *ext : kExtensions) {
            const size_t ext_len = std::strlen(ext);
            if (len < ext_len) {
                continue;
            }
            bool match = true;
            for (size_t k = 0; k < ext_len; ++k) {
                if (bash_lower_ascii(raw[len - ext_len + k]) != ext[k]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return true;
            }
        }
        return false;
    }

    // -- Python regex equivalents -------------------------------------------

    // _ASSIGNMENT_RE = ^[A-Za-z_][A-Za-z0-9_]*(?:\+)?= (match at the start).
    static bool assignment_re(kimix::string_view raw) noexcept {
        if (raw.empty()) {
            return false;
        }
        const char c0 = raw[0];
        if (!(bash_is_alpha(c0) || c0 == '_')) {
            return false;
        }
        size_t i = 1;
        while (i < raw.size() && (bash_is_alpha(raw[i]) || bash_is_digit(raw[i]) ||
                                  raw[i] == '_')) {
            ++i;
        }
        if (i < raw.size() && raw[i] == '+') {
            ++i;
        }
        return i < raw.size() && raw[i] == '=';
    }

    // _NAME_RE = ^[A-Za-z_][A-Za-z0-9_]*$ (fullmatch).
    static bool name_re(kimix::string_view raw) noexcept {
        if (raw.empty()) {
            return false;
        }
        const char c0 = raw[0];
        if (!(bash_is_alpha(c0) || c0 == '_')) {
            return false;
        }
        for (size_t i = 1; i < raw.size(); ++i) {
            if (!(bash_is_alpha(raw[i]) || bash_is_digit(raw[i]) || raw[i] == '_')) {
                return false;
            }
        }
        return true;
    }

    // _PATH_DRIVE_RE = [A-Za-z]:\\.* (fullmatch).
    static bool path_drive_re(kimix::string_view raw) noexcept {
        return raw.size() >= 3 && bash_is_alpha(raw[0]) && raw[1] == ':' &&
               raw[2] == '\\';
    }

    // _PATH_SEGMENT_RE = [A-Za-z0-9_.~\\-]+ (fullmatch).
    static bool path_segment_re(kimix::string_view raw) noexcept {
        if (raw.empty()) {
            return false;
        }
        for (const char c : raw) {
            const bool ok = bash_is_alpha(c) || bash_is_digit(c) || c == '_' ||
                            c == '.' || c == '~' || c == '\\' || c == '-';
            if (!ok) {
                return false;
            }
        }
        return true;
    }

    // -- small scanners -----------------------------------------------------

    size_t skip_single_quote(size_t i, size_t end) const noexcept {
        const size_t close = find_char('\'', i, end);
        return (close == npos) ? end : close + 1;
    }

    size_t skip_ansi_quote(size_t i, size_t end) const noexcept {
        while (i < end) {
            if (s[i] == '\\') {
                i += (i + 1 < end) ? 2 : 1;
            } else if (s[i] == '\'') {
                return i + 1;
            } else {
                ++i;
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
                ++i;
            }
        }
        return end;
    }

    static bool double_quote_escape(char c) noexcept {
        return c == '$' || c == '`' || c == '"' || c == '\\' || c == '\n';
    }

    size_t skip_double_quote(size_t i, size_t end) {
        while (i < end) {
            const char ch = s[i];
            if (ch == '\\' && i + 1 < end && double_quote_escape(s[i + 1])) {
                i += 2;
            } else if (ch == '"') {
                return i + 1;
            } else if (ch == '`') {
                const size_t close = find_backtick_end(i + 1, end);
                scan_range(i + 1, close);
                i = (close < end) ? close + 1 : end;
            } else if (ch == '$' && starts_at(i, "$(") && !starts_at(i, "$((")) {
                const size_t close = find_matching(i + 2, end);
                scan_range(i + 2, (close < end) ? close : end);
                i = (close < end) ? close + 1 : end;
            } else if (ch == '$' && starts_at(i, "${")) {
                i = skip_parameter(i + 2, end);
            } else {
                ++i;
            }
        }
        return end;
    }

    size_t skip_double_quote_for_matching(size_t i, size_t end) {
        while (i < end) {
            const char ch = s[i];
            if (ch == '\\' && i + 1 < end && double_quote_escape(s[i + 1])) {
                i += 2;
            } else if (ch == '"') {
                return i + 1;
            } else if (ch == '`') {
                const size_t close = find_backtick_end(i + 1, end);
                i = (close < end) ? close + 1 : end;
            } else if (ch == '$' && starts_at(i, "$(") && !starts_at(i, "$((")) {
                const size_t close = find_matching(i + 2, end);
                i = (close < end) ? close + 1 : end;
            } else {
                ++i;
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
                ++depth;
                ++i;
            } else if (s[i] == '}') {
                --depth;
                ++i;
                if (depth == 0) {
                    return i;
                }
            } else {
                ++i;
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
            } else if (ch == '$' && starts_at(i, "$(") && !starts_at(i, "$((")) {
                const size_t close = find_matching(i + 2, end);
                scan_range(i + 2, (close < end) ? close : end);
                i = (close < end) ? close + 1 : end;
            } else if (ch == '\'') {
                i = skip_single_quote(i + 1, end);
            } else if (ch == '"') {
                i = skip_double_quote(i + 1, end);
            } else if (ch == '{') {
                ++depth;
                ++i;
            } else if (ch == '}') {
                --depth;
                ++i;
                if (depth == 0) {
                    return i;
                }
            } else {
                ++i;
            }
        }
        return end;
    }

    size_t skip_arithmetic(size_t i, size_t end) {
        size_t depth = 1;
        while (i < end) {
            const char ch = s[i];
            if (ch == '$' && starts_at(i, "$(") && !starts_at(i, "$((")) {
                const size_t close = find_matching(i + 2, end);
                scan_range(i + 2, (close < end) ? close : end);
                i = (close < end) ? close + 1 : end;
            } else if (ch == '(' && starts_at(i, "((")) {
                ++depth;
                i += 2;
            } else if (ch == ')' && starts_at(i, "))")) {
                --depth;
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
                ++i;
            }
        }
        return end;
    }

    // _skip_conditional: skip a ``[[ ... ]]`` expression, scanning its
    // substitutions.
    size_t skip_conditional(size_t i, size_t end) {
        while (i < end) {
            const char ch = s[i];
            if (ch == ']' && starts_at(i, "]]")) {
                return i + 2;
            }
            if (ch == '\\') {
                i += (i + 1 < end) ? 2 : 1;
            } else if (ch == '$' && starts_at(i, "$'")) {
                i = skip_ansi_quote(i + 2, end);
            } else if (ch == '\'') {
                i = skip_single_quote(i + 1, end);
            } else if (ch == '"') {
                i = skip_double_quote(i + 1, end);
            } else if (ch == '`') {
                const size_t close = find_backtick_end(i + 1, end);
                scan_range(i + 1, close);
                i = (close < end) ? close + 1 : end;
            } else if (ch == '$' && starts_at(i, "$(") && !starts_at(i, "$((")) {
                const size_t close = find_matching(i + 2, end);
                scan_range(i + 2, (close < end) ? close : end);
                i = (close < end) ? close + 1 : end;
            } else if (ch == '$' && starts_at(i, "$((")) {
                i = skip_arithmetic(i + 3, end);
            } else {
                ++i;
            }
        }
        return end;
    }

    size_t read_word(size_t start, size_t end, bool scan_substitutions = true) {
        size_t i = start;
        while (i < end) {
            const char ch = s[i];
            if (bash_fix_word_end_char(ch)) {
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
                if (starts_at(i, "$((")) {
                    i = skip_arithmetic(i + 3, end);
                    continue;
                }
                if (starts_at(i, "$(")) {
                    const size_t close = find_matching(i + 2, end);
                    if (scan_substitutions) {
                        scan_range(i + 2, (close < end) ? close : end);
                    }
                    i = (close < end) ? close + 1 : end;
                    continue;
                }
                if (starts_at(i, "${")) {
                    i = scan_substitutions ? skip_parameter(i + 2, end)
                                           : skip_parameter_literal(i + 2, end);
                    continue;
                }
                if (starts_at(i, "$'")) {
                    i = skip_ansi_quote(i + 2, end);
                    continue;
                }
            }
            ++i;
        }
        return i;
    }

    void read_control_operator(size_t i, size_t end, kimix::string_view &op,
                               size_t &op_end) const noexcept {
        op = kimix::string_view();
        op_end = i;
        if (i >= end) {
            return;
        }
        const char ch = s[i];
        if (ch == ';') {
            if (starts_at(i, ";;&")) {
                op = ";;&";
                op_end = i + 3;
            } else if (starts_at(i, ";;")) {
                op = ";;";
                op_end = i + 2;
            } else if (starts_at(i, ";&")) {
                op = ";&";
                op_end = i + 2;
            } else {
                op = ";";
                op_end = i + 1;
            }
            return;
        }
        if (ch == '&') {
            if (starts_at(i, "&&")) {
                op = "&&";
                op_end = i + 2;
            } else {
                op = "&";
                op_end = i + 1;
            }
            return;
        }
        if (ch == '|') {
            if (starts_at(i, "||")) {
                op = "||";
                op_end = i + 2;
            } else if (starts_at(i, "|&")) {
                op = "|&";
                op_end = i + 2;
            } else {
                op = "|";
                op_end = i + 1;
            }
            return;
        }
        if (ch == '(' || ch == ')') {
            op = s.substr(i, 1);
            op_end = i + 1;
        }
    }

    void read_redirection(size_t i, size_t end, kimix::string_view &op,
                          size_t &op_end) const noexcept {
        static const char *const kOps[] = {"&>>", "&>", "<<<", "<<-", "<<",
                                           ">>",  "<>", ">|",  "<&",  ">&",
                                           "<",   ">"};
        op = kimix::string_view();
        op_end = i;
        for (const char *candidate : kOps) {
            const kimix::string_view lit(candidate);
            if (starts_at(i, lit)) {
                op = lit;
                op_end = i + lit.size();
                return;
            }
        }
    }

    bool redirection_after_fd(size_t i, size_t end) const noexcept {
        while (i < end && bash_is_digit(s[i])) {
            ++i;
        }
        return i < end && bash_fix_redirection_start(s[i]);
    }

    bool comment_starts(size_t i, size_t range_start) const noexcept {
        if (i <= range_start) {
            return true;
        }
        const char prev = s[i - 1];
        return prev == ' ' || prev == '\t' || prev == '\r' || prev == '\n' ||
               prev == ';' || prev == '&' || prev == '|' || prev == '(' ||
               prev == ')' || prev == '<' || prev == '>';
    }

    kimix::optional<size_t> empty_parentheses_end(size_t i, size_t end) const noexcept {
        while (i < end && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) {
            ++i;
        }
        if (i >= end || s[i] != '(') {
            return std::nullopt;
        }
        ++i;
        while (i < end && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) {
            ++i;
        }
        if (i < end && s[i] == ')') {
            return i + 1;
        }
        return std::nullopt;
    }

    kimix::optional<size_t> function_declaration_end(kimix::string_view raw, size_t i,
                                                     size_t end) const noexcept {
        if (!name_re(raw)) {
            return std::nullopt;
        }
        return empty_parentheses_end(i, end);
    }


    // -- heredoc delimiters --------------------------------------------------

    // Python chr(v) rendered into the UTF-8 byte string.
    static void append_code_point(kimix::string &out, uint32_t value) {
        if (value < 0x80u) {
            out.push_back(static_cast<char>(value));
        } else if (value < 0x800u) {
            out.push_back(static_cast<char>(0xC0u | (value >> 6)));
            out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
        } else if (value < 0x10000u) {
            out.push_back(static_cast<char>(0xE0u | (value >> 12)));
            out.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (value >> 18)));
            out.push_back(static_cast<char>(0x80u | ((value >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
        }
    }

    // _read_ansi_c_delimiter: decode a ``$'...'`` delimiter body.
    size_t read_ansi_c_delimiter(kimix::string_view raw, size_t i,
                                 kimix::string &value, bool &valid) const {
        static const char kSimpleFrom[] = "abeEfnrtv\\'\"?";
        static const char kSimpleTo[] = {'\a', '\b', '\x1b', '\x1b', '\f', '\n',
                                         '\r', '\t', '\v', '\\', '\'', '"', '?'};
        while (i < raw.size()) {
            if (raw[i] == '\'') {
                return i + 1;
            }
            if (raw[i] != '\\' || i + 1 >= raw.size()) {
                value.push_back(raw[i]);
                ++i;
                continue;
            }
            const char escape = raw[i + 1];
            const char *simple = std::strchr(kSimpleFrom, escape);
            if (simple != nullptr && escape != '\0') {
                value.push_back(kSimpleTo[simple - kSimpleFrom]);
                i += 2;
                continue;
            }
            if (escape >= '0' && escape <= '7') {
                size_t j = i + 1;
                while (j < raw.size() && j < i + 4 && raw[j] >= '0' && raw[j] <= '7') {
                    ++j;
                }
                uint32_t octal = 0;
                for (size_t k = i + 1; k < j; ++k) {
                    octal = octal * 8u + static_cast<uint32_t>(raw[k] - '0');
                }
                append_code_point(value, octal);
                i = j;
                continue;
            }
            if (escape == 'x' || escape == 'X' || escape == 'u' || escape == 'U') {
                const size_t width = (escape == 'u') ? 4u : (escape == 'U') ? 8u : 2u;
                size_t j = i + 2;
                const size_t limit = (raw.size() < j + width) ? raw.size() : j + width;
                while (j < limit && bash_is_hex_digit(raw[j])) {
                    ++j;
                }
                if (j > i + 2) {
                    uint32_t code = 0;
                    for (size_t k = i + 2; k < j; ++k) {
                        const char c = raw[k];
                        uint32_t digit = 0;
                        if (c >= '0' && c <= '9') {
                            digit = static_cast<uint32_t>(c - '0');
                        } else if (c >= 'a' && c <= 'f') {
                            digit = static_cast<uint32_t>(c - 'a' + 10);
                        } else if (c >= 'A' && c <= 'F') {
                            digit = static_cast<uint32_t>(c - 'A' + 10);
                        }
                        code = code * 16u + digit;
                    }
                    if (code <= 0x10FFFFu && !(code >= 0xD800u && code <= 0xDFFFu)) {
                        append_code_point(value, code);
                    } else {
                        // Bash accepts byte sequences outside Python's Unicode
                        // scalar range; Python flags the delimiter unmatchable.
                        valid = false;
                        value.append(raw.data() + i, j - i);
                    }
                    i = j;
                    continue;
                }
            }
            value.push_back('\\');
            value.push_back(escape);
            i += 2;
        }
        return i;
    }

    // _heredoc_delimiter: (delimiter-or-none, expands) or false for an empty
    // word (the reference returns None for empty input).
    bool heredoc_delimiter(kimix::string_view raw, kimix::optional<kimix::string> &delimiter,
                           bool &expands) const {
        if (raw.empty()) {
            return false;
        }
        kimix::string result;
        bool quoted = false;
        bool matchable = true;
        size_t i = 0;
        while (i < raw.size()) {
            if (raw_starts_at(raw, i, "$'")) {
                quoted = true;
                bool valid = true;
                i = read_ansi_c_delimiter(raw, i + 2, result, valid);
                matchable = matchable && valid;
                continue;
            }
            const char ch = raw[i];
            if (ch == '\'') {
                quoted = true;
                const size_t close = raw.find('\'', i + 1);
                if (close == npos) {
                    result.append(raw.data() + i + 1, raw.size() - i - 1);
                    i = raw.size();
                } else {
                    result.append(raw.data() + i + 1, close - i - 1);
                    i = close + 1;
                }
                continue;
            }
            if (ch == '"') {
                quoted = true;
                ++i;
                while (i < raw.size() && raw[i] != '"') {
                    if (raw[i] == '\\' && i + 1 < raw.size()) {
                        const char escaped = raw[i + 1];
                        if (double_quote_escape(escaped)) {
                            if (escaped != '\n') {
                                result.push_back(escaped);
                            }
                            i += 2;
                            continue;
                        }
                    }
                    result.push_back(raw[i]);
                    ++i;
                }
                if (i < raw.size()) {
                    ++i;
                }
                continue;
            }
            if (ch == '\\' && i + 1 < raw.size()) {
                quoted = true;
                result.push_back(raw[i + 1]);
                i += 2;
                continue;
            }
            result.push_back(ch);
            ++i;
        }
        if (matchable) {
            delimiter = result;
        } else {
            delimiter = kimix::optional<kimix::string>();
        }
        expands = !quoted;
        return true;
    }

    static bool heredoc_line_continues(kimix::string_view line) noexcept {
        size_t trailing = 0;
        size_t i = line.size();
        while (i > 0 && line[i - 1] == '\\') {
            ++trailing;
            --i;
        }
        return (trailing % 2) == 1;
    }

    // _skip_heredoc_bodies: consume the bodies of *documents* and record the
    // (redirection-line end, terminator end) events used by the trailing
    // operator repair.
    size_t skip_heredoc_bodies(size_t i, size_t end,
                               kimix::vector<bash_fix_heredoc> &documents,
                               bool scan_expansions = true) {
        size_t redir_line_end = npos;
        for (const bash_fix_heredoc &document : documents) {
            const size_t body_start = i;
            if (redir_line_end == npos && body_start > 0) {
                // Reference: redir_line_end stays -1 (no event) when the body
                // would start at offset 0.
                redir_line_end = body_start - 1;
            }
            kimix::string logical_line;
            size_t logical_start = i;
            bool terminated = false;
            while (i < end) {
                const size_t newline = find_char('\n', i, end);
                const size_t line_end = (newline == npos) ? end : newline;
                const kimix::string_view line = s.substr(i, line_end - i);
                const kimix::string_view compare =
                    document.strip_tabs ? bash_fix_lstrip_tabs(line) : line;
                if (logical_line.empty()) {
                    logical_start = i;
                }
                if (document.expands && heredoc_line_continues(compare)) {
                    logical_line.append(compare.data(), compare.size() - 1);
                    i = (newline == npos) ? end : newline + 1;
                    continue;
                }
                logical_line.append(compare.data(), compare.size());
                if (document.delimiter.has_value() &&
                    logical_line == *document.delimiter) {
                    if (scan_expansions && document.expands) {
                        scan_heredoc_expansions(body_start, logical_start);
                    }
                    i = (newline == npos) ? end : newline + 1;
                    terminated = true;
                    break;
                }
                logical_line.clear();
                i = (newline == npos) ? end : newline + 1;
            }
            if (!terminated && scan_expansions && document.expands) {
                scan_heredoc_expansions(body_start, end);
            }
        }
        if (redir_line_end != npos) {
            heredoc_events.push_back(std::make_pair(redir_line_end, i));
        }
        return i;
    }

    // _scan_heredoc_expansions: only a backslash suppresses expansion in a
    // heredoc body; quote characters are literal there.
    void scan_heredoc_expansions(size_t i, size_t end) {
        while (i < end) {
            const char ch = s[i];
            if (ch == '\\') {
                i += (i + 1 < end) ? 2 : 1;
            } else if (ch == '`') {
                const size_t close = find_backtick_end(i + 1, end);
                scan_range(i + 1, close);
                i = (close < end) ? close + 1 : end;
            } else if (ch == '$' && starts_at(i, "$(") && !starts_at(i, "$((")) {
                const size_t close = find_matching(i + 2, end);
                scan_range(i + 2, (close < end) ? close : end);
                i = (close < end) ? close + 1 : end;
            } else if (ch == '$' && starts_at(i, "$((")) {
                i = skip_arithmetic(i + 3, end);
            } else if (ch == '$' && starts_at(i, "${")) {
                i = skip_parameter(i + 2, end);
            } else {
                ++i;
            }
        }
    }

    // _scan_array_words: array elements are data words.
    void scan_array_words(size_t i, size_t end) {
        while (i < end) {
            const char ch = s[i];
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                ++i;
                continue;
            }
            if (ch == '\\' && i + 1 < end && s[i + 1] == '\n') {
                i += 2;
                continue;
            }
            if (ch == '#' && comment_starts(i, 0)) {
                const size_t newline = find_char('\n', i + 1, end);
                i = (newline == npos) ? end : newline;
                continue;
            }
            const size_t word_end = read_word(i, end);
            if (word_end <= i) {
                ++i;
                continue;
            }
            const kimix::string_view raw = s.substr(i, word_end - i);
            kimix::string replacement;
            if (path_replacement(raw, replacement)) {
                bash_fix_edit e;
                e.start = i;
                e.end = word_end;
                e.replacement = std::move(replacement);
                edits.push_back(std::move(e));
                path_notes.push_back(kimix::string(raw));
            }
            i = word_end;
        }
    }

    // -- recursion-bounded entry points -------------------------------------

    void scan_range(size_t start, size_t end) {
        if (aborted) {
            return;
        }
        if (nest_depth >= kMaxDepth) {
            // Reference: content beyond the bound is left byte-for-byte.
            return;
        }
        if (stack_exhausted()) {
            aborted = true;
            return;
        }
        ++nest_depth;
        scan_range_inner(start, end);
        --nest_depth;
    }

    size_t find_matching(size_t i, size_t end) {
        if (aborted || nest_depth >= kMaxDepth) {
            return end;
        }
        if (stack_exhausted()) {
            aborted = true;
            return end;
        }
        ++nest_depth;
        const size_t result = find_matching_inner(i, end);
        --nest_depth;
        return result;
    }

    // _find_matching_inner: locate the ``)`` matching the ``$(`` at i - 2.
    size_t find_matching_inner(size_t i, size_t end) {
        const char closing = ')';
        size_t depth = 0;
        kimix::vector<bash_fix_heredoc> pending_heredocs;
        kimix::vector<bash_fix_case_state> case_stack;
        while (i < end) {
            const char ch = s[i];
            if (ch == '\\') {
                i += (i + 1 < end) ? 2 : 1;
                continue;
            }
            if (ch == '\n') {
                ++i;
                if (!pending_heredocs.empty()) {
                    i = skip_heredoc_bodies(i, end, pending_heredocs, false);
                    pending_heredocs.clear();
                }
                continue;
            }
            if (ch == '$' && starts_at(i, "$((")) {
                i = skip_arithmetic(i + 3, end);
                if (!case_stack.empty() &&
                    case_stack.back() == bash_fix_case_state::word) {
                    case_stack.back() = bash_fix_case_state::await_in;
                }
                continue;
            }
            if (ch == '<' && starts_at(i, "<<") && !starts_at(i, "<<<")) {
                const bool strip_tabs = starts_at(i, "<<-");
                size_t delimiter_start = i + (strip_tabs ? 3 : 2);
                while (delimiter_start < end &&
                       (s[delimiter_start] == ' ' || s[delimiter_start] == '\t' ||
                        s[delimiter_start] == '\r')) {
                    ++delimiter_start;
                }
                const size_t delimiter_end = read_word(delimiter_start, end, false);
                kimix::optional<kimix::string> delimiter;
                bool expands = false;
                if (heredoc_delimiter(
                        s.substr(delimiter_start, delimiter_end - delimiter_start),
                        delimiter, expands)) {
                    bash_fix_heredoc document;
                    document.delimiter = std::move(delimiter);
                    document.strip_tabs = strip_tabs;
                    document.expands = expands;
                    pending_heredocs.push_back(std::move(document));
                }
                i = (delimiter_end > delimiter_start) ? delimiter_end : delimiter_start;
                continue;
            }
            if (ch == '\'') {
                i = skip_single_quote(i + 1, end);
                continue;
            }
            if (ch == '"') {
                i = skip_double_quote_for_matching(i + 1, end);
                continue;
            }
            if (ch == '`') {
                const size_t close = find_backtick_end(i + 1, end);
                i = (close < end) ? close + 1 : end;
                if (!case_stack.empty() &&
                    case_stack.back() == bash_fix_case_state::word) {
                    case_stack.back() = bash_fix_case_state::await_in;
                }
                continue;
            }
            if (ch == '#' && comment_starts(i, 0)) {
                const size_t newline = find_char('\n', i + 1, end);
                i = (newline == npos) ? end : newline;
                continue;
            }
            if (ch == ';' && starts_at(i, ";;&")) {
                if (!case_stack.empty()) {
                    case_stack.back() = bash_fix_case_state::patterns;
                }
                i += 3;
                continue;
            }
            if (ch == ';' && (starts_at(i, ";;") || starts_at(i, ";&"))) {
                if (!case_stack.empty()) {
                    case_stack.back() = bash_fix_case_state::patterns;
                }
                i += 2;
                continue;
            }
            if (!bash_fix_word_end_char(ch)) {
                const size_t word_end = read_word(i, end, false);
                if (word_end <= i) {
                    ++i;
                    continue;
                }
                const kimix::string_view word = s.substr(i, word_end - i);
                if (word == "case") {
                    case_stack.push_back(bash_fix_case_state::word);
                } else if (!case_stack.empty() &&
                           (case_stack.back() == bash_fix_case_state::patterns ||
                            case_stack.back() == bash_fix_case_state::body) &&
                           word == "esac") {
                    case_stack.pop_back();
                } else if (!case_stack.empty() &&
                           case_stack.back() == bash_fix_case_state::word) {
                    case_stack.back() = bash_fix_case_state::await_in;
                } else if (!case_stack.empty() &&
                           case_stack.back() == bash_fix_case_state::await_in &&
                           word == "in") {
                    case_stack.back() = bash_fix_case_state::patterns;
                }
                i = word_end;
                continue;
            }
            if (ch == '(') {
                ++depth;
                ++i;
                continue;
            }
            if (ch == closing) {
                if (!case_stack.empty() &&
                    case_stack.back() == bash_fix_case_state::patterns) {
                    case_stack.back() = bash_fix_case_state::body;
                    ++i;
                } else if (depth == 0) {
                    return i;
                } else {
                    --depth;
                    ++i;
                }
                continue;
            }
            ++i;
        }
        return end;
    }


    // -- Windows / Git Bash path normalization ------------------------------

    static bool plausible_path_segments(kimix::string_view raw) noexcept {
        // any(len(segment) >= 2 and segment[0].isalpha() for seg in raw.split("\\"))
        size_t start = 0;
        while (start <= raw.size()) {
            const size_t slash = raw.find('\\', start);
            const size_t end = (slash == npos) ? raw.size() : slash;
            if (end - start >= 2 && bash_is_alpha(raw[start])) {
                return true;
            }
            if (slash == npos) {
                break;
            }
            start = slash + 1;
        }
        return false;
    }

    static kimix::string decode_unquoted_word(kimix::string_view raw) {
        kimix::string value;
        size_t i = 0;
        while (i < raw.size()) {
            if (raw[i] == '\\' && i + 1 < raw.size()) {
                value.push_back(raw[i + 1]);
                i += 2;
            } else {
                value.push_back(raw[i]);
                ++i;
            }
        }
        return value;
    }

    static kimix::string normalize_windows_path(kimix::string_view raw) {
        kimix::string out;
        size_t i = 0;
        const size_t len = raw.size();
        if (len >= 2 && raw[0] == '\\' && raw[1] == '\\') {
            out.append("//", 2);
            i = 2;
        }
        while (i < len) {
            const char ch = raw[i];
            if (ch == '\\' && i + 1 < len) {
                const char next = raw[i + 1];
                if (next == '\\') {
                    out.push_back('/');
                } else if (bash_fix_escaped_literal_char(next)) {
                    out.push_back(next);
                } else {
                    out.push_back('/');
                    out.push_back(next);
                }
                i += 2;
            } else if (ch == '\\') {
                out.push_back('/');
                ++i;
            } else {
                out.push_back(ch);
                ++i;
            }
        }
        return out;
    }

    static kimix::string quote_path_word(kimix::string_view normalized) {
        bool safe = true;
        for (const char c : normalized) {
            if (!bash_fix_path_safe_char(c)) {
                safe = false;
                break;
            }
        }
        if (safe) {
            return kimix::string(normalized);
        }
        if (!normalized.empty() && normalized[0] == '~') {
            kimix::string out = "~";
            out += quote_path_word(normalized.substr(1));
            return out;
        }
        kimix::string escaped;
        for (const char c : normalized) {
            switch (c) {
            case '\\':
                escaped.append("\\\\", 2);
                break;
            case '"':
                escaped.append("\\\"", 2);
                break;
            case '$':
                escaped.append("\\$", 2);
                break;
            case '`':
                escaped.append("\\`", 2);
                break;
            default:
                escaped.push_back(c);
                break;
            }
        }
        kimix::string out = "\"";
        out += escaped;
        out += "\"";
        return out;
    }

    // _windows_path_replacement.
    bool windows_path_replacement(kimix::string_view raw, kimix::string &out) const {
        if (raw.empty() || raw.find('\\') == npos) {
            return false;
        }
        size_t backslashes = 0;
        for (const char ch : raw) {
            if (ch == '\\') {
                ++backslashes;
            } else if (ch == '\'' || ch == '"' || ch == '`' || ch == '$' ||
                       ch == '\n' || ch == '\r') {
                return false;
            }
        }
        const bool unc = raw.size() > 2 && raw[0] == '\\' && raw[1] == '\\';
        if (path_drive_re(raw)) {
            // pass: drive-absolute
        } else if (unc) {
            // pass: UNC share
        } else if (raw[0] == '\\' && backslashes >= 2) {
            if (!plausible_path_segments(raw)) {
                return false;
            }
        } else if (raw.size() >= 2 && raw[0] == '~' && raw[1] == '\\') {
            // pass: home-relative
        } else if (raw.size() >= 2 && raw[0] == '.' && raw[1] == '\\') {
            // pass: dot-relative
        } else if (raw.size() >= 3 && raw[0] == '.' && raw[1] == '.' && raw[2] == '\\') {
            // pass: dot-relative
        } else if (backslashes >= 2) {
            const kimix::string decoded = decode_unquoted_word(raw);
            if (decoded.size() < 2) {
                return false;
            }
            bool has_alnum = false;
            for (const char c : decoded) {
                if (bash_is_alpha(c) || bash_is_digit(c)) {
                    has_alnum = true;
                    break;
                }
            }
            if (!has_alnum || !path_segment_re(decoded) ||
                !plausible_path_segments(raw)) {
                return false;
            }
        } else {
            return false;
        }
        out = quote_path_word(normalize_windows_path(raw));
        return true;
    }

    // _git_bash_abs_path_replacement.
    bool git_bash_abs_path_replacement(kimix::string_view raw, kimix::string &out) const {
        if (raw.empty()) {
            return false;
        }
        if (raw.size() >= 4 && raw.substr(0, 4) == "/tmp") {
            if (raw.size() != 4 && !(raw.size() >= 5 && raw[4] == '/')) {
                return false;
            }
            kimix::string resolved = temp_dir;
            resolved.append(raw.data() + 4, raw.size() - 4);
            out = quote_path_word(resolved);
            return true;
        }
        if (raw.size() >= 3 && raw[0] == '/' && bash_is_alpha(raw[1]) && raw[2] == '/') {
            kimix::string resolved;
            resolved.push_back(bash_upper_ascii(raw[1])); // raw[1].upper()
            resolved.push_back(':');
            resolved.append(raw.data() + 2, raw.size() - 2);
            out = quote_path_word(resolved);
            return true;
        }
        return false;
    }

    // _path_replacement: backslash paths first, then Git Bash virtual mounts.
    bool path_replacement(kimix::string_view raw, kimix::string &out) const {
        if (windows_path_replacement(raw, out)) {
            return true;
        }
        return git_bash_abs_path_replacement(raw, out);
    }

    // -- ``cd /d <path>`` (cmd.exe flag form) -------------------------------

    void drop_cmd_cd_flag(size_t i, size_t end) {
        size_t j = i;
        while (j < end && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r')) {
            ++j;
        }
        if (j >= end) {
            return;
        }
        const size_t flag_end = read_word(j, end, false);
        if (flag_end <= j) {
            return;
        }
        const kimix::string_view flag = s.substr(j, flag_end - j);
        if (flag != "/d" && flag != "/D") {
            return;
        }
        size_t k = flag_end;
        while (k < end && (s[k] == ' ' || s[k] == '\t' || s[k] == '\r')) {
            ++k;
        }
        if (k >= end || bash_fix_operator_char(s[k]) || s[k] == '#') {
            return;
        }
        bash_fix_edit e;
        e.start = j;
        e.end = flag_end;
        edits.push_back(std::move(e));
        path_notes.push_back(kimix::string("cd /d"));
    }

    // -- command wrappers ---------------------------------------------------

    bash_fix_wrapper_action consume_wrapper_word(bash_fix_wrapper &wrapper,
                                                kimix::string_view raw) const {
        if (wrapper.skip_next) {
            wrapper.skip_next = false;
            return wrapper.opaque ? bash_fix_wrapper_action::inspect
                                  : bash_fix_wrapper_action::skip;
        }
        if (wrapper.opaque) {
            return bash_fix_wrapper_action::inspect;
        }
        if (wrapper.operands > 0 && !(raw.size() >= 1 && raw[0] == '-')) {
            --wrapper.operands;
            return bash_fix_wrapper_action::skip;
        }
        if (wrapper.kind == bash_fix_wrapper_kind::command &&
            (raw == "-v" || raw == "-V")) {
            return bash_fix_wrapper_action::inspect;
        }
        if (wrapper.kind == bash_fix_wrapper_kind::command) {
            bool p_cluster = false;
            if (raw == "-p") {
                p_cluster = true;
            } else if (raw.size() >= 2 && raw[0] == '-' &&
                       !(raw.size() >= 2 && raw[1] == '-')) {
                for (size_t i = 1; i < raw.size(); ++i) {
                    if (raw[i] == 'p') {
                        p_cluster = true;
                        break;
                    }
                }
            }
            if (p_cluster) {
                wrapper.opaque = true;
                return bash_fix_wrapper_action::skip;
            }
        }
        if (wrapper.kind == bash_fix_wrapper_kind::env &&
            (raw == "-S" || raw == "--split-string")) {
            wrapper.opaque = true;
            wrapper.skip_next = true;
            return bash_fix_wrapper_action::skip;
        }
        if (wrapper.kind == bash_fix_wrapper_kind::env) {
            const bool inline_split =
                raw.size() >= 15 && raw.substr(0, 15) == "--split-string=";
            const bool inline_s = raw.size() > 2 && raw[0] == '-' && raw[1] == 'S';
            if (inline_split || inline_s) {
                return bash_fix_wrapper_action::inspect;
            }
        }
        if (raw == "--") {
            return bash_fix_wrapper_action::skip;
        }
        if (bash_fix_wrapper_option_takes_value(wrapper.kind, raw)) {
            wrapper.skip_next = true;
            if (bash_fix_wrapper_path_option(wrapper.kind, raw)) {
                wrapper.path_value = true;
            }
            return bash_fix_wrapper_action::skip;
        }
        if (raw.size() >= 1 && raw[0] == '-') {
            return bash_fix_wrapper_action::skip;
        }
        if (wrapper.kind == bash_fix_wrapper_kind::env && assignment_re(raw)) {
            return bash_fix_wrapper_action::skip;
        }
        return bash_fix_wrapper_action::command;
    }

    // _coproc_name_before_compound: ``coproc NAME { ... }``.
    bool coproc_name_before_compound(kimix::string_view raw, size_t i,
                                     size_t end) const noexcept {
        if (!name_re(raw)) {
            return false;
        }
        while (i < end && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) {
            ++i;
        }
        if (i >= end) {
            return false;
        }
        if (starts_at(i, "{") || starts_at(i, "(") || starts_at(i, "[[") ||
            starts_at(i, "((")) {
            return true;
        }
        static const char *const kKeywords[] = {"case", "for",  "if",
                                                "select", "until", "while"};
        for (const char *keyword : kKeywords) {
            const kimix::string_view kw(keyword);
            const size_t keyword_end = i + kw.size();
            if (starts_at(i, kw) &&
                (keyword_end >= end ||
                 s[keyword_end] == ' ' || s[keyword_end] == '\t' ||
                 s[keyword_end] == '\r' || s[keyword_end] == '\n' ||
                 s[keyword_end] == ';' || s[keyword_end] == '&' ||
                 s[keyword_end] == '|' || s[keyword_end] == '(' ||
                 s[keyword_end] == ')' || s[keyword_end] == '<' ||
                 s[keyword_end] == '>' || s[keyword_end] == '{' ||
                 s[keyword_end] == '}')) {
                return true;
            }
        }
        return false;
    }


    // -- redundant shell wrappers -------------------------------------------

    bool is_nul_word(kimix::string_view raw) const noexcept {
        if (raw.size() != 3) {
            return false;
        }
        if (bash_lower_ascii(raw[0]) != 'n' || bash_lower_ascii(raw[1]) != 'u' ||
            bash_lower_ascii(raw[2]) != 'l') {
            return false;
        }
        for (const char c : raw) {
            if (c == '\'' || c == '"' || c == '\\' || c == '`' || c == '$') {
                return false;
            }
        }
        return true;
    }

    static size_t lstrip_hws(kimix::string_view text, size_t i, size_t end) noexcept {
        while (i < end && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r')) {
            ++i;
        }
        return i;
    }

    // Include *name* in *list* when absent (reference: ``x for x in ... if x
    // not in self.list`` evaluated lazily during extend).
    static void extend_unique(kimix::vector<kimix::string> &list,
                              const kimix::vector<kimix::string> &extra) {
        for (const kimix::string &value : extra) {
            bool seen = false;
            for (const kimix::string &existing : list) {
                if (existing == value) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                list.push_back(value);
            }
        }
    }

    void push_edit(size_t start, size_t end, kimix::string replacement) {
        bash_fix_edit e;
        e.start = start;
        e.end = end;
        e.replacement = std::move(replacement);
        edits.push_back(std::move(e));
    }

    // Adopt the inner scan's recorded names / notes (reference: _handle_shell_
    // wrapper and _watch_command_operand share this block).
    void adopt_inner(bash_fix_scanner &inner) {
        extend_unique(names, inner.names);
        for (const kimix::string &note : inner.path_notes) {
            path_notes.push_back(note);
        }
        extend_unique(shell_notes, inner.shell_notes);
        extend_unique(unsupported, inner.unsupported);
    }

    // _handle_shell_wrapper: repair a redundant ``bash``/``sh`` invocation at
    // command position. Returns true when the wrapper was rewritten; *i is
    // advanced past the repair and *keep_wrapper asks the caller to leave any
    // active command wrapper in place.
    bool handle_shell_wrapper(kimix::string_view shell_name, size_t word_start,
                              size_t &i, size_t end, bool assignment_prefix,
                              bool wrapped, bool &keep_wrapper) {
        keep_wrapper = false;
        if (assignment_prefix) {
            // ``VAR=x bash -c 'echo $VAR'``: the assignment is scoped to the
            // shell process, so the wrapper must stay.
            return false;
        }
        size_t j = lstrip_hws(s, i, end);
        if (j >= end || bash_fix_operator_char(s[j]) || s[j] == '#') {
            // Bare ``bash`` (or ``bash && ...`` / ``bash`` at EOF).
            return false;
        }
        const size_t next_end = read_word(j, end, false);
        if (next_end <= j) {
            return false;
        }
        const kimix::string_view next_raw = s.substr(j, next_end - j);

        if (!next_raw.empty() && next_raw[0] == '-') {
            // Optional leading login flag, then the ``-c`` family; every other
            // option changes shell behaviour and is left for bash.
            size_t opt_end = next_end;
            kimix::string_view opt = next_raw;
            if (opt == "-l" || opt == "-L" || opt == "--login") {
                const size_t cursor = lstrip_hws(s, opt_end, end);
                if (cursor >= end || bash_fix_operator_char(s[cursor]) ||
                    s[cursor] == '#') {
                    return false;
                }
                opt_end = read_word(cursor, end, false);
                if (opt_end <= cursor) {
                    return false;
                }
                opt = s.substr(cursor, opt_end - cursor);
            }
            if (!bash_fix_shell_c_option(opt)) {
                return false;
            }
            const size_t k = lstrip_hws(s, opt_end, end);
            if (k >= end || bash_fix_operator_char(s[k]) || s[k] == '#') {
                return false; // ``bash -c`` with no script
            }
            const size_t script_end = read_word(k, end, false);
            if (script_end <= k) {
                return false;
            }
            const size_t m = lstrip_hws(s, script_end, end);
            if (m < end && !bash_fix_operator_char(s[m]) && s[m] != '#') {
                return false; // trailing script argv: ``$0``/``$1`` semantics
            }
            const kimix::string_view script_raw = s.substr(k, script_end - k);
            const kimix::optional<kimix::string> script = literal_word_value(script_raw);
            if (!script.has_value()) {
                return false; // expansions inside the script: leave for bash
            }
            bash_fix_scanner inner;
            inner.s = *script;
            inner.n = script->size();
            inner.temp_dir = temp_dir;
            inner.stack_base = stack_base;
            inner.scan_range(0, script->size());
            if (inner.aborted) {
                return false;
            }
            kimix::string fixed = inner.build_source();
            bash_fix_fix_heredoc_trailing_operators(fixed, temp_dir);
            adopt_inner(inner);
            if (wrapped) {
                // Keep ``<wrapper> bash -c '<script>'`` and fix the script in
                // place: the wrapper runs bash natively and the nested bash
                // inherits the exported fallback functions.
                if (!inner.edits.empty()) {
                    push_edit(k, script_end, bash_fix_single_quote(fixed));
                }
                i = script_end;
                keep_wrapper = true;
                return true;
            }
            push_edit(word_start, script_end, std::move(fixed));
            shell_notes.push_back(kimix::string(shell_name));
            shell_notes.back() += " -c";
            i = script_end;
            return true;
        }

        if (plausible_script_file(next_raw)) {
            return false;
        }
        // ``bash <command> ...``: drop the redundant shell word (plus the
        // whitespace that separated it from the real command).
        push_edit(word_start, j, kimix::string());
        shell_notes.push_back(kimix::string(shell_name));
        i = j;
        keep_wrapper = wrapped;
        return true;
    }

    // _watch_command_operand: fix the inline script of a quoted ``watch``
    // command operand (procps ``watch`` runs it through ``sh -c``; the Git
    // Bash fallback mirrors that with ``eval "$*"``).
    void watch_command_operand(size_t word_start, size_t word_end,
                               kimix::string_view raw) {
        if (raw.empty() || (raw[0] != '\'' && raw[0] != '"')) {
            return;
        }
        const kimix::optional<kimix::string> script = literal_word_value(raw);
        if (!script.has_value()) {
            return;
        }
        bash_fix_scanner inner;
        inner.s = *script;
        inner.n = script->size();
        inner.temp_dir = temp_dir;
        inner.stack_base = stack_base;
        inner.scan_range(0, script->size());
        if (inner.aborted) {
            return;
        }
        kimix::string fixed = inner.build_source();
        bash_fix_fix_heredoc_trailing_operators(fixed, temp_dir);
        adopt_inner(inner);
        if (!inner.edits.empty()) {
            push_edit(word_start, word_end, bash_fix_single_quote(fixed));
        }
    }

    // -- the command-context scanner ----------------------------------------

    void scan_range_inner(size_t start, size_t end) {
        size_t i = start;
        bool command_expected = true;
        bool redirect_expected = false;
        bool redirect_resume = true;
        kimix::string_view redirect_op;
        bash_fix_wrapper wrapper;
        bool has_wrapper = false;
        kimix::string_view heredoc_operator; // "" | "<<" | "<<-"
        bool herestring_flag = false;
        kimix::vector<bash_fix_heredoc> pending_heredocs;
        kimix::vector<bash_fix_case_state> case_stack;
        bool function_name_expected = false;
        bool function_body_expected = false;
        bool assignment_prefix = false;

        while (i < end) {
            const char ch = s[i];

            if (ch == ' ' || ch == '\t' || ch == '\r') {
                ++i;
                continue;
            }
            if (ch == '\\' && i + 1 < end && s[i + 1] == '\n') {
                i += 2;
                continue;
            }
            if (ch == '\n') {
                ++i;
                if (!pending_heredocs.empty()) {
                    i = skip_heredoc_bodies(i, end, pending_heredocs);
                    pending_heredocs.clear();
                }
                command_expected = true;
                redirect_expected = false;
                redirect_op = kimix::string_view();
                heredoc_operator = kimix::string_view();
                herestring_flag = false;
                has_wrapper = false;
                assignment_prefix = false;
                continue;
            }
            if (ch == '#' && comment_starts(i, start)) {
                const size_t newline = find_char('\n', i + 1, end);
                i = (newline == npos) ? end : newline;
                continue;
            }

            const bool process_substitution =
                bash_fix_redirection_start(ch) &&
                (starts_at(i, "<(") || starts_at(i, ">("));
            if (!process_substitution &&
                (bash_fix_redirection_start(ch) || (ch == '&' && starts_at(i, "&>")) ||
                 (bash_is_digit(ch) && redirection_after_fd(i, end)))) {
                const size_t op_start = i;
                if (bash_is_digit(ch)) {
                    while (i < end && bash_is_digit(s[i])) {
                        ++i;
                    }
                }
                kimix::string_view op;
                size_t op_end = i;
                read_redirection(i, end, op, op_end);
                i = op_end;
                if (!op.empty()) {
                    redirect_resume = command_expected;
                    redirect_expected = true;
                    redirect_op = op;
                    herestring_flag = (op == "<<<");
                    if (op == "<<" || op == "<<-") {
                        heredoc_operator = op;
                    }
                    continue;
                }
                i = op_start;
            }

            if (redirect_expected) {
                size_t word_end = i;
                if (starts_at(i, "<(") || starts_at(i, ">(")) {
                    const size_t close = find_matching(i + 2, end);
                    scan_range(i + 2, (close < end) ? close : end);
                    word_end = (close < end) ? close + 1 : end;
                } else {
                    const bool scan_subs =
                        !(heredoc_operator == "<<" || heredoc_operator == "<<-");
                    word_end = read_word(i, end, scan_subs);
                }
                if (word_end <= i) {
                    ++i;
                    continue;
                }
                if (heredoc_operator == "<<" || heredoc_operator == "<<-") {
                    kimix::optional<kimix::string> delimiter;
                    bool expands = false;
                    if (heredoc_delimiter(s.substr(i, word_end - i), delimiter, expands)) {
                        bash_fix_heredoc document;
                        document.delimiter = std::move(delimiter);
                        document.strip_tabs = (heredoc_operator == "<<-");
                        document.expands = expands;
                        pending_heredocs.push_back(std::move(document));
                    }
                } else if (!herestring_flag) {
                    const kimix::string_view raw_word = s.substr(i, word_end - i);
                    // ``> nul`` / ``2> NUL`` / ``&> nul`` / ``>> nul`` would
                    // create a file literally named ``nul`` in Git Bash; only
                    // an unquoted literal target is rewritten (input
                    // redirections never create a file).
                    if (!redirect_op.empty() && redirect_op[0] != '<' &&
                        is_nul_word(raw_word)) {
                        push_edit(i, word_end, kimix::string("/dev/null"));
                        nul_fixes.push_back(kimix::string(raw_word));
                    } else {
                        kimix::string replacement;
                        if (path_replacement(raw_word, replacement)) {
                            push_edit(i, word_end, std::move(replacement));
                            path_notes.push_back(kimix::string(raw_word));
                        }
                    }
                }
                i = word_end;
                command_expected = redirect_resume;
                redirect_expected = false;
                redirect_op = kimix::string_view();
                heredoc_operator = kimix::string_view();
                continue;
            }

            if (ch == '[' && starts_at(i, "[[")) {
                function_body_expected = false;
                i = skip_conditional(i + 2, end);
                command_expected = false;
                continue;
            }
            if (ch == '(' && starts_at(i, "((")) {
                function_body_expected = false;
                i = skip_arithmetic(i + 2, end);
                command_expected = false;
                continue;
            }
            if (ch == '$' && starts_at(i, "$(") && !starts_at(i, "$((")) {
                const size_t close = find_matching(i + 2, end);
                scan_range(i + 2, (close < end) ? close : end);
                i = (close < end) ? close + 1 : end;
                if (!case_stack.empty() &&
                    case_stack.back() == bash_fix_case_state::word) {
                    case_stack.back() = bash_fix_case_state::await_in;
                }
                command_expected = false;
                continue;
            }
            if (ch == '`') {
                const size_t close = find_backtick_end(i + 1, end);
                scan_range(i + 1, close);
                i = (close < end) ? close + 1 : end;
                if (!case_stack.empty() &&
                    case_stack.back() == bash_fix_case_state::word) {
                    case_stack.back() = bash_fix_case_state::await_in;
                }
                command_expected = false;
                continue;
            }
            if (bash_fix_redirection_start(ch) &&
                (starts_at(i, "<(") || starts_at(i, ">("))) {
                const size_t close = find_matching(i + 2, end);
                scan_range(i + 2, (close < end) ? close : end);
                i = (close < end) ? close + 1 : end;
                if (!case_stack.empty() &&
                    case_stack.back() == bash_fix_case_state::word) {
                    case_stack.back() = bash_fix_case_state::await_in;
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
                    if (!case_stack.empty() &&
                        case_stack.back() == bash_fix_case_state::patterns) {
                        case_stack.back() = bash_fix_case_state::body;
                        command_expected = true;
                    } else {
                        command_expected = false;
                    }
                } else if (op == ";;" || op == ";&" || op == ";;&") {
                    if (!case_stack.empty()) {
                        case_stack.back() = bash_fix_case_state::patterns;
                        command_expected = false;
                    } else {
                        command_expected = true;
                    }
                } else {
                    command_expected = true;
                }
                redirect_expected = false;
                heredoc_operator = kimix::string_view();
                has_wrapper = false;
                assignment_prefix = false;
                continue;
            }

            const size_t word_start = i;
            const bool scan_subs =
                !(heredoc_operator == "<<" || heredoc_operator == "<<-");
            const size_t word_end = read_word(i, end, scan_subs);
            if (word_end <= i) {
                ++i;
                continue;
            }
            const kimix::string_view raw = s.substr(word_start, word_end - word_start);
            i = word_end;

            if (function_name_expected) {
                function_name_expected = false;
                function_body_expected = true;
                command_expected = false;
                const kimix::optional<size_t> declaration_end =
                    empty_parentheses_end(i, end);
                if (declaration_end.has_value()) {
                    i = *declaration_end;
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

            if (!case_stack.empty() && case_stack.back() == bash_fix_case_state::word) {
                case_stack.back() = bash_fix_case_state::await_in;
                command_expected = false;
                continue;
            }
            if (!case_stack.empty() &&
                case_stack.back() == bash_fix_case_state::await_in && raw == "in") {
                case_stack.back() = bash_fix_case_state::patterns;
                command_expected = false;
                continue;
            }
            if (!case_stack.empty() &&
                case_stack.back() == bash_fix_case_state::patterns) {
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
                    if (path_replacement(raw, replacement)) {
                        push_edit(word_start, word_end, std::move(replacement));
                        path_notes.push_back(kimix::string(raw));
                    }
                    if (assignment_re(raw) && i < end && s[i] == '(') {
                        // Array literal as a declaration-builtin argument
                        // (``declare -a arr=( ...)``): its elements are data.
                        const size_t close = find_matching(i + 1, end);
                        scan_array_words(i + 1, (close < end) ? close : end);
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
            const kimix::optional<size_t> declaration_end =
                function_declaration_end(raw, i, end);
            if (declaration_end.has_value()) {
                i = *declaration_end;
                function_body_expected = true;
                command_expected = false;
                continue;
            }
            if (bash_fix_command_start_keyword(raw)) {
                command_expected = true;
                continue;
            }
            if (bash_fix_command_end_keyword(raw)) {
                if (raw == "esac" && !case_stack.empty()) {
                    case_stack.pop_back();
                }
                command_expected = false;
                continue;
            }
            if (bash_fix_list_keyword(raw)) {
                if (raw == "case") {
                    case_stack.push_back(bash_fix_case_state::word);
                }
                command_expected = false;
                continue;
            }
            if (assignment_re(raw)) {
                if (i < end && s[i] == '(') {
                    const size_t close = find_matching(i + 1, end);
                    scan_array_words(i + 1, (close < end) ? close : end);
                    i = (close < end) ? close + 1 : end;
                }
                command_expected = true;
                assignment_prefix = true;
                continue;
            }

            if (raw == "cd") {
                drop_cmd_cd_flag(i, end);
            }

            // Wrapper bookkeeping (``env``/``timeout``/``xargs``/...).
            const bool executable_wrapper =
                has_wrapper && !bash_fix_same_shell_wrapper(wrapper.kind);
            if (has_wrapper && wrapper.kind == bash_fix_wrapper_kind::coproc) {
                if (coproc_name_before_compound(raw, i, end)) {
                    has_wrapper = false;
                    command_expected = true;
                    continue;
                }
            }
            bool inline_consumed = false;
            if (has_wrapper && bash_fix_wrapper_has_path_options(wrapper.kind)) {
                static const char *const kLongOptions[] = {"--chdir", "--output",
                                                           "--arg-file"};
                for (const char *option : kLongOptions) {
                    const kimix::string_view opt(option);
                    if (raw.size() > opt.size() &&
                        raw.substr(0, opt.size()) == opt && raw[opt.size()] == '=') {
                        const kimix::string_view value = raw.substr(opt.size() + 1);
                        kimix::string replacement;
                        if (path_replacement(value, replacement)) {
                            kimix::string combined(opt);
                            combined += "=";
                            combined += replacement;
                            push_edit(word_start, word_end, std::move(combined));
                            path_notes.push_back(kimix::string(raw));
                        }
                        // An inline option also fills a pending value slot and
                        // leaves the wrapper active for the command that
                        // follows it.
                        wrapper.skip_next = false;
                        wrapper.path_value = false;
                        command_expected = true;
                        inline_consumed = true;
                        break;
                    }
                }
            }
            if (inline_consumed) {
                continue;
            }
            if (has_wrapper) {
                const bool path_option_value = wrapper.path_value && wrapper.skip_next;
                const bash_fix_wrapper_action action = consume_wrapper_word(wrapper, raw);
                if (action == bash_fix_wrapper_action::skip) {
                    if (path_option_value) {
                        kimix::string replacement;
                        if (path_replacement(raw, replacement)) {
                            push_edit(word_start, word_end, std::move(replacement));
                            path_notes.push_back(kimix::string(raw));
                        }
                    }
                    command_expected = true;
                    continue;
                }
                if (action == bash_fix_wrapper_action::inspect) {
                    command_expected = false;
                    has_wrapper = false;
                    continue;
                }
                if (wrapper.kind == bash_fix_wrapper_kind::watch) {
                    // ``watch`` re-executes its command in the current shell,
                    // so a quoted operand is an inline script.
                    watch_command_operand(word_start, word_end, raw);
                    has_wrapper = false;
                }
            }

            // Fallback wrappers are checked first: ``sudo`` is both a plain
            // command wrapper and a fallback name whose definition must be
            // recorded for hosts without ``sudo.exe``.
            bash_fix_wrapper_kind fallback_wrapper = bash_fix_wrapper_kind::none;
            if (bash_fix_fallback_wrapper_kind_of(raw, fallback_wrapper)) {
                names.push_back(kimix::string(raw));
                if (executable_wrapper) {
                    // The wrapping executable (``xargs gtimeout ...``) cannot
                    // invoke shell functions: swap the word for the standalone
                    // runner (the wrapped command's names are still recorded
                    // and exported for the runner's nested bash).
                    push_edit(word_start, word_end, bash_fix_wrapper_runner(raw));
                }
                wrapper = bash_fix_wrapper();
                wrapper.kind = fallback_wrapper;
                wrapper.operands = bash_fix_wrapper_operand_count(fallback_wrapper);
                has_wrapper = true;
                command_expected = true;
                continue;
            }
            bash_fix_wrapper_kind wrapper_kind = bash_fix_wrapper_kind::none;
            if (bash_fix_wrapper_kind_of(raw, wrapper_kind)) {
                wrapper = bash_fix_wrapper();
                wrapper.kind = wrapper_kind;
                wrapper.operands = bash_fix_wrapper_operand_count(wrapper_kind);
                has_wrapper = true;
                command_expected = true;
                continue;
            }

            // Redundant shell invocation (``bash cd ...``, ``bash -c '...'``)
            // only at a plain command position.
            kimix::string shell_name;
            if (literal_shell_wrapper_name(raw, shell_name)) {
                bool keep_wrapper = false;
                if (handle_shell_wrapper(shell_name, word_start, i, end,
                                         assignment_prefix, has_wrapper,
                                         keep_wrapper)) {
                    command_expected = true;
                    redirect_expected = false;
                    heredoc_operator = kimix::string_view();
                    if (!keep_wrapper) {
                        has_wrapper = false;
                    }
                    assignment_prefix = false;
                    continue;
                }
            }

            kimix::string fallback_name;
            if (literal_command_name(raw, fallback_name)) {
                names.push_back(fallback_name);
                if (executable_wrapper) {
                    push_edit(word_start, word_end,
                              bash_fix_wrapper_runner(fallback_name));
                }
            } else {
                kimix::string unsupported_name;
                if (literal_unsupported_name(raw, unsupported_name)) {
                    bool recorded = false;
                    for (const kimix::string &existing : unsupported) {
                        if (existing == unsupported_name) {
                            recorded = true;
                            break;
                        }
                    }
                    if (!recorded) {
                        unsupported.push_back(unsupported_name);
                    }
                } else {
                    // A command word can itself be a Windows executable path
                    // (``C:\tools\rg.exe``) or a Git Bash virtual absolute path
                    // (``/c/tools/rg.exe``): quote removal would eat the
                    // backslashes and lose the command.
                    kimix::string replacement;
                    if (path_replacement(raw, replacement)) {
                        push_edit(word_start, word_end, std::move(replacement));
                        path_notes.push_back(kimix::string(raw));
                    }
                }
            }
            command_expected = false;
            has_wrapper = false;
        }
    }

    // _build_source: the source with all recorded edits applied.
    kimix::string build_source() const {
        if (edits.empty()) {
            return kimix::string(s);
        }
        kimix::vector<bash_fix_edit> ordered = edits;
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const bash_fix_edit &a, const bash_fix_edit &b) {
                             if (a.start != b.start) {
                                 return a.start < b.start;
                             }
                             return a.end < b.end;
                         });
        kimix::string out;
        size_t previous = 0;
        for (const bash_fix_edit &e : ordered) {
            out.append(s.data() + previous, e.start - previous);
            out.append(e.replacement);
            previous = e.end;
        }
        out.append(s.data() + previous, s.size() - previous);
        return out;
    }
};


// -- heredoc trailing-operator repair (module-level helpers) ----------------

bool bash_fix_is_heredoc_trailing_operator(kimix::string_view op) noexcept {
    return op == "&&" || op == "||" || op == "|" || op == "|&" || op == ";" ||
           op == "&";
}

// _read_shell_control_operator: note that this helper has NO "()" branch (the
// heredoc repair only recognizes list terminators).
void bash_fix_read_control_operator(kimix::string_view s, size_t i, size_t n,
                                    kimix::string_view &op, size_t &op_end) noexcept {
    op_end = i;
    op = kimix::string_view();
    if (i >= n) {
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
    }
}

size_t bash_fix_skip_blank_and_comments(kimix::string_view s, size_t i,
                                        size_t n) noexcept {
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
            const size_t newline = s.find('\n', i);
            i = (newline == kimix::string_view::npos) ? n : newline + 1;
            continue;
        }
        break;
    }
    return i;
}

// Python str.strip() on the ASCII whitespace set.
kimix::string_view bash_fix_strip_ascii_ws(kimix::string_view s) noexcept {
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

// Python str.splitlines() terminator set restricted to the byte range
// (LF, CR, CRLF, VT, FF, FS, GS, RS, NEL).
bool bash_fix_is_line_break(char c) noexcept {
    return c == '\n' || c == '\r' || c == '\v' || c == '\f' || c == '\x1c' ||
           c == '\x1d' || c == '\x1e' || static_cast<uint8_t>(c) == 0x85u;
}

void bash_fix_splitlines(kimix::string_view text,
                         kimix::vector<kimix::string_view> &out) {
    out.clear();
    size_t start = 0;
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (bash_fix_is_line_break(c)) {
            out.push_back(text.substr(start, i - start));
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            ++i;
            start = i;
        } else {
            ++i;
        }
    }
    if (start < text.size()) {
        out.push_back(text.substr(start, text.size() - start));
    }
}

// _apply_heredoc_operator_move: move a control-operator line following a
// heredoc terminator onto the redirection line (Bash requires the operator that
// continues a heredoc-delimited command to be on the ``<<`` line).
bool bash_fix_apply_heredoc_operator_move(kimix::string &source,
                                          size_t redir_line_end,
                                          size_t terminator_end) {
    const size_t n = source.size();
    if (redir_line_end >= n || source[redir_line_end] != '\n') {
        return false;
    }
    if (terminator_end > n) {
        return false;
    }
    const size_t i = bash_fix_skip_blank_and_comments(source, terminator_end, n);
    if (i >= n) {
        return false;
    }
    kimix::string_view op;
    size_t op_end = i;
    bash_fix_read_control_operator(source, i, n, op, op_end);
    if (op.empty() || !bash_fix_is_heredoc_trailing_operator(op)) {
        return false;
    }
    // ``&>`` / ``&>>`` are redirections, not list terminators.
    if (op == "&" && op_end < n && source[op_end] == '>') {
        return false;
    }

    const size_t move_start = i;
    size_t line_end = source.find('\n', i);
    size_t move_end = 0;
    if (line_end == kimix::string::npos) {
        line_end = n;
        move_end = n;
    } else {
        move_end = line_end + 1;
    }

    size_t rest_start = op_end;
    while (rest_start < line_end && (source[rest_start] == ' ' ||
                                     source[rest_start] == '\t' ||
                                     source[rest_start] == '\r')) {
        ++rest_start;
    }
    const kimix::string_view rest =
        kimix::string_view(source).substr(rest_start, line_end - rest_start);
    if (rest.empty() || rest[0] == '#') {
        const size_t k = bash_fix_skip_blank_and_comments(source, move_end, n);
        if (k >= n) {
            return false;
        }
        const size_t next_line_end = source.find('\n', k);
        move_end = (next_line_end == kimix::string::npos) ? n : next_line_end + 1;
    }

    kimix::vector<kimix::string_view> lines;
    bash_fix_splitlines(kimix::string_view(source).substr(move_start, move_end - move_start),
                        lines);
    kimix::vector<kimix::string_view> parts;
    if (!lines.empty()) {
        parts.push_back(bash_fix_strip_ascii_ws(lines[0].substr(op.size())));
        for (size_t k = 1; k < lines.size(); ++k) {
            parts.push_back(bash_fix_strip_ascii_ws(lines[k]));
        }
    }
    kimix::string joined;
    for (const kimix::string_view part : parts) {
        if (part.empty() || part[0] == '#') {
            continue;
        }
        if (!joined.empty()) {
            joined.push_back(' ');
        }
        joined.append(part.data(), part.size());
    }
    kimix::string moved;
    moved.append(op.data(), op.size());
    if (!joined.empty()) {
        moved.push_back(' ');
        moved += joined;
    }
    moved.push_back('\n');

    kimix::string out;
    out.reserve(source.size() + moved.size());
    out.append(source, 0, redir_line_end);
    out.push_back(' ');
    out += moved;
    if (move_start > redir_line_end + 1) {
        out.append(source, redir_line_end + 1, move_start - (redir_line_end + 1));
    }
    if (move_end < n) {
        out.append(source, move_end, n - move_end);
    }
    source = std::move(out);
    return true;
}

// _fix_heredoc_trailing_operators: repair heredoc commands whose trailing
// control operator sits on the wrong line.
bool bash_fix_fix_heredoc_trailing_operators(kimix::string &source,
                                             kimix::string_view temp_dir) {
    bash_fix_scanner scanner;
    scanner.s = source;
    scanner.n = source.size();
    scanner.temp_dir = kimix::string(temp_dir);
    char base = 0;
    scanner.stack_base = &base;
    scanner.scan_range(0, source.size());
    if (scanner.aborted || scanner.heredoc_events.empty()) {
        return false;
    }
    bool changed = false;
    for (auto it = scanner.heredoc_events.rbegin(); it != scanner.heredoc_events.rend();
         ++it) {
        if (bash_fix_apply_heredoc_operator_move(source, it->first, it->second)) {
            changed = true;
        }
    }
    return changed;
}

// _BashFixScanner.fix(): run the scan and compose the BashFix payload.
bash_fix_result bash_fix_apply(kimix::string_view command,
                               kimix::string_view temp_dir) {
    bash_fix_result result;
    bash_fix_scanner scanner;
    scanner.s = command;
    scanner.n = command.size();
    scanner.temp_dir = kimix::string(temp_dir);
    char base = 0;
    scanner.stack_base = &base;
    scanner.scan_range(0, command.size());
    if (scanner.aborted) {
        // Reference: RecursionError -> the command is returned unchanged.
        result.command = kimix::string(command);
        return result;
    }
    if (scanner.names.empty() && scanner.edits.empty() &&
        scanner.shell_notes.empty() && scanner.nul_fixes.empty() &&
        scanner.unsupported.empty()) {
        result.command = kimix::string(command);
        return result;
    }

    kimix::vector<kimix::string> unique_names;
    bash_fix_unique_in_order(scanner.names, unique_names);
    kimix::string definitions;
    for (size_t k = 0; k < unique_names.size(); ++k) {
        if (k != 0) {
            definitions.push_back('\n');
        }
        definitions += bash_fix_definition(unique_names[k]);
    }
    // Exported fallbacks are inherited by every nested bash; the export is
    // conditional on the function actually being installed (a fallback whose
    // ``command -v`` guard found a real executable installs nothing, and an
    // unconditional ``export -f`` would pollute stderr).
    kimix::string exports;
    for (size_t k = 0; k < unique_names.size(); ++k) {
        if (k != 0) {
            exports.push_back('\n');
        }
        exports += "if declare -F ";
        exports += unique_names[k];
        exports += " >/dev/null; then export -f ";
        exports += unique_names[k];
        exports += "; fi";
    }
    kimix::string source = scanner.build_source();
    bash_fix_fix_heredoc_trailing_operators(source, temp_dir);

    if (!definitions.empty()) {
        result.command = definitions;
        result.command.push_back('\n');
        result.command += exports;
        result.command.push_back('\n');
    }
    result.command += source;
    result.replacements = scanner.names;
    result.path_changes = scanner.path_notes;
    result.shell_wrappers = scanner.shell_notes;
    result.nul_fixes = scanner.nul_fixes;
    result.unsupported_commands = scanner.unsupported;
    return result;
}

} // namespace

// ===========================================================================
// Public API
// ===========================================================================

// BashFix.warning: the human-readable summary of everything the scan changed.
// Composed exactly like the reference property (including the U+2014 em dash in
// the unsupported-command detail and the ``... -> `/dev/null` (...)`` text).
kimix::string bash_fix_result::warning() const {
    kimix::vector<kimix::string> parts;
    if (!unsupported_commands.empty()) {
        kimix::string details;
        for (size_t i = 0; i < unsupported_commands.size(); ++i) {
            if (i != 0) {
                details += "; ";
            }
            details += "`";
            details += unsupported_commands[i];
            details += "` \xE2\x80\x94 ";
            const char *reason = bash_fix_reason_of(unsupported_commands[i]);
            if (reason != nullptr) {
                details += reason;
            }
        }
        kimix::string part =
            "Command(s) with no Windows Git Bash equivalent: ";
        part += details;
        part += ".";
        parts.push_back(std::move(part));
    }
    if (!replacements.empty()) {
        kimix::string names;
        for (size_t i = 0; i < replacements.size(); ++i) {
            if (i != 0) {
                names += ", ";
            }
            names += "`";
            names += replacements[i];
            names += "`";
        }
        kimix::string part =
            "Added Windows Git Bash fallback(s) for native command(s): ";
        part += names;
        part += ".";
        parts.push_back(std::move(part));
    }
    if (!path_changes.empty()) {
        kimix::string words;
        for (size_t i = 0; i < path_changes.size(); ++i) {
            if (i != 0) {
                words += ", ";
            }
            words += "`";
            words += path_changes[i];
            words += "`";
        }
        kimix::string part = "Rewrote Windows path(s) for Git Bash (backslashes "
                             "to forward slashes; Git Bash virtual paths to "
                             "native spellings): ";
        part += words;
        part += ".";
        parts.push_back(std::move(part));
    }
    if (!shell_wrappers.empty()) {
        kimix::string names;
        for (size_t i = 0; i < shell_wrappers.size(); ++i) {
            if (i != 0) {
                names += ", ";
            }
            names += "`";
            names += shell_wrappers[i];
            names += "`";
        }
        kimix::string part = "Removed redundant shell wrapper(s): ";
        part += names;
        part += ".";
        parts.push_back(std::move(part));
    }
    if (!nul_fixes.empty()) {
        kimix::string words;
        for (size_t i = 0; i < nul_fixes.size(); ++i) {
            if (i != 0) {
                words += ", ";
            }
            words += "`";
            words += nul_fixes[i];
            words += "`";
        }
        kimix::string part = "Rewrote null-device redirection target(s) for Git "
                             "Bash: ";
        part += words;
        part += " -> `/dev/null` (an unquoted `nul` would otherwise create an "
                "empty file named `nul`).";
        parts.push_back(std::move(part));
    }
    return bash_fix_join(parts, " ");
}

kimix::string bash_windows_temp_dir() {
    // tempfile.gettempdir(): TMPDIR / TEMP / TMP, then the platform default.
    // (The reference additionally probes candidate writability, and its
    // platform default is ``~\AppData\Local\Temp`` on Windows; both fallbacks
    // are reproduced here without touching the Win32 API.)
    static const char *const kEnvKeys[] = {"TMPDIR", "TEMP", "TMP"};
    for (const char *key : kEnvKeys) {
        const char *value = std::getenv(key);
        if (value != nullptr && value[0] != '\0') {
            kimix::string out(value);
            for (char &c : out) {
                if (c == '\\') {
                    c = '/';
                }
            }
            while (out.size() > 1 && out.back() == '/') {
                out.pop_back();
            }
            return out;
        }
    }
#ifdef KIMIX_PLATFORM_WINDOWS
    if (const char *profile = std::getenv("USERPROFILE")) {
        if (profile[0] != '\0') {
            kimix::string out(profile);
            for (char &c : out) {
                if (c == '\\') {
                    c = '/';
                }
            }
            while (out.size() > 1 && out.back() == '/') {
                out.pop_back();
            }
            out += "/AppData/Local/Temp";
            return out;
        }
    }
#endif
    return kimix::string("/tmp");
}

bool bash_fix_platform_enabled() noexcept {
#ifdef KIMIX_PLATFORM_WINDOWS
    return true;
#else
    return false;
#endif
}

kimix::string bash_compatibility_prelude() {
    // bash_fix.py: the platform gate lives in the app layer.
    if (!bash_fix_platform_enabled()) {
        return kimix::string();
    }
    kimix::string definitions;
    kimix::string exports;
    bool first = true;
    for (const bash_fix_fallback_body &entry : k_bash_fix_fallback_bodies) {
        if (!first) {
            definitions.push_back('\n');
            exports.push_back('\n');
        }
        first = false;
        definitions += bash_fix_definition(entry.name);
        exports += "if declare -F ";
        exports += entry.name;
        exports += " >/dev/null; then export -f ";
        exports += entry.name;
        exports += "; fi";
    }
    definitions.push_back('\n');
    definitions += exports;
    return definitions;
}

kimix::string_view bash_unsupported_reason(kimix::string_view name) noexcept {
    const char *reason = bash_fix_reason_of(name);
    return (reason == nullptr) ? kimix::string_view() : kimix::string_view(reason);
}

bash_fix_result fix_bash_command(kimix::string_view command,
                                 kimix::string_view windows_temp_dir) {
    bash_fix_result result;
    // bash_fix.py: empty input is returned byte-for-byte unchanged.
    if (command.empty()) {
        result.command = kimix::string(command);
        return result;
    }
    // ASCII gate: the reference operates on Python str (Unicode-aware
    // case insensitive comparisons); non-ASCII input is routed to the Python
    // mirror (project-wide convention).
    if (!bash_is_ascii(command)) {
        result.status = tool_status::unsupported;
        result.command = kimix::string(command);
        return result;
    }
    const kimix::string temp_dir =
        windows_temp_dir.empty() ? bash_windows_temp_dir()
                                 : kimix::string(windows_temp_dir);
    result = bash_fix_apply(command, temp_dir);
    // bash_fix.fix_bash_command repeats the heredoc repair on the composed
    // output (the prefix may itself contain heredoc-like text).
    bash_fix_fix_heredoc_trailing_operators(result.command, temp_dir);
    return result;
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
// _bash_subprocess_env: MSYS path-conversion opt-out on Windows, MSYSTEM
// neutralized, pipefail enforced through the command prefix instead).
kimix::vector<kimix::string> bash_native_env() {
    kimix::vector<kimix::string> env;
#ifdef KIMIX_PLATFORM_WINDOWS
    env.push_back("MSYS_NO_PATHCONV=1");
    env.push_back("MSYS2_ARG_CONV_EXCL=*");
    env.push_back("MSYSTEM=");
#endif
    return env;
}

} // namespace

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

    // 4. Execute-mode preflight: shell preparation, Windows Git Bash
    // compatibility fix, and RTK rewrite callbacks. The prepared command is
    // returned in the message so the Python binding can hand it to the
    // subprocess.
    kimix::string prepared = params.cmd;
    if (_cfg.prepare_command) {
        prepared = _cfg.prepare_command(prepared);
    }
    // ``_prepare_command``: native commands get their Git Bash fallbacks and
    // Windows path spellings; a command with no faithful Git Bash equivalent
    // is rejected here with the reason instead of spawning a process that is
    // guaranteed to fail with "command not found".
    if (_cfg.compat_fix_enabled) {
        bash_fix_result fix = fix_bash_command(prepared, _cfg.compat_temp_dir);
        if (fix.has_unsupported()) {
            const kimix::string msg = fix.warning();
            output_block = bash_build_blocked_block(params, "unsupported", msg);
            return {tool_status::unsupported, msg};
        }
        if (fix.status == tool_status::ok) {
            prepared = std::move(fix.command);
        }
        // Non-ASCII input is outside the native scanner's subset (the ASCII
        // gate documented in the fix section): the command is handed to Git
        // Bash unchanged instead of failing the run.
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
            opts.argv.push_back("set -o pipefail 2>/dev/null; " + output_block);
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
                kimix::string out = truncate_lines(rr.output, 500, true, 2);
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
                block.output_truncated = rr.truncated;
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
                    python::session_output_block block;
                    block.task_id = handle.task_id;
                    block.status = "running";
                    block.output = kimix::format("interactive bash started (pid {})",
                                                 handle.pid);
                    output_block = python::build_session_output_block(block);
                }
            }
        } else if (params.task_id.has_value()) {
            const kimix::string &tid = *params.task_id;
            if (params.mode == "send" && !params.cmd.empty()) {
                err = proc::send_task(tid, params.cmd, true);
            }
            if (!err.failed()) {
                const int64_t wait_ms =
                    params.timeout > 0 ? params.timeout * 1000
                                       : (params.wait_for_pattern.has_value() ? 30000 : 5000);
                const proc::task_wait_result tw = proc::wait_task(
                    tid, params.wait_for_pattern.value_or(""), wait_ms);
                kimix::string out;
                proc::read_task(tid, out);
                out = truncate_lines(out, 500, true, 2);
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
}

const kimix::vector<char> &Bash::serialized_result() const {
    return _result;
}


// Static registration: the class name "Bash" is the registry key (see
// tool_registry.h). The schema mirrors the Python BashParams model.
KIMIX_REGISTER_TOOL(
    Bash,
    "Execute a shell command with the system bash (native POSIX syntax). "
    "Modes: 'execute' (bounded foreground run), 'send' (write to a running "
    "interactive task), 'interactive' (start a persistent REPL task).",
    R"JSON({"type":"object","properties":{"cmd":{"type":"string","description":"Shell command to run (POSIX syntax)"},"mode":{"type":"string","enum":["execute","send","interactive"],"description":"execute: run now; send: write to task stdin; interactive: start persistent task"},"timeout":{"type":"integer","description":"Timeout in seconds (default 30)"},"task_id":{"type":"string","description":"Task id for send/interactive continuation"},"wait_for_pattern":{"type":"string","description":"Stop waiting when this literal appears in output"},"max_lines":{"type":"integer","description":"Max output lines to return"}},"required":["cmd"]})JSON");

} // namespace kimix::builtin_tools::bash
