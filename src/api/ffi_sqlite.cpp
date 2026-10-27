/*
 * ffi_sqlite.cpp -- implementation of the SQLite3 C FFI (api/ffi_sqlite.h).
 *
 * THE HANDLES
 * -----------
 * `kimix_sqlite_db` and `kimix_sqlite_stmt` are typedefs of the real sqlite3 /
 * sqlite3_stmt (see the header), so this file is a pure pass-through: no
 * wrapper struct, no bookkeeping, no guard words.  A handle created by
 * kimix_sqlite_open() IS the sqlite3 pointer, which is what makes the area
 * cheap and what makes every SQLite semantic (lazy open, error latching,
 * busy handling, statement ownership) carry over untouched.
 *
 * ERROR MAPPING
 * -------------
 * Every entry point validates its handles first (NULL is never passed on to
 * SQLite): status-returning calls answer KIMIX_ERR_INVALID_ARG, value-returning
 * calls answer the documented neutral value (0 / false / "" / NULL / MISUSE).
 * A real SQLite failure maps like this:
 *   - SQLITE_RANGE (bad parameter index / column)  -> KIMIX_ERR_OUT_OF_RANGE
 *     (the one case where the caller's argument, not the data, is wrong);
 *   - every other non-OK code                      -> KIMIX_ERR_FAILED
 *     (read kimix_sqlite_errmsg() / kimix_sqlite_errcode() on the connection).
 * kimix_sqlite_step() is the exception: it returns the raw int result code
 * because the ROW/DONE/extended-code vocabulary IS the API.
 *
 * THE ONE OWNED BUFFER (kimix_sqlite_exec)
 * ----------------------------------------
 * sqlite3_exec() hands its error message out of SQLite's own allocator.  To
 * keep the one-heap rule of api/ffi_common.h (rule 4) -- every buffer the FFI
 * hands to the caller comes from THIS library's mimalloc heap and is released
 * with kimix_mem_free() -- the wrapper COPIES the text with mi_strdup() and
 * sqlite3_free()s the original.  The copy is what the caller owns; the
 * borrowed twin stays readable through kimix_sqlite_errmsg() without any heap
 * involvement.
 *
 * UNITY BUILD
 * -----------
 * Same rule as the other src/api TUs: every file-local helper is
 * `sqlite_`-prefixed inside an anonymous namespace; the only global names this
 * file adds are the kimix_sqlite_* entry points the header declares.
 * <sqlite3.h> is included FIRST so the kimix_sqlite_db / kimix_sqlite_stmt
 * typedefs in api/ffi_sqlite.h complete against the real declarations.
 */
#include <sqlite3.h>

#include <api/ffi_sqlite.h>

#include <mimalloc.h>

#include <cstdint>

namespace src_api_ffi_sqlite {

/* Map a raw SQLite result code onto the kimix_status vocabulary of
 * api/ffi_common.h: SQLITE_RANGE means the caller's index was outside the
 * statement's parameter range, everything else non-OK is the generic failure
 * (the connection carries the detail). */
inline kimix_status sqlite_status_of(int rc) noexcept {
    switch (rc) {
    case SQLITE_OK:
        return KIMIX_OK;
    case SQLITE_RANGE:
        return KIMIX_ERR_OUT_OF_RANGE;
    default:
        return KIMIX_ERR_FAILED;
    }
}

} // namespace src_api_ffi_sqlite
using namespace src_api_ffi_sqlite;

namespace src_api_ffi_sqlite {

/* SQLite's column accessors take a NON-const sqlite3_stmt * (reading a column
 * may convert the value in place and materialise a text/blob buffer), so the
 * const-qualified FFI surface casts the const away here -- one place, one
 * explanation, instead of a cast at every call site below.  The logical
 * contract stays read-only: no accessor changes bindings or the result set. */
inline sqlite3_stmt *sqlite_mutable(const kimix_sqlite_stmt *stmt) noexcept {
    return const_cast<sqlite3_stmt *>(stmt);
}

inline sqlite3 *sqlite_mutable(const kimix_sqlite_db *db) noexcept {
    return const_cast<sqlite3 *>(db);
}

} // namespace src_api_ffi_sqlite
using namespace src_api_ffi_sqlite;

