// test_cli_signal.cpp - Unit tests for the cross-platform Ctrl-C module
// (src/cli/cli_signal.*, gap G8): installing the handler wires the platform
// facility (SetConsoleCtrlHandler / sigaction(SIGINT)) to one atomic flag;
// test_simulate_ctrlc() flips the flag exactly as the platform handler would
// so no real signal/console is needed.
//
// Framework: Boost.UT (tests/ut/ut.hpp).

#include "ut/ut.hpp"

#include <cli/cli_signal.h>

#include <atomic>

using namespace boost::ut;
using namespace boost::ut::literals;

int main() {
    "install_pending_uninstall"_test = [] {
        std::atomic<bool> flag{false};
        expect(kimix::cli::install_ctrlc_handler(&flag));
        expect(!flag.load());
        expect(!kimix::cli::ctrlc_pending());
        kimix::cli::test_simulate_ctrlc();
        expect(flag.load()); // the handler stored into the registered flag
        expect(kimix::cli::ctrlc_pending());
        kimix::cli::test_reset_ctrlc();
        expect(!kimix::cli::ctrlc_pending());
        kimix::cli::uninstall_ctrlc_handler();
        expect(!kimix::cli::ctrlc_pending());
    };

    "reinstall_moves_the_flag"_test = [] {
        std::atomic<bool> first{false};
        std::atomic<bool> second{false};
        expect(kimix::cli::install_ctrlc_handler(&first));
        expect(kimix::cli::install_ctrlc_handler(&second));
        kimix::cli::test_simulate_ctrlc();
        expect(!first.load());
        expect(second.load());
        kimix::cli::uninstall_ctrlc_handler();
        kimix::cli::test_simulate_ctrlc(); // no registration: no-op, no crash
        expect(!kimix::cli::ctrlc_pending());
    };

    "null_flag_rejected"_test = [] {
        expect(!kimix::cli::install_ctrlc_handler(nullptr));
    };
}
