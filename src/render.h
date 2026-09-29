#pragma once
#include <windows.h>
#include "md.h"

/*
 * Software drawing: DirectWrite renders the text (with color emoji) into a
 * bitmap we own, and the shapes and images are rasterized by hand. Colors
 * are 0xAARRGGBB. Everything except r_image_decode() runs on the UI thread.
 */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct r_font r_font_t;
typedef struct r_image r_image_t;

int r_init(void);
/*
 * A frame is drawn in horizontal bands through one small bitmap, only over
 * the part of `dc` that needs painting (its clip box). Repeat the whole paint
 * for each band; primitives outside it cost next to nothing:
 *
 *     while (r_begin(dc, w, h)) { ...draw everything...; r_end(dc); }
 */
int r_begin(HDC dc, int w, int h);
void r_end(HDC dc);
/* Whether rows [y, y + h) can be seen in the current band and clip: lets callers skip work. */
int r_visible(int y, int h);

void r_fill(int x, int y, int w, int h, unsigned argb);
void r_round(int x, int y, int w, int h, int radius, unsigned argb);
void r_circle(int x, int y, int d, unsigned argb);
/* Draws `img` scaled into a w x h rounded rectangle (radius = w/2 for a circle). */
void r_image(r_image_t *img, int x, int y, int w, int h, int radius);
/*
 * Animated GIFs keep their compressed data and one composed frame; the first
 * frame shows until r_image_advance() moves on. It returns how many ms until
 * the next frame is due (0 for still images). `now` is GetTickCount().
 */
int r_image_animated(const r_image_t *img);
unsigned r_image_advance(r_image_t *img, unsigned now);
/* Index of the frame shown, to tell whether r_image_advance() moved. */
int r_image_frame(const r_image_t *img);
/* Union of the rectangles the image was drawn into since the last call (empty if none). */
RECT r_image_drawn(r_image_t *img);
/* Memory the image holds, in bytes. */
size_t r_image_bytes(const r_image_t *img);
/* Same, but scaled to cover the rectangle and cropped around the center. */
void r_image_cover(r_image_t *img, int x, int y, int w, int h, int radius);
/* Its alpha as a mask filled with `argb`, unscaled: for icons. */
void r_image_tint(r_image_t *img, int x, int y, unsigned argb);
/* Average color of the opaque pixels, 0xFFRRGGBB (0 if there are none), computed when decoding. */
unsigned r_image_average(r_image_t *img);
/* Rounded rectangle filled with a vertical gradient, and a rounded outline. */
void r_round_gradient(int x, int y, int w, int h, int radius, unsigned top, unsigned bottom);
void r_round_outline(int x, int y, int w, int h, int radius, int width, unsigned argb);
void r_clip(int x, int y, int w, int h);
void r_unclip(void);

r_font_t *r_font(const wchar_t *family, int px, int weight, int italic);
/* Font from a TTF/OTF file in memory (the data is copied). NULL if it cannot be read. */
r_font_t *r_font_data(const void *data, size_t n, int px, int weight);
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

/*
 * Text drawn with a Discord display name style: `effect` is
 * display_name_styles.effect_id (1 solid, 2 gradient, 3 neon, 4 toon, 5 pop,
 * 6 glow, 7 prism, 8 gummy) and `colors` its 0xRRGGBB colors.
 */
void r_text_styled(r_font_t *f, const unsigned *colors, int ncolors, int effect, int x, int y, int w, int h,
                   const wchar_t *s, int len, unsigned flags);

/* ---- Rich text (parsed markdown) ---- */

typedef struct {
    r_font_t *body, *mono, *h1, *h2, *h3, *subtext;
    unsigned ink, muted, link, mention, mention_bg, code_bg, quote_bar, spoiler;
    int quote_indent, code_pad, block_gap, radius;
    /* Custom emoji: size inline and in emoji-only messages, and where their images come from
       (`id` is the emoji id, prefixed by "a" when animated; NULL while it loads). */
    int emoji_px, jumbo_px;
    r_image_t *(*emoji)(const char *id, int px);
} r_rich_style_t;

typedef struct r_rich r_rich_t;

/* Lays out `doc` at `width`. The doc and style must outlive the result. */
r_rich_t *r_rich_build(const md_doc_t *doc, const r_rich_style_t *style, int width);
int r_rich_height(r_rich_t *r);
void r_rich_draw(r_rich_t *r, int x, int y, int reveal_spoilers);
/* Styled span under (x, y), relative to the origin used to draw. Returns 0 over plain text. */
int r_rich_hit(r_rich_t *r, int x, int y, unsigned *flags, int *link);
void r_rich_free(r_rich_t *r);

/*
 * PNG, JPEG, GIF, WebP... decoded to premultiplied BGRA, scaled down so that
 * neither side exceeds `max_px` (0 keeps the size). Safe from any thread.
 */
r_image_t *r_image_decode(const void *data, size_t n, int max_px);
/* Writes a bitmap (from the clipboard, say) to `path` as a PNG. Returns 0 on failure. */
int r_bitmap_to_png(HBITMAP bmp, const wchar_t *path);
void r_image_free(r_image_t *img);
/* An image to fill in: w x h pixels, 0xAARRGGBB premultiplied (video frames are opaque). */
r_image_t *r_image_blank(int w, int h);
unsigned *r_image_bits(r_image_t *img);
void r_image_size(const r_image_t *img, int *w, int *h);

#ifdef __cplusplus
}
#endif
