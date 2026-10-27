// cli/cli_tools.cpp - Agent-manifest tool-path resolution (see cli_tools.h).
//
// Port of the Python resolution rule in kimi_cli/soul/toolset.py + tools/
// __init__.py: a "<module>:<attr>" string is looked up by its (module, attr)
// pair.  The native CLI keeps the union as a static table because the C++
// tools are ordinary registered classes (ToolRegistry), not importable
// modules.  See src/cli/PLAN.md §4.
//
// The Python package renamed its tool modules from `kimix.tools.*` to
// `kimi_cli.tools.*` (and `note:WritePlan` -> `note:write_plan`,
// `agent:*` -> `subagent:*`, `web.fetch_url:*` -> `web.kimix_fetch:*`), so
// k_clit_agent_tools holds the CURRENT reference spellings and
// k_clit_agent_tool_aliases keeps the pre-rename spellings resolvable for
// manifests and configs written against the older reference.  Only the
// canonical table feeds default_agent_tools(), so the alias rows can never
// duplicate a registry name in the built-in default tool list.
//
// Unity build: the only TU-local data lives in an anonymous namespace with the
// `clit_` prefix (never at namespace scope with a bare name), so concatenating
// this file with the other cli/*.cpp units cannot collide.

#include "cli/cli_tools.h"

#include <utility>

namespace kimix::cli {
namespace src_cli_cli_tools {

    // The 21 (manifest tool path, registry name) pairs of the agent_*.json union,
    // in the CURRENT reference spelling.  This is the production copy of the
    // table in tests/unit/builtin_tools/test_tool.cpp
    // ("registry_covers_every_agent_json_tool"); keep the two in sync.  Order ==
    // PLAN.md §4 (the union, most-specific role first), which is also the order
    // default_agent_tools() reports.
    struct clit_agent_tool_entry {
        const char *path; // "<module.path>:<attr>" as written in the JSON
        const char *registry_name; // ToolRegistry key (lowercase canonical name)
    };

    const clit_agent_tool_entry k_clit_agent_tools[] = {
        {"kimi_cli.tools.file:read", "read"},
        {"kimi_cli.tools.file:read_image", "read_image"},
        {"kimi_cli.tools.file:glob", "glob"},
        {"kimi_cli.tools.file:grep", "grep"},
        {"kimi_cli.tools.file:edit", "edit"},
        {"kimi_cli.tools.file:write", "write"},
        {"kimi_cli.tools.web.kimix_fetch:fetch_url", "fetch_url"},
        {"kimi_cli.tools.web:web_search", "web_search"},
        {"kimi_cli.tools.note:write_plan", "writeplan"},
        {"kimi_cli.tools.note:read_plan", "readplan"},
        {"kimi_cli.tools.note:edit_plan", "editplan"},
        {"kimi_cli.tools.subagent:subagent", "subagent"},
        {"kimi_cli.tools.subagent:list_agents", "list_agents"},
        {"kimi_cli.tools.subagent:interrupt_agent", "interrupt_agent"},
        {"kimi_cli.tools.swarm:workflow", "workflow"},
        {"kimi_cli.tools.todo:todo_list", "todo_list"},
        {"kimi_cli.tools.context:compact", "compact"},
        {"kimi_cli.tools.file.bash:bash", "bash"},
        {"kimi_cli.tools.file.bash:pwsh", "pwsh"},
        {"kimi_cli.tools.py:python", "python"},
        {"kimi_cli.tools.background:job_output", "job_output"},
    };

    // Pre-rename spellings the older reference manifests/configs still carry.
    // Resolve-only: these paths never appear in default_agent_tools().
    const clit_agent_tool_entry k_clit_agent_tool_aliases[] = {
        // The reference dropped memory:retrieve from every role manifest (the
        // memory is injected by auto_retrieve instead), so it is no longer part
        // of the union; the tool itself stays registered and an older manifest
        // that still names it keeps working.
        {"kimi_cli.tools.memory:retrieve", "retrieve"},
        {"kimix.tools.web.fetch_url:fetch_url", "fetch_url"},
        {"kimix.tools.note:WritePlan", "writeplan"},
        {"kimix.tools.note:ReadPlan", "readplan"},
        {"kimix.tools.note:EditPlan", "editplan"},
        {"kimix.tools.agent:subagent", "subagent"},
        {"kimix.tools.agent:list_agents", "list_agents"},
        {"kimix.tools.agent:interrupt_agent", "interrupt_agent"},
        {"kimix.tools.swarm:workflow", "workflow"},
        {"kimix.tools.context:compact", "compact"},
        {"kimix.tools.file.bash:bash", "bash"},
        {"kimix.tools.file.bash:pwsh", "pwsh"},
        {"kimix.tools.py:python", "python"},
        {"kimix.tools.background:job_output", "job_output"},
    };

    // Linear scan over one table; "" when the exact path is absent.
    kimix::string clit_lookup(const clit_agent_tool_entry *entries, size_t count,
                              kimix::string_view tool_path) {
        for (size_t i = 0; i < count; ++i) {
            if (kimix::string_view(entries[i].path) == tool_path) {
                return kimix::string(entries[i].registry_name);
            }
        }
        return {};
    }

} // namespace src_cli_cli_tools
using namespace src_cli_cli_tools;

const kimix::vector<std::pair<kimix::string, kimix::string>> &agent_tool_table() {
    static const kimix::vector<std::pair<kimix::string, kimix::string>> table = [] {
        const size_t count = sizeof(k_clit_agent_tools) / sizeof(k_clit_agent_tools[0]);
        kimix::vector<std::pair<kimix::string, kimix::string>> t;
        t.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            t.emplace_back(k_clit_agent_tools[i].path,
                           k_clit_agent_tools[i].registry_name);
        }
        return t;
    }();
    return table;
}

kimix::string resolve_tool_path(kimix::string_view tool_path) {
    // Split on the LAST ':' (Python rsplit(":", 1)); both halves must be
    // non-empty, otherwise the entry is malformed and resolves to nothing.
    const size_t pos = tool_path.rfind(':');
    if (pos == kimix::string_view::npos || pos == 0 || pos + 1 >= tool_path.size()) {
        return {};
    }
    // Canonical (current reference) spellings first, then the pre-rename
    // aliases so an older manifest keeps working.
    kimix::string registry = clit_lookup(k_clit_agent_tools,
                                         sizeof(k_clit_agent_tools) /
                                             sizeof(k_clit_agent_tools[0]),
                                         tool_path);
    if (!registry.empty()) {
        return registry;
    }
    return clit_lookup(k_clit_agent_tool_aliases,
                       sizeof(k_clit_agent_tool_aliases) /
                           sizeof(k_clit_agent_tool_aliases[0]),
                       tool_path);
}

const kimix::vector<kimix::string> &default_agent_tools() {
    static const kimix::vector<kimix::string> names = [] {
        kimix::vector<kimix::string> v;
        for (const std::pair<kimix::string, kimix::string> &entry : agent_tool_table()) {
            v.push_back(entry.second);
        }
        return v;
    }();
    return names;
}

} // namespace kimix::cli
