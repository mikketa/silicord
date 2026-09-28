#pragma once
#include <stddef.h>

/* Cryptographically secure random bytes from the system. Returns 0 on failure. */
int rng_bytes(void *p, size_t n);
