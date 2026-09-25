// cli/cli_repl.h - The interactive REPL: the port of
// kimix/cli_impl/core.py::_client_cli (PLAN.md §3.7).
//
// The loop prints the reference prompt (app_prompt_line()) and reads one input
// at a time; a blank line is skipped, a '/' input is split at the first colon
// found in the TRIMMED remainder (the slicing itself uses the untrimmed string),
// looked up in command_map() with the `unknown` fallback, and a non-slash token
// that names an existing file is either reported as unsupported (.py - the
// native CLI has no embedded Python interpreter) or read and sent as the prompt.
// EOF (or Ctrl-C at a prompt, which F6 behaves as EOF) prints "\nbye." and ends
// the loop with exit code 0.
//
// Rules: namespace kimix::cli, exception-free, unity build (TU-local helpers in
// an anonymous namespace with the `clirpl_` prefix).

#pragma once

#include <cstdint>
#include <cstdio>

#include <core/kimix_core.h>

#include "cli/cli_app.h"

namespace kimix::cli {

// Run the REPL until EOF / Ctrl-C / a `should_break` command.  `scripted` seeds
// the reference's `text_arr` queue (the --script lines); the queue is consulted
// before `in` on every iteration.  The reference prompt is written to `out`.
// Returns app.exit_code (0 unless a handler set one).
int repl_run(app_context &app, std::FILE *in, std::FILE *out,
             const kimix::vector<kimix::string> &scripted);

} // namespace kimix::cli
