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
    rc->error = 0;
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
        if (t <= ft)
            return t;
        rc->error = 1;
        return ft;
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
