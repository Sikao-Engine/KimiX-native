// Test for the Tool::valid() availability design (src/builtin_tools/tool.h)
// and the agent soul's validity gate (src/agent/soul.cpp).
// This test covers:
// - the tool_availability override table + tool_valid() (the hook every
//   concrete valid() consults),
// - session_work_dir_usable(), the shared probe of the file-system tools,
// - the environment probes: bash / pwsh / python follow their executable
//   detection, and a caller-resolved shell path counts,
// - the session gates: plan_enabled (plan tools), swarm_enabled (workflow),
//   native_io (job_output), the injected history-index view (retrieve), the
//   injected sub-agent runner (subagent) and the bound session (todo tools),
// - every registered tool answers valid() twice with the same answer (the
//   probe never mutates the tool),
// - KimiSoul: an invalid tool is never in tool_definitions(), cannot be
//   executed, and the bash -> pwsh shell fallback (tool list AND the
//   {shell_tool} substitution in the default system prompt) kicks in when the
//   bash tool is invalid.
//
// Nothing here spawns a process: the probes are pure existence checks, so the
// suite runs in any environment (the override table makes the "not installed"
// branches testable on a machine where the program IS installed).

#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "agent/soul.h"
#include "builtin_tools/agent_tool.h"
#include "builtin_tools/bash_tool.h"
#include "builtin_tools/job_output_tool.h"
#include "builtin_tools/plan_tool.h"
#include "builtin_tools/pwsh_tool.h"
#include "builtin_tools/python_tool.h"
#include "builtin_tools/read_tool.h"
#include "builtin_tools/retrieve_tool.h"
#include "builtin_tools/todo_tool.h"
#include "builtin_tools/tool.h"
#include "builtin_tools/tool_registry.h"
#include "builtin_tools/workflow_tool.h"

#include <cstdio>
#include <utility>

namespace {

using namespace kimix::builtin_tools;

// ── Probe tools registered through the public macro ─────────────────────────
// Two always-answered tools: the soul test drives the "never offered" path
// without touching any real environment dependency.
struct AlwaysValidTool : Tool {
    using Tool::Tool;
    void operator()(ToolParams const *parameters) override { seen = parameters; }
    bool valid() const override { return tool_valid("ttvalid", true); }
    ToolParams const *seen = nullptr;
};
struct AlwaysInvalidTool : Tool {
    using Tool::Tool;
    void operator()(ToolParams const *parameters) override { (void)parameters; }
    bool valid() const override { return tool_valid("tvinvalid", false); }
};
KIMIX_REGISTER_TOOL_NAMED(AlwaysValidTool, "ttvalid", "probe (always usable)",
                          R"JSON({"type":"object"})JSON");
KIMIX_REGISTER_TOOL_NAMED(AlwaysInvalidTool, "tvinvalid",
                          "probe (never usable)",
                          R"JSON({"type":"object"})JSON");

// Scripted chat backend: records every request so the tests can read the
// system prompt and the tool list that reached the "LLM backend".
class RecordingBackend : public kimix::agent::IChatBackend {
public:
    kimix::vector<kimix::vector<kimix::llm::Message>> requests;
    kimix::vector<kimix::vector<kimix::llm::Tool>> tool_lists;

    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk) override {
        (void)on_chunk;
        requests.push_back(messages);
        tool_lists.push_back(tools);
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content = "understood";
        return r;
    }
    int64_t max_context_size() const override { return 128000; }
    kimix::string model_name() const override { return "recording"; }
};

kimix::string tv_tmp_workspace(const char *tag) {
    std::error_code ec;
    const kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) /
        kimix::string("kimix_tool_valid_") / tag;
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

bool lists_tool(const kimix::vector<kimix::llm::Tool> &defs,
                kimix::string_view name) {
    for (const kimix::llm::Tool &t : defs) {
        if (t.name == name) {
            return true;
        }
    }
    return false;
}

