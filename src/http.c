#include "http.h"
#include "props.h"
#include "solve.h"
#include "stats.h"
#include "mem.h"
#include "utf.h"
#include "json.h"
#include "sc_asm.h"
#include <bcrypt.h>

#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)
/* Session default. Discord's API and the gateway replace it with the web client's. */
#define USER_AGENT L"Silicord/" WIDEN(SILICORD_VERSION) L" (+https://github.com/mikketa/silicord)"

/* Stable web build from discord.com/login on 2026-09-30, used only when that page cannot be read. */
#define FALLBACK_BUILD 623968u
#define FALLBACK_CHROME "148.0.0.0"
#define CAPTCHA_TRIES 4
#define COOKIE_MAX 24

static HINTERNET g_session;
static HINTERNET g_api;
static HINTERNET g_cdn;

static CRITICAL_SECTION g_lock;
static volatile LONG g_ready;
static volatile LONG g_preparing;
static sb_t g_ua, g_ver, g_props_json, g_props_b64, g_fp, g_locale, g_tz;

typedef struct {
    sb_t name;
    sb_t value;
} cookie_t;

static cookie_t g_ck[COOKIE_MAX];
static int g_ck_n;

int http_init(void)
{
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;

    InitializeCriticalSection(&g_lock);
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

static int version_ok(const sb_t *v)
{
    int dots = 0;

    if (!v->len)
        return 0;
    for (size_t i = 0; i < v->len; i++) {
        char c = v->data[i];
        if (c == '.')
            dots++;
        else if (c < '0' || c > '9')
            return 0;
    }
    return dots >= 1;
}

static int read_reg_sz(const wchar_t *key_name, const wchar_t *value, sb_t *out)
{
    HKEY key;
    wchar_t pv[64] = {0};
    DWORD bytes = sizeof pv, type = 0;
    size_t nch = 0;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_name, 0, KEY_READ | KEY_WOW64_64KEY, &key))
        return 0;
    if (!RegQueryValueExW(key, value, NULL, &type, (BYTE *)pv, &bytes) && type == REG_SZ) {
        while (nch < 63 && pv[nch])
            nch++;
        wide_to_utf8(pv, nch, out);
    }
    RegCloseKey(key);
    return version_ok(out);
}

/* The installed Chromium's version, so the user agent matches a browser on this machine. */
static void browser_full(sb_t *full)
{
    static const wchar_t *edge[] = {
        L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",
        L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",
    };
    static const wchar_t *chrome[] = {
        L"SOFTWARE\\Google\\Chrome\\BLBeacon",
        L"SOFTWARE\\WOW6432Node\\Google\\Chrome\\BLBeacon",
    };

    for (int i = 0; i < 2; i++)
        if (read_reg_sz(edge[i], L"pv", full))
            return;
    sb_clear(full);
    for (int i = 0; i < 2; i++)
        if (read_reg_sz(chrome[i], L"version", full))
            return;
    sb_clear(full);
}

/* Chrome's reduced user agent: 148.0.3595.94 is sent as 148.0.0.0. */
static void reduce_version(const sb_t *full, sb_t *out)
{
    size_t i = 0;

    if (!version_ok(full)) {
        sb_add(out, FALLBACK_CHROME);
        return;
    }
    while (i < full->len && full->data[i] != '.')
        i++;
    sb_addn(out, full->data, i);
    sb_add(out, ".0.0.0");
}

static void locale_name(sb_t *out)
{
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {0};
    int n = GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH);

    if (n > 1)
        wide_to_utf8(name, (size_t)(n - 1), out);
    if (!out->len)
        sb_add(out, "en-US");
}

