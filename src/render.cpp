/*
 * Direct2D / DirectWrite / WIC backend. C++ only because those headers are:
 * no exceptions, no RTTI, no C runtime, no static constructors.
 */
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include "render.h"

extern "C" {
#include "mem.h"
}

struct r_font {
    IDWriteTextFormat *format;
    IDWriteInlineObject *ellipsis;
};

struct r_image {
    UINT w, h;
    BYTE *pixels;          /* premultiplied BGRA */
    ID2D1Bitmap *bitmap;   /* created on first draw */
    unsigned generation;   /* render target the bitmap belongs to */
};

static ID2D1Factory *g_d2d;
static IDWriteFactory *g_dw;
static ID2D1DCRenderTarget *g_rt;
static ID2D1SolidColorBrush *g_brush;
static unsigned g_generation;

static D2D1_COLOR_F color(unsigned argb)
{
    D2D1_COLOR_F c;

    c.a = (float)((argb >> 24) & 0xFF) / 255.0f;
    c.r = (float)((argb >> 16) & 0xFF) / 255.0f;
    c.g = (float)((argb >> 8) & 0xFF) / 255.0f;
    c.b = (float)(argb & 0xFF) / 255.0f;
    return c;
}

static D2D1_RECT_F rectf(int x, int y, int w, int h)
{
    D2D1_RECT_F r = {(float)x, (float)y, (float)(x + w), (float)(y + h)};
    return r;
}

static ID2D1SolidColorBrush *brush(unsigned argb)
{
    D2D1_COLOR_F c = color(argb);
    g_brush->SetColor(&c);
    return g_brush;
}

static void release_target(void)
{
    if (g_brush)
        g_brush->Release();
    if (g_rt)
        g_rt->Release();
    g_brush = NULL;
    g_rt = NULL;
    g_generation++; /* cached bitmaps belong to the old target */
}

static int create_target(void)
{
    D2D1_RENDER_TARGET_PROPERTIES props = {};
    D2D1_COLOR_F black = {0, 0, 0, 1};

    props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
    if (FAILED(g_d2d->CreateDCRenderTarget(&props, &g_rt)))
        return 0;
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    return SUCCEEDED(g_rt->CreateSolidColorBrush(&black, NULL, &g_brush));
}

extern "C" int r_init(void)
{
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), NULL, (void **)&g_d2d)))
        return 0;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&g_dw)))
        return 0;
    return create_target();
}

extern "C" int r_begin(HDC dc, int w, int h)
{
    RECT rc = {0, 0, w, h};

    if (!g_rt && !create_target())
        return 0;
    if (FAILED(g_rt->BindDC(dc, &rc)))
        return 0;
    g_rt->BeginDraw();
    return 1;
}

extern "C" void r_end(void)
{
    if (g_rt && g_rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET)
        release_target();
}

extern "C" void r_fill(int x, int y, int w, int h, unsigned argb)
{
    D2D1_RECT_F r = rectf(x, y, w, h);
    g_rt->FillRectangle(&r, brush(argb));
}

static D2D1_ROUNDED_RECT rounded(int x, int y, int w, int h, int radius)
{
    D2D1_ROUNDED_RECT rr = {rectf(x, y, w, h), (float)radius, (float)radius};
    return rr;
}

extern "C" void r_round(int x, int y, int w, int h, int radius, unsigned argb)
{
    D2D1_ROUNDED_RECT rr = rounded(x, y, w, h, radius);
    g_rt->FillRoundedRectangle(&rr, brush(argb));
}

extern "C" void r_circle(int x, int y, int d, unsigned argb)
{
    D2D1_ELLIPSE e = {{(float)x + (float)d / 2, (float)y + (float)d / 2}, (float)d / 2, (float)d / 2};
    g_rt->FillEllipse(&e, brush(argb));
}

extern "C" void r_image(r_image_t *img, int x, int y, int w, int h, int radius)
{
    ID2D1BitmapBrush *bb;
    D2D1_MATRIX_3X2_F m;
    D2D1_ROUNDED_RECT rr;

    if (!img || !img->w || !img->h)
        return;
    if (img->bitmap && img->generation != g_generation) {
        img->bitmap->Release();
        img->bitmap = NULL;
    }
    if (!img->bitmap) {
        D2D1_BITMAP_PROPERTIES bp = {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96, 96};
        D2D1_SIZE_U size = {img->w, img->h};
        if (FAILED(g_rt->CreateBitmap(size, img->pixels, img->w * 4, &bp, &img->bitmap)))
            return;
        img->generation = g_generation;
    }
    if (FAILED(g_rt->CreateBitmapBrush(img->bitmap, &bb)))
        return;
    bb->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    m._11 = (float)w / (float)img->w;
    m._12 = 0;
    m._21 = 0;
    m._22 = (float)h / (float)img->h;
    m._31 = (float)x;
    m._32 = (float)y;
    bb->SetTransform(&m);
    rr = rounded(x, y, w, h, radius);
    g_rt->FillRoundedRectangle(&rr, bb);
    bb->Release();
}

extern "C" void r_clip(int x, int y, int w, int h)
{
    D2D1_RECT_F r = rectf(x, y, w, h);
    g_rt->PushAxisAlignedClip(&r, D2D1_ANTIALIAS_MODE_ALIASED);
}

extern "C" void r_unclip(void)
{
    g_rt->PopAxisAlignedClip();
}

/* ---- Text ---- */

extern "C" r_font_t *r_font(const wchar_t *family, int px, int weight, int italic)
{
    r_font_t *f = (r_font_t *)mem_alloc(sizeof *f);

    if (FAILED(g_dw->CreateTextFormat(family, NULL, (DWRITE_FONT_WEIGHT)weight,
                                      italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, (float)px, L"", &f->format))) {
        mem_free(f);
        return NULL;
    }
    g_dw->CreateEllipsisTrimmingSign(f->format, &f->ellipsis);
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

extern "C" void r_text(r_font_t *f, unsigned argb, int x, int y, int w, int h, const wchar_t *s, int len, unsigned flags)
{
    IDWriteTextLayout *l;
    D2D1_POINT_2F at = {(float)x, (float)y};

    if (!f || w <= 0 || !(l = layout(f, s, len, w, h, flags)))
        return;
    g_rt->DrawTextLayout(at, l, brush(argb), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP);
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

/* ---- Images ---- */

extern "C" r_image_t *r_image_decode(const void *data, size_t n)
{
    IWICImagingFactory *wic = NULL;
    IWICStream *stream = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
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
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0,
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
        }
    }
    if (conv)
        conv->Release();
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

extern "C" void r_image_free(r_image_t *img)
{
    if (!img)
        return;
    if (img->bitmap)
        img->bitmap->Release();
    mem_free(img->pixels);
    mem_free(img);
}
