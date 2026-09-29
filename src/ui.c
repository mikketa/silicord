/*
 * Custom-drawn Win32 UI: server rail, channel list, main pane. Everything is
 * painted by the renderer (render.h); the only child windows are the EDIT
 * controls and the popups that hold them (profile, emoji picker, quick switcher).
 * Runs on the UI thread only.
 */
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <commdlg.h>
#include <objbase.h>
#include <psapi.h>
#include <mmsystem.h>
#include "ui.h"
#include "render.h"
#include "img.h"
#include "md.h"
#include "memberlist.h"
#include "picture.h"
#include "screen.h"
#include "command.h"
#include "search.h"
#include "stats.h"
#include "emoji.h"
#include "mem.h"
#include "qr.h"
#include "utf.h"
#include "voice.h"
#include "audio.h"
#include "vsync.h"

/*
 * Palette (GDI COLORREF and ARGB): Discord's "Midnight" dark theme, its design
 * tokens flattened on black (the translucent ones mixed onto the surface they sit on).
 */
#define RGBX(r, g, b) RGB(r, g, b), (0xFF000000u | ((r) << 16) | ((g) << 8) | (b))
enum { C_RAIL, C_SIDE, C_MAIN, C_PANEL, C_ITEM, C_HOVER, C_SELECT, C_LINE, C_INK, C_MUTED, C_FAINT, C_BRAND, C_GREEN, C_TIP,
       C_MENTION, C_MENTION_HOVER, C_NEW, C_WARN, C_RED, C_TEXT, C_CHANNEL, C_CHANNEL_MUTED, C_COUNT };
static const struct { COLORREF gdi; unsigned argb; } k_color[C_COUNT] = {
    {RGBX(0x00, 0x00, 0x00)}, /* rail and title bar: app-frame-background */
    {RGBX(0x00, 0x00, 0x00)}, /* channel column: background-base-lowest */
    {RGBX(0x00, 0x00, 0x00)}, /* main pane: chat-background */
    {RGBX(0x00, 0x00, 0x00)}, /* user panel */
    {RGBX(0x12, 0x12, 0x14)}, /* buttons, placeholders: background-surface-higher */
    {RGBX(0x12, 0x12, 0x13)}, /* hovered row: interactive-background-hover */
    {RGBX(0x24, 0x24, 0x26)}, /* selected row: interactive-background-selected */
    {RGBX(0x1E, 0x1E, 0x20)}, /* separators: border-subtle */
    {RGBX(0xDC, 0xDC, 0xDF)}, /* names, titles, active: text-strong */
    {RGBX(0x96, 0x97, 0x9E)}, /* secondary text, idle icons: interactive-text-default */
    {RGBX(0x81, 0x82, 0x8A)}, /* hints, timestamps: text-muted */
    {RGBX(0x58, 0x65, 0xF2)}, /* accent: brand-500 */
    {RGBX(0x23, 0xA5, 0x5A)}, /* online: status-online */
    {RGBX(0x17, 0x18, 0x1B)}, /* tooltip: background-surface-highest */
    {RGBX(0x13, 0x0D, 0x00)}, /* message that mentions us: message-mentioned-background */
    {RGBX(0x1A, 0x12, 0x02)}, /* same, hovered */
    {RGBX(0xF2, 0x3F, 0x43)}, /* NEW line: status-danger */
    {RGBX(0xF0, 0xB2, 0x32)}, /* connecting, warnings: status-warning */
    {RGBX(0xF2, 0x3F, 0x43)}, /* errors, destructive actions: status-danger */
    {RGBX(0xD4, 0xD5, 0xD8)}, /* message text: text-default */
    {RGBX(0x7A, 0x7B, 0x83)}, /* idle channels, their icons, list headers: channels-default */
    {RGBX(0x31, 0x31, 0x35)}, /* muted channels: channels-default at 40% */
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
#define ICON 40
#define RAIL_STEP 48
#define RAIL_FIRST 56 /* the first server, under home and its separator */
#define PANEL_GAP 8   /* around the floating user panel */
#define SEARCH_W 244  /* the chat header search box */
#define SIDE_W 240
#define HEADER_H 48
#define PANEL_H 56
#define VOICE_BAR_H 92 /* above the user panel while in voice: its state, then a row of buttons */
#define VOICE_ROW1_H 52
#define CAT_H 40
#define ROW_H 34

enum { VIEW_LOGIN, VIEW_LOADING, VIEW_APP };
enum { HIT_NONE, HIT_HOME, HIT_GUILD, HIT_CHANNEL, HIT_LOGOUT, HIT_RETRY, HIT_SELF, HIT_FRIENDS, HIT_FOLDER, HIT_VOICE_LEAVE, HIT_VOICE_MUTE, HIT_VOICE_DEAF, HIT_VOICE_CAMERA, HIT_VOICE_SHARE,
       HIT_MIC, HIT_DEAFEN, HIT_VOICE_MENU, HIT_NAV, HIT_DM_SEARCH, HIT_NEW_DM, HIT_GUILD_HEADER };

typedef struct {
    char key[96];
    unsigned hash;   /* of key: lookups compare it first */
    r_image_t *img;
    unsigned used;   /* paint that last drew it */
    HWND wnd;        /* window that drew it, for animations */
} image_t;

/* A panel's messages measured once per paint (the panel is painted in bands): body and extras heights. */
typedef struct {
    int *th, *eh;
    unsigned frame;
} panel_layout_t;

/* Decoded images kept in memory; images off screen beyond this are dropped (the disk cache keeps them). */
#define IMAGE_BUDGET (16u << 20)
/* Time a paint may spend decoding images from the disk cache before deferring the rest. */
#define SYNC_DECODE_MS 8

#define PICK_W 420
#define PICK_H 440
#define PICK_COLS 9
#define PICK_CELL 40
#define PICK_TOP 92
#define PICK_TABS 44
#define PICK_FOOT 52
#define PICK_HEAD 28

enum { PICK_COMPOSER, PICK_REACTION };
enum { PI_HEADER, PI_UNICODE, PI_CUSTOM, PI_STICKER };
enum { TAB_EMOJI, TAB_GIFS, TAB_STICKERS };
#define STICKER_COLS 4

typedef struct {
    int kind;
    int index;          /* unicode: into k_emoji; header: category, or -1 - guild for a server */
    char id[24];        /* custom emoji, sticker */
    char name[40];
    int animated;       /* stickers: their format_type */
    int x, y, w, h;     /* in the picker, before scrolling */
} pick_item_t;

typedef struct {
    char id[24];        /* user id */
    int type;           /* 1 friend, 2 blocked, 3 incoming request, 4 outgoing request */
    sb_t name, username;
    char avatar[48];
} relation_t;

typedef struct {
    sb_t path;          /* UTF-8 */
    long long size;
    r_image_t *thumb;
} upload_t;

typedef struct {
    char guild[24], channel[24], user[24];
    int flags;          /* VOICE_* */
    sb_t name;
    char avatar[40];
    int asked;          /* its name was asked for */
} voice_t;

#define AC_MAX 10
#define AC_ROW 40

enum { AC_NONE, AC_USER, AC_CHANNEL, AC_EMOJI, AC_COMMAND, AC_OPTION };

typedef struct {
    char label[80];      /* shown */
    char insert[96];     /* put in the composer */
    char id[24];         /* user or channel id, for the mention on send */
    int emoji;           /* unicode emoji index, -1 otherwise */
    char custom[24];     /* custom emoji id */
    char avatar[48];     /* user avatar, or the application's icon for commands */
    char detail[120];    /* commands and options: their description */
    char app[48];        /* commands: the application's name; options: "required" */
} ac_item_t;

/* A mention picked from the suggestions: its text in the composer and what is sent. */
typedef struct {
    char text[96];
    char markup[32];
} mention_t;

enum { BAR_NONE, BAR_REPLY, BAR_EDIT };
#define BAR_H 36

typedef struct {
    char user[24];
    sb_t name;
    DWORD until;
} typing_t;

typedef struct {
    HWND wnd;          /* the app, a child of `top` under the title bar */
    HWND top;          /* the window itself: frame and title bar */
    HWND saved_focus;  /* given back when the window is activated again */
    int tb_hover, tb_down;
    /* Back and forward in the title bar: channel ids, "" for the friends page. */
    char hist[32][24];
    int hist_n, hist_pos, hist_nav;
    r_font_t *f_title, *f_h, *f_body, *f_small, *f_cat, *f_icon, *f_icon_big, *f_initial, *f_initial_small;
    r_font_t *f_mono, *f_h1, *f_h2, *f_h3, *f_name, *f_emoji, *f_icon_mid, *f_caption, *f_tb, *f_icon_tb;
    r_font_t *f_nav, *f_section, *f_small_mid, *f_gif, *f_nitro, *f_nitro_h, *f_nitro_card, *f_h1x, *f_menu, *f_chan, *f_welcome;
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
    int camera_on_connect; /* the video button started the call: the camera follows */
    int pop_docked;        /* the popout is the profile panel beside a DM */
    int home_page; /* HOME_*: what home shows when no conversation is open */
    int shop_state; /* 0 not asked, 1 loading, 2 loaded, 3 failed */
    int quests_state; /* the same, for the Quests page */
    int nitro_hover;  /* NB_*: what the pointer is over on the Nitro page */
    unsigned char folder_open[64];
    int hover_kind, hover_index;
    image_t *images;
    int nimages, cap_images;
    unsigned frame;            /* paint counter, for the image cache */
    HWND paint_wnd;            /* window being painted */
    int image_first;           /* requests jump the queue (message images) */
    int anim_timer;
    int log_memory;
    LARGE_INTEGER frame_start, qpf;

    /* Messages of the open channel, oldest first. */
    msg_t *msgs;
    int nmsgs, cap_msgs;
    char msgs_channel[24];
    int msgs_loading, msgs_older_loading, msgs_has_more, msgs_status;
    int msg_scroll;            /* distance from the bottom, in pixels */
    char new_after[24];        /* messages after this one were unread when the channel opened */
    char flash_id[24];         /* message briefly highlighted after a jump */
    msg_batch_t *pins;         /* pinned messages panel, NULL when closed */
    HWND search_edit;
    WNDPROC search_proc;
    msg_batch_t *results;      /* search results, NULL while searching */
    int results_open, results_scroll, results_content, result_hover;
    int result_y[64], result_h[64];
    panel_layout_t result_layout;
    int detached;              /* showing older messages after a jump: new ones are not appended */
    void *posts;               /* forum posts (post_t) */
    int nposts, forum_loaded, forum_scroll, forum_content, post_hover;
    int post_y[64];
    int pins_open, pins_scroll, pins_content;
    int pins_inbox;            /* the panel shows our recent mentions instead */
    int settings_open, settings_page, settings_hover, settings_edit_y;
    HWND settings_edit;        /* custom status */
    WNDPROC settings_edit_proc;
    struct {
        RECT r;
        int id;
    } set_hits[32];            /* what the settings screen drew, for clicks */
    int nset_hits;
    int pref_notify, pref_title; /* this computer's settings */
    voice_t *voices;           /* who is in the servers' voice channels */
    int nvoices, cap_voices;
    int voice_state;           /* our own connection, VOICE_* */
    char voice_channel[24];
    char voice_name[100];      /* its channel name */
    unsigned voice_speaking;   /* who spoke at the last check, one bit per member of our call */
    int voice_muted, voice_deafened, voice_camera;
    int share_state;           /* our Go Live stream's connection, VOICE_* (VOICE_OFF when not sharing) */
    char watch_user[24];       /* whose stream we watch, empty for nobody */
    int watch_state;           /* its connection, VOICE_* */
    struct {
        RECT r;
        char user[24];
    } live_hits[16];           /* the streams' tiles drawn with a button to watch them */
    int nlive_hits;
    RECT stream_close;         /* the watched stream's close button */
    int share_pick;            /* the screen picker is open */
    int share_pick_hover;      /* what the mouse is over in it: a screen, PICK_SOUND, PICK_CANCEL, else -1 */
    int share_no_sound;        /* share the screen without the sound of other programs */
    screen_info_t screens[8];  /* the screens it offers, and a still of each */
    r_image_t *screen_stills[8];
    int nscreens;
    struct {
        char channel[24];
        int ringing;           /* we are being rung */
    } calls[16];               /* calls going on in our direct messages */
    int ncalls, ring_sound;
    struct {
        char user[24];
        int stream;            /* their Go Live stream rather than their camera */
        r_image_t *img;
        unsigned serial;
    } video[16];               /* the latest picture of each video in our call */
    RECT call_join, call_decline, call_leave, card_join, card_decline;
    char card_channel[24];     /* the call the incoming call card is about */
    voice_prefs_t vprefs;      /* this computer's voice settings */
    wchar_t dev_in[16][AUDIO_NAME], dev_out[16][AUDIO_NAME];
    int ndev_in, ndev_out;
    int set_drag;              /* the settings slider being dragged, or 0 */
    int set_record;            /* waiting for the push to talk key */
    int mic_test, mic_db;      /* the microphone test runs; the meter's last level */
    struct {
        char user[24];
        short volume, muted;
    } uvol[64];                /* people's volumes in calls */
    int nuvol;
    sb_t voice_status;
    int pin_top[64], pin_h[64]; /* where the panel's messages are, unscrolled, for clicks */
    panel_layout_t pin_layout;
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
    int pop_status_y[4];
    unsigned pop_frame;        /* its last paint, whose images the main window keeps */
    int pop_badge_x[32], pop_badge_y[32];
    md_doc_t pop_bio;
    r_rich_t *pop_rich;
    int pop_rich_w;
    char pending_dm[24];      /* user whose new DM we open once it exists */

    /* Statuses of friends and people we see (PRESENCE_UPDATE). */
    struct presence *presences;
    int npresences, cap_presences;
    int my_status;             /* ML_*, what we set */

    /* Friends. */
    relation_t *rels;
    int nrels, cap_rels;
    int friend_tab, friend_scroll, friend_hover, friend_act;
    int tab_x[5], tab_w[5];
    HWND friend_edit;
    HWND friend_search;   /* filters the friends list by name */
    sb_t friend_result;

    /* Composer suggestions. */
    ac_item_t ac[AC_MAX];
    int ac_n, ac_sel, ac_kind, ac_start, ac_end, ac_ate;
    char ac_query[64];
    mention_t mention[32];
    int nmention;

    /* Quick switcher. */
    HWND qs, qs_edit;
    unsigned qs_frame;
    WNDPROC qs_edit_proc;
    HBRUSH qs_brush;
    int qs_kind[12], qs_index[12], nqs, qs_sel;

    /* Files to send with the next message. */
    upload_t uploads[10];
    int nuploads, upload_hover, hover_attach, hover_emoji, bar_hover;

    /* Emoji picker. */
    HWND picker, picker_edit;
    unsigned picker_frame;
    WNDPROC picker_edit_proc;
    HBRUSH picker_brush;
    int picker_mode;
    char picker_msg[24];
    void *pick_items;
    int npick, pick_scroll, pick_hover, pick_content;
    int picker_tab;            /* TAB_EMOJI, TAB_GIFS or TAB_STICKERS */
    sb_t gif_json;             /* last GIF answer */
    char qs_forward[24];       /* the switcher picks where to forward this message, when set */
    int qs_prompt;             /* PROMPT_*: the switcher asks for a name instead */
    char qs_prompt_id[24];     /* the message to start a thread from */
    char qs_prompt_channel[24];
    sb_t qs_prompt_title;      /* a forum post's title, while its message is asked */
    RECT forum_new;            /* the forum's New Post button */
    char react_key[96];        /* the reaction under the pointer: "message id emoji", empty if none */
    int react_x, react_y;      /* where to show who reacted */
    int react_shown;           /* its tooltip is up (after a short pause) */
    sb_t reactors;             /* "Ann, Bob and 3 others reacted with :x:" for react_key, once known */
    char reactors_key[96];
    char qs_forward_from[24];
    sb_t cmd_index;            /* slash commands of cmd_key's server or DM */
    char cmd_key[24];
    int cmd_loading;
    int gif_x[40], gif_y[40], gif_w[40], gif_h[40], ngif;

    /* Member list. */
    ml_t ml;
    int show_members, ml_scroll, ml_hover, ml_chunk;

    /* Message actions. */
    int hover_tool;            /* toolbar button under the mouse, -1 none */
    int bar;                   /* BAR_* above the composer */
    char bar_msg[24];
    sb_t bar_name;
    int bar_mention;
    int confirm, confirm_hover;
    char confirm_id[24];
    typing_t typing[8];
    int ntyping;
    DWORD typing_sent;

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

typedef struct presence {
    char user[24];
    unsigned hash;      /* of user: lookups compare it first */
    int status;
    sb_t activity;
    /* The first activity that is not a custom status, for Active Now's card. */
    sb_t game, details, state, image, album; /* image: a full URL, empty if none we can show */
    long long start, end;                    /* ms since 1970 it began and ends, 0 if not told */
    int listening;                           /* Spotify: a song, by an artist, on an album */
} presence_t;

typedef struct member {
    char guild[24];
    char user[24];
    unsigned hash;      /* of user: lookups compare it first */
    int known;          /* 0 asked, 1 answered */
    sb_t nick;
    sb_t roles;         /* comma-separated ids */
    unsigned color;     /* cached for `color_model` */
    const model_t *color_model;
} member_t;

static ui_t g_ui = {.guild = -1, .channel = -1, .hover_msg = -1, .notified_channel = -1, .hover_tool = -1,
                    .show_members = 1, .ml_hover = -1, .my_status = ML_ONLINE, .upload_hover = -1,
                    .friend_hover = -1};

static void paint_status(int x, int y, int d, int status, unsigned bg);
static const char *status_name(void);
static void status_dot(int cx, int cy, int s, int status, unsigned bg);
static presence_t *presence_find(const char *user);
static void anim_schedule(void);
static int friends_view(void);
static void rel_store(json_t obj);
static relation_t *rel_find(const char *id);
static void rel_remove(const char *id);

#define WM_TRAY (WM_APP + 60)
#define WM_VSYNC (WM_APP + 61) /* a refresh of the display, while something moves */
#define TIMER_ACK 1
#define TIMER_FLASH 3
#define TIMER_REACTORS 9
#define TIMER_VOICE 10 /* refreshes who is speaking in our call */
#define TIMER_MIC 11   /* the microphone meter in the voice settings */
#define TIMER_ACTIVE 14 /* Active Now's elapsed times, each second while shown */
#define REACTORS_DELAY 400
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

static void activity_free(activity_t *a)
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
    fill(x + 48 * unit, y, 4 * unit, 7 * unit, C_BRAND);
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

/* FNV-1a. */
static unsigned key_hash(const char *key)
{
    unsigned h = 2166136261u;

    while (*key)
        h = (h ^ (unsigned char)*key++) * 16777619u;
    return h;
}

static image_t *image_find(const char *key)
{
    unsigned h = key_hash(key);

    for (int i = 0; i < g_ui.nimages; i++)
        if (g_ui.images[i].hash == h && lstrcmpA(g_ui.images[i].key, key) == 0)
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

/* ---- Transitions ---- */

/*
 * Hover and selection ease in and out instead of snapping, as in Discord. A
 * transition is keyed by what it moves (a hit kind or a TW_ key, and an index)
 * and depends only on the frame's time, so every band of a frame reads the
 * same value. While one runs, the rectangles it covers are repainted at each
 * refresh of the display (vsync.h); once all have settled nothing wakes up.
 * Popups are not animated: their values jump to the target.
 */
#define TWEENS 64
#define TW_FAST 120  /* hover backgrounds */
#define TW_SHAPE 200 /* server icons and their pill */
enum { TW_PILL = 1000, TW_FOLDER_PILL, TW_HOME, TW_HOME_PILL, TW_MESSAGE, TW_MEMBER, TW_FRIEND, TW_ATTACH, TW_EMOJI, TW_MENTION,
       TW_BAR_CLOSE, TW_SHOP, TW_QUEST, TW_NITRO };

typedef struct {
    int kind, index;
    float from, to;
    unsigned t0, used; /* ms when it started; frame it was last read */
    int ms;
} tween_t;

static struct {
    tween_t t[TWEENS];
    int n;
    unsigned now;       /* ms, the same for the whole frame */
    RECT dirty, repaint; /* what running transitions cover: in this frame, and to repaint next */
} g_tween;

static float ease_out(float p)
{
    p = 1.f - p;
    return 1.f - p * p * p;
}

static float tween_at(const tween_t *t)
{
    unsigned el = g_tween.now - t->t0;

    if ((int)el >= t->ms)
        return t->to;
    return t->from + (t->to - t->from) * ease_out((float)(int)el / (float)t->ms);
}

/*
 * The value of kind/index easing to `target` over `ms`; (x, y, w, h) is where it
 * shows. `rest` is its value when nothing happens to it (not hovered, not
 * selected): only the ones away from it are remembered, a handful at a time.
 */
static float tween(int kind, int index, float target, float rest, int ms, int x, int y, int w, int h)
{
    tween_t *t = NULL;
    float v;

    if (g_ui.paint_wnd != g_ui.wnd)
        return target;
    for (int i = 0; i < g_tween.n && !t; i++)
        if (g_tween.t[i].kind == kind && g_tween.t[i].index == index)
            t = &g_tween.t[i];
    if (!t) {
        int i = g_tween.n;
        if (target == rest)
            return rest;
        if (i == TWEENS) { /* full: the one read longest ago goes */
            for (int k = i = 0; k < TWEENS; k++)
                if ((int)(g_tween.t[k].used - g_tween.t[i].used) < 0)
                    i = k;
        } else {
            g_tween.n++;
        }
        t = &g_tween.t[i];
        t->kind = kind;
        t->index = index;
        t->from = t->to = rest;
        t->t0 = g_tween.now - (unsigned)ms;
        t->ms = ms;
    }
    t->used = g_ui.frame;
    if (t->to != target) {
        t->from = tween_at(t);
        t->to = target;
        t->t0 = g_tween.now;
        t->ms = ms;
    }
    v = tween_at(t);
    if (v != t->to) {
        RECT r = {x, y, x + w, y + h};
        UnionRect(&g_tween.dirty, &g_tween.dirty, &r);
    } else if (t->to == rest) {
        *t = g_tween.t[--g_tween.n]; /* settled at rest: forget it */
    }
    return v;
}

/* 0 to 1 while kind/index is on, for a hover or a selection. */
static float tween_on(int kind, int index, int on, int ms, int x, int y, int w, int h)
{
    return tween(kind, index, on ? 1.f : 0.f, 0.f, ms, x, y, w, h);
}

static int lerp_i(int a, int b, float t)
{
    return a + (int)((float)(b - a) * t + (b > a ? .5f : -.5f));
}

/* ARGB colors mixed channel by channel. */
static unsigned lerp_argb(unsigned a, unsigned b, float t)
{
    unsigned out = 0;

    for (int s = 0; s < 32; s += 8)
        out |= (unsigned)lerp_i((int)(a >> s & 0xFF), (int)(b >> s & 0xFF), t) << s;
    return out;
}

/*
 * A row's background: it eases in on hover, and on to the selection's color.
 * Returns the color under the row (`base` when none), for the status dots cut out of it.
 */
static unsigned row_bg(int kind, int index, int sel, int hov, unsigned base, int x, int y, int w, int h, int radius)
{
    float t = tween(kind, index, sel ? 2.f : hov ? 1.f : 0.f, 0.f, TW_FAST, x, y, w, h);
    unsigned c = t <= 1.f ? lerp_argb(base, ARGB(C_HOVER), t) : lerp_argb(ARGB(C_HOVER), ARGB(C_SELECT), t - 1.f);

    if (t > 0.f)
        r_round(x, y, w, h, radius, c);
    return c;
}

/*
 * Popups (profiles, the emoji picker, the quick switcher) fade in instead of
 * appearing at once: layered while they fade, plain windows again after.
 */
#define FADE_MS 120
#define FADES 4

static struct {
    HWND wnd[FADES];
    LONGLONG t0[FADES];
    int n;
} g_fade;

static LONGLONG qpc_ms(void)
{
    LARGE_INTEGER now;

    if (!g_ui.qpf.QuadPart)
        QueryPerformanceFrequency(&g_ui.qpf);
    QueryPerformanceCounter(&now);
    return now.QuadPart * 1000 / g_ui.qpf.QuadPart;
}

static void fade_done(HWND w)
{
    SetLayeredWindowAttributes(w, 0, 255, LWA_ALPHA);
    SetWindowLongPtrW(w, GWL_EXSTYLE, GetWindowLongPtrW(w, GWL_EXSTYLE) & ~WS_EX_LAYERED);
}

/* Before the popup is shown. */
static void fade_in(HWND w)
{
    if (!w || g_fade.n == FADES)
        return;
    SetWindowLongPtrW(w, GWL_EXSTYLE, GetWindowLongPtrW(w, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(w, 0, 0, LWA_ALPHA);
    g_fade.wnd[g_fade.n] = w;
    g_fade.t0[g_fade.n++] = qpc_ms();
    vsync_request();
}

static void fade_tick(void)
{
    LONGLONG now = qpc_ms();

    for (int i = 0; i < g_fade.n;) {
        HWND w = g_fade.wnd[i];
        LONGLONG el = now - g_fade.t0[i];
        if (IsWindow(w) && el < FADE_MS) {
            SetLayeredWindowAttributes(w, 0, (BYTE)(ease_out((float)el / FADE_MS) * 255.f), LWA_ALPHA);
            i++;
            continue;
        }
        if (IsWindow(w))
            fade_done(w);
        g_fade.wnd[i] = g_fade.wnd[--g_fade.n];
        g_fade.t0[i] = g_fade.t0[g_fade.n];
    }
    if (g_fade.n)
        vsync_request();
}

static void tween_frame_start(void)
{
    if (!g_ui.qpf.QuadPart)
        QueryPerformanceFrequency(&g_ui.qpf);
    g_tween.now = (unsigned)(g_ui.frame_start.QuadPart * 1000 / g_ui.qpf.QuadPart);
    SetRectEmpty(&g_tween.dirty);
}

/* After a paint of the main window: what still moves is painted again at the next refresh. */
static void tween_frame_end(void)
{
    if (IsRectEmpty(&g_tween.dirty))
        return;
    UnionRect(&g_tween.repaint, &g_tween.repaint, &g_tween.dirty);
    vsync_request();
}

/* A refresh of the display: the transitions and fades still running take their next step. */
static void on_vsync(void)
{
    if (g_fade.n)
        fade_tick();
    if (!IsRectEmpty(&g_tween.repaint)) {
        InvalidateRect(g_ui.wnd, &g_tween.repaint, FALSE);
        SetRectEmpty(&g_tween.repaint);
    }
}

/* How long the process has run and the CPU time it used, in ms. */
static void process_times(unsigned long long *uptime_ms, unsigned long long *cpu_ms)
{
    FILETIME created, ended, kernel, user, now;
    ULARGE_INTEGER a, b, k, u;

    *uptime_ms = *cpu_ms = 0;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user))
        return;
    GetSystemTimeAsFileTime(&now);
    a.LowPart = created.dwLowDateTime, a.HighPart = created.dwHighDateTime;
    b.LowPart = now.dwLowDateTime, b.HighPart = now.dwHighDateTime;
    k.LowPart = kernel.dwLowDateTime, k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime, u.HighPart = user.dwHighDateTime;
    *uptime_ms = b.QuadPart > a.QuadPart ? (b.QuadPart - a.QuadPart) / 10000 : 0;
    *cpu_ms = (k.QuadPart + u.QuadPart) / 10000;
}

/* "Received: ..." and "CPU: ..." lines, for --debug and the About screen. */
static void usage_lines(sb_t *net, sb_t *cpu)
{
    unsigned long long uptime, cpu_ms;

    process_times(&uptime, &cpu_ms);
    stats_format(net, uptime);
    stats_format_cpu(cpu, cpu_ms, uptime);
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
    {
        sb_t net = {0}, cpu = {0};
        sb_add(&net, "[net] ");
        sb_add(&cpu, "[cpu] ");
        usage_lines(&net, &cpu);
        app_log(net.data);
        app_log(cpu.data);
        sb_free(&net);
        sb_free(&cpu);
    }
}

/*
 * Returns the image, from memory, else from the disk cache in the same frame
 * (while the paint has time left), else starts downloading it.
 */
static r_image_t *image_get(const char *key, const char *path, int max_px)
{
    image_t *im = image_find(key);

    if (im) {
        im->used = g_ui.frame;
        im->wnd = g_ui.paint_wnd;
        if (im->img && r_image_animated(im->img))
            anim_schedule();
        return im->img;
    }
    if (g_ui.nimages == g_ui.cap_images) {
        g_ui.cap_images = g_ui.cap_images ? g_ui.cap_images * 2 : 64;
        g_ui.images = mem_realloc(g_ui.images, (size_t)g_ui.cap_images * sizeof *g_ui.images);
    }
    im = &g_ui.images[g_ui.nimages++];
    lstrcpynA(im->key, key, sizeof im->key);
    im->hash = key_hash(im->key);
    im->used = g_ui.frame;
    im->wnd = g_ui.paint_wnd;
    im->img = frame_ms() < SYNC_DECODE_MS ? img_cached(path, max_px) : NULL;
    if (!im->img) {
        if (g_ui.image_first)
            img_request_first(key, path, max_px);
        else
            img_request(key, path, max_px);
    }
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

/* An animated avatar ("a_" hash) moves while `animate`, as Discord does on hover; otherwise its still image. */
static r_image_t *user_avatar_anim(const char *id, const char *hash, int animate)
{
    char key[96], path[160];
    r_image_t *img;

    if (!animate || hash[0] != 'a' || hash[1] != '_')
        return user_avatar(id, hash);
    wsprintfA(key, "ag:%s:%s", id, hash);
    wsprintfA(path, "/avatars/%s/%s.gif?size=64", id, hash);
    img = image_get(key, path, S(40));
    return img ? img : user_avatar(id, hash); /* the still one until the GIF arrives */
}

/* A member's avatar for one server (it lives under the guild), else their own. */
static r_image_t *member_avatar_anim(const char *guild, const char *id, const char *hash, const char *own, int animate)
{
    char key[128], path[200];
    int gif = animate && hash[0] == 'a' && hash[1] == '_';
    r_image_t *img;

    if (!guild || !guild[0] || !hash[0])
        return user_avatar_anim(id, own, animate);
    wsprintfA(key, "%s:%s:%s:%s", gif ? "mg" : "m", guild, id, hash);
    wsprintfA(path, "/guilds/%s/users/%s/avatars/%s.%s?size=64", guild, id, hash, gif ? "gif" : "png");
    img = image_get(key, path, S(40));
    if (!img && gif) /* the still one until the GIF arrives */
        return member_avatar_anim(guild, id, hash, own, 0);
    return img;
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

static void initials(const char *name, wchar_t *out, int max);

/* The next member of a group DM from model_t.recipients ("id\tavatar\tname\n"), or NULL at the end. */
static const char *recip_next(const char *p, char *id, char *avatar, char *name, int name_size)
{
    int n;

    if (!p || !*p)
        return NULL;
    for (n = 0; *p && *p != '\t' && n < 23; p++)
        id[n++] = *p;
    id[n] = 0;
    p += *p == '\t';
    for (n = 0; *p && *p != '\t' && n < 39; p++)
        avatar[n++] = *p;
    avatar[n] = 0;
    p += *p == '\t';
    for (n = 0; *p && *p != '\n'; p++)
        if (n < name_size - 1)
            name[n++] = *p;
    name[n] = 0;
    return p + (*p == '\n');
}

/*
 * A DM's picture at (x, y), d wide: the group's icon or the other person; a
 * group without one shows two of its members, one over the other, as in
 * Discord. `bg` is what it sits on, for the ring around the second face.
 */
static void paint_dm_icon(const channel_t *c, int x, int y, int d, unsigned bg)
{
    r_image_t *img = dm_icon(c);

    if (img) {
        r_image(img, x, y, d, d, d / 2);
    } else if (c->recipients) {
        const char *p = model_str(g_ui.model, c->recipients);
        char id[2][24] = {"", ""}, av[2][40] = {"", ""}, name[8];
        int f = d * 5 / 8, off = d - f;
        for (int k = 0; k < 2 && (p = recip_next(p, id[k], av[k], name, sizeof name)) != NULL; k++)
            ;
        for (int k = 0; k < 2; k++) {
            int fx = x + (k ? off : 0), fy = y + (k ? off : 0);
            r_image_t *face = id[k][0] ? user_avatar(id[k], av[k]) : NULL;
            if (k && !id[k][0])
                break;
            if (k)
                r_circle(fx - d / 16, fy - d / 16, f + d / 8, bg);
            if (face)
                r_image(face, fx, fy, f, f, f / 2);
            else
                r_circle(fx, fy, f, ARGB(C_ITEM));
        }
    } else {
        const char *name = model_str(g_ui.model, c->name);
        wchar_t ini[8];
        initials(name, ini, 8);
        r_circle(x, y, d, ARGB(C_ITEM));
        text_w(d > S(28) ? g_ui.f_small : g_ui.f_cat, C_INK, rect(x, y, d, d), ini, -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
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
    r_circle(x, y, S(24), ARGB(C_BRAND));
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
        text_w(g_ui.f_icon_big, C_BRAND, rect(tx, ty + S(52), tile, S(48)), ICON_CHECK, -1, DT_CENTER | DT_SINGLELINE);
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
        draw_mark(tx + tile / 2 - S(8), ty + tile / 2 - S(8), S(1), C_BRAND);
    } else {
        text(g_ui.f_body, C_RAIL, rect(tx, ty, tile, tile), "Getting a code\xE2\x80\xA6",
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    text(g_ui.f_body, C_BRAND, rect(0, card.bottom + S(24), rc.right, S(24)), str_or_empty(&g_ui.status),
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

/*
 * Rail layout: home, then guilds, with folders shown as one icon when closed,
 * or as a folder icon followed by their guilds when open. rail_rows() fills
 * g_rail with kind/index/y for each row (RAIL_GUILD: guild index, RAIL_FOLDER:
 * folder index) and returns the count.
 */
enum { RAIL_GUILD, RAIL_FOLDER };
#define RAIL_MAX 512

static struct {
    int kind[RAIL_MAX], index[RAIL_MAX], y[RAIL_MAX];
} g_rail;

static int folder_is_open(int f)
{
    return f < (int)sizeof g_ui.folder_open && g_ui.folder_open[f];
}

static int rail_rows(void)
{
    const model_t *m = g_ui.model;
    int n = 0, y = S(RAIL_FIRST) - g_ui.rail_scroll;

    for (unsigned i = 0; m && i < m->nguilds && n < RAIL_MAX; i++) {
        int f = m->guilds[i].folder;
        if (f >= 0 && (i == 0 || m->guilds[i - 1].folder != f)) {
            g_rail.kind[n] = RAIL_FOLDER;
            g_rail.index[n] = f;
            g_rail.y[n++] = y;
            y += S(RAIL_STEP);
        }
        if (f >= 0 && !folder_is_open(f))
            continue; /* closed folder */
        if (n < RAIL_MAX) {
            g_rail.kind[n] = RAIL_GUILD;
            g_rail.index[n] = (int)i;
            g_rail.y[n++] = y;
            y += S(RAIL_STEP);
        }
    }
    return n;
}

static int rail_y(int i) /* i = -1 for home; a guild in a closed folder gives its folder's row */
{
    int n;

    if (i < 0)
        return -g_ui.rail_scroll;
    n = rail_rows();
    for (int k = 0; k < n; k++)
        if (g_rail.kind[k] == RAIL_GUILD && g_rail.index[k] == i)
            return g_rail.y[k];
    for (int k = 0; k < n; k++)
        if (g_rail.kind[k] == RAIL_FOLDER && g_rail.index[k] == g_ui.model->guilds[i].folder)
            return g_rail.y[k];
    return -10000;
}

static int rail_folder_y(int f)
{
    int n = rail_rows();

    for (int k = 0; k < n; k++)
        if (g_rail.kind[k] == RAIL_FOLDER && g_rail.index[k] == f)
            return g_rail.y[k];
    return -10000;
}

static int rail_content(void)
{
    return S(12) + (rail_rows() + 1) * S(RAIL_STEP) + S(12) + S(12);
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
/* Above the conversations, as in Discord: Friends, Nitro, Shop and Quests, a line, then "Direct Messages". */
#define NAV_ROW 40
#define NAVS 4
#define DM_SECTION 37
#define DM_TOP (NAVS * NAV_ROW + 13 + DM_SECTION)
enum { HOME_FRIENDS, HOME_NITRO, HOME_SHOP, HOME_QUESTS };

/* ---- Voice channels: who is in them ---- */

static const char *open_guild_id(void);
static int is_voice_type(int type);

#define VOICE_ROW 30
enum { VOICE_MUTE = 1, VOICE_DEAF = 2, VOICE_STREAM = 4, VOICE_VIDEO = 8 };

static voice_t *voice_find(const char *guild, const char *user)
{
    for (int i = 0; i < g_ui.nvoices; i++)
        if (lstrcmpA(g_ui.voices[i].user, user) == 0 && lstrcmpA(g_ui.voices[i].guild, guild) == 0)
            return &g_ui.voices[i];
    return NULL;
}

/* A member object's display name and avatar, for the voice list. */
static void voice_name(voice_t *v, json_t member)
{
    json_t user, x;

    if (!json_get(member, "user", &user))
        return;
    sb_clear(&v->name);
    if (!(json_get(member, "nick", &x) && json_type(x) == JSON_STRING && json_str(x, &v->name)) &&
        !(json_get(user, "global_name", &x) && json_type(x) == JSON_STRING && json_str(x, &v->name)) &&
        json_get(user, "username", &x))
        json_str(x, &v->name);
    if (json_get(user, "avatar", &x) && json_type(x) == JSON_STRING)
        json_raw(x, v->avatar, sizeof v->avatar);
}

static int is_true_json(json_t obj, const char *key)
{
    json_t v;

    return json_get(obj, key, &v) && json_type(v) == JSON_TRUE;
}

/* One voice state: in a channel, or gone (channel_id null). */
static void voice_store(const char *guild, json_t s)
{
    json_t v, member;
    char user[24] = "", channel[24] = "";
    voice_t *e;

    if (json_get(s, "user_id", &v))
        json_raw(v, user, sizeof user);
    if (json_get(s, "channel_id", &v) && json_type(v) == JSON_STRING)
        json_raw(v, channel, sizeof channel);
    if (!user[0])
        return;
    if (channel[0])
        for (int i = g_ui.nvoices; i-- > 0;)
            if (lstrcmpA(g_ui.voices[i].user, user) == 0 && lstrcmpA(g_ui.voices[i].guild, guild) != 0) {
                sb_free(&g_ui.voices[i].name);
                g_ui.voices[i] = g_ui.voices[--g_ui.nvoices];
            }
    e = voice_find(guild, user);
    if (!channel[0]) {
        if (e) {
            sb_free(&e->name);
            *e = g_ui.voices[--g_ui.nvoices];
        }
        return;
    }
    if (!e) {
        if (g_ui.nvoices == g_ui.cap_voices) {
            g_ui.cap_voices = g_ui.cap_voices ? g_ui.cap_voices * 2 : 16;
            g_ui.voices = mem_realloc(g_ui.voices, (size_t)g_ui.cap_voices * sizeof *g_ui.voices);
        }
        e = &g_ui.voices[g_ui.nvoices++];
        *e = (voice_t){0};
        lstrcpynA(e->guild, guild, sizeof e->guild);
        lstrcpynA(e->user, user, sizeof e->user);
    }
    lstrcpynA(e->channel, channel, sizeof e->channel);
    e->flags = (is_true_json(s, "self_mute") || is_true_json(s, "mute") ? VOICE_MUTE : 0) |
               (is_true_json(s, "self_deaf") || is_true_json(s, "deaf") ? VOICE_DEAF : 0) |
               (is_true_json(s, "self_stream") ? VOICE_STREAM : 0) | (is_true_json(s, "self_video") ? VOICE_VIDEO : 0);
    if (json_get(s, "member", &member))
        voice_name(e, member);
}

static void voices_clear(void)
{
    for (int i = 0; i < g_ui.nvoices; i++)
        sb_free(&g_ui.voices[i].name);
    g_ui.nvoices = 0;
}

static int voice_count(const char *channel)
{
    int n = 0;

    for (int i = 0; i < g_ui.nvoices; i++)
        n += lstrcmpA(g_ui.voices[i].channel, channel) == 0;
    return n;
}

/* READY gives no names: ask the gateway for the open server's voice members once. */
static void voice_request_names(void)
{
    const char *guild = open_guild_id(), *ids[100];
    int n = 0;

    if (!guild)
        return;
    for (int i = 0; i < g_ui.nvoices && n < 100; i++) {
        voice_t *v = &g_ui.voices[i];
        if (v->name.len || v->asked || lstrcmpA(v->guild, guild) != 0)
            continue;
        v->asked = 1;
        ids[n++] = v->user;
    }
    app_request_members(guild, ids, n);
}

/* ---- Calls in direct messages ---- */

#define CALL_H 200
#define CALL_STREAM_H 420 /* the watched stream above the call's tiles */
#define CALL_RED 0xFFE5484Du

static void clamp_scroll(void);
static void redraw(void);
static int main_right(void);
static void go_to_channel(int i);

static int call_find(const char *channel)
{
    for (int i = 0; i < g_ui.ncalls; i++)
        if (lstrcmpA(g_ui.calls[i].channel, channel) == 0)
            return i;
    return -1;
}

static int in_call(const char *channel)
{
    return g_ui.voice_state != VOICE_OFF && lstrcmpA(g_ui.voice_channel, channel) == 0;
}

/* The ringtone plays while a call rings us and we are not in it. */
static void update_ringing(void)
{
    int ring = 0;

    for (int i = 0; i < g_ui.ncalls; i++)
        ring |= g_ui.calls[i].ringing && !in_call(g_ui.calls[i].channel);
    if (ring != g_ui.ring_sound) {
        g_ui.ring_sound = ring;
        if (ring) {
            FLASHWINFO fw = {sizeof fw, g_ui.top, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
            PlaySoundW(L"Notification.Looping.Call", NULL, SND_ALIAS | SND_ASYNC | SND_LOOP | SND_NODEFAULT);
            FlashWindowEx(&fw);
        } else {
            FLASHWINFO fw = {sizeof fw, g_ui.top, FLASHW_STOP, 0, 0};
            PlaySoundW(NULL, NULL, 0);
            FlashWindowEx(&fw);
        }
    }
}

/* CALL_CREATE and CALL_UPDATE: who is rung, and (on create) who is in it. */
static void call_store(json_t d, int create)
{
    json_t v, list, item;
    json_iter_t it;
    char channel[24] = "", me[24];
    int i;

    if (json_get(d, "channel_id", &v))
        json_raw(v, channel, sizeof channel);
    if (!channel[0] || !g_ui.model)
        return;
    if ((i = call_find(channel)) < 0) {
        if (g_ui.ncalls == (int)ARRAYSIZE(g_ui.calls))
            return;
        i = g_ui.ncalls++;
        lstrcpynA(g_ui.calls[i].channel, channel, sizeof g_ui.calls[i].channel);
    }
    g_ui.calls[i].ringing = 0;
    lstrcpynA(me, g_ui.model->user_id, sizeof me);
    if (json_get(d, "ringing", &list)) {
        json_iter(list, &it);
        while (json_next(&it, NULL, &item)) {
            char id[24] = "";
            json_raw(item, id, sizeof id);
            g_ui.calls[i].ringing |= lstrcmpA(id, me) == 0;
        }
    }
    if (create && json_get(d, "voice_states", &list)) {
        json_iter(list, &it);
        while (json_next(&it, NULL, &item))
            voice_store("", item);
    }
    update_ringing();
}

static void call_delete(const char *channel)
{
    int i = call_find(channel);

    if (i >= 0)
        g_ui.calls[i] = g_ui.calls[--g_ui.ncalls];
    for (int k = g_ui.nvoices; k-- > 0;)
        if (!g_ui.voices[k].guild[0] && lstrcmpA(g_ui.voices[k].channel, channel) == 0) {
            sb_free(&g_ui.voices[k].name);
            g_ui.voices[k] = g_ui.voices[--g_ui.nvoices];
        }
    update_ringing();
}

/* Joins a voice channel (guild_id empty for a call in a direct message). */
static void voice_join(const char *guild_id, const char *channel_id, const char *name)
{
    lstrcpynA(g_ui.voice_channel, channel_id, sizeof g_ui.voice_channel);
    lstrcpynA(g_ui.voice_name, name, sizeof g_ui.voice_name);
    g_ui.voice_state = VOICE_CONNECTING;
    sb_clear(&g_ui.voice_status);
    sb_add(&g_ui.voice_status, "Connecting\xE2\x80\xA6");
    app_voice_join(guild_id, channel_id);
    update_ringing();
    clamp_scroll();
    redraw();
}

static void share_pick_close(void);

static void voice_leave(void)
{
    if (g_ui.share_pick)
        share_pick_close();
    g_ui.voice_camera = 0;
    g_ui.share_state = g_ui.watch_state = VOICE_OFF;
    g_ui.watch_user[0] = 0;
    KillTimer(g_ui.wnd, TIMER_VOICE);
    app_voice_leave();
    g_ui.voice_state = VOICE_OFF;
    g_ui.voice_channel[0] = 0;
    update_ringing();
    clamp_scroll();
    redraw();
}

/* Starts the call of direct message `i`, or joins the one going on. */
static void call_start(int i)
{
    const channel_t *c = chan(i);
    int ongoing = call_find(c->id) >= 0;

    if (in_call(c->id))
        return;
    voice_join("", c->id, model_str(g_ui.model, c->name));
    if (!ongoing)
        app_call_ring(c->id, NULL);
}

/* Declines: stops the ringing, for us only. */
static void call_decline(const char *channel)
{
    int i = call_find(channel);

    if (i >= 0)
        g_ui.calls[i].ringing = 0;
    if (g_ui.model)
        app_call_ring(channel, g_ui.model->user_id);
    update_ringing();
    redraw();
}

/* A name and avatar for someone in a call: us, a friend, or the other side of the direct message. */
static const char *call_user(const char *user, const char *channel, const char **avatar)
{
    relation_t *r;
    int c;

    *avatar = "";
    if (g_ui.model && lstrcmpA(user, g_ui.model->user_id) == 0) {
        *avatar = g_ui.model->user_avatar;
        return g_ui.model->user_name ? model_str(g_ui.model, g_ui.model->user_name) : "";
    }
    if ((r = rel_find(user)) != NULL) {
        *avatar = r->avatar;
        return str_or_empty(&r->name);
    }
    c = g_ui.model ? model_find_channel(g_ui.model, channel) : -1;
    if (c >= 0 && lstrcmpA(chan(c)->user_id, user) == 0) {
        *avatar = chan(c)->avatar;
        return model_str(g_ui.model, chan(c)->name);
    }
    return "\xE2\x80\xA6";
}

/* The open direct message's call, above its messages: who is in it, and the buttons. */
/* The member of voice channel `channel` whose Go Live stream we watch, if any. */
static const voice_t *watched_in(const char *channel)
{
    if (!g_ui.watch_user[0] || !in_call(channel))
        return NULL;
    for (int i = 0; i < g_ui.nvoices; i++)
        if (lstrcmpA(g_ui.voices[i].channel, channel) == 0 && lstrcmpA(g_ui.voices[i].user, g_ui.watch_user) == 0 &&
            (g_ui.voices[i].flags & VOICE_STREAM))
            return &g_ui.voices[i];
    return NULL;
}

/* A direct message's call panel: taller while it shows a stream we watch. */
static int call_h(void)
{
    const channel_t *c;

    if (!g_ui.model || g_ui.channel < 0 || !is_dm_type((c = chan(g_ui.channel))->type) ||
        (call_find(c->id) < 0 && !in_call(c->id)))
        return 0;
    return S(CALL_H) + (watched_in(c->id) ? S(CALL_STREAM_H) : 0);
}

/* ---- Video tiles ---- */

/*
 * A new video picture: only the tiles change, so only their area is painted
 * again (the renderer skips the bands outside it). The voice grid fills the
 * main column under the header, a direct message's call panel sits at its top.
 */
static void invalidate_video(void)
{
    RECT r;
    const channel_t *c;

    if (g_ui.view != VIEW_APP || g_ui.settings_open || !g_ui.model || g_ui.channel < 0)
        return;
    c = chan(g_ui.channel);
    GetClientRect(g_ui.wnd, &r);
    r.left = S(RAIL_W + SIDE_W);
    r.right = main_right();
    r.top = S(HEADER_H);
    if (!(is_voice_type(c->type) && in_call(c->id))) {
        if (!call_h())
            return;
        r.bottom = r.top + call_h();
    }
    InvalidateRect(g_ui.wnd, &r, FALSE);
}

static void copy_picture(void *ctx, const unsigned *bgra, int w, int h)
{
    r_image_t **img = ctx;
    int iw, ih;

    r_image_size(*img, &iw, &ih);
    if (iw != w || ih != h) {
        r_image_free(*img);
        *img = r_image_blank(w, h);
    }
    if (*img)
        memcpy(r_image_bits(*img), bgra, (size_t)w * (size_t)h * 4);
}

/*
 * Someone's current video picture (with `stream`, their Go Live stream's),
 * or NULL when they show none; tile_w x tile_h is where it goes.
 */
static r_image_t *video_picture(const char *user, int stream, int tile_w, int tile_h)
{
    int i, free_slot = -1;

    for (i = 0; i < (int)ARRAYSIZE(g_ui.video); i++) {
        if (g_ui.video[i].stream == stream && lstrcmpA(g_ui.video[i].user, user) == 0)
            break;
        if (!g_ui.video[i].user[0] && free_slot < 0)
            free_slot = i;
    }
    if (i == (int)ARRAYSIZE(g_ui.video)) {
        if (free_slot < 0)
            return NULL;
        i = free_slot;
        lstrcpynA(g_ui.video[i].user, user, sizeof g_ui.video[i].user);
        g_ui.video[i].stream = stream;
        g_ui.video[i].serial = 0;
    }
    if (!app_video_take(user, stream, tile_w, tile_h, &g_ui.video[i].serial, copy_picture, &g_ui.video[i].img)) {
        r_image_free(g_ui.video[i].img);
        g_ui.video[i].img = NULL;
        g_ui.video[i].user[0] = 0;
        return NULL;
    }
    return g_ui.video[i].img;
}

/* The red LIVE of someone streaming. */
static void live_badge(int x, int y)
{
    r_round(x, y, S(34), S(16), S(8), CALL_RED);
    text(g_ui.f_cat, C_INK, rect(x, y, S(34), S(16)), "LIVE", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* A participant: their video letterboxed in the tile, or their avatar; name, mute state and speaking ring. */
static void paint_tile(const voice_t *v, const char *name, const char *avatar, int x, int y, int w, int h, int speaking)
{
    r_image_t *pic = video_picture(v->user, 0, w, h);
    int d = (w < h ? w : h) / 2;

    r_round(x, y, w, h, S(8), 0xFF121214);
    if (pic) {
        int pw, ph, fw, fh;
        r_image_size(pic, &pw, &ph);
        picture_fit(pw, ph, w, h, &fw, &fh);
        r_image(pic, x + (w - fw) / 2, y + (h - fh) / 2, fw, fh, S(8));
    } else {
        r_image_t *img = avatar && avatar[0] ? user_avatar(v->user, avatar) : NULL;
        if (img)
            r_image(img, x + (w - d) / 2, y + (h - d) / 2, d, d, d / 2);
        else
            r_circle(x + (w - d) / 2, y + (h - d) / 2, d, ARGB(C_ITEM));
    }
    {
        int tw = text_width(g_ui.f_small, name) + S(16), iy = y + h - S(28);
        if (tw > w - S(16))
            tw = w - S(16);
        r_round(x + S(8), iy, tw + (v->flags & (VOICE_MUTE | VOICE_DEAF) ? S(20) : 0), S(20), S(4), 0xB0000000u);
        text(g_ui.f_small, C_INK, rect(x + S(16), iy, tw - S(16), S(20)), name, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (v->flags & (VOICE_MUTE | VOICE_DEAF))
            text_w(g_ui.f_icon, C_MUTED, rect(x + S(8) + tw, iy, S(18), S(20)), v->flags & VOICE_DEAF ? L"\xE74F" : L"\xEC54", -1,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (v->flags & VOICE_STREAM)
        live_badge(x + S(8), y + S(8));
    if (speaking)
        r_round_outline(x, y, w, h, S(8), S(2), ARGB(C_GREEN));
}

/*
 * Someone's Go Live stream in a tile: ours shows what we send, the watched
 * one its picture (big, with a button to stop watching), the others a
 * button to watch them, which clicks anywhere on the tile press.
 */
static void paint_stream_tile(const voice_t *v, const char *name, int x, int y, int w, int h)
{
    int mine = g_ui.model && lstrcmpA(v->user, g_ui.model->user_id) == 0;
    int watched = !mine && lstrcmpA(g_ui.watch_user, v->user) == 0;
    r_image_t *pic = mine || watched ? video_picture(v->user, 1, w, h) : NULL;
    char label[140];

    r_round(x, y, w, h, S(8), 0xFF121214);
    if (pic) {
        int pw, ph, fw, fh;
        r_image_size(pic, &pw, &ph);
        picture_fit(pw, ph, w, h, &fw, &fh);
        r_image(pic, x + (w - fw) / 2, y + (h - fh) / 2, fw, fh, S(8));
    } else if (watched) {
        text(g_ui.f_body, C_MUTED, rect(x, y, w, h),
             g_ui.watch_state == VOICE_CONNECTED ? "Waiting for the stream\xE2\x80\xA6" : "Joining the stream\xE2\x80\xA6",
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else if (!mine) {
        int bw = S(140), bh = S(36), bx = x + (w - bw) / 2, by = y + (h - bh) / 2;
        r_round(bx, by, bw, bh, S(18), 0xFF4E5058u);
        text(g_ui.f_h, C_INK, rect(bx, by, bw, bh), "Watch Stream", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (g_ui.nlive_hits < (int)ARRAYSIZE(g_ui.live_hits)) {
            g_ui.live_hits[g_ui.nlive_hits].r = rect(x, y, w, h);
            lstrcpynA(g_ui.live_hits[g_ui.nlive_hits].user, v->user, sizeof g_ui.live_hits[0].user);
            g_ui.nlive_hits++;
        }
    }
    live_badge(x + S(8), y + S(8));
    if (mine)
        lstrcpynA(label, "Your screen", sizeof label);
    else
        wsprintfA(label, "%.100s\xE2\x80\x99s screen", name);
    {
        int tw = text_width(g_ui.f_small, label) + S(16), iy = y + h - S(28);
        if (tw > w - S(16))
            tw = w - S(16);
        r_round(x + S(8), iy, tw, S(20), S(4), 0xB0000000u);
        text(g_ui.f_small, C_INK, rect(x + S(16), iy, tw - S(16), S(20)), label,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (watched) {
        int cx = x + w - S(40), cy = y + S(8);
        r_round(cx, cy, S(32), S(32), S(16), 0xB0000000u);
        text_w(g_ui.f_icon, C_INK, rect(cx, cy, S(32), S(32)), L"\xE711", -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        g_ui.stream_close = rect(cx, cy, S(32), S(32));
    }
}

static RECT call_button(int x, int y, int w, const char *label, unsigned color)
{
    r_round(x, y, w, S(40), S(20), color);
    text(g_ui.f_h, C_INK, rect(x, y, w, S(40)), label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return rect(x, y, w, S(40));
}

static void paint_call(int x0, int w)
{
    const channel_t *c;
    const voice_t *watched;
    int h = call_h(), n = 0, k, x, y = S(HEADER_H), ty, ring, streams;

    SetRectEmpty(&g_ui.call_join);
    SetRectEmpty(&g_ui.call_decline);
    SetRectEmpty(&g_ui.call_leave);
    if (!h)
        return;
    c = chan(g_ui.channel);
    k = call_find(c->id);
    ring = k >= 0 && g_ui.calls[k].ringing && !in_call(c->id);
    watched = watched_in(c->id);
    /* Streams show once we are in the call: the one we watch above everyone, the others as tiles. */
    streams = in_call(c->id);
    fill(x0, y, w, h, C_RAIL);
    if (watched) {
        const char *avatar;
        paint_stream_tile(watched, call_user(watched->user, c->id, &avatar), x0 + S(16), y + S(16), w - S(32),
                          S(CALL_STREAM_H) - S(16));
        ty = y + S(CALL_STREAM_H);
    } else {
        ty = y;
    }
    for (int i = 0; i < g_ui.nvoices; i++) {
        const voice_t *v = &g_ui.voices[i];
        if (!v->guild[0] && lstrcmpA(v->channel, c->id) == 0)
            n += 1 + (streams && (v->flags & VOICE_STREAM) && v != watched);
    }
    x = x0 + (w - (n ? n * S(196) - S(12) : 0)) / 2;
    for (int i = 0; i < g_ui.nvoices; i++) {
        voice_t *v = &g_ui.voices[i];
        const char *avatar, *name;
        if (v->guild[0] || lstrcmpA(v->channel, c->id) != 0)
            continue;
        name = call_user(v->user, c->id, &avatar);
        paint_tile(v, name, avatar, x, ty + S(16), S(184), S(112),
                   in_call(c->id) && g_ui.voice_state == VOICE_CONNECTED && app_voice_speaking(v->user));
        x += S(196);
        if (streams && (v->flags & VOICE_STREAM) && v != watched) {
            paint_stream_tile(v, name, x, ty + S(16), S(184), S(112));
            x += S(196);
        }
    }
    if (!n)
        text(g_ui.f_body, C_MUTED, rect(x0, ty + S(40), w, S(60)), ring ? "Incoming call" : "Calling\xE2\x80\xA6",
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    y += h - S(56);
    if (in_call(c->id)) {
        g_ui.call_leave = call_button(x0 + (w - S(140)) / 2, y, S(140), "Leave", CALL_RED);
    } else if (ring) {
        g_ui.call_join = call_button(x0 + w / 2 - S(148), y, S(140), "Join Call", ARGB(C_GREEN));
        g_ui.call_decline = call_button(x0 + w / 2 + S(8), y, S(140), "Decline", CALL_RED);
    } else {
        g_ui.call_join = call_button(x0 + (w - S(140)) / 2, y, S(140), "Join Call", ARGB(C_GREEN));
    }
}

/* A call ringing us in another conversation: a card at the top of the window. */
static void paint_call_card(void)
{
    int x, y = S(HEADER_H) + S(12), w = S(360), h = S(120);
    const char *name;

    SetRectEmpty(&g_ui.card_join);
    SetRectEmpty(&g_ui.card_decline);
    g_ui.card_channel[0] = 0;
    for (int i = 0; i < g_ui.ncalls; i++) {
        int c = g_ui.model ? model_find_channel(g_ui.model, g_ui.calls[i].channel) : -1;
        if (!g_ui.calls[i].ringing || in_call(g_ui.calls[i].channel) || c < 0 || c == g_ui.channel)
            continue;
        lstrcpynA(g_ui.card_channel, g_ui.calls[i].channel, sizeof g_ui.card_channel);
        name = model_str(g_ui.model, chan(c)->name);
        x = S(RAIL_W + SIDE_W) + (main_right() - S(RAIL_W + SIDE_W) - w) / 2;
        r_round(x, y, w, h, S(8), 0xFF1E1E20);
        r_round_outline(x, y, w, h, S(8), 1, 0xFF2E2E31);
        text(g_ui.f_h, C_INK, rect(x + S(16), y + S(14), w - S(32), S(22)), name, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        text(g_ui.f_small, C_MUTED, rect(x + S(16), y + S(38), w - S(32), S(20)), "Incoming call", DT_LEFT | DT_SINGLELINE);
        g_ui.card_join = call_button(x + S(16), y + h - S(54), (w - S(44)) / 2, "Join Call", ARGB(C_GREEN));
        g_ui.card_decline = call_button(x + w / 2 + S(6), y + h - S(54), (w - S(44)) / 2, "Decline", CALL_RED);
        return;
    }
}

/* Asks for our stream state again (after UI_STREAM): a connection that failed is closed. */
static void stream_refresh(void)
{
    g_ui.share_state = app_stream_status(g_ui.watch_user, sizeof g_ui.watch_user, &g_ui.watch_state);
    if (g_ui.share_state == VOICE_FAILED) {
        app_screen_share(0, -1, 0);
        g_ui.share_state = VOICE_OFF;
    }
    if (g_ui.watch_state == VOICE_FAILED) {
        app_stream_watch(NULL);
        g_ui.watch_user[0] = 0;
        g_ui.watch_state = VOICE_OFF;
    }
    clamp_scroll();
    redraw();
}

/* Clicks on the call panel or card, or on a stream's tile; returns whether one was used. */
static int click_call(int x, int y)
{
    POINT pt = {x, y};

    if (PtInRect(&g_ui.stream_close, pt)) {
        app_stream_watch(NULL);
        stream_refresh();
        return 1;
    }
    for (int i = 0; i < g_ui.nlive_hits; i++)
        if (PtInRect(&g_ui.live_hits[i].r, pt)) {
            char user[24];
            lstrcpynA(user, g_ui.live_hits[i].user, sizeof user);
            app_stream_watch(user);
            stream_refresh();
            return 1;
        }

    if (PtInRect(&g_ui.card_join, pt) || PtInRect(&g_ui.card_decline, pt)) {
        char channel[24];
        int c;
        lstrcpynA(channel, g_ui.card_channel, sizeof channel);
        c = g_ui.model ? model_find_channel(g_ui.model, channel) : -1;
        if (PtInRect(&g_ui.card_decline, pt)) {
            call_decline(channel);
        } else if (c >= 0) {
            go_to_channel(c);
            call_start(c);
        }
        return 1;
    }
    if (g_ui.channel < 0)
        return 0;
    if (PtInRect(&g_ui.call_join, pt)) {
        call_start(g_ui.channel);
        return 1;
    }
    if (PtInRect(&g_ui.call_decline, pt)) {
        call_decline(chan(g_ui.channel)->id);
        return 1;
    }
    if (PtInRect(&g_ui.call_leave, pt)) {
        voice_leave();
        return 1;
    }
    return 0;
}

/* The people in voice channel i, under its row. */
static void paint_voice_users(unsigned i, int y)
{
    const char *channel = chan((int)i)->id;
    int x = S(RAIL_W) + S(8) + S(36), right = S(RAIL_W) + S(SIDE_W) - S(16);

    for (int k = 0; k < g_ui.nvoices; k++) {
        voice_t *v = &g_ui.voices[k];
        r_image_t *img;
        int ix = right;
        if (lstrcmpA(v->channel, channel) != 0)
            continue;
        img = user_avatar(v->user, v->avatar);
        if (img)
            r_image(img, x, y + S(4), S(22), S(22), S(11));
        else
            r_circle(x, y + S(4), S(22), ARGB(C_ITEM));
        /* Speaking: a green ring, for the people in our own call. */
        if (g_ui.voice_state == VOICE_CONNECTED && lstrcmpA(channel, g_ui.voice_channel) == 0 &&
            app_voice_speaking(v->user))
            r_round_outline(x - S(2), y + S(2), S(26), S(26), S(13), S(2), ARGB(C_GREEN));
        if (v->flags & VOICE_DEAF) {
            ix -= S(18);
            text_w(g_ui.f_icon, C_MUTED, rect(ix, y, S(18), S(VOICE_ROW)), L"\xE74F", -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (v->flags & VOICE_MUTE) {
            ix -= S(18);
            text_w(g_ui.f_icon, C_MUTED, rect(ix, y, S(18), S(VOICE_ROW)), L"\xEC54", -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (v->flags & VOICE_VIDEO) {
            ix -= S(20);
            text_w(g_ui.f_icon, C_MUTED, rect(ix, y, S(20), S(VOICE_ROW)), L"\xE714", -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (v->flags & VOICE_STREAM) {
            ix -= S(38);
            r_round(ix, y + S(7), S(34), S(16), S(8), 0xFFE5484Du);
            text(g_ui.f_cat, C_INK, rect(ix, y + S(7), S(34), S(16)), "LIVE", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        text(g_ui.f_body, C_MUTED, rect(x + S(30), y, ix - x - S(34), S(VOICE_ROW)), v->name.len ? v->name.data : "\xE2\x80\xA6",
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += S(VOICE_ROW);
    }
}

static int row_height(unsigned i)
{
    int type = chan((int)i)->type;
    if (is_voice_type(type))
        return S(ROW_H) + voice_count(chan((int)i)->id) * S(VOICE_ROW);
    return type == CH_CATEGORY ? S(CAT_H) : is_dm_type(type) ? S(DM_ROW_H) : S(ROW_H);
}

/* Top of the channel list under the header: home puts its links above the conversations. */
static int side_list_top(void)
{
    return S(8) + (g_ui.guild < 0 ? S(DM_TOP) : 0);
}

static int side_content(void)
{
    unsigned first, count;
    int h = side_list_top();

    if (!side_range(&first, &count))
        return 0;
    for (unsigned i = first; i < first + count; i++)
        if (!empty_category(first, count, i) && (chan((int)i)->type == CH_CATEGORY || !hidden(first, i)))
            h += row_height(i);
    return h + S(8);
}

static int voice_bar_h(void)
{
    return g_ui.voice_state != VOICE_OFF ? S(VOICE_BAR_H) : 0;
}

/* The voice bar's row of buttons, left to right, each this wide. */
static const int k_voice_buttons[4] = {HIT_VOICE_SHARE, HIT_VOICE_CAMERA, HIT_VOICE_MUTE, HIT_VOICE_DEAF};

static int panel_right(void);

static int voice_button_w(void)
{
    return (panel_right() - S(PANEL_GAP) - S(16) - 3 * S(8)) / 4;
}

/*
 * The floating panel at the bottom left, over the rail and the channel list,
 * as in Discord: the voice connection (while in a call) above our name.
 */
static int panel_top(RECT rc)
{
    return rc.bottom - S(PANEL_GAP) - S(PANEL_H) - voice_bar_h();
}

static int user_row_y(RECT rc)
{
    return rc.bottom - S(PANEL_GAP) - S(PANEL_H);
}

static int panel_right(void)
{
    return S(RAIL_W + SIDE_W) - S(PANEL_GAP);
}

/* The user row's buttons, right to left: settings, deafen's menu, deafen, mute's menu, mute. */
static const int k_panel_buttons[5] = {HIT_LOGOUT, HIT_VOICE_MENU, HIT_DEAFEN, HIT_VOICE_MENU, HIT_MIC};
static const int k_panel_button_w[5] = {32, 18, 32, 18, 32};

static int panel_button_x(int k)
{
    int x = panel_right() - S(8);

    for (int i = 0; i <= k; i++)
        x -= S(k_panel_button_w[i]) + (i == 2 ? S(6) : 0);
    return x;
}

static int side_view(RECT rc)
{
    return panel_top(rc) - S(HEADER_H);
}

static void clamp_scroll(void)
{
    RECT rc;
    int max;

    GetClientRect(g_ui.wnd, &rc);
    max = rail_content() - (panel_top(rc) - S(PANEL_GAP));
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
    if (x >= S(PANEL_GAP) && x < panel_right() && y >= panel_top(rc)) {
        int vy = panel_top(rc), uy = user_row_y(rc), cy = uy + (S(PANEL_H) - S(32)) / 2;
        if (voice_bar_h() && y < uy) {
            int by = vy + S(VOICE_ROW1_H), bw = voice_button_w();
            int bx = x - S(PANEL_GAP) - S(8), k = bx >= 0 ? bx / (bw + S(8)) : 4;
            if (y >= vy + (S(VOICE_ROW1_H) - S(32)) / 2 && y < vy + (S(VOICE_ROW1_H) + S(32)) / 2 &&
                x >= panel_right() - S(40) && x < panel_right() - S(8))
                *kind = HIT_VOICE_LEAVE;
            else if (y >= by && y < by + S(32) && k < 4 && bx % (bw + S(8)) < bw)
                *kind = k_voice_buttons[k];
            return;
        }
        if (y >= cy && y < cy + S(32)) {
            for (int k = 0; k < 5; k++)
                if (x >= panel_button_x(k) && x < panel_button_x(k) + S(k_panel_button_w[k])) {
                    *kind = k_panel_buttons[k];
                    return;
                }
            if (g_ui.disconnected && x >= panel_button_x(4) - S(36) && x < panel_button_x(4) - S(4))
                *kind = HIT_RETRY;
            else if (x < panel_button_x(4) - S(4))
                *kind = HIT_SELF;
        }
        return;
    }
    if (x < S(RAIL_W)) {
        if (y >= panel_top(rc) - S(PANEL_GAP))
            return;
        if (y >= rail_y(-1) && y < rail_y(-1) + S(ICON)) {
            *kind = HIT_HOME;
            return;
        }
        for (int k = 0, n = rail_rows(); k < n; k++)
            if (y >= g_rail.y[k] && y < g_rail.y[k] + S(ICON)) {
                *kind = g_rail.kind[k] == RAIL_GUILD ? HIT_GUILD : HIT_FOLDER;
                *index = g_rail.index[k];
                return;
            }
    } else if (x < S(RAIL_W + SIDE_W)) {
        unsigned first, count;
        if (y >= panel_top(rc))
            return;
        if (y < S(HEADER_H) && g_ui.guild < 0 && g_ui.model) {
            if (y >= S(9) && y < S(40) && x >= S(RAIL_W) + S(8) && x < S(RAIL_W + SIDE_W) - S(8))
                *kind = HIT_DM_SEARCH;
            return;
        }
        if (y < S(HEADER_H) && g_ui.guild >= 0 && g_ui.model) {
            if (y >= S(8) && y < S(40) && x >= S(RAIL_W) + S(8) && x < S(RAIL_W + SIDE_W) - S(8))
                *kind = HIT_GUILD_HEADER;
            return;
        }
        if (y >= S(HEADER_H) && g_ui.guild < 0 && g_ui.model) {
            int top = S(HEADER_H) + S(8) - g_ui.side_scroll, sec = top + S(NAVS * NAV_ROW) + S(13);
            if (y >= top && y < top + S(NAVS * NAV_ROW) && x >= S(RAIL_W) + S(8) && x < S(RAIL_W + SIDE_W) - S(8)) {
                *kind = HIT_NAV;
                *index = (y - top) / S(NAV_ROW);
                return;
            }
            if (y >= sec && y < sec + S(DM_SECTION) && x >= S(RAIL_W + SIDE_W) - S(40) && x < S(RAIL_W + SIDE_W) - S(8)) {
                *kind = HIT_NEW_DM;
                return;
            }
        }
        if (y >= S(HEADER_H) && side_range(&first, &count)) {
            int ry = S(HEADER_H) + side_list_top() - g_ui.side_scroll;
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

static long long now_ms(void)
{
    FILETIME ft;
    ULARGE_INTEGER t;

    GetSystemTimeAsFileTime(&ft);
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    return (long long)(t.QuadPart / 10000 - FILETIME_UNIX_MS);
}

static int channel_muted(unsigned i)
{
    return model_muted(g_ui.model, i, now_ms());
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
        if (!channel_muted(i) && model_unread(g_ui.model, i))
            *unread = 1;
    }
}

static void guild_state(int g, int *unread, int *mentions)
{
    const guild_t *gd = &g_ui.model->guilds[g];

    range_state(gd->first, gd->count, unread, mentions);
    if (model_guild_muted(g_ui.model, g, now_ms()))
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

    if (n && g_ui.pref_title)
        wsprintfW(title, L"(%d) Silicord", n);
    else
        lstrcpyW(title, L"Silicord");
    SetWindowTextW(g_ui.top, title);
    InvalidateRect(g_ui.top, NULL, FALSE);
}

/* Red count badge whose right edge is at `right`, vertically centered on `cy`. */
static void paint_badge(int right, int cy, int count)
{
    char label[12]; /* any int: counts come from the server */
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

/* The white pill left of a server: `height` is where it goes (`rest` without hover or selection), and it grows or shrinks there. */
static void paint_pill(int kind, int index, int y, int height, int rest)
{
    int h = (int)(tween(kind, index, (float)height, (float)rest, TW_SHAPE, 0, y, S(8), S(ICON)) + .5f);

    if (h > 0)
        r_round(-S(4), y + (S(ICON) - h) / 2, S(8), h, S(4) < h / 2 ? S(4) : h / 2, ARGB(C_INK));
}

/* Server icons are rounded squares, hovered or not, as in Discord's current look. */
static int icon_radius(float t)
{
    (void)t;
    return S(12);
}

static void paint_rail(RECT rc)
{
    int x = (S(RAIL_W) - S(ICON)) / 2, n = rail_rows();
    int home_y = rail_y(-1), sel_home = g_ui.guild < 0, hov_home = g_ui.hover_kind == HIT_HOME;

    fill(0, 0, S(RAIL_W), rc.bottom, C_RAIL);
    r_clip(0, 0, S(RAIL_W), panel_top(rc) - S(PANEL_GAP));

    /* Home: the Silicord mark on blurple, where Discord puts its own. */
    {
        float t = tween_on(TW_HOME, 0, sel_home || hov_home, TW_SHAPE, x, home_y, S(ICON), S(ICON));
        r_round(x, home_y, S(ICON), S(ICON), icon_radius(t), lerp_argb(ARGB(C_ITEM), ARGB(C_BRAND), t));
        int unit = S(24) / 16 > 0 ? S(24) / 16 : 1;
        draw_mark(x + (S(ICON) - 16 * unit) / 2, home_y + (S(ICON) - 16 * unit) / 2, unit, t >= .5f ? C_INK : C_MUTED);
        paint_pill(TW_HOME_PILL, 0, home_y, sel_home ? S(40) : hov_home ? S(20) : 0, 0);
    }
    if (g_ui.model) {
        int unread, mentions;
        range_state(g_ui.model->dm_first, g_ui.model->dm_count, &unread, &mentions);
        if (mentions) {
            r_circle(x + S(ICON) - S(20), home_y + S(ICON) - S(20), S(24), ARGB(C_RAIL));
            paint_badge(x + S(ICON) + S(2), home_y + S(ICON) - S(8), mentions);
        }
    }
    fill((S(RAIL_W) - S(32)) / 2, home_y + S(ICON) + S(8), S(32), S(1) > 1 ? S(1) : 1, C_LINE);

    /* Open folders: a tinted column behind the folder and its servers. */
    for (int k = 0; k < n; k++) {
        if (g_rail.kind[k] != RAIL_FOLDER || !folder_is_open(g_rail.index[k]))
            continue;
        {
            int end = k + 1;
            const folder_t *f = &g_ui.model->folders[g_rail.index[k]];
            unsigned c = f->has_color ? f->color : 0x5865F2;
            while (end < n && g_rail.kind[end] == RAIL_GUILD &&
                   g_ui.model->guilds[g_rail.index[end]].folder == g_rail.index[k])
                end++;
            r_round(x - S(4), g_rail.y[k] - S(4), S(ICON) + S(8), g_rail.y[end - 1] + S(ICON) + S(4) - (g_rail.y[k] - S(4)),
                    S(16), 0x33000000u | (c & 0xFFFFFF));
        }
    }
    for (int k = 0; k < n; k++) {
        int y = g_rail.y[k];
        if (g_rail.kind[k] != RAIL_FOLDER || y + S(ICON) < 0 || y > rc.bottom)
            continue;
        {
            int f = g_rail.index[k], open = folder_is_open(f);
            const folder_t *fd = &g_ui.model->folders[f];
            unsigned c = fd->has_color ? fd->color : 0x5865F2;
            int hov = g_ui.hover_kind == HIT_FOLDER && g_ui.hover_index == f, unread = 0, mentions = 0, sel = 0, j = 0;
            for (unsigned g = 0; g < g_ui.model->nguilds; g++)
                if (g_ui.model->guilds[g].folder == f) {
                    int u, mm;
                    guild_state((int)g, &u, &mm);
                    unread |= u;
                    mentions += mm;
                    sel |= g_ui.guild == (int)g;
                }
            if (open) {
                text_w(g_ui.f_icon_mid, C_INK, rect(x, y, S(ICON), S(ICON)), L"\xE8B7", -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                /* folder glyph tinted with its color */
                r_round(x + S(14), y + S(30), S(20), S(3), S(1), 0xFF000000u | c);
            } else {
                /* Closed: up to four of its icons in a grid, like Discord. */
                float t = tween_on(HIT_FOLDER, f, hov, TW_FAST, x, y, S(ICON), S(ICON));
                r_round(x, y, S(ICON), S(ICON), S(16), lerp_argb(0x40000000u | c, 0x66000000u | c, t));
                for (unsigned g = 0; g < g_ui.model->nguilds && j < 4; g++) {
                    if (g_ui.model->guilds[g].folder != f)
                        continue;
                    {
                        int gx = x + S(6) + (j % 2) * S(19), gy = y + S(6) + (j / 2) * S(19);
                        r_image_t *img = guild_icon(&g_ui.model->guilds[g]);
                        if (img)
                            r_image(img, gx, gy, S(17), S(17), S(9));
                        else
                            r_circle(gx, gy, S(17), ARGB(C_ITEM));
                    }
                    j++;
                }
                paint_pill(TW_FOLDER_PILL, f, y, sel ? S(40) : hov ? S(20) : unread || mentions ? S(8) : 0,
                           unread || mentions ? S(8) : 0);
                if (mentions) {
                    r_circle(x + S(ICON) - S(20), y + S(ICON) - S(20), S(24), ARGB(C_RAIL));
                    paint_badge(x + S(ICON) + S(2), y + S(ICON) - S(8), mentions);
                }
            }
        }
    }

    for (int k = 0; k < n; k++) {
        int i = g_rail.index[k], y = g_rail.y[k];
        int sel = g_ui.guild == i, hov = g_ui.hover_kind == HIT_GUILD && g_ui.hover_index == i, radius;
        float t;
        const guild_t *gd;
        r_image_t *img;

        if (g_rail.kind[k] != RAIL_GUILD || y + S(ICON) < 0 || y > rc.bottom)
            continue;
        t = tween_on(HIT_GUILD, i, sel || hov, TW_SHAPE, x, y, S(ICON), S(ICON));
        radius = icon_radius(t);
        gd = &g_ui.model->guilds[i];
        img = guild_icon(gd);
        if (img) {
            r_image(img, x, y, S(ICON), S(ICON), radius);
        } else {
            wchar_t ini[8];
            initials(model_str(g_ui.model, gd->name), ini, 8);
            r_round(x, y, S(ICON), S(ICON), radius, lerp_argb(ARGB(C_ITEM), ARGB(C_BRAND), t));
            text_w(lstrlenW(ini) > 2 ? g_ui.f_initial_small : g_ui.f_initial, t >= .5f ? C_RAIL : C_INK,
                   rect(x, y, S(ICON), S(ICON)), ini, -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        {
            int unread, mentions;
            guild_state(i, &unread, &mentions);
            paint_pill(TW_PILL, i, y, sel ? S(40) : hov ? S(20) : unread || mentions ? S(8) : 0, unread || mentions ? S(8) : 0);
            if (mentions) {
                r_circle(x + S(ICON) - S(20), y + S(ICON) - S(20), S(24), ARGB(C_RAIL));
                paint_badge(x + S(ICON) + S(2), y + S(ICON) - S(8), mentions);
            }
        }
    }
    r_unclip();
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
    r_round(x, y, S(4), h, S(2), 0xFF242426);
}

/*
 * Line icons of our own in Discord's style: a 24 unit grid, strokes 2 units
 * wide with round ends. Rendered once per size as masks, tinted when drawn.
 */
enum { SI_HASH, SI_CHEVRON_DOWN, SI_CHEVRON_RIGHT, SI_COUNT };
static const char *const k_sicon[SI_COUNT] = {
    "M10.5 3.5 8.5 20.5M16.5 3.5l-2 17M4.5 8.5h16M3.5 15.5h16",
    "M6 9.5l6 6 6-6",
    "M9.5 6l6 6-6 6",
};

static struct {
    int id, px;
    r_image_t *img;
} g_sicons[24];

static void sicon(int id, int x, int y, int px, unsigned argb)
{
    r_image_t *img = NULL;
    int k;

    for (k = 0; k < (int)ARRAYSIZE(g_sicons) && g_sicons[k].img; k++)
        if (g_sicons[k].id == id && g_sicons[k].px == px) {
            img = g_sicons[k].img;
            break;
        }
    if (!img) {
        char svg[512];
        int n = wsprintfA(svg,
                          "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\" viewBox=\"0 0 24 24\">"
                          "<path fill=\"none\" stroke=\"#fff\" stroke-width=\"2\" stroke-linecap=\"round\" "
                          "stroke-linejoin=\"round\" d=\"%s\"/></svg>",
                          k_sicon[id]);
        if (!(img = r_image_decode(svg, (size_t)n, px)))
            return;
        if (k == (int)ARRAYSIZE(g_sicons)) { /* full: the first goes */
            r_image_free(g_sicons[0].img);
            k = 0;
        }
        g_sicons[k].id = id;
        g_sicons[k].px = px;
        g_sicons[k].img = img;
    }
    r_image_tint(img, x, y, argb);
}

static void paint_channel_row(unsigned i, int y)
{
    const channel_t *c = chan((int)i);
    int x = S(RAIL_W) + S(8), w = S(SIDE_W) - S(16);
    int sel = g_ui.channel == (int)i, hov = g_ui.hover_kind == HIT_CHANNEL && g_ui.hover_index == (int)i;
    const char *name = model_str(g_ui.model, c->name);

    if (c->type == CH_CATEGORY) {
        /* the name in 14px medium, as Discord's refreshed type has it, and its chevron after it */
        int tw = text_width(g_ui.f_menu, name), max = w - S(8) - S(20);
        unsigned ink = hov ? ARGB(C_INK) : ARGB(C_CHANNEL);
        if (tw > max)
            tw = max;
        text(g_ui.f_menu, hov ? C_INK : C_CHANNEL, rect(x + S(8), y + S(16), tw + S(2), S(20)), name,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        sicon(g_ui.collapsed[i] ? SI_CHEVRON_RIGHT : SI_CHEVRON_DOWN, x + S(8) + tw + S(2), y + S(18), S(16), ink);
        return;
    }
    if (is_dm_type(c->type)) {
        r_image_t *img = dm_icon(c);
        unsigned bg = row_bg(HIT_CHANNEL, (int)i, sel, hov, ARGB(C_SIDE), x, y + S(1), w, S(DM_ROW_H) - S(2), S(8));
        (void)img;
        paint_dm_icon(c, x + S(8), y + S(6), S(32), bg);
        {
            int unread = channel_unread(i), badge = c->mentions ? S(30) : 0;
            presence_t *pr = c->type == CH_DM && c->user_id[0] ? presence_find(c->user_id) : NULL;
            if (c->type == CH_DM && c->user_id[0])
                paint_status(x + S(8), y + S(6), S(32), pr ? pr->status : ML_OFFLINE, bg);
            if ((pr && pr->activity.len && pr->status != ML_OFFLINE) || (c->type == CH_GROUP_DM && c->members)) {
                char members[32];
                wsprintfA(members, "%d Members", c->members);
                text(unread ? g_ui.f_h : g_ui.f_body, sel || hov || unread ? C_INK : C_MUTED,
                     rect(x + S(52), y + S(3), w - S(58) - badge, S(21)), name, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                text(g_ui.f_small, C_MUTED, rect(x + S(52), y + S(23), w - S(58) - badge, S(16)),
                     c->type == CH_GROUP_DM ? members : pr->activity.data, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            } else
            text(unread ? g_ui.f_h : g_ui.f_body, sel || hov || unread ? C_INK : C_MUTED,
                 rect(x + S(52), y, w - S(58) - badge, S(DM_ROW_H)), name,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (c->mentions)
                paint_badge(x + w - S(8), y + S(DM_ROW_H) / 2, c->mentions);
        }
        return;
    }
    if (model_is_thread(c->type)) {
        /* Threads hang under their channel with a curved line, like Discord. */
        int unread = channel_unread(i);
        fill(x + S(18), y - S(6), S(1) > 1 ? S(1) : 1, S(ROW_H) / 2 + S(6), C_LINE);
        fill(x + S(18), y + S(ROW_H) / 2, S(12), S(1) > 1 ? S(1) : 1, C_LINE);
        row_bg(HIT_CHANNEL, (int)i, sel, hov, ARGB(C_SIDE), x + S(34), y + S(1), w - S(34), S(ROW_H) - S(2), S(6));
        text(unread ? g_ui.f_h : g_ui.f_body, sel || hov || unread ? C_INK : C_MUTED, rect(x + S(42), y, w - S(48), S(ROW_H)),
             name, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }
    row_bg(HIT_CHANNEL, (int)i, sel, hov, ARGB(C_SIDE), x, y + S(1), w, S(ROW_H) - S(2), S(6));
    {
        int unread = channel_unread(i), muted = channel_muted(i), badge = c->mentions ? S(30) : 0;
        int color = sel || hov || unread ? C_INK : C_CHANNEL;
        /* muted channels fade to 40%, as in Discord */
        unsigned icon = sel || hov ? ARGB(C_INK) : muted ? ARGB(C_CHANNEL_MUTED) : ARGB(C_CHANNEL);
        if (c->type == CH_VOICE || c->type == CH_STAGE || c->type == CH_NEWS || c->type == CH_FORUM || c->type == CH_MEDIA)
            /* Speaker, megaphone for announcements, speech bubbles for forums, like Discord. */
            r_text(g_ui.f_icon_tb, icon, x + S(8), y, S(20), S(ROW_H),
                   c->type == CH_NEWS ? L"\xE789" : c->type == CH_FORUM || c->type == CH_MEDIA ? L"\xE8F2" : ICON_VOLUME, -1,
                   R_CENTER | R_VCENTER | R_SINGLE);
        else
            sicon(SI_HASH, x + S(8), y + (S(ROW_H) - S(20)) / 2, S(20), icon);
        if (unread && !sel)
            r_round(S(RAIL_W) - S(4), y + S(ROW_H) / 2 - S(4), S(8), S(8), S(4), ARGB(C_INK));
        text(unread ? g_ui.f_nav : g_ui.f_chan, muted && !sel && !hov ? C_CHANNEL_MUTED : color,
             rect(x + S(34), y, w - S(40) - badge, S(ROW_H)), name, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (c->mentions)
            paint_badge(x + w - S(8), y + S(ROW_H) / 2, c->mentions);
    }
}

/* A small icon button: hover eases in a background, `on` shows it red (muted, deafened). */
static void panel_button(int kind, int x, int y, int w, const wchar_t *glyph, r_font_t *font, int on)
{
    float t = tween_on(kind + 2000, x, g_ui.hover_kind == kind, TW_FAST, x, y, w, S(32));

    if (t > 0.f)
        r_round(x, y, w, S(32), S(8), lerp_argb(ARGB(C_PANEL), 0xFF1E1E20u, t));
    r_text(font, on ? ARGB(C_RED) : lerp_argb(ARGB(C_MUTED), ARGB(C_INK), t), x, y, w, S(32), glyph, -1,
           R_CENTER | R_VCENTER | R_SINGLE);
}

/* Our name and status, with mute, deafen and settings, as in Discord's user panel. */
static void paint_user_panel(RECT rc)
{
    int x0 = S(PANEL_GAP), y = user_row_y(rc), cy = y + (S(PANEL_H) - S(32)) / 2, ax = x0 + S(12);
    const char *name = g_ui.model && g_ui.model->user_name ? model_str(g_ui.model, g_ui.model->user_name)
                                                          : str_or_empty(&g_ui.account);
    int dot = g_ui.disconnected ? C_FAINT : g_ui.model && !g_ui.reconnecting ? C_GREEN : C_WARN;
    int tw = panel_button_x(4) - S(4) - (ax + S(40)) - (g_ui.disconnected ? S(36) : 0);
    int muted = g_ui.voice_muted || g_ui.voice_deafened;
    float hs = tween_on(HIT_SELF + 2000, 0, g_ui.hover_kind == HIT_SELF, TW_FAST, x0, y, S(PANEL_H), S(PANEL_H));

    /* Hovering our name lights it like a button. */
    if (hs > 0.f)
        r_round(ax - S(4), cy - S(4), tw + S(48), S(40), S(8), lerp_argb(ARGB(C_PANEL), 0xFF1E1E20u, hs));
    if (g_ui.model && g_ui.model->user_id[0]) {
        r_image_t *img = user_avatar(g_ui.model->user_id, g_ui.model->user_avatar);
        if (img)
            r_image(img, ax, cy, S(32), S(32), S(16));
        else
            r_circle(ax, cy, S(32), ARGB(C_ITEM));
    } else {
        r_circle(ax, cy, S(32), ARGB(C_ITEM));
    }
    if (g_ui.model && !g_ui.disconnected && !g_ui.reconnecting) {
        paint_status(ax, cy, S(32), g_ui.my_status, hs > 0.f ? lerp_argb(ARGB(C_PANEL), 0xFF1E1E20u, hs) : ARGB(C_PANEL));
    } else {
        r_circle(ax + S(21), cy + S(21), S(14), ARGB(C_PANEL));
        r_circle(ax + S(23), cy + S(23), S(10), ARGB(dot));
    }
    text(g_ui.f_h, C_INK, rect(ax + S(40), cy - S(2), tw, S(20)), name, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    text(g_ui.f_small, C_MUTED, rect(ax + S(40), cy + S(17), tw, S(18)),
         !g_ui.model || g_ui.disconnected || g_ui.reconnecting ? str_or_empty(&g_ui.status) /* connection trouble first */
         : g_ui.model->custom_status                         ? model_str(g_ui.model, g_ui.model->custom_status)
                                                             : status_name(),
         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    /* Mute and deafen with their menus (the voice settings), then settings. */
    panel_button(HIT_MIC, panel_button_x(4), cy, S(32), muted ? L"\xEC54" : L"\xE720", g_ui.f_icon_mid, muted);
    panel_button(HIT_VOICE_MENU, panel_button_x(3), cy, S(18), ICON_CHEVRON_DOWN, g_ui.f_caption, 0);
    panel_button(HIT_DEAFEN, panel_button_x(2), cy, S(32), g_ui.voice_deafened ? L"\xE74F" : L"\xE7F6", g_ui.f_icon_mid,
                 g_ui.voice_deafened);
    panel_button(HIT_VOICE_MENU + 100, panel_button_x(1), cy, S(18), ICON_CHEVRON_DOWN, g_ui.f_caption, 0);
    panel_button(HIT_LOGOUT, panel_button_x(0), cy, S(32), g_ui.model ? L"\xE713" : ICON_POWER, g_ui.f_icon_mid, 0);
    if (g_ui.disconnected) {
        int rx = panel_button_x(4) - S(36);
        if (g_ui.hover_kind == HIT_RETRY)
            r_round(rx, cy, S(32), S(32), S(8), 0xFF1E1E20u);
        text_w(g_ui.f_icon, C_WARN, rect(rx, cy, S(32), S(32)), ICON_REFRESH, -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

/* Our voice connection, at the top of the floating panel: its state, the channel, a button to leave, then our media. */
static void paint_voice_bar(RECT rc)
{
    int h = voice_bar_h(), x0 = S(PANEL_GAP), right = panel_right() - S(8), y, cy, by, bw = voice_button_w();
    int color = g_ui.voice_state == VOICE_CONNECTED ? C_GREEN : g_ui.voice_state == VOICE_FAILED ? C_FAINT : C_WARN;
    char code[40], line[160];

    if (!h)
        return;
    y = panel_top(rc);
    cy = y + (S(VOICE_ROW1_H) - S(32)) / 2;
    by = y + S(VOICE_ROW1_H);
    text(g_ui.f_h, color, rect(x0 + S(12), cy - S(2), right - x0 - S(52), S(20)),
         g_ui.voice_state == VOICE_CONNECTED ? "Voice Connected" : str_or_empty(&g_ui.voice_status),
         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    /* The channel, and the end-to-end encryption code others can compare. */
    lstrcpynA(line, g_ui.voice_name, 100);
    if (g_ui.voice_state == VOICE_CONNECTED && voice_privacy_code(code, sizeof code)) {
        lstrcatA(line, " \xC2\xB7 E2EE ");
        code[5] = 0;
        lstrcatA(line, code);
        lstrcatA(line, "\xE2\x80\xA6");
    }
    text(g_ui.f_small, C_MUTED, rect(x0 + S(12), cy + S(17), right - x0 - S(52), S(18)), line,
         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    /* Screen share, camera, mute and deafen: the icon shows the state, red when on. */
    {
        struct {
            int on;
            const wchar_t *icon;
        } b[4] = {{g_ui.share_state != VOICE_OFF, L"\xE7F4"},
                  {g_ui.voice_camera, L"\xE714"},
                  {g_ui.voice_muted || g_ui.voice_deafened, g_ui.voice_muted || g_ui.voice_deafened ? L"\xEC54" : L"\xE720"},
                  {g_ui.voice_deafened, g_ui.voice_deafened ? L"\xE74F" : L"\xE7F6"}};
        for (int k = 0; k < 4; k++) {
            int bx = x0 + S(8) + k * (bw + S(8)), hover = g_ui.hover_kind == k_voice_buttons[k];
            float t = tween_on(k_voice_buttons[k] + 2000, 0, hover, TW_FAST, bx, by, bw, S(32));
            r_round(bx, by, bw, S(32), S(8), lerp_argb(0xFF121214u, 0xFF1E1E20u, t));
            r_text(g_ui.f_icon, b[k].on ? ARGB(C_RED) : lerp_argb(ARGB(C_MUTED), ARGB(C_INK), t), bx, by, bw, S(32),
                   b[k].icon, -1, rflags(DT_CENTER | DT_VCENTER | DT_SINGLELINE));
        }
    }
    panel_button(HIT_VOICE_LEAVE, right - S(32), cy, S(32), L"\xE778", g_ui.f_icon_mid, 0);
    fill(x0 + S(8), y + h - S(1), right - x0, S(1) > 1 ? S(1) : 1, C_LINE);
}

/* The floating panel: rounded, outlined, over the bottom of the rail and the channel list. */
static void paint_panel(RECT rc)
{
    int x0 = S(PANEL_GAP), y = panel_top(rc), w = panel_right() - x0, h = rc.bottom - S(PANEL_GAP) - y;

    r_round(x0, y, w, h, S(8), ARGB(C_PANEL));
    r_round_outline(x0, y, w, h, S(8), S(1) > 1 ? S(1) : 1, 0xFF18181Au);
    paint_voice_bar(rc);
    paint_user_panel(rc);
}

/* Friends, Nitro, Shop and Quests, a line, then "Direct Messages" and its "+", from y. */
static void paint_home_links(int x0, int y)
{
    static const wchar_t *const icons[NAVS] = {L"\xE716", L"\xE734", L"\xE719", L"\xE7C1"};
    static const char *const labels[NAVS] = {"Friends", "Nitro", "Shop", "Quests"};
    int w = S(SIDE_W) - S(16), sec;

    for (int k = 0; k < NAVS; k++) {
        int ry = y + k * S(NAV_ROW), sel = g_ui.guild < 0 && g_ui.channel < 0 && g_ui.home_page == k;
        int hov = g_ui.hover_kind == HIT_NAV && g_ui.hover_index == k;
        row_bg(HIT_NAV, k, sel, hov, ARGB(C_SIDE), x0 + S(8), ry + S(1), w, S(NAV_ROW) - S(2), S(8));
        text_w(g_ui.f_icon_mid, sel || hov ? C_INK : C_MUTED, rect(x0 + S(16), ry, S(24), S(NAV_ROW)), icons[k], -1,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(g_ui.f_nav, sel || hov ? C_INK : C_MUTED, rect(x0 + S(52), ry, w - S(52), S(NAV_ROW)), labels[k],
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    sec = y + S(NAVS * NAV_ROW) + S(12);
    fill(x0 + S(8), sec, w, S(1) > 1 ? S(1) : 1, C_LINE);
    sec += S(1);
    {
        int hov = g_ui.hover_kind == HIT_NEW_DM;
        text(g_ui.f_section, hov ? C_INK : C_MUTED, rect(x0 + S(16), sec, w - S(48), S(DM_SECTION)), "Direct Messages",
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        text_w(g_ui.f_icon, hov ? C_INK : C_MUTED, rect(x0 + S(SIDE_W) - S(40), sec, S(32), S(DM_SECTION)), L"\xE710", -1,
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
        if (g_ui.guild < 0)
            paint_home_links(x0, S(HEADER_H) + S(8) - g_ui.side_scroll);
        y += g_ui.guild < 0 ? S(DM_TOP) : 0;
        voice_request_names();
        for (unsigned i = first; i < first + count; i++) {
            if (empty_category(first, count, i) || (chan((int)i)->type != CH_CATEGORY && hidden(first, i)))
                continue;
            if (y + row_height(i) > S(HEADER_H) && y < S(HEADER_H) + view) {
                paint_channel_row(i, y);
                if (is_voice_type(chan((int)i)->type))
                    paint_voice_users(i, y + S(ROW_H));
            }
            y += row_height(i);
        }
        paint_scrollbar(x0 + S(SIDE_W) - S(6), S(HEADER_H) + S(4), view - S(8), side_content(), g_ui.side_scroll);
        r_unclip();

        if (g_ui.guild < 0) {
            /* "Find or start a conversation": the quick switcher, as in Discord. */
            float t = tween_on(HIT_DM_SEARCH, 0, g_ui.hover_kind == HIT_DM_SEARCH, TW_FAST, x0 + S(8), S(9), S(SIDE_W) - S(16), S(31));
            r_round(x0 + S(8), S(9), S(SIDE_W) - S(16), S(31), S(8), lerp_argb(0xFF121213u, 0xFF1A1A1Cu, t));
            r_round_outline(x0 + S(8), S(9), S(SIDE_W) - S(16), S(31), S(8), S(1) > 1 ? S(1) : 1, 0xFF27272Au);
            text(g_ui.f_small_mid, t > .5f ? C_INK : C_TEXT, rect(x0 + S(8), S(9), S(SIDE_W) - S(16), S(31)),
                 "Find or start a conversation", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            /* Discord's guild dropdown: 32 high, 8 in, the name then a chevron, lit on hover */
            float t = tween_on(HIT_GUILD_HEADER, 0, g_ui.hover_kind == HIT_GUILD_HEADER, TW_FAST, x0 + S(8), S(8),
                               S(SIDE_W) - S(16), S(32));
            int tw = text_width(g_ui.f_nav, title), max = S(SIDE_W) - S(32) - S(20);
            if (t > 0.f)
                r_round(x0 + S(8), S(8), S(SIDE_W) - S(16), S(32), S(8), lerp_argb(ARGB(C_SIDE), 0xFF1A1A1Cu, t));
            if (tw > max)
                tw = max;
            text(g_ui.f_nav, C_INK, rect(x0 + S(16), S(8), tw + S(2), S(32)), title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            sicon(SI_CHEVRON_DOWN, x0 + S(16) + tw + S(4), S(8) + S(7), S(18), ARGB(C_INK));
        }
        if (!count)
            text(g_ui.f_body, C_MUTED, rect(x0 + S(16), S(HEADER_H) + S(12), S(SIDE_W) - S(32), S(24)),
                 g_ui.guild >= 0 ? "No channels you can see" : "No conversations yet", DT_LEFT | DT_SINGLELINE);
    } else {
        text(g_ui.f_h, C_INK, rect(x0 + S(16), 0, S(SIDE_W) - S(32), S(HEADER_H)), "Direct Messages",
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    fill(x0, S(HEADER_H) - S(1), S(SIDE_W), S(1) > 1 ? S(1) : 1, C_LINE);
    fill(x0 + S(SIDE_W) - S(1), 0, S(1) > 1 ? S(1) : 1, rc.bottom, C_LINE); /* against the chat */
}

/* ---- Messages ---- */

static void place_composer(void);
static void pop_close(void);
static void dm_profile_sync(void);
static void pop_dismiss(void);
static void open_self(void);
static void build_name_fonts(void);
static void profiles_clear(void);
static void pop_place(void);
static void on_font(int id, sb_t *data);
static void on_profile(profile_t *p);
static void paint_toolbar(void);
static void gifs_paint(int w, int h);
static void gifs_click(int x, int y);
static void on_gifs(const sb_t *p);
static void on_commands(const sb_t *p);
static void posts_clear(void);
static void on_forum(const sb_t *p);
static int forum_view(void);
static void paint_forum(RECT rc, int x0, int w);
static wchar_t *plain_text(const sb_t *text);
static void paint_search(void);
static void paint_detached(int x0, int w, int cy);
static int open_discord_link(const char *url);
static void jump_to(int i);
static LRESULT CALLBACK search_edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static void qs_open(void);
static void qs_close(void);
static void qs_rebuild(void);
static void prompt_submit(void);
static void qs_place(void);
static int pins_button_x(void);
static int call_button_x(void);
static void paint_settings(RECT rc);
static void settings_open(void);
static void settings_open_voice(void);
static int run_menu(HMENU menu);
static void guild_menu(int g);
static POINT g_menu_at; /* where run_menu() opens instead of at the pointer, while g_menu_at_set */
static int g_menu_at_set;
static void place_search(void);
static void place_friend_input(void);
static const char *const k_status_codes[] = {"online", "idle", "dnd", "invisible"};
static const char *const k_status_names[] = {"Online", "Idle", "Do Not Disturb", "Invisible"};
static const int k_status_states[] = {ML_ONLINE, ML_IDLE, ML_DND, ML_OFFLINE};
static int search_box_x(void);
static int members_shown(void);

/* The chat header's buttons, right to left from the search box, as in Discord. */
enum { HB_MEMBERS, HB_PINS, HB_VIDEO, HB_CALL, HB_BELL, HB_COUNT };

static int header_buttons(int *out)
{
    const channel_t *c;
    int n = 0;

    if (!g_ui.model || g_ui.channel < 0 || g_ui.view != VIEW_APP)
        return 0;
    c = chan(g_ui.channel);
    if (is_voice_type(c->type))
        return 0;
    if (g_ui.guild >= 0 || c->type == CH_GROUP_DM || (c->type == CH_DM && c->user_id[0]))
        out[n++] = HB_MEMBERS; /* a DM's: its person's profile */
    out[n++] = HB_PINS;
    if (g_ui.guild >= 0)
        out[n++] = HB_BELL; /* the channel's notifications, left of the pins as in Discord */
    if (is_dm_type(c->type)) {
        out[n++] = HB_VIDEO;
        out[n++] = HB_CALL;
    }
    return n;
}

/* Left edge of a header button's 32 wide box, far off when it is not shown. */
static int header_button_x(int kind)
{
    int k[HB_COUNT], n = header_buttons(k);

    for (int i = 0; i < n; i++)
        if (k[i] == kind)
            return search_box_x() - S(12) - S(32) - i * S(40);
    return -100000;
}
static void paint_pins(void);
static void pins_close(void);
static int divider_h(const msg_t *m);
static const char *find_str(const char *hay, const char *needle);
static void paint_friends(RECT rc, int x0, int w);
static void paint_shop(RECT rc, int x0, int w);
static int shop_view(void);
static int shop_hit(int x, int y);
static void shop_scroll_by(int delta);
static void paint_quests(RECT rc, int x0, int w);
static int quests_view(void);
static int quests_hit(int x, int y);
static void quests_scroll_by(int delta);
static void paint_nitro(RECT rc, int x0, int w);
static int nitro_view(void);
static int nitro_hit(int x, int y);
static void nitro_click(int id);
static void nitro_scroll_to(int to);
static void rels_clear(void);
static void paint_autocomplete(void);
static void ac_update(void);
static int tray_h(void);
static void paint_tray(int x0, int w, int bottom);
static void uploads_clear(void);
static void picker_open(int mode, const char *msg_id, int right, int bottom);
static void picker_close(void);
static void picker_rebuild(void);
static void picker_layout(void);
static int members_shown(void);
static void paint_members(RECT rc);
static void on_member_list(json_t d);
static void paint_bar(int x0, int w, int cy);
static void paint_confirm(void);
static void paint_typing(int x0, int w, int y);
static void typing_stop(const char *user);
static void typing_clear(void);
static void on_typing(const sb_t *p);

#define GROUP_MS (7 * 60 * 1000)
#define COMPOSER_H 56
#define COMPOSER_GAP 8   /* around the box, as in Discord */
#define TYPING_H 24      /* above the box: "X is typing..." */
/* The composer's buttons, right to left: emoji, stickers, GIFs. */
enum { CB_NONE, CB_EMOJI, CB_STICKER, CB_GIF, CB_COUNT };
#define WELCOME_H 190

static int is_voice_type(int type)
{
    return type == CH_VOICE || type == CH_STAGE;
}

static int open_is_text(void)
{
    return g_ui.model && g_ui.channel >= 0 && !is_voice_type(chan(g_ui.channel)->type) &&
           chan(g_ui.channel)->type != CH_FORUM && chan(g_ui.channel)->type != CH_MEDIA;
}

/* Local SYSTEMTIME of a snowflake. */
static SYSTEMTIME local_time(const char *id)
{
    ULARGE_INTEGER t;
    FILETIME ft;
    SYSTEMTIME utc, local;

    t.QuadPart = ((unsigned long long)snowflake_ms(id) + FILETIME_UNIX_MS) * 10000ull;
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
    r.right = main_right();
    r.top = S(HEADER_H) + call_h();
    r.bottom = rc.bottom - S(COMPOSER_GAP) - S(COMPOSER_H) - S(TYPING_H) - (g_ui.bar ? S(BAR_H) : 0) - tray_h();
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

/* Whether a message pings us: a user mention, @everyone, or one of our roles. */
static int pings_me(const msg_t *m)
{
    char tag[32];
    const char *s = m->content.data;

    if (!g_ui.model || !s || m->system)
        return 0;
    if (m->mention_everyone)
        return 1;
    if (m->reply_pings && lstrcmpA(m->reply_author, g_ui.model->user_id) == 0)
        return 1; /* a reply to us, with its ping on */
    wsprintfA(tag, "<@%s>", g_ui.model->user_id);
    if (find_str(s, tag))
        return 1;
    wsprintfA(tag, "<@!%s>", g_ui.model->user_id);
    if (find_str(s, tag))
        return 1;
    if (g_ui.guild >= 0)
        for (const char *p = s; (p = find_str(p, "<@&")) != NULL; p += 3) {
            char id[24];
            int k = 0;
            while (p[3 + k] >= '0' && p[3 + k] <= '9' && k < 23) {
                id[k] = p[3 + k];
                k++;
            }
            id[k] = 0;
            if (model_has_role(g_ui.model, g_ui.guild, id))
                return 1;
        }
    return 0;
}

/* grouped: 0 = starts a group, 1 = continues it, 2 = starts a group after a date divider. */
static void update_grouping(void)
{
    int seen_new = 0;

    for (int i = 0; i < g_ui.nmsgs; i++) {
        msg_t *m = &g_ui.msgs[i], *p = i ? &g_ui.msgs[i - 1] : NULL;
        SYSTEMTIME a, b;

        m->height_w = 0;
        m->mentions_me = pings_me(m);
        m->first_new = 0;
        if (!seen_new && g_ui.new_after[0] && model_id_cmp(m->id, g_ui.new_after) > 0 &&
            !(g_ui.model && lstrcmpA(m->author_id, g_ui.model->user_id) == 0)) {
            m->first_new = 1;
            seen_new = 1;
        }
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

/* ---- Presence ---- */

/* Linear, but a hash compare per entry: the friends list looks up every friend at each paint. */
static presence_t *presence_find(const char *user)
{
    unsigned h = key_hash(user);

    for (int i = 0; i < g_ui.npresences; i++)
        if (g_ui.presences[i].hash == h && lstrcmpA(g_ui.presences[i].user, user) == 0)
            return &g_ui.presences[i];
    return NULL;
}

/* A string member into `out` (cleared first), if there is one. */
static void json_str_of(json_t obj, const char *key, sb_t *out)
{
    json_t v;

    sb_clear(out);
    if (json_get(obj, key, &v) && json_type(v) == JSON_STRING)
        json_str(v, out);
}

/*
 * The first activity that is not a custom status: its name, details, state,
 * start and large picture. Pictures are "mp:" media proxy paths or assets of
 * the application; Spotify's covers are on a host we do not fetch from.
 */
static void presence_game(json_t obj, presence_t *p)
{
    json_t acts, a, v, assets, ts;
    json_iter_t it;
    long long type;

    sb_clear(&p->game);
    sb_clear(&p->details);
    sb_clear(&p->state);
    sb_clear(&p->image);
    sb_clear(&p->album);
    p->start = p->end = 0;
    p->listening = 0;
    if (!json_get(obj, "activities", &acts))
        return;
    json_iter(acts, &it);
    while (json_next(&it, NULL, &a)) {
        char app[24] = "";
        sb_t img = {0};
        if (!json_get(a, "type", &v) || !json_int(v, &type) || type == 4)
            continue;
        json_str_of(a, "name", &p->game);
        json_str_of(a, "details", &p->details);
        json_str_of(a, "state", &p->state);
        if (json_get(a, "timestamps", &ts)) {
            if (json_get(ts, "start", &v))
                json_int(v, &p->start);
            if (json_get(ts, "end", &v))
                json_int(v, &p->end);
        }
        p->listening = type == 2;
        if (json_get(a, "application_id", &v))
            json_raw(v, app, sizeof app);
        if (json_get(a, "assets", &assets)) {
            json_str_of(assets, "large_image", &img);
            json_str_of(assets, "large_text", &p->album);
        }
        if (img.len > 8 && CompareStringA(LOCALE_INVARIANT, 0, img.data, 8, "spotify:", 8) == CSTR_EQUAL) {
            sb_add(&p->image, "https://i.scdn.co/image/");
            sb_add(&p->image, img.data + 8);
        } else if (img.len > 3 && img.data[0] == 'm' && img.data[1] == 'p' && img.data[2] == ':') {
            sb_add(&p->image, "https://media.discordapp.net/");
            sb_addn(&p->image, img.data + 3, img.len - 3);
        } else if (img.len && app[0] && img.data[0] >= '0' && img.data[0] <= '9') {
            sb_add(&p->image, "https://cdn.discordapp.com/app-assets/");
            sb_add(&p->image, app);
            sb_add(&p->image, "/");
            sb_add(&p->image, img.data);
            sb_add(&p->image, ".png");
        }
        sb_free(&img);
        return;
    }
}

/* A presence object: {user: {id}, status, activities}. */
static void presence_store(json_t obj)
{
    json_t user, v;
    char id[24] = "";
    presence_t *p;

    /* READY and PRESENCE_UPDATE carry {user: {id}}; merged presences only a user_id. */
    if (json_get(obj, "user", &user) && json_get(user, "id", &v))
        json_raw(v, id, sizeof id);
    else if (json_get(obj, "user_id", &v))
        json_raw(v, id, sizeof id);
    else
        return;
    if (!(p = presence_find(id))) {
        if (g_ui.npresences == g_ui.cap_presences) {
            g_ui.cap_presences = g_ui.cap_presences ? g_ui.cap_presences * 2 : 64;
            g_ui.presences = mem_realloc(g_ui.presences, (size_t)g_ui.cap_presences * sizeof *g_ui.presences);
        }
        p = &g_ui.presences[g_ui.npresences++];
        *p = (presence_t){0};
        lstrcpynA(p->user, id, sizeof p->user);
        p->hash = key_hash(p->user);
    }
    p->status = json_get(obj, "status", &v) ? ml_status(v) : ML_OFFLINE;
    sb_clear(&p->activity);
    ml_activity(obj, &p->activity);
    presence_game(obj, p);
}

static void presences_clear(void)
{
    for (int i = 0; i < g_ui.npresences; i++) {
        sb_free(&g_ui.presences[i].activity);
        sb_free(&g_ui.presences[i].game);
        sb_free(&g_ui.presences[i].details);
        sb_free(&g_ui.presences[i].state);
        sb_free(&g_ui.presences[i].image);
        sb_free(&g_ui.presences[i].album);
    }
    g_ui.npresences = 0;
}

/* Status of a user, ML_UNKNOWN if we have not seen it (and ours as we set it). */
static int user_status(const char *user)
{
    presence_t *p;

    if (g_ui.model && lstrcmpA(user, g_ui.model->user_id) == 0)
        return g_ui.my_status;
    p = presence_find(user);
    return p ? p->status : ML_UNKNOWN;
}

/* ---- Server members ---- */

/* Linear, but a hash compare per entry: each message drawn looks its author up. */
static member_t *member_find(const char *guild, const char *user)
{
    unsigned h = key_hash(user);

    for (int i = 0; i < g_ui.nmembers; i++)
        if (g_ui.members[i].hash == h && lstrcmpA(g_ui.members[i].user, user) == 0 &&
            lstrcmpA(g_ui.members[i].guild, guild) == 0)
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
    mb->hash = key_hash(mb->user);
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

/* A sticker's image (format_type 1 PNG, 2 APNG, 4 GIF), decoded once at message size for the picker too. */
static r_image_t *sticker_image(const char *id, int format)
{
    char key[48], path[128];

    wsprintfA(key, "st:%s", id);
    if (format == 4)
        wsprintfA(path, "https://media.discordapp.net/stickers/%s.gif?size=160", id);
    else
        wsprintfA(path, "/stickers/%s.png?size=160", id);
    return image_get(key, path, S(160));
}

#define MEDIA_MAX_W 550
#define MEDIA_MAX_H 350
#define EMBED_MAX_W 516
#define FILE_W 432
#define REACTION_H 28

enum { PART_NONE, PART_FILE, PART_MEDIA, PART_EMBED_TITLE, PART_REACTION, PART_SPOILER, PART_POLL, PART_COMPONENT };
#define COMP_H 32

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
    int animated = id[0] == 'a';
    const char *num = animated ? id + 1 : id;

    wsprintfA(key, "e:%s%.30s", animated ? "a" : "", num);
    wsprintfA(path, "/emojis/%.30s.%s?size=64", num, animated ? "gif" : "png");
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
    g_ui.image_first = 1;
    img = image_get(key, full.data, w > h ? w : h);
    g_ui.image_first = 0;
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
        r_round(x, y, w, h, S(8), 0xFF17181B);
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
                r_round(x, y, cw, ch, S(8), 0xFF121214);
                r_round_outline(x, y, cw, ch, S(8), 1, 0xFF242426);
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
                    r_round(x, top, ew, ey - top, S(4), 0xFF121214);
                    r_round(x, top, S(4), ey - top, S(2), e->has_color ? 0xFF000000u | e->color : 0xFF2E2E31u);
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

    /* Poll: question, answers with their share, votes and time left. */
    if (m->poll) {
        msg_poll_t *pl = m->poll;
        int pw = w < S(440) ? w : S(440), top = y + gap, py = top + S(16), total = 0, voted = 0;
        long long left = pl->expiry_ms ? pl->expiry_ms - now_ms() : 0;
        for (int k = 0; k < pl->nanswers; k++) {
            total += pl->answers[k].count;
            voted |= pl->answers[k].me;
        }
        {
            int show = voted || pl->final || (pl->expiry_ms && left <= 0);
            int qh = S(24), rows = pl->nanswers * S(48), h = S(16) + qh + S(22) + rows + S(40);
            if (draw && r_visible(top, h)) {
                char foot[96];
                r_round(x, top, pw, h, S(8), 0xFF121214);
                text(g_ui.f_title, C_INK, rect(x + S(16), py, pw - S(32), qh), pl->question.data ? pl->question.data : "",
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                text(g_ui.f_small, C_MUTED, rect(x + S(16), py + qh, pw - S(32), S(20)),
                     pl->multi ? "Select one or more answers" : "Select one answer", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                for (int k = 0; k < pl->nanswers; k++) {
                    msg_answer_t *an = &pl->answers[k];
                    int ay = py + qh + S(26) + k * S(48), aw = pw - S(32), pct = total ? an->count * 100 / total : 0;
                    char right[16];
                    r_round(x + S(16), ay, aw, S(40), S(8), 0xFF242424);
                    if (show && pct)
                        r_round(x + S(16), ay, aw * pct / 100 > S(8) ? aw * pct / 100 : S(8), S(40), S(8),
                                an->me ? 0x66FFB000u : 0x33FFFFFFu);
                    if (an->me)
                        r_round_outline(x + S(16), ay, aw, S(40), S(8), S(2) > 1 ? S(2) : 1, ARGB(C_BRAND));
                    text(g_ui.f_body, C_INK, rect(x + S(28), ay, aw - S(90), S(40)), an->text.data ? an->text.data : "",
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    if (show) {
                        wsprintfA(right, "%d%%", pct);
                        text(g_ui.f_h, C_INK, rect(x + S(16) + aw - S(70), ay, S(58), S(40)), right,
                             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    }
                }
                if (pl->final || (pl->expiry_ms && left <= 0))
                    wsprintfA(foot, "%d vote%s \xE2\x80\xA2 Poll closed", total, total == 1 ? "" : "s");
                else if (left > 3600000ll)
                    wsprintfA(foot, "%d vote%s \xE2\x80\xA2 %dh left", total, total == 1 ? "" : "s", (int)(left / 3600000ll));
                else
                    wsprintfA(foot, "%d vote%s \xE2\x80\xA2 %dm left", total, total == 1 ? "" : "s", (int)(left / 60000ll) + 1);
                text(g_ui.f_small, C_MUTED, rect(x + S(16), top + h - S(34), pw - S(32), S(24)), foot,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            if (hit_part)
                for (int k = 0; k < pl->nanswers; k++) {
                    int ay = py + qh + S(26) + k * S(48);
                    if (hit(hx, hy, x + S(16), ay, pw - S(32), S(40)))
                        *hit_part = (part_t){PART_POLL, k};
                }
            y = top + h;
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
                r_image_t *img = sticker_image(m->sticker_id, m->sticker_format);
                if (img)
                    r_image(img, x, y, S(160), S(160), 0);
            }
            y += S(160);
        }
    }

    /* A bot's buttons and select menus, row by row. */
    if (m->ncomponents) {
        int cx = x, row = m->components[0].row;
        y += gap;
        for (int i = 0; i < m->ncomponents; i++) {
            msg_component_t *c = &m->components[i];
            int bw, lw = c->label.len ? text_width(g_ui.f_h, c->label.data) : 0, link = c->style == BUTTON_LINK;
            int has_emoji = c->emoji.len || c->emoji_id[0];
            if (c->type == COMP_BUTTON)
                bw = S(16) + (has_emoji ? S(20) : 0) + (has_emoji && lw ? S(6) : 0) + lw + (link ? S(20) : 0) + S(16);
            else
                bw = w < S(400) ? w : S(400);
            if (c->row != row || (cx + bw > x + w && cx > x)) {
                row = c->row;
                cx = x;
                y += S(COMP_H) + S(8);
            }
            if (draw && r_visible(y, S(COMP_H))) {
                unsigned ink = c->disabled ? C_MUTED : C_INK;
                if (c->type == COMP_BUTTON) {
                    static const unsigned colors[] = {0xFF4E5058, 0xFF5865F2, 0xFF4E5058, 0xFF248046, 0xFFDA373C, 0xFF4E5058, 0xFF5865F2};
                    unsigned fillc = colors[c->style >= 1 && c->style <= 6 ? c->style : 0];
                    int ix = cx + S(16);
                    r_round(cx, y, bw, S(COMP_H), S(8), c->disabled ? (fillc & 0x00FFFFFFu) | 0x80000000u : fillc);
                    if (c->emoji_id[0]) {
                        r_image_t *img = emoji_image(c->emoji_id, S(20));
                        if (img)
                            r_image(img, ix, y + (S(COMP_H) - S(20)) / 2, S(20), S(20), 0);
                    } else if (c->emoji.len) {
                        text(g_ui.f_body, ink, rect(ix - S(2), y, S(24), S(COMP_H)), c->emoji.data, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    }
                    if (has_emoji)
                        ix += S(20) + (lw ? S(6) : 0);
                    if (lw)
                        text(g_ui.f_h, ink, rect(ix, y, lw + S(2), S(COMP_H)), c->label.data, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    if (link) /* opens a page: the arrow says so */
                        text(g_ui.f_small, ink, rect(ix + lw + S(4), y, S(16), S(COMP_H)), "\xE2\x86\x97",
                             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                } else {
                    r_round(cx, y, bw, S(COMP_H) + S(8), S(8), 0xFF1E1F22);
                    r_round_outline(cx, y, bw, S(COMP_H) + S(8), S(8), 1, 0xFF242426);
                    text(g_ui.f_body, c->label.len && !c->disabled ? C_MUTED : C_FAINT,
                         rect(cx + S(12), y, bw - S(48), S(COMP_H) + S(8)), c->label.len ? c->label.data : "Make a selection",
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    text(g_ui.f_body, ink, rect(cx + bw - S(32), y, S(20), S(COMP_H) + S(8)), "\xE2\x96\xBE",
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
            }
            if (hit_part && hit(hx, hy, cx, y, bw, S(COMP_H) + (c->type == COMP_BUTTON ? 0 : S(8))))
                *hit_part = (part_t){PART_COMPONENT, i};
            if (c->type != COMP_BUTTON)
                y += S(8); /* selects are taller */
            cx += bw + S(8);
        }
        y += S(COMP_H);
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
                r_round(rx, y, pw, S(REACTION_H), S(8), r->me ? 0x33FFB000u : 0xFF17181B);
                if (r->me)
                    r_round_outline(rx, y, pw, S(REACTION_H), S(8), 1, ARGB(C_BRAND));
                if (r->emoji_id[0]) {
                    r_image_t *img = emoji_image(r->emoji_id, S(18));
                    if (img)
                        r_image(img, rx + S(8), y + (S(REACTION_H) - S(18)) / 2, S(18), S(18), 0);
                } else {
                    text(g_ui.f_body, C_INK, rect(rx + S(6), y, S(24), S(REACTION_H)), r->emoji.data ? r->emoji.data : "",
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
                text(g_ui.f_small, r->me ? C_BRAND : C_MUTED, rect(rx + S(32), y, cw + S(4), S(REACTION_H)), count,
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

/* The pins and search panels lay out their messages' embeds with msg_extras, which gives them a view too. */
static void batch_drop_views(msg_batch_t *b)
{
    for (int i = 0; b && i < b->n; i++)
        drop_view(&b->msgs[i]);
}

static void panel_batch_free(msg_batch_t *b, panel_layout_t *l)
{
    batch_drop_views(b);
    msg_batch_free(b);
    mem_free(l->th);
    mem_free(l->eh);
    l->th = l->eh = NULL;
    l->frame = 0;
}

/* Measures b's messages, text tw wide and at most max_th high, at the first band of a paint. */
static void panel_measure(panel_layout_t *l, msg_batch_t *b, int x, int tw, int max_th)
{
    if (l->frame == g_ui.frame)
        return;
    l->frame = g_ui.frame;
    l->th = mem_realloc(l->th, sizeof *l->th * (size_t)(b->n ? b->n : 1));
    l->eh = mem_realloc(l->eh, sizeof *l->eh * (size_t)(b->n ? b->n : 1));
    for (int k = 0; k < b->n; k++) {
        msg_t *m = &b->msgs[k];
        int th = 0;
        if (m->text.len) {
            wchar_t *body = plain_text(&m->text);
            th = r_text_height(g_ui.f_body, body, -1, tw);
            mem_free(body);
        }
        l->th[k] = th > max_th ? max_th : th;
        l->eh[k] = msg_extras(m, x, 0, tw, 0, 0, 0, NULL);
    }
}

static void invalidate_views(void)
{
    for (int i = 0; i < g_ui.nmsgs; i++)
        drop_view(&g_ui.msgs[i]);
    batch_drop_views(g_ui.pins);
    batch_drop_views(g_ui.results);
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

    if (!m->ui)
        msg_rich(m, text_w_px()); /* makes the view */
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

/* Height of message m in the message list, whose text is w wide (text_w_px()). */
static int msg_height_at(msg_t *m, int w)
{
    if (m->height_w != w) {
        int h = m->text.len || m->edited ? r_rich_height(msg_rich(m, w)) : 0;
        h += msg_extras(m, 0, 0, w, 0, 0, 0, NULL);
        if (m->system)
            h = S(16) + S(22);
        else if (m->grouped == 1)
            h = S(2) + (h ? h : S(20)) + S(2);
        else
            h = S(16) + (m->reply.len ? S(22) : 0) + S(22) + h + S(2);
        h += divider_h(m);
        m->height = h;
        m->height_w = w;
    }
    return m->height;
}

static int msg_height(msg_t *m)
{
    return msg_height_at(m, text_w_px());
}

static int messages_height(void)
{
    int h = S(16), w = text_w_px();

    for (int i = 0; i < g_ui.nmsgs; i++)
        h += msg_height_at(&g_ui.msgs[i], w);
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

/* How far local time is ahead of UTC on day "YYYY-MM-DD" (its own daylight saving time), in ms. */
static long long local_offset_ms(const char *date)
{
    long long ms = msg_iso_ms(date);
    unsigned long long t = ((unsigned long long)ms + FILETIME_UNIX_MS) * 10000ull;
    FILETIME ft, uft;
    SYSTEMTIME local, utc;
    ULARGE_INTEGER u;

    if (!ms)
        return 0;
    ft.dwLowDateTime = (DWORD)t;
    ft.dwHighDateTime = (DWORD)(t >> 32);
    /* Midnight of that day, read as local time, converted to UTC. */
    if (!FileTimeToSystemTime(&ft, &local) || !TzSpecificLocalTimeToSystemTime(NULL, &local, &utc) ||
        !SystemTimeToFileTime(&utc, &uft))
        return 0;
    u.LowPart = uft.dwLowDateTime;
    u.HighPart = uft.dwHighDateTime;
    return ((long long)t - (long long)u.QuadPart) / 10000;
}

/*
 * The locale's long date without its weekday ("September 29, 2026", "29 septembre 2026"):
 * the long date pattern with its "dddd" part and the separator after it removed.
 */
static void long_date_no_weekday(const SYSTEMTIME *st, wchar_t *out, int n)
{
    wchar_t pat[80], fmt[80];
    int k = 0;

    if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SLONGDATE, pat, ARRAYSIZE(pat))) {
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, st, NULL, out, n, NULL);
        return;
    }
    for (int i = 0; pat[i] && k < (int)ARRAYSIZE(fmt) - 1;) {
        if (pat[i] == L'\'') { /* quoted text is copied as is */
            fmt[k++] = pat[i++];
            while (pat[i] && pat[i] != L'\'' && k < (int)ARRAYSIZE(fmt) - 2)
                fmt[k++] = pat[i++];
            if (pat[i])
                fmt[k++] = pat[i++];
            continue;
        }
        if (pat[i] == L'd' && pat[i + 1] == L'd' && pat[i + 2] == L'd' && pat[i + 3] == L'd') {
            while (pat[i] == L'd')
                i++;
            while (pat[i] == L',' || pat[i] == L' ' || pat[i] == L'.')
                i++;
            continue;
        }
        fmt[k++] = pat[i++];
    }
    while (k > 0 && (fmt[k - 1] == L',' || fmt[k - 1] == L' '))
        k--; /* a weekday that came last */
    fmt[k] = 0;
    if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, st, fmt, out, n, NULL))
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, st, NULL, out, n, NULL);
}

/* Discord's <t:unix:style> in the user's locale: t, T, d, D, f (default), F, or R for "3 hours ago". */
static void format_timestamp(long long secs, char style, sb_t *out)
{
    unsigned long long t = ((unsigned long long)(secs * 1000) + FILETIME_UNIX_MS) * 10000ull;
    FILETIME ft, local;
    SYSTEMTIME st;
    wchar_t date[80], clock[32], text[128];

    if (style == 'R') {
        static const struct {
            long long secs;
            const char *unit;
        } units[] = {{31536000, "year"}, {2592000, "month"}, {86400, "day"}, {3600, "hour"}, {60, "minute"}};
        long long diff = secs - now_ms() / 1000, abs = diff < 0 ? -diff : diff;
        char buf[64];
        if (abs < 60) {
            sb_add(out, diff < 0 ? "a few seconds ago" : "in a few seconds");
            return;
        }
        for (int k = 0; k < (int)ARRAYSIZE(units); k++)
            if (abs >= units[k].secs) {
                int n = (int)(abs / units[k].secs);
                wsprintfA(buf, diff < 0 ? "%d %s%s ago" : "in %d %s%s", n, units[k].unit, n == 1 ? "" : "s");
                sb_add(out, buf);
                return;
            }
    }
    ft.dwLowDateTime = (DWORD)t;
    ft.dwHighDateTime = (DWORD)(t >> 32);
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, &st);
    date[0] = clock[0] = 0;
    /* d: short date; D and f: long date without the weekday, which only F shows. */
    if (style == 'd' || style == 'F')
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, style == 'd' ? DATE_SHORTDATE : DATE_LONGDATE, &st, NULL, date,
                        ARRAYSIZE(date), NULL);
    else if (style == 'D' || style == 'f')
        long_date_no_weekday(&st, date, ARRAYSIZE(date));
    if (style != 'd' && style != 'D')
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, style == 'T' ? 0 : TIME_NOSECONDS, &st, NULL, clock, ARRAYSIZE(clock));
    wsprintfW(text, L"%s%s%s", date, date[0] && clock[0] ? L" " : L"", clock);
    wide_to_utf8(text, (size_t)lstrlenW(text), out);
}

static int ts_style(char c)
{
    for (const char *p = "tTdDfFR"; *p; p++)
        if (*p == c)
            return 1;
    return 0;
}

/* Appends the name of role `id` in the open server; 0 if unknown. */
static int role_name(const char *id, sb_t *out)
{
    model_role_t r;
    unsigned cursor = 0;

    while (g_ui.model && g_ui.guild >= 0 && model_role_next(g_ui.model, g_ui.guild, &cursor, &r))
        if (lstrcmpA(r.id, id) == 0) {
            sb_addn(out, r.name, (size_t)r.name_len);
            return 1;
        }
    return 0;
}

/* Replaces <#id>, <@&id> and <t:...> with channel names, role names and dates, outside code. */
static void resolve_channels(msg_t *m)
{
    sb_t out = {0};
    const char *s = m->text.data;
    size_t n = m->text.len, i = 0;
    int changed = 0;

    while (i < n) {
        size_t code;
        if (s[i] == '\\' && i + 1 < n) { /* an escaped "<" stays text */
            sb_addn(&out, s + i, 2);
            i += 2;
            continue;
        }
        if (s[i] == '`') {
            size_t j = (code = md_code_end(s, n, i)) != 0 ? code : i + 1;
            sb_addn(&out, s + i, j - i);
            i = j;
            continue;
        }
        if (s[i] == '<' && i + 3 < n && s[i + 1] == '@' && s[i + 2] == '&') {
            size_t j = i + 3;
            while (j < n && s[j] >= '0' && s[j] <= '9')
                j++;
            if (j < n && s[j] == '>' && j - i - 3 < 24) {
                char id[24];
                lstrcpynA(id, s + i + 3, (int)(j - i - 2));
                sb_add(&out, MD_MENTION_OPEN "@");
                if (!role_name(id, &out))
                    sb_add(&out, "unknown-role");
                sb_add(&out, MD_MENTION_CLOSE);
                i = j + 1;
                changed = 1;
                continue;
            }
        }
        if (s[i] == '<' && i + 3 < n && s[i + 1] == 't' && s[i + 2] == ':') {
            size_t j = i + 3;
            long long secs = 0;
            int neg = j < n && s[j] == '-';
            char style = 'f';
            j += neg;
            while (j < n && s[j] >= '0' && s[j] <= '9' && j - i < 20)
                secs = secs * 10 + (s[j++] - '0');
            if (j + 2 < n && s[j] == ':' && s[j + 2] == '>' && ts_style(s[j + 1])) {
                style = s[j + 1];
                j += 2;
            }
            if (j < n && s[j] == '>' && j > i + 3 + neg) {
                format_timestamp(neg ? -secs : secs, style, &out);
                i = j + 1;
                changed = 1;
                continue;
            }
        }
        if (s[i] == '<' && i + 2 < n && s[i + 1] == '#') {
            size_t j = i + 2;
            while (j < n && s[j] >= '0' && s[j] <= '9')
                j++;
            if (j < n && s[j] == '>' && j - i - 2 < 24) {
                char id[24];
                int c;
                lstrcpynA(id, s + i + 2, (int)(j - i - 1));
                c = g_ui.model ? model_find_channel(g_ui.model, id) : -1;
                if (c >= 0) {
                    sb_add(&out, MD_MENTION_OPEN "#");
                    sb_add(&out, model_str(g_ui.model, chan(c)->name));
                    sb_add(&out, MD_MENTION_CLOSE);
                } else {
                    sb_add(&out, "#unknown");
                }
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
    if (b->kind == BATCH_SEARCH) {
        if (g_ui.results_open && !g_ui.results) {
            g_ui.results = b;
            g_ui.results_scroll = 0;
            return;
        }
        msg_batch_free(b);
        return;
    }
    if (b->kind == BATCH_PINS) {
        if (g_ui.pins_open && !g_ui.pins &&
            lstrcmpA(b->channel_id, g_ui.pins_inbox ? INBOX_CHANNEL : g_ui.msgs_channel) == 0) {
            g_ui.pins = b;
            return;
        }
        msg_batch_free(b);
        return;
    }
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
        g_ui.detached = b->around[0] != 0;
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
        typing_stop(b->msgs[0].author_id);
        if (g_ui.msgs_loading || g_ui.detached || find_msg(b->msgs[0].id) >= 0)
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
            /* The raw content too: editing again, copying and our mentions read it. */
            sb_free(&g_ui.msgs[i].content);
            g_ui.msgs[i].content = n->content;
            n->content = (sb_t){0};
            g_ui.msgs[i].mention_everyone = n->mention_everyone;
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
            if (n->ncomponents) {
                msg_components_free(&g_ui.msgs[i]);
                g_ui.msgs[i].components = n->components;
                g_ui.msgs[i].ncomponents = n->ncomponents;
                n->components = NULL;
                n->ncomponents = 0;
            }
            if (n->app_id[0])
                lstrcpynA(g_ui.msgs[i].app_id, n->app_id, sizeof n->app_id);
            if (n->poll) {
                msg_poll_free(g_ui.msgs[i].poll);
                g_ui.msgs[i].poll = n->poll;
                n->poll = NULL;
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
    case BATCH_POLL_VOTE: {
        int i = find_msg(b->msgs[0].id);
        msg_poll_t *pl = i >= 0 ? g_ui.msgs[i].poll : NULL;
        for (int k = 0; pl && k < pl->nanswers; k++)
            if (pl->answers[k].id == b->total && !(b->mine && pl->answers[k].me == (b->delta > 0))) {
                pl->answers[k].count += b->delta; /* ours were counted when clicked */
                if (b->mine)
                    pl->answers[k].me = b->delta > 0;
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
    {
        char around[24];
        lstrcpynA(around, b->kind == BATCH_HISTORY ? b->around : "", sizeof around);
        msg_batch_free(b);
        request_authors();
        update_grouping();
        clamp_msg_scroll();
        if (around[0] && find_msg(around) >= 0)
            jump_to(find_msg(around));
    }
    maybe_load_older();
    place_composer();
}

static r_image_t *shop_image(const char *url, int px);

/* The start of a channel, as Discord's: its icon in a faint circle, "Welcome to #name!" in 32px bold, a line under it. */
static void paint_welcome(int x0, int y, int w, const char *name, int voice)
{
    char title[192]; /* the longest format, 58 bytes, and 120 of the name */
    const channel_t *c = g_ui.channel >= 0 ? chan(g_ui.channel) : NULL;

    if (c && is_dm_type(c->type)) {
        r_image_t *img = dm_icon(c);
        if (img)
            r_image(img, x0 + S(16), y, S(68), S(68), S(34));
        else
            r_circle(x0 + S(16), y, S(68), ARGB(C_ITEM));
        text(g_ui.f_welcome, C_INK, rect(x0 + S(16), y + S(76), w - S(32), S(40)), name,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        wsprintfA(title, c->type == CH_DM ? "This is the beginning of your direct message history with %.120s."
                                          : "Welcome to the beginning of the %.120s group.", name);
        text(g_ui.f_body, C_TEXT, rect(x0 + S(16), y + S(122), w - S(32), S(24)), title,
             DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }
    r_circle(x0 + S(16), y, S(68), 0xFF0C0C0Du); /* background-mod-muted */
    {
        /* Discord's own pictures of the channel's kind, 44px, and ours until they come */
        r_image_t *icon = shop_image(voice ? "https://discord.com/assets/f8169a374e27b270.svg"
                                           : "https://discord.com/assets/4ab68153d28748ee.svg", S(44));
        if (icon)
            r_image(icon, x0 + S(16) + S(12), y + S(12), S(44), S(44), 0);
        else if (voice)
            text_w(g_ui.f_icon_big, C_INK, rect(x0 + S(16), y, S(68), S(68)), ICON_VOLUME, -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        else
            sicon(SI_HASH, x0 + S(16) + S(12), y + S(12), S(44), 0xFFFFFFFFu);
    }
    wsprintfA(title, "Welcome to %s%.120s!", voice ? "" : "#", name);
    text(g_ui.f_welcome, C_INK, rect(x0 + S(16), y + S(76), w - S(32), S(40)), title,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    wsprintfA(title, voice ? "Voice channels are not supported yet." : "This is the start of the #%.120s channel.", name);
    text(g_ui.f_body, C_TEXT, rect(x0 + S(16), y + S(122), w - S(32), S(24)), title,
         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
}

/* Space above a message for the date and "NEW" lines. */
static int divider_h(const msg_t *m)
{
    return (m->grouped == 2 ? S(44) : m->first_new ? S(20) : 0) + (m->first_new && m->grouped == 1 ? S(8) : 0);
}

/* Substring search (no shlwapi). CompareStringA reads all n bytes, so only where n are left. */
static const char *find_in(const char *hay, const char *needle, DWORD flags)
{
    size_t n = (size_t)lstrlenA(needle), left = (size_t)lstrlenA(hay);

    if (!n)
        return left ? hay : NULL;
    for (; left >= n; hay++, left--)
        if (CompareStringA(LOCALE_INVARIANT, flags, hay, (int)n, needle, (int)n) == CSTR_EQUAL)
            return hay;
    return NULL;
}

static const char *find_str(const char *hay, const char *needle)
{
    return find_in(hay, needle, 0);
}

/* Same, ignoring case. */
static const char *find_str_ci(const char *hay, const char *needle)
{
    return find_in(hay, needle, NORM_IGNORECASE);
}

/* The red "NEW" line above the first unread message; without `line`, only its label (a date divider draws the line). */
static void paint_new_line(int x0, int y, int w, int line)
{
    if (line)
        fill(x0 + S(16), y, w - S(32), 1, C_NEW);
    r_round(x0 + w - S(16) - S(36), y - S(8), S(36), S(16), S(4), ARGB(C_NEW));
    text(g_ui.f_cat, C_INK, rect(x0 + w - S(16) - S(36), y - S(8), S(36), S(16)), "NEW", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* The date above the first message of a day, "September 29, 2026" as in Discord; red when the NEW line falls on it. */
static void paint_divider(int x0, int y, int w, const char *id, int is_new)
{
    SYSTEMTIME st = local_time(id);
    wchar_t date[64];
    int tw;

    /* English like the rest of the interface: the user's locale would mix in its month names. */
    if (!GetDateFormatEx(L"en-US", 0, &st, L"MMMM d, yyyy", date, ARRAYSIZE(date), NULL))
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &st, NULL, date, ARRAYSIZE(date), NULL);
    tw = r_text_width(g_ui.f_cat, date, -1) + S(16);
    fill(x0 + S(16), y + S(22), w - S(32), 1, is_new ? C_NEW : C_LINE);
    fill(x0 + (w - tw) / 2, y + S(12), tw, S(20), C_MAIN);
    text_w(g_ui.f_cat, C_FAINT, rect(x0 + (w - tw) / 2, y + S(12), tw, S(20)), date, -1,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void paint_message(int i, int x0, int y, int w)
{
    msg_t *m = &g_ui.msgs[i];
    int tx = text_x(), tw = w - (tx - x0) - S(24), h = msg_height(m);

    if (m->grouped == 2) {
        paint_divider(x0, y, w, m->id, m->first_new);
        if (m->first_new)
            paint_new_line(x0, y + S(22), w, 0);
        y += S(44);
        h -= S(44);
    } else if (m->first_new) {
        paint_new_line(x0, y + S(10), w, 1);
        y += S(20);
        h -= S(20);
    }
    if (m->first_new && m->grouped == 1) {
        y += S(8);
        h -= S(8);
    }
    if (g_ui.flash_id[0] && lstrcmpA(g_ui.flash_id, m->id) == 0) {
        int top = y + (m->grouped == 1 ? 0 : S(12));
        fill(x0, top, w, h - (top - y), C_MENTION_HOVER);
    } else if (m->mentions_me) {
        /* Messages that ping us, like Discord: tinted with a bar on the left. */
        int top = y + (m->grouped == 1 ? 0 : S(12));
        fill(x0, top, w, h - (top - y), g_ui.hover_msg == i ? C_MENTION_HOVER : C_MENTION);
        r_fill(x0, top, S(2), h - (top - y), 0xFFCE8100u); /* text-feedback-warning */
    } else {
        row_bg(TW_MESSAGE, i, 0, g_ui.hover_msg == i, ARGB(C_MAIN), x0, y + (m->grouped == 1 ? 0 : S(12)), w,
               h - (m->grouped == 1 ? 0 : S(12)), 0);
    }

    if (m->system) {
        char line[160];
        text(g_ui.f_body, C_GREEN, rect(x0 + S(16), y + S(16), S(40), S(22)), "\xE2\x86\x92", DT_CENTER | DT_SINGLELINE);
        /* "Ann joined the server.", "Ann's poll ... has closed." */
        wsprintfA(line, "%.60s%s%.90s", m->author.data ? m->author.data : "",
                  m->text.data && m->text.data[0] == '\'' ? "" : " ", m->text.data ? m->text.data : "");
        text(g_ui.f_body, C_MUTED, rect(tx, y + S(16), tw, S(22)), line, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }
    if (m->grouped != 1) {
        wchar_t when[64];
        r_image_t *img;
        int ny = y + S(16);
        RECT nr;

        if (m->reply.len) {
            /* The spine from the avatar up to the message replied to, then its author's picture, "@name" and first line. */
            int th = S(2) > 2 ? S(2) : 2, sx = x0 + S(35), rx = tx;
            unsigned spine = 0xFF3F4046u;
            r_clip(sx, ny + S(10), S(30), S(12));
            r_round_outline(sx, ny + S(10), S(80), S(80), S(6), th, spine);
            r_unclip();
            if (m->reply_author[0]) {
                r_image_t *face = user_avatar(m->reply_author, m->reply_avatar);
                char name[128];
                int nl = m->reply_name_len < (int)sizeof name - 2 ? m->reply_name_len : (int)sizeof name - 2, nw;
                if (face)
                    r_image(face, rx, ny + S(2), S(16), S(16), S(8));
                else
                    r_circle(rx, ny + S(2), S(16), ARGB(C_ITEM));
                rx += S(20);
                name[0] = '@';
                lstrcpynA(name + 1, m->reply.data, nl + 1);
                if (!m->reply_pings)
                    lstrcpynA(name, m->reply.data, nl + 1);
                nw = text_width(g_ui.f_small_mid, name);
                text(g_ui.f_small_mid, C_INK, rect(rx, ny, tw - (rx - tx), S(20)), name, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                rx += nw + S(4);
                if (rx < tx + tw && m->reply.len > (size_t)nl + 2)
                    text(g_ui.f_section, C_MUTED, rect(rx, ny, tw - (rx - tx), S(20)), m->reply.data + nl + 2,
                         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            } else {
                text(g_ui.f_section, C_MUTED, rect(tx, ny, tw, S(20)), m->reply.data, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            ny += S(22);
        }
        img = m->author_id[0] ? user_avatar_anim(m->author_id, m->avatar,
                                                 g_ui.hover_msg >= 0 && m == &g_ui.msgs[g_ui.hover_msg])
                              : NULL;
        if (img)
            r_image(img, x0 + S(16), ny, S(40), S(40), S(20));
        else
            r_circle(x0 + S(16), ny, S(40), ARGB(C_ITEM));
        nr = rect(tx, ny, tw, S(22));
        {
            const char *author = author_name(m);
            unsigned color = author_color(m);
            wchar_t *wn = utf8_to_wide(author, lstrlenA(author));
            r_text(g_ui.f_h, color ? 0xFF000000u | color : ARGB(C_INK), nr.left, nr.top, nr.right - nr.left,
                   nr.bottom - nr.top, wn, -1, R_LEFT | R_SINGLE | R_ELLIPSIS);
            nr.left += r_text_width(g_ui.f_h, wn, -1) + S(10);
            mem_free(wn);
        }
        format_time(m->id, when, ARRAYSIZE(when));
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

static void paint_messages(const char *name)
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
            r_round(a.right - S(10), ty, S(6), th, S(3), 0xFF242426);
        }
    }
}

/* One tile of the voice grid: someone, or their Go Live stream. */
static void grid_tile(const voice_t *v, int stream, int x, int y, int w, int h)
{
    const char *name = v->name.len ? v->name.data : "\xE2\x80\xA6";

    if (stream)
        paint_stream_tile(v, name, x, y, w, h);
    else
        paint_tile(v, name, v->avatar, x, y, w, h, g_ui.voice_state == VOICE_CONNECTED && app_voice_speaking(v->user));
}

/*
 * The members of the voice channel we are in, and the streams going on, as
 * tiles of 16:9 filling the view; a stream we watch fills it instead, the
 * others in a row under it.
 */
static void paint_voice_grid(RECT rc, int x0, int w, const char *channel)
{
    struct {
        const voice_t *v;
        int stream;
    } tiles[64];
    const voice_t *watched = watched_in(channel);
    int n = 0, cols = 1, rows, tw, th, top = S(HEADER_H) + S(16), avail_h = rc.bottom - top - S(16);

    fill(x0, S(HEADER_H), w, rc.bottom - S(HEADER_H), C_RAIL);
    for (int i = 0; i < g_ui.nvoices && n < (int)ARRAYSIZE(tiles) - 1; i++) {
        const voice_t *v = &g_ui.voices[i];
        if (lstrcmpA(v->channel, channel) != 0)
            continue;
        tiles[n].v = v;
        tiles[n++].stream = 0;
        if ((v->flags & VOICE_STREAM) && v != watched) {
            tiles[n].v = v;
            tiles[n++].stream = 1;
        }
    }
    if (watched) {
        /* The stream above, everyone else in a row as wide as fits. */
        int rh = S(104), rw = rh * 16 / 9, fit = (w - S(32) + S(8)) / (rw + S(8)), shown = n < fit ? n : fit;
        int sh = avail_h - (shown ? rh + S(12) : 0), x = x0 + (w - (shown * (rw + S(8)) - S(8))) / 2;
        paint_stream_tile(watched, watched->name.len ? watched->name.data : "\xE2\x80\xA6", x0 + S(16), top,
                          w - S(32), sh);
        for (int k = 0; k < shown; k++, x += rw + S(8))
            grid_tile(tiles[k].v, tiles[k].stream, x, top + sh + S(12), rw, rh);
        return;
    }
    if (!n)
        return;
    while (cols * cols < n)
        cols++;
    rows = (n + cols - 1) / cols;
    tw = (w - S(32) - (cols - 1) * S(8)) / cols;
    th = tw * 9 / 16;
    if (rows * th + (rows - 1) * S(8) > avail_h) {
        th = (avail_h - (rows - 1) * S(8)) / rows;
        tw = th * 16 / 9;
    }
    for (int k = 0; k < n; k++) {
        int r = k / cols, col = k % cols, in_row = r == rows - 1 ? n - r * cols : cols;
        int row_w = in_row * tw + (in_row - 1) * S(8);
        grid_tile(tiles[k].v, tiles[k].stream, x0 + (w - row_w) / 2 + col * (tw + S(8)),
                  top + (avail_h - (rows * th + (rows - 1) * S(8))) / 2 + r * (th + S(8)), tw, th);
    }
}

/* The chat header: the conversation's picture or its "#", its name and topic, then call, video, pins, members and search. */
static void paint_chat_header(RECT rc, int x0, const channel_t *c, const char *name, int voice)
{
    int k[HB_COUNT], n = header_buttons(k), sx = search_box_x(), right = sx - S(12) - n * S(40), tx;

    (void)rc;
    if (is_dm_type(c->type)) {
        int ay = (S(HEADER_H) - S(24)) / 2;
        paint_dm_icon(c, x0 + S(16), ay, S(24), ARGB(C_MAIN));
        if (c->type == CH_DM && c->user_id[0]) {
            presence_t *pr = presence_find(c->user_id);
            paint_status(x0 + S(16), ay, S(24), pr ? pr->status : ML_OFFLINE, ARGB(C_MAIN));
        }
        tx = x0 + S(52);
    } else {
        if (voice)
            text_w(g_ui.f_icon_mid, C_FAINT, rect(x0 + S(12), 0, S(28), S(HEADER_H)), ICON_VOLUME, -1,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        else
            sicon(SI_HASH, x0 + S(14), (S(HEADER_H) - S(24)) / 2, S(24), ARGB(C_CHANNEL));
        tx = x0 + S(48);
    }
    {
        int nw = text_width(g_ui.f_h, name), topic = tx + nw + S(16);
        text(g_ui.f_h, C_INK, rect(tx, 0, right - tx, S(HEADER_H)), name, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (c->topic && topic + S(40) < right) {
            fill(topic - S(8), S(16), S(1) > 1 ? S(1) : 1, S(16), C_LINE);
            text(g_ui.f_section, C_MUTED, rect(topic + S(8), 0, right - topic - S(8), S(HEADER_H)), model_str(g_ui.model, c->topic),
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    for (int i = 0; i < n; i++) {
        static const wchar_t *const glyphs[HB_COUNT] = {L"\xE716", L"\xE718", L"\xE714", L"\xE717", L"\xEA8F"};
        int bx = header_button_x(k[i]), on = 0;
        unsigned ink = 0xFF81828Au; /* icon-muted */
        if (k[i] == HB_MEMBERS)
            on = members_shown();
        else if (k[i] == HB_PINS)
            on = g_ui.pins_open && !g_ui.pins_inbox;
        if (on)
            ink = ARGB(C_INK);
        if (k[i] == HB_CALL && in_call(c->id))
            ink = ARGB(C_GREEN);
        r_text(g_ui.f_icon_mid, ink, bx, 0, S(32), S(HEADER_H),
               k[i] == HB_MEMBERS && c->type == CH_DM ? L"\xE77B"
               : k[i] == HB_BELL && c->muted          ? L"\xE7ED" /* the bell struck through */
                                                      : glyphs[k[i]],
               -1, R_CENTER | R_VCENTER | R_SINGLE);
    }
    if (!voice) {
        r_round(sx, S(8), S(SEARCH_W), S(32), S(8), ARGB(C_MAIN));
        r_round_outline(sx, S(8), S(SEARCH_W), S(32), S(8), S(1) > 1 ? S(1) : 1, 0xFF242426u);
        r_text(g_ui.f_icon, 0xFF81828Au, sx + S(SEARCH_W) - S(32), S(8), S(28), S(32), L"\xE721", -1,
               R_CENTER | R_VCENTER | R_SINGLE);
    }
}

static void paint_main(RECT rc)
{
    int x0 = S(RAIL_W + SIDE_W), w = main_right() - x0;

    /* The streams' buttons are drawn again below, wherever they are now. */
    g_ui.nlive_hits = 0;
    SetRectEmpty(&g_ui.stream_close);

    fill(x0, 0, w, rc.bottom, C_MAIN);
    /* The header runs over the member list too, as in Discord. */
    fill(x0, 0, rc.right - x0, S(HEADER_H), C_MAIN);
    fill(x0, S(HEADER_H) - S(1), rc.right - x0, S(1) > 1 ? S(1) : 1, C_LINE);

    if (g_ui.model && g_ui.channel >= 0) {
        const channel_t *c = chan(g_ui.channel);
        const char *name = model_str(g_ui.model, c->name);
        int voice = is_voice_type(c->type);

        paint_chat_header(rc, x0, c, name, voice);

        if (voice && in_call(c->id)) {
            paint_voice_grid(rc, x0, w, c->id);
            return;
        }
        if (voice) {
            paint_welcome(x0 + S(8), rc.bottom - S(24) - S(WELCOME_H), w, name, 1);
            return;
        }
        if (forum_view()) {
            paint_forum(rc, x0, w);
            return;
        }
        paint_messages(name);
        paint_call(x0, w);
        paint_toolbar();
        paint_autocomplete();
        paint_pins();
        paint_search();

        /* Composer frame; the edit control sits inside it. */
        {
            int cy = rc.bottom - S(COMPOSER_GAP) - S(COMPOSER_H);
            paint_tray(x0, w, cy - (g_ui.bar ? S(BAR_H) : 0));
            paint_bar(x0, w, cy);
            paint_detached(x0, w, cy);
            /* The box: chat-background-default, outlined, as in Discord's current look. */
            r_round(x0 + S(COMPOSER_GAP), cy, w - 2 * S(COMPOSER_GAP), S(COMPOSER_H), S(8), 0xFF101013u);
            r_round_outline(x0 + S(COMPOSER_GAP), cy, w - 2 * S(COMPOSER_GAP), S(COMPOSER_H), S(8), S(1) > 1 ? S(1) : 1,
                            0xFF25252Au);
            /* Attach: a plain "+", drawn as two bars so it stays centered. */
            {
                int d = S(24), bx = x0 + S(COMPOSER_GAP) + S(16), by = cy + (S(COMPOSER_H) - d) / 2, len = S(16);
                int th = S(2) > 2 ? S(2) : 2;
                float t = tween_on(TW_ATTACH, 0, g_ui.hover_attach, TW_FAST, bx, by, d, d);
                unsigned ink = lerp_argb(ARGB(C_MUTED), ARGB(C_INK), t);
                r_round(bx + (d - len) / 2, by + (d - th) / 2, len, th, th / 2, ink);
                r_round(bx + (d - th) / 2, by + (d - len) / 2, th, len, th / 2, ink);
            }
            /* GIFs, stickers and emoji open the picker on their tab; lit while it is open there. */
            for (int b = CB_EMOJI; b < CB_COUNT; b++) {
                static const wchar_t *const glyphs[CB_COUNT] = {NULL, L"\xE76E", L"\xE8EC", L"\xF4A9"};
                int bx = x0 + w - S(COMPOSER_GAP) - S(8) - b * S(40), tab = b == CB_EMOJI ? TAB_EMOJI : b == CB_GIF ? TAB_GIFS : TAB_STICKERS;
                int open = g_ui.picker && g_ui.picker_mode == PICK_COMPOSER && g_ui.picker_tab == tab;
                float t = tween_on(TW_EMOJI, b, open || g_ui.hover_emoji == b, TW_FAST, bx, cy, S(40), S(COMPOSER_H));
                if (b == CB_GIF) {
                    /* "GIF" in a rounded box, like Discord's button */
                    unsigned ink = lerp_argb(ARGB(C_MUTED), ARGB(C_INK), t);
                    r_round_outline(bx + S(9), cy + (S(COMPOSER_H) - S(18)) / 2, S(22), S(18), S(4), S(2) > 2 ? S(2) : 2, ink);
                    r_text(g_ui.f_gif, ink, bx + S(9), cy + (S(COMPOSER_H) - S(18)) / 2, S(22), S(18), L"GIF", -1,
                           R_CENTER | R_VCENTER | R_SINGLE);
                } else {
                    r_text(g_ui.f_icon_mid, lerp_argb(ARGB(C_MUTED), ARGB(C_INK), t), bx, cy, S(40), S(COMPOSER_H), glyphs[b], -1,
                           R_CENTER | R_VCENTER | R_SINGLE);
                }
            }
            if (g_ui.send_error.len)
                text(g_ui.f_small, C_RED, rect(x0 + S(20), cy - S(20) - (g_ui.bar ? S(BAR_H) : 0), w - S(40), S(18)),
                     g_ui.send_error.data, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            paint_typing(x0, w, cy - (g_ui.bar ? S(BAR_H) : 0) - tray_h() - S(TYPING_H) + S(2));
        }
    } else if (friends_view()) {
        fill(x0, S(HEADER_H) - 1, w, 1, C_LINE);
        paint_friends(rc, x0, w);
    } else if (shop_view()) {
        paint_shop(rc, x0, w);
    } else if (quests_view()) {
        paint_quests(rc, x0, w);
    } else if (nitro_view()) {
        paint_nitro(rc, x0, w);
    } else if (g_ui.model && g_ui.guild < 0 && g_ui.channel < 0) {
        /* A home page not drawn above: its header only. */
        static const wchar_t *const icons[NAVS] = {L"\xE716", L"\xE734", L"\xE719", L"\xE7C1"};
        static const char *const titles[NAVS] = {"Friends", "Nitro", "Shop", "Quests"};
        int k = g_ui.home_page >= 0 && g_ui.home_page < NAVS ? g_ui.home_page : 0;
        fill(x0, S(HEADER_H) - S(1), w, S(1) > 1 ? S(1) : 1, C_LINE);
        text_w(g_ui.f_icon_mid, C_MUTED, rect(x0 + S(16), 0, S(24), S(HEADER_H)), icons[k], -1,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(g_ui.f_h, C_INK, rect(x0 + S(48), 0, w - S(64), S(HEADER_H)), titles[k], DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        text(g_ui.f_body, C_MUTED, rect(x0, rc.bottom / 2 - S(12), w, S(24)), "Not in Silicord yet.",
             DT_CENTER | DT_SINGLELINE);
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

/* ---- Who reacted ---- */

static void reaction_key(const msg_t *m, const msg_reaction_t *r, char *out)
{
    wsprintfA(out, "%.24s %.24s%.40s", m->id, r->emoji_id, r->emoji.data ? r->emoji.data : "");
}

/* The pointer is over reaction k of message i (i < 0: over none): the tooltip comes after a pause. */
static void react_hover(int i, int k, int x, int y)
{
    char key[96] = "";

    if (i >= 0 && i < g_ui.nmsgs && k >= 0 && k < g_ui.msgs[i].nreactions)
        reaction_key(&g_ui.msgs[i], &g_ui.msgs[i].reactions[k], key);
    if (lstrcmpA(key, g_ui.react_key) == 0)
        return;
    lstrcpynA(g_ui.react_key, key, sizeof g_ui.react_key);
    g_ui.react_x = x;
    g_ui.react_y = y;
    if (g_ui.react_shown)
        redraw();
    g_ui.react_shown = 0;
    KillTimer(g_ui.wnd, TIMER_REACTORS);
    if (key[0])
        SetTimer(g_ui.wnd, TIMER_REACTORS, REACTORS_DELAY, NULL);
}

static const msg_reaction_t *hovered_reaction(const msg_t **msg)
{
    for (int i = 0; g_ui.react_key[0] && i < g_ui.nmsgs; i++)
        for (int k = 0; k < g_ui.msgs[i].nreactions; k++) {
            char key[96];
            reaction_key(&g_ui.msgs[i], &g_ui.msgs[i].reactions[k], key);
            if (lstrcmpA(key, g_ui.react_key) == 0) {
                *msg = &g_ui.msgs[i];
                return &g_ui.msgs[i].reactions[k];
            }
        }
    return NULL;
}

/* The pause is over: show what we know, and ask Discord for the names. */
static void react_timer(void)
{
    const msg_t *m;
    const msg_reaction_t *r;

    KillTimer(g_ui.wnd, TIMER_REACTORS);
    if (!(r = hovered_reaction(&m)))
        return;
    g_ui.react_shown = 1;
    if (lstrcmpA(g_ui.reactors_key, g_ui.react_key) != 0) {
        sb_clear(&g_ui.reactors);
        g_ui.reactors_key[0] = 0;
        app_fetch_reactors(m->channel_id[0] ? m->channel_id : g_ui.msgs_channel, m->id, r, g_ui.react_key);
    }
    redraw();
}

static void on_reactors(const char *key, json_t users)
{
    const msg_t *m;
    const msg_reaction_t *r;
    json_iter_t it;
    json_t u, v;
    int n = 0;
    char more[64];

    if (lstrcmpA(key, g_ui.react_key) != 0 || !(r = hovered_reaction(&m)))
        return;
    sb_clear(&g_ui.reactors);
    json_iter(users, &it);
    while (n < 3 && json_next(&it, NULL, &u)) {
        if (n)
            sb_add(&g_ui.reactors, n == 2 && r->count == 3 ? " and " : ", ");
        if (!((json_get(u, "global_name", &v) && json_type(v) == JSON_STRING && json_str(v, &g_ui.reactors)) ||
              (json_get(u, "username", &v) && json_str(v, &g_ui.reactors))))
            sb_add(&g_ui.reactors, "someone");
        n++;
    }
    if (r->count > n && n) {
        wsprintfA(more, " and %d other%s", r->count - n, r->count - n == 1 ? "" : "s");
        sb_add(&g_ui.reactors, more);
    }
    sb_add(&g_ui.reactors, " reacted with ");
    if (r->emoji_id[0]) {
        sb_add(&g_ui.reactors, ":");
        sb_add(&g_ui.reactors, r->emoji.data ? r->emoji.data : "");
        sb_add(&g_ui.reactors, ":");
    } else {
        sb_add(&g_ui.reactors, r->emoji.data ? r->emoji.data : "");
    }
    lstrcpynA(g_ui.reactors_key, key, sizeof g_ui.reactors_key);
    redraw();
}

/* Above the reaction, as in Discord. */
static void paint_reactors(void)
{
    RECT rc;
    int tw, th = S(36), x, y;

    if (!g_ui.react_shown || !g_ui.reactors.len || lstrcmpA(g_ui.reactors_key, g_ui.react_key) != 0)
        return;
    GetClientRect(g_ui.wnd, &rc);
    tw = text_width(g_ui.f_body, g_ui.reactors.data) + S(24);
    if (tw > S(420))
        tw = S(420);
    x = g_ui.react_x - tw / 2;
    if (x + tw > rc.right - S(8))
        x = rc.right - S(8) - tw;
    if (x < S(8))
        x = S(8);
    y = g_ui.react_y - S(24) - th;
    r_round(x, y, tw, th, S(6), ARGB(C_TIP));
    text(g_ui.f_body, C_INK, rect(x + S(12), y, tw - S(24), th), g_ui.reactors.data,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void paint_tooltip(void)
{
    const char *name;
    int y, tw, th = S(36);

    if ((g_ui.hover_kind != HIT_GUILD && g_ui.hover_kind != HIT_FOLDER) || !g_ui.model)
        return;
    if (g_ui.hover_kind == HIT_FOLDER) {
        const folder_t *f = &g_ui.model->folders[g_ui.hover_index];
        name = f->name ? model_str(g_ui.model, f->name) : "Server folder";
    } else {
        name = model_str(g_ui.model, g_ui.model->guilds[g_ui.hover_index].name);
    }
    tw = text_width(g_ui.f_h, name) + S(24);
    if (tw > S(320))
        tw = S(320);
    /* hover_index is a folder index for folders, not a guild one. */
    y = (g_ui.hover_kind == HIT_FOLDER ? rail_folder_y(g_ui.hover_index) : rail_y(g_ui.hover_index)) + (S(ICON) - th) / 2;
    r_round(S(RAIL_W) + S(4), y, tw, th, S(6), ARGB(C_TIP));
    text(g_ui.f_h, C_INK, rect(S(RAIL_W) + S(16), y, tw - S(24), th), name,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

/* ---- Screen picker ---- */

#define PICK_TW 240 /* a screen's still */
#define PICK_TH 135
enum { PICK_SOUND = 100, PICK_CANCEL, PICK_INSIDE };

static int pick_cols(void)
{
    return g_ui.nscreens < 3 ? (g_ui.nscreens > 0 ? g_ui.nscreens : 1) : 3;
}

static RECT pick_rect(void)
{
    RECT rc;
    int cols = pick_cols(), rows = (g_ui.nscreens + cols - 1) / cols;
    int w = cols * S(PICK_TW + 16) - S(16) + 2 * S(24), h = S(84) + rows * S(PICK_TH + 52) + S(64);

    if (w < S(420))
        w = S(420);
    GetClientRect(g_ui.wnd, &rc);
    return rect((rc.right - w) / 2, (rc.bottom - h) / 2, w, h);
}

/* Where screen i's still goes. */
static RECT pick_tile(int i)
{
    RECT r = pick_rect();
    int cols = pick_cols(), grid = cols * S(PICK_TW + 16) - S(16);

    return rect(r.left + (r.right - r.left - grid) / 2 + i % cols * S(PICK_TW + 16), r.top + S(84) + i / cols * S(PICK_TH + 52),
                S(PICK_TW), S(PICK_TH));
}

static void share_pick_close(void)
{
    for (int i = 0; i < g_ui.nscreens; i++) {
        r_image_free(g_ui.screen_stills[i]);
        g_ui.screen_stills[i] = NULL;
    }
    g_ui.nscreens = 0;
    g_ui.share_pick = 0;
    g_ui.share_pick_hover = -1;
    redraw();
}

/* Opens the picker with a still of each screen, taken now. */
static void share_pick_open(void)
{
    g_ui.nscreens = screen_list(g_ui.screens, (int)ARRAYSIZE(g_ui.screens));
    for (int i = 0; i < g_ui.nscreens; i++) {
        const RECT *m = &g_ui.screens[i].rect;
        int w, h;
        picture_fit(m->right - m->left, m->bottom - m->top, S(PICK_TW), S(PICK_TH), &w, &h);
        g_ui.screen_stills[i] = r_image_blank(w, h);
        if (g_ui.screen_stills[i] && !screen_thumbnail(&g_ui.screens[i], w, h, r_image_bits(g_ui.screen_stills[i]))) {
            r_image_free(g_ui.screen_stills[i]);
            g_ui.screen_stills[i] = NULL;
        }
    }
    g_ui.share_pick = 1;
    g_ui.share_pick_hover = -1;
    redraw();
}

static void paint_share_pick(void)
{
    RECT rc, r;
    int bx, by;

    if (!g_ui.share_pick)
        return;
    GetClientRect(g_ui.wnd, &rc);
    r_fill(0, 0, rc.right, rc.bottom, 0xB0000000u);
    r = pick_rect();
    r_round(r.left, r.top, r.right - r.left, r.bottom - r.top, S(10), 0xFF151515);
    r_round_outline(r.left, r.top, r.right - r.left, r.bottom - r.top, S(10), 1, 0xFF242426);
    text(g_ui.f_title, C_INK, rect(r.left + S(24), r.top + S(18), r.right - r.left - S(48), S(28)), "Share Your Screen",
         DT_LEFT | DT_SINGLELINE);
    text(g_ui.f_body, C_MUTED, rect(r.left + S(24), r.top + S(50), r.right - r.left - S(48), S(22)),
         g_ui.nscreens ? "Choose the screen the others will see." : "No screen can be shared.", DT_LEFT | DT_SINGLELINE);
    for (int i = 0; i < g_ui.nscreens; i++) {
        RECT t = pick_tile(i);
        const RECT *m = &g_ui.screens[i].rect;
        char label[64];
        r_round(t.left, t.top, S(PICK_TW), S(PICK_TH), S(6), 0xFF121214);
        if (g_ui.screen_stills[i]) {
            int w, h;
            r_image_size(g_ui.screen_stills[i], &w, &h);
            r_image(g_ui.screen_stills[i], t.left + (S(PICK_TW) - w) / 2, t.top + (S(PICK_TH) - h) / 2, w, h, S(6));
        }
        if (g_ui.share_pick_hover == i)
            r_round_outline(t.left - S(3), t.top - S(3), S(PICK_TW) + S(6), S(PICK_TH) + S(6), S(8), S(2), ARGB(C_GREEN));
        wsprintfA(label, "Screen %d%s \xC2\xB7 %d\xC3\x97%d", i + 1, g_ui.screens[i].primary ? " (main)" : "",
                  m->right - m->left, m->bottom - m->top);
        text(g_ui.f_small, g_ui.share_pick_hover == i ? C_INK : C_MUTED, rect(t.left, t.top + S(PICK_TH) + S(8), S(PICK_TW), S(20)),
             label, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    /* The sound of other programs, and a way out. */
    bx = r.left + S(24);
    by = r.bottom - S(52);
    r_round(bx, by + S(9), S(18), S(18), S(4), g_ui.share_no_sound ? 0xFF242426 : ARGB(C_GREEN));
    if (!g_ui.share_no_sound)
        text_w(g_ui.f_icon, C_INK, rect(bx, by + S(9), S(18), S(18)), L"\xE73E", -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    text(g_ui.f_body, g_ui.share_pick_hover == PICK_SOUND ? C_INK : C_MUTED, rect(bx + S(28), by, S(200), S(36)), "Share sound",
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    r_round(r.right - S(120), by, S(96), S(36), S(6), g_ui.share_pick_hover == PICK_CANCEL ? 0xFF242426 : 0xFF1E1E20);
    text(g_ui.f_h, C_INK, rect(r.right - S(120), by, S(96), S(36)), "Cancel", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* A screen's index, PICK_SOUND, PICK_CANCEL, PICK_INSIDE elsewhere in the picker, or -1 outside it. */
static int share_pick_hit(int x, int y)
{
    RECT r = pick_rect();
    POINT pt = {x, y};
    int by = r.bottom - S(52);

    for (int i = 0; i < g_ui.nscreens; i++) {
        RECT t = pick_tile(i);
        t.bottom += S(32); /* its label too */
        if (PtInRect(&t, pt))
            return i;
    }
    if (y >= by && y < by + S(36)) {
        if (x >= r.left + S(24) && x < r.left + S(200))
            return PICK_SOUND;
        if (x >= r.right - S(120) && x < r.right - S(24))
            return PICK_CANCEL;
    }
    return PtInRect(&r, pt) ? PICK_INSIDE : -1;
}

static void share_pick_click(int x, int y)
{
    int h = share_pick_hit(x, y);

    if (h == PICK_SOUND) {
        g_ui.share_no_sound ^= 1;
        redraw();
    } else if (h == PICK_CANCEL || h == -1) {
        share_pick_close();
    } else if (h >= 0 && h < g_ui.nscreens) {
        app_screen_share(1, h, !g_ui.share_no_sound);
        share_pick_close();
        stream_refresh();
    }
}

static void paint_app(RECT rc)
{
    paint_main(rc);
    paint_call_card();
    if (members_shown())
        paint_members(rc);
    paint_side(rc);
    paint_rail(rc);
    /* The panels sit in a frame with a rounded top left corner, outlined, as in Discord. */
    r_clip(S(RAIL_W), 0, rc.right - S(RAIL_W), rc.bottom);
    r_round_outline(S(RAIL_W), 0, rc.right, rc.bottom + S(40), S(8), S(1) > 1 ? S(1) : 1, ARGB(C_LINE));
    r_unclip();
    paint_panel(rc);
    paint_tooltip();
    paint_reactors();
    paint_confirm();
    paint_share_pick();
}

/* ---- Animations ---- */

#define TIMER_ANIM 4

static int window_active(void)
{
    return GetForegroundWindow() == g_ui.top && !IsIconic(g_ui.top);
}

/* Only while the window is in front: nothing moves, nothing wakes up otherwise. */
static void anim_schedule(void)
{
    if (!g_ui.anim_timer && window_active()) {
        g_ui.anim_timer = 1;
        SetTimer(g_ui.wnd, TIMER_ANIM, 40, NULL);
    }
}

/* Moves visible animations on and repaints just where they are. */
static void anim_tick(void)
{
    unsigned now = GetTickCount(), next = 1000;
    int live = 0;

    if (window_active())
        for (int i = 0; i < g_ui.nimages; i++) {
            image_t *im = &g_ui.images[i];
            int before;
            unsigned wait;
            if (!im->img || !r_image_animated(im->img) || g_ui.frame - im->used > 3 || !im->wnd || !IsWindow(im->wnd))
                continue;
            before = r_image_frame(im->img);
            wait = r_image_advance(im->img, now);
            live = 1;
            if (wait < next)
                next = wait;
            if (r_image_frame(im->img) != before) {
                RECT r = r_image_drawn(im->img);
                if (r.right > r.left)
                    InvalidateRect(im->wnd, &r, FALSE);
            }
        }
    KillTimer(g_ui.wnd, TIMER_ANIM);
    g_ui.anim_timer = 0;
    if (live) {
        g_ui.anim_timer = 1;
        SetTimer(g_ui.wnd, TIMER_ANIM, next < 20 ? 20 : next, NULL);
    }
}

/*
 * One paint of a window, the main one or a popup: a new frame (images drawn
 * are marked with it, and decoding from the disk cache is timed from its
 * start), drawn in bands. Returns the frame.
 */
static unsigned paint_frame(HWND wnd, void (*draw)(RECT rc))
{
    PAINTSTRUCT ps;
    RECT rc;
    HDC dc = BeginPaint(wnd, &ps);

    GetClientRect(wnd, &rc);
    g_ui.frame++;
    g_ui.paint_wnd = wnd;
    QueryPerformanceCounter(&g_ui.frame_start);
    tween_frame_start();
    while (rc.right > 0 && rc.bottom > 0 && r_begin(dc, rc.right, rc.bottom)) {
        draw(rc);
        r_end(dc);
    }
    EndPaint(wnd, &ps);
    if (wnd == g_ui.wnd)
        tween_frame_end();
    return g_ui.frame;
}

/*
 * Context menus, drawn as Discord's instead of the system's. They are built
 * as before, with AppendMenuW, and shown by menu_track(): popups of our own
 * (rounded by DWM, as Discord's), submenus opening on hover, the keyboard
 * as in any menu. An item's data says how it is ticked: MENU_CHECK, a box,
 * or MENU_RADIO, one choice of several; or MENU_STATUS + a status, its dot
 * before the label. A label may carry a line of help after a newline.
 */
#define MENU_CHECK 1
#define MENU_RADIO 2
#define MENU_STATUS 0x100
#define MENU_ITEMS 40
#define MENU_LEVELS 3
#define MENU_PAD 8     /* the scroller's padding */
#define MENU_ITEM_H 32 /* an item: 8 around a 14px label, or a 20px accessory */
#define MENU_SEP_H 17  /* a separator: 8 above and below its line */
#define MENU_MIN_W 188
#define MENU_MAX_W 320

typedef struct {
    wchar_t label[64];
    UINT id, type, state;
    ULONG_PTR kind; /* MENU_CHECK, MENU_RADIO, MENU_STATUS + status or 0 */
    HMENU sub;
    int len; /* of the label before its line of help, if any */
} menu_item_t;

typedef struct {
    HWND wnd;
    menu_item_t item[MENU_ITEMS];
    int n, w, h;
    int hover; /* the focused item, -1 if none */
    int open;  /* the item whose submenu shows, -1 if none */
} menu_level_t;

static struct {
    menu_level_t lv[MENU_LEVELS];
    int depth;  /* levels shown */
    int lost;   /* the capture went elsewhere: the menu closes */
} g_menu;

/* Marks the items of `menu` as ticked with a box or a choice: the one with `id`, or all its strings if `id` is 0. */
static void menu_mark(HMENU menu, UINT id, int kind)
{
    MENUITEMINFOW mi;

    mi.cbSize = sizeof mi;
    mi.fMask = MIIM_DATA;
    mi.dwItemData = (ULONG_PTR)kind;
    if (id) {
        SetMenuItemInfoW(menu, id, FALSE, &mi);
        return;
    }
    for (int i = 0, n = GetMenuItemCount(menu); i < n; i++)
        if (!(GetMenuState(menu, (UINT)i, MF_BYPOSITION) & (MF_SEPARATOR | MF_POPUP)))
            SetMenuItemInfoW(menu, (UINT)i, TRUE, &mi);
}

static int menu_item_h(const menu_item_t *it)
{
    return it->type & MFT_SEPARATOR ? S(MENU_SEP_H) : it->label[it->len] ? S(MENU_ITEM_H + 18) : S(MENU_ITEM_H);
}

static int menu_item_y(const menu_level_t *l, int k)
{
    int y = S(MENU_PAD);

    for (int i = 0; i < k; i++)
        y += menu_item_h(&l->item[i]);
    return y;
}

/* The item at (x, y) in the level's window, -1 on a separator, the padding or outside. */
static int menu_item_at(const menu_level_t *l, int x, int y)
{
    int top = S(MENU_PAD);

    if (x < S(MENU_PAD) || x >= l->w - S(MENU_PAD))
        return -1;
    for (int i = 0; i < l->n; i++) {
        int h = menu_item_h(&l->item[i]);
        if (y >= top && y < top + h)
            return l->item[i].type & MFT_SEPARATOR ? -1 : i;
        top += h;
    }
    return -1;
}

static int menu_selectable(const menu_item_t *it)
{
    return !(it->type & MFT_SEPARATOR) && !(it->state & MFS_GRAYED);
}

/* Discord's destructive entries, in red. */
static int menu_danger(const wchar_t *label)
{
    return !lstrcmpW(label, L"Delete Message") || !lstrcmpW(label, L"Leave Server") || !lstrcmpW(label, L"Remove Friend");
}

/* "Copy ... ID": Discord's ID badge. */
static int menu_is_id(const wchar_t *label)
{
    int n = lstrlenW(label);

    return n > 3 && label[n - 3] == ' ' && label[n - 2] == 'I' && label[n - 1] == 'D';
}

/* The icon Discord shows on the right of an entry, as a glyph of the icon font; NULL if none. */
static const wchar_t *menu_icon(const wchar_t *label)
{
    static const struct {
        const wchar_t *label, *glyph;
    } k[] = {
        {L"Add Reaction", L"\xE76E"},  {L"Reply", L"\xE97A"},         {L"Edit Message", L"\xE70F"},
        {L"Forward", L"\xE72A"},       {L"Create Thread", L"\xE8F2"},  {L"Copy Text", L"\xE8C8"},
        {L"Pin Message", L"\xE718"},   {L"Unpin Message", L"\xE77A"},  {L"Mark Unread", L"\xE715"},
        {L"Copy Message Link", L"\xE71B"}, {L"Copy Link", L"\xE71B"}, {L"Delete Message", L"\xE74D"},
    };

    for (int i = 0; i < (int)ARRAYSIZE(k); i++)
        if (!lstrcmpW(label, k[i].label))
            return k[i].glyph;
    return NULL;
}

static void menu_load(menu_level_t *l, HMENU menu)
{
    int n = GetMenuItemCount(menu), w = S(MENU_MIN_W);

    l->n = 0;
    l->hover = l->open = -1;
    for (int i = 0; i < n && l->n < MENU_ITEMS; i++) {
        menu_item_t *it = &l->item[l->n];
        MENUITEMINFOW mi;
        mi.cbSize = sizeof mi;
        mi.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING | MIIM_DATA;
        mi.dwTypeData = it->label;
        mi.cch = ARRAYSIZE(it->label);
        it->label[0] = 0;
        if (!GetMenuItemInfoW(menu, (UINT)i, TRUE, &mi))
            continue;
        it->id = mi.wID;
        it->type = mi.fType;
        it->state = mi.fState;
        it->kind = mi.dwItemData;
        it->sub = mi.hSubMenu;
        for (it->len = 0; it->label[it->len] && it->label[it->len] != L'\n'; it->len++)
            ;
        l->n++;
    }
    for (int i = 0; i < l->n; i++)
        if (!(l->item[i].type & MFT_SEPARATOR)) {
            /* the label, 8 either side, the accessory with its 8 of margin, the scroller's padding */
            const menu_item_t *it = &l->item[i];
            int tw = r_text_width(g_ui.f_menu, it->label, it->len), help = 0;
            if (it->label[it->len])
                help = r_text_width(g_ui.f_small, it->label + it->len + 1, -1);
            tw = (tw > help ? tw : help) + S(16) + S(28) + 2 * S(MENU_PAD) + (it->kind >= MENU_STATUS ? S(20) : 0);
            if (tw > w)
                w = tw;
        }
    l->w = w < S(MENU_MAX_W) ? w : S(MENU_MAX_W);
    l->h = menu_item_y(l, l->n) + S(MENU_PAD);
}

/*
 * Shows level `d` at (x, y) on the screen, kept inside the app's window as
 * Discord keeps it in its viewport: past the right edge it opens to the left
 * of `flip_x` instead, past the bottom it goes up.
 */
static void menu_show(int d, int x, int y, int flip_x, int flip_y)
{
    menu_level_t *l = &g_menu.lv[d];
    RECT b;
    int corner = 2; /* DWMWCP_ROUND: 8px, as Discord's */
    COLORREF border = RGB(0x2D, 0x2D, 0x30);

    GetWindowRect(g_ui.top, &b);
    InflateRect(&b, -S(8), -S(8));
    if (x + l->w > b.right)
        x = flip_x - l->w >= b.left ? flip_x - l->w : b.right - l->w;
    if (x < b.left)
        x = b.left;
    if (y + l->h > b.bottom)
        y = flip_y - l->h >= b.top ? flip_y - l->h : b.bottom - l->h;
    if (y < b.top)
        y = b.top;
    l->wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"SilicordMenu", L"", WS_POPUP, x, y, l->w, l->h, g_ui.top,
                             NULL, NULL, NULL);
    DwmSetWindowAttribute(l->wnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof corner);
    DwmSetWindowAttribute(l->wnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof border);
    g_menu.depth = d + 1;
    ShowWindow(l->wnd, SW_SHOWNOACTIVATE);
    UpdateWindow(l->wnd);
}

/* Closes the levels from `d` on. */
static void menu_close_from(int d)
{
    for (int i = g_menu.depth - 1; i >= d; i--) {
        if (i > 0) /* the first holds the capture until the end */
            DestroyWindow(g_menu.lv[i].wnd);
        g_menu.lv[i].wnd = i ? NULL : g_menu.lv[i].wnd;
    }
    if (d < g_menu.depth)
        g_menu.depth = d;
    if (d > 0 && g_menu.lv[d - 1].open >= 0) {
        g_menu.lv[d - 1].open = -1;
        InvalidateRect(g_menu.lv[d - 1].wnd, NULL, FALSE);
    }
}

static void menu_hover(int d, int k)
{
    menu_level_t *l = &g_menu.lv[d];

    if (k >= 0 && !menu_selectable(&l->item[k]))
        k = -1;
    if (l->hover != k) {
        l->hover = k;
        InvalidateRect(l->wnd, NULL, FALSE);
    }
}

/* Opens the submenu of item `k` of level `d`, beside it. */
static void menu_open_sub(int d, int k)
{
    menu_level_t *l = &g_menu.lv[d];
    RECT r;

    if (l->open == k && g_menu.depth > d + 1)
        return;
    menu_close_from(d + 1);
    if (d + 1 >= MENU_LEVELS || !l->item[k].sub)
        return;
    menu_load(&g_menu.lv[d + 1], l->item[k].sub);
    GetWindowRect(l->wnd, &r);
    l->open = k;
    menu_show(d + 1, r.right + S(4), r.top + menu_item_y(l, k) - S(MENU_PAD), r.left - S(4),
              r.top + menu_item_y(l, k) + S(MENU_ITEM_H) + S(MENU_PAD));
}

/* The next selectable item from `k` going `dir`, round the ends; `k` itself if none. */
static int menu_step(const menu_level_t *l, int k, int dir)
{
    for (int i = 1; i <= l->n; i++) {
        int j = ((k < 0 ? (dir > 0 ? -1 : 0) : k) + dir * i + l->n * 2) % l->n;
        if (menu_selectable(&l->item[j]))
            return j;
    }
    return k;
}

static void menu_paint(RECT rc)
{
    const menu_level_t *l = NULL;

    for (int d = 0; d < g_menu.depth; d++)
        if (g_menu.lv[d].wnd == g_ui.paint_wnd)
            l = &g_menu.lv[d];
    r_fill(0, 0, rc.right, rc.bottom, 0xFF121214u); /* background-surface-higher */
    if (!l)
        return;
    for (int k = 0; k < l->n; k++) {
        const menu_item_t *it = &l->item[k];
        const wchar_t *glyph;
        int y = menu_item_y(l, k), x = S(MENU_PAD), w = l->w - 2 * S(MENU_PAD), ax = x + w - S(8) - S(20), ay;
        int danger = menu_danger(it->label), off = (it->state & MFS_GRAYED) != 0;
        int hot = (k == l->hover || k == l->open) && !off, checked = (it->state & MFS_CHECKED) != 0;
        unsigned bg = hot ? (danger ? 0xFF211417u : 0xFF222225u) : 0xFF121214u;
        unsigned ink = off ? 0xFF77777Au : danger ? 0xFFEB5F5Eu : 0xFFDCDCDFu;
        unsigned icon = off ? 0xFF55565Au : danger ? 0xFFEB5F5Eu : hot ? 0xFFFBFBFBu : 0xFF96979Eu;

        if (it->type & MFT_SEPARATOR) {
            r_fill(x + S(8), y + S(8), w - S(16), S(1) > 1 ? S(1) : 1, 0xFF2D2D30u); /* border-subtle */
            continue;
        }
        if (hot)
            r_round(x, y, w, menu_item_h(it), S(4), bg);
        {
            int lx = x + S(8);
            if (it->kind >= MENU_STATUS) { /* the status's dot, then its name */
                status_dot(lx, y + (S(MENU_ITEM_H) - S(12)) / 2, S(12), (int)(it->kind - MENU_STATUS), bg);
                lx += S(20);
            }
            r_text(g_ui.f_menu, ink, lx, y, x + w - S(8) - S(28) - lx, S(MENU_ITEM_H), it->label, it->len,
                   R_VCENTER | R_SINGLE | R_ELLIPSIS);
            if (it->label[it->len])
                r_text(g_ui.f_small, hot ? 0xFFB5B6BCu : ARGB(C_MUTED), lx, y + S(26), x + w - S(8) - lx, S(18),
                       it->label + it->len + 1, -1, R_SINGLE | R_ELLIPSIS);
        }
        ay = y + (S(MENU_ITEM_H) - S(20)) / 2;
        if (it->sub) {
            r_text(g_ui.f_icon, hot ? 0xFFFBFBFBu : 0xFF81828Au, ax, ay, S(20), S(20), L"\xE76C", -1, R_CENTER | R_VCENTER | R_SINGLE);
        } else if (it->kind == MENU_RADIO) {
            /* a ring, filled with the brand's color and a white dot when chosen */
            if (checked) {
                r_circle(ax + S(1), ay + S(1), S(18), ARGB(C_BRAND));
                r_circle(ax + S(6), ay + S(6), S(8), 0xFFFFFFFFu);
            } else {
                r_circle(ax + S(1), ay + S(1), S(18), icon);
                r_circle(ax + S(3), ay + S(3), S(14), bg);
            }
        } else if (it->kind == MENU_CHECK || checked) {
            if (checked) {
                r_round(ax + S(1), ay + S(1), S(18), S(18), S(4), ARGB(C_BRAND));
                r_text(g_ui.f_icon, 0xFFFFFFFFu, ax, ay, S(20), S(20), L"\xE73E", -1, R_CENTER | R_VCENTER | R_SINGLE);
            } else {
                r_round_outline(ax + S(1), ay + S(1), S(18), S(18), S(4), S(1) + 1, icon);
            }
        } else if (menu_is_id(it->label)) {
            r_round_outline(ax, ay + S(3), S(20), S(14), S(3), S(1) + 1, icon);
            r_text(g_ui.f_gif, icon, ax, ay + S(3), S(20), S(14), L"ID", -1, R_CENTER | R_VCENTER | R_SINGLE);
        } else if ((glyph = menu_icon(it->label)) != NULL) {
            r_text(g_ui.f_icon_tb, icon, ax, ay, S(20), S(20), glyph, -1, R_CENTER | R_VCENTER | R_SINGLE);
        }
    }
}

static LRESULT CALLBACK menu_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_frame(wnd, menu_paint);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_CAPTURECHANGED:
        if (g_menu.depth && wnd == g_menu.lv[0].wnd)
            g_menu.lost = 1;
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

/* A key while the menu is open; returns 1 when it closes it (`*result` the command chosen, or 0). */
static int menu_key(WPARAM key, UINT *result)
{
    int d = g_menu.depth - 1;
    menu_level_t *l = &g_menu.lv[d];

    switch (key) {
    case VK_ESCAPE:
        if (d == 0)
            return 1;
        menu_close_from(d);
        return 0;
    case VK_LEFT:
        if (d > 0)
            menu_close_from(d);
        return 0;
    case VK_UP:
    case VK_DOWN:
        menu_hover(d, menu_step(l, l->hover, key == VK_DOWN ? 1 : -1));
        return 0;
    case VK_RIGHT:
    case VK_RETURN:
    case VK_SPACE:
        if (l->hover < 0)
            return 0;
        if (l->item[l->hover].sub) {
            menu_open_sub(d, l->hover);
            if (g_menu.depth > d + 1)
                menu_hover(d + 1, menu_step(&g_menu.lv[d + 1], -1, 1));
            return 0;
        }
        if (key == VK_RIGHT)
            return 0;
        *result = l->item[l->hover].id;
        return 1;
    }
    return 0;
}

/*
 * Shows `menu` with its top left at (x, y) on the screen until an entry is
 * chosen, returning its command, or the menu is dismissed, returning 0. The
 * caller still owns (and destroys) `menu`.
 */
static int menu_track(HMENU menu, int x, int y)
{
    MSG m;
    UINT result = 0;
    int done = 0, pressed = 0;

    if (g_menu.depth)
        return 0;
    menu_load(&g_menu.lv[0], menu);
    if (!g_menu.lv[0].n)
        return 0;
    g_menu.lost = 0;
    menu_show(0, x, y, x, y);
    SetCapture(g_menu.lv[0].wnd);
    while (!done && !g_menu.lost) {
        if (!GetMessageW(&m, NULL, 0, 0)) {
            PostQuitMessage((int)m.wParam);
            break;
        }
        switch (m.message) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONUP: {
            POINT pt = {GET_X_LPARAM(m.lParam), GET_Y_LPARAM(m.lParam)};
            int d = -1, k = -1;
            ClientToScreen(m.hwnd, &pt);
            for (int i = g_menu.depth - 1; i >= 0 && d < 0; i--) {
                RECT r;
                GetWindowRect(g_menu.lv[i].wnd, &r);
                if (PtInRect(&r, pt)) {
                    POINT c = pt;
                    ScreenToClient(g_menu.lv[i].wnd, &c);
                    d = i;
                    k = menu_item_at(&g_menu.lv[i], c.x, c.y);
                }
            }
            if (m.message == WM_MOUSEMOVE) {
                if (d < 0) {
                    /* off the menus: the deepest loses its focus, unless it has a submenu open */
                    if (g_menu.lv[g_menu.depth - 1].open < 0)
                        menu_hover(g_menu.depth - 1, -1);
                } else if (k >= 0) {
                    menu_hover(d, k);
                    if (g_menu.lv[d].item[k].sub && menu_selectable(&g_menu.lv[d].item[k]))
                        menu_open_sub(d, k);
                    else
                        menu_close_from(d + 1);
                }
            } else if (m.message == WM_LBUTTONDOWN || m.message == WM_RBUTTONDOWN || m.message == WM_MBUTTONDOWN) {
                if (d < 0)
                    done = 1; /* a click away dismisses it */
                else
                    pressed = 1;
            } else if (pressed && d >= 0 && k >= 0 && menu_selectable(&g_menu.lv[d].item[k])) {
                /* the button up after one down on the menu: the button that opened it does not count */
                if (g_menu.lv[d].item[k].sub) {
                    menu_open_sub(d, k);
                } else {
                    result = g_menu.lv[d].item[k].id;
                    done = 1;
                }
            }
            continue;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (m.wParam == VK_MENU || m.wParam == VK_F10 || menu_key(m.wParam, &result))
                done = 1;
            continue;
        case WM_KEYUP:
        case WM_SYSKEYUP:
        case WM_CHAR:
        case WM_SYSCHAR:
        case WM_MOUSEWHEEL:
            continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    menu_close_from(1);
    g_menu.depth = 0;
    ReleaseCapture();
    DestroyWindow(g_menu.lv[0].wnd);
    g_menu.lv[0].wnd = NULL;
    return (int)result;
}

/* The popups are painted separately: what each shows was drawn at its last paint, and stays in memory. */
static unsigned oldest_shown_frame(void)
{
    unsigned keep = g_ui.frame;

    if (g_ui.pop && (int)(g_ui.pop_frame - keep) < 0)
        keep = g_ui.pop_frame;
    if (g_ui.picker && (int)(g_ui.picker_frame - keep) < 0)
        keep = g_ui.picker_frame;
    if (g_ui.qs && (int)(g_ui.qs_frame - keep) < 0)
        keep = g_ui.qs_frame;
    return keep;
}

static void paint_view(RECT rc)
{
    if (g_ui.view == VIEW_APP && g_ui.settings_open)
        paint_settings(rc);
    else if (g_ui.view == VIEW_APP)
        paint_app(rc);
    else if (g_ui.view == VIEW_LOADING)
        paint_loading(rc);
    else
        paint_login(rc);
}

static void paint(HWND wnd)
{
    paint_frame(wnd, paint_view);
    images_trim(oldest_shown_frame());
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

static int CALLBACK font_found(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM found)
{
    (void)lf;
    (void)tm;
    (void)type;
    *(int *)found = 1;
    return 0;
}

/* Windows 11's icon font when installed (Discord-like glyphs), else Windows 10's. */
static const wchar_t *icon_family(void)
{
    static int checked, fluent;

    if (!checked) {
        LOGFONTW lf = {0};
        HDC dc = GetDC(NULL);
        lstrcpyW(lf.lfFaceName, L"Segoe Fluent Icons");
        lf.lfCharSet = DEFAULT_CHARSET;
        EnumFontFamiliesExW(dc, &lf, font_found, (LPARAM)&fluent, 0);
        ReleaseDC(NULL, dc);
        checked = 1;
    }
    return fluent ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
}

static void make_fonts(void)
{
    r_font_t **f[] = {&g_ui.f_title, &g_ui.f_h, &g_ui.f_body, &g_ui.f_small, &g_ui.f_cat, &g_ui.f_icon,
                      &g_ui.f_icon_big, &g_ui.f_initial, &g_ui.f_initial_small, &g_ui.f_mono, &g_ui.f_h1,
                      &g_ui.f_h2, &g_ui.f_h3, &g_ui.f_name, &g_ui.f_emoji, &g_ui.f_icon_mid};

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
    g_ui.f_icon = r_font(icon_family(), S(14), FW_NORMAL, 0);
    g_ui.f_icon_big = r_font(icon_family(), S(32), FW_NORMAL, 0);
    g_ui.f_initial = r_font(L"Segoe UI", S(17), FW_SEMIBOLD, 0);
    g_ui.f_initial_small = r_font(L"Segoe UI", S(13), FW_SEMIBOLD, 0);
    g_ui.f_mono = r_font(L"Consolas", S(14), FW_NORMAL, 0);
    g_ui.f_h1 = r_font(L"Segoe UI", S(24), FW_BOLD, 0);
    g_ui.f_h2 = r_font(L"Segoe UI", S(20), FW_BOLD, 0);
    g_ui.f_h3 = r_font(L"Segoe UI", S(17), FW_BOLD, 0);
    g_ui.f_name = r_font(L"Segoe UI", S(20), FW_BOLD, 0);
    g_ui.f_emoji = r_font(L"Segoe UI Emoji", S(24), FW_NORMAL, 0);
    g_ui.f_icon_mid = r_font(icon_family(), S(20), FW_NORMAL, 0);
    g_ui.f_caption = r_font(icon_family(), S(10), FW_NORMAL, 0);
    g_ui.f_icon_tb = r_font(icon_family(), S(16), FW_NORMAL, 0);
    g_ui.f_tb = r_font(L"Segoe UI", S(14), FW_SEMIBOLD, 0);
    g_ui.f_nav = r_font(L"Segoe UI", S(16), FW_SEMIBOLD, 0);
    g_ui.f_section = r_font(L"Segoe UI", S(14), FW_NORMAL, 0);
    g_ui.f_small_mid = r_font(L"Segoe UI", S(14), FW_SEMIBOLD, 0);
    g_ui.f_menu = r_font(L"Segoe UI", S(14), FW_MEDIUM, 0);
    g_ui.f_chan = r_font(L"Segoe UI", S(16), FW_MEDIUM, 0);
    g_ui.f_welcome = r_font(L"Segoe UI", S(32), FW_BOLD, 0);
    g_ui.f_gif = r_font(L"Segoe UI", S(10), FW_BOLD, 0);
    /* Discord's marketing headings: heavy italic capitals (Segoe UI Black stands in for its own face). */
    g_ui.f_nitro = r_font(L"Segoe UI", S(64), FW_BLACK, 1);
    g_ui.f_nitro_h = r_font(L"Segoe UI", S(48), FW_BLACK, 1);
    g_ui.f_nitro_card = r_font(L"Segoe UI", S(32), FW_BOLD, 0);
    g_ui.f_h1x = r_font(L"Segoe UI", S(24), FW_EXTRABOLD, 0); /* Discord's heading-xxl/extrabold */
    build_name_fonts();

    g_ui.rich = (r_rich_style_t){
        .body = g_ui.f_body, .mono = g_ui.f_mono, .h1 = g_ui.f_h1, .h2 = g_ui.f_h2, .h3 = g_ui.f_h3,
        .subtext = g_ui.f_small,
        .ink = ARGB(C_TEXT), .muted = ARGB(C_MUTED), .link = 0xFF2781E7u, .mention = 0xFF8CA0FDu, /* text-link, mention-foreground */
        .mention_bg = 0x3D5865F2u, .code_bg = 0xFF0A0A0Cu, .quote_bar = 0xFF424246u, .spoiler = 0xFF17181Bu,
        .quote_indent = S(16), .code_pad = S(8), .block_gap = S(4), .radius = S(6),
        .emoji_px = S(22), .jumbo_px = S(48), .emoji = emoji_image,
    };

    /* The composer is a real EDIT control: it keeps a GDI font. */
    if (g_ui.composer) {
        HFONT font = CreateFontW(-S(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        SendMessageW(g_ui.composer, WM_SETFONT, (WPARAM)font, TRUE);
        if (old)
            DeleteObject(old);
    }
}

/* The executable's icon (src/silicord.rc) at the size Windows uses for `metric` at `dpi`. */
static HICON load_icon(int metric, UINT dpi)
{
    int px = GetSystemMetricsForDpi(metric, dpi);

    return (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, px, px, LR_DEFAULTCOLOR);
}

/* Sharp icons for the window's monitor: the title bar, the taskbar and the tray. */
static void set_icons(UINT dpi)
{
    HICON big = load_icon(SM_CXICON, dpi), sm = load_icon(SM_CXSMICON, dpi);

    if (!big || !sm) {
        if (big)
            DestroyIcon(big);
        if (sm)
            DestroyIcon(sm);
        return;
    }
    SendMessageW(g_ui.top, WM_SETICON, ICON_BIG, (LPARAM)big);
    SendMessageW(g_ui.top, WM_SETICON, ICON_SMALL, (LPARAM)sm);
    g_ui.tray.hIcon = sm;
    g_ui.tray.hBalloonIcon = big;
    if (g_ui.tray.hWnd) {
        g_ui.tray.uFlags = NIF_ICON; /* not NIF_INFO: that would show the last notification again */
        Shell_NotifyIconW(NIM_MODIFY, &g_ui.tray);
    }
    if (g_ui.icon_big)
        DestroyIcon(g_ui.icon_big);
    if (g_ui.icon_small)
        DestroyIcon(g_ui.icon_small);
    g_ui.icon_big = big;
    g_ui.icon_small = sm;
}

/* ---- Read markers and notifications ---- */

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
    if (!g_ui.pref_notify)
        return;
    const channel_t *c = chan(i);
    int g = model_channel_guild(g_ui.model, (unsigned)i);
    const char *author = a->author.data ? a->author.data : "";
    char title[200];
    FLASHWINFO fw = {sizeof fw, g_ui.top, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0};

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
            int dm = is_dm_type(c->type), g = model_channel_guild(g_ui.model, (unsigned)i), level;
            const guild_t *gd = g >= 0 ? &g_ui.model->guilds[g] : NULL;
            if (a->everyone && gd && gd->suppress_everyone)
                a->everyone = 0;
            if (!a->mentions_me && a->mention_roles.len && !(gd && gd->suppress_roles)) {
                /* @Role pings count when we have that role. */
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
            a->mentions_me |= a->everyone;
            if (a->mentions_me || dm)
                c->mentions++;
            /* Like Discord: pings get through a mute, other messages follow the notification level. */
            level = model_notify(g_ui.model, (unsigned)i);
            if (level != NOTIFY_NOTHING &&
                (a->mentions_me || (level == NOTIFY_ALL && !channel_muted((unsigned)i))))
                notify(i, a);
        }
    }
    update_title();
}

static void show_window(void)
{
    if (IsIconic(g_ui.top))
        ShowWindow(g_ui.top, SW_RESTORE);
    SetForegroundWindow(g_ui.top);
}

/* ---- Selection ---- */

static void place_composer(void)
{
    RECT rc;
    int x0 = S(RAIL_W + SIDE_W), show = g_ui.view == VIEW_APP && open_is_text() && !g_ui.msgs_status;

    GetClientRect(g_ui.wnd, &rc);
    if (show) {
        int cy = rc.bottom - S(COMPOSER_GAP) - S(COMPOSER_H);
        int eh = S(24);
        MoveWindow(g_ui.composer, x0 + S(COMPOSER_GAP) + S(56), cy + (S(COMPOSER_H) - eh) / 2,
                   main_right() - x0 - 2 * S(COMPOSER_GAP) - S(56) - (CB_COUNT - 1) * S(40) - S(16), eh, TRUE);
    }
    ShowWindow(g_ui.composer, show ? SW_SHOWNA : SW_HIDE);
}

static void hist_push(const char *id)
{
    if (g_ui.hist_nav || (g_ui.hist_n && lstrcmpA(g_ui.hist[g_ui.hist_pos], id) == 0))
        return;
    if (g_ui.hist_n) /* forward is lost, as in a browser */
        g_ui.hist_n = g_ui.hist_pos + 1;
    if (g_ui.hist_n == ARRAYSIZE(g_ui.hist)) {
        for (int i = 1; i < g_ui.hist_n; i++)
            lstrcpyA(g_ui.hist[i - 1], g_ui.hist[i]);
        g_ui.hist_n--;
    }
    lstrcpynA(g_ui.hist[g_ui.hist_n], id, sizeof g_ui.hist[0]);
    g_ui.hist_pos = g_ui.hist_n++;
    if (g_ui.top)
        InvalidateRect(g_ui.top, NULL, FALSE);
}

static void open_channel(int index)
{
    const channel_t *c;
    wchar_t *hint;
    char text[160];

    if (g_ui.model)
        hist_push(index >= 0 ? chan(index)->id : "");
    pop_close();
    g_ui.channel = index;
    g_ui.friend_hover = -1;
    qs_close();
    pins_close();
    picker_close();
    uploads_clear();
    g_ui.ac_kind = AC_NONE;
    g_ui.nmention = 0;
    typing_clear();
    g_ui.bar = BAR_NONE;
    g_ui.confirm = 0;
    free_messages();
    g_ui.msg_scroll = 0;
    g_ui.msgs_status = 0;
    g_ui.msgs_older_loading = 0;
    g_ui.hover_msg = -1;
    sb_clear(&g_ui.send_error);
    g_ui.msgs_channel[0] = 0;
    g_ui.detached = 0;
    posts_clear();
    if (index >= 0 && g_ui.model && (chan(index)->type == CH_FORUM || chan(index)->type == CH_MEDIA)) {
        lstrcpynA(g_ui.msgs_channel, chan(index)->id, sizeof g_ui.msgs_channel);
        g_ui.msgs_loading = 0;
        app_open_channel("");
        app_fetch_forum(chan(index)->id);
        place_composer();
        place_friend_input();
        place_search();
        return;
    }
    if (index < 0 || !g_ui.model || is_voice_type(chan(index)->type)) {
        g_ui.msgs_loading = 0;
        app_open_channel("");
        place_composer();
        place_friend_input();
        place_search();
        return;
    }
    place_friend_input();
    place_search();
    c = chan(index);
    lstrcpynA(g_ui.new_after, model_unread(g_ui.model, (unsigned)index) ? c->read : "", sizeof g_ui.new_after);
    mark_read(index);
    lstrcpynA(g_ui.msgs_channel, c->id, sizeof g_ui.msgs_channel);
    if (g_ui.guild >= 0)
        app_subscribe(g_ui.model->guilds[g_ui.guild].id, c->id);
    g_ui.msgs_loading = 1;
    g_ui.msgs_has_more = 1;
    app_open_channel(c->id);
    app_fetch_messages(c->id, NULL);

    wsprintfA(text, c->type == CH_DM ? "Message @%.120s" : c->type == CH_GROUP_DM ? "Message %.120s" : "Message #%.120s",
              model_str(g_ui.model, c->name));
    hint = utf8_to_wide(text, lstrlenA(text));
    SendMessageW(g_ui.composer, EM_SETCUEBANNER, TRUE, (LPARAM)hint);
    mem_free(hint);
    SetWindowTextW(g_ui.composer, L"");
    place_composer();
    dm_profile_sync();
}


static void select_guild(int i)
{
    if (!g_ui.model)
        return;
    if (i != g_ui.guild) {
        ml_free(&g_ui.ml);
        g_ui.ml_scroll = 0;
        g_ui.ml_chunk = 0;
        g_ui.ml_hover = -1;
    }
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

/* The Shop's answer: kept whole, parsed while painting. */
static int shop_parse(const sb_t *json);
static int quests_parse(const sb_t *json);

static void on_quests(sb_t *p)
{
    int n = quests_parse(p);

    sb_free(p);
    g_ui.quests_state = n >= 0 ? 2 : 3;
    redraw();
}

static void on_shop(sb_t *p)
{
    int n = shop_parse(p);

    sb_free(p);
    g_ui.shop_state = n ? 2 : 3;
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
        if (is_voice_type(chan(index)->type) && g_ui.guild >= 0 &&
            (g_ui.voice_state == VOICE_OFF || g_ui.voice_state == VOICE_FAILED ||
             lstrcmpA(g_ui.voice_channel, chan(index)->id) != 0)) {
            voice_join(g_ui.model->guilds[g_ui.guild].id, chan(index)->id, model_str(g_ui.model, chan(index)->name));
        }
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
        if (g_ui.model)
            settings_open();
        else
            app_logout();
        break;
    case HIT_SELF:
        open_self();
        break;
    case HIT_VOICE_CAMERA:
        g_ui.voice_camera = app_video_camera(!g_ui.voice_camera);
        redraw();
        break;
    case HIT_VOICE_SHARE:
        /* Sharing starts from the screen picker; the button stops it. */
        if (g_ui.share_state == VOICE_OFF) {
            share_pick_open();
        } else {
            app_screen_share(0, -1, 0);
            stream_refresh();
        }
        break;
    case HIT_VOICE_MENU:
        settings_open_voice();
        break;
    case HIT_MIC:
    case HIT_VOICE_MUTE:
        g_ui.voice_muted = !(g_ui.voice_muted || g_ui.voice_deafened);
        if (!g_ui.voice_muted)
            g_ui.voice_deafened = 0;
        app_voice_set(g_ui.voice_muted, g_ui.voice_deafened);
        redraw();
        break;
    case HIT_DEAFEN:
    case HIT_VOICE_DEAF:
        g_ui.voice_deafened = !g_ui.voice_deafened;
        app_voice_set(g_ui.voice_muted, g_ui.voice_deafened);
        redraw();
        break;
    case HIT_VOICE_LEAVE:
        voice_leave();
        break;
    case HIT_FOLDER:
        if (index >= 0 && index < (int)sizeof g_ui.folder_open)
            g_ui.folder_open[index] ^= 1;
        clamp_scroll();
        redraw();
        break;
    case HIT_FRIENDS:
        index = HOME_FRIENDS;
        /* fall through */
    case HIT_NAV:
        g_ui.home_page = index;
        InvalidateRect(g_ui.top, NULL, FALSE); /* the title bar names the page */
        if (index == HOME_SHOP && (g_ui.shop_state == 0 || g_ui.shop_state == 3)) {
            g_ui.shop_state = 1;
            app_fetch_shop();
        }
        if (index == HOME_QUESTS && g_ui.quests_state != 1) {
            g_ui.quests_state = 1; /* asked again each time: progress moves */
            app_fetch_quests();
        }
        g_ui.last_dm = -1;
        open_channel(-1);
        redraw();
        break;
    case HIT_DM_SEARCH:
    case HIT_NEW_DM:
        qs_open();
        break;
    case HIT_GUILD_HEADER: {
        /* the server's menu, under the header as Discord's dropdown */
        POINT pt = {S(RAIL_W) + S(8), S(44)};
        ClientToScreen(g_ui.wnd, &pt);
        g_menu_at = pt;
        g_menu_at_set = 1;
        if (g_ui.guild >= 0)
            guild_menu(g_ui.guild);
        g_menu_at_set = 0;
        break;
    }
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
    /* Set first so switching servers opens i, not the channel last seen there (which it would mark read). */
    if (g >= 0)
        g_ui.last_channel[g] = i;
    else
        g_ui.last_dm = i;
    if (g != g_ui.guild)
        select_guild(g);
    if (g_ui.channel != i)
        open_channel(i);
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
    /* The quick switcher and the picker hold indices into the old model. */
    if (g_ui.qs) {
        int sel = g_ui.qs_sel;
        qs_rebuild();
        g_ui.qs_sel = sel < g_ui.nqs ? sel : 0;
        qs_place();
    }
    if (g_ui.picker && g_ui.picker_tab != TAB_GIFS) {
        int scroll = g_ui.pick_scroll, max;
        picker_rebuild();
        max = g_ui.pick_content - (S(PICK_H) - S(PICK_FOOT) - S(PICK_TOP));
        g_ui.pick_scroll = scroll < max ? scroll : max > 0 ? max : 0;
        InvalidateRect(g_ui.picker, NULL, FALSE);
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

    if (lstrcmpA(name, "REACTORS") == 0) { /* the payload is "key", NUL, users */
        const char *key = p->data + n;
        size_t kn = (size_t)lstrlenA(key) + 1;
        json_t users;
        if (n + kn < p->len && json_parse(p->data + n + kn, p->len - n - kn, &users))
            on_reactors(key, users);
        return;
    }
    if (!g_ui.model || n >= p->len || !json_parse(p->data + n, p->len - n, &d))
        return;
    if (lstrcmpA(name, "GUILD_MEMBER_LIST_UPDATE") == 0) {
        on_member_list(d);
        return;
    }
    if (lstrcmpA(name, "THREAD_OURS") == 0) { /* a thread or post we just started: open it, as Discord does */
        json_t v;
        char id[24] = "";
        int c;
        if (json_get(d, "id", &v))
            json_raw(v, id, sizeof id);
        if (model_find_channel(g_ui.model, id) < 0 && (m = model_apply(g_ui.model, "THREAD_CREATE", d)) != NULL)
            replace_model(m);
        if ((c = model_find_channel(g_ui.model, id)) >= 0)
            go_to_channel(c);
        return;
    }
    if (lstrcmpA(name, "CALL_CREATE") == 0 || lstrcmpA(name, "CALL_UPDATE") == 0) {
        call_store(d, name[5] == 'C');
        clamp_scroll();
        redraw();
        return;
    }
    if (lstrcmpA(name, "CALL_DELETE") == 0) {
        json_t v;
        char channel[24] = "";
        if (json_get(d, "channel_id", &v))
            json_raw(v, channel, sizeof channel);
        call_delete(channel);
        clamp_scroll();
        redraw();
        return;
    }
    if (lstrcmpA(name, "VOICE_STATES") == 0 || lstrcmpA(name, "VOICE_STATE_UPDATE") == 0) {
        json_t v, list, item;
        json_iter_t it;
        char guild[24] = "";
        if (json_get(d, "guild_id", &v) && json_type(v) == JSON_STRING)
            json_raw(v, guild, sizeof guild);
        if (json_get(d, "voice_states", &list)) {
            json_iter(list, &it);
            while (json_next(&it, NULL, &item))
                voice_store(guild, item);
        } else {
            voice_store(guild, d);
        }
        clamp_scroll();
        redraw();
        return;
    }
    if (lstrcmpA(name, "RELATIONSHIPS") == 0) {
        json_iter_t it;
        json_t item;
        rels_clear();
        json_iter(d, &it);
        while (json_next(&it, NULL, &item))
            rel_store(item);
        return;
    }
    if (lstrcmpA(name, "RELATIONSHIP_ADD") == 0 || lstrcmpA(name, "RELATIONSHIP_UPDATE") == 0) {
        rel_store(d);
        return;
    }
    if (lstrcmpA(name, "RELATIONSHIP_REMOVE") == 0) {
        json_t v;
        char id[24] = "";
        if (json_get(d, "id", &v))
            json_raw(v, id, sizeof id);
        rel_remove(id);
        return;
    }
    if (lstrcmpA(name, "PRESENCES") == 0 || lstrcmpA(name, "PRESENCE_UPDATE") == 0) {
        if (json_type(d) == JSON_ARRAY) {
            json_iter_t it;
            json_t item;
            json_iter(d, &it);
            while (json_next(&it, NULL, &item))
                presence_store(item);
        } else {
            presence_store(d);
        }
        if (lstrcmpA(name, "PRESENCES") == 0) {
            char line[128];
            int online = 0, friends = 0;
            for (int i = 0; i < g_ui.npresences; i++) {
                relation_t *r = rel_find(g_ui.presences[i].user);
                online += g_ui.presences[i].status != ML_OFFLINE;
                friends += r && r->type == 1 && g_ui.presences[i].status != ML_OFFLINE;
            }
            wsprintfA(line, "[presence] %d known, %d not offline, %d of them friends (%d relationships)", g_ui.npresences,
                      online, friends, g_ui.nrels);
            app_log(line);
        }
        return;
    }
    if (lstrcmpA(name, "GUILD_MEMBERS_CHUNK") == 0 || lstrcmpA(name, "GUILD_MEMBER_UPDATE") == 0) {
        json_t v, list, item;
        json_iter_t it;
        char guild[24] = "";
        if (json_get(d, "guild_id", &v))
            json_raw(v, guild, sizeof guild);
        if (json_get(d, "members", &list)) {
            json_iter(list, &it);
            while (json_next(&it, NULL, &item)) {
                json_t user, uid;
                member_store(guild, item);
                if (json_get(item, "user", &user) && json_get(user, "id", &uid)) { /* names for the voice list */
                    char id[24];
                    voice_t *vc;
                    json_raw(uid, id, sizeof id);
                    if ((vc = voice_find(guild, id)) != NULL)
                        voice_name(vc, item);
                }
            }
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
    presences_clear();
    rels_clear();
    ml_free(&g_ui.ml);
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
    if (msg == UI_VIDEO) {
        invalidate_video();
        return;
    }
    if (msg == UI_STREAM) {
        stream_refresh();
        return;
    }
    if (msg == UI_QUESTS) {
        if (p) {
            on_quests(p);
            mem_free(p);
        }
        return;
    }
    if (msg == UI_SHOP) {
        if (p) {
            on_shop(p);
            mem_free(p);
        }
        return;
    }
    if (msg == UI_GIFS) {
        if (p) {
            on_gifs(p);
            sb_free(p);
            mem_free(p);
        }
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
        if (g_ui.pop)
            pop_place(); /* which repaints it */
        return;
    }
    if (msg == UI_ACTIVITY) {
        on_activity((activity_t *)lp);
        activity_free((activity_t *)lp);
        redraw();
        return;
    }

    switch (msg) {
    case UI_COMMANDS:
        if (p)
            on_commands(p);
        break;
    case UI_FORUM:
        if (p)
            on_forum(p);
        break;
    case UI_TYPING:
        if (p)
            on_typing(p);
        break;
    case UI_VOICE:
        /* A late report from a connection we already left changes nothing. */
        if (p && p->len >= 2 && g_ui.voice_state != VOICE_OFF) {
            g_ui.voice_state = p->data[0] - '0';
            sb_clear(&g_ui.voice_status);
            sb_add(&g_ui.voice_status, p->data + 2);
            if (g_ui.voice_state == VOICE_OFF)
                g_ui.voice_channel[0] = 0;
            if (g_ui.voice_state == VOICE_CONNECTED && g_ui.camera_on_connect) {
                g_ui.camera_on_connect = 0;
                g_ui.voice_camera = app_video_camera(1);
            }
            if (g_ui.voice_state != VOICE_CONNECTING && g_ui.voice_state != VOICE_CONNECTED)
                g_ui.camera_on_connect = 0;
            if (g_ui.voice_state == VOICE_CONNECTED)
                SetTimer(g_ui.wnd, TIMER_VOICE, 100, NULL);
            else
                KillTimer(g_ui.wnd, TIMER_VOICE);
            clamp_scroll();
        }
        break;
    case UI_QR:
        if (!p)
            break;
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
        voices_clear(); /* READY brings them again, and the calls come as CALL_CREATE */
        g_ui.ncalls = 0;
        update_ringing();
        if (g_ui.model)
            replace_model((model_t *)lp); /* reconnected with a fresh session */
        else
            set_model((model_t *)lp);
        update_title();
        set_text(&g_ui.status, "Online");
        for (int k = 0; k < 4; k++) /* our status as the other clients have it */
            if (lstrcmpA(g_ui.model->status, k_status_codes[k]) == 0)
                g_ui.my_status = k_status_states[k];
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
    case UI_FRIEND_RESULT:
        set_text(&g_ui.friend_result, s);
        if (g_ui.friend_edit && s[0] == 'S')
            SetWindowTextW(g_ui.friend_edit, L"");
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
            if (im->wnd && im->wnd != g_ui.wnd && IsWindow(im->wnd))
                InvalidateRect(im->wnd, NULL, FALSE); /* the picker or another child waits for it */
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
    top += divider_h(m);
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

    if (open_discord_link(url))
        return;
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
    top += divider_h(m);
    top += m->grouped == 1 ? S(2) : S(16) + (m->reply.len ? S(22) : 0) + S(22);
    if (m->text.len || m->edited)
        top += r_rich_height(msg_rich(m, w));
    *part = (part_t){PART_NONE, -1};
    msg_extras(m, text_x(), top, w, 0, x, y, part);
    *msg = i;
    return part->kind != PART_NONE;
}

/* A menu of a select's options at the mouse; returns the chosen value (to free) or NULL. */
static char *pick_option(const sb_t *options)
{
    HMENU menu = CreatePopupMenu();
    const char *p = options->data, *end = options->data + options->len;
    POINT pt;
    int n = 0, chosen;
    char *value = NULL;

    while (p < end && n < 25) {
        const char *tab = p, *nl;
        wchar_t *label;
        while (tab < end && *tab != '\t')
            tab++;
        for (nl = tab; nl < end && *nl != '\n'; nl++)
            ;
        label = utf8_to_wide(p, (int)(tab - p));
        AppendMenuW(menu, MF_STRING, (UINT_PTR)(++n), label);
        mem_free(label);
        p = nl + 1;
    }
    GetCursorPos(&pt);
    chosen = menu_track(menu, pt.x, pt.y);
    DestroyMenu(menu);
    for (p = options->data, n = 1; chosen && p < end; n++) {
        const char *tab = p, *nl;
        while (tab < end && *tab != '\t')
            tab++;
        for (nl = tab; nl < end && *nl != '\n'; nl++)
            ;
        if (n == chosen && tab < end) {
            value = mem_alloc((size_t)(nl - tab));
            CopyMemory(value, tab + 1, (size_t)(nl - tab - 1));
            break;
        }
        p = nl + 1;
    }
    return value;
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
    case PART_POLL: {
        msg_poll_t *pl = m->poll;
        int ids[16], n = 0;
        if (!pl || pl->final)
            break;
        /* Single choice: this answer (or none when clicking our vote again); multi: toggle it. */
        for (int k = 0; k < pl->nanswers && n < 16; k++) {
            int on = k == p.index ? !pl->answers[k].me : pl->multi && pl->answers[k].me;
            if (pl->answers[k].me && !on)
                pl->answers[k].count--;
            if (!pl->answers[k].me && on)
                pl->answers[k].count++;
            pl->answers[k].me = on;
            if (on)
                ids[n++] = pl->answers[k].id;
        }
        app_vote(m->channel_id[0] ? m->channel_id : g_ui.msgs_channel, m->id, ids, n);
        break;
    }
    case PART_COMPONENT: {
        msg_component_t *c = &m->components[p.index];
        const char *channel = m->channel_id[0] ? m->channel_id : g_ui.msgs_channel;
        if (c->disabled)
            break;
        if (c->type == COMP_BUTTON && c->style == BUTTON_LINK) {
            open_url(c->url.data ? c->url.data : "");
        } else if (c->type == COMP_BUTTON && c->custom_id.len) {
            app_press_component(open_guild_id(), channel, m->id, m->app_id, m->flags, c->type, c->custom_id.data, NULL);
        } else if (c->type == COMP_STRING_SELECT && c->options.len) {
            /*
             * The menu runs a message loop: the bot may edit or delete the
             * message meanwhile, freeing `m` and `c`. Keep copies of what
             * the interaction needs, and send it only if the message is still there.
             */
            char id[24], app[24], chan_id[24];
            int flags = m->flags;
            sb_t options = {0}, custom = {0};
            char *value;
            lstrcpynA(id, m->id, sizeof id);
            lstrcpynA(app, m->app_id, sizeof app);
            lstrcpynA(chan_id, channel, sizeof chan_id);
            sb_addn(&options, c->options.data, c->options.len);
            sb_addn(&custom, c->custom_id.data ? c->custom_id.data : "", c->custom_id.len);
            value = pick_option(&options);
            if (value && find_msg(id) >= 0)
                app_press_component(open_guild_id(), chan_id, id, app, flags, COMP_STRING_SELECT, custom.data, value);
            mem_free(value);
            sb_free(&options);
            sb_free(&custom);
        }
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

/* Scrolls so message i is in view and flashes it, as when clicking a reply in Discord. */
static void jump_to(int i)
{
    RECT a = message_area();
    int below = 0;

    for (int k = g_ui.nmsgs - 1; k > i; k--)
        below += msg_height(&g_ui.msgs[k]);
    /* Put the message about a third from the top. */
    g_ui.msg_scroll = below + msg_height(&g_ui.msgs[i]) - (a.bottom - a.top) * 2 / 3;
    clamp_msg_scroll();
    lstrcpynA(g_ui.flash_id, g_ui.msgs[i].id, sizeof g_ui.flash_id);
    SetTimer(g_ui.wnd, TIMER_FLASH, 1500, NULL);
    redraw();
}

/* A click on the "replying to" line. */
static int click_reply(int x, int y)
{
    int top, i = message_at(x, y, &top), target;
    msg_t *m;

    if (i < 0)
        return 0;
    m = &g_ui.msgs[i];
    top += divider_h(m);
    if (!m->reply.len || !m->reply_id[0] || m->grouped == 1 || y < top + S(16) || y >= top + S(38) || x < text_x())
        return 0;
    target = find_msg(m->reply_id);
    if (target >= 0)
        jump_to(target);
    return 1;
}

static int click_message(int x, int y)
{
    int i, link;

    if (click_reply(x, y))
        return 1;
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
#define PROFILE_W 340 /* the profile beside a DM, as in Discord: the popout, docked */
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

/* Takes ownership of p and returns it. */
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
    char key[112], path[200]; /* ids of 23 and a hash of 47 fit */

    if (!p || !p->avatar[0])
        return user_avatar(g_ui.pop_user, g_ui.pop_avatar);
    wsprintfA(key, "A:%s:%s:%s", p->id, p->member_avatar ? p->guild_id : "", p->avatar);
    /* Popouts play animated avatars and banners, as Discord does. */
    if (p->member_avatar)
        wsprintfA(path, "/guilds/%s/users/%s/avatars/%s.%s?size=256", p->guild_id, p->id, p->avatar,
                  p->avatar[0] == 'a' && p->avatar[1] == '_' ? "gif" : "png");
    else
        wsprintfA(path, "/avatars/%s/%s.%s?size=256", p->id, p->avatar, p->avatar[0] == 'a' && p->avatar[1] == '_' ? "gif" : "png");
    return image_get(key, path, S(POP_AVATAR));
}

static r_image_t *pop_banner(const profile_t *p)
{
    char key[96], path[200];

    if (!p || !p->banner[0])
        return NULL;
    wsprintfA(key, "b:%s:%s", p->id, p->banner);
    if (p->member_banner)
        wsprintfA(path, "/guilds/%s/users/%s/banners/%s.%s?size=600", p->guild_id, p->id, p->banner,
                  p->banner[0] == 'a' && p->banner[1] == '_' ? "gif" : "png");
    else
        wsprintfA(path, "/banners/%s/%s.%s?size=600", p->id, p->banner, p->banner[0] == 'a' && p->banner[1] == '_' ? "gif" : "png");
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

static int pop_width(void)
{
    return g_ui.pop_docked ? S(PROFILE_W) : S(POP_W);
}

/* Walks the popout layout; paints when `draw` is set. Returns the height. */
static int pop_render(int draw)
{
    const profile_t *p = g_ui.pop_profile;
    int w = pop_width(), pad = S(POP_PAD), inner = w - 2 * pad, radius = g_ui.pop_docked ? 0 : S(POP_RADIUS);
    int bh = pop_banner_h(p), y, ax = pad, ay = bh - S(POP_AVATAR) / 2;
    unsigned body = 0xFF121214u, border = 0xFF1E1E20u; /* background-surface-higher, border-subtle */
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
            r_round_gradient(0, 0, w, g_ui.pop_h, radius, rgb_argb(p->theme[0], 0xFF), rgb_argb(p->theme[1], 0xFF));
            r_round(0, 0, w, g_ui.pop_h, radius, 0x99000000u);
        } else {
            r_round(0, 0, w, g_ui.pop_h, radius, body);
        }
        /* Banner: image, else theme or accent color, else the avatar's average color. */
        r_clip(0, 0, w, bh);
        if (banner) {
            r_image_cover(banner, 0, 0, w, bh + S(POP_RADIUS), radius);
        } else {
            if (p && p->ntheme == 2)
                bc = rgb_argb(p->theme[0], 0xFF);
            else if (p && p->has_accent)
                bc = rgb_argb(p->accent, 0xFF);
            else if (avatar)
                bc = r_image_average(avatar);
            r_round(0, 0, w, bh + S(POP_RADIUS), radius, bc ? bc : 0xFF242426);
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
            r_image_t *deco = cdn_image("ad", "/avatar-decoration-presets/%s.png?size=240&passthrough=true",
                                        p->decoration, NULL, S(POP_AVATAR) * 6 / 5);
            int d = S(POP_AVATAR) * 6 / 5;
            if (deco)
                r_image(deco, ax - (d - S(POP_AVATAR)) / 2, ay - (d - S(POP_AVATAR)) / 2, d, d, 0);
        }
        {
            int st = user_status(g_ui.pop_user);
            if (g_ui.pop_self && (g_ui.disconnected || g_ui.reconnecting))
                st = ML_OFFLINE;
            if (st != ML_UNKNOWN)
                paint_status(ax, ay, S(POP_AVATAR), st,
                             p && p->ntheme == 2 ? 0xFF000000u | mix_dark(p->theme[0]) : body);
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
    if (p && !g_ui.pop_self && !g_ui.pop_docked && (p->mutual_friends > 0 || p->mutual_guilds > 0)) {
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

    /* Roles in this server, as pills with their color. */
    if (p && p->roles.len && g_ui.guild >= 0 && lstrcmpA(p->guild_id, g_ui.model->guilds[g_ui.guild].id) == 0) {
        model_role_t r, list[32];
        unsigned cursor = 0;
        int rx = pad, first = 1, nr = 0;
        y += S(12);
        if (draw)
            text(g_ui.f_cat, C_INK, rect(pad, y, inner, S(18)), "Roles", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += S(22);
        while (nr < 32 && model_role_next(g_ui.model, g_ui.guild, &cursor, &r)) {
            int in_list = 0;
            const char *q = p->roles.data;
            size_t idn = (size_t)lstrlenA(r.id);
            while (*q && !in_list) {
                size_t k = 0;
                while (q[k] && q[k] != ',')
                    k++;
                in_list = k == idn && CompareStringA(LOCALE_INVARIANT, 0, q, (int)k, r.id, (int)k) == CSTR_EQUAL;
                q += k + (q[k] == ',');
            }
            if (in_list)
                list[nr++] = r;
        }
        /* Highest role first, like Discord. */
        for (int a = 1; a < nr; a++) {
            model_role_t x = list[a];
            int b = a;
            while (b > 0 && list[b - 1].position < x.position) {
                list[b] = list[b - 1];
                b--;
            }
            list[b] = x;
        }
        for (int ri = 0; ri < nr; ri++) {
            char rname[64];
            int pw;
            r = list[ri];
            lstrcpynA(rname, r.name, r.name_len + 1 < (int)sizeof rname ? r.name_len + 1 : (int)sizeof rname);
            pw = text_width(g_ui.f_small, rname) + S(30);
            if (pw > inner)
                pw = inner;
            if (rx + pw > pad + inner && !first) {
                rx = pad;
                y += S(28);
            }
            if (draw) {
                r_round(rx, y, pw, S(24), S(4), 0xFF17181Bu);
                r_circle(rx + S(8), y + S(7), S(10), r.color ? 0xFF000000u | r.color : 0xFF99AAB5u);
                text(g_ui.f_small, C_INK, rect(rx + S(22), y, pw - S(26), S(24)), rname, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            rx += pw + S(4);
            first = 0;
        }
        y += S(24);
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

    /* When the account was made, read from its id as Discord shows it: "Member Since". */
    if (p && g_ui.pop_user[0]) {
        unsigned long long id = 0;
        for (const char *c = g_ui.pop_user; *c >= '0' && *c <= '9'; c++)
            id = id * 10 + (unsigned long long)(*c - '0');
        y += S(16);
        if (draw && id) {
            unsigned long long t = ((id >> 22) + FILETIME_UNIX_MS + 1420070400000ull) * 10000ull;
            FILETIME ft = {(DWORD)t, (DWORD)(t >> 32)}, local;
            SYSTEMTIME st;
            wchar_t date[48];
            FileTimeToLocalFileTime(&ft, &local);
            FileTimeToSystemTime(&local, &st);
            if (!GetDateFormatEx(L"en-US", 0, &st, L"MMM d, yyyy", date, ARRAYSIZE(date), NULL))
                date[0] = 0;
            text(g_ui.f_cat, C_INK, rect(pad, y, inner, S(18)), "Member Since", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            text_w(g_ui.f_section, C_MUTED, rect(pad, y + S(20), inner, S(20)), date, -1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        y += S(40);
    }

    /* Beside a DM, mutual servers and friends are rows of their own, as in Discord's panel. */
    if (p && g_ui.pop_docked && (p->mutual_friends > 0 || p->mutual_guilds > 0)) {
        const char *labels[2] = {"Mutual Servers", "Mutual Friends"};
        int counts[2] = {p->mutual_guilds, p->mutual_friends}, rows = (counts[0] > 0) + (counts[1] > 0), r = 0;
        y += S(16);
        if (draw)
            r_round(pad, y, inner, rows * S(44), S(8), 0xFF17181Bu);
        for (int k = 0; k < 2; k++) {
            char line[64];
            if (counts[k] <= 0)
                continue;
            wsprintfA(line, "%s \xE2\x80\x94 %d", labels[k], counts[k]);
            if (draw) {
                if (r)
                    r_fill(pad + S(12), y, inner - S(24), S(1) > 1 ? S(1) : 1, 0xFF242426u);
                text(g_ui.f_section, C_INK, rect(pad + S(12), y, inner - S(48), S(44)), line, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                text_w(g_ui.f_icon, C_MUTED, rect(pad + inner - S(36), y, S(24), S(44)), ICON_CHEVRON_RIGHT, -1,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            y += S(44);
            r++;
        }
    }

    /*
     * Our account, as Discord's popout has it: two cards of rows, Edit Profile
     * and our status (its choices in a menu), then Copy User ID.
     */
    if (g_ui.pop_self) {
        static const char *const labels[3] = {"Edit Profile", NULL, "Copy User ID"};
        int cur = 0;
        for (int k = 0; k < 4; k++)
            if (g_ui.my_status == k_status_states[k])
                cur = k;
        y += S(12);
        for (int k = 0; k < 3; k++) {
            int top = k == 0 || k == 2, bottom = k == 1 || k == 2, hot = g_ui.pop_hover == -10 - k;
            unsigned card = 0xFF202024u, bg = hot ? 0xFF2A2A2Eu : card;
            if (k == 2)
                y += S(8); /* the second card */
            g_ui.pop_status_y[k] = y;
            if (draw) {
                /* the card's corners are round at its ends only */
                r_round(pad, y, inner, S(40), top || bottom ? S(8) : 0, bg);
                if (!top)
                    r_fill(pad, y, inner, S(8), bg);
                if (!bottom)
                    r_fill(pad, y + S(32), inner, S(8), bg);
                if (k == 1) {
                    status_dot(pad + S(12), y + S(14), S(12), k_status_states[cur], bg);
                    text(g_ui.f_menu, C_INK, rect(pad + S(32), y, inner - S(72), S(40)), k_status_names[cur],
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    sicon(SI_CHEVRON_RIGHT, pad + inner - S(12) - S(18), y + S(11), S(18), hot ? ARGB(C_INK) : ARGB(C_MUTED));
                } else {
                    text(g_ui.f_menu, C_INK, rect(pad + S(12), y, inner - S(52), S(40)), labels[k], DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    if (k == 0) {
                        r_text(g_ui.f_icon_tb, hot ? ARGB(C_INK) : ARGB(C_MUTED), pad + inner - S(12) - S(20), y, S(20), S(40),
                               L"\xE70F", -1, R_CENTER | R_VCENTER | R_SINGLE);
                    } else {
                        unsigned ink = hot ? ARGB(C_INK) : ARGB(C_MUTED);
                        r_round_outline(pad + inner - S(12) - S(20), y + S(13), S(20), S(14), S(3), S(1) + 1, ink);
                        r_text(g_ui.f_gif, ink, pad + inner - S(12) - S(20), y + S(13), S(20), S(14), L"ID", -1,
                               R_CENTER | R_VCENTER | R_SINGLE);
                    }
                }
                if (k == 0) /* between the rows of a card */
                    r_fill(pad + S(12), y + S(40) - (S(1) > 1 ? S(1) : 1), inner - S(24), S(1) > 1 ? S(1) : 1, 0xFF2A2A2Eu);
            }
            y += S(40);
        }
    }

    /* Message box; the EDIT control sits inside it. Not beside a DM: its composer is right there. */
    if (!g_ui.pop_self && !g_ui.pop_docked) {
        y += S(16);
        g_ui.pop_input_y = y;
        if (draw)
            r_round(pad, y, inner, S(POP_INPUT_H), S(8), g_ui.pop_input_color);
        y += S(POP_INPUT_H);
    }
    y += pad;

    if (draw) {
        if (g_ui.pop_docked)
            r_fill(0, 0, S(1) > 1 ? S(1) : 1, g_ui.pop_h, border);
        else
            r_round_outline(0, 0, w, g_ui.pop_h, radius, S(1) > 1 ? S(1) : 1, border);
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
    int w = pop_width(), h, x, y;

    if (!g_ui.pop)
        return;
    GetClientRect(g_ui.wnd, &rc);
    if (g_ui.pop_docked) {
        pop_render(0);
        g_ui.pop_h = rc.bottom - S(HEADER_H);
        SetWindowPos(g_ui.pop, NULL, rc.right - w, S(HEADER_H), w, g_ui.pop_h, SWP_NOACTIVATE | SWP_NOZORDER);
        InvalidateRect(g_ui.pop, NULL, FALSE);
        return;
    }
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
                   pop_width() - 2 * S(POP_PAD) - S(24), eh, TRUE);
    }
    InvalidateRect(g_ui.pop, NULL, FALSE);
}

static void pop_reset_bio(void)
{
    r_rich_free(g_ui.pop_rich);
    g_ui.pop_rich = NULL;
    md_free(&g_ui.pop_bio);
}

/* Closes a floating popout; the profile docked beside a DM stays. */
static void pop_dismiss(void)
{
    if (g_ui.pop && !g_ui.pop_docked)
        pop_close();
}

static void pop_close(void)
{
    HWND pop = g_ui.pop;

    if (!pop)
        return;
    g_ui.pop = NULL;
    g_ui.pop_docked = 0;
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

/* "Message @name" in the popout's empty message box. */
static void pop_cue(const char *name)
{
    char hint[96];
    wchar_t *w;

    wsprintfA(hint, "Message @%.80s", name ? name : "");
    w = utf8_to_wide(hint, lstrlenA(hint));
    SendMessageW(g_ui.pop_edit, EM_SETCUEBANNER, TRUE, (LPARAM)w);
    mem_free(w);
}

static void pop_set_profile(profile_t *p)
{
    unsigned c = 0xFF17181Bu; /* user-profile-overlay-background */

    pop_reset_bio();
    g_ui.pop_profile = p;
    if (p && p->ntheme == 2)
        c = 0xFF000000u | mix_dark(p->theme[1]);
    g_ui.pop_input_color = c;
    if (g_ui.pop_brush)
        DeleteObject(g_ui.pop_brush);
    g_ui.pop_brush = CreateSolidBrush(RGB(c >> 16 & 0xFF, c >> 8 & 0xFF, c & 0xFF));
    if (g_ui.pop_edit && p)
        pop_cue(p->name.data);
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

/* Opens our direct message with a user, and sends `text` there when not NULL. */
static void open_dm(const char *user_id, const char *text)
{
    int dm = dm_with(user_id);

    if (dm >= 0) {
        go_to_channel(dm);
        if (text)
            app_send_message(chan(dm)->id, text);
    } else {
        /* No conversation yet: Discord creates it, then we switch to it. */
        lstrcpynA(g_ui.pending_dm, user_id, sizeof g_ui.pending_dm);
        app_open_dm(user_id, text ? text : "");
    }
}

static void pop_send(void)
{
    int n = GetWindowTextLengthW(g_ui.pop_edit);
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
    open_dm(user, text.data);
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
    POINT pt = {pop_width() - S(12) - S(32), S(12) + S(34)};
    int cmd;
    char id[24];

    lstrcpynA(id, g_ui.pop_user, sizeof id);
    if (g_ui.pop_profile)
        AppendMenuW(menu, MF_STRING, 1, L"Copy username");
    AppendMenuW(menu, MF_STRING, 2, L"Copy user ID");
    ClientToScreen(g_ui.pop, &pt);
    cmd = menu_track(menu, pt.x, pt.y);
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
    int bx = pop_width() - S(12) - S(32), by = S(12);

    if (x >= bx && x < bx + S(32) && y >= by && y < by + S(32))
        return -2;
    if (g_ui.pop_self)
        for (int k = 0; k < 3; k++)
            if (y >= g_ui.pop_status_y[k] && y < g_ui.pop_status_y[k] + S(40) && x >= S(POP_PAD) && x < pop_width() - S(POP_PAD))
                return -10 - k;
    for (int i = 0; p && i < p->nbadges && i < (int)ARRAYSIZE(g_ui.pop_badge_x); i++)
        if (x >= g_ui.pop_badge_x[i] && x < g_ui.pop_badge_x[i] + S(POP_BADGE) && y >= g_ui.pop_badge_y[i] &&
            y < g_ui.pop_badge_y[i] + S(POP_BADGE))
            return i;
    return -1;
}

/* Our status becomes the k-th of k_status_codes, here and on every client. */
static void set_status(int k)
{
    char fields[48];

    g_ui.my_status = k_status_states[k];
    app_set_status(k_status_codes[k], g_ui.model ? model_str(g_ui.model, g_ui.model->custom_status) : "");
    wsprintfA(fields, "\"status\":\"%s\"", k_status_codes[k]);
    app_user_settings(fields);
}

static void pop_draw(RECT rc)
{
    (void)rc;
    pop_render(1);
}

static LRESULT CALLBACK pop_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        g_ui.pop_frame = paint_frame(wnd, pop_draw);
        return 0;
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
            int hand = h == -2 || (h <= -10 && h >= -12) || (p && h >= 0 && h < p->nbadges && p->badges[h].link.len);
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
        if (h == -2) {
            pop_menu();
        } else if (h == -10) {
            pop_close();
            settings_open();
        } else if (h == -11) {
            /* the status choices, beside the row */
            HMENU menu = CreatePopupMenu();
            POINT pt = {pop_width() + S(8), g_ui.pop_status_y[1] - S(8)};
            int cmd;
            static const wchar_t *const labels[4] = {L"Online", L"Idle",
                                                     L"Do Not Disturb\nYou will not receive desktop notifications",
                                                     L"Invisible\nYou will appear offline"};
            for (int k = 0; k < 4; k++) {
                AppendMenuW(menu, MF_STRING, (UINT_PTR)(k + 1), labels[k]);
                menu_mark(menu, (UINT)(k + 1), MENU_STATUS + k_status_states[k]);
            }
            ClientToScreen(wnd, &pt);
            cmd = menu_track(menu, pt.x, pt.y);
            DestroyMenu(menu);
            if (cmd > 0) {
                set_status(cmd - 1);
                redraw();
                InvalidateRect(wnd, NULL, FALSE);
            }
        } else if (h == -12) {
            copy_text(g_ui.model ? g_ui.model->user_id : "");
            pop_close();
        }
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
    if (!p && g_ui.pop_edit)
        pop_cue(name);
    fade_in(g_ui.pop);
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
    top += divider_h(m);
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

/* ---- Message actions: hover toolbar, reply and edit bar, delete confirmation, typing ---- */

#define TOOL_BTN 32
#define TIMER_TYPING 2
#define TYPING_MS 10000

static const wchar_t *const k_quick[] = {L"\xD83D\xDC4D", L"\x2764\xFE0F", L"\xD83D\xDE02"}; /* thumbs up, heart, joy */
enum { TOOL_REACT0, TOOL_REACT1, TOOL_REACT2, TOOL_ADD, TOOL_REPLY, TOOL_EDIT, TOOL_DELETE, TOOL_COUNT };

static int own_message(const msg_t *m)
{
    return g_ui.model && lstrcmpA(m->author_id, g_ui.model->user_id) == 0;
}

/* Top of message i on screen (the same walk as message_at). */
static int msg_top(int i)
{
    RECT a = message_area();
    int y = a.bottom + g_ui.msg_scroll - S(16);

    for (int k = g_ui.nmsgs; k-- > i;)
        y -= msg_height(&g_ui.msgs[k]);
    y += divider_h(&g_ui.msgs[i]);
    return y;
}

/* Buttons shown for message i, in order; returns how many. */
static int tool_buttons(int i, int *out)
{
    int n = 0;

    for (int k = TOOL_REACT0; k <= TOOL_REPLY; k++)
        out[n++] = k;
    if (own_message(&g_ui.msgs[i])) {
        out[n++] = TOOL_EDIT;
        out[n++] = TOOL_DELETE;
    }
    return n;
}

static RECT toolbar_rect(int i, int n)
{
    RECT a = message_area();
    int w = n * S(TOOL_BTN) + S(8), y = msg_top(i) + (g_ui.msgs[i].grouped == 1 ? -S(20) : S(0));

    if (y < a.top + S(4))
        y = a.top + S(4);
    return rect(a.right - S(24) - w, y, w, S(TOOL_BTN) + S(4));
}

static void paint_toolbar(void)
{
    int i = g_ui.hover_msg, btn[TOOL_COUNT], n;
    RECT r;

    if (i < 0 || i >= g_ui.nmsgs || g_ui.msgs[i].system || g_ui.msgs[i].deleted)
        return;
    n = tool_buttons(i, btn);
    r = toolbar_rect(i, n);
    r_round(r.left, r.top, r.right - r.left, r.bottom - r.top, S(8), 0xFF121214);
    r_round_outline(r.left, r.top, r.right - r.left, r.bottom - r.top, S(8), 1, 0xFF242426);
    for (int k = 0; k < n; k++) {
        int x = r.left + S(4) + k * S(TOOL_BTN), y = r.top + S(2);
        if (g_ui.hover_tool == k)
            r_round(x, y, S(TOOL_BTN), S(TOOL_BTN), S(6), ARGB(C_SELECT));
        if (btn[k] <= TOOL_REACT2)
            text_w(g_ui.f_body, C_INK, rect(x, y, S(TOOL_BTN), S(TOOL_BTN)), k_quick[btn[k]], -1,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        else
            text_w(g_ui.f_icon, btn[k] == TOOL_DELETE && g_ui.hover_tool == k ? C_RED : C_MUTED,
                   rect(x, y, S(TOOL_BTN), S(TOOL_BTN)),
                   btn[k] == TOOL_ADD ? L"\xE76E" : btn[k] == TOOL_REPLY ? L"\xE97A" : btn[k] == TOOL_EDIT ? L"\xE70F" : L"\xE74D", -1,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

/* Index (in the toolbar) of the button under (x, y), or -1. */
static int toolbar_hit(int x, int y, int *action)
{
    int i = g_ui.hover_msg, btn[TOOL_COUNT], n;
    RECT r;

    if (i < 0 || i >= g_ui.nmsgs || g_ui.msgs[i].system)
        return -1;
    n = tool_buttons(i, btn);
    r = toolbar_rect(i, n);
    if (x < r.left + S(4) || x >= r.right - S(4) || y < r.top || y >= r.bottom)
        return -1;
    int k = (x - r.left - S(4)) / S(TOOL_BTN);
    if (k >= n)
        return -1;
    if (action)
        *action = btn[k];
    return k;
}

/* ---- Reply / edit bar above the composer ---- */

static void set_composer_text(const char *s)
{
    wchar_t *w = utf8_to_wide(s ? s : "", s ? lstrlenA(s) : 0);

    SetWindowTextW(g_ui.composer, w);
    SendMessageW(g_ui.composer, EM_SETSEL, (WPARAM)lstrlenW(w), (LPARAM)lstrlenW(w));
    mem_free(w);
}

static void bar_close(void)
{
    if (g_ui.bar == BAR_EDIT)
        SetWindowTextW(g_ui.composer, L"");
    g_ui.bar = BAR_NONE;
    g_ui.bar_msg[0] = 0;
    sb_clear(&g_ui.bar_name);
    place_composer();
    clamp_msg_scroll();
    redraw();
}

static void start_reply(int i)
{
    g_ui.bar = BAR_REPLY;
    g_ui.bar_mention = !own_message(&g_ui.msgs[i]);
    lstrcpynA(g_ui.bar_msg, g_ui.msgs[i].id, sizeof g_ui.bar_msg);
    set_text(&g_ui.bar_name, author_name(&g_ui.msgs[i]));
    place_composer();
    SetFocus(g_ui.composer);
    redraw();
}

static void start_edit(int i)
{
    g_ui.bar = BAR_EDIT;
    lstrcpynA(g_ui.bar_msg, g_ui.msgs[i].id, sizeof g_ui.bar_msg);
    set_composer_text(g_ui.msgs[i].content.data);
    place_composer();
    SetFocus(g_ui.composer);
    redraw();
}

/* Up arrow in an empty composer edits our last message, like Discord. */
static int edit_last(void)
{
    for (int i = g_ui.nmsgs; i-- > 0;)
        if (own_message(&g_ui.msgs[i]) && !g_ui.msgs[i].system) {
            start_edit(i);
            return 1;
        }
    return 0;
}

static void paint_bar(int x0, int w, int cy)
{
    char line[160];
    int y = cy - S(BAR_H);

    if (!g_ui.bar)
        return;
    r_round(x0 + S(COMPOSER_GAP), y, w - 2 * S(COMPOSER_GAP), S(BAR_H) + S(10), S(8), 0xFF121214u);
    if (g_ui.bar == BAR_REPLY)
        wsprintfA(line, "Replying to %.100s", g_ui.bar_name.data ? g_ui.bar_name.data : "");
    else
        lstrcpyA(line, "Editing message \xE2\x80\xA2 escape to cancel \xE2\x80\xA2 enter to save");
    text(g_ui.f_small, C_MUTED, rect(x0 + S(24), y, w - S(160), S(BAR_H)), line, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (g_ui.bar == BAR_REPLY) {
        /* Whether the reply pings its author: "@" and a switch whose knob slides across. */
        int zx = x0 + w - S(128), tw = S(34), th = S(20), tx = zx + S(64) - tw - S(4), ty = y + (S(BAR_H) - th) / 2;
        int hov = g_ui.bar_hover == 1, kd = th - S(6);
        float t = tween_on(TW_MENTION, 0, g_ui.bar_mention, TW_SHAPE, zx, ty, S(64), th);
        unsigned off = hov ? 0xFF4A4A4Au : 0xFF2E2E31u, on = hov ? 0xFF4752C4u : ARGB(C_BRAND);
        r_text(g_ui.f_h, lerp_argb(ARGB(hov ? C_MUTED : C_FAINT), ARGB(C_BRAND), t), zx, y, tx - zx - S(6), S(BAR_H), L"@", -1,
               rflags(DT_RIGHT | DT_VCENTER | DT_SINGLELINE));
        r_round(tx, ty, tw, th, th / 2, lerp_argb(off, on, t));
        r_circle(lerp_i(tx + S(3), tx + tw - S(3) - kd, t), ty + S(3), kd, lerp_argb(0xFFB5AFA4u, 0xFF121214u, t));
    }
    {
        int bx = x0 + w - S(56), by = y + (S(BAR_H) - S(24)) / 2;
        float t = tween_on(TW_BAR_CLOSE, 0, g_ui.bar_hover == 2, TW_FAST, bx, by, S(24), S(24));
        if (t > 0.f)
            r_circle(bx, by, S(24), lerp_argb(0xFF121214u, 0xFF242426u, t));
        r_text(g_ui.f_icon, lerp_argb(ARGB(C_MUTED), ARGB(C_INK), t), bx, y, S(24), S(BAR_H), L"\xE711", -1,
               rflags(DT_CENTER | DT_VCENTER | DT_SINGLELINE));
    }
}

/* The part of the reply or edit bar at (x, y): 1 the mention switch, 2 the close button, 0 none. */
static int bar_hit(int x, int y)
{
    RECT rc;
    int x0 = S(RAIL_W + SIDE_W), w, cy;

    if (!g_ui.bar)
        return 0;
    GetClientRect(g_ui.wnd, &rc);
    w = main_right() - x0;
    cy = rc.bottom - S(COMPOSER_GAP) - S(COMPOSER_H);
    if (y < cy - S(BAR_H) || y >= cy)
        return 0;
    if (x >= x0 + w - S(56) && x < x0 + w - S(32))
        return 2;
    if (g_ui.bar == BAR_REPLY && x >= x0 + w - S(128) && x < x0 + w - S(64))
        return 1;
    return 0;
}

/* Clicks on the bar: 1 handled. */
static int click_bar(int x, int y)
{
    RECT rc;
    int x0 = S(RAIL_W + SIDE_W), w, cy, top;

    if (!g_ui.bar)
        return 0;
    GetClientRect(g_ui.wnd, &rc);
    w = main_right() - x0;
    cy = rc.bottom - S(COMPOSER_GAP) - S(COMPOSER_H);
    top = cy - S(BAR_H);
    if (y < top || y >= cy)
        return 0;
    if (bar_hit(x, y) == 2)
        bar_close();
    else if (bar_hit(x, y) == 1) {
        g_ui.bar_mention ^= 1;
        redraw();
    }
    return 1;
}

/* ---- Delete confirmation ---- */

static RECT confirm_rect(void)
{
    RECT rc;

    GetClientRect(g_ui.wnd, &rc);
    return rect((rc.right - S(440)) / 2, (rc.bottom - S(200)) / 2, S(440), S(200));
}

static void paint_confirm(void)
{
    RECT rc, r;
    int i = g_ui.confirm ? find_msg(g_ui.confirm_id) : -1;
    wchar_t *preview;

    if (i < 0)
        return;
    GetClientRect(g_ui.wnd, &rc);
    r_fill(0, 0, rc.right, rc.bottom, 0xB0000000u);
    r = confirm_rect();
    r_round(r.left, r.top, r.right - r.left, r.bottom - r.top, S(10), 0xFF151515);
    r_round_outline(r.left, r.top, r.right - r.left, r.bottom - r.top, S(10), 1, 0xFF242426);
    text(g_ui.f_title, C_INK, rect(r.left + S(20), r.top + S(18), S(400), S(28)), "Delete Message", DT_LEFT | DT_SINGLELINE);
    text(g_ui.f_body, C_MUTED, rect(r.left + S(20), r.top + S(56), S(400), S(22)),
         "Are you sure you want to delete this message?", DT_LEFT | DT_SINGLELINE);
    preview = plain_text(&g_ui.msgs[i].text);
    r_round(r.left + S(20), r.top + S(86), S(400), S(40), S(6), 0xFF17181B);
    r_text(g_ui.f_body, ARGB(C_INK), r.left + S(32), r.top + S(86), S(376), S(40), preview, -1,
           R_LEFT | R_VCENTER | R_SINGLE | R_ELLIPSIS);
    mem_free(preview);
    r_round(r.right - S(220), r.bottom - S(56), S(96), S(36), S(6), g_ui.confirm_hover == 1 ? 0xFF242426 : 0xFF1E1E20);
    text(g_ui.f_h, C_INK, rect(r.right - S(220), r.bottom - S(56), S(96), S(36)), "Cancel", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    r_round(r.right - S(116), r.bottom - S(56), S(96), S(36), S(6), g_ui.confirm_hover == 2 ? 0xFFD83C3Eu : 0xFFC0363Au);
    text(g_ui.f_h, C_INK, rect(r.right - S(116), r.bottom - S(56), S(96), S(36)), "Delete", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* 1 cancel, 2 delete, 0 elsewhere in the dialog, -1 outside. */
static int confirm_hit(int x, int y)
{
    RECT r = confirm_rect();

    if (y >= r.bottom - S(56) && y < r.bottom - S(20)) {
        if (x >= r.right - S(220) && x < r.right - S(124))
            return 1;
        if (x >= r.right - S(116) && x < r.right - S(20))
            return 2;
    }
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom ? 0 : -1;
}

static void confirm_close(int do_delete)
{
    if (do_delete && find_msg(g_ui.confirm_id) >= 0)
        app_delete_message(g_ui.msgs_channel, g_ui.confirm_id);
    g_ui.confirm = 0;
    g_ui.confirm_hover = 0;
    redraw();
}

/* Adds our reaction r to message m, or takes it back when we already reacted so. */
static void toggle_reaction(msg_t *m, const msg_reaction_t *r)
{
    int k, add;

    for (k = 0; k < m->nreactions && !same_reaction(&m->reactions[k], r); k++)
        ;
    add = !(k < m->nreactions && m->reactions[k].me);
    app_react(m->channel_id[0] ? m->channel_id : g_ui.msgs_channel, m->id, r, add);
    apply_reaction(m, r, add ? 1 : -1, 1);
}

static void run_tool(int action)
{
    int i = g_ui.hover_msg;
    msg_t *m;

    if (i < 0 || i >= g_ui.nmsgs)
        return;
    m = &g_ui.msgs[i];
    switch (action) {
    case TOOL_REACT0:
    case TOOL_REACT1:
    case TOOL_REACT2: {
        msg_reaction_t r = {0};
        wide_to_utf8(k_quick[action], (size_t)lstrlenW(k_quick[action]), &r.emoji);
        toggle_reaction(m, &r);
        sb_free(&r.emoji);
        break;
    }
    case TOOL_ADD: {
        int btn[TOOL_COUNT], n = tool_buttons(i, btn);
        RECT r = toolbar_rect(i, n);
        picker_open(PICK_REACTION, m->id, r.left - S(8), r.top + S(PICK_H));
        break;
    }
    case TOOL_REPLY:
        start_reply(i);
        break;
    case TOOL_EDIT:
        start_edit(i);
        break;
    case TOOL_DELETE:
        if (GetKeyState(VK_SHIFT) < 0) { /* shift-click skips the question, like Discord */
            app_delete_message(g_ui.msgs_channel, m->id);
        } else {
            g_ui.confirm = 1;
            lstrcpynA(g_ui.confirm_id, m->id, sizeof g_ui.confirm_id);
        }
        break;
    }
    redraw();
}

/* ---- Typing ---- */

static void typing_prune(void)
{
    DWORD now = GetTickCount();
    int k = 0;

    for (int i = 0; i < g_ui.ntyping; i++)
        if ((int)(g_ui.typing[i].until - now) > 0)
            g_ui.typing[k++] = g_ui.typing[i];
        else
            sb_free(&g_ui.typing[i].name);
    g_ui.ntyping = k;
    if (!k)
        KillTimer(g_ui.wnd, TIMER_TYPING);
}

static void typing_stop(const char *user)
{
    for (int i = 0; i < g_ui.ntyping; i++)
        if (lstrcmpA(g_ui.typing[i].user, user) == 0)
            g_ui.typing[i].until = GetTickCount();
    typing_prune();
}

static void typing_clear(void)
{
    for (int i = 0; i < g_ui.ntyping; i++)
        sb_free(&g_ui.typing[i].name);
    g_ui.ntyping = 0;
    KillTimer(g_ui.wnd, TIMER_TYPING);
}

static void on_typing(const sb_t *p)
{
    const char *channel = p->data, *user = channel + lstrlenA(channel) + 1, *name = user + lstrlenA(user) + 1;
    int i;

    if (lstrcmpA(channel, g_ui.msgs_channel) != 0)
        return;
    for (i = 0; i < g_ui.ntyping && lstrcmpA(g_ui.typing[i].user, user) != 0; i++)
        ;
    if (i == g_ui.ntyping) {
        if (i == (int)ARRAYSIZE(g_ui.typing))
            return;
        g_ui.typing[i] = (typing_t){0};
        lstrcpynA(g_ui.typing[i].user, user, sizeof g_ui.typing[i].user);
        g_ui.ntyping++;
    }
    sb_clear(&g_ui.typing[i].name);
    if (*name) {
        sb_add(&g_ui.typing[i].name, name);
    } else { /* DMs: the name from a message or the conversation */
        for (int k = g_ui.nmsgs; k-- > 0;)
            if (lstrcmpA(g_ui.msgs[k].author_id, user) == 0) {
                sb_add(&g_ui.typing[i].name, author_name(&g_ui.msgs[k]));
                break;
            }
        if (!g_ui.typing[i].name.len && g_ui.channel >= 0)
            sb_add(&g_ui.typing[i].name, model_str(g_ui.model, chan(g_ui.channel)->name));
    }
    g_ui.typing[i].until = GetTickCount() + TYPING_MS;
    SetTimer(g_ui.wnd, TIMER_TYPING, 1000, NULL);
}

static void paint_typing(int x0, int w, int y)
{
    char line[200];

    if (!g_ui.ntyping)
        return;
    if (g_ui.ntyping == 1)
        wsprintfA(line, "%.80s is typing\xE2\x80\xA6", g_ui.typing[0].name.data);
    else if (g_ui.ntyping == 2)
        wsprintfA(line, "%.60s and %.60s are typing\xE2\x80\xA6", g_ui.typing[0].name.data, g_ui.typing[1].name.data);
    else if (g_ui.ntyping == 3)
        wsprintfA(line, "%.40s, %.40s and %.40s are typing\xE2\x80\xA6", g_ui.typing[0].name.data, g_ui.typing[1].name.data,
                  g_ui.typing[2].name.data);
    else
        lstrcpyA(line, "Several people are typing\xE2\x80\xA6");
    text(g_ui.f_small, C_MUTED, rect(x0 + S(20), y, w - S(40), S(20)), line, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

/* Tells the others we are typing, at most every 8 seconds. */
static void composer_changed(void)
{
    DWORD now = GetTickCount();

    if (!open_is_text())
        return;
    ac_update();
    redraw();
    if (g_ui.bar != BAR_EDIT && GetWindowTextLengthW(g_ui.composer) > 0 && now - g_ui.typing_sent > 8000) {
        g_ui.typing_sent = now;
        app_typing(g_ui.msgs_channel);
    }
}

/* ---- Member list (right column) ---- */

#define MEMBERS_W 240
#define ML_GROUP_H 40
#define ML_ROW_H 44

static int members_shown(void)
{
    return g_ui.show_members && g_ui.view == VIEW_APP && open_is_text() && !g_ui.settings_open &&
           (g_ui.guild >= 0 || chan(g_ui.channel)->type == CH_GROUP_DM ||
            (chan(g_ui.channel)->type == CH_DM && chan(g_ui.channel)->user_id[0]));
}

/* The right column's width: a DM shows the other person's profile there, wider than a member list. */
static int side_panel_w(void)
{
    return members_shown() && chan(g_ui.channel)->type == CH_DM ? S(PROFILE_W) : S(MEMBERS_W);
}

/* Opens or closes the profile beside a 1:1 DM to match what is shown. */
static void dm_profile_sync(void)
{
    const channel_t *c = g_ui.model && g_ui.channel >= 0 ? chan(g_ui.channel) : NULL;
    int want = c && members_shown() && c->type == CH_DM;

    if (g_ui.pop && g_ui.pop_docked && (!want || lstrcmpA(g_ui.pop_user, c->user_id) != 0))
        pop_close();
    if (want && !g_ui.pop) {
        HWND focus = GetFocus();
        pop_open(c->user_id, model_str(g_ui.model, c->name), c->avatar, 0, 0, 0);
        if (g_ui.pop) {
            g_ui.pop_docked = 1;
            if (g_ui.pop_edit) {
                DestroyWindow(g_ui.pop_edit);
                g_ui.pop_edit = NULL;
            }
            pop_place();
            SetFocus(focus && IsWindow(focus) ? focus : g_ui.composer);
        }
    }
}

/* A group DM's members, us included, sorted by name: fills ids, avatars and names, returns how many. */
#define DM_MEMBERS_MAX 16
typedef struct {
    char id[24], avatar[40], name[100];
} dm_member_t;

static int dm_members(dm_member_t *out)
{
    const channel_t *c = chan(g_ui.channel);
    const char *p = c->recipients ? model_str(g_ui.model, c->recipients) : NULL;
    int n = 0;

    while (n < DM_MEMBERS_MAX - 1 && (p = recip_next(p, out[n].id, out[n].avatar, out[n].name, sizeof out[n].name)) != NULL)
        n++;
    lstrcpynA(out[n].id, g_ui.model->user_id, sizeof out[n].id);
    lstrcpynA(out[n].avatar, g_ui.model->user_avatar, sizeof out[n].avatar);
    lstrcpynA(out[n].name, g_ui.model->user_name ? model_str(g_ui.model, g_ui.model->user_name) : "", sizeof out[n].name);
    n++;
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && CompareStringA(LOCALE_USER_DEFAULT, NORM_IGNORECASE, out[b - 1].name, -1, out[b].name, -1) ==
                                     CSTR_GREATER_THAN;
             b--) {
            dm_member_t t = out[b];
            out[b] = out[b - 1];
            out[b - 1] = t;
        }
    return n;
}

#define DM_MEMBER_H 42
#define DM_MEMBERS_TOP 44

/* Right edge of the message column. */
static int main_right(void)
{
    RECT rc;

    GetClientRect(g_ui.wnd, &rc);
    return rc.right - (members_shown() ? side_panel_w() : 0);
}

static unsigned status_color(int status)
{
    switch (status) {
    case ML_ONLINE: return 0xFF23A55Au;
    case ML_IDLE: return 0xFFF0B232u;
    case ML_DND: return 0xFFF23F43u;
    default: return 0xFF80848Eu;
    }
}

/* Status indicator of size s at (cx, cy): dot, crescent, bar or hollow ring like Discord. */
static void status_dot(int cx, int cy, int s, int status, unsigned bg)
{
    r_circle(cx, cy, s, status_color(status));
    if (status == ML_OFFLINE || status == ML_UNKNOWN)
        r_circle(cx + s / 4, cy + s / 4, s / 2, bg);
    else if (status == ML_IDLE)
        r_circle(cx - s / 6, cy - s / 6, s * 5 / 8, bg);
    else if (status == ML_DND)
        r_round(cx + s / 5, cy + s / 2 - S(1), s - 2 * (s / 5), S(2) > 1 ? S(2) : 1, S(1), bg);
}

/* Status dot on an avatar of size d at (x, y), with a ring of the background color. */
static void paint_status(int x, int y, int d, int status, unsigned bg)
{
    int s = d * 10 / 32, ring = s + S(6), cx = x + d - s + S(1), cy = y + d - s + S(1);

    r_circle(cx - (ring - s) / 2, cy - (ring - s) / 2, ring, bg);
    status_dot(cx, cy, s, status, bg);
}

static int ml_row_h(const ml_item_t *it)
{
    return it->group ? S(ML_GROUP_H) : S(ML_ROW_H);
}

static int ml_content(void)
{
    int h = S(8);

    for (int i = 0; i < g_ui.ml.n; i++)
        h += ml_row_h(&g_ui.ml.items[i]);
    return h;
}

static void ml_clamp(void)
{
    RECT rc;
    int view, max;

    GetClientRect(g_ui.wnd, &rc);
    view = rc.bottom - S(HEADER_H);
    max = ml_content() - view;
    if (g_ui.ml_scroll > max)
        g_ui.ml_scroll = max;
    if (g_ui.ml_scroll < 0)
        g_ui.ml_scroll = 0;
}

static void group_title(const ml_item_t *it, char *out, int size)
{
    model_role_t r;
    unsigned cursor = 0;
    const char *name = it->id;
    char buf[96], role[64];

    if (lstrcmpA(it->id, "online") == 0)
        name = "Online";
    else if (lstrcmpA(it->id, "offline") == 0)
        name = "Offline";
    else
        while (model_role_next(g_ui.model, g_ui.guild, &cursor, &r))
            if (lstrcmpA(r.id, it->id) == 0) {
                lstrcpynA(role, r.name, r.name_len + 1 < (int)sizeof role ? r.name_len + 1 : (int)sizeof role);
                name = role;
                break;
            }
    wsprintfA(buf, "%.60s \xE2\x80\x94 %d", name, it->count ? it->count : ml_group_count(&g_ui.ml, it->id));
    lstrcpynA(out, buf, size);
}

/* A group DM's members: "Members—3", then each with their status; those offline are faded, as in Discord. */
static void paint_dm_members(RECT rc, int x0)
{
    static dm_member_t m[DM_MEMBERS_MAX];
    int n = dm_members(m), y = S(HEADER_H) + S(16);
    char title[40];

    wsprintfA(title, "Members\xE2\x80\x94%d", n);
    text(g_ui.f_small_mid, C_MUTED, rect(x0 + S(16), y, S(MEMBERS_W) - S(32), S(20)), title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y = S(HEADER_H) + S(DM_MEMBERS_TOP);
    for (int i = 0; i < n && y < rc.bottom; i++, y += S(DM_MEMBER_H)) {
        int st = user_status(m[i].id), off = st == ML_OFFLINE || st == ML_UNKNOWN, hov = g_ui.ml_hover == 1000 + i;
        r_image_t *img = user_avatar(m[i].id, m[i].avatar);
        unsigned bg = row_bg(TW_MEMBER, 1000 + i, 0, hov, ARGB(C_SIDE), x0 + S(8), y + S(1), S(MEMBERS_W) - S(16),
                             S(DM_MEMBER_H) - S(2), S(8));
        int ay = y + (S(DM_MEMBER_H) - S(32)) / 2;
        wchar_t *name = utf8_to_wide(m[i].name, lstrlenA(m[i].name));
        if (img)
            r_image(img, x0 + S(16), ay, S(32), S(32), S(16));
        else
            r_circle(x0 + S(16), ay, S(32), ARGB(C_ITEM));
        if (!off)
            paint_status(x0 + S(16), ay, S(32), st, bg);
        else if (!hov)
            r_circle(x0 + S(16), ay, S(32), 0xB3000000u);
        r_text(g_ui.f_body, off && !hov ? 0x66DCDCDFu : hov ? ARGB(C_INK) : ARGB(C_MUTED), x0 + S(56), y, S(MEMBERS_W) - S(64),
               S(DM_MEMBER_H), name, -1, R_LEFT | R_VCENTER | R_SINGLE | R_ELLIPSIS);
        mem_free(name);
    }
}

static void paint_members(RECT rc)
{
    int x0 = rc.right - side_panel_w(), y = S(HEADER_H) + S(8) - g_ui.ml_scroll;

    fill(x0, S(HEADER_H), side_panel_w(), rc.bottom - S(HEADER_H), C_SIDE);
    fill(x0, S(HEADER_H), S(1) > 1 ? S(1) : 1, rc.bottom - S(HEADER_H), C_LINE);
    if (g_ui.guild < 0) {
        if (chan(g_ui.channel)->type == CH_GROUP_DM)
            paint_dm_members(rc, x0);
        return; /* a DM: the docked popout covers it */
    }
    r_clip(x0, S(HEADER_H), S(MEMBERS_W), rc.bottom - S(HEADER_H));
    for (int i = 0; i < g_ui.ml.n; i++) {
        const ml_item_t *it = &g_ui.ml.items[i];
        int h = ml_row_h(it);

        if (y + h > S(HEADER_H) && y < rc.bottom && r_visible(y, h)) {
            if (it->group && it->valid) {
                char title[128];
                wchar_t *wt;
                group_title(it, title, sizeof title);
                wt = utf8_to_wide(title, lstrlenA(title));
                /* as Discord's refreshed type: 14px medium, not in capitals */
                text_w(g_ui.f_menu, C_CHANNEL, rect(x0 + S(16), y + S(16), S(MEMBERS_W) - S(24), S(20)), wt, -1,
                       DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                mem_free(wt);
            } else if (it->valid) {
                int offline = it->status == ML_OFFLINE || it->status == ML_UNKNOWN;
                r_image_t *img = member_avatar_anim(g_ui.ml.guild, it->id, it->member_avatar, it->avatar, g_ui.ml_hover == i);
                unsigned color = model_role_color(g_ui.model, g_ui.guild, it->roles.data ? it->roles.data : "");
                unsigned ink = color ? 0xFF000000u | color : ARGB(C_INK);
                int ay = y + (h - S(32)) / 2, tx = x0 + S(56), tw = S(MEMBERS_W) - S(64);
                wchar_t *name;
                unsigned bg = row_bg(TW_MEMBER, i, 0, g_ui.ml_hover == i, ARGB(C_SIDE), x0 + S(8), y + S(1),
                                     S(MEMBERS_W) - S(16), h - S(2), S(6));

                if (img)
                    r_image(img, x0 + S(16), ay, S(32), S(32), S(16));
                else
                    r_circle(x0 + S(16), ay, S(32), ARGB(C_ITEM));
                if (!offline)
                    paint_status(x0 + S(16), ay, S(32), it->status, bg);
                else /* offline members are dimmed */
                    r_circle(x0 + S(16), ay, S(32), 0x80111111u);
                if (offline)
                    ink = (ink & 0xFFFFFF) | 0x80000000u;
                name = utf8_to_wide(it->name.data ? it->name.data : "", it->name.len);
                r_text(g_ui.f_h, ink, tx, it->activity.len ? y + S(4) : y, tw - (it->bot ? S(34) : 0),
                       it->activity.len ? S(20) : h, name, -1, R_LEFT | R_SINGLE | R_ELLIPSIS | (it->activity.len ? 0 : R_VCENTER));
                if (it->bot) {
                    int bw = r_text_width(g_ui.f_h, name, -1);
                    int bx = tx + (bw < tw - S(34) ? bw : tw - S(34)) + S(4), by = (it->activity.len ? y + S(6) : y + (h - S(16)) / 2);
                    r_round(bx, by, S(30), S(16), S(4), 0xFF5865F2u);
                    text(g_ui.f_cat, C_INK, rect(bx, by, S(30), S(16)), "APP", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
                mem_free(name);
                if (it->activity.len)
                    text(g_ui.f_small, C_MUTED, rect(tx, y + S(22), tw, S(18)), it->activity.data,
                         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }
        y += h;
    }
    r_unclip();
}

/* Member row under (x, y), -1 if none. */
static int ml_hit(int x, int y, int *top)
{
    RECT rc;
    int yy = S(HEADER_H) + S(8) - g_ui.ml_scroll;

    if (!members_shown())
        return -1;
    GetClientRect(g_ui.wnd, &rc);
    if (x < rc.right - S(MEMBERS_W) || y < S(HEADER_H))
        return -1;
    if (g_ui.guild < 0) { /* a group DM: 1000 + its place in dm_members() */
        dm_member_t m[DM_MEMBERS_MAX];
        int n = dm_members(m), k = (y - S(HEADER_H) - S(DM_MEMBERS_TOP)) / S(DM_MEMBER_H);
        if (y < S(HEADER_H) + S(DM_MEMBERS_TOP) || k >= n)
            return -1;
        if (top)
            *top = S(HEADER_H) + S(DM_MEMBERS_TOP) + k * S(DM_MEMBER_H);
        return 1000 + k;
    }
    for (int i = 0; i < g_ui.ml.n; i++) {
        int h = ml_row_h(&g_ui.ml.items[i]);
        if (y >= yy && y < yy + h) {
            if (top)
                *top = yy;
            return g_ui.ml.items[i].valid && !g_ui.ml.items[i].group ? i : -1;
        }
        yy += h;
    }
    return -1;
}

/* Asks for the part of the list being scrolled to (Discord streams it by ranges of 100). */
static void ml_request_visible(void)
{
    int yy = S(HEADER_H) + S(8) - g_ui.ml_scroll, first = -1;

    if (!members_shown())
        return;
    for (int i = 0; i < g_ui.ml.n && first < 0; i++) {
        yy += ml_row_h(&g_ui.ml.items[i]);
        if (yy > S(HEADER_H))
            first = i;
    }
    if (first < 0)
        first = g_ui.ml.n;
    if (first / 100 != g_ui.ml_chunk) {
        g_ui.ml_chunk = first / 100;
        app_subscribe_range(g_ui.model->guilds[g_ui.guild].id, g_ui.msgs_channel, g_ui.ml_chunk * 100);
    }
}

static void on_member_list(json_t d)
{
    const char *guild = open_guild_id();
    json_t v;
    char gid[24] = "";

    if (json_get(d, "guild_id", &v))
        json_raw(v, gid, sizeof gid);
    if (!guild || lstrcmpA(gid, guild) != 0)
        return;
    ml_apply(&g_ui.ml, d);
    /* The list tells us members' roles and nicknames: authors get their colors without asking. */
    for (int i = 0; i < g_ui.ml.n; i++) {
        const ml_item_t *it = &g_ui.ml.items[i];
        member_t *mb;
        if (!it->valid || it->group || ((mb = member_find(gid, it->id)) && mb->known))
            continue;
        mb = member_add(gid, it->id);
        mb->known = 1;
        sb_clear(&mb->roles);
        sb_add(&mb->roles, it->roles.data ? it->roles.data : "");
    }
    ml_clamp();
}

/* ---- Emoji picker ---- */

/* The composer's tabs, in Discord's order. */
#define PICK_TAB_W 90
static const int k_pick_tabs[] = {TAB_GIFS, TAB_STICKERS, TAB_EMOJI};
static const char *const k_pick_tab_names[] = {"GIFs", "Stickers", "Emoji"};

static const wchar_t *picker_cue(void)
{
    return g_ui.picker_tab == TAB_GIFS       ? L"Search GIFs"
           : g_ui.picker_tab == TAB_STICKERS ? L"Find the perfect sticker"
                                             : L"Find the perfect emoji";
}

/* Sends a GIF link or a sticker from the picker, as a reply when one is being written. */
static void send_picked(const char *text, const char *sticker)
{
    const char *reply = g_ui.bar == BAR_REPLY ? g_ui.bar_msg : NULL;

    if (sticker)
        app_send_sticker(g_ui.msgs_channel, sticker, reply, g_ui.bar_mention);
    else if (reply)
        app_send_reply(g_ui.msgs_channel, text, reply, g_ui.bar_mention);
    else
        app_send_message(g_ui.msgs_channel, text);
    if (g_ui.bar == BAR_REPLY)
        bar_close();
    g_ui.msg_scroll = 0;
}


static void picker_rebuild(void)
{
    char q[256] = ""; /* 63 UTF-16 units take up to 189 bytes of UTF-8 */
    int n = 0, cap = 64;
    model_emoji_t e;
    unsigned cursor = 0;

    if (g_ui.picker_edit) {
        wchar_t w[64];
        GetWindowTextW(g_ui.picker_edit, w, 64);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, q, sizeof q, NULL, NULL);
    }
    mem_free(g_ui.pick_items);
    g_ui.pick_items = mem_alloc((size_t)cap * sizeof(pick_item_t));
#define PUSH(it)                                                                       \
    do {                                                                               \
        if (n == cap) {                                                                \
            cap *= 2;                                                                  \
            g_ui.pick_items = mem_realloc(g_ui.pick_items, (size_t)cap * sizeof(pick_item_t)); \
        }                                                                              \
        ((pick_item_t *)g_ui.pick_items)[n++] = (it);                                  \
    } while (0)
    if (g_ui.picker_tab == TAB_STICKERS) {
        /* The open server's stickers first; other servers' need Nitro. */
        int ng = g_ui.model ? (int)g_ui.model->nguilds : 0;
        for (int k = -1; k < ng; k++) {
            int g = k < 0 ? g_ui.guild : k, header = 0;
            if (g < 0 || (k >= 0 && (g == g_ui.guild || !g_ui.model->premium)))
                continue;
            cursor = 0;
            while (model_sticker_next(g_ui.model, g, &cursor, &e)) {
                pick_item_t it = {PI_STICKER, 0};
                int len = e.name_len < 39 ? e.name_len : 39;
                if (e.format == 3) /* Lottie: not drawn */
                    continue;
                lstrcpynA(it.name, e.name, len + 1);
                if (q[0] && !find_str_ci(it.name, q))
                    continue;
                if (!header) {
                    pick_item_t h = {PI_HEADER, -1 - g};
                    PUSH(h);
                    header = 1;
                }
                lstrcpynA(it.id, e.id, sizeof it.id);
                it.animated = e.format;
                PUSH(it);
            }
        }
        goto done;
    }
    /* The server's own emoji first, like Discord. */
    if (g_ui.model && g_ui.guild >= 0) {
        int header = 0;
        while (model_emoji_next(g_ui.model, g_ui.guild, &cursor, &e)) {
            pick_item_t it = {PI_CUSTOM, 0};
            int len = e.name_len < 39 ? e.name_len : 39;
            lstrcpynA(it.name, e.name, len + 1);
            if (q[0] && !find_str_ci(it.name, q))
                continue;
            if (!header) {
                pick_item_t h = {PI_HEADER, -1 - g_ui.guild};
                PUSH(h);
                header = 1;
            }
            lstrcpynA(it.id, e.id, sizeof it.id);
            it.animated = e.animated;
            PUSH(it);
        }
    }
    for (int c = 0; c < EMOJI_CATEGORIES; c++) {
        int header = 0;
        for (int i = k_emoji_categories[c].first; i < k_emoji_categories[c].end; i++) {
            pick_item_t it = {PI_UNICODE, i};
            if (!emoji_matches(i, q))
                continue;
            if (!header) {
                pick_item_t h = {PI_HEADER, c};
                PUSH(h);
                header = 1;
            }
            emoji_main_name(i, it.name, sizeof it.name);
            PUSH(it);
        }
    }
done:
#undef PUSH
    g_ui.npick = n;
    g_ui.pick_scroll = 0;
    g_ui.pick_hover = -1;
    picker_layout();
}

/* Places the items: headers take a full row, emoji fill rows of PICK_COLS. */
static void picker_layout(void)
{
    int stickers = g_ui.picker_tab == TAB_STICKERS, cols = stickers ? STICKER_COLS : PICK_COLS;
    int cell = stickers ? (S(PICK_W) - S(24)) / STICKER_COLS : S(PICK_CELL), col = 0, y = S(PICK_TOP);
    int x0 = (S(PICK_W) - cols * cell) / 2;

    for (int i = 0; i < g_ui.npick; i++) {
        pick_item_t *it = &((pick_item_t *)g_ui.pick_items)[i];
        if (it->kind == PI_HEADER) {
            if (col) {
                y += cell;
                col = 0;
            }
            it->x = x0;
            it->y = y;
            it->w = cols * cell;
            it->h = S(PICK_HEAD);
            y += S(PICK_HEAD);
        } else {
            it->x = x0 + col * cell;
            it->y = y;
            it->w = it->h = cell;
            if (++col == cols) {
                col = 0;
                y += cell;
            }
        }
    }
    g_ui.pick_content = y + (col ? cell : 0) - S(PICK_TOP);
}

static void picker_paint(RECT rc)
{
    int w = S(PICK_W), h = S(PICK_H), grid_bottom = h - S(PICK_FOOT);

    (void)rc;
    r_fill(0, 0, w, h, ARGB(C_MAIN));
    r_round(0, 0, w, h, S(8), 0xFF121214);
    r_round_outline(0, 0, w, h, S(8), 1, 0xFF242426);
    if (g_ui.picker_mode == PICK_COMPOSER) {
        for (int t = 0; t < 3; t++) {
            int tx = S(12) + t * S(PICK_TAB_W), id = k_pick_tabs[t];
            if (g_ui.picker_tab == id)
                r_round(tx, S(10), S(PICK_TAB_W) - S(8), S(26), S(6), ARGB(C_SELECT));
            text(g_ui.f_h, g_ui.picker_tab == id ? C_INK : C_MUTED, rect(tx, S(10), S(PICK_TAB_W) - S(8), S(26)),
                 k_pick_tab_names[t], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    r_round(S(12), S(PICK_TABS), w - S(24), S(32), S(6), 0xFF17181B);
    if (g_ui.picker_tab == TAB_GIFS) {
        gifs_paint(w, h);
        return;
    }
    r_clip(0, S(PICK_TOP) - S(4), w, grid_bottom - S(PICK_TOP) + S(4));
    for (int i = 0; i < g_ui.npick; i++) {
        const pick_item_t *it = &((pick_item_t *)g_ui.pick_items)[i];
        int x_ = it->x, y_ = it->y - g_ui.pick_scroll, w_ = it->w, h_ = it->h;
        if (y_ + h_ <= S(PICK_TOP) - S(4) || y_ >= grid_bottom || !r_visible(y_, h_))
            continue;
        if (it->kind == PI_HEADER) {
            const char *title = it->index < 0 ? model_str(g_ui.model, g_ui.model->guilds[-1 - it->index].name)
                                              : k_emoji_categories[it->index].name;
            text(g_ui.f_cat, C_MUTED, rect(x_ + S(4), y_, w_ - S(8), h_), title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            continue;
        }
        if (g_ui.pick_hover == i)
            r_round(x_ + S(2), y_ + S(2), w_ - S(4), h_ - S(4), S(6), ARGB(C_SELECT));
        if (it->kind == PI_STICKER) {
            r_image_t *img = sticker_image(it->id, it->animated);
            if (img)
                r_image(img, x_ + S(6), y_ + S(6), w_ - S(12), h_ - S(12), 0);
        } else if (it->kind == PI_CUSTOM) {
            char key[48], path[96];
            r_image_t *img;
            wsprintfA(key, "e:%s", it->id);
            wsprintfA(path, "/emojis/%s.png?size=64", it->id);
            if ((img = image_get(key, path, S(32))) != NULL)
                r_image(img, x_ + S(4), y_ + S(4), S(32), S(32), 0);
        } else {
            wchar_t *we = utf8_to_wide(k_emoji[it->index].emoji, lstrlenA(k_emoji[it->index].emoji));
            r_text(g_ui.f_emoji, ARGB(C_INK), x_, y_, w_, h_, we, -1, R_CENTER | R_VCENTER | R_SINGLE);
            mem_free(we);
        }
    }
    r_unclip();

    /* Footer: the hovered emoji and its name. */
    fill(S(1), grid_bottom, w - S(2), S(1), C_LINE);
    if (g_ui.pick_hover >= 0 && g_ui.pick_hover < g_ui.npick) {
        const pick_item_t *it = &((pick_item_t *)g_ui.pick_items)[g_ui.pick_hover];
        char label[64];
        if (it->kind == PI_UNICODE) {
            wchar_t *we = utf8_to_wide(k_emoji[it->index].emoji, lstrlenA(k_emoji[it->index].emoji));
            r_text(g_ui.f_emoji, ARGB(C_INK), S(12), grid_bottom, S(40), S(PICK_FOOT), we, -1, R_CENTER | R_VCENTER | R_SINGLE);
            mem_free(we);
        }
        wsprintfA(label, it->kind == PI_STICKER ? "%.40s" : ":%.40s:", it->name);
        text(g_ui.f_h, C_INK, rect(S(60), grid_bottom, w - S(72), S(PICK_FOOT)), label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        text(g_ui.f_small, C_FAINT, rect(S(16), grid_bottom, w - S(32), S(PICK_FOOT)),
             g_ui.picker_tab == TAB_STICKERS ? (g_ui.npick ? "Pick a sticker" : "No stickers here")
                                             : g_ui.npick ? "Pick an emoji" : "No emoji match",
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

static int picker_hit(int x, int y)
{
    if (y < S(PICK_TOP) || y >= S(PICK_H) - S(PICK_FOOT))
        return -1;
    for (int i = 0; i < g_ui.npick; i++) {
        const pick_item_t *it = &((pick_item_t *)g_ui.pick_items)[i];
        int top = it->y - g_ui.pick_scroll;
        if (it->kind != PI_HEADER && x >= it->x && x < it->x + it->w && y >= top && y < top + it->h)
            return i;
    }
    return -1;
}

static void picker_close(void)
{
    HWND p = g_ui.picker;

    if (!p)
        return;
    g_ui.picker = NULL;
    g_ui.picker_edit = NULL;
    DestroyWindow(p);
    SetFocus(g_ui.composer);
}

/* Custom emoji of the open server by name, as Discord writes them in a message. */
static const char *custom_emoji_markup(void *ctx, const char *name, size_t n)
{
    static char out[96];
    model_emoji_t e;
    unsigned cursor = 0;

    (void)ctx;
    if (!g_ui.model || g_ui.guild < 0)
        return NULL;
    while (model_emoji_next(g_ui.model, g_ui.guild, &cursor, &e))
        if ((size_t)e.name_len == n && CompareStringA(LOCALE_INVARIANT, 0, e.name, (int)n, name, (int)n) == CSTR_EQUAL) {
            /* names in the model are not NUL-terminated */
            {
                char nm[48];
                lstrcpynA(nm, e.name, e.name_len + 1 < (int)sizeof nm ? e.name_len + 1 : (int)sizeof nm);
                wsprintfA(out, "<%s:%s:%s>", e.animated ? "a" : "", nm, e.id);
            }
            return out;
        }
    return NULL;
}

static void picker_choose(int i)
{
    const pick_item_t *it = &((pick_item_t *)g_ui.pick_items)[i];

    if (it->kind == PI_STICKER) {
        if (open_is_text())
            send_picked(NULL, it->id);
        picker_close();
    } else if (g_ui.picker_mode == PICK_REACTION) {
        int m = find_msg(g_ui.picker_msg);
        if (m >= 0) {
            msg_reaction_t r = {0};
            if (it->kind == PI_CUSTOM) {
                lstrcpynA(r.emoji_id, it->id, sizeof r.emoji_id);
                sb_add(&r.emoji, it->name);
            } else {
                sb_add(&r.emoji, k_emoji[it->index].emoji);
            }
            toggle_reaction(&g_ui.msgs[m], &r);
            sb_free(&r.emoji);
        }
        picker_close();
    } else {
        char ins[64];
        wchar_t *w;
        if (it->kind == PI_CUSTOM)
            wsprintfA(ins, ":%s:", it->name);
        else
            lstrcpynA(ins, k_emoji[it->index].emoji, sizeof ins);
        w = utf8_to_wide(ins, lstrlenA(ins));
        SendMessageW(g_ui.composer, EM_REPLACESEL, TRUE, (LPARAM)w);
        mem_free(w);
        if (GetKeyState(VK_SHIFT) >= 0) /* shift keeps it open to pick several, like Discord */
            picker_close();
    }
    redraw();
}

static LRESULT CALLBACK picker_edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        picker_close();
        return 0;
    }
    if (msg == WM_CHAR && (wp == VK_ESCAPE || wp == VK_RETURN)) {
        if (wp == VK_RETURN)
            for (int i = 0; i < g_ui.npick; i++)
                if (((pick_item_t *)g_ui.pick_items)[i].kind != PI_HEADER) {
                    picker_choose(i);
                    return 0;
                }
        return 0;
    }
    if (msg == WM_MOUSEWHEEL)
        return SendMessageW(g_ui.picker, msg, wp, lp);
    return CallWindowProcW(g_ui.picker_edit_proc, h, msg, wp, lp);
}

static LRESULT CALLBACK picker_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        g_ui.picker_frame = paint_frame(wnd, picker_paint);
        return 0;
    case WM_COMMAND:
        if ((HWND)lp == g_ui.picker_edit && HIWORD(wp) == EN_CHANGE) {
            if (g_ui.picker_tab == TAB_GIFS) {
                SetTimer(wnd, 1, 400, NULL); /* search once typing pauses */
            } else {
                picker_rebuild();
                InvalidateRect(wnd, NULL, FALSE);
            }
        }
        return 0;
    case WM_TIMER: {
        wchar_t q[64];
        sb_t s = {0};
        KillTimer(wnd, 1);
        GetWindowTextW(g_ui.picker_edit, q, 64);
        wide_to_utf8(q, (size_t)lstrlenW(q), &s);
        app_fetch_gifs(s.data ? s.data : "");
        sb_free(&s);
        return 0;
    }
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, GDI(C_INK));
        SetBkColor((HDC)wp, RGB(0x17, 0x18, 0x1B));
        return (LRESULT)g_ui.picker_brush;
    case WM_MOUSEMOVE: {
        int h = picker_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h != g_ui.pick_hover) {
            g_ui.pick_hover = h;
            InvalidateRect(wnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)(g_ui.pick_hover >= 0 ? IDC_HAND : IDC_ARROW)));
            return TRUE;
        }
        break;
    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), h;
        if (g_ui.picker_mode == PICK_COMPOSER && y >= S(10) && y < S(36) && x >= S(12) && x < S(12) + 3 * S(PICK_TAB_W)) {
            g_ui.picker_tab = k_pick_tabs[(x - S(12)) / S(PICK_TAB_W)];
            SetWindowTextW(g_ui.picker_edit, L"");
            SendMessageW(g_ui.picker_edit, EM_SETCUEBANNER, TRUE, (LPARAM)picker_cue());
            KillTimer(wnd, 1); /* clearing the edit armed a search */
            picker_rebuild();
            if (g_ui.picker_tab == TAB_GIFS)
                app_fetch_gifs("");
            InvalidateRect(wnd, NULL, FALSE);
            return 0;
        }
        if (g_ui.picker_tab == TAB_GIFS) {
            gifs_click(x, y);
            return 0;
        }
        h = picker_hit(x, y);
        if (h >= 0)
            picker_choose(h);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int view = S(PICK_H) - S(PICK_FOOT) - S(PICK_TOP), max;
        g_ui.pick_scroll -= GET_WHEEL_DELTA_WPARAM(wp) * S(PICK_CELL) * 3 / WHEEL_DELTA;
        max = g_ui.pick_content - view;
        if (g_ui.pick_scroll > max)
            g_ui.pick_scroll = max;
        if (g_ui.pick_scroll < 0)
            g_ui.pick_scroll = 0;
        g_ui.pick_hover = -1;
        InvalidateRect(wnd, NULL, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

/* ---- GIFs tab ---- */

static void on_gifs(const sb_t *p)
{
    const char *query = p->data;
    wchar_t now[64];
    sb_t cur = {0};

    if (!g_ui.picker || g_ui.picker_tab != TAB_GIFS)
        return;
    GetWindowTextW(g_ui.picker_edit, now, 64);
    wide_to_utf8(now, (size_t)lstrlenW(now), &cur);
    if (lstrcmpA(query, cur.data ? cur.data : "") == 0) { /* ignore answers to older queries */
        size_t n = p->len - (size_t)lstrlenA(query) - 1;
        sb_clear(&g_ui.gif_json);
        sb_addn(&g_ui.gif_json, p->data + lstrlenA(query) + 1, n);
        g_ui.pick_scroll = 0;
        InvalidateRect(g_ui.picker, NULL, FALSE);
    }
    sb_free(&cur);
}

/* Two columns of GIFs, each as tall as its aspect ratio asks. */
static void gifs_paint(int w, int h)
{
    json_t arr, g, v;
    json_iter_t it;
    int col_w = (w - S(36)) / 2, colh[2] = {S(PICK_TOP) - S(36), S(PICK_TOP) - S(36)}, bottom = h - S(8);

    g_ui.ngif = 0;
    if (!g_ui.gif_json.len || !json_parse(g_ui.gif_json.data, g_ui.gif_json.len, &arr) || json_type(arr) != JSON_ARRAY) {
        char msg[48];
        if (!g_ui.gif_json.len)
            lstrcpyA(msg, "Loading GIFs\xE2\x80\xA6");
        else if (g_ui.gif_json.data[0] == '!')
            wsprintfA(msg, "Couldn't load GIFs (%s)", g_ui.gif_json.data + 1);
        else
            lstrcpyA(msg, "No GIFs found");
        text(g_ui.f_body, C_MUTED, rect(0, h / 2, w, S(24)), msg, DT_CENTER | DT_SINGLELINE);
        return;
    }
    r_clip(0, S(PICK_TOP) - S(8), w, bottom - S(PICK_TOP) + S(8));
    json_iter(arr, &it);
    while (g_ui.ngif < 40 && json_next(&it, NULL, &g)) {
        long long gw = 1, gh = 1;
        int c = colh[0] <= colh[1] ? 0 : 1, x = S(12) + c * (col_w + S(12)), y = colh[c] - g_ui.pick_scroll, ch;
        sb_t src = {0};
        char key[64], id[32] = "";
        if (json_get(g, "width", &v))
            json_int(v, &gw);
        if (json_get(g, "height", &v))
            json_int(v, &gh);
        ch = gw > 0 ? (int)(col_w * gh / gw) : col_w;
        if (ch > col_w * 2)
            ch = col_w * 2;
        g_ui.gif_x[g_ui.ngif] = x;
        g_ui.gif_y[g_ui.ngif] = colh[c];
        g_ui.gif_w[g_ui.ngif] = col_w;
        g_ui.gif_h[g_ui.ngif] = ch;
        if (y + ch > S(PICK_TOP) - S(8) && y < bottom && r_visible(y, ch)) {
            r_image_t *img = NULL;
            if (json_get(g, "id", &v))
                json_raw(v, id, sizeof id);
            if (json_get(g, "src", &v))
                json_str(v, &src);
            wsprintfA(key, "gif:%s", id);
            if (src.len)
                img = image_get(key, src.data, col_w > ch ? col_w : ch);
            if (img)
                r_image_cover(img, x, y, col_w, ch, S(6));
            else
                r_round(x, y, col_w, ch, S(6), 0xFF17181B);
        }
        sb_free(&src);
        colh[c] += ch + S(8);
        g_ui.ngif++;
    }
    g_ui.pick_content = (colh[0] > colh[1] ? colh[0] : colh[1]) - (S(PICK_TOP) - S(36));
    r_unclip();
}

static void gifs_click(int x, int y)
{
    json_t arr, g, v;
    json_iter_t it;
    int k = 0;

    if (!g_ui.gif_json.len || !json_parse(g_ui.gif_json.data, g_ui.gif_json.len, &arr))
        return;
    json_iter(arr, &it);
    while (k < g_ui.ngif && json_next(&it, NULL, &g)) {
        int top = g_ui.gif_y[k] - g_ui.pick_scroll;
        if (x >= g_ui.gif_x[k] && x < g_ui.gif_x[k] + g_ui.gif_w[k] && y >= top && y < top + g_ui.gif_h[k] &&
            y >= S(PICK_TOP) - S(8)) {
            /* Discord sends the GIF's page link; the embed shows it. */
            sb_t url = {0};
            if (json_get(g, "url", &v))
                json_str(v, &url);
            if (url.len && open_is_text())
                send_picked(url.data, NULL);
            sb_free(&url);
            picker_close();
            return;
        }
        k++;
    }
}

/* Opens the picker with its bottom-right corner at (right, bottom), clamped to the window. */
static void picker_open(int mode, const char *msg_id, int right, int bottom)
{
    RECT rc;
    int w = S(PICK_W), h = S(PICK_H), x, y;
    HFONT font;

    picker_close();
    pop_dismiss();
    GetClientRect(g_ui.wnd, &rc);
    x = right - w;
    y = bottom - h;
    if (x < S(8))
        x = S(8);
    if (x + w > rc.right - S(8))
        x = rc.right - S(8) - w;
    if (y < S(8))
        y = S(8);
    if (y + h > rc.bottom - S(8))
        y = rc.bottom - S(8) - h;
    g_ui.picker_mode = mode;
    lstrcpynA(g_ui.picker_msg, msg_id ? msg_id : "", sizeof g_ui.picker_msg);
    if (mode != PICK_COMPOSER)
        g_ui.picker_tab = TAB_EMOJI;
    if (!g_ui.picker_brush)
        g_ui.picker_brush = CreateSolidBrush(RGB(0x17, 0x18, 0x1B));
    g_ui.picker = CreateWindowExW(0, L"SilicordEmoji", L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, x, y, w, h,
                                  g_ui.wnd, NULL, NULL, NULL);
    g_ui.picker_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, S(22), S(PICK_TABS) + S(6),
                                       w - S(44), S(20), g_ui.picker, NULL, NULL, NULL);
    g_ui.picker_edit_proc = (WNDPROC)SetWindowLongPtrW(g_ui.picker_edit, GWLP_WNDPROC, (LONG_PTR)picker_edit_proc);
    font = (HFONT)SendMessageW(g_ui.composer, WM_GETFONT, 0, 0);
    SendMessageW(g_ui.picker_edit, WM_SETFONT, (WPARAM)font, FALSE);
    SendMessageW(g_ui.picker_edit, EM_SETCUEBANNER, TRUE, (LPARAM)picker_cue());
    picker_rebuild();
    if (g_ui.picker_tab == TAB_GIFS)
        app_fetch_gifs("");
    fade_in(g_ui.picker);
    SetWindowPos(g_ui.picker, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    SetFocus(g_ui.picker_edit);
}

/* ---- Files waiting to be sent ---- */

#define UPLOAD_MAX 10
#define UPLOAD_LIMIT (10ll * 1024 * 1024) /* Discord's limit without Nitro */
#define TRAY_H 132
#define CARD 104

static int tray_h(void)
{
    return g_ui.nuploads ? S(TRAY_H) : 0;
}

static void uploads_clear(void)
{
    for (int i = 0; i < g_ui.nuploads; i++) {
        sb_free(&g_ui.uploads[i].path);
        r_image_free(g_ui.uploads[i].thumb);
    }
    g_ui.nuploads = 0;
}

static void upload_remove(int i)
{
    sb_free(&g_ui.uploads[i].path);
    r_image_free(g_ui.uploads[i].thumb);
    for (int k = i; k < g_ui.nuploads - 1; k++)
        g_ui.uploads[k] = g_ui.uploads[k + 1];
    g_ui.nuploads--;
}

/* Queues a file to send with the next message. */
static void upload_add(const wchar_t *path)
{
    HANDLE f;
    LARGE_INTEGER size;
    upload_t *u;

    if (!open_is_text())
        return;
    if (g_ui.nuploads == UPLOAD_MAX) {
        set_text(&g_ui.send_error, "You can send up to 10 files at a time");
        return;
    }
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    if (!GetFileSizeEx(f, &size) || size.QuadPart > UPLOAD_LIMIT) {
        CloseHandle(f);
        set_text(&g_ui.send_error, "Files can be up to 10 MB");
        return;
    }
    u = &g_ui.uploads[g_ui.nuploads++];
    *u = (upload_t){0};
    wide_to_utf8(path, (size_t)lstrlenW(path), &u->path);
    u->size = size.QuadPart;
    /* A preview for pictures, decoded small. */
    if (size.QuadPart < 8 * 1024 * 1024) {
        sb_t data = {0};
        DWORD got = 0;
        sb_reserve(&data, (size_t)size.QuadPart);
        if (ReadFile(f, data.data, (DWORD)size.QuadPart, &got, NULL))
            u->thumb = r_image_decode(data.data, got, S(CARD));
        sb_free(&data);
    }
    CloseHandle(f);
    sb_clear(&g_ui.send_error);
}

static const char *upload_name(const upload_t *u)
{
    const char *name = u->path.data;

    for (const char *p = u->path.data; *p; p++)
        if (*p == '\\' || *p == '/')
            name = p + 1;
    return name;
}

static void paint_tray(int x0, int w, int bottom)
{
    int y = bottom - S(TRAY_H), x = x0 + S(28);

    if (!g_ui.nuploads)
        return;
    r_round(x0 + S(COMPOSER_GAP), y, w - 2 * S(COMPOSER_GAP), S(TRAY_H) + S(10), S(8), 0xFF121214u);
    for (int i = 0; i < g_ui.nuploads; i++, x += S(CARD) + S(12)) {
        const upload_t *u = &g_ui.uploads[i];
        int cy = y + S(12);
        r_round(x, cy, S(CARD), S(CARD), S(8), 0xFF1E1E20);
        if (u->thumb)
            r_image_cover(u->thumb, x + S(8), cy + S(8), S(CARD) - S(16), S(CARD) - S(40), S(6));
        else
            text_w(g_ui.f_icon_big, C_MUTED, rect(x, cy + S(8), S(CARD), S(CARD) - S(40)), L"\xE8A5", -1,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(g_ui.f_small, C_INK, rect(x + S(8), cy + S(CARD) - S(28), S(CARD) - S(16), S(20)), upload_name(u),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        /* remove button */
        r_circle(x + S(CARD) - S(22), cy - S(6), S(26), g_ui.upload_hover == i ? 0xFFC0363Au : 0xFF242426);
        text_w(g_ui.f_icon, C_INK, rect(x + S(CARD) - S(22), cy - S(6), S(26), S(26)), L"\xE74D", -1,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

/* Remove button of card i under (x, y). */
static int tray_hit(int x, int y)
{
    RECT rc;
    int x0 = S(RAIL_W + SIDE_W), top, cx;

    if (!g_ui.nuploads)
        return -1;
    GetClientRect(g_ui.wnd, &rc);
    top = rc.bottom - S(COMPOSER_GAP) - S(COMPOSER_H) - (g_ui.bar ? S(BAR_H) : 0) - S(TRAY_H) + S(12);
    cx = x0 + S(28);
    for (int i = 0; i < g_ui.nuploads; i++, cx += S(CARD) + S(12))
        if (x >= cx + S(CARD) - S(22) && x < cx + S(CARD) + S(4) && y >= top - S(6) && y < top + S(20))
            return i;
    return -1;
}

static void pick_files(void)
{
    enum { N = 8192 };
    wchar_t *buf = mem_alloc(N * sizeof(wchar_t)); /* on the heap: no __chkstk without the CRT */
    OPENFILENAMEW ofn = {sizeof ofn};

    ofn.hwndOwner = g_ui.wnd;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = N;
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) {
        mem_free(buf);
        return;
    }
    /* One file: a full path. Several: the folder, then names, NUL-separated. */
    if (!buf[lstrlenW(buf) + 1]) {
        upload_add(buf);
    } else {
        wchar_t *path = mem_alloc(MAX_PATH * 4 * sizeof(wchar_t));
        for (wchar_t *name = buf + lstrlenW(buf) + 1; *name; name += lstrlenW(name) + 1) {
            wsprintfW(path, L"%s\\%s", buf, name);
            upload_add(path);
        }
        mem_free(path);
    }
    mem_free(buf);
    place_composer();
    clamp_msg_scroll();
    redraw();
}

/* Queues the files of a drop, dragged in or copied in Explorer; returns how many it holds. */
static UINT upload_drop(HDROP drop)
{
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
    wchar_t *path = mem_alloc(MAX_PATH * 4 * sizeof(wchar_t));

    for (UINT i = 0; i < n; i++)
        if (DragQueryFileW(drop, i, path, MAX_PATH * 4))
            upload_add(path);
    mem_free(path);
    return n;
}

static void on_drop(HDROP drop)
{
    upload_drop(drop);
    DragFinish(drop);
    place_composer();
    clamp_msg_scroll();
    redraw();
}

/* Pasted pictures become %TEMP%\Silicord-paste\<n>\image.png, named like Discord names them. */
static void paste_dir(wchar_t *out, unsigned n)
{
    DWORD len = GetTempPathW(MAX_PATH, out);

    /* Leave room for "Silicord-paste\<number>\image.png" within MAX_PATH, as callers assume. */
    if (!len || len >= MAX_PATH - 40)
        out[0] = 0;
    lstrcatW(out, L"Silicord-paste");
    if (n)
        wsprintfW(out + lstrlenW(out), L"\\%u", n);
}

/* Drops the pictures pasted in earlier runs: uploads read them while sending, so they stay until then. */
static void paste_cleanup(void)
{
    wchar_t *dir = mem_alloc(MAX_PATH * 3 * sizeof(wchar_t)), *path = dir + MAX_PATH;
    WIN32_FIND_DATAW fd;
    HANDLE find;

    paste_dir(dir, 0);
    lstrcpyW(path, dir);
    lstrcatW(path, L"\\*");
    find = FindFirstFileW(path, &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.')
                continue;
            wsprintfW(path, L"%s\\%s\\image.png", dir, fd.cFileName);
            DeleteFileW(path);
            wsprintfW(path, L"%s\\%s", dir, fd.cFileName);
            RemoveDirectoryW(path);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(dir);
    mem_free(dir);
}

/* Ctrl+V of files or a picture adds them to the uploads, like Discord. Returns 1 when handled. */
static int paste_files(void)
{
    static unsigned count;
    int done = 0;

    if (!open_is_text() || !OpenClipboard(g_ui.wnd))
        return 0;
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        HDROP drop = (HDROP)GetClipboardData(CF_HDROP);
        done = drop && upload_drop(drop) > 0;
    } else if (!IsClipboardFormatAvailable(CF_UNICODETEXT) && IsClipboardFormatAvailable(CF_BITMAP)) {
        HBITMAP bmp = (HBITMAP)GetClipboardData(CF_BITMAP);
        wchar_t *path = mem_alloc(MAX_PATH * 2 * sizeof(wchar_t));
        paste_dir(path, 0);
        CreateDirectoryW(path, NULL);
        paste_dir(path, GetTickCount() + ++count);
        CreateDirectoryW(path, NULL);
        lstrcatW(path, L"\\image.png");
        if (bmp && r_bitmap_to_png(bmp, path))
            upload_add(path);
        else
            DeleteFileW(path);
        mem_free(path);
        done = 1;
    }
    CloseClipboard();
    if (done) {
        place_composer();
        clamp_msg_scroll();
        redraw();
    }
    return done;
}

/* ---- Autocomplete: @members, #channels, :emoji: ---- */


static int ci_contains(const char *hay, const char *needle)
{
    if (!*needle)
        return 1;
    for (; *hay; hay++) {
        int k = 0;
        while (needle[k] && hay[k] && (hay[k] | 32) == (needle[k] | 32))
            k++;
        if (!needle[k])
            return 1;
    }
    return 0;
}

static int ac_has(const char *id)
{
    for (int i = 0; i < g_ui.ac_n; i++)
        if (lstrcmpA(g_ui.ac[i].id, id) == 0)
            return 1;
    return 0;
}

/* A blank suggestion at the end of the list; callers check there is room. */
static ac_item_t *ac_new(void)
{
    ac_item_t *it = &g_ui.ac[g_ui.ac_n++];

    *it = (ac_item_t){0};
    it->emoji = -1;
    return it;
}

static void ac_add_user(const char *id, const char *name, const char *avatar)
{
    ac_item_t *it;

    if (g_ui.ac_n == AC_MAX || !id[0] || !name || !name[0] || ac_has(id) || !ci_contains(name, g_ui.ac_query))
        return;
    it = ac_new();
    lstrcpynA(it->id, id, sizeof it->id);
    lstrcpynA(it->label, name, sizeof it->label);
    wsprintfA(it->insert, "@%.80s ", name);
    lstrcpynA(it->avatar, avatar ? avatar : "", sizeof it->avatar);
}

/* ---- Slash commands ---- */

/* `s` starts with `prefix`, ignoring case. */
static int starts_ci(const char *s, const char *prefix)
{
    int n = lstrlenA(prefix);

    return n <= lstrlenA(s) && CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, s, n, prefix, n) == CSTR_EQUAL;
}

/* Discord's own commands, done by the client before sending. */
static const struct {
    const char *name, *description;
} k_builtin[] = {
    {"shrug", "Appends \xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF to your message."},
    {"tableflip", "Appends (\xE2\x95\xAF\xC2\xB0\xE2\x96\xA1\xC2\xB0)\xE2\x95\xAF\xEF\xB8\xB5 \xE2\x94\xBB\xE2\x94\x81\xE2\x94\xBB to your message."},
    {"unflip", "Appends \xE2\x94\xAC\xE2\x94\x80\xE2\x94\xAC\xE3\x83\x8E( \xC2\xBA _ \xC2\xBA\xE3\x83\x8E) to your message."},
    {"me", "Displays text with emphasis."},
    {"spoiler", "Marks your message as a spoiler."},
};

/* Rewrites "/shrug hi" and the like into the message Discord would send. Returns 0 for anything else. */
static int builtin_rewrite(const char *s, size_t n, sb_t *out)
{
    static const char *const tails[] = {
        "\xC2\xAF\\\\\\_(\xE3\x83\x84)\\_/\xC2\xAF", /* escaped so markdown keeps it */
        "(\xE2\x95\xAF\xC2\xB0\xE2\x96\xA1\xC2\xB0)\xE2\x95\xAF\xEF\xB8\xB5 \xE2\x94\xBB\xE2\x94\x81\xE2\x94\xBB",
        "\xE2\x94\xAC\xE2\x94\x80\xE2\x94\xAC\xE3\x83\x8E( \xC2\xBA _ \xC2\xBA\xE3\x83\x8E)",
    };

    for (int k = 0; k < (int)ARRAYSIZE(k_builtin); k++) {
        size_t len = (size_t)lstrlenA(k_builtin[k].name), r;
        if (n < len + 1 || (n > len + 1 && s[len + 1] != ' ') ||
            CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, s + 1, (int)len, k_builtin[k].name, (int)len) != CSTR_EQUAL)
            continue;
        for (r = len + 1; r < n && s[r] == ' '; r++)
            ;
        if (k < 3) {
            sb_addn(out, s + r, n - r);
            if (n > r)
                sb_add(out, " ");
            sb_add(out, tails[k]);
        } else {
            if (n == r)
                return 0;
            sb_add(out, k == 3 ? "_" : "||");
            sb_addn(out, s + r, n - r);
            sb_add(out, k == 3 ? "_" : "||");
        }
        return 1;
    }
    return 0;
}

static const char *cmd_scope(void)
{
    const char *guild = open_guild_id();

    return guild ? guild : g_ui.msgs_channel;
}

/* Asks for the commands of the open server or DM once; UI_COMMANDS brings them. */
static void commands_ensure(void)
{
    const char *key = cmd_scope();

    if (!key[0] || (lstrcmpA(g_ui.cmd_key, key) == 0 && (g_ui.cmd_index.len || g_ui.cmd_loading)))
        return;
    lstrcpynA(g_ui.cmd_key, key, sizeof g_ui.cmd_key);
    sb_clear(&g_ui.cmd_index);
    g_ui.cmd_loading = 1;
    app_fetch_commands(open_guild_id(), open_guild_id() ? NULL : g_ui.msgs_channel);
}

static void on_commands(const sb_t *p)
{
    const char *key = p->data;
    size_t n = (size_t)lstrlenA(key) + 1;

    if (lstrcmpA(key, g_ui.cmd_key) != 0)
        return;
    g_ui.cmd_loading = 0;
    sb_clear(&g_ui.cmd_index);
    if (p->len > n)
        sb_addn(&g_ui.cmd_index, p->data + n, p->len - n);
    else
        sb_add(&g_ui.cmd_index, "{}"); /* nothing usable: don't ask again */
    ac_update();
}

static void ac_add_command(json_t index, json_t cmd, const char *label, const char *insert)
{
    ac_item_t *it;
    json_t v, app;
    sb_t s = {0};

    if (g_ui.ac_n == AC_MAX)
        return;
    it = ac_new();
    lstrcpynA(it->label, label, sizeof it->label);
    lstrcpynA(it->insert, insert, sizeof it->insert);
    if (json_get(cmd, "description", &v) && json_str(v, &s))
        lstrcpynA(it->detail, s.data, sizeof it->detail);
    sb_free(&s);
    if (json_get(cmd, "application_id", &v)) {
        json_raw(v, it->id, sizeof it->id);
        if (cmd_app(index, it->id, &app)) {
            if (json_get(app, "name", &v) && json_str(v, &s))
                lstrcpynA(it->app, s.data, sizeof it->app);
            sb_free(&s);
            if (json_get(app, "icon", &v) && json_type(v) == JSON_STRING)
                json_raw(v, it->avatar, sizeof it->avatar);
        }
    }
}

/*
 * Suggestions while a slash command is typed: the commands for the first word,
 * then its subcommands or options. Returns 0 when the text is no command of ours.
 */
static int ac_commands(const wchar_t *w, int end)
{
    sb_t s = {0};
    json_t index, cmd, v;
    size_t sp, word;
    char q[64];

    commands_ensure();
    g_ui.ac_n = 0;
    if (!g_ui.cmd_index.len || !json_parse(g_ui.cmd_index.data, g_ui.cmd_index.len, &index))
        json_parse("{}", 2, &index); /* loading: the built-in ones only */
    wide_to_utf8(w, (size_t)end, &s);
    for (sp = 1; sp < s.len && s.data[sp] != ' ' && s.data[sp] != '\n'; sp++)
        ;
    /* The word being typed, and where it starts in the composer (UTF-16). */
    for (word = s.len; word > 0 && s.data[word - 1] != ' '; word--)
        ;
    lstrcpynA(q, s.data ? s.data + word : "", sizeof q);
    g_ui.ac_end = end;
    g_ui.ac_start = end - MultiByteToWideChar(CP_UTF8, 0, s.data + word, (int)(s.len - word), NULL, 0);
    if (sp == s.len) {
        json_iter_t it = {0};
        g_ui.ac_kind = AC_COMMAND;
        lstrcpynA(g_ui.ac_query, q + 1, sizeof g_ui.ac_query);
        for (int k = 0; k < (int)ARRAYSIZE(k_builtin) && g_ui.ac_n < AC_MAX; k++) {
            ac_item_t *b;
            if (!starts_ci(k_builtin[k].name, q + 1))
                continue;
            b = ac_new();
            wsprintfA(b->label, "/%s", k_builtin[k].name);
            wsprintfA(b->insert, "/%s ", k_builtin[k].name);
            lstrcpynA(b->detail, k_builtin[k].description, sizeof b->detail);
            lstrcpyA(b->app, "Built-in");
        }
        while (g_ui.ac_n < AC_MAX && cmd_next(index, &it, &cmd)) {
            char name[40], label[48], insert[48];
            if (!json_get(cmd, "name", &v))
                continue;
            json_raw(v, name, sizeof name);
            if (!starts_ci(name, q + 1))
                continue;
            wsprintfA(label, "/%s", name);
            wsprintfA(insert, "/%s ", name);
            ac_add_command(index, cmd, label, insert);
        }
    } else if (cmd_find(index, s.data + 1, sp - 1, &cmd)) {
        json_t opts, o;
        json_iter_t it;
        size_t used;
        const char *args = s.data + sp + 1;
        size_t alen = s.len - sp - 1;
        int leaf = cmd_leaf(cmd, args, word > sp ? word - sp - 1 : 0, &opts, &used);
        g_ui.ac_kind = AC_OPTION;
        if (opts.p && !find_str(q, ":")) {
            json_iter(opts, &it);
            while (g_ui.ac_n < AC_MAX && json_next(&it, NULL, &o)) {
                char name[40], insert[48], mark[44];
                if (!json_get(o, "name", &v))
                    continue;
                json_raw(v, name, sizeof name);
                if (!starts_ci(name, q))
                    continue;
                wsprintfA(mark, "%s:", name);
                if (leaf && alen && find_str(args, mark)) /* already given */
                    continue;
                wsprintfA(insert, leaf ? "%s:" : "%s ", name);
                ac_add_command(index, o, name, insert);
                {
                    ac_item_t *it2 = &g_ui.ac[g_ui.ac_n - 1];
                    it2->id[0] = it2->avatar[0] = 0;
                    lstrcpyA(it2->app, leaf && json_get(o, "required", &v) && json_type(v) == JSON_TRUE ? "required" : "");
                }
            }
        }
    } else {
        sb_free(&s);
        return 0;
    }
    sb_free(&s);
    return 1;
}

/* Sends the composer's "/command ..." as an interaction. Returns 0 when it is no known command. */
static int send_command(const char *s, size_t n)
{
    json_t index, cmd, v;
    size_t sp;
    sb_t data = {0};
    char err[128], app[24] = "";

    if (g_ui.cmd_loading && lstrcmpA(g_ui.cmd_key, cmd_scope()) == 0) {
        set_text(&g_ui.send_error, "Commands are still loading");
        redraw();
        return 1;
    }
    if (!g_ui.cmd_index.len || lstrcmpA(g_ui.cmd_key, cmd_scope()) != 0 ||
        !json_parse(g_ui.cmd_index.data, g_ui.cmd_index.len, &index))
        return 0;
    for (sp = 1; sp < n && s[sp] != ' ' && s[sp] != '\n'; sp++)
        ;
    if (!cmd_find(index, s + 1, sp - 1, &cmd))
        return 0;
    if (!cmd_build(cmd, s + sp, n - sp, &data, err, sizeof err)) {
        set_text(&g_ui.send_error, err);
    } else {
        if (json_get(cmd, "application_id", &v))
            json_raw(v, app, sizeof app);
        app_run_command(open_guild_id(), g_ui.msgs_channel, app, data.data);
        sb_clear(&g_ui.send_error);
        SetWindowTextW(g_ui.composer, L"");
        g_ui.nmention = 0;
        g_ui.msg_scroll = 0;
    }
    sb_free(&data);
    redraw();
    return 1;
}

/* Finds the word being typed before the caret; returns its kind and fills g_ui.ac. */
static void ac_update(void)
{
    int len = GetWindowTextLengthW(g_ui.composer);
    DWORD start = 0, end = 0;
    wchar_t *w;
    int k;

    g_ui.ac_kind = AC_NONE;
    g_ui.ac_n = 0;
    if (len <= 0 || len > 4000)
        return;
    w = mem_alloc(((size_t)len + 1) * sizeof(wchar_t));
    GetWindowTextW(g_ui.composer, w, len + 1);
    SendMessageW(g_ui.composer, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    if ((int)end > len)
        end = (DWORD)len;
    if (w[0] == '/' && ac_commands(w, (int)end)) {
        mem_free(w);
        if (!g_ui.ac_n)
            g_ui.ac_kind = AC_NONE;
        if (g_ui.ac_sel >= g_ui.ac_n)
            g_ui.ac_sel = 0;
        return;
    }
    for (k = (int)end - 1; k >= 0 && w[k] != ' ' && w[k] != '\n' && w[k] != '@' && w[k] != '#' && w[k] != ':'; k--)
        ;
    if (k >= 0 && (w[k] == '@' || w[k] == '#' || w[k] == ':') && (k == 0 || w[k - 1] == ' ' || w[k - 1] == '\n')) {
        int qn = (int)end - k - 1;
        /* End at the bytes written: a UTF-16 count cuts non-ASCII queries (0 when too long). */
        int nb = qn ? WideCharToMultiByte(CP_UTF8, 0, w + k + 1, qn, g_ui.ac_query, sizeof g_ui.ac_query - 1, NULL, NULL) : 0;
        g_ui.ac_query[nb] = 0;
        g_ui.ac_start = k;
        g_ui.ac_end = (int)end;
        g_ui.ac_kind = w[k] == '@' ? AC_USER : w[k] == '#' ? AC_CHANNEL : AC_EMOJI;
        if (g_ui.ac_kind == AC_EMOJI && qn < 2)
            g_ui.ac_kind = AC_NONE; /* ":" alone is punctuation, Discord waits for two letters */
    }
    mem_free(w);

    if (g_ui.ac_kind == AC_USER) {
        /* Recent authors first, then the member list. */
        for (int i = g_ui.nmsgs; i-- > 0 && g_ui.ac_n < AC_MAX;)
            if (!g_ui.msgs[i].system)
                ac_add_user(g_ui.msgs[i].author_id, author_name(&g_ui.msgs[i]), g_ui.msgs[i].avatar);
        for (int i = 0; i < g_ui.ml.n && g_ui.ac_n < AC_MAX; i++)
            if (g_ui.ml.items[i].valid && !g_ui.ml.items[i].group)
                ac_add_user(g_ui.ml.items[i].id, g_ui.ml.items[i].name.data, g_ui.ml.items[i].avatar);
        if (g_ui.channel >= 0 && chan(g_ui.channel)->type == CH_DM)
            ac_add_user(chan(g_ui.channel)->user_id, model_str(g_ui.model, chan(g_ui.channel)->name),
                        chan(g_ui.channel)->avatar);
    } else if (g_ui.ac_kind == AC_CHANNEL && g_ui.guild >= 0) {
        const guild_t *gd = &g_ui.model->guilds[g_ui.guild];
        for (unsigned c = gd->first; c < gd->first + gd->count && g_ui.ac_n < AC_MAX; c++) {
            const channel_t *ch = chan((int)c);
            const char *name = model_str(g_ui.model, ch->name);
            ac_item_t *it;
            if (ch->type == CH_CATEGORY || !ci_contains(name, g_ui.ac_query))
                continue;
            it = ac_new();
            lstrcpynA(it->id, ch->id, sizeof it->id);
            lstrcpynA(it->label, name, sizeof it->label);
            wsprintfA(it->insert, "#%.80s ", name);
        }
    } else if (g_ui.ac_kind == AC_EMOJI) {
        model_emoji_t e;
        unsigned cursor = 0;
        while (g_ui.guild >= 0 && g_ui.ac_n < AC_MAX && model_emoji_next(g_ui.model, g_ui.guild, &cursor, &e)) {
            char name[48];
            ac_item_t *it;
            lstrcpynA(name, e.name, e.name_len + 1 < (int)sizeof name ? e.name_len + 1 : (int)sizeof name);
            if (!ci_contains(name, g_ui.ac_query))
                continue;
            it = ac_new();
            lstrcpynA(it->custom, e.id, sizeof it->custom);
            wsprintfA(it->label, ":%s:", name);
            wsprintfA(it->insert, ":%s: ", name);
        }
        for (int pass = 2; pass >= 1; pass--) /* names starting with the query first */
        for (int i = 0; i < k_nemoji && g_ui.ac_n < AC_MAX; i++) {
            ac_item_t *it;
            char name[48];
            if (emoji_matches(i, g_ui.ac_query) != pass)
                continue;
            it = ac_new();
            it->emoji = i;
            emoji_main_name(i, name, sizeof name);
            wsprintfA(it->label, ":%s:", name);
            wsprintfA(it->insert, "%s ", k_emoji[i].emoji);
        }
    }
    if (!g_ui.ac_n)
        g_ui.ac_kind = AC_NONE;
    if (g_ui.ac_sel >= g_ui.ac_n)
        g_ui.ac_sel = 0;
}

static RECT ac_rect(void)
{
    RECT rc;
    int x0 = S(RAIL_W + SIDE_W), w = main_right() - x0, h = g_ui.ac_n * S(AC_ROW) + S(40);
    int bottom;

    GetClientRect(g_ui.wnd, &rc);
    bottom = rc.bottom - S(COMPOSER_GAP) - S(COMPOSER_H) - (g_ui.bar ? S(BAR_H) : 0) - tray_h() - S(8);
    return rect(x0 + S(16), bottom - h, w - S(32), h);
}

static void paint_autocomplete(void)
{
    RECT r;
    const char *title;

    if (g_ui.ac_kind == AC_NONE)
        return;
    r = ac_rect();
    r_round(r.left, r.top, r.right - r.left, r.bottom - r.top, S(8), 0xFF151515);
    r_round_outline(r.left, r.top, r.right - r.left, r.bottom - r.top, S(8), 1, 0xFF242426);
    title = g_ui.ac_kind == AC_USER      ? "MEMBERS"
            : g_ui.ac_kind == AC_CHANNEL ? "TEXT CHANNELS"
            : g_ui.ac_kind == AC_COMMAND ? "COMMANDS"
            : g_ui.ac_kind == AC_OPTION  ? "OPTIONS"
                                         : "EMOJI MATCHING";
    text(g_ui.f_cat, C_MUTED, rect(r.left + S(16), r.top + S(8), S(300), S(24)), title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    for (int i = 0; i < g_ui.ac_n; i++) {
        const ac_item_t *it = &g_ui.ac[i];
        int y = r.top + S(36) + i * S(AC_ROW), x = r.left + S(8);
        if (i == g_ui.ac_sel)
            r_round(x, y, r.right - r.left - S(16), S(AC_ROW) - S(2), S(6), ARGB(C_SELECT));
        if (g_ui.ac_kind == AC_USER) {
            r_image_t *img = user_avatar(it->id, it->avatar);
            if (img)
                r_image(img, x + S(8), y + S(7), S(24), S(24), S(12));
            else
                r_circle(x + S(8), y + S(7), S(24), ARGB(C_ITEM));
        } else if (g_ui.ac_kind == AC_CHANNEL) {
            sicon(SI_HASH, x + S(10), y + (S(AC_ROW) - S(20)) / 2, S(20), ARGB(C_CHANNEL));
        } else if (g_ui.ac_kind == AC_COMMAND || g_ui.ac_kind == AC_OPTION) {
            r_image_t *img = NULL;
            int lw = text_width(g_ui.f_body, it->label), right = it->app[0] ? text_width(g_ui.f_small, it->app) + S(16) : 0;
            if (it->avatar[0]) {
                char key[64], path[128];
                wsprintfA(key, "app:%s", it->id);
                wsprintfA(path, "/app-icons/%s/%s.png?size=64", it->id, it->avatar);
                img = image_get(key, path, S(24));
            }
            if (img)
                r_image(img, x + S(8), y + S(7), S(24), S(24), S(12));
            else
                text(g_ui.f_h, C_FAINT, rect(x + S(8), y, S(24), S(AC_ROW)), "/", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            text(g_ui.f_body, C_INK, rect(x + S(44), y, lw + S(4), S(AC_ROW)), it->label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            text(g_ui.f_small, C_MUTED, rect(x + S(56) + lw, y, r.right - x - S(72) - lw - right, S(AC_ROW)), it->detail,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (right)
                text(g_ui.f_small, C_FAINT, rect(r.right - S(16) - right, y, right - S(8), S(AC_ROW)), it->app,
                     DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            continue;
        } else if (it->custom[0]) {
            r_image_t *img = emoji_image(it->custom, S(24));
            if (img)
                r_image(img, x + S(8), y + S(7), S(24), S(24), 0);
        } else {
            wchar_t *we = utf8_to_wide(k_emoji[it->emoji].emoji, lstrlenA(k_emoji[it->emoji].emoji));
            r_text(g_ui.f_body, ARGB(C_INK), x + S(4), y, S(32), S(AC_ROW), we, -1, R_CENTER | R_VCENTER | R_SINGLE);
            mem_free(we);
        }
        text(g_ui.f_body, C_INK, rect(x + S(44), y, r.right - x - S(60), S(AC_ROW)), it->label,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

/* Replaces the word being typed with suggestion i and remembers what a mention stands for. */
static void ac_accept(int i)
{
    /*
     * Replacing the text sends EN_CHANGE right away, and the suggestions are
     * updated from it (the caret now follows a space: none): copy what we
     * need from the item and the state first.
     */
    int kind = g_ui.ac_kind, command = kind == AC_COMMAND || kind == AC_OPTION;
    char insert[sizeof g_ui.ac[0].insert], id[sizeof g_ui.ac[0].id];
    wchar_t *w;

    lstrcpynA(insert, g_ui.ac[i].insert, sizeof insert);
    lstrcpynA(id, g_ui.ac[i].id, sizeof id);
    w = utf8_to_wide(insert, lstrlenA(insert));
    SendMessageW(g_ui.composer, EM_SETSEL, (WPARAM)g_ui.ac_start, (LPARAM)g_ui.ac_end);
    SendMessageW(g_ui.composer, EM_REPLACESEL, TRUE, (LPARAM)w);
    mem_free(w);
    if ((kind == AC_USER || kind == AC_CHANNEL) && insert[0] && g_ui.nmention < (int)ARRAYSIZE(g_ui.mention)) {
        mention_t *mn = &g_ui.mention[g_ui.nmention++];
        lstrcpynA(mn->text, insert, sizeof mn->text);
        mn->text[lstrlenA(mn->text) - 1] = 0; /* without the space */
        wsprintfA(mn->markup, kind == AC_USER ? "<@%s>" : "<#%s>", id);
    }
    g_ui.ac_kind = AC_NONE;
    g_ui.ac_n = 0;
    if (command) /* a command's options come next */
        ac_update();
    redraw();
}

/* "@name" and "#channel" picked from the suggestions become real mentions. */
static void apply_mentions(const char *s, size_t len, sb_t *out)
{
    size_t i = 0;

    while (i < len) {
        int best = -1;
        size_t best_len = 0;
        for (int k = 0; k < g_ui.nmention; k++) {
            size_t n = (size_t)lstrlenA(g_ui.mention[k].text);
            if (n > best_len && i + n <= len &&
                CompareStringA(LOCALE_INVARIANT, 0, s + i, (int)n, g_ui.mention[k].text, (int)n) == CSTR_EQUAL) {
                best = k;
                best_len = n;
            }
        }
        if (best >= 0) {
            sb_add(out, g_ui.mention[best].markup);
            i += best_len;
        } else {
            sb_addn(out, s + i, 1);
            i++;
        }
    }
}

/* Keys for the suggestion list while it is open; returns 1 when used. */
static int ac_key(WPARAM key)
{
    if (g_ui.ac_kind == AC_NONE)
        return 0;
    switch (key) {
    case VK_UP:
        g_ui.ac_sel = (g_ui.ac_sel + g_ui.ac_n - 1) % g_ui.ac_n;
        break;
    case VK_DOWN:
        g_ui.ac_sel = (g_ui.ac_sel + 1) % g_ui.ac_n;
        break;
    case VK_RETURN:
        if (g_ui.ac_kind == AC_OPTION)
            return 0; /* sends the command */
        ac_accept(g_ui.ac_sel);
        return 1;
    case VK_TAB:
        ac_accept(g_ui.ac_sel);
        return 1;
    case VK_ESCAPE:
        g_ui.ac_kind = AC_NONE;
        break;
    default:
        return 0;
    }
    redraw();
    return 1;
}

static int ac_hit(int x, int y)
{
    RECT r;

    if (g_ui.ac_kind == AC_NONE)
        return -1;
    r = ac_rect();
    if (x < r.left || x >= r.right || y < r.top + S(36))
        return -1;
    for (int i = 0; i < g_ui.ac_n; i++)
        if (y >= r.top + S(36) + i * S(AC_ROW) && y < r.top + S(36) + (i + 1) * S(AC_ROW))
            return i;
    return -1;
}

/* ---- Shop ---- */

/*
 * Discord's Shop: its collections, each a banner with its logo, then a grid
 * of cards (avatar decorations shown on our own avatar, profile effects,
 * nameplates, bundles) with their price and their Nitro price. The answer
 * (megabytes of JSON) is boiled down to these tables once; buying happens on
 * discord.com, where a card leads.
 */
enum { SHOP_DECORATION = 0, SHOP_EFFECT = 1, SHOP_NAMEPLATE = 2, SHOP_BUNDLE = 1000, SHOP_VARIANTS = 2000, SHOP_CREDITS = 3000 };

typedef struct {
    unsigned name, banner, logo; /* offsets in g_shop.strings: text, URLs */
    unsigned bg[2];
    int first, count;            /* its products in g_shop.items */
} shop_cat_t;

typedef struct {
    char sku[24];
    unsigned name, art, art2;    /* art: decoration asset, effect or bundle URL, nameplate asset; art2: a bundle's foreground */
    int type;                    /* SHOP_*: what it is, a group of variants taking its first variant's */
    int price, nitro, exponent;  /* in the smallest unit; nitro 0 when there is no Nitro price */
    char currency[8];
    unsigned bg[2];
    int nvariants;
    unsigned variant[4];         /* the variants' colors, for their swatches */
} shop_item_t;

static struct {
    sb_t strings;
    shop_cat_t *cats;
    shop_item_t *items;
    int ncats, nitems, cap_items;
    unsigned hero, hero_logo;
    int hero_cat; /* the collection the hero shows: its own banner is left out */
    int scroll, height, hover;
} g_shop = {.hover = -1};

static unsigned shop_str(json_t v)
{
    unsigned at;

    if (json_type(v) != JSON_STRING)
        return 0;
    at = (unsigned)g_shop.strings.len;
    json_str(v, &g_shop.strings);
    sb_addn(&g_shop.strings, "", 1);
    return at;
}

static unsigned shop_get_str(json_t obj, const char *key)
{
    json_t v;

    return json_get(obj, key, &v) ? shop_str(v) : 0;
}

static const char *shop_s(unsigned at)
{
    return at && g_shop.strings.data ? g_shop.strings.data + at : "";
}

/* The two background colors of a style block, as ARGB. */
static void shop_colors(json_t obj, unsigned *out)
{
    json_t styles, list, c;
    json_iter_t it;
    int n = 0;

    out[0] = out[1] = 0;
    if (!json_get(obj, "styles", &styles) || !json_get(styles, "background_colors", &list))
        return;
    json_iter(list, &it);
    while (n < 2 && json_next(&it, NULL, &c)) {
        long long v = 0;
        json_int(c, &v);
        out[n++] = 0xFF000000u | ((unsigned)v & 0xFFFFFF);
    }
    if (n == 1)
        out[1] = out[0];
}

/* prices[tier].country_prices.prices: the first real currency (not Orbs). */
static int shop_price(json_t prices, const char *tier, int *amount, int *exponent, char *currency)
{
    json_t t, cp, list, pr, v;
    json_iter_t it;

    if (!json_get(prices, tier, &t) || !json_get(t, "country_prices", &cp) || !json_get(cp, "prices", &list))
        return 0;
    json_iter(list, &it);
    while (json_next(&it, NULL, &pr)) {
        char cur[16] = "";
        long long a = 0, e = 0;
        if (json_get(pr, "currency", &v))
            json_raw(v, cur, sizeof cur);
        if (!lstrcmpA(cur, "discord_orb"))
            continue;
        if (json_get(pr, "amount", &v))
            json_int(v, &a);
        if (json_get(pr, "exponent", &v))
            json_int(v, &e);
        *amount = (int)a;
        if (exponent)
            *exponent = (int)e;
        if (currency)
            lstrcpynA(currency, cur, 8);
        return 1;
    }
    return 0;
}

/* The picture of a product from one of its items. */
static void shop_art(shop_item_t *it, json_t item)
{
    json_t v;
    long long t = it->type;

    if (json_get(item, "type", &v))
        json_int(v, &t);
    if (it->type != SHOP_BUNDLE)
        it->type = (int)t;
    if (t == SHOP_EFFECT)
        it->art = shop_get_str(item, "thumbnailPreviewSrc");
    else
        it->art = shop_get_str(item, "asset");
}

static void shop_add(json_t pr, int cat)
{
    shop_item_t *it;
    json_t v, items, first, prices, variants, var;
    json_iter_t iter;
    long long type = 0;

    if (g_shop.nitems == g_shop.cap_items) {
        g_shop.cap_items = g_shop.cap_items ? g_shop.cap_items * 2 : 256;
        g_shop.items = mem_realloc(g_shop.items, (size_t)g_shop.cap_items * sizeof *g_shop.items);
    }
    it = &g_shop.items[g_shop.nitems++];
    *it = (shop_item_t){0};
    if (json_get(pr, "sku_id", &v))
        json_raw(v, it->sku, sizeof it->sku);
    it->name = shop_get_str(pr, "name");
    if (json_get(pr, "type", &v))
        json_int(v, &type);
    it->type = (int)type;
    shop_colors(pr, it->bg);
    if (!it->bg[0]) {
        it->bg[0] = g_shop.cats[cat].bg[0];
        it->bg[1] = g_shop.cats[cat].bg[1];
    }
    /* A group of variants shows its first: its picture and price, and the others as swatches. */
    if (type == SHOP_VARIANTS && json_get(pr, "variants", &variants)) {
        json_iter(variants, &iter);
        while (json_next(&iter, NULL, &var)) {
            if (!it->nvariants) {
                pr = var;
                it->type = SHOP_DECORATION;
            }
            if (it->nvariants < 4 && json_get(var, "variant_value", &v)) {
                char hex[16] = "";
                unsigned c = 0;
                json_raw(v, hex, sizeof hex);
                for (const char *h = hex[0] == '#' ? hex + 1 : hex; *h; h++)
                    c = c * 16 + (unsigned)(*h >= 'a' ? *h - 'a' + 10 : *h >= 'A' ? *h - 'A' + 10 : *h - '0');
                it->variant[it->nvariants] = 0xFF000000u | c;
            }
            it->nvariants++;
        }
    }
    if (type == SHOP_BUNDLE && json_get(pr, "preview_assets", &v) && json_type(v) == JSON_OBJECT) {
        it->art = shop_get_str(v, "bg_static");
        it->art2 = shop_get_str(v, "fg_static");
    } else if (json_get(pr, "items", &items)) {
        json_iter(items, &iter);
        if (json_next(&iter, NULL, &first))
            shop_art(it, first);
    }
    if (json_get(pr, "prices", &prices)) {
        shop_price(prices, "0", &it->price, &it->exponent, it->currency);
        shop_price(prices, "4", &it->nitro, NULL, NULL);
    }
}

static void shop_free(void)
{
    sb_free(&g_shop.strings);
    mem_free(g_shop.cats);
    mem_free(g_shop.items);
    g_shop.cats = NULL;
    g_shop.items = NULL;
    g_shop.ncats = g_shop.nitems = g_shop.cap_items = 0;
    g_shop.hero = g_shop.hero_logo = 0;
}

/* Returns how many collections it found. */
static int shop_parse(const sb_t *json)
{
    json_t root, cat, products, pr, v;
    json_iter_t it, pit;
    int n;

    shop_free();
    sb_addn(&g_shop.strings, "", 1); /* offset 0: none */
    if (!json->len || !json_parse(json->data, json->len, &root) || json_type(root) != JSON_ARRAY)
        return 0;
    n = (int)json_count(root);
    g_shop.cats = mem_alloc((size_t)(n ? n : 1) * sizeof *g_shop.cats);
    json_iter(root, &it);
    while (json_next(&it, NULL, &cat)) {
        shop_cat_t *c = &g_shop.cats[g_shop.ncats];
        *c = (shop_cat_t){0};
        c->name = shop_get_str(cat, "name");
        c->banner = shop_get_str(cat, "catalog_banner_url");
        c->logo = shop_get_str(cat, "logo_url");
        shop_colors(cat, c->bg);
        if (!g_shop.hero) {
            g_shop.hero_cat = g_shop.ncats;
            g_shop.hero = shop_get_str(cat, "hero_banner_url");
            g_shop.hero_logo = shop_get_str(cat, "hero_logo_url");
        }
        c->first = g_shop.nitems;
        if (json_get(cat, "products", &products)) {
            json_iter(products, &pit);
            while (json_next(&pit, NULL, &pr))
            {
                long long t = 0;
                if (json_get(pr, "type", &v))
                    json_int(v, &t);
                if (t != SHOP_CREDITS) /* Nitro credits: not a thing to show on a card */
                    shop_add(pr, g_shop.ncats);
            }
        }
        c->count = g_shop.nitems - c->first;
        if (c->count)
            g_shop.ncats++;
    }
    return g_shop.ncats;
}

static int shop_view(void)
{
    return g_ui.view == VIEW_APP && g_ui.model && !g_ui.settings_open && g_ui.guild < 0 && g_ui.channel < 0 &&
           g_ui.home_page == HOME_SHOP;
}

/* "€5.99", "$4.99": the price in the smallest unit with its exponent. */
static void shop_money(int amount, int exponent, const char *currency, char *out)
{
    int div = 1, whole, frac;
    const char *sym = !lstrcmpiA(currency, "eur") ? "\xE2\x82\xAC" : !lstrcmpiA(currency, "usd") ? "$" : !lstrcmpiA(currency, "gbp") ? "\xC2\xA3" : "";

    for (int i = 0; i < exponent; i++)
        div *= 10;
    whole = amount / div;
    frac = amount % div;
    if (exponent == 2)
        wsprintfA(out, "%s%d.%02d%s%s", sym, whole, frac, sym[0] ? "" : " ", sym[0] ? "" : currency);
    else
        wsprintfA(out, "%s%d%s%s", sym, amount, sym[0] ? "" : " ", sym[0] ? "" : currency);
}

/* A picture of the Shop's CDN by its full URL, fitted to `px`. */
static r_image_t *shop_image(const char *url, int px)
{
    char key[96];
    int n = lstrlenA(url);

    if (!url[0])
        return NULL;
    wsprintfA(key, "sh:%d:%s", px, url + (n > 60 ? n - 60 : 0));
    return image_get(key, url, px);
}

#define SHOP_PAD 24
#define SHOP_GAP 16
#define SHOP_CARD_MIN 200
#define SHOP_HERO_H 240
#define SHOP_BANNER_H 140
#define SHOP_INFO_H 68

/* A product's card at (x, y), w wide: its picture on its colors, then its name and prices. */
static void shop_card(const shop_item_t *it, int index, int x, int y, int w, int ph)
{
    int hov = g_shop.hover == index;
    float t = tween_on(TW_SHOP, index, hov, TW_FAST, x, y, w, ph + S(SHOP_INFO_H));
    char price[40], nitro[40], line[96];

    r_round(x, y, w, ph + S(SHOP_INFO_H), S(8), 0xFF121214u);
    r_clip(x, y, w, ph);
    r_round_gradient(x, y, w, ph + S(8), S(8), it->bg[0] ? it->bg[0] : 0xFF2B2D31u, it->bg[1] ? it->bg[1] : 0xFF1E1F22u);
    r_unclip();
    switch (it->type) {
    case SHOP_DECORATION: {
        /* On our own avatar, as Discord previews it. */
        int d = ph * 45 / 100, ax = x + (w - d) / 2, ay = y + (ph - d) / 2, dd = d * 6 / 5;
        r_image_t *av = user_avatar(g_ui.model->user_id, g_ui.model->user_avatar);
        r_image_t *deco = it->art ? cdn_image("sd", "/avatar-decoration-presets/%s.png?size=240&passthrough=true",
                                               shop_s(it->art), NULL, dd)
                                  : NULL;
        if (av)
            r_image(av, ax, ay, d, d, d / 2);
        else
            r_circle(ax, ay, d, ARGB(C_ITEM));
        if (deco)
            r_image(deco, ax - (dd - d) / 2, ay - (dd - d) / 2, dd, dd, 0);
        break;
    }
    case SHOP_EFFECT: {
        r_image_t *img = shop_image(shop_s(it->art), ph);
        if (img) {
            r_clip(x, y, w, ph);
            r_image_cover(img, x, y, w, ph + S(8), S(8));
            r_unclip();
        }
        break;
    }
    case SHOP_NAMEPLATE: {
        /* The plate behind our name, as it shows in a member list. */
        char url[200];
        int ph2 = S(44), py = y + (ph - ph2) / 2, px = x + S(12), pw = w - S(24);
        r_image_t *img;
        wsprintfA(url, "https://cdn.discordapp.com/assets/collectibles/%sstatic.png", shop_s(it->art));
        img = shop_image(url, pw);
        r_round(px, py, pw, ph2, S(8), 0xFF000000u);
        if (img) {
            r_clip(px, py, pw, ph2);
            r_image_cover(img, px, py, pw, ph2, S(8));
            r_unclip();
        }
        {
            r_image_t *av = user_avatar(g_ui.model->user_id, g_ui.model->user_avatar);
            if (av)
                r_image(av, px + S(8), py + S(6), S(32), S(32), S(16));
            text(g_ui.f_h, C_INK, rect(px + S(48), py, pw - S(56), ph2),
                 g_ui.model->user_name ? model_str(g_ui.model, g_ui.model->user_name) : "", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        break;
    }
    case SHOP_BUNDLE: {
        r_image_t *bg = shop_image(shop_s(it->art), w), *fg = shop_image(shop_s(it->art2), w);
        r_clip(x, y, w, ph);
        if (bg)
            r_image_cover(bg, x, y, w, ph + S(8), S(8));
        if (fg)
            r_image_cover(fg, x, y, w, ph + S(8), S(8));
        r_unclip();
        break;
    }
    }
    if (it->nvariants > 1)
        for (int k = 0; k < it->nvariants && k < 4; k++) {
            int sx = x + S(12) + k * S(18), sy = y + ph - S(24);
            r_circle(sx - S(2), sy - S(2), S(16), 0x80000000u);
            r_circle(sx, sy, S(12), it->variant[k]);
        }
    text(g_ui.f_h, C_INK, rect(x + S(12), y + ph + S(10), w - S(24), S(22)), shop_s(it->name),
         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    shop_money(it->price, it->exponent, it->currency, price);
    if (it->nitro && it->nitro != it->price) {
        shop_money(it->nitro, it->exponent, it->currency, nitro);
        wsprintfA(line, "%s with Nitro", nitro);
        text(g_ui.f_small_mid, C_INK, rect(x + S(12), y + ph + S(36), w - S(24), S(20)), line,
             DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        {
            int lw = text_width(g_ui.f_small_mid, line) + S(8);
            text(g_ui.f_section, C_FAINT, rect(x + S(12) + lw, y + ph + S(36), w - S(24) - lw, S(20)), price,
                 DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    } else {
        text(g_ui.f_small_mid, C_INK, rect(x + S(12), y + ph + S(36), w - S(24), S(20)), price, DT_LEFT | DT_SINGLELINE);
    }
    if (t > 0.f)
        r_round_outline(x, y, w, ph + S(SHOP_INFO_H), S(8), S(2) > 2 ? S(2) : 2, lerp_argb(0x00000000u, ARGB(C_BRAND), t));
}

/*
 * Lays the Shop out from x0, w wide: paints it, or finds the card at (hx, hy).
 * Returns that card's index (-1 if none) when not drawing; sets g_shop.height.
 */
static int shop_walk(int x0, int w, int draw, int hx, int hy)
{
    int cw = w - 2 * S(SHOP_PAD), x = x0 + S(SHOP_PAD), y = S(HEADER_H) + S(SHOP_PAD) - g_shop.scroll;
    int cols = (cw + S(SHOP_GAP)) / (S(SHOP_CARD_MIN) + S(SHOP_GAP)), cardw, ph;

    if (cols < 1)
        cols = 1;
    if (cols > 5)
        cols = 5;
    cardw = (cw - (cols - 1) * S(SHOP_GAP)) / cols;
    ph = cardw * 3 / 4;

    /* The featured collection on top. */
    if (g_shop.hero) {
        if (draw && r_visible(y, S(SHOP_HERO_H))) {
            r_image_t *hero = shop_image(shop_s(g_shop.hero), cw), *logo = shop_image(shop_s(g_shop.hero_logo), cw / 2);
            r_round(x, y, cw, S(SHOP_HERO_H), S(8), 0xFF121214u);
            if (hero) {
                r_clip(x, y, cw, S(SHOP_HERO_H));
                r_image_cover(hero, x, y, cw, S(SHOP_HERO_H), S(8));
                r_unclip();
            }
            if (logo) {
                int lw, lh, fw, fh;
                r_image_size(logo, &lw, &lh);
                fh = S(SHOP_HERO_H) * 2 / 3;
                fw = lh ? lw * fh / lh : 0;
                if (fw > cw / 2) {
                    fw = cw / 2;
                    fh = lw ? lh * fw / lw : 0;
                }
                r_image(logo, x + S(40), y + (S(SHOP_HERO_H) - fh) / 2, fw, fh, 0);
            }
        }
        y += S(SHOP_HERO_H) + S(32);
    }
    for (int c = 0; c < g_shop.ncats; c++) {
        const shop_cat_t *cat = &g_shop.cats[c];
        int rows = (cat->count + cols - 1) / cols;
        /* The collection's banner with its logo; the featured one has the hero above it instead. */
        if (g_shop.hero && c == g_shop.hero_cat) {
            ;
        } else if (draw && r_visible(y, S(SHOP_BANNER_H))) {
            r_image_t *banner = shop_image(shop_s(cat->banner), cw), *logo = shop_image(shop_s(cat->logo), cw / 3);
            r_round_gradient(x, y, cw, S(SHOP_BANNER_H), S(8), cat->bg[0] ? cat->bg[0] : 0xFF121214u,
                             cat->bg[1] ? cat->bg[1] : 0xFF121214u);
            if (banner) {
                r_clip(x, y, cw, S(SHOP_BANNER_H));
                r_image_cover(banner, x, y, cw, S(SHOP_BANNER_H), S(8));
                r_unclip();
            }
            if (logo) {
                int lw, lh, fw, fh = S(SHOP_BANNER_H) / 2;
                r_image_size(logo, &lw, &lh);
                fw = lh ? lw * fh / lh : 0;
                if (fw > cw / 3) {
                    fw = cw / 3;
                    fh = lw ? lh * fw / lw : 0;
                }
                r_image(logo, x + S(32), y + (S(SHOP_BANNER_H) - fh) / 2, fw, fh, 0);
            } else {
                text(g_ui.f_h2, C_INK, rect(x + S(32), y, cw - S(64), S(SHOP_BANNER_H)), shop_s(cat->name),
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
        }
        if (!(g_shop.hero && c == g_shop.hero_cat))
            y += S(SHOP_BANNER_H) + S(SHOP_GAP);
        for (int k = 0; k < cat->count; k++) {
            int cx = x + (k % cols) * (cardw + S(SHOP_GAP)), cy = y + (k / cols) * (ph + S(SHOP_INFO_H) + S(SHOP_GAP));
            int index = cat->first + k;
            if (draw) {
                if (r_visible(cy, ph + S(SHOP_INFO_H)))
                    shop_card(&g_shop.items[index], index, cx, cy, cardw, ph);
            } else if (hx >= cx && hx < cx + cardw && hy >= cy && hy < cy + ph + S(SHOP_INFO_H) && hy >= S(HEADER_H)) {
                return index;
            }
        }
        y += rows * (ph + S(SHOP_INFO_H) + S(SHOP_GAP)) + S(24);
    }
    g_shop.height = y + g_shop.scroll - S(HEADER_H);
    return -1;
}

static void paint_shop(RECT rc, int x0, int w)
{
    fill(x0, S(HEADER_H) - S(1), w, S(1) > 1 ? S(1) : 1, C_LINE);
    text_w(g_ui.f_icon_mid, C_MUTED, rect(x0 + S(16), 0, S(24), S(HEADER_H)), L"\xE719", -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    text(g_ui.f_h, C_INK, rect(x0 + S(48), 0, w - S(64), S(HEADER_H)), "Shop", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (g_ui.shop_state != 2 || !g_shop.ncats) {
        text(g_ui.f_body, C_MUTED, rect(x0, rc.bottom / 2 - S(12), w, S(24)),
             g_ui.shop_state == 3 ? "The Shop could not be loaded." : g_ui.shop_state == 2 ? "Nothing in the Shop right now."
                                                                                         : "Loading the Shop\xE2\x80\xA6",
             DT_CENTER | DT_SINGLELINE);
        return;
    }
    r_clip(x0, S(HEADER_H), w, rc.bottom - S(HEADER_H));
    shop_walk(x0, w, 1, 0, 0);
    r_unclip();
}

static int shop_hit(int x, int y)
{
    int x0 = S(RAIL_W + SIDE_W);

    if (!shop_view() || g_ui.shop_state != 2 || x < x0 || y < S(HEADER_H))
        return -1;
    return shop_walk(x0, main_right() - x0, 0, x, y);
}

static void shop_scroll_by(int delta)
{
    RECT rc;
    int max;

    GetClientRect(g_ui.wnd, &rc);
    max = g_shop.height - (rc.bottom - S(HEADER_H)) + S(SHOP_PAD);
    g_shop.scroll += delta;
    if (g_shop.scroll > max)
        g_shop.scroll = max;
    if (g_shop.scroll < 0)
        g_shop.scroll = 0;
}

/* ---- Nitro ---- */

/*
 * Discord's Nitro page, laid out as its marketing page is: a bar of tabs that
 * jump to their section, a hero with a huge italic heading, What's New, the
 * best perks, the plans, and a last call to subscribe. Subscribing and gifting
 * happen on discord.com, where every button leads.
 */
enum { NB_NONE, NB_TAB, NB_HEART = NB_TAB + 5, NB_GIFT_TOP, NB_SUBSCRIBE, NB_GIFT, NB_PLAN_BASIC, NB_PLAN_NITRO, NB_FOOT_SUB,
       NB_FOOT_GIFT };

/* The perks Discord's page shows, with its words and its art (the dark theme's). */
static const struct {
    const char *art, *title, *text;
} k_nitro_perks[] = {
    {"https://discord.com/assets/d8edaaf5cb32248f.svg", "More Emoji Power", "Hype, roast, and meme with custom emoji anywhere."},
    {"https://discord.com/assets/3bcdc01b26c7f691.svg", "HD Video",
     "Better video resolutions for all your streams. Stream apps and games in sweet, sweet HD."},
    {"https://discord.com/assets/010eae6a6dbacc63.svg", "500 MB Uploads", "Upload what you want with increased 500 MB upload size."},
    {"https://cdn.discordapp.com/assets/content/bca160c31fc5390dd2b41d90060edcc912a45f6ab3beab44ea79e16bf1f6530f.png",
     "Custom App Icons", "Choose a mobile and in-app desktop icon that fits your vibe."},
    {"https://discord.com/assets/bd6751720573fb38.svg", "Use Custom Sounds",
     "Use custom sounds and personalized entrance sounds across voice channels."},
    {"https://discord.com/assets/1eb1b74667b4c0f0.svg", "Video Backgrounds", "Make video calls unique with your own backgrounds."},
    {"https://discord.com/assets/99b308eabe7fcfd2.svg", "Unlimited Super Reactions",
     "We made Super Reactions unlimited so you can unleash the chaos in your chats."},
    {"https://discord.com/assets/0838bda6ecd20d91.svg", "Special Sticker Access", "Use custom stickers anywhere."},
    {"https://discord.com/assets/42e77ef3b6c4c1bb.svg", "Subscriber Badge", "Get this cool badge for being a Nitro subscriber."},
};

/* Discord's own words for the page (its English strings), the apostrophe curly as there. */
/*
 * The hero's trinkets and flying Wumpus, where Discord's page puts them around
 * its heading: offsets from the heading block's left (or right, when `right`)
 * and from the top of the page, for a page 960 wide or more, then narrower.
 */
#define NITRO_ART "https://cdn.discordapp.com/assets/content/"
static const struct {
    const char *png;
    int right, x, y, x_narrow, y_narrow, h; /* h: height, 0 for the trinkets' 60 wide */
} k_nitro_trinkets[] = {
    {NITRO_ART "0b1a59149e615fc048010a3c7f109f8695c8b2004712e99417fcb0dec43fcb44.png", 0, -135, 110, -105, 60, 0}, /* star */
    {NITRO_ART "7c23a220a6f31150648930e2ebb435aa7cc89ad57895275bed6f1900869f4de0.png", 0, -70, 350, -40, 300, 0},  /* key */
    {NITRO_ART "6cb761e3e97838c7927f738882b67bd825d5eeed89633e0af126bda5f9d4e71d.png", 0, 40, 270, 70, 220, 0},    /* car */
    {NITRO_ART "b39a5f2755e6da320fce10c8a4a10bdebef9926b671256b1681976198c5656d4.png", 1, 90, 60, 90, 60, 0},      /* hammer */
    {NITRO_ART "ae5638c61a572593c6b03b92e80d3846e0cfe7a9e893f3faf05aecd670a4017d.png", 1, -250, 115, -100, 325, 0}, /* bolt */
    {NITRO_ART "46e72137fc3631c8024b00c33dbab5cf45740d4ab35f77bd96517830e727d0c5.png", 1, -155, 200, -100, 170, 293}, /* Wumpus */
};

static const char *const k_nitro_tabs[5] = {"Home", "What\xE2\x80\x99s New", "Best of Nitro", "Plans", "Compare"};

static struct {
    int scroll, height;
    int section[5]; /* where each tab's section starts, in content coordinates */
} g_nitro;

static int nitro_view(void)
{
    return g_ui.view == VIEW_APP && g_ui.model && !g_ui.settings_open && g_ui.guild < 0 && g_ui.channel < 0 &&
           g_ui.home_page == HOME_NITRO;
}

#define NITRO_NAV_H 48
#define NITRO_PAD 40

/* A heading in Discord's marketing style: heavy italic capitals, lines packed tight, centered in w. Returns its height. */
static int nitro_heading(r_font_t *f, int px, int x, int y, int w, const char *s, int draw)
{
    wchar_t *text = utf8_to_wide(s, lstrlenA(s)), line[256];
    int lh = px * 86 / 100, h = 0;

    CharUpperW(text);
    for (const wchar_t *p = text; *p;) {
        int n = 0, fit = 0;
        /* as many words as fit */
        for (;;) {
            int k = n;
            while (p[k] && p[k] != L' ')
                k++;
            if (k > 250)
                break;
            lstrcpynW(line, p, k + 1);
            if (fit && r_text_width(f, line, -1) > w)
                break;
            fit = k;
            if (!p[k])
                break;
            n = k + 1;
        }
        if (!fit)
            fit = lstrlenW(p) < 250 ? lstrlenW(p) : 250;
        lstrcpynW(line, p, fit + 1);
        if (draw)
            r_text(f, 0xFFFFFFFFu, x, y + h - px / 8, w, px * 5 / 4, line, -1, R_CENTER | R_SINGLE);
        h += lh;
        p += fit;
        while (*p == L' ')
            p++;
    }
    mem_free(text);
    return h;
}

/* A rounded button with an optional glyph; `hot` eases its color. */
static void nitro_button_at(RECT b, const wchar_t *glyph, const char *label, unsigned bg, unsigned bg_hot, unsigned ink, int hot,
                            int id)
{
    float t = tween_on(TW_NITRO, id, hot, TW_FAST, b.left, b.top, b.right - b.left, b.bottom - b.top);
    wchar_t *w = utf8_to_wide(label, lstrlenA(label));
    int tw = r_text_width(g_ui.f_nav, w, -1) + (glyph ? S(28) : 0), tx = b.left + (b.right - b.left - tw) / 2;

    r_round(b.left, b.top, b.right - b.left, b.bottom - b.top, S(8), lerp_argb(bg, bg_hot, t));
    if (glyph)
        r_text(g_ui.f_icon_mid, ink, tx, b.top, S(22), b.bottom - b.top, glyph, -1, R_CENTER | R_VCENTER | R_SINGLE);
    r_text(g_ui.f_nav, ink, tx + (glyph ? S(28) : 0), b.top, tw, b.bottom - b.top, w, -1, R_LEFT | R_VCENTER | R_SINGLE);
    mem_free(w);
}

/*
 * Lays the page out from x0, w wide, under the tab bar: paints it, or returns
 * the NB_* thing at (hx, hy). Also notes where each section starts.
 */
static int nitro_walk(RECT rc, int x0, int w, int draw, int hx, int hy)
{
    int top = S(HEADER_H), y = top - g_nitro.scroll, cw = w - 2 * S(NITRO_PAD), x = x0 + S(NITRO_PAD), hit = NB_NONE;
    int hover = g_ui.nitro_hover;

#define NB_HIT(r, id)                                                                                                      \
    if (!draw && hx >= (r).left && hx < (r).right && hy >= (r).top && hy < (r).bottom && hy >= top)                     \
        hit = (id);

    /* Hero: Nitro's glow, the heading, Subscribe and Gift, the fine print. */
    g_nitro.section[0] = 0;
    if (draw) {
        r_fill(x0, y, w, S(620), 0xFF000000u);
        r_round_gradient(x0, y - S(40), w, S(560), 0, 0xFFB43FD4u, 0x00000000u);
        r_round_gradient(x0, y + S(260), w, S(360), 0, 0x00000000u, 0xFF000000u);
    }
    {
        /* The heading block: 800 wide at most, 650 under 960, 500 under 720; 120 from the page's top, under the tabs. */
        int wide = w >= S(960), hw = wide ? S(800) : w >= S(720) ? S(650) : S(500), hl = x0 + (w - hw) / 2;
        int page = y - S(NITRO_NAV_H), hy0 = page + S(120), hh;
        if (draw)
            for (int k = 0; k < (int)ARRAYSIZE(k_nitro_trinkets); k++) {
                int tw = S(60), th = S(60), tx, ty;
                r_image_t *img = shop_image(k_nitro_trinkets[k].png, k_nitro_trinkets[k].h ? S(293) : S(120));
                if (!img)
                    continue;
                {
                    int iw, ih;
                    r_image_size(img, &iw, &ih);
                    if (k_nitro_trinkets[k].h) {
                        th = S(wide ? k_nitro_trinkets[k].h : 200);
                        tw = ih ? iw * th / ih : th;
                    } else {
                        th = iw ? ih * tw / iw : tw;
                    }
                }
                tx = S(wide ? k_nitro_trinkets[k].x : k_nitro_trinkets[k].x_narrow);
                ty = page + S(wide ? k_nitro_trinkets[k].y : k_nitro_trinkets[k].y_narrow);
                tx = k_nitro_trinkets[k].right ? hl + hw - tx - tw : hl + tx;
                r_image(img, tx, ty, tw, th, 0);
            }
        hh = nitro_heading(g_ui.f_nitro, S(64), hl, hy0, hw, "Unlock a World of Perks with Nitro", draw);
        int by = hy0 + hh + S(32), bw1 = S(150), bw2 = S(170), bx = x0 + (w - bw1 - bw2 - S(16)) / 2;
        RECT sub = rect(bx, by, bw1, S(44)), gift = rect(bx + bw1 + S(16), by, bw2, S(44));
        if (draw) {
            nitro_button_at(sub, L"\xE734", "Subscribe", 0xFFFFFFFFu, 0xFFEDEDF7u, 0xFF000000u, hover == NB_SUBSCRIBE, NB_SUBSCRIBE);
            nitro_button_at(gift, L"\xF133", "Gift Nitro", 0xFF232428u, 0xFF2E2F34u, 0xFFFFFFFFu, hover == NB_GIFT, NB_GIFT);
            text(g_ui.f_section, C_MUTED, rect(x0 + (w - S(420)) / 2, by + S(64), S(420), S(44)),
                 "Nitro is a recurring subscription. Cancel at any time in your settings.",
                 DT_CENTER | DT_WORDBREAK);
        }
        NB_HIT(sub, NB_SUBSCRIBE);
        NB_HIT(gift, NB_GIFT);
        y = by + S(160);
    }

    /* What's New: one big card. */
    g_nitro.section[1] = y + g_nitro.scroll - top;
    y += nitro_heading(g_ui.f_nitro_h, S(48), x, y, cw, "What\xE2\x80\x99s New", draw) + S(40);
    if (draw && r_visible(y, S(360))) {
        r_round_gradient(x, y, cw, S(360), S(16), 0xFF0F3A22u, 0xFF0A0A0Cu);
        r_round_outline(x, y, cw, S(360), S(16), S(1) > 1 ? S(1) : 1, 0xFF1E1E20u);
        text(g_ui.f_nitro_card, C_INK, rect(x + S(48), y + S(56), cw / 2, S(140)),
             "Your Nitro subscription now includes Xbox Game Pass (Starter Edition)", DT_LEFT | DT_WORDBREAK);
        text(g_ui.f_body, C_TEXT, rect(x + S(48), y + S(220), cw / 2, S(60)),
             "Play a rotating library of games on PC with your Nitro subscription, at no extra cost.", DT_LEFT | DT_WORDBREAK);
    }
    y += S(360) + S(96);

    /* Best of Nitro: "Favorite Nitro Perks", cards with Discord's art (a grid of 260 wide at least, 16 apart). */
    g_nitro.section[2] = y + g_nitro.scroll - top;
    if (draw)
        text(g_ui.f_h1x, C_INK, rect(x, y, cw, S(32)), "Favorite Nitro Perks", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    y += S(32) + S(32);
    {
        int cols = (cw + S(16)) / (S(260) + S(16)), pw, ph = S(300);
        if (cols < 1)
            cols = 1;
        pw = (cw - (cols - 1) * S(16)) / cols;
        for (int k = 0; k < (int)ARRAYSIZE(k_nitro_perks); k++) {
            int px = x + (k % cols) * (pw + S(16)), py = y + (k / cols) * (ph + S(16));
            if (!draw || !r_visible(py, ph))
                continue;
            r_round(px, py, pw, ph, S(12), 0xFF0A0A0Cu);
            r_round_outline(px, py, pw, ph, S(12), S(1) > 1 ? S(1) : 1, 0xFF1E1E20u);
            {
                /* The art, fitted in its box above the words, as the page sizes it. */
                r_image_t *art = shop_image(k_nitro_perks[k].art, S(240));
                if (art) {
                    int iw, ih, bw = pw - S(48), bh = S(140), fw, fh;
                    r_image_size(art, &iw, &ih);
                    fw = bw;
                    fh = iw ? ih * fw / iw : bh;
                    if (fh > bh) {
                        fh = bh;
                        fw = ih ? iw * fh / ih : bw;
                    }
                    r_image(art, px + (pw - fw) / 2, py + S(32) + (bh - fh) / 2, fw, fh, 0);
                }
            }
            text(g_ui.f_h, C_INK, rect(px + S(24), py + S(32) + S(140) + S(16), pw - S(48), S(22)), k_nitro_perks[k].title,
                 DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            text(g_ui.f_section, C_MUTED, rect(px + S(24), py + S(32) + S(140) + S(46), pw - S(48), S(60)), k_nitro_perks[k].text,
                 DT_CENTER | DT_WORDBREAK);
        }
        y += ((int)ARRAYSIZE(k_nitro_perks) + cols - 1) / cols * (ph + S(16)) + S(80);
    }

    /* Plans: Nitro Basic and Nitro, side by side. */
    g_nitro.section[3] = g_nitro.section[4] = y + g_nitro.scroll - top;
    y += nitro_heading(g_ui.f_nitro_h, S(48), x, y, cw, "Plans", draw) + S(12);
    if (draw)
        text(g_ui.f_body, C_MUTED, rect(x, y, cw, S(24)), "Pick the plan that works for you. Cancel at any time.",
             DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    y += S(24) + S(32);
    {
        static const char *const names[2] = {"Nitro Basic", "Nitro"};
        static const char *const lines[2][4] = {
            {"Custom emoji anywhere", "500 MB uploads", "Special Nitro badge", "Longer messages"},
            {"Everything in Basic", "HD video streaming", "2 Server Boosts", "Custom profiles and more"}};
        int pw = cw >= S(620) ? (cw - S(16)) / 2 : cw, ph = S(300);
        for (int k = 0; k < 2; k++) {
            int px = x + (pw == cw ? 0 : k * (pw + S(16))), py = y + (pw == cw ? k * (ph + S(16)) : 0);
            RECT b = rect(px + S(32), py + ph - S(32) - S(44), pw - S(64), S(44));
            if (draw && r_visible(py, ph)) {
                if (k)
                    r_round_gradient(px, py, pw, ph, S(12), 0xFF5A36C9u, 0xFFA23FC4u);
                else
                    r_round_gradient(px, py, pw, ph, S(12), 0xFF3B4BCFu, 0xFF2B63D9u);
                text(g_ui.f_nitro_card, 0xFFFFFFFFu, rect(px + S(32), py + S(28), pw - S(64), S(44)), names[k],
                     DT_LEFT | DT_SINGLELINE);
                for (int l = 0; l < 4; l++) {
                    text_w(g_ui.f_icon, 0xFFFFFFFFu, rect(px + S(32), py + S(92) + l * S(30), S(20), S(24)), ICON_CHECK, -1,
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    text(g_ui.f_body, 0xFFFFFFFFu, rect(px + S(60), py + S(92) + l * S(30), pw - S(92), S(24)), lines[k][l],
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                }
                nitro_button_at(b, NULL, "Subscribe", 0xFFFFFFFFu, 0xFFEDEDF7u, 0xFF000000u, hover == NB_PLAN_BASIC + k,
                                NB_PLAN_BASIC + k);
            }
            NB_HIT(b, NB_PLAN_BASIC + k);
        }
        y += (pw == cw ? 2 * ph + S(16) : ph) + S(120);
    }

    /* The last call. */
    {
        int hh = nitro_heading(g_ui.f_nitro, S(64), x0 + (w - S(760)) / 2, y, S(760), "What are you waiting for?", draw);
        int by = y + hh + S(32), bw1 = S(150), bw2 = S(170), bx = x0 + (w - bw1 - bw2 - S(16)) / 2;
        RECT sub = rect(bx, by, bw1, S(44)), gift = rect(bx + bw1 + S(16), by, bw2, S(44));
        if (draw) {
            nitro_button_at(sub, L"\xE734", "Subscribe", 0xFFFFFFFFu, 0xFFEDEDF7u, 0xFF000000u, hover == NB_FOOT_SUB, NB_FOOT_SUB);
            nitro_button_at(gift, L"\xF133", "Gift Nitro", 0xFF232428u, 0xFF2E2F34u, 0xFFFFFFFFu, hover == NB_FOOT_GIFT,
                            NB_FOOT_GIFT);
        }
        NB_HIT(sub, NB_FOOT_SUB);
        NB_HIT(gift, NB_FOOT_GIFT);
        y = by + S(44) + S(96);
    }
    g_nitro.height = y + g_nitro.scroll - top;

    /* The tab bar stays on top: the Nitro mark, the tabs, heart and Gift Nitro. */
    if (draw) {
        int tx = x0 + S(64);
        r_fill(x0, 0, w, S(NITRO_NAV_H), g_nitro.scroll > 0 ? 0xFF0A0A0Cu : 0xFF000000u);
        text_w(g_ui.f_icon_mid, C_INK, rect(x0 + S(16), 0, S(28), S(NITRO_NAV_H)), L"\xE734", -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        for (int k = 0; k < 5; k++) {
            int tw = text_width(g_ui.f_nav, k_nitro_tabs[k]), on = k == 0 ? g_nitro.scroll < g_nitro.section[1] - S(200) : 0;
            text(g_ui.f_nav, hover == NB_TAB + k || on ? C_INK : C_TEXT, rect(tx, 0, tw + S(4), S(NITRO_NAV_H)), k_nitro_tabs[k],
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            if (on)
                r_fill(tx, S(NITRO_NAV_H) - S(2), tw, S(2), 0xFFFFFFFFu);
            tx += tw + S(32);
        }
        {
            RECT heart = rect(x0 + w - S(16) - S(170) - S(12) - S(40), S(4), S(40), S(40)),
                 gift = rect(x0 + w - S(16) - S(170), S(4), S(170), S(40));
            nitro_button_at(heart, L"\xEB51", "", 0xFF121214u, 0xFF1E1E20u, 0xFFFFFFFFu, hover == NB_HEART, NB_HEART);
            nitro_button_at(gift, L"\xF133", "Gift Nitro", 0xFF121214u, 0xFF1E1E20u, 0xFFFFFFFFu, hover == NB_GIFT_TOP, NB_GIFT_TOP);
        }
    } else if (hy < top) {
        int tx = x0 + S(64);
        RECT heart = rect(x0 + w - S(16) - S(170) - S(12) - S(40), S(4), S(40), S(40)), gift = rect(x0 + w - S(16) - S(170), S(4), S(170), S(40));
        for (int k = 0; k < 5; k++) {
            int tw = text_width(g_ui.f_nav, k_nitro_tabs[k]);
            if (hx >= tx && hx < tx + tw)
                return NB_TAB + k;
            tx += tw + S(32);
        }
        if (hx >= heart.left && hx < heart.right && hy >= heart.top && hy < heart.bottom)
            return NB_HEART;
        if (hx >= gift.left && hx < gift.right && hy >= gift.top && hy < gift.bottom)
            return NB_GIFT_TOP;
        return NB_NONE;
    }
#undef NB_HIT
    (void)rc;
    return hit;
}

static void paint_nitro(RECT rc, int x0, int w)
{
    r_clip(x0, 0, w, rc.bottom);
    nitro_walk(rc, x0, w, 1, 0, 0);
    r_unclip();
}

static int nitro_hit(int x, int y)
{
    RECT rc;
    int x0 = S(RAIL_W + SIDE_W);

    if (!nitro_view() || x < x0)
        return NB_NONE;
    GetClientRect(g_ui.wnd, &rc);
    return nitro_walk(rc, x0, main_right() - x0, 0, x, y);
}

static void nitro_scroll_to(int to)
{
    RECT rc;
    int max;

    GetClientRect(g_ui.wnd, &rc);
    max = g_nitro.height - (rc.bottom - S(HEADER_H));
    g_nitro.scroll = to > max ? max : to;
    if (g_nitro.scroll < 0)
        g_nitro.scroll = 0;
}

/* A click on the page: tabs jump to their section, everything else leads to discord.com. */
static void nitro_click(int id)
{
    if (id >= NB_TAB && id < NB_TAB + 5) {
        nitro_scroll_to(g_nitro.section[id - NB_TAB]);
        redraw();
    } else if (id == NB_HEART) {
        ShellExecuteW(NULL, L"open", L"https://discord.com/nitro", NULL, NULL, SW_SHOWNORMAL);
    } else if (id != NB_NONE) {
        ShellExecuteW(NULL, L"open", id == NB_GIFT || id == NB_GIFT_TOP || id == NB_FOOT_GIFT ? L"https://discord.com/nitro#gift"
                                                                                              : L"https://discord.com/nitro",
                      NULL, NULL, SW_SHOWNORMAL);
    }
}

/* ---- Quests ---- */

/*
 * Discord's Quests: the ones still running, each a card with its game's
 * picture, its name and reward, and when it ends. Taking one on happens in
 * Discord, where the card's button leads.
 */
typedef struct {
    char id[24];
    char ends[24];              /* "Sep 1": when it ends, in the local time */
    unsigned name, game, publisher, reward, hero; /* offsets in g_quests.strings */
    unsigned logo, tile, reward_art;  /* URLs: the partner's logotype, the game's tile, the reward's picture */
    int minutes;                      /* the task: play this long */
    int orbs;                         /* the reward is Orbs, this many */
    int state;                  /* 0 available, 1 accepted, 2 completed */
} quest_t;

static struct {
    sb_t strings;
    quest_t *v;
    int n, scroll, height, hover;
} g_quests = {.hover = -1};

static unsigned quest_str(json_t obj, const char *key)
{
    json_t v;
    unsigned at;

    if (!json_get(obj, key, &v) || json_type(v) != JSON_STRING)
        return 0;
    at = (unsigned)g_quests.strings.len;
    json_str(v, &g_quests.strings);
    sb_addn(&g_quests.strings, "", 1);
    return at;
}

static const char *quest_s(unsigned at)
{
    return at && g_quests.strings.data ? g_quests.strings.data + at : "";
}

/* "2026-09-01T04:00:00+00:00" as a local "Sep 1". */
static void quest_date(const char *iso, char *out, int size)
{
    SYSTEMTIME st = {0}, local;
    wchar_t w[32];

    out[0] = 0;
    if (lstrlenA(iso) < 16)
        return;
    st.wYear = (WORD)((iso[0] - '0') * 1000 + (iso[1] - '0') * 100 + (iso[2] - '0') * 10 + (iso[3] - '0'));
    st.wMonth = (WORD)((iso[5] - '0') * 10 + (iso[6] - '0'));
    st.wDay = (WORD)((iso[8] - '0') * 10 + (iso[9] - '0'));
    st.wHour = (WORD)((iso[11] - '0') * 10 + (iso[12] - '0'));
    st.wMinute = (WORD)((iso[14] - '0') * 10 + (iso[15] - '0'));
    if (!SystemTimeToTzSpecificLocalTime(NULL, &st, &local))
        local = st;
    if (GetDateFormatEx(L"en-US", 0, &local, L"MMM d", w, ARRAYSIZE(w), NULL))
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, size, NULL, NULL);
}

/* Returns how many quests are still running, -1 if the answer could not be read. */
static int quests_parse(const sb_t *json)
{
    json_t root, list, q, config, v, msgs, assets, rewards, rlist, r, st;
    json_iter_t it, rit;
    SYSTEMTIME now;
    char now_iso[32];

    sb_free(&g_quests.strings);
    mem_free(g_quests.v);
    g_quests.v = NULL;
    g_quests.n = 0;
    sb_addn(&g_quests.strings, "", 1);
    if (!json->len || !json_parse(json->data, json->len, &root) || !json_get(root, "quests", &list))
        return -1;
    GetSystemTime(&now);
    wsprintfA(now_iso, "%04d-%02d-%02dT%02d:%02d:%02d", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    g_quests.v = mem_alloc((json_count(list) + 1) * sizeof *g_quests.v);
    json_iter(list, &it);
    while (json_next(&it, NULL, &q)) {
        quest_t *x = &g_quests.v[g_quests.n];
        char ends[40] = "";
        if (!json_get(q, "config", &config))
            continue;
        if (json_get(config, "expires_at", &v))
            json_raw(v, ends, sizeof ends);
        if (lstrcmpA(ends, now_iso) <= 0)
            continue; /* over: ISO dates compare as text */
        *x = (quest_t){0};
        if (json_get(q, "id", &v))
            json_raw(v, x->id, sizeof x->id);
        quest_date(ends, x->ends, sizeof x->ends);
        if (json_get(config, "messages", &msgs)) {
            x->name = quest_str(msgs, "quest_name");
            x->game = quest_str(msgs, "game_title");
            x->publisher = quest_str(msgs, "game_publisher");
        }
        if (json_get(config, "assets", &assets)) {
            /* the dark theme's pictures, as full CDN URLs */
            static const char *const keys[3] = {"hero", "logotype_dark", "game_tile_dark"};
            unsigned *out[3] = {&x->hero, &x->logo, &x->tile};
            for (int k = 0; k < 3; k++)
                if (json_get(assets, keys[k], &v) && json_type(v) == JSON_STRING) {
                    char head[16];
                    json_raw(v, head, sizeof head);
                    if (!lstrcmpA(head, "PLACEHOLDER"))
                        continue;
                    *out[k] = (unsigned)g_quests.strings.len;
                    sb_add(&g_quests.strings, "https://cdn.discordapp.com/");
                    json_str(v, &g_quests.strings);
                    sb_addn(&g_quests.strings, "", 1);
                }
        }
        {
            /* The task: "Play ... for 15 minutes", from its target in seconds. */
            json_t tc, tasks, task;
            json_iter_t tit;
            if (json_get(config, "task_config_v2", &tc) && json_get(tc, "tasks", &tasks)) {
                json_iter(tasks, &tit);
                if (json_next(&tit, NULL, &task) && json_get(task, "target", &v)) {
                    long long secs = 0;
                    json_int(v, &secs);
                    x->minutes = (int)((secs + 59) / 60);
                }
            }
        }
        if (json_get(config, "rewards_config", &rewards) && json_get(rewards, "rewards", &rlist)) {
            json_iter(rlist, &rit);
            if (json_next(&rit, NULL, &r)) {
                if (json_get(r, "messages", &msgs))
                    x->reward = quest_str(msgs, "name");
                if (json_get(r, "orb_quantity", &v)) {
                    long long o = 0;
                    json_int(v, &o);
                    x->orbs = (int)o;
                }
                if (json_get(r, "asset", &v) && json_type(v) == JSON_STRING) {
                    x->reward_art = (unsigned)g_quests.strings.len;
                    sb_add(&g_quests.strings, "https://cdn.discordapp.com/");
                    json_str(v, &g_quests.strings);
                    sb_addn(&g_quests.strings, "", 1);
                }
            }
        }
        if (json_get(q, "user_status", &st) && json_type(st) == JSON_OBJECT) {
            if (json_get(st, "completed_at", &v) && json_type(v) == JSON_STRING)
                x->state = 2;
            else if (json_get(st, "enrolled_at", &v) && json_type(v) == JSON_STRING)
                x->state = 1;
        }
        g_quests.n++;
    }
    return g_quests.n;
}

static int quests_view(void)
{
    return g_ui.view == VIEW_APP && g_ui.model && !g_ui.settings_open && g_ui.guild < 0 && g_ui.channel < 0 &&
           g_ui.home_page == HOME_QUESTS;
}

/* Discord's grid: cards 336 wide at least, 24 apart, 1310 at most in all; a card is 316 high, its picture 150. */
#define QUEST_MIN_W 336
#define QUEST_GAP 24
#define QUEST_MAX_W 1310
#define QUEST_H 316
#define QUEST_HERO_H 150

/* Lays the Quests out: paints them, or returns the card whose button is at (hx, hy) (-1 if none). */
static int quests_walk(int x0, int w, int draw, int hx, int hy)
{
    int cw = w - 2 * S(SHOP_PAD), x, y = S(HEADER_H) + S(SHOP_PAD) - g_quests.scroll, cols, cardw;
    char title[48];

    if (cw > S(QUEST_MAX_W))
        cw = S(QUEST_MAX_W);
    x = x0 + (w - cw) / 2;
    cols = (cw + S(QUEST_GAP)) / (S(QUEST_MIN_W) + S(QUEST_GAP));
    if (cols < 1)
        cols = 1;
    cardw = (cw - (cols - 1) * S(QUEST_GAP)) / cols;
    lstrcpyA(title, "All Quests");
    if (draw)
        text(g_ui.f_h1x, C_INK, rect(x, y, cw, S(32)), title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y += S(32) + S(24);
    for (int k = 0; k < g_quests.n; k++) {
        const quest_t *q = &g_quests.v[k];
        int cx = x + (k % cols) * (cardw + S(QUEST_GAP)), cy = y + (k / cols) * (S(QUEST_H) + S(QUEST_GAP));
        RECT b = rect(cx + S(16), cy + S(QUEST_H) - S(16) - S(36), cardw - S(32), S(36));
        if (!draw) {
            if (hx >= b.left && hx < b.right && hy >= b.top && hy < b.bottom && hy >= S(HEADER_H))
                return k;
            continue;
        }
        if (!r_visible(cy, S(QUEST_H)))
            continue;
        /* background-surface-high, the picture on top fading into it */
        r_round(cx, cy, cardw, S(QUEST_H), S(10), 0xFF0A0A0Cu);
        {
            r_image_t *hero = shop_image(quest_s(q->hero), cardw);
            r_clip(cx, cy, cardw, S(QUEST_HERO_H));
            if (hero)
                r_image_cover(hero, cx, cy, cardw, S(QUEST_HERO_H) + S(10), S(10));
            r_round_gradient(cx, cy + S(QUEST_HERO_H) * 45 / 100, cardw, S(QUEST_HERO_H) * 55 / 100 + S(1), 0, 0x000A0A0Cu,
                             0xFF0A0A0Cu);
            r_unclip();
        }
        {
            /* the partner's logotype at the bottom left of the picture, the info button at the top right */
            r_image_t *logo = shop_image(quest_s(q->logo), S(240));
            if (logo) {
                int iw, ih, lh = S(34), lw;
                r_image_size(logo, &iw, &ih);
                lw = ih ? iw * lh / ih : lh;
                if (lw > S(121)) {
                    lw = S(121);
                    lh = iw ? ih * lw / iw : lh;
                }
                r_image(logo, cx + S(12), cy + S(QUEST_HERO_H) - S(4) - lh, lw, lh, 0);
            }
            r_circle(cx + cardw - S(12) - S(24), cy + S(12), S(24), 0x7A1E1F22u);
            text_w(g_ui.f_icon, C_INK, rect(cx + cardw - S(12) - S(24), cy + S(12), S(24), S(24)), L"\xE946", -1,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        {
            char line[200];
            int ty = cy + S(QUEST_HERO_H) + S(4);
            wsprintfA(line, "Promoted by %.60s \xC2\xB7 Ends %s", quest_s(q->publisher), q->ends);
            text(g_ui.f_small, C_MUTED, rect(cx + S(16), ty, cardw - S(32), S(16)), line, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            text(g_ui.f_h, C_INK, rect(cx + S(16), ty + S(18), cardw - S(32), S(22)), quest_s(q->name),
                 DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            /* the reward: its tile and "Claim ...", in a well of the page's color */
            {
                int wy = ty + S(46);
                r_image_t *art = shop_image(quest_s(q->reward_art ? q->reward_art : q->tile), S(116));
                r_round(cx + S(16), wy, cardw - S(32), S(62), S(8), ARGB(C_MAIN));
                if (art)
                    r_image(art, cx + S(24), wy + S(4), S(54), S(54) - S(4), S(4));
                else if (q->orbs) {
                    r_round_gradient(cx + S(24), wy + S(4), S(54), S(54) - S(4), S(4), 0xFF7B5CFAu, 0xFFB45CF0u);
                    text(g_ui.f_h, 0xFFFFFFFFu, rect(cx + S(24), wy + S(4), S(54), S(50)), "\xE2\x97\x8F", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
                wsprintfA(line, "Claim %.120s", quest_s(q->reward));
                text(g_ui.f_small_mid, C_INK, rect(cx + S(88), wy + S(6), cardw - S(112), S(20)), line,
                     DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                if (q->minutes)
                    wsprintfA(line, "Play %.80s for %d minutes", quest_s(q->game), q->minutes);
                else
                    lstrcpynA(line, quest_s(q->game), sizeof line);
                text(g_ui.f_small, C_MUTED, rect(cx + S(88), wy + S(28), cardw - S(112), S(18)), line,
                     DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }
        {
            static const char *const labels[3] = {"Accept Quest", "In Progress", "Claimed"};
            float t = tween_on(TW_QUEST, k, g_quests.hover == k, TW_FAST, b.left, b.top, b.right - b.left, b.bottom - b.top);
            unsigned bg = q->state == 2 ? 0xFF248045u : q->state == 1 ? lerp_argb(0xFF242426u, 0xFF2E2E31u, t)
                                                                       : lerp_argb(ARGB(C_BRAND), 0xFF4752C4u, t);
            r_round(b.left, b.top, b.right - b.left, b.bottom - b.top, S(8), bg);
            text(g_ui.f_small_mid, C_INK, b, labels[q->state], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    g_quests.height = y + ((g_quests.n + cols - 1) / cols) * (S(QUEST_H) + S(QUEST_GAP)) + g_quests.scroll - S(HEADER_H);
    return -1;
}

static void paint_quests(RECT rc, int x0, int w)
{
    fill(x0, S(HEADER_H) - S(1), w, S(1) > 1 ? S(1) : 1, C_LINE);
    text_w(g_ui.f_icon_mid, C_MUTED, rect(x0 + S(16), 0, S(24), S(HEADER_H)), L"\xE7C1", -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    text(g_ui.f_h, C_INK, rect(x0 + S(48), 0, w - S(64), S(HEADER_H)), "Quests", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (g_ui.quests_state != 2 || !g_quests.n) {
        text(g_ui.f_body, C_MUTED, rect(x0, rc.bottom / 2 - S(12), w, S(24)),
             g_ui.quests_state == 3 ? "Quests could not be loaded." : g_ui.quests_state == 2 ? "No Quests right now. Check back soon!"
                                                                                             : "Loading Quests\xE2\x80\xA6",
             DT_CENTER | DT_SINGLELINE);
        return;
    }
    r_clip(x0, S(HEADER_H), w, rc.bottom - S(HEADER_H));
    quests_walk(x0, w, 1, 0, 0);
    r_unclip();
}

static int quests_hit(int x, int y)
{
    int x0 = S(RAIL_W + SIDE_W);

    if (!quests_view() || g_ui.quests_state != 2 || x < x0 || y < S(HEADER_H))
        return -1;
    return quests_walk(x0, main_right() - x0, 0, x, y);
}

static void quests_scroll_by(int delta)
{
    RECT rc;
    int max;

    GetClientRect(g_ui.wnd, &rc);
    max = g_quests.height - (rc.bottom - S(HEADER_H)) + S(SHOP_PAD);
    g_quests.scroll += delta;
    if (g_quests.scroll > max)
        g_quests.scroll = max;
    if (g_quests.scroll < 0)
        g_quests.scroll = 0;
}

/* ---- Friends (home screen) ---- */

#define FR_ROW 62

enum { REL_FRIEND = 1, REL_BLOCKED = 2, REL_INCOMING = 3, REL_OUTGOING = 4 };
enum { TAB_ONLINE, TAB_ALL, TAB_PENDING, TAB_BLOCKED, TAB_ADD, TAB_COUNT };

static relation_t *rel_find(const char *id)
{
    for (int i = 0; i < g_ui.nrels; i++)
        if (lstrcmpA(g_ui.rels[i].id, id) == 0)
            return &g_ui.rels[i];
    return NULL;
}

/* {type, user} or RELATIONSHIP_ADD's {id, type, user}. */
static void rel_store(json_t obj)
{
    json_t user, v;
    char id[24] = "";
    long long type = 0;
    relation_t *r;

    if (!json_get(obj, "user", &user) || !json_get(user, "id", &v))
        return;
    json_raw(v, id, sizeof id);
    if (json_get(obj, "type", &v))
        json_int(v, &type);
    if (!(r = rel_find(id))) {
        if (g_ui.nrels == g_ui.cap_rels) {
            g_ui.cap_rels = g_ui.cap_rels ? g_ui.cap_rels * 2 : 64;
            g_ui.rels = mem_realloc(g_ui.rels, (size_t)g_ui.cap_rels * sizeof *g_ui.rels);
        }
        r = &g_ui.rels[g_ui.nrels++];
        *r = (relation_t){0};
        lstrcpynA(r->id, id, sizeof r->id);
    }
    r->type = (int)type;
    sb_clear(&r->name);
    sb_clear(&r->username);
    if (!(json_get(user, "global_name", &v) && json_type(v) == JSON_STRING && json_str(v, &r->name)))
        if (json_get(user, "username", &v))
            json_str(v, &r->name);
    if (json_get(user, "username", &v))
        json_str(v, &r->username);
    r->avatar[0] = 0;
    if (json_get(user, "avatar", &v) && json_type(v) == JSON_STRING)
        json_raw(v, r->avatar, sizeof r->avatar);
}

static void rel_remove(const char *id)
{
    for (int i = 0; i < g_ui.nrels; i++)
        if (lstrcmpA(g_ui.rels[i].id, id) == 0) {
            sb_free(&g_ui.rels[i].name);
            sb_free(&g_ui.rels[i].username);
            g_ui.rels[i] = g_ui.rels[--g_ui.nrels];
            return;
        }
}

static void rels_clear(void)
{
    for (int i = 0; i < g_ui.nrels; i++) {
        sb_free(&g_ui.rels[i].name);
        sb_free(&g_ui.rels[i].username);
    }
    g_ui.nrels = 0;
}

static int friends_view(void)
{
    return g_ui.view == VIEW_APP && g_ui.model && g_ui.guild < 0 && g_ui.channel < 0 && g_ui.home_page == HOME_FRIENDS;
}

static int in_tab(const relation_t *r, int tab)
{
    int st = user_status(r->id);

    switch (tab) {
    case TAB_ONLINE: return r->type == REL_FRIEND && st != ML_OFFLINE && st != ML_UNKNOWN;
    case TAB_ALL: return r->type == REL_FRIEND;
    case TAB_PENDING: return r->type == REL_INCOMING || r->type == REL_OUTGOING;
    case TAB_BLOCKED: return r->type == REL_BLOCKED;
    }
    return 0;
}

/* The friends page, as in Discord: search, "Online - 3", the rows; "Active Now" on the right when there is room. */
#define FR_SEARCH_H 40
#define FR_ACTIVE_W 360
#define FR_PAD 24

static int friends_active_w(int w)
{
    return w >= S(860) ? S(FR_ACTIVE_W) : 0;
}

static int friends_search_y(void)
{
    return S(HEADER_H) + S(12);
}

static int friends_list_y(void)
{
    return friends_search_y() + S(FR_SEARCH_H) + S(20) + S(28);
}

/* What the search box holds, lowercased by the comparison. */
static int friend_matches(const relation_t *r)
{
    wchar_t q[64];
    sb_t u = {0};
    int ok;

    if (!g_ui.friend_search || !IsWindowVisible(g_ui.friend_search) || !GetWindowTextW(g_ui.friend_search, q, ARRAYSIZE(q)))
        return 1;
    wide_to_utf8(q, (size_t)lstrlenW(q), &u);
    ok = (r->name.data && find_str_ci(r->name.data, u.data)) || (r->username.data && find_str_ci(r->username.data, u.data));
    sb_free(&u);
    return ok;
}

/* Rows of the open tab, sorted by name. */
static int friend_rows(int *out, int max)
{
    int n = 0;

    for (int i = 0; i < g_ui.nrels && n < max; i++)
        if (in_tab(&g_ui.rels[i], g_ui.friend_tab) && friend_matches(&g_ui.rels[i]))
            out[n++] = i;
    for (int a = 1; a < n; a++) {
        int x = out[a], b = a;
        while (b > 0 && CompareStringA(LOCALE_USER_DEFAULT, NORM_IGNORECASE, g_ui.rels[out[b - 1]].name.data, -1,
                                       g_ui.rels[x].name.data, -1) == CSTR_GREATER_THAN) {
            out[b] = out[b - 1];
            b--;
        }
        out[b] = x;
    }
    return n;
}

/* Action buttons of a row, right to left: returns how many and their kinds. */
enum { ACT_MESSAGE, ACT_ACCEPT, ACT_IGNORE, ACT_UNBLOCK, ACT_REMOVE };

static int row_actions(const relation_t *r, int *acts)
{
    if (r->type == REL_INCOMING) {
        acts[0] = ACT_IGNORE;
        acts[1] = ACT_ACCEPT;
        return 2;
    }
    if (r->type == REL_OUTGOING || r->type == REL_BLOCKED) {
        acts[0] = r->type == REL_BLOCKED ? ACT_UNBLOCK : ACT_IGNORE;
        return 1;
    }
    acts[0] = ACT_REMOVE;
    acts[1] = ACT_MESSAGE;
    return 2;
}

/*
 * A button of the friends list: message, accept, ignore, or "more" (three
 * dots, glyph NULL). Bare, as in Discord; a round background shows on the
 * hovered row, lighter under the pointer.
 */
static void friend_button(int bx, int by, const wchar_t *glyph, int row_hovered, int hot, unsigned hot_ink)
{
    unsigned ink = hot ? hot_ink : ARGB(C_MUTED);

    if (row_hovered)
        r_circle(bx, by, S(36), hot ? 0xFF1E1E20u : 0xFF0A0A0Cu);
    if (glyph) {
        r_text(g_ui.f_icon_mid, ink, bx, by, S(36), S(36), glyph, -1, R_CENTER | R_VCENTER | R_SINGLE);
    } else {
        int d = S(4) > 3 ? S(4) : 3, cx = bx + (S(36) - d) / 2;
        for (int k = -1; k <= 1; k++)
            r_circle(cx, by + S(18) - d / 2 + k * S(7), d, ink);
    }
}

/* "Active Now": friends playing, listening or streaming, one card each. */
static void paint_active_now(RECT rc, int x, int w)
{
    int y = S(HEADER_H) + S(16), shown = 0;

    fill(x, S(HEADER_H), S(1) > 1 ? S(1) : 1, rc.bottom - S(HEADER_H), C_LINE);
    x += S(16);
    w -= S(32);
    text(g_ui.f_h2, C_INK, rect(x, y, w, S(28)), "Active Now", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y += S(44);
    for (int i = 0; i < g_ui.nrels && y < rc.bottom - S(80); i++) {
        const relation_t *r = &g_ui.rels[i];
        const presence_t *pr = presence_find(r->id);
        r_image_t *img;
        int rich, h, ih;
        if (r->type != REL_FRIEND || !pr || !pr->game.len || pr->status == ML_OFFLINE)
            continue; /* activities only, as Discord: not a custom status */
        /* a game with more to say gets Discord's inner card: its picture, name, details, state and time */
        rich = pr->game.len && (pr->details.len || pr->state.len || pr->image.len || pr->start);
        ih = pr->listening && pr->details.len && pr->end > pr->start && pr->start ? S(120) : S(92);
        h = S(72) + (rich ? ih + S(8) : 0);
        r_round(x, y, w, h, S(16), ARGB(C_MAIN));
        r_round_outline(x, y, w, h, S(16), S(1) > 1 ? S(1) : 1, 0xFF1E1E20u);
        img = user_avatar(r->id, r->avatar);
        if (img)
            r_image(img, x + S(16), y + S(16), S(40), S(40), S(20));
        else
            r_circle(x + S(16), y + S(16), S(40), ARGB(C_ITEM));
        paint_status(x + S(16), y + S(16), S(40), pr->status, ARGB(C_MAIN));
        text(g_ui.f_h, C_INK, rect(x + S(68), y + S(14), w - S(84), S(22)), r->name.data ? r->name.data : "",
             DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        text(g_ui.f_small, C_MUTED, rect(x + S(68), y + S(38), w - S(84), S(18)), pr->activity.data,
             DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (rich) {
            int cx = x + S(12), cy = y + S(72), cw = w - S(24), tx = cx + S(12), line = cy + S(14);
            r_image_t *art = pr->image.len ? shop_image(pr->image.data, S(64)) : NULL;
            r_round(cx, cy, cw, ih, S(8), 0xFF121214u);
            if (art) {
                r_image_cover(art, cx + S(14), cy + S(14), S(64), S(64), S(8));
                tx = cx + S(14) + S(64) + S(12);
            } else if (pr->image.len) {
                r_round(cx + S(14), cy + S(14), S(64), S(64), S(8), 0xFF1E1E20u);
                tx = cx + S(14) + S(64) + S(12);
            }
            if (pr->listening && pr->details.len) {
                /* Discord's listening card: the song, "by" the artist, "on" the album, and how far it is */
                char by[300];
                text(g_ui.f_small_mid, C_INK, rect(tx, line, cx + cw - S(12) - tx, S(18)), pr->details.data,
                     DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                line += S(18);
                if (pr->state.len) {
                    wsprintfA(by, "by %.280s", pr->state.data);
                    text(g_ui.f_small, C_TEXT, rect(tx, line, cx + cw - S(12) - tx, S(18)), by, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                    line += S(18);
                }
                if (pr->album.len) {
                    wsprintfA(by, "on %.280s", pr->album.data);
                    text(g_ui.f_small, C_TEXT, rect(tx, line, cx + cw - S(12) - tx, S(18)), by, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
                if (pr->start && pr->end > pr->start) {
                    long long len = (pr->end - pr->start) / 1000, at = (now_ms() - pr->start) / 1000;
                    int bw = cw - S(28), by0 = cy + ih - S(14);
                    char a[16], b[16];
                    if (at < 0)
                        at = 0;
                    if (at > len)
                        at = len;
                    r_round(cx + S(14), by0, bw, S(4), S(2), 0xFF3A3A3Fu);
                    r_round(cx + S(14), by0, (int)(bw * at / (len ? len : 1)), S(4), S(2), 0xFFDCDCDFu);
                    wsprintfA(a, "%d:%02d", (int)(at / 60), (int)(at % 60));
                    wsprintfA(b, "%d:%02d", (int)(len / 60), (int)(len % 60));
                    text(g_ui.f_gif, C_MUTED, rect(cx + S(14), by0 - S(16), S(60), S(14)), a, DT_LEFT | DT_SINGLELINE);
                    text(g_ui.f_gif, C_MUTED, rect(cx + cw - S(14) - S(60), by0 - S(16), S(60), S(14)), b, DT_RIGHT | DT_SINGLELINE);
                    SetTimer(g_ui.wnd, TIMER_ACTIVE, 1000, NULL);
                }
            } else {
                text(g_ui.f_small_mid, C_INK, rect(tx, line, cx + cw - S(12) - tx, S(18)), pr->game.data,
                     DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                line += S(18);
                if (pr->details.len) {
                    text(g_ui.f_small, C_TEXT, rect(tx, line, cx + cw - S(12) - tx, S(18)), pr->details.data,
                         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                    line += S(18);
                }
                if (pr->state.len && line < cy + S(62)) {
                    text(g_ui.f_small, C_TEXT, rect(tx, line, cx + cw - S(12) - tx, S(18)), pr->state.data,
                         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                    line += S(18);
                }
                if (pr->start && line < cy + S(78)) {
                    /* "01:23:45 elapsed", ticking */
                    long long el = (now_ms() - pr->start) / 1000;
                    char elapsed[48];
                    if (el < 0)
                        el = 0;
                    wsprintfA(elapsed, "%02d:%02d:%02d elapsed", (int)(el / 3600), (int)(el / 60 % 60), (int)(el % 60));
                    text(g_ui.f_small, C_TEXT, rect(tx, line, cx + cw - S(12) - tx, S(18)), elapsed,
                         DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                    SetTimer(g_ui.wnd, TIMER_ACTIVE, 1000, NULL);
                }
            }
        }
        y += h + S(8);
        shown++;
    }
    if (!shown) {
        text(g_ui.f_nav, C_INK, rect(x, y + S(8), w, S(22)), "It's quiet for now...", DT_CENTER | DT_SINGLELINE);
        text(g_ui.f_section, C_MUTED, rect(x + S(8), y + S(36), w - S(16), S(60)),
             "When a friend starts an activity\xE2\x80\x94like playing a game or hanging out on voice\xE2\x80\x94we'll show it here!",
             DT_CENTER | DT_WORDBREAK);
    }
}

static void paint_friends(RECT rc, int x0, int w)
{
    static const char *const tabs[TAB_COUNT] = {"Online", "All", "Pending", "Blocked", "Add Friend"};
    static int rows[512], n;
    static unsigned rows_frame;
    int aw = friends_active_w(w), lw = w - aw, x = x0 + S(16), y;

    /* Header: the icon, "Friends", a dot, then the tabs; Add Friend is a blurple button. */
    text_w(g_ui.f_icon_mid, C_MUTED, rect(x, 0, S(24), S(HEADER_H)), L"\xE716", -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    text(g_ui.f_h, C_INK, rect(x + S(32), 0, S(80), S(HEADER_H)), "Friends", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    x += S(32) + text_width(g_ui.f_h, "Friends") + S(12);
    r_circle(x, S(HEADER_H) / 2 - S(2), S(4), ARGB(C_FAINT));
    x += S(16);
    for (int t = 0; t < TAB_COUNT; t++) {
        int tw = text_width(g_ui.f_nav, tabs[t]) + S(24), sel = g_ui.friend_tab == t, hov = g_ui.friend_hover == -10 - t;
        int pending = 0, ty = (S(HEADER_H) - S(32)) / 2;
        float h;
        if (t == TAB_PENDING)
            for (int i = 0; i < g_ui.nrels; i++)
                pending += g_ui.rels[i].type == REL_INCOMING;
        if (t == TAB_PENDING && !pending && !sel) {
            g_ui.tab_x[t] = g_ui.tab_w[t] = -100000; /* Discord shows Pending only with requests */
            continue;
        }
        if (pending)
            tw += S(24);
        g_ui.tab_x[t] = x;
        g_ui.tab_w[t] = tw;
        h = tween_on(TW_FRIEND + 100, t, hov, TW_FAST, x, ty, tw, S(32));
        if (t == TAB_ADD) {
            if (!sel)
                r_round(x, ty, tw, S(32), S(8), lerp_argb(ARGB(C_BRAND), 0xFF4752C4u, h));
            r_text(g_ui.f_nav, sel ? 0xFF8CA0FDu : 0xFFFFFFFFu, x, ty, tw, S(32), L"Add Friend", -1,
                   R_CENTER | R_VCENTER | R_SINGLE);
        } else {
            if (sel || h > 0.f)
                r_round(x, ty, tw, S(32), S(8), sel ? ARGB(C_SELECT) : lerp_argb(ARGB(C_MAIN), ARGB(C_HOVER), h));
            text(g_ui.f_nav, sel || hov ? C_INK : C_MUTED, rect(x + S(12), ty, tw - S(24), S(32)), tabs[t],
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        if (pending)
            paint_badge(x + tw - S(8), S(HEADER_H) / 2, pending);
        x += tw + S(8);
    }
    text_w(g_ui.f_icon_mid, g_ui.friend_hover == -30 ? C_INK : C_MUTED, rect(x0 + w - S(16) - S(32), 0, S(32), S(HEADER_H)),
           L"\xE8F2", -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (aw)
        paint_active_now(rc, x0 + lw, aw);

    y = S(HEADER_H) + S(16);
    if (g_ui.friend_tab == TAB_ADD) {
        int bw = S(180), fy = y + S(64);
        text(g_ui.f_h2, C_INK, rect(x0 + S(FR_PAD), y, lw - S(2 * FR_PAD), S(28)), "Add Friend", DT_LEFT | DT_SINGLELINE);
        text(g_ui.f_section, C_MUTED, rect(x0 + S(FR_PAD), y + S(32), lw - S(2 * FR_PAD), S(22)),
             "You can add friends with their Discord username.", DT_LEFT | DT_SINGLELINE);
        r_round(x0 + S(FR_PAD), fy, lw - S(2 * FR_PAD), S(52), S(8), 0xFF121214u);
        r_round_outline(x0 + S(FR_PAD), fy, lw - S(2 * FR_PAD), S(52), S(8), S(1) > 1 ? S(1) : 1, 0xFF27272Au);
        r_round(x0 + lw - S(FR_PAD) - S(10) - bw, fy + S(10), bw, S(32), S(8), ARGB(C_BRAND));
        text(g_ui.f_small_mid, C_INK, rect(x0 + lw - S(FR_PAD) - S(10) - bw, fy + S(10), bw, S(32)), "Send Friend Request",
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (g_ui.friend_result.len)
            text(g_ui.f_small, lstrcmpA(g_ui.friend_result.data, "Success") > 0 ? C_GREEN : C_RED,
                 rect(x0 + S(FR_PAD), fy + S(60), lw - S(2 * FR_PAD), S(20)), g_ui.friend_result.data, DT_LEFT | DT_SINGLELINE);
        fill(x0, fy + S(100), lw, S(1) > 1 ? S(1) : 1, C_LINE);
        return;
    }

    /* Search, drawn around the edit control that place_friend_input() puts in it. */
    r_round(x0 + S(FR_PAD), friends_search_y(), lw - S(2 * FR_PAD), S(FR_SEARCH_H), S(8), ARGB(C_MAIN));
    r_round_outline(x0 + S(FR_PAD), friends_search_y(), lw - S(2 * FR_PAD), S(FR_SEARCH_H), S(8), S(1) > 1 ? S(1) : 1,
                    0xFF242426u);
    text_w(g_ui.f_icon_mid, C_MUTED, rect(x0 + S(FR_PAD) + S(8), friends_search_y(), S(24), S(FR_SEARCH_H)), L"\xE721", -1,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    /* The frame is painted in bands: look up statuses and sort once, not for each band. */
    if (rows_frame != g_ui.frame) {
        n = friend_rows(rows, 512);
        rows_frame = g_ui.frame;
    }
    y = friends_search_y() + S(FR_SEARCH_H) + S(20);
    {
        char title[64];
        static const char *const names[] = {"Online", "All friends", "Pending", "Blocked"};
        wsprintfA(title, "%s \xE2\x80\x94 %d", names[g_ui.friend_tab], n);
        text(g_ui.f_small_mid, C_INK, rect(x0 + S(FR_PAD), y, lw - S(2 * FR_PAD), S(20)), title,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    y = friends_list_y();
    if (!n) {
        text(g_ui.f_body, C_MUTED, rect(x0, rc.bottom / 2, lw, S(24)),
             g_ui.friend_tab == TAB_ONLINE    ? "No one's around to play with Wumpus."
             : g_ui.friend_tab == TAB_PENDING ? "There are no pending friend requests."
             : g_ui.friend_tab == TAB_BLOCKED ? "You can't unblock the Wumpus."
                                              : "Wumpus is waiting on friends.",
             DT_CENTER | DT_SINGLELINE);
        return;
    }
    r_clip(x0, y, lw, rc.bottom - y);
    y -= g_ui.friend_scroll;
    for (int k = 0; k < n; k++, y += S(FR_ROW)) {
        const relation_t *r = &g_ui.rels[rows[k]];
        r_image_t *img;
        int acts[2], na, st = user_status(r->id), hov = g_ui.friend_hover == k;
        int ax = x0 + S(FR_PAD);
        const presence_t *pr = presence_find(r->id);
        char sub[160];
        unsigned bg;
        if (!r_visible(y, S(FR_ROW)))
            continue;
        /* A line between rows, hidden next to the hovered one as in Discord. */
        if (!hov && g_ui.friend_hover != k - 1)
            fill(x0 + S(FR_PAD) + S(8), y, lw - S(2 * FR_PAD) - S(16), S(1) > 1 ? S(1) : 1, C_LINE);
        bg = row_bg(TW_FRIEND, k, 0, hov, ARGB(C_MAIN), x0 + S(FR_PAD) - S(4), y + S(1), lw - S(2 * FR_PAD) + S(8),
                    S(FR_ROW) - S(2), S(8));
        img = user_avatar(r->id, r->avatar);
        if (img)
            r_image(img, ax, y + S(15), S(32), S(32), S(16));
        else
            r_circle(ax, y + S(15), S(32), ARGB(C_ITEM));
        if (r->type == REL_FRIEND)
            paint_status(ax, y + S(15), S(32), st == ML_UNKNOWN ? ML_OFFLINE : st, bg);
        {
            /* The name, then on hover the username beside it, as Discord does. */
            int nw = text_width(g_ui.f_h, r->name.data ? r->name.data : "");
            text(g_ui.f_h, C_INK, rect(ax + S(44), y + S(10), lw - S(260), S(22)), r->name.data ? r->name.data : "",
                 DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (hov && r->username.data && nw < lw - S(360))
                text(g_ui.f_section, C_MUTED, rect(ax + S(44) + nw + S(6), y + S(11), S(200), S(22)), r->username.data,
                     DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        if (r->type == REL_INCOMING)
            lstrcpyA(sub, "Incoming Friend Request");
        else if (r->type == REL_OUTGOING)
            lstrcpyA(sub, "Outgoing Friend Request");
        else if (r->type == REL_BLOCKED)
            lstrcpyA(sub, "Blocked");
        else if (pr && pr->activity.len && st != ML_OFFLINE)
            lstrcpynA(sub, pr->activity.data, sizeof sub);
        else
            lstrcpyA(sub, st == ML_ONLINE ? "Online" : st == ML_IDLE ? "Idle" : st == ML_DND ? "Do Not Disturb" : "Offline");
        text(g_ui.f_section, C_MUTED, rect(ax + S(44), y + S(32), lw - S(260), S(18)), sub,
             DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        na = row_actions(r, acts);
        for (int a = 0; a < na; a++) {
            /* message, accept, ignore, unblock, and "more" for a friend (its menu holds Remove Friend) */
            static const wchar_t *const icons[] = {L"\xE8BD", L"\xE73E", L"\xE711", L"\xE711", NULL};
            int hot = hov && g_ui.friend_act == a;
            friend_button(x0 + lw - S(FR_PAD) - S(8) - S(36) - a * S(46), y + S(13), icons[acts[a]], hov, hot,
                          acts[a] == ACT_ACCEPT ? ARGB(C_GREEN) : acts[a] == ACT_IGNORE || acts[a] == ACT_UNBLOCK ? ARGB(C_RED)
                                                                                                     : ARGB(C_INK));
        }
    }
    r_unclip();
}

/* Row and action under (x, y) in the friends view; tabs report -10 - tab. */
static int friends_hit(int x, int y, int *act)
{
    int x0 = S(RAIL_W + SIDE_W), w = main_right() - x0, rows[512], n, top;

    *act = -1;
    if (!friends_view())
        return -1;
    if (y < S(HEADER_H)) {
        if (x >= x0 + w - S(16) - S(32) && x < x0 + w - S(16))
            return -30; /* new group DM */
        for (int t = 0; t < TAB_COUNT; t++)
            if (x >= g_ui.tab_x[t] && x < g_ui.tab_x[t] + g_ui.tab_w[t] && y >= S(8) && y < S(40))
                return -10 - t;
        return -1;
    }
    if (g_ui.friend_tab == TAB_ADD) {
        int by = S(HEADER_H) + S(16) + S(64) + S(10), lw = w - friends_active_w(w);
        if (x >= x0 + lw - S(FR_PAD) - S(10) - S(180) && x < x0 + lw - S(FR_PAD) - S(10) && y >= by && y < by + S(32))
            return -20;
        return -1;
    }
    n = friend_rows(rows, 512);
    w -= friends_active_w(w);
    top = friends_list_y() - g_ui.friend_scroll;
    if (y < friends_list_y())
        return -1;
    for (int k = 0; k < n; k++) {
        int ry = top + k * S(FR_ROW), acts[2], na;
        if (y < ry || y >= ry + S(FR_ROW) || x < x0 + S(FR_PAD) - S(4) || x >= x0 + w - S(FR_PAD) + S(4))
            continue;
        na = row_actions(&g_ui.rels[rows[k]], acts);
        for (int a = 0; a < na; a++) {
            int bx = x0 + w - S(FR_PAD) - S(8) - S(36) - a * S(46);
            if (x >= bx && x < bx + S(36) && y >= ry + S(13) && y < ry + S(49))
                *act = a;
        }
        return k;
    }
    return -1;
}

static void friends_click(int x, int y)
{
    int act, k = friends_hit(x, y, &act), rows[512];

    if (k <= -10 && k >= -10 - TAB_ADD) {
        g_ui.friend_tab = -10 - k;
        g_ui.friend_scroll = 0;
        place_friend_input();
        redraw();
        return;
    }
    if (k == -30) {
        qs_open(); /* a conversation to start: the quick switcher, as "Find or start a conversation" */
        return;
    }
    if (k == -20) {
        wchar_t w[64];
        sb_t name = {0};
        GetWindowTextW(g_ui.friend_edit, w, 64);
        wide_to_utf8(w, (size_t)lstrlenW(w), &name);
        if (name.len)
            app_add_friend(name.data);
        sb_free(&name);
        return;
    }
    if (k < 0 || act < 0)
        return;
    friend_rows(rows, 512);
    {
        relation_t *r = &g_ui.rels[rows[k]];
        int acts[2];
        row_actions(r, acts);
        switch (acts[act]) {
        case ACT_MESSAGE:
            open_dm(r->id, NULL);
            break;
        case ACT_ACCEPT:
            app_relationship(r->id, "PUT");
            break;
        case ACT_IGNORE:
        case ACT_UNBLOCK:
        case ACT_REMOVE: {
            /* The message box runs a message loop that can move or free the relationships: keep the id. */
            char id[24];
            lstrcpynA(id, r->id, sizeof id);
            if (acts[act] == ACT_REMOVE) {
                HMENU menu = CreatePopupMenu();
                POINT pt;
                int cmd;
                AppendMenuW(menu, MF_STRING, 1, L"Remove Friend");
                GetCursorPos(&pt);
                cmd = menu_track(menu, pt.x, pt.y);
                DestroyMenu(menu);
                if (cmd != 1 ||
                    MessageBoxW(g_ui.wnd, L"Remove this friend?", L"Remove Friend", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
                    break;
            }
            app_relationship(id, "DELETE");
            break;
        }
        }
    }
}

/* The username box of the Add Friend tab is a real EDIT control. */
static void place_friend_input(void)
{
    int show = friends_view() && g_ui.friend_tab == TAB_ADD, search = friends_view() && g_ui.friend_tab != TAB_ADD;
    int x0 = S(RAIL_W + SIDE_W), w = main_right() - x0, y = S(HEADER_H) + S(16) + S(64);

    w -= friends_active_w(w);
    if (search && !g_ui.friend_search) {
        HFONT font = (HFONT)SendMessageW(g_ui.composer, WM_GETFONT, 0, 0);
        g_ui.friend_search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_CLIPSIBLINGS | ES_AUTOHSCROLL, 0, 0, 0, 0,
                                             g_ui.wnd, NULL, NULL, NULL);
        SendMessageW(g_ui.friend_search, WM_SETFONT, (WPARAM)font, FALSE);
        SendMessageW(g_ui.friend_search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search");
        SendMessageW(g_ui.friend_search, EM_LIMITTEXT, 32, 0);
    }
    if (g_ui.friend_search) {
        if (search)
            MoveWindow(g_ui.friend_search, x0 + S(FR_PAD) + S(40), friends_search_y() + (S(FR_SEARCH_H) - S(24)) / 2,
                       w - S(2 * FR_PAD) - S(56), S(24), TRUE);
        ShowWindow(g_ui.friend_search, search ? SW_SHOWNA : SW_HIDE);
    }

    if (show && !g_ui.friend_edit) {
        HFONT font = (HFONT)SendMessageW(g_ui.composer, WM_GETFONT, 0, 0);
        g_ui.friend_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_CLIPSIBLINGS | ES_AUTOHSCROLL, 0, 0, 0, 0, g_ui.wnd,
                                           NULL, NULL, NULL);
        SendMessageW(g_ui.friend_edit, WM_SETFONT, (WPARAM)font, FALSE);
        SendMessageW(g_ui.friend_edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"You can add friends with their Discord username.");
        SendMessageW(g_ui.friend_edit, EM_LIMITTEXT, 32, 0);
    }
    if (g_ui.friend_edit) {
        if (show)
            MoveWindow(g_ui.friend_edit, x0 + S(FR_PAD) + S(14), y + S(15), w - S(2 * FR_PAD) - S(214), S(22), TRUE);
        ShowWindow(g_ui.friend_edit, show ? SW_SHOWNA : SW_HIDE);
    }
}

/* Message text for one-line previews: mention markers dropped, custom emoji as :name:. */
static wchar_t *plain_text(const sb_t *text)
{
    wchar_t *w = utf8_to_wide(text->data ? text->data : "", text->len), *src = w, *dst = w;

    for (; *src; src++) {
        if (*src == 0xE002) { /* custom emoji: keep ":name:" */
            while (src[1] && src[1] != ':' && src[1] != 0xE003)
                src++;
            *dst++ = ':';
            if (src[1] == ':')
                src++;
        } else if (*src == 0xE003) {
            *dst++ = ':';
        } else if (*src != 0xE000 && *src != 0xE001) {
            *dst++ = *src;
        }
    }
    *dst = 0;
    return w;
}

/* ---- Pinned messages ---- */

#define PINS_W 440

static int pins_button_x(void)
{
    return header_button_x(HB_PINS);
}

static int call_button_x(void)
{
    return header_button_x(HB_CALL);
}

static void pins_close(void)
{
    panel_batch_free(g_ui.pins, &g_ui.pin_layout);
    g_ui.pins = NULL;
    g_ui.pins_open = 0;
    g_ui.pins_inbox = 0;
    g_ui.pins_scroll = 0;
}

/* Opens the pins (inbox 0) or the inbox, or closes the panel when it already shows that. */
static void pins_toggle(int inbox)
{
    int same = g_ui.pins_open && g_ui.pins_inbox == inbox;

    pins_close();
    if (same)
        return;
    g_ui.pins_open = 1;
    g_ui.pins_inbox = inbox;
    if (inbox)
        app_fetch_mentions();
    else
        app_fetch_pins(g_ui.msgs_channel);
}

static RECT pins_rect(void);
static void navigate_to_message(const char *channel_id, const char *message_id);

/* A click in the panel: jumps to the message under it, as Discord's "Jump". */
static void pins_click(int x, int y)
{
    RECT r = pins_rect();
    char channel[24], id[24];

    if (!g_ui.pins || y < r.top + S(49))
        return;
    for (int k = 0; k < g_ui.pins->n && k < (int)ARRAYSIZE(g_ui.pin_top); k++) {
        int top = g_ui.pin_top[k] - g_ui.pins_scroll;
        msg_t *m = &g_ui.pins->msgs[k];
        if (x < r.left || x >= r.right || y < top || y >= top + g_ui.pin_h[k])
            continue;
        lstrcpynA(channel, m->channel_id[0] ? m->channel_id : g_ui.msgs_channel, sizeof channel);
        lstrcpynA(id, m->id, sizeof id);
        pins_close();
        navigate_to_message(channel, id);
        return;
    }
}

static RECT pins_rect(void)
{
    RECT rc;
    int h;

    GetClientRect(g_ui.wnd, &rc);
    h = rc.bottom * 7 / 10;
    return rect(main_right() - S(PINS_W) - S(16), S(HEADER_H) + S(4), S(PINS_W), h);
}

static void paint_pins(void)
{
    RECT r;
    int y;

    if (!g_ui.pins_open)
        return;
    r = pins_rect();
    r_round(r.left, r.top, r.right - r.left, r.bottom - r.top, S(8), 0xFF121214);
    r_round_outline(r.left, r.top, r.right - r.left, r.bottom - r.top, S(8), 1, 0xFF242426);
    text(g_ui.f_h, C_INK, rect(r.left + S(16), r.top, S(300), S(48)), g_ui.pins_inbox ? "Mentions" : "Pinned Messages",
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    fill(r.left + S(1), r.top + S(48), r.right - r.left - S(2), 1, C_LINE);
    if (!g_ui.pins) {
        text(g_ui.f_body, C_MUTED, rect(r.left, r.top + S(60), r.right - r.left, S(24)), "Loading\xE2\x80\xA6", DT_CENTER | DT_SINGLELINE);
        return;
    }
    if (!g_ui.pins->n) {
        text(g_ui.f_body, C_MUTED, rect(r.left, r.top + S(80), r.right - r.left, S(24)),
             g_ui.pins->status   ? (g_ui.pins_inbox ? "Could not load your mentions." : "Could not load the pins.")
             : g_ui.pins_inbox ? "You're all caught up!"
                               : "This channel doesn't have any pinned messages... yet.",
             DT_CENTER | DT_SINGLELINE);
        return;
    }
    r_clip(r.left, r.top + S(49), r.right - r.left, r.bottom - r.top - S(50));
    y = r.top + S(56) - g_ui.pins_scroll;
    panel_measure(&g_ui.pin_layout, g_ui.pins, r.left + S(60), r.right - r.left - S(76), S(88));
    for (int k = g_ui.pins->n; k-- > 0;) { /* newest first */
        msg_t *m = &g_ui.pins->msgs[k];
        int tw = r.right - r.left - S(76), th = g_ui.pin_layout.th[k], eh = g_ui.pin_layout.eh[k];
        wchar_t *body, when[64];
        r_image_t *img;
        if (k < (int)ARRAYSIZE(g_ui.pin_top)) {
            g_ui.pin_top[k] = y + g_ui.pins_scroll;
            g_ui.pin_h[k] = S(40) + th + eh + S(12);
        }
        if (!r_visible(y, S(40) + th + eh + S(12))) {
            y += S(40) + th + eh + S(20);
            continue;
        }
        body = plain_text(&m->text);
        img = user_avatar(m->author_id, m->avatar);
        r_round(r.left + S(8), y, r.right - r.left - S(16), S(40) + th + eh + S(12), S(6), 0xFF121214);
        if (img)
            r_image(img, r.left + S(16), y + S(10), S(32), S(32), S(16));
        text(g_ui.f_h, C_INK, rect(r.left + S(60), y + S(8), tw, S(20)), m->author.data ? m->author.data : "",
             DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        format_time(m->id, when, ARRAYSIZE(when));
        text_w(g_ui.f_small, C_FAINT, rect(r.left + S(60) + text_width(g_ui.f_h, m->author.data ? m->author.data : "") + S(8),
                                          y + S(10), S(200), S(18)), when, -1, DT_LEFT | DT_SINGLELINE);
        if (g_ui.pins_inbox) { /* where it was said */
            int c = model_find_channel(g_ui.model, m->channel_id), g = c >= 0 ? model_channel_guild(g_ui.model, (unsigned)c) : -1;
            char where[160];
            int from = r.left + S(60) + text_width(g_ui.f_h, m->author.data ? m->author.data : "") + S(8) +
                       r_text_width(g_ui.f_small, when, -1) + S(16);
            if (c >= 0 && from < r.right - S(48)) {
                if (g >= 0)
                    wsprintfA(where, "#%.60s \xE2\x80\xA2 %.60s", model_str(g_ui.model, chan(c)->name),
                              model_str(g_ui.model, g_ui.model->guilds[g].name));
                else
                    wsprintfA(where, "%.60s", model_str(g_ui.model, chan(c)->name));
                text(g_ui.f_small, C_MUTED, rect(from, y + S(10), r.right - S(16) - from, S(18)), where,
                     DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }
        if (th)
            r_text(g_ui.f_body, ARGB(C_INK), r.left + S(60), y + S(30), tw, th, body, -1, R_LEFT | R_WRAP | R_ELLIPSIS);
        if (eh)
            msg_extras(m, r.left + S(60), y + S(30) + th, tw, 1, 0, 0, NULL);
        mem_free(body);
        y += S(40) + th + eh + S(20);
    }
    g_ui.pins_content = y + g_ui.pins_scroll - (r.top + S(56));
    r_unclip();
}

/* ---- Quick switcher (Ctrl+K) ---- */

#define QS_W 560
#define QS_ROW 40
#define QS_MAX 12

enum { QS_CHANNEL, QS_DM, QS_GUILD };
enum { PROMPT_NONE, PROMPT_THREAD, PROMPT_POST_TITLE, PROMPT_POST_MESSAGE };

static void qs_rebuild(void)
{
    char q[256] = ""; /* 63 UTF-16 units take up to 189 bytes of UTF-8 */
    wchar_t w[64];
    const model_t *m = g_ui.model;

    g_ui.nqs = 0;
    g_ui.qs_sel = 0;
    if (!m || g_ui.qs_prompt)
        return;
    GetWindowTextW(g_ui.qs_edit, w, 64);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, q, sizeof q, NULL, NULL);
#define QS_ADD(k, i)                                              \
    do {                                                          \
        if (g_ui.nqs < QS_MAX) {                                  \
            g_ui.qs_kind[g_ui.nqs] = (k);                         \
            g_ui.qs_index[g_ui.nqs] = (i);                        \
            g_ui.nqs++;                                           \
        }                                                         \
    } while (0)
    /* Empty query: recent conversations, like Discord. */
    for (unsigned i = m->dm_first; i < m->dm_first + m->dm_count; i++)
        if (ci_contains(model_str(m, m->channels[i].name), q))
            QS_ADD(QS_DM, (int)i);
    if (q[0]) {
        for (unsigned g = 0; g < m->nguilds; g++) {
            if (!g_ui.qs_forward[0] && ci_contains(model_str(m, m->guilds[g].name), q))
                QS_ADD(QS_GUILD, (int)g);
            for (unsigned c = m->guilds[g].first; c < m->guilds[g].first + m->guilds[g].count; c++)
                if (m->channels[c].type != CH_CATEGORY && !is_voice_type(m->channels[c].type) &&
                    ci_contains(model_str(m, m->channels[c].name), q))
                    QS_ADD(QS_CHANNEL, (int)c);
        }
    }
#undef QS_ADD
}

static void qs_close(void)
{
    HWND w = g_ui.qs;

    if (!w)
        return;
    g_ui.qs = NULL;
    g_ui.qs_edit = NULL;
    g_ui.qs_forward[0] = 0;
    g_ui.qs_prompt = PROMPT_NONE;
    DestroyWindow(w);
    SetFocus(g_ui.composer);
    redraw();
}

static void qs_go(int i)
{
    int kind = g_ui.qs_kind[i], index = g_ui.qs_index[i];

    if (g_ui.qs_forward[0] && kind != QS_GUILD) {
        char id[24], from[24];
        int c = model_find_channel(g_ui.model, g_ui.qs_forward_from), g = c >= 0 ? model_channel_guild(g_ui.model, (unsigned)c) : -1;
        lstrcpynA(id, g_ui.qs_forward, sizeof id);
        lstrcpynA(from, g_ui.qs_forward_from, sizeof from);
        app_forward(chan(index)->id, from, g >= 0 ? g_ui.model->guilds[g].id : NULL, id);
        qs_close();
        return;
    }
    qs_close();
    if (kind == QS_GUILD)
        select_guild(index);
    else
        go_to_channel(index);
}

static int qs_height(void)
{
    if (g_ui.qs_prompt)
        return S(96) + S(QS_ROW);
    return S(96) + (g_ui.nqs ? g_ui.nqs : 1) * S(QS_ROW) + S(16);
}

static void qs_paint(RECT rc)
{
    int w = S(QS_W), h = qs_height(), y = S(96);
    const model_t *m = g_ui.model;

    (void)rc;
    r_fill(0, 0, w, h, 0xFF000000u);
    r_round(0, 0, w, h, S(10), 0xFF151515);
    r_round_outline(0, 0, w, h, S(10), 1, 0xFF242426);
    text(g_ui.f_h, C_INK, rect(S(20), S(12), w - S(40), S(24)),
         g_ui.qs_prompt == PROMPT_THREAD         ? "Create Thread"
         : g_ui.qs_prompt == PROMPT_POST_TITLE   ? "New Post"
         : g_ui.qs_prompt == PROMPT_POST_MESSAGE ? "New Post: first message"
         : g_ui.qs_forward[0]                    ? "Forward To"
                                                 : "Where would you like to go?",
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (g_ui.qs_prompt) {
        text(g_ui.f_small, C_FAINT, rect(S(20), y, w - S(40), S(QS_ROW)), "Enter to continue \xE2\x80\xA2 Esc to cancel",
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    r_round(S(20), S(44), w - S(40), S(40), S(6), 0xFF121214);
    if (!g_ui.nqs)
        text(g_ui.f_body, C_MUTED, rect(0, y, w, S(QS_ROW)), "No results", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    for (int i = 0; i < g_ui.nqs; i++, y += S(QS_ROW)) {
        int k = g_ui.qs_kind[i], idx = g_ui.qs_index[i];
        const char *name, *where = "";
        if (i == g_ui.qs_sel)
            r_round(S(12), y, w - S(24), S(QS_ROW) - S(2), S(6), ARGB(C_SELECT));
        if (k == QS_GUILD) {
            r_image_t *img = guild_icon(&m->guilds[idx]);
            name = model_str(m, m->guilds[idx].name);
            if (img)
                r_image(img, S(24), y + S(8), S(24), S(24), S(8));
            else
                r_round(S(24), y + S(8), S(24), S(24), S(8), ARGB(C_ITEM));
        } else {
            const channel_t *c = &m->channels[idx];
            int g = model_channel_guild(m, (unsigned)idx);
            name = model_str(m, c->name);
            if (g >= 0)
                where = model_str(m, m->guilds[g].name);
            if (k == QS_DM) {
                r_image_t *img = dm_icon(c);
                if (img)
                    r_image(img, S(24), y + S(8), S(24), S(24), S(12));
                else
                    r_circle(S(24), y + S(8), S(24), ARGB(C_ITEM));
            } else {
                text(g_ui.f_h, C_FAINT, rect(S(24), y, S(24), S(QS_ROW)), model_is_thread(c->type) ? "\xE2\x86\xB3" : "#",
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        text(g_ui.f_body, C_INK, rect(S(60), y, w - S(260), S(QS_ROW)), name, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (*where)
            text(g_ui.f_cat, C_FAINT, rect(w - S(200), y, S(176), S(QS_ROW)), where, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

static void qs_place(void)
{
    RECT rc;
    int w = S(QS_W), h = qs_height();

    GetClientRect(g_ui.wnd, &rc);
    SetWindowPos(g_ui.qs, HWND_TOP, (rc.right - w) / 2, rc.bottom / 5, w, h, SWP_NOACTIVATE);
    InvalidateRect(g_ui.qs, NULL, FALSE);
}

static LRESULT CALLBACK qs_edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) {
            qs_close();
            return 0;
        }
        if ((wp == VK_UP || wp == VK_DOWN) && g_ui.nqs) {
            g_ui.qs_sel = (g_ui.qs_sel + (wp == VK_DOWN ? 1 : g_ui.nqs - 1)) % g_ui.nqs;
            InvalidateRect(g_ui.qs, NULL, FALSE);
            return 0;
        }
        if (wp == VK_RETURN) {
            if (g_ui.qs_prompt)
                prompt_submit();
            else if (g_ui.nqs)
                qs_go(g_ui.qs_sel);
            return 0;
        }
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE))
        return 0;
    return CallWindowProcW(g_ui.qs_edit_proc, h, msg, wp, lp);
}

static int qs_hit(int y)
{
    int i = (y - S(96)) / S(QS_ROW);

    return y >= S(96) && i < g_ui.nqs ? i : -1;
}

static LRESULT CALLBACK qs_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        g_ui.qs_frame = paint_frame(wnd, qs_paint);
        return 0;
    case WM_COMMAND:
        if ((HWND)lp == g_ui.qs_edit && HIWORD(wp) == EN_CHANGE) {
            qs_rebuild();
            qs_place();
        }
        return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, GDI(C_INK));
        SetBkColor((HDC)wp, RGB(0x12, 0x12, 0x14));
        return (LRESULT)g_ui.qs_brush;
    case WM_MOUSEMOVE: {
        int i = qs_hit(GET_Y_LPARAM(lp));
        if (i >= 0 && i != g_ui.qs_sel) {
            g_ui.qs_sel = i;
            InvalidateRect(wnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        int i = qs_hit(GET_Y_LPARAM(lp));
        if (i >= 0)
            qs_go(i);
        return 0;
    }
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

/* The switcher as a one-line prompt: a thread's name, a forum post's title and message. */
static void prompt_open(int kind, const char *message_id, const char *channel_id, const wchar_t *cue)
{
    qs_close();
    qs_open();
    if (!g_ui.qs)
        return;
    g_ui.qs_prompt = kind;
    lstrcpynA(g_ui.qs_prompt_id, message_id ? message_id : "", sizeof g_ui.qs_prompt_id);
    lstrcpynA(g_ui.qs_prompt_channel, channel_id, sizeof g_ui.qs_prompt_channel);
    SendMessageW(g_ui.qs_edit, EM_SETCUEBANNER, TRUE, (LPARAM)cue);
    qs_rebuild();
    qs_place();
}

static void prompt_submit(void)
{
    wchar_t w[512];
    sb_t text = {0};
    char channel[24], message[24];
    int kind = g_ui.qs_prompt;

    GetWindowTextW(g_ui.qs_edit, w, ARRAYSIZE(w));
    wide_to_utf8(w, (size_t)lstrlenW(w), &text);
    if (!text.len)
        return;
    lstrcpynA(channel, g_ui.qs_prompt_channel, sizeof channel);
    lstrcpynA(message, g_ui.qs_prompt_id, sizeof message);
    if (kind == PROMPT_POST_TITLE) { /* then the post's first message */
        sb_clear(&g_ui.qs_prompt_title);
        sb_addn(&g_ui.qs_prompt_title, text.data, text.len);
        prompt_open(PROMPT_POST_MESSAGE, NULL, channel, L"Message");
    } else {
        qs_close();
        if (kind == PROMPT_THREAD)
            app_create_thread(channel, message, text.data, NULL);
        else
            app_create_thread(channel, NULL, g_ui.qs_prompt_title.data ? g_ui.qs_prompt_title.data : "", text.data);
    }
    sb_free(&text);
}

/* The switcher, picking where message `id` of the open channel goes. */
static void forward_open(const char *id)
{
    qs_close();
    qs_open();
    if (!g_ui.qs)
        return;
    lstrcpynA(g_ui.qs_forward, id, sizeof g_ui.qs_forward);
    lstrcpynA(g_ui.qs_forward_from, g_ui.msgs_channel, sizeof g_ui.qs_forward_from);
    SendMessageW(g_ui.qs_edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search for a channel or a conversation");
    InvalidateRect(g_ui.qs, NULL, FALSE);
}

static void qs_open(void)
{
    HFONT font;

    if (g_ui.qs || g_ui.view != VIEW_APP || !g_ui.model) {
        qs_close();
        return;
    }
    picker_close();
    pop_dismiss();
    if (!g_ui.qs_brush)
        g_ui.qs_brush = CreateSolidBrush(RGB(0x12, 0x12, 0x14));
    g_ui.qs = CreateWindowExW(0, L"SilicordSwitch", L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0, 0, g_ui.wnd,
                              NULL, NULL, NULL);
    g_ui.qs_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, S(32), S(53), S(QS_W) - S(64), S(22),
                                   g_ui.qs, NULL, NULL, NULL);
    g_ui.qs_edit_proc = (WNDPROC)SetWindowLongPtrW(g_ui.qs_edit, GWLP_WNDPROC, (LONG_PTR)qs_edit_proc);
    font = (HFONT)SendMessageW(g_ui.composer, WM_GETFONT, 0, 0);
    SendMessageW(g_ui.qs_edit, WM_SETFONT, (WPARAM)font, FALSE);
    SendMessageW(g_ui.qs_edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search for servers, channels or DMs");
    qs_rebuild();
    qs_place();
    fade_in(g_ui.qs);
    ShowWindow(g_ui.qs, SW_SHOWNA);
    SetFocus(g_ui.qs_edit);
}


/* ---- User settings screen ---- */

enum { SET_ACCOUNT, SET_VOICE, SET_NOTIFICATIONS, SET_ADVANCED, SET_ABOUT, SET_PAGES };
enum {
    SH_PAGE = 0,        /* + page */
    SH_CLOSE = 10, SH_LOGOUT, SH_SAVE, SH_CLEAR, SH_NOTIFY, SH_TITLE, SH_DEVELOPER, SH_SOURCE,
    SH_STATUS = 20,     /* + index in k_status_codes */
    SH_IN_VOL = 30, SH_OUT_VOL, SH_SENS, /* sliders */
    SH_IN_DEV, SH_OUT_DEV, SH_MIC_TEST, SH_MODE_VOICE, SH_MODE_PTT, SH_PTT_KEY,
};
#define SET_NAV_W 260
#define SET_ROW 72


static void prefs_path(wchar_t *out)
{
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Silicord", out, MAX_PATH);
    CreateDirectoryW(out, NULL);
    lstrcatW(out, L"\\settings.ini");
}

/* Settings of this computer only, beside the image cache. */
static void list_devices(void)
{
    g_ui.ndev_in = audio_devices(1, g_ui.dev_in, 16);
    g_ui.ndev_out = audio_devices(0, g_ui.dev_out, 16);
}

/* Devices are saved by name: their numbers change as they come and go. */
static int device_by_name(wchar_t (*names)[AUDIO_NAME], int n, const wchar_t *name)
{
    for (int i = 0; name[0] && i < n; i++)
        if (lstrcmpW(names[i], name) == 0)
            return i + 1;
    return 0;
}

static void write_int(const wchar_t *section, const wchar_t *key, int v, const wchar_t *path)
{
    wchar_t s[16];

    wsprintfW(s, L"%d", v);
    WritePrivateProfileStringW(section, key, s, path);
}

static int uvol_find(const char *user)
{
    for (int i = 0; i < g_ui.nuvol; i++)
        if (lstrcmpA(g_ui.uvol[i].user, user) == 0)
            return i;
    return -1;
}

/* Someone's volume in calls, sent to the mixer and saved. */
static void uvol_set(const char *user, int volume, int muted)
{
    wchar_t path[MAX_PATH + 16], key[24], val[16];
    int i = uvol_find(user);

    if (i < 0 && g_ui.nuvol < (int)ARRAYSIZE(g_ui.uvol))
        i = g_ui.nuvol++;
    if (i < 0)
        return;
    lstrcpynA(g_ui.uvol[i].user, user, sizeof g_ui.uvol[i].user);
    g_ui.uvol[i].volume = (short)volume;
    g_ui.uvol[i].muted = (short)muted;
    app_voice_user_volume(user, muted ? 0 : volume);
    prefs_path(path);
    MultiByteToWideChar(CP_UTF8, 0, user, -1, key, ARRAYSIZE(key));
    wsprintfW(val, muted ? L"%d m" : L"%d", volume);
    WritePrivateProfileStringW(L"user_volume", key, volume == 100 && !muted ? NULL : val, path);
}

static void prefs_load(void)
{
    wchar_t path[MAX_PATH + 16], name[AUDIO_NAME];
    static wchar_t vols[8192];
    voice_prefs_t *v = &g_ui.vprefs;

    prefs_path(path);
    g_ui.pref_notify = GetPrivateProfileIntW(L"app", L"notifications", 1, path) != 0;
    g_ui.pref_title = GetPrivateProfileIntW(L"app", L"title_count", 1, path) != 0;
    list_devices();
    GetPrivateProfileStringW(L"voice", L"input_device", L"", name, AUDIO_NAME, path);
    v->in_device = device_by_name(g_ui.dev_in, g_ui.ndev_in, name);
    GetPrivateProfileStringW(L"voice", L"output_device", L"", name, AUDIO_NAME, path);
    v->out_device = device_by_name(g_ui.dev_out, g_ui.ndev_out, name);
    v->in_volume = (int)GetPrivateProfileIntW(L"voice", L"input_volume", 100, path);
    v->out_volume = (int)GetPrivateProfileIntW(L"voice", L"output_volume", 100, path);
    v->sensitivity = (int)GetPrivateProfileIntW(L"voice", L"sensitivity", -50, path);
    v->push_to_talk = GetPrivateProfileIntW(L"voice", L"push_to_talk", 0, path) != 0;
    v->ptt_key = (int)GetPrivateProfileIntW(L"voice", L"push_to_talk_key", 0, path);
    app_voice_prefs(v);
    /* user_volume: "id=percent", with " m" when muted */
    g_ui.nuvol = 0;
    for (const wchar_t *p = vols, *end = vols + GetPrivateProfileSectionW(L"user_volume", vols, ARRAYSIZE(vols), path);
         p < end && *p; p += lstrlenW(p) + 1) {
        const wchar_t *eq = p;
        char user[24];
        int volume = 0, k = 0;
        while (*eq && *eq != '=')
            eq++;
        if (!*eq || eq - p >= 24 || g_ui.nuvol >= (int)ARRAYSIZE(g_ui.uvol))
            continue;
        for (const wchar_t *c = p; c < eq; c++)
            user[k++] = (char)*c;
        user[k] = 0;
        for (eq++; *eq >= '0' && *eq <= '9'; eq++)
            volume = volume * 10 + (*eq - '0');
        lstrcpynA(g_ui.uvol[g_ui.nuvol].user, user, 24);
        g_ui.uvol[g_ui.nuvol].volume = (short)(volume > 200 ? 200 : volume);
        g_ui.uvol[g_ui.nuvol].muted = *eq == ' ' && eq[1] == 'm';
        app_voice_user_volume(user, g_ui.uvol[g_ui.nuvol].muted ? 0 : g_ui.uvol[g_ui.nuvol].volume);
        g_ui.nuvol++;
    }
}

static void prefs_save(void)
{
    wchar_t path[MAX_PATH + 16];
    const voice_prefs_t *v = &g_ui.vprefs;

    prefs_path(path);
    WritePrivateProfileStringW(L"app", L"notifications", g_ui.pref_notify ? L"1" : L"0", path);
    WritePrivateProfileStringW(L"app", L"title_count", g_ui.pref_title ? L"1" : L"0", path);
    WritePrivateProfileStringW(L"voice", L"input_device", v->in_device ? g_ui.dev_in[v->in_device - 1] : L"", path);
    WritePrivateProfileStringW(L"voice", L"output_device", v->out_device ? g_ui.dev_out[v->out_device - 1] : L"", path);
    write_int(L"voice", L"input_volume", v->in_volume, path);
    write_int(L"voice", L"output_volume", v->out_volume, path);
    write_int(L"voice", L"sensitivity", v->sensitivity, path);
    write_int(L"voice", L"push_to_talk", v->push_to_talk, path);
    write_int(L"voice", L"push_to_talk_key", v->ptt_key, path);
}

static int developer_mode(void)
{
    return g_ui.model && g_ui.model->developer_mode;
}

static const char *status_code(void)
{
    for (int k = 0; k < 4; k++)
        if (g_ui.my_status == k_status_states[k])
            return k_status_codes[k];
    return "online";
}

static const char *status_name(void)
{
    for (int k = 0; k < 4; k++)
        if (g_ui.my_status == k_status_states[k])
            return k_status_names[k];
    return "Online";
}

static void set_hit(int x, int y, int w, int h, int id)
{
    if (g_ui.nset_hits < (int)ARRAYSIZE(g_ui.set_hits)) {
        g_ui.set_hits[g_ui.nset_hits].r = rect(x, y, w, h);
        g_ui.set_hits[g_ui.nset_hits].id = id;
        g_ui.nset_hits++;
    }
}

static int settings_hit(int x, int y)
{
    for (int k = g_ui.nset_hits; k-- > 0;) {
        RECT r = g_ui.set_hits[k].r;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return g_ui.set_hits[k].id;
    }
    return -1;
}

static int settings_content_x(void)
{
    return S(SET_NAV_W) + S(40);
}

static int settings_content_w(RECT rc)
{
    int w = rc.right - settings_content_x() - S(120);
    return w > S(660) ? S(660) : w;
}

/* The custom status box sits on the account page only. */
static void place_settings_edit(void)
{
    RECT rc;
    int show = g_ui.settings_open && g_ui.settings_page == SET_ACCOUNT;

    if (!g_ui.settings_edit)
        return;
    GetClientRect(g_ui.wnd, &rc);
    SetWindowPos(g_ui.settings_edit, HWND_TOP, settings_content_x() + S(14), g_ui.settings_edit_y + S(10),
                 settings_content_w(rc) - S(216) - S(28), S(20), SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

static void paint_button(int x, int y, int w, const char *label, int id, int primary)
{
    int hover = g_ui.settings_hover == id;
    unsigned fillc = primary ? (hover ? 0xFF4752C4u : ARGB(C_BRAND)) : (hover ? 0xFF2E2E31u : 0xFF242426u);

    r_round(x, y, w, S(38), S(6), fillc);
    text(g_ui.f_h, primary ? C_RAIL : C_INK, rect(x, y, w, S(38)), label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    set_hit(x, y, w, S(38), id);
}

/* A box showing the current choice, opening a menu. */
static void paint_dropdown(int x, int y, int w, const wchar_t *label, int id)
{
    r_round(x, y, w, S(40), S(6), g_ui.settings_hover == id ? 0xFF1E1E20u : 0xFF121214u);
    text_w(g_ui.f_body, C_INK, rect(x + S(12), y, w - S(48), S(40)), label, -1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    text_w(g_ui.f_icon, C_MUTED, rect(x + w - S(36), y, S(24), S(40)), L"\xE70D", -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    set_hit(x, y, w, S(40), id);
}

/* A horizontal slider; `meter`, when above min, shows a level along the track (green past the value). */
static void paint_slider(int x, int y, int w, int value, int min, int max, int meter, int id)
{
    int ty = y + S(10), kx = x + (int)((long long)(value - min) * w / (max - min));

    r_round(x, ty, w, S(8), S(4), 0xFF2E2E31u);
    if (meter > min) {
        int mx = x + (int)((long long)(meter - min) * w / (max - min));
        r_round(x, ty, mx - x, S(8), S(4), meter > value ? ARGB(C_GREEN) : 0xFF6A6A6A);
    } else if (meter <= -1000) {
        r_round(x, ty, kx - x, S(8), S(4), ARGB(C_BRAND));
    }
    r_round(kx - S(5), y + S(2), S(10), S(24), S(3), g_ui.settings_hover == id || g_ui.set_drag == id ? 0xFFFFFFFFu : 0xFFE0E0E0u);
    set_hit(x - S(8), y, w + S(16), S(28), id);
}

/* A choice among a few, as a round radio button. */
static int paint_radio(int x, int y, int w, const char *label, int on, int id)
{
    if (g_ui.settings_hover == id || on)
        r_round(x, y, w, S(40), S(6), on ? ARGB(C_SELECT) : ARGB(C_HOVER));
    r_round_outline(x + S(12), y + S(10), S(20), S(20), S(10), S(2), on ? ARGB(C_BRAND) : ARGB(C_MUTED));
    if (on)
        r_circle(x + S(17), y + S(15), S(10), ARGB(C_BRAND));
    text(g_ui.f_body, on ? C_INK : C_MUTED, rect(x + S(44), y, w - S(56), S(40)), label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    set_hit(x, y, w, S(40), id);
    return y + S(44);
}

static void key_name(int vk, wchar_t *out, int n)
{
    UINT sc;

    switch (vk) {
    case 0:
        lstrcpynW(out, L"No keybind set", n);
        return;
    case VK_XBUTTON1:
        lstrcpynW(out, L"Mouse 4", n);
        return;
    case VK_XBUTTON2:
        lstrcpynW(out, L"Mouse 5", n);
        return;
    case VK_MBUTTON:
        lstrcpynW(out, L"Middle Mouse", n);
        return;
    }
    sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    switch (vk) { /* the keys whose scan codes need the extended bit to be named apart from the keypad */
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_RCONTROL: case VK_RMENU: case VK_DIVIDE:
        sc |= 0x100;
    }
    if (!GetKeyNameTextW((LONG)(sc << 16), out, n))
        wsprintfW(out, L"Key %d", vk);
}

/* A setting with its explanation and a switch on the right. */
static int paint_toggle(int x, int y, int w, const char *title, const char *desc, int on, int id)
{
    int sx = x + w - S(44), sy = y + S(8);

    text(g_ui.f_h, C_INK, rect(x, y + S(4), w - S(64), S(22)), title, DT_LEFT | DT_SINGLELINE);
    text(g_ui.f_small, C_MUTED, rect(x, y + S(28), w - S(64), S(36)), desc, DT_LEFT | DT_WORDBREAK);
    r_round(sx, sy, S(44), S(24), S(12), on ? ARGB(C_BRAND) : 0xFF4E5058u);
    r_circle(on ? sx + S(22) : sx + S(2), sy + S(2), S(20), 0xFFFFFFFFu);
    set_hit(x, y, w, S(SET_ROW), id);
    fill(x, y + S(SET_ROW) - S(12), w, 1, C_LINE);
    return y + S(SET_ROW);
}

static void paint_settings(RECT rc)
{
    static const char *const pages[] = {"My Account", "Voice & Video", "Notifications", "Advanced", "About"};
    int nav = S(SET_NAV_W), x = settings_content_x(), w = settings_content_w(rc), y;

    g_ui.nset_hits = 0;
    fill(0, 0, rc.right, rc.bottom, C_MAIN);
    fill(0, 0, nav, rc.bottom, C_SIDE);

    /* Sections, then logging out, as in Discord's sidebar. */
    y = S(56);
    for (int k = 0; k < SET_PAGES; k++) {
        int sel = g_ui.settings_page == k, hov = g_ui.settings_hover == SH_PAGE + k;
        if (k == 0 || k == SET_VOICE || k == SET_ABOUT) {
            if (k)
                y += S(12);
            text(g_ui.f_cat, C_CHANNEL, rect(S(28), y, nav - S(40), S(28)), /* headers: channels-default */
                 k == 0 ? "USER SETTINGS" : k == SET_VOICE ? "APP SETTINGS" : "SILICORD", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            y += S(30);
        }
        /* Discord's items: 6 above and below a 24px line, 10 in, rounded 4, 2 apart; text-subtle, text-strong when on */
        if (sel || hov)
            r_round(S(16), y, nav - S(32), S(36), S(4), sel ? ARGB(C_SELECT) : ARGB(C_HOVER));
        text(g_ui.f_chan, sel || hov ? C_INK : C_MUTED, rect(S(26), y, nav - S(52), S(36)), pages[k],
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        set_hit(S(16), y, nav - S(32), S(36), SH_PAGE + k);
        y += S(38);
    }
    fill(S(28), y + S(8), nav - S(56), 1, C_LINE);
    y += S(20);
    if (g_ui.settings_hover == SH_LOGOUT)
        r_round(S(16), y, nav - S(32), S(36), S(4), ARGB(C_HOVER));
    text(g_ui.f_chan, C_INK, rect(S(26), y, nav - S(52), S(36)), "Log Out", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    text_w(g_ui.f_icon, C_MUTED, rect(nav - S(52), y, S(24), S(36)), ICON_POWER, -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    set_hit(S(16), y, nav - S(32), S(36), SH_LOGOUT);

    /* Close, with the Esc hint under it. */
    {
        int cx = x + w + S(40), cy = S(60);
        r_round_outline(cx, cy, S(36), S(36), S(18), S(2) > 1 ? S(2) : 1,
                        g_ui.settings_hover == SH_CLOSE ? ARGB(C_INK) : ARGB(C_MUTED));
        text_w(g_ui.f_icon, g_ui.settings_hover == SH_CLOSE ? C_INK : C_MUTED, rect(cx, cy, S(36), S(36)), L"\xE711", -1,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(g_ui.f_cat, g_ui.settings_hover == SH_CLOSE ? C_INK : C_MUTED, rect(cx - S(8), cy + S(40), S(52), S(20)), "ESC",
             DT_CENTER | DT_SINGLELINE); /* interactive-text-default, as the button */
        set_hit(cx, cy, S(36), S(36), SH_CLOSE);
    }

    y = S(60);
    text(g_ui.f_title, C_INK, rect(x, y, w, S(32)), pages[g_ui.settings_page], DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y += S(52);
    switch (g_ui.settings_page) {
    case SET_ACCOUNT: {
        const char *name = g_ui.model && g_ui.model->user_name ? model_str(g_ui.model, g_ui.model->user_name) : "";
        const char *custom = g_ui.model ? model_str(g_ui.model, g_ui.model->custom_status) : "";
        r_image_t *img = g_ui.model ? user_avatar(g_ui.model->user_id, g_ui.model->user_avatar) : NULL;
        r_round(x, y, w, S(112), S(8), 0xFF121214u);
        if (img)
            r_image(img, x + S(20), y + S(16), S(80), S(80), S(40));
        else
            r_circle(x + S(20), y + S(16), S(80), ARGB(C_ITEM));
        paint_status(x + S(20), y + S(16), S(80), g_ui.my_status, 0xFF121214u);
        text(g_ui.f_title, C_INK, rect(x + S(120), y + S(28), w - S(140), S(30)), name, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        text(g_ui.f_body, C_MUTED, rect(x + S(120), y + S(60), w - S(140), S(24)), custom[0] ? custom : status_name(),
             DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += S(136);
        text(g_ui.f_cat, C_FAINT, rect(x, y, w, S(20)), "STATUS", DT_LEFT | DT_SINGLELINE);
        y += S(28);
        for (int k = 0; k < 4; k++) {
            int sel = g_ui.my_status == k_status_states[k], hov = g_ui.settings_hover == SH_STATUS + k;
            if (sel || hov)
                r_round(x, y, w, S(40), S(6), sel ? ARGB(C_SELECT) : ARGB(C_HOVER));
            status_dot(x + S(16), y + S(12), S(16), k_status_states[k], sel ? ARGB(C_SELECT) : hov ? ARGB(C_HOVER) : ARGB(C_MAIN));
            text(g_ui.f_body, sel ? C_INK : C_MUTED, rect(x + S(48), y, w - S(60), S(40)), k_status_names[k],
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            set_hit(x, y, w, S(40), SH_STATUS + k);
            y += S(42);
        }
        y += S(24);
        text(g_ui.f_cat, C_FAINT, rect(x, y, w, S(20)), "CUSTOM STATUS", DT_LEFT | DT_SINGLELINE);
        y += S(28);
        r_round(x, y, w - S(216), S(40), S(8), 0xFF121214u);
        if (g_ui.settings_edit_y != y) {
            g_ui.settings_edit_y = y;
            place_settings_edit();
        }
        paint_button(x + w - S(204), y + S(1), S(96), "Save", SH_SAVE, 1);
        paint_button(x + w - S(100), y + S(1), S(96), "Clear", SH_CLEAR, 0);
        break;
    }
    case SET_VOICE: {
        const voice_prefs_t *v = &g_ui.vprefs;
        int col = (w - S(24)) / 2, x2 = x + col + S(24), level = app_voice_mic_level();
        wchar_t label[64];
        char line[40];
        text(g_ui.f_cat, C_FAINT, rect(x, y, col, S(20)), "INPUT DEVICE", DT_LEFT | DT_SINGLELINE);
        text(g_ui.f_cat, C_FAINT, rect(x2, y, col, S(20)), "OUTPUT DEVICE", DT_LEFT | DT_SINGLELINE);
        y += S(26);
        paint_dropdown(x, y, col, v->in_device ? g_ui.dev_in[v->in_device - 1] : L"Default", SH_IN_DEV);
        paint_dropdown(x2, y, col, v->out_device ? g_ui.dev_out[v->out_device - 1] : L"Default", SH_OUT_DEV);
        y += S(64);
        text(g_ui.f_cat, C_FAINT, rect(x, y, col, S(20)), "INPUT VOLUME", DT_LEFT | DT_SINGLELINE);
        text(g_ui.f_cat, C_FAINT, rect(x2, y, col, S(20)), "OUTPUT VOLUME", DT_LEFT | DT_SINGLELINE);
        wsprintfA(line, "%d%%", v->in_volume);
        text(g_ui.f_small, C_MUTED, rect(x, y, col, S(20)), line, DT_RIGHT | DT_SINGLELINE);
        wsprintfA(line, "%d%%", v->out_volume);
        text(g_ui.f_small, C_MUTED, rect(x2, y, col, S(20)), line, DT_RIGHT | DT_SINGLELINE);
        y += S(28);
        paint_slider(x, y, col, v->in_volume, 0, 200, -1000, SH_IN_VOL);
        paint_slider(x2, y, col, v->out_volume, 0, 200, -1000, SH_OUT_VOL);
        y += S(52);

        text(g_ui.f_cat, C_FAINT, rect(x, y, w, S(20)), "MIC TEST", DT_LEFT | DT_SINGLELINE);
        y += S(24);
        text(g_ui.f_small, C_MUTED, rect(x, y, w, S(20)),
             g_ui.voice_state == VOICE_CONNECTED ? "Your microphone, as your call hears it." :
                                                   "Say something and you will hear it back.", DT_LEFT | DT_SINGLELINE);
        y += S(28);
        if (g_ui.voice_state != VOICE_CONNECTED)
            paint_button(x, y, S(140), g_ui.mic_test ? "Stop Testing" : "Let's Check", SH_MIC_TEST, !g_ui.mic_test);
        {
            int mx = g_ui.voice_state != VOICE_CONNECTED ? x + S(160) : x, mw = x + w - mx, lit = (level + 100) * mw / 100;
            r_round(mx, y + S(15), mw, S(8), S(4), 0xFF2E2E31u);
            if (level > -100)
                r_round(mx, y + S(15), lit, S(8), S(4), ARGB(C_GREEN));
        }
        y += S(64);

        text(g_ui.f_cat, C_FAINT, rect(x, y, w, S(20)), "INPUT MODE", DT_LEFT | DT_SINGLELINE);
        y += S(26);
        y = paint_radio(x, y, w, "Voice Activity", !v->push_to_talk, SH_MODE_VOICE);
        y = paint_radio(x, y, w, "Push to Talk", v->push_to_talk, SH_MODE_PTT);
        y += S(16);
        if (v->push_to_talk) {
            text(g_ui.f_cat, C_FAINT, rect(x, y, w, S(20)), "SHORTCUT", DT_LEFT | DT_SINGLELINE);
            y += S(26);
            if (g_ui.set_record)
                lstrcpyW(label, L"Press a key or a mouse button\x2026");
            else
                key_name(v->ptt_key, label, ARRAYSIZE(label));
            r_round(x, y, col, S(40), S(8), 0xFF121214u);
            if (g_ui.set_record)
                r_round_outline(x, y, col, S(40), S(6), 1, ARGB(C_BRAND));
            text_w(g_ui.f_body, g_ui.set_record ? C_BRAND : C_INK, rect(x + S(12), y, col - S(24), S(40)), label, -1,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            set_hit(x, y, col, S(40), SH_PTT_KEY);
            paint_button(x + col + S(12), y + S(1), S(160), g_ui.set_record ? "Stop Recording" : "Record Keybind", SH_PTT_KEY, 0);
            y += S(52);
            text(g_ui.f_small, C_MUTED, rect(x, y, w, S(20)), "Works while Silicord is in the background.",
                 DT_LEFT | DT_SINGLELINE);
        } else {
            text(g_ui.f_cat, C_FAINT, rect(x, y, w, S(20)), "INPUT SENSITIVITY", DT_LEFT | DT_SINGLELINE);
            wsprintfA(line, "%d dB", v->sensitivity);
            text(g_ui.f_small, C_MUTED, rect(x, y, w, S(20)), line, DT_RIGHT | DT_SINGLELINE);
            y += S(28);
            paint_slider(x, y, w, v->sensitivity, -100, 0, level, SH_SENS);
            y += S(40);
            text(g_ui.f_small, C_MUTED, rect(x, y, w, S(20)),
                 "The microphone opens when its level passes the mark (green on the meter).", DT_LEFT | DT_SINGLELINE);
        }
        break;
    }
    case SET_NOTIFICATIONS:
        y = paint_toggle(x, y, w, "Enable Desktop Notifications",
                         "A Windows notification for direct messages and mentions, following each server's notification settings.",
                         g_ui.pref_notify, SH_NOTIFY);
        paint_toggle(x, y, w, "Unread Count in the Title",
                     "The number of unread mentions in the window title and on the taskbar.", g_ui.pref_title, SH_TITLE);
        break;
    case SET_ADVANCED:
        paint_toggle(x, y, w, "Developer Mode",
                     "Adds Copy ID to the menus of servers, channels, messages and users. Synced with your other Discord apps.",
                     developer_mode(), SH_DEVELOPER);
        break;
    case SET_ABOUT: {
        PROCESS_MEMORY_COUNTERS_EX pmc = {sizeof pmc};
        char line[160];
        K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc);
        text(g_ui.f_h, C_INK, rect(x, y, w, S(24)), "Silicord " SILICORD_VERSION, DT_LEFT | DT_SINGLELINE);
        y += S(30);
        text(g_ui.f_body, C_MUTED, rect(x, y, w, S(48)),
             "A native Discord client for Windows, in C and x64 assembly. No browser, no C runtime.", DT_LEFT | DT_WORDBREAK);
        y += S(56);
        wsprintfA(line, "Memory: %u KB used by Silicord, %u KB for the whole process.", (unsigned)(mem_used() >> 10),
                  (unsigned)(pmc.PrivateUsage >> 10));
        text(g_ui.f_body, C_MUTED, rect(x, y, w, S(24)), line, DT_LEFT | DT_SINGLELINE);
        y += S(26);
        {
            sb_t net = {0}, cpu = {0};
            usage_lines(&net, &cpu);
            sb_add(&net, ".");
            sb_add(&cpu, ".");
            net.data[0] = (char)(net.data[0] >= 'a' && net.data[0] <= 'z' ? net.data[0] - 32 : net.data[0]);
            text(g_ui.f_body, C_MUTED, rect(x, y, w, S(48)), net.data, DT_LEFT | DT_WORDBREAK);
            y += S(48);
            text(g_ui.f_body, C_MUTED, rect(x, y, w, S(24)), cpu.data, DT_LEFT | DT_SINGLELINE);
            sb_free(&net);
            sb_free(&cpu);
        }
        y += S(44);
        paint_button(x, y, S(160), "Source Code", SH_SOURCE, 0);
        break;
    }
    }
}

static LRESULT CALLBACK settings_edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

static void settings_open(void)
{
    if (!g_ui.settings_edit) {
        g_ui.settings_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, g_ui.wnd, NULL, NULL, NULL);
        g_ui.settings_edit_proc = (WNDPROC)SetWindowLongPtrW(g_ui.settings_edit, GWLP_WNDPROC, (LONG_PTR)settings_edit_proc);
        SendMessageW(g_ui.settings_edit, WM_SETFONT, SendMessageW(g_ui.composer, WM_GETFONT, 0, 0), FALSE);
        SendMessageW(g_ui.settings_edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Set a custom status");
        SendMessageW(g_ui.settings_edit, EM_LIMITTEXT, 128, 0);
    }
    {
        const char *custom = g_ui.model ? model_str(g_ui.model, g_ui.model->custom_status) : "";
        wchar_t *w = utf8_to_wide(custom, lstrlenA(custom));
        SetWindowTextW(g_ui.settings_edit, w);
        mem_free(w);
    }
    pop_close();
    picker_close();
    pins_close();
    g_ui.settings_open = 1;
    g_ui.settings_hover = -1;
    g_ui.settings_edit_y = -1;
    ShowWindow(g_ui.composer, SW_HIDE);
    if (g_ui.search_edit)
        ShowWindow(g_ui.search_edit, SW_HIDE);
    if (g_ui.friend_edit)
        ShowWindow(g_ui.friend_edit, SW_HIDE);
    if (g_ui.friend_search)
        ShowWindow(g_ui.friend_search, SW_HIDE);
    SetFocus(g_ui.wnd);
    redraw();
}

/* From the user panel's menus: the voice settings. */
static void settings_open_voice(void)
{
    settings_open();
    g_ui.settings_page = SET_VOICE;
    redraw();
}

static void settings_close(void)
{
    g_ui.settings_open = 0;
    g_ui.set_record = 0;
    g_ui.mic_test = app_voice_mic_test(0);
    KillTimer(g_ui.wnd, TIMER_MIC);
    place_settings_edit();
    place_composer();
    place_friend_input();
    place_search();
    dm_profile_sync();
    redraw();
}

/* Custom status from the box: set here and on every client. */
static void save_custom_status(int clear)
{
    wchar_t w[140];
    sb_t text = {0}, fields = {0};

    if (clear)
        SetWindowTextW(g_ui.settings_edit, L"");
    GetWindowTextW(g_ui.settings_edit, w, ARRAYSIZE(w));
    wide_to_utf8(w, (size_t)lstrlenW(w), &text);
    app_set_status(status_code(), text.data);
    if (text.len) {
        sb_add(&fields, "\"custom_status\":{\"text\":");
        sb_json_str(&fields, text.data, text.len);
        sb_add(&fields, "}");
    } else {
        sb_add(&fields, "\"custom_status\":null");
    }
    app_user_settings(fields.data);
    sb_free(&text);
    sb_free(&fields);
}

/* A slider follows the mouse: its value from the x position in its track. */
static void slider_drag(int id, int x)
{
    voice_prefs_t *v = &g_ui.vprefs;
    int min = id == SH_SENS ? -100 : 0, max = id == SH_SENS ? 0 : 200, value;

    for (int k = 0; k < g_ui.nset_hits; k++) {
        RECT r = g_ui.set_hits[k].r;
        int left = r.left + S(8), w = r.right - r.left - S(16);
        if (g_ui.set_hits[k].id != id || w <= 0)
            continue;
        value = min + (int)((long long)(x - left) * (max - min) / w);
        value = value < min ? min : value > max ? max : value;
        /* volumes snap to 100% */
        if (id != SH_SENS && value > 95 && value < 105)
            value = 100;
        *(id == SH_IN_VOL ? &v->in_volume : id == SH_OUT_VOL ? &v->out_volume : &v->sensitivity) = value;
        app_voice_prefs(v);
        redraw();
        return;
    }
}

/* A device menu under its box: Default, then what Windows has. */
static void pick_device(int input)
{
    HMENU menu = CreatePopupMenu();
    int n, cur, cmd;
    wchar_t (*names)[AUDIO_NAME];

    list_devices();
    n = input ? g_ui.ndev_in : g_ui.ndev_out;
    names = input ? g_ui.dev_in : g_ui.dev_out;
    cur = input ? g_ui.vprefs.in_device : g_ui.vprefs.out_device;
    if (cur > n)
        cur = 0;
    AppendMenuW(menu, MF_STRING | (cur == 0 ? MF_CHECKED : 0), 1, L"Default");
    for (int i = 0; i < n; i++)
        AppendMenuW(menu, MF_STRING | (cur == i + 1 ? MF_CHECKED : 0), (UINT_PTR)(i + 2), names[i]);
    menu_mark(menu, 0, MENU_RADIO);
    cmd = run_menu(menu);
    if (cmd > 0) {
        *(input ? &g_ui.vprefs.in_device : &g_ui.vprefs.out_device) = cmd - 1;
        app_voice_prefs(&g_ui.vprefs);
        prefs_save();
    }
}

static void settings_set_key(int vk)
{
    g_ui.vprefs.ptt_key = vk;
    app_voice_prefs(&g_ui.vprefs);
    prefs_save();
}

static void settings_click(int x, int y)
{
    int id = settings_hit(x, y);

    if (id != SH_PTT_KEY)
        g_ui.set_record = 0;
    if (id < 0) {
        redraw();
        return;
    }
    if (id >= SH_PAGE && id < SH_PAGE + SET_PAGES) {
        g_ui.settings_page = id - SH_PAGE;
        g_ui.settings_edit_y = -1;
        place_settings_edit();
        if (g_ui.settings_page == SET_VOICE) {
            list_devices();
            SetTimer(g_ui.wnd, TIMER_MIC, 50, NULL);
        } else {
            g_ui.mic_test = app_voice_mic_test(0);
            KillTimer(g_ui.wnd, TIMER_MIC);
        }
    } else if (id >= SH_STATUS && id < SH_STATUS + 4) {
        set_status(id - SH_STATUS);
    } else {
        switch (id) {
        case SH_CLOSE:
            settings_close();
            return;
        case SH_LOGOUT:
            if (MessageBoxW(g_ui.wnd, L"Are you sure you want to log out?", L"Log Out",
                            MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) == IDOK) {
                settings_close();
                app_logout();
            }
            return;
        case SH_SAVE:
        case SH_CLEAR:
            save_custom_status(id == SH_CLEAR);
            break;
        case SH_NOTIFY:
            g_ui.pref_notify = !g_ui.pref_notify;
            prefs_save();
            break;
        case SH_TITLE:
            g_ui.pref_title = !g_ui.pref_title;
            prefs_save();
            update_title();
            break;
        case SH_DEVELOPER:
            if (g_ui.model) {
                g_ui.model->developer_mode = !g_ui.model->developer_mode;
                app_user_settings(g_ui.model->developer_mode ? "\"developer_mode\":true" : "\"developer_mode\":false");
            }
            break;
        case SH_SOURCE:
            open_url("https://github.com/mikketa/silicord");
            break;
        case SH_IN_DEV:
        case SH_OUT_DEV:
            pick_device(id == SH_IN_DEV);
            break;
        case SH_MIC_TEST:
            g_ui.mic_test = app_voice_mic_test(!g_ui.mic_test);
            break;
        case SH_MODE_VOICE:
        case SH_MODE_PTT:
            g_ui.vprefs.push_to_talk = id == SH_MODE_PTT;
            app_voice_prefs(&g_ui.vprefs);
            prefs_save();
            break;
        case SH_PTT_KEY:
            g_ui.set_record = !g_ui.set_record;
            break;
        }
    }
    redraw();
}

static LRESULT CALLBACK settings_edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        settings_close();
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        save_custom_status(0);
        redraw();
        return 0;
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE))
        return 0;
    return CallWindowProcW(g_ui.settings_edit_proc, h, msg, wp, lp);
}

/* ---- Right-click menus ---- */

enum {
    CM_REACT = 1, CM_REPLY, CM_EDIT, CM_DELETE, CM_COPY_TEXT, CM_COPY_LINK, CM_COPY_ID,
    CM_MARK_READ, CM_MUTE, CM_UNMUTE, CM_LEAVE, CM_PROFILE, CM_MESSAGE, CM_COPY_USERNAME, CM_COPY_USER_ID,
    CM_SUPPRESS_EVERYONE, CM_SUPPRESS_ROLES, CM_FORWARD, CM_PIN, CM_MARK_UNREAD, CM_THREAD,
    CM_USER_MUTE,
    CM_USER_VOLUME = 90, /* + index in k_user_volumes */
    CM_MUTE_FOR = 100,   /* + index in k_mute_minutes */
    CM_NOTIFY = 120,     /* + NOTIFY_* */
};

static const int k_mute_minutes[] = {15, 60, 180, 480, 1440, 0};
static const int k_user_volumes[] = {200, 150, 125, 100, 75, 50, 25, 10};
static const wchar_t *const k_mute_names[] = {L"For 15 Minutes", L"For 1 Hour", L"For 3 Hours", L"For 8 Hours",
                                              L"For 24 Hours", L"Until I turn it back on"};

/* "Mute ..." with its durations, or "Unmute ..." when muted. */
static void add_mute_items(HMENU menu, int muted, const wchar_t *what)
{
    wchar_t label[48];

    if (muted) {
        wsprintfW(label, L"Unmute %s", what);
        AppendMenuW(menu, MF_STRING, CM_UNMUTE, label);
    } else {
        HMENU sub = CreatePopupMenu();
        for (int k = 0; k < (int)ARRAYSIZE(k_mute_minutes); k++)
            AppendMenuW(sub, MF_STRING, CM_MUTE_FOR + k, k_mute_names[k]);
        wsprintfW(label, L"Mute %s", what);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, label);
    }
}

/* Notification Settings: the levels, ticked at `current`; `inherit` names the default entry for channels. */
static void notify_items(HMENU sub, int current, const wchar_t *inherit);

static HMENU notify_menu(int current, const wchar_t *inherit)
{
    HMENU sub = CreatePopupMenu();

    notify_items(sub, current, inherit);
    return sub;
}

static void notify_items(HMENU sub, int current, const wchar_t *inherit)
{

    if (inherit)
        AppendMenuW(sub, MF_STRING | (current == NOTIFY_DEFAULT ? MF_CHECKED : 0), CM_NOTIFY + NOTIFY_DEFAULT, inherit);
    AppendMenuW(sub, MF_STRING | (current == NOTIFY_ALL ? MF_CHECKED : 0), CM_NOTIFY + NOTIFY_ALL, L"All Messages");
    AppendMenuW(sub, MF_STRING | (current == NOTIFY_MENTIONS ? MF_CHECKED : 0), CM_NOTIFY + NOTIFY_MENTIONS,
                L"Only @mentions");
    AppendMenuW(sub, MF_STRING | (current == NOTIFY_NOTHING ? MF_CHECKED : 0), CM_NOTIFY + NOTIFY_NOTHING, L"Nothing");
    menu_mark(sub, 0, MENU_RADIO);
}

/* The JSON Discord takes for a NOTIFY_* level. */
static void notify_fields(int level, char *out)
{
    wsprintfA(out, "\"message_notifications\":%d", level == NOTIFY_DEFAULT ? 3 : level - NOTIFY_ALL);
}

static int run_menu(HMENU menu)
{
    POINT pt;
    int cmd;

    if (g_menu_at_set)
        pt = g_menu_at;
    else
        GetCursorPos(&pt);
    cmd = menu_track(menu, pt.x, pt.y);
    DestroyMenu(menu);
    return cmd;
}

static void copy_link(const char *guild, const char *channel, const char *message)
{
    char link[160];

    if (message)
        wsprintfA(link, "https://discord.com/channels/%s/%s/%s", guild ? guild : "@me", channel, message);
    else
        wsprintfA(link, "https://discord.com/channels/%s/%s", guild ? guild : "@me", channel);
    copy_text(link);
}

/* Pinning needs Manage Messages or Pin Messages, except in DMs. */
static int can_pin(void)
{
    return g_ui.channel >= 0 &&
           (model_permissions(g_ui.model, (unsigned)g_ui.channel) & (PERM_MANAGE_MESSAGES | PERM_PIN_MESSAGES)) != 0;
}

/* Message i and those after it become unread: the NEW line goes above it and the read mark just before. */
static void mark_unread(int i)
{
    channel_t *c;
    char before[24];

    if (g_ui.channel < 0)
        return;
    c = &g_ui.model->channels[g_ui.channel];
    if (i > 0) {
        lstrcpynA(before, g_ui.msgs[i - 1].id, sizeof before);
    } else {
        /* The id one below: nothing older is loaded, and any smaller snowflake does. */
        unsigned long long id = 0;
        for (const char *p = g_ui.msgs[i].id; *p >= '0' && *p <= '9'; p++)
            id = id * 10 + (unsigned long long)(*p - '0');
        wsprintfA(before, "%I64u", id ? id - 1 : 0);
    }
    lstrcpynA(c->read, before, sizeof c->read);
    for (int k = 0; k < g_ui.nmsgs; k++)
        g_ui.msgs[k].first_new = k == i;
    app_ack_manual(c->id, before);
    update_title();
}

static void message_menu(int i)
{
    HMENU menu = CreatePopupMenu();
    msg_t *m = &g_ui.msgs[i];
    int own = own_message(m), cmd;
    char id[24];

    /* Discord's order: the reaction, what answers, what copies or marks, the deletion, the ID */
    AppendMenuW(menu, MF_STRING, CM_REACT, L"Add Reaction");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    if (own)
        AppendMenuW(menu, MF_STRING, CM_EDIT, L"Edit Message");
    AppendMenuW(menu, MF_STRING, CM_REPLY, L"Reply");
    AppendMenuW(menu, MF_STRING, CM_FORWARD, L"Forward");
    if (g_ui.guild >= 0 && g_ui.channel >= 0 && !model_is_thread(chan(g_ui.channel)->type))
        AppendMenuW(menu, MF_STRING, CM_THREAD, L"Create Thread");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    if (m->content.len)
        AppendMenuW(menu, MF_STRING, CM_COPY_TEXT, L"Copy Text");
    if (can_pin())
        AppendMenuW(menu, MF_STRING, CM_PIN, m->pinned ? L"Unpin Message" : L"Pin Message");
    AppendMenuW(menu, MF_STRING, CM_MARK_UNREAD, L"Mark Unread");
    AppendMenuW(menu, MF_STRING, CM_COPY_LINK, L"Copy Message Link");
    if (own) {
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, CM_DELETE, L"Delete Message");
    }
    if (developer_mode()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, CM_COPY_ID, L"Copy Message ID");
    }
    lstrcpynA(id, m->id, sizeof id);
    cmd = run_menu(menu);
    if ((i = find_msg(id)) < 0) /* it may have gone while the menu was open */
        return;
    m = &g_ui.msgs[i];
    switch (cmd) {
    case CM_REACT: {
        RECT a = message_area();
        picker_open(PICK_REACTION, m->id, a.right - S(24), msg_top(i) + S(PICK_H) / 2);
        break;
    }
    case CM_REPLY:
        start_reply(i);
        break;
    case CM_EDIT:
        start_edit(i);
        break;
    case CM_COPY_TEXT:
        copy_text(m->content.data);
        break;
    case CM_COPY_LINK:
        copy_link(open_guild_id(), g_ui.msgs_channel, m->id);
        break;
    case CM_PIN:
        m->pinned = !m->pinned;
        app_pin(g_ui.msgs_channel, m->id, m->pinned);
        break;
    case CM_MARK_UNREAD:
        mark_unread(i);
        break;
    case CM_THREAD:
        prompt_open(PROMPT_THREAD, m->id, g_ui.msgs_channel, L"Thread name");
        break;
    case CM_FORWARD:
        forward_open(m->id);
        break;
    case CM_COPY_ID:
        copy_text(m->id);
        break;
    case CM_DELETE:
        g_ui.confirm = 1;
        lstrcpynA(g_ui.confirm_id, m->id, sizeof g_ui.confirm_id);
        break;
    }
    redraw();
}

/* `bell`: the header's bell menu, Discord's: muting, then the notification levels inline. */
static void channel_menu_as(int i, int bell)
{
    HMENU menu = CreatePopupMenu();
    const channel_t *c = chan(i);
    int muted = c->muted && (!c->mute_until || c->mute_until > now_ms()), cmd, g = model_channel_guild(g_ui.model, (unsigned)i);
    char id[24];

    if (bell) {
        add_mute_items(menu, muted, L"Channel");
        if (g >= 0) {
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            notify_items(menu, c->notify, c->parent[0] ? L"Use Category Default" : L"Use Server Default");
        }
    } else if (c->type == CH_CATEGORY) {
        add_mute_items(menu, muted, L"Category");
        if (g >= 0)
            AppendMenuW(menu, MF_POPUP, (UINT_PTR)notify_menu(c->notify, L"Use Server Default"), L"Notification Settings");
        if (developer_mode()) {
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(menu, MF_STRING, CM_COPY_ID, L"Copy Category ID");
        }
    } else {
        AppendMenuW(menu, MF_STRING | (model_unread(g_ui.model, (unsigned)i) || c->mentions ? 0 : MF_GRAYED), CM_MARK_READ,
                    L"Mark As Read");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        add_mute_items(menu, muted, is_dm_type(c->type) ? L"Conversation" : L"Channel");
        if (g >= 0)
            AppendMenuW(menu, MF_POPUP, (UINT_PTR)notify_menu(c->notify, c->parent[0] ? L"Use Category Default" : L"Use Server Default"),
                        L"Notification Settings");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, CM_COPY_LINK, L"Copy Link");
        if (developer_mode())
            AppendMenuW(menu, MF_STRING, CM_COPY_ID, L"Copy Channel ID");
    }
    lstrcpynA(id, c->id, sizeof id);
    cmd = run_menu(menu);
    /* The menu ran a message loop: the model may have changed, look both up again. */
    if ((i = model_find_channel(g_ui.model, id)) < 0)
        return;
    g = model_channel_guild(g_ui.model, (unsigned)i);
    switch (cmd) {
    case CM_MARK_READ:
        mark_read(i);
        break;
    case CM_UNMUTE:
        g_ui.model->channels[i].muted = 0;
        app_mute(g >= 0 ? g_ui.model->guilds[g].id : NULL, id, 0, 0);
        break;
    case CM_COPY_LINK:
        copy_link(g >= 0 ? g_ui.model->guilds[g].id : NULL, id, NULL);
        break;
    case CM_COPY_ID:
        copy_text(id);
        break;
    default:
        if (cmd >= CM_MUTE_FOR && cmd < CM_MUTE_FOR + (int)ARRAYSIZE(k_mute_minutes)) {
            int minutes = k_mute_minutes[cmd - CM_MUTE_FOR];
            g_ui.model->channels[i].muted = 1;
            g_ui.model->channels[i].mute_until = minutes ? now_ms() + minutes * 60000ll : 0;
            app_mute(g >= 0 ? g_ui.model->guilds[g].id : NULL, id, 1, minutes);
        } else if (cmd >= CM_NOTIFY && cmd <= CM_NOTIFY + NOTIFY_NOTHING && g >= 0) {
            char fields[48];
            g_ui.model->channels[i].notify = cmd - CM_NOTIFY;
            notify_fields(cmd - CM_NOTIFY, fields);
            app_notify_settings(g_ui.model->guilds[g].id, id, fields);
        }
        break;
    }
    update_title();
    redraw();
}

static void channel_menu(int i)
{
    channel_menu_as(i, 0);
}

static void guild_menu(int g)
{
    HMENU menu = CreatePopupMenu();
    guild_t *gd = &g_ui.model->guilds[g];
    int unread, mentions, cmd;
    char id[24];

    guild_state(g, &unread, &mentions);
    AppendMenuW(menu, MF_STRING | (unread || mentions ? 0 : MF_GRAYED), CM_MARK_READ, L"Mark As Read");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    add_mute_items(menu, model_guild_muted(g_ui.model, g, now_ms()), L"Server");
    {
        HMENU sub = notify_menu(gd->notify == NOTIFY_DEFAULT ? gd->default_notify : gd->notify, NULL);
        AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
        AppendMenuW(sub, MF_STRING | (gd->suppress_everyone ? MF_CHECKED : 0), CM_SUPPRESS_EVERYONE,
                    L"Suppress @everyone and @here");
        AppendMenuW(sub, MF_STRING | (gd->suppress_roles ? MF_CHECKED : 0), CM_SUPPRESS_ROLES, L"Suppress All Role @mentions");
        menu_mark(sub, CM_SUPPRESS_EVERYONE, MENU_CHECK);
        menu_mark(sub, CM_SUPPRESS_ROLES, MENU_CHECK);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, L"Notification Settings");
    }
    if (developer_mode()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, CM_COPY_ID, L"Copy Server ID");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, CM_LEAVE, L"Leave Server");
    lstrcpynA(id, gd->id, sizeof id);
    cmd = run_menu(menu);
    if ((g = model_find_guild(g_ui.model, id)) < 0)
        return;
    gd = &g_ui.model->guilds[g];
    switch (cmd) {
    case CM_MARK_READ: {
        sb_t acks = {0};
        int n = 0;
        for (unsigned c = gd->first; c < gd->first + gd->count; c++) {
            channel_t *ch = &g_ui.model->channels[c];
            if (!model_unread(g_ui.model, c) && !ch->mentions)
                continue;
            lstrcpynA(ch->read, ch->last_message, sizeof ch->read);
            ch->mentions = 0;
            sb_add(&acks, ch->id);
            sb_addn(&acks, "", 1);
            sb_add(&acks, ch->last_message);
            sb_addn(&acks, "", 1);
            n++;
        }
        if (n)
            app_ack_bulk(acks.data, n);
        sb_free(&acks);
        break;
    }
    case CM_UNMUTE:
        gd->muted = 0;
        app_mute(id, NULL, 0, 0);
        break;
    case CM_SUPPRESS_EVERYONE:
        gd->suppress_everyone = !gd->suppress_everyone;
        app_notify_settings(id, NULL, gd->suppress_everyone ? "\"suppress_everyone\":true" : "\"suppress_everyone\":false");
        break;
    case CM_SUPPRESS_ROLES:
        gd->suppress_roles = !gd->suppress_roles;
        app_notify_settings(id, NULL, gd->suppress_roles ? "\"suppress_roles\":true" : "\"suppress_roles\":false");
        break;
    case CM_COPY_ID:
        copy_text(id);
        break;
    case CM_LEAVE: {
        wchar_t text[256]; /* 99 characters of text and up to 120 of the name */
        wchar_t *name = utf8_to_wide(model_str(g_ui.model, gd->name), lstrlenA(model_str(g_ui.model, gd->name)));
        wsprintfW(text, L"Are you sure you want to leave %.120s? You won't be able to rejoin this server unless you are re-invited.", name);
        mem_free(name);
        if (MessageBoxW(g_ui.wnd, text, L"Leave Server", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK)
            app_leave_guild(id);
        break;
    }
    default:
        if (cmd >= CM_MUTE_FOR && cmd < CM_MUTE_FOR + (int)ARRAYSIZE(k_mute_minutes)) {
            int minutes = k_mute_minutes[cmd - CM_MUTE_FOR];
            gd->muted = 1;
            gd->mute_until = minutes ? now_ms() + minutes * 60000ll : 0;
            app_mute(id, NULL, 1, minutes);
        } else if (cmd >= CM_NOTIFY + NOTIFY_ALL && cmd <= CM_NOTIFY + NOTIFY_NOTHING) {
            char fields[48];
            gd->notify = cmd - CM_NOTIFY;
            notify_fields(gd->notify, fields);
            app_notify_settings(id, NULL, fields);
        }
        break;
    }
    update_title();
    redraw();
}

/* `voice`: someone in a voice channel, whose volume can be set. */
static void user_menu(const char *user_id, const char *name, const char *avatar, int x, int y, int voice)
{
    HMENU menu = CreatePopupMenu();
    int cmd, self = g_ui.model && lstrcmpA(user_id, g_ui.model->user_id) == 0, u = uvol_find(user_id);
    int volume = u >= 0 ? g_ui.uvol[u].volume : 100, muted = u >= 0 && g_ui.uvol[u].muted;
    char id[24], nm[80], av[48];

    lstrcpynA(id, user_id, sizeof id);
    lstrcpynA(nm, name ? name : "", sizeof nm);
    lstrcpynA(av, avatar ? avatar : "", sizeof av);
    AppendMenuW(menu, MF_STRING, CM_PROFILE, L"Profile");
    if (!self)
        AppendMenuW(menu, MF_STRING, CM_MESSAGE, L"Message");
    if (voice && !self) {
        HMENU vol = CreatePopupMenu();
        for (int k = 0; k < (int)ARRAYSIZE(k_user_volumes); k++) {
            wchar_t label[16];
            wsprintfW(label, L"%d%%", k_user_volumes[k]);
            AppendMenuW(vol, MF_STRING | (volume == k_user_volumes[k] ? MF_CHECKED : 0), (UINT_PTR)(CM_USER_VOLUME + k), label);
        }
        menu_mark(vol, 0, MENU_RADIO);
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING | (muted ? MF_CHECKED : 0), CM_USER_MUTE, L"Mute");
        menu_mark(menu, CM_USER_MUTE, MENU_CHECK);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)vol, L"User Volume");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, CM_COPY_USERNAME, L"Copy Name");
    if (developer_mode())
        AppendMenuW(menu, MF_STRING, CM_COPY_USER_ID, L"Copy User ID");
    cmd = run_menu(menu);
    switch (cmd) {
    case CM_PROFILE:
        pop_open(id, nm, av, x, y, 0);
        break;
    case CM_MESSAGE:
        open_dm(id, NULL);
        break;
    case CM_COPY_USERNAME:
        copy_text(nm);
        break;
    case CM_COPY_USER_ID:
        copy_text(id);
        break;
    case CM_USER_MUTE:
        uvol_set(id, volume, !muted);
        break;
    default:
        if (cmd >= CM_USER_VOLUME && cmd < CM_USER_VOLUME + (int)ARRAYSIZE(k_user_volumes))
            uvol_set(id, k_user_volumes[cmd - CM_USER_VOLUME], muted);
        break;
    }
}

/* The person listed under a voice channel at (x, y), or -1. */
static int voice_user_at(int x, int y)
{
    unsigned first, count;
    int kind, index, ry = S(HEADER_H) + side_list_top() - g_ui.side_scroll, row, n = 0;

    hit_test(x, y, &kind, &index);
    if (kind != HIT_CHANNEL || !is_voice_type(chan(index)->type) || !side_range(&first, &count))
        return -1;
    for (unsigned i = first; i < (unsigned)index; i++)
        if (!empty_category(first, count, i) && (chan((int)i)->type == CH_CATEGORY || !hidden(first, i)))
            ry += row_height(i);
    if (y < ry + S(ROW_H))
        return -1;
    row = (y - ry - S(ROW_H)) / S(VOICE_ROW);
    for (int k = 0; k < g_ui.nvoices; k++)
        if (lstrcmpA(g_ui.voices[k].channel, chan(index)->id) == 0 && n++ == row)
            return k;
    return -1;
}

/* Right click anywhere in the main window. */
static void on_right_click(int x, int y)
{
    int kind, index, i, ax, ay, top;

    if (g_ui.view != VIEW_APP || !g_ui.model)
        return;
    pop_dismiss();
    picker_close();
    if ((i = ml_hit(x, y, &top)) >= 0) {
        const ml_item_t *it = &g_ui.ml.items[i];
        user_menu(it->id, it->name.data, it->avatar, main_right() - S(POP_W) - S(8), top, 0);
        return;
    }
    if (author_hit(x, y, &i, &ax, &ay)) {
        user_menu(g_ui.msgs[i].author_id, author_name(&g_ui.msgs[i]), g_ui.msgs[i].avatar, ax, ay, 0);
        return;
    }
    if ((i = voice_user_at(x, y)) >= 0) {
        voice_t *v = &g_ui.voices[i];
        user_menu(v->user, v->name.data, v->avatar, S(RAIL_W + SIDE_W) + S(8), y, 1);
        return;
    }
    if ((i = message_at(x, y, NULL)) >= 0 && !g_ui.msgs[i].system) {
        message_menu(i);
        return;
    }
    hit_test(x, y, &kind, &index);
    if (kind == HIT_CHANNEL)
        channel_menu(index);
    else if (kind == HIT_GUILD)
        guild_menu(index);
}

/* ---- Search and jumping to messages ---- */

#define RESULTS_W 460

/* Opens channel `channel_id` at message `message_id` (loading the messages around it). */
static void navigate_to_message(const char *channel_id, const char *message_id)
{
    int c = g_ui.model ? model_find_channel(g_ui.model, channel_id) : -1, i;

    if (c < 0)
        return;
    if (c != g_ui.channel)
        go_to_channel(c);
    if ((i = find_msg(message_id)) >= 0) {
        jump_to(i);
        return;
    }
    g_ui.msgs_loading = 1;
    app_fetch_around(channel_id, message_id);
    redraw();
}

/* discord.com/channels/{guild or @me}/{channel}[/{message}] links open here. */
static int open_discord_link(const char *url)
{
    static const char *const hosts[] = {"https://discord.com/channels/", "https://ptb.discord.com/channels/",
                                        "https://canary.discord.com/channels/"};
    char channel[24] = "", message[24] = "";
    const char *p = NULL;

    for (int h = 0; h < 3 && !p; h++) {
        int n = lstrlenA(hosts[h]);
        if (CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, url, n, hosts[h], n) == CSTR_EQUAL)
            p = url + n;
    }
    if (!p)
        return 0;
    while (*p && *p != '/') /* guild id or @me */
        p++;
    if (*p != '/')
        return 0;
    p++;
    for (int k = 0; *p >= '0' && *p <= '9' && k < 23; k++, p++)
        channel[k] = *p, channel[k + 1] = 0;
    if (*p == '/') {
        p++;
        for (int k = 0; *p >= '0' && *p <= '9' && k < 23; k++, p++)
            message[k] = *p, message[k + 1] = 0;
    }
    if (!channel[0] || model_find_channel(g_ui.model, channel) < 0)
        return 0; /* not a channel we have: let the browser deal with it */
    if (message[0])
        navigate_to_message(channel, message);
    else
        go_to_channel(model_find_channel(g_ui.model, channel));
    return 1;
}

static int search_box_x(void)
{
    RECT rc;

    GetClientRect(g_ui.wnd, &rc);
    return rc.right - S(8) - S(SEARCH_W);
}

static void place_search(void)
{
    int show = g_ui.view == VIEW_APP && open_is_text();

    if (show && !g_ui.search_edit) {
        HFONT font = (HFONT)SendMessageW(g_ui.composer, WM_GETFONT, 0, 0);
        g_ui.search_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_CLIPSIBLINGS | ES_AUTOHSCROLL, 0, 0, 0, 0, g_ui.wnd,
                                           NULL, NULL, NULL);
        g_ui.search_proc = (WNDPROC)SetWindowLongPtrW(g_ui.search_edit, GWLP_WNDPROC, (LONG_PTR)search_edit_proc);
        SendMessageW(g_ui.search_edit, WM_SETFONT, (WPARAM)font, FALSE);
        SendMessageW(g_ui.search_edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search");
    }
    if (g_ui.search_edit) {
        if (show)
            MoveWindow(g_ui.search_edit, search_box_x() + S(10), (S(HEADER_H) - S(20)) / 2, S(SEARCH_W) - S(46), S(20), TRUE);
        ShowWindow(g_ui.search_edit, show ? SW_SHOWNA : SW_HIDE);
    }
}

static void search_close(void)
{
    panel_batch_free(g_ui.results, &g_ui.result_layout);
    g_ui.results = NULL;
    g_ui.results_open = 0;
    g_ui.results_scroll = 0;
}

/* A user's id from a name typed in a filter: members, authors, friends, then the open DM. */
static int find_user_id(const char *name, char *out)
{
    for (int i = 0; i < g_ui.ml.n; i++)
        if (g_ui.ml.items[i].valid && !g_ui.ml.items[i].group && g_ui.ml.items[i].name.data &&
            lstrcmpiA(g_ui.ml.items[i].name.data, name) == 0) {
            lstrcpynA(out, g_ui.ml.items[i].id, 24);
            return 1;
        }
    for (int i = g_ui.nmsgs; i-- > 0;)
        if (!g_ui.msgs[i].system && lstrcmpiA(author_name(&g_ui.msgs[i]), name) == 0) {
            lstrcpynA(out, g_ui.msgs[i].author_id, 24);
            return 1;
        }
    for (int i = 0; i < g_ui.nrels; i++)
        if ((g_ui.rels[i].name.data && lstrcmpiA(g_ui.rels[i].name.data, name) == 0) ||
            (g_ui.rels[i].username.data && lstrcmpiA(g_ui.rels[i].username.data, name) == 0)) {
            lstrcpynA(out, g_ui.rels[i].id, 24);
            return 1;
        }
    if (g_ui.model && lstrcmpiA(model_str(g_ui.model, g_ui.model->user_name), name) == 0) {
        lstrcpynA(out, g_ui.model->user_id, 24);
        return 1;
    }
    return 0;
}

/* Filters to query parameters; what cannot be resolved stays in the text. */
static void search_params(const search_filter_t *f, int n, sb_t *params, sb_t *content)
{
    static const char *const has[] = {"link", "embed", "file", "video", "image", "sound", "sticker", "poll", "forward"};
    char buf[96], id[24];

    for (int k = 0; k < n; k++) {
        int ok = 0;
        unsigned long long lo, hi;
        switch (f[k].key) {
        case SF_FROM:
        case SF_MENTIONS:
            if ((ok = find_user_id(f[k].value, id)) != 0) {
                wsprintfA(buf, f[k].key == SF_FROM ? "&author_id=%s" : "&mentions=%s", id);
                sb_add(params, buf);
            }
            break;
        case SF_HAS:
            for (int h = 0; h < (int)ARRAYSIZE(has) && !ok; h++)
                if (lstrcmpiA(f[k].value, has[h]) == 0) {
                    wsprintfA(buf, "&has=%s", has[h]);
                    sb_add(params, buf);
                    ok = 1;
                }
            break;
        case SF_IN:
            if (g_ui.guild >= 0) {
                const guild_t *gd = &g_ui.model->guilds[g_ui.guild];
                for (unsigned c = gd->first; c < gd->first + gd->count && !ok; c++)
                    if (chan((int)c)->type != CH_CATEGORY && lstrcmpiA(model_str(g_ui.model, chan((int)c)->name), f[k].value) == 0) {
                        wsprintfA(buf, "&channel_id=%s", chan((int)c)->id);
                        sb_add(params, buf);
                        ok = 1;
                    }
            }
            break;
        case SF_BEFORE:
        case SF_AFTER:
        case SF_DURING:
            lo = search_day_snowflake(f[k].value, f[k].key == SF_AFTER, local_offset_ms(f[k].value));
            hi = search_day_snowflake(f[k].value, 1, local_offset_ms(f[k].value));
            if ((ok = lo != 0) != 0) {
                if (f[k].key != SF_BEFORE) {
                    wsprintfA(buf, "&min_id=%I64u", f[k].key == SF_AFTER ? hi : lo);
                    sb_add(params, buf);
                }
                if (f[k].key != SF_AFTER) {
                    wsprintfA(buf, "&max_id=%I64u", f[k].key == SF_BEFORE ? lo : hi);
                    sb_add(params, buf);
                }
            }
            break;
        case SF_PINNED:
            ok = lstrcmpiA(f[k].value, "true") == 0 || lstrcmpiA(f[k].value, "false") == 0;
            if (ok)
                sb_add(params, lstrcmpiA(f[k].value, "true") == 0 ? "&pinned=true" : "&pinned=false");
            break;
        }
        if (!ok) {
            if (content->len)
                sb_add(content, " ");
            sb_add(content, f[k].value);
        }
    }
}

static void search_run(void)
{
    wchar_t w[128];
    sb_t q = {0}, content = {0}, params = {0};
    search_filter_t f[8];
    int n;

    GetWindowTextW(g_ui.search_edit, w, 128);
    wide_to_utf8(w, (size_t)lstrlenW(w), &q);
    if (q.len && open_is_text()) {
        n = search_parse(q.data, f, (int)ARRAYSIZE(f), &content);
        search_params(f, n, &params, &content);
        search_close();
        g_ui.results_open = 1;
        pins_close();
        if (g_ui.guild >= 0)
            app_search(g_ui.model->guilds[g_ui.guild].id, NULL, content.data ? content.data : "", params.data);
        else
            app_search(NULL, g_ui.msgs_channel, content.data ? content.data : "", params.data);
    }
    sb_free(&q);
    sb_free(&content);
    sb_free(&params);
    redraw();
}

static LRESULT CALLBACK search_edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        search_run();
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        SetWindowTextW(h, L"");
        search_close();
        SetFocus(g_ui.composer);
        redraw();
        return 0;
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE))
        return 0;
    return CallWindowProcW(g_ui.search_proc, h, msg, wp, lp);
}

static RECT results_rect(void)
{
    RECT rc;

    GetClientRect(g_ui.wnd, &rc);
    return rect(main_right() - S(RESULTS_W) - S(16), S(HEADER_H) + S(4), S(RESULTS_W), rc.bottom - S(HEADER_H) - S(24));
}

static void paint_search(void)
{
    RECT r;
    int y;
    char title[64];

    if (!g_ui.results_open)
        return;
    r = results_rect();
    r_round(r.left, r.top, r.right - r.left, r.bottom - r.top, S(8), 0xFF121214);
    r_round_outline(r.left, r.top, r.right - r.left, r.bottom - r.top, S(8), 1, 0xFF242426);
    if (!g_ui.results)
        lstrcpyA(title, "Searching\xE2\x80\xA6");
    else if (g_ui.results->status)
        lstrcpyA(title, "Search failed");
    else
        wsprintfA(title, "%d Result%s", g_ui.results->total, g_ui.results->total == 1 ? "" : "s");
    text(g_ui.f_h, C_INK, rect(r.left + S(16), r.top, S(300), S(48)), title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    fill(r.left + S(1), r.top + S(48), r.right - r.left - S(2), 1, C_LINE);
    if (!g_ui.results)
        return;
    r_clip(r.left, r.top + S(49), r.right - r.left, r.bottom - r.top - S(50));
    y = r.top + S(56) - g_ui.results_scroll;
    /* Pictures, GIFs and embeds come along, as in Discord's results. */
    panel_measure(&g_ui.result_layout, g_ui.results, r.left + S(60), r.right - r.left - S(76), S(66));
    for (int k = 0; k < g_ui.results->n; k++) {
        msg_t *m = &g_ui.results->msgs[k];
        int tw = r.right - r.left - S(76), th = g_ui.result_layout.th[k], eh = g_ui.result_layout.eh[k], c;
        wchar_t *body, when[64];
        r_image_t *img;
        char where[96];
        g_ui.result_y[k < 64 ? k : 63] = y;
        g_ui.result_h[k < 64 ? k : 63] = S(20) + S(40) + th + eh + S(16);
        if (!r_visible(y, S(20) + S(40) + th + S(8) + eh)) {
            y += S(20) + S(40) + th + eh + S(16);
            continue;
        }
        c = model_find_channel(g_ui.model, m->channel_id);
        body = plain_text(&m->text);
        img = user_avatar(m->author_id, m->avatar);
        wsprintfA(where, "# %.80s", c >= 0 ? model_str(g_ui.model, chan(c)->name) : "unknown");
        text(g_ui.f_cat, C_MUTED, rect(r.left + S(16), y, tw, S(18)), where, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += S(20);
        r_round(r.left + S(8), y, r.right - r.left - S(16), S(40) + th + S(8) + eh, S(6),
                g_ui.result_hover == k ? 0xFF1E1E20 : 0xFF121214);
        if (img)
            r_image(img, r.left + S(16), y + S(8), S(32), S(32), S(16));
        text(g_ui.f_h, C_INK, rect(r.left + S(60), y + S(6), tw, S(20)), m->author.data ? m->author.data : "",
             DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        format_time(m->id, when, ARRAYSIZE(when));
        text_w(g_ui.f_small, C_FAINT, rect(r.left + S(60) + text_width(g_ui.f_h, m->author.data ? m->author.data : "") + S(8),
                                          y + S(8), S(200), S(18)), when, -1, DT_LEFT | DT_SINGLELINE);
        if (th)
            r_text(g_ui.f_body, ARGB(C_INK), r.left + S(60), y + S(28), tw, th, body, -1, R_LEFT | R_WRAP | R_ELLIPSIS);
        if (eh)
            msg_extras(m, r.left + S(60), y + S(28) + th, tw, 1, 0, 0, NULL);
        mem_free(body);
        y += S(40) + th + eh + S(16);
    }
    g_ui.results_content = y + g_ui.results_scroll - (r.top + S(56));
    r_unclip();
}

static int result_hit(int x, int y)
{
    RECT r = results_rect();

    if (!g_ui.results_open || !g_ui.results || x < r.left || x >= r.right || y < r.top + S(49) || y >= r.bottom)
        return -1;
    for (int k = 0; k < g_ui.results->n && k < 64; k++)
        if (y >= g_ui.result_y[k] && y < g_ui.result_y[k] + g_ui.result_h[k])
            return k;
    return -1;
}

/* Clicks while the results are open: 1 when handled. */
static int click_results(int x, int y)
{
    RECT r;
    int k;

    if (!g_ui.results_open)
        return 0;
    r = results_rect();
    if (x < r.left || x >= r.right || y < r.top || y >= r.bottom) {
        if (y >= S(HEADER_H)) {
            search_close();
            redraw();
        }
        return 0;
    }
    if ((k = result_hit(x, y)) >= 0) {
        char channel[24], id[24];
        lstrcpynA(channel, g_ui.results->msgs[k].channel_id, sizeof channel);
        lstrcpynA(id, g_ui.results->msgs[k].id, sizeof id);
        navigate_to_message(channel, id);
    }
    return 1;
}

/* Bar shown while older messages are on screen after a jump. */
static void paint_detached(int x0, int w, int cy)
{
    int y = cy - S(32) - (g_ui.bar ? S(BAR_H) : 0) - tray_h();

    if (!g_ui.detached)
        return;
    r_round(x0 + S(24), y, w - S(48), S(28), S(6), 0xFF121214);
    text(g_ui.f_small, C_MUTED, rect(x0 + S(36), y, w - S(200), S(28)), "You're viewing older messages",
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    text(g_ui.f_cat, C_INK, rect(x0 + w - S(180), y, S(144), S(28)), "Jump To Present \xE2\x86\x93",
         DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

static int click_detached(int x, int y)
{
    RECT rc;
    int x0 = S(RAIL_W + SIDE_W), w = main_right() - x0, cy, top;

    if (!g_ui.detached)
        return 0;
    GetClientRect(g_ui.wnd, &rc);
    cy = rc.bottom - S(COMPOSER_GAP) - S(COMPOSER_H);
    top = cy - S(32) - (g_ui.bar ? S(BAR_H) : 0) - tray_h();
    if (y < top || y >= top + S(28) || x < x0 + S(24) || x >= x0 + w - S(24))
        return 0;
    /* Back to the latest messages. */
    g_ui.detached = 0;
    g_ui.msgs_loading = 1;
    app_fetch_messages(g_ui.msgs_channel, NULL);
    redraw();
    return 1;
}

/* ---- Forum channels ---- */

#define POST_H 96

typedef struct {
    char id[24];
    sb_t name, preview, author, raw;
    char author_id[24], avatar[48];
    int count;
} post_t;

static void posts_clear(void)
{
    for (int i = 0; i < g_ui.nposts; i++) {
        post_t *pt = &((post_t *)g_ui.posts)[i];
        sb_free(&pt->name);
        sb_free(&pt->preview);
        sb_free(&pt->author);
        sb_free(&pt->raw);
    }
    mem_free(g_ui.posts);
    g_ui.posts = NULL;
    g_ui.nposts = 0;
    g_ui.forum_loaded = 0;
    g_ui.forum_scroll = 0;
}

/* {threads: [...], first_messages: [...]} from threads/search. */
static void on_forum(const sb_t *p)
{
    const char *channel = p->data, *body = channel + lstrlenA(channel) + 1;
    size_t n = p->len - (size_t)(body - p->data);
    json_t root, threads, firsts = {0}, t, v, m;
    json_iter_t it, fit;

    if (lstrcmpA(channel, g_ui.msgs_channel) != 0)
        return;
    posts_clear();
    g_ui.forum_loaded = 1;
    if (!n || !json_parse(body, n, &root) || !json_get(root, "threads", &threads))
        return;
    json_get(root, "first_messages", &firsts);
    g_ui.posts = mem_alloc((json_count(threads) + 1) * sizeof(post_t));
    json_iter(threads, &it);
    while (json_next(&it, NULL, &t)) {
        post_t *pt = &((post_t *)g_ui.posts)[g_ui.nposts];
        long long count = 0;
        if (!json_get(t, "id", &v))
            continue;
        json_raw(v, pt->id, sizeof pt->id);
        if (json_get(t, "name", &v))
            json_str(v, &pt->name);
        if (json_get(t, "message_count", &v))
            json_int(v, &count);
        pt->count = (int)count;
        sb_addn(&pt->raw, t.p, (size_t)(t.end - t.p));
        /* The post's first message has the thread's id: only that one is parsed. */
        if (firsts.p) {
            json_iter(firsts, &fit);
            while (json_next(&fit, NULL, &m)) {
                msg_t msg = {0};
                char id[sizeof msg.id];
                if (!json_get(m, "id", &v))
                    continue;
                json_raw(v, id, sizeof id);
                if (lstrcmpA(id, pt->id) != 0)
                    continue;
                if (msg_parse(m, &msg)) {
                    sb_addn(&pt->preview, msg.text.data ? msg.text.data : "", msg.text.len);
                    sb_addn(&pt->author, msg.author.data ? msg.author.data : "", msg.author.len);
                    lstrcpynA(pt->author_id, msg.author_id, sizeof pt->author_id);
                    lstrcpynA(pt->avatar, msg.avatar, sizeof pt->avatar);
                    msg_free(&msg);
                    break;
                }
                msg_free(&msg);
            }
        }
        g_ui.nposts++;
    }
}

static void paint_forum(RECT rc, int x0, int w)
{
    int y = S(HEADER_H) + S(16) - g_ui.forum_scroll;

    if (!g_ui.forum_loaded) {
        text(g_ui.f_body, C_MUTED, rect(x0, rc.bottom / 2, w, S(24)), "Loading posts\xE2\x80\xA6", DT_CENTER | DT_SINGLELINE);
        return;
    }
    /* New Post, above the posts as in Discord. */
    g_ui.forum_new = rect(x0 + S(24), y, S(120), S(36));
    r_round(x0 + S(24), y, S(120), S(36), S(6), ARGB(C_BRAND));
    text(g_ui.f_h, C_RAIL, g_ui.forum_new, "New Post", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    y += S(52);
    if (!g_ui.nposts) {
        text(g_ui.f_body, C_MUTED, rect(x0, rc.bottom / 2, w, S(24)), "There are no posts here yet.", DT_CENTER | DT_SINGLELINE);
        return;
    }
    r_clip(x0, S(HEADER_H), w, rc.bottom - S(HEADER_H));
    for (int i = 0; i < g_ui.nposts; i++, y += S(POST_H) + S(8)) {
        post_t *pt = &((post_t *)g_ui.posts)[i];
        int cx = x0 + S(24), cw = w - S(48);
        char count[16];
        wchar_t *prev;
        r_image_t *img;
        if (i < 64)
            g_ui.post_y[i] = y; /* for clicks, even when scrolled out of sight */
        if (!r_visible(y, S(POST_H)))
            continue;
        r_round(cx, y, cw, S(POST_H), S(8), g_ui.post_hover == i ? 0xFF1E1E20 : 0xFF1A1A1A);
        text(g_ui.f_title, C_INK, rect(cx + S(16), y + S(12), cw - S(100), S(26)), pt->name.data ? pt->name.data : "",
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        img = pt->author_id[0] ? user_avatar(pt->author_id, pt->avatar) : NULL;
        if (img)
            r_image(img, cx + S(16), y + S(46), S(18), S(18), S(9));
        text(g_ui.f_h, C_MUTED, rect(cx + S(40), y + S(44), S(160), S(22)), pt->author.data ? pt->author.data : "",
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        prev = plain_text(&pt->preview);
        r_text(g_ui.f_body, ARGB(C_MUTED), cx + S(206), y + S(44), cw - S(300), S(22), prev, -1, R_LEFT | R_VCENTER | R_SINGLE | R_ELLIPSIS);
        mem_free(prev);
        wsprintfA(count, "%d", pt->count);
        text_w(g_ui.f_icon, C_FAINT, rect(cx + cw - S(76), y + S(12), S(24), S(26)), L"\xE8F2", -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(g_ui.f_h, C_MUTED, rect(cx + cw - S(52), y + S(12), S(40), S(26)), count, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    g_ui.forum_content = y + g_ui.forum_scroll - S(HEADER_H);
    r_unclip();
}

static int forum_view(void)
{
    return g_ui.model && g_ui.channel >= 0 && (chan(g_ui.channel)->type == CH_FORUM || chan(g_ui.channel)->type == CH_MEDIA);
}

static int post_hit(int x, int y)
{
    int x0 = S(RAIL_W + SIDE_W), w = main_right() - x0;

    if (!forum_view() || y < S(HEADER_H) || x < x0 + S(24) || x >= x0 + w - S(24))
        return -1;
    for (int i = 0; i < g_ui.nposts && i < 64; i++)
        if (y >= g_ui.post_y[i] && y < g_ui.post_y[i] + S(POST_H))
            return i;
    return -1;
}

/* Opens a post: it joins the model as a channel under its forum, then opens like any thread. */
static void open_post(int i)
{
    post_t *pt = &((post_t *)g_ui.posts)[i];
    char id[24];
    int c;

    lstrcpynA(id, pt->id, sizeof id);
    if (model_find_channel(g_ui.model, id) < 0) {
        json_t d;
        model_t *m;
        if (json_parse(pt->raw.data, pt->raw.len, &d) && (m = model_apply(g_ui.model, "CHANNEL_CREATE", d)) != NULL)
            replace_model(m);
    }
    if ((c = model_find_channel(g_ui.model, id)) >= 0)
        go_to_channel(c);
}

/* ---- Keyboard shortcuts ---- */

/* Next (dir 1) or previous (-1) channel of the open list, optionally only unread ones. */
static void step_channel(int dir, int unread_only)
{
    unsigned first, count;
    int i;

    if (!g_ui.model || !side_range(&first, &count) || !count)
        return;
    i = g_ui.channel >= 0 ? g_ui.channel : (dir > 0 ? (int)first - 1 : (int)(first + count));
    for (unsigned k = 0; k < count; k++) {
        i += dir;
        if (i < (int)first)
            i = (int)(first + count - 1);
        if (i >= (int)(first + count))
            i = (int)first;
        if (chan(i)->type == CH_CATEGORY || is_voice_type(chan(i)->type))
            continue;
        if (unread_only && !channel_unread((unsigned)i) && !chan(i)->mentions)
            continue;
        go_to_channel(i);
        return;
    }
}

static void step_guild(int dir)
{
    int n = g_ui.model ? (int)g_ui.model->nguilds : 0, g;

    if (!n)
        return;
    g = g_ui.guild + dir;
    if (g < -1)
        g = n - 1;
    if (g >= n)
        g = -1; /* home comes around */
    select_guild(g);
}

/* Returns 1 when the key was a shortcut. */
static int shortcut(WPARAM key)
{
    int ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0, shift = GetKeyState(VK_SHIFT) < 0;

    if (g_ui.view != VIEW_APP)
        return 0;
    if ((key == VK_UP || key == VK_DOWN) && alt && ctrl) {
        step_guild(key == VK_DOWN ? 1 : -1);
        return 1;
    }
    if ((key == VK_UP || key == VK_DOWN) && alt) {
        step_channel(key == VK_DOWN ? 1 : -1, shift);
        return 1;
    }
    if ((key == VK_PRIOR || key == VK_NEXT) && open_is_text()) {
        RECT a = message_area();
        g_ui.msg_scroll += (key == VK_PRIOR ? 1 : -1) * (a.bottom - a.top) * 4 / 5;
        clamp_msg_scroll();
        maybe_load_older();
        redraw();
        return 1;
    }
    if (key == 'E' && ctrl && open_is_text()) {
        RECT rc;
        int x0 = S(RAIL_W + SIDE_W), w = main_right() - x0;
        GetClientRect(g_ui.wnd, &rc);
        if (g_ui.picker)
            picker_close();
        else
            picker_open(PICK_COMPOSER, NULL, x0 + w - S(16), rc.bottom - S(COMPOSER_GAP) - S(COMPOSER_H) - S(8));
        return 1;
    }
    return 0;
}

/* ---- Composer ---- */

static void send_composer(void)
{
    int n = GetWindowTextLengthW(g_ui.composer);
    wchar_t *w;
    sb_t text = {0};
    size_t a = 0, b;

    if ((n <= 0 && !g_ui.nuploads) || !open_is_text())
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
    if (b > a || g_ui.nuploads) {
        sb_t out = {0}, rewritten = {0}, mentioned = {0};
        if (text.data[a] == '/' && builtin_rewrite(text.data + a, b - a, &rewritten)) {
            sb_free(&text);
            text = rewritten;
            a = 0;
            b = text.len;
        }
        text.data[b] = 0;
        apply_mentions(text.data + a, b - a, &mentioned);
        if (text.data[a] == '/' && !g_ui.nuploads && g_ui.bar != BAR_EDIT && send_command(mentioned.data, mentioned.len)) {
            sb_free(&mentioned);
            sb_free(&text);
            return;
        }
        /* :smile: and :server_emoji: become the real thing, as in Discord. */
        emoji_expand(mentioned.data ? mentioned.data : "", mentioned.len, &out, custom_emoji_markup, NULL);
        sb_free(&mentioned);
        g_ui.nmention = 0;
        sb_clear(&g_ui.send_error);
        if (g_ui.nuploads && g_ui.bar != BAR_EDIT) {
            sb_t paths = {0};
            for (int i = 0; i < g_ui.nuploads; i++)
                sb_addn(&paths, g_ui.uploads[i].path.data, g_ui.uploads[i].path.len + 1);
            app_send_files(g_ui.msgs_channel, out.data ? out.data : "", g_ui.bar == BAR_REPLY ? g_ui.bar_msg : NULL,
                           g_ui.bar_mention, paths.data, g_ui.nuploads);
            sb_free(&paths);
            uploads_clear();
            place_composer();
        } else if (g_ui.bar == BAR_EDIT)
            app_edit_message(g_ui.msgs_channel, g_ui.bar_msg, out.data);
        else if (g_ui.bar == BAR_REPLY)
            app_send_reply(g_ui.msgs_channel, out.data, g_ui.bar_msg, g_ui.bar_mention);
        else
            app_send_message(g_ui.msgs_channel, out.data);
        sb_free(&out);
        g_ui.typing_sent = 0;
        SetWindowTextW(g_ui.composer, L"");
        if (g_ui.bar)
            bar_close();
        g_ui.msg_scroll = 0;
        redraw();
    }
    sb_free(&text);
}

static LRESULT CALLBACK composer_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && g_ui.ac_kind == AC_NONE && shortcut(wp))
        return 0;
    if (msg == WM_CHAR && wp == 5) /* Ctrl+E's control character */
        return 0;
    if (msg == WM_PASTE && paste_files())
        return 0;
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE && !g_ui.confirm && !g_ui.bar && !g_ui.share_pick && g_ui.ac_kind == AC_NONE &&
        g_ui.channel >= 0) {
        /* Esc marks the channel read, like Discord. */
        mark_read(g_ui.channel);
        redraw();
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == 'K' && GetKeyState(VK_CONTROL) < 0) {
        qs_open();
        return 0;
    }
    if (msg == WM_CHAR && wp == 11) /* Ctrl+K's control character */
        return 0;
    if (msg == WM_KEYDOWN && ac_key(wp)) {
        g_ui.ac_ate = wp == VK_RETURN || wp == VK_TAB || wp == VK_ESCAPE;
        return 0;
    }
    if (msg == WM_CHAR && g_ui.ac_ate && (wp == VK_RETURN || wp == VK_TAB || wp == VK_ESCAPE)) {
        g_ui.ac_ate = 0;
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_RETURN) {
        if (g_ui.confirm)
            confirm_close(1);
        else
            send_composer();
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE && (g_ui.confirm || g_ui.bar || g_ui.share_pick)) {
        if (g_ui.share_pick)
            share_pick_close();
        else if (g_ui.confirm)
            confirm_close(0);
        else
            bar_close();
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_ESCAPE)
        return 0;
    if (msg == WM_KEYDOWN && wp == VK_UP && !g_ui.bar && GetWindowTextLengthW(h) == 0 && edit_last())
        return 0;
    if (msg == WM_MOUSEWHEEL)
        return SendMessageW(g_ui.wnd, msg, wp, lp);
    if (msg == WM_LBUTTONDOWN)
        pop_dismiss();
    return CallWindowProcW(g_ui.composer_proc, h, msg, wp, lp);
}

/* ---- Window procedure ---- */

static void update_hover(int x, int y)
{
    int kind, index, m = message_at(x, y, NULL), link = rich_hit(x, y, NULL, NULL), i = -1, ax, ay;
    int tool = toolbar_hit(x, y, NULL);
    part_t part = {PART_NONE, -1};

    {
        int mh = ml_hit(x, y, NULL);
        if (mh != g_ui.ml_hover) {
            g_ui.ml_hover = mh;
            redraw();
        }
        link = link || mh >= 0;
    }
    {
        RECT crc;
        int th = tray_hit(x, y), cy, att, emo, bh;
        GetClientRect(g_ui.wnd, &crc);
        cy = crc.bottom - S(COMPOSER_GAP) - S(COMPOSER_H);
        att = open_is_text() && x >= S(RAIL_W + SIDE_W) + S(COMPOSER_GAP) + S(8) && x < S(RAIL_W + SIDE_W) + S(COMPOSER_GAP) + S(48) &&
              y >= cy && y < cy + S(COMPOSER_H);
        emo = CB_NONE;
        if (open_is_text() && y >= cy && y < cy + S(COMPOSER_H))
            for (int b = CB_EMOJI; b < CB_COUNT; b++) {
                int bx = main_right() - S(COMPOSER_GAP) - S(8) - b * S(40);
                if (x >= bx && x < bx + S(40))
                    emo = b;
            }
        bh = bar_hit(x, y);
        if (th != g_ui.upload_hover || att != g_ui.hover_attach || emo != g_ui.hover_emoji || bh != g_ui.bar_hover) {
            g_ui.upload_hover = th;
            g_ui.hover_attach = att;
            g_ui.hover_emoji = emo;
            g_ui.bar_hover = bh;
            redraw();
        }
        link = link || th >= 0 || att || emo || bh;
    }
    {
        int sh = shop_hit(x, y), qh = quests_hit(x, y), nh = nitro_hit(x, y);
        if (sh != g_shop.hover || qh != g_quests.hover || nh != g_ui.nitro_hover) {
            g_shop.hover = sh;
            g_quests.hover = qh;
            g_ui.nitro_hover = nh;
            redraw();
        }
        link = link || sh >= 0 || qh >= 0 || nh;
    }
    {
        int ph = post_hit(x, y);
        if (ph != g_ui.post_hover) {
            g_ui.post_hover = ph;
            redraw();
        }
        link = link || ph >= 0;
    }
    {
        int rh = result_hit(x, y);
        if (rh != g_ui.result_hover) {
            g_ui.result_hover = rh;
            redraw();
        }
        link = link || rh >= 0;
    }
    if (friends_view()) {
        int act, fh = friends_hit(x, y, &act);
        if (fh != g_ui.friend_hover || act != g_ui.friend_act) {
            g_ui.friend_hover = fh;
            g_ui.friend_act = act;
            redraw();
        }
        link = link || act >= 0 || fh <= -10;
    }
    if (g_ui.confirm) {
        int h = confirm_hit(x, y);
        if (h != g_ui.confirm_hover) {
            g_ui.confirm_hover = h;
            redraw();
        }
        return;
    }
    if (g_ui.share_pick) {
        int h = share_pick_hit(x, y);
        if (h != g_ui.share_pick_hover) {
            g_ui.share_pick_hover = h;
            redraw();
        }
        return;
    }
    /* The toolbar floats over the message above: keep the hovered message while over it. */
    if (tool >= 0)
        m = g_ui.hover_msg;
    if (tool != g_ui.hover_tool) {
        g_ui.hover_tool = tool;
        redraw();
    }
    link = link || tool >= 0;

    link = link || author_hit(x, y, &i, &ax, &ay) || part_hit(x, y, &i, &part);
    react_hover(part.kind == PART_REACTION ? i : -1, part.index, x, y);

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
        vsync_start(wnd, WM_VSYNC);
        g_ui.dpi = GetDpiForWindow(wnd);
        g_ui.b_composer = CreateSolidBrush(RGB(0x10, 0x10, 0x13));
        g_ui.composer = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_CLIPSIBLINGS | ES_AUTOHSCROLL, 0, 0, 0, 0, wnd, NULL,
                                        NULL, NULL);
        g_ui.composer_proc = (WNDPROC)SetWindowLongPtrW(g_ui.composer, GWLP_WNDPROC, (LONG_PTR)composer_proc);
        SendMessageW(g_ui.composer, EM_LIMITTEXT, 2000, 0);
        make_fonts();
        img_init(wnd, UI_IMAGE);
        paste_cleanup();
        prefs_load();
        DragAcceptFiles(wnd, TRUE);
        return 0;
    case WM_DROPFILES:
        on_drop((HDROP)wp);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_ui.settings_open) {
            if (g_ui.set_record) {
                if (wp != VK_ESCAPE)
                    settings_set_key((int)wp);
                g_ui.set_record = 0;
                redraw();
            } else if (wp == VK_ESCAPE) {
                settings_close();
            }
            return 0;
        }
        if (wp == 'K' && GetKeyState(VK_CONTROL) < 0) {
            qs_open();
            return 0;
        }
        if (shortcut(wp))
            return 0;
        break;
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
        if (g_ui.settings_open) {
            g_ui.settings_edit_y = -1;
            redraw();
            return 0;
        }
        place_composer();
        place_friend_input();
        place_search();
        pop_place();
        dm_profile_sync(); /* back from the tray, or fonts rebuilt: the DM's profile returns */
        redraw();
        return 0;
    case WM_COMMAND:
        if ((HWND)lp == g_ui.composer && HIWORD(wp) == EN_CHANGE)
            composer_changed();
        if ((HWND)lp == g_ui.friend_search && HIWORD(wp) == EN_CHANGE) {
            g_ui.friend_scroll = 0;
            redraw();
        }
        break;
    case WM_CTLCOLOREDIT:
        if ((HWND)lp == g_ui.friend_search) { /* in a box on the page's black */
            SetTextColor((HDC)wp, GDI(C_TEXT));
            SetBkColor((HDC)wp, GDI(C_MAIN));
            return (LRESULT)GetStockObject(BLACK_BRUSH);
        }
        if ((HWND)lp == g_ui.friend_edit) {
            static HBRUSH field;
            if (!field)
                field = CreateSolidBrush(RGB(0x12, 0x12, 0x14));
            SetTextColor((HDC)wp, GDI(C_TEXT));
            SetBkColor((HDC)wp, RGB(0x12, 0x12, 0x14));
            return (LRESULT)field;
        }
        if ((HWND)lp == g_ui.search_edit) {
            SetTextColor((HDC)wp, GDI(C_TEXT));
            SetBkColor((HDC)wp, GDI(C_MAIN));
            return (LRESULT)GetStockObject(BLACK_BRUSH);
        }
        if ((HWND)lp == g_ui.settings_edit) {
            static HBRUSH box;
            if (!box)
                box = CreateSolidBrush(RGB(0x12, 0x12, 0x14));
            SetTextColor((HDC)wp, GDI(C_TEXT));
            SetBkColor((HDC)wp, RGB(0x12, 0x12, 0x14));
            return (LRESULT)box;
        }
        SetTextColor((HDC)wp, GDI(C_TEXT));
        SetBkColor((HDC)wp, RGB(0x10, 0x10, 0x13));
        return (LRESULT)g_ui.b_composer;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(wnd);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g_ui.settings_open) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)(g_ui.settings_hover >= 0 ? IDC_HAND : IDC_ARROW)));
            return TRUE;
        }
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)(g_ui.hover_kind != HIT_NONE || g_ui.hover_link ? IDC_HAND : IDC_ARROW)));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        if (g_ui.view == VIEW_APP && g_ui.settings_open) {
            int id = settings_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (id == SH_IN_VOL || id == SH_OUT_VOL || id == SH_SENS) {
                g_ui.set_drag = id;
                SetCapture(wnd);
                slider_drag(id, GET_X_LPARAM(lp));
            }
            return 0;
        }
        break;
    case WM_XBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (g_ui.view == VIEW_APP && g_ui.settings_open && g_ui.set_record) {
            settings_set_key(msg == WM_MBUTTONDOWN ? VK_MBUTTON : HIWORD(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2);
            g_ui.set_record = 0;
            redraw();
            return msg == WM_XBUTTONDOWN ? TRUE : 0;
        }
        break;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, wnd, 0};
        TrackMouseEvent(&tme);
        if (g_ui.set_drag) {
            slider_drag(g_ui.set_drag, GET_X_LPARAM(lp));
            return 0;
        }
        if (g_ui.view == VIEW_APP && g_ui.settings_open) {
            int h = settings_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (h != g_ui.settings_hover) {
                g_ui.settings_hover = h;
                redraw();
            }
        } else if (g_ui.view == VIEW_APP) {
            update_hover(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        }
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
        if (g_ui.set_drag) {
            g_ui.set_drag = 0;
            ReleaseCapture();
            prefs_save();
            redraw();
            return 0;
        }
        if (g_ui.view == VIEW_APP && g_ui.settings_open) {
            settings_click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        }
        if (g_ui.view == VIEW_APP) {
            int kind, index;
            int action;
            if (g_ui.pop && !g_ui.pop_docked) {
                pop_close(); /* a click outside only closes the popout */
                dm_profile_sync();
                return 0;
            }
            if (g_ui.confirm) {
                int h = confirm_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                if (h == 1 || h == -1)
                    confirm_close(0);
                else if (h == 2)
                    confirm_close(1);
                return 0;
            }
            if (g_ui.share_pick) {
                share_pick_click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                return 0;
            }
            if (toolbar_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &action) >= 0) {
                run_tool(action);
                return 0;
            }
            if (g_ui.picker) {
                picker_close(); /* a click outside only closes the picker */
                return 0;
            }
            if (click_results(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)) || click_detached(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)))
                return 0;
            {
                int ph = post_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (forum_view() && g_ui.forum_loaded && PtInRect(&g_ui.forum_new, pt)) {
                    prompt_open(PROMPT_POST_TITLE, NULL, chan(g_ui.channel)->id, L"Post title");
                    return 0;
                }
                if (ph >= 0) {
                    open_post(ph);
                    return 0;
                }
            }
            {
                int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
                if (click_call(x, y))
                    return 0;
                if (open_is_text() && is_dm_type(chan(g_ui.channel)->type) && y < S(HEADER_H) && x >= call_button_x() &&
                    x < call_button_x() + S(32)) {
                    call_start(g_ui.channel);
                    return 0;
                }
                if (open_is_text() && y < S(HEADER_H) && x >= pins_button_x() && x < pins_button_x() + S(32)) {
                    pins_toggle(0);
                    redraw();
                    return 0;
                }
                if (open_is_text() && y < S(HEADER_H) && x >= header_button_x(HB_BELL) && x < header_button_x(HB_BELL) + S(32)) {
                    /* under the bell, as Discord's popout */
                    POINT pt = {header_button_x(HB_BELL), S(HEADER_H) - S(4)};
                    ClientToScreen(g_ui.wnd, &pt);
                    g_menu_at = pt;
                    g_menu_at_set = 1;
                    channel_menu_as(g_ui.channel, 1);
                    g_menu_at_set = 0;
                    return 0;
                }
                if (open_is_text() && y < S(HEADER_H) && x >= header_button_x(HB_VIDEO) && x < header_button_x(HB_VIDEO) + S(32)) {
                    /* A video call: the camera comes on once the call is connected. */
                    g_ui.camera_on_connect = 1;
                    call_start(g_ui.channel);
                    return 0;
                }
                if (g_ui.pins_open) {
                    RECT pr = pins_rect();
                    if (!(x >= pr.left && x < pr.right && y >= pr.top && y < pr.bottom))
                        pins_close();
                    else
                        pins_click(x, y);
                    redraw();
                    return 0;
                }
            }
            {
                int ai = ac_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                if (ai >= 0) {
                    ac_accept(ai);
                    SetFocus(g_ui.composer);
                    return 0;
                }
            }
            if (click_bar(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)))
                return 0;
            if (friends_view() && GET_X_LPARAM(lp) >= S(RAIL_W + SIDE_W)) {
                friends_click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                return 0;
            }
            if (nitro_view() && g_ui.nitro_hover) {
                nitro_click(g_ui.nitro_hover);
                return 0;
            }
            if (quests_view() && GET_X_LPARAM(lp) >= S(RAIL_W + SIDE_W)) {
                /* Quests are taken on in Discord: the button opens this one there. */
                int k = quests_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                if (k >= 0) {
                    wchar_t url[96];
                    wsprintfW(url, L"https://discord.com/quests/%S", g_quests.v[k].id);
                    ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
                }
                return 0;
            }
            if (shop_view() && GET_X_LPARAM(lp) >= S(RAIL_W + SIDE_W)) {
                /* Buying happens on Discord: the card leads to it there. */
                int k = shop_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                if (k >= 0) {
                    wchar_t url[96];
                    wsprintfW(url, L"https://discord.com/shop#itemSkuId=%S", g_shop.items[k].sku);
                    ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
                }
                return 0;
            }
            {
                int ti = tray_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                if (ti >= 0) {
                    upload_remove(ti);
                    g_ui.upload_hover = -1;
                    place_composer();
                    clamp_msg_scroll();
                    redraw();
                    return 0;
                }
                if (g_ui.hover_attach) {
                    pick_files();
                    return 0;
                }
            }
            {
                RECT crc;
                int y = GET_Y_LPARAM(lp), cx0 = S(RAIL_W + SIDE_W), cw, cy;
                GetClientRect(wnd, &crc);
                cw = main_right() - cx0;
                cy = crc.bottom - S(COMPOSER_GAP) - S(COMPOSER_H);
                if (open_is_text() && y >= cy && y < cy + S(COMPOSER_H) && g_ui.hover_emoji != CB_NONE) {
                    int tab = g_ui.hover_emoji == CB_EMOJI ? TAB_EMOJI : g_ui.hover_emoji == CB_GIF ? TAB_GIFS : TAB_STICKERS;
                    if (g_ui.picker && g_ui.picker_tab == tab) {
                        picker_close();
                    } else {
                        picker_close();
                        g_ui.picker_tab = tab;
                        picker_open(PICK_COMPOSER, NULL, cx0 + cw - S(COMPOSER_GAP), cy - S(8));
                    }
                    return 0;
                }
            }
            {
                int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), top, mi;
                if (open_is_text() && y < S(HEADER_H) && x >= header_button_x(HB_MEMBERS) &&
                    x < header_button_x(HB_MEMBERS) + S(32)) {
                    g_ui.show_members ^= 1;
                    dm_profile_sync();
                    place_composer();
                    invalidate_views();
                    clamp_msg_scroll();
                    redraw();
                    return 0;
                }
                if ((mi = ml_hit(x, y, &top)) >= 1000 && g_ui.guild < 0) {
                    dm_member_t m[DM_MEMBERS_MAX];
                    int n = dm_members(m);
                    if (mi - 1000 < n)
                        pop_open(m[mi - 1000].id, m[mi - 1000].name, m[mi - 1000].avatar, main_right() - S(POP_W) - S(8), top, 0);
                    return 0;
                }
                if (mi >= 0) {
                    const ml_item_t *it = &g_ui.ml.items[mi];
                    pop_open(it->id, it->name.data, it->avatar, main_right() - S(POP_W) - S(8), top, 0);
                    return 0;
                }
            }
            if (click_author(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)) || click_message(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)))
                return 0;
            hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &kind, &index);
            on_click(kind, index);
        }
        return 0;
    case WM_RBUTTONUP:
        on_right_click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSEWHEEL: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int delta = -GET_WHEEL_DELTA_WPARAM(wp) * S(ROW_H) * 3 / WHEEL_DELTA;
        ScreenToClient(wnd, &pt);
        if (g_ui.view != VIEW_APP)
            return 0;
        if (!g_ui.pop_docked)
            pop_close();
        if (nitro_view() && pt.x >= S(RAIL_W + SIDE_W)) {
            nitro_scroll_to(g_nitro.scroll + delta);
            update_hover(pt.x, pt.y);
            redraw();
            return 0;
        }
        if (quests_view() && pt.x >= S(RAIL_W + SIDE_W)) {
            quests_scroll_by(delta);
            update_hover(pt.x, pt.y);
            redraw();
            return 0;
        }
        if (shop_view() && pt.x >= S(RAIL_W + SIDE_W)) {
            shop_scroll_by(delta);
            update_hover(pt.x, pt.y);
            redraw();
            return 0;
        }
        if (forum_view() && pt.x >= S(RAIL_W + SIDE_W) && pt.x < main_right()) {
            int max = g_ui.forum_content - (int)(S(HEADER_H));
            g_ui.forum_scroll += delta;
            if (g_ui.forum_scroll > max)
                g_ui.forum_scroll = max;
            if (g_ui.forum_scroll < 0)
                g_ui.forum_scroll = 0;
            redraw();
            return 0;
        }
        if (g_ui.results_open) {
            RECT sr = results_rect();
            if (pt.x >= sr.left && pt.x < sr.right && pt.y >= sr.top && pt.y < sr.bottom) {
                int max = g_ui.results_content - (sr.bottom - sr.top - S(56));
                g_ui.results_scroll += delta;
                if (g_ui.results_scroll > max)
                    g_ui.results_scroll = max;
                if (g_ui.results_scroll < 0)
                    g_ui.results_scroll = 0;
                redraw();
                return 0;
            }
        }
        if (g_ui.pins_open) {
            RECT pr = pins_rect();
            if (pt.x >= pr.left && pt.x < pr.right && pt.y >= pr.top && pt.y < pr.bottom) {
                int max = g_ui.pins_content - (pr.bottom - pr.top - S(56));
                g_ui.pins_scroll += delta;
                if (g_ui.pins_scroll > max)
                    g_ui.pins_scroll = max;
                if (g_ui.pins_scroll < 0)
                    g_ui.pins_scroll = 0;
                redraw();
                return 0;
            }
        }
        if (friends_view() && pt.x >= S(RAIL_W + SIDE_W)) {
            g_ui.friend_scroll += delta;
            if (g_ui.friend_scroll < 0)
                g_ui.friend_scroll = 0;
        } else if (members_shown() && pt.x >= main_right()) {
            g_ui.ml_scroll += delta;
            ml_clamp();
            ml_request_visible();
        } else if (pt.x < S(RAIL_W)) {
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
        if (wp == TIMER_ACTIVE) {
            /* the elapsed times move on while Active Now shows; the next paint arms it again */
            KillTimer(g_ui.wnd, TIMER_ACTIVE);
            redraw();
            return 0;
        }
        if (wp == TIMER_ANIM) {
            anim_tick();
            return 0;
        }
        if (wp == TIMER_REACTORS) {
            react_timer();
            return 0;
        }
        if (wp == TIMER_FLASH) {
            KillTimer(wnd, TIMER_FLASH);
            g_ui.flash_id[0] = 0;
            redraw();
            return 0;
        }
        if (wp == TIMER_MIC) {
            int db = app_voice_mic_level();
            if (!g_ui.settings_open || g_ui.settings_page != SET_VOICE) {
                KillTimer(wnd, TIMER_MIC);
            } else if (db != g_ui.mic_db) {
                g_ui.mic_db = db;
                redraw();
            }
            return 0;
        }
        if (wp == TIMER_VOICE) {
            /* Repaint only when someone starts or stops speaking. */
            unsigned mask = 0;
            for (int k = 0, bit = 0; k < g_ui.nvoices && bit < 32; k++)
                if (lstrcmpA(g_ui.voices[k].channel, g_ui.voice_channel) == 0)
                    mask |= (unsigned)app_voice_speaking(g_ui.voices[k].user) << bit++;
            if (mask != g_ui.voice_speaking) {
                g_ui.voice_speaking = mask;
                redraw();
            }
            return 0;
        }
        if (wp == TIMER_TYPING) {
            typing_prune();
            redraw();
            return 0;
        }
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
    case WM_ACTIVATE: /* forwarded by the frame */
        if (LOWORD(wp) != WA_INACTIVE)
            redraw(); /* animations pick up again on the next paint */
        /* Coming back to the window counts as reading the open channel. */
        if (LOWORD(wp) != WA_INACTIVE && g_ui.model && g_ui.channel >= 0 &&
            (model_unread(g_ui.model, (unsigned)g_ui.channel) || chan(g_ui.channel)->mentions)) {
            mark_read(g_ui.channel);
            redraw();
        }
        return 0;
    case WM_VSYNC:
        on_vsync();
        return 0;
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
    default:
        if (msg >= UI_QR && msg <= UI_QUESTS) {
            on_worker(msg, wp, lp);
            return 0;
        }
        break;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

/* ---- Window frame: a title bar of our own, as Discord draws it ---- */

#define TITLE_H 32
#define TB_BTN_W 36 /* minimize, maximize, close */
enum { TB_NONE, TB_BACK, TB_FORWARD, TB_INBOX, TB_HELP, TB_MIN, TB_MAX, TB_CLOSE };

/* Part of the title bar at (x, y), in the frame's client coordinates. */
static int tb_part(int x, int y)
{
    RECT rc;
    int r;

    GetClientRect(g_ui.top, &rc);
    r = rc.right;
    if (y < 0 || y >= S(TITLE_H))
        return TB_NONE;
    if (x >= r - S(TB_BTN_W))
        return TB_CLOSE;
    if (x >= r - 2 * S(TB_BTN_W))
        return TB_MAX;
    if (x >= r - 3 * S(TB_BTN_W))
        return TB_MIN;
    if (x >= r - S(158) && x < r - S(126))
        return TB_HELP;
    if (x >= r - S(194) && x < r - S(162))
        return TB_INBOX;
    if (x >= S(10) && x < S(36))
        return TB_BACK;
    if (x >= S(36) && x < S(62))
        return TB_FORWARD;
    return TB_NONE;
}

/* What the window shows, for the middle of the title bar and the taskbar. */
static const char *tb_title(r_image_t **icon, const wchar_t **glyph)
{
    *icon = NULL;
    *glyph = NULL;
    if (g_ui.view != VIEW_APP || !g_ui.model)
        return "Silicord";
    if (g_ui.settings_open) {
        *glyph = L"\xE713";
        return "User Settings";
    }
    if (g_ui.guild >= 0) {
        *icon = guild_icon(&g_ui.model->guilds[g_ui.guild]);
        return model_str(g_ui.model, g_ui.model->guilds[g_ui.guild].name);
    }
    if (friends_view()) {
        *glyph = L"\xE716";
        return "Friends";
    }
    if (g_ui.channel < 0 && g_ui.home_page == HOME_NITRO) {
        *glyph = L"\xE734";
        return "Nitro";
    }
    if (g_ui.channel < 0 && g_ui.home_page == HOME_SHOP) {
        *glyph = L"\xE719";
        return "Shop";
    }
    if (g_ui.channel < 0 && g_ui.home_page == HOME_QUESTS) {
        *glyph = L"\xE7C1";
        return "Quests";
    }
    *glyph = L"\xE8BD";
    return "Direct Messages";
}

static void paint_titlebar(RECT rc)
{
    int r = rc.right, cy = S(TITLE_H) / 2, h = g_ui.tb_hover;
    int back = g_ui.hist_pos > 0, fwd = g_ui.hist_pos + 1 < g_ui.hist_n;
    unsigned idle = ARGB(C_MUTED), hot = ARGB(C_INK), off = 0xFF4B4C4Fu;
    const wchar_t *glyph;
    r_image_t *icon;
    const char *title = tb_title(&icon, &glyph);
    wchar_t *wt = utf8_to_wide(title, lstrlenA(title));
    int tw = r_text_width(g_ui.f_tb, wt, -1), iw = icon || glyph ? S(20) + S(8) : 0, tx;

    fill(0, 0, r, S(TITLE_H), C_RAIL);
    /* Back and forward, dimmed with nowhere to go. */
    r_text(g_ui.f_icon_tb, !back ? off : h == TB_BACK ? hot : idle, S(10), 0, S(26), S(TITLE_H), L"\xE72B", -1,
           R_CENTER | R_VCENTER | R_SINGLE);
    r_text(g_ui.f_icon_tb, !fwd ? off : h == TB_FORWARD ? hot : idle, S(36), 0, S(26), S(TITLE_H), L"\xE72A", -1,
           R_CENTER | R_VCENTER | R_SINGLE);

    /* In the middle: where we are, with its icon. */
    if (tw > r / 2)
        tw = r / 2;
    tx = (r - tw - iw) / 2;
    if (icon)
        r_image(icon, tx, cy - S(10), S(20), S(20), S(6));
    else if (glyph)
        r_text(g_ui.f_icon_tb, idle, tx, 0, S(20), S(TITLE_H), glyph, -1, R_CENTER | R_VCENTER | R_SINGLE);
    r_text(g_ui.f_tb, ARGB(C_TEXT), tx + iw, 0, tw, S(TITLE_H), wt, -1, R_LEFT | R_VCENTER | R_SINGLE | R_ELLIPSIS);
    mem_free(wt);

    /* Inbox and help, then the window's buttons. */
    r_text(g_ui.f_icon_tb, h == TB_INBOX || (g_ui.pins_open && g_ui.pins_inbox) ? hot : idle, r - S(194), 0, S(32),
           S(TITLE_H), L"\xE715", -1, R_CENTER | R_VCENTER | R_SINGLE);
    r_text(g_ui.f_icon_tb, h == TB_HELP ? hot : idle, r - S(158), 0, S(32), S(TITLE_H), L"\xE897", -1,
           R_CENTER | R_VCENTER | R_SINGLE);
    fill(r - 3 * S(TB_BTN_W) - S(6), cy - S(10), S(1) > 1 ? S(1) : 1, S(20), C_LINE);
    {
        const wchar_t *g[3] = {L"\xE921", IsZoomed(g_ui.top) ? L"\xE923" : L"\xE922", L"\xE8BB"};
        for (int k = 0; k < 3; k++) {
            int bx = r - (3 - k) * S(TB_BTN_W), part = TB_MIN + k, on = h == part;
            if (on)
                r_fill(bx, 0, S(TB_BTN_W), S(TITLE_H), part == TB_CLOSE ? 0xFFD83C3Eu : 0xFF1E1E20u);
            r_text(g_ui.f_caption, on ? (part == TB_CLOSE ? 0xFFFFFFFFu : hot) : idle, bx, 0, S(TB_BTN_W), S(TITLE_H), g[k],
                   -1, R_CENTER | R_VCENTER | R_SINGLE);
        }
    }
}

static void tb_set_hover(int part)
{
    if (part != g_ui.tb_hover) {
        g_ui.tb_hover = part;
        InvalidateRect(g_ui.top, NULL, FALSE);
    }
}

static void hist_go(int step)
{
    int pos = g_ui.hist_pos + step, c;

    if (!g_ui.model || pos < 0 || pos >= g_ui.hist_n)
        return;
    g_ui.hist_pos = pos;
    g_ui.hist_nav = 1;
    if (!g_ui.hist[pos][0]) {
        g_ui.last_dm = -1;
        select_guild(-1);
    } else if ((c = model_find_channel(g_ui.model, g_ui.hist[pos])) >= 0) {
        go_to_channel(c);
    }
    g_ui.hist_nav = 0;
    InvalidateRect(g_ui.top, NULL, FALSE);
}

static void tb_click(int part)
{
    switch (part) {
    case TB_BACK:
        hist_go(-1);
        break;
    case TB_FORWARD:
        hist_go(1);
        break;
    case TB_INBOX:
        pins_toggle(1);
        redraw();
        break;
    case TB_HELP:
        ShellExecuteW(NULL, L"open", L"https://github.com/mikketa/silicord#readme", NULL, NULL, SW_SHOWNORMAL);
        break;
    case TB_MIN:
        ShowWindow(g_ui.top, SW_MINIMIZE);
        break;
    case TB_MAX:
        ShowWindow(g_ui.top, IsZoomed(g_ui.top) ? SW_RESTORE : SW_MAXIMIZE);
        break;
    case TB_CLOSE:
        SendMessageW(g_ui.top, WM_CLOSE, 0, 0);
        break;
    }
}

static int frame_border(HWND wnd, int x)
{
    UINT dpi = GetDpiForWindow(wnd);

    return GetSystemMetricsForDpi(x ? SM_CXFRAME : SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

static LRESULT CALLBACK frame_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_ui.top = wnd;
        return 0;
    case WM_NCCALCSIZE:
        /* No caption: the client area reaches the top. The other borders stay, to resize by. */
        if (wp) {
            RECT *r = &((NCCALCSIZE_PARAMS *)lp)->rgrc[0];
            r->left += frame_border(wnd, 1);
            r->right -= frame_border(wnd, 1);
            r->bottom -= frame_border(wnd, 0);
            if (IsZoomed(wnd)) /* maximized, the frame hangs off the screen */
                r->top += frame_border(wnd, 0);
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProcW(wnd, msg, wp, lp);
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (hit != HTCLIENT)
            return hit;
        ScreenToClient(wnd, &pt);
        if (!IsZoomed(wnd) && pt.y < frame_border(wnd, 0))
            return HTTOP;
        switch (tb_part(pt.x, pt.y)) {
        case TB_MIN:
            return HTMINBUTTON;
        case TB_MAX:
            return HTMAXBUTTON; /* Windows 11 shows its snap layouts over it */
        case TB_CLOSE:
            return HTCLOSE;
        case TB_NONE:
            return pt.y < S(TITLE_H) ? HTCAPTION : HTCLIENT;
        }
        return HTCLIENT;
    }
    case WM_NCMOUSEMOVE: {
        TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE | TME_NONCLIENT, wnd, 0};
        int part = wp == HTMINBUTTON ? TB_MIN : wp == HTMAXBUTTON ? TB_MAX : wp == HTCLOSE ? TB_CLOSE : TB_NONE;
        TrackMouseEvent(&tme);
        tb_set_hover(part);
        if (part != TB_NONE)
            return 0;
        break;
    }
    case WM_NCMOUSELEAVE:
    case WM_MOUSELEAVE:
        tb_set_hover(TB_NONE);
        break;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE) {
            g_ui.tb_down = (int)wp;
            return 0; /* not DefWindowProc: it would draw the classic buttons */
        }
        break;
    case WM_NCLBUTTONUP:
        if (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE) {
            if (g_ui.tb_down == (int)wp)
                tb_click(wp == HTMINBUTTON ? TB_MIN : wp == HTMAXBUTTON ? TB_MAX : TB_CLOSE);
            g_ui.tb_down = 0;
            return 0;
        }
        break;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, wnd, 0};
        TrackMouseEvent(&tme);
        tb_set_hover(tb_part(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
        return 0;
    }
    case WM_LBUTTONUP:
        tb_click(tb_part(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
        return 0;
    case WM_SIZE:
        if (!g_ui.wnd)
            return 0;
        if (wp == SIZE_MINIMIZED) {
            SendMessageW(g_ui.wnd, WM_SIZE, SIZE_MINIMIZED, 0);
            return 0;
        }
        MoveWindow(g_ui.wnd, 0, S(TITLE_H), LOWORD(lp), HIWORD(lp) - S(TITLE_H), TRUE);
        InvalidateRect(wnd, NULL, FALSE);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            g_ui.saved_focus = GetFocus();
        } else if (!HIWORD(wp)) {
            HWND f = g_ui.saved_focus;
            SetFocus(f && IsWindow(f) && IsChild(wnd, f) ? f : g_ui.wnd);
        }
        if (g_ui.wnd)
            SendMessageW(g_ui.wnd, WM_ACTIVATE, wp, lp);
        return 0;
    case WM_SETFOCUS:
        if (g_ui.wnd)
            SetFocus(g_ui.wnd);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = MulDiv(940, GetDpiForWindow(wnd), 96);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = MulDiv(620, GetDpiForWindow(wnd), 96);
        return 0;
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        g_ui.dpi = HIWORD(wp);
        make_fonts();
        set_icons(g_ui.dpi);
        SetWindowPos(wnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_frame(wnd, paint_titlebar);
        return 0;
    case WM_CLOSE:
        Shell_NotifyIconW(NIM_DELETE, &g_ui.tray);
        app_quit();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

HWND ui_create(HINSTANCE inst)
{
    WNDCLASSEXW wc = {0};
    BOOL dark = TRUE;
    COLORREF caption = GDI(C_RAIL);
    UINT dpi = GetDpiForSystem();

    /* Single-threaded COM on the UI thread: the file dialog needs it (image decoding works either way). */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    r_init();

    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = load_icon(SM_CXICON, dpi); /* the window gets its own, per monitor, in set_icons() */
    wc.hIconSm = load_icon(SM_CXSMICON, dpi);
    wc.lpszClassName = L"Silicord";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = frame_proc;
    wc.lpszClassName = L"SilicordFrame";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = pop_proc;
    wc.hIcon = wc.hIconSm = NULL;
    wc.lpszClassName = L"SilicordPopout";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = picker_proc;
    wc.lpszClassName = L"SilicordEmoji";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = qs_proc;
    wc.lpszClassName = L"SilicordSwitch";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = menu_proc;
    wc.lpszClassName = L"SilicordMenu";
    RegisterClassExW(&wc);

    /* The frame, then the app inside it under the title bar. */
    CreateWindowExW(0, L"SilicordFrame", L"Silicord", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                    MulDiv(1200, dpi, 96), MulDiv(760, dpi, 96), NULL, NULL, inst, NULL);
    DwmSetWindowAttribute(g_ui.top, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    DwmSetWindowAttribute(g_ui.top, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof caption);
    SetWindowPos(g_ui.top, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    CreateWindowExW(0, L"Silicord", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0, g_ui.top, NULL,
                    inst, NULL);
    {
        RECT rc;
        GetClientRect(g_ui.top, &rc);
        MoveWindow(g_ui.wnd, 0, S(TITLE_H), rc.right, rc.bottom - S(TITLE_H), FALSE);
    }
    set_icons(GetDpiForWindow(g_ui.top));

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
    return g_ui.top;
}
