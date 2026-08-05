// Test for src/runtime/codec/wire_envelope.h (plan 007).
// This test covers:
// - envelope serialize/deserialize round-trip for a registry of type names
//   with nested / unicode / null / number payloads
// - payload embedded as a pre-parsed value (no string escape round-trip)
// - serialize -> deserialize -> serialize byte-identical
// - canonicalize_payload recursive key sort 3 levels deep
// - malformed frame / payload handling (None-like failures)

#include "ut/ut.hpp"
#include <runtime/codec/wire_envelope.h>

#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::codec;

namespace {

const char* kTypeNames[] = {
    "TurnBegin", "SteerInput", "TurnEnd", "StepBegin", "StepInterrupted",
    "StepRetry", "CompactionBegin", "CompactionEnd", "HookTriggered",
    "HookResolved", "MCPLoadingBegin", "MCPLoadingEnd", "MCPServerSnapshot",
    "MCPStatusSnapshot", "LLMToolSchema", "LLMToolsSnapshot", "LLMRequest",
    "MCPToolsDiscovered", "StatusUpdate", "Notification", "BtwBegin",
    "BtwEnd", "SubagentEvent", "ApprovalResponse", "ApprovalRequest",
    "ToolCallRequest", "QuestionRequest", "HookRequest", "TextPart",
    "ThinkPart", "ToolCall", "ToolCallPart", "ToolResult",
};

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "envelope_roundtrip_type_registry"_test = [] {
        // 10 representative payloads covering nested/unicode/null/numbers.
        const char* payloads[] = {
            R"({"user_input": "hello"})",
            R"({"n": 1, "next_attempt": 2, "wait_s": 1.5})",
            R"({"event": "PreToolUse", "target": "", "hook_count": 3})",
            R"({"type": "text", "text": "caf\u00e9 \u4e16\u754c"})",
            R"({"id": null, "category": "sys", "payload": {"a": [1, 2, 3]}})",
            R"({"request_id": "r1", "response": "approve", "feedback": ""})",
            R"({"tools": [{"name": "a", "parameters": {}}]})",
            R"({"loading": true, "connected": 0, "total": 3, "servers": []})",
            R"({"parent_tool_call_id": null, "agent_id": "ag-1"})",
            R"({"event": {"type": "text", "text": "\u00e9"}})",
        };
        const size_t n = sizeof(kTypeNames) / sizeof(kTypeNames[0]);
        for (size_t i = 0; i < n; ++i) {
            wire_envelope env;
            env.type = kTypeNames[i];
            env.payload_json = payloads[i % 10];

            kimix::string frame;
            serialize_envelope(env, frame);
            expect(!frame.empty()) << "frame must be non-empty";
            expect(frame.find("{\"type\":\"") == 0) << "frame must start with type";
            expect(frame.find("\"payload\":") != kimix::string::npos);

            wire_envelope back;
            expect(deserialize_envelope(frame, back));
            expect(eq(back.type, env.type)) << kTypeNames[i];
            // Semantic equality: re-serialize the deserialized payload and
            // the envelope must round-trip byte-identically.
            kimix::string frame2;
            serialize_envelope(back, frame2);
            expect(eq(frame, frame2)) << "serialize(deserialize(frame)) == frame for " << kTypeNames[i];
        }
    };

    "envelope_unicode_and_null_payload"_test = [] {
        wire_envelope env;
        env.type = "TextPart";
        env.payload_json = "{\"type\":\"text\",\"text\":\"\\u4e16\\u754c\\ud83d\\ude00\"}";
        kimix::string frame;
        serialize_envelope(env, frame);
        wire_envelope back;
        expect(deserialize_envelope(frame, back));
        expect(eq(back.type, kimix::string("TextPart")));
        // The escape-decoded payload re-serializes to the raw UTF-8 chars.
        expect(back.payload_json.find("\xe4\xb8\x96") != kimix::string::npos);

        env.payload_json = "null";
        serialize_envelope(env, frame);
        expect(deserialize_envelope(frame, back));
        expect(eq(back.payload_json, kimix::string("null")));

        env.payload_json = "{\"k\":null}";
        serialize_envelope(env, frame);
        expect(deserialize_envelope(frame, back));
        expect(eq(back.payload_json, kimix::string("{\"k\":null}")));
    };

    "envelope_invalid_inputs"_test = [] {
        wire_envelope out;
        expect(!deserialize_envelope("garbage", out));
        expect(!deserialize_envelope("{}", out));          // missing type/payload
        expect(!deserialize_envelope("{\"type\":1,\"payload\":{}}", out)); // type not str
        expect(!deserialize_envelope("[1,2]", out));       // not an object
        expect(out.type.empty());
        expect(out.payload_json.empty());

        // Invalid payload text is embedded as an escaped string (fallback),
        // so the envelope always parses back.
        wire_envelope env;
        env.type = "T";
        env.payload_json = "not json";
        kimix::string frame;
        serialize_envelope(env, frame);
        expect(!frame.empty());
        expect(deserialize_envelope(frame, out));
        expect(eq(out.type, kimix::string("T")));
        expect(!out.payload_json.empty());
    };

    "canonicalize_sorts_keys_recursively"_test = [] {
        const char* cases[][2] = {
            {R"({"b": 2, "a": 1})", R"({"a":1,"b":2})"},
            {R"({"z": {"d": 4, "c": {"f": 6, "e": 5}}, "a": [3, 1, 2]})",
             R"({"a":[3,1,2],"z":{"c":{"e":5,"f":6},"d":4}})"},
            {R"([{"b": 2}, {"a": 1}])", R"([{"b":2},{"a":1}])"},
            {R"({"a": [{"x": 9, "w": 8}], "b": null})",
             R"({"a":[{"w":8,"x":9}],"b":null})"},
            {R"(42)", R"(42)"},
            {R"("str")", R"("str")"},
            {R"(true)", R"(true)"},
            {R"({"a": 1, "A": 2})", R"({"A":2,"a":1})"}, // byte-order sort
        };
        for (const auto& c : cases) {
            kimix::string out;
            expect(canonicalize_payload(c[0], out));
            expect(eq(out, kimix::string(c[1]))) << "input: " << c[0];
        }
        // Invalid JSON -> false, empty out.
        kimix::string out = "x";
        expect(!canonicalize_payload("{", out));
        expect(out.empty());
    };

    "canonicalize_three_levels_deep"_test = [] {
        const char* input =
            R"({"l3": {"l2": {"l1": {"z": 1, "y": 2}, "b": 3}, "a": 4}, "m": 5})";
        kimix::string out;
        expect(canonicalize_payload(input, out));
        expect(eq(out, kimix::string(
            R"({"l3":{"a":4,"l2":{"b":3,"l1":{"y":2,"z":1}}},"m":5})")));
    };

    return 0;
}
