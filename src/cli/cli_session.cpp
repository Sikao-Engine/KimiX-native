// cli/cli_session.cpp - implementation of the native CLI's session store.
//
// Reference sources (read-only, kimi-agent):
//   kimi_cli/session.py (Session.create/find/list/copy, dir, refresh, is_empty)
//   kimi_cli/session_state.py (SessionState + load/save_session_state)
//   kimi_cli/utils/io.py (atomic_json_write: tempfile.mkstemp + os.replace)
//   kimi_cli/soul/context.py (JsonlContextStorage: append_messages/record_usage)
//   kimi_cli/wire/file.py + wire/types.py (wire.jsonl header + records)
//   kimi_cli/utils/export.py (build_export_markdown, reduced: see cli_session.h)
//   kimi_cli/metadata.py (KIMIX_CACHE_DIR_NAME), kimi_cli/utils/string.py (shorten)
//   kimix/utils/_globals.py (_refresh_cli_sessions: title + updated_at scan)
//
// Exception-free by construction: every filesystem call uses the std::error_code
// overloads and narrow->path conversion goes through kimix::path_from_narrow.
// Unity build note: TU-local helpers live in an anonymous namespace with the
// `clis_` prefix (no file-scope `using namespace`).

#include "cli/cli_session.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <system_error>

#include "llm/yyjson_alc.h"
#include "yyjson.h"

#include "cli/cli_common.h"

namespace kimix::cli {

namespace {

// File names inside a session directory (kimi_cli/session.py + session_state.py
// STATE_FILE_NAME + wire/file.py).
constexpr const char *kClisStateFile = "state.json";
constexpr const char *kClisContextFile = "context.jsonl";
constexpr const char *kClisWireFile = "wire.jsonl";
constexpr const char *kClisContextDbFile = "context.db";
constexpr const char *kClisCacheDir = ".kimix_cache";
// kimi_cli/wire/protocol.py::WIRE_PROTOCOL_VERSION.
constexpr const char *kClisWireProtocolVersion = "1.11";

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

bool clis_to_path(kimix::string_view text, kimix::filesystem::path &out) {
    return kimix::path_from_narrow(text, out);
}

// Best-effort single file delete (a missing file is success).
void clis_remove_file(const kimix::string &path) {
    kimix::filesystem::path p;
    if (!clis_to_path(path, p)) {
        return;
    }
    std::error_code ec;
    kimix::filesystem::remove(p, ec);
}

// Every file the store owns inside a session directory: state.json,
// context.jsonl (legacy history), wire.jsonl and the SQLite context.db with its
// journals, plus the temporary siblings the atomic writers leave behind on a
// crash.  Unrelated files (subagents/, tasks/, ...) are never touched.
void clis_remove_owned_files(const kimix::string &dir,
                             kimix::vector<kimix::string> &removed) {
    static const char *const kNames[] = {
        kClisStateFile,      kClisContextFile,     kClisWireFile,
        kClisContextDbFile,  "context.db-wal",     "context.db-shm",
        "context.jsonl.bak", "state.json.tmp",     "context.jsonl.tmp",
        "wire.jsonl.tmp"};
    for (const char *name : kNames) {
        const kimix::string path = join_path(dir, name);
        if (file_exists(path)) {
            clis_remove_file(path);
            removed.push_back(kimix::string(name));
        }
    }
}

// Recursive copy of `src` into `dst` (created when missing), overwriting
// existing files: shutil.copytree's body without its "target must not exist"
// precondition - the caller checks that (store_as) or owns the target
// (copy_into).
bool clis_copy_tree(const kimix::string &src, const kimix::string &dst,
                    kimix::string &error) {
    if (!dir_exists(src)) {
        error = "source session directory not found: " + src;
        return false;
    }
    if (!make_dirs(dst, error)) {
        return false;
    }
    kimix::filesystem::path root;
    kimix::filesystem::path target;
    if (!clis_to_path(src, root) || !clis_to_path(dst, target)) {
        error = "unrepresentable session directory: " + src;
        return false;
    }
    std::error_code ec;
    kimix::filesystem::directory_iterator it(root, ec);
    if (ec) {
        error = "cannot list session directory: " + src;
        return false;
    }
    const kimix::filesystem::directory_iterator end;
    while (it != end) {
        const kimix::filesystem::directory_entry entry = *it;
        std::error_code entry_ec;
        const bool is_dir = entry.is_directory(entry_ec);
        const kimix::string name = kimix::to_string(entry.path().filename());
        const kimix::string to = join_path(dst, name);
        if (is_dir) {
            if (!clis_copy_tree(join_path(src, name), to, error)) {
                return false;
            }
        } else if (entry.is_regular_file(entry_ec)) {
            std::error_code copy_ec;
            kimix::filesystem::copy_file(
                entry.path(), kimix::filesystem::path(kimix::string(to)),
                kimix::filesystem::copy_options::overwrite_existing, copy_ec);
            if (copy_ec) {
                error = "cannot copy " + name + " into " + dst;
                return false;
            }
        }
        it.increment(ec);
        if (ec) {
            error = "cannot list session directory: " + src;
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// JSON output helpers (orjson-compatible compact + OPT_INDENT_2 pretty)
// ---------------------------------------------------------------------------

// orjson/Python string escaping: \" \\ \b \f \n \r \t and \u00XX for the other
// control bytes; raw UTF-8 otherwise (kimi_cli writes UTF-8 JSON).
void clis_escape(kimix::string &out, kimix::string_view s) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20u) {
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0x0Fu]);
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
}

void clis_escape_quoted(kimix::string &out, kimix::string_view s) {
    out += '"';
    clis_escape(out, s);
    out += '"';
}

// Decimal integer append (std::to_chars into a stack buffer, so no std::string
// with a foreign allocator is ever involved).
void clis_append_int(kimix::string &out, int64_t value) {
    char buf[24];
    const auto res = std::to_chars(buf, buf + sizeof(buf), value);
    out.append(buf, static_cast<size_t>(res.ptr - buf));
}

// Float rendering matching orjson/Python's shortest representation (Ryu's
// d2s rule: fixed notation while -5 <= decimal exponent <= 15, scientific
// beyond, no zero-padded exponent), with the ".0" marker for integral values.
// std::to_chars(general) alone is not enough: it switches to scientific for
// values orjson writes in fixed notation (e.g. 1712345678.5).
void clis_real(kimix::string &out, double value) {
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), value,
                                   std::chars_format::scientific);
    if (res.ec != std::errc()) {
        out += "0.0"; // inf/nan are not representable in JSON
        return;
    }
    const size_t len = static_cast<size_t>(res.ptr - buf);
    size_t i = 0;
    bool negative = false;
    if (i < len && buf[i] == '-') {
        negative = true;
        ++i;
    }
    if (i >= len || buf[i] < '0' || buf[i] > '9') {
        out += "0.0"; // inf / nan
        return;
    }
    kimix::string digits;
    while (i < len && buf[i] != 'e' && buf[i] != 'E') {
        if (buf[i] != '.') {
            digits.push_back(buf[i]);
        }
        ++i;
    }
    if (i >= len || digits.empty()) {
        out += "0.0";
        return;
    }
    ++i; // 'e'
    bool exp_negative = false;
    if (i < len && (buf[i] == '+' || buf[i] == '-')) {
        exp_negative = buf[i] == '-';
        ++i;
    }
    int32_t exp10 = 0;
    for (; i < len; ++i) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            exp10 = exp10 * 10 + (buf[i] - '0');
        }
    }
    if (exp_negative) {
        exp10 = -exp10;
    }
    if (negative) {
        out.push_back('-');
    }
    if (exp10 >= -5 && exp10 <= 15) {
        // Fixed notation.
        if (exp10 >= 0) {
            const size_t whole = static_cast<size_t>(exp10) + 1;
            for (size_t d = 0; d < whole; ++d) {
                out.push_back(d < digits.size() ? digits[d] : '0');
            }
            if (digits.size() > whole) {
                out.push_back('.');
                out += digits.substr(whole);
            } else {
                out += ".0";
            }
        } else {
            out += "0.";
            for (int32_t z = 0; z < -exp10 - 1; ++z) {
                out.push_back('0');
            }
            out += digits;
        }
        return;
    }
    // Scientific notation: d[.ddd]e{+,-}N (orjson pads nothing).
    out.push_back(digits[0]);
    if (digits.size() > 1) {
        out.push_back('.');
        out += digits.substr(1);
    }
    out.push_back('e');
    out += exp10 < 0 ? "-" : "+";
    clis_append_int(out, exp10 < 0 ? -static_cast<int64_t>(exp10)
                                   : static_cast<int64_t>(exp10));
}

