// bash_tool.h - C++ port of the kimi-agent bash tool's remaining pure kernels.
//
// Plan: C:/dev/kimi-agent/plans/bash.md ("Plan: Rewrite bash to C++"), sections
// 3.1 (exit-code semantics), 3.3 (output truncation with error preservation),
// 3.4 (optional RTK rewrite scanner), plus the bounded-run capture/timeout/kill
// policy state machine (AGENT_TASK.md scope item: a pure state machine fed with
// streamed chunks + elapsed ms; NOT a real subprocess spawn).
//
// Source of truth (C:/dev/kimi-agent):
//   src/kimix/tools/file/bash/output_enhance.py
//     _has_top_level_pipe (57-99), _is_expected_exit_py (102-116),
//     interpret_exit_code (119-157), is_expected_exit (160-176),
//     _base_command_name (41-54)
//   src/kimix/tools/common.py
//     _ERROR_KEYWORDS (334-345, 36 entries) + _ERROR_PATTERN (347-350),
//     _find_error_line_index (353-358),
//     _truncate_lines (1100-1164),
//     _RTK_KNOWN_COMMANDS (363-439), _PREFIX_SKIP (1420),
//     _ASSIGNMENT_RE (1422), _split_shell_segments (1268-1347),
//     _read_shell_word (1350-1415), _rewrite_shell_segment (1429-1466),
//     _maybe_rewrite_shell_command_with_rtk (1469-1538),
//     quote helpers _find_ansi_c_end (1167-1179), _find_backtick_end
//     (1182-1194), _find_dq_end (1197-1224), _find_matching_paren (1227-1265)
//   src/kimix/tools/background/utils.py
//     DEFAULT_INACTIVITY_TIMEOUT (30), BACKGROUND_MAX_OUTPUT_CHARS (35),
//     bounded_append (42-76), get_output drain (213-229),
//     wait_for_output (307-369), wait_with_inactivity_timeout (253-305)
//   src/kimix/tools/common.py ProcessStream completion banner (2113-2126):
//     "[Process exited with code {rc}, error at line {n}]" /
//     "[Process exited with code {rc}]"
//
// Unicode parity. Every decision here is ASCII-only: `.lower()` on the command
// name uses ASCII lowering (lower_ascii), the \b word boundaries of
// _ERROR_PATTERN are ASCII word boundaries, and str.splitlines() reduces to
// splitting on LF / CRLF / CR (the only terminators filter_output can emit).
// Callers route non-ASCII input to the pure-Python mirror, matching the
// project-wide ASCII-gate convention (see src/runtime/tools/shell_safety.h).
//
// Ownership notes (src/builtin_tools/README.md "Cross-tool ownership"):
//   * truncate_lines / find_error_line_index / error_keywords are OWNED by this
//     tool - the python agent must not declare them.
//   * detect_self_kill / self_kill_hint are owned by pwsh; they are NOT
//     implemented here.
//
// Namespace: kimix::builtin_tools::bash. kimix-llm builds with a unity (jumbo)
// batch, so every symbol of this tool lives inside this namespace and internal
// helpers have internal linkage with bash-specific names.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "builtin_tools/tool_types.h"

