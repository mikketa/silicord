#include <emmintrin.h>
#include "json.h"
#include "sc_asm.h"

#define MAX_DEPTH 64

#ifdef _MSC_VER
#include <intrin.h>
static __inline unsigned first_bit(unsigned m)
{
    unsigned long i;
    _BitScanForward(&i, m);
    return (unsigned)i;
}
#else
#define first_bit(m) ((unsigned)__builtin_ctz(m))
#endif

/* Mask of the bytes among the 16 at p that are `a` or `b`. */
static __inline unsigned match2(const char *p, char a, char b)
{
    __m128i v = _mm_loadu_si128((const __m128i *)p);
    return (unsigned)_mm_movemask_epi8(
        _mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8(a)), _mm_cmpeq_epi8(v, _mm_set1_epi8(b))));
}

static const char *skip_ws(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

/* p points at the opening quote; returns the position after the closing one. 16 bytes a step. */
static const char *skip_str(const char *p, const char *e)
{
    for (p++;;) {
        unsigned m = 0;
        while (e - p >= 16 && !(m = match2(p, '"', '\\')))
            p += 16;
        if (m) {
            p += first_bit(m);
        } else {
            while (p < e && *p != '"' && *p != '\\')
                p++;
            if (p >= e)
                return NULL;
        }
        if (*p == '"')
            return p + 1;
        p += 2; /* the escaped character */
    }
}

static int is_scalar_char(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           c == '-' || c == '+' || c == '.' || c == 'E';
}

static const char *skip_value(const char *p, const char *e, int depth)
{
    char close;
    int obj;

    if (p >= e || depth > MAX_DEPTH)
        return NULL;
    if (*p == '"')
        return skip_str(p, e);
    if (*p != '{' && *p != '[') {
        const char *s = p;
        while (p < e && is_scalar_char(*p))
            p++;
        return p > s ? p : NULL;
    }

    obj = *p == '{';
    close = obj ? '}' : ']';
    p = skip_ws(p + 1, e);
    if (p < e && *p == close)
        return p + 1;
    for (;;) {
        if (obj) {
            if (p >= e || *p != '"' || !(p = skip_str(p, e)))
                return NULL;
            p = skip_ws(p, e);
            if (p >= e || *p != ':')
                return NULL;
            p = skip_ws(p + 1, e);
        }
        if (!(p = skip_value(p, e, depth + 1)))
            return NULL;
        p = skip_ws(p, e);
        if (p >= e)
            return NULL;
        if (*p == close)
            return p + 1;
        if (*p != ',')
            return NULL;
        p = skip_ws(p + 1, e);
    }
}

/* The bytes skip_checked() stops at: '"', '{' for an opening bracket, '}' for a closing one. */
static const unsigned char k_struct[256] = {['"'] = '"', ['{'] = '{', ['['] = '{', ['}'] = '}', [']'] = '}'};

/*
 * Steps over a value of a document json_parse() already checked: strings
 * are jumped, brackets counted. Much faster than skip_value() on the
 * large nested values that lookups walk past.
 */
static const char *skip_checked(const char *p, const char *e)
{
    int depth = 0;

    if (p >= e)
        return e;
    if (*p == '"') {
        const char *q = skip_str(p, e);
        return q ? q : e;
    }
    if (*p != '{' && *p != '[') {
        while (p < e && is_scalar_char(*p))
            p++;
        return p;
    }
    while (p < e) {
        switch (k_struct[(unsigned char)*p]) {
        case 0:
            p++;
            continue;
        case '"':
            if (!(p = skip_str(p, e)))
                return e;
            continue;
        case '{':
            depth++;
            break;
        default:
            if (--depth == 0)
                return p + 1;
        }
        p++;
    }
    return e;
}

int json_parse(const char *s, size_t n, json_t *out)
{
    const char *e = s + n;
    const char *p = skip_ws(s, e);
    const char *end = skip_value(p, e, 0);

    if (!end || skip_ws(end, e) != e)
        return 0;
    out->p = p;
    out->end = end;
    return 1;
}

void json_iter(json_t v, json_iter_t *it)
{
    int t = json_type(v);

    it->is_object = t == JSON_OBJECT;
    if (t == JSON_OBJECT || t == JSON_ARRAY) {
        it->p = skip_ws(v.p + 1, v.end);
        it->end = v.end - 1; /* closing bracket */
    } else {
        it->p = it->end = v.p;
    }
}

int json_next(json_iter_t *it, json_t *key, json_t *val)
{
    const char *p = it->p;
    const char *e = it->end;
    const char *vend;

    if (p >= e)
        return 0;
    if (it->is_object) {
        const char *kend = skip_str(p, e);
        if (key) {
            key->p = p;
            key->end = kend;
        }
        p = skip_ws(kend, e);
        p = skip_ws(p + 1, e); /* ':' */
    }
    vend = skip_checked(p, e);
    if (val) {
        val->p = p;
        val->end = vend;
    }
    p = skip_ws(vend, e);
    if (p < e && *p == ',')
        p = skip_ws(p + 1, e);
    it->p = p;
    return 1;
}

size_t json_count(json_t v)
{
    json_iter_t it;
    size_t n = 0;

    json_iter(v, &it);
    while (json_next(&it, NULL, NULL))
        n++;
    return n;
}

void json_raw(json_t v, char *dst, size_t size)
{
    const char *p = v.p, *e = v.end;
    size_t n = 0;

    if (json_type(v) == JSON_STRING) {
        p++;
        e--;
    } else if (json_type(v) != JSON_NUMBER) {
        e = p;
    }
    while (p < e && n + 1 < size)
        dst[n++] = *p++;
    if (size)
        dst[n] = 0;
}

int json_str_eq(json_t v, const char *s)
{
    const char *p = v.p + 1, *e = v.end - 1;

    /* Byte by byte, no strlen first: most comparisons (keys against a wanted one) end at the first byte. */
    if (json_type(v) != JSON_STRING || e < p)
        return 0;
    for (; p < e; p++, s++)
        if (!*s || *p != *s)
            return 0;
    return !*s;
}

int json_get(json_t obj, const char *key, json_t *out)
{
    json_iter_t it;
    json_t k, v;
    size_t n = sc_strlen(key);

    if (json_type(obj) != JSON_OBJECT)
        return 0;
    json_iter(obj, &it);
    while (json_next(&it, &k, &v)) {
        /* Keys are strings json_parse() checked: compare the length, then the bytes. */
        size_t i = 0;
        if ((size_t)(k.end - k.p) != n + 2)
            continue;
        while (i < n && k.p[1 + i] == key[i])
            i++;
        if (i == n) {
            *out = v;
            return 1;
        }
    }
    return 0;
}

int json_int(json_t v, long long *out)
{
    const char *p = v.p;
    int neg = 0;
    unsigned long long n = 0, max;

    if (json_type(v) != JSON_NUMBER)
        return 0;
    if (*p == '-') {
        neg = 1;
        p++;
    }
    /* Out of range is not an integer; the magnitude of LLONG_MIN is one more than LLONG_MAX. */
    max = neg ? 9223372036854775808ull : 9223372036854775807ull;
    for (; p < v.end && *p >= '0' && *p <= '9'; p++) {
        unsigned d = (unsigned)(*p - '0');
        if (n > (max - d) / 10)
            return 0;
        n = n * 10 + d;
    }
    *out = neg ? (long long)(0ull - n) : (long long)n;
    return 1;
}

static int hex4(const char *p, unsigned *out)
{
    unsigned v = 0;

    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static void put_utf8(sb_t *out, unsigned cp)
{
    char b[4];
    size_t n;

    if (cp < 0x80) {
        b[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    sb_addn(out, b, n);
}

int json_str(json_t v, sb_t *out)
{
    const char *p, *e, *run;

    if (json_type(v) != JSON_STRING)
        return 0;
    p = run = v.p + 1;
    e = v.end - 1;
    while (p < e) {
        unsigned cp, lo;

        if (*p != '\\') {
            p++;
            continue;
        }
        sb_addn(out, run, (size_t)(p - run));
        if (++p >= e)
            return 0;
        switch (*p++) {
        case '"':  sb_addn(out, "\"", 1); break;
        case '\\': sb_addn(out, "\\", 1); break;
        case '/':  sb_addn(out, "/", 1); break;
        case 'b':  sb_addn(out, "\b", 1); break;
        case 'f':  sb_addn(out, "\f", 1); break;
        case 'n':  sb_addn(out, "\n", 1); break;
        case 'r':  sb_addn(out, "\r", 1); break;
        case 't':  sb_addn(out, "\t", 1); break;
        case 'u':
            if (e - p < 4 || !hex4(p, &cp))
                return 0;
            p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u' &&
                hex4(p + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
            } else if (cp >= 0xD800 && cp < 0xE000) {
                cp = 0xFFFD; /* a lone surrogate has no UTF-8 form */
            }
            put_utf8(out, cp);
            break;
        default:
            return 0;
        }
        run = p;
    }
    sb_addn(out, run, (size_t)(p - run));
    return 1;
}
