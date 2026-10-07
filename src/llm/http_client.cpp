// http_client.cpp - see http_client.h.

#include "llm/http_client.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
    #include <windows.h> // after winsock2.h (kimix_core.h chain)
    #include <wincrypt.h>
#else
    #include <cerrno>
    #include <fcntl.h>
    #include <netdb.h>
    #include <signal.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

namespace kimix::net {

const char *to_string(Error err) {
    // Wording mirrors httplib::to_string() so the provider error texts
    // ("http error: ...") stay recognizable.
    switch (err) {
    case Error::success: return "Success (no error)";
    case Error::connection: return "Could not establish connection";
    case Error::connection_timeout: return "Connection timed out";
    case Error::timeout: return "Read timeout";
    case Error::read: return "Failed to read connection";
    case Error::write: return "Failed to write connection";
    case Error::canceled: return "Connection handling canceled";
    case Error::tls: return "SSL connection failed";
    case Error::certificate_rejected: return "SSL server verification failed";
    case Error::invalid_url: return "Invalid URL";
    }
    return "Unknown";
}

namespace {

bool ci_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const char x = a[i], y = b[i];
        const char lx = (x >= 'A' && x <= 'Z') ? static_cast<char>(x - 'A' + 'a') : x;
        const char ly = (y >= 'A' && y <= 'Z') ? static_cast<char>(y - 'A' + 'a') : y;
        if (lx != ly) {
            return false;
        }
    }
    return true;
}

std::string ascii_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

} // namespace

void Headers::set(std::string key, std::string value) {
    _entries.erase(std::remove_if(_entries.begin(), _entries.end(),
                                  [&](const value_type &e) {
                                      return ci_equal(e.first, key);
                                  }),
                   _entries.end());
    _entries.emplace_back(std::move(key), std::move(value));
}

std::string Headers::get(std::string_view name) const {
    for (const auto &[k, v] : _entries) {
        if (ci_equal(k, name)) {
            return v;
        }
    }
    return {};
}

namespace detail {

// ---------------------------------------------------------------------------
// Platform socket layer
// ---------------------------------------------------------------------------

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline bool would_block(int e) {
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINTR;
}
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
inline bool would_block(int e) {
    return e == EWOULDBLOCK || e == EINPROGRESS || e == EINTR;
}
#endif

inline void close_fd(socket_t s) {
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
}

inline void set_nonblocking(socket_t s, bool nb) {
#ifdef _WIN32
    u_long mode = nb ? 1u : 0u;
    ::ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = ::fcntl(s, F_GETFL, 0);
    if (nb) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    ::fcntl(s, F_SETFL, flags);
#endif
}

// Wait for readability (rd=true) or writability with a deadline.
// Returns false on timeout.
inline bool wait_on(socket_t s, bool rd, long timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout_ms / 1000);
    tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
#ifdef _WIN32
    const int rc = ::select(0, rd ? &set : nullptr, rd ? nullptr : &set, nullptr, &tv);
#else
    const int rc =
        ::select(static_cast<int>(s) + 1, rd ? &set : nullptr, rd ? nullptr : &set,
                 nullptr, &tv);
#endif
    return rc > 0 && FD_ISSET(s, &set);
}

// One-time network runtime init: Winsock startup on Windows (released at
// process exit by the static destructor; WSACleanup is refcount-balanced), and
// SIGPIPE ignored on POSIX (MSG_NOSIGNAL covers the data path; the signal
// ignore is belt and braces for connect()/close() corner cases).
inline void ensure_net() {
#ifdef _WIN32
    struct wsa_lifetime {
        wsa_lifetime() {
            WSADATA data{};
            ::WSAStartup(MAKEWORD(2, 2), &data);
        }
        ~wsa_lifetime() { ::WSACleanup(); }
    };
    static wsa_lifetime lifetime;
    (void)lifetime;
#else
    struct sigpipe_guard {
        sigpipe_guard() { ::signal(SIGPIPE, SIG_IGN); }
    };
    static sigpipe_guard guard;
    (void)guard;
#endif
}

// RAII TCP socket with deadline-bound connect/send/recv.
class Socket {
public:
    Socket() = default;
    ~Socket() { close(); }
    Socket(Socket &&o) noexcept : _s(o._s) { o._s = kInvalidSocket; }
    Socket &operator=(Socket &&o) noexcept {
        close();
        _s = o._s;
        o._s = kInvalidSocket;
        return *this;
    }
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;

    explicit operator bool() const { return _s != kInvalidSocket; }

