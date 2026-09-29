#include <emmintrin.h>
#include <string.h>
#include "vp8_int.h"

/* Stores the first n (4 or 8) of 8 pixels. */
static void store_px(unsigned char *dst, __m128i px, int n)
{
    if (n >= 8) {
        _mm_storel_epi64((__m128i *)dst, px);
    } else {
        int v = _mm_cvtsi128_si32(px);
        memcpy(dst, &v, 4);
    }
}

/* Trees (RFC 6386, section 8.1): positive entries index the next pair, others are negated leaves. */
const signed char vp8_kf_y_mode_tree[8] = {-B_PRED, 2, 4, 6, -DC_PRED, -V_PRED, -H_PRED, -TM_PRED};
const signed char vp8_y_mode_tree[8] = {-DC_PRED, 2, 4, 6, -V_PRED, -H_PRED, -TM_PRED, -B_PRED};
const signed char vp8_uv_mode_tree[6] = {-DC_PRED, 2, -V_PRED, 4, -H_PRED, -TM_PRED};
const signed char vp8_b_mode_tree[18] = {-B_DC_PRED, 2,          -B_TM_PRED, 4,  -B_VE_PRED, 6,  8,          12,
                                         -B_HE_PRED, 10,         -B_RD_PRED, -B_VR_PRED, -B_LD_PRED, 14, -B_VL_PRED, 16,
                                         -B_HD_PRED, -B_HU_PRED};
const signed char vp8_small_mv_tree[14] = {2, 8, 4, 6, -0, -1, -2, -3, 10, 12, -4, -5, -6, -7};
const signed char vp8_mv_ref_tree[8] = {-ZEROMV, 2, -NEARESTMV, 4, -NEARMV, 6, -NEWMV, -SPLITMV};
const signed char vp8_submv_ref_tree[6] = {-LEFT4X4, 2, -ABOVE4X4, 4, -ZERO4X4, -NEW4X4};
const signed char vp8_split_mv_tree[6] = {-3, 2, -2, 4, -0, -1};

const unsigned char vp8_zigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
const unsigned char vp8_bands[16] = {0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7};
/* Extra bits of the DCT_CAT3..6 tokens, most significant first (DCT_CAT1 and 2 are written inline). */
const unsigned char vp8_cat3[] = {173, 148, 140, 0};
const unsigned char vp8_cat4[] = {176, 155, 140, 135, 0};
const unsigned char vp8_cat5[] = {180, 157, 141, 134, 130, 0};
const unsigned char vp8_cat6[] = {254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0};

const short vp8_sixtap[8][6] = {
    {0, 0, 128, 0, 0, 0},     {0, -6, 123, 12, -1, 0}, {2, -11, 108, 36, -8, 1}, {0, -9, 93, 50, -6, 0},
    {3, -16, 77, 77, -16, 3}, {0, -6, 50, 93, -9, 0},  {1, -8, 36, 108, -11, 2}, {0, -1, 12, 123, -6, 0},
};
const short vp8_bilinear[8][6] = {
    {0, 0, 128, 0, 0, 0}, {0, 0, 112, 16, 0, 0}, {0, 0, 96, 32, 0, 0}, {0, 0, 80, 48, 0, 0},
    {0, 0, 64, 64, 0, 0}, {0, 0, 48, 80, 0, 0},  {0, 0, 32, 96, 0, 0}, {0, 0, 16, 112, 0, 0},
};

#define COS_M1 20091 /* cos(pi/8) * sqrt(2) - 1, Q16 */
#define SIN 35468    /* sin(pi/8) * sqrt(2), Q16 */

#define AVG3(a, b, c) (unsigned char)(((a) + 2 * (b) + (c) + 2) >> 2)
#define AVG2(a, b) (unsigned char)(((a) + (b) + 1) >> 1)

static void predict_dc(unsigned char *p, int stride, int n, int shift)
{
    int sum = 0;

    for (int i = 0; i < n; i++)
        sum += p[-stride + i] + p[i * stride - 1];
    sum = (sum + (1 << (shift - 1))) >> shift;
    for (int i = 0; i < n; i++)
        memset(p + i * stride, sum, (size_t)n);
}

