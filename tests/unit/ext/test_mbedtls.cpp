// test_mbedtls.cpp - Compile/link smoke test for the kimix-mbedtls TLS backend.
// Validates that cpp-httplib's MbedTLS backend (CPPHTTPLIB_MBEDTLS_SUPPORT)
// compiles, links, and that a client TLS context can be created. No network
// connection is attempted.

#include "ut/ut.hpp"

#include <httplib.h>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "mbedtls_ssl_client_context"_test = [] {
        // Constructing an SSLClient validates the MbedTLS backend compiles,
        // links, and the kimix-mbedtls target resolves; is_valid() checks the
        // client context was created successfully (no connection attempted).
        httplib::SSLClient cli("localhost");
        expect(cli.is_valid());
    };
}
