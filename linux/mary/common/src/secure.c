#include "common/secure.h"

#include <string.h>

void mc_secure_zero(void *p, size_t n) {
    if (!p || !n) return;
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25))
    explicit_bzero(p, n);
#else
    /* No explicit_bzero (macOS): call memset through a volatile pointer, which
     * the optimizer cannot prove is memset and so cannot drop as a dead store. */
    static void *(*const volatile wipe)(void *, int, size_t) = memset;
    wipe(p, 0, n);
#endif
}

bool mc_secure_equal(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++) diff |= x[i] ^ y[i];
    return diff == 0;
}