/* The Y2 block's inverse Walsh-Hadamard transform into the Y blocks' DC coefficients. */
void vp8i_iwht(short *coeffs)
{
    const short *in = coeffs + 24 * 16;
    short tmp[16];

    for (int i = 0; i < 4; i++) {
        int a1 = in[i] + in[12 + i], b1 = in[4 + i] + in[8 + i], c1 = in[4 + i] - in[8 + i], d1 = in[i] - in[12 + i];
        tmp[i] = (short)(a1 + b1);
        tmp[4 + i] = (short)(c1 + d1);
        tmp[8 + i] = (short)(a1 - b1);
        tmp[12 + i] = (short)(d1 - c1);
    }
    for (int i = 0; i < 4; i++) {
        const short *r = tmp + 4 * i;
        int a1 = r[0] + r[3], b1 = r[1] + r[2], c1 = r[1] - r[2], d1 = r[0] - r[3];
        coeffs[(4 * i + 0) * 16] = (short)((a1 + b1 + 3) >> 3);
        coeffs[(4 * i + 1) * 16] = (short)((c1 + d1 + 3) >> 3);
        coeffs[(4 * i + 2) * 16] = (short)((a1 - b1 + 3) >> 3);
        coeffs[(4 * i + 3) * 16] = (short)((d1 - c1 + 3) >> 3);
    }
}

/* Adds a block's inverse DCT to the prediction already at dst. */
/* (x * SIN) >> 16 for 16-bit x: SIN does not fit a signed 16-bit factor, so x * (SIN - 65536) >> 16, plus x. */
static __m128i mul_sin(__m128i x)
{
    return _mm_add_epi16(_mm_mulhi_epi16(x, _mm_set1_epi16((short)(SIN - 65536))), x);
}

/* x + ((x * COS_M1) >> 16) */
static __m128i mul_cos(__m128i x)
{
    return _mm_add_epi16(x, _mm_mulhi_epi16(x, _mm_set1_epi16(COS_M1)));
}

/* Sign-extends the four low 16-bit lanes to 32 bits. */
static __m128i widen(__m128i x)
{
    return _mm_srai_epi32(_mm_unpacklo_epi16(x, x), 16);
}

/* 4x4 of 16-bit lanes, rows in the low halves of r[0..3], transposed into the same. */
static void transpose4(__m128i r[4])
{
    __m128i a = _mm_unpacklo_epi16(r[0], r[1]), b = _mm_unpacklo_epi16(r[2], r[3]);
    __m128i lo = _mm_unpacklo_epi32(a, b), hi = _mm_unpackhi_epi32(a, b);

    r[0] = lo;
    r[1] = _mm_srli_si128(lo, 8);
    r[2] = hi;
    r[3] = _mm_srli_si128(hi, 8);
}

