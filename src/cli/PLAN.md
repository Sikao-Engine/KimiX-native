# Native CLI (`src/cli`) — implementation plan

Goal: a **native C++ CLI** (`bin/debug/kimix_cli.exe`) that is the drop-in equivalent of
kimi-agent's Python command line — `src/kimix/cli_impl/` (args, REPL, slash commands),
`src/kimix/ui/` (printing + wire streaming) and the config half of `src/kimix/utils/`
(`config.py`, `session.py`) — built on this repo's existing native core
(`kimix-core` → `kimix-llm`, which already carries `agent/` + `llm/` + `builtin_tools/`).

Reference (ground truth, read-only): `C:/dev/kimi-agent` (outer) and its inner checkout
`C:/dev/kimi-agent/kimi-cli`.  Specs derived from it:

| spec | covers |
|---|---|
| `.kimix_cache/cli_specs/01_args_and_flow.md` | `cli_impl/args.py`, `core.py`, `main.py`, `constants.py`, `agent_*.json` |
| `.kimix_cache/cli_specs/02_config.md` | `utils/config.py`, `kimi_cli/config.py`, provider/model/agent config schemas |
| `.kimix_cache/cli_specs/03_ui.md` | `ui/printing.py`, `ui/stream.py`, `base.py` re-exports, `cli_impl/utils.py` input |
| `.kimix_cache/cli_specs/04_commands_session.md` | `cli_impl/commands.py`, `utils/session.py`, `_globals.py`, `init.py` |

(The specs are working notes in the gitignored cache; the **behavioural contract** is the
Python source itself and the "Deviations" table below.)

---

## 1. Deliverables

1. `target("kimix-cli")` — static library, `src/cli/*.cpp` minus `main.cpp`.
2. `target("kimix_cli")` — binary `bin/<mode>/kimix_cli.exe`, entry `src/cli/main.cpp`.
3. `test_cli` — Boost.UT unit test executable `tests/unit/cli/test_cli.cpp`.
4. `scripts/cli_e2e.py` — real end-to-end driver: runs `kimix_cli.exe` against
   `C:/dev/ds_flash.json` + an `agent_*.json` manifest, asserts PASS/FAIL evidence.
5. `src/cli/reports/cli.md` — traceability report (what is ported, deviations, verification).

Invocation contract for the e2e/dry-run:

```bash
# dry run: parse + validate everything, no network
bin/debug/kimix_cli.exe --provider C:/dev/ds_flash.json \
    --agent-file C:/dev/kimi-agent/src/kimix/agent_worker.json --dry-run
# real single turn (used by the e2e driver)
bin/debug/kimix_cli.exe --provider C:/dev/ds_flash.json --agent-file <manifest> \
    --prompt "say hi and stop"
# scripted REPL (slash commands + prompts), non-interactive
bin/debug/kimix_cli.exe --provider ... --agent-file ... --script <file>
```

---

## 2. Build wiring (xmake)

`src/xmake.lua` gains exactly one line: `includes("cli")`.
New `src/cli/xmake.lua`:

```lua
target("kimix-cli")
    set_kind("static")
    add_files("*.cpp")
    remove_files("main.cpp")                       -- main() only lives in the binary
    add_headerfiles("*.h")
    add_includedirs("..", {public = true})          -- keeps `#include "cli/..."` working
    add_deps("kimix-llm")                           -- == agent + llm + builtin_tools
    add_defines("KIMIX_CORE_STATIC")                -- see kimix-core link model
    _config_project({batch_size = 8, project_kind = "static"})
target_end()

target("kimix_cli")
    set_kind("binary")
    add_files("main.cpp")
    add_deps("kimix-cli")
    _config_project({batch_size = 8})
