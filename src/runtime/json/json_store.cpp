/*
 * json_store.cpp - see json_store.h (plan 016).
 *
 * yyjson usage follows wire_envelope.cpp: immutable yyjson_read for input
 * documents, yyjson_doc_mut_copy / yyjson_val_mut_copy to move values into
 * the owned mutable doc, yyjson_mut_write for compact serialization, and
 * yyjson_mut_doc_imut_copy + the shared pretty printer for the
 * orjson-OPT_INDENT_2 file bytes. Write buffers are malloc'd by yyjson and
 * freed with free().
 */

#include <runtime/json/json_store.h>

#include <yyjson.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include <runtime/json/json_kernel_util.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace kimix {
namespace runtime {
namespace json {
namespace {

// Deep-merge `src` (immutable value) into `dst` (mutable object owned by
// `doc`). Returns true on success.
bool merge_value(yyjson_mut_doc* doc, yyjson_mut_val* dst,
                 const yyjson_val* src) noexcept {
    if (dst == nullptr || src == nullptr) {
        return false;
    }
    if (!yyjson_is_obj(src)) {
        return false;
    }
    size_t idx = 0;
    size_t max = 0;
    yyjson_val* key = nullptr;
    yyjson_val* val = nullptr;
    yyjson_obj_foreach(src, idx, max, key, val) {
        const char* k = yyjson_get_str(key);
        const size_t klen = static_cast<size_t>(yyjson_get_len(key));
        yyjson_mut_val* existing = yyjson_mut_obj_get(dst, k);
        if (existing != nullptr && yyjson_mut_is_obj(existing) && yyjson_is_obj(val)) {
            // Recursive merge for nested objects.
            if (!merge_value(doc, existing, val)) {
                return false;
            }
        } else {
            yyjson_mut_val* copy = yyjson_val_mut_copy(doc, val);
            if (copy == nullptr) {
                return false;
            }
            // The fork's obj_put/obj_add_val do NOT copy the key string --
            // copy it into the doc pool first (the update doc is freed by
            // the caller).
            yyjson_mut_val* key_copy = yyjson_mut_strncpy(doc, k, klen);
            yyjson_mut_obj_put(dst, key_copy, copy); // replace or append
        }
    }
    return true;
}

bool atomic_replace(const char* path, const kimix::string& blob) noexcept {
    kimix::string tmp(path);
    tmp += ".tmp";
    {
        std::ofstream out(tmp.c_str(), std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        out.flush();
        if (!out) {
            out.close();
            std::remove(tmp.c_str());
            return false;
        }
    }
#ifdef _WIN32
    // Replace atomically (MoveFileExA overwrites; WRITE_THROUGH flushes the
    // metadata to disk). tmp must not exist at the destination name.
    if (!MoveFileExA(tmp.c_str(), path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
#else
    if (std::rename(tmp.c_str(), path) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
#endif
}

} // namespace

JsonStore::~JsonStore() {
    reset_doc();
}

void JsonStore::reset_doc() noexcept {
    if (_doc != nullptr) {
        yyjson_mut_doc_free(_doc);
        _doc = nullptr;
    }
}

void JsonStore::ensure_doc() noexcept {
    if (_doc == nullptr) {
        _doc = yyjson_mut_doc_new(nullptr);
        if (_doc != nullptr) {
            yyjson_mut_val* root = yyjson_mut_obj(_doc);
            yyjson_mut_doc_set_root(_doc, root);
        }
    }
}

void JsonStore::load(kimix::string_view bytes) noexcept {
    reset_doc();
    yyjson_doc* parsed = yyjson_read(bytes.data(), bytes.size(), 0);
    if (parsed == nullptr) {
        ensure_doc(); // empty object on invalid input
        return;
    }
    _doc = yyjson_doc_mut_copy(parsed, nullptr);
    yyjson_doc_free(parsed);
    if (_doc == nullptr) {
        ensure_doc();
    }
}

void JsonStore::update(kimix::string_view json_bytes) noexcept {
    yyjson_doc* parsed = yyjson_read(json_bytes.data(), json_bytes.size(), 0);
    if (parsed == nullptr) {
        return; // invalid JSON: no change
    }
    yyjson_val* root = yyjson_doc_get_root(parsed);
    if (root == nullptr || !yyjson_is_obj(root)) {
        yyjson_doc_free(parsed);
        return;
    }
    ensure_doc();
    if (_doc == nullptr) {
        yyjson_doc_free(parsed);
        return;
    }
    yyjson_mut_val* dst = yyjson_mut_doc_get_root(_doc);
    if (dst == nullptr || !yyjson_mut_is_obj(dst)) {
        yyjson_mut_doc_free(_doc);
        _doc = nullptr;
        ensure_doc();
        dst = yyjson_mut_doc_get_root(_doc);
    }
    merge_value(_doc, dst, root);
    yyjson_doc_free(parsed);
}

void JsonStore::get(kimix::string& out_json) const noexcept {
    if (_doc == nullptr) {
        out_json = "{}";
        return;
    }
    json_write_pretty(_doc, out_json);
    if (out_json.empty()) {
        out_json = "{}";
    }
}

kimix::vector<kimix::string> JsonStore::keys() const noexcept {
    kimix::vector<kimix::string> out;
    if (_doc == nullptr) {
        return out;
    }
    yyjson_mut_val* root = yyjson_mut_doc_get_root(_doc);
    if (root == nullptr || !yyjson_mut_is_obj(root)) {
        return out;
    }
    yyjson_mut_obj_iter iter;
    yyjson_mut_obj_iter_init(root, &iter);
    yyjson_mut_val* key;
    while ((key = yyjson_mut_obj_iter_next(&iter)) != nullptr) {
        out.emplace_back(yyjson_mut_get_str(key),
                         static_cast<size_t>(yyjson_mut_get_len(key)));
    }
    return out;
}

bool JsonStore::save_atomic(kimix::string_view path, kimix::string& out_blob) const noexcept {
    get(out_blob);
    return atomic_replace(path.data(), out_blob);
}

void JsonStore::clear() noexcept {
    reset_doc();
    ensure_doc();
}

bool JsonStore::loaded() const noexcept {
    return _doc != nullptr;
}

void scan_notifications(kimix::string_view jsonl, uint64_t /*now_ms*/,
                        kimix::vector<notification_row>& out) noexcept {
    out.clear();
    kimix::vector<notification_row> rows;
    size_t start = 0;
    while (start <= jsonl.size()) {
        size_t nl = jsonl.find('\n', start);
        const size_t end = (nl == kimix::string_view::npos) ? jsonl.size() : nl;
        kimix::string_view line = jsonl.substr(start, end - start);
        if (!line.empty()) {
            yyjson_doc* parsed = yyjson_read(line.data(), line.size(), 0);
            if (parsed != nullptr) {
                yyjson_val* root = yyjson_doc_get_root(parsed);
                if (root != nullptr && yyjson_is_obj(root)) {
                    yyjson_val* event_val = yyjson_obj_get(root, "event");
                    yyjson_val* delivery_val = yyjson_obj_get(root, "delivery");
                    if (event_val != nullptr && yyjson_is_obj(event_val)) {
                        notification_row row;
                        yyjson_val* id_val = yyjson_obj_get(event_val, "id");
                        yyjson_val* created_val = yyjson_obj_get(event_val, "created_at");
                        if (id_val != nullptr && yyjson_is_str(id_val)) {
                            row.id.assign(yyjson_get_str(id_val),
                                          static_cast<size_t>(yyjson_get_len(id_val)));
                        }
                        if (created_val != nullptr && yyjson_is_num(created_val)) {
                            if (yyjson_is_uint(created_val)) {
                                row.created_at = static_cast<double>(yyjson_get_uint(created_val));
                            } else if (yyjson_is_sint(created_val)) {
                                row.created_at = static_cast<double>(yyjson_get_sint(created_val));
                            } else {
                                row.created_at = yyjson_get_real(created_val);
                            }
                        }
                        size_t len = 0;
                        char* text = yyjson_val_write(event_val, 0, &len);
                        if (text != nullptr) {
                            row.event_json.assign(text, len);
                            free(text);
                        }
                        if (delivery_val != nullptr && yyjson_is_obj(delivery_val)) {
                            text = yyjson_val_write(delivery_val, 0, &len);
                            if (text != nullptr) {
                                row.delivery_json.assign(text, len);
                                free(text);
                            }
                        }
                        rows.push_back(std::move(row));
                    }
                }
                yyjson_doc_free(parsed);
            }
        }
        if (nl == kimix::string_view::npos) {
            break;
        }
        start = nl + 1;
    }
    // Stable sort by created_at descending (Python: views.sort(key=lambda
    // v: v.event.created_at, reverse=True) -- stable, so ties keep the
    // id-ordered input order).
    std::stable_sort(rows.begin(), rows.end(),
                     [](const notification_row& a, const notification_row& b) {
                         return a.created_at > b.created_at;
                     });
    out = std::move(rows);
}

} // namespace json
} // namespace runtime
} // namespace kimix
