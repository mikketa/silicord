/* Silicord - native Discord client for Windows. No C runtime: entry point is `entry`. */
#include <windows.h>
#include "console.h"
#include "cred.h"
#include "gw.h"
#include "http.h"
#include "json.h"
#include "mem.h"
#include "model.h"
#include "msg.h"
#include "ra.h"
#include "sb.h"
#include "ui.h"
#include "utf.h"
#include "voice.h"
#include "audio.h"
#include "mixer.h"
#include "opus.h"
#include "opus_math.h"
#include "vp8.h"
#include "vp8_enc.h"
#include "camera.h"

#define JOIN_TIMEOUT 5000

static int g_debug;
static sb_t g_token;                 /* current account, UI thread only */
static HANDLE g_login_thread;
static HANDLE g_login_wake;
static volatile LONG g_login_stop;
static HANDLE g_session_thread;
static volatile LONG g_session_id;   /* bumped to silence a session that is being stopped */
static CRITICAL_SECTION g_open_lock;
static CRITICAL_SECTION g_session_lock; /* orders stopping a session against its gateway reset and token save */
static char g_open_channel[24];     /* live messages are forwarded for this channel only */

static void log_line(const char *prefix, const char *text)
{
    sb_t out = {0};

    if (!g_debug)
        return;
    sb_add(&out, prefix);
    sb_add(&out, text);
    sb_add(&out, "\r\n");
    con_print_sb(&out);
    sb_free(&out);
}

static sb_t *copy(const char *s, size_t n)
{
    sb_t *sb = mem_alloc(sizeof *sb);

    sb_addn(sb, s, n);
    return sb;
}

static void join(HANDLE *thread)
{
    if (*thread) {
        WaitForSingleObject(*thread, JOIN_TIMEOUT);
        CloseHandle(*thread);
        *thread = NULL;
    }
}

/* ---- QR login ---- */

static void on_qr(void *ctx, const char *url)
{
    (void)ctx;
    log_line("login: new code ", url);
    if (!g_login_stop)
        ui_post(UI_QR, ui_text(url));
}

static void on_scanned(void *ctx, const char *name)
{
    (void)ctx;
    log_line("login: scanned by ", name);
    if (!g_login_stop)
        ui_post(UI_SCANNED, ui_text(name));
}

static void on_login_status(void *ctx, const char *text)
{
    (void)ctx;
    log_line("login: ", text);
    if (!g_login_stop)
        ui_post(UI_STATUS, ui_text(text));
}

static DWORD WINAPI login_main(LPVOID arg)
{
    static const ra_events_t ev = {NULL, on_qr, on_scanned, on_login_status};
    sb_t token = {0};

    (void)arg;
    while (!g_login_stop) {
        if (ra_login(&ev, &token)) {
            if (!g_login_stop)
                ui_post(UI_TOKEN, copy(token.data, token.len));
            break;
        }
        sb_clear(&token);
        /* Do not hammer the server when offline. */
        WaitForSingleObject(g_login_wake, 3000);
    }
    sb_free(&token);
    return 0;
}

static void stop_login(void)
{
    if (!g_login_thread)
        return;
    InterlockedExchange(&g_login_stop, 1);
    SetEvent(g_login_wake);
    ra_cancel();
    join(&g_login_thread);
}

static void start_login(void)
{
    stop_login();
    InterlockedExchange(&g_login_stop, 0);
    ResetEvent(g_login_wake);
    ra_reset();
    ui_show_login();
    g_login_thread = CreateThread(NULL, 0, login_main, NULL, 0, NULL);
}

/* ---- Session: check the token, then hold the gateway ---- */

typedef struct {
    sb_t token;
    sb_t last_status;
    LONG id;
    char me[24];        /* our user id, known after READY */
    HANDLE prev;        /* the previous session's thread, if it had not ended yet */
} session_t;

static int current(const session_t *s)
{
    return s->id == g_session_id;
}

static void on_gw_status(void *ctx, const char *text)
{
    session_t *s = ctx;

    log_line("gateway: ", text);
    sb_clear(&s->last_status);
    sb_add(&s->last_status, text);
    if (!current(s))
        return;
    if (lstrcmpA(text, "Online") == 0)
        ui_post(UI_ONLINE, NULL);
    else
        ui_post(UI_STATUS, ui_text(text));
}

/* Debug only: logs the shape of READY (key names, never values). */
static void log_keys(const char *label, json_t v)
{
    json_iter_t it;
    json_t k;
    sb_t out = {0};

    if (!g_debug)
        return;
    sb_add(&out, label);
    sb_add(&out, json_type(v) == JSON_ARRAY ? " (array) " : " ");
    if (json_type(v) == JSON_ARRAY) {
        sb_u64(&out, json_count(v));
        sb_add(&out, " items");
    } else {
        json_iter(v, &it);
        while (json_next(&it, &k, NULL)) {
            sb_addn(&out, k.p + 1, (size_t)(k.end - k.p - 2));
            sb_add(&out, " ");
        }
    }
    log_line("ready: ", out.data);
    sb_free(&out);
}

static void log_ready_shape(json_t d)
{
    json_t guilds, g, v, first;
    json_iter_t it;

    log_keys("d:", d);
    if (json_get(d, "guilds", &guilds)) {
        json_iter(guilds, &it);
        if (json_next(&it, NULL, &g)) {
            log_keys("guild:", g);
            if (json_get(g, "properties", &v))
                log_keys("guild.properties:", v);
            if (json_get(g, "channels", &v)) {
                json_iter(v, &it);
                if (json_next(&it, NULL, &first))
                    log_keys("guild.channels[0]:", first);
            }
            if (json_get(g, "roles", &v)) {
                json_iter(v, &it);
                if (json_next(&it, NULL, &first))
                    log_keys("guild.roles[0]:", first);
            }
            if (json_get(g, "members", &v))
                log_keys("guild.members:", v);
        }
    }
    if (json_get(d, "merged_members", &v)) {
        log_keys("merged_members:", v);
        json_iter(v, &it);
        if (json_next(&it, NULL, &first)) {
            log_keys("merged_members[0]:", first);
            json_iter(first, &it);
            if (json_next(&it, NULL, &g))
                log_keys("merged_members[0][0]:", g);
        }
    }
    if (json_get(d, "user", &v))
        log_keys("user:", v);
    if (json_get(d, "relationships", &v)) {
        log_keys("relationships:", v);
        json_iter(v, &it);
        if (json_next(&it, NULL, &first)) {
            log_keys("relationships[0]:", first);
            if (json_get(first, "user", &g))
                log_keys("relationships[0].user:", g);
        }
    }
}

static int is_open(const char *channel_id)
{
    int open;

    EnterCriticalSection(&g_open_lock);
    open = g_open_channel[0] && lstrcmpA(g_open_channel, channel_id) == 0;
    LeaveCriticalSection(&g_open_lock);
    return open;
}

/* Named in the message (@everyone and roles are told apart: settings can silence them). */
static int mentions_me(json_t d, const char *me)
{
    json_t v, list, user;
    json_iter_t it;
    char id[24];

    if (!json_get(d, "mentions", &list))
        return 0;
    json_iter(list, &it);
    while (json_next(&it, NULL, &user))
        if (json_get(user, "id", &v)) {
            json_raw(v, id, sizeof id);
            if (lstrcmpA(id, me) == 0)
                return 1;
        }
    return 0;
}

static void post_activity(session_t *s, json_t d, const msg_t *m)
{
    activity_t *a = mem_alloc(sizeof *a);
    json_t v, roles, role;
    json_iter_t it;

    a->kind = ACTIVITY_MESSAGE;
    lstrcpynA(a->channel_id, m->channel_id, sizeof a->channel_id);
    if (json_get(d, "guild_id", &v))
        json_raw(v, a->guild_id, sizeof a->guild_id);
    if (json_get(d, "mention_roles", &roles)) {
        char id[24];
        json_iter(roles, &it);
        while (json_next(&it, NULL, &role)) {
            json_raw(role, id, sizeof id);
            if (a->mention_roles.len)
                sb_add(&a->mention_roles, ",");
            sb_add(&a->mention_roles, id);
        }
    }
    lstrcpynA(a->message_id, m->id, sizeof a->message_id);
    a->from_me = s->me[0] && lstrcmpA(m->author_id, s->me) == 0;
    a->mentions_me = !a->from_me && mentions_me(d, s->me);
    a->everyone = !a->from_me && json_get(d, "mention_everyone", &v) && json_type(v) == JSON_TRUE;
    sb_addn(&a->author, m->author.data ? m->author.data : "", m->author.len);
    sb_addn(&a->preview, m->text.data ? m->text.data : "", m->text.len);
    ui_post_activity(a);
}

static int is_structure_event(json_t t)
{
    static const char *const names[] = {
        "CHANNEL_CREATE", "CHANNEL_UPDATE", "CHANNEL_DELETE",
        "GUILD_CREATE", "GUILD_UPDATE", "GUILD_DELETE", "GUILD_MEMBER_UPDATE",
        "GUILD_ROLE_CREATE", "GUILD_ROLE_UPDATE", "GUILD_ROLE_DELETE", "GUILD_MEMBERS_CHUNK",
        "GUILD_MEMBER_LIST_UPDATE", "PRESENCE_UPDATE", "GUILD_EMOJIS_UPDATE", "GUILD_STICKERS_UPDATE",
        "USER_GUILD_SETTINGS_UPDATE", "USER_SETTINGS_UPDATE", "VOICE_STATE_UPDATE",
        "CALL_CREATE", "CALL_UPDATE", "CALL_DELETE",
        "RELATIONSHIP_ADD", "RELATIONSHIP_REMOVE", "RELATIONSHIP_UPDATE",
        "THREAD_CREATE", "THREAD_UPDATE", "THREAD_DELETE", "THREAD_MEMBER_UPDATE", "THREAD_MEMBERS_UPDATE",
    };

    for (int i = 0; i < (int)ARRAYSIZE(names); i++)
        if (json_str_eq(t, names[i]))
            return 1;
    return 0;
}

/* Hands the raw event to the UI thread, which owns the model. */
static void forward_event(session_t *s, json_t t, json_t d)
{
    json_t user, v;
    sb_t *p;

    (void)user;
    (void)v;
    if (!current(s))
        return;
    p = mem_alloc(sizeof *p);
    sb_addn(p, t.p + 1, (size_t)(t.end - t.p - 2));
    sb_addn(p, "", 1);
    sb_addn(p, d.p, (size_t)(d.end - d.p));
    ui_post(UI_EVENT, p);
}

/* ---- Voice: op 4 asks to join, the gateway answers with our voice state and the voice server ---- */

static CRITICAL_SECTION g_voice_lock;
static struct {
    int active;           /* we asked to be in `channel` */
    int have_state, have_server;
    char guild[24], channel[24];
    voice_params_t p;
} g_vc;

/* What we hear: every speaker's packets through the mixer to the sound card. */
static CRITICAL_SECTION g_mix_lock;
static mixer_t g_mixer;

/* The voice settings, set on the UI thread and read on the audio thread. */
static voice_prefs_t g_prefs = {0, 0, 100, 100, -50, 0, 0};

/* Each person's volume in calls (under g_mix_lock), kept across their leaving and coming back. */
static struct {
    unsigned long long user;
    float volume;
} g_volumes[64];
static int g_nvolumes;

static unsigned long long parse_id(const char *s)
{
    unsigned long long u = 0;

    for (; *s >= '0' && *s <= '9'; s++)
        u = u * 10 + (unsigned)(*s - '0');
    return u;
}