target_end()
```

`tests/xmake.lua` gains:

```lua
test_proj("test_cli", "unit/cli/test_cli.cpp", function()
    add_deps("kimix-llm", "kimix-cli")
end)
```

`kimix-llm` is the existing static lib; it already `add_files("agent/*.cpp")`,
`add_files("llm/**")` and `add_files("builtin_tools/*.cpp")`, so depending on it is exactly
the requested `agent + llm + builtin_tools` dependency set.

---

## 3. Module layout and frozen interfaces

All code lives in `namespace kimix::cli`, `src/cli/`, included as `"cli/xxx.h"`.
Rules that apply everywhere (see `.agents/skills/cpp`): C++20, K&R braces, 4-space indent,
`kimix::` containers in public APIs, **no RTTI**, **no exceptions** (`throw`/`try`/`catch`
are build errors — failures travel through `bool` + `error` out-parameters), unity build
(batch 8 → every TU-local helper is `static` or in an anonymous namespace with a
module-specific prefix, no file-scope `using namespace`), yyjson for JSON with the
mimalloc allocator (`kimix::llm::kYYJsonAlcMi`; writer buffers are released with
`mi_free()`, never `free()`).

### 3.1 `cli_common.{h,cpp}` — shared helpers

```cpp
bool read_file(const kimix::string &path, kimix::string &out, kimix::string &error);
bool write_file(const kimix::string &path, kimix::string_view text, kimix::string &error,
                bool append = false);
bool file_exists(const kimix::string &path);
bool dir_exists(const kimix::string &path);
bool make_dirs(const kimix::string &path, kimix::string &error);
bool remove_all(const kimix::string &path, kimix::string &error);
kimix::string current_dir();
kimix::string absolute_path(kimix::string_view path);
kimix::string join_path(kimix::string_view a, kimix::string_view b);
kimix::string parent_path(kimix::string_view path);
kimix::string file_name(kimix::string_view path);
kimix::string extension(kimix::string_view path);
kimix::string_view trim(kimix::string_view s);                    // ASCII ws incl. \x1c..\x1f
bool starts_with(kimix::string_view s, kimix::string_view prefix);
bool ends_with(kimix::string_view s, kimix::string_view suffix);
bool contains(kimix::string_view s, kimix::string_view needle);
kimix::string to_lower_ascii(kimix::string_view s);
kimix::string random_hex(size_t bytes = 16);                      // uuid4().hex shape
int64_t now_unix_seconds();
int64_t file_mtime_unix(const kimix::string &path);               // 0 when missing
kimix::string format_utc(int64_t unix_seconds, const char *fmt = "%Y-%m-%d %H:%M:%S");
kimix::string format_duration_hm(int64_t seconds);                // "H:MM:SS"
bool split_lines(kimix::string_view text, kimix::vector<kimix::string> &out);
bool get_env(const char *name, kimix::string &out);
bool set_env(const char *name, kimix::string_view value);
void set_stdout_utf8();                                           // Windows: CP 65001 + VT
```

### 3.2 `cli_args.{h,cpp}` — `cli_impl/args.py` + `constants.py`

```cpp
struct cli_options {
    bool clean = false;          // -c / --clean
    bool no_color = false;       // --no_color / -no_color
    bool no_think = false;       // --no_think / -no_think
    bool no_yolo = false;        // --no_yolo / -no_yolo
    bool manually_cot = false;   // --manually-cot
    bool help = false;           // -h / --help
    bool version = false;        // --version
    bool dry_run = false;        // --dry-run             (native addition)
    bool has_prompt = false;
    bool interactive_forced = false;   // --interactive (native addition)
    kimix::string config_path;         // --config PATH (combined) --provider PATH (provider only)
    bool config_is_provider_only = false;
    kimix::string agent_file;          // --agent-file PATH
    kimix::string work_dir;            // --work-dir PATH (default: cwd)
    kimix::string prompt;              // -p / --prompt TEXT
    kimix::string script_path;         // --script PATH
    kimix::vector<kimix::string> skill_dirs;      // -s / --skill-dir [DIR...]
    kimix::string subcommand;                     // "" | serve | gui | ssecli | mcp
    kimix::vector<kimix::string> subcommand_args; // e.g. {"serve"} or {"mcp","list"}
    kimix::vector<kimix::string> errors;          // usage errors -> exit 2
};