KIMIX_FFI_BEGIN

// ===========================================================================
// 1. Library
// ===========================================================================

const char *kimix_sqlite_libversion(void) {
    return sqlite3_libversion();
}

int kimix_sqlite_libversion_number(void) {
    return sqlite3_libversion_number();
}

bool kimix_sqlite_threadsafe(void) {
    return sqlite3_threadsafe() != 0;
}

// ===========================================================================
// 2. Connection lifetime
// ===========================================================================

kimix_status kimix_sqlite_open(const char *filename, kimix_sqlite_db **out_db) {
    return kimix_sqlite_open_v2(filename, out_db,
                                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX);
}

kimix_status kimix_sqlite_open_v2(const char *filename, kimix_sqlite_db **out_db, int flags) {
    if (!filename || !out_db) {
        return KIMIX_ERR_INVALID_ARG;
    }
    /* FULLMUTEX is forced on top of every caller flag set: the header
     * promises a serialized connection (a handle may cross threads), and a
     * caller asking for the legacy single-thread mode would only get a
      * data-race footgun for free. */
    sqlite3 *db = nullptr;
    const int rc = sqlite3_open_v2(filename, &db, flags | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        /* Even a failed open_v2 may return a connection object that owns the
         * error message -- SQLite's documented behaviour.  Nothing of it may
         * escape to the caller (out_db is only written on success), so close
         * it here, message included. */
        sqlite3_close(db);
        return KIMIX_ERR_FAILED;
    }
    *out_db = db; // written only on success
    return KIMIX_OK;
}

kimix_status kimix_sqlite_close(kimix_sqlite_db *db) {
    if (!db) {
        return KIMIX_ERR_INVALID_ARG;
    }
    return sqlite_status_of(sqlite3_close(db));
}

// ===========================================================================
// 3. Error reporting
// ===========================================================================

const char *kimix_sqlite_errmsg(const kimix_sqlite_db *db) {
    if (!db) {
        return ""; // the neutral string: no connection, no message
    }
    return sqlite3_errmsg(sqlite_mutable(db));
}

int kimix_sqlite_errcode(const kimix_sqlite_db *db) {
    if (!db) {
        return KIMIX_SQLITE_MISUSE;
    }
    return sqlite3_errcode(sqlite_mutable(db));
}

int kimix_sqlite_extended_errcode(const kimix_sqlite_db *db) {
    if (!db) {
        return KIMIX_SQLITE_MISUSE;
    }
    return sqlite3_extended_errcode(sqlite_mutable(db));
}

// ===========================================================================
// 4. One-shot execution
// ===========================================================================

kimix_status kimix_sqlite_exec(kimix_sqlite_db *db, const char *sql, char **out_errmsg) {
    if (!db || !sql) {
        return KIMIX_ERR_INVALID_ARG;
    }
    char *sqlite_msg = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &sqlite_msg);
    if (rc == SQLITE_OK) {
        return KIMIX_OK;
    }
    /* Copy the message onto the library heap (one-heap rule): the caller
     * releases the copy with kimix_mem_free(); the SQLite original goes back
     * to SQLite's allocator.  A NULL `out_errmsg` (nobody asks) still frees
     * the original.  OOM on the copy yields NULL -- the status already says
     * "failed", and kimix_sqlite_errmsg() remains the allocation-free twin. */
    if (out_errmsg) {
        *out_errmsg = sqlite_msg ? mi_strdup(sqlite_msg) : nullptr;
    }
    if (sqlite_msg) {
        sqlite3_free(sqlite_msg);
    }
    return KIMIX_ERR_FAILED;
}