namespace kimix::builtin_tools::bash {

// Shared vocabulary (builtin_tools/tool_types.h) - reuse, never re-declare.
using kimix::builtin_tools::tool_error;
using kimix::builtin_tools::tool_status;

// ---------------------------------------------------------------------------
// Exit-code semantics (plans/bash.md 3.1)
// ---------------------------------------------------------------------------

// output_enhance.py _has_top_level_pipe (57-99): true when *command* contains a
// top-level `|` pipeline operator. Quote- and substitution-aware: pipes inside
// '...', "...", `...`, or (...) subshells are ignored, backslash escapes are
// honoured, and the `||` logical-OR operator is not a pipeline. Used to decide
// whether exit 141 is the normal SIGPIPE consequence of `producer | head`.
bool has_top_level_pipe(kimix::string_view command);

// output_enhance.py _base_command_name (41-54): first non-assignment command
// word of the last `;`/`|`/`&&`/`||` segment, directory-stripped, ".exe"
// suffix removed (case-insensitive). Returns "" when no word remains.
// (Re-ported locally because kimix-llm cannot link the runtime_py copy in
// src/runtime/tools/shell_safety.h.)
kimix::string base_command_name(kimix::string_view command);

// output_enhance.py interpret_exit_code (119-157): explain a non-zero exit
// code for well-known commands; empty optional for exit 0 / None-equivalent /
// unknown codes. Rule order matches the reference exactly: SIGPIPE (141 with a
// top-level pipe) FIRST, then grep/egrep/fgrep/rg/ag/ack exit 1,
// diff/colordiff exit 1, find exit 1, test/[ exit 1, curl 6/7/22/28, git exit 1.
kimix::optional<kimix::string> interpret_exit_code(kimix::string_view command,
                                                   kimix::optional<int64_t> exit_code);

// output_enhance.py is_expected_exit (160-176) + _is_expected_exit_py
// (102-116): true when *exit_code* is the normal, expected outcome of
// *command* (grep "no matches", diff "files differ", test false, find partial
// results, or SIGPIPE 141 inside a pipeline).
bool is_expected_exit(kimix::string_view command, kimix::optional<int64_t> exit_code);

// ---------------------------------------------------------------------------
// Output truncation with error preservation (plans/bash.md 3.3)
// ---------------------------------------------------------------------------

// common.py _ERROR_KEYWORDS (334-345) - the exact 36-entry table in reference
// order. Owned by this tool (see ownership note above).
extern const kimix::string_view error_keywords[];
inline constexpr size_t error_keyword_count = 36;

// common.py _find_error_line_index (353-358) + _ERROR_PATTERN (347-350):
// 1-based index of the first line containing any error keyword, matched with
// ASCII word boundaries (\b) and ASCII case folding; nullopt when no line
// matches. Lines are split exactly like Python str.splitlines() on LF / CRLF /
// CR (the only terminators that survive filter_output). Owned by this tool.
kimix::optional<int64_t> find_error_line_index(kimix::string_view output);

// common.py _truncate_lines (1100-1164), byte-exact:
//   * empty output or max_lines <= 0 -> output unchanged
//   * n <= max_lines -> output unchanged
//   * head_n = max_lines // 2, tail_n = max_lines - head_n - 1
//   * fold marker "\n\n[... {omitted} lines omitted{note} ...]\n\n"
//   * preserve_errors: when the first error line (1-based) falls inside the
//     omitted region [head_n, n - tail_n), keep
//     lines[max(head_n, e-ctx) : min(n-tail_n, e+ctx+1)] before the marker and
//     append the note " ({k} error-context line(s) preserved)".
// Owned by this tool.
kimix::string truncate_lines(kimix::string_view output, int64_t max_lines,
                             bool preserve_errors = true,
                             int64_t error_context_lines = 2);

// ---------------------------------------------------------------------------
// RTK command rewrite scanner (plans/bash.md 3.4)
// ---------------------------------------------------------------------------

// One segment of a shell command line: the text plus the separator that
// followed it (`;`, `&&`, `||`, or "" for the final segment). A single `|` or
// `&` stays inside the segment text, exactly like common.py
// _split_shell_segments (1268-1347).
struct shell_segment {
    kimix::string text;
    kimix::string sep;
};

// common.py _split_shell_segments (1268-1347): split a shell command on `;`,
// `&&`, `||` (single `|` and `&` stay inside the segment). Quoted regions
// ('...', "...", $'...', `...`) and $(...) command substitutions are protected
// so separators inside them do not create spurious segments. Always returns at
// least one segment (empty command -> one empty segment).
void split_shell_segments(kimix::string_view command,
                          kimix::vector<shell_segment> &out);

// common.py _RTK_KNOWN_COMMANDS (363-439): true when the lowercase name (with
// an optional ".exe" suffix stripped, case-insensitive) is one of the RTK
// top-level commands. The table mirrors the reference exactly, `find`
// intentionally excluded (see the reference comment).
bool is_known_rtk_command(kimix::string_view name);

// common.py _rewrite_shell_segment (1429-1466): rewrite the leftmost command of
// a single segment by inserting the rtk prefix ("rtk ", or "& rtk " when
// *pwsh*) before the first real executable token. Assignments and the prefix
// modifiers sudo/time/nohup/nice are skipped; RTK_DISABLED=1, an existing rtk
// executable (with or without path / .exe / surrounding quotes) and unknown
// commands leave the segment unchanged. Returns (new_segment, did_rewrite).
struct rewrite_result {
    kimix::string segment;
    bool changed = false;
};
rewrite_result rewrite_shell_segment(kimix::string_view segment,
                                     bool exclude_read, bool pwsh = false);

// common.py _maybe_rewrite_shell_command_with_rtk (1469-1538) decision kernel:
// returns (rewritten_command, did_rewrite). `rtk_available` injects the
// Python-side _rtk_available()/_rtk_binary_path() gates (the kernel stays
// pure), and `rtk_binary_path` injects the absolute-path fast path (empty ==
// unknown -> skipped, matching rtk_path is None). Multi-segment commands (a
// top-level `;`/`&&`/`||`) are never rewritten (rtk cannot guarantee
// newline-terminated output; the reference falls back to the local dedup
// pipeline).
rewrite_result maybe_rewrite_shell_command_with_rtk(kimix::string_view command,
                                                    bool token_kill,
                                                    bool rtk_available,
                                                    kimix::string_view rtk_binary_path,
                                                    bool exclude_read = false,
                                                    bool pwsh = false);

// ---------------------------------------------------------------------------
// Bounded-run capture/timeout/kill policy state machine (AGENT_TASK.md scope)
// ---------------------------------------------------------------------------
// Pure decision kernel for the bounded "run and capture" loop: the caller feeds
// streamed output chunks and elapsed-time events; the machine decides when to
// keep waiting, stop early, or kill the process. It never spawns a process.
//
// The vendored kimix-reproc library (#include <reproc/reproc.h>) is available
// for a follow-up phase that moves the actual spawn into C++:
//   reproc_process / REPROC_PROCESS options (input/output/stop actions,
//   REPROC_REDIRECT, REPROC_DEADLINE, REPROC_TIMEOUT), reproc_start,
//   reproc_poll (REPROC_DEADLINE / REPROC_TIMEOUT event sources),
//   reproc_read (stdout/stderr drains), reproc_wait / reproc_stop
//   (REPROC_STOP / REPROC_TERMINATE / REPROC_KILL stop actions), plus the C++
//   wrapper reproc::process (reproc++/reproc.hpp). See reports/bash.md.
//
// Event ordering mirrors background/utils.py wait_for_output (307-369) and
// get_output (213-229):
//   1. feed every buffered chunk (bounded append to the capture buffer),
//   2. test the wait pattern against the FULL accumulated output,
//   3. a process-exit event performs one final drain before the machine stops,
//   4. the total-timeout check runs before the inactivity check, and the
//      inactivity timer only runs when inactivity_timeout_ms > 0.

// One input event for the state machine. `elapsed_ms` is the wall time since
// the run started (monotonic), injected by the caller.
struct capture_event {
    enum class kind : uint8_t {
        chunk,          // data arrived (text carries it; non-empty by contract)
        process_exited, // the process finished; exit_code carries its code
    };
    kind type = kind::chunk;
    kimix::string text;                // chunk payload (kind::chunk only)
    kimix::optional<int64_t> exit_code; // kind::process_exited only
    int64_t elapsed_ms = 0;            // monotonic ms since the run started
};

// Decision returned for every event.
struct capture_decision {
    enum class action : uint8_t {
        wait,          // keep capturing
        pattern_stop,  // the wait pattern matched: stop without killing
        complete_stop, // the process exited on its own: final drain done
        timeout_kill,  // total timeout exceeded: kill the process tree
        inactivity_stop // output stalled: stop early (process stays running)
    };
    action act = action::wait;
    bool matched = false;     // true only when the pattern matched before timeout
    bool truncated = false;   // a bounded-append truncation has happened so far
    int64_t elapsed_ms = 0;   // elapsed time echoed from the event
};

// Configuration of one bounded run. All timeouts are milliseconds; 0 disables
// the corresponding bound, exactly like the Python reference (timeout<=0 ->
// return after the current drain; inactivity disabled when not > 0).
struct capture_config {
    int64_t timeout_ms = 30000;          // total foreground timeout
    int64_t inactivity_timeout_ms = 0;   // 0 == disabled
    int64_t output_cap_chars = 200000;   // BACKGROUND_MAX_OUTPUT_CHARS
    kimix::string wait_pattern;          // empty == no pattern
};

// Pure state machine. Construct one per run; call on_event() for each event in
// arrival order. Never throws for well-formed input (allocation failure only).
class capture_machine {
  public:
    capture_machine();
    explicit capture_machine(capture_config config);

