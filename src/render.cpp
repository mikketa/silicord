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

/* ---- Rich text ---- */

struct rich_block {
    IDWriteTextLayout *layout;
    int kind;
    int start, len;   /* range in the doc */
    int x, y, h;      /* text origin relative to the rich origin, block height */
};

struct r_rich {
    const md_doc_t *doc;
    const r_rich_style_t *st;
    int width;
    unsigned generation;
    rich_block *blocks;
    int nblocks;
    int height;
    ID2D1SolidColorBrush *link, *mention, *muted;
};

static ID2D1SolidColorBrush *new_brush(unsigned argb)
{
    ID2D1SolidColorBrush *b = NULL;
    D2D1_COLOR_F c = color(argb);

    g_rt->CreateSolidColorBrush(&c, NULL, &b);
    return b;
}

static void release_rich(r_rich_t *r)
{
    for (int i = 0; i < r->nblocks; i++)
        if (r->blocks[i].layout)
            r->blocks[i].layout->Release();
    mem_free(r->blocks);
    r->blocks = NULL;
    r->nblocks = 0;
    if (r->link)
        r->link->Release();
    if (r->mention)
        r->mention->Release();
    if (r->muted)
        r->muted->Release();
    r->link = r->mention = r->muted = NULL;
}

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
}

static void build_rich(r_rich_t *r)
{
    const md_doc_t *d = r->doc;
    const r_rich_style_t *st = r->st;
    int y = 0;

    r->generation = g_generation;
    r->link = new_brush(st->link);
    r->mention = new_brush(st->mention);
    r->muted = new_brush(st->muted);
    r->blocks = (rich_block *)mem_alloc(((size_t)d->nblocks + 1) * sizeof *r->blocks);
    for (int i = 0; i < d->nblocks; i++) {
        const md_block_t *b = &d->blocks[i];
        rich_block *out = &r->blocks[r->nblocks];
        r_font_t *f = block_font(st, b->kind);
        int inset = b->kind == MD_QUOTE ? st->quote_indent : b->kind == MD_CODEBLOCK ? st->code_pad : 0;
        int w = r->width - inset - (b->kind == MD_CODEBLOCK ? st->code_pad : 0);
        DWRITE_TEXT_METRICS m;

        if (!f || w <= 0 ||
            FAILED(g_dw->CreateTextLayout(d->text + b->start, (UINT32)b->len, f->format, (float)w, 100000.0f,
                                          &out->layout)))
            continue;
        out->layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        if (b->kind != MD_CODEBLOCK)
            for (int s = 0; s < d->nspans; s++)
                style_range(r, out->layout, &d->spans[s], b->start, b->len);
        if (b->kind == MD_SUBTEXT) {
            DWRITE_TEXT_RANGE all = {0, (UINT32)b->len};
            out->layout->SetDrawingEffect(r->muted, all);
        }
        out->layout->GetMetrics(&m);
        if (i && (b->kind == MD_H1 || b->kind == MD_H2 || b->kind == MD_H3))
            y += st->block_gap * 2;
        out->kind = b->kind;
        out->start = b->start;
        out->len = b->len;
        out->x = inset;
        out->y = y + (b->kind == MD_CODEBLOCK ? st->code_pad : 0);
        out->h = (int)(m.height + 0.99f) + (b->kind == MD_CODEBLOCK ? 2 * st->code_pad : 0);
        y += out->h + st->block_gap;
        r->nblocks++;
    }
    r->height = y > 0 ? y - st->block_gap : 0;
}

extern "C" r_rich_t *r_rich_build(const md_doc_t *doc, const r_rich_style_t *style, int width)
{
    r_rich_t *r = (r_rich_t *)mem_alloc(sizeof *r);

    r->doc = doc;
    r->st = style;
    r->width = width;
    if (g_rt)
        build_rich(r);
    return r;
}

/* Layouts hold brushes of the render target they were built with. */
static void refresh(r_rich_t *r)
{
    if (r->generation != g_generation && g_rt) {
        release_rich(r);
        build_rich(r);
    }
}

extern "C" int r_rich_height(r_rich_t *r)
{
    refresh(r);
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

    refresh(r);
    for (int i = 0; i < r->nblocks; i++) {
        rich_block *b = &r->blocks[i];
        float bx = (float)(x + b->x), by = (float)(y + b->y);
        D2D1_POINT_2F at = {bx, by};

        if (b->kind == MD_QUOTE) {
            DWRITE_TEXT_METRICS m;
            b->layout->GetMetrics(&m);
            r_round(x, y + b->y, st->quote_indent / 4, (int)(m.height + 0.99f), st->quote_indent / 8, st->quote_bar);
        } else if (b->kind == MD_CODEBLOCK) {
            r_round(x, y + b->y - st->code_pad, r->width, b->h, st->radius, st->code_bg);
        }
        if (b->kind != MD_CODEBLOCK) {
            /* Code spans get the code background, mentions a tinted chip. */
            span_boxes(r, b, MD_CODE | MD_MENTION, [&](const DWRITE_HIT_TEST_METRICS &m, unsigned flags) {
                D2D1_ROUNDED_RECT rr = {{bx + m.left - 2, by + m.top, bx + m.left + m.width + 2, by + m.top + m.height},
                                        (float)st->radius / 2, (float)st->radius / 2};
                g_rt->FillRoundedRectangle(&rr, brush((flags & MD_MENTION) ? st->mention_bg : st->code_bg));
            });
        }
        g_rt->DrawTextLayout(at, b->layout, brush(st->ink), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        if (!reveal_spoilers)
            span_boxes(r, b, MD_SPOILER, [&](const DWRITE_HIT_TEST_METRICS &m, unsigned) {
                D2D1_ROUNDED_RECT rr = {{bx + m.left - 1, by + m.top, bx + m.left + m.width + 1, by + m.top + m.height},
                                        (float)st->radius / 2, (float)st->radius / 2};
                g_rt->FillRoundedRectangle(&rr, brush(st->spoiler));
            });
    }
}

extern "C" int r_rich_hit(r_rich_t *r, int x, int y, unsigned *flags, int *link)
{
    refresh(r);
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
    release_rich(r);
    mem_free(r);
}
