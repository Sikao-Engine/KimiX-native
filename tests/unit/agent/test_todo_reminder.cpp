// test_todo_reminder.cpp - Unit tests for the todo-reminder provider
// (src/agent/dynamic_injections/todo_reminder.h, a port of
// kimi_cli/soul/dynamic_injections/todo_reminder.py): the exact reminder
// wording (breadcrumb, depth indent, 20-item cap + overflow line), the
// signature+interval throttle, the all-done reset, and the compaction/afk
// resets.
//
// Framework: Boost.UT (tests/ut/ut.hpp).
#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include <agent/dynamic_injections/todo_reminder.h>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

using kimix::builtin_tools::todo::todo_item;
using kimix::builtin_tools::todo::todo_status;
using kimix::builtin_tools::todo::display_item;

todo_item make_item(kimix::string title, todo_status status) {
    todo_item item;
    item.content = std::move(title);
    item.status = status;
    return item;
}

kimix::agent::InjectionStepContext ctx(int32_t step) {
    kimix::agent::InjectionStepContext c;
    c.step_no = step;
    return c;
}

kimix::vector<kimix::agent::DynamicInjection>
collect(kimix::agent::TodoReminderProvider &p,
        const kimix::agent::InjectionStepContext &c) {
    kimix::vector<kimix::agent::DynamicInjection> out;
    kimix::string err;
    expect(p.get_injections(c, out, err)) << err;
    return out;
}

// A provider over a mutable in-memory todo list.
kimix::unique_ptr<kimix::agent::TodoReminderProvider> make_provider(
    kimix::shared_ptr<kimix::vector<todo_item>> todos, int32_t interval = 10,
    kimix::shared_ptr<kimix::vector<kimix::string>> stack = {}) {
    kimix::weak_ptr<kimix::vector<todo_item>> weak = todos;
    kimix::function<kimix::vector<todo_item>()> loader =
        [weak] {
            kimix::vector<todo_item> out;
            if (auto locked = weak.lock()) {
                out = *locked;
            }
            return out;
        };
    kimix::function<kimix::vector<kimix::string>()> stack_loader;
    if (stack) {
        kimix::weak_ptr<kimix::vector<kimix::string>> weak_stack = stack;
        stack_loader = [weak_stack] {
            kimix::vector<kimix::string> out;
            if (auto locked = weak_stack.lock()) {
                out = *locked;
            }
            return out;
        };
    }
    return kimix::unique_ptr<kimix::agent::TodoReminderProvider>(
        new kimix::agent::TodoReminderProvider(std::move(loader), interval,
                                               kimix::agent::kTodoReminderMaxItems,
                                               std::move(stack_loader)));
}

} // namespace