/* Windows time zone key -> the IANA name Discord's website sends. */
static const char *iana_zone(const wchar_t *key)
{
    static const struct {
        const wchar_t *win;
        const char *iana;
    } zones[] = {
        {L"Romance Standard Time", "Europe/Paris"},
        {L"Central Europe Standard Time", "Europe/Budapest"},
        {L"Central European Standard Time", "Europe/Warsaw"},
        {L"W. Europe Standard Time", "Europe/Berlin"},
        {L"GMT Standard Time", "Europe/London"},
        {L"Greenwich Standard Time", "Atlantic/Reykjavik"},
        {L"E. Europe Standard Time", "Europe/Chisinau"},
        {L"FLE Standard Time", "Europe/Kiev"},
        {L"GTB Standard Time", "Europe/Bucharest"},
        {L"Russian Standard Time", "Europe/Moscow"},
        {L"Turkey Standard Time", "Europe/Istanbul"},
        {L"Israel Standard Time", "Asia/Jerusalem"},
        {L"Egypt Standard Time", "Africa/Cairo"},
        {L"South Africa Standard Time", "Africa/Johannesburg"},
        {L"Morocco Standard Time", "Africa/Casablanca"},
        {L"W. Central Africa Standard Time", "Africa/Lagos"},
        {L"Namibia Standard Time", "Africa/Windhoek"},
        {L"Arab Standard Time", "Asia/Riyadh"},
        {L"Arabian Standard Time", "Asia/Dubai"},
        {L"Iran Standard Time", "Asia/Tehran"},
        {L"Arabic Standard Time", "Asia/Baghdad"},
        {L"Pakistan Standard Time", "Asia/Karachi"},
        {L"India Standard Time", "Asia/Kolkata"},
        {L"Sri Lanka Standard Time", "Asia/Colombo"},
        {L"Nepal Standard Time", "Asia/Kathmandu"},
        {L"Bangladesh Standard Time", "Asia/Dhaka"},
        {L"Myanmar Standard Time", "Asia/Yangon"},
        {L"SE Asia Standard Time", "Asia/Bangkok"},
        {L"China Standard Time", "Asia/Shanghai"},
        {L"Taipei Standard Time", "Asia/Taipei"},
        {L"Singapore Standard Time", "Asia/Singapore"},
        {L"W. Australia Standard Time", "Australia/Perth"},
        {L"Tokyo Standard Time", "Asia/Tokyo"},
        {L"Korea Standard Time", "Asia/Seoul"},
        {L"North Asia Standard Time", "Asia/Krasnoyarsk"},
        {L"North Asia East Standard Time", "Asia/Irkutsk"},
        {L"Yakutsk Standard Time", "Asia/Yakutsk"},
        {L"Vladivostok Standard Time", "Asia/Vladivostok"},
        {L"AUS Eastern Standard Time", "Australia/Sydney"},
        {L"E. Australia Standard Time", "Australia/Brisbane"},
        {L"Cen. Australia Standard Time", "Australia/Adelaide"},
        {L"AUS Central Standard Time", "Australia/Darwin"},
        {L"Tasmania Standard Time", "Australia/Hobart"},
        {L"New Zealand Standard Time", "Pacific/Auckland"},
        {L"Fiji Standard Time", "Pacific/Fiji"},
        {L"Tonga Standard Time", "Pacific/Tongatapu"},
        {L"Samoa Standard Time", "Pacific/Apia"},
        {L"Hawaiian Standard Time", "Pacific/Honolulu"},
        {L"Alaskan Standard Time", "America/Anchorage"},
        {L"Pacific Standard Time", "America/Los_Angeles"},
        {L"US Mountain Standard Time", "America/Phoenix"},
        {L"Mountain Standard Time", "America/Denver"},
        {L"Central Standard Time", "America/Chicago"},
        {L"Eastern Standard Time", "America/New_York"},
        {L"SA Pacific Standard Time", "America/Bogota"},
        {L"SA Western Standard Time", "America/La_Paz"},
        {L"Pacific SA Standard Time", "America/Santiago"},
        {L"SA Eastern Standard Time", "America/Cayenne"},
        {L"Argentina Standard Time", "America/Buenos_Aires"},
        {L"E. South America Standard Time", "America/Sao_Paulo"},
        {L"Bahia Standard Time", "America/Bahia"},
        {L"Montevideo Standard Time", "America/Montevideo"},
        {L"Atlantic Standard Time", "America/Halifax"},
        {L"Newfoundland Standard Time", "America/St_Johns"},
        {L"Azores Standard Time", "Atlantic/Azores"},
        {L"Cape Verde Standard Time", "Atlantic/Cape_Verde"},
        {L"UTC", "Etc/UTC"},
        {L"UTC-11", "Etc/GMT+11"},
        {L"Dateline Standard Time", "Etc/GMT+12"},
    };

    if (!key || !key[0])
        return NULL;
    for (int i = 0; i < (int)ARRAYSIZE(zones); i++) {
        const wchar_t *a = zones[i].win, *b = key;
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (!*a && !*b)
            return zones[i].iana;
    }
    return NULL;
}

