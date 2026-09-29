#include <emmintrin.h>
#include <string.h>
#include "vp8_enc.h"
#include "vp8_int.h"
#include "vp8_tables.h"
#include "mem.h"

#define BORDER 32
#define SEARCH 64  /* how far the motion search goes, in pixels */
#define KEY_EVERY 300 /* frames between key frames, when none is asked for */

/* ---- Boolean entropy encoder (RFC 6386, section 7.3) ---- */

typedef struct {
    sb_t out;
    unsigned range, bottom;
    int bits;
} be_t;

static void be_init(be_t *e)
{
    sb_clear(&e->out);
    e->range = 255;
    e->bottom = 0;
    e->bits = 24;
}

static void be_byte(be_t *e, unsigned v)
{
    char c = (char)v;

    sb_addn(&e->out, &c, 1);
}

/* A carry into the bytes already written. */
static void be_carry(be_t *e)
{
    size_t i = e->out.len;

    while (i > 0 && (unsigned char)e->out.data[i - 1] == 255)
        e->out.data[--i] = 0;
    if (i > 0)
        e->out.data[i - 1]++;
}

static void be_put(be_t *e, int prob, int bit)
{
    unsigned split = 1 + (((e->range - 1) * (unsigned)prob) >> 8);

    if (bit) {
        e->bottom += split;
        e->range -= split;
    } else {
        e->range = split;
    }
    while (e->range < 128) {
        e->range <<= 1;
        if (e->bottom & (1u << 31))
            be_carry(e);
        e->bottom <<= 1;
        if (!--e->bits) {
            be_byte(e, e->bottom >> 24);
            e->bottom &= (1u << 24) - 1;
            e->bits = 8;
        }
    }
}

static void be_flush(be_t *e)
{
    int c = e->bits;
    unsigned v = e->bottom;

    if (v & (1u << (32 - c)))
        be_carry(e);
    v <<= c & 7;
    c >>= 3;
    while (--c >= 0)
        v <<= 8;
    for (c = 0; c < 4; c++, v <<= 8)
        be_byte(e, v >> 24);
}

static void be_lit(be_t *e, int n, unsigned v)
{
    while (n--)
        be_put(e, 128, (int)(v >> n & 1));
}

/* The path to leaf `v` of a tree, written with the tree's probabilities. */
static int tree_path(const signed char *tree, int i, int v, int *bits, int *nodes, int depth)
{
    for (int b = 0; b < 2; b++) {
        int next = tree[i + b], n;
        bits[depth] = b;
        nodes[depth] = i;
        if (next <= 0) {
            if (-next == v)
                return depth + 1;
        } else if ((n = tree_path(tree, next, v, bits, nodes, depth + 1)) > 0) {
            return n;
        }
    }
    return 0;
}

static void be_tree(be_t *e, const signed char *tree, const unsigned char *probs, int v)
{
    int bits[16], nodes[16], n = tree_path(tree, 0, v, bits, nodes, 0);

    for (int k = 0; k < n; k++)
        be_put(e, probs[nodes[k] >> 1], bits[k]);
}

/* ---- State ---- */

typedef struct {
    vp8_mv_t best;             /* NEWMV's reference */
    unsigned char probs[4];    /* the mode tree's probabilities here */
} side_t;

struct vp8_encoder {
    int w, h, mb_cols, mb_rows, stride, uv_stride, fps, kbps, frames, q, key;
    unsigned char *mem[2], *y[2], *u[2], *v[2]; /* reconstructions: [cur] being made, [!cur] the last */
    int cur;
    unsigned char *sy, *su, *sv; /* the source, padded to whole macroblocks */
    vp8_mb_t *mbs;
    side_t *side;
    unsigned char (*above_ctx)[9], left_ctx[9];
    short dq[3][2];             /* [Y, Y2, UV][DC, AC] */
    short coeffs[25 * 16];      /* dequantized, as the decoder will have them */
    short quant[25 * 16];       /* quantized */
    unsigned char pred[16 * 16];
    vp8i_scratch_t scratch;
    be_t modes, tokens;
    int skips, intras;
};

static vp8_mb_t *mb_at(vp8_encoder_t *e, int row, int col)
{
    return &e->mbs[(row + 1) * (e->mb_cols + 1) + col + 1];
}

