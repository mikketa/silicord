/*
 * Custom-drawn Win32 UI: server rail, channel list, main pane. Everything is
 * painted into one back buffer; the only child windows are none so far.
 * Runs on the UI thread only.
 */
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include "ui.h"
#include "gfx.h"
#include "img.h"
#include "mem.h"
#include "qr.h"
#include "utf.h"

/* Palette (GDI COLORREF and GDI+ ARGB). */
#define RGBX(r, g, b) RGB(r, g, b), (0xFF000000u | ((r) << 16) | ((g) << 8) | (b))
enum { C_RAIL, C_SIDE, C_MAIN, C_PANEL, C_ITEM, C_HOVER, C_SELECT, C_LINE, C_INK, C_MUTED, C_FAINT, C_AMBER, C_GREEN, C_TIP, C_COUNT };
static const struct { COLORREF gdi; unsigned argb; } k_color[C_COUNT] = {
    {RGBX(0x0A, 0x0A, 0x0A)}, /* rail */
    {RGBX(0x11, 0x11, 0x11)}, /* channel column */
    {RGBX(0x16, 0x16, 0x16)}, /* main pane */
    {RGBX(0x0D, 0x0D, 0x0D)}, /* user panel */
    {RGBX(0x1F, 0x1F, 0x1F)}, /* server icon placeholder */
    {RGBX(0x1B, 0x1B, 0x1B)}, /* hovered row */
    {RGBX(0x26, 0x26, 0x26)}, /* selected row */
    {RGBX(0x22, 0x22, 0x22)}, /* separators */
    {RGBX(0xED, 0xE6, 0xD6)}, /* text */
    {RGBX(0x9A, 0x94, 0x88)}, /* secondary text */
    {RGBX(0x5F, 0x5B, 0x54)}, /* icons, hints */
    {RGBX(0xFF, 0xB0, 0x00)}, /* accent */
    {RGBX(0x3B, 0xA5, 0x5D)}, /* online */
    {RGBX(0x05, 0x05, 0x05)}, /* tooltip */
};
#define GDI(c) (k_color[c].gdi)
#define ARGB(c) (k_color[c].argb)

/* Segoe MDL2 Assets glyphs. */
#define ICON_VOLUME L"\xE767"
#define ICON_CHEVRON_DOWN L"\xE70D"
#define ICON_CHEVRON_RIGHT L"\xE76C"
#define ICON_POWER L"\xE7E8"
#define ICON_REFRESH L"\xE72C"
#define ICON_CHECK L"\xE73E"

/* Layout constants at 96 DPI. */
#define RAIL_W 72
#define ICON 48
#define RAIL_STEP 56
#define SIDE_W 240
#define HEADER_H 48
#define PANEL_H 56
#define CAT_H 40
#define ROW_H 34

enum { VIEW_LOGIN, VIEW_LOADING, VIEW_APP };
enum { HIT_NONE, HIT_HOME, HIT_GUILD, HIT_CHANNEL, HIT_LOGOUT, HIT_RETRY };

typedef struct {
    char key[96];
    gfx_image_t *img;
    int failed;
} image_t;

typedef struct {
    HWND wnd;
    HDC back;
    HBITMAP back_bmp, back_old;
    int back_w, back_h;
    gfx_t *g;
    HFONT f_title, f_h, f_body, f_small, f_cat, f_icon, f_icon_big, f_initial, f_initial_small;
    HICON icon_big, icon_small;
    int dpi, view, disconnected;

    /* Login. */
    qr_t *qr;
    sb_t status, scanned;

    /* Session. */
    model_t *model;
    sb_t account;
    int guild, channel;        /* selection, -1 = none */
    int *last_channel;         /* per guild */
    unsigned char *collapsed;  /* per channel (categories) */
    int rail_scroll, side_scroll;
    int hover_kind, hover_index;
    image_t *images;
    int nimages, cap_images;
} ui_t;

static ui_t g_ui = {.guild = -1, .channel = -1};

static const unsigned char k_word[8][7] = {
    {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E}, {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E},
    {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E},
    {0x00, 0x00, 0x0F, 0x10, 0x10, 0x10, 0x0F}, {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E},
    {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}, {0x01, 0x01, 0x0F, 0x11, 0x11, 0x11, 0x0F},
};

static const unsigned short k_mark[16] = {
    0x0000, 0x0000, 0x1FF8, 0x2004, 0x4002, 0x4C02, 0x4C02, 0x4C02,
    0x4C02, 0x4002, 0x2004, 0x17F8, 0x1800, 0x1000, 0x0000, 0x0000,
};

static int S(int v)
{
    return MulDiv(v, g_ui.dpi, 96);
}

/* ---- Posting from workers ---- */

sb_t *ui_text(const char *text)
{
    sb_t *sb = mem_alloc(sizeof *sb);

    sb_add(sb, text ? text : "");
    return sb;
}

void ui_post(UINT msg, sb_t *payload)
{
    if (!PostMessageW(g_ui.wnd, msg, 0, (LPARAM)payload) && payload) {
        sb_free(payload);
        mem_free(payload);
    }
}

void ui_post_model(model_t *model)
{
    if (!PostMessageW(g_ui.wnd, UI_READY, 0, (LPARAM)model))
        model_free(model);
}

static void set_text(sb_t *dst, const char *text)
{
    sb_clear(dst);
    sb_add(dst, text ? text : "");
}

static const char *str_or_empty(const sb_t *sb)
{
    return sb->data ? sb->data : "";
}

