// process_runner.cpp - reproc-backed async subprocess execution
// (see process_runner.h).
//
// Windows stream strategy: reproc implements pipes as loopback TCP sockets
// (pipe.windows.c socketpair), and MSYS2/Git Bash cannot dup2() socket-backed
// fds - any `>&2` redirection dies with "cannot duplicate fd: Bad file
// descriptor". To stay compatible with Git Bash (the reference shell of the
// kimi-agent bash tool), stdout AND stderr are redirected to temp FILES
// (REPROC_REDIRECT_PATH) and tailed back into the merged capture buffer by
// the poll loop. stdin stays a reproc pipe (writing works fine). On POSIX,
// reproc pipes are real OS pipes and MSYS does not exist, so plain
// nonblocking pipe polling is used there.
//
// The foreground loop mirrors background/utils.py wait_for_output ordering:
// drain -> wait-pattern check -> total-timeout kill -> inactivity stop, with
// the pure capture_machine owning the bounded-append and pattern policy.

#include "builtin_tools/process_runner.h"

#include <reproc/reproc.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#ifdef KIMIX_PLATFORM_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#endif

#include <core/clock.h>
#include <core/spin_mutex.h>

#include "builtin_tools/bash_tool.h"

namespace kimix::builtin_tools::proc {

namespace {

using bash::capture_config;
using bash::capture_event;
using bash::capture_machine;

constexpr size_t k_read_buf_size = 65536;

int64_t pr_now_ms() {
    return static_cast<int64_t>(kimix::Clock::now_ms());
}

struct argv_holder {
    kimix::vector<const char *> ptrs;
    const char *const *data() const { return ptrs.data(); }
};

argv_holder pr_make_argv(const kimix::vector<kimix::string> &argv) {
    argv_holder h;
    h.ptrs.reserve(argv.size() + 1);
    for (const kimix::string &a : argv) {
        h.ptrs.push_back(a.c_str());
    }
    h.ptrs.push_back(nullptr);
    return h;
}

argv_holder pr_make_env(const kimix::vector<kimix::string> &env) {
    argv_holder h;
    h.ptrs.reserve(env.size() + 1);
    for (const kimix::string &e : env) {
        h.ptrs.push_back(e.c_str());
    }
    h.ptrs.push_back(nullptr);
    return h;
}

// A unique temp file path for stream redirection.
kimix::string pr_temp_path(kimix::string_view prefix) {
    static std::atomic<uint64_t> seq{0};
    const uint64_t n = seq.fetch_add(1);
    std::error_code ec;
    kimix::filesystem::path tmp = kimix::filesystem::temp_directory_path(ec);
    kimix::string name(prefix);
#ifdef KIMIX_PLATFORM_WINDOWS
    name += kimix::format("_{}_{}.tmp",
                          static_cast<uint64_t>(::GetCurrentProcessId()), n);
#else
    name += kimix::format("_{}_{}.tmp", static_cast<uint64_t>(::getpid()), n);
#endif
    return kimix::to_string(tmp / name);
}

// Tail one redirect file: read everything appended since the last tail and
// return it (advancing the internal offset).
class file_tail {
public:
    explicit file_tail(kimix::string path) : _path(std::move(path)) {}
    ~file_tail() {
        close();
        if (!_path.empty()) {
            std::remove(_path.c_str());
        }
    }
    file_tail(const file_tail &) = delete;
    file_tail &operator=(const file_tail &) = delete;

    const kimix::string &path() const { return _path; }

    // Read newly appended bytes. Returns false when the file cannot be
    // opened (yet) - callers treat that as "no data".
    bool read_new(kimix::string &out) {
        out.clear();
        if (_path.empty()) {
            return false;
        }
        std::FILE *f = std::fopen(_path.c_str(), "rb");
        if (f == nullptr) {
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        if (size > static_cast<long>(_offset)) {
            std::fseek(f, static_cast<long>(_offset), SEEK_SET);
            char buf[k_read_buf_size];
            size_t n = 0;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
                out.append(buf, n);
            }
            _offset = static_cast<size_t>(size);
        }
        std::fclose(f);
        return true;
    }