// ===========================================================================
// 5. Changes
// ===========================================================================

int64_t kimix_sqlite_last_insert_rowid(const kimix_sqlite_db *db) {
    if (!db) {
        return 0;
    }
    return sqlite3_last_insert_rowid(sqlite_mutable(db));
}

int64_t kimix_sqlite_changes(const kimix_sqlite_db *db) {
    if (!db) {
        return 0;
    }
    return sqlite3_changes64(sqlite_mutable(db));
}

kimix_status kimix_sqlite_busy_timeout(kimix_sqlite_db *db, int milliseconds) {
    if (!db) {
        return KIMIX_ERR_INVALID_ARG;
    }
    return sqlite_status_of(sqlite3_busy_timeout(db, milliseconds));
}

// ===========================================================================
// 6. Prepared statements: lifetime
// ===========================================================================

kimix_status kimix_sqlite_prepare(kimix_sqlite_db *db, const char *sql, kimix_sqlite_stmt **out_stmt) {
    if (!db || !sql || !out_stmt) {
        return KIMIX_ERR_INVALID_ARG;
    }
    sqlite3_stmt *stmt = nullptr;
    const int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return KIMIX_ERR_FAILED; // the connection's errmsg/errcode carry the detail
    }
    *out_stmt = stmt; // written only on success
    return KIMIX_OK;
}

int kimix_sqlite_step(kimix_sqlite_stmt *stmt) {
    if (!stmt) {
        return KIMIX_SQLITE_MISUSE;
    }
    return sqlite3_step(stmt);
}

kimix_status kimix_sqlite_finalize(kimix_sqlite_stmt *stmt) {
    if (!stmt) {
        return KIMIX_ERR_INVALID_ARG;
    }
    /* finalize() reports what the last step could not: an error return here
     * is the step failure the caller saw earlier, delivered through the call
     * that ends the statement's life. */
    return sqlite_status_of(sqlite3_finalize(stmt));
}

kimix_status kimix_sqlite_reset(kimix_sqlite_stmt *stmt) {
    if (!stmt) {
        return KIMIX_ERR_INVALID_ARG;
    }
    return sqlite_status_of(sqlite3_reset(stmt));
}

kimix_status kimix_sqlite_clear_bindings(kimix_sqlite_stmt *stmt) {
    if (!stmt) {
        return KIMIX_ERR_INVALID_ARG;
    }
    return sqlite_status_of(sqlite3_clear_bindings(stmt));
}

// ===========================================================================
// 7. Prepared statements: parameter binding
// ===========================================================================

int kimix_sqlite_bind_parameter_count(const kimix_sqlite_stmt *stmt) {
    if (!stmt) {
        return 0;
    }
    return sqlite3_bind_parameter_count(sqlite_mutable(stmt));
}

int kimix_sqlite_bind_parameter_index(const kimix_sqlite_stmt *stmt, const char *name) {
    if (!stmt || !name) {
        return 0; // 0 == "no such parameter": the neutral value doubles as the error
    }
    return sqlite3_bind_parameter_index(sqlite_mutable(stmt), name);
}

kimix_status kimix_sqlite_bind_int64(kimix_sqlite_stmt *stmt, int index, int64_t value) {
    if (!stmt) {
        return KIMIX_ERR_INVALID_ARG;
    }
    return sqlite_status_of(sqlite3_bind_int64(stmt, index, value));
}

kimix_status kimix_sqlite_bind_double(kimix_sqlite_stmt *stmt, int index, double value) {
    if (!stmt) {
        return KIMIX_ERR_INVALID_ARG;
    }
    return sqlite_status_of(sqlite3_bind_double(stmt, index, value));
}

kimix_status kimix_sqlite_bind_null(kimix_sqlite_stmt *stmt, int index) {
    if (!stmt) {
        return KIMIX_ERR_INVALID_ARG;
    }
    return sqlite_status_of(sqlite3_bind_null(stmt, index));
}

