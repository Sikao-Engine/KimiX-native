// cli/cli_commands.cpp - The slash-command handlers (see cli_commands.h).
//
// Port of kimix/cli_impl/commands.py (revision 86b7bf6) plus the session /
// context helpers of kimix/utils/session.py:
//   _cmd_help/_cmd_clear/_cmd_compact/_cmd_context/_cmd_exit/_cmd_file/_cmd_txt/
//   _cmd_export/_cmd_resume/_cmd_store/_cmd_load/_cmd_sessions/_cmd_init/
//   _cmd_cmd/_cmd_fix/_cmd_todo/_cmd_plan/_cmd_swarm/_cmd_supervisor/
//   _cmd_reflection/_cmd_code/_cmd_unknown
// Every literal (message text, prompt) is traceable to
// .kimix_cache/cli_specs/04_commands_session.md §1.3; the reductions and
// deviations are documented in src/cli/reports/cli_commands.md.
//
// Unity build: every TU-local helper lives in an anonymous namespace with the
// `clicmd_` prefix.

#include "cli/cli_commands.h"

#include <cstdint>
#include <cstdio>

#include "builtin_tools/process_runner.h"

#include "cli/cli_common.h"
#include "cli/cli_print.h"
#include "cli/cli_stream.h"

namespace kimix::cli {

namespace {

namespace proc = kimix::builtin_tools::proc;

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

// The reference's `':'.join(task_split[1:])`: everything after the command's
// first ':'.  The REPL already splits at the first colon, so this is normally a
// single element; the join keeps handlers well defined when called directly.
kimix::string clicmd_payload(const kimix::vector<kimix::string> &args) {
    if (args.size() < 2) {
        return {};
    }
    kimix::string out = args[1];
    for (size_t i = 2; i < args.size(); ++i) {
        out += ':';
        out += args[i];
    }
    return out;
}

// Python's str.ljust: pad with spaces, never truncate.
kimix::string clicmd_pad(kimix::string_view text, size_t width) {
    kimix::string out(text);
    while (out.size() < width) {
        out.push_back(' ');
    }
    return out;
}

// [A-Za-z0-9] (the TODO word-boundary predicate is ASCII in the reference's
// character class).
bool clicmd_is_alnum(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9');
}

// Resolve a user-supplied path: absolute stays as given, relative resolves
// against the session's working directory.
kimix::string clicmd_resolve(const app_context &app, kimix::string_view given) {
    std::error_code ec;
    const kimix::filesystem::path path{kimix::string(given)};
    if (path.is_absolute()) {
        return absolute_path(given);
    }
    return absolute_path(join_path(app.work_dir, given));
}

// `_read_multi_line(text_arr, allow_cancel)`: read lines until /end (or, when
// `allow_cancel`, /cancel) through the shared input primitive.  EOF ends the
// block (the reference's EOFError inside _input is not caught there either).
void clicmd_read_multi_line(app_context &app, bool allow_cancel,
                            kimix::vector<kimix::string> &lines, bool &cancelled) {
    lines.clear();
    cancelled = false;
    for (;;) {
        kimix::string line;
        if (!app_read_input(app, "", line)) {
            return;
        }
        if (trim(line) == "/end") {
            return;
        }
        if (allow_cancel && trim(line) == "/cancel") {
            lines.clear();
            cancelled = true;
            return;
        }
        lines.push_back(line);
    }
}

// The "/end" / "/cancel" banner the four multi-line commands print.
kimix::string clicmd_multi_line_banner(kimix::string_view what) {
    return kimix::string("\n>>>> ") + kimix::string(what) + ", end with " +
           colorful_text("/end", 33) + ", cancel with " + colorful_text("/cancel", 33);
}

kimix::string clicmd_join_lines(const kimix::vector<kimix::string> &lines,
                                kimix::string_view separator) {
    kimix::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            out += separator;
        }
        out += lines[i];
    }
    return out;
}

// ---------------------------------------------------------------------------
// Process helpers (/cmd, /code; /fix uses clicmd_run_shell too)
// ---------------------------------------------------------------------------

struct clicmd_process_result {
    bool started = false;
    int64_t exit_code = -1;
    kimix::string output;
    kimix::string spawn_error;
    bool killed = false;
};

// `os.system`-equivalent: the command runs through the platform shell.
clicmd_process_result clicmd_run_shell(kimix::string_view command) {
    proc::run_options options;
#if defined(KIMIX_PLATFORM_WINDOWS)
    options.argv.push_back(kimix::string("cmd.exe"));
    options.argv.push_back(kimix::string("/c"));
#else
    options.argv.push_back(kimix::string("/bin/sh"));
    options.argv.push_back(kimix::string("-c"));
#endif
    options.argv.push_back(kimix::string(command));
    // os.system() has no timeout; the runner gets a generous bound so a hung
    // child can not block the CLI forever (documented in the report).
    options.timeout_ms = 600000;
    const proc::run_result result = proc::run_process(options);
    clicmd_process_result out;
    out.started = result.spawn_error.empty();
    out.output = result.output;
    out.spawn_error = result.spawn_error;
    out.killed = result.killed;
    if (result.exit_code.has_value()) {
        out.exit_code = result.exit_code.value();
    }
    return out;
}

// `subprocess.run([...], capture_output=False)`: the child writes directly to
// the CLI's own stdout/stderr (multi-line text goes through print_raw, never a
// bare printf).
clicmd_process_result clicmd_run_argv(const kimix::vector<kimix::string> &argv) {
    proc::run_options options;
    options.argv = argv;
    options.timeout_ms = 600000;
    const proc::run_result result = proc::run_process(options);
    clicmd_process_result out;
    out.started = result.spawn_error.empty();
    out.output = result.output;
    out.spawn_error = result.spawn_error;
    out.killed = result.killed;
    if (result.exit_code.has_value()) {
        out.exit_code = result.exit_code.value();
    }
    return out;
}

