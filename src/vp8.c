#include <string.h>
#include "vp8.h"
#include "vp8_tables.h"
#include "mem.h"

#define BORDER 32 /* around the luma planes; half of it around chroma */

/* Macroblock modes, then subblock modes, numbered as the probability tables are indexed. */
enum { DC_PRED, V_PRED, H_PRED, TM_PRED, B_PRED, NEARESTMV, NEARMV, ZEROMV, NEWMV, SPLITMV };
enum { B_DC_PRED, B_TM_PRED, B_VE_PRED, B_HE_PRED, B_LD_PRED, B_RD_PRED, B_VR_PRED, B_VL_PRED, B_HD_PRED, B_HU_PRED,
       LEFT4X4, ABOVE4X4, ZERO4X4, NEW4X4 };
enum { CURRENT, LAST, GOLDEN, ALTREF };
enum { TYPE_Y_AFTER_Y2, TYPE_Y2, TYPE_UV, TYPE_Y }; /* coefficient probability sets */

/* Trees (RFC 6386, section 8.1): positive entries index the next pair, others are negated leaves. */
static const signed char k_kf_y_mode_tree[8] = {-B_PRED, 2, 4, 6, -DC_PRED, -V_PRED, -H_PRED, -TM_PRED};
static const signed char k_y_mode_tree[8] = {-DC_PRED, 2, 4, 6, -V_PRED, -H_PRED, -TM_PRED, -B_PRED};
static const signed char k_uv_mode_tree[6] = {-DC_PRED, 2, -V_PRED, 4, -H_PRED, -TM_PRED};
static const signed char k_b_mode_tree[18] = {-B_DC_PRED, 2, -B_TM_PRED, 4, -B_VE_PRED, 6, 8, 12, -B_HE_PRED, 10,
                                              -B_RD_PRED, -B_VR_PRED, -B_LD_PRED, 14, -B_VL_PRED, 16, -B_HD_PRED, -B_HU_PRED};
static const signed char k_small_mv_tree[14] = {2, 8, 4, 6, -0, -1, -2, -3, 10, 12, -4, -5, -6, -7};
static const signed char k_mv_ref_tree[8] = {-ZEROMV, 2, -NEARESTMV, 4, -NEARMV, 6, -NEWMV, -SPLITMV};
static const signed char k_submv_ref_tree[6] = {-LEFT4X4, 2, -ABOVE4X4, 4, -ZERO4X4, -NEW4X4};
static const signed char k_split_mv_tree[6] = {-3, 2, -2, 4, -0, -1};

static const unsigned char k_zigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
static const unsigned char k_bands[16] = {0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7};
/* Extra bits of the DCT_CAT3..6 tokens, most significant first (DCT_CAT1 and 2 are written inline). */
static const unsigned char k_cat3[] = {173, 148, 140, 0};
static const unsigned char k_cat4[] = {176, 155, 140, 135, 0};
static const unsigned char k_cat5[] = {180, 157, 141, 134, 130, 0};
static const unsigned char k_cat6[] = {254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0};

static const short k_sixtap[8][6] = {
    {0, 0, 128, 0, 0, 0},     {0, -6, 123, 12, -1, 0}, {2, -11, 108, 36, -8, 1}, {0, -9, 93, 50, -6, 0},
    {3, -16, 77, 77, -16, 3}, {0, -6, 50, 93, -9, 0},  {1, -8, 36, 108, -11, 2}, {0, -1, 12, 123, -6, 0},
};
static const short k_bilinear[8][6] = {
    {0, 0, 128, 0, 0, 0}, {0, 0, 112, 16, 0, 0}, {0, 0, 96, 32, 0, 0}, {0, 0, 80, 48, 0, 0},
    {0, 0, 64, 64, 0, 0}, {0, 0, 48, 80, 0, 0},  {0, 0, 32, 96, 0, 0}, {0, 0, 16, 112, 0, 0},
};

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
    short x, y; /* in eighths of a pixel (quarter-pel values, doubled) */
} mv_t;

