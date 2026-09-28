// agent/agent_host.h - Production sub-agent runner installation (G7).
//
// The agent tools (src/builtin_tools/agent_tool.h) execute sub-agents through
// an INJECTABLE `subagent_runner`; until now only the (since removed)
// new_tools_e2e demo installed one, so in production
// send_message's push_steer/drain_steer path was never drained (gap G12's
// "no host that drains it"). install_subagent_runner() closes that gap: it
// installs a KimiSoul-backed runner on the session's agent_registry so
// background Agent tool calls run real turns, where
//   * the runner feeds registry.drain_steer(session_id) through the soul's
//     external steer source - a send_message to a RUNNING child is injected
//     as a follow-up user message between steps (the reference's Steer
//     delivery to a sub-agent loop), with mid-stream wake on request_steer;
//   * interrupt_agent's cancel flag chains into the child turn's CancelToken
//     (run_soul's cancel_event), aborting the child at the next step boundary
//     and interrupting its in-flight request;
//   * a shared mutex serializes the parent and child backend calls when the
//     parent is mid-request on the same IChatBackend.
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers,
// no exceptions, no RTTI.

#pragma once

#include <core/kimix_core.h>

#include "agent/approval.h"
#include "agent/soul.h"
#include "builtin_tools/agent_tool.h"

namespace kimix::agent {

// Install the KimiSoul-backed runner on `registry`. `backend` must outlive
// the registry's runs (borrow the app's backend). `opts_template` seeds the
// child souls (its system_prompt / enabled_tools / limits are reused as-is).
// `approval` (nullable) is the parent's approval gate: every child soul shares
// it (approval.py share()), so sub-agent tool calls hit the same gate and the
// same approve-for-session grant set.
void install_subagent_runner(builtin_tools::agents::agent_registry &registry,
                             IChatBackend &backend,
                             const KimiSoul::options &opts_template,
                             Approval *approval = nullptr);

} // namespace kimix::agent
