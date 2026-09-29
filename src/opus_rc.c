#include "opus_rc.h"

#define TOP (1u << 23) /* rng stays above this after normalization */

int rc_ilog(unsigned x)
{
    int n = 0;

    while (x) {
        n++;
        x >>= 1;
    }
    return n;
}

static int read_byte(opus_rc_t *rc)
{
    return rc->offs < rc->storage ? rc->buf[rc->offs++] : 0;
}

static int read_byte_from_end(opus_rc_t *rc)
{
    return rc->end_offs < rc->storage ? rc->buf[rc->storage - ++rc->end_offs] : 0;
}

/* Keeps rng above 2^23: each step shifts in the pending bit and 7 bits of the next byte. */
static void normalize(opus_rc_t *rc)
{
    while (rc->rng <= TOP) {
        int sym;
        rc->nbits_total += 8;
        rc->rng <<= 8;
        sym = rc->rem;
        rc->rem = read_byte(rc);
        sym = (sym << 8 | rc->rem) >> 1;
        rc->val = ((rc->val << 8) + (255 & ~(unsigned)sym)) & 0x7FFFFFFF;
    }
}

void rc_init(opus_rc_t *rc, const unsigned char *buf, unsigned n)
{
    rc->buf = buf;
    rc->storage = n;
    rc->offs = rc->end_offs = 0;
    rc->end_window = 0;
    rc->nend_bits = 0;
    rc->nbits_total = 9;
    rc->rng = 128;
    rc->rem = read_byte(rc);
    rc->val = rc->rng - 1 - (unsigned)(rc->rem >> 1);
    rc->ext = 0;
    normalize(rc);
}

unsigned rc_decode(opus_rc_t *rc, unsigned ft)
{
    unsigned s;

    rc->ext = rc->rng / ft;
    s = rc->val / rc->ext;
    return ft - (s + 1 < ft ? s + 1 : ft);
}

unsigned rc_decode_bin(opus_rc_t *rc, unsigned bits)
{
    unsigned s;

    rc->ext = rc->rng >> bits;
    s = rc->val / rc->ext;
    return (1u << bits) - (s + 1 < (1u << bits) ? s + 1 : (1u << bits));
}

void rc_update(opus_rc_t *rc, unsigned fl, unsigned fh, unsigned ft)
{
    unsigned s = rc->ext * (ft - fh);

    rc->val -= s;
    rc->rng = fl > 0 ? rc->ext * (fh - fl) : rc->rng - s;
    normalize(rc);
}

int rc_bit_logp(opus_rc_t *rc, unsigned logp)
{
    unsigned s = rc->rng >> logp;
    int bit = rc->val < s;

    if (!bit)
        rc->val -= s;
    rc->rng = bit ? s : rc->rng - s;
    normalize(rc);
    return bit;
}

int rc_icdf(opus_rc_t *rc, const unsigned char *icdf, unsigned ftb)
{
    unsigned r = rc->rng >> ftb, s = rc->rng, t;
    int k = -1;

    do {
        t = s;
        s = r * icdf[++k];
    } while (rc->val < s);
    rc->val -= s;
    rc->rng = t - s;
    normalize(rc);
    return k;
}

unsigned rc_bits(opus_rc_t *rc, unsigned n)
{
    unsigned window = rc->end_window, ret;
    int available = rc->nend_bits;

    if ((unsigned)available < n) {
        do {
            window |= (unsigned)read_byte_from_end(rc) << available;
            available += 8;
        } while (available <= 24);
    }
    ret = window & ((1u << n) - 1);
    rc->end_window = window >> n;
    rc->nend_bits = available - (int)n;
    rc->nbits_total += (int)n;
    return ret;
}

unsigned rc_uint(opus_rc_t *rc, unsigned ft)
{
    unsigned s, t;
    int ftb;

    ft--;
    ftb = rc_ilog(ft);
    if (ftb > 8) {
        unsigned ft1;
        ftb -= 8;
        ft1 = (ft >> ftb) + 1;
        s = rc_decode(rc, ft1);
        rc_update(rc, s, s + 1, ft1);
        t = s << ftb | rc_bits(rc, (unsigned)ftb);
        return t <= ft ? t : ft; /* a corrupt value saturates */
    }
    ft++;
    s = rc_decode(rc, ft);
    rc_update(rc, s, s + 1, ft);
    return s;
}

int rc_tell(const opus_rc_t *rc)
{
    return rc->nbits_total - rc_ilog(rc->rng);
}

unsigned rc_tell_frac(const opus_rc_t *rc)
{
    int lg = rc_ilog(rc->rng);
    unsigned r = rc->rng >> (lg - 16);

    /* Three more bits of lg, one squaring each. */
    for (int i = 0; i < 3; i++) {
        unsigned b;
        r = r * r >> 15;
        b = r >> 16;
        lg = 2 * lg + (int)b;
        r >>= b;
    }
    return (unsigned)(rc->nbits_total * 8 - lg);
}

/* ---- Encoder ---- */

#define CODE_TOP (1u << 31)
#define CODE_SHIFT 23

static void write_byte(opus_rce_t *e, unsigned v)
{
    if (e->offs + e->end_offs >= e->storage)
        e->error = 1;
    else
        e->buf[e->offs++] = (unsigned char)v;
}

