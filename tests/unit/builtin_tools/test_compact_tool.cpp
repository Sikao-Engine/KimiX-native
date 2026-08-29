// Test for the compact context-compaction kernels (builtin_tools/compact_tool.h).
//
// Covers the golden vectors from plans/compact.md §6.

#include "ut/ut.hpp"

#include "builtin_tools/compact_tool.h"
#include "builtin_tools/tool_types.h"

#include <cstdint>

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

    "should_auto_compact_zero_max_context"_test = [] {
        compaction_trigger_config cfg;
        cfg.max_context_size = 0;
        expect(!should_auto_compact(1000, cfg));
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

    return 0;
}