// orjson OPT_INDENT_2-compatible pretty printer (2 spaces per level, ": "
// separator, empty containers inline).  Mirrors runtime/common/json_pretty.h;
// kept local because that header pulls kimix::runtime::common::utf8 symbols
// which live in the runtime module, not in this static library.
void clis_pretty(const yyjson_val *val, size_t level, kimix::string &out) {
    if (val == nullptr) {
        out += "null";
        return;
    }
    switch (yyjson_get_type(val)) {
    case YYJSON_TYPE_OBJ: {
        if (yyjson_obj_size(val) == 0) {
            out += "{}";
            return;
        }
        out += "{\n";
        size_t idx = 0;
        size_t max = 0;
        yyjson_val *key = nullptr;
        yyjson_val *item = nullptr;
        yyjson_obj_foreach(val, idx, max, key, item) {
            out.append((level + 1) * 2, ' ');
            clis_escape_quoted(
                out, kimix::string_view(yyjson_get_str(key),
                                        static_cast<size_t>(yyjson_get_len(key))));
            out += ": ";
            clis_pretty(item, level + 1, out);
            if (idx + 1 < max) {
                out += ',';
            }
            out += '\n';
        }
        out.append(level * 2, ' ');
        out += '}';
        return;
    }
    case YYJSON_TYPE_ARR: {
        if (yyjson_arr_size(val) == 0) {
            out += "[]";
            return;
        }
        out += "[\n";
        size_t idx = 0;
        size_t max = 0;
        yyjson_val *item = nullptr;
        yyjson_arr_foreach(val, idx, max, item) {
            out.append((level + 1) * 2, ' ');
            clis_pretty(item, level + 1, out);
            if (idx + 1 < max) {
                out += ',';
            }
            out += '\n';
        }
        out.append(level * 2, ' ');
        out += ']';
        return;
    }
    case YYJSON_TYPE_STR:
        clis_escape_quoted(
            out, kimix::string_view(yyjson_get_str(val),
                                    static_cast<size_t>(yyjson_get_len(val))));
        return;
    case YYJSON_TYPE_NUM:
        if (yyjson_is_uint(val)) {
            char buf[24];
            const auto res = std::to_chars(buf, buf + sizeof(buf),
                                           yyjson_get_uint(val));
            out.append(buf, static_cast<size_t>(res.ptr - buf));
        } else if (yyjson_is_sint(val)) {
            char buf[24];
            const auto res = std::to_chars(buf, buf + sizeof(buf),
                                           yyjson_get_sint(val));
            out.append(buf, static_cast<size_t>(res.ptr - buf));
        } else {
            clis_real(out, yyjson_get_real(val));
        }
        return;
    case YYJSON_TYPE_BOOL:
        out += yyjson_is_true(val) ? "true" : "false";
        return;
    default:
        out += "null";
        return;
    }
}

// Parse `text` with the mimalloc allocator; the caller frees the result with
// yyjson_doc_free.  Null when the text is empty or not valid JSON.
yyjson_doc *clis_parse(kimix::string_view text) {
    if (text.empty()) {
        return nullptr;
    }
    return yyjson_read_opts(const_cast<char *>(text.data()), text.size(),
                            0 /* strict, stop on error */, &kimix::llm::kYYJsonAlcMi,
                            nullptr);
}