static float user_volume(unsigned long long user)
{
    for (int i = 0; i < g_nvolumes; i++)
        if (g_volumes[i].user == user)
            return g_volumes[i].volume;
    return 1.f;
}

void app_voice_user_volume(const char *user_id, int percent)
{
    unsigned long long u = parse_id(user_id);
    int i;

    EnterCriticalSection(&g_mix_lock);
    for (i = 0; i < g_nvolumes && g_volumes[i].user != u; i++)
        ;
    if (i < (int)ARRAYSIZE(g_volumes)) {
        g_volumes[i].user = u;
        g_volumes[i].volume = (float)percent / 100.f;
        if (i == g_nvolumes)
            g_nvolumes++;
    }
    LeaveCriticalSection(&g_mix_lock);
}

static void voice_frame(void *ctx, unsigned long long user, unsigned seq, const unsigned char *opus, size_t n)
{
    (void)ctx;
    EnterCriticalSection(&g_mix_lock);
    mixer_set_volume(&g_mixer, user, user_volume(user));
    mixer_push(&g_mixer, user, seq, opus, n);
    LeaveCriticalSection(&g_mix_lock);
}

static void audio_play(void *ctx, float *out)
{
    (void)ctx;
    EnterCriticalSection(&g_mix_lock);
    g_mixer.gain = (float)g_prefs.out_volume / 100.f;
    mixer_pull(&g_mixer, out);
    LeaveCriticalSection(&g_mix_lock);
}

int app_voice_speaking(const char *user_id)
{
    unsigned long long u = parse_id(user_id);
    int on;

    EnterCriticalSection(&g_mix_lock);
    on = mixer_speaking(&g_mixer, u);
    LeaveCriticalSection(&g_mix_lock);
    return on;
}

/* What we say: the microphone at its volume, opened by voice activity or push to talk, encoded and sent. */
#define MIC_HANG 15 /* voice activity keeps sending 300 ms after the last loud block */
#define PTT_HANG 2  /* push to talk sends 20 ms more after the key is released */

enum { AUDIO_OFF, AUDIO_CALL, AUDIO_TEST };

static opus_encoder_t g_encoder;
static volatile LONG g_muted, g_deafened, g_mic_db = -100;
static int g_mic_hang, g_audio_mode;
static CRITICAL_SECTION g_audio_lock; /* starting and stopping the sound card, from the UI and voice threads */
static float g_mic[960], g_echo[960];

/* The microphone block at its volume, into g_mic; its level goes to g_mic_db. */
static void mic_level(const float *in)
{
    float gain = (float)g_prefs.in_volume / 100.f, sum = 0, db;

    for (int i = 0; i < 960; i++) {
        float s = in[i] * gain;
        g_mic[i] = s > 1.f ? 1.f : s < -1.f ? -1.f : s;
        sum += g_mic[i] * g_mic[i];
    }
    db = sum > 0 ? 3.0103f * om_log2(sum / 960) : -100.f; /* 10 log10 of the mean square */
    InterlockedExchange(&g_mic_db, (LONG)(db < -100.f ? -100.f : db));
}

static void audio_capture(void *ctx, const float *in)
{
    unsigned char packet[1276];
    int n;

    (void)ctx;
    mic_level(in);
    if (g_muted || g_deafened) {
        if (g_mic_hang) {
            g_mic_hang = 0;
            voice_quiet();
        }
        return;
    }
    if (g_prefs.push_to_talk) {
        if (g_prefs.ptt_key && GetAsyncKeyState(g_prefs.ptt_key) < 0)
            g_mic_hang = PTT_HANG;
    } else if (g_mic_db > g_prefs.sensitivity) {
        g_mic_hang = MIC_HANG;
    }
    if (!g_mic_hang)
        return;
    n = opus_encode(&g_encoder, g_mic, packet);
    if (n)
        voice_send(packet, (size_t)n);
    if (--g_mic_hang == 0)
        voice_quiet();
}

static void app_voice_deafen(int deafened)
{
    InterlockedExchange(&g_deafened, deafened);
    EnterCriticalSection(&g_mix_lock);
    g_mixer.deafened = deafened;
    LeaveCriticalSection(&g_mix_lock);
}

static void app_voice_mute(int muted)
{
    InterlockedExchange(&g_muted, muted);
}

/* The microphone test: what it hears, played back. */
static void test_capture(void *ctx, const float *in)
{
    (void)ctx;
    mic_level(in);
    EnterCriticalSection(&g_mix_lock);
    CopyMemory(g_echo, g_mic, sizeof g_echo);
    LeaveCriticalSection(&g_mix_lock);
}

static void test_play(void *ctx, float *out)
{
    float gain = (float)g_prefs.out_volume / 100.f;

    (void)ctx;
    EnterCriticalSection(&g_mix_lock);
    for (int i = 0; i < 960; i++)
        out[2 * i] = out[2 * i + 1] = g_echo[i] * gain;
    LeaveCriticalSection(&g_mix_lock);
}

/* Opens the sound card for a call or the test with the chosen devices, or closes it. */
static void audio_mode(int mode)
{
    audio_io_t io = {0};

    io.play = mode == AUDIO_TEST ? test_play : audio_play;
    io.capture = mode == AUDIO_TEST ? test_capture : audio_capture;
    io.out_device = (unsigned)g_prefs.out_device;
    io.in_device = (unsigned)g_prefs.in_device;
    EnterCriticalSection(&g_audio_lock);
    InterlockedExchange(&g_mic_db, -100);
    ZeroMemory(g_echo, sizeof g_echo);
    if (mode == AUDIO_OFF)
        audio_stop();
    g_audio_mode = mode != AUDIO_OFF && audio_start(&io) ? mode : AUDIO_OFF;
    LeaveCriticalSection(&g_audio_lock);
}

void app_voice_prefs(const voice_prefs_t *p)
{
    int devices = p->in_device != g_prefs.in_device || p->out_device != g_prefs.out_device;

    g_prefs = *p;
    if (devices && g_audio_mode != AUDIO_OFF)
        audio_mode(g_audio_mode);
}

int app_voice_mic_level(void)
{
    return g_audio_mode != AUDIO_OFF ? (int)g_mic_db : -100;
}

int app_voice_mic_test(int on)
{
    if (on && g_audio_mode == AUDIO_OFF)
        audio_mode(AUDIO_TEST);
    else if (!on && g_audio_mode == AUDIO_TEST)
        audio_mode(AUDIO_OFF);
    return g_audio_mode == AUDIO_TEST;
}

static void voice_state_changed(void *ctx, int state, const char *text)
{
    sb_t *p = mem_alloc(sizeof *p);

    (void)ctx;
    if (state == VOICE_CONNECTED) {
        /* Still running when we moved to another channel or voice server: it encodes with g_encoder. */
        audio_mode(AUDIO_OFF);
        opus_encoder_init(&g_encoder);
        g_mic_hang = 0;
        audio_mode(AUDIO_CALL);
    } else if (state != VOICE_CONNECTING) {
        audio_mode(AUDIO_OFF);
    }
    sb_i64(p, state);
    sb_addn(p, "", 1);
    sb_add(p, text);
    ui_post(UI_VOICE, p);
}

/* ---- Video from the call: one decoder per person, the latest picture kept as BGRA for the UI ---- */

#define MAX_VIEWS 16

static struct {
    unsigned long long user;
    vp8_decoder_t *dec;   /* the UDP thread's alone */
    unsigned *bgra;       /* under g_video_lock */
    int w, h;
    unsigned serial;      /* bumped with each new picture */
} g_views[MAX_VIEWS];
static int g_nviews;
static CRITICAL_SECTION g_video_lock;
static volatile LONG g_video_posted;

static int view_find(unsigned long long user)
{
    for (int i = 0; i < g_nviews; i++)
        if (g_views[i].user == user)
            return i;
    return -1;
}

