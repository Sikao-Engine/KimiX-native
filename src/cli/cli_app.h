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

#include <cstdint>
#include <cstdio>

#include <core/kimix_core.h>

#include "agent/soul.h"
#include "cli/cli_args.h"
#include "cli/cli_config.h"
#include "cli/cli_session.h"
#include "cli/cli_stream.h"
#include "llm/llm.h"

namespace kimix::cli {

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
    // Borrowed scripted-input queue + streams: the REPL owns them and publishes
    // them here so the command handlers can share the reference's `_input`
    // (pending queue first, then a line from `input`, prompted on `output`).
    kimix::vector<kimix::string> *pending = nullptr;
    std::FILE *input = nullptr;
    std::FILE *output = nullptr;
    kimix::agent::IChatBackend *injected = nullptr; // borrowed (tests)
    bool initialized = false;  // configs loaded (--dry-run leaves it sessionless)
    bool session_closed = false; // /exit already saved + closed the session
    bool title_locked = false;   // custom_title no longer derived from input
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
bool app_run_prompt(app_context &app, kimix::string_view input);

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

// The reference REPL prompt: "\n>>>>>>>>> Enter your prompt or command:\n".
kimix::string app_prompt_line();

// ---------------------------------------------------------------------------
// S5 additions shared with cli_commands.cpp / cli_repl.cpp
// ---------------------------------------------------------------------------
// `_input(prompt, text_arr)`: pop the pending queue first (printing nothing),
// otherwise print `prompt` to app.output and read one line from app.input.
// Returns false on EOF (the REPL's "\nbye." path) or when no input is available.
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

} // namespace kimix::cli
