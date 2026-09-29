/*
 * Software renderer: DirectWrite draws the text (color emoji included) into a
 * GDI bitmap we own, and everything else (anti-aliased shapes, scaled images)
 * is rasterized here. No Direct2D, so no WARP device and its caches.
 *
 * A frame is drawn in horizontal bands of BAND rows reusing one small bitmap:
 * the caller repeats its whole paint for each band and primitives outside it
 * are skipped. C++ only because the DirectWrite headers are: no exceptions,
 * no RTTI, no C runtime, no static constructors.
 */
#include <windows.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <emmintrin.h>
extern "C" {
#include "md.h"
}
#include "render.h"

extern "C" {
#include "apng.h"
#include "mem.h"
}

#define BAND 256
#define MAX_CLIP 16

/* Placement new without the C++ runtime. */
struct place_t {};
inline void *operator new(size_t, place_t, void *p) { return p; }
inline void operator delete(void *, place_t, void *) {}

struct r_font {
    IDWriteTextFormat *format;
    IDWriteInlineObject *ellipsis;
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
static RECT g_clip[MAX_CLIP];
static int g_nclip;
static UINT32 *g_scratch;           /* saved pixels while drawing text */
static size_t g_scratch_n;

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

/* Visible area: the band, then the clip stack. */
static RECT clip_rect(void)
{
    RECT r = {0, g_y0, g_w, g_y0 + g_bh};

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
        g_active = 1;
        g_y0 = 0;
        g_y_end = h;
        /* Only the bands the update region touches. */
        if (GetClipBox(dc, &clip) != ERROR && clip.bottom > clip.top) {
            g_y0 = clip.top / BAND * BAND;
            g_y_end = clip.bottom < h ? clip.bottom : h;
        }
    } else {
        g_y0 += BAND;
    }
    if (g_y0 >= g_y_end || g_y0 >= h || w <= 0 || !ensure_target(w, BAND)) {
        /* Frame done: the text scratch can reach a band's size, do not keep it between frames. */
        mem_free(g_scratch);
        g_scratch = NULL;
        g_scratch_n = 0;
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
    BitBlt(dc, 0, g_y0, g_w, g_bh, g_mem, 0, 0, SRCCOPY);
}

extern "C" int r_visible(int y, int h)
{
    RECT r = clip_rect();

    return y < r.bottom && y + h > r.top;
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
    for (int yy = r.top; yy < r.bottom; yy++) {
        UINT32 *p = row(yy) + r.left;
        if (a == 255)
            for (int xx = r.left; xx < r.right; xx++)
                *p++ = argb & 0xFFFFFF;
        else
            for (int xx = r.left; xx < r.right; xx++)
                blend(p++, argb, a);
    }
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
    float rad = (float)imin(radius, imin(w, h) / 2), s = (float)stroke;

    if (w <= 0 || h <= 0 || !intersect(&r, x, y, w, h))
        return;
    for (int yy = r.top; yy < r.bottom; yy++) {
        UINT32 *p = row(yy) + r.left;
        float py = (float)yy + 0.5f;
        unsigned c = top == bottom ? top : lerp_color(top, bottom, ((float)(yy - y) + 0.5f) / (float)h);
        unsigned a = c >> 24;
        /* Rows clear of the corners only need work near the left and right edges. */
        int plain = !stroke && py - y0 >= rad + 1 && y1 - py >= rad + 1;

        for (int xx = r.left; xx < r.right; xx++, p++) {
            float px = (float)xx + 0.5f, cov;
            if (plain && px - x0 >= 1 && x1 - px >= 1) {
                blend(p, c, a);
                continue;
            }
            cov = rr_cover(px, py, x0, y0, x1, y1, rad);
            if (stroke)
                cov -= rr_cover(px, py, x0 + s, y0 + s, x1 - s, y1 - s, rad - s > 0 ? rad - s : 0);
            if (cov > 0)
                blend(p, c, (unsigned)(cov * (float)a + 0.5f));
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

/* ---- Images ---- */

static inline void sample_bilinear(const r_image_t *img, float u, float v, unsigned out[4])
{
    int x0, y0, x1, y1;
    float fx, fy;

    u -= 0.5f;
    v -= 0.5f;
    if (u < 0) u = 0;
    if (v < 0) v = 0;
    x0 = (int)u;
    y0 = (int)v;
    if (x0 > (int)img->w - 1) x0 = (int)img->w - 1;
    if (y0 > (int)img->h - 1) y0 = (int)img->h - 1;
    x1 = imin(x0 + 1, (int)img->w - 1);
    y1 = imin(y0 + 1, (int)img->h - 1);
    fx = u - (float)x0;
    fy = v - (float)y0;
    if (fx > 1) fx = 1;
    if (fy > 1) fy = 1;
    for (int c = 0; c < 4; c++) {
        float a = img->pixels[((size_t)y0 * img->w + x0) * 4 + c], b = img->pixels[((size_t)y0 * img->w + x1) * 4 + c];
        float d = img->pixels[((size_t)y1 * img->w + x0) * 4 + c], e = img->pixels[((size_t)y1 * img->w + x1) * 4 + c];
        float top = a + (b - a) * fx, bot = d + (e - d) * fx;
        out[c] = (unsigned)(top + (bot - top) * fy + 0.5f);
    }
}

/* Average of the source pixels under a destination pixel, when shrinking. */
static inline void sample_box(const r_image_t *img, float u0, float v0, float u1, float v1, unsigned out[4])
{
    int xa = (int)u0, ya = (int)v0, xb = (int)(u1 + 0.999f), yb = (int)(v1 + 0.999f), n = 0;
    unsigned sum[4] = {0, 0, 0, 0};

    xa = imax(xa, 0);
    ya = imax(ya, 0);
    xb = imin(imax(xb, xa + 1), (int)img->w);
    yb = imin(imax(yb, ya + 1), (int)img->h);
    for (int yy = ya; yy < yb; yy++)
        for (int xx = xa; xx < xb; xx++, n++) {
            const BYTE *p = img->pixels + ((size_t)yy * img->w + xx) * 4;
            sum[0] += p[0];
            sum[1] += p[1];
            sum[2] += p[2];
            sum[3] += p[3];
        }
    for (int c = 0; c < 4; c++)
        out[c] = n ? (sum[c] + (unsigned)n / 2) / (unsigned)n : 0;
}

/* Draws the source rectangle (su, sv, sw, sh) of img into (x, y, w, h), rounded by radius. */
static void draw_image(const r_image_t *img, float su, float sv, float sw, float sh, int x, int y, int w, int h, int radius)
{
    RECT r = clip_rect();
    float kx = sw / (float)w, ky = sh / (float)h, rad = (float)imin(radius, imin(w, h) / 2);
    int shrink = kx > 1.5f || ky > 1.5f;

    if (!img || !img->pixels || w <= 0 || h <= 0 || !intersect(&r, x, y, w, h))
        return;
    /* Drawn whole at its own size: each pixel is a source pixel, which is what filtering would give. */
    int exact = su == 0 && sv == 0 && sw == (float)w && sh == (float)h && img->w == (UINT)w && img->h == (UINT)h;
    for (int yy = r.top; yy < r.bottom; yy++) {
        UINT32 *p = row(yy) + r.left;
        float py = (float)yy + 0.5f, v = sv + (py - (float)y) * ky;
        const BYTE *src = exact ? img->pixels + ((size_t)(yy - y) * img->w + (size_t)(r.left - x)) * 4 : NULL;
        /* Rows clear of the corners are covered whole, as rr_cover() would find. */
        int plain = rad <= 0 || (py - (float)y >= rad + 1 && (float)(y + h) - py >= rad + 1);

        for (int xx = r.left; xx < r.right; xx++, p++) {
            float px = (float)xx + 0.5f, u = su + (px - (float)x) * kx, cov = 1;
            unsigned s[4], a, d, out = 0;

            if (!plain && (cov = rr_cover(px, py, (float)x, (float)y, (float)(x + w), (float)(y + h), rad)) <= 0)
                continue;
            if (exact) {
                const BYTE *q = src + (size_t)(xx - r.left) * 4;
                s[0] = q[0];
                s[1] = q[1];
                s[2] = q[2];
                s[3] = q[3];
            } else if (shrink)
                sample_box(img, u - kx / 2, v - ky / 2, u + kx / 2, v + ky / 2, s);
            else
                sample_bilinear(img, u, v, s);
            /* Premultiplied source over the opaque destination. */
            a = (unsigned)((float)s[3] * cov + 0.5f);
            if (!a)
                continue;
            d = *p;
            for (int c = 0; c < 3; c++) {
                unsigned sc = (unsigned)((float)s[c] * cov + 0.5f), dc = d >> (c * 8) & 0xFF;
                unsigned v2 = sc + dc * (255 - a) / 255;
                out |= (v2 > 255 ? 255 : v2) << (c * 8);
            }
            *p = out;
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

/* Composes frame `index` over the canvas, applying the previous frame's disposal first. */
static int compose(r_image_t *img, UINT index)
{
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

extern "C" int r_image_lost(const r_image_t *img)
{
    (void)img;
    return 0; /* images are drawn from their own pixels */
}

extern "C" size_t r_image_bytes(const r_image_t *img)
{
    return img ? sizeof *img + (size_t)img->w * img->h * 4 * (img->saved ? 2 : 1) + img->file_n +
                     (img->apng ? apng_bytes(img->apng) : 0)
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
        const draw_ctx *c = (const draw_ctx *)ctx;
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
            return g_target->DrawGlyphRun(x, y, mm, run, g_params, gdi_color(argb), NULL);
        }
        if (c->pass != PASS_MONO) {
            /* Color font layers (emoji); ClearType fringes would tint them. */
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
        const draw_ctx *c = (const draw_ctx *)ctx;
        unsigned argb = c->pass == PASS_MONO ? 0xFFFFFFFFu : effect ? ((ColorEffect *)effect)->argb : c->argb;
        int t = (int)(thickness + 0.5f);

        if (c->pass == PASS_COLOR)
            return;
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
    if (a >= 255 && ink.left >= vis.left && ink.top >= vis.top && ink.right <= vis.right && ink.bottom <= vis.bottom) {
        l->Draw(&ctx, g_renderer, (FLOAT)x, (FLOAT)(y - g_y0));
        return;
    }
    w = ink.right - ink.left;
    saved = scratch((size_t)w * (size_t)(ink.bottom - ink.top));
    save_rect(&ink, saved);
    l->Draw(&ctx, g_renderer, (FLOAT)x, (FLOAT)(y - g_y0));
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

extern "C" void r_font_free(r_font_t *f)
{
    if (!f)
        return;
    if (f->ellipsis)
        f->ellipsis->Release();
    f->format->Release();
    mem_free(f);
}

/* ---- Text ---- */

static IDWriteTextLayout *layout(r_font_t *f, const wchar_t *s, int len, int w, int h, unsigned flags)
{
    IDWriteTextLayout *l;

    if (len < 0)
        len = lstrlenW(s);
    if (FAILED(g_dw->CreateTextLayout(s, (UINT32)len, f->format, (float)w, (float)h, &l)))
        return NULL;
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

/* Cheap test before building a layout: the box, with room for overhangs. */
static int box_visible(int y, int h)
{
    RECT r = clip_rect();

    return y - h < r.bottom && y + 2 * h > r.top;
}

extern "C" void r_text(r_font_t *f, unsigned argb, int x, int y, int w, int h, const wchar_t *s, int len, unsigned flags)
{
    IDWriteTextLayout *l;
    RECT box = {x, y, x + w, y + h};

    if (!f || w <= 0 || !box_visible(y, h) || !(l = layout(f, s, len, w, h, flags)))
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

    if (!f || w <= 0 || !box_visible(y, h) || !(l = layout(f, s, len, w, h, flags)))
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

/* A custom emoji inside a text layout: reserves a square and draws the image there. */
struct EmojiObject : IDWriteInlineObject {
    LONG refs;
    int size;
    const r_rich_style_t *st;
    char id[32];

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

        if (c->pass == PASS_MONO || !st->emoji || !(img = st->emoji(id, size)))
            return S_OK;
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

struct r_rich {
    const md_doc_t *doc;
    const r_rich_style_t *st;
    int width;
    rich_block *blocks;
    int nblocks;
    int height;
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
        draw_layout(b->layout, bx, by, NULL, st->ink, PASS_ALL);
        if (!reveal_spoilers)
            span_boxes(r, b, MD_SPOILER, [&](const DWRITE_HIT_TEST_METRICS &m, unsigned) {
                r_round(bx + (int)m.left - 1, by + (int)m.top, (int)(m.width + 0.5f) + 2, (int)(m.height + 0.5f),
                        st->radius / 2, st->spoiler);
            });
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

extern "C" r_image_t *r_image_decode(const void *data, size_t n, int max_px)
{
    IWICImagingFactory *wic = NULL;
    IWICStream *stream = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICBitmapSource *src = NULL;
    IWICFormatConverter *conv = NULL;
    r_image_t *img = NULL;
    UINT w = 0, h = 0;

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
    return img ? (unsigned *)img->pixels : NULL;
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
    mem_free(img->file);
    mem_free(img->saved);
    mem_free(img->pixels);
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