    // Connect with a deadline. `pinned_ip` bypasses DNS (fake-IP map): the
    // numeric address is dialed while the hostname is kept for Host/SNI.
    bool connect(const std::string &host, const std::string &port,
                 const std::string *pinned_ip, long timeout_ms, Error &err) {
        ensure_net();
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *info = nullptr;
        const char *node = pinned_ip != nullptr ? pinned_ip->c_str() : host.c_str();
        const char *svc = port.empty() ? nullptr : port.c_str();
        if (::getaddrinfo(node, svc, &hints, &info) != 0 || info == nullptr) {
            err = Error::connection;
            return false;
        }
        bool any_timeout = false;
        bool connected = false;
        for (addrinfo *ai = info; ai != nullptr && !connected; ai = ai->ai_next) {
            const socket_t s =
                ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (s == kInvalidSocket) {
                continue;
            }
            set_nonblocking(s, true);
            int rc = ::connect(s, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
#ifdef _WIN32
            const int connect_errno = (rc != 0) ? WSAGetLastError() : 0;
#else
            const int connect_errno = (rc != 0) ? errno : 0;
#endif
            if (rc != 0 && would_block(connect_errno)) {
                if (wait_on(s, false, timeout_ms)) {
                    int soerr = 0;
                    socklen_t n = sizeof(soerr);
                    ::getsockopt(s, SOL_SOCKET, SO_ERROR,
                                 reinterpret_cast<char *>(&soerr), &n);
                    if (soerr == 0) {
                        rc = 0;
                    } else {
#ifdef _WIN32
                        any_timeout |= (soerr == WSAETIMEDOUT);
#else
                        any_timeout |= (soerr == ETIMEDOUT);
#endif
                    }
                } else {
                    any_timeout = true; // select deadline
                }
            } else if (rc != 0) {
#ifdef _WIN32
                any_timeout |= (connect_errno == WSAETIMEDOUT);
#else
                any_timeout |= (connect_errno == ETIMEDOUT);
#endif
            }
            if (rc == 0) {
                set_nonblocking(s, false);
                _s = s;
                connected = true;
            } else {
                close_fd(s);
            }
        }
        ::freeaddrinfo(info);
        if (!connected) {
            err = any_timeout ? Error::connection_timeout : Error::connection;
            return false;
        }
        return true;
    }

    void close() {
        if (_s != kInvalidSocket) {
            close_fd(_s);
            _s = kInvalidSocket;
        }
    }

    // >0 bytes read, 0 on EOF, -1 on timeout, -2 on error.
    long recv_raw(char *buf, size_t cap, long timeout_ms) {
        int spin = 0;
        for (;;) {
            if (!wait_on(_s, true, timeout_ms)) {
                return -1;
            }
#ifdef _WIN32
            const int n = ::recv(_s, buf, static_cast<int>(cap), 0);
            if (n > 0) {
                return n;
            }
            if (n == 0) {
                return 0;
            }
            const int e = WSAGetLastError();
#else
            const ssize_t n = ::recv(_s, buf, cap, 0);
            if (n > 0) {
                return n;
            }
            if (n == 0) {
                return 0;
            }
            const int e = errno;
#endif
            if (would_block(e) && ++spin < 8) {
                continue;
            }
            return -2;
        }
    }

    bool send_raw(const char *data, size_t len, long timeout_ms) {
        size_t sent = 0;
        int spin = 0;
        while (sent < len) {
            if (!wait_on(_s, false, timeout_ms)) {
                return false;
            }
#ifdef _WIN32
            const int n = ::send(_s, data + sent, static_cast<int>(len - sent), 0);
#else
            const ssize_t n = ::send(_s, data + sent, len - sent, MSG_NOSIGNAL);
#endif
            if (n > 0) {
                sent += static_cast<size_t>(n);
                spin = 0;
                continue;
            }
            if (n < 0) {
#ifdef _WIN32
                const int e = WSAGetLastError();
#else
                const int e = errno;
#endif
                if (would_block(e) && ++spin < 8) {
                    continue;
                }
            }
            return false;
        }
        return true;
    }

private:
    socket_t _s = kInvalidSocket;
};

// ---------------------------------------------------------------------------
// TLS (vendored mbedTLS)
// ---------------------------------------------------------------------------

// Process-wide entropy/DRBG, seeded once by a magic static (thread-safe).
inline mbedtls_ctr_drbg_context *shared_drbg() {
    struct Drbg {
        mbedtls_entropy_context entropy;
        mbedtls_ctr_drbg_context drbg;
        bool ok = false;
        Drbg() {
            mbedtls_entropy_init(&entropy);
            mbedtls_ctr_drbg_init(&drbg);
            static const char pers[] = "kimix_http_client";
            ok = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                       reinterpret_cast<const unsigned char *>(pers),
                                       sizeof(pers) - 1) == 0;
        }
        ~Drbg() {
            mbedtls_ctr_drbg_free(&drbg);
            mbedtls_entropy_free(&entropy);
        }
    };
    static Drbg shared;
    return shared.ok ? &shared.drbg : nullptr;
}

