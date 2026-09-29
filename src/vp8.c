#include <emmintrin.h>
#include <string.h>
#include "vp8_int.h"
#include "vp8_tables.h"
#include "mem.h"

#define BORDER 32 /* around the luma planes; half of it around chroma */

/* ---- Boolean entropy decoder (section 7) ---- */

typedef struct {
    const unsigned char *p, *end;
    unsigned range, value;
    int bits;
} bd_t;

static void bd_init(bd_t *d, const unsigned char *p, size_t n)
{
    d->p = p;
    d->end = p + n;
    d->value = 0;
    for (int i = 0; i < 2; i++)
        d->value = d->value << 8 | (d->p < d->end ? *d->p++ : 0);
    d->range = 255;
    d->bits = 0;
}

static int bd_get(bd_t *d, int prob)
{
    unsigned split = 1 + (((d->range - 1) * (unsigned)prob) >> 8), big = split << 8;
    int bit;

    if (d->value >= big) {
        bit = 1;
        d->range -= split;
        d->value -= big;
    } else {
        bit = 0;
        d->range = split;
    }
    while (d->range < 128) {
        d->value <<= 1;
        d->range <<= 1;
        if (++d->bits == 8) {
            d->bits = 0;
            if (d->p < d->end)
                d->value |= *d->p++;
        }
    }
    return bit;
}

static int bd_bit(bd_t *d)
{
    return bd_get(d, 128);
}

static int bd_uint(bd_t *d, int n)
{
    int v = 0;

    while (n--)
        v = v << 1 | bd_bit(d);
    return v;
}

/* An n-bit magnitude and a sign, when its presence flag is set. */
static int bd_maybe_int(bd_t *d, int n)
{
    int v;

    if (!bd_bit(d))
        return 0;
    v = bd_uint(d, n);
    return bd_bit(d) ? -v : v;
}

static int bd_tree(bd_t *d, const signed char *tree, const unsigned char *probs)
{
    int i = 0;

    while ((i = tree[i + bd_get(d, probs[i >> 1])]) > 0)
        ;
    return -i;
}

/* ---- State ---- */

typedef struct {
    unsigned char *mem, *y, *u, *v;
} frame_t;

typedef struct {
    unsigned char coeff[4][8][3][11];
    unsigned char mv[2][19];
    unsigned char y_mode[4], uv_mode[3];
} entropy_t;

struct vp8_decoder {
    int w, h, mb_cols, mb_rows, stride, uv_stride, have_key;
    frame_t frames[4];
    int ref[4];               /* frames[] index of CURRENT, LAST, GOLDEN and ALTREF */
    vp8_mb_t *mbs;                /* (mb_rows + 1) x (mb_cols + 1): a border row above and column left */
    unsigned char (*above_ctx)[9], left_ctx[9];

    int key, version, show;
    struct {
        int enabled, update_map, update_data, abs;
        int quant[4], lf[4];
        unsigned char probs[3];
    } seg;
    struct {
        int simple, level, sharpness, delta_enabled;
        int ref_delta[4], mode_delta[4];
    } lf;
    int partitions;
    bd_t part[8];
    int q_index, y1_dc, y2_dc, y2_ac, uv_dc, uv_ac;
    short dq[4][3][2];        /* per segment: [Y, Y2, UV][DC, AC] */
    int refresh_last, refresh_gf, refresh_arf, copy_gf, copy_arf, sign_bias[4], refresh_entropy;
    entropy_t ent, saved;
    int skip_enabled, prob_skip, prob_inter, prob_last, prob_gf;

    short coeffs[25 * 16];
    vp8i_scratch_t scratch;
};

vp8_decoder_t *vp8_decoder_new(void)
{
    vp8_decoder_t *d = mem_alloc(sizeof *d);

    for (int i = 0; i < 4; i++)
        d->ref[i] = -1;
    return d;
}

static void free_frames(vp8_decoder_t *d)
{
    for (int i = 0; i < 4; i++) {
        mem_free(d->frames[i].mem);
        d->frames[i].mem = NULL;
        d->ref[i] = -1;
    }
    mem_free(d->mbs);
    mem_free(d->above_ctx);
    d->mbs = NULL;
    d->above_ctx = NULL;
}

void vp8_decoder_free(vp8_decoder_t *d)
{
    if (d) {
        free_frames(d);
        mem_free(d);
    }
}

static void alloc_frames(vp8_decoder_t *d, int w, int h)
{
    int rows;

    free_frames(d);
    d->w = w;
    d->h = h;
    d->mb_cols = (w + 15) / 16;
    d->mb_rows = (h + 15) / 16;
    d->stride = d->mb_cols * 16 + 2 * BORDER;
    d->uv_stride = d->mb_cols * 8 + BORDER;
    rows = d->mb_rows * 16 + 2 * BORDER;
    for (int i = 0; i < 4; i++) {
        frame_t *f = &d->frames[i];
        size_t y_size = (size_t)d->stride * (size_t)rows, uv_size = (size_t)d->uv_stride * (size_t)(rows / 2);
        f->mem = mem_alloc(y_size + 2 * uv_size);
        f->y = f->mem + BORDER * d->stride + BORDER;
        f->u = f->mem + y_size + BORDER / 2 * d->uv_stride + BORDER / 2;
        f->v = f->u + uv_size;
    }
    d->mbs = mem_alloc(sizeof(vp8_mb_t) * (size_t)(d->mb_cols + 1) * (size_t)(d->mb_rows + 1));
    d->above_ctx = mem_alloc(9 * (size_t)d->mb_cols);
}

static vp8_mb_t *mb_at(vp8_decoder_t *d, int row, int col)
{
    return &d->mbs[(row + 1) * (d->mb_cols + 1) + col + 1];
}

/* ---- Frame header (sections 9 and 19.2) ---- */

static void read_segmentation(vp8_decoder_t *d, bd_t *b)
{
    if (d->key)
        memset(&d->seg, 0, sizeof d->seg);
    d->seg.enabled = bd_bit(b);
    d->seg.update_map = d->seg.update_data = 0;
    if (!d->seg.enabled)
        return;
    d->seg.update_map = bd_bit(b);
    d->seg.update_data = bd_bit(b);
    if (d->seg.update_data) {
        d->seg.abs = bd_bit(b);
        for (int i = 0; i < 4; i++)
            d->seg.quant[i] = bd_maybe_int(b, 7);
        for (int i = 0; i < 4; i++)
            d->seg.lf[i] = bd_maybe_int(b, 6);
    }
    if (d->seg.update_map)
        for (int i = 0; i < 3; i++)
            d->seg.probs[i] = (unsigned char)(bd_bit(b) ? bd_uint(b, 8) : 255);
}

