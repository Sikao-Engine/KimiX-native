/*
 * prompt_builder.cpp - see prompt_builder.h (plan 016).
 *
 * Format matches SimpleCompaction.prepare EXACTLY (default options:
 * mode=balanced, avoid_cascade=False, decision_section disabled,
 * custom_instruction=""):
 *
 *   compact_message.content = [
 *     TextPart("## Message 1\nRole: user\nContent:\n"),
 *     ...TextParts of message 1...,
 *     TextPart("## Message 2\nRole: assistant\nContent:\n"),
 *     ...TextParts of message 2...,
 *     TextPart("\n" + COMPACT + "\n\n" + BALANCED_GUIDANCE)
 *   ]
 *
 * cascade_depth counts messages whose TextParts contain
 * "Previous context has been compacted" (_detect_cascade_depth).
 */

#include <runtime/soul/prompt_builder.h>

#include <runtime/soul/prompt_texts.h>
#include <runtime/soul/soul_util.h>

namespace kimix {
namespace runtime {
namespace soul {
namespace {

// _detect_cascade_depth: messages whose content carries the compaction
// summary marker (one count per message, first matching part only).
uint32_t detect_cascade_depth(kimix::span<const message_view> msgs) noexcept {
    uint32_t depth = 0;
    for (const message_view& m : msgs) {
        for (const part_view& p : m.parts) {
            if (p.kind == part_kind::TEXT &&
                contains(p.text, "Previous context has been compacted")) {
                ++depth;
                break;
            }
        }
    }
    return depth;
}

} // namespace

void build_compaction_prompt(kimix::span<const message_view> msgs,
                             kimix::string_view system_prompt,
                             kimix::string& out) noexcept {
    out.clear();
    if (!system_prompt.empty()) {
        out.append(system_prompt.data(), system_prompt.size());
        out += "\n\n";
    }

    for (size_t i = 0; i < msgs.size(); ++i) {
        out += "## Message ";
        out += std::to_string(i + 1);
        out += "\nRole: ";
        out += role_name(msgs[i].role);
        out += "\nContent:\n";
        for (const part_view& p : msgs[i].parts) {
            if (p.kind == part_kind::TEXT) {
                out.append(p.text.data(), p.text.size());
            }
        }
    }

    out += "\n";
    if (detect_cascade_depth(msgs) >= 3) {
        out += kCompactCascadePromptText;
    } else {
        out += kCompactPromptText;
    }
    out += "\n\n";
    out += kBalancedModeGuidance;
}

} // namespace soul
} // namespace runtime
} // namespace kimix