// The /init template (a native reduction: the reference's wizard asks the user
// for model / base_url / api_key and writes <repo>/src/kimix/default_config.json).
constexpr const char *k_clicmd_default_config_template =
    "{\n"
    "  \"model\": \"\",\n"
    "  \"type\": \"openai\",\n"
    "  \"url\": \"\",\n"
    "  \"api_key\": \"\",\n"
    "  \"max_context_size\": 0,\n"
    "  \"max_tokens\": 0\n"
    "}\n";

// `_filter_error_output(result, code, ('error',), skip_success=True)`: the
// whole output when it succeeded, otherwise everything from the first line
// containing "error" (the whole output when no line does).
kimix::string clicmd_filter_error_output(kimix::string_view output, bool succeeded) {
    if (succeeded) {
        return {};
    }
    kimix::vector<kimix::string> lines;
    split_lines(output, lines);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (contains(to_lower_ascii(lines[i]), "error")) {
            kimix::vector<kimix::string> tail(lines.begin() + static_cast<ptrdiff_t>(i),
                                              lines.end());
            return clicmd_join_lines(tail, "\n");
        }
    }
    return kimix::string(output);
}

// ---------------------------------------------------------------------------
// /todo - the 7 suffix families' TODO-comment scan
//
// The native comment scanner that already exists (src/runtime/parse/
// comment_scanner.h: scan_comments(lang_kind, ...)) is compiled into the
// runtime_py target only and src/cli/xmake.lua (frozen, globs *.cpp) can not
// add it to kimix-cli, so this file carries a self-contained scanner for the
// same seven families.  It implements the subset the TODO prompt needs (the
// reference's Comment.content/line), following the span table of that header.
// ---------------------------------------------------------------------------

enum class clicmd_lang : int32_t { python, c, shell, html, pascal_lang, lisp, sql, unsupported };

struct clicmd_comment {
    int64_t line = 1;       // 1-based start line (Comment.line)
    kimix::string content;  // Comment.content (markers included/excluded per family)
};

clicmd_lang clicmd_lang_for_suffix(const kimix::string &suffix) {
    if (suffix == ".py") {
        return clicmd_lang::python;
    }
    if (suffix == ".c" || suffix == ".cpp" || suffix == ".cc" || suffix == ".cxx" ||
        suffix == ".h" || suffix == ".hpp" || suffix == ".java" || suffix == ".js" ||
        suffix == ".ts" || suffix == ".jsx" || suffix == ".tsx" || suffix == ".cs" ||
        suffix == ".go" || suffix == ".rs") {
        return clicmd_lang::c;
    }
    if (suffix == ".sh" || suffix == ".bash" || suffix == ".zsh") {
        return clicmd_lang::shell;
    }
    if (suffix == ".html" || suffix == ".htm" || suffix == ".xml" || suffix == ".svg") {
        return clicmd_lang::html;
    }
    if (suffix == ".pas" || suffix == ".pp" || suffix == ".inc" || suffix == ".dpr") {
        return clicmd_lang::pascal_lang;
    }
    if (suffix == ".lisp" || suffix == ".lsp" || suffix == ".clj" || suffix == ".scm" ||
        suffix == ".ss" || suffix == ".el") {
        return clicmd_lang::lisp;
    }
    if (suffix == ".sql") {
        return clicmd_lang::sql;
    }
    return clicmd_lang::unsupported;
}

// The line number of byte offset `at` in `text` (1-based).
int64_t clicmd_line_at(kimix::string_view text, size_t at) {
    int64_t line = 1;
    for (size_t i = 0; i < at && i < text.size(); ++i) {
        if (text[i] == '\n') {
            ++line;
        }
    }
    return line;
}

// Skip an ASCII string literal starting at text[i] (quote); returns the index
// one past the closing quote (or text.size()).
size_t clicmd_skip_string(kimix::string_view text, size_t i, char quote) {
    ++i;
    while (i < text.size()) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            i += 2;
            continue;
        }
        if (text[i] == quote) {
            return i + 1;
        }
        ++i;
    }
    return text.size();
}

void clicmd_push_comment(kimix::vector<clicmd_comment> &out, int64_t line,
                         kimix::string_view content) {
    clicmd_comment c;
    c.line = line;
    c.content.assign(content.data(), content.size());
    out.push_back(std::move(c));
}

