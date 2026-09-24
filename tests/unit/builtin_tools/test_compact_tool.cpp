// Test for the compact context-compaction kernels (builtin_tools/compact_tool.h).
//
// Covers the golden vectors from plans/compact.md §6.

#include "ut/ut.hpp"

#include "builtin_tools/compact_tool.h"
#include "builtin_tools/tool_types.h"

#include <cstdint>
#include <cstdint>
#include <cstdlib>
using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools;
using namespace kimix::builtin_tools::compact;

namespace {

message make_text_message(kimix::string_view role, kimix::string_view text) {
    message msg;
    msg.role = role;
    msg.content.push_back(content_part{"text", kimix::string(text)});
    return msg;
}

message make_think_message(kimix::string_view role, kimix::string_view text) {
    message msg;
    msg.role = role;
    msg.content.push_back(content_part{"think", kimix::string(text)});
    return msg;
}

// Repeat a UTF-8 unit n times (for the kimi-cli/tests/utils/test_tokens.py vectors).
kimix::string repeat_unit(kimix::string_view unit, size_t n) {
    kimix::string out;
    out.reserve(unit.size() * n);
    for (size_t i = 0; i < n; ++i) {
        out.append(unit);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Tool pairing + preserve boundary
//
// compact_tool.cpp ports kimi_cli/soul/tool_pairing.py (message_tool_call_delta /
// balanced_cut_indices / nearest_balanced_cut_before) and the boundary math of
// SimpleCompaction.prepare (compaction.py:711-772, including the Phase-6 primacy
// re-insertion and its re-cut).  The vectors below were produced by running the
// real reference implementation; regenerate with
// `python scripts/gen_tool_pairing_goldens.py`.
// ---------------------------------------------------------------------------
#include "tool_pairing_goldens.inc"

namespace {

// '|'-separated column -> tokens (an empty column yields no token).
kimix::vector<kimix::string> tp_split(kimix::string_view s) {
    kimix::vector<kimix::string> out;
    if (s.empty()) {
        return out;
    }
    size_t start = 0;
    while (true) {
        const size_t bar = s.find('|', start);
        if (bar == kimix::string_view::npos) {
            out.push_back(kimix::string(s.substr(start)));
            return out;
        }
        out.push_back(kimix::string(s.substr(start, bar - start)));
        start = bar + 1;
    }
}

int32_t tp_token_int(const kimix::vector<kimix::string> &cols, size_t i) {
    if (i >= cols.size() || cols[i].empty()) {
        return 0;
    }
    return static_cast<int32_t>(std::strtol(cols[i].c_str(), nullptr, 10));
}

// Rebuild the C++ history for one golden row. Content order mirrors the
// generator's kosong Message: a think part first (when thinks == 1), then the
// text part (when non-empty).
kimix::vector<message> tp_messages(const tp_history_case &row) {
    const kimix::vector<kimix::string> roles = tp_split(row.roles);
    const kimix::vector<kimix::string> calls = tp_split(row.calls);
    const kimix::vector<kimix::string> texts = tp_split(row.texts);
    const kimix::vector<kimix::string> thinks = tp_split(row.thinks);
    const kimix::vector<kimix::string> think_texts = tp_split(row.think_texts);
    kimix::vector<message> out;
    out.reserve(static_cast<size_t>(row.count));
    for (size_t i = 0; i < static_cast<size_t>(row.count); ++i) {
        message m;
        m.role = i < roles.size() ? roles[i] : kimix::string();
        if (i < thinks.size() && thinks[i] == "1") {
            m.content.push_back(
                content_part{"think", i < think_texts.size() ? think_texts[i] : kimix::string()});
        }
        if (i < texts.size() && !texts[i].empty()) {
            m.content.push_back(content_part{"text", texts[i]});
        }
        // `calls` holds the reference's message_tool_call_delta: +N for an
        // assistant tool call, -1 for a tool result, 0 otherwise. Only the
        // assistant count maps onto message::tool_call_count.
        const int32_t delta = tp_token_int(calls, i);
        m.tool_call_count = delta > 0 ? delta : 0;
        out.push_back(std::move(m));
    }
    return out;
}

const tp_history_case *tp_history_named(kimix::string_view name) {
    for (size_t i = 0; i < kToolPairingHistoryCount; ++i) {
        if (name == kToolPairingHistories[i].name) {
            return &kToolPairingHistories[i];
        }
    }
    return nullptr;
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    // ── should_auto_compact ─────────────────────────────────────────────────────

    "should_auto_compact_ratio_boundary"_test = [] {
        compaction_trigger_config cfg;
        cfg.max_context_size = 100000;
        cfg.reserved_context_size = 8192;
        cfg.safety_margin_tokens = 1024;
        cfg.trigger_ratio = 0.75;

        expect(!should_auto_compact(69999, cfg));
        expect(should_auto_compact(75000, cfg));
    };

    "should_auto_compact_reserved_boundary"_test = [] {
        compaction_trigger_config cfg;
        cfg.max_context_size = 100000;
        cfg.reserved_context_size = 8192;
        cfg.safety_margin_tokens = 1024;
        cfg.trigger_ratio = 0.95;

        const int64_t output_size = cfg.max_tokens + cfg.safety_margin_tokens; // 1024
        const int64_t reserved = (cfg.tool_call_buffer_tokens > cfg.reserved_context_size)
                                     ? cfg.tool_call_buffer_tokens
                                     : cfg.reserved_context_size;
        const int64_t effective_reserved =
            (reserved < cfg.max_context_size - cfg.reserved_context_size)
                ? reserved
                : cfg.max_context_size - cfg.reserved_context_size; // 8192
        const int64_t threshold = cfg.max_context_size - effective_reserved; // 91808

        expect(!should_auto_compact(threshold - 1, cfg));
        expect(should_auto_compact(threshold, cfg));
    };

    "should_auto_compact_max_tokens_dominates"_test = [] {
        compaction_trigger_config cfg;
        cfg.max_context_size = 100000;
        cfg.reserved_context_size = 8192;
        cfg.max_tokens = 50000;
        cfg.safety_margin_tokens = 1024;
        cfg.trigger_ratio = 0.75;

        const int64_t output_size = cfg.max_tokens + cfg.safety_margin_tokens; // 51024
        const int64_t min_input_room = cfg.max_context_size - cfg.reserved_context_size; // 91808
        const int64_t effective_reserved = (output_size < min_input_room) ? output_size : min_input_room;

        expect(!should_auto_compact(cfg.max_context_size - effective_reserved - 1, cfg));
        expect(should_auto_compact(cfg.max_context_size - effective_reserved, cfg));
    };

    "should_auto_compact_tool_call_buffer_dominates"_test = [] {
        compaction_trigger_config cfg;
        cfg.max_context_size = 100000;
        cfg.reserved_context_size = 8192;
        cfg.tool_call_buffer_tokens = 20000;
        cfg.safety_margin_tokens = 1024;
        cfg.trigger_ratio = 0.95;

        const int64_t min_input_room = cfg.max_context_size - cfg.reserved_context_size; // 91808
        const int64_t effective_reserved =
            (cfg.tool_call_buffer_tokens < min_input_room) ? cfg.tool_call_buffer_tokens : min_input_room;

        expect(!should_auto_compact(cfg.max_context_size - effective_reserved - 1, cfg));
        expect(should_auto_compact(cfg.max_context_size - effective_reserved, cfg));
    };

    // The reference has no `max_context_size <= 0` guard: with a zero/negative
    // window the ratio comparison `token_count >= max_context_size * ratio`
    // fires for every non-negative count.  Expected values below were produced
    // by kimi_cli.soul.compaction.should_auto_compact itself (see
    // python/tests/test_parity_compact.py, test_should_auto_compact_zero_or_
    // negative_window).
    "should_auto_compact_zero_or_negative_max_context"_test = [] {
        compaction_trigger_config cfg;
        cfg.max_context_size = 0;
        expect(should_auto_compact(1000, cfg));
        expect(should_auto_compact(0, cfg));
        expect(!should_auto_compact(-5, cfg));

        cfg.max_context_size = -1;
        expect(should_auto_compact(1000, cfg));
        expect(!should_auto_compact(-5, cfg));
    };

    // ── adaptive_preserve_depth ───────────────────────────────────────────────

    "adaptive_preserve_depth_empty"_test = [] {
        kimix::vector<message> messages;
        expect(eq(adaptive_preserve_depth(messages, 1, 10), 1));
    };

    "adaptive_preserve_depth_error_signal"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_text_message("user", "I see an error here"));
        expect(eq(adaptive_preserve_depth(messages, 1, 10), 2));
    };

    "adaptive_preserve_depth_exception_signal"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_text_message("assistant", "This is an exception"));
        expect(eq(adaptive_preserve_depth(messages, 1, 10), 2));
    };

