// agent/tool_argument_repair.cpp - Argument anti-hallucination repairs
// (see tool_argument_repair.h).

#include "agent/tool_argument_repair.h"

#include "builtin_tools/tool.h" // ToolParams / ValueElement (repair kernels)

#include <algorithm>
#include <cstdio>

#ifdef KIMIX_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <mimalloc.h>
#include <llm/yyjson_alc.h>
#include <core/json_repair.h>
#include "yyjson.h"

namespace kimix::agent {

namespace {

// ── yyjson helpers ───────────────────────────────────────────────────────────

yyjson_doc *read_doc(kimix::string_view text) {
    if (text.empty()) {
        return nullptr;
    }
    kimix::string buffer(text);
    return yyjson_read_opts(buffer.data(), buffer.size(),
                            YYJSON_READ_STOP_WHEN_DONE,
                            &kimix::llm::kYYJsonAlcMi, nullptr);
}

// Serialize a mutable document to compact UTF-8 JSON text. The buffer comes
// from write_opts with the mimalloc allocator (yyjson_mut_write itself falls
// back to malloc), so it is released with mi_free (D3 contract).
bool write_doc(yyjson_mut_doc *mdoc, kimix::string &out) {
    size_t len = 0;
    char *json =
        yyjson_mut_write_opts(mdoc, 0 /* compact */, &kimix::llm::kYYJsonAlcMi,
                              &len, nullptr);
    if (json == nullptr) {
        return false;
    }
    out.assign(json, len);
    mi_free(json);
    return true;
}

// Recursively copy `val` into `mdoc` with object keys sorted byte-wise
// (orjson's OPT_SORT_KEYS / _sort_json_value analogue: str comparison).
yyjson_mut_val *sorted_copy(yyjson_val *val, yyjson_mut_doc *mdoc) {
    if (val == nullptr) {
        return yyjson_mut_null(mdoc);
    }
    if (yyjson_is_obj(val)) {
        // Collect the keys, sort, then insert in order.
        kimix::vector<std::pair<kimix::string, yyjson_val *>> entries;
        entries.reserve(yyjson_obj_size(val));
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(val, &iter);
        yyjson_val *key = nullptr;
        while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
            entries.emplace_back(
                kimix::string(yyjson_get_str(key),
                              static_cast<size_t>(yyjson_get_len(key))),
                yyjson_obj_iter_get_val(key));
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });
        yyjson_mut_val *obj = yyjson_mut_obj(mdoc);
        for (auto &entry : entries) {
            yyjson_mut_obj_add(obj, yyjson_mut_strcpy(mdoc, entry.first.c_str()),
                               sorted_copy(entry.second, mdoc));
        }
        return obj;
    }
    if (yyjson_is_arr(val)) {
        yyjson_mut_val *arr = yyjson_mut_arr(mdoc);
        yyjson_arr_iter iter;
        yyjson_arr_iter_init(val, &iter);
        yyjson_val *item = nullptr;
        while ((item = yyjson_arr_iter_next(&iter)) != nullptr) {
            yyjson_mut_arr_append(arr, sorted_copy(item, mdoc));
        }
        return arr;
    }
    if (yyjson_is_str(val)) {
        return yyjson_mut_strcpy(mdoc, yyjson_get_str(val));
    }
    if (yyjson_is_bool(val)) {
        return yyjson_mut_bool(mdoc, yyjson_get_bool(val));
    }
    if (yyjson_is_int(val)) {
        return yyjson_mut_sint(mdoc, yyjson_get_sint(val));
    }
    if (yyjson_is_uint(val)) {
        return yyjson_mut_uint(mdoc, yyjson_get_uint(val));
    }
    if (yyjson_is_real(val)) {
        return yyjson_mut_real(mdoc, yyjson_get_real(val));
    }
    return yyjson_mut_null(mdoc);
}

// Parse a JSON-looking string strictly, then through the repair kernel
// (utils.py repair_json_string / kosong jsonx.loads_relaxed). Returns a
// document the caller owns, or null.
yyjson_doc *parse_relaxed(kimix::string_view text) {
    yyjson_doc *doc = read_doc(text);
    if (doc != nullptr) {
        return doc;
    }
    const kimix::string repaired = kimix::repair(text);
    if (repaired.empty()) {
        return nullptr;
    }
    return read_doc(repaired);
}

bool starts_with_json_opener(kimix::string_view value) {
    size_t b = 0;
    while (b < value.size() &&
           (value[b] == ' ' || value[b] == '\t' || value[b] == '\n' ||
            value[b] == '\r')) {
        ++b;
    }
    return b < value.size() &&
           (value[b] == '{' || value[b] == '[');
}

