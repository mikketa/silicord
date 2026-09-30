#include <windows.h>
#include "lottie.h"
#include "json.h"
#include "mem.h"
#include "opus_math.h"

#define MAX_DEPTH 12     /* groups within groups, precomps within precomps, parents of parents */
#define MAX_LAYERS 1000  /* of one composition */
#define MAX_POINTS 10000 /* of one path */
#define CHUNK 65536

typedef struct {
    double a, b, c, d, e, f; /* as SVG's matrix(a b c d e f) */
} mat_t;

/* A fill or a stroke, at one frame. */
typedef struct {
    int on;
    double rgb[3], opacity, width;
    int cap, join;
} paint_t;

/* A keyframe: its time, value (and end value, in old files), and easing toward the next. */
typedef struct {
    double t, ox, oy, ix, iy;
    int hold, eased, ns, ne;
    double *s, *e;
} kf_t;

/*
 * A property: a value, or keyframes. A path's values are its vertices, six
 * numbers each: the point, its in tangent and its out tangent.
 */
typedef struct {
    int n, nkf, closed;
    double *v;
    kf_t *kf;
} prop_t;

/* translate(p) rotate(r) scale(s) translate(-a), and an opacity o. */
typedef struct {
    prop_t a, p, px, py, s, r, o;
    int split; /* position as two properties, x and y */
} xform_t;

enum { I_GROUP, I_PATH, I_ELLIPSE, I_RECT, I_FILL, I_STROKE };

typedef struct item item_t;
struct item {
    int kind, cap, join;
    /* path: vertices; ellipse and rect: position, size, roundness; fill and stroke: color, opacity, width */
    prop_t p1, p2, p3;
    /* groups: shapes and groups bottom first, the fill and stroke they take, their transform */
    item_t **draw;
    int ndraw;
    item_t *fill, *stroke;
    xform_t *tr;
};

typedef struct {
    int ty, parent, comp, hidden; /* parent: index in the composition; comp: a precomp's (or -1) */
    double ip, op, st, sr, sw, sh;
    xform_t ks;
    item_t *shapes;
    char color[16]; /* a solid's */
    char ref[64];   /* a precomp's asset id */
} layer_t;

typedef struct {
    char id[64];
    json_t json; /* while loading only */
    layer_t *layers;
    int n;
    mat_t *world; /* each layer's transform through its parents, worked out once a frame */
    double *opacity;
    char *done;
} comp_t;

typedef struct chunk {
    struct chunk *next;
    size_t used, cap, pad;
} chunk_t;

struct lottie {
    double w, h, ip, op, fr;
    comp_t *comps; /* the main composition, then the assets' */
    int ncomps;
    double *scratch; /* a path's vertices at one frame */
    int scratch_n;
    sb_t d;
    chunk_t *chunks;
    size_t bytes;
};

/* ---- Loading ---- */

/* Zeroed memory that lives as long as the animation. */
static void *arena(lottie_t *L, size_t n)
{
    chunk_t *c = L->chunks;
    void *p;

    n = (n + 15) & ~(size_t)15;
    if (!c || c->used + n > c->cap) {
        size_t cap = n > CHUNK ? n : CHUNK;
        c = mem_alloc(sizeof *c + cap);
        c->cap = cap;
        c->next = L->chunks;
        L->chunks = c;
        L->bytes += sizeof *c + cap;
    }
    p = (char *)(c + 1) + c->used;
    c->used += n;
    return p;
}

static double to_num(json_t v)
{
    const char *p = v.p;
    double n = 0, scale = 1;
    int neg = 0;

    if (json_type(v) != JSON_NUMBER)
        return 0;
    if (*p == '-') {
        neg = 1;
        p++;
    }
    for (; p < v.end && *p >= '0' && *p <= '9'; p++)
        n = n * 10 + (*p - '0');
    if (p < v.end && *p == '.')
        for (p++; p < v.end && *p >= '0' && *p <= '9'; p++)
            n += (*p - '0') * (scale /= 10);
    if (p < v.end && (*p == 'e' || *p == 'E')) {
        int ex = 0, eneg = 0;
        if (++p < v.end && (*p == '-' || *p == '+'))
            eneg = *p++ == '-';
        for (; p < v.end && *p >= '0' && *p <= '9' && ex < 400; p++)
            ex = ex * 10 + (*p - '0');
        while (ex-- > 0)
            n = eneg ? n / 10 : n * 10;
    }
    return neg ? -n : n;
}

