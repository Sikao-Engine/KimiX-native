// agent/dynamic_injections/todo_reminder.h - unfinished-todo re-injection.
//
// Byte-faithful port of kimi_cli/soul/dynamic_injections/todo_reminder.py
// (156 lines): re-injects the unfinished todo tree (max 20 items, two-space
// indent per depth, optional stack breadcrumb) at the context tail whenever
// the unfinished set first appears, the (titles + statuses + stack)
// signature changes, or interval_steps have passed. The loader must never
// raise; an empty unfinished set resets the throttle so the next unfinished
// todo triggers immediately.
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "agent/dynamic_injection.h"
#include "builtin_tools/todo_tool.h"

namespace kimix::agent {

// Maximum unfinished items shown per reminder (_MAX_REMINDER_ITEMS).
inline constexpr int32_t kTodoReminderMaxItems = 20;

class TodoReminderProvider final : public DynamicInjectionProvider {
public:
    // todos_loader: zero-arg callable returning the current todo tree (root
    // or subagent scope); must never fail. interval_steps: minimum steps
    // between repeated injections of an unchanged list
    // (LoopControl.todo_reminder_interval_steps, default 20). max_items:
    // maximum unfinished items shown (default 20). stack_loader: optional
    // breadcrumb loader (root -> current focus parent); nullptr means root
    // scope (no breadcrumb line). The native todo state has no legacy stack,
    // so the soul passes none.
    explicit TodoReminderProvider(
        kimix::function<kimix::vector<builtin_tools::todo::todo_item>()> todos_loader,
        int32_t interval_steps = 20,
        int32_t max_items = kTodoReminderMaxItems,
        kimix::function<kimix::vector<kimix::string>()> stack_loader = {});

    bool get_injections(const InjectionStepContext &ctx,
                        kimix::vector<DynamicInjection> &out,
                        kimix::string &error) override;

    void on_context_compacted() override;
    void on_afk_changed(bool enabled) override;

    // The rendered reminder body (todo_reminder.py:773-786), shared with
    // tests. `items` are the flattened unfinished tree (depth-first,
    // done items omitted but their unfinished children kept); `stack` is
    // the optional breadcrumb. Returns "" when items is empty.
    static kimix::string render_reminder(
        kimix::span<const builtin_tools::todo::display_item> items,
        kimix::span<const kimix::string> stack, int32_t max_items);

private:
    // The (stack + depth/status/title tree) signature hash
    // (todo_reminder.py:705-731); 0 == no signature yet.
    static uint64_t signature(
        kimix::span<const builtin_tools::todo::display_item> items,
        kimix::span<const kimix::string> stack);

    kimix::function<kimix::vector<builtin_tools::todo::todo_item>()> _todos_loader;
    int32_t _interval_steps;
    int32_t _max_items;
    kimix::function<kimix::vector<kimix::string>()> _stack_loader;
    kimix::optional<int32_t> _last_injected_step;
    uint64_t _last_signature = 0;
};

} // namespace kimix::agent
