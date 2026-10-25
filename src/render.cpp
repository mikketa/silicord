/*
 * Software renderer: DirectWrite lays the text out and rasterizes its glyphs,
 * once each (kept, then blended here), or draws them into a GDI bitmap we own
 * (color emoji, large text); everything else (anti-aliased shapes, scaled
 * images) is rasterized here. No Direct2D, so no WARP device and its caches.
 *
 * A frame is drawn in horizontal bands of BAND rows reusing one small bitmap:
 * the caller repeats its whole paint for each band and primitives outside it
 * are skipped. C++ only because the DirectWrite headers are: no exceptions,
 * no RTTI, no C runtime, no static constructors.
 */
#include <windows.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <d2d1_3.h>
#include <emmintrin.h>
extern "C" {
#include "md.h"
}
#include "render.h"

extern "C" {
#include "apng.h"
#include "lottie.h"
#include "mem.h"
#include "opus_math.h"
#include "twemoji.h"
}

#define BAND 512
#define MAX_CLIP 16

/* Placement new without the C++ runtime. */
struct place_t {};
inline void *operator new(size_t, place_t, void *p) { return p; }
inline void operator delete(void *, place_t, void *) {}

struct r_font {
    IDWriteTextFormat *format;
    IDWriteInlineObject *ellipsis;
};

/* Direct2D drawing on the CPU into a w x h bitmap, from which our pixels are copied. */
struct svg_target_t {
    ID2D1Factory *f;
    IWICImagingFactory *wic;
    IWICBitmap *bmp;
    ID2D1RenderTarget *rt;
    ID2D1DeviceContext5 *dc;
    UINT w, h;
};

struct r_image {
    UINT w, h;
    BYTE *pixels; /* premultiplied BGRA; for animations, the composed current frame */
    unsigned average;
    RECT drawn;   /* where it was drawn since r_image_drawn() */
    /* Animation (GIF): the file, its decoder, and where we are. */
    BYTE *file;
    size_t file_n;
    IWICImagingFactory *wic;
    IWICStream *stream;
    IWICBitmapDecoder *dec;
    UINT frames, frame;
    unsigned due;         /* GetTickCount() when the next frame is due */
    UINT delay;           /* of the current frame, ms */
    UINT disposal;        /* of the current frame */
    RECT area;            /* of the current frame, in the canvas */
    BYTE *saved;          /* canvas before the current frame, for disposal 3 */
    apng_t *apng;         /* animated PNGs (avatar decorations) play through their own decoder */
    lottie_t *lottie;     /* Lottie stickers: each frame drawn as SVG when due */
    double lottie_first, lottie_step;
    svg_target_t *target; /* ... into a Direct2D target kept while it plays (UI thread) */
    /*
     * A still image drawn at another size is resampled into a copy of that
     * size, kept for the next draws, two sizes at most (draw_image). Its
     * version changes when its pixels may have, through r_image_bits().
     */
    struct {
        int w, h;
        float su, sv, sw, sh;
        unsigned version;
        BYTE *pixels;
    } sized[2];
    unsigned version, sized_next;
};

static IDWriteFactory *g_dw;
static IDWriteFactory4 *g_dw4;      /* color glyph layers; NULL before Windows 10 1607 */
static IDWriteGdiInterop *g_gdi;
static IDWriteBitmapRenderTarget1 *g_target;
static IDWriteRenderingParams *g_params;
static HDC g_mem;
static UINT32 *g_bits;              /* row 0 of the band bitmap */
static int g_stride;                /* in pixels, negative for a bottom-up DIB */
static int g_tw, g_th;              /* band bitmap size */
static DWRITE_TEXT_ANTIALIAS_MODE g_aa = DWRITE_TEXT_ANTIALIAS_MODE_CLEARTYPE;

/* Frame in progress. */
static int g_active, g_w, g_h, g_y0, g_bh, g_y_end;
static RECT g_update; /* the frame's update box: nothing outside it is drawn */
static RECT g_clip[MAX_CLIP];
static int g_nclip;
static UINT32 *g_scratch;           /* saved pixels while drawing text */
static size_t g_scratch_n;
static r_image_t *(*g_emoji_image)(int index, int px); /* Discord's images of Unicode emoji, once set */

static void glyphs_frame_done(void);

/* ---- Pixels ---- */

static inline UINT32 *row(int y) /* global y, inside the band */
{
    return g_bits + (ptrdiff_t)(y - g_y0) * g_stride;
}

static inline void blend(UINT32 *d, unsigned argb, unsigned a) /* a: 0..255 */
{
    unsigned v = *d, out = 0;

    if (a >= 255) {
        *d = argb & 0xFFFFFF;
        return;
    }
    if (!a)
        return;
    for (int s = 0; s < 24; s += 8) {
        int dc = (int)(v >> s & 0xFF), sc = (int)(argb >> s & 0xFF);
        out |= (unsigned)(dc + ((sc - dc) * (int)a + 127) / 255) << s;
    }
    *d = out;
}

/*
 * A run of n pixels of one color at one alpha: stored as is when opaque, else
 * blended as blend() does, through a table per channel once the run is long.
 */
static void span(UINT32 *p, int n, unsigned argb, unsigned a)
{
    BYTE t[3][256];

    if (n <= 0 || !a)
        return;
    if (a >= 255) {
        UINT32 v = argb & 0xFFFFFF;
        for (int i = 0; i < n; i++)
            p[i] = v;
        return;
    }
    if (n < 64) {
        for (int i = 0; i < n; i++)
            blend(p + i, argb, a);
        return;
    }
    for (int c = 0; c < 3; c++) {
        int sc = (int)(argb >> (c * 8) & 0xFF);
        for (int d = 0; d < 256; d++)
            t[c][d] = (BYTE)(d + ((sc - d) * (int)a + 127) / 255);
    }
    for (int i = 0; i < n; i++) {
        UINT32 v = p[i];
        p[i] = (UINT32)t[0][v & 0xFF] | (UINT32)t[1][v >> 8 & 0xFF] << 8 | (UINT32)t[2][v >> 16 & 0xFF] << 16;
    }
}

static inline float fsqrt(float v)
{
    return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(v)));
}

static inline float clamp01(float v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int ifloor(float v) { return (int)v - ((float)(int)v > v); }

/* Visible area: the band, then the clip stack. */
static RECT clip_rect(void)
{
    RECT r = {g_update.left, imax(g_y0, g_update.top), g_update.right, imin(g_y0 + g_bh, g_update.bottom)};

    if (g_nclip) {
        const RECT *c = &g_clip[g_nclip - 1];
        r.left = imax(r.left, c->left);
        r.top = imax(r.top, c->top);
        r.right = imin(r.right, c->right);
        r.bottom = imin(r.bottom, c->bottom);
    }
    return r;
}

static int intersect(RECT *r, int x, int y, int w, int h)
{
    r->left = imax(r->left, x);
    r->top = imax(r->top, y);
    r->right = imin(r->right, x + w);
    r->bottom = imin(r->bottom, y + h);
    return r->left < r->right && r->top < r->bottom;
}

/* ---- Frame ---- */

static int ensure_target(int w, int h)
{
    HBITMAP bm;
    DIBSECTION ds;
    UINT32 *base;
    int pitch, top_down;

    if (g_target && w <= g_tw && h <= g_th)
        return 1;
    w = imax(w, g_tw);
    h = imax(h, g_th);
    if (!g_target) {
        IDWriteBitmapRenderTarget *t;
        if (FAILED(g_gdi->CreateBitmapRenderTarget(NULL, (UINT32)w, (UINT32)h, &t)))
            return 0;
        t->QueryInterface(__uuidof(IDWriteBitmapRenderTarget1), (void **)&g_target);
        t->Release();
        if (!g_target)
            return 0;
        g_target->SetPixelsPerDip(1);
        g_target->SetTextAntialiasMode(g_aa);
        g_mem = g_target->GetMemoryDC();
    } else if (FAILED(g_target->Resize((UINT32)w, (UINT32)h))) {
        return 0;
    }
    bm = (HBITMAP)GetCurrentObject(g_mem, OBJ_BITMAP);
    if (!bm || GetObjectW(bm, sizeof ds, &ds) != sizeof ds || !ds.dsBm.bmBits)
        return 0;
    base = (UINT32 *)ds.dsBm.bmBits;
    pitch = ds.dsBm.bmWidthBytes / 4;
    /* The DIB's row order is not documented: ask GDI where (0, 0) lands. */
    base[0] = 0;
    SetPixelV(g_mem, 0, 0, RGB(255, 255, 255));
    GdiFlush();
    top_down = base[0] != 0;
    g_bits = top_down ? base : base + (ptrdiff_t)(ds.dsBm.bmHeight - 1) * pitch;
    g_stride = top_down ? pitch : -pitch;
    g_tw = w;
    g_th = h;
    return 1;
}

extern "C" int r_begin(HDC dc, int w, int h)
{
    if (!g_active) {
        RECT clip;
        if (GetClipBox(dc, &clip) == NULLREGION)
            return 0; /* nothing to paint: a WM_PAINT with an empty update region */
        g_active = 1;
        g_y0 = 0;
        g_y_end = h;
        g_update.left = g_update.top = 0;
        g_update.right = w;
        g_update.bottom = h;
        /* Only the bands the update region touches, and in them only its box. */
        if (GetClipBox(dc, &clip) != ERROR && clip.bottom > clip.top && clip.right > clip.left) {
            g_y0 = clip.top / BAND * BAND;
            g_y_end = clip.bottom < h ? clip.bottom : h;
            g_update.left = imax(clip.left, 0);
            g_update.top = imax(clip.top, 0);
            g_update.right = imin(clip.right, w);
            g_update.bottom = imin(clip.bottom, h);
        }
    } else {
        g_y0 += BAND;
    }
    if (g_y0 >= g_y_end || g_y0 >= h || w <= 0 || !ensure_target(w, BAND)) {
        /* Frame done: the text scratches can reach a band's size, do not keep them between frames. */
        mem_free(g_scratch);
        g_scratch = NULL;
        g_scratch_n = 0;
        glyphs_frame_done();
        g_active = 0;
        return 0;
    }
    g_w = w;
    g_h = h;
    g_bh = imin(BAND, h - g_y0);
    g_nclip = 0;
    return 1;
}

extern "C" void r_end(HDC dc)
{
    int top = imax(g_y0, g_update.top), bottom = imin(g_y0 + g_bh, g_update.bottom);

    if (bottom > top && g_update.right > g_update.left)
        BitBlt(dc, g_update.left, top, g_update.right - g_update.left, bottom - top, g_mem, g_update.left, top - g_y0, SRCCOPY);
}

extern "C" int r_visible(int y, int h)
{
    RECT r = clip_rect();

    return y < r.bottom && y + h > r.top;
}

extern "C" int r_visible_box(int x, int y, int w, int h)
{
    RECT r = clip_rect();

    return intersect(&r, x, y, w, h);
}

extern "C" void r_clip(int x, int y, int w, int h)
{
    RECT r = g_nclip ? g_clip[g_nclip - 1] : RECT{-100000, -100000, 100000, 100000};

    if (g_nclip == MAX_CLIP)
        return;
    r.left = imax(r.left, x);
    r.top = imax(r.top, y);
    r.right = imin(r.right, x + w);
    r.bottom = imin(r.bottom, y + h);
    if (r.right < r.left)
        r.right = r.left;
    if (r.bottom < r.top)
        r.bottom = r.top;
    g_clip[g_nclip++] = r;
}

extern "C" void r_unclip(void)
{
    if (g_nclip)
        g_nclip--;
}

/* ---- Shapes ---- */

extern "C" void r_fill(int x, int y, int w, int h, unsigned argb)
{
    RECT r = clip_rect();
    unsigned a = argb >> 24;

    if (!a || !intersect(&r, x, y, w, h))
        return;
    for (int yy = r.top; yy < r.bottom; yy++)
        span(row(yy) + r.left, r.right - r.left, argb, a);
}

/* Coverage of the pixel centered on (px, py) by a rounded rectangle. */
static inline float rr_cover(float px, float py, float x0, float y0, float x1, float y1, float r)
{
    float cx = px < x0 + r ? x0 + r : px > x1 - r ? x1 - r : px;
    float cy = py < y0 + r ? y0 + r : py > y1 - r ? y1 - r : py;
    float dx = px - cx, dy = py - cy;

    if (dx == 0 && dy == 0) {
        float e = px - x0;
        if (x1 - px < e) e = x1 - px;
        if (py - y0 < e) e = py - y0;
        if (y1 - py < e) e = y1 - py;
        return clamp01(e + 0.5f);
    }
    return clamp01(r - fsqrt(dx * dx + dy * dy) + 0.5f);
}

static unsigned lerp_color(unsigned a, unsigned b, float t)
{
    unsigned out = 0;

    for (int s = 0; s < 32; s += 8) {
        float ca = (float)(a >> s & 0xFF), cb = (float)(b >> s & 0xFF);
        out |= (unsigned)(int)(ca + (cb - ca) * t + 0.5f) << s;
    }
    return out;
}

/*
 * Rounded rectangle, optionally as an outline `stroke` pixels wide, filled
 * with a vertical gradient from `top` to `bottom`.
 */
static void shape(int x, int y, int w, int h, int radius, int stroke, unsigned top, unsigned bottom)
{
    RECT r = clip_rect();
    float x0 = (float)x, y0 = (float)y, x1 = (float)(x + w), y1 = (float)(y + h);
    int irad = imin(radius, imin(w, h) / 2);
    float rad = (float)irad, s = (float)stroke, rin = rad - s > 0 ? rad - s : 0;
    /*
     * Past the corners' reach and the stroke from either side, the coverage of a
     * row no longer depends on x: the middle of each row is one span, and only
     * its two ends are computed pixel by pixel.
     */
    int mid0 = x + irad + stroke + 1, mid1 = x + w - irad - stroke - 1;

    if (w <= 0 || h <= 0 || !intersect(&r, x, y, w, h))
        return;
    if (mid1 < mid0)
        mid1 = mid0;
    for (int yy = r.top; yy < r.bottom; yy++) {
        UINT32 *line = row(yy);
        float py = (float)yy + 0.5f;
        unsigned c = top == bottom ? top : lerp_color(top, bottom, ((float)(yy - y) + 0.5f) / (float)h);
        unsigned a = c >> 24;
        int ms = imax(r.left, mid0), me = imin(r.right, mid1);

        for (int part = 0; part < 2; part++) {
            int from = part ? imax(r.left, mid1) : r.left, to = part ? r.right : imin(r.right, imax(r.left, mid0));
            for (int xx = from; xx < to; xx++) {
                float px = (float)xx + 0.5f, cov = rr_cover(px, py, x0, y0, x1, y1, rad);
                if (stroke)
                    cov -= rr_cover(px, py, x0 + s, y0 + s, x1 - s, y1 - s, rin);
                if (cov > 0)
                    blend(line + xx, c, (unsigned)(cov * (float)a + 0.5f));
            }
        }
        if (ms < me) {
            float px = (float)ms + 0.5f, cov = rr_cover(px, py, x0, y0, x1, y1, rad);
            if (stroke)
                cov -= rr_cover(px, py, x0 + s, y0 + s, x1 - s, y1 - s, rin);
            if (cov > 0)
                span(line + ms, me - ms, c, (unsigned)(cov * (float)a + 0.5f));
        }
    }
}

extern "C" void r_round(int x, int y, int w, int h, int radius, unsigned argb)
{
    shape(x, y, w, h, radius, 0, argb, argb);
}

extern "C" void r_circle(int x, int y, int d, unsigned argb)
{
    shape(x, y, d, d, d / 2, 0, argb, argb);
}

extern "C" void r_round_gradient(int x, int y, int w, int h, int radius, unsigned top, unsigned bottom)
{
    shape(x, y, w, h, radius, 0, top, bottom);
}

extern "C" void r_round_outline(int x, int y, int w, int h, int radius, int width, unsigned argb)
{
    shape(x, y, w, h, radius, width, argb, argb);
}

/* Distance from (px, py) to the center line of one piece of a path. */
static float seg_distance(const r_seg_t *s, float px, float py)
{
    float ax = px - s->x0, ay = py - s->y0, bx = px - s->x1, by = py - s->y1;
    float to_ends = fsqrt(ax * ax + ay * ay < bx * bx + by * by ? ax * ax + ay * ay : bx * bx + by * by);

    if (s->arc) {
        float sx = s->x0 - s->cx, sy = s->y0 - s->cy, ex = s->x1 - s->cx, ey = s->y1 - s->cy;
        float dx = px - s->cx, dy = py - s->cy;
        /* Inside the sweep: between the radii to both ends, on the side the arc turns to. */
        if ((sx * dy - sy * dx) * (float)s->arc >= 0 && (dx * ey - dy * ex) * (float)s->arc >= 0) {
            float d = fsqrt(dx * dx + dy * dy) - fsqrt(sx * sx + sy * sy);
            return d < 0 ? -d : d;
        }
        return to_ends;
    }
    float lx = s->x1 - s->x0, ly = s->y1 - s->y0, len2 = lx * lx + ly * ly;
    if (len2 <= 0)
        return to_ends;
    float t = clamp01((ax * lx + ay * ly) / len2), qx = ax - t * lx, qy = ay - t * ly;
    return fsqrt(qx * qx + qy * qy);
}

extern "C" void r_stroke(const r_seg_t *seg, int n, float width, unsigned argb)
{
    RECT r = clip_rect();
    float hw = width / 2, x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    unsigned a = argb >> 24;

    if (n <= 0 || !a)
        return;
    /* Bounds: the ends of each line, the whole circle of each arc. */
    for (int i = 0; i < n; i++) {
        const r_seg_t *s = &seg[i];
        float l = s->x0 < s->x1 ? s->x0 : s->x1, t = s->y0 < s->y1 ? s->y0 : s->y1;
        float rr = s->x0 < s->x1 ? s->x1 : s->x0, b = s->y0 < s->y1 ? s->y1 : s->y0;
        if (s->arc) {
            float dx = s->x0 - s->cx, dy = s->y0 - s->cy, rad = fsqrt(dx * dx + dy * dy);
            l = s->cx - rad, t = s->cy - rad, rr = s->cx + rad, b = s->cy + rad;
        }
        x0 = l < x0 ? l : x0, y0 = t < y0 ? t : y0, x1 = rr > x1 ? rr : x1, y1 = b > y1 ? b : y1;
    }
    int bx = (int)(x0 - hw) - 1, by = (int)(y0 - hw) - 1;
    if (!intersect(&r, bx, by, (int)(x1 + hw) + 2 - bx, (int)(y1 + hw) + 2 - by))
        return;
    for (int yy = r.top; yy < r.bottom; yy++) {
        UINT32 *line = row(yy);
        float py = (float)yy + 0.5f;
        for (int xx = r.left; xx < r.right; xx++) {
            float px = (float)xx + 0.5f, d = 1e9f;
            for (int i = 0; i < n; i++) {
                float di = seg_distance(&seg[i], px, py);
                d = di < d ? di : d;
            }
            float cov = clamp01(hw - d + 0.5f);
            if (cov > 0)
                blend(line + xx, argb, (unsigned)(cov * (float)a + 0.5f));
        }
    }
}

/* ---- Images ---- */

/* A pixel's four bytes as 32-bit lanes. */
static inline __m128i px_lanes(const BYTE *p)
{
    __m128i zero = _mm_setzero_si128();
    UINT32 v;

    memcpy(&v, p, 4);
    return _mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)v), zero), zero);
}

