// Test for src/runtime/tools/export_builder.h (plan 016).
// This test covers export.py::build_export_markdown formatting:
// - metadata header (session_id/exported_at/work_dir/message_count/token_count)
// - Overview (topic via shorten, comma-grouped token count)
// - turn grouping at real user messages; internal messages skipped
// - assistant content parts incl. Thinking details; tool-call blocks with
//   indented args JSON; tool-result collapsible blocks
// - escaping (quotes in text)

#include "ut/ut.hpp"
#include <runtime/tools/export_builder.h>
#include <runtime/soul/message_view.h>
#include "unit/native/soul_test_util.h"

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::soul;
using namespace kimix::runtime::tools;
using soul_test::message_builder;
using soul_test::part_kind;

namespace {

kimix::string build(const kimix::vector<kimix::runtime::soul::message_view>& msgs) {
    export_options opts;
    opts.session_id = "sess-001";
    opts.work_dir = "C:/work";
    opts.exported_at = "2024-05-01T12:00:00";
    opts.token_count = 1234567;
    kimix::string out;
    build_export_markdown(msgs, opts, out);
    return out;
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "export_header_and_overview"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "What is the weather?");
        b.begin_message(kRoleAssistant);
        b.part(part_kind::TEXT, "It is sunny.");
        const kimix::string out = build(b.finish());
        expect(out.find("---\nsession_id: sess-001\nexported_at: 2024-05-01T12:00:00\n"
                        "work_dir: C:/work\nmessage_count: 2\ntoken_count: 1234567\n---\n\n"
                        "# Kimi Session Export\n") == 0);
        // Overview: topic from the first real user message, comma-grouped.
        expect(out.find("## Overview\n\n- **Topic**: What is the weather?\n"
                        "- **Conversation**: 1 turns | 0 tool calls | 1,234,567 tokens\n\n---") !=
               kimix::string::npos);
        expect(out.find("## Turn 1\n\n### User\n\nWhat is the weather?\n\n"
                        "### Assistant\n\nIt is sunny.") != kimix::string::npos);
    };

    "export_tool_call_and_result_blocks"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "read a.txt");
        b.begin_message(kRoleAssistant);
        b.call("call_1", "read_file", R"({"path": "a.txt"})");
        b.begin_message(kRoleTool, "call_1");
        b.part(part_kind::TEXT, "file contents here");
        const kimix::string out = build(b.finish());
        expect(out.find("#### Tool Call: read_file (`a.txt`)") != kimix::string::npos);
        expect(out.find("<!-- call_id: call_1 -->") != kimix::string::npos);
        // args re-serialized with orjson OPT_INDENT_2 (2-space indent).
        expect(out.find("```json\n{\n  \"path\": \"a.txt\"\n}\n```") != kimix::string::npos);
        expect(out.find("<details><summary>Tool Result: read_file (`a.txt`)</summary>\n\n"
                        "<!-- call_id: call_1 -->\nfile contents here\n\n</details>") !=
               kimix::string::npos);
    };

    "export_thinking_details_and_escaping"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "say \"hi\"");
        b.begin_message(kRoleAssistant);
        b.part(part_kind::THINK, "considering");
        b.part(part_kind::TEXT, "done");
        const kimix::string out = build(b.finish());
        expect(out.find("<details><summary>Thinking</summary>\n\nconsidering\n\n</details>") !=
               kimix::string::npos);
        expect(out.find("say \"hi\"") != kimix::string::npos);
    };

    "export_internal_messages_skipped"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "<system-reminder>\nr\n</system-reminder>");
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "real");
        b.begin_message(kRoleAssistant);
        b.part(part_kind::TEXT, "answer");
        const kimix::string out = build(b.finish());
        // The reminder is skipped; message_count still counts it.
        expect(out.find("message_count: 3") != kimix::string::npos);
        expect(out.find("system-reminder") == kimix::string::npos);
        expect(out.find("## Turn 1\n\n### User\n\nreal\n\n### Assistant\n\nanswer") !=
               kimix::string::npos);
    };

    "export_empty_history"_test = [] {
        kimix::vector<kimix::runtime::soul::message_view> none;
        const kimix::string out = build(none);
        expect(out.find("- **Topic**: (empty)") != kimix::string::npos);
        expect(out.find("0 turns | 0 tool calls | 1,234,567 tokens") != kimix::string::npos);
    };

    return 0;
}
