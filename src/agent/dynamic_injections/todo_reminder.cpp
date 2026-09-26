// agent/dynamic_injections/todo_reminder.cpp - see todo_reminder.h.

#include "agent/dynamic_injections/todo_reminder.h"

#include <core/stl/hash.h>

namespace kimix::agent {

TodoReminderProvider::TodoReminderProvider(
    kimix::function<kimix::vector<builtin_tools::todo::todo_item>()> todos_loader,
    int32_t interval_steps, int32_t max_items,
    kimix::function<kimix::vector<kimix::string>()> stack_loader)
    : _todos_loader(std::move(todos_loader)),
      _interval_steps(interval_steps < 1 ? 1 : interval_steps),
      _max_items(max_items), _stack_loader(std::move(stack_loader)) {}

uint64_t TodoReminderProvider::signature(
    kimix::span<const builtin_tools::todo::display_item> items,
    kimix::span<const kimix::string> stack) {
    // todo_reminder.py:705-731: hash of the stack titles plus the
    // (depth, status, title) tree pairs, so any edit inside a scope -
    // including a child status change or a push/pop - changes the signature.
    // The reference streams xxh64 over title + b"\x02", b"\x03", then per
    // item depth + b"\x00" + status + b"\x01" + title + b"\x04"; the native
    // port accumulates the same byte stream into kimix::hash64 (the digest
    // value itself is internal throttle state, not a wire format).
    kimix::string bytes;
    for (const kimix::string &title : stack) {
        bytes += title;
        bytes += '\x02';
    }
    bytes += '\x03';
    for (const builtin_tools::todo::display_item &item : items) {
        bytes += kimix::string(std::to_string(item.depth));
        bytes += '\x00';
        bytes.append(builtin_tools::todo::status_name(item.status).data(),
                     builtin_tools::todo::status_name(item.status).size());
        bytes += '\x01';
        bytes += item.title;
        bytes += '\x04';
    }
    return kimix::hash64(bytes.data(), bytes.size());
}

kimix::string TodoReminderProvider::render_reminder(
    kimix::span<const builtin_tools::todo::display_item> items,
    kimix::span<const kimix::string> stack, int32_t max_items) {
    if (items.empty()) {
        return kimix::string();
    }
    // todo_reminder.py:773-786 (exact lines and wording).
    kimix::string body =
        "Reminder — unfinished todo_write tasks (re-injected to keep your "
        "plan in focus):";
    if (!stack.empty()) {
        body += "\n- (stack: ";
        for (size_t i = 0; i < stack.size(); ++i) {
            if (i != 0) {
                body += " > ";
            }
            body += stack[i];
        }
        body += ")";
    }
    const size_t shown =
        items.size() > static_cast<size_t>(max_items)
            ? static_cast<size_t>(max_items)
            : items.size();
    for (size_t i = 0; i < shown; ++i) {
        const builtin_tools::todo::display_item &item = items[i];
        body += '\n';
        for (int32_t d = 0; d < item.depth; ++d) {
            body += "  "; // f"{'  ' * depth}- [{status}] {title}"
        }
        body += "- [";
        body.append(builtin_tools::todo::status_name(item.status).data(),
                    builtin_tools::todo::status_name(item.status).size());
        body += "] ";
        body += item.title;
    }
    if (items.size() > static_cast<size_t>(max_items)) {
        // f"- … and {len - max} more (call `todo_write` to read all)"
        body += "\n- … and ";
        body += kimix::string(std::to_string(items.size() - static_cast<size_t>(max_items)));
        body += " more (call `todo_write` to read all)";
    }
    body +=
        "\nKeep exactly one item `in_progress` and mark items `done` as you "
        "finish them.";
    return body;
}

bool TodoReminderProvider::get_injections(const InjectionStepContext &ctx,
                                          kimix::vector<DynamicInjection> &out,
                                          kimix::string &error) {
    (void)error;
    (void)ctx;
    // todo_reminder.py:733-787. The loaders must never raise (the native
    // loader reads the session todo state, which cannot fail).
    kimix::vector<builtin_tools::todo::todo_item> todos = _todos_loader();
    kimix::vector<kimix::string> stack;
    if (_stack_loader) {
        stack = _stack_loader();
    }

    // Unfinished items across the whole tree: done items are omitted but
    // their (possibly unfinished) descendants are still traversed, so a
    // finished parent never hides pending children
    // (session_state.flatten_todo_tree(include_done=False),
    // session_state.py:55-84). build_display_items flattens with depths but
    // keeps every status, so the done entries are dropped here.
    kimix::vector<builtin_tools::todo::display_item> all =
        builtin_tools::todo::build_display_items(todos);
    kimix::vector<builtin_tools::todo::display_item> unfinished;
    unfinished.reserve(all.size());
    for (builtin_tools::todo::display_item &item : all) {
        if (item.status != builtin_tools::todo::todo_status::done) {
            unfinished.push_back(std::move(item));
        }
    }
    if (unfinished.empty()) {
        // Reset so the next unfinished todo triggers an immediate reminder.
        _last_injected_step.reset();
        _last_signature = 0;
        return true;
    }

    const uint64_t sig = signature(unfinished, stack);
    const int32_t step_no = ctx.step_no;

    if (_last_injected_step.has_value()) {
        const bool unchanged = sig == _last_signature;
        const bool within_interval =
            (step_no - *_last_injected_step) < _interval_steps;
        if (unchanged && within_interval) {
            return true;
        }
    }

    _last_injected_step = step_no;
    _last_signature = sig;

    DynamicInjection injection;
    injection.type = "todo_reminder";
    injection.content = render_reminder(unfinished, stack, _max_items);
    out.push_back(std::move(injection));
    return true;
}

void TodoReminderProvider::on_context_compacted() {
    // todo_reminder.py:789-792: re-anchor the plan right after compaction.
    _last_injected_step.reset();
    _last_signature = 0;
}

void TodoReminderProvider::on_afk_changed(bool enabled) {
    // todo_reminder.py:794-797.
    (void)enabled;
    _last_injected_step.reset();
    _last_signature = 0;
}

} // namespace kimix::agent
