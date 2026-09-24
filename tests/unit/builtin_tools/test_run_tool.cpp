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
#include <runtime/tools/shell_safety.h>

#include "builtin_tools/bash_tool.h"
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

    "run_params_timeout_clamps_and_coerces"_test = [] {
        // Reference semantics (kosong's argument repair: lax coercion +
        // _clamp_numeric_value), verified live in python/tests/test_parity_run.py
        // and against the real tool in kimi-agent's tests/test_run.py:
        //   timeout: 0 -> 1, 1000 -> 900, "45" -> 45, 7.9 -> error.
        struct case_t {
            int kind; // 0 = int, 1 = real, 2 = bool, 3 = string
            int64_t ival;
            double rval;
            bool bval;
            const char *sval;
            bool ok;
            int64_t value;
        };
        static const case_t cases[] = {
            {0, 0, 0, false, "", true, 1},
            {0, -5, 0, false, "", true, 1},
            {0, 1, 0, false, "", true, 1},
            {0, 900, 0, false, "", true, 900},
            {0, 901, 0, false, "", true, 900},
            {0, 1000, 0, false, "", true, 900},
            {1, 0, 8.0, false, "", true, 8},
            {1, 0, 7.9, false, "", false, 0},
            {1, 0, 1e9, false, "", true, 900},
            {2, 0, 0, true, "", true, 1},
            {2, 0, 0, false, "", false, 0},
            {3, 0, 0, false, "45", true, 45},
            {3, 0, 0, false, " 45 ", true, 45},
            {3, 0, 0, false, "+45", true, 45},
            {3, 0, 0, false, "1_0", true, 10},
            {3, 0, 0, false, "0", false, 0},
            {3, 0, 0, false, "abc", false, 0},
        };
        for (const case_t &c : cases) {
            ToolParams params;
            params.values["command"] = ValueElement::make_string(kix("x"));
            switch (c.kind) {
            case 0:
                params.values["timeout"] = ValueElement::make_int(c.ival);
                break;
            case 1:
                params.values["timeout"] = ValueElement::make_real(c.rval);
                break;
            case 2:
                params.values["timeout"] = ValueElement::make_bool(c.bval);
                break;
            default:
                params.values["timeout"] = ValueElement::make_string(kix(c.sval));
                break;
            }
            run_params out;
            const tool_error err = parse_params(&params, out);
            expect(err.failed() == !c.ok) << "timeout case " << c.kind << "/"
                                          << c.ival << c.sval;
            if (c.ok) {
                expect(out.timeout_seconds == c.value)
                    << "timeout should be " << c.value;
            }
        }
    };

    "run_params_max_lines_clamps"_test = [] {
        ToolParams params;
        params.values["command"] = ValueElement::make_string(kix("x"));
        params.values["max_lines"] = ValueElement::make_int(2);
        run_params out;
        expect(!parse_params(&params, out).failed());
        expect(out.max_lines.has_value() && *out.max_lines == 3_i)
            << "max_lines: 2 clamps to 3 (ge=3), it is not an error";
        params.values["max_lines"] = ValueElement::make_int(0);
        expect(!parse_params(&params, out).failed());
        expect(out.max_lines.has_value() && *out.max_lines == 3_i);
        params.values["max_lines"] = ValueElement::make_string(kix("7"));
        expect(!parse_params(&params, out).failed());
        expect(out.max_lines.has_value() && *out.max_lines == 7_i);
        params.values["max_lines"] = ValueElement::make_real(4.5);
        expect(parse_params(&params, out).failed());
        params.values["max_lines"] = ValueElement::make_int(3);
        expect(!parse_params(&params, out).failed());
        expect(out.max_lines.has_value() && *out.max_lines == 3_i);
    };

    "run_params_bool_coercion"_test = [] {
        // pydantic lax bool: ints/floats by truthiness, plus the string table.
        struct case_t {
            int kind; // 0 = bool, 1 = int, 2 = real, 3 = string
            bool bval;
            int64_t ival;
            double rval;
            const char *sval;
            bool ok;
            bool value;
        };
        static const case_t cases[] = {
            {0, true, 0, 0, "", true, true},
            {0, false, 0, 0, "", true, false},
            {1, false, 1, 0, "", true, true},
            {1, false, 0, 0, "", true, false},
            {1, false, 2, 0, "", true, true},
            {2, false, 0, 1.0, "", true, true},
            {2, false, 0, 0.0, "", true, false},
            {3, false, 0, 0, "true", true, true},
            {3, false, 0, 0, "TRUE", true, true},
            {3, false, 0, 0, "yes", true, true},
            {3, false, 0, 0, "on", true, true},
            {3, false, 0, 0, "1", true, true},
            {3, false, 0, 0, "no", true, false},
            {3, false, 0, 0, "off", true, false},
            {3, false, 0, 0, "0", true, false},
            {3, false, 0, 0, "maybe", false, false},
        };
        for (const case_t &c : cases) {
            ToolParams params;
            params.values["command"] = ValueElement::make_string(kix("x"));
            switch (c.kind) {
            case 0:
                params.values["run_in_background"] =
                    ValueElement::make_bool(c.bval);
                break;
            case 1:
                params.values["run_in_background"] =
                    ValueElement::make_int(c.ival);
                break;
            case 2:
                params.values["run_in_background"] =
                    ValueElement::make_real(c.rval);
                break;
            default:
                params.values["run_in_background"] =
                    ValueElement::make_string(kix(c.sval));
                break;
            }
            run_params out;
            const tool_error err = parse_params(&params, out);
            expect(err.failed() == !c.ok) << "bool case " << c.kind << "/"
                                          << c.sval;
            if (c.ok) {
                expect(out.run_in_background == c.value);
            }
        }
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
    // cd_prefix - goldens from run.py _cd_prefix (tests/test_run.py)
    // -----------------------------------------------------------------------
    "cd_prefix_goldens"_test = [] {
        for (const cd_golden &g : kCdGoldens) {
            expect(sv_of(cd_prefix(kix(g.cwd), kix(g.shell))) ==
                   std::string(g.expected))
                << "cd_prefix mismatch for cwd=" << g.cwd << " shell=" << g.shell;
        }
    };

    // -----------------------------------------------------------------------
    // dedup_output - the common.py _dedup_output flavour (NOT the consecutive
    // run flavour of output_utils.dedup_lines)
    // -----------------------------------------------------------------------
    "dedup_output_counts_every_occurrence"_test = [] {
        // Regression: run_tool used output_utils.dedup_lines (consecutive runs,
        // marker = run_len - 1), which for 10 identical lines produced
        // "ERROR  (9 repeats)" instead of the reference's "ERROR  (10 repeats)"
        // (run.py test_success_message_includes_original_path_after_dedup feeds
        // exactly "ERROR\n" * 10).
        expect(dedup_output(kix("ERROR\nERROR\nERROR\nERROR\nERROR\nERROR\nERROR\n"
                                "ERROR\nERROR\nERROR\n")) ==
               kix("ERROR  (10 repeats)"));
        expect(dedup_output(kix("ERROR\nERROR\nERROR\nERROR\n")) ==
               kix("ERROR  (4 repeats)"));
        // threshold=3 collapses at count > 3 only.
        expect(dedup_output(kix("ERROR\nERROR\nERROR\n")) ==
               kix("ERROR\nERROR\nERROR"));
        // Non-consecutive repeats still count (Counter, not a run scanner);
        // lines at or below the threshold keep every occurrence.
        expect(dedup_output(kix("x\ny\nx\ny\nx\ny\nx\n")) ==
               kix("x  (4 repeats)\ny\ny\ny"));
        // Low-count lines pass through untouched, in order.
        expect(dedup_output(kix("a\nb\nc")) == kix("a\nb\nc"));
        expect(dedup_output(kix("")) == kix(""));
    };

    "dedup_output_normalizes_line_endings"_test = [] {
        // _dedup_output re-joins str.splitlines() with '\n', so CRLF is
        // normalized and the trailing terminator is dropped.
        expect(dedup_output(kix("a\r\nb\r\n")) == kix("a\nb"));
        expect(dedup_output(kix("a\rb\r")) == kix("a\nb"));
        expect(dedup_output(kix("a\nb\n")) == kix("a\nb"));
        expect(dedup_output(kix("ERROR\r\nERROR\r\nERROR\r\nERROR\r\nERROR\r\n")) ==
               kix("ERROR  (5 repeats)"));
    };

    "shape_output_goldens"_test = [] {
        for (const shape_golden &g : kShapeGoldens) {
            kimix::optional<int64_t> max_lines;
            if (g.max_lines >= 0) {
                max_lines = static_cast<int64_t>(g.max_lines);
            }
            const shaped_output s =
                shape_output(kix(g.input), max_lines, /*token_kill=*/true,
                             /*rtk_rewritten=*/false);
            expect(sv_of(s.text) == std::string(g.expected))
                << "shape mismatch (max_lines=" << g.max_lines << ")";
            expect(s.changed == g.changed)
                << "changed flag mismatch (max_lines=" << g.max_lines << ")";
        }
    };

    "shape_output_rtk_skips_dedup"_test = [] {
        const kimix::string repeated = kix("ERROR\nERROR\nERROR\nERROR\n");
        // token_kill && !rtk_rewritten -- an rtk rewrite skips local dedup
        // (rtk already collapsed the repeats) but keeps the fold.
        const shaped_output rtk = shape_output(
            repeated, std::nullopt, true, /*rtk_rewritten=*/true);
        expect(rtk.text == repeated);
        expect(!rtk.changed);
        const shaped_output plain = shape_output(
            repeated, std::nullopt, true, /*rtk_rewritten=*/false);
        expect(plain.text == kix("ERROR  (4 repeats)"));
        // The fold still runs for an rtk rewrite.
        const kimix::string many =
            kix("l0\nl1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\nl10\nl11");
        const shaped_output folded =
            shape_output(many, int64_t{6}, true, true);
        expect(sv_of(folded.text).find("lines omitted") != std::string::npos);
        // No filters at all -> the input is returned verbatim.
        const shaped_output none =
            shape_output(repeated, std::nullopt, false, false);
        expect(none.text == repeated);
        expect(!none.changed);
    };

    "shape_output_preserves_error_context_in_fold"_test = [] {
        // _truncate_lines(max_lines, preserve_errors=True,
        // error_context_lines=2): a diagnostic line inside the folded-away
        // region is kept (with context) and the marker says so. The port used
        // to pass preserve_errors=false, silently hiding the first error.
        kimix::string text = "ok\n";
        for (int i = 0; i < 30; ++i) {
            text += kimix::format("l{}\n", i);
        }
        text += "error: boom\n";
        for (int i = 0; i < 30; ++i) {
            text += kimix::format("t{}\n", i);
        }
        const shaped_output s = shape_output(
            kimix::string_view(text.data(), text.size()), int64_t{6}, true,
            false);
        expect(sv_of(s.text).find("error: boom") != std::string::npos)
            << "the first diagnostic line must survive the fold";
        expect(sv_of(s.text).find("error-context line(s) preserved") !=
               std::string::npos)
            << "the fold marker must report the preserved context";
    };

    // -----------------------------------------------------------------------
    // exit-code classification + result messages
    // -----------------------------------------------------------------------
    "exit_code_goldens"_test = [] {
        for (const exit_golden &g : kExitGoldens) {
            kimix::optional<int64_t> code;
            if (g.has_code) {
                code = static_cast<int64_t>(g.exit_code);
            }
            const kimix::optional<kimix::string> meaning =
                bash::interpret_exit_code(kix(g.command), code);
            if (g.meaning == nullptr) {
                expect(!meaning.has_value()) << "meaning should be None for: "
                                             << g.command;
            } else {
                expect(meaning.has_value())
                    << "meaning missing for: " << g.command;
                if (meaning.has_value()) {
                    // NOTE: `&&` inside expect() is overloaded by boost.ut and
                    // evaluates both sides - never guard a deref that way.
                    expect(sv_of(*meaning) == std::string(g.meaning))
                        << "meaning mismatch for: " << g.command;
                }
            }
            expect(bash::is_expected_exit(kix(g.command), code) == g.expected)
                << "is_expected_exit mismatch for: " << g.command;
            const bool success = g.has_code && g.exit_code == 0;
            expect(sv_of(success_message(success, false, meaning)) ==
                   std::string(g.ok_message))
                << "ok message mismatch for: " << g.command;
            expect(sv_of(failure_message(false, std::nullopt)) ==
                   std::string(g.fail_message))
                << "fail message mismatch for: " << g.command;
        }
    };

    "rtk_result_messages"_test = [] {
        expect(success_message(true, false, std::nullopt) == kix("success"));
        expect(success_message(true, true, std::nullopt) ==
               kix("[rtk] success"));
        expect(failure_message(false, std::nullopt) == kix("failed"));
        expect(failure_message(true, std::nullopt) == kix("[rtk] failed"));
        expect(failure_message(false, kix("Hint text")) ==
               kix("failed Hint: Hint text"));
        // Expected non-zero exit: the ok message carries the meaning.
        expect(success_message(false, false, kix("No matches")) ==
               kix("No matches"));
        expect(success_message(false, false, std::nullopt) ==
               kix("expected non-zero exit"));
    };

    "annotate_failure_goldens"_test = [] {
        for (const annotate_golden &g : kAnnotateGoldens) {
            kimix::optional<int64_t> code;
            if (g.has_code) {
                code = static_cast<int64_t>(g.exit_code);
            }
            // The same kernel run_tool.cpp uses (the bash tool wraps it with
            // an extra ASCII gate of its own).
            const kimix::optional<kimix::string> hint =
                kimix::runtime::tools::annotate_failure(kix(g.output),
                                                        kix(g.command), code);
            if (g.hint == nullptr) {
                expect(!hint.has_value())
                    << "hint should be None for: " << g.output;
            } else {
                expect(hint.has_value())
                    << "hint missing for: " << g.output;
                if (hint.has_value()) {
                    expect(sv_of(*hint) == std::string(g.hint))
                        << "hint mismatch for: " << g.output;
                }
            }
        }
    };

    // -----------------------------------------------------------------------
    // RTK rewrite wiring (run.py 369-380 +
    // tests/test_run.py::test_run_prepends_rtk_for_known_command)
    // -----------------------------------------------------------------------
    "run_rtk_rewrite_for_known_command"_test = [] {
        Session session; // native_io == false -> the prepared command is the block
        run_config cfg;
        int calls = 0;
        cfg.run_rtk_check = [&calls](kimix::string_view stem)
            -> kimix::optional<kimix::string> {
            ++calls;
            if (stem == "git") {
                return kimix::string("/fake/share/bin/rtk");
            }
            return std::nullopt;
        };
        Run tool(&session, cfg);
        ToolParams params;
        params.values["command"] = ValueElement::make_string(kix("git status"));
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find("rtk git status") != std::string::npos)
            << "rtk-known commands must be displayed through rtk";
        expect(calls == 1);

        // Unknown command: the gate rejects it before the callback runs.
        ToolParams other;
#ifdef KIMIX_PLATFORM_WINDOWS
        other.values["command"] = ValueElement::make_string(kix("cmd /c exit 0"));
#else
        other.values["command"] = ValueElement::make_string(kix("sh -c 'exit 0'"));
#endif
        tool(&other);
        const kimix::string json2(tool.serialized_result().data(),
                                  tool.serialized_result().size());
        expect(sv_of(json2).find("rtk") == std::string::npos)
            << "unknown commands are never rewritten";
        expect(calls == 1) << "the rtk probe must not run for unknown commands";

        // A known command with no rtk binary installed stays untouched.
        cfg.run_rtk_check = [](kimix::string_view) {
            return kimix::optional<kimix::string>{};
        };
        Run tool2(&session, cfg);
        tool2(&params);
        const kimix::string json3(tool2.serialized_result().data(),
                                  tool2.serialized_result().size());
        expect(sv_of(json3).find("rtk") == std::string::npos)
            << "no rtk binary -> no rewrite";
        expect(sv_of(json3).find("git status") != std::string::npos);
    };

    "run_rtk_rewrite_skips_rtk_itself"_test = [] {
        Session session;
        run_config cfg;
        int calls = 0;
        cfg.run_rtk_check = [&calls](kimix::string_view)
            -> kimix::optional<kimix::string> {
            ++calls;
            return kimix::string("/fake/rtk");
        };
        Run tool(&session, cfg);
        ToolParams params;
        params.values["command"] = ValueElement::make_string(kix("rtk git status"));
        tool(&params);
        expect(calls == 0) << "an executable that IS rtk is never rewrapped";
    };

    // -----------------------------------------------------------------------
    // Live native shaping (real spawn)
    // -----------------------------------------------------------------------
    "run_tool_native_dedups_repeated_lines"_test = [] {
        Session session;
        session.native_io = true;
        Run tool(&session);
        ToolParams params;
#ifdef KIMIX_PLATFORM_WINDOWS
        params.values["command"] =
            ValueElement::make_string(kix("cmd /c \"echo x&echo x&echo x&echo x\""));
#else
        params.values["command"] = ValueElement::make_string(
            kix("printf 'x\\nx\\nx\\nx\\n'"));
#endif
        params.values["timeout"] = ValueElement::make_int(30);
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find("x  (4 repeats)") != std::string::npos)
            << "the reference dedup marker must reach the output block: "
            << json.c_str();
    };

    "run_tool_native_exit_code_message"_test = [] {
        Session session;
        session.native_io = true;
        Run tool(&session);
        ToolParams params;
#ifdef KIMIX_PLATFORM_WINDOWS
        params.values["command"] = ValueElement::make_string(kix("cmd /c exit 7"));
#else
        params.values["command"] = ValueElement::make_string(kix("sh -c 'exit 7'"));
#endif
        params.values["timeout"] = ValueElement::make_int(30);
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find("exit_code: 7") != std::string::npos)
            << "the real exit code must be reported: " << json.c_str();
        expect(sv_of(json).find("\"failed\"") != std::string::npos)
            << "a non-expected failure reports the plain `failed` message";
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