// The string value of `val` when it is a JSON string.
kimix::string string_value_of(yyjson_val *val) {
    if (val == nullptr || !yyjson_is_str(val)) {
        return kimix::string();
    }
    return kimix::string(yyjson_get_str(val),
                         static_cast<size_t>(yyjson_get_len(val)));
}

// common.py _TODO_ITEM_TITLE_KEYS / _TODO_BATCH_KEYS (toolset.py:712-730).
constexpr kimix::string_view kTodoItemTitleKeys[] = {"task", "todo", "item",
                                                     "name"};
constexpr kimix::string_view kTodoBatchKeys[] = {
    "todos",  "items",   "list",       "tasks",  "entries", "updates",
    "edits",  "changes", "operations", "actions", "modifications", "batch"};
constexpr kimix::string_view kTodoItemExtraKeys[] = {
    "status", "notes", "rename_to", "complete", "parent"};

bool contains_key(const kimix::string_view *keys, size_t n,
                  kimix::string_view key) {
    for (size_t i = 0; i < n; ++i) {
        if (keys[i] == key) {
            return true;
        }
    }
    return false;
}

// The temp-folder file name counter (common.py _temp_idx: one monotonic
// sequence per process).
size_t &temp_index() {
    static size_t index = 0;
    return index;
}

size_t process_id() {
#ifdef KIMIX_PLATFORM_WINDOWS
    return static_cast<size_t>(::GetCurrentProcessId());
#else
    return static_cast<size_t>(::getpid());
#endif
}

} // namespace

