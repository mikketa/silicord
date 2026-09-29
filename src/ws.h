#pragma once
#include <windows.h>
#include <winhttp.h>
#include "sb.h"

/*
 * WebSocket client over WinHTTP. Embed a ws_t in a long-lived struct and call
 * ws_init() once: the lock stays valid so ws_shutdown() is safe from any
 * thread, even after ws_close().
 */
typedef struct {
    HINTERNET conn;
    HINTERNET socket;
    CRITICAL_SECTION lock;
} ws_t;

void ws_init(ws_t *ws);
/* `headers` may be NULL, otherwise CRLF-separated "Name: value" lines. */
/* `port`: INTERNET_DEFAULT_HTTPS_PORT, or the one a voice endpoint names. */
int ws_connect(ws_t *ws, const wchar_t *host, INTERNET_PORT port, const wchar_t *path, const wchar_t *headers);
int ws_send(ws_t *ws, const sb_t *msg);
int ws_send_binary(ws_t *ws, const void *data, size_t n);
/* Receives one complete text or binary message. Returns 0 once the socket is closed. */
int ws_recv(ws_t *ws, sb_t *msg);
/* The same, telling a binary message from a text one. */
int ws_recv_kind(ws_t *ws, sb_t *msg, int *binary);
/*
 * Starts the closing handshake with `code`; a pending ws_recv() then returns 0.
 * Discord ends the session on 1000 and 1001: close with another code to resume.
 */
#define WS_CLOSE_NORMAL 1000
#define WS_CLOSE_RESUME 4000
void ws_shutdown(ws_t *ws, unsigned short code);
/* Close code sent by the server (0 if none), reason appended to `reason` if not NULL. */
unsigned ws_close_status(ws_t *ws, sb_t *reason);
void ws_close(ws_t *ws);
