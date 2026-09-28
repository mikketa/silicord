#include <xmmintrin.h>
#include "opus_math.h"

float om_sqrt(float x)
{
    return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x)));
}

typedef union {
    float f;
    unsigned u;
} fbits_t;

float om_exp2(float x)
{
    fbits_t r;
    float f, p;
    int i;

    if (x > 127.f)
        x = 127.f;
    if (x < -126.f)
        return 0.f;
    /* x = i + f with f in [-0.5, 0.5] */
    i = (int)(x < 0 ? x - 0.5f : x + 0.5f);
    f = x - (float)i;
    /* 2^f by its Taylor series in f*ln2, to degree 7 (error below 1e-8 on the interval) */
    {
        float t = f * 0.69314718056f;
        p = 1.f + t * (1.f + t * (0.5f + t * (1.f / 6 + t * (1.f / 24 + t * (1.f / 120 + t * (1.f / 720 + t * (1.f / 5040)))))));
    }
    r.u = (unsigned)(i + 127) << 23;
    return p * r.f;
}

float om_log2(float x)
{
    fbits_t v;
    int e;
    float m, t, t2;

    v.f = x;
    e = (int)((v.u >> 23) & 255) - 127;
    v.u = (v.u & 0x7FFFFF) | 0x3F800000; /* mantissa in [1, 2) */
    m = v.f;
    if (m > 1.41421356f) {
        m *= 0.5f;
        e++;
    }
    /* log2(m) = 2/ln2 * atanh((m-1)/(m+1)) */
    t = (m - 1.f) / (m + 1.f);
    t2 = t * t;
    return (float)e + 2.88539008f * t * (1.f + t2 * (1.f / 3 + t2 * (1.f / 5 + t2 * (1.f / 7 + t2 * (1.f / 9)))));
}

/* sin on [-pi/4, pi/4] and cos on the same, as Taylor series to beyond double precision's needs. */
static double sin_small(double x)
{
    double x2 = x * x, term = x, sum = x;

    for (int n = 1; n < 12; n++) {
        term *= -x2 / (double)((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

static double cos_small(double x)
{
    double x2 = x * x, term = 1, sum = 1;

    for (int n = 1; n < 12; n++) {
        term *= -x2 / (double)((2 * n - 1) * (2 * n));
        sum += term;
    }
    return sum;
}

double om_sin(double x)
{
    long long q;
    double r;

    /* x = q * pi/2 + r, r in [-pi/4, pi/4] */
    q = (long long)(x / (OM_PI / 2) + (x < 0 ? -0.5 : 0.5));
    r = x - (double)q * (OM_PI / 2);
    switch (q & 3) {
    case 0:
        return sin_small(r);
    case 1:
        return cos_small(r);
    case 2:
        return -sin_small(r);
    default:
        return -cos_small(r);
    }
}

double om_cos(double x)
{
    return om_sin(x + OM_PI / 2);
}
