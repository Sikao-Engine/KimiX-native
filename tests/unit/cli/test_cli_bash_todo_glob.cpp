// test_cli_bash_todo_glob.cpp - crash repro for the real-CLI failure where a
// session died silently right after the model emitted three tool calls
// (bash `pwd && echo --- && uname -a 2>/dev/null || ver`, todo_list read,
// glob `*`) and the CLI printed their "⚡" headers: the session's wire.jsonl
// ends after the tool-call step with NO ToolResult record ever written (the
// WireWriter flushes per record, so no tool finished - the process died
// inside the first dispatch). Serial dispatch is the CLI default
// (dispatch_concurrency is not configurable from the CLI config at all), so
// this is NOT a parallel-dispatch race: the crash lives in the ordinary
// bash -> todo_list -> glob tool run inside the full app (renderer, wire
// writer, REPL reader thread blocked on stdin, session store).
//
// This suite drives the production stack in-process - app_init + repl_run
// with a scripted IChatBackend (no network) and a stdin PIPE whose reader
// thread blocks during the turn, exactly like the crashed interactive
// session - over many fresh sessions, and pins the post-condition the crash
// violated: all three ToolResult records reach wire.jsonl.
//
// Framework: Boost.UT (tests/ut/ut.hpp). Environment-dependent: needs a real
// Git Bash for the bash call; when bash is missing the suite prints a skip
// note and passes without exercising the tools (the test-process-runner
// precedent for probing the environment).

#include "ut/ut.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define pt_write _write
#define pt_close _close
#else
#include <unistd.h>
#define pt_write write
#define pt_close close
#endif

#include <core/kimix_core.h>

#include "agent/soul.h"
#include "builtin_tools/tool_registry.h"
#include "cli/cli_app.h"
#include "cli/cli_common.h"
#include "cli/cli_print.h"
#include "cli/cli_repl.h"
#include "cli/cli_stream.h"

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;

namespace {

// One scripted chat step: the text/reasoning/tool calls one backend call
// returns (and streams through the chunk callback).
struct pt_step {
    kimix::string content;
    kimix::string reasoning;
    kimix::vector<kimix::llm::ToolCall> calls;
};

// The scripted IChatBackend: returns steps[calls] and streams it exactly like
// a provider (text delta, reasoning delta, one chunk per tool call).
class pt_backend : public kimix::agent::IChatBackend {
public:
    kimix::vector<pt_step> steps;
    std::atomic<int32_t> calls{0};
    int64_t context_size = 1000;

    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &,
         const kimix::vector<kimix::llm::Tool> &,
         const kimix::llm::ChunkCallback &on_chunk,
         const kimix::llm::AbortCheck * /*abort*/) override {
        kimix::llm::ChatResult result;
        result.ok = true;
        const int32_t n = calls.fetch_add(1);
        if (static_cast<size_t>(n) < steps.size()) {
            const pt_step &step = steps[static_cast<size_t>(n)];
            result.content = step.content;
            result.reasoning = step.reasoning;
            result.tool_calls = step.calls;
        }
        if (on_chunk) {
            if (!result.reasoning.empty()) {
                kimix::llm::Chunk chunk;
                chunk.ok = true;
                chunk.reasoning = result.reasoning;
                on_chunk(chunk);
            }
            if (!result.content.empty()) {
                kimix::llm::Chunk chunk;
                chunk.ok = true;
                chunk.content = result.content;
                on_chunk(chunk);
            }
            for (const kimix::llm::ToolCall &call : result.tool_calls) {
                kimix::llm::Chunk chunk;
                chunk.ok = true;
                chunk.tool_calls.push_back(call);
                on_chunk(chunk);
            }
        }
        return result;
    }
    int64_t max_context_size() const override { return context_size; }
    kimix::string model_name() const override { return "scripted-test"; }
};

// Waits until the session's wire.jsonl carries a TurnEnd record (the
// WireWriter flushes per record, so this is exact), then feeds "/exit" so
// the REPL unwinds after the turn - never during it (mid-turn input would
// STEER the running turn, adding a call the crashed session never made).
struct pt_turn_watcher {
    kimix::string wire_path;
    bool turn_ended() {
        kimix::string wire;
        kimix::string wire_error;
        if (!cli::read_file(wire_path, wire, wire_error)) {
            return false;
        }
        return wire.find("\"type\":\"TurnEnd\"") != kimix::string::npos;
    }
};

kimix::string pt_ws(const char *name) {
    std::error_code ec;
    const kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) / name;
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

// A stdin pipe whose read end feeds the REPL and whose write end the test
// holds open: the CLI's reader thread blocks in fgetc for the whole turn,
// exactly like an interactive session waiting at the keyboard.
struct pt_stdin_pipe {
    std::FILE *read_end = nullptr;
    int write_fd = -1;

