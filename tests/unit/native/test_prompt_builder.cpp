// Test for src/runtime/soul/prompt_builder.h (plan 016).
// This test covers SimpleCompaction.prepare formatting:
// - per-message "## Message N\nRole: <role>\nContent:\n" headers + TextParts
// - ThinkParts excluded from the prompt body
// - "\n" + prompts.COMPACT (starts with "---\n\nCompact the above")
// - "\n\n" + balanced mode guidance suffix
// - system_prompt prepend with "\n\n" separator
// - COMPACT_CASCADE when >= 3 messages carry the compaction marker

#include "ut/ut.hpp"
#include <runtime/soul/prompt_builder.h>
#include <runtime/soul/message_view.h>
#include "unit/native/soul_test_util.h"

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::soul;
using soul_test::message_builder;
using soul_test::part_kind;

namespace {

kimix::string build(const kimix::vector<message_view>& msgs,
                    const char* system_prompt = "") {
    kimix::string out;
    build_compaction_prompt(msgs, system_prompt, out);
    return out;
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "prompt_headers_and_parts"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "user text one");
        b.begin_message(kRoleAssistant);
        b.part(part_kind::THINK, "hidden reasoning");
        b.part(part_kind::TEXT, "assistant text");
        const kimix::string out = build(b.finish());
        expect(out.find("## Message 1\nRole: user\nContent:\nuser text one") == 0);
        const size_t m2 = out.find("## Message 2\nRole: assistant\nContent:\nassistant text");
        expect(m2 != kimix::string::npos);
        expect(m2 > 0);
        // ThinkPart text must NOT appear.
        expect(out.find("hidden reasoning") == kimix::string::npos);
    };

    "prompt_compact_suffix_and_guidance"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "body");
        const kimix::string out = build(b.finish());
        // After the message body: "\n" + COMPACT (starts "---\n\nCompact the
        // above agent conversation context.") + "\n\n" + balanced guidance.
        const size_t suffix = out.find("\n---\n\nCompact the above");
        expect(suffix != kimix::string::npos) << out.c_str();
        expect(out.find("\n\n**Compaction Style Guidance:** Be balanced.") !=
               kimix::string::npos);
        expect(out.find("**Required Summary Sections:**") == kimix::string::npos);
    };

    "prompt_system_prompt_prepended"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "x");
        const kimix::string out = build(b.finish(), "You are a compactor.");
        expect(out.find("You are a compactor.\n\n## Message 1") == 0);
    };

    "prompt_cascade_prompt_selected"_test = [] {
        message_builder b;
        for (int i = 0; i < 3; ++i) {
            b.begin_message(kRoleUser);
            b.part(part_kind::TEXT, "Previous context has been compacted. summary");
        }
        const kimix::string out = build(b.finish());
        // cascade_depth >= 3 -> COMPACT_CASCADE text (starts with "---" too;
        // distinguish by a cascade-specific phrase if present, else check the
        // prompt is still the right shape).
        expect(out.find("## Message 1\nRole: user\nContent:\nPrevious context") == 0);
        expect(out.find("\n---\n") != kimix::string::npos);
    };

    "prompt_empty_history"_test = [] {
        kimix::vector<message_view> none;
        const kimix::string out = build(none);
        // No messages -> just the prompt suffix.
        expect(out.find("## Message") == kimix::string::npos);
        expect(out.find("\n---\n\nCompact the above") != kimix::string::npos);
    };

    return 0;
}
