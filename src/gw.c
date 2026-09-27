#include <windows.h>
#include <winhttp.h>
#include "gw.h"
#include "http.h"
#include "json.h"
#include "console.h"
#include "sb.h"
#include "sc_asm.h"

#define GW_HOST L"gateway.discord.gg"
#define GW_PATH L"/?v=10&encoding=json"
#define RECV_CHUNK 16384

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
    HINTERNET conn;
    HINTERNET ws;
    HANDLE stop;
    HANDLE heartbeat;
    CRITICAL_SECTION send_lock;
    const char *token;
    DWORD interval;
    volatile LONG64 seq;   /* -1 until the first dispatch */
    volatile LONG acked;
} gw_t;

static gw_t g_gw;

static int gw_send(gw_t *g, const sb_t *msg)
{
    DWORD err;

    EnterCriticalSection(&g->send_lock);
    err = WinHttpWebSocketSend(g->ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                               msg->data, (DWORD)msg->len);
    LeaveCriticalSection(&g->send_lock);
    return err == NO_ERROR;
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
    ok = gw_send(g, &msg);
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
    ok = gw_send(g, &msg);
    sb_free(&msg);
    return ok;
}

static DWORD WINAPI heartbeat_main(LPVOID arg)
{
    gw_t *g = arg;
    /* The first beat is jittered, as the gateway docs ask. */
    DWORD wait = (DWORD)((unsigned long long)g->interval * (GetTickCount() % 1000) / 1000);

    while (WaitForSingleObject(g->stop, wait) == WAIT_TIMEOUT) {
        if (!InterlockedExchange(&g->acked, 0)) {
            con_print("gateway: no heartbeat ack, closing\r\n");
            gw_stop();
            break;
        }
        if (!send_heartbeat(g))
            break;
        wait = g->interval;
    }
    return 0;
}

static int gw_connect(gw_t *g)
{
    HINTERNET req;

    g->conn = WinHttpConnect(http_session(), GW_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!g->conn)
        return 0;
    req = WinHttpOpenRequest(g->conn, L"GET", GW_PATH, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req)
        return 0;
    if (WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0) &&
        WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) &&
        WinHttpReceiveResponse(req, NULL))
        g->ws = WinHttpWebSocketCompleteUpgrade(req, 0);
    WinHttpCloseHandle(req);
    return g->ws != NULL;
}

/* Receives one complete message, reassembling fragments. */
static int gw_recv(gw_t *g, sb_t *msg)
{
    sb_clear(msg);
    for (;;) {
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD got = 0;

        sb_reserve(msg, RECV_CHUNK);
        if (WinHttpWebSocketReceive(g->ws, msg->data + msg->len,
                                    (DWORD)(msg->cap - msg->len - 1), &got, &type) != NO_ERROR)
            return 0;
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            return 0;
        msg->len += got;
        msg->data[msg->len] = 0;
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
            return 1;
    }
}

static void on_ready(json_t d)
{
    json_t user, name, guilds;
    sb_t out = {0};

    sb_add(&out, "gateway: ready");
    if (json_get(d, "user", &user) && json_get(user, "username", &name)) {
        sb_add(&out, " as ");
        json_str(name, &out);
    }
    if (json_get(d, "guilds", &guilds)) {
        sb_add(&out, ", ");
        sb_u64(&out, json_count(guilds));
        sb_add(&out, " servers");
    }
    sb_add(&out, "\r\n");
    con_print_sb(&out);
    sb_free(&out);
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
        if (json_get(root, "t", &t) && json_str_eq(t, "READY"))
            on_ready(d);
        return 1;
    case OP_RECONNECT:
        con_print("gateway: server asked to reconnect\r\n");
        return 0;
    case OP_INVALID_SESSION:
        con_print("gateway: invalid session\r\n");
        return 0;
    default:
        return 1;
    }
}

int gw_run(const char *token)
{
    gw_t *g = &g_gw;
    sb_t msg = {0};
    USHORT status = 0;
    DWORD reason_len = 0;
    char reason[124];

    g->token = token;
    g->seq = -1;
    InitializeCriticalSection(&g->send_lock);
    g->stop = CreateEventW(NULL, TRUE, FALSE, NULL);

    if (!gw_connect(g)) {
        con_print("gateway: connection failed\r\n");
    } else {
        con_print("gateway: connected\r\n");
        while (gw_recv(g, &msg) && handle(g, &msg))
            ;
        if (WinHttpWebSocketQueryCloseStatus(g->ws, &status, reason, sizeof reason - 1,
                                             &reason_len) == NO_ERROR && status) {
            sb_t out = {0};
            sb_add(&out, "gateway: closed (");
            sb_u64(&out, status);
            if (reason_len) {
                sb_add(&out, " ");
                sb_addn(&out, reason, reason_len);
            }
            sb_add(&out, ")\r\n");
            con_print_sb(&out);
            sb_free(&out);
        }
    }

    SetEvent(g->stop);
    if (g->heartbeat) {
        WaitForSingleObject(g->heartbeat, INFINITE);
        CloseHandle(g->heartbeat);
    }
    /* The lock and the stop event live until exit: gw_stop() may still run from the console handler. */
    EnterCriticalSection(&g->send_lock);
    if (g->ws)
        WinHttpCloseHandle(g->ws);
    g->ws = NULL;
    LeaveCriticalSection(&g->send_lock);
    if (g->conn)
        WinHttpCloseHandle(g->conn);
    sb_free(&msg);
    return status == WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS;
}

void gw_stop(void)
{
    gw_t *g = &g_gw;

    if (!g->stop)
        return;
    SetEvent(g->stop);
    /* Sends a close frame; the pending receive in gw_run then returns. */
    EnterCriticalSection(&g->send_lock);
    if (g->ws)
        WinHttpWebSocketShutdown(g->ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
    LeaveCriticalSection(&g->send_lock);
}
