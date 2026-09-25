// test_cli.cpp - Unit tests for the native CLI session store (cli/cli_session.h,
// step S3 of src/cli/PLAN.md §3.6).  S4/S6 extend this file with the stream /
// REPL / command coverage; the layout below follows .agents/skills/test.
//
// S6 adds the remaining module coverage and the reference-derived golden gate:
//   * cli/cli_args.h   - parse_args for every reference flag and native addition
//                        in both `--opt value` and `--opt=value` spellings, the
//                        --config/--provider distinction, -s/--skill-dir arity,
//                        the four subcommands with their remaining tokens, the
//                        usage-error paths, program_name and the cli_main
//                        exit-code contract (kExitOk/Config/Usage/Unsupported;
//                        kExitRuntime needs a network turn and is asserted by
//                        value only).
//   * cli/cli_common.h - trim/trim_ascii/is_blank with the Unicode spaces and
//                        the ASCII \x1c..\x1f set, the search/compare helpers,
//                        split/split_lines/join/replace_all, the path helpers,
//                        read_file/write_file (round-trip, append, failure),
//                        make_dirs/remove_all, random_hex, the time formatters,
//                        file_mtime_unix, get_env/set_env/first_env.
//   * cli/cli_tools.h  - the 25 module:attr -> registry-name entries,
//                        default_agent_tools()/agent_tool_table(), every
//                        malformed/unknown resolve_tool_path input, and the
//                        generated manifest-union goldens replayed through
//                        resolve_tool_path + ToolRegistry.
//   * cli/cli_config.h - both provider dialects (flat kimix and nested
//                        kimi-cli with a models table), url/base_url precedence,
//                        the model-default limit resolution, the error paths,
//                        type normalisation accepted by create_llm(), the agent
//                        manifest merge semantics, the deterministic reports and
//                        the generated _MODEL_DEFAULTS/typo/fuzz goldens
//                        (tests/unit/cli/cli_config_goldens.inc).
//   * cli/cli_app.h    - app_dry_run_report over the real ds_flash.json +
//                        agent_worker.json pair (skipped when the read-only
//                        reference checkout is absent).
//
// Coverage: anonymous vs named ids, session layout (<work_dir>/.kimix_cache/<id>),
// state.json round-trip including unknown-key preservation and the
// todos_json passthrough, the atomic rewrite (tmp + replace over an existing
// file, the case Windows refuses), history save/load round-trip with a
// multi-part message (thinking + text + tool calls + tool result), the
// context.jsonl / wire.jsonl record shapes, the context.db-only limitation,
// list() ordering / title fallback / usage, store_as (and its existing-target
// failure), copy_into (/load), close() deleting only anonymous sessions,
// clear_context (/clear) and export_markdown (/export).
//
// Framework: Boost.UT (tests/ut/ut.hpp).  No network access, no real session
// data: every test works in a fresh directory under the system temp directory.
// Reference sources (read-only): kimi_cli/session.py, session_state.py,
// utils/io.py, soul/context.py, wire/file.py, utils/export.py,
// kimix/utils/_globals.py.

#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "builtin_tools/todo_tool.h"

#include "agent/soul.h"

#include "cli/cli_app.h"
#include "cli/cli_args.h"
#include "cli/cli_commands.h"
#include "cli/cli_common.h"
#include "cli/cli_config.h"
#include "cli/cli_repl.h"
#include "cli/cli_session.h"
#include "cli/cli_print.h"
#include "cli/cli_stream.h"
#include "cli/cli_tools.h"
#include "builtin_tools/tool_registry.h"
#include "llm/llm.h"
#include <utility>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

namespace cli = kimix::cli;
using namespace boost::ut;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// A fresh, empty workspace: <temp>/<name>, removed and recreated.
kimix::string ws_dir(const char *name) {
    std::error_code ec;
    const kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) / name;
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

kimix::string file_text(const kimix::string &path) {
    kimix::string out;
    kimix::string error;
    const bool ok = cli::read_file(path, out, error);
    expect(ok) << "read_file(" << path << "): " << error;
    return out;
}

bool has_substr(kimix::string_view haystack, kimix::string_view needle) {
    return cli::contains(haystack, needle);
}

size_t count_occurrences(kimix::string_view haystack, kimix::string_view needle) {
    size_t count = 0;
    size_t from = 0;
    while (from < haystack.size()) {
        const size_t at = cli::find(haystack, needle, from);
        if (at == kimix::string::npos) {
            break;
        }
        ++count;
        from = at + needle.size();
    }
    return count;
}

size_t count_lines(kimix::string_view text) {
    size_t count = 0;
    for (char c : text) {
        if (c == '\n') {
            ++count;
        }
    }
    return count;
}

// Move a file's mtime by `seconds` so list() ordering is deterministic (the
// store reports whole unix seconds).
void bump_mtime(const kimix::string &path, int64_t seconds) {
    std::error_code ec;
    const kimix::filesystem::path p{kimix::string(path)};
    const auto now = kimix::filesystem::last_write_time(p, ec);
    if (ec) {
        expect(false) << "last_write_time(" << path << ")";
        return;
    }
    kimix::filesystem::last_write_time(p, now + std::chrono::seconds(seconds), ec);
    expect(!ec) << "bump_mtime(" << path << ")";
}

// Force two files onto exactly the same mtime (list()'s tie-break check).
void same_mtime(const kimix::string &from, const kimix::string &to) {
    std::error_code ec;
    const kimix::filesystem::path a{kimix::string(from)};
    const kimix::filesystem::path b{kimix::string(to)};
    const auto t = kimix::filesystem::last_write_time(a, ec);
    expect(!ec);
    kimix::filesystem::last_write_time(b, t, ec);
    expect(!ec);
}

const cli::session_info *find_info(const kimix::vector<cli::session_info> &rows,
                                   kimix::string_view id) {
    for (const cli::session_info &row : rows) {
        if (row.id == id) {
            return &row;
        }
    }
    return nullptr;
}

// A user turn, an assistant turn with thinking + text + one tool call, its tool
// result and a final assistant text - the multi-part history the round-trip
// test needs.
kimix::vector<kimix::llm::Message> sample_history() {
    kimix::vector<kimix::llm::Message> history;
    kimix::llm::Message user;
    user.role = "user";
    user.content = "hello";
    history.push_back(user);

    kimix::llm::Message assistant;
    assistant.role = "assistant";
    assistant.content = "Running it.";
    assistant.thinking = "I should run ls";
    assistant.thinking_signature = "sig-1";
    kimix::llm::ToolCall call;
    call.id = "call_1";
    call.name = "bash";
    call.arguments = R"({"command":"ls -la"})";
    assistant.tool_calls.push_back(call);
    history.push_back(assistant);

    kimix::llm::Message result;
    result.role = "tool";
    result.content = "total 0";
    result.tool_call_id = "call_1";
    history.push_back(result);

    kimix::llm::Message final;
    final.role = "assistant";
      final.content = "Done.";
      history.push_back(final);
      return history;
  }

  // -------------------------------------------------------------------------
  // Stream-renderer helpers (S4: cli/cli_stream.h)
  // -------------------------------------------------------------------------

  // Read back everything written to a tmpfile() as exact bytes.
  kimix::string read_stream(std::FILE *file) {
      expect(std::fflush(file) == 0);
      expect(std::fseek(file, 0, SEEK_END) == 0);
      const long size = std::ftell(file);
      expect(size >= 0);
      expect(std::fseek(file, 0, SEEK_SET) == 0);
      kimix::string out;
      if (size > 0) {
          out.resize(static_cast<size_t>(size));
          const size_t got = std::fread(out.data(), 1, out.size(), file);
          expect(got == out.size()) << "short read";
      }
      return out;
  }

  // Readable form of exact bytes: ESC -> \e, newline -> \n, control -> \xNN, so
  // a failing byte comparison shows what actually differs.
  kimix::string escaped(kimix::string_view text) {
      kimix::string out;
      for (char c : text) {
          const unsigned char u = static_cast<unsigned char>(c);
          if (u == 0x1b) {
              out.append("\\e");
          } else if (c == '\n') {
              out.append("\\n");
          } else if (u < 0x20 || u == 0x7f) {
              out.append(kimix::format("\\x{:02x}", static_cast<int32_t>(u)));
          } else {
              out.push_back(c);
          }
      }
      return out;
  }

  void expect_bytes(kimix::string_view actual, kimix::string_view expected,
                    const char *what) {
      const bool same = (actual == expected);
      expect(same) << what << ": actual=[" << escaped(actual) << "] expected=["
                   << escaped(expected) << "]";
  }

  kimix::llm::ToolCall mk_tool_call(const char *name, const char *arguments) {
      kimix::llm::ToolCall call;
      call.id = "call_1";
      call.name = name;
      call.arguments = arguments;
      return call;
  }

  cli::display_block mk_brief(const char *text) {
      cli::display_block block;
      block.kind = cli::display_block_kind::brief;
      block.text = text;
      return block;
  }

// ---------------------------------------------------------------------------
// S5 helpers: the app/repl/command layer driven in-process by a scripted
// IChatBackend (no network, no real provider).
// ---------------------------------------------------------------------------

// fd-level duplicates so a test can capture what cli_print / the stream
// renderer write to stdout+stderr (both are plain stdio streams).
int test_dup(int fd) {
#if defined(_WIN32)
    return _dup(fd);
#else
    return dup(fd);
#endif
}

int test_dup2(int from, int to) {
#if defined(_WIN32)
    return _dup2(from, to);
#else
    return dup2(from, to);
#endif
}

int test_fileno(std::FILE *stream) {
#if defined(_WIN32)
    return _fileno(stream);
#else
    return fileno(stream);
#endif
}

void test_close_fd(int fd) {
#if defined(_WIN32)
    _close(fd);
#else
    close(fd);
#endif
}

// One scripted chat step: the text/reasoning/tool calls one backend call
// returns (and streams through the chunk callback).
struct test_step {
    kimix::string content;
    kimix::string reasoning;
    kimix::vector<kimix::llm::ToolCall> calls;
};

// The scripted IChatBackend: returns steps[calls] and streams it exactly like a
// provider (text delta, reasoning delta, one chunk per tool call).
class test_backend : public kimix::agent::IChatBackend {
public:
    kimix::vector<test_step> steps;
    int32_t calls = 0;
    int64_t context_size = 1000;

    kimix::llm::ChatResult chat(const kimix::vector<kimix::llm::Message> &,
                                const kimix::vector<kimix::llm::Tool> &,
                                const kimix::llm::ChunkCallback &on_chunk) override {
        kimix::llm::ChatResult result;
        result.ok = true;
        if (static_cast<size_t>(calls) < steps.size()) {
            const test_step &step = steps[static_cast<size_t>(calls)];
            result.content = step.content;
            result.reasoning = step.reasoning;
            result.tool_calls = step.calls;
        }
        ++calls;
        if (on_chunk) {
            if (!result.reasoning.empty()) {
                kimix::llm::Chunk chunk;
                chunk.ok = true;
                chunk.reasoning = result.reasoning;
                on_chunk(chunk);
            }
            if (!result.content.empty()) {
                kimix::llm::Chunk chunk;
                chunk.ok = true;
                chunk.content = result.content;
                on_chunk(chunk);
            }
            for (const kimix::llm::ToolCall &call : result.tool_calls) {
                kimix::llm::Chunk chunk;
                chunk.ok = true;
                chunk.tool_calls.push_back(call);
                on_chunk(chunk);
            }
        }
        return result;
    }

    int64_t max_context_size() const override { return context_size; }
    kimix::string model_name() const override { return "scripted-test"; }
};

// Redirect fd 1+2 into `path` for the duration of a test (the CLI's prints do
// not go through a swappable stream).
class output_capture {
public:
    bool begin(const kimix::string &path) {
        path_ = path;
        std::fflush(stdout);
        std::fflush(stderr);
        file_ = std::fopen(path.c_str(), "wb");
        if (file_ == nullptr) {
            return false;
        }
        saved_out_ = test_dup(1);
        saved_err_ = test_dup(2);
        test_dup2(test_fileno(file_), 1);
        test_dup2(test_fileno(file_), 2);
        return true;
    }

    kimix::string end() {
        std::fflush(stdout);
        std::fflush(stderr);
        if (saved_out_ >= 0) {
            test_dup2(saved_out_, 1);
            test_close_fd(saved_out_);
            saved_out_ = -1;
        }
        if (saved_err_ >= 0) {
            test_dup2(saved_err_, 2);
            test_close_fd(saved_err_);
            saved_err_ = -1;
        }
        if (file_ != nullptr) {
            std::fclose(file_);
            file_ = nullptr;
        }
        kimix::string text;
        kimix::string error;
        cli::read_file(path_, text, error);
        return text;
    }

private:
    kimix::string path_;
    std::FILE *file_ = nullptr;
    int saved_out_ = -1;
    int saved_err_ = -1;
};

// Read everything currently written to a FILE* (position preserved).
kimix::string slurp(std::FILE *stream) {
    if (stream == nullptr) {
        return {};
    }
    std::fflush(stream);
    const long pos = std::ftell(stream);
    std::fseek(stream, 0, SEEK_SET);
    kimix::string out;
    char buffer[4096];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), stream)) > 0) {
        out.append(buffer, read);
    }
    if (pos >= 0) {
        std::fseek(stream, pos, SEEK_SET);
    }
    return out;
}

// Write `lines` as a newline-terminated script file and return its path.
kimix::string script_file(const kimix::string &dir, const char *name,
                          const kimix::vector<kimix::string> &lines) {
    const kimix::string path = cli::join_path(dir, name);
    kimix::string text;
    for (const kimix::string &line : lines) {
        text += line;
        text += '\n';
    }
    kimix::string error;
    if (!cli::write_file(path, text, error)) {
        expect(false) << "write_file(" << path << "): " << error;
    }
    return path;
}

// A configured app_context with a scripted backend (no network access): the
// provider JSON is real, so the config -> soul wiring (tools, limits, prompts)
// is exercised for real.
struct app_fixture {
    kimix::string work;
    kimix::string provider;
    cli::cli_options opts;
    cli::app_context app;
    test_backend backend;
    std::FILE *render_out = nullptr;
    cli::stream_renderer renderer{true, true};
    kimix::string error;

    bool init(const char *name) {
        work = ws_dir(name);
        provider = cli::join_path(work, "provider.json");
        kimix::string write_error;
        const kimix::string json =
            "{\"model\":\"scripted-test-model\",\"type\":\"openai\","
            "\"url\":\"http://127.0.0.1:1/v1\",\"api_key\":\"test\","
            "\"max_context_size\":1000,\"max_tokens\":100}";
        if (!cli::write_file(provider, json, write_error)) {
            error = write_error;
            return false;
        }
        opts.config_path = provider;
        opts.config_is_provider_only = true;
        opts.work_dir = work;
        opts.no_color = true;
        render_out = std::tmpfile();
        renderer.set_output(render_out);
        app.renderer = &renderer;
        return cli::app_init(opts, app, error, &backend);
    }

    kimix::string rendered() { return slurp(render_out); }
    kimix::string session_file(const char *name) {
        return cli::join_path(app.store.dir(), name);
    }
    void shutdown() {
        app.soul.reset();
        app.session.reset();
        kimix::string close_error;
        app.store.close(true, close_error);
    }
};

// ---------------------------------------------------------------------------
// S6 helpers: cli_args parsing, the generated config goldens
// (cli_config_goldens.inc) and the temp-file plumbing the cli_common /
// cli_config sections need.  Every helper is `cli_`-prefixed so a unity build
// cannot collide with another test file.
// ---------------------------------------------------------------------------

// parse_args() over a dynamic argument vector; "kimix_cli" is prepended as
// argv[0] so the program name is stable.
bool cli_parse(const kimix::vector<kimix::string> &args, cli::cli_options &out) {
    kimix::vector<kimix::string> full;
    full.push_back(kimix::string("kimix_cli"));
    for (const kimix::string &arg : args) {
        full.push_back(arg);
    }
    kimix::vector<char *> argv;
    argv.reserve(full.size());
    for (kimix::string &arg : full) {
        argv.push_back(const_cast<char *>(arg.c_str()));
    }
    return cli::parse_args(static_cast<int>(argv.size()), argv.data(), out);
}

// A '|'-separated golden column -> tokens (an empty column yields no token).
kimix::vector<kimix::string> cli_golden_tokens(const char *column) {
    kimix::vector<kimix::string> out;
    if (column == nullptr || column[0] == '\0') {
        return out;
    }
    const kimix::string_view text(column);
    size_t start = 0;
    for (;;) {
        const size_t bar = text.find('|', start);
        const size_t end = (bar == kimix::string_view::npos) ? text.size() : bar;
        out.emplace_back(text.substr(start, end - start));
        if (bar == kimix::string_view::npos) {
            break;
        }
        start = bar + 1;
    }
    return out;
}

// Join `tokens` with `separator`.
kimix::string cli_golden_join(const kimix::vector<kimix::string> &tokens,
                              const char *separator) {
    kimix::string out;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i != 0) {
            out += separator;
        }
        out += tokens[i];
    }
    return out;
}

// Write `json` to <dir>/<name> (the write is expected to succeed).
kimix::string cli_write_json(const kimix::string &dir, const char *name,
                             kimix::string_view json) {
    const kimix::string path = cli::join_path(dir, name);
    kimix::string error;
    if (!cli::write_file(path, json, error)) {
        expect(false) << "write_file(" << path << "): " << error;
    }
    return path;
}

bool cli_load_provider_json(const kimix::string &dir, const char *name,
                            kimix::string_view json, cli::provider_config &out,
                            kimix::string &error) {
    const kimix::string path = cli_write_json(dir, name, json);
    return cli::load_provider_config(path, out, error);
}

bool cli_load_agent_json(const kimix::string &dir, const char *name,
                         kimix::string_view json, cli::agent_config &out,
                         kimix::string &error) {
    const kimix::string path = cli_write_json(dir, name, json);
    return cli::load_agent_config(path, out, error);
}

// Value of `key` in a pair list ("" when absent).
kimix::string cli_pair_value(
    const kimix::vector<std::pair<kimix::string, kimix::string>> &pairs,
    kimix::string_view key) {
    for (const std::pair<kimix::string, kimix::string> &kv : pairs) {
        if (kv.first == key) {
            return kv.second;
        }
    }
    return {};
}

// True when one warning contains `needle`.
bool cli_has_warning(const kimix::vector<kimix::string> &warnings, kimix::string_view needle) {
    for (const kimix::string &warning : warnings) {
        if (cli::contains(warning, needle)) {
            return true;
        }
    }
    return false;
}

// True when the vector contains `value` (an exact match).
bool cli_has_value(const kimix::vector<kimix::string> &values, kimix::string_view value) {
    for (const kimix::string &item : values) {
        if (item == value) {
            return true;
        }
    }
    return false;
}

// The reference checkout the runtime-only checks read (KIMI_AGENT_ROOT, else
// the repo's documented default).
kimix::string cli_reference_root() {
    kimix::string value;
    if (cli::get_env("KIMI_AGENT_ROOT", value) && !value.empty()) {
        return value;
    }
    return kimix::string("C:/dev/kimi-agent");
}

// Value of the first `"key": "value"` pair of a JSON text.  A tiny scanner,
// used only to prove that a real config's api_key never reaches a report.
kimix::string cli_json_field(kimix::string_view text, kimix::string_view key) {
    const kimix::string needle = kimix::string("\"") + kimix::string(key) + "\"";
    const size_t at = cli::find(text, needle);
    if (at == kimix::string_view::npos) {
        return {};
    }
    const size_t colon = cli::find(text, ":", at + needle.size());
    if (colon == kimix::string_view::npos) {
        return {};
    }
    size_t begin = colon + 1;
    while (begin < text.size() && text[begin] != '"') {
        ++begin;
    }
    if (begin >= text.size()) {
        return {};
    }
    const size_t end = cli::find(text, "\"", begin + 1);
    if (end == kimix::string_view::npos) {
        return {};
    }
    return kimix::string(text.substr(begin + 1, end - begin - 1));
}

} // namespace

// ---------------------------------------------------------------------------
// Reference-derived expectations (scripts/gen_cli_config_goldens.py).  The file
// is generated data: never edit it by hand, regenerate it and keep
// `python scripts/gen_cli_config_goldens.py --check` green.
// ---------------------------------------------------------------------------
#include "cli_config_goldens.inc"