// One pass over the source for one language family.
void clicmd_scan_comments(clicmd_lang lang, kimix::string_view text,
                          kimix::vector<clicmd_comment> &out) {
    out.clear();
    size_t i = 0;
    while (i < text.size()) {
        const char ch = text[i];
        if (lang == clicmd_lang::c) {
            if (ch == '"' || ch == '\'') {
                i = clicmd_skip_string(text, i, ch);
                continue;
            }
            if (ch == '/' && i + 1 < text.size() && text[i + 1] == '/') {
                size_t end = text.find('\n', i);
                if (end == kimix::string_view::npos) {
                    end = text.size();
                }
                clicmd_push_comment(out, clicmd_line_at(text, i), text.substr(i + 2, end - i - 2));
                i = end;
                continue;
            }
            if (ch == '/' && i + 1 < text.size() && text[i + 1] == '*') {
                const bool doc = (i + 2 < text.size() && text[i + 2] == '*');
                const size_t open = doc ? 3 : 2;
                size_t end = text.find("*/", i + open);
                if (end == kimix::string_view::npos) {
                    end = text.size();
                }
                clicmd_push_comment(out, clicmd_line_at(text, i),
                                    text.substr(i + open, end - i - open));
                i = (end == text.size()) ? end : end + 2;
                continue;
            }
            ++i;
            continue;
        }
        if (lang == clicmd_lang::python) {
            if (ch == '"' || ch == '\'') {
                const bool triple = (i + 2 < text.size() && text[i + 1] == ch &&
                                     text[i + 2] == ch);
                if (triple) {
                    size_t end = text.find(kimix::string(3, ch), i + 3);
                    kimix::string_view inner = (end == kimix::string_view::npos)
                                                   ? text.substr(i)
                                                   : text.substr(i, end - i + 3);
                    clicmd_push_comment(out, clicmd_line_at(text, i), inner);
                    i = (end == kimix::string_view::npos) ? text.size() : end + 3;
                    continue;
                }
                i = clicmd_skip_string(text, i, ch);
                continue;
            }
            if (ch == '#') {
                size_t end = text.find('\n', i);
                if (end == kimix::string_view::npos) {
                    end = text.size();
                }
                clicmd_push_comment(out, clicmd_line_at(text, i), text.substr(i, end - i));
                i = end;
                continue;
            }
            ++i;
            continue;
        }
        if (lang == clicmd_lang::shell) {
            if (ch == '"' || ch == '\'') {
                i = clicmd_skip_string(text, i, ch);
                continue;
            }
            if (ch == '#') {
                size_t end = text.find('\n', i);
                if (end == kimix::string_view::npos) {
                    end = text.size();
                }
                clicmd_push_comment(out, clicmd_line_at(text, i), text.substr(i, end - i));
                i = end;
                continue;
            }
            ++i;
            continue;
        }
        if (lang == clicmd_lang::html) {
            if (ch == '<' && i + 3 < text.size() && text.compare(i, 4, "<!--") == 0) {
                size_t end = text.find("-->", i + 4);
                if (end == kimix::string_view::npos) {
                    break; // unclosed constructs at EOF emit NO comment
                }
                clicmd_push_comment(out, clicmd_line_at(text, i), text.substr(i + 4, end - i - 4));
                i = end + 3;
                continue;
            }
            if (ch == '<' && i + 1 < text.size() && text[i + 1] == '?') {
                size_t end = text.find("?>", i + 2);
                if (end == kimix::string_view::npos) {
                    break;
                }
                clicmd_push_comment(out, clicmd_line_at(text, i), text.substr(i + 2, end - i - 2));
                i = end + 2;
                continue;
            }
            ++i;
            continue;
        }
        if (lang == clicmd_lang::pascal_lang) {
            if (ch == '{') {
                size_t end = text.find('}', i + 1);
                if (end == kimix::string_view::npos) {
                    end = text.size();
                }
                clicmd_push_comment(out, clicmd_line_at(text, i), text.substr(i + 1, end - i - 1));
                i = (end == text.size()) ? end : end + 1;
                continue;
            }
            if (ch == '(' && i + 1 < text.size() && text[i + 1] == '*') {
                size_t end = text.find("*)", i + 2);
                if (end == kimix::string_view::npos) {
                    end = text.size();
                }
                clicmd_push_comment(out, clicmd_line_at(text, i), text.substr(i + 2, end - i - 2));
                i = (end == text.size()) ? end : end + 2;
                continue;
            }
            if (ch == '/' && i + 1 < text.size() && text[i + 1] == '/') {
                size_t end = text.find('\n', i);
                if (end == kimix::string_view::npos) {
                    end = text.size();
                }
                clicmd_push_comment(out, clicmd_line_at(text, i), text.substr(i + 2, end - i - 2));
                i = end;
                continue;
            }
            if (ch == '"' || ch == '\'') {
                i = clicmd_skip_string(text, i, ch);
                continue;
            }
            ++i;
            continue;
        }
        if (lang == clicmd_lang::lisp) {
            if (ch == ';') {
                size_t end = text.find('\n', i);
                if (end == kimix::string_view::npos) {
                    end = text.size();
                }
                clicmd_push_comment(out, clicmd_line_at(text, i), text.substr(i, end - i));
                i = end;
                continue;
            }
            if (ch == '#' && i + 1 < text.size() && text[i + 1] == '|') {
                size_t end = text.find("|#", i + 2);
                if (end == kimix::string_view::npos) {
                    end = text.size();
                }
                clicmd_push_comment(out, clicmd_line_at(text, i),
                                    text.substr(i, (end == text.size() ? end : end + 2) - i));
                i = (end == text.size()) ? end : end + 2;
                continue;
            }
            ++i;
            continue;
        }
        // sql
        if (ch == '-' && i + 1 < text.size() && text[i + 1] == '-') {
            const char after = (i + 2 < text.size()) ? text[i + 2] : '\0';
            if (after == ' ' || after == '\t' || after == '\n' || after == '\r' || after == '\0') {
                size_t end = text.find('\n', i);
                if (end == kimix::string_view::npos) {
                    end = text.size();
                }
                clicmd_push_comment(out, clicmd_line_at(text, i), text.substr(i + 2, end - i - 2));
                i = end;
                continue;
            }
            ++i;
            continue;
        }
        if (ch == '#') {
            size_t end = text.find('\n', i);
            if (end == kimix::string_view::npos) {
                end = text.size();
            }
            clicmd_push_comment(out, clicmd_line_at(text, i), text.substr(i + 1, end - i - 1));
            i = end;
            continue;
        }
        if (ch == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            size_t end = text.find("*/", i + 2);
            if (end == kimix::string_view::npos) {
                end = text.size();
            }
            clicmd_push_comment(out, clicmd_line_at(text, i), text.substr(i + 2, end - i - 2));
            i = (end == text.size()) ? end : end + 2;
            continue;
        }
        if (ch == '\'' || ch == '"') {
            i = clicmd_skip_string(text, i, ch);
            continue;
        }
        ++i;
    }
}

// `regex.search(r'(?<![a-zA-Z0-9])TODO(?![a-zA-Z0-9])', content.upper())`.
bool clicmd_has_todo(kimix::string_view content) {
    const kimix::string upper = to_upper_ascii(content);
    size_t from = 0;
    for (;;) {
        const size_t at = upper.find("TODO", from);
        if (at == kimix::string::npos) {
            return false;
        }
        const bool before_ok = (at == 0) || !clicmd_is_alnum(upper[at - 1]);
        const bool after_ok =
            (at + 4 >= upper.size()) || !clicmd_is_alnum(upper[at + 4]);
        if (before_ok && after_ok) {
            return true;
        }
        from = at + 4;
    }
}

