#include <windows.h>
#include "gw.h"
#include "sb.h"
#include "sc_asm.h"
#include "ws.h"

#define GW_HOST L"gateway.discord.gg"
#define GW_PATH L"/?v=10&encoding=json"

enum {
    OP_DISPATCH = 0,
    OP_HEARTBEAT = 1,
    OP_IDENTIFY = 2,
    OP_RESUME = 6,
    OP_RECONNECT = 7,
    OP_INVALID_SESSION = 9,
    OP_HELLO = 10,
    OP_HEARTBEAT_ACK = 11,
};

#define CONTINUE (-1)

typedef struct {
    ws_t ws;
    HANDLE cancel;     /* set by gw_stop() until gw_reset() */
    HANDLE done;       /* set when one gw_run() ends */
    HANDLE heartbeat;
    const gw_events_t *ev;
    const char *token;
    DWORD interval;
    volatile LONG64 seq;   /* -1 until the first dispatch */
    volatile LONG acked;
    volatile LONG zombie;  /* no heartbeat ack: the connection is dead */
    volatile LONG ready;   /* events created; gw_run() and gw_reset() run on one thread */
    int resuming;
    int *established;
    char session_id[80];
    wchar_t resume_host[128];
} gw_t;

static gw_t g_gw;

static void status(gw_t *g, const char *text)
{
    if (g->ev->status)
        g->ev->status(g->ev->ctx, text);
}

static int send_heartbeat(gw_t *g)
{
    sb_t msg = {0};
    LONG64 seq = InterlockedCompareExchange64(&g->seq, 0, 0);
    int ok;

    sb_add(&msg, "{\"op\":1,\"d\":");
    if (seq < 0)
        sb_add(&msg, "null");
    else
        sb_i64(&msg, seq);
    sb_add(&msg, "}");
    ok = ws_send(&g->ws, &msg);
    sb_free(&msg);
    return ok;
}

static int send_identify(gw_t *g)
{
    sb_t msg = {0};
    int ok;

    sb_add(&msg, "{\"op\":2,\"d\":{\"token\":");
    sb_json_str(&msg, g->token, sc_strlen(g->token));
    sb_add(&msg, ",\"properties\":{\"os\":\"Windows\",\"browser\":\"Silicord\",\"device\":\"Silicord\"}}}");
    ok = ws_send(&g->ws, &msg);
    sb_free(&msg);
    return ok;
}

static int send_resume(gw_t *g)
{
    sb_t msg = {0};
    int ok;

    sb_add(&msg, "{\"op\":6,\"d\":{\"token\":");
    sb_json_str(&msg, g->token, sc_strlen(g->token));
    sb_add(&msg, ",\"session_id\":");
    sb_json_str(&msg, g->session_id, sc_strlen(g->session_id));
    sb_add(&msg, ",\"seq\":");
    sb_i64(&msg, InterlockedCompareExchange64(&g->seq, 0, 0));
    sb_add(&msg, "}}");
    ok = ws_send(&g->ws, &msg);
    sb_free(&msg);
    return ok;
}

static DWORD WINAPI heartbeat_main(LPVOID arg)
{
    gw_t *g = arg;
    HANDLE events[2] = {g->done, g->cancel};
    /* The first beat is jittered, as the gateway docs ask. */
    DWORD wait = (DWORD)((unsigned long long)g->interval * (GetTickCount() % 1000) / 1000);

    while (WaitForMultipleObjects(2, events, FALSE, wait) == WAIT_TIMEOUT) {
        if (!InterlockedExchange(&g->acked, 0)) {
            /* Zombie connection: close it and let gw_run() resume. */
            InterlockedExchange(&g->zombie, 1);
            ws_shutdown(&g->ws);
            break;
        }
        if (!send_heartbeat(g))
            break;
        wait = g->interval;
    }
    return 0;
}

static void forget_session(gw_t *g)
{
    g->session_id[0] = 0;
    g->resume_host[0] = 0;
    InterlockedExchange64(&g->seq, -1);
}

/* Keeps what RESUME needs: the session id and the host to resume on. */
static void remember_session(gw_t *g, json_t d)
{
    json_t v;
    char url[128];
    const char *host;
    int n = 0;

    if (json_get(d, "session_id", &v))
        json_raw(v, g->session_id, sizeof g->session_id);
    g->resume_host[0] = 0;
    if (!json_get(d, "resume_gateway_url", &v))
        return;
    json_raw(v, url, sizeof url);
    host = url;
    for (const char *p = url; *p; p++)
        if (p[0] == '/' && p[1] == '/') {
            host = p + 2;
            break;
        }
    while (host[n] && host[n] != '/' && host[n] != '?' && n < (int)ARRAYSIZE(g->resume_host) - 1) {
        g->resume_host[n] = (wchar_t)(unsigned char)host[n];
        n++;
    }
    g->resume_host[n] = 0;
}