kimix_status kimix_sqlite_bind_text(kimix_sqlite_stmt *stmt, int index, const char *text, int length) {
    if (!stmt) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (length < 0) {
        return KIMIX_ERR_INVALID_ARG; // the length is explicit on this surface (see the header)
    }
    /* A NULL pointer binds SQL NULL; any other pointer binds exactly
     * `length` bytes (SQLite copies them below), so "" / 0 is the EMPTY
     * string -- the NULL-value spelling is kimix_sqlite_bind_null(). */
    if (!text) {
        return sqlite_status_of(sqlite3_bind_null(stmt, index));
    }
    if (length == 0) {
        static const char k_empty[] = "";
        return sqlite_status_of(sqlite3_bind_text(stmt, index, k_empty, 0, SQLITE_TRANSIENT));
    }
    /* SQLITE_TRANSIENT: the statement copies the bytes NOW, so the caller's
     * buffer (a stack slot, a rented array, anything) may die at return. */
    return sqlite_status_of(sqlite3_bind_text(stmt, index, text, length, SQLITE_TRANSIENT));
}

kimix_status kimix_sqlite_bind_blob(kimix_sqlite_stmt *stmt, int index, const void *data, int length) {
    if (!stmt) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (length < 0) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (!data) {
        return sqlite_status_of(sqlite3_bind_null(stmt, index));
    }
    if (length == 0) {
        static const char k_empty = 0;
        return sqlite_status_of(sqlite3_bind_blob(stmt, index, &k_empty, 0, SQLITE_TRANSIENT));
    }
    return sqlite_status_of(sqlite3_bind_blob(stmt, index, data, length, SQLITE_TRANSIENT));
}

// ===========================================================================
// 8. Result columns
// ===========================================================================

int kimix_sqlite_column_count(const kimix_sqlite_stmt *stmt) {
    if (!stmt) {
        return 0;
    }
    return sqlite3_column_count(sqlite_mutable(stmt));
}

int kimix_sqlite_column_type(const kimix_sqlite_stmt *stmt, int col) {
    if (!stmt) {
        return KIMIX_SQLITE_NULL;
    }
    return sqlite3_column_type(sqlite_mutable(stmt), col);
}

int64_t kimix_sqlite_column_int64(const kimix_sqlite_stmt *stmt, int col) {
    if (!stmt) {
        return 0;
    }
    return sqlite3_column_int64(sqlite_mutable(stmt), col);
}

double kimix_sqlite_column_double(const kimix_sqlite_stmt *stmt, int col) {
    if (!stmt) {
        return 0.0;
    }
    return sqlite3_column_double(sqlite_mutable(stmt), col);
}

int kimix_sqlite_column_bytes(const kimix_sqlite_stmt *stmt, int col) {
    if (!stmt) {
        return 0;
    }
    return sqlite3_column_bytes(sqlite_mutable(stmt), col);
}

const char *kimix_sqlite_column_text(const kimix_sqlite_stmt *stmt, int col) {
    if (!stmt) {
        return nullptr;
    }
    /* The reinterpret_cast matches SQLite's own sqlite3_column_text() spelling
     * (it returns `const unsigned char *` for historical reasons; the bytes
     * are NUL-terminated UTF-8 either way). */
    return reinterpret_cast<const char *>(sqlite3_column_text(sqlite_mutable(stmt), col));
}

const void *kimix_sqlite_column_blob(const kimix_sqlite_stmt *stmt, int col) {
    if (!stmt) {
        return nullptr;
    }
    return sqlite3_column_blob(sqlite_mutable(stmt), col);
}

const char *kimix_sqlite_column_name(const kimix_sqlite_stmt *stmt, int col) {
    if (!stmt) {
        return nullptr;
    }
    return sqlite3_column_name(sqlite_mutable(stmt), col);
}

KIMIX_FFI_END
