// cli/main.cpp - entry point of the native CLI executable (kimix_cli).
//
// The whole CLI lives in the kimix-cli library (src/cli/*.cpp); main() only
// configures the console and forwards to kimix::cli::cli_main() so the CLI is
// drivable from tests as well.  See src/cli/PLAN.md for the port map and
// src/cli/reports/cli_commands.md for every documented deviation.
//
// Exit-code contract (unchanged since S1):
//   0 = ok, 1 = config missing/invalid, 2 = usage error,
//   3 = recognised but unsupported subcommand (serve/gui/ssecli/mcp),
//   4 = runtime failure (a failed --prompt turn).

#include <core/kimix_core.h>
#include <core/fiber.h> // the root main binds the process-wide fiber pool

#include "cli/cli_app.h"
#include "cli/cli_common.h"

int main(int argc, char **argv) {
    // Windows: UTF-8 code page + ENABLE_VIRTUAL_TERMINAL_PROCESSING so colour
    // escapes and UTF-8 output work; a no-op elsewhere.
    kimix::cli::enable_console_ansi();
    // Process-wide fiber scheduler, bound HERE at the root main for the whole
    // run: tool fan-outs (grep engine, parallel tool dispatch, workflow
    // best-of-n / swarm, background sub-agents, interactive task drains)
    // submit to whatever pool the calling thread is bound to and never create
    // one themselves. The shared pool is lazily created (one worker per
    // logical core, floor 4, KIMIX_FIBER_WORKER_THREADS overrides) and
    // intentionally NEVER destroyed, so main deliberately never unbinds:
    // exit must not wait on an in-flight background fiber.
    kimix::fiber::shared_scheduler().bind();
    const int code = kimix::cli::cli_main(argc, argv);
    kimix::cli::flush_streams();
    return code;
}
