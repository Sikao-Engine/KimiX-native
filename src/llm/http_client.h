// http_client.h - Minimal hand-written HTTP(S) client for kimix-llm.
//
// Replaces the vendored cpp-httplib with exactly the API surface the project
// uses (client only: Get/Post, ordered case-insensitive headers, a streaming
// ContentReceiver, timeouts, redirects, an HTTP(S) proxy and a pinned-IP
// hostname map), on top of raw sockets + the vendored mbedTLS (kimix-mbedtls):
//   * HTTP/1.1, one request per connection ("Connection: close"),
//   * Content-Length / chunked / close-delimited response framing,
//   * interim 1xx status lines skipped defensively,
//   * TLS 1.2+ via mbedTLS, VERIFY_REQUIRED against the system CA bundle
//     (non-Windows) or the Windows full-chain session verifier policy
//     (use_windows_certificate_verifier, Windows only - see http_client.cpp),
//   * http proxy = absolute-URI request form, https proxy = CONNECT tunnel,
//   * redirect following for GET (301/302/303 -> GET, 307/308 -> re-POST),
//   * exception-free (kimix-llm builds with /EHs-c- / -fno-exceptions).
//
// The transport types intentionally use std::string for wire-level bodies and
// header values: the call sites (provider transports, fetch/web_search tools)
// already operate on std::string, and this header is a private implementation
// detail of the kimix-llm target, not a kimix-facing API.
//
// This header includes <core/kimix_core.h> first so winsock2.h is set up
// before any windows.h toucher in the kimix-llm unity batches.

#pragma once

#include <core/kimix_core.h> // winsock2/ws2tcpip first on Windows

#ifndef _WIN32
    #include <arpa/inet.h> // inet_ntop (e.g. the fetch_url gate's own getaddrinfo)
    #include <netdb.h>     // getaddrinfo/freeaddrinfo/addrinfo
#endif

#include <chrono>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace kimix::net {

// Transport error category (mirrors the httplib::Error subset the project
// maps onto llm::TransportErrorKind).
enum class Error {
    success = 0,
    connection,         // connect() failed (DNS, refused, unreachable)
    connection_timeout, // connect() deadline exceeded
    timeout,            // read inactivity deadline exceeded ("Read timeout")
    read,               // socket read failure
    write,              // socket write failure / write deadline
    canceled,           // the ContentReceiver returned false (abort)
    tls,                // TLS handshake / record-layer failure
    certificate_rejected, // post-handshake policy rejection (Windows verifier)
    invalid_url,        // malformed request target
};

const char *to_string(Error err);

// Ordered header multimap with case-insensitive lookup and insertion-ordered
// iteration (same contract as httplib::Headers).
class Headers {
public:
    using value_type = std::pair<std::string, std::string>;

    Headers() = default;
    Headers(std::initializer_list<value_type> init) : _entries(init) {}

    void emplace(std::string key, std::string value) {
        _entries.emplace_back(std::move(key), std::move(value));
    }
    void set(std::string key, std::string value); // replaces all same-name entries
    std::string get(std::string_view name) const; // first match, "" when absent
    bool empty() const { return _entries.empty(); }
    size_t size() const { return _entries.size(); }

    std::vector<value_type>::const_iterator begin() const { return _entries.begin(); }
    std::vector<value_type>::const_iterator end() const { return _entries.end(); }

private:
    std::vector<value_type> _entries;
};

// One received HTTP response.
struct Response {
    int status = 0;
    Headers headers;
    std::string body;
    std::string get_header_value(std::string_view name) const {
        return headers.get(name);
    }
};

// Value-or-error handle, mimics httplib::Result: operator bool is true when
// a response (any status) was received; otherwise error() carries the cause.
class Result {
public:
    Result(Error err) : _err(err) {} // NOLINT(google-explicit-constructor)
    explicit Result(Response &&res)
        : _res(new Response(std::move(res))), _err(Error::success) {}

    Result(Result &&) noexcept = default;
    Result &operator=(Result &&) noexcept = default;
    Result(const Result &) = delete;
    Result &operator=(const Result &) = delete;

    explicit operator bool() const { return _res != nullptr; }
    Error error() const { return _err; }
    const Response *operator->() const { return _res.get(); }
    const Response &operator*() const { return *_res; }

private:
    kimix::unique_ptr<Response> _res;
    Error _err = Error::success;
};

// Streaming body sink. Returning false cancels the request: the socket is
// closed and Post returns an error Result (Error::canceled).
using ContentReceiver = kimix::function<bool(const char *data, size_t len)>;

// Starts the platform network runtime (Winsock on Windows, SIGPIPE policy on
// POSIX) before any getaddrinfo outside a Client request path - e.g. the
// fetch_url safety gate resolves the host itself. Idempotent, thread-safe.
void ensure_network_initialized();

class Client {
public:
    // "http://host[:port]" or "https://host[:port]".
    explicit Client(std::string scheme_host_port);
    bool is_valid() const { return _valid; }

    void set_connection_timeout(std::chrono::milliseconds ms);
    void set_connection_timeout(int sec);
    void set_read_timeout(std::chrono::milliseconds ms);
    void set_read_timeout(int sec, int usec);
    void set_write_timeout(std::chrono::milliseconds ms);
    void set_write_timeout(int sec, int usec);

    void set_default_headers(Headers headers) { _default_headers = std::move(headers); }
    void set_proxy(const std::string &host, int port);
    void set_hostname_addr_map(std::map<std::string, std::string> map) {
        _addr_map = std::move(map);
    }
    void set_follow_location(bool on) { _follow_location = on; }

    // mbedTLS chain verification against the system CA bundle (default on).
    void enable_server_certificate_verification(bool on) { _verify = on; }

    // Windows-only full-chain policy (ported from the old llm/http_tls.h):
    // handshake runs VERIFY_NONE, then the received peer chain is verified
    // with CertGetCertificateChain plus a SAN hostname match. No-op elsewhere.
    void use_windows_certificate_verifier(std::string host);

    Result Get(const std::string &path, const Headers &headers = {});
    Result Post(const std::string &path, const Headers &headers,
                const std::string &body, const char *content_type,
                ContentReceiver receiver = {});
    Result Post(const std::string &path, const std::string &body,
                const char *content_type);

private:
    // One request over a fresh connection (DNS + connect + optional proxy
    // tunnel / TLS + request + response read). `receiver` streams the body
    // when set.
    Result request(const std::string &method, const std::string &scheme,
                   const std::string &host, int port, const std::string &path,
                   const Headers &headers, const std::string &body,
                   const char *content_type, const ContentReceiver *receiver);
    std::string _scheme;
    std::string _host;
    int _port = 0;
    bool _valid = false;

    long _connect_timeout_ms = 300000; // httplib defaults
    long _read_timeout_ms = 300000;
    long _write_timeout_ms = 300000;

    Headers _default_headers;
    std::string _proxy_host;
    int _proxy_port = 0;
    bool _has_proxy = false;
    std::map<std::string, std::string> _addr_map;
    bool _follow_location = false;
    bool _verify = true;
    std::string _win_verify_host; // empty == Windows policy not installed
};

} // namespace kimix::net
