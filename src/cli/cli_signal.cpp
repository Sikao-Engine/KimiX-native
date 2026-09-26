// cli/cli_signal.cpp - see cli_signal.h.

#include "cli/cli_signal.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#endif

namespace kimix::cli {

namespace {
std::atomic<bool> *g_flag = nullptr;

#if defined(_WIN32)
BOOL WINAPI clisig_console_handler(DWORD event) noexcept {
    if (event != CTRL_C_EVENT) {
        return FALSE; // let the default process the close/logoff events
    }
    std::atomic<bool> *flag = g_flag;
    if (flag != nullptr) {
        flag->store(true, std::memory_order_release);
    }
    return TRUE; // handled: do not terminate the process
}
#else
void clisig_sigint_handler(int) noexcept {
    std::atomic<bool> *flag = g_flag;
    if (flag != nullptr) {
        flag->store(true, std::memory_order_release);
    }
}
#endif
} // namespace

bool install_ctrlc_handler(std::atomic<bool> *flag) noexcept {
    if (flag == nullptr) {
        return false;
    }
#if defined(_WIN32)
    g_flag = flag;
    return SetConsoleCtrlHandler(clisig_console_handler, TRUE) != 0;
#else
    struct sigaction sa;
    sa.sa_handler = &clisig_sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // no SA_RESTART: slow reads may report EINTR
    g_flag = flag;
    return sigaction(SIGINT, &sa, nullptr) == 0;
#endif
}

void uninstall_ctrlc_handler() noexcept {
#if defined(_WIN32)
    if (g_flag != nullptr) {
        SetConsoleCtrlHandler(clisig_console_handler, FALSE);
    }
#else
    if (g_flag != nullptr) {
        struct sigaction sa;
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, nullptr);
    }
#endif
    g_flag = nullptr;
}

void test_simulate_ctrlc() noexcept {
    std::atomic<bool> *flag = g_flag;
    if (flag != nullptr) {
        flag->store(true, std::memory_order_release);
    }
}

bool ctrlc_pending() noexcept {
    const std::atomic<bool> *flag = g_flag;
    return flag != nullptr && flag->load(std::memory_order_acquire);
}

void test_reset_ctrlc() noexcept {
    std::atomic<bool> *flag = g_flag;
    if (flag != nullptr) {
        flag->store(false, std::memory_order_release);
    }
}

} // namespace kimix::cli
