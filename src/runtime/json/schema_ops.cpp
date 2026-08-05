/*
 * schema_ops.cpp - see schema_ops.h (plan 016).
 *
 * Exact ports of kosong/utils/jsonschema.py::deref_json_schema and
 * ensure_property_types. The input is parsed ONCE (immutable yyjson); the
 * result is built into a fresh mutable doc (deep-copy semantics -- the
 * caller's bytes are never mutated) and serialized compactly.
 */

#include <runtime/json/schema_ops.h>

#include <yyjson.h>

#include <cstdlib>

#include <runtime/json/json_kernel_util.h>

namespace kimix {
namespace runtime {
namespace json {
namespace {

// ---------------------------------------------------------------------------
// deref_json_schema
// ---------------------------------------------------------------------------

struct deref_ctx {
    const yyjson_doc* schema_doc = nullptr; // immutable input (pointer target)
    yyjson_mut_doc* out_doc = nullptr;      // result document
    kimix::set<kimix::string> visited;      // refs currently being resolved
};

// _JSON_POINTER_INDEX_RE: "0" or [1-9]\d*
bool is_array_index(kimix::string_view part) noexcept {
    if (part.empty()) {
        return false;
    }
    if (part.size() > 1 && part[0] == '0') {
        return false;
    }
    for (char c : part) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

// resolve_pointer(ref, ctx) -> target immutable value or nullptr. `ref` is
// "#" or "#/...". Returns a pointer into the input schema doc.
const yyjson_val* resolve_pointer(const yyjson_val* full_schema,
                                  kimix::string_view ref) noexcept {
    if (ref == "#") {
        return full_schema;
    }
    const yyjson_val* current = full_schema;
    // ref[2:] skips "#/"
    size_t pos = 2;
    while (pos < ref.size()) {
        size_t slash = ref.find('/', pos);
        const size_t end = (slash == kimix::string_view::npos) ? ref.size() : slash;
        kimix::string_view raw_part = ref.substr(pos, end - pos);
        // JSON pointer unescaping: ~1 -> /, ~0 -> ~
        kimix::string part;
        part.reserve(raw_part.size());
        for (size_t i = 0; i < raw_part.size(); ++i) {
            if (raw_part[i] == '~' && i + 1 < raw_part.size()) {
                if (raw_part[i + 1] == '1') {
                    part.push_back('/');
                    ++i;
                    continue;
                }
                if (raw_part[i + 1] == '0') {
                    part.push_back('~');
                    ++i;
                    continue;
                }
            }
            part.push_back(raw_part[i]);
        }
        if (current != nullptr && yyjson_is_obj(current)) {
            const yyjson_val* next = yyjson_obj_get(current, part.c_str());
            if (next == nullptr) {
                return nullptr;
            }
            current = next;
        } else if (current != nullptr && yyjson_is_arr(current)) {
            if (!is_array_index(part)) {
                return nullptr;
            }
            size_t index = 0;
            for (char c : part) {
                index = index * 10 + static_cast<size_t>(c - '0');
            }
            if (index >= yyjson_arr_size(current)) {
                return nullptr;
            }
            current = yyjson_arr_get(current, index);
        } else {
            return nullptr;
        }
        if (slash == kimix::string_view::npos) {
            break;
        }
        pos = slash + 1;
    }
    return current;
}

// Copy one immutable value into the output doc (deep).
yyjson_mut_val* copy_value(deref_ctx& ctx, const yyjson_val* val) noexcept;

// traverse(node): inline local refs.
yyjson_mut_val* traverse(deref_ctx& ctx, const yyjson_val* node) noexcept {
    if (node == nullptr) {
        return nullptr;
    }
    if (yyjson_is_obj(node)) {
        const yyjson_val* ref = yyjson_obj_get(node, "$ref");
        if (ref != nullptr && yyjson_is_str(ref)) {
            kimix::string_view ref_str(yyjson_get_str(ref),
                                       static_cast<size_t>(yyjson_get_len(ref)));
            if (ref_str == "#" || (ref_str.size() > 1 && ref_str[0] == '#' &&
                                   ref_str[1] == '/')) {
                kimix::string ref_key(ref_str);
                if (ctx.visited.find(ref_key) != ctx.visited.end()) {
                    // Circular reference -- keep the $ref as-is.
                    return copy_value(ctx, node);
                }
                const yyjson_val* target =
                    resolve_pointer(yyjson_doc_get_root(ctx.schema_doc), ref_str);
                if (target != nullptr) {
                    ctx.visited.insert(ref_key);
                    yyjson_mut_val* resolved = traverse(ctx, target);
                    ctx.visited.erase(ref_key);
                    if (resolved != nullptr && yyjson_mut_is_obj(resolved)) {
                        // Merge sibling keywords over the resolved definition
                        // (local keys take precedence).
                        yyjson_mut_val* merged = yyjson_mut_obj(ctx.out_doc);
                        size_t idx = 0;
                        size_t max = 0;
                        yyjson_mut_val* key = nullptr;
                        yyjson_mut_val* item = nullptr;
                        // Deep-copy each resolved pair into `merged` (a
                        // borrowed item would alias the source object's child
                        // chain and corrupt the writer -- T6 lesson).
                        yyjson_mut_obj_foreach(resolved, idx, max, key, item) {
                            yyjson_mut_val* key_copy = yyjson_mut_strncpy(
                                ctx.out_doc, yyjson_mut_get_str(key),
                                yyjson_mut_get_len(key));
                            yyjson_mut_val* item_copy = yyjson_mut_val_mut_copy(
                                ctx.out_doc, item);
                            yyjson_mut_obj_add_val(ctx.out_doc, merged,
                                                   yyjson_mut_get_str(key_copy), item_copy);
                        }
                        // Sibling keys (everything except "$ref").
                        idx = 0;
                        max = 0;
                        yyjson_val* skey = nullptr;
                        yyjson_val* sval = nullptr;
                        yyjson_obj_foreach(node, idx, max, skey, sval) {
                            kimix::string_view k(yyjson_get_str(skey),
                                                 static_cast<size_t>(yyjson_get_len(skey)));
                            if (k == "$ref") {
                                continue;
                            }
                            yyjson_mut_val* key_copy = yyjson_mut_strncpy(
                                ctx.out_doc, k.data(), k.size());
                            yyjson_mut_val* tval = traverse(ctx, sval);
                            yyjson_mut_obj_add_val(ctx.out_doc, merged,
                                                   yyjson_mut_get_str(key_copy), tval);
                        }
                        return merged;
                    }
                    return resolved;
                }
            }
            // Remote or unresolvable reference -- leave the node as-is.
            return copy_value(ctx, node);
        }
        // Regular object: rebuild with traversed values.
        yyjson_mut_val* obj = yyjson_mut_obj(ctx.out_doc);
        size_t idx = 0;
        size_t max = 0;
        yyjson_val* key = nullptr;
        yyjson_val* val = nullptr;
        yyjson_obj_foreach(node, idx, max, key, val) {
            yyjson_mut_val* key_copy = yyjson_mut_strncpy(
                ctx.out_doc, yyjson_get_str(key),
                static_cast<size_t>(yyjson_get_len(key)));
            yyjson_mut_val* tval = traverse(ctx, val);
            yyjson_mut_obj_add_val(ctx.out_doc, obj, yyjson_mut_get_str(key_copy), tval);
        }
        return obj;
    }
    if (yyjson_is_arr(node)) {
        yyjson_mut_val* arr = yyjson_mut_arr(ctx.out_doc);
        size_t idx = 0;
        size_t max = 0;
        yyjson_val* item = nullptr;
        yyjson_arr_foreach(node, idx, max, item) {
            yyjson_mut_val* tval = traverse(ctx, item);
            yyjson_mut_arr_append(arr, tval);
        }
        return arr;
    }
    return copy_value(ctx, node);
}

yyjson_mut_val* copy_value(deref_ctx& ctx, const yyjson_val* val) noexcept {
    return yyjson_val_mut_copy(ctx.out_doc, val);
}

// _has_unresolved_definition_ref: any $ref "#/<bucket>/..." outside the
// bucket itself.
bool has_unresolved_definition_ref(const yyjson_mut_val* node,
                                   kimix::string_view bucket_key) noexcept {
    if (node == nullptr) {
        return false;
    }
    if (yyjson_mut_is_arr(node)) {
        size_t idx = 0;
        size_t max = 0;
        yyjson_mut_val* item = nullptr;
        yyjson_mut_arr_foreach(node, idx, max, item) {
            if (has_unresolved_definition_ref(item, bucket_key)) {
                return true;
            }
        }
        return false;
    }
    if (yyjson_mut_is_obj(node)) {
        const yyjson_mut_val* ref = yyjson_mut_obj_get(node, "$ref");
        if (ref != nullptr && yyjson_mut_is_str(ref)) {
            kimix::string_view r(yyjson_mut_get_str(ref),
                                 static_cast<size_t>(yyjson_mut_get_len(ref)));
            // ref.startswith("#/<bucket>/")
            const kimix::string prefix("#/");
            const size_t need = prefix.size() + bucket_key.size() + 1;
            if (r.size() >= need && r.substr(0, prefix.size()) == prefix &&
                r.substr(prefix.size(), bucket_key.size()) == bucket_key &&
                r[prefix.size() + bucket_key.size()] == '/') {
                return true;
            }
        }
        size_t idx = 0;
        size_t max = 0;
        yyjson_mut_val* key = nullptr;
        yyjson_mut_val* item = nullptr;
        yyjson_mut_obj_foreach(node, idx, max, key, item) {
            kimix::string_view k(yyjson_mut_get_str(key),
                                 static_cast<size_t>(yyjson_mut_get_len(key)));
            if (k != bucket_key && has_unresolved_definition_ref(item, bucket_key)) {
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// ensure_property_types
// ---------------------------------------------------------------------------

// Child-schema slots from jsonschema.py _CHILD_SCHEMA_SLOTS: (key, kind)
// where kind: 0=single 1=array 2=map 3=schema-or-array.
struct child_slot {
    const char* key;
    uint8_t kind;
};

constexpr child_slot kChildSlots[] = {
    {"$defs", 2},              {"definitions", 2},          {"dependencies", 2},
    {"dependentSchemas", 2},   {"patternProperties", 2},    {"properties", 2},
    {"additionalItems", 0},    {"additionalProperties", 0}, {"contains", 0},
    {"contentSchema", 0},      {"else", 0},                 {"if", 0},
    {"not", 0},                {"propertyNames", 0},        {"then", 0},
    {"unevaluatedItems", 0},   {"unevaluatedProperties", 0},{"allOf", 1},
    {"anyOf", 1},              {"oneOf", 1},                {"prefixItems", 1},
    {"items", 3},
};

// _TYPE_COMPLETION_SKIP_KEYS
bool is_skip_key(kimix::string_view k) noexcept {
    return k == "$ref" || k == "allOf" || k == "anyOf" || k == "else" ||
           k == "if" || k == "not" || k == "oneOf" || k == "then";
}

// Structural keyword sets (member checks; the lists are tiny).
bool has_any_key(const yyjson_mut_val* obj, const char* const* keys,
                 size_t count) noexcept {
    for (size_t i = 0; i < count; ++i) {
        if (yyjson_mut_obj_get(obj, keys[i]) != nullptr) {
            return true;
        }
    }
    return false;
}

constexpr const char* kObjectStructureKeys[] = {
    "additionalProperties", "dependentRequired", "dependentSchemas", "maxProperties",
    "minProperties",        "patternProperties", "properties",       "required",
    "unevaluatedProperties",
};

constexpr const char* kArrayStructureKeys[] = {
    "additionalItems", "contains", "items",   "maxContains", "maxItems",
    "minContains",     "minItems", "prefixItems", "unevaluatedItems", "uniqueItems",
};

constexpr const char* kStringStructureKeys[] = {
    "contentEncoding", "contentMediaType", "contentSchema", "format",
    "maxLength",       "minLength",        "pattern",
};

constexpr const char* kNumericStructureKeys[] = {
    "exclusiveMaximum", "exclusiveMinimum", "maximum", "minimum", "multipleOf",
};

// _classify_value -> JSON Schema type string (bool BEFORE int).
const char* classify_value(const yyjson_mut_val* v) noexcept {
    if (v == nullptr) {
        return nullptr;
    }
    switch (yyjson_mut_get_type(v)) {
    case YYJSON_TYPE_BOOL:
        return "boolean";
    case YYJSON_TYPE_STR:
        return "string";
    case YYJSON_TYPE_NULL:
        return "null";
    case YYJSON_TYPE_OBJ:
        return "object";
    case YYJSON_TYPE_ARR:
        return "array";
    case YYJSON_TYPE_NUM:
        if (yyjson_mut_is_uint(v) || yyjson_mut_is_sint(v)) {
            return "integer";
        }
        return "number";
    default:
        return nullptr;
    }
}

// _infer_type_from_values: single type -> it; {integer, number} -> number;
// anything else -> "string".
kimix::string infer_type_from_values(const yyjson_mut_val* values) noexcept {
    kimix::set<kimix::string> inferred;
    size_t idx = 0;
    size_t max = 0;
    yyjson_mut_val* item = nullptr;
    yyjson_mut_arr_foreach(values, idx, max, item) {
        const char* kind = classify_value(item);
        if (kind == nullptr) {
            return "string";
        }
        inferred.insert(kind);
    }
    if (inferred.size() == 1) {
        return *inferred.begin();
    }
    if (inferred.size() == 2 && inferred.find("integer") != inferred.end() &&
        inferred.find("number") != inferred.end()) {
        return "number";
    }
    return "string";
}

// _try_infer_single_type: strict; empty string on mixed/unclassifiable.
kimix::string try_infer_single_type(const yyjson_mut_val* values) noexcept {
    kimix::set<kimix::string> inferred;
    size_t idx = 0;
    size_t max = 0;
    yyjson_mut_val* item = nullptr;
    yyjson_mut_arr_foreach(values, idx, max, item) {
        const char* kind = classify_value(item);
        if (kind == nullptr) {
            return kimix::string();
        }
        inferred.insert(kind);
    }
    if (inferred.find("number") != inferred.end()) {
        inferred.erase("integer"); // integer is a subset of number
    }
    if (inferred.size() == 1) {
        return *inferred.begin();
    }
    return kimix::string();
}

// _infer_type_from_structure
const char* infer_type_from_structure(const yyjson_mut_val* node) noexcept {
    if (has_any_key(node, kObjectStructureKeys,
                    sizeof(kObjectStructureKeys) / sizeof(kObjectStructureKeys[0]))) {
        return "object";
    }
    if (has_any_key(node, kArrayStructureKeys,
                    sizeof(kArrayStructureKeys) / sizeof(kArrayStructureKeys[0]))) {
        return "array";
    }
    if (has_any_key(node, kStringStructureKeys,
                    sizeof(kStringStructureKeys) / sizeof(kStringStructureKeys[0]))) {
        return "string";
    }
    if (has_any_key(node, kNumericStructureKeys,
                    sizeof(kNumericStructureKeys) / sizeof(kNumericStructureKeys[0]))) {
        return "number";
    }
    return "string";
}

// _remove_irrelevant_structure_keys: rebuild `node` dropping object/array
// structure keywords that no longer apply after a type repair.
void remove_irrelevant_structure_keys(yyjson_mut_doc* doc, yyjson_mut_val* node,
                                      kimix::string_view new_type) noexcept {
    const bool drop_object = new_type != "object";
    const bool drop_array = new_type != "array";
    if (!drop_object && !drop_array) {
        return;
    }
    // Collect surviving pairs.
    struct pair_t {
        kimix::string key;
        yyjson_mut_val* val;
    };
    kimix::vector<pair_t> pairs;
    size_t idx = 0;
    size_t max = 0;
    yyjson_mut_val* key = nullptr;
    yyjson_mut_val* item = nullptr;
    yyjson_mut_obj_foreach(node, idx, max, key, item) {
        kimix::string_view k(yyjson_mut_get_str(key),
                             static_cast<size_t>(yyjson_mut_get_len(key)));
        bool drop = false;
        if (drop_object) {
            for (const char* ok : kObjectStructureKeys) {
                if (k == ok) {
                    drop = true;
                    break;
                }
            }
        }
        if (!drop && drop_array) {
            for (const char* ak : kArrayStructureKeys) {
                if (k == ak) {
                    drop = true;
                    break;
                }
            }
        }
        if (!drop) {
            pair_t p;
            p.key.assign(k.data(), k.size());
            p.val = item;
            pairs.push_back(std::move(p));
        }
    }
    yyjson_mut_obj_clear(node);
    for (const pair_t& p : pairs) {
        yyjson_mut_val* key_copy =
            yyjson_mut_strncpy(doc, p.key.data(), p.key.size());
        yyjson_mut_obj_add_val(doc, node, yyjson_mut_get_str(key_copy), p.val);
    }
}

void recurse_schema(yyjson_mut_doc* doc, yyjson_mut_val* node) noexcept;
void normalize_property(yyjson_mut_doc* doc, yyjson_mut_val* node) noexcept;

void recurse_schema(yyjson_mut_doc* doc, yyjson_mut_val* node) noexcept {
    if (node == nullptr || !yyjson_mut_is_obj(node)) {
        return;
    }
    for (const child_slot& slot : kChildSlots) {
        yyjson_mut_val* value = yyjson_mut_obj_get(node, slot.key);
        if (value == nullptr) {
            continue;
        }
        if (slot.kind == 0) { // single
            if (yyjson_mut_is_obj(value)) {
                normalize_property(doc, value);
            }
        } else if (slot.kind == 1) { // array
            if (yyjson_mut_is_arr(value)) {
                size_t idx = 0;
                size_t max = 0;
                yyjson_mut_val* item = nullptr;
                yyjson_mut_arr_foreach(value, idx, max, item) {
                    normalize_property(doc, item);
                }
            }
        } else if (slot.kind == 2) { // map
            if (yyjson_mut_is_obj(value)) {
                size_t idx = 0;
                size_t max = 0;
                yyjson_mut_val* key = nullptr;
                yyjson_mut_val* item = nullptr;
                yyjson_mut_obj_foreach(value, idx, max, key, item) {
                    normalize_property(doc, item);
                }
            }
        } else { // schema-or-array
            if (yyjson_mut_is_obj(value)) {
                normalize_property(doc, value);
            } else if (yyjson_mut_is_arr(value)) {
                size_t idx = 0;
                size_t max = 0;
                yyjson_mut_val* item = nullptr;
                yyjson_mut_arr_foreach(value, idx, max, item) {
                    normalize_property(doc, item);
                }
            }
        }
    }
}

void normalize_property(yyjson_mut_doc* doc, yyjson_mut_val* node) noexcept {
    if (node == nullptr || !yyjson_mut_is_obj(node)) {
        return;
    }
    bool has_skip_keys = false;
    {
        size_t idx = 0;
        size_t max = 0;
        yyjson_mut_val* key = nullptr;
        yyjson_mut_val* ignore = nullptr;
        yyjson_mut_obj_foreach(node, idx, max, key, ignore) {
            kimix::string_view k(yyjson_mut_get_str(key),
                                 static_cast<size_t>(yyjson_mut_get_len(key)));
            if (is_skip_key(k)) {
                has_skip_keys = true;
                break;
            }
        }
    }

    yyjson_mut_val* type_val = yyjson_mut_obj_get(node, "type");
    const bool has_type = type_val != nullptr && yyjson_mut_is_str(type_val);

    if (!has_type && !has_skip_keys) {
        // Infer and add `type`.
        yyjson_mut_val* enum_val = yyjson_mut_obj_get(node, "enum");
        const char* inferred = nullptr;
        if (enum_val != nullptr && yyjson_mut_is_arr(enum_val) &&
            yyjson_mut_arr_size(enum_val) > 0) {
            kimix::string t = infer_type_from_values(enum_val);
            yyjson_mut_obj_add_strncpy(doc, node, "type", t.data(), t.size());
            recurse_schema(doc, node);
            return;
        }
        yyjson_mut_val* const_val = yyjson_mut_obj_get(node, "const");
        if (const_val != nullptr) {
            // _infer_type_from_values([node["const"]])
            const char* kind = classify_value(const_val);
            kimix::string t = (kind != nullptr) ? kimix::string(kind) : kimix::string("string");
            yyjson_mut_obj_add_strncpy(doc, node, "type", t.data(), t.size());
            recurse_schema(doc, node);
            return;
        }
        inferred = infer_type_from_structure(node);
        yyjson_mut_obj_add_strncpy(doc, node, "type", inferred, std::char_traits<char>::length(inferred));
        recurse_schema(doc, node);
        return;
    }

    if (!has_skip_keys && has_type) {
        // Repair an explicit type that contradicts the enum/const values.
        yyjson_mut_val* values = nullptr;
        yyjson_mut_val* enum_val = yyjson_mut_obj_get(node, "enum");
        if (enum_val != nullptr && yyjson_mut_is_arr(enum_val) &&
            yyjson_mut_arr_size(enum_val) > 0) {
            values = enum_val;
        } else {
            yyjson_mut_val* const_val = yyjson_mut_obj_get(node, "const");
            if (const_val != nullptr) {
                values = const_val;
            }
        }
        if (values != nullptr) {
            kimix::string inferred = try_infer_single_type(values);
            if (!inferred.empty()) {
                kimix::string_view current(
                    yyjson_mut_get_str(type_val),
                    static_cast<size_t>(yyjson_mut_get_len(type_val)));
                if (current != inferred) {
                    yyjson_mut_obj_put(node,
                                       yyjson_mut_strncpy(doc, "type", 4),
                                       yyjson_mut_strncpy(doc, inferred.data(),
                                                         inferred.size()));
                    remove_irrelevant_structure_keys(doc, node, inferred);
                }
            }
        }
    }

    recurse_schema(doc, node);
}

} // namespace

void deref_json_schema(kimix::string_view schema,
                       kimix::span<const kimix::string_view> /*registry*/,
                       kimix::string& out) noexcept {
    out.clear();
    yyjson_doc* doc = yyjson_read(schema.data(), schema.size(), 0);
    if (doc == nullptr) {
        return;
    }
    yyjson_mut_doc* out_doc = yyjson_mut_doc_new(nullptr);
    if (out_doc == nullptr) {
        yyjson_doc_free(doc);
        return;
    }
    deref_ctx ctx;
    ctx.schema_doc = doc;
    ctx.out_doc = out_doc;
    yyjson_mut_val* resolved = traverse(ctx, yyjson_doc_get_root(doc));
    if (resolved == nullptr) {
        yyjson_mut_doc_free(out_doc);
        yyjson_doc_free(doc);
        return;
    }
    yyjson_mut_doc_set_root(out_doc, resolved);

    // Drop definition buckets with no remaining unresolved refs
    // (Python: resolved.pop("$defs"/"definitions", None) -- only when no
    // refs into them remain; cyclic refs keep their buckets).
    if (resolved != nullptr && yyjson_mut_is_obj(resolved)) {
        if (!has_unresolved_definition_ref(resolved, "$defs")) {
            yyjson_mut_obj_remove(resolved, yyjson_mut_strncpy(out_doc, "$defs", 5));
        }
        if (!has_unresolved_definition_ref(resolved, "definitions")) {
            yyjson_mut_obj_remove(resolved,
                                  yyjson_mut_strncpy(out_doc, "definitions", 11));
        }
    }

    json_write_compact(out_doc, out);
    yyjson_mut_doc_free(out_doc);
    yyjson_doc_free(doc);
}

void ensure_property_types(kimix::string_view schema, kimix::string& out) noexcept {
    out.clear();
    yyjson_doc* doc = yyjson_read(schema.data(), schema.size(), 0);
    if (doc == nullptr) {
        return;
    }
    yyjson_mut_doc* out_doc = yyjson_doc_mut_copy(doc, nullptr);
    yyjson_doc_free(doc);
    if (out_doc == nullptr) {
        return;
    }
    yyjson_mut_val* root = yyjson_mut_doc_get_root(out_doc);
    // The outer schema is a container: recurse into child positions without
    // normalizing the root itself.
    recurse_schema(out_doc, root);
    json_write_compact(out_doc, out);
    yyjson_mut_doc_free(out_doc);
}

} // namespace json
} // namespace runtime
} // namespace kimix