static unsigned char clamp8(int v)
{
    return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* BT.601 studio range, as VP8 video is. */
static void i420_to_bgra(const vp8_image_t *img, unsigned *out)
{
    for (int y = 0; y < img->h; y++) {
        const unsigned char *py = img->y + y * img->y_stride, *pu = img->u + (y >> 1) * img->uv_stride,
                            *pv = img->v + (y >> 1) * img->uv_stride;
        unsigned *o = out + (size_t)y * (size_t)img->w;
        for (int x = 0; x < img->w; x++) {
            int c = 298 * (py[x] - 16), d = pu[x >> 1] - 128, e = pv[x >> 1] - 128;
            o[x] = 0xFF000000u | (unsigned)clamp8((c + 409 * e + 128) >> 8) << 16 |
                   (unsigned)clamp8((c - 100 * d - 208 * e + 128) >> 8) << 8 | clamp8((c + 516 * d + 128) >> 8);
        }
    }
}

/* A picture into view i (under g_video_lock). */
static void view_store(int i, const vp8_image_t *img)
{
    if (g_views[i].w != img->w || g_views[i].h != img->h) {
        mem_free(g_views[i].bgra);
        g_views[i].bgra = mem_alloc((size_t)img->w * (size_t)img->h * 4);
        g_views[i].w = img->w;
        g_views[i].h = img->h;
    }
    i420_to_bgra(img, g_views[i].bgra);
    g_views[i].serial++;
}

static void voice_video(void *ctx, unsigned long long user, const unsigned char *vp8, size_t n)
{
    vp8_image_t img;
    int i, shown = 0;

    (void)ctx;
    /* Decoding under the lock: a view may be removed from another thread meanwhile. */
    EnterCriticalSection(&g_video_lock);
    i = view_find(user);
    if (i < 0 && g_nviews < MAX_VIEWS) {
        i = g_nviews++;
        g_views[i].user = user;
        g_views[i].dec = vp8_decoder_new();
        g_views[i].bgra = NULL;
        g_views[i].w = g_views[i].h = 0;
    }
    if (i >= 0 && vp8_decode(g_views[i].dec, vp8, n, &img) == 1) {
        view_store(i, &img);
        shown = 1;
    }
    LeaveCriticalSection(&g_video_lock);
    if (shown && !InterlockedExchange(&g_video_posted, 1))
        ui_post(UI_VIDEO, NULL);
}

static void view_remove(unsigned long long user)
{
    int i;

    EnterCriticalSection(&g_video_lock);
    if ((i = view_find(user)) >= 0) {
        vp8_decoder_free(g_views[i].dec);
        mem_free(g_views[i].bgra);
        g_views[i] = g_views[--g_nviews];
    }
    LeaveCriticalSection(&g_video_lock);
}

static void voice_video_state(void *ctx, unsigned long long user, int on)
{
    (void)ctx;
    if (!on)
        view_remove(user);
    ui_post(UI_VIDEO, NULL);
}

/* Someone left the call: free their decoders now rather than when the call ends. */
static void voice_left(void *ctx, unsigned long long user)
{
    EnterCriticalSection(&g_mix_lock);
    mixer_remove(&g_mixer, user);
    LeaveCriticalSection(&g_mix_lock);
    voice_video_state(ctx, user, 0);
}

/* ---- Our camera: captured, encoded and sent, and shown to us ---- */

#define CAMERA_W 640
#define CAMERA_H 360
#define CAMERA_FPS 15
#define CAMERA_KBPS 800

static vp8_encoder_t *g_venc;    /* the capture thread's */
static volatile LONG g_want_key, g_camera_on;
static sb_t g_vframe;

static void camera_frame(void *ctx, const vp8_image_t *img, unsigned long long ms)
{
    int key = (int)InterlockedExchange(&g_want_key, 0), i;

    (void)ctx;
    if (!g_venc) {
        g_venc = vp8_encoder_new(img->w, img->h, CAMERA_KBPS, CAMERA_FPS);
        key = 1;
    }
    if (g_venc && vp8_encode(g_venc, img, key, &g_vframe))
        voice_video_send((const unsigned char *)g_vframe.data, g_vframe.len, (unsigned)(ms * 90));
    /* Our own tile shows what we send. */
    EnterCriticalSection(&g_video_lock);
    i = view_find(g_vc.p.user_id);
    if (i < 0 && g_nviews < MAX_VIEWS) {
        i = g_nviews++;
        g_views[i].user = g_vc.p.user_id;
        g_views[i].dec = NULL;
        g_views[i].bgra = NULL;
        g_views[i].w = g_views[i].h = 0;
    }
    if (i >= 0)
        view_store(i, img);
    LeaveCriticalSection(&g_video_lock);
    if (!InterlockedExchange(&g_video_posted, 1))
        ui_post(UI_VIDEO, NULL);
}

static void voice_key_frame(void *ctx)
{
    (void)ctx;
    InterlockedExchange(&g_want_key, 1);
}

static void camera_off(void)
{
    if (!InterlockedExchange(&g_camera_on, 0))
        return;
    camera_stop();
    voice_video_active(0);
    vp8_encoder_free(g_venc);
    g_venc = NULL;
    sb_free(&g_vframe);
    view_remove(g_vc.p.user_id);
    ui_post(UI_VIDEO, NULL);
}

static void views_clear(void)
{
    while (g_nviews)
        view_remove(g_views[0].user);
}

int app_video_take(const char *user_id, unsigned *serial, void (*copy)(void *ctx, const unsigned *bgra, int w, int h),
                   void *ctx)
{
    unsigned long long u = parse_id(user_id);
    int i, fresh = 0;

    InterlockedExchange(&g_video_posted, 0);
    EnterCriticalSection(&g_video_lock);
    if ((i = view_find(u)) >= 0 && g_views[i].bgra && g_views[i].serial != *serial) {
        *serial = g_views[i].serial;
        copy(ctx, g_views[i].bgra, g_views[i].w, g_views[i].h);
        fresh = 1;
    }
    LeaveCriticalSection(&g_video_lock);
    return i >= 0 ? 1 + fresh : 0;
}

static void voice_log(void *ctx, const char *text)
{
    (void)ctx;
    log_line("", text);
}

/* Op 4: into a channel, or out of voice with channel NULL. */
static void send_voice_state(const char *guild, const char *channel)
{
    sb_t m = {0};

    /* A call in a direct message has no server. */
    if (guild && guild[0]) {
        sb_add(&m, "{\"op\":4,\"d\":{\"guild_id\":\"");
        sb_add(&m, guild);
        sb_add(&m, "\",\"channel_id\":");
    } else {
        sb_add(&m, "{\"op\":4,\"d\":{\"guild_id\":null,\"channel_id\":");
    }
    if (channel) {
        sb_add(&m, "\"");
        sb_add(&m, channel);
        sb_add(&m, "\"");
    } else {
        sb_add(&m, "null");
    }
    sb_add(&m, g_muted || g_deafened ? ",\"self_mute\":true" : ",\"self_mute\":false");
    sb_add(&m, g_camera_on ? ",\"self_video\":true" : ",\"self_video\":false");
    sb_add(&m, g_deafened ? ",\"self_deaf\":true}}" : ",\"self_deaf\":false}}");
    gw_send(&m);
    sb_free(&m);
}

/* Both halves known: connect (again, when Discord moves us to another voice server). */
static void voice_try_start(void)
{
    voice_events_t ev = {0};

    ev.state = voice_state_changed;
    ev.log = voice_log;
    ev.frame = voice_frame;
    ev.video = voice_video;
    ev.left = voice_left;
    ev.video_state = voice_video_state;
    ev.key_frame = voice_key_frame;
    if (g_vc.active && g_vc.have_state && g_vc.have_server) {
        g_vc.have_server = 0;
        voice_start(&g_vc.p, &ev);
    }
}

void app_voice_join(const char *guild_id, const char *channel_id)
{
    char old[24];
    int was;

    EnterCriticalSection(&g_voice_lock);
    was = g_vc.active;
    lstrcpynA(old, g_vc.guild, sizeof old);
    g_vc.active = 1;
    g_vc.have_state = g_vc.have_server = 0;
    lstrcpynA(g_vc.guild, guild_id, sizeof g_vc.guild);
    lstrcpynA(g_vc.channel, channel_id, sizeof g_vc.channel);
    LeaveCriticalSection(&g_voice_lock);
    voice_stop();
    app_voice_mic_test(0);
    if (was && lstrcmpA(old, guild_id) != 0)
        send_voice_state(old, NULL);
    send_voice_state(guild_id, channel_id);
}

/* Tells the others our new mute and deafen state. */
static void voice_state_update(void)
{
    char guild[24] = "", channel[24] = "";

    EnterCriticalSection(&g_voice_lock);
    if (g_vc.active) {
        lstrcpynA(guild, g_vc.guild, sizeof guild);
        lstrcpynA(channel, g_vc.channel, sizeof channel);
    }
    LeaveCriticalSection(&g_voice_lock);
    if (channel[0])
        send_voice_state(guild, channel);
}

int app_video_camera(int on)
{
    if (!on) {
        camera_off();
    } else if (!g_camera_on) {
        if (!voice_video_active(1))
            return 0;
        InterlockedExchange(&g_want_key, 1);
        InterlockedExchange(&g_camera_on, 1);
        if (!camera_start(0, CAMERA_W, CAMERA_H, CAMERA_FPS, camera_frame, NULL)) {
            InterlockedExchange(&g_camera_on, 0);
            voice_video_active(0);
            return 0;
        }
    }
    voice_state_update();
    return g_camera_on != 0;
}

void app_voice_set(int muted, int deafened)
{
    app_voice_mute(muted);
    app_voice_deafen(deafened);
    voice_state_update();
}

void app_voice_leave(void)
{
    char guild[24];
    int was;

    EnterCriticalSection(&g_voice_lock);
    was = g_vc.active;
    lstrcpynA(guild, g_vc.guild, sizeof guild);
    g_vc.active = 0;
    LeaveCriticalSection(&g_voice_lock);
    camera_off();
    voice_stop();
    audio_mode(AUDIO_OFF);
    views_clear();
    EnterCriticalSection(&g_mix_lock);
    mixer_free(&g_mixer);
    mixer_init(&g_mixer);
    LeaveCriticalSection(&g_mix_lock);
    if (was)
        send_voice_state(guild, NULL);
}

/* Our own VOICE_STATE_UPDATE (the session id) and VOICE_SERVER_UPDATE (token and endpoint). */
static void voice_dispatch(session_t *s, json_t t, json_t d)
{
    json_t v;
    char user[24] = "", channel[24] = "", guild[24] = "";

    if (json_get(d, "guild_id", &v) && json_type(v) == JSON_STRING)
        json_raw(v, guild, sizeof guild);
    EnterCriticalSection(&g_voice_lock);
    if (g_vc.active && lstrcmpA(guild, g_vc.guild) == 0) {
        if (json_str_eq(t, "VOICE_STATE_UPDATE")) {
            if (json_get(d, "user_id", &v))
                json_raw(v, user, sizeof user);
            if (json_get(d, "channel_id", &v) && json_type(v) == JSON_STRING)
                json_raw(v, channel, sizeof channel);
            if (lstrcmpA(user, s->me) == 0 && lstrcmpA(channel, g_vc.channel) == 0 && json_get(d, "session_id", &v)) {
                json_raw(v, g_vc.p.session_id, sizeof g_vc.p.session_id);
                g_vc.p.user_id = parse_id(s->me);
                /* in a direct message, the call's server is the channel */
                g_vc.p.server_id = parse_id(g_vc.guild[0] ? g_vc.guild : g_vc.channel);
                g_vc.p.channel_id = parse_id(g_vc.channel);
                g_vc.have_state = 1;
                voice_try_start();
            }
        } else if (json_get(d, "endpoint", &v) && json_type(v) == JSON_STRING && json_get(d, "token", &t)) {
            json_raw(v, g_vc.p.endpoint, sizeof g_vc.p.endpoint);
            json_raw(t, g_vc.p.token, sizeof g_vc.p.token);
            g_vc.have_server = 1;
            voice_try_start();
        }
    }
    LeaveCriticalSection(&g_voice_lock);
}

static void on_dispatch(void *ctx, json_t t, json_t d)
{
    session_t *s = ctx;
    int kind;
    msg_batch_t *b;

    if (current(s) && (json_str_eq(t, "VOICE_STATE_UPDATE") || json_str_eq(t, "VOICE_SERVER_UPDATE"))) {
        voice_dispatch(s, t, d);
        if (json_str_eq(t, "VOICE_SERVER_UPDATE"))
            return;
    }

    if (json_str_eq(t, "MESSAGE_ACK")) {
        json_t v;
        activity_t *a;
        if (!current(s))
            return;
        a = mem_alloc(sizeof *a);
        a->kind = ACTIVITY_ACK;
        if (json_get(d, "channel_id", &v))
            json_raw(v, a->channel_id, sizeof a->channel_id);
        if (json_get(d, "message_id", &v))
            json_raw(v, a->message_id, sizeof a->message_id);
        ui_post_activity(a);
        return;
    }
    if (is_structure_event(t)) {
        forward_event(s, t, d);
        return;
    }
    if (json_str_eq(t, "TYPING_START")) {
        json_t v, member, user;
        char channel[24] = "", user_id[24] = "";
        sb_t *p;
        if (json_get(d, "channel_id", &v))
            json_raw(v, channel, sizeof channel);
        if (json_get(d, "user_id", &v))
            json_raw(v, user_id, sizeof user_id);
        if (!current(s) || !is_open(channel) || lstrcmpA(user_id, s->me) == 0)
            return;
        p = mem_alloc(sizeof *p);
        sb_add(p, channel);
        sb_addn(p, "", 1);
        sb_add(p, user_id);
        sb_addn(p, "", 1);
        if (json_get(d, "member", &member)) {
            if (!(json_get(member, "nick", &v) && json_type(v) == JSON_STRING && json_str(v, p)) &&
                json_get(member, "user", &user) &&
                !((json_get(user, "global_name", &v) && json_type(v) == JSON_STRING && json_str(v, p))))
                if (json_get(user, "username", &v))
                    json_str(v, p);
        }
        ui_post(UI_TYPING, p);
        return;
    }
    if (json_str_eq(t, "MESSAGE_POLL_VOTE_ADD") || json_str_eq(t, "MESSAGE_POLL_VOTE_REMOVE")) {
        b = msg_batch_poll_vote(d, json_str_eq(t, "MESSAGE_POLL_VOTE_ADD") ? 1 : -1, s->me);
        if (b->n && current(s) && is_open(b->channel_id))
            ui_post_batch(b);
        else
            msg_batch_free(b);
        return;
    }
    if (json_str_eq(t, "MESSAGE_REACTION_ADD") || json_str_eq(t, "MESSAGE_REACTION_REMOVE")) {
        b = msg_batch_reaction(d, json_str_eq(t, "MESSAGE_REACTION_ADD") ? 1 : -1, s->me);
        if (b->n && current(s) && is_open(b->channel_id))
            ui_post_batch(b);
        else
            msg_batch_free(b);
        return;
    }
    if (json_str_eq(t, "MESSAGE_CREATE"))
        kind = BATCH_NEW;
    else if (json_str_eq(t, "MESSAGE_UPDATE"))
        kind = BATCH_UPDATE;
    else if (json_str_eq(t, "MESSAGE_DELETE"))
        kind = BATCH_DELETE;
    else
        return;
    b = msg_batch_one(d, kind);
    if (b->n && kind == BATCH_NEW && current(s))
        post_activity(s, d, &b->msgs[0]);
    if (b->n && current(s) && is_open(b->channel_id))
        ui_post_batch(b);
    else
        msg_batch_free(b);
}

static void on_ready(void *ctx, json_t d)
{
    session_t *s = ctx;
    model_t *model;
    json_t user, v;

    if (json_get(d, "user", &user) && json_get(user, "id", &v))
        json_raw(v, s->me, sizeof s->me);
    log_line("gateway: ", "ready");
    log_ready_shape(d);
    model = model_from_ready(d);
    if (current(s)) {
        json_t presences;
        ui_post_model(model);
        /* Friends and requests: resolved to {id, type, username, global_name, avatar} for the UI. */
        {
            json_t rels, rel, rv, ruser, users, u;
            json_iter_t rit, uit;
            sb_t *p = mem_alloc(sizeof *p);
            int first = 1;
            sb_add(p, "RELATIONSHIPS");
            sb_addn(p, "", 1);
            sb_add(p, "[");
            if (json_get(d, "relationships", &rels)) {
                json_iter(rels, &rit);
                while (json_next(&rit, NULL, &rel)) {
                    char id[24] = "";
                    int found = 0;
                    if (json_get(rel, "user", &ruser) && json_type(ruser) == JSON_OBJECT) {
                        found = 1;
                    } else if ((json_get(rel, "user_id", &rv) || json_get(rel, "id", &rv)) && json_get(d, "users", &users)) {
                        json_raw(rv, id, sizeof id);
                        json_iter(users, &uit);
                        while (!found && json_next(&uit, NULL, &u)) {
                            json_t uid;
                            char got[24];
                            if (json_get(u, "id", &uid)) {
                                json_raw(uid, got, sizeof got);
                                if (lstrcmpA(got, id) == 0) {
                                    ruser = u;
                                    found = 1;
                                }
                            }
                        }
                    }
                    if (!found)
                        continue;
                    if (!first)
                        sb_add(p, ",");
                    first = 0;
                    sb_add(p, "{\"type\":");
                    if (json_get(rel, "type", &rv))
                        sb_addn(p, rv.p, (size_t)(rv.end - rv.p));
                    else
                        sb_add(p, "0");
                    sb_add(p, ",\"ruser\":");
                    sb_addn(p, ruser.p, (size_t)(ruser.end - ruser.p));
                    sb_add(p, "}");
                }
            }
            sb_add(p, "]");
            ui_post(UI_EVENT, p);
        }
        /* Who sits in which voice channel, per server. */
        {
            json_t guilds, g, id, states;
            json_iter_t git;
            if (json_get(d, "guilds", &guilds)) {
                json_iter(guilds, &git);
                while (json_next(&git, NULL, &g))
                    if (json_get(g, "id", &id) && json_get(g, "voice_states", &states) && json_count(states)) {
                        sb_t *p = mem_alloc(sizeof *p);
                        sb_add(p, "VOICE_STATES");
                        sb_addn(p, "", 1);
                        sb_add(p, "{\"guild_id\":");
                        sb_addn(p, id.p, (size_t)(id.end - id.p));
                        sb_add(p, ",\"voice_states\":");
                        sb_addn(p, states.p, (size_t)(states.end - states.p));
                        sb_add(p, "}");
                        ui_post(UI_EVENT, p);
                    }
            }
        }
        /* Friends' statuses, applied once the model is in place. */
        if (json_get(d, "presences", &presences)) {
            sb_t *p = mem_alloc(sizeof *p);
            char line[64];
            wsprintfA(line, "%d presences", (int)json_count(presences));
            log_line("ready: ", line);
            sb_add(p, "PRESENCES");
            sb_addn(p, "", 1);
            sb_addn(p, presences.p, (size_t)(presences.end - presences.p));
            ui_post(UI_EVENT, p);
        }
    } else {
        model_free(model);
    }
}
/* GET /users/@me. Returns the HTTP status (0 if unreachable) and the account name. */
static DWORD check_token(const char *token, sb_t *name)
{
    http_resp_t resp = {0};
    json_t root, v;
    DWORD status = 0;

    if (http_request("GET", "/users/@me", token, NULL, 0, &resp)) {
        status = resp.status;
        if (status == 200 && json_parse(resp.body.data, resp.body.len, &root) && json_get(root, "username", &v))
            json_str(v, name);
    }
    http_resp_free(&resp);
    return status;
}

/* Waits between attempts: 1 s, 2 s, 5 s, 10 s, then every 30 s. */
static unsigned backoff(int attempt)
{
    static const unsigned steps[] = {1000, 2000, 5000, 10000, 30000};
    return steps[attempt < (int)ARRAYSIZE(steps) ? attempt : (int)ARRAYSIZE(steps) - 1];
}

static void post_reconnecting(session_t *s, unsigned delay)
{
    char text[64];

    wsprintfA(text, "Reconnecting in %u s\xE2\x80\xA6", (delay + 999) / 1000);
    log_line("session: ", text);
    if (current(s))
        ui_post(UI_RECONNECTING, ui_text(text));
}

static DWORD WINAPI session_main(LPVOID arg)
{
    session_t *s = arg;
    gw_events_t ev = {s, on_gw_status, on_ready, on_dispatch};
    sb_t name = {0}, text = {0};
    DWORD status;
    int attempt = 0, resume = 0, established, saved;

    /*
     * A session stopped in the middle of a network call ends on its own
     * later: wait for it here (not on the UI thread) before taking the
     * gateway, which both would share. Then reset it, unless we were
     * stopped too in the meantime.
     */
    if (s->prev) {
        WaitForSingleObject(s->prev, INFINITE);
        CloseHandle(s->prev);
        s->prev = NULL;
    }
    EnterCriticalSection(&g_session_lock);
    if (current(s))
        gw_reset();
    LeaveCriticalSection(&g_session_lock);
    if (!current(s))
        goto end;

    /* Check the token; while offline or rate limited, keep trying instead of giving up. */
    for (;;) {
        status = check_token(s->token.data, &name);
        if (status && status < 500 && status != 429)
            break;
        post_reconnecting(s, backoff(attempt));
        if (gw_wait(backoff(attempt++)))
            goto end;
    }
    if (!current(s))
        goto end; /* stopped (logged out, perhaps) while checking */
    if (status == 401) {
        cred_delete();
        ui_post(UI_LOGIN_FAILED, ui_text("Discord rejected this token."));
        goto end;
    }
    if (status != 200) {
        sb_add(&text, "Discord answered with HTTP ");
        sb_u64(&text, status);
        if (current(s))
            ui_post(UI_DISCONNECTED, copy(text.data, text.len));
        goto end;
    }
    /* Under the lock: once app_logout has stopped us, the token must not be saved again. */
    EnterCriticalSection(&g_session_lock);
    saved = !current(s) || cred_save(s->token.data, s->token.len);
    LeaveCriticalSection(&g_session_lock);
    if (!saved)
        log_line("session: ", "could not save the token");
    if (current(s))
        ui_post(UI_ACCOUNT, copy(name.data ? name.data : "", name.len));

    for (attempt = 0;;) {
        gw_result_t r = gw_run(s->token.data, &ev, resume, &established);
        unsigned delay;

        if (established)
            attempt = 0;
        if (r == GW_STOPPED || !current(s))
            break;
        if (r == GW_AUTH_FAILED) {
            cred_delete();
            if (current(s))
                ui_post(UI_LOGIN_FAILED, ui_text("Your session expired. Log in again."));
            break;
        }
        if (r == GW_FATAL) {
            sb_add(&text, s->last_status.len ? s->last_status.data : "Disconnected");
            if (current(s))
                ui_post(UI_DISCONNECTED, copy(text.data, text.len));
            break;
        }
        resume = r == GW_RESUME;
        delay = backoff(attempt++);
        post_reconnecting(s, delay);
        if (gw_wait(delay))
            break;
    }
end:
    log_line("session: ", "ended");
    sb_free(&text);
    sb_free(&name);
    sb_free(&s->last_status);
    sb_free(&s->token);
    mem_free(s);
    return 0;
}

/*
 * Silences the session and stops its gateway. A thread stuck in a network
 * call (a connection attempt cannot be interrupted) is kept in
 * g_session_thread: the next session waits for it before using the gateway.
 */
static void stop_session(void)
{
    if (!g_session_thread)
        return;
    EnterCriticalSection(&g_session_lock);
    InterlockedIncrement(&g_session_id);
    gw_stop();
    LeaveCriticalSection(&g_session_lock);
    if (WaitForSingleObject(g_session_thread, JOIN_TIMEOUT) == WAIT_OBJECT_0) {
        CloseHandle(g_session_thread);
        g_session_thread = NULL;
    }
}

static void start_session(void)
{
    session_t *s = mem_alloc(sizeof *s);

    stop_session();
    s->prev = g_session_thread;
    s->id = InterlockedIncrement(&g_session_id);
    sb_addn(&s->token, g_token.data, g_token.len);
    g_session_thread = CreateThread(NULL, 0, session_main, s, 0, NULL);
}

/* ---- Messages over REST ---- */

typedef struct {
    sb_t token;
    sb_t text;
    char channel[24];
    char before[24];   /* also: the message replied to, edited or deleted */
    int flag;          /* reply: mention the author */
    sb_t files;        /* files to upload: UTF-8 paths, each followed by a NUL */
    int nfiles;
    char sticker[24];  /* sticker sent with the message */
    char guild[24];
    char message[24];  /* components: the message they belong to */
    int message_flags;
} rest_job_t;

static rest_job_t *new_job(const char *channel_id)
{
    rest_job_t *j = mem_alloc(sizeof *j);

    sb_addn(&j->token, g_token.data, g_token.len);
    lstrcpynA(j->channel, channel_id, sizeof j->channel);
    return j;
}

static void free_job(rest_job_t *j)
{
    sb_free(&j->token);
    sb_free(&j->text);
    sb_free(&j->files);
    mem_free(j);
}

static DWORD WINAPI fetch_main(LPVOID arg)
{
    rest_job_t *j = arg;
    http_resp_t resp = {0};
    msg_batch_t *b = NULL;
    json_t root;
    char path[128];
    int kind = j->flag == 1 || j->flag == 3 ? BATCH_PINS : j->before[0] && j->flag != 2 ? BATCH_OLDER : BATCH_HISTORY;

    if (j->flag == 1)
        wsprintfA(path, "/channels/%s/pins", j->channel);
    else if (j->flag == 3) /* the inbox: recent mentions everywhere */
        lstrcpyA(path, "/users/@me/mentions?limit=25&roles=true&everyone=true");
    else if (j->flag == 2)
        wsprintfA(path, "/channels/%s/messages?limit=50&around=%s", j->channel, j->before);
    else if (j->before[0])
        wsprintfA(path, "/channels/%s/messages?limit=50&before=%s", j->channel, j->before);
    else
        wsprintfA(path, "/channels/%s/messages?limit=50", j->channel);
    if (http_request("GET", path, j->token.data, NULL, 0, &resp) && resp.status == 200 &&
        json_parse(resp.body.data, resp.body.len, &root))
        b = msg_batch_from_array(root, kind, j->channel, 50);
    if (!b) {
        b = mem_alloc(sizeof *b);
        b->kind = kind;
        lstrcpynA(b->channel_id, j->channel, sizeof b->channel_id);
        b->status = resp.status ? (int)resp.status : -1;
    }
    if (j->flag == 2) {
        lstrcpynA(b->around, j->before, sizeof b->around);
        b->has_more = 1;
    } else {
        lstrcpynA(b->before, j->before, sizeof b->before);
    }
    ui_post_batch(b);
    http_resp_free(&resp);
    free_job(j);
    return 0;
}

#define BOUNDARY "----SilicordFormBoundary7MA4YWxk"

/* Wraps the JSON payload and the files of job j into a multipart body, replacing *body. */
static int multipart(rest_job_t *j, sb_t *body)
{
    sb_t out = {0};
    const char *p = j->files.data;
    int ok = 1;

    sb_add(&out, "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"payload_json\"\r\n"
                 "Content-Type: application/json\r\n\r\n");
    sb_addn(&out, body->data, body->len);
    sb_add(&out, "\r\n");
    for (int i = 0; i < j->nfiles && ok; i++, p += lstrlenA(p) + 1) {
        wchar_t *wpath = utf8_to_wide(p, (size_t)lstrlenA(p));
        HANDLE f = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        const char *name = p;
        DWORD size, got = 0;

        mem_free(wpath);
        for (const char *q = p; *q; q++)
            if (*q == '\\' || *q == '/')
                name = q + 1;
        if (f == INVALID_HANDLE_VALUE) {
            ok = 0;
            break;
        }
        size = GetFileSize(f, NULL);
        sb_add(&out, "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"files[");
        sb_i64(&out, i);
        sb_add(&out, "]\"; filename=");
        sb_json_str(&out, name, (size_t)lstrlenA(name)); /* a quoted, escaped string */
        sb_add(&out, "\r\nContent-Type: application/octet-stream\r\n\r\n");
        if (size == INVALID_FILE_SIZE || size > (100u << 20)) {
            ok = 0;
        } else {
            sb_reserve(&out, size);
            ok = ReadFile(f, out.data + out.len, size, &got, NULL) && got == size;
            if (ok) {
                out.len += got;
                out.data[out.len] = 0;
            }
        }
        CloseHandle(f);
        sb_add(&out, "\r\n");
    }
    sb_add(&out, "--" BOUNDARY "--\r\n");
    sb_free(body);
    *body = out;
    return ok;
}

static DWORD WINAPI send_main(LPVOID arg)
{
    rest_job_t *j = arg;
    http_resp_t resp = {0};
    sb_t body = {0};
    json_t root;
    char path[96];
    FILETIME ft;
    ULARGE_INTEGER now;
    unsigned long long nonce;

    /* A snowflake for now: lets Discord drop duplicates if the request is retried. */
    GetSystemTimeAsFileTime(&ft);
    now.LowPart = ft.dwLowDateTime;
    now.HighPart = ft.dwHighDateTime;
    nonce = (now.QuadPart / 10000 - 11644473600000ull - 1420070400000ull) << 22;

    wsprintfA(path, "/channels/%s/messages", j->channel);
    sb_add(&body, "{\"content\":");
    sb_json_str(&body, j->text.data, j->text.len);
    sb_add(&body, ",\"nonce\":\"");
    sb_u64(&body, nonce);
    sb_add(&body, "\",\"tts\":false");
    if (j->sticker[0]) {
        sb_add(&body, ",\"sticker_ids\":[\"");
        sb_add(&body, j->sticker);
        sb_add(&body, "\"]");
    }
    if (j->before[0]) {
        sb_add(&body, ",\"message_reference\":{\"message_id\":\"");
        sb_add(&body, j->before);
        sb_add(&body, "\"},\"allowed_mentions\":{\"parse\":[\"users\",\"roles\",\"everyone\"],\"replied_user\":");
        sb_add(&body, j->flag ? "true}" : "false}");
    }
    if (j->nfiles) {
        const char *p = j->files.data;
        sb_add(&body, ",\"attachments\":[");
        for (int i = 0; i < j->nfiles; i++, p += lstrlenA(p) + 1) {
            const char *name = p;
            for (const char *q = p; *q; q++)
                if (*q == '\\' || *q == '/')
                    name = q + 1;
            if (i)
                sb_add(&body, ",");
            sb_add(&body, "{\"id\":");
            sb_i64(&body, i);
            sb_add(&body, ",\"filename\":");
            sb_json_str(&body, name, (size_t)lstrlenA(name));
            sb_add(&body, "}");
        }
        sb_add(&body, "]");
    }
    sb_add(&body, "}");
    if (j->nfiles && !multipart(j, &body)) {
        ui_post(UI_SEND_FAILED, ui_text("Could not read a file to upload"));
        sb_free(&body);
        free_job(j);
        return 0;
    }

    if (!http_request_type("POST", path, j->token.data,
                           j->nfiles ? "multipart/form-data; boundary=" BOUNDARY : NULL, body.data, body.len, &resp)) {
        ui_post(UI_SEND_FAILED, ui_text("Could not reach discord.com"));
    } else if (resp.status == 200 && json_parse(resp.body.data, resp.body.len, &root)) {
        msg_batch_t *b = msg_batch_one(root, BATCH_NEW);
        ui_post_batch(b);
    } else {
        char text[96];
        if (resp.status == 429)
            lstrcpyA(text, "You are sending messages too fast");
        else if (resp.status == 403)
            lstrcpyA(text, "You cannot send messages in this channel");
        else if (resp.status == 413)
            lstrcpyA(text, "Your files are too big");
        else
            wsprintfA(text, "Message not sent (HTTP %u)", resp.status);
        ui_post(UI_SEND_FAILED, ui_text(text));
    }
    sb_free(&body);
    http_resp_free(&resp);
    free_job(j);
    return 0;
}

/* ---- Called by the UI ---- */

void app_login_token(const char *token)
{
    stop_login();
    sb_free(&g_token);
    sb_add(&g_token, token);
    ui_show_loading("Logging in\xE2\x80\xA6");
    start_session();
}

void app_logout(void)
{
    app_voice_leave();
    stop_session();
    cred_delete();
    sb_free(&g_token);
    start_login();
}

void app_reconnect(void)
{
    start_session();
}

static DWORD WINAPI ack_main(LPVOID arg)
{
    rest_job_t *j = arg;
    http_resp_t resp = {0};
    char path[128];
    static const char body[] = "{\"token\":null}", manual[] = "{\"manual\":true,\"mention_count\":0}";

    wsprintfA(path, "/channels/%s/messages/%s/ack", j->channel, j->before);
    if (j->flag)
        http_request("POST", path, j->token.data, manual, sizeof manual - 1, &resp);
    else
        http_request("POST", path, j->token.data, body, sizeof body - 1, &resp);
    http_resp_free(&resp);
    free_job(j);
    return 0;
}

static DWORD WINAPI channel_main(LPVOID arg)
{
    rest_job_t *j = arg;
    http_resp_t resp = {0};
    char path[96];

    wsprintfA(path, "/channels/%s", j->channel);
    if (http_request("GET", path, j->token.data, NULL, 0, &resp) && resp.status == 200) {
        sb_t *p = mem_alloc(sizeof *p);
        sb_add(p, "CHANNEL_CREATE");
        sb_addn(p, "", 1);
        sb_addn(p, resp.body.data, resp.body.len);
        ui_post(UI_EVENT, p);
    }
    http_resp_free(&resp);
    free_job(j);
    return 0;
}

static DWORD WINAPI profile_main(LPVOID arg)
{
    rest_job_t *j = arg;
    http_resp_t resp = {0};
    profile_t *p = mem_alloc(sizeof *p);
    json_t root;
    char path[160];

    /* j->channel holds the user id, j->before the server. */
    wsprintfA(path, "/users/%s/profile?with_mutual_guilds=true&with_mutual_friends=true&with_mutual_friends_count=true%s%s",
              j->channel, j->before[0] ? "&guild_id=" : "", j->before);
    if (!(http_request("GET", path, j->token.data, NULL, 0, &resp) && resp.status == 200 &&
          json_parse(resp.body.data, resp.body.len, &root) && profile_parse(root, j->before, p))) {
        profile_free(p);
        lstrcpynA(p->id, j->channel, sizeof p->id);
        lstrcpynA(p->guild_id, j->before, sizeof p->guild_id);
    }
    ui_post_profile(p);
    http_resp_free(&resp);
    free_job(j);
    return 0;
}

void app_fetch_profile(const char *user_id, const char *guild_id)
{
    rest_job_t *j = new_job(user_id);

    lstrcpynA(j->before, guild_id ? guild_id : "", sizeof j->before);
    CloseHandle(CreateThread(NULL, 0, profile_main, j, 0, NULL));
}

static DWORD WINAPI dm_main(LPVOID arg)
{
    rest_job_t *j = arg;
    http_resp_t resp = {0};
    sb_t body = {0};
    json_t root, id;
    char channel[24] = "";

    /* Returns the existing DM if there is one. */
    sb_add(&body, "{\"recipients\":[\"");
    sb_add(&body, j->channel);
    sb_add(&body, "\"]}");
    if (http_request("POST", "/users/@me/channels", j->token.data, body.data, body.len, &resp) &&
        resp.status == 200 && json_parse(resp.body.data, resp.body.len, &root) && json_get(root, "id", &id)) {
        sb_t *p = mem_alloc(sizeof *p);
        json_raw(id, channel, sizeof channel);
        sb_add(p, "CHANNEL_CREATE");
        sb_addn(p, "", 1);
        sb_addn(p, resp.body.data, resp.body.len);
        ui_post(UI_EVENT, p);
        ui_post(UI_DM_OPENED, ui_text(channel));
    } else {
        ui_post(UI_SEND_FAILED, ui_text("Could not open the conversation"));
    }
    sb_free(&body);
    http_resp_free(&resp);
    if (channel[0] && j->text.len) {
        lstrcpynA(j->channel, channel, sizeof j->channel);
        return send_main(j); /* frees the job */
    }
    free_job(j);
    return 0;
}

void app_open_dm(const char *user_id, const char *text)
{
    rest_job_t *j = new_job(user_id);

    sb_add(&j->text, text ? text : "");
    CloseHandle(CreateThread(NULL, 0, dm_main, j, 0, NULL));
}

/* ---- Display name fonts ---- */

#define FONTS_HOST L"raw.githubusercontent.com"
#define FONTS_REPO "/google/fonts/23e54b51ddffbc7713c583748e3bd86f62b1fa4a/"

typedef struct {
    int id;
    char file[96];
} font_job_t;

static int font_cache_path(int id, wchar_t *out, int size)
{
    wchar_t dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);

    if (!n || n > MAX_PATH - 40)
        return 0;
    lstrcatW(dir, L"\\Silicord");
    CreateDirectoryW(dir, NULL);
    lstrcatW(dir, L"\\fonts");
    CreateDirectoryW(dir, NULL);
    if (size < MAX_PATH)
        return 0;
    wsprintfW(out, L"%s\\font-%d.ttf", dir, id);
    return 1;
}

static int is_font(const sb_t *b)
{
    const unsigned char *p = (const unsigned char *)b->data;

    return b->len > 1024 && ((p[0] == 0 && p[1] == 1 && p[2] == 0 && p[3] == 0) || (p[0] == 'O' && p[1] == 'T' && p[2] == 'T' && p[3] == 'O') ||
                             (p[0] == 't' && p[1] == 'r' && p[2] == 'u' && p[3] == 'e'));
}

static DWORD WINAPI font_main(LPVOID arg)
{
    font_job_t *j = arg;
    sb_t *data = mem_alloc(sizeof *data);
    wchar_t path[MAX_PATH];
    int cached = font_cache_path(j->id, path, MAX_PATH);
    HANDLE f;

    if (cached && (f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL)) != INVALID_HANDLE_VALUE) {
        DWORD size = GetFileSize(f, NULL), got = 0;
        if (size != INVALID_FILE_SIZE && size < (8u << 20)) {
            sb_reserve(data, size);
            if (ReadFile(f, data->data, size, &got, NULL))
                data->len = got;
        }
        CloseHandle(f);
    }
    if (!is_font(data)) {
        http_resp_t resp = {0};
        sb_t url = {0};
        sb_clear(data);
        sb_add(&url, FONTS_REPO);
        sb_add(&url, j->file);
        if (http_get(FONTS_HOST, url.data, &resp) && resp.status == 200 && is_font(&resp.body)) {
            *data = resp.body;
            resp.body = (sb_t){0};
            if (cached && (f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL)) != INVALID_HANDLE_VALUE) {
                DWORD put;
                WriteFile(f, data->data, (DWORD)data->len, &put, NULL);
                CloseHandle(f);
            }
        }
        http_resp_free(&resp);
        sb_free(&url);
    }
    if (!is_font(data)) {
        sb_free(data);
        mem_free(data);
        data = NULL;
    }
    ui_post_font(j->id, data);
    mem_free(j);
    return 0;
}

