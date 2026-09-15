// Test for builtin_tools/run_tool.h (namespace kimix::builtin_tools::run).
//
// Covers:
// - shlex_split: 100 CPython-generated golden vectors (both posix modes),
//   including the "No closing quotation" / "No escaped character" ValueErrors
// - shlex_quote / shlex_join: CPython-generated goldens
// - strip_outer_double_quotes
// - resolve_executable: progressive-prefix lookup for unquoted paths with
//   spaces, non-posix quote stripping, single-token commands
// - which / check_executable: PATH+PATHEXT scan, bare-python fallback,
//   path-separator vs bare-name resolution
// - parse_env: the `A = B` triple rule (string form), the list form, K=V
//   splitting and the K -> K=1 default
// - cd_prefix: bash shlex quoting and pwsh '' doubling
// - validate_workdir / py_repr_char: allowed set + Python repr wording
// - normalize_forbidden / find_forbidden / forbidden_message
// - parse_params: aliases, mode normalization, _infer_mode, _validate_cmd
//   messages, timeout / max_lines bounds
// - build_display_command: arg clipping, shlex.join, huge-command cull
// - Run Tool wrapper: hardline floor, forbidden floor, unsupported shell
//   syntax, prepared-command (non-native) mode
//
// All test logic lives in main() scope; no file-scope static registrations.
#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "builtin_tools/run_tool.h"

#include <string>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools;
using namespace kimix::builtin_tools::run;

