// cli/main.cpp - entry point of the native CLI executable (kimix_cli).
//
// The interesting work lives in the kimix-cli library (src/cli/*.cpp); main()
// only forwards to kimix::cli::cli_main() so the whole CLI is drivable from
// tests as well.  See src/cli/PLAN.md for the port map and the deviations.

#include <cstdio>
#include <core/kimix_core.h>

#include "cli/cli_args.h"
#include "cli/cli_common.h"
#include "cli/cli_print.h"

namespace {

// Temporary bootstrap used until the app/repl layer lands (S5 in
// src/cli/PLAN.md): it already implements the argument contract, the help and
// version commands, and the usage/unsupported exit codes, so the target is
// runnable and testable from day one.
int bootstrap_main(kimix::cli::cli_options &opts) {
    namespace cli = kimix::cli;
    cli::init_printing(opts.no_color);
    cli::set_quiet(false);

    if (opts.help) {
        cli::print_string(cli::cli_help_text_extended(cli::colorful()));
        return cli::kExitOk;
    }
    if (opts.version) {
        cli::print_string(kimix::string("kimix_cli ") + KIMIX_CORE_VERSION +
                          " (kimix " KIMIX_CORE_VERSION ")");
        return cli::kExitOk;
    }
    if (!opts.subcommand.empty()) {
        cli::print_error(opts.subcommand +
                         ": not supported by the native CLI (the Python CLI's "
                         "serve/gui/ssecli/mcp front ends are Python-only)");
        return cli::kExitUnsupported;
    }
    if (opts.dry_run) {
        cli::print_error("--dry-run: the config layer is not wired up yet "
                         "(step S2 in src/cli/PLAN.md)");
        return cli::kExitRuntime;
    }
    cli::print_error("the agent loop is not wired up yet "
                     "(step S5 in src/cli/PLAN.md); use --help for the options");
    return cli::kExitRuntime;
}

} // namespace

int main(int argc, char **argv) {
    namespace cli = kimix::cli;

    cli::cli_options opts;
    const bool parsed = cli::parse_args(argc, argv, opts);

    // Print early so even usage errors respect --no_color.
    cli::init_printing(opts.no_color);

    if (!parsed) {
        for (const kimix::string &error : opts.errors) {
            cli::print_error(error);
        }
        cli::print_string(cli::cli_usage_line(opts.program_name));
        cli::print_string("try '" + opts.program_name + " --help' for more information");
        return cli::kExitUsage;
    }

    const int code = bootstrap_main(opts);
    cli::flush_streams();
    return code;
}