// Compact JSON of one parsed value (used for the todos_json round-trip).
kimix::string clis_compact_val(const yyjson_val *val) {
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

// Compact JSON of a mutable document (one JSONL record).
kimix::string clis_compact_doc(yyjson_mut_doc *doc) {
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

// Adds one member with a NUL-safe key (yyjson_mut_obj_add_val takes a
// NUL-terminated key, which would silently truncate a key carrying an embedded
// NUL - "a\u0000b" parses into exactly such a key).
bool clis_obj_add(yyjson_mut_doc *doc, yyjson_mut_val *obj, kimix::string_view key,
                  yyjson_mut_val *val) {
    if (obj == nullptr || val == nullptr) {
        return false;
    }
    yyjson_mut_val *key_val = yyjson_mut_strncpy(doc, key.data(), key.size());
    return key_val != nullptr && yyjson_mut_obj_add(obj, key_val, val);
}

bool clis_obj_add_str(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                      kimix::string_view key, kimix::string_view value) {
    return clis_obj_add(doc, obj, key,
                        yyjson_mut_strncpy(doc, value.data(), value.size()));
}

bool clis_obj_add_bool(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                       kimix::string_view key, bool value) {
    return clis_obj_add(doc, obj, key, yyjson_mut_bool(doc, value));
}

bool clis_obj_add_int(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                      kimix::string_view key, int64_t value) {
    return clis_obj_add(doc, obj, key, yyjson_mut_sint(doc, value));
}

// `null` when `has` is false, the string otherwise (the reference's
// `custom_title: str | None`).
bool clis_obj_add_str_or_null(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                              kimix::string_view key, bool has,
                              kimix::string_view value) {
    return clis_obj_add(doc, obj, key,
                        has ? yyjson_mut_strncpy(doc, value.data(), value.size())
                            : yyjson_mut_null(doc));
}

// Copy of a parsed (immutable) value into a mutable document; null when absent.
yyjson_mut_val *clis_mut_copy(yyjson_mut_doc *doc, const yyjson_val *val) {
    if (doc == nullptr || val == nullptr) {
        return nullptr;
    }
    return yyjson_val_mut_copy(doc, val);
}

// Reads a member of a parsed object (null when the object/member is absent).
const yyjson_val *clis_member(const yyjson_val *obj, kimix::string_view key) {
    if (obj == nullptr || !yyjson_is_obj(obj) || key.empty()) {
        return nullptr;
    }
    return yyjson_obj_getn(obj, key.data(), key.size());
}

bool clis_has_member(const yyjson_val *obj, kimix::string_view key) {
    return clis_member(obj, key) != nullptr;
}

bool clis_has_mut_member(const yyjson_mut_val *obj, kimix::string_view key) {
    if (obj == nullptr || key.empty()) {
        return false;
    }
    return yyjson_mut_obj_getn(obj, key.data(), key.size()) != nullptr;
}

bool clis_get_bool(const yyjson_val *obj, kimix::string_view key, bool fallback) {
    const yyjson_val *v = clis_member(obj, key);
    if (v == nullptr || !yyjson_is_bool(v)) {
        return fallback;
    }
    return yyjson_is_true(v);
}

int64_t clis_get_int(const yyjson_val *obj, kimix::string_view key,
                     int64_t fallback) {
    const yyjson_val *v = clis_member(obj, key);
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

double clis_get_real(const yyjson_val *obj, kimix::string_view key,
                     double fallback) {
    const yyjson_val *v = clis_member(obj, key);
    if (v == nullptr || !yyjson_is_num(v)) {
        return fallback;
    }
    if (yyjson_is_real(v)) {
        return yyjson_get_real(v);
    }
    if (yyjson_is_uint(v)) {
        return static_cast<double>(yyjson_get_uint(v));
    }
    return static_cast<double>(yyjson_get_sint(v));
}

kimix::string clis_get_str(const yyjson_val *obj, kimix::string_view key) {
    const yyjson_val *v = clis_member(obj, key);
    if (v == nullptr || !yyjson_is_str(v)) {
        return {};
    }
    return kimix::string(yyjson_get_str(v), static_cast<size_t>(yyjson_get_len(v)));
}

// Adds `key` with a copy of the old document's member, or `fallback` (null
// means the JSON null literal) when the member is absent.
void clis_add_preserved(yyjson_mut_doc *doc, yyjson_mut_val *root,
                        kimix::string_view key, const yyjson_val *old_root,
                        yyjson_mut_val *fallback) {
    const yyjson_val *old = clis_member(old_root, key);
    clis_obj_add(doc, root, key,
                 old != nullptr ? clis_mut_copy(doc, old)
                                : (fallback != nullptr ? fallback
                                                       : yyjson_mut_null(doc)));
}

// The keys save_state() owns; everything else in an existing file is copied
// through verbatim.  `context_usage` / `context_tokens` are the native store's
// usage bookkeeping (the reference keeps usage in memory only): they are
// written when set_usage() is known and preserved otherwise.
bool clis_is_managed_key(kimix::string_view key) {
    static const char *const kKeys[] = {
        "version",      "approval",           "additional_dirs",
        "custom_title", "title_generated",    "title_generate_attempts",
        "wire_mtime",   "archived",           "archived_at",
        "auto_archive_exempt", "todos",       "archived_todos",
        "todo_stack",   "context_usage",      "context_tokens"};
    for (const char *candidate : kKeys) {
        if (key == kimix::string_view(candidate)) {
            return true;
        }
    }
    return false;
}

// Atomic write: <path>.tmp first, then the replace (atomic_json_write's
// tempfile + os.replace).  Windows refuses a rename onto an existing file, so
// the target is removed and the rename retried; a leftover tmp file is removed
// when the write fails.
bool clis_atomic_write(const kimix::string &path, kimix::string_view text,
                       kimix::string &error) {
    error.clear();
    const kimix::string tmp = path + ".tmp";
    if (!write_file(tmp, text, error)) {
        return false;
    }
    kimix::filesystem::path from;
    kimix::filesystem::path to;
    if (!clis_to_path(tmp, from) || !clis_to_path(path, to)) {
        error = "unrepresentable path: " + path;
        clis_remove_file(tmp);
        return false;
    }
    std::error_code ec;
    kimix::filesystem::rename(from, to, ec);
    if (ec) {
        // Windows: std::filesystem::rename does not replace an existing file.
        std::error_code remove_ec;
        kimix::filesystem::remove(to, remove_ec);
        kimix::filesystem::rename(from, to, ec);
        if (ec) {
            clis_remove_file(tmp);
            const std::string detail = ec.message();
            error = "cannot replace " + path + ": " +
                    kimix::string(detail.data(), detail.size());
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// History records (kimi_cli/soul/context.py JsonlContextStorage)
// ---------------------------------------------------------------------------

// One part of a message content array.  kind: 0 text, 1 think.
struct clis_part {
    int32_t kind = 0;
    kimix::string text;
    kimix::string encrypted; // ThinkPart.encrypted (the thinking signature)
};

// Rebuilt message view: the reference stores content as a bare string when the
// message holds exactly one text part and as a part array otherwise
// (Message._serialize_content), with tool_calls / tool_call_id present only
// when they are not None (model_dump_json(exclude_none=True)).
struct clis_msg_view {
    kimix::string role;
    kimix::vector<clis_part> parts;
    kimix::vector<kimix::llm::ToolCall> tool_calls;
    kimix::string tool_call_id;
};

void clis_parse_parts(const yyjson_val *content, clis_msg_view &view) {
    if (content == nullptr || yyjson_is_null(content)) {
        return;
    }
    if (yyjson_is_str(content)) {
        clis_part part;
        part.kind = 0;
        part.text.assign(yyjson_get_str(content),
                         static_cast<size_t>(yyjson_get_len(content)));
        view.parts.push_back(std::move(part));
        return;
    }
    if (!yyjson_is_arr(content)) {
        return;
    }
    size_t idx = 0;
    size_t max = 0;
    yyjson_val *item = nullptr;
    yyjson_arr_foreach(content, idx, max, item) {
        if (!yyjson_is_obj(item)) {
            continue;
        }
        const kimix::string type = clis_get_str(item, "type");
        clis_part part;
        if (type == "think") {
            part.kind = 1;
            part.text = clis_get_str(item, "think");
            part.encrypted = clis_get_str(item, "encrypted");
        } else if (type == "text") {
            part.kind = 0;
            part.text = clis_get_str(item, "text");
        } else {
            continue; // media / unknown parts cannot be represented natively
        }
        view.parts.push_back(std::move(part));
    }
}

bool clis_parse_message_record(const yyjson_val *root, clis_msg_view &view) {
    if (root == nullptr || !yyjson_is_obj(root)) {
        return false;
    }
    const kimix::string role = clis_get_str(root, "role");
    if (role != "system" && role != "user" && role != "assistant" &&
        role != "tool") {
        return false; // meta records (_system_prompt/_usage/_checkpoint) + junk
    }
    view.role = role;
    clis_parse_parts(clis_member(root, "content"), view);
    const yyjson_val *calls = clis_member(root, "tool_calls");
    if (calls != nullptr && yyjson_is_arr(calls)) {
        size_t idx = 0;
        size_t max = 0;
        yyjson_val *item = nullptr;
        yyjson_arr_foreach(calls, idx, max, item) {
            if (!yyjson_is_obj(item)) {
                continue;
            }
            kimix::llm::ToolCall call;
            call.type = "function";
            call.id = clis_get_str(item, "id");
            const yyjson_val *fn = clis_member(item, "function");
            if (fn != nullptr && yyjson_is_obj(fn)) {
                call.name = clis_get_str(fn, "name");
                call.arguments = clis_get_str(fn, "arguments");
            }
            view.tool_calls.push_back(std::move(call));
        }
    }
    view.tool_call_id = clis_get_str(root, "tool_call_id");
    return true;
}

// Serialize one message as the reference's JSONL record.
void clis_write_message(yyjson_mut_doc *doc, const kimix::llm::Message &msg,
                        kimix::string_view content_key) {
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    clis_obj_add_str(doc, root, "role", msg.role);

    const bool has_think =
        !msg.thinking.empty() || !msg.thinking_signature.empty();
    if (has_think) {
        yyjson_mut_val *parts = yyjson_mut_arr(doc);
        yyjson_mut_val *think = yyjson_mut_obj(doc);
        clis_obj_add_str(doc, think, "type", "think");
        clis_obj_add_str(doc, think, "think", msg.thinking);
        if (!msg.thinking_signature.empty()) {
            clis_obj_add_str(doc, think, "encrypted", msg.thinking_signature);
        }
        yyjson_mut_arr_add_val(parts, think);
        if (!msg.content.empty()) {
            yyjson_mut_val *text = yyjson_mut_obj(doc);
            clis_obj_add_str(doc, text, "type", "text");
            clis_obj_add_str(doc, text, "text", msg.content);
            yyjson_mut_arr_add_val(parts, text);
        }
        clis_obj_add(doc, root, content_key, parts);
    } else if (msg.content.empty()) {
        // The reference stores content as a lone (possibly empty) text part as a
        // bare string; a message built with an empty part list stays [].
        if (msg.tool_calls.empty()) {
            clis_obj_add_str(doc, root, content_key, kimix::string_view());
        } else {
            clis_obj_add(doc, root, content_key, yyjson_mut_arr(doc));
        }
    } else {
        clis_obj_add_str(doc, root, content_key, msg.content);
    }

    if (!msg.tool_calls.empty()) {
        yyjson_mut_val *calls = yyjson_mut_arr(doc);
        for (const kimix::llm::ToolCall &call : msg.tool_calls) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            clis_obj_add_str(doc, item, "type",
                             call.type.empty() ? kimix::string_view("function")
                                               : kimix::string_view(call.type));
            clis_obj_add_str(doc, item, "id", call.id);
            yyjson_mut_val *fn = yyjson_mut_obj(doc);
            clis_obj_add_str(doc, fn, "name", call.name);
            if (!call.arguments.empty()) {
                clis_obj_add_str(doc, fn, "arguments", call.arguments);
            }
            clis_obj_add(doc, item, "function", fn);
            yyjson_mut_arr_add_val(calls, item);
        }
        clis_obj_add(doc, root, "tool_calls", calls);
    }
    if (!msg.tool_call_id.empty()) {
        clis_obj_add_str(doc, root, "tool_call_id", msg.tool_call_id);
    }
}

// One wire record: {"timestamp": <float>, "message": {"type": <name>,
// "payload": {...}}} (wire/file.py _dump_line + wire/types.py
// WireMessageEnvelope.from_wire_message).  The payload is built inside the
// record's own document so its lifetime is unambiguous.
void clis_wire_emit(
    kimix::string_view type, double ts, kimix::string &out,
    const kimix::function<void(yyjson_mut_doc *, yyjson_mut_val *)> &build) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    if (doc == nullptr) {
        return;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    clis_obj_add(doc, root, "timestamp", yyjson_mut_real(doc, ts));
    yyjson_mut_val *message = yyjson_mut_obj(doc);
    clis_obj_add_str(doc, message, "type", type);
    yyjson_mut_val *payload = yyjson_mut_obj(doc);
    build(doc, payload);
    clis_obj_add(doc, message, "payload", payload);
    clis_obj_add(doc, root, "message", message);
    out += clis_compact_doc(doc);
    out += '\n';
    yyjson_mut_doc_free(doc);
}

// The CLI's transcript for one history message.  Only the parts a native
// message can carry are emitted (text / thinking / tool calls / tool results) -
// the reference's wire stream additionally carries step, status, hook and
// approval records the native CLI does not produce yet.
void clis_wire_records(const kimix::llm::Message &msg, double ts,
                       kimix::string &out) {
    if (msg.role == "tool") {
        clis_wire_emit("ToolResult", ts, out,
                       [&msg](yyjson_mut_doc *doc, yyjson_mut_val *payload) {
                           clis_obj_add_str(doc, payload, "tool_call_id",
                                            msg.tool_call_id);
                           yyjson_mut_val *value = yyjson_mut_obj(doc);
                           clis_obj_add_bool(doc, value, "is_error", false);
                           clis_obj_add_str(doc, value, "output", msg.content);
                           clis_obj_add_str(doc, value, "message", "");
                           clis_obj_add(doc, value, "display", yyjson_mut_arr(doc));
                           clis_obj_add(doc, payload, "return_value", value);
                       });
        return;
    }
    if (msg.role == "user") {
        clis_wire_emit("TurnBegin", ts, out,
                       [&msg](yyjson_mut_doc *doc, yyjson_mut_val *payload) {
                           if (msg.thinking.empty() &&
                               msg.thinking_signature.empty()) {
                               clis_obj_add_str(doc, payload, "user_input",
                                                msg.content);
                               return;
                           }
                           yyjson_mut_val *parts = yyjson_mut_arr(doc);
                           yyjson_mut_val *think = yyjson_mut_obj(doc);
                           clis_obj_add_str(doc, think, "type", "think");
                           clis_obj_add_str(doc, think, "think", msg.thinking);
                           yyjson_mut_arr_add_val(parts, think);
                           if (!msg.content.empty()) {
                               yyjson_mut_val *text = yyjson_mut_obj(doc);
                               clis_obj_add_str(doc, text, "type", "text");
                               clis_obj_add_str(doc, text, "text", msg.content);
                               yyjson_mut_arr_add_val(parts, text);
                           }
                           clis_obj_add(doc, payload, "user_input", parts);
                       });
        return;
    }
    // assistant / system: content parts, then one ToolCall record per call.
    if (!msg.thinking.empty() || !msg.thinking_signature.empty()) {
        clis_wire_emit("ThinkPart", ts, out,
                       [&msg](yyjson_mut_doc *doc, yyjson_mut_val *payload) {
                           clis_obj_add_str(doc, payload, "type", "think");
                           clis_obj_add_str(doc, payload, "think", msg.thinking);
                           if (!msg.thinking_signature.empty()) {
                               clis_obj_add_str(doc, payload, "encrypted",
                                                msg.thinking_signature);
                           }
                       });
    }
    if (!msg.content.empty()) {
        clis_wire_emit("TextPart", ts, out,
                       [&msg](yyjson_mut_doc *doc, yyjson_mut_val *payload) {
                           clis_obj_add_str(doc, payload, "type", "text");
                           clis_obj_add_str(doc, payload, "text", msg.content);
                       });
    }
    for (const kimix::llm::ToolCall &call : msg.tool_calls) {
        clis_wire_emit("ToolCall", ts, out,
                       [&call](yyjson_mut_doc *doc, yyjson_mut_val *payload) {
                           clis_obj_add_str(doc, payload, "type", "function");
                           clis_obj_add_str(doc, payload, "id", call.id);
                           yyjson_mut_val *fn = yyjson_mut_obj(doc);
                           clis_obj_add_str(doc, fn, "name", call.name);
                           clis_obj_add_str(doc, fn, "arguments",
                                            call.arguments);
                           clis_obj_add(doc, payload, "function", fn);
                       });
    }
}

// ---------------------------------------------------------------------------
// Text helpers (kimi_cli/utils/string.py::shorten)
// ---------------------------------------------------------------------------

size_t clis_cp_count(kimix::string_view text) {
    size_t count = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0u) != 0x80u) {
            ++count;
        }
    }
    return count;
}

// Byte offset of code point `index` (== size when index is past the end).
size_t clis_cp_offset(kimix::string_view text, size_t index) {
    size_t cp = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0u) != 0x80u) {
            if (cp == index) {
                return i;
            }
            ++cp;
        }
    }
    return text.size();
}

