#include <string.h>
#include "opus_celt.h"
#include "opus_math.h"

#define BITRES 3
#define MAX_LM 3
#define SHORT_MDCT 120
#define MAX_PERIOD 1024
#define MAX_FINE_BITS 8
#define FINE_OFFSET 21
#define QTHETA_OFFSET 4
#define QTHETA_OFFSET_TWOPHASE 16
#define ALLOC_STEPS 6
#define COMBFILTER_MINPERIOD 15
#define SPREAD_NONE 0
#define SPREAD_AGGRESSIVE 3
#define SPREAD_NORMAL 2
#define LAPLACE_MINP 1
#define LAPLACE_NMIN 16
#define EPSILON 1e-15f
#define SIG_SCALE 32768.f

/* ---- The mode: numeric tables of RFC 6716's normative reference ---- */

static const short k_ebands[CELT_BANDS + 1] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  10, 12,
                                               14, 16, 20, 24, 28, 34, 40, 48, 60, 78, 100};

static const unsigned char k_alloc[11 * CELT_BANDS] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    90,  80,  75,  69,  63,  56,  49,  40,  34,  29,  20,  18,  10,  0,   0,   0,   0,   0,   0,   0,   0,
    110, 100, 90,  84,  78,  71,  65,  58,  51,  45,  39,  32,  26,  20,  12,  0,   0,   0,   0,   0,   0,
    118, 110, 103, 93,  86,  80,  75,  70,  65,  59,  53,  47,  40,  31,  23,  15,  4,   0,   0,   0,   0,
    126, 119, 112, 104, 95,  89,  83,  78,  72,  66,  60,  54,  47,  39,  32,  25,  17,  12,  1,   0,   0,
    134, 127, 120, 114, 103, 97,  91,  85,  78,  72,  66,  60,  54,  47,  41,  35,  29,  23,  16,  10,  1,
    144, 137, 130, 124, 113, 107, 101, 95,  88,  82,  76,  70,  64,  57,  51,  45,  39,  33,  26,  15,  1,
    152, 145, 138, 132, 123, 117, 111, 105, 98,  92,  86,  80,  74,  67,  61,  55,  49,  43,  36,  20,  1,
    162, 155, 148, 142, 133, 127, 121, 115, 108, 102, 96,  90,  84,  77,  71,  65,  59,  53,  46,  30,  1,
    172, 165, 158, 152, 143, 137, 131, 125, 118, 112, 106, 100, 94,  87,  81,  75,  69,  63,  56,  45,  20,
    200, 200, 200, 200, 200, 200, 200, 200, 198, 193, 188, 183, 178, 173, 168, 163, 158, 153, 148, 129, 104,
};

static const short k_log_n[CELT_BANDS] = {0, 0, 0, 0, 0, 0, 0, 0, 8, 8, 8, 8, 16, 16, 16, 21, 21, 24, 29, 34, 36};

static const short k_cache_index[105] = {
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  0,   0,   0,   0,   41,  41,  41,  82,  82,  123, 164, 200, 222,
    0,   0,   0,   0,   0,   0,   0,   0,   41,  41,  41,  41,  123, 123, 123, 164, 164, 240, 266, 283, 295,
    41,  41,  41,  41,  41,  41,  41,  41,  123, 123, 123, 123, 240, 240, 240, 266, 266, 305, 318, 328, 336,
    123, 123, 123, 123, 123, 123, 123, 123, 240, 240, 240, 240, 305, 305, 305, 318, 318, 343, 351, 358, 364,
    240, 240, 240, 240, 240, 240, 240, 240, 305, 305, 305, 305, 343, 343, 343, 351, 351, 370, 376, 382, 387,
};

static const unsigned char k_cache_bits[392] = {
    40,  7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,
    7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   40,  15,  23,  28,  31,
    34,  36,  38,  39,  41,  42,  43,  44,  45,  46,  47,  47,  49,  50,  51,  52,  53,  54,  55,  55,  57,  58,  59,
    60,  61,  62,  63,  63,  65,  66,  67,  68,  69,  70,  71,  71,  40,  20,  33,  41,  48,  53,  57,  61,  64,  66,
    69,  71,  73,  75,  76,  78,  80,  82,  85,  87,  89,  91,  92,  94,  96,  98,  101, 103, 105, 107, 108, 110, 112,
    114, 117, 119, 121, 123, 124, 126, 128, 40,  23,  39,  51,  60,  67,  73,  79,  83,  87,  91,  94,  97,  100, 102,
    105, 107, 111, 115, 118, 121, 124, 126, 129, 131, 135, 139, 142, 145, 148, 150, 153, 155, 159, 163, 166, 169, 172,
    174, 177, 179, 35,  28,  49,  65,  78,  89,  99,  107, 114, 120, 126, 132, 136, 141, 145, 149, 153, 159, 165, 171,
    176, 180, 185, 189, 192, 199, 205, 211, 216, 220, 225, 229, 232, 239, 245, 251, 21,  33,  58,  79,  97,  112, 125,
    137, 148, 157, 166, 174, 182, 189, 195, 201, 207, 217, 227, 235, 243, 251, 17,  35,  63,  86,  106, 123, 139, 152,
    165, 177, 187, 197, 206, 214, 222, 230, 237, 250, 25,  31,  55,  75,  91,  105, 117, 128, 138, 146, 154, 161, 168,
    174, 180, 185, 190, 200, 208, 215, 222, 229, 235, 240, 245, 255, 16,  36,  65,  89,  110, 128, 144, 159, 173, 185,
    196, 207, 217, 226, 234, 242, 250, 11,  41,  74,  103, 128, 151, 172, 191, 209, 225, 241, 255, 9,   43,  79,  110,
    138, 163, 186, 207, 227, 246, 12,  39,  71,  99,  123, 144, 164, 182, 198, 214, 228, 241, 253, 9,   44,  81,  113,
    142, 168, 192, 214, 235, 255, 7,   49,  90,  127, 160, 191, 220, 247, 6,   51,  95,  134, 170, 203, 234, 7,   47,
    87,  123, 155, 184, 212, 237, 6,   52,  97,  137, 174, 208, 240, 5,   57,  106, 151, 192, 231, 5,   59,  111, 158,
    202, 243, 5,   55,  103, 147, 187, 224, 5,   60,  113, 161, 206, 248, 4,   65,  122, 175, 224, 4,   67,  127, 182,
    234,
};

static const unsigned char k_cache_caps[168] = {
    224, 224, 224, 224, 224, 224, 224, 224, 160, 160, 160, 160, 185, 185, 185, 178, 178, 168, 134, 61,  37,
    224, 224, 224, 224, 224, 224, 224, 224, 240, 240, 240, 240, 207, 207, 207, 198, 198, 183, 144, 66,  40,
    160, 160, 160, 160, 160, 160, 160, 160, 185, 185, 185, 185, 193, 193, 193, 183, 183, 172, 138, 64,  38,
    240, 240, 240, 240, 240, 240, 240, 240, 207, 207, 207, 207, 204, 204, 204, 193, 193, 180, 143, 66,  40,
    185, 185, 185, 185, 185, 185, 185, 185, 193, 193, 193, 193, 193, 193, 193, 183, 183, 172, 138, 65,  39,
    207, 207, 207, 207, 207, 207, 207, 207, 204, 204, 204, 204, 201, 201, 201, 188, 188, 176, 141, 66,  40,
    193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 194, 194, 194, 184, 184, 173, 139, 65,  39,
    204, 204, 204, 204, 204, 204, 204, 204, 201, 201, 201, 201, 198, 198, 198, 187, 187, 175, 140, 66,  40,
};

/* Mean band energies (log2), and the coarse energy predictor. */
static const float k_e_means[25] = {6.437500f, 6.250000f, 5.750000f, 5.312500f, 5.062500f, 4.812500f, 4.500000f,
                                    4.375000f, 4.875000f, 4.687500f, 4.562500f, 4.437500f, 4.875000f, 4.625000f,
                                    4.312500f, 4.500000f, 4.375000f, 4.625000f, 4.750000f, 4.437500f, 3.750000f,
                                    3.750000f, 3.750000f, 3.750000f, 3.750000f};
static const float k_pred_coef[4] = {29440 / 32768.f, 26112 / 32768.f, 21248 / 32768.f, 16384 / 32768.f};
static const float k_beta_coef[4] = {30147 / 32768.f, 22282 / 32768.f, 12124 / 32768.f, 6554 / 32768.f};
static const float k_beta_intra = 4915 / 32768.f;

/* Laplace parameters of the coarse energy: [LM][intra][band] as (P(0), decay) in Q8. */
static const unsigned char k_e_prob[4][2][42] = {
    {{72, 127, 65,  129, 66,  128, 65,  128, 64,  128, 62,  128, 64,  128, 64,  128, 92,  78,  92,  79,  92,
      78, 90,  79,  116, 41,  115, 40,  114, 40,  132, 26,  132, 26,  145, 17,  161, 12,  176, 10,  177, 11},
     {24, 179, 48,  138, 54,  135, 54,  132, 53,  134, 56,  133, 55,  132, 55,  132, 61,  114, 70,  96,  74,
      88, 75,  88,  87,  74,  89,  66,  91,  67,  100, 59,  108, 50,  120, 40,  122, 37,  97,  43,  78,  50}},
    {{83, 78,  84,  81,  88,  75,  86,  74,  87,  71,  90,  73,  93,  74,  93,  74,  109, 40,  114, 36,  117,
      34, 117, 34,  143, 17,  145, 18,  146, 19,  162, 12,  165, 10,  178, 7,   189, 6,   190, 8,   177, 9},
     {23, 178, 54,  115, 63,  102, 66,  98,  69,  99,  74,  89,  71,  91,  73,  91,  78,  89,  86,  80,  92,
      66, 93,  64,  102, 59,  103, 60,  104, 60,  117, 52,  123, 44,  138, 35,  133, 31,  97,  38,  77,  45}},
    {{61, 90,  93,  60,  105, 42,  107, 41,  110, 45,  116, 38,  113, 38,  112, 38,  124, 26,  132, 27,  136,
      19, 140, 20,  155, 14,  159, 16,  158, 18,  170, 13,  177, 10,  187, 8,   192, 6,   175, 9,   159, 10},
     {21, 178, 59,  110, 71,  86,  75,  85,  84,  83,  91,  66,  88,  73,  87,  72,  92,  75,  98,  72,  105,
      58, 107, 54,  115, 52,  114, 55,  112, 56,  129, 51,  132, 40,  150, 33,  140, 29,  98,  35,  77,  42}},
    {{42, 121, 96,  66,  108, 43,  111, 40,  117, 44,  123, 32,  120, 36,  119, 33,  127, 33,  134, 34,  139,
      21, 147, 23,  152, 20,  158, 25,  154, 26,  166, 21,  173, 16,  184, 13,  184, 10,  150, 13,  139, 15},
     {22, 178, 63,  114, 74,  82,  84,  83,  92,  82,  103, 62,  96,  72,  96,  67,  101, 73,  107, 72,  113,
      55, 118, 52,  125, 52,  118, 52,  117, 55,  135, 49,  137, 39,  157, 32,  145, 29,  97,  33,  77,  40}},
};

static const unsigned char k_small_energy_icdf[3] = {2, 1, 0};
static const unsigned char k_log2_frac[24] = {0,  8,  13, 16, 19, 21, 23, 24, 26, 27, 28, 29,
                                              30, 31, 32, 32, 33, 34, 34, 35, 36, 36, 37, 37};
