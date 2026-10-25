/*
 * Time to paint a frame shaped like the client's at 1920x1080: server rail,
 * channel list, twenty messages with avatars, the member list, the composer.
 * Not a test: run it by hand, before and after a change to the renderer.
 *
 *     render_bench DATA_DIR   (messages.json, from tools/make_bench_data.py)
 *
 * With BENCH_DUMP set, it times nothing and writes the frames instead, to
 * compare a renderer's pixels before and after a change.
 */
#include <windows.h>
#include "test.h"
#include "json.h"
#include "md.h"
#include "mem.h"
#include "msg.h"
#include "render.h"
#include "utf.h"

#define W 1920
#define H 1080
#define NMSG 20

static LARGE_INTEGER g_freq;
static r_font_t *g_body, *g_bold, *g_small, *g_chan, *g_mono, *g_h1;
static r_image_t *g_avatar[8];
static r_rich_t *g_rich[NMSG];
static md_doc_t g_doc[NMSG];
static wchar_t *g_names[40];

static long long now_us(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000000 / g_freq.QuadPart;
}

static r_image_t *noise_image(int w, int h, unsigned seed)
{
    r_image_t *img = r_image_blank(w, h);
    unsigned *px = r_image_bits(img);

    for (int i = 0; i < w * h; i++) {
        seed = seed * 1103515245u + 12345u;
        px[i] = 0xFF000000u | (seed >> 8 & 0xFFFFFF);
    }
    return img;
}

static r_image_t *no_emoji(const char *id, int px)
{
    (void)id;
    (void)px;
    return NULL;
}

enum { SHAPES = 1, TEXT = 2, IMAGES = 4, RICH = 8, ALL = 15 };

static void paint(HDC dc, int what)
{
    while (r_begin(dc, W, H)) {
        if (what & SHAPES) {
            r_fill(0, 0, 72, H, 0xFF121214);
            r_fill(72, 0, 240, H, 0xFF1A1A1E);
            r_fill(312, 0, W - 312 - 240, H, 0xFF1F1F23);
            r_fill(W - 240, 0, 240, H, 0xFF1A1A1E);
            r_round(80, 100, 224, 34, 6, 0x33FFFFFF);
            r_round(328, H - 70, W - 312 - 240 - 32, 52, 8, 0xFF2A2A30);
            for (int i = 0; i < 20; i++)
                r_circle(12, 12 + i * 56, 48, 0xFF5865F2);
        }
        if (what & IMAGES) {
            for (int i = 0; i < 20; i++)
                r_image(g_avatar[i & 7], 12, 12 + i * 56, 48, 48, 16);
            for (int i = 0; i < NMSG; i++)
                r_image(g_avatar[i & 7], 328, 20 + i * 50, 40, 40, 20);
            for (int i = 0; i < 30; i++)
                r_image(g_avatar[(i + 3) & 7], W - 228, 60 + i * 34, 32, 32, 16);
        }
        if (what & TEXT) {
            for (int i = 0; i < 30; i++)
                r_text(g_chan, 0xFF949BA4, 104, 60 + i * 32, 190, 24, g_names[i], -1, R_SINGLE | R_ELLIPSIS | R_VCENTER);
            for (int i = 0; i < 30; i++)
                r_text(g_bold, 0xFFDBDEE1, W - 186, 60 + i * 34, 170, 32, g_names[i + 10], -1,
                       R_SINGLE | R_ELLIPSIS | R_VCENTER);
            for (int i = 0; i < NMSG; i++) {
                r_text(g_bold, 0xFFF2F3F5, 384, 20 + i * 50, 300, 20, g_names[i], -1, R_SINGLE);
                r_text(g_small, 0xFF949BA4, 600, 22 + i * 50, 200, 20, L"Today at 12:34", -1, R_SINGLE);
            }
        }
        if (what & RICH)
            for (int i = 0; i < NMSG; i++)
                r_rich_draw(g_rich[i], 384, 40 + i * 50, 0);
        r_end(dc);
    }
    GdiFlush();
}

static void report(const char *name, long long us)
{
    char line[128];
    wsprintfA(line, "%-28s %8d us\r\n", name, (int)us);
    out(line);
}

/* With BENCH_ONLY set to the start of a name, only those lines run, and longer: to profile them. */
static char g_only[64];

static void time_paint(HDC dc, const char *name, int what)
{
    long long best = 0x7FFFFFFFFFFFFFFF;
    int i = 0;

    while (g_only[i] && g_only[i] == name[i])
        i++;
    if (g_only[i])
        return;
    paint(dc, what); /* layouts and glyphs cached, as from the second frame on */
    for (int run = 0; run < (g_only[0] ? 400 : 30); run++) {
        long long t0 = now_us(), dt;
        paint(dc, what);
        dt = now_us() - t0;
        if (dt < best)
            best = dt;
    }
    report(name, best);
}