void vp8i_idct_add(unsigned char *dst, int stride, const short *in)
{
    __m128i zero = _mm_setzero_si128(), r[4], a1, b1, c1, d1, out[4];
    int ac = 0;

    for (int i = 1; i < 16; i++)
        ac |= in[i];
    if (!ac) {
        /* No AC coefficients (none at all in a skipped macroblock): every pixel moves by the same amount. */
        int dc = (in[0] + 4) >> 3;
        if (dc) {
            __m128i d = _mm_set1_epi16((short)dc);
            for (int i = 0; i < 4; i++, dst += stride) {
                __m128i px = _mm_unpacklo_epi8(_mm_cvtsi32_si128(vp8i_load4(dst)), zero);
                int v = _mm_cvtsi128_si32(_mm_packus_epi16(_mm_add_epi16(px, d), zero));
                memcpy(dst, &v, 4);
            }
        }
        return;
    }
    /*
     * Down the columns, on the rows as vectors: the scalar code truncates
     * this pass to 16 bits, so 16-bit lanes wrap to the same values.
     */
    for (int i = 0; i < 4; i++)
        r[i] = _mm_loadl_epi64((const __m128i *)(in + 4 * i));
    a1 = _mm_add_epi16(r[0], r[2]);
    b1 = _mm_sub_epi16(r[0], r[2]);
    c1 = _mm_sub_epi16(mul_sin(r[1]), mul_cos(r[3]));
    d1 = _mm_add_epi16(mul_cos(r[1]), mul_sin(r[3]));
    r[0] = _mm_add_epi16(a1, d1);
    r[1] = _mm_add_epi16(b1, c1);
    r[2] = _mm_sub_epi16(b1, c1);
    r[3] = _mm_sub_epi16(a1, d1);
    /* Across the rows, in 32 bits: these sums may not fit 16 before (x + 4) >> 3. */
    transpose4(r);
    {
        __m128i x0 = widen(r[0]), x2 = widen(r[2]), four = _mm_set1_epi32(4);
        /* x + ((x * COS_M1) >> 16) reaches 42,814: that sum is made in 32 bits too. */
        __m128i cos1 = _mm_add_epi32(widen(r[1]), widen(_mm_mulhi_epi16(r[1], _mm_set1_epi16(COS_M1))));
        __m128i cos3 = _mm_add_epi32(widen(r[3]), widen(_mm_mulhi_epi16(r[3], _mm_set1_epi16(COS_M1))));
        a1 = _mm_add_epi32(x0, x2);
        b1 = _mm_sub_epi32(x0, x2);
        c1 = _mm_sub_epi32(widen(mul_sin(r[1])), cos3);
        d1 = _mm_add_epi32(cos1, widen(mul_sin(r[3])));
        out[0] = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(a1, d1), four), 3);
        out[1] = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(b1, c1), four), 3);
        out[2] = _mm_srai_epi32(_mm_add_epi32(_mm_sub_epi32(b1, c1), four), 3);
        out[3] = _mm_srai_epi32(_mm_add_epi32(_mm_sub_epi32(a1, d1), four), 3);
    }
    /* Lane i of out[j] is pixel j of row i: back to rows, added to the prediction and clamped. */
    for (int j = 0; j < 4; j++)
        out[j] = _mm_packs_epi32(out[j], zero);
    transpose4(out);
    for (int i = 0; i < 4; i++, dst += stride) {
        __m128i px = _mm_unpacklo_epi8(_mm_cvtsi32_si128(vp8i_load4(dst)), zero);
        int v = _mm_cvtsi128_si32(_mm_packus_epi16(_mm_add_epi16(px, out[i]), zero));
        memcpy(dst, &v, 4);
    }
}

void vp8i_predict_block(unsigned char *p, int stride, int n, int mode)
{
    switch (mode) {
    case DC_PRED:
        predict_dc(p, stride, n, n == 16 ? 5 : n == 8 ? 4 : 3);
        break;
    case V_PRED:
        for (int i = 0; i < n; i++)
            memcpy(p + i * stride, p - stride, (size_t)n);
        break;
    case H_PRED:
        for (int i = 0; i < n; i++)
            memset(p + i * stride, p[i * stride - 1], (size_t)n);
        break;
    default: { /* TM_PRED: left + above - corner, clamped by the pack */
        const unsigned char *above = p - stride;
        __m128i zero = _mm_setzero_si128(), a, lo, hi;
        if (n == 16)
            a = _mm_loadu_si128((const __m128i *)above);
        else if (n == 8)
            a = _mm_loadl_epi64((const __m128i *)above);
        else
            a = _mm_cvtsi32_si128(vp8i_load4(above));
        lo = _mm_unpacklo_epi8(a, zero);
        hi = _mm_unpackhi_epi8(a, zero);
        for (int i = 0; i < n; i++) {
            __m128i d = _mm_set1_epi16((short)(p[i * stride - 1] - above[-1]));
            __m128i row = _mm_packus_epi16(_mm_add_epi16(lo, d), _mm_add_epi16(hi, d));
            if (n == 16)
                _mm_storeu_si128((__m128i *)(p + i * stride), row);
            else
                store_px(p + i * stride, row, n);
        }
        break;
    }
    }
}

