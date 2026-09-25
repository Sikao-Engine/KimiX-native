// cli/cli_tools.cpp - Agent-manifest tool-path resolution (see cli_tools.h).
//
// Port of the Python resolution rule in kimi_cli/soul/toolset.py + tools/
// __init__.py: a "<module>:<attr>" string is looked up by its (module, attr)
// pair.  The native CLI keeps the union as a static table because the C++
// tools are ordinary registered classes (ToolRegistry), not importable
// modules.  See src/cli/PLAN.md §4.
//
// Unity build: the only TU-local data lives in an anonymous namespace with the
// `clit_` prefix (never at namespace scope with a bare name), so concatenating
// this file with the other cli/*.cpp units cannot collide.

#include "cli/cli_tools.h"

#include <utility>

namespace kimix::cli {

namespace {

// The 25 (manifest tool path, registry name) pairs of the agent_*.json union.
// This is the production copy of the table in
// tests/unit/builtin_tools/test_tool.cpp ("registry_covers_every_agent_json_tool");
// keep the two in sync.  Order == PLAN.md §4 (the union, most-specific role
// first), which is also the order default_agent_tools() reports.
struct clit_agent_tool_entry {
    const char *path;          // "<module.path>:<attr>" as written in the JSON
    const char *registry_name; // ToolRegistry key (C++ class name)
};

const clit_agent_tool_entry k_clit_agent_tools[] = {
    {"kimi_cli.tools.file:read", "Read"},
    {"kimi_cli.tools.file:read_image", "ReadImage"},
    {"kimi_cli.tools.file:glob", "Glob"},
    {"kimi_cli.tools.file:grep", "Grep"},
    {"kimi_cli.tools.file:edit", "Edit"},
    {"kimi_cli.tools.file:write", "Write"},
    {"kimix.tools.web.fetch_url:fetch_url", "FetchUrl"},
    {"kimi_cli.tools.web:web_search", "WebSearch"},
    {"kimix.tools.note:WritePlan", "WritePlan"},
    {"kimix.tools.note:ReadPlan", "ReadPlan"},
    {"kimix.tools.note:EditPlan", "EditPlan"},
    {"kimix.tools.agent:subagent", "Subagent"},
    {"kimix.tools.agent:send_message", "SendMessage"},
    {"kimix.tools.agent:list_agents", "ListAgents"},
    {"kimix.tools.agent:interrupt_agent", "InterruptAgent"},
    {"kimix.tools.swarm:workflow", "Workflow"},
    {"kimi_cli.tools.todo:todo_write", "TodoWrite"},
    {"kimi_cli.tools.todo:todo_update", "TodoUpdate"},
    {"kimi_cli.tools.memory:retrieve", "Retrieve"},
    {"kimix.tools.context:compact", "Compact"},
    {"kimix.tools.file.bash:bash", "Bash"},
    {"kimix.tools.file.bash:pwsh", "Pwsh"},
    {"kimix.tools.file.run:Run", "Run"},
    {"kimix.tools.py:python", "Python"},
    {"kimix.tools.background:job_output", "JobOutput"},
};

} // namespace

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
    for (const std::pair<kimix::string, kimix::string> &entry : agent_tool_table()) {
        if (entry.first == tool_path) {
            return entry.second;
        }
    }
    return {};
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
