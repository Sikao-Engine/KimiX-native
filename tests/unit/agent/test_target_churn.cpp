// test_target_churn.cpp - Unit tests for the target-churn provider
// (src/agent/dynamic_injections/target_churn.h, a port of
// kimi_cli/soul/dynamic_injections/target_churn.py): per-file edit counting
// across edit/shell tools (redirect / sed -i / tee extraction), normalized
// error-fingerprint streaks, the warn/strong/cooldown throttles, per-turn
// dedup, and the compaction reset. All expected strings are the reference's
// verbatim text.
//
// Framework: Boost.UT (tests/ut/ut.hpp).
#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include <agent/dynamic_injections/target_churn.h>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

kimix::llm::Message assistant_call(kimix::string_view name, kimix::string_view args) {
    kimix::llm::Message m;
    m.role = "assistant";
    kimix::llm::ToolCall tc;
    tc.id = "call-1";
    tc.name.assign(name.data(), name.size());
    tc.arguments.assign(args.data(), args.size());
    m.tool_calls.push_back(std::move(tc));
    return m;
}

kimix::llm::Message tool_result(kimix::string_view text) {
    kimix::llm::Message m;
    m.role = "tool";
    m.tool_call_id = "call-1";
    m.content.assign(text.data(), text.size());
    return m;
}

kimix::agent::InjectionStepContext ctx(kimix::vector<kimix::llm::Message> &history,
                                       int32_t step, uint64_t turn = 1) {
    kimix::agent::InjectionStepContext c;
    c.history = &history;
    c.step_no = step;
    c.turn_seq = turn;
    return c;
}

kimix::vector<kimix::agent::DynamicInjection>
collect(kimix::agent::TargetChurnProvider &p,
        const kimix::agent::InjectionStepContext &c) {
    kimix::vector<kimix::agent::DynamicInjection> out;
    kimix::string err;
    expect(p.get_injections(c, out, err)) << err;
    return out;
}

// Appends n edit-tool calls to one file (counted as n more edits).
void edit_n(kimix::vector<kimix::llm::Message> &h, kimix::string_view path, int n) {
    for (int i = 0; i < n; ++i) {
        h.push_back(assistant_call("write", R"({"path":")" + kimix::string(path) +
                                       R"("})"));
    }
}

} // namespace