/* Where a bilinear sample reads along one axis: the two source pixels and the weight of the second. */
typedef struct {
    int i0, i1;
    float f;
} tap_t;

static inline tap_t bilinear_tap(float u, int size)
{
    tap_t t;

    u -= 0.5f;
    if (u < 0) u = 0;
    t.i0 = (int)u;
    if (t.i0 > size - 1) t.i0 = size - 1;
    t.i1 = imin(t.i0 + 1, size - 1);
    t.f = u - (float)t.i0;
    if (t.f > 1) t.f = 1;
    return t;
}

/*
 * Bilinear sample, the four channels at once: the same float operations in
 * the same order as channel by channel, so the same result.
 */
static inline __m128i sample_bilinear(const r_image_t *img, tap_t tx, tap_t ty)
{
    const BYTE *r0 = img->pixels + (size_t)ty.i0 * img->w * 4, *r1 = img->pixels + (size_t)ty.i1 * img->w * 4;
    __m128 a = _mm_cvtepi32_ps(px_lanes(r0 + tx.i0 * 4)), b = _mm_cvtepi32_ps(px_lanes(r0 + tx.i1 * 4));
    __m128 d = _mm_cvtepi32_ps(px_lanes(r1 + tx.i0 * 4)), e = _mm_cvtepi32_ps(px_lanes(r1 + tx.i1 * 4));
    __m128 vx = _mm_set1_ps(tx.f), vy = _mm_set1_ps(ty.f);
    __m128 top = _mm_add_ps(a, _mm_mul_ps(_mm_sub_ps(b, a), vx)), bot = _mm_add_ps(d, _mm_mul_ps(_mm_sub_ps(e, d), vx));

    return _mm_cvttps_epi32(_mm_add_ps(_mm_add_ps(top, _mm_mul_ps(_mm_sub_ps(bot, top), vy)), _mm_set1_ps(0.5f)));
}

/* Average of the source pixels under a destination pixel, when shrinking. */
static inline __m128i sample_box(const r_image_t *img, float u0, float v0, float u1, float v1)
{
    int xa = (int)u0, ya = (int)v0, xb = (int)(u1 + 0.999f), yb = (int)(v1 + 0.999f), n;
    __m128i sum = _mm_setzero_si128();
    unsigned s[4];

    xa = imax(xa, 0);
    ya = imax(ya, 0);
    xb = imin(imax(xb, xa + 1), (int)img->w);
    yb = imin(imax(yb, ya + 1), (int)img->h);
    n = (xb - xa) * (yb - ya);
    for (int yy = ya; yy < yb; yy++) {
        const BYTE *p = img->pixels + ((size_t)yy * img->w + xa) * 4;
        for (int xx = xa; xx < xb; xx++, p += 4)
            sum = _mm_add_epi32(sum, px_lanes(p));
    }
    if (n <= 0)
        return _mm_setzero_si128();
    _mm_storeu_si128((__m128i *)s, sum);
    for (int c = 0; c < 4; c++)
        s[c] = (s[c] + (unsigned)n / 2) / (unsigned)n;
    return _mm_loadu_si128((const __m128i *)s);
}

/* Premultiplied source s over the opaque destination pixel d, with coverage cov. */
static inline UINT32 blend(__m128i sv, UINT32 d, float cov)
{
    unsigned s[4], a, out = 0;

    _mm_storeu_si128((__m128i *)s, sv);
    if (cov == 1) {
        /* Whole coverage: s * cov + 0.5 rounds back to s, and an opaque source replaces d. */
        a = s[3];
        if (a == 255)
            return s[0] | s[1] << 8 | s[2] << 16;
        if (!a)
            return d;
        for (int c = 0; c < 3; c++) {
            unsigned v2 = s[c] + (d >> (c * 8) & 0xFF) * (255 - a) / 255;
            out |= (v2 > 255 ? 255 : v2) << (c * 8);
        }
        return out;
    }
    a = (unsigned)((float)s[3] * cov + 0.5f);
    if (!a)
        return d;
    for (int c = 0; c < 3; c++) {
        unsigned sc = (unsigned)((float)s[c] * cov + 0.5f), dc = d >> (c * 8) & 0xFF;
        unsigned v2 = sc + dc * (255 - a) / 255;
        out |= (v2 > 255 ? 255 : v2) << (c * 8);
    }
    return out;
}

/* The columns' taps of the image being drawn (painting is single-threaded); wider draws work them out per pixel. */
#define MAX_TAPS 4096
static tap_t g_taps[MAX_TAPS];

/* Draws the source rectangle (su, sv, sw, sh) of img into (x, y, w, h), rounded by radius. */
static float g_image_alpha = 1; /* r_image_fade(): how opaque the image is drawn */

#define SIZED_MAX (128 * 128) /* pixels of the largest resampled copy kept */

/*
 * The source rectangle of a still image resampled to w x h, as draw_image
 * samples it pixel by pixel (the same operations: the same bytes), made
 * once for the draws at that size; NULL when it is not kept.
 */
static const BYTE *sized_copy(r_image_t *img, float su, float sv, float sw, float sh, int w, int h)
{
    float kx = sw / (float)w, ky = sh / (float)h;
    int shrink = kx > 1.5f || ky > 1.5f, k;
    BYTE *out;

    if (w * h > SIZED_MAX || img->frames > 1 || img->apng || img->lottie)
        return NULL;
    for (k = 0; k < 2; k++)
        if (img->sized[k].pixels && img->sized[k].w == w && img->sized[k].h == h && img->sized[k].su == su &&
            img->sized[k].sv == sv && img->sized[k].sw == sw && img->sized[k].sh == sh && img->sized[k].version == img->version)
            return img->sized[k].pixels;
    k = img->sized_next++ & 1;
    mem_free(img->sized[k].pixels);
    out = img->sized[k].pixels = (BYTE *)mem_alloc((size_t)w * h * 4);
    img->sized[k].w = w;
    img->sized[k].h = h;
    img->sized[k].su = su;
    img->sized[k].sv = sv;
    img->sized[k].sw = sw;
    img->sized[k].sh = sh;
    img->sized[k].version = img->version;
    for (int j = 0; j < h; j++) {
        float v = sv + ((float)j + 0.5f) * ky;
        tap_t ty = bilinear_tap(v, (int)img->h);
        for (int i = 0; i < w; i++, out += 4) {
            float u = su + ((float)i + 0.5f) * kx;
            __m128i s = shrink ? sample_box(img, u - kx / 2, v - ky / 2, u + kx / 2, v + ky / 2)
                               : sample_bilinear(img, bilinear_tap(u, (int)img->w), ty);
            s = _mm_packus_epi16(_mm_packs_epi32(s, s), s); /* four lanes of 0 to 255 back to bytes */
            *(UINT32 *)out = (UINT32)_mm_cvtsi128_si32(s);
        }
    }
    return img->sized[k].pixels;
}

static void draw_image(r_image_t *img, float su, float sv, float sw, float sh, int x, int y, int w, int h, int radius)
{
    RECT r = clip_rect();
    float kx = sw / (float)w, ky = sh / (float)h, rad = (float)imin(radius, imin(w, h) / 2);
    int shrink = kx > 1.5f || ky > 1.5f;
    const BYTE *pixels;
    int pw;

    if (!img || !img->pixels || w <= 0 || h <= 0 || !intersect(&r, x, y, w, h))
        return;
    /* Drawn whole at its own size: each pixel is a source pixel, which is what filtering would give. */
    int exact = su == 0 && sv == 0 && sw == (float)w && sh == (float)h && img->w == (UINT)w && img->h == (UINT)h;
    pixels = img->pixels;
    pw = (int)img->w;
    /* At another size: from the copy resampled to it, when there is one, as if drawn at its own size. */
    if (!exact && (pixels = sized_copy(img, su, sv, sw, sh, w, h)) != NULL) {
        exact = 1;
        pw = w;
    } else if (!exact) {
        pixels = img->pixels;
    }
    /* A column's horizontal taps are the same on every row: worked out once. */
    if (!exact && !shrink)
        for (int xx = r.left; xx < r.right && xx - r.left < MAX_TAPS; xx++)
            g_taps[xx - r.left] = bilinear_tap(su + ((float)xx + 0.5f - (float)x) * kx, (int)img->w);
    for (int yy = r.top; yy < r.bottom; yy++) {
        float py = (float)yy + 0.5f, v = sv + (py - (float)y) * ky;
        const BYTE *src = exact ? pixels + ((size_t)(yy - y) * pw + (size_t)(r.left - x)) * 4 : NULL;
        /* Rows clear of the corners are covered whole, as rr_cover() would find. */
        int plain = rad <= 0 || (py - (float)y >= rad + 1 && (float)(y + h) - py >= rad + 1);
        /*
         * Along the row: nothing before out_l and from out_r, whole coverage
         * in [full_l, full_r), rr_cover() only in between. The bounds come
         * from the corners' circles with a pixel to spare, so rr_cover()
         * would find exactly 0 and 1 where it is not asked.
         */
        int out_l = r.left, full_l = r.left, full_r = r.right, out_r = r.right;
        tap_t ty = bilinear_tap(v, (int)img->h);

        if (!plain) {
            float cxl = (float)x + rad, cxr = (float)(x + w) - rad;
            float cy = py < (float)y + rad ? (float)y + rad : py > (float)(y + h) - rad ? (float)(y + h) - rad : py;
            float dy2 = (py - cy) * (py - cy), in2 = (rad - .5f) * (rad - .5f) - dy2, out2 = (rad + .5f) * (rad + .5f) - dy2;
            float hw_out = out2 > 0 ? fsqrt(out2) : 0;
            out_l = imax(r.left, ifloor(cxl - hw_out) - 2);
            out_r = imin(r.right, ifloor(cxr + hw_out) + 2);
            if (rad >= .5f && in2 > 0) {
                float hw_in = fsqrt(in2);
                full_l = imax(out_l, ifloor(cxl - hw_in) + 2);
                full_r = imin(out_r, ifloor(cxr + hw_in) - 1);
            }
            if (rad < .5f || in2 <= 0 || full_r <= full_l)
                full_l = full_r = out_l;
        }
        for (int part = 0; part < 3; part++) {
            int xx = part == 0 ? out_l : part == 1 ? full_l : full_r, to = part == 0 ? full_l : part == 1 ? full_r : out_r;
            UINT32 *p = row(yy) + xx;

            if (part == 1 && exact && g_image_alpha >= 1) {
                /* Four opaque pixels at a time replace the destination's, alpha byte cleared as blending leaves it. */
                const __m128i opaque = _mm_set1_epi32(~0x00FFFFFF), rgb = _mm_set1_epi32(0x00FFFFFF);
                for (; xx + 4 <= to; xx += 4, p += 4) {
                    __m128i s4 = _mm_loadu_si128((const __m128i *)(src + (size_t)(xx - r.left) * 4));
                    if (_mm_movemask_epi8(_mm_cmpeq_epi32(_mm_and_si128(s4, opaque), opaque)) != 0xFFFF)
                        break;
                    _mm_storeu_si128((__m128i *)p, _mm_and_si128(s4, rgb));
                }
            }
            for (; xx < to; xx++, p++) {
                float px = (float)xx + 0.5f, u = su + (px - (float)x) * kx, cov = 1;
                __m128i s;

                if (part != 1 && (cov = rr_cover(px, py, (float)x, (float)y, (float)(x + w), (float)(y + h), rad)) <= 0)
                    continue;
                cov *= g_image_alpha;
                if (exact)
                    s = px_lanes(src + (size_t)(xx - r.left) * 4);
                else if (shrink)
                    s = sample_box(img, u - kx / 2, v - ky / 2, u + kx / 2, v + ky / 2);
                else
                    s = sample_bilinear(img, xx - r.left < MAX_TAPS ? g_taps[xx - r.left] : bilinear_tap(u, (int)img->w), ty);
                *p = blend(s, *p, cov);
            }
        }
    }
}

