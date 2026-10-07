// test_mbedtls.cpp - Compile/link smoke test for the kimix-mbedtls TLS stack
// behind the hand-written kimix::net HTTP(S) client. Validates that the
// mbedTLS-backed client (llm/http_client.cpp, linked through kimix-llm)
// compiles, links, and that a client endpoint can be created. No network
// connection is attempted.

#include "ut/ut.hpp"

#include <llm/http_client.h>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "mbedtls_ssl_client_context"_test = [] {
        // Constructing the HTTPS client validates the mbedTLS backend
        // compiles, links, and the kimix-mbedtls target resolves; is_valid()
        // checks the endpoint parsed successfully (no connection attempted).
        kimix::net::Client cli("https://localhost");
        expect(cli.is_valid());
    };
}
