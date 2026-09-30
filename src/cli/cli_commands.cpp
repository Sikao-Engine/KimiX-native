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
#include "builtin_tools/python_code_session.h"

#include "cli/cli_common.h"
#include "cli/cli_init_wizard.h"
#include "cli/cli_print.h"
#include "cli/cli_stream.h"

#if defined(KIMIX_PLATFORM_WINDOWS)
#include <windows.h>
#endif

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

  // The reference's _context_is_non_empty (commands.py:733-752), the 3-tier
  // empty-context check /clear and /reflection share.  A previous guard keyed
  // on estimated_tokens(), which carries the per-request overhead (system
  // prompt + tool schemas) and is therefore never zero - the fast path was
  // unreachable (tests/unit/cli/test_cli.cpp pins that).  Tiers:
  //   1. recorded provider usage (status.context_usage > 1e-8)
  //   2. the live context (the session history - a recorded token count > 0
  //      implies at least one message)
  //   3. the persisted context records (context.jsonl / context.db non-empty,
  //      the reference's Session.is_empty() wire/db/jsonl tier)
  bool clicmd_context_is_non_empty(const app_context &app) {
      double ratio = 0.0;
      int64_t tokens = 0;
      bool known = false;
      app.store.usage(ratio, tokens, known);
      if (known && ratio > 1e-8) {
          return true;
      }
      if (app.session != nullptr && !app.session->history().empty()) {
          return true;
      }
      return app.store.has_context_records();
  }

// [A-Za-z0-9] (the TODO word-boundary predicate is ASCII in the reference's
// character class).
bool clicmd_is_alnum(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9');
}

// ---------------------------------------------------------------------------
// I2 helpers: the directory listing / import source checks
// ---------------------------------------------------------------------------

// kimi_cli list_directory equivalent (a reduced renderer: one entry per line,
// directories suffixed with "/", sorted with dirs first then names).
bool clicmd_list_directory(const kimix::string &path, kimix::string &out,
                           kimix::string &error) {
    kimix::filesystem::path dir;
    if (!kimix::path_from_narrow(path, dir)) {
        error = "invalid path: " + path;
        return false;
    }
    std::error_code ec;
    if (!kimix::filesystem::is_directory(dir, ec) || ec) {
        error = "not a directory: " + path;
        return false;
    }
    kimix::vector<kimix::string> names;
    for (kimix::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        names.push_back(kimix::to_string(it->path().filename()) +
                        (it->is_directory(ec) && !ec ? kimix::string("/") : kimix::string()));
    }
    if (ec) {
        error = "cannot list directory: " + path;
        return false;
    }
    // Stable, case-insensitive ordering so the listing is deterministic.
    kimix::vector<kimix::string> lowered;
    lowered.reserve(names.size());
    for (const kimix::string &n : names) {
        lowered.push_back(to_lower_ascii(n));
    }
    for (size_t i = 0; i + 1 < names.size(); ++i) {
        for (size_t j = i + 1; j < names.size(); ++j) {
            if (lowered[j] < lowered[i]) {
                kimix::string tmp_name = names[i];
                names[i] = names[j];
                names[j] = tmp_name;
                kimix::string tmp_low = lowered[i];
                lowered[i] = lowered[j];
                lowered[j] = tmp_low;
            }
        }
    }
    out = join(names, "\n");
    return true;
}

// export.py:367-450 _IMPORTABLE_EXTENSIONS (text-based formats).
bool clicmd_is_importable_suffix(kimix::string_view suffix) {
    static const char *const kExtensions[] = {
        ".md", ".markdown", ".txt", ".text", ".rst",
        ".json", ".jsonl", ".yaml", ".yml", ".toml", ".ini", ".cfg", ".conf",
        ".csv", ".tsv", ".xml", ".env", ".properties",
        ".py", ".js", ".ts", ".jsx", ".tsx", ".java", ".kt", ".go", ".rs",
        ".c", ".cpp", ".h", ".hpp", ".cs", ".rb", ".php", ".swift", ".scala",
        ".sh", ".bash", ".zsh", ".fish", ".ps1", ".bat", ".cmd", ".r", ".lua",
        ".pl", ".pm", ".ex", ".exs", ".erl", ".hs", ".ml", ".sql", ".graphql",
        ".proto", ".html", ".htm", ".css", ".scss", ".sass", ".less", ".svg",
        ".log", ".tex", ".bib", ".org", ".adoc", ".wiki",
    };
    const kimix::string lowered = to_lower_ascii(suffix);
    for (const char *ext : kExtensions) {
        if (lowered == ext) {
            return true;
        }
    }
    return false;
}

// export.py:589-605 _SENSITIVE_FILE_PATTERNS / is_sensitive_file.
bool clicmd_is_sensitive_file(kimix::string_view filename) {
    static const char *const kPatterns[] = {
        ".env", "credentials", "secrets", ".pem", ".key", ".p12", ".pfx",
        ".keystore",
    };
    const kimix::string lowered = to_lower_ascii(filename);
    for (const char *pattern : kPatterns) {
        if (contains(lowered, pattern)) {
            return true;
        }
    }
    return false;
}

// export.py MAX_IMPORT_SIZE.
constexpr int64_t k_clicmd_max_import_size = 10 * 1024 * 1024;