static void note_drawn(r_image_t *img, int x, int y, int w, int h)
{
    RECT *d = &img->drawn;

    if (d->right <= d->left) {
        d->left = x;
        d->top = y;
        d->right = x + w;
        d->bottom = y + h;
    } else {
        d->left = imin(d->left, x);
        d->top = imin(d->top, y);
        d->right = imax(d->right, x + w);
        d->bottom = imax(d->bottom, y + h);
    }
}

extern "C" void r_image(r_image_t *img, int x, int y, int w, int h, int radius)
{
    if (img) {
        draw_image(img, 0, 0, (float)img->w, (float)img->h, x, y, w, h, radius);
        if (img->frames > 1)
            note_drawn(img, x, y, w, h);
    }
}

extern "C" void r_image_fade(r_image_t *img, int x, int y, int w, int h, int radius, float alpha)
{
    if (alpha <= 0)
        return;
    g_image_alpha = alpha < 1 ? alpha : 1;
    r_image(img, x, y, w, h, radius);
    g_image_alpha = 1;
}

/* The image's alpha as a mask, filled with one color, at its own size (icons drawn in the color of the moment). */
extern "C" void r_image_tint(r_image_t *img, int x, int y, unsigned argb)
{
    RECT r = clip_rect();
    unsigned a = argb >> 24;

    if (!img || !a || !intersect(&r, x, y, (int)img->w, (int)img->h))
        return;
    for (int yy = r.top; yy < r.bottom; yy++) {
        const BYTE *src = img->pixels + ((size_t)(yy - y) * img->w + (size_t)(r.left - x)) * 4;
        UINT32 *p = row(yy) + r.left;
        for (int xx = r.left; xx < r.right; xx++, src += 4, p++)
            if (src[3])
                blend(p, argb, (src[3] * a + 127) / 255);
    }
}

extern "C" RECT r_image_drawn(r_image_t *img)
{
    RECT r = img->drawn;

    img->drawn = RECT{0, 0, 0, 0};
    return r;
}

extern "C" void r_image_cover(r_image_t *img, int x, int y, int w, int h, int radius)
{
    float s, sw, sh;

    if (!img || !img->w || !img->h || w <= 0 || h <= 0)
        return;
    s = (float)w / (float)img->w > (float)h / (float)img->h ? (float)w / (float)img->w : (float)h / (float)img->h;
    sw = (float)w / s;
    sh = (float)h / s;
    draw_image(img, ((float)img->w - sw) / 2, ((float)img->h - sh) / 2, sw, sh, x, y, w, h, radius);
}

/* ---- Animation ---- */

static UINT meta_uint(IWICMetadataQueryReader *q, const wchar_t *name, UINT fallback)
{
    PROPVARIANT v;
    UINT out = fallback;

    PropVariantInit(&v);
    if (q && SUCCEEDED(q->GetMetadataByName(name, &v))) {
        if (v.vt == VT_UI1)
            out = v.bVal;
        else if (v.vt == VT_UI2)
            out = v.uiVal;
        else if (v.vt == VT_UI4)
            out = v.ulVal;
    }
    PropVariantClear(&v);
    return out;
}

static void svg_size(const BYTE *p, size_t n, float *w, float *h);
static int svg_target_open(svg_target_t *t, UINT w, UINT h);
static void svg_target_close(svg_target_t *t);
static int svg_draw(svg_target_t *t, const void *data, size_t n, float sw, float sh, BYTE *out);

/*
 * Draws frame `index` of a Lottie animation in place of the last. Its
 * Direct2D target stays between frames: making one costs more than drawing.
 */
static int lottie_frame(r_image_t *img, UINT index)
{
    sb_t svg = {0};
    float sw, sh;
    int ok;

    if (!img->target) {
        img->target = (svg_target_t *)mem_alloc(sizeof *img->target);
        if (!svg_target_open(img->target, img->w, img->h)) {
            mem_free(img->target);
            img->target = NULL;
            return 0;
        }
    }
    lottie_svg(img->lottie, img->lottie_first + index * img->lottie_step, &svg);
    svg_size((const BYTE *)svg.data, svg.len, &sw, &sh);
    ok = sw > 0 && sh > 0 && svg_draw(img->target, svg.data, svg.len, sw, sh, img->pixels);
    sb_free(&svg);
    return ok;
}

/* Composes frame `index` over the canvas, applying the previous frame's disposal first. */
static int compose(r_image_t *img, UINT index)
{
    if (img->lottie)
        return lottie_frame(img, index);
    if (img->apng) {
        unsigned ms = apng_next(img->apng, img->pixels);
        if (!ms)
            return 0;
        img->delay = ms;
        return 1;
    }
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    IWICMetadataQueryReader *q = NULL;
    UINT fw = 0, fh = 0, fx, fy;
    BYTE *px = NULL;
    int ok = 0;

    if (index == 0) {
        memset(img->pixels, 0, (size_t)img->w * img->h * 4);
    } else if (img->disposal == 2) { /* restore to background: clear the previous frame's area */
        for (LONG y = img->area.top; y < img->area.bottom; y++)
            memset(img->pixels + ((size_t)y * img->w + img->area.left) * 4, 0, (size_t)(img->area.right - img->area.left) * 4);
    } else if (img->disposal == 3 && img->saved) { /* restore to previous */
        memcpy(img->pixels, img->saved, (size_t)img->w * img->h * 4);
    }
    if (FAILED(img->dec->GetFrame(index, &frame)))
        return 0;
    frame->GetMetadataQueryReader(&q);
    fx = meta_uint(q, L"/imgdesc/Left", 0);
    fy = meta_uint(q, L"/imgdesc/Top", 0);
    img->delay = meta_uint(q, L"/grctlext/Delay", 10) * 10;
    if (img->delay < 20)
        img->delay = 100; /* like browsers */
    img->disposal = meta_uint(q, L"/grctlext/Disposal", 0);
    if (img->disposal == 3) {
        if (!img->saved)
            img->saved = (BYTE *)mem_alloc((size_t)img->w * img->h * 4);
        memcpy(img->saved, img->pixels, (size_t)img->w * img->h * 4);
    }
    if (SUCCEEDED(frame->GetSize(&fw, &fh)) && SUCCEEDED(img->wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0,
                                   WICBitmapPaletteTypeCustom)) &&
        fw && fh && fw <= 4096 && fh <= 4096) {
        px = (BYTE *)mem_alloc((size_t)fw * fh * 4);
        if (SUCCEEDED(conv->CopyPixels(NULL, fw * 4, fw * fh * 4, px))) {
            /* Source-over onto the canvas, clipped to it. */
            for (UINT y = 0; y < fh && fy + y < img->h; y++)
                for (UINT x = 0; x < fw && fx + x < img->w; x++) {
                    BYTE *s = px + ((size_t)y * fw + x) * 4, *d = img->pixels + ((size_t)(fy + y) * img->w + fx + x) * 4;
                    unsigned a = s[3];
                    if (a == 255) {
                        d[0] = s[0], d[1] = s[1], d[2] = s[2], d[3] = 255;
                    } else if (a) {
                        for (int c = 0; c < 4; c++)
                            d[c] = (BYTE)(s[c] + d[c] * (255 - a) / 255);
                    }
                }
            /* Clamped on every side: a frame placed past the canvas leaves an empty area, not a negative one. */
            img->area.left = (LONG)(fx < img->w ? fx : img->w);
            img->area.top = (LONG)(fy < img->h ? fy : img->h);
            img->area.right = (LONG)(fx + fw < img->w ? fx + fw : img->w);
            img->area.bottom = (LONG)(fy + fh < img->h ? fy + fh : img->h);
            ok = 1;
        }
        mem_free(px);
    }
    if (conv)
        conv->Release();
    if (q)
        q->Release();
    frame->Release();
    return ok;
}

/* Keeps an animated GIF playable; called by the decoder with its objects. Returns 1 when adopted. */
static int adopt_animation(r_image_t *img, IWICImagingFactory *wic, const void *data, size_t n)
{
    IWICMetadataQueryReader *q = NULL;
    GUID fmt;
    UINT frames = 0, cw, ch;

    if (!img || n > (8u << 20))
        return 0;
    /* A decoder of our own over our own copy of the file: the caller's buffer goes away. */
    img->file = (BYTE *)mem_alloc(n);
    memcpy(img->file, data, n);
    img->file_n = n;
    if (FAILED(wic->CreateStream(&img->stream)) || FAILED(img->stream->InitializeFromMemory(img->file, (DWORD)n)) ||
        FAILED(wic->CreateDecoderFromStream(img->stream, NULL, WICDecodeMetadataCacheOnDemand, &img->dec)) ||
        FAILED(img->dec->GetContainerFormat(&fmt)) || !InlineIsEqualGUID(fmt, GUID_ContainerFormatGif) || /* no memcmp without the CRT */
        FAILED(img->dec->GetFrameCount(&frames)) || frames < 2)
        goto fail;
    img->dec->GetMetadataQueryReader(&q);
    cw = meta_uint(q, L"/logscrdesc/Width", 0);
    ch = meta_uint(q, L"/logscrdesc/Height", 0);
    if (q)
        q->Release();
    if (!cw || !ch || cw > 1024 || ch > 1024)
        goto fail;
    /* The canvas is the GIF's logical screen, not the first frame's size. */
    mem_free(img->pixels);
    img->w = cw;
    img->h = ch;
    img->pixels = (BYTE *)mem_alloc((size_t)cw * ch * 4);
    wic->AddRef();
    img->wic = wic;
    img->frames = frames;
    img->frame = 0;
    if (!compose(img, 0)) {
        img->frames = 0;
        return 0;
    }
    return 1;
fail:
    if (img->dec)
        img->dec->Release();
    if (img->stream)
        img->stream->Release();
    mem_free(img->file);
    img->dec = NULL;
    img->stream = NULL;
    img->file = NULL;
    img->file_n = 0;
    return 0;
}

/* An APNG plays at its own size; its first frame replaces WIC's still image only once it decoded. */
static void adopt_apng(r_image_t *img, const void *data, size_t n)
{
    apng_t *a = apng_open(data, n);
    unsigned w, h, ms;
    BYTE *canvas;

    if (!a)
        return;
    apng_size(a, &w, &h);
    canvas = (BYTE *)mem_alloc((size_t)w * h * 4);
    if (!(ms = apng_next(a, canvas))) {
        mem_free(canvas);
        apng_free(a);
        return;
    }
    mem_free(img->pixels);
    img->pixels = canvas;
    img->w = w;
    img->h = h;
    img->apng = a;
    img->frames = apng_frames(a);
    img->frame = 0;
    img->delay = ms;
}

extern "C" int r_image_frame(const r_image_t *img)
{
    return img ? (int)img->frame : 0;
}

extern "C" int r_image_animated(const r_image_t *img)
{
    return img && img->frames > 1;
}

extern "C" unsigned r_image_advance(r_image_t *img, unsigned now)
{
    if (!img || img->frames < 2)
        return 0;
    if (!img->due)
        img->due = now + img->delay;
    if ((int)(now - img->due) >= 0) {
        img->frame = (img->frame + 1) % img->frames;
        if (!compose(img, img->frame))
            img->frames = 0; /* broken file: stay on this frame */
        img->due = now + img->delay;
    }
    return (int)(img->due - now) > 0 ? img->due - now : 1;
}

extern "C" size_t r_image_bytes(const r_image_t *img)
{
    size_t sized = 0;

    for (int k = 0; img && k < 2; k++)
        if (img->sized[k].pixels)
            sized += (size_t)img->sized[k].w * img->sized[k].h * 4;
    return img ? sizeof *img + (size_t)img->w * img->h * 4 * (img->saved ? 2 : 1) + img->file_n +
                     (img->apng ? apng_bytes(img->apng) : 0) + lottie_bytes(img->lottie) +
                     (img->target ? (size_t)img->w * img->h * 4 : 0) + sized
               : 0;
}

extern "C" unsigned r_image_average(r_image_t *img)
{
    return img ? img->average : 0;
}

static unsigned average_of(const r_image_t *img)
{
    unsigned long long r = 0, g = 0, b = 0, n = 0;
    UINT step;

    if (!img->pixels)
        return 0;
    step = img->w * img->h > 4096 ? img->w * img->h / 4096 : 1;
    for (UINT i = 0; i < img->w * img->h; i += step) {
        const BYTE *p = img->pixels + (size_t)i * 4;
        if (p[3] < 250)
            continue;
        b += p[0];
        g += p[1];
        r += p[2];
        n++;
    }
    if (!n)
        return 0;
    return 0xFF000000u | (unsigned)(r / n) << 16 | (unsigned)(g / n) << 8 | (unsigned)(b / n);
}

/* ---- Text rendering ---- */

enum { PASS_ALL, PASS_MONO, PASS_COLOR }; /* PASS_MONO: coverage mask, no color glyphs */

struct draw_ctx {
    unsigned argb;
    int pass;
    /*
     * draw_layout's: what shows (band, update box, clip) and what the layout
     * can touch in the band. DirectWrite draws anywhere, opaque: when the ink
     * is not all inside `vis` or the text is translucent (`guard`), what it
     * draws is undone outside and made translucent after, from `saved`, the
     * ink area as it was before DirectWrite first drew there. Glyphs from the
     * cache are clipped and blended as they go and need none of it.
     */
    RECT vis, ink;
    int guard;
    UINT32 *saved;
};

/* A text color, set as a DirectWrite drawing effect on ranges of a layout. */
struct ColorEffect : IUnknown {
    LONG refs;
    unsigned argb;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (id == __uuidof(IUnknown)) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = NULL;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG n = InterlockedDecrement(&refs);
        if (!n)
            mem_free(this);
        return (ULONG)n;
    }
};

static ColorEffect *new_effect(unsigned argb)
{
    ColorEffect *e = new (place_t(), mem_alloc(sizeof(ColorEffect))) ColorEffect();

    e->refs = 1;
    e->argb = argb;
    return e;
}

static inline COLORREF gdi_color(unsigned argb)
{
    return RGB(argb >> 16 & 0xFF, argb >> 8 & 0xFF, argb & 0xFF);
}

static void set_aa(DWRITE_TEXT_ANTIALIAS_MODE m)
{
    if (g_aa != m) {
        g_aa = m;
        g_target->SetTextAntialiasMode(m);
    }
}

static UINT32 *scratch(size_t n);
static void save_rect(const RECT *r, UINT32 *out);

/* DirectWrite is about to draw for draw_layout: keeps what it can change, when that must be undone. */
static void guard(draw_ctx *c)
{
    if (c->guard && !c->saved) {
        c->saved = scratch((size_t)(c->ink.right - c->ink.left) * (size_t)(c->ink.bottom - c->ink.top));
        save_rect(&c->ink, c->saved);
    }
}

/* ---- Glyph cache ----
 * DirectWrite's bitmap target rasterizes every glyph again at each draw, then
 * blends it through GDI: about 2 us a glyph, most of a frame. Instead, the
 * ClearType coverage of each glyph is made once (IDWriteGlyphRunAnalysis) at
 * the sixth of a pixel DirectWrite places glyphs at, kept, and added up for
 * each run: that is the run's own coverage, bit for bit. It is blended here
 * as the bitmap target does, measured against it: in a space of gamma
 * GetGamma() (1.8), coverage first raised by GetEnhancedContrast() (0.5),
 * less for light text and not at all for white (the adjustment Windows
 * Terminal's renderer documents, dwrite_helpers.hlsl). The result is
 * DirectWrite's to a level or two of 255.
 */