bool clis_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

// Python's " ".join(text.split()) for ASCII whitespace (the reference also
// collapses Unicode spaces there; the CLI's inputs are overwhelmingly ASCII).
kimix::string clis_flat_space(kimix::string_view text) {
    kimix::string out;
    bool seen = false;
    bool pending = false;
    for (char c : text) {
        if (clis_ascii_space(c)) {
            if (seen) {
                pending = true;
            }
            continue;
        }
        if (pending) {
            out.push_back(' ');
            pending = false;
        }
        out.push_back(c);
        seen = true;
    }
    return out;
}

kimix::string clis_rstrip_ascii(kimix::string_view text) {
    size_t end = text.size();
    while (end > 0 && clis_ascii_space(text[end - 1])) {
        --end;
    }
    return kimix::string(text.substr(0, end));
}

// shorten(text, width=<width>, placeholder="\xE2\x80\xA6")
kimix::string clis_shorten(kimix::string_view text, size_t width) {
    const kimix::string flat = clis_flat_space(text);
    if (clis_cp_count(flat) <= width) {
        return flat;
    }
    const size_t placeholder = 1; // "\xE2\x80\xA6" is one code point
    if (width <= placeholder) {
        return kimix::string(flat.substr(0, clis_cp_offset(flat, width)));
    }
    const size_t cut = width - placeholder;
    // text.rfind(" ", 0, cut + 1): the last space inside the first cut+1 code
    // points.
    const size_t search_end = clis_cp_offset(flat, cut + 1);
    size_t space = kimix::string::npos;
    for (size_t i = search_end; i > 0; --i) {
        if (flat[i - 1] == ' ') {
            space = i - 1;
            break;
        }
    }
    if (space != kimix::string::npos && space > 0) {
        return kimix::string(flat.substr(0, space)) + "\xE2\x80\xA6";
    }
    return clis_rstrip_ascii(flat.substr(0, clis_cp_offset(flat, cut))) + "\xE2\x80\xA6";
}

