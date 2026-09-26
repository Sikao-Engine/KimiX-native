// test_context_db.cpp - unit tests for the SQLite context store
// (src/agent/context_db.{h,cpp}), the C++ port of kimi_cli/soul/context_db.py.
//
// Covers: backend auto-detection helpers, schema creation (+ WAL file), the
// JSONL record byte shape, rowid pagination order, migration from a fixture
// JSONL (including lenient repair of a corrupt line), meta tables (system
// prompt / usage / checkpoints + revert), cross-reopen persistence and a
// basic two-connection concurrency sanity check. Every test works inside its
// own directory under the system temp dir.

#include "ut/ut.hpp"
#include <cstdio>
#include <system_error>
#include <thread>
#include "agent/context_db.h"
#include "core/stl/filesystem.h"
#include "core/stl/format.h"
#include "sqlite3.h"

using namespace boost::ut;
using kimix::agent::ContextBackend;
using kimix::agent::detect_context_backend;
using kimix::agent::needs_context_migration;
using kimix::agent::resolve_context_db_path;

namespace {

kimix::filesystem::path test_dir(const char *name) {
    std::error_code ec;
    kimix::filesystem::path dir = kimix::filesystem::temp_directory_path(ec) /
                                  "kimix_context_db_test" / name;
    kimix::filesystem::remove_all(dir, ec);
    kimix::filesystem::create_directories(dir, ec);
    return dir;
}

// One raw SQL helper so the schema assertions do not depend on the class
// under test.
bool raw_exec(const kimix::filesystem::path &db, const char *sql) {
    sqlite3 *conn = nullptr;
    if (sqlite3_open_v2(db.string().c_str(), &conn,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
        SQLITE_OK) {
        if (conn != nullptr) {
            sqlite3_close(conn);
        }
        return false;
    }
    char *errmsg = nullptr;
    const int rc = sqlite3_exec(conn, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        sqlite3_free(errmsg);
        sqlite3_close(conn);
        return false;
    }
    sqlite3_close(conn);
    return true;
}

int64_t raw_query_int(const kimix::filesystem::path &db, const char *sql,
                      bool &ok) {
    sqlite3 *conn = nullptr;
    ok = sqlite3_open_v2(db.string().c_str(), &conn, SQLITE_OPEN_READWRITE,
                         nullptr) == SQLITE_OK;
    sqlite3_stmt *stmt = nullptr;
    int64_t out = 0;
    if (ok) {
        ok = sqlite3_prepare_v2(conn, sql, -1, &stmt, nullptr) == SQLITE_OK;
    }
    if (ok) {
        ok = sqlite3_step(stmt) == SQLITE_ROW;
    }
    if (ok) {
        out = sqlite3_column_int64(stmt, 0);
    }
    if (stmt != nullptr) {
        sqlite3_finalize(stmt);
    }
    if (conn != nullptr) {
        sqlite3_close(conn);
    }
    return out;
}

bool write_text(const kimix::filesystem::path &p, kimix::string_view text) {
    FILE *f = std::fopen(p.string().c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    const size_t n = fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return n == text.size();
}

kimix::llm::Message user_msg(kimix::string_view content) {
    kimix::llm::Message m;
    m.role = "user";
    m.content = kimix::string(content);
    return m;
}

} // namespace

int main() {
    using namespace boost::ut::literals;

    // =======================================================================
    "backend_detection"_test = [] {
        expect(detect_context_backend("context.db") == ContextBackend::sqlite);
        expect(detect_context_backend("context.jsonl") == ContextBackend::jsonl);
        expect(detect_context_backend("context.db-wal") == ContextBackend::jsonl);
        expect(eq(resolve_context_db_path("sessions/s1/context.jsonl"),
                  kimix::string("sessions/s1/context.db")));
        expect(eq(resolve_context_db_path("sessions/s1/context.db"),
                  kimix::string("sessions/s1/context.db")));

        const kimix::filesystem::path dir = test_dir("backend");
        const kimix::filesystem::path jsonl = dir / "context.jsonl";
        expect(!needs_context_migration(jsonl.string()));// neither side exists yet
        expect(write_text(jsonl, "{\"role\":\"user\",\"content\":\"hi\"}\n"));
        expect(needs_context_migration(jsonl.string()));// jsonl yes, db no
        expect(raw_exec(dir / "context.db", "CREATE TABLE t(x)"));
        expect(!needs_context_migration(jsonl.string()));// db now exists
    };

    // =======================================================================
    "schema_and_wal"_test = [] {
        const kimix::filesystem::path dir = test_dir("schema");
        const kimix::filesystem::path db = dir / "context.db";
        kimix::string error;
        kimix::agent::ContextDb store(db);
        expect(store.open(error)) << error;
        expect(store.is_open());
        expect(error.empty());

        // WAL mode actually engaged: the -wal sidecar exists while the
        // connection is open and has written.
        kimix::agent::ContextRecord rec;
        rec.role = "user";
        rec.content = R"({"role":"user","content":"hi"})";
        expect(store.append(rec, error)) << error;
        expect(kimix::filesystem::exists(db.string() + "-wal"))
            << "WAL sidecar missing";

        // The 5 reference tables + 2 indexes exist (raw connection, so this
        // does not trust the class under test).
        expect(raw_exec(db,
                        "SELECT rowid, role, content, content_text, created_at "
                        "FROM messages ORDER BY rowid"));
        expect(raw_exec(db, "SELECT id, content, updated_at FROM system_prompt"));
        expect(raw_exec(db, "SELECT id, message_rowid, created_at FROM checkpoints"));
        expect(raw_exec(db, "SELECT rowid, token_count, created_at FROM "
                            "usage_snapshots"));
        expect(raw_exec(db, "SELECT key, value FROM meta"));
        expect(raw_exec(db, "SELECT role FROM messages INDEXED BY idx_messages_role "
                            "WHERE role = 'user'"));
        expect(raw_exec(db, "SELECT created_at FROM messages INDEXED BY "
                            "idx_messages_created_at LIMIT 1"));
        store.close();
        expect(!store.is_open());
        // close() is idempotent.
        store.close();
    };

    // =======================================================================
    "record_byte_shape"_test = [] {
        // The exact literals tests/unit/cli/test_cli.cpp asserts for the
        // native context.jsonl writer (session_history_roundtrip): the DB
        // record bytes must be identical.
        expect(eq(kimix::agent::context_record_from_message(user_msg("hello")),
                  kimix::string(R"({"role":"user","content":"hello"})")));

        kimix::llm::Message assistant;
        assistant.role = "assistant";
        assistant.thinking = "I should run ls";
        assistant.thinking_signature = "sig-1";
        assistant.content = "Running it.";
        kimix::llm::ToolCall call;
        call.id = "call_1";
        call.name = "bash";
        call.arguments = R"({"command":"ls -la"})";
        assistant.tool_calls.push_back(call);
        expect(eq(kimix::agent::context_record_from_message(assistant),
                  kimix::string(R"({"role":"assistant","content":[{"type":"think","think":"I should run ls","encrypted":"sig-1"},{"type":"text","text":"Running it."}],"tool_calls":[{"type":"function","id":"call_1","function":{"name":"bash","arguments":"{\"command\":\"ls -la\"}"}}]})")));

        kimix::llm::Message tool;
        tool.role = "tool";
        tool.content = "total 0";
        tool.tool_call_id = "call_1";
        expect(eq(kimix::agent::context_record_from_message(tool),
                  kimix::string(R"({"role":"tool","content":"total 0","tool_call_id":"call_1"})")));

        kimix::llm::Message empty;
        empty.role = "user";
        expect(eq(kimix::agent::context_record_from_message(empty),
                  kimix::string(R"({"role":"user","content":""})")));

        // Message round-trip through the record bytes.
        kimix::llm::Message parsed;
        expect(kimix::agent::context_message_from_record(
            kimix::agent::context_record_from_message(assistant), parsed));
        expect(eq(parsed.role, assistant.role));
        expect(eq(parsed.thinking, assistant.thinking));
        expect(eq(parsed.thinking_signature, assistant.thinking_signature));
        expect(eq(parsed.content, assistant.content));
        expect(parsed.tool_calls.size() == 1u);
        expect(eq(parsed.tool_calls[0].name, kimix::string("bash")));
        expect(eq(parsed.tool_calls[0].arguments, call.arguments));
        // Meta roles are not messages.
        expect(!kimix::agent::context_message_from_record(
            R"({"role":"_usage","token_count":3})", parsed));
        expect(!kimix::agent::context_message_from_record("not json", parsed));
    };

    // =======================================================================
    "append_read_pagination"_test = [] {
        const kimix::filesystem::path dir = test_dir("pagination");
        const kimix::filesystem::path db = dir / "context.db";
        kimix::string error;
        kimix::agent::ContextDb store(db);
        expect(store.open(error)) << error;
        for (int i = 0; i < 10; ++i) {
            expect(store.append_message(user_msg(kimix::format("m{}", i)), error))
                << error;
        }
        int64_t count = 0;
        expect(store.message_count(count, error)) << error;
        expect(count == 10_i);

        // Rowid pagination: strictly increasing rowids, same page semantics as
        // the reference (after_rowid exclusive, limit caps the page).
        kimix::vector<kimix::agent::ContextDb::MessageRow> rows;
        expect(store.read_after(0, 4, rows, error)) << error;
        expect(rows.size() == 4u);
        expect(eq(rows[0].content, kimix::string(R"({"role":"user","content":"m0"})")));
        expect(eq(rows[3].content, kimix::string(R"({"role":"user","content":"m3"})")));
        const int64_t page_last = rows.back().rowid;
        expect(store.read_after(page_last, 4, rows, error)) << error;
        expect(rows.size() == 4u);
        expect(eq(rows[0].content, kimix::string(R"({"role":"user","content":"m4"})")));
        expect(store.read_after(page_last, -1, rows, error)) << error;
        expect(rows.size() == 6u);
        expect(eq(rows.back().content, kimix::string(R"({"role":"user","content":"m9"})")));

        // Read-all equals paged concatenation and preserves order.
        kimix::vector<kimix::agent::ContextDb::MessageRow> all;
        expect(store.read_all(all, error)) << error;
        expect(all.size() == 10u);
        for (size_t i = 1; i < all.size(); ++i) {
            expect(all[i].rowid > all[i - 1].rowid);
        }
        int64_t last = 0;
        expect(store.last_message_rowid(last, error)) << error;
        expect(last == all.back().rowid);
        bool visible = false;
        expect(store.has_visible_messages(visible, error)) << error;
        expect(visible);

        // Delete-range keeps rowid <= boundary.
        expect(store.delete_after(all[4].rowid, error)) << error;
        expect(store.message_count(count, error)) << error;
        expect(count == 5_i);
        expect(store.read_all(all, error)) << error;
        expect(eq(all.back().content, kimix::string(R"({"role":"user","content":"m4"})")));

        // Clear empties every table.
        expect(store.clear(error)) << error;
        expect(store.message_count(count, error)) << error;
        expect(count == 0_i);
        int64_t usage = 0;
        bool found = false;
        expect(store.record_usage(42, error)) << error;
        expect(store.clear(error)) << error;
        expect(store.latest_usage(usage, found, error)) << error;
        expect(!found);
    };

    // =======================================================================
    "meta_tables"_test = [] {
        const kimix::filesystem::path dir = test_dir("meta");
        const kimix::filesystem::path db = dir / "context.db";
        kimix::string error;
        kimix::agent::ContextDb store(db);
        expect(store.open(error)) << error;

        // System prompt singleton (id=1, last write wins).
        expect(store.set_system_prompt("prompt v1", error)) << error;
        expect(store.set_system_prompt("prompt v2", error)) << error;
        kimix::string prompt;
        bool found = false;
        expect(store.get_system_prompt(prompt, found, error)) << error;
        expect(found);
        expect(eq(prompt, kimix::string("prompt v2")));

        // Usage snapshots: append-only history, latest wins.
        expect(store.record_usage(100, error)) << error;
        expect(store.record_usage(200, error)) << error;
        int64_t usage = 0;
        expect(store.latest_usage(usage, found, error)) << error;
        expect(found);
        expect(usage == 200_i);

        // Checkpoints anchor at the current max message rowid; reverting
        // deletes everything after the checkpoint's boundary.
        expect(store.append_message(user_msg("one"), error)) << error;
        expect(store.append_message(user_msg("two"), error)) << error;
        int64_t rowid = 0;
        expect(store.create_checkpoint(0, rowid, error)) << error;
        expect(rowid == 2_i);
        expect(store.append_message(user_msg("three"), error)) << error;
        expect(store.record_usage(300, error)) << error;
        int64_t latest_cp = -1;
        expect(store.latest_checkpoint_id(latest_cp, error)) << error;
        expect(latest_cp == 0_i);
        expect(store.revert_to_checkpoint(0, error)) << error;
        int64_t count = 0;
        expect(store.message_count(count, error)) << error;
        expect(count == 2_i);
        expect(store.latest_checkpoint_id(latest_cp, error)) << error;
        expect(latest_cp == -1_i);// checkpoint row itself is gone, like the reference
        expect(store.latest_usage(usage, found, error)) << error;
        expect(usage == 200_i);// snapshots after the boundary were dropped

        // Unknown checkpoint is an error, not a crash.
        expect(!store.revert_to_checkpoint(99, error));
        expect(!error.empty());
    };

    // =======================================================================
    "migration_from_jsonl"_test = [] {
        const kimix::filesystem::path dir = test_dir("migration");
        const kimix::filesystem::path jsonl = dir / "context.jsonl";
        const kimix::filesystem::path db = dir / "context.db";
        // Fixture: system prompt + usage + checkpoint + visible messages, a
        // line that only parses after repair (single quotes + trailing comma),
        // and a line that never parses at all. Matches the record shapes the
        // Python reference writes.
        const char *fixture =
            "{\"role\":\"_system_prompt\",\"content\":\"the prompt\"}\n"
            "{\"role\":\"user\",\"content\":\"first\"}\n"
            "{\"role\":\"_usage\",\"token_count\":1234}\n"
            "{\"role\":\"user\",\"content\":\"second\"}\n"
            "{\"role\":\"_checkpoint\",\"id\":0}\n"
            "{'role':'user','content':'repaired',}\n"
            "this line is not json at all\n"
            "{\"role\":\"assistant\",\"content\":\"answer\"}\n";
        expect(write_text(jsonl, fixture));

        kimix::string error;
        kimix::agent::ContextDb store(db);
        bool migrated = false;
        expect(store.migrate_jsonl(jsonl, migrated, error)) << error;
        expect(migrated);

        // The JSONL moved aside (session.py's .jsonl.bak rename).
        expect(!kimix::filesystem::exists(jsonl));
        expect(kimix::filesystem::exists(dir / "context.jsonl.bak"));

        int64_t count = 0;
        expect(store.message_count(count, error)) << error;
        expect(count == 4_i);// 5 good lines - 1 meta - ... = 4 messages
        kimix::vector<kimix::agent::ContextDb::MessageRow> rows;
        expect(store.read_all(rows, error)) << error;
        expect(eq(rows[0].content, kimix::string(R"({"role":"user","content":"first"})")));
        expect(eq(rows[1].content, kimix::string(R"({"role":"user","content":"second"})")));
        // The repaired line was normalized to canonical JSON, like orjson.dumps.
        expect(eq(rows[2].content, kimix::string(R"({"role":"user","content":"repaired"})")));
        expect(eq(rows[3].content, kimix::string(R"({"role":"assistant","content":"answer"})")));
        expect(rows[0].rowid < rows[1].rowid && rows[1].rowid < rows[2].rowid);

        // Meta records landed in their own tables.
        kimix::string prompt;
        bool found = false;
        expect(store.get_system_prompt(prompt, found, error)) << error;
        expect(found);
        expect(eq(prompt, kimix::string("the prompt")));
        int64_t usage = 0;
        expect(store.latest_usage(usage, found, error)) << error;
        expect(found);
        expect(usage == 1234_i);
        int64_t cp_rowid = 0;
        expect(store.checkpoint_message_rowid(0, cp_rowid, found, error)) << error;
        expect(found);
        expect(cp_rowid == rows[1].rowid);// anchored at the last message before it

        // content_text was extracted for the search column.
        bool ok = false;
        expect(raw_query_int(db,
                             "SELECT COUNT(*) FROM messages WHERE content_text "
                             "LIKE '%repaired%'",
                             ok) == 1 && ok);

        // Idempotent: nothing left to migrate.
        expect(store.migrate_jsonl(jsonl, migrated, error)) << error;
        expect(!migrated);
        expect(store.migrate_jsonl(dir / "missing.jsonl", migrated, error)) << error;
        expect(!migrated);
    };

    // =======================================================================
    "cross_reopen_persistence"_test = [] {
        const kimix::filesystem::path dir = test_dir("reopen");
        const kimix::filesystem::path db = dir / "context.db";
        kimix::string error;
        {
            kimix::agent::ContextDb store(db);
            expect(store.open(error)) << error;
            expect(store.append_message(user_msg("persist me"), error)) << error;
            expect(store.set_system_prompt("p", error)) << error;
            expect(store.record_usage(7, error)) << error;
            store.close();
        }
        {
            kimix::agent::ContextDb store(db);
            expect(store.open(error)) << error;
            kimix::vector<kimix::agent::ContextDb::MessageRow> rows;
            expect(store.read_all(rows, error)) << error;
            expect(rows.size() == 1u);
            expect(eq(rows[0].content, kimix::string(R"({"role":"user","content":"persist me"})")));
            kimix::string prompt;
            bool found = false;
            expect(store.get_system_prompt(prompt, found, error)) << error;
            expect(found && prompt == "p");
            int64_t usage = 0;
            expect(store.latest_usage(usage, found, error)) << error;
            expect(found && usage == 7_i);
            // Rowids survive the reopen (AUTOINCREMENT continuity).
            expect(store.append_message(user_msg("next"), error)) << error;
            expect(store.read_all(rows, error)) << error;
            expect(rows[1].rowid > rows[0].rowid);
        }
    };

    // =======================================================================
    "concurrent_connections"_test = [] {
        const kimix::filesystem::path dir = test_dir("concurrent");
        const kimix::filesystem::path db = dir / "context.db";
        kimix::string error;
        kimix::agent::ContextDb a(db);
        kimix::agent::ContextDb b(db);
        expect(a.open(error)) << error;
        expect(b.open(error)) << error;

        constexpr int kThreads = 4;
        constexpr int kPerThread = 25;
        kimix::vector<std::thread> threads;
        bool failures[kThreads] = {};
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < kPerThread; ++i) {
                    kimix::agent::ContextDb &self = (t % 2 == 0) ? a : b;
                    if (!self.append_message(
                            user_msg(kimix::format("t{}i{}", t, i)), error)) {
                        failures[t] = true;
                        return;
                    }
                }
            });
        }
        for (auto &th : threads) {
            th.join();
        }
        for (bool failed : failures) {
            expect(!failed) << error;
        }
        int64_t count = 0;
        expect(a.message_count(count, error)) << error;
        expect(count == kThreads * kPerThread);
    };
}
