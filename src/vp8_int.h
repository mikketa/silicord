#pragma once
#include "vp8.h"

/*
 * What the VP8 decoder and encoder share: modes, trees and filters,
 * prediction, the inverse transforms and the motion vector census, so
 * that the encoder reconstructs exactly what decoders will.
 */

/* Macroblock modes, then subblock modes, numbered as the probability tables are indexed. */
enum { DC_PRED, V_PRED, H_PRED, TM_PRED, B_PRED, NEARESTMV, NEARMV, ZEROMV, NEWMV, SPLITMV };
enum { B_DC_PRED, B_TM_PRED, B_VE_PRED, B_HE_PRED, B_LD_PRED, B_RD_PRED, B_VR_PRED, B_VL_PRED, B_HD_PRED, B_HU_PRED,
       LEFT4X4, ABOVE4X4, ZERO4X4, NEW4X4 };
enum { CURRENT, LAST, GOLDEN, ALTREF };
enum { TYPE_Y_AFTER_Y2, TYPE_Y2, TYPE_UV, TYPE_Y }; /* coefficient probability sets */

typedef struct {
    short x, y; /* in eighths of a pixel (quarter-pel values, doubled) */
} vp8_mv_t;

typedef struct {
    unsigned char y_mode, uv_mode, segment, ref, skip, coded;
    vp8_mv_t mv;
    vp8_mv_t mvs[16];              /* SPLITMV */
    unsigned char modes[16];   /* B_PRED */
} vp8_mb_t;

/* Scratch for predicting a block from a reference frame, and the frame's subpixel filters. */
typedef struct {
    unsigned char edge[32 * 32];   /* a reference block extended past the frame edges */
    unsigned char pass[16 * 21];   /* the horizontal pass of subpixel filtering */
    const short (*filters)[6];
} vp8i_scratch_t;

extern const signed char vp8_kf_y_mode_tree[8];
extern const signed char vp8_y_mode_tree[8];
extern const signed char vp8_uv_mode_tree[6];
extern const signed char vp8_b_mode_tree[18];
extern const signed char vp8_small_mv_tree[14];
extern const signed char vp8_mv_ref_tree[8];
extern const signed char vp8_submv_ref_tree[6];
extern const signed char vp8_split_mv_tree[6];
extern const unsigned char vp8_zigzag[16];
extern const unsigned char vp8_bands[16];
extern const unsigned char vp8_cat3[];
extern const unsigned char vp8_cat4[];
extern const unsigned char vp8_cat5[];
extern const unsigned char vp8_cat6[];
extern const short vp8_sixtap[8][6];
extern const short vp8_bilinear[8][6];

/* Inline: the loop filter and the predictions call it for every pixel. */
static __inline unsigned char vp8i_clamp255(int v)
{
    return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}
/* The Y2 block's inverse Walsh-Hadamard transform into the Y blocks' DC coefficients. */
void vp8i_iwht(short *coeffs);
/* Adds a block's inverse DCT to the prediction already at dst. */
void vp8i_idct_add(unsigned char *dst, int stride, const short *in);
void vp8i_predict_block(unsigned char *p, int stride, int n, int mode);
/* A 4x4 subblock: above[-1..7] and left[0..3] are its edges in the frame. */
void vp8i_predict_sub(unsigned char *p, int s, int mode);
/*
 * The edges outside the frame: 127 above, 129 to the left, and for DC
 * prediction a copy of the other edge, which averages that edge alone.
 */
void vp8i_fixup_left(unsigned char *p, int stride, int n, int row, int mode);
void vp8i_fixup_above(unsigned char *p, int stride, int n, int col, int mode);
int vp8i_mv_eq(vp8_mv_t a, vp8_mv_t b);
int vp8i_mv_zero(vp8_mv_t a);
vp8_mv_t vp8i_clamp_mv(vp8_mv_t mv, int left, int right, int top, int bottom);
/* A macroblock's vector for its chroma: halved, rounding away from zero; whole pixels only with `full_pixel`. */
vp8_mv_t vp8i_chroma_mv(vp8_mv_t mv, int full_pixel);
/* The neighbours' vectors (above, left, above-left), weighted into best, nearest and near (section 16.3). */
void vp8i_find_near_mvs(const int *sign_bias, const vp8_mb_t *m, const vp8_mb_t *above, const vp8_mb_t *left,
                        vp8_mv_t near_mvs[4], int cnt[4]);
/*
 * A bw x bh block of plane `ref` (pw x ph, the macroblock-aligned size)
 * at (x, y) moved by mv, into dst. Pixels past the edges repeat the edge;
 * fractional positions go through the frame's six-tap or bilinear filters,
 * horizontally then vertically.
 */
void vp8i_predict_inter_block(vp8i_scratch_t *d, unsigned char *dst, int ds, const unsigned char *ref, int rs, int pw,
                              int ph, int x, int y, int bw, int bh, vp8_mv_t mv);