vp8_encoder_t *vp8_encoder_new(int w, int h, int kbps, int fps)
{
    vp8_encoder_t *e;
    int rows;

    if (w <= 0 || h <= 0 || w > 16383 || h > 16383)
        return NULL;
    e = mem_alloc(sizeof *e);
    e->w = w;
    e->h = h;
    e->kbps = kbps > 0 ? kbps : 1000;
    e->fps = fps > 0 ? fps : 30;
    e->q = 40;
    e->mb_cols = (w + 15) / 16;
    e->mb_rows = (h + 15) / 16;
    e->stride = e->mb_cols * 16 + 2 * BORDER;
    e->uv_stride = e->mb_cols * 8 + BORDER;
    rows = e->mb_rows * 16 + 2 * BORDER;
    for (int i = 0; i < 2; i++) {
        size_t ys = (size_t)e->stride * (size_t)rows, uvs = (size_t)e->uv_stride * (size_t)(rows / 2);
        e->mem[i] = mem_alloc(ys + 2 * uvs);
        e->y[i] = e->mem[i] + BORDER * e->stride + BORDER;
        e->u[i] = e->mem[i] + ys + BORDER / 2 * e->uv_stride + BORDER / 2;
        e->v[i] = e->u[i] + uvs;
    }
    e->sy = mem_alloc((size_t)e->mb_cols * 16 * (size_t)e->mb_rows * 16 * 3 / 2);
    e->su = e->sy + (size_t)e->mb_cols * 16 * (size_t)e->mb_rows * 16;
    e->sv = e->su + (size_t)e->mb_cols * 8 * (size_t)e->mb_rows * 8;
    e->mbs = mem_alloc(sizeof(vp8_mb_t) * (size_t)(e->mb_cols + 1) * (size_t)(e->mb_rows + 1));
    e->side = mem_alloc(sizeof(side_t) * (size_t)e->mb_cols * (size_t)e->mb_rows);
    e->above_ctx = mem_alloc(9 * (size_t)e->mb_cols);
    e->scratch.filters = vp8_sixtap;
    return e;
}

void vp8_encoder_free(vp8_encoder_t *e)
{
    if (!e)
        return;
    mem_free(e->mem[0]);
    mem_free(e->mem[1]);
    mem_free(e->sy);
    mem_free(e->mbs);
    mem_free(e->side);
    mem_free(e->above_ctx);
    sb_free(&e->modes.out);
    sb_free(&e->tokens.out);
    mem_free(e);
}

void vp8_encoder_recon(const vp8_encoder_t *e, vp8_image_t *out)
{
    out->w = e->w;
    out->h = e->h;
    out->y = e->y[!e->cur];
    out->u = e->u[!e->cur];
    out->v = e->v[!e->cur];
    out->y_stride = e->stride;
    out->uv_stride = e->uv_stride;
}

/* The source picture, its edges repeated out to whole macroblocks. */
static void load_source(vp8_encoder_t *e, const vp8_image_t *img)
{
    int aw = e->mb_cols * 16, ah = e->mb_rows * 16;

    for (int y = 0; y < ah; y++) {
        const unsigned char *s = img->y + (y < img->h ? y : img->h - 1) * img->y_stride;
        unsigned char *d = e->sy + y * aw;
        memcpy(d, s, (size_t)img->w);
        memset(d + img->w, s[img->w - 1], (size_t)(aw - img->w));
    }
    for (int y = 0; y < ah / 2; y++) {
        int sy = y < (img->h + 1) / 2 ? y : (img->h + 1) / 2 - 1, cw = (img->w + 1) / 2;
        const unsigned char *su = img->u + sy * img->uv_stride, *sv = img->v + sy * img->uv_stride;
        unsigned char *du = e->su + y * aw / 2, *dv = e->sv + y * aw / 2;
        memcpy(du, su, (size_t)cw);
        memset(du + cw, su[cw - 1], (size_t)(aw / 2 - cw));
        memcpy(dv, sv, (size_t)cw);
        memset(dv + cw, sv[cw - 1], (size_t)(aw / 2 - cw));
    }
}

/* ---- Transforms and quantization ---- */

/*
 * Two 4x4 blocks side by side, eight 16-bit lanes: rows (or columns) 0-3
 * of the left block, then of the right one. Swaps rows and columns.
 */
static void transpose_pair(__m128i *m)
{
    __m128i a0 = _mm_unpacklo_epi16(m[0], m[1]), a1 = _mm_unpackhi_epi16(m[0], m[1]);
    __m128i a2 = _mm_unpacklo_epi16(m[2], m[3]), a3 = _mm_unpackhi_epi16(m[2], m[3]);
    __m128i b0 = _mm_unpacklo_epi32(a0, a2), b1 = _mm_unpackhi_epi32(a0, a2); /* left: 0 and 1, 2 and 3 */
    __m128i b2 = _mm_unpacklo_epi32(a1, a3), b3 = _mm_unpackhi_epi32(a1, a3); /* right */

    m[0] = _mm_unpacklo_epi64(b0, b2);
    m[1] = _mm_unpackhi_epi64(b0, b2);
    m[2] = _mm_unpacklo_epi64(b1, b3);
    m[3] = _mm_unpackhi_epi64(b1, b3);
}

/* (x * 2217 + y * 5352 + r) >> n, or y * -5352 (neg), in 32 bits and back. */
static __m128i rotate(__m128i x, __m128i y, int r, int n, int neg)
{
    __m128i k = _mm_set1_epi32((neg ? -5352 : 5352) * 65536 + 2217);
    __m128i lo = _mm_madd_epi16(_mm_unpacklo_epi16(x, y), k), hi = _mm_madd_epi16(_mm_unpackhi_epi16(x, y), k);
    __m128i round = _mm_set1_epi32(r);

    return _mm_packs_epi32(_mm_srai_epi32(_mm_add_epi32(lo, round), n), _mm_srai_epi32(_mm_add_epi32(hi, round), n));
}

/*
 * The forward DCT of two blocks side by side: m[r] holds row r of both
 * (residuals, -255..255), out gets the left block's 16 coefficients, then
 * the right one's. Every sum fits 16 bits: the largest, 8 times the sum of
 * a block's 16 residuals, is 32,640; the products take 32.
 */
