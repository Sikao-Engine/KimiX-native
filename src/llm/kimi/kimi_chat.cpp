// kimi_chat.cpp - Kimi (Moonshot) chat provider, the C++ port of kosong's
// `kosong/chat_provider/kimi.py` (see kimi_chat.h for the contract summary).
//
// <httplib.h> comes first so winsock2.h is included before
// <core/kimix_core.h> pulls in <windows.h> (windows.h-before-winsock2.h
// breaks ws2tcpip.h on Windows; the kimix-llm unity build merges these TUs).

#include <httplib.h>

#include "llm/kimi/kimi_chat.h"

#include "llm/http_tls.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "yyjson.h"

#include "llm/yyjson_alc.h"

namespace kimix::llm::kimi {

namespace {

// ===========================================================================
// Generic read/mut value helpers
// ===========================================================================

kimix::string_view sv(yyjson_val *v) {
    return (v != nullptr && yyjson_is_str(v))
               ? kimix::string_view(yyjson_get_str(v), yyjson_get_len(v))
               : kimix::string_view{};
}

kimix::string str_of(yyjson_val *v) {
    const kimix::string_view s = sv(v);
    return kimix::string(s.data(), s.size());
}

kimix::string_view sv_mut(yyjson_mut_val *v) {
    return (v != nullptr && yyjson_mut_is_str(v))
               ? kimix::string_view(yyjson_mut_get_str(v), yyjson_mut_get_len(v))
               : kimix::string_view{};
}

// ===========================================================================
// Local $ref inlining (deref_json_schema) — recursive-copy over the READ tree
// ===========================================================================
// The Python reference deep-copies the schema and rebuilds it with local
// `$ref` pointers replaced by {resolved definition + sibling keys winning};
// because every emitted node is a fresh copy, no node is ever shared. The
// port keeps the immutable READ document as the traversal source and writes
// into a fresh MUTABLE document; cycles are broken by the `path` of raw $ref
// strings currently being followed (a repeated pointer keeps its `$ref`, and
// with it the definition bucket).

// Resolve a local JSON pointer ("#/definitions/Mode", "#") inside `root`.
// Returns null when the pointer does not resolve.
yyjson_val *resolve_pointer(yyjson_val *root, kimix::string_view pointer) {
    if (pointer == "#") {
        return root;
    }
    if (pointer.size() < 3 || pointer[0] != '#' || pointer[1] != '/') {
        return nullptr;
    }
    yyjson_val *current = root;
    kimix::string_view rest = pointer.substr(2);
    size_t pos = 0;
    for (;;) {
        size_t slash = rest.find('/', pos);
        kimix::string_view raw_part = slash == kimix::string_view::npos
                                          ? rest.substr(pos)
                                          : rest.substr(pos, slash - pos);
        // JSON-pointer unescaping: ~1 -> '/', ~0 -> '~' (in that order).
        kimix::string part;
        for (size_t i = 0; i < raw_part.size(); ++i) {
            if (raw_part[i] == '~' && i + 1 < raw_part.size() &&
                (raw_part[i + 1] == '0' || raw_part[i + 1] == '1')) {
                part.push_back(raw_part[i + 1] == '0' ? '~' : '/');
                ++i;
            } else {
                part.push_back(raw_part[i]);
            }
        }
        yyjson_val *next = nullptr;
        if (yyjson_is_obj(current)) {
            next = yyjson_obj_getn(current, part.data(), part.size());
        } else if (yyjson_is_arr(current)) {
            // _JSON_POINTER_INDEX_RE: 0 or [1-9][0-9]*
            bool valid = !part.empty();
            size_t index = 0;
            for (size_t i = 0; valid && i < part.size(); ++i) {
                const char c = part[i];
                if (c < '0' || c > '9') {
                    valid = false;
                } else if (i == 0 && c == '0' && part.size() > 1) {
                    valid = false; // leading zero
                } else {
                    if (index > (SIZE_MAX - 9) / 10) {
                        valid = false;
                        break;
                    }
                    index = index * 10 + (size_t)(c - '0');
                }
            }
            if (valid) {
                next = yyjson_arr_get(current, index);
            }
        }
        if (next == nullptr) {
            return nullptr;
        }
        current = next;
        if (slash == kimix::string_view::npos) {
            break;
        }
        pos = slash + 1;
    }
    return current;
}

// Emit `val` into `parent`: under `*key` when parent is an object and a key
// was given, otherwise appended when parent is an array.
void emit(yyjson_mut_doc *doc, yyjson_mut_val *parent, const kimix::string *key,
          yyjson_mut_val *val) {
    if (val == nullptr) {
        return;
    }
    if (yyjson_mut_is_arr(parent)) {
        yyjson_mut_arr_append(parent, val);
    } else if (yyjson_mut_is_obj(parent) && key != nullptr) {
        // The document copies NOTHING out of `key`: both the key value and
        // its buffer must live in the document (the caller's kimix::string
        // is a loop-local temporary), hence strncpy + the 3-arg obj_add.
        yyjson_mut_val *k = yyjson_mut_strncpy(doc, key->data(), key->size());
        yyjson_mut_obj_add(parent, k, val);
    }
}

void traverse_read(yyjson_mut_doc *doc, yyjson_val *root, yyjson_val *node,
                   yyjson_mut_val *parent, const kimix::string *key,
                   kimix::vector<kimix::string> &path);

// The object case: a local $ref is inlined as {resolved definition's keys
// (recursively deref'd) + the node's own sibling keys (the siblings win; the
// $ref key itself is dropped)}. Remote / unresolvable / cyclic pointers
// mirror the reference and emit the node unchanged (deep-copied).
void traverse_object_read(yyjson_mut_doc *doc, yyjson_val *root, yyjson_val *node,
                          yyjson_mut_val *parent, const kimix::string *key,
                          kimix::vector<kimix::string> &path) {
    yyjson_val *ref = yyjson_obj_get(node, "$ref");
    if (yyjson_is_str(ref)) {
        const kimix::string_view ref_sv(yyjson_get_str(ref), yyjson_get_len(ref));
        if (ref_sv == "#" ||
            (ref_sv.size() >= 2 && ref_sv[0] == '#' && ref_sv[1] == '/')) {
            bool cyclic = false;
            for (const kimix::string &p : path) {
                if (p == ref_sv) {
                    cyclic = true;
                    break;
                }
            }
            if (!cyclic) {
                if (yyjson_val *target = resolve_pointer(root, ref_sv)) {
                    path.push_back(kimix::string(ref_sv));
                    if (yyjson_is_obj(target)) {
                        yyjson_mut_val *merged = yyjson_mut_obj(doc);
                        size_t idx = 0;
                        size_t max = 0;
                        yyjson_val *k = nullptr;
                        yyjson_val *v = nullptr;
                        yyjson_obj_foreach(target, idx, max, k, v) {
                            kimix::string tkey = str_of(k);
                            if (tkey == "$ref" ||
                                yyjson_obj_getn(node, tkey.data(), tkey.size()) !=
                                    nullptr) {
                                continue; // overridden by a local sibling
                            }
                            traverse_read(doc, root, v, merged, &tkey, path);
                        }
                        idx = 0;
                        max = 0;
                        yyjson_obj_foreach(node, idx, max, k, v) {
                            kimix::string tkey = str_of(k);
                            if (tkey == "$ref") {
                                continue;
                            }
                            traverse_read(doc, root, v, merged, &tkey, path);
                        }
                        path.pop_back();
                        emit(doc, parent, key, merged);
                        return;
                    }
                    // A resolved definition that is not an object: the
                    // reference returns the resolved value itself.
                    yyjson_mut_val *holder = yyjson_mut_arr(doc);
                    traverse_read(doc, root, target, holder, nullptr, path);
                    path.pop_back();
                    if (yyjson_mut_val *resolved = yyjson_mut_arr_get(holder, 0)) {
                        emit(doc, parent, key, resolved);
                        return;
                    }
                }
            }
        }
        // Remote / unresolvable / cyclic: emit the node as-is.
        emit(doc, parent, key, yyjson_val_mut_copy(doc, node));
        return;
    }
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    size_t idx = 0;
    size_t max = 0;
    yyjson_val *k = nullptr;
    yyjson_val *v = nullptr;
    yyjson_obj_foreach(node, idx, max, k, v) {
        kimix::string tkey = str_of(k);
        traverse_read(doc, root, v, obj, &tkey, path);
    }
    emit(doc, parent, key, obj);
}

void traverse_read(yyjson_mut_doc *doc, yyjson_val *root, yyjson_val *node,
                   yyjson_mut_val *parent, const kimix::string *key,
                   kimix::vector<kimix::string> &path) {
    if (node == nullptr) {
        return;
    }
    if (yyjson_is_obj(node)) {
        traverse_object_read(doc, root, node, parent, key, path);
        return;
    }
    if (yyjson_is_arr(node)) {
        yyjson_mut_val *arr = yyjson_mut_arr(doc);
        size_t idx = 0;
        size_t max = 0;
        yyjson_val *item = nullptr;
        yyjson_arr_foreach(node, idx, max, item) {
            traverse_read(doc, root, item, arr, nullptr, path);
        }
        emit(doc, parent, key, arr);
        return;
    }
    emit(doc, parent, key, yyjson_val_mut_copy(doc, node));
}

// Whether any `$ref: "#/<bucket>/..."` pointer remains OUTSIDE the bucket's
// own subtree (_has_unresolved_definition_ref).
bool has_unresolved_definition_ref(yyjson_mut_val *node, kimix::string_view bucket) {
    if (node == nullptr) {
        return false;
    }
    if (yyjson_mut_is_arr(node)) {
        size_t idx = 0;
        size_t max = 0;
        yyjson_mut_val *item = nullptr;
        yyjson_mut_arr_foreach(node, idx, max, item) {
            if (has_unresolved_definition_ref(item, bucket)) {
                return true;
            }
        }
        return false;
    }
    if (!yyjson_mut_is_obj(node)) {
        return false;
    }
    const kimix::string_view ref = sv_mut(yyjson_mut_obj_get(node, "$ref"));
    // starts with "#/<bucket>/"
    if (ref.size() > bucket.size() + 3 && ref[0] == '#' && ref[1] == '/' &&
        ref.substr(2, bucket.size()) == bucket && ref[2 + bucket.size()] == '/') {
        return true;
    }
    size_t idx = 0;
    size_t max = 0;
    yyjson_mut_val *k = nullptr;
    yyjson_mut_val *v = nullptr;
    yyjson_mut_obj_foreach(node, idx, max, k, v) {
        if (sv_mut(k) == bucket) {
            continue; // the bucket's own subtree never counts
        }
        if (has_unresolved_definition_ref(v, bucket)) {
            return true;
        }
    }
    return false;
}

// ===========================================================================
// ensure_property_types — in-place on the mutable (deref'd) result tree
// ===========================================================================
// After deref the tree shares no nodes, so in-place normalization is safe.

// _TYPE_COMPLETION_SKIP_KEYS.
bool is_skip_key(kimix::string_view key) {
    static const char *const keys[] = {"$ref", "allOf", "anyOf", "else", "if",
                                       "not",  "oneOf", "then"};
    for (const char *k : keys) {
        if (key == k) {
            return true;
        }
    }
    return false;
}

// _CHILD_SCHEMA_SLOTS (kind: 's' single, 'm' map, 'a' array, 'o'
// schema-or-array; parent_type == the type whose structure keys the slot
// implies).
struct ChildSlot {
    const char *key;
    char kind;
    const char *parent_type;
};
constexpr ChildSlot kChildSlots[] = {
    {"$defs", 'm', ""},
    {"definitions", 'm', ""},
    {"dependencies", 'm', "object"},
    {"dependentSchemas", 'm', "object"},
    {"patternProperties", 'm', "object"},
    {"properties", 'm', "object"},
    {"additionalItems", 's', "array"},
    {"additionalProperties", 's', "object"},
    {"contains", 's', "array"},
    {"contentSchema", 's', "string"},
    {"else", 's', ""},
    {"if", 's', ""},
    {"not", 's', ""},
    {"propertyNames", 's', "object"},
    {"then", 's', ""},
    {"unevaluatedItems", 's', "array"},
    {"unevaluatedProperties", 's', "object"},
    {"allOf", 'a', ""},
    {"anyOf", 'a', ""},
    {"oneOf", 'a', ""},
    {"prefixItems", 'a', "array"},
    {"items", 'o', "array"},
};

// Structure-key checks: the parent type's child slots + the extra keywords
// the reference unions in (_OBJECT_STRUCTURE_KEYS, _ARRAY_STRUCTURE_KEYS,
// _STRING_STRUCTURE_KEYS, _NUMERIC_STRUCTURE_KEYS).
bool has_structure_key_of_type(yyjson_mut_val *node, kimix::string_view parent_type,
                               const char *const *extra, size_t extra_count) {
    size_t idx = 0;
    size_t max = 0;
    yyjson_mut_val *k = nullptr;
    yyjson_mut_val *v = nullptr;
    yyjson_mut_obj_foreach(node, idx, max, k, v) {
        const kimix::string_view key = sv_mut(k);
        for (const ChildSlot &slot : kChildSlots) {
            if (parent_type == slot.parent_type && key == slot.key) {
                return true;
            }
        }
        for (size_t i = 0; i < extra_count; ++i) {
            if (key == extra[i]) {
                return true;
            }
        }
    }
    return false;
}

bool has_object_structure_key(yyjson_mut_val *node) {
    static const char *const extra[] = {"dependentRequired", "maxProperties",
                                        "minProperties", "required"};
    return has_structure_key_of_type(node, "object", extra, 4);
}
bool has_array_structure_key(yyjson_mut_val *node) {
    static const char *const extra[] = {"maxContains", "maxItems", "minContains",
                                        "minItems", "uniqueItems"};
    return has_structure_key_of_type(node, "array", extra, 5);
}
bool has_string_structure_key(yyjson_mut_val *node) {
    static const char *const extra[] = {"contentEncoding", "contentMediaType",
                                        "format", "maxLength", "minLength", "pattern"};
    return has_structure_key_of_type(node, "string", extra, 6);
}
bool has_numeric_structure_key(yyjson_mut_val *node) {
    static const char *const keys[] = {"exclusiveMaximum", "exclusiveMinimum",
                                       "maximum", "minimum", "multipleOf"};
    for (const char *key : keys) {
        if (yyjson_mut_obj_get(node, key) != nullptr) {
            return true;
        }
    }
    return false;
}

void normalize_property(yyjson_mut_doc *doc, yyjson_mut_val *node);
void recurse_schema(yyjson_mut_doc *doc, yyjson_mut_val *node);

// _classify_value: JSON value -> schema type string ("boolean" checked
// before the numeric kinds, exactly like the Python bool-is-not-int rule).
kimix::string_view classify_value_mut(yyjson_mut_val *v) {
    if (v == nullptr) {
        return {};
    }
    if (yyjson_mut_is_true(v) || yyjson_mut_is_false(v)) {
        return "boolean";
    }
    if (yyjson_mut_is_int(v)) {
        return "integer";
    }
    if (yyjson_mut_is_real(v)) {
        return "number";
    }
    if (yyjson_mut_is_str(v)) {
        return "string";
    }
    if (yyjson_mut_is_null(v)) {
        return "null";
    }
    if (yyjson_mut_is_obj(v)) {
        return "object";
    }
    if (yyjson_mut_is_arr(v)) {
        return "array";
    }
    return {};
}

// _infer_type_from_values: single type wins; {integer, number} (and ONLY
// those two) -> "number"; anything else mixed -> "string".
kimix::string infer_type_from_values_mut(yyjson_mut_val *values) {
    kimix::string_view single;
    bool multiple = false;
    bool saw_integer = false;
    bool saw_number = false;
    size_t count = 0;
    size_t idx = 0;
    size_t max = 0;
    yyjson_mut_val *v = nullptr;
    yyjson_mut_arr_foreach(values, idx, max, v) {
        ++count;
        const kimix::string_view kind = classify_value_mut(v);
        if (kind.empty()) {
            return "string"; // defensive: unclassifiable -> safe string type
        }
        if (kind == "integer") {
            saw_integer = true;
        } else if (kind == "number") {
            saw_number = true;
        }
        if (single.empty()) {
            single = kind;
        } else if (single != kind) {
            multiple = true;
        }
    }
    if (!multiple && !single.empty()) {
        return kimix::string(single);
    }
    if (count == 2 && saw_integer && saw_number) {
        return "number";
    }
    return "string";
}

// _try_infer_single_type (strict): false when mixed / unclassifiable instead
// of guessing "string". "integer" collapses into "number".
bool try_infer_single_type_mut(yyjson_mut_val *values, kimix::string &out) {
    kimix::string_view single;
    bool ambiguous = false;
    size_t idx = 0;
    size_t max = 0;
    yyjson_mut_val *v = nullptr;
    yyjson_mut_arr_foreach(values, idx, max, v) {
        const kimix::string_view kind = classify_value_mut(v);
        if (kind.empty()) {
            return false;
        }
        if (kind == "integer") {
            continue; // integer is a subset of number
        }
        if (single.empty()) {
            single = kind;
        } else if (single != kind) {
            ambiguous = true;
        }
    }
    if (ambiguous || single.empty()) {
        return false;
    }
    out.assign(single.data(), single.size());
    return true;
}

// _infer_type_from_structure.
kimix::string infer_type_from_structure(yyjson_mut_val *node) {
    if (has_object_structure_key(node)) {
        return "object";
    }
    if (has_array_structure_key(node)) {
        return "array";
    }
    if (has_string_structure_key(node)) {
        return "string";
    }
    if (has_numeric_structure_key(node)) {
        return "number";
    }
    return "string";
}

// Drop every member named `key` from an object.
void remove_key_mut(yyjson_mut_val *obj, const char *key) {
    // obj_remove_strn already removes ALL pairs whose key matches (the
    // return value is only the first removed value or NULL); calling it in a
    // loop would dereference the stale return pointer, so one call is right.
    (void)yyjson_mut_obj_remove_strn(obj, key, std::strlen(key));
}

// _remove_irrelevant_structure_keys: after a type repair, drop the object /
// array structure keywords that no longer apply.
void remove_irrelevant_structure_keys(yyjson_mut_val *node,
                                      kimix::string_view new_type) {
    for (const ChildSlot &slot : kChildSlots) {
        const kimix::string_view parent(slot.parent_type);
        if (parent.empty()) {
            continue;
        }
        if ((parent == "object" && new_type != "object") ||
            (parent == "array" && new_type != "array")) {
            remove_key_mut(node, slot.key);
        }
    }
    static const char *const object_extra[] = {"dependentRequired", "maxProperties",
                                               "minProperties", "required"};
    static const char *const array_extra[] = {"maxContains", "maxItems", "minContains",
                                              "minItems", "uniqueItems"};
    if (new_type != "object") {
        for (const char *key : object_extra) {
            remove_key_mut(node, key);
        }
    }
    if (new_type != "array") {
        for (const char *key : array_extra) {
            remove_key_mut(node, key);
        }
    }
}

// _normalize_property: ensure a property schema declares a `type`, repair a
// type contradicting enum/const values, then recurse. The recursion follows
// the reference's slot iteration order EXACTLY ('m' before 'a' before 'o'):
// a slot the parent normalizes (e.g. "prefixItems") must be visited before a
// later one that walks the same subtree (e.g. "items"), so no node is ever
// normalized twice - re-entering a child whose enum the parent repair has
// just deleted would leave a dangling reference (a use-after-free).
void normalize_property(yyjson_mut_doc *doc, yyjson_mut_val *node) {
    if (node == nullptr || !yyjson_mut_is_obj(node)) {
        return;
    }
    bool has_skip = false;
    {
        size_t idx = 0;
        size_t max = 0;
        yyjson_mut_val *k = nullptr;
        yyjson_mut_val *v = nullptr;
        yyjson_mut_obj_foreach(node, idx, max, k, v) {
            if (is_skip_key(sv_mut(k))) {
                has_skip = true;
                break;
            }
        }
    }
    yyjson_mut_val *type_val = yyjson_mut_obj_get(node, "type");
    if (type_val == nullptr && !has_skip) {
        yyjson_mut_val *enum_values = yyjson_mut_obj_get(node, "enum");
        yyjson_mut_val *const_val = yyjson_mut_obj_get(node, "const");
        kimix::string inferred;
        if (yyjson_mut_is_arr(enum_values) && yyjson_mut_arr_size(enum_values) > 0) {
            inferred = infer_type_from_values_mut(enum_values);
        } else if (const_val != nullptr) {
            const kimix::string_view kind = classify_value_mut(const_val);
            inferred = kind.empty() ? kimix::string("string") : kimix::string(kind);
        } else {
            inferred = infer_type_from_structure(node);
        }
        yyjson_mut_obj_add_strcpy(doc, node, "type", inferred.c_str());
    } else if (!has_skip && yyjson_mut_is_str(type_val)) {
        // Repair an explicit type contradicting the enum/const values (a
        // known Xcode-MCP generator bug); ambiguous values stay untouched.
        yyjson_mut_val *enum_values = yyjson_mut_obj_get(node, "enum");
        yyjson_mut_val *const_val = yyjson_mut_obj_get(node, "const");
        kimix::string inferred;
        bool ok = false;
        if (yyjson_mut_is_arr(enum_values) && yyjson_mut_arr_size(enum_values) > 0) {
            ok = try_infer_single_type_mut(enum_values, inferred);
        } else if (const_val != nullptr) {
            const kimix::string_view kind = classify_value_mut(const_val);
            if (!kind.empty()) {
                inferred.assign(kind.data(), kind.size());
                ok = true;
            }
        }
        if (ok && sv_mut(type_val) != kimix::string_view(inferred)) {
            // Replace the value IN PLACE. The replacement node comes from the
            // document's free list and carries its own `next` (a self-link);
            // copying it wholesale would splice that self-link into `node`'s
            // member list and corrupt the traversal, so the ORIGINAL next
            // pointer is saved across the copy.
            const kimix::string new_str = inferred; // ensure c_str buffer
            yyjson_mut_val *fresh = yyjson_mut_strcpy(doc, new_str.c_str());
            yyjson_mut_val *saved_next = type_val->next;
            *type_val = *fresh;
            type_val->next = saved_next;
            remove_irrelevant_structure_keys(node, inferred);
        }
    }
    // Children: the reference's single pass, in slot-table order.
    recurse_schema(doc, node);
}

// _recurse_schema: walk every child-schema slot (node itself is a container
// and was normalized by the CALLER, not here). Children are iterated ONCE
// each - a shared slot value (e.g. one array under both "prefixItems" and
// "items") is normalized at most once, exactly like the reference's
// single-pass recursion. The slots' order matches the reference table
// ('m' map slots before 'a'/'o' array slots), so the first slot reaching a
// child is the slot the reference normalizes it under.
void recurse_schema(yyjson_mut_doc *doc, yyjson_mut_val *node) {
    if (node == nullptr || !yyjson_mut_is_obj(node)) {
        return;
    }
    // The child slots of this node, in table order.
    struct Ref {
        yyjson_mut_val *val;
        char kind;
    };
    kimix::vector<Ref> children;
    for (const ChildSlot &slot : kChildSlots) {
        yyjson_mut_val *value = yyjson_mut_obj_get(node, slot.key);
        if (value == nullptr) {
            continue;
        }
        switch (slot.kind) {
        case 's':
            if (yyjson_mut_is_obj(value)) {
                children.push_back({value, 'n'});
            }
            break;
        case 'm':
            if (yyjson_mut_is_obj(value)) {
                size_t idx = 0;
                size_t max = 0;
                yyjson_mut_val *vk = nullptr;
                yyjson_mut_val *vv = nullptr;
                yyjson_mut_obj_foreach(value, idx, max, vk, vv) {
                    children.push_back({vv, 'n'});
                }
            }
            break;
        case 'a':
            if (yyjson_mut_is_arr(value)) {
                size_t idx = 0;
                size_t max = 0;
                yyjson_mut_val *item = nullptr;
                yyjson_mut_arr_foreach(value, idx, max, item) {
                    children.push_back({item, 'n'});
                }
            }
            break;
        default: // 'o' schema-or-array
            if (yyjson_mut_is_obj(value)) {
                children.push_back({value, 'n'});
            } else if (yyjson_mut_is_arr(value)) {
                size_t idx = 0;
                size_t max = 0;
                yyjson_mut_val *item = nullptr;
                yyjson_mut_arr_foreach(value, idx, max, item) {
                    children.push_back({item, 'n'});
                }
            }
            break;
        }
    }
    // Deduplicate by pointer identity: a slot whose array is also reachable
    // through an earlier slot (e.g. "items" nested inside an "anyOf"
    // element's schema) would otherwise normalize the same node twice, and
    // a parent repair deleting the child's enum between passes is a
    // use-after-free. The reference walks each child once; keep that.
    for (size_t i = 0; i < children.size(); ++i) {
        bool dup = false;
        for (size_t j = 0; j < i; ++j) {
            if (children[j].val == children[i].val) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        normalize_property(doc, children[i].val);
    }
}

// ===========================================================================
// Wire serialization
// ===========================================================================

// Serialize one Kimi wire message (kosong _convert_message's dumped shape).
void add_wire_message(yyjson_mut_doc *doc, yyjson_mut_val *arr, const ChatMessage &m) {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_arr_append(arr, obj);
    add_json_str(doc, obj, "role", m.role);
    if (m.omit_content) {
        // content absent entirely (assistant tool-call, empty visible text)
    } else if (!m.parts.empty()) {
        yyjson_mut_val *content_arr = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, obj, "content", content_arr);
        for (const auto &part : m.parts) {
            using K = ContentPart::Kind;
            if (part.kind == K::text) {
                yyjson_mut_val *block = yyjson_mut_obj(doc);
                yyjson_mut_arr_append(content_arr, block);
                yyjson_mut_obj_add_str(doc, block, "type", "text");
                add_json_str(doc, block, "text", part.text);
            } else if (part.kind == K::image_url || part.kind == K::audio_url ||
                       part.kind == K::video_url) {
                const char *type =
                    part.kind == K::image_url
                        ? "image_url"
                        : (part.kind == K::audio_url ? "audio_url" : "video_url");
                yyjson_mut_val *block = yyjson_mut_obj(doc);
                yyjson_mut_arr_append(content_arr, block);
                add_json_str(doc, block, "type", type);
                yyjson_mut_val *payload = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_val(doc, block, type, payload);
                add_json_str(doc, payload, "url", part.url);
                if (!part.detail.empty()) {
                    add_json_str(doc, payload, "detail", part.detail);
                }
            }
        }
    } else if (m.null_content) {
        yyjson_mut_obj_add_null(doc, obj, "content");
    } else {
        add_json_str(doc, obj, "content", m.content);
    }
    if (!m.tool_call_id.empty()) {
        add_json_str(doc, obj, "tool_call_id", m.tool_call_id);
    }
    if (!m.tool_calls.empty()) {
        yyjson_mut_val *tc_arr = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, obj, "tool_calls", tc_arr);
        for (const auto &tc : m.tool_calls) {
            yyjson_mut_val *tc_obj = yyjson_mut_obj(doc);
            yyjson_mut_arr_append(tc_arr, tc_obj);
            add_json_str(doc, tc_obj, "id", tc.id);
            add_json_str(doc, tc_obj, "type", tc.type);
            yyjson_mut_val *fn = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_val(doc, tc_obj, "function", fn);
            add_json_str(doc, fn, "name", tc.name);
            add_json_str(doc, fn, "arguments", tc.arguments);
        }
    }
    if (m.has_reasoning) {
        add_json_str(doc, obj, "reasoning_content", m.reasoning_content);
    }
}

// One tool definition (the two shapes of _convert_tool) into the parent
// object `holder` under `key`. `to_array` selects the container semantics:
// true -> `holder` is an array and the tool object is appended; false ->
// `holder` is an object and the tool object is stored under `key`. Takes the
// three wire fields directly so both openai::Tool and kimix::llm::Tool feed
// it.
void add_wire_tool(yyjson_mut_doc *doc, yyjson_mut_val *holder, const char *key,
                   bool to_array, const kimix::string &name,
                   const kimix::string &description,
                   const kimix::string &parameters_json) {
    yyjson_mut_val *tool_obj = yyjson_mut_obj(doc);
    if (to_array) {
        yyjson_mut_arr_append(holder, tool_obj);
    } else {
        yyjson_mut_obj_add_val(doc, holder, key, tool_obj);
    }
    if (!name.empty() && name[0] == '$') {
        // Kimi builtin functions ($web_search, $browse): the API only
        // accepts type + name.
        yyjson_mut_obj_add_str(doc, tool_obj, "type", "builtin_function");
        yyjson_mut_val *fn = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, tool_obj, "function", fn);
        add_json_str(doc, fn, "name", name);
        return;
    }
    yyjson_mut_obj_add_str(doc, tool_obj, "type", "function");
    yyjson_mut_val *fn = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, tool_obj, "function", fn);
    add_json_str(doc, fn, "name", name);
    add_json_str(doc, fn, "description", description);
    kimix::string params = normalize_tool_parameters(parameters_json);
    if (params.empty()) {
        params = parameters_json;
    }
    if (!add_json_fragment(doc, fn, "parameters", params)) {
        yyjson_mut_obj_add_null(doc, fn, "parameters");
    }
}

} // namespace

