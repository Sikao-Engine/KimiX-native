// Test for the bash built-in tool kernels (builtin_tools/bash_tool.h).
//
// Covers (plans/bash.md §3.1, §3.3, §3.4 + AGENT_TASK.md scope):
// - has_top_level_pipe: quote/subshell/backslash-aware top-level `|`
//   detection, `||` exclusion (golden vectors from output_enhance.py)
// - base_command_name / interpret_exit_code / is_expected_exit:
//   byte-exact message tables incl. the SIGPIPE-141 rule and the U+2014 em
//   dash in the git message (golden vectors from output_enhance.py)
// - find_error_line_index + error_keywords: 1-based first error line with
//   ASCII \b boundaries and case folding (golden vectors from common.py)
// - truncate_lines: head/tail fold, omitted counts, error-context
//   preservation, CRLF/CR handling (golden vectors from common.py)
// - RTK rewrite kernels: split_shell_segments, is_known_rtk_command,
//   rewrite_shell_segment, maybe_rewrite_shell_command_with_rtk
//   (golden vectors from common.py)
// - bounded_append_capture + capture_machine: the pure bounded-run
//   capture/timeout/kill policy state machine (no subprocess spawning)
// - process_exited_banner (common.py ProcessStream completion banner)
#include "ut/ut.hpp"

#include "builtin_tools/bash_tool.h"
#include "builtin_tools/tool.h"
#include "builtin_tools/utf8_util.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools::bash;

namespace {

struct exit_golden {
    const char *command;
    bool has_code;
    int64_t code;
    const char *message; // nullptr == None
    bool expected;
};

struct rtk_golden {
    const char *command;
    bool token_kill;
    bool exclude_read;
    bool pwsh;
    const char *rewritten;
    bool changed;
};

kimix::optional<int64_t> exit_code(const exit_golden &g) {
    return g.has_code ? kimix::optional<int64_t>(g.code) : std::nullopt;
}

kimix::string opt_to_string(kimix::optional<kimix::string> v) {
    return v.has_value() ? *v : kimix::string("<none>");
}

} // namespace

// ---------------------------------------------------------------------------
// Windows Git Bash compatibility fix (bash_fix.py / _shell_compat.py)
//
// The scanner is verified byte-for-byte against the canonical pure-Python
// reference (kimi-agent bin/kimix_native/_shell_compat.py) through generated
// golden vectors: every row carries the reference's replacements /
// path_changes / shell_wrappers / nul_fixes / unsupported tuples, the
// rewritten source and the warning string.  The prefix composition (fallback
// definitions + conditional exports + bash_compatibility_prelude) is covered
// by the prefix goldens.  Regenerate with scripts/gen_bash_fix_data.py.
// ---------------------------------------------------------------------------
#include "bash_fix_goldens.inc"
#include "bash_fix_prefix_goldens.inc"
// ---------------------------------------------------------------------------
// RTK rewrite scanner (kimi-agent src/kimix/tools/common.py)
//
// Same deal as the compat fix: generated, byte-exact vectors covering
// _split_shell_segments, _rewrite_shell_segment, _is_known_rtk_command and
// _maybe_rewrite_shell_command_with_rtk (4 rewrite profiles per command over
// the commands kimi-agent's own suite uses, an adversarial corpus and a
// deterministic fuzz corpus).  Regenerate with
// `python scripts/gen_bash_fix_data.py --rtk`.
// ---------------------------------------------------------------------------
#include "bash_rtk_goldens.inc"

namespace {

// Decode the split-golden encoding: segments joined with \x1e, each segment
// written as "text\x1fsep" (an empty command encodes as "\x1f").
struct rtk_split_segment {
    kimix::string text;
    kimix::string sep;
};

kimix::vector<rtk_split_segment> rtk_decode_segments(kimix::string_view encoded) {
    kimix::vector<rtk_split_segment> out;
    size_t start = 0;
    while (true) {
        const size_t sep_pos = encoded.find('\x1e', start);
        const kimix::string_view part =
            sep_pos == kimix::string_view::npos
                ? encoded.substr(start)
                : encoded.substr(start, sep_pos - start);
        rtk_split_segment seg;
        const size_t us = part.find('\x1f');
        if (us == kimix::string_view::npos) {
            seg.text = kimix::string(part);
        } else {
            seg.text = kimix::string(part.substr(0, us));
            seg.sep = kimix::string(part.substr(us + 1));
        }
        out.push_back(std::move(seg));
        if (sep_pos == kimix::string_view::npos) {
            break;
        }
        start = sep_pos + 1;
    }
    return out;
}

} // namespace

namespace {

// Golden list fields are unit-separator joined ("" == empty list).
kimix::vector<kimix::string> fix_split_field(kimix::string_view field) {
    kimix::vector<kimix::string> out;
    if (field.empty()) {
        return out;
    }
    size_t start = 0;
    while (true) {
        const size_t pos = field.find('\x1f', start);
        if (pos == kimix::string_view::npos) {
            out.push_back(kimix::string(field.substr(start)));
            return out;
        }
        out.push_back(kimix::string(field.substr(start, pos - start)));
        start = pos + 1;
    }
}

bool fix_list_eq(const kimix::vector<kimix::string> &want,
                 const kimix::vector<kimix::string> &got) {
    if (want.size() != got.size()) {
        return false;
    }
    for (size_t i = 0; i < want.size(); ++i) {
        if (want[i] != got[i]) {
            return false;
        }
    }
    return true;
}

kimix::string fix_join_repr(const kimix::vector<kimix::string> &values) {    kimix::string out = "(";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += "`";
        out += values[i];
        out += "`";
    }
    out += ")";
    return out;
}

void fix_report(const char *command, const char *what, const kimix::string_view want,
                const kimix::string_view got) {
    std::fprintf(stderr, "  [bash_fix] %s mismatch for command `%.*s`\n", what,
                 static_cast<int>(strlen(command)), command);
    std::fprintf(stderr, "    want: %.*s\n", static_cast<int>(want.size()), want.data());
    std::fprintf(stderr, "    got : %.*s\n", static_cast<int>(got.size()), got.data());
}

