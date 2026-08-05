/*
 * schema_ops.h - JSON Schema deref + type normalization (kimix::runtime::json).
 *
 * Plan 016: bytes-in/bytes-out ports of
 * kosong/utils/jsonschema.py (verified against source):
 *
 *   deref_json_schema(schema, registry, out) : recursive $ref inlining for
 *     local JSON pointers ("#", "#/$defs/...", "#/definitions/..."). Sibling
 *     keys next to a $ref are preserved (2020-12 semantics); circular refs
 *     are detected (visited set) and kept as $ref; unresolvable/remote refs
 *     are left untouched. Definition buckets ($defs/definitions) are dropped
 *     only when no unresolved refs into them remain. `registry` is reserved
 *     (the reference resolves local pointers only).
 *
 *   ensure_property_types(schema, out) : deep copy with an explicit `type`
 *     on every nested property schema (Moonshot rejects schemas whose
 *     properties omit type). Walks the well-known child-schema positions;
 *     infers type from enum/const values, then structural keywords, falling
 *     back to "string"; repairs an explicit type that contradicts the
 *     enum/const values (Xcode MCP bug) and drops structure keywords that no
 *     longer apply. Combinator nodes (anyOf/oneOf/allOf/$ref/if/then/else/
 *     not) are left alone; the outer schema object is a container and is
 *     never normalized.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace json {

// Inline local $ref pointers, drop dead definition buckets, serialize the
// result (compact JSON) into `out`. `registry` is reserved for future
// cross-document resolution (unused today -- local pointers only, exactly
// like the reference).
KIMIX_RUNTIME_API void deref_json_schema(kimix::string_view schema,
                                         kimix::span<const kimix::string_view> registry,
                                         kimix::string& out) noexcept;

// Deep-copy `schema` and inject `type` on every nested property schema;
// serialize the result (compact JSON) into `out`.
KIMIX_RUNTIME_API void ensure_property_types(kimix::string_view schema,
                                             kimix::string& out) noexcept;

} // namespace json
} // namespace runtime
} // namespace kimix