static void fdct_pair(__m128i *m, short *out)
{
    __m128i a1, b1, c1, d1;

    transpose_pair(m); /* m[k]: column k */
    a1 = _mm_slli_epi16(_mm_add_epi16(m[0], m[3]), 3);
    b1 = _mm_slli_epi16(_mm_add_epi16(m[1], m[2]), 3);
    c1 = _mm_slli_epi16(_mm_sub_epi16(m[1], m[2]), 3);
    d1 = _mm_slli_epi16(_mm_sub_epi16(m[0], m[3]), 3);
    m[0] = _mm_add_epi16(a1, b1);
    m[1] = rotate(c1, d1, 14500, 12, 0);
    m[2] = _mm_sub_epi16(a1, b1);
    m[3] = rotate(d1, c1, 7500, 12, 1);
    transpose_pair(m); /* back to rows */
    a1 = _mm_add_epi16(m[0], m[3]);
    b1 = _mm_add_epi16(m[1], m[2]);
    c1 = _mm_sub_epi16(m[1], m[2]);
    d1 = _mm_sub_epi16(m[0], m[3]);
    m[0] = _mm_srai_epi16(_mm_add_epi16(_mm_add_epi16(a1, b1), _mm_set1_epi16(7)), 4);
    /* + (d1 != 0): + 1, then - 1 where d1 is 0 */
    m[1] = _mm_add_epi16(_mm_add_epi16(rotate(c1, d1, 12000, 16, 0), _mm_set1_epi16(1)),
                         _mm_cmpeq_epi16(d1, _mm_setzero_si128()));
    m[2] = _mm_srai_epi16(_mm_add_epi16(_mm_sub_epi16(a1, b1), _mm_set1_epi16(7)), 4);
    m[3] = rotate(d1, c1, 51000, 16, 1);
    for (int r = 0; r < 4; r++) {
        _mm_storel_epi64((__m128i *)(out + 4 * r), m[r]);
        _mm_storel_epi64((__m128i *)(out + 16 + 4 * r), _mm_unpackhi_epi64(m[r], m[r]));
    }
}

/* The forward Walsh-Hadamard transform of the 16 Y blocks' DC coefficients. */
static void fwht(const short *in, short *out)
{
    int t[16];

    for (int i = 0; i < 4; i++) {
        const short *p = in + 4 * i;
        int a1 = (p[0] + p[2]) * 4, d1 = (p[1] + p[3]) * 4, c1 = (p[1] - p[3]) * 4, b1 = (p[0] - p[2]) * 4;
        t[4 * i] = a1 + d1 + (a1 != 0);
        t[4 * i + 1] = b1 + c1;
        t[4 * i + 2] = b1 - c1;
        t[4 * i + 3] = a1 - d1;
    }
    for (int i = 0; i < 4; i++) {
        int a1 = t[i] + t[8 + i], d1 = t[4 + i] + t[12 + i], c1 = t[4 + i] - t[12 + i], b1 = t[i] - t[8 + i];
        int a2 = a1 + d1, b2 = b1 + c1, c2 = b1 - c1, d2 = a1 - d1;
        a2 += a2 < 0;
        b2 += b2 < 0;
        c2 += c2 < 0;
        d2 += d2 < 0;
        out[i] = (short)((a2 + 3) >> 3);
        out[4 + i] = (short)((b2 + 3) >> 3);
        out[8 + i] = (short)((c2 + 3) >> 3);
        out[12 + i] = (short)((d2 + 3) >> 3);
    }
}

/* Quantizes a block from `first`; `deq` gets what decoders will multiply back. Returns whether any is nonzero. */
static int quantize(const short *in, short *q, short *deq, int first, const short *dq)
{
    /*
     * Eight coefficients at a time in 16 bits. n = |c| plus the rounding is
     * below 2^16; mulhi(n, 2^16 / step) is the quotient n / step or one less
     * (its error is below n / 2^16 < 1), which the remainder corrects.
     */
    short ac_round = (short)(dq[1] * 3 / 8), ac_inv = (short)(65536 / dq[1]);
    const __m128i steps[2] = {_mm_setr_epi16(dq[0], dq[1], dq[1], dq[1], dq[1], dq[1], dq[1], dq[1]),
                              _mm_set1_epi16(dq[1])};
    const __m128i inv[2] = {_mm_setr_epi16((short)(65536 / dq[0]), ac_inv, ac_inv, ac_inv, ac_inv, ac_inv, ac_inv,
                                           ac_inv),
                            _mm_set1_epi16(ac_inv)};
    /* A dead zone on the AC coefficients saves bits where they matter least. */
    const __m128i round[2] = {_mm_setr_epi16((short)(dq[0] / 2), ac_round, ac_round, ac_round, ac_round, ac_round,
                                             ac_round, ac_round),
                              _mm_set1_epi16(ac_round)};
    const __m128i one = _mm_set1_epi16(1), max = _mm_set1_epi16(2048 + 66), zero = _mm_setzero_si128();
    /* With `first`, coefficient 0 is the Y2 block's: left as it is. */
    const __m128i keep = _mm_setr_epi16((short)-first, 0, 0, 0, 0, 0, 0, 0);
    __m128i any = zero;

    for (int h = 0; h < 2; h++) {
        __m128i c = _mm_loadu_si128((const __m128i *)(in + 8 * h)), sign = _mm_srai_epi16(c, 15);
        __m128i n = _mm_add_epi16(_mm_sub_epi16(_mm_xor_si128(c, sign), sign), round[h]);
        __m128i v = _mm_mulhi_epu16(n, inv[h]), rem = _mm_sub_epi16(n, _mm_mullo_epi16(v, steps[h]));
        v = _mm_sub_epi16(v, _mm_cmpgt_epi16(rem, _mm_sub_epi16(steps[h], one)));
        v = _mm_sub_epi16(_mm_xor_si128(_mm_min_epi16(v, max), sign), sign);
        if (h == 0) {
            v = _mm_andnot_si128(keep, v);
            any = v;
            v = _mm_or_si128(v, _mm_and_si128(keep, _mm_loadu_si128((const __m128i *)q)));
            _mm_storeu_si128((__m128i *)deq, _mm_or_si128(_mm_andnot_si128(keep, _mm_mullo_epi16(v, steps[0])),
                                                          _mm_and_si128(keep, _mm_loadu_si128((const __m128i *)deq))));
        } else {
            any = _mm_or_si128(any, v);
            _mm_storeu_si128((__m128i *)(deq + 8), _mm_mullo_epi16(v, steps[1]));
        }
        _mm_storeu_si128((__m128i *)(q + 8 * h), v);
    }
    return _mm_movemask_epi8(_mm_cmpeq_epi16(any, zero)) != 0xFFFF;
}