/* A number, or the first `max` numbers of an array. Returns how many. */
static int nums(json_t v, double *out, int max)
{
    json_iter_t it;
    json_t x;
    int n = 0;

    if (json_type(v) == JSON_NUMBER) {
        out[0] = to_num(v);
        return 1;
    }
    if (json_type(v) != JSON_ARRAY)
        return 0;
    json_iter(v, &it);
    while (n < max && json_next(&it, NULL, &x) && json_type(x) == JSON_NUMBER)
        out[n++] = to_num(x);
    return n;
}

static double first_num(json_t obj, const char *key, double def)
{
    json_t v;
    double n[4];

    return json_get(obj, key, &v) && nums(v, n, 4) ? n[0] : def;
}

static int truthy(json_t v)
{
    return json_type(v) == JSON_TRUE || (json_type(v) == JSON_NUMBER && to_num(v) != 0);
}

static int is_true(json_t obj, const char *key)
{
    json_t v;

    return json_get(obj, key, &v) && truthy(v);
}

/* A keyframe's value: up to four numbers, or a path's vertices. Returns how many numbers. */
static int read_value(lottie_t *L, json_t v, int path, double **out, int *closed)
{
    json_t fv, fi, fo;
    json_iter_t a, b, c;
    size_t n;

    if (!path) {
        *out = arena(L, 4 * sizeof **out);
        return nums(v, *out, 4);
    }
    if (json_type(v) == JSON_ARRAY) { /* keyframe values come wrapped: [{v, i, o, c}] */
        json_iter(v, &a);
        if (!json_next(&a, NULL, &v))
            return 0;
    }
    if (!json_get(v, "v", &fv) || !json_get(v, "i", &fi) || !json_get(v, "o", &fo) ||
        (n = json_count(fv)) > MAX_POINTS)
        return 0;
    *out = arena(L, (n ? n : 1) * 6 * sizeof **out);
    json_iter(fv, &a);
    json_iter(fi, &b);
    json_iter(fo, &c);
    for (size_t i = 0; i < n; i++) {
        json_t x;
        if (json_next(&a, NULL, &x))
            nums(x, *out + i * 6, 2);
        if (json_next(&b, NULL, &x))
            nums(x, *out + i * 6 + 2, 2);
        if (json_next(&c, NULL, &x))
            nums(x, *out + i * 6 + 4, 2);
    }
    if (closed)
        *closed = is_true(v, "c");
    if ((int)n * 6 > L->scratch_n)
        L->scratch_n = (int)n * 6;
    return (int)n * 6;
}

/* Property `key` of obj ({a, k}: a value, or keyframes {t, s, e?, h?, o?, i?}). Returns 0 if it has none. */
static int read_prop(lottie_t *L, json_t obj, const char *key, prop_t *p, int path)
{
    json_t prop, k, first, kf, v, o, i;
    json_iter_t it;
    size_t n;
    int j = 0;

    if (!json_get(obj, key, &prop) || json_type(prop) != JSON_OBJECT || !json_get(prop, "k", &k))
        return 0;
    json_iter(k, &it);
    if (json_type(k) != JSON_ARRAY || !json_next(&it, NULL, &first) || json_type(first) != JSON_OBJECT ||
        !json_get(first, "t", &v)) {
        p->n = read_value(L, k, path, &p->v, &p->closed);
        return 1;
    }
    n = json_count(k);
    p->kf = arena(L, n * sizeof *p->kf);
    json_iter(k, &it);
    while (j < (int)n && json_next(&it, NULL, &kf)) {
        kf_t *q = &p->kf[j];
        q->t = first_num(kf, "t", 0);
        if (json_get(kf, "s", &v))
            q->ns = read_value(L, v, path, &q->s, j ? NULL : &p->closed);
        if (json_get(kf, "e", &v))
            q->ne = read_value(L, v, path, &q->e, NULL);
        q->hold = is_true(kf, "h");
        if (json_get(kf, "o", &o) && json_get(kf, "i", &i)) {
            q->eased = 1;
            q->ox = first_num(o, "x", 0);
            q->oy = first_num(o, "y", 0);
            q->ix = first_num(i, "x", 1);
            q->iy = first_num(i, "y", 1);
        }
        if (!q->ns && j) { /* an old-style last keyframe, only a time: where the one before ends */
            kf_t *b = &p->kf[j - 1];
            q->s = b->ne ? b->e : b->s;
            q->ns = b->ne ? b->ne : b->ns;
        }
        j++;
    }
    p->nkf = j;
    return 1;
}