#ifndef _WIN32
// System CA bundle for the VERIFY_REQUIRED path (non-Windows). Tries the
// common bundle files (Debian/Ubuntu, RHEL, macOS) then the hashed cert dir;
// nullptr when nothing loaded - the handshake then fails closed, matching the
// old effective behavior (the httplib mbedTLS setup loaded no explicit CA on
// Linux either, but a working bundle path is a net improvement).
inline mbedtls_x509_crt *system_ca() {
    struct Ca {
        mbedtls_x509_crt chain;
        bool loaded = false;
        Ca() {
            mbedtls_x509_crt_init(&chain);
            static const char *files[] = {
                "/etc/ssl/certs/ca-certificates.crt",
                "/etc/pki/tls/certs/ca-bundle.crt",
                "/etc/ssl/cert.pem",
            };
            for (const char *f : files) {
                if (mbedtls_x509_crt_parse_file(&chain, f) == 0) {
                    loaded = true;
                    break;
                }
            }
            if (!loaded &&
                mbedtls_x509_crt_parse_path(&chain, "/etc/ssl/certs") == 0) {
                loaded = true;
            }
        }
        ~Ca() { mbedtls_x509_crt_free(&chain); }
    };
    static Ca ca;
    return ca.loaded ? &ca.chain : nullptr;
}
#endif // _WIN32

#ifdef _WIN32
// Windows full-chain verification policy (ported verbatim from the deleted
// llm/http_tls.h). Problem being solved: mbedTLS' own store misses some
// modern chains (e.g. Google Trust Services WE1 ECDSA intermediates), so the
// handshake runs VERIFY_NONE and the received peer chain is verified with the
// Windows cert engine instead (CertGetCertificateChain resolves the trusted
// root from the system store, same as curl/schannel), plus a SAN hostname
// match on the leaf.

// Case-insensitive DNS name match with a single leading '*' wildcard level
// (RFC 6125: '*' matches one label only, must be the leftmost label).
inline bool tls_match_dns_name(const std::string &pattern, const std::string &host) {
    auto eq = [](char a, char b) {
        return (a | 32) == (b | 32) &&
               ((a >= 'a' && a <= 'z') || (a >= 'A' && a <= 'Z') ||
                (a >= '0' && a <= '9') || a == '.' || a == '-' || a == '*');
    };
    if (pattern.size() >= 2 && pattern[0] == '*' && pattern[1] == '.') {
        const std::string suffix = pattern.substr(1); // ".example.com"
        if (host.size() <= suffix.size()) {
            return false;
        }
        const size_t cut = host.find('.');
        if (cut == std::string::npos) {
            return false; // '*' must match a label
        }
        const std::string rest = host.substr(cut); // ".sub.example.com"
        if (rest.size() != suffix.size()) {
            return false;
        }
        for (size_t i = 0; i < rest.size(); ++i) {
            if (!eq(rest[i], suffix[i])) {
                return false;
            }
        }
        return true;
    }
    if (pattern.size() != host.size()) {
        return false;
    }
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (!eq(pattern[i], host[i])) {
            return false;
        }
    }
    return true;
}

// Decode the leaf cert's SubjectAltName extension and match the hostname
// against its DNS entries. Returns false when the extension is absent or no
// DNS name matches (CN fallback is intentionally NOT done: modern CAs issue
// SAN-only certs and RFC 6125 deprecates CN matching).
inline bool tls_hostname_matches_cert(PCCERT_CONTEXT leaf, const std::string &hostname) {
    if (leaf == nullptr) {
        return false;
    }
    PCERT_EXTENSION ext = CertFindExtension(
        szOID_SUBJECT_ALT_NAME2, leaf->pCertInfo->cExtension,
        leaf->pCertInfo->rgExtension);
    if (ext == nullptr) {
        return false;
    }
    PCERT_ALT_NAME_INFO info = nullptr;
    DWORD size = 0;
    if (!CryptDecodeObjectEx(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                             szOID_SUBJECT_ALT_NAME2, ext->Value.pbData,
                             ext->Value.cbData, CRYPT_DECODE_ALLOC_FLAG, nullptr,
                             &info, &size) ||
        info == nullptr) {
        return false;
    }
    bool matched = false;
    for (DWORD i = 0; i < info->cAltEntry && !matched; ++i) {
        const CERT_ALT_NAME_ENTRY &e = info->rgAltEntry[i];
        if (e.dwAltNameChoice != CERT_ALT_NAME_DNS_NAME || e.pwszDNSName == nullptr) {
            continue;
        }
        // Convert the wide DNS name to ASCII (SAN DNS names are ASCII).
        std::string san;
        for (const wchar_t *w = e.pwszDNSName; *w != L'\0'; ++w) {
            san.push_back(static_cast<char>(*w));
        }
        if (tls_match_dns_name(san, hostname)) {
            matched = true;
        }
    }
    LocalFree(info);
    return matched;
}