// common.py _display_temp_path: forward-slashed display form; paths inside
// the session work directory are shown relative to it (".kimix_cache/
// tmp_<pid>/0.txt"). Exposed here because the retry message embeds it.
static kimix::string display_temp_path(kimix::string_view path,
                                       kimix::string_view work_dir) {
    kimix::string out(path);
    if (!work_dir.empty() && out.size() > work_dir.size() &&
        out.compare(0, work_dir.size(), work_dir.data(), work_dir.size()) == 0) {
        // Cut the work-dir prefix and any separator right behind it.
        size_t start = work_dir.size();
        while (start < out.size() &&
               (out[start] == '/' || out[start] == '\\')) {
            ++start;
        }
        out = out.substr(start);
    }
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

// ── 1. Format repairs ────────────────────────────────────────────────────────

bool unwrap_nested_arguments(kimix::string_view text, kimix::string &out) {
    yyjson_doc *doc = read_doc(text);
    if (doc == nullptr) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    // _unwrap_nested_arguments (toolset.py:657-666): keys <= {"arguments",
    // "args"} and len(arguments) == 1, inner value dict/list/str.
    if (root != nullptr && yyjson_is_obj(root)) {
        const size_t size = yyjson_obj_size(root);
        if (size == 1) {
            yyjson_val *key = yyjson_obj_get(root, "arguments");
            if (key == nullptr) {
                key = yyjson_obj_get(root, "args");
            }
            if (key != nullptr &&
                (yyjson_is_obj(key) || yyjson_is_arr(key) || yyjson_is_str(key))) {
                yyjson_mut_doc *mdoc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
                yyjson_mut_val *copy = sorted_copy(key, mdoc);
                yyjson_mut_doc_set_root(mdoc, copy);
                const bool ok = write_doc(mdoc, out);
                yyjson_mut_doc_free(mdoc);
                yyjson_doc_free(doc);
                return ok;
            }
        }
    }
    yyjson_doc_free(doc);
    out = kimix::string(text);
    return true;
}

bool repair_argument_format(kimix::string_view text, kimix::string &out) {
    // toolset.py:688-699: unwrap -> parse stringified -> unwrap again. (The
    // reference's loads_relaxed has already decoded the outer arguments JSON,
    // so a model that stringified the WHOLE argument object arrives here as
    // a JSON string value whose decoded text opens with '{' or '[' - exactly
    // what _parse_stringified_arguments then parses.)
    yyjson_doc *doc = read_doc(text);
    if (doc == nullptr) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    kimix::string current(text);
    if (root != nullptr && yyjson_is_str(root)) {
        // The arguments are a JSON string: parse the decoded value when it
        // looks like a JSON object/array (_parse_stringified_arguments,
        // toolset.py:669-685).
        const kimix::string decoded(yyjson_get_str(root),
                                    static_cast<size_t>(yyjson_get_len(root)));
          size_t b = 0;
          while (b < decoded.size() &&
                 (decoded[b] == ' ' || decoded[b] == '\t' ||
                  decoded[b] == '\n' || decoded[b] == '\r')) {
              ++b;
          }
        if (b < decoded.size() && (decoded[b] == '{' || decoded[b] == '[')) {
            yyjson_doc *inner = read_doc(decoded);
            if (inner == nullptr) {
                inner = parse_relaxed(decoded);
            }
            if (inner != nullptr) {
                yyjson_val *iroot = yyjson_doc_get_root(inner);
                if (iroot != nullptr &&
                    (yyjson_is_obj(iroot) || yyjson_is_arr(iroot))) {
                    yyjson_mut_doc *mdoc =
                        yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
                    yyjson_mut_val *copy = sorted_copy(iroot, mdoc);
                    yyjson_mut_doc_set_root(mdoc, copy);
                    kimix::string parsed;
                    if (write_doc(mdoc, parsed)) {
                        current = parsed; // replaced by the parsed value
                    }
                    yyjson_mut_doc_free(mdoc);
                }
                yyjson_doc_free(inner);
            }
        }
    }
    yyjson_doc_free(doc);

    // Unwrap (1): {"arguments": ...} / {"args": ...}.
    kimix::string unwrapped;
    if (!unwrap_nested_arguments(current, unwrapped)) {
        out = current;
        return true;
    }
    // Unwrap (2): in case the parsed string was itself wrapped.
    kimix::string final_text;
    if (!unwrap_nested_arguments(unwrapped, final_text)) {
        out = unwrapped;
        return true;
    }
    out = final_text;
    return true;
}

// ── 2. Schema-driven JSON-string repair ──────────────────────────────────────

bool repair_tool_arguments(kimix::string_view text,
                           const tool_param_schema &schema,
                           kimix::string &out) {
    // utils.py repair_tool_arguments (51-86): only fields whose schema
    // annotation is NOT a plain str are candidates; a JSON-looking string is
    // parsed and substituted. Non-dict inputs degrade to {} (the reference's
    // `dict(arguments)` guard); the caller has already coerced the root.
    out = kimix::string(text);
    if (schema.empty()) {
        return true;
    }
    yyjson_doc *doc = read_doc(text);
    if (doc == nullptr) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root == nullptr || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return false;
    }
    bool changed = false;
    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    yyjson_mut_val *mroot = yyjson_mut_obj(mdoc);
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(root, &iter);
    yyjson_val *key = nullptr;
    while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
        yyjson_val *value = yyjson_obj_iter_get_val(key);
        const kimix::string name(yyjson_get_str(key),
                                 static_cast<size_t>(yyjson_get_len(key)));
        yyjson_mut_val *mvalue = sorted_copy(value, mdoc);
        if (yyjson_is_str(value) && !schema.is_plain_string(name)) {
            const kimix::string raw = string_value_of(value);
            if (starts_with_json_opener(raw)) {
                yyjson_doc *parsed = parse_relaxed(raw);
                if (parsed != nullptr) {
                    yyjson_val *proot = yyjson_doc_get_root(parsed);
                    if (proot != nullptr &&
                        (yyjson_is_obj(proot) || yyjson_is_arr(proot))) {
                        mvalue = sorted_copy(proot, mdoc);
                        changed = true;
                    }
                    yyjson_doc_free(parsed);
                }
            }
        }
        yyjson_mut_obj_add(mroot, yyjson_mut_strcpy(mdoc, name.c_str()), mvalue);
    }
    yyjson_mut_doc_set_root(mdoc, mroot);
    if (changed) {
        kimix::string written;
        if (write_doc(mdoc, written)) {
            out = written;
        }
    }
    yyjson_mut_doc_free(mdoc);
    yyjson_doc_free(doc);
    return true;
}

// ── 3. Todo top-level shape repair ───────────────────────────────────────────

kimix::string_view todo_batch_key_for(kimix::string_view tool_name) {
    if (tool_name == "todo_update") {
        return "updates";
    }
    return "todos"; // todo_write (the reference's single todo_list tool)
}