static void timezone_name(sb_t *out)
{
    HKEY key;
    wchar_t name[128] = {0};
    DWORD bytes = sizeof name, type = 0;
    const char *iana = NULL;

    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\TimeZoneInformation", 0, KEY_READ,
                        &key)) {
        if (!RegQueryValueExW(key, L"TimeZoneKeyName", NULL, &type, (BYTE *)name, &bytes) && type == REG_SZ)
            iana = iana_zone(name);
        RegCloseKey(key);
    }
    sb_add(out, iana ? iana : "Etc/UTC");
}

static void uuid_str(char out[40])
{
    unsigned char b[16];
    static const char hex[] = "0123456789abcdef";
    int p = 0;

    if (BCryptGenRandom(NULL, b, sizeof b, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) {
        DWORD t = GetTickCount();
        for (int i = 0; i < 16; i++)
            b[i] = (unsigned char)(t >> ((i & 3) * 8));
    }
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[p++] = '-';
        out[p++] = hex[b[i] >> 4];
        out[p++] = hex[b[i] & 15];
    }
    out[p] = 0;
}

static int header_clean(const char *s)
{
    if (!s)
        return 0;
    for (const char *p = s; *p; p++)
        if (*p == '\r' || *p == '\n')
            return 0;
    return s[0] != 0;
}

static void add_line(sb_t *hdr, const char *name, const char *value)
{
    if (!header_clean(value))
        return;
    sb_add(hdr, name);
    sb_add(hdr, ": ");
    sb_add(hdr, value);
    sb_add(hdr, "\r\n");
}

static void add_accept_language(sb_t *hdr, const char *locale)
{
    const char *dash;

    if (!header_clean(locale))
        locale = "en-US";
    sb_add(hdr, "Accept-Language: ");
    sb_add(hdr, locale);
    dash = locale;
    while (*dash && *dash != '-')
        dash++;
    if (*dash == '-' && dash != locale) {
        sb_add(hdr, ",");
        sb_addn(hdr, locale, (size_t)(dash - locale));
        sb_add(hdr, ";q=0.9");
    }
    sb_add(hdr, "\r\n");
}

static void add_ch_ua(sb_t *hdr, const char *ver)
{
    char major[8];
    size_t i = 0;

    if (!ver)
        return;
    while (ver[i] >= '0' && ver[i] <= '9' && i < 7) {
        major[i] = ver[i];
        i++;
    }
    major[i] = 0;
    if (!i)
        return;
    sb_add(hdr, "sec-ch-ua: \"Chromium\";v=\"");
    sb_add(hdr, major);
    sb_add(hdr, "\", \"Google Chrome\";v=\"");
    sb_add(hdr, major);
    sb_add(hdr, "\", \"Not.A/Brand\";v=\"99\"\r\n");
    sb_add(hdr, "sec-ch-ua-mobile: ?0\r\nsec-ch-ua-platform: \"Windows\"\r\n");
}

static int ci_prefix(const char *s, size_t n, const char *lit)
{
    size_t i = 0;

    for (; lit[i]; i++) {
        char c, d;
        if (i >= n)
            return 0;
        c = s[i];
        d = lit[i];
        if (c >= 'A' && c <= 'Z')
            c += 32;
        if (d >= 'A' && d <= 'Z')
            d += 32;
        if (c != d)
            return 0;
    }
    return 1;
}

