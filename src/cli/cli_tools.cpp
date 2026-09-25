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
    const char *registry_name; // ToolRegistry key (lowercase canonical name)
};

    const clit_agent_tool_entry k_clit_agent_tools[] = {
        {"kimi_cli.tools.file:read", "read"},
        {"kimi_cli.tools.file:read_image", "read_image"},
        {"kimi_cli.tools.file:glob", "glob"},
        {"kimi_cli.tools.file:grep", "grep"},
        {"kimi_cli.tools.file:edit", "edit"},
        {"kimi_cli.tools.file:write", "write"},
        {"kimix.tools.web.fetch_url:fetch_url", "fetch_url"},
        {"kimi_cli.tools.web:web_search", "web_search"},
        {"kimix.tools.note:WritePlan", "writeplan"},
        {"kimix.tools.note:ReadPlan", "readplan"},
        {"kimix.tools.note:EditPlan", "editplan"},
        {"kimix.tools.agent:subagent", "subagent"},
        {"kimix.tools.agent:send_message", "send_message"},
        {"kimix.tools.agent:list_agents", "list_agents"},
        {"kimix.tools.agent:interrupt_agent", "interrupt_agent"},
        {"kimix.tools.swarm:workflow", "workflow"},
        {"kimi_cli.tools.todo:todo_write", "todo_write"},
        {"kimi_cli.tools.todo:todo_update", "todo_update"},
        {"kimi_cli.tools.memory:retrieve", "retrieve"},
        {"kimix.tools.context:compact", "compact"},
        {"kimix.tools.file.bash:bash", "bash"},
        {"kimix.tools.file.bash:pwsh", "pwsh"},
        {"kimix.tools.file.run:Run", "run"},
        {"kimix.tools.py:python", "python"},
        {"kimix.tools.background:job_output", "job_output"},
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
