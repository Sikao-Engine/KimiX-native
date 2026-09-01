/*
 * json_pretty.h - orjson OPT_INDENT_2-compatible JSON pretty printer
 * (kimix::runtime::common).
 *
 * Shared by the JSON store (atomic_json_write parity: kimi_cli writes
 * orjson.dumps(data, option=orjson.OPT_INDENT_2)) and the export markdown
 * builder (tool-call arguments). Format verified against orjson 3.11:
 *
 *   - 2-space indent per level, "key": value with ": " separator
 *   - ",\n" between pairs/items; empty containers stay inline ("{}", "[]")
 *   - strings escaped with append_json_escaped semantics (raw UTF-8,
 *     \u00XX lowercase hex for control bytes)
 *   - integers as decimal; floats via std::to_chars shortest round-trip
 *     (same algorithm family as Python float repr / orjson) with ".0"
 *     appended when the shortest repr has no '.'/'e' (orjson keeps the
 *     float marker for integral values)
 *
 * Operates on IMMUTABLE yyjson values; the store converts its mut doc with
 * yyjson_mut_doc_imut_copy before printing (one extra copy per save).
 */

#pragma once

#include <core/kimix_core.h>

#include <yyjson.h>

#include <charconv>
#include <cstdlib>

#include <runtime/common/text_util.h>

namespace kimix {
namespace runtime {
namespace common {
namespace json_pretty_detail {

inline void append_indent(kimix::string& out, size_t level) noexcept {
    out.append(level * 2, ' ');
}

inline void append_real(kimix::string& out, double value) noexcept {
    char buf[64];
    const auto res =
        std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::general);
    if (res.ec == std::errc()) {
        const size_t len = static_cast<size_t>(res.ptr - buf);
        bool has_dot_or_exp = false;
        for (size_t i = 0; i < len; ++i) {
            if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E') {
                has_dot_or_exp = true;
                break;
            }
        }
        out.append(buf, len);
        if (!has_dot_or_exp) {
            out += ".0"; // orjson keeps the float marker for integral floats
        }
        return;
    }
    out += "0.0"; // unreachable for finite doubles
}

} // namespace json_pretty_detail

// Serialize one immutable value with 2-space indentation starting at
// `level`. Appends to `out`.
inline void pretty_write_val(const yyjson_val* val, size_t level,
                             kimix::string& out) noexcept {
    using namespace json_pretty_detail;
    if (val == nullptr) {
        out += "null";
        return;
    }
    const yyjson_type type = yyjson_get_type(val);
    switch (type) {
    case YYJSON_TYPE_OBJ: {
        const size_t size = yyjson_obj_size(val);
        if (size == 0) {
            out += "{}";
            return;
        }
        out += "{\n";
        size_t idx = 0;
        size_t max = 0;
        yyjson_val* key = nullptr;
        yyjson_val* item = nullptr;
        yyjson_obj_foreach(val, idx, max, key, item) {
            append_indent(out, level + 1);
            out += '"';
            append_json_escaped(
                out, kimix::string_view(yyjson_get_str(key),
                                        static_cast<size_t>(yyjson_get_len(key))));
            out += "\": ";
            pretty_write_val(item, level + 1, out);
            if (idx + 1 < max) {
                out += ',';
            }
            out += '\n';
        }
        append_indent(out, level);
        out += '}';
        return;
    }
    case YYJSON_TYPE_ARR: {
        const size_t size = yyjson_arr_size(val);
        if (size == 0) {
            out += "[]";
            return;
        }
        out += "[\n";
        size_t idx = 0;
        size_t max = 0;
        yyjson_val* item = nullptr;
        yyjson_arr_foreach(val, idx, max, item) {
            append_indent(out, level + 1);
            pretty_write_val(item, level + 1, out);
            if (idx + 1 < max) {
                out += ',';
            }
            out += '\n';
        }
        append_indent(out, level);
        out += ']';
        return;
    }
    case YYJSON_TYPE_STR:
        out += '"';
        append_json_escaped(
            out, kimix::string_view(yyjson_get_str(val),
                                    static_cast<size_t>(yyjson_get_len(val))));
        out += '"';
        return;
    case YYJSON_TYPE_NUM: {
        if (yyjson_is_uint(val)) {
            // to_chars into a stack buffer: std::to_string allocates a new
            // string per number, which dominates printing of int-heavy docs.
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
            append_real(out, yyjson_get_real(val));
        }
        return;
    }
    case YYJSON_TYPE_BOOL:
        out += yyjson_is_true(val) ? "true" : "false";
        return;
    case YYJSON_TYPE_NULL:
        out += "null";
        return;
    default:
        out += "null";
        return;
    }
}

// Serialize a parsed immutable document root with 2-space indent.
inline void pretty_write_doc(const yyjson_doc* doc, kimix::string& out) noexcept {
    if (doc == nullptr) {
        return;
    }
    pretty_write_val(yyjson_doc_get_root(doc), 0, out);
}

} // namespace common
} // namespace runtime
} // namespace kimix
