// cli/cli_tools.h - Agent-manifest tool-path resolution for the native CLI.
//
// The five KimiX role manifests (C:/dev/kimi-agent/src/kimix/agent_*.json) list
// each tool as a Python "<module.path>:<attr>" string (agent_worker.json,
// agent_boss.json, agent_planner.json, agent_readonly.json, agent_subagent.json).
// The Python CLI resolves them dynamically with importlib + a class scan; the
// native CLI instead maps every distinct path to the C++ tool class registered
// in kimix::builtin_tools::ToolRegistry (see src/builtin_tools/tool_registry.h).
// The pair table is PLAN.md §4; its counterpart (the same 25 entries, same
// order) is the k_agent_tools array in the
// tests/unit/builtin_tools/test_tool.cpp "registry_covers_every_agent_json_tool"
// test, which validates that every entry resolves to a real registered class.
//
// Rules (see src/cli/PLAN.md and .agents/skills/cpp): namespace kimix::cli,
// kimix:: containers, no RTTI, no exceptions; unity build (batch 8) so every
// TU-local helper is static / in an anonymous namespace with the `clit_` prefix.

#pragma once

#include <utility>

#include <core/kimix_core.h>

namespace kimix::cli {

// Resolve one manifest tool path ("<module>:<attr>") to its built-in tool
// registry name ("Bash", "Read", ...).  The path is split on the LAST ':'; both
// halves must be non-empty and the resulting pair must appear in
// agent_tool_table().  Returns "" when the path is malformed or unknown (the
// caller records a warning and drops the tool) - it never fails hard.
kimix::string resolve_tool_path(kimix::string_view tool_path);

// The built-in default agent's tool list: the 25 registry names of the
// agent_*.json union, in agent_tool_table() order (PLAN.md §4).  Used when an
// agent manifest sets extend:"default" without listing its own tools.
const kimix::vector<kimix::string> &default_agent_tools();

// Every distinct "<module>:<attr>" string used by the five agent_*.json
// manifests, paired with its registry name, in manifest order (PLAN.md §4).
// Counterpart: tests/unit/builtin_tools/test_tool.cpp (k_agent_tools).
const kimix::vector<std::pair<kimix::string, kimix::string>> &agent_tool_table();

} // namespace kimix::cli