/* A 4x4 subblock: above[-1..7] and left[0..3] are its edges in the frame. */
void vp8i_predict_sub(unsigned char *p, int s, int mode)
{
    const unsigned char *A = p - s;
    int L[4] = {p[-1], p[s - 1], p[2 * s - 1], p[3 * s - 1]}, P = A[-1];
    unsigned char o[4][4];

    switch (mode) {
    case B_DC_PRED:
        predict_dc(p, s, 4, 3);
        return;
    case B_TM_PRED:
        vp8i_predict_block(p, s, 4, TM_PRED);
        return;
    case B_VE_PRED:
        for (int j = 0; j < 4; j++)
            o[0][j] = AVG3(A[j - 1], A[j], A[j + 1]);
        for (int i = 1; i < 4; i++)
            memcpy(o[i], o[0], 4);
        break;
    case B_HE_PRED:
        memset(o[0], AVG3(P, L[0], L[1]), 4);
        memset(o[1], AVG3(L[0], L[1], L[2]), 4);
        memset(o[2], AVG3(L[1], L[2], L[3]), 4);
        memset(o[3], AVG3(L[2], L[3], L[3]), 4);
        break;
    case B_LD_PRED:
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) {
                int k = i + j;
                o[i][j] = k < 6 ? AVG3(A[k], A[k + 1], A[k + 2]) : AVG3(A[6], A[7], A[7]);
            }
        break;
    case B_RD_PRED: {
        /* The edge from the bottom of the left column to the right end of the above row. */
        int e[9] = {L[3], L[2], L[1], L[0], P, A[0], A[1], A[2], A[3]};
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) {
                int k = 3 - i + j;
                o[i][j] = AVG3(e[k], e[k + 1], e[k + 2]);
            }
        break;
    }
    case B_VR_PRED: {
        int e[9] = {L[3], L[2], L[1], L[0], P, A[0], A[1], A[2], A[3]};
        o[3][0] = AVG3(e[1], e[2], e[3]);
        o[2][0] = AVG3(e[2], e[3], e[4]);
        o[3][1] = o[1][0] = AVG3(e[3], e[4], e[5]);
        o[2][1] = o[0][0] = AVG2(e[4], e[5]);
        o[3][2] = o[1][1] = AVG3(e[4], e[5], e[6]);
        o[2][2] = o[0][1] = AVG2(e[5], e[6]);
        o[3][3] = o[1][2] = AVG3(e[5], e[6], e[7]);
        o[2][3] = o[0][2] = AVG2(e[6], e[7]);
        o[1][3] = AVG3(e[6], e[7], e[8]);
        o[0][3] = AVG2(e[7], e[8]);
        break;
    }
    case B_VL_PRED:
        o[0][0] = AVG2(A[0], A[1]);
        o[1][0] = AVG3(A[0], A[1], A[2]);
        o[2][0] = o[0][1] = AVG2(A[1], A[2]);
        o[1][1] = o[3][0] = AVG3(A[1], A[2], A[3]);
        o[2][1] = o[0][2] = AVG2(A[2], A[3]);
        o[3][1] = o[1][2] = AVG3(A[2], A[3], A[4]);
        o[2][2] = o[0][3] = AVG2(A[3], A[4]);
        o[3][2] = o[1][3] = AVG3(A[3], A[4], A[5]);
        o[2][3] = AVG3(A[4], A[5], A[6]);
        o[3][3] = AVG3(A[5], A[6], A[7]);
        break;
    case B_HD_PRED: {
        int e[9] = {L[3], L[2], L[1], L[0], P, A[0], A[1], A[2], A[3]};
        o[3][0] = AVG2(e[0], e[1]);
        o[3][1] = AVG3(e[0], e[1], e[2]);
        o[2][0] = o[3][2] = AVG2(e[1], e[2]);
        o[2][1] = o[3][3] = AVG3(e[1], e[2], e[3]);
        o[2][2] = o[1][0] = AVG2(e[2], e[3]);
        o[2][3] = o[1][1] = AVG3(e[2], e[3], e[4]);
        o[1][2] = o[0][0] = AVG2(e[3], e[4]);
        o[1][3] = o[0][1] = AVG3(e[3], e[4], e[5]);
        o[0][2] = AVG3(e[4], e[5], e[6]);
        o[0][3] = AVG3(e[5], e[6], e[7]);
        break;
    }
    default: /* B_HU_PRED */
        o[0][0] = AVG2(L[0], L[1]);
        o[0][1] = AVG3(L[0], L[1], L[2]);
        o[0][2] = o[1][0] = AVG2(L[1], L[2]);
        o[0][3] = o[1][1] = AVG3(L[1], L[2], L[3]);
        o[1][2] = o[2][0] = AVG2(L[2], L[3]);
        o[1][3] = o[2][1] = AVG3(L[2], L[3], L[3]);
        o[2][2] = o[2][3] = o[3][0] = o[3][1] = o[3][2] = o[3][3] = (unsigned char)L[3];
        break;
    }
    for (int i = 0; i < 4; i++)
        memcpy(p + i * s, o[i], 4);
}