static void read_loop_filter(vp8_decoder_t *d, bd_t *b)
{
    if (d->key)
        memset(&d->lf, 0, sizeof d->lf);
    d->lf.simple = bd_bit(b);
    d->lf.level = bd_uint(b, 6);
    d->lf.sharpness = bd_uint(b, 3);
    d->lf.delta_enabled = bd_bit(b);
    /* Deltas not sent keep their value. */
    if (d->lf.delta_enabled && bd_bit(b)) {
        for (int i = 0; i < 4; i++)
            if (bd_bit(b)) {
                int v = bd_uint(b, 6);
                d->lf.ref_delta[i] = bd_bit(b) ? -v : v;
            }
        for (int i = 0; i < 4; i++)
            if (bd_bit(b)) {
                int v = bd_uint(b, 6);
                d->lf.mode_delta[i] = bd_bit(b) ? -v : v;
            }
    }
}

static int clamp_q(int q)
{
    return q < 0 ? 0 : q > 127 ? 127 : q;
}

static void init_dequant(vp8_decoder_t *d)
{
    for (int i = 0; i < 4; i++) {
        int q = d->q_index;
        if (d->seg.enabled)
            q = d->seg.abs ? d->seg.quant[i] : q + d->seg.quant[i];
        d->dq[i][0][0] = vp8_dc_q[clamp_q(q + d->y1_dc)];
        d->dq[i][0][1] = vp8_ac_q[clamp_q(q)];
        d->dq[i][1][0] = (short)(vp8_dc_q[clamp_q(q + d->y2_dc)] * 2);
        d->dq[i][1][1] = (short)(vp8_ac_q[clamp_q(q + d->y2_ac)] * 155 / 100);
        if (d->dq[i][1][1] < 8)
            d->dq[i][1][1] = 8;
        d->dq[i][2][0] = vp8_dc_q[clamp_q(q + d->uv_dc)];
        if (d->dq[i][2][0] > 132)
            d->dq[i][2][0] = 132;
        d->dq[i][2][1] = vp8_ac_q[clamp_q(q + d->uv_ac)];
    }
}

static int read_partitions(vp8_decoder_t *d, bd_t *b, const unsigned char *data, size_t n)
{
    size_t table;

    d->partitions = 1 << bd_uint(b, 2);
    table = 3 * (size_t)(d->partitions - 1);
    if (n < table)
        return 0;
    for (int i = 0; i < d->partitions; i++) {
        size_t start = table, size;
        const unsigned char *p = data;
        for (int k = 0; k < i; k++, p += 3)
            start += (size_t)(p[0] | p[1] << 8 | p[2] << 16);
        size = i < d->partitions - 1 ? (size_t)(p[0] | p[1] << 8 | p[2] << 16) : n > start ? n - start : 0;
        if (start > n || size > n - start)
            return 0;
        bd_init(&d->part[i], data + start, size);
    }
    return 1;
}

static void read_entropy(vp8_decoder_t *d, bd_t *b)
{
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            for (int k = 0; k < 3; k++)
                for (int l = 0; l < 11; l++)
                    if (bd_get(b, vp8_coeff_update_probs[i][j][k][l]))
                        d->ent.coeff[i][j][k][l] = (unsigned char)bd_uint(b, 8);
    d->skip_enabled = bd_bit(b);
    d->prob_skip = d->skip_enabled ? bd_uint(b, 8) : 0;
    if (d->key)
        return;
    d->prob_inter = bd_uint(b, 8);
    d->prob_last = bd_uint(b, 8);
    d->prob_gf = bd_uint(b, 8);
    if (bd_bit(b))
        for (int i = 0; i < 4; i++)
            d->ent.y_mode[i] = (unsigned char)bd_uint(b, 8);
    if (bd_bit(b))
        for (int i = 0; i < 3; i++)
            d->ent.uv_mode[i] = (unsigned char)bd_uint(b, 8);
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 19; j++)
            if (bd_get(b, vp8_mv_update_probs[i][j])) {
                int x = bd_uint(b, 7);
                d->ent.mv[i][j] = (unsigned char)(x ? x << 1 : 1);
            }
}

/* ---- Modes and motion vectors (sections 11, 16 and 17) ---- */

static int above_b_mode(const vp8_mb_t *m, const vp8_mb_t *above, int b)
{
    if (b >= 4)
        return m->modes[b - 4];
    switch (above->y_mode) {
    case B_PRED:
        return above->modes[b + 12];
    case V_PRED:
        return B_VE_PRED;
    case H_PRED:
        return B_HE_PRED;
    case TM_PRED:
        return B_TM_PRED;
    default:
        return B_DC_PRED;
    }
}

static int left_b_mode(const vp8_mb_t *m, const vp8_mb_t *left, int b)
{
    if (b & 3)
        return m->modes[b - 1];
    switch (left->y_mode) {
    case B_PRED:
        return left->modes[b + 3];
    case V_PRED:
        return B_VE_PRED;
    case H_PRED:
        return B_HE_PRED;
    case TM_PRED:
        return B_TM_PRED;
    default:
        return B_DC_PRED;
    }
}

static int read_mv_component(bd_t *b, const unsigned char *p)
{
    enum { IS_SHORT, SIGN, SHORT, LONG = 9 };
    int x = 0;

    if (bd_get(b, p[IS_SHORT])) {
        for (int i = 0; i < 3; i++)
            x += bd_get(b, p[LONG + i]) << i;
        for (int i = 9; i > 3; i--)
            x += bd_get(b, p[LONG + i]) << i;
        /* bit 3 is implicit when no higher bit is set */
        if (!(x & 0xFFF0) || bd_get(b, p[LONG + 3]))
            x += 8;
    } else {
        x = bd_tree(b, vp8_small_mv_tree, p + SHORT);
    }
    if (x && bd_get(b, p[SIGN]))
        x = -x;
    return x * 2;
}

