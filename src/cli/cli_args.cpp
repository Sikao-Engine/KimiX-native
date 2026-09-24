// cli/cli_args.cpp - command-line parsing and the help/usage text.
//
// The help text is the reference constants.py::HELP_STR, *generated* into
// cli_help_text.inc by scripts/gen_cli_help.py (byte-exact, column alignment
// and 2-space indents included); this file only concatenates the segments and
// wraps the slash-command names in Color.YELLOW when colour is enabled.

#include <cstring>

#include "cli/cli_args.h"
#include "cli/cli_common.h"

namespace kimix::cli {

namespace {

// Generated help text segments (scripts/gen_cli_help.py).
#include "cli_help_text.inc"

const char *const kSubcommands[] = {"serve", "gui", "ssecli", "mcp"};

bool clia_is_subcommand(kimix::string_view text) {
    for (const char *const name : kSubcommands) {
        if (text == name) {
            return true;
        }
    }
    return false;
}

// ANSI yellow (Color.YELLOW == 33) applied without touching the global colour
// state, so the help text is deterministic for a given `colorful` argument.
kimix::string clia_yellow(kimix::string_view text, bool colorful) {
    if (!colorful) {
        return kimix::string(text);
    }
    kimix::string out("\x1b[33m");
    out.append(text);
    out.append("\x1b[0m");
    return out;
}

// Cursor over argv that consumes options in both their "--opt=value" and
// "--opt value" spellings.
struct arg_cursor {
    int argc = 0;
    char **argv = nullptr;
    int i = 1;

    kimix::string_view peek() const {
        return (i < argc) ? kimix::string_view(argv[i]) : kimix::string_view();
    }

    kimix::string_view next() {
        return (i < argc) ? kimix::string_view(argv[i++]) : kimix::string_view();
    }

    // Match a value-taking option.  Returns the 1-based index of the matched
    // name (0 == no match); `value` receives the inline or following token.
    int match_value(const kimix::vector<kimix::string_view> &names,
                    kimix::string &value, kimix::string &error) {
        const kimix::string_view arg = peek();
        for (size_t n = 0; n < names.size(); ++n) {
            const kimix::string_view name = names[n];
            if (arg == name) {
                if (i + 1 >= argc) {
                    error = "argument " + kimix::string(name) + ": expected one argument";
                    i = argc; // stop parsing
                    return 0;
                }
                value.assign(argv[i + 1]);
                i += 2;
                return static_cast<int>(n + 1);
            }
            if (arg.size() > name.size() && arg.compare(0, name.size(), name) == 0 &&
                arg[name.size()] == '=') {
                value.assign(arg.substr(name.size() + 1));
                ++i;
                return static_cast<int>(n + 1);
            }
        }
        return 0;
    }

