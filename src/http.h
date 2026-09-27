#pragma once
#include <windows.h>
#include <winhttp.h>
#include "sb.h"

#define API_HOST L"discord.com"
#define API_BASE "/api/v10"
#define CDN_HOST L"cdn.discordapp.com"

typedef struct {
    DWORD status;
    sb_t body;
} http_resp_t;

int http_init(void);
HINTERNET http_session(void);

/*
 * Synchronous HTTPS request to the Discord API. `path` is relative to API_BASE,
 * `token` and `body` may be NULL. On transport failure returns 0 and GetLastError() holds
 * the WinHTTP error.
 */
int http_request(const char *method, const char *path, const char *token,
                 const char *body, size_t body_len, http_resp_t *resp);
/* GET on the media CDN, path starting with '/'. */
int http_cdn_get(const char *path, http_resp_t *resp);
/* GET on another HTTPS host, without credentials. */
int http_get(const wchar_t *host, const char *path, http_resp_t *resp);
void http_resp_free(http_resp_t *resp);