static vp8_mv_t read_mv(vp8_decoder_t *d, bd_t *b)
{
    vp8_mv_t mv;

    mv.y = (short)read_mv_component(b, d->ent.mv[0]);
    mv.x = (short)read_mv_component(b, d->ent.mv[1]);
    return mv;
}

static vp8_mv_t left_block_mv(const vp8_mb_t *m, const vp8_mb_t *left, int b)
{
    if (b & 3)
        return m->mvs[b - 1];
    return left->y_mode == SPLITMV ? left->mvs[b + 3] : left->mv;
}

static vp8_mv_t above_block_mv(const vp8_mb_t *m, const vp8_mb_t *above, int b)
{
    if (b >= 4)
        return m->mvs[b - 4];
    return above->y_mode == SPLITMV ? above->mvs[b + 12] : above->mv;
}

static void read_split_mv(vp8_decoder_t *d, bd_t *b, vp8_mb_t *m, const vp8_mb_t *left, const vp8_mb_t *above, vp8_mv_t best)
{
    int id = bd_tree(b, vp8_split_mv_tree, vp8_split_mv_probs), mask = 0;
    const unsigned char *part = vp8_mv_partitions[id];

    for (int j = 0; mask != 0xFFFF; j++) {
        int k = 0, ctx;
        vp8_mv_t l, a, mv;
        while (part[k] != j)
            k++;
        l = left_block_mv(m, left, k);
        a = above_block_mv(m, above, k);
        if (vp8i_mv_eq(l, a))
            ctx = vp8i_mv_zero(l) ? 4 : 3;
        else if (vp8i_mv_zero(a))
            ctx = 2;
        else if (vp8i_mv_zero(l))
            ctx = 1;
        else
            ctx = 0;
        switch (bd_tree(b, vp8_submv_ref_tree, vp8_submv_ref_probs[ctx])) {
        case LEFT4X4:
            mv = l;
            break;
        case ABOVE4X4:
            mv = a;
            break;
        case ZERO4X4:
            mv.x = mv.y = 0;
            break;
        default:
            mv = read_mv(d, b);
            mv.x = (short)(mv.x + best.x);
            mv.y = (short)(mv.y + best.y);
            break;
        }
        for (; k < 16; k++)
            if (part[k] == j) {
                m->mvs[k] = mv;
                mask |= 1 << k;
            }
    }
}

static void read_inter_modes(vp8_decoder_t *d, bd_t *b, vp8_mb_t *m, int row, int col)
{
    vp8_mb_t *above = mb_at(d, row - 1, col), *left = m - 1;
    vp8_mv_t near_mvs[4];
    int cnt[4];
    unsigned char probs[4];
    int to_left = -((col + 1) << 7), to_right = (d->mb_cols - col) << 7;
    int to_top = -((row + 1) << 7), to_bottom = (d->mb_rows - row) << 7;

    m->ref = (unsigned char)(bd_get(b, d->prob_last) ? 2 + bd_get(b, d->prob_gf) : LAST);
    vp8i_find_near_mvs(d->sign_bias, m, above, left, near_mvs, cnt);
    for (int i = 0; i < 4; i++)
        probs[i] = vp8_mv_counts_to_probs[cnt[i]][i];
    m->y_mode = m->uv_mode = (unsigned char)bd_tree(b, vp8_mv_ref_tree, probs);
    switch (m->y_mode) {
    case NEARESTMV:
        m->mv = vp8i_clamp_mv(near_mvs[1], to_left, to_right, to_top, to_bottom);
        break;
    case NEARMV:
        m->mv = vp8i_clamp_mv(near_mvs[2], to_left, to_right, to_top, to_bottom);
        break;
    case ZEROMV:
        m->mv.x = m->mv.y = 0;
        break;
    case NEWMV: {
        vp8_mv_t best = vp8i_clamp_mv(near_mvs[0], to_left, to_right, to_top, to_bottom), mv = read_mv(d, b);
        m->mv.x = (short)(mv.x + best.x);
        m->mv.y = (short)(mv.y + best.y);
        break;
    }
    default:
        read_split_mv(d, b, m, left, above, vp8i_clamp_mv(near_mvs[0], to_left, to_right, to_top, to_bottom));
        m->mv = m->mvs[15];
        break;
    }
}

static void read_modes(vp8_decoder_t *d, bd_t *b, vp8_mb_t *m, int row, int col)
{
    if (d->seg.update_map)
        m->segment = (unsigned char)(bd_get(b, d->seg.probs[0]) ? 2 + bd_get(b, d->seg.probs[2]) : bd_get(b, d->seg.probs[1]));
    else if (d->key)
        m->segment = 0;
    m->skip = (unsigned char)(d->skip_enabled ? bd_get(b, d->prob_skip) : 0);
    if (d->key) {
        const vp8_mb_t *above = mb_at(d, row - 1, col), *left = m - 1;
        m->y_mode = (unsigned char)bd_tree(b, vp8_kf_y_mode_tree, vp8_kf_y_mode_probs);
        if (m->y_mode == B_PRED)
            for (int i = 0; i < 16; i++)
                m->modes[i] = (unsigned char)bd_tree(b, vp8_b_mode_tree,
                                                     vp8_kf_b_mode_probs[above_b_mode(m, above, i)][left_b_mode(m, left, i)]);
        m->uv_mode = (unsigned char)bd_tree(b, vp8_uv_mode_tree, vp8_kf_uv_mode_probs);
    } else if (bd_get(b, d->prob_inter)) {
        read_inter_modes(d, b, m, row, col);
        return;
    } else {
        m->y_mode = (unsigned char)bd_tree(b, vp8_y_mode_tree, d->ent.y_mode);
        if (m->y_mode == B_PRED)
            for (int i = 0; i < 16; i++)
                m->modes[i] = (unsigned char)bd_tree(b, vp8_b_mode_tree, vp8_default_b_mode_probs);
        m->uv_mode = (unsigned char)bd_tree(b, vp8_uv_mode_tree, d->ent.uv_mode);
    }
    m->ref = CURRENT;
    m->mv.x = m->mv.y = 0;
}

/* ---- Coefficients (section 13) ---- */

static int read_extra(bd_t *b, const unsigned char *probs)
{
    int v = 0;

    for (; *probs; probs++)
        v = v << 1 | bd_get(b, *probs);
    return v;
}

