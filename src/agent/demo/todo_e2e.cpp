// todo_e2e.cpp - End-to-end test of the TodoWrite/TodoUpdate agent tools
// against a real LLM provider, including session persistence (state.json).
//
// Usage: xmake run todo_e2e [config.json]
//   config.json defaults to C:/dev/ds_ucloud.json (openai_legacy provider)
//
// The binary:
//   1. creates an AgentSession with a dedicated state_dir (native_io tools)
//   2. drives one KimiSoul turn whose prompt asks the model to build a todo
//      tree with TodoWrite, progress it with TodoUpdate (batch + complete),
//      and read it back with TodoWrite (no parameters)
//   3. verifies the transcript shows both tool calls and that the persisted
//      <state_dir>/state.json holds the expected statuses
//   4. simulates a session restart: a SECOND AgentSession loads the state
//      from the same state_dir and another turn asks the model to continue
//      work from the persisted list (proving load-with-session through the
//      real LLM loop)
//   5. prints PASS/FAIL per check; exit code 0 only when all pass.

#include <cstdio>

#include <core/kimix_core.h>

#include "agent/soul.h"
#include "builtin_tools/todo_tool.h"
#include "llm/llm.h"

namespace {

namespace todo = kimix::builtin_tools::todo;

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

// Find a root todo by title in a state (null when absent).
const todo::todo_item *find_root(const todo::todo_state &st,
                                 kimix::string_view title) {
    for (const todo::todo_item &it : st.todos) {
        if (it.content == title) {
            return &it;
        }
    }
    return nullptr;
}

} // namespace

