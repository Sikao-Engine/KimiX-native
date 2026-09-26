// test_verification_gate.cpp - Unit tests for the soul-layer verification
// gate (src/agent/verification_gate.h), a port of
// kimi_cli/soul/verification_gate.py: unfinished todos and edits-without-
// verification block a no_tool_calls stop with a graded nudge (max 2 per
// turn). All expected strings are the reference's verbatim text.
//
// Framework: Boost.UT (tests/ut/ut.hpp).
#include "ut/ut.hpp"

#include <agent/verification_gate.h>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

using kimix::agent::VerificationGate;
using kimix::builtin_tools::todo::todo_item;
using kimix::builtin_tools::todo::todo_status;

todo_item make_item(kimix::string title, todo_status status) {
    todo_item item;
    item.content = std::move(title);
    item.status = status;
    return item;
}

kimix::llm::Message user_msg(kimix::string_view text) {
    kimix::llm::Message m;
    m.role = "user";
    m.content.assign(text.data(), text.size());
    return m;
}

kimix::llm::Message assistant_with_call(kimix::string_view name,
                                        kimix::string_view args) {
    kimix::llm::Message m;
    m.role = "assistant";
    kimix::llm::ToolCall tc;
    tc.id = "call-1";
    tc.name.assign(name.data(), name.size());
    tc.arguments.assign(args.data(), args.size());
    m.tool_calls.push_back(std::move(tc));
    return m;
}

} // namespace