/* One block's tokens, dequantized into out[] (natural order). Returns whether any token was coded. */
static int read_block(bd_t *b, const unsigned char (*probs)[3][11], int ctx, int first, short *out, const short *dq)
{
    const unsigned char *p = probs[vp8_bands[first]][ctx];
    int c = first;

    if (!bd_get(b, p[0]))
        return 0;
    for (;;) {
        int v, next;
        if (!bd_get(b, p[1])) { /* a zero: the next token cannot be the end of block */
            if (++c == 16)
                break;
            p = probs[vp8_bands[c]][0];
            continue;
        }
        if (!bd_get(b, p[2])) {
            v = 1;
            next = 1;
        } else {
            if (!bd_get(b, p[3]))
                v = !bd_get(b, p[4]) ? 2 : 3 + bd_get(b, p[5]);
            else if (!bd_get(b, p[6]))
                v = !bd_get(b, p[7]) ? 5 + bd_get(b, 159) : 7 + (bd_get(b, 165) << 1) + bd_get(b, 145);
            else if (!bd_get(b, p[8]))
                v = !bd_get(b, p[9]) ? 11 + read_extra(b, vp8_cat3) : 19 + read_extra(b, vp8_cat4);
            else
                v = !bd_get(b, p[10]) ? 35 + read_extra(b, vp8_cat5) : 67 + read_extra(b, vp8_cat6);
            next = 2;
        }
        if (bd_bit(b))
            v = -v;
        out[vp8_zigzag[c]] = (short)(v * dq[c > 0]);
        if (++c == 16)
            break;
        p = probs[vp8_bands[c]][next];
        if (!bd_get(b, p[0]))
            break;
    }
    return 1;
}

/* All of a macroblock's blocks: Y2 (when present), 16 Y, 4 U, 4 V, with their above and left contexts. */
static int read_tokens(vp8_decoder_t *d, bd_t *b, const vp8_mb_t *m, unsigned char *above, unsigned char *left)
{
    const short (*dq)[2] = d->dq[d->seg.enabled ? m->segment : 0];
    int y2 = m->y_mode != B_PRED && m->y_mode != SPLITMV, any = 0, type = y2 ? TYPE_Y_AFTER_Y2 : TYPE_Y;

    if (y2) {
        int t = read_block(b, d->ent.coeff[TYPE_Y2], above[8] + left[8], 0, d->coeffs + 24 * 16, dq[1]);
        above[8] = left[8] = (unsigned char)t;
        any |= t;
    }
    for (int i = 0; i < 16; i++) {
        int t = read_block(b, d->ent.coeff[type], above[i & 3] + left[i >> 2], y2, d->coeffs + i * 16, dq[0]);
        above[i & 3] = left[i >> 2] = (unsigned char)t;
        any |= t;
    }
    for (int i = 16; i < 24; i++) {
        int a = 4 + ((i - 16) >> 2) * 2 + (i & 1), l = 4 + ((i - 16) >> 2) * 2 + ((i >> 1) & 1);
        int t = read_block(b, d->ent.coeff[TYPE_UV], above[a] + left[l], 0, d->coeffs + i * 16, dq[2]);
        above[a] = left[l] = (unsigned char)t;
        any |= t;
    }
    return any;
}

/* ---- Intra prediction (section 12), in place in the frame ---- */

static void predict_intra(vp8_decoder_t *d, const vp8_mb_t *m, unsigned char *y, unsigned char *u, unsigned char *v)
{
    short *c = d->coeffs;
    int s = d->stride, us = d->uv_stride;

    if (m->y_mode == B_PRED) {
        /* The pixels above-right of subblock 3 serve the right column's subblocks 7, 11 and 15 too. */
        for (int r = 4; r < 16; r += 4)
            memcpy(y + (r - 1) * s + 16, y - s + 16, 4);
        for (int i = 0; i < 16; i++) {
            unsigned char *p = y + (i >> 2) * 4 * s + (i & 3) * 4;
            vp8i_predict_sub(p, s, m->modes[i]);
            vp8i_idct_add(p, s, c + i * 16);
        }
    } else {
        vp8i_predict_block(y, s, 16, m->y_mode);
        vp8i_iwht(c);
        for (int i = 0; i < 16; i++)
            vp8i_idct_add(y + (i >> 2) * 4 * s + (i & 3) * 4, s, c + i * 16);
    }
    vp8i_predict_block(u, us, 8, m->uv_mode);
    vp8i_predict_block(v, us, 8, m->uv_mode);
    for (int i = 0; i < 4; i++) {
        vp8i_idct_add(u + (i >> 1) * 4 * us + (i & 1) * 4, us, c + (16 + i) * 16);
        vp8i_idct_add(v + (i >> 1) * 4 * us + (i & 1) * 4, us, c + (20 + i) * 16);
    }
}

/* ---- Inter prediction (section 18) ---- */

