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
 *   - output: written directly into the caller's kimix::string via the
 *     allocation-free yyjson_*_write_buf callbacks (growing it geometrically
 *     on demand), so a reused output string needs no per-call malloc/copy;
 *     the write buffer is never an intermediate heap buffer.
 *
 * All functions are noexcept: failures are reported via return values /
 * empty output, never via exceptions.
 */

#include <runtime/codec/wire_envelope.h>

#include <yyjson.h>

#include <llm/yyjson_alc.h>

#include <algorithm>
#include <cstring>

namespace kimix {
namespace runtime {
namespace codec {
namespace {

// ---------------------------------------------------------------------------
// Allocation-free compact JSON writing into kimix::string.
//
// yyjson_*_write_buf writes into a caller-provided buffer without allocating
// and returns 0 when the buffer is too small (the writer's null allocator
// aborts the attempt cleanly). Growing `out` geometrically across attempts
// means a reused output string settles into a zero-allocation steady state:
// no intermediate heap buffer, no final memcpy of the whole JSON.
// ---------------------------------------------------------------------------

template <typename Writer>
bool write_json_into_impl(kimix::string& out, Writer&& writer) noexcept {
    constexpr size_t kMinCap = 64;
    size_t cap = out.capacity() < kMinCap ? kMinCap : out.capacity();
    for (;;) {
        out.resize(cap); // make the whole [0, cap) range writable
        const size_t n = writer(out.data(), cap);
        if (n != 0) {
            out.resize(n);
            return true;
        }
        out.resize(0);
        // Sanity bound: nothing in this codec produces documents anywhere
        // near this size (RecvBuffer caps frames at 10 MiB).
        if (cap >= (size_t{1} << 40)) {
            return false;
        }
        cap = cap + cap / 2 + 16;
    }
}

bool write_json_into(kimix::string& out, const yyjson_mut_doc* doc) noexcept {
    return write_json_into_impl(out, [doc](char* buf, size_t cap) noexcept {
        return yyjson_mut_write_buf(buf, cap, doc, 0, nullptr);
    });
}

bool write_json_into(kimix::string& out, const yyjson_val* val) noexcept {
    return write_json_into_impl(out, [val](char* buf, size_t cap) noexcept {
        return yyjson_val_write_buf(buf, cap, val, 0, nullptr);
    });
}

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
            const char* key; // borrowed: lives in the doc arena
            size_t key_len;
            yyjson_mut_val* val;
        };

        const size_t n = static_cast<size_t>(yyjson_mut_obj_size(obj));
        // Small objects (the overwhelming majority in tool payloads) sort on
        // the stack; only wide objects pay for a heap buffer.
        pair_t stack_pairs[24];
        kimix::vector<pair_t> heap_pairs;
        pair_t* pairs = stack_pairs;
        if (n > 24) {
            heap_pairs.reserve(n);
            pairs = heap_pairs.data();
        }

        size_t count = 0;
        yyjson_mut_obj_iter_init(obj, &iter);
        while ((key = yyjson_mut_obj_iter_next(&iter)) != nullptr) {
            yyjson_mut_val* val = yyjson_mut_obj_iter_get_val(key);
            if (val == nullptr) {
                break;
            }
            // Keys are compared by borrowed pointer + length (memcmp byte
            // order == std::string semantics), never copied out of the arena.
            pairs[count].key = yyjson_mut_get_str(key);
            pairs[count].key_len =
                static_cast<size_t>(yyjson_mut_get_len(key));
            pairs[count].val = val;
            ++count;
        }

        std::sort(pairs, pairs + count,
                  [](const pair_t& a, const pair_t& b) {
                      const size_t common =
                          a.key_len < b.key_len ? a.key_len : b.key_len;
                      const int c = common == 0
                                        ? 0
                                        : std::memcmp(a.key, b.key, common);
                      return c != 0 ? c < 0 : a.key_len < b.key_len;
                  });

        yyjson_mut_obj_clear(obj);
        for (size_t i = 0; i < count; ++i) {
            // The key string must be owned by the doc (yyjson_mut_obj_add_val
            // stores the key POINTER without copying).
            yyjson_mut_val* key_val = yyjson_mut_strncpy(
                doc, pairs[i].key, pairs[i].key_len);
            yyjson_mut_obj_add_val(doc, obj, yyjson_mut_get_str(key_val),
                                   pairs[i].val);
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
    return write_json_into(out, doc);
}

} // namespace

void serialize_envelope(const wire_envelope& e, kimix::string& out) noexcept {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
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
    yyjson_doc* parsed = yyjson_read_opts((char*)e.payload_json.data(), e.payload_json.size(), 0, &kimix::llm::kYYJsonAlcMi, nullptr);
    if (parsed != nullptr) {
        payload_doc = yyjson_doc_mut_copy(parsed, &kimix::llm::kYYJsonAlcMi);
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

    write_json_into(out, doc);
    if (payload_doc != nullptr) {
        yyjson_mut_doc_free(payload_doc);
    }
    yyjson_mut_doc_free(doc);
}

bool deserialize_envelope(kimix::string_view frame, wire_envelope& out) noexcept {
    out.type.clear();
    out.payload_json.clear();
    yyjson_doc* doc = yyjson_read_opts((char*)frame.data(), frame.size(), 0, &kimix::llm::kYYJsonAlcMi, nullptr);
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
            // Compact re-serialization of the payload value, written
            // straight into out.payload_json (no intermediate buffer).
            ok = write_json_into(out.payload_json, payload_val);
        }
    }
    yyjson_doc_free(doc);
    return ok;
}

bool canonicalize_payload(kimix::string_view json, kimix::string& out) noexcept {
    out.clear();
    yyjson_doc* doc = yyjson_read_opts((char*)json.data(), json.size(), 0, &kimix::llm::kYYJsonAlcMi, nullptr);
    if (doc == nullptr) {
        return false;
    }
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (root == nullptr) {
        yyjson_doc_free(doc);
        return false;
    }
    // Convert the immutable doc into a mutable doc so we can sort in place.
    yyjson_mut_doc* mdoc = yyjson_doc_mut_copy(doc, &kimix::llm::kYYJsonAlcMi);
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