/*
 * The edges outside the frame: 127 above, 129 to the left, and for DC
 * prediction a copy of the other edge, which averages that edge alone.
 */
void vp8i_fixup_left(unsigned char *p, int stride, int n, int row, int mode)
{
    if (mode == DC_PRED && row) {
        for (int i = 0; i < n; i++)
            p[i * stride - 1] = p[-stride + i];
    } else {
        for (int i = -1; i < n; i++)
            p[i * stride - 1] = 129;
    }
}

void vp8i_fixup_above(unsigned char *p, int stride, int n, int col, int mode)
{
    if (mode == DC_PRED && col) {
        for (int i = 0; i < n; i++)
            p[-stride + i] = p[i * stride - 1];
    } else {
        memset(p - stride - 1, 127, (size_t)n + 1);
    }
    memset(p - stride + n, 127, 4);
}

int vp8i_mv_eq(vp8_mv_t a, vp8_mv_t b)
{
    return a.x == b.x && a.y == b.y;
}

int vp8i_mv_zero(vp8_mv_t a)
{
    return !a.x && !a.y;
}

vp8_mv_t vp8i_clamp_mv(vp8_mv_t mv, int left, int right, int top, int bottom)
{
    mv.x = (short)(mv.x < left ? left : mv.x > right ? right : mv.x);
    mv.y = (short)(mv.y < top ? top : mv.y > bottom ? bottom : mv.y);
    return mv;
}

/* A macroblock's vector for its chroma: halved, rounding away from zero; whole pixels only with `full_pixel`. */
vp8_mv_t vp8i_chroma_mv(vp8_mv_t mv, int full_pixel)
{
    mv.x = (short)(mv.x < 0 ? (mv.x - 1) / 2 : (mv.x + 1) / 2);
    mv.y = (short)(mv.y < 0 ? (mv.y - 1) / 2 : (mv.y + 1) / 2);
    if (full_pixel) {
        mv.x = (short)(mv.x & ~7);
        mv.y = (short)(mv.y & ~7);
    }
    return mv;
}

/* The neighbours' vectors (above, left, above-left), weighted into best, nearest and near (section 16.3). */
void vp8i_find_near_mvs(const int *sign_bias, const vp8_mb_t *m, const vp8_mb_t *above, const vp8_mb_t *left,
                        vp8_mv_t near_mvs[4],
                          int cnt[4])
{
    const vp8_mb_t *neighbours[3] = {above, left, above - 1};
    static const int weight[3] = {2, 2, 1};
    int n = 0; /* index of the last vector found */

    memset(near_mvs, 0, sizeof(vp8_mv_t) * 4);
    cnt[0] = cnt[1] = cnt[2] = cnt[3] = 0;
    for (int k = 0; k < 3; k++) {
        const vp8_mb_t *nb = neighbours[k];
        if (nb->ref == CURRENT)
            continue;
        if (!vp8i_mv_zero(nb->mv)) {
            vp8_mv_t mv = nb->mv;
            if (sign_bias[nb->ref] ^ sign_bias[m->ref]) {
                mv.x = (short)-mv.x;
                mv.y = (short)-mv.y;
            }
            if (k == 0 || !vp8i_mv_eq(mv, near_mvs[n]))
                near_mvs[++n] = mv;
            cnt[n] += weight[k];
        } else {
            cnt[0] += weight[k];
        }
    }
    /* Three distinct vectors: the above-left one may merge with nearest. */
    if (cnt[3] && vp8i_mv_eq(near_mvs[3], near_mvs[1]))
        cnt[1] += 1;
    cnt[3] = (above->y_mode == SPLITMV) * 2 + (left->y_mode == SPLITMV) * 2 + ((above - 1)->y_mode == SPLITMV);
    if (cnt[2] > cnt[1]) {
        int t = cnt[1];
        vp8_mv_t v = near_mvs[1];
        cnt[1] = cnt[2];
        cnt[2] = t;
        near_mvs[1] = near_mvs[2];
        near_mvs[2] = v;
    }
    if (cnt[1] >= cnt[0])
        near_mvs[0] = near_mvs[1];
}