/* Text in the cases a renderer change can get wrong: sizes, colors, backgrounds, translucency, clips, bands. */
static void paint_text_cases(HDC dc)
{
    static const wchar_t sample[] = L"The quick brown fox jumps over the lazy dog 0123456789 \x00E9\x00E8\x00E0 fi fl -> \x2192";
    static const unsigned inks[] = {0xFFDBDEE1, 0xFFF2F3F5, 0xFF949BA4, 0xFF2781E7, 0xFFFF6B6B, 0xFF57F287, 0xFF000000,
                                    0x80FFFFFF, 0xC0F0B232};
    static const unsigned grounds[] = {0xFF1F1F23, 0xFF313338, 0xFFFFFFFF, 0xFF5865F2, 0xFF000000};
    r_font_t *fonts[] = {g_small, g_body, g_bold, g_chan, g_mono, g_h1};

    while (r_begin(dc, W, H)) {
        r_fill(0, 0, W, H, 0xFF1F1F23);
        for (int g = 0; g < 5; g++)
            r_fill(0, g * 200, W / 2, 200, grounds[g]);
        r_image(g_avatar[0], W / 2, 0, W / 2, H, 0); /* noise: every background at once */
        for (int k = 0; k < 54; k++) {
            int x = (k & 1) * (W / 2) + 8, y = 10 + (k / 2) * 39;
            r_text(fonts[k % 6], inks[k % 9], x, y, W / 2 - 16, 36, sample, -1, R_SINGLE);
        }
        /* across the band edges (512, 1024) and inside clips */
        r_text(g_h1, 0xFFFFFFFF, 30, 500, 900, 40, sample, -1, R_SINGLE);
        r_clip(1000, 700, 300, 14);
        r_text(g_h1, 0xFFFFFFFF, 980, 690, 900, 40, sample, -1, R_SINGLE);
        r_unclip();
        r_text(g_body, 0xFFDBDEE1, 400, 1015, 600, 60, sample, -1, 0);
        r_end(dc);
    }
    GdiFlush();
}

/* Images in the cases a renderer change can get wrong: sizes up and down, radii, fades, alpha, clips, bands. */
static void paint_image_cases(HDC dc)
{
    static r_image_t *src[4];
    static const int sizes[] = {40, 48, 32, 24, 18, 64, 20, 36, 44};
    int x, y;

    if (!src[0]) {
        src[0] = g_avatar[1];               /* 48 px, opaque */
        src[1] = noise_image(40, 40, 4242); /* 40 px, as avatars are decoded */
        src[2] = noise_image(48, 48, 777);  /* translucent: premultiplied, alpha across it */
        for (int i = 0; i < 48 * 48; i++) {
            unsigned *p = r_image_bits(src[2]) + i, a = (unsigned)(i % 48 * 255 / 47);
            *p = a << 24 | ((*p >> 16 & 0xFF) * a / 255) << 16 | ((*p >> 8 & 0xFF) * a / 255) << 8 | (*p & 0xFF) * a / 255;
        }
        src[3] = noise_image(200, 150, 99); /* large, shrunk */
    }
    while (r_begin(dc, W, H)) {
        r_fill(0, 0, W, H, 0xFF313338);
        r_image(g_avatar[0], W / 2, 0, W / 2, H, 0);
        x = 6;
        y = 6;
        for (int s = 0; s < 4; s++)
            for (int k = 0; k < 4; k++)
                for (int i = 0; i < (int)(sizeof sizes / sizeof *sizes); i++) {
                    int d = sizes[i], radius = k == 0 ? 0 : k == 1 ? 4 : k == 2 ? 8 : d / 2;
                    if (x + d > W - 6) {
                        x = 6;
                        y += 70;
                    }
                    r_image(src[s], x, y, d, d, radius);
                    x += d + 6;
                }
        y += 80;
        for (int i = 0; i < 9; i++) { /* fades, as folders open */
            r_image_fade(src[i % 2], 6 + i * 50, y, 40, 40, 20, (float)(i + 1) / 10.f);
            r_image_fade(src[3], 500 + i * 50, y, 40, 40, 8, (float)(i + 1) / 10.f);
        }
        r_image_cover(src[3], 1000, y, 160, 60, 8);
        r_image_cover(src[3], 1200, y, 60, 120, 30);
        r_image(src[3], 1300, y, 300, 220, 12); /* enlarged */
        r_clip(1650, y + 10, 30, 25);          /* a circle seen through a clip */
        r_image(src[1], 1640, y, 40, 40, 20);
        r_unclip();
        for (int i = 0; i < 12; i++) /* across the band edge (512) */
            r_image(src[i % 3], 6 + i * 60, 492, 40, 40, i % 2 ? 20 : 6);
        r_end(dc);
    }
    GdiFlush();
}

