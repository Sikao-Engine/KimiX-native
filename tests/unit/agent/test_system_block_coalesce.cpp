// test_system_block_coalesce.cpp - E10: the model-visible wrapper helpers
// (soul/message.py system() / system_reminder()) and the Layer-1 coalesce
// passes (message.py coalesce_tool_metadata / coalesce_content_parts) that
// merge duplicate adjacent <system> metadata so it costs tokens once per
// request. Applied through normalize_history (the request-serialization
// step); a history without adjacent <system> blocks is byte-identical.
//
// Framework: Boost.UT (tests/ut/ut.hpp). CPU-only.

#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "agent/dynamic_injection.h"

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

kimix::llm::Message msg(kimix::string_view role, kimix::string_view content) {
    kimix::llm::Message m;
    m.role.assign(role.data(), role.size());
    m.content.assign(content.data(), content.size());
    return m;
}

} // namespace

int main() {
    using namespace kimix::agent;

    "system_block_wrapper_is_exact"_test = [] {
        expect(system_block_text("CHECKPOINT 12") ==
               kimix::string("<system>CHECKPOINT 12</system>"));
        expect(system_block_text("") == kimix::string("<system></system>"));
    };

    "system_reminder_wrapper_is_exact"_test = [] {
        expect(system_reminder_text("hello") ==
               kimix::string("<system-reminder>\nhello\n</system-reminder>"));
    };

    "extract_system_block_matches_one_block"_test = [] {
        kimix::string inner;
        expect(extract_system_block("<system>Results truncated.</system>",
                                    inner));
        expect(inner == kimix::string("Results truncated."));
        // Inner text is stripped before the comparison (message.py:93).
        expect(extract_system_block("<system>  spaced  </system>", inner));
        expect(inner == kimix::string("spaced"));
        // A block with a tail is NOT one block (the leading-block variant
        // handles the tail case).
        expect(!extract_system_block("<system>a</system>\noutput", inner));
        expect(!extract_system_block("plain text", inner));
        expect(!extract_system_block("<system>unterminated", inner));
    };

    "coalesce_tool_metadata_merges_identical_adjacent_blocks"_test = [] {
        kimix::vector<kimix::llm::Message> history;
        history.push_back(msg("user", "list the files"));
        history.push_back(msg("assistant", ""));
        history.back().tool_calls.push_back({"call-1", "function", "read", "{}"});
        history.push_back(msg("tool", "<system>Results truncated to 20 "
                                       "lines.</system>\nfile-a"));
        history.back().tool_call_id = "call-1";
        history.push_back(msg("tool", "<system>Results truncated to 20 "
                                      "lines.</system>\nfile-b"));
        history.back().tool_call_id = "call-2";
        history.push_back(msg("tool", "<system>Results truncated to 20 "
                                      "lines.</system>\nfile-c"));
        history.back().tool_call_id = "call-3";
        history.push_back(msg("tool", "<system>Different note.</system>\nX"));
        history.back().tool_call_id = "call-4";

        const size_t removed = coalesce_adjacent_tool_metadata(history);
        expect(removed == 2_u);
        // The first message of the run keeps the block, annotated [x3]
        // (U+00D7 in the reference's f"[×{total}] ").
        expect(history[2].content ==
               kimix::string("<system>[\xC3\x97""3] Results truncated to 20 "
                             "lines.</system>\nfile-a"));
        // The followers lose their copy but keep their output.
        expect(history[3].content == kimix::string("\nfile-b"));
        expect(history[4].content == kimix::string("\nfile-c"));
        // A different block and a non-tool message are untouched.
        expect(history[5].content ==
               kimix::string("<system>Different note.</system>\nX"));
    };

    "coalesce_tool_metadata_never_empties_a_tool_message"_test = [] {
        kimix::vector<kimix::llm::Message> history;
        history.push_back(msg("tool", "<system>note.</system>\npayload"));
        history.back().tool_call_id = "call-1";
        // The follower carries ONLY the block: stripping it would leave an
        // empty tool result, which providers reject - the copy is kept.
        history.push_back(msg("tool", "<system>note.</system>"));
        history.back().tool_call_id = "call-2";

        const size_t removed = coalesce_adjacent_tool_metadata(history);
        expect(removed == 0_u);
        expect(history[0].content ==
               kimix::string("<system>[\xC3\x97""2] note.</system>\npayload"));
        expect(history[1].content == kimix::string("<system>note.</system>"));
    };

    "coalesce_adjacent_system_blocks_joins_inner_text"_test = [] {
        // message.py:179: merged = ". ".join(pending_system_texts).
        expect(coalesce_adjacent_system_blocks(
                   "<system>a</system><system>b</system>") ==
               kimix::string("<system>a. b</system>"));
        // Whitespace-only separation counts as adjacency in the flat model;
        // separators around the merged run are preserved.
        expect(coalesce_adjacent_system_blocks(
                   "<system>a</system>\n<system>b</system>\n<c>") ==
               kimix::string("<system>a. b</system>\n<c>"));
        // Non-adjacent blocks stay separate.
        expect(coalesce_adjacent_system_blocks(
                   "<system>a</system>text<system>b</system>") ==
               kimix::string("<system>a</system>text<system>b</system>"));
        // Inner text is stripped before the join (message.py:93).
        expect(coalesce_adjacent_system_blocks(
                   "<system> a </system><system>b</system>") ==
               kimix::string("<system>a. b</system>"));
        // Plain content is returned unchanged.
        expect(coalesce_adjacent_system_blocks("no blocks here") ==
               kimix::string("no blocks here"));
    };

    "normalize_history_applies_the_coalesce_pass"_test = [] {
        // Two adjacent user messages whose content is a <system> block each:
        // the merge concatenates them, the in-message pass collapses the run.
        kimix::vector<kimix::llm::Message> history;
        history.push_back(msg("user", "<system>checkpoint 1</system>"));
        history.push_back(msg("user", "<system>checkpoint 2</system>"));
        const kimix::vector<kimix::llm::Message> normalized =
            normalize_history(history);
        expect(normalized.size() == 1_u);
        expect(normalized[0].content ==
               kimix::string("<system>checkpoint 1. checkpoint 2</system>"));
    };

    "normalize_history_is_invisible_without_adjacent_system_blocks"_test = [] {
        // The reminders and the tool envelope survive verbatim: the pass must
        // not perturb a plain-text request.
        kimix::vector<kimix::llm::Message> history;
        history.push_back(msg("system", "prompt"));
        history.push_back(msg("user", "hello"));
        history.push_back(msg("user", "<system-reminder>\nnudge\n"
                                      "</system-reminder>"));
        history.push_back(msg("tool", "<system>ERROR: nope</system>"));
        history.back().tool_call_id = "call-1";
        history.push_back(msg("tool", "<system>ERROR: nope</system>"));
        history.back().tool_call_id = "call-2";

        const kimix::vector<kimix::llm::Message> normalized =
            normalize_history(history);
        // A standalone reminder never merges with the plain user message, so
        // the five messages all survive; only the tool-result metadata is
        // coalesced.
        expect(normalized.size() == 5_u);
        expect(normalized[1].content == kimix::string("hello"));
        // The two tool results share the same leading block: the FIRST keeps
        // the annotated copy (the reference annotates whenever a run exists,
        // even when every follower is block-only and keeps its copy).
        expect(normalized[3].content ==
               kimix::string("<system>[\xC3\x97""2] ERROR: nope</system>"));
        expect(normalized[4].content ==
               kimix::string("<system>ERROR: nope</system>"));
    };

    return 0;
}
