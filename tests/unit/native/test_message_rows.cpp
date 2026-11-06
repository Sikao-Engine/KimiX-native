// Test for src/runtime/index/message_rows.{h,cpp} — history persistence rows
// over the arena (report candidate B2, serialisation only).
//
// Covers the row JSON contract (a lone TextPart collapses to a string, a think
// part keeps `encrypted: null`, tool calls use the model_dump shape), the FTS
// text (`"\n".join(text parts)`), the packed framing, and the parse_rows round
// trip back into a HistoryStore incl. malformed input.

#include "ut/ut.hpp"

#include <runtime/common/le_bytes.h>
#include <runtime/index/message_rows.h>
#include <runtime/soul/history_store.h>
#include <runtime/soul/kmb2.h>
#include <runtime/soul/message_view.h>

#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;
using kimix::runtime::index::context_rows;
using kimix::runtime::index::parse_rows;
using kimix::runtime::soul::HistoryStore;
using kimix::runtime::soul::message_view;
using kimix::runtime::soul::part_kind;
using kimix::runtime::soul::part_view;
using kimix::runtime::soul::tool_call_view;

namespace tests_unit_native_test_message_rows {

struct fixture {
    kimix::vector<kimix::vector<part_view>> parts;
    kimix::vector<kimix::vector<tool_call_view>> calls;
    kimix::vector<message_view> msgs;
    kimix::vector<kimix::string> tcid;

