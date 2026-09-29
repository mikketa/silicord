#pragma once
#include <stddef.h>
#include "vp8.h"

/* Memory a conversion keeps for the next one: pictures arrive at the same sizes frame after frame. */
typedef struct {
    unsigned char *mem;
    size_t size;
} picture_scratch_t;

/* The largest w x h with the aspect ratio of pw x ph that fits in tw x th, letterboxed (at least 1 x 1). */
void picture_fit(int pw, int ph, int tw, int th, int *w, int *h);

/*
 * A picture (BT.601 video range, as VP8 video is) as opaque BGRA of exactly
 * w x h, so that it is drawn by copying. Shrinking averages the pixels
 * under each one: by 2 or 4 in YUV first, as many pixels fewer to convert,
 * then by the area each covers; enlarging is bilinear. At its own size the
 * conversion is exact, pixel by pixel.
 */
void picture_to_bgra(const vp8_image_t *img, int w, int h, unsigned *out, picture_scratch_t *scratch);
/*
 * A BGRA picture of sw x sh, rows `stride` pixels apart, resampled to
 * exactly w x h (area shrinking, bilinear enlarging), as picture_to_bgra()
 * does after its conversion.
 */
void picture_scale_bgra(const unsigned *src, int sw, int sh, int stride, int w, int h, unsigned *out,
                        picture_scratch_t *scratch);
/*
 * BGRA to I420 in BT.601 video range, as a VP8 encoder takes it: luma
 * pixel by pixel, chroma from the average of each 2 x 2 box (the last row
 * or column doubled when h or w is odd).
 */
void picture_bgra_to_i420(const unsigned *bgra, int w, int h, int stride, unsigned char *y, unsigned char *u,
                          unsigned char *v, int y_stride, int uv_stride);
/* A mouse pointer's shape, as the screen capture gives it (the values are DXGI's). */
enum { PICTURE_CURSOR_MONOCHROME = 1, PICTURE_CURSOR_COLOR = 2, PICTURE_CURSOR_MASKED = 4 };
/*
 * Draws a pointer shape of sw x sh (a monochrome one: its AND mask, then
 * its XOR mask, each sh / 2 rows of 1 bit a pixel) with its top left at
 * (x, y) of a BGRA picture, clipped to it: color shapes blended by their
 * alpha, masked ones replacing or inverting where their alpha is 0 or not.
 */
void picture_draw_cursor(unsigned *bgra, int w, int h, int stride, int x, int y, int type, const unsigned char *shape,
                         int sw, int sh, int pitch);
void picture_scratch_free(picture_scratch_t *scratch);