#define GLYPH_BUCKETS 1024     /* a power of two */
#define GLYPH_BUDGET (1 << 21) /* bytes of coverage kept; past it the cache starts over, between frames */
#define GLYPH_MAX_EM 64.f      /* larger text goes through DirectWrite */

struct glyph_t {
    glyph_t *next;
    IDWriteFontFace *face; /* held */
    float em;
    UINT16 index;
    BYTE phase, mode, mm; /* x in sixths of a pixel (3 bits), y in quarters; rendering and measuring modes */
    int left, top, w, h;  /* coverage box, from the glyph's origin */
    BYTE cov[1];          /* w * h * 3: red, green and blue coverage */
};

static glyph_t *g_glyphs[GLYPH_BUCKETS];
static size_t g_glyph_bytes;
static int g_ct_ready, g_ct_bgr;  /* g_ct_ready -1: ClearType through DirectWrite only */
static float g_contrast;
static UINT16 g_lin[256];         /* a channel in the blending space, 0 to 65535 */
static BYTE g_unlin[65536];       /* and back, rounded */
static BYTE *g_run_cov;           /* a run's coverage, its glyphs added up */
static size_t g_run_cov_n;

static void glyphs_clear(void)
{
    for (int b = 0; b < GLYPH_BUCKETS; b++)
        while (g_glyphs[b]) {
            glyph_t *g = g_glyphs[b];
            g_glyphs[b] = g->next;
            g->face->Release();
            mem_free(g);
        }
    g_glyph_bytes = 0;
}

/* After a frame: the run buffer goes, and the glyphs if they grew past their budget. */
static void glyphs_frame_done(void)
{
    mem_free(g_run_cov);
    g_run_cov = NULL;
    g_run_cov_n = 0;
    if (g_glyph_bytes > GLYPH_BUDGET)
        glyphs_clear();
}

static float powf_(float x, float y)
{
    return x <= 0.f ? 0.f : om_exp2(y * om_log2(x));
}

static int ct_ready(void)
{
    if (!g_ct_ready) {
        DWRITE_PIXEL_GEOMETRY geo = g_params->GetPixelGeometry();
        float gamma = g_params->GetGamma();
        int l = 0;

        g_contrast = g_params->GetEnhancedContrast();
        for (int v = 0; v < 256; v++)
            g_lin[v] = (UINT16)(powf_((float)v / 255.f, gamma) * 65535.f + .5f);
        /* each value takes the linear values closer to it than to its neighbours, in its own space */
        for (int v = 0; v < 256; v++) {
            int end = v == 255 ? 65536 : (int)(powf_(((float)v + .5f) / 255.f, gamma) * 65535.f + .5f);
            for (; l < end; l++)
                g_unlin[l] = (BYTE)v;
        }
        g_ct_bgr = geo == DWRITE_PIXEL_GEOMETRY_BGR;
        g_ct_ready = geo == DWRITE_PIXEL_GEOMETRY_FLAT ? -1 : 1;
    }
    return g_ct_ready > 0;
}

/* Glyph `index` of a run's face at sub-pixel `phase`: from the cache, else rasterized; NULL if it cannot be. */
static glyph_t *glyph_get(const DWRITE_GLYPH_RUN *run, UINT16 index, int phase, DWRITE_RENDERING_MODE mode,
                          DWRITE_MEASURING_MODE mm)
{
    unsigned h = (unsigned)((size_t)run->fontFace >> 4) * 31u + (unsigned)(run->fontEmSize * 64.f);
    glyph_t **bucket, *g;
    IDWriteGlyphRunAnalysis *an;
    DWRITE_GLYPH_RUN one = *run;
    FLOAT advance = 0;
    DWRITE_GLYPH_OFFSET offset = {0, 0};
    RECT b;
    UINT32 n;
    float level;

    h = (h * 31u + index) * 31u + (unsigned)phase;
    bucket = &g_glyphs[(h ^ h >> 11) & (GLYPH_BUCKETS - 1)];
    for (g = *bucket; g; g = g->next)
        if (g->face == run->fontFace && g->em == run->fontEmSize && g->index == index && g->phase == phase &&
            g->mode == (BYTE)mode && g->mm == (BYTE)mm)
            return g;

    one.glyphCount = 1;
    one.glyphIndices = &index;
    one.glyphAdvances = &advance;
    one.glyphOffsets = &offset;
    if (FAILED(g_dw->CreateGlyphRunAnalysis(&one, 1, NULL, mode, mm, (float)(phase & 7) / 6.f, (float)(phase >> 3) / 4.f,
                                            &an)))
        return NULL;
    if (FAILED(an->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &b))) {
        an->Release();
        return NULL;
    }
    if (b.right < b.left || b.bottom < b.top)
        b.right = b.left = b.bottom = b.top = 0;
    n = (UINT32)((b.right - b.left) * (b.bottom - b.top) * 3);
    g = (glyph_t *)mem_alloc(sizeof(glyph_t) + n);
    if (n && FAILED(an->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &b, g->cov, n))) {
        an->Release();
        mem_free(g);
        return NULL;
    }
    an->Release();
    /* Below a ClearType level of 1, the colored fringes give way to gray. */
    level = g_params->GetClearTypeLevel();
    for (UINT32 i = 0; n && level < 1.f && i < n; i += 3) {
        float gray = ((float)g->cov[i] + (float)g->cov[i + 1] + (float)g->cov[i + 2]) / 3.f;
        for (int c = 0; c < 3; c++)
            g->cov[i + c] = (BYTE)(gray + ((float)g->cov[i + c] - gray) * level + .5f);
    }
    if (g_ct_bgr)
        for (UINT32 i = 0; i < n; i += 3) {
            BYTE t = g->cov[i];
            g->cov[i] = g->cov[i + 2];
            g->cov[i + 2] = t;
        }
    g->face = run->fontFace;
    g->face->AddRef();
    g->em = run->fontEmSize;
    g->index = index;
    g->phase = (BYTE)phase;
    g->mode = (BYTE)mode;
    g->mm = (BYTE)mm;
    g->left = b.left;
    g->top = b.top;
    g->w = b.right - b.left;
    g->h = b.bottom - b.top;
    g->next = *bucket;
    *bucket = g;
    g_glyph_bytes += n;
    return g;
}

/* Coverage (0 to 255) to the weight (0 to 256) of the text color, for the color of the last table made. */
static unsigned g_weights_for = 0xFF000000u; /* rgb of the table, alpha bits set: never a text's */
static unsigned short g_weights[256];

static void make_weights(unsigned argb)
{
    float light = ((float)(argb >> 16 & 0xFF) * .30f + (float)(argb >> 8 & 0xFF) * .59f + (float)(argb & 0xFF) * .11f) / 255.f;
    float k = g_contrast * clamp01(4.f * (.75f - light));

    for (int v = 0; v < 256; v++) {
        float a = (float)v / 255.f;
        g_weights[v] = (unsigned short)(a * (k + 1.f) / (a * k + 1.f) * 256.f + .5f);
    }
    g_weights_for = argb;
}

/*
 * A run of plain glyphs drawn from the cache at (x, y) (y in the band), in
 * argb: their coverage added up as DirectWrite rasterizes a run whole, then
 * blended once, inside c->vis. Returns 0 when the run is for DirectWrite.
 */
static int cached_run(draw_ctx *c, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mm, const DWRITE_GLYPH_RUN *run, unsigned argb)
{
    enum { MAX_GLYPHS = 256 };
    static glyph_t *gl[MAX_GLYPHS]; /* static: no __chkstk without the C runtime; text is drawn on one thread */
    static int gx[MAX_GLYPHS], gy[MAX_GLYPHS];
    DWRITE_RENDERING_MODE mode;
    RECT box = {0x7FFFFFFF, 0x7FFFFFFF, -0x7FFFFFFF, -0x7FFFFFFF}, vis;
    FLOAT pen = x;
    int bw, bh;
    unsigned color = c->argb & 0xFF000000u | argb & 0xFFFFFF;

    if (g_aa != DWRITE_TEXT_ANTIALIAS_MODE_CLEARTYPE || run->isSideways || (run->bidiLevel & 1) ||
        run->glyphCount > MAX_GLYPHS || run->fontEmSize > GLYPH_MAX_EM || !ct_ready() ||
        FAILED(run->fontFace->GetRecommendedRenderingMode(run->fontEmSize, 1, mm, g_params, &mode)) ||
        mode == DWRITE_RENDERING_MODE_ALIASED || mode == DWRITE_RENDERING_MODE_OUTLINE)
        return 0;
    for (UINT32 i = 0; i < run->glyphCount; i++) {
        FLOAT px = pen + (run->glyphOffsets ? run->glyphOffsets[i].advanceOffset : 0);
        FLOAT py = y - (run->glyphOffsets ? run->glyphOffsets[i].ascenderOffset : 0);
        int ix = (int)(px * 6.f + 393216.5f) - 393216, iy = (int)(py * 4.f + 262144.5f) - 262144; /* nearest 1/6, 1/4 */

        if (!(gl[i] = glyph_get(run, run->glyphIndices[i], (ix + 393216) % 6 | (iy & 3) << 3, mode, mm)))
            return 0;
        gx[i] = (ix + 393216) / 6 - 65536 + gl[i]->left;
        gy[i] = (iy >> 2) + gl[i]->top + g_y0;
        if (gl[i]->w && gl[i]->h) {
            box.left = imin(box.left, gx[i]);
            box.top = imin(box.top, gy[i]);
            box.right = imax(box.right, gx[i] + gl[i]->w);
            box.bottom = imax(box.bottom, gy[i] + gl[i]->h);
        }
        pen += run->glyphAdvances[i];
    }
    vis = c->vis;
    if (!(color >> 24) || !intersect(&vis, box.left, box.top, box.right - box.left, box.bottom - box.top))
        return 1;
    /* The coverage of the visible part, glyphs added up (they only overlap at their edges). */
    bw = vis.right - vis.left;
    bh = vis.bottom - vis.top;
    if ((size_t)bw * bh * 3 > g_run_cov_n) {
        mem_free(g_run_cov);
        g_run_cov_n = (size_t)bw * bh * 3;
        g_run_cov = (BYTE *)mem_alloc(g_run_cov_n);
    }
    memset(g_run_cov, 0, (size_t)bw * bh * 3);
    for (UINT32 i = 0; i < run->glyphCount; i++) {
        const glyph_t *g = gl[i];
        int x0 = imax(gx[i], vis.left), x1 = imin(gx[i] + g->w, vis.right);
        int y0 = imax(gy[i], vis.top), y1 = imin(gy[i] + g->h, vis.bottom);
        for (int yy = y0; yy < y1; yy++) {
            const BYTE *s = g->cov + ((size_t)(yy - gy[i]) * g->w + (x0 - gx[i])) * 3;
            BYTE *d = g_run_cov + ((size_t)(yy - vis.top) * bw + (x0 - vis.left)) * 3;
            for (int k = 0; k < (x1 - x0) * 3; k++) {
                unsigned v = d[k] + s[k];
                d[k] = (BYTE)(v > 255 ? 255 : v);
            }
        }
    }
    if ((color & 0xFFFFFF) != g_weights_for)
        make_weights(color & 0xFFFFFF);
    {
        int fr = g_lin[color >> 16 & 0xFF], fg = g_lin[color >> 8 & 0xFF], fb = g_lin[color & 0xFF];
        unsigned alpha = color >> 24;
        for (int yy = vis.top; yy < vis.bottom; yy++) {
            const BYTE *s = g_run_cov + (size_t)(yy - vis.top) * bw * 3;
            UINT32 *p = row(yy) + vis.left;
            for (int xx = 0; xx < bw; xx++, s += 3, p++) {
                UINT32 d;
                int r, gg, b;
                if (!(s[0] | s[1] | s[2]))
                    continue;
                d = *p;
                r = g_lin[d >> 16 & 0xFF];
                gg = g_lin[d >> 8 & 0xFF];
                b = g_lin[d & 0xFF];
                r += (fr - r) * (int)g_weights[s[0]] >> 8;
                gg += (fg - gg) * (int)g_weights[s[1]] >> 8;
                b += (fb - b) * (int)g_weights[s[2]] >> 8;
                if (alpha >= 255) {
                    *p = (UINT32)g_unlin[r] << 16 | (UINT32)g_unlin[gg] << 8 | (UINT32)g_unlin[b];
                } else { /* translucent text: drawn opaque, then laid over what was there, as draw_layout does */
                    *p = d;
                    blend(p, (unsigned)g_unlin[r] << 16 | (unsigned)g_unlin[gg] << 8 | g_unlin[b], alpha);
                }
            }
            /* Already clipped and blended: draw_layout's undoing, if it comes, must leave these pixels be. */
            if (c->saved && yy >= c->ink.top && yy < c->ink.bottom) {
                int a0 = imax(vis.left, c->ink.left), a1 = imin(vis.right, c->ink.right);
                if (a1 > a0)
                    memcpy(c->saved + (size_t)(yy - c->ink.top) * (c->ink.right - c->ink.left) + (a0 - c->ink.left),
                           row(yy) + a0, (size_t)(a1 - a0) * 4);
            }
        }
    }
    return 1;
}