/* ---- Tokens (the inverse of the decoder's reading) ---- */

static void put_extra(be_t *e, const unsigned char *probs, int v, int n)
{
    for (int i = 0; i < n; i++)
        be_put(e, probs[i], v >> (n - 1 - i) & 1);
}

static int write_block(be_t *e, const unsigned char (*probs)[3][11], int ctx, int first, const short *q)
{
    int last = -1, c = first;
    const unsigned char *p = probs[vp8_bands[first]][ctx];

    for (int i = first; i < 16; i++)
        if (q[vp8_zigzag[i]])
            last = i;
    be_put(e, p[0], last >= 0);
    if (last < 0)
        return 0;
    for (;;) {
        int v = q[vp8_zigzag[c]], a = v < 0 ? -v : v, next;
        if (!a) {
            be_put(e, p[1], 0);
            p = probs[vp8_bands[++c]][0];
            continue;
        }
        be_put(e, p[1], 1);
        if (a == 1) {
            be_put(e, p[2], 0);
            next = 1;
        } else {
            be_put(e, p[2], 1);
            if (a <= 4) {
                be_put(e, p[3], 0);
                be_put(e, p[4], a != 2);
                if (a != 2)
                    be_put(e, p[5], a == 4);
            } else if (a <= 10) {
                be_put(e, p[3], 1);
                be_put(e, p[6], 0);
                be_put(e, p[7], a > 6);
                if (a <= 6)
                    be_put(e, 159, a - 5);
                else
                    put_extra(e, (const unsigned char[]){165, 145}, a - 7, 2);
            } else {
                be_put(e, p[3], 1);
                be_put(e, p[6], 1);
                be_put(e, p[8], a > 34);
                if (a <= 18) {
                    be_put(e, p[9], 0);
                    put_extra(e, vp8_cat3, a - 11, 3);
                } else if (a <= 34) {
                    be_put(e, p[9], 1);
                    put_extra(e, vp8_cat4, a - 19, 4);
                } else if (a <= 66) {
                    be_put(e, p[10], 0);
                    put_extra(e, vp8_cat5, a - 35, 5);
                } else {
                    be_put(e, p[10], 1);
                    put_extra(e, vp8_cat6, a - 67, 11);
                }
            }
            next = 2;
        }
        be_put(e, 128, v < 0);
        if (++c == 16)
            break;
        p = probs[vp8_bands[c]][next];
        be_put(e, p[0], c <= last);
        if (c > last)
            break;
    }
    return 1;
}

/* ---- Macroblocks ---- */

/* The sum of absolute differences of two w x h blocks (w is 16 or 8), a row per SSE2 instruction. */
static int sad(const unsigned char *a, int as, const unsigned char *b, int bs, int w, int h)
{
    __m128i s = _mm_setzero_si128();

    for (int y = 0; y < h; y++, a += as, b += bs) {
        __m128i ra = w == 16 ? _mm_loadu_si128((const __m128i *)a) : _mm_loadl_epi64((const __m128i *)a);
        __m128i rb = w == 16 ? _mm_loadu_si128((const __m128i *)b) : _mm_loadl_epi64((const __m128i *)b);
        s = _mm_add_epi64(s, _mm_sad_epu8(ra, rb));
    }
    return _mm_cvtsi128_si32(s) + _mm_cvtsi128_si32(_mm_srli_si128(s, 8));
}

