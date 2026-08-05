/*
 * message_view.h - Message-view bridge structs (kimix::runtime::soul).
 *
 * Plan 014: the message-view bridge. Python builds these views cheaply from
 * pydantic messages (no deepcopy, no model_dump): one big UTF-8 buffer holds
 * every string, and the views are slices into it. C++ runs prune scans,
 * normalize plans, id fixes, and the OpenAI payload conversion over the
 * views and returns plans / JSON bytes that Python applies.
 *
 * Roles: 0=system 1=user 2=assistant 3=tool (kosong Role literal).
 * `tool_calls` / `tool_call_id` empty spans mean the field is None on the
 * pydantic message (exclude_none drops it during serialization).
 * `preserved_thinking_enabled` mirrors the per-request preserved-thinking
 * flag; build_payload takes the authoritative value as a parameter.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace soul {

enum class part_kind : uint8_t {
    TEXT = 0,
    THINK = 1,
    TOOL_CALL = 2,
    IMAGE = 3,
    AUDIO = 4,
    FILE = 5,
    OTHER = 6,
};

// One content part. For TEXT the span is the raw text; for THINK the think
// text; for every other kind the span carries the part's serialized JSON
// object (e.g. {"type":"image_url","image_url":{"url":"..."}}) which the
// payload/export builders embed verbatim.
struct part_view {
    part_kind kind;
    kimix::string_view text;
};

// One assistant tool call. `arguments` empty => function.arguments is None
// (the "arguments" key is omitted, matching exclude_none=True).
struct tool_call_view {
    kimix::string_view id;
    kimix::string_view name;
    kimix::string_view arguments;
};

enum : uint8_t {
    kRoleSystem = 0,
    kRoleUser = 1,
    kRoleAssistant = 2,
    kRoleTool = 3,
};

// Compact view of one history message. All spans point into the caller's
// UTF-8 buffer and stay valid for the duration of the kernel call.
struct message_view {
    uint8_t role; // 0 system 1 user 2 assistant 3 tool
    kimix::string_view tool_call_id;
    kimix::span<const part_view> parts;
    kimix::span<const tool_call_view> tool_calls;
    bool preserved_thinking_enabled = false;
};

} // namespace soul
} // namespace runtime
} // namespace kimix