// _stringify_message (a reduced stringify_context_history: "role: text" per
// message; tool calls render as their names + arguments).
kimix::string clicmd_stringify_history(const kimix::vector<kimix::llm::Message> &history) {
    kimix::string out;
    for (const kimix::llm::Message &msg : history) {
        if (msg.role == "tool") {
            out += "tool result: " + msg.content + "\n";
            continue;
        }
        out += msg.role + ": " + msg.content + "\n";
        for (const kimix::llm::ToolCall &call : msg.tool_calls) {
            out += "  tool call " + call.name + "(" + call.arguments + ")\n";
        }
    }
    return out;
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

// Python Path.is_absolute() over a possibly-relative user string.
bool clicmd_is_absolute(kimix::string_view path) {
    const kimix::filesystem::path p{kimix::string(path)};
    return p.is_absolute();
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
    // Nothing to clear: the 3-tier empty-context check (recorded usage /
    // live history / persisted context records) - see
    // clicmd_context_is_non_empty.  A fresh session takes this fast path; a
    // session that carried any conversation falls through to the full reset.
    if (!clicmd_context_is_non_empty(app)) {
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
      // slash.py cmd_compact: a no-op on an empty history ("The context is
      // empty."). The previous usage-epsilon guard keyed on estimated_tokens()
      // no longer works now that the estimate carries the per-request overhead
      // (system prompt + tool schemas) and is never zero.
      if (app.soul == nullptr || app.session == nullptr ||
          app.session->history().empty()) {
          print_string("The context is empty.");
          return {};
      }
      print_debug("Start compacting...");
      app_compact(app, "");
      return {};
  }

  command_result clicmd_prune(const kimix::vector<kimix::string> &, app_context &app,
                              kimix::vector<kimix::string> &) {
      // soul/slash.py cmd_prune (140-175): manually trigger one context-prune
      // pass (smart history removal). Non-destructive - the pruned list is a
      // preview; only the auto pass inside a turn applies stubs to requests.
      if (app.soul == nullptr || app.session == nullptr) {
          print_string("The context is empty.");
          return {};
      }
      if (!app.soul->loop_control().context_pruning_enabled) {
          print_string("Context pruning is disabled in config.");
          return {};
      }
      const kimix::vector<kimix::llm::Message> history = app.session->history();
      const int64_t mctx = app.soul->max_context_size();
      kimix::agent::prune_call pc;
      pc.current_step = app.soul->current_step_no();
      pc.context_usage = mctx > 0
                             ? static_cast<double>(app.soul->estimated_tokens()) /
                                   static_cast<double>(mctx)
                             : 0.0;
      pc.max_context_size = mctx;
      pc.current_turn_index = kimix::agent::current_turn_start_index(history);
      kimix::agent::pruning_result result = app.soul->pruner().prune(history, pc);
      if (!result.earliest_removed_index.has_value()) {
          print_string("No prunable content found.");
          return {};
      }
      kimix::string line = "Context pruned: freed ";
      line += std::to_string(static_cast<long long>(result.freed_tokens)).c_str();
      line += " tokens, earliest change at index ";
      line += std::to_string(static_cast<long long>(*result.earliest_removed_index))
                  .c_str();
      line += ".";
      print_string(line);
      // slash.py refreshes the context meter after a manual prune.
      print_success("Context usage: " + app_usage_text(app));
      return {};
  }

command_result clicmd_context(const kimix::vector<kimix::string> &, app_context &app,
                              kimix::vector<kimix::string> &) {
    print_success("Context usage: " + app_usage_text(app));
    return {};
}

command_result clicmd_btw(const kimix::vector<kimix::string> &args, app_context &app,
                          kimix::vector<kimix::string> &) {
    // G11 /btw (btw.py): a side question over the same system prompt +
    // normalized history; the answer streams to the terminal and never
    // touches the main history. The reference TUI intercepts "/btw <text>" at
    // the input classifier and collects it until the turn ends; the native
    // REPL is synchronous (commands run between turns), so the question runs
    // immediately. Both spellings work: "/btw:question" (the REPL's colon
    // split) and "/btw question" (a direct handler call).
    kimix::string question(trim(clicmd_payload(args)));
    if (question.empty() && args.size() >= 1 && args[0].size() > 3) {
        const kimix::string_view rest = trim(kimix::string_view(args[0]).substr(3));
        question.assign(rest.data(), rest.size());
    }
    if (question.empty()) {
        print_error("usage: /btw:<question>");
        return {};
    }
    if (app.soul == nullptr) {
        print_error("no session is open");
        return {};
    }
    // The streamed answer text goes straight to the terminal (the BtwEnd
    // record carries the full text for UI clients).
    const kimix::agent::SideQuestionResult result = app.soul->run_side_question(
        question, [](const kimix::llm::Chunk &chunk) { print_string(chunk.content); });
    if (!result.error.empty()) {
        print_error(result.error);
        return {};
    }
    print_string("");
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
    app.wire.reset(); // release wire.jsonl before the directory is deleted
    kimix::string close_error;
    app.store.close(/*delete_if_anonymous=*/true, close_error);
    app.session_closed = true;
    // H6 (commands.py:407-423 _cmd_exit): remove the shared tool temp folder
    // (.kimix_cache/tmp_<pid>) plus the leftovers of previously killed
    // processes before the goodbye.  Best effort; never blocks the exit.
    {
        kimix::string cleanup_error;
        if (!cleanup_temp_folder(app.work_dir, cleanup_error)) {
            print_debug(cleanup_error);
        }
    }
    // H11: the final cache bookkeeping row (the reference's final
    // _add_cli_session before close_session).
    app_touch_cli_session(app);
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
    // I2/I6: the soul-level /export accepts the whitespace form as well as the
    // REPL's colon form ("/export out.md" == "/export:out.md").
    kimix::string given;
    if (args.size() >= 2) {
        given = kimix::string(trim(clicmd_payload(args)));
    } else if (!args.empty() && args[0].size() > 6 &&
               starts_with(args[0], "export")) {
        // The whitespace form arrives as a single "export <args>" token (the
        // REPL only splits at a colon; see /btw's identical handling).
        given = kimix::string(trim(kimix::string_view(args[0]).substr(6)));
    }
    if (given.empty()) {
        print_error("Command must be /export:file");
        return {};
    }
    // _session.py:803-830: expanduser, relative -> work_dir, and a trailing
    // separator or an existing directory selects the directory form (the
    // default file name inside it).
    kimix::string target = expand_home(given);
    if (!clicmd_is_absolute(target)) {
        target = join_path(app.work_dir, target);
    }
    bool directory_form = ends_with(given, "/") || ends_with(given, "\\") ||
                          ends_with(given, "//");
    std::error_code ec;
    if (kimix::filesystem::is_directory(kimix::filesystem::path(kimix::string(target)), ec) &&
        !ec) {
        directory_form = true;
    }
    if (directory_form) {
        // kimi-export-<id8>-<YYYYMMDD-HHMMSS>.md (local time, like pendulum).
        const kimix::string stamp = format_local(now_unix_seconds(), "%Y%m%d-%H%M%S");
        kimix::string id8 = app.store.id();
        if (id8.size() > 8) {
            id8 = id8.substr(0, 8);
        }
        const kimix::string default_name =
            "kimi-export-" + id8 + "-" + stamp + ".md";
        target = join_path(target, default_name);
    }    const kimix::string resolved = absolute_path(target);
    kimix::string error;
    const size_t count = app.session->history().size();
    if (!app.store.export_markdown(app.session->history(), resolved, error)) {
        print_error("Export failed: " + error);
        return {};
    }
    // commands.py:93 echoes the RESOLVED path that was written.
    print_success(kimix::format("Exported {} messages to {}", count, resolved));
    // slash.py:343-347: the sensitive-information note (soul-level /export).
    print_string("  Note: The exported file may contain sensitive information. "
                 "Please be cautious when sharing it externally.");
    return {};
}

// ---------------------------------------------------------------------------
// G5 / I2: the soul-level slash commands (kimi_cli/soul/slash.py), exposed in
// the native CLI's table so both spellings the reference product knows work.
// The user-visible text is `wire_send(TextPart(...))` in the reference; in the
// CLI these are plain terminal lines.
// ---------------------------------------------------------------------------

// slash.py:198-219 cmd_yolo.
command_result clicmd_yolo(const kimix::vector<kimix::string> &, app_context &app,
                           kimix::vector<kimix::string> &) {
    if (app.approval == nullptr) {
        print_error("no approval gate is active");
        return {};
    }
    if (app.approval->is_yolo()) {
        app.approval->set_yolo(false);
        if (app.approval->is_afk()) {
            // Yolo off but afk still on -> tool calls remain auto-approved.
            print_string("Yolo disabled, but afk is still on \xe2\x80\x94 tool calls "
                         "remain auto-approved. Use /afk to turn off afk.");
        } else {
            print_string("You only die once! Actions will require approval.");
        }
    } else {
        app.approval->set_yolo(true);
        print_string("You only live once! All actions will be auto-approved.");
    }
    return {};
}

// slash.py:222-246 cmd_afk.
command_result clicmd_afk(const kimix::vector<kimix::string> &, app_context &app,
                          kimix::vector<kimix::string> &) {
    if (app.approval == nullptr) {
        print_error("no approval gate is active");
        return {};
    }
    if (app.approval->is_afk()) {
        app.approval->set_afk(false);
        if (app.approval->is_yolo()) {
            print_string("afk mode disabled. You are back at the terminal. "
                         "Yolo is still on.");
        } else {
            print_string("afk mode disabled. You are back at the terminal.");
        }
    } else {
        app.approval->set_afk(true);
        print_string("afk mode enabled. AskUserQuestion will be auto-dismissed "
                     "and tool calls auto-approved.");
    }
    return {};
}

// slash.py:249-320 cmd_add_dir.
command_result clicmd_add_dir(const kimix::vector<kimix::string> &args, app_context &app,
                              kimix::vector<kimix::string> &) {
    kimix::string given;
    if (args.size() >= 2) {
        given = kimix::string(trim(clicmd_payload(args)));
    } else if (!args.empty() && args[0].size() > 7 && starts_with(args[0], "add-dir")) {
        given = kimix::string(trim(kimix::string_view(args[0]).substr(7)));
    }
    if (given.empty()) {
        // List the added directories (slash.py:257-264).
        if (app.state.additional_dirs.empty()) {
            print_string("No additional directories. Usage: /add-dir <path>");
        } else {
            kimix::string out = "Additional directories:";
            for (const kimix::string &dir : app.state.additional_dirs) {
                out += "\n  - " + dir;
            }
            print_string(out);
        }
        return {};
    }
    kimix::string path = expand_home(given);
    path = clicmd_is_absolute(path) ? kimix::string(path)
                                    : join_path(app.work_dir, path);
    path = absolute_path(path);
    if (!dir_exists(path)) {
        print_string("Directory does not exist: " + path);
        return {};
    }
    // Redundancy checks, in the reference's order.
    for (const kimix::string &dir : app.state.additional_dirs) {
        if (dir == path) {
            print_string("Directory already in workspace: " + path);
            return {};
        }
    }
    if (is_within_directory(path, app.work_dir)) {
        print_string("Directory is already within the working directory: " + path);
        return {};
    }
    for (const kimix::string &existing : app.state.additional_dirs) {
        if (is_within_directory(path, existing)) {
            print_string("Directory is already within an added directory `" +
                         existing + "`: " + path);
            return {};
        }
    }
    // Validate readability before committing state (slash.py:296-301).
    kimix::string listing, list_error;
    if (!clicmd_list_directory(path, listing, list_error)) {
        print_string("Cannot read directory: " + path + " (" + list_error + ")");
        return {};
    }
    app.state.additional_dirs.push_back(path);
    // Persist to session state (slash.py:306-308) - the next save writes it.
    kimix::string save_error;
    app_save_session(app, save_error);
    // Inject the system message informing the LLM (slash.py:310-317).
    if (app.session != nullptr) {
        kimix::llm::Message message;
        message.role = "user";
        message.content =
            kimix::string("The user has added an additional directory to the workspace: `") +
            path + "`\n\nDirectory listing:\n```\n" + listing +
            "\n```\n\nYou can now read, write, search, and glob files in this "
            "directory as if it were part of the working directory.";
        app.session->history().push_back(std::move(message));
    }
    print_string("Added directory to workspace: " + path);
    return {};
}

// slash.py:350-361 cmd_refresh_env (kimi_cli/utils/environment.py
// refresh_windows_env): re-read PATH/PATHEXT from the registry into the
// process environment (HKLM first, then HKCU, deduplicated, REG_EXPAND_SZ
// values expanded by the registry read).
command_result clicmd_refresh_env(const kimix::vector<kimix::string> &, app_context &,
                                  kimix::vector<kimix::string> &) {
#if defined(KIMIX_PLATFORM_WINDOWS)
    auto read_registry = [](HKEY root, const char *subkey, const char *value,
                            kimix::string &out) {
        HKEY key = nullptr;
        if (RegOpenKeyExA(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
            return;
        }
        DWORD type = 0;
        DWORD size = 0;
        if (RegQueryValueExA(key, value, nullptr, &type, nullptr, &size) ==
                ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ) && size > 0) {
            kimix::string buffer(static_cast<size_t>(size), '\0');
            if (RegQueryValueExA(key, value, nullptr, &type,
                                 reinterpret_cast<LPBYTE>(buffer.data()), &size) ==
                ERROR_SUCCESS) {
                // REG_EXPAND_SZ: ExpandEnvironmentStringsA (the reference's
                // _expand_registry_string).
                if (type == REG_EXPAND_SZ &&
                    buffer.find('%') != kimix::string::npos) {
                    char expanded[4096] = {};
                    const DWORD n = ExpandEnvironmentStringsA(buffer.c_str(), expanded,
                                                              sizeof(expanded));
                    if (n > 0 && n < sizeof(expanded)) {
                        out.assign(expanded, n - 1);
                    } else {
                        out = buffer;
                    }
                } else {
                    out = buffer;
                }
                while (!out.empty() && out.back() == '\0') {
                    out.pop_back();
                }
            }
        }
        RegCloseKey(key);
    };
    auto merge_dedup = [](const kimix::string &sys_val, const kimix::string &usr_val) {
        // _merge_dedup_paths: sys entries first, then new user entries.
        kimix::vector<kimix::string> parts;
        auto split_paths = [&parts](const kimix::string &value) {
            size_t begin = 0;
            while (begin <= value.size()) {
                const size_t end = value.find(';', begin);
                const size_t stop = end == kimix::string::npos ? value.size() : end;
                kimix::string entry = kimix::string(trim(value.substr(begin, stop - begin)));
                if (!entry.empty()) {
                    parts.push_back(entry);
                }
                if (end == kimix::string::npos) {
                    break;
                }
                begin = end + 1;
            }
        };
        split_paths(sys_val);
        kimix::vector<kimix::string> user_parts;
        size_t begin = 0;
        while (begin <= usr_val.size()) {
            const size_t end = usr_val.find(';', begin);
            const size_t stop = end == kimix::string::npos ? usr_val.size() : end;
            kimix::string entry = kimix::string(trim(usr_val.substr(begin, stop - begin)));
            if (!entry.empty()) {
                user_parts.push_back(entry);
            }
            if (end == kimix::string::npos) {
                break;
            }
            begin = end + 1;
        }
        for (const kimix::string &entry : user_parts) {
            bool seen = false;
            for (const kimix::string &part : parts) {
                if (to_lower_ascii(part) == to_lower_ascii(entry)) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                parts.push_back(entry);
            }
        }
        return join(parts, ";");
    };
    kimix::string sys_path, usr_path, sys_pathext, usr_pathext;
    read_registry(HKEY_LOCAL_MACHINE,
                  "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
                  "Path", sys_path);
    read_registry(HKEY_CURRENT_USER, "Environment", "Path", usr_path);
    const kimix::string merged_path = merge_dedup(sys_path, usr_path);
    if (!merged_path.empty()) {
        set_env("PATH", merged_path);
    }
    read_registry(HKEY_LOCAL_MACHINE,
                  "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
                  "PATHEXT", sys_pathext);
    read_registry(HKEY_CURRENT_USER, "Environment", "PATHEXT", usr_pathext);
    const kimix::string merged_pathext = merge_dedup(sys_pathext, usr_pathext);
    if (!merged_pathext.empty()) {
        set_env("PATHEXT", merged_pathext);
    }
    print_string("PATH and PATHEXT have been refreshed from the registry.");
    return {};
#else
    print_string("This command is only available on Windows.");
    return {};
#endif
}

// slash.py:364-401 cmd_import (kimi_cli/utils/export.py perform_import).
command_result clicmd_import(const kimix::vector<kimix::string> &args, app_context &app,
                             kimix::vector<kimix::string> &) {
    kimix::string target;
    if (args.size() >= 2) {
        target = kimix::string(trim(clicmd_payload(args)));
    } else if (!args.empty() && args[0].size() > 6 && starts_with(args[0], "import")) {
        target = kimix::string(trim(kimix::string_view(args[0]).substr(6)));
    }
    if (target.empty()) {
        print_string("Usage: /import <file_path or session_id>");
        return {};
    }
    if (app.session == nullptr) {
        print_error("No active session to import into.");
        return {};
    }

    kimix::string content;
    kimix::string source_desc;
    // --- resolve_import_source (export.py:634-717) --------------------------
    const kimix::string path = expand_home(target);
    const kimix::string resolved =
        clicmd_is_absolute(path) ? path : join_path(app.work_dir, path);
    if (dir_exists(resolved)) {
        print_string("The specified path is a directory; please provide a file to import.");
        return {};
    }
    if (file_exists(resolved)) {
        const kimix::string suffix = to_lower_ascii(extension(resolved));
        if (!suffix.empty() && !clicmd_is_importable_suffix(suffix)) {
            print_string("Unsupported file type '" + suffix +
                         "'. /import only supports text-based files "
                         "(e.g. .md, .txt, .json, .py, .log, ...).");
            return {};
        }
        kimix::string read_error;
        if (!read_file(resolved, content, read_error)) {
            print_string("Failed to read file: " + read_error);
            return {};
        }
        if (static_cast<int64_t>(content.size()) > k_clicmd_max_import_size) {
            print_string(kimix::format("File is too large ({:.1f} MB). Maximum import "
                                       "size is {} MB.",
                                       static_cast<double>(content.size()) / 1024.0 / 1024.0,
                                       k_clicmd_max_import_size / (1024 * 1024)));
            return {};
        }
        if (trim(content).empty()) {
            print_string("The file is empty, nothing to import.");
            return {};
        }
        source_desc = "file '" + file_name(resolved) + "'";
    } else {
        // Not a file on disk: try as a session id.
        if (target == app.store.id()) {
            print_string("Cannot import the current session into itself.");
            return {};
        }
        if (!dir_exists(session_store::session_dir(app.work_dir, target))) {
            print_string("'" + target + "' is not a valid file path or session ID.");
            return {};
        }
        session_store source;
        kimix::string open_error;
        if (!source.open(app.work_dir, target, /*resume=*/true, open_error)) {
            print_string("Failed to load source session: " + open_error);
            return {};
        }
        kimix::vector<kimix::llm::Message> history;
        kimix::string load_error;
        if (!source.load_history(history, load_error)) {
            print_string("Failed to load source session: " + load_error);
            return {};
        }
        if (history.empty()) {
            print_string("The source session has no messages.");
            return {};
        }
        content = clicmd_stringify_history(history);
        if (static_cast<int64_t>(content.size()) > k_clicmd_max_import_size) {
            print_string(kimix::format("Session content is too large ({:.1f} MB). "
                                       "Maximum import size is {} MB.",
                                       static_cast<double>(content.size()) / 1024.0 / 1024.0,
                                       k_clicmd_max_import_size / (1024 * 1024)));
            return {};
        }
        source_desc = "session '" + target + "'";
    }

    // build_import_message (export.py:718-732): a user message carrying the
    // imported context; the native message model has no content-part list, so
    // the system preamble + the import block share the one text body.
    kimix::llm::Message message;
    message.role = "user";
    message.content =
        kimix::string("The user has imported context from ") + source_desc +
        ". This is a prior conversation history that may be relevant to the "
        "current session. Please review this context and use it to inform your "
        "responses.\n<imported_context source=\"" + source_desc + "\">\n" + content +
        "\n</imported_context>";
    app.session->history().push_back(std::move(message));
    kimix::string save_error;
    app_save_session(app, save_error);
    print_string("Imported context from " + source_desc + " (" +
                 kimix::format("{}", content.size()) + " chars).");
    if (starts_with(source_desc, "file") &&
        clicmd_is_sensitive_file(file_name(resolved))) {
        print_string("Warning: This file may contain secrets (API keys, tokens, "
                     "credentials). The content is now part of your session context.");
    }
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
    const kimix::string source_id = app.store.id();
    if (target == source_id) {
        print_error("Target session name must be different from current session name.");
        return {};
    }
    // I9 (commands.py:131-147 _release_session_resources): save, set the
    // cancel event, tear the live tools/soul/context writer down, then copy.
    // (The native store keeps no open handles or DB connection - the copy is
    // directory-level - so the release is the bookkeeping parity: nothing
    // holds the session while the copy runs.)
    kimix::string save_error;
    if (!app_save_session(app, save_error)) {
        save_error.clear(); // best effort, like the reference's release step
    }
    app.cancel.cancel();
    app.soul.reset();
    app.session.reset();
    if (app.approval != nullptr) {
        app.approval->set_wire_sink(nullptr);
    }
    app.wire.reset();
    kimix::string error;
    if (!app.store.store_as(target, error)) {
        print_error("Store failed: " + error);
        // commands.py:179-199: recover the original session so the CLI is not
        // left broken.
        kimix::string recovery;
        if (!app_rebind_session(app, recovery)) {
            print_error("Failed to resume original session: " + recovery);
        }
        return {};
    }
    // commands.py:205-223: re-open the source session (resume) and continue.
    if (!app_rebind_session(app, error)) {
        print_error("Store succeeded but failed to resume original session: " + error);
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
        // I7: the new named session joins the in-process cache.
        app_touch_cli_session(app);
        print_success("Created and switched to session: " + name);
        return {};
    }
    // /sessions - the in-process session cache (I7, _globals._cli_sessions via
    // commands.py:361-404): sessions this run created or resumed, the current
    // row's usage refreshed from the live session.
    kimix::vector<cli_session_row> rows = app.cli_sessions;
    if (rows.empty()) {
        print_warning("No sessions found.");
        return {};
    }
    {
        double ratio = 0.0;
        int64_t tokens = 0;
        app_usage(app, ratio, tokens);
        for (cli_session_row &row : rows) {
            if (row.id == app.store.id()) {
                row.context_usage = ratio;
                row.context_tokens = tokens;
                row.usage_known = true;
            }
        }
    }
    // Sort by updated_at descending, ties by id ascending (deterministic).
    for (size_t i = 0; i + 1 < rows.size(); ++i) {
        for (size_t j = i + 1; j < rows.size(); ++j) {
            const bool before =
                rows[j].updated_at > rows[i].updated_at ||
                (rows[j].updated_at == rows[i].updated_at && rows[j].id < rows[i].id);
            if (before) {
                cli_session_row tmp = rows[i];
                rows[i] = rows[j];
                rows[j] = tmp;
            }
        }
    }
    size_t id_width = kimix::string("session id").size();
    for (const cli_session_row &row : rows) {
        id_width = row.id.size() > id_width ? row.id.size() : id_width;
    }
    // commands.py:394: f'{" ":1}  {"session id":<{id_width}}  ...' - one space
    // plus two literal spaces, so the id column starts at offset 3.
    print_info("   " + clicmd_pad("session id", id_width) + "  " +
               clicmd_pad("updated at", 19) + "  " + clicmd_pad("context usage", 22) +
               "  title");
    for (const cli_session_row &row : rows) {
        const kimix::string marker = (row.id == app.store.id()) ? "*" : " ";
        const kimix::string usage =
            clicmd_usage_cell(row.context_usage, row.context_tokens, row.usage_known);
        // commands.py:398: pendulum.from_timestamp(updated_at).strftime - LOCAL
        // time, not UTC.
        print_string(marker + "  " + clicmd_pad(row.id, id_width) + "  " +
                     format_local(row.updated_at) + "  " + clicmd_pad(usage, 22) + "  " +
                     (row.title.empty() ? kimix::string("Untitled") : row.title));
    }
    return {};
}

command_result clicmd_init(const kimix::vector<kimix::string> &, app_context &app,
                           kimix::vector<kimix::string> &) {
    // H1 (commands.py:503-509 -> init.py:268-333): the interactive wizard -
    // provider templates, the validated question walk and the save - then a
    // fresh anonymous session picks the settings up.  The reference writes
    // <repo>/src/kimix/default_config.json; the native CLI writes
    // <work_dir>/default_config.json (the file cli_main's boot search finds).
    const kimix::string config_path = join_path(app.work_dir, "default_config.json");
    init_input_fn input = [&app](kimix::string_view prompt, kimix::string &line) {
        // init.py's prompts print in print_info's bright magenta with no
        // trailing newline; _ask then reads one line through _input.
        if (!prompt.empty()) {
            print_raw(colorful_text(prompt, static_cast<int>(color::bright_magenta)));
            std::fflush(stdout);
        }
        return app_read_input(app, "", line);
    };
    // A Ctrl-C / EOF keeps the CLI alive with "keyboard interruped." and the
    // partially collected config (init.py:322-324); /init still re-opens the
    // session and prints Initialized. (commands.py:505-508).
    kimix::string open_error;
    if (!run_init_wizard(app, config_path, /*initialize=*/true, /*open_after=*/true,
                         input)) {
        print_warning("keyboard interruped.");
    }
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
    // I8 (commands.py:650-652): the failure carries the exception text.
    kimix::string prompt_error;
    if (!app_run_prompt(app, prompt_str, &prompt_error)) {
        print_error("Prompt failed: " +
                    (prompt_error.empty() ? kimix::string("the turn failed")
                                          : prompt_error));
    }
    return {};
}

// prompt.py:815-839 build_plan_retry_reminder - the automated plan-file check
// nudge delivered to the planner as a fresh user-role turn.
kimix::string clicmd_plan_retry_reminder(const kimix::string &requirement) {
    kimix::string out =
        "[system check] This is an automated verification generated by the "
        "plan-file check after your previous turn; it is NOT a message from the "
        "user. No human has read, confirmed, or reviewed anything yet. "
        "The check failed: the plan file was not found on disk or is empty, so the "
        "plan was never saved. Call WritePlan with the complete plan now, then end "
        "your turn. Do not ask the user questions or phrase your reply as a "
        "confirmation request.\n\nRequirement:\n";
    out += kimix::string(trim(requirement));
    return out;
}

// A planner agent: the current manifest's toolset narrowed to the plan tools
// plus the read-only research tools (the reduced read-only TodoMaker planner).
agent_config clicmd_planner_agent(const app_context &app) {
    agent_config planner = app.agent;
    kimix::vector<kimix::string> tools;
    for (const char *name : {"writeplan", "readplan", "editplan", "read", "glob",
                             "grep", "list"}) {
        tools.push_back(kimix::string(name));
    }
    planner.enabled_tools = tools;
    return planner;
}

// prompt_plan_async (prompt.py:842-1072): the isolated planner sub-session,
// 3 attempts + the retry nudge, the file-existence check, the OS reveal, the
// y/n review + revision loop and the implement + review turns.  The planner
// has no read-only runtime flag on the native side, so the reduction is the
// TOOLSET above: the planner session only registers the plan + read tools.
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

    // 1. A pre-existing plan file is removed first (prompt.py:845-847).
    if (file_exists(path)) {
        kimix::string remove_error;
        remove_all(path, remove_error); // remove_all removes files too
    }
    if (app.session != nullptr) {
        // The plan tools gate their answers on plan_writing_path + the flag
        // (note._enable_plan); the PLANNER session carries its own copies.
        // The main session gets plan mode too (the implement phase revises
        // the plan) - and, like the planner, the default plan file so the
        // tools never answer "no plan_writing_path set" (bug_tool.md item 3).
        app.session->tool_session().plan_enabled = true;
        if (app.session->tool_session().plan_path.empty()) {
            app.session->tool_session().plan_path =
                cli_default_plan_path(app.work_dir);
        }
    }

    // 2. The isolated planner sub-session (prompt.py:854-906).
    session_store planner_store;
    kimix::string error;
    if (!planner_store.open(app.work_dir, "", /*resume=*/false, error)) {
        print_error("prompt_plan failed: " + error);
        return {};
    }
    kimix::agent::AgentSession planner_session(app.work_dir);
    planner_session.set_state_dir(planner_store.dir());
    planner_session.tool_session().session_id = planner_store.id();
    planner_session.tool_session().plan_path = path;
    planner_session.tool_session().plan_enabled = true;
    const agent_config planner_agent = clicmd_planner_agent(app);
    kimix::agent::IChatBackend *chat =
        app.backend ? static_cast<kimix::agent::IChatBackend *>(app.backend.get())
                    : app.injected;
    if (chat == nullptr) {
        print_error("prompt_plan failed: no chat backend is available");
        kimix::string close_error;
        planner_store.close(/*delete_if_anonymous=*/true, close_error);
        return {};
    }
    kimix::agent::KimiSoul planner_soul(planner_session, *chat,
                                        app.soul_options);
    // The planner renders through the same terminal (the reference streams its
    // messages through print_agent_json too).

    // 3. Up to three generation attempts (prompt.py:914-955).
    const int max_plan_attempts = 3;
    bool plan_generated = false;
    kimix::string reminder =
        "read the following requirement carefully and generate a comprehensive plan. "
        "save the complete plan to a file using the WritePlan tool. "
        "Requirement:\n" + kimix::string(trim(requirement));
    for (int attempt = 0; attempt < max_plan_attempts; ++attempt) {
        if (app.cancel.cancelled()) {
            break;
        }
        // colorful_print_word(BRIGHT_CYAN, require_new_line=True).
        print_word(colorful_text("Generating plan (attempt " +
                                     std::to_string(static_cast<long long>(attempt + 1)) +
                                     "/" +
                                     std::to_string(static_cast<long long>(max_plan_attempts)) +
                                     ")...\n",
                                 96),
                   true, false);
        app_run_isolated_turn(app, planner_session, planner_soul, reminder, "Start...");
        print_word("\n", true, true);
        if (file_exists(path)) {
            kimix::string content, read_error;
            if (read_file(path, content, read_error) && !content.empty()) {
                plan_generated = true;
                break;
            }
        }
        if (attempt < max_plan_attempts - 1) {
            reminder = clicmd_plan_retry_reminder(requirement);
        }
    }
    if (!plan_generated) {
        // prompt.py:948-955.
        print_error("Plan generation failed: plan file not found.");
        kimix::string close_error;
        planner_store.close(/*delete_if_anonymous=*/true, close_error);
        return {};
    }

    // 4. "Plan generated: {abs}" + the OS reveal (prompt.py:957-975).
    const kimix::string plan_abs = absolute_path(path);
    print_word(colorful_text("Plan generated: " + plan_abs + "\n", 92, -1, "1"),
               true, true);
    open_with_default_app(path);

    // 5. The review + revision loop (prompt.py:977-1025).
    bool execute_plan = true;
    for (;;) {
        kimix::string answer;
        if (!app_read_input(app, "Do you want to implement the plan? (y/n): ", answer)) {
            execute_plan = false; // EOF: give up without implementing
            break;
        }
        if (to_lower_ascii(trim(answer)) == "y") {
            break;
        }
        kimix::string feedback;
        if (!app_read_input(app, "Please describe the changes you want (/quit to give up): ",
                            feedback)) {
            execute_plan = false;
            break;
        }
        feedback = kimix::string(trim(feedback));
        if (feedback.empty()) {
            continue;
        }
        if (to_lower_ascii(feedback) == "/quit") {
            execute_plan = false;
            break;
        }
        const kimix::string revision_reminder =
            "The user reviewed the plan and wants the following changes:\n\n" +
            feedback + "\n\nPlease update the plan file accordingly using the WritePlan "
                       "or EditPlan tools. ";
        print_word(colorful_text("Revising plan...\n", 96), true, false);
        app_run_isolated_turn(app, planner_session, planner_soul, revision_reminder,
                              "Start...");
        print_word("\n", true, true);
        if (file_exists(path)) {
            open_with_default_app(path);
        }
    }

    if (execute_plan) {
        // 6. The implement + review turns (prompt.py:1027-1061).
        if (!file_exists(path)) {
            print_error("Plan file " + path + " no longer exists. Aborting.");
        } else {
            kimix::string plan_content;
            kimix::string read_error;
            if (!read_file(path, plan_content, read_error)) {
                print_error("prompt_plan failed: " + read_error);
            } else {
                const size_t plan_size = plan_content.size();
                // A NEW default session takes the implement + review turns
                // (prompt.py:1042); the native CLI switches to a fresh
                // anonymous session the same way.
                if (!app_open_session(app, "", /*resume=*/false, error)) {
                    print_error("prompt_plan failed: " + error);
                } else {
                    kimix::string impl_prompt;
                    kimix::string review_reminder;
                    if (plan_size > 100 * 1024) {
                        impl_prompt =
                            "Read this plan `" + path +
                            "`, carefully research, read all related files first, call "
                            "todo_list to record, then implement the plan step-by-step.";
                        review_reminder =
                            "Review the plan in `" + path +
                            "` and ensure all tasks are completed.";
                    } else {
                        impl_prompt =
                            "Read this plan:\n\n" + plan_content +
                            "\n\ncarefully research, read all related files first, call "
                            "todo_list to record, then implement the plan step-by-step:";
                        review_reminder =
                            "Review this plan and ensure all tasks are completed:\n\n" +
                            plan_content;
                    }
                    kimix::string plan_error;
                    if (!app_run_prompt(app, impl_prompt, &plan_error)) {
                        print_error("prompt_plan failed: " + plan_error);
                    }
                    if (!app_run_prompt(app, review_reminder, &plan_error)) {
                        print_error("prompt_plan failed: " + plan_error);
                    }
                }
            }
        }
    }
    // 7. finally: close the planner session (prompt.py:1069-1072).
    kimix::string close_error;
    planner_store.close(/*delete_if_anonymous=*/true, close_error);
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
    // The reference ignores the cancel flag here (text, _ = _read_multi_line):
    // /cancel leaves the task empty, so the warning below still prints.
    (void)cancelled;
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
    // The reference's _context_is_non_empty 3-tier check
    // (commands.py:733-752 -> _cmd_reflection:909): the old tokens==0 guard
    // keyed on estimated_tokens(), which carries the system prompt + tool
    // schema overhead and is never zero, so the fast path was unreachable.
    if (!clicmd_context_is_non_empty(app)) {
        print_error("Context is empty. /reflection requires a non-empty context.");
        return {};
    }
    // The display line keeps the full next-request estimate (history +
    // overhead), matching the old behaviour now that the guard no longer
    // derives from it.
    double usage_ratio = 0.0;
    int64_t tokens = 0;
    app_usage(app, usage_ratio, tokens);
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
    // I8 (commands.py:915-917): the failure carries the exception text.
    kimix::string reflection_error;
    if (!app_run_prompt(app, prompt_str, &reflection_error)) {
        print_error("Reflection failed: " +
                    (reflection_error.empty() ? kimix::string("the turn failed")
                                              : reflection_error));
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
        const kimix::string name = file_name(script);
        print_info("Executing " + name);
        print_raw("\n");
        // H8 (commands.py:690-711): the persistent exec_ctx. The reference
        // executes .py scripts IN PROCESS with exec(s, exec_ctx), so state
        // (imports, definitions, module-level variables) survives across
        // /code calls and sys.argv is set per call. The native analogue is a
        // dedicated long-lived interpreter process speaking newline-JSON
        // (see builtin_tools/python_code_session.h); state persistence and
        // sys.argv/__file__ behave the same. When the persistent child cannot
        // start (no interpreter, spawn failure, handshake timeout) the call
        // falls back to the pre-H8 one-shot `python <script> args...` spawn.
        kimix::builtin_tools::python::exec_result result;
        kimix::string session_error;
        kimix::builtin_tools::python::CodeExecSession &session =
            kimix::builtin_tools::python::code_exec_session();
        if (session.start(app.work_dir, session_error) &&
            session.exec_file(script,
                              kimix::vector<kimix::string>(parts.begin() + 1,
                                                           parts.end()),
                              result, session_error)) {
            if (!result.output.empty()) {
                print_raw(result.output);
            }
            if (!result.stderr_text.empty()) {
                print_raw(result.stderr_text);
            }
            if (result.ok) {
                print_success(kimix::string("Done."));
            } else {
                // commands.py:705-708: the exception text, then the traceback
                // (no exit-code line on this path - the reference's exec() has
                // no exit code).
                print_error(result.error);
                if (!result.traceback.empty()) {
                    print_error(result.traceback);
                }
            }
            return {};
        }
        if (session_error.empty()) {
            session_error = "persistent interpreter unavailable";
        }
        print_warning("Persistent python session unavailable (" + session_error +
                      "); falling back to a one-shot run.");
        argv.push_back(kimix::string("python"));
        argv.push_back(script);
        for (size_t i = 1; i < parts.size(); ++i) {
            argv.push_back(parts[i]);
        }
    } else {
        argv.push_back(script);
        for (size_t i = 1; i < parts.size(); ++i) {
            argv.push_back(parts[i]);
        }
        // The reference prints ' '.join(cmd) with the resolved script path.
        print_info("Running: " + clicmd_join_lines(argv, " "));
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
        add("btw", "Ask a side question without touching the main conversation",
            &clicmd_btw);
        add("cmd", "Execute system command", &clicmd_cmd);
        add("fix", "Run a command and fix errors if any", &clicmd_fix);
        add("txt", "Input multiple line text", &clicmd_txt);
        add("file", "Load a file and execute its content line by line", &clicmd_file);
        add("plan", "Plan a long-term task, step-by-step, then execute", &clicmd_plan);
                  add("compact", "Compact conversation context", &clicmd_compact);
          add("prune", "Prune stale context content (smart elision)",
              &clicmd_prune);
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
        // I2/G5: the soul-level slash layer (slash.py COMMANDS), so the native
        // CLI answers the same names the reference's soul dispatches.
        add("yolo", "Toggle YOLO mode (auto-approve all actions)", &clicmd_yolo);
        add("afk",
            "Toggle afk mode (auto-dismiss AskUserQuestion, auto-approve tool calls)",
            &clicmd_afk);
        add("add-dir",
            "Add a directory to the workspace. Usage: /add-dir <path>. Run without "
            "args to list added dirs",
            &clicmd_add_dir);
        add("refresh-env",
            "Refresh PATH/PATHEXT from the Windows registry (no restart required)",
            &clicmd_refresh_env);
        add("import", "Import context from a file or session ID", &clicmd_import);
        // The _cmd_unknown fallback (not a user-visible command).
        add("unknown", "Fallback for an unrecognised command", &clicmd_unknown);
        return table;
    }();
    return entries;
}

const command_entry *find_command(kimix::string_view name) {
    // slash.py:418-426: COMMANDS.get(ALIASES.get(name, name)) - "/reset" is
    // the one alias and resolves to /clear.
    if (name == "reset") {
        name = "clear";
    }
    for (const command_entry &entry : command_map()) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

const kimix::vector<command_info> &list_command_infos() {
    // slash.py:404-415 COMMANDS (insertion order) + ALIASES + the docstrings.
    static const kimix::vector<command_info> infos = [] {
        kimix::vector<command_info> table;
        auto add = [&table](const char *name, const char *description,
                            kimix::vector<kimix::string> aliases = {}) {
            command_info info;
            info.name = name;
            info.description = description;
            info.aliases = std::move(aliases);
            table.push_back(std::move(info));
        };
        add("init", "Analyze the codebase and generate an `AGENTS.md` file");
        add("compact",
            "Compact the context (optionally with a custom focus, e.g. /compact "
            "keep db discussions)");
        add("prune", "Manually trigger context pruning (smart history removal)");
        add("clear", "Clear the context", {"reset"});
        add("yolo", "Toggle YOLO mode (auto-approve all actions)");
        add("afk", "Toggle afk mode (auto-dismiss AskUserQuestion, auto-approve tool calls)");
        add("add-dir",
            "Add a directory to the workspace. Usage: /add-dir <path>. Run without "
            "args to list added dirs");
        add("export", "Export current session context to a markdown file");
        add("refresh-env",
            "Refresh PATH/PATHEXT from the Windows registry (no restart required)");
        add("import", "Import context from a file or session ID");
        return table;
    }();
    return infos;
}

} // namespace kimix::cli