/* The luma SAD of moving by mv from the last frame. */
static int inter_sad(vp8_encoder_t *e, int row, int col, vp8_mv_t mv)
{
    int aw = e->mb_cols * 16, ah = e->mb_rows * 16, x = col * 16, y = row * 16;
    int rx = x + (mv.x >> 3), ry = y + (mv.y >> 3);

    /* Whole pixels inside the frame or its extended borders: compare in place. */
    if (!(mv.x & 7) && !(mv.y & 7) && rx >= -BORDER && ry >= -BORDER && rx + 16 <= aw + BORDER &&
        ry + 16 <= ah + BORDER)
        return sad(e->y[!e->cur] + ry * e->stride + rx, e->stride, e->sy + y * aw + x, aw, 16, 16);
    vp8i_predict_inter_block(&e->scratch, e->pred, 16, e->y[!e->cur], e->stride, aw, e->mb_rows * 16, x, y, 16, 16, mv);
    return sad(e->pred, 16, e->sy + y * aw + x, aw, 16, 16);
}

static int mv_bits(vp8_mv_t mv, vp8_mv_t best)
{
    int bits = 2;

    for (int k = 0; k < 2; k++) {
        int d = (k ? mv.x - best.x : mv.y - best.y) / 2;
        d = d < 0 ? -d : d;
        while (d) {
            bits += 2;
            d >>= 1;
        }
    }
    return bits;
}

/* A diamond search around `start` (whole pixels), then half- and quarter-pixel refinement. */
static vp8_mv_t search(vp8_encoder_t *e, int row, int col, vp8_mv_t start, int *cost, vp8_mv_t best, int lambda)
{
    static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    vp8_mv_t mv = start, c;
    int s = inter_sad(e, row, col, mv) + lambda * mv_bits(mv, best);

    for (int step = 8 * 8; step >= 2; step >>= 1)
        for (int moved = 1; moved;) {
            moved = 0;
            for (int k = 0; k < 4; k++) {
                int cs;
                c.x = (short)(mv.x + dx[k] * step);
                c.y = (short)(mv.y + dy[k] * step);
                if (c.x < -SEARCH * 8 - col * 128 || c.x > SEARCH * 8 + (e->mb_cols - col) * 128 ||
                    c.y < -SEARCH * 8 - row * 128 || c.y > SEARCH * 8 + (e->mb_rows - row) * 128)
                    continue;
                cs = inter_sad(e, row, col, c) + lambda * mv_bits(c, best);
                if (cs < s) {
                    s = cs;
                    mv = c;
                    moved = step >= 8; /* whole-pixel steps keep walking; fractional ones look once */
                }
            }
        }
    *cost = s;
    return mv;
}

static int intra_luma(vp8_encoder_t *e, int row, int col, unsigned char *y, const unsigned char *src, int aw)
{
    int best = -1, mode = DC_PRED;

    for (int m = DC_PRED; m <= TM_PRED; m++) {
        int s;
        if (col == 0)
            vp8i_fixup_left(y, e->stride, 16, row, m);
        if (row == 0)
            vp8i_fixup_above(y, e->stride, 16, col, m);
        vp8i_predict_block(y, e->stride, 16, m);
        s = sad(y, e->stride, src, aw, 16, 16);
        if (best < 0 || s < best) {
            best = s;
            mode = m;
        }
    }
    return mode | best << 3;
}

static int intra_chroma(vp8_encoder_t *e, int row, int col, unsigned char *u, unsigned char *v, const unsigned char *su,
                        const unsigned char *sv, int aw)
{
    int best = -1, mode = DC_PRED;

    for (int m = DC_PRED; m <= TM_PRED; m++) {
        int s;
        if (col == 0) {
            vp8i_fixup_left(u, e->uv_stride, 8, row, m);
            vp8i_fixup_left(v, e->uv_stride, 8, row, m);
        }
        if (row == 0) {
            vp8i_fixup_above(u, e->uv_stride, 8, col, m);
            vp8i_fixup_above(v, e->uv_stride, 8, col, m);
        }
        vp8i_predict_block(u, e->uv_stride, 8, m);
        vp8i_predict_block(v, e->uv_stride, 8, m);
        s = sad(u, e->uv_stride, su, aw / 2, 8, 8) + sad(v, e->uv_stride, sv, aw / 2, 8, 8);
        if (best < 0 || s < best) {
            best = s;
            mode = m;
        }
    }
    return mode;
}