// ===========================================================================
// Message conversion
// ===========================================================================

bool visible_content_effectively_empty(const kimix::vector<ContentPart> &parts,
                                       kimix::string_view backbone, bool has_parts) {
    const auto has_nonws = [](kimix::string_view s) {
        for (const char c : s) {
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' &&
                c != '\v') {
                return true;
            }
        }
        return false;
    };
    if (!has_parts) {
        return !has_nonws(backbone);
    }
    for (const ContentPart &part : parts) {
        if (part.kind != ContentPart::Kind::text) {
            return false; // a media part is never "effectively empty"
        }
        if (has_nonws(part.text)) {
            return false;
        }
    }
    return true;
}

ChatMessage convert_message(const openai::ChatMessage &m, kimix::string_view thinking,
                            bool preserved_thinking_enabled) {
    ChatMessage out;
    out.role = m.role;
    out.content = m.content;
    out.tool_call_id = m.tool_call_id;
    out.tool_calls = m.tool_calls;

    // extract_reasoning_from_content: the think parts leave the visible
    // content and become the reasoning round-trip. The unified message model
    // carries them in `thinking` (message_set_parts folds ThinkPart text
    // there); an embedded think part is honored too. An explicitly empty
    // think part still marks the message as having reasoning (the empty
    // string must round-trip - Moonshot rejects thinking-mode histories
    // whose assistant messages lost the field).
    bool has_reasoning = false;
    if (!thinking.empty()) {
        out.reasoning_content.assign(thinking.data(), thinking.size());
        has_reasoning = true;
    }
    kimix::vector<ContentPart> visible;
    size_t think_count = 0;
    for (const ContentPart &part : m.parts) {
        if (part.kind == ContentPart::Kind::think) {
            out.reasoning_content += part.text;
            has_reasoning = true;
            ++think_count;
        } else {
            visible.push_back(part);
        }
    }
    out.parts = think_count > 0 ? std::move(visible) : m.parts;
    out.has_reasoning = has_reasoning;

    const bool has_parts = !m.parts.empty();
    if (m.role == "assistant" && !m.tool_calls.empty() &&
        visible_content_effectively_empty(out.parts, m.content, has_parts)) {
        // The Kimi-for-Coding compat layer 400s on an empty text part inside
        // a tool-call message's content list ("text content is empty");
        // omitting the key is always accepted (kosong pops "content").
        out.omit_content = true;
    } else if (!has_parts && m.content.empty() && m.role != "system") {
        // A content-less message keeps the OpenAI Chat provider's
        // "content: null" behavior (the dumped Message has content=None
        // only when there is nothing to send; exclude_content only fires
        // for assistant tool-call messages, handled above).
        out.null_content = true;
    }

    // preserved_thinking_enabled (thinking.keep == "all" and thinking not
    // disabled): EVERY assistant message must carry reasoning_content; an
    // empty string is sent for messages without reasoning.
    if (preserved_thinking_enabled && m.role == "assistant" && !out.has_reasoning) {
        out.has_reasoning = true;
        out.reasoning_content.clear();
    }
    return out;
}

