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
    OP_RECONNECT = 7,
    OP_INVALID_SESSION = 9,
    OP_HELLO = 10,
    OP_HEARTBEAT_ACK = 11,
};

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
    volatile LONG ready;   /* events created; gw_run() and gw_reset() run on one thread */
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

static DWORD WINAPI heartbeat_main(LPVOID arg)
{
    gw_t *g = arg;
    HANDLE events[2] = {g->done, g->cancel};
    /* The first beat is jittered, as the gateway docs ask. */
    DWORD wait = (DWORD)((unsigned long long)g->interval * (GetTickCount() % 1000) / 1000);

    while (WaitForMultipleObjects(2, events, FALSE, wait) == WAIT_TIMEOUT) {
        if (!InterlockedExchange(&g->acked, 0)) {
            status(g, "Connection lost");
            gw_stop();
            break;
        }
        if (!send_heartbeat(g))
            break;
        wait = g->interval;
    }
    return 0;
}

/* Returns 0 when the session must end. */
static int handle(gw_t *g, const sb_t *msg)
{
    json_t root, op, s, t, d = {0};
    long long opcode, seq;

    if (!json_parse(msg->data, msg->len, &root) || !json_get(root, "op", &op) ||
        !json_int(op, &opcode))
        return 1;
    json_get(root, "d", &d);

    switch (opcode) {
    case OP_HELLO: {
        json_t hb;
        long long interval;

        if (!json_get(d, "heartbeat_interval", &hb) || !json_int(hb, &interval) || interval <= 0)
            return 0;
        g->interval = (DWORD)interval;
        g->acked = 1;
        g->heartbeat = CreateThread(NULL, 0, heartbeat_main, g, 0, NULL);
        return send_identify(g);
    }
    case OP_HEARTBEAT:
        return send_heartbeat(g);
    case OP_HEARTBEAT_ACK:
        InterlockedExchange(&g->acked, 1);
        return 1;
    case OP_DISPATCH:
        if (json_get(root, "s", &s) && json_int(s, &seq))
            InterlockedExchange64(&g->seq, seq);
        if (json_get(root, "t", &t) && json_str_eq(t, "READY") && g->ev->ready)
            g->ev->ready(g->ev->ctx, d);
        return 1;
    case OP_RECONNECT:
        status(g, "Discord asked to reconnect");
        return 0;
    case OP_INVALID_SESSION:
        status(g, "Session invalidated");
        return 0;
    default:
        return 1;
    }
}

static void init_once(gw_t *g)
{
    if (g->ready)
        return;
    ws_init(&g->ws);
    g->cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    g->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    InterlockedExchange(&g->ready, 1);
}

static int cancelled(gw_t *g)
{
    return WaitForSingleObject(g->cancel, 0) == WAIT_OBJECT_0;
}

int gw_run(const char *token, const gw_events_t *ev)
{
    gw_t *g = &g_gw;
    sb_t msg = {0};
    unsigned code = 0;

    init_once(g);
    if (cancelled(g))
        return 0;
    ResetEvent(g->done);
    g->ev = ev;
    g->token = token;
    g->seq = -1;
    g->heartbeat = NULL;

    if (!ws_connect(&g->ws, GW_HOST, GW_PATH, NULL)) {
        status(g, "Could not connect to the gateway");
        return 0;
    }
    status(g, "Connected");
    while (!cancelled(g) && ws_recv(&g->ws, &msg) && handle(g, &msg))
        ;

    code = ws_close_status(&g->ws, NULL);
    if (code) {
        sb_t text = {0};
        sb_add(&text, "Disconnected (code ");
        sb_u64(&text, code);
        sb_add(&text, ")");
        status(g, text.data);
        sb_free(&text);
    }
    SetEvent(g->done);
    if (g->heartbeat) {
        WaitForSingleObject(g->heartbeat, INFINITE);
        CloseHandle(g->heartbeat);
    }
    ws_close(&g->ws);
    sb_free(&msg);
    return code == WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS;
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
}