void app_fetch_font(int id, const char *file)
{
    font_job_t *j = mem_alloc(sizeof *j);

    j->id = id;
    lstrcpynA(j->file, file, sizeof j->file);
    CloseHandle(CreateThread(NULL, 0, font_main, j, 0, NULL));
}

static DWORD WINAPI react_main(LPVOID arg)
{
    rest_job_t *j = arg;
    http_resp_t resp = {0};

    /* j->text holds the whole path, j->before the method. */
    http_request(j->before, j->text.data, j->token.data, NULL, 0, &resp);
    if (resp.status != 204 && resp.status != 200)
        ui_post(UI_SEND_FAILED, ui_text(resp.status == 403 ? "You cannot react here" : "Reaction not saved"));
    http_resp_free(&resp);
    free_job(j);
    return 0;
}

void app_react(const char *channel_id, const char *message_id, const msg_reaction_t *r, int add)
{
    rest_job_t *j = new_job(channel_id);

    sb_add(&j->text, "/channels/");
    sb_add(&j->text, channel_id);
    sb_add(&j->text, "/messages/");
    sb_add(&j->text, message_id);
    sb_add(&j->text, "/reactions/");
    msg_reaction_path(r, &j->text);
    sb_add(&j->text, "/@me?location=Message&type=0");
    lstrcpyA(j->before, add ? "PUT" : "DELETE");
    CloseHandle(CreateThread(NULL, 0, react_main, j, 0, NULL));
}

