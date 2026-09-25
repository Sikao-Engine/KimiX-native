// test_process_runner.cpp - real subprocess lifecycle tests for the reproc
// backed runner (builtin_tools/process_runner.cpp), the single spawn path used
// by the bash / pwsh / python / run tools.
//
// Scope (reproc skill: every reproc_start must be paired with
// reproc_wait/reproc_stop AND reproc_destroy, and one reproc_t must not be
// driven from two threads at the same time):
// * run_process   - exit code, merged stdout+stderr, stdin write, timeout kill,
//                   early stop on inactivity, no leftover registry entries.
// * task registry - start_task / send_task / wait_task / read_task / stop_task,
//                   stopping while the drain thread is still reading, stopping
//                   a task another thread is waiting on, stop_all_tasks
//                   idempotence.
// Children are Git Bash (or python) when installed; the tests skip otherwise,
// exactly like the existing runner tests in tests/unit/agent/test_agent.cpp.
#include <atomic>
#include <cstdio>
#include <thread>

#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "builtin_tools/bash_tool.h"
#include "builtin_tools/process_runner.h"
#include "builtin_tools/pwsh_tool.h"
#include "builtin_tools/python_tool.h"
#include "builtin_tools/tool.h"
#include "builtin_tools/tool_registry.h"

namespace {

using namespace kimix::builtin_tools;
namespace proc = kimix::builtin_tools::proc;

// The shell the tests spawn. Empty when nothing usable is installed, in which
// case every test below returns early (the runner is still exercised by the
// pure-logic suites on such a host).
kimix::string probe_bash() { return bash::Bash::detect_bash_path(); }

proc::run_options bash_opts(const kimix::string &bash,
                            kimix::string_view script) {
  proc::run_options opts;
  opts.argv = {bash, "--noprofile", "--norc", "-c", kimix::string(script)};
  opts.timeout_ms = 20000;
  return opts;
}

// The registry is process-global, so a test that leaves a task behind would
// change another test's counts. Every task test takes one of these.
struct registry_guard {
  registry_guard() = default;
  ~registry_guard() { proc::stop_all_tasks(); }
  registry_guard(const registry_guard &) = delete;
  registry_guard &operator=(const registry_guard &) = delete;
};

} // namespace

