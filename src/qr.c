/* Follows the structure of Project Nayuki's QR Code generator (MIT), reduced to what Silicord needs. */
#include <string.h>
#include "qr.h"
#include "mem.h"

#define MAX_CODEWORDS 346 /* raw codewords of version 10 */

/* Level M, indexed by version. */
static const unsigned char k_ecc_per_block[QR_MAX_VERSION + 1] = {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
static const unsigned char k_blocks[QR_MAX_VERSION + 1]        = {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5};
#define FORMAT_BITS_M 0

typedef struct {
    qr_t *qr;
    unsigned char *is_function;
    int version;
} ctx_t;

static void set_module(ctx_t *c, int x, int y, int dark)
{
    c->qr->module[y * c->qr->size + x] = (unsigned char)(dark != 0);
}

static void set_function(ctx_t *c, int x, int y, int dark)
{
    set_module(c, x, y, dark);
    c->is_function[y * c->qr->size + x] = 1;
}

static int bit(unsigned v, int i)
{
    return (v >> i) & 1;
}

static int abs_i(int v)
{
    return v < 0 ? -v : v;
}

static int raw_data_modules(int ver)
{
    int n = (16 * ver + 128) * ver + 64;

    if (ver >= 2) {
        int align = ver / 7 + 2;
        n -= (25 * align - 10) * align - 55;
        if (ver >= 7)
            n -= 36;
    }
    return n;
}

static int data_codewords(int ver)
{
    return raw_data_modules(ver) / 8 - k_ecc_per_block[ver] * k_blocks[ver];
}

/* ---- Reed-Solomon over GF(2^8), polynomial 0x11D ---- */

static unsigned char gf_mul(unsigned char x, unsigned char y)
{
    int z = 0;

    for (int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return (unsigned char)z;
}

static void rs_divisor(int degree, unsigned char *out)
{
    unsigned char root = 1;

    memset(out, 0, (size_t)degree);
    out[degree - 1] = 1;
    for (int i = 0; i < degree; i++) {
        for (int j = 0; j < degree; j++) {
            out[j] = gf_mul(out[j], root);
            if (j + 1 < degree)
                out[j] ^= out[j + 1];
        }
        root = gf_mul(root, 0x02);
    }
}

static void rs_remainder(const unsigned char *data, int n, const unsigned char *div, int degree,
                         unsigned char *out)
{
    memset(out, 0, (size_t)degree);
    for (int i = 0; i < n; i++) {
        unsigned char factor = data[i] ^ out[0];
        memmove(out, out + 1, (size_t)degree - 1);
        out[degree - 1] = 0;
        for (int j = 0; j < degree; j++)
            out[j] ^= gf_mul(div[j], factor);
    }
}

/* ---- Function patterns ---- */

static void draw_finder(ctx_t *c, int x, int y)
{
    int size = c->qr->size;

    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -4; dx <= 4; dx++) {
            int dist = abs_i(dx) > abs_i(dy) ? abs_i(dx) : abs_i(dy);
            int xx = x + dx, yy = y + dy;
            if (xx >= 0 && xx < size && yy >= 0 && yy < size)
                set_function(c, xx, yy, dist != 2 && dist != 4);
        }
    }
}

static void draw_alignment(ctx_t *c, int x, int y)
{
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++)
            set_function(c, x + dx, y + dy, (abs_i(dx) > abs_i(dy) ? abs_i(dx) : abs_i(dy)) != 1);
}

static void draw_format(ctx_t *c, int mask)
{
    int size = c->qr->size;
    unsigned data = (FORMAT_BITS_M << 3) | (unsigned)mask;
    unsigned rem = data, bits;

    for (int i = 0; i < 10; i++)
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    bits = ((data << 10) | rem) ^ 0x5412;

    for (int i = 0; i <= 5; i++)
        set_function(c, 8, i, bit(bits, i));
    set_function(c, 8, 7, bit(bits, 6));
    set_function(c, 8, 8, bit(bits, 7));
    set_function(c, 7, 8, bit(bits, 8));
    for (int i = 9; i < 15; i++)
        set_function(c, 14 - i, 8, bit(bits, i));

    for (int i = 0; i < 8; i++)
        set_function(c, size - 1 - i, 8, bit(bits, i));
    for (int i = 8; i < 15; i++)
        set_function(c, 8, size - 15 + i, bit(bits, i));
    set_function(c, 8, size - 8, 1);
}