size_t fix_count(kimix::string_view haystack, kimix::string_view needle) {
    size_t count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != kimix::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// The final line of a rewritten command: the source text after the
// definitions/exports prefix.
kimix::string fix_last_line(kimix::string_view command) {
    const size_t pos = command.rfind('\n');
    return kimix::string(pos == kimix::string_view::npos ? command
                                                        : command.substr(pos + 1));
}

// Optional argv[1] substring filter (debugging aid; empty == run everything).
const char *g_fix_filter = nullptr;
bool fix_selected(const char *command) {
    if (g_fix_filter == nullptr || g_fix_filter[0] == '\0') {
        return true;
    }
    return kimix::string_view(command).find(g_fix_filter) != kimix::string_view::npos;
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));
    if (argc > 1 && argv[1][0] != '-' && argv[1][0] != '\0') {
        g_fix_filter = argv[1];
    }

    // =======================================================================
    // 3.1 has_top_level_pipe — golden vectors from output_enhance.py
    // =======================================================================
    "has_top_level_pipe_golden"_test = [] {
        struct {
            const char *cmd;
            bool want;
        } cases[] = {
            {"ls | head", true},
            {"a || b", false},
            {"echo \"a | b\"", false},
            {"echo 'a | b'", false},
            {"echo `a | b`", false},
            {"echo $(a | b)", false},
            {"cat <<EOF\nx | y\nEOF", true},
            {"a | b | c", true},
            {"a && b | c", true},
            {"", false},
            {"   ", false},
            {"echo a | wc -l; echo x", true},
            {"ls > file | head", true},
            {"foo(bar | baz)", false},
            {"a |& b", true},
            {"echo hi | (cat | head)", true},
            {"x | y | head", true},
            {"echo \\|", false},
            {"echo \"\\\"|\\\"\"", false},
            {"echo 'a'|wc", true},
            {"a; b | c", true},
        };
        for (const auto &c : cases) {
            expect(eq(has_top_level_pipe(c.cmd), c.want)) << "cmd=" << c.cmd;
        }
    };

    "has_top_level_pipe_edges"_test = [] {
        // || is not a pipeline; but "a | | b" has a lone | at depth 0 after a
        // space, so the reference reports a pipeline (the backward check only
        // skips the second pipe of an adjacent ||).
        expect(!has_top_level_pipe("a || b"));
        expect(has_top_level_pipe("a | | b"));
        // Escaped pipe is skipped.
        expect(!has_top_level_pipe("echo \\| x"));
        // Unclosed quote consumes the rest of the command.
        expect(!has_top_level_pipe("echo 'a | b"));
        expect(!has_top_level_pipe("echo \"a | b"));
        expect(!has_top_level_pipe("echo `a | b"));
        // Backslash inside double quotes escapes the quote char in the
        // reference scanner (byte-exact mirror).
        expect(!has_top_level_pipe("echo \"a\\\"| b\""));
        // Parenthesis depth: pipe inside ( ) is ignored.
        expect(!has_top_level_pipe("(ls | head)"));
        expect(has_top_level_pipe("(ls); echo x | head"));
        // Heredoc bodies are NOT protected by the reference scanner: the pipe
        // inside the heredoc body is at depth 0 with no open quote.
        expect(has_top_level_pipe("cat <<EOF\nx | y\nEOF"));
        // Nested parens.
        expect(!has_top_level_pipe("foo(bar(baz | qux))"));
        expect(has_top_level_pipe("foo(bar | baz); ls | head"));
        // A single '&' background operator does not create a pipeline.
        expect(!has_top_level_pipe("sleep 5 & echo done"));
    };

    // =======================================================================
    // 3.1 base_command_name
    // =======================================================================
    "base_command_name"_test = [] {
        expect(eq(base_command_name("grep foo file"), kimix::string("grep")));
        expect(eq(base_command_name("/usr/bin/grep -r foo"), kimix::string("grep")));
        expect(eq(base_command_name("FOO=1 git diff"), kimix::string("git")));
        expect(eq(base_command_name("python -m http.server"), kimix::string("python")));
        expect(eq(base_command_name("Grep.exe x"), kimix::string("Grep")));
        expect(eq(base_command_name("git.exe status"), kimix::string("git")));
        expect(eq(base_command_name("sudo git diff"), kimix::string("sudo")));
        expect(eq(base_command_name("echo a && grep b"), kimix::string("grep")));
        expect(eq(base_command_name("echo a; grep b"), kimix::string("grep")));
        expect(eq(base_command_name("echo a | grep b"), kimix::string("grep")));
        expect(eq(base_command_name("echo a || grep b"), kimix::string("grep")));
        expect(eq(base_command_name("  "), kimix::string()));
        expect(eq(base_command_name(""), kimix::string()));
        expect(eq(base_command_name("-x=1 cmd"), kimix::string("-x=1")));
        // -x=1 starts with '-' so it is not skipped as an assignment.
        expect(eq(base_command_name("A=1 -x=1 cmd"), kimix::string("-x=1")));
    };

    // =======================================================================
    // 3.1 interpret_exit_code + is_expected_exit — golden vectors
    // =======================================================================
    "interpret_exit_code_golden"_test = [] {
        const exit_golden cases[] = {
            {"grep foo file", true, 1, "No matches found (not an error)", true},
            {"egrep x", true, 1, "No matches found (not an error)", true},
            {"fgrep x", true, 1, "No matches found (not an error)", true},
            {"rg x", true, 1, "No matches found (not an error)", true},
            {"ag x", true, 1, "No matches found (not an error)", true},
            {"ack x", true, 1, "No matches found (not an error)", true},
            {"grep foo file", true, 0, nullptr, false},
            {"grep foo file", true, 2, nullptr, false},
            {"diff a b", true, 1, "Files differ (expected, not an error)", true},
            {"colordiff a b", true, 1, "Files differ (expected, not an error)", true},
            {"diff a b", true, 2, nullptr, false},
            {"find . -name x", true, 1,
             "Some directories were inaccessible (partial results may still be valid)",
             true},
            {"find . -name x", true, 0, nullptr, false},
            {"test -f x", true, 1,
             "Condition evaluated to false (expected, not an error)", true},
            {"[ -f x ]", true, 1,
             "Condition evaluated to false (expected, not an error)", true},
            {"test -f x", true, 0, nullptr, false},
            {"curl http://x", true, 6, "Could not resolve host (DNS failure)", false},
            {"curl http://x", true, 7, "Failed to connect to host", false},
            {"curl http://x", true, 22, "HTTP error (server returned an error status)", false},
            {"curl http://x", true, 28, "Connection timed out", false},
            {"curl http://x", true, 0, nullptr, false},
            {"curl http://x", true, 56, nullptr, false},
            {"git diff", true, 1,
             "Non-zero exit (often normal \xE2\x80\x94 e.g. 'git diff' returns 1 when files differ)",
             false},
            {"git status", true, 1,
             "Non-zero exit (often normal \xE2\x80\x94 e.g. 'git diff' returns 1 when files differ)",
             false},
            {"git diff", true, 0, nullptr, false},
            {"producer | head", true, 141,
             "SIGPIPE: an upstream pipeline stage was truncated (expected when piping to head/tail)",
             true},
            {"producer | head", true, 142, nullptr, false},
            {"echo hi", true, 141, nullptr, false},
            {"unknown", true, 3, nullptr, false},
            {"ls", true, 0, nullptr, false},
            {"ls", false, 0, nullptr, false},
            {"", true, 1, nullptr, false},
            {"  ", true, 1, nullptr, false},
            {"FOO=1 git diff", true, 1,
             "Non-zero exit (often normal \xE2\x80\x94 e.g. 'git diff' returns 1 when files differ)",
             false},
            {"/usr/bin/grep -r foo", true, 1, "No matches found (not an error)", true},
            {"python -m http.server", true, 1, nullptr, false},
            {"Grep.exe x", true, 1, "No matches found (not an error)", true},
            {"sudo git diff", true, 1, nullptr, false},
            {"echo a && grep b", true, 1, "No matches found (not an error)", true},
            {"echo a; grep b", true, 1, "No matches found (not an error)", true},
        };
        for (const auto &g : cases) {
            const auto msg = interpret_exit_code(g.command, exit_code(g));
            if (g.message == nullptr) {
                expect(!msg.has_value()) << "cmd=" << g.command << " code=" << g.code;
            } else {
                expect(msg.has_value()) << "cmd=" << g.command << " code=" << g.code;
                if (msg.has_value()) {
                    expect(eq(*msg, kimix::string(g.message)))
                        << "cmd=" << g.command << " code=" << g.code;
                }
            }
            expect(eq(is_expected_exit(g.command, exit_code(g)), g.expected))
                << "cmd=" << g.command << " code=" << g.code;
        }
    };

    "interpret_exit_code_sigpipe_first"_test = [] {
        // SIGPIPE wins over the base-command table even for known commands.
        const auto r = interpret_exit_code("grep foo | head", 141);
        expect(r.has_value());
        expect(eq(*r,
                  kimix::string("SIGPIPE: an upstream pipeline stage was truncated "
                                "(expected when piping to head/tail)")));
        expect(is_expected_exit("grep foo | head", 141));
        // 141 without a top-level pipe falls through to the ordinary table.
        const auto r2 = interpret_exit_code("grep foo", 141);
        expect(!r2.has_value());
        expect(!is_expected_exit("grep foo", 141));
    };

    "interpret_exit_code_git_em_dash_bytes"_test = [] {
        // The git message contains U+2014 (EM DASH), UTF-8 bytes E2 80 94 —
        // verify the exact byte sequence so the message stays byte-identical
        // to output_enhance.py.
        const auto r = interpret_exit_code("git diff", 1);
        expect(r.has_value());
        if (r.has_value()) {
            const kimix::string want(
                "Non-zero exit (often normal \xE2\x80\x94 e.g. 'git diff' returns 1 when files differ)");
            expect(eq(*r, want));
            expect((*r).find("\xE2\x80\x94") != kimix::string::npos);
        }
    };

    // =======================================================================
    // 3.3 find_error_line_index — golden vectors from common.py
    // =======================================================================
    "find_error_line_index_golden"_test = [] {
        struct {
            const char *output;
            int64_t want; // -1 == None
        } cases[] = {
            {"ok\nthis failed\nnext", 2},
            {"clean output\nall good", -1},
            {"Traceback (most recent call last):\n  File \"x.py\"", 1},
            {"Permission denied", 1},
            {"line with timeout", 1},
            {"ERROR: something", 1},
            {"not an error line", 1},
            {"myerror is here", -1},
            {"a\nb\nc\nassertion failed at d", 4},
            {"", -1},
            {"  error  ", 1},
            {"syntaxerror at line 3", 1},
            {"ModuleNotFoundError: no module named foo", 1},
        };
        for (const auto &c : cases) {
            const auto r = find_error_line_index(c.output);
            if (c.want < 0) {
                expect(!r.has_value()) << "output=" << c.output;
            } else {
                expect(r.has_value()) << "output=" << c.output;
                if (r.has_value()) {
                    expect(eq(*r, c.want)) << "output=" << c.output;
                }
            }
        }
    };

    "error_keywords_table"_test = [] {
        // The public table must have the reference count and each keyword
        // must be detected on its own line with \b boundaries and case
        // folding.
        expect(eq(error_keyword_count, size_t(36)));
        static const char *reference_order[] = {
            "error", "exception", "traceback", "failed", "failure",
            "fatal", "panic", "abort", "assertion", "undefined",
            "syntaxerror", "typeerror", "valueerror", "keyerror",
            "importerror", "modulenotfounderror", "attributeerror",
            "nameerror", "runtimeerror", "oserror", "ioerror",
            "zerodivisionerror", "indexerror", "memoryerror",
            "recursionerror", "unboundlocalerror", "referenceerror",
            "permission denied", "access denied", "not found",
            "cannot find", "does not exist", "no such file",
            "connection refused", "timeout", "unhandled",
        };
        for (size_t i = 0; i < error_keyword_count; ++i) {
            expect(eq(error_keywords[i], kimix::string_view(reference_order[i])));
            const kimix::string line(error_keywords[i]);
            expect(eq(*find_error_line_index(line), int64_t(1)))
                << "keyword=" << line;
        }
        // Case-insensitive ASCII folding.
        expect(eq(*find_error_line_index("THIS Is A Failure"), int64_t(1)));
        expect(eq(*find_error_line_index("PANIC at the disco"), int64_t(1)));
        expect(eq(*find_error_line_index("syntaxerror at line 3"), int64_t(1)));
        // \b boundaries: the keyword must not match inside a longer word.
        expect(!find_error_line_index("myerror").has_value());
        expect(!find_error_line_index("errorr").has_value());
        expect(!find_error_line_index("timeoutx").has_value());
        expect(!find_error_line_index("unhandledy").has_value());
        expect(!find_error_line_index("permission_denied").has_value());
        // "importerror" IS a table keyword (matches on its own word boundary).
        expect(eq(*find_error_line_index("importerror is fine here"), int64_t(1)));
        // Boundary at line start / line end.
        expect(eq(*find_error_line_index("error"), int64_t(1)));
        expect(eq(*find_error_line_index("x\nerror"), int64_t(2)));
        expect(eq(*find_error_line_index("x\npanic"), int64_t(2)));
        // Second line wins over a non-matching first line.
        expect(eq(*find_error_line_index("clean\nconnection refused"), int64_t(2)));
        // CRLF/CR line endings.
        expect(eq(*find_error_line_index("ok\r\nfailed\r\nx"), int64_t(2)));
        expect(eq(*find_error_line_index("ok\rfailed\rx"), int64_t(2)));
    };

    // =======================================================================
    // 3.3 truncate_lines — golden vectors from common.py
    // =======================================================================
    "truncate_lines_golden"_test = [] {
        struct {
            const char *output;
            int64_t max_lines;
            bool preserve_errors;
            int64_t ctx;
            const char *want;
        } cases[] = {
            {"l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8", 4, true, 2,
             "l1\nl2\n\n[... 5 lines omitted ...]\n\nl8"},
            {"l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8", 4, false, 2,
             "l1\nl2\n\n[... 5 lines omitted ...]\n\nl8"},
            {"a\nb\nc", 5, true, 2, "a\nb\nc"},
            {"", 4, true, 2, ""},
            {"a\nb\nc\nd", 4, true, 2, "a\nb\nc\nd"},
            {"x1\nx2\nx3\nx4\nx5\nx6\nx7\nx8\nx9\nx10", 5, true, 2,
             "x1\nx2\n\n[... 6 lines omitted ...]\n\nx9\nx10"},
            {"ok\nok\nfailed\ntail1\ntail2", 3, true, 2,
             "ok\nok\nfailed\ntail1\n\n[... 3 lines omitted (3 error-context line(s) preserved) ...]\n\ntail2"},
            {"ok\nok\nfailed\ntail1\ntail2", 3, true, 0,
             "ok\nfailed\n\n[... 3 lines omitted (1 error-context line(s) preserved) ...]\n\ntail2"},
            {"ok\nok\nfailed\ntail1\ntail2", 3, true, 1,
             "ok\nok\nfailed\ntail1\n\n[... 3 lines omitted (3 error-context line(s) preserved) ...]\n\ntail2"},
            {"ok\nok\nfailed\ntail1\ntail2", 3, false, 2,
             "ok\n\n[... 3 lines omitted ...]\n\ntail2"},
            {"l1\r\nl2\r\nl3\r\nl4\r\nl5\r\nl6", 3, false, 2,
             "l1\n\n[... 4 lines omitted ...]\n\nl6"},
            {"single", 0, true, 2, "single"},
            {"l1\nl2\nl3\nl4\nl5", 2, true, 2,
             "l1\n\n[... 4 lines omitted ...]\n\n"},
            {"l1\nl2\nl3\nl4\nl5\nl6", 3, true, 2,
             "l1\n\n[... 4 lines omitted ...]\n\nl6"},
        };
        for (const auto &c : cases) {
            const kimix::string got =
                truncate_lines(c.output, c.max_lines, c.preserve_errors, c.ctx);
            expect(eq(got, kimix::string(c.want)))
                << "output=" << c.output << " max=" << c.max_lines
                << " preserve=" << c.preserve_errors << " ctx=" << c.ctx;
        }
    };

    "truncate_lines_behaviour"_test = [] {
        // n <= max_lines -> unchanged (byte-identical, incl. CRLF).
        const kimix::string in = "a\r\nb\r\nc";
        expect(eq(truncate_lines(in, 3, true, 2), in));
        expect(eq(truncate_lines("x", 1, true, 2), kimix::string("x")));
        // max_lines <= 0 -> unchanged.
        expect(eq(truncate_lines("x\ny", 0, true, 2), kimix::string("x\ny")));
        expect(eq(truncate_lines("x\ny", -3, true, 2), kimix::string("x\ny")));
        // max_lines == 1: head_n = 0, tail_n = 0.
        expect(eq(truncate_lines("a\nb\nc", 1, true, 2),
                  kimix::string("\n\n[... 3 lines omitted ...]\n\n")));
        // Error inside the omitted region keeps context; error in the head or
        // tail region is not preserved (already visible).
        expect(eq(truncate_lines("err\na\nb\nc\nd\ne\nf", 3, true, 2),
                  kimix::string("err\n\n[... 5 lines omitted ...]\n\nf")));
        // Error exactly at omitted_lo (1-based head_n+1).
        // n=5, max=3: head_n=1, omitted_lo=1(0-based e=1), error at line 2.
        expect(eq(truncate_lines("ok\nfailed\ntail1\ntail2\ntail3", 3, true, 0),
                  kimix::string("ok\nfailed\n\n[... 3 lines omitted (1 error-context line(s) preserved) ...]\n\ntail3")));
        // CR-only input splits lines and rejoins with \n.
        expect(eq(truncate_lines("l1\rl2\rl3\rl4\rl5\rl6", 3, false, 2),
                  kimix::string("l1\n\n[... 4 lines omitted ...]\n\nl6")));
        // Trailing newline does not add an empty line (splitlines semantics).
        expect(eq(truncate_lines("l1\nl2\nl3\nl4\nl5\n", 3, false, 2),
                  kimix::string("l1\n\n[... 3 lines omitted ...]\n\nl5")));
        // Default parameters: preserve_errors=true, context=2.
        expect(eq(truncate_lines("ok\nok\nfailed\ntail1\ntail2", 3),
                  kimix::string("ok\nok\nfailed\ntail1\n\n[... 3 lines omitted (3 error-context line(s) preserved) ...]\n\ntail2")));
    };

    // =======================================================================
    // 3.4 RTK rewrite kernels — golden vectors from common.py
    // =======================================================================
    "split_shell_segments"_test = [] {
        kimix::vector<shell_segment> segs;
        split_shell_segments("git status", segs);
        expect(eq(segs.size(), size_t(1)));
        expect(eq(segs[0].text, kimix::string("git status")));
        expect(eq(segs[0].sep, kimix::string()));

        split_shell_segments("a; b && c || d; e", segs);
        expect(eq(segs.size(), size_t(5)));
        expect(eq(segs[0].text, kimix::string("a")));
        expect(eq(segs[0].sep, kimix::string(";")));
        expect(eq(segs[1].text, kimix::string(" b ")));
        expect(eq(segs[1].sep, kimix::string("&&")));
        expect(eq(segs[2].text, kimix::string(" c ")));
        expect(eq(segs[2].sep, kimix::string("||")));
        expect(eq(segs[3].text, kimix::string(" d")));
        expect(eq(segs[3].sep, kimix::string(";")));
        expect(eq(segs[4].text, kimix::string(" e")));
        expect(eq(segs[4].sep, kimix::string()));

        // Separators inside quotes / substitutions stay in the segment.
        split_shell_segments("echo 'a;b' | cat", segs);
        expect(eq(segs.size(), size_t(1)));
        expect(eq(segs[0].text, kimix::string("echo 'a;b' | cat")));

        split_shell_segments("echo \"a&&b\" && echo c", segs);
        expect(eq(segs.size(), size_t(2)));
        expect(eq(segs[0].text, kimix::string("echo \"a&&b\" ")));
        expect(eq(segs[0].sep, kimix::string("&&")));
        expect(eq(segs[1].text, kimix::string(" echo c")));

        split_shell_segments("echo $(x; y) ; echo z", segs);
        expect(eq(segs.size(), size_t(2)));
        expect(eq(segs[0].text, kimix::string("echo $(x; y) ")));
        expect(eq(segs[0].sep, kimix::string(";")));
        expect(eq(segs[1].text, kimix::string(" echo z")));

        split_shell_segments("echo `a | b`; ls", segs);
        expect(eq(segs.size(), size_t(2)));
        expect(eq(segs[0].text, kimix::string("echo `a | b`")));
        expect(eq(segs[0].sep, kimix::string(";")));

        // Single | and & stay inside the segment.
        split_shell_segments("a | b & c", segs);
        expect(eq(segs.size(), size_t(1)));
        expect(eq(segs[0].text, kimix::string("a | b & c")));

        // Unterminated quotes consume the rest.
        split_shell_segments("echo 'unterminated && ls", segs);
        expect(eq(segs.size(), size_t(1)));
        expect(eq(segs[0].text, kimix::string("echo 'unterminated && ls")));

        // Empty command -> one empty segment.
        split_shell_segments("", segs);
        expect(eq(segs.size(), size_t(1)));
        expect(eq(segs[0].text, kimix::string()));
    };

    "is_known_rtk_command"_test = [] {
        expect(is_known_rtk_command("git"));
        expect(is_known_rtk_command("GIT"));
        expect(is_known_rtk_command("git.exe"));
        expect(is_known_rtk_command("GIT.EXE"));
        expect(is_known_rtk_command("ls"));
        expect(is_known_rtk_command("rg"));
        expect(is_known_rtk_command("npm"));
        expect(is_known_rtk_command("gradlew"));
        expect(is_known_rtk_command("mvn"));
        expect(!is_known_rtk_command("find")); // intentionally excluded
        expect(!is_known_rtk_command("echo"));
        expect(!is_known_rtk_command("cat"));
        expect(!is_known_rtk_command("rtk"));
        expect(!is_known_rtk_command(""));
    };

    "rewrite_shell_segment"_test = [] {
        const auto r1 = rewrite_shell_segment("git status", false, false);
        expect(eq(r1.segment, kimix::string("rtk git status")));
        expect(r1.changed);

        const auto r2 = rewrite_shell_segment("git status", false, true);
        expect(eq(r2.segment, kimix::string("& rtk git status")));
        expect(r2.changed);

        // Unknown command unchanged.
        const auto r3 = rewrite_shell_segment("echo hello", false, false);
        expect(eq(r3.segment, kimix::string("echo hello")));
        expect(!r3.changed);

        // RTK_DISABLED=1 short-circuits.
        const auto r4 = rewrite_shell_segment("RTK_DISABLED=1 git status", false, false);
        expect(eq(r4.segment, kimix::string("RTK_DISABLED=1 git status")));
        expect(!r4.changed);

        // Assignments and prefix modifiers are skipped.
        const auto r5 = rewrite_shell_segment("FOO=1 git status", false, false);
        expect(eq(r5.segment, kimix::string("FOO=1 rtk git status")));
        expect(r5.changed);

        const auto r6 = rewrite_shell_segment("sudo git status", false, false);
        expect(eq(r6.segment, kimix::string("sudo rtk git status")));
        expect(r6.changed);

        // Already-rtk executables are left alone (stem match).
        const auto r7 = rewrite_shell_segment("rtk git status", false, false);
        expect(eq(r7.segment, kimix::string("rtk git status")));
        expect(!r7.changed);
        const auto r8 = rewrite_shell_segment("rtk.exe status", false, false);
        expect(eq(r8.segment, kimix::string("rtk.exe status")));
        expect(!r8.changed);

        // Quoted token: stem strips quotes so it still matches.
        const auto r9 = rewrite_shell_segment("\"git\" status", false, false);
        expect(eq(r9.segment, kimix::string("rtk \"git\" status")));
        expect(r9.changed);

        // Path token.
        const auto r10 = rewrite_shell_segment("/usr/bin/git status", false, false);
        expect(eq(r10.segment, kimix::string("rtk /usr/bin/git status")));
        expect(r10.changed);

        // exclude_read.
        const auto r11 = rewrite_shell_segment("read x", true, false);
        expect(eq(r11.segment, kimix::string("read x")));
        expect(!r11.changed);
        const auto r12 = rewrite_shell_segment("read x", false, false);
        expect(eq(r12.segment, kimix::string("rtk read x")));
        expect(r12.changed);

        // Leftmost pipeline command is the only one rewritten (single | stays
        // in the segment).
        const auto r13 = rewrite_shell_segment("echo a | grep b", false, false);
        expect(eq(r13.segment, kimix::string("echo a | grep b")));
        expect(!r13.changed);
        const auto r14 = rewrite_shell_segment("git status | head", false, false);
        expect(eq(r14.segment, kimix::string("rtk git status | head")));
        expect(r14.changed);
    };

    "maybe_rewrite_shell_command_with_rtk_golden"_test = [] {
        const rtk_golden cases[] = {
            {"git status", true, false, false, "rtk git status", true},
            {"git status --short", true, true, false, "rtk git status --short", true},
            {"echo hello", true, false, false, "echo hello", false},
            {"rtk git status", true, false, false, "rtk git status", false},
            {"RTK_DISABLED=1 git status", true, false, false,
             "RTK_DISABLED=1 git status", false},
            {"FOO=1 git status", true, false, false, "FOO=1 rtk git status", true},
            {"sudo git status", true, false, false, "sudo rtk git status", true},
            {"time git status", true, false, false, "time rtk git status", true},
            {"git status; echo x", true, false, false, "git status; echo x", false},
            {"git status && echo x", true, false, false, "git status && echo x", false},
            {"echo \"git status\"", true, false, false, "echo \"git status\"", false},
            {"echo $(git status)", true, false, false, "echo $(git status)", false},
            {"/usr/bin/git status", true, false, false, "rtk /usr/bin/git status", true},
            {"\"git\" status", true, false, false, "rtk \"git\" status", true},
            {"grep foo", true, false, false, "rtk grep foo", true},
            {"read x", true, true, false, "read x", false},
            {"read x", true, false, false, "rtk read x", true},
            {"ls -la", true, false, false, "rtk ls -la", true},
            {"git status", true, false, true, "& rtk git status", true},
            {"   git status", true, false, false, "   rtk git status", true},
            {"", true, false, false, "", false},
            {"   ", true, false, false, "   ", false},
            {"npm run build", true, false, false, "rtk npm run build", true},
            {"git.exe status", true, false, false, "rtk git.exe status", true},
            {"rtk.exe status", true, false, false, "rtk.exe status", false},
            {"& rtk git status", true, false, false, "& rtk git status", false},
            {"echo a | grep b", true, false, false, "echo a | grep b", false},
            {"git status | head", true, false, false, "rtk git status | head", true},
        };
        for (const auto &g : cases) {
            const rewrite_result r = maybe_rewrite_shell_command_with_rtk(
                g.command, g.token_kill, true, "", g.exclude_read, g.pwsh);
            expect(eq(r.segment, kimix::string(g.rewritten)))
                << "cmd=" << g.command;
            expect(eq(r.changed, g.changed)) << "cmd=" << g.command;
        }
    };

    "maybe_rewrite_shell_command_with_rtk_gates"_test = [] {
        // token_kill=false short-circuits.
        const auto r1 = maybe_rewrite_shell_command_with_rtk("git status", false, true, "");
        expect(eq(r1.segment, kimix::string("git status")));
        expect(!r1.changed);

        // rtk_available=false short-circuits.
        const auto r2 = maybe_rewrite_shell_command_with_rtk("git status", true, false, "");
        expect(eq(r2.segment, kimix::string("git status")));
        expect(!r2.changed);

        // Whitespace-only and empty commands are untouched.
        const auto r3 = maybe_rewrite_shell_command_with_rtk("", true, true, "");
        expect(eq(r3.segment, kimix::string()));
        expect(!r3.changed);
        const auto r4 = maybe_rewrite_shell_command_with_rtk(" \t\n", true, true, "");
        expect(eq(r4.segment, kimix::string(" \t\n")));
        expect(!r4.changed);

        // Absolute rtk-binary path fast path (unquoted and quoted, with and
        // without the pwsh `&` call operator).
        const kimix::string_view path = "C:/share/bin/rtk.exe";
        const auto r5 = maybe_rewrite_shell_command_with_rtk("C:/share/bin/rtk.exe status",
                                                             true, true, path);
        expect(eq(r5.segment, kimix::string("C:/share/bin/rtk.exe status")));
        expect(!r5.changed);
        const auto r6 = maybe_rewrite_shell_command_with_rtk("\"C:/share/bin/rtk.exe\" status",
                                                             true, true, path);
        expect(eq(r6.segment, kimix::string("\"C:/share/bin/rtk.exe\" status")));
        expect(!r6.changed);
        const auto r7 = maybe_rewrite_shell_command_with_rtk("& C:/share/bin/rtk.exe status",
                                                             true, true, path);
        expect(eq(r7.segment, kimix::string("& C:/share/bin/rtk.exe status")));
        expect(!r7.changed);
        // A different rtk path still rewrites (and a different binary path
        // whose stem is `rtk` is treated as rtk itself).
        const auto r8 = maybe_rewrite_shell_command_with_rtk("C:/other/git status", true, true, path);
        expect(eq(r8.segment, kimix::string("rtk C:/other/git status")));
        expect(r8.changed);
        const auto r8b = maybe_rewrite_shell_command_with_rtk("C:/other/rtk status", true, true, path);
        expect(eq(r8b.segment, kimix::string("C:/other/rtk status")));
        expect(!r8b.changed);

        // Multi-segment commands are never rewritten.
        const auto r9 = maybe_rewrite_shell_command_with_rtk("git status; echo x", true, true, "");
        expect(eq(r9.segment, kimix::string("git status; echo x")));
        expect(!r9.changed);

        // `& rtk` prefix fast path.
        const auto r10 = maybe_rewrite_shell_command_with_rtk("& rtk git status", true, true, "");
        expect(eq(r10.segment, kimix::string("& rtk git status")));
        expect(!r10.changed);
    };

    // -----------------------------------------------------------------------
    // RTK rewrite scanner: generated byte-exact vectors from the reference
    // (kimi-agent src/kimix/tools/common.py).  One row per (command, profile):
    // profiles are plain / exclude_read / pwsh / absolute-rtk-path.
    // -----------------------------------------------------------------------
    "rtk_rewrite_goldens"_test = [] {
        size_t rows = 0;
        for (const auto &g : k_bash_rtk_rewrite_goldens) {
            const rewrite_result r = maybe_rewrite_shell_command_with_rtk(
                g.command, g.token_kill, /*rtk_available=*/true, g.rtk_binary_path,
                g.exclude_read, g.pwsh);
            expect(eq(r.segment, kimix::string(g.rewritten))) << "cmd=" << g.command;
            expect(eq(r.changed, g.changed)) << "cmd=" << g.command;
            ++rows;
        }
        expect(rows > 3000_u); // guard against a truncated golden file
    };

    "rtk_rewrite_segment_goldens"_test = [] {
        size_t rows = 0;
        for (const auto &g : k_bash_rtk_segment_goldens) {
            const rewrite_result r =
                rewrite_shell_segment(g.segment, g.exclude_read, g.pwsh);
            expect(eq(r.segment, kimix::string(g.rewritten))) << "seg=" << g.segment;
            expect(eq(r.changed, g.changed)) << "seg=" << g.segment;
            ++rows;
        }
        expect(rows > 2000_u);
    };

    "rtk_split_goldens"_test = [] {
        kimix::vector<shell_segment> segs;
        size_t rows = 0;
        for (const auto &g : k_bash_rtk_split_goldens) {
            split_shell_segments(g.command, segs);
            const auto want = rtk_decode_segments(g.segments);
            expect(eq(segs.size(), want.size())) << "cmd=" << g.command;
            if (segs.size() == want.size()) {
                for (size_t i = 0; i < segs.size(); ++i) {
                    expect(eq(segs[i].text, want[i].text)) << "cmd=" << g.command;
                    expect(eq(segs[i].sep, want[i].sep)) << "cmd=" << g.command;
                }
            }
            ++rows;
        }
        expect(rows > 900_u);
    };

    "rtk_known_command_goldens"_test = [] {
        size_t rows = 0;
        for (const auto &g : k_bash_rtk_known_goldens) {
            expect(eq(is_known_rtk_command(g.name), g.known)) << "name=" << g.name;
            ++rows;
        }
        expect(rows > 80_u);
    };

    // =======================================================================
    // Bounded-run capture/timeout/kill policy state machine (pure, no spawn)
    // =======================================================================
    "bounded_append_capture"_test = [] {
        bool trunc = false;
        // Under cap: unchanged, no truncation flag.
        kimix::string out = bounded_append_capture("", "hello", 100, trunc);
        expect(eq(out, kimix::string("hello")));
        expect(!trunc);

        trunc = false;
        out = bounded_append_capture("hello", " world", 100, trunc);
        expect(eq(out, kimix::string("hello world")));
        expect(!trunc);

        // Exactly at cap: unchanged.
        trunc = false;
        out = bounded_append_capture("abcdef", "", 6, trunc);
        expect(eq(out, kimix::string("abcdef")));
        expect(!trunc);

        // Over cap: head 40% + marker + tail 60%, truncation flag set.
        trunc = false;
        kimix::string big;
        for (int i = 0; i < 30; ++i) {
            big.push_back('a' + (i % 26));
        }
        out = bounded_append_capture("", big, 10, trunc);
        // cap=10 -> head_len=4, tail_len=6; the marker is appended ON TOP of
        // head+tail (Python bounded_append does not budget the marker).
        expect(trunc);
        expect(eq(out,
                  kimix::string("abcd\n[... (output truncated, keeping first 4 and last 6 chars)]\nyzabcd")));

        // Appending to existing content over the cap.
        trunc = false;
        out = bounded_append_capture("abcd", "efghijklmnopqrstuvwxyz", 10, trunc);
        expect(trunc);
        expect(eq(out,
                  kimix::string("abcd\n[... (output truncated, keeping first 4 and last 6 chars)]\nuvwxyz")));

        // Character-based: a multi-byte (UTF-8) chunk counts by code point and
        // never splits a sequence.
        trunc = false;
        kimix::string u8;
        for (int i = 0; i < 8; ++i) {
            u8 += "\xE2\x86\x92"; // U+2192, 3 bytes
        }
        out = bounded_append_capture("", u8, 6, trunc);
        expect(trunc);
        // Head 2 + marker + tail 4 code points; byte-exact, never splits a
        // multi-byte sequence.
        expect(eq(out,
                  kimix::string("\xE2\x86\x92\xE2\x86\x92\n[... (output truncated, keeping first 2 and last 4 chars)]\n\xE2\x86\x92\xE2\x86\x92\xE2\x86\x92\xE2\x86\x92")));
        expect(kimix::builtin_tools::utf8_validate(out));

        // cap == 0 with empty text: unchanged.
        trunc = false;
        out = bounded_append_capture("", "", 0, trunc);
        expect(eq(out, kimix::string()));
        expect(!trunc);
    };

    "capture_machine_pattern_stop"_test = [] {
        capture_config cfg;
        cfg.timeout_ms = 10000;
        cfg.wait_pattern = "READY";
        capture_machine m(cfg);

        auto d = m.on_event(capture_event{capture_event::kind::chunk, "start...", std::nullopt, 10});
        expect(d.act == capture_decision::action::wait);
        expect(!m.finished());

        d = m.on_event(capture_event{capture_event::kind::chunk, "\nREADY\n", std::nullopt, 25});
        expect(d.act == capture_decision::action::pattern_stop);
        expect(d.matched);
        expect(m.finished());
        expect(m.matched());
        expect(eq(m.output(), kimix::string("start...\nREADY\n")));

        // Finished machine replays the stop decision and ignores later chunks.
        d = m.on_event(capture_event{capture_event::kind::chunk, "more", std::nullopt, 40});
        expect(d.act == capture_decision::action::pattern_stop);
        expect(eq(m.output(), kimix::string("start...\nREADY\n")));

        // A late process-exit event is recorded and still replays the stop.
        d = m.on_event(capture_event{capture_event::kind::process_exited, "", 0, 50});
        expect(d.act == capture_decision::action::pattern_stop);
        expect(m.exit_code().has_value());
        expect(eq(*m.exit_code(), int64_t(0)));
    };

    "capture_machine_timeout_kill"_test = [] {
        capture_config cfg;
        cfg.timeout_ms = 5000;
        capture_machine m(cfg);

        auto d = m.on_event(capture_event{capture_event::kind::chunk, "work", std::nullopt, 1000});
        expect(d.act == capture_decision::action::wait);

        d = m.on_event(capture_event{capture_event::kind::chunk, "still going", std::nullopt, 4999});
        expect(d.act == capture_decision::action::wait);

        d = m.on_event(capture_event{capture_event::kind::chunk, "x", std::nullopt, 5000});
        expect(d.act == capture_decision::action::timeout_kill);
        expect(!d.matched);
        expect(m.finished());

        // Timeout == 0 fires immediately (elapsed >= timeout).
        capture_config cfg0;
        cfg0.timeout_ms = 0;
        capture_machine m0(cfg0);
        d = m0.on_event(capture_event{capture_event::kind::chunk, "hi", std::nullopt, 0});
        expect(d.act == capture_decision::action::timeout_kill);
        expect(m0.finished());
    };

    "capture_machine_complete_stop"_test = [] {
        capture_config cfg;
        cfg.timeout_ms = 10000;
        capture_machine m(cfg);

        auto d = m.on_event(capture_event{capture_event::kind::chunk, "out", std::nullopt, 100});
        expect(d.act == capture_decision::action::wait);

        d = m.on_event(capture_event{capture_event::kind::process_exited, "", 3, 200});
        expect(d.act == capture_decision::action::complete_stop);
        expect(m.finished());
        expect(m.exit_code().has_value());
        expect(eq(*m.exit_code(), int64_t(3)));

        // Process exit wins over a same-event timeout: the caller's
        // thread_is_alive check takes the completion path.
        capture_config cfg2;
        cfg2.timeout_ms = 100;
        capture_machine m2(cfg2);
        d = m2.on_event(capture_event{capture_event::kind::process_exited, "", 1, 500});
        expect(d.act == capture_decision::action::complete_stop);
        expect(m2.finished());
    };

    "capture_machine_inactivity_stop"_test = [] {
        capture_config cfg;
        cfg.timeout_ms = 60000;
        cfg.inactivity_timeout_ms = 3000;
        capture_machine m(cfg);

        // Chunk at t=0 arms the activity timer.
        auto d = m.on_event(capture_event{capture_event::kind::chunk, "a", std::nullopt, 0});
        expect(d.act == capture_decision::action::wait);

        // t=2999 still waiting.
        d = m.on_event(capture_event{capture_event::kind::chunk, "", std::nullopt, 2999});
        expect(d.act == capture_decision::action::wait);

        // t=3000 with no new output since t=0 -> inactivity stop.
        d = m.on_event(capture_event{capture_event::kind::chunk, "", std::nullopt, 3000});
        expect(d.act == capture_decision::action::inactivity_stop);
        expect(m.finished());

        // New output refreshes the timer.
        capture_machine m2(cfg);
        d = m2.on_event(capture_event{capture_event::kind::chunk, "a", std::nullopt, 0});
        expect(d.act == capture_decision::action::wait);
        d = m2.on_event(capture_event{capture_event::kind::chunk, "b", std::nullopt, 1000});
        expect(d.act == capture_decision::action::wait);
        d = m2.on_event(capture_event{capture_event::kind::chunk, "", std::nullopt, 3999});
        expect(d.act == capture_decision::action::wait);
        d = m2.on_event(capture_event{capture_event::kind::chunk, "", std::nullopt, 4000});
        expect(d.act == capture_decision::action::inactivity_stop);

        // Inactivity disabled (0) never stops for inactivity.
        capture_config cfg3;
        cfg3.timeout_ms = 200000;
        cfg3.inactivity_timeout_ms = 0;
        capture_machine m3(cfg3);
        d = m3.on_event(capture_event{capture_event::kind::chunk, "", std::nullopt, 100000});
        expect(d.act == capture_decision::action::wait);
    };

    "capture_machine_truncation_flag"_test = [] {
        capture_config cfg;
        cfg.timeout_ms = 60000;
        cfg.output_cap_chars = 8;
        capture_machine m(cfg);
        auto d = m.on_event(capture_event{capture_event::kind::chunk, "abcdefghij", std::nullopt, 0});
        // 10 chars > cap 8 -> truncated; head 3 + marker + tail 5.
        expect(d.truncated);
        expect(m.truncated());
        expect(m.output().starts_with("abc"));
        expect(m.output().ends_with("fghij"));
        expect(m.output().find("output truncated, keeping first 3 and last 5 chars") != kimix::string::npos);
        // Decision still wait (no pattern, no timeout).
        expect(d.act == capture_decision::action::wait);
    };

    "process_exited_banner"_test = [] {
        expect(eq(process_exited_banner(1, kimix::optional<int64_t>(3)),
                  kimix::string("\n[Process exited with code 1, error at line 3]")));
        expect(eq(process_exited_banner(127, std::nullopt),
                  kimix::string("\n[Process exited with code 127]")));
        expect(eq(process_exited_banner(0, std::nullopt),
                  kimix::string("\n[Process exited with code 0]")));
    };

    // =======================================================================
    // 3.4.1 Hardline safety floor — golden vectors from safety.py
    // =======================================================================
    "command_detection_variants"_test = [] {
        kimix::vector<kimix::string> v;
        command_detection_variants("rm -rf /", v);
        expect(eq(v.size(), size_t(1)));
        expect(eq(v[0], kimix::string("rm -rf /")));

        command_detection_variants("  r\\'m -rf /  ", v);
        expect(eq(v.size(), size_t(2)));
        expect(eq(v[0], kimix::string("r\\'m -rf /")));
        expect(eq(v[1], kimix::string("rm -rf /")));

        command_detection_variants("  \n\t  ", v);
        expect(v.empty());

        command_detection_variants("FOO BAR", v);
        expect(eq(v.size(), size_t(2)));
        expect(eq(v[0], kimix::string("FOO BAR")));
        expect(eq(v[1], kimix::string("foo bar")));
    };

    "detect_hardline_command_recursive_delete"_test = [] {
        auto r = detect_hardline_command("rm -rf /");
        expect(r.blocked);
        expect(r.description.has_value());

        r = detect_hardline_command("rm -rf /home /tmp");
        expect(!r.blocked);

        r = detect_hardline_command("rm -f /*");
        expect(r.blocked);

        r = detect_hardline_command("rm -rf ~");
        expect(r.blocked);

        r = detect_hardline_command("rm -rf $HOME");
        expect(r.blocked);

        r = detect_hardline_command("rm -rf ${HOME}");
        expect(r.blocked);

        r = detect_hardline_command("rm -rf /tmp/build");
        expect(!r.blocked);

        r = detect_hardline_command("del /f /s C:\\*");
        expect(r.blocked);

        r = detect_hardline_command("rmdir /s /q C:\\");
        expect(r.blocked);

        r = detect_hardline_command("rm -f file.txt");
        expect(!r.blocked);
    };

    "detect_hardline_command_other_patterns"_test = [] {
        auto r = detect_hardline_command("mkfs.ext4 /dev/sda1");
        expect(r.blocked);
        expect(r.description.has_value());

        r = detect_hardline_command("dd if=/dev/zero of=/dev/sda");
        expect(r.blocked);

        r = detect_hardline_command("shutdown -h now");
        expect(r.blocked);

        r = detect_hardline_command("reboot");
        expect(r.blocked);

        r = detect_hardline_command("poweroff");
        expect(r.blocked);

        r = detect_hardline_command("halt");
        expect(r.blocked);

        r = detect_hardline_command(":(){ :|:& };:");
        expect(r.blocked);

        r = detect_hardline_command("kill 1");
        expect(r.blocked);

        r = detect_hardline_command("kill $PPID");
        expect(r.blocked);

        r = detect_hardline_command("format C:");
        expect(r.blocked);

        r = detect_hardline_command("format D:/");
        expect(r.blocked);

        r = detect_hardline_command("ls -la");
        expect(!r.blocked);

        r = detect_hardline_command("git status");
        expect(!r.blocked);
    };

    "check_hardline_blocked_variants"_test = [] {
        // Quote/escape obfuscation must be defeated.
        auto r = check_hardline_blocked("r\\'m -rf /");
        expect(r.blocked);

        r = check_hardline_blocked("  rm   -rf   /  ");
        expect(r.blocked);

        // safe.
        r = check_hardline_blocked("echo hello");
        expect(!r.blocked);
    };

    // =======================================================================
    // 3.4.2 Foreground / background guidance
    // =======================================================================
    "foreground_background_guidance"_test = [] {
        const kimix::string hint(
            "Long-running command detected; use `job_output` to wait for it or to stop it.");

        auto r = foreground_background_guidance("npm run dev");
        expect(r.has_value());
        expect(eq(*r, hint));

        // NOTE: the reference patterns are
        // ``\b(?:npm|pnpm|yarn|bun)\s+run\s+(?:dev|start|serve|watch)\b`` --
        // ``run`` is mandatory, so ``yarn start`` / ``pnpm watch`` are ordinary
        // commands (this file used to assert the opposite, encoding the port's
        // "run is optional" bug; kimi-agent's safety.py has no such pattern).
        r = foreground_background_guidance("yarn run start");
        expect(r.has_value());

        r = foreground_background_guidance("pnpm run watch");
        expect(r.has_value());

        r = foreground_background_guidance("yarn start");
        expect(!r.has_value());

        r = foreground_background_guidance("pnpm watch");
        expect(!r.has_value());

        r = foreground_background_guidance("npm start");
        expect(!r.has_value());

        r = foreground_background_guidance("next dev");
        expect(r.has_value());

        r = foreground_background_guidance("vite");
        expect(r.has_value());

        r = foreground_background_guidance("nodemon");
        expect(r.has_value());

        r = foreground_background_guidance("uvicorn main:app");
        expect(r.has_value());

        r = foreground_background_guidance("gunicorn app:app");
        expect(r.has_value());

        r = foreground_background_guidance("python -m http.server");
        expect(r.has_value());

        r = foreground_background_guidance("docker compose up");
        expect(r.has_value());

        r = foreground_background_guidance("docker-compose up");
        expect(r.has_value());

        r = foreground_background_guidance("sleep 5 &");
        expect(r.has_value());

        r = foreground_background_guidance("nohup python server.py");
        expect(r.has_value());

        r = foreground_background_guidance("setsid python server.py");
        expect(r.has_value());

        // Quoted spans are ignored.
        r = foreground_background_guidance("echo 'npm run dev'");
        expect(!r.has_value());

        r = foreground_background_guidance("ls -la");
        expect(!r.has_value());

        // The patterns carry real \b boundaries, so a keyword inside a longer
        // token matches (``vite.dev``, a path to the binary) while a longer
        // token that merely *contains* it does not.
        expect(foreground_background_guidance("vite.dev").has_value());
        expect(foreground_background_guidance("./node_modules/.bin/vite").has_value());
        expect(foreground_background_guidance("(vite)").has_value());
        expect(foreground_background_guidance("vite;").has_value());
        expect(!foreground_background_guidance("xvite").has_value());
        expect(!foreground_background_guidance("vitex").has_value());
        expect(!foreground_background_guidance("VITE").has_value());
        expect(foreground_background_guidance("&").has_value());
    };

    // -----------------------------------------------------------------------
    // Hardline rule regressions: cases where a token-based command-word scan or
    // an approximated pattern disagreed with safety.py.  Expectations are the
    // reference's own outputs (verified against kimi-agent).
    // -----------------------------------------------------------------------
    "check_hardline_blocked_command_words"_test = [] {
        // The command word is found after ANY non-word character, not only
        // after whitespace: `\b(rm|rmdir|del)(?:\.exe)?\b` over the text.
        const char *blocked[] = {
            "./rm -rf /",
            "/bin/rm -rf /",
            "/usr/bin/rm -rf /",
            "'/bin/rm' -rf /",
            "sudo/bin/rm -rf /",
            "c:\\windows\\system32\\rm.exe -rf /",
            "sh -c 'rm -rf /'",
            "sh -c \"rm -rf /\"",
            "./format C:",
            "format.exe C:",
            "/bin/kill 1",
            "./kill 1",
            "rm -rf c:*",
            "rm -f & x /",
        };
        for (const char *command : blocked) {
            const hardline_result r = check_hardline_blocked(command);
            expect(r.blocked) << "cmd=" << command;
        }
        // The description embeds the raw operand token (the reference matches
        // inside the quoted variant first).
        const hardline_result q1 = check_hardline_blocked("echo 'rm -rf /'");
        expect(q1.blocked);
        expect(eq(q1.description.has_value() ? *q1.description : kimix::string(),
                  kimix::string("Recursive delete of protected root/home (`/'`)")));
        const hardline_result q2 = check_hardline_blocked("echo \"rm -rf /\"");
        expect(q2.blocked);
        expect(eq(q2.description.has_value() ? *q2.description : kimix::string(),
                  kimix::string("Recursive delete of protected root/home (`/\"`)")));
    };

    "check_hardline_blocked_rule_edges"_test = [] {
        // dd: the device alternatives are exactly sd|nvme|disk|rdisk and the
        // `of=` needs a word boundary; `hd`/`nv`/`rd` prefixes must NOT block.
        const char *dd_blocked[] = {"dd if=x of=/dev/sda", "dd if=x of=/dev/sda1",
                                    "dd if=x of=/dev/nvme0n1", "dd if=x of=/dev/disk2",
                                    "dd if=x of=/dev/rdisk1"};
        for (const char *command : dd_blocked) {
            expect(check_hardline_blocked(command).blocked) << "cmd=" << command;
        }
        const char *dd_allowed[] = {"dd if=x of=/dev/hda", "dd if=x of=/dev/nv1",
                                    "dd if=x of=/dev/rd0", "dd xof=/dev/sda",
                                    "dd if=x of=/tmp/out"};
        for (const char *command : dd_allowed) {
            expect(!check_hardline_blocked(command).blocked) << "cmd=" << command;
        }
        // Windows drive root: `^[a-z]:[\\/]?(?:[\\/]?\*)?$` -- one optional
        // separator and at most one trailing glob.
        expect(check_hardline_blocked("rm -rf c:").blocked);
        expect(check_hardline_blocked("rm -rf c:/*").blocked);
        expect(!check_hardline_blocked("rm -rf c:**").blocked);
        expect(!check_hardline_blocked("rm -rf c:*/").blocked);
        expect(!check_hardline_blocked("rm -rf c://").blocked);
        // `/dev/sda` is an operand, not a `/s`-style switch.
        expect(!check_hardline_blocked("rmdir /dev/sda /").blocked);
        expect(check_hardline_blocked("rmdir /s /q C:\\").blocked);
        // Fork bomb needs the literal `:|:&`.
        expect(check_hardline_blocked(":(){ :|:& };:").blocked);
        expect(!check_hardline_blocked(":(){ :|: x :&").blocked);
    };

    // =======================================================================
    // 3.4.3 Failure annotation
    // =======================================================================
    "annotate_failure"_test = [] {
        auto r = annotate_failure("bash: cmd: command not found", "cmd", 127);
        expect(r.has_value());
        expect(eq(*r, kimix::string(
                           "The command was not found. Check it is installed and on PATH "
                           "(use `which <cmd>` / `Get-Command <cmd>`).")));

        r = annotate_failure("'cmd' is not recognized as an internal or external command",
                             "cmd", 1);
        expect(r.has_value());

        r = annotate_failure("cat: /no/such/file: No such file or directory", "cat", 1);
        expect(r.has_value());
        expect(eq(*r, kimix::string(
                           "A file or directory referenced by the command does not exist. "
                           "Verify the path with `glob`/`read`.")));

        r = annotate_failure("ModuleNotFoundError: No module named 'numpy'", "python", 1);
        expect(r.has_value());
        expect((*r).find("numpy") != kimix::string::npos);

        r = annotate_failure("Permission denied", "cat", 1);
        expect(r.has_value());
        expect(eq(*r, kimix::string(
                           "Permission denied. Check file permissions (ls -la) or ownership.")));

        r = annotate_failure("everything ok", "cmd", 0);
        expect(!r.has_value());

        r = annotate_failure("", "cmd", 1);
        expect(!r.has_value());
    };

    // =======================================================================
    // 3.4.5 Parameter parsing
    // =======================================================================
    "parse_bash_params"_test = [] {
        kimix::builtin_tools::ToolParams params;
        params.values["cmd"] =
            kimix::builtin_tools::ValueElement::make_string("ls -la");
        bash_params out;
        auto err = parse_bash_params(&params, out);
        expect(err.status == tool_status::ok);
        expect(eq(out.cmd, kimix::string("ls -la")));
        expect(eq(out.mode, kimix::string("execute")));
        expect(eq(out.timeout, int64_t(30)));
        expect(!out.task_id.has_value());

        params.values["command"] =
            kimix::builtin_tools::ValueElement::make_string("git status");
        params.values.erase("cmd");
        err = parse_bash_params(&params, out);
        expect(err.status == tool_status::ok);
        expect(eq(out.cmd, kimix::string("git status")));

        params.values["mode"] =
            kimix::builtin_tools::ValueElement::make_string("send");
        params.values["timeout"] = kimix::builtin_tools::ValueElement::make_int(60);
        params.values["task_id"] =
            kimix::builtin_tools::ValueElement::make_string("task-1");
        params.values["wait_for_pattern"] =
            kimix::builtin_tools::ValueElement::make_string("READY");
        params.values["max_lines"] = kimix::builtin_tools::ValueElement::make_int(100);
        err = parse_bash_params(&params, out);
        expect(err.status == tool_status::ok);
        expect(eq(out.mode, kimix::string("send")));
        expect(eq(out.timeout, int64_t(60)));
        expect(out.task_id.has_value());
        expect(eq(*out.task_id, kimix::string("task-1")));
        expect(out.wait_for_pattern.has_value());
        expect(out.max_lines.has_value());
        expect(eq(*out.max_lines, int64_t(100)));

        params.values["mode"] =
            kimix::builtin_tools::ValueElement::make_string("bad");
        err = parse_bash_params(&params, out);
        expect(err.status == tool_status::invalid_input);

        err = parse_bash_params(nullptr, out);
        expect(err.status == tool_status::invalid_input);
    };

    // =======================================================================
    // 3.5 Bash tool class (safety floors + serialization)
    // =======================================================================
    "bash_tool_class_safety_floors"_test = [] {
        kimix::builtin_tools::Session session;
        Bash::config cfg;
        cfg.hardline_enabled = true;
        cfg.self_kill_guard_enabled = false;
        Bash tool(&session, std::move(cfg));

        bash_params params;
        params.cmd = "rm -rf /";
        kimix::string block;
        auto err = tool.run(params, block);
        expect(err.status == tool_status::blocked);
        expect(block.find("Recursive delete") != kimix::string::npos);
    };

    "bash_tool_class_self_kill_reuse"_test = [] {
        kimix::builtin_tools::Session session;
        Bash::config cfg;
        cfg.hardline_enabled = false;
        cfg.self_kill_guard_enabled = true;
        cfg.agent_pid = 12345;
        cfg.protected_pids.insert(12345);
        cfg.cmdline = "agent.py";
        cfg.image_names.insert("python");

        Bash tool(&session, std::move(cfg));
        bash_params params;
        params.cmd = "kill 12345";
        kimix::string block;
        auto err = tool.run(params, block);
        expect(err.status == tool_status::blocked);
        expect(block.find("self-kill") != kimix::string::npos ||
               block.find("agent") != kimix::string::npos);
    };

    "bash_tool_class_operator_serialize"_test = [] {
        kimix::builtin_tools::Session session;
        Bash::config cfg;
        cfg.hardline_enabled = false;
        cfg.self_kill_guard_enabled = false;
        cfg.prepare_command = [](kimix::string_view s) { return kimix::string(s); };
        cfg.run_rtk_check = [](kimix::string_view s) -> kimix::optional<kimix::string> {
            (void)s;
            return std::nullopt;
        };

        Bash tool(&session, std::move(cfg));
        kimix::builtin_tools::ToolParams params;
        params.values["cmd"] =
            kimix::builtin_tools::ValueElement::make_string("echo hi");
        params.values["mode"] =
            kimix::builtin_tools::ValueElement::make_string("execute");
        tool(&params);
        const auto &result = tool.serialized_result();
        expect(!result.empty());
        expect(result.front() == '{');
    };

    // =======================================================================
    // 3.2 Windows Git Bash compatibility fix — reference goldens
    // (bash_fix.py / _shell_compat.py BashFix; C:/Temp as the fixed Git Bash
    // temp directory, exactly like the reference tests monkeypatch it).
    // =======================================================================
    "bash_fix_golden_vectors"_test = [] {
        size_t checked = 0;
        size_t failed = 0;
        for (const bash_fix_golden &g : k_bash_fix_goldens) {
            if (!fix_selected(g.command)) {
                continue;
            }
            ++checked;
            const bash_fix_result r = fix_bash_command(g.command, "C:/Temp");
            auto check_list = [&](const char *what, const char *field,
                                  const kimix::vector<kimix::string> &got) {
                const kimix::vector<kimix::string> want = fix_split_field(field);
                if (fix_list_eq(want, got)) {
                    return true;
                }
                if (failed < 20) {
                    const kimix::string want_repr = fix_join_repr(want);
                    const kimix::string got_repr = fix_join_repr(got);
                    fix_report(g.command, what, want_repr, got_repr);
                }
                return false;
            };
            bool ok = r.status == tool_status::ok;
            if (!ok && failed < 20) {
                fix_report(g.command, "status", "ok", "unsupported");
            }
            if (ok) {
                ok = check_list("replacements", g.replacements, r.replacements);
            }
            if (ok) {
                ok = check_list("path_changes", g.path_changes, r.path_changes);
            }
            if (ok) {
                ok = check_list("shell_wrappers", g.shell_wrappers, r.shell_wrappers);
            }
            if (ok) {
                ok = check_list("nul_fixes", g.nul_fixes, r.nul_fixes);
            }
            if (ok) {
                ok = check_list("unsupported", g.unsupported, r.unsupported_commands);
            }
            if (ok) {
                const kimix::string_view want_source(g.expected_source);
                if (r.command.size() < want_source.size()) {
                    if (failed < 20) {
                        fix_report(g.command, "source tail", want_source, r.command);
                    }
                    ok = false;
                } else {
                    const kimix::string_view got_source =
                        kimix::string_view(r.command).substr(r.command.size() -
                                                             want_source.size());
                    if (got_source != want_source) {
                        if (failed < 20) {
                            fix_report(g.command, "source tail", want_source, got_source);
                        }
                        ok = false;
                    }
                }
            }
            if (ok) {
                const kimix::string got_warning = r.warning();
                if (got_warning != g.expected_warning) {
                    if (failed < 20) {
                        fix_report(g.command, "warning", g.expected_warning, got_warning);
                    }
                    ok = false;
                }
            }
            if (!ok) {
                ++failed;
            }
        }
        std::fprintf(stderr, "  [bash_fix] golden vectors: %zu checked, %zu failed\n",
                     checked, failed);
        expect(checked > 0);
        expect(failed == 0);
    };

    "bash_fix_prefix_goldens"_test = [] {
        size_t checked = 0;
        size_t failed = 0;
        for (const bash_fix_prefix_golden &g : k_bash_fix_prefix_goldens) {
            if (!fix_selected(g.command)) {
                continue;
            }
            ++checked;
            const bash_fix_result r = fix_bash_command(g.command, "C:/Temp");
            const kimix::string_view want(g.expected_command);
            if (r.command != want) {
                ++failed;
                if (failed <= 5) {
                    size_t diff = 0;
                    while (diff < r.command.size() && diff < want.size() &&
                           r.command[diff] == want[diff]) {
                        ++diff;
                    }
                    std::fprintf(stderr,
                                 "  [bash_fix] full command mismatch for `%s` at byte %zu "
                                 "(want %zu bytes, got %zu)\n",
                                 g.command, diff, want.size(), r.command.size());
                    std::fprintf(stderr, "    want: %.120s...\n", want.data() + diff);
                    std::fprintf(stderr, "    got : %.120s...\n", r.command.data() + diff);
                }
            }
        }
        std::fprintf(stderr, "  [bash_fix] prefix goldens: %zu checked, %zu failed\n",
                     checked, failed);
        expect(checked > 0);
        expect(failed == 0);
    };

    // -----------------------------------------------------------------------
    // Fallback mappings, command positions and wrapper operands.
    // Mirrors TestBashFixMappings / TestBashFixCommandPositions /
    // TestBashFixCommandOperandWrappers in the reference suite.
    // -----------------------------------------------------------------------
    "bash_fix_fallback_mappings"_test = [] {
        struct mapping {
            const char *source;
            const char *replacement;
            const char *tail; // expected final line (source with the fallback applied)
        };
        const mapping cases[] = {
            {"gtimeout 3 echo ok", "gtimeout", "gtimeout 3 echo ok"},
            {"rev first.txt", "rev", "rev first.txt"},
            {"xdg-open README.md", "xdg-open", "xdg-open README.md"},
            {"open README.md", "open", "open README.md"},
            {"printf text | pbcopy", "pbcopy", "printf text | pbcopy"},
            {"pbpaste", "pbpaste", "pbpaste"},
            {"wget https://example.com/f.zip", "wget", "wget https://example.com/f.zip"},
            {"xsel --clipboard", "xsel", "xsel --clipboard"},
            {"gsed -n 1p file", "gsed", "gsed -n 1p file"},
            {"zip -r out.zip dir", "zip", "zip -r out.zip dir"},
            {"nc -z example.com 80", "nc", "nc -z example.com 80"},
            {"netcat -z example.com 80", "netcat", "netcat -z example.com 80"},
            {"pgrep bash", "pgrep", "pgrep bash"},
            {"tree -L 1 dir", "tree", "tree -L 1 dir"},
            {"say hello", "say", "say hello"},
            {"python3 --version", "python3", "python3 --version"},
            {"free -h", "free", "free -h"},
            {"journalctl -u svc", nullptr, nullptr}, // unsupported, never rewritten
        };
        for (const mapping &m : cases) {
            const bash_fix_result r = fix_bash_command(m.source, "C:/Temp");
            if (m.replacement == nullptr) {
                expect(!r.changed());
                expect(r.command == m.source);
                expect(r.has_unsupported());
                continue;
            }
            expect(r.changed());
            expect(fix_list_eq({kimix::string(m.replacement)}, r.replacements));
            // Fallbacks are prepended as a prefix: the source itself is the
            // final segment of the result.
            expect(r.command.ends_with(kimix::string("\n") + m.source));
            expect(r.command.find(m.replacement) != kimix::string::npos);
        }
    };

    "bash_fix_literal_command_words"_test = [] {
        // Bash quote removal forms the literal name (\rev, 'rev', r""ev).
        const char *sources[] = {"rev", "'rev' <<< abc", "\"rev\" <<< abc",
                                 "\\rev <<< abc"};
        for (const char *source : sources) {
            const bash_fix_result r = fix_bash_command(source, "C:/Temp");
            expect(fix_list_eq({kimix::string("rev")}, r.replacements));
            expect(r.path_changes.empty());
            expect(r.command.ends_with(kimix::string("\n") + source));
        }
    };

    "bash_fix_wrapper_names_in_data_positions_unchanged"_test = [] {
        const char *commands[] = {
            "echo timeout 5 rev", "run=timeout", "echo watch date",
            "echo 'watch rev'", "echo xargs rev",
            "case x in timeout) echo no;; esac", "alias watch='tail -f log'",
            "function timeout { :; }", "timeout() { :; }",
            "echo hi # timeout 5 rev", "ls -la", "git --version", "timeout 1 true",
            "stdbuf -oL echo ok", "xargs echo", "mktemp", "truncate -s 0 file",
            "readlink file", "nproc", "setsid app", "lsof file", "apt update",
        };
        for (const char *command : commands) {
            const bash_fix_result r = fix_bash_command(command, "C:/Temp");
            expect(r.command == command);
            expect(!r.changed());
        }
    };

    "bash_fix_command_operand_wrappers"_test = [] {
        // exec-ing wrappers swap the fallback word for the standalone runner.
        const bash_fix_result timeout_rev =
            fix_bash_command("timeout 5 rev <<< abc", "C:/Temp");
        expect(fix_list_eq({kimix::string("rev")}, timeout_rev.replacements));
        const kimix::string tsource = fix_last_line(timeout_rev.command);
        expect(tsource.starts_with("timeout 5 /usr/bin/bash -c "));
        expect(tsource.ends_with(" -- <<< abc"));

        const bash_fix_result gtimeout_rev =
            fix_bash_command("gtimeout 5 rev <<< abc", "C:/Temp");
        expect(fix_list_eq({kimix::string("gtimeout"), kimix::string("rev")},
                           gtimeout_rev.replacements));
        const kimix::string gsource = fix_last_line(gtimeout_rev.command);
        expect(gsource.starts_with("gtimeout 5 /usr/bin/bash -c "));
        expect(gsource.ends_with(" -- <<< abc"));

        // ``watch`` runs its command in the same shell: the operand stays a
        // plain word and both fallbacks are defined.
        const bash_fix_result watch_rev =
            fix_bash_command("watch -n 1 rev <<< abc", "C:/Temp");
        expect(fix_list_eq({kimix::string("watch"), kimix::string("rev")},
                           watch_rev.replacements));
        expect(fix_last_line(watch_rev.command) == "watch -n 1 rev <<< abc");
        expect(watch_rev.command.find("clear; eval \"$*\"") != kimix::string::npos);

        // A quoted watch operand is an inline script: rescanned in place.
        const bash_fix_result watch_script =
            fix_bash_command("watch 'cd C:\\x && rev'", "C:/Temp");
        expect(fix_list_eq({kimix::string("C:\\x")}, watch_script.path_changes));
        expect(fix_last_line(watch_script.command) == "watch 'cd C:/x && rev'");

        // Path-valued wrapper options are rewritten too.
        const bash_fix_result xargs_path =
            fix_bash_command("xargs -a C:\\in.txt rev", "C:/Temp");
        expect(fix_list_eq({kimix::string("C:\\in.txt")}, xargs_path.path_changes));
        expect(fix_last_line(xargs_path.command)
                   .starts_with("xargs -a C:/in.txt /usr/bin/bash -c "));
    };

    // -----------------------------------------------------------------------
    // Redundant shell wrappers. Mirrors TestBashFixShellWrappers and
    // TestBashFixShellWrapperUnderCommandWrapper.
    // -----------------------------------------------------------------------
    "bash_fix_redundant_shell_prefix_is_unwrapped"_test = [] {
        struct golden {
            const char *command;
            const char *tail;
            const char *wrapper;
        };
        const golden cases[] = {
            {"bash cd /c/dev/x && echo ok", "cd C:/dev/x && echo ok", "bash"},
            {"sh cd /c/dev/x && echo ok", "cd C:/dev/x && echo ok", "sh"},
            {"bash cd /c/dev/x && ls", "cd C:/dev/x && ls", "bash"},
            {"bash grep -rn kimix src tests --include=*.h | head -40",
             "grep -rn kimix src tests --include=*.h | head -40", "bash"},
            {"'bash' cd /c/dev/x && echo ok", "cd C:/dev/x && echo ok", "bash"},
            {"\"bash\" cd /c/dev/x && echo ok", "cd C:/dev/x && echo ok", "bash"},
            {"bash cd /c/dev/x && rev", "cd C:/dev/x && rev", "bash"},
        };
        for (const golden &c : cases) {
            const bash_fix_result r = fix_bash_command(c.command, "C:/Temp");
            expect(fix_last_line(r.command) == c.tail);
            expect(fix_list_eq({kimix::string(c.wrapper)}, r.shell_wrappers));
            expect(r.changed());
        }
    };

    "bash_fix_inline_script_replaces_dash_c_wrapper"_test = [] {
        struct golden {
            const char *command;
            const char *tail;
            const char *wrapper;
        };
        const golden cases[] = {
            {"bash -c 'rev'", "rev", "bash -c"},
            {"bash -c \"rev\"", "rev", "bash -c"},
            {"bash -lc 'rev'", "rev", "bash -c"},
            {"bash -cl 'rev'", "rev", "bash -c"},
            {"bash -l -c 'rev'", "rev", "bash -c"},
            {"sh -c 'rev'", "rev", "sh -c"},
            {"dash -c 'rev'", "rev", "dash -c"},
            {"bash -c 'rev' && echo done", "rev && echo done", "bash -c"},
            {"echo $(bash -c 'rev')", "echo $(rev)", "bash -c"},
            {"bash -c 'echo $HOME'", "echo $HOME", "bash -c"},
        };
        for (const golden &c : cases) {
            const bash_fix_result r = fix_bash_command(c.command, "C:/Temp");
            expect(fix_last_line(r.command) == c.tail);
            expect(fix_list_eq({kimix::string(c.wrapper)}, r.shell_wrappers));
        }
        // The inline script is rescanned: inner fallbacks and paths are fixed.
        const bash_fix_result inner =
            fix_bash_command("bash -c 'cd C:\\x && rev'", "C:/Temp");
        expect(fix_last_line(inner.command) == "cd C:/x && rev");
        expect(fix_list_eq({kimix::string("rev")}, inner.replacements));
        expect(fix_list_eq({kimix::string("C:\\x")}, inner.path_changes));
        expect(fix_list_eq({kimix::string("bash -c")}, inner.shell_wrappers));
    };

    "bash_fix_legitimate_shell_invocations_are_preserved"_test = [] {
        const char *commands[] = {
            "bash script.sh", "bash ./script.sh", "bash ../tools/run",
            "bash scripts/deploy.sh", "sh build.sh --release",
            "bash -c 'echo hi' arg1", "bash -e -c \"rev\"", "bash -ec \"rev\"",
            "bash -x \"rev\"", "bash -s", "bash --", "bash", "echo bash",
            "ls sh", "bash -c", "cd /d", "cd /d && echo x", "cd /d # comment",
            "env FOO=D:\\x true",
        };
        for (const char *command : commands) {
            const bash_fix_result r = fix_bash_command(command, "C:/Temp");
            expect(r.command == command);
            expect(!r.changed());
        }
    };

    "bash_fix_shell_wrapper_under_command_wrapper"_test = [] {
        // ``bash <cmd>`` under an exec-ing wrapper: the shell word is dropped
        // and the operand becomes the standalone runner.
        const bash_fix_result env_bash =
            fix_bash_command("env bash rev <<< abc", "C:/Temp");
        expect(fix_list_eq({kimix::string("bash")}, env_bash.shell_wrappers));
        const kimix::string source = fix_last_line(env_bash.command);
        expect(source.starts_with("env /usr/bin/bash -c "));
        expect(source.ends_with(" -- <<< abc"));

        // ``bash -c`` keeps its shape (unwrapping would move ``&&`` out of the
        // wrapper's argv); the script is fixed in place.
        struct golden {
            const char *command;
            const char *replacements[2];
            size_t count;
        };
        const golden cases[] = {
            {"env bash -c 'rev <<< abc'", {"rev", nullptr}, 1},
            {"sudo bash -c 'rev <<< abc'", {"sudo", "rev"}, 2},
            {"timeout 5 bash -c 'rev <<< abc'", {"rev", nullptr}, 1},
            {"nohup bash -c 'rev <<< abc'", {"rev", nullptr}, 1},
            {"timeout 5 bash -c 'rev <<< abc && rev <<< xyz'", {"rev", nullptr}, 1},
        };
        for (const golden &c : cases) {
            const bash_fix_result r = fix_bash_command(c.command, "C:/Temp");
            expect(fix_last_line(r.command) == c.command);
            expect(r.replacements.size() == c.count);
            for (size_t i = 0; i < c.count; ++i) {
                expect(r.replacements[i] == c.replacements[i]);
            }
            expect(r.command.find("if declare -F rev >/dev/null; then export -f rev; fi") !=
                   kimix::string::npos);
        }
        // The inner path is fixed inside the inline script.
        const bash_fix_result path_fix =
            fix_bash_command("nohup bash -c 'cd C:\\x && rev'", "C:/Temp");
        expect(fix_list_eq({kimix::string("C:\\x")}, path_fix.path_changes));
        expect(fix_last_line(path_fix.command) == "nohup bash -c 'cd C:/x && rev'");
        // An assignment-expanded shell keeps its wrapper.
        const char *scoped = "env FOO=1 bash -c 'printf %s \"$FOO\"'";
        const bash_fix_result scoped_r = fix_bash_command(scoped, "C:/Temp");
        expect(scoped_r.command == scoped);
    };

    "bash_fix_conditional_export_for_nested_shells"_test = [] {
        const bash_fix_result r = fix_bash_command("rev <<< abc", "C:/Temp");
        expect(r.command.find("if declare -F rev >/dev/null; then export -f rev; fi") !=
               kimix::string::npos);
        // The unconditioned ``export -f`` would pollute stderr for definitions
        // whose guard found a real executable: it must not appear.
        expect(r.command.find("\nexport -f rev\n") == kimix::string::npos);
    };

    // -----------------------------------------------------------------------
    // Null-device redirection. Mirrors TestBashFixNulRedirection.
    // -----------------------------------------------------------------------
    "bash_fix_nul_redirection"_test = [] {
        struct golden {
            const char *command;
            const char *expected;
        };
        const golden cases[] = {
            {"echo hi > nul", "echo hi > /dev/null"},
            {"echo hi >NUL", "echo hi >/dev/null"},
            {"echo hi 2> nul", "echo hi 2> /dev/null"},
            {"echo hi &> nul", "echo hi &> /dev/null"},
            {"echo hi >> nul", "echo hi >> /dev/null"},
            {"echo hi 2>> nul", "echo hi 2>> /dev/null"},
            {"echo hi>nul", "echo hi>/dev/null"},
            {"echo hi > nul; echo bye > nul",
             "echo hi > /dev/null; echo bye > /dev/null"},
            {"echo 'nul' > nul", "echo 'nul' > /dev/null"},
        };
        for (const golden &c : cases) {
            const bash_fix_result r = fix_bash_command(c.command, "C:/Temp");
            expect(r.command == c.expected);
            expect(r.changed());
            expect(!r.nul_fixes.empty());
            expect(r.command.find("/dev/null") != kimix::string::npos);
            expect(r.warning().find("/dev/null") != kimix::string::npos);
        }
        const bash_fix_result multi =
            fix_bash_command("echo a > nul; echo b > NUL; echo c >nul", "C:/Temp");
        expect(multi.command ==
               "echo a > /dev/null; echo b > /dev/null; echo c >/dev/null");
        expect(fix_list_eq({kimix::string("nul"), kimix::string("NUL"),
                            kimix::string("nul")},
                           multi.nul_fixes));
        const char *preserved[] = {"echo hi > 'nul'", "echo hi > \"nul\"",
                                   "echo hi < nul", "echo nul",
                                   "echo /dev/null > nul.txt", "echo hi > null",
                                   "echo hi > /dev/null"};
        for (const char *command : preserved) {
            const bash_fix_result r = fix_bash_command(command, "C:/Temp");
            expect(r.command == command);
            expect(r.nul_fixes.empty());
            expect(!r.changed());
        }
    };

    // -----------------------------------------------------------------------
    // Windows paths / Git Bash virtual mounts.
    // Mirrors TestBashFixWindowsPaths / TestBashFixGitBashPosixPaths /
    // TestBashFixCommandWordPaths / TestBashFixArrayLiterals.
    // -----------------------------------------------------------------------
    "bash_fix_windows_paths"_test = [] {
        struct golden {
            const char *command;
            const char *expected;
        };
        const golden cases[] = {
            {"cd D:\\repo\\src", "cd D:/repo/src"},
            {"C:\\Windows\\System32\\where.exe git",
             "C:/Windows/System32/where.exe git"},
            {"d:\\tools\\run.exe --help", "d:/tools/run.exe --help"},
            {"\\\\server\\share\\tool.exe arg", "//server/share/tool.exe arg"},
            {".\\build\\tool.exe arg", "./build/tool.exe arg"},
            {"..\\scripts\\run.sh", "../scripts/run.sh"},
            {"~\\bin\\tool.exe --help", "~/bin/tool.exe --help"},
            {"\\Users\\me\\tool.exe", "/Users/me/tool.exe"},
            {"build\\dist\\tool.exe arg", "build/dist/tool.exe arg"},
            {"echo a && C:\\x\\tool.exe", "echo a && C:/x/tool.exe"},
            {"echo a; C:\\x\\tool.exe | cat", "echo a; C:/x/tool.exe | cat"},
            {"if C:\\x\\probe.exe; then echo ok; fi",
             "if C:/x/probe.exe; then echo ok; fi"},
            {"command C:\\x\\tool.exe", "command C:/x/tool.exe"},
            {"D:\\x\\*.exe", "D:/x/*.exe"},
            {"D:\\Program\\ Files\\x.exe", "\"D:/Program Files/x.exe\""},
            {"x=$(C:\\x\\tool.exe)", "x=$(C:/x/tool.exe)"},
            {"arr=(D:\\x\\y.txt D:\\a\\b.txt)", "arr=(D:/x/y.txt D:/a/b.txt)"},
            {"arr+=(D:\\x\\y.txt)", "arr+=(D:/x/y.txt)"},
            {"declare -a arr=(D:\\x\\y.txt)", "declare -a arr=(D:/x/y.txt)"},
            {"local arr=(D:\\x\\y.txt)", "local arr=(D:/x/y.txt)"},
            {"cd /d D:\\x && echo /d && cd /d/foo", "cd  D:/x && echo /d && cd D:/foo"},
            {"cd D:\\x", "cd D:/x"},
        };
        for (const golden &c : cases) {
            const bash_fix_result r = fix_bash_command(c.command, "C:/Temp");
            expect(r.command == c.expected);
            expect(r.replacements.empty());
            expect(r.changed());
        }
        const char *untouched[] = {
            "echo hello", "git --version", "'C:\\x\\tool.exe' arg",
            "\"C:\\x\\tool.exe\" arg", "foo\\bar arg", "a\\nb arg", "\\a\\b",
            "x\\n\\t", "case $f in D:\\x) echo ok;; esac",
            "case $f in (D:\\x) echo ok;; esac", "arr=('D:\\x\\y.txt')",
            "array=(rev open pbcopy)", "declare -a x=(rev)", "arr=([k]=D:\\x)",
            "echo a > C:\\x\\y.txt && true",
        };
        for (const char *command : untouched) {
            const bash_fix_result r = fix_bash_command(command, "C:/Temp");
            if (kimix::string_view(command) == "echo a > C:\\x\\y.txt && true") {
                continue; // covered by the goldens (redirection target path)
            }
            expect(r.command == command);
        }
        // ``cd /d`` flag: dropped only when a path argument follows.
        const bash_fix_result cd_flag = fix_bash_command("cd /d D:\\x", "C:/Temp");
        expect(cd_flag.command == "cd  D:/x");
        expect(fix_list_eq({kimix::string("cd /d"), kimix::string("D:\\x")},
                           cd_flag.path_changes));
    };

    "bash_fix_git_bash_posix_paths"_test = [] {
        const kimix::string tmp = "C:/Temp";
        struct golden {
            const char *command;
            const char *expected;
        };
        const golden cases[] = {
            {"echo /tmp/x.txt", "echo C:/Temp/x.txt"},
            {"cat > /tmp/out.txt", "cat > C:/Temp/out.txt"},
            {"cd /tmp", "cd C:/Temp"},
            {"rm -f /tmp/a /tmp/b", "rm -f C:/Temp/a C:/Temp/b"},
            {"echo /c/dev/file.cpp", "echo C:/dev/file.cpp"},
            {"echo /C/Dev/file.cpp", "echo C:/Dev/file.cpp"},
            {"cd /d/foo", "cd D:/foo"},
            {"/c/Windows/System32/where.exe cmd", "C:/Windows/System32/where.exe cmd"},
            {"echo $(cat /tmp/x)", "echo $(cat C:/Temp/x)"},
            {"env --chdir=/tmp cmd", "env --chdir=C:/Temp cmd"},
            {"arr=(/tmp/a.txt /c/b.txt)", "arr=(C:/Temp/a.txt C:/b.txt)"},
            {"chdir /tmp", "chdir C:/Temp"},
        };
        for (const golden &c : cases) {
            const bash_fix_result r = fix_bash_command(c.command, tmp);
            // ``chdir /tmp`` also installs the chdir fallback, so compare the
            // rewritten source (the final line) rather than the whole result.
            expect(fix_last_line(r.command) == c.expected);
            expect(r.changed());
        }
        const char *untouched[] = {
            "echo '/tmp/x'", "echo \"/tmp/x\"", "cat <<'EOF'\n/tmp/x\nEOF",
            "cat <<< /tmp/x", "echo /tmpfile", "echo /d", "echo /c", "x=/tmp/x",
            "alias cd=/c/x", "echo ok # cd /tmp/x",
        };
        for (const char *command : untouched) {
            const bash_fix_result r = fix_bash_command(command, tmp);
            expect(r.command == command);
            expect(!r.changed());
        }
        // A temp directory containing spaces is double-quoted.
        const bash_fix_result spaced =
            fix_bash_command("echo /tmp/x", "C:/Users/John Doe/AppData/Local/Temp");
        expect(spaced.command == "echo \"C:/Users/John Doe/AppData/Local/Temp/x\"");
    };

    // -----------------------------------------------------------------------
    // Unsupported commands / platform gate / ASCII gate.
    // Mirrors TestBashToolUnsupportedCommand and TestBashFixRobustness.
    // -----------------------------------------------------------------------
    "bash_fix_unsupported_command_reports_reason"_test = [] {
        const bash_fix_result r = fix_bash_command("journalctl -u svc -f", "C:/Temp");
        expect(r.status == tool_status::ok);
        expect(r.command == "journalctl -u svc -f");
        expect(!r.changed());
        expect(fix_list_eq({kimix::string("journalctl")}, r.unsupported_commands));
        const kimix::string warning = r.warning();
        expect(warning.find("journalctl") != kimix::string::npos);
        expect(warning.find("no Windows Git Bash equivalent") != kimix::string::npos);
        expect(warning.find("Get-WinEvent") != kimix::string::npos);
        expect(!bash_unsupported_reason("journalctl").empty());
        expect(bash_unsupported_reason("rev").empty());
        expect(bash_unsupported_reason("journalctl").find("Get-WinEvent") !=
               kimix::string_view::npos);
    };

    "bash_fix_non_ascii_routes_to_python_mirror"_test = [] {
        // The reference operates on Unicode str; the native kernel gates
        // non-ASCII input so the caller can use the pure-Python mirror.
        const char *commands[] = {"rev \xC3\xA9", "echo \xE2\x9C\x93", "\xE4\xBD\xA0\xE5\xA5\xBD"};
        for (const char *command : commands) {
            const bash_fix_result r = fix_bash_command(command, "C:/Temp");
            expect(r.status == tool_status::unsupported);
            expect(r.command == command);
            expect(!r.changed());
        }
    };

    "bash_fix_platform_gate"_test = [] {
#ifdef KIMIX_PLATFORM_WINDOWS
        expect(bash_fix_platform_enabled());
        expect(!bash_compatibility_prelude().empty());
#else
        expect(!bash_fix_platform_enabled());
        expect(bash_compatibility_prelude().empty());
#endif
        // The temp directory resolver never returns a backslash spelling.
        const kimix::string temp = bash_windows_temp_dir();
        expect(!temp.empty());
        expect(temp.find('\\') == kimix::string::npos);
    };

