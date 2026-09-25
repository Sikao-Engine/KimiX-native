// new_tools_e2e.cpp - End-to-end test of the ten C++ ports of the kimi-agent
// built-in tools that were missing from the native registry (plan file tools,
// Run, JobOutput, sub-agent tools, Workflow/AgentSwarm) against a real LLM
// provider.
//
// Usage: xmake run new_tools_e2e [config.json]
//   config.json defaults to C:/dev/ds_ucloud.json (openai_legacy provider)
//
// The binary:
//   1. creates an AgentSession in a scratch work dir with the plan-file gate
//      enabled (plan_path set), the swarm gate enabled (swarm_enabled) and a
//      REAL sub-agent runner injected into the session agent registry. The
//      runner drives genuine nested KimiSoul step loops against the same LLM
//      backend, polling the cancel flag (interrupt_agent) and the steer queue
//      (send_message to a running agent) between steps - the C++ counterpart
//      of the Python asyncio sub-agent runner.
//   2. drives six KimiSoul turns, each asking the model to exercise one tool
//      group with explicit one-tool-call-per-step instructions:
//        A. Run (run_in_background) + JobOutput (list + wait/get)
//        B. WritePlan + ReadPlan + EditPlan (+ on-disk verification)
//        C. Subagent (foreground) + SendMessage (queued to a closed session)
//           + ListAgents
//        D. SendMessage (steer, delivered to a RUNNING agent) + InterruptAgent
//           on a slow background child that the host pre-started
//        E. Workflow fanout (2 coder sub-agents through the real runner)
//        F. Workflow parallel_sample (best-of-N: 2 samples + LLM selector)
//   3. performs PASS/FAIL evidence checks over the transcript, the registry
//      state and the files on disk (exit code 0 only when all pass).

#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include <core/clock.h>
#include <core/kimix_core.h>

#include "agent/soul.h"
#include "builtin_tools/agent_tool.h"
#include "builtin_tools/process_runner.h"
#include "llm/llm.h"

namespace {

namespace agents = kimix::builtin_tools::agents;

void log_chunk(const kimix::llm::Chunk &chunk) {
    if (!chunk.content.empty()) {
        std::printf("%s", chunk.content.c_str());
        std::fflush(stdout);
    }
    for (const auto &tc : chunk.tool_calls) {
        if (!tc.name.empty()) {
            std::printf("\n  [tool-call] %s args:%s", tc.name.c_str(),
                        tc.arguments.c_str());
            std::fflush(stdout);
        }
    }
}

bool contains(const kimix::string &hay, kimix::string_view needle) {
    return hay.find(kimix::string(needle)) != kimix::string::npos;
}

kimix::string read_file_text(const kimix::filesystem::path &p, bool &ok) {
    ok = false;
    kimix::string out;
    std::FILE *f = std::fopen(kimix::to_string(p).c_str(), "rb");
    if (f == nullptr) {
        return out;
    }
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    ok = true;
    return out;
}

// Serializing backend decorator: kimix::llm::LLM is not proven thread-safe,
// and background sub-agents + workflow fan-out call chat() from worker
// threads while the parent soul is mid-turn. ONE mutex for every chat call
// in the process (parent, children, best-of-N selector) keeps the provider
// client single-threaded, exactly like the Python asyncio event loop.
class mutex_backend : public kimix::agent::IChatBackend {
public:
    mutex_backend(kimix::agent::IChatBackend &inner, std::mutex &mx)
        : _inner(inner), _mx(mx) {}

    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk) override {
        std::lock_guard<std::mutex> g(_mx);
        return _inner.chat(messages, tools, on_chunk);
    }
    int64_t max_context_size() const override {
        return _inner.max_context_size();
    }
    kimix::string model_name() const override { return _inner.model_name(); }

private:
    kimix::agent::IChatBackend &_inner;
    std::mutex &_mx;
};

// ---------------------------------------------------------------------------
// Real sub-agent host: one shared LLM backend + per-step child turn loop.
// ---------------------------------------------------------------------------
//
// The agent/Workflow tools bind every LLM-facing hook through the session
// agent registry's `runner`; this is a genuine implementation of it. The loop
// mirrors KimiSoul::turn but additionally polls, between steps, the cancel
// flag (interrupt_agent) and the steer queue (send_message to a running
// agent) - exactly what the Python asyncio runner does with Steer/close.
struct subagent_host {
    kimix::agent::IChatBackend *backend; // the mutex_backend wrapper
    agents::agent_registry *registry;
    kimix::string parent_session_id; // owner session (send-to-parent target)
    std::mutex llm_mutex; // every chat() in the process serializes on this