bool repair_todo_arguments(kimix::string_view tool_name,
                           kimix::string_view text, kimix::string &out) {
    // _repair_todo_arguments (toolset.py:790-802): scoped to the todo tools.
    out = kimix::string(text);
    if (tool_name != "todo_write" && tool_name != "todo_update") {
        return true;
    }
    yyjson_doc *doc = read_doc(text);
    if (doc == nullptr) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root == nullptr || !yyjson_is_obj(root) || yyjson_obj_size(root) == 0) {
        yyjson_doc_free(doc);
        return false; // not a non-empty object: unchanged
    }
    const kimix::string_view batch_key = todo_batch_key_for(tool_name);
    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    yyjson_mut_val *mroot = yyjson_mut_obj(mdoc);
    yyjson_mut_doc_set_root(mdoc, mroot);
    bool changed = false;
    bool handled = false;

    const auto emit_item = [&](yyjson_val *item) -> yyjson_mut_val * {
        // _wrap_todo_item (toolset.py:739-750): a bare string becomes
        // {"title": <value>}; JSON-looking strings are left for the
        // JSON-string repair; objects pass through.
        if (yyjson_is_str(item)) {
            const kimix::string raw = string_value_of(item);
            if (!raw.empty() && raw[0] != '{' && raw[0] != '[') {
                yyjson_mut_val *obj = yyjson_mut_obj(mdoc);
                yyjson_mut_obj_add(obj, yyjson_mut_strcpy(mdoc, "title"),
                                   yyjson_mut_strcpy(mdoc, raw.c_str()));
                return obj;
            }
        }
        return sorted_copy(item, mdoc);
    };

    // Pass 1: a batch key carrying the item list.
    for (kimix::string_view key : kTodoBatchKeys) {
        yyjson_val *value = yyjson_obj_getn(root, key.data(), key.size());
        if (value == nullptr) {
            continue;
        }
        yyjson_mut_val *list = yyjson_mut_arr(mdoc);
        if (yyjson_is_str(value)) {
            const kimix::string raw = string_value_of(value);
            if (!raw.empty() && raw[0] != '{' && raw[0] != '[') {
                // A bare string IS the one item.
                yyjson_mut_val *obj = yyjson_mut_obj(mdoc);
                yyjson_mut_obj_add(obj, yyjson_mut_strcpy(mdoc, "title"),
                                   yyjson_mut_strcpy(mdoc, raw.c_str()));
                yyjson_mut_arr_append(list, obj);
            } else {
                yyjson_mut_arr_append(list, sorted_copy(value, mdoc));
            }
        } else if (yyjson_is_arr(value)) {
            yyjson_arr_iter aiter;
            yyjson_arr_iter_init(value, &aiter);
            yyjson_val *item = nullptr;
            while ((item = yyjson_arr_iter_next(&aiter)) != nullptr) {
                yyjson_mut_arr_append(list, emit_item(item));
            }
        } else if (yyjson_is_obj(value)) {
            yyjson_mut_arr_append(list, emit_item(value));
        } else {
            yyjson_mut_arr_append(list, sorted_copy(value, mdoc));
        }
        // Copy the remaining top-level keys, replacing the batch key.
        yyjson_obj_iter riter;
        yyjson_obj_iter_init(root, &riter);
        yyjson_val *k = nullptr;
        while ((k = yyjson_obj_iter_next(&riter)) != nullptr) {
            const kimix::string name(yyjson_get_str(k),
                                     static_cast<size_t>(yyjson_get_len(k)));
            if (name == batch_key) {
                continue; // the canonical key is re-added below
            }
            if (kimix::string_view(name) == key) {
                continue; // the retired key is folded away
            }
            yyjson_mut_obj_add(mroot, yyjson_mut_strcpy(mdoc, name.c_str()),
                               sorted_copy(yyjson_obj_iter_get_val(k), mdoc));
        }
            yyjson_mut_obj_add(mroot, yyjson_mut_strcpy(mdoc, batch_key.data()),
                               list);
        changed = true;
        handled = true;
        break;
    }

    // Pass 2: a singular item key promoted to a one-item list, with the
    // top-level status/notes/rename_to/complete/parent extras folded in.
    if (!handled) {
        for (kimix::string_view key : kTodoItemTitleKeys) {
            yyjson_val *raw_item =
                yyjson_obj_getn(root, key.data(), key.size());
            if (raw_item == nullptr) {
                continue;
            }
            yyjson_mut_val *item = nullptr;
            if (yyjson_is_str(raw_item)) {
                item = yyjson_mut_obj(mdoc);
                yyjson_mut_obj_add(item, yyjson_mut_strcpy(mdoc, "title"),
                                   yyjson_mut_strcpy(mdoc,
                                                     string_value_of(raw_item).c_str()));
            } else if (yyjson_is_obj(raw_item)) {
                item = sorted_copy(raw_item, mdoc);
            } else {
                item = sorted_copy(raw_item, mdoc);
            }
            yyjson_obj_iter riter;
            yyjson_obj_iter_init(root, &riter);
            yyjson_val *k = nullptr;
            while ((k = yyjson_obj_iter_next(&riter)) != nullptr) {
                const kimix::string name(yyjson_get_str(k),
                                         static_cast<size_t>(yyjson_get_len(k)));
                if (kimix::string_view(name) == key) {
                    continue; // the singular key is consumed by the item
                }
                const bool extra =
                    contains_key(kTodoItemExtraKeys,
                                 sizeof(kTodoItemExtraKeys) /
                                     sizeof(kTodoItemExtraKeys[0]),
                                 name);
      if (extra && yyjson_mut_is_obj(item)) {
          if (yyjson_mut_obj_get(item, name.c_str()) == nullptr) {
                        yyjson_mut_obj_add(item,
                                           yyjson_mut_strcpy(mdoc, name.c_str()),
                                           sorted_copy(yyjson_obj_iter_get_val(k),
                                                       mdoc));
                        changed = true;
                    }
                    continue;
                }
                yyjson_mut_obj_add(mroot, yyjson_mut_strcpy(mdoc, name.c_str()),
                                   sorted_copy(yyjson_obj_iter_get_val(k), mdoc));
            }
            yyjson_mut_val *list = yyjson_mut_arr(mdoc);
            yyjson_mut_arr_append(list, item);
            yyjson_mut_obj_add(mroot, yyjson_mut_strcpy(mdoc, batch_key.data()),
                               list);
            changed = true;
            break;
        }
    }

    if (changed) {
        kimix::string written;
        if (write_doc(mdoc, written)) {
            out = written;
        }
    }
    yyjson_mut_doc_free(mdoc);
    yyjson_doc_free(doc);
    return true;
}

