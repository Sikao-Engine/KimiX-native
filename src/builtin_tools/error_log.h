// error_log.h - Reflection-mode tool-error log (one JSONL file per session).
//
// Plan: reflection mode is a config-level switch (the top-level "reflection"
// key of the provider config, default false; see src/cli/cli_config.*). When
// the agent runs with reflection on, every built-in tool call whose result
// payload is NOT status "ok" is recorded here: the ORIGINAL arguments, the
// time the tool spent (kimix::Clock), and the returned message + output.
//
// The record lands in <work_dir>/.kimix_cache/error_log/<session_id>.jsonl -
// ONE file per session, one JSON object per line, appended. A session without
// an id logs to "default.jsonl". Writing is best-effort: a logging failure
// never fails the tool call.
//
// Design rules (see tool.h): namespace kimix::builtin_tools; kimix:: strings
// in the API; no exceptions; JSON through the vendored yyjson with the
// mimalloc allocator (kimix::llm::kYYJsonAlcMi).

#pragma once

#include <core/kimix_core.h> // kimix::string, string_view

namespace kimix::builtin_tools {

// One reflection-mode error record (the fields of a single JSONL line).
struct tool_error_record {
    kimix::string tool;      // the registry name of the failed tool
    kimix::string arguments; // the ORIGINAL tool-call arguments JSON
    double elapsed_ms = 0.0; // kimix::Clock-measured tool time
    kimix::string message;   // the result payload's message
    kimix::string output;    // the result payload's output
};

// Append one record to <work_dir>/.kimix_cache/error_log/<session_id>.jsonl
// (creating the directories and the file as needed). `work_dir` may be empty
// (the process cwd applies), `session_id` may be empty ("default"). Never
// throws; a failure to build the line, create the directory or open/append
// the file is silently ignored - reflection logging must not break dispatch.
void tool_error_log_append(kimix::string_view work_dir,
                           kimix::string_view session_id,
                           const tool_error_record &record);

} // namespace kimix::builtin_tools