static void draw_version(ctx_t *c)
{
    int size = c->qr->size;
    unsigned rem = (unsigned)c->version, bits;

    if (c->version < 7)
        return;
    for (int i = 0; i < 12; i++)
        rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    bits = ((unsigned)c->version << 12) | rem;
    for (int i = 0; i < 18; i++) {
        int a = size - 11 + i % 3, b = i / 3;
        set_function(c, a, b, bit(bits, i));
        set_function(c, b, a, bit(bits, i));
    }
}

static void draw_function_patterns(ctx_t *c)
{
    int size = c->qr->size;

    for (int i = 0; i < size; i++) {
        set_function(c, 6, i, i % 2 == 0);
        set_function(c, i, 6, i % 2 == 0);
    }
    draw_finder(c, 3, 3);
    draw_finder(c, size - 4, 3);
    draw_finder(c, 3, size - 4);

    if (c->version >= 2) {
        int n = c->version / 7 + 2;
        int step = (c->version * 8 + n * 3 + 5) / (n * 4 - 4) * 2;
        int pos[7];

        pos[0] = 6;
        for (int i = n - 1, p = size - 7; i >= 1; i--, p -= step)
            pos[i] = p;
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                if (!((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)))
                    draw_alignment(c, pos[i], pos[j]);
    }
    draw_format(c, 0); /* placeholder, overwritten once the mask is chosen */
    draw_version(c);
}

/* ---- Data placement and masking ---- */

static void draw_codewords(ctx_t *c, const unsigned char *data, int n)
{
    int size = c->qr->size;
    int i = 0;

    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6)
            right = 5;
        for (int vert = 0; vert < size; vert++) {
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                int upward = ((right + 1) & 2) == 0;
                int y = upward ? size - 1 - vert : vert;
                if (!c->is_function[y * size + x] && i < n * 8) {
                    set_module(c, x, y, bit(data[i >> 3], 7 - (i & 7)));
                    i++;
                }
            }
        }
    }
}

