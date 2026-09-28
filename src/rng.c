#include "rng.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>

int rng_bytes(void *p, size_t n)
{
    return BCryptGenRandom(NULL, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}
#else
#include <sys/random.h>

int rng_bytes(void *p, size_t n)
{
    unsigned char *b = p;

    while (n) {
        ssize_t got = getrandom(b, n, 0);
        if (got <= 0)
            return 0;
        b += got;
        n -= (size_t)got;
    }
    return 1;
}
#endif