/* Receives the glyph runs of a layout and draws them into the band bitmap. */
struct TextRenderer : IDWriteTextRenderer {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWritePixelSnapping) || id == __uuidof(IDWriteTextRenderer)) {
            *out = this;
            return S_OK;
        }
        *out = NULL;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void *, BOOL *off) override
    {
        *off = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void *, DWRITE_MATRIX *m) override
    {
        m->m11 = m->m22 = 1;
        m->m12 = m->m21 = m->dx = m->dy = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void *, FLOAT *ppd) override
    {
        *ppd = 1;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void *ctx, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mm,
                                           const DWRITE_GLYPH_RUN *run, const DWRITE_GLYPH_RUN_DESCRIPTION *desc,
                                           IUnknown *effect) override
    {
        draw_ctx *c = (draw_ctx *)ctx;
        unsigned argb = c->pass == PASS_MONO ? 0xFFFFFFFFu : effect ? ((ColorEffect *)effect)->argb : c->argb;
        IDWriteColorGlyphRunEnumerator1 *layers = NULL;
        DWRITE_GLYPH_IMAGE_FORMATS formats =
            DWRITE_GLYPH_IMAGE_FORMATS_TRUETYPE | DWRITE_GLYPH_IMAGE_FORMATS_CFF | DWRITE_GLYPH_IMAGE_FORMATS_COLR;
        D2D1_POINT_2F origin = {x, y};
        DWRITE_TEXT_ANTIALIAS_MODE aa = g_aa;

        if (!g_dw4 || FAILED(g_dw4->TranslateColorGlyphRun(origin, run, desc, formats, mm, NULL, 0, &layers))) {
            /* Plain glyphs (DWRITE_E_NOCOLOR). */
            if (c->pass == PASS_COLOR)
                return S_OK;
            if (c->pass == PASS_ALL && cached_run(c, x, y, mm, run, argb))
                return S_OK;
            guard(c);
            return g_target->DrawGlyphRun(x, y, mm, run, g_params, gdi_color(argb), NULL);
        }
        if (c->pass != PASS_MONO) {
            /* Color font layers (emoji); ClearType fringes would tint them. */
            guard(c);
            set_aa(DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            for (;;) {
                BOOL more = FALSE;
                const DWRITE_COLOR_GLYPH_RUN1 *layer;
                if (FAILED(layers->MoveNext(&more)) || !more || FAILED(layers->GetCurrentRun(&layer)))
                    break;
                COLORREF col = layer->paletteIndex == 0xFFFF
                                   ? gdi_color(argb)
                                   : RGB((int)(layer->runColor.r * 255 + 0.5f), (int)(layer->runColor.g * 255 + 0.5f),
                                         (int)(layer->runColor.b * 255 + 0.5f));
                g_target->DrawGlyphRun(layer->baselineOriginX, layer->baselineOriginY, mm, &layer->glyphRun, g_params,
                                       col, NULL);
            }
            set_aa(aa);
        }
        layers->Release();
        return S_OK;
    }

    void line(void *ctx, FLOAT x, FLOAT y, FLOAT width, FLOAT offset, FLOAT thickness, IUnknown *effect)
    {
        draw_ctx *c = (draw_ctx *)ctx;
        unsigned argb = c->pass == PASS_MONO ? 0xFFFFFFFFu : effect ? ((ColorEffect *)effect)->argb : c->argb;
        int t = (int)(thickness + 0.5f);

        if (c->pass == PASS_COLOR)
            return;
        guard(c); /* drawn opaque, made translucent with the rest */
        r_fill((int)(x + 0.5f), (int)(y + offset + 0.5f) + g_y0, (int)(width + 0.5f), t > 0 ? t : 1, argb | 0xFF000000u);
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void *ctx, FLOAT x, FLOAT y, const DWRITE_UNDERLINE *u, IUnknown *effect) override
    {
        line(ctx, x, y, u->width, u->offset, u->thickness, effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void *ctx, FLOAT x, FLOAT y, const DWRITE_STRIKETHROUGH *s,
                                                IUnknown *effect) override
    {
        line(ctx, x, y, s->width, s->offset, s->thickness, effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void *ctx, FLOAT x, FLOAT y, IDWriteInlineObject *obj, BOOL sideways,
                                               BOOL rtl, IUnknown *effect) override
    {
        guard((draw_ctx *)ctx);
        return obj->Draw(ctx, this, x, y, sideways, rtl, effect);
    }
};

static TextRenderer *g_renderer;
static __declspec(align(16)) unsigned char g_renderer_mem[sizeof(TextRenderer)];

/* Rectangle the ink of layout `l` drawn at (x, y) can touch, in global coordinates. */
static RECT ink_rect(IDWriteTextLayout *l, int x, int y)
{
    DWRITE_OVERHANG_METRICS o;
    RECT r;

    l->GetOverhangMetrics(&o);
    r.left = x - (int)(o.left + 1.99f);
    r.top = y - (int)(o.top + 1.99f);
    r.right = x + (int)(l->GetMaxWidth() + o.right + 1.99f);
    r.bottom = y + (int)(l->GetMaxHeight() + o.bottom + 1.99f);
    return r;
}

static UINT32 *scratch(size_t n)
{
    if (n > g_scratch_n) {
        mem_free(g_scratch);
        g_scratch = (UINT32 *)mem_alloc(n * sizeof *g_scratch);
        g_scratch_n = n;
    }
    return g_scratch;
}

static void save_rect(const RECT *r, UINT32 *out)
{
    for (int y = r->top; y < r->bottom; y++, out += r->right - r->left)
        memcpy(out, row(y) + r->left, (size_t)(r->right - r->left) * 4);
}

/*
 * Draws a layout at (x, y). DirectWrite ignores clipping, so the pixels it may
 * touch outside the visible area (and the box, when given) are saved first and
 * put back after; the same copy gives translucent text.
 */
static void draw_layout(IDWriteTextLayout *l, int x, int y, const RECT *box, unsigned argb, int pass)
{
    draw_ctx ctx = {argb, pass};
    RECT vis = clip_rect(), ink = ink_rect(l, x, y), area = {0, g_y0, g_w, g_y0 + g_bh};
    unsigned a = argb >> 24;
    UINT32 *saved;
    int w;

    if (box && !intersect(&vis, box->left, box->top, box->right - box->left, box->bottom - box->top))
        return;
    if (!intersect(&area, ink.left, ink.top, ink.right - ink.left, ink.bottom - ink.top) ||
        !intersect(&ink, vis.left, vis.top, vis.right - vis.left, vis.bottom - vis.top))
        return; /* nothing visible */
    ink = area; /* now: what the drawing can touch, inside the band */
    ctx.vis = vis;
    ctx.ink = ink;
    ctx.guard = a < 255 || ink.left < vis.left || ink.top < vis.top || ink.right > vis.right || ink.bottom > vis.bottom;
    l->Draw(&ctx, g_renderer, (FLOAT)x, (FLOAT)(y - g_y0));
    if (!(saved = ctx.saved))
        return;
    w = ink.right - ink.left;
    for (int yy = ink.top; yy < ink.bottom; yy++) {
        UINT32 *p = row(yy) + ink.left, *s = saved + (size_t)(yy - ink.top) * w;
        int inside_y = yy >= vis.top && yy < vis.bottom;
        for (int xx = ink.left; xx < ink.right; xx++, p++, s++) {
            if (!inside_y || xx < vis.left || xx >= vis.right) {
                *p = *s;
            } else if (a < 255 && *p != *s) {
                UINT32 drawn = *p;
                *p = *s;
                blend(p, drawn, a);
            }
        }
    }
}

/* ---- Fonts ---- */

static r_font_t *make_font(const wchar_t *family, IDWriteFontCollection *coll, int px, int weight, int italic)
{
    r_font_t *f = (r_font_t *)mem_alloc(sizeof *f);

    if (FAILED(g_dw->CreateTextFormat(family, coll, (DWRITE_FONT_WEIGHT)weight,
                                      italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, (float)px, L"", &f->format))) {
        mem_free(f);
        return NULL;
    }
    g_dw->CreateEllipsisTrimmingSign(f->format, &f->ellipsis);
    return f;
}

extern "C" r_font_t *r_font(const wchar_t *family, int px, int weight, int italic)
{
    return make_font(family, NULL, px, weight, italic);
}

/* Registered once, kept for the life of the process. */
static IDWriteInMemoryFontFileLoader *g_mem_loader;

extern "C" r_font_t *r_font_data(const void *data, size_t n, int px, int weight)
{
    IDWriteFactory5 *f5 = NULL;
    IDWriteFontFile *file = NULL;
    IDWriteFontSetBuilder1 *builder = NULL;
    IDWriteFontSet *set = NULL;
    IDWriteFontCollection1 *coll = NULL;
    IDWriteFontFamily *family = NULL;
    IDWriteLocalizedStrings *names = NULL;
    WCHAR name[128];
    r_font_t *f = NULL;

    if (FAILED(g_dw->QueryInterface(__uuidof(IDWriteFactory5), (void **)&f5)))
        return NULL;
    if (!g_mem_loader && SUCCEEDED(f5->CreateInMemoryFontFileLoader(&g_mem_loader)))
        f5->RegisterFontFileLoader(g_mem_loader);
    if (g_mem_loader &&
        SUCCEEDED(g_mem_loader->CreateInMemoryFontFileReference(f5, data, (UINT32)n, NULL, &file)) &&
        SUCCEEDED(f5->CreateFontSetBuilder(&builder)) && SUCCEEDED(builder->AddFontFile(file)) &&
        SUCCEEDED(builder->CreateFontSet(&set)) && SUCCEEDED(f5->CreateFontCollectionFromFontSet(set, &coll)) &&
        coll->GetFontFamilyCount() > 0 && SUCCEEDED(coll->GetFontFamily(0, &family)) &&
        SUCCEEDED(family->GetFamilyNames(&names)) && SUCCEEDED(names->GetString(0, name, 128)))
        f = make_font(name, coll, px, weight, 0);
    if (names)
        names->Release();
    if (family)
        family->Release();
    if (coll)
        coll->Release();
    if (set)
        set->Release();
    if (builder)
        builder->Release();
    if (file)
        file->Release();
    f5->Release();
    return f;
}

static void layouts_drop(r_font_t *f);

extern "C" void r_font_free(r_font_t *f)
{
    if (!f)
        return;
    layouts_drop(f);
    if (f->ellipsis)
        f->ellipsis->Release();
    f->format->Release();
    mem_free(f);
}

/* ---- Text ---- */

/*
 * Layouts are the costly part of text: the same strings come back in every
 * band of a frame and in every frame, so the last ones built are kept, one
 * per slot of a small table indexed by a hash of what makes them.
 */
#define LAYOUTS 512 /* a power of two */

static struct {
    r_font_t *f;
    wchar_t *s;
    int len, w, h;
    unsigned flags, hash;
    IDWriteTextLayout *l;
} g_layouts[LAYOUTS];

static unsigned layout_hash(r_font_t *f, const wchar_t *s, int len, int w, int h, unsigned flags)
{
    unsigned x = 2166136261u;

    for (int i = 0; i < len; i++)
        x = (x ^ s[i]) * 16777619u;
    x = (x ^ (unsigned)(UINT_PTR)f) * 16777619u;
    x = (x ^ (unsigned)w) * 16777619u;
    x = (x ^ (unsigned)h) * 16777619u;
    return (x ^ flags) * 16777619u;
}

/* Forgets the layouts of font `f` (NULL: all of them). */
static void layouts_drop(r_font_t *f)
{
    for (int i = 0; i < LAYOUTS; i++)
        if (g_layouts[i].l && (!f || g_layouts[i].f == f)) {
            g_layouts[i].l->Release();
            mem_free(g_layouts[i].s);
            g_layouts[i].l = NULL;
            g_layouts[i].s = NULL;
        }
}

static IDWriteTextLayout *layout_new(r_font_t *f, const wchar_t *s, int len, int w, int h, unsigned flags);

/* The layout of `s`, from the table or new; the caller releases it. */
static IDWriteTextLayout *layout(r_font_t *f, const wchar_t *s, int len, int w, int h, unsigned flags)
{
    IDWriteTextLayout *l;
    unsigned hash;
    int k, same;

    if (len < 0)
        len = lstrlenW(s);
    hash = layout_hash(f, s, len, w, h, flags);
    k = (int)(hash & (LAYOUTS - 1));
    same = g_layouts[k].l && g_layouts[k].hash == hash && g_layouts[k].f == f && g_layouts[k].len == len &&
           g_layouts[k].w == w && g_layouts[k].h == h && g_layouts[k].flags == flags;
    for (int i = 0; same && i < len; i++)
        same = g_layouts[k].s[i] == s[i];
    if (same) {
        g_layouts[k].l->AddRef();
        return g_layouts[k].l;
    }
    if (!(l = layout_new(f, s, len, w, h, flags)))
        return NULL;
    if (g_layouts[k].l) {
        g_layouts[k].l->Release();
        mem_free(g_layouts[k].s);
    }
    g_layouts[k].s = (wchar_t *)mem_alloc(((size_t)len + 1) * sizeof(wchar_t));
    for (int i = 0; i < len; i++)
        g_layouts[k].s[i] = s[i];
    g_layouts[k].f = f;
    g_layouts[k].len = len;
    g_layouts[k].w = w;
    g_layouts[k].h = h;
    g_layouts[k].flags = flags;
    g_layouts[k].hash = hash;
    g_layouts[k].l = l;
    l->AddRef();
    return l;
}

#define R_PLAIN 0x40000000u /* internal: the font draws the emoji (an image's stand-in while it loads) */
#define EMOJI_EM 1.375f     /* Discord's emoji in text: 1.375em, the height of its line */

static void emoji_objects(IDWriteTextLayout *l, const wchar_t *s, int len, int size, int mode, const md_doc_t *doc,
                          int start);

static IDWriteTextLayout *layout_new(r_font_t *f, const wchar_t *s, int len, int w, int h, unsigned flags)
{
    IDWriteTextLayout *l;

    if (FAILED(g_dw->CreateTextLayout(s, (UINT32)len, f->format, (float)w, (float)h, &l)))
        return NULL;
    /* The interface's text: what the font would draw in color only, so arrows and "™" stay text. */
    if (g_emoji_image && !(flags & R_PLAIN))
        emoji_objects(l, s, len, (int)(f->format->GetFontSize() * EMOJI_EM + 0.5f), TWEMOJI_COLOR, NULL, 0);
    l->SetTextAlignment((flags & R_CENTER)  ? DWRITE_TEXT_ALIGNMENT_CENTER
                        : (flags & R_RIGHT) ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                            : DWRITE_TEXT_ALIGNMENT_LEADING);
    l->SetParagraphAlignment((flags & R_VCENTER) ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    l->SetWordWrapping((flags & R_WRAP) ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if ((flags & R_ELLIPSIS) && f->ellipsis) {
        DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        l->SetTrimming(&trim, f->ellipsis);
    }
    return l;
}

/* Cheap test before building a layout: the box, with room for overhangs above and below (and on the sides). */
static int box_visible(int x, int y, int w, int h)
{
    RECT r = clip_rect();

    return y - h < r.bottom && y + 2 * h > r.top && x - h < r.right && x + w + h > r.left;
}

extern "C" void r_text(r_font_t *f, unsigned argb, int x, int y, int w, int h, const wchar_t *s, int len, unsigned flags)
{
    IDWriteTextLayout *l;
    RECT box = {x, y, x + w, y + h};

    if (!f || w <= 0 || !box_visible(x, y, w, h) || !(l = layout(f, s, len, w, h, flags)))
        return;
    draw_layout(l, x, y, &box, argb, PASS_ALL);
    l->Release();
}

extern "C" int r_text_width(r_font_t *f, const wchar_t *s, int len)
{
    IDWriteTextLayout *l;
    DWRITE_TEXT_METRICS m;
    int w = 0;

    if (f && (l = layout(f, s, len, 100000, 1000, R_SINGLE))) {
        if (SUCCEEDED(l->GetMetrics(&m)))
            w = (int)(m.widthIncludingTrailingWhitespace + 0.99f);
        l->Release();
    }
    return w;
}

extern "C" int r_text_height(r_font_t *f, const wchar_t *s, int len, int width)
{
    IDWriteTextLayout *l;
    DWRITE_TEXT_METRICS m;
    int h = 0;

    if (f && width > 0 && (l = layout(f, s, len, width, 100000, R_WRAP))) {
        if (SUCCEEDED(l->GetMetrics(&m)))
            h = (int)(m.height + 0.99f);
        l->Release();
    }
    return h;
}

/* ---- Selection ---- */

/* The caret position nearest to (x, y) in l: before or after the character there. */
static int layout_pos(IDWriteTextLayout *l, float x, float y)
{
    BOOL trailing, inside;
    DWRITE_HIT_TEST_METRICS m;

    if (FAILED(l->HitTestPoint(x, y, &trailing, &inside, &m)))
        return 0;
    return (int)m.textPosition + (trailing ? (int)m.length : 0);
}

/* Fills the boxes covering [a, a + n) of l drawn at (x, y): one per line, or per run of a direction. */
static void mark_range(IDWriteTextLayout *l, int x, int y, int a, int n, unsigned argb)
{
    DWRITE_HIT_TEST_METRICS few[16], *boxes = few;
    UINT32 count = 0;
    HRESULT hr = l->HitTestTextRange((UINT32)a, (UINT32)n, 0, 0, few, 16, &count);

    if (hr == E_NOT_SUFFICIENT_BUFFER && count) {
        boxes = (DWRITE_HIT_TEST_METRICS *)mem_alloc(count * sizeof *boxes);
        hr = l->HitTestTextRange((UINT32)a, (UINT32)n, 0, 0, boxes, count, &count);
    }
    for (UINT32 k = 0; SUCCEEDED(hr) && k < count; k++) {
        int x0 = x + (int)(boxes[k].left + 0.5f), x1 = x + (int)(boxes[k].left + boxes[k].width + 0.5f);
        int y0 = y + (int)(boxes[k].top + 0.5f), y1 = y + (int)(boxes[k].top + boxes[k].height + 0.5f);
        r_fill(x0, y0, x1 - x0, y1 - y0, argb);
    }
    if (boxes != few)
        mem_free(boxes);
}

extern "C" int r_text_pos(r_font_t *f, const wchar_t *s, int len, int w, int h, unsigned flags, int x, int y)
{
    IDWriteTextLayout *l;
    int pos = 0;

    if (f && w > 0 && (l = layout(f, s, len, w, h, flags))) {
        pos = layout_pos(l, (float)x, (float)y);
        l->Release();
    }
    return pos;
}

extern "C" void r_text_mark(r_font_t *f, unsigned argb, int x, int y, int w, int h, const wchar_t *s, int len,
                            unsigned flags, int a, int b)
{
    IDWriteTextLayout *l;

    if (!f || w <= 0 || b <= a || !box_visible(x, y, w, h) || !(l = layout(f, s, len, w, h, flags)))
        return;
    mark_range(l, x, y, a, b - a, argb);
    l->Release();
}

/* ---- Styled display names ---- */

static unsigned mix(unsigned a, unsigned b, int pct) /* pct of b, 0xRRGGBB */
{
    unsigned out = 0;

    for (int s = 0; s < 24; s += 8) {
        int ca = (int)(a >> s & 0xFF), cb = (int)(b >> s & 0xFF);
        out |= (unsigned)(ca + (cb - ca) * pct / 100) << s;
    }
    return out;
}

/* Color at t (0..1) along evenly spaced stops. */
static unsigned stops_at(const unsigned *c, int n, float t)
{
    float pos;
    int i;

    if (n <= 1)
        return n ? c[0] : 0xFFFFFF;
    pos = clamp01(t) * (float)(n - 1);
    i = (int)pos;
    if (i >= n - 1)
        return c[n - 1];
    return lerp_color(c[i], c[i + 1], pos - (float)i) & 0xFFFFFF;
}

struct mask_t {
    RECT r;
    BYTE *cov;
};

/* Composites the mask shifted by (dx, dy) in `color` (or a gradient) at opacity `alpha`. */
static void composite(const mask_t *m, const RECT *vis, int dx, int dy, unsigned alpha, unsigned color,
                      const unsigned *grad, int ngrad, int gx0, int gx1, int vertical, int gy0, int gy1)
{
    int w = m->r.right - m->r.left;

    for (int yy = vis->top; yy < vis->bottom; yy++) {
        int sy = yy - dy;
        if (sy < m->r.top || sy >= m->r.bottom)
            continue;
        UINT32 *p = row(yy) + vis->left;
        for (int xx = vis->left; xx < vis->right; xx++, p++) {
            int sx = xx - dx;
            unsigned c, a;
            if (sx < m->r.left || sx >= m->r.right)
                continue;
            a = m->cov[(size_t)(sy - m->r.top) * w + (sx - m->r.left)];
            if (!a)
                continue;
            c = color;
            if (grad)
                c = vertical ? stops_at(grad, ngrad, (float)(yy - gy0) / (float)(gy1 > gy0 ? gy1 - gy0 : 1))
                             : stops_at(grad, ngrad, (float)(xx - gx0) / (float)(gx1 > gx0 ? gx1 - gx0 : 1));
            blend(p, c, a * alpha / 255);
        }
    }
}

extern "C" void r_text_styled(r_font_t *f, const unsigned *colors, int ncolors, int effect, int x, int y, int w, int h,
                              const wchar_t *s, int len, unsigned flags)
{
    IDWriteTextLayout *l;
    DWRITE_TEXT_METRICS tm;
    RECT box = {x, y, x + w, y + h}, vis = clip_rect(), area = {0, g_y0, g_w, g_y0 + g_bh};
    unsigned first = ncolors > 0 ? colors[0] & 0xFFFFFF : 0xFFFFFF;
    unsigned second = ncolors > 1 ? colors[1] & 0xFFFFFF : mix(first, 0x000000, 50);
    int margin = h / 6 + 2, tx0, tx1, ty0, ty1, mw;
    draw_ctx ctx = {0xFFFFFFFFu, PASS_MONO};
    mask_t m;
    UINT32 *saved;

    if (!f || w <= 0 || !box_visible(x, y, w, h) || !(l = layout(f, s, len, w, h, flags)))
        return;
    if (!intersect(&vis, box.left - margin, box.top - margin, w + 2 * margin, h + 2 * margin)) {
        l->Release();
        return;
    }
    /* Coverage mask: white glyphs on black, grayscale anti-aliasing, color glyphs left out. */
    m.r = ink_rect(l, x, y);
    if (!intersect(&area, m.r.left, m.r.top, m.r.right - m.r.left, m.r.bottom - m.r.top)) {
        l->Release();
        return;
    }
    m.r = area;
    mw = m.r.right - m.r.left;
    saved = scratch((size_t)mw * (size_t)(m.r.bottom - m.r.top) * 2);
    m.cov = (BYTE *)(saved + (size_t)mw * (size_t)(m.r.bottom - m.r.top));
    save_rect(&m.r, saved);
    for (int yy = m.r.top; yy < m.r.bottom; yy++)
        memset(row(yy) + m.r.left, 0, (size_t)mw * 4);
    set_aa(DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    l->Draw(&ctx, g_renderer, (FLOAT)x, (FLOAT)(y - g_y0));
    set_aa(DWRITE_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    for (int yy = m.r.top; yy < m.r.bottom; yy++) {
        UINT32 *p = row(yy) + m.r.left;
        BYTE *c = m.cov + (size_t)(yy - m.r.top) * mw;
        for (int xx = 0; xx < mw; xx++)
            c[xx] = (BYTE)(p[xx] >> 8 & 0xFF);
        memcpy(p, saved + (size_t)(yy - m.r.top) * mw, (size_t)mw * 4);
    }

    l->GetMetrics(&tm);
    tx0 = x + (int)tm.left;
    tx1 = x + (int)(tm.left + tm.width + 0.5f);
    ty0 = y + (int)tm.top;
    ty1 = y + (int)(tm.top + tm.height + 0.5f);
    {
        int px = (int)f->format->GetFontSize(), r1 = px / 10 > 1 ? px / 10 : 1, r2 = px / 20 > 1 ? px / 20 : 1;
        static const int dirs[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
        switch (effect) {
        case 3: /* neon: light text in a soft glow of the color */
            for (int i = 0; i < 8; i++)
                composite(&m, &vis, dirs[i][0] * r1, dirs[i][1] * r1, 0x30, first, NULL, 0, 0, 0, 0, 0, 0);
            for (int i = 0; i < 8; i++)
                composite(&m, &vis, dirs[i][0] * r2, dirs[i][1] * r2, 0x50, first, NULL, 0, 0, 0, 0, 0, 0);
            composite(&m, &vis, 0, 0, 255, mix(first, 0xFFFFFF, 65), NULL, 0, 0, 0, 0, 0, 0);
            break;
        case 4: { /* toon: dark outline, vertical gradient */
            unsigned v[2] = {mix(first, 0xFFFFFF, 25), first};
            int t = px / 14 > 1 ? px / 14 : 1;
            for (int i = 0; i < 8; i++)
                composite(&m, &vis, dirs[i][0] * t, dirs[i][1] * t, 255, mix(second, 0x000000, 70), NULL, 0, 0, 0, 0, 0, 0);
            composite(&m, &vis, 0, 0, 255, 0, v, 2, 0, 0, 1, ty0, ty1);
            break;
        }
        case 5: /* pop: offset colored shadow */
            composite(&m, &vis, px / 12, px / 12, 255, second, NULL, 0, 0, 0, 0, 0, 0);
            composite(&m, &vis, 0, 0, 255, first, NULL, 0, 0, 0, 0, 0, 0);
            break;
        case 2: case 6: case 7: case 8: /* gradients (glow, prism and gummy are animated in Discord) */
            if (ncolors > 1) {
                composite(&m, &vis, 0, 0, 255, 0, colors, ncolors, tx0, tx1, 0, 0, 0);
                break;
            }
            /* fall through */
        default:
            composite(&m, &vis, 0, 0, 255, first, NULL, 0, 0, 0, 0, 0, 0);
            break;
        }
    }
    /* Color glyphs (emoji) keep their own colors, on top. */
    draw_layout(l, x, y, &box, 0xFF000000u | first, PASS_COLOR);
    l->Release();
}

/* ---- Rich text ---- */

struct rich_block {
    IDWriteTextLayout *layout;
    int kind;
    int quoted;       /* drawn with the quote bar */
    int start, len;   /* range in the doc */
    int x, y, h;      /* text origin relative to the rich origin, block height */
};

static void emoji_glyph(void *ctx, FLOAT x, FLOAT y, int size, const wchar_t *s, int len);

/* An emoji inside a text layout: reserves a square and draws the image there. */
struct EmojiObject : IDWriteInlineObject {
    LONG refs;
    int size;
    const r_rich_style_t *st;
    char id[32];      /* a custom emoji's id */
    int unicode;      /* or a Unicode emoji's index in k_twemoji, plus one */
    wchar_t text[24]; /* its text, drawn by the font while the image loads */
    int len;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteInlineObject)) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = NULL;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG n = InterlockedDecrement(&refs);
        if (!n)
            mem_free(this);
        return (ULONG)n;
    }
    HRESULT STDMETHODCALLTYPE Draw(void *ctx, IDWriteTextRenderer *, FLOAT x, FLOAT y, BOOL, BOOL, IUnknown *) override
    {
        const draw_ctx *c = (const draw_ctx *)ctx;
        r_image_t *img;

        if (c->pass == PASS_MONO)
            return S_OK;
        if (unicode) {
            if (!g_emoji_image || !(img = g_emoji_image(unicode - 1, size))) {
                emoji_glyph(ctx, x, y, size, text, len);
                return S_OK;
            }
        } else if (!st || !st->emoji || !(img = st->emoji(id, size))) {
            return S_OK;
        }
        r_image(img, (int)(x + 0.5f), (int)(y + 0.5f) + g_y0, size, size, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS *m) override
    {
        m->width = (FLOAT)size;
        m->height = (FLOAT)size;
        m->baseline = (FLOAT)size * 0.8f;
        m->supportsSideways = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS *o) override
    {
        o->left = o->top = o->right = o->bottom = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION *before, DWRITE_BREAK_CONDITION *after) override
    {
        *before = *after = DWRITE_BREAK_CONDITION_NEUTRAL;
        return S_OK;
    }
};

/* Fonts for emoji whose image is not there yet, by size. */
static struct {
    int px;
    r_font_t *f;
} g_emoji_fonts[8];

/*
 * The font's emoji in an object's square while its image loads. Drawn
 * straight into the band: we are inside draw_layout(), whose save and
 * restore of the pixels around already cover this.
 */
static void emoji_glyph(void *ctx, FLOAT x, FLOAT y, int size, const wchar_t *s, int len)
{
    int px = size * 3 / 4 > 1 ? size * 3 / 4 : 1, k;
    IDWriteTextLayout *l;

    for (k = 0; k < (int)ARRAYSIZE(g_emoji_fonts) && g_emoji_fonts[k].f && g_emoji_fonts[k].px != px; k++)
        ;
    if (k == (int)ARRAYSIZE(g_emoji_fonts)) { /* full: the first goes */
        r_font_free(g_emoji_fonts[0].f);
        g_emoji_fonts[0].f = NULL;
        k = 0;
    }
    if (!g_emoji_fonts[k].f) {
        g_emoji_fonts[k].px = px;
        if (!(g_emoji_fonts[k].f = make_font(L"Segoe UI Emoji", NULL, px, DWRITE_FONT_WEIGHT_NORMAL, 0)))
            return;
    }
    if (!(l = layout(g_emoji_fonts[k].f, s, len, size, size, R_CENTER | R_VCENTER | R_SINGLE | R_PLAIN)))
        return;
    l->Draw(ctx, g_renderer, x, y);
    l->Release();
}

/*
 * Puts an emoji object, `size` px square, on each Unicode emoji of s[0, len)
 * in layout l, so that it shows Discord's image. In a message (doc given,
 * s starting at doc position `start`), not inside code, as Discord.
 */
static void emoji_objects(IDWriteTextLayout *l, const wchar_t *s, int len, int size, int mode, const md_doc_t *doc,
                          int start)
{
    for (int i = 0; i < len;) {
        int index, n = twemoji_match(s + i, len - i, mode, &index), code = 0;
        if (n <= 0) {
            i++;
            continue;
        }
        for (int k = 0; doc && k < doc->nspans; k++) {
            const md_span_t *sp = &doc->spans[k];
            code |= (sp->flags & (MD_CODE | MD_EMOJI)) && start + i < sp->start + sp->len && start + i + n > sp->start;
        }
        if (!code && n <= (int)ARRAYSIZE(EmojiObject::text)) {
            EmojiObject *e = new (place_t(), mem_alloc(sizeof(EmojiObject))) EmojiObject();
            DWRITE_TEXT_RANGE range = {(UINT32)i, (UINT32)n};
            e->refs = 1;
            e->size = size;
            e->unicode = index + 1;
            e->len = n;
            for (int k = 0; k < n; k++)
                e->text[k] = s[i + k];
            l->SetInlineObject(e, range);
            e->Release(); /* the layout holds it */
        }
        i += n;
    }
}

extern "C" void r_set_emoji_images(r_image_t *(*image)(int index, int px))
{
    g_emoji_image = image;
    layouts_drop(NULL); /* built without the images */
}

struct r_rich {
    const md_doc_t *doc;
    const r_rich_style_t *st;
    int width;
    rich_block *blocks;
    int nblocks;
    int height;
    int hover; /* the link underlined under the mouse, -1 for none */
    int sel_a, sel_b; /* the selected text, in the doc */
    ColorEffect *link, *mention, *muted;
};

static r_font_t *block_font(const r_rich_style_t *st, int kind)
{
    switch (kind) {
    case MD_CODEBLOCK: return st->mono;
    case MD_H1: return st->h1;
    case MD_H2: return st->h2;
    case MD_H3: return st->h3;
    case MD_SUBTEXT: return st->subtext;
    default: return st->body;
    }
}

static void style_range(r_rich_t *r, IDWriteTextLayout *l, const md_span_t *sp, int start, int len)
{
    DWRITE_TEXT_RANGE range;
    int a = sp->start > start ? sp->start : start;
    int b = sp->start + sp->len < start + len ? sp->start + sp->len : start + len;
    WCHAR family[64];

    if (b <= a)
        return;
    range.startPosition = (UINT32)(a - start);
    range.length = (UINT32)(b - a);
    if (sp->flags & MD_BOLD)
        l->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
    if (sp->flags & MD_ITALIC)
        l->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
    if (sp->flags & MD_UNDERLINE)
        l->SetUnderline(TRUE, range);
    if (sp->flags & MD_STRIKE)
        l->SetStrikethrough(TRUE, range);
    if ((sp->flags & MD_CODE) && SUCCEEDED(r->st->mono->format->GetFontFamilyName(family, 64))) {
        l->SetFontFamilyName(family, range);
        l->SetFontSize(r->st->mono->format->GetFontSize(), range);
    }
    if (sp->flags & MD_LINK)
        l->SetDrawingEffect(r->link, range);
    if (sp->flags & MD_MENTION) {
        l->SetDrawingEffect(r->mention, range);
        l->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, range);
    }
    if (sp->flags & MD_EDITED) {
        l->SetDrawingEffect(r->muted, range);
        l->SetFontSize(r->st->subtext->format->GetFontSize() * 0.85f, range);
    }
    if ((sp->flags & MD_EMOJI) && sp->link >= 0) {
        const char *id = md_link(r->doc, sp->link);
        EmojiObject *e = new (place_t(), mem_alloc(sizeof(EmojiObject))) EmojiObject();
        e->refs = 1;
        e->st = r->st;
        e->size = r->doc->jumbo ? r->st->jumbo_px : r->st->emoji_px;
        lstrcpynA(e->id, id, sizeof e->id);
        l->SetInlineObject(e, range);
        e->Release(); /* the layout holds it */
    }
}

extern "C" r_rich_t *r_rich_build(const md_doc_t *d, const r_rich_style_t *st, int width)
{
    r_rich_t *r = (r_rich_t *)mem_alloc(sizeof *r);
    int y = 0;

    r->doc = d;
    r->st = st;
    r->width = width;
    r->hover = -1;
    r->link = new_effect(st->link);
    r->mention = new_effect(st->mention);
    r->muted = new_effect(st->muted);
    r->blocks = (rich_block *)mem_alloc(((size_t)d->nblocks + 1) * sizeof *r->blocks);
    for (int i = 0; i < d->nblocks; i++) {
        const md_block_t *b = &d->blocks[i];
        rich_block *out = &r->blocks[r->nblocks];
        r_font_t *f = block_font(st, b->kind);
        /* Quoted blocks of any kind sit right of the quote bar; code blocks also inside their box. */
        int quote = b->kind == MD_QUOTE || b->quoted ? st->quote_indent : 0;
        int inset = quote + (b->kind == MD_CODEBLOCK ? st->code_pad : 0);
        int w = r->width - inset - (b->kind == MD_CODEBLOCK ? st->code_pad : 0);
        DWRITE_TEXT_METRICS m;

        if (!f || w <= 0 ||
            FAILED(g_dw->CreateTextLayout(d->text + b->start, (UINT32)b->len, f->format, (float)w, 100000.0f,
                                          &out->layout)))
            continue;
        out->layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        if (d->jumbo) {
            DWRITE_TEXT_RANGE all = {0, (UINT32)b->len};
            out->layout->SetFontSize((FLOAT)st->jumbo_px * 0.9f, all);
        }
        if (b->kind != MD_CODEBLOCK)
            for (int s = 0; s < d->nspans; s++)
                style_range(r, out->layout, &d->spans[s], b->start, b->len);
        if (b->kind != MD_CODEBLOCK && g_emoji_image) {
            /* Unicode emoji as Discord's images, the size of custom ones (in a heading, as much larger) */
            float body = st->body ? st->body->format->GetFontSize() : 0;
            int size = d->jumbo || body <= 0 ? (d->jumbo ? st->jumbo_px : st->emoji_px)
                                             : (int)(f->format->GetFontSize() * (float)st->emoji_px / body + 0.5f);
            emoji_objects(out->layout, d->text + b->start, b->len, size, TWEMOJI_ANY, d, b->start);
        }
        if (b->kind == MD_SUBTEXT) {
            DWRITE_TEXT_RANGE all = {0, (UINT32)b->len};
            out->layout->SetDrawingEffect(r->muted, all);
        }
        out->layout->GetMetrics(&m);
        /* Only the measured height matters from now on: keep overhang metrics tight. */
        out->layout->SetMaxHeight(m.height);
        if (i && (b->kind == MD_H1 || b->kind == MD_H2 || b->kind == MD_H3))
            y += st->block_gap * 2;
        out->kind = b->kind;
        out->quoted = b->kind == MD_QUOTE || b->quoted;
        out->start = b->start;
        out->len = b->len;
        out->x = inset;
        out->y = y + (b->kind == MD_CODEBLOCK ? st->code_pad : 0);
        out->h = (int)(m.height + 0.99f) + (b->kind == MD_CODEBLOCK ? 2 * st->code_pad : 0);
        y += out->h + st->block_gap;
        r->nblocks++;
    }
    r->height = y > 0 ? y - st->block_gap : 0;
    return r;
}

extern "C" int r_rich_height(r_rich_t *r)
{
    return r->height;
}

/* Calls fn for each box covering the spans with `flag` in block b. */
template <typename F>
static void span_boxes(r_rich_t *r, const rich_block *b, unsigned flag, F fn)
{
    DWRITE_HIT_TEST_METRICS boxes[16];

    for (int s = 0; s < r->doc->nspans; s++) {
        const md_span_t *sp = &r->doc->spans[s];
        int a = sp->start > b->start ? sp->start : b->start;
        int e = sp->start + sp->len < b->start + b->len ? sp->start + sp->len : b->start + b->len;
        UINT32 n = 0;

        if (!(sp->flags & flag) || e <= a)
            continue;
        if (FAILED(b->layout->HitTestTextRange((UINT32)(a - b->start), (UINT32)(e - a), 0, 0, boxes, 16, &n)))
            continue;
        for (UINT32 k = 0; k < n && k < 16; k++)
            fn(boxes[k], sp->flags);
    }
}

extern "C" void r_rich_draw(r_rich_t *r, int x, int y, int reveal_spoilers)
{
    const r_rich_style_t *st = r->st;
    int pad = st->code_pad + 4;

    for (int i = 0; i < r->nblocks; i++) {
        rich_block *b = &r->blocks[i];
        int bx = x + b->x, by = y + b->y;

        if (!r_visible(by - pad, b->h + 2 * pad))
            continue;
        if (b->quoted) {
            /* Down to the next quoted block, so a quote's blocks share one bar. */
            int bar_h = b->h;
            int top = b->kind == MD_CODEBLOCK ? by - st->code_pad : by;
            if (i + 1 < r->nblocks && r->blocks[i + 1].quoted)
                bar_h = r->blocks[i + 1].y - (r->blocks[i + 1].kind == MD_CODEBLOCK ? st->code_pad : 0) - (top - y);
            r_round(x, top, st->quote_indent / 4, bar_h, st->quote_indent / 8, st->quote_bar);
        }
        if (b->kind == MD_CODEBLOCK) {
            int left = b->quoted ? st->quote_indent : 0;
            r_round(x + left, by - st->code_pad, r->width - left, b->h, st->radius, st->code_bg);
        }
        if (b->kind != MD_CODEBLOCK) {
            /* Code spans get the code background, mentions a tinted chip. */
            span_boxes(r, b, MD_CODE | MD_MENTION, [&](const DWRITE_HIT_TEST_METRICS &m, unsigned flags) {
                r_round(bx + (int)m.left - 2, by + (int)m.top, (int)(m.width + 0.5f) + 4, (int)(m.height + 0.5f),
                        st->radius / 2, (flags & MD_MENTION) ? st->mention_bg : st->code_bg);
            });
        }
        {
            int a = r->sel_a > b->start ? r->sel_a : b->start;
            int e = r->sel_b < b->start + b->len ? r->sel_b : b->start + b->len;
            if (e > a)
                mark_range(b->layout, bx, by, a - b->start, e - a, st->select);
        }
        draw_layout(b->layout, bx, by, NULL, st->ink, PASS_ALL);
        if (!reveal_spoilers) {
            span_boxes(r, b, MD_SPOILER, [&](const DWRITE_HIT_TEST_METRICS &m, unsigned) {
                r_round(bx + (int)m.left - 1, by + (int)m.top, (int)(m.width + 0.5f) + 2, (int)(m.height + 0.5f),
                        st->radius / 2, st->spoiler);
            });
            /* Selected, a hidden spoiler shows the selection's color, its text still hidden (as Discord). */
            for (int s = 0; s < r->doc->nspans && r->sel_b > r->sel_a; s++) {
                const md_span_t *sp = &r->doc->spans[s];
                int a = sp->start, e = sp->start + sp->len;
                a = a > r->sel_a ? a : r->sel_a;
                a = a > b->start ? a : b->start;
                e = e < r->sel_b ? e : r->sel_b;
                e = e < b->start + b->len ? e : b->start + b->len;
                if ((sp->flags & MD_SPOILER) && e > a)
                    mark_range(b->layout, bx, by, a - b->start, e - a, st->select);
            }
        }
    }
}

extern "C" int r_rich_hit(r_rich_t *r, int x, int y, unsigned *flags, int *link)
{
    for (int i = 0; i < r->nblocks; i++) {
        rich_block *b = &r->blocks[i];
        BOOL trailing, inside;
        DWRITE_HIT_TEST_METRICS m;

        if (y < b->y || y >= b->y + b->h)
            continue;
        if (FAILED(b->layout->HitTestPoint((float)(x - b->x), (float)(y - b->y), &trailing, &inside, &m)) || !inside)
            return 0;
        for (int s = 0; s < r->doc->nspans; s++) {
            const md_span_t *sp = &r->doc->spans[s];
            int pos = (int)m.textPosition + b->start;
            if (pos >= sp->start && pos < sp->start + sp->len) {
                *flags = sp->flags;
                *link = sp->link;
                return 1;
            }
        }
        return 0;
    }
    return 0;
}

/* Turns the underline of every span of `link` on, or back to what its markdown says. */
static void underline_link(r_rich_t *r, int link, BOOL on)
{
    for (int i = 0; i < r->nblocks; i++) {
        rich_block *b = &r->blocks[i];
        for (int s = 0; s < r->doc->nspans && b->layout; s++) {
            const md_span_t *sp = &r->doc->spans[s];
            int a = sp->start > b->start ? sp->start : b->start;
            int e = sp->start + sp->len < b->start + b->len ? sp->start + sp->len : b->start + b->len;
            DWRITE_TEXT_RANGE range = {(UINT32)(a - b->start), (UINT32)(e - a)};
            if ((sp->flags & MD_LINK) && sp->link == link && e > a)
                b->layout->SetUnderline(on || (sp->flags & MD_UNDERLINE), range);
        }
    }
}

extern "C" void r_rich_hover(r_rich_t *r, int link)
{
    if (!r || r->hover == link)
        return;
    if (r->hover >= 0)
        underline_link(r, r->hover, FALSE);
    if (link >= 0)
        underline_link(r, link, TRUE);
    r->hover = link;
}

extern "C" int r_rich_pos(r_rich_t *r, int x, int y)
{
    rich_block *b;
    int i = 0;

    if (!r->nblocks)
        return 0;
    /* The block at y, or the next one down from a gap; the last one below them all. */
    while (i + 1 < r->nblocks && y >= r->blocks[i].y + r->blocks[i].h)
        i++;
    b = &r->blocks[i];
    return b->start + layout_pos(b->layout, (float)(x - b->x), (float)(y - b->y));
}

extern "C" int r_rich_over(r_rich_t *r, int x, int y)
{
    for (int i = 0; i < r->nblocks; i++) {
        rich_block *b = &r->blocks[i];
        BOOL trailing, inside;
        DWRITE_HIT_TEST_METRICS m;

        if (y >= b->y && y < b->y + b->h)
            return SUCCEEDED(b->layout->HitTestPoint((float)(x - b->x), (float)(y - b->y), &trailing, &inside, &m)) &&
                   inside;
    }
    return 0;
}

extern "C" void r_rich_select(r_rich_t *r, int a, int b)
{
    if (!r)
        return;
    r->sel_a = a;
    r->sel_b = b;
}

extern "C" void r_rich_free(r_rich_t *r)
{
    if (!r)
        return;
    for (int i = 0; i < r->nblocks; i++)
        if (r->blocks[i].layout)
            r->blocks[i].layout->Release();
    mem_free(r->blocks);
    r->link->Release();
    r->mention->Release();
    r->muted->Release();
    mem_free(r);
}

/* ---- Decoding ---- */

extern "C" int r_bitmap_to_png(HBITMAP bmp, const wchar_t *path)
{
    IWICImagingFactory *wic = NULL;
    IWICBitmap *src = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    UINT w = 0, h = 0;
    int ok = 0;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    /* Clipboard alpha is usually garbage (screenshots): drop it, like Discord's PNGs of pastes. */
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                   (void **)&wic)) &&
        SUCCEEDED(wic->CreateBitmapFromHBITMAP(bmp, NULL, WICBitmapIgnoreAlpha, &src)) &&
        SUCCEEDED(src->GetSize(&w, &h)) && w && h && SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, NULL, &enc)) &&
        SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, NULL)) &&
        SUCCEEDED(frame->Initialize(NULL)) && SUCCEEDED(frame->SetSize(w, h)) &&
        SUCCEEDED(frame->SetPixelFormat(&format)) && SUCCEEDED(frame->WriteSource(src, NULL)) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit()))
        ok = 1;
    if (frame)
        frame->Release();
    if (enc)
        enc->Release();
    if (stream)
        stream->Release();
    if (src)
        src->Release();
    if (wic)
        wic->Release();
    return ok;
}