// ---------------------------------------------------------------------------
// Export helpers (kimi_cli/utils/export.py)
// ---------------------------------------------------------------------------

// orjson's comma grouping in the Overview's token count ("{token_count:,}").
kimix::string clis_comma_group(int64_t value) {
    const bool negative = value < 0;
    uint64_t magnitude = negative ? static_cast<uint64_t>(-(value + 1)) + 1u
                                  : static_cast<uint64_t>(value);
    char digits[24];
    size_t len = 0;
    if (magnitude == 0) {
        digits[len++] = '0';
    }
    while (magnitude > 0) {
        digits[len++] = static_cast<char>('0' + (magnitude % 10u));
        magnitude /= 10u;
    }
    kimix::string out;
    if (negative) {
        out.push_back('-');
    }
    for (size_t i = len; i > 0; --i) {
        out.push_back(digits[i - 1]);
        const size_t remaining = i - 1;
        if (remaining > 0 && remaining % 3u == 0) {
            out.push_back(',');
        }
    }
    return out;
}

// _extract_tool_call_hint(args_json): the well-known keys first, then the first
// short string value, each shortened to 60.
kimix::string clis_tool_hint(kimix::string_view args_json) {
    yyjson_doc *doc = clis_parse(args_json);
    if (doc == nullptr) {
        return {};
    }
    kimix::string out;
    const yyjson_val *root = yyjson_doc_get_root(doc);
    if (yyjson_is_obj(root)) {
        static const char *const kHintKeys[] = {"path", "file_path", "command",
                                               "query", "url", "name",
                                               "pattern"};
        for (const char *key : kHintKeys) {
            const yyjson_val *v = yyjson_obj_get(root, key);
            if (v == nullptr || !yyjson_is_str(v)) {
                continue;
            }
            const kimix::string_view value(yyjson_get_str(v),
                                           static_cast<size_t>(yyjson_get_len(v)));
            if (!trim(value).empty()) {
                out = clis_shorten(value, 60);
                break;
            }
        }
        if (out.empty()) {
            size_t idx = 0;
            size_t max = 0;
            yyjson_val *key = nullptr;
            yyjson_val *item = nullptr;
            yyjson_obj_foreach(root, idx, max, key, item) {
                (void)key;
                if (item == nullptr || !yyjson_is_str(item)) {
                    continue;
                }
                const size_t len = static_cast<size_t>(yyjson_get_len(item));
                if (len == 0 || len > 80) {
                    continue;
                }
                out = clis_shorten(kimix::string_view(yyjson_get_str(item), len), 60);
                break;
            }
        }
    }
    yyjson_doc_free(doc);
    return out;
}

// _format_tool_call_md: "#### Tool Call: name (`hint`)" + the call-id comment +
// the arguments as OPT_INDENT_2 JSON in a fenced block.
void clis_tool_call_md(const kimix::llm::ToolCall &call, kimix::string &out) {
    const kimix::string_view args =
        call.arguments.empty() ? kimix::string_view("{}")
                               : kimix::string_view(call.arguments);
    const kimix::string hint = clis_tool_hint(args);
    out += "#### Tool Call: ";
    out += call.name;
    if (!hint.empty()) {
        out += " (`";
        out += hint;
        out += "`)";
    }
    out += "\n<!-- call_id: ";
    out += call.id;
    out += " -->\n```json\n";
    yyjson_doc *doc = clis_parse(args);
    if (doc != nullptr) {
        clis_pretty(yyjson_doc_get_root(doc), 0, out);
        yyjson_doc_free(doc);
    } else {
        out += call.arguments;
    }
    out += "\n```\n";
}

// _format_tool_result_md.
void clis_tool_result_md(kimix::string_view content, kimix::string_view call_id,
                         kimix::string_view name, kimix::string_view hint,
                         kimix::string &out) {
    out += "<details><summary>Tool Result: ";
    out += name.empty() ? kimix::string_view("unknown") : name;
    if (!hint.empty()) {
        out += " (`";
        out += hint;
        out += "`)";
    }
    out += "</summary>\n\n<!-- call_id: ";
    out += call_id.empty() ? kimix::string_view("unknown") : call_id;
    out += " -->\n";
    out += content;
    out += "\n\n</details>\n";
}

// One message -> one export section: "### <Role>" + the rendered body parts
// (thinking, text, tool calls / tool result).
void clis_message_md(
    const kimix::llm::Message &msg,
    const kimix::map<kimix::string, std::pair<kimix::string, kimix::string>>
        &tool_info,
    kimix::string &out) {
    out += "### ";
    out += msg.role == "user"          ? kimix::string_view("User")
           : msg.role == "assistant"   ? kimix::string_view("Assistant")
           : msg.role == "tool"        ? kimix::string_view("Tool")
                                       : kimix::string_view("System");
    out += "\n\n";
    if (!msg.thinking.empty()) {
        out += "<details><summary>Thinking</summary>\n\n";
        out += msg.thinking;
        out += "\n\n</details>\n\n";
    }
    if (msg.role == "tool") {
        kimix::string_view name = "unknown";
        kimix::string_view hint;
        const auto found = tool_info.find(msg.tool_call_id);
        if (found != tool_info.end()) {
            name = found->second.first;
            hint = found->second.second;
        }
        clis_tool_result_md(msg.content, msg.tool_call_id, name, hint, out);
        return;
    }
    if (!msg.content.empty()) {
        out += msg.content;
        out += "\n\n";
    }
    for (const kimix::llm::ToolCall &call : msg.tool_calls) {
        clis_tool_call_md(call, out);
        out += "\n";
    }
}

// ---------------------------------------------------------------------------
// Time helpers
// ---------------------------------------------------------------------------

// time.time(): a fractional unix timestamp (the reference's record timestamps).
double clis_now_seconds() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

int64_t clis_now_unix() {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    return static_cast<int64_t>(now);
}