// Whitespace split (Python's str.split()).
void clicmd_split_ws(kimix::string_view text, kimix::vector<kimix::string> &out) {
    out.clear();
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
                                   text[i] == '\r' || text[i] == '\v' || text[i] == '\f')) {
            ++i;
        }
        const size_t start = i;
        while (i < text.size() && !(text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
                                    text[i] == '\r' || text[i] == '\v' || text[i] == '\f')) {
            ++i;
        }
        if (i > start) {
            out.emplace_back(text.substr(start, i - start));
        }
    }
}

// ---------------------------------------------------------------------------
// Session helpers
// ---------------------------------------------------------------------------

// The /sessions table row usage cell.
kimix::string clicmd_usage_cell(double ratio, int64_t tokens, bool known) {
    if (!known) {
        return kimix::string("-");
    }
    return kimix::format("{:.1f}% ({} tokens)", ratio * 100.0, tokens);
}

// Switch to a named session (the /sessions:<name> and /resume recovery paths).
// (removed: every switch path inlines its own error text, like the reference)

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

command_result clicmd_help(const kimix::vector<kimix::string> &, app_context &,
                           kimix::vector<kimix::string> &) {
    print_string(cli_help_text_extended(colorful()));
    return {};
}

command_result clicmd_clear(const kimix::vector<kimix::string> &, app_context &app,
                            kimix::vector<kimix::string> &) {
    // clear_default_context(): nothing to clear below the 1e-8 usage epsilon -
    // it just re-prints the usage line.
    double ratio = 0.0;
    int64_t tokens = 0;
    app_usage(app, ratio, tokens);
    if (tokens == 0 || ratio <= 1e-8) {
        print_success("Context usage: " + app_usage_text(app));
        return {};
    }
    kimix::string error;
    app.soul.reset();
    app.session.reset();
    kimix::string close_error;
    app.store.close(/*delete_if_anonymous=*/false, close_error);
    if (!app.store.clear_context(error)) {
        print_error(error);
        kimix::string recovery;
        app_rebind_session(app, recovery);
        return {};
    }
    app.state = session_state{};
    app.title_locked = false;
    if (!app_rebind_session(app, error)) {
        print_error(error);
        return {};
    }
    print_success("Context usage: " + app_usage_text(app));
    return {};
}

command_result clicmd_compact(const kimix::vector<kimix::string> &, app_context &app,
                              kimix::vector<kimix::string> &) {
    // compact_default_context(): a no-op without live context usage.
    double ratio = 0.0;
    int64_t tokens = 0;
    app_usage(app, ratio, tokens);
    if (tokens == 0 || ratio <= 1e-8) {
        return {};
    }
    print_debug("Start compacting...");
    app_compact(app, "");
    return {};
}

command_result clicmd_context(const kimix::vector<kimix::string> &, app_context &app,
                              kimix::vector<kimix::string> &) {
    print_success("Context usage: " + app_usage_text(app));
    return {};
}

command_result clicmd_exit(const kimix::vector<kimix::string> &, app_context &app,
                           kimix::vector<kimix::string> &) {
    kimix::string error;
    if (!app_save_session(app, error)) {
        print_error(error);
    }
    app.soul.reset();
    app.session.reset();
    kimix::string close_error;
    app.store.close(/*delete_if_anonymous=*/true, close_error);
    app.session_closed = true;
    print_success("bye!");
    command_result result;
    result.should_break = true;
    return result;
}

command_result clicmd_cmd(const kimix::vector<kimix::string> &args, app_context &,
                          kimix::vector<kimix::string> &) {
    if (args.size() < 2) {
        print_error("Command must be /cmd:xx yy");
        return {};
    }
    const kimix::string command = clicmd_payload(args);
    const clicmd_process_result result = clicmd_run_shell(command);
    if (!result.output.empty()) {
        print_raw(result.output);
    }
    if (!result.started) {
        print_error(result.spawn_error.empty() ? kimix::string("command failed to start")
                                               : result.spawn_error);
        return {};
    }
    if (result.exit_code == 0) {
        print_success("Done.");
    } else {
        print_warning("Failed.");
    }
    return {};
}

command_result clicmd_fix(const kimix::vector<kimix::string> &args, app_context &app,
                          kimix::vector<kimix::string> &) {
    const kimix::string command = kimix::string(trim(clicmd_payload(args)));
    if (args.size() < 2 || command.empty()) {
        print_error("Command must be /fix:<command>");
        return {};
    }
    // fix_error(command, session, max_loop=4): run, then ask the agent to fix.
    for (int32_t attempt = 0; attempt < 4; ++attempt) {
        print_info("Shell: " + command);
        const clicmd_process_result result = clicmd_run_shell(command);
        const bool succeeded = result.started && result.exit_code == 0;
        if (succeeded) {
            if (attempt == 0) {
                print_success("No error.");
            }
            return {};
        }
        const kimix::string error_text = clicmd_filter_error_output(result.output, succeeded);
        const kimix::string prompt =
            "Fix error from command `" + command + "`:\n\n" + error_text + "\n";
        app_run_prompt(app, prompt);
    }
    return {};
}

command_result clicmd_txt(const kimix::vector<kimix::string> &, app_context &app,
                          kimix::vector<kimix::string> &text_arr) {
    print_string(clicmd_multi_line_banner("Start input multiple-lines"));
    kimix::vector<kimix::string> lines;
    bool cancelled = false;
    clicmd_read_multi_line(app, /*allow_cancel=*/true, lines, cancelled);
    // /txt ignores the cancel flag: nothing is queued.
    if (cancelled) {
        return {};
    }
    for (const kimix::string &block : split_text_blocks(lines)) {
        text_arr.push_back(block);
    }
    return {};
}