static DWORD WINAPI reactors_main(LPVOID arg)
{
    rest_job_t *j = arg;
    http_resp_t resp = {0};
    sb_t *p = mem_alloc(sizeof *p);

    /* j->text: the path; j->files: the key the UI gave. */
    sb_add(p, "REACTORS");
    sb_addn(p, "", 1);
    sb_addn(p, j->files.data ? j->files.data : "", j->files.len);
    sb_addn(p, "", 1);
    if (http_request("GET", j->text.data, j->token.data, NULL, 0, &resp) && resp.status == 200)
        sb_addn(p, resp.body.data, resp.body.len);
    else
        sb_add(p, "[]");
    ui_post(UI_EVENT, p);
    http_resp_free(&resp);
    free_job(j);
    return 0;
}

void app_fetch_reactors(const char *channel_id, const char *message_id, const msg_reaction_t *r, const char *key)
{
    rest_job_t *j = new_job(channel_id);

    sb_add(&j->text, "/channels/");
    sb_add(&j->text, channel_id);
    sb_add(&j->text, "/messages/");
    sb_add(&j->text, message_id);
    sb_add(&j->text, "/reactions/");
    msg_reaction_path(r, &j->text);
    sb_add(&j->text, "?limit=3&type=0");
    sb_add(&j->files, key);
    CloseHandle(CreateThread(NULL, 0, reactors_main, j, 0, NULL));
}