    void close() {
        // Nothing persistent is held between read_new calls.
    }

private:
    kimix::string _path;
    size_t _offset = 0;
};

reproc_options pr_base_options(const run_options &opts, const argv_holder &env,
                               const kimix::string &out_path,
                               const kimix::string &err_path) {
    reproc_options o{};
    o.working_directory = opts.working_directory.empty()
                              ? nullptr
                              : opts.working_directory.c_str();
    o.env.behavior = REPROC_ENV_EXTEND;
    o.env.extra = env.ptrs.size() > 1 ? env.data() : nullptr;
    o.redirect.in.type = REPROC_REDIRECT_PIPE;
#ifdef KIMIX_PLATFORM_WINDOWS
    // MSYS-safe file redirection (see file header).
    o.redirect.out.type = out_path.empty() ? REPROC_REDIRECT_PIPE
                                           : REPROC_REDIRECT_PATH;
    if (!out_path.empty()) {
        o.redirect.out.path = out_path.c_str();
    }
    // Merge stderr into the stdout FILE (dup2 between two real file handles
    // works in MSYS; dup2 onto a socket-backed pipe does not).
    o.redirect.err.type = out_path.empty() ? REPROC_REDIRECT_PATH
                                           : REPROC_REDIRECT_STDOUT;
    if (out_path.empty() && !err_path.empty()) {
        o.redirect.err.path = err_path.c_str();
    }
    o.nonblocking = false; // no socket pipes to poll on Windows
#else
    (void)out_path;
    (void)err_path;
    o.redirect.out.type = REPROC_REDIRECT_PIPE;
    o.redirect.err.type = REPROC_REDIRECT_PIPE;
    o.nonblocking = true;
#endif
    o.stop.first.action = REPROC_STOP_TERMINATE;
    o.stop.first.timeout = 2000;
    o.stop.second.action = REPROC_STOP_KILL;
    o.stop.second.timeout = 0;
    o.stop.third.action = REPROC_STOP_NOOP;
    o.stop.third.timeout = 0;
    return o;
}

capture_config pr_capture_config(const run_options &opts) {
    capture_config c;
    c.timeout_ms = opts.timeout_ms;
    c.inactivity_timeout_ms = opts.inactivity_timeout_ms;
    c.output_cap_chars = opts.output_cap_chars;
    c.wait_pattern = opts.wait_pattern;
    return c;
}

} // namespace

kimix::string sanitize_utf8(kimix::string_view bytes) {
    // Fast path: pure ASCII needs no copying decision - reuse the bytes as-is.
    bool ascii = true;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (static_cast<unsigned char>(bytes[i]) >= 0x80) {
            ascii = false;
            break;
        }
    }
    if (ascii) {
        return kimix::string(bytes);
    }
    static const char kRepl[] = "\xEF\xBF\xBD"; // U+FFFD
    kimix::string out;
    out.reserve(bytes.size());
    const auto at = [&bytes](size_t p) {
        return static_cast<unsigned char>(bytes[p]);
    };
    size_t i = 0;
    while (i < bytes.size()) {
        const unsigned char c = at(i);
        if (c < 0x80) {
            out.push_back(bytes[i]);
            ++i;
            continue;
        }
        // Expected continuation-byte count + accepted second-byte range
        // (the range excludes overlongs, surrogates and > U+10FFFF).
        size_t need = 0;
        unsigned char lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) {
            need = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            need = 2;
            if (c == 0xE0) {
                lo = 0xA0;
            }
            if (c == 0xED) {
                hi = 0x9F; // UTF-16 surrogates are not scalar values
            }
        } else if (c >= 0xF0 && c <= 0xF4) {
            need = 3;
            if (c == 0xF0) {
                lo = 0x90;
            }
            if (c == 0xF4) {
                hi = 0x8F;
            }
        } else {
            out.append(kRepl, 3); // stray continuation or invalid lead
            ++i;
            continue;
        }
        // Truncated sequence at the end of the input: CPython's "unexpected
        // end of data" replaces the whole tail with ONE U+FFFD.
        if (i + 1 + need > bytes.size()) {
            out.append(kRepl, 3);
            break;
        }
        const unsigned char c2 = at(i + 1);
        if (c2 < lo || c2 > hi) {
            out.append(kRepl, 3); // invalid second byte: rescan from it
            ++i;
            continue;
        }
        bool ok = true;
        for (size_t k = 2; k <= need; ++k) {
            const unsigned char ck = at(i + k);
            if (ck < 0x80 || ck > 0xBF) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            out.append(kRepl, 3); // invalid later continuation: rescan there
            ++i;
            continue;
        }
        out.append(bytes.data() + i, 1 + need);
        i += 1 + need;
    }
    return out;
}

