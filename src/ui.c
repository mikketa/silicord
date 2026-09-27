/*
 * Custom-drawn Win32 UI: server rail, channel list, main pane. Everything is
 * painted into one back buffer; the only child windows are none so far.
 * Runs on the UI thread only.
 */
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <psapi.h>
#include "ui.h"
#include "render.h"
#include "img.h"
#include "md.h"
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
enum { HIT_NONE, HIT_HOME, HIT_GUILD, HIT_CHANNEL, HIT_LOGOUT, HIT_RETRY, HIT_SELF };

typedef struct {
    char key[96];
    r_image_t *img;
    int failed;
    unsigned used;   /* paint that last drew it */
} image_t;

/* Decoded images kept in memory; images off screen beyond this are dropped (the disk cache keeps them). */
#define IMAGE_BUDGET (16u << 20)
/* Time a paint may spend decoding images from the disk cache before deferring the rest. */
#define SYNC_DECODE_MS 8

typedef struct {
    HWND wnd;
    r_font_t *f_title, *f_h, *f_body, *f_small, *f_cat, *f_icon, *f_icon_big, *f_initial, *f_initial_small;
    r_font_t *f_mono, *f_h1, *f_h2, *f_h3, *f_name;
    r_rich_style_t rich;
    int hover_link;
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
    int last_dm;
    unsigned char *collapsed;  /* per channel (categories) */
    int rail_scroll, side_scroll;
    int hover_kind, hover_index;
    image_t *images;
    int nimages, cap_images;
    unsigned frame;            /* paint counter, for the image cache */
    int log_memory;
    LARGE_INTEGER frame_start, qpf;

    /* Messages of the open channel, oldest first. */
    msg_t *msgs;
    int nmsgs, cap_msgs;
    char msgs_channel[24];
    int msgs_loading, msgs_older_loading, msgs_has_more, msgs_status;
    int msg_scroll;            /* distance from the bottom, in pixels */
    int layout_w;              /* width the cached heights were computed for */
    int hover_msg;
    sb_t send_error;
    HWND composer;
    WNDPROC composer_proc;
    HBRUSH b_composer;

    /* Notifications. */
    NOTIFYICONDATAW tray;
    int notified_channel;
    int ack_pending;
    int reconnecting;
    activity_t *pending[8];   /* messages in DMs we are still looking up */

    /* Profile popout. */
    HWND pop, pop_edit, pop_focus;
    WNDPROC pop_edit_proc;
    HFONT pop_font;
    HBRUSH pop_brush;
    unsigned pop_input_color;
    profile_t *pop_profile;   /* NULL while loading; owned by the cache */
    char pop_user[24], pop_guild[24], pop_avatar[48];
    sb_t pop_name;
    int pop_self, pop_failed, pop_hover, pop_h, pop_input_y;
    int pop_ax, pop_ay, pop_above;
    unsigned pop_frame;
    int pop_badge_x[32], pop_badge_y[32];
    md_doc_t pop_bio;
    r_rich_t *pop_rich;
    int pop_rich_w;
    char pending_dm[24];      /* user whose new DM we open once it exists */

    /* Server members seen in messages: nickname and roles, for names in role colors. */
    struct member *members;
    int nmembers, cap_members;
    profile_t *profiles[8];
    DWORD profile_time[8];
    /* Display name fonts, downloaded on first use. */
    r_font_t *name_fonts[9];
    sb_t font_data[9];
    unsigned char font_state[9];
} ui_t;

typedef struct member {
    char guild[24];
    char user[24];
    int known;          /* 0 asked, 1 answered */
    sb_t nick;
    sb_t roles;         /* comma-separated ids */
    unsigned color;     /* cached for `color_model` */
    const model_t *color_model;
} member_t;

static ui_t g_ui = {.guild = -1, .channel = -1, .hover_msg = -1, .notified_channel = -1};

#define WM_TRAY (WM_APP + 60)
#define TIMER_ACK 1
#define ACK_DELAY 1500

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

void activity_free(activity_t *a)
{
    sb_free(&a->author);
    sb_free(&a->preview);
    sb_free(&a->mention_roles);
    mem_free(a);
}

void ui_post_activity(activity_t *a)
{
    if (!PostMessageW(g_ui.wnd, UI_ACTIVITY, 0, (LPARAM)a))
        activity_free(a);
}

void ui_post_profile(profile_t *p)
{
    if (!PostMessageW(g_ui.wnd, UI_PROFILE, 0, (LPARAM)p)) {
        profile_free(p);
        mem_free(p);
    }
}

void ui_post_font(int id, sb_t *data)
{
    if (!PostMessageW(g_ui.wnd, UI_FONT, (WPARAM)id, (LPARAM)data) && data) {
        sb_free(data);
        mem_free(data);
    }
}

void ui_post_batch(msg_batch_t *batch)
{
    if (!PostMessageW(g_ui.wnd, UI_MESSAGES, 0, (LPARAM)batch))
        msg_batch_free(batch);
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
    r_fill(x, y, w, h, ARGB(c));
}

/* DrawText-style flags onto the renderer's. */
static unsigned rflags(UINT dt)
{
    unsigned f = 0;

    if (dt & DT_CENTER)
        f |= R_CENTER;
    if (dt & DT_RIGHT)
        f |= R_RIGHT;
    if (dt & DT_VCENTER)
        f |= R_VCENTER;
    if (dt & DT_SINGLELINE)
        f |= R_SINGLE;
    if (dt & DT_END_ELLIPSIS)
        f |= R_ELLIPSIS;
    if (dt & DT_WORDBREAK)
        f |= R_WRAP;
    return f;
}

static void text_w(r_font_t *font, int c, RECT r, const wchar_t *s, int len, UINT flags)
{
    r_text(font, ARGB(c), r.left, r.top, r.right - r.left, r.bottom - r.top, s, len, rflags(flags));
}

static void text(r_font_t *font, int c, RECT r, const char *s, UINT flags)
{
    wchar_t *w = utf8_to_wide(s, lstrlenA(s));

    text_w(font, c, r, w, -1, flags);
    mem_free(w);
}

static int text_width(r_font_t *font, const char *s)
{
    wchar_t *w = utf8_to_wide(s, lstrlenA(s));
    int n = r_text_width(font, w, -1);

    mem_free(w);
    return n;
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

/* Milliseconds since the current paint started. */
static int frame_ms(void)
{
    LARGE_INTEGER now;

    if (!g_ui.qpf.QuadPart)
        QueryPerformanceFrequency(&g_ui.qpf);
    QueryPerformanceCounter(&now);
    return (int)((now.QuadPart - g_ui.frame_start.QuadPart) * 1000 / g_ui.qpf.QuadPart);
}

/* --debug: where the memory goes, ours against the whole process. */
static void log_memory(const char *when)
{
    PROCESS_MEMORY_COUNTERS_EX pmc = {sizeof pmc};
    size_t images = 0;
    char line[200];

    for (int i = 0; i < g_ui.nimages; i++)
        images += r_image_bytes(g_ui.images[i].img);
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc);
    wsprintfA(line, "[mem] %s: ours %u KB (peak %u KB, images %u KB in %d), process private %u KB, working set %u KB",
              when, (unsigned)(mem_used() >> 10), (unsigned)(mem_peak() >> 10), (unsigned)(images >> 10), g_ui.nimages,
              (unsigned)(pmc.PrivateUsage >> 10), (unsigned)(pmc.WorkingSetSize >> 10));
    app_log(line);
}

/*
 * Returns the image, from memory, else from the disk cache in the same frame
 * (while the paint has time left), else starts downloading it.
 */
static r_image_t *image_get(const char *key, const char *path, int max_px)
{
    image_t *im = image_find(key);

    if (im && im->img && r_image_lost(im->img)) { /* render target was recreated */
        r_image_free(im->img);
        im->img = NULL;
        im->failed = 0;
        img_request(key, path, max_px);
    }
    if (im) {
        im->used = g_ui.frame;
        return im->img;
    }
    if (g_ui.nimages == g_ui.cap_images) {
        g_ui.cap_images = g_ui.cap_images ? g_ui.cap_images * 2 : 64;
        g_ui.images = mem_realloc(g_ui.images, (size_t)g_ui.cap_images * sizeof *g_ui.images);
    }
    im = &g_ui.images[g_ui.nimages++];
    lstrcpynA(im->key, key, sizeof im->key);
    im->failed = 0;
    im->used = g_ui.frame;
    im->img = frame_ms() < SYNC_DECODE_MS ? img_cached(path, max_px) : NULL;
    if (!im->img)
        img_request(key, path, max_px);
    return im->img;
}

/* Drops the least recently drawn images until the cache fits its budget. Images drawn in `keep` or later stay. */
static void images_trim(unsigned keep)
{
    size_t total = 0;

    for (int i = 0; i < g_ui.nimages; i++)
        total += r_image_bytes(g_ui.images[i].img);
    while (total > IMAGE_BUDGET) {
        int oldest = -1;
        for (int i = 0; i < g_ui.nimages; i++)
            if (g_ui.images[i].img && (int)(g_ui.images[i].used - keep) < 0 &&
                (oldest < 0 || (int)(g_ui.images[i].used - g_ui.images[oldest].used) < 0))
                oldest = i;
        if (oldest < 0)
            return; /* everything left is on screen */
        total -= r_image_bytes(g_ui.images[oldest].img);
        r_image_free(g_ui.images[oldest].img);
        g_ui.images[oldest] = g_ui.images[--g_ui.nimages];
    }
}

static void images_clear(void)
{
    img_clear();
    for (int i = 0; i < g_ui.nimages; i++)
        r_image_free(g_ui.images[i].img);
    g_ui.nimages = 0;
}

static r_image_t *guild_icon(const guild_t *gd)
{
    char key[96], path[160];

    if (!gd->icon[0])
        return NULL;
    wsprintfA(key, "g:%s:%s", gd->id, gd->icon);
    wsprintfA(path, "/icons/%s/%s.png?size=96", gd->id, gd->icon);
    return image_get(key, path, S(ICON));
}

static r_image_t *user_avatar(const char *id, const char *hash)
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
    return image_get(key, path, S(40));
}