static const unsigned char k_trim_icdf[11] = {126, 124, 119, 109, 87, 41, 19, 9, 4, 2, 0};
static const unsigned char k_spread_icdf[4] = {25, 23, 2, 0};
static const unsigned char k_tapset_icdf[3] = {2, 1, 0};
static const signed char k_tf_select[4][8] = {
    {0, -1, 0, -1, 0, -1, 0, -1},
    {0, -1, 0, -2, 1, 0, 1, -1},
    {0, -2, 0, -3, 2, 0, 1, -1},
    {0, -2, 0, -3, 3, 0, 1, -1},
};
static const int k_ordery[30] = {1, 0, 3, 0, 2, 1, 7, 0, 4, 3, 6, 1, 5, 2, 15, 0, 8, 7, 12, 3, 11, 4, 14, 1, 9, 6, 13, 2, 10, 5};
static const float k_comb_gains[3][3] = {
    {0.3066406250f, 0.2170410156f, 0.1296386719f},
    {0.4638671875f, 0.2680664062f, 0.f},
    {0.7998046875f, 0.1000976562f, 0.f},
};

/* ---- Tables computed once: the window, FFT roots and MDCT rotations ---- */

typedef struct {
    float r, i;
} cpx_t;

static float g_window[CELT_OVERLAP];
static cpx_t g_roots[480];            /* e^(2 pi i k / 480) */
static cpx_t g_rot[MAX_LM + 1][480];  /* e^(i 2 pi (k + 1/8) / N) per frame size */
static volatile int g_tables_ready;

static void init_tables(void)
{
    if (g_tables_ready)
        return;
    for (int i = 0; i < CELT_OVERLAP; i++) {
        double s = om_sin(0.5 * OM_PI * (i + 0.5) / CELT_OVERLAP);
        g_window[i] = (float)om_sin(0.5 * OM_PI * s * s);
    }
    for (int k = 0; k < 480; k++) {
        g_roots[k].r = (float)om_cos(2 * OM_PI * k / 480);
        g_roots[k].i = (float)om_sin(2 * OM_PI * k / 480);
    }
    for (int lm = 0; lm <= MAX_LM; lm++) {
        int n = 2 * (SHORT_MDCT << lm);
        for (int k = 0; k < n / 4; k++) {
            g_rot[lm][k].r = (float)om_cos(2 * OM_PI * (k + 0.125) / n);
            g_rot[lm][k].i = (float)om_sin(2 * OM_PI * (k + 0.125) / n);
        }
    }
    g_tables_ready = 1;
}

/* ---- Inverse MDCT through an N/4-point complex FFT ---- */

/* Unscaled inverse DFT of n (60 to 480) points, in place, by recursive decimation in time. */
static void ifft_rec(cpx_t *out, const cpx_t *in, int n, int stride)
{
    int p, m;

    if (n == 1) {
        out[0] = in[0];
        return;
    }
    p = n % 4 == 0 ? 4 : n % 2 == 0 ? 2 : n % 3 == 0 ? 3 : 5;
    m = n / p;
    for (int r = 0; r < p; r++)
        ifft_rec(out + r * m, in + r * stride, m, stride * p);
    for (int k = 0; k < m; k++) {
        cpx_t t[5], u[5];
        for (int r = 0; r < p; r++) {
            cpx_t a = out[r * m + k], w = g_roots[(r * k * (480 / n)) % 480];
            t[r].r = a.r * w.r - a.i * w.i;
            t[r].i = a.r * w.i + a.i * w.r;
        }
        for (int q = 0; q < p; q++) {
            u[q].r = u[q].i = 0;
            for (int r = 0; r < p; r++) {
                cpx_t w = g_roots[(r * q * (480 / p)) % 480];
                u[q].r += t[r].r * w.r - t[r].i * w.i;
                u[q].i += t[r].r * w.i + t[r].i * w.r;
            }
        }
        for (int q = 0; q < p; q++)
            out[k + q * m] = u[q];
    }
}

/*
 * One inverse MDCT of the coefficients in[0], in[stride], ... (N/2 of them)
 * into out[-(N/2 - overlap)/2 ...], windowed at both ends; the first
 * overlap samples are added to what out already holds.
 */
static void imdct(celt_scratch_t *tmp, const float *in, float *out, int lm, int stride)
{
    int n = 2 * (SHORT_MDCT << lm), n2 = n / 2, n4 = n / 4;
    const cpx_t *rot = g_rot[lm];
    cpx_t *z = (cpx_t *)tmp->z, *f = (cpx_t *)tmp->f;
    float *f2 = tmp->f2;

    /* Pre-rotation: z_i = -(x2 + i x1) e^(i 2 pi (i + 1/8) / N) */
    for (int i = 0; i < n4; i++) {
        float x1 = in[2 * i * stride], x2 = in[(n2 - 1 - 2 * i) * stride];
        z[i].r = -(x2 * rot[i].r - x1 * rot[i].i);
        z[i].i = -(x2 * rot[i].i + x1 * rot[i].r);
    }
    ifft_rec(f, z, n4, 1);
    /* Post-rotation, then the de-shuffle of the window's middle. */
    for (int i = 0; i < n4; i++) {
        cpx_t g;
        g.r = f[i].r * rot[i].r - f[i].i * rot[i].i;
        g.i = f[i].r * rot[i].i + f[i].i * rot[i].r;
        f[i] = g;
    }
    for (int i = 0; i < n4; i++) {
        f2[2 * i] = -f[i].r;
        f2[2 * i + 1] = f[n4 - 1 - i].i;
    }
    out -= (n2 - CELT_OVERLAP) >> 1;
    /* Mirror on both sides for the time-domain aliasing cancellation. */
    {
        const float *fp1 = f2 + n4 - 1;
        float *xp1 = out + n2 - 1, *yp1 = out + n4 - CELT_OVERLAP / 2;
        const float *wp1 = g_window, *wp2 = g_window + CELT_OVERLAP - 1;
        int i;
        for (i = 0; i < n4 - CELT_OVERLAP / 2; i++)
            *xp1-- = *fp1--;
        for (; i < n4; i++) {
            float x1 = *fp1--;
            *yp1++ += -*wp1 * x1;
            *xp1-- += *wp2 * x1;
            wp1++;
            wp2--;
        }
    }
    {
        const float *fp2 = f2 + n4;
        float *xp2 = out + n2, *yp2 = out + n - 1 - (n4 - CELT_OVERLAP / 2);
        const float *wp1 = g_window, *wp2 = g_window + CELT_OVERLAP - 1;
        int i;
        for (i = 0; i < n4 - CELT_OVERLAP / 2; i++)
            *xp2++ = *fp2++;
        for (; i < n4; i++) {
            float x2 = *fp2++;
            *yp2-- = *wp1 * x2;
            *xp2++ = *wp2 * x2;
            wp1++;
            wp2--;
        }
    }
}

/* ---- Integer helpers that decide the bitstream's parsing (bit-exact) ---- */

static int imin(int a, int b)
{
    return a < b ? a : b;
}

static int imax(int a, int b)
{
    return a > b ? a : b;
}

static int frac_mul16(int a, int b)
{
    return (16384 + (int)(short)a * (short)b) >> 15;
}

static int bitexact_cos(int x)
{
    int x2 = (4096 + x * x) >> 13;

    x2 = (32767 - x2) + frac_mul16(x2, (-7651 + frac_mul16(x2, (8277 + frac_mul16(-626, x2)))));
    return 1 + x2;
}

static int bitexact_log2tan(int isin, int icos)
{
    int lc = rc_ilog((unsigned)icos), ls = rc_ilog((unsigned)isin);

    icos <<= 15 - lc;
    isin <<= 15 - ls;
    return (ls - lc) * (1 << 11) + frac_mul16(isin, frac_mul16(isin, -2597) + 7932) -
           frac_mul16(icos, frac_mul16(icos, -2597) + 7932);
}

static unsigned isqrt32(unsigned v)
{
    unsigned g = 0;
    int bshift = (rc_ilog(v) - 1) >> 1;
    unsigned b = 1u << bshift;

    do {
        unsigned t = ((g << 1) + b) << bshift;
        if (t <= v) {
            g += b;
            v -= t;
        }
        b >>= 1;
        bshift--;
    } while (bshift >= 0);
    return g;
}

static unsigned lcg_rand(unsigned seed)
{
    return 1664525u * seed + 1013904223u;
}

/* ---- Energy ---- */

static int laplace_decode(opus_rc_t *rc, unsigned fs, int decay)
{
    int val = 0;
    unsigned fl = 0, fm = rc_decode_bin(rc, 15);

    if (fm >= fs) {
        val++;
        fl = fs;
        fs = ((32768 - LAPLACE_MINP * (2 * LAPLACE_NMIN) - fs) * (unsigned)(16384 - decay) >> 15) + LAPLACE_MINP;
        while (fs > LAPLACE_MINP && fm >= fl + 2 * fs) {
            fs *= 2;
            fl += fs;
            fs = ((fs - 2 * LAPLACE_MINP) * (unsigned)decay) >> 15;
            fs += LAPLACE_MINP;
            val++;
        }
        if (fs <= LAPLACE_MINP) {
            int di = (int)((fm - fl) >> 1);
            val += di;
            fl += 2 * (unsigned)di * LAPLACE_MINP;
        }
        if (fm < fl + fs)
            val = -val;
        else
            fl += fs;
    }
    rc_update(rc, fl, fl + fs < 32768 ? fl + fs : 32768, 32768);
    return val;
}

static void unquant_coarse(celt_decoder_t *st, int intra, opus_rc_t *rc, int C, int lm)
{
    const unsigned char *prob = k_e_prob[lm][intra];
    float prev[2] = {0, 0}, coef = intra ? 0 : k_pred_coef[lm], beta = intra ? k_beta_intra : k_beta_coef[lm];
    int budget = (int)rc->storage * 8;

    for (int i = st->start; i < st->end; i++) {
        for (int c = 0; c < C; c++) {
            int qi, tell = rc_tell(rc);
            float q, tmp, *old = &st->old_band_e[i + c * CELT_BANDS];
            if (budget - tell >= 15) {
                int pi = 2 * imin(i, 20);
                qi = laplace_decode(rc, (unsigned)prob[pi] << 7, prob[pi + 1] << 6);
            } else if (budget - tell >= 2) {
                qi = rc_icdf(rc, k_small_energy_icdf, 2);
                qi = (qi >> 1) ^ -(qi & 1);
            } else if (budget - tell >= 1) {
                qi = -rc_bit_logp(rc, 1);
            } else {
                qi = -1;
            }
            q = (float)qi;
            if (*old < -9.f)
                *old = -9.f;
            tmp = coef * *old + prev[c] + q;
            *old = tmp;
            prev[c] = prev[c] + q - beta * q;
        }
    }
}

/* ---- Bit allocation ---- */

static const unsigned char *pulse_cache(int band, int lm)
{
    return k_cache_bits + k_cache_index[(lm + 1) * CELT_BANDS + band];
}

static int get_pulses(int i)
{
    return i < 8 ? i : (8 + (i & 7)) << ((i >> 3) - 1);
}

static int bits2pulses(int band, int lm, int bits)
{
    const unsigned char *cache = pulse_cache(band, lm);
    int lo = 0, hi = cache[0];

    bits--;
    for (int i = 0; i < 6; i++) {
        int mid = (lo + hi + 1) >> 1;
        if (cache[mid] >= bits)
            hi = mid;
        else
            lo = mid;
    }
    return bits - (lo == 0 ? -1 : cache[lo]) <= cache[hi] - bits ? lo : hi;
}

static int pulses2bits(int band, int lm, int pulses)
{
    return pulses == 0 ? 0 : pulse_cache(band, lm)[pulses] + 1;
}

