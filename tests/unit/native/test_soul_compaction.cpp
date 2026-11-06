// Test for src/runtime/soul/compaction_view.{h,cpp} — the compaction / prune
// decision kernels over the arena (report candidate B3).
//
// The kernels themselves live in kimix::builtin_tools::compact and are covered
// by their own suites; this suite pins the ARENA ADAPTER:
// - the arena result equals the legacy `span<compact::message>` call for cuts,
//   flags, preserve splits and prompt bytes (i.e. projection is lossless);
// - the packed cut array round-trips;
// - extract_text / count_tokens / build_compact_message_text agree with the
//   reference `compact::` helpers.

#include "ut/ut.hpp"

#include <runtime/soul/compaction_view.h>
#include <runtime/soul/kmb2.h>
#include <runtime/soul/message_view.h>

using namespace boost::ut;
using namespace boost::ut::literals;
namespace compact = kimix::builtin_tools::compact;
using kimix::runtime::soul::compaction_scratch;
using kimix::runtime::soul::message_view;
using kimix::runtime::soul::part_kind;
using kimix::runtime::soul::part_view;
using kimix::runtime::soul::tool_call_view;

namespace tests_unit_native_test_soul_compaction {

// A history that exercises every branch: user/assistant/tool pairing, a think
// part, and a streamed tool_call part marker.
struct fixture {
    kimix::vector<kimix::vector<part_view>> parts;
    kimix::vector<kimix::vector<tool_call_view>> calls;
    kimix::vector<message_view> msgs;
    kimix::vector<kimix::string> tcid;
    // Owns every generated text: a part_view must never point at a temporary.
    kimix::vector<kimix::string> texts;

