// soul_e2e.cpp - End-to-end agent test against a real LLM provider.
//
// Usage: xmake run soul_e2e [config.json] [work_dir]
//   config.json defaults to D:/ds_cmdcode.json (openai_legacy deepseek proxy)
//
// The binary:
//   1. creates an AgentSession in a scratch work dir (native_io tools)
//   2. drives one KimiSoul turn that asks the model to exercise every core
//      capability: write a file, read it back, run a POSIX bash command,
//      grep a directory, glob a pattern
//   3. performs a manual compaction (compact_context) and reports the
//      before/after history sizes and token estimates
//   4. verifies the transcript contains evidence of every capability and
//      prints PASS/FAIL per check (exit code 0 only when all pass).

#include <cstdio>

#include <core/kimix_core.h>

#include "agent/soul.h"
#include "builtin_tools/process_runner.h"
#include "llm/llm.h"

namespace {

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

} // namespace

int main(int argc, char *argv[]) {
    const kimix::string config_path =
        argc > 1 ? argv[1] : kimix::string("D:/ds_cmdcode.json");

    // ── 1. Session + workspace ──────────────────────────────────────────────
    std::error_code ec;
    kimix::filesystem::path ws =
        kimix::filesystem::temp_directory_path(ec) / "kimix_soul_e2e";
    kimix::filesystem::remove_all(ws, ec);
    kimix::filesystem::create_directories(ws, ec);
    const kimix::string work_dir = kimix::to_string(ws);

    kimix::agent::AgentSession session(work_dir);
    std::printf("session id: %s\nwork dir:   %s\n", session.id().c_str(),
                session.work_dir().c_str());

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
    opts.enabled_tools = {"read", "write", "bash", "grep", "glob", "edit"};
    opts.max_steps = 24;
    opts.auto_compact = false; // the e2e drives compaction explicitly
    kimix::agent::KimiSoul soul(session, backend, opts);

    // ── 2. One turn exercising every capability ────────────────────────────
    const kimix::string task =
        "Work in the current directory and complete ALL of the following "
        "steps, using one tool call per step:\n"
        "1. Use the Write tool to create `report.txt` with exactly this "
        "content:\n"
        "alpha line\nbeta needle line\ngamma line\n"
        "2. Use the Read tool to read `report.txt` back.\n"
        "3. Use the Bash tool (POSIX syntax) to run: "
        "`wc -l report.txt && echo E2E_BASH_OK`.\n"
        "4. Use the Grep tool to search for the regex `needle` in the current "
        "directory with output_mode content.\n"
        "5. Use the Glob tool to list `*.txt` in the current directory.\n"
        "When every step succeeded, reply with the single word DONE and "
        "nothing else.";

    std::printf("── turn ──────────────────────────────────────────────\n");
    const kimix::agent::TurnResult tr = soul.turn(task, log_chunk);
    std::printf("\n── turn result: ok=%d steps=%d ───────────────────────\n",
                static_cast<int>(tr.ok), tr.steps);
    if (!tr.ok) {
        std::fprintf(stderr, "turn failed: %s\n", tr.error.c_str());
    }

    // ── 3. Evidence checks over the transcript ─────────────────────────────
    kimix::string transcript;
    bool saw_write_call = false;
    bool saw_read_call = false;
    bool saw_bash_call = false;
    bool saw_grep_call = false;
    bool saw_glob_call = false;
    for (const kimix::llm::Message &m : session.history()) {
        transcript += m.role;
        transcript += '\n';
        transcript += m.content;
        transcript += '\n';
        for (const kimix::llm::ToolCall &tc : m.tool_calls) {
            if (tc.name == "write") {
                saw_write_call = true;
            } else if (tc.name == "read") {
                saw_read_call = true;
            } else if (tc.name == "bash") {
                saw_bash_call = true;
            } else if (tc.name == "grep") {
                saw_grep_call = true;
            } else if (tc.name == "glob") {
                saw_glob_call = true;
            }
        }
    }

    const bool file_written =
        kimix::filesystem::exists(ws / "report.txt", ec);
    kimix::string file_content;
    if (file_written) {
        std::FILE *f = std::fopen(kimix::to_string(ws / "report.txt").c_str(),
                                  "rb");
        if (f != nullptr) {
            char buf[4096];
            size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = '\0';
            file_content.assign(buf, n);
            std::fclose(f);
        }
    }
    const bool read_worked =
        contains(transcript, "beta needle line") && saw_read_call;
    const bool bash_worked = contains(transcript, "E2E_BASH_OK") && saw_bash_call;
    const bool grep_worked =
        (contains(transcript, "report.txt") && saw_grep_call) &&
        (contains(transcript, "needle"));
    const bool glob_worked =
        contains(transcript, "report.txt") && saw_glob_call;
    const bool turn_ok = tr.ok && contains(tr.content, "DONE");

    int failures = 0;
    auto check = [&](const char *name, bool ok) {
        std::printf("[%-28s] %s\n", name, ok ? "PASS" : "FAIL");
        if (!ok) {
            ++failures;
        }
    };
    std::printf("\n── checks ────────────────────────────────────────────\n");
    check("turn completed with DONE", turn_ok);
    check("Write tool called", saw_write_call);
    check("file exists on disk", file_written);
    check("file content correct", contains(file_content, "beta needle line"));
    check("Read tool returned content", read_worked);
    check("Bash tool ran posix command", bash_worked);
    check("Grep tool found pattern", grep_worked);
    check("Glob tool listed files", glob_worked);

    // ── 4. Compaction ───────────────────────────────────────────────────────
    const size_t before_msgs = session.history().size();
    const int64_t before_tokens = soul.estimated_tokens();
    kimix::string cerr;
    const bool compacted =
        soul.compact_context("remember the E2E evidence checks", cerr);
    const size_t after_msgs = session.history().size();
    const int64_t after_tokens = soul.estimated_tokens();
    std::printf("\n── compaction ────────────────────────────────────────\n");
    std::printf("messages: %zu -> %zu   est tokens: %lld -> %lld\n",
                before_msgs, after_msgs, (long long)before_tokens,
                (long long)after_tokens);
    if (compacted && !session.history().empty()) {
        std::printf("\n── summary message (first 1500 chars) ──\n%.1500s\n──\n",
                    session.history().front().content.c_str());
    }
    check("compaction succeeded", compacted);
    check("history shrank", compacted && after_msgs < before_msgs);
    check("tokens shrank", compacted && after_tokens < before_tokens);
    if (!compacted) {
        std::fprintf(stderr, "compact error: %s\n", cerr.c_str());
    }

    // ── 5. Post-compaction turn: the model must still know the state ───────
    std::printf("\n── post-compaction turn ──────────────────────────────\n");
    const kimix::agent::TurnResult tr2 = soul.turn(
        "Without calling any tools, answer in one sentence: what file did we "
        "create earlier, and which word did we grep for?",
        log_chunk);
    std::printf("\n");
    const bool memory_kept =
        tr2.ok && (contains(tr2.content, "report.txt") ||
                   contains(tr2.content, "report"));
    check("summary retained task state", memory_kept);

    kimix::builtin_tools::proc::stop_all_tasks();
    std::printf("\n%s (%d failure(s))\n", failures == 0 ? "E2E PASS" : "E2E FAIL",
                failures);
    return failures == 0 ? 0 : 1;
}