/* ---- Drawing primitives ---- */

static void fill(int x, int y, int w, int h, int c)
{
    RECT r = {x, y, x + w, y + h};

    gfx_flush(g_ui.g);
    SetDCBrushColor(g_ui.back, GDI(c));
    FillRect(g_ui.back, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

static void text_w(HFONT font, int c, RECT r, const wchar_t *s, int len, UINT flags)
{
    gfx_flush(g_ui.g);
    SelectObject(g_ui.back, font);
    SetTextColor(g_ui.back, GDI(c));
    SetBkMode(g_ui.back, TRANSPARENT);
    DrawTextW(g_ui.back, s, len, &r, flags | DT_NOPREFIX);
}

static void text(HFONT font, int c, RECT r, const char *s, UINT flags)
{
    wchar_t *w = utf8_to_wide(s, lstrlenA(s));

    text_w(font, c, r, w, -1, flags);
    mem_free(w);
}

static int text_width(HFONT font, const char *s)
{
    wchar_t *w = utf8_to_wide(s, lstrlenA(s));
    RECT r = {0, 0, 10000, 100};

    SelectObject(g_ui.back, font);
    DrawTextW(g_ui.back, w, -1, &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    mem_free(w);
    return r.right;
}

static RECT rect(int x, int y, int w, int h)
{
    RECT r = {x, y, x + w, y + h};
    return r;
}

static void draw_wordmark(int x, int y, int unit)
{
    for (int g = 0; g < 8; g++)
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if ((k_word[g][row] >> (4 - col)) & 1)
                    fill(x + (g * 6 + col) * unit, y + row * unit, unit, unit, C_INK);
    fill(x + 48 * unit, y, 4 * unit, 7 * unit, C_AMBER);
}

static int wordmark_width(int unit)
{
    return 52 * unit;
}

static void draw_mark(int x, int y, int unit, int c)
{
    for (int row = 0; row < 16; row++)
        for (int col = 0; col < 16; col++)
            if ((k_mark[row] >> (15 - col)) & 1)
                fill(x + col * unit, y + row * unit, unit, unit, c);
}

/* ---- Images ---- */

static image_t *image_find(const char *key)
{
    for (int i = 0; i < g_ui.nimages; i++)
        if (lstrcmpA(g_ui.images[i].key, key) == 0)
            return &g_ui.images[i];
    return NULL;
}

/* Returns the image if loaded; starts loading it the first time it is asked for. */
static gfx_image_t *image_get(const char *key, const char *path)
{
    image_t *im = image_find(key);

    if (im)
        return im->img;
    if (g_ui.nimages == g_ui.cap_images) {
        g_ui.cap_images = g_ui.cap_images ? g_ui.cap_images * 2 : 64;
        g_ui.images = mem_realloc(g_ui.images, (size_t)g_ui.cap_images * sizeof *g_ui.images);
    }
    im = &g_ui.images[g_ui.nimages++];
    lstrcpynA(im->key, key, sizeof im->key);
    im->img = NULL;
    im->failed = 0;
    img_request(key, path);
    return NULL;
}

static void images_clear(void)
{
    img_clear();
    for (int i = 0; i < g_ui.nimages; i++)
        gfx_image_free(g_ui.images[i].img);
    g_ui.nimages = 0;
}

static gfx_image_t *guild_icon(const guild_t *gd)
{
    char key[96], path[160];

    if (!gd->icon[0])
        return NULL;
    wsprintfA(key, "g:%s:%s", gd->id, gd->icon);
    wsprintfA(path, "/icons/%s/%s.png?size=96", gd->id, gd->icon);
    return image_get(key, path);
}

static gfx_image_t *user_avatar(const char *id, const char *hash)
{
    char key[96], path[160];

    if (hash[0]) {
        wsprintfA(key, "a:%s:%s", id, hash);
        wsprintfA(path, "/avatars/%s/%s.png?size=64", id, hash);
    } else {
        /* Default avatar: (id >> 22) % 6 */
        unsigned long long n = 0;
        for (const char *p = id; *p >= '0' && *p <= '9'; p++)
            n = n * 10 + (unsigned long long)(*p - '0');
        wsprintfA(key, "d:%d", (int)((n >> 22) % 6));
        wsprintfA(path, "/embed/avatars/%d.png", (int)((n >> 22) % 6));
    }
    return image_get(key, path);
}

/* ---- Login view ---- */

static RECT login_card(void)
{
    RECT rc;
    int w = S(760), h = S(380);

    GetClientRect(g_ui.wnd, &rc);
    return rect((rc.right - w) / 2, (rc.bottom - h) / 2 + S(30), w, h);
}

static void paint_step(int x, int y, const char *n, const char *label)
{
    gfx_circle(g_ui.g, x, y, S(24), ARGB(C_AMBER));
    text(g_ui.f_small, C_RAIL, rect(x, y, S(24), S(24)), n, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    text(g_ui.f_body, C_INK, rect(x + S(38), y, S(360), S(24)), label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void paint_login(RECT rc)
{
    RECT card = login_card();
    int x = card.left + S(44), y = card.top + S(44);
    int tile = S(232), tx = card.right - S(44) - tile, ty = card.top + (card.bottom - card.top - tile) / 2;

    fill(0, 0, rc.right, rc.bottom, C_RAIL);
    draw_wordmark((rc.right - wordmark_width(S(3))) / 2, card.top - S(72), S(3));
    gfx_round_rect(g_ui.g, card.left, card.top, card.right - card.left, card.bottom - card.top, S(16), ARGB(C_MAIN));

    text(g_ui.f_title, C_INK, rect(x, y, S(400), S(32)), "Log in with your phone", DT_LEFT | DT_SINGLELINE);
    text(g_ui.f_body, C_MUTED, rect(x, y + S(40), S(400), S(24)),
         "Scan the code with the Discord mobile app.", DT_LEFT | DT_SINGLELINE);
    paint_step(x, y + S(92), "1", "Open Discord on your phone");
    paint_step(x, y + S(132), "2", "Go to Settings \xE2\x80\xBA Scan QR Code");
    paint_step(x, y + S(172), "3", "Scan this code and confirm");
    text(g_ui.f_small, C_FAINT, rect(x, y + S(230), S(400), S(40)),
         "Passkeys, two-factor codes and SMS checks happen on your phone.", DT_LEFT | DT_WORDBREAK);

    gfx_round_rect(g_ui.g, tx, ty, tile, tile, S(12), g_ui.scanned.len ? ARGB(C_PANEL) : ARGB(C_INK));
    if (g_ui.scanned.len) {
        text_w(g_ui.f_icon_big, C_AMBER, rect(tx, ty + S(52), tile, S(48)), ICON_CHECK, -1, DT_CENTER | DT_SINGLELINE);
        text(g_ui.f_h, C_INK, rect(tx + S(8), ty + S(116), tile - S(16), S(24)), g_ui.scanned.data,
             DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        text(g_ui.f_small, C_MUTED, rect(tx, ty + S(144), tile, S(20)), "Confirm the login on your phone",
             DT_CENTER | DT_SINGLELINE);
    } else if (g_ui.qr) {
        int m = (tile - S(40)) / g_ui.qr->size; /* leaves a ~4 module quiet zone */
        int off = (tile - m * g_ui.qr->size) / 2;
        for (int qy = 0; qy < g_ui.qr->size; qy++)
            for (int qx = 0; qx < g_ui.qr->size; qx++)
                if (qr_dark(g_ui.qr, qx, qy))
                    fill(tx + off + qx * m, ty + off + qy * m, m, m, C_RAIL);
        /* Silicord mark in the middle: level M error correction absorbs it. */
        gfx_round_rect(g_ui.g, tx + tile / 2 - S(15), ty + tile / 2 - S(15), S(30), S(30), S(8), ARGB(C_INK));
        gfx_round_rect(g_ui.g, tx + tile / 2 - S(12), ty + tile / 2 - S(12), S(24), S(24), S(6), ARGB(C_RAIL));
        draw_mark(tx + tile / 2 - S(8), ty + tile / 2 - S(8), S(1), C_AMBER);
    } else {
        text(g_ui.f_body, C_RAIL, rect(tx, ty, tile, tile), "Getting a code\xE2\x80\xA6",
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    text(g_ui.f_body, C_AMBER, rect(0, card.bottom + S(24), rc.right, S(24)), str_or_empty(&g_ui.status),
         DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void paint_loading(RECT rc)
{
    int unit = S(4);

    fill(0, 0, rc.right, rc.bottom, C_RAIL);
    draw_wordmark((rc.right - wordmark_width(unit)) / 2, rc.bottom / 2 - S(40), unit);
    text(g_ui.f_body, C_MUTED, rect(0, rc.bottom / 2 + S(10), rc.right, S(24)), str_or_empty(&g_ui.status),
         DT_CENTER | DT_SINGLELINE);
}

/* ---- App view: geometry ---- */

static int rail_y(int i) /* i = -1 for home */
{
    return S(12) + (i + 1) * S(RAIL_STEP) + (i >= 0 ? S(12) : 0) - g_ui.rail_scroll;
}

static int rail_content(void)
{
    return S(12) + (g_ui.model ? (int)g_ui.model->nguilds + 1 : 1) * S(RAIL_STEP) + S(12) + S(12);
}

static const channel_t *chan(int i)
{
    return &g_ui.model->channels[i];
}

/* Whether channel i is hidden inside a collapsed category. */
static int hidden(const guild_t *gd, unsigned i)
{
    for (unsigned j = i; j-- > gd->first;)
        if (chan((int)j)->type == CH_CATEGORY)
            return g_ui.collapsed[j];
    return 0;
}

static int row_height(unsigned i)
{
    return chan((int)i)->type == CH_CATEGORY ? S(CAT_H) : S(ROW_H);
}

static int side_content(void)
{
    const guild_t *gd;
    int h = S(8);

    if (!g_ui.model || g_ui.guild < 0)
        return 0;
    gd = &g_ui.model->guilds[g_ui.guild];
    for (unsigned i = gd->first; i < gd->first + gd->count; i++)
        if (chan((int)i)->type == CH_CATEGORY || !hidden(gd, i))
            h += row_height(i);
    return h + S(8);
}

static int side_view(RECT rc)
{
    return rc.bottom - S(HEADER_H) - S(PANEL_H);
}

static void clamp_scroll(void)
{
    RECT rc;
    int max;

    GetClientRect(g_ui.wnd, &rc);
    max = rail_content() - rc.bottom;
    if (g_ui.rail_scroll > max)
        g_ui.rail_scroll = max;
    if (g_ui.rail_scroll < 0)
        g_ui.rail_scroll = 0;
    max = side_content() - side_view(rc);
    if (g_ui.side_scroll > max)
        g_ui.side_scroll = max;
    if (g_ui.side_scroll < 0)
        g_ui.side_scroll = 0;
}

static void hit_test(int x, int y, int *kind, int *index)
{
    RECT rc;

    *kind = HIT_NONE;
    *index = -1;
    GetClientRect(g_ui.wnd, &rc);
    if (x < S(RAIL_W)) {
        if (y >= rail_y(-1) && y < rail_y(-1) + S(ICON)) {
            *kind = HIT_HOME;
            return;
        }
        for (int i = 0; g_ui.model && i < (int)g_ui.model->nguilds; i++)
            if (y >= rail_y(i) && y < rail_y(i) + S(ICON)) {
                *kind = HIT_GUILD;
                *index = i;
                return;
            }
    } else if (x < S(RAIL_W + SIDE_W)) {
        int top = rc.bottom - S(PANEL_H) + (S(PANEL_H) - S(32)) / 2;
        int right = S(RAIL_W + SIDE_W) - S(8);
        if (y >= rc.bottom - S(PANEL_H)) {
            if (y >= top && y < top + S(32) && x >= right - S(32) && x < right)
                *kind = HIT_LOGOUT;
            else if (g_ui.disconnected && y >= top && y < top + S(32) && x >= right - S(68) && x < right - S(36))
                *kind = HIT_RETRY;
            return;
        }
        if (y >= S(HEADER_H) && g_ui.model && g_ui.guild >= 0) {
            const guild_t *gd = &g_ui.model->guilds[g_ui.guild];
            int ry = S(HEADER_H) + S(8) - g_ui.side_scroll;
            for (unsigned i = gd->first; i < gd->first + gd->count; i++) {
                int h;
                if (chan((int)i)->type != CH_CATEGORY && hidden(gd, i))
                    continue;
                h = row_height(i);
                if (y >= ry && y < ry + h) {
                    *kind = HIT_CHANNEL;
                    *index = (int)i;
                    return;
                }
                ry += h;
            }
        }
    }
}

/* ---- App view: painting ---- */

static void initials(const char *name, wchar_t *out, int max)
{
    wchar_t *w = utf8_to_wide(name, lstrlenA(name));
    int n = 0, word = 1;

    for (wchar_t *p = w; *p && n < max - 2; p++) {
        if (*p == L' ') {
            word = 1;
        } else if (word) {
            out[n++] = *p;
            if (*p >= 0xD800 && *p < 0xDC00 && p[1])
                out[n++] = *++p;
            word = 0;
            if (n >= 3)
                break;
        }
    }
    out[n] = 0;
    mem_free(w);
}

static void paint_pill(int y, int height)
{
    gfx_round_rect(g_ui.g, -S(4), y + (S(ICON) - height) / 2, S(8), height, S(4), ARGB(C_INK));
}

static void paint_rail(RECT rc)
{
    int x = (S(RAIL_W) - S(ICON)) / 2;
    int home_y = rail_y(-1), sel_home = g_ui.guild < 0, hov_home = g_ui.hover_kind == HIT_HOME;

    fill(0, 0, S(RAIL_W), rc.bottom, C_RAIL);

    /* Home: the Silicord mark. */
    gfx_round_rect(g_ui.g, x, home_y, S(ICON), S(ICON), sel_home || hov_home ? S(16) : S(ICON) / 2,
                   sel_home || hov_home ? ARGB(C_AMBER) : ARGB(C_ITEM));
    draw_mark(x + S(8), home_y + S(8), S(2), sel_home || hov_home ? C_RAIL : C_AMBER);
    if (sel_home)
        paint_pill(home_y, S(40));
    fill(x + S(8), home_y + S(ICON) + S(10), S(32), S(2), C_LINE);

    for (int i = 0; g_ui.model && i < (int)g_ui.model->nguilds; i++) {
        const guild_t *gd = &g_ui.model->guilds[i];
        int y = rail_y(i), sel = g_ui.guild == i, hov = g_ui.hover_kind == HIT_GUILD && g_ui.hover_index == i;
        int radius = sel || hov ? S(16) : S(ICON) / 2;
        gfx_image_t *img;

        if (y + S(ICON) < 0 || y > rc.bottom)
            continue;
        img = guild_icon(gd);
        if (img) {
            gfx_image(g_ui.g, img, x, y, S(ICON), S(ICON), radius);
        } else {
            wchar_t ini[8];
            initials(model_str(g_ui.model, gd->name), ini, 8);
            gfx_round_rect(g_ui.g, x, y, S(ICON), S(ICON), radius, sel || hov ? ARGB(C_AMBER) : ARGB(C_ITEM));
            text_w(lstrlenW(ini) > 2 ? g_ui.f_initial_small : g_ui.f_initial, sel || hov ? C_RAIL : C_INK,
                   rect(x, y, S(ICON), S(ICON)), ini, -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (sel || hov)
            paint_pill(y, sel ? S(40) : S(20));
    }
}

static void paint_scrollbar(int x, int top, int view, int content, int scroll)
{
    int h, y;

    if (content <= view)
        return;
    h = view * view / content;
    if (h < S(24))
        h = S(24);
    y = top + (view - h) * scroll / (content - view);
    gfx_round_rect(g_ui.g, x, y, S(4), h, S(2), 0xFF2E2E2E);
}

static void paint_channel_row(unsigned i, int y)
{
    const channel_t *c = chan((int)i);
    int x = S(RAIL_W) + S(8), w = S(SIDE_W) - S(16);
    int sel = g_ui.channel == (int)i, hov = g_ui.hover_kind == HIT_CHANNEL && g_ui.hover_index == (int)i;
    const char *name = model_str(g_ui.model, c->name);

    if (c->type == CH_CATEGORY) {
        text_w(g_ui.f_icon, hov ? C_INK : C_FAINT, rect(x - S(2), y + S(18), S(14), S(18)),
               g_ui.collapsed[i] ? ICON_CHEVRON_RIGHT : ICON_CHEVRON_DOWN, -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(g_ui.f_cat, hov ? C_INK : C_MUTED, rect(x + S(14), y + S(18), w - S(14), S(18)), name,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }
    if (sel || hov)
        gfx_round_rect(g_ui.g, x, y + S(1), w, S(ROW_H) - S(2), S(6), sel ? ARGB(C_SELECT) : ARGB(C_HOVER));
    if (c->type == CH_VOICE || c->type == CH_STAGE)
        text_w(g_ui.f_icon, C_FAINT, rect(x + S(8), y, S(20), S(ROW_H)), ICON_VOLUME, -1,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    else
        text(g_ui.f_h, C_FAINT, rect(x + S(8), y, S(20), S(ROW_H)), "#", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    text(g_ui.f_body, sel ? C_INK : hov ? C_INK : C_MUTED, rect(x + S(34), y, w - S(40), S(ROW_H)), name,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void paint_user_panel(RECT rc)
{
    int x0 = S(RAIL_W), y = rc.bottom - S(PANEL_H), cy = y + (S(PANEL_H) - S(32)) / 2;
    int right = S(RAIL_W + SIDE_W) - S(8);
    const char *name = g_ui.model && g_ui.model->user_name ? model_str(g_ui.model, g_ui.model->user_name)
                                                          : str_or_empty(&g_ui.account);
    int dot = g_ui.disconnected ? C_FAINT : g_ui.model ? C_GREEN : C_AMBER;

    fill(x0, y, S(SIDE_W), S(PANEL_H), C_PANEL);
    if (g_ui.model && g_ui.model->user_id[0]) {
        gfx_image_t *img = user_avatar(g_ui.model->user_id, g_ui.model->user_avatar);
        if (img)
            gfx_image(g_ui.g, img, x0 + S(10), cy, S(32), S(32), S(16));
        else
            gfx_circle(g_ui.g, x0 + S(10), cy, S(32), ARGB(C_ITEM));
    } else {
        gfx_circle(g_ui.g, x0 + S(10), cy, S(32), ARGB(C_ITEM));
    }
    gfx_circle(g_ui.g, x0 + S(10) + S(21), cy + S(21), S(14), ARGB(C_PANEL));
    gfx_circle(g_ui.g, x0 + S(10) + S(23), cy + S(23), S(10), ARGB(dot));

    text(g_ui.f_h, C_INK, rect(x0 + S(52), cy - S(2), S(120), S(20)), name, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    text(g_ui.f_small, C_MUTED, rect(x0 + S(52), cy + S(17), S(120), S(18)), str_or_empty(&g_ui.status),
         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (g_ui.hover_kind == HIT_LOGOUT)
        gfx_round_rect(g_ui.g, right - S(32), cy, S(32), S(32), S(6), ARGB(C_SELECT));
    text_w(g_ui.f_icon, g_ui.hover_kind == HIT_LOGOUT ? C_INK : C_MUTED, rect(right - S(32), cy, S(32), S(32)),
           ICON_POWER, -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (g_ui.disconnected) {
        if (g_ui.hover_kind == HIT_RETRY)
            gfx_round_rect(g_ui.g, right - S(68), cy, S(32), S(32), S(6), ARGB(C_SELECT));
        text_w(g_ui.f_icon, C_AMBER, rect(right - S(68), cy, S(32), S(32)), ICON_REFRESH, -1,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static void paint_side(RECT rc)
{
    int x0 = S(RAIL_W), view = side_view(rc);

    fill(x0, 0, S(SIDE_W), rc.bottom, C_SIDE);

    if (g_ui.model && g_ui.guild >= 0) {
        const guild_t *gd = &g_ui.model->guilds[g_ui.guild];
        int y = S(HEADER_H) + S(8) - g_ui.side_scroll;
        HRGN clip = CreateRectRgn(x0, S(HEADER_H), x0 + S(SIDE_W), S(HEADER_H) + view);

        gfx_flush(g_ui.g);
        SelectClipRgn(g_ui.back, clip);
        gfx_end(g_ui.g);
        g_ui.g = gfx_begin(g_ui.back); /* GDI+ picks up the clip region */
        for (unsigned i = gd->first; i < gd->first + gd->count; i++) {
            if (chan((int)i)->type != CH_CATEGORY && hidden(gd, i))
                continue;
            if (y + row_height(i) > S(HEADER_H) && y < S(HEADER_H) + view)
                paint_channel_row(i, y);
            y += row_height(i);
        }
        paint_scrollbar(x0 + S(SIDE_W) - S(6), S(HEADER_H) + S(4), view - S(8), side_content(), g_ui.side_scroll);
        gfx_end(g_ui.g);
        SelectClipRgn(g_ui.back, NULL);
        DeleteObject(clip);
        g_ui.g = gfx_begin(g_ui.back);

        text(g_ui.f_h, C_INK, rect(x0 + S(16), 0, S(SIDE_W) - S(32), S(HEADER_H)), model_str(g_ui.model, gd->name),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        text(g_ui.f_h, C_INK, rect(x0 + S(16), 0, S(SIDE_W) - S(32), S(HEADER_H)), "Home",
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (g_ui.model) {
            char line[64];
            wsprintfA(line, "%u servers", g_ui.model->nguilds);
            text(g_ui.f_body, C_MUTED, rect(x0 + S(16), S(HEADER_H) + S(12), S(SIDE_W) - S(32), S(24)), line,
                 DT_LEFT | DT_SINGLELINE);
        }
    }
    fill(x0, S(HEADER_H) - 1, S(SIDE_W), 1, C_LINE);
    paint_user_panel(rc);
}

static void paint_main(RECT rc)
{
    int x0 = S(RAIL_W + SIDE_W), w = rc.right - x0;

    fill(x0, 0, w, rc.bottom, C_MAIN);
    fill(x0, S(HEADER_H) - 1, w, 1, C_LINE);

    if (g_ui.model && g_ui.channel >= 0) {
        const channel_t *c = chan(g_ui.channel);
        const char *name = model_str(g_ui.model, c->name);
        int voice = c->type == CH_VOICE || c->type == CH_STAGE;
        int by = rc.bottom - S(24) - S(44) - S(24) - S(170);
        char title[160];

        if (voice)
            text_w(g_ui.f_icon, C_FAINT, rect(x0 + S(16), 0, S(24), S(HEADER_H)), ICON_VOLUME, -1,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        else
            text(g_ui.f_title, C_FAINT, rect(x0 + S(16), 0, S(24), S(HEADER_H)), "#", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(g_ui.f_h, C_INK, rect(x0 + S(46), 0, w - S(62), S(HEADER_H)), name,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        /* Start-of-channel welcome, like the top of an empty history. */
        gfx_circle(g_ui.g, x0 + S(24), by, S(68), ARGB(C_ITEM));
        if (voice)
            text_w(g_ui.f_icon_big, C_INK, rect(x0 + S(24), by, S(68), S(68)), ICON_VOLUME, -1,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        else
            text(g_ui.f_title, C_INK, rect(x0 + S(24), by, S(68), S(68)), "#", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        wsprintfA(title, "Welcome to %s%.120s", voice ? "" : "#", name);
        text(g_ui.f_title, C_INK, rect(x0 + S(24), by + S(84), w - S(48), S(32)), title,
             DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        text(g_ui.f_body, C_MUTED, rect(x0 + S(24), by + S(122), w - S(48), S(24)),
             voice ? "Voice channels are not supported yet." : "Reading and sending messages arrives in the next version.",
             DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        /* Composer, not wired yet. */
        if (!voice) {
            char hint[160];
            int cy = rc.bottom - S(24) - S(44);
            gfx_round_rect(g_ui.g, x0 + S(16), cy, w - S(32), S(44), S(10), 0xFF1F1F1F);
            wsprintfA(hint, "Message #%.120s", name);
            text(g_ui.f_body, C_FAINT, rect(x0 + S(32), cy, w - S(64), S(44)), hint,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    } else {
        int unit = S(4);
        int y = rc.bottom / 2 - S(60);
        draw_wordmark(x0 + (w - wordmark_width(unit)) / 2, y, unit);
        text(g_ui.f_h, C_INK, rect(x0, y + S(56), w, S(24)),
             g_ui.model ? "Pick a server on the left" : "Connecting\xE2\x80\xA6", DT_CENTER | DT_SINGLELINE);
        text(g_ui.f_body, C_MUTED, rect(x0, y + S(84), w, S(24)),
             "Native, tiny, and asleep until something happens.", DT_CENTER | DT_SINGLELINE);
    }
}

static void paint_tooltip(void)
{
    const char *name;
    int y, tw, th = S(36);

    if (g_ui.hover_kind != HIT_GUILD || !g_ui.model)
        return;
    name = model_str(g_ui.model, g_ui.model->guilds[g_ui.hover_index].name);
    tw = text_width(g_ui.f_h, name) + S(24);
    if (tw > S(320))
        tw = S(320);
    y = rail_y(g_ui.hover_index) + (S(ICON) - th) / 2;
    gfx_round_rect(g_ui.g, S(RAIL_W) + S(4), y, tw, th, S(6), ARGB(C_TIP));
    text(g_ui.f_h, C_INK, rect(S(RAIL_W) + S(16), y, tw - S(24), th), name,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void paint_app(RECT rc)
{
    paint_main(rc);
    paint_side(rc);
    paint_rail(rc);
    paint_tooltip();
}

static void ensure_back_buffer(HDC dc, int w, int h)
{
    if (g_ui.back && g_ui.back_w == w && g_ui.back_h == h)
        return;
    if (g_ui.back) {
        SelectObject(g_ui.back, g_ui.back_old);
        DeleteObject(g_ui.back_bmp);
        DeleteDC(g_ui.back);
    }
    g_ui.back = CreateCompatibleDC(dc);
    g_ui.back_bmp = CreateCompatibleBitmap(dc, w, h);
    g_ui.back_old = SelectObject(g_ui.back, g_ui.back_bmp);
    g_ui.back_w = w;
    g_ui.back_h = h;
}

static void paint(HWND wnd)
{
    PAINTSTRUCT ps;
    RECT rc;
    HDC dc = BeginPaint(wnd, &ps);

    GetClientRect(wnd, &rc);
    if (rc.right > 0 && rc.bottom > 0) {
        ensure_back_buffer(dc, rc.right, rc.bottom);
        g_ui.g = gfx_begin(g_ui.back);
        if (g_ui.view == VIEW_APP)
            paint_app(rc);
        else if (g_ui.view == VIEW_LOADING)
            paint_loading(rc);
        else
            paint_login(rc);
        gfx_end(g_ui.g);
        g_ui.g = NULL;
        BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
               ps.rcPaint.bottom - ps.rcPaint.top, g_ui.back, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    }
    EndPaint(wnd, &ps);
}

static void redraw(void)
{
    InvalidateRect(g_ui.wnd, NULL, FALSE);
}

/* ---- Fonts, icon ---- */

static HFONT make_font(const wchar_t *face, int px, int weight)
{
    return CreateFontW(-S(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

static void make_fonts(void)
{
    HFONT *f[] = {&g_ui.f_title, &g_ui.f_h, &g_ui.f_body, &g_ui.f_small, &g_ui.f_cat,
                  &g_ui.f_icon, &g_ui.f_icon_big, &g_ui.f_initial, &g_ui.f_initial_small};

    for (int i = 0; i < (int)ARRAYSIZE(f); i++)
        if (*f[i])
            DeleteObject(*f[i]);
    g_ui.f_title = make_font(L"Segoe UI", 22, FW_SEMIBOLD);
    g_ui.f_h = make_font(L"Segoe UI", 15, FW_SEMIBOLD);
    g_ui.f_body = make_font(L"Segoe UI", 15, FW_NORMAL);
    g_ui.f_small = make_font(L"Segoe UI", 13, FW_NORMAL);
    g_ui.f_cat = make_font(L"Segoe UI", 12, FW_BOLD);
    g_ui.f_icon = make_font(L"Segoe MDL2 Assets", 14, FW_NORMAL);
    g_ui.f_icon_big = make_font(L"Segoe MDL2 Assets", 32, FW_NORMAL);
    g_ui.f_initial = make_font(L"Segoe UI", 17, FW_SEMIBOLD);
    g_ui.f_initial_small = make_font(L"Segoe UI", 13, FW_SEMIBOLD);
}

static HICON make_icon(int px)
{
    int scale = px / 16;
    BITMAPV5HEADER bi = {0};
    ICONINFO ii = {0};
    unsigned *bits;
    HDC dc = GetDC(NULL);
    HICON icon;

    bi.bV5Size = sizeof bi;
    bi.bV5Width = px;
    bi.bV5Height = -px;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    ii.fIcon = TRUE;
    ii.hbmColor = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    ii.hbmMask = CreateBitmap(px, px, 1, 1, NULL);
    for (int y = 0; y < px; y++)
        for (int x = 0; x < px; x++)
            bits[y * px + x] = (k_mark[y / scale] >> (15 - x / scale)) & 1 ? 0xFFFFB000u : 0;
    icon = CreateIconIndirect(&ii);
    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
    ReleaseDC(NULL, dc);
    return icon;
}

/* ---- Selection ---- */

static void select_guild(int i)
{
    if (!g_ui.model)
        return;
    g_ui.guild = i;
    g_ui.side_scroll = 0;
    g_ui.channel = i >= 0 ? g_ui.last_channel[i] : -1;
    if (i >= 0 && g_ui.channel < 0) {
        const guild_t *gd = &g_ui.model->guilds[i];
        for (unsigned c = gd->first; c < gd->first + gd->count; c++)
            if (chan((int)c)->type != CH_CATEGORY && chan((int)c)->type != CH_VOICE && chan((int)c)->type != CH_STAGE) {
                g_ui.channel = (int)c;
                break;
            }
    }
    redraw();
}

static void on_click(int kind, int index)
{
    switch (kind) {
    case HIT_HOME:
        select_guild(-1);
        break;
    case HIT_GUILD:
        select_guild(index);
        break;
    case HIT_CHANNEL:
        if (chan(index)->type == CH_CATEGORY) {
            g_ui.collapsed[index] ^= 1;
            clamp_scroll();
        } else {
            g_ui.channel = index;
            g_ui.last_channel[g_ui.guild] = index;
        }
        redraw();
        break;
    case HIT_LOGOUT:
        app_logout();
        break;
    case HIT_RETRY:
        g_ui.disconnected = 0;
        set_text(&g_ui.status, "Connecting\xE2\x80\xA6");
        redraw();
        app_reconnect();
        break;
    }
}

static void set_model(model_t *m)
{
    model_free(g_ui.model);
    mem_free(g_ui.last_channel);
    mem_free(g_ui.collapsed);
    g_ui.model = m;
    g_ui.last_channel = mem_alloc(((size_t)m->nguilds + 1) * sizeof *g_ui.last_channel);
    for (unsigned i = 0; i < m->nguilds; i++)
        g_ui.last_channel[i] = -1;
    g_ui.collapsed = mem_alloc((size_t)m->nchannels + 1);
    g_ui.guild = g_ui.channel = -1;
    g_ui.rail_scroll = g_ui.side_scroll = 0;
}

static void clear_session(void)
{
    if (g_ui.model) {
        model_free(g_ui.model);
        g_ui.model = NULL;
    }
    images_clear();
    g_ui.guild = g_ui.channel = -1;
    g_ui.hover_kind = HIT_NONE;
    sb_clear(&g_ui.account);
}

/* ---- Messages from workers ---- */

static void on_worker(UINT msg, WPARAM wp, LPARAM lp)
{
    sb_t *p = (sb_t *)lp;
    const char *s = p && p->data ? p->data : "";

    switch (msg) {
    case UI_QR:
        if (!g_ui.qr)
            g_ui.qr = mem_alloc(sizeof *g_ui.qr);
        if (!qr_encode(s, p->len, g_ui.qr)) {
            mem_free(g_ui.qr);
            g_ui.qr = NULL;
        }
        sb_clear(&g_ui.scanned);
        set_text(&g_ui.status, "");
        break;
    case UI_SCANNED:
        set_text(&g_ui.scanned, *s ? s : "Your account");
        break;
    case UI_STATUS:
        set_text(&g_ui.status, s);
        break;
    case UI_TOKEN:
        app_login_token(s);
        break;
    case UI_LOGIN_FAILED:
        ui_show_login();
        set_text(&g_ui.status, s);
        break;
    case UI_ACCOUNT:
        set_text(&g_ui.account, s);
        set_text(&g_ui.status, "Connecting\xE2\x80\xA6");
        g_ui.disconnected = 0;
        g_ui.view = VIEW_APP;
        break;
    case UI_READY:
        set_model((model_t *)lp);
        set_text(&g_ui.status, "Online");
        g_ui.view = VIEW_APP;
        redraw();
        return;
    case UI_DISCONNECTED:
        g_ui.disconnected = 1;
        set_text(&g_ui.status, s);
        g_ui.view = VIEW_APP;
        break;
    case UI_IMAGE: {
        image_t *im = image_find(s);
        if (im) {
            im->img = (gfx_image_t *)wp;
            im->failed = !wp;
        } else {
            gfx_image_free((gfx_image_t *)wp);
        }
        break;
    }
    }
    if (p) {
        sb_free(p);
        mem_free(p);
    }
    redraw();
}

void ui_show_login(void)
{
    clear_session();
    if (g_ui.qr) {
        mem_free(g_ui.qr);
        g_ui.qr = NULL;
    }
    sb_clear(&g_ui.scanned);
    set_text(&g_ui.status, "");
    g_ui.disconnected = 0;
    g_ui.view = VIEW_LOGIN;
    redraw();
}

void ui_show_loading(const char *s)
{
    set_text(&g_ui.status, s);
    g_ui.view = VIEW_LOADING;
    redraw();
}

/* ---- Window procedure ---- */

static void update_hover(int x, int y)
{
    int kind, index;

    hit_test(x, y, &kind, &index);
    if (kind != g_ui.hover_kind || index != g_ui.hover_index) {
        g_ui.hover_kind = kind;
        g_ui.hover_index = index;
        SetCursor(LoadCursorW(NULL, (LPCWSTR)(kind != HIT_NONE ? IDC_HAND : IDC_ARROW)));
        redraw();
    }
}

static LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_ui.wnd = wnd;
        g_ui.dpi = GetDpiForWindow(wnd);
        make_fonts();
        img_init(wnd, UI_IMAGE);
        return 0;
    case WM_SIZE:
        clamp_scroll();
        redraw();
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = MulDiv(940, GetDpiForWindow(wnd), 96);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = MulDiv(620, GetDpiForWindow(wnd), 96);
        return 0;
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        g_ui.dpi = HIWORD(wp);
        make_fonts();
        SetWindowPos(wnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(wnd);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)(g_ui.hover_kind != HIT_NONE ? IDC_HAND : IDC_ARROW)));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, wnd, 0};
        TrackMouseEvent(&tme);
        if (g_ui.view == VIEW_APP)
            update_hover(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_ui.hover_kind != HIT_NONE) {
            g_ui.hover_kind = HIT_NONE;
            g_ui.hover_index = -1;
            redraw();
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_ui.view == VIEW_APP) {
            int kind, index;
            hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &kind, &index);
            on_click(kind, index);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int delta = -GET_WHEEL_DELTA_WPARAM(wp) * S(ROW_H) * 3 / WHEEL_DELTA;
        ScreenToClient(wnd, &pt);
        if (g_ui.view != VIEW_APP)
            return 0;
        if (pt.x < S(RAIL_W))
            g_ui.rail_scroll += delta;
        else if (pt.x < S(RAIL_W + SIDE_W))
            g_ui.side_scroll += delta;
        clamp_scroll();
        update_hover(pt.x, pt.y);
        redraw();
        return 0;
    }
    case WM_CLOSE:
        app_quit();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        if (msg >= UI_QR && msg <= UI_IMAGE) {
            on_worker(msg, wp, lp);
            return 0;
        }
        break;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

HWND ui_create(HINSTANCE inst)
{
    WNDCLASSEXW wc = {0};
    BOOL dark = TRUE;
    COLORREF caption = GDI(C_RAIL);
    UINT dpi = GetDpiForSystem();

    gfx_init();
    g_ui.icon_big = make_icon(32);
    g_ui.icon_small = make_icon(16);

    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = g_ui.icon_big;
    wc.hIconSm = g_ui.icon_small;
    wc.lpszClassName = L"Silicord";
    RegisterClassExW(&wc);

    g_ui.wnd = CreateWindowExW(0, L"Silicord", L"Silicord", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(1200, dpi, 96), MulDiv(760, dpi, 96),
                               NULL, NULL, inst, NULL);
    DwmSetWindowAttribute(g_ui.wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    DwmSetWindowAttribute(g_ui.wnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof caption);
    return g_ui.wnd;
}