int main() {
    "no_todos_no_injection"_test = [] {
        auto todos = kimix::shared_ptr<kimix::vector<todo_item>>(new kimix::vector<todo_item>);
        auto p = make_provider(todos);
        expect(collect(*p, ctx(1)).empty());
    };

    "single_unfinished_item_verbatim"_test = [] {
        auto todos = kimix::shared_ptr<kimix::vector<todo_item>>(new kimix::vector<todo_item>);
        todos->push_back(make_item("Fix the parser", todo_status::in_progress));
        auto p = make_provider(todos);
        const auto got = collect(*p, ctx(1));
        expect(eq(got.size(), 1u));
        expect(eq(got[0].type, kimix::string("todo_reminder")));
        // todo_reminder.py:773-786 verbatim.
        expect(eq(got[0].content,
                  kimix::string("Reminder — unfinished todo_write tasks (re-injected to keep "
                                "your plan in focus):\n"
                                "- [in_progress] Fix the parser\n"
                                "Keep exactly one item `in_progress` and mark items `done` as "
                                "you finish them.")));
        // Same signature, within the interval: no re-fire.
        expect(collect(*p, ctx(5)).empty());
        // Interval passed: re-fires the unchanged list.
        expect(collect(*p, ctx(11)).size() == 1u);
    };

    "done_items_skipped_but_their_pending_children_kept"_test = [] {
        auto todos = kimix::shared_ptr<kimix::vector<todo_item>>(new kimix::vector<todo_item>);
        todo_item parent = make_item("Parent done", todo_status::done);
        parent.children.push_back(make_item("Child pending", todo_status::pending));
        todos->push_back(std::move(parent));
        todos->push_back(make_item("Root pending", todo_status::pending));
        auto p = make_provider(todos);
        const auto got = collect(*p, ctx(1));
        expect(eq(got.size(), 1u));
        expect(eq(got[0].content,
                  kimix::string("Reminder — unfinished todo_write tasks (re-injected to keep "
                                "your plan in focus):\n"
                                "  - [pending] Child pending\n"
                                "- [pending] Root pending\n"
                                "Keep exactly one item `in_progress` and mark items `done` as "
                                "you finish them.")));
    };

    "signature_change_refires_immediately"_test = [] {
        auto todos = kimix::shared_ptr<kimix::vector<todo_item>>(new kimix::vector<todo_item>);
        todos->push_back(make_item("One", todo_status::pending));
        auto p = make_provider(todos, /*interval=*/10);
        expect(collect(*p, ctx(1)).size() == 1u);
        (*todos)[0].status = todo_status::done;
        todos->push_back(make_item("Two", todo_status::pending));
        const auto got = collect(*p, ctx(2)); // within interval but new signature
        expect(eq(got.size(), 1u));
        expect(got[0].content.find("- [pending] Two") != kimix::string::npos);
    };

    "all_done_resets_the_throttle"_test = [] {
        auto todos = kimix::shared_ptr<kimix::vector<todo_item>>(new kimix::vector<todo_item>);
        todos->push_back(make_item("One", todo_status::pending));
        auto p = make_provider(todos);
        expect(collect(*p, ctx(1)).size() == 1u);
        (*todos)[0].status = todo_status::done;
        expect(collect(*p, ctx(2)).empty()); // nothing unfinished
        todos->push_back(make_item("Fresh", todo_status::pending));
        // The reset makes the next unfinished todo fire immediately, even at
        // the same step as the empty evaluation.
        expect(collect(*p, ctx(2)).size() == 1u);
    };

    "overflow_line_after_max_items"_test = [] {
        auto todos = kimix::shared_ptr<kimix::vector<todo_item>>(new kimix::vector<todo_item>);
        for (int i = 0; i < 23; ++i) {
            todos->push_back(make_item("Task " + kimix::string(std::to_string(i)),
                                       todo_status::pending));
        }
        auto p = make_provider(todos);
        const auto got = collect(*p, ctx(1));
        expect(eq(got.size(), 1u));
        expect(got[0].content.find("- … and 3 more (call `todo_write` to read all)") !=
               kimix::string::npos);
        expect(got[0].content.find("Task 19") != kimix::string::npos);
        expect(got[0].content.find("Task 20") == kimix::string::npos);
    };

    "stack_breadcrumb_renders_and_changes_signature"_test = [] {
        auto todos = kimix::shared_ptr<kimix::vector<todo_item>>(new kimix::vector<todo_item>);
        todos->push_back(make_item("One", todo_status::pending));
        auto stack = kimix::shared_ptr<kimix::vector<kimix::string>>(
            new kimix::vector<kimix::string>{"Scope A", "Scope B"});
        auto p = make_provider(todos, 10, stack);
        const auto got = collect(*p, ctx(1));
        expect(eq(got.size(), 1u));
        expect(got[0].content.find("- (stack: Scope A > Scope B)\n") !=
               kimix::string::npos);
        // A push/pop changes the signature even with the list unchanged.
        stack->pop_back();
        expect(collect(*p, ctx(2)).size() == 1u);
    };

    "compaction_and_afk_reset_the_throttle"_test = [] {
        auto todos = kimix::shared_ptr<kimix::vector<todo_item>>(new kimix::vector<todo_item>);
        todos->push_back(make_item("One", todo_status::pending));
        auto p = make_provider(todos);
        expect(collect(*p, ctx(1)).size() == 1u);
        expect(collect(*p, ctx(2)).empty()); // throttled by interval
        p->on_context_compacted();
        expect(collect(*p, ctx(2)).size() == 1u); // re-anchored
        expect(collect(*p, ctx(3)).empty());
        p->on_afk_changed(false);
        expect(collect(*p, ctx(3)).size() == 1u); // re-anchored again
    };
}