// ISO-8601 with seconds precision (pendulum's isoformat(timespec="seconds")).
kimix::string clis_iso_seconds(int64_t unix_seconds) {
    return format_utc(unix_seconds, "%Y-%m-%dT%H:%M:%S");
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / open
// ---------------------------------------------------------------------------

session_store::session_store() = default;

kimix::string session_store::cache_root(const kimix::string &work_dir) {
    const kimix::string base =
        work_dir.empty() ? current_dir() : absolute_path(work_dir);
    if (base.empty()) {
        return {};
    }
    return join_path(base, kClisCacheDir);
}

kimix::string session_store::session_dir(const kimix::string &work_dir,
                                         kimix::string_view id) {
    const kimix::string root = cache_root(work_dir);
    if (root.empty() || id.empty()) {
        return {};
    }
    return join_path(root, id);
}

bool session_store::open(kimix::string_view work_dir, kimix::string_view id,
                         bool resume, kimix::string &error) {
    error.clear();
    const kimix::string base =
        work_dir.empty() ? current_dir() : absolute_path(work_dir);
    if (base.empty()) {
        error = "cannot resolve the session work directory";
        return false;
    }
    // Every local is built first: `id` may be a view into this store's state.
    const kimix::string root = join_path(base, kClisCacheDir);
    if (!make_dirs(root, error)) {
        return false;
    }
    const bool anonymous = id.empty();
    const kimix::string session_id =
        anonymous ? random_hex(16) : kimix::string(id);
    const kimix::string dir = join_path(root, session_id);
    if (!make_dirs(dir, error)) {
        return false;
    }
    if (!resume) {
        // Session.create: the directory contents this store owns are reset.
        // Nothing else in the directory is touched.
        kimix::vector<kimix::string> removed;
        clis_remove_owned_files(dir, removed);
    }
    _work_dir = base;
    _id = session_id;
    _dir = dir;
    _anonymous = anonymous;
    _open = true;
    _usage = 0.0;
    _usage_tokens = 0;
    _usage_known = false;
    return true;
}

// ---------------------------------------------------------------------------
// state.json
// ---------------------------------------------------------------------------

bool session_store::load_state(session_state &out, kimix::string &error) const {
    error.clear();
    out = session_state{};
    if (_dir.empty()) {
        error = "no session is open";
        return false;
    }
    const kimix::string path = join_path(_dir, kClisStateFile);
    if (!file_exists(path)) {
        return true; // the reference's defaults
    }
    kimix::string text;
    if (!read_file(path, text, error)) {
        return false;
    }
    if (trim(text).empty()) {
        return true; // an interrupted write: an empty file counts as absent
    }
    yyjson_doc *doc = clis_parse(text);
    const yyjson_val *root = doc != nullptr ? yyjson_doc_get_root(doc) : nullptr;
    if (doc == nullptr || root == nullptr || !yyjson_is_obj(root)) {
        if (doc != nullptr) {
            yyjson_doc_free(doc);
        }
        error = "corrupt state file (not a JSON object): " + path;
        return false;
    }
    out.custom_title = clis_get_str(root, "custom_title");
    out.title_generated = clis_get_bool(root, "title_generated", false);
    out.title_generate_attempts = static_cast<int32_t>(
        clis_get_int(root, "title_generate_attempts", 0));
    const yyjson_val *approval = clis_member(root, "approval");
    out.yolo = clis_get_bool(approval, "yolo", false);
    out.afk = clis_get_bool(approval, "afk", false);
    const yyjson_val *actions = clis_member(approval, "auto_approve_actions");
    if (actions != nullptr && yyjson_is_arr(actions)) {
        out.auto_approve_actions = yyjson_arr_size(actions) > 0;
    } else {
        out.auto_approve_actions =
            clis_get_bool(approval, "auto_approve_actions", false);
    }
    const yyjson_val *dirs = clis_member(root, "additional_dirs");
    if (dirs != nullptr && yyjson_is_arr(dirs)) {
        size_t idx = 0;
        size_t max = 0;
        yyjson_val *item = nullptr;
        yyjson_arr_foreach(dirs, idx, max, item) {
            if (yyjson_is_str(item)) {
                out.additional_dirs.push_back(kimix::string(
                    yyjson_get_str(item),
                    static_cast<size_t>(yyjson_get_len(item))));
            }
        }
    }
    out.archived = clis_get_bool(root, "archived", false);
    out.auto_archive_exempt = clis_get_bool(root, "auto_archive_exempt", false);
    const yyjson_val *todos = clis_member(root, "todos");
    if (todos != nullptr && yyjson_is_arr(todos)) {
        out.todos_json = clis_compact_val(todos);
    }
    yyjson_doc_free(doc);
    return true;
}

bool session_store::save_state(const session_state &st,
                               kimix::string &error) const {
    error.clear();
    if (_dir.empty()) {
        error = "no session is open";
        return false;
    }
    // todos_json must be a JSON array: it becomes state.json's "todos" member.
    yyjson_doc *todos_doc = clis_parse(st.todos_json);
    const yyjson_val *todos_val =
        todos_doc != nullptr ? yyjson_doc_get_root(todos_doc) : nullptr;
    if (todos_doc == nullptr || todos_val == nullptr ||
        !yyjson_is_arr(todos_val)) {
        if (todos_doc != nullptr) {
            yyjson_doc_free(todos_doc);
        }
        error = "todos_json is not a JSON array"; // state.json left untouched
        return false;
    }

    // The existing file is re-read so every key the native store does not model
    // survives the rewrite (the reference's save_state re-reads a subset of
    // these from disk for the same reason).
    const kimix::string path = join_path(_dir, kClisStateFile);
    kimix::string existing;
    kimix::string read_error;
    yyjson_doc *old = nullptr;
    if (file_exists(path) && read_file(path, existing, read_error)) {
        old = clis_parse(existing);
    }
    const yyjson_val *old_root =
        old != nullptr ? yyjson_doc_get_root(old) : nullptr;
    if (old_root != nullptr && !yyjson_is_obj(old_root)) {
        old_root = nullptr;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    if (doc == nullptr) {
        yyjson_doc_free(todos_doc);
        if (old != nullptr) {
            yyjson_doc_free(old);
        }
        error = "cannot allocate the state.json document";
        return false;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    // Reference key order: SessionState's declaration order in
    // kimi_cli/session_state.py.
    clis_add_preserved(doc, root, "version", old_root, yyjson_mut_sint(doc, 1));
    {
        yyjson_mut_val *approval = yyjson_mut_obj(doc);
        clis_obj_add_bool(doc, approval, "yolo", st.yolo);
        clis_obj_add_bool(doc, approval, "afk", st.afk);
        // The reference types this as set[str] (action names owned by Python);
        // the native bool must never be written as a JSON bool - pydantic would
        // reject the whole file - so the on-disk array is preserved.
        const yyjson_val *old_actions =
            clis_member(clis_member(old_root, "approval"), "auto_approve_actions");
        if (old_actions != nullptr && yyjson_is_arr(old_actions)) {
            clis_obj_add(doc, approval, "auto_approve_actions",
                         clis_mut_copy(doc, old_actions));
        } else {
            clis_obj_add(doc, approval, "auto_approve_actions",
                         yyjson_mut_arr(doc));
        }
        clis_obj_add(doc, root, "approval", approval);
    }
    {
        yyjson_mut_val *dirs = yyjson_mut_arr(doc);
        for (const kimix::string &dir : st.additional_dirs) {
            yyjson_mut_arr_add_strn(doc, dirs, dir.data(), dir.size());
        }
        clis_obj_add(doc, root, "additional_dirs", dirs);
    }
    clis_obj_add_str_or_null(doc, root, "custom_title", !st.custom_title.empty(),
                             st.custom_title);
    clis_obj_add_bool(doc, root, "title_generated", st.title_generated);
    clis_obj_add_int(doc, root, "title_generate_attempts",
                     st.title_generate_attempts);
    clis_add_preserved(doc, root, "wire_mtime", old_root, nullptr);
    clis_obj_add_bool(doc, root, "archived", st.archived);
    clis_add_preserved(doc, root, "archived_at", old_root, nullptr);
    clis_obj_add_bool(doc, root, "auto_archive_exempt", st.auto_archive_exempt);
    clis_obj_add(doc, root, "todos", yyjson_val_mut_copy(doc, todos_val));
    clis_add_preserved(doc, root, "archived_todos", old_root,
                       yyjson_mut_arr(doc));
    clis_add_preserved(doc, root, "todo_stack", old_root, yyjson_mut_arr(doc));
    if (_usage_known) {
        clis_obj_add(doc, root, "context_usage", yyjson_mut_real(doc, _usage));
        clis_obj_add_int(doc, root, "context_tokens", _usage_tokens);
    } else {
        // Preserved rather than dropped: a load -> save cycle must not lose the
        // usage another writer recorded.  Absent values stay absent (the
        // reference's state.json carries no usage keys at all).
        if (clis_has_member(old_root, "context_usage")) {
            clis_add_preserved(doc, root, "context_usage", old_root, nullptr);
        }
        if (clis_has_member(old_root, "context_tokens")) {
            clis_add_preserved(doc, root, "context_tokens", old_root, nullptr);
        }
    }
    // Every remaining member of the old document, verbatim.
    if (old_root != nullptr) {
        size_t idx = 0;
        size_t max = 0;
        yyjson_val *key = nullptr;
        yyjson_val *item = nullptr;
        yyjson_obj_foreach(old_root, idx, max, key, item) {
            const kimix::string_view key_view(
                yyjson_get_str(key), static_cast<size_t>(yyjson_get_len(key)));
            if (key_view.empty() || clis_is_managed_key(key_view) ||
                clis_has_mut_member(yyjson_mut_doc_get_root(doc), key_view)) {
                continue;
            }
            clis_obj_add(doc, yyjson_mut_doc_get_root(doc), key_view,
                         clis_mut_copy(doc, item));
        }
    }

    yyjson_doc *immutable =
        yyjson_mut_doc_imut_copy(doc, &kimix::llm::kYYJsonAlcMi);
    yyjson_mut_doc_free(doc);
    yyjson_doc_free(todos_doc);
    if (old != nullptr) {
        yyjson_doc_free(old);
    }
    if (immutable == nullptr) {
        error = "cannot serialize the state document";
        return false;
    }
    kimix::string text;
    clis_pretty(yyjson_doc_get_root(immutable), 0, text);
    yyjson_doc_free(immutable);
    return clis_atomic_write(path, text, error);
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

bool session_store::save_history(const kimix::vector<kimix::llm::Message> &h,
                                 kimix::string &error) const {
    error.clear();
    if (_dir.empty()) {
        error = "no session is open";
        return false;
    }
    // context.jsonl: one model_dump_json(exclude_none=True) line per record
    // (kimi_cli/soul/context.py JsonlContextStorage.append_messages).
    kimix::string context;
    for (const kimix::llm::Message &msg : h) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
        if (doc == nullptr) {
            error = "cannot allocate a history record";
            return false;
        }
        clis_write_message(doc, msg, "content");
        context += clis_compact_doc(doc);
        context += '\n';
        yyjson_mut_doc_free(doc);
    }
    if (!clis_atomic_write(join_path(_dir, kClisContextFile), context, error)) {
        return false;
    }

    // wire.jsonl: the protocol header, then the transcript.
    kimix::string protocol_version;
    const kimix::string wire_path = join_path(_dir, kClisWireFile);
    if (file_exists(wire_path)) {
        // WireFile.__post_init__ keeps the version an existing file declares.
        kimix::string existing;
        kimix::string read_error;
        if (read_file(wire_path, existing, read_error) && !existing.empty()) {
            const size_t end = existing.find('\n');
            const kimix::string_view first =
                existing.substr(0, end == kimix::string::npos ? existing.size()
                                                             : end);
            yyjson_doc *header = clis_parse(trim(first));
            if (header != nullptr) {
                const yyjson_val *header_root = yyjson_doc_get_root(header);
                if (clis_get_str(header_root, "type") == "metadata") {
                    protocol_version =
                        clis_get_str(header_root, "protocol_version");
                }
                yyjson_doc_free(header);
            }
        }
    }
    if (protocol_version.empty()) {
        protocol_version = kClisWireProtocolVersion;
    }
    kimix::string wire;
    {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
        if (doc == nullptr) {
            error = "cannot allocate the wire header";
            return false;
        }
        yyjson_mut_val *root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        clis_obj_add_str(doc, root, "type", "metadata");
        clis_obj_add_str(doc, root, "protocol_version", protocol_version);
        wire += clis_compact_doc(doc);
        wire += '\n';
        yyjson_mut_doc_free(doc);
    }
    const double timestamp = clis_now_seconds();
    for (const kimix::llm::Message &msg : h) {
        clis_wire_records(msg, timestamp, wire);
    }
    return clis_atomic_write(wire_path, wire, error);
}

bool session_store::load_history(kimix::vector<kimix::llm::Message> &h,
                                 kimix::string &error) const {
    error.clear();
    h.clear();
    if (_dir.empty()) {
        error = "no session is open";
        return false;
    }
    const kimix::string path = join_path(_dir, kClisContextFile);
    if (!file_exists(path)) {
        if (file_exists(join_path(_dir, kClisContextDbFile))) {
            // PLAN.md section 5.4: the SQLite store is neither written nor read
            // natively; say so instead of returning an empty history.
            error = "session history lives in context.db (SQLite); the native "
                    "CLI only reads the legacy context.jsonl format "
                    "(src/cli/PLAN.md \xC2\xA7" "5.4)";
            return false;
        }
        return true; // no history yet
    }
    kimix::string text;
    if (!read_file(path, text, error)) {
        return false;
    }
    kimix::vector<kimix::string> lines;
    split_lines(text, lines);
    for (const kimix::string &line : lines) {
        const kimix::string_view trimmed = trim(line);
        if (trimmed.empty()) {
            continue;
        }
        yyjson_doc *doc = clis_parse(trimmed);
        if (doc == nullptr) {
            continue; // unparseable records are skipped, like the reference
        }
        clis_msg_view view;
        if (!clis_parse_message_record(yyjson_doc_get_root(doc), view)) {
            yyjson_doc_free(doc);
            continue;
        }
        kimix::llm::Message msg;
        msg.role = view.role;
        kimix::string thinking_signature;
        for (const clis_part &part : view.parts) {
            if (part.kind == 1) {
                msg.thinking += part.text;
                if (!part.encrypted.empty()) {
                    thinking_signature = part.encrypted;
                }
            } else {
                msg.content += part.text;
            }
        }
        msg.thinking_signature = thinking_signature;
        msg.tool_calls = std::move(view.tool_calls);
        msg.tool_call_id = std::move(view.tool_call_id);
        h.push_back(std::move(msg));
        yyjson_doc_free(doc);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

void session_store::set_usage(double ratio, int64_t tokens, bool known) {
    _usage = ratio;
    _usage_tokens = tokens;
    _usage_known = known;
}

// ---------------------------------------------------------------------------
// /store, /load, close, /clear
// ---------------------------------------------------------------------------

bool session_store::store_as(kimix::string_view new_id, kimix::string &error) {
    error.clear();
    if (_dir.empty() || !_open) {
        error = "no active session to store";
        return false;
    }
    if (new_id.empty()) {
        error = "target session id cannot be empty";
        return false;
    }
    if (new_id == kimix::string_view(_id.data(), _id.size())) {
        error = "target session id must differ from the current session id";
        return false;
    }
    const kimix::string target = join_path(cache_root(_work_dir), new_id);
    if (dir_exists(target) || file_exists(target)) {
        error = "target session already exists: " + kimix::string(new_id);
        return false;
    }
    if (!dir_exists(_dir)) {
        error = "source session directory not found: " + _dir;
        return false;
    }
    // shutil.copytree: the source session stays open and current.
    return clis_copy_tree(_dir, target, error);
}

bool session_store::copy_into(kimix::string_view new_id, kimix::string &error) {
    error.clear();
    if (_dir.empty() || !_open) {
        error = "no session is open";
        return false;
    }
    if (new_id.empty()) {
        error = "session id cannot be empty";
        return false;
    }
    const kimix::string source = join_path(cache_root(_work_dir), new_id);
    if (!dir_exists(source)) {
        error = "source session not found: " + kimix::string(new_id);
        return false;
    }
    if (source == _dir) {
        error = "source session is the current session: " + kimix::string(new_id);
        return false;
    }
    // The files this store owns are replaced by the loaded session's content;
    // the id and the directory stay (the reference loads into a fresh anonymous
    // id and keeps it).
    kimix::vector<kimix::string> removed;
    clis_remove_owned_files(_dir, removed);
    if (!clis_copy_tree(source, _dir, error)) {
        return false;
    }
    _usage = 0.0;
    _usage_tokens = 0;
    _usage_known = false;
    return true;
}

bool session_store::close(bool delete_if_anonymous, kimix::string &error) {
    error.clear();
    if (_dir.empty()) {
        return true; // nothing was opened
    }
    // No file handle is held across calls (every write opens, writes and
    // closes), so only the directory removal is left.  Nothing is saved
    // implicitly: the caller saves first (the reference's /exit path).
    const bool remove_dir = delete_if_anonymous && _anonymous;
    _open = false;
    _usage = 0.0;
    _usage_tokens = 0;
    _usage_known = false;
    if (!remove_dir) {
        return true;
    }
    return remove_all(_dir, error);
}

bool session_store::clear_context(kimix::string &error) {
    error.clear();
    if (_dir.empty()) {
        error = "no session is open";
        return false;
    }
    kimix::vector<kimix::string> removed;
    clis_remove_owned_files(_dir, removed);
    _usage = 0.0;
    _usage_tokens = 0;
    _usage_known = false;
    return true;
}

// ---------------------------------------------------------------------------
// /export
// ---------------------------------------------------------------------------

bool session_store::export_markdown(const kimix::vector<kimix::llm::Message> &h,
                                    kimix::string_view path,
                                    kimix::string &error) const {
    error.clear();
    if (_dir.empty()) {
        error = "no session is open";
        return false;
    }
    if (h.empty()) {
        // kimi_cli Session.export / _cmd_export: ValueError("No messages to
        // export.") - the native CLI reports it through `error`.
        error = "No messages to export.";
        return false;
    }
    const int64_t now = clis_now_unix();
    kimix::string target;
    if (path.empty()) {
        target = join_path(_dir, "export_" + format_utc(now, "%Y%m%d-%H%M%S") +
                                     ".md");
    } else {
        // The reference resolves a relative path against the work directory.
        kimix::filesystem::path resolved;
        if (!clis_to_path(path, resolved)) {
            error = "unrepresentable export path: " + kimix::string(path);
            return false;
        }
        if (!resolved.is_absolute()) {
            kimix::filesystem::path base;
            if (!clis_to_path(_work_dir, base)) {
                error = "unrepresentable work directory: " + _work_dir;
                return false;
            }
            resolved = base / resolved;
        }
        target = kimix::to_string(resolved);
    }

    size_t turns = 0;
    size_t tool_calls = 0;
    kimix::string topic;
    for (const kimix::llm::Message &msg : h) {
        if (msg.role == "user") {
            if (turns == 0) {
                topic = clis_shorten(msg.content, 80);
            }
            ++turns;
        }
        tool_calls += msg.tool_calls.size();
    }
    const int64_t tokens = _usage_known ? _usage_tokens : 0;

    kimix::string out;
    // Front matter: kimi_cli/utils/export.py build_export_markdown (340-351).
    // The reference emits no title/model key here.
    out += "---\nsession_id: ";
    out += _id;
    out += "\nexported_at: ";
    out += clis_iso_seconds(now);
    out += "\nwork_dir: ";
    out += _work_dir;
    out += "\nmessage_count: ";
    clis_append_int(out, static_cast<int64_t>(h.size()));
    out += "\ntoken_count: ";
    clis_append_int(out, tokens);
    out += "\n---\n\n# Kimi Session Export\n\n";
    // Overview (_build_overview).
    out += "## Overview\n\n- **Topic**: ";
    out += topic.empty() ? kimix::string("(empty)") : topic;
    out += "\n- **Conversation**: ";
    clis_append_int(out, static_cast<int64_t>(turns));
    out += " turns | ";
    clis_append_int(out, static_cast<int64_t>(tool_calls));
    out += " tool calls | ";
    out += clis_comma_group(tokens);
    out += " tokens\n\n---\n";
    // One section per message (reduction: no "## Turn N" grouping).
    kimix::map<kimix::string, std::pair<kimix::string, kimix::string>> tool_info;
    for (const kimix::llm::Message &msg : h) {
        clis_message_md(msg, tool_info, out);
        for (const kimix::llm::ToolCall &call : msg.tool_calls) {
            tool_info[call.id] =
                std::make_pair(call.name,
                               clis_tool_hint(call.arguments.empty()
                                                  ? kimix::string_view("{}")
                                                  : kimix::string_view(
                                                        call.arguments)));
        }
    }

    const kimix::string parent = parent_path(target);
    if (!parent.empty() && !make_dirs(parent, error)) {
        return false;
    }
    return write_file(target, out, error);
}

// ---------------------------------------------------------------------------
// list()
// ---------------------------------------------------------------------------

kimix::vector<session_info> session_store::list(const kimix::string &work_dir) {
    kimix::vector<session_info> out;
    const kimix::string root = cache_root(work_dir);
    if (root.empty()) {
        return out;
    }
    kimix::filesystem::path root_path;
    if (!clis_to_path(root, root_path)) {
        return out;
    }
    std::error_code ec;
    if (!kimix::filesystem::is_directory(root_path, ec)) {
        return out;
    }
    kimix::filesystem::directory_iterator it(root_path, ec);
    if (ec) {
        return out;
    }
    const kimix::filesystem::directory_iterator end;
    while (it != end) {
        const kimix::filesystem::directory_entry entry = *it;
        std::error_code entry_ec;
        if (entry.is_directory(entry_ec)) {
            session_info info;
            info.id = kimix::to_string(entry.path().filename());
            info.title = "Untitled";
            if (!info.id.empty()) {
                const kimix::string dir = join_path(root, info.id);
                // updated_at: 0 unless the session carries one of its files;
                // otherwise the newest of state.json / context.db /
                // context.jsonl.
                int64_t updated = file_mtime_unix(join_path(dir, kClisStateFile));
                const int64_t db_mtime =
                    file_mtime_unix(join_path(dir, kClisContextDbFile));
                if (db_mtime > updated) {
                    updated = db_mtime;
                }
                const int64_t jsonl_mtime =
                    file_mtime_unix(join_path(dir, kClisContextFile));
                if (jsonl_mtime > updated) {
                    updated = jsonl_mtime;
                }
                info.updated_at = updated;

                const kimix::string state_path = join_path(dir, kClisStateFile);
                kimix::string text;
                kimix::string read_error;
                if (file_exists(state_path) &&
                    read_file(state_path, text, read_error)) {
                    yyjson_doc *doc = clis_parse(text);
                    const yyjson_val *state_root =
                        doc != nullptr ? yyjson_doc_get_root(doc) : nullptr;
                    if (state_root != nullptr && yyjson_is_obj(state_root)) {
                        const kimix::string title =
                            clis_get_str(state_root, "custom_title");
                        if (!title.empty()) {
                            info.title = title;
                        }
                        if (clis_has_member(state_root, "context_usage") &&
                            clis_has_member(state_root, "context_tokens")) {
                            info.context_usage =
                                clis_get_real(state_root, "context_usage", 0.0);
                            info.context_tokens =
                                clis_get_int(state_root, "context_tokens", 0);
                            info.usage_known = true;
                        }
                    }
                    if (doc != nullptr) {
                        yyjson_doc_free(doc);
                    }
                }
                out.push_back(std::move(info));
            }
        }
        it.increment(ec);
        if (ec) {
            break;
        }
    }
    std::sort(out.begin(), out.end(),
              [](const session_info &a, const session_info &b) {
                  if (a.updated_at != b.updated_at) {
                      return a.updated_at > b.updated_at;
                  }
                  return a.id < b.id;
              });
    return out;
}

} // namespace kimix::cli