// Verify the full mbedtls peer chain with the Windows cert engine. Returns
// true when the chain is trusted AND the hostname matches.
inline bool tls_verify_chain_windows(const mbedtls_x509_crt *chain,
                                     const std::string &hostname) {
    if (chain == nullptr) {
        return false;
    }
    // In-memory store with every cert of the received chain.
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
    if (store == nullptr) {
        return false;
    }
    PCCERT_CONTEXT leaf_ctx = nullptr; // store-owned leaf context
    for (const mbedtls_x509_crt *c = chain; c != nullptr; c = c->next) {
        if (c->raw.p == nullptr || c->raw.len == 0) {
            continue;
        }
        // Add the DER cert straight into the store and keep the store's own
        // context for the first (leaf) entry - it stays valid until the store
        // closes (unlike a temporary context freed right after the add).
        PCCERT_CONTEXT added = nullptr;
        if (!CertAddEncodedCertificateToStore(
                store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, c->raw.p,
                static_cast<DWORD>(c->raw.len), CERT_STORE_ADD_REPLACE_EXISTING,
                &added) ||
            added == nullptr) {
            continue;
        }
        if (leaf_ctx == nullptr) {
            leaf_ctx = added;
        } else {
            CertFreeCertificateContext(added);
        }
    }
    if (leaf_ctx == nullptr) {
        CertCloseStore(store, 0);
        return false;
    }
    bool ok = false;
    CERT_CHAIN_PARA chain_para{};
    chain_para.cbSize = sizeof(chain_para);
    PCCERT_CHAIN_CONTEXT chain_ctx = nullptr;
    // NOTE: chain_ctx must stay nullptr-initialized - CertGetCertificateChain
    // leaves it undefined on failure and freeing garbage crashes. No
    // revocation flags: offline hosts must not hang on a CRL/OCSP fetch
    // (unknown revocation does not set dwErrorStatus).
    const BOOL chain_ok =
        CertGetCertificateChain(nullptr, leaf_ctx, nullptr, store, &chain_para, 0,
                                nullptr, &chain_ctx);
    if (chain_ok && chain_ctx != nullptr) {
        if (chain_ctx->TrustStatus.dwErrorStatus == CERT_TRUST_NO_ERROR) {
            ok = tls_hostname_matches_cert(leaf_ctx, hostname);
        }
        CertFreeCertificateChain(chain_ctx);
    }
    CertFreeCertificateContext(leaf_ctx);
    CertCloseStore(store, 0);
    return ok;
}
#endif // _WIN32

struct Tls {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    bool active = false;

    Tls() {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
    }
    ~Tls() {
        if (active) {
            mbedtls_ssl_free(&ssl);
        }
        mbedtls_ssl_config_free(&conf);
    }
    Tls(const Tls &) = delete;
    Tls &operator=(const Tls &) = delete;
};

// Byte transport: plain socket, or mbedTLS records over that socket.
struct Transport {
    Socket sock;
    Tls tls;
    long read_timeout_ms = 300000;
    long write_timeout_ms = 300000;

    // >0 bytes read, 0 on EOF, -1 on timeout, -2 on error (err set).
    long read(char *buf, size_t cap, Error &err) {
        if (!tls.active) {
            const long n = sock.recv_raw(buf, cap, read_timeout_ms);
            if (n == -1) {
                err = Error::timeout;
            } else if (n == -2) {
                err = Error::read;
            }
            return n;
        }
        int rc;
        do {
            rc = mbedtls_ssl_read(&tls.ssl, reinterpret_cast<unsigned char *>(buf),
                                  cap);
        } while (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE);
        if (rc > 0) {
            return rc;
        }
        if (rc == MBEDTLS_ERR_SSL_TIMEOUT) {
            err = Error::timeout;
            return -1;
        }
        if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == MBEDTLS_ERR_SSL_CONN_EOF ||
            rc == 0) {
            return 0;
        }
        err = Error::read;
        return -2;
    }

    bool write(const char *data, size_t len, Error &err) {
        if (!tls.active) {
            if (!sock.send_raw(data, len, write_timeout_ms)) {
                err = Error::write;
                return false;
            }
            return true;
        }
        size_t off = 0;
        while (off < len) {
            const int rc = mbedtls_ssl_write(
                &tls.ssl, reinterpret_cast<const unsigned char *>(data) + off,
                len - off);
            if (rc > 0) {
                off += static_cast<size_t>(rc);
                continue;
            }
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }
            err = Error::write;
            return false;
        }
        return true;
    }
};

// mbedTLS BIO callbacks over the raw socket (the callbacks block inside the
// timeout-bound recv_raw/send_raw, so the handshake completes in one call and
// deadline overruns surface as MBEDTLS_ERR_SSL_TIMEOUT).
int tls_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    Transport *t = static_cast<Transport *>(ctx);
    return t->sock.send_raw(reinterpret_cast<const char *>(buf), len,
                            t->write_timeout_ms)
               ? static_cast<int>(len)
               : MBEDTLS_ERR_SSL_WANT_WRITE;
}