static void predict_inter(vp8_decoder_t *d, const vp8_mb_t *m, int row, int col, unsigned char *y, unsigned char *u,
                          unsigned char *v)
{
    const frame_t *r = &d->frames[d->ref[m->ref]];
    int s = d->stride, us = d->uv_stride, pw = d->mb_cols * 16, ph = d->mb_rows * 16, x = col * 16, yy = row * 16;
    short *c = d->coeffs;
    vp8_mv_t uvmv[4];

    if (m->y_mode != SPLITMV) {
        vp8i_predict_inter_block(&d->scratch, y, s, r->y, s, pw, ph, x, yy, 16, 16, m->mv);
        uvmv[0] = vp8i_chroma_mv(m->mv, d->version == 3);
        for (int i = 1; i < 4; i++)
            uvmv[i] = uvmv[0];
        vp8i_iwht(c);
    } else {
        for (int b = 0; b < 16; b++)
            vp8i_predict_inter_block(&d->scratch, y + (b >> 2) * 4 * s + (b & 3) * 4, s, r->y, s, pw, ph,
                                     x + (b & 3) * 4, yy + (b >> 2) * 4, 4, 4, m->mvs[b]);
        /* Each chroma subblock moves by the average of the four luma vectors it covers. */
        for (int i = 0; i < 4; i++) {
            int b = (i >> 1) * 8 + (i & 1) * 2;
            int sx = m->mvs[b].x + m->mvs[b + 1].x + m->mvs[b + 4].x + m->mvs[b + 5].x;
            int sy = m->mvs[b].y + m->mvs[b + 1].y + m->mvs[b + 4].y + m->mvs[b + 5].y;
            sx += sx < 0 ? -4 : 4;
            sy += sy < 0 ? -4 : 4;
            uvmv[i].x = (short)(sx / 8);
            uvmv[i].y = (short)(sy / 8);
            if (d->version == 3) {
                uvmv[i].x = (short)(uvmv[i].x & ~7);
                uvmv[i].y = (short)(uvmv[i].y & ~7);
            }
        }
    }
    for (int i = 0; i < 4; i++) {
        int bx = (i & 1) * 4, by = (i >> 1) * 4;
        vp8i_predict_inter_block(&d->scratch, u + by * us + bx, us, r->u, us, pw / 2, ph / 2, x / 2 + bx, yy / 2 + by, 4,
                                 4, uvmv[i]);
        vp8i_predict_inter_block(&d->scratch, v + by * us + bx, us, r->v, us, pw / 2, ph / 2, x / 2 + bx, yy / 2 + by, 4,
                                 4, uvmv[i]);
    }
    for (int i = 0; i < 16; i++)
        vp8i_idct_add(y + (i >> 2) * 4 * s + (i & 3) * 4, s, c + i * 16);
    for (int i = 0; i < 4; i++) {
        vp8i_idct_add(u + (i >> 1) * 4 * us + (i & 1) * 4, us, c + (16 + i) * 16);
        vp8i_idct_add(v + (i >> 1) * 4 * us + (i & 1) * 4, us, c + (20 + i) * 16);
    }
}

/* ---- Loop filter (section 15) ---- */

/*
 * Sixteen positions along an edge at a time (eight for chroma, in the low
 * lanes): each line across an edge is filtered on its own, so this is the
 * scalar filter's result. Pixels are biased to signed bytes (x ^ 0x80),
 * where clamping p + a to 0..255 is a saturating add, and the thresholds
 * (at most 193) fit the saturating unsigned compares.
 */
typedef struct {
    __m128i p3, p2, p1, p0, q0, q1, q2, q3;
} lf_px_t;

static __m128i abs_diff(__m128i a, __m128i b)
{
    return _mm_or_si128(_mm_subs_epu8(a, b), _mm_subs_epu8(b, a));
}

/* All-ones lanes where a <= limit. */
static __m128i at_most(__m128i a, __m128i limit)
{
    return _mm_cmpeq_epi8(_mm_subs_epu8(a, limit), _mm_setzero_si128());
}

/* |p0 - q0| * 2 + |p1 - q1| / 2 <= limit */
static __m128i simple_mask(const lf_px_t *x, int limit)
{
    /* Halving bytes as 16-bit lanes, their low bits cleared first so nothing crosses into the next byte. */
    __m128i d = abs_diff(x->p0, x->q0);
    __m128i h = _mm_srli_epi16(_mm_and_si128(abs_diff(x->p1, x->q1), _mm_set1_epi8(-2)), 1);

    return at_most(_mm_adds_epu8(_mm_adds_epu8(d, d), h), _mm_set1_epi8((char)limit));
}

/* Arithmetic shift right by 3 of signed bytes. */
static __m128i sra3_s8(__m128i a)
{
    __m128i zero = _mm_setzero_si128();

    /* Each byte as the high half of a 16-bit lane: shifting by 8 + 3 keeps its sign. */
    return _mm_packs_epi16(_mm_srai_epi16(_mm_unpacklo_epi8(zero, a), 11),
                           _mm_srai_epi16(_mm_unpackhi_epi8(zero, a), 11));
}

/* (w * k + 63) >> 7 for signed bytes w, as signed bytes. */
static __m128i tap_s8(__m128i w, int k)
{
    __m128i zero = _mm_setzero_si128(), m = _mm_set1_epi16((short)k), r = _mm_set1_epi16(63);
    __m128i lo = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_srai_epi16(_mm_unpacklo_epi8(zero, w), 8), m), r), 7);
    __m128i hi = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_srai_epi16(_mm_unpackhi_epi8(zero, w), 8), m), r), 7);

    return _mm_packs_epi16(lo, hi);
}

static __m128i blend(__m128i mask, __m128i yes, __m128i no)
{
    return _mm_or_si128(_mm_and_si128(mask, yes), _mm_andnot_si128(mask, no));
}

/*
 * Filters the lanes in `mask`. `outer` selects the lanes whose base value
 * takes p1 - q1 into account (the others move p1 and q1 too); `mb` those
 * filtered as a macroblock edge, over three pixels on each side.
 */
static void lf_filter(lf_px_t *x, __m128i mask, __m128i outer, __m128i mb)
{
    const __m128i bias = _mm_set1_epi8(-128);
    __m128i ps2 = _mm_xor_si128(x->p2, bias), ps1 = _mm_xor_si128(x->p1, bias), ps0 = _mm_xor_si128(x->p0, bias);
    __m128i qs0 = _mm_xor_si128(x->q0, bias), qs1 = _mm_xor_si128(x->q1, bias), qs2 = _mm_xor_si128(x->q2, bias);
    __m128i d = _mm_subs_epi8(qs0, ps0), pq = _mm_subs_epi8(ps1, qs1);
    /* s8(3 * (q0 - p0) [+ s8(p1 - q1)]): the saturating adds all go d's way, so they clamp once in effect. */
    __m128i a = _mm_adds_epi8(_mm_adds_epi8(_mm_adds_epi8(_mm_and_si128(_mm_or_si128(outer, mb), pq), d), d), d);
    __m128i f1 = sra3_s8(_mm_adds_epi8(a, _mm_set1_epi8(4))), f2 = sra3_s8(_mm_adds_epi8(a, _mm_set1_epi8(3)));
    /* (f1 + 1) >> 1, for p1 and q1 inside a block */
    __m128i one = _mm_set1_epi16(1), zero = _mm_setzero_si128();
    __m128i u = _mm_packs_epi16(_mm_srai_epi16(_mm_add_epi16(_mm_srai_epi16(_mm_unpacklo_epi8(zero, f1), 8), one), 1),
                                _mm_srai_epi16(_mm_add_epi16(_mm_srai_epi16(_mm_unpackhi_epi8(zero, f1), 8), one), 1));
    __m128i inner = _mm_andnot_si128(_mm_or_si128(outer, mb), mask), common = _mm_andnot_si128(mb, mask);
    __m128i a27 = tap_s8(a, 27), a18 = tap_s8(a, 18), a9 = tap_s8(a, 9);
    mb = _mm_and_si128(mb, mask);
    /* The base filter moves p0 and q0 (and, inside, p1 and q1); the macroblock filter three pixels each side. */
    ps0 = blend(common, _mm_adds_epi8(ps0, f2), blend(mb, _mm_adds_epi8(ps0, a27), ps0));
    qs0 = blend(common, _mm_subs_epi8(qs0, f1), blend(mb, _mm_subs_epi8(qs0, a27), qs0));
    ps1 = blend(inner, _mm_adds_epi8(ps1, u), blend(mb, _mm_adds_epi8(ps1, a18), ps1));
    qs1 = blend(inner, _mm_subs_epi8(qs1, u), blend(mb, _mm_subs_epi8(qs1, a18), qs1));
    ps2 = blend(mb, _mm_adds_epi8(ps2, a9), ps2);
    qs2 = blend(mb, _mm_subs_epi8(qs2, a9), qs2);
    x->p2 = _mm_xor_si128(ps2, bias);
    x->p1 = _mm_xor_si128(ps1, bias);
    x->p0 = _mm_xor_si128(ps0, bias);
    x->q0 = _mm_xor_si128(qs0, bias);
    x->q1 = _mm_xor_si128(qs1, bias);
    x->q2 = _mm_xor_si128(qs2, bias);
}

