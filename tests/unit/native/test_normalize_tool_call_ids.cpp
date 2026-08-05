// Test for src/runtime/soul/normalize_tool_call_ids.h (plan 014).
// This test covers kimi.py::_normalize_tool_call_ids semantics:
// - valid ids keep their value (empty plan)
// - invalid chars replaced with _ (per code point), truncated to 64
// - dedupe with _2/_3 suffixes; empty base -> "tool_call"
// - tool_call_id fixes use the UINT32_MAX sentinel
// - tool_calls before tool_call_id per message; index-ascending

#include "ut/ut.hpp"
#include <runtime/soul/normalize_tool_call_ids.h>
#include <runtime/soul/message_view.h>
#include "unit/native/soul_test_util.h"

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::soul;
using soul_test::message_builder;

namespace {

kimix::vector<id_fix> run(const kimix::vector<message_view>& msgs) {
    kimix::vector<id_fix> out;
    normalize_tool_call_ids(msgs, out);
    return out;
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "ids_already_valid_no_fixes"_test = [] {
        message_builder b;
        b.begin_message(kRoleAssistant);
        b.call("call_abc-123", "f", "{}");
        b.begin_message(kRoleTool, "call_abc-123");
        b.part(part_kind::TEXT, "r");
        const auto fixes = run(b.finish());
        expect(fixes.empty());
    };

    "invalid_chars_sanitized_and_truncated"_test = [] {
        message_builder b;
        b.begin_message(kRoleAssistant);
        b.call("Read:9", "f", "{}");
        // 70-char id -> truncated to 64
        kimix::string long_id(70, 'a');
        b.call(long_id.c_str(), "g", "{}");
        const auto fixes = run(b.finish());
        expect(eq(fixes.size(), size_t{2}));
        // "Read:9" -> "Read_9"
        expect(eq(fixes[0].msg_index, 0u));
        expect(eq(fixes[0].call_index, 0u));
        expect(eq(fixes[0].new_id, kimix::string("Read_9")));
        // 70 x 'a' -> 64 x 'a'
        expect(eq(fixes[1].call_index, 1u));
        expect(eq(fixes[1].new_id.size(), size_t{64}));
        expect(fixes[1].new_id.find_first_not_of('a') == kimix::string::npos);
    };

    "dedupe_suffixes_and_empty_base"_test = [] {
        message_builder b;
        b.begin_message(kRoleAssistant);
        b.call("bad id", "f", "{}");
        b.call("bad\tid", "g", "{}"); // tab -> _ : collides with "bad_id"
        b.call("", "h", "{}"); // empty id -> "tool_call"
        const auto fixes = run(b.finish());
        expect(eq(fixes.size(), size_t{3}));
        expect(eq(fixes[0].new_id, kimix::string("bad_id")));
        expect(eq(fixes[1].new_id, kimix::string("bad_id_2")));
        expect(eq(fixes[2].new_id, kimix::string("tool_call")));
    };

    "tool_call_id_fix_sentinel"_test = [] {
        message_builder b;
        b.begin_message(kRoleTool, "Read:9");
        b.part(part_kind::TEXT, "r");
        const auto fixes = run(b.finish());
        expect(eq(fixes.size(), size_t{1}));
        expect(eq(fixes[0].msg_index, 0u));
        expect(eq(fixes[0].call_index, UINT32_MAX));
        expect(eq(fixes[0].new_id, kimix::string("Read_9")));
    };

    "tool_calls_before_tool_call_id_per_message"_test = [] {
        message_builder b;
        b.begin_message(kRoleAssistant);
        b.call("a b", "f", "{}");
        b.begin_message(kRoleTool, "a b");
        b.part(part_kind::TEXT, "r");
        const auto fixes = run(b.finish());
        expect(eq(fixes.size(), size_t{2}));
        expect(eq(fixes[0].msg_index, 0u));
        expect(eq(fixes[0].call_index, 0u));
        expect(eq(fixes[1].msg_index, 1u));
        expect(eq(fixes[1].call_index, UINT32_MAX));
        expect(eq(fixes[0].new_id, fixes[1].new_id)); // consistent rewrite
    };

    "empty_history"_test = [] {
        kimix::vector<message_view> none;
        expect(run(none).empty());
    };

    return 0;
}
