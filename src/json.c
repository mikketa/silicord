#include "json.h"
#include "sc_asm.h"

#define MAX_DEPTH 64

static const char *skip_ws(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

/* p points at the opening quote; returns the position after the closing one. */
static const char *skip_str(const char *p, const char *e)
{
    for (p++; p < e; p++) {
        if (*p == '\\')
            p++;
        else if (*p == '"')
            return p + 1;
    }
    return NULL;
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

int json_type(json_t v)
{
    if (v.p >= v.end)
        return JSON_INVALID;
    switch (*v.p) {
    case '{': return JSON_OBJECT;
    case '[': return JSON_ARRAY;
    case '"': return JSON_STRING;
    case 't': return JSON_TRUE;
    case 'f': return JSON_FALSE;
    case 'n': return JSON_NULL;
    default:  return (*v.p == '-' || (*v.p >= '0' && *v.p <= '9')) ? JSON_NUMBER : JSON_INVALID;
    }
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
    vend = skip_value(p, e, 0);
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
    size_t n = sc_strlen(s);
    const char *p = v.p + 1;

    if (json_type(v) != JSON_STRING || (size_t)(v.end - v.p) != n + 2)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (p[i] != s[i])
            return 0;
    return 1;
}

int json_get(json_t obj, const char *key, json_t *out)
{
    json_iter_t it;
    json_t k, v;

    if (json_type(obj) != JSON_OBJECT)
        return 0;
    json_iter(obj, &it);
    while (json_next(&it, &k, &v)) {
        if (json_str_eq(k, key)) {
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