    "adaptive_preserve_depth_failed_signal"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_text_message("user", "The build failed"));
        expect(eq(adaptive_preserve_depth(messages, 1, 10), 2));
    };

    "adaptive_preserve_depth_think_signal"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_think_message("assistant", "reasoning"));
        expect(eq(adaptive_preserve_depth(messages, 1, 10), 2));
    };

    "adaptive_preserve_depth_file_refs"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_text_message(
            "user", "file:a.py file:b.md file:c.py"));
        // file_refs (>2) = +1
        expect(eq(adaptive_preserve_depth(messages, 1, 10), 2));
    };

    "adaptive_preserve_depth_clamp"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_text_message(
            "user", "file:a.py file:b.md file:c.py ERROR"));
        messages.back().content.push_back(content_part{"think", kimix::string("reason")});
        // min(1) + error + think + file_refs = 4
        expect(eq(adaptive_preserve_depth(messages, 1, 3), 3));
    };

    "adaptive_preserve_depth_only_system"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_text_message("system", "instructions"));
        expect(eq(adaptive_preserve_depth(messages, 1, 10), 1));
    };

    // The reference early-returns `min_preserved` *unclamped* when the history is
    // empty or contains no user/assistant turn (compaction.py:297-309); only the
    // signalling path clamps to [min_preserved, max_preserved].  Expected values
    // come from kimi_cli.soul.compaction.adaptive_preserve_depth.
    "adaptive_preserve_depth_empty_min_above_max"_test = [] {
        kimix::vector<message> messages;
        expect(eq(adaptive_preserve_depth(messages, 5, 3), 5));
        expect(eq(adaptive_preserve_depth(messages, 0, 0), 0));
    };

    "adaptive_preserve_depth_no_user_or_assistant_min_above_max"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_text_message("system", "error here"));
        expect(eq(adaptive_preserve_depth(messages, 5, 3), 5));

        kimix::vector<message> tools;
        tools.push_back(make_text_message("tool", "failed"));
        expect(eq(adaptive_preserve_depth(tools, 5, 3), 5));
    };

    "adaptive_preserve_depth_signal_path_clamps"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_text_message("user", "failed"));
        // error boost then clamp: min(max(2, 5), 3) == 3
        expect(eq(adaptive_preserve_depth(messages, 5, 3), 3));
        expect(eq(adaptive_preserve_depth(messages, 0, 0), 0));
    };

    "adaptive_preserve_depth_reference_corpus"_test = [] {
        // kimi-cli/tests/core/test_adaptive_preserve.py
        kimix::vector<message> plain;
        plain.push_back(make_text_message("user", "Hello"));
        plain.push_back(make_text_message("assistant", "Hi there"));
        expect(eq(adaptive_preserve_depth(plain, 1, 5), 1));

        kimix::vector<message> error_in_build;
        error_in_build.push_back(make_text_message("user", "There was an error in the build"));
        expect(eq(adaptive_preserve_depth(error_in_build, 1, 5), 2));

        kimix::vector<message> file_edits;
        file_edits.push_back(
            make_text_message("assistant", "Edited file: foo.py and bar.md and baz.py"));
        expect(eq(adaptive_preserve_depth(file_edits, 1, 5), 2));

        // Only the most recent user/assistant turn is inspected.
        kimix::vector<message> last_turn_clean;
        last_turn_clean.push_back(make_text_message("user", "error here"));
        last_turn_clean.push_back(make_text_message("assistant", "all good"));
        expect(eq(adaptive_preserve_depth(last_turn_clean, 1, 5), 1));

        // system / tool roles are skipped when looking for the last turn.
        kimix::vector<message> skips_roles;
        skips_roles.push_back(make_text_message("system", "error here"));
        skips_roles.push_back(make_text_message("user", "all good"));
        expect(eq(adaptive_preserve_depth(skips_roles, 1, 5), 1));

        // min floor
        kimix::vector<message> floor_msgs;
        floor_msgs.push_back(make_text_message("user", "plain chat"));
        expect(eq(adaptive_preserve_depth(floor_msgs, 2, 5), 2));

        // combined signals + max cap
        kimix::vector<message> capped;
        capped.push_back(make_text_message(
            "assistant", "error exception failed file: a.py b.py c.py d.py"));
        capped.back().content.push_back(content_part{"think", kimix::string("think...")});
        expect(eq(adaptive_preserve_depth(capped, 1, 3), 3));
        expect(eq(adaptive_preserve_depth(capped, 1, 5), 4));
    };

    // ── detect_cascade_depth ────────────────────────────────────────────────────

    "detect_cascade_depth_empty"_test = [] {
        kimix::vector<message> messages;
        expect(eq(detect_cascade_depth(messages), 0));
    };

    "detect_cascade_depth_one_marker"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_text_message(
            "assistant", "Previous context has been compacted"));
        expect(eq(detect_cascade_depth(messages), 1));
    };

    "detect_cascade_depth_two_messages"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_text_message(
            "assistant", "Previous context has been compacted"));
        messages.push_back(make_text_message(
            "user", "Previous context has been compacted again"));
        expect(eq(detect_cascade_depth(messages), 2));
    };

    "detect_cascade_depth_two_parts_count_once"_test = [] {
        kimix::vector<message> messages;
        message msg;
        msg.role = "assistant";
        msg.content.push_back(content_part{"text", kimix::string("Previous context has been compacted")});
        msg.content.push_back(content_part{"text", kimix::string("Previous context has been compacted")});
        messages.push_back(std::move(msg));
        expect(eq(detect_cascade_depth(messages), 1));
    };

    "detect_cascade_depth_no_marker"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_text_message("user", "hello"));
        expect(eq(detect_cascade_depth(messages), 0));
    };

    // The reference counts only TextPart occurrences (compaction.py:224-232); a
    // think part carrying the marker is not a compaction summary.
    "detect_cascade_depth_ignores_think_parts"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_think_message(
            "assistant", "Previous context has been compacted"));
        expect(eq(detect_cascade_depth(messages), 0));
        messages.push_back(make_text_message(
            "user", "Previous context has been compacted"));
        expect(eq(detect_cascade_depth(messages), 1));
    };

    // ── parse_compact_mode and mode_guidance ────────────────────────────────────

    "parse_compact_mode_valid"_test = [] {
        expect(parse_compact_mode("balanced") == CompactMode::balanced);
        expect(parse_compact_mode("aggressive") == CompactMode::aggressive);
        expect(parse_compact_mode("retentive") == CompactMode::retentive);
        expect(parse_compact_mode("technical") == CompactMode::technical);
    };

    "parse_compact_mode_unknown_defaults_retentive"_test = [] {
        expect(parse_compact_mode("unknown") == CompactMode::retentive);
    };

    "mode_guidance_non_empty_for_valid"_test = [] {
        expect(!mode_guidance(CompactMode::balanced).empty());
        expect(!mode_guidance(CompactMode::aggressive).empty());
        expect(!mode_guidance(CompactMode::retentive).empty());
        expect(!mode_guidance(CompactMode::technical).empty());
    };

    "mode_guidance_balanced_text"_test = [] {
        expect(eq(kimix::string(mode_guidance(CompactMode::balanced)),
                  kimix::string("**Compaction Style Guidance:** Be balanced. Preserve essential context "
                                "while condensing redundant information. Keep current task state, errors "
                                "and solutions, code state, design decisions, and TODO items.")));
    };

    // ── build_compaction_prompt ─────────────────────────────────────────────────

    "build_compaction_prompt_avoid_cascade"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_text_message(
            "assistant", "Previous context has been compacted"));
        messages.push_back(make_text_message(
            "assistant", "Previous context has been compacted"));
        messages.push_back(make_text_message(
            "assistant", "Previous context has been compacted"));

        compaction_prompt_input in;
        in.to_compact = messages;
        in.options.avoid_cascade = true;
        in.prompt_compact = "BASE";
        in.prompt_compact_cascade = "CASCADE";

        compaction_prompt_output out;
        tool_error err = build_compaction_prompt(in, out);
        expect(!err.failed());
        expect(out.prompt_text.find("BASE") != kimix::string::npos);
        expect(out.prompt_text.find("CASCADE") == kimix::string::npos);
    };

    "build_compaction_prompt_cascade_threshold"_test = [] {
        kimix::vector<message> messages;
        for (int i = 0; i < 3; ++i) {
            messages.push_back(make_text_message(
                "assistant", "Previous context has been compacted"));
        }

        compaction_prompt_input in;
        in.to_compact = messages;
        in.options.avoid_cascade = false;
        in.prompt_compact = "BASE";
        in.prompt_compact_cascade = "CASCADE";

        compaction_prompt_output out;
        tool_error err = build_compaction_prompt(in, out);
        expect(!err.failed());
        expect(out.prompt_text.find("CASCADE") != kimix::string::npos);
        expect(out.cascade_depth == 3);
    };

    "build_compaction_prompt_cascade_below_threshold"_test = [] {
        kimix::vector<message> messages;
        for (int i = 0; i < 2; ++i) {
            messages.push_back(make_text_message(
                "assistant", "Previous context has been compacted"));
        }

        compaction_prompt_input in;
        in.to_compact = messages;
        in.options.avoid_cascade = false;
        in.prompt_compact = "BASE";
        in.prompt_compact_cascade = "CASCADE";

        compaction_prompt_output out;
        tool_error err = build_compaction_prompt(in, out);
        expect(!err.failed());
        expect(out.prompt_text.find("BASE") != kimix::string::npos);
        expect(out.prompt_text.find("CASCADE") == kimix::string::npos);
    };

    "build_compaction_prompt_mode_guidance"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_text_message("user", "hi"));

        compaction_prompt_input in;
        in.to_compact = messages;
        in.options.mode = CompactMode::technical;
        in.prompt_compact = "BASE";
        in.prompt_compact_cascade = "CASCADE";

        compaction_prompt_output out;
        tool_error err = build_compaction_prompt(in, out);
        expect(!err.failed());
        expect(out.prompt_text.find("Focus on technical specifics") !=
               kimix::string::npos);
    };

    "build_compaction_prompt_decision_section"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_text_message("user", "hi"));

        compaction_prompt_input in;
        in.to_compact = messages;
        in.options.decision_section_enabled = true;
        in.prompt_compact = "BASE";
        in.prompt_compact_cascade = "CASCADE";

        compaction_prompt_output out;
        tool_error err = build_compaction_prompt(in, out);
        expect(!err.failed());
        expect(out.prompt_text.find("## Decisions & Conclusions") !=
               kimix::string::npos);
        expect(out.prompt_text.find("## Verification Status") !=
               kimix::string::npos);
    };

    "build_compaction_prompt_custom_instruction"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_text_message("user", "hi"));

        compaction_prompt_input in;
        in.to_compact = messages;
        in.custom_instruction = "Keep file paths.";
        in.prompt_compact = "BASE";
        in.prompt_compact_cascade = "CASCADE";

        compaction_prompt_output out;
        tool_error err = build_compaction_prompt(in, out);
        expect(!err.failed());
        expect(out.prompt_text.find("**User's Custom Compaction Instruction:**") !=
               kimix::string::npos);
        expect(out.prompt_text.find("Keep file paths.") != kimix::string::npos);
    };

    "build_compaction_prompt_no_custom_when_empty"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_text_message("user", "hi"));

        compaction_prompt_input in;
        in.to_compact = messages;
        in.custom_instruction = "";
        in.prompt_compact = "BASE";
        in.prompt_compact_cascade = "CASCADE";

        compaction_prompt_output out;
        tool_error err = build_compaction_prompt(in, out);
        expect(!err.failed());
        expect(out.prompt_text.find("User's Custom Compaction Instruction") ==
               kimix::string::npos);
    };

    // ── build_compact_message_text ──────────────────────────────────────────────

    "build_compact_message_text_empty"_test = [] {
        compact_message_request req;
        req.prompt_text = "\nBASE";
        kimix::string text = build_compact_message_text(req);
        expect(eq(text, kimix::string("\nBASE")));
    };

    "build_compact_message_text_one_message"_test = [] {
        compact_message_request req;
        kimix::vector<message> msgs;
        msgs.push_back(make_text_message("user", "hello"));
        req.to_compact = msgs;
        req.prompt_text = "\nBASE";
        kimix::string text = build_compact_message_text(req);
        expect(eq(text, kimix::string("## Message 1\nRole: user\nContent:\nhello\nBASE")));
    };

    "build_compact_message_text_two_messages"_test = [] {
        compact_message_request req;
        kimix::vector<message> msgs;
        msgs.push_back(make_text_message("user", "hello"));
        msgs.push_back(make_text_message("assistant", "world"));
        req.to_compact = msgs;
        req.prompt_text = "\nBASE";
        kimix::string text = build_compact_message_text(req);
        expect(eq(text, kimix::string("## Message 1\nRole: user\nContent:\nhello"
                                      "## Message 2\nRole: assistant\nContent:\nworld\nBASE")));
    };

    "build_compact_message_text_ignores_non_text"_test = [] {
        compact_message_request req;
        kimix::vector<message> msgs;
        message msg;
        msg.role = "user";
        msg.content.push_back(content_part{"text", kimix::string("visible")});
        msg.content.push_back(content_part{"think", kimix::string("hidden")});
        msg.content.push_back(content_part{"other", kimix::string("ignored")});
        msgs.push_back(std::move(msg));
        req.to_compact = msgs;
        req.prompt_text = "\nBASE";
        kimix::string text = build_compact_message_text(req);
        expect(eq(text, kimix::string("## Message 1\nRole: user\nContent:\nvisible\nBASE")));
    };

    // ── prepare_compaction_input ──────────────────────────────────────────────────

    "prepare_compaction_input_zero_index"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_text_message("user", "hello"));

        prepare_request req;
        req.messages = messages;
        req.preserve_start_index = 0;
        req.prompt_compact = "BASE";
        req.prompt_compact_cascade = "CASCADE";

        prepare_result out;
        tool_error err = prepare_compaction_input(req, out);
        expect(err.status == tool_status::no_change);
        expect(out.to_compact.empty());
        expect(eq(out.to_preserve.size(), size_t(1)));
    };

    "prepare_compaction_input_full_compact"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_text_message("user", "hello"));
        messages.push_back(make_text_message("assistant", "world"));

        prepare_request req;
        req.messages = messages;
        req.preserve_start_index = 2;
        req.prompt_compact = "BASE";
        req.prompt_compact_cascade = "CASCADE";

        prepare_result out;
        tool_error err = prepare_compaction_input(req, out);
        expect(!err.failed());
        expect(eq(out.to_compact.size(), size_t(2)));
        expect(out.to_preserve.empty());
        expect(!out.compact_message_text.empty());
        expect(!out.prompt_text.empty());
    };

    "prepare_compaction_input_split"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_text_message("user", "old"));
        messages.push_back(make_text_message("assistant", "reply"));
        messages.push_back(make_text_message("user", "new"));

        prepare_request req;
        req.messages = messages;
        req.preserve_start_index = 2;
        req.prompt_compact = "BASE";
        req.prompt_compact_cascade = "CASCADE";

        prepare_result out;
        tool_error err = prepare_compaction_input(req, out);
        expect(!err.failed());
        expect(eq(out.to_compact.size(), size_t(2)));
        expect(eq(out.to_preserve.size(), size_t(1)));
        expect(eq(out.to_preserve[0].role, kimix::string("user")));
    };

    // ── compute_surface_fingerprint ─────────────────────────────────────────────

    "surface_fingerprint_empty"_test = [] {
        kimix::vector<message> messages;
        surface_fingerprint fp = compute_surface_fingerprint(messages);
        expect(eq(fp.history_len, uint32_t(0)));
        expect(eq(fp.token_count, int64_t(0)));
        expect(!fp.last_message_text.has_value());
    };

    "surface_fingerprint_custom_counter"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_text_message("user", "hello"));
        auto counter = [](kimix::span<const message>) -> int64_t { return 42; };
        surface_fingerprint fp = compute_surface_fingerprint(messages, counter);
        expect(eq(fp.history_len, uint32_t(1)));
        expect(eq(fp.token_count, int64_t(42)));
        expect(fp.last_message_text.has_value());
        expect(eq(fp.last_message_text.value(), kimix::string("hello")));
    };

    "surface_fingerprint_joins_with_space"_test = [] {
        kimix::vector<message> messages;
        message msg;
        msg.role = "assistant";
        msg.content.push_back(content_part{"text", kimix::string("hello")});
        msg.content.push_back(content_part{"text", kimix::string("world")});
        messages.push_back(std::move(msg));

        surface_fingerprint fp = compute_surface_fingerprint(messages);
        expect(fp.last_message_text.has_value());
        expect(eq(fp.last_message_text.value(), kimix::string("hello world")));
    };

    // The reference mirrors None only for an empty history; a non-empty history
    // whose last message carries no TextPart yields "" (compaction.py:183-190).
    "surface_fingerprint_empty_last_text_is_empty_string"_test = [] {
        kimix::vector<message> messages;
        messages.push_back(make_think_message("user", "reasoning only"));
        surface_fingerprint fp = compute_surface_fingerprint(messages);
        expect(fp.last_message_text.has_value());
        expect(fp.last_message_text.value().empty());

        kimix::vector<message> messages2;
        messages2.push_back(make_text_message("user", "a"));
        message empty_tail;
        empty_tail.role = "assistant";
        empty_tail.content.push_back(content_part{"other", kimix::string("ignored")});
        messages2.push_back(std::move(empty_tail));
        surface_fingerprint fp2 = compute_surface_fingerprint(messages2);
        expect(fp2.last_message_text.has_value());
        expect(fp2.last_message_text.value().empty());
        expect(eq(fp2.token_count, int64_t(1)));
    };

    // ── estimate_text_tokens / estimate_message_tokens ──────────────────────────

    "estimate_text_tokens_empty"_test = [] {
        expect(eq(estimate_text_tokens(""), int64_t(0)));
    };

    "estimate_text_tokens_ascii"_test = [] {
        expect(eq(estimate_text_tokens("abcd"), int64_t(1)));
        expect(eq(estimate_text_tokens("abcdefgh"), int64_t(2)));
    };

    "estimate_text_tokens_cjk"_test = [] {
        // "\u4e00\u4e00\u4e00" is 3 CJK code points (>15% threshold)
        kimix::string s = "\xE4\xB8\x80\xE4\xB8\x80\xE4\xB8\x80";
        expect(eq(estimate_text_tokens(s), int64_t(1)));
    };

    "estimate_text_tokens_mixed"_test = [] {
        // 5 ASCII + 1 CJK = 6 code points, ascii_ratio = 5/6 = 0.833 (<0.95),
        // cjk_ratio = 1/6 = 0.166 (>0.15)
        kimix::string s = "hello\xE4\xB8\x80";
        expect(eq(estimate_text_tokens(s), int64_t(2))); // 6 // 3 = 2
    };

    "estimate_message_tokens_sums_text_parts"_test = [] {
        kimix::vector<message> messages;
        message msg;
        msg.role = "user";
        msg.content.push_back(content_part{"text", kimix::string("abcd")});
        msg.content.push_back(content_part{"think", kimix::string("ignored")});
        messages.push_back(std::move(msg));
        expect(eq(estimate_message_tokens(messages), int64_t(1)));
    };

    // Golden vector table generated from the pure-Python reference
    // kimi_cli/utils/tokens.py::_estimate_chars_tokens (fallback body, i.e. with
    // kimix_native's native gate off) -- see
    // python/tests/test_parity_compact.py, test_estimate_text_tokens_reference_corpus,
    // which regenerates the same numbers from the reference for every string here.
    "estimate_text_tokens_reference_goldens"_test = [] {
        const kimix::string cjk2 = "\xE4\xB8\xAD\xE6\x96\x87";                 // 2 CJK cps
        const kimix::string cjk4 = "\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95";
        const kimix::string kana3 = "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86";  // 3 kana cps
        const kimix::string fullwidth2 = "\xEF\xBC\xA1\xEF\xBC\xA2";           // ＡＢ

        expect(eq(estimate_text_tokens(""), int64_t(0)));
        expect(eq(estimate_text_tokens("a"), int64_t(1)));
        expect(eq(estimate_text_tokens("abc"), int64_t(1)));
        expect(eq(estimate_text_tokens("abcd"), int64_t(1)));
        expect(eq(estimate_text_tokens("abcde"), int64_t(1)));
        expect(eq(estimate_text_tokens("aaaaaaa"), int64_t(1)));
        expect(eq(estimate_text_tokens("aaaaaaaa"), int64_t(2)));
        expect(eq(estimate_text_tokens(kimix::string(100, 'a')), int64_t(25)));
        expect(eq(estimate_text_tokens(kimix::string(1000, 'a')), int64_t(250)));
        expect(eq(estimate_text_tokens("hello world, this is a test."), int64_t(7)));
        expect(eq(estimate_text_tokens("def f(x):\n    return x + 1\n"), int64_t(6)));
        expect(eq(estimate_text_tokens(cjk2), int64_t(1)));
        expect(eq(estimate_text_tokens(cjk4), int64_t(1)));
        expect(eq(estimate_text_tokens(kana3), int64_t(1)));
        expect(eq(estimate_text_tokens(fullwidth2), int64_t(1)));
        // 3 ASCII + 2 CJK: cjk_ratio 0.4 > 0.15 -> 5 // 3
        expect(eq(estimate_text_tokens(kimix::string("abc") + cjk2), int64_t(1)));
        // 7 ASCII + 1 CJK: ascii 0.875 <= 0.95, cjk 0.125 <= 0.15 -> int(8/3.5)
        expect(eq(estimate_text_tokens(kimix::string("abcdefg") + "\xE4\xB8\xAD"),
                  int64_t(2)));
        // 19 ASCII + 1 CJK: ascii_ratio == 0.95 is NOT > 0.95 -> int(20/3.5)
        expect(eq(estimate_text_tokens(kimix::string(19, 'a') + "\xE4\xB8\xAD"),
                  int64_t(5)));
        expect(eq(estimate_text_tokens(kimix::string(17, 'a') + cjk2), int64_t(5)));
        expect(eq(estimate_text_tokens(kimix::string("mix ") + cjk2 + " abcdef"),
                  int64_t(4)));

        // kimi-cli/tests/utils/test_tokens.py::TestEstimateCharsTokens
        expect(eq(estimate_text_tokens(kimix::string(400, 'a')), int64_t(100)));
        expect(eq(estimate_text_tokens(repeat_unit("\xE4\xBD\xA0", 300)), int64_t(100)));
        expect(eq(estimate_text_tokens("def foo():\n return '\xE4\xBD\xA0\xE5\xA5\xBD'"),
                  int64_t(6)));
        // ascii 96/100 == 0.96 > 0.95 -> 100 // 4
        expect(eq(estimate_text_tokens(kimix::string(96, 'a') +
                                       repeat_unit("\xE4\xBD\xA0", 4)),
                  int64_t(25)));
        // ascii 95/100 == 0.95 is not > 0.95, cjk 5/100 <= 0.15 -> int(100/3.5)
        expect(eq(estimate_text_tokens(kimix::string(95, 'a') +
                                       repeat_unit("\xE4\xBD\xA0", 5)),
                  int64_t(28)));
        expect(eq(estimate_text_tokens("Hello world \xE4\xB8\x96"), int64_t(3)));
        expect(eq(estimate_text_tokens("Hello\xE4\xB8\x96\xE7\x95\x8C"), int64_t(2)));
        expect(eq(estimate_text_tokens("\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C"),
                  int64_t(1)));
        // Hangul (U+AC00-U+D7AF) and kana are in the reference's _CJK_RE.
        expect(eq(estimate_text_tokens("\xEC\x95\x88\xEB\x85\x95\xED\x95\x98\xEC\x84\xB8\xEC\x9A\x94"),
                  int64_t(1)));
        expect(eq(estimate_text_tokens("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF"),
                  int64_t(1)));
        expect(eq(estimate_text_tokens("Hello world"), int64_t(2)));
    };

    // count_message_tokens sums one estimate per TextPart (never one estimate per
    // concatenated message), so two tiny parts cost more than their join.
    "estimate_message_tokens_per_part_not_concatenated"_test = [] {
        kimix::vector<message> split;
        message msg;
        msg.role = "user";
        msg.content.push_back(content_part{"text", kimix::string("a")});
        msg.content.push_back(content_part{"text", kimix::string("a")});
        split.push_back(std::move(msg));
        expect(eq(estimate_message_tokens(split), int64_t(2)));

        kimix::vector<message> joined;
        joined.push_back(make_text_message("user", "aa"));
        expect(eq(estimate_message_tokens(joined), int64_t(1)));
    };

    "estimate_message_tokens_ignores_think_and_other_parts"_test = [] {
        kimix::vector<message> messages;
        message msg;
        msg.role = "assistant";
        msg.content.push_back(content_part{"think", kimix::string("a thousand words")});
        msg.content.push_back(content_part{"other", kimix::string("also ignored")});
        messages.push_back(std::move(msg));
        expect(eq(estimate_message_tokens(messages), int64_t(0)));
    };

    // ── Compact Tool operator() smoke tests ─────────────────────────────────────

    "compact_tool_null_parameters"_test = [] {
        Compact compact(nullptr);
        compact(nullptr);
        // The operator() serializes a result; we verify it does not crash and
        // does not throw.  There is no return value to inspect here.
        expect(true);
    };

    "compact_tool_basic_call"_test = [] {
        using VE = kimix::builtin_tools::ValueElement;
        using TP = kimix::builtin_tools::ToolParams;

        kimix::shared_ptr<TP> params(new TP());

        VE::Array content;
        kimix::shared_ptr<TP> part(new TP());
        part->values["type"] = VE::make_string("text");
        part->values["text"] = VE::make_string("hello");
        content.push_back(VE::make_object(std::move(part)));

        kimix::shared_ptr<TP> msg(new TP());
        msg->values["role"] = VE::make_string("user");
        msg->values["content"] = VE::make_array(std::move(content));

        VE::Array messages;
        messages.push_back(VE::make_object(std::move(msg)));

        params->values["messages"] = VE::make_array(std::move(messages));
        params->values["preserve_start_index"] = VE::make_int(0);
        params->values["prompt_compact"] = VE::make_string("BASE");
        params->values["prompt_compact_cascade"] = VE::make_string("CASCADE");

        Compact compact(nullptr);
        compact(params.get());
        expect(true);
    };

    // ── Tool pairing / preserve boundary (reference-derived goldens) ────────────

    "tool_pairing_delta_and_cuts_match_reference_goldens"_test = [] {
        for (size_t ci = 0; ci < kToolPairingHistoryCount; ++ci) {
            const tp_history_case &row = kToolPairingHistories[ci];
            const kimix::vector<message> msgs = tp_messages(row);
            expect(eq(msgs.size(), static_cast<size_t>(row.count))) << row.name;
            const kimix::vector<kimix::string> calls = tp_split(row.calls);
            for (size_t i = 0; i < msgs.size(); ++i) {
                expect(eq(message_tool_call_delta(msgs[i]), tp_token_int(calls, i)))
                    << row.name;
            }
            const balanced_cuts_result folds = balanced_cut_indices(msgs);
            expect(eq(folds.unbalanced, row.unbalanced)) << row.name;
            if (row.unbalanced) {
                expect(eq(static_cast<int32_t>(folds.unbalanced_index),
                          row.unbalanced_index))
                    << row.name;
                continue;
            }
            const kimix::vector<kimix::string> want = tp_split(row.cuts);
            expect(eq(folds.cuts.size(), want.size())) << row.name;
            for (size_t i = 0; i < want.size() && i < folds.cuts.size(); ++i) {
                expect(eq(static_cast<int32_t>(folds.cuts[i]), tp_token_int(want, i)))
                    << row.name;
            }
        }
    };

    "tool_pairing_nearest_cut_matches_reference_goldens"_test = [] {
        for (size_t ci = 0; ci < kToolPairingNearestCount; ++ci) {
            const tp_nearest_case &row = kToolPairingNearest[ci];
            const tp_history_case *hist = tp_history_named(row.name);
            expect(hist != nullptr) << row.name;
            if (hist == nullptr) {
                continue;
            }
            const kimix::vector<message> msgs = tp_messages(*hist);
            bool unbalanced = false;
            const size_t got = nearest_balanced_cut_before(msgs, row.index, &unbalanced);
            expect(eq(unbalanced, row.unbalanced)) << row.name;
            if (!row.unbalanced) {
                expect(eq(static_cast<int32_t>(got), row.expect)) << row.name;
            }
        }
    };

    "tool_pairing_preserve_split_matches_reference_goldens"_test = [] {
        for (size_t ci = 0; ci < kToolPairingSplitCount; ++ci) {
            const tp_split_case &row = kToolPairingSplits[ci];
            const tp_history_case *hist = tp_history_named(row.name);
            expect(hist != nullptr) << row.name;
            if (hist == nullptr) {
                continue;
            }
            const kimix::vector<message> msgs = tp_messages(*hist);
            // The adaptive rows resolve the depth through the same kernel the
            // reference uses (LoopControl.min/max preserved messages = 1/2).
            const int32_t depth =
                row.adaptive ? adaptive_preserve_depth(msgs, 1, 2) : row.depth;
            const preserve_split got = resolve_preserve_split(msgs, depth, true);
            expect(eq(got.compact, row.compact)) << row.name;
            expect(eq(got.unbalanced, row.unbalanced)) << row.name;
            if (row.compact) {
                expect(eq(static_cast<int32_t>(got.preserve_start_index),
                          row.preserve_start))
                    << row.name;
                expect(eq(got.keep_first_message, row.keep_first)) << row.name;
            }
            expect(eq(got.recut_fallback, row.recut_fallback)) << row.name;
        }
    };

    "tool_pairing_toolcallpart_content_is_counted"_test = [] {
        // ToolCallPart branch of tool_pairing.message_tool_call_delta (lines
        // 21-23). kosong no longer lets a ToolCallPart live in Message.content, so
        // no Python-derived golden can cover it; this pins the port directly.
        message m;
        m.role = "assistant";
        m.content.push_back(content_part{"tool_call", kimix::string()});
        m.content.push_back(content_part{"tool_call", kimix::string()});
        expect(eq(message_tool_call_delta(m), 2));
        expect(is_tool_call_part(m.content[0]));
        expect(!is_tool_call_part(content_part{"text", kimix::string("x")}));

        // Persisted tool calls and streamed parts are summed, like the reference.
        m.tool_call_count = 3;
        expect(eq(message_tool_call_delta(m), 5));

        // A ToolCall *header* is never a content part: an assistant with tool
        // calls but no delta is still a call marker (role-only roles are inert).
        message tool_result;
        tool_result.role = "tool";
        expect(eq(message_tool_call_delta(tool_result), -1));
        message plain_user;
        plain_user.role = "user";
        expect(eq(message_tool_call_delta(plain_user), 0));
    };

    "tool_pairing_preserve_split_corner_cases"_test = [] {
        // Nothing to compact: fewer user/assistant messages than the depth.
        auto build = [](std::initializer_list<const char *> roles) {
            kimix::vector<message> msgs;
            for (const char *r : roles) {
                message m;
                m.role = r;
                m.content.push_back(content_part{"text", kimix::string(r)});
                msgs.push_back(std::move(m));
            }
            return msgs;
        };

        // Empty history / non-positive depth -> no compaction.
        const kimix::vector<message> none;
        expect(!resolve_preserve_split(none, 1, true).compact);
        expect(!resolve_preserve_split(build({"user", "assistant"}), 0, true).compact);
        // Only tool messages: the preserve walk finds nothing to preserve.
        expect(!resolve_preserve_split(build({"tool", "tool"}), 1, true).compact);
        // Two user/assistant messages but depth 5.
        expect(!resolve_preserve_split(build({"user", "assistant", "user", "assistant"}),
                                       5, true)
                    .compact);

        // Unbalanced history: the reference raises ValueError out of prepare();
        // the port reports the refusal instead of aborting the turn.
        const preserve_split orphan =
            resolve_preserve_split(build({"user", "tool", "user"}), 1, true);
        expect(orphan.unbalanced);
        expect(!orphan.compact);

        // A tolerated dangling assistant tool call is not "unbalanced".
        expect(!resolve_preserve_split(build({"user", "assistant"}), 1, true).unbalanced);

        // Primacy bias: the first message is always kept when the boundary is a
        // plain cut, so a long conversation never loses its original prompt.
        // Reference: SimpleCompaction(max_preserved_messages=1).prepare(
        //   [q0, a0, q1, a1, q2, a2, q3, a3]).to_preserve == [q0, a3].
        kimix::vector<message> long_hist;
        for (int i = 0; i < 4; ++i) {
            message u;
            u.role = "user";
            u.content.push_back(content_part{"text", kimix::format("q{}", i)});
            long_hist.push_back(std::move(u));
            message a;
            a.role = "assistant";
            a.content.push_back(content_part{"text", kimix::format("a{}", i)});
            long_hist.push_back(std::move(a));
        }
        const preserve_split shaped = resolve_preserve_split(long_hist, 1, true);
        expect(shaped.compact);
        expect(shaped.keep_first_message);
        expect(!shaped.recut_fallback);
        expect(eq(static_cast<int32_t>(shaped.preserve_start_index), 7));

        // The balanced snap is what keeps the tail off a mid call/result pair:
        // reference cuts for [u0, a0(two calls), t0, u1] are {0, 1, 4}, so the
        // walked boundary 3 snaps to 1 -- and after the Phase-6 removal the
        // compacted region is empty, i.e. the reference does not compact at all.
        kimix::vector<message> snapped;
        {
            message u;
            u.role = "user";
            u.content.push_back(content_part{"text", kimix::string("u0")});
            snapped.push_back(std::move(u));
            message a;
            a.role = "assistant";
            a.content.push_back(content_part{"text", kimix::string("a0")});
            a.tool_call_count = 2;
            snapped.push_back(std::move(a));
            message t;
            t.role = "tool";
            t.content.push_back(content_part{"text", kimix::string("t0")});
            snapped.push_back(std::move(t));
            message u1;
            u1.role = "user";
            u1.content.push_back(content_part{"text", kimix::string("u1")});
            snapped.push_back(std::move(u1));
            expect(eq(nearest_balanced_cut_before(snapped, 3), static_cast<size_t>(1)));
        }
        const preserve_split refused = resolve_preserve_split(snapped, 1, true);
        expect(!refused.compact);
        expect(!refused.unbalanced);
        // Without the balanced-cut snap the raw walk (index 3) is used instead.
        const preserve_split raw = resolve_preserve_split(snapped, 1, false);
        expect(raw.compact);
        expect(eq(static_cast<int32_t>(raw.preserve_start_index), 3));
        expect(raw.keep_first_message);
    };

    return 0;
}
