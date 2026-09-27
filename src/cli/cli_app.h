// cli/cli_app.h - The native CLI's application layer: config -> agent wiring,
// one-turn execution, compaction, the --dry-run report and the process entry
// point.  This is src/cli/PLAN.md §3.7's frozen interface plus the documented
// S5 additions listed at the bottom of this header.
//
// Port map (reference, read-only: C:/dev/kimi-agent/src/kimix):
//   * kimix/cli_impl/core.py::_run_cli      -> cli_main
//   * kimix/cli_impl/core.py::_client_cli   -> repl_run (cli_repl.cpp)
//   * kimix/utils/__init__.py::prompt       -> app_run_prompt
//   * kimix/utils/session.py::print_usage / _print_usage -> app_usage_text /
//     the per-turn banner
//   * kimix/utils/session.py::compact_default_context    -> app_compact
//   * kimi_cli/config.py + soul/agent.py (provider/agent -> LLM + tool list)
//     -> app_init
//   * the `--dry-run` report (a native addition) -> app_dry_run_report
// Every documented deviation of this layer is listed in
// src/cli/reports/cli_commands.md.
//
// Rules (see src/cli/PLAN.md §3 and .agents/skills/cpp): namespace kimix::cli,
// exception-free (throw/try/catch are build errors - failures travel through
// bool + `error` out-parameters), no RTTI, kimix:: containers/strings in every
// public API, K&R braces, 4-space indent, fixed-width ints, unity build (batch
// 8) with TU-local helpers in an anonymous namespace under the `cliapp_`
// prefix, and no file-scope `using namespace`.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>

#include <core/kimix_core.h>

#include "agent/approval.h"
#include "agent/cancel.h"
#include "agent/soul.h"
#include "agent/wire.h"
#include "cli/cli_args.h"
#include "cli/cli_config.h"
#include "cli/cli_session.h"
#include "cli/cli_skills.h"
#include "cli/cli_stream.h"
#include "llm/llm.h"
#include "cli/cli_stream.h"
#include "llm/llm.h"

namespace kimix::cli {

struct app_context; // declared below; cli_input_queue only borrows a pointer.

// One pending approval prompt answer (G1): while the turn blocks inside the
// approval gate, the REPL reader thread routes the next typed line here
// instead of steering the turn. The blocked approver callback waits on `cv`.
struct approval_answer_slot {
    std::mutex mutex;
    std::condition_variable cv;
    kimix::string line;
    bool answered = false;
};

// stdin reader thread state (cli_repl.cpp owns the thread; cli_app.cpp's
// app_read_input waits on it): finished lines queue here; at EOF `eof`
// latches.  While `app->steering` is set, the reader routes lines straight to
// the running soul as interrupting steers instead of the queue; while the
// turn blocks inside the approval gate they go to `approval_slot`.  Sharing
// this queue with the command handlers removes the second fgetc reader on
// app.input: there is exactly one consumer of stdin, so multi-line
// terminators like /end and /cancel are never raced between two readers.
struct cli_input_queue {
    std::mutex mutex;
    std::condition_variable cv;
    kimix::deque<kimix::string> lines;
    bool eof = false;
    std::atomic<bool> stop{false}; // latched by the REPL owner at teardown
    app_context *app = nullptr;    // borrowed (steering flag + soul target)
};

// Wait until a line (or EOF) is available, a Ctrl-C arrives, or `stop` fires.
// Returns true when a line was dequeued.
bool cli_input_next_line(cli_input_queue &queue, kimix::string &line,
                         std::atomic<bool> &stop);

// I7: one row of the in-process session cache (kimix/utils/_globals.py
// _cli_sessions, commands.py:361-404): a session this process created or
// resumed.  /sessions renders this cache (not a filesystem scan).
struct cli_session_row {
    kimix::string id;
    kimix::string title;    // "" renders as "Untitled"
    int64_t updated_at = 0; // unix seconds (Session.updated_at)
    double context_usage = 0.0;
    int64_t context_tokens = 0;
    bool usage_known = false; // false renders the "-" usage cell
};

// The whole CLI state of one process run (PLAN.md §3.7).
struct app_context {
    cli_options opts;
    provider_config provider;
    agent_config agent;
    session_store store;
    session_state state;
    kimix::unique_ptr<kimix::llm::LLM> llm;
    kimix::unique_ptr<kimix::agent::LLMBackend> backend;
    kimix::unique_ptr<kimix::agent::AgentSession> session;
    kimix::unique_ptr<kimix::agent::KimiSoul> soul;
    stream_renderer *renderer = nullptr; // borrowed, one per turn
    int32_t exit_code = 0;