bool parse_args(int argc, char **argv, cli_options &out);   // false == usage error
kimix::string cli_help_text(bool colorful);                 // port of constants.HELP_STR
const char *cli_usage_line();                               // one-line synopsis
```

Flag semantics (mirror `args.py` exactly, plus the three native additions):

* Reference flags keep their reference names/dest: `-c/--clean`, `--no_color`,
  `--no_think`, `--no_yolo`, `--manually-cot`, `-s/--skill-dir` (`nargs="*"`).
* `--config` is **not** an argparse option in the reference: it is post-scanned from the
  leftovers, accepting `--config=v` and `--config v`. Reproduce that: the scanner removes
  the occurrence and its value, first hit wins; the value may be either a combined config
  (provider **and** agent sections) or a plain provider JSON.
* Subcommands `serve|gui|ssecli|mcp` are recognised (with their reference options parsed
  into `subcommand_args`) and then refused with an explicit
  `"<name>: not supported by the native CLI"` message → exit 3.
* Unknown option / missing value / unknown subcommand → message on stderr + usage, exit 2
  (argparse behaviour).
* The reference has **no positional prompt** in this revision; the native CLI adds
  `-p/--prompt TEXT` (one turn, then exit) and `--script PATH` (REPL inputs read from a
  file) so the CLI is drivable head-lessly. `--dry-run` is likewise a native addition.
  All three are documented additions, not silent deviations.

### 3.3 `cli_print.{h,cpp}` — `ui/printing.py`

```cpp
enum class color : int { black = 30, red, green, yellow, blue, magenta, cyan, white,
                         bright_black = 90, bright_red, bright_green, bright_yellow,
                         bright_blue, bright_magenta, bright_cyan, bright_white };
enum class bg_color : int { black = 40, red, green, yellow, blue, magenta, cyan, white,
                            bright_black = 100, bright_red, bright_green, bright_yellow,
                            bright_blue, bright_magenta, bright_cyan, bright_white };
enum class style : int { reset = 0, bold = 1, dim = 2, italic = 3, underline = 4,
                         blink = 5, inverse = 7, hidden = 8, strike = 9 };

void set_colorful(bool on);  bool colorful();
void set_quiet(bool on);     bool quiet();
void set_color_stream(std::FILE *f);          // default stdout
void set_plain_stream(std::FILE *f);          // default stderr (errors)

kimix::string ansi_prefix(int fg, int bg, kimix::string_view styles);   // "\x1b[...m"
kimix::string colorful_text(kimix::string_view text, int fg = -1, int bg = -1,
                            kimix::string_view styles = "");
void print_string(kimix::string_view text);                    // plain, newline
void print_info(kimix::string_view text);                      // bright_magenta
void print_success(kimix::string_view text);                   // bright_green + bold
void print_error(kimix::string_view text);                     // bright_red + bold -> stderr
void print_warning(kimix::string_view text);                   // bright_yellow + bold
void print_debug(kimix::string_view text);                     // bright_cyan, muted by _quiet
void print_raw(kimix::string_view text);                       // no newline, no colour
```

`colorful_text` returns the text unchanged when colour is off (exact reference behaviour);
the ANSI code order is `styles;fg;bg` (see spec §1). Colour is auto-disabled when the
stream is not a console, unless `--no_color`/colour-off is explicitly requested.

### 3.4 `cli_config.{h,cpp}` + `cli_tools.{h,cpp}` — `utils/config.py` + `kimi_cli/config.py`

```cpp
struct service_endpoint { kimix::string base_url, api_key; };
struct openai_settings { bool thinking = true, reasoning = true, chat_template_kwargs = true; };

