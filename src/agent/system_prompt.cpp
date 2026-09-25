// agent/system_prompt.cpp - implementation of build_system_prompt()
// (see system_prompt.h). The prompt template table below is generated
// mechanically from kimi-agent's kimix/utils/system_prompt.py so the embedded
// strings are byte-identical to the Python reference.

#include "agent/system_prompt.h"

#include <cstddef>

namespace kimix::agent {

namespace {

// ── Python str helpers (port-only; inputs are plain ASCII) ───────────────────

bool sp_is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
           c == '\f';
}

// Python str.strip() equivalent for the ASCII whitespace set.
kimix::string sp_strip(const kimix::string &s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && sp_is_space(s[b])) {
        ++b;
    }
    while (e > b && sp_is_space(s[e - 1])) {
        --e;
    }
    return s.substr(b, e - b);
}

// Python str.replace(): every occurrence of `from` becomes `to`.
void sp_replace_all(kimix::string &text, const kimix::string &from,
                    const kimix::string &to) {
    if (from.empty()) {
        return;
    }
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != kimix::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// ── Prompt template table ────────────────────────────────────────────────────
// GENERATED from kimix/utils/system_prompt.py (_TEMPLATES and
// _WORKER_OPTIONAL_CLAUSES). Do not hand-edit; regenerate with
// scripts/gen_system_prompt_tables.py.
// TEMPLATE-TABLE-BEGIN
constexpr const char *k_sp_template = "{TOOL_CONVENTIONS}{AGENT_ROLE}:\n{NUMBERED}\n{AGENTS_MD}{SKILLS}";
constexpr const char *k_sp_tool_conventions = "# Tool Conventions\n- **Output folding**: long outputs keep first/last N lines; middle replaced by a truncation marker.\n- **Output dedup**: repeated lines from known commands are deduplicated automatically; output is always token-filtered.\n- **`rtk`**: invoke known CLI tools as `rtk <process> <arguments...>` — deduplicates and truncates output.\n- **`wait_for_pattern`**: blocks up to `timeout` seconds until the pattern appears.\n- **`timeout`**: seconds; range/default are in each tool's parameter schema.\n- **Working directory**: `Run` takes `cwd`/`workdir`; `bash`/`pwsh`: `cd <dir> && <cmd>` / `cd <dir>; <cmd>`; `python` runs in the process cwd.\n";
constexpr const char *k_sp_base_items = "Call tools in parallel.\nOS: {KIMI_OS} WORK DIR: {KIMI_WORK_DIR}\n";
constexpr const char *k_sp_windows_item = "Windows paths use backslashes (`\\`); always `\\` instead of `/` for file paths.\n";
constexpr const char *k_sp_worker_core = "Read references/skills/files first; act on evidence, not knowledge.\nPersist until requirements met; one action per turn.\nFor long commands, use `python` instead of `{shell_tool}`.\nOn error: retry, adjust, or decompose.\nVerify: run tests/checks before declaring done — never declare done from reading alone.\ncompact after each milestone.\nTrack with todo_* tools.\n";
constexpr const char *k_sp_worker_optional = "{YOLO}\n{RETRIEVE}\n{SUBAGENT}\n{TRIVIAL}\n";
constexpr const char *k_sp_thinker_items = "Think in <thinking>...</thinking>. End with <quit/>. Concise, no text outside tags.\nSelf-verify: catch errors and bad assumptions.\n";
constexpr const char *k_sp_todomaker_items = "Plan only. Do not implement.\nRecord a comprehensive plan with `WritePlan` `EditPlan`; include file paths per phase.\nYou cannot write files or run commands — reject requirements needing those abilities.\n";
constexpr const char *k_sp_reader_items = "Read the given content and report a concise summary: key results, errors, warnings, and next steps.\nNo commands, edits, or questions.\nFor large content, cover the most relevant parts and note omissions.\n";
constexpr const char *k_sp_supervisor_items = "Before delegating: outline goals, constraints, unknowns, acceptance criteria.\nDecompose into non-overlapping tasks (Explorer/Worker/Reviewer/Verifier); serial if same output.\nDispatch via `subagent` (background by default; `send_message` follow-ups; `interrupt_agent` to stop).\nNever do sub-agent work yourself. Route failures through inquiry, then narrow correction.\nTrack with `todo_write`; accept or inquire/reject each result, then run one overall verification.\nFinal: report tasks, deliverables, verification, unresolved work, merged conclusion.\n";
constexpr const char *k_sp_swarm_leader_items = "The user wants parallel work across multiple homogeneous sub-agents.\nSplit into independent, homogeneous sub-tasks.\nCall `workflow` with: description, subagent_type (coder/explore/plan), prompt_template containing {{item}}, and items list.\nDo not implement tasks yourself; only dispatch and summarize the aggregated result. If not parallelizable, explain why and stop.\n";
constexpr const char *k_sp_agent_md = "AGENTS.md:\n```\n{content}\n```\n";
constexpr const char *k_sp_read_agents_md = "read AGENTS.md before work\n";
constexpr const char *k_sp_skills = "Skills:\n{skills}\n";
constexpr const char *k_clause_yolo = "Yolo: never ask — independently pick the best option and continue.";
constexpr const char *k_clause_retrieve = "Use `retrieve` whenever unsure about past conversation history.";
constexpr const char *k_clause_subagent = "Sub-Agent: deliver a self-contained final result — the parent sees only your result, not your transcript or reasoning.\nReport coverage: what you completed, what you only sampled/approximated, and anything unverified, so the parent can trust or re-check.";
constexpr const char *k_clause_trivial = "If you need clarification from the parent agent, call the `send_message` tool with your question, then stop.";
// TEMPLATE-TABLE-END

// ── _load_items() port ───────────────────────────────────────────────────────

// Split `text` into lines and keep the ones whose Python line.strip() is
// non-empty (blank lines never become bullets). Substitutions are applied by
// the caller BEFORE splitting, exactly like the reference: a substituted
// value containing '\n' (the SUBAGENT clause) yields several items.
kimix::vector<kimix::string> sp_load_items(const kimix::string &text) {
    kimix::vector<kimix::string> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const kimix::string line =
            nl == kimix::string::npos ? text.substr(pos)
                                      : text.substr(pos, nl - pos);
        bool blank = true;
        for (const char c : line) {
            if (!sp_is_space(c)) {
                blank = false;
                break;
            }
        }
        if (!blank) {
            out.push_back(line);
        }
        if (nl == kimix::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    return out;
}

// ── Role dispatch (port of get_system_prompt's match statement) ──────────────

void worker_logic(bool is_sub_agent, bool yolo, const kimix::string &shell_tool,
                  kimix::vector<kimix::string> &items) {
    items.reserve(items.size() + 16);
    // sp_worker_core with the {shell_tool} substitution.
    kimix::string core = k_sp_worker_core;
    sp_replace_all(core, "{shell_tool}", shell_tool);
    const kimix::vector<kimix::string> core_items = sp_load_items(core);
    items.insert(items.end(), core_items.begin(), core_items.end());
    // sp_worker_optional: sub-agent gets the SUBAGENT clause only; otherwise
    // YOLO (when enabled) and RETRIEVE. Blank substitutions drop their line.
    kimix::string yolo_clause;
    kimix::string retrieve_clause;
    kimix::string subagent_clause;
    if (is_sub_agent) {
        subagent_clause = k_clause_subagent;
    } else {
        if (yolo) {
            yolo_clause = k_clause_yolo;
        }
        retrieve_clause = k_clause_retrieve;
    }
    kimix::string optional = k_sp_worker_optional;
    sp_replace_all(optional, "{YOLO}", yolo_clause);
    sp_replace_all(optional, "{RETRIEVE}", retrieve_clause);
    sp_replace_all(optional, "{SUBAGENT}", subagent_clause);
    sp_replace_all(optional, "{TRIVIAL}", kimix::string());
    const kimix::vector<kimix::string> opt_items = sp_load_items(optional);
    items.insert(items.end(), opt_items.begin(), opt_items.end());
}

} // namespace

// ---------------------------------------------------------------------------
// build_system_prompt
// ---------------------------------------------------------------------------

kimix::string build_system_prompt(const system_prompt_input &in) {
    // tool_conventions = '' / _TOOL_CONVENTIONS (.strip() + '\n')
    kimix::string tool_conventions;
    kimix::vector<kimix::string> items;
    kimix::string role_doc;
    bool use_agent_md = false;
    bool use_skills = false;

    // sp_base_items: always first.
    {
        kimix::string base = k_sp_base_items;
        sp_replace_all(base, "{KIMI_OS}", in.os);
        sp_replace_all(base, "{KIMI_WORK_DIR}", in.work_dir);
        const kimix::vector<kimix::string> base_items = sp_load_items(base);
        items.insert(items.end(), base_items.begin(), base_items.end());
    }
    // sp_windows_item: only on Windows.
    if (in.os == "Windows") {
        const kimix::vector<kimix::string> win_items =
            sp_load_items(kimix::string(k_sp_windows_item));
        items.insert(items.end(), win_items.begin(), win_items.end());
    }

    const bool is_sub_agent =
        in.role == system_prompt_role::trivial_sub_agent || in.is_sub_agent;
    switch (in.role) {
    case system_prompt_role::worker:
        tool_conventions = sp_strip(kimix::string(k_sp_tool_conventions)) + "\n";
        use_agent_md = true;
        use_skills = true;
        role_doc = "You are a helpful software engineer assistant";
        worker_logic(is_sub_agent, in.yolo, in.shell_tool, items);
        break;
    case system_prompt_role::todomaker:
        use_agent_md = true;
        use_skills = true;
        role_doc = "You are a helpful software engineer planner";
        {
            const kimix::vector<kimix::string> v =
                sp_load_items(kimix::string(k_sp_todomaker_items));
            items.insert(items.end(), v.begin(), v.end());
        }
        break;
    case system_prompt_role::thinker:
        tool_conventions = sp_strip(kimix::string(k_sp_tool_conventions)) + "\n";
        use_agent_md = true;
        use_skills = true;
        role_doc = "You are a helpful software engineer thinker";
        worker_logic(is_sub_agent, in.yolo, in.shell_tool, items);
        {
            const kimix::vector<kimix::string> v =
                sp_load_items(kimix::string(k_sp_thinker_items));
            items.insert(items.end(), v.begin(), v.end());
        }
        break;
    case system_prompt_role::trivial_sub_agent:
        tool_conventions = sp_strip(kimix::string(k_sp_tool_conventions)) + "\n";
        use_agent_md = true;
        use_skills = true;
        role_doc = "You are a helpful software engineer assistant sub-agent";
        worker_logic(/*is_sub_agent=*/true, in.yolo, in.shell_tool, items);
        // items.append(TRIVIAL) - a single item, not split like _load_items.
        items.push_back(k_clause_trivial);
        break;
    case system_prompt_role::reader:
        role_doc = "You are a helpful software engineer assistant reader";
        {
            const kimix::vector<kimix::string> v =
                sp_load_items(kimix::string(k_sp_reader_items));
            items.insert(items.end(), v.begin(), v.end());
        }
        break;
    case system_prompt_role::supervisor:
        use_agent_md = true;
        use_skills = true;
        role_doc = "You are a helpful software engineer assistant supervisor";
        {
            const kimix::vector<kimix::string> v =
                sp_load_items(kimix::string(k_sp_supervisor_items));
            items.insert(items.end(), v.begin(), v.end());
        }
        break;
    case system_prompt_role::swarm_leader:
        use_agent_md = true;
        use_skills = true;
        role_doc = "You are a helpful software engineer assistant swarm orchestrator";
        {
            const kimix::vector<kimix::string> v =
                sp_load_items(kimix::string(k_sp_swarm_leader_items));
            items.insert(items.end(), v.begin(), v.end());
        }
        break;
    }

    // Pre-compaction export notice (compact_export_path non-empty).
    if (!in.compact_export_path.empty()) {
        items.push_back("Pre-compaction context exported to: " +
                        in.compact_export_path);
    }

    // AGENTS_MD block (byte length > 4096 -> pointer line).
    kimix::string agent_md_doc;
    if (use_agent_md && !in.agents_md.empty()) {
        if (in.agents_md.size() > 4096) {
            agent_md_doc = k_sp_read_agents_md;
        } else {
            agent_md_doc = k_sp_agent_md;
            sp_replace_all(agent_md_doc, "{content}", in.agents_md);
        }
    }

    // SKILLS block.
    kimix::string skill_doc;
    if (use_skills && !in.skills_text.empty()) {
        skill_doc = k_sp_skills;
        sp_replace_all(skill_doc, "{skills}", in.skills_text);
    }

    // numbered_block = ''.join(f'- {item}\n' for item in items)
    kimix::string numbered_block;
    for (const kimix::string &item : items) {
        numbered_block += "- ";
        numbered_block += item;
        numbered_block += "\n";
    }

    // prompt = sp_template.format(...).strip()
    kimix::string prompt;
    prompt.reserve(tool_conventions.size() + role_doc.size() +
                   numbered_block.size() + agent_md_doc.size() +
                   skill_doc.size() + 8);
    prompt += tool_conventions;
    prompt += sp_strip(role_doc);
    prompt += ":\n";
    prompt += numbered_block;
    prompt += "\n";
    prompt += agent_md_doc;
    prompt += skill_doc;
    return sp_strip(prompt);
}

bool system_prompt_role_uses_agent_md(system_prompt_role role) noexcept {
    return role != system_prompt_role::reader;
}

} // namespace kimix::agent