/* Taps a and b of filter f, for _mm_madd_epi16 on pixel pairs. */
static __m128i tap_pair(const short *f, int a)
{
    return _mm_set1_epi32((int)((unsigned)(unsigned short)f[a] | (unsigned)(unsigned short)f[a + 1] << 16));
}


/*
 * One pass of a subpixel filter over bw x bh pixels (bw a multiple of 4);
 * `step` is 1 across a row, the stride down a column. Eight pixels at a
 * time, the taps summed in 32 bits (16 would overflow, and saturating
 * would round differently from the reference), then (sum + 64) >> 7
 * clamped to 0..255 by the packs. Reads up to 16 bytes from two pixels
 * left of each group: within the planes' borders and the scratch buffers.
 */
static void filter_pass(unsigned char *dst, int ds, const unsigned char *src, int ss, int step, int bw, int bh,
                        const short *f)
{
    const __m128i f01 = tap_pair(f, 0), f23 = tap_pair(f, 2), f45 = tap_pair(f, 4), zero = _mm_setzero_si128();
    const __m128i round = _mm_set1_epi32(64);

    for (int r = 0; r < bh; r++, dst += ds, src += ss)
        for (int c = 0; c < bw; c += 8) {
            __m128i lo, hi;
            if (step == 1) {
                /* Pixels 0, 2, 4, 6 and 1, 3, 5, 7 each take their taps from pairs of neighbours. */
                __m128i v = _mm_loadu_si128((const __m128i *)(src + c - 2));
                __m128i even = _mm_add_epi32(
                    _mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi8(v, zero), f01),
                                  _mm_madd_epi16(_mm_unpacklo_epi8(_mm_srli_si128(v, 2), zero), f23)),
                    _mm_madd_epi16(_mm_unpacklo_epi8(_mm_srli_si128(v, 4), zero), f45));
                __m128i odd = _mm_add_epi32(
                    _mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi8(_mm_srli_si128(v, 1), zero), f01),
                                  _mm_madd_epi16(_mm_unpacklo_epi8(_mm_srli_si128(v, 3), zero), f23)),
                    _mm_madd_epi16(_mm_unpacklo_epi8(_mm_srli_si128(v, 5), zero), f45));
                even = _mm_srai_epi32(_mm_add_epi32(even, round), 7);
                odd = _mm_srai_epi32(_mm_add_epi32(odd, round), 7);
                /* Back in order as 16-bit lanes: each result fits, the even ones in the low halves. */
                lo = _mm_or_si128(_mm_and_si128(even, _mm_set1_epi32(0xFFFF)), _mm_slli_epi32(odd, 16));
                store_px(dst + c, _mm_packus_epi16(lo, lo), bw - c);
            } else {
                const unsigned char *s = src + c;
                __m128i r0 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(s - 2 * step)), zero);
                __m128i r1 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(s - step)), zero);
                __m128i r2 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)s), zero);
                __m128i r3 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(s + step)), zero);
                __m128i r4 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(s + 2 * step)), zero);
                __m128i r5 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(s + 3 * step)), zero);
                lo = _mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(r0, r1), f01),
                                                 _mm_madd_epi16(_mm_unpacklo_epi16(r2, r3), f23)),
                                   _mm_madd_epi16(_mm_unpacklo_epi16(r4, r5), f45));
                hi = _mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(_mm_unpackhi_epi16(r0, r1), f01),
                                                 _mm_madd_epi16(_mm_unpackhi_epi16(r2, r3), f23)),
                                   _mm_madd_epi16(_mm_unpackhi_epi16(r4, r5), f45));
                lo = _mm_srai_epi32(_mm_add_epi32(lo, round), 7);
                hi = _mm_srai_epi32(_mm_add_epi32(hi, round), 7);
                lo = _mm_packs_epi32(lo, hi);
                store_px(dst + c, _mm_packus_epi16(lo, lo), bw - c);
            }
        }
}

