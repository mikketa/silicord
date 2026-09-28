#pragma once
#include "sb.h"
#include "vp8.h"

/*
 * A VP8 encoder for live video: key frames and inter frames predicting
 * from the last one, 16x16 intra modes, motion found by a diamond search,
 * and a quantizer steered toward a bitrate. It reconstructs each frame
 * with the decoder's own code, so what it predicts from is exactly what
 * decoders see.
 */

typedef struct vp8_encoder vp8_encoder_t;

vp8_encoder_t *vp8_encoder_new(int w, int h, int kbps, int fps);
void vp8_encoder_free(vp8_encoder_t *e);
/* Encodes a w x h I420 picture (a key frame when `key` is set, and always first) into `out`. */
int vp8_encode(vp8_encoder_t *e, const vp8_image_t *img, int key, sb_t *out);
/* The last frame as decoders will reconstruct it. */
void vp8_encoder_recon(const vp8_encoder_t *e, vp8_image_t *out);
/* The quantizer index in use (0-127). */
int vp8_encoder_q(const vp8_encoder_t *e);