struct provider_config {
    kimix::string model;                 // "model"
    kimix::string type;                  // as written, e.g. "openai_legacy" | "kimi"
    kimix::string provider_family;       // "openai" | "openai_responses" | "anthropic"
    kimix::string base_url;              // "base_url" else "url"
    kimix::string api_key;
    kimix::string reasoning_key;         // default "reasoning_content"
    kimix::string thinking_effort;       // default "high"
    int64_t max_context_size = 0;        // resolved (explicit > model defaults > 0)
    int64_t max_tokens = 0;              // resolved (explicit > model default > ctx/4)
    bool show_thinking_stream = true;
    bool max_context_size_explicit = false;
    bool max_tokens_explicit = false;
    kimix::vector<kimix::string> capabilities;                     // image_in/video_in/thinking/...
    kimix::vector<std::pair<kimix::string, kimix::string>> custom_headers;
    kimix::vector<std::pair<kimix::string, kimix::string>> env;
    openai_settings openai;
    service_endpoint search, fetch;
    bool has_oauth = false; kimix::string oauth_storage, oauth_key;
    kimix::string source_path;
    kimix::vector<kimix::string> warnings;   // unknown keys / dropped entries
};

struct agent_config {
    kimix::string name, extend, manifest_path, manifest_dir;
    kimix::string system_prompt_path;        // resolved against manifest_dir when relative
    kimix::vector<std::pair<kimix::string, kimix::string>> system_prompt_args;
    kimix::string model, when_to_use;        // "" == inherit
    bool has_tools = false;
    kimix::vector<kimix::string> tools, allowed_tools, exclude_tools, subagents;
    kimix::vector<kimix::string> enabled_tools;   // resolved registry names, in order
    kimix::vector<kimix::string> warnings;
};

bool load_provider_config(const kimix::string &path, provider_config &out, kimix::string &error);
bool load_agent_config(const kimix::string &path, agent_config &out, kimix::string &error);
bool resolve_model_defaults(kimix::string_view model_name,
                            int64_t &max_context_size, int64_t &max_output);   // false == unknown
kimix::llm::Config to_llm_config(const provider_config &p);
kimix::string provider_report(const provider_config &p);   // --dry-run text
kimix::string agent_report(const agent_config &a);

