/* openssl_stub.c - Empty stub for the kimix-openssl xmake target.
 *
 * The real OpenSSL libraries (libssl/libcrypto) are produced by OpenSSL's own
 * Configure + nmake/make build (see before_build in src/ext/xmake.lua) and
 * linked by consumers through the target's public linkdirs/links. This stub
 * simply gives the static target a translation unit so xmake archives it.
 */
int kimix_openssl_stub_symbol(void) {
    return 0;
}