static int has_break(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] == '\r' || s[i] == '\n' || s[i] == ';')
            return 1;
    return 0;
}

static void cookie_put(const char *name, size_t nn, const char *value, size_t vn)
{
    if (!nn || has_break(name, nn) || has_break(value, vn))
        return;
    for (int i = 0; i < g_ck_n; i++) {
        if (g_ck[i].name.len == nn) {
            size_t j = 0;
            while (j < nn && g_ck[i].name.data[j] == name[j])
                j++;
            if (j != nn)
                continue;
            sb_free(&g_ck[i].value);
            sb_addn(&g_ck[i].value, value, vn);
            return;
        }
    }
    if (g_ck_n >= COOKIE_MAX)
        return;
    sb_addn(&g_ck[g_ck_n].name, name, nn);
    sb_addn(&g_ck[g_ck_n].value, value, vn);
    g_ck_n++;
}

static void take_cookies(const char *raw, size_t n)
{
    size_t i = 0;

    if (!raw)
        return;
    while (i < n) {
        size_t start = i, len, skip = 11;
        const char *p;
        size_t left, name_n, cut;

        while (i < n && raw[i] != '\n')
            i++;
        len = i - start;
        if (len && raw[start + len - 1] == '\r')
            len--;
        if (i < n)
            i++;
        if (!ci_prefix(raw + start, len, "set-cookie:") || len <= skip)
            continue;
        p = raw + start + skip;
        left = len - skip;
        while (left && (*p == ' ' || *p == '\t')) {
            p++;
            left--;
        }
        name_n = 0;
        while (name_n < left && p[name_n] != '=' && p[name_n] != ';')
            name_n++;
        if (!name_n || name_n >= left || p[name_n] != '=')
            continue;
        cut = 0;
        while (cut < left - name_n - 1 && p[name_n + 1 + cut] != ';')
            cut++;
        cookie_put(p, name_n, p + name_n + 1, cut);
    }
}

static void read_set_cookie(HINTERNET req)
{
    DWORD bytes = 0;
    wchar_t *w;
    sb_t raw = {0};

    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, NULL, &bytes,
                            WINHTTP_NO_HEADER_INDEX) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t))
        return;
    w = mem_alloc(bytes + sizeof(wchar_t));
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, w, &bytes,
                            WINHTTP_NO_HEADER_INDEX)) {
        size_t nw = bytes / sizeof(wchar_t);
        if (nw && w[nw - 1] == 0)
            nw--;
        wide_to_utf8(w, nw, &raw);
    }
    mem_free(w);
    EnterCriticalSection(&g_lock);
    take_cookies(raw.data, raw.len);
    LeaveCriticalSection(&g_lock);
    sb_free(&raw);
}

/* Discord's website headers. The caller adds Authorization, the body type and a captcha. */
static void append_client_headers(sb_t *hdr)
{
    sb_t cookie = {0};

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_ck_n; i++) {
        if (i)
            sb_add(&cookie, "; ");
        sb_addn(&cookie, g_ck[i].name.data, g_ck[i].name.len);
        sb_add(&cookie, "=");
        sb_addn(&cookie, g_ck[i].value.data, g_ck[i].value.len);
    }
    add_line(hdr, "User-Agent", g_ua.data);
    add_line(hdr, "Accept", "*/*");
    add_accept_language(hdr, g_locale.data);
    sb_add(hdr, "Origin: https://discord.com\r\n");
    add_ch_ua(hdr, g_ver.data);
    sb_add(hdr, "Sec-Fetch-Dest: empty\r\nSec-Fetch-Mode: cors\r\nSec-Fetch-Site: same-origin\r\n");
    add_line(hdr, "X-Discord-Locale", g_locale.len ? g_locale.data : "en-US");
    add_line(hdr, "X-Discord-Timezone", g_tz.data);
    sb_add(hdr, "X-Debug-Options: bugReporterEnabled\r\n");
    add_line(hdr, "X-Super-Properties", g_props_b64.data);
    add_line(hdr, "X-Fingerprint", g_fp.data);
    add_line(hdr, "Cookie", cookie.data);
    LeaveCriticalSection(&g_lock);
    sb_free(&cookie);
}