// cli_tools.h — agent manifest tool-path resolution (see §4)
kimix::string resolve_tool_path(kimix::string_view tool_path);   // "" when unknown
const kimix::vector<kimix::string> &default_agent_tools();       // 25 registry names
const kimix::vector<std::pair<kimix::string, kimix::string>> &agent_tool_table();
```

Union dialect (both must load unchanged):

* **kimix flat** (`C:/dev/ds_flash.json`): top-level `model`, `type`, `url`, `api_key`,
  `max_context_size`, `max_tokens`, `capabilities`, `custom_headers`,
  `show_thinking_stream`, `thinking_effort`, `services{search,fetch}`, `oauth{...}`,
  `openai_settings{...}`, `env`, `reasoning_key`.
* **kimi-cli nested** (`kimi_cli/config.py`): `model` referencing `models[...]`, plus
  `provider{type,base_url,api_key,env,custom_headers,reasoning_key,openai_settings,oauth}`,
  `loop_control{...}`, `services{...}`, `show_thinking_stream`, `thinking_effort`,
  `default_thinking`, `default_yolo`, `theme`, `max_tokens`.
  `base_url` wins over `url`; nested keys win over flat duplicates.
* Unknown keys are collected into `warnings`, never fatal.
* `type` → `provider_family`: `openai`/`openai_legacy`/`kimi`/`moonshot*` → `openai`;
  `openai_responses*` → `openai_responses`; `anthropic*`/`claude*` → `anthropic`;
  anything else → error naming the value (the C++ providers only implement those three).
* `api_key` falls back to `$KIMI_API_KEY` then `$KIMIX_API_KEY` (reference order);
  `env` is applied to the process environment after load.
* `max_context_size`/`max_tokens` resolution implements `_MODEL_DEFAULTS` +
  `_resolve_model_defaults` verbatim: lowercase split on `[^a-z0-9]+`, first matching
  keyword row wins, numeric tokens must match exactly, alphabetic tokens may fuzzy-match a
  remaining alphabetic token with Indel ratio ≥ 80 (`_FUZZY_TOKEN_THRESHOLD`); unknown
  model with no explicit context size → hard error (reference `sys.exit(1)` → exit 1).
  `max_tokens` unset → model default; still unset → `max_context_size / 4`.

### 3.5 `cli_stream.{h,cpp}` — `ui/stream.py` (the parts reachable from the CLI)

```cpp
class stream_renderer {
public:
    stream_renderer(bool show_thinking, bool show_usage = true);
    void on_step_begin(int32_t step, int32_t max_steps);
    void on_text_delta(kimix::string_view delta);
    void on_reasoning_delta(kimix::string_view delta);
    void on_tool_call_begin(const kimix::llm::ToolCall &call);
    void on_tool_call_args_delta(kimix::string_view delta);
    void on_tool_result(kimix::string_view name, bool ok, kimix::string_view message,
                        kimix::string_view output_summary = {});
    void on_compaction_begin();
    void on_compaction_end(bool ok);
    void on_context_usage(double ratio, int64_t tokens);
    void on_error(kimix::string_view message);
    void finish_turn();
    const kimix::string &captured_text() const;   // final assistant text (for export/asserts)
};
kimix::string percentage_and_token(double ratio, int64_t tokens);   // "12.3% (456 tokens)"
kimix::string percentage_str(double ratio);
kimix::string context_usage_banner(double ratio, int64_t tokens);   // 80-char rule
```

Byte-exact requirements (from spec `03_ui.md` §2): `[Think] ` banner in bright cyan before
the first reasoning chunk; `⚡ <Name>` bright magenta header before streamed tool arguments;
`✓ <Name>` bright green / `✗ <Name>` bright red for tool results with the dim
`  <message>` detail line; `Compacting...` bright magenta; the context-usage divider is
GRAY `==================== Context usage: {usage} ` padded with `=` to exactly 80 chars;
thinking text is suppressed in quiet mode; a newline is emitted when switching between
text/thinking/tool-call states.

### 3.6 `cli_session.{h,cpp}` — `utils/session.py` session store

```cpp
struct session_state {                   // <session_dir>/state.json
    kimix::string custom_title;
    bool title_generated = false;
    int32_t title_generate_attempts = 0;
    bool yolo = true, afk = false, auto_approve_actions = false;
    kimix::vector<kimix::string> additional_dirs;
    bool archived = false, auto_archive_exempt = false;
};

struct session_info {
    kimix::string id, title;
    int64_t updated_at = 0;
    double context_usage = 0.0;
    int64_t context_tokens = 0;
    bool usage_known = false;
};

