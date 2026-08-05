// Test for src/runtime/json/schema_ops.h (plan 016).
// This test covers:
// - deref_json_schema: $ref inlining (sibling merge, $defs drop, chains)
// - deref cycle detection (ref kept, bucket preserved)
// - deref unresolvable/remote refs left untouched
// - ensure_property_types: enum/const inference, structure inference,
//   explicit-type repair (Xcode MCP bug), combinator skip, recursion

#include "ut/ut.hpp"
#include <runtime/json/schema_ops.h>

#include <cstdio>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::json;

namespace {

kimix::string deref(const char* schema) {
    kimix::string out;
    deref_json_schema(schema, {}, out);
    return out;
}

kimix::string ensure(const char* schema) {
    kimix::string out;
    ensure_property_types(schema, out);
    return out;
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "deref_basic_inline"_test = [] {
        // Local $ref inlines the definition; sibling keys are preserved and
        // take precedence; the dead $defs bucket is dropped.
        const kimix::string out = deref(
            R"({"$ref": "#/$defs/A", "minLength": 2, "$defs": {"A": {"type": "string"}}})");
        expect(eq(out, kimix::string(R"({"type":"string","minLength":2})"))) << out.c_str();
    };

    "deref_chain_and_definitions_bucket"_test = [] {
        const kimix::string out = deref(
            R"({"properties": {"a": {"$ref": "#/definitions/B"}},
                "definitions": {"B": {"$ref": "#/definitions/C"}, "C": {"type": "integer"}}})");
        expect(eq(out, kimix::string(R"({"properties":{"a":{"type":"integer"}}})"))) << out.c_str();
    };

    "deref_cycle_keeps_ref_and_bucket"_test = [] {
        const kimix::string out = deref(
            R"({"properties": {"a": {"$ref": "#/$defs/A"}},
                "$defs": {"A": {"type": "object", "properties": {"self": {"$ref": "#/$defs/A"}}}}})");
        // The outer ref is inlined; the cyclic inner ref stays a $ref and the
        // $defs bucket must be preserved so it stays resolvable.
        expect(out.find("\"properties\"") != kimix::string::npos);
        expect(out.find("\"self\"") != kimix::string::npos);
        expect(out.find("\"$ref\":\"#/$defs/A\"") != kimix::string::npos);
        expect(out.find("\"$defs\"") != kimix::string::npos);
    };

    "deref_unresolvable_and_remote_kept"_test = [] {
        const kimix::string out = deref(
            R"({"properties": {"a": {"$ref": "#/missing"}, "b": {"$ref": "https://x/y"}}})");
        expect(out.find("\"$ref\":\"#/missing\"") != kimix::string::npos);
        expect(out.find("\"$ref\":\"https://x/y\"") != kimix::string::npos);
    };

    "deref_pointer_escapes_and_arrays"_test = [] {
        const kimix::string out = deref(
            R"({"items": {"$ref": "#/$defs/list/0"}, "$defs": {"list": [{"type": "number"}]}})");
        expect(eq(out, kimix::string(R"({"items":{"type":"number"}})"))) << out.c_str();
    };

    "ensure_enum_and_const_inference"_test = [] {
        expect(eq(ensure(R"({"properties": {"x": {"enum": ["a", "b"]}}})"),
                  kimix::string(R"({"properties":{"x":{"enum":["a","b"],"type":"string"}}})")));
        expect(eq(ensure(R"({"properties": {"x": {"const": 3}}})"),
                  kimix::string(R"({"properties":{"x":{"const":3,"type":"integer"}}})")));
        expect(eq(ensure(R"({"properties": {"x": {"enum": [1, 2.5]}}})"),
                  kimix::string(R"({"properties":{"x":{"enum":[1,2.5],"type":"number"}}})")));
    };

    "ensure_structure_inference_and_default"_test = [] {
        expect(eq(ensure(R"({"properties": {"x": {"properties": {}}}})"),
                  kimix::string(R"({"properties":{"x":{"properties":{},"type":"object"}}})")));
        // "items" is a child-schema position: {} normalizes to {"type":"string"}.
        expect(eq(ensure(R"({"properties": {"x": {"items": {}}}})"),
                  kimix::string(R"({"properties":{"x":{"items":{"type":"string"},"type":"array"}}})")));
        expect(eq(ensure(R"({"properties": {"x": {"maxLength": 5}}})"),
                  kimix::string(R"({"properties":{"x":{"maxLength":5,"type":"string"}}})")));
        expect(eq(ensure(R"({"properties": {"x": {"maximum": 5}}})"),
                  kimix::string(R"({"properties":{"x":{"maximum":5,"type":"number"}}})")));
        expect(eq(ensure(R"({"properties": {"x": {}}})"),
                  kimix::string(R"({"properties":{"x":{"type":"string"}}})")));
    };

    "ensure_repairs_contradictory_type"_test = [] {
        // Xcode MCP bug: type object alongside string enum values -> repair.
        expect(eq(ensure(R"({"properties": {"x": {"type": "object", "enum": ["a", "b"]}}})"),
                  kimix::string(R"({"properties":{"x":{"type":"string","enum":["a","b"]}}})")));
    };

    "ensure_skips_combinators_and_nested"_test = [] {
        expect(eq(ensure(R"({"properties": {"x": {"anyOf": [{"enum": ["a"]}]}}})"),
                  kimix::string(R"({"properties":{"x":{"anyOf":[{"enum":["a"],"type":"string"}]}}})")));
        // $defs is a map slot: A itself is normalized (object structure keys).
        expect(eq(ensure(R"({"$defs": {"A": {"properties": {"p": {"enum": [true]}}}}})"),
                  kimix::string(R"({"$defs":{"A":{"properties":{"p":{"enum":[true],"type":"boolean"}},"type":"object"}}})")));
    };

    return 0;
}
