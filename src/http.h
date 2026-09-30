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
/* Reads the website's build number, user agent and fingerprint. Safe to call again. */
void http_prepare_client(void);
/* Appends the gateway identify `properties` object (the website's, plus its session fields). */
void http_identify_properties(sb_t *out);
/* Origin and User-Agent for a Discord websocket upgrade. 0 if `dst` is too small. */
int http_ws_headers(wchar_t *dst, size_t cap);

/*
 * Synchronous HTTPS request to the Discord API. `path` is relative to API_BASE,
 * `token` and `body` may be NULL. On transport failure returns 0 and GetLastError() holds
 * the WinHTTP error.
 */
int http_request(const char *method, const char *path, const char *token,
                 const char *body, size_t body_len, http_resp_t *resp);
/* Same, with another Content-Type than JSON for the body (multipart uploads). */
int http_request_type(const char *method, const char *path, const char *token, const char *content_type,
                      const char *body, size_t body_len, http_resp_t *resp);
/* Same, with more headers: "Name: value\r\n" lines (a solved captcha's key, for one). */
int http_request_hdr(const char *method, const char *path, const char *token, const char *content_type,
                     const char *headers, const char *body, size_t body_len, http_resp_t *resp);
/* GET on the media CDN, path starting with '/'. */
int http_cdn_get(const char *path, http_resp_t *resp);
/* GET on another HTTPS host, without credentials. */
int http_get(const wchar_t *host, const char *path, http_resp_t *resp);
void http_resp_free(http_resp_t *resp);