static void read_xform(lottie_t *L, json_t ks, xform_t *x)
{
    json_t pos;

    read_prop(L, ks, "a", &x->a, 0);
    if (json_get(ks, "p", &pos) && is_true(pos, "s")) {
        x->split = 1;
        read_prop(L, pos, "x", &x->px, 0);
        read_prop(L, pos, "y", &x->py, 0);
    } else {
        read_prop(L, ks, "p", &x->p, 0);
    }
    read_prop(L, ks, "s", &x->s, 0);
    if (!read_prop(L, ks, "r", &x->r, 0))
        read_prop(L, ks, "rz", &x->r, 0);
    read_prop(L, ks, "o", &x->o, 0);
}

/*
 * A group's items. Lottie lists them top first, so they are kept in
 * reverse; its first fill and stroke paint its shapes (and those of the
 * groups within that have none of their own).
 */
static item_t *read_group(lottie_t *L, json_t items, int depth)
{
    item_t *g = arena(L, sizeof *g);
    json_iter_t iter;
    json_t it, k, v;
    size_t n = json_count(items);

    g->kind = I_GROUP;
    g->draw = arena(L, (n ? n : 1) * sizeof *g->draw);
    json_iter(items, &iter);
    while (json_next(&iter, NULL, &it)) {
        json_iter_t kit;
        json_t ty = {0}, sub = {0};
        int hidden = 0;
        item_t *x;
        /* one pass over its keys: "hd" often comes after a group's whole content */
        json_iter(it, &kit);
        while (json_next(&kit, &k, &v)) {
            if (json_str_eq(k, "ty"))
                ty = v;
            else if (json_str_eq(k, "it"))
                sub = v;
            else if (json_str_eq(k, "hd"))
                hidden = truthy(v);
        }
        if (hidden)
            continue;
        if (json_str_eq(ty, "tr")) {
            g->tr = arena(L, sizeof *g->tr);
            read_xform(L, it, g->tr);
            continue;
        }
        if (json_str_eq(ty, "gr")) {
            if (depth < MAX_DEPTH && g->ndraw < (int)n)
                g->draw[g->ndraw++] = read_group(L, sub, depth + 1);
            continue;
        }
        x = arena(L, sizeof *x);
        if (json_str_eq(ty, "fl") || json_str_eq(ty, "st")) {
            x->kind = json_str_eq(ty, "fl") ? I_FILL : I_STROKE;
            read_prop(L, it, "c", &x->p1, 0);
            read_prop(L, it, "o", &x->p2, 0);
            read_prop(L, it, "w", &x->p3, 0);
            x->cap = (int)first_num(it, "lc", 2);
            x->join = (int)first_num(it, "lj", 2);
            if (x->kind == I_FILL && !g->fill)
                g->fill = x;
            if (x->kind == I_STROKE && !g->stroke)
                g->stroke = x;
            continue;
        }
        if (json_str_eq(ty, "sh")) {
            x->kind = I_PATH;
            read_prop(L, it, "ks", &x->p1, 1);
        } else if (json_str_eq(ty, "el") || json_str_eq(ty, "rc")) {
            x->kind = json_str_eq(ty, "el") ? I_ELLIPSE : I_RECT;
            read_prop(L, it, "p", &x->p1, 0);
            read_prop(L, it, "s", &x->p2, 0);
            read_prop(L, it, "r", &x->p3, 0);
        } else {
            continue; /* gradients, trims, repeaters, merges: left out */
        }
        if (g->ndraw < (int)n)
            g->draw[g->ndraw++] = x;
    }
    for (int i = 0, j = g->ndraw - 1; i < j; i++, j--) {
        item_t *t = g->draw[i];
        g->draw[i] = g->draw[j];
        g->draw[j] = t;
    }
    return g;
}

