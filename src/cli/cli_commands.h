// cli/cli_commands.h - The slash-command table (PLAN.md §3.7/§3.8).
//
// Port of kimix/cli_impl/commands.py: `_command_map` (21 handlers, verbatim
// keys, case-sensitive lookup, no trimming) plus the `_cmd_unknown` fallback.
// A handler receives the reference's `task_split` list (the command name and,
// when the input carried a ':', everything after the first colon), the app
// context and the `text_arr` queue; it returns
//   * `has_input` + `next_input` - the next REPL input to process (the native
//     form of the reference's `(new_input_str, should_break)` tuple; see the
//     deviation note in src/cli/reports/cli_commands.md),
//   * `should_break`            - exit the REPL (/exit).
//
// Rules (see src/cli/PLAN.md §3 and .agents/skills/cpp): namespace kimix::cli,
// exception-free, no RTTI, kimix:: containers in every public API, unity build
// (batch 8) so every TU-local helper lives in an anonymous namespace with the
// `clicmd_` prefix and no file-scope `using namespace` appears.

#pragma once

#include <core/kimix_core.h>

#include "cli/cli_app.h"

namespace kimix::cli {

// What a handler asks the REPL to do next.
struct command_result {
    bool has_input = false;  // feed `next_input` as the next REPL input
    kimix::string next_input;
    bool should_break = false; // exit the REPL
};

struct command_entry {
    kimix::string name; // "help", "clear", ... (no leading '/')
    kimix::string help; // one-line help (for /help completion)
    kimix::function<command_result(const kimix::vector<kimix::string> &args, app_context &app,
                                   kimix::vector<kimix::string> &text_arr)>
        handler;
};

// The command table, in the reference's `_command_map` insertion order, with
// the `unknown` fallback entry last.  `help` of every entry is the HELP_STR
// description.
const kimix::vector<command_entry> &command_map();

// Exact (case-sensitive, unstripped) name lookup; nullptr when the name is not
// a command.  The REPL's fallback is `find_command("unknown")`.
const command_entry *find_command(kimix::string_view name);

// `_split_text(lines, _command_map_keys)`: split multi-line input into queue
// entries - a trimmed line that starts with a KNOWN `/command` becomes its own
// entry, every other line joins the current block (blank lines become '').
kimix::vector<kimix::string> split_text_blocks(const kimix::vector<kimix::string> &lines);

} // namespace kimix::cli
