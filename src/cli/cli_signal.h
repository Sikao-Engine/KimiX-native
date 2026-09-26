// cli/cli_signal.h - Cross-platform Ctrl-C handling for the native CLI (G8).
//
// Hides the platform detail behind one module:
// * Windows: SetConsoleCtrlHandler - the console thread runs the handler,
//   which stores `true` into the registered flag and reports the event as
//   HANDLED so the process is not terminated and the (blocked) console read
//   is left alone;
// * POSIX: sigaction(SIGINT, ...) without SA_RESTART - the handler stores
//   `true` into the registered flag; slow reads may return EINTR, but the
//   pollers below never rely on that.
//
// The handler only performs ONE async-signal-safe lock-free atomic store; the
// owner (the REPL) polls the flag: at the prompt a Ctrl-C means "print bye.
// and exit" (kimix/cli_impl/core.py's KeyboardInterrupt -> "\nbye."), while a
// running turn polls it through its CancelToken, aborts at the next step
// boundary, interrupts the in-flight request and keeps the session (the
// reference's "Keyboard Interrupt." mid-turn semantics).
//
// One live registration per process: installing again moves the flag.

#pragma once

#include <atomic>

#include <core/kimix_core.h>

namespace kimix::cli {

// Register the Ctrl-C handler storing into `flag` (borrowed: must outlive the
// registration). False when the platform call fails.
bool install_ctrlc_handler(std::atomic<bool> *flag) noexcept;

// Restore the default handling and forget the flag.
void uninstall_ctrlc_handler() noexcept;

// Test hook: flip the registered flag exactly as the platform handler would
// (no signal is raised, so tests need no console/permissions).
void test_simulate_ctrlc() noexcept;

// True after a Ctrl-C was seen since the last consume_ctrlc().
bool ctrlc_pending() noexcept;
// Test hook: clear the seen state (the real flag is one-shot until reset by
// the owner; tests reset between scenarios).
void test_reset_ctrlc() noexcept;

} // namespace kimix::cli