int tls_bio_recv(void *ctx, unsigned char *buf, size_t len) {
    Transport *t = static_cast<Transport *>(ctx);
    const long n = t->sock.recv_raw(reinterpret_cast<char *>(buf), len,
                                    t->read_timeout_ms);
    if (n > 0) {
        return static_cast<int>(n);
    }
    if (n == 0) {
        return MBEDTLS_ERR_SSL_CONN_EOF;
    }
    if (n == -1) {
        return MBEDTLS_ERR_SSL_TIMEOUT;
    }
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

// ---------------------------------------------------------------------------
// Buffered response reader
// ---------------------------------------------------------------------------

struct Reader {
    Transport &t;
    char buf[16384];
    size_t pos = 0;
    size_t len = 0;

    // 1 = byte read, 0 = EOF, -1 = error (err set).
    int get(char &c, Error &err) {
        if (pos >= len) {
            const long n = t.read(buf, sizeof(buf), err);
            if (n <= 0) {
                return static_cast<int>(n);
            }
            pos = 0;
            len = static_cast<size_t>(n);
        }
        c = buf[pos++];
        return 1;
    }

    // Read exactly n bytes; streamed to `rc` when set, else appended to out.
    bool read_full(std::string &out, size_t n, const ContentReceiver *rc, Error &err) {
        while (n > 0) {
            if (pos >= len) {
                const long r = t.read(buf, sizeof(buf), err);
                if (r <= 0) {
                    return false;
                }
                pos = 0;
                len = static_cast<size_t>(r);
            }
            const size_t take = (len - pos) < n ? (len - pos) : n;
            if (rc != nullptr) {
                if (!(*rc)(buf + pos, take)) {
                    err = Error::canceled;
                    return false;
                }
            } else {
                out.append(buf + pos, take);
            }
            pos += take;
            n -= take;
        }
        return true;
    }

    // Read until EOF (close-delimited body).
    bool read_to_eof(std::string &out, const ContentReceiver *rc, Error &err) {
        for (;;) {
            if (pos < len) {
                const size_t take = len - pos;
                if (rc != nullptr) {
                    if (!(*rc)(buf + pos, take)) {
                        err = Error::canceled;
                        return false;
                    }
                } else {
                    out.append(buf + pos, take);
                }
                pos = len;
            }
            const long r = t.read(buf, sizeof(buf), err);
            if (r == 0) {
                return true;
            }
            if (r < 0) {
                return false;
            }
            pos = 0;
            len = static_cast<size_t>(r);
        }
    }

    // Read one LF-terminated line (CRLF stripped); capped by `cap`.
    // 1 = line, 0 = EOF, -1 = error (err set).
    int read_line(std::string &line, size_t cap, Error &err) {
        line.clear();
        for (;;) {
            char c;
            const int r = get(c, err);
            if (r <= 0) {
                return r;
            }
            if (c == '\n') {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                return 1;
            }
            line.push_back(c);
            if (line.size() > cap) {
                err = Error::read;
                return -1;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// HTTP response parsing
// ---------------------------------------------------------------------------

constexpr size_t kMaxHeaderBlock = 64 * 1024;

// Read the status line + header block; interim 1xx lines are skipped.
bool read_response_head(Reader &r, Response &res, Error &err) {
    std::string line;
    for (;;) {
        const int rr = r.read_line(line, 16 * 1024, err);
        if (rr <= 0) {
            return false;
        }
        if (line.empty()) {
            continue; // tolerate leading blank lines
        }
        if (line.compare(0, 5, "HTTP/") != 0) {
            err = Error::read;
            return false;
        }
        const size_t sp = line.find(' ');
        if (sp == std::string::npos) {
            err = Error::read;
            return false;
        }
        res.status = std::atoi(line.c_str() + sp + 1);
        if (res.status >= 100 && res.status < 200) {
            continue; // interim (e.g. 100 Continue)
        }
        break;
    }
    size_t total = 0;
    for (;;) {
        const int rr = r.read_line(line, kMaxHeaderBlock, err);
        if (rr <= 0) {
            return false;
        }
        if (line.empty()) {
            break;
        }
        total += line.size();
        if (total > kMaxHeaderBlock) {
            err = Error::read;
            return false;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) {
            continue; // tolerate malformed continuation lines
        }
        std::string value = line.substr(colon + 1);
        while (!value.empty() &&
               (value.front() == ' ' || value.front() == '\t')) {
            value.erase(value.begin());
        }
        while (!value.empty() &&
               (value.back() == ' ' || value.back() == '\t')) {
            value.pop_back();
        }
        res.headers.emplace(line.substr(0, colon), std::move(value));
    }
    return true;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool read_chunked(Reader &r, Response &res, const ContentReceiver *rc, Error &err) {
    std::string line;
    for (;;) {
        if (r.read_line(line, 16 * 1024, err) <= 0) {
            return false;
        }
        const size_t semi = line.find(';'); // strip chunk extensions
        const std::string size_txt = ascii_lower(line.substr(0, semi));
        size_t chunk = 0;
        bool any = false;
        for (const char c : size_txt) {
            const int d = hex_value(c);
            if (d < 0) {
                err = Error::read;
                return false;
            }
            chunk = chunk * 16 + static_cast<size_t>(d);
            any = true;
        }
        if (!any) {
            err = Error::read;
            return false;
        }
        if (chunk == 0) {
            // Trailer section ends at a blank line (usually empty).
            for (;;) {
                const int rr = r.read_line(line, kMaxHeaderBlock, err);
                if (rr <= 0) {
                    return false;
                }
                if (line.empty()) {
                    return true;
                }
            }
        }
        if (!r.read_full(res.body, chunk, rc, err)) {
            return false;
        }
        // Each chunk is followed by CRLF; tolerate a missing final CRLF when
        // the server half-closes right after the last chunk (RFC 9112 leniency).
        const int rr = r.read_line(line, 1024, err);
        if (rr < 0) {
            return false;
        }
    }
}

// Read the body per its framing: no-body statuses, chunked, Content-Length,
// or close-delimited. Streamed to `rc` when set (the body then stays empty,
// matching the old httplib ContentReceiver behavior).
bool read_response_body(Reader &r, Response &res, const ContentReceiver *rc,
                        Error &err) {
    if (res.status == 204 || res.status == 304 ||
        (res.status >= 100 && res.status < 200)) {
        return true;
    }
    const std::string te = ascii_lower(res.headers.get("Transfer-Encoding"));
    if (te.find("chunked") != std::string::npos) {
        return read_chunked(r, res, rc, err);
    }
    const std::string cl = res.headers.get("Content-Length");
    if (!cl.empty()) {
        size_t n = 0;
        bool valid = false;
        for (const char c : cl) {
            if (c < '0' || c > '9') {
                break;
            }
            n = n * 10 + static_cast<size_t>(c - '0');
            valid = true;
        }
        if (valid) {
            return n == 0 ? true : r.read_full(res.body, n, rc, err);
        }
    }
    return r.read_to_eof(res.body, rc, err);
}

} // namespace detail

void ensure_network_initialized() { detail::ensure_net(); }

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

Client::Client(std::string scheme_host_port) {
    const size_t sep = scheme_host_port.find("://");
    if (sep == std::string::npos || sep == 0) {
        return;
    }
    _scheme = ascii_lower(std::string_view(scheme_host_port).substr(0, sep));
    if (_scheme != "http" && _scheme != "https") {
        return;
    }
    std::string rest = scheme_host_port.substr(sep + 3);
    const size_t slash = rest.find('/');
    if (slash != std::string::npos) {
        rest = rest.substr(0, slash); // path in a ctor URL is not supported
    }
    const size_t at = rest.rfind('@');
    if (at != std::string::npos) {
        rest = rest.substr(at + 1); // userinfo is never forwarded
    }
    if (rest.empty()) {
        return;
    }
    const bool default_port = _scheme == "https";
    _port = default_port ? 443 : 80;
    if (rest.front() == '[') {
        // IPv6 literal: [::1] or [::1]:8080.
        const size_t close = rest.find(']');
        if (close == std::string::npos) {
            return;
        }
        _host = rest.substr(1, close - 1);
        if (close + 1 < rest.size() && rest[close + 1] == ':') {
            _port = std::atoi(rest.c_str() + close + 2);
        }
    } else {
        const size_t colon = rest.rfind(':');
        if (colon != std::string::npos) {
            _host = rest.substr(0, colon);
            _port = std::atoi(rest.c_str() + colon + 1);
        } else {
            _host = std::move(rest);
        }
    }
    _valid = !_host.empty() && _port > 0 && _port <= 65535;
}

void Client::set_connection_timeout(std::chrono::milliseconds ms) {
    _connect_timeout_ms = static_cast<long>(ms.count());
}

void Client::set_connection_timeout(int sec) {
    _connect_timeout_ms = static_cast<long>(sec) * 1000;
}

void Client::set_read_timeout(std::chrono::milliseconds ms) {
    _read_timeout_ms = static_cast<long>(ms.count());
}

void Client::set_read_timeout(int sec, int usec) {
    _read_timeout_ms = static_cast<long>(sec) * 1000 + usec / 1000;
}

void Client::set_write_timeout(std::chrono::milliseconds ms) {
    _write_timeout_ms = static_cast<long>(ms.count());
}

void Client::set_write_timeout(int sec, int usec) {
    _write_timeout_ms = static_cast<long>(sec) * 1000 + usec / 1000;
}

void Client::set_proxy(const std::string &host, int port) {
    _proxy_host = host;
    _proxy_port = port;
    _has_proxy = !host.empty() && port > 0;
}

void Client::use_windows_certificate_verifier(std::string host) {
#ifdef _WIN32
    _win_verify_host = std::move(host);
#else
    (void)host; // no-op: mbedTLS system-CA verification stays in effect
#endif
}

namespace {

// Establish an https-over-proxy CONNECT tunnel (the tunneled 200 response is
// consumed here; TLS starts right after).
bool establish_tunnel(detail::Transport &t, const std::string &host, int port,
                      Error &err) {
    const std::string target = host + ":" + std::to_string(port);
    const std::string req = "CONNECT " + target + " HTTP/1.1\r\nHost: " + target +
                            "\r\n\r\n";
    if (!t.write(req.data(), req.size(), err)) {
        return false;
    }
    detail::Reader r{t};
    Response res;
    if (!detail::read_response_head(r, res, err)) {
        return false;
    }
    if (res.status < 200 || res.status >= 300) {
        err = Error::connection;
        return false;
    }
    return true;
}

} // namespace

namespace {

// TLS handshake over the connected transport. `verify` enables mbedTLS
// VERIFY_REQUIRED against the system CA bundle (non-Windows); a non-empty
// `win_verify_host` installs the Windows full-chain session policy instead
// (handshake VERIFY_NONE + post-handshake CertGetCertificateChain check).
bool setup_tls(detail::Transport &t, const std::string &host, bool verify,
               const std::string &win_verify_host, Error &err) {
    detail::Tls &tls = t.tls;
    if (mbedtls_ssl_config_defaults(&tls.conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        err = Error::tls;
        return false;
    }
    mbedtls_ssl_conf_rng(&tls.conf, mbedtls_ctr_drbg_random, detail::shared_drbg());
    mbedtls_ssl_conf_min_tls_version(&tls.conf, MBEDTLS_SSL_VERSION_TLS1_2);
    const bool windows_policy = !win_verify_host.empty();
    const bool mbed_verify = verify && !windows_policy;
    mbedtls_ssl_conf_authmode(&tls.conf, mbed_verify ? MBEDTLS_SSL_VERIFY_REQUIRED
                                                     : MBEDTLS_SSL_VERIFY_NONE);
    if (mbed_verify) {
#ifndef _WIN32
        mbedtls_x509_crt *ca = detail::system_ca();
        if (ca != nullptr) {
            mbedtls_ssl_conf_ca_chain(&tls.conf, ca, nullptr);
        }
        // No CA loaded: VERIFY_REQUIRED fails the handshake closed, exactly
        // like the previous effective behavior.
#endif
    }
    if (mbedtls_ssl_setup(&tls.ssl, &tls.conf) != 0) {
        err = Error::tls;
        return false;
    }
    tls.active = true;
    // SNI + (for the verify path) the certificate hostname check.
    mbedtls_ssl_set_hostname(&tls.ssl, host.c_str());
    mbedtls_ssl_set_bio(&tls.ssl, &t, detail::tls_bio_send, detail::tls_bio_recv,
                        nullptr);
    int rc = mbedtls_ssl_handshake(&tls.ssl);
    while (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        rc = mbedtls_ssl_handshake(&tls.ssl);
    }
    if (rc != 0) {
        if (rc == MBEDTLS_ERR_SSL_TIMEOUT) {
            err = Error::timeout;
        } else if (mbedtls_ssl_get_verify_result(&tls.ssl) != 0) {
            err = Error::certificate_rejected;
        } else {
            err = Error::tls;
        }
        return false;
    }
#ifdef _WIN32
    if (windows_policy) {
        // Post-handshake full-chain Windows verification of the peer chain
        // (the handshake itself ran VERIFY_NONE; see the policy note above
        // tls_match_dns_name).
        const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&tls.ssl);
        if (!detail::tls_verify_chain_windows(peer, win_verify_host)) {
            err = Error::certificate_rejected;
            return false;
        }
    }
#endif
    return true;
}

} // namespace

namespace {

// Build one HTTP/1.1 request. With `absolute_form` (plain-http proxy) the
// request target is the full http://host:port/path URI; otherwise the
// origin-form path. Per-request headers override same-name defaults (UA
// included); Content-Type (from the Post argument) and Content-Length/
// Connection are forced last.
std::string build_request(const std::string &method, const std::string &scheme,
                          const std::string &host, int port,
                          const std::string &path, const Headers &defaults,
                          const Headers &req_headers, const std::string &body,
                          const char *content_type, bool absolute_form) {
    const bool default_port =
        (scheme == "https" && port == 443) || (scheme == "http" && port == 80);
    const std::string authority =
        default_port ? host : host + ":" + std::to_string(port);

    std::string target = path.empty() ? "/" : path;
    if (absolute_form) {
        target = scheme + "://" + authority + target;
    }

    Headers merged = defaults;
    for (const auto &[k, v] : req_headers) {
        merged.set(k, v);
    }

    std::string out;
    out.reserve(256 + body.size());
    out += method;
    out += ' ';
    out += target;
    out += " HTTP/1.1\r\nHost: ";
    out += authority;
    out += "\r\n";
    bool has_user_agent = false;
    for (const auto &[k, v] : merged) {
        if (ci_equal(k, "Host") || ci_equal(k, "Connection") ||
            ci_equal(k, "Content-Length")) {
            continue;
        }
        if (content_type != nullptr && ci_equal(k, "Content-Type")) {
            continue;
        }
        if (ci_equal(k, "User-Agent")) {
            has_user_agent = true;
        }
        out += k;
        out += ": ";
        out += v;
        out += "\r\n";
    }
    if (!has_user_agent) {
        // Every request carries a UA, like the previous stack did (httplib
        // sent its default); the fetch tool's UA string is reused.
        out += "User-Agent: Mozilla/5.0 (compatible; kimix-native/1.0; "
               "+https://kimix.dev)\r\n";
    }
    if (content_type != nullptr) {
        out += "Content-Type: ";
        out += content_type;
        out += "\r\n";
    }
    if (method == "POST") {
        out += "Content-Length: ";
        out += std::to_string(body.size());
        out += "\r\n";
    }
    out += "Connection: close\r\n\r\n";
    out += body;
    return out;
}

// Parse a redirect Location and fold it into the current target. Returns
// false when the location is unusable (caller keeps the redirect response).
bool resolve_location(const std::string &loc, std::string &scheme,
                      std::string &host, int &port, std::string &path) {
    std::string l = loc;
    while (!l.empty() && (l.front() == ' ' || l.front() == '\t')) {
        l.erase(l.begin());
    }
    if (l.empty()) {
        return false;
    }
    const size_t sep = l.find("://");
    if (sep != std::string::npos && sep > 0) {
        scheme = ascii_lower(std::string_view(l).substr(0, sep));
        if (scheme != "http" && scheme != "https") {
            return false;
        }
        l = l.substr(sep + 3);
        const size_t end = l.find_first_of("/?#");
        std::string auth = l.substr(0, end);
        const std::string rest = end == std::string::npos ? "/" : l.substr(end);
        const size_t at = auth.rfind('@');
        if (at != std::string::npos) {
            auth = auth.substr(at + 1);
        }
        int new_port = scheme == "https" ? 443 : 80;
        std::string new_host = auth;
        if (!auth.empty() && auth.front() == '[') {
            const size_t close = auth.find(']');
            if (close == std::string::npos) {
                return false;
            }
            new_host = auth.substr(1, close - 1);
            if (close + 1 < auth.size() && auth[close + 1] == ':') {
                new_port = std::atoi(auth.c_str() + close + 2);
            }
        } else {
            const size_t colon = auth.rfind(':');
            if (colon != std::string::npos) {
                new_host = auth.substr(0, colon);
                new_port = std::atoi(auth.c_str() + colon + 1);
            }
        }
        if (new_host.empty() || new_port <= 0 || new_port > 65535) {
            return false;
        }
        host = std::move(new_host);
        port = new_port;
        path = rest;
        return true;
    }
    if (l.front() == '/') {
        path = std::move(l); // absolute path
        return true;
    }
    // Relative path: merge against the current path's directory.
    const size_t slash = path.rfind('/');
    const std::string base = slash == std::string::npos ? "/" : path.substr(0, slash + 1);
    path = base + l;
    return true;
}

} // namespace

Result Client::request(const std::string &method, const std::string &scheme,
                       const std::string &host, int port, const std::string &path,
                       const Headers &headers, const std::string &body,
                       const char *content_type, const ContentReceiver *receiver) {
    const bool https = scheme == "https";
    const std::string connect_host = _has_proxy ? _proxy_host : host;
    const int connect_port = _has_proxy ? _proxy_port : port;
    std::string pinned_ip;
    const std::string *pinned = nullptr;
    if (!_has_proxy) {
        const auto it = _addr_map.find(host);
        if (it != _addr_map.end()) {
            pinned_ip = it->second;
            pinned = &pinned_ip;
        }
    }

    detail::Transport t;
    t.read_timeout_ms = _read_timeout_ms;
    t.write_timeout_ms = _write_timeout_ms;
    Error err = Error::success;
    if (!t.sock.connect(connect_host, std::to_string(connect_port), pinned,
                        _connect_timeout_ms, err)) {
        return Result(err);
    }

    if (https) {
        if (_has_proxy && !establish_tunnel(t, host, port, err)) {
            return Result(err);
        }
        if (!setup_tls(t, host, _verify, _win_verify_host, err)) {
            return Result(err);
        }
    }

    const bool absolute_form = _has_proxy && !https;
    const std::string req =
        build_request(method, scheme, host, port, path, _default_headers, headers,
                      body, content_type, absolute_form);
    if (!t.write(req.data(), req.size(), err)) {
        return Result(err);
    }

    detail::Reader reader{t};
    Response res;
    if (!detail::read_response_head(reader, res, err)) {
        return Result(err);
    }
    if (!detail::read_response_body(reader, res, receiver, err)) {
        return Result(err);
    }
    return Result(std::move(res));
}

Result Client::Get(const std::string &path, const Headers &headers) {
    std::string scheme = _scheme;
    std::string host = _host;
    int port = _port;
    std::string cur_path = path.empty() ? "/" : path;
    constexpr int kMaxRedirects = 5;
    Result last = Result(Error::read);
    for (int hop = 0; hop <= kMaxRedirects; ++hop) {
        last = request("GET", scheme, host, port, cur_path, headers, "", nullptr,
                       nullptr);
        if (!_follow_location || !last) {
            return last;
        }
        const int st = last->status;
        const bool redirect =
            st == 301 || st == 302 || st == 303 || st == 307 || st == 308;
        if (!redirect) {
            return last;
        }
        const std::string loc = last->get_header_value("Location");
        if (loc.empty() || !resolve_location(loc, scheme, host, port, cur_path)) {
            return last;
        }
        // Next hop: fresh DNS + connect, exactly like httplib's internal
        // set_follow_location loop.
    }
    return last; // hop cap reached: return the last redirect response
}

Result Client::Post(const std::string &path, const Headers &headers,
                    const std::string &body, const char *content_type,
                    ContentReceiver receiver) {
    const ContentReceiver *rc = static_cast<bool>(receiver) ? &receiver : nullptr;
    return request("POST", _scheme, _host, _port, path, headers, body, content_type,
                   rc);
}

Result Client::Post(const std::string &path, const std::string &body,
                    const char *content_type) {
    return request("POST", _scheme, _host, _port, path, Headers{}, body, content_type,
                   nullptr);
}

} // namespace kimix::net
