Mbed TLS (kimix-mbedtls)

Vendored copy of Mbed TLS 3.6.7 (Apache-2.0 / GPL-2.0-or-later; see
LICENSE) that provides the cross-platform TLS/crypto backend for the
project's HTTPS traffic. The consumer is the hand-written kimix::net HTTP(S)
client (src/llm/http_client.h/.cpp, raw sockets + this library); it replaced
both the previous OpenSSL (openssl3 xrepo package) dependency and the
formerly vendored cpp-httplib (which used this library via
CPPHTTPLIB_MBEDTLS_SUPPORT and has since been removed).

Why Mbed TLS

- Single codebase, fully cross-platform (Windows / macOS / Linux) — no
  per-OS TLS implementations.
- Compiles purely with xmake from vendored sources: no perl, no Configure,
  no scripts. The official release tarball includes all pre-generated
  sources (library/error.c, ssl_debug_helpers_generated.c, etc.).
- Windows cert verification uses the full-chain session policy in
  http_client.cpp (CertGetCertificateChain over the mbedTLS peer chain, for
  chains mbedTLS' own store misses); non-Windows verification loads the
  system CA bundle onto this library's X.509 CRT parser.

Vendored footprint (minimal)

Only what a static library build needs:

[code block: 8 lines]

Not vendored: tests/, programs/, scripts/, docs/, cmake/,
visualc/, configs/, doxygen/, framework/, 3rdparty/, etc.

Config trimming

include/mbedtls/mbedtls_config.h is the upstream 3.6.7 default with the
following modules disabled (clear wins — nothing the HTTPS client needs
depends on them):

| Disabled | Reason |
|---|---|
| MBEDTLS_PSA_CRYPTO_C / STORAGE / ITS_FILE / KEY_STORE_DYNAMIC | the client uses the classic entropy/CTR_DRBG RNG, not PSA Crypto |
| MBEDTLS_SSL_PROTO_TLS1_3 (+ TLS 1.3 options) | TLS 1.3 requires PSA Crypto; HTTPS works over TLS 1.2 (MBEDTLS_SSL_PROTO_TLS1_2 kept) |
| MBEDTLS_SSL_PROTO_DTLS (+ DTLS options) | client TLS only; no DTLS |
| MBEDTLS_DEBUG_C, MBEDTLS_SELF_TEST | no debug/self-test needed |
| MBEDTLS_LMS_C, MBEDTLS_ECJPAKE_C | post-quantum / exotic key exchange unused |
| MBEDTLS_CAMELLIA_C, ARIA_C, DES_C, CHACHA20_C, CHACHAPOLY_C, POLY1305_C, RIPEMD160_C, SHA3_C, CMAC_C, NIST_KW_C, HKDF_C, HMAC_DRBG_C, PKCS5_C, PKCS7_C, PKCS12_C | unused cipher/algorithm modules |
| MBEDTLS_X509_CRL_PARSE_C, CSR_PARSE_C, CREATE_C, CRT_WRITE_C, CSR_WRITE_C, PEM_WRITE_C, PK_WRITE_C | client only needs X.509 CRT parse + PEM parse |
| MBEDTLS_SSL_CACHE_C, COOKIE_C, TICKET_C | server-side session/cookie/ticket machinery unused by the client |
| MBEDTLS_SSL_CONTEXT_SERIALIZATION, KEYING_MATERIAL_EXPORT, ALL_ALERT_MESSAGES | optional client features not needed |

Kept (everything the HTTPS client references): TLS 1.2
client/server, X.509 CRT parse/use, PK + PK parse, entropy, CTR_DRBG, MD +
MD5/SHA1/SHA224/SHA256/SHA384/SHA512, OID, ASN.1 parse/write, PEM parse,
Base64, bignum (MPI), RSA + ECP/ECDSA/ECDH, AES + GCM/CCM ciphers, platform,
error, timing, net_sockets, version, SNI, session tickets, keep-peer-cert.

If a future consumer needs a module that was trimmed, re-enable the
specific define in mbedtls_config.h (the config lives at the standard path
so no MBEDTLS_CONFIG_FILE define is needed by consumers).

Build

Consumed as the kimix-mbedtls xmake target in ../xmake.lua. It is a
plain static library: add_files("library/*.c"), no unity build, no PCH
(the project-wide kimix_basic_settings rule applies common flags). Windows
links ws2_32 (Winsock) and crypt32 (the Windows cert-chain verification in
http_client.cpp).

Version

Pinned: Mbed TLS 3.6.7 (3.6 LTS, supported until ~2027). Do NOT bump to
4.x — 4.x restructures PSA Crypto into a separate TF-PSA-Crypto subtree;
the 3.6 classic-RNG/V3-era API surface is what http_client.cpp is written
against.