int main() {
    "clean_turn_passes_the_gate"_test = [] {
        VerificationGate gate;
        const kimix::vector<kimix::llm::Message> history{user_msg("do it")};
        const kimix::optional<kimix::string> nudge = gate.check(history, {});
        expect(!nudge.has_value());
    };

    "unfinished_todos_produce_verbatim_nudge"_test = [] {
        VerificationGate gate;
        kimix::vector<todo_item> todos;
        todos.push_back(make_item("Fix bug", todo_status::pending));
        const kimix::vector<kimix::llm::Message> history{user_msg("do it")};
        const auto nudge = gate.check(history, todos);
        expect(nudge.has_value());
        expect(*nudge ==
               "The turn cannot finish yet — verification gate findings:\n\n"
               "Unfinished todo_write tasks remain:\n"
               "- [pending] Fix bug\n\n"
               "Address the findings above, then finish. (nudge 1/2 this turn)");
        expect(gate.nudge_count() == 1);
        // Second nudge carries the counter; then the budget is exhausted.
        const auto nudge2 = gate.check(history, todos);
        expect(nudge2.has_value());
        expect(nudge2->find("(nudge 2/2 this turn)") != kimix::string::npos);
        expect(!gate.check(history, todos).has_value());
    };

    "done_todos_do_not_block_the_turn"_test = [] {
        VerificationGate gate;
        kimix::vector<todo_item> todos;
        todos.push_back(make_item("Fixed bug", todo_status::done));
        const kimix::vector<kimix::llm::Message> history{user_msg("do it")};
        expect(!gate.check(history, todos).has_value());
    };

    "unfinished_listing_caps_at_ten_plus_more"_test = [] {
        VerificationGate gate;
        kimix::vector<todo_item> todos;
        for (int i = 1; i <= 12; ++i) {
            todos.push_back(make_item(kimix::format("task {}", i),
                                      i == 12 ? todo_status::done
                                              : todo_status::pending));
        }
        const kimix::vector<kimix::llm::Message> history{user_msg("do it")};
        const auto nudge = gate.check(history, todos);
        expect(nudge.has_value());
        expect(nudge->find("- [pending] task 1") != kimix::string::npos);
        expect(nudge->find("- [pending] task 10") != kimix::string::npos);
        expect(nudge->find("- … and 1 more") != kimix::string::npos);
        expect(nudge->find("task 11") == kimix::string::npos); // beyond the cap
    };

    "edits_without_verification_produce_nudge"_test = [] {
        VerificationGate gate;
        // The edit and the final answer belong to the SAME turn: no real user
        // message may appear between them.
        kimix::vector<kimix::llm::Message> history;
        history.push_back(user_msg("do it"));
        history.push_back(assistant_with_call("write", R"({"path":"a.cpp"})"));
        kimix::llm::Message done;
        done.role = "assistant";
        done.content = "done";
        history.push_back(done);
        const auto nudge = gate.check(history, {});
        expect(nudge.has_value());
        expect(nudge->find("You modified code this turn but ran no verification "
                           "(no tests/check commands). "
                           "Run the project's tests or a verification command "
                           "before finishing.") != kimix::string::npos);
    };

    "shell_tool_call_counts_as_verification"_test = [] {
        VerificationGate gate;
        kimix::vector<kimix::llm::Message> history;
        history.push_back(user_msg("do it"));
        history.push_back(assistant_with_call("write", R"({"path":"a.cpp"})"));
        history.push_back(assistant_with_call("bash", R"({"command":"make test"})"));
        kimix::llm::Message done;
        done.role = "assistant";
        done.content = "done";
        history.push_back(done);
        expect(!gate.check(history, {}).has_value());
    };

    "todo_write_marks_done_counts_as_verification"_test = [] {
        VerificationGate gate;
        kimix::vector<kimix::llm::Message> history;
        history.push_back(user_msg("do it"));
        history.push_back(assistant_with_call("write", R"({"path":"a.cpp"})"));
        history.push_back(assistant_with_call(
            "todo_write", R"({"todos":[{"title":"t","status":"done"}]})"));
        kimix::llm::Message done;
        done.role = "assistant";
        done.content = "done";
        history.push_back(done);
        expect(!gate.check(history, {}).has_value());
    };

    "injected_system_reminder_user_messages_do_not_end_the_turn_scope"_test = [] {
        VerificationGate gate;
        kimix::vector<kimix::llm::Message> history;
        history.push_back(user_msg("do it"));
        history.push_back(assistant_with_call("write", R"({"path":"a.cpp"})"));
        // A previous gate nudge is a system-reminder user message: the turn
        // scope must still see the write call above it.
        history.push_back(user_msg("<system-reminder>\nprevious nudge\n</system-reminder>"));
        kimix::llm::Message done;
        done.role = "assistant";
        done.content = "done";
        history.push_back(done);
        const auto nudge = gate.check(history, {});
        expect(nudge.has_value());
        expect(nudge->find("You modified code this turn") != kimix::string::npos);
    };

    "todolist_marks_done_argument_parsing"_test = [] {
        using kimix::agent::VerificationGate;
        expect(VerificationGate::todolist_marks_done(
            R"({"todos":[{"status":"done"}]})"));
        expect(VerificationGate::todolist_marks_done(
            R"({"items":[{"status":"completed"}]})"));
        // A single dict is treated as a one-element list.
        expect(VerificationGate::todolist_marks_done(R"({"items":{"status":"done"}})"));
        expect(!VerificationGate::todolist_marks_done(
            R"({"todos":[{"status":"pending"}]})"));
        expect(!VerificationGate::todolist_marks_done(R"({"todos":[]})"));
        expect(!VerificationGate::todolist_marks_done(""));
        expect(!VerificationGate::todolist_marks_done("garbage"));
        expect(!VerificationGate::todolist_marks_done(R"({"other":1})"));
    };

    "begin_turn_resets_the_nudge_budget"_test = [] {
        VerificationGate gate;
        kimix::vector<todo_item> todos;
        todos.push_back(make_item("Fix bug", todo_status::pending));
        const kimix::vector<kimix::llm::Message> history{user_msg("do it")};
        expect(gate.check(history, todos).has_value());
        expect(gate.check(history, todos).has_value());
        expect(!gate.check(history, todos).has_value()); // exhausted
        gate.begin_turn();
        expect(gate.nudge_count() == 0);
        expect(gate.check(history, todos).has_value()); // fresh budget
    };

    "zero_max_nudges_disables_the_gate"_test = [] {
        VerificationGate gate(0);
        kimix::vector<todo_item> todos;
        todos.push_back(make_item("Fix bug", todo_status::pending));
        kimix::vector<kimix::llm::Message> history;
        history.push_back(user_msg("do it"));
        history.push_back(assistant_with_call("write", R"({"path":"a"})"));
        expect(!gate.check(history, todos).has_value());
    };

    return 0;
}
