#include <windows.h>
#include <shlwapi.h>
#include "gfx.h"
#include "mem.h"

/* The GDI+ flat API headers are C++ only: declare the few entry points used here. */
typedef int GpStatus;
typedef void GpGraphics, GpBrush, GpPath, GpImage;
typedef float REAL;

typedef struct {
    UINT32 GdiplusVersion;
    void *DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} GdiplusStartupInput;

GpStatus WINAPI GdiplusStartup(ULONG_PTR *token, const GdiplusStartupInput *input, void *output);
GpStatus WINAPI GdipCreateFromHDC(HDC dc, GpGraphics **g);
GpStatus WINAPI GdipDeleteGraphics(GpGraphics *g);
GpStatus WINAPI GdipFlush(GpGraphics *g, int intention);
GpStatus WINAPI GdipSetSmoothingMode(GpGraphics *g, int mode);
GpStatus WINAPI GdipSetPixelOffsetMode(GpGraphics *g, int mode);
GpStatus WINAPI GdipSetInterpolationMode(GpGraphics *g, int mode);
GpStatus WINAPI GdipCreateSolidFill(unsigned argb, GpBrush **brush);
GpStatus WINAPI GdipCreateTexture(GpImage *img, int wrap, GpBrush **brush);
GpStatus WINAPI GdipScaleTextureTransform(GpBrush *brush, REAL sx, REAL sy, int order);
GpStatus WINAPI GdipTranslateTextureTransform(GpBrush *brush, REAL dx, REAL dy, int order);
GpStatus WINAPI GdipDeleteBrush(GpBrush *brush);
GpStatus WINAPI GdipFillEllipseI(GpGraphics *g, GpBrush *brush, INT x, INT y, INT w, INT h);
GpStatus WINAPI GdipFillRectangleI(GpGraphics *g, GpBrush *brush, INT x, INT y, INT w, INT h);
GpStatus WINAPI GdipCreatePath(int fill_mode, GpPath **path);
GpStatus WINAPI GdipAddPathArcI(GpPath *path, INT x, INT y, INT w, INT h, REAL start, REAL sweep);
GpStatus WINAPI GdipClosePathFigure(GpPath *path);
GpStatus WINAPI GdipFillPath(GpGraphics *g, GpBrush *brush, GpPath *path);
GpStatus WINAPI GdipDeletePath(GpPath *path);
GpStatus WINAPI GdipCreateBitmapFromStream(IStream *stream, GpImage **img);
GpStatus WINAPI GdipGetImageWidth(GpImage *img, UINT *w);
GpStatus WINAPI GdipGetImageHeight(GpImage *img, UINT *h);
GpStatus WINAPI GdipDisposeImage(GpImage *img);

enum {
    SMOOTHING_ANTIALIAS = 4,
    PIXEL_OFFSET_HALF = 4,
    INTERPOLATION_HQ_BICUBIC = 7,
    WRAP_CLAMP = 4,
    MATRIX_APPEND = 1,
    FLUSH_SYNC = 1,
};

struct gfx {
    GpGraphics *g;
};

struct gfx_image {
    GpImage *img;
    UINT w, h;
};

int gfx_init(void)
{
    GdiplusStartupInput in = {1, NULL, FALSE, FALSE};
    ULONG_PTR token;

    return GdiplusStartup(&token, &in, NULL) == 0;
}

gfx_t *gfx_begin(HDC dc)
{
    static gfx_t ctx; /* painting only happens on the UI thread */

    if (GdipCreateFromHDC(dc, &ctx.g) != 0)
        return NULL;
    GdipSetSmoothingMode(ctx.g, SMOOTHING_ANTIALIAS);
    GdipSetPixelOffsetMode(ctx.g, PIXEL_OFFSET_HALF);
    GdipSetInterpolationMode(ctx.g, INTERPOLATION_HQ_BICUBIC);
    return &ctx;
}

void gfx_flush(gfx_t *g)
{
    if (g)
        GdipFlush(g->g, FLUSH_SYNC);
}

void gfx_end(gfx_t *g)
{
    if (g)
        GdipDeleteGraphics(g->g);
}

static GpPath *round_path(int x, int y, int w, int h, int r)
{
    GpPath *p;
    int d = 2 * r;

    GdipCreatePath(0, &p);
    GdipAddPathArcI(p, x, y, d, d, 180.0f, 90.0f);
    GdipAddPathArcI(p, x + w - d, y, d, d, 270.0f, 90.0f);
    GdipAddPathArcI(p, x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    GdipAddPathArcI(p, x, y + h - d, d, d, 90.0f, 90.0f);
    GdipClosePathFigure(p);
    return p;
}

static void fill_shape(gfx_t *g, GpBrush *b, int x, int y, int w, int h, int r)
{
    if (r <= 0) {
        GdipFillRectangleI(g->g, b, x, y, w, h);
    } else if (2 * r >= w && w == h) {
        GdipFillEllipseI(g->g, b, x, y, w, h);
    } else {
        GpPath *p = round_path(x, y, w, h, r);
        GdipFillPath(g->g, b, p);
        GdipDeletePath(p);
    }
}

void gfx_round_rect(gfx_t *g, int x, int y, int w, int h, int radius, unsigned argb)
{
    GpBrush *b;

    if (!g || GdipCreateSolidFill(argb, &b) != 0)
        return;
    fill_shape(g, b, x, y, w, h, radius);
    GdipDeleteBrush(b);
}

void gfx_circle(gfx_t *g, int x, int y, int d, unsigned argb)
{
    gfx_round_rect(g, x, y, d, d, d / 2, argb);
}

void gfx_image(gfx_t *g, gfx_image_t *img, int x, int y, int w, int h, int radius)
{
    GpBrush *b;

    if (!g || !img || !img->w || !img->h || GdipCreateTexture(img->img, WRAP_CLAMP, &b) != 0)
        return;
    GdipScaleTextureTransform(b, (REAL)w / (REAL)img->w, (REAL)h / (REAL)img->h, MATRIX_APPEND);
    GdipTranslateTextureTransform(b, (REAL)x, (REAL)y, MATRIX_APPEND);
    fill_shape(g, b, x, y, w, h, radius);
    GdipDeleteBrush(b);
}

gfx_image_t *gfx_image_load(const void *data, size_t n)
{
    IStream *s = SHCreateMemStream((const BYTE *)data, (UINT)n);
    GpImage *img = NULL;
    gfx_image_t *out = NULL;

    if (!s)
        return NULL;
    if (GdipCreateBitmapFromStream(s, &img) == 0) {
        out = mem_alloc(sizeof *out);
        out->img = img;
        GdipGetImageWidth(img, &out->w);
        GdipGetImageHeight(img, &out->h);
    }
    /* GDI+ holds its own reference to the stream while the image lives. */
    s->lpVtbl->Release(s);
    return out;
}

void gfx_image_free(gfx_image_t *img)
{
    if (!img)
        return;
    GdipDisposeImage(img->img);
    mem_free(img);
}