static int interp_bits2pulses(int start, int end, int skip_start, const int *bits1, const int *bits2,
                              const int *thresh, const int *cap, int total, int *out_balance, int skip_rsv,
                              int *intensity, int intensity_rsv, int *dual_stereo, int dual_stereo_rsv, int *bits,
                              int *ebits, int *fine_priority, int C, int lm, opus_rc_t *rc, opus_rce_t *enc, int prev)
{
    int psum, lo = 0, hi = 1 << ALLOC_STEPS, done, coded, alloc_floor = C << BITRES, stereo = C > 1;
    int log_m = lm << BITRES, left, percoeff, balance, j;

    for (int i = 0; i < ALLOC_STEPS; i++) {
        int mid = (lo + hi) >> 1;
        psum = 0;
        done = 0;
        for (j = end; j-- > start;) {
            int tmp = bits1[j] + (mid * bits2[j] >> ALLOC_STEPS);
            if (tmp >= thresh[j] || done) {
                done = 1;
                psum += imin(tmp, cap[j]);
            } else if (tmp >= alloc_floor) {
                psum += alloc_floor;
            }
        }
        if (psum > total)
            hi = mid;
        else
            lo = mid;
    }
    psum = 0;
    done = 0;
    for (j = end; j-- > start;) {
        int tmp = bits1[j] + (lo * bits2[j] >> ALLOC_STEPS);
        if (tmp < thresh[j] && !done)
            tmp = tmp >= alloc_floor ? alloc_floor : 0;
        else
            done = 1;
        tmp = imin(tmp, cap[j]);
        bits[j] = tmp;
        psum += tmp;
    }

    /* Which bands to skip, working down from the top. */
    for (coded = end;; coded--) {
        int band_width, band_bits, rem;
        j = coded - 1;
        if (j <= skip_start) {
            total += skip_rsv;
            break;
        }
        left = total - psum;
        percoeff = left / (k_ebands[coded] - k_ebands[start]);
        left -= (k_ebands[coded] - k_ebands[start]) * percoeff;
        rem = imax(left - (k_ebands[j] - k_ebands[start]), 0);
        band_width = k_ebands[coded] - k_ebands[j];
        band_bits = bits[j] + percoeff * band_width + rem;
        if (band_bits >= imax(thresh[j], alloc_floor + (1 << BITRES))) {
            if (enc) {
                /* The encoder's choice, with some hysteresis against bands flickering in and out. */
                if (band_bits > ((j < prev ? 7 : 9) * band_width << lm << BITRES) >> 4) {
                    rce_bit_logp(enc, 1, 1);
                    break;
                }
                rce_bit_logp(enc, 0, 1);
            } else if (rc_bit_logp(rc, 1)) {
                break;
            }
            psum += 1 << BITRES;
            band_bits -= 1 << BITRES;
        }
        psum -= bits[j] + intensity_rsv;
        if (intensity_rsv > 0)
            intensity_rsv = k_log2_frac[j - start];
        psum += intensity_rsv;
        if (band_bits >= alloc_floor) {
            psum += alloc_floor;
            bits[j] = alloc_floor;
        } else {
            bits[j] = 0;
        }
    }

    if (intensity_rsv > 0) {
        if (enc) {
            *intensity = imin(*intensity, coded);
            rce_uint(enc, (unsigned)(*intensity - start), (unsigned)(coded + 1 - start));
        } else {
            *intensity = start + (int)rc_uint(rc, (unsigned)(coded + 1 - start));
        }
    } else {
        *intensity = 0;
    }
    if (*intensity <= start) {
        total += dual_stereo_rsv;
        dual_stereo_rsv = 0;
    }
    if (dual_stereo_rsv > 0 && enc)
        rce_bit_logp(enc, *dual_stereo, 1);
    else
        *dual_stereo = dual_stereo_rsv > 0 ? rc_bit_logp(rc, 1) : 0;

    /* The remaining bits, spread over the coded bands. */
    left = total - psum;
    percoeff = left / (k_ebands[coded] - k_ebands[start]);
    left -= (k_ebands[coded] - k_ebands[start]) * percoeff;
    for (j = start; j < coded; j++)
        bits[j] += percoeff * (k_ebands[j + 1] - k_ebands[j]);
    for (j = start; j < coded; j++) {
        int tmp = imin(left, k_ebands[j + 1] - k_ebands[j]);
        bits[j] += tmp;
        left -= tmp;
    }

    balance = 0;
    for (j = start; j < coded; j++) {
        int n0 = k_ebands[j + 1] - k_ebands[j], n = n0 << lm, excess;
        bits[j] += balance;
        if (n > 1) {
            int den, nclogn, offset;
            excess = imax(bits[j] - cap[j], 0);
            bits[j] -= excess;
            den = C * n + (C == 2 && n > 2 && !*dual_stereo && j < *intensity ? 1 : 0);
            nclogn = den * (k_log_n[j] + log_m);
            offset = (nclogn >> 1) - den * FINE_OFFSET;
            if (n == 2)
                offset += den << BITRES >> 2;
            if (bits[j] + offset < den * 2 << BITRES)
                offset += nclogn >> 2;
            else if (bits[j] + offset < den * 3 << BITRES)
                offset += nclogn >> 3;
            ebits[j] = imax(0, (bits[j] + offset + (den << (BITRES - 1))) / (den << BITRES));
            if (C * ebits[j] > (bits[j] >> BITRES))
                ebits[j] = bits[j] >> stereo >> BITRES;
            ebits[j] = imin(ebits[j], MAX_FINE_BITS);
            fine_priority[j] = ebits[j] * (den << BITRES) >= bits[j] + offset;
            bits[j] -= C * ebits[j] << BITRES;
        } else {
            excess = imax(0, bits[j] - (C << BITRES));
            bits[j] -= excess;
            ebits[j] = 0;
            fine_priority[j] = 1;
        }
        if (excess > 0) {
            int extra_fine = imin(excess >> (stereo + BITRES), MAX_FINE_BITS - ebits[j]);
            int extra_bits = extra_fine * C << BITRES;
            ebits[j] += extra_fine;
            fine_priority[j] = extra_bits >= excess - balance;
            excess -= extra_bits;
        }
        balance = excess;
    }
    *out_balance = balance;
    for (; j < end; j++) {
        ebits[j] = bits[j] >> stereo >> BITRES;
        bits[j] = 0;
        fine_priority[j] = ebits[j] < 1;
    }
    return coded;
}

static int compute_allocation(int start, int end, const int *offsets, const int *cap, int alloc_trim,
                              int *intensity, int *dual_stereo, int total, int *balance, int *pulses, int *ebits,
                              int *fine_priority, int C, int lm, opus_rc_t *rc, opus_rce_t *enc, int prev)
{
    int lo = 1, hi = 10, skip_start = start, skip_rsv, intensity_rsv = 0, dual_stereo_rsv = 0;
    int bits1[CELT_BANDS], bits2[CELT_BANDS], thresh[CELT_BANDS], trim_offset[CELT_BANDS];

    total = imax(total, 0);
    skip_rsv = total >= 1 << BITRES ? 1 << BITRES : 0;
    total -= skip_rsv;
    if (C == 2) {
        intensity_rsv = k_log2_frac[end - start];
        if (intensity_rsv > total) {
            intensity_rsv = 0;
        } else {
            total -= intensity_rsv;
            dual_stereo_rsv = total >= 1 << BITRES ? 1 << BITRES : 0;
            total -= dual_stereo_rsv;
        }
    }
    for (int j = start; j < end; j++) {
        int w = k_ebands[j + 1] - k_ebands[j];
        thresh[j] = imax(C << BITRES, (3 * w << lm << BITRES) >> 4);
        trim_offset[j] = C * w * (alloc_trim - 5 - lm) * (end - j - 1) * (1 << (lm + BITRES)) >> 6;
        if (w << lm == 1)
            trim_offset[j] -= C << BITRES;
    }
    do {
        int done = 0, psum = 0, mid = (lo + hi) >> 1;
        for (int j = end; j-- > start;) {
            int n = k_ebands[j + 1] - k_ebands[j], bitsj = C * n * k_alloc[mid * CELT_BANDS + j] << lm >> 2;
            if (bitsj > 0)
                bitsj = imax(0, bitsj + trim_offset[j]);
            bitsj += offsets[j];
            if (bitsj >= thresh[j] || done) {
                done = 1;
                psum += imin(bitsj, cap[j]);
            } else if (bitsj >= C << BITRES) {
                psum += C << BITRES;
            }
        }
        if (psum > total)
            hi = mid - 1;
        else
            lo = mid + 1;
    } while (lo <= hi);
    hi = lo--;
    for (int j = start; j < end; j++) {
        int n = k_ebands[j + 1] - k_ebands[j];
        int b1 = C * n * k_alloc[lo * CELT_BANDS + j] << lm >> 2;
        int b2 = hi >= 11 ? cap[j] : C * n * k_alloc[hi * CELT_BANDS + j] << lm >> 2;
        if (b1 > 0)
            b1 = imax(0, b1 + trim_offset[j]);
        if (b2 > 0)
            b2 = imax(0, b2 + trim_offset[j]);
        if (lo > 0)
            b1 += offsets[j];
        b2 += offsets[j];
        if (offsets[j] > 0)
            skip_start = j;
        bits1[j] = b1;
        bits2[j] = imax(0, b2 - b1);
    }
    return interp_bits2pulses(start, end, skip_start, bits1, bits2, thresh, cap, total, balance, skip_rsv, intensity,
                              intensity_rsv, dual_stereo, dual_stereo_rsv, pulses, ebits, fine_priority, C, lm, rc,
                              enc, prev);
}

/* ---- Pulse vectors (PVQ) ---- */

/* Row U(n, 0..k+1) of the PVQ codebook size recurrence, and V(n, k). */
static unsigned ncwrs_urow(int n, int k, unsigned *u)
{
    int len = k + 2;

    u[0] = 0;
    u[1] = 1;
    for (int j = 2; j < len; j++)
        u[j] = (unsigned)(2 * j - 1);
    for (int j = 2; j < n; j++) {
        /* The next row: u[i][j] = u[i-1][j] + u[i][j-1] + u[i-1][j-1] */
        unsigned u0 = 1, u1;
        int i = 1;
        unsigned *ui = u + 1;
        do {
            u1 = ui[i] + ui[i - 1] + u0;
            ui[i - 1] = u0;
            u0 = u1;
        } while (++i < k + 1);
        ui[i - 1] = u0;
    }
    return u[k] + u[k + 1];
}

static void uprev(unsigned *u, int n, unsigned u0)
{
    unsigned u1;
    int j = 1;

    do {
        u1 = u[j] - u[j - 1] - u0;
        u[j - 1] = u0;
        u0 = u1;
    } while (++j < n);
    u[j - 1] = u0;
}

static void decode_pulses(int *y, int n, int k, opus_rc_t *rc)
{
    unsigned u[132];
    unsigned i = rc_uint(rc, ncwrs_urow(n, k, u));

    for (int j = 0; j < n; j++) {
        unsigned p = u[k + 1];
        int s = -(i >= p), yj;
        i -= p & (unsigned)s;
        yj = k;
        p = u[k];
        while (p > i)
            p = u[--k];
        i -= p;
        yj -= k;
        y[j] = (yj + s) ^ s;
        uprev(u, k + 2, 0);
    }
}

static void exp_rotation1(float *x, int len, int stride, float c, float s)
{
    float *p = x;

    for (int i = 0; i < len - stride; i++) {
        float x1 = p[0], x2 = p[stride];
        p[stride] = c * x2 + s * x1;
        *p++ = c * x1 - s * x2;
    }
    p = &x[len - 2 * stride - 1];
    for (int i = len - 2 * stride - 1; i >= 0; i--) {
        float x1 = p[0], x2 = p[stride];
        p[stride] = c * x2 + s * x1;
        *p-- = c * x1 - s * x2;
    }
}

static void exp_rotation(float *x, int len, int dir, int stride, int k, int spread)
{
    static const int factors[3] = {15, 10, 5};
    int stride2 = 0;
    float gain, theta, c, s;

    if (2 * k >= len || spread == SPREAD_NONE)
        return;
    gain = (float)len / (float)(len + factors[spread - 1] * k);
    theta = 0.5f * gain * gain;
    c = (float)om_cos(0.5 * OM_PI * theta);
    s = (float)om_cos(0.5 * OM_PI * (1.f - theta));
    if (len >= 8 * stride) {
        stride2 = 1;
        while ((stride2 * stride2 + stride2) * stride + (stride >> 2) < len)
            stride2++;
    }
    len /= stride;
    for (int i = 0; i < stride; i++) {
        if (dir < 0) {
            if (stride2)
                exp_rotation1(x + i * len, len, stride2, s, c);
            exp_rotation1(x + i * len, len, 1, c, s);
        } else {
            exp_rotation1(x + i * len, len, 1, c, -s);
            if (stride2)
                exp_rotation1(x + i * len, len, stride2, s, -c);
        }
    }
}