    kimix::string system_prompt_for(kimix::string_view type) const {
        kimix::string p =
            "You are a focused sub-agent (type: ";
        p.append(type.data(), type.size());
        p += ") running inside a C++ agent runtime. You share the parent's "
             "working directory; relative paths resolve against it. Complete "
             "the single task you are given with the fewest possible tool "
             "calls, then answer with plain text. Do not ask questions.";
        return p;
    }
};

double now_seconds() {
    return static_cast<double>(kimix::Clock::now_ms()) / 1000.0;
}

// One genuine child turn loop. Returns the run result (turns recorded for
// history_format / evidence checks).
agents::subagent_run_result
run_child(subagent_host &host, const agents::subagent_request &req,
          const kimix::string &work_dir) {
    using kimix::agent::AgentSession;
    using kimix::agent::KimiSoul;
    agents::subagent_run_result out;

    AgentSession child(work_dir);
    child.tool_session().session_id = req.session_id;
    child.tool_session().is_sub_agent = true;
    child.tool_session().parent_session_id = host.parent_session_id;

    KimiSoul::options opts;
    opts.system_prompt = host.system_prompt_for(
        req.subagent_type.empty() ? kimix::string_view("coder")
                                  : kimix::string_view(req.subagent_type));
    opts.enabled_tools = {"read", "write", "bash", "grep", "glob", "run"};
    opts.max_steps = 40;
    opts.auto_compact = false;
    KimiSoul soul(child, *host.backend, opts);
    auto &history = child.history();

    agents::conversation_turn t0;
    t0.role = "user";
    t0.content = req.prompt;
    t0.timestamp = now_seconds();
    out.turns.push_back(t0);

    kimix::llm::Message user_msg;
    user_msg.role = "user";
    user_msg.content = req.prompt;
    history.push_back(std::move(user_msg));

    const kimix::vector<kimix::llm::Tool> tools = soul.tool_definitions();
    kimix::string last_text;

    for (int32_t step = 0; step < opts.max_steps; ++step) {
        // Steer first (send_message to a running agent lands here), then the
        // cancel check - so an interrupted agent still records the message.
        if (host.registry != nullptr) {
            const kimix::vector<kimix::string> steer =
                host.registry->drain_steer(req.session_id);
            if (!steer.empty()) {
                kimix::string joined;
                for (size_t i = 0; i < steer.size(); ++i) {
                    if (i != 0) {
                        joined += "\n\n";
                    }
                    joined += steer[i];
                }
                agents::conversation_turn ts;
                ts.role = "user";
                ts.type = "steer";
                ts.content = joined;
                ts.timestamp = now_seconds();
                out.turns.push_back(ts);
                kimix::llm::Message sm;
                sm.role = "user";
                sm.content = std::move(joined);
                history.push_back(std::move(sm));
            }
        }
        if (req.cancel != nullptr && req.cancel->load()) {
            out.cancelled = true;
            out.ok = false;
            out.error = "cancelled by interrupt_agent";
            return out;
        }

        kimix::vector<kimix::llm::Message> messages;
        kimix::llm::Message sys;
        sys.role = "system";
        sys.content = opts.system_prompt;
        messages.push_back(std::move(sys));
        for (const kimix::llm::Message &m : history) {
            messages.push_back(m);
        }

        // chat() serializes through the shared mutex_backend wrapper.
        const kimix::llm::ChatResult res =
            host.backend->chat(messages, tools, {});
        if (!res.ok) {
            out.ok = false;
            out.error = "chat failed: " + res.error;
            return out;
        }

        agents::conversation_turn ta;
        ta.role = "assistant";
        ta.type = "text";
        ta.content = res.content;
        ta.timestamp = now_seconds();
        out.turns.push_back(ta);

        kimix::llm::Message assistant;
        assistant.role = "assistant";
        assistant.content = res.content;
        assistant.thinking = res.reasoning;
        assistant.tool_calls = res.tool_calls;
        history.push_back(std::move(assistant));

        if (res.tool_calls.empty()) {
            out.ok = true;
            out.output = res.content;
            return out;
        }
        last_text = res.content;

        for (const kimix::llm::ToolCall &tc : res.tool_calls) {
            kimix::string terr;
            kimix::string result =
                soul.execute_tool_call(tc.name, tc.arguments, terr);
            agents::conversation_turn tt;
            tt.role = "tool";
            tt.type = "tool_result";
            tt.content = tc.name;
            tt.content += " ";
            tt.content += result.size() > 400 ? result.substr(0, 400) : result;
            tt.timestamp = now_seconds();
            out.turns.push_back(tt);
            kimix::llm::Message tool_msg;
            tool_msg.role = "tool";
            tool_msg.tool_call_id = tc.id;
            tool_msg.content = std::move(result);
            history.push_back(std::move(tool_msg));
        }
    }
    out.ok = false;
    out.error = "max steps reached";
    out.output = last_text;
    return out;
}

} // namespace