// ── 4. Canonical call key ────────────────────────────────────────────────────

kimix::string canonical_tool_arguments(kimix::string_view arguments_json) {
    // _canonical_tool_arguments / _canonical_tool_arguments_text
    // (toolset.py:958-980): orjson.dumps(_sort_json_value(arguments));
    // non-JSON input is returned unchanged.
    yyjson_doc *doc = read_doc(arguments_json);
    if (doc == nullptr) {
        return kimix::string(arguments_json);
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root == nullptr) {
        yyjson_doc_free(doc);
        return kimix::string(arguments_json);
    }
    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    yyjson_mut_val *copy = sorted_copy(root, mdoc);
    yyjson_mut_doc_set_root(mdoc, copy);
    kimix::string out;
    if (!write_doc(mdoc, out)) {
        out = kimix::string(arguments_json);
    }
    yyjson_mut_doc_free(mdoc);
    yyjson_doc_free(doc);
    return out;
}

// ── 5. Long malformed content params ─────────────────────────────────────────

kimix::vector<kimix::string> long_content_params_of(kimix::string_view tool_name) {
    // common.py _LONG_CONTENT_PARAMS (31-39), verbatim.
    struct entry {
        kimix::string_view tool;
        kimix::vector<kimix::string_view> params;
    };
    static const entry kTable[] = {
        {"bash", {"command", "cmd"}},
        {"pwsh", {"command", "cmd"}},
        {"Run", {"command", "cmd"}},
        {"python", {"code", "source_code", "file"}},
        {"write", {"content", "text"}},
        {"edit", {"old_string", "new_string", "old", "new"}},
        {"subagent", {"prompt", "task"}},
    };
    kimix::vector<kimix::string> out;
    for (const entry &row : kTable) {
        if (row.tool == tool_name) {
            for (kimix::string_view p : row.params) {
                out.emplace_back(p);
            }
            return out;
        }
    }
    return out;
}