    // --- S5 additions (documented in src/cli/reports/cli_commands.md) -------
    kimix::string work_dir;      // resolved working dir (opts.work_dir else cwd)
    kimix::string provider_path; // resolved provider config path ("" == none)
    kimix::string agent_path;    // resolved manifest path ("" == built-in default)
    // The soul options app_init resolved (system prompt / tools / limits); kept
    // so /resume, /store and /load can rebuild the soul for a new session.
    kimix::agent::KimiSoul::options soul_options;
    // Startup skill discovery (auto + .kimix/skill.json + -s/--skill-dir):
    // the resolved roots and the scope-grouped system-prompt block the soul
    // options consume.
    skill_bundle skills;
    // Borrowed scripted-input queue + streams: the REPL owns them and publishes
    // them here so the command handlers can share the reference's `_input`
    // (pending queue first, then a line from `input`, prompted on `output`).
    kimix::vector<kimix::string> *pending = nullptr;
    std::FILE *input = nullptr;
    std::FILE *output = nullptr;
    kimix::agent::IChatBackend *injected = nullptr; // borrowed (tests)
    bool initialized = false;  // configs loaded (--dry-run leaves it sessionless)
    bool session_closed = false; // /exit already saved + closed the session
    bool title_locked = false; // custom_title no longer derived from input

    // --- Phase 3 (G7/G8/B7) -------------------------------------------------
    // G8: the per-turn cancellation token. The Ctrl-C handler (cli_signal)
    // stores into its raw flag; a running turn polls it through the token and
    // aborts at the next step boundary, interrupting the in-flight request.
    kimix::agent::CancelToken cancel;
    // G7: while a turn runs, the REPL reader thread routes typed lines here
    // as request_steer() calls instead of queueing them as the next prompt.
    std::atomic<bool> steering{false};
    // B7: the live wire.jsonl event stream of the open session (append-only;
    // save_history no longer regenerates the file). Owned so the soul's raw
    // sink pointer stays valid; detached before the soul is destroyed.
    kimix::unique_ptr<kimix::agent::WireWriter> wire;

    // --- Phase 3 part 2 (G1-G4 approval runtime) -----------------------------
    // The approval gate: owns the session's ApprovalState (yolo / persisted
    // afk / invocation-only runtime_afk / the approve-for-session grant set).
    // Created in app_init after the session state loads, kept across soul
    // rebinds so grants and afk survive /resume, installed on every soul.
    kimix::unique_ptr<kimix::agent::Approval> approval;
    // While the turn blocks inside the gate, the REPL reader thread routes
    // typed lines here instead of steering (see approval_answer_slot). Null
    // between prompts.
    std::atomic<approval_answer_slot *> approval_slot{nullptr};
    // Borrowed; the REPL reader queue (exactly one stdin consumer): command
    // handlers blocking on app_read_input are fed from it (published by
    // repl_run, cleared at teardown). Null outside the REPL.
    cli_input_queue *input_queue = nullptr;

