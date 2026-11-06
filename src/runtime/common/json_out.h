/*
 * json_out.h - shared yyjson writer helpers for the runtime kernels
 * (kimix::runtime::common).
 *
 * The soul/session/index kernels all emit JSON with the mimalloc-backed
 * allocator (kimix::llm::kYYJsonAlcMi). Three of them need the same three
 * primitives - "add this string, sanitizing invalid UTF-8", "add this
 * already-serialized JSON value verbatim", "write the document out" - so they
 * live here once.
 *
 * ONE definition matters: runtime_py builds with unity build (batch_size = 8),
 * and two file-local copies in sibling namespaces make every unqualified call
 * ambiguous in the merged TU. Each kernel imports what it needs with a
 * using-declaration.
 *
 * Invalid UTF-8 is repaired lossily (one U+FFFD per invalid byte) rather than
 * rejected: yyjson refuses to write a document holding invalid UTF-8, and agent
 * text reaches the kernels as Python `str` bytes encoded with
 * errors="surrogatepass", which can carry lone surrogates.
 *
 * Header-only, noexcept where yyjson permits (yyjson itself never throws).
 */

#pragma once

#include <core/kimix_core.h>

#include <yyjson.h>

#include <llm/yyjson_alc.h>

#include <cstring>

namespace kimix {
namespace runtime {
namespace common {

inline bool json_utf8_valid(kimix::string_view s) noexcept {
    const char *it = s.data();
    const char *end = it + s.size();
    while (it < end) {
        const auto b0 = static_cast<unsigned char>(*it);
        if (b0 < 0x80) {
            ++it;
            continue;
        }
        size_t n = 0;
        if ((b0 & 0xE0) == 0xC0) {
            n = 2;
        } else if ((b0 & 0xF0) == 0xE0) {
            n = 3;
        } else if ((b0 & 0xF8) == 0xF0) {
            n = 4;
        } else {
            return false;
        }
        if (it + n > end) {
            return false;
        }
        uint32_t v = b0 & (0xFFu >> (n + 1));
        for (size_t k = 1; k < n; ++k) {
            const auto b = static_cast<unsigned char>(it[k]);
            if ((b & 0xC0) != 0x80) {
                return false;
            }
            v = (v << 6) | (b & 0x3Fu);
        }
        if ((n == 2 && v < 0x80) || (n == 3 && v < 0x800) ||
            (n == 4 && v < 0x10000) || (v >= 0xD800 && v <= 0xDFFF) ||
            v > 0x10FFFF) {
            return false;
        }
        it += n;
    }
    return true;
}

// Copy `s`, replacing every invalid byte with U+FFFD.
inline kimix::string json_utf8_lossy(kimix::string_view s) {
    kimix::string out;
    out.reserve(s.size() + 8);
    const char *it = s.data();
    const char *end = it + s.size();
    while (it < end) {
        const auto b0 = static_cast<unsigned char>(*it);
        if (b0 < 0x80) {
            out.push_back(*it++);
            continue;
        }
        size_t n = 0;
        if ((b0 & 0xE0) == 0xC0) {
            n = 2;
        } else if ((b0 & 0xF0) == 0xE0) {
            n = 3;
        } else if ((b0 & 0xF8) == 0xF0) {
            n = 4;
        }
        bool ok = n != 0 && it + n <= end;
        uint32_t v = 0;
        if (ok) {
            v = b0 & (0xFFu >> (n + 1));
            for (size_t k = 1; k < n; ++k) {
                const auto b = static_cast<unsigned char>(it[k]);
                if ((b & 0xC0) != 0x80) {
                    ok = false;
                    break;
                }
                v = (v << 6) | (b & 0x3Fu);
            }
        }
        if (ok) {
            ok = !((n == 2 && v < 0x80) || (n == 3 && v < 0x800) ||
                   (n == 4 && v < 0x10000) || (v >= 0xD800 && v <= 0xDFFF) ||
                   v > 0x10FFFF);
        }
        if (ok) {
            out.append(it, n);
            it += n;
        } else {
            out.append("\xEF\xBF\xBD", 3);
            ++it;
        }
    }
    return out;
}

// A JSON string value from a possibly-invalid UTF-8 view, COPIED into `doc`.
//
// The copy is load-bearing: `yyjson_mut_strn()` does NOT copy (the caller keeps
// the bytes alive until the document is written), and the kernels here pass
// short-lived buffers - a joined message text, a sanitized temporary, a
// `kimix::format()` result. `yyjson_mut_strncpy()` copies, so the value outlives
// the expression that produced it.
inline yyjson_mut_val *json_make_str(yyjson_mut_doc *doc,
                                     kimix::string_view value) {
    static const char kEmpty[] = "";
    const char *data = value.empty() ? kEmpty : value.data();
    if (json_utf8_valid(value)) {
        return yyjson_mut_strncpy(doc, data, value.size());
    }
    const kimix::string fixed = json_utf8_lossy(value);
    return yyjson_mut_strncpy(doc, fixed.data(), fixed.size());
}

// Add `obj[key] = "value"`.
inline void json_add_str(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                         const char *key, kimix::string_view value) {
    if (yyjson_mut_val *v = json_make_str(doc, value)) {
        yyjson_mut_obj_add_val(doc, obj, key, v);
    }
}

// Parse `raw` and deep-copy it under `key`; false when it is not JSON.
inline bool json_add_fragment(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                              const char *key, kimix::string_view raw) {
    if (raw.empty()) {
        return false;
    }
    kimix::string fixed;
    const char *data = raw.data();
    size_t len = raw.size();
    if (!json_utf8_valid(raw)) {
        fixed = json_utf8_lossy(raw);
        data = fixed.data();
        len = fixed.size();
    }
    yyjson_doc *parsed = yyjson_read_opts(const_cast<char *>(data), len, 0,
                                          &kimix::llm::kYYJsonAlcMi, nullptr);
    if (parsed == nullptr) {
        return false;
    }
    yyjson_mut_val *copy = yyjson_val_mut_copy(doc, yyjson_doc_get_root(parsed));
    yyjson_doc_free(parsed);
    if (copy == nullptr) {
        return false;
    }
    return yyjson_mut_obj_add_val(doc, obj, key, copy);
}

// Parse `raw` into a value owned by `doc` (nullptr when unparseable).
inline yyjson_mut_val *json_make_fragment(yyjson_mut_doc *doc,
                                          kimix::string_view raw) {
    kimix::string fixed;
    const char *data = raw.data();
    size_t len = raw.size();
    if (!json_utf8_valid(raw)) {
        fixed = json_utf8_lossy(raw);
        data = fixed.data();
        len = fixed.size();
    }
    yyjson_doc *parsed = yyjson_read_opts(const_cast<char *>(data), len, 0,
                                          &kimix::llm::kYYJsonAlcMi, nullptr);
    if (parsed == nullptr) {
        return nullptr;
    }
    yyjson_mut_val *copy = yyjson_val_mut_copy(doc, yyjson_doc_get_root(parsed));
    yyjson_doc_free(parsed);
    return copy;
}

// Serialize (compact) and release the document. False when yyjson failed; `out`
// then carries yyjson's reason ("invalid utf-8 encoding in string", ...) instead
// of the body, so a caller's error path can report why.
inline bool json_write(yyjson_mut_doc *doc, kimix::string &out) noexcept {
    if (doc == nullptr) {
        return false;
    }
    yyjson_write_err err;
    char *json = yyjson_mut_write_opts(doc, 0, &kimix::llm::kYYJsonAlcMi, nullptr,
                                       &err);
    bool ok = false;
    if (json != nullptr) {
        out.assign(json, strlen(json));
        kimix::deallocate(json);
        ok = true;
    } else if (err.msg != nullptr) {
        // Diagnostic for the caller's error path (the document is gone either
        // way, so the buffer is free to carry the reason).
        out.assign(err.msg, strlen(err.msg));
    }
    yyjson_mut_doc_free(doc);
    return ok;
}

} // namespace common
} // namespace runtime
} // namespace kimix