static int do_request(HINTERNET conn, int discord, const wchar_t *referer, const char *method, const char *url,
                      const char *token, const char *body, size_t body_len, const char *ctype, const char *extra,
                      http_resp_t *resp)
{
    sb_t hdr = {0};
    wchar_t *wmethod, *wurl, *whdr;
    HINTERNET req;
    DWORD size = sizeof resp->status;
    DWORD err;
    int ok = 0;

    resp->status = 0;
    sb_free(&resp->body);
    if (discord)
        append_client_headers(&hdr);
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
    if (extra)
        sb_add(&hdr, extra);

    wmethod = utf8_to_wide(method, sc_strlen(method));
    wurl = utf8_to_wide(url, sc_strlen(url));
    whdr = utf8_to_wide(hdr.data ? hdr.data : "", hdr.len);

    req = WinHttpOpenRequest(conn, wmethod, wurl, NULL, referer ? referer : WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (req &&
        (!hdr.len || WinHttpAddRequestHeaders(req, whdr, (DWORD)-1,
                                              WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) &&
        WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)body, (DWORD)body_len, (DWORD)body_len, 0) &&
        WinHttpReceiveResponse(req, NULL) &&
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &resp->status, &size, WINHTTP_NO_HEADER_INDEX)) {
        if (discord)
            read_set_cookie(req);
        ok = read_body(req, &resp->body);
    }
    err = GetLastError();
    stats_add(conn == g_api ? STAT_API : STAT_CDN, resp->body.len);

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

/* 1 when Discord wants a captcha and the user solved it. `headers` then holds X-Captcha-*. */
static int captcha_continue(http_resp_t *resp, sb_t *headers)
{
    props_captcha_t c = {0};
    int again = 0;

    if (!resp->body.data || !props_captcha_read(resp->body.data, resp->body.len, &c)) {
        props_captcha_clear(&c);
        return 0;
    }
    if (c.sitekey.len)
        again = solve_captcha(&c, headers);
    props_captcha_clear(&c);
    return again;
}