    bool open() {
        int fds[2] = {-1, -1};
#ifdef _WIN32
        if (_pipe(fds, 4096, _O_BINARY) != 0) {
            return false;
        }
        read_end = _fdopen(fds[0], "rb");
#else
        if (pipe(fds) != 0) {
            return false;
        }
        read_end = fdopen(fds[0], "rb");
#endif
        write_fd = fds[1];
        if (read_end == nullptr) {
            pt_close(fds[0]);
            pt_close(fds[1]);
            return false;
        }
        return true;
    }
    void send_line(const char *line) {
        if (write_fd >= 0) {
            pt_write(write_fd, line, static_cast<int>(std::strlen(line)));
        }
    }
    void close_write() {
        if (write_fd >= 0) {
            pt_close(write_fd);
            write_fd = -1;
        }
    }
    ~pt_stdin_pipe() { close_write(); }
};

// Redirect fd 1+2 into `path` for the duration of a test (the CLI's prints do
// not go through a swappable stream).
class pt_output_capture {
public:
    bool begin(const kimix::string &path) {
        path_ = path;
        std::fflush(stdout);
        std::fflush(stderr);
        file_ = std::fopen(path.c_str(), "wb");
        if (file_ == nullptr) {
            return false;
        }
        saved_out_ = dup_fd(1);
        saved_err_ = dup_fd(2);
        dup2_fd(fd_no(file_), 1);
        dup2_fd(fd_no(file_), 2);
        return true;
    }
    kimix::string end() {
        std::fflush(stdout);
        std::fflush(stderr);
        if (saved_out_ >= 0) {
            dup2_fd(saved_out_, 1);
            close_fd(saved_out_);
            saved_out_ = -1;
        }
        if (saved_err_ >= 0) {
            dup2_fd(saved_err_, 2);
            close_fd(saved_err_);
            saved_err_ = -1;
        }
        if (file_ != nullptr) {
            const long pos = std::ftell(file_);
            std::fseek(file_, 0, SEEK_SET);
            kimix::string out;
            if (pos > 0) {
                char buffer[4096];
                size_t read = 0;
                while ((read = std::fread(buffer, 1, sizeof(buffer), file_)) >
                       0) {
                    out.append(buffer, read);
                }
            }
            std::fclose(file_);
            file_ = nullptr;
            return out;
        }
        return kimix::string();
    }
    ~pt_output_capture() { end(); }

private:
    static int dup_fd(int fd) {
#ifdef _WIN32
        return _dup(fd);
#else
        return dup(fd);
#endif
    }
    static void dup2_fd(int from, int to) {
#ifdef _WIN32
        _dup2(from, to);
#else
        dup2(from, to);
#endif
    }
    static void close_fd(int fd) {
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
    }
    static int fd_no(std::FILE *f) {
#ifdef _WIN32
        return _fileno(f);
#else
        return fileno(f);
#endif
    }
    kimix::string path_;
    std::FILE *file_ = nullptr;
    int saved_out_ = -1;
    int saved_err_ = -1;
};

// A configured app_context with a scripted backend (no network access): the
// provider JSON is real, so the config -> soul wiring (tools, limits,
// prompts) is exercised for real.
struct pt_fixture {
    kimix::string work;
    kimix::string provider;
    cli::cli_options opts;
    cli::app_context app;
    pt_backend backend;
    std::FILE *render_out = nullptr;
    cli::stream_renderer renderer{true, true};
    kimix::string error;

    bool init(const char *name) {
        work = pt_ws(name);
        // KIMIX_BTG_WS overrides the work directory so the suite can run the
        // tools against the exact directory a crashed session was working
        // in (e.g. the real home directory, with everything that is in it).
        if (const char *env = std::getenv("KIMIX_BTG_WS")) {
            if (*env != '\0') {
                work = kimix::string(env);
            }
        }
        provider = cli::join_path(work, "provider.json");
        kimix::string write_error;
        // No loop_control overrides: every reminder/meter/auto-retrieval
        // provider keeps its default state, matching the real session's
        // config (~/.kimi/config.toml defaults) far more closely than the
        // sanitized fixture variants.
        const kimix::string json =
            "{\"model\":\"scripted-test-model\",\"type\":\"openai\","
            "\"url\":\"http://127.0.0.1:1/v1\",\"api_key\":\"test\","
            "\"max_context_size\":128000,\"max_tokens\":100}";
        if (!cli::write_file(provider, json, write_error)) {
            error = write_error;
            return false;
        }
        opts.config_path = provider;
        opts.config_is_provider_only = true;
        opts.work_dir = work;
        opts.no_color = true;
        render_out = std::tmpfile();
        renderer.set_output(render_out);
        app.renderer = &renderer;
        return cli::app_init(opts, app, error, &backend);
    }
    kimix::string rendered() {
        std::fflush(render_out);
        const long pos = std::ftell(render_out);
        std::fseek(render_out, 0, SEEK_SET);
        kimix::string out;
        if (pos > 0) {
            char buffer[4096];
            size_t read = 0;
            while ((read = std::fread(buffer, 1, sizeof(buffer), render_out)) >
                   0) {
                out.append(buffer, read);
            }
            std::fseek(render_out, pos, SEEK_SET);
        }
        return out;
    }
    kimix::string session_file(const char *name) {
        return cli::join_path(app.store.dir(), name);
    }
    void shutdown() {
        app.soul.reset();
        app.session.reset();
        kimix::string close_error;
        app.store.close(true, close_error);
    }
};