// RAII guard: pin availability for one test, then restore the real probes.
class pin_availability {
public:
    pin_availability(kimix::string_view key, bool available) {
        tool_availability::set_override(key, available);
    }
    pin_availability(pin_availability &&) = delete;
    pin_availability(const pin_availability &) = delete;
    ~pin_availability() { tool_availability::clear_all(); }
};

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));
    using namespace boost::ut;
    using namespace boost::ut::literals;

    // ── the override table + tool_valid() ───────────────────────────────────
    "tool_valid_uses_the_real_probe_without_an_override"_test = [] {
        tool_availability::clear_all();
        expect(tool_valid("tt_any_key", true));
        expect(!tool_valid("tt_any_key", false));
        expect(!tool_availability::override_of("tt_any_key").has_value());
    };

    "override_pins_the_answer_and_can_be_dropped"_test = [] {
        tool_availability::clear_all();
        tool_availability::set_override("tt_any_key", false);
        expect(!tool_valid("tt_any_key", true)) << "the pin wins over the probe";
        const auto pinned = tool_availability::override_of("tt_any_key");
        expect(pinned.has_value());
        expect(pinned.has_value() && !*pinned);
        tool_availability::set_override("tt_any_key", true);
        expect(tool_valid("tt_any_key", false)) << "a pin can be flipped";
        tool_availability::clear_override("tt_any_key");
        expect(!tool_availability::override_of("tt_any_key").has_value());
        expect(!tool_valid("tt_any_key", false)) << "clearing restores the probe";
        tool_availability::clear_all();
    };

    "validity_hook_survives_the_probe_short_circuit"_test = [] {
        // A registered tool routes through tool_valid(), so pinning its key
        // replaces its own probe entirely (the probe here is `false`).
        tool_availability::clear_all();
        Session s;
        auto tool = ToolRegistry::instance().create("tvinvalid", &s);
        expect(tool != nullptr);
        expect(tool != nullptr && !tool->valid());
        tool_availability::set_override("tvinvalid", true);
        expect(tool != nullptr && tool->valid()) << "the pin overrides the tool";
        tool_availability::clear_all();
        auto ok = ToolRegistry::instance().create("ttvalid", &s);
        expect(ok != nullptr && ok->valid());
    };

    // ── the shared work-directory probe ─────────────────────────────────────
    "session_work_dir_usable"_test = [] {
        expect(session_work_dir_usable(nullptr)) << "no session == process cwd";
        Session s;
        expect(session_work_dir_usable(&s)) << "an empty work dir == cwd";
        s.work_dir = tv_tmp_workspace("workdir");
        expect(session_work_dir_usable(&s)) << "an existing directory";
        s.work_dir = kimix::string(reinterpret_cast<const char *>("\1"), 1) +
                    "definitely/not/a/real/directory";
        expect(!session_work_dir_usable(&s)) << "a missing directory";
    };

    // ── the external-program probes ─────────────────────────────────────────
    "bash_validity_follows_the_shell_detection"_test = [] {
        tool_availability::clear_all();
        const kimix::string detected =
            bash::Bash::detect_bash_path(); // no spawn, existence only
        Session s;
        s.work_dir = tv_tmp_workspace("bash");
        s.native_io = true;
        bash::Bash bash_tool(&s);
        expect(eq(bash_tool.valid(), !detected.empty()))
            << "valid exactly when a bash executable exists";
        expect(!bash_tool.valid() || bash_tool.valid())
            << "the probe is idempotent";
        // A caller-resolved shell (the Python shim owns that path) counts even
        // on a machine where the built-in probe finds nothing.
        bash::Bash::config cfg;
        cfg.bash_path = kimix::string("/definitely/not/here/but-the-caller-said-so");
        bash::Bash pinned(&s, std::move(cfg));
        expect(pinned.valid()) << "an explicit config.bash_path is a shell";
    };

    "pwsh_validity_follows_the_powershell_detection"_test = [] {
        tool_availability::clear_all();
        Session s;
        s.work_dir = tv_tmp_workspace("pwsh");
        s.native_io = true;
        pwsh::Pwsh pwsh_tool(&s);
        expect(eq(pwsh_tool.valid(), !pwsh::detect_pwsh_path().empty()))
            << "valid exactly when a PowerShell host exists";
    };

    "python_validity_follows_the_interpreter_detection"_test = [] {
        tool_availability::clear_all();
        Session s;
        s.work_dir = tv_tmp_workspace("python");
        s.native_io = true;
        python::Python python_tool(&s);
        expect(eq(python_tool.valid(),
                  !python::Python::detect_python_exe(s.work_dir).empty()))
            << "valid exactly when an interpreter resolves";
    };

    // ── the session / injection gates ───────────────────────────────────────
    "plan_tools_gate_on_plan_enabled"_test = [] {
        tool_availability::clear_all();
        Session s;
        s.work_dir = tv_tmp_workspace("plan");
        s.native_io = true;
        s.plan_enabled = false;
        plan::WritePlan write(&s);
        plan::ReadPlan read(&s);
        plan::EditPlan edit(&s);
        expect(!write.valid()) << "SkipThisTool parity: plan off == no tool";
        expect(!read.valid());
        expect(!edit.valid());
        s.plan_enabled = true;
        expect(write.valid() && read.valid() && edit.valid());
        plan::WritePlan orphan(nullptr);
        expect(!orphan.valid()) << "a plan tool without a session has no gate";
    };

    "workflow_gates_on_swarm_enabled"_test = [] {
        tool_availability::clear_all();
        Session s;
        s.work_dir = tv_tmp_workspace("workflow");
        s.native_io = true;
        workflow::Workflow wf(&s);
        expect(!wf.valid()) << "outside a swarm session the tool is not offered";
        s.swarm_enabled = true;
        expect(wf.valid());
    };

    "retrieve_gates_on_the_injected_history_view"_test = [] {
        tool_availability::clear_all();
        Session s;
        s.work_dir = tv_tmp_workspace("retrieve");
        s.native_io = true;
        retrieve::Retrieve rv(&s);
        expect(!rv.valid()) << "no index view == every call would fail";
        rv.view.search_with_recency =
            [](kimix::string_view, int32_t) {
                return kimix::vector<retrieve::history_turn>{};
            };
        expect(!rv.valid()) << "both halves of the view are required";
        rv.view.get_by_id = [](kimix::string_view) {
            return kimix::optional<retrieve::history_turn>{};
        };
        expect(rv.valid());
    };

    "job_output_gates_on_the_task_registry"_test = [] {
        tool_availability::clear_all();
        Session plain;
        plain.work_dir = tv_tmp_workspace("job_plain");
        plain.native_io = false;
        job_output::JobOutput kernel_only(&plain);
        expect(!kernel_only.valid()) << "no tasks to read without native_io";
        Session native;
        native.work_dir = tv_tmp_workspace("job_native");
        native.native_io = true;
        job_output::JobOutput native_tool(&native);
        expect(native_tool.valid()) << "the reproc registry is bound";
        job_output::JobOutput mirrored(&plain);
        mirrored.source.list = [] {
            return kimix::vector<job_output::job_task>{};
        };
        expect(mirrored.valid()) << "an injected TaskSource is enough";
    };

    "todo_tools_need_a_session"_test = [] {
        tool_availability::clear_all();
        Session s;
        s.work_dir = tv_tmp_workspace("todo");
        todo::TodoWrite write(&s);
        todo::TodoUpdate update(&s);
        expect(write.valid() && update.valid());
        todo::TodoWrite orphan(nullptr);
        expect(!orphan.valid()) << "the list state lives in the session";
    };

    "subagent_gates_on_the_injected_runner"_test = [] {
        tool_availability::clear_all();
        Session s;
        s.work_dir = tv_tmp_workspace("subagent");
        s.native_io = true;
        agents::Subagent sub(&s);
        expect(!sub.valid()) << "no runner == the tool could only fail";
        expect(s.agents == nullptr) << "valid() must not create the registry";
        agents::session_registry(&s).runner =
            [](const agents::subagent_request &) {
                return agents::subagent_run_result{};
            };
        expect(sub.valid()) << "installing the runner re-enables the tool";
        // The sibling agent tools only need the session they route through.
        agents::SendMessageTool send(&s);
        agents::ListAgents list(&s);
        agents::InterruptAgent interrupt(&s);
        expect(send.valid() && list.valid() && interrupt.valid());
        agents::ListAgents orphan(nullptr);
        expect(!orphan.valid());
    };

    // ── the whole registry answers ──────────────────────────────────────────
    "every_registered_tool_answers_valid_twice_the_same"_test = [] {
        tool_availability::clear_all();
        Session s;
        s.work_dir = tv_tmp_workspace("registry");
        s.native_io = true;
        s.plan_enabled = true;
        s.swarm_enabled = true;
        size_t checked = 0;
        size_t usable = 0;
        for (const ToolMeta &meta : ToolRegistry::instance().all()) {
            expect(static_cast<bool>(meta.factory)) << meta.name;
            if (!meta.factory) {
                continue;
            }
            kimix::unique_ptr<Tool> tool = meta.factory(&s);
            expect(tool != nullptr) << meta.name;
            if (tool == nullptr) {
                continue;
            }
            const bool first = tool->valid();
            const bool second = tool->valid();
            expect(eq(first, second)) << meta.name;
            // A pinned-false override must replace the tool's own answer.
            tool_availability::set_override(meta.name, false);
            expect(!tool->valid()) << meta.name;
            tool_availability::clear_all();
            expect(eq(tool->valid(), first)) << meta.name;
            ++checked;
            if (first) {
                ++usable;
            }
        }
        expect(checked >= 25u) << "the probe ran over the whole registry";
        // The file-system tools and the shells must be usable here; the
        // session-gated ones (plan/workflow) are on, so most of the registry
        // is expected to answer true.
        expect(usable >= 15u) << usable;
    };

    "file_tools_report_invalid_when_the_work_dir_is_gone"_test = [] {
        tool_availability::clear_all();
        Session s;
        s.native_io = true;
        s.work_dir = tv_tmp_workspace("fs_ok");
        read::Read ok(&s);
        expect(ok.valid());
        s.work_dir = "z:/definitely/not/a/real/directory";
        read::Read gone(&s);
        expect(!gone.valid()) << "relative paths could not resolve";
        // The override pins the answer for the whole registry key.
        s.work_dir = tv_tmp_workspace("fs_ok2");
        read::Read again(&s);
        expect(again.valid());
        tool_availability::set_override("read", false);
        expect(!again.valid());
        tool_availability::clear_all();
    };

    // ── the soul's gate ─────────────────────────────────────────────────────
    "soul_lists_exactly_the_tools_that_can_run_here"_test = [] {
        // The no-filter shape of the gate: with options::enabled_tools empty
        // (every registered tool), tool_definitions() must be the registry
        // minus the tools that answer valid() == false for this session.
        tool_availability::clear_all();
        kimix::agent::AgentSession session(tv_tmp_workspace("soul_all"));
        RecordingBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        const auto defs = soul.tool_definitions();
        size_t runnable = 0;
        for (const ToolMeta &meta : ToolRegistry::instance().all()) {
            auto probe = meta.factory(&session.tool_session());
            const bool valid = (probe != nullptr) && probe->valid();
            expect(eq(lists_tool(defs, meta.name), valid)) << meta.name;
            if (valid) {
                ++runnable;
            }
        }
        expect(eq(defs.size(), runnable));
        // Enough survives the gate on any host to keep the agent useful, and
        // the session-gated tools do not (plan_enabled / swarm_enabled are off,
        // no sub-agent runner and no history index are injected).
        expect(defs.size() >= 12u) << defs.size();
        expect(!lists_tool(defs, "writeplan")) << "plan tools are off";
        expect(!lists_tool(defs, "workflow")) << "not a swarm session";
        expect(!lists_tool(defs, "subagent")) << "no injected runner";
        expect(!lists_tool(defs, "retrieve")) << "no history index view";
        expect(lists_tool(defs, "ttvalid")) << "a valid probe tool is listed";
        expect(!lists_tool(defs, "tvinvalid")) << "an invalid one is not";
    };

    "soul_reports_the_tools_it_dropped"_test = [] {
        // unavailable_tools() is the explainability seam: the names the
        // manifest asked for that the gate removed.
        tool_availability::clear_all();
        kimix::agent::AgentSession session(tv_tmp_workspace("soul_report"));
        RecordingBackend backend;
        kimix::agent::KimiSoul::options opts;
        opts.enabled_tools = {"read", "tvinvalid", "writeplan", "ttvalid"};
        kimix::agent::KimiSoul soul(session, backend, opts);
        const auto dropped = soul.unavailable_tools();
        auto has = [](const kimix::vector<kimix::string> &v,
                      kimix::string_view name) {
            for (const kimix::string &s : v) {
                if (s == name) {
                    return true;
                }
            }
            return false;
        };
        expect(has(dropped, "tvinvalid")) << "invalid tool";
        expect(has(dropped, "writeplan")) << "plan_enabled is off";
        expect(!has(dropped, "read")) << "the file tool runs here";
        expect(!has(dropped, "ttvalid")) << "the probe tool runs here";
        expect(eq(dropped.size(), static_cast<size_t>(2)));
    };

    "soul_never_offers_an_invalid_tool"_test = [] {
        tool_availability::clear_all();
        kimix::agent::AgentSession session(tv_tmp_workspace("soul_gate"));
        RecordingBackend backend;
        kimix::agent::KimiSoul::options opts;
        opts.enabled_tools = {"read", "ttvalid", "tvinvalid"};
        kimix::agent::KimiSoul soul(session, backend, opts);

        const auto defs = soul.tool_definitions();
        expect(lists_tool(defs, "read"));
        expect(lists_tool(defs, "ttvalid"));
        expect(!lists_tool(defs, "tvinvalid"))
            << "an invalid tool never reaches the LLM backend";
        expect(eq(defs.size(), static_cast<size_t>(2)));

        // A call for the dropped tool is refused instead of running it.
        kimix::string error;
        const kimix::string out = soul.execute_tool_call("tvinvalid", "{}", error);
        expect(out.find("not available") != kimix::string::npos) << out;
        expect(error == out) << "the refusal is reported through `error` too";
        const kimix::string ok = soul.execute_tool_call("ttvalid", "{}", error);
        expect(error.empty()) << error;
        expect(!ok.empty());
        const kimix::string unknown =
            soul.execute_tool_call("tt_no_such_tool", "{}", error);
        expect(unknown.find("unknown tool") != kimix::string::npos) << unknown;
    };

    "soul_drops_a_tool_whose_environment_gate_is_off"_test = [] {
        tool_availability::clear_all();
        kimix::agent::AgentSession session(tv_tmp_workspace("soul_plan"));
        RecordingBackend backend;
        kimix::agent::KimiSoul::options opts;
        opts.enabled_tools = {"read", "writeplan", "readplan", "editplan"};
        kimix::agent::KimiSoul soul(session, backend, opts);
        // The session has plan_enabled == false, so the plan tools are out.
        const auto defs = soul.tool_definitions();
        expect(eq(defs.size(), static_cast<size_t>(1))) << "only read survives";
        session.tool_session().plan_enabled = true;
        const auto opened = soul.tool_definitions();
        expect(eq(opened.size(), static_cast<size_t>(4)))
            << "the gate is re-evaluated on every rebuild";
    };

    "soul_shell_fallback_enables_pwsh_when_bash_is_invalid"_test = [] {
        tool_availability::clear_all();
        pin_availability bash_missing("bash", false);
        kimix::agent::AgentSession session(tv_tmp_workspace("soul_shell"));
        RecordingBackend backend;
        kimix::agent::KimiSoul::options opts;
        opts.enabled_tools = {"read", "bash"}; // no pwsh in the manifest
        opts.shell_tool = "bash";
        kimix::agent::KimiSoul soul(session, backend, opts);

        const auto defs = soul.tool_definitions();
        expect(!lists_tool(defs, "bash"));
        const bool pwsh_exists = !pwsh::detect_pwsh_path().empty();
        expect(eq(lists_tool(defs, "pwsh"), pwsh_exists))
            << "pwsh is added as the shell fallback when a host exists";
        expect(lists_tool(defs, "read"));

        // The default system prompt names the shell that actually works.
        const kimix::agent::TurnResult turn = soul.turn("hello");
        expect(turn.ok);
        expect(!backend.requests.empty());
        const kimix::string &system_prompt = backend.requests[0][0].content;
        if (pwsh_exists) {
            expect(system_prompt.find("instead of `pwsh`") != kimix::string::npos)
                << "the {shell_tool} substitution follows the fallback";
            expect(system_prompt.find("instead of `bash`") ==
                   kimix::string::npos);
        } else {
            expect(system_prompt.find("instead of `bash`") !=
                   kimix::string::npos)
                << "with no PowerShell host either, the prompt keeps its word";
        }
    };

    "soul_keeps_bash_when_it_is_available"_test = [] {
        tool_availability::clear_all();
        pin_availability bash_present("bash", true);
        kimix::agent::AgentSession session(tv_tmp_workspace("soul_bash"));
        RecordingBackend backend;
        kimix::agent::KimiSoul::options opts;
        opts.enabled_tools = {"read", "bash", "pwsh"};
        opts.shell_tool = "bash";
        kimix::agent::KimiSoul soul(session, backend, opts);
        const auto defs = soul.tool_definitions();
        expect(lists_tool(defs, "bash"));
        const kimix::agent::TurnResult turn = soul.turn("hello");
        expect(turn.ok);
        expect(!backend.requests.empty());
        expect(backend.requests[0][0].content.find("instead of `bash`") !=
               kimix::string::npos);
    };

    "soul_rebuild_never_repeats_the_fallback_tool"_test = [] {
        tool_availability::clear_all();
        pin_availability bash_missing("bash", false);
        kimix::agent::AgentSession session(tv_tmp_workspace("soul_dup"));
        RecordingBackend backend;
        kimix::agent::KimiSoul::options opts;
        opts.enabled_tools = {"read", "bash", "pwsh"};
        kimix::agent::KimiSoul soul(session, backend, opts);
        const auto defs = soul.tool_definitions();
        size_t pwsh_entries = 0;
        for (const kimix::llm::Tool &t : defs) {
            if (t.name == "pwsh") {
                ++pwsh_entries;
            }
        }
        expect(eq(pwsh_entries, static_cast<size_t>(1)))
            << "pwsh is already in the manifest, so the fallback must not add "
               "a second definition";
    };

    return 0;
}