    fixture() {
        parts.resize(4);
        calls.resize(0);
        calls.resize(4);
        tcid.resize(4);

        // 0 user: one text part -> content collapses to a string.
        parts[0].push_back(part_view{part_kind::TEXT, "hello"});
        // 1 assistant: text + tool call with arguments.
        parts[1].push_back(part_view{part_kind::TEXT, "reading"});
        calls[1].push_back(tool_call_view{"call_1", "read", "{\"path\":\"a\"}"});
        // 2 tool: result with a multi-part content list.
        parts[2].push_back(part_view{part_kind::TEXT, "line one"});
        parts[2].push_back(part_view{part_kind::TEXT, "line two"});
        tcid[2] = "call_1";
        // 3 user: think part + text part.
        parts[3].push_back(part_view{part_kind::THINK, "why"});
        parts[3].push_back(part_view{part_kind::TEXT, "answer"});

        msgs.resize(4);
        const uint8_t roles[4] = {kimix::runtime::soul::kRoleUser,
                                  kimix::runtime::soul::kRoleAssistant,
                                  kimix::runtime::soul::kRoleTool,
                                  kimix::runtime::soul::kRoleUser};
        const bool has_tcid[4] = {false, false, true, false};
        for (size_t i = 0; i < 4; ++i) {
            msgs[i].role = roles[i];
            msgs[i].parts = kimix::span<const part_view>(parts[i].data(), parts[i].size());
            msgs[i].tool_calls =
                kimix::span<const tool_call_view>(calls[i].data(), calls[i].size());
            if (has_tcid[i]) {
                msgs[i].tool_call_id = kimix::string_view(tcid[i]);
            }
        }
    }
};

kimix::vector<kimix::string> decode_rows(kimix::string_view packed) {
    kimix::vector<kimix::string> rows;
    const uint32_t count = kimix::runtime::common::read_le32(packed.data());
    size_t offset = 4;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t json_len =
            kimix::runtime::common::read_le32(packed.data() + offset);
        const uint32_t fts_len =
            kimix::runtime::common::read_le32(packed.data() + offset + 4);
        offset += 8;
        rows.push_back(kimix::string(packed.data() + offset, json_len));
        rows.push_back(kimix::string(packed.data() + offset + json_len, fts_len));
        offset += json_len + fts_len;
    }
    return rows;
}

} // namespace tests_unit_native_test_message_rows
using namespace tests_unit_native_test_message_rows;

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "message_rows_json_contract"_test = [] {
        fixture f;
        kimix::string packed;
        context_rows(f.msgs, packed);
        expect(!packed.empty());
        expect(kimix::runtime::common::read_le32(packed.data()) == 4u);

        const kimix::vector<kimix::string> rows = decode_rows(packed);
        expect(rows.size() == 8u); // 4 (json, fts) pairs
        // A lone TextPart collapses to a plain string.
        expect(rows[0] == kimix::string_view("{\"role\":\"user\",\"content\":\"hello\"}"));
        expect(rows[1] == kimix::string_view("hello"));
        // Tool call in the model_dump(exclude_none=True) shape.
        expect(rows[2] ==
               kimix::string_view("{\"role\":\"assistant\",\"content\":\"reading\","
                                  "\"tool_calls\":[{\"type\":\"function\","
                                  "\"id\":\"call_1\",\"function\":{\"name\":\"read\","
                                  "\"arguments\":\"{\\\"path\\\":\\\"a\\\"}\"}}]}"));
        expect(rows[3] == kimix::string_view("reading"));
        // Two text parts -> a content list + a newline-joined FTS text.
        expect(rows[4] == kimix::string_view(
                              "{\"role\":\"tool\",\"content\":[{\"type\":\"text\","
                              "\"text\":\"line one\"},{\"type\":\"text\","
                              "\"text\":\"line two\"}],\"tool_call_id\":\"call_1\"}"));
        expect(rows[5] == kimix::string_view("line one\nline two"));
        // Think parts keep the None default of `encrypted` (model_dump, not
        // model_dump_json).
        expect(rows[6] == kimix::string_view(
                              "{\"role\":\"user\",\"content\":[{\"type\":\"think\","
                              "\"think\":\"why\",\"encrypted\":null},{\"type\":\"text\","
                              "\"text\":\"answer\"}]}"));
        expect(rows[7] == kimix::string_view("answer"));
    };

    "message_rows_empty"_test = [] {
        kimix::string packed;
        context_rows(kimix::span<const message_view>(), packed);
        expect(packed.size() == 4u);
        expect(kimix::runtime::common::read_le32(packed.data()) == 0u);
    };

    "message_rows_roundtrip"_test = [] {
        fixture f;
        kimix::string packed;
        context_rows(f.msgs, packed);

        HistoryStore store;
        expect(parse_rows(packed, store) == 4u);
        expect(store.size() == 4u);
        for (uint32_t i = 0; i < 4; ++i) {
            message_view a = f.msgs[i];
            message_view b;
            expect(store.view(i, b));
            expect(a.role == b.role);
            expect(a.parts.size() == b.parts.size());
            if (i == 3) {
                // A think part round-trips through its text.
                expect(b.parts[0].kind == part_kind::THINK);
                expect(b.parts[0].text == "why");
            } else {
                expect(a.parts[0].text == b.parts[0].text);
            }
            expect(a.tool_calls.size() == b.tool_calls.size());
            if (!a.tool_calls.empty()) {
                expect(a.tool_calls[0].id == b.tool_calls[0].id);
                expect(a.tool_calls[0].name == b.tool_calls[0].name);
                expect(a.tool_calls[0].arguments == b.tool_calls[0].arguments);
            }
            expect(a.tool_call_id == b.tool_call_id);
        }
    };

    "message_rows_rejects_malformed"_test = [] {
        fixture f;
        kimix::string packed;
        context_rows(f.msgs, packed);

        // Framing failures append nothing at all.
        HistoryStore empty;
        expect(parse_rows("", empty) == 0u);
        expect(parse_rows("abc", empty) == 0u);
        expect(empty.size() == 0u);

        // A count larger than the blob decodes the rows it has and reports a
        // SHORT count (that is how the caller detects the corrupt row).
        kimix::string bad = packed;
        kimix::runtime::common::put_le32(bad.data(), 99u);
        HistoryStore short_store;
        const uint32_t decoded = parse_rows(bad, short_store);
        expect(decoded == 4u);
        expect(decoded < 99u);

        // A truncated row / a non-JSON row stop before the last message.
        kimix::string truncated = packed.substr(0, packed.size() - 3);
        HistoryStore trunc_store;
        expect(parse_rows(truncated, trunc_store) < 4u);

        kimix::string not_json = packed;
        const size_t first_row_offset = 12; // 4 (count) + 8 (lens)
        std::memset(not_json.data() + first_row_offset, 'x', 4);
        HistoryStore bad_row_store;
        expect(parse_rows(not_json, bad_row_store) < 4u);
    };

    "bench_message_rows_750"_test = [] {
        // 750 messages with one text part each: the report's fixture size.
        kimix::vector<kimix::vector<part_view>> parts(750);
        kimix::vector<message_view> msgs(750);
        kimix::vector<kimix::string> texts(750);
        for (size_t i = 0; i < 750; ++i) {
            texts[i] = kimix::format("message body number {}", i);
            parts[i].push_back(part_view{part_kind::TEXT, kimix::string_view(texts[i])});
            msgs[i].role = static_cast<uint8_t>(i % 4);
            msgs[i].parts = kimix::span<const part_view>(parts[i].data(), parts[i].size());
        }
        kimix::string packed;
        context_rows(msgs, packed);
        expect(!packed.empty());
        HistoryStore store;
        expect(parse_rows(packed, store) == 750u);
        expect(store.size() == 750u);
    };

    return 0;
}