command_result clicmd_file(const kimix::vector<kimix::string> &args, app_context &app,
                           kimix::vector<kimix::string> &) {
    if (args.size() < 2) {
        print_error("command format error, must be /file:path");
        return {};
    }
    kimix::string path = clicmd_payload(args);
    const kimix::string resolved = clicmd_resolve(app, path);
    if (!file_exists(resolved)) {
        print_error("file not found: " + path);
        return {};
    }
    kimix::string content;
    kimix::string error;
    if (!read_file(resolved, content, error)) {
        print_error(error);
        return {};
    }
    command_result result;
    result.has_input = true;
    result.next_input = std::move(content);
    return result;
}

command_result clicmd_export(const kimix::vector<kimix::string> &args, app_context &app,
                             kimix::vector<kimix::string> &) {
    if (app.session == nullptr) {
        print_error("No active session to export.");
        return {};
    }
    if (args.size() < 2) {
        print_error("Command must be /export:file");
        return {};
    }
    const kimix::string path = clicmd_payload(args);
    kimix::string error;
    const size_t count = app.session->history().size();
    if (!app.store.export_markdown(app.session->history(), path, error)) {
        print_error("Export failed: " + error);
        return {};
    }
    print_success(kimix::format("Exported {} messages to {}", count, path));
    return {};
}

command_result clicmd_resume(const kimix::vector<kimix::string> &args, app_context &app,
                             kimix::vector<kimix::string> &) {
    if (args.size() < 2) {
        print_error("Command must be /resume:session_id");
        return {};
    }
    const kimix::string id = clicmd_payload(args);
    if (!dir_exists(session_store::session_dir(app.work_dir, id))) {
        print_debug("Session " + id + " not found.");
    }
    kimix::string error;
    if (!app_open_session(app, id, /*resume=*/true, error)) {
        print_error("Failed to resume session: " + error);
        return {};
    }
    print_success("Resumed session " + id);
    return {};
}

command_result clicmd_store(const kimix::vector<kimix::string> &args, app_context &app,
                            kimix::vector<kimix::string> &) {
    if (args.size() < 2) {
        print_error("Command must be /store:session_id");
        return {};
    }
    const kimix::string target = clicmd_payload(args);
    if (app.session == nullptr) {
        print_error("No active session to store.");
        return {};
    }
    if (target == app.store.id()) {
        print_error("Target session name must be different from current session name.");
        return {};
    }
    kimix::string error;
    if (!app.store.store_as(target, error)) {
        print_error("Store failed: " + error);
        return {};
    }
    print_success("Session stored as " + target);
    return {};
}

command_result clicmd_load(const kimix::vector<kimix::string> &args, app_context &app,
                           kimix::vector<kimix::string> &) {
    if (args.size() < 2) {
        print_error("Command must be /load:session_id");
        return {};
    }
    const kimix::string source = clicmd_payload(args);
    // Confirm replacing a session that has used context tokens.
    double ratio = 0.0;
    int64_t tokens = 0;
    app_usage(app, ratio, tokens);
    if (tokens > 0) {
        print_warning(kimix::format(
            "Current session \"{}\" has {} context tokens. Loading will release it. "
            "Continue? (y/n)",
            app.store.id(), tokens));
        kimix::string answer;
        for (;;) {
            if (!app_read_input(app, "", answer)) {
                // EOF at the confirmation: the reference raises EOFError here
                // (the outer handler aborts with a traceback); the native CLI
                // treats it as "no" and cancels the load (documented).
                print_info("Load cancelled.");
                return {};
            }
            const kimix::string lowered = to_lower_ascii(trim(answer));
            if (lowered == "y" || lowered == "n") {
                if (lowered != "y") {
                    print_info("Load cancelled.");
                    return {};
                }
                break;
            }
            print_warning("Please enter y or n.");
        }
    }
    kimix::string error;
    if (!app_save_session(app, error)) {
        error.clear();
    }
    app.soul.reset();
    app.session.reset();
    kimix::string close_error;
    app.store.close(/*delete_if_anonymous=*/false, close_error);
    if (!app.store.open(app.work_dir, "", /*resume=*/false, error)) {
        print_error("Load failed: " + error);
        return {};
    }
    if (!app.store.copy_into(source, error)) {
        print_error("Load failed: " + error);
        kimix::string recovery;
        app_rebind_session(app, recovery);
        return {};
    }
    app.state = session_state{};
    if (!app.store.load_state(app.state, error)) {
        print_error("Load failed: " + error);
        return {};
    }
    app.title_locked = !app.state.custom_title.empty();
    if (!app_rebind_session(app, error)) {
        print_error("Loaded session but failed to resume copy: " + error);
        return {};
    }
    print_success("Loaded session " + source + " into anonymous session " + app.store.id());
    return {};
}

