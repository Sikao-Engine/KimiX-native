/*
 * wire_envelope.cpp - see wire_envelope.h (plan 007).
 *
 * Uses the vendored kimix-yyjson (a trimmed 0.12 fork): it has
 * yyjson_read/yyjson_read_opts, the full yyjson_mut_* builder API and
 * yyjson_doc_mut_copy, but NO yyjson_mut_read and NO yyjson_free helper.
 * Consequences (documented deviations):
 *   - payload insertion: immutable yyjson_read (ONE text parse) +
 *     yyjson_doc_mut_copy to get a mutable value, then the value is
 *     borrowed into the envelope doc (the copy doc is kept alive until the
 *     write finishes). Still one parse, no string-escape round-trip.
 *   - write buffers are allocated with the default (malloc) allocator and
 *     freed with free() (same convention as tests/unit/ext/test_yyjson.cpp).
 *
 * All functions are noexcept: failures are reported via return values /
 * empty output, never via exceptions.
 */

#include <runtime/codec/wire_envelope.h>

#include <yyjson.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace kimix {
namespace runtime {
namespace codec {
namespace {

// ---------------------------------------------------------------------------
// Recursive object-key sort (matches toolset._sort_json_value).
// ---------------------------------------------------------------------------

void sort_obj_recursive(yyjson_mut_doc* doc, yyjson_mut_val* obj) {
    if (yyjson_mut_is_obj(obj)) {
        // Recurse into all values first (deepest containers sort first so a
        // parent rebuild never disturbs an already-sorted child). NOTE: this
        // fork's iter_next returns the ADVANCED key; the value of the current
        // pair comes from yyjson_mut_obj_iter_get_val(key) (the documented
        // pattern of this fork, see yyjson.h around line 3723).
        yyjson_mut_obj_iter iter;
        yyjson_mut_obj_iter_init(obj, &iter);
        yyjson_mut_val* key;
        while ((key = yyjson_mut_obj_iter_next(&iter)) != nullptr) {
            yyjson_mut_val* val = yyjson_mut_obj_iter_get_val(key);
            if (val != nullptr) {
                sort_obj_recursive(doc, val);
            }
        }

        struct pair_t {
            kimix::string key;
            yyjson_mut_val* val;
        };
        kimix::vector<pair_t> pairs;
        pairs.reserve(static_cast<size_t>(yyjson_mut_obj_size(obj)));

        yyjson_mut_obj_iter_init(obj, &iter);
        while ((key = yyjson_mut_obj_iter_next(&iter)) != nullptr) {
            yyjson_mut_val* val = yyjson_mut_obj_iter_get_val(key);
            if (val == nullptr) {
                break;
            }
            pair_t p;
            p.key.assign(yyjson_mut_get_str(key),
                         static_cast<size_t>(yyjson_mut_get_len(key)));
            p.val = val;
            pairs.push_back(std::move(p));
        }

        std::sort(pairs.begin(), pairs.end(),
                  [](const pair_t& a, const pair_t& b) { return a.key < b.key; });

        yyjson_mut_obj_clear(obj);
        for (auto& p : pairs) {
            // The key string must be owned by the doc (yyjson_mut_obj_add_val
            // stores the key POINTER without copying).
            yyjson_mut_val* key_val =
                yyjson_mut_strncpy(doc, p.key.data(), p.key.size());
            yyjson_mut_obj_add_val(doc, obj, yyjson_mut_get_str(key_val), p.val);
        }
    } else if (yyjson_mut_is_arr(obj)) {
        yyjson_mut_val* item;
        size_t idx = 0;
        size_t max = 0;
        yyjson_mut_arr_foreach(obj, idx, max, item) {
            sort_obj_recursive(doc, item);
        }
    }
}

// Sort + write one value to `out`. Returns false on failure.
bool write_sorted(yyjson_mut_doc* doc, yyjson_mut_val* root, kimix::string& out) {
    if (root == nullptr) {
        return false;
    }
    sort_obj_recursive(doc, root);
    yyjson_mut_doc_set_root(doc, root);
    size_t len = 0;
    char* text = yyjson_mut_write(doc, 0, &len);
    if (text == nullptr) {
        return false;
    }
    out.assign(text, len);
    free(text); // default allocator -> malloc/free
    return true;
}

} // namespace

void serialize_envelope(const wire_envelope& e, kimix::string& out) noexcept {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    if (doc == nullptr) {
        return;
    }
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    if (root == nullptr) {
        yyjson_mut_doc_free(doc);
        return;
    }
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_strncpy(doc, root, "type", e.type.data(), e.type.size());

    // Parse the payload ONCE (immutable), copy to a mutable doc, then borrow
    // the payload value into the envelope doc. The payload mut doc must stay
    // alive until the envelope is written (the value node belongs to it).
    yyjson_mut_doc* payload_doc = nullptr;
    yyjson_doc* parsed = yyjson_read(e.payload_json.data(), e.payload_json.size(), 0);
    if (parsed != nullptr) {
        payload_doc = yyjson_doc_mut_copy(parsed, nullptr);
        yyjson_doc_free(parsed);
    }
    if (payload_doc != nullptr) {
        yyjson_mut_val* payload = yyjson_mut_doc_get_root(payload_doc);
        if (payload != nullptr) {
            yyjson_mut_obj_add_val(doc, root, "payload", payload);
        } else {
            yyjson_mut_doc_free(payload_doc);
            payload_doc = nullptr;
        }
    }
    if (payload_doc == nullptr) {
        // Invalid payload JSON - embed as an escaped string so the envelope
        // stays serializable (only reachable on invalid input).
        yyjson_mut_obj_add_strncpy(doc, root, "payload", e.payload_json.data(),
                                   e.payload_json.size());
    }

    size_t len = 0;
    char* text = yyjson_mut_write(doc, 0, &len);
    if (text != nullptr) {
        out.assign(text, len);
        free(text);
    }
    if (payload_doc != nullptr) {
        yyjson_mut_doc_free(payload_doc);
    }
    yyjson_mut_doc_free(doc);
}

bool deserialize_envelope(kimix::string_view frame, wire_envelope& out) noexcept {
    out.type.clear();
    out.payload_json.clear();
    yyjson_doc* doc = yyjson_read(frame.data(), frame.size(), 0);
    if (doc == nullptr) {
        return false;
    }
    yyjson_val* root = yyjson_doc_get_root(doc);
    bool ok = false;
    if (root != nullptr && yyjson_is_obj(root)) {
        yyjson_val* type_val = yyjson_obj_get(root, "type");
        yyjson_val* payload_val = yyjson_obj_get(root, "payload");
        if (type_val != nullptr && yyjson_is_str(type_val) &&
            payload_val != nullptr) {
            out.type.assign(yyjson_get_str(type_val),
                            static_cast<size_t>(yyjson_get_len(type_val)));
            size_t len = 0;
            char* text = yyjson_val_write(payload_val, 0, &len);
            if (text != nullptr) {
                out.payload_json.assign(text, len);
                free(text);
                ok = true;
            }
        }
    }
    yyjson_doc_free(doc);
    return ok;
}

bool canonicalize_payload(kimix::string_view json, kimix::string& out) noexcept {
    out.clear();
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    if (doc == nullptr) {
        return false;
    }
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (root == nullptr) {
        yyjson_doc_free(doc);
        return false;
    }
    // Convert the immutable doc into a mutable doc so we can sort in place.
    yyjson_mut_doc* mdoc = yyjson_doc_mut_copy(doc, nullptr);
    yyjson_doc_free(doc);
    if (mdoc == nullptr) {
        return false;
    }
    bool ok = write_sorted(mdoc, yyjson_mut_doc_get_root(mdoc), out);
    yyjson_mut_doc_free(mdoc);
    return ok;
}

} // namespace codec
} // namespace runtime
} // namespace kimix