static DWORD WINAPI relation_main(LPVOID arg)
{
    rest_job_t *j = arg;
    http_resp_t resp = {0};
    char path[96];

    /* j->channel: user id (or empty to add by username in j->text); j->before: method. */
    if (j->channel[0]) {
        wsprintfA(path, "/users/@me/relationships/%s", j->channel);
        http_request(j->before, path, j->token.data, lstrcmpA(j->before, "PUT") == 0 ? "{}" : NULL,
                     lstrcmpA(j->before, "PUT") == 0 ? 2 : 0, &resp);
    } else {
        sb_t body = {0};
        sb_add(&body, "{\"username\":");
        sb_json_str(&body, j->text.data, j->text.len);
        sb_add(&body, ",\"discriminator\":null}");
        http_request("POST", "/users/@me/relationships", j->token.data, body.data, body.len, &resp);
        sb_free(&body);
        ui_post(UI_FRIEND_RESULT, ui_text(resp.status == 204 || resp.status == 200
                                              ? "Success! Your friend request was sent."
                                              : "Hm, didn't work. Double check that the username is correct."));
    }
    http_resp_free(&resp);
    free_job(j);
    return 0;
}

void app_relationship(const char *user_id, const char *method)
{
    rest_job_t *j = new_job(user_id);

    lstrcpynA(j->before, method, sizeof j->before);
    CloseHandle(CreateThread(NULL, 0, relation_main, j, 0, NULL));
}

void app_add_friend(const char *username)
{
    rest_job_t *j = new_job("");

    sb_add(&j->text, username);
    CloseHandle(CreateThread(NULL, 0, relation_main, j, 0, NULL));
}

static void rest(const char *method, const char *path, const sb_t *body);

void app_set_status(const char *status, const char *custom)
{
    sb_t msg = {0};

    /* Op 3, Presence Update: this session's status, the custom status as a type 4 activity. */
    sb_add(&msg, "{\"op\":3,\"d\":{\"status\":\"");
    sb_add(&msg, status);
    sb_add(&msg, "\",\"since\":0,\"activities\":[");
    if (custom && custom[0]) {
        sb_add(&msg, "{\"type\":4,\"name\":\"Custom Status\",\"id\":\"custom\",\"state\":");
        sb_json_str(&msg, custom, (size_t)lstrlenA(custom));
        sb_add(&msg, "}");
    }
    sb_add(&msg, "],\"afk\":false}}");
    gw_send(&msg);
    sb_free(&msg);
}

void app_call_ring(const char *channel_id, const char *stop_for)
{
    sb_t path = {0}, body = {0};

    sb_add(&path, "/channels/");
    sb_add(&path, channel_id);
    if (stop_for) {
        sb_add(&path, "/call/stop-ringing");
        sb_add(&body, "{\"recipients\":[\"");
        sb_add(&body, stop_for);
        sb_add(&body, "\"]}");
    } else {
        sb_add(&path, "/call/ring");
        sb_add(&body, "{\"recipients\":null}");
    }
    rest("POST", path.data, &body);
    sb_free(&path);
    sb_free(&body);
}

void app_user_settings(const char *fields)
{
    sb_t body = {0};

    sb_add(&body, "{");
    sb_add(&body, fields);
    sb_add(&body, "}");
    rest("PATCH", "/users/@me/settings", &body);
    sb_free(&body);
}

void app_subscribe_range(const char *guild_id, const char *channel_id, int start)
{
    sb_t msg = {0};

    sb_add(&msg, "{\"op\":14,\"d\":{\"guild_id\":\"");
    sb_add(&msg, guild_id);
    sb_add(&msg, "\",\"typing\":true,\"threads\":true,\"activities\":true,\"members\":[],\"channels\":{\"");
    sb_add(&msg, channel_id);
    sb_add(&msg, "\":[[0,99]");
    if (start > 0) {
        char r[48];
        wsprintfA(r, ",[%d,%d]", start, start + 99);
        sb_add(&msg, r);
    }
    sb_add(&msg, "]}}}");
    gw_send(&msg);
    sb_free(&msg);
}

void app_subscribe(const char *guild_id, const char *channel_id)
{
    app_subscribe_range(guild_id, channel_id, 0);
}

void app_request_members(const char *guild_id, const char *const *user_ids, int n)
{
    sb_t msg = {0};

    if (n <= 0)
        return;
    /* Op 8, Request Guild Members: answered by GUILD_MEMBERS_CHUNK. */
    sb_add(&msg, "{\"op\":8,\"d\":{\"guild_id\":\"");
    sb_add(&msg, guild_id);
    sb_add(&msg, "\",\"user_ids\":[");
    for (int i = 0; i < n && i < 100; i++) {
        sb_add(&msg, i ? ",\"" : "\"");
        sb_add(&msg, user_ids[i]);
        sb_add(&msg, "\"");
    }
    sb_add(&msg, "],\"presences\":false}}");
    gw_send(&msg);
    sb_free(&msg);
}


void app_vote(const char *channel_id, const char *message_id, const int *answers, int n)
{
    sb_t body = {0};
    char path[128];

    wsprintfA(path, "/channels/%s/polls/%s/answers/@me", channel_id, message_id);
    sb_add(&body, "{\"answer_ids\":[");
    for (int i = 0; i < n; i++) {
        char a[24];
        wsprintfA(a, "%s\"%d\"", i ? "," : "", answers[i]);
        sb_add(&body, a);
    }
    sb_add(&body, "]}");
    rest("PUT", path, &body);
    sb_free(&body);
}

void app_fetch_channel(const char *channel_id)
{
    CloseHandle(CreateThread(NULL, 0, channel_main, new_job(channel_id), 0, NULL));
}

static DWORD WINAPI settings_main(LPVOID arg)
{
    rest_job_t *j = arg;
    http_resp_t resp = {0};

    /* j->before: method, j->text: "path\0body". */
    {
        const char *path = j->text.data, *body = path + lstrlenA(path) + 1;
        size_t blen = j->text.len - (size_t)(body - path);
        http_request(j->before, path, j->token.data, blen ? body : NULL, blen, &resp);
    }
    if (resp.status >= 400)
        ui_post(UI_SEND_FAILED, ui_text("That change was not saved"));
    http_resp_free(&resp);
    free_job(j);
    return 0;
}

static void rest(const char *method, const char *path, const sb_t *body)
{
    rest_job_t *j = new_job("");

    lstrcpynA(j->before, method, sizeof j->before);
    sb_add(&j->text, path);
    sb_addn(&j->text, "", 1);
    if (body && body->len)
        sb_addn(&j->text, body->data, body->len);
    CloseHandle(CreateThread(NULL, 0, settings_main, j, 0, NULL));
}

void app_notify_settings(const char *guild_id, const char *channel_id, const char *fields)
{
    sb_t body = {0};
    char path[96];

    wsprintfA(path, "/users/@me/guilds/%s/settings", guild_id ? guild_id : "@me");
    if (channel_id) {
        sb_add(&body, "{\"channel_overrides\":{\"");
        sb_add(&body, channel_id);
        sb_add(&body, "\":{");
        sb_add(&body, fields);
        sb_add(&body, "}}}");
    } else {
        sb_add(&body, "{");
        sb_add(&body, fields);
        sb_add(&body, "}");
    }
    rest("PATCH", path, &body);
    sb_free(&body);
}

