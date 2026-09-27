// agent/auto_retrieve.cpp - implementation (see auto_retrieve.h).

#include "agent/auto_retrieve.h"

#include <cstdio>

#include <builtin_tools/compact_tool.h>
#include "agent/soul.h"

namespace kimix::agent {

namespace {

// Tokens for the <system-reminder> tags and newlines around one citation
// (kimisoul.py:753).
constexpr int64_t k_wrapper_overhead_tokens = 15;

const char *turn_role_name(uint8_t role) noexcept {
    switch (role) {
    case 0:
        return "user";
    case 1:
        return "assistant";
    case 2:
        return "tool";
    default:
        return "unknown";
    }
}

// Fixed two-decimal render for the relevance literal ("{x:.2f}").
kimix::string score2(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return kimix::string(buf);
}

// "> **{role}**\n> {text}" with every newline of the text prefixed "> ".
kimix::string citation_body(uint8_t role, const kimix::string &text) {
    kimix::string out = "> **";
    out += turn_role_name(role);
    out += "**\n> ";
    for (const char c : text) {
        out += c;
        if (c == '\n') {
            out += "> ";
        }
    }
    return out;
}

// One injected turn's bookkeeping: ordered id list (for the cap truncation)
// plus a lookup set.
struct used_turns {
    kimix::vector<uint32_t> ordered;
    kimix::set<uint32_t> set;
    bool contains(uint32_t id) const { return set.count(id) != 0; }
    void add(uint32_t id) {
        ordered.push_back(id);
        set.insert(id);
    }
    void truncate(size_t n) {
        if (ordered.size() <= n) {
            return;
        }
        ordered.resize(n);
        set.clear();
        for (const uint32_t id : ordered) {
            set.insert(id);
        }
    }
};

} // namespace

kimix::vector<DynamicInjection>
collect_auto_retrieval_injections(const auto_retrieve_context &ctx) {
    kimix::vector<DynamicInjection> injections;
    const LoopControl &lc = *ctx.loop_control;
    if (!(lc.auto_retrieve_history || lc.auto_retrieve_working_memory ||
          lc.auto_retrieve_recency_memory)) {
        return injections;
    }
    if (ctx.step_no != 1 || ctx.session == nullptr ||
        ctx.recently_retrieved == nullptr) {
        return injections;
    }
    const kimix::string_view query = ctx.query;
    if (query.size() < 10) {
        return injections;
    }

    // search_with_recency; any index error degrades to no injections
    // (kimisoul.py:735-743 wraps the search in try/except).
    const kimix::vector<kimix::runtime::index::turn_meta> results =
        ctx.session->history_search_with_recency(query, 10,
                                                 lc.auto_retrieve_recency_weight);

    // Deduplicate against recently retrieved turn ids.
    kimix::vector<kimix::runtime::index::turn_meta> candidates;
    for (const kimix::runtime::index::turn_meta &r : results) {
        if (ctx.recently_retrieved->count(r.turn_id) == 0) {
            candidates.push_back(r);
        }
    }

    used_turns used;
    int64_t spent = 0;
    const int64_t budget = lc.auto_retrieve_max_tokens_per_turn;
    const auto can_afford = [&](const kimix::string &citation) {
        const int64_t cost =
            builtin_tools::compact::estimate_text_tokens(citation) +
            k_wrapper_overhead_tokens;
        if (spent + cost <= budget) {
            spent += cost;
            return true;
        }
        return false;
    };

    // ── A. Long-term memory (best compacted turn) ────────────────────────────
    if (lc.auto_retrieve_history) {
        for (const kimix::runtime::index::turn_meta &r : candidates) {
            if (!r.is_compacted || used.contains(r.turn_id)) {
                continue;
            }
            if (r.score >= lc.auto_retrieve_history_threshold) {
                kimix::string citation =
                    "[Auto-retrieved from past conversation \xE2\x80\x94 relevance: ";
                citation += score2(r.score);
                citation += "]\n";
                citation += citation_body(r.role, r.text);
                if (can_afford(citation)) {
                    used.add(r.turn_id);
                    DynamicInjection inj;
                    inj.type = "auto_retrieved_history";
                    inj.content = std::move(citation);
                    injections.push_back(std::move(inj));
                }
            }
            break; // pool-ordered: only the first compacted candidate matters
        }
    }

    // ── B. Working memory (best non-compacted turn outside the last 2 live) ──
    if (lc.auto_retrieve_working_memory) {
        const kimix::vector<uint32_t> live = ctx.session->non_compacted_turn_ids();
        kimix::set<uint32_t> recent_turn_ids;
        if (live.size() >= 2) {
            recent_turn_ids.insert(live[live.size() - 2]);
            recent_turn_ids.insert(live[live.size() - 1]);
        } else {
            for (const uint32_t id : live) {
                recent_turn_ids.insert(id);
            }
        }
        for (const kimix::runtime::index::turn_meta &r : candidates) {
            if (r.is_compacted || used.contains(r.turn_id) ||
                recent_turn_ids.count(r.turn_id) != 0) {
                continue;
            }
            if (r.score >= lc.auto_retrieve_working_memory_threshold) {
                kimix::string citation = "[Relevant context from our current conversation]\n";
                citation += citation_body(r.role, r.text);
                if (can_afford(citation)) {
                    used.add(r.turn_id);
                    DynamicInjection inj;
                    inj.type = "working_memory";
                    inj.content = std::move(citation);
                    injections.push_back(std::move(inj));
                }
            }
            break;
        }
    }

    // ── C. Recency memory (best boosted score, any compaction state) ─────────
    if (lc.auto_retrieve_recency_memory) {
        for (const kimix::runtime::index::turn_meta &r : candidates) {
            if (used.contains(r.turn_id)) {
                continue;
            }
            const double boosted = r.boosted_score != 0.0 ? r.boosted_score : r.score;
            if (boosted >= lc.auto_retrieve_recency_memory_threshold) {
                kimix::string citation = "[Recently discussed \xE2\x80\x94 relevance: ";
                citation += score2(boosted);
                citation += "]\n";
                citation += citation_body(r.role, r.text);
                if (can_afford(citation)) {
                    used.add(r.turn_id);
                    DynamicInjection inj;
                    inj.type = "recency_memory";
                    inj.content = std::move(citation);
                    injections.push_back(std::move(inj));
                }
            }
            break;
        }
    }

    // Respect the per-turn cap (kimisoul.py:869-873).
    const size_t max_inj =
        static_cast<size_t>(lc.auto_retrieve_max_injections_per_turn);
    if (injections.size() > max_inj) {
        injections.resize(max_inj);
        used.truncate(max_inj);
    }

    // Update the dedup tracking, bounded to the last ~10 ids (the ids are
    // monotonically increasing, so the smallest is the oldest).
    for (const uint32_t id : used.ordered) {
        ctx.recently_retrieved->insert(id);
    }
    while (ctx.recently_retrieved->size() > 10) {
        ctx.recently_retrieved->erase(ctx.recently_retrieved->begin());
    }
    return injections;
}

} // namespace kimix::agent
