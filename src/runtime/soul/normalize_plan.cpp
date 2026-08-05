/*
 * normalize_plan.cpp - see normalize_plan.h (plan 015).
 *
 * Exact port of dynamic_injection.py::normalize_history:
 *   result = []
 *   for msg in history:
 *       if (result and result[-1].role == msg.role and msg.role == "user"
 *               and not is_notification_message(result[-1])
 *               and not is_notification_message(msg)):
 *           merged = list(result[-1].content) + list(msg.content)
 *           result[-1] = Message(role="user", content=merged)
 *       else:
 *           result.append(msg)
 *
 * A merged aggregate never satisfies is_notification_message (its content is
 * the concatenation of two non-notification contents), so tracking the
 * notification flag of the message the last result entry ORIGINATED from is
 * exact.
 */

#include <runtime/soul/normalize_plan.h>

#include <runtime/soul/soul_util.h>

namespace kimix {
namespace runtime {
namespace soul {

void build_normalize_plan(kimix::span<const message_view> msgs,
                          kimix::vector<normalize_step>& out) noexcept {
    out.clear();
    if (msgs.empty()) {
        return;
    }

    // State of the last entry of the (virtual) result list.
    uint8_t last_role = msgs[0].role;
    bool last_is_notification = is_notification_msg(msgs[0]);
    uint32_t last_origin = 0; // input index the last result entry stems from

    for (size_t i = 0; i < msgs.size(); ++i) {
        normalize_step step;
        step.index = static_cast<uint32_t>(i);
        const message_view& m = msgs[i];
        if (i > 0 && last_role == m.role && m.role == kRoleUser &&
            !last_is_notification && !is_notification_msg(m)) {
            // Merge into the previous result entry (which originated at
            // last_origin; chained merges keep the same target).
            step.op = kNormalizeMergeInto;
            step.target_index = last_origin;
        } else {
            step.op = kNormalizeKeep;
            step.target_index = static_cast<uint32_t>(i);
            last_role = m.role;
            last_is_notification = is_notification_msg(m);
            last_origin = static_cast<uint32_t>(i);
        }
        out.push_back(step);
    }
}

} // namespace soul
} // namespace runtime
} // namespace kimix
