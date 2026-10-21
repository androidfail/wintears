/* SHA-256 through Windows CNG (upstream uses CommonCrypto). */
#include <windows.h>
#include <bcrypt.h>

#include "host/hos.h"

static BCRYPT_ALG_HANDLE provider;
static INIT_ONCE provider_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK open_provider(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    return BCryptOpenAlgorithmProvider(&provider, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0;
}

void host_sha256_begin(host_sha256 *s) {
    if (!InitOnceExecuteOnce(&provider_once, open_provider, NULL, NULL)) die("SHA-256 provider");
    BCRYPT_HASH_HANDLE h;
    if (BCryptCreateHash(provider, &h, NULL, 0, NULL, 0, 0)) die("SHA-256 hash object");
    s->h = h;
}

void host_sha256_update(host_sha256 *s, const void *p, size_t n) {
    while (n) {
        ULONG chunk = n > 0x40000000u ? 0x40000000u : (ULONG)n;
        if (BCryptHashData((BCRYPT_HASH_HANDLE)s->h, (PUCHAR)(uintptr_t)p, chunk, 0)) die("SHA-256 update");
        p = (const uint8_t *)p + chunk;
        n -= chunk;
    }
}

void host_sha256_hex(host_sha256 *s, char hex[65]) {
    uint8_t d[32];
    if (BCryptFinishHash((BCRYPT_HASH_HANDLE)s->h, d, sizeof d, 0)) die("SHA-256 finish");
    BCryptDestroyHash((BCRYPT_HASH_HANDLE)s->h);
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
}
