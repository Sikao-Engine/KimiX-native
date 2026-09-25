// test_system_prompt.cpp - Golden tests for agent/system_prompt.cpp:
// build_system_prompt() must be a byte-faithful port of kimi-agent's
// kimix/utils/system_prompt.py. The expected strings below are hand-expanded
// from that module's templates (fixed input: os=Windows, work_dir=D:\proj,
// yolo=true, shell_tool=bash, agents_md="Project rules.\n- Run tests.",
// skills_text="- alpha\n- beta") and cross-checked against an independent
// expansion of the template table.
//
// Framework: Boost.UT (tests/ut/ut.hpp). No network access.

#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "agent/soul.h"
#include "agent/system_prompt.h"
#include "llm/common.h"

#include <cstdio>

#include <cstdio>

namespace {

using namespace boost::ut;

// ── Shared hand-expanded template pieces (fixed golden input) ────────────────

// _TOOL_CONVENTIONS = sp_tool_conventions.strip() + '\n'
const char *k_golden_tool_conventions =
    "# Tool Conventions\n"
    "- **Output folding**: long outputs keep first/last N lines; middle "
    "replaced by a truncation marker.\n"
    "- **Output dedup**: repeated lines from known commands are deduplicated "
    "automatically; output is always token-filtered.\n"
    "- **`rtk`**: invoke known CLI tools as `rtk <process> <arguments...>` — "
    "deduplicates and truncates output.\n"
    "- **`wait_for_pattern`**: blocks up to `timeout` seconds until the "
    "pattern appears.\n"
    "- **`timeout`**: seconds; range/default are in each tool's parameter "
    "schema.\n"
    "- **Working directory**: `Run` takes `cwd`/`workdir`; `bash`/`pwsh`: `cd "
    "<dir> && <cmd>` / `cd <dir>; <cmd>`; `python` runs in the process "
    "cwd.\n";

// sp_base_items + sp_windows_item bullets (Windows golden input).
const char *k_golden_base_windows =
    "- Call tools in parallel.\n"
    "- OS: Windows WORK DIR: D:\\proj\n"
    "- Windows paths use backslashes (`\\`); always `\\` instead of `/` for "
    "file paths.\n";

// sp_worker_core bullets with {shell_tool} = bash.
const char *k_golden_worker_core =
    "- Read references/skills/files first; act on evidence, not "
    "knowledge.\n"
    "- Persist until requirements met; one action per turn.\n"
    "- For long commands, use `python` instead of `bash`.\n"
    "- On error: retry, adjust, or decompose.\n"
    "- Verify: run tests/checks before declaring done — never declare done "
    "from reading alone.\n"
    "- compact after each milestone.\n"
    "- Track with todo_* tools.\n";

// sp_worker_optional with yolo=true, not a sub-agent: YOLO + RETRIEVE.
const char *k_golden_worker_optional_yolo =
    "- Yolo: never ask — independently pick the best option and continue.\n"
    "- Use `retrieve` whenever unsure about past conversation history.\n";

// sp_worker_optional for a sub-agent: SUBAGENT clause only (its embedded
// newline makes two bullets), then the TRIVIAL item appended separately.
const char *k_golden_subagent_items =
    "- Sub-Agent: deliver a self-contained final result — the parent sees "
    "only your result, not your transcript or reasoning.\n"
    "- Report coverage: what you completed, what you only sampled/"
    "approximated, and anything unverified, so the parent can trust or "
    "re-check.\n"
    "- If you need clarification from the parent agent, call the "
    "`send_message` tool with your question, then stop.\n";

const char *k_golden_agent_md_block =
    "AGENTS.md:\n"
    "```\n"
    "Project rules.\n"
    "- Run tests.\n"
    "```\n";

const char *k_golden_skills_block = "Skills:\n- alpha\n- beta";

// Fixed golden input (see file header).
kimix::agent::system_prompt_input golden_input(kimix::agent::system_prompt_role role) {
    kimix::agent::system_prompt_input in;
    in.role = role;
    in.os = "Windows";
    in.work_dir = "D:\\proj";
    in.yolo = true;
    in.is_sub_agent = false;
    in.agents_md = "Project rules.\n- Run tests.";
    in.skills_text = "- alpha\n- beta";
    return in;
}

// Scripted chat backend (same pattern as test_agent.cpp): pops one canned
// result per chat() call and records the requests.
class FakeBackend : public kimix::agent::IChatBackend {
public:
    kimix::vector<kimix::llm::ChatResult> scripted;
    kimix::vector<kimix::vector<kimix::llm::Message>> requests;
    size_t index = 0;
    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk) override {
        (void)tools;
        (void)on_chunk;
        requests.push_back(messages);
        if (index < scripted.size()) {
            return scripted[index++];
        }
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content = "done";
        return r;
    }
    int64_t max_context_size() const override { return 128000; }
    kimix::string model_name() const override { return "fake"; }
};

