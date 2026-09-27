// agent/auto_retrieve.h - auto-retrieval memory injection at step 1
// (report.md row D11, gap-check 05-pruning-index.md G50).
//
// Native port of kimi_cli/soul/kimisoul.py::_maybe_auto_retrieve_history
// (707-812, read in full): search the history index with recency boost before
// the first step of a turn and inject up to three distinct citations:
//   A. long-term memory   - best COMPACTED turn, raw score >=
//      auto_retrieve_history_threshold (5.0); header
//      "[Auto-retrieved from past conversation — relevance: {score:.2f}]"
//   B. working memory     - best non-compacted turn outside the last two live
//      turns, raw score >= auto_retrieve_working_memory_threshold (5.0);
//      header "[Relevant context from our current conversation]"
//   C. recency memory     - best remaining turn by BOOSTED score >=
//      auto_retrieve_recency_memory_threshold (4.0); header
//      "[Recently discussed — relevance: {boosted:.2f}]"
// Shared rules: query >= 10 chars, top_k=10 candidate pool, <=3 injections,
// <= auto_retrieve_max_tokens_per_turn (20000) tokens per turn (each citation
// costs its token estimate + a 15-token wrapper overhead), dedup against the
// previously injected turn ids (set capped at 10, oldest evicted), and the
// fixed citation body "> **{role}**\n> {text}".
//
// Errors never propagate: an index failure degrades to no injections (the
// reference try/except around search_with_recency).
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "agent/dynamic_injection.h"
#include "agent/loop_control.h"

namespace kimix::agent {

class AgentSession; // agent/soul.h

// One auto-retrieval decision's inputs.
struct auto_retrieve_context {
    const LoopControl *loop_control = nullptr; // the prune/auto-retrieve knobs
    AgentSession *session = nullptr;           // the history index dispatch
    kimix::string_view query;                  // the current turn's user text
    int32_t step_no = 0;                       // fires on step 1 only
    // The session's dedup set (kimisoul._recently_retrieved_turn_ids): turn
    // ids injected by recent auto-retrievals. The callee inserts the ids it
    // injects and keeps the set bounded to the last 10 (turn ids increase
    // monotonically, so the smallest id is the oldest).
    kimix::set<uint32_t> *recently_retrieved = nullptr;
};

// kimisoul.py:707-812. Returns 0..max_injections DynamicInjections with types
// "auto_retrieved_history" / "working_memory" / "recency_memory", auto-
// retrieval first (the caller prepends them before the provider injections).
kimix::vector<DynamicInjection>
collect_auto_retrieval_injections(const auto_retrieve_context &ctx);

} // namespace kimix::agent
