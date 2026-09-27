#pragma once
#include <stddef.h>

/* QR code encoder: byte mode, error correction level M, versions 1 to 10 (up to 213 bytes). */

#define QR_MAX_VERSION 10
#define QR_MAX_SIZE (QR_MAX_VERSION * 4 + 17)

typedef struct {
    int size;
    unsigned char module[QR_MAX_SIZE * QR_MAX_SIZE]; /* 1 = dark, row-major */
} qr_t;

/* Returns 0 if the data does not fit. Allocate `out` on the heap: it is ~3 KB. */
int qr_encode(const void *data, size_t n, qr_t *out);

static __inline int qr_dark(const qr_t *qr, int x, int y)
{
    return qr->module[y * qr->size + x];
}