// ===========================================================================
// Tool parameter schema normalization
// ===========================================================================

kimix::string normalize_tool_parameters(kimix::string_view parameters_json) {
    if (parameters_json.empty()) {
        return {};
    }
    // Parse strict into the READ tree (the traversal source; immutable, so
    // shared reads can never alias an edit).
    kimix::string scratch;
    const char *data = parameters_json.data();
    size_t len = parameters_json.size();
    if (!utf8_valid(parameters_json)) {
        scratch = utf8_sanitize(parameters_json);
        data = scratch.data();
        len = scratch.size();
    }
    yyjson_doc *parsed =
        yyjson_read_opts(const_cast<char *>(data), len, 0, &kYYJsonAlcMi, nullptr);
    if (parsed == nullptr) {
        return {};
    }
    yyjson_val *src = yyjson_doc_get_root(parsed);
    if (!yyjson_is_obj(src)) {
        yyjson_doc_free(parsed);
        return {};
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kYYJsonAlcMi);
    if (doc == nullptr) {
        yyjson_doc_free(parsed);
        return {};
    }
    // deref_json_schema: rebuild the tree with local $ref pointers inlined.
    // The top-level schema is itself a container (never $ref-replaced and
    // never type-completed), so its members are traversed directly into the
    // fresh root.
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    kimix::vector<kimix::string> path;
    {
        size_t idx = 0;
        size_t max = 0;
        yyjson_val *k = nullptr;
        yyjson_val *v = nullptr;
        yyjson_obj_foreach(src, idx, max, k, v) {
            kimix::string tkey = str_of(k);
            traverse_read(doc, src, v, root, &tkey, path);
        }
    }
    yyjson_doc_free(parsed);

    // Drop the definition buckets when no unresolved pointer into them
    // remains (cyclic refs keep theirs so they stay resolvable). The bucket
    // decision runs on the UNTOUCHED deref output and a kept bucket is
    // detached, deep-copied and re-attached after the type-completion pass:
    // deref'd copies share nodes with their definition bucket (an inlined
    // object is the same tree), and normalize_property repairs nodes IN
    // PLACE, so a repair reached through an inline would otherwise leak into
    // the bucket (Python has no such sharing - its result is a pure deep
    // copy - and its type repair additionally drops structure keywords the
    // inlined copy must not lose either).
    struct BucketPlan {
        const char *name;
        yyjson_mut_val *snapshot; // detached clone when the bucket stays
    };
    BucketPlan plans[] = {{"$defs", nullptr}, {"definitions", nullptr}};
    for (auto &plan : plans) {
        if (yyjson_mut_obj_get(root, plan.name) == nullptr) {
            continue;
        }
        bool unresolved = false;
        size_t idx = 0;
        size_t max = 0;
        yyjson_mut_val *k = nullptr;
        yyjson_mut_val *v = nullptr;
        yyjson_mut_obj_foreach(root, idx, max, k, v) {
            if (sv_mut(k) == plan.name) {
                continue;
            }
            if (has_unresolved_definition_ref(v, plan.name)) {
                unresolved = true;
                break;
            }
        }
        if (unresolved) {
            plan.snapshot =
                yyjson_mut_val_mut_copy(doc, yyjson_mut_obj_get(root, plan.name));
        }
        remove_key_mut(root, plan.name);
    }

    // ensure_property_types: walk the child-schema slots.
    recurse_schema(doc, root);

    for (auto &plan : plans) {
        if (plan.snapshot != nullptr) {
            yyjson_mut_obj_add_val(doc, root, plan.name, plan.snapshot);
        }
    }

    kimix::string error;
    return write_json_doc(doc, error);
}

