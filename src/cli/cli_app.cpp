// cli/cli_app.cpp - The application layer (see cli_app.h for the port map).
//
// This file owns the config -> LLM/agent wiring, the one-turn execution with
// streamed rendering, the persistence of one turn's state and the process
// entry point.  It is the C++ port of kimix/cli_impl/core.py::_run_cli plus the
// kimix/utils/__init__.py::prompt() / session.py print_usage helpers.
//
// Tool calls and tool results (deviation note, see src/cli/reports/cli_commands.md):
// KimiSoul::turn() streams text / reasoning / tool-call deltas through its single
// SoulEventCallback, but it executes the tools itself and never reports the tool
// *results*.  The CLI therefore drives the renderer from that same callback
// (text, reasoning, tool-call header + argument fragments) and flushes the tool
// results lazily out of the live session history: before the first chunk of the
// next step (the soul appends the tool messages between two chat calls) and once
// more after the turn returns.  No soul.cpp behaviour was changed for this.
//
// Unity build: every TU-local helper lives in an anonymous namespace with the
// `cliapp_` prefix.

#include "cli/cli_app.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <utility>

#include "llm/yyjson_alc.h"
#include "yyjson.h"

#include "agent/agent_host.h"
#include "builtin_tools/todo_tool.h"
#include "builtin_tools/tool.h"
#include "mcp/mcp_client.h"

#include "cli/cli_common.h"
#include "cli/cli_init_wizard.h"
#include "cli/cli_print.h"
#include "cli/cli_repl.h"
#include "cli/cli_signal.h"
#include "cli/cli_tools.h"

namespace kimix::cli {

  namespace {

  // ---------------------------------------------------------------------------
  // Small helpers
  // ---------------------------------------------------------------------------

  // I8: the failing turn's error text (cliapp_run_turn stores it;
  // app_run_prompt surfaces it through its error out-param).
  kimix::string cliapp_last_turn_error;

  // Directory of the running executable (argv[0]); "" when undeterminable.
kimix::string cliapp_exe_dir(const char *argv0) {
    if (argv0 == nullptr || argv0[0] == '\0') {
        return {};
    }
    return parent_path(absolute_path(argv0));
}

  // The reference (kimix/utils/config.py::_load_config_file) resolves a config
  // path in this order:
  //   1. the path itself
  //   2. the cwd and each of its parents, matched by leaf name only
  //   3. the executable directory and each of its parents, matched by leaf name
  //   4. each PATH entry, matched by leaf name
  // Returns "" when nothing matched.  Shared by the explicit
  // --config/--provider handling and the default_config.json lookup.
  kimix::string cliapp_seek_config(const kimix::string &given, const kimix::string &exe_dir) {
      if (given.empty()) {
          return {};
      }
      if (file_exists(given)) {
          return given;
      }
      const kimix::string leaf = file_name(given);
      if (leaf.empty()) {
          return {};
      }
      // Walk `start` and its parents; stops at the filesystem root (whose
      // parent_path() returns itself).
      auto walk = [&leaf](kimix::string start) {
          while (!start.empty()) {
              const kimix::string candidate = join_path(start, leaf);
              if (file_exists(candidate)) {
                  return candidate;
              }
              const kimix::string parent = parent_path(start);
              if (parent.empty() || parent == start) {
                  break;
              }
              start = parent;
          }
          return kimix::string();
      };
      kimix::string found = walk(current_dir());
      if (!found.empty()) {
          return found;
      }
      found = walk(exe_dir);
      if (!found.empty()) {
          return found;
      }
      kimix::string path_env;
      if (get_env("PATH", path_env)) {
#if defined(KIMIX_PLATFORM_WINDOWS)
          constexpr char kSep = ';';
#else
          constexpr char kSep = ':';
#endif
          size_t begin = 0;
          while (begin <= path_env.size()) {
              const size_t end = find(path_env, kimix::string_view(&kSep, 1), begin);
              const size_t len =
                  (end == kimix::string::npos) ? path_env.size() - begin : end - begin;
              const kimix::string_view entry(path_env.data() + begin, len);
              if (end == kimix::string::npos) {
                  begin = path_env.size() + 1;
              } else {
                  begin = end + 1;
              }
              if (entry.empty()) {
                  continue;
              }
              const kimix::string candidate = join_path(kimix::string(trim(entry)), leaf);
              if (file_exists(candidate)) {
                  return candidate;
              }
          }
      }
      return {};
  }

  // The reference looks for default_config.json next to the module
  // (<repo>/src/kimix/default_config.json); the native CLI walks the cwd, the
  // executable directory and their parents, then PATH (the reference's
  // _load_config_file search).
  kimix::string cliapp_find_config(const kimix::string &exe_dir, const char *leaf) {
      return cliapp_seek_config(kimix::string(leaf), exe_dir);
  }

// ${name} substitution of an agent manifest's system_prompt_args (the
// reference's BuiltinSystemPromptArgs).  Unknown placeholders are left as-is.
kimix::string cliapp_substitute(kimix::string_view text,
                                const kimix::vector<std::pair<kimix::string, kimix::string>> &args) {
    kimix::string out(text);
    for (const std::pair<kimix::string, kimix::string> &kv : args) {
        const kimix::string needle = "${" + kv.first + "}";
        out = replace_all(out, needle, kv.second);
    }
    return out;
}

  // True when the resolved tool list contains one of the plan tools (the native
  // counterpart of the reference's `plan_writing_path` session flag).  The
  // registry keys are lowercase ("writeplan"/"readplan"/"editplan", see
  // tool_registry.h); the legacy CamelCase spellings are still accepted so
  // hand-written manifests keep working (they resolve through the registry
  // aliases anyway).
  bool cliapp_has_plan_tools(const kimix::vector<kimix::string> &tools) {
      for (const kimix::string &name : tools) {
          if (name == "writeplan" || name == "readplan" || name == "editplan" ||
              name == "WritePlan" || name == "ReadPlan" || name == "EditPlan") {
              return true;
          }
    }
    return false;
}

// The manifest's system prompt file contents (empty when unset, so the soul's
// default applies).  A relative path is already resolved by the manifest loader.
kimix::string cliapp_system_prompt(const agent_config &agent);
// The KimiSoul options app_init resolved from the provider config + manifest.
kimix::agent::KimiSoul::options cliapp_soul_options(const app_context &app,
                                                    const agent_config &agent,
                                                    bool swarm_enabled) {
      kimix::agent::KimiSoul::options opts;
      opts.system_prompt = cliapp_system_prompt(agent);
      opts.enabled_tools = agent.enabled_tools;
      opts.max_tokens = app.provider.max_tokens;
      // The parsed [loop_control] section (kimi_cli.config.LoopControl) drives
      // the loop-facing option defaults, exactly like the reference's runtime
      // reads agent.runtime.config.loop_control (app.py:205-207 overrides
      // max_steps_per_turn / max_retries_per_step the same way).
      const agent::LoopControl &lc = app.provider.loop_control;
      opts.loop_control = lc;
      opts.max_steps = static_cast<int32_t>(lc.max_steps_per_turn);
      opts.auto_compact_ratio = lc.compaction_trigger_ratio;
      opts.reserved_context = lc.reserved_context_size;
      opts.min_preserved_turns = lc.min_preserved_messages;
      opts.max_preserved_turns = lc.max_preserved_messages;
        // The tool-call buffer is the LIVE estimate the soul computes per step
        // (kimisoul.py _tool_call_buffer_tokens -> toolset.py
        // estimate_tool_output_token_budget), not the old static
        // max_tokens/4; leaving the override at 0 enables it.
        opts.tool_call_buffer_tokens = 0;
      opts.auto_compact = true;
      // Default system prompt inputs (utils/system_prompt.py port, see
      // agent/system_prompt.h): skills block from the startup discovery,
      // yolo from --no_yolo, and the shell tool the conventions name.
      opts.skills_text = app.skills.prompt_text;
      opts.yolo = !app.opts.no_yolo;
#if defined(KIMIX_PLATFORM_WINDOWS)
      opts.shell_tool = "pwsh"; // PowerShell is the native Windows shell
#else
      opts.shell_tool = "bash";
#endif
      (void)swarm_enabled; // the swarm flag lives on builtin_tools::Session
      return opts;
}

kimix::string cliapp_system_prompt(const agent_config &agent) {
    if (agent.system_prompt_path.empty()) {
        return {};
    }
    kimix::string text;
    kimix::string error;
    if (!read_file(agent.system_prompt_path, text, error)) {
        return {};
    }
    return cliapp_substitute(text, agent.system_prompt_args);
}

// ---------------------------------------------------------------------------
// G1-G4: the approval runtime (gate wiring + the interactive prompt)
// ---------------------------------------------------------------------------

// The effective yolo default (G4/G19): an explicit --no_yolo always wins; a
// config default_yolo decides when present (config.py:689: the flag feeds the
// approval default); the native historical default stays yolo-on.
bool cliapp_effective_yolo(const cli_options &opts, const provider_config &provider) {
    if (opts.no_yolo) {
        return false;
    }
    if (provider.has_default_yolo) {
        return provider.default_yolo;
    }
    return true;
}

// Block until one line lands in the approval slot, the user hits Ctrl-C, or
// `stop` fires. Returns false on cancel (the caller rejects the request; the
// running turn notices the cancel token at the next step boundary and aborts
// cleanly - the reference's ApprovalCancelledError surface).
bool cliapp_wait_answer(app_context &app, approval_answer_slot &slot,
                        kimix::string &line) {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(slot.mutex);
            if (slot.answered) {
                line = std::move(slot.line);
                // Re-arm UNDER THE SAME LOCK, before the caller processes the
                // answer: the reader thread may already hold the next typed
                // line (e.g. the rejection reason right after "n") and must
                // never find a stale answered=true that swallows it.
                slot.answered = false;
                return true;
            }
        }
        if (app.cancel.cancelled()) {
            return false; // Ctrl-C during the prompt aborts the turn
        }
        std::unique_lock<std::mutex> lock(slot.mutex);
        slot.cv.wait_for(lock, std::chrono::milliseconds(50));
    }
}