/* The residual of the predicted macroblock (already in the frame): transformed, quantized, reconstructed. */
static int code_residual(vp8_encoder_t *e, unsigned char *y, unsigned char *u, unsigned char *v, const unsigned char *sy,
                         const unsigned char *su, const unsigned char *sv, int aw)
{
    short f[32], dc[16];
    int any = 0;

    /* Nothing to clear: quantize() writes each block from `first`, vp8i_iwht() the Y blocks' dequantized DC;
       their quantized DC is never read (Y2 codes it). Blocks go by pairs side by side. */
    for (int b = 0; b < 24; b += 2) {
        const unsigned char *s, *p;
        int ss, ps;
        __m128i m[4], zero = _mm_setzero_si128();
        if (b < 16) {
            s = sy + (b >> 2) * 4 * aw + (b & 3) * 4;
            p = y + (b >> 2) * 4 * e->stride + (b & 3) * 4;
            ss = aw;
            ps = e->stride;
        } else {
            int k = (b - 16) & 3;
            s = (b < 20 ? su : sv) + (k >> 1) * 4 * (aw / 2);
            p = (b < 20 ? u : v) + (k >> 1) * 4 * e->uv_stride;
            ss = aw / 2;
            ps = e->uv_stride;
        }
        for (int r = 0; r < 4; r++)
            m[r] = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(s + r * ss)), zero),
                                 _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(p + r * ps)), zero));
        fdct_pair(m, f);
        for (int k = 0; k < 2; k++) {
            int i = b + k;
            if (i < 16) {
                dc[i] = f[16 * k];
                any |= quantize(f + 16 * k, e->quant + i * 16, e->coeffs + i * 16, 1, e->dq[0]);
            } else {
                any |= quantize(f + 16 * k, e->quant + i * 16, e->coeffs + i * 16, 0, e->dq[2]);
            }
        }
    }
    fwht(dc, f);
    any |= quantize(f, e->quant + 24 * 16, e->coeffs + 24 * 16, 0, e->dq[1]);
    /* What the decoder does with them. */
    vp8i_iwht(e->coeffs);
    for (int i = 0; i < 16; i++)
        vp8i_idct_add(y + (i >> 2) * 4 * e->stride + (i & 3) * 4, e->stride, e->coeffs + i * 16);
    for (int i = 0; i < 4; i++) {
        vp8i_idct_add(u + (i >> 1) * 4 * e->uv_stride + (i & 1) * 4, e->uv_stride, e->coeffs + (16 + i) * 16);
        vp8i_idct_add(v + (i >> 1) * 4 * e->uv_stride + (i & 1) * 4, e->uv_stride, e->coeffs + (20 + i) * 16);
    }
    return any;
}

static void write_tokens(vp8_encoder_t *e, unsigned char *above, unsigned char *left)
{
    int t = write_block(&e->tokens, vp8_default_coeff_probs[TYPE_Y2], above[8] + left[8], 0, e->quant + 24 * 16);

    above[8] = left[8] = (unsigned char)t;
    for (int i = 0; i < 16; i++) {
        t = write_block(&e->tokens, vp8_default_coeff_probs[TYPE_Y_AFTER_Y2], above[i & 3] + left[i >> 2], 1,
                        e->quant + i * 16);
        above[i & 3] = left[i >> 2] = (unsigned char)t;
    }
    for (int i = 16; i < 24; i++) {
        int a = 4 + ((i - 16) >> 2) * 2 + (i & 1), l = 4 + ((i - 16) >> 2) * 2 + ((i >> 1) & 1);
        t = write_block(&e->tokens, vp8_default_coeff_probs[TYPE_UV], above[a] + left[l], 0, e->quant + i * 16);
        above[a] = left[l] = (unsigned char)t;
    }
}