namespace {

// Feed one chunk into the capture machine (skips empty chunks). Raw child
// bytes are sanitized to valid UTF-8 first - see sanitize_utf8 above.
void pr_feed(capture_machine &m, kimix::string_view chunk, int64_t elapsed_ms) {
    if (chunk.empty()) {
        return;
    }
    capture_event ev;
    ev.type = capture_event::kind::chunk;
    ev.text = sanitize_utf8(chunk);
    ev.elapsed_ms = elapsed_ms;
    m.on_event(ev);
}

} // namespace

run_result run_process(const run_options &opts) {
    run_result res;
    if (opts.argv.empty()) {
        res.status = tool_status::invalid_input;
        res.spawn_error = "empty argv";
        return res;
    }
    const argv_holder argv = pr_make_argv(opts.argv);
    const argv_holder env = pr_make_env(opts.extra_env);

#ifdef KIMIX_PLATFORM_WINDOWS
    // stdout goes to a temp file; stderr is merged into it (see
    // pr_base_options). opts.stderr_path overrides the stdout file location.
    const kimix::string out_path =
        opts.stderr_path.empty() ? pr_temp_path("reproc_out") : opts.stderr_path;
    file_tail out_tail(out_path);
    reproc_options o = pr_base_options(opts, env, out_path, kimix::string());
#else
    file_tail out_tail(kimix::string());
    reproc_options o = pr_base_options(opts, env, {}, {});
#endif

    reproc_t *p = reproc_new();
    if (p == nullptr) {
        res.status = tool_status::external_library;
        res.spawn_error = "reproc_new failed";
        return res;
    }
    const int sr = reproc_start(p, argv.data(), o);
    if (sr < 0) {
        res.status = tool_status::invalid_input;
        res.spawn_error = kimix::string("spawn failed: ") + reproc_strerror(sr);
        p = reproc_destroy(p);
        return res;
    }
    if (!opts.stdin_input.empty()) {
        reproc_write(p, reinterpret_cast<const uint8_t *>(opts.stdin_input.data()),
                     opts.stdin_input.size());
    }
    reproc_close(p, REPROC_STREAM_IN);

    const int64_t start_ms = pr_now_ms();
    capture_machine machine(pr_capture_config(opts));
    kimix::optional<int64_t> exit_code;
    bool finished = false;
    uint8_t buf[k_read_buf_size];

    // One drain pass over every live stream source.
    auto drain_all = [&]() {
        const int64_t elapsed = pr_now_ms() - start_ms;
#ifdef KIMIX_PLATFORM_WINDOWS
        kimix::string chunk;
        if (out_tail.read_new(chunk)) {
            pr_feed(machine, chunk, elapsed);
        }
#else
        for (REPROC_STREAM stream : {REPROC_STREAM_OUT, REPROC_STREAM_ERR}) {
            while (true) {
                const int r = reproc_read(p, stream, buf, sizeof(buf));
                if (r <= 0) {
                    break; // EWOULDBLOCK / EPIPE / error
                }
                pr_feed(machine,
                        kimix::string_view(reinterpret_cast<const char *>(buf),
                                           static_cast<size_t>(r)),
                        elapsed);
                if (static_cast<size_t>(r) < sizeof(buf)) {
                    break;
                }
            }
        }
#endif
    };

    while (!finished) {
        // Wait up to 100ms for the exit event (Windows: no pollable pipes,
        // so reproc_wait doubles as the poll tick; POSIX: reproc_poll).
        int wr = -1;
#ifdef KIMIX_PLATFORM_WINDOWS
        wr = reproc_wait(p, 100);
#else
        reproc_event_source source{};
        source.process = p;
        source.interests =
            REPROC_EVENT_OUT | REPROC_EVENT_ERR | REPROC_EVENT_EXIT;
        const int pr = reproc_poll(&source, 1, 100);
        if (pr >= 0) {
            if ((source.events & REPROC_EVENT_EXIT) != 0) {
                wr = reproc_wait(p, 0);
            }
        }
#endif
        drain_all();
        if (wr >= 0) {
            // Final drain after the exit.
            drain_all();
            capture_event ex;
            ex.type = capture_event::kind::process_exited;
            ex.exit_code = wr;
            ex.elapsed_ms = pr_now_ms() - start_ms;
            machine.on_event(ex);
            exit_code = machine.exit_code();
            finished = true;
            break;
        }
        // Early-stop policy (background/utils.py wait_for_output ordering):
        // wait pattern -> total timeout (kill) -> inactivity (detach).
        if (machine.matched()) {
            res.matched = true;
            res.killed = true;
            finished = true;
            break;
        }
        const int64_t elapsed = pr_now_ms() - start_ms;
        if (opts.timeout_ms > 0 && elapsed >= opts.timeout_ms) {
            res.killed = true;
            finished = true;
            break;
        }
        if (opts.inactivity_timeout_ms > 0 &&
            machine.last_output_elapsed_ms() > 0 &&
            elapsed - machine.last_output_elapsed_ms() >=
                opts.inactivity_timeout_ms) {
            res.still_running = true;
            finished = true;
            break;
        }
    }

    res.output = machine.output();
    res.matched = res.matched || machine.matched();
    res.truncated = machine.truncated();
    res.elapsed_ms = pr_now_ms() - start_ms;

    if (res.still_running) {
        res.exit_code = std::nullopt;
        p = reproc_destroy(p);
        return res;
    }

    reproc_stop_actions stop{};
    stop.first.action = REPROC_STOP_WAIT;
    stop.first.timeout = res.killed ? 0 : 100;
    stop.second.action = REPROC_STOP_TERMINATE;
    stop.second.timeout = 2000;
    stop.third.action = REPROC_STOP_KILL;
    stop.third.timeout = 0;
    const int swr = reproc_stop(p, stop);
    if (swr >= 0 && !exit_code.has_value()) {
        exit_code = swr;
    }
    // The process is gone: one last drain so late output is not lost.
    drain_all();
    res.output = machine.output();
    res.exit_code = exit_code;
    p = reproc_destroy(p);
    return res;
}