    explicit fixture(uint32_t count) {
        parts.resize(count);
        calls.resize(count);
        tcid.resize(count);
        texts.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t slot = i % 6;
            if (slot == 0 || slot == 4) {
                texts[i] = kimix::format("user question {}", i);
                parts[i].push_back(part_view{part_kind::TEXT, kimix::string_view(texts[i])});
            } else if (slot == 1) {
                parts[i].push_back(part_view{part_kind::THINK, "thinking"});
                parts[i].push_back(part_view{part_kind::TEXT, "answer"});
                calls[i].push_back(tool_call_view{"call_1", "read", "{}"});
            } else if (slot == 2) {
                parts[i].push_back(part_view{part_kind::TEXT, "tool output"});
                tcid[i] = "call_1";
            } else {
                parts[i].push_back(part_view{part_kind::TEXT, "more text"});
            }
        }
        msgs.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t role = kimix::runtime::soul::kRoleUser;
            const uint32_t slot = i % 6;
            if (slot == 1) {
                role = kimix::runtime::soul::kRoleAssistant;
            } else if (slot == 2) {
                role = kimix::runtime::soul::kRoleTool;
            }
            msgs[i].role = role;
            msgs[i].parts = kimix::span<const part_view>(parts[i].data(), parts[i].size());
            msgs[i].tool_calls =
                kimix::span<const tool_call_view>(calls[i].data(), calls[i].size());
            if (!tcid[i].empty()) {
                msgs[i].tool_call_id = kimix::string_view(tcid[i]);
            }
        }
    }
};

} // namespace tests_unit_native_test_soul_compaction
using namespace tests_unit_native_test_soul_compaction;

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "compaction_projection_is_lossless"_test = [] {
        fixture f(12);
        compaction_scratch scratch;
        kimix::runtime::soul::project(f.msgs, scratch);
        expect(scratch.msgs.size() == f.msgs.size());
        for (size_t i = 0; i < f.msgs.size(); ++i) {
            const char* expected_role =
                f.msgs[i].role == kimix::runtime::soul::kRoleAssistant
                    ? "assistant"
                    : (f.msgs[i].role == kimix::runtime::soul::kRoleTool
                           ? "tool"
                           : (f.msgs[i].role == kimix::runtime::soul::kRoleSystem
                                  ? "system"
                                  : "user"));
            expect(scratch.msgs[i].role == kimix::string_view(expected_role));
            expect(scratch.msgs[i].content.size() == f.msgs[i].parts.size());
            expect(scratch.msgs[i].tool_call_count ==
                   static_cast<int32_t>(f.msgs[i].tool_calls.size()));
        }
        // Reusing the scratch must not accumulate.
        kimix::runtime::soul::project(f.msgs, scratch);
        expect(scratch.msgs.size() == f.msgs.size());
    };

    "compaction_cuts_match_kernel"_test = [] {
        fixture f(12);
        compaction_scratch scratch;
        kimix::runtime::soul::project(f.msgs, scratch);
        const auto direct = compact::balanced_cut_indices(scratch.msgs);

        kimix::vector<uint32_t> cuts;
        bool unbalanced = false;
        int64_t unbalanced_index = -1;
        kimix::runtime::soul::balanced_cut_indices(f.msgs, cuts, unbalanced,
                                                   unbalanced_index);
        expect(cuts.size() == direct.cuts.size());
        for (size_t i = 0; i < cuts.size(); ++i) {
            expect(cuts[i] == static_cast<uint32_t>(direct.cuts[i]));
        }
        expect(unbalanced == direct.unbalanced);
        expect(unbalanced_index ==
               (direct.unbalanced ? static_cast<int64_t>(direct.unbalanced_index) : -1));
        expect(!cuts.empty());
        expect(cuts.front() == 0u);
        expect(cuts.back() == 12u);
    };

    "compaction_nearest_cut"_test = [] {
        fixture f(12);
        compaction_scratch scratch;
        kimix::runtime::soul::project(f.msgs, scratch);
        for (uint32_t index : {0u, 1u, 5u, 11u, 12u}) {
            bool flag = false;
            const uint32_t from_arena =
                kimix::runtime::soul::nearest_balanced_cut_before(f.msgs, index, flag);
            bool direct_flag = false;
            const size_t direct = compact::nearest_balanced_cut_before(
                scratch.msgs, static_cast<int64_t>(index), &direct_flag);
            expect(from_arena == static_cast<uint32_t>(direct));
            expect(flag == direct_flag);
        }
    };

    "compaction_preserve_split"_test = [] {
        fixture f(12);
        compaction_scratch scratch;
        kimix::runtime::soul::project(f.msgs, scratch);
        for (int32_t depth = 1; depth <= 4; ++depth) {
            compact::preserve_split from_arena;
            kimix::runtime::soul::resolve_preserve_split(f.msgs, depth, true,
                                                         from_arena);
            const compact::preserve_split direct =
                compact::resolve_preserve_split(scratch.msgs, depth, true);
            expect(from_arena.compact == direct.compact);
            expect(from_arena.preserve_start_index == direct.preserve_start_index);
            expect(from_arena.keep_first_message == direct.keep_first_message);
            expect(from_arena.unbalanced == direct.unbalanced);
            expect(from_arena.recut_fallback == direct.recut_fallback);
        }
    };

    "compaction_prompt_matches_kernel"_test = [] {
        fixture f(12);
        compact::compaction_options opts;
        opts.mode = compact::CompactMode::balanced;

        compact::compaction_prompt_output from_arena;
        expect(kimix::runtime::soul::build_compaction_prompt(
            f.msgs, opts, "custom instruction", "PROMPT", "CASCADE", from_arena));

        compaction_scratch scratch;
        kimix::runtime::soul::project(f.msgs, scratch);
        compact::compaction_prompt_input in;
        in.to_compact = scratch.msgs;
        in.options = opts;
        in.custom_instruction = "custom instruction";
        in.prompt_compact = "PROMPT";
        in.prompt_compact_cascade = "CASCADE";
        compact::compaction_prompt_output direct;
        expect(!compact::build_compaction_prompt(in, direct).failed());
        expect(from_arena.prompt_text == direct.prompt_text);
        expect(from_arena.cascade_depth == direct.cascade_depth);
    };

    "compaction_prepare_and_text"_test = [] {
        fixture f(12);
        compact::compaction_options opts;
        compact::prepare_result prepared;
        expect(kimix::runtime::soul::prepare_compaction_input(
            f.msgs, 6, opts, "", "PROMPT", "CASCADE", prepared));
        expect(prepared.to_compact.size() == 6u);
        expect(prepared.to_preserve.size() == 6u);
        expect(!prepared.compact_message_text.empty());
        expect(!prepared.prompt_text.empty());

        // Nothing to compact: the kernel reports failure.
        compact::prepare_result none;
        expect(!kimix::runtime::soul::prepare_compaction_input(
            f.msgs, 0, opts, "", "PROMPT", "CASCADE", none));

        // The legacy text is the compacted messages followed by the ASSEMBLED
        // prompt, which the kernel returns next to it.
        kimix::string legacy;
        kimix::runtime::soul::build_compact_message_text(
            kimix::span<const message_view>(f.msgs.data(), 6), prepared.prompt_text,
            legacy);
        expect(legacy == prepared.compact_message_text);
    };

    "compaction_text_and_tokens"_test = [] {
        fixture f(12);
        kimix::string joined;
        kimix::runtime::soul::extract_text(f.msgs, "\n", joined);
        expect(!joined.empty());
        expect(joined.find("answer") != kimix::string::npos);
        expect(joined.find("tool output") != kimix::string::npos);
        // The join is newline-separated between consecutive text parts.
        expect(joined.find("\n") != kimix::string::npos);

        const int64_t tokens = kimix::runtime::soul::count_tokens(f.msgs);
        expect(tokens > 0);
        compaction_scratch scratch;
        kimix::runtime::soul::project(f.msgs, scratch);
        expect(tokens == compact::estimate_message_tokens(scratch.msgs));

        // A message with no text part contributes no separator.
        kimix::vector<part_view> think_only;
        think_only.push_back(part_view{part_kind::THINK, "only thinking"});
        message_view mv;
        mv.role = kimix::runtime::soul::kRoleAssistant;
        mv.parts = kimix::span<const part_view>(think_only.data(), think_only.size());
        kimix::string out;
        kimix::runtime::soul::extract_text(kimix::span<const message_view>(&mv, 1), "\n",
                                          out);
        expect(out.empty());
        expect(kimix::runtime::soul::count_tokens(
                   kimix::span<const message_view>(&mv, 1)) == 0);
    };

    "compaction_trigger"_test = [] {
        compact::compaction_trigger_config cfg;
        cfg.max_context_size = 1000;
        cfg.trigger_ratio = 0.75;
        cfg.safety_margin_tokens = 100;
        expect(!kimix::runtime::soul::should_auto_compact(100, cfg));
        expect(kimix::runtime::soul::should_auto_compact(900, cfg));
    };

    "bench_compaction_750"_test = [] {
        fixture f(750);
        compaction_scratch scratch;
        kimix::vector<uint32_t> cuts;
        bool unbalanced = false;
        int64_t index = -1;
        for (int i = 0; i < 20; ++i) {
            kimix::runtime::soul::project(f.msgs, scratch);
            kimix::runtime::soul::balanced_cut_indices(f.msgs, cuts, unbalanced, index);
        }
        expect(!cuts.empty());
        kimix::string prompt;
        compact::compaction_prompt_output out;
        compact::compaction_options opts;
        expect(kimix::runtime::soul::build_compaction_prompt(f.msgs, opts, "", "P",
                                                             "C", out));
        expect(!out.prompt_text.empty());
    };

    return 0;
}