typedef struct {
    unsigned char y_mode, uv_mode, segment, ref, skip, coded;
    mv_t mv;
    mv_t mvs[16];              /* SPLITMV */
    unsigned char modes[16];   /* B_PRED */
} mb_t;

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
    mb_t *mbs;                /* (mb_rows + 1) x (mb_cols + 1): a border row above and column left */
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
    const short (*filters)[6];

    short coeffs[25 * 16];
    unsigned char edge[32 * 32];   /* a reference block extended past the frame edges */
    unsigned char pass[16 * 21];   /* the horizontal pass of subpixel filtering */
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
    d->mbs = mem_alloc(sizeof(mb_t) * (size_t)(d->mb_cols + 1) * (size_t)(d->mb_rows + 1));
    d->above_ctx = mem_alloc(9 * (size_t)d->mb_cols);
}

static mb_t *mb_at(vp8_decoder_t *d, int row, int col)
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

static int above_b_mode(const mb_t *m, const mb_t *above, int b)
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

static int left_b_mode(const mb_t *m, const mb_t *left, int b)
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
        x = bd_tree(b, k_small_mv_tree, p + SHORT);
    }
    if (x && bd_get(b, p[SIGN]))
        x = -x;
    return x * 2;
}

static mv_t read_mv(vp8_decoder_t *d, bd_t *b)
{
    mv_t mv;

    mv.y = (short)read_mv_component(b, d->ent.mv[0]);
    mv.x = (short)read_mv_component(b, d->ent.mv[1]);
    return mv;
}

static int mv_eq(mv_t a, mv_t b)
{
    return a.x == b.x && a.y == b.y;
}

static int mv_zero(mv_t a)
{
    return !a.x && !a.y;
}

static mv_t clamp_mv(mv_t mv, int left, int right, int top, int bottom)
{
    mv.x = (short)(mv.x < left ? left : mv.x > right ? right : mv.x);
    mv.y = (short)(mv.y < top ? top : mv.y > bottom ? bottom : mv.y);
    return mv;
}

/* The neighbours' vectors (above, left, above-left), weighted into best, nearest and near (section 16.3). */
static void find_near_mvs(vp8_decoder_t *d, const mb_t *m, const mb_t *above, const mb_t *left, mv_t near_mvs[4],
                          int cnt[4])
{
    const mb_t *neighbours[3] = {above, left, above - 1};
    static const int weight[3] = {2, 2, 1};
    int n = 0; /* index of the last vector found */

    memset(near_mvs, 0, sizeof(mv_t) * 4);
    cnt[0] = cnt[1] = cnt[2] = cnt[3] = 0;
    for (int k = 0; k < 3; k++) {
        const mb_t *nb = neighbours[k];
        if (nb->ref == CURRENT)
            continue;
        if (!mv_zero(nb->mv)) {
            mv_t mv = nb->mv;
            if (d->sign_bias[nb->ref] ^ d->sign_bias[m->ref]) {
                mv.x = (short)-mv.x;
                mv.y = (short)-mv.y;
            }
            if (k == 0 || !mv_eq(mv, near_mvs[n]))
                near_mvs[++n] = mv;
            cnt[n] += weight[k];
        } else {
            cnt[0] += weight[k];
        }
    }
    /* Three distinct vectors: the above-left one may merge with nearest. */
    if (cnt[3] && mv_eq(near_mvs[3], near_mvs[1]))
        cnt[1] += 1;
    cnt[3] = (above->y_mode == SPLITMV) * 2 + (left->y_mode == SPLITMV) * 2 + ((above - 1)->y_mode == SPLITMV);
    if (cnt[2] > cnt[1]) {
        int t = cnt[1];
        mv_t v = near_mvs[1];
        cnt[1] = cnt[2];
        cnt[2] = t;
        near_mvs[1] = near_mvs[2];
        near_mvs[2] = v;
    }
    if (cnt[1] >= cnt[0])
        near_mvs[0] = near_mvs[1];
}

static mv_t left_block_mv(const mb_t *m, const mb_t *left, int b)
{
    if (b & 3)
        return m->mvs[b - 1];
    return left->y_mode == SPLITMV ? left->mvs[b + 3] : left->mv;
}