/* BENCH_DUMP=PREFIX: the frame, the text and the image cases as raw BGRA, PREFIX.frame.raw and so on. */
static void dump_frames(HDC dc, const void *bits, const char *prefix)
{
    static const char *const names[] = {"frame", "text", "images"};
    char path[MAX_PATH];
    DWORD put;
    HANDLE f;

    for (int k = 0; k < 3; k++) {
        if (k == 0)
            paint(dc, ALL);
        else if (k == 1)
            paint_text_cases(dc);
        else
            paint_image_cases(dc);
        wsprintfA(path, "%s.%s.raw", prefix, names[k]);
        f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) {
            out("cannot write the dump\r\n");
            ExitProcess(1);
        }
        WriteFile(f, bits, W * H * 4, &put, NULL);
        CloseHandle(f);
    }
}

static void load_messages(const char *dir)
{
    char path[MAX_PATH];
    HANDLE f;
    DWORD got = 0;
    LARGE_INTEGER size;
    sb_t page = {0};
    json_t root;
    msg_batch_t *b;
    r_rich_style_t style = {
        .body = g_body, .mono = g_mono, .h1 = g_h1, .h2 = g_bold, .h3 = g_bold, .subtext = g_small,
        .ink = 0xFFDBDEE1, .muted = 0xFF949BA4, .link = 0xFF2781E7u, .mention = 0xFF8CA0FDu,
        .mention_bg = 0x3D5865F2u, .code_bg = 0xFF0A0A0Cu, .quote_bar = 0xFF424246u, .spoiler = 0xFF17181Bu,
        .quote_indent = 16, .code_pad = 8, .block_gap = 4, .radius = 6, .emoji_px = 22, .jumbo_px = 48,
        .emoji = no_emoji,
    };
    static r_rich_style_t s_style;

    s_style = style;
    wsprintfA(path, "%s\\messages.json", dir);
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE || !GetFileSizeEx(f, &size)) {
        out("usage: render_bench DATA_DIR (made by tools/make_bench_data.py)\r\n");
        ExitProcess(1);
    }
    sb_reserve(&page, (size_t)size.QuadPart + 1);
    ReadFile(f, page.data, (DWORD)size.QuadPart, &got, NULL);
    CloseHandle(f);
    page.len = got;
    json_parse(page.data, page.len, &root);
    b = msg_batch_from_array(root, BATCH_HISTORY, "1", 50);
    for (int i = 0; i < NMSG && i < b->n; i++) {
        md_parse(b->msgs[i].text.data ? b->msgs[i].text.data : "", b->msgs[i].text.len, &g_doc[i]);
        g_rich[i] = r_rich_build(&g_doc[i], &s_style, W - 312 - 240 - 100);
    }
    for (int i = 0; i < 40; i++) {
        const sb_t *a = &b->msgs[i % b->n].author;
        g_names[i] = utf8_to_wide(a->data ? a->data : "x", a->data ? a->len : 1);
    }
    msg_batch_free(b);
    sb_free(&page);
}

void entry(void)
{
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), W, -H, 1, 32}};
    HDC dc = CreateCompatibleDC(NULL);
    void *bits = NULL;
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    const char *dir = GetCommandLineA();

    for (const char *p = dir; *p; p++)
        if (*p == ' ' && p[1] && p[1] != ' ')
            dir = p + 1;
    QueryPerformanceFrequency(&g_freq);
    GetEnvironmentVariableA("BENCH_ONLY", g_only, sizeof g_only);
    SetThreadAffinityMask(GetCurrentThread(), 1);
    SelectObject(dc, bm);
    if (!r_init()) {
        out("r_init failed\r\n");
        ExitProcess(1);
    }
    g_body = r_font(L"Segoe UI", 15, FW_NORMAL, 0);
    g_bold = r_font(L"Segoe UI", 15, FW_SEMIBOLD, 0);
    g_small = r_font(L"Segoe UI", 13, FW_NORMAL, 0);
    g_chan = r_font(L"Segoe UI", 16, FW_MEDIUM, 0);
    g_mono = r_font(L"Consolas", 14, FW_NORMAL, 0);
    g_h1 = r_font(L"Segoe UI", 24, FW_BOLD, 0);
    for (int i = 0; i < 8; i++)
        g_avatar[i] = noise_image(48, 48, (unsigned)i * 7919u + 1);
    load_messages(dir);

    {
        char dump[MAX_PATH];
        if (GetEnvironmentVariableA("BENCH_DUMP", dump, sizeof dump)) {
            dump_frames(dc, bits, dump);
            ExitProcess(0);
        }
    }
    time_paint(dc, "frame: everything", ALL);
    time_paint(dc, "frame: shapes", SHAPES);
    time_paint(dc, "frame: images", IMAGES);
    time_paint(dc, "frame: text", TEXT);
    time_paint(dc, "frame: messages (rich)", RICH);
    ExitProcess(0);
}
