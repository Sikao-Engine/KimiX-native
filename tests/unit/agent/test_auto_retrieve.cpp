// test_auto_retrieve.cpp - Unit tests for the D11 auto-retrieval memory
// injection (src/agent/auto_retrieve.*, kimisoul.py:707-812).
//
// Covers the gates (tier enable flags, step-1 only, query >= 10 chars), the
// three citation tiers with their fixed headers (long-term/compacted,
// working/non-compacted with the last-2 exclusion, recency/boosted), the
// score thresholds, the per-turn injection + token budgets, the dedup set
// (capped at 10) and the "> **role**\n> text" citation body.
//
// The score thresholds are set to 0.0 in most cases so any BM25 match passes;
// threshold gating is tested separately with a huge threshold (the exact BM25
// magnitudes are pinned by test_native_history_index, not here).
//
// Framework: Boost.UT (tests/ut/ut.hpp).
#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include <agent/auto_retrieve.h>
#include <agent/soul.h>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

kimix::llm::Message user_msg(kimix::string_view text) {
    kimix::llm::Message m;
    m.role = "user";
    m.content.assign(text.data(), text.size());
    return m;
}

kimix::agent::LoopControl open_lc() {
    kimix::agent::LoopControl lc;
    lc.auto_retrieve_history_threshold = 0.0;
    lc.auto_retrieve_working_memory_threshold = 0.0;
    lc.auto_retrieve_recency_memory_threshold = 0.0;
    return lc;
}

kimix::vector<kimix::agent::DynamicInjection>
run(kimix::agent::AgentSession &session, kimix::agent::LoopControl &lc,
    kimix::string_view query, int32_t step,
    kimix::set<uint32_t> *recently = nullptr) {
    kimix::set<uint32_t> local;
    kimix::agent::auto_retrieve_context ctx;
    ctx.loop_control = &lc;
    ctx.session = &session;
    ctx.query = query;
    ctx.step_no = step;
    ctx.recently_retrieved = recently != nullptr ? recently : &local;
    return kimix::agent::collect_auto_retrieval_injections(ctx);
}

bool has_type(const kimix::vector<kimix::agent::DynamicInjection> &v,
              kimix::string_view type) {
    for (const kimix::agent::DynamicInjection &inj : v) {
        if (inj.type == type) {
            return true;
        }
    }
    return false;
}

const kimix::agent::DynamicInjection *
find_type(const kimix::vector<kimix::agent::DynamicInjection> &v,
          kimix::string_view type) {
    for (const kimix::agent::DynamicInjection &inj : v) {
        if (inj.type == type) {
            return &inj;
        }
    }
    return nullptr;
}

} // namespace

