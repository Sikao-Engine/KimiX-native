// Test for src/runtime/soul/payload_builder.h (plan 014).
// This test covers _convert_message semantics:
// - role mapping (system/user/assistant/tool)
// - single TextPart content -> plain string; multi-part -> array
// - ThinkPart split into visible content + reasoning_content
// - empty-content-drop for assistant tool-call messages (Kimi quirk)
// - reasoning_content backfill "" with preserved_thinking
// - exclude_none field dropping (tool_call_id absent, arguments None)
// - non-text parts embedded as raw JSON fragments

#include "ut/ut.hpp"
#include <runtime/soul/payload_builder.h>
#include <runtime/soul/message_view.h>
#include "unit/native/soul_test_util.h"

#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::soul;
using soul_test::message_builder;

namespace {

kimix::string build(const kimix::vector<message_view>& msgs, bool preserved = false) {
    kimix::string out;
    build_payload(msgs, preserved, out);
    return out;
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "payload_role_mapping_and_text"_test = [] {
        message_builder b;
        b.begin_message(kRoleSystem);
        b.part(part_kind::TEXT, "sys");
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "hi there");
        b.begin_message(kRoleAssistant);
        b.part(part_kind::TEXT, "ok");
        b.begin_message(kRoleTool, "call_1");
        b.part(part_kind::TEXT, "result");
        const kimix::string out = build(b.finish());
        expect(eq(out, kimix::string(
            R"([{"role":"system","content":"sys"},{"role":"user","content":"hi there"},)"
            R"({"role":"assistant","content":"ok"},{"role":"tool","content":"result","tool_call_id":"call_1"}])")));
    };

    "payload_reasoning_split"_test = [] {
        message_builder b;
        b.begin_message(kRoleAssistant);
        b.part(part_kind::THINK, "let me think");
        b.part(part_kind::TEXT, "visible answer");
        const kimix::string out = build(b.finish());
        expect(eq(out, kimix::string(
            R"([{"role":"assistant","content":"visible answer","reasoning_content":"let me think"}])")));
    };

    "payload_think_only_no_tool_calls"_test = [] {
        message_builder b;
        b.begin_message(kRoleAssistant);
        b.part(part_kind::THINK, "r");
        // visible content is empty and there are no tool calls: content stays [].
        const kimix::string out = build(b.finish());
        expect(eq(out, kimix::string(
            R"([{"role":"assistant","content":[],"reasoning_content":"r"}])")));
    };

    "payload_empty_content_drop_for_tool_calls"_test = [] {
        message_builder b;
        b.begin_message(kRoleAssistant);
        b.call("call_1", "read_file", R"({"path":"a.txt"})");
        // assistant + tool_calls + effectively-empty visible content -> content dropped.
        const kimix::string out = build(b.finish());
        expect(eq(out, kimix::string(
            R"([{"role":"assistant","tool_calls":[{"type":"function","id":"call_1",)"
            R"("function":{"name":"read_file","arguments":"{\"path\":\"a.txt\"}"}}]}])")));
        // With non-empty visible content the content field is kept.
        message_builder b2;
        b2.begin_message(kRoleAssistant);
        b2.part(part_kind::TEXT, "visible");
        b2.call("c", "f", "{}");
        expect(eq(build(b2.finish()), kimix::string(
            R"([{"role":"assistant","content":"visible","tool_calls":[{"type":"function","id":"c",)"
            R"("function":{"name":"f","arguments":"{}"}}]}])")));
        // Whitespace-only visible content is effectively empty -> dropped.
        message_builder b3;
        b3.begin_message(kRoleAssistant);
        b3.part(part_kind::TEXT, "  ");
        b3.call("c", "f", "{}");
        expect(eq(build(b3.finish()), kimix::string(
            R"([{"role":"assistant","tool_calls":[{"type":"function","id":"c",)"
            R"("function":{"name":"f","arguments":"{}"}}]}])")));
    };

    "payload_preserved_thinking_backfill"_test = [] {
        message_builder b;
        b.begin_message(kRoleAssistant);
        b.part(part_kind::TEXT, "no reasoning here");
        // preserved_thinking -> "" backfill on every assistant message.
        expect(eq(build(b.finish(), true), kimix::string(
            R"([{"role":"assistant","content":"no reasoning here","reasoning_content":""}])")));
        // without preserved_thinking no reasoning_content appears.
        expect(eq(build(b.finish(), false), kimix::string(
            R"([{"role":"assistant","content":"no reasoning here"}])")));
        // tool messages never get the backfill.
        message_builder b2;
        b2.begin_message(kRoleTool, "c1");
        b2.part(part_kind::TEXT, "r");
        expect(eq(build(b2.finish(), true), kimix::string(
            R"([{"role":"tool","content":"r","tool_call_id":"c1"}])")));
    };

    "payload_multi_part_content_array"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "a");
        b.part(part_kind::TEXT, "b");
        const kimix::string out = build(b.finish());
        expect(eq(out, kimix::string(
            R"([{"role":"user","content":[{"type":"text","text":"a"},{"type":"text","text":"b"}]}])")));
    };

    "payload_image_fragment_embed"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::IMAGE, R"({"type":"image_url","image_url":{"url":"https://x/y.png"}})");
        const kimix::string out = build(b.finish());
        expect(eq(out, kimix::string(
            R"([{"role":"user","content":[{"type":"image_url","image_url":{"url":"https://x/y.png"}}]}])")));
    };

    "payload_arguments_none_dropped"_test = [] {
        message_builder b;
        b.begin_message(kRoleAssistant);
        b.call_args_none("c1", "f");
        const kimix::string out = build(b.finish());
        expect(eq(out, kimix::string(
            R"([{"role":"assistant","tool_calls":[{"type":"function","id":"c1",)"
            R"("function":{"name":"f"}}]}])")));
    };

    "payload_escaping_and_unicode"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "quote \" backslash \\ newline \n tab \t");
        const kimix::string out = build(b.finish());
        expect(eq(out, kimix::string(
            R"([{"role":"user","content":"quote \" backslash \\ newline \n tab \t"}])")));
    };

    "payload_empty_history"_test = [] {
        kimix::vector<message_view> none;
        expect(eq(build(none), kimix::string("[]")));
    };

    return 0;
}
