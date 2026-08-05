// Test for reproc (subprocess library).
// This test covers:
// - Including reproc C and C++ headers
// - Creating and destroying a reproc_t object
// - Checking reproc error constants
// - Basic C++ process API compilation checks

#include "ut/ut.hpp"

// reproc C API
#include <reproc/reproc.h>
#include <reproc/drain.h>

// reproc C++ API
#include <reproc++/reproc.hpp>

#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "reproc_c_api"_test = [] {
        expect(neq(REPROC_EINVAL, 0));
        expect(neq(REPROC_ETIMEDOUT, 0));
        expect(neq(REPROC_EPIPE, 0));
        expect(neq(REPROC_ENOMEM, 0));
        expect(neq(REPROC_EWOULDBLOCK, 0));

        expect(neq(REPROC_SIGKILL, 0));
        expect(neq(REPROC_SIGTERM, 0));

        expect(lt(REPROC_INFINITE, 0));
        expect(lt(REPROC_DEADLINE, 0));

        reproc_t *proc = reproc_new();
        expect(proc != nullptr) << "reproc_new() should return a valid pointer";

        int stop_noop = REPROC_STOP_NOOP;
        int stop_wait = REPROC_STOP_WAIT;
        expect(neq(stop_noop, stop_wait));

        int redirect_pipe = REPROC_REDIRECT_PIPE;
        int redirect_discard = REPROC_REDIRECT_DISCARD;
        expect(neq(redirect_pipe, redirect_discard));

        int stream_in = REPROC_STREAM_IN;
        int stream_out = REPROC_STREAM_OUT;
        int stream_err = REPROC_STREAM_ERR;
        expect(eq(stream_in, 0));
        expect(eq(stream_out, 1));
        expect(eq(stream_err, 2));

        reproc_destroy(proc);
    };

    "reproc_cpp_api"_test = [] {
        reproc::milliseconds default_timeout{100};
        expect(eq(default_timeout.count(), 100));

        reproc::stop_action sa{reproc::stop::wait, reproc::milliseconds{100}};
        expect(eq(sa.timeout.count(), 100));

        static_assert(reproc::redirect::pipe == 1, "pipe should be 1");
        static_assert(reproc::redirect::discard == 3, "discard should be 3");

        reproc::options options{};
        options.redirect.err.type = reproc::redirect::discard;

        reproc::process process;
        auto [pid_val, pid_err] = process.pid();
        (void)pid_val;
        expect(pid_err != std::error_code{}) << "pid() on non-started process should error";
    };

    "reproc_drain_api"_test = [] {
        reproc_t *proc = reproc_new();
        expect(proc != nullptr);

        reproc_sink sink{};
        (void)sink;
        expect(true) << "reproc_sink type is accessible";

        (void)reproc_drain;
        expect(true) << "reproc_drain function is accessible";

        reproc_destroy(proc);
    };
}