class session_store {
public:
    session_store();
    bool open(kimix::string_view work_dir, kimix::string_view id, bool resume,
              kimix::string &error);                  // empty id -> anonymous random id
    const kimix::string &id() const; const kimix::string &dir() const;
    const kimix::string &work_dir() const;
    bool anonymous() const;
    bool load_state(session_state &out, kimix::string &error) const;
    bool save_state(const session_state &st, kimix::string &error) const;
    bool save_history(const kimix::vector<kimix::llm::Message> &h, kimix::string &error) const;
    bool load_history(kimix::vector<kimix::llm::Message> &h, kimix::string &error) const;
    void set_usage(double ratio, int64_t tokens, bool known = true);
    bool store_as(kimix::string_view new_id, kimix::string &error);
    bool copy_into(kimix::string_view new_id, kimix::string &error);   // /load semantics
    bool close(bool delete_if_anonymous, kimix::string &error);
    bool clear_context(kimix::string &error);                          // /clear
    bool export_markdown(const kimix::vector<kimix::llm::Message> &h, kimix::string_view path,
                         kimix::string &error) const;                  // /export
    static kimix::vector<session_info> list(const kimix::string &work_dir);   // newest first
};
```

Layout (reference-compatible): cache root `<work_dir>/.kimix_cache`, one directory per
session named by its id (`uuid4().hex` = 32 hex chars, or a user-supplied name), containing
`state.json` (2-space indent, atomic replace via tmp+rename), `context.jsonl` (the legacy
Python history format the reference still reads and migrates) and `wire.jsonl` (metadata
header + `{"timestamp","message"}` records). `state.json` keeps the reference keys even
when we do not use them, and an unknown-key-preserving rewrite keeps Python-written files
intact.

### 3.7 `cli_repl.{h,cpp}` + `cli_commands.{h,cpp}` + `cli_app.{h,cpp}` + `main.cpp`

```cpp
struct command_result {
    bool has_input = false;        // feed `next_input` as the next REPL input
    kimix::string next_input;
    bool should_break = false;     // exit the REPL
};
struct command_entry {
    kimix::string name;            // "help", "clear", ... (no leading '/')
    kimix::string help;            // one-line help (for /help completion)
    kimix::function<command_result(const kimix::vector<kimix::string> &args,
                                   app_context &app,
                                   kimix::vector<kimix::string> &text_arr)> handler;
};
const kimix::vector<command_entry> &command_map();
const command_entry *find_command(kimix::string_view name);

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
    stream_renderer *renderer = nullptr;      // borrowed, one per turn
    int32_t exit_code = 0;
};
bool app_init(const cli_options &opts, app_context &app, kimix::string &error);
bool app_run_prompt(app_context &app, kimix::string_view input);   // one turn
bool app_compact(app_context &app, kimix::string_view instruction);
kimix::string app_dry_run_report(const app_context &app);
int  cli_main(int argc, char **argv);            // used by main() and by tests
int  repl_run(app_context &app, std::FILE *in, std::FILE *out,
              const kimix::vector<kimix::string> &scripted);
