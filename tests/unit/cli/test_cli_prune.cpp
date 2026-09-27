// test_cli_prune.cpp - CLI-level tests for /prune (report.md row D12) and the
// B4/B6 session-store wiring (context.db read/write + JSONL->DB migration).
//
// /prune: the command-table entry runs the soul's pruner over a history copy
// and prints the reference's "Context pruned: freed N tokens, earliest change
// at index I." line (or "No prunable content found." / the cooldown no-op).
// B4/B6: a session directory whose context.db exists opens through ContextDb
// (".db first, then .jsonl"); load_history migrates a legacy JSONL on access
// (renaming it to .jsonl.bak) and save_history writes through the store.
//
// Framework: Boost.UT (tests/ut/ut.hpp).
#include "ut/ut.hpp"

#include <cstdio>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <core/kimix_core.h>

#include <agent/context_db.h>
#include <agent/soul.h>
#include <cli/cli_app.h>
#include <cli/cli_commands.h>
#include <cli/cli_common.h>
#include <cli/cli_print.h>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace cli = kimix::cli;

namespace {

kimix::llm::Message make_msg(kimix::string_view role, kimix::string_view content) {
    kimix::llm::Message m;
    m.role.assign(role.data(), role.size());
    m.content.assign(content.data(), content.size());
    return m;
}

kimix::string filler(size_t n) { return kimix::string(n, 'x'); }

// ── /prune fixture ───────────────────────────────────────────────────────────

class prune_backend : public kimix::agent::IChatBackend {
public:
    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &,
         const kimix::vector<kimix::llm::Tool> &,
         const kimix::llm::ChunkCallback &,
         const kimix::llm::AbortCheck * /*abort*/) override {
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content = "done";
        return r;
    }
    int64_t max_context_size() const override { return 1000; }
    kimix::string model_name() const override { return "prune-test"; }
};

// An app_context with a live soul over a scripted backend (no network).
struct prune_fixture {
    kimix::string work;
    cli::app_context app;
    prune_backend backend;

    bool init(kimix::string_view name, bool pruning_enabled) {
        std::error_code ec;
        const kimix::filesystem::path base =
            kimix::filesystem::temp_directory_path(ec) / "kimix_prune_test";
        kimix::filesystem::create_directories(base, ec);
        work = kimix::to_string(base / kimix::string(name));
        kimix::string mkdir_error;
        cli::make_dirs(work, mkdir_error);
        // Pin the print layer to plain mode BEFORE any stdout redirection:
        // colorful() lazily probes stream_is_console(stdout), whose isatty
        // assert would fire on the capture file's descriptor.
        cli::init_printing(/*no_color=*/true);
        app.work_dir = work;
        app.injected = &backend;
        app.session.reset(new kimix::agent::AgentSession(work));
        kimix::agent::KimiSoul::options opts;
        opts.loop_control.context_pruning_enabled = pruning_enabled;
        opts.max_steps = 4;
        app.soul.reset(new kimix::agent::KimiSoul(*app.session, *app.injected, opts));
        return true;
    }
    // Runs a slash handler with process stdout captured: the cli_print layer
    // (print_string et al.) writes to stdout directly, not the stream
    // renderer. The fd behind stdout is swapped with _dup2 (freopen fights the
    // debug CRT when stdout started as a pipe), then restored.
    kimix::string run_command(const cli::command_entry *entry) {
        const kimix::string capture_path = work + "/stdout_capture.txt";
        std::fflush(stdout);
        const int stdout_fd =
#if defined(_WIN32)
            _fileno(stdout);
#else
            ::fileno(stdout);
#endif
        const int saved_fd =
#if defined(_WIN32)
            _dup(stdout_fd);
#else
            ::dup(stdout_fd);
#endif
        if (saved_fd < 0) {
            return kimix::string();
        }
        std::FILE *capture = std::fopen(capture_path.c_str(), "w");
        if (capture == nullptr) {
            return kimix::string();
        }
#if defined(_WIN32)
        _dup2(_fileno(capture), stdout_fd);
#else
        ::dup2(::fileno(capture), stdout_fd);
#endif
        const kimix::vector<kimix::string> args;
        kimix::vector<kimix::string> text_arr;
        entry->handler(args, app, text_arr);
        std::fflush(stdout);
#if defined(_WIN32)
        _dup2(saved_fd, stdout_fd);
        _close(saved_fd);
#else
        ::dup2(saved_fd, stdout_fd);
        ::close(saved_fd);
#endif
        std::fclose(capture);
        kimix::string out;
        kimix::string read_error;
        cli::read_file(capture_path, out, read_error);
        return out;
    }
};

