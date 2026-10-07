// one_shot_server.h - Minimal loopback HTTP/1.1 server for kimix::net tests.
//
// Replaces the old cpp-httplib-based test server: raw sockets, binds
// 127.0.0.1 on an ephemeral port, serves connections sequentially on a
// background thread, and answers every request with the bytes its handler
// builds. The handler receives the raw request (request line + headers +
// body) so tests can assert on the wire format and script multi-request
// flows (redirects, per-path behavior).
//
// Every helper is inline (no .cpp) so the unity-batched test TUs that include
// this header do not collide.

#pragma once

#include <core/kimix_core.h> // winsock2 first on Windows (winsock2-before-windows.h)

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <utility>

#ifndef _WIN32
    #include <cerrno>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

namespace kimix::test {

class one_shot_server {
public:
    // Receives one full request, returns the raw response bytes.
    using handler_fn = std::function<std::string(const std::string &request)>;

    explicit one_shot_server(handler_fn handler) : _handler(std::move(handler)) {
        start();
    }
    ~one_shot_server() { stop(); }
    one_shot_server(const one_shot_server &) = delete;
    one_shot_server &operator=(const one_shot_server &) = delete;

    bool ok() const { return _ok; }
    int port() const { return _port; }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(_port); }

    // A complete "HTTP/1.1 <status> <reason>" response with Content-Length and
    // Connection: close.
    static std::string respond(int status, const char *content_type,
                               const std::string &body) {
        const char *reason = "";
        switch (status) {
        case 200: reason = "OK"; break;
        case 204: reason = "No Content"; break;
        case 301: reason = "Moved Permanently"; break;
        case 302: reason = "Found"; break;
        case 307: reason = "Temporary Redirect"; break;
        case 400: reason = "Bad Request"; break;
        case 404: reason = "Not Found"; break;
        case 500: reason = "Internal Server Error"; break;
        default: break;
        }
        std::string out = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
        if (content_type != nullptr) {
            out += "Content-Type: ";
            out += content_type;
            out += "\r\n";
        }
        out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        out += "Connection: close\r\n\r\n";
        out += body;
        return out;
    }

private:
#ifdef _WIN32
    using socket_t = SOCKET;
    static constexpr socket_t k_invalid = INVALID_SOCKET;
    static void close_fd(socket_t s) { ::closesocket(s); }
#else
    using socket_t = int;
    static constexpr socket_t k_invalid = -1;
    static void close_fd(socket_t s) { ::close(s); }
#endif

    void start() {
#ifdef _WIN32
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return;
        }
        _wsa_started = true;
#endif
        _listen = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (_listen == k_invalid) {
            return;
        }
        int one = 1;
        ::setsockopt(_listen, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char *>(&one), sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // ephemeral
        if (::bind(_listen, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            return;
        }
        socklen_t n = sizeof(addr);
        if (::getsockname(_listen, reinterpret_cast<sockaddr *>(&addr), &n) != 0) {
            return;
        }
        _port = static_cast<int>(::ntohs(addr.sin_port));
        if (::listen(_listen, 8) != 0) {
            return;
        }
        _thread = std::thread([this] { loop(); });
        _ok = true;
    }

    void stop() {
        _stop.store(true);
        if (_listen != k_invalid) {
            // Wake the blocking accept() before closing the socket.
#ifdef _WIN32
            ::shutdown(_listen, SD_BOTH);
#else
            ::shutdown(_listen, SHUT_RDWR);
#endif
            close_fd(_listen);
            _listen = k_invalid;
        }
        if (_thread.joinable()) {
            _thread.join();
        }
#ifdef _WIN32
        if (_wsa_started) {
            ::WSACleanup();
            _wsa_started = false;
        }
#endif
    }

    // Sequential accept loop; each connection is fully served before the
    // next is accepted (the kimix::net client follows redirects on fresh
    // connections, one at a time).
    void loop() {
        while (!_stop.load()) {
            sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            const socket_t c = ::accept(_listen, reinterpret_cast<sockaddr *>(&peer),
                                        &plen);
            if (c == k_invalid) {
                if (_stop.load()) {
                    break;
                }
#ifdef _WIN32
                if (::WSAGetLastError() == WSAEINTR) {
                    continue;
                }
#else
                if (errno == EINTR) {
                    continue;
                }
#endif
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            serve(c);
            close_fd(c);
        }
    }

    // Read one full request (headers + Content-Length body), hand it to the
    // handler, send the response bytes back.
    void serve(socket_t c) {
        std::string req;
        char buf[4096];
        size_t header_end = std::string::npos;
        while (header_end == std::string::npos) {
            const int n = static_cast<int>(
                ::recv(c, buf, sizeof(buf), 0));
            if (n <= 0) {
                return;
            }
            req.append(buf, static_cast<size_t>(n));
            header_end = req.find("\r\n\r\n");
            if (req.size() > 65536) {
                return;
            }
        }
        // Drain the declared body so the handler sees the full request.
        size_t content_length = 0;
        const std::string head = req.substr(0, header_end);
        const std::string needle = "content-length:";
        std::string lower_head = head;
        for (char &ch : lower_head) {
            ch = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
        }
        const size_t pos = lower_head.find(needle);
        if (pos != std::string::npos) {
            const size_t line_end = lower_head.find('\n', pos);
            const std::string value =
                head.substr(pos + needle.size(),
                            line_end == std::string::npos
                                ? std::string::npos
                                : line_end - pos - needle.size());
            content_length = static_cast<size_t>(std::strtoul(value.c_str(), nullptr, 10));
        }
        size_t have = req.size() - (header_end + 4);
        while (have < content_length) {
            const int n = static_cast<int>(::recv(c, buf, sizeof(buf), 0));
            if (n <= 0) {
                return;
            }
            req.append(buf, static_cast<size_t>(n));
            have += static_cast<size_t>(n);
        }
        const std::string response = _handler(req);
        size_t sent = 0;
        while (sent < response.size()) {
#ifdef _WIN32
            const int n = ::send(c, response.data() + sent,
                                 static_cast<int>(response.size() - sent), 0);
#else
            const ssize_t n =
                ::send(c, response.data() + sent, response.size() - sent, 0);
#endif
            if (n <= 0) {
                return;
            }
            sent += static_cast<size_t>(n);
        }
    }

    handler_fn _handler;
    socket_t _listen = k_invalid;
    int _port = 0;
    bool _ok = false;
    std::atomic<bool> _stop{false};
    std::thread _thread;
#ifdef _WIN32
    bool _wsa_started = false;
#endif
};

} // namespace kimix::test