void app_mute(const char *guild_id, const char *channel_id, int muted, int minutes)
{
    char fields[160];

    if (!muted) {
        lstrcpyA(fields, "\"muted\":false");
    } else if (!minutes) {
        lstrcpyA(fields, "\"muted\":true,\"mute_config\":{\"selected_time_window\":-1,\"end_time\":null}");
    } else {
        /* Discord keeps the end time and lifts the mute itself. */
        FILETIME ft;
        ULARGE_INTEGER t;
        SYSTEMTIME st;
        GetSystemTimeAsFileTime(&ft);
        t.LowPart = ft.dwLowDateTime;
        t.HighPart = ft.dwHighDateTime;
        t.QuadPart += (unsigned long long)minutes * 60 * 10000000;
        ft.dwLowDateTime = t.LowPart;
        ft.dwHighDateTime = t.HighPart;
        FileTimeToSystemTime(&ft, &st);
        wsprintfA(fields,
                  "\"muted\":true,\"mute_config\":{\"selected_time_window\":%d,"
                  "\"end_time\":\"%04u-%02u-%02uT%02u:%02u:%02u.000Z\"}",
                  minutes * 60, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    }
    app_notify_settings(guild_id, channel_id, fields);
}

void app_ack_bulk(const char *pairs, int n)
{
    sb_t body = {0};
    const char *p = pairs;

    sb_add(&body, "{\"read_states\":[");
    for (int i = 0; i < n; i++) {
        const char *channel = p, *message = p + lstrlenA(p) + 1;
        p = message + lstrlenA(message) + 1;
        sb_add(&body, i ? ",{\"channel_id\":\"" : "{\"channel_id\":\"");
        sb_add(&body, channel);
        sb_add(&body, "\",\"message_id\":\"");
        sb_add(&body, message);
        sb_add(&body, "\",\"read_state_type\":0}");
    }
    sb_add(&body, "]}");
    rest("POST", "/read-states/ack-bulk", &body);
    sb_free(&body);
}

void app_leave_guild(const char *guild_id)
{
    sb_t body = {0};
    char path[80];

    wsprintfA(path, "/users/@me/guilds/%s", guild_id);
    sb_add(&body, "{\"lurking\":false}");
    rest("DELETE", path, &body);
    sb_free(&body);
}

void app_ack(const char *channel_id, const char *message_id)
{
    rest_job_t *j = new_job(channel_id);

    lstrcpynA(j->before, message_id, sizeof j->before);
    CloseHandle(CreateThread(NULL, 0, ack_main, j, 0, NULL));
}

static DWORD WINAPI thread_main(LPVOID arg)
{
    rest_job_t *j = arg;
    http_resp_t resp = {0};
    char path[128];

    /* j->before: the message, or empty for a forum post; j->text: the JSON body. */
    if (j->before[0])
        wsprintfA(path, "/channels/%s/messages/%s/threads", j->channel, j->before);
    else
        wsprintfA(path, "/channels/%s/threads", j->channel);
    if (http_request("POST", path, j->token.data, j->text.data, j->text.len, &resp) &&
        (resp.status == 200 || resp.status == 201)) {
        sb_t *p = mem_alloc(sizeof *p);
        sb_add(p, "THREAD_OURS");
        sb_addn(p, "", 1);
        sb_addn(p, resp.body.data, resp.body.len);
        ui_post(UI_EVENT, p);
    } else {
        char text[64];
        wsprintfA(text, "Could not create the thread (HTTP %u)", resp.status);
        ui_post(UI_SEND_FAILED, ui_text(text));
    }
    http_resp_free(&resp);
    free_job(j);
    return 0;
}

void app_create_thread(const char *channel_id, const char *message_id, const char *name, const char *content)
{
    rest_job_t *j = new_job(channel_id);

    lstrcpynA(j->before, message_id ? message_id : "", sizeof j->before);
    sb_add(&j->text, "{\"name\":");
    sb_json_str(&j->text, name, (size_t)lstrlenA(name));
    sb_add(&j->text, ",\"auto_archive_duration\":4320");
    if (!message_id) {
        sb_add(&j->text, ",\"message\":{\"content\":");
        sb_json_str(&j->text, content ? content : "", content ? (size_t)lstrlenA(content) : 0);
        sb_add(&j->text, "}");
    }
    sb_add(&j->text, "}");
    CloseHandle(CreateThread(NULL, 0, thread_main, j, 0, NULL));
}

void app_ack_manual(const char *channel_id, const char *message_id)
{
    rest_job_t *j = new_job(channel_id);

    lstrcpynA(j->before, message_id, sizeof j->before);
    j->flag = 1;
    CloseHandle(CreateThread(NULL, 0, ack_main, j, 0, NULL));
}

void app_pin(const char *channel_id, const char *message_id, int pin)
{
    char path[128];

    wsprintfA(path, "/channels/%s/messages/pins/%s", channel_id, message_id);
    rest(pin ? "PUT" : "DELETE", path, NULL);
}

void app_open_channel(const char *channel_id)
{
    EnterCriticalSection(&g_open_lock);
    lstrcpynA(g_open_channel, channel_id ? channel_id : "", sizeof g_open_channel);
    LeaveCriticalSection(&g_open_lock);
}

void app_fetch_messages(const char *channel_id, const char *before)
{
    rest_job_t *j = new_job(channel_id);

    if (before)
        lstrcpynA(j->before, before, sizeof j->before);
    CloseHandle(CreateThread(NULL, 0, fetch_main, j, 0, NULL));
}

void app_fetch_around(const char *channel_id, const char *message_id)
{
    rest_job_t *j = new_job(channel_id);

    lstrcpynA(j->before, message_id, sizeof j->before);
    j->flag = 2;
    CloseHandle(CreateThread(NULL, 0, fetch_main, j, 0, NULL));
}

static DWORD WINAPI search_main(LPVOID arg)
{
    rest_job_t *j = arg;
    http_resp_t resp = {0};
    msg_batch_t *b = NULL;
    json_t root;
    sb_t path = {0};

    /* j->channel: server id, or the DM channel when j->flag; j->text: the query. */
    sb_add(&path, j->flag ? "/channels/" : "/guilds/");
    sb_add(&path, j->channel);
    sb_add(&path, "/messages/search?");
    if (j->text.len)
        sb_add(&path, "content=");
    for (size_t i = 0; i < j->text.len; i++) {
        unsigned char c = (unsigned char)j->text.data[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            sb_addn(&path, (const char *)&c, 1);
        } else {
            char e[4];
            wsprintfA(e, "%%%02X", c);
            sb_add(&path, e);
        }
    }
    /* The filters, already encoded ("&author_id=..."); j->files is free for them in a search. */
    if (j->files.len)
        sb_addn(&path, j->files.data + !j->text.len, j->files.len - !j->text.len);
    /* Discord answers 202 while it indexes: give it a moment, like the client does. */
    for (int tries = 0; tries < 3; tries++) {
        http_resp_free(&resp);
        if (!http_request("GET", path.data, j->token.data, NULL, 0, &resp) || resp.status != 202)
            break;
        Sleep(1500);
    }
    if (resp.status == 200 && json_parse(resp.body.data, resp.body.len, &root))
        b = msg_batch_search(root);
    if (!b) {
        b = mem_alloc(sizeof *b);
        b->kind = BATCH_SEARCH;
        b->status = resp.status ? (int)resp.status : -1;
    }
    lstrcpynA(b->channel_id, j->channel, sizeof b->channel_id);
    ui_post_batch(b);
    http_resp_free(&resp);
    sb_free(&path);
    free_job(j);
    return 0;
}

void app_search(const char *guild_id, const char *dm_channel_id, const char *query, const char *params)
{
    rest_job_t *j = new_job(guild_id ? guild_id : dm_channel_id);

    j->flag = guild_id == NULL;
    sb_add(&j->text, query);
    sb_add(&j->files, params ? params : "");
    CloseHandle(CreateThread(NULL, 0, search_main, j, 0, NULL));
}

static DWORD WINAPI forum_main(LPVOID arg)
{
    rest_job_t *j = arg;
    http_resp_t resp = {0};
    char path[160];
    sb_t *p = mem_alloc(sizeof *p);

    wsprintfA(path, "/channels/%s/threads/search?archived=false&sort_by=last_message_time&sort_order=desc&limit=25&offset=0",
              j->channel);
    http_request("GET", path, j->token.data, NULL, 0, &resp);
    sb_add(p, j->channel);
    sb_addn(p, "", 1);
    if (resp.status == 200)
        sb_addn(p, resp.body.data, resp.body.len);
    ui_post(UI_FORUM, p);
    http_resp_free(&resp);
    free_job(j);
    return 0;
}

/* A 429's "retry_after" (seconds, maybe fractional) in ms, capped at 5 s. */
static DWORD retry_after_ms(const http_resp_t *resp)
{
    json_t root, v;
    char raw[24];
    DWORD ms = 0, scale = 1000;

    if (!json_parse(resp->body.data, resp->body.len, &root) || !json_get(root, "retry_after", &v))
        return 1000;
    json_raw(v, raw, sizeof raw);
    for (const char *c = raw; *c; c++) {
        if (*c == '.') {
            scale = 100;
            continue;
        }
        if (*c < '0' || *c > '9' || !scale)
            break;
        if (scale == 1000) {
            ms = ms * 10 + (DWORD)(*c - '0') * 1000;
        } else {
            ms += (DWORD)(*c - '0') * scale;
            scale /= 10;
        }
    }
    return ms < 100 ? 100 : ms > 5000 ? 5000 : ms;
}

static DWORD WINAPI gifs_main(LPVOID arg)
{
    rest_job_t *j = arg;
    http_resp_t resp = {0};
    sb_t path = {0};
    sb_t *p = mem_alloc(sizeof *p);
    json_t root, gifs;

    if (j->text.len) {
        sb_add(&path, "/gifs/search?media_format=tinygif&locale=en-US&limit=40&provider=tenor&q=");
        for (size_t i = 0; i < j->text.len; i++) {
            unsigned char c = (unsigned char)j->text.data[i];
            char e[4];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                sb_addn(&path, (const char *)&c, 1);
            } else {
                wsprintfA(e, "%%%02X", c);
                sb_add(&path, e);
            }
        }
    } else {
        sb_add(&path, "/gifs/trending-gifs?media_format=tinygif&provider=tenor&locale=en-US&limit=40");
    }
    /* Rate limited: wait what Discord asks, then try again. */
    for (int tries = 0; tries < 3; tries++) {
        http_resp_free(&resp);
        if (!http_request("GET", path.data, j->token.data, NULL, 0, &resp) || resp.status != 429)
            break;
        Sleep(retry_after_ms(&resp));
    }
    sb_addn(p, j->text.data ? j->text.data : "", j->text.len);
    sb_addn(p, "", 1);
    if (g_debug && resp.body.len) {
        char head[301];
        lstrcpynA(head, resp.body.data, resp.body.len < 300 ? (int)resp.body.len + 1 : 301);
        log_line("[gifs] ", head);
    }
    if (resp.status == 200 && json_parse(resp.body.data, resp.body.len, &root)) {
        /* Search answers an array; some versions wrap it as {gifs: [...]}. */
        if (json_type(root) == JSON_OBJECT && json_get(root, "gifs", &gifs))
            sb_addn(p, gifs.p, (size_t)(gifs.end - gifs.p));
        else
            sb_addn(p, resp.body.data, resp.body.len);
    } else {
        char e[16];
        wsprintfA(e, "!%d", resp.status); /* failure marker, with the status for the logs */
        sb_add(p, e);
        log_line("[gifs] failed: ", e + 1);
    }
    ui_post(UI_GIFS, p);
    http_resp_free(&resp);
    sb_free(&path);
    free_job(j);
    return 0;
}

static DWORD WINAPI commands_main(LPVOID arg)
{
    rest_job_t *j = arg;
    http_resp_t resp = {0};
    char path[96];
    sb_t *p = mem_alloc(sizeof *p);

    if (j->guild[0])
        wsprintfA(path, "/guilds/%s/application-command-index", j->guild);
    else
        wsprintfA(path, "/channels/%s/application-command-index", j->channel);
    for (int tries = 0; tries < 3; tries++) {
        http_resp_free(&resp);
        if (!http_request("GET", path, j->token.data, NULL, 0, &resp) || resp.status != 429)
            break;
        Sleep(retry_after_ms(&resp));
    }
    sb_add(p, j->guild[0] ? j->guild : j->channel);
    sb_addn(p, "", 1);
    if (resp.status == 200)
        sb_addn(p, resp.body.data, resp.body.len);
    ui_post(UI_COMMANDS, p);
    http_resp_free(&resp);
    free_job(j);
    return 0;
}