bool looks_like_malformed_json_param(kimix::string_view value) {
    // common.py _looks_like_malformed_json_param (57-104).
    if (value.size() < kLongParamMinLength) {
        return false;
    }
    kimix::string stripped(value);
    size_t b = 0;
    size_t e = stripped.size();
    while (b < e && (stripped[b] == ' ' || stripped[b] == '\t' ||
                     stripped[b] == '\n' || stripped[b] == '\r')) {
        ++b;
    }
    while (e > b && (stripped[e - 1] == ' ' || stripped[e - 1] == '\t' ||
                     stripped[e - 1] == '\n' || stripped[e - 1] == '\r')) {
        --e;
    }
    stripped = stripped.substr(b, e - b);
    if (stripped.empty()) {
        return false;
    }
    // Case 1: a JSON-encoded string ("..." with escapes).
    if (stripped.front() == '"' && stripped.back() == '"') {
        yyjson_doc *doc = read_doc(stripped);
        if (doc != nullptr) {
            yyjson_val *root = yyjson_doc_get_root(doc);
            const bool is_long_string =
                root != nullptr && yyjson_is_str(root) &&
                yyjson_get_len(root) > kLongParamMinLength;
            yyjson_doc_free(doc);
            if (is_long_string) {
                return true;
            }
        }
        // A JSON string that was not decoded: embedded quotes + escapes.
        const kimix::string inner = stripped.substr(1, stripped.size() - 2);
        if (inner.find('"') != kimix::string::npos &&
            inner.find('\\') != kimix::string::npos) {
            return true;
        }
    }
    // Case 2: a JSON array/object where a plain string is expected.
    if (stripped.front() == '[' || stripped.front() == '{') {
        yyjson_doc *doc = read_doc(stripped);
        if (doc != nullptr) {
            yyjson_val *root = yyjson_doc_get_root(doc);
            bool shaped = false;
            if (root != nullptr && yyjson_is_arr(root) && yyjson_arr_size(root) > 1) {
                shaped = true;
                yyjson_arr_iter iter;
                yyjson_arr_iter_init(root, &iter);
                yyjson_val *item = nullptr;
                while ((item = yyjson_arr_iter_next(&iter)) != nullptr) {
                    if (!yyjson_is_str(item)) {
                        shaped = false;
                        break;
                    }
                }
            }
            if (root != nullptr && yyjson_is_obj(root)) {
                shaped = true;
            }
            yyjson_doc_free(doc);
            if (shaped) {
                return true;
            }
        }
    }
    // Case 3: escaped newlines (\n instead of real newlines).
    if (stripped.find("\\n") != kimix::string::npos &&
        stripped.find('\n') == kimix::string::npos &&
        stripped.size() > kLongParamMinLength) {
        return true;
    }
    return false;
}

kimix::optional<kimix::string>
extract_content_from_malformed(kimix::string_view value) {
    // common.py _extract_content_from_malformed (107-153).
    kimix::string stripped(value);
    size_t b = 0;
    size_t e = stripped.size();
    while (b < e && (stripped[b] == ' ' || stripped[b] == '\t' ||
                     stripped[b] == '\n' || stripped[b] == '\r')) {
        ++b;
    }
    while (e > b && (stripped[e - 1] == ' ' || stripped[e - 1] == '\t' ||
                     stripped[e - 1] == '\n' || stripped[e - 1] == '\r')) {
        --e;
    }
    stripped = stripped.substr(b, e - b);
    if (stripped.empty()) {
        return kimix::optional<kimix::string>();
    }
    // Case 1: a JSON-encoded string - parse it.
    if (stripped.front() == '"' && stripped.back() == '"') {
        yyjson_doc *doc = read_doc(stripped);
        if (doc != nullptr) {
            yyjson_val *root = yyjson_doc_get_root(doc);
            if (root != nullptr && yyjson_is_str(root)) {
                kimix::string out(yyjson_get_str(root),
                                  static_cast<size_t>(yyjson_get_len(root)));
                yyjson_doc_free(doc);
                return kimix::optional<kimix::string>(std::move(out));
            }
            yyjson_doc_free(doc);
        }
    }
    // Case 2: a JSON array of strings - join them.
    if (stripped.front() == '[') {
        yyjson_doc *doc = read_doc(stripped);
        if (doc != nullptr) {
            yyjson_val *root = yyjson_doc_get_root(doc);
            bool all_strings = root != nullptr && yyjson_is_arr(root) &&
                               yyjson_arr_size(root) > 0;
            kimix::string joined;
            if (all_strings) {
                yyjson_arr_iter iter;
                yyjson_arr_iter_init(root, &iter);
                yyjson_val *item = nullptr;
                bool first = true;
                while ((item = yyjson_arr_iter_next(&iter)) != nullptr) {
                    if (!yyjson_is_str(item)) {
                        all_strings = false;
                        break;
                    }
                    if (!first) {
                        joined += "\n";
                    }
                    joined.append(yyjson_get_str(item),
                                  static_cast<size_t>(yyjson_get_len(item)));
                    first = false;
                }
            }
            yyjson_doc_free(doc);
            if (all_strings) {
                return kimix::optional<kimix::string>(std::move(joined));
            }
        }
    }
    // Case 3: a JSON object - look for a content-like key.
    if (stripped.front() == '{') {
        yyjson_doc *doc = read_doc(stripped);
        if (doc != nullptr) {
            yyjson_val *root = yyjson_doc_get_root(doc);
            kimix::optional<kimix::string> found;
            if (root != nullptr && yyjson_is_obj(root)) {
                for (const char *key :
                     {"content", "text", "code", "command", "cmd", "prompt",
                      "task", "value"}) {
                    yyjson_val *val = yyjson_obj_get(root, key);
                    if (val != nullptr && yyjson_is_str(val) &&
                        yyjson_get_len(val) > kLongParamMinLength) {
                        found.emplace(yyjson_get_str(val),
                                      static_cast<size_t>(yyjson_get_len(val)));
                        break;
                    }
                    if (val != nullptr && yyjson_is_arr(val)) {
                        bool all_strings = yyjson_arr_size(val) > 0;
                        kimix::string joined;
                        yyjson_arr_iter iter;
                        yyjson_arr_iter_init(val, &iter);
                        yyjson_val *item = nullptr;
                        bool first = true;
                        while ((item = yyjson_arr_iter_next(&iter)) != nullptr) {
                            if (!yyjson_is_str(item)) {
                                all_strings = false;
                                break;
                            }
                            if (!first) {
                                joined += "\n";
                            }
                            joined.append(yyjson_get_str(item),
                                          static_cast<size_t>(yyjson_get_len(item)));
                            first = false;
                        }
                        if (all_strings) {
                            found.emplace(std::move(joined));
                            break;
                        }
                    }
                }
            }
            yyjson_doc_free(doc);
            if (found.has_value()) {
                return found;
            }
        }
    }
    // Case 4: escaped newlines.
    if (stripped.find("\\n") != kimix::string::npos &&
        stripped.find('\n') == kimix::string::npos) {
        kimix::string out;
        out.reserve(stripped.size());
        for (size_t i = 0; i < stripped.size(); ++i) {
            if (i + 1 < stripped.size() && stripped[i] == '\\' &&
                stripped[i + 1] == 'n') {
                out += '\n';
                ++i;
                continue;
            }
            out += stripped[i];
        }
        return kimix::optional<kimix::string>(std::move(out));
    }
    return kimix::optional<kimix::string>();
}