// ---------------------------------------------------------------------------
// Interactive task registry
// ---------------------------------------------------------------------------

namespace {

struct task_entry {
    kimix::string id;
    reproc_t *proc = nullptr;
    int64_t pid = 0;
    int64_t start_ms = 0; // pr_now_ms() at registration (job_output "elapsed")
    std::thread drain_thread;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> exited{false};
    std::atomic<int64_t> exit_code{-1};
    kimix::spin_mutex buf_mutex;
    kimix::string pending; // output not yet consumed by read_task
    kimix::string full;    // bounded full transcript (for peek/pattern)
    int64_t last_output_ms = 0;
    // Windows file-redirect tails (empty path == pipe mode).
    kimix::string out_path;
    kimix::string err_path;
#ifdef KIMIX_PLATFORM_WINDOWS
    void *stdin_write = nullptr; // parent side of the real Win32 stdin pipe
#endif
};

kimix::spin_mutex &pr_registry_mutex() {
    static kimix::spin_mutex m;
    return m;
}

kimix::vector<task_entry *> &pr_registry() {
    static kimix::vector<task_entry *> reg;
    return reg;
}

std::atomic<int64_t> &pr_next_task_id() {
    static std::atomic<int64_t> next{1};
    return next;
}

task_entry *pr_find_locked(kimix::string_view id) {
    for (task_entry *e : pr_registry()) {
        if (e->id == id) {
            return e;
        }
    }
    return nullptr;
}

// Per-base-id collision counters for requested task ids. Mirrors
// background/utils.py generate_task_id: the first task of a base id keeps the
// bare id, later ones get "_<n>" appended (n starting at 1).
kimix::unordered_map<kimix::string, int64_t, kimix::string_hash> &
pr_task_names() {
    static kimix::unordered_map<kimix::string, int64_t, kimix::string_hash> m;
    return m;
}

// Allocate the effective task id. Called with pr_registry_mutex() held.
kimix::string pr_alloc_task_id(kimix::string_view requested) {
    if (requested.empty()) {
        const int64_t n = pr_next_task_id().fetch_add(1);
        return kimix::format("task_{}", n);
    }
    const kimix::string base(requested);
    auto &names = pr_task_names();
    auto it = names.find(base);
    if (it == names.end()) {
        names.emplace(base, 0);
        return base;
    }
    it->second += 1;
    kimix::string candidate = kimix::format("{}_{}", base, it->second);
    // Extremely unlikely, but never hand out an id that is already live.
    while (pr_find_locked(candidate) != nullptr) {
        it->second += 1;
        candidate = kimix::format("{}_{}", base, it->second);
    }
    return candidate;
}

void pr_bounded_append(kimix::string &content, kimix::string_view text,
                       int64_t cap) {
    content.append(text.data(), text.size());
    if (cap > 0 && static_cast<int64_t>(content.size()) > cap) {
        const size_t head = static_cast<size_t>(cap * 4 / 10);
        const size_t tail = static_cast<size_t>(cap) - head;
        kimix::string kept;
        kept.reserve(head + tail + 64);
        kept.append(content.data(), head);
        kept += "\n[... output truncated ...]\n";
        kept.append(content.data() + content.size() - tail, tail);
        content = std::move(kept);
    }
}

void pr_drain_thread_main(task_entry *e, int64_t cap) {
    const bool file_mode = !e->out_path.empty() || !e->err_path.empty();
    file_tail out_tail(e->out_path);
    file_tail err_tail(e->err_path);
    uint8_t buf[k_read_buf_size];

    auto append_locked = [&](kimix::string_view chunk) {
        if (chunk.empty()) {
            return;
        }
      std::lock_guard<kimix::spin_mutex> g(e->buf_mutex);
          const kimix::string clean = sanitize_utf8(chunk);
          pr_bounded_append(e->pending, clean, cap);
          pr_bounded_append(e->full, clean, cap);
          e->last_output_ms = pr_now_ms();
      };

    auto drain_pass = [&]() {
        if (file_mode) {
            kimix::string chunk;
            if (out_tail.read_new(chunk)) {
                append_locked(chunk);
            }
            if (err_tail.read_new(chunk)) {
                append_locked(chunk);
            }
            return;
        }
        for (REPROC_STREAM stream : {REPROC_STREAM_OUT, REPROC_STREAM_ERR}) {
            while (true) {
                const int r = reproc_read(e->proc, stream, buf, sizeof(buf));
                if (r <= 0) {
                    break;
                }
                append_locked(kimix::string_view(
                    reinterpret_cast<const char *>(buf), static_cast<size_t>(r)));
                if (static_cast<size_t>(r) < sizeof(buf)) {
                    break;
                }
            }
        }
    };

    while (!e->stop_requested.load()) {
        int wr = -1;
        if (file_mode) {
#ifdef KIMIX_PLATFORM_WINDOWS
            wr = reproc_wait(e->proc, 100);
#else
            wr = reproc_wait(e->proc, 100);
#endif
        } else {
            reproc_event_source source{};
            source.process = e->proc;
            source.interests =
                REPROC_EVENT_OUT | REPROC_EVENT_ERR | REPROC_EVENT_EXIT;
            const int pr = reproc_poll(&source, 1, 100);
            if (pr >= 0 && (source.events & REPROC_EVENT_EXIT) != 0) {
                wr = reproc_wait(e->proc, 0);
            }
        }
        drain_pass();
        if (wr >= 0) {
            drain_pass(); // final tail
            e->exit_code.store(wr);
            break;
        }
    }
    e->exited.store(true);
}

void pr_destroy_entry(task_entry *e) {
    e->stop_requested.store(true);
    if (e->proc != nullptr) {
        reproc_stop_actions stop{};
        stop.first.action = REPROC_STOP_TERMINATE;
        stop.first.timeout = 1000;
        stop.second.action = REPROC_STOP_KILL;
        stop.second.timeout = 500;
        stop.third.action = REPROC_STOP_NOOP;
        reproc_stop(e->proc, stop);
    }
    if (e->drain_thread.joinable()) {
        e->drain_thread.join();
    }
    if (e->proc != nullptr) {
        e->proc = reproc_destroy(e->proc);
    }
#ifdef KIMIX_PLATFORM_WINDOWS
    if (e->stdin_write != nullptr) {
        CloseHandle(static_cast<HANDLE>(e->stdin_write));
        e->stdin_write = nullptr;
    }
#endif
    delete e;
}

} // namespace

