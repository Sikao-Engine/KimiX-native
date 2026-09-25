// run_tool.h - C++ port of the kimi-agent `Run` tool (direct process
// execution without a shell).
//
// Python source of truth (C:/dev/kimi-agent/src/kimix/tools/file/run.py):
//   _HUGE_CMD_THRESHOLD                       66
//   _cd_prefix                                70-88
//   find_bash                                 90-136
//   RunParams (command|cmd, mode, shell, timeout, output_path, cwd|workdir,
//              env, run_in_background, task_id, wait_for_pattern, max_lines)
//                                             138-179
//   RunParams._infer_mode                     160-166
//   RunParams._validate_cmd                   168-179
//   Run.name / description                    181-186
//   Run.__init__ forbidden-command normalize  196-215
//   Run._hardline_blocked                     216-236
//   Run.__call__                              237-...
//     shlex.split(command, posix=use_posix)   258
//     progressive-prefix executable resolve   270-292
//     forbidden keyword check                 296-310
//     is_process / bare-python resolution     318-343
//     "This tool does not support shell commands; use the `bash` tool."
//     display command (shlex.join + huge-cmd cull)
//     env parsing (string shlex form / list form)
//     ProcessTask start / background / wait_for_pattern / timeout
//   Run._run_via_shell                        (bash|pwsh delegation)
//   Run._compile_pattern / _continue_session / _process_output /
//   Run._format_session_result
// Also:
//   tools/prompt_common.py timeout_field (default 30, ge=1, le=900),
//                          max_lines_field (ge=3), mode_field, task_id_field,
//                          wait_for_pattern_field
//   tools/common.py _interactive_scope_text(is_shell=False)  1632-1635
//   tools/security.py validate_workdir (+ the shim's _WORKDIR_ALLOWED set)
//   CPython shlex.split / shlex.quote / shlex.join
//
// Design notes (project conventions):
// * namespace kimix::builtin_tools::run; TU-local helpers use the `rn_` prefix
//   (kimix-llm builds every src/builtin_tools/*.cpp as one unity TU).
// * kimix:: containers only; no RTTI; kernels never throw across the tool
//   boundary - failures are data (tool_error / run_result).
// * The parsing/quoting kernels are pure and injectable-probe based
//   (`is_file_probe`) so unit tests need no fixtures. Real spawning goes
//   through builtin_tools/process_runner.h (reproc) and only happens when
//   Session::native_io is set.
// * Ownership: `validate_workdir` and the shlex family are re-ported here
//   because runtime/tools/security.cpp is NOT linked into kimix-llm (only
//   shell_scanner.cpp and shell_safety.cpp are). The hardline floor and
//   annotate_failure DO come from kimix::runtime::tools (shell_safety.h), the
//   same arrangement pwsh_tool.cpp already uses.
// * Exit-code semantics, truncation and the session output block are reused
//   from the bash/python tools (bash::interpret_exit_code,
//   bash::is_expected_exit, bash::truncate_lines,
//   python::build_session_output_block) - never re-implemented.
#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "builtin_tools/tool.h"
#include "builtin_tools/tool_types.h"

