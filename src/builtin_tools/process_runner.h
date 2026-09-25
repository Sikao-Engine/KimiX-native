// process_runner.h - Async (non-blocking poll loop) subprocess execution on
// top of the vendored reproc library. This is the real spawn/stream/wait layer
// the bash and python tools were waiting for (bash_tool.h "follow-up phase
// that moves the actual spawn into C++").
//
// The runner mirrors kimi-agent's ProcessTask + background/utils.py bounded
// run loop: the child is started with piped, non-blocking stdout/stderr; a
// reproc_poll loop drains both streams and feeds every chunk into the bash
// tool's pure capture_machine, which decides when to stop (wait pattern
// matched, total timeout -> kill, inactivity timeout -> stop, process exit ->
// final drain). Interactive ("send" / "interactive" mode) tasks keep running
// in a global task registry so later calls can send input or read buffered
// output by task id.
//
// Namespace: kimix::builtin_tools::proc.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "builtin_tools/tool_types.h"

namespace kimix::builtin_tools::proc {

using kimix::builtin_tools::tool_error;
using kimix::builtin_tools::tool_status;

// Everything the runner needs to start one child process.
struct run_options {
  kimix::vector<kimix::string> argv; // argv[0] is the executable
  kimix::string working_directory;   // "" == inherit parent cwd
  kimix::vector<kimix::string>
      extra_env;                     // "KEY=VALUE" entries (extend parent env)
  int64_t timeout_ms = 30000;        // total run bound; <= 0 disables
  int64_t inactivity_timeout_ms = 0; // 0 == disabled
  int64_t output_cap_chars = 200000; // bounded-append capture cap
  kimix::string wait_pattern;        // "" == no early stop pattern
  kimix::string stdin_input;         // pre-written to child stdin, then closed
  // When non-empty, the child's stderr is redirected to this file and the
  // runner tails it into the merged capture buffer. Needed on Windows:
  // MSYS2/Git Bash cannot duplicate reproc's socket-based pipes, so a plain
  // stderr pipe breaks any `>&2` redirection ("cannot duplicate fd").
  kimix::string stderr_path;
  // Requested task id for start_task() ("" == the default "task_<n>" scheme).
  // The Run tool passes "run_<executable stem>" so job_output can report the
  // originating kind, mirroring background/utils.py generate_task_id(kind,
  // name). A collision gets the same "_<n>" suffix the Python allocator adds.
  kimix::string requested_task_id;
};

// Outcome of one bounded foreground run.
struct run_result {
  tool_status status = tool_status::ok;
  kimix::string output;               // merged stdout+stderr (bounded)
  kimix::optional<int64_t> exit_code; // nullopt when killed/still running
  bool matched = false;               // wait pattern matched before timeout
  bool truncated = false;             // capture cap truncation happened
  bool killed = false;                // process was terminated on timeout
  bool still_running = false;         // stopped early; child handed to the
                                      // task registry (see task_id)
  kimix::string task_id;              // set when still_running: the adopted
                                      // child's registry id, readable and
                                      // stoppable through job_output
  int64_t elapsed_ms = 0;
  kimix::string spawn_error; // set when the spawn itself failed
};

// Run one bounded foreground process: spawn, poll, drain, enforce the
// timeout / inactivity / wait-pattern policy, and kill on total timeout.
// Never throws; failures are reported through run_result.
//
// `stdin_input` is written incrementally from inside the poll loop (a single
// reproc_write only delivers "up to size" bytes and the child may not be
// reading yet), then the child's stdin is closed so programs that read until
// EOF terminate. When the inactivity bound fires first the child is NOT killed:
// it is adopted into the interactive task registry and `task_id` names it, so
// the "running in background" report the tools give is true.
run_result run_process(const run_options &opts);

// ---------------------------------------------------------------------------
// Interactive task registry (bash mode "interactive" / "send" / task output)
// ---------------------------------------------------------------------------

// A long-lived child with a drain thread. Created by start_task(); later
// calls address it by task id. The drain thread OWNS the reproc handle: every
// reproc_wait/reproc_read/reproc_poll/reproc_stop call for a live task runs on
// that thread, because reproc forbids driving one process from two threads at
// the same time (reproc README, Multithreading: "waiting on the same child
// process from two different threads at the same time will result in issues").
// A stop request therefore only sets a flag; the owner terminates the child,
// and the requester joins the thread before reproc_destroy. The registry owns
// the handle and the Windows stdin write handle. On Windows the task's stdin
// is a real Win32 anonymous pipe (MSYS cannot read reproc's socket pipes), so
// the registry also owns the parent-side write handle.
struct task_handle {
  kimix::string task_id;
  int64_t pid = 0;
};

// Start a background/interactive task. Returns the task id ("" + error on
// failure). The drain thread keeps reading stdout/stderr into a bounded
// buffer until the process exits or the task is stopped.
tool_error start_task(const run_options &opts, task_handle &out);

// Send text to the task's stdin (plus a trailing '\n' when add_newline).
tool_error send_task(kimix::string_view task_id, kimix::string_view text,
                     bool add_newline);

// Block up to `timeout_ms` waiting until `pattern` appears in the task's
// accumulated output since the last read, the process exits, or the timeout
// expires. Returns matched/exited flags.
struct task_wait_result {
  bool matched = false;
  bool exited = false;
  int64_t elapsed_ms = 0;
};
task_wait_result wait_task(kimix::string_view task_id,
                           kimix::string_view pattern, int64_t timeout_ms);

// Read (and consume) all output buffered since the previous read.
tool_error read_task(kimix::string_view task_id, kimix::string &out);

// Read output without consuming it (peek).
tool_error peek_task(kimix::string_view task_id, kimix::string &out);

struct task_status_info {
  bool exists = false;
  bool exited = false;
  kimix::optional<int64_t> exit_code;
  int64_t pid = 0;
  int64_t elapsed_ms = 0; // since start_task(); 0 when unknown
};
task_status_info query_task(kimix::string_view task_id);

// Terminate + kill the task's process tree and join its drain thread.
tool_error stop_task(kimix::string_view task_id);

// Same as stop_task, but hands back whatever output was still buffered.
// The drain thread is joined BEFORE the buffer is taken, so `final_output`
// is the complete tail (job_output action='kill' mirrors Python's
// `await stream.stop(); output = await stream.pop_output()`).
tool_error stop_task(kimix::string_view task_id, kimix::string &final_output);

// Stop every live task (called on process shutdown).
void stop_all_tasks();

// -----------------------------------------------------------------------
// Registry enumeration (job_output action='list' / TaskOutput._list_tasks)
// -----------------------------------------------------------------------
// Snapshot of one registered task, in registration order.
struct task_summary {
  kimix::string task_id;
  int64_t pid = 0;
  bool exited = false;                // drain loop saw the process exit
  kimix::optional<int64_t> exit_code; // set once exited
  int64_t elapsed_ms = 0;             // wall time since the task was started
};

// List every task currently in the registry (live and finished-but-not-yet
// removed). Never throws.
kimix::vector<task_summary> list_tasks();

// Drop a task from the registry (its process is stopped and the drain
// thread joined first). Port of background/utils.py remove_task_id: the
// Python tools call it once a foreground task has been fully drained so a
// later job_output list no longer shows it. not_found when unknown.
tool_error remove_task(kimix::string_view task_id);

// -----------------------------------------------------------------------
// Captured-output sanitization
// -----------------------------------------------------------------------
// Lossy UTF-8 decode of raw child-process bytes with CPython
// errors='replace' parity: every invalid byte or malformed sequence
// becomes U+FFFD (EF BF BD). Child processes may emit arbitrary byte
// streams (a Chinese-locale timeout.exe prints GBK, tools dump binary),
// and ToolParams::serialize rejects invalid UTF-8 (yyjson_mut_strncpy
// returns NULL), which would turn any such output into a hard tool
// error. Applied centrally in the capture feed (pr_feed), so Run / Bash /
// Python / JobOutput all present replacement characters exactly like the
// Python tools. Pure; ASCII input is returned unchanged.
kimix::string sanitize_utf8(kimix::string_view bytes);

} // namespace kimix::builtin_tools::proc