tool_error start_task(const run_options &opts, task_handle &out) {
    if (opts.argv.empty()) {
        return {tool_status::invalid_input, "empty argv"};
    }
    const argv_holder argv = pr_make_argv(opts.argv);
    const argv_holder env = pr_make_env(opts.extra_env);

    auto *e = new task_entry();
    {
        std::lock_guard<kimix::spin_mutex> g(pr_registry_mutex());
        e->id = pr_alloc_task_id(opts.requested_task_id);
    }
    e->start_ms = pr_now_ms();
#ifdef KIMIX_PLATFORM_WINDOWS
    e->out_path = opts.stderr_path.empty() ? pr_temp_path("reproc_task_out")
                                           : opts.stderr_path;
    reproc_options o = pr_base_options(opts, env, e->out_path, kimix::string());
#else
    reproc_options o = pr_base_options(opts, env, {}, {});
#endif

#ifdef KIMIX_PLATFORM_WINDOWS
    // Real Win32 anonymous pipe for stdin: MSYS2/Git Bash cannot read from
    // reproc's socket-based pipes (it sees EOF immediately and interactive
    // shells exit). The read end is inheritable and handed to the child.
    HANDLE rd = INVALID_HANDLE_VALUE;
    HANDLE wr = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        delete e;
        return {tool_status::external_library, "CreatePipe failed"};
    }
    // The write end must NOT be inherited by the child.
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    o.redirect.in.type = REPROC_REDIRECT_HANDLE;
    o.redirect.in.handle = rd;
    e->stdin_write = wr;