namespace {

kimix::string kix(std::string_view sv) {
    return kimix::string(sv.data(), sv.size());
}

std::string sv_of(const kimix::string &s) {
    return std::string(s.data(), s.size());
}

// Golden vectors generated from the host CPython shlex module by
// .kimix_cache/run_goldens.py.
#include "run_goldens.inc"

// Probe backed by an explicit set of "existing files".
// Normalize both separators so a probe written with '/' also matches the
// Windows backslash joins produced by the runner.
std::string norm_seps(std::string_view text) {
    std::string out(text);
    for (char &c : out) {
        if (c == 0x5C) { // backslash
            c = '/';
        }
    }
    return out;
}

// Join directories with the platform PATH separator (`;` on Windows, `:`
// elsewhere) - the runner reads PATH with that separator.
kimix::string path_of(std::vector<std::string> dirs) {
#ifdef KIMIX_PLATFORM_WINDOWS
    const char sep = ';';
#else
    const char sep = ':';
#endif
    kimix::string out;
    for (const std::string &dir : dirs) {
        if (!out.empty()) {
            out.push_back(sep);
        }
        out += kix(dir);
    }
    return out;
}

// Convenience overloads so callers can write path_of("/a") or
// path_of("/a", "/b") without building a vector by hand.
inline kimix::string path_of(const char *a) { return path_of({std::string(a)}); }
inline kimix::string path_of(const char *a, const char *b) {
    return path_of({std::string(a), std::string(b)});
}

// Rewrite '/' to the platform separator for path-with-separator inputs.
kimix::string sep_path(std::string_view posix_style) {
    kimix::string out(posix_style.data(), posix_style.size());
#ifdef KIMIX_PLATFORM_WINDOWS
    for (char &c : out) {
        if (c == '/') {
            c = 0x5C; // backslash
        }
    }
#else
    (void)out;
#endif
    return out;
}

is_file_probe probe_of(std::vector<std::string> existing) {
    auto shared = kimix::shared_ptr<std::vector<std::string>>(
        new std::vector<std::string>(std::move(existing)));
    return [shared](kimix::string_view path) {
        const std::string want = norm_seps(path);
        for (const std::string &have : *shared) {
            if (norm_seps(have) == want) {
                return true;
            }
        }
        return false;
    };
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    // -----------------------------------------------------------------------
    // shlex_split - CPython goldens
    // -----------------------------------------------------------------------
    "shlex_split_goldens"_test = [] {
        size_t checked = 0;
        for (const shlex_golden &g : kShlexGoldens) {
            kimix::vector<kimix::string> tokens;
            kimix::string error;
            const bool ok =
                shlex_split(kix(g.input), g.posix, tokens, error);
            expect(ok == g.ok) << "ok flag mismatch for: " << g.input;
            if (!g.ok) {
                expect(sv_of(error) == std::string(g.error))
                    << "error mismatch for: " << g.input;
                ++checked;
                continue;
            }
            expect(tokens.size() == static_cast<size_t>(g.token_count))
                << "token count mismatch for: " << g.input << " (posix="
                << g.posix << ") got " << tokens.size() << " want "
                << g.token_count;
            for (int i = 0; i < g.token_count && i < 8; ++i) {
                if (g.tokens[i] == nullptr) {
                    break;
                }
                if (static_cast<size_t>(i) < tokens.size()) {
                    expect(sv_of(tokens[static_cast<size_t>(i)]) ==
                           std::string(g.tokens[i]))
                        << "token " << i << " mismatch for: " << g.input
                        << " (posix=" << g.posix << ")";
                }
            }
            ++checked;
        }
        expect(checked > 90u) << "golden table should be exercised";
    };

    "shlex_split_tool_error_wrapper"_test = [] {
        kimix::vector<kimix::string> tokens;
        const tool_error err = shlex_split_tool("'unterminated", true, tokens);
        expect(err.failed());
        expect(err.status == tool_status::invalid_input);
        expect(err.message == kix("No closing quotation"));
    };

    // -----------------------------------------------------------------------
    // shlex_quote / shlex_join - CPython goldens
    // -----------------------------------------------------------------------
    "shlex_quote_goldens"_test = [] {
        for (const quote_golden &g : kQuoteGoldens) {
            expect(sv_of(shlex_quote(kix(g.input))) == std::string(g.quoted))
                << "shlex_quote mismatch for: " << g.input;
        }
    };

    "shlex_join_goldens"_test = [] {
        for (const join_golden &g : kJoinGoldens) {
            kimix::vector<kimix::string> argv;
            for (int i = 0; i < g.count && i < 4; ++i) {
                if (g.argv[i] == nullptr) {
                    break;
                }
                argv.push_back(kix(g.argv[i]));
            }
            expect(sv_of(shlex_join(kimix::span<const kimix::string>(argv))) ==
                   std::string(g.joined))
                << "shlex_join mismatch for count " << g.count;
        }
    };

    "shlex_quote_round_trips_through_split"_test = [] {
        const std::vector<std::string> originals = {
            "simple", "with space", "with'quote", "with\"double",
            "with\\backslash", "with$var", "with`tick`", "", "tab\there"};
        for (const std::string &original : originals) {
            const kimix::string quoted = shlex_quote(kix(original));
            kimix::vector<kimix::string> tokens;
            kimix::string error;
            const bool ok = shlex_split(quoted, true, tokens, error);
            expect(ok) << "quoted form must parse: " << original;
            expect(tokens.size() == 1u) << "one token for: " << original;
            if (tokens.size() == 1u) {
                expect(sv_of(tokens[0]) == original)
                    << "round trip mismatch for: " << original;
            }
        }
    };

    // -----------------------------------------------------------------------
    // strip_outer_double_quotes
    // -----------------------------------------------------------------------
    "strip_outer_double_quotes"_test = [] {
        expect(strip_outer_double_quotes("\"abc\"") == kix("abc"));
        expect(strip_outer_double_quotes("abc") == kix("abc"));
        expect(strip_outer_double_quotes("\"") == kix("\""));
        expect(strip_outer_double_quotes("\"\"") == kix(""));
        expect(strip_outer_double_quotes("a\"b\"") == kix("a\"b\""));
        expect(strip_outer_double_quotes("'abc'") == kix("'abc'"));
    };

    // -----------------------------------------------------------------------
    // resolve_executable
    // -----------------------------------------------------------------------
    "resolve_executable_single_token"_test = [] {
        const kimix::vector<kimix::string> parts = {kix("git")};
        const resolved_command r = resolve_executable(
            kimix::span<const kimix::string>(parts), true,
            probe_of({"git"}));
        expect(r.executable == kix("git"));
        expect(r.args.empty());
        expect(r.consumed_tokens == 1u);
    };

    "resolve_executable_progressive_prefix"_test = [] {
        // "C:\Program Files\tool.exe --flag" split non-posix keeps the space
        // split, and the progressive prefix lookup glues the path back.
        kimix::vector<kimix::string> tokens;
        kimix::string error;
        expect(shlex_split("C:\\Program Files\\tool.exe --flag", false, tokens,
                           error));
        const resolved_command r = resolve_executable(
            kimix::span<const kimix::string>(tokens), false,
            probe_of({"C:\\Program Files\\tool.exe"}));
        expect(r.executable == kix("C:\\Program Files\\tool.exe"));
        expect(r.args.size() == 1u);
        expect(r.args[0] == kix("--flag"));
        expect(r.consumed_tokens == 2u);
    };

    "resolve_executable_prefers_first_existing_prefix"_test = [] {
        kimix::vector<kimix::string> tokens;
        kimix::string error;
        expect(shlex_split("/opt/my tool/run arg1 arg2", true, tokens, error));
        const resolved_command r = resolve_executable(
            kimix::span<const kimix::string>(tokens), true,
            probe_of({"/opt/my tool/run"}));
        expect(r.executable == kix("/opt/my tool/run"));
        expect(r.args.size() == 2u);
        expect(r.args[0] == kix("arg1"));
        expect(r.args[1] == kix("arg2"));
    };

    "resolve_executable_strips_quotes_non_posix"_test = [] {
        kimix::vector<kimix::string> tokens;
        kimix::string error;
        expect(shlex_split("\"C:\\dir\\app.exe\" \"an arg\"", false, tokens,
                           error));
        const resolved_command r = resolve_executable(
            kimix::span<const kimix::string>(tokens), false,
            probe_of({"C:\\dir\\app.exe"}));
        expect(r.executable == kix("C:\\dir\\app.exe"));
        expect(r.args.size() == 1u);
        expect(r.args[0] == kix("an arg"));
    };

    // -----------------------------------------------------------------------
    // which / check_executable
    // -----------------------------------------------------------------------
    "which_finds_on_path"_test = [] {
        const kimix::string found = which("git", path_of("/usr/bin", "/bin"),
                                          "", probe_of({"/usr/bin/git"}));
        expect(norm_seps(found) == "/usr/bin/git") << "got: " << found.c_str();
    };

    "which_returns_empty_when_absent"_test = [] {
        const kimix::string found = which("nope", path_of("/usr/bin", "/bin"),
                                          "", probe_of({"/usr/bin/git"}));
        expect(found.empty());
    };

    "which_honours_pathext_on_windows"_test = [] {
        // A bare name with no extension resolves through PATHEXT.
        const kimix::string found =
            which("tool", path_of("/opt/bin"), ".EXE;.CMD",
                  probe_of({"/opt/bin/tool.EXE"}));
#ifdef KIMIX_PLATFORM_WINDOWS
        expect(!found.empty()) << "PATHEXT lookup should resolve tool.EXE";
#else
        expect(found.empty()) << "POSIX has no PATHEXT expansion";
#endif
    };

    "check_executable_path_separator_requires_file"_test = [] {
        const executable_check hit = check_executable(
            sep_path("/opt/tool/run"), probe_of({"/opt/tool/run"}),
            path_of("/usr/bin"), "");
        expect(hit.is_process);
        expect(!hit.is_python_fallback);
        const executable_check miss = check_executable(
            sep_path("/opt/tool/missing"), probe_of({}), path_of("/usr/bin"),
            "");
        expect(!miss.is_process);
    };

    "check_executable_bare_name_uses_path"_test = [] {
        const executable_check hit = check_executable(
            "git", probe_of({"/usr/bin/git"}), path_of("/usr/bin"), "");
        expect(hit.is_process);
        const executable_check miss =
            check_executable("git", probe_of({}), path_of("/usr/bin"), "");
        expect(!miss.is_process);
    };

    "check_executable_python_fallback"_test = [] {
        // `python` with no PATH hit and no ./python resolves to the injected
        // interpreter (Python: sys.executable).
        const executable_check r =
            check_executable("python", probe_of({}), "", "/usr/local/bin/py3");
        expect(r.is_process);
        expect(r.is_python_fallback);
        expect(r.executable == kix("/usr/local/bin/py3"));
        // With a PATH hit there is no fallback.
        const executable_check direct =
            check_executable("python", probe_of({"/usr/bin/python"}),
                             path_of("/usr/bin"), "/fallback");
        expect(direct.is_process);
        expect(!direct.is_python_fallback);
        expect(direct.executable == kix("python"));
    };

    // -----------------------------------------------------------------------
    // parse_env
    // -----------------------------------------------------------------------
    "parse_env_list_form"_test = [] {
        const kimix::vector<kimix::string> list = {kix("A=1"), kix("B")};
        const env_parse_result r =
            parse_env("", false, kimix::span<const kimix::string>(list), true);
        expect(!r.error.failed());
        expect(r.values.size() == 2u);
        expect(r.values[0].name == kix("A"));
        expect(r.values[0].value == kix("1"));
        expect(r.values[1].name == kix("B"));
        expect(r.values[1].value == kix("1")); // no '=' -> '1'
    };

    "parse_env_string_form_triple_rule"_test = [] {
        // "A = B C=1" -> shlex tokens [A, =, B, C=1]; the `A = B` triple is
        // re-joined into A=B, and C=1 is kept as-is.
        const env_parse_result r =
            parse_env("A = B C=1", true, {}, true);
        expect(!r.error.failed());
        expect(r.values.size() == 2u);
        expect(r.values[0].name == kix("A"));
        expect(r.values[0].value == kix("B"));
        expect(r.values[1].name == kix("C"));
        expect(r.values[1].value == kix("1"));
    };

    "parse_env_value_split_on_first_equals"_test = [] {
        const kimix::vector<kimix::string> list = {kix("URL=a=b=c")};
        const env_parse_result r =
            parse_env("", false, kimix::span<const kimix::string>(list), true);
        expect(r.values.size() == 1u);
        expect(r.values[0].name == kix("URL"));
        expect(r.values[0].value == kix("a=b=c"));
    };

    "env_to_extra_env_format"_test = [] {
        const kimix::vector<named_value> values = {{kix("A"), kix("1")},
                                                   {kix("B"), kix("x y")}};
        const kimix::vector<kimix::string> out = env_to_extra_env(
            kimix::span<const named_value>(values));
        expect(out.size() == 2u);
        expect(out[0] == kix("A=1"));
        expect(out[1] == kix("B=x y"));
    };

    // -----------------------------------------------------------------------
    // cd_prefix
    // -----------------------------------------------------------------------
    "cd_prefix_bash_quotes_with_shlex"_test = [] {
        expect(cd_prefix("", "bash").empty());
        expect(cd_prefix("/tmp/x", "bash") == kix("cd /tmp/x && "));
        expect(cd_prefix("/tmp/a b", "bash") == kix("cd '/tmp/a b' && "));
        expect(cd_prefix("/tmp/it's", "bash") ==
               kix("cd '/tmp/it'\"'\"'s' && "));
    };

    "cd_prefix_pwsh_doubles_single_quotes"_test = [] {
        expect(cd_prefix("C:\\x", "pwsh") == kix("cd 'C:\\x'; "));
        expect(cd_prefix("C:\\it's", "pwsh") == kix("cd 'C:\\it''s'; "));
    };

    // -----------------------------------------------------------------------
    // validate_workdir
    // -----------------------------------------------------------------------
    "validate_workdir_allows_the_safe_set"_test = [] {
        expect(!validate_workdir("").has_value());
        expect(!validate_workdir("C:/dev/kimix-native").has_value());
        expect(!validate_workdir("/home/user_1/.config").has_value());
        expect(!validate_workdir("~/projects").has_value());
        expect(!validate_workdir("dir with space").has_value());
    };

    "validate_workdir_rejects_metacharacters"_test = [] {
        const auto dollar = validate_workdir("a$b");
        expect(dollar.has_value());
        expect(*dollar ==
               kix("Invalid workdir: character '$' is not allowed."));
        const auto semi = validate_workdir("a;b");
        expect(*semi == kix("Invalid workdir: character ';' is not allowed."));
        // A single quote switches Python's repr() to double quotes.
        const auto quote = validate_workdir("a'b");
        expect(*quote ==
               kix("Invalid workdir: character \"'\" is not allowed."));
    };

    "validate_workdir_reports_the_first_offender"_test = [] {
        const auto err = validate_workdir("ok|bad&");
        expect(err.has_value());
        expect(sv_of(*err).find("'|'") != std::string::npos);
    };

    "validate_workdir_rejects_non_ascii"_test = [] {
        const auto err = validate_workdir("caf\xc3\xa9");
        expect(err.has_value());
        expect(sv_of(*err).find("is not allowed.") != std::string::npos);
    };

    "py_repr_char_escapes"_test = [] {
        expect(py_repr_char('a') == kix("'a'"));
        expect(py_repr_char('\'') == kix("\"'\""));
        expect(py_repr_char('\\') == kix("'\\\\'"));
        expect(py_repr_char('\n') == kix("'\\n'"));
        expect(py_repr_char('\t') == kix("'\\t'"));
        expect(py_repr_char('\r') == kix("'\\r'"));
        expect(py_repr_char(0x07) == kix("'\\x07'"));
        expect(py_repr_char(0x7F) == kix("'\\x7f'"));
    };

    // -----------------------------------------------------------------------
    // forbidden commands
    // -----------------------------------------------------------------------
    "normalize_forbidden_collapses_and_dedupes"_test = [] {
        const kimix::vector<kimix::string> raw = {
            kix("git   push"), kix("git push"), kix(""), kix("  "),
            kix("rm -rf /")};
        const kimix::vector<kimix::string> out = normalize_forbidden(
            kimix::span<const kimix::string>(raw));
        expect(out.size() == 2u);
        expect(out[0] == kix("git push"));
        expect(out[1] == kix("rm -rf /"));
    };

    "find_forbidden_matches_normalized_command"_test = [] {
        const kimix::vector<kimix::string> keywords = {kix("git push")};
        expect(find_forbidden("git   push origin",
                              kimix::span<const kimix::string>(keywords)) ==
               kix("git push"));
        expect(find_forbidden("git status",
                              kimix::span<const kimix::string>(keywords))
                   .empty());
        expect(find_forbidden("", kimix::span<const kimix::string>(keywords))
                   .empty());
    };

    "forbidden_message_wording"_test = [] {
        expect(forbidden_message("git  push") ==
               kix("Command `git  push` is forbidden by config rule."));
        expect(shell_not_supported_message() ==
               kix(" This tool does not support shell commands; use the "
                   "`bash` tool."));
    };

    // -----------------------------------------------------------------------
    // parse_params
    // -----------------------------------------------------------------------
    "run_params_defaults"_test = [] {
        ToolParams params;
        params.values["command"] = ValueElement::make_string(kix("git status"));
        run_params out;
        expect(!parse_params(&params, out).failed());
        expect(out.command == kix("git status"));
        expect(out.mode == kix("execute"));
        expect(!out.shell);
        expect(out.timeout_seconds == 30_i);
        expect(!out.run_in_background);
        expect(!out.task_id.has_value());
        expect(!out.max_lines.has_value());
    };

    "run_params_cmd_alias"_test = [] {
        ToolParams params;
        params.values["cmd"] = ValueElement::make_string(kix("ls"));
        run_params out;
        expect(!parse_params(&params, out).failed());
        expect(out.command == kix("ls"));
    };

    "run_params_workdir_alias"_test = [] {
        ToolParams params;
        params.values["command"] = ValueElement::make_string(kix("ls"));
        params.values["workdir"] = ValueElement::make_string(kix("/tmp"));
        run_params out;
        expect(!parse_params(&params, out).failed());
        expect(out.cwd.has_value());
        expect(*out.cwd == kix("/tmp"));
    };

    "run_params_infer_send_from_task_id"_test = [] {
        ToolParams params;
        params.values["command"] = ValueElement::make_string(kix("exit"));
        params.values["task_id"] = ValueElement::make_string(kix("run_bash"));
        run_params out;
        expect(!parse_params(&params, out).failed());
        expect(out.mode == kix("send"));
    };

    "run_params_mode_aliases"_test = [] {
        ToolParams params;
        params.values["command"] = ValueElement::make_string(kix("x"));
        params.values["task_id"] = ValueElement::make_string(kix("t"));
        params.values["mode"] = ValueElement::make_string(kix("background"));
        run_params out;
        expect(!parse_params(&params, out).failed());
        expect(out.mode == kix("send"));
    };

    "run_params_empty_command_execute"_test = [] {
        ToolParams params;
        run_params out;
        const tool_error err = parse_params(&params, out);
        expect(err.failed());
        expect(err.message ==
               kix("command cannot be empty when mode='execute'"));
    };

    "run_params_send_without_task_id"_test = [] {
        ToolParams params;
        params.values["command"] = ValueElement::make_string(kix("x"));
        params.values["mode"] = ValueElement::make_string(kix("send"));
        run_params out;
        const tool_error err = parse_params(&params, out);
        expect(err.failed());
        expect(err.message == kix("mode='send' requires task_id to identify "
                                  "the target session"));
    };

    "run_params_send_without_command"_test = [] {
        ToolParams params;
        params.values["mode"] = ValueElement::make_string(kix("send"));
        params.values["task_id"] = ValueElement::make_string(kix("t"));
        run_params out;
        const tool_error err = parse_params(&params, out);
        expect(err.failed());
        expect(err.message == kix("command cannot be empty when mode='send'"));
    };

    "run_params_timeout_bounds"_test = [] {
        ToolParams params;
        params.values["command"] = ValueElement::make_string(kix("x"));
        params.values["timeout"] = ValueElement::make_int(901);
        run_params out;
        const tool_error err = parse_params(&params, out);
        expect(err.failed());
        expect(sv_of(err.message).find("less than or equal to 900") !=
               std::string::npos);
        params.values["timeout"] = ValueElement::make_int(0);
        const tool_error err2 = parse_params(&params, out);
        expect(err2.failed());
        expect(sv_of(err2.message).find("greater than or equal to 1") !=
               std::string::npos);
    };

    "run_params_max_lines_minimum"_test = [] {
        ToolParams params;
        params.values["command"] = ValueElement::make_string(kix("x"));
        params.values["max_lines"] = ValueElement::make_int(2);
        run_params out;
        const tool_error err = parse_params(&params, out);
        expect(err.failed());
        expect(sv_of(err.message).find("greater than or equal to 3") !=
               std::string::npos);
        params.values["max_lines"] = ValueElement::make_int(3);
        expect(!parse_params(&params, out).failed());
        expect(out.max_lines.has_value());
        expect(*out.max_lines == 3_i);
    };

    "run_params_env_string_and_list"_test = [] {
        ToolParams params;
        params.values["command"] = ValueElement::make_string(kix("x"));
        params.values["env"] = ValueElement::make_string(kix("A=1"));
        run_params out;
        expect(!parse_params(&params, out).failed());
        expect(out.has_env);
        expect(out.env_is_string);
        expect(out.env_string == kix("A=1"));

        ValueElement::Array arr;
        arr.push_back(ValueElement::make_string(kix("B=2")));
        params.values["env"] = ValueElement::make_array(std::move(arr));
        expect(!parse_params(&params, out).failed());
        expect(out.has_env);
        expect(!out.env_is_string);
        expect(out.env_list.size() == 1u);
        expect(out.env_list[0] == kix("B=2"));
    };

    // -----------------------------------------------------------------------
    // build_display_command
    // -----------------------------------------------------------------------
    "display_command_joins_with_shlex_quote"_test = [] {
        const kimix::vector<kimix::string> args = {kix("-c"), kix("print(1)")};
        const display_command d = build_display_command(
            "python", kimix::span<const kimix::string>(args), false);
        expect(d.command == kix("python -c 'print(1)'"));
        expect(!d.rtk_rewritten);
    };

    "display_command_clips_long_args"_test = [] {
        const kimix::string long_arg(150, 'a');
        const kimix::vector<kimix::string> args = {long_arg};
        const display_command d = build_display_command(
            "tool", kimix::span<const kimix::string>(args), false);
        expect(sv_of(d.command).find(kimix::string(100, 'a').c_str()) !=
               std::string::npos);
        expect(sv_of(d.command).find("...") != std::string::npos);
        expect(sv_of(d.command).find(kimix::string(101, 'a').c_str()) ==
               std::string::npos);
    };

    "display_command_culls_huge_commands"_test = [] {
        // Each argument is clipped to 100 chars + "..." first, so the
        // 10000-char threshold needs many arguments (200 * ~104 > 10000).
        kimix::vector<kimix::string> args;
        for (int i = 0; i < 200; ++i) {
            args.push_back(kimix::string(120, 'b'));
        }
        const display_command d = build_display_command(
            "tool", kimix::span<const kimix::string>(args), false);
        expect(d.command == kix("tool"));
        // Below the threshold the full joined command is kept.
        kimix::vector<kimix::string> few(5, kimix::string(120, 'b'));
        const display_command short_cmd = build_display_command(
            "tool", kimix::span<const kimix::string>(few), false);
        expect(short_cmd.command != kix("tool"));
        expect(short_cmd.command.size() > 100u);
    };

    "display_command_rtk_label"_test = [] {
        const kimix::vector<kimix::string> args = {kix("status")};
        const display_command d = build_display_command(
            "git", kimix::span<const kimix::string>(args), true);
        expect(d.rtk_rewritten);
        expect(d.command == kix("rtk status"));
    };

    // -----------------------------------------------------------------------
    // Run Tool wrapper
    // -----------------------------------------------------------------------
    "run_tool_rejects_shell_syntax_without_shell_flag"_test = [] {
        Session session; // native_io == false
        Run tool(&session);
        ToolParams params;
        // `cd` is a shell builtin, not a resolvable executable.
        params.values["command"] =
            ValueElement::make_string(kix("cd /tmp && ls"));
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find("does not support shell commands") !=
               std::string::npos);
    };