    // --- H/I gap-closure additions ------------------------------------------
    // I7: the in-process session cache (the reference's _globals._cli_sessions).
    kimix::vector<cli_session_row> cli_sessions;
    // I3: runtime.current_prompt - the (possibly transformed) prompt string of
    // the running/last prompt, mirrored into the todo tool's session state.
    kimix::string current_prompt;
};

// Resolve the provider + agent configs, build the LLM (or the injected backend)
// and open the first (anonymous) session.  When `injected != nullptr` that
// backend is used instead of creating one from the provider config - the same
// seam KimiSoul itself exposes, which makes the REPL/command layer testable
// without a network.  `opts.dry_run` stops after the configs + LLM are resolved
// (no session directory is created, nothing is sent).  Returns false with
// `error` set; never prints and never exits.
bool app_init(const cli_options &opts, app_context &app, kimix::string &error,
              kimix::agent::IChatBackend *injected = nullptr);

// One user turn: stream it through app.renderer, persist state.json /
// context.jsonl / wire.jsonl plus the context usage and print the reference's
// "Finished, context usage: ...  time: H:MM:SS" banner.  A failed turn prints
// the error through print_error and returns false (the REPL stays alive).
//
// I3/I8: `error` (optional) receives the failure text so callers can print
// the reference's "Prompt failed: {e}" wording.  The full prompt() wrapper:
// strip + escape_file_paths, the >64 KB "read and execute: `{file}`" temp-file
// rule, runtime.current_prompt tracking, the cli_closing_reminder_rounds
// todo-review loop ("Todo review..." / "Final todo review...") and the
// post-prompt todo clearing.
bool app_run_prompt(app_context &app, kimix::string_view input,
                    kimix::string *error = nullptr);

// KimiSoul::compact_context + the reference's
// "Context usage from A to B  time: H:MM:SS" line (green/bold).  The caller
// prints "Start compacting..." (cmd_compact).
bool app_compact(app_context &app, kimix::string_view instruction);

// The --dry-run text: "LLMConfig: ..." + provider_report + agent_report + "OK".
// Deterministic; never prints an api_key value.
kimix::string app_dry_run_report(const app_context &app);

// Entry point used by main() and by the tests.  Exit codes: 0 ok, 1 config,
// 2 usage, 3 unsupported subcommand, 4 runtime failure (see exit_code).
int cli_main(int argc, char **argv);

// Resolve a provider/agent config path the way the reference's
// kimix/utils/config.py::_load_config_file does: the path itself first, then
// the cwd and each parent directory (matched by leaf name), then the
// executable directory and its parents, then each PATH entry.  Returns ""
// when nothing matched.  `exe_dir` may be "" to skip the executable walk.
kimix::string resolve_config_path(const kimix::string &given, const kimix::string &exe_dir);

// The reference REPL prompt: "\n>>>>>>>>> Enter your prompt or command:\n".
kimix::string app_prompt_line();

// ---------------------------------------------------------------------------
// S5 additions shared with cli_commands.cpp / cli_repl.cpp
// ---------------------------------------------------------------------------
// `_input(prompt, text_arr)`: pop the pending queue first (printing nothing),
// otherwise, when the REPL published its reader queue (app.input_queue), wait
// on it - the reader thread owns stdin and every command-handler prompt is fed
// from its lines, so multi-line terminators like /end and /cancel are never
// raced between two fgetc readers; Ctrl-C and the queue's EOF latch end the
// wait (both close the multi-line block).  Otherwise (non-REPL/test callers)
// print `prompt` to app.output and read one line from app.input.  Returns
// false on EOF (the REPL's "\nbye." path) or when no input is available.
bool app_read_input(app_context &app, kimix::string_view prompt, kimix::string &line);

// Close the current session store and open `id` (`resume` == reopen an existing
// directory; "" == a fresh anonymous id), reload state.json / context.jsonl and
// rebuild the AgentSession + KimiSoul bound to the new store.  This is what
// /resume, /store, /load, /sessions:<name> and /clear use.
bool app_open_session(app_context &app, kimix::string_view id, bool resume,
                      kimix::string &error);

// Persist the session: state.json (session_state fields + the todo list from
// todo::session_todos serialized into `todos_json`), context.jsonl, wire.jsonl
// and the store's recorded usage.
bool app_save_session(app_context &app, kimix::string &error);

// Context usage: `tokens` = soul->estimated_tokens(), `ratio` = tokens / the
// provider's max_context_size (the backend's when the config has none).
void app_usage(const app_context &app, double &ratio, int64_t &tokens);

// "P% (T tokens)" - the /context and /compact number format
// (stream::percentage_and_token).
kimix::string app_usage_text(const app_context &app);

// Run one turn in an isolated anonymous session (the reduced /swarm and
// /supervisor): `agent` supplies the tools/prompt, `swarm_enabled` mirrors the
// reference's custom_data['is_swarm_session'].  The current session, store and
// soul are left untouched and the temporary session directory is deleted.
bool app_run_isolated(app_context &app, const agent_config &agent, bool swarm_enabled,
                      kimix::string_view input, kimix::string &error);

// (Re)build app.session + app.soul over the store's current directory, loading
// the persisted todo state + history.  Used by app_init/app_open_session and by
// /clear (which keeps the same id/directory after dropping the context).
bool app_rebind_session(app_context &app, kimix::string &error);

// I1: one turn of an ARBITRARY (isolated) session/soul pair - the /plan
// planner sub-session's generation/revision turns.  Streams through
// app.renderer, prints the cyan `label` line and the per-turn banner; the
// caller owns the session/soul lifetimes.
bool app_run_isolated_turn(app_context &app, kimix::agent::AgentSession &session,
                           kimix::agent::KimiSoul &soul, kimix::string_view input,
                           kimix::string_view label);

// I7: insert/refresh the current session's row in app.cli_sessions (the
// reference's _add_cli_session bookkeeping: id, title, updated_at=now, usage).
void app_touch_cli_session(app_context &app);

// I3: prompt_str.py escape_file_paths - wrap plausible file paths in the text
// in backticks (paths already inside quotes/backticks are left alone; URLs are
// ignored).  The sanitising half of the reference (NFKC/emoji/whitespace) is a
// documented reduction: the native CLI only applies the path escaping.
kimix::string escape_file_paths(kimix::string_view text);

} // namespace kimix::cli
