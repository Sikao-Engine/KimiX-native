// tool_schema_validate.h - JSON-Schema meta-validation of tool parameter
// schemas (F12 / audit G26).
//
// Port of kosong/tooling/__init__.py:33-79: every tool's parameters schema is
// validated AGAINST THE META-SCHEMA when the tool object is constructed, so a
// broken schema fails at load instead of shipping a malformed tool definition
// to the provider. The reference raises from the pydantic model validator
// (`_validate_parameters`, 67-79) - an invalid schema makes the TOOLSET
// unusable. The native port has no exceptions and a static-constructor
// registry, so the mirror is: an invalid schema is REFUSED at registration
// with a diagnostic (the tool is not usable), and the diagnostics stay
// queryable for a host/test to report.
//
// This is a light-weight STRUCTURAL validator - the subset of the Draft
// 2020-12 meta-schema that catches the real mistakes a hand-written schema
// can make (the full meta-schema is a large allOf/dynamicRef document):
//   * the document must be a JSON object (or the boolean schemas true/false);
//   * "type": string or array of strings, values from the simpleTypes set
//     {object, array, string, number, integer, boolean, null};
//   * "properties" / "patternProperties" / "$defs" / "definitions":
//     objects whose values are valid schemas;
//   * "items" / "additionalProperties" / "not" / "if" / "then" / "else" /
//     "contains" / "propertyNames" / "unevaluatedItems" /
//     "unevaluatedProperties": valid schemas;
//   * "prefixItems" / "allOf" / "anyOf" / "oneOf": arrays of valid schemas;
//   * "required": array of strings (non-empty strings);
//   * "enum": a non-empty array;
//   * "enum"/"const"/"default"/"examples": any JSON values are allowed.
// Unknown keywords are allowed (JSON Schema is an open world).
//
// Rules (see .agents/skills/cpp): namespace kimix::builtin_tools, kimix::
// containers, no exceptions, no RTTI.

#pragma once

#include <core/kimix_core.h>

#include <llm/yyjson_alc.h>
#include "yyjson.h"

namespace kimix::builtin_tools {
namespace schema_validate {

// Validate `schema_json` as a JSON-Schema document. True when valid; on
// failure returns false with a descriptive `error`
// ("parameters schema: <why> at <path>").
bool validate_parameters_schema(kimix::string_view schema_json,
                                kimix::string &error);

// Same for an already-parsed document root (used by the recursion).
bool validate_schema_value(yyjson_val *root, kimix::string &error);

} // namespace schema_validate
} // namespace kimix::builtin_tools