bool unescape_escaped_newline_params(kimix::string_view arguments_json,
                                     kimix::string_view tool_name,
                                     kimix::string &repaired_args) {
    repaired_args.clear();
    const kimix::vector<kimix::string> params = long_content_params_of(tool_name);
    if (params.empty() || arguments_json.empty()) {
        return false;
    }
    builtin_tools::ToolParams parsed;
    kimix::string perr;
    if (!parsed.try_deserialize(
            kimix::span<char const>(arguments_json.data(), arguments_json.size()),
            perr)) {
        return false;
    }
    bool changed = false;
    for (const kimix::string &name : params) {
        const auto it = parsed.values.find(name);
        if (it == parsed.values.end() || !it->second.is_string()) {
            continue;
        }
        const kimix::string &value = it->second.as_string();
        // Only the unambiguous escaped-newline shape: literal "\\n" runs and
        // no real newline. A JSON-shaped value (quoted string / array /
        // object) keeps the reference save+refuse extraction path.
        if (value.find("\\n") == kimix::string::npos ||
            value.find('\n') != kimix::string::npos) {
            continue;
        }
        kimix::string_view stripped = value;
        while (!stripped.empty() &&
               (stripped.front() == ' ' || stripped.front() == '\t')) {
            stripped.remove_prefix(1);
        }
        while (!stripped.empty() &&
               (stripped.back() == ' ' || stripped.back() == '\t')) {
            stripped.remove_suffix(1);
        }
        if (!stripped.empty() &&
            (stripped.front() == '"' || stripped.front() == '[' ||
             stripped.front() == '{')) {
            continue;
        }
        kimix::string out;
        out.reserve(value.size());
        for (size_t i = 0; i < value.size(); ++i) {
            if (i + 1 < value.size() && value[i] == '\\' && value[i + 1] == 'n') {
                out += '\n';
                ++i;
                continue;
            }
            out += value[i];
        }
        it->second = builtin_tools::ValueElement::make_string(std::move(out));
        changed = true;
    }
    if (!changed) {
        return false;
    }
    kimix::vector<char> out;
    if (!parsed.serialize(out)) {
        return false;
    }
    repaired_args.assign(out.data(), out.size());
    return true;
}