static int mask_bit(int mask, int x, int y)
{
    switch (mask) {
    case 0:  return (x + y) % 2 == 0;
    case 1:  return y % 2 == 0;
    case 2:  return x % 3 == 0;
    case 3:  return (x + y) % 3 == 0;
    case 4:  return (x / 3 + y / 2) % 2 == 0;
    case 5:  return x * y % 2 + x * y % 3 == 0;
    case 6:  return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

static void apply_mask(ctx_t *c, int mask)
{
    int size = c->qr->size;

    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            if (!c->is_function[y * size + x] && mask_bit(mask, x, y))
                c->qr->module[y * size + x] ^= 1;
}

/* Penalty rules from ISO/IEC 18004, section 7.8.3. */
static long penalty(const qr_t *qr)
{
    static const unsigned char finder_like[2][11] = {
        {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0},
        {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1},
    };
    int size = qr->size;
    long score = 0, dark = 0;

    for (int pass = 0; pass < 2; pass++) {
        for (int a = 0; a < size; a++) {
            int run = 0, prev = -1;
            for (int b = 0; b < size; b++) {
                int m = pass ? qr_dark(qr, a, b) : qr_dark(qr, b, a);
                if (m == prev) {
                    run++;
                    if (run == 5)
                        score += 3;
                    else if (run > 5)
                        score++;
                } else {
                    prev = m;
                    run = 1;
                }
                if (b + 11 <= size) {
                    for (int f = 0; f < 2; f++) {
                        int k = 0;
                        while (k < 11 && (pass ? qr_dark(qr, a, b + k) : qr_dark(qr, b + k, a)) == finder_like[f][k])
                            k++;
                        if (k == 11)
                            score += 40;
                    }
                }
            }
        }
    }
    for (int y = 0; y + 1 < size; y++)
        for (int x = 0; x + 1 < size; x++) {
            int m = qr_dark(qr, x, y);
            if (m == qr_dark(qr, x + 1, y) && m == qr_dark(qr, x, y + 1) && m == qr_dark(qr, x + 1, y + 1))
                score += 3;
        }
    for (int i = 0; i < size * size; i++)
        dark += qr->module[i];
    {
        long total = (long)size * size;
        long k = (abs_i((int)(dark * 20 - total * 10)) + total - 1) / total - 1;
        score += (k > 0 ? k : 0) * 10;
    }
    return score;
}

/* ---- Encoding ---- */

static void put_bits(unsigned char *buf, int *len, unsigned v, int n)
{
    for (int i = n - 1; i >= 0; i--, (*len)++)
        if ((v >> i) & 1)
            buf[*len >> 3] |= (unsigned char)(0x80 >> (*len & 7));
}

int qr_encode(const void *data, size_t n, qr_t *out)
{
    const unsigned char *bytes = data;
    unsigned char cw[MAX_CODEWORDS] = {0};
    unsigned char final[MAX_CODEWORDS];
    unsigned char div[30], ecc[30];
    int version, cap, bits = 0;
    ctx_t c;

    for (version = 1; version <= QR_MAX_VERSION; version++) {
        int count_bits = version <= 9 ? 8 : 16;
        if (4 + count_bits + (int)n * 8 <= data_codewords(version) * 8)
            break;
    }
    if (version > QR_MAX_VERSION)
        return 0;
    cap = data_codewords(version);

    put_bits(cw, &bits, 0x4, 4); /* byte mode */
    put_bits(cw, &bits, (unsigned)n, version <= 9 ? 8 : 16);
    for (size_t i = 0; i < n; i++)
        put_bits(cw, &bits, bytes[i], 8);
    put_bits(cw, &bits, 0, cap * 8 - bits < 4 ? cap * 8 - bits : 4);
    put_bits(cw, &bits, 0, (8 - bits % 8) % 8);
    for (unsigned char pad = 0xEC; bits < cap * 8; pad ^= 0xEC ^ 0x11)
        put_bits(cw, &bits, pad, 8);

    /* Split into blocks, append ECC, interleave. */
    {
        int blocks = k_blocks[version];
        int ecc_len = k_ecc_per_block[version];
        int raw = raw_data_modules(version) / 8;
        int short_blocks = blocks - raw % blocks;
        int short_len = raw / blocks;
        int k = 0, o = 0;
        unsigned char block[5][MAX_CODEWORDS / 4];

        rs_divisor(ecc_len, div);
        for (int i = 0; i < blocks; i++) {
            int dat_len = short_len - ecc_len + (i < short_blocks ? 0 : 1);
            int p = 0;
            memcpy(block[i], cw + k, (size_t)dat_len);
            p = dat_len;
            if (i < short_blocks)
                block[i][p++] = 0;
            rs_remainder(cw + k, dat_len, div, ecc_len, ecc);
            memcpy(block[i] + p, ecc, (size_t)ecc_len);
            k += dat_len;
        }
        for (int i = 0; i < short_len + 1; i++)
            for (int j = 0; j < blocks; j++)
                if (i != short_len - ecc_len || j >= short_blocks)
                    final[o++] = block[j][i];
    }

    memset(out, 0, sizeof *out);
    out->size = version * 4 + 17;
    c.qr = out;
    c.version = version;
    c.is_function = mem_alloc((size_t)out->size * out->size);
    draw_function_patterns(&c);
    draw_codewords(&c, final, raw_data_modules(version) / 8);

    {
        int best = 0;
        long best_score = -1;
        for (int mask = 0; mask < 8; mask++) {
            long s;
            apply_mask(&c, mask);
            draw_format(&c, mask);
            s = penalty(out);
            if (best_score < 0 || s < best_score) {
                best = mask;
                best_score = s;
            }
            apply_mask(&c, mask); /* undo */
        }
        apply_mask(&c, best);
        draw_format(&c, best);
    }
    mem_free(c.is_function);
    return 1;
}