/* Byte compare: no memcmp without the CRT. */
static int bytes_eq(const void *a, const char *b, size_t n)
{
    const BYTE *p = (const BYTE *)a;

    for (size_t i = 0; i < n; i++)
        if (p[i] != (BYTE)b[i])
            return 0;
    return 1;
}

/* Whether the bytes are an SVG document (after a BOM and blanks, an XML prolog or the <svg> tag). */
static int is_svg(const BYTE *p, size_t n)
{
    size_t i = 0;

    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
        i = 3;
    while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n'))
        i++;
    return n - i >= 5 && (bytes_eq(p + i, "<svg", 4) || bytes_eq(p + i, "<?xml", 5));
}

/* A number in SVG text at s[*k], moving past it (and the blanks or commas before it). */
static float svg_number(const char *s, size_t lim, size_t *k)
{
    float whole = 0, frac = 0, scale = 1;
    int neg = 0;

    while (*k < lim && (s[*k] == ' ' || s[*k] == ','))
        (*k)++;
    if (*k < lim && s[*k] == '-')
        neg = 1, (*k)++;
    while (*k < lim && s[*k] >= '0' && s[*k] <= '9')
        whole = whole * 10 + (float)(s[(*k)++] - '0');
    if (*k < lim && s[*k] == '.')
        for ((*k)++; *k < lim && s[*k] >= '0' && s[*k] <= '9'; (*k)++)
            frac = frac * 10 + (float)(s[*k] - '0'), scale *= 10;
    return (whole + frac / scale) * (neg ? -1.f : 1.f);
}