static void extend_plane(unsigned char *p, int stride, int w, int h)
{
    int b = (stride - w) / 2;

    for (int r = 0; r < h; r++) {
        unsigned char *row = p + r * stride;
        memset(row - b, row[0], (size_t)b);
        memset(row + w, row[w - 1], (size_t)b);
    }
    for (int r = 1; r <= b; r++) {
        memcpy(p - r * stride - b, p - b, (size_t)stride);
        memcpy(p + (h - 1 + r) * stride - b, p + (h - 1) * stride - b, (size_t)stride);
    }
}

void vp8i_extend_borders(unsigned char *y, unsigned char *u, unsigned char *v, int stride, int uv_stride, int w,
                         int h)
{
    extend_plane(y, stride, w, h);
    extend_plane(u, uv_stride, w / 2, h / 2);
    extend_plane(v, uv_stride, w / 2, h / 2);
}

/*
 * A bw x bh block of plane `ref` (pw x ph, the macroblock-aligned size)
 * at (x, y) moved by mv, into dst. Pixels past the edges repeat the edge:
 * the extended borders hold them, and a block reaching past the borders
 * too is copied with its coordinates clamped. Fractional positions go
 * through the frame's six-tap or bilinear filters, horizontally then
 * vertically. A whole-pixel component's filter is {0, 0, 128, 0, 0, 0},
 * which returns its input: that pass is skipped.
 */
void vp8i_predict_inter_block(vp8i_scratch_t *d, unsigned char *dst, int ds, const unsigned char *ref, int rs, int pw,
                                int ph, int x, int y, int bw, int bh, vp8_mv_t mv)
{
    int mx = mv.x & 7, my = mv.y & 7, b = (rs - pw) / 2;
    const unsigned char *src;
    int ss;

    x += mv.x >> 3;
    y += mv.y >> 3;
    /* In place if the taps and filter_pass's wide reads (up to 16 bytes from x - 2) stay within the borders. */
    if (x - 2 < -b || y - 2 < -b || x + (bw < 8 ? 8 : bw) + 6 > pw + b || y + bh + 3 > ph + b) {
        for (int r = 0; r < bh + 5; r++) {
            int yy = y - 2 + r;
            const unsigned char *row = ref + (yy < 0 ? 0 : yy >= ph ? ph - 1 : yy) * rs;
            for (int c = 0; c < bw + 5; c++) {
                int xx = x - 2 + c;
                d->edge[r * 32 + c] = row[xx < 0 ? 0 : xx >= pw ? pw - 1 : xx];
            }
        }
        src = d->edge + 2 * 32 + 2;
        ss = 32;
    } else {
        src = ref + y * rs + x;
        ss = rs;
    }
    if (!(mx | my)) {
        for (int r = 0; r < bh; r++)
            memcpy(dst + r * ds, src + r * ss, (size_t)bw);
    } else if (!my) {
        filter_pass(dst, ds, src, ss, 1, bw, bh, d->filters[mx]);
    } else if (!mx) {
        filter_pass(dst, ds, src, ss, ss, bw, bh, d->filters[my]);
    } else {
        /* Two rows above and three below for the vertical taps. */
        filter_pass(d->pass, 16, src - 2 * ss, ss, 1, bw, bh + 5, d->filters[mx]);
        filter_pass(dst, ds, d->pass + 2 * 16, 16, 16, bw, bh, d->filters[my]);
    }
}