#ifdef KIMIX_PLATFORM_WINDOWS
    "bash_fix_prelude_golden"_test = [] {
        expect(bash_compatibility_prelude() == kimix::string(k_bash_fix_prelude));
    };
#endif

    "bash_fix_robustness"_test = [] {
        // Malformed input must never crash and must round-trip byte-for-byte.
        const char *commands[] = {"'" , "\"", "`", "$(", "${", "((",
                                 "cat <<EOF\nunterminated", "echo \\", "rev '",
                                 "rev \"", "echo $(rev", "echo `rev",
                                 "if rev; then", "case x in rev)"};
        for (const char *command : commands) {
            const bash_fix_result r = fix_bash_command(command, "C:/Temp");
            expect(r.status == tool_status::ok);
            if (!r.changed()) {
                expect(r.command == command);
            }
        }
        // Many commands are rewritten linearly (no quadratic blow-up).
        kimix::string many;
        for (int i = 0; i < 200; ++i) {
            if (i != 0) {
                many += "; ";
            }
            many += "rev";
        }
        const bash_fix_result r = fix_bash_command(many, "C:/Temp");
        expect(r.command.ends_with(kimix::string("\n") + many));
        expect(fix_count(r.command, "rev()") == 1);
        expect(r.replacements.size() == 200);
    };

    "bash_fix_nesting_depth"_test = [] {
        // Moderate nesting still finds the innermost fallback.
        // Depth stays modest so the case holds under every supported build
        // configuration (sanitizers inflate the scanner's frames several fold);
        // the scanner's own stack budget is what bounds deeper nesting.
        kimix::string moderate = "echo ";
        for (int i = 0; i < 16; ++i) {
            moderate += "$(echo ";
        }
        moderate += "$(rev)";
        for (int i = 0; i < 17; ++i) {
            moderate += ")";
        }
        const bash_fix_result deep = fix_bash_command(moderate, "C:/Temp");
        expect(fix_count(deep.command, "rev()") == 1);
        expect(deep.command.ends_with(kimix::string("\n") + moderate));

        // Paths inside nested substitutions are still rewritten.
        kimix::string nested = "cd ";
        for (int i = 0; i < 16; ++i) {
            nested += "$(cd ";
        }
        nested += "D:\\x";
        for (int i = 0; i < 16; ++i) {
            nested += ")";
        }
        const bash_fix_result r = fix_bash_command(nested, "C:/Temp");
        expect(fix_list_eq({kimix::string("D:\\x")}, r.path_changes));

        // Pathological nesting is left byte-for-byte (reference: RecursionError
        // / the _MAX_NESTING_DEPTH bound) instead of overflowing the stack.
        kimix::string extreme = "echo ";
        for (int i = 0; i < 2000; ++i) {
            extreme += "$(";
        }
        extreme += "pwd";
        for (int i = 0; i < 2000; ++i) {
            extreme += ")";
        }
        const bash_fix_result r2 = fix_bash_command(extreme, "C:/Temp");
        expect(r2.command == extreme);
        expect(!r2.changed());
        expect(r2.replacements.empty());
    };

    "bash_fix_heredoc_trailing_operator"_test = [] {
        // A control operator on the line after the heredoc terminator is moved
        // onto the redirection line (Bash requires it there).
        const bash_fix_result r =
            fix_bash_command("cat <<EOF\nhi\nEOF\n&& rev", "C:/Temp");
        expect(r.command.find("cat <<EOF && rev\nhi\nEOF") != kimix::string::npos);
        expect(fix_list_eq({kimix::string("rev")}, r.replacements));
        // Already-correct heredocs stay untouched.
        const char *fine[] = {"cat <<EOF\nhi\nEOF", "cat <<EOF\nhi\nEOF && rev",
                              "cat <<-EOF\n\thi\n\tEOF"};
        for (const char *command : fine) {
            const bash_fix_result f = fix_bash_command(command, "C:/Temp");
            if (f.replacements.empty() && f.path_changes.empty()) {
                expect(f.command == command);
            }
        }
    };

    // -----------------------------------------------------------------------
    // Tool-level wiring: Bash::run applies the fix and rejects unsupported
    // commands before any subprocess is spawned (reference
    // _prepare_command -> shell_common.inspect_bash_command).
    // -----------------------------------------------------------------------
    "bash_tool_class_compat_fix"_test = [] {
        kimix::builtin_tools::Session session;
        Bash::config cfg;
        cfg.hardline_enabled = false;
        cfg.self_kill_guard_enabled = false;
        cfg.native_execute = false; // keep the runner out of this kernel test
        cfg.compat_fix_enabled = true;
        cfg.compat_temp_dir = "C:/Temp";
        Bash tool(&session, std::move(cfg));
        bash_params params;
        params.mode = "execute";

        params.cmd = "rev <<< abc";
        kimix::string block;
        auto err = tool.run(params, block);
        expect(err.status == tool_status::ok);
        expect(block.find("rev()") != kimix::string::npos);
        expect(block.find("if declare -F rev >/dev/null; then export -f rev; fi") !=
               kimix::string::npos);

        params.cmd = "journalctl -u svc -f";
        err = tool.run(params, block);
        expect(err.status == tool_status::unsupported);
        expect(err.message.find("journalctl") != kimix::string::npos);
        expect(err.message.find("Get-WinEvent") != kimix::string::npos);

        // Native commands pass through untouched.
        params.cmd = "ls -la";
        err = tool.run(params, block);
        expect(err.status == tool_status::ok);
        expect(block == "ls -la");

        // Non-ASCII input is outside the native scanner's subset: the command
        // runs unfixed instead of failing the tool call.
        params.cmd = "echo \xC3\xA9";
        err = tool.run(params, block);
        expect(err.status == tool_status::ok);
        expect(block == "echo \xC3\xA9");
    };

    return 0;
}