int main() {
    "fires_only_on_step_1"_test = [] {
        kimix::agent::AgentSession session;
        session.append_history(user_msg("the deployment script builds artifacts"));
        kimix::agent::LoopControl lc = open_lc();
        expect(run(session, lc, "the deployment script", 2).empty());
        expect(run(session, lc, "the deployment script", 1).size() == 1u);
    };

    "query_must_be_at_least_10_chars"_test = [] {
        kimix::agent::AgentSession session;
        session.append_history(user_msg("the deployment script builds artifacts"));
        kimix::agent::LoopControl lc = open_lc();
        expect(run(session, lc, "short", 1).empty());
        expect(run(session, lc, "the deployment", 1).size() == 1u);
    };

    "all_tiers_disabled_fires_nothing"_test = [] {
        kimix::agent::AgentSession session;
        session.append_history(user_msg("the deployment script builds artifacts"));
        kimix::agent::LoopControl lc = open_lc();
        lc.auto_retrieve_history = false;
        lc.auto_retrieve_working_memory = false;
        lc.auto_retrieve_recency_memory = false;
        expect(run(session, lc, "the deployment script", 1).empty());
    };

    "working_memory_excludes_last_two_live_turns"_test = [] {
        kimix::agent::AgentSession session;
        session.append_history(user_msg("the deployment script builds artifacts")); // 0
        session.append_history(user_msg("second turn about something else entirely")); // 1
        session.append_history(user_msg("third turn follows up on the second")); // 2
        kimix::agent::LoopControl lc = open_lc();
        lc.auto_retrieve_history = false;
        lc.auto_retrieve_recency_memory = false; // isolate the working tier
        const auto injections = run(session, lc, "the deployment script", 1);
        // Turns 1+2 are the last two live turns -> only turn 0 is eligible.
        expect(injections.size() == 1u) << injections.size();
        const kimix::agent::DynamicInjection *inj =
            find_type(injections, "working_memory");
        expect(inj != nullptr);
        expect(inj->content.find("[Relevant context from our current conversation]") ==
               0u)
            << inj->content;
        expect(inj->content.find("> **user**\n> the deployment script builds artifacts") !=
               kimix::string::npos)
            << inj->content;
    };

    "citation_body_prefixes_every_line"_test = [] {
        kimix::agent::AgentSession session;
        session.append_history(user_msg("first line of the note\nsecond line here")); // 0
        session.append_history(user_msg("filler one")); // 1
        session.append_history(user_msg("filler two")); // 2
        kimix::agent::LoopControl lc = open_lc();
        lc.auto_retrieve_history = false;
        lc.auto_retrieve_recency_memory = false; // isolate the working tier
        const auto injections = run(session, lc, "first line of the note", 1);
        expect(injections.size() == 1u);
        expect(injections[0].content.find("> first line of the note\n> second line here") !=
               kimix::string::npos)
            << injections[0].content;
    };

    "dedup_against_recently_retrieved"_test = [] {
        kimix::agent::AgentSession session;
        session.append_history(user_msg("the deployment script builds artifacts")); // 0
        session.append_history(user_msg("filler one")); // 1
        session.append_history(user_msg("filler two")); // 2
        kimix::agent::LoopControl lc = open_lc();
        lc.auto_retrieve_history = false;
        lc.auto_retrieve_recency_memory = false; // isolate the working tier
        kimix::set<uint32_t> recently;
        expect(run(session, lc, "the deployment script", 1, &recently).size() == 1u);
        expect(recently.size() == 1u);
        // Same turn is not injected twice.
        expect(run(session, lc, "the deployment script", 1, &recently).empty());
    };

    "dedup_set_capped_at_ten"_test = [] {
        kimix::agent::AgentSession session;
        session.append_history(user_msg("the deployment script builds artifacts")); // 0
        session.append_history(user_msg("filler one")); // 1
        session.append_history(user_msg("filler two")); // 2
        kimix::agent::LoopControl lc = open_lc();
        lc.auto_retrieve_history = false;
        lc.auto_retrieve_recency_memory = false; // isolate the working tier
        kimix::set<uint32_t> recently;
        for (uint32_t i = 100; i < 110; ++i) {
            recently.insert(i);
        }
        expect(run(session, lc, "the deployment script", 1, &recently).size() == 1u);
        // The cap discards the smallest id while the set is above 10.
        expect(recently.size() == 10u);
        expect(recently.count(0) == 0u); // smallest id evicted
        expect(recently.count(109) == 1u);
    };

    "score_thresholds_gate_each_tier"_test = [] {
        kimix::agent::AgentSession session;
        session.append_history(user_msg("the deployment script builds artifacts")); // 0
        session.append_history(user_msg("filler one")); // 1
        session.append_history(user_msg("filler two")); // 2
        kimix::agent::LoopControl lc = open_lc();
        lc.auto_retrieve_history_threshold = 1e9; // nothing passes any tier
        lc.auto_retrieve_working_memory_threshold = 1e9;
        lc.auto_retrieve_recency_memory_threshold = 1e9;
        expect(run(session, lc, "the deployment script", 1).empty());
    };

    "token_budget_gates_injections"_test = [] {
        kimix::agent::AgentSession session;
        session.append_history(user_msg("the deployment script builds artifacts")); // 0
        session.append_history(user_msg("filler one")); // 1
        session.append_history(user_msg("filler two")); // 2
        kimix::agent::LoopControl lc = open_lc();
        lc.auto_retrieve_max_tokens_per_turn = 16; // less than one citation
        expect(run(session, lc, "the deployment script", 1).empty());
    };

    "max_injections_per_turn_caps_the_list"_test = [] {
        kimix::agent::AgentSession session;
        session.append_history(user_msg("the deployment script builds artifacts")); // 0
        session.append_history(user_msg("filler one")); // 1
        session.append_history(user_msg("filler two")); // 2
        kimix::agent::LoopControl lc = open_lc();
        lc.auto_retrieve_history = false;
        lc.auto_retrieve_recency_memory = false; // isolate the working tier
        lc.auto_retrieve_max_injections_per_turn = 1;
        const auto injections = run(session, lc, "the deployment script", 1);
        expect(injections.size() == 1u);
    };

    "long_term_memory_injects_compacted_turns"_test = [] {
        kimix::agent::AgentSession session;
        session.append_history(user_msg("the deployment script builds artifacts"));
        session.append_history(user_msg("another compacted note about deployments"));
        // Everything indexed so far is now "compacted" (pre-summary turns).
        session.history_index().mark_compacted();
        kimix::agent::LoopControl lc = open_lc();
        lc.auto_retrieve_working_memory = false;
        lc.auto_retrieve_recency_memory = false;
        const auto injections = run(session, lc, "the deployment script", 1);
        expect(injections.size() == 1u);
        expect(injections[0].type == "auto_retrieved_history");
        expect(injections[0].content.find(
                   "[Auto-retrieved from past conversation \xE2\x80\x94 relevance: ") ==
               0u)
            << injections[0].content;
    };

    "recency_memory_uses_boosted_score"_test = [] {
        kimix::agent::AgentSession session;
        session.append_history(user_msg("the deployment script builds artifacts")); // 0
        session.append_history(user_msg("filler one")); // 1
        session.append_history(user_msg("filler two")); // 2
        kimix::agent::LoopControl lc = open_lc();
        lc.auto_retrieve_history = false;
        lc.auto_retrieve_working_memory = false;
        const auto injections = run(session, lc, "the deployment script", 1);
        // Turn 0 is inside the last-2 working exclusion but the recency tier
        // (boosted score) may still pick it.
        expect(injections.size() == 1u);
        expect(injections[0].type == "recency_memory");
        expect(injections[0].content.find("[Recently discussed \xE2\x80\x94 relevance: ") ==
               0u)
            << injections[0].content;
    };

    "tiers_share_used_turn_ids"_test = [] {
        kimix::agent::AgentSession session;
        session.append_history(user_msg("the deployment script builds artifacts")); // 0
        session.append_history(user_msg("filler one")); // 1
        session.append_history(user_msg("filler two")); // 2
        kimix::agent::LoopControl lc = open_lc();
        // Compact everything: the long-term tier injects turn 0; working and
        // recency must not inject the same turn again.
        session.history_index().mark_compacted();
        const auto injections = run(session, lc, "the deployment script", 1);
        expect(injections.size() == 1u);
        expect(injections[0].type == "auto_retrieved_history");
    };

    return 0;
}