#endif

    e->proc = reproc_new();
    if (e->proc == nullptr) {
#ifdef KIMIX_PLATFORM_WINDOWS
        CloseHandle(rd);
        CloseHandle(wr);
#endif
        delete e;
        return {tool_status::external_library, "reproc_new failed"};
    }
    const int r = reproc_start(e->proc, argv.data(), o);
    if (r < 0) {
        kimix::string msg = kimix::string("spawn failed: ") + reproc_strerror(r);
        e->proc = reproc_destroy(e->proc);
#ifdef KIMIX_PLATFORM_WINDOWS
        CloseHandle(rd);
        CloseHandle(wr);
#endif
        delete e;
        return {tool_status::invalid_input, msg};
    }
#ifdef KIMIX_PLATFORM_WINDOWS
    CloseHandle(rd); // the child owns the only remaining read end now
#endif
    e->pid = reproc_pid(e->proc);
    if (!opts.stdin_input.empty()) {
#ifdef KIMIX_PLATFORM_WINDOWS
        DWORD written = 0;
        WriteFile(wr, opts.stdin_input.data(),
                  static_cast<DWORD>(opts.stdin_input.size()), &written, nullptr);
#else
        reproc_write(e->proc,
                     reinterpret_cast<const uint8_t *>(opts.stdin_input.data()),
                     opts.stdin_input.size());
#endif
    }
    const int64_t cap = opts.output_cap_chars > 0 ? opts.output_cap_chars : 200000;
    e->drain_thread = std::thread([e, cap] { pr_drain_thread_main(e, cap); });

    std::lock_guard<kimix::spin_mutex> g(pr_registry_mutex());
    pr_registry().push_back(e);
    out.task_id = e->id;
    out.pid = e->pid;
    return {tool_status::ok, {}};
}