kimix::string convert_tool_json(const openai::Tool &tool) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kYYJsonAlcMi);
    if (doc == nullptr) {
        return {};
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    add_wire_tool(doc, root, "tool", false, tool.name, tool.description,
                  tool.parameters_json);
    // The single-object shape: add_wire_tool stores the object under `key`
    // when not appending; re-root it.
    yyjson_mut_val *wrapped = yyjson_mut_obj_get(root, "tool");
    if (wrapped != nullptr) {
        yyjson_mut_doc_set_root(doc, wrapped);
    }
    kimix::string error;
    return write_json_doc(doc, error);
}

// ===========================================================================
// Request body
// ===========================================================================

kimix::string build_chat_body(const Config &cfg, const kimix::vector<ChatMessage> &messages,
                              const kimix::vector<openai::Tool> &tools,
                              kimix::string *out_error) {
    kimix::string *err = out_error;
    kimix::string scratch;
    if (!err) {
        err = &scratch;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kYYJsonAlcMi);
    if (!doc) {
        *err = "no memory for the JSON document";
        return {};
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    add_json_str(doc, root, "model", cfg.model);
    yyjson_mut_obj_add_bool(doc, root, "stream", true);

    yyjson_mut_val *stream_options = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, root, "stream_options", stream_options);
    yyjson_mut_obj_add_bool(doc, stream_options, "include_usage", true);

    // kimi.generate normalizes max_tokens -> max_completion_tokens (the
    // modern reasoning-model field; it wins when both are set) and clamps it
    // to kMaxOutputTokens. When neither is configured no cap goes on the
    // wire at all.
    if (cfg.max_tokens > 0) {
        int64_t budget = cfg.max_tokens;
        if (budget > kMaxOutputTokens) {
            budget = kMaxOutputTokens;
        }
        yyjson_mut_obj_add_int(doc, root, "max_completion_tokens", budget);
    }

    // Sampling controls - sent only when configured (0 == unset), exactly
    // like the reference's `if v is not None` guards.
    if (cfg.temperature != 0.0) {
        yyjson_mut_obj_add_real(doc, root, "temperature", cfg.temperature);
    }
    if (cfg.top_p != 0.0) {
        yyjson_mut_obj_add_real(doc, root, "top_p", cfg.top_p);
    }

    // The session id mapped to Moonshot's server-side prompt cache key.
    if (!cfg.prompt_cache_key.empty()) {
        add_json_str(doc, root, "prompt_cache_key", cfg.prompt_cache_key);
    }

    // No top-level `reasoning_effort` is ever sent on this contract.

    yyjson_mut_val *msg_arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "messages", msg_arr);
    for (const auto &m : messages) {
        add_wire_message(doc, msg_arr, m);
    }

    if (!tools.empty()) {
        yyjson_mut_val *tools_arr = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, root, "tools", tools_arr);
        for (const auto &t : tools) {
            add_wire_tool(doc, tools_arr, nullptr, true, t.name, t.description,
                          t.parameters_json);
        }
    }

    // The thinking object (with_thinking + with_extra_body semantics):
    // enabled -> {"type":"enabled","effort":<rank>} (the effort string
    // passes through verbatim); disabled -> {"type":"disabled"} with NO
    // stale effort. A configured keep survives effort changes and the off
    // switch.
    yyjson_mut_val *thinking = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, root, "thinking", thinking);
    if (thinking_enabled(cfg)) {
        yyjson_mut_obj_add_str(doc, thinking, "type", "enabled");
        add_json_str(doc, thinking, "effort", cfg.thinking_effort);
    } else {
        yyjson_mut_obj_add_str(doc, thinking, "type", "disabled");
    }
    if (!cfg.thinking_keep.empty()) {
        add_json_str(doc, thinking, "keep", cfg.thinking_keep);
    }

    return write_json_doc(doc, *err);
}

