/* Renderer: draws a scene in bands into a bitmap and checks pixels. */
#include <windows.h>
#include "test.h"
#include "render.h"

#define W 400
#define H 300 /* more than one band */

static UINT32 *g_px;

static unsigned px(int x, int y)
{
    return g_px[y * W + x] & 0xFFFFFF;
}

static int ch(unsigned c, int shift)
{
    return (int)(c >> shift & 0xFF);
}

/* Some pixel in the rectangle satisfies test(). */
static int any(int x0, int y0, int x1, int y1, int (*test)(unsigned))
{
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (test(px(x, y)))
                return 1;
    return 0;
}

static int all_bg(int x0, int y0, int x1, int y1)
{
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (px(x, y) != 0x101010)
                return 0;
    return 1;
}

static int reddish(unsigned c) { return ch(c, 16) > ch(c, 0) + 80; }
static int bluish(unsigned c) { return ch(c, 0) > ch(c, 16) + 80; }
static int white(unsigned c) { return ch(c, 16) > 200 && ch(c, 8) > 200 && ch(c, 0) > 200; }
static int colorful(unsigned c)
{
    int r = ch(c, 16), g = ch(c, 8), b = ch(c, 0);
    int hi = r > g ? (r > b ? r : b) : (g > b ? g : b), lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
    return hi - lo > 80;
}

void entry(void)
{
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), W, -H, 1, 32}};
    HDC dc = CreateCompatibleDC(NULL);
    void *bits = NULL;
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    static const unsigned grad[2] = {0xFF0000, 0x0000FF};
    r_font_t *big, *mid;
    int bands = 0;

    SelectObject(dc, bm);
    g_px = bits;
    check(r_init(), "init");
    big = r_font(L"Segoe UI", 40, FW_BOLD, 0);
    mid = r_font(L"Segoe UI", 32, FW_NORMAL, 0);
    check(big && mid, "fonts");

    while (r_begin(dc, W, H)) {
        bands++;
        r_fill(0, 0, W, H, 0xFF101010);
        /* A gradient display name, red to blue. */
        r_text_styled(big, grad, 2, 2, 10, 10, 380, 50, L"WWWWWWWWWW", -1, R_SINGLE | R_VCENTER);
        /* Text clipped to (0, 70, 200, 40). */
        r_clip(0, 70, 200, 40);
        r_text(mid, 0xFFFFFFFF, 10, 60, 380, 60, L"MMMMMMMMMMMMMMMMMM", -1, R_SINGLE | R_VCENTER);
        r_unclip();
        /* A green circle, an emoji and translucent white across the band edge. */
        r_circle(300, 120, 40, 0xFF00FF00);
        r_text(mid, 0xFFFFFFFF, 10, 130, 200, 50, L"\xD83C\xDF38", -1, R_SINGLE);
        r_fill(0, 240, W, 40, 0x80FFFFFF);
        r_end(dc);
    }
    GdiFlush();

    check(bands == 2, "frame drawn in two bands");
    check(any(10, 10, 120, 60, reddish), "gradient starts red");
    check(any(260, 10, 390, 60, bluish), "gradient ends blue");
    check(any(10, 70, 190, 110, white), "text inside the clip is drawn");
    check(all_bg(205, 60, 400, 120), "text right of the clip is not drawn");
    check(all_bg(0, 60, 200, 70) && all_bg(0, 110, 200, 120), "text above and below the clip is not drawn");
    check(px(320, 140) == 0x00FF00, "circle center is solid");
    check(px(301, 121) == 0x101010, "circle leaves the corner of its box");
    check(ch(px(320, 120), 8) > 0x10 && ch(px(320, 120), 8) < 0xFF, "circle edge is anti-aliased");
    check(any(10, 130, 60, 180, colorful), "emoji drawn in color");
    check(ch(px(5, 250), 16) > 0x80 && ch(px(5, 250), 16) < 0xA0 && ch(px(5, 270), 16) == ch(px(5, 250), 16),
          "translucent fill blends the same in both bands");
    finish();
}
