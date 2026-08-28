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
#include "builtin_tools/utf8_util.h"

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

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

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

    return 0;
}