static void renormalise(float *x, int n, float gain)
{
    float e = EPSILON, g;

    for (int i = 0; i < n; i++)
        e += x[i] * x[i];
    g = gain / om_sqrt(e);
    for (int i = 0; i < n; i++)
        x[i] *= g;
}

static unsigned alg_unquant(float *x, int n, int k, int spread, int b, opus_rc_t *rc, float gain)
{
    int y[176];
    float ryy = 0, g;
    unsigned mask = 0;

    decode_pulses(y, n, k, rc);
    for (int i = 0; i < n; i++)
        ryy += (float)y[i] * (float)y[i];
    g = gain / om_sqrt(ryy);
    for (int i = 0; i < n; i++)
        x[i] = g * (float)y[i];
    exp_rotation(x, n, -1, b, k, spread);
    if (b <= 1)
        return 1;
    for (int i = 0, n0 = n / b; i < b; i++)
        for (int j = 0; j < n0; j++)
            mask |= (unsigned)(y[i * n0 + j] != 0) << i;
    return mask;
}

/* ---- Band decoding ---- */

static void haar1(float *x, int n0, int stride)
{
    n0 >>= 1;
    for (int i = 0; i < stride; i++)
        for (int j = 0; j < n0; j++) {
            float t1 = 0.70710678f * x[stride * 2 * j + i], t2 = 0.70710678f * x[stride * (2 * j + 1) + i];
            x[stride * 2 * j + i] = t1 + t2;
            x[stride * (2 * j + 1) + i] = t1 - t2;
        }
}

static void deinterleave_hadamard(float *x, int n0, int stride, int hadamard)
{
    float tmp[176];
    int n = n0 * stride;

    for (int i = 0; i < stride; i++)
        for (int j = 0; j < n0; j++)
            tmp[(hadamard ? k_ordery[stride - 2 + i] : i) * n0 + j] = x[j * stride + i];
    memcpy(x, tmp, sizeof *x * (size_t)n);
}

static void interleave_hadamard(float *x, int n0, int stride, int hadamard)
{
    float tmp[176];
    int n = n0 * stride;

    for (int i = 0; i < stride; i++)
        for (int j = 0; j < n0; j++)
            tmp[j * stride + i] = x[(hadamard ? k_ordery[stride - 2 + i] : i) * n0 + j];
    memcpy(x, tmp, sizeof *x * (size_t)n);
}

static int compute_qn(int n, int b, int offset, int pulse_cap, int stereo)
{
    static const short exp2_table8[8] = {16384, 17866, 19483, 21247, 23170, 25267, 27554, 30048};
    int qn, qb, n2 = 2 * n - 1;

    if (stereo && n == 2)
        n2--;
    qb = imin(b - pulse_cap - (4 << BITRES), (b + n2 * offset) / n2);
    qb = imin(8 << BITRES, qb);
    if (qb < (1 << BITRES >> 1))
        return 1;
    qn = exp2_table8[qb & 7] >> (14 - (qb >> BITRES));
    return (qn + 1) >> 1 << 1;
}

static void stereo_merge(float *x, float *y, float mid, int n)
{
    float xp = 0, side = 0, el, er, lgain, rgain;

    for (int j = 0; j < n; j++) {
        xp += x[j] * y[j];
        side += y[j] * y[j];
    }
    xp *= mid;
    el = mid * mid + side - 2 * xp;
    er = mid * mid + side + 2 * xp;
    if (er < 6e-4f || el < 6e-4f) {
        memcpy(y, x, sizeof *x * (size_t)n);
        return;
    }
    lgain = 1.f / om_sqrt(el);
    rgain = 1.f / om_sqrt(er);
    for (int j = 0; j < n; j++) {
        float l = mid * x[j], r = y[j];
        x[j] = lgain * (l - r);
        y[j] = rgain * (l + r);
    }
}

typedef struct {
    opus_rc_t *rc;
    int spread, intensity, lm0;
    int remaining_bits;
    unsigned seed;
} band_ctx_t;

/* Decodes one band (or a half of one, recursively), mono or stereo. Returns its collapse mask. */
static unsigned quant_band(band_ctx_t *ctx, int i, float *x, float *y, int n, int b, int big_b, int tf_change,
                           float *lowband, int lm, float *lowband_out, int level, float gain, float *lowband_scratch,
                           int fill)
{
    opus_rc_t *rc = ctx->rc;
    int n0 = n, n_b = n / big_b, n_b0, b0 = big_b, time_divide = 0, recombine = 0, inv = 0;
    int stereo = y != NULL, split = stereo, long_blocks = b0 == 1;
    float mid = 0, side = 0;
    unsigned cm = 0;

    if (n == 1) {
        float *v = x;
        for (int c = 0; c < 1 + stereo; c++) {
            int sign = 0;
            if (ctx->remaining_bits >= 1 << BITRES) {
                sign = (int)rc_bits(rc, 1);
                ctx->remaining_bits -= 1 << BITRES;
                b -= 1 << BITRES;
            }
            v[0] = sign ? -1.f : 1.f;
            v = y;
        }
        if (lowband_out)
            lowband_out[0] = x[0];
        return 1;
    }

    if (!stereo && level == 0) {
        if (tf_change > 0)
            recombine = tf_change;
        if (lowband && (recombine || ((n_b & 1) == 0 && tf_change < 0) || b0 > 1)) {
            memcpy(lowband_scratch, lowband, sizeof *lowband * (size_t)n);
            lowband = lowband_scratch;
        }
        for (int k = 0; k < recombine; k++) {
            static const unsigned char bit_interleave[16] = {0, 1, 1, 1, 2, 3, 3, 3, 2, 3, 3, 3, 2, 3, 3, 3};
            if (lowband)
                haar1(lowband, n >> k, 1 << k);
            fill = bit_interleave[fill & 0xF] | bit_interleave[fill >> 4] << 2;
        }
        big_b >>= recombine;
        n_b <<= recombine;
        while ((n_b & 1) == 0 && tf_change < 0) {
            if (lowband)
                haar1(lowband, n_b, big_b);
            fill |= fill << big_b;
            big_b <<= 1;
            n_b >>= 1;
            time_divide++;
            tf_change++;
        }
        b0 = big_b;
        n_b0 = n_b;
        if (b0 > 1 && lowband)
            deinterleave_hadamard(lowband, n_b >> recombine, b0 << recombine, long_blocks);
    } else {
        n_b0 = n_b;
    }

    /* Split the band in two when it wants 1.5 bits more than the codebook can take. */
    {
        const unsigned char *cache = pulse_cache(i, lm);
        if (!stereo && lm != -1 && b > cache[cache[0]] + 12 && n > 2) {
            n >>= 1;
            y = x + n;
            split = 1;
            lm -= 1;
            if (big_b == 1)
                fill = (fill & 1) | (fill << 1);
            big_b = (big_b + 1) >> 1;
        }
    }

    if (split) {
        int qn, itheta = 0, mbits, sbits, delta, qalloc, pulse_cap, offset, orig_fill;
        int tell, imid, iside;

        pulse_cap = k_log_n[i] + lm * (1 << BITRES);
        offset = (pulse_cap >> 1) - (stereo && n == 2 ? QTHETA_OFFSET_TWOPHASE : QTHETA_OFFSET);
        qn = compute_qn(n, b, offset, pulse_cap, stereo);
        if (stereo && i >= ctx->intensity)
            qn = 1;
        tell = (int)rc_tell_frac(rc);
        if (qn != 1) {
            if (stereo && n > 2) {
                /* A step: probability 3 up to the middle, 1 after. */
                int p0 = 3, x0 = qn / 2, ft = p0 * (x0 + 1) + x0, fs = (int)rc_decode(rc, (unsigned)ft), v;
                v = fs < (x0 + 1) * p0 ? fs / p0 : x0 + 1 + (fs - (x0 + 1) * p0);
                rc_update(rc, (unsigned)(v <= x0 ? p0 * v : (v - 1 - x0) + (x0 + 1) * p0),
                          (unsigned)(v <= x0 ? p0 * (v + 1) : (v - x0) + (x0 + 1) * p0), (unsigned)ft);
                itheta = v;
            } else if (b0 > 1 || stereo) {
                itheta = (int)rc_uint(rc, (unsigned)(qn + 1));
            } else {
                /* Triangular */
                int fs, fl, ft = ((qn >> 1) + 1) * ((qn >> 1) + 1);
                unsigned fm = rc_decode(rc, (unsigned)ft);
                if (fm < (unsigned)((qn >> 1) * ((qn >> 1) + 1) >> 1)) {
                    itheta = (int)(isqrt32(8 * fm + 1) - 1) >> 1;
                    fs = itheta + 1;
                    fl = itheta * (itheta + 1) >> 1;
                } else {
                    itheta = (2 * (qn + 1) - (int)isqrt32(8 * (unsigned)(ft - (int)fm - 1) + 1)) >> 1;
                    fs = qn + 1 - itheta;
                    fl = ft - ((qn + 1 - itheta) * (qn + 2 - itheta) >> 1);
                }
                rc_update(rc, (unsigned)fl, (unsigned)(fl + fs), (unsigned)ft);
            }
            itheta = itheta * 16384 / qn;
        } else if (stereo) {
            if (b > 2 << BITRES && ctx->remaining_bits > 2 << BITRES)
                inv = rc_bit_logp(rc, 2);
            itheta = 0;
        }
        qalloc = (int)rc_tell_frac(rc) - tell;
        b -= qalloc;

        orig_fill = fill;
        if (itheta == 0) {
            imid = 32767;
            iside = 0;
            fill &= (1 << big_b) - 1;
            delta = -16384;
        } else if (itheta == 16384) {
            imid = 0;
            iside = 32767;
            fill &= ((1 << big_b) - 1) << big_b;
            delta = 16384;
        } else {
            imid = bitexact_cos(itheta);
            iside = bitexact_cos(16384 - itheta);
            delta = frac_mul16((n - 1) << 7, bitexact_log2tan(iside, imid));
        }
        mid = (1.f / 32768) * (float)imid;
        side = (1.f / 32768) * (float)iside;

        if (n == 2 && stereo) {
            /* Mid and side are orthogonal: the side needs only a sign. */
            int c, sign = 0;
            float *x2, *y2, tmp;
            mbits = b;
            sbits = itheta != 0 && itheta != 16384 ? 1 << BITRES : 0;
            mbits -= sbits;
            c = itheta > 8192;
            ctx->remaining_bits -= qalloc + sbits;
            x2 = c ? y : x;
            y2 = c ? x : y;
            if (sbits)
                sign = (int)rc_bits(rc, 1);
            sign = 1 - 2 * sign;
            cm = quant_band(ctx, i, x2, NULL, n, mbits, big_b, tf_change, lowband, lm, lowband_out, level, gain,
                            lowband_scratch, orig_fill);
            y2[0] = (float)-sign * x2[1];
            y2[1] = (float)sign * x2[0];
            x[0] *= mid;
            x[1] *= mid;
            y[0] *= side;
            y[1] *= side;
            tmp = x[0];
            x[0] = tmp - y[0];
            y[0] = tmp + y[0];
            tmp = x[1];
            x[1] = tmp - y[1];
            y[1] = tmp + y[1];
        } else {
            float *next_lowband2 = NULL, *next_lowband_out1 = NULL;
            int next_level = 0, rebalance;
            if (b0 > 1 && !stereo && (itheta & 0x3fff)) {
                if (itheta > 8192)
                    delta -= delta >> (4 - lm);
                else
                    delta = imin(0, delta + (n << BITRES >> (5 - lm)));
            }
            mbits = imax(0, imin(b, (b - delta) / 2));
            sbits = b - mbits;
            ctx->remaining_bits -= qalloc;
            if (lowband && !stereo)
                next_lowband2 = lowband + n;
            if (stereo)
                next_lowband_out1 = lowband_out;
            else
                next_level = level + 1;
            rebalance = ctx->remaining_bits;
            if (mbits >= sbits) {
                cm = quant_band(ctx, i, x, NULL, n, mbits, big_b, tf_change, lowband, lm, next_lowband_out1,
                                next_level, stereo ? 1.f : gain * mid, lowband_scratch, fill);
                rebalance = mbits - (rebalance - ctx->remaining_bits);
                if (rebalance > 3 << BITRES && itheta != 0)
                    sbits += rebalance - (3 << BITRES);
                cm |= quant_band(ctx, i, y, NULL, n, sbits, big_b, tf_change, next_lowband2, lm, NULL, next_level,
                                 gain * side, NULL, fill >> big_b)
                      << ((b0 >> 1) & (stereo - 1));
            } else {
                cm = quant_band(ctx, i, y, NULL, n, sbits, big_b, tf_change, next_lowband2, lm, NULL, next_level,
                                gain * side, NULL, fill >> big_b)
                     << ((b0 >> 1) & (stereo - 1));
                rebalance = sbits - (rebalance - ctx->remaining_bits);
                if (rebalance > 3 << BITRES && itheta != 16384)
                    mbits += rebalance - (3 << BITRES);
                cm |= quant_band(ctx, i, x, NULL, n, mbits, big_b, tf_change, lowband, lm, next_lowband_out1,
                                 next_level, stereo ? 1.f : gain * mid, lowband_scratch, fill);
            }
        }
    } else {
        /* No split: the pulses themselves. */
        int q = bits2pulses(i, lm, b), curr = pulses2bits(i, lm, q);
        ctx->remaining_bits -= curr;
        while (ctx->remaining_bits < 0 && q > 0) {
            ctx->remaining_bits += curr;
            q--;
            curr = pulses2bits(i, lm, q);
            ctx->remaining_bits -= curr;
        }
        if (q != 0) {
            cm = alg_unquant(x, n, get_pulses(q), ctx->spread, big_b, rc, gain);
        } else {
            unsigned cm_mask = (unsigned)(1ul << big_b) - 1;
            fill &= (int)cm_mask;
            if (!fill) {
                memset(x, 0, sizeof *x * (size_t)n);
            } else {
                if (!lowband) {
                    for (int j = 0; j < n; j++) {
                        ctx->seed = lcg_rand(ctx->seed);
                        x[j] = (float)((int)ctx->seed >> 20);
                    }
                    cm = cm_mask;
                } else {
                    for (int j = 0; j < n; j++) {
                        ctx->seed = lcg_rand(ctx->seed);
                        x[j] = lowband[j] + (ctx->seed & 0x8000 ? 1.f / 256 : -1.f / 256);
                    }
                    cm = (unsigned)fill;
                }
                renormalise(x, n, gain);
            }
        }
    }

    if (stereo) {
        if (n != 2)
            stereo_merge(x, y, mid, n);
        if (inv)
            for (int j = 0; j < n; j++)
                y[j] = -y[j];
    } else if (level == 0) {
        if (b0 > 1)
            interleave_hadamard(x, n_b >> recombine, b0 << recombine, long_blocks);
        n_b = n_b0;
        big_b = b0;
        for (int k = 0; k < time_divide; k++) {
            big_b >>= 1;
            n_b <<= 1;
            cm |= cm >> big_b;
            haar1(x, n_b, big_b);
        }
        for (int k = 0; k < recombine; k++) {
            static const unsigned char bit_deinterleave[16] = {0x00, 0x03, 0x0C, 0x0F, 0x30, 0x33, 0x3C, 0x3F,
                                                               0xC0, 0xC3, 0xCC, 0xCF, 0xF0, 0xF3, 0xFC, 0xFF};
            cm = bit_deinterleave[cm];
            haar1(x, n0 >> k, 1 << k);
        }
        big_b <<= recombine;
        if (lowband_out) {
            float s = om_sqrt((float)n0);
            for (int j = 0; j < n0; j++)
                lowband_out[j] = s * x[j];
        }
        cm &= (1u << big_b) - 1;
    }
    return cm;
}