static void prepare_identity(void)
{
    sb_t full = {0}, json = {0};
    char launch[40], beat[40];
    props_in_t in = {0};
    http_resp_t page = {0}, exp = {0};
    unsigned build = 0;

    browser_full(&full);
    reduce_version(&full, &g_ver);
    sb_add(&g_ua, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/");
    sb_add(&g_ua, g_ver.data);
    sb_add(&g_ua, " Safari/537.36");
    locale_name(&g_locale);
    timezone_name(&g_tz);

    do_request(g_api, 1, L"https://discord.com/", "GET", "/login", NULL, NULL, 0, NULL, NULL, &page);
    build = props_find_build(page.body.data, page.body.len);
    if (!build)
        build = FALLBACK_BUILD;

    uuid_str(launch);
    uuid_str(beat);
    in.ua = g_ua.data;
    in.browser_version = g_ver.data;
    in.locale = g_locale.data;
    in.launch_id = launch;
    in.heartbeat_id = beat;
    in.build = build;
    props_json(&json, &in);
    props_b64(&g_props_b64, &in);
    sb_addn(&g_props_json, json.data, json.len);

    do_request(g_api, 1, L"https://discord.com/channels/@me", "GET", API_BASE "/experiments", NULL, NULL, 0, NULL, NULL,
               &exp);
    if (exp.status == 200 && exp.body.data) {
        json_t root, v;
        if (json_parse(exp.body.data, exp.body.len, &root) && json_get(root, "fingerprint", &v))
            json_str(v, &g_fp);
    }

    http_resp_free(&page);
    http_resp_free(&exp);
    sb_free(&json);
    sb_free(&full);
}

void http_prepare_client(void)
{
    if (InterlockedCompareExchange(&g_ready, 0, 0))
        return;
    EnterCriticalSection(&g_lock);
    if (g_ready) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    if (g_preparing) {
        LeaveCriticalSection(&g_lock);
        while (!InterlockedCompareExchange(&g_ready, 0, 0))
            Sleep(10);
        return;
    }
    g_preparing = 1;
    /* Released so the requests below can record cookies. */
    LeaveCriticalSection(&g_lock);
    prepare_identity();
    InterlockedExchange(&g_ready, 1);
}

void http_identify_properties(sb_t *out)
{
    http_prepare_client();
    if (g_props_json.len > 1 && g_props_json.data[g_props_json.len - 1] == '}') {
        sb_addn(out, g_props_json.data, g_props_json.len - 1);
        sb_add(out, ",\"client_app_state\":\"focused\",\"is_fast_connect\":false}");
    }
}

int http_ws_headers(wchar_t *dst, size_t cap)
{
    static const wchar_t prefix[] = L"Origin: https://discord.com\r\nUser-Agent: ";
    size_t i = 0;
    const char *ua;

    http_prepare_client();
    if (!dst || cap < 8)
        return 0;
    for (; prefix[i]; i++) {
        if (i + 1 >= cap) {
            dst[0] = 0;
            return 0;
        }
        dst[i] = prefix[i];
    }
    ua = g_ua.data ? g_ua.data : "";
    for (; *ua; ua++) {
        if (i + 1 >= cap) {
            dst[0] = 0;
            return 0;
        }
        dst[i++] = (wchar_t)(unsigned char)*ua;
    }
    if (i + 3 >= cap) {
        dst[0] = 0;
        return 0;
    }
    dst[i++] = L'\r';
    dst[i++] = L'\n';
    dst[i] = 0;
    return 1;
}

int http_request_hdr(const char *method, const char *path, const char *token, const char *content_type,
                     const char *headers, const char *body, size_t body_len, http_resp_t *resp)
{
    sb_t url = {0}, captcha = {0}, extra = {0};
    int ok = 0;

    sb_add(&url, API_BASE);
    sb_add(&url, path);
    http_prepare_client();
    for (int attempt = 0; attempt < CAPTCHA_TRIES; attempt++) {
        sb_clear(&extra);
        if (headers)
            sb_add(&extra, headers);
        if (captcha.len)
            sb_addn(&extra, captcha.data, captcha.len);
        ok = do_request(g_api, 1, L"https://discord.com/channels/@me", method, url.data, token, body, body_len,
                        content_type, extra.len ? extra.data : NULL, resp);
        sb_free(&captcha);
        if (!ok || !captcha_continue(resp, &captcha))
            break;
    }
    sb_free(&captcha);
    sb_free(&extra);
    sb_free(&url);
    return ok;
}

int http_request_type(const char *method, const char *path, const char *token, const char *content_type,
                      const char *body, size_t body_len, http_resp_t *resp)
{
    return http_request_hdr(method, path, token, content_type, NULL, body, body_len, resp);
}

int http_request(const char *method, const char *path, const char *token, const char *body, size_t body_len,
                 http_resp_t *resp)
{
    return http_request_type(method, path, token, NULL, body, body_len, resp);
}

int http_cdn_get(const char *path, http_resp_t *resp)
{
    return do_request(g_cdn, 0, NULL, "GET", path, NULL, NULL, 0, NULL, NULL, resp);
}

int http_get(const wchar_t *host, const char *path, http_resp_t *resp)
{
    HINTERNET conn = WinHttpConnect(g_session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    int ok;

    if (!conn)
        return 0;
    ok = do_request(conn, 0, NULL, "GET", path, NULL, NULL, 0, NULL, NULL, resp);
    WinHttpCloseHandle(conn);
    return ok;
}

void http_resp_free(http_resp_t *resp)
{
    sb_free(&resp->body);
    resp->status = 0;
}
