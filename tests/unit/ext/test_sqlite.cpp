// test_sqlite.cpp - amalgamation sanity checks for the vendored SQLite
// (src/ext/sqlite_amalgamation, built as the static library kimix-sqlite3).
//
// The single SQLite input of this repository is a checked-in pre-generated
// amalgamation, refreshed by scripts/fetch_sqlite_amalgamation.py. These tests
// are the cheap early warning for a bad bump:
// - the compiled library and the header agree on the version
//   (sqlite3_libversion() == SQLITE_VERSION), catching a half-updated pair;
// - FTS5 is compiled in, which src/runtime/index/sqlite_history_index.cpp and
//   src/agent/context_db.cpp depend on (sqlite_xmake.lua sets
//   -DSQLITE_ENABLE_FTS5);
// - an FTS5 virtual table round-trips a query, so the amalgamation is usable,
//   not merely present.
//
// All test logic lives in main() scope; no file-scope static registrations.

#include "ut/ut.hpp"

#include "sqlite3.h"

#include <cstdio>
#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

// Thin helper: run one statement, report the failure on stderr (this project
// has no logger) and return false so the call site's expect() reports it.
bool exec_sql(sqlite3 *db, const char *sql) {
    char *errmsg = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "sqlite3_exec failed (%d): %s\n", rc,
                     errmsg != nullptr ? errmsg : sqlite3_errmsg(db));
        sqlite3_free(errmsg);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    // A bump that replaces only one of sqlite3.c / sqlite3.h is caught here.
    "version_header_matches_library"_test = [] {
        expect(std::strcmp(sqlite3_libversion(), SQLITE_VERSION) == 0)
            << "sqlite3.c and sqlite3.h disagree: library "
            << sqlite3_libversion() << ", header " << SQLITE_VERSION;
    };

    // The history index (sqlite_history_index) and the agent context store
    // (context_db) need FTS5; sqlite_xmake.lua enables it with that define.
    "fts5_compiled_in"_test = [] {
        expect(sqlite3_compileoption_used("ENABLE_FTS5") == 1)
            << "SQLite built without FTS5 (SQLITE_ENABLE_FTS5 missing)";
    };

    // Usable FTS5: create an in-memory index, insert, MATCH, read back, close.
    "fts5_roundtrip"_test = [] {
        sqlite3 *db = nullptr;
        const int rc = sqlite3_open(":memory:", &db);
        expect(rc == SQLITE_OK) << "sqlite3_open: " << sqlite3_errmsg(db);
        if (rc != SQLITE_OK) {
            if (db != nullptr) {
                sqlite3_close(db);
            }
            return;
        }

        expect(exec_sql(db, "CREATE VIRTUAL TABLE ft USING fts5(body)"))
            << "FTS5 virtual table creation failed";

        expect(exec_sql(db,
                        "INSERT INTO ft(body) VALUES ('hello sqlite fts5 world')"))
            << "insert into the FTS5 table failed: " << sqlite3_errmsg(db);

        sqlite3_stmt *stmt = nullptr;
        const int prep_rc = sqlite3_prepare_v2(
            db, "SELECT body FROM ft WHERE ft MATCH 'fts5'", -1, &stmt, nullptr);
        expect(prep_rc == SQLITE_OK) << "prepare: " << sqlite3_errmsg(db);

        bool found = false;
        if (prep_rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *text = sqlite3_column_text(stmt, 0);
            found = text != nullptr &&
                    std::strcmp(reinterpret_cast<const char *>(text),
                                "hello sqlite fts5 world") == 0;
        }
        expect(found) << "FTS5 MATCH did not return the inserted row";

        sqlite3_finalize(stmt);
        sqlite3_close(db);
    };

    return 0;
}
