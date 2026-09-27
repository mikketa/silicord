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

#define JOIN_TIMEOUT 5000

static int g_debug;
static sb_t g_token;                 /* current account, UI thread only */
static HANDLE g_login_thread;
static HANDLE g_login_wake;
static volatile LONG g_login_stop;
static HANDLE g_session_thread;
static volatile LONG g_session_id;   /* bumped to silence a session that is being stopped */
static CRITICAL_SECTION g_open_lock;
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
}

static int is_open(const char *channel_id)
{
    int open;

    EnterCriticalSection(&g_open_lock);
    open = g_open_channel[0] && lstrcmpA(g_open_channel, channel_id) == 0;
    LeaveCriticalSection(&g_open_lock);
    return open;
}

/* Mentioned directly, or through @everyone / @here. Role mentions are not resolved yet. */
static int mentions_me(json_t d, const char *me)
{
    json_t v, list, user;
    json_iter_t it;
    char id[24];

    if (json_get(d, "mention_everyone", &v) && json_type(v) == JSON_TRUE)
        return 1;
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

static void on_dispatch(void *ctx, json_t t, json_t d)
{
    session_t *s = ctx;
    int kind;
    msg_batch_t *b;

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
    if (current(s))
        ui_post_model(model);
    else
        model_free(model);
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
    int attempt = 0, resume = 0, established;

    /* Check the token; while offline, keep trying instead of giving up. */
    for (;;) {
        status = check_token(s->token.data, &name);
        if (status && status < 500)
            break;
        post_reconnecting(s, backoff(attempt));
        if (gw_wait(backoff(attempt++)))
            goto end;
    }
    if (status == 401) {
        cred_delete();
        if (current(s))
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
    if (!cred_save(s->token.data, s->token.len))
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

static void stop_session(void)
{
    if (!g_session_thread)
        return;
    InterlockedIncrement(&g_session_id);
    gw_stop();
    join(&g_session_thread);
}

static void start_session(void)
{
    session_t *s = mem_alloc(sizeof *s);

    stop_session();
    gw_reset();
    s->id = InterlockedIncrement(&g_session_id);
    sb_addn(&s->token, g_token.data, g_token.len);
    g_session_thread = CreateThread(NULL, 0, session_main, s, 0, NULL);
}

/* ---- Messages over REST ---- */

typedef struct {
    sb_t token;
    sb_t text;
    char channel[24];
    char before[24];
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
    mem_free(j);
}

static DWORD WINAPI fetch_main(LPVOID arg)
{
    rest_job_t *j = arg;
    http_resp_t resp = {0};
    msg_batch_t *b = NULL;
    json_t root;
    char path[128];
    int kind = j->before[0] ? BATCH_OLDER : BATCH_HISTORY;

    if (j->before[0])
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
    lstrcpynA(b->before, j->before, sizeof b->before);
    ui_post_batch(b);
    http_resp_free(&resp);
    free_job(j);
    return 0;
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
    sb_add(&body, "\",\"tts\":false}");

    if (!http_request("POST", path, j->token.data, body.data, body.len, &resp)) {
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
    static const char body[] = "{\"token\":null}";

    wsprintfA(path, "/channels/%s/messages/%s/ack", j->channel, j->before);
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

void app_fetch_channel(const char *channel_id)
{
    CloseHandle(CreateThread(NULL, 0, channel_main, new_job(channel_id), 0, NULL));
}

void app_ack(const char *channel_id, const char *message_id)
{
    rest_job_t *j = new_job(channel_id);

    lstrcpynA(j->before, message_id, sizeof j->before);
    CloseHandle(CreateThread(NULL, 0, ack_main, j, 0, NULL));
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

void app_quit(void)
{
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