static mv_t above_block_mv(const mb_t *m, const mb_t *above, int b)
{
    if (b >= 4)
        return m->mvs[b - 4];
    return above->y_mode == SPLITMV ? above->mvs[b + 12] : above->mv;
}

static void read_split_mv(vp8_decoder_t *d, bd_t *b, mb_t *m, const mb_t *left, const mb_t *above, mv_t best)
{
    int id = bd_tree(b, k_split_mv_tree, vp8_split_mv_probs), mask = 0;
    const unsigned char *part = vp8_mv_partitions[id];

    for (int j = 0; mask != 0xFFFF; j++) {
        int k = 0, ctx;
        mv_t l, a, mv;
        while (part[k] != j)
            k++;
        l = left_block_mv(m, left, k);
        a = above_block_mv(m, above, k);
        if (mv_eq(l, a))
            ctx = mv_zero(l) ? 4 : 3;
        else if (mv_zero(a))
            ctx = 2;
        else if (mv_zero(l))
            ctx = 1;
        else
            ctx = 0;
        switch (bd_tree(b, k_submv_ref_tree, vp8_submv_ref_probs[ctx])) {
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

static void read_inter_modes(vp8_decoder_t *d, bd_t *b, mb_t *m, int row, int col)
{
    mb_t *above = mb_at(d, row - 1, col), *left = m - 1;
    mv_t near_mvs[4];
    int cnt[4];
    unsigned char probs[4];
    int to_left = -((col + 1) << 7), to_right = (d->mb_cols - col) << 7;
    int to_top = -((row + 1) << 7), to_bottom = (d->mb_rows - row) << 7;

    m->ref = (unsigned char)(bd_get(b, d->prob_last) ? 2 + bd_get(b, d->prob_gf) : LAST);
    find_near_mvs(d, m, above, left, near_mvs, cnt);
    for (int i = 0; i < 4; i++)
        probs[i] = vp8_mv_counts_to_probs[cnt[i]][i];
    m->y_mode = m->uv_mode = (unsigned char)bd_tree(b, k_mv_ref_tree, probs);
    switch (m->y_mode) {
    case NEARESTMV:
        m->mv = clamp_mv(near_mvs[1], to_left, to_right, to_top, to_bottom);
        break;
    case NEARMV:
        m->mv = clamp_mv(near_mvs[2], to_left, to_right, to_top, to_bottom);
        break;
    case ZEROMV:
        m->mv.x = m->mv.y = 0;
        break;
    case NEWMV: {
        mv_t best = clamp_mv(near_mvs[0], to_left, to_right, to_top, to_bottom), mv = read_mv(d, b);
        m->mv.x = (short)(mv.x + best.x);
        m->mv.y = (short)(mv.y + best.y);
        break;
    }
    default:
        read_split_mv(d, b, m, left, above, clamp_mv(near_mvs[0], to_left, to_right, to_top, to_bottom));
        m->mv = m->mvs[15];
        break;
    }
}

static void read_modes(vp8_decoder_t *d, bd_t *b, mb_t *m, int row, int col)
{
    if (d->seg.update_map)
        m->segment = (unsigned char)(bd_get(b, d->seg.probs[0]) ? 2 + bd_get(b, d->seg.probs[2]) : bd_get(b, d->seg.probs[1]));
    else if (d->key)
        m->segment = 0;
    m->skip = (unsigned char)(d->skip_enabled ? bd_get(b, d->prob_skip) : 0);
    if (d->key) {
        const mb_t *above = mb_at(d, row - 1, col), *left = m - 1;
        m->y_mode = (unsigned char)bd_tree(b, k_kf_y_mode_tree, vp8_kf_y_mode_probs);
        if (m->y_mode == B_PRED)
            for (int i = 0; i < 16; i++)
                m->modes[i] = (unsigned char)bd_tree(b, k_b_mode_tree,
                                                     vp8_kf_b_mode_probs[above_b_mode(m, above, i)][left_b_mode(m, left, i)]);
        m->uv_mode = (unsigned char)bd_tree(b, k_uv_mode_tree, vp8_kf_uv_mode_probs);
    } else if (bd_get(b, d->prob_inter)) {
        read_inter_modes(d, b, m, row, col);
        return;
    } else {
        m->y_mode = (unsigned char)bd_tree(b, k_y_mode_tree, d->ent.y_mode);
        if (m->y_mode == B_PRED)
            for (int i = 0; i < 16; i++)
                m->modes[i] = (unsigned char)bd_tree(b, k_b_mode_tree, vp8_default_b_mode_probs);
        m->uv_mode = (unsigned char)bd_tree(b, k_uv_mode_tree, d->ent.uv_mode);
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
    const unsigned char *p = probs[k_bands[first]][ctx];
    int c = first;

    if (!bd_get(b, p[0]))
        return 0;
    for (;;) {
        int v, next;
        if (!bd_get(b, p[1])) { /* a zero: the next token cannot be the end of block */
            if (++c == 16)
                break;
            p = probs[k_bands[c]][0];
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
                v = !bd_get(b, p[9]) ? 11 + read_extra(b, k_cat3) : 19 + read_extra(b, k_cat4);
            else
                v = !bd_get(b, p[10]) ? 35 + read_extra(b, k_cat5) : 67 + read_extra(b, k_cat6);
            next = 2;
        }
        if (bd_bit(b))
            v = -v;
        out[k_zigzag[c]] = (short)(v * dq[c > 0]);
        if (++c == 16)
            break;
        p = probs[k_bands[c]][next];
        if (!bd_get(b, p[0]))
            break;
    }
    return 1;
}

/* All of a macroblock's blocks: Y2 (when present), 16 Y, 4 U, 4 V, with their above and left contexts. */
static int read_tokens(vp8_decoder_t *d, bd_t *b, const mb_t *m, unsigned char *above, unsigned char *left)
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

/* ---- Inverse transforms (section 14) ---- */

static unsigned char clamp255(int v)
{
    return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* The Y2 block's inverse Walsh-Hadamard transform into the Y blocks' DC coefficients. */
static void iwht(short *coeffs)
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

#define COS_M1 20091 /* cos(pi/8) * sqrt(2) - 1, Q16 */
#define SIN 35468    /* sin(pi/8) * sqrt(2), Q16 */

/* Adds a block's inverse DCT to the prediction already at dst. */
static void idct_add(unsigned char *dst, int stride, const short *in)
{
    short tmp[16];

    for (int i = 0; i < 4; i++) {
        int a1 = in[i] + in[8 + i], b1 = in[i] - in[8 + i];
        int c1 = ((in[4 + i] * SIN) >> 16) - (in[12 + i] + ((in[12 + i] * COS_M1) >> 16));
        int d1 = (in[4 + i] + ((in[4 + i] * COS_M1) >> 16)) + ((in[12 + i] * SIN) >> 16);
        tmp[i] = (short)(a1 + d1);
        tmp[12 + i] = (short)(a1 - d1);
        tmp[4 + i] = (short)(b1 + c1);
        tmp[8 + i] = (short)(b1 - c1);
    }
    for (int i = 0; i < 4; i++, dst += stride) {
        const short *r = tmp + 4 * i;
        int a1 = r[0] + r[2], b1 = r[0] - r[2];
        int c1 = ((r[1] * SIN) >> 16) - (r[3] + ((r[3] * COS_M1) >> 16));
        int d1 = (r[1] + ((r[1] * COS_M1) >> 16)) + ((r[3] * SIN) >> 16);
        dst[0] = clamp255(dst[0] + ((a1 + d1 + 4) >> 3));
        dst[3] = clamp255(dst[3] + ((a1 - d1 + 4) >> 3));
        dst[1] = clamp255(dst[1] + ((b1 + c1 + 4) >> 3));
        dst[2] = clamp255(dst[2] + ((b1 - c1 + 4) >> 3));
    }
}

/* ---- Intra prediction (section 12), in place in the frame ---- */

static void predict_dc(unsigned char *p, int stride, int n, int shift)
{
    int sum = 0;

    for (int i = 0; i < n; i++)
        sum += p[-stride + i] + p[i * stride - 1];
    sum = (sum + (1 << (shift - 1))) >> shift;
    for (int i = 0; i < n; i++)
        memset(p + i * stride, sum, (size_t)n);
}

static void predict_block(unsigned char *p, int stride, int n, int mode)
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
    default: /* TM_PRED */
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                p[i * stride + j] = clamp255(p[i * stride - 1] + p[-stride + j] - p[-stride - 1]);
        break;
    }
}

#define AVG3(a, b, c) (unsigned char)(((a) + 2 * (b) + (c) + 2) >> 2)
#define AVG2(a, b) (unsigned char)(((a) + (b) + 1) >> 1)

/* A 4x4 subblock: above[-1..7] and left[0..3] are its edges in the frame. */
static void predict_sub(unsigned char *p, int s, int mode)
{
    const unsigned char *A = p - s;
    int L[4] = {p[-1], p[s - 1], p[2 * s - 1], p[3 * s - 1]}, P = A[-1];
    unsigned char o[4][4];

    switch (mode) {
    case B_DC_PRED:
        predict_dc(p, s, 4, 3);
        return;
    case B_TM_PRED:
        predict_block(p, s, 4, TM_PRED);
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
static void fixup_left(unsigned char *p, int stride, int n, int row, int mode)
{
    if (mode == DC_PRED && row) {
        for (int i = 0; i < n; i++)
            p[i * stride - 1] = p[-stride + i];
    } else {
        for (int i = -1; i < n; i++)
            p[i * stride - 1] = 129;
    }
}

static void fixup_above(unsigned char *p, int stride, int n, int col, int mode)
{
    if (mode == DC_PRED && col) {
        for (int i = 0; i < n; i++)
            p[-stride + i] = p[i * stride - 1];
    } else {
        memset(p - stride - 1, 127, (size_t)n + 1);
    }
    memset(p - stride + n, 127, 4);
}

static void predict_intra(vp8_decoder_t *d, const mb_t *m, unsigned char *y, unsigned char *u, unsigned char *v)
{
    short *c = d->coeffs;
    int s = d->stride, us = d->uv_stride;

    if (m->y_mode == B_PRED) {
        /* The pixels above-right of subblock 3 serve the right column's subblocks 7, 11 and 15 too. */
        for (int r = 4; r < 16; r += 4)
            memcpy(y + (r - 1) * s + 16, y - s + 16, 4);
        for (int i = 0; i < 16; i++) {
            unsigned char *p = y + (i >> 2) * 4 * s + (i & 3) * 4;
            predict_sub(p, s, m->modes[i]);
            idct_add(p, s, c + i * 16);
        }
    } else {
        predict_block(y, s, 16, m->y_mode);
        iwht(c);
        for (int i = 0; i < 16; i++)
            idct_add(y + (i >> 2) * 4 * s + (i & 3) * 4, s, c + i * 16);
    }
    predict_block(u, us, 8, m->uv_mode);
    predict_block(v, us, 8, m->uv_mode);
    for (int i = 0; i < 4; i++) {
        idct_add(u + (i >> 1) * 4 * us + (i & 1) * 4, us, c + (16 + i) * 16);
        idct_add(v + (i >> 1) * 4 * us + (i & 1) * 4, us, c + (20 + i) * 16);
    }
}

/* ---- Inter prediction (section 18) ---- */

/*
 * A bw x bh block of plane `ref` (pw x ph, the macroblock-aligned size)
 * at (x, y) moved by mv, into dst. Pixels past the edges repeat the edge;
 * fractional positions go through the frame's six-tap or bilinear filters,
 * horizontally then vertically.
 */
static void predict_inter_block(vp8_decoder_t *d, unsigned char *dst, int ds, const unsigned char *ref, int rs, int pw,
                                int ph, int x, int y, int bw, int bh, mv_t mv)
{
    int mx = mv.x & 7, my = mv.y & 7;
    const unsigned char *src;
    int ss;

    x += mv.x >> 3;
    y += mv.y >> 3;
    if (x < 2 || y < 2 || x + bw + 3 > pw || y + bh + 3 > ph) {
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
        return;
    }
    {
        const short *fh = d->filters[mx], *fv = d->filters[my];
        for (int r = -2; r < bh + 3; r++) {
            const unsigned char *s = src + r * ss;
            for (int c = 0; c < bw; c++)
                d->pass[(r + 2) * 16 + c] = clamp255((s[c - 2] * fh[0] + s[c - 1] * fh[1] + s[c] * fh[2] + s[c + 1] * fh[3] +
                                                      s[c + 2] * fh[4] + s[c + 3] * fh[5] + 64) >> 7);
        }
        for (int r = 0; r < bh; r++)
            for (int c = 0; c < bw; c++) {
                const unsigned char *t = d->pass + (r + 2) * 16 + c;
                dst[r * ds + c] = clamp255((t[-32] * fv[0] + t[-16] * fv[1] + t[0] * fv[2] + t[16] * fv[3] + t[32] * fv[4] +
                                            t[48] * fv[5] + 64) >> 7);
            }
    }
}

static short chroma_half(int v)
{
    return (short)(v < 0 ? (v - 1) / 2 : (v + 1) / 2);
}

static void predict_inter(vp8_decoder_t *d, const mb_t *m, int row, int col, unsigned char *y, unsigned char *u,
                          unsigned char *v)
{
    const frame_t *r = &d->frames[d->ref[m->ref]];
    int s = d->stride, us = d->uv_stride, pw = d->mb_cols * 16, ph = d->mb_rows * 16, x = col * 16, yy = row * 16;
    short *c = d->coeffs;
    mv_t uvmv[4];

    if (m->y_mode != SPLITMV) {
        mv_t mv = m->mv;
        predict_inter_block(d, y, s, r->y, s, pw, ph, x, yy, 16, 16, mv);
        mv.x = chroma_half(mv.x);
        mv.y = chroma_half(mv.y);
        if (d->version == 3) {
            mv.x = (short)(mv.x & ~7);
            mv.y = (short)(mv.y & ~7);
        }
        for (int i = 0; i < 4; i++)
            uvmv[i] = mv;
        iwht(c);
    } else {
        for (int b = 0; b < 16; b++)
            predict_inter_block(d, y + (b >> 2) * 4 * s + (b & 3) * 4, s, r->y, s, pw, ph, x + (b & 3) * 4, yy + (b >> 2) * 4,
                                4, 4, m->mvs[b]);
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
        predict_inter_block(d, u + by * us + bx, us, r->u, us, pw / 2, ph / 2, x / 2 + bx, yy / 2 + by, 4, 4, uvmv[i]);
        predict_inter_block(d, v + by * us + bx, us, r->v, us, pw / 2, ph / 2, x / 2 + bx, yy / 2 + by, 4, 4, uvmv[i]);
    }
    for (int i = 0; i < 16; i++)
        idct_add(y + (i >> 2) * 4 * s + (i & 3) * 4, s, c + i * 16);
    for (int i = 0; i < 4; i++) {
        idct_add(u + (i >> 1) * 4 * us + (i & 1) * 4, us, c + (16 + i) * 16);
        idct_add(v + (i >> 1) * 4 * us + (i & 1) * 4, us, c + (20 + i) * 16);
    }
}

/* ---- Loop filter (section 15) ---- */

static int s8(int v)
{
    return v < -128 ? -128 : v > 127 ? 127 : v;
}

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

/* p points at q0; `step` crosses the edge. */
static int simple_ok(const unsigned char *p, int step, int limit)
{
    return iabs(p[-step] - p[0]) * 2 + (iabs(p[-2 * step] - p[step]) >> 1) <= limit;
}

static int normal_ok(const unsigned char *p, int step, int edge, int interior)
{
    int p3 = p[-4 * step], p2 = p[-3 * step], p1 = p[-2 * step], p0 = p[-step];
    int q0 = p[0], q1 = p[step], q2 = p[2 * step], q3 = p[3 * step];

    return simple_ok(p, step, 2 * edge + interior) && iabs(p3 - p2) <= interior && iabs(p2 - p1) <= interior &&
           iabs(p1 - p0) <= interior && iabs(q3 - q2) <= interior && iabs(q2 - q1) <= interior &&
           iabs(q1 - q0) <= interior;
}

static int hev(const unsigned char *p, int step, int threshold)
{
    return iabs(p[-2 * step] - p[-step]) > threshold || iabs(p[step] - p[0]) > threshold;
}

static void filter_common(unsigned char *p, int step, int outer)
{
    int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    int a = 3 * (q0 - p0), f1, f2;

    if (outer)
        a += s8(p1 - q1);
    a = s8(a);
    f1 = (a + 4 > 127 ? 127 : a + 4) >> 3;
    f2 = (a + 3 > 127 ? 127 : a + 3) >> 3;
    p[-step] = clamp255(p0 + f2);
    p[0] = clamp255(q0 - f1);
    if (!outer) {
        a = (f1 + 1) >> 1;
        p[-2 * step] = clamp255(p1 + a);
        p[step] = clamp255(q1 - a);
    }
}

static void filter_mb(unsigned char *p, int step)
{
    int p2 = p[-3 * step], p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step], q2 = p[2 * step];
    int w = s8(s8(p1 - q1) + 3 * (q0 - p0)), a;

    a = (27 * w + 63) >> 7;
    p[-step] = clamp255(p0 + a);
    p[0] = clamp255(q0 - a);
    a = (18 * w + 63) >> 7;
    p[-2 * step] = clamp255(p1 + a);
    p[step] = clamp255(q1 - a);
    a = (9 * w + 63) >> 7;
    p[-3 * step] = clamp255(p2 + a);
    p[2 * step] = clamp255(q2 - a);
}

/* An edge of n pixels: `step` crosses it, `along` follows it. */
static void edge_normal(unsigned char *p, int step, int along, int n, int edge, int interior, int threshold, int mb)
{
    for (int i = 0; i < n; i++, p += along)
        if (normal_ok(p, step, edge, interior)) {
            if (mb && !hev(p, step, threshold))
                filter_mb(p, step);
            else if (mb)
                filter_common(p, step, 1);
            else
                filter_common(p, step, hev(p, step, threshold));
        }
}

static void edge_simple(unsigned char *p, int step, int along, int limit)
{
    for (int i = 0; i < 16; i++, p += along)
        if (simple_ok(p, step, limit))
            filter_common(p, step, 1);
}

static void loop_filter(vp8_decoder_t *d, const frame_t *f)
{
    int s = d->stride, us = d->uv_stride;

    for (int row = 0; row < d->mb_rows; row++)
        for (int col = 0; col < d->mb_cols; col++) {
            const mb_t *m = mb_at(d, row, col);
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
                    edge_simple(y, s, 1, mb_limit);
                if (inner)
                    for (int k = 4; k < 16; k += 4)
                        edge_simple(y + k * s, s, 1, b_limit);
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
                edge_normal(y, s, 1, 16, level + 2, interior, threshold, 1);
                edge_normal(u, us, 1, 8, level + 2, interior, threshold, 1);
                edge_normal(v, us, 1, 8, level + 2, interior, threshold, 1);
            }
            if (inner) {
                for (int k = 4; k < 16; k += 4)
                    edge_normal(y + k * s, s, 1, 16, level, interior, threshold, 0);
                edge_normal(u + 4 * us, us, 1, 8, level, interior, threshold, 0);
                edge_normal(v + 4 * us, us, 1, 8, level, interior, threshold, 0);
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
        d->filters = d->version ? k_bilinear : k_sixtap;
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
            mb_t *m = mb_at(d, row, col);
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
                fixup_left(y, d->stride, 16, row, m->y_mode);
                fixup_left(u, d->uv_stride, 8, row, m->uv_mode);
                fixup_left(v, d->uv_stride, 8, row, m->uv_mode);
                if (row == 0)
                    y[-d->stride - 1] = 127;
            }
            if (row == 0) {
                fixup_above(y, d->stride, 16, col, m->y_mode);
                fixup_above(u, d->uv_stride, 8, col, m->uv_mode);
                fixup_above(v, d->uv_stride, 8, col, m->uv_mode);
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
