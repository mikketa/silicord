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
void picture_scratch_free(picture_scratch_t *scratch);