static void read_comp(lottie_t *L, comp_t *c)
{
    json_iter_t it;
    json_t l;
    size_t n = json_count(c->json), cap;
    long long *ind, *parent;

    if (n > MAX_LAYERS)
        n = MAX_LAYERS;
    cap = n ? n : 1;
    c->layers = arena(L, cap * sizeof *c->layers);
    c->world = arena(L, cap * sizeof *c->world);
    c->opacity = arena(L, cap * sizeof *c->opacity);
    c->done = arena(L, cap);
    ind = mem_alloc(cap * 2 * sizeof *ind);
    parent = ind + cap;
    json_iter(c->json, &it);
    while (c->n < (int)n && json_next(&it, NULL, &l)) {
        layer_t *y = &c->layers[c->n];
        json_iter_t kit;
        json_t k, v, shapes = {0};
        y->ty = -1;
        y->ip = -1e9;
        y->op = 1e9;
        y->sr = 1;
        y->parent = y->comp = -1;
        ind[c->n] = parent[c->n] = -1;
        /* one pass over its keys: lookups one by one would step over its shapes each time */
        json_iter(l, &kit);
        while (json_next(&kit, &k, &v)) {
            if (json_str_eq(k, "ks"))
                read_xform(L, v, &y->ks);
            else if (json_str_eq(k, "shapes"))
                shapes = v;
            else if (json_str_eq(k, "refId"))
                json_raw(v, y->ref, sizeof y->ref);
            else if (json_str_eq(k, "sc"))
                json_raw(v, y->color, sizeof y->color);
            else if (json_str_eq(k, "ty"))
                y->ty = (int)to_num(v);
            else if (json_str_eq(k, "ip"))
                y->ip = to_num(v);
            else if (json_str_eq(k, "op"))
                y->op = to_num(v);
            else if (json_str_eq(k, "st"))
                y->st = to_num(v);
            else if (json_str_eq(k, "sr"))
                y->sr = to_num(v) > 0 ? to_num(v) : 1;
            else if (json_str_eq(k, "sw"))
                y->sw = to_num(v);
            else if (json_str_eq(k, "sh"))
                y->sh = to_num(v);
            else if (json_str_eq(k, "ind"))
                json_int(v, &ind[c->n]);
            else if (json_str_eq(k, "parent"))
                json_int(v, &parent[c->n]);
            /* hidden, or a matte's source (mattes are left out): not drawn, still a parent */
            else if ((json_str_eq(k, "hd") || json_str_eq(k, "td")) && truthy(v))
                y->hidden = 1;
        }
        if (y->ty == 4 && shapes.p)
            y->shapes = read_group(L, shapes, 0);
        c->n++;
    }
    for (int i = 0; i < c->n; i++)
        for (int j = 0; parent[i] >= 0 && j < c->n; j++)
            if (j != i && ind[j] == parent[i]) {
                c->layers[i].parent = j;
                break;
            }
    mem_free(ind);
}

