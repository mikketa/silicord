/* Custom-drawn Win32 UI in the Silicord palette. Everything runs on the UI thread. */
#include <windows.h>
#include <dwmapi.h>
#include "ui.h"
#include "mem.h"
#include "qr.h"
#include "utf.h"

#define C_BG     RGB(0x0E, 0x0E, 0x0E)
#define C_PANEL  RGB(0x16, 0x16, 0x16)
#define C_FIELD  RGB(0x1C, 0x1C, 0x1C)
#define C_SELECT RGB(0x26, 0x26, 0x26)
#define C_BORDER RGB(0x2E, 0x2E, 0x2E)
#define C_INK    RGB(0xED, 0xE6, 0xD6)
#define C_MUTED  RGB(0x8C, 0x87, 0x7D)
#define C_AMBER  RGB(0xFF, 0xB0, 0x00)

#define ID_TOGGLE 1
#define ID_LOGIN  2
#define ID_LOGOUT 3
#define ID_RETRY  4
#define ID_TOKEN  5
#define ID_GUILDS 6

enum { VIEW_QR, VIEW_TOKEN, VIEW_LOADING, VIEW_MAIN };
enum { BTN_PRIMARY = 1, BTN_LINK = 2 };

typedef struct {
    HWND wnd, toggle, login, logout, retry, token, guilds;
    HFONT f_title, f_body, f_small, f_label;
    HBRUSH b_field, b_panel;
    HICON icon_big, icon_small;
    WNDPROC edit_proc;
    int dpi, view, disconnected;
    qr_t *qr;
    sb_t status, scanned, account;
    /* Layout, in client pixels. */
    RECT r_word, r_box, r_title, r_hint, r_status, r_field, r_side, r_top, r_center;
    int word_unit;
} ui_t;

static ui_t g_ui;

/* 5x7 pixel font of the wordmark, bit 4 = leftmost column. */
static const unsigned char k_word[8][7] = {
    {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E}, {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E},
    {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E},
    {0x00, 0x00, 0x0F, 0x10, 0x10, 0x10, 0x0F}, {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E},
    {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}, {0x01, 0x01, 0x0F, 0x11, 0x11, 0x11, 0x0F},
};

/* The speech bubble mark, 16x16, for the window icon. */
static const unsigned short k_mark[16] = {
    0x0000, 0x0000, 0x1FF8, 0x2004, 0x4002, 0x4C02, 0x4C02, 0x4C02,
    0x4C02, 0x4002, 0x2004, 0x17F8, 0x1800, 0x1000, 0x0000, 0x0000,
};

static int S(int v)
{
    return MulDiv(v, g_ui.dpi, 96);
}

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

static void set_text(sb_t *dst, const char *text)
{
    sb_clear(dst);
    sb_add(dst, text ? text : "");
}

/* ---- Drawing helpers ---- */

static void fill(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r = {x, y, x + w, y + h};

    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

static void text_w(HDC dc, HFONT font, COLORREF color, RECT r, const wchar_t *s, UINT flags)
{
    SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &r, flags | DT_NOPREFIX);
}

static void text(HDC dc, HFONT font, COLORREF color, RECT r, const char *s, UINT flags)
{
    wchar_t *w = utf8_to_wide(s, lstrlenA(s));

    text_w(dc, font, color, r, w, flags);
    mem_free(w);
}

static int word_width(int unit)
{
    return (8 * 6 + 4) * unit; /* 8 glyphs + gaps, then the cursor */
}

static void draw_wordmark(HDC dc, int x, int y, int unit)
{
    for (int g = 0; g < 8; g++)
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if ((k_word[g][row] >> (4 - col)) & 1)
                    fill(dc, x + (g * 6 + col) * unit, y + row * unit, unit, unit, C_INK);
    fill(dc, x + 48 * unit, y, 4 * unit, 7 * unit, C_AMBER);
}