command_result clicmd_sessions(const kimix::vector<kimix::string> &args, app_context &app,
                               kimix::vector<kimix::string> &) {
    if (args.size() >= 2) {
        // /sessions:<name> - create a new named session and switch to it.
        const kimix::string name = clicmd_payload(args);
        kimix::string error;
        if (!app_save_session(app, error)) {
            error.clear();
        }
        app.soul.reset();
        app.session.reset();
        kimix::string close_error;
        app.store.close(/*delete_if_anonymous=*/false, close_error);
        if (!app.store.open(app.work_dir, name, /*resume=*/false, error)) {
            print_error(kimix::format("Failed to create session \"{}\": {}", name, error));
            kimix::string recovery;
            app_open_session(app, "", false, recovery);
            return {};
        }
        app.state = session_state{};
        app.title_locked = false;
        if (!app_rebind_session(app, error)) {
            print_error(kimix::format("Failed to create session \"{}\": {}", name, error));
            return {};
        }
        print_success("Created and switched to session: " + name);
        return {};
    }
    // /sessions - the resumable-session table (filesystem scan; the reference
    // reads its in-memory cache).
    kimix::vector<session_info> rows = session_store::list(app.work_dir);
    if (rows.empty()) {
        print_warning("No sessions found.");
        return {};
    }
    double ratio = 0.0;
    int64_t tokens = 0;
    app_usage(app, ratio, tokens);
    for (session_info &row : rows) {
        if (row.id == app.store.id()) {
            row.context_usage = ratio;
            row.context_tokens = tokens;
            row.usage_known = tokens > 0;
        }
    }
    size_t id_width = kimix::string("session id").size();
    for (const session_info &row : rows) {
        id_width = row.id.size() > id_width ? row.id.size() : id_width;
    }
    print_info("   " + clicmd_pad("session id", id_width) + "  " +
               clicmd_pad("updated at", 19) + "  " + clicmd_pad("context usage", 22) +
               "  title");
    for (const session_info &row : rows) {
        const kimix::string marker = (row.id == app.store.id()) ? "*" : " ";
        const kimix::string usage =
            clicmd_usage_cell(row.context_usage, row.context_tokens, row.usage_known);
        print_string(marker + "  " + clicmd_pad(row.id, id_width) + "  " +
                     format_utc(row.updated_at) + "  " + clicmd_pad(usage, 22) + "  " +
                     (row.title.empty() ? kimix::string("Untitled") : row.title));
    }
    return {};
}

command_result clicmd_init(const kimix::vector<kimix::string> &, app_context &app,
                           kimix::vector<kimix::string> &) {
    // Reduced /init (documented): the interactive wizard is not ported.  A
    // default_config.json template is written into the working directory (the
    // reference writes <repo>/src/kimix/default_config.json) and the flags are
    // explained, then a fresh anonymous session picks the settings up.
    const kimix::string cwd_template = join_path(app.work_dir, "default_config.json");
    const kimix::string target =
        file_exists(cwd_template) ? join_path(app.work_dir, "default_config.template.json")
                                 : cwd_template;
    kimix::string error;
    if (!write_file(target, k_clicmd_default_config_template, error)) {
        print_error(error);
        return {};
    }
    print_info("Wrote a default provider config template to " + target);
    print_info("Set \"model\", \"url\" and \"api_key\", then start the CLI with "
               "--provider " + target);
    print_info("Flags: --no_think (suppress reasoning), --no_yolo (disable automatic "
               "approval), --no_color (plain output), -s/--skill-dir DIR (extra skill "
               "directories), -c/--clean (delete this session's cache on exit)");
    kimix::string open_error;
    if (!app_open_session(app, "", /*resume=*/false, open_error)) {
        print_error(open_error);
        return {};
    }
    print_success("Initialized.");
    return {};
}

command_result clicmd_todo(const kimix::vector<kimix::string> &args, app_context &app,
                           kimix::vector<kimix::string> &) {
    if (args.size() < 2) {
        print_error("Command must be /todo:<path>");
        return {};
    }
    const kimix::string given = clicmd_payload(args);
    const kimix::string path = clicmd_resolve(app, given);
    if (!file_exists(path)) {
        print_error("file not found: " + given);
        return {};
    }
    const kimix::string suffix = to_lower_ascii(extension(path));
    const clicmd_lang lang = clicmd_lang_for_suffix(suffix);
    if (lang == clicmd_lang::unsupported) {
        print_error("Unsupported file type: " + suffix);
        return {};
    }
    kimix::string text;
    kimix::string error;
    if (!read_file(path, text, error)) {
        print_error("Parse failed: " + error);
        return {};
    }
    kimix::vector<clicmd_comment> comments;
    clicmd_scan_comments(lang, text, comments);
    kimix::vector<clicmd_comment> todos;
    for (const clicmd_comment &comment : comments) {
        if (clicmd_has_todo(comment.content)) {
            todos.push_back(comment);
        }
    }
    if (todos.empty()) {
        print_warning("No TODO comments found.");
        return {};
    }
    kimix::string prompt_str;
    if (todos.size() == 1) {
        const kimix::string item =
            kimix::format("Line {}: {}", todos[0].line,
                          kimix::string(trim(todos[0].content)));
        prompt_str = "Implement the TODO in " + given + ":\n" + item +
                     "\n\nRemove TODO comment after done.";
    } else {
        kimix::string items;
        for (size_t i = 0; i < todos.size(); ++i) {
            if (i > 0) {
                items += "\n";
            }
            items += kimix::format("{}. Line {}: {}", i + 1, todos[i].line,
                                   kimix::string(trim(todos[i].content)));
        }
        prompt_str = "Implement all TODOs in " + given + " at once:\n\n" + items +
                     "\n\nMake sure to handle each TODO completely."
                     "\n\nRemove TODO comment after done.";
    }
    print_info(prompt_str);
    if (!app_run_prompt(app, prompt_str)) {
        print_error("Prompt failed.");
    }
    return {};
}

command_result clicmd_plan(const kimix::vector<kimix::string> &args, app_context &app,
                           kimix::vector<kimix::string> &) {
    kimix::string path;
    if (args.size() >= 2) {
        path = kimix::string(trim(clicmd_payload(args)));
    } else {
        // secrets.token_hex(8): 16 hex characters under <work_dir>/.kimix_cache.
        path = join_path(join_path(app.work_dir, ".kimix_cache"),
                         "plan_" + random_hex(8) + ".md");
    }
    print_string(clicmd_multi_line_banner("Start input requirement for plan"));
    kimix::vector<kimix::string> lines;
    bool cancelled = false;
    clicmd_read_multi_line(app, /*allow_cancel=*/true, lines, cancelled);
    const kimix::string requirement =
        kimix::string(trim(clicmd_join_lines(lines, "\n")));
    if (requirement.empty()) {
        print_warning("No requirement provided.");
        return {};
    }
    // Reduced /plan (documented): one generation attempt on the current session
    // with the plan file wired into the session (plan_writing_path); the
    // planner sub-session, the retry nudge and the y/n review loop are not
    // ported.
    print_debug("Generating plan (attempt 1/3)...");
    if (app.session != nullptr) {
        app.session->tool_session().plan_path = path;
        app.session->tool_session().plan_enabled = true;
    }
    const kimix::string prompt =
        "Write a complete, step-by-step implementation plan for the requirement below to "
        "the plan file.\n\nPlan file: `" + path + "`\n\nRequirement:\n" + requirement +
        "\n\nUse the plan tools (WritePlan/ReadPlan/EditPlan) to write the plan file. "
        "After the plan is written, end your turn.";
    if (!app_run_prompt(app, prompt)) {
        print_error("prompt_plan failed.");
    }
    return {};
}

