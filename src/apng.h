#pragma once
#include <stddef.h>

/*
 * Animated PNG player (avatar decorations are APNGs; WIC only sees their
 * first frame). Frames are decoded one at a time onto the caller's canvas,
 * so an animation costs its file and one canvas, not every frame.
 * Handles 8-bit RGBA, RGB and palette images, not interlaced.
 */
typedef struct apng apng_t;

/* NULL unless `data` is an animated PNG this player handles. The bytes are copied. */
apng_t *apng_open(const void *data, size_t n);
void apng_size(const apng_t *a, unsigned *w, unsigned *h);
unsigned apng_frames(const apng_t *a);
size_t apng_bytes(const apng_t *a);
/*
 * Composes the next frame (the first on the first call, looping after the
 * last) onto `canvas`: w * h premultiplied BGRA pixels that the caller keeps
 * between calls. Returns the frame's delay in ms, or 0 on broken data.
 */
unsigned apng_next(apng_t *a, unsigned char *canvas);
void apng_free(apng_t *a);