    // Process one event and return the decision for it (see the ordering note
    // above). After a stop decision the machine is finished; further calls keep
    // returning the stop decision (idempotent) so callers can drain races.
    capture_decision on_event(const capture_event &event);

    const kimix::string &output() const;      // accumulated (bounded) output
    kimix::optional<int64_t> exit_code() const; // set by process_exited
    bool matched() const;                     // pattern matched before timeout
    bool truncated() const;                   // bounded-append truncation seen
    bool finished() const;                    // a stop decision was issued
    int64_t last_output_elapsed_ms() const;   // monotonic ms of the last chunk

  private:
    void bounded_append_chunk(kimix::string_view text);
    bool pattern_matches() const;

    capture_config config_;
    kimix::string output_;
    kimix::optional<int64_t> exit_code_;
    bool matched_ = false;
    bool truncated_ = false;
    bool finished_ = false;
    bool inactivity_armed_ = false;      // inactivity_timeout_ms > 0
    int64_t last_output_elapsed_ms_ = 0; // last chunk arrival (monotonic ms)
    capture_decision last_stop_;         // replayed once finished
};

// Bounded append mirroring background/utils.py bounded_append (42-76): appends
// *text* to *content*; when the result exceeds *cap* chars it is rewritten to
// head int(cap*0.4) + marker + tail (cap - head), and `truncated` is set.
// Character-based (code points) like the Python reference. Returns the new
// content.
kimix::string bounded_append_capture(kimix::string_view content,
                                     kimix::string_view text, int64_t cap,
                                     bool &truncated);

// common.py ProcessStream completion banner (2113-2126): the line the stream
// appends when a foreground process exits non-zero. error_line is the 1-based
// find_error_line_index of the full output (nullopt when none). Byte-exact:
//   "\n[Process exited with code {rc}, error at line {n}]"
//   "\n[Process exited with code {rc}]"
kimix::string process_exited_banner(int64_t exit_code,
                                    kimix::optional<int64_t> error_line);

} // namespace kimix::builtin_tools::bash