// Read one line through the REPL reader thread (routed to `slot` while the
// gate blocks). EOF / cancel reports false.
bool cliapp_prompt_line(app_context &app, kimix::string_view prompt,
                        approval_answer_slot &slot, kimix::string &line) {
    if (!prompt.empty() && app.output != nullptr) {
        std::fwrite(prompt.data(), 1, prompt.size(), app.output);
        std::fflush(app.output);
    }
    // The slot is already disarmed by cliapp_wait_answer's take, which also
    // moved the previous line out. Do NOT clear slot.line here: the reader
    // may already have delivered the next answer (e.g. the rejection reason
    // right after "n") and it must survive until the next wait consumes it.
    return cliapp_wait_answer(app, slot, line);
}

// The interactive approver callback installed on the gate (approval.py's
// runtime wait becomes this synchronous prompt). The wording composes the
// reference pieces: the tool's one-line description ("Edit file `{path}` —
// {justification}"), the action grant key, and the ACP option names
// ("Approve once" / "Approve for this session" / "Reject").
kimix::agent::ApprovalResponse cliapp_ask_approval(
    app_context &app, kimix::string_view sender, kimix::string_view action,
      kimix::string_view description, kimix::string &feedback) {
      approval_answer_slot slot;
      app.approval_slot.store(&slot, std::memory_order_release);
    struct slot_guard {
        app_context &app;
        ~slot_guard() {
            app.approval_slot.store(nullptr, std::memory_order_release);
        }
    } guard{app};

    kimix::string banner = "\nApproval requested (";
    banner.append(sender.data(), sender.size());
    banner += ")\n  ";
    banner.append(description.data(), description.size());
    banner += "\n  action: ";
    banner.append(action.data(), action.size());
    banner += "\n[y] Approve once  [a] Approve for this session  [n] Reject\n> ";
    for (;;) {
        kimix::string line;
        if (!cliapp_prompt_line(app, banner, slot, line)) {
            return kimix::agent::ApprovalResponse::reject; // cancelled
        }
        const kimix::string answer(trim(line));
        if (answer == "y" || answer == "Y" || answer == "yes") {
            return kimix::agent::ApprovalResponse::approve;
        }
        if (answer == "a" || answer == "A" || answer == "always") {
            return kimix::agent::ApprovalResponse::approve_for_session;
        }
        if (answer == "n" || answer == "N" || answer == "no") {
            // The rejection reason rides back to the model as the typed
            // feedback (approval.py:225-226); empty is allowed.
            kimix::string reason;
            if (cliapp_prompt_line(app, "Reason (optional): ", slot, reason)) {
                feedback = kimix::string(trim(reason));
            }
            return kimix::agent::ApprovalResponse::reject;
        }
        banner = "> ";
    }
}

// Build the session's approval gate after the session state loaded: seed
// yolo/afk/grants (G2), mark invocation-only afk for non-interactive runs
// (G4: -p/--prompt and --script == the reference's --print runtime_afk), and
// install the approver + the on_change persistence hook (agent.py
// _on_approval_change copies the state into session.state for the next save).
void cliapp_create_approval(app_context &app) {
    app.approval.reset(new kimix::agent::Approval());
    kimix::agent::ApprovalState &state = app.approval->state();
    state.yolo = cliapp_effective_yolo(app.opts, app.provider);
    state.afk = app.state.afk;
    for (const kimix::string &action : app.state.auto_approve_actions) {
        state.auto_approve_actions.insert(action);
    }
    if (app.opts.has_prompt || !app.opts.script_path.empty()) {
        // Non-interactive: nobody can answer a prompt (the reference's
        // runtime_afk for --print/--afk; never persisted).
        app.approval->set_runtime_afk(true);
    }
    app.approval->set_approver([&app](kimix::string_view sender,
                                      kimix::string_view action,
                                      kimix::string_view description,
                                      kimix::string &feedback) {
        return cliapp_ask_approval(app, sender, action, description, feedback);
    });
    state.on_change = [&app] {
        // Persist-through: copy the gate state into session.state so the next
        // save_state() writes it (notify_change, approval.py:76-79).
        app.state.yolo = app.approval->is_yolo();
        app.state.afk = app.approval->is_afk_flag();
        app.state.auto_approve_actions.clear();
        for (const kimix::string &action :
             app.approval->state().auto_approve_actions) {
            app.state.auto_approve_actions.push_back(action);
        }
    };
}

// Re-seed the persisted half of the gate (persisted afk + the grant set) from
// a freshly loaded session state (/resume, /load, /sessions:<name>): the
// invocation-level yolo/runtime_afk flags are kept.
void cliapp_resync_approval(app_context &app) {
    if (app.approval == nullptr) {
        return;
    }
    kimix::agent::ApprovalState &state = app.approval->state();
    state.afk = app.state.afk;
    state.auto_approve_actions.clear();
    for (const kimix::string &action : app.state.auto_approve_actions) {
        state.auto_approve_actions.insert(action);
    }
}

// The provider's context window (the backend's when the config has none).
int64_t cliapp_context_size(const app_context &app) {
    if (app.provider.max_context_size > 0) {
        return app.provider.max_context_size;
    }
    const kimix::agent::IChatBackend *chat =
        app.backend ? static_cast<const kimix::agent::IChatBackend *>(app.backend.get())
                    : app.injected;
    if (chat != nullptr) {
        const int64_t size = chat->max_context_size();
        if (size > 0) {
            return size;
        }
    }
    return 1;
}

// One parse of a tool result payload: {"status","out"|"output","message","brief"}.
// Invalid / truncated payloads keep ok=true and use the raw text as the summary.
void cliapp_tool_result_fields(kimix::string_view json, bool &ok, kimix::string &message,
                               kimix::string &summary) {
    ok = true;
    message.clear();
    summary.clear();
    if (json.empty()) {
        return;
    }
    yyjson_doc *doc =
        yyjson_read_opts(const_cast<char *>(json.data()), json.size(),
                         YYJSON_READ_STOP_WHEN_DONE, &kimix::llm::kYYJsonAlcMi, nullptr);
    if (doc == nullptr) {
        summary = kimix::string(json);
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root != nullptr && yyjson_is_obj(root)) {
        yyjson_val *status = yyjson_obj_get(root, "status");
        if (yyjson_is_str(status)) {
            const kimix::string_view value(yyjson_get_str(status),
                                           static_cast<size_t>(yyjson_get_len(status)));
            ok = (value == "ok");
        }
        yyjson_val *msg = yyjson_obj_get(root, "message");
        if (yyjson_is_str(msg)) {
            message.assign(yyjson_get_str(msg), static_cast<size_t>(yyjson_get_len(msg)));
        }
        yyjson_val *out = yyjson_obj_get(root, "output");
        if (yyjson_is_str(out)) {
            summary.assign(yyjson_get_str(out), static_cast<size_t>(yyjson_get_len(out)));
        } else {
            yyjson_val *brief = yyjson_obj_get(root, "brief");
            if (yyjson_is_str(brief)) {
                summary.assign(yyjson_get_str(brief),
                               static_cast<size_t>(yyjson_get_len(brief)));
            }
        }
    } else {
        summary = kimix::string(json);
    }
    yyjson_doc_free(doc);
}