bool extract_and_save_long_param(kimix::string_view arguments_json,
                                 kimix::string_view tool_name,
                                 kimix::string_view work_dir,
                                 kimix::vector<long_param_save> &saved,
                                 kimix::string &error) {
    saved.clear();
    const kimix::vector<kimix::string> params =
        long_content_params_of(tool_name);
    if (params.empty() || arguments_json.empty()) {
        return false;
    }
    yyjson_doc *doc = read_doc(arguments_json);
    if (doc == nullptr) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root == nullptr || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return false;
    }

    // The shared temp folder (common.py _temp_folder): <base>/.kimix_cache/
    // tmp_<pid>. The native session anchors it at the work dir like the
    // python tool does (the reference builds it against the process cwd).
    const kimix::string base_dir =
        work_dir.empty()
            ? kimix::to_string(kimix::filesystem::current_path())
            : kimix::string(work_dir);
    const kimix::filesystem::path base(base_dir);
    const kimix::filesystem::path temp_dir =
        base / ".kimix_cache" /
        (kimix::string("tmp_") + kimix::format("{}", process_id()));

    for (const kimix::string &param : params) {
        yyjson_val *value = yyjson_obj_get(root, param.c_str());
        if (value == nullptr) {
            continue;
        }
        kimix::optional<kimix::string> content;
        if (yyjson_is_str(value)) {
            const kimix::string raw(yyjson_get_str(value),
                                    static_cast<size_t>(yyjson_get_len(value)));
            if (raw.size() < kLongParamMinLength) {
                continue; // short strings are never extracted
            }
            content = extract_content_from_malformed(raw);
        } else if (yyjson_is_arr(value)) {
            // A list of lines: joined content, no minimum length.
            kimix::string joined;
            yyjson_arr_iter iter;
            yyjson_arr_iter_init(value, &iter);
            yyjson_val *item = nullptr;
            bool first = true;
            while ((item = yyjson_arr_iter_next(&iter)) != nullptr) {
                if (!first) {
                    joined += "\n";
                }
                if (yyjson_is_str(item)) {
                    joined.append(yyjson_get_str(item),
                                  static_cast<size_t>(yyjson_get_len(item)));
                } else {
                    joined += "<non-string item>";
                }
                first = false;
            }
            content = kimix::optional<kimix::string>(std::move(joined));
        } else if (yyjson_is_obj(value)) {
            // A dict: serialized (indent-2 in the reference; compact here is
            // not observable through the file's purpose) and saved.
            kimix::string text;
            yyjson_mut_doc *mdoc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
            yyjson_mut_val *copy = sorted_copy(value, mdoc);
            yyjson_mut_doc_set_root(mdoc, copy);
            if (!write_doc(mdoc, text)) {
                text = "<unserializable object>";
            }
            yyjson_mut_doc_free(mdoc);
            content = kimix::optional<kimix::string>(std::move(text));
        }
        if (!content.has_value()) {
            continue;
        }
        // common.py _create_script_file: <temp>/<index><ext>, monotonic.
        std::error_code ec;
        kimix::filesystem::create_directories(temp_dir, ec);
        kimix::filesystem::path file =
            temp_dir / (kimix::format("{}", temp_index()) + ".txt");
        ++temp_index();
        std::FILE *f = std::fopen(kimix::to_string(file).c_str(), "wb");
        if (f == nullptr) {
            error = "cannot create temp file: " + kimix::to_string(file);
            continue;
        }
        const kimix::string &text = *content;
        const size_t n = std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
        if (n != text.size()) {
            error = "short write: " + kimix::to_string(file);
            continue;
        }
        long_param_save save;
        save.param = param;
        save.path = kimix::to_string(file);
        saved.push_back(std::move(save));
        (void)base_dir;
    }
    yyjson_doc_free(doc);
    return !saved.empty();
}

kimix::string build_long_param_retry_msg(
    const kimix::vector<long_param_save> &saved,
    kimix::string_view original_error, kimix::string_view work_dir) {
    // common.py _build_long_param_retry_msg (219-250), verbatim text; the
    // file references use the short forward-slashed display form
    // (_display_temp_path).
    kimix::string out(original_error);
    out += "\n\n[Long content extracted to temp files]\n";
    out += "The following parameters appear to be in the wrong format. ";
    out += "The raw content has been saved to temp files.\n";
    out += "Please use `read` to inspect the files and retry with the correct format.\n";
    for (const long_param_save &save : saved) {
        out += "  - `";
        out += save.param;
        out += "`: saved to `";
        out += display_temp_path(save.path, work_dir);
        out += "`\n";
    }
    return out;
}

} // namespace kimix::agent