/* The normal filter's lanes: the edge test, then high edge variance picks the filter. */
static void lf_normal(lf_px_t *x, int edge, int interior, int threshold, int mb)
{
    __m128i lim = _mm_set1_epi8((char)interior), thr = _mm_set1_epi8((char)threshold);
    __m128i m = _mm_max_epu8(_mm_max_epu8(abs_diff(x->p3, x->p2), abs_diff(x->p2, x->p1)),
                             _mm_max_epu8(abs_diff(x->q3, x->q2), abs_diff(x->q2, x->q1)));
    __m128i v = _mm_max_epu8(abs_diff(x->p1, x->p0), abs_diff(x->q1, x->q0));
    __m128i mask = _mm_and_si128(simple_mask(x, 2 * edge + interior), at_most(_mm_max_epu8(m, v), lim));
    __m128i hev = _mm_xor_si128(at_most(v, thr), _mm_set1_epi8(-1));

    lf_filter(x, mask, hev, mb ? _mm_andnot_si128(hev, mask) : _mm_setzero_si128());
}

/* 16 (or 8) rows of 8 pixels, p - 4 .. p + 3, into the eight columns' lanes. */
static void lf_load_across(lf_px_t *x, const unsigned char *p, int stride, int n)
{
    __m128i r[16], b[8], lo[4], hi[4], c[8];

    for (int i = 0; i < 16; i++)
        r[i] = i < n ? _mm_loadl_epi64((const __m128i *)(p - 4 + i * stride)) : _mm_setzero_si128();
    for (int k = 0; k < 8; k++)
        b[k] = _mm_unpacklo_epi8(r[2 * k], r[2 * k + 1]);
    for (int k = 0; k < 4; k++) {
        lo[k] = _mm_unpacklo_epi16(b[2 * k], b[2 * k + 1]); /* columns 0-3 of rows 4k..4k+3 */
        hi[k] = _mm_unpackhi_epi16(b[2 * k], b[2 * k + 1]); /* columns 4-7 */
    }
    for (int h = 0; h < 2; h++) {
        __m128i *s = h ? hi : lo;
        __m128i a0 = _mm_unpacklo_epi32(s[0], s[1]), a1 = _mm_unpackhi_epi32(s[0], s[1]);
        __m128i b0 = _mm_unpacklo_epi32(s[2], s[3]), b1 = _mm_unpackhi_epi32(s[2], s[3]);
        c[4 * h] = _mm_unpacklo_epi64(a0, b0);
        c[4 * h + 1] = _mm_unpackhi_epi64(a0, b0);
        c[4 * h + 2] = _mm_unpacklo_epi64(a1, b1);
        c[4 * h + 3] = _mm_unpackhi_epi64(a1, b1);
    }
    x->p3 = c[0];
    x->p2 = c[1];
    x->p1 = c[2];
    x->p0 = c[3];
    x->q0 = c[4];
    x->q1 = c[5];
    x->q2 = c[6];
    x->q3 = c[7];
}

/* The inverse of lf_load_across. */
static void lf_store_across(const lf_px_t *x, unsigned char *p, int stride, int n)
{
    __m128i c[8] = {x->p3, x->p2, x->p1, x->p0, x->q0, x->q1, x->q2, x->q3}, b[8], d[8];

    for (int k = 0; k < 4; k++) {
        b[2 * k] = _mm_unpacklo_epi8(c[2 * k], c[2 * k + 1]);     /* rows 0-7, columns 2k and 2k+1 */
        b[2 * k + 1] = _mm_unpackhi_epi8(c[2 * k], c[2 * k + 1]); /* rows 8-15 */
    }
    for (int h = 0; h < 2; h++) {
        __m128i a0 = _mm_unpacklo_epi16(b[h], b[2 + h]), a1 = _mm_unpackhi_epi16(b[h], b[2 + h]);
        __m128i b0 = _mm_unpacklo_epi16(b[4 + h], b[6 + h]), b1 = _mm_unpackhi_epi16(b[4 + h], b[6 + h]);
        d[4 * h] = _mm_unpacklo_epi32(a0, b0);     /* rows 8h, 8h+1: columns 0-7 each */
        d[4 * h + 1] = _mm_unpackhi_epi32(a0, b0); /* rows 8h+2, 8h+3 */
        d[4 * h + 2] = _mm_unpacklo_epi32(a1, b1);
        d[4 * h + 3] = _mm_unpackhi_epi32(a1, b1);
    }
    for (int i = 0; i < n; i++) {
        __m128i row = d[i >> 1];
        _mm_storel_epi64((__m128i *)(p - 4 + i * stride), (i & 1) ? _mm_srli_si128(row, 8) : row);
    }
}