/* The size an SVG is drawn at: its width and height attributes, else its viewBox's; 0 if none says. */
static void svg_size(const BYTE *p, size_t n, float *w, float *h)
{
    const char *s = (const char *)p;
    size_t lim = n < 4096 ? n : 4096, start = 0;
    float vw = 0, vh = 0;

    *w = *h = 0;
    /* the attributes of the <svg> tag only */
    for (size_t i = 0; i + 4 < lim; i++)
        if (bytes_eq(s + i, "<svg", 4)) {
            start = i;
            break;
        }
    for (size_t i = start + 4; i + 9 < lim && s[i] != '>'; i++) {
        size_t k;
        if ((s[i - 1] == ' ' || s[i - 1] == '\n' || s[i - 1] == '\t') && bytes_eq(s + i, "width=\"", 7)) {
            k = i + 7;
            *w = svg_number(s, lim, &k);
        } else if ((s[i - 1] == ' ' || s[i - 1] == '\n' || s[i - 1] == '\t') && bytes_eq(s + i, "height=\"", 8)) {
            k = i + 8;
            *h = svg_number(s, lim, &k);
        } else if (bytes_eq(s + i, "viewBox=\"", 9)) {
            k = i + 9;
            svg_number(s, lim, &k);
            svg_number(s, lim, &k);
            vw = svg_number(s, lim, &k);
            vh = svg_number(s, lim, &k);
        }
    }
    if (*w <= 0 || *h <= 0) {
        *w = vw;
        *h = vh;
    }
}

/*
 * Direct2D's SVG reader ignores <style>: Illustrator's exports color their
 * shapes with classes (".cls-5{fill:#fff}") and would come out black. The
 * rules of the style sheet become attributes of the elements with those
 * classes ("fill=\"#fff\"", unless the element sets it itself), and the sheet
 * goes. Only class selectors are understood, which is all such art uses.
 */
#define SVG_RULES 256

