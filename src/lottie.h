#pragma once
#include <stddef.h>
#include "sb.h"

/*
 * Lottie animations (Discord's animated stickers, format 3), drawn one frame
 * at a time as an SVG document: shape layers (paths, ellipses, rectangles,
 * fills and strokes), groups, parenting and precompositions. Masks, mattes,
 * gradients and trim paths are left out.
 *
 * The JSON is read once, into keyframes and shapes ready to evaluate, so a
 * frame costs only arithmetic and the SVG text.
 */
typedef struct lottie lottie_t;

/* NULL if the bytes are not a Lottie document. The bytes can go once it returns. */
lottie_t *lottie_load(const char *s, size_t n);
/* Its first and last frames and its rate (frames a second). */
void lottie_info(const lottie_t *L, double *first, double *last, double *fps);
/* Replaces `svg` with frame `frame` of the animation. */
void lottie_svg(lottie_t *L, double frame, sb_t *svg);
size_t lottie_bytes(const lottie_t *L);
void lottie_free(lottie_t *L);
