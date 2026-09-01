// Test for src/runtime/tools/export_builder.h (plan 016).
// This test covers export.py::build_export_markdown formatting:
// - metadata header (session_id/exported_at/work_dir/message_count/token_count)
// - Overview (topic via shorten, comma-grouped token count)
// - turn grouping at real user messages; internal messages skipped
// - assistant content parts incl. Thinking details; tool-call blocks with
//   indented args JSON; tool-result collapsible blocks
// - escaping (quotes in text)

#include "ut/ut.hpp"
#include "unit/native/bench_util.h"
#include <runtime/tools/export_builder.h>
#include <runtime/soul/message_view.h>
#include "unit/native/soul_test_util.h"

#include <string>

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

size_t count_occ(kimix::string_view hay, kimix::string_view needle) {
    size_t n = 0;
    size_t pos = 0;
    while ((pos = hay.find(needle, pos)) != kimix::string_view::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

// Synthetic conversation for export benchmarks: strict user/assistant/tool
// alternation; every assistant message carries one tool call whose JSON
// arguments are args_base + (i % args_step) bytes long (valid JSON).  All
// strings live in one stable buffer so message_view spans stay valid.
struct export_batch {
    kimix::string blob;
    kimix::vector<kimix::runtime::soul::part_view> parts;
    kimix::vector<kimix::runtime::soul::tool_call_view> calls;
    kimix::vector<kimix::runtime::soul::message_view> msgs;
    size_t n_users = 0;
    size_t n_calls = 0;
};

export_batch make_export_batch(size_t n_msgs, size_t args_base,
                               size_t payload_steps, size_t payload_step) {
    export_batch b;
    b.blob.reserve(n_msgs * (args_base + payload_steps * payload_step + 512));

    struct part_rec {
        part_kind kind;
        size_t off;
        size_t len;
    };
    struct call_rec {
        size_t id_o, id_l, name_o, name_l, args_o, args_l;
    };
    struct msg_rec {
        uint8_t role;
        size_t tcid_o = 0, tcid_l = 0;
        size_t part_b, part_e, call_b, call_e;
    };
    kimix::vector<part_rec> parts;
    kimix::vector<call_rec> calls;
    kimix::vector<msg_rec> msgs;
    parts.reserve(n_msgs);
    calls.reserve(n_msgs / 3 + 1);
    msgs.reserve(n_msgs);

    const auto put = [&](kimix::string_view s) {
        const size_t off = b.blob.size();
        b.blob.append(s.data(), s.size());
        return off;
    };
    const auto put_str = [&](const std::string& s) {
        const size_t off = b.blob.size();
        b.blob.append(s.data(), s.size());
        return off;
    };

    static const char* kUseful =
        "0123456789abcdefghijklmnopqrstuvwxyz";

    for (size_t i = 0; i < n_msgs; ++i) {
        msg_rec m;
        m.part_b = parts.size();
        m.call_b = calls.size();
        switch (i % 3) {
        case 0: { // user
            m.role = kimix::runtime::soul::kRoleUser;
            std::string text =
                "What is in file sample_" + std::to_string(i) + ".txt?";
            const size_t o = put_str(text);
            parts.push_back({part_kind::TEXT, o, text.size()});
            ++b.n_users;
            break;
        }
        case 1: { // assistant: text + one tool call
            m.role = kimix::runtime::soul::kRoleAssistant;
            static const char* kText = "Let me check the file.";
            const size_t o = put(kimix::string_view(kText));
            parts.push_back({part_kind::TEXT, o, std::char_traits<char>::length(kText)});
            const std::string id = "call_" + std::to_string(i);
            const std::string name = "read_file";
            const size_t payload_len = args_base + (i % payload_steps) * payload_step;
            std::string args = "{\"query\": \"describe sample";
            args += std::to_string(i);
            args += "\", \"path\": \"C:/data/sample_";
            args += std::to_string(i % 1000);
            args += ".txt\", \"payload\": \"";
            args.reserve(args.size() + payload_len + 64);
            for (size_t k = 0; k < payload_len; ++k) {
                args.push_back(kUseful[k % 36]);
            }
            args += "\", \"config\": {\"depth\": 3, \"tags\": [\"read\", \"scan\", \"report\"]}}";
            call_rec c;
            c.id_o = put_str(id);
            c.id_l = id.size();
            c.name_o = put_str(name);
            c.name_l = name.size();
            c.args_o = put(args);
            c.args_l = args.size();
            calls.push_back(c);
            ++b.n_calls;
            break;
        }
        default: { // tool result
            m.role = kimix::runtime::soul::kRoleTool;
            const std::string call_id = "call_" + std::to_string(i - 1);
            m.tcid_o = put_str(call_id);
            m.tcid_l = call_id.size();
            std::string text =
                "File sample_" + std::to_string(i) + ".txt contains: ";
            text.reserve(text.size() + 192);
            for (size_t k = 0; k < 192; ++k) {
                text.push_back("0123456789abcdef"[k % 16]);
            }
            const size_t o = put_str(text);
            parts.push_back({part_kind::TEXT, o, text.size()});
            break;
        }
        }
        m.part_e = parts.size();
        m.call_e = calls.size();
        msgs.push_back(m);
    }

    const char* base = b.blob.data();
    b.parts.reserve(parts.size());
    for (const part_rec& pr : parts) {
        kimix::runtime::soul::part_view p;
        p.kind = pr.kind;
        p.text = kimix::string_view(base + pr.off, pr.len);
        b.parts.push_back(p);
    }
    b.calls.reserve(calls.size());
    for (const call_rec& c : calls) {
        kimix::runtime::soul::tool_call_view t;
        t.id = kimix::string_view(base + c.id_o, c.id_l);
        t.name = kimix::string_view(base + c.name_o, c.name_l);
        t.arguments = kimix::string_view(base + c.args_o, c.args_l);
        b.calls.push_back(t);
    }
    b.msgs.reserve(msgs.size());
    for (const msg_rec& m : msgs) {
        kimix::runtime::soul::message_view v;
        v.role = m.role;
        v.tool_call_id = (m.tcid_l > 0)
                             ? kimix::string_view(base + m.tcid_o, m.tcid_l)
                             : kimix::string_view();
        v.parts = kimix::span<const kimix::runtime::soul::part_view>(
            b.parts.data() + m.part_b, m.part_e - m.part_b);
        v.tool_calls = kimix::span<const kimix::runtime::soul::tool_call_view>(
            b.calls.data() + m.call_b, m.call_e - m.call_b);
        b.msgs.push_back(v);
    }
    return b;
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

    // ------------------------------------------------------------------
    // Benchmarks (kimix_bench harness; "[bench] ..." lines go to stderr).
    // Conversation-export workloads sized like production.
    // ------------------------------------------------------------------

    "bench_export_1k_messages"_test = [] {
        export_batch batch = make_export_batch(1000, 4096, 12, 1000); // args 4-16 KB
        expect(batch.n_users == 334u);
        expect(batch.n_calls == 333u);
        export_options opts;
        opts.session_id = "sess-bench";
        opts.work_dir = "C:/work";
        opts.exported_at = "2024-05-01T12:00:00";
        opts.token_count = 1234567;
        kimix::string out;
        build_export_markdown(batch.msgs, opts, out);
        expect(out.find("---\nsession_id: sess-bench") == 0);
        expect(count_occ(out, "#### Tool Call: ") == batch.n_calls);
        expect(count_occ(out, "## Turn ") == batch.n_users);
        expect(out.size() > 2'500'000);
        const double ref_bytes = double(out.size());
        kimix_bench::run("export/1k_messages", [&] {
            build_export_markdown(batch.msgs, opts, out);
            kimix_bench::sink(out.size());
        }, 1, ref_bytes, 0.25);
    };

    "bench_export_10k_messages"_test = [] {
        export_batch batch = make_export_batch(10000, 4096, 8, 512); // args 4-8 KB
        expect(batch.n_users == 3334u);
        expect(batch.n_calls == 3333u);
        export_options opts;
        opts.session_id = "sess-bench";
        opts.work_dir = "C:/work";
        opts.exported_at = "2024-05-01T12:00:00";
        opts.token_count = 1234567;
        kimix::string out;
        build_export_markdown(batch.msgs, opts, out);
        expect(out.find("---\nsession_id: sess-bench") == 0);
        expect(count_occ(out, "#### Tool Call: ") == batch.n_calls);
        expect(count_occ(out, "## Turn ") == batch.n_users);
        expect(out.size() > 16'000'000);
        const double ref_bytes = double(out.size());
        kimix_bench::run("export/10k_messages", [&] {
            build_export_markdown(batch.msgs, opts, out);
            kimix_bench::sink(out.size());
        }, 1, ref_bytes, 0.25);
    };

    return 0;
}