int main(int argc, char *argv[]) {
    const kimix::string config_path =
        argc > 1 ? argv[1] : kimix::string("C:/dev/ds_ucloud.json");

    // ── 1. Session + workspace + state dir ─────────────────────────────────
    std::error_code ec;
    kimix::filesystem::path ws =
        kimix::filesystem::temp_directory_path(ec) / "kimix_todo_e2e";
    kimix::filesystem::remove_all(ws, ec);
    kimix::filesystem::create_directories(ws, ec);
    const kimix::string work_dir = kimix::to_string(ws);
    const kimix::string state_dir = kimix::to_string(ws / "session");
    kimix::filesystem::create_directories(kimix::filesystem::path(state_dir),
                                          ec);

    kimix::agent::AgentSession session(work_dir);
    session.set_state_dir(state_dir);
    std::printf("session id: %s\nwork dir:   %s\nstate dir:  %s\n",
                session.id().c_str(), session.work_dir().c_str(),
                session.state_dir().c_str());

    auto llm = kimix::llm::create_llm_from_file(config_path);
    if (llm == nullptr) {
        std::fprintf(stderr, "failed to load LLM config from %s\n",
                     config_path.c_str());
        return 2;
    }
    std::printf("provider:   model=%s type=%s ctx=%d\n\n",
                llm->model_name().c_str(), llm->config().type.c_str(),
                llm->max_context_size());
    kimix::agent::LLMBackend backend(std::move(llm));

    kimix::agent::KimiSoul::options opts;
    opts.enabled_tools = {"TodoWrite", "TodoUpdate"};
    opts.max_steps = 24;
    opts.auto_compact = false;
    kimix::agent::KimiSoul soul(session, backend, opts);

    // ── 2. Turn 1: build + progress the todo list through the tools ────────
    const kimix::string task =
        "You are tracking a small development task list. Complete ALL of the "
        "following steps in order, using exactly one tool call per step:\n"
        "1. Call TodoWrite with `todos` set to this complete list:\n"
        "   - \"Write design doc\" with status \"in_progress\" and notes "
        "\"draft the API section first\"\n"
        "   - \"Implement feature\" with status \"pending\"\n"
        "   - \"Run tests\" with status \"pending\"\n"
        "2. Call TodoUpdate once with `updates` containing two edits: set "
        "\"Write design doc\" status to \"done\", and set \"Implement "
        "feature\" status to \"in_progress\".\n"
        "3. Call TodoUpdate with title=\"Implement feature\" and "
        "complete=true to finish that item and its subtree.\n"
        "4. Call TodoWrite with NO parameters (empty arguments {}) to read "
        "the current todo tree back.\n"
        "When all four steps succeeded, reply with the single word DONE and "
        "nothing else.";

    std::printf("── turn 1 (create + progress) ────────────────────────\n");
    const kimix::agent::TurnResult tr = soul.turn(task, log_chunk);
    std::printf("\n── turn 1 result: ok=%d steps=%d ─────────────────────\n",
                static_cast<int>(tr.ok), tr.steps);
    if (!tr.ok) {
        std::fprintf(stderr, "turn failed: %s\n", tr.error.c_str());
    }

    // ── 3. Evidence checks over transcript + persisted state ───────────────
    bool saw_write_call = false;
    bool saw_update_call = false;
    kimix::string transcript;
    for (const kimix::llm::Message &m : session.history()) {
        transcript += m.role;
        transcript += '\n';
        transcript += m.content;
        transcript += '\n';
        for (const kimix::llm::ToolCall &tc : m.tool_calls) {
            if (tc.name == "TodoWrite") {
                saw_write_call = true;
            } else if (tc.name == "TodoUpdate") {
                saw_update_call = true;
            }
        }
    }

    const kimix::string state_path = todo::state_file_path(state_dir);
    todo::todo_state persisted;
    kimix::string lerr;
    const bool state_loaded =
        todo::load_state_file(state_path, persisted, lerr);
    if (!state_loaded) {
        std::fprintf(stderr, "state load error: %s\n", lerr.c_str());
    }
    const todo::todo_item *design = find_root(persisted, "Write design doc");
    const todo::todo_item *impl = find_root(persisted, "Implement feature");
    const todo::todo_item *tests = find_root(persisted, "Run tests");

    int failures = 0;
    auto check = [&](const char *name, bool ok) {
        std::printf("[%-38s] %s\n", name, ok ? "PASS" : "FAIL");
        if (!ok) {
            ++failures;
        }
    };
    std::printf("\n── checks (turn 1) ───────────────────────────────────\n");
    check("turn 1 completed with DONE", tr.ok && contains(tr.content, "DONE"));
    check("TodoWrite tool called", saw_write_call);
    check("TodoUpdate tool called", saw_update_call);
    check("state.json exists on disk",
          kimix::filesystem::exists(kimix::filesystem::path(state_path), ec));
    check("state.json parses", state_loaded);
    check("3 root todos persisted", persisted.todos.size() == 3);
    check("\"Write design doc\" done",
          design != nullptr && design->status == todo::todo_status::done);
    check("\"Implement feature\" done (complete=true)",
          impl != nullptr && impl->status == todo::todo_status::done);
    check("\"Run tests\" still pending",
          tests != nullptr && tests->status == todo::todo_status::pending);
    check("read-back visible in transcript",
          contains(transcript, "Current todo list:"));

    // ── 4. Session restart: a fresh session LOADS the list, model continues ─
    std::printf("\n── turn 2 (fresh session, loaded state) ──────────────\n");
    kimix::agent::AgentSession session2(work_dir);
    session2.set_state_dir(state_dir);
    kimix::string lerr2;
    const bool reloaded = session2.load_state(lerr2);
    if (!reloaded) {
        std::fprintf(stderr, "reload error: %s\n", lerr2.c_str());
    }
    check("session2 load_state() succeeded", reloaded);
    const bool mem_ok =
        session2.tool_session().todo_state != nullptr &&
        session2.tool_session().todo_state->todos.size() == 3;
    check("session2 in-memory todo cache has 3 items", mem_ok);

    kimix::agent::KimiSoul soul2(session2, backend, opts);
    const kimix::string task2 =
        "A previous session left a todo list persisted for you. Do the "
        "following, one tool call per step:\n"
        "1. Call TodoWrite with NO parameters (empty arguments {}) to read "
        "the current todo list.\n"
        "2. Call TodoUpdate with title=\"Run tests\" and "
        "status=\"in_progress\".\n"
        "Then answer in one short sentence: which items are already done?\n";
    const kimix::agent::TurnResult tr2 = soul2.turn(task2, log_chunk);
    std::printf("\n── turn 2 result: ok=%d steps=%d ─────────────────────\n",
                static_cast<int>(tr2.ok), tr2.steps);

    todo::todo_state after2;
    const bool state2_loaded = todo::load_state_file(state_path, after2, lerr2);
    const todo::todo_item *tests2 = find_root(after2, "Run tests");
    std::printf("\n── checks (turn 2) ───────────────────────────────────\n");
    check("turn 2 completed", tr2.ok);
    check("model saw persisted items",
          contains(transcript, "Implement feature") ||
              contains(tr2.content, "Implement feature") ||
              contains(tr2.content, "done"));
    check("state.json reparses after turn 2", state2_loaded);
    check("\"Run tests\" now in_progress (persisted)",
          tests2 != nullptr &&
              tests2->status == todo::todo_status::in_progress);
    check("\"Write design doc\" survived the restart",
          find_root(after2, "Write design doc") != nullptr &&
              find_root(after2, "Write design doc")->status ==
                  todo::todo_status::done);

    std::printf("\n%s (%d failure(s))\n", failures == 0 ? "E2E PASS" : "E2E FAIL",
                failures);
    return failures == 0 ? 0 : 1;
}
