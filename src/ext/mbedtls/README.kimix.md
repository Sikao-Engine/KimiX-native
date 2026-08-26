# Mbed TLS (kimix-mbedtls)

Vendored copy of **Mbed TLS 3.6.7** (Apache-2.0 / GPL-2.0-or-later; see
`LICENSE`) that provides the cross-platform TLS/crypto backend for
cpp-httplib (`CPPHTTPLIB_MBEDTLS_SUPPORT`). It replaces the previous OpenSSL
(`openssl3` xrepo package) dependency for the HTTPS demos.

## Why Mbed TLS

- Single codebase, fully cross-platform (Windows / macOS / Linux) — no
  per-OS TLS implementations.
- cpp-httplib (vendored in `../cpp-httplib`) ships a first-class MbedTLS
  backend (`CPPHTTPLIB_MBEDTLS_V3` path for 3.x), so **no changes** to
  third-party `cpp-httplib` are needed.
- Compiles purely with xmake from vendored sources: no perl, no `Configure`,
  no scripts. The official release tarball includes all pre-generated
  sources (`library/error.c`, `ssl_debug_helpers_generated.c`, etc.).
- Windows Schannel cert verification (`verify_cert_with_windows_schannel`)
  works with the MbedTLS backend, so HTTPS root-cert validation uses the
  system store just like the OpenSSL path did.

## Vendored footprint (minimal)

Only what a static library build needs:

```
include/mbedtls/**   public headers (incl. mbedtls_config.h)
include/psa/**       PSA headers kept for files that reference them under guards
library/*.c          all library sources (PSA sources compile to no-ops with PSA off)
library/*.h          private headers used by library sources
LICENSE              Apache-2.0 / GPL-2.0-or-later
README.md            upstream readme
README.kimix.md      this file
```

Not vendored: `tests/`, `programs/`, `scripts/`, `docs/`, `cmake/`,
`visualc/`, `configs/`, `doxygen/`, `framework/`, `3rdparty/`, etc.

## Config trimming

`include/mbedtls/mbedtls_config.h` is the upstream 3.6.7 default with the
following modules disabled (clear wins — nothing cpp-httplib's MbedTLS V3
backend needs depends on them):

| Disabled | Reason |
|---|---|
| `MBEDTLS_PSA_CRYPTO_C` / `STORAGE` / `ITS_FILE` / `KEY_STORE_DYNAMIC` | cpp-httplib V3 path uses the classic entropy/CTR_DRBG RNG, not PSA Crypto |
| `MBEDTLS_SSL_PROTO_TLS1_3` (+ TLS 1.3 options) | TLS 1.3 requires PSA Crypto; HTTPS works over TLS 1.2 (`MBEDTLS_SSL_PROTO_TLS1_2` kept) |
| `MBEDTLS_SSL_PROTO_DTLS` (+ DTLS options) | client TLS only; no DTLS |
| `MBEDTLS_DEBUG_C`, `MBEDTLS_SELF_TEST` | no debug/self-test needed |
| `MBEDTLS_LMS_C`, `MBEDTLS_ECJPAKE_C` | post-quantum / exotic key exchange unused |
| `MBEDTLS_CAMELLIA_C`, `ARIA_C`, `DES_C`, `CHACHA20_C`, `CHACHAPOLY_C`, `POLY1305_C`, `RIPEMD160_C`, `SHA3_C`, `CMAC_C`, `NIST_KW_C`, `HKDF_C`, `HMAC_DRBG_C`, `PKCS5_C`, `PKCS7_C`, `PKCS12_C` | unused cipher/algorithm modules |
| `MBEDTLS_X509_CRL_PARSE_C`, `CSR_PARSE_C`, `CREATE_C`, `CRT_WRITE_C`, `CSR_WRITE_C`, `PEM_WRITE_C`, `PK_WRITE_C` | client only needs X.509 CRT parse + PEM parse |
| `MBEDTLS_SSL_CACHE_C`, `COOKIE_C`, `TICKET_C` | server-side session/cookie/ticket machinery unused by the client demos |
| `MBEDTLS_SSL_CONTEXT_SERIALIZATION`, `KEYING_MATERIAL_EXPORT`, `ALL_ALERT_MESSAGES` | optional client features not needed |

Kept (everything cpp-httplib's MbedTLS backend references): TLS 1.2
client/server, X.509 CRT parse/use, PK + PK parse, entropy, CTR_DRBG, MD +
MD5/SHA1/SHA224/SHA256/SHA384/SHA512, OID, ASN.1 parse/write, PEM parse,
Base64, bignum (MPI), RSA + ECP/ECDSA/ECDH, AES + GCM/CCM ciphers, platform,
error, timing, net_sockets, version, SNI, session tickets, keep-peer-cert.

If a future cpp-httplib upgrade needs a module that was trimmed, re-enable the
specific define in `mbedtls_config.h` (the config lives at the standard path
so no `MBEDTLS_CONFIG_FILE` define is needed by consumers).

## Build

Consumed as the `kimix-mbedtls` xmake target in `../xmake.lua`. It is a
plain static library: `add_files("library/*.c")`, no unity build, no PCH
(the project-wide `kimix_basic_settings` rule applies common flags). Windows
links `ws2_32` (Winsock) and `crypt32` (Schannel cert verification).

## Version

Pinned: **Mbed TLS 3.6.7** (3.6 LTS, supported until ~2027). Do NOT bump to
4.x — 4.x restructures PSA Crypto into a separate TF-PSA-Crypto subtree and
moves to the `CPPHTTPLIB_MBEDTLS_V4` path; the V3 path is the safe target for
the vendored cpp-httplib.