static r_image_t *dm_icon(const channel_t *c)
{
    char key[96], path[160];

    if (c->type == CH_DM)
        return c->user_id[0] ? user_avatar(c->user_id, c->avatar) : NULL;
    if (!c->avatar[0])
        return NULL;
    wsprintfA(key, "c:%s:%s", c->id, c->avatar);
    wsprintfA(path, "/channel-icons/%s/%s.png?size=64", c->id, c->avatar);
    return image_get(key, path, S(40));
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
    r_circle(x, y, S(24), ARGB(C_AMBER));
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
    r_round(card.left, card.top, card.right - card.left, card.bottom - card.top, S(16), ARGB(C_MAIN));

    text(g_ui.f_title, C_INK, rect(x, y, S(400), S(32)), "Log in with your phone", DT_LEFT | DT_SINGLELINE);
    text(g_ui.f_body, C_MUTED, rect(x, y + S(40), S(400), S(24)),
         "Scan the code with the Discord mobile app.", DT_LEFT | DT_SINGLELINE);
    paint_step(x, y + S(92), "1", "Open Discord on your phone");
    paint_step(x, y + S(132), "2", "Go to Settings \xE2\x80\xBA Scan QR Code");
    paint_step(x, y + S(172), "3", "Scan this code and confirm");
    text(g_ui.f_small, C_FAINT, rect(x, y + S(230), S(400), S(40)),
         "Passkeys, two-factor codes and SMS checks happen on your phone.", DT_LEFT | DT_WORDBREAK);

    r_round(tx, ty, tile, tile, S(12), g_ui.scanned.len ? ARGB(C_PANEL) : ARGB(C_INK));
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
        r_round(tx + tile / 2 - S(15), ty + tile / 2 - S(15), S(30), S(30), S(8), ARGB(C_INK));
        r_round(tx + tile / 2 - S(12), ty + tile / 2 - S(12), S(24), S(24), S(6), ARGB(C_RAIL));
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

static int is_dm_type(int type)
{
    return type == CH_DM || type == CH_GROUP_DM;
}

/* Channels listed in the side column: the open server's, or the DMs on the home screen. */
static int side_range(unsigned *first, unsigned *count)
{
    if (!g_ui.model)
        return 0;
    if (g_ui.guild >= 0) {
        *first = g_ui.model->guilds[g_ui.guild].first;
        *count = g_ui.model->guilds[g_ui.guild].count;
    } else {
        *first = g_ui.model->dm_first;
        *count = g_ui.model->dm_count;
    }
    return 1;
}

/* Categories stay in the model when empty so new channels have a home; they are not shown. */
static int empty_category(unsigned first, unsigned count, unsigned i)
{
    const channel_t *c = chan((int)i);

    if (c->type != CH_CATEGORY)
        return 0;
    return i + 1 >= first + count || lstrcmpA(chan((int)i + 1)->parent, c->id) != 0;
}

/* Whether channel i is hidden inside a collapsed category. */
static int hidden(unsigned first, unsigned i)
{
    for (unsigned j = i; j-- > first;)
        if (chan((int)j)->type == CH_CATEGORY)
            return g_ui.collapsed[j];
    return 0;
}

#define DM_ROW_H 44

static int row_height(unsigned i)
{
    int type = chan((int)i)->type;
    return type == CH_CATEGORY ? S(CAT_H) : is_dm_type(type) ? S(DM_ROW_H) : S(ROW_H);
}

static int side_content(void)
{
    unsigned first, count;
    int h = S(8);

    if (!side_range(&first, &count))
        return 0;
    for (unsigned i = first; i < first + count; i++)
        if (!empty_category(first, count, i) && (chan((int)i)->type == CH_CATEGORY || !hidden(first, i)))
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
            else if (y >= top && y < top + S(32) && x >= S(RAIL_W) + S(6) && x < S(RAIL_W) + S(176))
                *kind = HIT_SELF;
            return;
        }
        unsigned first, count;
        if (y >= S(HEADER_H) && side_range(&first, &count)) {
            int ry = S(HEADER_H) + S(8) - g_ui.side_scroll;
            for (unsigned i = first; i < first + count; i++) {
                int h;
                if (empty_category(first, count, i) || (chan((int)i)->type != CH_CATEGORY && hidden(first, i)))
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

/* ---- Unread state ---- */

#define C_BADGE 0xFFE5484Du

static int channel_muted(unsigned i)
{
    int g = model_channel_guild(g_ui.model, i);
    return chan((int)i)->muted || (g >= 0 && g_ui.model->guilds[g].muted);
}

static int channel_unread(unsigned i)
{
    return !channel_muted(i) && model_unread(g_ui.model, i);
}

static void range_state(unsigned first, unsigned count, int *unread, int *mentions)
{
    *unread = *mentions = 0;
    for (unsigned i = first; i < first + count; i++) {
        *mentions += chan((int)i)->mentions;
        if (!chan((int)i)->muted && model_unread(g_ui.model, i))
            *unread = 1;
    }
}

static void guild_state(int g, int *unread, int *mentions)
{
    const guild_t *gd = &g_ui.model->guilds[g];

    range_state(gd->first, gd->count, unread, mentions);
    if (gd->muted)
        *unread = 0;
}

static int total_mentions(void)
{
    int total = 0;

    for (unsigned i = 0; g_ui.model && i < g_ui.model->nchannels; i++)
        total += g_ui.model->channels[i].mentions;
    return total;
}

static void update_title(void)
{
    int n = total_mentions();
    wchar_t title[32];

    if (n)
        wsprintfW(title, L"(%d) Silicord", n);
    else
        lstrcpyW(title, L"Silicord");
    SetWindowTextW(g_ui.wnd, title);
}

/* Red count badge whose right edge is at `right`, vertically centered on `cy`. */
static void paint_badge(int right, int cy, int count)
{
    char label[8];
    int w;

    if (count > 99)
        lstrcpyA(label, "99+");
    else
        wsprintfA(label, "%d", count);
    w = text_width(g_ui.f_cat, label) + S(10);
    if (w < S(18))
        w = S(18);
    r_round(right - w, cy - S(9), w, S(18), S(9), C_BADGE);
    text(g_ui.f_cat, C_INK, rect(right - w, cy - S(9), w, S(18)), label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void paint_pill(int y, int height)
{
    r_round(-S(4), y + (S(ICON) - height) / 2, S(8), height, S(4), ARGB(C_INK));
}

static void paint_rail(RECT rc)
{
    int x = (S(RAIL_W) - S(ICON)) / 2;
    int home_y = rail_y(-1), sel_home = g_ui.guild < 0, hov_home = g_ui.hover_kind == HIT_HOME;

    fill(0, 0, S(RAIL_W), rc.bottom, C_RAIL);

    /* Home: the Silicord mark. */
    r_round(x, home_y, S(ICON), S(ICON), sel_home || hov_home ? S(16) : S(ICON) / 2,
                   sel_home || hov_home ? ARGB(C_AMBER) : ARGB(C_ITEM));
    draw_mark(x + S(8), home_y + S(8), S(2), sel_home || hov_home ? C_RAIL : C_AMBER);
    if (sel_home)
        paint_pill(home_y, S(40));
    if (g_ui.model) {
        int unread, mentions;
        range_state(g_ui.model->dm_first, g_ui.model->dm_count, &unread, &mentions);
        if (mentions) {
            r_circle(x + S(ICON) - S(20), home_y + S(ICON) - S(20), S(24), ARGB(C_RAIL));
            paint_badge(x + S(ICON) + S(2), home_y + S(ICON) - S(8), mentions);
        }
    }
    fill(x + S(8), home_y + S(ICON) + S(10), S(32), S(2), C_LINE);

    for (int i = 0; g_ui.model && i < (int)g_ui.model->nguilds; i++) {
        const guild_t *gd = &g_ui.model->guilds[i];
        int y = rail_y(i), sel = g_ui.guild == i, hov = g_ui.hover_kind == HIT_GUILD && g_ui.hover_index == i;
        int radius = sel || hov ? S(16) : S(ICON) / 2;
        r_image_t *img;

        if (y + S(ICON) < 0 || y > rc.bottom)
            continue;
        img = guild_icon(gd);
        if (img) {
            r_image(img, x, y, S(ICON), S(ICON), radius);
        } else {
            wchar_t ini[8];
            initials(model_str(g_ui.model, gd->name), ini, 8);
            r_round(x, y, S(ICON), S(ICON), radius, sel || hov ? ARGB(C_AMBER) : ARGB(C_ITEM));
            text_w(lstrlenW(ini) > 2 ? g_ui.f_initial_small : g_ui.f_initial, sel || hov ? C_RAIL : C_INK,
                   rect(x, y, S(ICON), S(ICON)), ini, -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        {
            int unread, mentions;
            guild_state(i, &unread, &mentions);
            if (sel || hov)
                paint_pill(y, sel ? S(40) : S(20));
            else if (unread || mentions)
                paint_pill(y, S(8));
            if (mentions) {
                r_circle(x + S(ICON) - S(20), y + S(ICON) - S(20), S(24), ARGB(C_RAIL));
                paint_badge(x + S(ICON) + S(2), y + S(ICON) - S(8), mentions);
            }
        }
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
    r_round(x, y, S(4), h, S(2), 0xFF2E2E2E);
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
    if (is_dm_type(c->type)) {
        r_image_t *img = dm_icon(c);
        if (sel || hov)
            r_round(x, y + S(1), w, S(DM_ROW_H) - S(2), S(6), sel ? ARGB(C_SELECT) : ARGB(C_HOVER));
        if (img) {
            r_image(img, x + S(8), y + S(6), S(32), S(32), S(16));
        } else {
            wchar_t ini[8];
            initials(name, ini, 8);
            r_circle(x + S(8), y + S(6), S(32), ARGB(C_ITEM));
            text_w(g_ui.f_small, C_INK, rect(x + S(8), y + S(6), S(32), S(32)), ini, -1,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        {
            int unread = channel_unread(i), badge = c->mentions ? S(30) : 0;
            text(unread ? g_ui.f_h : g_ui.f_body, sel || hov || unread ? C_INK : C_MUTED,
                 rect(x + S(50), y, w - S(56) - badge, S(DM_ROW_H)), name,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (c->mentions)
                paint_badge(x + w - S(8), y + S(DM_ROW_H) / 2, c->mentions);
        }
        return;
    }
    if (sel || hov)
        r_round(x, y + S(1), w, S(ROW_H) - S(2), S(6), sel ? ARGB(C_SELECT) : ARGB(C_HOVER));
    if (c->type == CH_VOICE || c->type == CH_STAGE)
        text_w(g_ui.f_icon, C_FAINT, rect(x + S(8), y, S(20), S(ROW_H)), ICON_VOLUME, -1,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    else
        text(g_ui.f_h, C_FAINT, rect(x + S(8), y, S(20), S(ROW_H)), "#", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    {
        int unread = channel_unread(i), muted = channel_muted(i), badge = c->mentions ? S(30) : 0;
        int color = sel || hov || unread ? C_INK : muted ? C_FAINT : C_MUTED;
        if (unread && !sel)
            r_round(S(RAIL_W) - S(4), y + S(ROW_H) / 2 - S(4), S(8), S(8), S(4), ARGB(C_INK));
        text(unread ? g_ui.f_h : g_ui.f_body, color, rect(x + S(34), y, w - S(40) - badge, S(ROW_H)), name,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (c->mentions)
            paint_badge(x + w - S(8), y + S(ROW_H) / 2, c->mentions);
    }
}

static void paint_user_panel(RECT rc)
{
    int x0 = S(RAIL_W), y = rc.bottom - S(PANEL_H), cy = y + (S(PANEL_H) - S(32)) / 2;
    int right = S(RAIL_W + SIDE_W) - S(8);
    const char *name = g_ui.model && g_ui.model->user_name ? model_str(g_ui.model, g_ui.model->user_name)
                                                          : str_or_empty(&g_ui.account);
    int dot = g_ui.disconnected ? C_FAINT : g_ui.model && !g_ui.reconnecting ? C_GREEN : C_AMBER;

    fill(x0, y, S(SIDE_W), S(PANEL_H), C_PANEL);
    if (g_ui.model && g_ui.model->user_id[0]) {
        r_image_t *img = user_avatar(g_ui.model->user_id, g_ui.model->user_avatar);
        if (img)
            r_image(img, x0 + S(10), cy, S(32), S(32), S(16));
        else
            r_circle(x0 + S(10), cy, S(32), ARGB(C_ITEM));
    } else {
        r_circle(x0 + S(10), cy, S(32), ARGB(C_ITEM));
    }
    r_circle(x0 + S(10) + S(21), cy + S(21), S(14), ARGB(C_PANEL));
    r_circle(x0 + S(10) + S(23), cy + S(23), S(10), ARGB(dot));

    text(g_ui.f_h, C_INK, rect(x0 + S(52), cy - S(2), S(120), S(20)), name, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    text(g_ui.f_small, C_MUTED, rect(x0 + S(52), cy + S(17), S(120), S(18)), str_or_empty(&g_ui.status),
         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (g_ui.hover_kind == HIT_LOGOUT)
        r_round(right - S(32), cy, S(32), S(32), S(6), ARGB(C_SELECT));
    text_w(g_ui.f_icon, g_ui.hover_kind == HIT_LOGOUT ? C_INK : C_MUTED, rect(right - S(32), cy, S(32), S(32)),
           ICON_POWER, -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (g_ui.disconnected) {
        if (g_ui.hover_kind == HIT_RETRY)
            r_round(right - S(68), cy, S(32), S(32), S(6), ARGB(C_SELECT));
        text_w(g_ui.f_icon, C_AMBER, rect(right - S(68), cy, S(32), S(32)), ICON_REFRESH, -1,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static void paint_side(RECT rc)
{
    int x0 = S(RAIL_W), view = side_view(rc);

    fill(x0, 0, S(SIDE_W), rc.bottom, C_SIDE);

    unsigned first, count;

    if (side_range(&first, &count)) {
        int y = S(HEADER_H) + S(8) - g_ui.side_scroll;
        const char *title = g_ui.guild >= 0 ? model_str(g_ui.model, g_ui.model->guilds[g_ui.guild].name)
                                            : "Direct Messages";

        r_clip(x0, S(HEADER_H), S(SIDE_W), view);
        for (unsigned i = first; i < first + count; i++) {
            if (empty_category(first, count, i) || (chan((int)i)->type != CH_CATEGORY && hidden(first, i)))
                continue;
            if (y + row_height(i) > S(HEADER_H) && y < S(HEADER_H) + view)
                paint_channel_row(i, y);
            y += row_height(i);
        }
        paint_scrollbar(x0 + S(SIDE_W) - S(6), S(HEADER_H) + S(4), view - S(8), side_content(), g_ui.side_scroll);
        r_unclip();

        text(g_ui.f_h, C_INK, rect(x0 + S(16), 0, S(SIDE_W) - S(32), S(HEADER_H)), title,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (!count)
            text(g_ui.f_body, C_MUTED, rect(x0 + S(16), S(HEADER_H) + S(12), S(SIDE_W) - S(32), S(24)),
                 g_ui.guild >= 0 ? "No channels you can see" : "No conversations yet", DT_LEFT | DT_SINGLELINE);
    } else {
        text(g_ui.f_h, C_INK, rect(x0 + S(16), 0, S(SIDE_W) - S(32), S(HEADER_H)), "Direct Messages",
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    fill(x0, S(HEADER_H) - 1, S(SIDE_W), 1, C_LINE);
    paint_user_panel(rc);
}

/* ---- Messages ---- */

static void place_composer(void);
static void invalidate_views(void);
static void pop_close(void);
static void open_self(void);
static void build_name_fonts(void);
static void profiles_clear(void);
static void pop_place(void);
static void on_font(int id, sb_t *data);
static void on_profile(profile_t *p);

#define GROUP_MS (7 * 60 * 1000)
#define COMPOSER_H 44
#define WELCOME_H 190

static int is_voice_type(int type)
{
    return type == CH_VOICE || type == CH_STAGE;
}

static int open_is_text(void)
{
    return g_ui.model && g_ui.channel >= 0 && !is_voice_type(chan(g_ui.channel)->type);
}

/* Local SYSTEMTIME of a snowflake. */
static SYSTEMTIME local_time(const char *id)
{
    ULARGE_INTEGER t;
    FILETIME ft;
    SYSTEMTIME utc, local;

    t.QuadPart = ((unsigned long long)snowflake_ms(id) + 11644473600000ull) * 10000ull;
    ft.dwLowDateTime = t.LowPart;
    ft.dwHighDateTime = t.HighPart;
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(NULL, &utc, &local);
    return local;
}

static int same_day(SYSTEMTIME a, SYSTEMTIME b)
{
    return a.wYear == b.wYear && a.wMonth == b.wMonth && a.wDay == b.wDay;
}

/* "Today at 14:05", "Yesterday at 09:12" or "27/09/2026 14:05" in the user's locale. */
static void format_time(const char *id, wchar_t *out, int n)
{
    SYSTEMTIME st = local_time(id), now, yesterday;
    FILETIME ft;
    ULARGE_INTEGER t;
    wchar_t clock[32];
    int len = 0;

    GetLocalTime(&now);
    SystemTimeToFileTime(&now, &ft);
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    t.QuadPart -= 24ull * 3600 * 10000000;
    ft.dwLowDateTime = t.LowPart;
    ft.dwHighDateTime = t.HighPart;
    FileTimeToSystemTime(&ft, &yesterday);

    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, clock, ARRAYSIZE(clock));
    if (same_day(st, now)) {
        lstrcpynW(out, L"Today at ", n);
    } else if (same_day(st, yesterday)) {
        lstrcpynW(out, L"Yesterday at ", n);
    } else {
        len = GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, out, n, NULL);
        if (len > 0 && len < n) {
            out[len - 1] = L' ';
            out[len] = 0;
        }
    }
    len = lstrlenW(out);
    lstrcpynW(out + len, clock, n - len);
}

static RECT message_area(void)
{
    RECT rc, r;

    GetClientRect(g_ui.wnd, &rc);
    r.left = S(RAIL_W + SIDE_W);
    r.right = rc.right;
    r.top = S(HEADER_H);
    r.bottom = rc.bottom - S(24) - S(COMPOSER_H) - S(8);
    return r;
}

static int text_x(void)
{
    return S(RAIL_W + SIDE_W) + S(72);
}

static int text_w_px(void)
{
    RECT a = message_area();
    return a.right - S(24) - text_x();
}

static int text_height(const sb_t *s, int width)
{
    wchar_t *w;
    int h;

    if (!s->len)
        return 0;
    w = utf8_to_wide(s->data, s->len);
    h = r_text_height(g_ui.f_body, w, -1, width);
    mem_free(w);
    return h;
}

/* grouped: 0 = starts a group, 1 = continues it, 2 = starts a group after a date divider. */
static void update_grouping(void)
{
    for (int i = 0; i < g_ui.nmsgs; i++) {
        msg_t *m = &g_ui.msgs[i], *p = i ? &g_ui.msgs[i - 1] : NULL;
        SYSTEMTIME a, b;

        m->height_w = 0;
        if (!p) {
            m->grouped = 0;
            continue;
        }
        a = local_time(m->id);
        b = local_time(p->id);
        if (!same_day(a, b))
            m->grouped = 2;
        else if (!m->system && !p->system && !m->reply.len && lstrcmpA(m->author_id, p->author_id) == 0 &&
                 snowflake_ms(m->id) - snowflake_ms(p->id) < GROUP_MS)
            m->grouped = 1;
        else
            m->grouped = 0;
    }
}

/* ---- Server members ---- */

static member_t *member_find(const char *guild, const char *user)
{
    for (int i = 0; i < g_ui.nmembers; i++)
        if (lstrcmpA(g_ui.members[i].user, user) == 0 && lstrcmpA(g_ui.members[i].guild, guild) == 0)
            return &g_ui.members[i];
    return NULL;
}

static member_t *member_add(const char *guild, const char *user)
{
    member_t *mb = member_find(guild, user);

    if (mb)
        return mb;
    if (g_ui.nmembers == g_ui.cap_members) {
        g_ui.cap_members = g_ui.cap_members ? g_ui.cap_members * 2 : 64;
        g_ui.members = mem_realloc(g_ui.members, (size_t)g_ui.cap_members * sizeof *g_ui.members);
    }
    mb = &g_ui.members[g_ui.nmembers++];
    *mb = (member_t){0};
    lstrcpynA(mb->guild, guild, sizeof mb->guild);
    lstrcpynA(mb->user, user, sizeof mb->user);
    return mb;
}

static void members_clear(void)
{
    for (int i = 0; i < g_ui.nmembers; i++) {
        sb_free(&g_ui.members[i].nick);
        sb_free(&g_ui.members[i].roles);
    }
    g_ui.nmembers = 0;
}

/* Stores a member object ({user, nick, roles}) of guild `guild`. */
static void member_store(const char *guild, json_t obj)
{
    json_t user, v, roles, role;
    json_iter_t it;
    char id[24] = "";
    member_t *mb;

    if (!json_get(obj, "user", &user) || !json_get(user, "id", &v))
        return;
    json_raw(v, id, sizeof id);
    mb = member_add(guild, id);
    mb->known = 1;
    mb->color_model = NULL;
    sb_clear(&mb->nick);
    if (json_get(obj, "nick", &v) && json_type(v) == JSON_STRING)
        json_str(v, &mb->nick);
    sb_clear(&mb->roles);
    if (json_get(obj, "roles", &roles)) {
        json_iter(roles, &it);
        while (json_next(&it, NULL, &role)) {
            json_raw(role, id, sizeof id);
            if (mb->roles.len)
                sb_add(&mb->roles, ",");
            sb_add(&mb->roles, id);
        }
    }
}

static const char *open_guild_id(void)
{
    return g_ui.model && g_ui.guild >= 0 ? g_ui.model->guilds[g_ui.guild].id : NULL;
}

/* Role color of a message author in the open server, 0 for the default. */
static unsigned author_color(const msg_t *m)
{
    const char *guild = open_guild_id();
    member_t *mb;

    if (!guild || !m->author_id[0] || !(mb = member_find(guild, m->author_id)) || !mb->known)
        return 0;
    if (mb->color_model != g_ui.model) {
        mb->color = model_role_color(g_ui.model, g_ui.guild, mb->roles.data ? mb->roles.data : "");
        mb->color_model = g_ui.model;
    }
    return mb->color;
}

/* Name to show for an author: the server nickname once we know it. */
static const char *author_name(const msg_t *m)
{
    const char *guild = open_guild_id();
    member_t *mb;

    if (guild && m->author_id[0] && (mb = member_find(guild, m->author_id)) && mb->known && mb->nick.len)
        return mb->nick.data;
    return m->author.data ? m->author.data : "";
}

/* Asks the gateway for the authors of the loaded messages we know nothing about. */
static void request_authors(void)
{
    const char *guild = open_guild_id(), *ids[100];
    static char buf[100][24]; /* copies: adding members may move the cache */
    int n = 0;

    if (!guild)
        return;
    for (int i = 0; i < g_ui.nmsgs && n < 100; i++) {
        msg_t *m = &g_ui.msgs[i];
        member_t *mb;
        if (!m->author_id[0] || m->system || member_find(guild, m->author_id))
            continue;
        mb = member_add(guild, m->author_id);
        if (m->has_member) { /* the gateway gave us the roles already */
            mb->known = 1;
            sb_add(&mb->roles, m->member_roles.data ? m->member_roles.data : "");
            if (m->author.len)
                sb_add(&mb->nick, m->author.data);
            continue;
        }
        lstrcpynA(buf[n], m->author_id, sizeof buf[n]);
        ids[n] = buf[n];
        n++;
    }
    app_request_members(guild, ids, n);
}

/* ---- Attachments, embeds, stickers, reactions ---- */

#define MEDIA_MAX_W 550
#define MEDIA_MAX_H 350
#define EMBED_MAX_W 516
#define FILE_W 432
#define REACTION_H 28

enum { PART_NONE, PART_FILE, PART_MEDIA, PART_EMBED_TITLE, PART_REACTION, PART_SPOILER };

typedef struct {
    int kind, index;
} part_t;

/* Parsed markdown of embed descriptions and field values, kept with the message view. */
typedef struct {
    md_doc_t doc;
    r_rich_t *rich;
    int width;
} emb_text_t;

static emb_text_t *emb_text(msg_t *m, int slot, const sb_t *src, int width);

/* Custom emoji images for the rich text renderer. */
static r_image_t *emoji_image(const char *id, int px)
{
    char key[48], path[96];
    const char *num = id[0] == 'a' ? id + 1 : id;

    wsprintfA(key, "e:%.30s", num);
    wsprintfA(path, "/emojis/%.30s.png?size=64", num);
    return image_get(key, path, px);
}

/* Whether a URL is on one of Discord's resizing image proxies (*.discordapp.net). */
static int proxied(const char *url)
{
    const char *host = url + 8, *end = host;

    if (lstrlenA(url) < 9 || CompareStringA(LOCALE_INVARIANT, 0, url, 8, "https://", 8) != CSTR_EQUAL)
        return 0;
    while (*end && *end != '/')
        end++;
    return end - host > 15 &&
           CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, end - 15, 15, ".discordapp.net", 15) == CSTR_EQUAL;
}

/* Image of a message part; keyed by message, kind and index since URLs are long and signed. */
static r_image_t *part_image(const msg_t *m, char kind, int index, const sb_t *url, int w, int h)
{
    char key[96];
    sb_t full = {0};
    r_image_t *img;

    if (!url->len)
        return NULL;
    wsprintfA(key, "%c:%s:%d:%d", kind, m->id, index, w);
    /* The media proxy resizes on its side: ask for the size we show. */
    sb_addn(&full, url->data, url->len);
    if (proxied(url->data)) {
        char q[48];
        int query = 0;
        for (size_t k = 0; k < url->len; k++)
            query |= url->data[k] == '?';
        wsprintfA(q, "%cwidth=%d&height=%d", query ? '&' : '?', w, h);
        sb_add(&full, q);
    }
    img = image_get(key, full.data, w > h ? w : h);
    sb_free(&full);
    return img;
}

/* Fits (w, h) into (max_w, max_h) without enlarging. */
static void fit(int w, int h, int max_w, int max_h, int *ow, int *oh)
{
    if (w <= 0 || h <= 0) {
        *ow = max_w;
        *oh = max_h / 2;
        return;
    }
    if (w > max_w) {
        h = (int)((long long)h * max_w / w);
        w = max_w;
    }
    if (h > max_h) {
        w = (int)((long long)w * max_h / h);
        h = max_h;
    }
    *ow = w > 1 ? w : 1;
    *oh = h > 1 ? h : 1;
}

static void format_size(long long n, char *out)
{
    if (n >= 1024 * 1024)
        wsprintfA(out, "%d.%d MB", (int)(n / (1024 * 1024)), (int)(n % (1024 * 1024) * 10 / (1024 * 1024)));
    else if (n >= 1024)
        wsprintfA(out, "%d.%d KB", (int)(n / 1024), (int)(n % 1024 * 10 / 1024));
    else
        wsprintfA(out, "%d bytes", (int)n);
}

static int hit(int hx, int hy, int x, int y, int w, int h)
{
    return hx >= x && hx < x + w && hy >= y && hy < y + h;
}

static void draw_media(r_image_t *img, int x, int y, int w, int h, int spoiler)
{
    if (img)
        r_image(img, x, y, w, h, S(8));
    else
        r_round(x, y, w, h, S(8), 0xFF1E1E1E);
    if (spoiler) {
        r_round(x, y, w, h, S(8), 0xF2141414u);
        r_round(x + (w - S(76)) / 2, y + (h - S(28)) / 2, S(76), S(28), S(14), 0xFF000000u);
        text(g_ui.f_cat, C_INK, rect(x, y + (h - S(28)) / 2, w, S(28)), "SPOILER", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

/*
 * Walks what follows the text of message m, laid out at (x, y) in width w:
 * files, embeds, the sticker and reactions. Paints when `draw`, and reports
 * the part under (hx, hy) in `hit_part` when it is not NULL. Returns the height.
 */
static int msg_extras(msg_t *m, int x, int y, int w, int draw, int hx, int hy, part_t *hit_part)
{
    int y0 = y, gap = S(4);

    /* Attachments: images inline, other files as a card. */
    for (int i = 0; i < m->nfiles; i++) {
        msg_file_t *f = &m->files[i];
        y += gap;
        if (f->image) {
            int iw, ih;
            fit(f->width, f->height, w < S(MEDIA_MAX_W) ? w : S(MEDIA_MAX_W), S(MEDIA_MAX_H), &iw, &ih);
            if (draw && r_visible(y, ih))
                draw_media(part_image(m, 'f', i, &f->url, iw, ih), x, y, iw, ih, f->spoiler && !m->revealed);
            if (hit_part && hit(hx, hy, x, y, iw, ih))
                *hit_part = (part_t){f->spoiler && !m->revealed ? PART_SPOILER : PART_MEDIA, i};
            y += ih;
        } else {
            int cw = w < S(FILE_W) ? w : S(FILE_W), ch = S(64);
            if (draw && r_visible(y, ch)) {
                char size[32];
                r_round(x, y, cw, ch, S(8), 0xFF1B1B1B);
                r_round_outline(x, y, cw, ch, S(8), 1, 0xFF2A2A2A);
                text_w(g_ui.f_icon_big, C_MUTED, rect(x + S(12), y + S(12), S(40), S(40)), L"\xE8A5", -1,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                text(g_ui.f_body, C_INK, rect(x + S(60), y + S(12), cw - S(72), S(20)), f->name.data,
                     DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                format_size(f->size, size);
                text(g_ui.f_small, C_FAINT, rect(x + S(60), y + S(34), cw - S(72), S(18)), size, DT_LEFT | DT_SINGLELINE);
            }
            if (hit_part && hit(hx, hy, x, y, cw, ch))
                *hit_part = (part_t){PART_FILE, i};
            y += ch;
        }
    }

    /* Embeds. */
    for (int i = 0; i < m->nembeds; i++) {
        msg_embed_t *e = &m->embeds[i];
        y += gap;
        if (e->media_only) {
            int iw, ih;
            fit(e->image_w, e->image_h, w < S(MEDIA_MAX_W) ? w : S(MEDIA_MAX_W), S(MEDIA_MAX_H), &iw, &ih);
            if (draw && r_visible(y, ih))
                draw_media(part_image(m, 'm', i, &e->image, iw, ih), x, y, iw, ih, 0);
            if (hit_part && hit(hx, hy, x, y, iw, ih))
                *hit_part = (part_t){PART_EMBED_TITLE, i};
            y += ih;
            continue;
        }
        {
            int ew = w < S(EMBED_MAX_W) ? w : S(EMBED_MAX_W), top = y, pad = S(12), ix = x + S(4) + pad;
            int thumb = e->thumbnail.len ? S(80) : 0, iw = ew - S(4) - 2 * pad, tw = iw - (thumb ? thumb + S(16) : 0);
            int ey = y + pad, slot = i * 32;

            /* When painting, a first pass measures so the card can be drawn under the content. */
            for (int pass = draw ? 0 : 1; pass < 2; pass++) {
                ey = y + pad;
                if (e->provider.len) {
                    if (pass && draw)
                        text(g_ui.f_small, C_MUTED, rect(ix, ey, tw, S(16)), e->provider.data, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                    ey += S(16) + S(6);
                }
                if (e->author.len) {
                    if (pass && draw)
                        text(g_ui.f_h, C_INK, rect(ix, ey, tw, S(20)), e->author.data, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                    ey += S(20) + S(6);
                }
                if (e->title.len) {
                    wchar_t *wt = utf8_to_wide(e->title.data, e->title.len);
                    int th = r_text_height(g_ui.f_h, wt, -1, tw);
                    if (pass && draw)
                        r_text(g_ui.f_h, e->url.len ? 0xFF6CB6FFu : ARGB(C_INK), ix, ey, tw, th, wt, -1, R_WRAP);
                    if (pass && hit_part && e->url.len && hit(hx, hy, ix, ey, tw, th))
                        *hit_part = (part_t){PART_EMBED_TITLE, i};
                    mem_free(wt);
                    ey += th + S(6);
                }
                if (e->description.len) {
                    emb_text_t *d = emb_text(m, slot, &e->description, tw);
                    if (pass && draw)
                        r_rich_draw(d->rich, ix, ey, 1);
                    ey += r_rich_height(d->rich) + S(6);
                }
                /* Fields: up to three inline ones share a row. */
                for (int f = 0; f < e->nfields;) {
                    int n = 1, rowh = 0;
                    if (e->fields[f].inline_)
                        while (n < 3 && f + n < e->nfields && e->fields[f + n].inline_)
                            n++;
                    for (int k = 0; k < n; k++) {
                        msg_field_t *fd = &e->fields[f + k];
                        int cw = (iw - S(8) * (n - 1)) / n, cx = ix + k * (cw + S(8)), h = S(20);
                        emb_text_t *v = emb_text(m, slot + 1 + f + k, &fd->value, cw);
                        if (pass && draw) {
                            text(g_ui.f_h, C_INK, rect(cx, ey, cw, S(20)), fd->name.data ? fd->name.data : "",
                                 DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                            r_rich_draw(v->rich, cx, ey + S(20), 1);
                        }
                        h += r_rich_height(v->rich);
                        if (h > rowh)
                            rowh = h;
                    }
                    ey += rowh + S(8);
                    f += n;
                }
                if (thumb && ey < y + pad + thumb + S(6))
                    ey = y + pad + thumb + S(6);
                if (e->image.len) {
                    int mw, mh;
                    fit(e->image_w, e->image_h, iw, S(300), &mw, &mh);
                    if (pass && draw && r_visible(ey, mh))
                        draw_media(part_image(m, 'i', i, &e->image, mw, mh), ix, ey, mw, mh, 0);
                    if (pass && hit_part && hit(hx, hy, ix, ey, mw, mh))
                        *hit_part = (part_t){PART_EMBED_TITLE, i};
                    ey += mh + S(8);
                }
                if (e->footer.len) {
                    if (pass && draw)
                        text(g_ui.f_small, C_MUTED, rect(ix, ey, iw, S(16)), e->footer.data, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                    ey += S(16) + S(6);
                }
                ey += pad - S(6);
                if (!pass && draw) {
                    r_round(x, top, ew, ey - top, S(4), 0xFF1B1B1B);
                    r_round(x, top, S(4), ey - top, S(2), e->has_color ? 0xFF000000u | e->color : 0xFF3A3A3Au);
                    if (thumb) {
                        int mw, mh;
                        fit(e->thumb_w, e->thumb_h, thumb, thumb, &mw, &mh);
                        draw_media(part_image(m, 't', i, &e->thumbnail, mw, mh), x + ew - pad - mw, y + pad, mw, mh, 0);
                    }
                }
            }
            y = ey;
        }
    }

    /* Sticker. */
    if (m->sticker_id[0]) {
        y += gap;
        if (m->sticker_format == 3) { /* Lottie animation: only the name */
            if (draw)
                text(g_ui.f_small, C_MUTED, rect(x, y, w, S(18)), m->sticker_name.data ? m->sticker_name.data : "Sticker",
                     DT_LEFT | DT_SINGLELINE);
            y += S(18);
        } else {
            if (draw && r_visible(y, S(160))) {
                char key[48], path[128];
                r_image_t *img;
                wsprintfA(key, "st:%s", m->sticker_id);
                if (m->sticker_format == 4)
                    wsprintfA(path, "https://media.discordapp.net/stickers/%s.gif?size=160", m->sticker_id);
                else
                    wsprintfA(path, "/stickers/%s.png?size=160", m->sticker_id);
                img = image_get(key, path, S(160));
                if (img)
                    r_image(img, x, y, S(160), S(160), 0);
            }
            y += S(160);
        }
    }

    /* Reactions: pills of emoji and count, ours highlighted. */
    if (m->nreactions) {
        int rx = x;
        y += S(6);
        for (int i = 0; i < m->nreactions; i++) {
            msg_reaction_t *r = &m->reactions[i];
            char count[16];
            int cw, pw;
            wsprintfA(count, "%d", r->count);
            cw = text_width(g_ui.f_small, count);
            pw = S(8) + S(18) + S(6) + cw + S(8);
            if (rx + pw > x + w && rx > x) {
                rx = x;
                y += S(REACTION_H) + S(4);
            }
            if (draw && r_visible(y, S(REACTION_H))) {
                r_round(rx, y, pw, S(REACTION_H), S(8), r->me ? 0x33FFB000u : 0xFF1E1E1E);
                if (r->me)
                    r_round_outline(rx, y, pw, S(REACTION_H), S(8), 1, ARGB(C_AMBER));
                if (r->emoji_id[0]) {
                    r_image_t *img = emoji_image(r->emoji_id, S(18));
                    if (img)
                        r_image(img, rx + S(8), y + (S(REACTION_H) - S(18)) / 2, S(18), S(18), 0);
                } else {
                    text(g_ui.f_body, C_INK, rect(rx + S(6), y, S(24), S(REACTION_H)), r->emoji.data ? r->emoji.data : "",
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
                text(g_ui.f_small, r->me ? C_AMBER : C_MUTED, rect(rx + S(32), y, cw + S(4), S(REACTION_H)), count,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            if (hit_part && hit(hx, hy, rx, y, pw, S(REACTION_H)))
                *hit_part = (part_t){PART_REACTION, i};
            rx += pw + S(4);
        }
        y += S(REACTION_H);
    }
    return y - y0;
}

typedef struct {
    md_doc_t doc;
    r_rich_t *rich;
    int width;
    emb_text_t *emb;   /* embed descriptions and field values, 32 slots per embed */
    int nemb;
} msg_view_t;

static void drop_view(msg_t *m)
{
    msg_view_t *v = m->ui;

    if (!v)
        return;
    r_rich_free(v->rich);
    md_free(&v->doc);
    for (int i = 0; i < v->nemb; i++) {
        r_rich_free(v->emb[i].rich);
        md_free(&v->emb[i].doc);
    }
    mem_free(v->emb);
    mem_free(v);
    m->ui = NULL;
    m->height_w = 0;
}

static void ui_msg_free(msg_t *m)
{
    drop_view(m);
    msg_free(m);
}

static void invalidate_views(void)
{
    for (int i = 0; i < g_ui.nmsgs; i++)
        drop_view(&g_ui.msgs[i]);
}

/* Parsed and laid-out body of a message, rebuilt when the width changes. */
static r_rich_t *msg_rich(msg_t *m, int width)
{
    msg_view_t *v = m->ui;

    if (!v) {
        sb_t src = {0};
        v = mem_alloc(sizeof *v);
        sb_addn(&src, m->text.data ? m->text.data : "", m->text.len);
        if (m->edited)
            sb_add(&src, m->text.len ? " " MD_EDITED_MARK : MD_EDITED_MARK);
        md_parse(src.data ? src.data : "", src.len, &v->doc);
        sb_free(&src);
        m->ui = v;
    }
    if (!v->rich || v->width != width) {
        r_rich_free(v->rich);
        v->rich = r_rich_build(&v->doc, &g_ui.rich, width);
        v->width = width;
    }
    return v->rich;
}

static emb_text_t *emb_text(msg_t *m, int slot, const sb_t *src, int width)
{
    msg_view_t *v;
    emb_text_t *t;

    msg_rich(m, text_w_px()); /* makes sure the view exists */
    v = m->ui;
    if (!v->emb) {
        v->nemb = m->nembeds * 32;
        v->emb = mem_alloc(((size_t)v->nemb + 1) * sizeof *v->emb);
    }
    t = &v->emb[slot < v->nemb ? slot : v->nemb - 1];
    if (!t->doc.text && src->len)
        md_parse(src->data, src->len, &t->doc);
    if (!t->rich || t->width != width) {
        r_rich_free(t->rich);
        t->rich = r_rich_build(&t->doc, &g_ui.rich, width);
        t->width = width;
    }
    return t;
}

static int msg_height(msg_t *m)
{
    int w = text_w_px();

    if (m->height_w != w) {
        int h = m->text.len || m->edited ? r_rich_height(msg_rich(m, w)) : 0;
        h += msg_extras(m, 0, 0, w, 0, 0, 0, NULL);
        if (m->system)
            h = S(16) + S(22);
        else if (m->grouped == 1)
            h = S(2) + (h ? h : S(20)) + S(2);
        else
            h = S(16) + (m->reply.len ? S(22) : 0) + S(22) + h + S(2);
        if (m->grouped == 2)
            h += S(44);
        m->height = h;
        m->height_w = w;
    }
    return m->height;
}

static int messages_height(void)
{
    int h = S(16);

    for (int i = 0; i < g_ui.nmsgs; i++)
        h += msg_height(&g_ui.msgs[i]);
    if (!g_ui.msgs_has_more)
        h += S(WELCOME_H);
    return h;
}

static void clamp_msg_scroll(void)
{
    RECT a = message_area();
    int max = messages_height() - (a.bottom - a.top);

    if (g_ui.msg_scroll > max)
        g_ui.msg_scroll = max;
    if (g_ui.msg_scroll < 0)
        g_ui.msg_scroll = 0;
}

static void maybe_load_older(void)
{
    RECT a = message_area();

    if (!g_ui.msgs_has_more || g_ui.msgs_loading || g_ui.msgs_older_loading || !g_ui.nmsgs)
        return;
    /* Fetch before the user reaches the top. */
    if (messages_height() - (a.bottom - a.top) - g_ui.msg_scroll < S(600)) {
        g_ui.msgs_older_loading = 1;
        app_fetch_messages(g_ui.msgs_channel, g_ui.msgs[0].id);
    }
}

/* Replaces <#id> with #name using the channel list. */
static void resolve_channels(msg_t *m)
{
    sb_t out = {0};
    const char *s = m->text.data;
    size_t n = m->text.len, i = 0;
    int changed = 0;

    while (i < n) {
        if (s[i] == '<' && i + 2 < n && s[i + 1] == '#') {
            size_t j = i + 2;
            while (j < n && s[j] >= '0' && s[j] <= '9')
                j++;
            if (j < n && s[j] == '>' && j - i - 2 < 24) {
                char id[24];
                int found = 0;
                lstrcpynA(id, s + i + 2, (int)(j - i - 1));
                for (unsigned c = 0; g_ui.model && c < g_ui.model->nchannels; c++)
                    if (lstrcmpA(g_ui.model->channels[c].id, id) == 0) {
                        sb_add(&out, MD_MENTION_OPEN "#");
                        sb_add(&out, model_str(g_ui.model, g_ui.model->channels[c].name));
                        sb_add(&out, MD_MENTION_CLOSE);
                        found = 1;
                        break;
                    }
                if (!found)
                    sb_add(&out, "#unknown");
                i = j + 1;
                changed = 1;
                continue;
            }
        }
        sb_addn(&out, s + i, 1);
        i++;
    }
    if (changed) {
        sb_free(&m->text);
        m->text = out;
    } else {
        sb_free(&out);
    }
}

static void free_messages(void)
{
    for (int i = 0; i < g_ui.nmsgs; i++)
        ui_msg_free(&g_ui.msgs[i]);
    g_ui.nmsgs = 0;
}

static int find_msg(const char *id)
{
    for (int i = g_ui.nmsgs; i-- > 0;)
        if (lstrcmpA(g_ui.msgs[i].id, id) == 0)
            return i;
    return -1;
}

static void reserve_msgs(int extra)
{
    if (g_ui.nmsgs + extra <= g_ui.cap_msgs)
        return;
    while (g_ui.cap_msgs < g_ui.nmsgs + extra)
        g_ui.cap_msgs = g_ui.cap_msgs ? g_ui.cap_msgs * 2 : 128;
    g_ui.msgs = mem_realloc(g_ui.msgs, (size_t)g_ui.cap_msgs * sizeof *g_ui.msgs);
}

static int same_reaction(const msg_reaction_t *a, const msg_reaction_t *b)
{
    if (a->emoji_id[0] || b->emoji_id[0])
        return lstrcmpA(a->emoji_id, b->emoji_id) == 0;
    return a->emoji.len == b->emoji.len && a->emoji.data && b->emoji.data && lstrcmpA(a->emoji.data, b->emoji.data) == 0;
}

/* Counts a reaction added (+1) or removed (-1). Our own ones were counted when clicked. */
static void apply_reaction(msg_t *m, const msg_reaction_t *r, int delta, int mine)
{
    int k;

    for (k = 0; k < m->nreactions && !same_reaction(&m->reactions[k], r); k++)
        ;
    if (mine && (k < m->nreactions ? m->reactions[k].me : 0) == (delta > 0))
        return; /* already applied */
    if (k == m->nreactions) {
        if (delta < 0)
            return;
        m->reactions = mem_realloc(m->reactions, ((size_t)m->nreactions + 1) * sizeof *m->reactions);
        m->reactions[k] = (msg_reaction_t){0};
        lstrcpynA(m->reactions[k].emoji_id, r->emoji_id, sizeof r->emoji_id);
        sb_addn(&m->reactions[k].emoji, r->emoji.data ? r->emoji.data : "", r->emoji.len);
        m->nreactions++;
    }
    m->reactions[k].count += delta;
    if (mine)
        m->reactions[k].me = delta > 0;
    if (m->reactions[k].count <= 0) {
        sb_free(&m->reactions[k].emoji);
        memmove(m->reactions + k, m->reactions + k + 1, (size_t)(m->nreactions - k - 1) * sizeof *m->reactions);
        m->nreactions--;
    }
    m->height_w = 0;
}

static void on_batch(msg_batch_t *b)
{
    if (lstrcmpA(b->channel_id, g_ui.msgs_channel) != 0) {
        msg_batch_free(b);
        return;
    }
    for (int i = 0; i < b->n; i++)
        resolve_channels(&b->msgs[i]);

    switch (b->kind) {
    case BATCH_HISTORY:
        free_messages();
        g_ui.msgs_loading = 0;
        g_ui.msgs_status = b->status;
        g_ui.msgs_has_more = b->has_more;
        g_ui.msg_scroll = 0;
        g_ui.log_memory = 1; /* after the next paint lays the messages out */
        reserve_msgs(b->n);
        for (int i = 0; i < b->n; i++)
            g_ui.msgs[g_ui.nmsgs++] = b->msgs[i];
        b->n = 0;
        break;
    case BATCH_OLDER:
        g_ui.msgs_older_loading = 0;
        if (b->status || !g_ui.nmsgs || lstrcmpA(b->before, g_ui.msgs[0].id) != 0)
            break;
        g_ui.msgs_has_more = b->has_more;
        reserve_msgs(b->n);
        memmove(g_ui.msgs + b->n, g_ui.msgs, (size_t)g_ui.nmsgs * sizeof *g_ui.msgs);
        for (int i = 0; i < b->n; i++)
            g_ui.msgs[i] = b->msgs[i];
        g_ui.nmsgs += b->n;
        b->n = 0;
        break;
    case BATCH_NEW:
        if (g_ui.msgs_loading || find_msg(b->msgs[0].id) >= 0)
            break;
        reserve_msgs(1);
        g_ui.msgs[g_ui.nmsgs++] = b->msgs[0];
        b->n = 0;
        break;
    case BATCH_UPDATE: {
        int i = find_msg(b->msgs[0].id);
        msg_t *n = &b->msgs[0];
        if (i < 0)
            break;
        drop_view(&g_ui.msgs[i]);
        /* Partial updates (embeds resolving) carry no author: keep the text we have. */
        if (n->author_id[0]) {
            sb_free(&g_ui.msgs[i].text);
            g_ui.msgs[i].text = n->text;
            n->text = (sb_t){0};
            g_ui.msgs[i].edited = n->edited;
            msg_free_extras(&g_ui.msgs[i]);
        } else if (n->nembeds) {
            for (int k = 0; k < g_ui.msgs[i].nembeds; k++)
                msg_embed_free(&g_ui.msgs[i].embeds[k]);
            mem_free(g_ui.msgs[i].embeds);
            g_ui.msgs[i].embeds = NULL;
            g_ui.msgs[i].nembeds = 0;
        }
        if (n->author_id[0] || n->nembeds) {
            /* Take whatever parts the update carries. */
            if (n->nfiles) {
                g_ui.msgs[i].files = n->files;
                g_ui.msgs[i].nfiles = n->nfiles;
                n->files = NULL;
                n->nfiles = 0;
            }
            if (n->nembeds) {
                g_ui.msgs[i].embeds = n->embeds;
                g_ui.msgs[i].nembeds = n->nembeds;
                n->embeds = NULL;
                n->nembeds = 0;
            }
            if (n->nreactions) {
                g_ui.msgs[i].reactions = n->reactions;
                g_ui.msgs[i].nreactions = n->nreactions;
                n->reactions = NULL;
                n->nreactions = 0;
            }
            if (n->sticker_id[0]) {
                lstrcpynA(g_ui.msgs[i].sticker_id, n->sticker_id, sizeof n->sticker_id);
                g_ui.msgs[i].sticker_format = n->sticker_format;
                g_ui.msgs[i].sticker_name = n->sticker_name;
                n->sticker_name = (sb_t){0};
            }
        }
        break;
    }
    case BATCH_REACTION: {
        int i = find_msg(b->msgs[0].id);
        if (i >= 0)
            apply_reaction(&g_ui.msgs[i], &b->msgs[0].reactions[0], b->delta, b->mine);
        break;
    }
    case BATCH_DELETE: {
        int i = find_msg(b->msgs[0].id);
        if (i >= 0) {
            ui_msg_free(&g_ui.msgs[i]);
            memmove(g_ui.msgs + i, g_ui.msgs + i + 1, (size_t)(g_ui.nmsgs - i - 1) * sizeof *g_ui.msgs);
            g_ui.nmsgs--;
        }
        break;
    }
    }
    msg_batch_free(b);
    request_authors();
    update_grouping();
    clamp_msg_scroll();
    maybe_load_older();
    place_composer();
}

static void paint_welcome(int x0, int y, int w, const char *name, int voice)
{
    char title[160];
    const channel_t *c = g_ui.channel >= 0 ? chan(g_ui.channel) : NULL;

    if (c && is_dm_type(c->type)) {
        r_image_t *img = dm_icon(c);
        if (img)
            r_image(img, x0 + S(16), y, S(68), S(68), S(34));
        else
            r_circle(x0 + S(16), y, S(68), ARGB(C_ITEM));
        text(g_ui.f_title, C_INK, rect(x0 + S(16), y + S(84), w - S(32), S(32)), name,
             DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        wsprintfA(title, c->type == CH_DM ? "This is the beginning of your direct message history with %.120s."
                                          : "Welcome to the beginning of the %.120s group.", name);
        text(g_ui.f_body, C_MUTED, rect(x0 + S(16), y + S(122), w - S(32), S(24)), title,
             DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }
    r_circle(x0 + S(16), y, S(68), ARGB(C_ITEM));
    if (voice)
        text_w(g_ui.f_icon_big, C_INK, rect(x0 + S(16), y, S(68), S(68)), ICON_VOLUME, -1,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    else
        text(g_ui.f_title, C_INK, rect(x0 + S(16), y, S(68), S(68)), "#", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    wsprintfA(title, "Welcome to %s%.120s", voice ? "" : "#", name);
    text(g_ui.f_title, C_INK, rect(x0 + S(16), y + S(84), w - S(32), S(32)), title,
         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    wsprintfA(title, voice ? "Voice channels are not supported yet." : "This is the start of the #%.120s channel.", name);
    text(g_ui.f_body, C_MUTED, rect(x0 + S(16), y + S(122), w - S(32), S(24)), title,
         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void paint_divider(int x0, int y, int w, const char *id)
{
    SYSTEMTIME st = local_time(id);
    wchar_t date[64];
    RECT r;
    int tw;

    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &st, NULL, date, ARRAYSIZE(date), NULL);
    (void)r;
    tw = r_text_width(g_ui.f_cat, date, -1) + S(16);
    fill(x0 + S(16), y + S(22), w - S(32), 1, C_LINE);
    fill(x0 + (w - tw) / 2, y + S(12), tw, S(20), C_MAIN);
    text_w(g_ui.f_cat, C_FAINT, rect(x0 + (w - tw) / 2, y + S(12), tw, S(20)), date, -1,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void paint_message(int i, int x0, int y, int w)
{
    msg_t *m = &g_ui.msgs[i];
    int tx = text_x(), tw = w - (tx - x0) - S(24), h = msg_height(m);

    if (m->grouped == 2) {
        paint_divider(x0, y, w, m->id);
        y += S(44);
        h -= S(44);
    }
    if (g_ui.hover_msg == i)
        fill(x0, y + (m->grouped == 1 ? 0 : S(12)), w, h - (m->grouped == 1 ? 0 : S(12)), C_HOVER);

    if (m->system) {
        char line[160];
        text(g_ui.f_body, C_GREEN, rect(x0 + S(16), y + S(16), S(40), S(22)), "\xE2\x86\x92", DT_CENTER | DT_SINGLELINE);
        wsprintfA(line, "%.60s %.90s", m->author.data ? m->author.data : "", m->text.data ? m->text.data : "");
        text(g_ui.f_body, C_MUTED, rect(tx, y + S(16), tw, S(22)), line, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }
    if (m->grouped != 1) {
        wchar_t when[64];
        r_image_t *img;
        int ny = y + S(16);
        RECT nr;

        if (m->reply.len) {
            fill(x0 + S(36), ny + S(10), S(2), S(14), C_LINE);
            fill(x0 + S(36), ny + S(10), S(26), S(2), C_LINE);
            text(g_ui.f_small, C_MUTED, rect(tx, ny, tw, S(20)), m->reply.data, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            ny += S(22);
        }
        img = m->author_id[0] ? user_avatar(m->author_id, m->avatar) : NULL;
        if (img)
            r_image(img, x0 + S(16), ny, S(40), S(40), S(20));
        else
            r_circle(x0 + S(16), ny, S(40), ARGB(C_ITEM));
        nr = rect(tx, ny, tw, S(22));
        {
            unsigned color = author_color(m);
            wchar_t *wn = utf8_to_wide(author_name(m), lstrlenA(author_name(m)));
            r_text(g_ui.f_h, color ? 0xFF000000u | color : ARGB(C_INK), nr.left, nr.top, nr.right - nr.left,
                   nr.bottom - nr.top, wn, -1, R_LEFT | R_SINGLE | R_ELLIPSIS);
            mem_free(wn);
        }
        format_time(m->id, when, ARRAYSIZE(when));
        nr.left += text_width(g_ui.f_h, author_name(m)) + S(10);
        text_w(g_ui.f_small, C_FAINT, rect(nr.left, ny + S(3), tw, S(18)), when, -1, DT_LEFT | DT_SINGLELINE);
        y = ny + S(22);
    } else {
        if (g_ui.hover_msg == i) {
            SYSTEMTIME st = local_time(m->id);
            wchar_t clock[16];
            GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, clock, ARRAYSIZE(clock));
            text_w(g_ui.f_small, C_FAINT, rect(x0, y + S(3), S(64), S(18)), clock, -1, DT_CENTER | DT_SINGLELINE);
        }
        y += S(2);
    }
    if (m->text.len || m->edited) {
        r_rich_t *r = msg_rich(m, tw);
        r_rich_draw(r, tx, y, m->revealed);
        y += r_rich_height(r);
    }
    msg_extras(m, tx, y, tw, 1, 0, 0, NULL);
}

static void paint_messages(RECT rc, const char *name)
{
    RECT a = message_area();
    int x0 = a.left, w = a.right - a.left;
    int y = a.bottom + g_ui.msg_scroll - S(16);

    if (g_ui.msgs_loading && !g_ui.nmsgs) {
        text(g_ui.f_body, C_MUTED, a, "Loading messages\xE2\x80\xA6", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    if (g_ui.msgs_status) {
        text(g_ui.f_body, C_MUTED, a,
             g_ui.msgs_status == 403 ? "You do not have access to this channel." : "Could not load the messages.",
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    (void)rc;
    r_clip(a.left, a.top, a.right - a.left, a.bottom - a.top);

    for (int i = g_ui.nmsgs; i-- > 0;) {
        int h = msg_height(&g_ui.msgs[i]);
        y -= h;
        if (y + h < a.top)
            break;
        if (y < a.bottom && r_visible(y, h))
            paint_message(i, x0, y, w);
    }
    if (!g_ui.msgs_has_more && y > a.top - S(WELCOME_H))
        paint_welcome(x0, y - S(WELCOME_H) + S(24), w, name, 0);

    r_unclip();

    /* Scrollbar */
    {
        int content = messages_height(), view = a.bottom - a.top;
        if (content > view) {
            int th = view * view / content, ty;
            if (th < S(32))
                th = S(32);
            ty = a.top + (view - th) - (view - th) * g_ui.msg_scroll / (content - view);
            r_round(a.right - S(10), ty, S(6), th, S(3), 0xFF2A2A2A);
        }
    }
}

static void paint_main(RECT rc)
{
    int x0 = S(RAIL_W + SIDE_W), w = rc.right - x0;

    fill(x0, 0, w, rc.bottom, C_MAIN);
    fill(x0, S(HEADER_H) - 1, w, 1, C_LINE);

    if (g_ui.model && g_ui.channel >= 0) {
        const channel_t *c = chan(g_ui.channel);
        const char *name = model_str(g_ui.model, c->name);
        int voice = is_voice_type(c->type);

        if (voice)
            text_w(g_ui.f_icon, C_FAINT, rect(x0 + S(16), 0, S(24), S(HEADER_H)), ICON_VOLUME, -1,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        else
            text(g_ui.f_title, C_FAINT, rect(x0 + S(16), 0, S(24), S(HEADER_H)), is_dm_type(c->type) ? "@" : "#",
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(g_ui.f_h, C_INK, rect(x0 + S(46), 0, w - S(62), S(HEADER_H)), name,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (voice) {
            paint_welcome(x0 + S(8), rc.bottom - S(24) - S(WELCOME_H), w, name, 1);
            return;
        }
        paint_messages(rc, name);

        /* Composer frame; the edit control sits inside it. */
        {
            int cy = rc.bottom - S(24) - S(COMPOSER_H);
            r_round(x0 + S(16), cy, w - S(32), S(COMPOSER_H), S(10), 0xFF1F1F1F);
            if (g_ui.send_error.len)
                text(g_ui.f_small, C_AMBER, rect(x0 + S(20), cy - S(20), w - S(40), S(18)), g_ui.send_error.data,
                     DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    } else {
        int unit = S(4);
        int y = rc.bottom / 2 - S(60);
        draw_wordmark(x0 + (w - wordmark_width(unit)) / 2, y, unit);
        text(g_ui.f_h, C_INK, rect(x0, y + S(56), w, S(24)),
             !g_ui.model ? "Connecting\xE2\x80\xA6" : g_ui.guild < 0 ? "Pick a conversation or a server" : "Pick a channel",
             DT_CENTER | DT_SINGLELINE);
        text(g_ui.f_body, C_MUTED, rect(x0, y + S(84), w, S(24)),
             "Native, tiny, and asleep until something happens.", DT_CENTER | DT_SINGLELINE);
    }
}

/* Hit test for message hover: index of the message under y, or -1. */
static int message_at(int x, int y, int *top)
{
    RECT a = message_area();
    int yy = a.bottom + g_ui.msg_scroll - S(16);

    if (!open_is_text() || x < a.left || y < a.top || y >= a.bottom)
        return -1;
    for (int i = g_ui.nmsgs; i-- > 0;) {
        int h = msg_height(&g_ui.msgs[i]);
        yy -= h;
        if (y >= yy && y < yy + h) {
            if (top)
                *top = yy;
            return i;
        }
        if (yy < a.top)
            break;
    }
    return -1;
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
    r_round(S(RAIL_W) + S(4), y, tw, th, S(6), ARGB(C_TIP));
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

static void paint(HWND wnd)
{
    PAINTSTRUCT ps;
    RECT rc;
    HDC dc = BeginPaint(wnd, &ps);

    GetClientRect(wnd, &rc);
    g_ui.frame++;
    QueryPerformanceCounter(&g_ui.frame_start);
    while (rc.right > 0 && rc.bottom > 0 && r_begin(dc, rc.right, rc.bottom)) {
        if (g_ui.view == VIEW_APP)
            paint_app(rc);
        else if (g_ui.view == VIEW_LOADING)
            paint_loading(rc);
        else
            paint_login(rc);
        r_end(dc);
    }
    EndPaint(wnd, &ps);
    /* The popout is painted separately: what it shows was drawn at its last paint. */
    images_trim(g_ui.pop && (int)(g_ui.pop_frame - g_ui.frame) < 0 ? g_ui.pop_frame : g_ui.frame);
    if (g_ui.log_memory && g_ui.nmsgs) {
        char when[64];
        g_ui.log_memory = 0;
        wsprintfA(when, "%d messages shown", g_ui.nmsgs);
        log_memory(when);
    }
}

static void redraw(void)
{
    InvalidateRect(g_ui.wnd, NULL, FALSE);
    if (g_ui.pop)
        InvalidateRect(g_ui.pop, NULL, FALSE); /* images it waits for arrive through the main window */
}

/* ---- Fonts, icon ---- */

static void make_fonts(void)
{
    r_font_t **f[] = {&g_ui.f_title, &g_ui.f_h, &g_ui.f_body, &g_ui.f_small, &g_ui.f_cat, &g_ui.f_icon,
                      &g_ui.f_icon_big, &g_ui.f_initial, &g_ui.f_initial_small, &g_ui.f_mono, &g_ui.f_h1,
                      &g_ui.f_h2, &g_ui.f_h3, &g_ui.f_name};

    invalidate_views(); /* message layouts point at the old fonts */
    pop_close();
    HFONT old = g_ui.composer ? (HFONT)SendMessageW(g_ui.composer, WM_GETFONT, 0, 0) : NULL;

    for (int i = 0; i < (int)ARRAYSIZE(f); i++)
        r_font_free(*f[i]);
    g_ui.f_title = r_font(L"Segoe UI", S(22), FW_SEMIBOLD, 0);
    g_ui.f_h = r_font(L"Segoe UI", S(15), FW_SEMIBOLD, 0);
    g_ui.f_body = r_font(L"Segoe UI", S(15), FW_NORMAL, 0);
    g_ui.f_small = r_font(L"Segoe UI", S(13), FW_NORMAL, 0);
    g_ui.f_cat = r_font(L"Segoe UI", S(12), FW_BOLD, 0);
    g_ui.f_icon = r_font(L"Segoe MDL2 Assets", S(14), FW_NORMAL, 0);
    g_ui.f_icon_big = r_font(L"Segoe MDL2 Assets", S(32), FW_NORMAL, 0);
    g_ui.f_initial = r_font(L"Segoe UI", S(17), FW_SEMIBOLD, 0);
    g_ui.f_initial_small = r_font(L"Segoe UI", S(13), FW_SEMIBOLD, 0);
    g_ui.f_mono = r_font(L"Consolas", S(14), FW_NORMAL, 0);
    g_ui.f_h1 = r_font(L"Segoe UI", S(24), FW_BOLD, 0);
    g_ui.f_h2 = r_font(L"Segoe UI", S(20), FW_BOLD, 0);
    g_ui.f_h3 = r_font(L"Segoe UI", S(17), FW_BOLD, 0);
    g_ui.f_name = r_font(L"Segoe UI", S(20), FW_BOLD, 0);
    build_name_fonts();

    g_ui.rich = (r_rich_style_t){
        .body = g_ui.f_body, .mono = g_ui.f_mono, .h1 = g_ui.f_h1, .h2 = g_ui.f_h2, .h3 = g_ui.f_h3,
        .subtext = g_ui.f_small,
        .ink = ARGB(C_INK), .muted = ARGB(C_MUTED), .link = 0xFF6CB6FFu, .mention = 0xFFFFC857u,
        .mention_bg = 0x33FFB000u, .code_bg = 0xFF0F0F0Fu, .quote_bar = 0xFF3A3A3Au, .spoiler = 0xFF2E2E2Eu,
        .quote_indent = S(16), .code_pad = S(8), .block_gap = S(4), .radius = S(6),
        .emoji_px = S(22), .jumbo_px = S(48), .emoji = emoji_image,
    };

    /* The composer is a real EDIT control: it keeps a GDI font. */
    if (g_ui.composer) {
        HFONT font = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        SendMessageW(g_ui.composer, WM_SETFONT, (WPARAM)font, TRUE);
        if (old)
            DeleteObject(old);
    }
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

/* ---- Read markers and notifications ---- */

static int window_active(void)
{
    return GetForegroundWindow() == g_ui.wnd && !IsIconic(g_ui.wnd);
}

/* Marks channel i read locally and tells Discord if something was unread. */
static void mark_read(int i)
{
    channel_t *c = &g_ui.model->channels[i];
    int was_unread = model_unread(g_ui.model, (unsigned)i) || c->mentions;

    if (c->last_message[0])
        lstrcpynA(c->read, c->last_message, sizeof c->read);
    c->mentions = 0;
    if (was_unread && c->last_message[0])
        app_ack(c->id, c->last_message);
    update_title();
}

static void utf8_to_buf(const char *s, size_t n, wchar_t *out, int size)
{
    wchar_t *w = utf8_to_wide(s ? s : "", s ? n : 0);

    lstrcpynW(out, w, size);
    mem_free(w);
}

static void notify(int i, const activity_t *a)
{
    const channel_t *c = chan(i);
    int g = model_channel_guild(g_ui.model, (unsigned)i);
    const char *author = a->author.data ? a->author.data : "";
    char title[200];
    FLASHWINFO fw = {sizeof fw, g_ui.wnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0};

    if (g >= 0)
        wsprintfA(title, "%.60s (#%.60s, %.60s)", author, model_str(g_ui.model, c->name),
                  model_str(g_ui.model, g_ui.model->guilds[g].name));
    else if (c->type == CH_GROUP_DM)
        wsprintfA(title, "%.60s (%.100s)", author, model_str(g_ui.model, c->name));
    else
        wsprintfA(title, "%.60s", author);

    g_ui.tray.uFlags = NIF_INFO;
    utf8_to_buf(title, (size_t)lstrlenA(title), g_ui.tray.szInfoTitle, ARRAYSIZE(g_ui.tray.szInfoTitle));
    if (a->preview.len) {
        wchar_t *src = g_ui.tray.szInfo, *dst = g_ui.tray.szInfo;
        utf8_to_buf(a->preview.data, a->preview.len, g_ui.tray.szInfo, ARRAYSIZE(g_ui.tray.szInfo));
        for (; *src; src++) {
            if (*src == 0xE002) { /* custom emoji: keep ":name:" */
                while (src[1] && src[1] != ':' && src[1] != 0xE003)
                    src++;
                *dst++ = ':';
                if (src[1] == ':')
                    src++;
                continue;
            }
            if (*src == 0xE003) {
                *dst++ = ':';
                continue;
            }
            if (*src != 0xE000 && *src != 0xE001)
                *dst++ = *src;
        }
        *dst = 0;
    }
    else
        lstrcpyW(g_ui.tray.szInfo, L"Sent a message");
    g_ui.tray.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    g_ui.tray.hBalloonIcon = g_ui.icon_big;
    Shell_NotifyIconW(NIM_MODIFY, &g_ui.tray);
    g_ui.notified_channel = i;
    if (!window_active())
        FlashWindowEx(&fw);
}

static void on_activity(activity_t *a)
{
    channel_t *c;
    int i;

    if (!g_ui.model)
        return;
    if ((i = model_find_channel(g_ui.model, a->channel_id)) < 0) {
        /* A DM we have never seen: look it up, then replay this message. */
        if (a->kind == ACTIVITY_MESSAGE && !a->guild_id[0]) {
            for (int k = 0; k < (int)ARRAYSIZE(g_ui.pending); k++)
                if (!g_ui.pending[k]) {
                    activity_t *copy = mem_alloc(sizeof *copy);
                    *copy = *a;
                    a->author = a->preview = a->mention_roles = (sb_t){0};
                    g_ui.pending[k] = copy;
                    app_fetch_channel(a->channel_id);
                    break;
                }
        }
        return;
    }
    c = &g_ui.model->channels[i];

    if (a->kind == ACTIVITY_ACK) {
        if (model_id_cmp(a->message_id, c->read) > 0)
            lstrcpynA(c->read, a->message_id, sizeof c->read);
        if (!model_unread(g_ui.model, (unsigned)i))
            c->mentions = 0;
    } else {
        if (model_id_cmp(a->message_id, c->last_message) > 0)
            lstrcpynA(c->last_message, a->message_id, sizeof c->last_message);
        if (a->from_me) {
            lstrcpynA(c->read, a->message_id, sizeof c->read);
            c->mentions = 0;
        } else if (i == g_ui.channel && window_active()) {
            /* Being read right now: acknowledge after a short pause to batch bursts. */
            lstrcpynA(c->read, a->message_id, sizeof c->read);
            g_ui.ack_pending = 1;
            SetTimer(g_ui.wnd, TIMER_ACK, ACK_DELAY, NULL);
        } else {
            int dm = is_dm_type(c->type);
            if (!a->mentions_me && a->mention_roles.len) {
                /* @Role pings count when we have that role. */
                int g = model_channel_guild(g_ui.model, (unsigned)i);
                const char *p = a->mention_roles.data;
                while (*p && !a->mentions_me) {
                    char role[24];
                    int n = 0;
                    while (*p && *p != ',' && n < 23)
                        role[n++] = *p++;
                    role[n] = 0;
                    if (*p == ',')
                        p++;
                    a->mentions_me = model_has_role(g_ui.model, g, role);
                }
            }
            if (a->mentions_me || dm)
                c->mentions++;
            if (a->mentions_me || (dm && !c->muted))
                notify(i, a);
        }
    }
    update_title();
}

static void show_window(void)
{
    if (IsIconic(g_ui.wnd))
        ShowWindow(g_ui.wnd, SW_RESTORE);
    SetForegroundWindow(g_ui.wnd);
}

/* ---- Selection ---- */

static void place_composer(void)
{
    RECT rc;
    int x0 = S(RAIL_W + SIDE_W), show = g_ui.view == VIEW_APP && open_is_text() && !g_ui.msgs_status;

    GetClientRect(g_ui.wnd, &rc);
    if (show) {
        int cy = rc.bottom - S(24) - S(COMPOSER_H);
        int eh = S(22);
        MoveWindow(g_ui.composer, x0 + S(32), cy + (S(COMPOSER_H) - eh) / 2, rc.right - x0 - S(64), eh, TRUE);
    }
    ShowWindow(g_ui.composer, show ? SW_SHOWNA : SW_HIDE);
}

static void open_channel(int index)
{
    const channel_t *c;
    wchar_t *hint;
    char text[160];

    pop_close();
    g_ui.channel = index;
    free_messages();
    g_ui.msg_scroll = 0;
    g_ui.msgs_status = 0;
    g_ui.msgs_older_loading = 0;
    g_ui.hover_msg = -1;
    sb_clear(&g_ui.send_error);
    g_ui.msgs_channel[0] = 0;
    if (index < 0 || !g_ui.model || is_voice_type(chan(index)->type)) {
        g_ui.msgs_loading = 0;
        app_open_channel("");
        place_composer();
        return;
    }
    c = chan(index);
    mark_read(index);
    lstrcpynA(g_ui.msgs_channel, c->id, sizeof g_ui.msgs_channel);
    g_ui.msgs_loading = 1;
    g_ui.msgs_has_more = 1;
    app_open_channel(c->id);
    app_fetch_messages(c->id, NULL);

    wsprintfA(text, is_dm_type(c->type) ? "Message @%.120s" : "Message #%.120s", model_str(g_ui.model, c->name));
    hint = utf8_to_wide(text, lstrlenA(text));
    SendMessageW(g_ui.composer, EM_SETCUEBANNER, TRUE, (LPARAM)hint);
    mem_free(hint);
    SetWindowTextW(g_ui.composer, L"");
    place_composer();
}


static void select_guild(int i)
{
    if (!g_ui.model)
        return;
    g_ui.guild = i;
    g_ui.side_scroll = 0;
    g_ui.channel = i >= 0 ? g_ui.last_channel[i] : g_ui.last_dm;
    if (i >= 0 && g_ui.channel < 0) {
        const guild_t *gd = &g_ui.model->guilds[i];
        for (unsigned c = gd->first; c < gd->first + gd->count; c++)
            if (chan((int)c)->type != CH_CATEGORY && chan((int)c)->type != CH_VOICE && chan((int)c)->type != CH_STAGE) {
                g_ui.channel = (int)c;
                break;
            }
    }
    open_channel(g_ui.channel);
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
        } else if (index != g_ui.channel) {
            if (g_ui.guild >= 0)
                g_ui.last_channel[g_ui.guild] = index;
            else
                g_ui.last_dm = index;
            open_channel(index);
        }
        redraw();
        break;
    case HIT_LOGOUT:
        app_logout();
        break;
    case HIT_SELF:
        open_self();
        break;
    case HIT_RETRY:
        g_ui.disconnected = 0;
        set_text(&g_ui.status, "Connecting\xE2\x80\xA6");
        redraw();
        app_reconnect();
        break;
    }
}

/* Jumps to channel i, as when clicking a notification. */
static void go_to_channel(int i)
{
    int g;

    if (!g_ui.model || i < 0 || (unsigned)i >= g_ui.model->nchannels)
        return;
    g = model_channel_guild(g_ui.model, (unsigned)i);
    if (g != g_ui.guild)
        select_guild(g);
    if (g_ui.channel != i) {
        if (g >= 0)
            g_ui.last_channel[g] = i;
        else
            g_ui.last_dm = i;
        open_channel(i);
    }
    redraw();
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
    g_ui.last_dm = -1;
    g_ui.guild = -1;
    open_channel(-1);
    g_ui.rail_scroll = g_ui.side_scroll = 0;
}

static const char *id_or_empty(int i)
{
    return i >= 0 ? chan(i)->id : "";
}

static int map_channel(const model_t *m, const char *id)
{
    return id[0] ? model_find_channel(m, id) : -1;
}

/* Swaps in an updated model (live event or new READY) and keeps what the user was looking at. */
static void replace_model(model_t *m)
{
    model_t *old = g_ui.model;
    char guild[24], channel[24], last_dm[24], notified[24];
    int *last = mem_alloc(((size_t)m->nguilds + 1) * sizeof *last);
    unsigned char *collapsed = mem_alloc((size_t)m->nchannels + 1);
    int ch;

    lstrcpynA(guild, g_ui.guild >= 0 ? old->guilds[g_ui.guild].id : "", sizeof guild);
    lstrcpynA(channel, id_or_empty(g_ui.channel), sizeof channel);
    lstrcpynA(last_dm, id_or_empty(g_ui.last_dm), sizeof last_dm);
    lstrcpynA(notified, id_or_empty(g_ui.notified_channel), sizeof notified);
    for (unsigned g = 0; g < m->nguilds; g++) {
        int og = model_find_guild(old, m->guilds[g].id);
        last[g] = og >= 0 ? map_channel(m, id_or_empty(g_ui.last_channel[og])) : -1;
    }
    for (unsigned i = 0; i < old->nchannels; i++)
        if (g_ui.collapsed[i]) {
            int ni = model_find_channel(m, old->channels[i].id);
            if (ni >= 0)
                collapsed[ni] = 1;
        }

    mem_free(g_ui.last_channel);
    mem_free(g_ui.collapsed);
    g_ui.last_channel = last;
    g_ui.collapsed = collapsed;
    g_ui.model = m;
    g_ui.guild = guild[0] ? model_find_guild(m, guild) : -1;
    g_ui.last_dm = map_channel(m, last_dm);
    g_ui.notified_channel = map_channel(m, notified);
    g_ui.hover_kind = HIT_NONE;
    ch = map_channel(m, channel);
    model_free(old);

    if (guild[0] && g_ui.guild < 0) {
        select_guild(-1); /* we left the server we were looking at */
    } else if (channel[0] && ch < 0) {
        open_channel(-1); /* the channel is gone */
    } else {
        g_ui.channel = ch; /* same channel, new index: keep its messages */
    }
    clamp_scroll();
    update_title();
}

static void on_event(sb_t *p)
{
    const char *name = p->data;
    size_t n = (size_t)lstrlenA(name) + 1;
    json_t d;
    model_t *m;

    if (!g_ui.model || n >= p->len || !json_parse(p->data + n, p->len - n, &d))
        return;
    if (lstrcmpA(name, "GUILD_MEMBERS_CHUNK") == 0 || lstrcmpA(name, "GUILD_MEMBER_UPDATE") == 0) {
        json_t v, list, item;
        json_iter_t it;
        char guild[24] = "";
        if (json_get(d, "guild_id", &v))
            json_raw(v, guild, sizeof guild);
        if (json_get(d, "members", &list)) {
            json_iter(list, &it);
            while (json_next(&it, NULL, &item))
                member_store(guild, item);
        } else if (json_get(d, "user", &item) && json_get(item, "id", &v)) {
            char id[24];
            json_raw(v, id, sizeof id);
            if (member_find(guild, id))
                member_store(guild, d);
        }
    }
    m = model_apply(g_ui.model, name, d);
    if (m)
        replace_model(m);

    /* Replay messages that were waiting for this channel. */
    for (int k = 0; k < (int)ARRAYSIZE(g_ui.pending); k++) {
        activity_t *a = g_ui.pending[k];
        if (a && model_find_channel(g_ui.model, a->channel_id) >= 0) {
            g_ui.pending[k] = NULL;
            on_activity(a);
            activity_free(a);
        }
    }
}

static void clear_session(void)
{
    for (int k = 0; k < (int)ARRAYSIZE(g_ui.pending); k++)
        if (g_ui.pending[k]) {
            activity_free(g_ui.pending[k]);
            g_ui.pending[k] = NULL;
        }
    g_ui.reconnecting = 0;
    if (g_ui.model) {
        model_free(g_ui.model);
        g_ui.model = NULL;
    }
    pop_close();
    profiles_clear();
    members_clear();
    images_clear();
    g_ui.guild = -1;
    open_channel(-1);
    g_ui.hover_kind = HIT_NONE;
    sb_clear(&g_ui.account);
}

/* ---- Messages from workers ---- */

static void on_worker(UINT msg, WPARAM wp, LPARAM lp)
{
    sb_t *p = (sb_t *)lp;
    const char *s = p && p->data ? p->data : "";

    if (msg == UI_MESSAGES) {
        on_batch((msg_batch_t *)lp);
        redraw();
        return;
    }
    if (msg == UI_PROFILE) {
        on_profile((profile_t *)lp);
        return;
    }
    if (msg == UI_FONT) {
        on_font((int)wp, p);
        if (p) {
            sb_free(p);
            mem_free(p);
        }
        if (g_ui.pop) {
            pop_place();
            InvalidateRect(g_ui.pop, NULL, FALSE);
        }
        return;
    }
    if (msg == UI_ACTIVITY) {
        on_activity((activity_t *)lp);
        activity_free((activity_t *)lp);
        redraw();
        return;
    }

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
        place_composer();
        break;
    case UI_READY:
        g_ui.reconnecting = 0;
        if (g_ui.model)
            replace_model((model_t *)lp); /* reconnected with a fresh session */
        else
            set_model((model_t *)lp);
        update_title();
        set_text(&g_ui.status, "Online");
        g_ui.view = VIEW_APP;
        redraw();
        return;
    case UI_EVENT:
        if (p)
            on_event(p);
        break;
    case UI_RECONNECTING:
        g_ui.reconnecting = 1;
        set_text(&g_ui.status, s);
        break;
    case UI_ONLINE:
        g_ui.reconnecting = 0;
        set_text(&g_ui.status, "Online");
        break;
    case UI_DISCONNECTED:
        g_ui.disconnected = 1;
        set_text(&g_ui.status, s);
        g_ui.view = VIEW_APP;
        break;
    case UI_SEND_FAILED:
        set_text(&g_ui.send_error, s);
        break;
    case UI_DM_OPENED:
        /* The DM exists in the model by now (CHANNEL_CREATE came first). */
        if (g_ui.pending_dm[0] && g_ui.model) {
            g_ui.pending_dm[0] = 0;
            go_to_channel(map_channel(g_ui.model, s));
        }
        break;
    case UI_IMAGE: {
        image_t *im = image_find(s);
        if (im) {
            im->img = (r_image_t *)wp;
            im->failed = !wp;
        } else {
            r_image_free((r_image_t *)wp);
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
    place_composer();
    redraw();
}

void ui_show_loading(const char *s)
{
    set_text(&g_ui.status, s);
    g_ui.view = VIEW_LOADING;
    place_composer();
    redraw();
}

/* ---- Clicks in messages ---- */

/*
 * Styled span under (x, y) in the message list: returns 1 over a link or a
 * hidden spoiler, and reports which message and span.
 */
static int rich_hit(int x, int y, int *msg, int *link)
{
    int top, i = message_at(x, y, &top), tx = text_x();
    unsigned flags = 0;
    int l = -1;
    msg_t *m;

    if (i < 0)
        return 0;
    m = &g_ui.msgs[i];
    if (m->system || !(m->text.len || m->edited) || !m->ui)
        return 0;
    if (m->grouped == 2)
        top += S(44);
    top += m->grouped == 1 ? S(2) : S(16) + (m->reply.len ? S(22) : 0) + S(22);
    if (!r_rich_hit(((msg_view_t *)m->ui)->rich, x - tx, y - top, &flags, &l))
        return 0;
    if (!(flags & MD_LINK) && !((flags & MD_SPOILER) && !m->revealed))
        return 0;
    if (msg)
        *msg = i;
    if (link)
        *link = (flags & MD_SPOILER) && !m->revealed ? -1 : l;
    return 1;
}

static void open_url(const char *url)
{
    wchar_t *w;

    int n = lstrlenA(url);

    /* Only web links: never hand a file path or another scheme to the shell. */
    if (!(n > 8 && CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, url, 8, "https://", 8) == CSTR_EQUAL) &&
        !(n > 7 && CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, url, 7, "http://", 7) == CSTR_EQUAL))
        return;
    w = utf8_to_wide(url, n);
    ShellExecuteW(g_ui.wnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    mem_free(w);
}

/* Image, file, embed or reaction under (x, y). */
static int part_hit(int x, int y, int *msg, part_t *part)
{
    int top, i = message_at(x, y, &top), w = text_w_px();
    msg_t *m;

    if (i < 0)
        return 0;
    m = &g_ui.msgs[i];
    if (m->system)
        return 0;
    if (m->grouped == 2)
        top += S(44);
    top += m->grouped == 1 ? S(2) : S(16) + (m->reply.len ? S(22) : 0) + S(22);
    if (m->text.len || m->edited)
        top += r_rich_height(msg_rich(m, w));
    *part = (part_t){PART_NONE, -1};
    msg_extras(m, text_x(), top, w, 0, x, y, part);
    *msg = i;
    return part->kind != PART_NONE;
}

static int click_part(int x, int y)
{
    int i;
    part_t p;
    msg_t *m;

    if (!part_hit(x, y, &i, &p))
        return 0;
    m = &g_ui.msgs[i];
    switch (p.kind) {
    case PART_SPOILER:
        m->revealed = 1;
        break;
    case PART_FILE:
    case PART_MEDIA:
        open_url(m->files[p.index].url.data);
        break;
    case PART_EMBED_TITLE: {
        msg_embed_t *e = &m->embeds[p.index];
        open_url(e->url.len ? e->url.data : e->image.data ? e->image.data : "");
        break;
    }
    case PART_REACTION: {
        msg_reaction_t r = m->reactions[p.index];
        int add = !r.me;
        sb_t copy = {0};
        sb_addn(&copy, r.emoji.data ? r.emoji.data : "", r.emoji.len);
        r.emoji = copy;
        app_react(m->channel_id[0] ? m->channel_id : g_ui.msgs_channel, m->id, &r, add);
        apply_reaction(m, &r, add ? 1 : -1, 1);
        sb_free(&copy);
        break;
    }
    }
    redraw();
    return 1;
}

static int click_message(int x, int y)
{
    int i, link;

    if (click_part(x, y))
        return 1;
    if (!rich_hit(x, y, &i, &link))
        return 0;
    if (link < 0) {
        g_ui.msgs[i].revealed = 1;
        redraw();
    } else {
        msg_view_t *v = g_ui.msgs[i].ui;
        open_url(md_link(&v->doc, link));
    }
    return 1;
}

/* ---- Profile popout ---- */

/*
 * A child window over the main one, laid out like Discord's user popout:
 * banner, avatar with its decoration, styled display name, username with the
 * server tag and badges, mutual friends and servers, bio, and a box to
 * message the user. The layout is walked once to measure and again to paint.
 */

#define POP_W 300
#define POP_PAD 16
#define POP_AVATAR 80
#define POP_BANNER 105
#define POP_BANNER_PLAIN 60
#define POP_BADGE 22
#define POP_INPUT_H 40
#define POP_RADIUS 8
#define PROFILE_TTL 120000

static const struct {
    int id;
    const char *file;
    int weight;
} k_name_fonts[] = {
    {3, "ofl/cherrybombone/CherryBombOne-Regular.ttf", FW_NORMAL},
    {4, "ofl/chicle/Chicle-Regular.ttf", FW_NORMAL},
    {6, "ofl/museomoderno/MuseoModerno%5Bwght%5D.ttf", FW_BOLD},
    {8, "ofl/pixelifysans/PixelifySans%5Bwght%5D.ttf", FW_BOLD},
    {12, "ofl/zillaslab/ZillaSlab-Bold.ttf", FW_BOLD},
    {13, "ofl/playpensans/PlaypenSans%5Bwght%5D.ttf", FW_BOLD},
    {14, "ofl/orbitron/Orbitron%5Bwght%5D.ttf", FW_BOLD},
    {15, "ofl/newrocker/NewRocker-Regular.ttf", FW_NORMAL},
    {16, "ofl/kalam/Kalam-Bold.ttf", FW_BOLD},
};

static int name_font_slot(int id)
{
    for (int i = 0; i < (int)ARRAYSIZE(k_name_fonts); i++)
        if (k_name_fonts[i].id == id)
            return i;
    return -1;
}

static void build_name_fonts(void)
{
    for (int i = 0; i < (int)ARRAYSIZE(k_name_fonts); i++) {
        r_font_free(g_ui.name_fonts[i]);
        g_ui.name_fonts[i] = g_ui.font_data[i].len ? r_font_data(g_ui.font_data[i].data, g_ui.font_data[i].len,
                                                                 S(20), k_name_fonts[i].weight)
                                                   : NULL;
    }
}

/* Font of a display name style; falls back to the UI font while it downloads, or for fonts we do not have. */
static r_font_t *name_font(int id)
{
    int i = name_font_slot(id);

    if (i < 0)
        return g_ui.f_name;
    if (!g_ui.font_state[i]) {
        g_ui.font_state[i] = 1;
        app_fetch_font(id, k_name_fonts[i].file);
    }
    return g_ui.name_fonts[i] ? g_ui.name_fonts[i] : g_ui.f_name;
}

static void on_font(int id, sb_t *data)
{
    int i = name_font_slot(id);

    if (i < 0 || !data) {
        if (i >= 0)
            g_ui.font_state[i] = 2; /* failed: keep the fallback */
        return;
    }
    sb_free(&g_ui.font_data[i]);
    g_ui.font_data[i] = *data;
    *data = (sb_t){0};
    g_ui.font_state[i] = 2;
    r_font_free(g_ui.name_fonts[i]);
    g_ui.name_fonts[i] = r_font_data(g_ui.font_data[i].data, g_ui.font_data[i].len, S(20), k_name_fonts[i].weight);
}

/* ---- Cache ---- */

static profile_t *cached_profile(const char *user, const char *guild, int *fresh)
{
    for (int i = 0; i < (int)ARRAYSIZE(g_ui.profiles); i++) {
        profile_t *p = g_ui.profiles[i];
        if (p && lstrcmpA(p->id, user) == 0 && lstrcmpA(p->guild_id, guild) == 0) {
            *fresh = GetTickCount() - g_ui.profile_time[i] < PROFILE_TTL;
            return p;
        }
    }
    return NULL;
}

/* Takes ownership of p; returns it, or NULL if it was dropped. */
static profile_t *cache_profile(profile_t *p)
{
    int n = (int)ARRAYSIZE(g_ui.profiles), slot = -1;
    DWORD now = GetTickCount();

    for (int i = 0; i < n && slot < 0; i++)
        if (g_ui.profiles[i] && lstrcmpA(g_ui.profiles[i]->id, p->id) == 0 &&
            lstrcmpA(g_ui.profiles[i]->guild_id, p->guild_id) == 0)
            slot = i;
    for (int i = 0; i < n && slot < 0; i++)
        if (!g_ui.profiles[i])
            slot = i;
    if (slot < 0) {
        /* Full: evict the oldest one that is not on screen. */
        DWORD age = 0;
        for (int i = 0; i < n; i++)
            if (g_ui.profiles[i] != g_ui.pop_profile && (slot < 0 || now - g_ui.profile_time[i] > age)) {
                slot = i;
                age = now - g_ui.profile_time[i];
            }
    }
    if (g_ui.profiles[slot]) {
        if (g_ui.profiles[slot] == g_ui.pop_profile)
            g_ui.pop_profile = NULL;
        profile_free(g_ui.profiles[slot]);
        mem_free(g_ui.profiles[slot]);
    }
    g_ui.profiles[slot] = p;
    g_ui.profile_time[slot] = now;
    return p;
}

static void profiles_clear(void)
{
    for (int i = 0; i < (int)ARRAYSIZE(g_ui.profiles); i++)
        if (g_ui.profiles[i]) {
            profile_free(g_ui.profiles[i]);
            mem_free(g_ui.profiles[i]);
            g_ui.profiles[i] = NULL;
        }
}

/* ---- Images ---- */

static r_image_t *pop_avatar(const profile_t *p)
{
    char key[96], path[200];

    if (!p || !p->avatar[0])
        return user_avatar(g_ui.pop_user, g_ui.pop_avatar);
    wsprintfA(key, "A:%s:%s:%s", p->id, p->member_avatar ? p->guild_id : "", p->avatar);
    if (p->member_avatar)
        wsprintfA(path, "/guilds/%s/users/%s/avatars/%s.png?size=256", p->guild_id, p->id, p->avatar);
    else
        wsprintfA(path, "/avatars/%s/%s.png?size=256", p->id, p->avatar);
    return image_get(key, path, S(POP_AVATAR));
}

static r_image_t *pop_banner(const profile_t *p)
{
    char key[96], path[200];

    if (!p || !p->banner[0])
        return NULL;
    wsprintfA(key, "b:%s:%s", p->id, p->banner);
    if (p->member_banner)
        wsprintfA(path, "/guilds/%s/users/%s/banners/%s.png?size=600", p->guild_id, p->id, p->banner);
    else
        wsprintfA(path, "/banners/%s/%s.png?size=600", p->id, p->banner);
    return image_get(key, path, S(POP_W));
}

static r_image_t *cdn_image(const char *prefix, const char *path_fmt, const char *a, const char *b, int max_px)
{
    char key[96], path[200];

    wsprintfA(key, "%s:%s:%s", prefix, a, b ? b : "");
    wsprintfA(path, path_fmt, a, b);
    return image_get(key, path, max_px);
}

/* ---- Layout ---- */

static unsigned rgb_argb(unsigned rgb, unsigned alpha)
{
    return alpha << 24 | (rgb & 0xFFFFFF);
}

/* A theme color at 40%, as the body under Discord's dark overlay. */
static unsigned mix_dark(unsigned rgb)
{
    return ((rgb >> 16 & 0xFF) * 2 / 5) << 16 | ((rgb >> 8 & 0xFF) * 2 / 5) << 8 | (rgb & 0xFF) * 2 / 5;
}

static int pop_banner_h(const profile_t *p)
{
    return S(p && p->banner[0] ? POP_BANNER : POP_BANNER_PLAIN);
}

static void pop_tooltip(const char *s, int cx, int bottom, int w)
{
    int tw = text_width(g_ui.f_small, s) + S(16), th = S(28), x;

    if (tw > w - S(16))
        tw = w - S(16);
    x = cx - tw / 2;
    if (x < S(8))
        x = S(8);
    if (x + tw > w - S(8))
        x = w - S(8) - tw;
    r_round(x, bottom - th, tw, th, S(6), ARGB(C_TIP));
    text(g_ui.f_small, C_INK, rect(x + S(8), bottom - th, tw - S(16), th), s,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

/* Walks the popout layout; paints when `draw` is set. Returns the height. */
static int pop_render(int draw)
{
    const profile_t *p = g_ui.pop_profile;
    int w = S(POP_W), pad = S(POP_PAD), inner = w - 2 * pad;
    int bh = pop_banner_h(p), y, ax = pad, ay = bh - S(POP_AVATAR) / 2;
    unsigned body = 0xFF111111, border = 0xFF262626;
    r_image_t *avatar = pop_avatar(p);
    const char *name = p ? p->name.data : g_ui.pop_name.data;

    if (p && p->ntheme == 2) {
        /* Nitro theme: the user's two colors, darkened like Discord's dark theme. */
        border = rgb_argb(p->theme[0], 0xFF);
    }
    if (draw) {
        r_image_t *banner = pop_banner(p);
        unsigned bc = 0;

        fill(0, 0, w, S(4000), g_ui.pop_ax < S(RAIL_W + SIDE_W) ? C_SIDE : C_MAIN); /* behind the corners */
        if (p && p->ntheme == 2) {
            r_round_gradient(0, 0, w, g_ui.pop_h, S(POP_RADIUS), rgb_argb(p->theme[0], 0xFF),
                             rgb_argb(p->theme[1], 0xFF));
            r_round(0, 0, w, g_ui.pop_h, S(POP_RADIUS), 0x99000000u);
        } else {
            r_round(0, 0, w, g_ui.pop_h, S(POP_RADIUS), body);
        }
        /* Banner: image, else theme or accent color, else the avatar's average color. */
        r_clip(0, 0, w, bh);
        if (banner) {
            r_image_cover(banner, 0, 0, w, bh + S(POP_RADIUS), S(POP_RADIUS));
        } else {
            if (p && p->ntheme == 2)
                bc = rgb_argb(p->theme[0], 0xFF);
            else if (p && p->has_accent)
                bc = rgb_argb(p->accent, 0xFF);
            else if (avatar)
                bc = r_image_average(avatar);
            r_round(0, 0, w, bh + S(POP_RADIUS), S(POP_RADIUS), bc ? bc : 0xFF2A2A2A);
        }
        r_unclip();

        /* More menu, on the banner. */
        {
            int bx = w - S(12) - S(32), by = S(12);
            r_circle(bx, by, S(32), g_ui.pop_hover == -2 ? 0xCC000000u : 0x99000000u);
            text_w(g_ui.f_icon, C_INK, rect(bx, by, S(32), S(32)), L"\xE712", -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        /* Avatar in a ring of the body color, decoration on top. */
        r_circle(ax - S(6), ay - S(6), S(POP_AVATAR) + S(12), p && p->ntheme == 2 ? 0xFF000000u | mix_dark(p->theme[0]) : body);
        if (avatar)
            r_image(avatar, ax, ay, S(POP_AVATAR), S(POP_AVATAR), S(POP_AVATAR) / 2);
        else
            r_circle(ax, ay, S(POP_AVATAR), ARGB(C_ITEM));
        if (p && p->decoration[0]) {
            r_image_t *deco = cdn_image("ad", "/avatar-decoration-presets/%s.png?size=240&passthrough=false",
                                        p->decoration, NULL, S(POP_AVATAR) * 6 / 5);
            int d = S(POP_AVATAR) * 6 / 5;
            if (deco)
                r_image(deco, ax - (d - S(POP_AVATAR)) / 2, ay - (d - S(POP_AVATAR)) / 2, d, d, 0);
        }
        if (g_ui.pop_self) {
            int dot = g_ui.disconnected ? C_FAINT : g_ui.model && !g_ui.reconnecting ? C_GREEN : C_AMBER;
            int dx = ax + S(POP_AVATAR) - S(22), dy = ay + S(POP_AVATAR) - S(22);
            r_circle(dx - S(5), dy - S(5), S(26), body);
            r_circle(dx, dy, S(16), ARGB(dot));
        }
    }
    y = ay + S(POP_AVATAR) + S(12);

    /* Display name with its font and effect. */
    if (draw) {
        wchar_t *wn = utf8_to_wide(name ? name : "", name ? lstrlenA(name) : 0);
        unsigned ink = ARGB(C_INK) & 0xFFFFFF;
        if (p && (p->ncolors || p->font_id))
            r_text_styled(name_font(p->font_id), p->ncolors ? p->colors : &ink, p->ncolors ? p->ncolors : 1,
                          p->ncolors ? p->effect_id : 1, pad, y, inner,
                          S(28), wn, -1, R_SINGLE | R_ELLIPSIS | R_VCENTER);
        else
            r_text(g_ui.f_name, ARGB(C_INK), pad, y, inner, S(28), wn, -1, R_SINGLE | R_ELLIPSIS | R_VCENTER);
        mem_free(wn);
    }
    y += S(30);

    /* Username, pronouns, server tag, then badges; badges wrap if they do not fit. */
    if (p) {
        int x = pad, lh = S(POP_BADGE);
        char line[160];

        if (p->pronouns.len)
            wsprintfA(line, "%.64s \xE2\x80\xA2 %.60s", p->username.data, p->pronouns.data);
        else
            wsprintfA(line, "%.64s", p->username.data);
        if (draw)
            text(g_ui.f_body, C_INK, rect(x, y, inner, lh), line, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        x += text_width(g_ui.f_body, line) + S(8);
        if (x > w - pad)
            x = w - pad;

        if (p->tag[0]) {
            int tw = text_width(g_ui.f_cat, p->tag) + S(8) + (p->tag_badge[0] ? S(16) : 0);
            if (x + tw > w - pad) {
                x = pad;
                y += lh + S(4);
            }
            if (draw) {
                r_round(x, y + S(2), tw, lh - S(4), S(4), 0x26FFFFFFu);
                if (p->tag_badge[0] && p->tag_guild[0]) {
                    r_image_t *tb = cdn_image("gt", "/guild-tag-badges/%s/%s.png?size=32", p->tag_guild, p->tag_badge,
                                              S(14));
                    if (tb)
                        r_image(tb, x + S(4), y + (lh - S(14)) / 2, S(14), S(14), 0);
                }
                text(g_ui.f_cat, C_INK, rect(x + S(4) + (p->tag_badge[0] ? S(16) : 0), y, tw, lh), p->tag,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            x += tw + S(8);
        }

        for (int i = 0; i < p->nbadges && i < (int)ARRAYSIZE(g_ui.pop_badge_x); i++) {
            if (x + S(POP_BADGE) > w - pad) {
                x = pad;
                y += lh + S(4);
            }
            g_ui.pop_badge_x[i] = x;
            g_ui.pop_badge_y[i] = y;
            if (draw) {
                r_image_t *bi = cdn_image("bi", "/badge-icons/%s.png?size=64", p->badges[i].icon, NULL, S(POP_BADGE));
                if (g_ui.pop_hover == i)
                    r_round(x - S(2), y - S(2) + (lh - S(POP_BADGE)) / 2, S(POP_BADGE) + S(4), S(POP_BADGE) + S(4), S(4),
                            0x1FFFFFFFu);
                if (bi)
                    r_image(bi, x, y + (lh - S(POP_BADGE)) / 2, S(POP_BADGE), S(POP_BADGE), 0);
            }
            x += S(POP_BADGE) + S(4);
        }
        y += lh;
    } else if (draw) {
        text(g_ui.f_small, C_MUTED, rect(pad, y, inner, S(POP_BADGE)),
             g_ui.pop_failed ? "Could not load this profile." : "Loading\xE2\x80\xA6", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    if (!p)
        y += S(POP_BADGE);

    /* Mutual friends and servers. */
    if (p && !g_ui.pop_self && (p->mutual_friends > 0 || p->mutual_guilds > 0)) {
        char line[96] = "";
        int x = pad;

        y += S(12);
        if (p->mutual_friends > 0)
            wsprintfA(line, "%d mutual friend%s", p->mutual_friends, p->mutual_friends == 1 ? "" : "s");
        if (p->mutual_guilds > 0)
            wsprintfA(line + lstrlenA(line), "%s%d mutual server%s", line[0] ? " \xE2\x80\xA2 " : "", p->mutual_guilds,
                      p->mutual_guilds == 1 ? "" : "s");
        if (draw) {
            for (int i = 0; i < p->nfriends; i++) {
                r_image_t *fa = user_avatar(p->friends[i].id, p->friends[i].avatar);
                r_circle(x - S(2), y, S(20), body);
                if (fa)
                    r_image(fa, x, y + S(2), S(16), S(16), S(8));
                else
                    r_circle(x, y + S(2), S(16), ARGB(C_ITEM));
                x += S(12);
            }
            if (p->nfriends)
                x += S(10);
            text(g_ui.f_small, C_MUTED, rect(x, y, w - pad - x, S(20)), line, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        y += S(20);
    }

    /* Bio, as markdown. */
    if (p && p->bio.len) {
        if (!g_ui.pop_rich || g_ui.pop_rich_w != inner) {
            r_rich_free(g_ui.pop_rich);
            if (!g_ui.pop_bio.text)
                md_parse(p->bio.data, p->bio.len, &g_ui.pop_bio);
            g_ui.pop_rich = r_rich_build(&g_ui.pop_bio, &g_ui.rich, inner);
            g_ui.pop_rich_w = inner;
        }
        y += S(12);
        if (draw)
            r_rich_draw(g_ui.pop_rich, pad, y, 1);
        y += r_rich_height(g_ui.pop_rich);
    }

    /* Message box; the EDIT control sits inside it. */
    if (!g_ui.pop_self) {
        y += S(16);
        g_ui.pop_input_y = y;
        if (draw)
            r_round(pad, y, inner, S(POP_INPUT_H), S(8), g_ui.pop_input_color);
        y += S(POP_INPUT_H);
    }
    y += pad;

    if (draw) {
        r_round_outline(0, 0, w, g_ui.pop_h, S(POP_RADIUS), 1, border);
        if (p && g_ui.pop_hover >= 0 && g_ui.pop_hover < p->nbadges)
            pop_tooltip(p->badges[g_ui.pop_hover].description.data ? p->badges[g_ui.pop_hover].description.data : "",
                        g_ui.pop_badge_x[g_ui.pop_hover] + S(POP_BADGE) / 2, g_ui.pop_badge_y[g_ui.pop_hover] - S(4), w);
    }
    return y;
}

/* Sizes and places the popout next to its anchor, inside the main window. */
static void pop_place(void)
{
    RECT rc;
    int w = S(POP_W), h, x, y;

    if (!g_ui.pop)
        return;
    GetClientRect(g_ui.wnd, &rc);
    h = pop_render(0);
    if (h > rc.bottom - S(16))
        h = rc.bottom - S(16);
    g_ui.pop_h = h;
    x = g_ui.pop_ax;
    y = g_ui.pop_above ? g_ui.pop_ay - h : g_ui.pop_ay;
    if (x + w > rc.right - S(8))
        x = rc.right - S(8) - w;
    if (y + h > rc.bottom - S(8))
        y = rc.bottom - S(8) - h;
    if (y < S(8))
        y = S(8);
    SetWindowPos(g_ui.pop, HWND_TOP, x, y, w, h, SWP_NOACTIVATE);
    if (g_ui.pop_edit) {
        int eh = S(20);
        MoveWindow(g_ui.pop_edit, S(POP_PAD) + S(12), g_ui.pop_input_y + (S(POP_INPUT_H) - eh) / 2,
                   S(POP_W) - 2 * S(POP_PAD) - S(24), eh, TRUE);
    }
    InvalidateRect(g_ui.pop, NULL, FALSE);
}

static void pop_reset_bio(void)
{
    r_rich_free(g_ui.pop_rich);
    g_ui.pop_rich = NULL;
    md_free(&g_ui.pop_bio);
}

static void pop_close(void)
{
    HWND pop = g_ui.pop;

    if (!pop)
        return;
    g_ui.pop = NULL;
    g_ui.pop_edit = NULL;
    DestroyWindow(pop);
    if (g_ui.pop_font) {
        DeleteObject(g_ui.pop_font);
        g_ui.pop_font = NULL;
    }
    if (g_ui.pop_brush) {
        DeleteObject(g_ui.pop_brush);
        g_ui.pop_brush = NULL;
    }
    pop_reset_bio();
    g_ui.pop_profile = NULL;
    g_ui.pop_user[0] = 0;
    SetFocus(g_ui.pop_focus && IsWindow(g_ui.pop_focus) ? g_ui.pop_focus : g_ui.wnd);
}

static void pop_set_profile(profile_t *p)
{
    unsigned c = 0xFF1E1E1E;

    pop_reset_bio();
    g_ui.pop_profile = p;
    if (p && p->ntheme == 2)
        c = 0xFF000000u | mix_dark(p->theme[1]);
    g_ui.pop_input_color = c;
    if (g_ui.pop_brush)
        DeleteObject(g_ui.pop_brush);
    g_ui.pop_brush = CreateSolidBrush(RGB(c >> 16 & 0xFF, c >> 8 & 0xFF, c & 0xFF));
    if (g_ui.pop_edit && p) {
        char hint[96];
        wchar_t *w;
        wsprintfA(hint, "Message @%.80s", p->name.data ? p->name.data : "");
        w = utf8_to_wide(hint, lstrlenA(hint));
        SendMessageW(g_ui.pop_edit, EM_SETCUEBANNER, TRUE, (LPARAM)w);
        mem_free(w);
    }
    pop_place();
}

static int dm_with(const char *user_id)
{
    const model_t *m = g_ui.model;

    for (unsigned i = m ? m->dm_first : 0; m && i < m->dm_first + m->dm_count; i++)
        if (m->channels[i].type == CH_DM && lstrcmpA(m->channels[i].user_id, user_id) == 0)
            return (int)i;
    return -1;
}

static void pop_send(void)
{
    int n = GetWindowTextLengthW(g_ui.pop_edit), dm;
    wchar_t *w;
    sb_t text = {0};
    char user[24];

    if (n <= 0)
        return;
    w = mem_alloc(((size_t)n + 1) * sizeof(wchar_t));
    GetWindowTextW(g_ui.pop_edit, w, n + 1);
    wide_to_utf8(w, (size_t)n, &text);
    mem_free(w);
    lstrcpynA(user, g_ui.pop_user, sizeof user);
    pop_close();
    dm = dm_with(user);
    if (dm >= 0) {
        go_to_channel(dm);
        app_send_message(chan(dm)->id, text.data);
    } else {
        /* No conversation yet: Discord creates it, then we switch to it. */
        lstrcpynA(g_ui.pending_dm, user, sizeof g_ui.pending_dm);
        app_open_dm(user, text.data);
    }
    sb_free(&text);
}

static LRESULT CALLBACK pop_edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CHAR && wp == VK_RETURN) {
        pop_send();
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        pop_close();
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_ESCAPE)
        return 0;
    return CallWindowProcW(g_ui.pop_edit_proc, h, msg, wp, lp);
}

static void copy_text(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    HGLOBAL mem;

    if (n <= 0 || !OpenClipboard(g_ui.wnd))
        return;
    EmptyClipboard();
    if ((mem = GlobalAlloc(GMEM_MOVEABLE, (size_t)n * sizeof(wchar_t))) != NULL) {
        MultiByteToWideChar(CP_UTF8, 0, s, -1, GlobalLock(mem), n);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem))
            GlobalFree(mem);
    }
    CloseClipboard();
}

static void pop_menu(void)
{
    HMENU menu = CreatePopupMenu();
    POINT pt = {S(POP_W) - S(12) - S(32), S(12) + S(34)};
    int cmd;
    char id[24];

    lstrcpynA(id, g_ui.pop_user, sizeof id);
    if (g_ui.pop_profile)
        AppendMenuW(menu, MF_STRING, 1, L"Copy username");
    AppendMenuW(menu, MF_STRING, 2, L"Copy user ID");
    ClientToScreen(g_ui.pop, &pt);
    cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g_ui.pop, NULL);
    DestroyMenu(menu);
    if (cmd == 1 && g_ui.pop_profile)
        copy_text(g_ui.pop_profile->username.data);
    else if (cmd == 2)
        copy_text(id);
}

/* -2 over the menu button, a badge index, or -1. */
static int pop_hit(int x, int y)
{
    const profile_t *p = g_ui.pop_profile;
    int bx = S(POP_W) - S(12) - S(32), by = S(12);

    if (x >= bx && x < bx + S(32) && y >= by && y < by + S(32))
        return -2;
    for (int i = 0; p && i < p->nbadges && i < (int)ARRAYSIZE(g_ui.pop_badge_x); i++)
        if (x >= g_ui.pop_badge_x[i] && x < g_ui.pop_badge_x[i] + S(POP_BADGE) && y >= g_ui.pop_badge_y[i] &&
            y < g_ui.pop_badge_y[i] + S(POP_BADGE))
            return i;
    return -1;
}

static LRESULT CALLBACK pop_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(wnd, &ps);
        GetClientRect(wnd, &rc);
        g_ui.pop_frame = ++g_ui.frame;
        QueryPerformanceCounter(&g_ui.frame_start);
        while (rc.right > 0 && rc.bottom > 0 && r_begin(dc, rc.right, rc.bottom)) {
            pop_render(1);
            r_end(dc);
        }
        EndPaint(wnd, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, GDI(C_INK));
        SetBkColor((HDC)wp, RGB(g_ui.pop_input_color >> 16 & 0xFF, g_ui.pop_input_color >> 8 & 0xFF,
                                 g_ui.pop_input_color & 0xFF));
        return (LRESULT)g_ui.pop_brush;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, wnd, 0};
        int hit = pop_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        TrackMouseEvent(&tme);
        if (hit != g_ui.pop_hover) {
            g_ui.pop_hover = hit;
            InvalidateRect(wnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_ui.pop_hover != -1) {
            g_ui.pop_hover = -1;
            InvalidateRect(wnd, NULL, FALSE);
        }
        return 0;
    case WM_SETCURSOR: {
        const profile_t *p = g_ui.pop_profile;
        int h = g_ui.pop_hover;
        if (LOWORD(lp) == HTCLIENT) {
            int hand = h == -2 || (p && h >= 0 && h < p->nbadges && p->badges[h].link.len);
            SetCursor(LoadCursorW(NULL, (LPCWSTR)(hand ? IDC_HAND : IDC_ARROW)));
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONDOWN:
        SetFocus(wnd);
        return 0;
    case WM_LBUTTONUP: {
        const profile_t *p = g_ui.pop_profile;
        int h = pop_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h == -2)
            pop_menu();
        else if (p && h >= 0 && h < p->nbadges && p->badges[h].link.len)
            open_url(p->badges[h].link.data);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            pop_close();
        return 0;
    case WM_MOUSEWHEEL:
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

/*
 * Opens the popout for a user. (ax, ay) is its top-left corner, or its
 * bottom-left one when `above` is set. `name` and `avatar` are shown while
 * the profile loads.
 */
static void pop_open(const char *user_id, const char *name, const char *avatar, int ax, int ay, int above)
{
    const char *guild = g_ui.guild >= 0 && g_ui.model ? g_ui.model->guilds[g_ui.guild].id : "";
    int fresh = 0, self;
    profile_t *p;

    if (g_ui.pop && lstrcmpA(g_ui.pop_user, user_id) == 0) {
        pop_close(); /* clicking the same user again toggles */
        return;
    }
    pop_close();
    self = g_ui.model && lstrcmpA(user_id, g_ui.model->user_id) == 0;
    lstrcpynA(g_ui.pop_user, user_id, sizeof g_ui.pop_user);
    lstrcpynA(g_ui.pop_guild, guild, sizeof g_ui.pop_guild);
    lstrcpynA(g_ui.pop_avatar, avatar ? avatar : "", sizeof g_ui.pop_avatar);
    set_text(&g_ui.pop_name, name ? name : "");
    g_ui.pop_ax = ax;
    g_ui.pop_ay = ay;
    g_ui.pop_above = above;
    g_ui.pop_self = self;
    g_ui.pop_failed = 0;
    g_ui.pop_hover = -1;

    g_ui.pop = CreateWindowExW(0, L"SilicordPopout", L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0, 0,
                               g_ui.wnd, NULL, NULL, NULL);
    if (!self) {
        g_ui.pop_font = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_ui.pop_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, g_ui.pop,
                                        NULL, NULL, NULL);
        g_ui.pop_edit_proc = (WNDPROC)SetWindowLongPtrW(g_ui.pop_edit, GWLP_WNDPROC, (LONG_PTR)pop_edit_proc);
        SendMessageW(g_ui.pop_edit, WM_SETFONT, (WPARAM)g_ui.pop_font, FALSE);
        SendMessageW(g_ui.pop_edit, EM_LIMITTEXT, 2000, 0);
    }
    p = cached_profile(user_id, guild, &fresh);
    if (!p || !fresh)
        app_fetch_profile(user_id, guild);
    pop_set_profile(p);
    if (!p && g_ui.pop_edit) {
        char hint[96];
        wchar_t *w;
        wsprintfA(hint, "Message @%.80s", name ? name : "");
        w = utf8_to_wide(hint, lstrlenA(hint));
        SendMessageW(g_ui.pop_edit, EM_SETCUEBANNER, TRUE, (LPARAM)w);
        mem_free(w);
    }
    ShowWindow(g_ui.pop, SW_SHOWNA);
    g_ui.pop_focus = GetFocus(); /* given back on close, usually the composer */
    SetFocus(g_ui.pop);
}

static void on_profile(profile_t *p)
{
    int mine = g_ui.pop && lstrcmpA(p->id, g_ui.pop_user) == 0 && lstrcmpA(p->guild_id, g_ui.pop_guild) == 0;

    if (!p->username.len) {
        if (mine && !g_ui.pop_profile) {
            g_ui.pop_failed = 1;
            InvalidateRect(g_ui.pop, NULL, FALSE);
        }
        profile_free(p);
        mem_free(p);
        return;
    }
    p = cache_profile(p);
    if (mine && p)
        pop_set_profile(p);
}

/* Author avatar or name under (x, y) in the message list: opens the popout there. */
static int author_hit(int x, int y, int *msg, int *ax, int *ay)
{
    int top, i = message_at(x, y, &top), x0, tx = text_x(), ny, nw;
    msg_t *m;

    if (i < 0)
        return 0;
    m = &g_ui.msgs[i];
    if (m->system || m->grouped == 1 || !m->author_id[0])
        return 0;
    x0 = message_area().left;
    if (m->grouped == 2)
        top += S(44);
    ny = top + S(16) + (m->reply.len ? S(22) : 0);
    nw = text_width(g_ui.f_h, m->author.data ? m->author.data : "");
    if (y < ny || y >= ny + S(40))
        return 0;
    if (x >= x0 + S(16) && x < x0 + S(56)) {
        *ax = x0 + S(64);
    } else if (x >= tx && x < tx + nw && y < ny + S(22)) {
        *ax = tx + nw + S(8);
    } else {
        return 0;
    }
    *msg = i;
    *ay = ny;
    return 1;
}

static int click_author(int x, int y)
{
    int i, ax, ay;

    if (!author_hit(x, y, &i, &ax, &ay))
        return 0;
    pop_open(g_ui.msgs[i].author_id, g_ui.msgs[i].author.data, g_ui.msgs[i].avatar, ax, ay, 0);
    return 1;
}

static void open_self(void)
{
    RECT rc;
    const char *name = g_ui.model && g_ui.model->user_name ? model_str(g_ui.model, g_ui.model->user_name) : "";

    if (!g_ui.model || !g_ui.model->user_id[0])
        return;
    GetClientRect(g_ui.wnd, &rc);
    pop_open(g_ui.model->user_id, name, g_ui.model->user_avatar, S(RAIL_W) + S(8), rc.bottom - S(PANEL_H) - S(8), 1);
}

/* ---- Composer ---- */

static void send_composer(void)
{
    int n = GetWindowTextLengthW(g_ui.composer);
    wchar_t *w;
    sb_t text = {0};
    size_t a = 0, b;

    if (n <= 0 || !open_is_text())
        return;
    w = mem_alloc(((size_t)n + 1) * sizeof(wchar_t));
    GetWindowTextW(g_ui.composer, w, n + 1);
    wide_to_utf8(w, (size_t)n, &text);
    mem_free(w);
    b = text.len;
    while (a < b && (text.data[a] == ' ' || text.data[a] == '	'))
        a++;
    while (b > a && (text.data[b - 1] == ' ' || text.data[b - 1] == '	'))
        b--;
    if (b > a) {
        text.data[b] = 0;
        sb_clear(&g_ui.send_error);
        app_send_message(g_ui.msgs_channel, text.data + a);
        SetWindowTextW(g_ui.composer, L"");
        g_ui.msg_scroll = 0;
        redraw();
    }
    sb_free(&text);
}

static LRESULT CALLBACK composer_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CHAR && wp == VK_RETURN) {
        send_composer();
        return 0;
    }
    if (msg == WM_MOUSEWHEEL)
        return SendMessageW(g_ui.wnd, msg, wp, lp);
    if (msg == WM_LBUTTONDOWN)
        pop_close();
    return CallWindowProcW(g_ui.composer_proc, h, msg, wp, lp);
}

/* ---- Window procedure ---- */

static void update_hover(int x, int y)
{
    int kind, index, m = message_at(x, y, NULL), link = rich_hit(x, y, NULL, NULL), i, ax, ay;
    part_t part;

    link = link || author_hit(x, y, &i, &ax, &ay) || part_hit(x, y, &i, &part);

    hit_test(x, y, &kind, &index);
    if (m != g_ui.hover_msg || link != g_ui.hover_link) {
        g_ui.hover_msg = m;
        g_ui.hover_link = link;
        redraw();
    }
    if (kind != g_ui.hover_kind || index != g_ui.hover_index) {
        g_ui.hover_kind = kind;
        g_ui.hover_index = index;
        SetCursor(LoadCursorW(NULL, (LPCWSTR)(kind != HIT_NONE || link ? IDC_HAND : IDC_ARROW)));
        redraw();
    }
}

static LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_ui.wnd = wnd;
        g_ui.dpi = GetDpiForWindow(wnd);
        g_ui.b_composer = CreateSolidBrush(RGB(0x1F, 0x1F, 0x1F));
        g_ui.composer = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_CLIPSIBLINGS | ES_AUTOHSCROLL, 0, 0, 0, 0, wnd, NULL,
                                        NULL, NULL);
        g_ui.composer_proc = (WNDPROC)SetWindowLongPtrW(g_ui.composer, GWLP_WNDPROC, (LONG_PTR)composer_proc);
        SendMessageW(g_ui.composer, EM_LIMITTEXT, 2000, 0);
        make_fonts();
        img_init(wnd, UI_IMAGE);
        return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            /* Sitting in the tray: return free heap pages and the working set to Windows. */
            pop_close();
            HeapCompact(GetProcessHeap(), 0);
            SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
            return 0;
        }
        clamp_scroll();
        clamp_msg_scroll();
        place_composer();
        pop_place();
        redraw();
        return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, GDI(C_INK));
        SetBkColor((HDC)wp, RGB(0x1F, 0x1F, 0x1F));
        return (LRESULT)g_ui.b_composer;
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
            SetCursor(LoadCursorW(NULL, (LPCWSTR)(g_ui.hover_kind != HIT_NONE || g_ui.hover_link ? IDC_HAND : IDC_ARROW)));
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
            if (g_ui.pop) {
                pop_close(); /* a click outside only closes the popout */
                return 0;
            }
            if (click_author(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)) || click_message(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)))
                return 0;
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
        pop_close();
        if (pt.x < S(RAIL_W)) {
            g_ui.rail_scroll += delta;
        } else if (pt.x < S(RAIL_W + SIDE_W)) {
            g_ui.side_scroll += delta;
        } else if (open_is_text()) {
            g_ui.msg_scroll -= delta;
            clamp_msg_scroll();
            maybe_load_older();
        }
        clamp_scroll();
        update_hover(pt.x, pt.y);
        redraw();
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_ACK) {
            KillTimer(wnd, TIMER_ACK);
            if (g_ui.ack_pending && g_ui.model && g_ui.channel >= 0) {
                const channel_t *c = chan(g_ui.channel);
                if (c->last_message[0])
                    app_ack(c->id, c->last_message);
            }
            g_ui.ack_pending = 0;
        }
        return 0;
    case WM_ACTIVATE:
        /* Coming back to the window counts as reading the open channel. */
        if (LOWORD(wp) != WA_INACTIVE && g_ui.model && g_ui.channel >= 0 &&
            (model_unread(g_ui.model, (unsigned)g_ui.channel) || chan(g_ui.channel)->mentions)) {
            mark_read(g_ui.channel);
            redraw();
        }
        break;
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case NIN_BALLOONUSERCLICK:
            show_window();
            go_to_channel(g_ui.notified_channel);
            break;
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONUP:
            show_window();
            break;
        }
        return 0;
    case WM_CLOSE:
        Shell_NotifyIconW(NIM_DELETE, &g_ui.tray);
        app_quit();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        if (msg >= UI_QR && msg <= UI_FONT) {
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

    r_init();
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
    wc.lpfnWndProc = pop_proc;
    wc.hIcon = wc.hIconSm = NULL;
    wc.lpszClassName = L"SilicordPopout";
    RegisterClassExW(&wc);

    g_ui.wnd = CreateWindowExW(0, L"Silicord", L"Silicord", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                               CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(1200, dpi, 96), MulDiv(760, dpi, 96),
                               NULL, NULL, inst, NULL);
    DwmSetWindowAttribute(g_ui.wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    DwmSetWindowAttribute(g_ui.wnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof caption);

    g_ui.tray.cbSize = sizeof g_ui.tray;
    g_ui.tray.hWnd = g_ui.wnd;
    g_ui.tray.uID = 1;
    g_ui.tray.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_ui.tray.uCallbackMessage = WM_TRAY;
    g_ui.tray.hIcon = g_ui.icon_small;
    lstrcpyW(g_ui.tray.szTip, L"Silicord");
    Shell_NotifyIconW(NIM_ADD, &g_ui.tray);
    g_ui.tray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_ui.tray);
    return g_ui.wnd;
}