command_result clicmd_swarm(const kimix::vector<kimix::string> &, app_context &app,
                            kimix::vector<kimix::string> &) {
    print_string(clicmd_multi_line_banner("Start input for swarm"));
    kimix::vector<kimix::string> lines;
    bool cancelled = false;
    clicmd_read_multi_line(app, /*allow_cancel=*/true, lines, cancelled);
    if (cancelled) {
        return {};
    }
    const kimix::string task = kimix::string(trim(clicmd_join_lines(lines, "\n")));
    if (task.empty()) {
        print_warning("No input provided for swarm.");
        return {};
    }
    print_debug("Creating swarm session...");
    // Reduced /swarm (documented): an isolated anonymous session with
    // swarm_enabled (the reference's custom_data['is_swarm_session']), one turn,
    // then the temporary session is closed.
    kimix::string error;
    if (!app_run_isolated(app, app.agent, /*swarm_enabled=*/true, task, error)) {
        print_error("Swarm prompt failed: " + error);
    }
    return {};
}

command_result clicmd_supervisor(const kimix::vector<kimix::string> &, app_context &app,
                                 kimix::vector<kimix::string> &) {
    print_string(clicmd_multi_line_banner("Start input for supervisor"));
    kimix::vector<kimix::string> lines;
    bool cancelled = false;
    clicmd_read_multi_line(app, /*allow_cancel=*/true, lines, cancelled);
    const kimix::string task = kimix::string(trim(clicmd_join_lines(lines, "\n")));
    if (task.empty()) {
        print_warning("No input provided for supervisor.");
        return {};
    }
    print_debug("Creating supervisor session...");
    // Reduced /supervisor (documented): an isolated anonymous session driven by
    // the boss manifest when it is available next to the worker manifest, one
    // turn, then the temporary session is closed.
    agent_config boss = app.agent;
    const kimix::string boss_path =
        app.agent.manifest_dir.empty() ? kimix::string()
                                       : join_path(app.agent.manifest_dir, "agent_boss.json");
    if (!boss_path.empty() && file_exists(boss_path)) {
        kimix::string load_error;
        agent_config loaded;
        if (load_agent_config(boss_path, loaded, load_error)) {
            boss = std::move(loaded);
        } else {
            print_warning("Falling back to the current agent manifest: " + load_error);
        }
    } else {
        print_warning("agent_boss.json not found next to the current manifest; using the "
                      "current agent manifest");
    }
    kimix::string error;
    if (!app_run_isolated(app, boss, /*swarm_enabled=*/false, task, error)) {
        print_error("Supervisor prompt failed: " + error);
    }
    return {};
}

command_result clicmd_reflection(const kimix::vector<kimix::string> &, app_context &app,
                                 kimix::vector<kimix::string> &) {
    if (app.session == nullptr) {
        print_error("No active session. Start a conversation first.");
        return {};
    }
    double ratio = 0.0;
    int64_t tokens = 0;
    app_usage(app, ratio, tokens);
    if (tokens == 0) {
        print_error("Context is empty. /reflection requires a non-empty context.");
        return {};
    }
    // Reduced /reflection (documented): the reference's Python repo
    // introspection (importlib + inspect.getfile over the tool manifest) is
    // replaced by a native path/tool listing.
    kimix::string tools;
    for (const kimix::string &name : app.agent.enabled_tools) {
        tools += "- `" + name + "`\n";
    }
    const kimix::string report_path =
        join_path(app.work_dir,
                  "docs/reflection_report_" + format_utc(now_unix_seconds(), "%Y%m%d_%H%M%S") +
                      ".md");
    const kimix::string prompt_str =
        kimix::format("# Reflection Task\n\n"
                      "Reflect on the conversation context above. Find misunderstandings "
                      "caused by the current agent design, then change the source code to "
                      "make this project better.\n\n"
                      "## Context\n"
                      "- Current context: {} messages, {} tokens (full conversation is "
                      "visible above)\n"
                      "- Working directory: `{}`\n\n"
                      "## Builtin tools (this session)\n{}\n"
                      "## Architecture map (source of truth)\n"
                      "- Native CLI: `src/cli/cli_app.{{h,cpp}}`, "
                      "`src/cli/cli_commands.{{h,cpp}}`, `src/cli/cli_repl.{{h,cpp}}`\n"
                      "- Agent soul: `src/agent/soul.{{h,cpp}}`\n"
                      "- Built-in tools: `src/builtin_tools/*.{{h,cpp}}` "
                      "(registry: src/builtin_tools/tool_registry.h)\n"
                      "- Provider config: `src/cli/cli_config.{{h,cpp}}`\n\n"
                      "## Testing rules\n"
                      "- Build with: python scripts/build_locked.py -- xmake build test_cli\n"
                      "- Run: ./bin/debug/test_cli.exe\n\n"
                      "## Change code rules\n"
                      "- Exception-free (throw/try/catch are build errors), no RTTI, "
                      "kimix:: containers in public APIs, K&R braces, 4-space indent.\n"
                      "- Never run a git write command in this repository.\n\n"
                      "## Report\n"
                      "After finishing all changes, write a report introducing the changes "
                      "to: `{}`\n",
                      app.session->history().size(), tokens, app.work_dir, tools,
                      report_path);
    print_info(prompt_str);
    if (!app_run_prompt(app, prompt_str)) {
        print_error("Reflection failed.");
    }
    return {};
}