int main(int argc, char *argv[]) {
    const kimix::string config_path =
        argc > 1 ? argv[1] : kimix::string("C:/dev/ds_ucloud.json");

    // ── 1. Session + workspace + gates + real runner ────────────────────────
    std::error_code ec;
    kimix::filesystem::path ws =
        kimix::filesystem::temp_directory_path(ec) / "kimix_new_tools_e2e";
    kimix::filesystem::remove_all(ws, ec);
    kimix::filesystem::create_directories(ws, ec);
    const kimix::string work_dir = kimix::to_string(ws);

    kimix::agent::AgentSession session(work_dir);
    session.tool_session().session_id = session.id();
    session.tool_session().plan_enabled = true;
    session.tool_session().plan_path = kimix::to_string(ws / "plan.md");
    session.tool_session().swarm_enabled = true;
    std::printf("session id: %s\nwork dir:   %s\nplan path:  %s\n",
                session.id().c_str(), session.work_dir().c_str(),
                session.tool_session().plan_path.c_str());

    auto llm = kimix::llm::create_llm_from_file(config_path);
    if (llm == nullptr) {
        std::fprintf(stderr, "failed to load LLM config from %s\n",
                     config_path.c_str());
        return 2;
    }
    std::printf("provider:   model=%s type=%s ctx=%d\n\n",
                llm->model_name().c_str(), llm->config().type.c_str(),
                llm->max_context_size());
    kimix::agent::LLMBackend real_backend(std::move(llm));
    subagent_host host;
    host.parent_session_id = session.id();
    mutex_backend backend(real_backend, host.llm_mutex);
    host.backend = &backend;
    agents::agent_registry &registry =
        agents::session_registry(&session.tool_session());
    host.registry = &registry;
    registry.runner = [&host, &session, &work_dir](
                          const agents::subagent_request &req)
        -> agents::subagent_run_result {
        return run_child(host, req, work_dir);
    };

    kimix::agent::KimiSoul::options opts;
    opts.enabled_tools = {"read",  "write",       "bash",        "grep",
                          "glob",  "edit",        "run",         "job_output",
                          "writeplan", "readplan", "editplan",   "subagent",
                          "send_message", "list_agents", "interrupt_agent",
                          "workflow"};
    opts.max_steps = 32;
    opts.auto_compact = false;
    kimix::agent::KimiSoul soul(session, backend, opts); // serialized backend

    int failures = 0;
    auto check = [&](const char *name, bool ok) {
        std::printf("[%-42s] %s\n", name, ok ? "PASS" : "FAIL");
        if (!ok) {
            ++failures;
        }
    };

    // Transcript collector over the whole session history.
    auto collect_transcript = [&session]() {
        kimix::string t;
        for (const kimix::llm::Message &m : session.history()) {
            t += m.role;
            t += '\n';
            t += m.content;
            t += '\n';
            for (const kimix::llm::ToolCall &tc : m.tool_calls) {
                t += "[tool-call] ";
                t += tc.name;
                t += ' ';
                t += tc.arguments;
                t += '\n';
            }
        }
        return t;
    };
    auto count_calls = [&session](kimix::string_view name) {
        int n = 0;
        for (const kimix::llm::Message &m : session.history()) {
            for (const kimix::llm::ToolCall &tc : m.tool_calls) {
                if (tc.name == kimix::string(name)) {
                    ++n;
                }
            }
        }
        return n;
    };

    // ── 2. Turn A: Run background + JobOutput list/get ──────────────────────
    const kimix::string task_a =
        "Complete ALL of the following steps in order, using exactly one tool "
        "call per step:\n"
        "1. Call the Run tool with run_in_background=true and command exactly: "
        "python -c \"import time;print('E2E_BG_START');time.sleep(3);print("
        "'E2E_BG_DONE')\"\n"
        "2. Call the JobOutput tool with action=\"list\" (no job_id) to see "
        "the running task.\n"
        "3. Call the JobOutput tool with action=\"get\", the job_id returned "
        "by step 1, wait=true and timeout=30.\n"
        "When all three steps succeeded and the output contains "
        "E2E_BG_DONE, reply with the single word DONE and nothing else.";

    std::printf("── turn A (Run background + JobOutput) ───────────────\n");
    kimix::agent::TurnResult tr_a = soul.turn(task_a, log_chunk);
    std::printf("\n── turn A result: ok=%d steps=%d ─────────────────────\n",
                static_cast<int>(tr_a.ok), tr_a.steps);

    // ── 3. Turn B: WritePlan / ReadPlan / EditPlan ──────────────────────────
    const kimix::string task_b =
        "Complete ALL of the following steps in order, using exactly one tool "
        "call per step:\n"
        "1. Call WritePlan with content exactly (three lines, each starting "
        "with \"- \"):\n"
        "# Plan\n"
        "- [ ] step one: fetch data\n"
        "- [ ] step two: crunch data\n"
        "- [ ] step three: ship report\n"
        "2. Call ReadPlan with no parameters.\n"
        "3. Call EditPlan with a single edit: old=\"- [ ] step two: crunch "
        "data\", new=\"- [x] step two: crunch data\".\n"
        "4. Call ReadPlan again with no parameters to confirm the edit.\n"
        "When all four steps succeeded, reply with the single word DONE and "
        "nothing else.";

    std::printf("\n── turn B (plan tools) ───────────────────────────────\n");
    kimix::agent::TurnResult tr_b = soul.turn(task_b, log_chunk);
    std::printf("\n── turn B result: ok=%d steps=%d ─────────────────────\n",
                static_cast<int>(tr_b.ok), tr_b.steps);

    // ── 4. Turn C: Subagent (foreground) + SendMessage (queued) + ListAgents ─
    const kimix::string task_c =
        "Complete ALL of the following steps in order, using exactly one tool "
        "call per step:\n"
        "1. Call the Subagent tool with run_in_background=false, "
        "session_id=\"sub_demo_agent\" and prompt: \"Use the Write tool to "
        "create the file subagent_result.txt with exactly the content "
        "SUBAGENT_OK, then reply with the single word OK.\"\n"
        "2. Call the SendMessage tool with subagent_id=\"sub_demo_agent\" and "
        "message=\"queued follow-up hello\".\n"
        "3. Call the ListAgents tool with no parameters.\n"
        "When all three steps succeeded, reply with the single word DONE and "
        "nothing else.";

    std::printf("\n── turn C (subagent + send_message + list_agents) ────\n");
    kimix::agent::TurnResult tr_c = soul.turn(task_c, log_chunk);
    std::printf("\n── turn C result: ok=%d steps=%d ─────────────────────\n",
                static_cast<int>(tr_c.ok), tr_c.steps);

    // ── 5. Turn D: steer + InterruptAgent on a live background child ────────
    // Pre-start a slow child directly through the registry (what the Subagent
    // tool does internally for run_in_background=true). The child writes
    // loop_XX.txt files with a 2s Run pacing between writes, so it stays
    // alive for the whole turn.
    agents::subagent_request slow_req;
    slow_req.session_id = "interrupt_demo_agent";
    slow_req.prompt =
        "You are a slow worker. Repeat 10 times with a zero-padded counter "
        "NN from 01 to 10: use the Write tool to create the file loop_NN.txt "
        "(NN = the counter) with content exactly \"tick\", then call the Run "
        "tool with command exactly: python -c \"import time;time.sleep(2)\" "
        "to pace yourself. After the 10th repetition reply with the single "
        "word ALL_DONE. (Do not write more than one loop file per step.)";
    slow_req.work_dir = work_dir;
    slow_req.description = "slow interrupt demo worker";
    slow_req.background = true;
    agents::agent_entry slow_entry;
    slow_entry.session_id = slow_req.session_id;
    slow_entry.created_at = registry.clock_now();
    slow_entry.last_accessed = registry.clock_now();
    slow_entry.is_active = true;
    slow_entry.state = "running";
    registry.put(slow_entry);
    registry.register_session(slow_req.session_id);
    const bool slow_started = registry.start_background(slow_req.session_id,
                                                        slow_req);
    std::printf("\n── pre-started background agent: %s ────────────────────\n",
                slow_started ? "interrupt_demo_agent (running)"
                             : "FAILED TO START");

    const kimix::string task_d =
        "A background sub-agent with session id \"interrupt_demo_agent\" is "
        "running right now in this session (it writes loop_XX.txt files). "
        "Complete ALL of the following steps in order, using exactly one tool "
        "call per step:\n"
        "1. Call the SendMessage tool with subagent_id=\"interrupt_demo_agent"
        "\" and message=\"status check: how many files have you written so "
        "far?\".\n"
        "2. Call the InterruptAgent tool with agent_id=\"interrupt_demo_agent"
        "\" to stop it.\n"
        "When both steps succeeded, reply with the single word DONE and "
        "nothing else.";

    std::printf("── turn D (send_message steer + interrupt_agent) ─────\n");
    kimix::agent::TurnResult tr_d = soul.turn(task_d, log_chunk);
    std::printf("\n── turn D result: ok=%d steps=%d ─────────────────────\n",
                static_cast<int>(tr_d.ok), tr_d.steps);

    // Collect the interrupted child's run result.
    agents::subagent_run_result slow_result;
    bool slow_joined = false;
    for (int i = 0; i < 240 && !slow_joined; ++i) {
        if (registry.run_finished(slow_req.session_id)) {
            slow_joined = registry.join_run(slow_req.session_id, slow_result);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    int loop_files = 0;
    for (int i = 1; i <= 10; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "loop_%02d.txt", i);
        if (kimix::filesystem::exists(ws / name, ec)) {
            ++loop_files;
        }
    }
    bool steer_seen = false;
    for (const agents::conversation_turn &t : slow_result.turns) {
        if (t.type == "steer" && contains(t.content, "status check")) {
            steer_seen = true;
        }
    }

    // ── 6. Turn E: Workflow fanout ──────────────────────────────────────────
    const kimix::string task_e =
        "Call the Workflow tool exactly once with these parameters: "
        "description=\"fanout demo\", mode=\"fanout\", prompt_template=\"Write "
        "a file named wf_{{item}}.txt containing exactly the word {{item}} "
        "uppercased and nothing else, then reply with the single word "
        "{{item}}.\", items=[\"alpha\", \"beta\"]. Wait for it to finish, "
        "then reply with the single word DONE and nothing else.";

    std::printf("\n── turn E (workflow fanout) ──────────────────────────\n");
    kimix::agent::TurnResult tr_e = soul.turn(task_e, log_chunk);
    std::printf("\n── turn E result: ok=%d steps=%d ─────────────────────\n",
                static_cast<int>(tr_e.ok), tr_e.steps);

    // ── 7. Turn F: Workflow parallel_sample (best-of-N) ─────────────────────
    const kimix::string task_f =
        "Call the Workflow tool exactly once with these parameters: "
        "description=\"best of n demo\", mode=\"parallel_sample\", "
        "sample_n=2, prompt_template=\"Write a file named poem.txt containing "
        "exactly the single line: the quick brown fox. Then reply with the "
        "single word WRITTEN.\" (the template has no {{item}} placeholder; "
        "that is intentional). Wait for it to finish, then reply with the "
        "single word DONE and nothing else.";

    std::printf("\n── turn F (workflow best-of-N) ───────────────────────\n");
    kimix::agent::TurnResult tr_f = soul.turn(task_f, log_chunk);
    std::printf("\n── turn F result: ok=%d steps=%d ─────────────────────\n",
                static_cast<int>(tr_f.ok), tr_f.steps);

    // ── 8. Evidence checks ──────────────────────────────────────────────────
    const kimix::string transcript = collect_transcript();

    bool file_ok = false;
    const kimix::string plan_text = read_file_text(ws / "plan.md", file_ok);
    bool sub_ok = false;
    const kimix::string sub_text = read_file_text(ws / "subagent_result.txt",
                                                  sub_ok);
    bool alpha_ok = false;
    const kimix::string alpha_text = read_file_text(ws / "wf_alpha.txt",
                                                    alpha_ok);
    bool beta_ok = false;
    const kimix::string beta_text = read_file_text(ws / "wf_beta.txt", beta_ok);
    bool poem_ok = false;
    const kimix::string poem_text = read_file_text(ws / "poem.txt", poem_ok);

    std::printf("\n── checks ────────────────────────────────────────────\n");
    std::printf("turn results: A=%d B=%d C=%d D=%d E=%d F=%d\n\n",
                static_cast<int>(tr_a.ok), static_cast<int>(tr_b.ok),
                static_cast<int>(tr_c.ok), static_cast<int>(tr_d.ok),
                static_cast<int>(tr_e.ok), static_cast<int>(tr_f.ok));

    // Turn A - Run + JobOutput
    check("A: turn completed with DONE", tr_a.ok && contains(tr_a.content, "DONE"));
    check("A: Run tool called", count_calls("run") >= 1);
    check("A: JobOutput called twice (list+get)", count_calls("job_output") >= 2);
    check("A: background output captured (E2E_BG_DONE)",
          contains(transcript, "E2E_BG_DONE"));
    check("A: terminal [status: completed] suffix",
          contains(transcript, "[status: completed]"));

    // Turn B - plan tools
    check("B: turn completed with DONE", tr_b.ok && contains(tr_b.content, "DONE"));
    check("B: WritePlan/ReadPlan/EditPlan all called",
          count_calls("writeplan") >= 1 && count_calls("readplan") >= 2 &&
              count_calls("editplan") >= 1);
    check("B: plan.md on disk has the edited step",
          file_ok && contains(plan_text, "- [x] step two: crunch data"));
    check("B: edit success message in transcript",
          contains(transcript, "successfully edited"));

    // Turn C - subagent + send_message + list_agents
    check("C: turn completed with DONE", tr_c.ok && contains(tr_c.content, "DONE"));
    check("C: Subagent called with sub_demo_agent",
          count_calls("subagent") >= 1 &&
              contains(transcript, "sub_demo_agent"));
    check("C: child wrote subagent_result.txt with SUBAGENT_OK",
          sub_ok && contains(sub_text, "SUBAGENT_OK"));
    check("C: SendMessage queued for closed session",
          count_calls("send_message") >= 1 &&
              contains(transcript, "Message queued"));
    check("C: message parked in registry (pending count == 1)",
          registry.pending_message_count("sub_demo_agent") == 1);
    check("C: ListAgents called", count_calls("list_agents") >= 1);

    // Turn D - steer + interrupt
    check("D: background agent pre-started", slow_started);
    check("D: turn completed with DONE", tr_d.ok && contains(tr_d.content, "DONE"));
    check("D: SendMessage delivered to running agent",
          contains(transcript, "Message delivered to agent "
                              "'interrupt_demo_agent'"));
    check("D: steer reached the child turn loop", steer_seen);
    check("D: InterruptAgent called",
          count_calls("interrupt_agent") >= 1 &&
              contains(transcript, "Session interrupt_demo_agent closed."));
    check("D: child run cancelled", slow_joined && slow_result.cancelled);
    check("D: child wrote at least one loop file before cancel",
          loop_files >= 1 && loop_files < 10);

    // Turn E - workflow fanout
    check("E: turn completed with DONE", tr_e.ok && contains(tr_e.content, "DONE"));
    check("E: Workflow fanout called", count_calls("workflow") >= 1 &&
                                           contains(transcript,
                                                    "<agent_swarm_result>"));
    check("E: wf_alpha.txt contains ALPHA",
          alpha_ok && contains(alpha_text, "ALPHA"));
    check("E: wf_beta.txt contains BETA", beta_ok && contains(beta_text, "BETA"));

    // Turn F - best of N
    check("F: turn completed with DONE", tr_f.ok && contains(tr_f.content, "DONE"));
    check("F: best-of-N result rendered",
          contains(transcript, "<best_of_n_result>"));
    check("F: poem.txt applied to main workspace",
          poem_ok && contains(poem_text, "the quick brown fox"));

    kimix::builtin_tools::proc::stop_all_tasks();
    std::printf("\n%s (%d failure(s))\n", failures == 0 ? "E2E PASS" : "E2E FAIL",
                failures);
    return failures == 0 ? 0 : 1;
}