static void quant_all_bands(band_ctx_t *ctx, celt_scratch_t *tmp, int start, int end, float *x_, float *y_,
                            unsigned char *collapse, const int *pulses, int short_blocks, int dual_stereo,
                            const int *tf_res, int total_bits, int balance, int lm, int coded)
{
    float *norm = tmp->norm, *scratch = tmp->lowband;
    float *norm2 = norm + 8 * 100;
    int m = 1 << lm, big_b = short_blocks ? m : 1, lowband_offset = 0, update_lowband = 1, C = y_ ? 2 : 1;
    opus_rc_t *rc = ctx->rc;

    for (int i = start; i < end; i++) {
        int tell = (int)rc_tell_frac(rc), b, n = m * k_ebands[i + 1] - m * k_ebands[i], effective_lowband = -1;
        int tf_change = tf_res[i];
        float *x = x_ + m * k_ebands[i], *y = y_ ? y_ + m * k_ebands[i] : NULL;
        unsigned x_cm, y_cm;

        if (i != start)
            balance -= tell;
        ctx->remaining_bits = total_bits - tell - 1;
        if (i <= coded - 1) {
            int curr_balance = balance / imin(3, coded - i);
            b = imax(0, imin(16383, imin(ctx->remaining_bits + 1, pulses[i] + curr_balance)));
        } else {
            b = 0;
        }
        /* RFC 8251: the first band after start can always fold (hybrid mode). */
        if ((m * k_ebands[i] - n >= m * k_ebands[start] || i == start + 1) && (update_lowband || lowband_offset == 0))
            lowband_offset = i;
        if (i == start + 1) {
            int n1 = m * (k_ebands[start + 1] - k_ebands[start]), n2 = m * (k_ebands[start + 2] - k_ebands[start + 1]);
            int offset = m * k_ebands[start];
            if (n2 > n1) {
                memmove(&norm[offset + n1], &norm[offset + 2 * n1 - n2], sizeof *norm * (size_t)(n2 - n1));
                if (C == 2)
                    memmove(&norm2[offset + n1], &norm2[offset + 2 * n1 - n2], sizeof *norm * (size_t)(n2 - n1));
            }
        }
        if (lowband_offset != 0 && (ctx->spread != SPREAD_AGGRESSIVE || big_b > 1 || tf_change < 0)) {
            int fold_start, fold_end, fold_i;
            effective_lowband = imax(m * k_ebands[start], m * k_ebands[lowband_offset] - n);
            fold_start = lowband_offset;
            while (m * k_ebands[--fold_start] > effective_lowband)
                ;
            fold_end = lowband_offset - 1;
            while (++fold_end < i && m * k_ebands[fold_end] < effective_lowband + n)
                ;
            x_cm = y_cm = 0;
            fold_i = fold_start;
            do {
                x_cm |= collapse[fold_i * C + 0];
                y_cm |= collapse[fold_i * C + C - 1];
            } while (++fold_i < fold_end);
        } else {
            x_cm = y_cm = (1u << big_b) - 1;
        }
        if (dual_stereo && i == ctx->intensity) {
            dual_stereo = 0;
            for (int j = m * k_ebands[start]; j < m * k_ebands[i]; j++)
                norm[j] = 0.5f * (norm[j] + norm2[j]);
        }
        if (dual_stereo) {
            x_cm = quant_band(ctx, i, x, NULL, n, b / 2, big_b, tf_change,
                              effective_lowband != -1 ? norm + effective_lowband : NULL, lm, norm + m * k_ebands[i], 0,
                              1.f, scratch, (int)x_cm);
            y_cm = quant_band(ctx, i, y, NULL, n, b / 2, big_b, tf_change,
                              effective_lowband != -1 ? norm2 + effective_lowband : NULL, lm, norm2 + m * k_ebands[i],
                              0, 1.f, scratch, (int)y_cm);
        } else {
            x_cm = quant_band(ctx, i, x, y, n, b, big_b, tf_change,
                              effective_lowband != -1 ? norm + effective_lowband : NULL, lm, norm + m * k_ebands[i], 0,
                              1.f, scratch, (int)(x_cm | y_cm));
            y_cm = x_cm;
        }
        collapse[i * C + 0] = (unsigned char)x_cm;
        collapse[i * C + C - 1] = (unsigned char)y_cm;
        balance += pulses[i] + tell;
        update_lowband = b > n << BITRES;
    }
}

static void anti_collapse(celt_decoder_t *st, float *x_, const unsigned char *collapse, int lm, int C, int size,
                          const int *pulses, unsigned seed)
{
    for (int i = st->start; i < st->end; i++) {
        int n0 = k_ebands[i + 1] - k_ebands[i], depth = (1 + pulses[i]) / (n0 << lm);
        float thresh = 0.5f * om_exp2(-0.125f * (float)depth), sqrt_1 = 1.f / om_sqrt((float)(n0 << lm));
        for (int c = 0; c < C; c++) {
            float prev1 = st->old_log_e[c * CELT_BANDS + i], prev2 = st->old_log_e2[c * CELT_BANDS + i], ediff, r;
            float *x = x_ + c * size + (k_ebands[i] << lm);
            int renorm = 0;
            if (C == 1) {
                if (st->old_log_e[CELT_BANDS + i] > prev1)
                    prev1 = st->old_log_e[CELT_BANDS + i];
                if (st->old_log_e2[CELT_BANDS + i] > prev2)
                    prev2 = st->old_log_e2[CELT_BANDS + i];
            }
            ediff = st->old_band_e[c * CELT_BANDS + i] - (prev1 < prev2 ? prev1 : prev2);
            if (ediff < 0)
                ediff = 0;
            r = 2.f * om_exp2(-ediff);
            if (lm == 3)
                r *= 1.41421356f;
            if (r > thresh)
                r = thresh;
            r *= sqrt_1;
            for (int k = 0; k < 1 << lm; k++)
                if (!(collapse[i * C + c] & 1 << k)) {
                    for (int j = 0; j < n0; j++) {
                        seed = lcg_rand(seed);
                        x[(j << lm) + k] = seed & 0x8000 ? r : -r;
                    }
                    renorm = 1;
                }
            if (renorm)
                renormalise(x, n0 << lm, 1.f);
        }
    }
}

/* ---- Synthesis ---- */

static void comb_filter(float *y, const float *x, int t0, int t1, int n, float g0, float g1, int tapset0,
                        int tapset1)
{
    float g00 = g0 * k_comb_gains[tapset0][0], g01 = g0 * k_comb_gains[tapset0][1], g02 = g0 * k_comb_gains[tapset0][2];
    float g10 = g1 * k_comb_gains[tapset1][0], g11 = g1 * k_comb_gains[tapset1][1], g12 = g1 * k_comb_gains[tapset1][2];
    int i;

    for (i = 0; i < CELT_OVERLAP && i < n; i++) {
        float f = g_window[i] * g_window[i], nf = 1.f - f;
        y[i] = x[i] + nf * g00 * x[i - t0] + nf * g01 * (x[i - t0 - 1] + x[i - t0 + 1]) +
               nf * g02 * (x[i - t0 - 2] + x[i - t0 + 2]) + f * g10 * x[i - t1] + f * g11 * (x[i - t1 - 1] + x[i - t1 + 1]) +
               f * g12 * (x[i - t1 - 2] + x[i - t1 + 2]);
    }
    for (; i < n; i++)
        y[i] = x[i] + g10 * x[i - t1] + g11 * (x[i - t1 - 1] + x[i - t1 + 1]) + g12 * (x[i - t1 - 2] + x[i - t1 + 2]);
}

void celt_init(celt_decoder_t *st, int channels)
{
    init_tables();
    memset(st, 0, sizeof *st);
    st->channels = st->stream_channels = channels;
    st->start = 0;
    st->end = CELT_BANDS;
    celt_reset(st);
}

void celt_reset(celt_decoder_t *st)
{
    int channels = st->channels, stream = st->stream_channels, start = st->start, end = st->end;

    memset(st, 0, sizeof *st);
    st->channels = channels;
    st->stream_channels = stream;
    st->start = start;
    st->end = end;
    for (int i = 0; i < 2 * CELT_BANDS; i++)
        st->old_log_e[i] = st->old_log_e2[i] = -28.f;
}