```

REPL semantics (port of `_client_cli`): prompt string
`"\n>>>>>>>>> Enter your prompt or command:\n"`; empty input → `continue`; input starting
with `/` → split the first `:` found in `trim(input)`, slice the **unstripped** remainder,
look the command up in `command_map()` with `_cmd_unknown` fallback
(`"Unrecognized command."`); a non-slash path that is an existing file → `.py` files are
not executed (documented deviation: no embedded Python interpreter) but reported, other
files are read as the prompt; EOF / Ctrl-C → `"\nbye."` in bright green + bold. The agent's
final answer is streamed through `stream_renderer`.

### 3.8 Commands

Implemented: `help`, `clear`, `compact`, `context`, `exit`, `file`, `txt`, `export`,
`resume`, `store`, `load`, `sessions`, `init`, `cmd`, `fix`, `todo`, `plan`, `swarm`,
`supervisor`, `reflection`, `unknown`.

| command | port | note |
|---|---|---|
| `/help` | full | prints `cli_help_text(colorful)`, same text as `constants.HELP_STR` (incl. the `writen` typos) |
| `/clear` | full | close+reopen the session, drop history, keep id/dir |
| `/compact` | full | requires context usage > 0, prints `Start compacting...` and `Context usage from A to B  time: H:MM:SS` |
| `/context` | full | `Context usage: P% (T tokens)` |
| `/exit` | full | save state, close (delete when anonymous), `bye!` |
| `/file:<path>` | full | returns the file text to the caller (REPL `.py` → reported unsupported) |
| `/txt` | full | multi-line until `/end` / `/cancel`, splits blocks back into `text_arr` |
| `/export[:path]` | full | markdown export to the given path or `<session>/export_<ts>.md` |
| `/resume:<id>` | full | close current, open `id` (creating it when absent) |
| `/store:<id>` | full | copy the current session dir to `<id>` |
| `/load:<id>` | full | copy a named session into a fresh anonymous session |
| `/sessions[:<name>]` | full | list (sorted by `updated_at` desc, `*` = current, `-` = unknown usage, UTC `%Y-%m-%d %H:%M:%S`), or create+switch when a name is given |
| `/init` | reduced | write a `default_config.json` template next to the exe/cwd and explain the flags (no interactive wizard) — documented |
| `/cmd:<cmd>` | full | run through the process runner, `Done.` / `Failed.` |
| `/fix:<cmd>` | full | run the command, then prompt the agent with the error text (4 attempts max) |
| `/todo:<path>` | full | TODO-comment scan (the 7 suffix families) then prompt |
| `/plan[:path]` | reduced | build the plan-writing prompt and prompt the agent with `plan_writing_path` set; the 3-attempt y/n revision loop is simplified to a single turn — documented |
| `/swarm` | reduced | anonymous session with `swarm_enabled`; `/cancel` honoured |
| `/supervisor` | reduced | anonymous session with the boss manifest; one turn |
| `/reflection` | reduced | prompt the agent to reflect on the current context (no Python repo introspection) |
| unknown | full | `Unrecognized command.` |

Every reduction is listed in `src/cli/reports/cli.md` with its reason (mostly: needs an
embedded Python interpreter, the SQLite session store, the wire protocol server, or
`asyncio`).

---

## 4. Agent manifest + tool mapping

`agent_*.json` (`C:/dev/kimi-agent/src/kimix/`) has the shape
`{"agent": {"extend": "default", "tools": ["<module>:<attr>", ...]}}`. Rules:

* `extend: "default"` → the built-in default agent: `enabled_tools = default_agent_tools()`
  (all 25 registry names), system prompt = the soul's default.
* `tools` **replaces** the inherited list (reference `AgentSpec` merge rule);
  `allowed_tools` (when present) wins over `tools`; `exclude_tools` removes by exact match.
* `<module>:<attr>` splits on the **last** `:`; the `module:attr` pair is mapped to a
  registry name by `agent_tool_table()` (the union already validated by
  `test_builtin_tool`'s `registry_covers_every_agent_json_tool` test — the table moves into
  `cli_tools.cpp` as production data, and the test is rewired to it). Unknown paths are
  warned about and dropped (reference: warn + None).
* `plan_*` tools need `session.plan_path`; `plan_tool` enables itself when the agent
  manifest lists them (`plan_enabled`), `is_sub_agent`/`parent_session_id`/`swarm_enabled`
  are filled from the session flags.

The 25-entry table (module path → registry name):

```
kimi_cli.tools.file:read -> Read            kimix.tools.file.bash:bash      -> Bash
kimi_cli.tools.file:read_image -> ReadImage kimix.tools.file.bash:pwsh      -> Pwsh
kimi_cli.tools.file:glob -> Glob            kimix.tools.file.run:Run        -> Run
kimi_cli.tools.file:grep -> Grep            kimix.tools.py:python           -> Python
kimi_cli.tools.file:edit -> Edit            kimix.tools.background:job_output -> JobOutput
kimi_cli.tools.file:write -> Write          kimix.tools.note:WritePlan      -> WritePlan
kimi_cli.tools.web:web_search -> WebSearch  kimix.tools.note:ReadPlan       -> ReadPlan
kimix.tools.web.fetch_url:fetch_url -> FetchUrl  kimix.tools.note:EditPlan  -> EditPlan
kimix.tools.agent:subagent -> Subagent      kimix.tools.agent:send_message  -> SendMessage
kimix.tools.agent:list_agents -> ListAgents kimix.tools.agent:interrupt_agent -> InterruptAgent
kimix.tools.swarm:workflow -> Workflow      kimi_cli.tools.todo:todo_write  -> TodoWrite
kimi_cli.tools.todo:todo_update -> TodoUpdate  kimi_cli.tools.memory:retrieve -> Retrieve
kimix.tools.context:compact -> Compact
```

---

## 5. Deviations (all documented in `src/cli/reports/cli.md`)

1. **No subcommands** `serve` / `gui` / `ssecli` / `mcp` — they start the Python server,
   GUI or MCP bridge. Recognised and refused with exit 3.
2. **Native additions**: `-p/--prompt`, `--script`, `--dry-run`, `--work-dir`,
   `--agent-file`, `--provider`, `--version`, `--interactive`.
3. **No embedded Python**: `/code:<x.py>`, the `.py` branch of `/file:`/REPL paths and
   `/reflection`'s repo introspection are reduced; `/init` writes a template instead of
   running the wizard.
4. **Session store**: `state.json` + `context.jsonl` + `wire.jsonl` are written with the
   reference schemas, but the SQLite `context.db`/`history.db` are not produced — Python
   migrates `context.jsonl` automatically, which keeps the round-trip lossless.
5. **Custom headers / reasoning key**: parsed and reported by `--dry-run`, but the three
   native providers currently send only `Authorization` + `Content-Type`; a non-empty
   `custom_headers` therefore produces a warning.
6. **No `readline`**: the reference's line editor was removed upstream (`759a35a`); the
   native REPL reads a line from stdin (with a `/` command completion table reserved for a
   later pass).
7. **Unbalanced-history compaction** stays report-don't-abort (inherited from `soul.cpp`).

---

## 6. Work packages (each ends with a build + test gate)

| # | owner | files | gate |
|---|---|---|---|
| S1 | planner | `src/cli/xmake.lua`, `src/xmake.lua` (1 line), `cli_common`, `cli_args`, `cli_print`, `main.cpp` skeleton | `xmake build kimix_cli`; `kimix_cli --help`, `--version`, unknown flag → exit 2 |
| S2 | subagent | `cli_config.{h,cpp}`, `cli_tools.{h,cpp}` (+ `scripts/gen_cli_config_goldens.py` if needed) | build + `--dry-run` on `ds_flash.json` + all five `agent_*.json` |
| S3 | subagent | `cli_session.{h,cpp}` | build + session round-trip check |
| S4 | subagent | `cli_stream.{h,cpp}` | build + rendering diff vs captured reference output |
| S5 | subagent | `cli_repl`, `cli_commands`, `cli_app`, `main.cpp` | build + scripted REPL against a scripted local HTTP LLM stub |
| S6 | subagent | `tests/unit/cli/test_cli.cpp`, `tests/xmake.lua` | `xmake build test_cli` + `test_cli.exe` all green |
| S7 | subagent + planner | `scripts/cli_e2e.py`, `src/cli/reports/cli.md` | real `ds_flash.json` end-to-end PASS |

Rules handed to every implementer subagent: read `AGENTS.md`, the `xmake`/`cpp`/`test`
skills, this file, and the relevant `.kimix_cache/cli_specs/*.md`; never weaken or delete a
failing assertion; build with `python scripts/build_locked.py --timeout 600 -- xmake build
<target>` (one target per invocation); verify with `git diff <file>`; add
`KIMIX_REGISTER_TOOL`-style registrations only where the design says so.

**Never run a git write command** (`git commit`/`add`/`reset`/`checkout`/`stash`/`restore`)
in this repository — the worktree carries un-committed work from several waves and only the
user decides when it is committed. Read-only git (`log`, `show`, `diff`, `status`) is fine.
(A recon subagent once created a stray `wip` commit; it was reverted with
`git reset --mixed 236c023` and the worktree is un-committed again.)

### 6.1 Interface additions made while scaffolding (S1)

* `cli_print.h` gained the 256-colour / true-colour constructors
  (`ansi_prefix_256`, `ansi_prefix_true`, `colorful_text_256`, `colorful_text_true`), the
  named greys (`gray_text`, `gray_light_text`, `kGray256` = 245, `kGrayLight256` = 250) and
  the `PrintStream` state helpers (`print_word`, `last_char_was_newline`,
  `reset_print_state`) that `ui/stream.py` needs.
* `cli_args.h` gained `struct cli_help_segment` and the help text is now **generated**:
  `scripts/gen_cli_help.py` extracts `constants.py::HELP_STR`, splits it at the
  `{colorful_text(..., fg=Color.YELLOW)}` boundaries and writes
  `src/cli/cli_help_text.inc` (`--check` fails when it is out of date).  Verified
  byte-identical to the reference in both the plain and the coloured form (1828 chars,
  43 segments, 21 command names).
* `--dry-run` is wired in `main.cpp`'s bootstrap as soon as the config layer exists (S2);
  S5 moves that code into `cli_app.cpp`.