kimix::string tmp_workspace() {
    std::error_code ec;
    kimix::filesystem::path base = kimix::filesystem::temp_directory_path(ec) /
                                   "kimix_system_prompt_test_ws";
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

} // namespace

int main() {
    using namespace boost::ut;
    using kimix::agent::system_prompt_role;

    // ── Role goldens (fixed input, see file header) ──────────────────────────

    "worker_prompt_golden"_test = [] {
        const kimix::string expected =
            kimix::string(k_golden_tool_conventions) +
            "You are a helpful software engineer assistant:\n" +
            k_golden_base_windows + k_golden_worker_core +
            k_golden_worker_optional_yolo + "\n" + k_golden_agent_md_block +
            k_golden_skills_block;
        expect(kimix::agent::build_system_prompt(
                   golden_input(system_prompt_role::worker)) == expected);
    };

    "todomaker_prompt_golden"_test = [] {
        const kimix::string expected =
            "You are a helpful software engineer planner:\n" +
            kimix::string(k_golden_base_windows) +
            "- Plan only. Do not implement.\n"
            "- Record a comprehensive plan with `WritePlan` `EditPlan`; "
            "include file paths per phase.\n"
            "- You cannot write files or run commands — reject requirements "
            "needing those abilities.\n"
            "\n" +
            k_golden_agent_md_block + k_golden_skills_block;
        expect(kimix::agent::build_system_prompt(
                   golden_input(system_prompt_role::todomaker)) == expected);
    };

    "thinker_prompt_golden"_test = [] {
        const kimix::string expected =
            kimix::string(k_golden_tool_conventions) +
            "You are a helpful software engineer thinker:\n" +
            k_golden_base_windows + k_golden_worker_core +
            k_golden_worker_optional_yolo +
            "- Think in <thinking>...</thinking>. End with <quit/>. Concise, "
            "no text outside tags.\n"
            "- Self-verify: catch errors and bad assumptions.\n"
            "\n" +
            k_golden_agent_md_block + k_golden_skills_block;
        expect(kimix::agent::build_system_prompt(
                   golden_input(system_prompt_role::thinker)) == expected);
    };

    "trivial_sub_agent_prompt_golden"_test = [] {
        const kimix::string expected =
            kimix::string(k_golden_tool_conventions) +
            "You are a helpful software engineer assistant sub-agent:\n" +
            k_golden_base_windows + k_golden_worker_core +
            k_golden_subagent_items + "\n" + k_golden_agent_md_block +
            k_golden_skills_block;
        expect(kimix::agent::build_system_prompt(
                   golden_input(system_prompt_role::trivial_sub_agent)) ==
               expected);
    };

    "reader_prompt_golden"_test = [] {
        // Reader embeds no AGENTS.md, no Skills and no tool conventions, so
        // the prompt is just the role line + numbered items (stripped).
        const kimix::string expected =
            "You are a helpful software engineer assistant reader:\n"
            "- Call tools in parallel.\n"
            "- OS: Windows WORK DIR: D:\\proj\n"
            "- Windows paths use backslashes (`\\`); always `\\` instead of "
            "`/` for file paths.\n"
            "- Read the given content and report a concise summary: key "
            "results, errors, warnings, and next steps.\n"
            "- No commands, edits, or questions.\n"
            "- For large content, cover the most relevant parts and note "
            "omissions.";
        expect(kimix::agent::build_system_prompt(
                   golden_input(system_prompt_role::reader)) == expected);
    };

    "supervisor_prompt_golden"_test = [] {
        const kimix::string expected =
            "You are a helpful software engineer assistant supervisor:\n" +
            kimix::string(k_golden_base_windows) +
            "- Before delegating: outline goals, constraints, unknowns, "
            "acceptance criteria.\n"
            "- Decompose into non-overlapping tasks (Explorer/Worker/"
            "Reviewer/Verifier); serial if same output.\n"
            "- Dispatch via `subagent` (background by default; "
            "`send_message` follow-ups; `interrupt_agent` to stop).\n"
            "- Never do sub-agent work yourself. Route failures through "
            "inquiry, then narrow correction.\n"
            "- Track with `todo_write`; accept or inquire/reject each "
            "result, then run one overall verification.\n"
            "- Final: report tasks, deliverables, verification, unresolved "
            "work, merged conclusion.\n"
            "\n" +
            k_golden_agent_md_block + k_golden_skills_block;
        expect(kimix::agent::build_system_prompt(
                   golden_input(system_prompt_role::supervisor)) == expected);
    };

    "swarm_leader_prompt_golden"_test = [] {
        const kimix::string expected =
            "You are a helpful software engineer assistant swarm "
            "orchestrator:\n" +
            kimix::string(k_golden_base_windows) +
            "- The user wants parallel work across multiple homogeneous "
            "sub-agents.\n"
            "- Split into independent, homogeneous sub-tasks.\n"
            "- Call `workflow` with: description, subagent_type (coder/"
            "explore/plan), prompt_template containing {{item}}, and items "
            "list.\n"
            "- Do not implement tasks yourself; only dispatch and summarize "
            "the aggregated result. If not parallelizable, explain why and "
            "stop.\n"
            "\n" +
            k_golden_agent_md_block + k_golden_skills_block;
        expect(kimix::agent::build_system_prompt(
                   golden_input(system_prompt_role::swarm_leader)) ==
               expected);
    };

    // ── Conditional behaviour ────────────────────────────────────────────────

    "non_windows_omits_backslash_item"_test = [] {
        kimix::agent::system_prompt_input in =
            golden_input(system_prompt_role::worker);
        in.os = "Linux";
        in.work_dir = "/home/proj";
        const kimix::string p = kimix::agent::build_system_prompt(in);
        const kimix::string expected =
            kimix::string(k_golden_tool_conventions) +
            "You are a helpful software engineer assistant:\n"
            "- Call tools in parallel.\n"
            "- OS: Linux WORK DIR: /home/proj\n" +
            k_golden_worker_core + k_golden_worker_optional_yolo + "\n" +
            k_golden_agent_md_block + k_golden_skills_block;
        expect(p == expected);
        expect(p.find("backslashes") == kimix::string::npos);
    };

    "shell_tool_substitution"_test = [] {
        kimix::agent::system_prompt_input in =
            golden_input(system_prompt_role::worker);
        in.shell_tool = "pwsh";
        const kimix::string p = kimix::agent::build_system_prompt(in);
        // The reference template wraps the placeholder in backticks
        // ("instead of `{shell_tool}`."), so the substitution renders
        // "instead of `pwsh`.".
        expect(p.find("use `python` instead of `pwsh`.") != kimix::string::npos);
        expect(p.find("{shell_tool}") == kimix::string::npos);
    };

    "agents_md_over_4096_bytes_uses_pointer_line"_test = [] {
        kimix::agent::system_prompt_input in =
            golden_input(system_prompt_role::worker);
        in.agents_md = kimix::string(5000, 'x');
        in.skills_text.clear();
        const kimix::string p = kimix::agent::build_system_prompt(in);
        // Over-long AGENTS.md collapses to the pointer line; empty skills
        // drop the Skills block entirely.
        expect(p.find("read AGENTS.md before work") != kimix::string::npos);
        expect(p.find("AGENTS.md:\n```") == kimix::string::npos);
        expect(p.find("Skills:") == kimix::string::npos);
    };

    "empty_skills_omits_skills_block"_test = [] {
        kimix::agent::system_prompt_input in =
            golden_input(system_prompt_role::worker);
        in.skills_text.clear();
        const kimix::string p = kimix::agent::build_system_prompt(in);
        expect(p.find("Skills:") == kimix::string::npos);
        // AGENTS.md is still embedded.
        expect(p.find("AGENTS.md:\n```\nProject rules.") !=
               kimix::string::npos);
    };

    "yolo_false_drops_yolo_clause"_test = [] {
        kimix::agent::system_prompt_input in =
            golden_input(system_prompt_role::worker);
        in.yolo = false;
        const kimix::string p = kimix::agent::build_system_prompt(in);
        expect(p.find("Yolo:") == kimix::string::npos);
        expect(p.find("Use `retrieve` whenever unsure") !=
               kimix::string::npos);
    };

    "strip_no_trailing_whitespace"_test = [] {
        // The Python closure .strip()s the assembled template: the C++ port
        // must not leave a trailing newline (or the blank line before a
        // missing AGENTS_MD/SKILLS block).
        for (const system_prompt_role role :
             {system_prompt_role::worker, system_prompt_role::todomaker,
              system_prompt_role::thinker, system_prompt_role::trivial_sub_agent,
              system_prompt_role::reader, system_prompt_role::supervisor,
              system_prompt_role::swarm_leader}) {
            kimix::agent::system_prompt_input in = golden_input(role);
            const kimix::string p = kimix::agent::build_system_prompt(in);
            expect(!p.empty());
            expect(p.back() != '\n');
            expect(p.back() != ' ');
            expect(p.find("\n\n\n") == kimix::string::npos);
        }
        // With no AGENTS.md and no skills the prompt still ends on the last
        // numbered item (strip eats the template's trailing newlines).
        kimix::agent::system_prompt_input bare =
            golden_input(system_prompt_role::worker);
        bare.agents_md.clear();
        bare.skills_text.clear();
        const kimix::string p = kimix::agent::build_system_prompt(bare);
        expect(p.rfind("past conversation history.") ==
               p.size() - kimix::string("past conversation history.").size());
    };

    // ── KimiSoul plumbing ────────────────────────────────────────────────────

    "explicit_system_prompt_overrides_default"_test = [] {
        kimix::agent::AgentSession session(kimix::string("D:\\proj"));
        FakeBackend backend;
        kimix::agent::KimiSoul::options opts;
        opts.system_prompt = "CUSTOM PROMPT VERBATIM";
        kimix::agent::KimiSoul soul(session, backend, opts);
        const kimix::agent::TurnResult r = soul.turn("hi");
        expect(r.ok);
        expect(backend.requests.size() == 1u);
        expect(backend.requests[0][0].role == "system");
        expect(backend.requests[0][0].content == "CUSTOM PROMPT VERBATIM");
    };

    "soul_default_prompt_is_built_system_prompt"_test = [] {
        kimix::agent::AgentSession session(kimix::string("D:\\proj"));
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        const kimix::agent::TurnResult r = soul.turn("hi");
        expect(r.ok);
        expect(backend.requests.size() == 1u);
        const kimix::string sys = backend.requests[0][0].content;
        expect(backend.requests[0][0].role == "system");
#if defined(KIMIX_PLATFORM_WINDOWS)
        // On Windows the soul-built default must equal build_system_prompt()
        // for the same inputs (empty skills + a nonexistent work_dir, so no
        // AGENTS.md is embedded).
        kimix::agent::system_prompt_input in =
            golden_input(system_prompt_role::worker);
        in.skills_text.clear();
        in.agents_md.clear();
        expect(sys == kimix::agent::build_system_prompt(in));
#else
        // Platform-agnostic shape checks elsewhere.
        expect(sys.find("# Tool Conventions\n") == 0u);
        expect(sys.find("You are a helpful software engineer assistant:\n") !=
               kimix::string::npos);
#endif
    };

    "soul_reads_agents_md_from_disk"_test = [] {
        const kimix::string dir = tmp_workspace();
        const kimix::string md_path =
            kimix::to_string(kimix::filesystem::path(dir) / "AGENTS.md");
        {
            std::FILE *f = std::fopen(md_path.c_str(), "wb");
            expect(f != nullptr);
            const char text[] = "Disk rules.\n- From disk.";
            expect(std::fwrite(text, 1, sizeof(text) - 1, f) ==
                   sizeof(text) - 1);
            std::fclose(f);
        }
        kimix::agent::AgentSession session(dir);
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        const kimix::agent::TurnResult r = soul.turn("hi");
        expect(r.ok);
        const kimix::string sys = backend.requests[0][0].content;
        expect(sys.find("AGENTS.md:\n```\nDisk rules.\n- From disk.\n```") !=
               kimix::string::npos);
        std::error_code ec;
        kimix::filesystem::remove_all(kimix::filesystem::path(dir), ec);
    };

    // The template table is documented as byte-identical to the Python
    // reference, and the prompt goes straight into the request JSON. MSVC reads
    // a BOM-less UTF-8 source with the system ANSI codepage unless /utf-8 is
    // set: on a GBK (ACP 936) host the em dashes of k_sp_tool_conventions and
    // k_clause_* compiled to E2 80 3F - invalid UTF-8, which yyjson's writer
    // rejects outright, so every turn failed with "failed to build request
    // body". The goldens above cannot catch it (they are miscompiled the same
    // way and still compare equal), so the bytes are checked directly.
    "embedded_literals_survive_the_compiler_charset"_test = [] {
        const char *const em_dash = "\xE2\x80\x94"; // U+2014
        const char *const mangled = "\xE2\x80\x3F"; // codepage round-trip
        const system_prompt_role roles[] = {
            system_prompt_role::worker,
            system_prompt_role::todomaker,
            system_prompt_role::thinker,
            system_prompt_role::trivial_sub_agent,
            system_prompt_role::reader,
            system_prompt_role::supervisor,
            system_prompt_role::swarm_leader,
        };
        for (const system_prompt_role role : roles) {
            const kimix::string prompt =
                kimix::agent::build_system_prompt(golden_input(role));
            expect(kimix::llm::utf8_valid(prompt))
                << "invalid UTF-8 in the prompt of role "
                << static_cast<int>(static_cast<int32_t>(role));
        }
        // The Tool Conventions block carries a real em dash; a miscompiled
        // build turns its third byte into '?'.
        const kimix::string prompt =
            kimix::agent::build_system_prompt(golden_input(system_prompt_role::worker));
        expect(prompt.find(em_dash) != kimix::string::npos) << "em dash lost";
        expect(prompt.find(mangled) == kimix::string::npos) << "em dash mangled";
    };

    return 0;
}
