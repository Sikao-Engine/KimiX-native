/*
 * soul_util.h - shared soul-domain predicates (kimix::runtime::soul).
 *
 * Inline helpers shared by the soul kernels (prune scanner, normalize plan,
 * reminder stripper). Marked `inline` so the unity build never hits
 * redefinition collisions. All predicates mirror the kimi-cli reference
 * exactly:
 *   - is_system_reminder_msg  : soul/message.py is_system_reminder_message
 *   - is_notification_msg     : notifications/llm.py is_notification_message
 *   - concat_text_parts       : concatenated TextPart texts of a message
 */

#pragma once

#include <core/kimix_core.h>
#include <runtime/soul/message_view.h>
#include <runtime/common/text_util.h>

namespace kimix {
namespace runtime {
namespace soul {

inline bool is_system_reminder_msg(const message_view& m) noexcept {
    if (m.role != kRoleUser || m.parts.size() != 1) {
        return false;
    }
    const part_view& p = m.parts[0];
    return p.kind == part_kind::TEXT &&
           common::trimmed_starts_with(p.text, "<system-reminder>");
}

inline bool is_notification_msg(const message_view& m) noexcept {
    if (m.role != kRoleUser || m.parts.size() != 1) {
        return false;
    }
    const part_view& p = m.parts[0];
    if (p.kind != part_kind::TEXT) {
        return false;
    }
    // part.text.lstrip().startswith("<notification ")
    return common::ltrim_py_ws(p.text).substr(0, 14) == "<notification ";
}

// export.py::_is_checkpoint_message: role=user, single TextPart whose
// stripped text starts with "<system>CHECKPOINT".
inline bool is_checkpoint_msg(const message_view& m) noexcept {
    if (m.role != kRoleUser || m.parts.size() != 1) {
        return false;
    }
    const part_view& p = m.parts[0];
    return p.kind == part_kind::TEXT &&
           common::trimmed_starts_with(p.text, "<system>CHECKPOINT");
}

inline bool contains(kimix::string_view haystack, kimix::string_view needle) noexcept {
    return haystack.find(needle) != kimix::string_view::npos;
}

// Concatenated TextPart texts of a message (Python: the reference scans
// `for part in msg.content: if isinstance(part, TextPart): text += part.text`).
inline kimix::string concat_text_parts(const message_view& m) noexcept {
    kimix::string out;
    for (const part_view& p : m.parts) {
        if (p.kind == part_kind::TEXT) {
            out.append(p.text.data(), p.text.size());
        }
    }
    return out;
}

// kosong Role literal -> wire string (defensive fallback: "user").
inline const char* role_name(uint8_t role) noexcept {
    switch (role) {
    case kRoleSystem:
        return "system";
    case kRoleUser:
        return "user";
    case kRoleAssistant:
        return "assistant";
    case kRoleTool:
        return "tool";
    default:
        return "user";
    }
}

} // namespace soul
} // namespace runtime
} // namespace kimix