static void encode_mb(vp8_encoder_t *e, int row, int col)
{
    int aw = e->mb_cols * 16, s = e->stride, us = e->uv_stride;
    vp8_mb_t *m = mb_at(e, row, col);
    side_t *side = &e->side[row * e->mb_cols + col];
    unsigned char *y = e->y[e->cur] + row * 16 * s + col * 16, *u = e->u[e->cur] + row * 8 * us + col * 8,
                  *v = e->v[e->cur] + row * 8 * us + col * 8;
    const unsigned char *sy = e->sy + row * 16 * aw + col * 16, *su = e->su + row * 8 * (aw / 2) + col * 8,
                        *sv = e->sv + row * 8 * (aw / 2) + col * 8;
    int lambda = e->dq[0][1] / 4 + 1, intra = intra_luma(e, row, col, y, sy, aw), inter_cost = 0x7FFFFFFF;

    memset(m, 0, sizeof *m);
    if (!e->key) {
        vp8_mv_t near_mvs[4], cand[3], mv;
        int cnt[4], cost, to_left = -((col + 1) << 7), to_right = (e->mb_cols - col) << 7;
        int to_top = -((row + 1) << 7), to_bottom = (e->mb_rows - row) << 7;
        m->ref = LAST;
        vp8i_find_near_mvs((const int[4]){0, 0, 0, 0}, m, mb_at(e, row - 1, col), m - 1, near_mvs, cnt);
        for (int i = 0; i < 4; i++)
            side->probs[i] = vp8_mv_counts_to_probs[cnt[i]][i];
        side->best = vp8i_clamp_mv(near_mvs[0], to_left, to_right, to_top, to_bottom);
        cand[0].x = cand[0].y = 0;
        cand[1] = vp8i_clamp_mv(near_mvs[1], to_left, to_right, to_top, to_bottom);
        cand[2] = vp8i_clamp_mv(near_mvs[2], to_left, to_right, to_top, to_bottom);
        /* The free vectors first: zero, nearest and near cost only their mode. */
        for (int k = 0; k < 3; k++) {
            int c = inter_sad(e, row, col, cand[k]) + lambda * (1 + k);
            if (c < inter_cost) {
                inter_cost = c;
                m->y_mode = (unsigned char)(k == 0 ? ZEROMV : k == 1 ? NEARESTMV : NEARMV);
                m->mv = cand[k];
            }
        }
        mv.x = (short)(cand[1].x & ~7);
        mv.y = (short)(cand[1].y & ~7);
        mv = search(e, row, col, mv, &cost, side->best, lambda);
        cost += lambda * 4;
        if (cost < inter_cost && !vp8i_mv_eq(mv, cand[0]) && !vp8i_mv_eq(mv, cand[1]) && !vp8i_mv_eq(mv, cand[2])) {
            inter_cost = cost;
            m->y_mode = NEWMV;
            m->mv = mv;
        }
    }
    if (e->key || (intra >> 3) + lambda * 12 < inter_cost) {
        m->ref = CURRENT;
        m->y_mode = (unsigned char)(intra & 7);
        m->mv.x = m->mv.y = 0;
        if (col == 0)
            vp8i_fixup_left(y, s, 16, row, m->y_mode);
        if (row == 0)
            vp8i_fixup_above(y, s, 16, col, m->y_mode);
        vp8i_predict_block(y, s, 16, m->y_mode);
        m->uv_mode = (unsigned char)intra_chroma(e, row, col, u, v, su, sv, aw);
        if (col == 0) {
            vp8i_fixup_left(u, us, 8, row, m->uv_mode);
            vp8i_fixup_left(v, us, 8, row, m->uv_mode);
        }
        if (row == 0) {
            vp8i_fixup_above(u, us, 8, col, m->uv_mode);
            vp8i_fixup_above(v, us, 8, col, m->uv_mode);
        }
        vp8i_predict_block(u, us, 8, m->uv_mode);
        vp8i_predict_block(v, us, 8, m->uv_mode);
        e->intras++;
    } else {
        vp8_mv_t uv = vp8i_chroma_mv(m->mv, 0);
        int pw = e->mb_cols * 16, ph = e->mb_rows * 16;
        m->uv_mode = m->y_mode;
        /* The decoder fixes the edges up for every macroblock, whatever its mode. */
        if (col == 0) {
            vp8i_fixup_left(y, s, 16, row, m->y_mode);
            vp8i_fixup_left(u, us, 8, row, m->uv_mode);
            vp8i_fixup_left(v, us, 8, row, m->uv_mode);
        }
        if (row == 0) {
            vp8i_fixup_above(y, s, 16, col, m->y_mode);
            vp8i_fixup_above(u, us, 8, col, m->uv_mode);
            vp8i_fixup_above(v, us, 8, col, m->uv_mode);
        }
        vp8i_predict_inter_block(&e->scratch, y, s, e->y[!e->cur], s, pw, ph, col * 16, row * 16, 16, 16, m->mv);
        for (int i = 0; i < 4; i++) {
            int bx = (i & 1) * 4, by = (i >> 1) * 4;
            vp8i_predict_inter_block(&e->scratch, u + by * us + bx, us, e->u[!e->cur], us, pw / 2, ph / 2,
                                     col * 8 + bx, row * 8 + by, 4, 4, uv);
            vp8i_predict_inter_block(&e->scratch, v + by * us + bx, us, e->v[!e->cur], us, pw / 2, ph / 2,
                                     col * 8 + bx, row * 8 + by, 4, 4, uv);
        }
    }
    m->skip = (unsigned char)!code_residual(e, y, u, v, sy, su, sv, aw);
    if (m->skip) {
        memset(e->above_ctx[col], 0, 9);
        memset(e->left_ctx, 0, 9);
        e->skips++;
    } else {
        write_tokens(e, e->above_ctx[col], e->left_ctx);
    }
}

static void write_mv_component(be_t *b, const unsigned char *p, int v)
{
    enum { IS_SHORT, SIGN, SHORT, LONG = 9 };
    int x = (v < 0 ? -v : v) / 2;

    if (x < 8) {
        be_put(b, p[IS_SHORT], 0);
        be_tree(b, vp8_small_mv_tree, p + SHORT, x);
    } else {
        be_put(b, p[IS_SHORT], 1);
        for (int i = 0; i < 3; i++)
            be_put(b, p[LONG + i], x >> i & 1);
        for (int i = 9; i > 3; i--)
            be_put(b, p[LONG + i], x >> i & 1);
        if (x & 0xFFF0)
            be_put(b, p[LONG + 3], x >> 3 & 1);
    }
    if (x)
        be_put(b, p[SIGN], v < 0);
}

static void write_modes(vp8_encoder_t *e, int prob_skip, int prob_intra)
{
    be_t *b = &e->modes;

    for (int row = 0; row < e->mb_rows; row++)
        for (int col = 0; col < e->mb_cols; col++) {
            const vp8_mb_t *m = mb_at(e, row, col);
            const side_t *side = &e->side[row * e->mb_cols + col];
            be_put(b, prob_skip, m->skip);
            if (e->key) {
                be_tree(b, vp8_kf_y_mode_tree, vp8_kf_y_mode_probs, m->y_mode);
                be_tree(b, vp8_uv_mode_tree, vp8_kf_uv_mode_probs, m->uv_mode);
                continue;
            }
            be_put(b, prob_intra, m->ref != CURRENT);
            if (m->ref == CURRENT) {
                be_tree(b, vp8_y_mode_tree, vp8_default_y_mode_probs, m->y_mode);
                be_tree(b, vp8_uv_mode_tree, vp8_default_uv_mode_probs, m->uv_mode);
                continue;
            }
            be_put(b, 255, 0); /* the last frame */
            be_tree(b, vp8_mv_ref_tree, side->probs, m->y_mode);
            if (m->y_mode == NEWMV) {
                write_mv_component(b, vp8_default_mv_probs[0], m->mv.y - side->best.y);
                write_mv_component(b, vp8_default_mv_probs[1], m->mv.x - side->best.x);
            }
        }
}

