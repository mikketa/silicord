/* Silicord - native Discord client for Windows. No C runtime: entry point is `entry`. */
#include <windows.h>
#include "console.h"
#include "cred.h"
#include "gw.h"
#include "http.h"
#include "json.h"
#include "mem.h"
#include "model.h"
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
    if (current(s))
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

static void on_ready(void *ctx, json_t d)
{
    session_t *s = ctx;
    model_t *model;

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

static DWORD WINAPI session_main(LPVOID arg)
{
    session_t *s = arg;
    gw_events_t ev = {s, on_gw_status, on_ready};
    sb_t name = {0}, text = {0};
    DWORD status = check_token(s->token.data, &name);

    if (status == 200) {
        if (!cred_save(s->token.data, s->token.len))
            log_line("session: ", "could not save the token");
        if (current(s))
            ui_post(UI_ACCOUNT, copy(name.data ? name.data : "", name.len));
        gw_run(s->token.data, &ev);
        sb_add(&text, s->last_status.len ? s->last_status.data : "Disconnected");
        if (current(s))
            ui_post(UI_DISCONNECTED, copy(text.data, text.len));
    } else if (status == 401) {
        cred_delete();
        if (current(s))
            ui_post(UI_LOGIN_FAILED, ui_text("Discord rejected this token."));
    } else {
        if (status) {
            sb_add(&text, "Discord answered with HTTP ");
            sb_u64(&text, status);
        } else {
            sb_add(&text, "Could not reach discord.com");
        }
        if (current(s))
            ui_post(UI_DISCONNECTED, copy(text.data, text.len));
    }
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
