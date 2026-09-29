#include "ws.h"
#include "http.h"

#define RECV_CHUNK 16384

void ws_init(ws_t *ws)
{
    ws->conn = ws->socket = NULL;
    InitializeCriticalSection(&ws->lock);
}

int ws_connect(ws_t *ws, const wchar_t *host, INTERNET_PORT port, const wchar_t *path, const wchar_t *headers)
{
    HINTERNET conn, req, socket = NULL;

    conn = WinHttpConnect(http_session(), host, port, 0);
    if (!conn)
        return 0;
    req = WinHttpOpenRequest(conn, L"GET", path, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (req &&
        WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0) &&
        (!headers || WinHttpAddRequestHeaders(req, headers, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD)) &&
        WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) &&
        WinHttpReceiveResponse(req, NULL))
        socket = WinHttpWebSocketCompleteUpgrade(req, 0);
    if (req)
        WinHttpCloseHandle(req);
    if (!socket) {
        WinHttpCloseHandle(conn);
        return 0;
    }
    EnterCriticalSection(&ws->lock);
    ws->conn = conn;
    ws->socket = socket;
    LeaveCriticalSection(&ws->lock);
    return 1;
}

static int send_typed(ws_t *ws, WINHTTP_WEB_SOCKET_BUFFER_TYPE type, const void *data, size_t n)
{
    DWORD err = ERROR_INVALID_HANDLE;

    EnterCriticalSection(&ws->lock);
    if (ws->socket)
        err = WinHttpWebSocketSend(ws->socket, type, (void *)data, (DWORD)n);
    LeaveCriticalSection(&ws->lock);
    return err == NO_ERROR;
}

int ws_send(ws_t *ws, const sb_t *msg)
{
    return send_typed(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, msg->data, msg->len);
}

int ws_send_binary(ws_t *ws, const void *data, size_t n)
{
    return send_typed(ws, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, data, n);
}

int ws_recv(ws_t *ws, sb_t *msg)
{
    int binary;

    return ws_recv_kind(ws, msg, &binary);
}

int ws_recv_kind(ws_t *ws, sb_t *msg, int *binary)
{
    sb_clear(msg);
    for (;;) {
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD got = 0;

        sb_reserve(msg, RECV_CHUNK);
        if (WinHttpWebSocketReceive(ws->socket, msg->data + msg->len,
                                    (DWORD)(msg->cap - msg->len - 1), &got, &type) != NO_ERROR)
            return 0;
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            return 0;
        msg->len += got;
        msg->data[msg->len] = 0;
        *binary = type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
            return 1;
    }
}

void ws_shutdown(ws_t *ws, unsigned short code)
{
    EnterCriticalSection(&ws->lock);
    if (ws->socket)
        WinHttpWebSocketShutdown(ws->socket, code, NULL, 0);
    LeaveCriticalSection(&ws->lock);
}

unsigned ws_close_status(ws_t *ws, sb_t *reason)
{
    USHORT status = 0;
    DWORD len = 0;
    char buf[124];

    if (!ws->socket ||
        WinHttpWebSocketQueryCloseStatus(ws->socket, &status, buf, sizeof buf, &len) != NO_ERROR)
        return 0;
    if (reason && len)
        sb_addn(reason, buf, len);
    return status;
}

void ws_close(ws_t *ws)
{
    EnterCriticalSection(&ws->lock);
    if (ws->socket)
        WinHttpCloseHandle(ws->socket);
    if (ws->conn)
        WinHttpCloseHandle(ws->conn);
    ws->socket = ws->conn = NULL;
    LeaveCriticalSection(&ws->lock);
}