// 4 head fillers + one oversized tool result at index 4 + 6 tail fillers:
// the only prunable slot under the reference default protected set.
void build_history(kimix::agent::AgentSession &session) {
    for (int i = 0; i < 4; ++i) {
        session.append_history(make_msg("user", filler(64)));
    }
    kimix::llm::Message tool = make_msg("tool", filler(9000)); // ~2250 tokens freed
    tool.tool_call_id = "c1";
    session.append_history(tool);
    for (int i = 0; i < 6; ++i) {
        session.append_history(make_msg(i % 2 ? "assistant" : "user", filler(64)));
    }
}

// ── B4/B6 helpers ────────────────────────────────────────────────────────────

kimix::string ws_dir(kimix::string_view name) {
    std::error_code ec;
    const kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) / "kimix_ctxdb_test";
    kimix::filesystem::create_directories(base, ec);
    const kimix::string dir = kimix::to_string(base / kimix::string(name));
    kimix::filesystem::remove_all(dir, ec);
    return dir;
}

// Populate <dir>/context.db the way a Python session would: one ContextDb
// record per message.
bool seed_context_db(const kimix::string &dir,
                     const kimix::vector<kimix::llm::Message> &history,
                     kimix::string &error) {
    kimix::agent::ContextDb db{kimix::filesystem::path(
        cli::join_path(dir, "context.db"))};
    if (!db.open(error)) {
        return false;
    }
    kimix::vector<kimix::agent::ContextRecord> records;
    for (const kimix::llm::Message &m : history) {
        kimix::agent::ContextRecord rec;
        rec.role = m.role;
        rec.content = kimix::agent::context_record_from_message(m);
        records.push_back(std::move(rec));
    }
    if (!db.append_batch(kimix::span<const kimix::agent::ContextRecord>(
                             records.data(), static_cast<int64_t>(records.size())),
                         error)) {
        db.close();
        return false;
    }
    db.close();
    return true;
}

} // namespace