/* Output of a lost frame: for now the memory decays to silence (a fuller concealment belongs to the Opus layer). */
static void conceal(celt_decoder_t *st, float *pcm, int n)
{
    int cc = st->channels;

    for (int c = 0; c < cc; c++) {
        float *mem = st->mem[c], m = st->preemph_mem[c];
        memmove(mem, mem + n, sizeof *mem * (size_t)(CELT_BUFFER - n + CELT_OVERLAP));
        for (int j = 0; j < n; j++) {
            float s = st->mem[c][CELT_BUFFER - n + j] = 0;
            float tmp = s + m;
            m = 0.85000610f * tmp;
            pcm[j * cc + c] = tmp * (1.f / SIG_SCALE);
        }
        st->preemph_mem[c] = m;
    }
    st->loss_count++;
}

int celt_decode(celt_decoder_t *st, const unsigned char *data, int len, float *pcm, int frame_size, opus_rc_t *rc_in)
{
    int cc = st->channels, C = st->stream_channels, lm, m, n, eff_end, total_bits, tell, silence;
    int postfilter_pitch = 0, postfilter_tapset = 0, is_transient, short_blocks, intra, spread;
    int tf_res[CELT_BANDS], cap[CELT_BANDS], offsets[CELT_BANDS], pulses[CELT_BANDS], fine[CELT_BANDS];
    int fine_priority[CELT_BANDS], dynalloc_logp, alloc_trim, bits, anti_collapse_rsv, coded, intensity = 0;
    int dual_stereo = 0, balance, anti_collapse_on = 0;
    float postfilter_gain = 0, band_e[2 * CELT_BANDS], *freq = st->tmp.freq, *x = st->tmp.x;
    unsigned char collapse[2 * CELT_BANDS];
    opus_rc_t local;
    opus_rc_t *rc = rc_in;
    band_ctx_t ctx;

    for (lm = 0; lm <= MAX_LM && SHORT_MDCT << lm != frame_size; lm++)
        ;
    if (lm > MAX_LM || len < 0 || len > 1275)
        return -1;
    m = 1 << lm;
    n = m * SHORT_MDCT;
    eff_end = st->end;
    if (data == NULL || len <= 1) {
        conceal(st, pcm, n);
        return n;
    }
    if (!rc) {
        rc_init(&local, data, (unsigned)len);
        rc = &local;
    }
    if (C == 1)
        for (int i = 0; i < CELT_BANDS; i++)
            if (st->old_band_e[CELT_BANDS + i] > st->old_band_e[i])
                st->old_band_e[i] = st->old_band_e[CELT_BANDS + i];

    total_bits = len * 8;
    tell = rc_tell(rc);
    if (tell >= total_bits)
        silence = 1;
    else if (tell == 1)
        silence = rc_bit_logp(rc, 15);
    else
        silence = 0;
    if (silence) {
        /* Pretend every remaining bit was read. */
        tell = len * 8;
        rc->nbits_total += tell - rc_tell(rc);
    }

    if (st->start == 0 && tell + 16 <= total_bits) {
        if (rc_bit_logp(rc, 1)) {
            int octave = (int)rc_uint(rc, 6), qg;
            postfilter_pitch = (16 << octave) + (int)rc_bits(rc, (unsigned)(4 + octave)) - 1;
            qg = (int)rc_bits(rc, 3);
            if (rc_tell(rc) + 2 <= total_bits)
                postfilter_tapset = rc_icdf(rc, k_tapset_icdf, 2);
            postfilter_gain = 0.09375f * (float)(qg + 1);
        }
        tell = rc_tell(rc);
    }
    is_transient = lm > 0 && tell + 3 <= total_bits ? rc_bit_logp(rc, 3) : 0;
    if (lm > 0 && tell + 3 <= total_bits)
        tell = rc_tell(rc);
    short_blocks = is_transient ? m : 0;
    intra = tell + 3 <= total_bits ? rc_bit_logp(rc, 3) : 0;
    unquant_coarse(st, intra, rc, C, lm);

    /* Time-frequency resolution changes per band. */
    {
        unsigned budget = rc->storage * 8, t = (unsigned)rc_tell(rc);
        int logp = is_transient ? 2 : 4, tf_select_rsv = lm > 0 && t + (unsigned)logp + 1 <= budget, curr = 0;
        int tf_changed = 0, tf_select = 0;
        budget -= (unsigned)tf_select_rsv;
        for (int i = st->start; i < st->end; i++) {
            if (t + (unsigned)logp <= budget) {
                curr ^= rc_bit_logp(rc, (unsigned)logp);
                t = (unsigned)rc_tell(rc);
                tf_changed |= curr;
            }
            tf_res[i] = curr;
            logp = is_transient ? 4 : 5;
        }
        if (tf_select_rsv && k_tf_select[lm][4 * is_transient + 0 + tf_changed] !=
                                 k_tf_select[lm][4 * is_transient + 2 + tf_changed])
            tf_select = rc_bit_logp(rc, 1);
        for (int i = st->start; i < st->end; i++)
            tf_res[i] = k_tf_select[lm][4 * is_transient + 2 * tf_select + tf_res[i]];
    }

    tell = rc_tell(rc);
    spread = tell + 4 <= total_bits ? rc_icdf(rc, k_spread_icdf, 5) : SPREAD_NORMAL;

    for (int i = 0; i < CELT_BANDS; i++)
        cap[i] = (k_cache_caps[CELT_BANDS * (2 * lm + C - 1) + i] + 64) * C * ((k_ebands[i + 1] - k_ebands[i]) << lm) >> 2;

    /* Dynamic allocation boosts. */
    dynalloc_logp = 6;
    total_bits <<= BITRES;
    tell = (int)rc_tell_frac(rc);
    for (int i = st->start; i < st->end; i++) {
        int width = C * (k_ebands[i + 1] - k_ebands[i]) << lm;
        int quanta = imin(width << BITRES, imax(6 << BITRES, width)), loop_logp = dynalloc_logp, boost = 0;
        while (tell + (loop_logp << BITRES) < total_bits && boost < cap[i]) {
            int flag = rc_bit_logp(rc, (unsigned)loop_logp);
            tell = (int)rc_tell_frac(rc);
            if (!flag)
                break;
            boost += quanta;
            total_bits -= quanta;
            loop_logp = 1;
        }
        offsets[i] = boost;
        if (boost > 0)
            dynalloc_logp = imax(2, dynalloc_logp - 1);
    }
    alloc_trim = tell + (6 << BITRES) <= total_bits ? rc_icdf(rc, k_trim_icdf, 7) : 5;

    bits = ((len * 8) << BITRES) - (int)rc_tell_frac(rc) - 1;
    anti_collapse_rsv = is_transient && lm >= 2 && bits >= (lm + 2) << BITRES ? 1 << BITRES : 0;
    bits -= anti_collapse_rsv;
    coded = compute_allocation(st->start, st->end, offsets, cap, alloc_trim, &intensity, &dual_stereo, bits, &balance,
                               pulses, fine, fine_priority, C, lm, rc, NULL, 0);

    /* Fine energy */
    for (int i = st->start; i < st->end; i++) {
        if (fine[i] <= 0)
            continue;
        for (int c = 0; c < C; c++) {
            int q2 = (int)rc_bits(rc, (unsigned)fine[i]);
            st->old_band_e[i + c * CELT_BANDS] += ((float)q2 + .5f) * (float)(1 << (14 - fine[i])) * (1.f / 16384) - .5f;
        }
    }

    memset(x, 0, sizeof st->tmp.x);
    ctx.rc = rc;
    ctx.spread = spread;
    ctx.intensity = intensity;
    ctx.seed = st->rng;
    quant_all_bands(&ctx, &st->tmp, st->start, st->end, x, C == 2 ? x + n : NULL, collapse, pulses, short_blocks, dual_stereo,
                    tf_res, len * (8 << BITRES) - anti_collapse_rsv, balance, lm, coded);
    if (anti_collapse_rsv > 0)
        anti_collapse_on = (int)rc_bits(rc, 1);

    /* The last fine energy bits, by priority. */
    {
        int bits_left = len * 8 - rc_tell(rc);
        for (int prio = 0; prio < 2; prio++)
            for (int i = st->start; i < st->end && bits_left >= C; i++) {
                if (fine[i] >= MAX_FINE_BITS || fine_priority[i] != prio)
                    continue;
                for (int c = 0; c < C; c++) {
                    int q2 = (int)rc_bits(rc, 1);
                    st->old_band_e[i + c * CELT_BANDS] += ((float)q2 - .5f) * (float)(1 << (14 - fine[i] - 1)) * (1.f / 16384);
                    bits_left--;
                }
            }
    }
    if (anti_collapse_on)
        anti_collapse(st, x, collapse, lm, C, n, pulses, ctx.seed);

    /* Band amplitudes; RFC 8251 caps the log energy at 32. */
    for (int c = 0; c < C; c++)
        for (int i = 0; i < CELT_BANDS; i++) {
            float lg = st->old_band_e[i + c * CELT_BANDS] + k_e_means[i];
            if (lg > 32.f)
                lg = 32.f;
            band_e[i + c * CELT_BANDS] = i >= st->start && i < st->end ? om_exp2(lg) : 0;
        }
    if (silence)
        for (int i = 0; i < C * CELT_BANDS; i++) {
            band_e[i] = 0;
            st->old_band_e[i] = -28.f;
        }

    /* Denormalization */
    memset(freq, 0, sizeof st->tmp.freq);
    for (int c = 0; c < C; c++)
        for (int i = st->start; i < eff_end; i++)
            for (int j = m * k_ebands[i]; j < m * k_ebands[i + 1]; j++)
                freq[c * n + j] = x[c * n + j] * band_e[i + c * CELT_BANDS];

    for (int c = 0; c < cc; c++)
        memmove(st->mem[c], st->mem[c] + n, sizeof(float) * (size_t)(CELT_BUFFER - n));
    if (cc == 2 && C == 1)
        memcpy(freq + n, freq, sizeof(float) * (size_t)n);
    if (cc == 1 && C == 2)
        for (int i = 0; i < n; i++)
            freq[i] = 0.5f * (freq[i] + freq[n + i]);

    /* Inverse MDCTs into the buffer's end, then the overlap. */
    for (int c = 0; c < cc; c++) {
        float *out = st->mem[c] + CELT_BUFFER - n, *overlap = st->mem[c] + CELT_BUFFER;
        float *buf = st->tmp.syn;
        int blocks = short_blocks ? short_blocks : 1, nb = short_blocks ? SHORT_MDCT : n;
        memset(buf, 0, sizeof(float) * CELT_OVERLAP);
        for (int b = 0; b < blocks; b++)
            imdct(&st->tmp, &freq[c * n + b], buf + nb * b, short_blocks ? 0 : lm, blocks);
        for (int j = 0; j < CELT_OVERLAP; j++)
            out[j] = buf[j] + overlap[j];
        for (int j = CELT_OVERLAP; j < n; j++)
            out[j] = buf[j];
        for (int j = 0; j < CELT_OVERLAP; j++)
            overlap[j] = buf[n + j];
    }

    /* Pitch post-filter, crossfaded from the previous frame's parameters. */
    for (int c = 0; c < cc; c++) {
        float *syn = st->mem[c] + CELT_BUFFER - n;
        if (st->postfilter_period < COMBFILTER_MINPERIOD)
            st->postfilter_period = COMBFILTER_MINPERIOD;
        if (st->postfilter_period_old < COMBFILTER_MINPERIOD)
            st->postfilter_period_old = COMBFILTER_MINPERIOD;
        comb_filter(syn, syn, st->postfilter_period_old, st->postfilter_period, SHORT_MDCT, st->postfilter_gain_old,
                    st->postfilter_gain, st->postfilter_tapset_old, st->postfilter_tapset);
        if (lm != 0)
            comb_filter(syn + SHORT_MDCT, syn + SHORT_MDCT, st->postfilter_period, postfilter_pitch, n - SHORT_MDCT,
                        st->postfilter_gain, postfilter_gain, st->postfilter_tapset, postfilter_tapset);
    }
    st->postfilter_period_old = st->postfilter_period;
    st->postfilter_gain_old = st->postfilter_gain;
    st->postfilter_tapset_old = st->postfilter_tapset;
    st->postfilter_period = postfilter_pitch;
    st->postfilter_gain = postfilter_gain;
    st->postfilter_tapset = postfilter_tapset;
    if (lm != 0) {
        st->postfilter_period_old = st->postfilter_period;
        st->postfilter_gain_old = st->postfilter_gain;
        st->postfilter_tapset_old = st->postfilter_tapset;
    }

    /* Energy history */
    if (C == 1)
        memcpy(st->old_band_e + CELT_BANDS, st->old_band_e, sizeof(float) * CELT_BANDS);
    if (!is_transient) {
        memcpy(st->old_log_e2, st->old_log_e, sizeof st->old_log_e);
        memcpy(st->old_log_e, st->old_band_e, sizeof st->old_log_e);
        for (int i = 0; i < 2 * CELT_BANDS; i++) {
            float bg = st->background_log_e[i] + (float)m * 0.001f;
            st->background_log_e[i] = bg < st->old_band_e[i] ? bg : st->old_band_e[i];
        }
    } else {
        for (int i = 0; i < 2 * CELT_BANDS; i++)
            if (st->old_band_e[i] < st->old_log_e[i])
                st->old_log_e[i] = st->old_band_e[i];
    }
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < st->start; i++) {
            st->old_band_e[c * CELT_BANDS + i] = 0;
            st->old_log_e[c * CELT_BANDS + i] = st->old_log_e2[c * CELT_BANDS + i] = -28.f;
        }
        for (int i = st->end; i < CELT_BANDS; i++) {
            st->old_band_e[c * CELT_BANDS + i] = 0;
            st->old_log_e[c * CELT_BANDS + i] = st->old_log_e2[c * CELT_BANDS + i] = -28.f;
        }
    }
    st->rng = rc->rng;

    /* De-emphasis */
    for (int c = 0; c < cc; c++) {
        const float *syn = st->mem[c] + CELT_BUFFER - n;
        float mem = st->preemph_mem[c];
        for (int j = 0; j < n; j++) {
            float tmp = syn[j] + mem;
            mem = 0.85000610f * tmp;
            pcm[j * cc + c] = tmp * (1.f / SIG_SCALE);
        }
        st->preemph_mem[c] = mem;
    }
    st->loss_count = 0;
    return rc_tell(rc) > 8 * len ? -1 : n;
}

