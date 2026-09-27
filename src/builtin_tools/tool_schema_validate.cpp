// tool_schema_validate.cpp - Light-weight JSON-Schema meta-validation
// (see tool_schema_validate.h).

#include "builtin_tools/tool_schema_validate.h"

#include <llm/yyjson_alc.h>
#include "yyjson.h"

namespace kimix::builtin_tools {
namespace schema_validate {

namespace {

// The meta-schema's $defs.simpleTypes enum (Draft 2020-12).
bool is_simple_type(kimix::string_view type) {
    return type == "object" || type == "array" || type == "string" ||
           type == "number" || type == "integer" || type == "boolean" ||
           type == "null";
}

// Keywords whose value is a single schema (validated recursively).
constexpr kimix::string_view kSchemaValueKeywords[] = {
    "items",                 "additionalProperties",  "not",
    "if",                    "then",                  "else",
    "contains",              "propertyNames",         "unevaluatedItems",
    "unevaluatedProperties", "contentSchema",
};

// Keywords whose value is an array of schemas.
constexpr kimix::string_view kSchemaArrayKeywords[] = {
    "prefixItems", "allOf", "anyOf", "oneOf"};

// Keywords whose value is an object mapping names to schemas.
constexpr kimix::string_view kSchemaMapKeywords[] = {
    "properties", "patternProperties", "$defs", "definitions",
    "dependentSchemas"};

// Appends " at <path>" style context to an error message.
void at_path(kimix::string &error, kimix::string_view path) {
    if (!path.empty()) {
        error += " at ";
        error += path;
    }
}

} // namespace

bool validate_schema_value(yyjson_val *root, kimix::string &error) {
    // A boolean schema is valid (true/false).
    if (yyjson_is_bool(root)) {
        return true;
    }
    if (root == nullptr || !yyjson_is_obj(root)) {
        error = "parameters schema: the schema must be an object";
        return false;
    }
    const auto child_path = [](kimix::string_view path,
                               kimix::string_view key) {
        kimix::string out(path);
        if (!out.empty()) {
            out += ".";
        }
        out += key;
        return out;
    };

    // "type": string or array of simpleTypes strings.
    if (yyjson_val *type = yyjson_obj_get(root, "type")) {
        if (yyjson_is_str(type)) {
            const kimix::string_view value(yyjson_get_str(type),
                                           yyjson_get_len(type));
            if (!is_simple_type(value)) {
                error = "parameters schema: unknown type '";
                error += value;
                error += "'";
                at_path(error, "type");
                return false;
            }
        } else if (yyjson_is_arr(type)) {
            yyjson_arr_iter iter;
            yyjson_arr_iter_init(type, &iter);
            yyjson_val *item = nullptr;
            while ((item = yyjson_arr_iter_next(&iter)) != nullptr) {
                if (!yyjson_is_str(item) ||
                    !is_simple_type(kimix::string_view(yyjson_get_str(item),
                                                       yyjson_get_len(item)))) {
                    error =
                        "parameters schema: 'type' array must contain "
                        "type names";
                    at_path(error, "type");
                    return false;
                }
            }
        } else {
            error = "parameters schema: 'type' must be a string or array of "
                    "strings";
            return false;
        }
    }

    // Schema-valued / schema-array / schema-map keywords.
    for (kimix::string_view key : kSchemaValueKeywords) {
        yyjson_val *value = yyjson_obj_getn(root, key.data(), key.size());
        if (value == nullptr) {
            continue;
        }
        if (yyjson_is_arr(value)) {
            // Draft 2019-07 style "items": [schema, ...] is accepted too.
            yyjson_arr_iter iter;
            yyjson_arr_iter_init(value, &iter);
            yyjson_val *item = nullptr;
            while ((item = yyjson_arr_iter_next(&iter)) != nullptr) {
                if (!validate_schema_value(item, error)) {
                    at_path(error, child_path("", key));
                    return false;
                }
            }
            continue;
        }
        if (!validate_schema_value(value, error)) {
            at_path(error, key);
            return false;
        }
    }
    for (kimix::string_view key : kSchemaArrayKeywords) {
        yyjson_val *value = yyjson_obj_getn(root, key.data(), key.size());
        if (value == nullptr) {
            continue;
        }
        if (!yyjson_is_arr(value)) {
            error = "parameters schema: '";
            error += key;
            error += "' must be an array";
            return false;
        }
        yyjson_arr_iter iter;
        yyjson_arr_iter_init(value, &iter);
        yyjson_val *item = nullptr;
        while ((item = yyjson_arr_iter_next(&iter)) != nullptr) {
            if (!validate_schema_value(item, error)) {
                at_path(error, child_path("", key));
                return false;
            }
        }
    }
    for (kimix::string_view key : kSchemaMapKeywords) {
        yyjson_val *value = yyjson_obj_getn(root, key.data(), key.size());
        if (value == nullptr) {
            continue;
        }
        if (!yyjson_is_obj(value)) {
            error = "parameters schema: '";
            error += key;
            error += "' must be an object";
            return false;
        }
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(value, &iter);
        yyjson_val *k = nullptr;
        while ((k = yyjson_obj_iter_next(&iter)) != nullptr) {
            if (!validate_schema_value(yyjson_obj_iter_get_val(k), error)) {
                at_path(error, child_path(key, kimix::string_view(
                                                    yyjson_get_str(k),
                                                    yyjson_get_len(k))));
                return false;
            }
        }
    }

    // "required": array of strings.
    if (yyjson_val *required = yyjson_obj_get(root, "required")) {
        if (!yyjson_is_arr(required)) {
            error = "parameters schema: 'required' must be an array of strings";
            return false;
        }
        yyjson_arr_iter iter;
        yyjson_arr_iter_init(required, &iter);
        yyjson_val *item = nullptr;
        while ((item = yyjson_arr_iter_next(&iter)) != nullptr) {
            if (!yyjson_is_str(item) || yyjson_get_len(item) == 0) {
                error = "parameters schema: 'required' must contain only "
                        "non-empty strings";
                return false;
            }
        }
    }
    // "enum": a non-empty array.
    if (yyjson_val *enumeration = yyjson_obj_get(root, "enum")) {
        if (!yyjson_is_arr(enumeration) || yyjson_arr_size(enumeration) == 0) {
            error = "parameters schema: 'enum' must be a non-empty array";
            return false;
        }
    }
    return true;
}

bool validate_parameters_schema(kimix::string_view schema_json,
                                kimix::string &error) {
    // kosong/tooling/__init__.py:33-52: an empty/absent schema never reaches
    // the validator (the soul substitutes {"type":"object"}); here the empty
    // text is valid-by-absence for the same reason.
    if (schema_json.empty()) {
        return true;
    }
    kimix::string buffer(schema_json);
    // NOTE: this validator runs from the KIMIX_REGISTER_TOOL_* static
    // constructors, i.e. before main() and possibly before the process-wide
    // kYYJsonAlcMi inline variable is initialized (static-init order across
    // unity-batch TUs is unspecified). yyjson falls back to its default
    // malloc allocator when no alc is given, which has no init-order
    // dependency; the document is freed before returning.
    yyjson_doc *doc = yyjson_read_opts(buffer.data(), buffer.size(),
                                       YYJSON_READ_STOP_WHEN_DONE, nullptr,
                                       nullptr);
    if (doc == nullptr) {
        error = "parameters schema: not parseable JSON";
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    const bool ok = validate_schema_value(root, error);
    yyjson_doc_free(doc);
    return ok;
}

} // namespace schema_validate
} // namespace kimix::builtin_tools