static int handle(gw_t *g, const sb_t *msg)
{
    json_t root, op, s, t, d = {0};
    long long opcode, seq;

    if (!json_parse(msg->data, msg->len, &root) || !json_get(root, "op", &op) ||
        !json_int(op, &opcode))
        return CONTINUE;
    json_get(root, "d", &d);

    switch (opcode) {
    case OP_HELLO: {
        json_t hb;
        long long interval;

        if (!json_get(d, "heartbeat_interval", &hb) || !json_int(hb, &interval) || interval <= 0)
            return GW_RESUME;
        g->interval = (DWORD)interval;
        g->acked = 1;
        g->heartbeat = CreateThread(NULL, 0, heartbeat_main, g, 0, NULL);
        if (g->resuming)
            return send_resume(g) ? CONTINUE : GW_RESUME;
        forget_session(g);
        return send_identify(g) ? CONTINUE : GW_RESUME;
    }
    case OP_HEARTBEAT:
        return send_heartbeat(g) ? CONTINUE : GW_RESUME;
    case OP_HEARTBEAT_ACK:
        InterlockedExchange(&g->acked, 1);
        return CONTINUE;
    case OP_DISPATCH:
        if (json_get(root, "s", &s) && json_int(s, &seq))
            InterlockedExchange64(&g->seq, seq);
        if (!json_get(root, "t", &t))
            return CONTINUE;
        if (json_str_eq(t, "READY")) {
            remember_session(g, d);
            *g->established = 1;
            if (g->ev->ready)
                g->ev->ready(g->ev->ctx, d);
        } else if (json_str_eq(t, "RESUMED")) {
            *g->established = 1;
            status(g, "Online");
        } else if (g->ev->dispatch) {
            g->ev->dispatch(g->ev->ctx, t, d);
        }
        return CONTINUE;
    case OP_RECONNECT:
        status(g, "Discord asked to reconnect");
        return GW_RESUME;
    case OP_INVALID_SESSION:
        if (json_type(d) == JSON_TRUE)
            return GW_RESUME;
        forget_session(g);
        return GW_REIDENTIFY;
    default:
        return CONTINUE;
    }
}

/* What a close code from Discord means for the next attempt. */
static gw_result_t after_close(gw_t *g, unsigned code)
{
    switch (code) {
    case 4004:
        return GW_AUTH_FAILED;
    case 4010: case 4011: case 4012: case 4013: case 4014:
        return GW_FATAL;
    case 4007: case 4009:
        forget_session(g);
        return GW_REIDENTIFY;
    default:
        return g->session_id[0] ? GW_RESUME : GW_REIDENTIFY;
    }
}

static void init_once(gw_t *g)
{
    if (g->ready)
        return;
    ws_init(&g->ws);
    g->cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    g->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    g->seq = -1;
    InterlockedExchange(&g->ready, 1);
}

static int cancelled(gw_t *g)
{
    return WaitForSingleObject(g->cancel, 0) == WAIT_OBJECT_0;
}

gw_result_t gw_run(const char *token, const gw_events_t *ev, int resume, int *established)
{
    gw_t *g = &g_gw;
    sb_t msg = {0};
    int result = CONTINUE;
    const wchar_t *host;

    init_once(g);
    *established = 0;
    if (cancelled(g))
        return GW_STOPPED;
    ResetEvent(g->done);
    g->ev = ev;
    g->token = token;
    g->established = established;
    g->heartbeat = NULL;
    g->zombie = 0;
    g->resuming = resume && g->session_id[0];
    host = g->resuming && g->resume_host[0] ? g->resume_host : GW_HOST;

    if (!ws_connect(&g->ws, host, GW_PATH, NULL)) {
        g->resume_host[0] = 0; /* next time, try the main host */
        status(g, "Could not reach the gateway");
        return cancelled(g) ? GW_STOPPED : (g->session_id[0] ? GW_RESUME : GW_REIDENTIFY);
    }
    status(g, g->resuming ? "Resuming\xE2\x80\xA6" : "Connected");
    while (!cancelled(g) && ws_recv(&g->ws, &msg) && (result = handle(g, &msg)) == CONTINUE)
        ;

    if (cancelled(g))
        result = GW_STOPPED;
    else if (result == CONTINUE)
        result = g->zombie ? GW_RESUME : after_close(g, ws_close_status(&g->ws, NULL));
    SetEvent(g->done);
    if (g->heartbeat) {
        WaitForSingleObject(g->heartbeat, INFINITE);
        CloseHandle(g->heartbeat);
    }
    ws_shutdown(&g->ws);
    ws_close(&g->ws);
    sb_free(&msg);
    return (gw_result_t)result;
}

int gw_send(const sb_t *payload)
{
    return g_gw.ready && ws_send(&g_gw.ws, payload);
}

void gw_session_id(char *out, size_t size)
{
    size_t i = 0;

    for (; i + 1 < size && g_gw.session_id[i]; i++)
        out[i] = g_gw.session_id[i];
    if (size)
        out[i] = 0;
}

int gw_wait(unsigned ms)
{
    init_once(&g_gw);
    return WaitForSingleObject(g_gw.cancel, ms) == WAIT_OBJECT_0;
}

void gw_stop(void)
{
    gw_t *g = &g_gw;

    if (!g->ready)
        return;
    SetEvent(g->cancel);
    ws_shutdown(&g->ws);
}

void gw_reset(void)
{
    init_once(&g_gw);
    ResetEvent(g_gw.cancel);
    forget_session(&g_gw);
}