static void lf_load_along(lf_px_t *x, const unsigned char *p, int stride, int n)
{
    __m128i *v = &x->p3;

    for (int k = 0; k < 8; k++) {
        const unsigned char *r = p + (k - 4) * stride;
        v[k] = n == 16 ? _mm_loadu_si128((const __m128i *)r) : _mm_loadl_epi64((const __m128i *)r);
    }
}

static void lf_store_along(const lf_px_t *x, unsigned char *p, int stride, int n)
{
    const __m128i *v = &x->p3;

    for (int k = 1; k < 7; k++) {
        unsigned char *r = p + (k - 4) * stride;
        if (n == 16)
            _mm_storeu_si128((__m128i *)r, v[k]);
        else
            _mm_storel_epi64((__m128i *)r, v[k]);
    }
}

/* An edge of n (16 or 8) pixels: `step` crosses it, 1 for a vertical edge, the stride for a horizontal one. */
static void edge_normal(unsigned char *p, int step, int stride, int n, int edge, int interior, int threshold, int mb)
{
    lf_px_t x;

    if (step == 1) {
        lf_load_across(&x, p, stride, n);
        lf_normal(&x, edge, interior, threshold, mb);
        lf_store_across(&x, p, stride, n);
    } else {
        lf_load_along(&x, p, stride, n);
        lf_normal(&x, edge, interior, threshold, mb);
        lf_store_along(&x, p, stride, n);
    }
}

static void edge_simple(unsigned char *p, int step, int stride, int limit)
{
    lf_px_t x;
    __m128i all = _mm_set1_epi8(-1), none = _mm_setzero_si128();

    if (step == 1) {
        lf_load_across(&x, p, stride, 16);
        lf_filter(&x, simple_mask(&x, limit), all, none);
        lf_store_across(&x, p, stride, 16);
    } else {
        lf_load_along(&x, p, stride, 16);
        lf_filter(&x, simple_mask(&x, limit), all, none);
        lf_store_along(&x, p, stride, 16);
    }
}

static void loop_filter(vp8_decoder_t *d, const frame_t *f)
{
    int s = d->stride, us = d->uv_stride;

    for (int row = 0; row < d->mb_rows; row++)
        for (int col = 0; col < d->mb_cols; col++) {
            const vp8_mb_t *m = mb_at(d, row, col);
            unsigned char *y = f->y + row * 16 * s + col * 16, *u = f->u + row * 8 * us + col * 8, *v = f->v + row * 8 * us + col * 8;
            int level = d->lf.level, interior, threshold, inner;
            if (d->seg.enabled) {
                level = d->seg.abs ? d->seg.lf[m->segment] : level + d->seg.lf[m->segment];
                level = level < 0 ? 0 : level > 63 ? 63 : level;
            }
            if (d->lf.delta_enabled) {
                level += d->lf.ref_delta[m->ref];
                if (m->ref == CURRENT)
                    level += m->y_mode == B_PRED ? d->lf.mode_delta[0] : 0;
                else
                    level += d->lf.mode_delta[m->y_mode == ZEROMV ? 1 : m->y_mode == SPLITMV ? 3 : 2];
                level = level < 0 ? 0 : level > 63 ? 63 : level;
            }
            if (!level)
                continue;
            interior = level;
            if (d->lf.sharpness) {
                interior >>= d->lf.sharpness > 4 ? 2 : 1;
                if (interior > 9 - d->lf.sharpness)
                    interior = 9 - d->lf.sharpness;
            }
            if (interior < 1)
                interior = 1;
            threshold = (level >= 40) + (level >= 15) + (level >= 20 && !d->key);
            inner = m->coded || m->y_mode == SPLITMV || m->y_mode == B_PRED;
            if (d->lf.simple) {
                int mb_limit = (level + 2) * 2 + interior, b_limit = level * 2 + interior;
                if (col)
                    edge_simple(y, 1, s, mb_limit);
                if (inner)
                    for (int k = 4; k < 16; k += 4)
                        edge_simple(y + k, 1, s, b_limit);
                if (row)
                    edge_simple(y, s, s, mb_limit);
                if (inner)
                    for (int k = 4; k < 16; k += 4)
                        edge_simple(y + k * s, s, s, b_limit);
                continue;
            }
            if (col) {
                edge_normal(y, 1, s, 16, level + 2, interior, threshold, 1);
                edge_normal(u, 1, us, 8, level + 2, interior, threshold, 1);
                edge_normal(v, 1, us, 8, level + 2, interior, threshold, 1);
            }
            if (inner) {
                for (int k = 4; k < 16; k += 4)
                    edge_normal(y + k, 1, s, 16, level, interior, threshold, 0);
                edge_normal(u + 4, 1, us, 8, level, interior, threshold, 0);
                edge_normal(v + 4, 1, us, 8, level, interior, threshold, 0);
            }
            if (row) {
                edge_normal(y, s, s, 16, level + 2, interior, threshold, 1);
                edge_normal(u, us, us, 8, level + 2, interior, threshold, 1);
                edge_normal(v, us, us, 8, level + 2, interior, threshold, 1);
            }
            if (inner) {
                for (int k = 4; k < 16; k += 4)
                    edge_normal(y + k * s, s, s, 16, level, interior, threshold, 0);
                edge_normal(u + 4 * us, us, us, 8, level, interior, threshold, 0);
                edge_normal(v + 4 * us, us, us, 8, level, interior, threshold, 0);
            }
        }
}

/* ---- Frames ---- */

static int free_frame(const vp8_decoder_t *d)
{
    for (int i = 0; i < 4; i++)
        if (i != d->ref[LAST] && i != d->ref[GOLDEN] && i != d->ref[ALTREF])
            return i;
    return 0;
}