namespace kimix::builtin_tools::run {

// Shared vocabulary (builtin_tools/tool_types.h) - reuse, never re-declare.
using kimix::builtin_tools::tool_error;
using kimix::builtin_tools::tool_status;
using kimix::builtin_tools::ToolParams;
using kimix::builtin_tools::ValueElement;

// run.py 66: above this many characters the displayed command is culled to
// just the executable.
inline constexpr size_t k_huge_cmd_threshold = 10000;
// prompt_common.timeout_field(): default 30, ge=1, le=900.
inline constexpr int64_t k_default_timeout_seconds = 30;
inline constexpr int64_t k_min_timeout_seconds = 1;
inline constexpr int64_t k_max_timeout_seconds = 900;
// prompt_common.max_lines_field(): ge=3.
inline constexpr int64_t k_min_max_lines = 3;
// run.py: a `python -c` payload longer than this is offloaded to a script file.
inline constexpr size_t k_inline_python_limit = 30000;
// run.py / swarm: prompts and outputs above 100 KiB are offloaded.
inline constexpr size_t k_large_output_chars = 65536;

// ---------------------------------------------------------------------------
// 1. shlex family (CPython shlex module, whitespace_split=True)
// ---------------------------------------------------------------------------
// shlex.quote(s): "''" for the empty string, `s` unchanged when every
// character is in [\w@%+=:,./-] (ASCII \w), else the single-quoted form with
// every "'" expanded to '"'"'.
kimix::string shlex_quote(kimix::string_view text);

// shlex.join(argv) == " ".join(shlex_quote(a) for a in argv).
kimix::string shlex_join(kimix::span<const kimix::string> argv);

// shlex.split(s, posix=...). Port of shlex.read_token with
// whitespace_split=True, commenters='' and punctuation_chars disabled:
// * posix=true  - quotes are removed, '\' escapes the next character outside
//   quotes, and inside double quotes only '\' and '"' are escapable (a
//   backslash before anything else survives literally). A closing quote
//   continues the token (state 'a'), so `x="1 2"` is ONE token `x=1 2`.
// * posix=false - quotes are KEPT in the token and only start a quoted
//   section at the beginning of a token; '\' is an ordinary character.
// Returns false and fills `error` with the CPython ValueError wording
// ("No closing quotation" / "No escaped character") on malformed input.
bool shlex_split(kimix::string_view text, bool posix,
                 kimix::vector<kimix::string> &out, kimix::string &error);

// Convenience: posix split that reports failures through tool_error.
tool_error shlex_split_tool(kimix::string_view text, bool posix,
                            kimix::vector<kimix::string> &out);

// ---------------------------------------------------------------------------
// 2. Command-line decomposition (run.py 258-292)
// ---------------------------------------------------------------------------
// Strip one pair of surrounding double quotes (only meaningful for the
// posix=false split, which keeps them).
kimix::string strip_outer_double_quotes(kimix::string_view arg);

// Existence probe injected by the caller (Python: Path(candidate).is_file()).
using is_file_probe = kimix::function<bool(kimix::string_view)>;

struct resolved_command {
    kimix::string executable;
    kimix::vector<kimix::string> args;
    // Number of leading split tokens consumed by the executable (>1 means an
    // unquoted path containing spaces was glued back together).
    size_t consumed_tokens = 1;
};

// run.py 270-292: the first token is the executable; when the command has
// more than one token, progressively longer space-joined prefixes are tested
// with `is_file` so unquoted paths with spaces resolve correctly. The chosen
// prefix has its surrounding double quotes stripped in non-posix mode, and so
// does every remaining argument.
resolved_command resolve_executable(kimix::span<const kimix::string> parts,
                                    bool posix, const is_file_probe &is_file);

// run.py 318-343: decide whether the resolved executable can actually be
// spawned. A path containing a separator must be an existing file; a bare name
// must be resolvable on PATH. `python`/`python.exe` with no PATH hit falls
// back to the interpreter path supplied in `python_fallback` (Python:
// sys.executable).
struct executable_check {
    bool is_process = false;
    bool is_python_fallback = false;
    kimix::string executable; // possibly rewritten to python_fallback
};
executable_check check_executable(kimix::string_view executable,
                                  const is_file_probe &is_file,
                                  kimix::string_view path_env,
                                  kimix::string_view python_fallback);

// PATH lookup equivalent to shutil.which for a bare command name: scans the
// separator-split PATH for `<dir>/<name>`, trying the PATHEXT suffixes on
// Windows. Returns the resolved path or "".
kimix::string which(kimix::string_view name, kimix::string_view path_env,
                    kimix::string_view pathext_env,
                    const is_file_probe &is_file);

// ---------------------------------------------------------------------------
// 3. Environment handling (run.py env parsing)
// ---------------------------------------------------------------------------
// Parse the `env` parameter. String form: shlex-split, then re-join the
// `A = B` triple into `A=B`; every other token is kept as-is. List form: the
// items are used verbatim. Each resulting item becomes KEY=VALUE (split on the
// first '=') or KEY=1 when it contains no '='.
struct env_parse_result {
    kimix::vector<kimix::builtin_tools::named_value> values;
    tool_error error;
};
// `is_string` selects the string branch (shlex) from the list branch.
env_parse_result parse_env(kimix::string_view text, bool is_string,
                           kimix::span<const kimix::string> list_form,
                           bool posix);

// common.py _env_with_rg_bin_path is Python-side (it prepends the shared bin
// dir); the native equivalent is this explicit "KEY=VALUE" list builder used
// by proc::run_options::extra_env.
kimix::vector<kimix::string> env_to_extra_env(
    kimix::span<const kimix::builtin_tools::named_value> values);

// ---------------------------------------------------------------------------
// 4. Shell delegation (run.py _cd_prefix 70-88)
// ---------------------------------------------------------------------------
// "cd <quoted-cwd> && " for shell == "bash", "cd '<cwd with '' doubled>'; "
// for shell == "pwsh", "" when cwd is empty.
kimix::string cd_prefix(kimix::string_view cwd, kimix::string_view shell);

// ---------------------------------------------------------------------------
// 5. Safety floors
// ---------------------------------------------------------------------------
// tools/security.py validate_workdir: None when safe, else
// "Invalid workdir: character {c!r} is not allowed." for the first offending
// character. Allowed set: A-Za-z0-9 plus " _.-\\/:~". Empty input is safe.
kimix::optional<kimix::string> validate_workdir(kimix::string_view workdir);

// Python repr() of a single code point, for the message above.
kimix::string py_repr_char(uint32_t code_point);

// run.py 196-215: normalize each configured forbidden command with
// " ".join(cmd.split()) (whitespace collapse), drop empties and non-strings,
// and dedupe preserving first-seen order.
kimix::vector<kimix::string> normalize_forbidden(
    kimix::span<const kimix::string> raw);

// run.py 305-310: the whitespace-collapsed command containing any normalized
// keyword is forbidden. Returns the matching keyword ("" when none).
kimix::string find_forbidden(kimix::string_view command,
                             kimix::span<const kimix::string> keywords);

// "Command `{full_cmd}` is forbidden by config rule."
kimix::string forbidden_message(kimix::string_view full_command);

// run.py 338-342 (the reference message starts with a space).
kimix::string shell_not_supported_message();

// ---------------------------------------------------------------------------
// 6. Parameters
// ---------------------------------------------------------------------------
struct run_params {
    kimix::string command; // `command` (alias `cmd`)
    kimix::string mode = "execute"; // "execute" | "send"
    bool shell = false; // true -> delegate to bash/pwsh
    int64_t timeout_seconds = k_default_timeout_seconds;
    kimix::optional<kimix::string> output_path;
    kimix::optional<kimix::string> cwd; // `cwd` (alias `workdir`)
    // env: either a shlex string or a list of "K=V" items.
    bool env_is_string = false;
    kimix::string env_string;
    kimix::vector<kimix::string> env_list;
    bool has_env = false;
    bool run_in_background = false;
    kimix::optional<kimix::string> task_id;
    kimix::optional<kimix::string> wait_for_pattern;
    kimix::optional<int64_t> max_lines;
};

// RunParams + _infer_mode + _validate_cmd. Byte-exact pydantic/ValueError
// wording on failure:
// * "command cannot be empty when mode='execute'"
// * "command cannot be empty when mode='send'"
// * "mode='send' requires task_id to identify the target session"
// * "task_id requires mode='send'"
tool_error parse_params(const ToolParams *params, run_params &out);

// ---------------------------------------------------------------------------
// 6b. Output shaping (common.py _token_filter_output, portable stages)
// ---------------------------------------------------------------------------
// common.py _dedup_output(output, threshold=3, max_block_lines=1) -- the single
// -line branch that `_token_filter_output` uses for shell output. Every line
// whose TOTAL occurrence count (anywhere in the output - not only consecutive
// runs) is greater than `threshold` is collapsed to its first occurrence plus
// "  (<count> repeats)"; all other lines pass through in original order.
// Lines are split with str.splitlines() semantics for the documented ASCII
// subset (LF / CRLF / CR; other Unicode terminators are the project-wide ASCII
// gate) and re-joined with '\n', which also drops the trailing terminator.
// NOTE: the reference's `threshold=3` collapses at count >= 4, and the marker
// carries the TOTAL count - not the run length (that is the
// output_utils.dedup_lines flavour used by other tools).
kimix::string dedup_output(kimix::string_view output,
                           int64_t threshold = 3);

// The portable stages of `_token_filter_output` in reference order:
//   apply_dedup = token_kill && !rtk_rewritten
//   if (!apply_dedup && !max_lines) -> unchanged
//   1. (rich ANSI strip + micro_compress - NOT ported, see the report)
//   2. apply_dedup -> dedup_output()
//   3. max_lines  -> bash::truncate_lines(output, max_lines, true, 2)
// `changed` mirrors the reference's `output != original_output` test, which is
// what decides whether the original stream is exported to a temp file.
struct shaped_output {
    kimix::string text;
    bool changed = false;
};
shaped_output shape_output(kimix::string_view output,
                           const kimix::optional<int64_t> &max_lines,
                           bool token_kill, bool rtk_rewritten);

// ---------------------------------------------------------------------------
// 6c. Result messages (run.py 557-559 / 586-587)
// ---------------------------------------------------------------------------
// ToolOk message: `success` (or `[rtk] success` for an rtk rewrite) when the
// exit code was 0, otherwise the exit-code meaning, or "expected non-zero
// exit" when the non-zero code is the normal outcome for the command (grep
// with no match, diff with differences, a SIGPIPE-truncated pipeline).
kimix::string success_message(bool success, bool rtk_rewritten,
                              const kimix::optional<kimix::string> &meaning);

// ToolError message: `failed` (or `[rtk] failed`), plus ` Hint: <hint>` when
// annotate_failure produced one. The ` [original saved to ...]` suffix the
// reference appends is a Python-side temp-file concern (see the report).
kimix::string failure_message(bool rtk_rewritten,
                              const kimix::optional<kimix::string> &hint);

// ---------------------------------------------------------------------------
// 7. Display command (run.py display_executable / display_args / cmd_str)
// ---------------------------------------------------------------------------
struct display_command {
    kimix::string command; // what the UI/LLM sees
    bool rtk_rewritten = false;
};
// Each argument longer than 100 characters is clipped to `arg[:100] + "..."`,
// then shlex.join([executable] + args); when the joined text exceeds
// k_huge_cmd_threshold only the executable is shown.
display_command build_display_command(kimix::string_view executable,
                                      kimix::span<const kimix::string> args,
                                      bool rtk_rewritten);

// ---------------------------------------------------------------------------
// 8. Tool class
// ---------------------------------------------------------------------------
// Configuration owned by the host (mirrors Run.__init__ reading
// session.custom_config["config_json"]).
struct run_config {
    bool hardline_enabled = true; // shell.hardline (default true)
    bool redact_secrets = true; // shell.redact_secrets (default true)
    bool native_execute = true; // spawn through reproc when native_io
    kimix::vector<kimix::string> forbidden_keywords; // already normalized
    kimix::string python_exe; // "" == auto-detect (Python: sys.executable)
    kimix::string bash_path; // "" == auto-detect (shell=true on POSIX)
    kimix::string pwsh_path; // "" == auto-detect (shell=true on Windows)
    // Python-side callbacks (optional; "" / nullopt == feature off).
    kimix::function<kimix::string(kimix::string_view)> redact_output;
    // Called with the resolved executable's stem once the RTK gate matched a
    // command rtk knows (`bash::is_known_rtk_command`); returns the absolute
    // rtk binary path (None/nullopt == no rewrite). Mirrors run.py's
    // `_rtk_binary_path()` probe + `_is_known_rtk_command(Path(exe).stem)`.
    kimix::function<kimix::optional<kimix::string>(kimix::string_view)>
        run_rtk_check;
};

class Run : public kimix::builtin_tools::Tool {
public:
    explicit Run(kimix::builtin_tools::Session *session, run_config cfg);
    // Registry-friendly constructor: default config.
    explicit Run(kimix::builtin_tools::Session *session);
    // Always valid: the tool spawns the process directly through the vendored
    // reproc runner, so no external program has to be installed. The modes
    // that delegate to a shell (shell=true, the cwd prefix) report their own
    // "no shell available" error when neither bash nor pwsh exists - and the
    // soul keeps at least one of those two shells offered (see
    // KimiSoul::effective_shell_tool).
    bool valid() const override;

    void operator()(const ToolParams *parameters) override;
    kimix::vector<char> const &serialized_result() const { return _result; }
    void result_json(kimix::vector<char> &out) const override { out = _result; }

    // Synchronous kernel entry (no spawning): validates the params and the
    // safety floors and fills `output_block` with the command that WOULD run.
    // Used by the Python mirror and by unit tests.
    tool_error prepare(const run_params &params, kimix::string &output_block);

    run_config &config() { return _cfg; }
    const run_config &config() const { return _cfg; }

    // Platform shell detection (run.py find_bash / USE_SYSTEM_PWSH_ON_WINDOWS).
    static kimix::string detect_bash_path();
    static kimix::string detect_pwsh_path();

private:
    run_config _cfg;
    kimix::vector<char> _result;
};

} // namespace kimix::builtin_tools::run