/* ==== Encoder ==== */

/* The forward MDCT matching imdct(): fold with the window, N/4-point FFT, rotations. */
static void mdct_forward(celt_encoder_t *st, const float *in, float *out, int lm)
{
    int n = 2 * (SHORT_MDCT << lm), n2 = n / 2, n4 = n / 4, i;
    const cpx_t *rot = g_rot[lm];
    cpx_t *z = (cpx_t *)st->z, *f = (cpx_t *)st->f;
    const float *xp1 = in + (CELT_OVERLAP >> 1), *xp2 = in + n2 - 1 + (CELT_OVERLAP >> 1);
    const float *wp1 = g_window + (CELT_OVERLAP >> 1), *wp2 = g_window + (CELT_OVERLAP >> 1) - 1;

    for (i = 0; i < CELT_OVERLAP >> 2; i++) {
        z[i].r = *wp2 * xp1[n2] + *wp1 * *xp2;
        z[i].i = *wp1 * *xp1 - *wp2 * xp2[-n2];
        xp1 += 2;
        xp2 -= 2;
        wp1 += 2;
        wp2 -= 2;
    }
    wp1 = g_window;
    wp2 = g_window + CELT_OVERLAP - 1;
    for (; i < n4 - (CELT_OVERLAP >> 2); i++) {
        z[i].r = *xp2;
        z[i].i = *xp1;
        xp1 += 2;
        xp2 -= 2;
    }
    for (; i < n4; i++) {
        z[i].r = -*wp1 * xp1[-n2] + *wp2 * *xp2;
        z[i].i = *wp2 * *xp1 + *wp1 * xp2[n2];
        xp1 += 2;
        xp2 -= 2;
        wp1 += 2;
        wp2 -= 2;
    }
    /* Pre-rotation by -e^(-i theta), conjugated for the forward transform through the inverse FFT. */
    for (i = 0; i < n4; i++) {
        float re = z[i].r, im = z[i].i;
        cpx_t w;
        w.r = -(re * rot[i].r + im * rot[i].i);
        w.i = -(im * rot[i].r - re * rot[i].i);
        z[i].r = w.r;
        z[i].i = -w.i;
    }
    ifft_rec(f, z, n4, 1);
    for (i = 0; i < n4; i++) {
        float fr = f[i].r / (float)n4, fi = -f[i].i / (float)n4;
        out[2 * i] = fr * rot[i].r + fi * rot[i].i;
        out[n2 - 1 - 2 * i] = -(fi * rot[i].r - fr * rot[i].i);
    }
}

static void laplace_encode(opus_rce_t *enc, int *value, unsigned fs, int decay)
{
    unsigned fl = 0;
    int val = *value;

    if (val) {
        int s = -(val < 0), i;
        val = (val + s) ^ s;
        fl = fs;
        fs = (32768 - LAPLACE_MINP * (2 * LAPLACE_NMIN) - fs) * (unsigned)(16384 - decay) >> 15;
        for (i = 1; fs > 0 && i < val; i++) {
            fs *= 2;
            fl += fs + 2 * LAPLACE_MINP;
            fs = (fs * (unsigned)decay) >> 15;
        }
        if (!fs) {
            int ndi_max = (int)(32768 - fl + LAPLACE_MINP - 1), di;
            ndi_max = (ndi_max - s) >> 1;
            di = imin(val - i, ndi_max - 1);
            fl += (unsigned)((2 * di + 1 + s) * LAPLACE_MINP);
            fs = 32768 - fl < LAPLACE_MINP ? 32768 - fl : LAPLACE_MINP;
            *value = (i + di + s) ^ s;
        } else {
            fs += LAPLACE_MINP;
            fl += fs & (unsigned)~s;
        }
    }
    rce_encode_bin(enc, fl, fl + fs, 15);
}

static void quant_coarse(celt_encoder_t *st, opus_rce_t *enc, const float *log_e, float *error, int intra, int lm,
                         int budget, float max_decay)
{
    const unsigned char *prob = k_e_prob[lm][intra];
    float prev = 0, coef = intra ? 0 : k_pred_coef[lm], beta = intra ? k_beta_intra : k_beta_coef[lm];

    if (rce_tell(enc) + 3 <= budget)
        rce_bit_logp(enc, intra, 3);
    for (int i = 0; i < CELT_BANDS; i++) {
        float x = log_e[i], old_e = st->old_band_e[i] < -9.f ? -9.f : st->old_band_e[i], f, decay_bound;
        int qi, tell, bits_left;
        f = x - coef * old_e - prev;
        qi = (int)(f + .5f >= 0 ? f + .5f : f + .5f - 1.f); /* floor */
        decay_bound = (st->old_band_e[i] < -28.f ? -28.f : st->old_band_e[i]) - max_decay;
        if (qi < 0 && x < decay_bound) {
            qi += (int)(decay_bound - x);
            if (qi > 0)
                qi = 0;
        }
        tell = rce_tell(enc);
        bits_left = budget - tell - 3 * (CELT_BANDS - i);
        if (i != 0 && bits_left < 30) {
            if (bits_left < 24)
                qi = imin(1, qi);
            if (bits_left < 16)
                qi = imax(-1, qi);
        }
        if (budget - tell >= 15) {
            int pi = 2 * imin(i, 20);
            laplace_encode(enc, &qi, (unsigned)prob[pi] << 7, prob[pi + 1] << 6);
        } else if (budget - tell >= 2) {
            qi = imax(-1, imin(qi, 1));
            rce_icdf(enc, 2 * qi ^ -(qi < 0), k_small_energy_icdf, 2);
        } else if (budget - tell >= 1) {
            qi = imin(0, qi);
            rce_bit_logp(enc, -qi, 1);
        } else {
            qi = -1;
        }
        error[i] = f - (float)qi;
        st->old_band_e[i] = coef * old_e + prev + (float)qi;
        prev = prev + (float)qi - beta * (float)qi;
    }
}

/* PVQ codeword index of a pulse vector (the inverse of decode_pulses). */
static void encode_pulses(const int *y, int n, int k, opus_rce_t *enc)
{
    unsigned u[132], idx, nc;
    int j = n - 2, kk;

    u[0] = 0;
    for (kk = 1; kk <= k + 1; kk++)
        u[kk] = (unsigned)((kk << 1) - 1);
    idx = y[n - 1] < 0;
    kk = y[n - 1] < 0 ? -y[n - 1] : y[n - 1];
    idx += u[kk];
    kk += y[j] < 0 ? -y[j] : y[j];
    if (y[j] < 0)
        idx += u[kk + 1];
    while (j-- > 0) {
        /* The next row: u[i][j] = u[i-1][j] + u[i][j-1] + u[i-1][j-1] */
        unsigned u0 = 0, u1;
        int m = 1;
        do {
            u1 = u[m] + u[m - 1] + u0;
            u[m - 1] = u0;
            u0 = u1;
        } while (++m < k + 2);
        u[m - 1] = u0;
        idx += u[kk];
        kk += y[j] < 0 ? -y[j] : y[j];
        if (y[j] < 0)
            idx += u[kk + 1];
    }
    nc = u[kk] + u[kk + 1];
    rce_uint(enc, idx, nc);
}

/* Pyramid vector quantization of x (unit norm) with k pulses: a greedy search after a projection. */
static void alg_quant(float *x, int n, int k, int spread, int b, opus_rce_t *enc)
{
    int iy[176], signx[176], pulses_left = k;
    float y[176], xy = 0, yy = 0;

    exp_rotation(x, n, 1, b, k, spread);
    for (int j = 0; j < n; j++) {
        signx[j] = x[j] > 0 ? 1 : -1;
        if (x[j] < 0)
            x[j] = -x[j];
        iy[j] = 0;
        y[j] = 0;
    }
    if (k > n >> 1) {
        float sum = 0, rcp;
        for (int j = 0; j < n; j++)
            sum += x[j];
        if (!(sum > EPSILON && sum < 64)) {
            x[0] = 1.f;
            for (int j = 1; j < n; j++)
                x[j] = 0;
            sum = 1.f;
        }
        rcp = (float)(k - 1) / sum;
        for (int j = 0; j < n; j++) {
            iy[j] = (int)(rcp * x[j]); /* floor, x is positive */
            y[j] = (float)iy[j];
            yy += y[j] * y[j];
            xy += x[j] * y[j];
            y[j] *= 2;
            pulses_left -= iy[j];
        }
    }
    if (pulses_left > n + 3) {
        float t = (float)pulses_left;
        yy += t * t + t * y[0];
        iy[0] += pulses_left;
        pulses_left = 0;
    }
    for (int i = 0; i < pulses_left; i++) {
        int best = 0;
        float best_num = -1e15f, best_den = 0;
        yy += 1;
        for (int j = 0; j < n; j++) {
            float rxy = xy + x[j], ryy = yy + y[j];
            rxy *= rxy;
            if (best_den * rxy > ryy * best_num) {
                best_den = ryy;
                best_num = rxy;
                best = j;
            }
        }
        xy += x[best];
        yy += y[best];
        y[best] += 2;
        iy[best]++;
    }
    for (int j = 0; j < n; j++)
        if (signx[j] < 0)
            iy[j] = -iy[j];
    encode_pulses(iy, n, k, enc);
}

typedef struct {
    opus_rce_t *enc;
    int spread, remaining_bits;
} enc_ctx_t;