    "run_tool_hardline_floor"_test = [] {
        Session session;
        Run tool(&session);
        ToolParams params;
        params.values["command"] =
            ValueElement::make_string(kix("rm -rf /"));
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find("Blocked (hardline)") != std::string::npos);
    };

    "run_tool_hardline_can_be_disabled"_test = [] {
        Session session;
        run_config cfg;
        cfg.hardline_enabled = false;
        Run tool(&session, cfg);
        ToolParams params;
        params.values["command"] =
            ValueElement::make_string(kix("rm -rf /"));
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find("Blocked (hardline)") == std::string::npos);
    };

    "run_tool_invalid_workdir"_test = [] {
        Session session;
        Run tool(&session);
        ToolParams params;
        params.values["command"] = ValueElement::make_string(kix("git status"));
        params.values["cwd"] = ValueElement::make_string(kix("/tmp;rm"));
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find("Invalid workdir: character ';'") !=
               std::string::npos);
    };

    "run_tool_forbidden_command"_test = [] {
        Session session;
        run_config cfg;
        cfg.forbidden_keywords = normalize_forbidden(
            kimix::span<const kimix::string>());
        const kimix::vector<kimix::string> raw = {kix("git push")};
        cfg.forbidden_keywords = normalize_forbidden(
            kimix::span<const kimix::string>(raw));
        Run tool(&session, cfg);
        ToolParams params;
        params.values["command"] =
            ValueElement::make_string(kix("git   push origin main"));
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find("is forbidden by config rule") !=
               std::string::npos);
    };

    "run_tool_invalid_params"_test = [] {
        Session session;
        Run tool(&session);
        ToolParams params; // no command
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find(
                   "command cannot be empty when mode='execute'") !=
               std::string::npos);
        expect(sv_of(json).find("invalid_input") != std::string::npos);
    };

    "run_tool_send_requires_existing_task"_test = [] {
        Session session;
        session.native_io = true;
        Run tool(&session);
        ToolParams params;
        params.values["command"] = ValueElement::make_string(kix("exit"));
        params.values["task_id"] =
            ValueElement::make_string(kix("run_nonexistent"));
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find("not found") != std::string::npos);
    };

    "run_tool_prepared_command_for_shell_mode"_test = [] {
        Session session; // native_io false -> pure prepare path
        Run tool(&session);
        ToolParams params;
        params.values["command"] =
            ValueElement::make_string(kix("ls -la | head"));
        params.values["shell"] = ValueElement::make_bool(true);
        params.values["cwd"] = ValueElement::make_string(kix("/tmp"));
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
#ifdef KIMIX_PLATFORM_WINDOWS
        expect(sv_of(json).find("cd '/tmp'; ls -la | head") !=
               std::string::npos);
#else
        expect(sv_of(json).find("cd /tmp && ls -la | head") !=
               std::string::npos);
#endif
    };

    // -----------------------------------------------------------------------
    // Real end-to-end native execution (skipped without native_io)
    // -----------------------------------------------------------------------
    "run_tool_native_echo"_test = [] {
        Session session;
        session.native_io = true;
        Run tool(&session);
        ToolParams params;
#ifdef KIMIX_PLATFORM_WINDOWS
        params.values["command"] = ValueElement::make_string(kix("cmd /c echo hello_run_tool"));
#else
        params.values["command"] =
            ValueElement::make_string(kix("echo hello_run_tool"));
#endif
        params.values["timeout"] = ValueElement::make_int(30);
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find("hello_run_tool") != std::string::npos)
            << "native run should capture stdout";
    };
}
