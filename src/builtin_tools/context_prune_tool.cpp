// builtin_tools/context_prune_tool.cpp - implementation (see context_prune_tool.h).
//
// Reference: kimi-cli/src/kimi_cli/tools/context_prune.py (read in full).
// Every user-visible literal (refusal texts, the summary bullets, the
// dry-run/completion outputs) is byte-exact vs the reference.

#include "builtin_tools/context_prune_tool.h"

#include <cstdio>

#include "agent/soul.h"

namespace kimix::builtin_tools::context_prune {

namespace {

// Parsed mode of one call.
enum class mode_kind : uint8_t { prune, compact, strip_reasoning };

struct cp_params {
    mode_kind mode = mode_kind::prune;
    kimix::optional<int64_t> target_token_count;
    bool remove_reasoning = true;
    bool remove_tool_results = true;
    int32_t keep_recent_turns = 6;
    bool dry_run = false;
};

// Fuzzy argument aliases (tool.h): wrong-but-reasonable LLM spellings.
constexpr const kimix::builtin_tools::param_alias k_cp_aliases[] = {
    {"mode", "strategy"},
    {"target_token_count", "target tokens target_tokens max_tokens"},
    {"remove_reasoning", "strip_reasoning remove_thinking"},
    {"remove_tool_results", "remove_tool_output drop_tool_results"},
    {"keep_recent_turns", "keep_recent keep_turns recent_turns"},
    {"dry_run", "preview dryrun"},
};

kimix::string mode_name(mode_kind m) {
    switch (m) {
    case mode_kind::compact:
        return kimix::string("compact");
    case mode_kind::strip_reasoning:
        return kimix::string("strip_reasoning");
    default:
        return kimix::string("prune");
    }
}

// Serialize one result payload. `status` is the tool_status string the soul's
// envelope builder maps: "ok" -> success; anything else -> <system>ERROR.
void write_result(kimix::vector<char> &out, bool ok, kimix::string_view status,
                  kimix::string_view message, kimix::string_view brief,
                  kimix::string_view output) {
    kimix::builtin_tools::ToolParams result;
    result.values["ok"] = ValueElement::make_bool(ok);
    result.values["status"] = ValueElement::make_string(kimix::string(status));
    result.values["message"] = ValueElement::make_string(kimix::string(message));
    result.values["brief"] = ValueElement::make_string(kimix::string(brief));
    result.values["output"] = ValueElement::make_string(kimix::string(output));
    result.serialize(out);
}

// _build_summary (context_prune.py:390-425): the markdown summary shared by
// the dry-run and applied answers.
kimix::string build_summary(kimix::string_view mode, size_t before, size_t after,
                            const kimix::agent::pruning_result &result) {
    kimix::string out;
    out += "- **Mode:** ";
    out.append(mode.data(), mode.size());
    out += "\n- **Messages before:** ";
    out += std::to_string(static_cast<long long>(before)).c_str();
    out += "\n- **Messages after:** ";
    out += std::to_string(static_cast<long long>(after)).c_str();
    out += "\n- **Messages changed/dropped:** ";
    out += std::to_string(
               static_cast<long long>(before - after + result.elided.size()))
               .c_str();
    out += "\n- **Estimated tokens freed:** ";
    out += std::to_string(static_cast<long long>(result.freed_tokens)).c_str();
    if (result.earliest_removed_index.has_value()) {
        out += "\n- **Earliest changed index:** ";
        out += std::to_string(static_cast<long long>(*result.earliest_removed_index))
                   .c_str();
    }
    out += "\n- **Elided references:** ";
    if (result.elided.empty()) {
        out += "none";
    } else {
        for (size_t i = 0; i < result.elided.size(); ++i) {
            if (i > 0) {
                out += ", ";
            }
            out += '`';
            out += result.elided[i].ref;
            out += '`';
        }
    }
    return out;
}

// _run_strip_reasoning (context_prune.py:251-315): clear the thinking of
// assistant messages outside the protected set. When the provider requires
// the reasoning back-pass the thinking part is kept as an empty string.
kimix::agent::pruning_result
run_strip_reasoning(kimix::agent::KimiSoul &soul,
                    const kimix::vector<kimix::llm::Message> &history,
                    const cp_params &params) {
    using namespace kimix::agent;
    const kimix::set<int64_t> protected_indices = protect_tool_pair_indices(
        history,
        compute_protected_indices(history, soul.pruner().options().stable_prefix_messages,
                                  params.keep_recent_turns,
                                  current_turn_start_index(history)));
    const bool thinking_active = soul.thinking_active();
    pruning_result result;
    result.messages.reserve(history.size());
    kimix::set<int64_t> changed;
    for (int64_t i = 0; i < static_cast<int64_t>(history.size()); ++i) {
        const kimix::llm::Message &msg = history[static_cast<size_t>(i)];
        if (msg.role != "assistant" || protected_indices.count(i) != 0 ||
            msg.thinking.empty()) {
            result.messages.push_back(msg);
            continue;
        }
        result.freed_tokens += std::max<int64_t>(static_cast<int64_t>(msg.thinking.size()) / 4, 1);
        changed.insert(i);
        kimix::llm::Message copy = msg;
        if (thinking_active) {
            copy.thinking.clear(); // ThinkPart(think="") back-pass invariant
        } else {
            copy.thinking.clear();
        }
        result.messages.push_back(std::move(copy));
    }
    if (!changed.empty()) {
        result.earliest_removed_index = *changed.begin();
    }
    return result;
}

} // namespace

ContextPrune::ContextPrune(kimix::builtin_tools::Session *session)
    : kimix::builtin_tools::Tool(session) {}

bool ContextPrune::valid() const {
    // The reference registers context_prune only when the toolset is a
    // KimiToolset; the native factory only sees the Session, so the soul
    // binding is the availability probe.
    const bool soul_bound = _session != nullptr && _session->agent_soul != nullptr;
    return tool_valid("context_prune", soul_bound);
}

void ContextPrune::operator()(kimix::builtin_tools::ToolParams const *parameters,
                              kimix::string &display_str) {
    const kimix::builtin_tools::tool_display_scope k_display{
        *this, display_str};
    _last_result.clear();
    using namespace kimix::agent;
    kimix::builtin_tools::ToolParams resolved =
        kimix::builtin_tools::ToolParams::with_aliases(parameters, k_cp_aliases);
    if (parameters != nullptr) {
        parameters = &resolved;
    }

    cp_params params;
    kimix::string parse_error;
    do {
        if (parameters == nullptr) {
            break;
        }
        if (const ValueElement *mode = parameters->get("mode")) {
            if (!mode->is_string()) {
                parse_error = "mode must be a string";
                break;
            }
            const kimix::string &m = mode->as_string();
            if (m == "compact") {
                params.mode = mode_kind::compact;
            } else if (m == "strip_reasoning") {
                params.mode = mode_kind::strip_reasoning;
            } else if (m == "prune") {
                params.mode = mode_kind::prune;
            } else {
                parse_error = "mode must be one of: prune, compact, strip_reasoning";
                break;
            }
        }
        if (const ValueElement *t = parameters->get("target_token_count")) {
            if (t->is_int()) {
                params.target_token_count = t->as_int();
            } else if (!t->is_null()) {
                parse_error = "target_token_count must be an integer";
                break;
            }
        }
        if (const ValueElement *b = parameters->get("remove_reasoning")) {
            if (b->is_bool()) {
                params.remove_reasoning = b->as_bool();
            }
        }
        if (const ValueElement *b = parameters->get("remove_tool_results")) {
            if (b->is_bool()) {
                params.remove_tool_results = b->as_bool();
            }
        }
        if (const ValueElement *k = parameters->get("keep_recent_turns")) {
            if (k->is_int()) {
                params.keep_recent_turns = static_cast<int32_t>(k->as_int());
            } else if (!k->is_null()) {
                parse_error = "keep_recent_turns must be an integer";
                break;
            }
        }
        if (const ValueElement *b = parameters->get("dry_run")) {
            if (b->is_bool()) {
                params.dry_run = b->as_bool();
            }
        }
    } while (false);
    if (!parse_error.empty()) {
        write_result(_last_result, /*ok=*/false, "invalid_input", parse_error,
                     "Invalid arguments", "");
        return;
    }

    std::fprintf(stderr, "context_prune invoked: mode=%s, keep_recent_turns=%d, "
                         "target_token_count=%s, dry_run=%d\n",
                 mode_name(params.mode).c_str(), params.keep_recent_turns,
                 params.target_token_count.has_value()
                     ? std::to_string(*params.target_token_count).c_str()
                     : "none",
                 params.dry_run ? 1 : 0);

    KimiSoul *soul = _session != nullptr ? _session->agent_soul : nullptr;
    if (soul == nullptr) {
        write_result(_last_result, /*ok=*/false, "unsupported",
                     "context_prune is not available in this session (no soul bound)",
                     "No soul bound", "");
        return;
    }

    // Subagent gate (context_prune.py:114-124), matching the auto-pass gate.
    const LoopControl &lc = soul->loop_control();
    if (_session->is_sub_agent && !lc.prune_subagents) {
        write_result(_last_result, /*ok=*/false, "blocked",
                     "context_prune is not enabled for subagents. Enable "
                     "loop_control.prune_subagents or run from the root session.",
                     "Subagent pruning disabled", "");
        return;
    }

    const kimix::vector<kimix::llm::Message> history = soul->session().history();
    const int64_t max_context_size = soul->max_context_size();

    // Structural validation (context_prune.py:321-379).
    if (params.target_token_count.has_value() && *params.target_token_count < 1000) {
        write_result(_last_result, /*ok=*/false, "invalid_input",
                     "target_token_count must be >= 1000", "Invalid target", "");
        return;
    }
    if (params.keep_recent_turns < 1 || params.keep_recent_turns > 20) {
        write_result(_last_result, /*ok=*/false, "invalid_input",
                     "keep_recent_turns must be between 1 and 20",
                     "Invalid keep_recent_turns", "");
        return;
    }
    const int64_t n = static_cast<int64_t>(history.size());
    const int64_t stable_prefix = soul->pruner().options().stable_prefix_messages;
    if (params.keep_recent_turns > std::max<int64_t>(0, n - stable_prefix)) {
        kimix::string msg = "keep_recent_turns=";
        msg += std::to_string(params.keep_recent_turns).c_str();
        msg += " is too large: history has ";
        msg += std::to_string(n).c_str();
        msg += " messages and ";
        msg += std::to_string(stable_prefix).c_str();
        msg += " are protected as a stable prefix.";
        write_result(_last_result, /*ok=*/false, "invalid_input", msg,
                     "Invalid keep_recent_turns", "");
        return;
    }
    if (params.target_token_count.has_value()) {
        const kimix::set<int64_t> protected_indices = protect_tool_pair_indices(
            history,
            compute_protected_indices(history,
                                      static_cast<int32_t>(stable_prefix),
                                      params.keep_recent_turns,
                                      current_turn_start_index(history)));
        kimix::vector<kimix::llm::Message> protected_messages;
        for (const int64_t idx : protected_indices) {
            if (idx >= 0 && idx < n) {
                protected_messages.push_back(history[static_cast<size_t>(idx)]);
            }
        }
        const int64_t protected_tokens = estimate_history_tokens(protected_messages);
        if (*params.target_token_count < protected_tokens) {
            kimix::string msg = "target_token_count=";
            msg += std::to_string(*params.target_token_count).c_str();
            msg += " is below the ";
            msg += std::to_string(protected_tokens).c_str();
            msg += " tokens required by the protected prefix/recent turns. "
                   "Increase the target or reduce keep_recent_turns.";
            write_result(_last_result, /*ok=*/false, "invalid_input", msg,
                         "Target too low", "");
            return;
        }
    }
    if (params.mode == mode_kind::prune) {
        int64_t user_assistant = 0;
        for (const kimix::llm::Message &m : history) {
            if (m.role == "user" || m.role == "assistant") {
                ++user_assistant;
            }
        }
        if (user_assistant <= 2) {
            write_result(_last_result, /*ok=*/false, "invalid_input",
                         "Refusing to prune: the history contains only one "
                         "user/assistant pair. Pruning would leave the "
                         "conversation empty.",
                         "History too short", "");
            return;
        }
    }

    if (params.mode == mode_kind::compact) {
        // _run_compact (context_prune.py:231-249): LLM-driven; a true preview
        // would need an LLM call.
          if (params.dry_run) {
              write_result(_last_result, /*ok=*/true, "ok", "Compact dry run",
                           "Compact dry run",
                           "**Dry run** \xE2\x80\x94 session unchanged.\n\nMode 'compact' "
                           "would invoke the compaction subsystem. A precise "
                           "preview requires an LLM call; run without dry_run "
                           "to compact.");
              return;
          }
        kimix::string cerr;
        if (!soul->compact_context("", cerr, /*manual=*/true)) {
            write_result(_last_result, /*ok=*/false, "failed",
                         cerr.empty() ? kimix::string_view("compaction failed")
                                      : kimix::string_view(cerr),
                         "Compaction failed", "");
            return;
        }
          write_result(_last_result, /*ok=*/true, "ok", "Context compacted",
                       "Context compacted", "Context compacted successfully.");
          return;
    }

    pruning_result result;
    if (params.mode == mode_kind::strip_reasoning) {
        result = run_strip_reasoning(*soul, history, params);
    } else {
        prune_policy_call pc;
        pc.remove_reasoning = params.remove_reasoning;
        pc.remove_tool_results = params.remove_tool_results;
        pc.keep_recent_turns = params.keep_recent_turns;
        pc.target_token_count = params.target_token_count;
        pc.max_context_size = max_context_size;
        pc.current_step = soul->current_step_no();
        pc.current_turn_index = current_turn_start_index(history);
        pc.alloc_ref = [soul] { return soul->alloc_prune_ref(); };
        result = soul->pruner().prune_with_policy(history, pc);
    }

    const kimix::string mode = mode_name(params.mode);
      if (!result.earliest_removed_index.has_value()) {
          kimix::string out = "context_prune (";
          out += mode;
          out += "): no removable content found.";
          write_result(_last_result, /*ok=*/true, "ok", "Nothing to prune",
                       "Nothing to prune", out);
          return;
      }

    const kimix::string summary =
        build_summary(mode, history.size(), result.messages.size(), result);

      if (params.dry_run) {
          std::fprintf(stderr, "context_prune dry-run: %s\n", summary.c_str());
          kimix::string out = "**Dry run** \xE2\x80\x94 session unchanged.\n\n";
          out += summary;
          write_result(_last_result, /*ok=*/true, "ok", "Dry run complete",
                       "Dry run complete", out);
          return;
      }

    // Persist the pruned history + archive elided originals + StatusUpdate
    // (context_prune.py:186-211, applied inside the soul).
    soul->apply_pruned_history(result.messages, result.elided);

    std::fprintf(stderr, "context_prune applied: mode=%s, freed=%lld, earliest=%lld, "
                         "elided=%zu\n",
                 mode.c_str(), static_cast<long long>(result.freed_tokens),
                 static_cast<long long>(*result.earliest_removed_index),
                 result.elided.size());

      kimix::string out = "context_prune (";
      out += mode;
      out += ") applied.\n\n";
      out += summary;
      write_result(_last_result, /*ok=*/true, "ok", "Context pruned",
                   "Context pruned", out);
                   // CLI display line: the mode plus what it freed (the summary
                   // text stays in the payload).
                   display_str = tool_display_join(
                       {mode, kimix::format("freed {} tokens, {} rows elided",
                                             result.freed_tokens, result.elided.size())});
  }

} // namespace kimix::builtin_tools::context_prune