// The fallback display line when a call carries no tool-authored one (an
// external/MCP tool, or a result the soul never recorded): the reference's
// terminal never prints the raw output either, so keep the first non-empty
// line and clamp it. `kCliSummaryMaxChars` bounds the printed line.
constexpr size_t kCliSummaryMaxChars = 200;

kimix::string cliapp_short_display(kimix::string_view text) {
    size_t begin = 0;
    while (begin < text.size() && (text[begin] == '\r' || text[begin] == '\n' ||
                                   text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    size_t end = text.find_first_of("\r\n", begin);
    const bool more_lines = (end != kimix::string_view::npos &&
                             end + 1 < text.size()); // trailing content after \r\n
    if (end == kimix::string_view::npos) {
        end = text.size();
    }
    kimix::string line = kimix::string(text.substr(begin, end - begin));
    const bool clamped = line.size() > kCliSummaryMaxChars;
    if (clamped) {
        line.resize(kCliSummaryMaxChars);
        // Never split a UTF-8 sequence: step back over continuation bytes.
        while (!line.empty() &&
               (static_cast<unsigned char>(line.back()) & 0xC0) == 0x80) {
            line.pop_back();
        }
    }
    if (clamped || more_lines) {
        line += "...";
    }
    return line;
}

// Flush every tool message appended since `rendered` to the renderer, resolving
// the tool name from the assistant message that issued the call. `soul` supplies
// the short CLI display line the tool recorded for the call (builtin_tools::
// Tool::operator()'s display_str, kept by KimiSoul per tool_call_id); a call
// with no recorded line falls back to the first line of its own summary.
void cliapp_flush_tool_results(app_context &app, kimix::agent::AgentSession &session,
                               size_t &rendered, kimix::agent::KimiSoul *soul) {
    const kimix::vector<kimix::llm::Message> &history = session.history();
    for (size_t i = rendered; i < history.size(); ++i) {
        const kimix::llm::Message &msg = history[i];
        if (msg.role != "tool") {
            continue;
        }
        kimix::string name;
        for (size_t j = i; j-- > 0;) {
            const kimix::llm::Message &prev = history[j];
            if (prev.role != "assistant") {
                continue;
            }
            for (const kimix::llm::ToolCall &call : prev.tool_calls) {
                if (call.id == msg.tool_call_id) {
                    name = call.name;
                    break;
                }
            }
            break; // only the nearest assistant message can own the tool call
        }
        bool ok = true;
        kimix::string message;
        kimix::string summary;
        cliapp_tool_result_fields(msg.content, ok, message, summary);
        // The tool's own display line wins: it is the short summary the tool
        // chose for the terminal, never the payload's full output.
        kimix::string display;
        if (soul != nullptr && soul->take_tool_display(msg.tool_call_id, display) &&
            !display.empty()) {
            summary = std::move(display);
        } else {
            summary = cliapp_short_display(summary);
        }
        if (app.renderer != nullptr) {
            app.renderer->on_tool_result(name, ok, message, summary);
        }
    }
    rendered = history.size();
}

// The chunk callback KimiSoul::turn drives: tool results of the previous step
// first (the soul appends them between two chat calls), then this chunk's own
// reasoning / text / tool-call output.
void cliapp_on_chunk(app_context &app, kimix::agent::AgentSession &session, size_t &rendered,
                     const kimix::llm::Chunk &chunk, kimix::agent::KimiSoul *soul) {
    if (app.renderer == nullptr) {
        return;
    }
    cliapp_flush_tool_results(app, session, rendered, soul);
    if (!chunk.reasoning.empty()) {
        app.renderer->on_reasoning_delta(chunk.reasoning);
    }
    if (!chunk.content.empty()) {
        app.renderer->on_text_delta(chunk.content);
    }
    for (const kimix::llm::ToolCall &call : chunk.tool_calls) {
        if (!call.name.empty()) {
            // The header printer also feeds call.arguments to the incremental
            // argument lexer, so a delta that carries both must not be fed twice.
            app.renderer->on_tool_call_begin(call);
        } else if (!call.arguments.empty()) {
            app.renderer->on_tool_call_args_delta(call.arguments);
        }
    }
}

// "P% (T tokens)" for the turn banner (soul-independent so /swarm and
// /supervisor can report their isolated session).
kimix::string cliapp_usage_text(const app_context &app, const kimix::agent::KimiSoul &soul) {
    const int64_t tokens = soul.estimated_tokens();
    const double ratio =
        static_cast<double>(tokens) / static_cast<double>(cliapp_context_size(app));
    return percentage_and_token(ratio, tokens);
}

// One turn of `soul` over `session`: stream through app.renderer, then the
// reference's "Finished, context usage: ...  time: H:MM:SS" banner.  `label`
// is the per-prompt cyan label ("Start...", "Todo review...", ...).
bool cliapp_run_turn(app_context &app, kimix::agent::AgentSession &session,
                     kimix::agent::KimiSoul &soul, kimix::string_view input,
                     kimix::string_view label = "Start...") {
    const size_t rendered_start = session.history().size();
    size_t rendered = rendered_start;
    if (app.renderer != nullptr) {
        app.renderer->reset_capture();
    }
    // The reference prints a cyan "<label>\n" before every prompt attempt
    // (utils/prompt.py _run_single_prompt label=).
    kimix::string label_line(label);
    label_line.push_back('\n');
    print_word(colorful_text(label_line, 96), true, false);
    const auto started = std::chrono::steady_clock::now();
    kimix::string turn_error;
    app.cancel.reset();
    // stream.py percentage_and_token(session): the in-turn dividers read the
    // soul's LIVE usage at print time, so bind this turn's soul as the
    // renderer's usage source (cleared below - the borrowed reference must not
    // outlive the turn).
    if (app.renderer != nullptr) {
        app.renderer->set_usage_source([&app, &soul](double &ratio, int64_t &tokens) {
            tokens = soul.estimated_tokens();
            const int64_t size = cliapp_context_size(app);
            ratio = size > 0 ? static_cast<double>(tokens) /
                                       static_cast<double>(size)
                                 : 0.0;
        });
    }
    app.steering.store(true);
        const kimix::agent::TurnResult result = soul.turn(
            input, [&app, &session, &rendered, &soul](const kimix::llm::Chunk &chunk) {
                cliapp_on_chunk(app, session, rendered, chunk, &soul);
            },
            app.cancel);
        app.steering.store(false);

        cliapp_flush_tool_results(app, session, rendered, &soul);
    if (app.renderer != nullptr) {
        app.renderer->finish_turn();
    }
    if (result.cancelled) {
        print_warning("Keyboard Interrupt.");
    } else if (!result.ok && !result.ignored) {
        print_error(result.error.empty() ? kimix::string("the turn failed") : result.error);
    }
    const int64_t elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    const kimix::string usage = cliapp_usage_text(app, soul);
    if (app.renderer != nullptr) {
        const int64_t tokens = soul.estimated_tokens();
        app.renderer->on_context_usage(
            static_cast<double>(tokens) / static_cast<double>(cliapp_context_size(app)), tokens);
        // The turn is over: drop the borrowed soul reference so a later
        // transition (never expected outside a turn) keeps the snapshot.
        app.renderer->set_usage_source(nullptr);
    }
    print_word(colorful_text("Finished, context usage: " + usage + "  time: " +
                                 format_duration_hm(elapsed) + "\n",
                             92, -1, "1"),
               true, true);
    // I8: the turn's failure text (the reference's exception message), read by
    // app_run_prompt for its "Prompt failed: {e}" callers.
    cliapp_last_turn_error = result.ok ? kimix::string() : result.error;
    return result.ok;
}

// One line from `in` (Python's input(): the trailing newline / CRLF is dropped,
// a final unterminated line is returned, EOF with nothing read reports false).
bool cliapp_read_line(std::FILE *in, kimix::string &line) {
    line.clear();
    if (in == nullptr) {
        return false;
    }
    bool any = false;
    for (;;) {
        const int ch = std::fgetc(in);
        if (ch == EOF) {
            break;
        }
        any = true;
        if (ch == '\n') {
            break;
        }
        line.push_back(static_cast<char>(ch));
    }
    if (!any) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return true;
}

// state.json's "todos" member: the array half of
// todo::serialize_state({"todos","archived_todos"}).
kimix::string cliapp_todos_array(const kimix::string &state_json) {
    yyjson_doc *doc = yyjson_read_opts(const_cast<char *>(state_json.data()), state_json.size(),
                                       YYJSON_READ_STOP_WHEN_DONE, &kimix::llm::kYYJsonAlcMi,
                                       nullptr);
    if (doc == nullptr) {
        return kimix::string("[]");
    }
    kimix::string out = "[]";
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *todos = (root != nullptr && yyjson_is_obj(root))
                            ? yyjson_obj_get(root, "todos")
                            : nullptr;
    if (todos != nullptr && yyjson_is_arr(todos)) {
        size_t len = 0;
        char *json = yyjson_val_write_opts(todos, YYJSON_WRITE_NOFLAG, &kimix::llm::kYYJsonAlcMi,
                                           &len, nullptr);
        if (json != nullptr) {
            out.assign(json, len);
            mi_free(json);
        }
    } else if (todos != nullptr) {
        out = "[]";
    }
    yyjson_doc_free(doc);
    return out;
}

// ---------------------------------------------------------------------------
// I3: the shared prompt() wrapper helpers (utils/prompt.py + prompt_str.py)
// ---------------------------------------------------------------------------

// common.py _export_to_temp_file(key=None, content): write the content under
// <cwd>/.kimix_cache/tmp_<pid>/<n>.txt and return the path.
bool cliapp_export_temp_file(app_context &app, kimix::string_view content,
                             kimix::string &path) {
    static int32_t temp_idx = 0;
    const kimix::string dir = cli_temp_dir(app.work_dir);
    kimix::string mk_error;
    if (!make_dirs(dir, mk_error)) {
        return false;
    }
    path = join_path(dir, kimix::format("{}.txt", temp_idx));
    ++temp_idx;
    kimix::string write_error;
    return write_file(path, content, write_error);
}

// prompt.py:_maybe_build_todo_reminder - the weak/strong closing reminder.
// Returns false when there is nothing pending (the reference's None).
bool cliapp_build_todo_reminder(app_context &app, bool strong, kimix::string &reminder) {
    if (app.session == nullptr) {
        return false;
    }
    const builtin_tools::todo::todo_state &todos =
        builtin_tools::todo::session_todos(app.session->tool_session());
    if (todos.todos.empty()) {
        return false;
    }
    bool all_done = true;
    for (const builtin_tools::todo::todo_item &item : todos.todos) {
        if (item.status != builtin_tools::todo::todo_status::done) {
            all_done = false;
            break;
        }
    }
    if (all_done) {
        return false;
    }
    kimix::vector<kimix::string> lines;
    // Original request context: >200 code points -> first 100 + "..." + last 100.
    kimix::string current_prompt = app.current_prompt;
    if (!current_prompt.empty()) {
        if (current_prompt.size() > 200) {
            kimix::string head = current_prompt.substr(0, 100);
            kimix::string tail = current_prompt.substr(current_prompt.size() - 100);
            current_prompt = head + "..." + tail;
        }
        lines.push_back("Original request: " + current_prompt);
        lines.push_back("");
    }
    lines.push_back(strong
                        ? "CRITICAL: Unfinished todo items remain. Mark every remaining "
                          "item `completed` with `todo_list` (mode='merge') before ending "
                          "this session. Do not declare completion or run final "
                          "verification until the todo list is empty or all entries show "
                          "`[completed]`."
                        : "You have unfinished todo items. Update statuses with `todo_list` "
                          "(mode='merge') and mark every pending/in-progress item "
                          "`completed` before finishing.");
    // The pending-item renderer (deep-first, children indented, done skipped).
    kimix::function<void(const builtin_tools::todo::todo_item &, int)> render =
        [&](const builtin_tools::todo::todo_item &item, int depth) {
            if (item.status == builtin_tools::todo::todo_status::done) {
                return;
            }
            kimix::string prefix;
            for (int i = 0; i < depth; ++i) {
                prefix += "  ";
            }
            kimix::string line = prefix + "- [" +
                                 kimix::string(builtin_tools::todo::status_name(item.status)) +
                                 "] " + item.content;
            if (item.notes.has_value() && !item.notes->empty()) {
                line += "  Notes: " + item.notes.value();
            }
            lines.push_back(line);
            for (const builtin_tools::todo::todo_item &child : item.children) {
                render(child, depth + 1);
            }
        };
    for (const builtin_tools::todo::todo_item &item : todos.todos) {
        render(item, 0);
    }
    reminder = join(lines, "\n");
    return true;
}

// prompt.py:_clear_session_todos: drop the active todo tree (the persisted
// state.json copy is rewritten by the following save).
void cliapp_clear_session_todos(app_context &app) {
    if (app.session == nullptr) {
        return;
    }
    builtin_tools::todo::todo_state &todos =
        builtin_tools::todo::session_todos(app.session->tool_session());
    todos.todos.clear();
}


// ---------------------------------------------------------------------------
// Config loading / session wiring
// ---------------------------------------------------------------------------

// Resolve the agent manifest: --agent-file wins, then the agent section of a
// combined --config document, then the built-in default agent.
bool cliapp_resolve_agent(const cli_options &opts, app_context &app, kimix::string &error) {
    if (!opts.agent_file.empty()) {
        app.agent_path = opts.agent_file;
        return load_agent_config(app.agent_path, app.agent, error);
    }
    if (!opts.config_is_provider_only && !opts.config_path.empty()) {
        kimix::string text;
        kimix::string read_error;
        if (read_file(opts.config_path, text, read_error) && contains(text, "\"agent\"")) {
            if (load_agent_config(opts.config_path, app.agent, error)) {
                app.agent_path = opts.config_path;
                return true;
            }
            error.clear(); // a combined document without a usable agent section
        }
    }
    app.agent = agent_config{};
    app.agent.extend = "default";
    app.agent.enabled_tools = default_agent_tools();
    app.agent.warnings.push_back(
        "no agent manifest found (--agent-file absent); using the built-in default agent");
    app.agent_path.clear();
    return true;
}

// Open the store's first (anonymous) session and bind an AgentSession + soul.
bool cliapp_open_first_session(app_context &app, kimix::string &error) {
    if (!app.store.open(app.work_dir, "", /*resume=*/false, error)) {
        return false;
    }
    app.state = session_state{};
    if (!app.store.load_state(app.state, error)) {
        return false;
    }
    app.title_locked = !app.state.custom_title.empty();
    // G1-G4: a resumed session's persisted afk + grant set re-seed the gate.
    cliapp_resync_approval(app);
    if (!app_rebind_session(app, error)) {
        return false;
    }
    // I7: the first row of the in-process session cache.
    app_touch_cli_session(app);
    return true;
}

  } // namespace

  // ---------------------------------------------------------------------------
  // Public seam (tests)
  // ---------------------------------------------------------------------------

  kimix::string resolve_config_path(const kimix::string &given, const kimix::string &exe_dir) {
      return cliapp_seek_config(given, exe_dir);
  }

  // ---------------------------------------------------------------------------
  // I3: prompt_str.py escape_file_paths (path-escaping half)
  // ---------------------------------------------------------------------------

  bool cliapp_is_path_char(char ch) {
      const unsigned char c = static_cast<unsigned char>(ch);
      if (c >= 'a' && c <= 'z') return true;
      if (c >= 'A' && c <= 'Z') return true;
      if (c >= '0' && c <= '9') return true;
      switch (ch) {
      case '/': case '\\': case '.': case '_': case '-':
        case '~': case ':':
          return true;
      default:
          return false;
      }
  }

  kimix::string escape_file_paths(kimix::string_view text) {
      // The reference wraps plausible file paths (a token containing a path
      // separator, made of path characters) in backticks; tokens already
      // inside backticks/quotes and URLs are left alone.  Non-path text is
      // returned unchanged.
      if (!contains(text, "/") && !contains(text, "\\")) {
          return kimix::string(text);
      }
      kimix::string out;
      out.reserve(text.size());
      size_t i = 0;
      const size_t n = text.size();
      while (i < n) {
          const char ch = text[i];
          if (ch == '`' || ch == '"' || ch == '\'') {
              // Copy a quoted/backticked span verbatim.
              const size_t close = find(text, kimix::string_view(&ch, 1), i + 1);
              const size_t stop = (close == kimix::string_view::npos) ? n : close + 1;
              out.append(text.substr(i, stop - i));
              i = stop;
              continue;
          }
          if (cliapp_is_path_char(ch) &&
              (ch == '/' || ch == '\\' || ch == '.' || ch == '~')) {
              // A path-shaped token: gather the maximal run of path chars.
              size_t start = i;
              while (start > 0 && cliapp_is_path_char(text[start - 1])) {
                  --start;
              }
              size_t end = i;
              while (end < n && cliapp_is_path_char(text[end])) {
                  ++end;
              }
              kimix::string_view token = text.substr(start, end - start);
              const bool is_url =
                  starts_with(token, "http://") || starts_with(token, "https://");
              const bool has_sep = contains(token, "/") || contains(token, "\\");
              if (!is_url && has_sep) {
                  out.push_back('`');
                  out.append(token);
                  out.push_back('`');
              } else {
                  out.append(token);
              }
              i = end;
              continue;
          }
          out.push_back(ch);
          ++i;
      }
      return out;
  }

  // ---------------------------------------------------------------------------
  // I7: the in-process session cache
  // ---------------------------------------------------------------------------

  void app_touch_cli_session(app_context &app) {
      if (app.store.id().empty()) {
          return;
      }
      cli_session_row row;
      row.id = app.store.id();
      row.title = app.state.custom_title;
      row.updated_at = now_unix_seconds();
      double ratio = 0.0;
      int64_t tokens = 0;
      app_usage(app, ratio, tokens);
      row.context_usage = ratio;
      row.context_tokens = tokens;
      row.usage_known = true;
      for (cli_session_row &existing : app.cli_sessions) {
          if (existing.id == row.id) {
              existing.title = row.title;
              existing.updated_at = row.updated_at;
              existing.context_usage = row.context_usage;
              existing.context_tokens = row.context_tokens;
              existing.usage_known = row.usage_known;
              return;
          }
      }
      app.cli_sessions.push_back(std::move(row));
  }

  // ---------------------------------------------------------------------------
  // Session re-binding (public: /clear rebuilds the session in place)
  // ---------------------------------------------------------------------------

bool app_rebind_session(app_context &app, kimix::string &error) {
    app.soul.reset();
    app.session.reset();
    if (app.approval != nullptr) {
        app.approval->set_wire_sink(nullptr); // detach before the writer dies
    }
    app.wire.reset();
    app.session.reset(new kimix::agent::AgentSession(app.work_dir));
    app.session->set_state_dir(app.store.dir());
      app.session->tool_session().session_id = app.store.id();
      app.session->tool_session().plan_enabled = cliapp_has_plan_tools(app.agent.enabled_tools);
      // Bug_tool.md item 3: plan_enabled without a plan_writing_path left the
      // plan tools answering "no plan_writing_path set". Give the session the
      // same default plan file the /plan command uses.
      if (app.session->tool_session().plan_enabled &&
          app.session->tool_session().plan_path.empty()) {
          app.session->tool_session().plan_path = cli_default_plan_path(app.work_dir);
      }
    if (!app.session->load_state(error)) {
        return false;
    }
    kimix::vector<kimix::llm::Message> history;
    if (!app.store.load_history(history, error)) {
        return false;
    }
    app.session->history() = std::move(history);
 // Feed the resumed turns into the in-memory history index so the retrieve
 // tool can resolve them (append-history equivalent for a loaded context).
 app.session->reindex_history();
    kimix::agent::IChatBackend *chat =
        app.backend ? static_cast<kimix::agent::IChatBackend *>(app.backend.get())
                    : app.injected;
    if (chat == nullptr) {
        error = "no chat backend is available (app_init created none)";
        return false;
    }
    app.soul.reset(new kimix::agent::KimiSoul(*app.session, *chat, app.soul_options));
    // context.py restore(): the persisted usage snapshot anchors the resumed
    // session's token count (readout + compaction trigger) until the next
    // provider response supersedes it.
    if (const kimix::optional<int64_t> resumed_usage = app.store.last_usage()) {
        app.soul->seed_token_ledger(*resumed_usage);
    }
 // B7: attach the live wire.jsonl event stream (<session dir>/wire.jsonl);
 // save_history no longer regenerates it. Failure is non-fatal (the turn
 // runs without a stream), matching the reference's wire-file laziness.
 app.wire.reset();
 if (!app.store.dir().empty()) {
 app.wire.reset(new kimix::agent::WireWriter());
 kimix::string wire_error;
 if (app.wire->open(join_path(app.store.dir(), "wire.jsonl"), wire_error)) {
 app.soul->set_wire_sink(app.wire.get());
 // G10: seed the request recorder's dedup sets from the existing stream so
 // a resumed session does not re-log the durable tools/prompt snapshots.
 app.soul->restore_request_recorder(app.wire->path());
 } else {
 print_debug(wire_error);
 app.wire.reset();
 }
 }
 // G1-G4: the gate rides across the rebind (its state is session-owned, the
 // soul only borrows it).
 app.soul->set_approval(app.approval.get());
 // G7: install the production sub-agent runner on this session's registry
 // (KimiSoul-backed): background Agent tool calls run real turns, steers
 // pushed via send_message are drained between steps, and interrupt_agent
 // cancels the child turn. G1-G4: the child souls share the approval gate.
 install_subagent_runner(
 kimix::builtin_tools::agents::session_registry(&app.session->tool_session()),
 *chat, app.soul_options, app.approval.get());
     app.session_closed = false;
    return true;
}

// ---------------------------------------------------------------------------
// app_init
// ---------------------------------------------------------------------------

bool app_init(const cli_options &opts, app_context &app, kimix::string &error,
              kimix::agent::IChatBackend *injected) {
    error.clear();
    app.opts = opts;
    app.injected = injected;
    app.initialized = false;
    app.work_dir = opts.work_dir.empty() ? current_dir() : absolute_path(opts.work_dir);

    if (opts.config_path.empty()) {
        error =
            "no provider config found: pass --provider PATH (or --config PATH), or place "
            "default_config.json in the working directory or next to the executable";
        return false;
    }
    app.provider_path = opts.config_path;
    if (!load_provider_config(opts.config_path, app.provider, error)) {
        return false;
    }
    if (opts.no_think) {
        // E7 (config.py:338 set_default_thinking(False) + kimi_cli/llm.py
        // create_llm thinking=False): the request itself must stop asking for
        // reasoning - to_llm_config maps enable_thinking onto the wire shape
        // the providers send.  The renderer also keeps hiding the stream
        // (show_thinking=false above).
        app.provider.enable_thinking = false;
        app.provider.reasoning_key.clear();
    }
    if (!cliapp_resolve_agent(opts, app, error)) {
        return false;
    }

    // The frozen config -> LLM bridge: to_llm_config + create_llm (no request).
    kimix::llm::Config llm_config = to_llm_config(app.provider);
    if (injected == nullptr) {
        app.llm = kimix::llm::create_llm(llm_config);
        if (app.llm == nullptr) {
            error = "failed to create an LLM for provider type '" +
                    app.provider.provider_family + "' (model='" + app.provider.model + "')";
            return false;
        }
    }
    // Skill discovery (base.py get_skill_dirs + config.py _load_skill_json /
    // init step 4): the resolved roots + the formatted prompt block are kept
    // on the context for the soul's system prompt wiring.
    app.skills = init_skill_bundle(opts.skill_dirs);

    app.soul_options = cliapp_soul_options(app, app.agent, false);

    if (opts.dry_run) {
        // No session directory, no request: --dry-run only reports.
        app.initialized = true;
        return true;
    }

    if (injected == nullptr) {
        app.backend = kimix::unique_ptr<kimix::agent::LLMBackend>(
            new kimix::agent::LLMBackend(std::move(app.llm)));
    }
    if (!cliapp_open_first_session(app, error)) {
        return false;
    }
    // G1-G4: the approval gate, seeded from the session state that just
    // loaded (grants + persisted afk survive a resume; yolo comes from the
    // flags/config, runtime_afk from the invocation mode). Attaching it to
    // the live soul here: the rebind that created the soul ran before the
    // gate existed.
    cliapp_create_approval(app);
    if (app.soul != nullptr) {
        app.soul->set_approval(app.approval.get());
    }
    app.initialized = true;
    return true;
}

// ---------------------------------------------------------------------------
// app_run_prompt / app_compact / app_usage
// ---------------------------------------------------------------------------

bool app_run_prompt(app_context &app, kimix::string_view input,
                    kimix::string *error) {
    if (error != nullptr) {
        error->clear();
    }
    if (app.session == nullptr || app.soul == nullptr) {
        if (error != nullptr) {
            *error = "no session is open";
        }
        return false;
    }
    // custom_title: the reference derives it from the first turn (or an LLM
    // title pass).  The native CLI stores the first input, truncated to 60
    // characters (documented reduction: no LLM title generation).
    if (!app.title_locked) {
        const kimix::string_view trimmed = trim(input);
        const size_t take = std::min<size_t>(60, trimmed.size());
        if (take > 0) {
            app.state.custom_title.assign(trimmed.data(), take);
        }
        app.title_locked = true;
    }
    // I3: prompt_async (utils/prompt.py:620-661) - strip + escape_file_paths,
    // the >64 KB temp-file rule and runtime.current_prompt tracking.
    kimix::string prompt_str = escape_file_paths(kimix::string(trim(input)));
    if (prompt_str.size() > 65536) {
        kimix::string temp_path;
        if (cliapp_export_temp_file(app, prompt_str, temp_path)) {
            prompt_str = "read and execute: `" + temp_path + "`";
        }
    }
    // I3: runtime.current_prompt - read back by the closing todo reminder (the
    // todo tool carries its own current_prompt through write_params, so the
    // app-level copy below feeds _maybe_build_todo_reminder's "Original
    // request" line).
    app.current_prompt = prompt_str;

    const bool ok = cliapp_run_turn(app, *app.session, *app.soul, prompt_str);
    if (!ok && error != nullptr) {
        *error = cliapp_last_turn_error.empty() ? kimix::string("the turn failed")
                                                : cliapp_last_turn_error;
    }
    // I3/G13: the closing todo-review loop (prompt.py:701-730).  Rounds come
    // from the parsed cli_closing_reminder_rounds knob (default 1; 0 off).
    if (ok) {
        const int rounds =
            static_cast<int>(app.provider.loop_control.cli_closing_reminder_rounds);
        for (int attempt = 0; attempt < rounds; ++attempt) {
            kimix::string reminder;
            if (!cliapp_build_todo_reminder(app, attempt > 0, reminder)) {
                break; // nothing pending (the reference's None reminder)
            }
            if (reminder.size() > 65536) {
                kimix::string temp_path;
                if (cliapp_export_temp_file(app, reminder, temp_path)) {
                    reminder = "read and execute: `" + temp_path + "`";
                }
            }
            const char *todo_label =
                (attempt == 0) ? "Todo review..." : "Final todo review...";
            const bool reminder_ok =
                cliapp_run_turn(app, *app.session, *app.soul, reminder, todo_label);
            if (!reminder_ok) {
                // prompt.py:723-730 - one failed review ends the loop.
                print_error("Todo reminder failed: " +
                            (cliapp_last_turn_error.empty()
                                 ? kimix::string("the turn failed")
                                 : cliapp_last_turn_error));
                break;
            }
        }
    } else {
        // prompt.py:731-732 ("prompt failed.", bold red).
        print_error("prompt failed.");
    }
    // I3: the finally block clears the session todos (prompt.py:742-745).
    cliapp_clear_session_todos(app);
    // Persist + refresh the I7 in-process cache row (the reference's
    // _add_cli_session bookkeeping after every prompt).
    kimix::string save_error;
    if (!app_save_session(app, save_error)) {
        print_error(save_error);
    }
    app_touch_cli_session(app);
    return ok;
}

bool app_compact(app_context &app, kimix::string_view instruction) {
    if (app.soul == nullptr) {
        return false;
    }
    const kimix::string before = app_usage_text(app);
    const auto started = std::chrono::steady_clock::now();
    kimix::string error;
    if (!app.soul->compact_context(instruction, error, /*manual=*/true)) {
        print_error(error.empty() ? kimix::string("compaction failed") : error);
        return false;
    }
    const kimix::string after = app_usage_text(app);
    const int64_t elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    // compact_default_context() persists the result (state.json + history).
    kimix::string save_error;
    if (!app_save_session(app, save_error)) {
        print_error(save_error);
    }
    print_word(colorful_text("Context usage from " + before + " to " + after +
                                 "  time: " + format_duration_hm(elapsed) + "\n",
                             92, -1, "1"),
               true, true);
    return true;
}

void app_usage(const app_context &app, double &ratio, int64_t &tokens) {
    tokens = app.soul != nullptr ? app.soul->estimated_tokens() : 0;
    ratio = static_cast<double>(tokens) / static_cast<double>(cliapp_context_size(app));
}

kimix::string app_usage_text(const app_context &app) {
    if (app.soul == nullptr) {
        return percentage_and_token(0.0, 0);
    }
    return cliapp_usage_text(app, *app.soul);
}

kimix::string app_dry_run_report(const app_context &app) {
    const kimix::llm::Config llm_config = to_llm_config(app.provider);
    const bool created = (app.llm != nullptr) || (app.backend != nullptr) ||
                         (app.injected != nullptr);
    kimix::string out = "LLMConfig: model=" + llm_config.model + " type=" + llm_config.type +
                        " create_llm=" + (created ? "ok" : "null");
    out += "\n";
    out += provider_report(app.provider);
    out += "\n";
    out += agent_report(app.agent);
    out += "\n";
    out += "OK";
    return out;
}

// ---------------------------------------------------------------------------
// Session lifecycle shared with the command layer
// ---------------------------------------------------------------------------

bool app_open_session(app_context &app, kimix::string_view id, bool resume,
                      kimix::string &error) {
    error.clear();
    // The soul holds a reference to the AgentSession: destroy it first.
    app.soul.reset();
    app.session.reset();
    if (app.approval != nullptr) {
        app.approval->set_wire_sink(nullptr); // detach before the writer dies
    }
    app.wire.reset();
    kimix::string close_error;
    app.store.close(/*delete_if_anonymous=*/false, close_error);
    if (!app.store.open(app.work_dir, id, resume, error)) {
        return false;
    }
    app.state = session_state{};
    if (!app.store.load_state(app.state, error)) {
        return false;
    }
    app.title_locked = !app.state.custom_title.empty();
    if (!app_rebind_session(app, error)) {
        return false;
    }
    // I7: the resumed session joins the in-process cache.
    app_touch_cli_session(app);
    return true;
}

bool app_run_isolated_turn(app_context &app, kimix::agent::AgentSession &session,
                           kimix::agent::KimiSoul &soul, kimix::string_view input,
                           kimix::string_view label) {
    return cliapp_run_turn(app, session, soul, input, label);
}

bool app_save_session(app_context &app, kimix::string &error) {
    error.clear();
    if (app.session == nullptr) {
        error = "no session is open";
        return false;
    }
    // state.json: the session_state fields + the todo tool's list (the
    // reference's SessionState.todos, written through the same file the
    // todo tool reads).
    if (app.approval != nullptr && app.approval->state().on_change) {
        // G2: flush the gate state (yolo/afk/grants) into session_state before
        // the write, exactly like the reference's notify_change persistence.
        app.approval->state().on_change();
    }
    const kimix::string todos_json = cliapp_todos_array(
        builtin_tools::todo::serialize_state(
            builtin_tools::todo::session_todos(app.session->tool_session())));
    app.state.todos_json = todos_json;
    double ratio = 0.0;
    int64_t tokens = 0;
    app_usage(app, ratio, tokens);
    app.store.set_usage(ratio, tokens, true);
    if (!app.store.save_state(app.state, error)) {
        return false;
    }
    if (!app.store.save_history(app.session->history(), error)) {
        return false;
    }
    return true;
}

bool app_run_isolated(app_context &app, const agent_config &agent, bool swarm_enabled,
                      kimix::string_view input, kimix::string &error) {
    error.clear();
    session_store store;
    if (!store.open(app.work_dir, "", /*resume=*/false, error)) {
        return false;
    }
    kimix::agent::AgentSession session(app.work_dir);
    session.set_state_dir(store.dir());
    session.tool_session().session_id = store.id();
      session.tool_session().swarm_enabled = swarm_enabled;
      session.tool_session().plan_enabled = cliapp_has_plan_tools(agent.enabled_tools);
      if (session.tool_session().plan_enabled &&
          session.tool_session().plan_path.empty()) {
          session.tool_session().plan_path = cli_default_plan_path(app.work_dir);
      }
    kimix::agent::IChatBackend *chat =
        app.backend ? static_cast<kimix::agent::IChatBackend *>(app.backend.get())
                    : app.injected;
    if (chat == nullptr) {
        error = "no chat backend is available (app_init created none)";
        kimix::string ignore;
        store.close(true, ignore);
        return false;
    }
    kimix::agent::KimiSoul soul(session, *chat, cliapp_soul_options(app, agent, swarm_enabled));
    const bool ok = cliapp_run_turn(app, session, soul, input);
    kimix::string close_error;
    store.close(/*delete_if_anonymous=*/true, close_error);
    return ok;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

kimix::string app_prompt_line() {
    return kimix::string("\n>>>>>>>>> Enter your prompt or command:\n");
}

// Wait until a line (or EOF) is available, a Ctrl-C arrives, or `stop` fires.
// Returns true when a line was dequeued.  Moved verbatim from cli_repl.cpp so
// the command handlers share the REPL reader queue instead of racing it with
// a second fgetc on the same FILE (see app_read_input).
bool cli_input_next_line(cli_input_queue &queue, kimix::string &line,
                         std::atomic<bool> &stop) {
    std::unique_lock<std::mutex> lock(queue.mutex);
    for (;;) {
        if (!queue.lines.empty()) {
            line = std::move(queue.lines.front());
            queue.lines.pop_front();
            return true;
        }
        if (queue.eof) {
            return false;
        }
        if (stop.load(std::memory_order_acquire)) {
            return false;
        }
        if (ctrlc_pending()) {
            return false;
        }
        // Timed wait: the Ctrl-C handler only stores a flag (async-signal-
        // safe), it cannot notify the condvar - polling it here keeps the
        // prompt responsive on both platforms.
        queue.cv.wait_for(lock, std::chrono::milliseconds(50));
    }
}

bool app_read_input(app_context &app, kimix::string_view prompt, kimix::string &line) {
    line.clear();
    if (app.pending != nullptr && !app.pending->empty()) {
        line = app.pending->front();
        app.pending->erase(app.pending->begin());
        return true;
    }
    if (app.input_queue != nullptr) {
        // The REPL reader thread owns stdin: the handler waits on its queue so
        // /end, /cancel and every other blocking prompt get exactly the lines
        // the user types (a raw fgetc on app.input here would race the reader
        // thread on the same FILE and randomly steal or lose lines).
        return cli_input_next_line(*app.input_queue, line, app.input_queue->stop);
    }
    if (!prompt.empty() && app.output != nullptr) {
        std::fwrite(prompt.data(), 1, prompt.size(), app.output);
        std::fflush(app.output);
    }
    return cliapp_read_line(app.input, line);
}

// ---------------------------------------------------------------------------
// cli_main
// ---------------------------------------------------------------------------

// F7 (mcp_cmd.py mcp_list): list configured MCP servers from the global
// (~/.kimi/mcp.json) and project (./.kimix/mcp.json) configs, merged with the
// project winning. Nothing is launched; this is a config listing only.
void mcp_list_command() {
    kimix::vector<kimix::string> warnings;
    kimix::vector<kimix::mcp::server_config> global;
    kimix::vector<kimix::mcp::server_config> servers;
    kimix::string home;
    if (!get_env("USERPROFILE", home) || home.empty()) {
        (void)get_env("HOME", home);
    }
    if (!home.empty()) {
        kimix::string text, error;
        const kimix::string path = join_path(join_path(home, ".kimi"), "mcp.json");
        print_info("MCP config file: " + path);
        if (read_file(path, text, error)) {
            if (!kimix::mcp::parse_mcp_config_text(text, global, warnings)) {
                print_warning("MCP config file " + path +
                              " must contain a JSON object.");
            }
            for (const kimix::string &w : warnings) {
                print_warning(w);
            }
            warnings.clear();
        }
    } else {
        print_info("MCP config file: .kimi/mcp.json");
    }
    kimix::string text, error;
    const kimix::string project_path =
        join_path(join_path(current_dir(), ".kimix"), "mcp.json");
    if (read_file(project_path, text, error)) {
        if (!kimix::mcp::parse_mcp_config_text(text, servers, warnings)) {
            print_warning("MCP config file " + project_path +
                          " must contain a JSON object.");
        }
        for (const kimix::string &w : warnings) {
            print_warning(w);
        }
    }
    const kimix::vector<kimix::mcp::server_config> merged =
        kimix::mcp::merge_server_lists(global, servers);
    if (merged.empty()) {
        print_info("No MCP servers configured.");
        return;
    }
    for (const kimix::mcp::server_config &cfg : merged) {
        kimix::string line;
        if (!cfg.command.empty()) {
            line = cfg.name + " (stdio): " + cfg.command;
            for (const kimix::string &a : cfg.args) {
                line += " " + a;
            }
        } else {
            kimix::string transport = cfg.transport;
            if (transport == "streamable-http") {
                transport = "http";
            }
            line = cfg.name + " (" + transport + "): " + cfg.url;
        }
        print_info("  " + line);
    }
}

int cli_main(int argc, char **argv) {
    cli_options opts;
    const bool parsed = parse_args(argc, argv, opts);

    // Print early so even usage errors respect --no_color.
    init_printing(opts.no_color);
    if (!parsed) {
        for (const kimix::string &error : opts.errors) {
            print_error(error);
        }
        print_string(cli_usage_line(opts.program_name));
        print_string("try '" + opts.program_name + " --help' for more information");
        flush_streams();
        uninstall_ctrlc_handler();
        return kExitUsage;
    }
    set_quiet(false);

    if (opts.help) {
        print_string(cli_help_text_extended(colorful()));
        flush_streams();
        uninstall_ctrlc_handler();
        return kExitOk;
    }
    if (opts.version) {
        print_string(kimix::string("kimix_cli ") + KIMIX_CORE_VERSION + " (kimix " +
                     KIMIX_CORE_VERSION + ")");
        flush_streams();
        uninstall_ctrlc_handler();
        return kExitOk;
    }
      if (!opts.subcommand.empty()) {
          // F7: `kimix mcp list` (mcp_cmd.py mcp_list) is native: it prints the
          // merged global + project MCP server configs without launching
          // anything. serve/test stay refused with the other Python-only
          // front ends below (I4).
          if (opts.subcommand == "mcp" &&
              (opts.subcommand_args.empty() || opts.subcommand_args[0] == "list")) {
              mcp_list_command();
              flush_streams();
              uninstall_ctrlc_handler();
              return kExitOk;
          }
          // I4: the four Python-only front ends, explicitly refused with the
          // list of what exists only in the reference CLI.  Exit code stays
          // kExitUnsupported (3).
          print_error(opts.subcommand +
                      ": not supported by the native CLI (the Python CLI's "
                      "serve/gui/ssecli/mcp front ends are Python-only)");
          print_info("Front ends implemented only in the Python CLI: serve "
                     "(HTTP/SSE server), gui (backend + Vite frontend), ssecli "
                     "(SSE debug client), mcp (MCP serve/list/test).");
          flush_streams();
          uninstall_ctrlc_handler();
          return kExitUnsupported;
      }

      // H5: args.py:26-40 _load_project_mcp_config - read ./.kimix/mcp.json on
      // every invocation and validate its shape; F7: with servers configured,
      // connect to them and bridge their tools into the registry.
      {
          kimix::vector<kimix::mcp::server_config> mcp_servers;
          const kimix::string mcp_path =
              join_path(join_path(current_dir(), ".kimix"), "mcp.json");
          if (file_exists(mcp_path)) {
              kimix::string text, read_error;
              if (!read_file(mcp_path, text, read_error)) {
                  print_warning("Failed to load MCP config file " + mcp_path + ": " +
                                read_error);
              } else {
              kimix::vector<kimix::string> mcp_warnings;
              // A file that is not JSON at all keeps the reference's
              // parse-failure wording; a JSON non-object gets the
              // must-contain wording (both from the audit row).
              yyjson_read_err parse_err;
              yyjson_doc *probe = yyjson_read_opts(
                  const_cast<char *>(text.data()), text.size(), 0,
                  &kimix::llm::kYYJsonAlcMi, &parse_err);
              if (probe == nullptr) {
                  print_warning("Failed to parse MCP config file " + mcp_path +
                                ": " +
                                (parse_err.msg != nullptr ? parse_err.msg
                                                          : "invalid JSON"));
              } else {
                  const bool is_obj = yyjson_is_obj(yyjson_doc_get_root(probe));
                  yyjson_doc_free(probe);
                  if (!is_obj ||
                      !kimix::mcp::parse_mcp_config_text(text, mcp_servers,
                                                         mcp_warnings)) {
                      print_warning("MCP config file " + mcp_path +
                                    " must contain a JSON object.");
                  }
                  for (const kimix::string &w : mcp_warnings) {
                      print_warning(w);
                  }
                  print_debug("Loaded MCP config from " + mcp_path);
              }
              }
          }
          if (!mcp_servers.empty() && !opts.dry_run) {
              // F7: connect every configured server and bridge its tools
              // (collisions skipped with the reference's message). Status is
              // debug-level, like the reference's toast/log lines.
              print_debug("connecting to mcp servers...");
              kimix::vector<kimix::string> status_lines;
              kimix::string mcp_error;
              kimix::mcp::McpManager::instance().start_all(
                  mcp_servers, status_lines, mcp_error);
              for (const kimix::string &line : status_lines) {
                  print_debug(line);
              }
          }
      }
      // F7: children and registry entries are released on every exit path
      // after this point (the guard dies with cli_main's frame, before the
      // static singletons tear down).
      struct McpShutdownGuard {
          ~McpShutdownGuard() { kimix::mcp::McpManager::instance().shutdown(); }
      } mcp_shutdown_guard;

    // H6: config.py:343-345 - the clean-mode announcement.
    if (opts.clean) {
        print_debug("Clean mode ON, delete cache file after quit.");
    }


    // Provider config resolution (the reference's default_config.json search).
    const kimix::string exe_dir = cliapp_exe_dir(argc > 0 ? argv[0] : nullptr);
    const bool explicit_config = !opts.config_path.empty();
    if (!explicit_config) {
        const kimix::string found = cliapp_find_config(exe_dir, "default_config.json");
        if (!found.empty()) {
            opts.config_path = found;
            opts.config_is_provider_only = true;
        }
    } else {
        // Explicit --config/--provider: resolve it the way the reference's
        // _load_config_file does (direct path, then the cwd/exe parents by leaf
        // name, then PATH).  A "--config=qwen_scnet.json" anywhere under a
        // directory that (transitively) contains qwen_scnet.json must find it.
        const kimix::string resolved = cliapp_seek_config(opts.config_path, exe_dir);
        if (resolved.empty()) {
            print_error("Config file not found: " + opts.config_path);
            flush_streams();
            uninstall_ctrlc_handler();
            return kExitConfig;
        }
        opts.config_path = resolved;
    }

    // H9: config.py:218-227 - invalid JSON only WARNS ("Invalid JSON in config
    // file: {path} ({e})") and leaves the provider unset.  An auto-discovered
    // default_config.json then falls through to the H1 auto-init below (the
    // task's "warn + continue"); an EXPLICIT --config/--provider keeps the
    // reference's eventual failure: the warning is followed by the config
    // error and exit 1 (args.config set suppresses _maybe_run_default_config_init).
    if (!opts.config_path.empty()) {
        provider_config probe;
        kimix::string probe_error;
        bool json_error = false;
        // H9 probe: announce_model=false - the reference's _load_config_file
        // only parses the JSON here; _load_and_set_provider (and its
        // "Provider model:" line) runs once in app_init.
        if (!load_provider_config(opts.config_path, probe, probe_error, &json_error,
                                  /*announce_model=*/false) &&
            json_error) {
            kimix::string detail = probe_error;
            const size_t sep = find(detail, "': ");
            if (sep != kimix::string::npos) {
                detail = detail.substr(sep + 3);
            }
            print_warning("Invalid JSON in config file: " + opts.config_path + " (" +
                          detail + ")");
            if (!explicit_config) {
                opts.config_path.clear();
            } else {
                flush_streams();
                uninstall_ctrlc_handler();
                return kExitConfig;
            }
        }
    }

    // H1: args.py:55-108 _maybe_run_default_config_init - when no provider is
    // configured, run the wizard on a TTY or write the kimi template (with the
    // env api_key) otherwise, then continue with the freshly created config.
    if (opts.config_path.empty()) {
        const kimix::string boot_path =
            join_path(opts.work_dir.empty() ? current_dir() : absolute_path(opts.work_dir),
                      "default_config.json");
        if (stream_is_console(stdin)) {
            // TTY: the interactive wizard, gate first (run_init(False)).
            app_context boot_app;
            boot_app.work_dir =
                opts.work_dir.empty() ? current_dir() : absolute_path(opts.work_dir);
            init_input_fn tty_input = [](kimix::string_view prompt, kimix::string &line) {
                // The wizard's prompt line (print_info's BRIGHT_MAGENTA, no
                // trailing newline) + one blocking stdin line.
                if (!prompt.empty()) {
                    print_raw(colorful_text(prompt, static_cast<int>(color::bright_magenta)));
                    std::fflush(stdout);
                }
                line.clear();
                for (;;) {
                    const int ch = std::fgetc(stdin);
                    if (ch == EOF) {
                        return false;
                    }
                    if (ch == '\n') {
                        break;
                    }
                    line.push_back(static_cast<char>(ch));
                }
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                return true;
            };
            run_init_wizard(boot_app, boot_path, /*initialize=*/false,
                            /*open_after=*/false, tty_input);
        } else {
            // Non-interactive: write the minimal template so the CLI can
            // start (args.py:86-96).
            kimix::string api_key;
            get_env("KIMI_API_KEY", api_key);
            if (api_key.empty()) {
                get_env("KIMIX_API_KEY", api_key);
            }
            kimix::string text(k_init_default_config_template);
            // Fill the api_key member of the template JSON.
            yyjson_mut_doc *doc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
            if (doc != nullptr) {
                yyjson_doc *templ = yyjson_read_opts(
                    const_cast<char *>(k_init_default_config_template),
                    std::strlen(k_init_default_config_template), 0,
                    &kimix::llm::kYYJsonAlcMi, nullptr);
                if (templ != nullptr && yyjson_is_obj(yyjson_doc_get_root(templ))) {
                    yyjson_mut_val *root =
                        yyjson_val_mut_copy(doc, yyjson_doc_get_root(templ));
                    if (root != nullptr) {
                        yyjson_mut_doc_set_root(doc, root);
                        yyjson_mut_obj_put(root, yyjson_mut_str(doc, "api_key"),
                                           yyjson_mut_strncpy(doc, api_key.c_str(),
                                                              api_key.size()));
                        size_t len = 0;
                        char *json = yyjson_mut_write_opts(
                            doc, YYJSON_WRITE_PRETTY_TWO_SPACES, &kimix::llm::kYYJsonAlcMi,
                            &len, nullptr);
                        if (json != nullptr) {
                            text.assign(json, len);
                            text.push_back('\n');
                            mi_free(json);
                        }
                    }
                }
                yyjson_doc_free(templ);
                yyjson_mut_doc_free(doc);
            }
            kimix::string write_error;
            if (write_file(boot_path, text, write_error)) {
                print_warning("Created default config at " + boot_path +
                              ". Please set KIMI_API_KEY/KIMIX_API_KEY or edit the file.");
            } else {
                print_error(write_error);
            }
        }
        // Reload with the newly created config (args.py:98-108).
        if (file_exists(boot_path)) {
            opts.config_path = boot_path;
            opts.config_is_provider_only = true;
        }
    }

    app_context app;
    stream_renderer renderer(/*show_thinking=*/!opts.no_think, /*show_usage=*/true);
    renderer.set_output(stdout);
    // H4: the CLI always passes format_output=True in the reference
    // (core.py:127-131) - text parts buffer and flush as rendered markdown.
    renderer.set_markdown(true);

    kimix::string error;
    if (!app_init(opts, app, error, nullptr)) {
        print_error(error.empty() ? kimix::string("failed to initialise the CLI") : error);
        flush_streams();
        uninstall_ctrlc_handler();
        return kExitConfig;
    }
    // G8: Ctrl-C handling (SetConsoleCtrlHandler / sigaction behind
    // cli_signal): the handler stores into the turn token's flag; the REPL
    // interprets it as "bye." at the prompt and as a turn cancellation
    // ("Keyboard Interrupt.", session kept) mid-turn.
    install_ctrlc_handler(&app.cancel.flag());
    if (opts.dry_run) {
        print_string(app_dry_run_report(app));
        flush_streams();
        uninstall_ctrlc_handler();
        return kExitOk;
    }
    app.renderer = &renderer;

    int code = kExitOk;
    if (opts.has_prompt) {
        if (!app_run_prompt(app, opts.prompt)) {
            code = kExitRuntime;
        }
    } else {
        kimix::vector<kimix::string> scripted;
        if (!opts.script_path.empty()) {
            kimix::string text;
            kimix::string read_error;
            if (!read_file(opts.script_path, text, read_error)) {
                print_error(read_error);
                flush_streams();
                uninstall_ctrlc_handler();
                return kExitConfig;
            }
            split_lines(text, scripted);
        }
        code = repl_run(app, stdin, stdout, scripted);
    }

    // Teardown: every turn already persisted the session.  H11: the reference's
    // final _add_cli_session bookkeeping runs before the session closes.
    if (app.initialized) {
        if (!app.session_closed) {
            app_touch_cli_session(app); // final save + cache bookkeeping
        }
        app.soul.reset();
        app.session.reset();
        if (app.approval != nullptr) {
            app.approval->set_wire_sink(nullptr); // detach before the writer dies
        }
        app.wire.reset();
        kimix::string close_error;
        if (opts.clean) {
            // H6 (main.py:12-17 + session.py:62-77): -c/--clean removes the
            // whole cache root - every session directory, tmp_<pid> folder
            // included - and reports it in bright green.
            const kimix::string cache_root = session_store::cache_root(app.work_dir);
            if (!remove_all(cache_root, close_error)) {
                print_error(close_error);
            } else {
                print_success(cache_root + " deleted.");
            }
        } else if (!app.session_closed) {
            app.store.close(/*delete_if_anonymous=*/true, close_error);
            // H6 (commands.py:407-423): drop the shared tool temp folder and
            // the leftovers of previously killed processes on the way out.
            kimix::string cleanup_error;
            cleanup_temp_folder(app.work_dir, cleanup_error);
        }
    }
    flush_streams();
    uninstall_ctrlc_handler();
    return code;
}

} // namespace kimix::cli