    // Match a boolean flag; consumes the token and returns true on a match.
    bool match_flag(const kimix::vector<kimix::string_view> &names) {
        const kimix::string_view arg = peek();
        for (const kimix::string_view name : names) {
            if (arg == name) {
                ++i;
                return true;
            }
        }
        return false;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
bool parse_args(int argc, char **argv, cli_options &out) {
    out = cli_options{};
    if (argc > 0 && argv != nullptr && argv[0] != nullptr) {
        const kimix::string full(argv[0]);
        const size_t slash = full.find_last_of("/\\");
        out.program_name = (slash == kimix::string::npos) ? full : full.substr(slash + 1);
    } else {
        out.program_name = "kimix_cli";
    }

    arg_cursor cur;
    cur.argc = argc;
    cur.argv = argv;

    bool end_of_options = false;
    while (cur.i < argc) {
        const kimix::string_view arg = cur.peek();

        if (end_of_options) {
            // Positionals are unsupported (the reference's argparse exits 2 for
            // them): report and stop.
            out.errors.push_back("unrecognized arguments: " + kimix::string(arg) +
                                 " (use -p/--prompt TEXT to send one prompt)");
            ++cur.i;
            continue;
        }
        if (arg == "--") {
            end_of_options = true;
            ++cur.i;
            continue;
        }

        kimix::string value;
        kimix::string error;

        // ---- flags ---------------------------------------------------------
        if (cur.match_flag({"-h", "--help"})) {
            out.help = true;
            continue;
        }
        if (cur.match_flag({"--version"})) {
            out.version = true;
            continue;
        }
        if (cur.match_flag({"-c", "--clean"})) {
            out.clean = true;
            continue;
        }
        if (cur.match_flag({"--manually-cot"})) {
            out.manually_cot = true;
            continue;
        }
        if (cur.match_flag({"--no_color", "-no_color"})) {
            out.no_color = true;
            continue;
        }
        if (cur.match_flag({"--no_think", "-no_think"})) {
            out.no_think = true;
            continue;
        }
        if (cur.match_flag({"--no_yolo", "-no_yolo"})) {
            out.no_yolo = true;
            continue;
        }
        if (cur.match_flag({"--dry-run", "--dry_run"})) {
            out.dry_run = true;
            continue;
        }
        if (cur.match_flag({"--interactive"})) {
            out.interactive_forced = true;
            continue;
        }

        // ---- value options -------------------------------------------------
        int hit = cur.match_value({"-p", "--prompt"}, value, error);
        if (!error.empty()) {
            out.errors.push_back(error);
            break;
        }
        if (hit != 0) {
            out.prompt = value;
            out.has_prompt = true;
            continue;
        }
        hit = cur.match_value({"--script"}, value, error);
        if (!error.empty()) {
            out.errors.push_back(error);
            break;
        }
        if (hit != 0) {
            out.script_path = value;
            continue;
        }
        hit = cur.match_value({"--work-dir", "--work_dir"}, value, error);
        if (!error.empty()) {
            out.errors.push_back(error);
            break;
        }
        if (hit != 0) {
            out.work_dir = value;
            continue;
        }
        hit = cur.match_value({"--agent-file", "--agent_file"}, value, error);
        if (!error.empty()) {
            out.errors.push_back(error);
            break;
        }
        if (hit != 0) {
            out.agent_file = value;
            continue;
        }
        // --provider is provider-only; --config may also carry the agent section.
        hit = cur.match_value({"--provider"}, value, error);
        if (!error.empty()) {
            out.errors.push_back(error);
            break;
        }
        if (hit != 0) {
            if (out.config_path.empty()) {
                out.config_path = value;
                out.config_is_provider_only = true;
            }
            continue;
        }
        hit = cur.match_value({"--config"}, value, error);
        if (!error.empty()) {
            out.errors.push_back(error);
            break;
        }
        if (hit != 0) {
            if (out.config_path.empty()) {
                out.config_path = value;
                out.config_is_provider_only = false;
            }
            continue;
        }

        // -s/--skill-dir takes zero or more values (argparse nargs="*").
        if (arg == "-s" || arg == "--skill-dir" || arg == "--skill_dir") {
            ++cur.i;
            while (cur.i < argc) {
                const kimix::string_view next_arg = cur.peek();
                if (next_arg.empty() || next_arg[0] == '-' ||
                    clia_is_subcommand(next_arg)) {
                    break;
                }
                out.skill_dirs.push_back(kimix::string(next_arg));
                ++cur.i;
            }
            continue;
        }

        // ---- subcommand ----------------------------------------------------
        if (out.subcommand.empty() && clia_is_subcommand(arg)) {
            out.subcommand = kimix::string(arg);
            ++cur.i;
            while (cur.i < argc) {
                out.subcommand_args.push_back(kimix::string(cur.next()));
            }
            break;
        }

        // ---- unknown -------------------------------------------------------
        if (!arg.empty() && arg[0] == '-') {
            out.errors.push_back("unrecognized argument: " + kimix::string(arg));
            ++cur.i;
            continue;
        }
        out.errors.push_back("unrecognized arguments: " + kimix::string(arg) +
                             " (use -p/--prompt TEXT to send one prompt)");
        ++cur.i;
    }

    return out.errors.empty();
}

// ---------------------------------------------------------------------------
// Usage / help text
// ---------------------------------------------------------------------------
kimix::string cli_usage_line(kimix::string_view program_name) {
    return "usage: " + kimix::string(program_name) +
           " [-h] [-c] [--no_think] [--no_yolo] [--no_color] [--manually-cot]\n"
           "                       [-s [SKILL_DIR ...]] [--provider FILE]\n"
           "                       [--agent-file FILE] [--work-dir DIR]\n"
           "                       [-p PROMPT] [--script FILE] [--dry-run]";
}

kimix::string cli_help_text(bool colorful) {
    // The text itself is generated from the reference HELP_STR so it stays
    // byte-identical (column alignment and 2-space indents included).
    kimix::string out;
    for (const cli_help_segment &segment : kCliHelpSegments) {
        out.append(clia_yellow(segment.text, colorful && segment.is_command));
    }
    return out;
}

kimix::string cli_help_text_extended(bool colorful) {
    kimix::string out = cli_help_text(colorful);
    out.append("\n");
    out.append("Native additions (not in the Python CLI):\n");
    out.append("    --provider FILE - LLM provider JSON (same schema as --config)\n");
    out.append("    --agent-file FILE - Agent manifest JSON ({agent:{tools:[...]}})\n");
    out.append("    --work-dir DIR - Session working directory (default: cwd)\n");
    out.append("    -p, --prompt TEXT - Run one turn with TEXT and exit\n");
    out.append("    --script FILE - Feed FILE's lines to the REPL, then exit\n");
    out.append("    --dry-run - Validate the configs and print the resolved plan\n");
    out.append("    --interactive - Force the REPL even when stdin is not a console\n");
    out.append("    --version - Print the version and exit\n");
    return out;
}

} // namespace kimix::cli