tool_error send_task(kimix::string_view task_id, kimix::string_view text,
                     bool add_newline) {
    std::lock_guard<kimix::spin_mutex> g(pr_registry_mutex());
    task_entry *e = pr_find_locked(task_id);
    if (e == nullptr) {
        kimix::string msg = "no such task: ";
        msg.append(task_id.data(), task_id.size());
        return {tool_status::not_found, msg};
    }
    if (e->exited.load()) {
        return {tool_status::invalid_input, "task already exited"};
    }
    kimix::string payload(text);
    if (add_newline && (payload.empty() || payload.back() != '\n')) {
        payload += '\n';
    }
#ifdef KIMIX_PLATFORM_WINDOWS
    if (e->stdin_write == nullptr) {
        return {tool_status::invalid_input, "task stdin pipe is closed"};
    }
    DWORD written = 0;
    if (!WriteFile(static_cast<HANDLE>(e->stdin_write), payload.data(),
                   static_cast<DWORD>(payload.size()), &written, nullptr)) {
        return {tool_status::invalid_input, "WriteFile on task stdin failed"};
    }
    return {tool_status::ok, {}};
#else
    const int r = reproc_write(e->proc,
                               reinterpret_cast<const uint8_t *>(payload.data()),
                               payload.size());
    if (r < 0) {
        return {tool_status::invalid_input,
                kimix::string("write failed: ") + reproc_strerror(r)};
    }
    return {tool_status::ok, {}};
#endif
}