// ===========================================================================
// Stream + transport
// ===========================================================================
namespace {

// Read the non-streaming chat.completion shape (kosong
// _convert_non_stream_response): the whole assistant message arrives in one
// JSON body instead of SSE events. Returns true when the body parsed into a
// usable message.
bool parse_non_stream_body(const kimix::string &body, openai::ChatResult &result,
                           kimix::string &finish_reason) {
    yyjson_doc *doc = yyjson_read_opts(const_cast<char *>(body.data()), body.size(), 0,
                                       &kYYJsonAlcMi, nullptr);
    if (doc == nullptr) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    bool ok = false;
    if (yyjson_is_obj(root)) {
        yyjson_val *choices = yyjson_obj_get(root, "choices");
        auto take_str = [](yyjson_val *obj, const char *key, kimix::string &out) {
            yyjson_val *v = yyjson_obj_get(obj, key);
            if (yyjson_is_str(v)) {
                out.assign(yyjson_get_str(v), yyjson_get_len(v));
            }
        };
        if (yyjson_is_arr(choices) && yyjson_arr_size(choices) > 0) {
            yyjson_val *choice = yyjson_arr_get_first(choices);
            yyjson_val *msg =
                yyjson_is_obj(choice) ? yyjson_obj_get(choice, "message") : nullptr;
            if (yyjson_is_obj(msg)) {
                ok = true;
                // The reference yields an empty ThinkPart for an explicitly
                // empty reasoning_content (round-trip requirement), so the
                // field is copied whenever it is a string at all; the
                // `reasoning` fallback applies only when it is absent.
                yyjson_val *rc = yyjson_obj_get(msg, "reasoning_content");
                if (yyjson_is_str(rc)) {
                    result.reasoning.assign(yyjson_get_str(rc), yyjson_get_len(rc));
                } else {
                    take_str(msg, "reasoning", result.reasoning);
                }
                take_str(msg, "content", result.content);
                yyjson_val *tcs = yyjson_obj_get(msg, "tool_calls");
                if (yyjson_is_arr(tcs)) {
                    size_t idx = 0;
                    size_t max = 0;
                    yyjson_val *tc = nullptr;
                    yyjson_arr_foreach(tcs, idx, max, tc) {
                        if (!yyjson_is_obj(tc)) {
                            continue;
                        }
                        openai::ToolCall call;
                        take_str(tc, "id", call.id);
                        take_str(tc, "type", call.type);
                        yyjson_val *fn = yyjson_obj_get(tc, "function");
                        if (yyjson_is_obj(fn)) {
                            take_str(fn, "name", call.name);
                            take_str(fn, "arguments", call.arguments);
                        }
                        result.tool_calls.push_back(std::move(call));
                    }
                }
            }
            if (yyjson_is_obj(choice)) {
                take_str(choice, "finish_reason", finish_reason);
            }
        }
        yyjson_val *usage = yyjson_obj_get(root, "usage");
        if (yyjson_is_obj(usage)) {
            yyjson_val *v = yyjson_obj_get(usage, "prompt_tokens");
            if (yyjson_is_int(v)) {
                result.prompt_tokens = yyjson_get_int(v);
            }
            v = yyjson_obj_get(usage, "completion_tokens");
            if (yyjson_is_int(v)) {
                result.completion_tokens = yyjson_get_int(v);
            }
            v = yyjson_obj_get(usage, "total_tokens");
            if (yyjson_is_int(v)) {
                result.total_tokens = yyjson_get_int(v);
            }
            v = yyjson_obj_get(usage, "cached_tokens");
            if (yyjson_is_int(v)) {
                result.cached_tokens = yyjson_get_int(v);
            } else {
                yyjson_val *details = yyjson_obj_get(usage, "prompt_tokens_details");
                if (yyjson_is_obj(details)) {
                    v = yyjson_obj_get(details, "cached_tokens");
                    if (yyjson_is_int(v)) {
                        result.cached_tokens = yyjson_get_int(v);
                    }
                }
            }
        }
    }
    yyjson_doc_free(doc);
    return ok;
}

} // namespace