// The exact three tool calls of the crashed session.
kimix::vector<kimix::llm::ToolCall> pt_crashed_session_calls() {
    kimix::vector<kimix::llm::ToolCall> calls;
    const char *ids[3] = {"call_bash", "call_todo", "call_glob"};
    const char *names[3] = {"bash", "todo_list", "glob"};
    const char *args[3] = {
        R"JSON({"command":"pwd && echo --- && uname -a 2>/dev/null || ver"})JSON",
        R"JSON({})JSON",
        R"JSON({"pattern":"*"})JSON",
    };
    for (int i = 0; i < 3; ++i) {
        kimix::llm::ToolCall call;
        call.id.assign(ids[i]);
        call.name.assign(names[i]);
        call.arguments = args[i];
        calls.push_back(std::move(call));
    }
    return calls;
}

} // namespace

int main() {
    // Environment probe: the repro needs the REAL bash spawn; without Git
    // Bash the bash call is refused before it ever runs and there is nothing
    // to reproduce. Skip loudly but pass.
    {
        kimix::builtin_tools::Session probe_session;
        probe_session.native_io = true;
        kimix::unique_ptr<kimix::builtin_tools::Tool> probe =
            kimix::builtin_tools::ToolRegistry::instance().create("bash",
                                                                  &probe_session);
        if (probe == nullptr || !probe->valid()) {
            std::fprintf(stderr,
                         "SKIP: no usable bash in this environment - the "
                         "bash/todo_list/glob crash repro needs a real "
                         "spawn.\n");
            return 0;
        }
    }

    "cli_bash_todo_glob_serial_dispatch_survives"_test = [] {
        int iters = 20;
        if (const char *env = std::getenv("KIMIX_BTG_ITERS")) {
            if (*env != '\0') {
                iters = std::atoi(env);
            }
        }
        for (int i = 0; i < iters; ++i) {
            const kimix::string name =
                kimix::format("kimix_btg_repro_{}", i);
            pt_fixture fx;
            expect(fx.init(name.c_str())) << "app_init: " << fx.error;
            fx.backend.steps.push_back({"", "", pt_crashed_session_calls()});
            fx.backend.steps.push_back({"finished", "", {}});

            pt_stdin_pipe pipe;
            expect(pipe.open()) << "stdin pipe opens";
            pt_output_capture capture;
            expect(capture.begin(cli::join_path(fx.work, "stdout.txt")));

            // Closes the pipe once the turn's TurnEnd record lands in
            // wire.jsonl (the WireWriter flushes per record): the reader
            // thread then sees EOF and the REPL unwinds via its "bye." path
            // - which, unlike /exit, keeps app.session alive for the
            // post-conditions below.
            std::thread feeder([&]() {
                pt_turn_watcher watcher{fx.session_file("wire.jsonl")};
                for (int spin = 0; spin < 6000; ++spin) {
                    if (watcher.turn_ended()) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                pipe.close_write();
            });

            kimix::vector<kimix::string> scripted;
            scripted.push_back("try call some tools and report");
            cli::set_colorful(false);
            const int code = cli::repl_run(fx.app, pipe.read_end, stdout,
                                           scripted);
            capture.end();
            cli::set_colorful(true);
            feeder.join();
            std::fclose(pipe.read_end);

            expect(eq(code, 0)) << "iteration " << i << " exits cleanly";
            expect(fx.backend.calls.load() >= 2)
                << "iteration " << i
                << " ran the tool step plus at least one more step";

            // The post-condition the crash violated: the turn completed, so
            // all three tool results are in the session history.
            size_t tool_results = 0;
            for (const kimix::llm::Message &m : fx.app.session->history()) {
                if (m.role == "tool") {
                    ++tool_results;
                }
            }
            expect(eq(tool_results, size_t(3)))
                << "iteration " << i
                << " recorded all three tool results (the crash left none)";

            fx.shutdown();
        }
    };

    return 0;
}