static void write_byte_at_end(opus_rce_t *e, unsigned v)
{
    if (e->offs + e->end_offs >= e->storage)
        e->error = 1;
    else
        e->buf[e->storage - ++e->end_offs] = (unsigned char)v;
}

/* Outputs a byte, holding back runs of 255 until a carry is known. */
static void carry_out(opus_rce_t *e, int c)
{
    if (c != 255) {
        int carry = c >> 8;
        if (e->rem >= 0)
            write_byte(e, (unsigned)(e->rem + carry));
        for (; e->ext > 0; e->ext--)
            write_byte(e, (255u + (unsigned)carry) & 255);
        e->rem = c & 255;
    } else {
        e->ext++;
    }
}

static void enc_normalize(opus_rce_t *e)
{
    while (e->rng <= TOP) {
        carry_out(e, (int)(e->val >> CODE_SHIFT));
        e->val = (e->val << 8) & (CODE_TOP - 1);
        e->rng <<= 8;
        e->nbits_total += 8;
    }
}

void rce_init(opus_rce_t *e, unsigned char *buf, unsigned n)
{
    e->buf = buf;
    e->storage = n;
    e->offs = e->end_offs = e->end_window = 0;
    e->nend_bits = 0;
    e->nbits_total = 33;
    e->rng = CODE_TOP;
    e->rem = -1;
    e->val = e->ext = 0;
    e->error = 0;
}

void rce_encode(opus_rce_t *e, unsigned fl, unsigned fh, unsigned ft)
{
    unsigned r = e->rng / ft;

    if (fl > 0) {
        e->val += e->rng - r * (ft - fl);
        e->rng = r * (fh - fl);
    } else {
        e->rng -= r * (ft - fh);
    }
    enc_normalize(e);
}

void rce_encode_bin(opus_rce_t *e, unsigned fl, unsigned fh, unsigned bits)
{
    unsigned r = e->rng >> bits;

    if (fl > 0) {
        e->val += e->rng - r * ((1u << bits) - fl);
        e->rng = r * (fh - fl);
    } else {
        e->rng -= r * ((1u << bits) - fh);
    }
    enc_normalize(e);
}

void rce_bit_logp(opus_rce_t *e, int val, unsigned logp)
{
    unsigned s = e->rng >> logp, r = e->rng - s;

    if (val)
        e->val += r;
    e->rng = val ? s : r;
    enc_normalize(e);
}

void rce_icdf(opus_rce_t *e, int s, const unsigned char *icdf, unsigned ftb)
{
    unsigned r = e->rng >> ftb;

    if (s > 0) {
        e->val += e->rng - r * icdf[s - 1];
        e->rng = r * (unsigned)(icdf[s - 1] - icdf[s]);
    } else {
        e->rng -= r * icdf[s];
    }
    enc_normalize(e);
}

void rce_bits(opus_rce_t *e, unsigned fl, unsigned n)
{
    unsigned window = e->end_window;
    int used = e->nend_bits;

    if (used + (int)n > 32) {
        do {
            write_byte_at_end(e, window & 255);
            window >>= 8;
            used -= 8;
        } while (used >= 8);
    }
    window |= fl << used;
    used += (int)n;
    e->end_window = window;
    e->nend_bits = used;
    e->nbits_total += (int)n;
}

void rce_uint(opus_rce_t *e, unsigned fl, unsigned ft)
{
    int ftb;

    ft--;
    ftb = rc_ilog(ft);
    if (ftb > 8) {
        unsigned ft1, f;
        ftb -= 8;
        ft1 = (ft >> ftb) + 1;
        f = fl >> ftb;
        rce_encode(e, f, f + 1, ft1);
        rce_bits(e, fl & ((1u << ftb) - 1), (unsigned)ftb);
    } else {
        rce_encode(e, fl, fl + 1, ft + 1);
    }
}

int rce_tell(const opus_rce_t *e)
{
    return e->nbits_total - rc_ilog(e->rng);
}

unsigned rce_tell_frac(const opus_rce_t *e)
{
    opus_rc_t view;

    view.nbits_total = e->nbits_total;
    view.rng = e->rng;
    return rc_tell_frac(&view);
}

int rce_done(opus_rce_t *e)
{
    int l = 32 - rc_ilog(e->rng), used;
    unsigned msk = (CODE_TOP - 1) >> l, end = (e->val + msk) & ~msk, window;

    if ((end | msk) >= e->val + e->rng) {
        l++;
        msk >>= 1;
        end = (e->val + msk) & ~msk;
    }
    while (l > 0) {
        carry_out(e, (int)(end >> CODE_SHIFT));
        end = (end << 8) & (CODE_TOP - 1);
        l -= 8;
    }
    if (e->rem >= 0 || e->ext > 0)
        carry_out(e, 0);
    window = e->end_window;
    used = e->nend_bits;
    while (used >= 8) {
        write_byte_at_end(e, window & 255);
        window >>= 8;
        used -= 8;
    }
    if (!e->error) {
        for (unsigned i = e->offs; i < e->storage - e->end_offs; i++)
            e->buf[i] = 0;
        if (used > 0) {
            if (e->end_offs >= e->storage) {
                e->error = 1;
            } else {
                l = -l;
                if (e->offs + e->end_offs >= e->storage && l < used) {
                    window &= (1u << l) - 1;
                    e->error = 1;
                }
                e->buf[e->storage - e->end_offs - 1] |= (unsigned char)window;
            }
        }
    }
    return !e->error;
}