/* The band's angle between its two halves, in 1/16384 of a quarter turn. */
static int band_itheta(const float *x, const float *y, int n)
{
    float emid = EPSILON, eside = EPSILON;

    for (int i = 0; i < n; i++) {
        emid += x[i] * x[i];
        eside += y[i] * y[i];
    }
    return (int)(.5f + 16384 * 0.63662f * om_atan2(om_sqrt(eside), om_sqrt(emid)));
}

/* quant_band() of the decoder, encoding a mono band: the same splits and the same bit accounting. */
static void quant_band_enc(enc_ctx_t *ctx, int i, float *x, int n, int b, int big_b, int tf_change, int lm, int level)
{
    opus_rce_t *enc = ctx->enc;
    int n_b = n / big_b, b0 = big_b, recombine = 0, split = 0, long_blocks = b0 == 1;
    float *y = NULL;

    if (n == 1) {
        if (ctx->remaining_bits >= 1 << BITRES) {
            rce_bits(enc, x[0] < 0, 1);
            ctx->remaining_bits -= 1 << BITRES;
        }
        return;
    }
    if (level == 0) {
        if (tf_change > 0)
            recombine = tf_change;
        for (int k = 0; k < recombine; k++)
            haar1(x, n >> k, 1 << k);
        big_b >>= recombine;
        n_b <<= recombine;
        while ((n_b & 1) == 0 && tf_change < 0) {
            haar1(x, n_b, big_b);
            big_b <<= 1;
            n_b >>= 1;
            tf_change++;
        }
        b0 = big_b;
        if (b0 > 1)
            deinterleave_hadamard(x, n_b >> recombine, b0 << recombine, long_blocks);
    }
    {
        const unsigned char *cache = pulse_cache(i, lm);
        if (lm != -1 && b > cache[cache[0]] + 12 && n > 2) {
            n >>= 1;
            y = x + n;
            split = 1;
            lm -= 1;
            big_b = (big_b + 1) >> 1;
        }
    }
    if (split) {
        int pulse_cap = k_log_n[i] + lm * (1 << BITRES), offset = (pulse_cap >> 1) - QTHETA_OFFSET;
        int qn = compute_qn(n, b, offset, pulse_cap, 0), itheta = band_itheta(x, y, n), tell, qalloc, delta, mbits;
        int sbits, rebalance;
        tell = (int)rce_tell_frac(enc);
        if (qn != 1) {
            itheta = (itheta * qn + 8192) >> 14;
            if (b0 > 1) {
                rce_uint(enc, (unsigned)itheta, (unsigned)(qn + 1));
            } else {
                int ft = ((qn >> 1) + 1) * ((qn >> 1) + 1);
                int fs = itheta <= qn >> 1 ? itheta + 1 : qn + 1 - itheta;
                int fl = itheta <= qn >> 1 ? itheta * (itheta + 1) >> 1 : ft - ((qn + 1 - itheta) * (qn + 2 - itheta) >> 1);
                rce_encode(enc, (unsigned)fl, (unsigned)(fl + fs), (unsigned)ft);
            }
            itheta = itheta * 16384 / qn;
        } else {
            itheta = 0;
        }
        qalloc = (int)rce_tell_frac(enc) - tell;
        b -= qalloc;
        if (itheta == 0)
            delta = -16384;
        else if (itheta == 16384)
            delta = 16384;
        else
            delta = frac_mul16((n - 1) << 7, bitexact_log2tan(bitexact_cos(16384 - itheta), bitexact_cos(itheta)));
        if (b0 > 1 && (itheta & 0x3fff)) {
            if (itheta > 8192)
                delta -= delta >> (4 - lm);
            else
                delta = imin(0, delta + (n << BITRES >> (5 - lm)));
        }
        mbits = imax(0, imin(b, (b - delta) / 2));
        sbits = b - mbits;
        ctx->remaining_bits -= qalloc;
        rebalance = ctx->remaining_bits;
        if (mbits >= sbits) {
            quant_band_enc(ctx, i, x, n, mbits, big_b, tf_change, lm, level + 1);
            rebalance = mbits - (rebalance - ctx->remaining_bits);
            if (rebalance > 3 << BITRES && itheta != 0)
                sbits += rebalance - (3 << BITRES);
            quant_band_enc(ctx, i, y, n, sbits, big_b, tf_change, lm, level + 1);
        } else {
            quant_band_enc(ctx, i, y, n, sbits, big_b, tf_change, lm, level + 1);
            rebalance = sbits - (rebalance - ctx->remaining_bits);
            if (rebalance > 3 << BITRES && itheta != 16384)
                mbits += rebalance - (3 << BITRES);
            quant_band_enc(ctx, i, x, n, mbits, big_b, tf_change, lm, level + 1);
        }
    } else {
        int q = bits2pulses(i, lm, b), curr = pulses2bits(i, lm, q);
        ctx->remaining_bits -= curr;
        while (ctx->remaining_bits < 0 && q > 0) {
            ctx->remaining_bits += curr;
            q--;
            curr = pulses2bits(i, lm, q);
            ctx->remaining_bits -= curr;
        }
        if (q != 0)
            alg_quant(x, n, get_pulses(q), ctx->spread, big_b, enc);
    }
}

void celt_encoder_init(celt_encoder_t *st)
{
    init_tables();
    memset(st, 0, sizeof *st);
}

int celt_encode(celt_encoder_t *st, const float *pcm, unsigned char *out, int nbytes)
{
    const int lm = 3, n = 960, total_bits = nbytes * 8;
    float band_e[CELT_BANDS], log_e[CELT_BANDS], error[CELT_BANDS];
    int cap[CELT_BANDS], offsets[CELT_BANDS] = {0}, pulses[CELT_BANDS], fine[CELT_BANDS], fine_priority[CELT_BANDS];
    int tf_res[CELT_BANDS], intensity = 0, dual_stereo = 0, balance, coded, bits, tell;
    opus_rce_t enc;
    enc_ctx_t ctx;

    if (nbytes < 2 || nbytes > 1275)
        return 0;
    rce_init(&enc, out, (unsigned)nbytes);

    /* Pre-emphasis, after the previous frame's overlap. */
    memcpy(st->in, st->in_mem, sizeof st->in_mem);
    for (int i = 0; i < n; i++) {
        float v = pcm[i] * SIG_SCALE;
        if (!(v == v))
            v = 0;
        st->in[CELT_OVERLAP + i] = v - st->preemph_mem;
        st->preemph_mem = 0.85000610f * v;
    }
    memcpy(st->in_mem, st->in + n, sizeof st->in_mem);

    if (rce_tell(&enc) == 1)
        rce_bit_logp(&enc, 0, 15); /* not silence */
    if (rce_tell(&enc) + 16 <= total_bits)
        rce_bit_logp(&enc, 0, 1); /* no pitch post-filter */
    if (rce_tell(&enc) + 3 <= total_bits)
        rce_bit_logp(&enc, 0, 3); /* no transient: one long MDCT */

    mdct_forward(st, st->in, st->freq, lm);
    for (int i = 0; i < CELT_BANDS; i++) {
        float sum = 1e-27f, g;
        for (int j = k_ebands[i] << lm; j < k_ebands[i + 1] << lm; j++)
            sum += st->freq[j] * st->freq[j];
        band_e[i] = om_sqrt(sum);
        log_e[i] = om_log2(band_e[i]) - k_e_means[i];
        g = 1.f / (1e-27f + band_e[i]);
        for (int j = k_ebands[i] << lm; j < k_ebands[i + 1] << lm; j++)
            st->x[j] = st->freq[j] * g;
    }

    /* Intra energy on the first frame, then prediction from the previous one. */
    quant_coarse(st, &enc, log_e, error, st->frames == 0, lm, total_bits, 16.f < .125f * (float)nbytes ? 16.f : .125f * (float)nbytes);

    /* No time-frequency changes. */
    {
        unsigned budget = (unsigned)total_bits, t = (unsigned)rce_tell(&enc);
        int tf_select_rsv = t + 4 + 1 <= budget;
        budget -= (unsigned)tf_select_rsv;
        for (int i = 0, logp = 4; i < CELT_BANDS; i++, logp = 5)
            if (t + (unsigned)logp <= budget) {
                rce_bit_logp(&enc, 0, (unsigned)logp);
                t = (unsigned)rce_tell(&enc);
            }
        if (tf_select_rsv && k_tf_select[lm][0] != k_tf_select[lm][2])
            rce_bit_logp(&enc, 0, 1);
        for (int i = 0; i < CELT_BANDS; i++)
            tf_res[i] = k_tf_select[lm][0];
    }
    if (rce_tell(&enc) + 4 <= total_bits)
        rce_icdf(&enc, SPREAD_NORMAL, k_spread_icdf, 5);

    for (int i = 0; i < CELT_BANDS; i++)
        cap[i] = (k_cache_caps[CELT_BANDS * (2 * lm) + i] + 64) * ((k_ebands[i + 1] - k_ebands[i]) << lm) >> 2;
    /* No dynamic allocation boosts. */
    tell = (int)rce_tell_frac(&enc);
    for (int i = 0; i < CELT_BANDS; i++)
        if (tell + (6 << BITRES) < total_bits << BITRES && 0 < cap[i]) {
            rce_bit_logp(&enc, 0, 6);
            tell = (int)rce_tell_frac(&enc);
        }
    if (tell + (6 << BITRES) <= total_bits << BITRES)
        rce_icdf(&enc, 5, k_trim_icdf, 7);

    bits = ((nbytes * 8) << BITRES) - (int)rce_tell_frac(&enc) - 1;
    coded = compute_allocation(0, CELT_BANDS, offsets, cap, 5, &intensity, &dual_stereo, bits, &balance, pulses, fine,
                               fine_priority, 1, lm, NULL, &enc, st->last_coded_bands);
    st->last_coded_bands = coded;

    /* Fine energy */
    for (int i = 0; i < CELT_BANDS; i++) {
        int frac = 1 << fine[i], q2;
        float offset;
        if (fine[i] <= 0)
            continue;
        q2 = (int)((error[i] + .5f) * (float)frac + 1024.f) - 1024; /* floor */
        q2 = q2 > frac - 1 ? frac - 1 : q2 < 0 ? 0 : q2;
        rce_bits(&enc, (unsigned)q2, (unsigned)fine[i]);
        offset = ((float)q2 + .5f) * (float)(1 << (14 - fine[i])) * (1.f / 16384) - .5f;
        st->old_band_e[i] += offset;
        error[i] -= offset;
    }

    /* The shapes, band by band. */
    ctx.enc = &enc;
    ctx.spread = SPREAD_NORMAL;
    for (int i = 0; i < CELT_BANDS; i++) {
        int t = (int)rce_tell_frac(&enc), b, w = (k_ebands[i + 1] - k_ebands[i]) << lm;
        if (i != 0)
            balance -= t;
        ctx.remaining_bits = nbytes * (8 << BITRES) - t - 1;
        if (i <= coded - 1) {
            int curr_balance = balance / imin(3, coded - i);
            b = imax(0, imin(16383, imin(ctx.remaining_bits + 1, pulses[i] + curr_balance)));
        } else {
            b = 0;
        }
        quant_band_enc(&ctx, i, st->x + (k_ebands[i] << lm), w, b, 1, tf_res[i], lm, 0);
        balance += pulses[i] + t;
    }

    /* The last fine energy bits, by priority. */
    {
        int bits_left = nbytes * 8 - rce_tell(&enc);
        for (int prio = 0; prio < 2; prio++)
            for (int i = 0; i < CELT_BANDS && bits_left >= 1; i++) {
                int q2;
                if (fine[i] >= MAX_FINE_BITS || fine_priority[i] != prio)
                    continue;
                q2 = error[i] < 0 ? 0 : 1;
                rce_bits(&enc, (unsigned)q2, 1);
                st->old_band_e[i] += ((float)q2 - .5f) * (float)(1 << (14 - fine[i] - 1)) * (1.f / 16384);
                bits_left--;
            }
    }
    st->frames++;
    st->rng = enc.rng;
    return rce_done(&enc);
}
