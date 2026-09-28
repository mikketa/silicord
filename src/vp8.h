#pragma once
#include <stddef.h>

/*
 * A VP8 video decoder (RFC 6386): key and inter frames, all four
 * bitstream versions, segmentation, golden and altref references, and the
 * normal and simple loop filters. Its output is bit-exact with the
 * reference decoder.
 */

typedef struct vp8_decoder vp8_decoder_t;

/* A decoded picture, I420: planes of `w` x `h` and (w + 1) / 2 x (h + 1) / 2, valid until the next decode. */
typedef struct {
    int w, h;
    const unsigned char *y, *u, *v;
    int y_stride, uv_stride;
} vp8_image_t;

vp8_decoder_t *vp8_decoder_new(void);
void vp8_decoder_free(vp8_decoder_t *d);
/* Decodes one frame. Returns 1 with the picture when the frame is to be shown, 0 when it only updates the
   references, -1 for a frame that cannot be decoded (corrupt, or an inter frame before any key frame). */
int vp8_decode(vp8_decoder_t *d, const unsigned char *data, size_t n, vp8_image_t *out);