void app_fetch_commands(const char *guild_id, const char *channel_id)
{
    rest_job_t *j = new_job(channel_id ? channel_id : "");

    lstrcpynA(j->guild, guild_id ? guild_id : "", sizeof j->guild);
    CloseHandle(CreateThread(NULL, 0, commands_main, j, 0, NULL));
}

static DWORD WINAPI command_main(LPVOID arg)
{
    rest_job_t *j = arg;
    http_resp_t resp = {0};
    sb_t body = {0};
    char session[80], nonce[24];
    FILETIME ft;
    ULARGE_INTEGER now;

    GetSystemTimeAsFileTime(&ft);
    now.LowPart = ft.dwLowDateTime;
    now.HighPart = ft.dwHighDateTime;
    wsprintfA(nonce, "%I64u", (now.QuadPart / 10000 - 11644473600000ull - 1420070400000ull) << 22);
    gw_session_id(session, sizeof session);
    sb_add(&body, "{\"type\":");
    sb_i64(&body, j->flag ? j->flag : 2);
    if (j->message[0]) {
        sb_add(&body, ",\"message_id\":\"");
        sb_add(&body, j->message);
        sb_add(&body, "\",\"message_flags\":");
        sb_i64(&body, j->message_flags);
    }
    sb_add(&body, ",\"application_id\":\"");
    sb_add(&body, j->before);
    if (j->guild[0]) {
        sb_add(&body, "\",\"guild_id\":\"");
        sb_add(&body, j->guild);
    }
    sb_add(&body, "\",\"channel_id\":\"");
    sb_add(&body, j->channel);
    sb_add(&body, "\",\"session_id\":\"");
    sb_add(&body, session);
    sb_add(&body, "\",\"data\":");
    sb_addn(&body, j->text.data, j->text.len);
    sb_add(&body, ",\"nonce\":\"");
    sb_add(&body, nonce);
    sb_add(&body, "\"}");
    /* The answer (or the bot's "thinking...") comes over the gateway like any message. */
    if (!http_request("POST", "/interactions", j->token.data, body.data, body.len, &resp)) {
        ui_post(UI_SEND_FAILED, ui_text("Could not reach discord.com"));
    } else if (resp.status != 204 && resp.status != 200) {
        char text[64];
        wsprintfA(text, j->flag == 3 ? "The interaction failed (HTTP %u)" : "The command failed (HTTP %u)", resp.status);
        ui_post(UI_SEND_FAILED, ui_text(text));
    }
    sb_free(&body);
    http_resp_free(&resp);
    free_job(j);
    return 0;
}

void app_press_component(const char *guild_id, const char *channel_id, const char *message_id, const char *application_id,
                         int message_flags, int component_type, const char *custom_id, const char *value)
{
    rest_job_t *j = new_job(channel_id);

    lstrcpynA(j->guild, guild_id ? guild_id : "", sizeof j->guild);
    lstrcpynA(j->before, application_id, sizeof j->before);
    lstrcpynA(j->message, message_id, sizeof j->message);
    j->flag = 3;
    j->message_flags = message_flags;
    sb_add(&j->text, "{\"component_type\":");
    sb_i64(&j->text, component_type);
    sb_add(&j->text, ",\"custom_id\":");
    sb_json_str(&j->text, custom_id, (size_t)lstrlenA(custom_id));
    if (value) {
        sb_add(&j->text, ",\"values\":[");
        sb_json_str(&j->text, value, (size_t)lstrlenA(value));
        sb_add(&j->text, "]");
    }
    sb_add(&j->text, "}");
    CloseHandle(CreateThread(NULL, 0, command_main, j, 0, NULL));
}

void app_run_command(const char *guild_id, const char *channel_id, const char *application_id, const char *data)
{
    rest_job_t *j = new_job(channel_id);

    lstrcpynA(j->guild, guild_id ? guild_id : "", sizeof j->guild);
    lstrcpynA(j->before, application_id, sizeof j->before);
    sb_add(&j->text, data);
    CloseHandle(CreateThread(NULL, 0, command_main, j, 0, NULL));
}

void app_fetch_gifs(const char *query)
{
    rest_job_t *j = new_job("");

    sb_add(&j->text, query ? query : "");
    CloseHandle(CreateThread(NULL, 0, gifs_main, j, 0, NULL));
}

void app_fetch_forum(const char *channel_id)
{
    CloseHandle(CreateThread(NULL, 0, forum_main, new_job(channel_id), 0, NULL));
}

void app_fetch_pins(const char *channel_id)
{
    rest_job_t *j = new_job(channel_id);

    j->flag = 1;
    CloseHandle(CreateThread(NULL, 0, fetch_main, j, 0, NULL));
}

void app_fetch_mentions(void)
{
    rest_job_t *j = new_job(INBOX_CHANNEL);

    j->flag = 3;
    CloseHandle(CreateThread(NULL, 0, fetch_main, j, 0, NULL));
}

void app_send_message(const char *channel_id, const char *text)
{
    rest_job_t *j = new_job(channel_id);

    sb_add(&j->text, text);
    CloseHandle(CreateThread(NULL, 0, send_main, j, 0, NULL));
}

void app_log(const char *text)
{
    log_line("", text);
}

void app_send_files(const char *channel_id, const char *text, const char *reply_id, int mention, const char *paths, int n)
{
    rest_job_t *j = new_job(channel_id);
    const char *p = paths;

    sb_add(&j->text, text ? text : "");
    lstrcpynA(j->before, reply_id ? reply_id : "", sizeof j->before);
    j->flag = mention;
    for (int i = 0; i < n; i++, p += lstrlenA(p) + 1)
        sb_addn(&j->files, p, (size_t)lstrlenA(p) + 1);
    j->nfiles = n;
    CloseHandle(CreateThread(NULL, 0, send_main, j, 0, NULL));
}

void app_forward(const char *to_channel, const char *channel_id, const char *guild_id, const char *message_id)
{
    sb_t body = {0};
    char path[96];

    wsprintfA(path, "/channels/%s/messages", to_channel);
    sb_add(&body, "{\"content\":\"\",\"message_reference\":{\"type\":1,\"message_id\":\"");
    sb_add(&body, message_id);
    sb_add(&body, "\",\"channel_id\":\"");
    sb_add(&body, channel_id);
    if (guild_id) {
        sb_add(&body, "\",\"guild_id\":\"");
        sb_add(&body, guild_id);
    }
    sb_add(&body, "\"}}");
    rest("POST", path, &body);
    sb_free(&body);
}

void app_send_sticker(const char *channel_id, const char *sticker_id, const char *reply_id, int mention)
{
    rest_job_t *j = new_job(channel_id);

    lstrcpynA(j->sticker, sticker_id, sizeof j->sticker);
    lstrcpynA(j->before, reply_id ? reply_id : "", sizeof j->before);
    j->flag = mention;
    CloseHandle(CreateThread(NULL, 0, send_main, j, 0, NULL));
}

void app_send_reply(const char *channel_id, const char *text, const char *reply_id, int mention)
{
    rest_job_t *j = new_job(channel_id);

    sb_add(&j->text, text);
    lstrcpynA(j->before, reply_id, sizeof j->before);
    j->flag = mention;
    CloseHandle(CreateThread(NULL, 0, send_main, j, 0, NULL));
}

static DWORD WINAPI edit_main(LPVOID arg)
{
    rest_job_t *j = arg;
    http_resp_t resp = {0};
    sb_t body = {0};
    char path[96];

    wsprintfA(path, "/channels/%s/messages/%s", j->channel, j->before);
    if (j->flag) {
        http_request("DELETE", path, j->token.data, NULL, 0, &resp);
        if (resp.status != 204)
            ui_post(UI_SEND_FAILED, ui_text(resp.status == 403 ? "You cannot delete this message" : "Message not deleted"));
    } else {
        sb_add(&body, "{\"content\":");
        sb_json_str(&body, j->text.data, j->text.len);
        sb_add(&body, "}");
        http_request("PATCH", path, j->token.data, body.data, body.len, &resp);
        if (resp.status != 200)
            ui_post(UI_SEND_FAILED, ui_text("Edit not saved"));
    }
    sb_free(&body);
    http_resp_free(&resp);
    free_job(j);
    return 0;
}

void app_edit_message(const char *channel_id, const char *message_id, const char *text)
{
    rest_job_t *j = new_job(channel_id);

    sb_add(&j->text, text);
    lstrcpynA(j->before, message_id, sizeof j->before);
    CloseHandle(CreateThread(NULL, 0, edit_main, j, 0, NULL));
}

void app_delete_message(const char *channel_id, const char *message_id)
{
    rest_job_t *j = new_job(channel_id);

    lstrcpynA(j->before, message_id, sizeof j->before);
    j->flag = 1;
    CloseHandle(CreateThread(NULL, 0, edit_main, j, 0, NULL));
}

static DWORD WINAPI typing_main(LPVOID arg)
{
    rest_job_t *j = arg;
    http_resp_t resp = {0};
    char path[96];

    wsprintfA(path, "/channels/%s/typing", j->channel);
    http_request("POST", path, j->token.data, "", 0, &resp);
    http_resp_free(&resp);
    free_job(j);
    return 0;
}

void app_typing(const char *channel_id)
{
    CloseHandle(CreateThread(NULL, 0, typing_main, new_job(channel_id), 0, NULL));
}

void app_quit(void)
{
    /* Leave voice first, while the gateway can still say so. */
    app_voice_leave();
    stop_login();
    stop_session();
    ExitProcess(0);
}

/* ---- Entry ---- */

static const char *skip_space(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static int has_flag(const char *flag)
{
    const char *p = GetCommandLineA();

    if (*p == '"') {
        for (p++; *p && *p != '"'; p++)
            ;
        if (*p)
            p++;
    } else {
        while (*p && *p != ' ' && *p != '\t')
            p++;
    }
    for (p = skip_space(p); *p; p = skip_space(p)) {
        const char *f = flag;
        while (*f && *p == *f) {
            p++;
            f++;
        }
        if (!*f && (!*p || *p == ' ' || *p == '\t'))
            return 1;
        while (*p && *p != ' ' && *p != '\t')
            p++;
    }
    return 0;
}

void entry(void)
{
    MSG msg;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_debug = has_flag("--debug");
    if (g_debug && AllocConsole())
        con_init();
    if (!http_init()) {
        MessageBoxW(NULL, L"Could not initialize WinHTTP.", L"Silicord", MB_ICONERROR);
        ExitProcess(1);
    }
    g_login_wake = CreateEventW(NULL, TRUE, FALSE, NULL);
    InitializeCriticalSection(&g_open_lock);
    InitializeCriticalSection(&g_session_lock);
    InitializeCriticalSection(&g_voice_lock);
    InitializeCriticalSection(&g_mix_lock);
    InitializeCriticalSection(&g_audio_lock);
    InitializeCriticalSection(&g_video_lock);
    mixer_init(&g_mixer);

    ShowWindow(ui_create(GetModuleHandleW(NULL)), SW_SHOWDEFAULT);
    if (cred_load(&g_token)) {
        ui_show_loading("Connecting\xE2\x80\xA6");
        start_session();
    } else {
        start_login();
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ExitProcess(0);
}