int main() {
    // ── D12: /prune ──────────────────────────────────────────────────────────
    "prune_command_runs_and_reports"_test = [] {
        prune_fixture fx;
        expect(fx.init("prune_ok", true));
        build_history(*fx.app.session);
        const cli::command_entry *entry = cli::find_command("prune");
        expect(entry != nullptr);
        const kimix::string out = fx.run_command(entry);
        expect(out.find("Context pruned: freed ") == 0u) << out;
        expect(out.find("tokens, earliest change at index 4.") != kimix::string::npos)
            << out;
        // Non-destructive: the stored history keeps the original content.
        expect(!kimix::agent::is_pruned_stub(fx.app.session->history()[4]));
        // The cooldown blocks an immediate second pass.
        expect(fx.run_command(entry).find("No prunable content found.") !=
               kimix::string::npos);
    };

    "prune_command_disabled_in_config"_test = [] {
        prune_fixture fx;
        expect(fx.init("prune_off", false));
        build_history(*fx.app.session);
        const kimix::string out = fx.run_command(cli::find_command("prune"));
        expect(out.find("Context pruning is disabled in config.") == 0u) << out;
    };

    "prune_command_registered_in_help"_test = [] {
        expect(cli::find_command("prune") != nullptr);
        bool listed = false;
        for (const cli::command_entry &e : cli::command_map()) {
            if (e.name == "prune") {
                listed = true;
            }
        }
        expect(listed);
    };

    // ── B4/B6: the context.db session backend ────────────────────────────────
    "db_session_roundtrip_load_and_save"_test = [] {
        kimix::string error;
        const kimix::string ws = ws_dir("db_roundtrip");
        cli::session_store store;
        expect(store.open(ws, "db-session", false, error)) << error;
        kimix::vector<kimix::llm::Message> history;
        history.push_back(make_msg("user", "hello db"));
        history.push_back(make_msg("assistant", "hi there"));
        // Seed a Python-style context.db (no context.jsonl at all).
        expect(seed_context_db(store.dir(), history, error)) << error;
        expect(!cli::file_exists(cli::join_path(store.dir(), "context.jsonl")));

        kimix::vector<kimix::llm::Message> back;
        expect(store.load_history(back, error)) << error;
        expect(back.size() == 2u);
        expect(back[0].role == "user");
        expect(back[0].content == "hello db");
        expect(back[1].content == "hi there");

        // The save path writes through the store; the JSONL stays absent.
        back.push_back(make_msg("user", "third message"));
        expect(store.save_history(back, error)) << error;
        expect(!cli::file_exists(cli::join_path(store.dir(), "context.jsonl")));
        kimix::vector<kimix::llm::Message> reread;
        expect(store.load_history(reread, error)) << error;
        expect(reread.size() == 3u);
        expect(reread[2].content == "third message");
        expect(store.has_context_records());
    };

    "jsonl_session_migrates_to_db_on_load"_test = [] {
        kimix::string error;
        const kimix::string ws = ws_dir("db_migration");
        cli::session_store store;
        expect(store.open(ws, "migrate-session", false, error)) << error;
        kimix::vector<kimix::llm::Message> history;
        history.push_back(make_msg("user", "legacy jsonl content"));
        history.push_back(make_msg("assistant", "and the answer"));
        expect(store.save_history(history, error)) << error;
        expect(cli::file_exists(cli::join_path(store.dir(), "context.jsonl")));

        // session.py's auto-migration on access: the JSONL is imported into
        // context.db and renamed to .jsonl.bak.
        kimix::vector<kimix::llm::Message> back;
        expect(store.load_history(back, error)) << error;
        expect(back.size() == 2u);
        expect(back[0].content == "legacy jsonl content");
        expect(cli::file_exists(cli::join_path(store.dir(), "context.db")));
        expect(cli::file_exists(cli::join_path(store.dir(), "context.jsonl.bak")));
        expect(!cli::file_exists(cli::join_path(store.dir(), "context.jsonl")));

        // Subsequent saves write through the DB.
        back.push_back(make_msg("user", "after migration"));
        expect(store.save_history(back, error)) << error;
        expect(!cli::file_exists(cli::join_path(store.dir(), "context.jsonl")));
        kimix::vector<kimix::llm::Message> reread;
        expect(store.load_history(reread, error)) << error;
        expect(reread.size() == 3u);
        expect(reread[2].content == "after migration");
    };

    "corrupt_context_db_reports_an_error"_test = [] {
        kimix::string error;
        const kimix::string ws = ws_dir("db_corrupt");
        cli::session_store store;
        expect(store.open(ws, "corrupt-session", false, error)) << error;
        expect(cli::write_file(cli::join_path(store.dir(), "context.db"), "not sqlite",
                               error))
            << error;
        kimix::vector<kimix::llm::Message> history;
        expect(!store.load_history(history, error));
        expect(history.empty());
        expect(error.find("context.db") != kimix::string::npos) << error;
        expect(error.find("SQLite") != kimix::string::npos) << error;
    };

    return 0;
}