static void draw_qr_box(HDC dc)
{
    RECT r = g_ui.r_box;
    int w = r.right - r.left;

    if (g_ui.scanned.len) {
        RECT t = r;
        fill(dc, r.left, r.top, w, w, C_PANEL);
        t.top += w / 2 - S(40);
        text(dc, g_ui.f_body, C_MUTED, t, "Scanned by", DT_CENTER | DT_SINGLELINE);
        t.top += S(24);
        text(dc, g_ui.f_title, C_INK, t, g_ui.scanned.data, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        t.top += S(40);
        text(dc, g_ui.f_body, C_AMBER, t, "Confirm on your phone", DT_CENTER | DT_SINGLELINE);
        return;
    }
    fill(dc, r.left, r.top, w, w, C_INK);
    if (!g_ui.qr) {
        text(dc, g_ui.f_body, C_BG, r, "Getting a code\xE2\x80\xA6", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    {
        int n = g_ui.qr->size + 8; /* 4-module quiet zone */
        int m = w / n;
        int off = (w - m * g_ui.qr->size) / 2;
        for (int y = 0; y < g_ui.qr->size; y++)
            for (int x = 0; x < g_ui.qr->size; x++)
                if (qr_dark(g_ui.qr, x, y))
                    fill(dc, r.left + off + x * m, r.top + off + y * m, m, m, C_BG);
    }
}

static void draw_field_frame(HDC dc)
{
    RECT r = g_ui.r_field;
    int focus = GetFocus() == g_ui.token;

    fill(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, focus ? C_AMBER : C_BORDER);
    fill(dc, r.left + 1, r.top + 1, r.right - r.left - 2, r.bottom - r.top - 2, C_FIELD);
}

static void paint_login(HDC dc)
{
    int qr = g_ui.view == VIEW_QR;
    RECT label = g_ui.r_field;

    draw_wordmark(dc, g_ui.r_word.left, g_ui.r_word.top, g_ui.word_unit);
    if (qr) {
        draw_qr_box(dc);
    } else {
        label.bottom = label.top;
        label.top -= S(26);
        text(dc, g_ui.f_label, C_MUTED, label, "TOKEN", DT_LEFT | DT_SINGLELINE);
        draw_field_frame(dc);
    }
    text(dc, g_ui.f_title, C_INK, g_ui.r_title,
         qr ? "Log in with your phone" : "Log in with a token", DT_CENTER | DT_SINGLELINE);
    text(dc, g_ui.f_body, C_MUTED, g_ui.r_hint,
         qr ? "Open Discord on your phone, then Settings \xE2\x80\xBA Scan QR Code. "
              "Passkeys and two-factor codes are handled there."
            : "Your token is checked with Discord, then stored encrypted in the Windows Credential Manager.",
         DT_CENTER | DT_WORDBREAK);
    text(dc, g_ui.f_body, C_AMBER, g_ui.r_status, g_ui.status.data ? g_ui.status.data : "",
         DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void paint_loading(HDC dc)
{
    draw_wordmark(dc, g_ui.r_word.left, g_ui.r_word.top, g_ui.word_unit);
    text(dc, g_ui.f_body, C_MUTED, g_ui.r_status, g_ui.status.data ? g_ui.status.data : "",
         DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void paint_main(HDC dc)
{
    RECT r = g_ui.r_side, t = g_ui.r_top, c = g_ui.r_center;

    fill(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, C_PANEL);
    r.left += S(20);
    r.top += S(22);
    text(dc, g_ui.f_label, C_MUTED, r, "SERVERS", DT_LEFT | DT_SINGLELINE);

    fill(dc, t.left, t.bottom - 1, t.right - t.left, 1, C_BORDER);
    t.left += S(28);
    t.bottom -= S(22);
    t.top += S(12);
    text(dc, g_ui.f_title, C_INK, t, g_ui.account.data ? g_ui.account.data : "",
         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    t.top = t.bottom;
    t.bottom = t.top + S(18);
    text(dc, g_ui.f_small, g_ui.disconnected ? C_AMBER : C_MUTED, t,
         g_ui.status.data ? g_ui.status.data : "", DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    draw_wordmark(dc, g_ui.r_word.left, g_ui.r_word.top, g_ui.word_unit);
    c.top = g_ui.r_word.bottom + S(28);
    text(dc, g_ui.f_body, C_INK, c, "Pick a server on the left.", DT_CENTER | DT_SINGLELINE);
    c.top += S(24);
    text(dc, g_ui.f_body, C_MUTED, c, "Reading and sending messages comes in the next version.",
         DT_CENTER | DT_SINGLELINE);
}

static void paint(HWND wnd)
{
    PAINTSTRUCT ps;
    RECT rc;
    HDC dc = BeginPaint(wnd, &ps), mem;
    HBITMAP bmp, old;

    GetClientRect(wnd, &rc);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    old = SelectObject(mem, bmp);
    fill(mem, 0, 0, rc.right, rc.bottom, C_BG);

    if (g_ui.view == VIEW_MAIN)
        paint_main(mem);
    else if (g_ui.view == VIEW_LOADING)
        paint_loading(mem);
    else
        paint_login(mem);

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(wnd, &ps);
}

/* ---- Layout ---- */

static void place(HWND h, int show, int x, int y, int w, int hgt)
{
    if (show)
        MoveWindow(h, x, y, w, hgt, TRUE);
    ShowWindow(h, show ? SW_SHOWNA : SW_HIDE);
}

static void set_button_text(HWND h, const wchar_t *s)
{
    SetWindowTextW(h, s);
    InvalidateRect(h, NULL, FALSE);
}

static void layout(void)
{
    RECT rc;
    int w, h, col, x, y, box, login = g_ui.view == VIEW_QR || g_ui.view == VIEW_TOKEN;

    GetClientRect(g_ui.wnd, &rc);
    w = rc.right;
    h = rc.bottom;

    if (g_ui.view == VIEW_MAIN) {
        int side = S(260), top = S(72), cw = w - side;
        SetRect(&g_ui.r_side, 0, 0, side, h);
        SetRect(&g_ui.r_top, side, 0, w, top);
        g_ui.word_unit = S(3);
        x = side + (cw - word_width(g_ui.word_unit)) / 2;
        y = top + (h - top) / 2 - S(60);
        SetRect(&g_ui.r_word, x, y, x + word_width(g_ui.word_unit), y + 7 * g_ui.word_unit);
        SetRect(&g_ui.r_center, side, 0, w, h);
        place(g_ui.guilds, 1, S(8), S(52), side - S(16), h - S(60));
        place(g_ui.logout, 1, w - S(28) - S(90), S(22), S(90), S(28));
        place(g_ui.retry, g_ui.disconnected, side + (cw - S(160)) / 2, g_ui.r_word.bottom + S(96), S(160), S(40));
        place(g_ui.toggle, 0, 0, 0, 0, 0);
        place(g_ui.login, 0, 0, 0, 0, 0);
        place(g_ui.token, 0, 0, 0, 0, 0);
    } else {
        col = S(400);
        box = S(240);
        g_ui.word_unit = S(4);
        y = (h - S(520)) / 2;
        if (y < S(24))
            y = S(24);
        if (!login)
            y = h / 2 - S(40);
        x = (w - word_width(g_ui.word_unit)) / 2;
        SetRect(&g_ui.r_word, x, y, x + word_width(g_ui.word_unit), y + 7 * g_ui.word_unit);
        y = g_ui.r_word.bottom + S(36);
        x = (w - col) / 2;
        if (g_ui.view == VIEW_TOKEN) {
            /* Title and hint first, then the form, in the same overall height as the QR view. */
            SetRect(&g_ui.r_title, x, y, x + col, y + S(28));
            SetRect(&g_ui.r_hint, x, y + S(34), x + col, y + S(34) + S(60));
            y += S(34) + S(60) + S(36);
            SetRect(&g_ui.r_field, x, y, x + col, y + S(40));
            y = g_ui.r_word.bottom + S(36) + box + S(28) + S(34) + S(60) + S(10);
        } else {
            SetRect(&g_ui.r_box, (w - box) / 2, y, (w + box) / 2, y + box);
            y += box + S(28);
            SetRect(&g_ui.r_title, x, y, x + col, y + S(28));
            SetRect(&g_ui.r_hint, x, y + S(34), x + col, y + S(34) + S(60));
            y += S(34) + S(60) + S(10);
        }
        SetRect(&g_ui.r_status, x - S(60), y, x + col + S(60), y + S(20));
        if (!login)
            SetRect(&g_ui.r_status, 0, g_ui.r_word.bottom + S(28), w, g_ui.r_word.bottom + S(48));

        place(g_ui.toggle, login, x, y + S(32), col, S(28));
        place(g_ui.token, g_ui.view == VIEW_TOKEN, g_ui.r_field.left + S(12), g_ui.r_field.top + S(10),
              col - S(24), S(22));
        place(g_ui.login, g_ui.view == VIEW_TOKEN, x, g_ui.r_field.bottom + S(16), col, S(40));
        place(g_ui.guilds, 0, 0, 0, 0, 0);
        place(g_ui.logout, 0, 0, 0, 0, 0);
        place(g_ui.retry, 0, 0, 0, 0, 0);
        set_button_text(g_ui.toggle, g_ui.view == VIEW_QR ? L"Use a token instead" : L"Use a QR code instead");
    }
    InvalidateRect(g_ui.wnd, NULL, FALSE);
}

static void set_view(int view)
{
    g_ui.view = view;
    layout();
    if (view == VIEW_TOKEN)
        SetFocus(g_ui.token);
    else
        SetFocus(g_ui.wnd);
}

/* ---- Fonts, DPI, icon ---- */

static HFONT make_font(int px, int weight)
{
    return CreateFontW(-S(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

static void make_fonts(void)
{
    HFONT old[4] = {g_ui.f_title, g_ui.f_body, g_ui.f_small, g_ui.f_label};

    g_ui.f_title = make_font(20, FW_SEMIBOLD);
    g_ui.f_body = make_font(14, FW_NORMAL);
    g_ui.f_small = make_font(12, FW_NORMAL);
    g_ui.f_label = make_font(11, FW_BOLD);
    for (int i = 0; i < 4; i++)
        if (old[i])
            DeleteObject(old[i]);
    SendMessageW(g_ui.token, WM_SETFONT, (WPARAM)g_ui.f_body, FALSE);
    SendMessageW(g_ui.guilds, WM_SETFONT, (WPARAM)g_ui.f_body, FALSE);
    SendMessageW(g_ui.guilds, LB_SETITEMHEIGHT, 0, S(36));
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

/* ---- Owner drawing ---- */

static void draw_button(const DRAWITEMSTRUCT *di)
{
    wchar_t label[64];
    int kind = (int)GetWindowLongPtrW(di->hwndItem, GWLP_USERDATA);
    RECT r = di->rcItem;
    HDC dc = di->hDC;
    int pressed = (di->itemState & ODS_SELECTED) != 0;

    GetWindowTextW(di->hwndItem, label, ARRAYSIZE(label));
    fill(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, C_BG);
    if (kind == BTN_PRIMARY) {
        fill(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, pressed ? RGB(0xD9, 0x96, 0x00) : C_AMBER);
        text_w(dc, g_ui.f_body, C_BG, r, label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        text_w(dc, g_ui.f_body, pressed ? C_INK : C_AMBER, r, label,
               DT_VCENTER | DT_SINGLELINE | (di->hwndItem == g_ui.logout ? DT_RIGHT : DT_CENTER));
    }
}

static void draw_guild(const DRAWITEMSTRUCT *di)
{
    wchar_t name[256];
    RECT r = di->rcItem;
    HDC dc = di->hDC;
    int selected = (di->itemState & ODS_SELECTED) != 0;

    if (di->itemID == (UINT)-1)
        return;
    fill(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, selected ? C_SELECT : C_PANEL);
    if (selected)
        fill(dc, r.left, r.top + S(8), S(3), r.bottom - r.top - S(16), C_AMBER);
    if (SendMessageW(di->hwndItem, LB_GETTEXTLEN, di->itemID, 0) < (LRESULT)ARRAYSIZE(name)) {
        SendMessageW(di->hwndItem, LB_GETTEXT, di->itemID, (LPARAM)name);
        r.left += S(14);
        r.right -= S(8);
        text_w(dc, g_ui.f_body, selected ? C_INK : C_MUTED, r, name,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

/* ---- Token field ---- */

static void submit_token(void)
{
    int n = GetWindowTextLengthW(g_ui.token);
    wchar_t *w;
    sb_t token = {0};
    size_t a = 0, b;

    if (n <= 0)
        return;
    w = mem_alloc(((size_t)n + 1) * sizeof(wchar_t));
    GetWindowTextW(g_ui.token, w, n + 1);
    SetWindowTextW(g_ui.token, L"");
    wide_to_utf8(w, (size_t)n, &token);
    SecureZeroMemory(w, ((size_t)n + 1) * sizeof(wchar_t));
    mem_free(w);

    b = token.len;
    while (a < b && (token.data[a] == ' ' || token.data[a] == '"' || token.data[a] == '\''))
        a++;
    while (b > a && (token.data[b - 1] == ' ' || token.data[b - 1] == '"' || token.data[b - 1] == '\'' ||
                     token.data[b - 1] == '\r' || token.data[b - 1] == '\n'))
        b--;
    if (b > a) {
        token.data[b] = 0;
        app_login_token(token.data + a);
    }
    sb_free(&token);
}

static LRESULT CALLBACK edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CHAR && wp == VK_RETURN) {
        submit_token();
        return 0;
    }
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS)
        InvalidateRect(g_ui.wnd, &g_ui.r_field, FALSE);
    return CallWindowProcW(g_ui.edit_proc, h, msg, wp, lp);
}

/* ---- Messages from workers ---- */

static void fill_guilds(const sb_t *payload)
{
    const char *p = payload->data, *end = payload->data + payload->len;
    int first = 1;

    SendMessageW(g_ui.guilds, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_ui.guilds, LB_RESETCONTENT, 0, 0);
    while (p < end) {
        const char *nl = p;
        while (nl < end && *nl != '\n')
            nl++;
        if (first) {
            sb_clear(&g_ui.account);
            sb_addn(&g_ui.account, p, (size_t)(nl - p));
            first = 0;
        } else if (nl > p) {
            wchar_t *w = utf8_to_wide(p, (size_t)(nl - p));
            SendMessageW(g_ui.guilds, LB_ADDSTRING, 0, (LPARAM)w);
            mem_free(w);
        }
        p = nl + 1;
    }
    SendMessageW(g_ui.guilds, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_ui.guilds, NULL, TRUE);
}

static void on_worker(UINT msg, sb_t *p)
{
    const char *s = p ? p->data : "";

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
        set_text(&g_ui.scanned, *s ? s : "your account");
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
        SendMessageW(g_ui.guilds, LB_RESETCONTENT, 0, 0);
        set_view(VIEW_MAIN);
        break;
    case UI_READY:
        fill_guilds(p);
        set_text(&g_ui.status, "Online");
        break;
    case UI_DISCONNECTED:
        g_ui.disconnected = 1;
        set_text(&g_ui.status, s);
        if (g_ui.view == VIEW_LOADING)
            set_view(VIEW_MAIN);
        else
            layout();
        break;
    }
    InvalidateRect(g_ui.wnd, NULL, FALSE);
}

void ui_show_login(void)
{
    if (g_ui.qr) {
        mem_free(g_ui.qr);
        g_ui.qr = NULL;
    }
    sb_clear(&g_ui.scanned);
    set_text(&g_ui.status, "");
    g_ui.disconnected = 0;
    set_view(VIEW_QR);
}

void ui_show_loading(const char *s)
{
    set_text(&g_ui.status, s);
    set_view(VIEW_LOADING);
}

/* ---- Window procedure ---- */

static void on_command(int id)
{
    switch (id) {
    case ID_TOGGLE:
        set_view(g_ui.view == VIEW_QR ? VIEW_TOKEN : VIEW_QR);
        break;
    case ID_LOGIN:
        submit_token();
        break;
    case ID_LOGOUT:
        app_logout();
        break;
    case ID_RETRY:
        g_ui.disconnected = 0;
        set_text(&g_ui.status, "Connecting\xE2\x80\xA6");
        layout();
        app_reconnect();
        break;
    }
}

static HWND make_button(HWND parent, int id, int kind, const wchar_t *label)
{
    HWND h = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                             0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, NULL, NULL);

    SetWindowLongPtrW(h, GWLP_USERDATA, kind);
    return h;
}

static LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_ui.wnd = wnd;
        g_ui.dpi = GetDpiForWindow(wnd);
        g_ui.toggle = make_button(wnd, ID_TOGGLE, BTN_LINK, L"Use a token instead");
        g_ui.login = make_button(wnd, ID_LOGIN, BTN_PRIMARY, L"Log in");
        g_ui.logout = make_button(wnd, ID_LOGOUT, BTN_LINK, L"Log out");
        g_ui.retry = make_button(wnd, ID_RETRY, BTN_PRIMARY, L"Reconnect");
        g_ui.token = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
                                     0, 0, 0, 0, wnd, (HMENU)(INT_PTR)ID_TOKEN, NULL, NULL);
        g_ui.edit_proc = (WNDPROC)SetWindowLongPtrW(g_ui.token, GWLP_WNDPROC, (LONG_PTR)edit_proc);
        g_ui.guilds = CreateWindowExW(0, L"LISTBOX", L"",
                                      WS_CHILD | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                                          LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
                                      0, 0, 0, 0, wnd, (HMENU)(INT_PTR)ID_GUILDS, NULL, NULL);
        make_fonts();
        return 0;
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = MulDiv(720, GetDpiForWindow(wnd), 96);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = MulDiv(600, GetDpiForWindow(wnd), 96);
        return 0;
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        g_ui.dpi = HIWORD(wp);
        make_fonts();
        SetWindowPos(wnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(wnd);
        return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, C_INK);
        SetBkColor((HDC)wp, C_FIELD);
        return (LRESULT)g_ui.b_field;
    case WM_CTLCOLORLISTBOX:
        return (LRESULT)g_ui.b_panel;
    case WM_MEASUREITEM:
        ((MEASUREITEMSTRUCT *)lp)->itemHeight = S(36);
        return TRUE;
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *di = (const DRAWITEMSTRUCT *)lp;
        if (di->CtlType == ODT_BUTTON)
            draw_button(di);
        else if (di->CtlType == ODT_LISTBOX)
            draw_guild(di);
        return TRUE;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED)
            on_command(LOWORD(wp));
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(wnd);
        return 0;
    case WM_CLOSE:
        app_quit();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        if (msg >= UI_QR && msg <= UI_DISCONNECTED) {
            sb_t *p = (sb_t *)lp;
            on_worker(msg, p);
            if (p) {
                sb_free(p);
                mem_free(p);
            }
            return 0;
        }
        return DefWindowProcW(wnd, msg, wp, lp);
    }
}

HWND ui_create(HINSTANCE inst)
{
    WNDCLASSEXW wc = {0};
    BOOL dark = TRUE;
    COLORREF caption = C_BG;
    UINT dpi = GetDpiForSystem();

    g_ui.b_field = CreateSolidBrush(C_FIELD);
    g_ui.b_panel = CreateSolidBrush(C_PANEL);
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

    g_ui.wnd = CreateWindowExW(0, L"Silicord", L"Silicord", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                               CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(1000, dpi, 96), MulDiv(680, dpi, 96),
                               NULL, NULL, inst, NULL);
    DwmSetWindowAttribute(g_ui.wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    DwmSetWindowAttribute(g_ui.wnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof caption);
    return g_ui.wnd;
}