int main() {
    using namespace boost::ut;

    // =======================================================================
    // Session ids, layout and open()
    // =======================================================================
    "session_ids_and_layout"_test = [] {
        const kimix::string ws = ws_dir("kimix_cli_sess_ids");
        kimix::string error;

        cli::session_store store;
        expect(store.open(ws, "", false, error)) << error;
        expect(store.anonymous());
        // uuid4().hex shape: 32 lowercase hex characters.
        expect(store.id().size() == 32u) << store.id();
        bool hex = !store.id().empty();
        for (char c : store.id()) {
            const bool digit = c >= '0' && c <= '9';
            const bool alpha = c >= 'a' && c <= 'f';
            if (!digit && !alpha) {
                hex = false;
            }
        }
        expect(hex) << store.id();
        expect(store.work_dir() == cli::absolute_path(ws));
        expect(cli::session_store::cache_root(ws) ==
               cli::join_path(cli::absolute_path(ws), ".kimix_cache"));
        expect(cli::session_store::session_dir(ws, store.id()) == store.dir());
        expect(cli::dir_exists(store.dir()));
        const kimix::string anon_dir = store.dir();
        // An anonymous session's directory goes away on close().
        expect(store.close(true, error)) << error;
        expect(!cli::dir_exists(anon_dir));

        cli::session_store named;
        expect(named.open(ws, "my-session", false, error)) << error;
        expect(!named.anonymous());
        expect(named.id() == "my-session");
        expect(named.dir() ==
               cli::join_path(cli::session_store::cache_root(ws), "my-session"));
        // A named session survives close(), even with delete_if_anonymous.
        expect(named.close(true, error)) << error;
        expect(cli::dir_exists(named.dir()));
    };

    // =======================================================================
    // open(): resume semantics
    // =======================================================================
    "session_open_resume_semantics"_test = [] {
        const kimix::string ws = ws_dir("kimix_cli_sess_resume");
        kimix::string error;

        // resume=true creates the directory when it is absent.
        cli::session_store resumed;
        expect(resumed.open(ws, "resumed", true, error)) << error;
        expect(cli::dir_exists(resumed.dir()));
        const kimix::string state_path =
            cli::join_path(resumed.dir(), "state.json");
        cli::session_state initial;
        initial.custom_title = "Kept";
        expect(resumed.save_state(initial, error)) << error;
        // An unrelated file in the directory must never be touched.
        const kimix::string keep = cli::join_path(resumed.dir(), "keep.txt");
        expect(cli::write_file(keep, "keep", error)) << error;

        cli::session_store again;
        expect(again.open(ws, "resumed", true, error)) << error;
        cli::session_state loaded;
        expect(again.load_state(loaded, error)) << error;
        expect(loaded.custom_title == "Kept");
        expect(cli::file_exists(keep));

        // resume=false resets the files the store owns, nothing else.
        cli::session_store fresh;
        expect(fresh.open(ws, "resumed", false, error)) << error;
        cli::session_state cleared;
        expect(fresh.load_state(cleared, error)) << error;
        expect(cleared.custom_title.empty());
        expect(!cli::file_exists(state_path));
        expect(cli::file_exists(keep));
    };

    // =======================================================================
    // state.json: round-trip + unknown-key preservation
    // =======================================================================
    "session_state_roundtrip_preserves_unknown_keys"_test = [] {
        const kimix::string ws = ws_dir("kimix_cli_state_keys");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "state-session", false, error)) << error;
        const kimix::string path = cli::join_path(store.dir(), "state.json");

        // A state.json as the Python writer leaves it (session_state.py order)
        // with two keys the native reader does not model.
        const kimix::string python_state =
            R"({
  "version": 1,
  "approval": {
    "yolo": false,
    "afk": true,
    "auto_approve_actions": [
      "Shell"
    ]
  },
  "custom_title": "Python Title",
  "title_generated": true,
  "title_generate_attempts": 3,
  "wire_mtime": 1712345678.5,
  "archived": true,
  "archived_at": 1712345600.25,
  "published_url": "https://example.invalid/x",
  "nested_unknown": {
    "a": [
      1,
      2
    ]
  }
})";
        expect(cli::write_file(path, python_state, error)) << error;

        cli::session_state state;
        expect(store.load_state(state, error)) << error;
        expect(state.custom_title == "Python Title");
        expect(state.title_generated);
        expect(state.title_generate_attempts == 3);
        expect(!state.yolo);
        expect(state.afk);
        expect(state.auto_approve_actions); // non-empty array -> true
        expect(state.archived);
        expect(state.todos_json == "[]");

        state.custom_title = "Native Title";
        state.yolo = true;
        state.afk = false;
        state.additional_dirs.push_back("C:/extra");
        expect(store.save_state(state, error)) << error;

        const kimix::string rewritten = file_text(path);
        // The known fields are rewritten...
        expect(has_substr(rewritten, "\"custom_title\": \"Native Title\""));
        expect(has_substr(rewritten, "\"afk\": false"));
        expect(has_substr(rewritten, "\"yolo\": true"));
        expect(has_substr(rewritten, "\"additional_dirs\": [\n    \"C:/extra\"\n  ]"));
        // ... 2-space indented like orjson OPT_INDENT_2, in the reference order.
        expect(cli::starts_with(rewritten, "{\n  \"version\": 1,\n  \"approval\": {\n"));
        expect(rewritten.find("\"custom_title\"") <
               rewritten.find("\"published_url\""))
            << "managed keys stay in the reference order";
        // ... and nothing the native reader does not model is lost.
        expect(has_substr(rewritten, "\"published_url\": \"https://example.invalid/x\""));
        expect(has_substr(rewritten, "\"nested_unknown\": {\n    \"a\": [\n      1,\n      2\n    ]\n  }"));
        expect(has_substr(rewritten, "\"title_generated\": true"));
        expect(has_substr(rewritten, "\"title_generate_attempts\": 3"));
        expect(has_substr(rewritten, "\"wire_mtime\": 1712345678.5"));
        expect(has_substr(rewritten, "\"archived\": true"));
        expect(has_substr(rewritten, "\"archived_at\": 1712345600.25"));
        expect(has_substr(rewritten, "\"auto_approve_actions\": [\n      \"Shell\"\n    ]"));
        expect(has_substr(rewritten, "\"archived_todos\": []"));
        expect(has_substr(rewritten, "\"todo_stack\": []"));

        // A second load sees the native values.
        cli::session_state reloaded;
        expect(store.load_state(reloaded, error)) << error;
        expect(reloaded.custom_title == "Native Title");
        expect(reloaded.yolo);
        expect(!reloaded.afk);
        expect(reloaded.additional_dirs.size() == 1u);
        expect(reloaded.archived);
    };

    // =======================================================================
    // state.json: missing / corrupt files
    // =======================================================================
    "session_state_missing_and_corrupt"_test = [] {
        const kimix::string ws = ws_dir("kimix_cli_state_corrupt");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "corrupt-session", false, error)) << error;

        // Missing file -> the reference defaults.
        cli::session_state state;
        expect(store.load_state(state, error)) << error;
        expect(state.custom_title.empty());
        expect(state.yolo); // the native CLI's default
        expect(!state.archived);
        expect(state.todos_json == "[]");

        // Non-JSON content -> an explicit error (never a silent default).
        const kimix::string path = cli::join_path(store.dir(), "state.json");
        expect(cli::write_file(path, "{ not json", error)) << error;
        expect(!store.load_state(state, error));
        expect(has_substr(error, "corrupt state file")) << error;
    };

    // =======================================================================
    // state.json: atomic rewrite over an existing file
    // =======================================================================
    "session_state_atomic_rewrite"_test = [] {
        const kimix::string ws = ws_dir("kimix_cli_state_atomic");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "atomic-session", false, error)) << error;
        const kimix::string path = cli::join_path(store.dir(), "state.json");

        cli::session_state first;
        first.custom_title = "First";
        expect(store.save_state(first, error)) << error;
        expect(cli::file_exists(path));
        expect(!cli::file_exists(path + ".tmp"));

        // The second write replaces an existing state.json - the rename Windows
        // refuses without removing the target first.
        cli::session_state second;
        second.custom_title = "Second";
        second.yolo = false;
        expect(store.save_state(second, error)) << error;
        expect(!cli::file_exists(path + ".tmp"));

        const kimix::string text = file_text(path);
        expect(has_substr(text, "\"custom_title\": \"Second\""));
        expect(!has_substr(text, "First")) << text;
        expect(count_occurrences(text, "\"version\": 1") == 1u);
        expect(count_occurrences(text, "\"custom_title\"") == 1u);
        // A third write still succeeds and the file stays valid JSON.
        cli::session_state third;
        third.custom_title = "Third";
        expect(store.save_state(third, error)) << error;
        expect(has_substr(file_text(path), "\"custom_title\": \"Third\""));
    };

    // =======================================================================
    // state.json: todos_json passthrough
    // =======================================================================
    "session_state_todos_json_roundtrip"_test = [] {
        const kimix::string ws = ws_dir("kimix_cli_state_todos");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "todos-session", false, error)) << error;
        const kimix::string path = cli::join_path(store.dir(), "state.json");

        const kimix::string todos =
            R"([{"title":"wire the store","status":"in_progress","notes":null,"children":[]}])";
        cli::session_state state;
        state.todos_json = todos;
        expect(store.save_state(state, error)) << error;
        const kimix::string text = file_text(path);
        // Written as the state.json "todos" array, pretty-printed with the rest
        // of the document (member order and unknown item keys survive).
        expect(has_substr(text,
                          "\"todos\": [\n    {\n      \"title\": \"wire the store\","
                          "\n      \"status\": \"in_progress\",\n      \"notes\": null,"
                          "\n      \"children\": []\n    }\n  ]"))
            << text;

        cli::session_state loaded;
        expect(store.load_state(loaded, error)) << error;
        expect(loaded.todos_json == todos) << loaded.todos_json;

        // A non-array passthrough is refused and leaves the file alone.
        cli::session_state bad;
        bad.todos_json = "{\"todos\": []}";
        expect(!store.save_state(bad, error));
        expect(has_substr(error, "todos_json")) << error;
        expect(file_text(path) == text);
    };

    // =======================================================================
    // context.jsonl + wire.jsonl: history round-trip
    // =======================================================================
    "session_history_roundtrip"_test = [] {
        const kimix::string ws = ws_dir("kimix_cli_history");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "history-session", false, error)) << error;
        const kimix::vector<kimix::llm::Message> history = sample_history();
        expect(store.save_history(history, error)) << error;

        // --- context.jsonl: one reference record per line ------------------
        const kimix::string context_path =
            cli::join_path(store.dir(), "context.jsonl");
        expect(cli::file_exists(context_path));
        expect(!cli::file_exists(context_path + ".tmp"));
        const kimix::string context = file_text(context_path);
        expect(count_lines(context) == 4u) << context;
        expect(has_substr(context, "{\"role\":\"user\",\"content\":\"hello\"}\n"));
        // Multi-part assistant message: thinking + text + one tool call.
        const kimix::string assistant_line =
            R"({"role":"assistant","content":[{"type":"think","think":"I should run ls","encrypted":"sig-1"},{"type":"text","text":"Running it."}],"tool_calls":[{"type":"function","id":"call_1","function":{"name":"bash","arguments":"{\"command\":\"ls -la\"}"}}]})";
        expect(has_substr(context, assistant_line + "\n")) << context;
        expect(has_substr(
            context,
            "{\"role\":\"tool\",\"content\":\"total 0\",\"tool_call_id\":\"call_1\"}\n"));
        expect(has_substr(context,
                          "{\"role\":\"assistant\",\"content\":\"Done.\"}\n"));

        // --- wire.jsonl: header + one record per message part --------------
        const kimix::string wire = file_text(cli::join_path(store.dir(), "wire.jsonl"));
        expect(cli::starts_with(wire,
                                "{\"type\":\"metadata\",\"protocol_version\":\"1.11\"}\n"))
            << wire;
        expect(has_substr(wire, "{\"timestamp\":"));
        expect(has_substr(
            wire,
            "\"message\":{\"type\":\"TurnBegin\",\"payload\":{\"user_input\":\"hello\"}}"));
        expect(has_substr(
            wire,
            "\"message\":{\"type\":\"ThinkPart\",\"payload\":{\"type\":\"think\",\"think\":\"I should run ls\",\"encrypted\":\"sig-1\"}}"));
        expect(has_substr(wire,
                          "\"message\":{\"type\":\"TextPart\",\"payload\":{\"type\":\"text\",\"text\":\"Running it.\"}}"));
        expect(has_substr(
            wire,
            "\"message\":{\"type\":\"ToolCall\",\"payload\":{\"type\":\"function\",\"id\":\"call_1\",\"function\":{\"name\":\"bash\",\"arguments\":\"{\\\"command\\\":\\\"ls -la\\\"}\"}}"));
        expect(has_substr(
            wire,
            "\"message\":{\"type\":\"ToolResult\",\"payload\":{\"tool_call_id\":\"call_1\",\"return_value\":{\"is_error\":false,\"output\":\"total 0\",\"message\":\"\",\"display\":[]}}"));
        // header + TurnBegin + ThinkPart + TextPart + ToolCall + ToolResult + TextPart
        expect(count_lines(wire) == 7u) << wire;

        // --- load_history rebuilds the messages ---------------------------
        kimix::vector<kimix::llm::Message> back;
        expect(store.load_history(back, error)) << error;
        expect(back.size() == history.size());
        expect(back[0].role == "user");
        expect(back[0].content == "hello");
        expect(back[1].role == "assistant");
        expect(back[1].thinking == "I should run ls");
        expect(back[1].thinking_signature == "sig-1");
        expect(back[1].content == "Running it.");
        expect(back[1].tool_calls.size() == 1u);
        expect(back[1].tool_calls[0].id == "call_1");
        expect(back[1].tool_calls[0].type == "function");
        expect(back[1].tool_calls[0].name == "bash");
        expect(back[1].tool_calls[0].arguments == R"({"command":"ls -la"})");
        expect(back[2].role == "tool");
        expect(back[2].content == "total 0");
        expect(back[2].tool_call_id == "call_1");
        expect(back[3].role == "assistant");
        expect(back[3].content == "Done.");

        // Re-saving what was loaded changes nothing about the record shape.
        expect(store.save_history(back, error)) << error;
        expect(file_text(context_path) == context);
        // The existing header's protocol version is kept on rewrite.
        expect(cli::starts_with(file_text(cli::join_path(store.dir(), "wire.jsonl")),
                                "{\"type\":\"metadata\",\"protocol_version\":\"1.11\"}\n"));
    };

    // =======================================================================
    // load_history: a SQLite-only history must say so
    // =======================================================================
    "session_history_context_db_only"_test = [] {
        const kimix::string ws = ws_dir("kimix_cli_history_db");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "db-session", false, error)) << error;
        expect(cli::write_file(cli::join_path(store.dir(), "context.db"), "sqlite",
                               error))
            << error;

        kimix::vector<kimix::llm::Message> history;
        expect(!store.load_history(history, error));
        expect(history.empty());
        expect(has_substr(error, "context.db")) << error;
        expect(has_substr(error, "SQLite")) << error;
    };

    // =======================================================================
    // state.json: float formatting matches orjson
    // =======================================================================
    "session_state_float_format_matches_orjson"_test = [] {
        // orjson (the reference's writer) uses Ryu's d2s notation: fixed form
        // while -5 <= decimal exponent <= 15, scientific beyond, "1.0" style
        // markers for integral values.  Verified against orjson 3.11.
        const kimix::string ws = ws_dir("kimix_cli_state_floats");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "float-session", false, error)) << error;
        const kimix::string path = cli::join_path(store.dir(), "state.json");
        const kimix::string original =
            "{\"floats\":[1712345678.5,9900000000000000.0,1e16,1.5e-5,1e-6,0.0001,"
            "0.25,1234.0,1e20,3.14159265358979]}";
        expect(cli::write_file(path, original, error)) << error;
        cli::session_state state;
        expect(store.load_state(state, error)) << error;
        expect(store.save_state(state, error)) << error;
        const kimix::string text = file_text(path);
        expect(has_substr(text,
                          "\"floats\": [\n    1712345678.5,\n    9900000000000000.0,"
                          "\n    1e+16,\n    0.000015,\n    1e-6,\n    0.0001,\n    0.25,"
                          "\n    1234.0,\n    1e+20,\n    3.14159265358979\n  ]"))
            << text;
        expect(has_substr(text, "\"version\": 1")) << text;
    };

    // =======================================================================
    // state.json: interop with the built-in todo tool's own writer/reader
    // =======================================================================
    "session_state_interops_with_todo_tool"_test = [] {
        // The todo tool persists the same <state_dir>/state.json
        // (builtin_tools/todo_tool.h: state_file_path + load/save_state_file).
        // The store's rewrite must stay loadable by the tool and the tool's
        // rewrite (which merges its two keys into the existing document) must
        // stay loadable by the store - including the unknown keys.
        const kimix::string ws = ws_dir("kimix_cli_state_todo_interop");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "interop-session", false, error)) << error;
        const kimix::string path = cli::join_path(store.dir(), "state.json");

        cli::session_state state;
        state.custom_title = "Interop";
        state.todos_json =
            R"([{"title":"store side","status":"in_progress","notes":null,"children":[]}])";
        expect(store.save_state(state, error)) << error;

        namespace todo = kimix::builtin_tools::todo;
        todo::todo_state todos;
        expect(todo::load_state_file(path, todos, error)) << error;
        expect(todos.todos.size() == 1u);
        expect(todos.todos[0].content == "store side");
        expect(todos.todos[0].status == todo::todo_status::in_progress);

        // The tool's own write is read back by the store (its writer keeps the
        // unknown / unmodelled keys of the document it merges into).
        todos.todos.push_back(todo::todo_item{});
        todos.todos[1].content = "tool side";
        expect(todo::save_state_file(path, todos, error)) << error;
        cli::session_state reloaded;
        expect(store.load_state(reloaded, error)) << error;
        expect(reloaded.custom_title == "Interop");
        expect(has_substr(reloaded.todos_json, "store side")) << reloaded.todos_json;
        expect(has_substr(reloaded.todos_json, "tool side")) << reloaded.todos_json;
    };

    // =======================================================================
    // list(): ordering, title fallback, usage
    // =======================================================================
    "session_list_ordering_and_title"_test = [] {
        const kimix::string ws = ws_dir("kimix_cli_list");
        kimix::string error;

        struct row {
            const char *id;
            const char *title; // nullptr -> Untitled
            int64_t offset;
        };
        const row rows[] = {{"alpha", "Alpha", 0},
                            {"beta", nullptr, 10},
                            {"charlie", "Charlie", 20}};
        for (const row &r : rows) {
            cli::session_store store;
            expect(store.open(ws, r.id, false, error)) << error;
            cli::session_state state;
            if (r.title != nullptr) {
                state.custom_title = r.title;
            }
            expect(store.save_state(state, error)) << error;
            expect(store.save_history({}, error)) << error;
            bump_mtime(cli::join_path(store.dir(), "context.jsonl"), r.offset);
        }

        const kimix::vector<cli::session_info> listed = cli::session_store::list(ws);
        expect(listed.size() == 3u) << listed.size();
        expect(listed[0].id == "charlie");
        expect(listed[1].id == "beta");
        expect(listed[2].id == "alpha");
        expect(listed[0].title == "Charlie");
        expect(listed[1].title == "Untitled");
        expect(listed[2].title == "Alpha");
        expect(listed[0].updated_at > listed[1].updated_at);
        expect(listed[1].updated_at > listed[2].updated_at);
        expect(listed[0].updated_at > 0);
        // Nothing recorded a usage yet.
        expect(!listed[0].usage_known);
        expect(listed[0].context_tokens == 0);

        // set_usage + save_state round-trips into the list row.
        cli::session_store charlie;
        expect(charlie.open(ws, "charlie", true, error)) << error;
        charlie.set_usage(0.25, 1234, true);
        cli::session_state state;
        expect(charlie.load_state(state, error)) << error;
        expect(charlie.save_state(state, error)) << error;
        const kimix::vector<cli::session_info> with_usage =
            cli::session_store::list(ws);
        const cli::session_info *row_charlie = find_info(with_usage, "charlie");
        expect(row_charlie != nullptr);
        if (row_charlie != nullptr) {
            expect(row_charlie->usage_known);
            expect(row_charlie->context_usage > 0.24 &&
                   row_charlie->context_usage < 0.26)
                << row_charlie->context_usage;
            expect(row_charlie->context_tokens == 1234);
        }
        // A session without any file has no timestamp at all.
        std::error_code ec;
        kimix::filesystem::create_directories(
            kimix::filesystem::path(kimix::string(
                cli::join_path(cli::session_store::cache_root(ws), "empty-session"))),
            ec);
        const kimix::vector<cli::session_info> with_empty =
            cli::session_store::list(ws);
        const cli::session_info *row_empty = find_info(with_empty, "empty-session");
        expect(row_empty != nullptr);
        if (row_empty != nullptr) {
            expect(row_empty->updated_at == 0);
            expect(row_empty->title == "Untitled");
            expect(with_empty.back().id == "empty-session"); // 0 sorts last
        }
        // Files inside the cache root are not sessions.
        expect(cli::write_file(
                   cli::join_path(cli::session_store::cache_root(ws), "stray.json"),
                   "{}", error))
            << error;
        expect(cli::session_store::list(ws).size() == 4u);
        // A cache root that does not exist lists nothing.
        expect(cli::session_store::list(ws + "_missing").empty());
    };

    "session_list_tie_break_by_id"_test = [] {
        const kimix::string ws = ws_dir("kimix_cli_list_tie");
        kimix::string error;
        for (const char *id : {"bbb", "aaa"}) {
            cli::session_store store;
            expect(store.open(ws, id, false, error)) << error;
            expect(store.save_history({}, error)) << error;
        }
        same_mtime(
            cli::join_path(cli::join_path(cli::session_store::cache_root(ws), "bbb"),
                           "context.jsonl"),
            cli::join_path(cli::join_path(cli::session_store::cache_root(ws), "aaa"),
                           "context.jsonl"));
        const kimix::vector<cli::session_info> listed = cli::session_store::list(ws);
        expect(listed.size() == 2u);
        expect(listed[0].updated_at == listed[1].updated_at);
        expect(listed[0].id == "aaa") << listed[0].id;
        expect(listed[1].id == "bbb");
    };

    // =======================================================================
    // store_as (/store)
    // =======================================================================
    "session_store_as"_test = [] {
        const kimix::string ws = ws_dir("kimix_cli_store_as");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "src-session", false, error)) << error;
        cli::session_state state;
        state.custom_title = "Source";
        expect(store.save_state(state, error)) << error;
        expect(store.save_history(sample_history(), error)) << error;
        const kimix::string source_dir = store.dir();

        expect(store.store_as("copy-1", error)) << error;
        const kimix::string target = cli::session_store::session_dir(ws, "copy-1");
        expect(cli::dir_exists(target));
        expect(cli::file_exists(cli::join_path(target, "state.json")));
        expect(file_text(cli::join_path(target, "context.jsonl")) ==
               file_text(cli::join_path(source_dir, "context.jsonl")));
        expect(has_substr(file_text(cli::join_path(target, "state.json")),
                          "\"custom_title\": \"Source\""));
        // The source session stays open and current.
        expect(store.id() == "src-session");
        expect(store.dir() == source_dir);
        expect(!store.anonymous());

        // shutil.copytree refuses an existing target.
        expect(!store.store_as("copy-1", error));
        expect(has_substr(error, "already exists")) << error;
        // ... and copying onto itself.
        expect(!store.store_as("src-session", error));
        expect(has_substr(error, "must differ")) << error;
    };

    // =======================================================================
    // copy_into (/load)
    // =======================================================================
    "session_copy_into"_test = [] {
        const kimix::string ws = ws_dir("kimix_cli_copy_into");
        kimix::string error;

        cli::session_store source;
        expect(source.open(ws, "loaded-session", false, error)) << error;
        cli::session_state state;
        state.custom_title = "Loaded";
        expect(source.save_state(state, error)) << error;
        expect(source.save_history(sample_history(), error)) << error;

        // /load: a fresh anonymous session that receives the named session.
        cli::session_store fresh;
        expect(fresh.open(ws, "", false, error)) << error;
        const kimix::string anon_id = fresh.id();
        const kimix::string anon_dir = fresh.dir();
        expect(fresh.anonymous());
        expect(fresh.copy_into("loaded-session", error)) << error;
        // The anonymous id and directory are kept (the reference loads into the
        // fresh uuid and keeps it).
        expect(fresh.id() == anon_id);
        expect(fresh.dir() == anon_dir);
        cli::session_state loaded;
        expect(fresh.load_state(loaded, error)) << error;
        expect(loaded.custom_title == "Loaded");
        kimix::vector<kimix::llm::Message> history;
        expect(fresh.load_history(history, error)) << error;
        expect(history.size() == 4u);
        expect(history[1].tool_calls.size() == 1u);

        // An unknown source, and the current directory as the source.
        expect(!fresh.copy_into("nope", error));
        expect(has_substr(error, "not found")) << error;
        expect(!fresh.copy_into(anon_id, error));
        expect(has_substr(error, "current session")) << error;
        expect(fresh.close(true, error)) << error;
    };

    // =======================================================================
    // clear_context (/clear)
    // =======================================================================
    "session_clear_context"_test = [] {
        const kimix::string ws = ws_dir("kimix_cli_clear");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "clear-session", false, error)) << error;
        cli::session_state state;
        state.custom_title = "Clear me";
        expect(store.save_state(state, error)) << error;
        expect(store.save_history(sample_history(), error)) << error;
        store.set_usage(0.5, 5000, true);
        expect(store.save_state(state, error)) << error;
        const kimix::string dir = store.dir();
        expect(cli::file_exists(cli::join_path(dir, "context.jsonl")));
        expect(cli::file_exists(cli::join_path(dir, "wire.jsonl")));
        expect(cli::file_exists(cli::join_path(dir, "state.json")));

        expect(store.clear_context(error)) << error;
        expect(!cli::file_exists(cli::join_path(dir, "context.jsonl")));
        expect(!cli::file_exists(cli::join_path(dir, "wire.jsonl")));
        // The reference's Session.clear also drops state.json, which is what
        // resets the recorded usage.
        expect(!cli::file_exists(cli::join_path(dir, "state.json")));
        expect(cli::dir_exists(dir));
        expect(store.id() == "clear-session");
        const kimix::vector<cli::session_info> rows = cli::session_store::list(ws);
        const cli::session_info *row = find_info(rows, "clear-session");
        expect(row != nullptr);
        if (row != nullptr) {
            expect(!row->usage_known);
            expect(row->title == "Untitled");
        }
        // The session is still usable afterwards.
        kimix::vector<kimix::llm::Message> history;
        expect(store.load_history(history, error)) << error;
        expect(history.empty());
    };

    // =======================================================================
    // export_markdown (/export)
    // =======================================================================
    "session_export_markdown"_test = [] {
        const kimix::string ws = ws_dir("kimix_cli_export");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "export-session", false, error)) << error;
        const kimix::vector<kimix::llm::Message> history = sample_history();

        // An empty history is refused with the reference's message.
        expect(!store.export_markdown({}, "empty.md", error));
        expect(error == "No messages to export.") << error;

        // A relative path resolves against the work directory.
        expect(store.export_markdown(history, "out/session.md", error)) << error;
        const kimix::string out_path = cli::join_path(cli::absolute_path(ws),
                                                      "out/session.md");
        expect(cli::file_exists(out_path));
        const kimix::string text = file_text(out_path);
        // Front matter: the reference's five keys (export.py:340-351).
        expect(cli::starts_with(text, "---\nsession_id: export-session\nexported_at: "))
            << text.substr(0, 120);
        expect(has_substr(text, "\nwork_dir: " + cli::absolute_path(ws) + "\n"));
        expect(has_substr(text, "\nmessage_count: 4\ntoken_count: 0\n---\n\n"
                                "# Kimi Session Export\n\n"));
        expect(has_substr(text, "## Overview\n\n- **Topic**: hello\n"));
        expect(has_substr(text, "- **Conversation**: 1 turns | 1 tool calls | 0 tokens\n"));
        expect(has_substr(text, "### User\n\nhello\n"));
        expect(has_substr(text, "### Assistant\n\n"
                                "<details><summary>Thinking</summary>\n\n"
                                "I should run ls\n\n</details>\n\nRunning it.\n\n"));
        expect(has_substr(text, "#### Tool Call: bash (`ls -la`)\n"
                                "<!-- call_id: call_1 -->\n```json\n"
                                "{\n  \"command\": \"ls -la\"\n}\n```\n"));
        expect(has_substr(text,
                          "### Tool\n\n<details><summary>Tool Result: bash (`ls -la`)"
                          "</summary>\n\n<!-- call_id: call_1 -->\ntotal 0\n\n</details>\n"));
        expect(has_substr(text, "### Assistant\n\nDone.\n"));

        // The token count comes from the recorded usage.
        store.set_usage(0.1, 12345, true);
        expect(store.export_markdown(history, "used.md", error)) << error;
        expect(has_substr(file_text(cli::join_path(cli::absolute_path(ws), "used.md")),
                          "- **Conversation**: 1 turns | 1 tool calls | 12,345 tokens\n"));

        // An empty path writes <session_dir>/export_<stamp>.md.
        expect(store.export_markdown(history, "", error)) << error;
        std::error_code ec;
        size_t exports = 0;
        kimix::filesystem::directory_iterator it(
            kimix::filesystem::path(kimix::string(store.dir())), ec);
        const kimix::filesystem::directory_iterator end;
        while (it != end) {
            const kimix::string name = kimix::to_string(it->path().filename());
            if (cli::starts_with(name, "export_") && cli::ends_with(name, ".md")) {
                ++exports;
            }
            it.increment(ec);
            if (ec) {
                break;
            }
        }
        expect(exports == 1u) << exports;
    };

    // =======================================================================
    // close()
    // =======================================================================
    "session_close_keeps_named_sessions"_test = [] {
        const kimix::string ws = ws_dir("kimix_cli_close");
        kimix::string error;
        cli::session_store named;
        expect(named.open(ws, "kept-session", false, error)) << error;
        const kimix::string kept = named.dir();
        expect(named.close(false, error)) << error;
        expect(cli::dir_exists(kept));

        cli::session_store anon;
        expect(anon.open(ws, "", false, error)) << error;
        const kimix::string temporary = anon.dir();
        expect(anon.close(false, error)) << error;
        expect(cli::dir_exists(temporary)); // kept: delete_if_anonymous == false
        expect(anon.close(true, error)) << error;
        expect(!cli::dir_exists(temporary));
        // Closing again is harmless.
          expect(anon.close(true, error)) << error;
      };

      // A copied renderer re-points its argument printer, so it keeps feeding its
      // own stream (the printer holds a back-pointer to the renderer).
      "stream_renderer_copy_routes_to_own_stream"_test = [] {
          cli::set_colorful(true);
          std::FILE *a = std::tmpfile();
          std::FILE *b = std::tmpfile();
          expect(a != nullptr && b != nullptr);
          cli::stream_renderer src(false, true);
          src.set_output(a);
          src.on_tool_call_begin(mk_tool_call("write", ""));
          cli::stream_renderer dst(src);
          dst.set_output(b);
          expect(dst.output() == b);
          src.on_tool_call_args_delta(R"({"content":"src"})");
          dst.on_tool_call_args_delta(R"({"content":"dst"})");
          src.finish_turn();
          dst.finish_turn();
          const kimix::string prefix =
              "\x1b[95m" "\xe2\x9a\xa1" " write\x1b[0m";
          const kimix::string label = "\x1b[38;5;245m\ncontent:\n\x1b[0m";
          // The copy renders into its own stream (its printer belongs to it).
          expect_bytes(read_stream(b), label + "\x1b[90mdst\x1b[0m\n", "copy stream");
          // The source keeps its header and receives only its own fragment.
          expect_bytes(read_stream(a), prefix + label + "\x1b[90msrc\x1b[0m\n",
                       "source stream");
          std::fclose(a);
          std::fclose(b);
      };

      // =======================================================================
      // S4: stream_renderer (cli/cli_stream.h) - byte-exact rendering
      // =======================================================================
      // Text deltas: plain, concatenated, no colour, no trailing newline; the
      // text is also accumulated for captured_text().
      "stream_text_run_exact"_test = [] {
          cli::set_colorful(true);
          cli::set_quiet(false);
          std::FILE *out = std::tmpfile();
          expect(out != nullptr);
          cli::stream_renderer r(true, true);
          r.set_output(out);
          expect(r.output() == out);
          r.on_step_begin(1, 5); // StepBegin prints nothing
          r.on_text_delta("Hello");
          r.on_text_delta(", ");
          r.on_text_delta("world");
          r.finish_turn();
          expect_bytes(read_stream(out), "Hello, world", "text run");
          expect(r.captured_text() == "Hello, world") << r.captured_text();
          r.reset_capture();
          expect(r.captured_text().empty());
          std::fclose(out);
      };

      // Reasoning: "[Think] " in bright cyan before the first chunk of a run
      // only, the continuation chunk raw (stream.py:1075-1081).
      "stream_reasoning_banner_exact"_test = [] {
          cli::set_colorful(true);
          cli::set_quiet(false);
          std::FILE *out = std::tmpfile();
          expect(out != nullptr);
          cli::stream_renderer r(true, true);
          r.set_output(out);
          r.on_reasoning_delta("hmm");
          r.on_reasoning_delta(" more");
          r.finish_turn();
          expect_bytes(read_stream(out),
                       "\x1b[96m[Think] hmm\x1b[0m"
                       "\x1b[96m more\x1b[0m",
                       "reasoning run");
          expect(r.captured_text().empty()); // reasoning is not assistant text
          std::fclose(out);
      };

      // Tool call with complete compact arguments: short values stay inline on
      // the header line as " key:value" in bright magenta (stream.py:804-822).
      "stream_tool_call_compact_args_exact"_test = [] {
          cli::set_colorful(true);
          std::FILE *out = std::tmpfile();
          expect(out != nullptr);
          cli::stream_renderer r(false, true);
          r.set_output(out);
          r.on_tool_call_begin(mk_tool_call("glob", R"({"query":"abc","limit":5})"));
          r.finish_turn();
          expect_bytes(read_stream(out),
                       "\x1b[95m"
                       "\xe2\x9a\xa1"
                       " glob\x1b[0m"
                       "\x1b[95m query:abc\x1b[0m"
                       "\x1b[95m limit:5\x1b[0m"
                       "\n",
                       "compact args");
          std::fclose(out);
      };

      // Streamed "content": its own "content:\n" label in GRAY (256;245), the
      // decoded value streaming in BRIGHT_BLACK (90) as the fragments arrive.
      "stream_tool_call_streamed_content_exact"_test = [] {
          cli::set_colorful(true);
          std::FILE *out = std::tmpfile();
          expect(out != nullptr);
          cli::stream_renderer r(false, true);
          r.set_output(out);
          r.on_tool_call_begin(mk_tool_call("write", ""));
          r.on_tool_call_args_delta(R"({"content":"he)");
          r.on_tool_call_args_delta(R"(llo\nworld"})");
          r.finish_turn();
          expect_bytes(read_stream(out),
                       "\x1b[95m"
                       "\xe2\x9a\xa1"
                       " write\x1b[0m"
                       "\x1b[38;5;245m\ncontent:\n\x1b[0m"
                       "\x1b[90mhe\x1b[0m"
                       "\x1b[90mllo\nworld\x1b[0m"
                       "\n",
                       "streamed content");
          std::fclose(out);
      };

      // "command" is an inline streamed key: a GRAY space instead of a label,
      // then the value in BRIGHT_BLUE (stream.py:778-781, 474).
      "stream_tool_call_inline_command_exact"_test = [] {
          cli::set_colorful(true);
          std::FILE *out = std::tmpfile();
          expect(out != nullptr);
          cli::stream_renderer r(false, true);
          r.set_output(out);
          r.on_tool_call_begin(mk_tool_call("bash", R"({"command":"ls -la"})"));
          r.finish_turn();
          expect_bytes(read_stream(out),
                       "\x1b[95m"
                       "\xe2\x9a\xa1"
                       " bash\x1b[0m"
                       "\x1b[38;5;245m \x1b[0m"
                       "\x1b[94mls -la\x1b[0m"
                       "\n",
                       "inline command");
          std::fclose(out);
      };

      // Successful tool result: the ✓ line in bright green plus the dim
      // two-space detail line (stream.py:1020-1033).
      "stream_tool_result_ok_with_message_exact"_test = [] {
          cli::set_colorful(true);
          std::FILE *out = std::tmpfile();
          expect(out != nullptr);
          cli::stream_renderer r(false, true);
          r.set_output(out);
          r.on_tool_call_begin(mk_tool_call("bash", R"({"command":"ls"})"));
          r.on_tool_result("bash", true, "3 files");
          r.finish_turn();
          expect_bytes(read_stream(out),
                       "\x1b[95m"
                       "\xe2\x9a\xa1"
                       " bash\x1b[0m"
                       "\x1b[38;5;245m \x1b[0m"
                       "\x1b[94mls\x1b[0m"
                       "\n"
                       "\x1b[92m"
                       "\xe2\x9c\x93"
                       " bash\x1b[0m"
                       "\n\x1b[90m  3 files\x1b[0m",
                       "ok tool result");
          std::fclose(out);
      };

      // Failed tool result with a trivial message: the ✗ line in bright red and
      // no detail line (the four trivial messages are suppressed).
      "stream_tool_result_trivial_message_exact"_test = [] {
          cli::set_colorful(true);
          std::FILE *out = std::tmpfile();
          expect(out != nullptr);
          cli::stream_renderer r(false, true);
          r.set_output(out);
          r.on_tool_result("glob", false, "failed");
          r.finish_turn();
          expect_bytes(read_stream(out),
                       "\x1b[91m"
                       "\xe2\x9c\x97"
                       " glob\x1b[0m",
                       "trivial failure");
          std::fclose(out);
      };

      // output_summary renders as a Brief display block (code 90) before the ✓
      // line; an empty name selects the reference's fallback branch.
      "stream_tool_result_brief_and_fallback_exact"_test = [] {
          cli::set_colorful(true);
          std::FILE *a = std::tmpfile();
          expect(a != nullptr);
          cli::stream_renderer r1(false, true);
          r1.set_output(a);
          r1.on_tool_result("read", true, "success", "Read 3 lines");
          expect_bytes(read_stream(a),
                       "\x1b[90mRead 3 lines\x1b[0m\n"
                       "\x1b[92m"
                       "\xe2\x9c\x93"
                       " read\x1b[0m",
                       "brief + ok");
          std::fclose(a);

          std::FILE *b = std::tmpfile();
          expect(b != nullptr);
          cli::stream_renderer r2(false, true);
          r2.set_output(b);
          r2.on_tool_result("", false, "boom");
          expect_bytes(read_stream(b),
                       "\x1b[91m"
                       "\xe2\x9c\x97"
                       " boom\x1b[0m",
                       "no tool call fallback");
          std::fclose(b);
      };

      // Every display-block kind in one document, joined with "\n" plus one
      // trailing "\n" (stream.py:136-184).
      "stream_display_blocks_exact"_test = [] {
          cli::set_colorful(true);
          kimix::vector<cli::display_block> blocks;
          blocks.push_back(mk_brief("Reading file"));
          cli::display_block diff;
          diff.kind = cli::display_block_kind::diff;
          diff.text = "a.py";
          diff.old_text = "old line\n";
          diff.new_text = "new line\n";
          blocks.push_back(diff);
          cli::display_block todo;
          todo.kind = cli::display_block_kind::todo;
          cli::todo_display_item done;
          done.title = "Task A";
          done.status = "done";
          cli::todo_display_item wip;
          wip.title = "Task B";
          wip.status = "in_progress";
          cli::todo_display_item pending;
          pending.title = "Task C";
          pending.status = "pending";
          todo.items.push_back(done);
          todo.items.push_back(wip);
          todo.items.push_back(pending);
          blocks.push_back(todo);
          cli::display_block shell;
          shell.kind = cli::display_block_kind::shell;
          shell.text = "ignored";
          blocks.push_back(shell);
          cli::display_block background;
          background.kind = cli::display_block_kind::background;
          background.status = "running";
          background.task_id = "t7";
          background.text = "build";
          blocks.push_back(background);
          cli::display_block unknown;
          unknown.kind = cli::display_block_kind::unknown;
          unknown.text = "weird";
          blocks.push_back(unknown);
          expect_bytes(cli::format_display_blocks(blocks),
                       "\x1b[90mReading file\x1b[0m\n"
                       "\x1b[93mDiff: a.py\x1b[0m\n"
                       "\x1b[91m- old line\x1b[0m\n"
                       "\x1b[92m+ new line\x1b[0m\n"
                       "\x1b[90m- ~~Task A~~\x1b[0m\n"
                       "\x1b[93m- Task B "
                       "\xe2\x86\x90"
                       "\x1b[0m\n"
                       "\x1b[38;5;250m- Task C\x1b[0m\n"
                       "\x1b[90m[running] t7: build\x1b[0m\n"
                       "\x1b[90mweird\x1b[0m\n",
                       "display blocks");
          // The per-kind corner cases: skipped blocks, CRLF splitlines, the
          // status normalisation and the base DisplayBlock fallback.
          expect(cli::format_display_blocks({}).empty());
          kimix::vector<cli::display_block> only_shell;
          only_shell.push_back(shell);
          expect(cli::format_display_blocks(only_shell).empty());
          expect(cli::format_display_blocks({mk_brief("")}).empty());
          expect_bytes(cli::format_display_blocks({mk_brief("x")}), "\x1b[90mx\x1b[0m\n",
                       "brief only");
          cli::display_block crlf;
          crlf.kind = cli::display_block_kind::diff;
          crlf.text = "f.txt";
          crlf.old_text = "a\r\nb\n";
          expect_bytes(cli::format_display_blocks({crlf}),
                       "\x1b[93mDiff: f.txt\x1b[0m\n"
                       "\x1b[91m- a\x1b[0m\n"
                       "\x1b[91m- b\x1b[0m\n",
                       "diff splitlines");
          cli::display_block upper;
          upper.kind = cli::display_block_kind::todo;
          cli::todo_display_item up;
          up.title = "T";
          up.status = "DONE";
          upper.items.push_back(up);
          expect_bytes(cli::format_display_blocks({upper}), "\x1b[90m- ~~T~~\x1b[0m\n",
                       "todo DONE");
          cli::display_block hyphen;
          hyphen.kind = cli::display_block_kind::todo;
          cli::todo_display_item hy;
          hy.title = "T";
          hy.status = "in-progress"; // only "_" becomes a space
          hyphen.items.push_back(hy);
          expect_bytes(cli::format_display_blocks({hyphen}),
                       "\x1b[38;5;250m- T\x1b[0m\n", "todo in-progress status");
          cli::display_block base;
          base.kind = cli::display_block_kind::base;
          base.text = "{'k': 'v'}";
          expect_bytes(cli::format_display_blocks({base}),
                       "\x1b[38;5;250m{'k': 'v'}\x1b[0m\n", "base display block");
          expect(cli::format_display_blocks({cli::display_block{}}).empty()); // empty brief
      };

      // The context-usage transition banner: GRAY (256;245), 20 '=' + label +
      // percentage_and_token + one space + padding to exactly 80 characters
      // (stream.py:117-133).
      "stream_transition_banner_exact"_test = [] {
          cli::set_colorful(true);
          cli::set_quiet(false);
          struct banner_case {
              double ratio;
              int64_t tokens;
              const char *literal;
          };
          const banner_case cases[] = {
              {0.125, 1024,
               "==================== Context usage: 12.5% (1024 tokens) "
               "========================"},
              {0.5, 4096,
               "==================== Context usage: 50.0% (4096 tokens) "
               "========================"},
              {0.9999, 123456,
               "==================== Context usage: 100.0% (123456 tokens) "
               "====================="},
          };
          for (const banner_case &c : cases) {
              const kimix::string banner = cli::context_usage_banner(c.ratio, c.tokens);
              expect(banner.size() == 80u) << escaped(banner);
              expect(banner == c.literal) << escaped(banner);
              std::FILE *out = std::tmpfile();
              expect(out != nullptr);
              cli::stream_renderer r(true, true);
              r.set_output(out);
              r.on_context_usage(c.ratio, c.tokens);
              r.on_reasoning_delta("x");
              r.on_text_delta("ok");
              r.finish_turn();
              const kimix::string expected =
                  kimix::string("\x1b[96m[Think] x\x1b[0m\n\x1b[38;5;245m") + c.literal +
                  "\n\x1b[0mok";
              expect_bytes(read_stream(out), expected, "transition banner");
              std::fclose(out);
          }
      };

      // context_usage_banner's 80-character rule for several ratios, plus the
      // "single =" clamp when the left side would overflow the target width.
      "stream_context_usage_banner_width"_test = [] {
          cli::set_colorful(true);
          bool all_80 = true;
          const double ratios[] = {0.125, 0.5, 0.0733, 0.0, 0.9999};
          const int64_t tokens[] = {1024, 4096, 777, 0, 123456};
          for (size_t i = 0; i < 5; ++i) {
              const kimix::string banner =
                  cli::context_usage_banner(ratios[i], tokens[i]);
              if (banner.size() != 80u) {
                  all_80 = false;
              }
          }
          expect(all_80);
          expect(cli::context_usage_banner(0.125, 1024) ==
                 "==================== Context usage: 12.5% (1024 tokens) "
                 "========================");
          expect(cli::context_usage_banner(0.0, 0) ==
                 "==================== Context usage: 0.0% (0 tokens) "
                 "============================");
          expect(cli::context_usage_banner(0.9999, 9223372036854775807).size() == 80u);
          // len(left) > 79 (a huge ratio overflows the label): exactly one '='.
          const kimix::string huge = cli::context_usage_banner(1e30, 0);
          expect(huge.size() == 85u) << escaped(huge);
          expect(huge ==
                 "==================== Context usage: "
                 "100000000000000005366162204393472.0% (0 tokens) =")
              << escaped(huge);
      };

      // Quiet mode (or show_thinking=false) suppresses the whole reasoning path
      // while text still prints (stream.py:1072-1083).
      "stream_quiet_suppresses_reasoning"_test = [] {
          cli::set_colorful(true);
          cli::set_quiet(true);
          std::FILE *out = std::tmpfile();
          expect(out != nullptr);
          cli::stream_renderer r(true, true);
          r.set_output(out);
          r.on_reasoning_delta("secret");
          r.on_reasoning_delta(" more");
          r.finish_turn();
          expect_bytes(read_stream(out), "", "quiet reasoning");
          std::fclose(out);

          std::FILE *plain = std::tmpfile();
          expect(plain != nullptr);
          cli::stream_renderer r2(false, true); // show_thinking == false
          r2.set_output(plain);
          r2.on_reasoning_delta("secret");
          r2.finish_turn();
          expect_bytes(read_stream(plain), "", "show_thinking off");
          std::fclose(plain);
          cli::set_quiet(false);

          std::FILE *mixed = std::tmpfile();
          expect(mixed != nullptr);
          cli::set_quiet(true);
          cli::stream_renderer r3(true, true);
          r3.set_output(mixed);
          r3.on_reasoning_delta("secret");
          r3.on_text_delta("visible");
          r3.finish_turn();
          // The reasoning text is gone; the text still prints (the transition
          // banner fires because the think message type was recorded first).
          expect_bytes(read_stream(mixed),
                       "\x1b[38;5;245m"
                       "==================== Context usage: 0.0% (0 tokens) "
                       "============================\n\x1b[0mvisible",
                       "quiet mixed");
          std::fclose(mixed);
          cli::set_quiet(false);
      };

      // With colour off every rendered line keeps its text, spacing and newlines
      // but loses the ANSI wrapper (the only global colour gate).
      "stream_colour_off_plain_text"_test = [] {
          cli::set_colorful(false);
          cli::set_quiet(false);

          std::FILE *a = std::tmpfile();
          expect(a != nullptr);
          cli::stream_renderer r1(true, true);
          r1.set_output(a);
          r1.on_text_delta("hello");
          r1.on_text_delta(" world");
          r1.finish_turn();
          expect_bytes(read_stream(a), "hello world", "plain text");
          std::fclose(a);

          std::FILE *b = std::tmpfile();
          expect(b != nullptr);
          cli::stream_renderer r2(false, true);
          r2.set_output(b);
          r2.on_tool_call_begin(mk_tool_call("bash", R"({"command":"ls -la"})"));
          r2.finish_turn();
          expect_bytes(read_stream(b),
                       "\xe2\x9a\xa1"
                       " bash ls -la\n",
                       "plain tool call");
          std::fclose(b);

          std::FILE *c = std::tmpfile();
          expect(c != nullptr);
          cli::stream_renderer r3(false, true);
          r3.set_output(c);
          r3.on_tool_result("bash", true, "success");
          r3.finish_turn();
          expect_bytes(read_stream(c),
                       "\xe2\x9c\x93"
                       " bash",
                       "plain tool result");
          std::fclose(c);

          std::FILE *d = std::tmpfile();
          expect(d != nullptr);
          cli::stream_renderer r4(true, true);
          r4.set_output(d);
          r4.on_reasoning_delta("hmm");
          r4.finish_turn();
          expect_bytes(read_stream(d), "[Think] hmm", "plain reasoning");
          std::fclose(d);
          cli::set_colorful(true);
      };

      // percentage_str / percentage_and_token (stream.py:1183-1189).
      "stream_percentage_helpers"_test = [] {
          expect(cli::percentage_str(0.125) == "12.5%") << cli::percentage_str(0.125);
          expect(cli::percentage_str(0.0) == "0.0%");
          expect(cli::percentage_str(0.9999) == "100.0%");
          expect(cli::percentage_and_token(0.5, 4096) == "50.0% (4096 tokens)");
          expect(cli::percentage_and_token(0.0, 0) == "0.0% (0 tokens)");
          expect(cli::percentage_and_token(0.125, 9223372036854775807) ==
                 "12.5% (9223372036854775807 tokens)");
      };

      // finish_turn() terminates a half-written argument line, and a new tool
      // call supersedes the previous printer (stream.py:548, 912).
      "stream_finish_turn_terminates_argument_line"_test = [] {
          cli::set_colorful(true);
          std::FILE *out = std::tmpfile();
          expect(out != nullptr);
          cli::stream_renderer r(false, true);
          r.set_output(out);
          r.on_tool_call_begin(mk_tool_call("write", R"({"content":"abc)"));
          r.finish_turn();
          r.on_tool_call_begin(mk_tool_call("glob", R"({"query":"x"})"));
          r.finish_turn();
          expect_bytes(read_stream(out),
                       "\x1b[95m"
                       "\xe2\x9a\xa1"
                       " write\x1b[0m"
                       "\x1b[38;5;245m\ncontent:\n\x1b[0m"
                       "\x1b[90mabc\x1b[0m"
                       "\n"
                       "\x1b[95m"
                       "\xe2\x9a\xa1"
                       " glob\x1b[0m"
                       "\x1b[95m query:x\x1b[0m"
                       "\n",
                       "finish + supersede");
          std::fclose(out);
      };

      // Argument rendering: compact values truncate at 60 characters + "...",
      // streamed values print in full (stream.py:816-817).
      "stream_argument_truncation"_test = [] {
          cli::set_colorful(true);
          std::FILE *out = std::tmpfile();
          expect(out != nullptr);
          cli::stream_renderer r(false, true);
          r.set_output(out);
          r.on_tool_call_begin(mk_tool_call(
              "grep",
              "{\"query\":\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
              "xxxxxxxxxx\"}"));
          r.finish_turn();
          expect_bytes(
              read_stream(out),
              "\x1b[95m"
              "\xe2\x9a\xa1"
              " grep\x1b[0m"
              "\x1b[95m query:xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
              "...\x1b[0m"
              "\n",
              "compact truncation");
          std::fclose(out);

          std::FILE *full = std::tmpfile();
          expect(full != nullptr);
          cli::stream_renderer r2(false, true);
          r2.set_output(full);
          r2.on_tool_call_begin(mk_tool_call(
              "write",
              "{\"content\":\"yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy"
              "yyyyyy\"}"));
          r2.finish_turn();
          expect_bytes(
              read_stream(full),
              "\x1b[95m"
              "\xe2\x9a\xa1"
              " write\x1b[0m"
              "\x1b[38;5;245m\ncontent:\n\x1b[0m"
              "\x1b[90myyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy"
              "\x1b[0m"
              "\n",
              "streamed value not truncated");
          std::fclose(full);
      };

      // JSON escape decoding: \uXXXX, surrogate pairs, lone surrogates as U+FFFD,
      // and unknown escapes keeping their backslash (stream.py:687-761).  The
      // first two cases mirror the captured reference output exactly: orjson
      // refuses a document with surrogate escapes, so the completion gate stays
      // closed and no terminating newline is emitted.
      "stream_argument_escape_decoding"_test = [] {
          cli::set_colorful(true);
          std::FILE *out = std::tmpfile();
          expect(out != nullptr);
          cli::stream_renderer r(false, true);
          r.set_output(out);
          r.on_tool_call_begin(
              mk_tool_call("write", R"({"content":"\u0041\u00e9\ud83d\ude00\ud83d"})"));
          expect_bytes(read_stream(out),
                       "\x1b[95m"
                       "\xe2\x9a\xa1"
                       " write\x1b[0m"
                       "\x1b[38;5;245m\ncontent:\n\x1b[0m"
                       "\x1b[90mA"
                       "\xc3\xa9"
                       "\xf0\x9f\x98\x80"
                       "\xef\xbf\xbd"
                       "\x1b[0m",
                       "unicode escapes");
          std::fclose(out);

          // A document without surrogate escapes validates, so finish() runs and
          // terminates the line.
          std::FILE *valid = std::tmpfile();
          expect(valid != nullptr);
          cli::stream_renderer r2(false, true);
          r2.set_output(valid);
          r2.on_tool_call_begin(mk_tool_call("write", R"({"content":"a\u00e9b"})"));
          expect_bytes(read_stream(valid),
                       "\x1b[95m"
                       "\xe2\x9a\xa1"
                       " write\x1b[0m"
                       "\x1b[38;5;245m\ncontent:\n\x1b[0m"
                       "\x1b[90ma"
                       "\xc3\xa9"
                       "b\x1b[0m"
                       "\n",
                       "valid unicode escape");
          std::fclose(valid);

          std::FILE *raw = std::tmpfile();
          expect(raw != nullptr);
          cli::stream_renderer r3(false, true);
          r3.set_output(raw);
          // Not valid JSON (a lone backslash), so the completion gate never
          // fires (finish_turn() would terminate the line).
          r3.on_tool_call_begin(mk_tool_call("write", R"({"content":"C:\dev\alpha"})"));
          expect_bytes(read_stream(raw),
                       "\x1b[95m"
                       "\xe2\x9a\xa1"
                       " write\x1b[0m"
                       "\x1b[38;5;245m\ncontent:\n\x1b[0m"
                       "\x1b[90mC:\\dev\\alpha\x1b[0m",
                       "unknown escape verbatim");
          std::fclose(raw);
      };

      // Compaction + the native on_error/on_display_blocks entry points.
      "stream_compaction_error_and_display_entry"_test = [] {
          cli::set_colorful(true);
          std::FILE *out = std::tmpfile();
          expect(out != nullptr);
          cli::stream_renderer r(false, true);
          r.set_output(out);
          r.on_compaction_begin();
          r.on_compaction_end(true); // _handle_noop: nothing
          r.on_step_begin(1, 3);     // _handle_noop: nothing
          expect_bytes(read_stream(out), "\x1b[95mCompacting...\x1b[0m", "compaction begin");
          r.on_error("boom");
          expect_bytes(read_stream(out),
                       "\x1b[95mCompacting...\x1b[0m"
                       "\n\x1b[1;91mboom\x1b[0m",
                       "compaction + error");
          r.on_display_blocks({mk_brief("note")});
          expect_bytes(read_stream(out),
                       "\x1b[95mCompacting...\x1b[0m"
                       "\n\x1b[1;91mboom\x1b[0m"
                       "\n\x1b[90mnote\x1b[0m\n",
                       "display block entry point");
            std::fclose(out);
        };

        // ---------------------------------------------------------------------
        // Printing layer escapes (cli_print.cpp).
        //
        // Regression guard for the S1 bug found while porting the stream layer:
        // colorful_text() used to hand an already-built escape sequence to
        // clip_wrap(), which wrapped it a second time - producing
        // "\x1b[\x1b[92mm..." for every print_info/success/error/warning/debug
        // and gray_text() call.  Each helper must emit exactly one escape.
        // ---------------------------------------------------------------------
        "print_colour_escapes_exact"_test = [] {
            cli::set_colorful(true);
            expect(cli::ansi_prefix(92, -1) == kimix::string("\x1b[92m"))
                << "ansi_prefix is a single raw escape";
            expect(cli::colorful_text("x", 92) == kimix::string("\x1b[92mx\x1b[0m"))
                << "colorful_text wraps exactly once";
            expect(cli::colorful_text("x", 92, -1, "1") ==
                   kimix::string("\x1b[1;92mx\x1b[0m"))
                << "styles come first";
            expect(cli::ansi_prefix(92, 100, "1") == kimix::string("\x1b[1;92;100m"))
                << "style;fg;bg order";
            expect(cli::gray_text("y") == kimix::string("\x1b[38;5;245my\x1b[0m"))
                << "gray_text uses the 256-colour prefix";
            expect(cli::gray_light_text("y") == kimix::string("\x1b[38;5;250my\x1b[0m"));
            expect(cli::colorful_text_256("z", 245, -1) ==
                   kimix::string("\x1b[38;5;245mz\x1b[0m"));
            expect(cli::colorful_text_true("z", 1, 2, 3) ==
                   kimix::string("\x1b[38;2;1;2;3mz\x1b[0m"));
            expect(cli::colorful_text("x") == kimix::string("x"))
                << "no codes -> no escape";
            // No doubled escape introducer anywhere.
            const kimix::string line = cli::colorful_text("q", 95);
            expect(line.find("\x1b[\x1b[") == kimix::string::npos)
                << "escape is never wrapped twice";
            cli::set_colorful(false);
            expect(cli::colorful_text("x", 92) == kimix::string("x"))
                << "colour off returns the text unchanged";
            expect(cli::gray_text("y") == kimix::string("y"));
            cli::set_colorful(true); // leave the state the other tests expect
        };
    
        // =====================================================================
        // S5: the app layer, the REPL and the slash commands, driven in-process
        // with a scripted IChatBackend (no network) and a fake stdin/stdout.
        // Reference: src/cli/PLAN.md §3.7/§3.8, specs 01 §4-§5 and 04 §1.3.
        // =====================================================================

        "app_prompt_line_exact"_test = [] {
            expect(cli::app_prompt_line() ==
                   kimix::string("\n>>>>>>>>> Enter your prompt or command:\n"))
                << "the reference _client_cli prompt string";
        };

        "app_init_wires_config_and_tools"_test = [] {
            app_fixture fx;
            expect(fx.init("cli_app_init")) << "app_init: " << fx.error;
            expect(fx.app.initialized);
            expect(fx.app.session != nullptr);
            expect(fx.app.soul != nullptr);
            expect(fx.app.injected == &fx.backend) << "the injected backend is used";
            expect(fx.app.backend == nullptr) << "no LLM/LLMBackend is created";
            expect(fx.app.provider.max_context_size == 1000);
            expect(fx.app.provider.max_tokens == 100);
            // The agent manifest's resolved tool list reaches the soul.
            expect(fx.app.soul->tool_definitions().size() ==
                   fx.app.agent.enabled_tools.size())
                << "one tool definition per enabled registry name";
            expect(cli::dir_exists(fx.app.store.dir()));
            expect(fx.app.store.anonymous()) << "the first session is anonymous";
            expect(cli::dir_exists(cli::session_store::cache_root(fx.work)));
            expect(fx.error.empty());
            fx.shutdown();
        };

        "repl_prompt_blank_input_and_turn"_test = [] {
            app_fixture fx;
            expect(fx.init("cli_repl_basic")) << "app_init: " << fx.error;
            const kimix::string answer =
                "hello from the scripted model, with enough words to count tokens";
            fx.backend.steps.push_back({answer, "", {}});
            const kimix::string in_path =
                script_file(fx.work, "input.txt", {"", "hello   there"});
            std::FILE *in = std::fopen(in_path.c_str(), "rb");
            expect(in != nullptr);
            output_capture capture;
            expect(capture.begin(cli::join_path(fx.work, "stdout.txt")));
            cli::set_colorful(false);
            const int code = cli::repl_run(fx.app, in, stdout, {});
            const kimix::string out = capture.end();
            cli::set_colorful(true);
            if (in != nullptr) {
                std::fclose(in);
            }
            expect(eq(code, 0));
            // The blank line re-prompts without touching the backend; the
            // trailing empty line is EOF.
            expect(eq(fx.backend.calls, 1)) << "blank input never calls the model";
            expect(count_occurrences(out, ">>>>>>>>> Enter your prompt or command:") == 3)
                << "one prompt per read (blank, prompt, EOF)";
            expect(has_substr(out, "\nbye.")) << "EOF prints the reference goodbye";
            expect(has_substr(fx.rendered(), answer))
                << "the streamed answer reaches the renderer";
            expect(has_substr(out, "Finished, context usage:"));
            expect(eq(fx.app.session->history().size(), size_t(2)));
            fx.shutdown();
        };

        "repl_scripted_lines_feed_the_queue"_test = [] {
            app_fixture fx;
            expect(fx.init("cli_repl_scripted")) << "app_init: " << fx.error;
            fx.backend.steps.push_back({"scripted answer", "", {}});
            kimix::vector<kimix::string> scripted;
            scripted.push_back("/context");
            scripted.push_back("hi from the script");
            scripted.push_back("/exit");
            std::FILE *in = std::tmpfile();
            expect(in != nullptr);
            output_capture capture;
            expect(capture.begin(cli::join_path(fx.work, "stdout.txt")));
            cli::set_colorful(false);
            const int code = cli::repl_run(fx.app, in, stdout, scripted);
            const kimix::string out = capture.end();
            cli::set_colorful(true);
            if (in != nullptr) {
                std::fclose(in);
            }
            expect(eq(code, 0));
            expect(eq(fx.backend.calls, 1));
            expect(has_substr(out, "Context usage: 0.0% (0 tokens)"));
            expect(has_substr(out, "bye!"));
            expect(has_substr(fx.rendered(), "scripted answer"));
            // _input pops the queue first: no prompt is ever printed while the
            // queue is non-empty (the --script path).
            expect(count_occurrences(out, ">>>>>>>>> Enter your prompt or command:") == 0)
                << "queued input prints no prompt";
            fx.shutdown();
        };

        "repl_slash_split_rule_and_unknown"_test = [] {
            app_fixture fx;
            expect(fx.init("cli_repl_split")) << "app_init: " << fx.error;
            const kimix::string target = cli::join_path(fx.work, "target.txt");
            kimix::string write_error;
            expect(cli::write_file(target, "content\n", write_error));
            const kimix::string in_path =
                script_file(fx.work, "split.txt",
                            {"/help ", "/ help", "/context", "/context:payload",
                             "/file: " + target, "/exit"});
            std::FILE *in = std::fopen(in_path.c_str(), "rb");
            expect(in != nullptr);
            output_capture capture;
            expect(capture.begin(cli::join_path(fx.work, "stdout.txt")));
            cli::set_colorful(false);
            const int code = cli::repl_run(fx.app, in, stdout, {});
            const kimix::string out = capture.end();
            cli::set_colorful(true);
            if (in != nullptr) {
                std::fclose(in);
            }
            expect(eq(code, 0));
            expect(eq(fx.backend.calls, 0)) << "no command calls the model";
            // "/help " -> the key is "help " (the unstripped remainder), so it
            // is NOT the help command; same for "/ help" -> " help".
            expect(count_occurrences(out, "Unrecognized command.") == 2)
                << "the command key is used verbatim (no strip)";
            expect(!has_substr(out, "Command line options:")) << "/help must not run";
            // "/context" and "/context:payload" both resolve to the context
            // command (the split happens at the FIRST colon).
            expect(count_occurrences(out, "Context usage: 0.0% (0 tokens)") == 2);
            // "/file: <path>" keeps the leading space in the payload ->
            // "file not found:  <path>" (two spaces).
            expect(has_substr(out, "file not found:  " + target))
                << "the payload is sliced from the unstripped string";
            fx.shutdown();
        };

        "command_map_and_unknown_fallback"_test = [] {
            const kimix::vector<cli::command_entry> &map = cli::command_map();
            expect(eq(map.size(), size_t(22)))
                << "the reference's 21 commands + the `unknown` fallback";
            const char *names[] = {"help", "clear", "exit", "context", "cmd", "fix",
                                   "txt", "file", "plan", "compact", "export",
                                   "resume", "store", "load", "sessions",
                                   "reflection", "supervisor", "swarm", "init",
                                   "todo", "code", "unknown"};
            for (const char *name : names) {
                const cli::command_entry *entry = cli::find_command(name);
                expect(entry != nullptr) << "missing command: " << name;
                if (entry != nullptr) {
                    expect(!entry->name.empty());
                    expect(!entry->help.empty()) << "one-line help for " << name;
                }
            }
            expect(cli::find_command("Help") == nullptr)
                << "the lookup is case-sensitive";
            expect(cli::find_command("") == nullptr);
            expect(cli::find_command("nope") == nullptr);

            app_fixture fx;
            expect(fx.init("cli_cmd_unknown")) << "app_init: " << fx.error;
            kimix::vector<kimix::string> args;
            args.push_back("nope");
            kimix::vector<kimix::string> text_arr;
            output_capture capture;
            expect(capture.begin(cli::join_path(fx.work, "stdout.txt")));
            cli::set_colorful(false);
            const cli::command_result result =
                cli::find_command("unknown")->handler(args, fx.app, text_arr);
            const kimix::string out = capture.end();
            cli::set_colorful(true);
            expect(!result.should_break);
            expect(!result.has_input);
            expect(text_arr.empty());
            expect(has_substr(out, "Unrecognized command."));
            fx.shutdown();
        };

        "command_split_text_blocks_exact"_test = [] {
            kimix::vector<kimix::string> lines;
            lines.push_back("a");
            lines.push_back("");
            lines.push_back("/help");
            lines.push_back("/bogus");
            lines.push_back("c");
            const kimix::vector<kimix::string> blocks = cli::split_text_blocks(lines);
            expect(eq(blocks.size(), size_t(3)));
            if (blocks.size() == 3) {
                expect(blocks[0] == kimix::string("a\n"))
                    << "a blank line joins the block as ''";
                expect(blocks[1] == kimix::string("/help"))
                    << "a known command becomes its own queue entry";
                expect(blocks[2] == kimix::string("/bogus\nc"))
                    << "an unknown slash line stays in the block";
            }
            kimix::vector<kimix::string> only_command;
            only_command.push_back("/exit");
            const kimix::vector<kimix::string> one = cli::split_text_blocks(only_command);
            expect(eq(one.size(), size_t(1)));
            if (!one.empty()) {
                expect(one[0] == kimix::string("/exit"));
            }
        };

        "command_txt_queues_blocks"_test = [] {
            app_fixture fx;
            expect(fx.init("cli_cmd_txt")) << "app_init: " << fx.error;
            kimix::vector<kimix::string> queue;
            queue.push_back("line one");
            queue.push_back("line two");
            queue.push_back("/end");
            fx.app.pending = &queue;
            kimix::vector<kimix::string> args;
            args.push_back("txt");
            kimix::vector<kimix::string> text_arr;
            output_capture capture;
            expect(capture.begin(cli::join_path(fx.work, "stdout.txt")));
            cli::set_colorful(false);
            const cli::command_result result =
                cli::find_command("txt")->handler(args, fx.app, text_arr);
            const kimix::string out = capture.end();
            cli::set_colorful(true);
            fx.app.pending = nullptr;
            expect(!result.should_break);
            expect(has_substr(out, ">>>> Start input multiple-lines, end with /end, "
                                   "cancel with /cancel"));
            expect(eq(queue.size(), size_t(0))) << "/end ends the multi-line block";
            expect(eq(text_arr.size(), size_t(1)))
                << "one block is queued back for the REPL";
            if (!text_arr.empty()) {
                expect(text_arr[0] == kimix::string("line one\nline two"));
            }
            fx.shutdown();
        };

        "app_run_prompt_persists_and_titles"_test = [] {
            app_fixture fx;
            expect(fx.init("cli_app_turn")) << "app_init: " << fx.error;
            fx.backend.steps.push_back(
                {"a fairly long scripted answer with enough words to count tokens", "", {}});
            output_capture capture;
            expect(capture.begin(cli::join_path(fx.work, "stdout.txt")));
            cli::set_colorful(false);
            const bool ok = cli::app_run_prompt(fx.app, "hello agent");
            const kimix::string out = capture.end();
            cli::set_colorful(true);
            expect(ok);
            expect(eq(fx.backend.calls, 1)) << "one turn = one backend call";
            expect(has_substr(fx.rendered(), "a fairly long scripted answer"));
            expect(has_substr(out, "Start...")) << "the reference's cyan label";
            expect(has_substr(out, "Finished, context usage:")) << "the usage banner";
            expect(has_substr(out, "time:")) << "the elapsed time suffix";
            // custom_title from the first input (no LLM title pass).
            cli::session_state state;
            kimix::string error;
            expect(fx.app.store.load_state(state, error)) << error;
            expect(state.custom_title == kimix::string("hello agent"));
            // state.json + history + wire records are written after the turn.
            expect(cli::file_exists(fx.session_file("state.json")));
            expect(cli::file_exists(fx.session_file("context.jsonl")));
            expect(cli::file_exists(fx.session_file("wire.jsonl")));
            expect(eq(fx.app.session->history().size(), size_t(2)));
            kimix::vector<kimix::llm::Message> history;
            kimix::string load_error;
            expect(fx.app.store.load_history(history, load_error)) << load_error;
            expect(eq(history.size(), size_t(2)));
            if (history.size() == 2) {
                expect(history[0].role == kimix::string("user"));
                expect(history[1].role == kimix::string("assistant"));
                expect(has_substr(history[0].content, "hello agent"));
            }
            fx.shutdown();
        };

        "tool_call_dispatch_renders_result"_test = [] {
            app_fixture fx;
            expect(fx.init("cli_app_tool")) << "app_init: " << fx.error;
            const kimix::string target = cli::join_path(fx.work, "readme.txt");
            kimix::string write_error;
            expect(cli::write_file(target, "alpha\nbeta needle\ngamma\n", write_error));
            // The fake model asks for the real `Read` tool: the soul dispatches
            // it through the real ToolRegistry, so the file content must come
            // back into the renderer.
            kimix::llm::ToolCall call;
            call.id = "call_1";
            call.type = "function";
            call.name = "Read";
            // JSON needs forward slashes (a raw Windows path is an invalid
            // escape sequence).
            call.arguments =
                "{\"file_path\":\"" + cli::replace_all(target, "\\", "/") + "\"}";
            fx.backend.steps.push_back({"", "", {call}});
            fx.backend.steps.push_back({"DONE", "", {}});
            output_capture capture;
            expect(capture.begin(cli::join_path(fx.work, "stdout.txt")));
            cli::set_colorful(false);
            const bool ok = cli::app_run_prompt(fx.app, "read the file");
            const kimix::string out = capture.end();
            cli::set_colorful(true);
            expect(ok);
            expect(eq(fx.backend.calls, 2)) << "tool call + final answer";
            const kimix::string rendered = fx.rendered();
            expect(has_substr(rendered, "Read")) << "the tool-call header";
            expect(has_substr(rendered, "beta needle"))
                << "the Read result (file content) reaches the renderer";
            expect(has_substr(rendered, "DONE"));
            expect(has_substr(rendered, "\xe2\x9c\x93")) << "the tool-result marker";
            expect(eq(fx.app.session->history().size(), size_t(4)))
                << "user + assistant(tool call) + tool + assistant";
            bool saw_tool_message = false;
            for (const kimix::llm::Message &message : fx.app.session->history()) {
                if (message.role == kimix::string("tool") &&
                    has_substr(message.content, "beta needle")) {
                    saw_tool_message = true;
                }
            }
            expect(saw_tool_message) << "the tool result is in the history";
            expect(has_substr(out, "Finished, context usage:"));
            fx.shutdown();
        };

        "command_context_and_compact_refusal"_test = [] {
            app_fixture fx;
            expect(fx.init("cli_cmd_context")) << "app_init: " << fx.error;
            kimix::vector<kimix::string> text_arr;
            const cli::command_entry *context = cli::find_command("context");
            const cli::command_entry *compact = cli::find_command("compact");
            expect(context != nullptr);
            expect(compact != nullptr);
            kimix::vector<kimix::string> args;
            args.push_back("context");
            output_capture capture;
            expect(capture.begin(cli::join_path(fx.work, "ctx.txt")));
            cli::set_colorful(false);
            context->handler(args, fx.app, text_arr);
            kimix::string out = capture.end();
            expect(has_substr(out, "Context usage: 0.0% (0 tokens)"));
            // /compact is a no-op without live context usage (the reference's
            // 1e-8 epsilon guard) - nothing is printed and nothing is compacted.
            args.clear();
            args.push_back("compact");
            expect(capture.begin(cli::join_path(fx.work, "compact0.txt")));
            compact->handler(args, fx.app, text_arr);
            out = capture.end();
            cli::set_colorful(true);
            expect(!has_substr(out, "Start compacting"))
                << "no compaction without context usage";
            expect(eq(fx.app.soul->compaction_count(), 0));
            // After a turn the usage is non-zero, so /compact announces the
            // attempt (the compaction itself needs a longer history).
            fx.backend.steps.push_back(
                {"an answer long enough to move the estimated token count", "", {}});
            expect(capture.begin(cli::join_path(fx.work, "turn.txt")));
            cli::set_colorful(false);
            expect(cli::app_run_prompt(fx.app, "hello"));
            out = capture.end();
            expect(has_substr(out, "Finished, context usage:"));
            args.clear();
            args.push_back("compact");
            expect(capture.begin(cli::join_path(fx.work, "compact1.txt")));
            compact->handler(args, fx.app, text_arr);
            out = capture.end();
            cli::set_colorful(true);
            expect(has_substr(out, "Start compacting..."));
            fx.shutdown();
        };

        "command_export_store_load_sessions_clear_exit"_test = [] {
            app_fixture fx;
            expect(fx.init("cli_cmd_session")) << "app_init: " << fx.error;
            fx.backend.steps.push_back({"answer one with a few words", "", {}});
            fx.backend.steps.push_back({"answer two with a few words", "", {}});
            kimix::vector<kimix::string> text_arr;
            output_capture capture;
            expect(capture.begin(cli::join_path(fx.work, "turn1.txt")));
            cli::set_colorful(false);
            expect(cli::app_run_prompt(fx.app, "first question"));
            capture.end();
            const kimix::string first_id = fx.app.store.id();
            expect(!first_id.empty());

            // /export writes the markdown export.
            const kimix::string export_path = cli::join_path(fx.work, "export_test.md");
            kimix::vector<kimix::string> args;
            args.push_back("export");
            args.push_back(export_path);
            expect(capture.begin(cli::join_path(fx.work, "export.txt")));
            cli::find_command("export")->handler(args, fx.app, text_arr);
            kimix::string out = capture.end();
            expect(has_substr(out, "Exported 2 messages to"));
            expect(cli::file_exists(export_path));
            expect(has_substr(file_text(export_path), "# Kimi Session Export"));

            // /store copies the session directory and keeps the current one.
            args.clear();
            args.push_back("store");
            args.push_back("saved_copy");
            expect(capture.begin(cli::join_path(fx.work, "store.txt")));
            cli::find_command("store")->handler(args, fx.app, text_arr);
            out = capture.end();
            expect(has_substr(out, "Session stored as saved_copy"));
            expect(cli::dir_exists(cli::session_store::session_dir(fx.work, "saved_copy")));
            expect(fx.app.store.id() == first_id) << "/store keeps the current session";

            // /sessions prints the reference table (with the current marker).
            args.clear();
            args.push_back("sessions");
            expect(capture.begin(cli::join_path(fx.work, "sessions.txt")));
            cli::find_command("sessions")->handler(args, fx.app, text_arr);
            out = capture.end();
            expect(has_substr(out, "session id"));
            expect(has_substr(out, "updated at"));
            expect(has_substr(out, "context usage"));
            expect(has_substr(out, "title"));
            expect(has_substr(out, "*  " + first_id)) << "the current session marker";
            expect(has_substr(out, "saved_copy"));

            // /load:<id> copies a named session into a new anonymous one.
            args.clear();
            args.push_back("load");
            args.push_back("saved_copy");
            kimix::vector<kimix::string> answers;
            answers.push_back("y");
            fx.app.pending = &answers;
            expect(capture.begin(cli::join_path(fx.work, "load.txt")));
            cli::find_command("load")->handler(args, fx.app, text_arr);
            out = capture.end();
            fx.app.pending = nullptr;
            expect(has_substr(out, "Loaded session saved_copy into anonymous session"));
            expect(fx.app.store.id() != first_id);
            expect(fx.app.store.anonymous());
            expect(eq(fx.app.session->history().size(), size_t(2)));
            const kimix::string loaded_id = fx.app.store.id();

            // /clear drops the context and keeps id/directory.
            fx.backend.steps.push_back({"a third answer with a few words", "", {}});
            expect(capture.begin(cli::join_path(fx.work, "turn2.txt")));
            expect(cli::app_run_prompt(fx.app, "second question"));
            capture.end();
            expect(gt(fx.app.session->history().size(), size_t(2)));
            args.clear();
            args.push_back("clear");
            expect(capture.begin(cli::join_path(fx.work, "clear.txt")));
            cli::find_command("clear")->handler(args, fx.app, text_arr);
            out = capture.end();
            expect(eq(fx.app.session->history().size(), size_t(0)))
                << "/clear drops the history";
            expect(!cli::file_exists(cli::join_path(
                       cli::session_store::session_dir(fx.work, loaded_id), "context.jsonl")))
                << "/clear deletes the context file";
            expect(fx.app.store.id() == loaded_id) << "/clear keeps the id";
            expect(has_substr(out, "Context usage: 0.0% (0 tokens)"));

            // /exit saves, closes (an anonymous directory is deleted) and breaks.
            args.clear();
            args.push_back("exit");
            expect(capture.begin(cli::join_path(fx.work, "exit.txt")));
            const cli::command_result result =
                cli::find_command("exit")->handler(args, fx.app, text_arr);
            out = capture.end();
            cli::set_colorful(true);
            expect(result.should_break) << "/exit is the only breaking command";
            expect(!result.has_input);
            expect(has_substr(out, "bye!"));
            expect(fx.app.session == nullptr);
            expect(fx.app.soul == nullptr);
            expect(!cli::dir_exists(cli::session_store::session_dir(fx.work, loaded_id)))
                << "closing an anonymous session deletes its directory";
            expect(cli::dir_exists(cli::session_store::session_dir(fx.work, "saved_copy")))
                << "a named session survives";
        };

        "cli_main_help_version_usage_and_dry_run"_test = [] {
            const kimix::string work = ws_dir("cli_main_paths");
            cli::set_colorful(false);
            {
                const char *argv[] = {"kimix_cli", "--help"};
                output_capture capture;
                expect(capture.begin(cli::join_path(work, "help.txt")));
                const int code =
                    cli::cli_main(2, const_cast<char **>(argv));
                const kimix::string out = capture.end();
                expect(eq(code, 0));
                expect(has_substr(out, "Command line options:"));
                expect(has_substr(out, "Native additions"));
                expect(has_substr(out, "/compact"));
            }
            {
                const char *argv[] = {"kimix_cli", "--version"};
                output_capture capture;
                expect(capture.begin(cli::join_path(work, "version.txt")));
                const int code = cli::cli_main(2, const_cast<char **>(argv));
                const kimix::string out = capture.end();
                expect(eq(code, 0));
                expect(has_substr(out, "kimix_cli "));
            }
            {
                const char *argv[] = {"kimix_cli", "--bogus"};
                output_capture capture;
                expect(capture.begin(cli::join_path(work, "usage.txt")));
                const int code = cli::cli_main(2, const_cast<char **>(argv));
                const kimix::string out = capture.end();
                expect(eq(code, 2)) << "an unknown option is a usage error";
                expect(has_substr(out, "unrecognized argument"));
                expect(has_substr(out, "usage:"));
            }
            {
                const char *argv[] = {"kimix_cli", "serve"};
                output_capture capture;
                expect(capture.begin(cli::join_path(work, "serve.txt")));
                const int code = cli::cli_main(2, const_cast<char **>(argv));
                const kimix::string out = capture.end();
                expect(eq(code, 3)) << "serve is recognised and refused";
                expect(has_substr(out, "not supported by the native CLI"));
            }
            const kimix::string provider = cli::join_path(work, "provider.json");
            kimix::string write_error;
            expect(cli::write_file(
                provider,
                "{\"model\":\"scripted-test-model\",\"type\":\"openai\","
                "\"url\":\"http://127.0.0.1:1/v1\",\"api_key\":\"test\","
                "\"max_context_size\":1000,\"max_tokens\":100}",
                write_error));
            {
                const char *argv[] = {"kimix_cli", "--dry-run", "--provider",
                                      provider.c_str()};
                output_capture capture;
                expect(capture.begin(cli::join_path(work, "dryrun.txt")));
                const int code = cli::cli_main(4, const_cast<char **>(argv));
                const kimix::string out = capture.end();
                expect(eq(code, 0));
                expect(has_substr(out, "LLMConfig: model=scripted-test-model"));
                expect(has_substr(out, "create_llm=ok"));
                expect(has_substr(out, "OK"));
                expect(!cli::dir_exists(cli::join_path(work, ".kimix_cache")))
                    << "--dry-run creates no session";
            }
            {
                const kimix::string missing = cli::join_path(work, "missing.json");
                const char *argv[] = {"kimix_cli", "--dry-run", "--provider",
                                      missing.c_str()};
                output_capture capture;
                expect(capture.begin(cli::join_path(work, "missing.txt")));
                const int code = cli::cli_main(4, const_cast<char **>(argv));
                const kimix::string out = capture.end();
                expect(eq(code, 1)) << "a missing provider config exits 1";
                expect(has_substr(out, "missing.json"));
            }
            cli::set_colorful(true);
        };

        // =====================================================================
        // S6 · cli_args (cli/cli_args.h) - the reference argparse option set
        // (kimix/cli_impl/args.py) plus the documented native additions, in both
        // the `--opt value` and the `--opt=value` spelling, the --config vs
        // --provider distinction, -s/--skill-dir arity, the four recognised
        // subcommands, the usage-error paths and the cli_main exit-code contract.
        // =====================================================================

        "cli_args_flags_all_spellings"_test = [] {
            cli::cli_options opts;
            expect(cli_parse({"-c"}, opts)) << "-c";
            expect(opts.clean);
            expect(cli_parse({"--clean"}, opts));
            expect(opts.clean);
            expect(cli_parse({"-no_color"}, opts));
            expect(opts.no_color);
            expect(cli_parse({"--no_color"}, opts));
            expect(opts.no_color);
            expect(cli_parse({"-no_think"}, opts));
            expect(opts.no_think);
            expect(cli_parse({"--no_think"}, opts));
            expect(opts.no_think);
            expect(cli_parse({"-no_yolo"}, opts));
            expect(opts.no_yolo);
            expect(cli_parse({"--no_yolo"}, opts));
            expect(opts.no_yolo);
            expect(cli_parse({"--manually-cot"}, opts));
            expect(opts.manually_cot);
            expect(cli_parse({"-h"}, opts));
            expect(opts.help);
            expect(cli_parse({"--help"}, opts));
            expect(opts.help);
            expect(cli_parse({"--version"}, opts)) << "a native addition";
            expect(opts.version);
            expect(cli_parse({"--dry-run"}, opts));
            expect(opts.dry_run);
            expect(cli_parse({"--dry_run"}, opts)) << "the underscore spelling";
            expect(opts.dry_run);
            expect(cli_parse({"--interactive"}, opts));
            expect(opts.interactive_forced);
            // A flag-only command line leaves every other field at its default.
            expect(cli_parse({"--clean"}, opts));
            expect(!opts.no_color);
            expect(!opts.no_think);
            expect(!opts.no_yolo);
            expect(!opts.manually_cot);
            expect(!opts.help);
            expect(!opts.version);
            expect(!opts.dry_run);
            expect(!opts.interactive_forced);
            expect(!opts.has_prompt);
            expect(opts.prompt.empty());
            expect(opts.script_path.empty());
            expect(opts.work_dir.empty());
            expect(opts.agent_file.empty());
            expect(opts.config_path.empty());
            expect(!opts.config_is_provider_only);
            expect(opts.skill_dirs.empty());
            expect(opts.subcommand.empty());
            expect(opts.subcommand_args.empty());
            expect(opts.errors.empty());
            expect(opts.program_name == "kimix_cli");
            // Several flags in one command line.
            expect(cli_parse({"-c", "--no_think", "--no_yolo", "--no_color", "--manually-cot"},
                             opts));
            expect(opts.clean);
            expect(opts.no_think);
            expect(opts.no_yolo);
            expect(opts.no_color);
            expect(opts.manually_cot);
        };

        "cli_args_value_options_both_spellings"_test = [] {
            cli::cli_options opts;
            // -p / --prompt in all four spellings.
            expect(cli_parse({"-p", "hello"}, opts));
            expect(opts.has_prompt);
            expect(opts.prompt == "hello");
            expect(cli_parse({"--prompt", "hello"}, opts));
            expect(opts.has_prompt);
            expect(opts.prompt == "hello");
            expect(cli_parse({"--prompt=hello"}, opts));
            expect(opts.has_prompt);
            expect(opts.prompt == "hello");
            expect(cli_parse({"-p=hello"}, opts));
            expect(opts.has_prompt);
            expect(opts.prompt == "hello");
            expect(cli_parse({"--prompt=a b c"}, opts)) << "the value keeps its spaces";
            expect(opts.prompt == "a b c");
            // An empty value is still a value.
            expect(cli_parse({"-p", ""}, opts));
            expect(opts.has_prompt);
            expect(opts.prompt.empty());
            // A value-taking option consumes the next token even when it looks
            // like one of our flags (argparse does the same).
            expect(cli_parse({"-p", "--clean"}, opts));
            expect(opts.prompt == "--clean");
            expect(!opts.clean);
            // --script / --work-dir / --agent-file.
            expect(cli_parse({"--script", "s.txt"}, opts));
            expect(opts.script_path == "s.txt");
            expect(cli_parse({"--script=s.txt"}, opts));
            expect(opts.script_path == "s.txt");
            expect(cli_parse({"--work-dir", "d"}, opts));
            expect(opts.work_dir == "d");
            expect(cli_parse({"--work_dir=d"}, opts));
            expect(opts.work_dir == "d");
            expect(cli_parse({"--agent-file", "a.json"}, opts));
            expect(opts.agent_file == "a.json");
            expect(cli_parse({"--agent_file=a.json"}, opts));
            expect(opts.agent_file == "a.json");
            // --provider is provider-only; --config may carry the agent section.
            expect(cli_parse({"--provider", "p.json"}, opts));
            expect(opts.config_path == "p.json");
            expect(opts.config_is_provider_only);
            expect(cli_parse({"--provider=p.json"}, opts));
            expect(opts.config_path == "p.json");
            expect(opts.config_is_provider_only);
            expect(cli_parse({"--config", "c.json"}, opts));
            expect(opts.config_path == "c.json");
            expect(!opts.config_is_provider_only);
            expect(cli_parse({"--config=c.json"}, opts));
            expect(opts.config_path == "c.json");
            expect(!opts.config_is_provider_only);
        };

        "cli_args_config_and_provider_precedence"_test = [] {
            cli::cli_options opts;
            // The first hit wins and decides the provider-only flag.
            expect(cli_parse({"--provider", "p", "--config", "c"}, opts));
            expect(opts.config_path == "p");
            expect(opts.config_is_provider_only);
            expect(cli_parse({"--config", "c", "--provider", "p"}, opts));
            expect(opts.config_path == "c");
            expect(!opts.config_is_provider_only);
            expect(cli_parse({"--provider=p1", "--provider=p2"}, opts));
            expect(opts.config_path == "p1");
            expect(cli_parse({"--config=c1", "--config=c2"}, opts));
            expect(opts.config_path == "c1");
        };

        "cli_args_skill_dir_arity"_test = [] {
            cli::cli_options opts;
            // nargs="*": zero values is legal.
            expect(cli_parse({"-s"}, opts));
            expect(opts.skill_dirs.empty());
            expect(cli_parse({"--skill-dir"}, opts));
            expect(opts.skill_dirs.empty());
            expect(cli_parse({"--skill_dir"}, opts));
            expect(opts.skill_dirs.empty());
            // One value.
            expect(cli_parse({"-s", "a"}, opts));
            expect(eq(opts.skill_dirs.size(), size_t(1)));
            expect(opts.skill_dirs[0] == "a");
            // N values.
            expect(cli_parse({"-s", "a", "b", "c"}, opts));
            expect(eq(opts.skill_dirs.size(), size_t(3)));
            expect(opts.skill_dirs[0] == "a");
            expect(opts.skill_dirs[1] == "b");
            expect(opts.skill_dirs[2] == "c");
            // A following flag ends the list.
            expect(cli_parse({"-s", "a", "--clean"}, opts));
            expect(eq(opts.skill_dirs.size(), size_t(1)));
            expect(opts.clean);
            // A subcommand name ends the list, and the subcommand is still taken.
            expect(cli_parse({"-s", "a", "b", "serve", "c"}, opts));
            expect(eq(opts.skill_dirs.size(), size_t(2)));
            expect(opts.subcommand == "serve");
            expect(eq(opts.subcommand_args.size(), size_t(1)));
            expect(cli_parse({"-s", "serve"}, opts));
            expect(opts.skill_dirs.empty());
            expect(opts.subcommand == "serve");
            // Documented deviation: the native -s parser only accepts the
            // separated spelling ("-s DIR"), while argparse also accepts
            // "--skill-dir=DIR" (an explicit nargs="*" argument).
            expect(!cli_parse({"--skill-dir=a"}, opts));
            expect(!opts.errors.empty());
            if (!opts.errors.empty()) {
                expect(has_substr(opts.errors[0], "--skill-dir=a"));
            }
        };

        "cli_args_subcommands_and_remaining_tokens"_test = [] {
            cli::cli_options opts;
            expect(cli_parse({"serve"}, opts));
            expect(opts.subcommand == "serve");
            expect(opts.subcommand_args.empty());
            expect(cli_parse({"gui"}, opts));
            expect(opts.subcommand == "gui");
            expect(cli_parse({"ssecli"}, opts));
            expect(opts.subcommand == "ssecli");
            // The tokens after the subcommand land verbatim in subcommand_args
            // and are never parsed as our own options.
            expect(cli_parse({"mcp", "list", "--json"}, opts));
            expect(opts.subcommand == "mcp");
            expect(eq(opts.subcommand_args.size(), size_t(2)));
            expect(opts.subcommand_args[0] == "list");
            expect(opts.subcommand_args[1] == "--json");
            expect(opts.errors.empty());
            // Flags before the subcommand are still parsed.
            expect(cli_parse({"--no_color", "serve", "--port", "1"}, opts));
            expect(opts.no_color);
            expect(opts.subcommand == "serve");
            expect(eq(opts.subcommand_args.size(), size_t(2)));
            // An unknown bare token is a positional usage error, not a subcommand.
            expect(!cli_parse({"unknowncmd"}, opts));
            expect(opts.subcommand.empty());
            expect(eq(opts.errors.size(), size_t(1)));
            if (!opts.errors.empty()) {
                expect(has_substr(opts.errors[0], "unrecognized arguments: unknowncmd"));
            }
            // A usage error before the subcommand does not lose the subcommand.
            expect(!cli_parse({"--bogus", "serve"}, opts));
            expect(opts.subcommand == "serve");
            expect(eq(opts.errors.size(), size_t(1)));
        };

        "cli_args_error_paths"_test = [] {
            cli::cli_options opts;
            // Unknown option.
            expect(!cli_parse({"--bogus"}, opts));
            expect(eq(opts.errors.size(), size_t(1)));
            if (!opts.errors.empty()) {
                expect(has_substr(opts.errors[0], "unrecognized argument: --bogus"));
            }
            // Unknown positional: the reference has no positional prompt.
            expect(!cli_parse({"just-a-word"}, opts));
            expect(eq(opts.errors.size(), size_t(1)));
            if (!opts.errors.empty()) {
                expect(has_substr(opts.errors[0], "unrecognized arguments: just-a-word"));
                expect(has_substr(opts.errors[0], "--prompt"));
            }
            // Two unknowns accumulate.
            expect(!cli_parse({"--bogus", "--also-bogus"}, opts));
            expect(eq(opts.errors.size(), size_t(2)));
            // A malformed flag spelling is an unknown option.
            expect(!cli_parse({"--clean=x"}, opts));
            if (!opts.errors.empty()) {
                expect(has_substr(opts.errors[0], "unrecognized argument: --clean=x"));
            }
            // Missing option value: the parse stops at that point.
            expect(!cli_parse({"--provider"}, opts));
            expect(eq(opts.errors.size(), size_t(1)));
            if (!opts.errors.empty()) {
                expect(has_substr(opts.errors[0], "argument --provider: expected one argument"));
            }
            expect(!cli_parse({"-p"}, opts));
            if (!opts.errors.empty()) {
                expect(has_substr(opts.errors[0], "argument -p: expected one argument"));
            }
            expect(!cli_parse({"--prompt"}, opts));
            if (!opts.errors.empty()) {
                expect(has_substr(opts.errors[0], "argument --prompt: expected one argument"));
            }
            expect(!cli_parse({"--work-dir"}, opts));
            if (!opts.errors.empty()) {
                expect(has_substr(opts.errors[0], "argument --work-dir: expected one argument"));
            }
            expect(!cli_parse({"--agent-file"}, opts));
            expect(!cli_parse({"--script"}, opts));
            // -s does not swallow a flag, so the flag is reported as missing its value.
            expect(!cli_parse({"-s", "--prompt"}, opts));
            expect(opts.skill_dirs.empty());
            if (!opts.errors.empty()) {
                expect(has_substr(opts.errors[0], "expected one argument"));
            }
            // Everything after "--" is positional (and therefore an error).
            expect(!cli_parse({"--", "x"}, opts));
            if (!opts.errors.empty()) {
                expect(has_substr(opts.errors[0], "unrecognized arguments: x"));
            }
            expect(!cli_parse({"--", "--clean"}, opts));
            expect(!opts.clean) << "-- is not an option prefix";
            // A bare "--" is accepted and ends the options.
            expect(cli_parse({"--"}, opts));
            expect(opts.errors.empty());
        };

        "cli_args_program_name_and_usage_text"_test = [] {
            cli::cli_options opts;
            {
                const char *argv[] = {"C:/tools/sub/kimix_cli.exe"};
                expect(cli::parse_args(1, const_cast<char **>(argv), opts));
                expect(opts.program_name == "kimix_cli.exe") << "the basename is used";
            }
            {
                const char *argv[] = {"C:\\tools\\sub\\kimix_cli.exe"};
                expect(cli::parse_args(1, const_cast<char **>(argv), opts));
                expect(opts.program_name == "kimix_cli.exe");
            }
            {
                const char *argv[] = {"kimix_cli"};
                expect(cli::parse_args(1, const_cast<char **>(argv), opts));
                expect(opts.program_name == "kimix_cli");
            }
            {
                const char *argv[] = {nullptr};
                expect(cli::parse_args(1, const_cast<char **>(argv), opts));
                expect(opts.program_name == "kimix_cli") << "a null argv[0] falls back";
            }
            // argc == 0 never dereferences argv.
            expect(cli::parse_args(0, nullptr, opts));
            expect(opts.program_name == "kimix_cli");
            expect(opts.errors.empty());
            // The one-line synopsis.
            const kimix::string usage = cli::cli_usage_line(opts.program_name);
            expect(cli::starts_with(usage, "usage: kimix_cli "));
            expect(cli::contains(usage, "[-s [SKILL_DIR ...]]"));
            expect(cli::contains(usage, "--provider FILE"));
            expect(cli::contains(usage, "--agent-file FILE"));
            expect(cli::contains(usage, "--work-dir DIR"));
            expect(cli::contains(usage, "--dry-run"));
            // The reference HELP_STR (byte-exactness is gen_cli_help.py's job;
            // here: the plain/coloured split and the native additions section).
            const kimix::string plain = cli::cli_help_text(false);
            expect(!cli::contains(plain, "\x1b")) << "colour off: not a single escape";
            expect(plain.size() > 1500);
            expect(cli::contains(plain, "Command line options:"));
            expect(cli::contains(plain, "--no_color"));
            expect(cli::contains(plain, "--manually-cot"));
            expect(cli::contains(plain, "-s, --skill-dir"));
            expect(cli::contains(plain, "/compact"));
            const kimix::string colored = cli::cli_help_text(true);
            expect(cli::contains(colored, "\x1b[33m"));
            expect(cli::contains(colored, "\x1b[0m"));
            expect(colored.size() > plain.size());
            expect(colored.find("\x1b[\x1b[") == kimix::string::npos);
            const kimix::string extended = cli::cli_help_text_extended(false);
            expect(cli::starts_with(extended, plain));
            expect(cli::contains(extended, "Native additions"));
            expect(cli::contains(extended, "--agent-file FILE"));
            expect(cli::contains(extended, "--version"));
        };

        "cli_args_exit_code_contract"_test = [] {
            // The documented exit-code contract (cli/cli_args.h).
            expect(eq(static_cast<int>(cli::kExitOk), 0));
            expect(eq(static_cast<int>(cli::kExitConfig), 1));
            expect(eq(static_cast<int>(cli::kExitUsage), 2));
            expect(eq(static_cast<int>(cli::kExitUnsupported), 3));
            expect(eq(static_cast<int>(cli::kExitRuntime), 4));
            const kimix::string work = ws_dir("cli_args_exit_codes");
            cli::set_colorful(false);
            // Usage errors (2), no config needed.
            {
                const char *argv[] = {"kimix_cli", "--provider"};
                output_capture capture;
                expect(capture.begin(cli::join_path(work, "usage_missing_value.txt")));
                const int code = cli::cli_main(2, const_cast<char **>(argv));
                const kimix::string out = capture.end();
                expect(eq(code, cli::kExitUsage));
                expect(has_substr(out, "expected one argument"));
                expect(has_substr(out, "usage: kimix_cli"));
                expect(has_substr(out, "try 'kimix_cli --help'"));
            }
            {
                const char *argv[] = {"kimix_cli", "positional"};
                output_capture capture;
                expect(capture.begin(cli::join_path(work, "usage_positional.txt")));
                const int code = cli::cli_main(2, const_cast<char **>(argv));
                const kimix::string out = capture.end();
                expect(eq(code, cli::kExitUsage));
                expect(has_substr(out, "unrecognized arguments: positional"));
            }
            // The four recognised-but-unsupported subcommands (3), with their own
            // arguments present so the parser has to consume them.
            const char *const subcommands[] = {"serve", "gui", "ssecli", "mcp"};
            for (const char *const name : subcommands) {
                const char *argv[] = {"kimix_cli", name, "--x", "1"};
                output_capture capture;
                expect(capture.begin(cli::join_path(work, kimix::string(name) + "_no.txt")));
                const int code = cli::cli_main(4, const_cast<char **>(argv));
                const kimix::string out = capture.end();
                expect(eq(code, cli::kExitUnsupported)) << name;
                expect(has_substr(out, "not supported by the native CLI")) << name;
                expect(has_substr(out, name)) << name;
            }
            // Config errors (1) that need no network.
            {
                const kimix::string missing = cli::join_path(work, "absent.json");
                const char *argv[] = {"kimix_cli", "--dry-run", "--provider", missing.c_str(),
                                      "--work-dir", work.c_str()};
                output_capture capture;
                expect(capture.begin(cli::join_path(work, "config_missing.txt")));
                const int code = cli::cli_main(6, const_cast<char **>(argv));
                const kimix::string out = capture.end();
                expect(eq(code, cli::kExitConfig));
                expect(has_substr(out, "absent.json"));
            }
            {
                const kimix::string broken = cli::join_path(work, "broken.json");
                kimix::string write_error;
                expect(cli::write_file(broken, "{not json", write_error));
                const char *argv[] = {"kimix_cli", "--dry-run", "--provider", broken.c_str(),
                                      "--work-dir", work.c_str()};
                output_capture capture;
                expect(capture.begin(cli::join_path(work, "config_broken.txt")));
                const int code = cli::cli_main(6, const_cast<char **>(argv));
                const kimix::string out = capture.end();
                expect(eq(code, cli::kExitConfig));
                expect(has_substr(out, "invalid JSON"));
            }
            {
                const kimix::string provider = cli::join_path(work, "ok.json");
                const kimix::string manifest = cli::join_path(work, "absent_agent.json");
                kimix::string write_error;
                expect(cli::write_file(
                    provider,
                    "{\"model\":\"gpt-5.4\",\"type\":\"openai\","
                    "\"url\":\"http://127.0.0.1:1/v1\",\"api_key\":\"k\","
                    "\"max_context_size\":1000,\"max_tokens\":100}",
                    write_error));
                const char *argv[] = {"kimix_cli", "--dry-run", "--provider",
                                      provider.c_str(), "--agent-file", manifest.c_str(),
                                      "--work-dir", work.c_str()};
                output_capture capture;
                expect(capture.begin(cli::join_path(work, "config_agent.txt")));
                const int code = cli::cli_main(8, const_cast<char **>(argv));
                const kimix::string out = capture.end();
                expect(eq(code, cli::kExitConfig)) << "a missing manifest is a config error";
                expect(has_substr(out, "absent_agent.json"));
            }
            // No provider at all: cli_main looks for default_config.json in the
            // working directory (and next to argv[0]) first, so this is only
            // asserted when neither exists.
            {
                const kimix::string cwd_default =
                    cli::join_path(cli::current_dir(), "default_config.json");
                if (cli::file_exists(cwd_default)) {
                    printf("[skip] %s exists - the no-provider path is not reachable\n",
                           cwd_default.c_str());
                } else {
                    const char *argv[] = {"kimix_cli", "--dry-run", "--work-dir", work.c_str()};
                    output_capture capture;
                    expect(capture.begin(cli::join_path(work, "config_none.txt")));
                    const int code = cli::cli_main(4, const_cast<char **>(argv));
                    const kimix::string out = capture.end();
                    expect(eq(code, cli::kExitConfig));
                    expect(has_substr(out, "no provider config found"));
                }
            }
            // kExitRuntime (4) needs a failed LLM turn, which requires a socket
            // round trip; the mapping (app_run_prompt == false -> kExitRuntime) is
            // exercised at the app layer, and the value is asserted above.
            cli::set_colorful(true);
        };

        // =====================================================================
        // S6 · cli_common (cli/cli_common.h) - the string/path/time/env helpers.
        // Reference: utils/io.py, utils/session.py and the soul's blank-input
        // rule (Python str.strip() whitespace + the Unicode spaces).
        // =====================================================================

        "cli_common_trim_and_blank"_test = [] {
            expect(cli::trim("") == kimix::string_view(""));
            expect(cli::trim("abc") == kimix::string_view("abc"));
            expect(cli::trim("  abc  ") == kimix::string_view("abc"));
            expect(cli::trim("\t\n\r\v\f abc \t\n\r\v\f") == kimix::string_view("abc"));
            expect(cli::trim("\x1c\x1d\x1e\x1f abc \x1c\x1f") == kimix::string_view("abc"))
                << "the ASCII 0x1c..0x1f set is Python str.strip() whitespace";
            expect(cli::trim(" \x1c ") == kimix::string_view(""))
                << "everything trimmed -> an empty view";
            expect(cli::trim("a b") == kimix::string_view("a b"))
                << "interior whitespace is untouched";
            // The Unicode spaces the soul's blank-input rule also accepts.
            const kimix::string nbsp = "\xc2\xa0";             // U+00A0
            const kimix::string nel = "\xc2\x85";              // U+0085
            const kimix::string ogham = "\xe1\x9a\x80";        // U+1680
            const kimix::string en_quad = "\xe2\x80\x80";      // U+2000
            const kimix::string em_space = "\xe2\x80\x83";     // U+2003
            const kimix::string hair_space = "\xe2\x80\x8a";   // U+200A
            const kimix::string line_sep = "\xe2\x80\xa8";     // U+2028
            const kimix::string para_sep = "\xe2\x80\xa9";     // U+2029
            const kimix::string narrow_nbsp = "\xe2\x80\xaf";  // U+202F
            const kimix::string math_space = "\xe2\x81\x9f";   // U+205F
            const kimix::string ideographic = "\xe3\x80\x80";  // U+3000
            const kimix::string zero_width = "\xe2\x80\x8b";   // U+200B (not whitespace)
            const kimix::string reversed_9 = "\xe2\x80\x9f";    // U+201F (not whitespace)
            expect(cli::trim(nbsp + "x" + ideographic) == kimix::string_view("x"));
            expect(cli::trim(em_space + "x" + line_sep) == kimix::string_view("x"));
            expect(cli::trim(nel + ogham + en_quad + hair_space) == kimix::string_view(""));
            expect(cli::trim(para_sep + narrow_nbsp + math_space) == kimix::string_view(""));
            expect(cli::trim(nbsp + reversed_9 + ideographic) == kimix::string_view(reversed_9))
                << "U+201F is not Python whitespace";
            expect(cli::trim(nbsp + zero_width + ideographic) == kimix::string_view(zero_width))
                << "U+200B is not Python whitespace either";
            // trim_ascii never touches a non-ASCII byte.
            expect(cli::trim_ascii(nbsp + "x" + ideographic) ==
                   kimix::string_view(nbsp + "x" + ideographic));
            expect(cli::trim_ascii(" " + nbsp + " ") == kimix::string_view(nbsp));
            expect(cli::trim_ascii(" \x1c x \x1f ") == kimix::string_view("x"));
            expect(cli::trim_ascii(nbsp) == kimix::string_view(nbsp))
                << "an all-Unicode-space text is not blank for trim_ascii";
            // is_blank == trim().empty().
            expect(cli::is_blank(""));
            expect(cli::is_blank(" \t\n"));
            expect(cli::is_blank(nbsp));
            expect(cli::is_blank(ideographic + line_sep + em_space));
            expect(cli::is_blank("\x1c\x1d\x1e\x1f"));
            expect(!cli::is_blank("x"));
            expect(!cli::is_blank(nbsp + "x"));
            expect(!cli::is_blank(zero_width));
            // A NUL byte is not whitespace.
            const kimix::string with_nul("a\0b", 3);
            expect(cli::trim(with_nul) == kimix::string_view(with_nul));
        };

        "cli_common_search_and_case"_test = [] {
            expect(cli::starts_with("abc", "ab"));
            expect(cli::starts_with("abc", ""));
            expect(cli::starts_with("abc", "abc"));
            expect(!cli::starts_with("abc", "abcd")) << "a longer prefix cannot match";
            expect(!cli::starts_with("abc", "b"));
            expect(cli::ends_with("abc", "bc"));
            expect(cli::ends_with("abc", ""));
            expect(cli::ends_with("abc", "abc"));
            expect(!cli::ends_with("abc", "zabc"));
            expect(cli::contains("abc", "b"));
            expect(cli::contains("abc", ""));
            expect(!cli::contains("abc", "d"));
            expect(!cli::contains("", "a"));
            expect(eq(cli::find("a,b,c", ",", 0), size_t(1)));
            expect(eq(cli::find("a,b,c", ",", 2), size_t(3)));
            expect(cli::find("a,b,c", ",", 4) == kimix::string::npos);
            expect(cli::find("abc", "zzz") == kimix::string::npos);
            expect(eq(cli::find("abc", ""), size_t(0)));
            expect(eq(cli::rfind("a,b,c", ","), size_t(3)));
            expect(cli::rfind("abc", "z") == kimix::string::npos);
            expect(eq(cli::rfind("abc", ""), size_t(3)));
            // ASCII-only, locale independent, byte-wise on non-ASCII input.
            expect(cli::to_lower_ascii("AbC-_9 \xc3\x84") == kimix::string("abc-_9 \xc3\x84"));
            expect(cli::to_upper_ascii("aBc-9") == kimix::string("ABC-9"));
            expect(cli::to_lower_ascii("") == kimix::string(""));
        };

        "cli_common_split_join_replace"_test = [] {
            kimix::vector<kimix::string> parts;
            cli::split("a,b,,c,", ',', parts);
            expect(eq(parts.size(), size_t(5))) << "keep_empty defaults to true";
            expect(parts[0] == "a");
            expect(parts[1] == "b");
            expect(parts[2] == "");
            expect(parts[3] == "c");
            expect(parts[4] == "");
            cli::split("a,b,,c,", ',', parts, /*keep_empty=*/false);
            expect(eq(parts.size(), size_t(3)));
            expect(parts[0] == "a");
            expect(parts[1] == "b");
            expect(parts[2] == "c");
            cli::split("", ',', parts, false);
            expect(parts.empty());
            cli::split("", ',', parts, true);
            expect(eq(parts.size(), size_t(1)));
            expect(parts[0].empty());
            cli::split("abc", ',', parts, false);
            expect(eq(parts.size(), size_t(1)));
            expect(parts[0] == "abc");

            kimix::vector<kimix::string> lines;
            cli::split_lines("a\nb\n", lines);
            expect(eq(lines.size(), size_t(2))) << "no empty line after the final \\n";
            expect(lines[0] == "a");
            expect(lines[1] == "b");
            cli::split_lines("a\r\nb\nc", lines);
            expect(eq(lines.size(), size_t(3)));
            expect(lines[0] == "a") << "a trailing \\r is dropped";
            expect(lines[1] == "b");
            expect(lines[2] == "c");
            cli::split_lines("a\n\nb", lines);
            expect(eq(lines.size(), size_t(3)));
            expect(lines[0] == "a");
            expect(lines[1].empty()) << "an interior blank line is kept";
            expect(lines[2] == "b");
            cli::split_lines("", lines);
            expect(eq(lines.size(), size_t(1)));
            expect(lines[0].empty()) << "an empty text yields one empty line";
            cli::split_lines("a", lines);
            expect(eq(lines.size(), size_t(1)));
            expect(lines[0] == "a");

            expect(cli::join({"a", "b", "c"}, ", ") == kimix::string("a, b, c"));
            expect(cli::join({}, ",") == kimix::string(""));
            expect(cli::join({"x"}, ",") == kimix::string("x"));
            expect(cli::join({"", ""}, "-") == kimix::string("-"));
            expect(cli::replace_all("aaa", "aa", "X") == kimix::string("Xa"))
                << "non-overlapping, left to right";
            expect(cli::replace_all("a-b-c", "-", "_") == kimix::string("a_b_c"));
            expect(cli::replace_all("abc", "", "X") == kimix::string("abc"))
                << "an empty needle is a no-op";
            expect(cli::replace_all("abc", "z", "X") == kimix::string("abc"));
            expect(cli::replace_all("abc", "abc", "") == kimix::string(""));
        };

        "cli_common_paths"_test = [] {
            const kimix::string dir = ws_dir("cli_common_paths");
            const kimix::string joined = cli::join_path(dir, "child");
            expect(cli::file_name(joined) == "child");
            expect(cli::parent_path(joined) == dir);
            expect(cli::ends_with(joined, "child"));
            expect(joined.size() > dir.size());
            expect(cli::file_name("C:/a/b.txt") == "b.txt");
            expect(cli::parent_path("C:/a/b.txt") == "C:/a");
            expect(cli::extension("C:/a/b.txt") == ".txt");
            expect(cli::extension("C:/a/b") == "");
            expect(cli::extension(".gitignore") == "")
                << "a leading dot is not an extension";
            expect(cli::extension("archive.tar.gz") == ".gz");
            expect(cli::file_name("") == "");
            expect(cli::parent_path("") == "");
            expect(cli::with_file_name("C:/a/b.txt", "c.md") == cli::join_path("C:/a", "c.md"))
                << "the parent is kept and the file name replaced (native separator)";
            expect(cli::with_file_name("b.txt", "c.md") == kimix::string("c.md"));
            expect(cli::with_file_name("C:/a/b.txt", "C:/x/y.md") == kimix::string("C:/x/y.md"))
                << "an absolute replacement name wins over the parent";
            // absolute_path: an already absolute path is returned (lexically
            // normalised); a relative one is resolved against the cwd and ".."
            // collapses.
            expect(cli::absolute_path(dir) == dir);
            const kimix::string rel = "cli_common_paths_rel.txt";
            const kimix::string abs_rel = cli::absolute_path(rel);
            expect(cli::starts_with(abs_rel, cli::current_dir()));
            expect(cli::ends_with(abs_rel, rel));
            expect(cli::file_name(abs_rel) == rel);
            expect(cli::absolute_path(cli::join_path(dir, "sub/..")) == dir)
                << "lexically_normal collapses a .. component";
            expect(cli::absolute_path(cli::join_path(dir, "./x")) ==
                   cli::join_path(dir, "x")) << "a . component collapses";
        };

        "cli_common_file_io"_test = [] {
            const kimix::string dir = ws_dir("cli_common_io");
            const kimix::string path = cli::join_path(dir, "text.bin");
            kimix::string error = "sentinel";
            kimix::string text = "sentinel";
            expect(!cli::read_file(cli::join_path(dir, "nope.bin"), text, error));
            expect(!error.empty()) << "a missing file reports an error";
            expect(text.empty()) << "out is cleared before the read";
            expect(!cli::file_exists(cli::join_path(dir, "nope.bin")));
            expect(!cli::dir_exists(cli::join_path(dir, "nope.bin")));
            kimix::string write_error;
            const kimix::string payload = "line1\nline2 \xc3\xa4\xe2\x82\xac";
            expect(cli::write_file(path, payload, write_error)) << write_error;
            expect(cli::file_exists(path));
            expect(!cli::dir_exists(path)) << "file_exists != dir_exists";
            kimix::string round;
            kimix::string read_error;
            expect(cli::read_file(path, round, read_error)) << read_error;
            expect(round == payload) << "UTF-8 bytes survive the round trip";
            expect(cli::write_file(path, "", write_error));
            expect(cli::read_file(path, round, read_error));
            expect(round.empty()) << "a write without append truncates";
            expect(cli::write_file(path, "A", write_error));
            expect(cli::write_file(path, "B", write_error, /*append=*/true));
            expect(cli::read_file(path, round, read_error));
            expect(round == kimix::string("AB"));
            // Failure paths: a missing parent directory and a directory target.
            const kimix::string orphan = cli::join_path(cli::join_path(dir, "missing"), "x.txt");
            write_error.clear();
            expect(!cli::write_file(orphan, "x", write_error));
            expect(!write_error.empty());
            expect(!cli::file_exists(orphan));
            write_error.clear();
            expect(!cli::write_file(dir, "x", write_error)) << "a directory is not writable";
            expect(!write_error.empty());
        };

        "cli_common_make_dirs_and_remove_all"_test = [] {
            const kimix::string dir = ws_dir("cli_common_dirs");
            const kimix::string deep = cli::join_path(cli::join_path(dir, "a"), "b");
            kimix::string error;
            expect(!cli::dir_exists(deep));
            expect(cli::make_dirs(deep, error)) << error;
            expect(cli::dir_exists(cli::join_path(dir, "a")));
            expect(cli::dir_exists(deep));
            expect(cli::make_dirs(deep, error)) << "an existing directory is not an error";
            expect(cli::make_dirs("", error)) << "an empty path is a no-op";
            kimix::string write_error;
            expect(cli::write_file(cli::join_path(deep, "f.txt"), "x", write_error));
            expect(cli::remove_all(dir, error)) << error;
            expect(!cli::dir_exists(dir));
            expect(!cli::file_exists(cli::join_path(deep, "f.txt")));
            expect(cli::remove_all(dir, error)) << "a missing path is success";
        };

        "cli_common_random_hex_and_time"_test = [] {
            const kimix::string id = cli::random_hex();
            expect(eq(id.size(), size_t(32))) << "uuid4().hex shape";
            bool all_hex = true;
            size_t first_bad = 0;
            for (size_t i = 0; i < id.size(); ++i) {
                const char c = id[i];
                const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                if (!hex && all_hex) {
                    first_bad = i;
                }
                all_hex = all_hex && hex;
            }
            expect(all_hex) << "lowercase hex only, bad byte at " << first_bad << ": " << id;
            expect(cli::random_hex(0).empty());
            expect(eq(cli::random_hex(4).size(), size_t(8)));
            expect(eq(cli::random_hex(8).size(), size_t(16)));
            expect(cli::random_hex() != id) << "two draws differ";
            expect(cli::random_hex() != cli::random_hex());
            // Time formatting.
            const int64_t now = cli::now_unix_seconds();
            expect(now > 1700000000) << "sanity: after 2023-11-14";
            expect(cli::format_utc(0) == kimix::string("1970-01-01 00:00:00"));
            expect(cli::format_utc(1700000000) == kimix::string("2023-11-14 22:13:20"));
            expect(cli::format_utc(1700000000, "%Y-%m-%d") == kimix::string("2023-11-14"));
            expect(cli::format_utc(1700000000, "%H:%M") == kimix::string("22:13"));
            expect(cli::format_duration_hm(0) == kimix::string("0:00:00"));
            expect(cli::format_duration_hm(59) == kimix::string("0:00:59"));
            expect(cli::format_duration_hm(60) == kimix::string("0:01:00"));
            expect(cli::format_duration_hm(3599) == kimix::string("0:59:59"));
            expect(cli::format_duration_hm(3600) == kimix::string("1:00:00"));
            expect(cli::format_duration_hm(3661) == kimix::string("1:01:01"));
            expect(cli::format_duration_hm(86400) == kimix::string("24:00:00"));
            expect(cli::format_duration_hm(360000) == kimix::string("100:00:00"));
            expect(cli::format_duration_hm(-5) == kimix::string("0:00:00"))
                << "a negative duration clamps to 0";
            // file_mtime_unix.
            const kimix::string dir = ws_dir("cli_common_mtime");
            const kimix::string path = cli::join_path(dir, "mtime.txt");
            kimix::string error;
            expect(cli::write_file(path, "x", error));
            const int64_t mtime = cli::file_mtime_unix(path);
            expect(mtime > 1700000000);
            expect(mtime <= cli::now_unix_seconds() + 5) << "mtime is 'now'";
            expect(cli::file_mtime_unix(cli::join_path(dir, "absent.txt")) == 0)
                << "a missing file has mtime 0";
        };

        "cli_common_env"_test = [] {
            kimix::string value = "sentinel";
            expect(!cli::get_env("KIMIX_CLI_TEST_ENV_ABSENT_VARIABLE", value));
            expect(value.empty()) << "get_env clears out first";
            expect(!cli::get_env(nullptr, value));
            expect(!cli::set_env(nullptr, "x"));
            expect(cli::set_env("KIMIX_CLI_TEST_ENV_A", "one"));
            expect(cli::get_env("KIMIX_CLI_TEST_ENV_A", value));
            expect(value == "one");
            expect(cli::set_env("KIMIX_CLI_TEST_ENV_A", "two"));
            expect(cli::get_env("KIMIX_CLI_TEST_ENV_A", value));
            expect(value == "two");
            expect(cli::set_env("KIMIX_CLI_TEST_ENV_A", ""));
#if defined(_WIN32)
            // _putenv_s(name, "") *removes* the variable on Windows, so a
            // set-but-empty state is not observable there.
            expect(!cli::get_env("KIMIX_CLI_TEST_ENV_A", value));
#else
            expect(cli::get_env("KIMIX_CLI_TEST_ENV_A", value))
                << "set but empty is still set";
            expect(value.empty());
#endif
            // first_env: the first set variable of a name list wins.
            const char *const names[] = {"KIMIX_CLI_TEST_ENV_ABSENT_VARIABLE",
                                         "KIMIX_CLI_TEST_ENV_B", "KIMIX_CLI_TEST_ENV_C"};
            expect(cli::first_env(names, 3).empty());
            expect(cli::first_env(names, 0).empty());
            expect(cli::set_env("KIMIX_CLI_TEST_ENV_C", "cee"));
            expect(cli::first_env(names, 3) == kimix::string("cee"));
            expect(cli::set_env("KIMIX_CLI_TEST_ENV_B", "bee"));
            expect(cli::first_env(names, 3) == kimix::string("bee"))
                << "the earlier name wins";
        };

        // =====================================================================
        // S6 · cli_tools (cli/cli_tools.h) - the agent_*.json union, the
        // module:attr -> registry-name table and every malformed input.
        // =====================================================================

        "cli_tools_table_and_resolve"_test = [] {
            const kimix::vector<std::pair<kimix::string, kimix::string>> &table =
                cli::agent_tool_table();
            expect(eq(table.size(), size_t(25))) << "the agent_*.json union";
            for (const std::pair<kimix::string, kimix::string> &entry : table) {
                expect(cli::resolve_tool_path(entry.first) == entry.second)
                    << "resolve_tool_path(" << entry.first << ")";
            }
            // The documented paths of PLAN.md §4.
            expect(cli::resolve_tool_path("kimi_cli.tools.file:read") == kimix::string("Read"));
            expect(cli::resolve_tool_path("kimi_cli.tools.file:read_image") ==
                   kimix::string("ReadImage"));
            expect(cli::resolve_tool_path("kimi_cli.tools.file:glob") == kimix::string("Glob"));
            expect(cli::resolve_tool_path("kimi_cli.tools.file:grep") == kimix::string("Grep"));
            expect(cli::resolve_tool_path("kimi_cli.tools.file:edit") == kimix::string("Edit"));
            expect(cli::resolve_tool_path("kimi_cli.tools.file:write") == kimix::string("Write"));
            expect(cli::resolve_tool_path("kimix.tools.web.fetch_url:fetch_url") ==
                   kimix::string("FetchUrl"));
            expect(cli::resolve_tool_path("kimi_cli.tools.web:web_search") ==
                   kimix::string("WebSearch"));
            expect(cli::resolve_tool_path("kimix.tools.note:WritePlan") ==
                   kimix::string("WritePlan"));
            expect(cli::resolve_tool_path("kimix.tools.note:ReadPlan") ==
                   kimix::string("ReadPlan"));
            expect(cli::resolve_tool_path("kimix.tools.note:EditPlan") ==
                   kimix::string("EditPlan"));
            expect(cli::resolve_tool_path("kimix.tools.agent:subagent") ==
                   kimix::string("Subagent"));
            expect(cli::resolve_tool_path("kimix.tools.agent:send_message") ==
                   kimix::string("SendMessage"));
            expect(cli::resolve_tool_path("kimix.tools.agent:list_agents") ==
                   kimix::string("ListAgents"));
            expect(cli::resolve_tool_path("kimix.tools.agent:interrupt_agent") ==
                   kimix::string("InterruptAgent"));
            expect(cli::resolve_tool_path("kimix.tools.swarm:workflow") ==
                   kimix::string("Workflow"));
            expect(cli::resolve_tool_path("kimi_cli.tools.todo:todo_write") ==
                   kimix::string("TodoWrite"));
            expect(cli::resolve_tool_path("kimi_cli.tools.todo:todo_update") ==
                   kimix::string("TodoUpdate"));
            expect(cli::resolve_tool_path("kimi_cli.tools.memory:retrieve") ==
                   kimix::string("Retrieve"));
            expect(cli::resolve_tool_path("kimix.tools.context:compact") ==
                   kimix::string("Compact"));
            expect(cli::resolve_tool_path("kimix.tools.file.bash:bash") == kimix::string("Bash"));
            expect(cli::resolve_tool_path("kimix.tools.file.bash:pwsh") == kimix::string("Pwsh"));
            expect(cli::resolve_tool_path("kimix.tools.file.run:Run") == kimix::string("Run"));
            expect(cli::resolve_tool_path("kimix.tools.py:python") == kimix::string("Python"));
            expect(cli::resolve_tool_path("kimix.tools.background:job_output") ==
                   kimix::string("JobOutput"));
            // Malformed and unknown inputs never resolve.
            expect(cli::resolve_tool_path("").empty());
            expect(cli::resolve_tool_path("noColon").empty());
            expect(cli::resolve_tool_path(":read").empty()) << "an empty module half";
            expect(cli::resolve_tool_path("kimi_cli.tools.file:").empty())
                << "an empty attr half";
            expect(cli::resolve_tool_path(":").empty());
            expect(cli::resolve_tool_path("kimi_cli.tools.file:READ").empty())
                << "the mapping is case sensitive";
            expect(cli::resolve_tool_path("KIMI_CLI.TOOLS.FILE:read").empty());
            expect(cli::resolve_tool_path(" kimi_cli.tools.file:read").empty());
            expect(cli::resolve_tool_path("kimi_cli.tools.file:read ").empty());
            expect(cli::resolve_tool_path("kimi_cli.tools.file:read\n").empty());
            expect(cli::resolve_tool_path("a:b:read").empty())
                << "the module half is not re-split";
            expect(cli::resolve_tool_path("kimi_cli.tools.file:read:extra").empty());
            expect(cli::resolve_tool_path("unknown.module:attr").empty());
            expect(cli::resolve_tool_path("kimix.tools.context:compat").empty());
            // default_agent_tools(): 25 unique registry names, all registered.
            const kimix::vector<kimix::string> &defaults = cli::default_agent_tools();
            expect(eq(defaults.size(), size_t(25)));
            for (size_t i = 0; i < defaults.size(); ++i) {
                expect(kimix::builtin_tools::ToolRegistry::instance().find(defaults[i]) != nullptr)
                    << "registered: " << defaults[i];
                for (size_t j = i + 1; j < defaults.size(); ++j) {
                    expect(defaults[i] != defaults[j]) << "duplicate: " << defaults[i];
                }
            }
            for (size_t i = 0; i < table.size(); ++i) {
                expect(defaults[i] == table[i].second) << "table order differs at " << i;
            }
        };

        // =====================================================================
        // S6 · the generated reference goldens
        // (tests/unit/cli/cli_config_goldens.inc, scripts/gen_cli_config_goldens.py).
        // =====================================================================

        "cli_config_golden_model_rows"_test = [] {
            expect(eq(kCliGoldenModelRowCount, size_t(17))) << "_MODEL_DEFAULTS rows";
            for (size_t i = 0; i < kCliGoldenModelRowCount; ++i) {
                const cli_golden_model_row &row = kCliGoldenModelRows[i];
                int64_t context = 0;
                int64_t output = 0;
                const bool known = cli::resolve_model_defaults(row.canonical, context, output);
                const int64_t want_output =
                    (row.canonical_output < 0) ? int64_t(0) : int64_t(row.canonical_output);
                expect(known == row.canonical_known) << row.canonical;
                if (known && row.canonical_known) {
                    expect(eq(context, int64_t(row.canonical_context))) << row.canonical;
                    expect(eq(output, want_output)) << row.canonical;
                }
            }
        };

        "cli_config_golden_model_corpus"_test = [] {
            expect(eq(kCliGoldenModelCaseCount, size_t(175)))
                << "the reference-derived model-name corpus";
            size_t known_cases = 0;
            size_t mask_cases = 0;
            for (size_t i = 0; i < kCliGoldenModelCaseCount; ++i) {
                const cli_golden_model_case &item = kCliGoldenModelCases[i];
                const kimix::string where = kimix::string(item.group) + " '" + item.name + "'";
                int64_t context = 0;
                int64_t output = 0;
                const bool known = cli::resolve_model_defaults(item.name, context, output);
                const int64_t want_context = item.context;
                const int64_t want_output =
                    (item.output < 0) ? int64_t(0) : int64_t(item.output);
                expect(known == item.known) << where;
                if (known && item.known) {
                    expect(eq(context, want_context)) << where;
                    expect(eq(output, want_output)) << where;
                }
                // The reference's per-row _keywords_match bitmask: a name resolves
                // exactly when some row matches, and the FIRST matching row is the
                // one _resolve_model_defaults returns.
                const bool mask_empty = (item.mask == 0u);
                expect(mask_empty == !item.known) << where;
                if (!mask_empty) {
                    size_t first = 0;
                    while (first < 32 && ((item.mask >> first) & 1u) == 0u) {
                        ++first;
                    }
                    expect(first < kCliGoldenModelRowCount) << where;
                    if (first < kCliGoldenModelRowCount) {
                        const cli_golden_model_row &row = kCliGoldenModelRows[first];
                        expect(eq(int64_t(row.context), want_context))
                            << where << " (first matching row)";
                        expect(eq(row.output < 0 ? int64_t(0) : int64_t(row.output), want_output))
                            << where << " (first matching row)";
                        ++mask_cases;
                    }
                }
                // The reference tokeniser is separator-insensitive and lowercases;
                // the token-joined and upper-cased spellings must resolve alike.
                const kimix::vector<kimix::string> tokens = cli_golden_tokens(item.tokens);
                if (!tokens.empty()) {
                    const kimix::string joined = cli_golden_join(tokens, "-");
                    int64_t joined_context = 0;
                    int64_t joined_output = 0;
                    const bool joined_known =
                        cli::resolve_model_defaults(joined, joined_context, joined_output);
                    expect(joined_known == item.known) << where << " via " << joined;
                    if (joined_known && item.known) {
                        expect(eq(joined_context, want_context)) << where << " via " << joined;
                        expect(eq(joined_output, want_output)) << where << " via " << joined;
                    }
                    const kimix::string upper = cli::to_upper_ascii(joined);
                    int64_t upper_context = 0;
                    int64_t upper_output = 0;
                    const bool upper_known =
                        cli::resolve_model_defaults(upper, upper_context, upper_output);
                    expect(upper_known == item.known) << where << " via " << upper;
                    if (upper_known && item.known) {
                        expect(eq(upper_context, want_context)) << where << " via " << upper;
                        expect(eq(upper_output, want_output)) << where << " via " << upper;
                    }
                }
                if (item.known) {
                    ++known_cases;
                }
            }
            expect(gt(known_cases, size_t(0))) << "the corpus covers resolvable names";
            expect(lt(known_cases, size_t(kCliGoldenModelCaseCount)))
                << "the corpus covers unresolvable names too";
            expect(gt(mask_cases, size_t(0))) << "and first-row cases";
        };

        "cli_tools_golden_manifest_paths"_test = [] {
            expect(eq(kCliGoldenManifestCount, size_t(5)));
            expect(kCliGoldenManifests[0].file == kimix::string("agent_worker.json"))
                << "the golden manifest order (worker first)";
            const kimix::vector<kimix::string> &defaults = cli::default_agent_tools();
            for (size_t m = 0; m < kCliGoldenManifestCount; ++m) {
                const cli_golden_manifest &manifest = kCliGoldenManifests[m];
                if (!manifest.present) {
                    printf("[skip] %s is not part of the reference checkout\n", manifest.file);
                    continue;
                }
                const kimix::vector<kimix::string> paths = cli_golden_tokens(manifest.tools);
                expect(eq(paths.size(), manifest.count)) << manifest.file;
                for (const kimix::string &path : paths) {
                    const kimix::string name = cli::resolve_tool_path(path);
                    expect(!name.empty()) << manifest.file << ": " << path;
                    expect(kimix::builtin_tools::ToolRegistry::instance().find(name) != nullptr)
                        << manifest.file << ": " << path << " -> " << name;
                    expect(cli_has_value(defaults, name)) << manifest.file << ": " << name;
                }
            }
            // The distinct union: 25 paths, all resolvable, all registered and
            // exactly the 25 registry names of default_agent_tools().
            expect(eq(kCliGoldenDistinctToolPathCount, size_t(25)));
            kimix::vector<kimix::string> resolved;
            for (size_t i = 0; i < kCliGoldenDistinctToolPathCount; ++i) {
                const kimix::string path = kCliGoldenDistinctToolPaths[i];
                const kimix::string name = cli::resolve_tool_path(path);
                expect(!name.empty()) << path;
                expect(kimix::builtin_tools::ToolRegistry::instance().find(name) != nullptr)
                    << path << " -> " << name;
                expect(!cli_has_value(resolved, name))
                    << "distinct paths map to distinct tools: " << name;
                resolved.push_back(name);
            }
            expect(eq(defaults.size(), resolved.size()));
            for (const kimix::string &name : defaults) {
                expect(cli_has_value(resolved, name)) << name << " missing from the union";
            }
            expect(eq(cli::agent_tool_table().size(), resolved.size()));
        };

        // =====================================================================
        // S6 · cli_config (cli/cli_config.h) - both provider dialects, the model
        // default resolution, the error paths, the agent manifest merge rules and
        // the deterministic secret-free reports.
        // =====================================================================

        "cli_config_flat_provider_dialect"_test = [] {
            const kimix::string dir = ws_dir("cli_config_flat");
            const kimix::string json =
                "{"
                "\"model\":\"gpt-5.4\","
                "\"type\":\"openai_legacy\","
                "\"base_url\":\"https://base.invalid/v1\","
                "\"url\":\"https://url.invalid/v1\","
                "\"api_key\":\"sk-flat-secret-value\","
                "\"max_context_size\":250000,"
                "\"max_tokens\":1234,"
                "\"capabilities\":[\"thinking\",\"image_in\"],"
                "\"custom_headers\":{\"X-One\":\"1\",\"X-Two\":\"2\"},"
                "\"reasoning_key\":\"reasoning_content_custom\","
                "\"thinking_effort\":\"low\","
                "\"show_thinking_stream\":false,"
                "\"openai_settings\":{\"thinking\":false,\"reasoning\":true},"
                "\"services\":{\"search\":{\"base_url\":\"https://search.invalid\","
                "\"api_key\":\"srch\"},\"fetch\":{\"base_url\":\"https://fetch.invalid\"}},"
                "\"oauth\":{\"key\":\"oauth-key\"},"
                "\"env\":{\"KIMIX_CLI_TEST_ENV_CFG\":\"from-config\"},"
                "\"unknown_flat_key\":1"
                "}";
            cli::provider_config provider;
            kimix::string error;
            const kimix::string path = cli_write_json(dir, "flat.json", json);
            expect(cli::load_provider_config(path, provider, error)) << error;
            expect(provider.source_path == path);
            expect(provider.model == "gpt-5.4");
            expect(provider.type == "openai_legacy") << "the type is kept verbatim";
            expect(provider.provider_family == "openai");
            expect(provider.base_url == "https://base.invalid/v1") << "base_url beats url";
            expect(provider.api_key == "sk-flat-secret-value");
            expect(provider.reasoning_key == "reasoning_content_custom");
            expect(provider.thinking_effort == "low");
            expect(!provider.show_thinking_stream);
            expect(provider.max_context_size == 250000);
            expect(provider.max_context_size_explicit);
            expect(provider.max_tokens == 1234);
            expect(provider.max_tokens_explicit);
            expect(eq(provider.capabilities.size(), size_t(2)));
            expect(provider.capabilities[0] == "thinking");
            expect(provider.capabilities[1] == "image_in");
            expect(eq(provider.custom_headers.size(), size_t(2)));
            expect(cli_pair_value(provider.custom_headers, "X-One") == "1");
            expect(cli_pair_value(provider.custom_headers, "X-Two") == "2");
            expect(!provider.openai.thinking);
            expect(provider.openai.reasoning);
            expect(provider.openai.chat_template_kwargs) << "an unset key keeps the default";
            expect(provider.search.base_url == "https://search.invalid");
            expect(provider.search.api_key == "srch");
            expect(provider.fetch.base_url == "https://fetch.invalid");
            expect(provider.fetch.api_key.empty());
            expect(provider.has_oauth);
            expect(provider.oauth_storage == "file") << "the OAuthRef default storage";
            expect(provider.oauth_key == "oauth-key");
            expect(eq(provider.env.size(), size_t(1)));
            kimix::string applied;
            expect(cli::get_env("KIMIX_CLI_TEST_ENV_CFG", applied));
            expect(applied == "from-config") << "env entries are applied after a good load";
            expect(cli_has_warning(provider.warnings, "unknown_flat_key"));
            expect(cli_has_warning(provider.warnings, "in provider config"));
            // to_llm_config: the normalised family create_llm() accepts, with the
            // resolved limits carried over.
            const kimix::llm::Config cfg = cli::to_llm_config(provider);
            expect(cfg.model == "gpt-5.4");
            expect(cfg.url == "https://base.invalid/v1");
            expect(cfg.api_key == "sk-flat-secret-value");
            expect(cfg.type == "openai");
            expect(cfg.max_context_size == 250000);
            expect(cfg.max_tokens == 1234);
            expect(!cfg.show_thinking_stream);
            expect(cfg.thinking_effort == "low");
            expect(kimix::llm::create_llm(cfg) != nullptr);
        };

        "cli_config_nested_provider_dialect"_test = [] {
            const kimix::string dir = ws_dir("cli_config_nested");
            const kimix::string json =
                "{"
                "\"model\":\"my-test-model\","
                "\"models\":{\"my-test-model\":{\"max_context_size\":777000,"
                "\"max_tokens\":8888,\"capabilities\":[\"thinking\"],"
                "\"display_name\":\"My Test\"}},"
                "\"provider\":{\"type\":\"anthropic\",\"base_url\":\"https://nested.invalid\","
                "\"url\":\"https://provider-url.invalid\",\"api_key\":\"sk-nested\","
                "\"env\":{\"KIMIX_CLI_TEST_ENV_NESTED\":\"1\"},"
                "\"custom_headers\":{\"Y\":\"2\"},\"openai_settings\":{\"thinking\":true}},"
                "\"type\":\"openai\",\"url\":\"https://flat-url.invalid\","
                "\"api_key\":\"sk-flat\",\"max_tokens\":1,\"show_thinking_stream\":true,"
                "\"thinking_effort\":\"medium\",\"default_thinking\":false,"
                "\"theme\":\"dark\",\"loop_control\":{\"max_steps_per_turn\":5}"
                "}";
            cli::provider_config provider;
            kimix::string error;
            const kimix::string path = cli_write_json(dir, "nested.json", json);
            expect(cli::load_provider_config(path, provider, error)) << error;
            expect(provider.model == "my-test-model");
            expect(provider.type == "anthropic") << "the nested provider type wins";
            expect(provider.provider_family == "anthropic");
            expect(provider.base_url == "https://nested.invalid")
                << "provider.base_url beats provider.url and the flat duplicates";
            expect(provider.api_key == "sk-nested");
            expect(provider.max_context_size == 777000) << "the models table entry wins";
            expect(provider.max_context_size_explicit);
            expect(provider.max_tokens == 8888) << "the models table entry wins";
            expect(provider.max_tokens_explicit);
            expect(eq(provider.capabilities.size(), size_t(1)));
            expect(provider.capabilities[0] == "thinking");
            expect(provider.reasoning_key == "reasoning_content") << "the default applies";
            expect(provider.thinking_effort == "medium");
            expect(provider.show_thinking_stream);
            expect(provider.openai.thinking);
            expect(cli_pair_value(provider.custom_headers, "Y") == "2");
            kimix::string applied;
            expect(cli::get_env("KIMIX_CLI_TEST_ENV_NESTED", applied));
            expect(applied == "1");
            expect(provider.warnings.empty()) << "every nested key is recognised";
            const kimix::llm::Config cfg = cli::to_llm_config(provider);
            expect(cfg.type == "anthropic");
            expect(cfg.max_context_size == 777000);
            expect(cfg.max_tokens == 8888);
            expect(kimix::llm::create_llm(cfg) != nullptr);
            // A `[model]` table form (no models table) is accepted too.
            cli::provider_config tabled;
            kimix::string table_error;
            expect(cli_load_provider_json(dir, "table.json",
                "{\"model\":{\"model\":\"tabled-model\",\"max_context_size\":1000,"
                "\"max_tokens\":100},\"type\":\"openai\",\"url\":\"https://x.invalid\"}",
                tabled, table_error)) << table_error;
            expect(tabled.model == "tabled-model");
            expect(tabled.max_context_size == 1000);
            expect(tabled.max_tokens == 100);
        };

        "cli_config_model_default_limits"_test = [] {
            const kimix::string dir = ws_dir("cli_config_limits");
            // (a) No explicit size: both limits come from the model defaults.
            {
                cli::provider_config provider;
                kimix::string error;
                expect(cli_load_provider_json(dir, "derived.json",
                    "{\"model\":\"gpt-5.4\",\"type\":\"openai\",\"url\":\"https://x.invalid\"}",
                    provider, error)) << error;
                expect(provider.max_context_size == 1000000);
                expect(provider.max_tokens == 128000);
                expect(!provider.max_context_size_explicit);
                expect(!provider.max_tokens_explicit);
                expect(cli_has_warning(provider.warnings,
                                       "max_context_size derived from the model defaults"));
                expect(cli_has_warning(provider.warnings,
                                       "max_tokens derived from the model defaults"));
            }
            // (b) An explicit context only: max_tokens = max_context_size / 4.
            {
                cli::provider_config provider;
                kimix::string error;
                expect(cli_load_provider_json(dir, "ctx_only.json",
                    "{\"model\":\"gpt-5.4\",\"type\":\"openai\",\"url\":\"https://x.invalid\","
                    "\"max_context_size\":40000}", provider, error)) << error;
                expect(provider.max_context_size == 40000);
                expect(provider.max_tokens == 10000);
                expect(provider.max_context_size_explicit);
                expect(!provider.max_tokens_explicit);
                expect(cli_has_warning(provider.warnings,
                                       "max_tokens derived from max_context_size / 4"));
            }
            // (c) A row whose default output is None (grok): context / 4, and
            // "kimi" normalises to the openai family.
            {
                cli::provider_config provider;
                kimix::string error;
                expect(cli_load_provider_json(dir, "grok.json",
                    "{\"model\":\"grok\",\"type\":\"kimi\",\"url\":\"https://u.invalid\","
                    "\"base_url\":\"https://b.invalid\"}", provider, error)) << error;
                expect(provider.provider_family == "openai");
                expect(provider.base_url == "https://b.invalid");
                expect(provider.max_context_size == 2000000);
                expect(provider.max_tokens == 500000)
                    << "the grok row's None output falls back to ctx / 4";
            }
            // (d) An unknown model without an explicit size is a config error.
            {
                cli::provider_config provider;
                kimix::string error;
                expect(!cli_load_provider_json(dir, "unknown.json",
                    "{\"model\":\"mystery-model-9\",\"type\":\"openai\","
                    "\"url\":\"https://x.invalid\"}", provider, error));
                expect(cli::contains(error, "Unknown model 'mystery-model-9'"));
                expect(cli::contains(error, "Cannot determine max_context_size"));
            }
            // (e) Direct resolver checks, including the fuzzy boundary.
            int64_t context = 0;
            int64_t output = 0;
            expect(cli::resolve_model_defaults("gpt-5.4", context, output));
            expect(context == 1000000);
            expect(output == 128000);
            expect(cli::resolve_model_defaults("gpt-5.4-mini", context, output));
            expect(context == 1000000);
            expect(output == 65536) << "the more specific mini row wins";
            expect(cli::resolve_model_defaults("grok", context, output));
            expect(context == 2000000);
            expect(output == 0) << "the reference's None maps to 0";
            expect(cli::resolve_model_defaults("supergrok-heavy", context, output));
            expect(context == 2000000);
            expect(output == 0);
            expect(!cli::resolve_model_defaults("", context, output));
            expect(!cli::resolve_model_defaults("llama-3-70b", context, output));
            // rapidfuzz fuzz.ratio("mini", "gemini") == 80 == the threshold, so the
            // alphabetic keyword "mini" fuzzy-matches the model token "gemini".
            expect(cli::resolve_model_defaults("gpt-5.4-gemini", context, output));
            expect(context == 1000000);
            expect(output == 65536);
            // Numeric keyword tokens must match exactly.
            expect(!cli::resolve_model_defaults("gpt-5.9", context, output));
            expect(!cli::resolve_model_defaults("claude-opus-6", context, output));
            expect(!cli::resolve_model_defaults("gemini-3.7", context, output));
        };

        "cli_config_type_normalisation"_test = [] {
            const kimix::string dir = ws_dir("cli_config_types");
            struct cli_type_case {
                const char *type;
                const char *family;
            };
            static const cli_type_case kCases[] = {
                {"openai", "openai"},
                {"openai_legacy", "openai"},
                {"kimi", "openai"},
                {"moonshot", "openai"},
                {"moonshot-v1-128k", "openai"},
                {"openai_chat", "openai"},
                {"openai_chat_completions", "openai"},
                {"OpenAI_Legacy", "openai"},
                {"openai_responses", "openai_responses"},
                {"openai_responses_v2", "openai_responses"},
                {"responses", "openai_responses"},
                {"anthropic", "anthropic"},
                {"anthropic_messages", "anthropic"},
                {"claude-3-5-sonnet", "anthropic"},
            };
            for (const cli_type_case &item : kCases) {
                const kimix::string json =
                    kimix::string("{\"model\":\"gpt-5.4\",\"type\":\"") + item.type +
                    "\",\"url\":\"https://x.invalid\",\"max_context_size\":1000,"
                    "\"max_tokens\":100}";
                cli::provider_config provider;
                kimix::string error;
                expect(cli_load_provider_json(dir, "type.json", json, provider, error))
                    << item.type << ": " << error;
                expect(provider.provider_family == item.family) << item.type;
                const kimix::llm::Config cfg = cli::to_llm_config(provider);
                expect(kimix::llm::create_llm(cfg) != nullptr)
                    << item.type << " -> " << cfg.type << " is accepted by create_llm";
            }
            // Unsupported types are rejected by name.
            cli::provider_config provider;
            kimix::string error;
            expect(!cli_load_provider_json(dir, "bad.json",
                "{\"model\":\"gpt-5.4\",\"type\":\"llama\",\"url\":\"https://x.invalid\","
                "\"max_context_size\":1000}", provider, error));
            expect(cli::contains(error, "unsupported provider type 'llama'"));
            expect(!cli_load_provider_json(dir, "bad2.json",
                "{\"model\":\"gpt-5.4\",\"type\":\"gemini\",\"url\":\"https://x.invalid\","
                "\"max_context_size\":1000}", provider, error));
            expect(cli::contains(error, "unsupported provider type 'gemini'"));
        };

        "cli_config_error_paths"_test = [] {
            const kimix::string dir = ws_dir("cli_config_errors");
            cli::provider_config provider;
            kimix::string error;
            expect(!cli::load_provider_config(cli::join_path(dir, "absent.json"), provider,
                                              error));
            expect(cli::contains(error, "cannot read provider config"));
            expect(provider.model.empty()) << "out is reset on failure";
            expect(!cli_load_provider_json(dir, "broken.json", "{oops", provider, error));
            expect(cli::contains(error, "invalid JSON in provider config"));
            expect(!cli_load_provider_json(dir, "array.json", "[1,2]", provider, error));
            expect(cli::contains(error, "must be a JSON object"));
            expect(!cli_load_provider_json(dir, "no_type.json",
                "{\"model\":\"gpt-5.4\",\"url\":\"https://x.invalid\"}", provider, error));
            expect(cli::contains(error, "missing required field 'type'"));
            expect(!cli_load_provider_json(dir, "no_model.json",
                "{\"type\":\"openai\",\"url\":\"https://x.invalid\"}", provider, error));
            expect(cli::contains(error, "missing required field 'model'"));
            expect(!cli_load_provider_json(dir, "no_url.json",
                "{\"model\":\"gpt-5.4\",\"type\":\"openai\"}", provider, error));
            expect(cli::contains(error, "missing required field 'url' or 'base_url'"));
            // An explicit context size rescues an otherwise unknown model.
            expect(cli_load_provider_json(dir, "unknown_ok.json",
                "{\"model\":\"mystery-9\",\"type\":\"openai\",\"url\":\"https://x.invalid\","
                "\"max_context_size\":123}", provider, error)) << error;
            expect(provider.max_context_size == 123);
            expect(provider.max_tokens == 30) << "123 / 4";
            expect(cli_has_warning(provider.warnings, "max_tokens derived from "
                                                      "max_context_size / 4"));
        };

        "cli_config_api_key_env_fallback"_test = [] {
            const kimix::string dir = ws_dir("cli_config_api_key");
            // Preserve whatever the outer environment has: this test must not
            // leak into the other sections.
            kimix::string saved_kimi;
            kimix::string saved_kimix;
            const bool had_kimi = cli::get_env("KIMI_API_KEY", saved_kimi);
            const bool had_kimix = cli::get_env("KIMIX_API_KEY", saved_kimix);
            const kimix::string json =
                "{\"model\":\"gpt-5.4\",\"type\":\"openai\",\"url\":\"https://x.invalid\","
                "\"max_context_size\":1000,\"max_tokens\":100}";
            cli::provider_config provider;
            kimix::string error;
            // (a) $KIMI_API_KEY first.
            expect(cli::set_env("KIMI_API_KEY", "kimi-env-key"));
            expect(cli::set_env("KIMIX_API_KEY", "kimix-env-key"));
            expect(cli_load_provider_json(dir, "env_a.json", json, provider, error)) << error;
            expect(provider.api_key == "kimi-env-key");
            expect(cli_has_warning(provider.warnings, "using $KIMI_API_KEY"));
            // (b) $KIMIX_API_KEY when the first is set-but-empty.
            expect(cli::set_env("KIMI_API_KEY", ""));
            expect(cli_load_provider_json(dir, "env_b.json", json, provider, error)) << error;
            expect(provider.api_key == "kimix-env-key");
            expect(cli_has_warning(provider.warnings, "using $KIMIX_API_KEY"));
            // (c) Neither: the key stays empty and is warned about.
            expect(cli::set_env("KIMI_API_KEY", ""));
            expect(cli::set_env("KIMIX_API_KEY", ""));
            expect(cli_load_provider_json(dir, "env_c.json", json, provider, error)) << error;
            expect(provider.api_key.empty());
            expect(cli_has_warning(provider.warnings, "api_key missing in config"));
            // (d) An explicit api_key always wins over the environment.
            expect(cli::set_env("KIMI_API_KEY", "kimi-env-key"));
            expect(cli_load_provider_json(dir, "env_d.json",
                "{\"model\":\"gpt-5.4\",\"type\":\"openai\",\"url\":\"https://x.invalid\","
                "\"api_key\":\"explicit-key\",\"max_context_size\":1000,\"max_tokens\":100}",
                provider, error)) << error;
            expect(provider.api_key == "explicit-key");
            expect(!cli_has_warning(provider.warnings, "api_key missing"));
            if (had_kimi) {
                expect(cli::set_env("KIMI_API_KEY", saved_kimi));
            } else {
                expect(cli::set_env("KIMI_API_KEY", ""));
            }
            if (had_kimix) {
                expect(cli::set_env("KIMIX_API_KEY", saved_kimix));
            } else {
                expect(cli::set_env("KIMIX_API_KEY", ""));
            }
        };

        "cli_config_agent_manifest_semantics"_test = [] {
            const kimix::string dir = ws_dir("cli_config_agent");
            const kimix::string prompts = cli::join_path(dir, "prompts");
            kimix::string dir_error;
            expect(cli::make_dirs(prompts, dir_error)) << dir_error;
            // tools replaces the inherited list; unknown paths warn and drop.
            {
                cli::agent_config agent;
                kimix::string error;
                expect(cli_load_agent_json(dir, "replace.json",
                    "{\"agent\":{\"extend\":\"default\",\"tools\":["
                    "\"kimi_cli.tools.file:read\",\"kimi_cli.tools.file:grep\","
                    "\"nope.module:thing\"]}}", agent, error)) << error;
                expect(agent.extend == "default");
                expect(agent.has_tools);
                expect(eq(agent.tools.size(), size_t(3)));
                expect(eq(agent.enabled_tools.size(), size_t(2)));
                expect(agent.enabled_tools[0] == "Read");
                expect(agent.enabled_tools[1] == "Grep");
                expect(agent.name.empty());
                expect(agent.model.empty());
                expect(agent.when_to_use.empty());
                expect(agent.system_prompt_path.empty());
                expect(agent.manifest_path == cli::join_path(dir, "replace.json"));
                expect(agent.manifest_dir == dir);
                expect(cli_has_warning(agent.warnings,
                                       "dropped unknown tool path 'nope.module:thing'"));
            }
            // allowed_tools wins over tools; exclude_tools removes exact matches.
            {
                cli::agent_config agent;
                kimix::string error;
                expect(cli_load_agent_json(dir, "allowed.json",
                    "{\"agent\":{\"tools\":[\"kimi_cli.tools.file:read\"],"
                    "\"allowed_tools\":[\"kimi_cli.tools.file:read\","
                    "\"kimi_cli.tools.file:glob\",\"kimi_cli.tools.file:grep\"],"
                    "\"exclude_tools\":[\"kimi_cli.tools.file:glob\"],"
                    "\"name\":\"role-name\",\"model\":\"gpt-5.4\","
                    "\"when_to_use\":\"sometimes\","
                    "\"subagents\":{\"alpha\":{},\"beta\":{}},"
                    "\"system_prompt_path\":\"prompts/main.md\","
                    "\"system_prompt_args\":{\"one\":\"1\",\"two\":\"2\"}}}",
                    agent, error)) << error;
                expect(eq(agent.tools.size(), size_t(1)));
                expect(eq(agent.allowed_tools.size(), size_t(3)));
                expect(eq(agent.exclude_tools.size(), size_t(1)));
                expect(eq(agent.enabled_tools.size(), size_t(2)));
                expect(agent.enabled_tools[0] == "Read");
                expect(agent.enabled_tools[1] == "Grep")
                    << "allowed_tools wins over tools; glob was excluded";
                expect(agent.name == "role-name");
                expect(agent.model == "gpt-5.4");
                expect(agent.when_to_use == "sometimes");
                expect(eq(agent.subagents.size(), size_t(2)));
                expect(agent.subagents[0] == "alpha");
                expect(agent.subagents[1] == "beta");
                expect(cli::file_name(agent.system_prompt_path) == "main.md");
                expect(cli::parent_path(agent.system_prompt_path) == prompts)
                    << "a relative prompt path resolves against the manifest dir";
                expect(eq(agent.system_prompt_args.size(), size_t(2)));
                expect(agent.system_prompt_args[0].first == "one");
                expect(agent.system_prompt_args[0].second == "1");
                expect(agent.system_prompt_args[1].first == "two");
                expect(agent.system_prompt_args[1].second == "2");
                expect(agent.warnings.empty()) << "no unknown key at all";
            }
            // No tools/allowed_tools -> the built-in default agent.
            {
                cli::agent_config agent;
                kimix::string error;
                expect(cli_load_agent_json(dir, "default.json",
                    "{\"agent\":{\"extend\":\"default\"}}", agent, error)) << error;
                expect(!agent.has_tools);
                expect(agent.enabled_tools.size() == cli::default_agent_tools().size());
                bool same = true;
                for (size_t i = 0; i < agent.enabled_tools.size(); ++i) {
                    if (agent.enabled_tools[i] != cli::default_agent_tools()[i]) {
                        same = false;
                    }
                }
                expect(same) << "extend:'default' yields the 25 built-in tools in order";
            }
            // Prompt path resolution and the version field.
            {
                cli::agent_config agent;
                kimix::string error;
                expect(cli_load_agent_json(dir, "rel.json",
                    "{\"version\":1,\"agent\":{\"system_prompt_path\":\"./rel.md\"}}",
                    agent, error)) << error;
                expect(agent.system_prompt_path == cli::join_path(dir, "rel.md"))
                    << "a leading ./ is stripped";
                expect(cli_load_agent_json(dir, "abs.json",
                    "{\"agent\":{\"system_prompt_path\":\"/abs/prompt.md\"}}",
                    agent, error)) << error;
                expect(agent.system_prompt_path == "/abs/prompt.md")
                    << "an absolute prompt path is kept";
                expect(cli_load_agent_json(dir, "drive.json",
                    "{\"agent\":{\"system_prompt_path\":\"C:/abs/prompt.md\"}}",
                    agent, error)) << error;
                expect(agent.system_prompt_path == "C:/abs/prompt.md");
            }
            // A bare top-level agent object (no "agent" wrapper) is accepted.
            {
                cli::agent_config agent;
                kimix::string error;
                expect(cli_load_agent_json(dir, "bare.json",
                    "{\"tools\":[\"kimi_cli.tools.file:read\"]}", agent, error)) << error;
                expect(eq(agent.enabled_tools.size(), size_t(1)));
                expect(agent.enabled_tools[0] == "Read");
            }
            // Error paths.
            {
                cli::agent_config agent;
                kimix::string error;
                expect(!cli_load_agent_json(dir, "v2.json",
                    "{\"version\":\"2\",\"agent\":{\"extend\":\"default\"}}", agent, error));
                expect(cli::contains(error, "unsupported agent spec version: 2"));
                agent = cli::agent_config{};
                error.clear();
                expect(!cli_load_agent_json(dir, "arr.json", "[1,2]", agent, error));
                expect(cli::contains(error, "must be a JSON object"));
                agent = cli::agent_config{};
                error.clear();
                expect(!cli_load_agent_json(dir, "bad.json", "{oops", agent, error));
                expect(cli::contains(error, "invalid JSON in agent manifest"));
                error.clear();
                expect(!cli::load_agent_config(cli::join_path(dir, "absent.json"), agent,
                                               error));
                expect(cli::contains(error, "cannot read agent manifest"));
            }
        };

        "cli_config_reports_are_secret_free"_test = [] {
            const kimix::string dir = ws_dir("cli_config_reports");
            const kimix::string secret = "sk-SECRET-DO-NOT-PRINT-4242";
            cli::provider_config provider;
            kimix::string error;
            const kimix::string json =
                "{\"model\":\"gpt-5.4\",\"type\":\"openai\",\"url\":\"https://x.invalid\","
                "\"api_key\":\"" + secret + "\",\"max_context_size\":250000,"
                "\"max_tokens\":1234,\"capabilities\":[\"thinking\"],"
                "\"custom_headers\":{\"X\":\"1\"},\"env\":{\"E\":\"1\"},"
                "\"services\":{\"search\":{\"base_url\":\"https://s.invalid\"}},"
                "\"oauth\":{\"key\":\"k\"}}";
            expect(cli_load_provider_json(dir, "report.json", json, provider, error)) << error;
            const kimix::string report = cli::provider_report(provider);
            expect(cli::starts_with(report, "ProviderConfig: "));
            expect(cli::contains(report, "  model: gpt-5.4\n"));
            expect(cli::contains(report, "  type: openai\n"));
            expect(cli::contains(report, "  family: openai\n"));
            expect(cli::contains(report, "  base_url: https://x.invalid\n"));
            expect(cli::contains(report, "  api_key: present\n")) << "presence, never the value";
            expect(cli::contains(report, "  reasoning_key: reasoning_content\n"));
            expect(cli::contains(report, "  thinking_effort: high\n"));
            expect(cli::contains(report, "  show_thinking_stream: true\n"));
            expect(cli::contains(report, "  max_context_size: 250000 (explicit)\n"));
            expect(cli::contains(report, "  max_tokens: 1234 (explicit)\n"));
            expect(cli::contains(report, "  capabilities: thinking\n"));
            expect(cli::contains(report, "  custom_headers: 1\n"));
            expect(cli::contains(report, "  env: 1\n"));
            expect(cli::contains(report, "  has_oauth: true\n"));
            expect(cli::contains(report, "  services: search=present fetch=absent\n"));
            expect(cli::contains(report, "  warnings: 0\n"));
            expect(!cli::contains(report, secret)) << "the api_key value is never printed";
            expect(report == cli::provider_report(provider)) << "deterministic";
            // The absent-api_key and label variants.
            cli::provider_config bare;
            bare.model = "gpt-5.4";
            bare.type = "openai";
            bare.provider_family = "openai";
            bare.base_url = "https://x.invalid";
            bare.reasoning_key = "reasoning_content";
            bare.thinking_effort = "high";
            const kimix::string bare_report = cli::provider_report(bare);
            expect(cli::contains(bare_report, "  api_key: absent\n"));
            expect(cli::contains(bare_report, "  max_context_size: 0 (from model defaults)\n"));
            expect(cli::contains(bare_report, "  max_tokens: 0 (from model defaults)\n"));
            expect(cli::contains(bare_report, "  capabilities: (none)\n"));
            cli::provider_config derived;
            derived.max_context_size = 40000;
            derived.max_context_size_explicit = true;
            derived.max_tokens = 10000;
            expect(cli::contains(cli::provider_report(derived),
                                 "  max_tokens: 10000 (derived: max_context_size / 4)\n"));
            // agent_report: the built-in default and a loaded manifest.
            cli::agent_config agent;
            const kimix::string agent_text = cli::agent_report(agent);
            expect(cli::starts_with(agent_text, "AgentConfig: (built-in default agent)\n"));
            expect(cli::contains(agent_text, "  name: (inherit)\n"));
            expect(cli::contains(agent_text, "  extend: default (built-in)\n"));
            expect(cli::contains(agent_text, "  model: (inherit)\n"));
            expect(cli::contains(agent_text,
                                 "  system_prompt_path: (inherited from default)\n"));
            expect(cli::contains(agent_text, "  tools requested: 0\n"));
            expect(cli::contains(agent_text, "  tools enabled: 0\n"));
            expect(cli::contains(agent_text, "  tools dropped: 0\n"));
            expect(cli::contains(agent_text, "  enabled: (none)\n"));
            expect(cli::contains(agent_text, "  warnings: 0\n"));
            expect(agent_text == cli::agent_report(agent));
            expect(!cli::contains(agent_text, secret));
            cli::agent_config loaded;
            kimix::string load_error;
            expect(cli_load_agent_json(dir, "report_agent.json",
                "{\"agent\":{\"tools\":[\"kimi_cli.tools.file:read\",\"bogus:x\"]}}",
                loaded, load_error)) << load_error;
            const kimix::string loaded_text = cli::agent_report(loaded);
            expect(cli::contains(loaded_text, "  tools requested: 2\n"));
            expect(cli::contains(loaded_text, "  tools enabled: 1\n"));
            expect(cli::contains(loaded_text, "  tools dropped: 1\n"));
            expect(cli::contains(loaded_text, "  enabled: Read\n"));
            expect(cli::contains(loaded_text, "bogus:x"));
            expect(!cli::contains(loaded_text, secret));
        };

        // =====================================================================
        // S6 · cli_app (cli/cli_app.h) - the --dry-run report over the real
        // reference provider + manifest pair (read-only; skipped when absent).
        // =====================================================================

        "cli_app_dry_run_report_real_provider"_test = [] {
            const kimix::string provider_path = "C:/dev/ds_flash.json";
            const kimix::string manifest_path =
                cli::join_path(cli::join_path(cli_reference_root(), "src"),
                               "kimix/agent_worker.json");
            if (!cli::file_exists(provider_path) || !cli::file_exists(manifest_path)) {
                printf("[skip] reference configs missing (%s / %s)\n", provider_path.c_str(),
                       manifest_path.c_str());
            } else {
                const kimix::string work = ws_dir("cli_app_dry_run_real");
                cli::cli_options opts;
                opts.config_path = provider_path;
                opts.config_is_provider_only = true;
                opts.agent_file = manifest_path;
                opts.work_dir = work;
                opts.no_color = true;
                opts.dry_run = true;
                cli::app_context app;
                kimix::string error;
                expect(cli::app_init(opts, app, error, nullptr)) << error;
                const kimix::string report = cli::app_dry_run_report(app);
                expect(cli::starts_with(
                    report, "LLMConfig: model=deepseek-v4.1-flash-official type=openai"))
                    << report;
                expect(cli::contains(report, "create_llm=ok"));
                expect(cli::contains(report, "ProviderConfig: " + provider_path));
                expect(cli::contains(report, "  family: openai\n"));
                expect(cli::contains(report, "  max_context_size: 1024000 (explicit)\n"));
                expect(cli::contains(report, "  max_tokens: 256000 "
                                             "(derived: max_context_size / 4)\n"));
                expect(cli::contains(report, "AgentConfig: " + manifest_path));
                expect(cli::contains(report, "  tools requested: 22\n"));
                expect(cli::contains(report, "  tools enabled: 22\n"));
                expect(cli::contains(report, "  tools dropped: 0\n"));
                expect(cli::ends_with(report, "\n\nOK"));
                // The enabled list is the worker manifest's tool paths, in
                // manifest order, mapped through resolve_tool_path (the golden
                // .inc holds the reference's path list).
                const kimix::vector<kimix::string> golden =
                    cli_golden_tokens(kCliGoldenManifests[0].tools);
                kimix::string expected;
                for (size_t i = 0; i < golden.size(); ++i) {
                    if (i != 0) {
                        expected += ", ";
                    }
                    expected += cli::resolve_tool_path(golden[i]);
                }
                expect(cli::contains(report, "  enabled: " + expected + "\n")) << report;
                // The real api_key value must never reach the report.
                kimix::string provider_text;
                kimix::string read_error;
                expect(cli::read_file(provider_path, provider_text, read_error)) << read_error;
                const kimix::string api_key = cli_json_field(provider_text, "api_key");
                expect(!api_key.empty()) << "the fixture really carries an api_key";
                expect(!cli::contains(report, api_key))
                    << "the provider api_key value must never be printed";
                expect(cli::contains(report, "  api_key: present\n"));
                expect(!cli::dir_exists(cli::join_path(work, ".kimix_cache")))
                    << "--dry-run creates no session";
            }
        };
    }