lottie_t *lottie_load(const char *s, size_t n)
{
    json_t root, layers, assets = {0}, a, v;
    json_iter_t it;
    lottie_t *L;
    int na = 0;

    /* a cheap refusal of everything that is not JSON */
    while (n && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n'))
        s++, n--;
    if (!n || *s != '{' || !json_parse(s, n, &root) || !json_get(root, "layers", &layers) ||
        json_type(layers) != JSON_ARRAY || first_num(root, "w", 0) <= 0 || first_num(root, "h", 0) <= 0)
        return NULL;
    L = mem_alloc(sizeof *L);
    L->bytes = sizeof *L;
    L->w = first_num(root, "w", 0);
    L->h = first_num(root, "h", 0);
    L->ip = first_num(root, "ip", 0);
    L->op = first_num(root, "op", L->ip);
    L->fr = first_num(root, "fr", 30);
    json_get(root, "assets", &assets);
    json_iter(assets, &it);
    while (na < MAX_LAYERS && json_next(&it, NULL, &a))
        na += json_get(a, "layers", &v);
    L->comps = arena(L, (size_t)(1 + na) * sizeof *L->comps);
    L->comps[0].json = layers;
    L->ncomps = 1;
    json_iter(assets, &it);
    while (L->ncomps < 1 + na && json_next(&it, NULL, &a))
        if (json_get(a, "layers", &L->comps[L->ncomps].json)) {
            if (json_get(a, "id", &v))
                json_raw(v, L->comps[L->ncomps].id, sizeof L->comps[L->ncomps].id);
            L->ncomps++;
        }
    for (int i = 0; i < L->ncomps; i++)
        read_comp(L, &L->comps[i]);
    for (int i = 0; i < L->ncomps; i++) /* precomps to their compositions */
        for (int j = 0; j < L->comps[i].n; j++) {
            layer_t *y = &L->comps[i].layers[j];
            for (int k = 1; y->ty == 0 && y->ref[0] && k < L->ncomps; k++)
                if (!lstrcmpA(y->ref, L->comps[k].id)) {
                    y->comp = k;
                    break;
                }
        }
    L->scratch = arena(L, (size_t)(L->scratch_n ? L->scratch_n : 1) * sizeof *L->scratch);
    return L;
}

void lottie_info(const lottie_t *L, double *first, double *last, double *fps)
{
    *first = L->ip;
    *last = L->op;
    *fps = L->fr;
}

size_t lottie_bytes(const lottie_t *L)
{
    return L ? L->bytes + L->d.cap : 0;
}

void lottie_free(lottie_t *L)
{
    if (!L)
        return;
    while (L->chunks) {
        chunk_t *c = L->chunks;
        L->chunks = c->next;
        mem_free(c);
    }
    sb_free(&L->d);
    mem_free(L);
}

/* ---- One frame ---- */

/* One coordinate of a cubic bezier from 0 to 1 with control points p1, p2. */
static double bez(double p1, double p2, double t)
{
    double s = 1 - t;
    return 3 * s * s * t * p1 + 3 * s * t * t * p2 + t * t * t;
}

/* A keyframe's easing (a CSS-style cubic-bezier from its handles) at linear progress u. */
static double ease(const kf_t *k, double u)
{
    double lo = 0, hi = 1, t = u;

    if (!k->eased)
        return u;
    for (int i = 0; i < 30; i++) {
        t = (lo + hi) / 2;
        if (bez(k->ox, k->ix, t) < u)
            lo = t;
        else
            hi = t;
    }
    return bez(k->oy, k->iy, t);
}

/* A property at frame f, into `out` (left alone where it has nothing). Returns how many numbers. */
static int eval(const prop_t *p, double f, double *out, int max)
{
    const double *a = p->v, *b = p->v;
    int na = p->n, nb = p->n, n;
    double u = 0;

    if (p->nkf) {
        const kf_t *k = p->kf;
        int j = 0;
        while (j + 1 < p->nkf && f >= k[j + 1].t)
            j++;
        a = b = k[j].s;
        na = nb = k[j].ns;
        if (j + 1 < p->nkf && f > k[j].t && !k[j].hold && k[j + 1].t > k[j].t) {
            b = k[j].ne ? k[j].e : k[j + 1].s;
            nb = k[j].ne ? k[j].ne : k[j + 1].ns;
            u = ease(&k[j], (f - k[j].t) / (k[j + 1].t - k[j].t));
        }
    }
    if (nb != na) /* shapes that do not match: no blending */
        u = 0;
    n = na < max ? na : max;
    for (int i = 0; i < n; i++)
        out[i] = a[i] + (b[i] - a[i]) * u;
    return n;
}

static mat_t mul(mat_t x, mat_t y)
{
    mat_t r;
    r.a = x.a * y.a + x.c * y.b;
    r.b = x.b * y.a + x.d * y.b;
    r.c = x.a * y.c + x.c * y.d;
    r.d = x.b * y.c + x.d * y.d;
    r.e = x.a * y.e + x.c * y.f + x.e;
    r.f = x.b * y.e + x.d * y.f + x.f;
    return r;
}

static mat_t transform(const xform_t *x, double f, double *opacity)
{
    double an[3] = {0, 0, 0}, p[3] = {0, 0, 0}, s[3] = {100, 100, 100}, r = 0, o = 100, c, sn;
    mat_t m;

    eval(&x->a, f, an, 3);
    if (x->split) {
        eval(&x->px, f, &p[0], 1);
        eval(&x->py, f, &p[1], 1);
    } else {
        eval(&x->p, f, p, 3);
    }
    if (eval(&x->s, f, s, 3) == 1)
        s[1] = s[0];
    eval(&x->r, f, &r, 1);
    eval(&x->o, f, &o, 1);
    c = om_cos(r * OM_PI / 180);
    sn = om_sin(r * OM_PI / 180);
    m.a = s[0] / 100 * c;
    m.b = s[0] / 100 * sn;
    m.c = -s[1] / 100 * sn;
    m.d = s[1] / 100 * c;
    m.e = p[0] - (m.a * an[0] + m.c * an[1]);
    m.f = p[1] - (m.b * an[0] + m.d * an[1]);
    if (opacity)
        *opacity = o < 0 ? 0 : o > 100 ? 1 : o / 100;
    return m;
}

/* A number with up to `decimals` (2 or 4) digits after the point. */
static void put(sb_t *svg, double v, int decimals)
{
    long long scale = decimals > 2 ? 10000 : 100, n;
    char frac[8];
    int k;

    if (v > 1e9)
        v = 1e9;
    if (v < -1e9)
        v = -1e9;
    n = (long long)(v * (double)scale + (v < 0 ? -.5 : .5));
    if (n < 0) {
        sb_add(svg, "-");
        n = -n;
    }
    sb_i64(svg, n / scale);
    n %= scale;
    if (!n)
        return;
    for (k = decimals > 2 ? 4 : 2; k > 0; k--, n /= 10)
        frac[k] = (char)('0' + n % 10);
    frac[0] = '.';
    for (k = decimals > 2 ? 4 : 2; frac[k] == '0'; k--)
        ;
    sb_addn(svg, frac, (size_t)k + 1);
}

static void put_xy(sb_t *svg, const char *cmd, double x, double y)
{
    sb_add(svg, cmd);
    put(svg, x, 2);
    sb_add(svg, ",");
    put(svg, y, 2);
}

static void open_group(sb_t *svg, mat_t m, double opacity)
{
    const double *v = &m.a;

    sb_add(svg, "<g transform=\"matrix(");
    for (int i = 0; i < 6; i++) {
        if (i)
            sb_add(svg, " ");
        put(svg, v[i], 4);
    }
    sb_add(svg, ")\"");
    if (opacity < 1) {
        sb_add(svg, " opacity=\"");
        put(svg, opacity, 4);
        sb_add(svg, "\"");
    }
    sb_add(svg, ">");
}

static void path_d(lottie_t *L, const item_t *it, double f, sb_t *d)
{
    const double *v = L->scratch;
    int n = eval(&it->p1, f, L->scratch, L->scratch_n) / 6;

    for (int j = 1; j <= n; j++) {
        const double *a = v + (j - 1) * 6, *b = v + (j % n) * 6;
        if (j == 1)
            put_xy(d, "M", a[0], a[1]);
        if (j == n && !it->p1.closed)
            break;
        put_xy(d, "C", a[0] + a[4], a[1] + a[5]);
        put_xy(d, " ", b[0] + b[2], b[1] + b[3]);
        put_xy(d, " ", b[0], b[1]);
    }
    if (n && it->p1.closed)
        sb_add(d, "Z");
}

/* An ellipse as four bezier arcs. */
static void ellipse_d(const item_t *it, double f, sb_t *d)
{
    double c[3] = {0, 0, 0}, s[3] = {0, 0, 0}, rx, ry, k = 0.5522847498;

    eval(&it->p1, f, c, 3);
    eval(&it->p2, f, s, 3);
    rx = s[0] / 2;
    ry = s[1] / 2;
    put_xy(d, "M", c[0], c[1] - ry);
    put_xy(d, "C", c[0] + k * rx, c[1] - ry);
    put_xy(d, " ", c[0] + rx, c[1] - k * ry);
    put_xy(d, " ", c[0] + rx, c[1]);
    put_xy(d, "C", c[0] + rx, c[1] + k * ry);
    put_xy(d, " ", c[0] + k * rx, c[1] + ry);
    put_xy(d, " ", c[0], c[1] + ry);
    put_xy(d, "C", c[0] - k * rx, c[1] + ry);
    put_xy(d, " ", c[0] - rx, c[1] + k * ry);
    put_xy(d, " ", c[0] - rx, c[1]);
    put_xy(d, "C", c[0] - rx, c[1] - k * ry);
    put_xy(d, " ", c[0] - k * rx, c[1] - ry);
    put_xy(d, " ", c[0], c[1] - ry);
    sb_add(d, "Z");
}

/* A rectangle centered on its position, its corners rounded. */
static void rect_d(const item_t *it, double f, sb_t *d)
{
    double c[3] = {0, 0, 0}, s[3] = {0, 0, 0}, r = 0, x, y, w, h;

    eval(&it->p1, f, c, 3);
    eval(&it->p2, f, s, 3);
    eval(&it->p3, f, &r, 1);
    w = s[0];
    h = s[1];
    x = c[0] - w / 2;
    y = c[1] - h / 2;
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;
    if (r <= 0) {
        put_xy(d, "M", x, y);
        put_xy(d, "L", x + w, y);
        put_xy(d, "L", x + w, y + h);
        put_xy(d, "L", x, y + h);
        sb_add(d, "Z");
        return;
    }
    put_xy(d, "M", x + r, y);
    put_xy(d, "L", x + w - r, y);
    put_xy(d, "Q", x + w, y);
    put_xy(d, " ", x + w, y + r);
    put_xy(d, "L", x + w, y + h - r);
    put_xy(d, "Q", x + w, y + h);
    put_xy(d, " ", x + w - r, y + h);
    put_xy(d, "L", x + r, y + h);
    put_xy(d, "Q", x, y + h);
    put_xy(d, " ", x, y + h - r);
    put_xy(d, "L", x, y + r);
    put_xy(d, "Q", x, y);
    put_xy(d, " ", x + r, y);
    sb_add(d, "Z");
}

static paint_t paint_at(const item_t *it, double f)
{
    paint_t p = {1, {0, 0, 0}, 1, 1, 2, 2};
    double c[4] = {0, 0, 0, 1}, o = 100;

    eval(&it->p1, f, c, 4);
    if (c[0] > 1 || c[1] > 1 || c[2] > 1) /* some exporters write 0-255 */
        for (int i = 0; i < 3; i++)
            c[i] /= 255;
    for (int i = 0; i < 3; i++)
        p.rgb[i] = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
    eval(&it->p2, f, &o, 1);
    p.opacity = o < 0 ? 0 : o > 100 ? 1 : o / 100;
    eval(&it->p3, f, &p.width, 1);
    p.cap = it->cap;
    p.join = it->join;
    return p;
}

static void put_color(sb_t *svg, const double *rgb)
{
    static const char hex[] = "0123456789abcdef";
    char s[8] = "#";

    for (int i = 0; i < 3; i++) {
        int b = (int)(rgb[i] * 255 + .5);
        s[1 + i * 2] = hex[b >> 4];
        s[2 + i * 2] = hex[b & 15];
    }
    s[7] = 0;
    sb_add(svg, s);
}

/* Draws path `d` with the fill, then the stroke, that apply to it. */
static void emit(sb_t *svg, const sb_t *d, const paint_t *fill, const paint_t *stroke)
{
    static const char *caps[] = {"butt", "butt", "round", "square"};
    static const char *joins[] = {"miter", "miter", "round", "bevel"};

    if (!d->len)
        return;
    if (fill->on && fill->opacity > 0) {
        sb_add(svg, "<path d=\"");
        sb_addn(svg, d->data, d->len);
        sb_add(svg, "\" fill=\"");
        put_color(svg, fill->rgb);
        if (fill->opacity < 1) {
            sb_add(svg, "\" fill-opacity=\"");
            put(svg, fill->opacity, 4);
        }
        sb_add(svg, "\"/>");
    }
    if (stroke->on && stroke->opacity > 0 && stroke->width > 0) {
        sb_add(svg, "<path d=\"");
        sb_addn(svg, d->data, d->len);
        sb_add(svg, "\" fill=\"none\" stroke=\"");
        put_color(svg, stroke->rgb);
        if (stroke->opacity < 1) {
            sb_add(svg, "\" stroke-opacity=\"");
            put(svg, stroke->opacity, 4);
        }
        sb_add(svg, "\" stroke-width=\"");
        put(svg, stroke->width, 2);
        sb_add(svg, "\" stroke-linecap=\"");
        sb_add(svg, caps[stroke->cap & 3]);
        sb_add(svg, "\" stroke-linejoin=\"");
        sb_add(svg, joins[stroke->join & 3]);
        sb_add(svg, "\"/>");
    }
}

static void draw_group(lottie_t *L, sb_t *svg, const item_t *g, double f, const paint_t *fill_in,
                       const paint_t *stroke_in)
{
    paint_t fill = g->fill ? paint_at(g->fill, f) : *fill_in, stroke = g->stroke ? paint_at(g->stroke, f) : *stroke_in;
    double opacity = 1;

    if (g->tr)
        open_group(svg, transform(g->tr, f, &opacity), opacity);
    for (int i = 0; i < g->ndraw; i++) {
        const item_t *it = g->draw[i];
        if (it->kind == I_GROUP) {
            draw_group(L, svg, it, f, &fill, &stroke);
            continue;
        }
        sb_clear(&L->d);
        if (it->kind == I_PATH)
            path_d(L, it, f, &L->d);
        else if (it->kind == I_ELLIPSE)
            ellipse_d(it, f, &L->d);
        else
            rect_d(it, f, &L->d);
        emit(svg, &L->d, &fill, &stroke);
    }
    if (g->tr)
        sb_add(svg, "</g>");
}

/* A layer's transform, through its parents' (whose opacity it does not take). */
static mat_t world(comp_t *c, int i, double f, int depth)
{
    layer_t *y = &c->layers[i];

    if (!c->done[i]) {
        c->done[i] = 1;
        c->world[i] = transform(&y->ks, f, &c->opacity[i]);
        if (y->parent >= 0 && depth < MAX_DEPTH)
            c->world[i] = mul(world(c, y->parent, f, depth + 1), c->world[i]);
    }
    return c->world[i];
}

static void draw_comp(lottie_t *L, sb_t *svg, int ci, double f, int depth)
{
    static const paint_t none = {0};
    comp_t *c = &L->comps[ci];

    if (depth > MAX_DEPTH)
        return;
    for (int i = 0; i < c->n; i++)
        c->done[i] = 0;
    for (int i = c->n - 1; i >= 0; i--) {
        layer_t *y = &c->layers[i];
        double opacity;
        mat_t m;
        if (y->hidden || f < y->ip || f >= y->op)
            continue;
        m = world(c, i, f, 0);
        if ((opacity = c->opacity[i]) <= 0)
            continue;
        if (y->ty == 4 && y->shapes) {
            open_group(svg, m, opacity);
            draw_group(L, svg, y->shapes, f, &none, &none);
            sb_add(svg, "</g>");
        } else if (y->ty == 0 && y->comp > 0) { /* a precomposition, on its own clock */
            open_group(svg, m, opacity);
            draw_comp(L, svg, y->comp, (f - y->st) / y->sr, depth + 1);
            sb_add(svg, "</g>");
        } else if (y->ty == 1 && y->color[0]) { /* a solid: "#rrggbb", sw by sh */
            open_group(svg, m, opacity);
            sb_add(svg, "<path d=\"M0,0H");
            put(svg, y->sw, 2);
            sb_add(svg, "V");
            put(svg, y->sh, 2);
            sb_add(svg, "H0Z\" fill=\"");
            sb_add(svg, y->color);
            sb_add(svg, "\"/></g>");
        }
    }
}

void lottie_svg(lottie_t *L, double frame, sb_t *svg)
{
    sb_clear(svg);
    sb_add(svg, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"");
    put(svg, L->w, 2);
    sb_add(svg, "\" height=\"");
    put(svg, L->h, 2);
    sb_add(svg, "\" viewBox=\"0 0 ");
    put(svg, L->w, 2);
    sb_add(svg, " ");
    put(svg, L->h, 2);
    sb_add(svg, "\">");
    draw_comp(L, svg, 0, frame, 0);
    sb_add(svg, "</svg>");
}
