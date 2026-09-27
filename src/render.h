#pragma once
#include <windows.h>
#include "md.h"

/*
 * Drawing through Direct2D (software rasterizer, no GPU driver loaded) and
 * DirectWrite: anti-aliased shapes, images, and text with color emoji.
 * Colors are 0xAARRGGBB. Everything except r_image_decode() runs on the UI thread.
 */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct r_font r_font_t;
typedef struct r_image r_image_t;

int r_init(void);
/* Starts a frame drawn onto `dc` (w x h pixels); r_end() presents it. */
int r_begin(HDC dc, int w, int h);
void r_end(void);

void r_fill(int x, int y, int w, int h, unsigned argb);
void r_round(int x, int y, int w, int h, int radius, unsigned argb);
void r_circle(int x, int y, int d, unsigned argb);
/* Draws `img` scaled into a w x h rounded rectangle (radius = w/2 for a circle). */
void r_image(r_image_t *img, int x, int y, int w, int h, int radius);
void r_clip(int x, int y, int w, int h);
void r_unclip(void);

r_font_t *r_font(const wchar_t *family, int px, int weight, int italic);
void r_font_free(r_font_t *f);

enum {
    R_LEFT = 0,
    R_CENTER = 1,
    R_RIGHT = 2,
    R_VCENTER = 4,
    R_SINGLE = 8,     /* one line, no wrapping */
    R_ELLIPSIS = 16,  /* trim with "..." when too long */
    R_WRAP = 32,      /* wrap at word boundaries */
};

void r_text(r_font_t *f, unsigned argb, int x, int y, int w, int h, const wchar_t *s, int len, unsigned flags);
int r_text_width(r_font_t *f, const wchar_t *s, int len);
int r_text_height(r_font_t *f, const wchar_t *s, int len, int width);

/* ---- Rich text (parsed markdown) ---- */

typedef struct {
    r_font_t *body, *mono, *h1, *h2, *h3, *subtext;
    unsigned ink, muted, link, mention, mention_bg, code_bg, quote_bar, spoiler;
    int quote_indent, code_pad, block_gap, radius;
} r_rich_style_t;

typedef struct r_rich r_rich_t;

/* Lays out `doc` at `width`. The doc and style must outlive the result. */
r_rich_t *r_rich_build(const md_doc_t *doc, const r_rich_style_t *style, int width);
int r_rich_height(r_rich_t *r);
void r_rich_draw(r_rich_t *r, int x, int y, int reveal_spoilers);
/* Styled span under (x, y), relative to the origin used to draw. Returns 0 over plain text. */
int r_rich_hit(r_rich_t *r, int x, int y, unsigned *flags, int *link);
void r_rich_free(r_rich_t *r);

/* PNG, JPEG, GIF, WebP... decoded to premultiplied BGRA. Safe from any thread. */
r_image_t *r_image_decode(const void *data, size_t n);
void r_image_free(r_image_t *img);

#ifdef __cplusplus
}
#endif
