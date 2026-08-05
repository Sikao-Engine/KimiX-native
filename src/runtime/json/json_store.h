/*
 * json_store.h - native JSON document store + notification batch scan
 * (kimix::runtime::json).
 *
 * Plan 016: one yyjson document per task/notification file. load() parses
 * once; update() deep-merges a partial JSON object; get()/save_atomic()
 * serialize with the orjson OPT_INDENT_2-compatible printer so the written
 * files are byte-compatible with kimi_cli utils/io.py::atomic_json_write
 * (tmp file + atomic rename, fsync'd).
 *
 * Update-merge semantics (deep): for each key of the update object, when
 * both the existing and the update value are JSON objects the merge recurses
 * key-by-key; otherwise the update value replaces the existing one. New keys
 * are appended (dict insertion-order semantics).
 */

#pragma once

#include <core/kimix_core.h>

// Opaque yyjson mutable doc (the .cpp owns the definition).
struct yyjson_mut_doc;

namespace kimix {
namespace runtime {
namespace json {

// One scanned notification view (sorted by created_at descending).
struct notification_row {
    kimix::string id;
    double created_at = 0.0;
    kimix::string event_json;    // compact JSON of the event object
    kimix::string delivery_json; // compact JSON of the delivery object ("" if absent)
};

class KIMIX_RUNTIME_API JsonStore {
public:
    JsonStore() noexcept = default;
    ~JsonStore();

    JsonStore(const JsonStore&) = delete;
    JsonStore& operator=(const JsonStore&) = delete;

    // Parse `bytes` as the new document. Invalid JSON resets the store to an
    // empty object ("{}"). Always replaces the previous document.
    void load(kimix::string_view bytes) noexcept;

    // Deep-merge a partial JSON object into the current document. Invalid
    // JSON is ignored (no change). When the store is empty, update acts as
    // load (the merged document is the update itself).
    void update(kimix::string_view json_bytes) noexcept;

    // Serialize the document (orjson OPT_INDENT_2 bytes) into `out_json`.
    // Empty store serializes "{}".
    void get(kimix::string& out_json) const noexcept;

    // Top-level keys in document order.
    kimix::vector<kimix::string> keys() const noexcept;

    // Atomically write the pretty serialization to `path` (tmp + rename +
    // flush). Returns false on I/O failure (out_blob receives the bytes that
    // would have been written, so callers can retry).
    bool save_atomic(kimix::string_view path, kimix::string& out_blob) const noexcept;

    // Reset to an empty document.
    void clear() noexcept;

    // True once load()/update() have produced a document.
    bool loaded() const noexcept;

private:
    yyjson_mut_doc* _doc = nullptr;

    void reset_doc() noexcept;
    void ensure_doc() noexcept;
};

// Batch scan of a JSONL of notification views: each line is
// {"event": {...}, "delivery": {...}}. ONE parse, ONE sort (created_at
// descending; ties keep input order -- stable, matching Python's stable
// sort after id-ordered iteration). `now_ms` is reserved for future
// time-based filtering (the reference list_views has no time filter).
KIMIX_RUNTIME_API void scan_notifications(kimix::string_view jsonl,
                                          uint64_t now_ms,
                                          kimix::vector<notification_row>& out) noexcept;

} // namespace json
} // namespace runtime
} // namespace kimix