int main(int argc, char *argv[]) {
  boost::ut::detail::cfg::parse_arg_with_fallback(
      argc, const_cast<const char **>(argv));
  using namespace boost::ut;

  // =========================================================================
  // run_process: foreground, bounded
  // =========================================================================
  "run_process reports exit code and merged stdout+stderr"_test = [] {
    const kimix::string bash = probe_bash();
    if (bash.empty()) {
      return;
    }
    const proc::run_result rr =
        proc::run_process(bash_opts(bash, "echo out_token; echo err_token >&2; "
                                          "exit 7"));
    expect(rr.spawn_error.empty()) << rr.spawn_error;
    expect(rr.exit_code.has_value());
    if (rr.exit_code.has_value()) {
      expect(eq(*rr.exit_code, int64_t(7)));
    }
    expect(rr.output.find("out_token") != kimix::string::npos) << rr.output;
    expect(rr.output.find("err_token") != kimix::string::npos)
        << "stderr must be merged into the captured output";
    expect(!rr.killed);
    expect(!rr.truncated);
  };

  "run_process writes the whole stdin payload before closing it"_test = [] {
    const kimix::string bash = probe_bash();
    if (bash.empty()) {
      return;
    }
    // 200 KiB is far past the OS pipe buffer, so a single reproc_write()
    // cannot deliver it: the runner must keep writing until EOF.
    kimix::string payload;
    payload.reserve(200 * 1024);
    for (int i = 0; i < 20000; ++i) {
      payload += "0123456789";
    }
    proc::run_options opts = bash_opts(bash, "cat");
    opts.stdin_input = payload;
    const proc::run_result rr = proc::run_process(opts);
    expect(rr.spawn_error.empty()) << rr.spawn_error;
    expect(rr.exit_code.has_value());
    expect(eq(rr.output.size(), payload.size()))
        << "stdin must reach the child in full; got " << rr.output.size()
        << " bytes, head=[" << rr.output.substr(0, 80) << "]";
  };

  "run_process kills the child on timeout and leaves no task behind"_test = [] {
    const kimix::string bash = probe_bash();
    if (bash.empty()) {
      return;
    }
    const registry_guard guard{};
    const size_t base = proc::list_tasks().size();
    proc::run_options opts = bash_opts(bash, "sleep 60");
    opts.timeout_ms = 500;
    const proc::run_result rr = proc::run_process(opts);
    expect(rr.killed);
    expect(!rr.exit_code.has_value()) << "a killed child reports no exit code";
    expect(!rr.still_running);
    expect(rr.task_id.empty())
        << "a killed child is not handed to the registry";
    expect(eq(proc::list_tasks().size(), base))
        << "a killed foreground run must not register a task";
  };

  "run_process stops on the wait pattern without calling it a timeout"_test =
      [] {
        const kimix::string bash = probe_bash();
        if (bash.empty()) {
          return;
        }
        const registry_guard guard{};
        proc::run_options opts = bash_opts(bash, "echo ready_token; sleep 60");
        opts.timeout_ms = 20000; // far away: only the pattern may stop this run
        opts.wait_pattern = "ready_token";
        const proc::run_result rr = proc::run_process(opts);
        expect(rr.spawn_error.empty()) << rr.spawn_error;
        expect(rr.matched) << "the pattern must stop the poll loop";
        expect(!rr.killed) << "a matched wait is not a timeout";
        expect(!rr.still_running);
        expect(!rr.exit_code.has_value())
            << "a run we stopped reports no exit status";
        expect(rr.output.find("ready_token") != kimix::string::npos)
            << rr.output;
        expect(eq(proc::list_tasks().size(), static_cast<size_t>(0)));
      };

  "run_process hands a quiet child to the task registry (no orphan)"_test = [] {
    const kimix::string bash = probe_bash();
    if (bash.empty()) {
      return;
    }
    const registry_guard guard{};
    const size_t base = proc::list_tasks().size();
    // Talks, goes quiet for longer than the inactivity bound, then talks
    // again while the process keeps running.
    proc::run_options opts =
        bash_opts(bash, "echo quiet_token; sleep 2; echo late_token; sleep 60");
    opts.timeout_ms = 0; // disabled: only the inactivity bound may fire
    opts.inactivity_timeout_ms = 700;
    opts.requested_task_id = "adopted_task";
    const proc::run_result rr = proc::run_process(opts);
    expect(rr.still_running) << "the inactivity bound must stop the run early";
    expect(!rr.killed) << "a quiet child must not be killed";
    expect(!rr.exit_code.has_value());
    expect(rr.output.find("quiet_token") != kimix::string::npos) << rr.output;
    // The child must still be alive and addressable: that is what makes the
    // caller's "running in background" report true.
    expect(!rr.task_id.empty()) << "the adopted task needs an id";
    expect(rr.task_id == "adopted_task") << rr.task_id;
    const proc::task_status_info info = proc::query_task(rr.task_id);
    expect(info.exists);
    expect(!info.exited) << "the adopted child must still be running";
    expect(eq(proc::list_tasks().size(), base + 1));
    // The drain thread of the adopted task keeps collecting: the late line
    // must arrive, and the earlier one must not be reported twice.
    const proc::task_wait_result tw =
        proc::wait_task(rr.task_id, "late_token", 20000);
    expect(tw.matched) << "output after the hand-off must be captured";
    kimix::string transcript;
    expect(!proc::peek_task(rr.task_id, transcript).failed());
    expect(transcript.find("late_token") != kimix::string::npos) << transcript;
    expect(eq(transcript.find("quiet_token"), transcript.rfind("quiet_token")))
        << "the tail offset must resume, not restart";
    expect(!proc::stop_task(rr.task_id).failed());
    expect(eq(proc::list_tasks().size(), base));
  };

  "run_process reports a spawn failure without leaking"_test = [] {
    const registry_guard guard{};
    const size_t base = proc::list_tasks().size();
    proc::run_options opts;
    opts.argv = {kimix::string("Z:/definitely/not/here.exe")};
    opts.timeout_ms = 2000;
    const proc::run_result rr = proc::run_process(opts);
    expect(!rr.spawn_error.empty());
    expect(eq(proc::list_tasks().size(), base));
  };

  "run_process rejects empty argv as invalid input"_test = [] {
    proc::run_options opts;
    const proc::run_result rr = proc::run_process(opts);
    expect(rr.status == tool_status::invalid_input);
    expect(!rr.spawn_error.empty());
  };

  // =========================================================================
  // Task registry: interactive children
  // =========================================================================
  "task start / send / wait / read / stop round trip"_test = [] {
    const kimix::string bash = probe_bash();
    if (bash.empty()) {
      return;
    }
    const registry_guard guard{};
    proc::run_options opts;
    opts.argv = {bash, "--noprofile", "--norc", "-i"};
    opts.requested_task_id = "task_rt";
    proc::task_handle h;
    const tool_error terr = proc::start_task(opts, h);
    expect(!terr.failed()) << terr.message;
    expect(h.task_id == "task_rt") << h.task_id;
    expect(h.pid > 0);

    const std::size_t live_before = proc::list_tasks().size();
    expect(live_before >= 1);

    expect(!proc::send_task(h.task_id, "echo round_trip_ok", true).failed());
    const proc::task_wait_result tw =
        proc::wait_task(h.task_id, "round_trip_ok", 15000);
    expect(tw.matched);
    kimix::string out;
    expect(!proc::read_task(h.task_id, out).failed());
    expect(out.find("round_trip_ok") != kimix::string::npos) << out;
    kimix::string peeked;
    expect(!proc::peek_task(h.task_id, peeked).failed());

    expect(!proc::stop_task(h.task_id).failed());
    expect(!proc::query_task(h.task_id).exists);
    expect(eq(proc::list_tasks().size(), live_before - 1));
  };

  "task ids collide with the generate_task_id suffix scheme"_test = [] {
    const kimix::string bash = probe_bash();
    if (bash.empty()) {
      return;
    }
    const registry_guard guard{};
    const size_t base = proc::list_tasks().size();
    proc::run_options opts;
    opts.argv = {bash, "--noprofile", "--norc", "-i"};
    opts.requested_task_id = "run_bash";
    proc::task_handle a, b;
    expect(!proc::start_task(opts, a).failed());
    expect(!proc::start_task(opts, b).failed());
    expect(a.task_id == "run_bash") << a.task_id;
    expect(b.task_id == "run_bash_1") << b.task_id;
    proc::stop_task(a.task_id);
    proc::stop_task(b.task_id);
    expect(eq(proc::list_tasks().size(), base));
  };

  "stop_task is safe while the drain thread is still reading"_test = [] {
    const kimix::string bash = probe_bash();
    if (bash.empty()) {
      return;
    }
    const registry_guard guard{};
    const size_t base = proc::list_tasks().size();
    // A chatty child keeps the drain thread inside reproc_read()/reproc_wait()
    // when the stop arrives: reproc_t must never be driven from two threads.
    for (int i = 0; i < 5; ++i) {
      proc::run_options opts;
      opts.argv = {bash, "--noprofile", "--norc", "-c",
                   "while true; do echo spam; sleep 0.01; done"};
      proc::task_handle h;
      const tool_error terr = proc::start_task(opts, h);
      expect(!terr.failed()) << terr.message;
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      const tool_error serr = proc::stop_task(h.task_id);
      expect(!serr.failed()) << serr.message;
    }
    expect(eq(proc::list_tasks().size(), base));
  };

  "stop_task hands back the final buffered output"_test = [] {
    const kimix::string bash = probe_bash();
    if (bash.empty()) {
      return;
    }
    const registry_guard guard{};
    proc::run_options opts;
    opts.argv = {bash, "--noprofile", "--norc", "-i"};
    proc::task_handle h;
    expect(!proc::start_task(opts, h).failed());
    expect(!proc::send_task(h.task_id, "echo final_tail_token", true).failed());
    proc::wait_task(h.task_id, "final_tail_token", 15000);
    kimix::string final_output;
    expect(!proc::stop_task(h.task_id, final_output).failed());
    expect(final_output.find("final_tail_token") != kimix::string::npos)
        << final_output;
  };

  "a task stopped by one thread must not break a waiter on another"_test = [] {
    const kimix::string bash = probe_bash();
    if (bash.empty()) {
      return;
    }
    const registry_guard guard{};
    const size_t base = proc::list_tasks().size();
    proc::run_options opts;
    opts.argv = {bash, "--noprofile", "--norc", "-i"};
    proc::task_handle h;
    expect(!proc::start_task(opts, h).failed());
    std::atomic<bool> done{false};
    bool waiter_saw_nothing = false;
    std::thread waiter([&] {
      const proc::task_wait_result tw =
          proc::wait_task(h.task_id, "never_printed", 4000);
      waiter_saw_nothing = !tw.matched;
      done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    expect(!proc::stop_task(h.task_id).failed());
    // Give the waiter a bounded amount of time to finish its in-flight poll
    // on the entry the stop just removed.
    for (int i = 0; i < 100 && !done.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    waiter.join();
    expect(done.load()) << "wait_task must return once its task is gone";
    expect(waiter_saw_nothing);
    expect(eq(proc::list_tasks().size(), base));
  };

  "wait_task and query_task report unknown ids as absent"_test = [] {
    const proc::task_wait_result tw =
        proc::wait_task("task_does_not_exist", "", 10);
    expect(!tw.matched);
    expect(!tw.exited);
    expect(!proc::query_task("task_does_not_exist").exists);
    kimix::string out;
    expect(proc::read_task("task_does_not_exist", out).failed());
    expect(proc::send_task("task_does_not_exist", "x", true).failed());
    expect(proc::stop_task("task_does_not_exist").failed());
    expect(proc::remove_task("task_does_not_exist").failed());
  };

  "an exited task reports its exit code and refuses further input"_test = [] {
    const kimix::string bash = probe_bash();
    if (bash.empty()) {
      return;
    }
    const registry_guard guard{};
    proc::run_options opts;
    opts.argv = {bash, "--noprofile", "--norc", "-c", "echo bye; exit 5"};
    proc::task_handle h;
    expect(!proc::start_task(opts, h).failed());
    const proc::task_wait_result tw = proc::wait_task(h.task_id, "bye", 15000);
    expect(tw.exited || tw.matched);
    for (int i = 0; i < 100; ++i) {
      if (proc::query_task(h.task_id).exited) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const proc::task_status_info info = proc::query_task(h.task_id);
    expect(info.exists);
    expect(info.exited);
    if (info.exit_code.has_value()) {
      expect(eq(*info.exit_code, int64_t(5))) << "exit code must be recorded";
    }
    const tool_error serr = proc::send_task(h.task_id, "echo too_late", true);
    expect(serr.failed()) << "sending to an exited task must not succeed";
    proc::stop_task(h.task_id);
  };

  "stop_all_tasks is idempotent and clears the registry"_test = [] {
    const kimix::string bash = probe_bash();
    if (bash.empty()) {
      return;
    }
    for (int i = 0; i < 3; ++i) {
      proc::run_options opts;
      opts.argv = {bash, "--noprofile", "--norc", "-i"};
      proc::task_handle h;
      expect(!proc::start_task(opts, h).failed());
    }
    expect(proc::list_tasks().size() >= 3);
    proc::stop_all_tasks();
    expect(eq(proc::list_tasks().size(), static_cast<size_t>(0)));
    proc::stop_all_tasks(); // second call on an empty registry
    expect(eq(proc::list_tasks().size(), static_cast<size_t>(0)));
  };

  "remove_task forgets a finished task so job_output stops listing it"_test =
      [] {
        const kimix::string bash = probe_bash();
        if (bash.empty()) {
          return;
        }
        const registry_guard guard{};
        proc::run_options opts;
        opts.argv = {bash, "--noprofile", "--norc", "-c", "echo done"};
        proc::task_handle h;
        expect(!proc::start_task(opts, h).failed());
        proc::wait_task(h.task_id, "done", 15000);
        expect(proc::list_tasks().size() >= 1);
        expect(!proc::remove_task(h.task_id).failed());
        expect(!proc::query_task(h.task_id).exists);
      };

  // =========================================================================
  // The tools must reach the runner: bash/pwsh/python execute for real
  // =========================================================================
  "bash tool executes for real through the runner"_test = [] {
    const kimix::string bash = probe_bash();
    if (bash.empty()) {
      return;
    }
    const registry_guard guard{};
    std::error_code ec;
    const kimix::filesystem::path dir =
        kimix::filesystem::temp_directory_path(ec) /
        kimix::format("kimix_bash_run_{}",
                      static_cast<uint64_t>(kimix::Clock::now_ms()));
    kimix::filesystem::create_directories(dir, ec);
    if (ec) {
      return;
    }
    Session session;
    session.native_io = true;
    session.work_dir = kimix::to_string(dir);
    auto tool = ToolRegistry::instance().create("bash", &session);
    expect(tool != nullptr);
    ToolParams params;
    params.values["command"] =
        ValueElement::make_string(kimix::string("echo native_bash_token"));
    params.values["timeout"] = ValueElement::make_int(20);
    (*tool)(&params);
    kimix::vector<char> json;
    tool->result_json(json);
    const kimix::string text(json.data(), json.size());
    expect(text.find("native_bash_token") != kimix::string::npos) << text;
    expect(text.find("completed") != kimix::string::npos) << text;
    expect(eq(proc::list_tasks().size(), static_cast<size_t>(0)))
        << "a foreground bash run must not leave a task registered";
    kimix::filesystem::remove_all(dir, ec);
  };

  "python tool executes for real through the runner"_test = [] {
    const registry_guard guard{};
    const kimix::string python_exe =
        python::Python::detect_python_exe(kimix::string_view());
    if (python_exe.empty()) {
      return;
    }
    std::error_code ec;
    const kimix::filesystem::path dir =
        kimix::filesystem::temp_directory_path(ec) /
        kimix::format("kimix_py_run_{}",
                      static_cast<uint64_t>(kimix::Clock::now_ms()));
    kimix::filesystem::create_directories(dir, ec);
    if (ec) {
      return;
    }
    Session session;
    session.native_io = true;
    session.work_dir = kimix::to_string(dir);
    python::Python tool(&session);
    ToolParams params;
    params.values["code"] =
        ValueElement::make_string(kimix::string("print('native_py_token')"));
    params.values["timeout"] = ValueElement::make_int(30);
    tool(&params);
    kimix::vector<char> json;
    tool.result_json(json);
    const kimix::string text(json.data(), json.size());
    expect(text.find("native_py_token") != kimix::string::npos) << text;
    expect(eq(proc::list_tasks().size(), static_cast<size_t>(0)))
        << "a foreground python run must not leave a task registered";
    kimix::filesystem::remove_all(dir, ec);
  };

  "pwsh tool executes for real through the runner"_test = [] {
    const registry_guard guard{};
    const kimix::string host = pwsh::detect_pwsh_path();
    if (host.empty()) {
      return; // no PowerShell on this host
    }
    std::error_code ec;
    const kimix::filesystem::path dir =
        kimix::filesystem::temp_directory_path(ec) /
        kimix::format("kimix_pwsh_run_{}",
                      static_cast<uint64_t>(kimix::Clock::now_ms()));
    kimix::filesystem::create_directories(dir, ec);
    if (ec) {
      return;
    }
    Session session;
    session.native_io = true;
    session.work_dir = kimix::to_string(dir);
    auto tool = ToolRegistry::instance().create("pwsh", &session);
    expect(tool != nullptr);

    ToolParams params;
    params.values["command"] = ValueElement::make_string(
        kimix::string("Write-Output native_pwsh_token"));
    params.values["timeout"] = ValueElement::make_int(60);
    (*tool)(&params);
    kimix::vector<char> json;
    tool->result_json(json);
    const kimix::string text(json.data(), json.size());
    expect(text.find("native_pwsh_token") != kimix::string::npos) << text;
    expect(text.find("completed") != kimix::string::npos) << text;
    expect(text.find("unknown mode") == kimix::string::npos)
        << "an execution request must not be answered by a kernel mode";
    expect(eq(proc::list_tasks().size(), static_cast<size_t>(0)))
        << "a foreground pwsh run must not leave a task registered";

    // A failing command reports the exit code the wrapper preserved.
    ToolParams failing;
    failing.values["command"] =
        ValueElement::make_string(kimix::string("exit 4"));
    failing.values["timeout"] = ValueElement::make_int(60);
    (*tool)(&failing);
    tool->result_json(json);
    const kimix::string ftext(json.data(), json.size());
    expect(ftext.find("failed") != kimix::string::npos) << ftext;

    // A destructive command is stopped by the hardline floor before any
    // process is spawned (the rule table is the shared one: textual
    // patterns, checked on the raw AND the prepared command line).
    ToolParams blocked;
    blocked.values["command"] =
        ValueElement::make_string(kimix::string("rmdir /s /q C:\\\\"));
    (*tool)(&blocked);
    tool->result_json(json);
    const kimix::string btext(json.data(), json.size());
    expect(btext.find("blocked") != kimix::string::npos) << btext;
    expect(eq(proc::list_tasks().size(), static_cast<size_t>(0)))
        << "a blocked command must never be spawned";

    // mode=interactive starts a REPL task that accepts a continuation.
    ToolParams interactive;
    interactive.values["command"] =
        ValueElement::make_string(kimix::string("Get-Date"));
    interactive.values["mode"] =
        ValueElement::make_string(kimix::string("interactive"));
    (*tool)(&interactive);
    tool->result_json(json);
    const kimix::string itext(json.data(), json.size());
    expect(itext.find("running") != kimix::string::npos) << itext;
    const kimix::vector<proc::task_summary> tasks = proc::list_tasks();
    expect(eq(tasks.size(), static_cast<size_t>(1))) << tasks.size();
    if (!tasks.empty()) {
      const kimix::string task_id = tasks[0].task_id;
      ToolParams cont;
      cont.values["command"] = ValueElement::make_string(
          kimix::string("Write-Output repl_round_trip"));
      cont.values["task_id"] = ValueElement::make_string(task_id);
      cont.values["timeout"] = ValueElement::make_int(60);
      (*tool)(&cont);
      tool->result_json(json);
      const kimix::string ctext(json.data(), json.size());
      expect(ctext.find("repl_round_trip") != kimix::string::npos) << ctext;
      expect(!proc::stop_task(task_id).failed());
    }
    proc::stop_all_tasks();
    kimix::filesystem::remove_all(dir, ec);
  };

  "pwsh tool needs no native session and says so"_test = [] {
    Session session; // native_io == false
    auto tool = ToolRegistry::instance().create("pwsh", &session);
    expect(tool != nullptr);
    ToolParams params;
    params.values["command"] =
        ValueElement::make_string(kimix::string("Write-Output x"));
    params.values["mode"] = ValueElement::make_string(kimix::string("execute"));
    (*tool)(&params);
    kimix::vector<char> json;
    tool->result_json(json);
    const kimix::string text(json.data(), json.size());
    expect(text.find("unsupported") != kimix::string::npos) << text;
    expect(eq(proc::list_tasks().size(), static_cast<size_t>(0)));
  };

  // =========================================================================
  // One-shot argv construction (shell_common.py pwsh_argv)
  // =========================================================================
  "wrap_pwsh_command matches the reference wrapper"_test = [] {
    const kimix::string wrapped = pwsh::wrap_pwsh_command("Get-Date");
    expect(wrapped.find(pwsh::k_pwsh_console_init) == 0);
    expect(
        wrapped.find("try{Get-Date}catch{$_|Out-String|Write-Error;exit 1}") !=
        kimix::string::npos)
        << wrapped;
    expect(wrapped.size() >= 17 &&
           wrapped.compare(wrapped.size() - 19, 19, ";exit $LASTEXITCODE") == 0)
        << wrapped;
  };

  "pwsh_maybe_encode picks -C below and -Enc above 8000 characters"_test = [] {
    const pwsh::pwsh_payload short_form = pwsh::pwsh_maybe_encode("echo hi");
    expect(short_form.param == "-C") << short_form.param;
    expect(!short_form.encoded);
    expect(short_form.value == "echo hi");
    // 8001 ASCII characters: one past the reference's limit (which counts
    // CHARACTERS, not bytes - checked by the non-ASCII case below).
    const kimix::string big(8001, 'a');
    const pwsh::pwsh_payload encoded = pwsh::pwsh_maybe_encode(big);
    expect(encoded.param == "-Enc") << encoded.param;
    expect(encoded.encoded);
    // The -Enc payload is base64 of the UTF-16LE encoding of the command.
    expect(encoded.value ==
           pwsh::pwsh_base64_string(pwsh::pwsh_utf16le_string(big)));
  };

  "the pwsh -Enc limit counts characters not bytes"_test = [] {
    // 5000 code points of a 2-byte sequence are 10000 bytes but stay BELOW
    // the 8000-CHARACTER limit (Python compares len(str)).
    kimix::string wide;
    for (int i = 0; i < 5000; ++i) {
      wide += "\xC3\xA9"; // U+00E9
    }
    expect(!pwsh::pwsh_maybe_encode(wide).encoded)
        << "10000 bytes of non-ASCII is only 5000 characters";
    // 8001 of the same characters (16002 bytes) DO cross it.
    for (int i = 0; i < 3001; ++i) {
      wide += "\xC3\xA9";
    }
    expect(pwsh::pwsh_maybe_encode(wide).encoded)
        << "a long non-ASCII command must be -EncodedCommand";
  };

  "pwsh base64 + UTF-16LE match Python base64/utf-16-le"_test = [] {
    // Vectors produced with:
    //   base64.b64encode(s.encode("utf-16-le")).decode()
    // (`pwsh_base64_string` encodes BYTES; -Enc is the two-step chain.)
    const kimix::string v1 =
        pwsh::pwsh_base64_string(pwsh::pwsh_utf16le_string("echo hi"));
    expect(v1 == kimix::string("ZQBjAGgAbwAgAGgAaQA=")) << v1;
    const kimix::string v2 = pwsh::pwsh_base64_string(
        pwsh::pwsh_utf16le_string("Write-Output \"caf\xC3\xA9\""));
    expect(v2 == kimix::string("VwByAGkAdABlAC0ATwB1AHQAcAB1AHQAIAAiAGMAYQBm"
                               "AOkAIgA="))
        << v2;
    // Astral code point -> surrogate pair (UTF-8 F0 9F 98 80 = U+1F600).
    const kimix::string v3 =
        pwsh::pwsh_base64_string(pwsh::pwsh_utf16le_string("a\xF0\x9F\x98\x80"
                                                           "b"));
    expect(v3 == kimix::string("YQA92ADeYgA=")) << v3;
    // A byte-level vector, to pin the encoder itself (base64 of b"abc").
    expect(pwsh::pwsh_base64_string("abc") == kimix::string("YWJj"));
    const kimix::string u16 = pwsh::pwsh_utf16le_string("a\xF0\x9F\x98\x80"
                                                        "b");
    expect(eq(u16.size(), static_cast<size_t>(8))) << u16.size();
  };

  "build_pwsh_argv produces the reference one-shot argv"_test = [] {
    const pwsh::pwsh_argv_result r = pwsh::build_pwsh_argv(
        "Get-Process", "C:/Program Files/PowerShell/7/pwsh.exe");
    expect(r.status == tool_status::ok)
        << "status=" << static_cast<int>(r.status) << " msg=" << r.message;
    expect(eq(r.argv.size(), static_cast<size_t>(8)))
        << "argv=" << r.argv.size();
    if (r.argv.size() == 8) {
      expect(r.argv[0] == "C:/Program Files/PowerShell/7/pwsh.exe")
          << r.argv[0];
      expect(r.argv[1] == "-NoP") << r.argv[1];
      expect(r.argv[2] == "-NonI") << r.argv[2];
      expect(r.argv[3] == "-Exec") << r.argv[3];
      expect(r.argv[4] == "Bypass") << r.argv[4];
      expect(r.argv[5] == "-NoL") << r.argv[5];
      expect(r.argv[6] == "-C") << r.argv[6];
      expect(r.argv[7].find("try{Get-Process}catch{") != kimix::string::npos)
          << r.argv[7];
    }
    // The parser repairs an unbalanced quote (reference behaviour: a
    // warning, not a refusal), so only a blank command is unusable.
    const pwsh::pwsh_argv_result repaired =
        pwsh::build_pwsh_argv("Write-Output \"unclosed", "pwsh.exe");
    expect(repaired.status == tool_status::ok) << repaired.message;
    expect(repaired.prepared.find("\"") != kimix::string::npos)
        << repaired.prepared;
    const pwsh::pwsh_argv_result blank =
        pwsh::build_pwsh_argv("   ", "pwsh.exe");
    expect(blank.status != tool_status::ok);
    expect(eq(blank.argv.size(), static_cast<size_t>(0)));
    // No host at all -> unsupported (never an empty argv spawn).
    const pwsh::pwsh_argv_result none = pwsh::build_pwsh_argv("Get-Date", "");
    expect(none.status == tool_status::ok ||
           none.status == tool_status::unsupported);
  };

  "pwsh host detection distinguishes 5.1 from 7"_test = [] {
    expect(pwsh::pwsh_is_windows_powershell(
        "C:/Windows/System32/WindowsPowerShell/v1.0/powershell.EXE"));
    expect(pwsh::pwsh_is_windows_powershell("powershell"));
    expect(!pwsh::pwsh_is_windows_powershell(
        "C:/Program Files/PowerShell/7/pwsh.exe"));
    expect(!pwsh::pwsh_is_windows_powershell("pwsh"));
  };

  "interactive argv keeps the console attached"_test = [] {
    const kimix::vector<kimix::string> argv = pwsh::build_pwsh_interactive_argv(
        "Get-Date", "C:/Program Files/PowerShell/7/pwsh.exe");
    expect(eq(argv.size(), static_cast<size_t>(8))) << argv.size();
    bool has_noexit = false;
    bool has_noninteractive = false;
    for (const kimix::string &a : argv) {
      if (a == "-NoExit") {
        has_noexit = true;
      }
      if (a == "-NonI") {
        has_noninteractive = true;
      }
    }
    expect(has_noexit) << "a REPL must survive its first command";
    expect(!has_noninteractive) << "-NonI would kill the interactive console";
    expect(!argv.empty() &&
           argv.back().find("Get-Date") != kimix::string::npos);
  };

  return 0;
}
