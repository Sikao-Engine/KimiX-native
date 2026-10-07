// test_http_client.cpp - Unit tests for the hand-written kimix::net HTTP(S)
// client (llm/http_client.h). Covers the response-framing behaviors the LLM
// providers and the fetch/web_search tools rely on: Content-Length and
// close-delimited bodies, chunked Transfer-Encoding decoding, redirect
// following, POST request shape (default headers, content type), receiver
// cancel, and the connect-refused error mapping. Everything runs against the
// loopback one_shot_server helper - no external network.

#include "ut/ut.hpp"

#include "one_shot_server.h"

#include <llm/http_client.h>

#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

// Extracts "GET /target HTTP/1.1" -> "/target".
std::string request_target(const std::string &req) {
    const size_t first = req.find(' ');
    const size_t last = req.find(' ', first + 1);
    if (first == std::string::npos || last == std::string::npos) {
        return {};
    }
    return req.substr(first + 1, last - first - 1);
}

std::string request_header(const std::string &req, const std::string &name) {
    const size_t head_end = req.find("\r\n\r\n");
    const std::string head = req.substr(0, head_end);
    std::string lower = head;
    for (char &c : lower) {
        c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    const std::string needle = "\n" + name + ":";
    const size_t pos = lower.find(needle);
    if (pos == std::string::npos) {
        return {};
    }
    const size_t begin = pos + needle.size();
    const size_t end = head.find('\r', begin);
    std::string value = head.substr(begin, end - begin);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.erase(value.begin());
    }
    return value;
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "get_with_content_length_body"_test = [] {
        kimix::test::one_shot_server svr([](const std::string &) {
            return kimix::test::one_shot_server::respond(200, "text/plain",
                                                         "hello world");
        });
        expect(svr.ok());
        kimix::net::Client cli(svr.url());
        expect(cli.is_valid());
        const auto res = cli.Get("/a?b=c");
        expect(static_cast<bool>(res));
        if (res) {
            expect(eq(res->status, 200));
            expect(res->body == "hello world");
            expect(res->get_header_value("content-type") == "text/plain");
            expect(res->get_header_value("Content-Type") == "text/plain");
        }
    };

    "post_sends_default_headers_and_content_type"_test = [] {
        std::string seen_content_type;
        std::string seen_ua;
        std::string seen_target;
        std::string seen_method;
        kimix::test::one_shot_server svr([&](const std::string &req) {
            seen_content_type = request_header(req, "content-type");
            seen_ua = request_header(req, "user-agent");
            seen_target = request_target(req);
            seen_method = req.substr(0, req.find(' '));
            return kimix::test::one_shot_server::respond(200, "application/json",
                                                         "{\"ok\":true}");
        });
        kimix::net::Client cli(svr.url());
        cli.set_default_headers(
            kimix::net::Headers{{"User-Agent", "kimix-test/1.0"}});
        const auto res = cli.Post("/submit?x=1", std::string("{\"a\":1}"),
                                  "application/json");
        expect(static_cast<bool>(res));
        expect(seen_method == "POST");
        expect(seen_target == "/submit?x=1");
        expect(seen_content_type == "application/json");
        expect(seen_ua == "kimix-test/1.0");
        if (res) {
            expect(res->body == "{\"ok\":true}");
        }
    };

    "chunked_transfer_encoding_is_decoded"_test = [] {
        kimix::test::one_shot_server svr([](const std::string &) {
            // "hello " + "stream" framed as two chunks.
            return std::string(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Transfer-Encoding: chunked\r\n"
                "Connection: close\r\n"
                "\r\n"
                "6\r\nhello \r\n"
                "6\r\nstream\r\n"
                "0\r\n"
                "\r\n");
        });
        kimix::net::Client cli(svr.url());
        const auto res = cli.Get("/events");
        expect(static_cast<bool>(res));
        if (res) {
            expect(eq(res->status, 200));
            expect(res->body == "hello stream") << res->body;
        }
    };

    "close_delimited_body_reads_to_eof"_test = [] {
        kimix::test::one_shot_server svr([](const std::string &) {
            // No Content-Length, no chunked: the body ends at connection close.
            return std::string("HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/plain\r\n"
                               "Connection: close\r\n"
                               "\r\n"
                               "eof-terminated body");
        });
        kimix::net::Client cli(svr.url());
        const auto res = cli.Get("/");
        expect(static_cast<bool>(res));
        if (res) {
            expect(res->body == "eof-terminated body") << res->body;
        }
    };

    "follow_location_resolves_relative_redirect"_test = [] {
        int redirects = 0;
        kimix::test::one_shot_server svr([&](const std::string &req) {
            if (request_target(req) == "/start") {
                ++redirects;
                return std::string("HTTP/1.1 302 Found\r\n"
                                   "Location: /target\r\n"
                                   "Content-Length: 0\r\n"
                                   "Connection: close\r\n\r\n");
            }
            return kimix::test::one_shot_server::respond(200, "text/plain",
                                                         "arrived");
        });
        kimix::net::Client cli(svr.url());
        cli.set_follow_location(true);
        const auto res = cli.Get("/start");
        expect(static_cast<bool>(res));
        expect(eq(redirects, 1));
        if (res) {
            expect(eq(res->status, 200));
            expect(res->body == "arrived") << res->body;
        }
    };

    "receiver_returning_false_cancels_the_request"_test = [] {
        kimix::test::one_shot_server svr([](const std::string &) {
            // A larger-than-socket-buffer body guarantees several receiver
            // callbacks even if the peer coalesces the read.
            return kimix::test::one_shot_server::respond(200, "application/octet-stream",
                                                         std::string(64 * 1024, 'x'));
        });
        kimix::net::Client cli(svr.url());
        size_t received = 0;
        auto res = cli.Post(
            "/upload", kimix::net::Headers{}, std::string("data"),
            "application/octet-stream",
            [&](const char *, size_t len) {
                received += len;
                return false; // cancel on the first chunk
            });
        expect(!static_cast<bool>(res));
        expect(res.error() == kimix::net::Error::canceled);
        expect(received > 0u);
    };

    "http_proxy_sends_absolute_form_request_target"_test = [] {
        std::string seen_target;
        std::string seen_host;
        kimix::test::one_shot_server svr([&](const std::string &req) {
            seen_target = req.substr(0, req.find("\r\n"));
            seen_host = request_header(req, "host");
            return kimix::test::one_shot_server::respond(200, "text/plain",
                                                         "via-proxy");
        });
        // The proxy itself is the loopback server; the "origin" is a fake
        // host that only exists in the absolute-URI request line.
        kimix::net::Client cli("http://origin.example:8080");
        expect(cli.is_valid());
        cli.set_proxy("127.0.0.1", svr.port());
        const auto res = cli.Get("/page?q=1");
        expect(static_cast<bool>(res));
        expect(seen_target ==
               "GET http://origin.example:8080/page?q=1 HTTP/1.1")
            << seen_target;
        expect(seen_host == "origin.example:8080") << seen_host;
        if (res) {
            expect(res->body == "via-proxy");
        }
    };

    "https_proxy_connect_failure_is_a_connection_error"_test = [] {
        std::string seen_method;
        kimix::test::one_shot_server svr([&](const std::string &req) {
            seen_method = req.substr(0, req.find(' '));
            return std::string("HTTP/1.1 502 Bad Gateway\r\n"
                               "Content-Length: 0\r\n"
                               "Connection: close\r\n\r\n");
        });
        kimix::net::Client cli("https://origin.example:443");
        expect(cli.is_valid());
        cli.set_proxy("127.0.0.1", svr.port());
        // TLS never starts over the failed tunnel, so no cert policy runs.
        cli.enable_server_certificate_verification(false);
        const auto res = cli.Get("/");
        expect(seen_method == "CONNECT") << seen_method;
        expect(!static_cast<bool>(res));
        expect(res.error() == kimix::net::Error::connection);
    };

    "connect_to_closed_port_is_a_connection_error"_test = [] {
        // Grab a free loopback port by binding then closing a probe socket.
#ifdef _WIN32
        const SOCKET probe = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        expect(probe != INVALID_SOCKET);
#else
        const int probe = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        expect(probe >= 0);
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        expect(::bind(probe, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
        socklen_t n = sizeof(addr);
        expect(::getsockname(probe, reinterpret_cast<sockaddr *>(&addr), &n) == 0);
        const int port = static_cast<int>(::ntohs(addr.sin_port));
#ifdef _WIN32
        ::closesocket(probe);
#else
        ::close(probe);
#endif
        kimix::net::Client cli("http://127.0.0.1:" + std::to_string(port));
        expect(cli.is_valid());
        // Bound the probe: hosts that answer closed ports with an RST report
        // Error::connection instantly, firewalled/sandboxed hosts that drop
        // the SYN report Error::connection_timeout after the deadline.
        cli.set_connection_timeout(3);
        const auto res = cli.Get("/");
        expect(!static_cast<bool>(res));
        expect(res.error() == kimix::net::Error::connection ||
               res.error() == kimix::net::Error::connection_timeout);
    };
}
