// cli/cli_args.h - Command-line parsing for the native CLI.
//
// Port of kimi-agent's kimix/cli_impl/args.py together with the help/usage text
// of kimix/cli_impl/constants.py.  The reference parses with argparse and then
// post-scans the leftovers for `--config`; this parser recognises the same
// option set in one pass (both `--opt=value` and `--opt value` forms), which
// accepts a superset of the reference command lines while keeping the same
// failure semantics: an unknown option/positional and a missing option value are
// usage errors (exit code 2).
//
// Native additions (documented in src/cli/PLAN.md §5):
//   -p/--prompt TEXT   run one turn and exit (non-interactive)
//   --script PATH      feed PATH's lines to the REPL as input
//   --dry-run          parse/validate configs and print the resolved plan
//   --provider PATH    provider-only JSON (like --config but explicit)
//   --agent-file PATH  agent manifest JSON (tool list / prompts)
//   --work-dir PATH    session working directory (default: process cwd)
//   --interactive      force the REPL even when stdin is not a console
//   --version          print the version and exit

#pragma once

#include <cstdint>
#include <core/kimix_core.h>

namespace kimix::cli {

// Exit codes used by main()/cli_main(); mirrors the reference where it defines
// one and adds the native ones.
enum exit_code : int32_t {
    kExitOk = 0,          // success
    kExitConfig = 1,      // config file missing/invalid (reference: sys.exit(1))
    kExitUsage = 2,       // argparse usage error
    kExitUnsupported = 3, // recognised but unsupported feature (serve/gui/...)
    kExitRuntime = 4,     // runtime failure (LLM/tool/session error)
};

struct cli_options {
    // Reference flags -------------------------------------------------------
    bool clean = false;         // -c / --clean
    bool no_color = false;      // --no_color
    bool no_think = false;      // --no_think
    bool no_yolo = false;       // --no_yolo
    bool manually_cot = false;  // --manually-cot
    bool help = false;          // -h / --help
    kimix::vector<kimix::string> skill_dirs; // -s / --skill-dir [DIR...]

    // Native additions -----------------------------------------------------
    bool version = false;        // --version
    bool dry_run = false;        // --dry-run
    bool interactive_forced = false; // --interactive
    bool has_prompt = false;     // -p / --prompt
    kimix::string prompt;
    kimix::string script_path;   // --script
    kimix::string work_dir;      // --work-dir (default: cwd)
    kimix::string agent_file;    // --agent-file
    kimix::string config_path;   // --config / --provider
    // True when the config came from --provider (provider JSON only); a
    // --config file may additionally carry the agent section.
    bool config_is_provider_only = false;

    // Subcommands (recognised, then refused by the CLI) ----------------------
    kimix::string subcommand;                       // "" | serve | gui | ssecli | mcp
    kimix::vector<kimix::string> subcommand_args;   // remaining tokens, verbatim

    kimix::vector<kimix::string> errors; // usage errors -> exit 2
    kimix::string program_name;          // argv[0] basename, for usage text
};

// Parse argv (argv[0] is the program name).  Returns false and fills
// `out.errors` when the command line is invalid.
bool parse_args(int argc, char **argv, cli_options &out);

// The one-line synopsis used in usage errors.
kimix::string cli_usage_line(kimix::string_view program_name);

// One segment of the reference help text (see src/cli/cli_help_text.inc, which
// scripts/gen_cli_help.py generates from the Python constants.py::HELP_STR).
// `is_command` marks a slash-command name the caller wraps in Color.YELLOW.
struct cli_help_segment {
    const char *text;
    bool is_command;
};

// Port of constants.HELP_STR: the full help text, with the command names
// wrapped in ANSI yellow when `colorful` is true.
kimix::string cli_help_text(bool colorful);

// The `/help` text with the native additions listed (used by the REPL banner).
kimix::string cli_help_text_extended(bool colorful);

} // namespace kimix::cli
