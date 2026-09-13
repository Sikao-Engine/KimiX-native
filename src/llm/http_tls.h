// http_tls.h - Shared TLS verification policy for the LLM HTTP clients.
//
// Problem: cpp-httplib's mbedtls backend verifies the server chain (handshake,
// MBEDTLS_SSL_VERIFY_REQUIRED) against the Windows ROOT/CA store snapshot, but
// some modern chains (e.g. Google Trust Services WE1 ECDSA intermediates that
// are not cached locally) fail with MBEDTLS_X509_BADCERT_MISSING|BAD_KEY even
// though the OS trusts them (curl/schannel accept the same server). httplib's
// schannel fallback only receives the LEAF cert, so CertGetCertificateChain
// cannot complete a partial chain either.
//
// Fix (Windows only): switch the mbedtls handshake to VERIFY_NONE and install
// a session verifier that performs the authoritative Windows check with the
// FULL peer chain mbedtls received during the handshake:
//   1. walk the mbedtls_x509_crt chain and add every raw DER cert to an
//      in-memory HCERTSTORE,
//   2. CertGetCertificateChain against that store (Windows resolves the
//      trusted root from the system store, same as curl/schannel),
//   3. hostname matching against the leaf's DNS SANs (wildcard-aware),
//      decoded with CryptDecodeObjectEx - httplib's mbedtls verify_hostname
//      is unusable here (it crashes on mbedtls 3.6 SAN layouts).
// The session verifier runs after the handshake and before any request data
// is exchanged; CertificateRejected aborts the connection exactly like the
// default path would.
//
// On non-Windows platforms this header is a no-op and the default mbedtls
// system-CA verification stays in effect.
//
// Include AFTER <httplib.h>.

#pragma once

#include <httplib.h>

#include <string>
#include <vector>

#ifdef CPPHTTPLIB_WINDOWS_AUTOMATIC_ROOT_CERTIFICATES_UPDATE
    #include <windows.h>
    #include <wincrypt.h>

    #include <mbedtls/x509_crt.h>
#endif

namespace kimix::llm {

#ifdef CPPHTTPLIB_WINDOWS_AUTOMATIC_ROOT_CERTIFICATES_UPDATE
namespace detail {

// Case-insensitive DNS name match with a single leading '*' wildcard level
// (RFC 6125: '*' matches one label only, must be the leftmost label).
inline bool tls_match_dns_name(const std::string &pattern,
                               const std::string &host) {
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
inline bool tls_hostname_matches_cert(PCCERT_CONTEXT leaf,
                                      const std::string &hostname) {
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
                             ext->Value.cbData, CRYPT_DECODE_ALLOC_FLAG,
                             nullptr, &info, &size) ||
        info == nullptr) {
        return false;
    }
    bool matched = false;
    for (DWORD i = 0; i < info->cAltEntry && !matched; ++i) {
        const CERT_ALT_NAME_ENTRY &e = info->rgAltEntry[i];
        if (e.dwAltNameChoice != CERT_ALT_NAME_DNS_NAME ||
            e.pwszDNSName == nullptr) {
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
                static_cast<DWORD>(c->raw.len),
                CERT_STORE_ADD_REPLACE_EXISTING, &added) ||
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
    const BOOL chain_ok = CertGetCertificateChain(
        nullptr, leaf_ctx, nullptr, store, &chain_para, 0, nullptr, &chain_ctx);
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

} // namespace detail
#endif

// Apply the Windows schannel verification policy to an httplib client. Call
// after constructing the client and before the first request.
inline void install_windows_tls_verifier(httplib::Client &cli,
                                         const std::string &host) {
#ifdef CPPHTTPLIB_WINDOWS_AUTOMATIC_ROOT_CERTIFICATES_UPDATE
    // mbedtls handshake: skip chain verification (Windows does it below).
    cli.enable_server_certificate_verification(false);
    cli.set_session_verifier(
        [host](httplib::tls::session_t session) {
            auto cert = httplib::tls::get_peer_cert(session);
            if (cert == nullptr) {
                return httplib::SSLVerifierResponse::CertificateRejected;
            }
            // cert is the mbedtls_x509_crt* head-of-chain under the mbedtls
            // backend (httplib::tls::cert_t == void*).
            const auto *chain = static_cast<const mbedtls_x509_crt *>(
                static_cast<const void *>(cert));
            const bool ok = detail::tls_verify_chain_windows(chain, host);
            return ok ? httplib::SSLVerifierResponse::CertificateAccepted
                      : httplib::SSLVerifierResponse::CertificateRejected;
        });
#else
    (void)cli;
    (void)host;
#endif
}

} // namespace kimix::llm
