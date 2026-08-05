/*
 * json_kernel_util.h - shared JSON kernel helpers (kimix::runtime::json).
 *
 * Inline helpers shared by the json kernels (json_store, schema_ops) so the
 * unity build never hits redefinition collisions. All functions are inline.
 */

#pragma once

#include <core/kimix_core.h>

#include <yyjson.h>

#include <cstdlib>

#include <runtime/common/json_pretty.h>

namespace kimix {
namespace runtime {
namespace json {

// Compact-serialize a mutable doc root (malloc'd by yyjson, freed here).
inline bool json_write_compact(yyjson_mut_doc* doc, kimix::string& out) noexcept {
    size_t len = 0;
    char* text = yyjson_mut_write(doc, 0, &len);
    if (text == nullptr) {
        return false;
    }
    out.assign(text, len);
    free(text);
    return true;
}

// orjson OPT_INDENT_2-serialize a mutable doc root. The trimmed yyjson fork's
// yyjson_mut_doc_imut_copy is unreliable (see T6), so we serialize compactly
// first and re-parse the immutable doc for the pretty printer.
inline void json_write_pretty(yyjson_mut_doc* doc, kimix::string& out) noexcept {
    out.clear();
    kimix::string compact;
    if (!json_write_compact(doc, compact)) {
        return;
    }
    yyjson_doc* parsed = yyjson_read(compact.data(), compact.size(), 0);
    if (parsed == nullptr) {
        return;
    }
    common::pretty_write_doc(parsed, out);
    yyjson_doc_free(parsed);
}

} // namespace json
} // namespace runtime
} // namespace kimix
