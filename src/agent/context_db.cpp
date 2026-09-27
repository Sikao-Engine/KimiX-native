// agent/context_db.cpp - implementation of the SQLite context store.
// See context_db.h for the reference mapping. Unity build note: TU-local
// helpers live in an anonymous namespace with the `cxdb_` prefix.

#include "agent/context_db.h"
#include <cstdio>
#include <system_error>
#include "core/json_repair.h"
#include "core/stl/filesystem.h"
#include "core/stl/format.h"
#include "llm/yyjson_alc.h"
#include "runtime/common/utf8.h"
#include "sqlite3.h"
#include "yyjson.h"

namespace kimix::agent {
namespace {

// ---------------------------------------------------------------------------
// Schema (context_db.py:59-94): the same 5 tables, columns and indexes.
// Executed inside a transaction by ContextDb::open().
// ---------------------------------------------------------------------------
constexpr const char *kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS messages (
    rowid INTEGER PRIMARY KEY AUTOINCREMENT,
    role TEXT NOT NULL,
    content TEXT NOT NULL,
    content_text TEXT,
    created_at  REAL NOT NULL DEFAULT (unixepoch())
);

CREATE TABLE IF NOT EXISTS system_prompt (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    content TEXT NOT NULL,
    updated_at  REAL NOT NULL DEFAULT (unixepoch())
);

CREATE TABLE IF NOT EXISTS checkpoints (
    id INTEGER NOT NULL,
    message_rowid INTEGER,
    created_at REAL NOT NULL DEFAULT (unixepoch()),
    PRIMARY KEY (id)
);

CREATE TABLE IF NOT EXISTS usage_snapshots (
    rowid INTEGER PRIMARY KEY AUTOINCREMENT,
    token_count INTEGER NOT NULL,
    created_at  REAL NOT NULL DEFAULT (unixepoch())
);

CREATE TABLE IF NOT EXISTS meta (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_messages_role ON messages(role);
CREATE INDEX IF NOT EXISTS idx_messages_created_at ON messages(created_at);
)SQL";

// ---------------------------------------------------------------------------
// Small path / file helpers (POSIX paths only, std::error_code overloads -
// never throw).
// ---------------------------------------------------------------------------
bool cxdb_to_path(kimix::string_view text, kimix::filesystem::path &out) {
    return kimix::path_from_narrow(text, out);
}

bool cxdb_exists(const kimix::filesystem::path &p) {
    std::error_code ec;
    return kimix::filesystem::exists(p, ec) && !ec;
}

// ---------------------------------------------------------------------------
// yyjson helpers (same conventions as the session store's clis_* helpers).
// ---------------------------------------------------------------------------
bool cxdb_obj_add(yyjson_mut_doc *doc, yyjson_mut_val *obj, kimix::string_view key,
                  yyjson_mut_val *val) {
    if (obj == nullptr || val == nullptr) {
        return false;
    }
    yyjson_mut_val *key_val = yyjson_mut_strncpy(doc, key.data(), key.size());
    return key_val != nullptr && yyjson_mut_obj_add(obj, key_val, val);
}

void cxdb_add_str(yyjson_mut_doc *doc, yyjson_mut_val *obj, kimix::string_view key,
                  kimix::string_view value) {
    // yyjson_mut_strncpy rejects a null data pointer; an empty view's data()
    // is null, so route it at a stable empty literal (the key must serialize
    // as "" exactly like the reference's exclude_none=False empty string).
    cxdb_obj_add(doc, obj, key,
                 yyjson_mut_strncpy(doc, value.empty() ? "" : value.data(),
                                    value.size()));
}

const yyjson_val *cxdb_member(const yyjson_val *obj, kimix::string_view key) {
    if (obj == nullptr || !yyjson_is_obj(obj) || key.empty()) {
        return nullptr;
    }
    return yyjson_obj_getn(obj, key.data(), key.size());
}

kimix::string cxdb_get_str(const yyjson_val *obj, kimix::string_view key) {
    const yyjson_val *v = cxdb_member(obj, key);
    if (v == nullptr || !yyjson_is_str(v)) {
        return {};
    }
    return kimix::string(yyjson_get_str(v), yyjson_get_len(v));
}

int64_t cxdb_get_int(const yyjson_val *obj, kimix::string_view key, int64_t fallback) {
    const yyjson_val *v = cxdb_member(obj, key);
    if (v == nullptr || !yyjson_is_num(v)) {
        return fallback;
    }
    if (yyjson_is_uint(v)) {
        return static_cast<int64_t>(yyjson_get_uint(v));
    }
    if (yyjson_is_sint(v)) {
        return yyjson_get_sint(v);
    }
    return static_cast<int64_t>(yyjson_get_real(v));
}

// Compact JSON of one mutable document (one JSONL record).
kimix::string cxdb_compact_doc(yyjson_mut_doc *doc) {
    kimix::string out;
    size_t len = 0;
    char *json =
        yyjson_mut_write_opts(doc, 0, &kimix::llm::kYYJsonAlcMi, &len, nullptr);
    if (json == nullptr) {
        return out;
    }
    out.assign(json, len);
    mi_free(json);
    return out;
}

// Compact JSON of one parsed (immutable) value.
kimix::string cxdb_compact_val(const yyjson_val *val) {
    kimix::string out;
    if (val == nullptr) {
        return out;
    }
    size_t len = 0;
    char *json =
        yyjson_val_write_opts(val, 0, &kimix::llm::kYYJsonAlcMi, &len, nullptr);
    if (json == nullptr) {
        return out;
    }
    out.assign(json, len);
    mi_free(json);
    return out;
}

yyjson_doc *cxdb_parse_strict(kimix::string_view text) {
    if (text.empty()) {
        return nullptr;
    }
    return yyjson_read_opts(const_cast<char *>(text.data()), text.size(),
                            0 /* strict, stop on error */, &kimix::llm::kYYJsonAlcMi,
                            nullptr);
}

// ---------------------------------------------------------------------------
// loads_relaxed (kosong/utils/jsonx.py): strict parse first, repair fallback.
// Invalid UTF-8 bytes are replaced with U+FFFD first, matching the reference's
// errors="replace" file reads. Returns nullptr when the line is unparseable
// even after repair (callers skip it).
// ---------------------------------------------------------------------------
kimix::string cxdb_utf8_replace(kimix::string_view bytes) {
    if (kimix::runtime::common::is_ascii(bytes)) {
        return kimix::string(bytes);
    }
    kimix::string out;
    out.reserve(bytes.size());
    const char *it = bytes.data();
    const char *end = it + bytes.size();
    while (it < end) {
        const uint32_t cp = kimix::runtime::common::decode_cp(it, end);
        const size_t n = kimix::runtime::common::utf8_byte_length(cp);
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else {
            // decode_cp already yields U+FFFD for invalid bytes; re-encode.
            if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            (void)n;
        }
    }
    return out;
}

yyjson_doc *cxdb_parse_lenient(kimix::string_view line) {
    const kimix::string cleaned = cxdb_utf8_replace(line);
    yyjson_doc *doc = cxdb_parse_strict(cleaned);
    if (doc != nullptr) {
        return doc;
    }
    const kimix::string repaired = kimix::repair(cleaned);
    if (repaired.empty()) {
        return nullptr;// unrepairable
    }
    return cxdb_parse_strict(repaired);
}

// ---------------------------------------------------------------------------
// extract_text_from_content (fts5_search.py:200-235): plain searchable text
// of a message "content" value (str, one part object, or a part list), parts
// joined by newlines.
// ---------------------------------------------------------------------------
void cxdb_extract_append(kimix::string &out, const yyjson_val *value) {
    if (value == nullptr) {
        return;
    }
    if (yyjson_is_str(value)) {
        if (!out.empty()) {
            out.push_back('\n');
        }
        out.append(yyjson_get_str(value), yyjson_get_len(value));
        return;
    }
    if (yyjson_is_obj(value)) {
        const yyjson_val *text = cxdb_member(value, "text");
        if (text != nullptr && yyjson_is_str(text)) {
            if (!out.empty()) {
                out.push_back('\n');
            }
            out.append(yyjson_get_str(text), yyjson_get_len(text));
        }
    }
}

kimix::string cxdb_extract_text(const yyjson_val *content) {
    kimix::string out;
    if (content == nullptr) {
        return out;
    }
    if (yyjson_is_arr(content)) {
        size_t idx = 0, max = 0;
        yyjson_val *item = nullptr;
        yyjson_arr_foreach(content, idx, max, item) { cxdb_extract_append(out, item); }
        return out;
    }
    cxdb_extract_append(out, content);
    return out;
}

// ---------------------------------------------------------------------------
// Record <-> Message conversion. Byte-compatible with the native CLI's
// context.jsonl writer (src/cli/cli_session.cpp clis_write_message) and the
// reader (clis_parse_message_record), which themselves mirror the reference's
// model_dump_json(exclude_none=True) record.
// ---------------------------------------------------------------------------

// Serialize one message as the reference's JSONL record.
void cxdb_write_message(yyjson_mut_doc *doc, const kimix::llm::Message &msg) {
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    cxdb_add_str(doc, root, "role", msg.role);
    const bool has_think = !msg.thinking.empty() || !msg.thinking_signature.empty();
    // E1/E2: media parts (kind >= 2) force the part-array record form - the
    // same shape the session store's JSONL writer emits (the reference's
    // part.model_dump(): {"type":"image_url","image_url":{"url":...}}).
    bool has_media = false;
    for (const kimix::llm::ContentPart &part : msg.parts) {
        if (part.kind != kimix::llm::ContentPart::Kind::text &&
            part.kind != kimix::llm::ContentPart::Kind::think) {
            has_media = true;
            break;
        }
    }
    if (has_think || has_media) {
        yyjson_mut_val *parts = yyjson_mut_arr(doc);
        if (has_think) {
            yyjson_mut_val *think = yyjson_mut_obj(doc);
            cxdb_add_str(doc, think, "type", "think");
            cxdb_add_str(doc, think, "think", msg.thinking);
            if (!msg.thinking_signature.empty()) {
                cxdb_add_str(doc, think, "encrypted", msg.thinking_signature);
            }
            yyjson_mut_arr_add_val(parts, think);
        }
        if (!msg.content.empty()) {
            yyjson_mut_val *text = yyjson_mut_obj(doc);
            cxdb_add_str(doc, text, "type", "text");
            cxdb_add_str(doc, text, "text", msg.content);
            yyjson_mut_arr_add_val(parts, text);
        }
        for (const kimix::llm::ContentPart &part : msg.parts) {
            const char *type_name = nullptr;
            if (part.kind == kimix::llm::ContentPart::Kind::image_url) {
                type_name = "image_url";
            } else if (part.kind == kimix::llm::ContentPart::Kind::audio_url) {
                type_name = "audio_url";
            } else if (part.kind == kimix::llm::ContentPart::Kind::video_url) {
                type_name = "video_url";
            } else {
                continue;
            }
            yyjson_mut_val *media = yyjson_mut_obj(doc);
            yyjson_mut_arr_add_val(parts, media);
            cxdb_add_str(doc, media, "type", type_name);
            yyjson_mut_val *payload = yyjson_mut_obj(doc);
            cxdb_obj_add(doc, media, type_name, payload);
            cxdb_add_str(doc, payload, "url", part.url);
        }
        cxdb_obj_add(doc, root, "content", parts);
    } else if (msg.content.empty()) {
        // Bare string content, possibly empty; a message with tool calls and no
        // text keeps the reference's empty part list.
        if (msg.tool_calls.empty()) {
            cxdb_add_str(doc, root, "content", kimix::string_view());
        } else {
            cxdb_obj_add(doc, root, "content", yyjson_mut_arr(doc));
        }
    } else {
        cxdb_add_str(doc, root, "content", msg.content);
    }
    if (!msg.tool_calls.empty()) {
        yyjson_mut_val *calls = yyjson_mut_arr(doc);
        for (const kimix::llm::ToolCall &call : msg.tool_calls) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            cxdb_add_str(doc, item, "type",
                         call.type.empty() ? kimix::string_view("function")
                                           : kimix::string_view(call.type));
            cxdb_add_str(doc, item, "id", call.id);
            yyjson_mut_val *fn = yyjson_mut_obj(doc);
            cxdb_add_str(doc, fn, "name", call.name);
            if (!call.arguments.empty()) {
                cxdb_add_str(doc, fn, "arguments", call.arguments);
            }
            cxdb_obj_add(doc, item, "function", fn);
            yyjson_mut_arr_add_val(calls, item);
        }
        cxdb_obj_add(doc, root, "tool_calls", calls);
    }
    if (!msg.tool_call_id.empty()) {
        cxdb_add_str(doc, root, "tool_call_id", msg.tool_call_id);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Backend auto-detection (context.py:558-576)
// ---------------------------------------------------------------------------

ContextBackend detect_context_backend(kimix::string_view path) noexcept {
    return path.size() >= 3 && path.substr(path.size() - 3) == ".db"
               ? ContextBackend::sqlite
               : ContextBackend::jsonl;
}

kimix::string resolve_context_db_path(kimix::string_view path) {
    if (path.size() >= 6 && path.substr(path.size() - 6) == ".jsonl") {
        kimix::string out(path.substr(0, path.size() - 6));
        out.append(".db");
        return out;
    }
    return kimix::string(path);
}

bool needs_context_migration(kimix::string_view path) noexcept {
    kimix::filesystem::path jsonl;
    kimix::filesystem::path db;
    if (!cxdb_to_path(path, jsonl)) {
        return false;
    }
    db = jsonl;
    db.replace_extension(".db");
    jsonl.replace_extension(".jsonl");
    return cxdb_exists(jsonl) && !cxdb_exists(db);
}

// ---------------------------------------------------------------------------
// Record <-> Message conversion (public)
// ---------------------------------------------------------------------------

kimix::string context_record_from_message(const kimix::llm::Message &msg) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    if (doc == nullptr) {
        return {};
    }
    cxdb_write_message(doc, msg);
    kimix::string out = cxdb_compact_doc(doc);
    yyjson_mut_doc_free(doc);
    return out;
}

bool context_message_from_record(kimix::string_view record, kimix::llm::Message &out) {
    yyjson_doc *doc = cxdb_parse_strict(record);
    if (doc == nullptr) {
        return false;
    }
    const yyjson_val *root = yyjson_doc_get_root(doc);
    bool ok = false;
    if (yyjson_is_obj(root)) {
        const kimix::string role = cxdb_get_str(root, "role");
        if (role == "system" || role == "user" || role == "assistant" ||
            role == "tool") {
            out.role = role;
            out.content.clear();
            out.thinking.clear();
            out.thinking_signature.clear();
            out.tool_calls.clear();
            out.tool_call_id.clear();
            // Content: bare string or a part array (think/text parts), exactly
            // like the session store's reader.
    const yyjson_val *content = cxdb_member(root, "content");
    kimix::string thinking_signature;
    bool has_media = false;
    kimix::vector<kimix::llm::ContentPart> media_parts;
    if (yyjson_is_str(content)) {
        out.content.assign(yyjson_get_str(content), yyjson_get_len(content));
    } else if (yyjson_is_arr(content)) {
        size_t idx = 0, max = 0;
        yyjson_val *item = nullptr;
        yyjson_arr_foreach(content, idx, max, item) {
            if (!yyjson_is_obj(item)) {
                continue;
            }
            const kimix::string type = cxdb_get_str(item, "type");
            if (type == "think") {
                out.thinking += cxdb_get_str(item, "think");
                const kimix::string enc = cxdb_get_str(item, "encrypted");
                if (!enc.empty()) {
                    thinking_signature = enc;
                }
            } else if (type == "text") {
                out.content += cxdb_get_str(item, "text");
            } else if (type == "image_url" || type == "audio_url" ||
                       type == "video_url") {
                // E1/E2: restore the media part (nested reference record
                // shape, or a flat url member).
                has_media = true;
                kimix::llm::ContentPart media;
                media.kind =
                    type == "image_url"
                        ? kimix::llm::ContentPart::Kind::image_url
                        : type == "audio_url"
                              ? kimix::llm::ContentPart::Kind::audio_url
                              : kimix::llm::ContentPart::Kind::video_url;
                const yyjson_val *payload = cxdb_member(item, type);
                if (payload != nullptr && yyjson_is_obj(payload)) {
                    media.url = cxdb_get_str(payload, "url");
                }
                if (media.url.empty()) {
                    media.url = cxdb_get_str(item, "url");
                }
                media_parts.push_back(std::move(media));
            }
        }
    }
    out.thinking_signature = thinking_signature;
    if (has_media) {
        // Restore the parts adjunct: the text backbone as one leading text
        // part, then the media parts (the E1/E2 invariant).
        kimix::vector<kimix::llm::ContentPart> parts;
        if (!out.content.empty()) {
            kimix::llm::ContentPart text;
            text.kind = kimix::llm::ContentPart::Kind::text;
            text.text = out.content;
            parts.push_back(std::move(text));
        }
        for (kimix::llm::ContentPart &media : media_parts) {
            parts.push_back(std::move(media));
        }
        kimix::llm::message_set_parts(out, std::move(parts));
    }
            const yyjson_val *calls = cxdb_member(root, "tool_calls");
            if (calls != nullptr && yyjson_is_arr(calls)) {
                size_t idx = 0, max = 0;
                yyjson_val *item = nullptr;
                yyjson_arr_foreach(calls, idx, max, item) {
                    if (!yyjson_is_obj(item)) {
                        continue;
                    }
                    kimix::llm::ToolCall call;
                    call.type = cxdb_get_str(item, "type");
                    call.id = cxdb_get_str(item, "id");
                    const yyjson_val *fn = cxdb_member(item, "function");
                    if (fn != nullptr && yyjson_is_obj(fn)) {
                        call.name = cxdb_get_str(fn, "name");
                        call.arguments = cxdb_get_str(fn, "arguments");
                    }
                    out.tool_calls.push_back(std::move(call));
                }
            }
            out.tool_call_id = cxdb_get_str(root, "tool_call_id");
            ok = true;
        }
    }
    yyjson_doc_free(doc);
    return ok;
}

// ---------------------------------------------------------------------------
// ContextDb
// ---------------------------------------------------------------------------

ContextDb::ContextDb(kimix::filesystem::path db_path) : _db_path(std::move(db_path)) {}

ContextDb::~ContextDb() { close(); }

ContextDb::ContextDb(ContextDb &&other) noexcept
    : _db_path(std::move(other._db_path)), _db(other._db),
      _in_transaction(other._in_transaction),
      _last_message_rowid(other._last_message_rowid) {
    other._db = nullptr;
    other._in_transaction = false;
}

ContextDb &ContextDb::operator=(ContextDb &&other) noexcept {
    if (this != &other) {
        close();
        _db_path = std::move(other._db_path);
        _db = other._db;
        _in_transaction = other._in_transaction;
        _last_message_rowid = other._last_message_rowid;
        other._db = nullptr;
        other._in_transaction = false;
    }
    return *this;
}

bool ContextDb::exec(const char *sql, kimix::string &error) const {
    char *errmsg = nullptr;
    const int rc = sqlite3_exec(_db, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        error = "sqlite: ";
        error += errmsg != nullptr ? errmsg : sqlite3_errstr(rc);
        sqlite3_free(errmsg);
        return false;
    }
    return true;
}

bool ContextDb::prepare(const char *sql, sqlite3_stmt *&stmt, kimix::string &error) const {
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        stmt = nullptr;
        error = "sqlite prepare failed: ";
        error += sqlite3_errmsg(_db);
        return false;
    }
    return true;
}

bool ContextDb::maybe_commit(kimix::string &error) const {
    if (_in_transaction || sqlite3_get_autocommit(_db) != 0) {
        return true;// explicit tx active, or sqlite already auto-committed
    }
    return exec("COMMIT", error);
}

bool ContextDb::open(kimix::string &error) {
    error.clear();
    if (_db != nullptr) {
        return true;
    }
    std::error_code ec;
    const kimix::filesystem::path parent = _db_path.parent_path();
    if (!parent.empty()) {
        kimix::filesystem::create_directories(parent, ec);
        if (ec) {
            error = "cannot create database directory: ";
            error += kimix::to_string(_db_path);
            return false;
        }
    }
    if (sqlite3_open_v2(kimix::to_string(_db_path).c_str(), &_db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        error = "sqlite open failed: ";
        error += _db != nullptr ? sqlite3_errmsg(_db) : "out of memory";
        if (_db != nullptr) {
            sqlite3_close(_db);
            _db = nullptr;
        }
        return false;
    }
    // context_db.py:216-229 pragma set, plus a busy timeout so concurrent
    // session processes wait instead of failing with SQLITE_BUSY.
    static const char *const kPragmas[] = {
        "PRAGMA journal_mode=WAL",
        "PRAGMA foreign_keys=ON",
        "PRAGMA synchronous=NORMAL",
        "PRAGMA cache_size=-32000",// 32 MB cache
        "PRAGMA temp_store=MEMORY",
        "PRAGMA busy_timeout=5000",
    };
    for (const char *pragma : kPragmas) {
        if (!exec(pragma, error)) {
            close();
            return false;
        }
    }
    // Schema inside a transaction, like the reference's initialize+commit.
    if (!exec("BEGIN", error) || !exec(kSchemaSql, error) || !exec("COMMIT", error)) {
        exec("ROLLBACK", error);
        close();
        return false;
    }
    return true;
}

void ContextDb::close() noexcept {
    if (_db == nullptr) {
        return;
    }
    // Best-effort passive checkpoint so the -wal file is folded back when no
    // other connection holds it (the reference relies on the last close
    // doing this implicitly).
    sqlite3_exec(_db, "PRAGMA wal_checkpoint(PASSIVE)", nullptr, nullptr, nullptr);
    sqlite3_close_v2(_db);
    _db = nullptr;
    _in_transaction = false;
}

bool ContextDb::begin_transaction(kimix::string &error) {
    error.clear();
    if (_db == nullptr && !open(error)) {
        return false;
    }
    if (_in_transaction) {
        error = "a transaction is already active";
        return false;
    }
    if (!exec("BEGIN", error)) {
        return false;
    }
    _in_transaction = true;
    return true;
}

bool ContextDb::commit_transaction(kimix::string &error) {
    error.clear();
    if (!_in_transaction) {
        error = "no transaction is active";
        return false;
    }
    if (!exec("COMMIT", error)) {
        return false;
    }
    _in_transaction = false;
    return true;
}

bool ContextDb::rollback_transaction(kimix::string &error) {
    error.clear();
    if (!_in_transaction) {
        return true;
    }
    const bool ok = exec("ROLLBACK", error);
    _in_transaction = false;
    return ok;
}

bool ContextDb::append(const ContextRecord &rec, kimix::string &error) {
    return append_batch(kimix::span<const ContextRecord>(&rec, 1), error);
}

bool ContextDb::append_batch(kimix::span<const ContextRecord> recs, kimix::string &error) {
    error.clear();
    if (_db == nullptr && !open(error)) {
        return false;
    }
    // Every record lands in the messages table with the JSONL byte shape, like
    // context.py's append_messages for both backends.
    sqlite3_stmt *stmt = nullptr;
    if (!prepare("INSERT INTO messages (role, content, content_text) VALUES (?, ?, ?)",
                 stmt, error)) {
        return false;
    }
    bool ok = true;
    for (const ContextRecord &rec : recs) {
        yyjson_doc *doc = cxdb_parse_strict(rec.content);
        kimix::string text;
        if (doc != nullptr) {
            text = cxdb_extract_text(cxdb_member(yyjson_doc_get_root(doc), "content"));
        }
        sqlite3_bind_text(stmt, 1, rec.role.data(),
                          static_cast<int>(rec.role.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, rec.content.data(),
                          static_cast<int>(rec.content.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, text.data(), static_cast<int>(text.size()),
                          SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            error = "sqlite insert failed: ";
            error += sqlite3_errmsg(_db);
            ok = false;
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        if (doc != nullptr) {
            yyjson_doc_free(doc);
        }
        if (!ok) {
            break;
        }
    }
    sqlite3_finalize(stmt);
    return ok && maybe_commit(error);
}

bool ContextDb::append_message(const kimix::llm::Message &msg, kimix::string &error) {
    ContextRecord rec;
    rec.role = msg.role;
    rec.content = context_record_from_message(msg);
    if (rec.content.empty()) {
        error = "cannot serialize the message record";
        return false;
    }
    return append_batch(kimix::span<const ContextRecord>(&rec, 1), error);
}

bool ContextDb::read_after(int64_t after_rowid, int64_t limit,
                           kimix::vector<MessageRow> &out, kimix::string &error) const {
    error.clear();
    out.clear();
    if (_db == nullptr) {
        error = "context database is not open";
        return false;
    }
    // context_db.py:348-364: rowid > after_rowid ordered by rowid, optional LIMIT.
    const bool bounded = limit >= 0;
    sqlite3_stmt *stmt = nullptr;
    if (!prepare(bounded ? "SELECT rowid, role, content, created_at FROM messages "
                           "WHERE rowid > ? ORDER BY rowid LIMIT ?"
                         : "SELECT rowid, role, content, created_at FROM messages "
                           "WHERE rowid > ? ORDER BY rowid",
                 stmt, error)) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, after_rowid);
    if (bounded) {
        sqlite3_bind_int64(stmt, 2, limit);
    }
    bool ok = true;
    while (true) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            error = "sqlite select failed: ";
            error += sqlite3_errmsg(_db);
            ok = false;
            break;
        }
        MessageRow row;
        row.rowid = sqlite3_column_int64(stmt, 0);
        const unsigned char *role = sqlite3_column_text(stmt, 1);
        const unsigned char *content = sqlite3_column_text(stmt, 2);
        row.role.assign(role != nullptr ? reinterpret_cast<const char *>(role) : "",
                        role != nullptr ? static_cast<size_t>(sqlite3_column_bytes(stmt, 1))
                                        : 0);
        row.content.assign(content != nullptr ? reinterpret_cast<const char *>(content) : "",
                           content != nullptr
                               ? static_cast<size_t>(sqlite3_column_bytes(stmt, 2))
                               : 0);
        row.created_at = sqlite3_column_double(stmt, 3);
        out.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool ContextDb::message_count(int64_t &out, kimix::string &error) const {
    error.clear();
    out = 0;
    if (_db == nullptr) {
        error = "context database is not open";
        return false;
    }
    sqlite3_stmt *stmt = nullptr;
    if (!prepare("SELECT COUNT(*) FROM messages", stmt, error)) {
        return false;
    }
    const bool ok = sqlite3_step(stmt) == SQLITE_ROW;
    if (ok) {
        out = sqlite3_column_int64(stmt, 0);
    } else {
        error = "sqlite count failed: ";
        error += sqlite3_errmsg(_db);
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool ContextDb::last_message_rowid(int64_t &out, kimix::string &error) const {
    error.clear();
    out = 0;
    if (_db == nullptr) {
        error = "context database is not open";
        return false;
    }
    sqlite3_stmt *stmt = nullptr;
    if (!prepare("SELECT MAX(rowid) FROM messages", stmt, error)) {
        return false;
    }
    bool ok = true;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out = sqlite3_column_type(stmt, 0) == SQLITE_NULL ? 0 : sqlite3_column_int64(stmt, 0);
    } else {
        error = "sqlite select failed: ";
        error += sqlite3_errmsg(_db);
        ok = false;
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool ContextDb::has_visible_messages(bool &out, kimix::string &error) const {
    error.clear();
    out = false;
    if (_db == nullptr) {
        error = "context database is not open";
        return false;
    }
    sqlite3_stmt *stmt = nullptr;
    if (!prepare("SELECT 1 FROM messages WHERE role NOT IN "
                 "('_system_prompt', '_usage', '_checkpoint') LIMIT 1",
                 stmt, error)) {
        return false;
    }
    const int rc = sqlite3_step(stmt);
    const bool ok = rc == SQLITE_ROW || rc == SQLITE_DONE;
    out = rc == SQLITE_ROW;
    if (!ok) {
        error = "sqlite select failed: ";
        error += sqlite3_errmsg(_db);
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool ContextDb::delete_after(int64_t rowid, kimix::string &error) {
    error.clear();
    if (_db == nullptr) {
        error = "context database is not open";
        return false;
    }
    sqlite3_stmt *stmt = nullptr;
    if (!prepare("DELETE FROM messages WHERE rowid > ?", stmt, error)) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, rowid);
    const bool stepped = sqlite3_step(stmt) == SQLITE_DONE;
    if (!stepped) {
        error = "sqlite delete failed: ";
        error += sqlite3_errmsg(_db);
    }
    sqlite3_finalize(stmt);
    return stepped && maybe_commit(error);
}

bool ContextDb::clear(kimix::string &error) {
    error.clear();
    if (_db == nullptr && !open(error)) {
        return false;
    }
    const bool nested = _in_transaction;
    if (!nested && !exec("BEGIN", error)) {
        return false;
    }
    static const char *const kDeletes[] = {
        "DELETE FROM messages",
        "DELETE FROM system_prompt",
        "DELETE FROM checkpoints",
        "DELETE FROM usage_snapshots",
        "DELETE FROM meta",
    };
    bool ok = true;
    for (const char *sql : kDeletes) {
        if (!exec(sql, error)) {
            ok = false;
            break;
        }
    }
    if (!nested) {
        if (!exec(ok ? "COMMIT" : "ROLLBACK", error)) {
              return false;
          }
      }
      return ok;
  }

  bool ContextDb::clear_checkpoints(kimix::string &error) {
      error.clear();
      if (_db == nullptr && !open(error)) {
          return false;
      }
      return exec("DELETE FROM checkpoints", error);
  }

  bool ContextDb::clear_usage(kimix::string &error) {
      error.clear();
      if (_db == nullptr && !open(error)) {
          return false;
      }
      return exec("DELETE FROM usage_snapshots", error);
  }

  bool ContextDb::set_system_prompt(kimix::string_view content, kimix::string &error) {
    error.clear();
    if (_db == nullptr && !open(error)) {
        return false;
    }
    sqlite3_stmt *stmt = nullptr;
    if (!prepare("INSERT OR REPLACE INTO system_prompt (id, content, updated_at) "
                 "VALUES (1, ?, unixepoch())",
                 stmt, error)) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, content.data(), static_cast<int>(content.size()),
                      SQLITE_TRANSIENT);
    const bool stepped = sqlite3_step(stmt) == SQLITE_DONE;
    if (!stepped) {
        error = "sqlite insert failed: ";
        error += sqlite3_errmsg(_db);
    }
    sqlite3_finalize(stmt);
    return stepped && maybe_commit(error);
}

bool ContextDb::get_system_prompt(kimix::string &out, bool &found,
                                  kimix::string &error) const {
    error.clear();
    out.clear();
    found = false;
    if (_db == nullptr) {
        error = "context database is not open";
        return false;
    }
    sqlite3_stmt *stmt = nullptr;
    if (!prepare("SELECT content FROM system_prompt WHERE id = 1", stmt, error)) {
        return false;
    }
    const int rc = sqlite3_step(stmt);
    const bool ok = rc == SQLITE_ROW || rc == SQLITE_DONE;
    if (rc == SQLITE_ROW) {
        const unsigned char *content = sqlite3_column_text(stmt, 0);
        out.assign(content != nullptr ? reinterpret_cast<const char *>(content) : "",
                   content != nullptr ? static_cast<size_t>(sqlite3_column_bytes(stmt, 0))
                                      : 0);
        found = true;
    } else if (!ok) {
        error = "sqlite select failed: ";
        error += sqlite3_errmsg(_db);
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool ContextDb::record_usage(int64_t token_count, kimix::string &error) {
    error.clear();
    if (_db == nullptr && !open(error)) {
        return false;
    }
    sqlite3_stmt *stmt = nullptr;
    if (!prepare("INSERT INTO usage_snapshots (token_count) VALUES (?)", stmt, error)) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, token_count);
    const bool stepped = sqlite3_step(stmt) == SQLITE_DONE;
    if (!stepped) {
        error = "sqlite insert failed: ";
        error += sqlite3_errmsg(_db);
    }
    sqlite3_finalize(stmt);
    return stepped && maybe_commit(error);
}

bool ContextDb::latest_usage(int64_t &out, bool &found, kimix::string &error) const {
    error.clear();
    out = 0;
    found = false;
    if (_db == nullptr) {
        error = "context database is not open";
        return false;
    }
    sqlite3_stmt *stmt = nullptr;
    if (!prepare("SELECT token_count FROM usage_snapshots ORDER BY rowid DESC LIMIT 1",
                 stmt, error)) {
        return false;
    }
    const int rc = sqlite3_step(stmt);
    const bool ok = rc == SQLITE_ROW || rc == SQLITE_DONE;
    if (rc == SQLITE_ROW) {
        out = sqlite3_column_int64(stmt, 0);
        found = true;
    } else if (!ok) {
        error = "sqlite select failed: ";
        error += sqlite3_errmsg(_db);
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool ContextDb::export_usage_history(kimix::vector<int64_t> &out,
                                     kimix::string &error) const {
    error.clear();
    out.clear();
    if (_db == nullptr) {
        error = "context database is not open";
        return false;
    }
    sqlite3_stmt *stmt = nullptr;
    if (!prepare("SELECT token_count FROM usage_snapshots ORDER BY rowid", stmt,
                 error)) {
        return false;
    }
    int rc = sqlite3_step(stmt);
    while (rc == SQLITE_ROW) {
        out.push_back(sqlite3_column_int64(stmt, 0));
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        error = "sqlite select failed: ";
        error += sqlite3_errmsg(_db);
        return false;
    }
    return true;
}

bool ContextDb::create_checkpoint(int64_t checkpoint_id, int64_t &message_rowid,
                                  kimix::string &error) {
    error.clear();
    message_rowid = 0;
    if (_db == nullptr && !open(error)) {
        return false;
    }
    sqlite3_stmt *stmt = nullptr;
    // context_db.py:732-745: one subquery anchors the checkpoint at the
    // current max message rowid.
    if (!prepare("INSERT INTO checkpoints (id, message_rowid) VALUES (?, "
                 "(SELECT MAX(rowid) FROM messages))",
                 stmt, error)) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, checkpoint_id);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!ok) {
        error = "sqlite insert failed: ";
        error += sqlite3_errmsg(_db);
        return false;
    }
    if (!maybe_commit(error)) {
        return false;
    }
    bool found = false;
    if (!checkpoint_message_rowid(checkpoint_id, message_rowid, found, error)) {
        return false;
    }
    if (!found) {
        message_rowid = 0;
    }
    return true;
}

bool ContextDb::latest_checkpoint_id(int64_t &out, kimix::string &error) const {
    error.clear();
    out = -1;
    if (_db == nullptr) {
        error = "context database is not open";
        return false;
    }
    sqlite3_stmt *stmt = nullptr;
    if (!prepare("SELECT COALESCE(MAX(id), -1) FROM checkpoints", stmt, error)) {
        return false;
    }
    const bool ok = sqlite3_step(stmt) == SQLITE_ROW;
    if (ok) {
        out = sqlite3_column_int64(stmt, 0);
    } else {
        error = "sqlite select failed: ";
        error += sqlite3_errmsg(_db);
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool ContextDb::list_checkpoint_ids(kimix::vector<int64_t> &out,
                                    kimix::string &error) const {
    error.clear();
    out.clear();
    if (_db == nullptr) {
        error = "context database is not open";
        return false;
    }
    sqlite3_stmt *stmt = nullptr;
    if (!prepare("SELECT id FROM checkpoints ORDER BY id", stmt, error)) {
        return false;
    }
    int rc = sqlite3_step(stmt);
    while (rc == SQLITE_ROW) {
        out.push_back(sqlite3_column_int64(stmt, 0));
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        error = "sqlite select failed: ";
        error += sqlite3_errmsg(_db);
        return false;
    }
    return true;
}

bool ContextDb::checkpoint_message_rowid(int64_t checkpoint_id, int64_t &out,
                                         bool &found, kimix::string &error) const {
    error.clear();
    out = 0;
    found = false;
    if (_db == nullptr) {
        error = "context database is not open";
        return false;
    }
    sqlite3_stmt *stmt = nullptr;
    if (!prepare("SELECT message_rowid FROM checkpoints WHERE id = ?", stmt, error)) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, checkpoint_id);
    const int rc = sqlite3_step(stmt);
    const bool ok = rc == SQLITE_ROW || rc == SQLITE_DONE;
    if (rc == SQLITE_ROW) {
        out = sqlite3_column_type(stmt, 0) == SQLITE_NULL ? 0
                                                          : sqlite3_column_int64(stmt, 0);
        found = true;
    } else if (!ok) {
        error = "sqlite select failed: ";
        error += sqlite3_errmsg(_db);
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool ContextDb::revert_to_checkpoint(int64_t checkpoint_id, kimix::string &error) {
    error.clear();
    int64_t message_rowid = 0;
    bool found = false;
    if (!checkpoint_message_rowid(checkpoint_id, message_rowid, found, error)) {
        return false;
    }
    if (!found) {
        error = kimix::format("checkpoint {} not found", checkpoint_id);
        return false;
    }
    const bool nested = _in_transaction;
    if (!nested && !exec("BEGIN", error)) {
        return false;
    }
    bool ok = true;
    {
        sqlite3_stmt *stmt = nullptr;
        if (prepare("DELETE FROM messages WHERE rowid > ?", stmt, error)) {
            sqlite3_bind_int64(stmt, 1, message_rowid);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
            sqlite3_finalize(stmt);
        } else {
            ok = false;
        }
    }
    if (ok) {
        sqlite3_stmt *stmt = nullptr;
        if (prepare("DELETE FROM checkpoints WHERE id >= ?", stmt, error)) {
            sqlite3_bind_int64(stmt, 1, checkpoint_id);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
            sqlite3_finalize(stmt);
        } else {
            ok = false;
        }
    }
    if (ok) {
        // context_db.py:777-780: drop usage snapshots past the message cutoff.
        sqlite3_stmt *stmt = nullptr;
        if (prepare("DELETE FROM usage_snapshots WHERE rowid > COALESCE((SELECT "
                     "MAX(rowid) FROM usage_snapshots WHERE rowid <= ?), 0)",
                    stmt, error)) {
            sqlite3_bind_int64(stmt, 1, message_rowid);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
            sqlite3_finalize(stmt);
        } else {
            ok = false;
        }
    }
    if (!nested) {
        if (!exec(ok ? "COMMIT" : "ROLLBACK", error)) {
            return false;
        }
    }
    if (!ok && error.empty()) {
        error = "sqlite delete failed: ";
        error += sqlite3_errmsg(_db);
    }
    return ok;
}

bool ContextDb::fix_checkpoint_message_rowids(kimix::string &error) {
    error.clear();
    if (_db == nullptr && !open(error)) {
        return false;
    }
    return exec("UPDATE checkpoints SET message_rowid = id WHERE message_rowid = 0",
                error) &&
           maybe_commit(error);
}

bool ContextDb::migrate_jsonl(const kimix::filesystem::path &jsonl_path, bool &migrated,
                              kimix::string &error) {
    error.clear();
    migrated = false;
    // session.py: Migration 2 runs only when the JSONL exists and the DB does
    // not; both already-absent states make the call a no-op (idempotent).
    if (!cxdb_exists(jsonl_path)) {
        return true;
    }
    kimix::filesystem::path db_path = jsonl_path;
    db_path.replace_extension(".db");
    const bool we_created = !cxdb_exists(db_path) && !is_open();
    if (cxdb_exists(db_path) && !is_open()) {
        return true;// DB already exists: migration already happened (or is owned elsewhere)
    }
    if (_db_path != db_path) {
        error = "database path does not match the JSONL's resolved .db path";
        return false;
    }
    // Read the whole JSONL (errors="replace" + loads_relaxed per line).
    kimix::filesystem::path fs_jsonl = jsonl_path;
    std::error_code ec;
    const auto file_size = kimix::filesystem::file_size(fs_jsonl, ec);
    if (ec) {
        error = "cannot stat context JSONL: ";
        error += kimix::to_string(fs_jsonl);
        return false;
    }
    FILE *f = nullptr;
    {
        const kimix::string narrow = kimix::to_string(fs_jsonl);
        f = std::fopen(narrow.c_str(), "rb");
        if (f == nullptr) {
            error = "cannot open context JSONL: " + narrow;
            return false;
        }
    }
    kimix::string text;
    text.resize(static_cast<size_t>(file_size));
    const size_t got = fread(text.data(), 1, text.size(), f);
    fclose(f);
    text.resize(got);

    if (!begin_transaction(error)) {
        return false;
    }
    bool ok = true;
    int64_t imported_messages = 0;
    _last_message_rowid = 0;
    {
        const char *cursor = text.data();
        const char *end = cursor + text.size();
        while (cursor < end && ok) {
            const char *eol = cursor;
            while (eol < end && *eol != '\n') {
                ++eol;
            }
            kimix::string_view line(cursor, static_cast<size_t>(eol - cursor));
            // Trim like the reference's `if not line.strip(): continue`.
            size_t b = 0, e2 = line.size();
            while (b < e2 && (line[b] == ' ' || line[b] == '\t' || line[b] == '\r')) {
                ++b;
            }
            while (e2 > b && (line[e2 - 1] == ' ' || line[e2 - 1] == '\t' ||
                              line[e2 - 1] == '\r')) {
                --e2;
            }
            line = line.substr(b, e2 - b);
            cursor = eol < end ? eol + 1 : end;
            if (line.empty()) {
                continue;
            }
            yyjson_doc *doc = cxdb_parse_lenient(line);
            if (doc == nullptr) {
                continue;// unparseable even after repair: skip (reference parity)
            }
            const yyjson_val *root = yyjson_doc_get_root(doc);
            if (!yyjson_is_obj(root)) {
                yyjson_doc_free(doc);
                continue;
            }
            const kimix::string role = cxdb_get_str(root, "role");
            if (role.empty()) {
                yyjson_doc_free(doc);
                continue;
            }
            if (role == "_system_prompt") {
                const kimix::string content = cxdb_get_str(root, "content");
                sqlite3_stmt *stmt = nullptr;
                if (prepare("INSERT OR REPLACE INTO system_prompt (id, content, "
                            "updated_at) VALUES (1, ?, unixepoch())",
                            stmt, error)) {
                    sqlite3_bind_text(stmt, 1, content.data(),
                                      static_cast<int>(content.size()), SQLITE_TRANSIENT);
                    ok = sqlite3_step(stmt) == SQLITE_DONE;
                    sqlite3_finalize(stmt);
                } else {
                    ok = false;
                }
            } else if (role == "_usage") {
                const int64_t token_count = cxdb_get_int(root, "token_count", 0);
                sqlite3_stmt *stmt = nullptr;
                if (prepare("INSERT INTO usage_snapshots (token_count) VALUES (?)",
                            stmt, error)) {
                    sqlite3_bind_int64(stmt, 1, token_count);
                    ok = sqlite3_step(stmt) == SQLITE_DONE;
                    sqlite3_finalize(stmt);
                } else {
                    ok = false;
                }
            } else if (role == "_checkpoint") {
                const int64_t cpid = cxdb_get_int(root, "id", 0);
                sqlite3_stmt *stmt = nullptr;
                if (prepare("INSERT INTO checkpoints (id, message_rowid) VALUES (?, ?)",
                            stmt, error)) {
                    sqlite3_bind_int64(stmt, 1, cpid);
                    sqlite3_bind_int64(stmt, 2, _last_message_rowid);
                    ok = sqlite3_step(stmt) == SQLITE_DONE;
                    sqlite3_finalize(stmt);
                } else {
                    ok = false;
                }
            } else {
                // Message record: store the re-serialized line (orjson.dumps
                // parity) plus its extracted searchable text.
                const kimix::string content = cxdb_compact_val(root);
                const kimix::string text_extracted =
                    cxdb_extract_text(cxdb_member(root, "content"));
                sqlite3_stmt *stmt = nullptr;
                if (prepare("INSERT INTO messages (role, content, content_text) "
                            "VALUES (?, ?, ?)",
                            stmt, error)) {
                    sqlite3_bind_text(stmt, 1, role.data(),
                                      static_cast<int>(role.size()), SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 2, content.data(),
                                      static_cast<int>(content.size()), SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 3, text_extracted.data(),
                                      static_cast<int>(text_extracted.size()),
                                      SQLITE_TRANSIENT);
                    ok = sqlite3_step(stmt) == SQLITE_DONE;
                    sqlite3_finalize(stmt);
                } else {
                    ok = false;
                }
                if (ok) {
                    _last_message_rowid = sqlite3_last_insert_rowid(_db);
                    ++imported_messages;
                }
            }
            yyjson_doc_free(doc);
        }
    }
    if (ok) {
        ok = fix_checkpoint_message_rowids(error);
    }
    if (ok) {
        ok = commit_transaction(error);
    } else {
        kimix::string rollback_error;
        rollback_transaction(rollback_error);
    }
    if (!ok) {
        // session.py: partial-DB cleanup when this call created the file.
        if (we_created) {
            close();
            std::error_code remove_ec;
            kimix::filesystem::remove(db_path, remove_ec);
            kimix::filesystem::remove(
                kimix::filesystem::path(kimix::to_string(db_path) + "-wal"), remove_ec);
            kimix::filesystem::remove(
                kimix::filesystem::path(kimix::to_string(db_path) + "-shm"), remove_ec);
        }
        if (error.empty()) {
            error = "context migration failed";
        }
        return false;
    }
    // session.py row-count verification: every imported line became a row
    // (meta records map onto their own tables, so compare against messages).
    int64_t db_count = 0;
    if (!message_count(db_count, error)) {
        return false;
    }
    if (db_count != imported_messages) {
        error = kimix::format("migration verification failed: JSONL messages={} DB "
                              "messages={}",
                              imported_messages, db_count);
        return false;
    }
    // Success: back the JSONL up like session.py's .jsonl.bak rename.
    kimix::filesystem::path backup = jsonl_path;
    backup.replace_extension(".jsonl.bak");
    std::error_code rename_ec;
    kimix::filesystem::rename(jsonl_path, backup, rename_ec);
    if (rename_ec) {
        // The import is durable; a leftover JSONL must not fail the migration.
        std::error_code remove_ec;
        kimix::filesystem::remove(jsonl_path, remove_ec);
    }
    migrated = true;
    return true;
}

} // namespace kimix::agent
