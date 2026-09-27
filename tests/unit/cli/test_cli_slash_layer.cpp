// test_cli_slash_layer.cpp - G5/I1/I2/I3/I6/I7/I8/I9/H6: the extended slash
// layer and the session commands of the native CLI, driven in-process over a
// scripted IChatBackend (no network).
//
// Covered:
//   * /yolo + /afk (kimi_cli/soul/slash.py:198-246): the four user-visible
//     strings incl. the "yolo off but afk still on" warning, and the
//     persistence through the approval gate's on_change hook (G5).
//   * /reset == /clear (slash.py ALIASES) and list_command_infos() (I2).
//   * /add-dir + /import + /refresh-env (I2).
//   * /export resolved-path echo, the directory form and the whitespace arg
//     form (I6); the failure string carrying the error (I8).
//   * /sessions over the in-process cache with the 3-space header and the
//     local-time column (I7).
//   * /store release + recovery (I9) and /exit temp-folder cleanup (H6).
//   * /plan: 3 attempts + the retry nudge + "plan file not found", the
//     y/n review + revision loop, /quit, and the implement + review turns (I1).
//   * app_run_prompt: the todo closing rounds (G13), the >64 KB temp-file rule
//     and escape_file_paths (I3), and the "Prompt failed: {e}" wording (I8).
//
// Framework: Boost.UT (tests/ut/ut.hpp); every test body lives in a
// main()-scope "name"_test lambda.

#include "ut/ut.hpp"

#include <cstdio>
#include <cstdlib>
#include <system_error>

#include <core/kimix_core.h>

#include "builtin_tools/todo_tool.h"
#include "agent/soul.h"

#include "cli/cli_app.h"
#include "cli/cli_commands.h"
#include "cli/cli_common.h"
#include "cli/cli_init_wizard.h"
#include "cli/cli_print.h"
#include "cli/cli_session.h"

#include <utility>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

namespace cli = kimix::cli;
using namespace boost::ut;
using kimix::cli::contains;
using kimix::cli::find;
using kimix::cli::rfind;

bool has_substr(kimix::string_view haystack, kimix::string_view needle) {
    return cli::contains(haystack, needle);
}

// A fresh, empty workspace under the system temp directory (a per-file name
// prefix keeps the fixed dirs of the parallel suites distinct).
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

// One scripted chat step.
struct test_step {
    kimix::string content;
    kimix::string reasoning;
    kimix::vector<kimix::llm::ToolCall> calls;
};

// The scripted backend: steps[calls++] per chat call, plus an optional hook
// the /plan tests use to write the plan file from inside a planner turn.
class test_backend : public kimix::agent::IChatBackend {
public:
    kimix::vector<test_step> steps;
    kimix::function<void(int32_t call_index)> on_call;
    kimix::string fail_error; // when set, every call fails with this error
    int32_t calls = 0;
    int64_t context_size = 100000;

    kimix::llm::ChatResult chat(const kimix::vector<kimix::llm::Message> &,
                                const kimix::vector<kimix::llm::Tool> &,
                                const kimix::llm::ChunkCallback &on_chunk) override {
        kimix::llm::ChatResult result;
        if (!fail_error.empty()) {
            result.ok = false;
            result.error = fail_error;
            ++calls;
            return result;
        }
        result.ok = true;
        if (on_call) {
            on_call(calls);
        }
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
    kimix::string model_name() const override { return "slash-layer-test"; }
};

// Redirect fd 1+2 into `path` for the duration of a test.
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

// A configured app_context with the scripted backend.  `extra_config` is
// spliced into the provider JSON (loop_control knobs, sub-providers, ...).
struct app_fixture {
    kimix::string work;
    kimix::string provider;
    cli::cli_options opts;
    cli::app_context app;
    test_backend backend;
    std::FILE *render_out = nullptr;
    cli::stream_renderer renderer{true, true};
    kimix::string error;

