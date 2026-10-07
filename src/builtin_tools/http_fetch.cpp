// http_fetch.cpp - see http_fetch.h.

#include "builtin_tools/http_fetch.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "builtin_tools/fetch_url_tool.h" // url_safety kernels (owned there)
#include "llm/http_client.h" // kimix::net HTTP(S) client (owns Winsock init)

namespace kimix::builtin_tools::http_fetch {

namespace {

// Minimal ASCII URL split: scheme, host (IDNA-ASCII applied), port, path.
struct url_parts {
    kimix::string scheme;
    kimix::string host;
    kimix::string port; // "" == default for the scheme
    kimix::string path; // "/" when absent
    kimix::string query; // without '?'
    bool valid = false;
};

kimix::string ascii_lower(kimix::string_view s) {
    kimix::string out;
    out.reserve(s.size());
    for (const char c : s) {
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

url_parts split_url(kimix::string_view url) {
    url_parts p;
    const size_t scheme_end = url.find("://");
    if (scheme_end == kimix::string_view::npos || scheme_end == 0) {
        return p;
    }
    p.scheme = ascii_lower(url.substr(0, scheme_end));
    if (p.scheme != "http" && p.scheme != "https") {
        return p;
    }
    kimix::string_view rest = url.substr(scheme_end + 3);
    // Fragment is never sent.
    const size_t frag = rest.find('#');
    if (frag != kimix::string_view::npos) {
        rest = rest.substr(0, frag);
    }
    kimix::string_view authority = rest;
    kimix::string_view tail;
    const size_t slash = rest.find('/');
    if (slash != kimix::string_view::npos) {
        authority = rest.substr(0, slash);
        tail = rest.substr(slash);
    }
    // Strip userinfo (never forwarded).
    const size_t at = authority.rfind('@');
    if (at != kimix::string_view::npos) {
        authority = authority.substr(at + 1);
    }
    if (authority.empty()) {
        return p;
    }
    if (!authority.empty() && authority.front() == '[') {
        // IPv6 literal: [::1] or [::1]:8080.
        const size_t close = authority.find(']');
        if (close == kimix::string_view::npos) {
            return p;
        }
        p.host = kimix::string(authority.substr(1, close - 1));
        if (close + 1 < authority.size() && authority[close + 1] == ':') {
            p.port = kimix::string(authority.substr(close + 2));
        }
    } else {
        const size_t colon = authority.rfind(':');
        if (colon != kimix::string_view::npos) {
            p.host = kimix::string(authority.substr(0, colon));
            p.port = kimix::string(authority.substr(colon + 1));
        } else {
            p.host = kimix::string(authority);
        }
    }
    if (p.host.empty()) {
        return p;
    }
    // IDNA-ASCII the host (same normalization the request URL uses).
    kimix::string ascii_host;
    if (kimix::builtin_tools::fetch_url::idna_encode_host(p.host, ascii_host)) {
        p.host = std::move(ascii_host);
    }
    const size_t qmark = tail.find('?');
    if (qmark == kimix::string_view::npos) {
        p.path = tail.empty() ? kimix::string("/") : kimix::string(tail);
    } else {
        p.path = kimix::string(tail.substr(0, qmark));
        if (p.path.empty()) {
            p.path = "/";
        }
        p.query = kimix::string(tail.substr(qmark + 1));
    }
    p.valid = true;
    return p;
}

kimix::string host_header_of(const url_parts &parts) {
    if (parts.port.empty() ||
        (parts.scheme == "http" && parts.port == "80") ||
        (parts.scheme == "https" && parts.port == "443")) {
        return parts.host;
    }
    return parts.host + ":" + parts.port;
}

} // namespace

namespace {

// The HTTP(S)_PROXY environment setting ("http://host:port" or "host:port").
// A configured proxy delegates the private-address decision (url_safety's
// proxy escape) and is the only way through fake-IP DNS resolvers.
kimix::string proxy_env_for(bool https_target) {
    const char *names[] = {https_target ? "HTTPS_PROXY" : "HTTP_PROXY",
                           https_target ? "https_proxy" : "http_proxy"};
    for (const char *name : names) {
        const char *v = std::getenv(name);
        if (v != nullptr && kimix::string(v).empty() == false) {
            return kimix::string(v);
        }
    }
    return {};
}

void split_proxy(kimix::string_view proxy, kimix::string &host, int &port) {
    kimix::string_view rest = proxy;
    if (const size_t scheme = rest.find("://"); scheme != kimix::string_view::npos) {
        rest = rest.substr(scheme + 3);
    }
    if (const size_t slash = rest.find('/'); slash != kimix::string_view::npos) {
        rest = rest.substr(0, slash);
    }
    const size_t colon = rest.rfind(':');
    if (colon != kimix::string_view::npos) {
        host.assign(rest.substr(0, colon));
        port = std::atoi(kimix::string(rest.substr(colon + 1)).c_str());
    } else {
        host.assign(rest);
        port = 80;
    }
}

} // namespace

kimix::string urlencode_component(kimix::string_view value) {
    static const char *kHex = "0123456789ABCDEF";
    kimix::string out;
    out.reserve(value.size());
    for (const char c : value) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool unreserved = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
                                (u >= '0' && u <= '9') || u == '-' || u == '_' ||
                                u == '.' || u == '~';
        if (unreserved) {
            out.push_back(c);
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(kHex[u >> 4]);
            out.push_back(kHex[u & 0xF]);
        }
    }
    return out;
}

fetch_result get(kimix::string_view url, int timeout_ms, size_t max_body_bytes) {
    using namespace kimix::builtin_tools::fetch_url;
    // Winsock up before the gate's own getaddrinfo (no-op off Windows).
    kimix::net::ensure_network_initialized();
    fetch_result out;
    out.final_url = kimix::string(url);

    const url_parts parts = split_url(url);
    if (!parts.valid) {
        out.error = "invalid or unsupported URL (http/https only): " + out.final_url;
        out.expected_refusal = true;
        return out;
    }
    if (is_blocked_hostname(parts.host)) {
        out.error = "blocked hostname: " + parts.host;
        out.expected_refusal = true;
        return out;
    }

    // DNS resolution + classification (url_safety.is_safe_url's pure decision
    // with the resolution performed here).
    resolve_outcome resolved;
    {
        struct addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *info = nullptr;
        const int rc = ::getaddrinfo(parts.host.c_str(),
                                     parts.port.empty() ? nullptr
                                                        : parts.port.c_str(),
                                     &hints, &info);
        if (rc != 0 || info == nullptr) {
            resolved.dns_failed = true;
        } else {
            for (struct addrinfo *it = info; it != nullptr; it = it->ai_next) {
                char addr[INET6_ADDRSTRLEN] = {0};
                if (it->ai_family == AF_INET) {
                    const auto *sa = reinterpret_cast<struct sockaddr_in *>(it->ai_addr);
                    ::inet_ntop(AF_INET, &sa->sin_addr, addr, sizeof(addr));
                } else if (it->ai_family == AF_INET6) {
                    const auto *sa = reinterpret_cast<struct sockaddr_in6 *>(it->ai_addr);
                    ::inet_ntop(AF_INET6, &sa->sin6_addr, addr, sizeof(addr));
                } else {
                    continue;
                }
                resolved.addresses.emplace_back(addr);
            }
            ::freeaddrinfo(info);
        }
    }
    const char *allow_private_env = std::getenv("KIMI_ALLOW_PRIVATE_URLS");
    const bool allow_all_private =
        allow_private_env != nullptr && kimix::string(allow_private_env) == "true";
    const kimix::string proxy_env = proxy_env_for(parts.scheme == "https");
    // Fake-IP DNS accommodation: benchmark-range resolvers (198.18.0.0/15,
    // Clash/Mihomo-style TUN proxies) answer EVERY name with a synthetic
    // address and route the actual connection by hostname/SNI. When ALL
    // resolved addresses sit in that range the resolution carries no private
    // network information, so the gate delegates the address decision
    // (proxy_configured=true) and the transport pins the fake IP via
    // set_hostname_addr_map - the same trust model as an HTTP(S)_PROXY.
    bool fake_ip_dns = false;
    if (!resolved.addresses.empty()) {
        fake_ip_dns = true;
        for (const kimix::string &ip : resolved.addresses) {
            if (!(ip.rfind("198.18.", 0) == 0 || ip.rfind("198.19.", 0) == 0)) {
                fake_ip_dns = false;
                break;
            }
        }
    }
    // Through the gate, a fake-IP resolution carries no usable address
    // information - presented exactly like the reference's proxy escape
    // (dns_failed + a configured proxy): scheme/host/blocked-hostname checks
    // still run, the uninformative addresses delegate to the TUN proxy.
    resolve_outcome gate_outcome = resolved;
    if (fake_ip_dns) {
        gate_outcome.dns_failed = true;
        gate_outcome.addresses.clear();
    }
    const bool delegate_address_check = !proxy_env.empty() || fake_ip_dns;
    if (!is_safe_url_decision(out.final_url, allow_all_private,
                              /*proxy_configured=*/delegate_address_check,
                              gate_outcome)) {
        out.error = "URL refused by the SSRF safety gate (non-public address, "
                    "blocked host, or unresolvable host): " + out.final_url +
                    " (a configured HTTP(S)_PROXY delegates this check)";
        out.expected_refusal = true;
        return out;
    }

    // Transport: the kimix::net client (raw sockets + mbedTLS, same stack as
    // the LLM clients).
    const std::string default_port = (parts.scheme == "https") ? "443" : "80";
    const std::string scheme_host_port =
        std::string(parts.scheme) + "://" + std::string(parts.host) + ":" +
        (parts.port.empty() ? default_port : std::string(parts.port));
    kimix::net::Client cli(scheme_host_port);
    if (!cli.is_valid()) {
        out.error = "cannot create HTTP client for " + out.final_url;
        return out;
    }
    if (parts.scheme == "https") {
        cli.use_windows_certificate_verifier(std::string(parts.host));
    }
    if (!proxy_env.empty()) {
        kimix::string proxy_host;
        int proxy_port = 0;
        split_proxy(proxy_env, proxy_host, proxy_port);
        cli.set_proxy(std::string(proxy_host), proxy_port);
    } else if (fake_ip_dns && !resolved.addresses.empty()) {
        // Connect to the fake IP, keep the hostname for the Host header / SNI.
        std::map<std::string, std::string> addr_map;
        addr_map.emplace(std::string(parts.host),
                         std::string(resolved.addresses.front()));
        cli.set_hostname_addr_map(std::move(addr_map));
    }
    cli.set_follow_location(true);
    const std::chrono::milliseconds timeout(timeout_ms);
    cli.set_connection_timeout(timeout);
    cli.set_read_timeout(timeout);
    cli.set_write_timeout(timeout);
    kimix::net::Headers headers;
    headers.emplace("User-Agent",
                    "Mozilla/5.0 (compatible; kimix-native/1.0; +https://kimix.dev)");
    headers.emplace("Accept", "text/html,application/xhtml+xml,*/*;q=0.8");

    kimix::string path_with_query = parts.path;
    if (!parts.query.empty()) {
        path_with_query += "?" + parts.query;
    }
    auto res = cli.Get(path_with_query.c_str(), headers);
    if (!res) {
        out.error = "request failed (connection error or timeout): " + out.final_url;
        // An expected refusal when the address was never verified: a plain
        // DNS failure (the report's DNS-failure case), or a fake-IP DNS
        // resolution whose proxy could not reach the real host (the
        // benchmark-range answer carries no address information, so the
        // connect failure means the name does not exist upstream). A
        // failure against a REAL resolved address stays the runtime error.
        out.expected_refusal = resolved.dns_failed || fake_ip_dns;
        return out;
    }
    out.status = res->status;
    if (res->status < 200 || res->status >= 300) {
        out.error = "HTTP " + kimix::format("{}", res->status) + " for " +
                    out.final_url;
        return out;
    }
    out.body.assign(res->body.data(),
                    res->body.size() > max_body_bytes ? max_body_bytes
                                                      : res->body.size());
    out.ok = !out.body.empty() || res->status == 204;
    if (!out.ok) {
        out.error = "empty response body for " + out.final_url;
    }
    return out;
}

} // namespace kimix::builtin_tools::http_fetch
