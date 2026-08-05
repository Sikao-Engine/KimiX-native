/*
 * reminder_stripper.h - leading system-reminder counter
 * (kimix::runtime::soul).
 *
 * Plan 015: O(n) replacement for the O(n^2) front-pop loop of
 * soul/message.py::strip_system_reminders. The kernel returns the number of
 * CONSECUTIVE LEADING system-reminder user messages; Python slices the
 * history once (msgs = msgs[count:]).
 *
 * NOTE (documented deviation): the current reference implementation removes
 * reminder messages at ANY position; the native kernel deliberately counts
 * only the leading run (the plan's specified contract -- reminders are
 * re-injected fresh at the front of every step, so the leading run is the
 * one that accumulates).
 */

#pragma once

#include <core/kimix_core.h>
#include <runtime/soul/message_view.h>
#include <runtime/soul/soul_util.h>

namespace kimix {
namespace runtime {
namespace soul {

// Number of consecutive leading messages matching is_system_reminder_message
// (role=user, single TextPart whose stripped text starts with
// "<system-reminder>"). Stops at the first non-reminder message.
inline uint32_t count_leading_reminders(kimix::span<const message_view> msgs) noexcept {
    uint32_t count = 0;
    for (const message_view& m : msgs) {
        if (!is_system_reminder_msg(m)) {
            break;
        }
        ++count;
    }
    return count;
}

} // namespace soul
} // namespace runtime
} // namespace kimix