    bool init(const char *name, const kimix::string &extra_config = {}) {
        work = ws_dir(name);
        provider = cli::join_path(work, "provider.json");
        kimix::string write_error;
        kimix::string json =
            "{\"model\":\"slash-test-model\",\"type\":\"openai\","
            "\"url\":\"http://127.0.0.1:1/v1\",\"api_key\":\"test\","
            "\"max_context_size\":100000,\"max_tokens\":100,"
            "\"loop_control\":{"
            "\"budget_reminder_enabled\":false,"
            "\"context_meter_enabled\":false,"
            "\"todo_reminder_enabled\":false,"
            "\"target_churn_enabled\":false,"
            "\"compact_reminder_enabled\":false,"
            "\"auto_retrieve_history\":false,"
            "\"auto_retrieve_working_memory\":false,"
            "\"auto_retrieve_recency_memory\":false"
            "}}";
        if (!extra_config.empty()) {
            // splice extra top-level members before the final '}'
            json.insert(json.size() - 1, ",");
            json.insert(json.size() - 1, extra_config);
        }
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

    void shutdown() {
        app.soul.reset();
        app.session.reset();
        kimix::string close_error;
        app.store.close(true, close_error);
    }
};

// The command handlers print through cli_print -> stdout; capture one call.
kimix::string capture_handler(const cli::command_entry *entry,
                              const kimix::vector<kimix::string> &args,
                              cli::app_context &app,
                              cli::command_result *result = nullptr) {
    kimix::vector<kimix::string> text_arr;
    output_capture capture;
    expect(capture.begin(cli::join_path(app.work_dir, "capture.txt")));
    cli::set_colorful(false);
    const cli::command_result r = entry->handler(args, app, text_arr);
    const kimix::string out = capture.end();
    cli::set_colorful(true);
    if (result != nullptr) {
        *result = r;
    }
    return out;
}

} // namespace

int main() {
    "yolo_toggle_strings_and_persistence"_test = [] {
        app_fixture fx;
        expect(fx.init("cli_slash_yolo"));
        const cli::command_entry *yolo = cli::find_command("yolo");
        expect(yolo != nullptr);
        // The fixture starts yolo-on (no --no_yolo): first toggle disables it.
        kimix::string out = capture_handler(yolo, {"yolo"}, fx.app);
        expect(has_substr(out, "You only die once! Actions will require approval."));
        expect(!fx.app.approval->is_yolo());
        // afk stays off -> the plain wording (no "afk still on" variant).
        expect(!has_substr(out, "afk is still on"));
        out = capture_handler(yolo, {"yolo"}, fx.app);
        expect(has_substr(out, "You only live once! All actions will be auto-approved."));
        expect(fx.app.approval->is_yolo());
        // G5: with afk on, disabling yolo warns that tool calls remain approved.
        fx.app.approval->set_afk(true);
        out = capture_handler(yolo, {"yolo"}, fx.app);
        expect(has_substr(out, "Yolo disabled, but afk is still on"));
        expect(has_substr(out, "Use /afk to turn off afk."));
        // The gate's on_change hook copied the state into session state.
        expect(!fx.app.approval->is_yolo());
        expect(!fx.app.state.yolo) << "the persisted yolo flag flipped";
        fx.shutdown();
    };

    "afk_toggle_strings_and_persistence"_test = [] {
        app_fixture fx;
        expect(fx.init("cli_slash_afk"));
        const cli::command_entry *afk = cli::find_command("afk");
        expect(afk != nullptr);
        kimix::string out = capture_handler(afk, {"afk"}, fx.app);
        expect(has_substr(out,
                          "afk mode enabled. AskUserQuestion will be auto-dismissed "
                          "and tool calls auto-approved."));
        expect(fx.app.approval->is_afk());
        expect(fx.app.state.afk) << "the persisted afk flag flipped";
        // Disabling with yolo still on names it (slash.py:228-235).
        out = capture_handler(afk, {"afk"}, fx.app);
        expect(has_substr(out, "afk mode disabled. You are back at the terminal."));
        expect(has_substr(out, "Yolo is still on."));
        expect(!fx.app.approval->is_afk());
        fx.shutdown();
    };

    "reset_alias_resolves_to_clear"_test = [] {
        expect(cli::find_command("reset") == cli::find_command("clear"))
            << "slash.py ALIASES: /reset == /clear";
        app_fixture fx;
        expect(fx.init("cli_slash_reset"));
        fx.backend.steps.push_back({"an answer with a few words", "", {}});
        output_capture capture;
        expect(capture.begin(cli::join_path(fx.work, "turn.txt")));
        cli::set_colorful(false);
        expect(cli::app_run_prompt(fx.app, "hello"));
        capture.end();
        expect(!fx.app.session->history().empty());
        const cli::command_entry *reset = cli::find_command("reset");
        kimix::string out = capture_handler(reset, {"reset"}, fx.app);
        expect(fx.app.session->history().empty()) << "/reset cleared the context";
        expect(has_substr(out, "Context usage: "));
        fx.shutdown();
    };

    "list_command_infos_soul_table"_test = [] {
        const kimix::vector<cli::command_info> &infos = cli::list_command_infos();
        // slash.py:404-415 COMMANDS, in insertion order.
        expect(eq(infos.size(), size_t(10)));
        expect(infos[0].name == kimix::string("init"));
        expect(infos[0].description ==
               kimix::string("Analyze the codebase and generate an `AGENTS.md` file"));
        bool clear_has_reset = false;
        for (const cli::command_info &info : infos) {
            expect(!info.description.empty());
            if (info.name == "clear") {
                clear_has_reset = !info.aliases.empty() && info.aliases[0] == "reset";
            }
        }
        expect(clear_has_reset) << "the /reset alias rides on /clear";
    };

    "add_dir_lifecycle"_test = [] {
        app_fixture fx;
        expect(fx.init("cli_slash_addir"));
        // A sibling directory outside the work dir.
        const kimix::string outside =
            cli::join_path(kimix::string{kimix::to_string(
                               kimix::filesystem::path(fx.work).parent_path())},
                           "cli_slash_addir_outside");
        std::error_code ec;
        kimix::filesystem::remove_all(kimix::filesystem::path(outside), ec);
        kimix::filesystem::create_directories(kimix::filesystem::path(outside), ec);
        expect(kimix::filesystem::exists(kimix::filesystem::path(outside), ec));

        const cli::command_entry *add_dir = cli::find_command("add-dir");
        expect(add_dir != nullptr);
        // Listing with nothing added.
        kimix::string out = capture_handler(add_dir, {"add-dir"}, fx.app);
        expect(has_substr(out, "No additional directories. Usage: /add-dir <path>"));
        // Missing directory.
        out = capture_handler(add_dir, {"add-dir", "no/such/dir"}, fx.app);
        expect(has_substr(out, "Directory does not exist: "));
        // Within the work dir -> refused (an existing directory, the
        // reference checks existence first).
        kimix::string mk_error;
        expect(cli::make_dirs(cli::join_path(fx.work, "sub"), mk_error));
        out = capture_handler(add_dir, {"add-dir", "sub"}, fx.app);
        expect(has_substr(out,
                          "Directory is already within the working directory: "));
        // The real thing.
        out = capture_handler(add_dir, {"add-dir", outside}, fx.app);
        expect(has_substr(out, "Added directory to workspace: "));
        expect(fx.app.state.additional_dirs.size() == 1);
        // Duplicate.
        out = capture_handler(add_dir, {"add-dir", outside}, fx.app);
        expect(has_substr(out, "Directory already in workspace: "));
        // Listing.
        out = capture_handler(add_dir, {"add-dir"}, fx.app);
        expect(has_substr(out, "Additional directories:"));
        expect(has_substr(out, "  - "));
        // Cleanup.
        kimix::filesystem::remove_all(kimix::filesystem::path(outside), ec);
        fx.shutdown();
    };

    "import_from_file_and_session"_test = [] {
        app_fixture fx;
        expect(fx.init("cli_slash_import"));
        const cli::command_entry *import = cli::find_command("import");
        expect(import != nullptr);
        // Usage.
        kimix::string out = capture_handler(import, {"import"}, fx.app);
        expect(has_substr(out, "Usage: /import <file_path or session_id>"));
        // Directory refusal.
        out = capture_handler(import, {"import", "."}, fx.app);
        expect(has_substr(out,
                          "The specified path is a directory; please provide a "
                          "file to import."));
        // Unsupported extension.
        const kimix::string bin = cli::join_path(fx.work, "blob.bin");
        kimix::string error;
        expect(cli::write_file(bin, "\x00\x01", error));
        out = capture_handler(import, {"import", "blob.bin"}, fx.app);
        expect(has_substr(out, "Unsupported file type '.bin'."));
        // Unknown target.
        out = capture_handler(import, {"import", "nope.md"}, fx.app);
        expect(has_substr(out, "'nope.md' is not a valid file path or session ID."));
        // A real file, with a sensitive name -> the secrets warning rides on.
        const kimix::string notes = cli::join_path(fx.work, "secrets.md");
        expect(cli::write_file(notes, "imported body", error));
        out = capture_handler(import, {"import", "secrets.md"}, fx.app);
        expect(has_substr(out, "Imported context from file 'secrets.md' (13 chars)."));
        expect(has_substr(out,
                          "Warning: This file may contain secrets (API keys, tokens, "
                          "credentials)."));
        expect(fx.app.session->history().back().role == kimix::string("user"));
        expect(has_substr(fx.app.session->history().back().content,
                          "<imported_context source=\"file 'secrets.md'\">"));
        // Session import: store a named copy first, then import it.
        kimix::string store_error;
        expect(fx.app.store.store_as("import_src", store_error)) << store_error;
        out = capture_handler(import, {"import", "import_src"}, fx.app);
        expect(has_substr(out, "Imported context from session 'import_src' ("));
        // The current session cannot import itself.
        out = capture_handler(import, {"import", fx.app.store.id()}, fx.app);
        expect(has_substr(out, "Cannot import the current session into itself."));
        fx.shutdown();
    };

    "refresh_env_reports"_test = [] {
        app_fixture fx;
        expect(fx.init("cli_slash_refenv"));
        const cli::command_entry *refresh = cli::find_command("refresh-env");
        expect(refresh != nullptr);
        const kimix::string out = capture_handler(refresh, {"refresh-env"}, fx.app);
#if defined(KIMIX_PLATFORM_WINDOWS)
        expect(has_substr(out, "PATH and PATHEXT have been refreshed from the registry."));
#else
        expect(has_substr(out, "This command is only available on Windows."));
#endif
        fx.shutdown();
    };

    "export_resolved_path_forms"_test = [] {
        app_fixture fx;
        expect(fx.init("cli_slash_export"));
        fx.backend.steps.push_back({"answer one", "", {}});
        output_capture capture;
        expect(capture.begin(cli::join_path(fx.work, "turn.txt")));
        cli::set_colorful(false);
        expect(cli::app_run_prompt(fx.app, "hello"));
        capture.end();
        const cli::command_entry *export_cmd = cli::find_command("export");
        expect(export_cmd != nullptr);

        // Colon form: the echo is the RESOLVED (absolute) path (I6).
        kimix::string out =
            capture_handler(export_cmd, {"export", "out.md"}, fx.app);
        const kimix::string expected =
            cli::absolute_path(cli::join_path(fx.work, "out.md"));
        expect(has_substr(out, "Exported 2 messages to " + expected));
        expect(cli::file_exists(expected));
        // The sensitive-information note (slash.py:343-347).
        expect(has_substr(out, "Note: The exported file may contain sensitive information."));

        // Whitespace form ("export out2.md" arrives as one token).
        out = capture_handler(export_cmd, {"export out2.md"}, fx.app);
        expect(has_substr(out, "Exported 2 messages to " +
                                  cli::absolute_path(cli::join_path(fx.work, "out2.md"))));

        // Directory form: trailing separator -> the default name inside it.
        const kimix::string dir = cli::join_path(fx.work, "exports");
        kimix::string mk_error;
        expect(cli::make_dirs(dir, mk_error));
        out = capture_handler(export_cmd, {"export", dir + "/"}, fx.app);
        expect(has_substr(out, "Exported 2 messages to "));
        expect(has_substr(out, "kimi-export-"));
        expect(has_substr(out, ".md"));
        fx.shutdown();
    };

    "export_failure_carries_error"_test = [] {
        app_fixture fx;
        expect(fx.init("cli_slash_export_fail"));
        const cli::command_entry *export_cmd = cli::find_command("export");
        // An empty history fails with the store's message (I8).
        kimix::string out =
            capture_handler(export_cmd, {"export", "nope.md"}, fx.app);
        expect(has_substr(out, "Export failed: "));
        expect(has_substr(out, "No messages to export."));
        fx.shutdown();
    };

    "sessions_cache_table"_test = [] {
        app_fixture fx;
        expect(fx.init("cli_slash_sessions"));
        fx.backend.steps.push_back({"an answer with words", "", {}});
        output_capture capture;
        expect(capture.begin(cli::join_path(fx.work, "turn.txt")));
        cli::set_colorful(false);
        expect(cli::app_run_prompt(fx.app, "hello there"));
        capture.end();
        const cli::command_entry *sessions = cli::find_command("sessions");
        expect(sessions != nullptr);
        // /sessions:<name> creates + switches; the cache gains the old row.
        kimix::string out =
            capture_handler(sessions, {"sessions", "named_one"}, fx.app);
        expect(has_substr(out, "Created and switched to session: named_one"));
        expect(eq(fx.app.cli_sessions.size(), size_t(2)));
        out = capture_handler(sessions, {"sessions"}, fx.app);
        // commands.py:394: f'{" ":1}  {"session id"...' -> three leading spaces.
        expect(has_substr(out, "   session id"));
        expect(has_substr(out, "*  "));
        // Both rows show a local-time stamp shaped like the reference's
        // pendulum "%Y-%m-%d %H:%M:%S".
        expect(has_substr(out, "named_one"));
        bool date_seen = false;
        for (const cli::cli_session_row &row : fx.app.cli_sessions) {
            const kimix::string stamp = cli::format_local(row.updated_at);
            date_seen = date_seen || (stamp.size() == 19 && stamp[4] == '-');
        }
        expect(date_seen) << "local-time updated_at rendering";
        fx.shutdown();
    };

    "store_release_and_recovery"_test = [] {
        app_fixture fx;
        expect(fx.init("cli_slash_store"));
        fx.backend.steps.push_back({"answer one with words", "", {}});
        output_capture capture;
        expect(capture.begin(cli::join_path(fx.work, "turn.txt")));
        cli::set_colorful(false);
        expect(cli::app_run_prompt(fx.app, "first"));
        capture.end();
        const kimix::string source_id = fx.app.store.id();
        const cli::command_entry *store = cli::find_command("store");
        expect(store != nullptr);
        // Failure (existing target) reports the error AND recovers the
        // original session (commands.py:172-200).
        kimix::string store_error;
        expect(fx.app.store.store_as("existing_copy", store_error));
        kimix::string out =
            capture_handler(store, {"store", "existing_copy"}, fx.app);
        expect(has_substr(out, "Store failed: "));
        expect(fx.app.session != nullptr) << "the original session was rebound";
        expect(fx.app.store.id() == source_id);
        // Success releases + re-opens the live session around the copy (I9).
        out = capture_handler(store, {"store", "saved_copy"}, fx.app);
        expect(has_substr(out, "Session stored as saved_copy"));
        expect(fx.app.store.id() == source_id) << "/store keeps the current session";
        expect(fx.app.session != nullptr && fx.app.soul != nullptr);
        fx.shutdown();
    };

    "exit_cleans_temp_folders"_test = [] {
        app_fixture fx;
        expect(fx.init("cli_slash_exit"));
        // A dead process's leftover tmp folder.
        const kimix::string stale =
            cli::join_path(cli::session_store::cache_root(fx.work), "tmp_999999");
        kimix::string error;
        expect(cli::make_dirs(stale, error));
        expect(cli::write_file(cli::join_path(stale, "0.txt"), "junk", error));
        const cli::command_entry *exit_cmd = cli::find_command("exit");
        expect(exit_cmd != nullptr);
        cli::command_result result{};
        const kimix::string exit_out = capture_handler(exit_cmd, {"exit"}, fx.app, &result);
        expect(result.should_break) << "/exit breaks the REPL";
        expect(has_substr(exit_out, "bye!"));
        expect(!cli::dir_exists(cli::cli_temp_dir(fx.work)))
            << "/exit removed the shared tmp_<pid> folder";
        expect(!cli::dir_exists(stale)) << "the dead-process leftover was swept";
        fx.shutdown();
    };

    "plan_fails_after_three_attempts"_test = [] {
        app_fixture fx;
        expect(fx.init("cli_slash_plan_fail"));
        fx.backend.steps.push_back({"no file written", "", {}});
        fx.backend.steps.push_back({"still nothing", "", {}});
        fx.backend.steps.push_back({"nothing again", "", {}});
        const cli::command_entry *plan = cli::find_command("plan");
        expect(plan != nullptr);
        cli::set_env("KIMIX_CLI_SKIP_OPEN", "1");
        kimix::vector<kimix::string> answers;
        answers.push_back("build the thing"); // the requirement
        answers.push_back("/end");
        fx.app.pending = &answers;
        fx.app.output = stdout; // capture the review prompts
        kimix::string out;
        {
            output_capture capture;
            expect(capture.begin(cli::join_path(fx.work, "plan.txt")));
            cli::set_colorful(false);
            kimix::vector<kimix::string> text_arr;
            const cli::command_result r =
                plan->handler({"plan", cli::join_path(fx.work, "plan.md")}, fx.app,
                              text_arr);
            (void)r;
            out = capture.end();
            cli::set_colorful(true);
        }
        fx.app.pending = nullptr;
        fx.app.output = nullptr;
        expect(eq(fx.backend.calls, 3)) << "three planner attempts";
        expect(has_substr(out, "Generating plan (attempt 1/3)..."));
        expect(has_substr(out, "Generating plan (attempt 2/3)..."));
        expect(has_substr(out, "Generating plan (attempt 3/3)..."));
        expect(has_substr(out, "Plan generation failed: plan file not found."));
        // (The planner session was isolated; just assert no implement turn ran.)
        expect(!has_substr(out, "Review this plan"));
        cli::set_env("KIMIX_CLI_SKIP_OPEN", "");
    };

    "plan_success_review_revision_and_quit"_test = [] {
        app_fixture fx;
        expect(fx.init("cli_slash_plan_rev"));
        fx.backend.on_call = [&fx](int32_t index) {
            // Every planner turn "writes" the plan file.
            kimix::string error;
            const kimix::string path = cli::join_path(fx.work, "plan_rev.md");
            if (index == 0) {
                expect(cli::write_file(path, "# Plan\n- step one", error));
            } else {
                expect(cli::write_file(path, "# Plan revised\n- step one", error));
            }
        };
        fx.backend.steps.push_back({"plan written", "", {}});
        fx.backend.steps.push_back({"plan revised", "", {}});
        const cli::command_entry *plan = cli::find_command("plan");
        cli::set_env("KIMIX_CLI_SKIP_OPEN", "1");
        // Review answers: "n" -> feedback -> "/quit" gives up (no implement).
        kimix::vector<kimix::string> answers;
        answers.push_back("build the thing");  // the requirement
        answers.push_back("/end");
          fx.app.pending = &answers;
          // The review answers come from a scripted stdin file so the review
          // prompts themselves print (the pending queue prints nothing).
          const kimix::string review_path = cli::join_path(fx.work, "review.txt");
          kimix::string review_error;
          expect(cli::write_file(review_path, "n\nmake it shorter\n/quit\n",
                                 review_error));
          fx.app.input = std::fopen(review_path.c_str(), "rb");
          fx.app.output = stdout; // capture the review prompts
        kimix::string out;
        {
            output_capture capture;
            expect(capture.begin(cli::join_path(fx.work, "plan.txt")));
            cli::set_colorful(false);
            kimix::vector<kimix::string> text_arr;
            plan->handler({"plan", cli::join_path(fx.work, "plan_rev.md")}, fx.app,
                          text_arr);
            out = capture.end();
            cli::set_colorful(true);
        }
        fx.app.pending = nullptr;
        fx.app.output = nullptr;
        expect(has_substr(out, "Plan generated: "));
        expect(has_substr(out, "Do you want to implement the plan? (y/n): "));
        expect(has_substr(out, "Please describe the changes you want (/quit to give up): "));
        expect(has_substr(out, "Revising plan..."));
        expect(eq(fx.backend.calls, 2)) << "generation + one revision, no implement";
        expect(!has_substr(out, "Review this plan"));
        cli::set_env("KIMIX_CLI_SKIP_OPEN", "");
        fx.shutdown();
    };

    "plan_success_implement_and_review_turns"_test = [] {
        app_fixture fx;
        expect(fx.init("cli_slash_plan_impl"));
        const kimix::string plan_path = cli::join_path(fx.work, "plan_impl.md");
        fx.backend.on_call = [&fx, plan_path](int32_t index) {
            kimix::string error;
            if (index == 0) {
                expect(cli::write_file(plan_path, "# Plan\n- build it", error));
            }
        };
        fx.backend.steps.push_back({"plan written", "", {}});
        fx.backend.steps.push_back({"implementing the plan", "", {}});
        fx.backend.steps.push_back({"reviewed, all tasks done", "", {}});
        const cli::command_entry *plan = cli::find_command("plan");
        cli::set_env("KIMIX_CLI_SKIP_OPEN", "1");
        kimix::vector<kimix::string> answers;
        answers.push_back("build the thing");  // the requirement
        answers.push_back("/end");
        answers.push_back("y");                 // review: implement
        fx.app.pending = &answers;
        fx.app.output = stdout; // capture the review prompts
        kimix::string out;
        {
            output_capture capture;
            expect(capture.begin(cli::join_path(fx.work, "plan.txt")));
            cli::set_colorful(false);
            kimix::vector<kimix::string> text_arr;
            plan->handler({"plan", plan_path}, fx.app, text_arr);
            out = capture.end();
            cli::set_colorful(true);
        }
        fx.app.pending = nullptr;
        fx.app.output = nullptr;
        expect(has_substr(out, "Plan generated: "));
        expect(eq(fx.backend.calls, 3))
            << "generation + implement + review turns";
        // The implement + review turns ran on a NEW anonymous session whose
        // history carries the plan body and the review reminder.
        const kimix::vector<kimix::llm::Message> &history = fx.app.session->history();
        bool saw_plan_prompt = false;
        bool saw_review_prompt = false;
        for (const kimix::llm::Message &m : history) {
            if (m.role == kimix::string("user")) {
                if (contains(m.content, "# Plan") &&
                    contains(m.content, "implement the plan step-by-step")) {
                    saw_plan_prompt = true;
                }
                if (contains(m.content, "Review this plan and ensure all tasks "
                                       "are completed")) {
                    saw_review_prompt = true;
                }
            }
        }
        expect(saw_plan_prompt) << "the <100 KB inline plan prompt";
        expect(saw_review_prompt) << "the review turn's reminder";
        cli::set_env("KIMIX_CLI_SKIP_OPEN", "");
        fx.shutdown();
    };

    "prompt_closing_loop_runs_two_rounds"_test = [] {
        app_fixture fx;
        expect(fx.init("cli_slash_closing2"));
        // Two closing rounds (the parsed knob drives the loop length).
        fx.app.provider.loop_control.cli_closing_reminder_rounds = 2;
        // Pre-seed a pending todo so the reminder is non-empty.
        kimix::builtin_tools::todo::todo_state &todos =
            kimix::builtin_tools::todo::session_todos(fx.app.session->tool_session());
        kimix::builtin_tools::todo::todo_item item;
        item.content = "write the report";
        item.status = kimix::builtin_tools::todo::todo_status::pending;
        todos.todos.push_back(item);

        fx.backend.steps.push_back({"working on it", "", {}});
        fx.backend.steps.push_back({"review pass one", "", {}});
        fx.backend.steps.push_back({"final review pass", "", {}});
        output_capture capture;
        expect(capture.begin(cli::join_path(fx.work, "closing.txt")));
        cli::set_colorful(false);
        expect(cli::app_run_prompt(fx.app, "do the task"));
        const kimix::string out = capture.end();
        cli::set_colorful(true);
        expect(has_substr(out, "Todo review..."));
        expect(has_substr(out, "Final todo review..."));
        // The reminder text reached the model: weak first, strong second.
        bool saw_weak = false;
        bool saw_strong = false;
        bool saw_original_request = false;
        for (const kimix::llm::Message &m : fx.app.session->history()) {
            if (m.role != kimix::string("user")) {
                continue;
            }
            if (contains(m.content, "You have unfinished todo items.")) {
                saw_weak = true;
            }
            if (contains(m.content, "CRITICAL: Unfinished todo items remain.")) {
                saw_strong = true;
            }
            if (contains(m.content, "Original request: do the task")) {
                saw_original_request = true;
            }
        }
        expect(saw_weak);
        expect(saw_strong);
        expect(saw_original_request);
        // I3: the finally block clears the session todos.
        expect(kimix::builtin_tools::todo::session_todos(fx.app.session->tool_session())
                   .todos.empty());
        fx.shutdown();
    };

    "prompt_over_64kb_goes_to_temp_file"_test = [] {
        app_fixture fx;
        expect(fx.init("cli_slash_bigprompt"));
        fx.backend.steps.push_back({"ok", "", {}});
        kimix::string big(70000, 'x');
        output_capture capture;
        expect(capture.begin(cli::join_path(fx.work, "big.txt")));
        cli::set_colorful(false);
        expect(cli::app_run_prompt(fx.app, big));
        capture.end();
        // The model saw the file pointer, not the 70 KB blob.
        const kimix::vector<kimix::llm::Message> &history = fx.app.session->history();
        expect(has_substr(history.front().content, "read and execute: `"));
        expect(fx.app.current_prompt == history.front().content)
            << "runtime.current_prompt tracks the transformed prompt";
        // The temp file lives in the shared tool temp folder and holds the body.
        const kimix::string start = history.front().content;
        const size_t a = find(start, "`") + 1;
        const size_t b = rfind(start, "`");
        const kimix::string temp_path = start.substr(a, b - a);
        expect(cli::file_exists(temp_path));
        expect(file_text(temp_path).size() == 70000);
        fx.shutdown();
    };

    "escape_file_paths_wraps_paths_not_urls"_test = [] {
        const kimix::string plain = cli::escape_file_paths("no separators here");
        expect(plain == kimix::string("no separators here"));
        // Documented reduction: the native escaper wraps space-free paths (the
        // reference's _PATH_RE also accepts single interior spaces).
        const kimix::string wrapped = cli::escape_file_paths("check D:/tmp/a.md now");
        expect(has_substr(wrapped, "`D:/tmp/a.md`"));
        const kimix::string url =
            cli::escape_file_paths("open https://example.com/x/y.html please");
        expect(!has_substr(url, "`https://"));
        const kimix::string quoted = cli::escape_file_paths("see `C:/a/b.txt` ok");
        expect(quoted == kimix::string("see `C:/a/b.txt` ok"))
            << "already-backticked paths stay untouched";
    };

    "todo_failure_carries_error"_test = [] {
        app_fixture fx;
        expect(fx.init("cli_slash_fail"));
        // A TODO-bearing source file for /todo to scan.
        const kimix::string source = cli::join_path(fx.work, "code.py");
        kimix::string error;
        expect(cli::write_file(source, "def f():\n    pass  # TODO fix me\n", error));
        // The turn fails with a provider error.
        fx.backend.fail_error = "boom: 500 from provider";
        const cli::command_entry *todo = cli::find_command("todo");
        expect(todo != nullptr);
        output_capture capture;
        expect(capture.begin(cli::join_path(fx.work, "todo.txt")));
        cli::set_colorful(false);
        kimix::vector<kimix::string> text_arr;
        todo->handler({"todo", source}, fx.app, text_arr);
        const kimix::string out = capture.end();
        cli::set_colorful(true);
        expect(has_substr(out, "Prompt failed:"))
            << "I8: the failure string carries the error text";
        expect(has_substr(out, "boom"));
        fx.shutdown();
    };

    "reflection_failure_carries_error"_test = [] {
        app_fixture fx;
        expect(fx.init("cli_slash_refl_fail"));
        fx.backend.steps.push_back({"an answer", "", {}});
        expect(cli::app_run_prompt(fx.app, "hello"));
        fx.backend.fail_error = "kaboom: rate limited";
        const cli::command_entry *reflection = cli::find_command("reflection");
        output_capture capture;
        expect(capture.begin(cli::join_path(fx.work, "refl.txt")));
        cli::set_colorful(false);
        kimix::vector<kimix::string> text_arr;
        reflection->handler({"reflection"}, fx.app, text_arr);
        const kimix::string out = capture.end();
        cli::set_colorful(true);
        expect(has_substr(out, "Reflection failed:"));
        expect(has_substr(out, "kaboom"));
        fx.shutdown();
    };
}