task_wait_result wait_task(kimix::string_view task_id,
                           kimix::string_view pattern, int64_t timeout_ms) {
    task_wait_result wr;
    const int64_t start = pr_now_ms();
    while (true) {
        task_entry *e = nullptr;
        {
            std::lock_guard<kimix::spin_mutex> g(pr_registry_mutex());
            e = pr_find_locked(task_id);
        }
        if (e == nullptr) {
            return wr;
        }
        if (!pattern.empty()) {
            kimix::string snapshot;
            {
                std::lock_guard<kimix::spin_mutex> g(e->buf_mutex);
                snapshot = e->full;
            }
            if (snapshot.find(kimix::string(pattern)) != kimix::string::npos) {
                wr.matched = true;
            }
        }
        if (e->exited.load()) {
            wr.exited = true;
        }
        if (wr.matched || wr.exited) {
            wr.elapsed_ms = pr_now_ms() - start;
            return wr;
        }
        if (timeout_ms > 0 && pr_now_ms() - start >= timeout_ms) {
            wr.elapsed_ms = pr_now_ms() - start;
            return wr;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

tool_error read_task(kimix::string_view task_id, kimix::string &out) {
    std::lock_guard<kimix::spin_mutex> g(pr_registry_mutex());
    task_entry *e = pr_find_locked(task_id);
    if (e == nullptr) {
        kimix::string msg = "no such task: ";
        msg.append(task_id.data(), task_id.size());
        return {tool_status::not_found, msg};
    }
    std::lock_guard<kimix::spin_mutex> bg(e->buf_mutex);
    out = std::move(e->pending);
    e->pending.clear();
    return {tool_status::ok, {}};
}

tool_error peek_task(kimix::string_view task_id, kimix::string &out) {
    std::lock_guard<kimix::spin_mutex> g(pr_registry_mutex());
    task_entry *e = pr_find_locked(task_id);
    if (e == nullptr) {
        kimix::string msg = "no such task: ";
        msg.append(task_id.data(), task_id.size());
        return {tool_status::not_found, msg};
    }
    std::lock_guard<kimix::spin_mutex> bg(e->buf_mutex);
    out = e->full;
    return {tool_status::ok, {}};
}

task_status_info query_task(kimix::string_view task_id) {
    task_status_info info;
    std::lock_guard<kimix::spin_mutex> g(pr_registry_mutex());
    task_entry *e = pr_find_locked(task_id);
    if (e == nullptr) {
        return info;
    }
    info.exists = true;
    info.exited = e->exited.load();
    info.pid = e->pid;
    info.elapsed_ms = (e->start_ms > 0) ? (pr_now_ms() - e->start_ms) : 0;
    if (info.exited) {
        const int64_t code = e->exit_code.load();
        info.exit_code =
            (code >= 0) ? kimix::optional<int64_t>(code) : std::nullopt;
    }
    return info;
}

tool_error stop_task(kimix::string_view task_id) {
    task_entry *e = nullptr;
    {
        std::lock_guard<kimix::spin_mutex> g(pr_registry_mutex());
        auto &reg = pr_registry();
        for (size_t i = 0; i < reg.size(); ++i) {
            if (reg[i]->id == task_id) {
                e = reg[i];
                reg.erase(reg.begin() + static_cast<ptrdiff_t>(i));
                break;
            }
        }
    }
    if (e == nullptr) {
        kimix::string msg = "no such task: ";
        msg.append(task_id.data(), task_id.size());
        return {tool_status::not_found, msg};
    }
    pr_destroy_entry(e);
    return {tool_status::ok, {}};
}

tool_error stop_task(kimix::string_view task_id, kimix::string &final_output) {
    task_entry *e = nullptr;
    {
        std::lock_guard<kimix::spin_mutex> g(pr_registry_mutex());
        auto &reg = pr_registry();
        for (size_t i = 0; i < reg.size(); ++i) {
            if (reg[i]->id == task_id) {
                e = reg[i];
                reg.erase(reg.begin() + static_cast<ptrdiff_t>(i));
                break;
            }
        }
    }
    if (e == nullptr) {
        kimix::string msg = "no such task: ";
        msg.append(task_id.data(), task_id.size());
        return {tool_status::not_found, msg};
    }
    // Terminate first, then join the drain thread so the buffer is complete.
    e->stop_requested.store(true);
    if (e->proc != nullptr) {
        reproc_stop_actions stop{};
        stop.first.action = REPROC_STOP_TERMINATE;
        stop.first.timeout = 1000;
        stop.second.action = REPROC_STOP_KILL;
        stop.second.timeout = 500;
        stop.third.action = REPROC_STOP_NOOP;
        reproc_stop(e->proc, stop);
    }
    if (e->drain_thread.joinable()) {
        e->drain_thread.join();
    }
    {
        std::lock_guard<kimix::spin_mutex> bg(e->buf_mutex);
        final_output = std::move(e->pending);
        e->pending.clear();
    }
    pr_destroy_entry(e); // teardown only: the thread is no longer joinable
    return {tool_status::ok, {}};
}

void stop_all_tasks() {
    kimix::vector<task_entry *> entries;
    {
        std::lock_guard<kimix::spin_mutex> g(pr_registry_mutex());
        entries = pr_registry();
        pr_registry().clear();
    }
    for (task_entry *e : entries) {
        pr_destroy_entry(e);
    }
}

kimix::vector<task_summary> list_tasks() {
    kimix::vector<task_summary> out;
    const int64_t now = pr_now_ms();
    std::lock_guard<kimix::spin_mutex> g(pr_registry_mutex());
    out.reserve(pr_registry().size());
    for (const task_entry *e : pr_registry()) {
        task_summary s;
        s.task_id = e->id;
        s.pid = e->pid;
        s.exited = e->exited.load();
        if (s.exited) {
            const int64_t code = e->exit_code.load();
            s.exit_code =
                (code >= 0) ? kimix::optional<int64_t>(code) : std::nullopt;
        }
        s.elapsed_ms = (e->start_ms > 0) ? (now - e->start_ms) : 0;
        out.push_back(std::move(s));
    }
    return out;
}

tool_error remove_task(kimix::string_view task_id) {
    // Same semantics as stop_task: the entry leaves the registry, its process
    // tree is terminated and the drain thread joined. Kept as a separate entry
    // point because the Python tools distinguish "kill" (stop_task) from
    // "the foreground run finished, forget the id" (remove_task_id).
    return stop_task(task_id);
}

} // namespace kimix::builtin_tools::proc