static int clamp_prob(int p)
{
    return p < 1 ? 1 : p > 255 ? 255 : p;
}

int vp8_encode(vp8_encoder_t *e, const vp8_image_t *img, int key, sb_t *out)
{
    int total = e->mb_cols * e->mb_rows, prob_skip, prob_intra = 128;
    be_t *h = &e->modes;
    size_t first, target;
    unsigned tag;

    if (img->w != e->w || img->h != e->h)
        return 0;
    e->key = key || e->frames % KEY_EVERY == 0;
    load_source(e, img);
    e->dq[0][0] = vp8_dc_q[e->q];
    e->dq[0][1] = vp8_ac_q[e->q];
    e->dq[1][0] = (short)(vp8_dc_q[e->q] * 2);
    e->dq[1][1] = (short)(vp8_ac_q[e->q] * 155 / 100 < 8 ? 8 : vp8_ac_q[e->q] * 155 / 100);
    e->dq[2][0] = (short)(vp8_dc_q[e->q] > 132 ? 132 : vp8_dc_q[e->q]);
    e->dq[2][1] = vp8_ac_q[e->q];

    be_init(&e->tokens);
    e->skips = e->intras = 0;
    memset(e->above_ctx, 0, 9 * (size_t)e->mb_cols);
    for (int row = 0; row < e->mb_rows; row++) {
        memset(e->left_ctx, 0, sizeof e->left_ctx);
        for (int col = 0; col < e->mb_cols; col++)
            encode_mb(e, row, col);
        /* past the right edge, as the decoder extends it */
        {
            unsigned char *y = e->y[e->cur] + row * 16 * e->stride + e->mb_cols * 16;
            memset(y + 15 * e->stride, y[15 * e->stride - 1], 4);
        }
    }
    be_flush(&e->tokens);
    vp8i_extend_borders(e->y[e->cur], e->u[e->cur], e->v[e->cur], e->stride, e->uv_stride, e->mb_cols * 16,
                        e->mb_rows * 16);

    /* The header and modes, now that the counts are known. */
    prob_skip = clamp_prob((total - e->skips) * 256 / total);
    if (!e->key)
        prob_intra = clamp_prob(e->intras * 256 / total);
    be_init(h);
    if (e->key)
        be_lit(h, 2, 0);  /* color space, clamping */
    be_lit(h, 1, 0);      /* no segmentation */
    be_lit(h, 1, 0);      /* normal loop filter */
    be_lit(h, 6, 0);      /* ... at level 0: off */
    be_lit(h, 3, 0);      /* sharpness */
    be_lit(h, 1, 0);      /* no filter adjustments */
    be_lit(h, 2, 0);      /* one token partition */
    be_lit(h, 7, (unsigned)e->q);
    be_lit(h, 5, 0);      /* no quantizer deltas */
    if (e->key) {
        be_lit(h, 1, 1);  /* refresh the probabilities */
    } else {
        be_lit(h, 2, 0);  /* no golden or altref refresh */
        be_lit(h, 4, 0);  /* no copies */
        be_lit(h, 2, 0);  /* sign biases */
        be_lit(h, 1, 1);  /* refresh the probabilities */
        be_lit(h, 1, 1);  /* refresh the last frame */
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            for (int k = 0; k < 3; k++)
                for (int l = 0; l < 11; l++)
                    be_put(h, vp8_coeff_update_probs[i][j][k][l], 0);
    be_lit(h, 1, 1); /* skipped macroblocks are flagged */
    be_lit(h, 8, (unsigned)prob_skip);
    if (!e->key) {
        be_lit(h, 8, (unsigned)prob_intra);
        be_lit(h, 8, 255); /* always the last frame */
        be_lit(h, 8, 128);
        be_lit(h, 2, 0);   /* no mode probability updates */
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 19; j++)
                be_put(h, vp8_mv_update_probs[i][j], 0);
    }
    write_modes(e, prob_skip, prob_intra);
    be_flush(h);

    first = h->out.len;
    tag = (e->key ? 0 : 1) | 0 << 1 | 1 << 4 | (unsigned)first << 5;
    sb_clear(out);
    {
        unsigned char head[10] = {(unsigned char)tag, (unsigned char)(tag >> 8), (unsigned char)(tag >> 16),
                                  0x9d, 0x01, 0x2a, (unsigned char)e->w, (unsigned char)(e->w >> 8),
                                  (unsigned char)e->h, (unsigned char)(e->h >> 8)};
        sb_addn(out, (const char *)head, e->key ? 10 : 3);
    }
    sb_addn(out, h->out.data, h->out.len);
    sb_addn(out, e->tokens.out.data, e->tokens.out.len);

    /* Steer the quantizer toward the bitrate (key frames may take more). */
    target = (size_t)e->kbps * 1000 / 8 / (size_t)e->fps;
    if (e->key)
        target *= 4;
    if (out->len > target + target / 4)
        e->q += out->len > 2 * target ? 4 : 1;
    else if (out->len < target - target / 4)
        e->q--;
    e->q = e->q < 4 ? 4 : e->q > 127 ? 127 : e->q;
    e->cur = !e->cur;
    e->frames++;
    return 1;
}
