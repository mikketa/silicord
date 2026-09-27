#include "http.h"
#include "mem.h"
#include "utf.h"
#include "sc_asm.h"

#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)
#define USER_AGENT L"Silicord/" WIDEN(SILICORD_VERSION) L" (+https://github.com/mikketa/silicord)"

static HINTERNET g_session;
static HINTERNET g_api;
static HINTERNET g_cdn;

int http_init(void)
{
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;

    g_session = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_session)
        return 0;
    WinHttpSetOption(g_session, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof decompress);
    g_api = WinHttpConnect(g_session, API_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    g_cdn = WinHttpConnect(g_session, CDN_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    return g_api && g_cdn;
}

HINTERNET http_session(void)
{
    return g_session;
}

static int read_body(HINTERNET req, sb_t *out)
{
    for (;;) {
        DWORD avail = 0, got = 0;

        if (!WinHttpQueryDataAvailable(req, &avail))
            return 0;
        if (!avail)
            return 1;
        sb_reserve(out, avail);
        if (!WinHttpReadData(req, out->data + out->len, avail, &got))
            return 0;
        out->len += got;
        out->data[out->len] = 0;
    }
}

static int do_request(HINTERNET conn, const char *method, const sb_t *url, const char *token,
                      const char *body, size_t body_len, http_resp_t *resp, const char *ctype)
{
    sb_t hdr = {0};
    wchar_t *wmethod, *wurl, *whdr;
    HINTERNET req;
    DWORD size = sizeof resp->status;
    DWORD err;
    int ok = 0;

    if (token) {
        sb_add(&hdr, "Authorization: ");
        sb_add(&hdr, token);
        sb_add(&hdr, "\r\n");
    }
    if (body) {
        sb_add(&hdr, "Content-Type: ");
        sb_add(&hdr, ctype ? ctype : "application/json");
        sb_add(&hdr, "\r\n");
    }

    wmethod = utf8_to_wide(method, sc_strlen(method));
    wurl = utf8_to_wide(url->data, url->len);
    whdr = utf8_to_wide(hdr.data, hdr.len);

    resp->status = 0;
    req = WinHttpOpenRequest(conn, wmethod, wurl, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (req &&
        (!hdr.len || WinHttpAddRequestHeaders(req, whdr, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD)) &&
        WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)body,
                           (DWORD)body_len, (DWORD)body_len, 0) &&
        WinHttpReceiveResponse(req, NULL) &&
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &resp->status, &size,
                            WINHTTP_NO_HEADER_INDEX))
        ok = read_body(req, &resp->body);
    err = GetLastError();

    if (req)
        WinHttpCloseHandle(req);
    SecureZeroMemory(whdr, (hdr.len + 1) * sizeof(wchar_t));
    mem_free(whdr);
    mem_free(wurl);
    mem_free(wmethod);
    sb_free(&hdr);
    SetLastError(err);
    return ok;
}

int http_request_type(const char *method, const char *path, const char *token, const char *content_type,
                      const char *body, size_t body_len, http_resp_t *resp)
{
    sb_t url = {0};
    int ok;

    sb_add(&url, API_BASE);
    sb_add(&url, path);
    ok = do_request(g_api, method, &url, token, body, body_len, resp, content_type);
    sb_free(&url);
    return ok;
}

int http_request(const char *method, const char *path, const char *token,
                 const char *body, size_t body_len, http_resp_t *resp)
{
    return http_request_type(method, path, token, NULL, body, body_len, resp);
}

int http_cdn_get(const char *path, http_resp_t *resp)
{
    sb_t url = {0};
    int ok;

    sb_add(&url, path);
    ok = do_request(g_cdn, "GET", &url, NULL, NULL, 0, resp, NULL);
    sb_free(&url);
    return ok;
}

int http_get(const wchar_t *host, const char *path, http_resp_t *resp)
{
    HINTERNET conn = WinHttpConnect(g_session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    sb_t url = {0};
    int ok;

    if (!conn)
        return 0;
    sb_add(&url, path);
    ok = do_request(conn, "GET", &url, NULL, NULL, 0, resp, NULL);
    sb_free(&url);
    WinHttpCloseHandle(conn);
    return ok;
}

void http_resp_free(http_resp_t *resp)
{
    sb_free(&resp->body);
    resp->status = 0;
}