openai::ChatResult chat_completion_stream(const Config &cfg,
                                         const kimix::vector<ChatMessage> &messages,
                                         const kimix::vector<openai::Tool> &tools,
                                         const openai::ChunkCallback &on_chunk,
                                         const AbortCheck *abort) {
    openai::ChatResult result;
    kimix::string why;
    const kimix::string body = build_chat_body(cfg, messages, tools, &why);
    if (body.empty()) {
        result.error = "failed to build request body";
        if (!why.empty()) {
            result.error += ": " + why;
        }
        return result;
    }

    const Endpoint ep = parse_endpoint(cfg.url);
    if (ep.host.empty()) {
        result.error = "invalid config url: " + cfg.url;
        return result;
    }
    const kimix::string path = join_path(ep.path_prefix, "chat/completions");

    httplib::Client cli(std::string(ep.scheme) + "://" + std::string(ep.host) + ":" +
                        std::to_string(ep.port));
    install_windows_tls_verifier(cli, std::string(ep.host));
    cli.set_connection_timeout(30);
    cli.set_read_timeout(180, 0);
    cli.set_write_timeout(30, 0);

    httplib::Headers headers = {
        {"Accept", "text/event-stream"},
        {"Authorization", "Bearer " + std::string(cfg.api_key)},
        // kimi_cli/llm.py _kimi_default_headers: the Kimi provider is the one
        // family the reference identifies itself to.
        {"User-Agent", "KimiCLI/kimix"},
    };

    // Same transient-failure retry schedule as the OpenAI Chat transport.
    constexpr int kMaxAttempts = 3;
    uint64_t backoff_rng = 0x2E703B2F8EEF8F43ull; // xorshift jitter state
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        result.content.clear();
        result.reasoning.clear();
        result.tool_calls.clear();
        result.prompt_tokens = 0;
        result.completion_tokens = 0;
        result.total_tokens = 0;
        result.cached_tokens = -1;

        openai::SseParser parser;
        kimix::vector<openai::ToolCall> acc_tool_calls;
        kimix::string finish_reason;
        int64_t cached_tokens = -1;

        const auto consume = [&](const openai::ChatChunk &chunk) {
            if (on_chunk) {
                on_chunk(chunk);
            }
            if (chunk.done) {
                return;
            }
            result.content += chunk.content;
            result.reasoning += chunk.reasoning_content;
            for (const auto &tcd : chunk.tool_calls) {
                if ((size_t)tcd.index >= acc_tool_calls.size()) {
                    acc_tool_calls.resize((size_t)tcd.index + 1);
                }
                openai::ToolCall &acc = acc_tool_calls[(size_t)tcd.index];
                if (!tcd.id.empty()) {
                    acc.id = tcd.id;
                }
                if (!tcd.type.empty()) {
                    acc.type = tcd.type;
                }
                if (!tcd.name.empty()) {
                    acc.name = tcd.name;
                }
                acc.arguments += tcd.arguments;
            }
            if (!chunk.finish_reason.empty()) {
                finish_reason = chunk.finish_reason;
            }
            if (chunk.has_usage) {
                result.prompt_tokens = chunk.prompt_tokens;
                result.completion_tokens = chunk.completion_tokens;
                result.total_tokens = chunk.total_tokens;
                if (chunk.cached_tokens >= 0) {
                    cached_tokens = chunk.cached_tokens;
                }
            }
        };

        httplib::ContentReceiver receiver = [&](const char *data, size_t len) -> bool {
            if (abort != nullptr && abort->aborted()) {
                return false;
            }
            for (const auto &chunk : parser.feed(data, len)) {
                consume(chunk);
            }
            return true;
        };
        httplib::Result res =
            cli.Post(std::string(path), headers, std::string(body), "application/json",
                     receiver);
        for (const auto &chunk : parser.finish()) {
            consume(chunk);
        }
        if (abort != nullptr && abort->aborted()) {
            result.content.clear();
            result.reasoning.clear();
            result.tool_calls.clear();
            result.error = "request aborted";
            return result;
        }

        // A 200 body nothing parsed from the SSE stream: the endpoint may
        // answer the non-streaming chat.completion shape anyway (the
        // reference does exactly that when stream=False). Absorb it silently.
        if (res && res->status == 200 && result.content.empty() &&
            result.reasoning.empty() && acc_tool_calls.empty() &&
            finish_reason.empty()) {
            parse_non_stream_body(kimix::string(res->body.data(), res->body.size()),
                                result, finish_reason);
        }
        result.cached_tokens = cached_tokens;

        const bool unusable = acc_tool_calls.empty() && result.content.empty() &&
                              result.reasoning.empty();
        const bool retriable =
            !res || is_retriable_status(res->status) || (res->status == 200 && unusable);
        if (retriable && attempt < kMaxAttempts) {
            const int32_t status = res ? res->status : 0;
            const double retry_after =
                res ? parse_retry_after_seconds(res->get_header_value("Retry-After")) : 0.0;
            std::this_thread::sleep_for(std::chrono::duration<double>(
                rate_limit_aware_wait(attempt, status, retry_after, backoff_rng)));
            continue;
        }

        if (!res) {
            result.error_kind =
                res.error() == httplib::Error::Timeout ||
                        res.error() == httplib::Error::ConnectionTimeout
                    ? TransportErrorKind::timeout
                    : TransportErrorKind::connection;
            result.error = "http error: " + httplib::to_string(res.error());
            return result;
        }
        if (res->status != 200) {
            result.error_kind = TransportErrorKind::http;
            result.error_status = res->status;
            result.retry_after_seconds =
                parse_retry_after_seconds(res->get_header_value("Retry-After"));
            result.error = "http status " + std::to_string(res->status) + ": " +
                           res->body.substr(0, 500);
            return result;
        }
        if (unusable) {
            result.error_kind = TransportErrorKind::empty_response;
            result.error = "backend returned an unusable response body "
                           "(no parseable events: invalid JSON or empty stream)";
            return result;
        }
        result.tool_calls = std::move(acc_tool_calls);
        // Surface the accumulated (or non-stream-parsed) finish reason so it
        // reaches the unified ChatResult (openai::ChatResult.finish_reason).
        result.finish_reason = finish_reason;
        result.ok = true;
        return result;
    }

    result.error = "exhausted retries";
    return result;
}

} // namespace kimix::llm::kimi