int vp8_decode(vp8_decoder_t *d, const unsigned char *data, size_t n, vp8_image_t *out)
{
    unsigned raw;
    size_t first;
    bd_t b;
    frame_t *f;

    if (n < 3)
        return -1;
    raw = (unsigned)data[0] | (unsigned)data[1] << 8 | (unsigned)data[2] << 16;
    d->key = !(raw & 1);
    d->version = (int)(raw >> 1 & 7);
    d->show = (int)(raw >> 4 & 1);
    first = raw >> 5;
    data += 3;
    n -= 3;
    if (d->key) {
        int w, h;
        if (n < 7 || data[0] != 0x9d || data[1] != 0x01 || data[2] != 0x2a)
            return -1;
        w = (data[3] | data[4] << 8) & 0x3FFF;
        h = (data[5] | data[6] << 8) & 0x3FFF;
        if (!w || !h)
            return -1;
        if (w != d->w || h != d->h || !d->mbs)
            alloc_frames(d, w, h);
        data += 7;
        n -= 7;
        d->scratch.filters = d->version ? vp8_bilinear : vp8_sixtap;
        d->have_key = 1;
    } else if (!d->have_key) {
        return -1;
    }
    if (first > n)
        return -1;
    bd_init(&b, data, first);
    if (d->key)
        bd_uint(&b, 2); /* color space and clamping type: clamping is always done */
    read_segmentation(d, &b);
    read_loop_filter(d, &b);
    if (!read_partitions(d, &b, data + first, n - first))
        return -1;
    d->q_index = bd_uint(&b, 7);
    d->y1_dc = bd_maybe_int(&b, 4);
    d->y2_dc = bd_maybe_int(&b, 4);
    d->y2_ac = bd_maybe_int(&b, 4);
    d->uv_dc = bd_maybe_int(&b, 4);
    d->uv_ac = bd_maybe_int(&b, 4);
    if (d->key) {
        d->refresh_gf = d->refresh_arf = d->refresh_last = 1;
        d->copy_gf = d->copy_arf = 0;
        d->sign_bias[GOLDEN] = d->sign_bias[ALTREF] = 0;
        d->refresh_entropy = bd_bit(&b);
        memcpy(d->ent.coeff, vp8_default_coeff_probs, sizeof d->ent.coeff);
        memcpy(d->ent.mv, vp8_default_mv_probs, sizeof d->ent.mv);
        memcpy(d->ent.y_mode, vp8_default_y_mode_probs, sizeof d->ent.y_mode);
        memcpy(d->ent.uv_mode, vp8_default_uv_mode_probs, sizeof d->ent.uv_mode);
    } else {
        d->refresh_gf = bd_bit(&b);
        d->refresh_arf = bd_bit(&b);
        d->copy_gf = d->refresh_gf ? 0 : bd_uint(&b, 2);
        d->copy_arf = d->refresh_arf ? 0 : bd_uint(&b, 2);
        d->sign_bias[GOLDEN] = bd_bit(&b);
        d->sign_bias[ALTREF] = bd_bit(&b);
        d->refresh_entropy = bd_bit(&b);
        d->refresh_last = bd_bit(&b);
        if (d->ref[LAST] < 0)
            return -1;
    }
    /* Probabilities changed by a frame that does not refresh them last for that frame only. */
    if (!d->refresh_entropy)
        d->saved = d->ent;
    read_entropy(d, &b);
    init_dequant(d);

    d->ref[CURRENT] = free_frame(d);
    f = &d->frames[d->ref[CURRENT]];
    memset(d->above_ctx, 0, 9 * (size_t)d->mb_cols);
    for (int row = 0; row < d->mb_rows; row++) {
        bd_t *tokens = &d->part[row & (d->partitions - 1)];
        unsigned char *y = f->y + row * 16 * d->stride, *u = f->u + row * 8 * d->uv_stride, *v = f->v + row * 8 * d->uv_stride;
        memset(d->left_ctx, 0, sizeof d->left_ctx);
        for (int col = 0; col < d->mb_cols; col++, y += 16, u += 8, v += 8) {
            vp8_mb_t *m = mb_at(d, row, col);
            unsigned char *above = d->above_ctx[col];
            read_modes(d, &b, m, row, col);
            memset(d->coeffs, 0, sizeof d->coeffs);
            if (!m->skip) {
                m->coded = (unsigned char)read_tokens(d, tokens, m, above, d->left_ctx);
            } else {
                /* A skipped macroblock clears the contexts, except Y2's when it has no Y2. */
                int y2 = m->y_mode != B_PRED && m->y_mode != SPLITMV;
                memset(above, 0, y2 ? 9 : 8);
                memset(d->left_ctx, 0, y2 ? 9 : 8);
                m->coded = 0;
            }
            if (col == 0) {
                vp8i_fixup_left(y, d->stride, 16, row, m->y_mode);
                vp8i_fixup_left(u, d->uv_stride, 8, row, m->uv_mode);
                vp8i_fixup_left(v, d->uv_stride, 8, row, m->uv_mode);
                if (row == 0)
                    y[-d->stride - 1] = 127;
            }
            if (row == 0) {
                vp8i_fixup_above(y, d->stride, 16, col, m->y_mode);
                vp8i_fixup_above(u, d->uv_stride, 8, col, m->uv_mode);
                vp8i_fixup_above(v, d->uv_stride, 8, col, m->uv_mode);
            }
            if (m->ref == CURRENT)
                predict_intra(d, m, y, u, v);
            else
                predict_inter(d, m, row, col, y, u, v);
        }
        /* Past the right edge, the next row's above-right pixels repeat the last one. */
        memset(y + 15 * d->stride, y[15 * d->stride - 1], 4);
    }
    if (d->lf.level)
        loop_filter(d, f);
    vp8i_extend_borders(f->y, f->u, f->v, d->stride, d->uv_stride, d->mb_cols * 16, d->mb_rows * 16);
    if (!d->refresh_entropy)
        d->ent = d->saved;

    /* References: copies first (altref, then golden), then refreshes. */
    if (d->copy_arf == 1)
        d->ref[ALTREF] = d->ref[LAST];
    else if (d->copy_arf == 2)
        d->ref[ALTREF] = d->ref[GOLDEN];
    if (d->copy_gf == 1)
        d->ref[GOLDEN] = d->ref[LAST];
    else if (d->copy_gf == 2)
        d->ref[GOLDEN] = d->ref[ALTREF];
    if (d->refresh_gf)
        d->ref[GOLDEN] = d->ref[CURRENT];
    if (d->refresh_arf)
        d->ref[ALTREF] = d->ref[CURRENT];
    if (d->refresh_last)
        d->ref[LAST] = d->ref[CURRENT];

    if (!d->show)
        return 0;
    out->w = d->w;
    out->h = d->h;
    out->y = f->y;
    out->u = f->u;
    out->v = f->v;
    out->y_stride = d->stride;
    out->uv_stride = d->uv_stride;
    return 1;
}