typedef struct {
    const char *name, *decl; /* class name and its declarations, in the source */
    size_t name_n, decl_n;
} svg_rule_t;

typedef struct {
    char *p;
    size_t n, cap;
} svg_out_t;

static void svg_put(svg_out_t *o, const char *s, size_t n)
{
    if (o->n + n > o->cap)
        return; /* sized for the worst case; never happens */
    memcpy(o->p + o->n, s, n);
    o->n += n;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Whether the tag text [tag, end) already has attribute `name` (n bytes). */
static int tag_has(const char *tag, const char *end, const char *name, size_t n)
{
    for (const char *q = tag; q + n + 2 <= end; q++)
        if (is_space(q[0]) && bytes_eq(q + 1, name, n) && q[1 + n] == '=')
            return 1;
    return 0;
}

/* Returns a copy of the SVG with its class rules inlined (mem_free it), or NULL when it has no <style>. */
static char *svg_inline_styles(const char *in, size_t n, size_t *out_n)
{
    svg_rule_t *rules; /* on the heap: no __chkstk without the CRT */
    int nrules = 0;
    const char *st = NULL, *st_end = NULL, *body = NULL, *body_end = NULL;
    svg_out_t o;

    for (size_t i = 0; i + 7 < n; i++)
        if (bytes_eq(in + i, "<style", 6)) {
            st = in + i;
            break;
        }
    if (!st)
        return NULL;
    for (const char *q = st; q < in + n; q++)
        if (*q == '>') {
            body = q + 1;
            break;
        }
    for (const char *q = body; q && q + 8 <= in + n; q++)
        if (bytes_eq(q, "</style>", 8)) {
            body_end = q;
            st_end = q + 8;
            break;
        }
    if (!body || !st_end)
        return NULL;
    rules = (svg_rule_t *)mem_alloc(SVG_RULES * sizeof *rules);
    /* ".a,.b{decl}" rules */
    for (const char *q = body; q < body_end && nrules < SVG_RULES;) {
        const char *sel = q, *brace, *close;
        while (q < body_end && *q != '{')
            q++;
        brace = q;
        while (q < body_end && *q != '}')
            q++;
        close = q;
        if (q < body_end)
            q++;
        if (brace >= body_end)
            break;
        for (const char *c = sel; c < brace && nrules < SVG_RULES;) {
            const char *a, *b;
            while (c < brace && (is_space(*c) || *c == ','))
                c++;
            a = c;
            while (c < brace && *c != ',')
                c++;
            b = c;
            while (b > a && is_space(b[-1]))
                b--;
            if (b - a > 1 && *a == '.') {
                rules[nrules].name = a + 1;
                rules[nrules].name_n = (size_t)(b - a - 1);
                rules[nrules].decl = brace + 1;
                rules[nrules].decl_n = (size_t)(close - brace - 1);
                nrules++;
            }
        }
    }
    o.cap = n * 4 + 65536;
    o.p = (char *)mem_alloc(o.cap);
    o.n = 0;
    for (const char *q = in; q < in + n;) {
        const char *tag_end, *cls;
        if (q == st) { /* the sheet goes */
            q = st_end;
            continue;
        }
        if (*q != '<' || q + 1 >= in + n || q[1] == '/' || q[1] == '!' || q[1] == '?') {
            svg_put(&o, q, 1);
            q++;
            continue;
        }
        for (tag_end = q; tag_end < in + n && *tag_end != '>'; tag_end++)
            ;
        cls = NULL;
        for (const char *c = q; c + 8 <= tag_end; c++)
            if (is_space(c[0]) && bytes_eq(c + 1, "class=\"", 7)) {
                cls = c + 8;
                break;
            }
        /* The tag up to its end, then the declarations of its classes as attributes. */
        {
            const char *close = tag_end > q && tag_end[-1] == '/' ? tag_end - 1 : tag_end;
            svg_put(&o, q, (size_t)(close - q));
            for (const char *c = cls; c && c < tag_end && *c != '"';) {
                const char *a;
                while (c < tag_end && is_space(*c))
                    c++;
                a = c;
                while (c < tag_end && *c != '"' && !is_space(*c))
                    c++;
                for (int r = 0; r < nrules; r++) {
                    const char *d, *de;
                    if (rules[r].name_n != (size_t)(c - a) || !bytes_eq(a, rules[r].name, rules[r].name_n))
                        continue;
                    d = rules[r].decl;
                    de = d + rules[r].decl_n;
                    while (d < de) {
                        const char *k = d, *colon, *v, *ve;
                        while (d < de && *d != ';')
                            d++;
                        for (colon = k; colon < d && *colon != ':'; colon++)
                            ;
                        if (colon < d) {
                            const char *kb = k, *ke = colon;
                            while (kb < ke && is_space(*kb))
                                kb++;
                            while (ke > kb && is_space(ke[-1]))
                                ke--;
                            v = colon + 1;
                            ve = d;
                            while (v < ve && is_space(*v))
                                v++;
                            while (ve > v && is_space(ve[-1]))
                                ve--;
                            if (ke > kb && ve > v && !tag_has(q, tag_end, kb, (size_t)(ke - kb))) {
                                svg_put(&o, " ", 1);
                                svg_put(&o, kb, (size_t)(ke - kb));
                                svg_put(&o, "=\"", 2);
                                svg_put(&o, v, (size_t)(ve - v));
                                svg_put(&o, "\"", 1);
                            }
                        }
                        if (d < de)
                            d++;
                    }
                }
            }
            svg_put(&o, close, (size_t)(tag_end - close) + (tag_end < in + n));
        }
        q = tag_end + (tag_end < in + n);
    }
    mem_free(rules);
    *out_n = o.n;
    return o.p;
}

static void svg_target_close(svg_target_t *t)
{
    if (t->dc)
        t->dc->Release();
    if (t->rt)
        t->rt->Release();
    if (t->bmp)
        t->bmp->Release();
    if (t->wic)
        t->wic->Release();
    if (t->f)
        t->f->Release();
    memset(t, 0, sizeof *t);
}

static int svg_target_open(svg_target_t *t, UINT w, UINT h)
{
    D2D1_RENDER_TARGET_PROPERTIES props = {D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                           {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96, 96,
                                           D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT};

    memset(t, 0, sizeof *t);
    t->w = w;
    t->h = h;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), NULL, (void **)&t->f)) &&
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                   (void **)&t->wic)) &&
        SUCCEEDED(t->wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &t->bmp)) &&
        SUCCEEDED(t->f->CreateWicBitmapRenderTarget(t->bmp, &props, &t->rt)) &&
        SUCCEEDED(t->rt->QueryInterface(__uuidof(ID2D1DeviceContext5), (void **)&t->dc)))
        return 1;
    svg_target_close(t);
    return 0;
}

/* Draws an SVG of size sw x sh scaled to the whole target, then copies the pixels into `out`. */
static int svg_draw(svg_target_t *t, const void *data, size_t n, float sw, float sh, BYTE *out)
{
    ID2D1SvgDocument *doc = NULL;
    IStream *st = NULL;
    HGLOBAL mem;
    int ok = 0;

    {
        size_t in_n;
        char *inl = svg_inline_styles((const char *)data, n, &in_n);
        if (inl) {
            data = inl;
            n = in_n;
        }
        mem = GlobalAlloc(GMEM_MOVEABLE, n);
        if (mem) {
            memcpy(GlobalLock(mem), data, n);
            GlobalUnlock(mem);
        }
        if (inl)
            mem_free(inl);
        if (!mem)
            return 0;
    }
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &st))) {
        GlobalFree(mem); /* the stream owns it once made */
        return 0;
    }
    if (SUCCEEDED(t->dc->CreateSvgDocument(st, D2D1_SIZE_F{sw, sh}, &doc))) {
        /* Drawn at its own size, scaled to ours: with or without a viewBox, it fills the bitmap. */
        D2D1_COLOR_F clear = {0, 0, 0, 0};
        D2D1_MATRIX_3X2_F scale = {(float)t->w / sw, 0, 0, (float)t->h / sh, 0, 0};
        D2D1_COLOR_F ink = {0xDC / 255.f, 0xDC / 255.f, 0xDF / 255.f, 1};
        ID2D1SvgElement *root = NULL;
        /* Art painted with currentColor takes the text color, as it would in Discord's dark theme. */
        doc->GetRoot(&root);
        if (root) {
            root->SetAttributeValue(L"color", D2D1_SVG_ATTRIBUTE_POD_TYPE_COLOR, &ink, sizeof ink);
            root->Release();
        }
        t->dc->BeginDraw();
        t->dc->Clear(&clear);
        t->dc->SetTransform(&scale);
        t->dc->DrawSvgDocument(doc);
        ok = SUCCEEDED(t->dc->EndDraw()) && SUCCEEDED(t->bmp->CopyPixels(NULL, t->w * 4, t->w * t->h * 4, out));
        doc->Release();
    }
    st->Release();
    return ok;
}

/*
 * An SVG (the art of Discord's pages): Direct2D draws it once, on the CPU,
 * into a bitmap of ours; its factory and target go right after, so nothing
 * of Direct2D stays around.
 */
static r_image_t *svg_decode(const void *data, size_t n, int max_px)
{
    svg_target_t t;
    r_image_t *img = NULL;
    float sw, sh;
    UINT w, h;

    svg_size((const BYTE *)data, n, &sw, &sh);
    if (sw <= 0 || sh <= 0)
        sw = sh = 256;
    if (max_px <= 0)
        max_px = 512;
    if (sw >= sh) {
        w = (UINT)max_px;
        h = (UINT)((float)max_px * sh / sw + .5f);
    } else {
        h = (UINT)max_px;
        w = (UINT)((float)max_px * sw / sh + .5f);
    }
    if (!w || !h || w > 4096 || h > 4096 || !svg_target_open(&t, w, h))
        return NULL;
    img = (r_image_t *)mem_alloc(sizeof *img);
    img->w = w;
    img->h = h;
    img->pixels = (BYTE *)mem_alloc((size_t)w * h * 4);
    if (svg_draw(&t, data, n, sw, sh, img->pixels)) {
        img->average = average_of(img);
    } else {
        mem_free(img->pixels);
        mem_free(img);
        img = NULL;
    }
    svg_target_close(&t);
    return img;
}

/*
 * A Lottie animation (animated stickers): its first frame now, the others
 * drawn from the JSON as they come due, at up to 30 frames a second.
 */
static r_image_t *lottie_decode(lottie_t *L, int max_px)
{
    sb_t svg = {0};
    r_image_t *img;
    double first, last, fps, step;

    lottie_info(L, &first, &last, &fps);
    step = fps > 30 ? fps / 30 : 1;
    lottie_svg(L, first, &svg);
    img = svg_decode(svg.data, svg.len, max_px);
    sb_free(&svg);
    if (!img || fps <= 0 || last - first < 2 * step || (last - first) / step >= 10000) {
        lottie_free(L); /* a still */
    } else {
        img->lottie = L;
        img->lottie_first = first;
        img->lottie_step = step;
        img->frames = (UINT)((last - first) / step);
        img->delay = (UINT)(1000 * step / fps + .5);
    }
    return img;
}

extern "C" r_image_t *r_image_decode(const void *data, size_t n, int max_px)
{
    lottie_t *lottie;
    IWICImagingFactory *wic = NULL;
    IWICStream *stream = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICBitmapSource *src = NULL;
    IWICFormatConverter *conv = NULL;
    r_image_t *img = NULL;
    UINT w = 0, h = 0;

    if (is_svg((const BYTE *)data, n))
        return svg_decode(data, n, max_px);
    if ((lottie = lottie_load((const char *)data, n)) != NULL)
        return lottie_decode(lottie, max_px);
    /* Worker threads call this: make sure COM is up (once per thread is enough). */
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                   (void **)&wic)) &&
        SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory((BYTE *)data, (DWORD)n)) &&
        SUCCEEDED(wic->CreateDecoderFromStream(stream, NULL, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w && h) {
        src = frame;
        /* Keep no more pixels than will ever be shown. */
        if (max_px > 0 && (w > (UINT)max_px || h > (UINT)max_px) &&
            SUCCEEDED(wic->CreateBitmapScaler(&scaler))) {
            UINT sw = w >= h ? (UINT)max_px : (UINT)((unsigned long long)w * (UINT)max_px / h);
            UINT sh = h >= w ? (UINT)max_px : (UINT)((unsigned long long)h * (UINT)max_px / w);
            if (SUCCEEDED(scaler->Initialize(frame, sw ? sw : 1, sh ? sh : 1, WICBitmapInterpolationModeFant)))
                src = scaler;
        }
    }
    if (src && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0,
                                   WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(conv->GetSize(&w, &h)) && w && h && w <= 4096 && h <= 4096) {
        img = (r_image_t *)mem_alloc(sizeof *img);
        img->w = w;
        img->h = h;
        img->pixels = (BYTE *)mem_alloc((size_t)w * h * 4);
        if (FAILED(conv->CopyPixels(NULL, w * 4, w * h * 4, img->pixels))) {
            mem_free(img->pixels);
            mem_free(img);
            img = NULL;
        } else {
            img->average = average_of(img);
            if (!adopt_animation(img, wic, data, n)) /* GIFs with several frames play */
                adopt_apng(img, data, n);            /* and so do animated PNGs */
        }
    }
    if (conv)
        conv->Release();
    if (scaler)
        scaler->Release();
    if (frame)
        frame->Release();
    if (dec)
        dec->Release();
    if (stream)
        stream->Release();
    if (wic)
        wic->Release();
    return img;
}

extern "C" r_image_t *r_image_blank(int w, int h)
{
    r_image_t *img;

    if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
        return NULL;
    img = (r_image_t *)mem_alloc(sizeof *img);
    img->w = (UINT)w;
    img->h = (UINT)h;
    img->pixels = (BYTE *)mem_alloc((size_t)w * (size_t)h * 4);
    return img;
}

extern "C" unsigned *r_image_bits(r_image_t *img)
{
    if (!img)
        return NULL;
    img->version++; /* written through this: the copies made from it are out of date */
    return (unsigned *)img->pixels;
}

extern "C" void r_image_size(const r_image_t *img, int *w, int *h)
{
    *w = img ? (int)img->w : 0;
    *h = img ? (int)img->h : 0;
}

extern "C" void r_image_free(r_image_t *img)
{
    if (!img)
        return;
    if (img->dec)
        img->dec->Release();
    if (img->stream)
        img->stream->Release();
    if (img->wic)
        img->wic->Release();
    apng_free(img->apng);
    lottie_free(img->lottie);
    if (img->target) {
        svg_target_close(img->target);
        mem_free(img->target);
    }
    mem_free(img->file);
    mem_free(img->saved);
    mem_free(img->pixels);
    for (int k = 0; k < 2; k++)
        mem_free(img->sized[k].pixels);
    mem_free(img);
}

/* ---- Setup ---- */

extern "C" int r_init(void)
{
    IDWriteRenderingParams *def = NULL;

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&g_dw)))
        return 0;
    g_dw->QueryInterface(__uuidof(IDWriteFactory4), (void **)&g_dw4);
    if (FAILED(g_dw->GetGdiInterop(&g_gdi)))
        return 0;
    /* The monitor's ClearType settings, with symmetric smoothing like Direct2D had. */
    if (SUCCEEDED(g_dw->CreateRenderingParams(&def))) {
        g_dw->CreateCustomRenderingParams(def->GetGamma(), def->GetEnhancedContrast(), def->GetClearTypeLevel(),
                                          def->GetPixelGeometry(), DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, &g_params);
        def->Release();
    }
    if (!g_params)
        return 0;
    g_renderer = new (place_t(), g_renderer_mem) TextRenderer();
    return ensure_target(1, BAND);
}
