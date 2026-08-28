// tool.cpp - Generic tool-parameter + tool-base infrastructure.
//
// Implements ToolParams::serialize / deserialize (and the non-throwing
// try_deserialize convenience) with the vendored yyjson library using the
// shared mimalloc-backed allocator kimix::llm::kYYJsonAlcMi (D3), plus the
// recursive ValueElement <-> yyjson converters and the out-of-line Tool
// destructor (vtable anchor).
//
// Unity-build rules (see tool.h): every helper here is file-local (anonymous
// namespace) and prefixed `tl_` so the concatenated kimix-llm translation
// unit cannot collide with other tools.

#include "builtin_tools/tool.h"

#include <mimalloc.h>
#include <yyjson.h>
#include "llm/yyjson_alc.h" // kimix::llm::kYYJsonAlcMi (mimalloc-backed)

#include <limits>
#include <stdexcept>
#include <utility>

namespace kimix::builtin_tools {

namespace {

// ── Recursive ValueElement <-> yyjson converters (TU-local, tl_ prefix) ─────

// Serializes one ValueElement into a mutable yyjson value owned by `doc`.
yyjson_mut_val *tl_to_json(yyjson_mut_doc *doc, const ValueElement &e) {
    if (e.is_null()) {
        return yyjson_mut_null(doc);
    }
    if (e.is_bool()) {
        return yyjson_mut_bool(doc, e.as_bool());
    }
    if (e.is_int()) {
        return yyjson_mut_int(doc, e.as_int());
    }
    if (e.is_uint()) {
        return yyjson_mut_uint(doc, e.as_uint());
    }
    if (e.is_real()) {
        return yyjson_mut_real(doc, e.as_real());
    }
    if (e.is_string()) {
        const kimix::string &s = e.as_string();
        // Copies the bytes (embedded NULs are preserved); the writer escapes
        // control characters when emitting.
        return yyjson_mut_strncpy(doc, s.data(), s.size());
    }
    if (e.is_array()) {
        yyjson_mut_val *arr = yyjson_mut_arr(doc);
        for (const ValueElement &elem : e.as_array()) {
            yyjson_mut_arr_append(arr, tl_to_json(doc, elem));
        }
        return arr;
    }
    // Object -> nested ToolParams.
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    const ToolParams *inner = e.as_object();
    if (inner != nullptr) {
        for (const auto &[k, v] : inner->values) {
            // yyjson copies the key into the document, so k.c_str() is safe.
            yyjson_mut_obj_add_val(doc, obj, k.c_str(), tl_to_json(doc, v));
        }
    }
    return obj;
}

// Parses one immutable yyjson value into a ValueElement (deep copy).
ValueElement tl_from_json(const yyjson_val *v) {
    switch (yyjson_get_type(v)) {
        case YYJSON_TYPE_NULL:
            return ValueElement::make_null();
        case YYJSON_TYPE_BOOL:
            return ValueElement::make_bool(yyjson_get_bool(v));
        case YYJSON_TYPE_NUM:
            // yyjson stores every non-negative integer as uint, so keep
            // uint64_t only for values beyond INT64_MAX; everything else that
            // is integral maps to int64_t, and anything with a fraction or
            // exponent maps to double. Preserves 1 as int and 1.0 as real.
            if (yyjson_is_uint(v)) {
                const uint64_t u = yyjson_get_uint(v);
                if (u <= static_cast<uint64_t>(
                             std::numeric_limits<int64_t>::max())) {
                    return ValueElement::make_int(static_cast<int64_t>(u));
                }
                return ValueElement::make_uint(u);
            }
            if (yyjson_is_sint(v)) {
                return ValueElement::make_int(yyjson_get_sint(v));
            }
            return ValueElement::make_real(yyjson_get_real(v));
        case YYJSON_TYPE_STR:
            return ValueElement::make_string(
                kimix::string(yyjson_get_str(v), yyjson_get_len(v)));
        case YYJSON_TYPE_ARR: {
            ValueElement::Array arr;
            size_t i, n;
            yyjson_val *item;
            yyjson_arr_foreach(v, i, n, item) {
                arr.push_back(tl_from_json(item));
            }
            return ValueElement::make_array(std::move(arr));
        }
        case YYJSON_TYPE_OBJ: {
            kimix::shared_ptr<ToolParams> obj(new ToolParams());
            size_t i, n;
            yyjson_val *key;
            yyjson_val *val;
            yyjson_obj_foreach(v, i, n, key, val) {
                kimix::string k(yyjson_get_str(key), yyjson_get_len(key));
                obj->values[std::move(k)] = tl_from_json(val);
            }
            return ValueElement::make_object(std::move(obj));
        }
        default:
            // Unreachable for values produced by a successful yyjson parse.
            return ValueElement::make_null();
    }
}

} // namespace

void ToolParams::serialize(kimix::vector<char> &out) const {
    out.clear();
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    if (doc == nullptr) {
        throw std::runtime_error(
            "ToolParams::serialize: failed to create yyjson document");
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    for (const auto &[k, v] : values) {
        yyjson_mut_obj_add_val(doc, root, k.c_str(), tl_to_json(doc, v));
    }

    size_t len = 0;
    // Pass the mimalloc allocator explicitly: yyjson_mut_write() itself falls
    // back to YYJSON_DEFAULT_ALC (malloc), so the buffer must come from
    // write_opts with kYYJsonAlcMi to keep the mi_free contract (D3).
    char *json = yyjson_mut_write_opts(doc, 0 /* compact */,
                                       &kimix::llm::kYYJsonAlcMi, &len, nullptr);
    if (json == nullptr) {
        yyjson_mut_doc_free(doc);
        throw std::runtime_error(
            "ToolParams::serialize: failed to serialize JSON");
    }
    // The write buffer was allocated through the mimalloc allocator passed to
    // write_opts: release with mi_free, never free(). Copy before freeing.
    out.assign(json, json + len);
    mi_free(json);
    yyjson_mut_doc_free(doc);
}

void ToolParams::deserialize(kimix::span<char const> in) {
    yyjson_read_err err{};
    yyjson_doc *doc = yyjson_read_opts(const_cast<char *>(in.data()), in.size(),
                                       0 /* no flags: strict, stop-on-error */,
                                       &kimix::llm::kYYJsonAlcMi, &err);
    if (doc == nullptr) {
        kimix::string msg = "ToolParams::deserialize: invalid JSON: ";
        msg += (err.msg != nullptr) ? err.msg : "unknown error";
        throw std::runtime_error(msg.c_str());
    }
    // RAII-style cleanup: doc is released on every exit path, including throws.
    struct doc_guard {
        yyjson_doc *d;
        ~doc_guard() {
            if (d != nullptr) {
                yyjson_doc_free(d);
            }
        }
    } guard{doc};

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root == nullptr || !yyjson_is_obj(root)) {
        throw std::runtime_error(
            "ToolParams::deserialize: root must be a JSON object");
    }
    values.clear();
    size_t i, n;
    yyjson_val *key;
    yyjson_val *val;
    yyjson_obj_foreach(root, i, n, key, val) {
        kimix::string k(yyjson_get_str(key), yyjson_get_len(key));
        values[std::move(k)] = tl_from_json(val);
    }
}

bool ToolParams::try_deserialize(kimix::span<char const> in,
                                 kimix::string &error) {
    error.clear();
    try {
        deserialize(in);
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}

Tool::~Tool() = default; // out-of-line: anchors the vtable in kimix-llm

} // namespace kimix::builtin_tools