command_result clicmd_code(const kimix::vector<kimix::string> &args, app_context &app,
                           kimix::vector<kimix::string> &) {
    if (args.size() < 2) {
        print_error("Command must be /code:<script_path> [args...]");
        return {};
    }
    kimix::vector<kimix::string> parts;
    clicmd_split_ws(clicmd_payload(args), parts);
    if (parts.empty()) {
        print_error("Script path is required.");
        return {};
    }
    kimix::string script = parts[0];
    if (!file_exists(script)) {
        const kimix::string resolved = clicmd_resolve(app, script);
        if (file_exists(resolved)) {
            script = resolved;
        } else {
            print_error("Script file not found: " + parts[0]);
            return {};
        }
    }
    kimix::vector<kimix::string> argv;
    if (to_lower_ascii(extension(script)) == ".py") {
        // No embedded Python interpreter (documented): the reference's
        // `exec(src, exec_ctx)` is replaced by spawning `python <script> args...`.
        const kimix::string name = file_name(script);
        print_info("Executing " + name);
        print_raw("\n");
        argv.push_back(kimix::string("python"));
        argv.push_back(script);
        for (size_t i = 1; i < parts.size(); ++i) {
            argv.push_back(parts[i]);
        }
    } else {
        print_info("Running: " + clicmd_join_lines(parts, " "));
        argv.push_back(script);
        for (size_t i = 1; i < parts.size(); ++i) {
            argv.push_back(parts[i]);
        }
    }
    const clicmd_process_result result = clicmd_run_argv(argv);
    if (!result.started) {
        print_error("Executable not found: " + script);
        return {};
    }
    if (!result.output.empty()) {
        print_raw(result.output);
    }
    if (result.exit_code == 0) {
        print_success(to_lower_ascii(extension(script)) == ".py"
                          ? kimix::string("Done.")
                          : kimix::string("Done (exit code 0)."));
    } else if (to_lower_ascii(extension(script)) == ".py") {
        print_warning(kimix::format("Exited with code {}.", result.exit_code));
    } else {
        print_warning(kimix::format("Exited with code {}.", result.exit_code));
    }
    return {};
}

command_result clicmd_unknown(const kimix::vector<kimix::string> &, app_context &,
                              kimix::vector<kimix::string> &) {
    print_warning("Unrecognized command.");
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// _split_text / the command table
// ---------------------------------------------------------------------------

kimix::vector<kimix::string> split_text_blocks(const kimix::vector<kimix::string> &lines) {
    kimix::vector<kimix::string> blocks;
    kimix::vector<kimix::string> current;
    for (const kimix::string &line : lines) {
        const kimix::string_view strip_line = trim(line);
        if (strip_line.empty()) {
            current.push_back(kimix::string());
            continue;
        }
        if (strip_line[0] == '/') {
            bool known = true;
            if (strip_line.size() > 1) {
                kimix::vector<kimix::string> words;
                clicmd_split_ws(strip_line.substr(1), words);
                if (words.empty() || find_command(words[0]) == nullptr) {
                    known = false;
                }
            }
            if (!known) {
                current.push_back(line);
                continue;
            }
            if (!current.empty()) {
                blocks.push_back(clicmd_join_lines(current, "\n"));
                current.clear();
            }
            if (strip_line.size() > 1) {
                blocks.emplace_back(strip_line);
            }
            continue;
        }
        current.push_back(line);
    }
    if (!current.empty()) {
        blocks.push_back(clicmd_join_lines(current, "\n"));
    }
    return blocks;
}

const kimix::vector<command_entry> &command_map() {
    static const kimix::vector<command_entry> entries = [] {
        kimix::vector<command_entry> table;
        auto add = [&table](const char *name, const char *help,
                            command_result (*handler)(const kimix::vector<kimix::string> &,
                                                      app_context &,
                                                      kimix::vector<kimix::string> &)) {
            command_entry entry;
            entry.name = name;
            entry.help = help;
            entry.handler = handler;
            table.push_back(std::move(entry));
        };
        // The reference's _command_map, in its insertion order.
        add("help", "Show this help message", &clicmd_help);
        add("clear", "Clear the conversation context", &clicmd_clear);
        add("exit", "Exit the program", &clicmd_exit);
        add("context", "Print context usage", &clicmd_context);
        add("cmd", "Execute system command", &clicmd_cmd);
        add("fix", "Run a command and fix errors if any", &clicmd_fix);
        add("txt", "Input multiple line text", &clicmd_txt);
        add("file", "Load a file and execute its content line by line", &clicmd_file);
        add("plan", "Plan a long-term task, step-by-step, then execute", &clicmd_plan);
        add("compact", "Compact conversation context", &clicmd_compact);
        add("export", "Export session messages to file", &clicmd_export);
        add("resume", "Close current session and resume a session by ID", &clicmd_resume);
        add("store", "Copy the current session to a new named session", &clicmd_store);
        add("load", "Copy a named session into a new anonymous session", &clicmd_load);
        add("sessions", "List resumable sessions for the current working directory",
            &clicmd_sessions);
        add("reflection", "Reflect on current context and refactor agent source code",
            &clicmd_reflection);
        add("supervisor", "Start a supervisor session with multi-line input text",
            &clicmd_supervisor);
        add("swarm", "Start a swarm session with multi-line input text", &clicmd_swarm);
        add("init", "Initialize default LLM config", &clicmd_init);
        add("todo", "Scan code file for TODO comments and prompt agent to implement them",
            &clicmd_todo);
        add("code", "Run a script file with optional arguments", &clicmd_code);
        // The _cmd_unknown fallback (not a user-visible command).
        add("unknown", "Fallback for an unrecognised command", &clicmd_unknown);
        return table;
    }();
    return entries;
}

const command_entry *find_command(kimix::string_view name) {
    for (const command_entry &entry : command_map()) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace kimix::cli
