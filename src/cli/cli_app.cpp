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
#include <system_error>
#include <utility>

#include "llm/yyjson_alc.h"
#include "yyjson.h"

#include "builtin_tools/todo_tool.h"
#include "builtin_tools/tool.h"

#include "cli/cli_common.h"
#include "cli/cli_print.h"
#include "cli/cli_repl.h"
#include "cli/cli_tools.h"

namespace kimix::cli {

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

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
      // The reference's LoopControl derives the dynamic tool-output budget from
      // the model output budget; the provider config carries no explicit value.
      opts.tool_call_buffer_tokens = app.provider.max_tokens > 0 ? app.provider.max_tokens / 4 : 0;
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

// Flush every tool message appended since `rendered` to the renderer, resolving
// the tool name from the assistant message that issued the call.
void cliapp_flush_tool_results(app_context &app, kimix::agent::AgentSession &session,
                               size_t &rendered) {
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
                     const kimix::llm::Chunk &chunk) {
    if (app.renderer == nullptr) {
        return;
    }
    cliapp_flush_tool_results(app, session, rendered);
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
// reference's "Finished, context usage: ...  time: H:MM:SS" banner.
bool cliapp_run_turn(app_context &app, kimix::agent::AgentSession &session,
                     kimix::agent::KimiSoul &soul, kimix::string_view input) {
    const size_t rendered_start = session.history().size();
    size_t rendered = rendered_start;
    if (app.renderer != nullptr) {
        app.renderer->reset_capture();
    }
    // The reference prints a cyan "Start..." label before every prompt attempt
    // (the native CLI has no retry/backup-provider loop, so there is one label).
    print_word(colorful_text("Start...\n", 96), true, false);
    const auto started = std::chrono::steady_clock::now();
    const kimix::agent::TurnResult result =
        soul.turn(input, [&app, &session, &rendered](const kimix::llm::Chunk &chunk) {
            cliapp_on_chunk(app, session, rendered, chunk);
        });
    cliapp_flush_tool_results(app, session, rendered);
    if (app.renderer != nullptr) {
        app.renderer->finish_turn();
    }
    if (!result.ok && !result.ignored) {
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
    }
    print_word(colorful_text("Finished, context usage: " + usage + "  time: " +
                                 format_duration_hm(elapsed) + "\n",
                             92, -1, "1"),
               true, true);
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
    return app_rebind_session(app, error);
}

  } // namespace

  // ---------------------------------------------------------------------------
  // Public seam (tests)
  // ---------------------------------------------------------------------------

  kimix::string resolve_config_path(const kimix::string &given, const kimix::string &exe_dir) {
      return cliapp_seek_config(given, exe_dir);
  }

  // ---------------------------------------------------------------------------
  // Session re-binding (public: /clear rebuilds the session in place)
  // ---------------------------------------------------------------------------

bool app_rebind_session(app_context &app, kimix::string &error) {
    app.soul.reset();
    app.session.reset();
    app.session.reset(new kimix::agent::AgentSession(app.work_dir));
    app.session->set_state_dir(app.store.dir());
    app.session->tool_session().session_id = app.store.id();
    app.session->tool_session().plan_enabled = cliapp_has_plan_tools(app.agent.enabled_tools);
    if (!app.session->load_state(error)) {
        return false;
    }
    kimix::vector<kimix::llm::Message> history;
    if (!app.store.load_history(history, error)) {
        return false;
    }
    app.session->history() = std::move(history);
    kimix::agent::IChatBackend *chat =
        app.backend ? static_cast<kimix::agent::IChatBackend *>(app.backend.get())
                    : app.injected;
    if (chat == nullptr) {
        error = "no chat backend is available (app_init created none)";
        return false;
    }
    app.soul.reset(new kimix::agent::KimiSoul(*app.session, *chat, app.soul_options));
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
        // The reference's set_default_thinking(False) turns the request's
        // reasoning off.  The three native providers always send the
        // DeepSeek-style thinking/reasoning keys, so the only native mechanisms
        // are the reasoning_key round-trip (cleared here) and suppressing the
        // reasoning in the renderer (see src/cli/reports/cli_commands.md).
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
    app.initialized = true;
    return true;
}

// ---------------------------------------------------------------------------
// app_run_prompt / app_compact / app_usage
// ---------------------------------------------------------------------------

bool app_run_prompt(app_context &app, kimix::string_view input) {
    if (app.session == nullptr || app.soul == nullptr) {
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
    const bool ok = cliapp_run_turn(app, *app.session, *app.soul, input);
    kimix::string error;
    if (!app_save_session(app, error)) {
        print_error(error);
    }
    return ok;
}

bool app_compact(app_context &app, kimix::string_view instruction) {
    if (app.soul == nullptr) {
        return false;
    }
    const kimix::string before = app_usage_text(app);
    const auto started = std::chrono::steady_clock::now();
    kimix::string error;
    if (!app.soul->compact_context(instruction, error)) {
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
    return app_rebind_session(app, error);
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

bool app_read_input(app_context &app, kimix::string_view prompt, kimix::string &line) {
    line.clear();
    if (app.pending != nullptr && !app.pending->empty()) {
        line = app.pending->front();
        app.pending->erase(app.pending->begin());
        return true;
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
        return kExitUsage;
    }
    set_quiet(false);

    if (opts.help) {
        print_string(cli_help_text_extended(colorful()));
        flush_streams();
        return kExitOk;
    }
    if (opts.version) {
        print_string(kimix::string("kimix_cli ") + KIMIX_CORE_VERSION + " (kimix " +
                     KIMIX_CORE_VERSION + ")");
        flush_streams();
        return kExitOk;
    }
    if (!opts.subcommand.empty()) {
        print_error(opts.subcommand +
                    ": not supported by the native CLI (the Python CLI's "
                    "serve/gui/ssecli/mcp front ends are Python-only)");
        flush_streams();
        return kExitUnsupported;
    }

    // Provider config resolution (the reference's default_config.json search).
    const kimix::string exe_dir = cliapp_exe_dir(argc > 0 ? argv[0] : nullptr);
    if (opts.config_path.empty()) {
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
            return kExitConfig;
        }
        opts.config_path = resolved;
    }

    app_context app;
    stream_renderer renderer(/*show_thinking=*/!opts.no_think, /*show_usage=*/true);
    renderer.set_output(stdout);

    kimix::string error;
    if (!app_init(opts, app, error, nullptr)) {
        print_error(error.empty() ? kimix::string("failed to initialise the CLI") : error);
        flush_streams();
        return kExitConfig;
    }
    if (opts.dry_run) {
        print_string(app_dry_run_report(app));
        flush_streams();
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
                return kExitConfig;
            }
            split_lines(text, scripted);
        }
        code = repl_run(app, stdin, stdout, scripted);
    }

    // Teardown: every turn already persisted the session.  The reference closes
    // the session on /exit (deleting an anonymous directory) and, under
    // -c/--clean, removes the cache; the native CLI deletes the current
    // session's directory for --clean (never the whole .kimix_cache root, so
    // -c can not destroy other sessions).
    if (app.initialized) {
        app.soul.reset();
        app.session.reset();
        kimix::string close_error;
        if (opts.clean) {
            // -c/--clean removes this session's directory (the reference
            // removes the whole cache root plus its tmp_<pid> folder; the
            // native CLI never touches another session's directory).
            if (!remove_all(app.store.dir(), close_error)) {
                print_error(close_error);
            }
        } else if (!app.session_closed) {
            app.store.close(/*delete_if_anonymous=*/true, close_error);
        }
    }
    flush_streams();
    return code;
}

} // namespace kimix::cli
