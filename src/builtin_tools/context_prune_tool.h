// builtin_tools/context_prune_tool.h - the context_prune agent tool
// (report.md row D2, gap-check 05-pruning-index.md G02).
//
// Native port of kimi-cli/src/kimi_cli/tools/context_prune.py (read in full):
// manual session-content removal with three modes -
//   * "prune"           - policy-driven ephemeral/substantive elision via
//                         ContextPruner::prune_with_policy (D1)
//   * "compact"         - delegates to KimiSoul::compact_context (manual)
//   * "strip_reasoning" - clears old assistant thinking outside the protected
//                         tail, keeping an empty thinking part when the
//                         provider requires the reasoning back-pass
// Six parameters (mode, target_token_count >= 1000, remove_reasoning,
// remove_tool_results, keep_recent_turns 1..20, dry_run), the reference's
// structural validation refusals, the markdown summary, the subagent gate
// (loop_control.prune_subagents) and the StatusUpdate wire refresh after a
// real prune (G41, emitted inside KimiSoul::apply_pruned_history).
//
// The soul binding mirrors the reference's context_prune(soul) registration:
// the KimiSoul publishes itself on builtin_tools::Session::agent_soul, and
// valid() answers false when no soul is bound (the analogue of "the toolset
// is not a KimiToolset"), so the model never sees a tool that cannot run.
//
// Rules (see .agents/skills/cpp): namespace kimix::builtin_tools::context_prune,
// no RTTI, exception-free.

#pragma once

#include <core/kimix_core.h>

#include "builtin_tools/tool.h"

namespace kimix::builtin_tools::context_prune {

class ContextPrune : public kimix::builtin_tools::Tool {
public:
    explicit ContextPrune(kimix::builtin_tools::Session *session);
    // True when a soul is bound to this session (the reference registers the
    // tool only on a KimiToolset; the native factory only sees the Session).
    bool valid() const override;
    void operator()(kimix::builtin_tools::ToolParams const *parameters,
                    kimix::string &display_str) override;
    void result_json(kimix::vector<char> &out) const override { out = _last_result; }

private:
    kimix::vector<char> _last_result;
};

} // namespace kimix::builtin_tools::context_prune
