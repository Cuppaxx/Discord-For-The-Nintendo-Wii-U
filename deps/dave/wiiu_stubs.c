/* POSIX bits newlib/wut lack that libcrypto references, plus the entropy hook. */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <coreinit/time.h>
#include "dave_c.h"

int getuid(void)  { return 1000; }
int geteuid(void) { return 1000; }
int getgid(void)  { return 1000; }
int getegid(void) { return 1000; }

static int (*g_entropy_cb)(void *buf, size_t len) = 0;

void dave_set_entropy_source(int (*cb)(void *buf, size_t len)) { g_entropy_cb = cb; }

/* OpenSSL's rand_unix.c calls getentropy() (weak lookup) to seed its DRBG.
 * Prefer the application-registered source (return 0 on success).
 * FALLBACK (weak! timing jitter only, SplitMix64 over console timebase): replace
 * with a real source from the app via dave_set_entropy_source() for production. */
int getentropy(void *buf, size_t len) {
    if (g_entropy_cb && g_entropy_cb(buf, len) == 0) return 0;
    uint8_t *p = (uint8_t *)buf;
    uint64_t x = (uint64_t)OSGetSystemTime() ^ ((uint64_t)OSGetSystemTick() << 32);
    for (size_t i = 0; i < len; i += 8) {
        x += 0x9E3779B97F4A7C15ULL + (uint64_t)OSGetSystemTick() * 0x100000001B3ULL;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z ^= z >> 31;
        for (size_t j = 0; j < 8 && i + j < len; j++) p[i + j] = (uint8_t)(z >> (8 * j));
    }
    return 0;
}