int main() {
    "file_warn_fires_with_verbatim_string"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        edit_n(h, "src/main.cpp", 4); // file_warn = 3 for this test
        kimix::agent::TargetChurnProvider p(/*file_warn=*/3, /*file_strong=*/6,
                                            /*error_warn=*/2, /*cooldown=*/2);
        const auto got = collect(p, ctx(h, 5));
        expect(eq(got.size(), 1u));
        expect(eq(got[0].type, kimix::string("target_churn")));
        // target_churn.py:618-627 verbatim (path normalized Windows-style,
        // lowercase).
        expect(eq(got[0].content,
                  kimix::string("You have edited `src\\main.cpp` 4 times. If you are "
                                "iterating "
                                "without progress, pause and reconsider: verify your "
                                "understanding "
                                "of the failure, and consider rewriting the file or choosing a "
                                "different approach instead of another small patch.")));
    };

    "threshold_edges_silent_below_warn"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        edit_n(h, "a.txt", 2);
        kimix::agent::TargetChurnProvider p(3, 6, 2, 2);
        expect(collect(p, ctx(h, 3)).empty());
    };

    "strong_alert_has_priority_and_verbatim_string"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        edit_n(h, "loop.py", 8); // file_strong = 6
        kimix::agent::TargetChurnProvider p(/*file_warn=*/3, /*file_strong=*/6,
                                            /*error_warn=*/2, /*cooldown=*/2);
        const auto got = collect(p, ctx(h, 9));
        expect(eq(got.size(), 1u));
        // target_churn.py:583-593 verbatim.
        expect(eq(got[0].content,
                  kimix::string("You have modified the same file `loop.py` 8 times. "
                                "Repeated patching of one file is a strong signal of a wrong "
                                "approach.\n"
                                "Stop patching. Rewrite the file as a whole from your current "
                                "understanding, or switch to a fundamentally different "
                                "approach. If tests keep failing, re-read the error and fix "
                                "the root cause instead of iterating on the same spot.")));
    };

    "cooldown_silences_followups_and_turn_dedup_holds"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        edit_n(h, "a.txt", 3);
        kimix::agent::TargetChurnProvider p(3, 6, 2, /*cooldown=*/4);
        expect(collect(p, ctx(h, 5, 1)).size() == 1u); // file warn fires
        edit_n(h, "b.txt", 3);
        // Inside the cooldown: nothing, even though b.txt crossed the warn.
        expect(collect(p, ctx(h, 6, 1)).empty());
        // Cooldown clear: b.txt warns.
        const auto b_alert = collect(p, ctx(h, 10, 1));
        expect(eq(b_alert.size(), 1u));
        expect(b_alert[0].content.find("`b.txt`") != kimix::string::npos);
        // a.txt already warned this turn: stays quiet.
        edit_n(h, "a.txt", 10);
        expect(collect(p, ctx(h, 11, 1)).empty());
        // A new turn re-arms the per-turn dedup and a.txt crossed the strong
        // threshold, but the cooldown still applies (11 - 10 < 4): quiet.
        expect(collect(p, ctx(h, 11, 2)).empty());
        const auto strong_alert = collect(p, ctx(h, 15, 2)); // a.txt now strong
        expect(eq(strong_alert.size(), 1u));
        expect(strong_alert[0].content.find("modified the same file") !=
               kimix::string::npos);
    };

    "shell_commands_count_toward_the_same_file"_test = [] {
        // Cross-tool churn: write x2 + redirect + sed -i + tee = 5 edits.
        kimix::vector<kimix::llm::Message> h;
        edit_n(h, "out.log", 2);
        h.push_back(assistant_call("bash", R"({"command":"echo hi >> out.log"})"));
        h.push_back(assistant_call("pwsh",
                                   R"({"command":"sed -i 's/a/b/' out.log"})"));
        h.push_back(assistant_call("bash", R"({"command":"tail x | tee out.log"})"));
        kimix::agent::TargetChurnProvider p(/*file_warn=*/5, /*file_strong=*/9,
                                            /*error_warn=*/2, /*cooldown=*/2);
        const auto got = collect(p, ctx(h, 6));
        expect(eq(got.size(), 1u));
        expect(eq(got[0].content,
                  kimix::string("You have edited `out.log` 5 times. If you are iterating "
                                "without progress, pause and reconsider: verify your "
                                "understanding "
                                "of the failure, and consider rewriting the file or choosing a "
                                "different approach instead of another small patch.")));
    };

    "redirect_lookbehind_ignores_fd_aggregates"_test = [] {
        // 2>&1 must NOT count as a write target ("&" is excluded); only the
        // plain redirect counts.
        kimix::vector<kimix::llm::Message> h;
        h.push_back(assistant_call("bash", R"({"command":"run 2>&1 | cat"})"));
        kimix::agent::TargetChurnProvider p(2, 4, 2, 2);
        expect(collect(p, ctx(h, 2)).empty());
        h.push_back(assistant_call("bash", R"({"command":"run > real.txt"})"));
        h.push_back(assistant_call("bash", R"({"command":"run > real.txt"})"));
        const auto got = collect(p, ctx(h, 4));
        expect(eq(got.size(), 1u));
        expect(got[0].content.find("`real.txt` 2 times") != kimix::string::npos);
    };

    "sed_inplace_backup_suffix_and_flags"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(assistant_call("bash",
                                   R"({"command":"sed -i.bak 's/a/b/g' note.md"})"));
        // The reference regex requires the quoted script(s) AFTER -i, so a
        // flag-then-target order extracts nothing (verified against
        // target_churn.py's _SED_I_RE).
        h.push_back(assistant_call("bash",
                                   R"({"command":"sed -e 's/x/y/' -i other.md"})"));
        h.push_back(assistant_call("bash",
                                   R"({"command":"xargs sed -i 's/a/b/' note.md"})"));
        kimix::agent::TargetChurnProvider p(2, 4, 2, 2);
        const auto got = collect(p, ctx(h, 4));
        expect(eq(got.size(), 1u));
        expect(got[0].content.find("`note.md` 2 times") != kimix::string::npos);
    };

    "error_streak_fires_once_with_verbatim_string"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        // Three identical-modulo-numbers errors (error_warn = 3).
        for (int i = 0; i < 3; ++i) {
            h.push_back(tool_result("<system>ERROR: SyntaxError at line " +
                                    kimix::string(std::to_string(10 + i)) +
                                    " of /src/app.py</system>"));
        }
        kimix::agent::TargetChurnProvider p(5, 8, /*error_warn=*/3, /*cooldown=*/2);
        const auto got = collect(p, ctx(h, 4));
        expect(eq(got.size(), 1u));
        // target_churn.py:601-611 verbatim.
        expect(eq(got[0].content,
                  kimix::string("The same error has occurred 3 times in a row "
                                "(identical modulo line numbers/paths). Retrying the same "
                                "fix is not "
                                "working.\n"
                                "Analyze the root cause: read the full error output, inspect "
                                "the exact code involved, and form a new hypothesis before "
                                "your next action.")));
        // One error alert per turn.
        h.push_back(tool_result("<system>ERROR: SyntaxError at line 99 of /src/app.py</system>"));
        expect(collect(p, ctx(h, 5, 1)).empty());
    };

    "successful_result_breaks_the_error_streak"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(tool_result("<system>ERROR: boom 1</system>"));
        h.push_back(tool_result("<system>ERROR: boom 2</system>"));
        h.push_back(tool_result("all good"));
        h.push_back(tool_result("<system>ERROR: boom 3</system>"));
        kimix::agent::TargetChurnProvider p(5, 8, 3, 2);
        expect(collect(p, ctx(h, 5)).empty()); // streak is 1 after the break
    };

    "different_errors_do_not_streak_together"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(tool_result("<system>ERROR: TypeError: nope</system>"));
        h.push_back(tool_result("<system>ERROR: ValueError: nope</system>"));
        h.push_back(tool_result("<system>ERROR: KeyError: nope</system>"));
        kimix::agent::TargetChurnProvider p(5, 8, 3, 2);
        expect(collect(p, ctx(h, 4)).empty());
    };

    "history_rewind_defensively_resets"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        edit_n(h, "a.txt", 3);
        kimix::agent::TargetChurnProvider p(3, 6, 2, 2);
        expect(collect(p, ctx(h, 4)).size() == 1u);
        // History rebuilt shorter without a compaction notification (D-Mail
        // revert): the cursor must not scan backwards.
        h.clear();
        h.push_back(tool_result("fresh"));
        expect(collect(p, ctx(h, 5)).empty());
    };

    "compaction_resets_all_counters"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        edit_n(h, "a.txt", 3);
        kimix::agent::TargetChurnProvider p(3, 6, 2, 2);
        expect(collect(p, ctx(h, 4)).size() == 1u);
        p.on_context_compacted();
        h.clear();
        edit_n(h, "a.txt", 2); // only 2 edits after compaction
        expect(collect(p, ctx(h, 5)).empty());
    };

    "knob_clamps_match_the_reference"_test = [] {
        // file_warn clamps to >= 2; file_strong to >= file_warn + 1.
        kimix::vector<kimix::llm::Message> h;
        edit_n(h, "x.txt", 2);
        kimix::agent::TargetChurnProvider p(1, 2, 1, 0);
        const auto got = collect(p, ctx(h, 3)); // warn=2, strong=3
        expect(eq(got.size(), 1u));
        expect(got[0].content.find("You have edited `x.txt` 2 times") !=
               kimix::string::npos);
    };

    "malformed_arguments_are_ignored"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(assistant_call("write", "{not json"));
        h.push_back(assistant_call("unknown_tool", R"({"path":"x"})"));
        h.push_back(assistant_call("bash", R"({"command":42})")); // not a string
        kimix::agent::TargetChurnProvider p(2, 4, 2, 0);
        expect(collect(p, ctx(h, 4)).empty());
    };
}
