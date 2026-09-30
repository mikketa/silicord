/* Captcha view. hCaptcha rejects a site key unless the page origin is discord.com. */
#include <windows.h>
#include <objbase.h>
#include "cap.h"
#include "mem.h"
#include "sb.h"
#include "utf.h"

static const GUID IID_UNKNOWN = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID IID_ENV_HANDLER = {0x4e8a3389, 0xc9d8, 0x4bd2, {0xb6, 0xb5, 0x12, 0x4f, 0xee, 0x6c, 0xc1, 0x4d}};
static const GUID IID_CTL_HANDLER = {0x6c4819f3, 0xc9b7, 0x4260, {0x81, 0x27, 0xc9, 0xf5, 0xbd, 0xe7, 0xf6, 0x8c}};
static const GUID IID_SCRIPT_HANDLER = {0xb99369f3, 0x9b11, 0x47b5, {0xbc, 0x6f, 0x8e, 0x78, 0x95, 0xfc, 0xea, 0x17}};
static const GUID IID_MSG_HANDLER = {0x57213f19, 0x00e6, 0x49fa, {0x8e, 0x07, 0x89, 0x8e, 0xa0, 0x1e, 0xcb, 0xd2}};
static const GUID IID_ENV = {0xb96d755e, 0x0319, 0x4e92, {0xa2, 0x96, 0x23, 0x43, 0x6f, 0x46, 0xa1, 0xfc}};
static const GUID IID_CTL2 = {0xc979903e, 0xd4ca, 0x4228, {0x92, 0xeb, 0x47, 0xee, 0x3f, 0xa9, 0x6e, 0xab}};
static const GUID IID_CTL3 = {0xf9614724, 0x5d2b, 0x41dc, {0xae, 0xf7, 0x73, 0xd6, 0x2b, 0x51, 0x54, 0x3b}};

typedef struct handler handler_t;
typedef struct {
    HRESULT(STDMETHODCALLTYPE *qi)(handler_t *, const GUID *, void **);
    ULONG(STDMETHODCALLTYPE *add)(handler_t *);
    ULONG(STDMETHODCALLTYPE *rel)(handler_t *);
    HRESULT(STDMETHODCALLTYPE *invoke)(handler_t *, HRESULT, void *);
} handler_vt;
typedef struct {
    HRESULT(STDMETHODCALLTYPE *qi)(handler_t *, const GUID *, void **);
    ULONG(STDMETHODCALLTYPE *add)(handler_t *);
    ULONG(STDMETHODCALLTYPE *rel)(handler_t *);
    HRESULT(STDMETHODCALLTYPE *invoke)(handler_t *, void *, void *);
} msg_vt;

struct handler {
    const void *lpVtbl;
    LONG ref;
    LONG view;
    int kind;
};

static handler_t g_on_env, g_on_ctl, g_on_script, g_on_msg;
static HWND g_parent, g_pump;
static void *g_env, *g_ctl, *g_wv;
static RECT g_bounds;
static int g_dpi = 96;
static LONG g_view;
static int g_in_cb, g_reported, g_kind, g_invisible;
static sb_t g_sitekey, g_rqdata;
static cap_done_fn g_done;
static void *g_ctx;
static HMODULE g_loader;
static int g_ready;

static int guid_eq(const GUID *a, const GUID *b)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;

    for (int i = 0; i < 16; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

static void **vtable(void *obj)
{
    return *(void ***)obj;
}

static void release(void *obj)
{
    if (obj)
        ((ULONG(STDMETHODCALLTYPE *)(void *))vtable(obj)[2])(obj);
}

static void *query(void *obj, const GUID *iid)
{
    void *out = NULL;
    HRESULT hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, const GUID *, void **))vtable(obj)[0])(obj, iid, &out);

    return SUCCEEDED(hr) ? out : NULL;
}

static HRESULT STDMETHODCALLTYPE h_qi(handler_t *self, const GUID *iid, void **out)
{
    const GUID *mine = self->kind == 1 ? &IID_CTL_HANDLER
                       : self->kind == 2 ? &IID_SCRIPT_HANDLER
                       : self->kind == 3 ? &IID_MSG_HANDLER
                                         : &IID_ENV_HANDLER;

    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!guid_eq(iid, &IID_UNKNOWN) && !guid_eq(iid, mine))
        return E_NOINTERFACE;
    InterlockedIncrement(&self->ref);
    *out = self;
    return S_OK;
}

static ULONG STDMETHODCALLTYPE h_add(handler_t *self)
{
    return (ULONG)InterlockedIncrement(&self->ref);
}

static ULONG STDMETHODCALLTYPE h_rel(handler_t *self)
{
    LONG n = InterlockedDecrement(&self->ref);

    /* Static objects. Keep a base reference so a later captcha can reuse them. */
    if (n < 1) {
        InterlockedIncrement(&self->ref);
        return 1;
    }
    return (ULONG)n;
}

static void report(const char *token, const char *err)
{
    cap_done_fn done;
    void *ctx;

    if (g_reported)
        return;
    g_reported = 1;
    done = g_done;
    ctx = g_ctx;
    if (done)
        done(ctx, token, err);
}

static void close_now(void)
{
    void *ctl = g_ctl, *wv = g_wv;

    g_ctl = g_wv = NULL;
    if (ctl)
        ((HRESULT(STDMETHODCALLTYPE *)(void *))vtable(ctl)[24])(ctl);
    release(wv);
    release(ctl);
}

static LRESULT CALLBACK pump_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_APP) {
        if ((LONG)lp == g_view)
            close_now();
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

static void ensure_pump(void)
{
    WNDCLASSEXW wc = {0};

    if (g_pump)
        return;
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = pump_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"SilicordCaptcha";
    RegisterClassExW(&wc);
    g_pump = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
}

void cap_close(void)
{
    if (g_in_cb) {
        ensure_pump();
        if (g_pump)
            PostMessageW(g_pump, WM_APP, 0, (LPARAM)g_view);
        return;
    }
    close_now();
}

static void apply_view(void)
{
    typedef struct {
        BYTE a, r, g, b;
    } color_t;
    void *ctl2, *ctl3;

    if (!g_ctl)
        return;
    ((HRESULT(STDMETHODCALLTYPE *)(void *, BOOL))vtable(g_ctl)[4])(g_ctl, TRUE);
    ((HRESULT(STDMETHODCALLTYPE *)(void *, RECT))vtable(g_ctl)[6])(g_ctl, g_bounds);
    ((HRESULT(STDMETHODCALLTYPE *)(void *))vtable(g_ctl)[23])(g_ctl);
    ctl2 = query(g_ctl, &IID_CTL2);
    if (ctl2) {
        color_t c = {255, 0x0E, 0x0E, 0x0E};
        ((HRESULT(STDMETHODCALLTYPE *)(void *, color_t))vtable(ctl2)[27])(ctl2, c);
        release(ctl2);
    }
    ctl3 = query(g_ctl, &IID_CTL3);
    if (ctl3) {
        /* Let the runtime track the monitor. Passing a scale would pull in the CRT. */
        ((HRESULT(STDMETHODCALLTYPE *)(void *, BOOL))vtable(ctl3)[31])(ctl3, TRUE);
        release(ctl3);
    }
}

void cap_bounds(RECT bounds, int dpi)
{
    g_bounds = bounds;
    if (dpi > 0)
        g_dpi = dpi;
    apply_view();
}

static int eq(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    for (; *a || *b; a++, b++)
        if (*a != *b)
            return 0;
    return 1;
}

static void build_script(sb_t *js)
{
    const char *src = "https://js.hcaptcha.com/1/api.js?onload=__scLoad&render=explicit";

    if (g_kind == 1)
        src = "https://www.google.com/recaptcha/enterprise.js?onload=__scLoad&render=explicit";
    else if (g_kind == 2)
        src = "https://www.google.com/recaptcha/api.js?onload=__scLoad&render=explicit";

    sb_add(js,
           "(function(){\n"
           "if(window.__sc)return;window.__sc=1;\n"
           "function fail(m){try{chrome.webview.postMessage('sc-error:'+m);}catch(e){}}\n"
           "function mount(){\n"
           "var sitekey=");
    sb_json_str(js, g_sitekey.data ? g_sitekey.data : "", g_sitekey.len);
    sb_add(js, ",rq=");
    sb_json_str(js, g_rqdata.data ? g_rqdata.data : "", g_rqdata.len);
    sb_add(js, ",invisible=");
    sb_add(js, g_invisible ? "true" : "false");
    sb_add(js, ",kind=");
    sb_u64(js, (unsigned)g_kind);
    sb_add(js,
           ";\n"
           "document.documentElement.innerHTML='<head><meta charset=\"utf-8\">"
           "<style>html,body{margin:0;height:100%;background:#0e0e0e}"
           "body{display:flex;align-items:center;justify-content:center}</style>"
           "</head><body><div id=\"sc\"></div></body>';\n"
           "var s=document.createElement('script');s.async=true;s.src='");
    sb_add(js, src);
    sb_add(js,
           "';s.onerror=function(){fail('Could not load the captcha.');};\n"
           "window.__scLoad=function(){\n"
           "try{\n"
           "if(kind===0&&window.hcaptcha){\n"
           "var opts={sitekey:sitekey,callback:function(t){chrome.webview.postMessage(t);},"
           "'error-callback':function(){fail('The captcha failed.');},"
           "'expired-callback':function(){fail('The captcha expired.');}};\n"
           "if(rq)opts.rqdata=rq;if(invisible)opts.size='invisible';\n"
           "hcaptcha.render('sc',opts);if(invisible)hcaptcha.execute();\n"
           "}else if(window.grecaptcha&&grecaptcha.enterprise){\n"
           "grecaptcha.enterprise.render('sc',{sitekey:sitekey,callback:function(t){chrome.webview.postMessage(t);}});\n"
           "}else if(window.grecaptcha){\n"
           "grecaptcha.render('sc',{sitekey:sitekey,callback:function(t){chrome.webview.postMessage(t);}});\n"
           "}else fail('The captcha library did not start.');\n"
           "}catch(e){fail(e&&e.message?e.message:'captcha');}\n"
           "};\n"
           "document.head.appendChild(s);\n"
           "}\n"
           "if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',mount);else mount();\n"
           "})();\n");
}

static int wstarts(const wchar_t *s, const wchar_t *p)
{
    for (; *p; s++, p++)
        if (*s != *p)
            return 0;
    return 1;
}

static HRESULT STDMETHODCALLTYPE on_msg(handler_t *self, void *sender, void *args)
{
    LPWSTR text = NULL;
    LPWSTR src = NULL;
    sb_t utf = {0};
    const char *err = NULL;

    (void)sender;
    if (self->view != g_view || !args || g_reported)
        return S_OK;
    ((HRESULT(STDMETHODCALLTYPE *)(void *, LPWSTR *))vtable(args)[3])(args, &src);
    if (!src || !wstarts(src, L"https://discord.com")) {
        CoTaskMemFree(src);
        return S_OK;
    }
    CoTaskMemFree(src);
    if (FAILED(((HRESULT(STDMETHODCALLTYPE *)(void *, LPWSTR *))vtable(args)[5])(args, &text)) || !text)
        return S_OK;
    {
        size_t n = 0;
        while (text[n])
            n++;
        wide_to_utf8(text, n, &utf);
    }
    CoTaskMemFree(text);
    g_in_cb++;
    if (utf.len >= 9 && utf.data[0] == 's' && utf.data[1] == 'c' && utf.data[2] == '-' && utf.data[3] == 'e' &&
        utf.data[4] == 'r' && utf.data[5] == 'r' && utf.data[6] == 'o' && utf.data[7] == 'r' && utf.data[8] == ':')
        err = utf.data + 9;
    if (err)
        report(NULL, err[0] ? err : "The captcha failed.");
    else if (utf.len && utf.len < 16000)
        report(utf.data, NULL);
    else
        report(NULL, "The captcha returned nothing.");
    g_in_cb--;
    sb_free(&utf);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE on_script(handler_t *self, HRESULT hr, void *id)
{
    (void)id;
    if (self->view != g_view || !g_wv)
        return S_OK;
    if (FAILED(hr)) {
        report(NULL, "Could not prepare the captcha page.");
        return S_OK;
    }
    ((HRESULT(STDMETHODCALLTYPE *)(void *, LPCWSTR))vtable(g_wv)[5])(g_wv, L"https://discord.com/login");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE on_ctl(handler_t *self, HRESULT hr, void *ctl)
{
    sb_t js = {0};
    wchar_t *wjs;
    long long cookie = 0;

    g_in_cb++;
    if (self->view != g_view)
        goto out;
    if (FAILED(hr) || !ctl) {
        report(NULL, "Could not open the captcha view.");
        goto out;
    }
    ((ULONG(STDMETHODCALLTYPE *)(void *))vtable(ctl)[1])(ctl);
    g_ctl = ctl;
    apply_view();
    if (FAILED(((HRESULT(STDMETHODCALLTYPE *)(void *, void **))vtable(ctl)[25])(ctl, &g_wv)) || !g_wv) {
        report(NULL, "Could not open the captcha view.");
        goto out;
    }
    g_on_msg.view = g_view;
    ((HRESULT(STDMETHODCALLTYPE *)(void *, void *, long long *))vtable(g_wv)[34])(g_wv, &g_on_msg, &cookie);
    build_script(&js);
    wjs = utf8_to_wide(js.data, js.len);
    sb_free(&js);
    g_on_script.view = g_view;
    hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, LPCWSTR, void *))vtable(g_wv)[27])(g_wv, wjs, &g_on_script);
    mem_free(wjs);
    if (FAILED(hr))
        report(NULL, "Could not prepare the captcha page.");
out:
    g_in_cb--;
    return S_OK;
}

static void create_controller(void)
{
    g_on_ctl.view = g_view;
    if (!g_env || !g_parent)
        return;
    if (FAILED(((HRESULT(STDMETHODCALLTYPE *)(void *, HWND, void *))vtable(g_env)[3])(g_env, g_parent, &g_on_ctl)))
        report(NULL, "Could not open the captcha view.");
}

static HRESULT STDMETHODCALLTYPE on_env(handler_t *self, HRESULT hr, void *env)
{
    if (self->view != g_view)
        return S_OK;
    if (FAILED(hr) || !env) {
        report(NULL, hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
                         ? "Showing this check needs the Microsoft Edge WebView2 runtime."
                         : "Could not start the captcha view.");
        return S_OK;
    }
    ((ULONG(STDMETHODCALLTYPE *)(void *))vtable(env)[1])(env);
    g_env = env;
    create_controller();
    return S_OK;
}

static int data_dir(wchar_t *out, size_t cap)
{
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", out, (DWORD)cap);

    if (!n || n + 24 >= cap) {
        n = GetTempPathW((DWORD)cap, out);
        if (!n || n >= cap)
            return 0;
    }
    {
        size_t i = 0;
        while (out[i])
            i++;
        if (i && out[i - 1] != L'\\')
            out[i++] = L'\\';
        for (const wchar_t *p = L"Silicord"; *p && i + 1 < cap; p++)
            out[i++] = *p;
        out[i] = 0;
        CreateDirectoryW(out, NULL);
        if (i + 1 < cap)
            out[i++] = L'\\';
        for (const wchar_t *p = L"WebView2"; *p && i + 1 < cap; p++)
            out[i++] = *p;
        out[i] = 0;
        CreateDirectoryW(out, NULL);
    }
    return 1;
}

static HMODULE load_loader(void)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t *slash;

    if (g_loader)
        return g_loader;
    if (n && n < MAX_PATH) {
        slash = path + n;
        while (slash > path && slash[-1] != L'\\' && slash[-1] != L'/')
            slash--;
        if ((size_t)(slash - path) + 20 < MAX_PATH) {
            lstrcpyW(slash, L"WebView2Loader.dll");
            g_loader = LoadLibraryW(path);
        }
    }
    if (!g_loader)
        g_loader = LoadLibraryW(L"WebView2Loader.dll");
    return g_loader;
}

static void init_handlers(void)
{
    static const handler_vt vt_env = {h_qi, h_add, h_rel, on_env};
    static const handler_vt vt_ctl = {h_qi, h_add, h_rel, on_ctl};
    static const handler_vt vt_script = {h_qi, h_add, h_rel, on_script};
    static const msg_vt vt_msg = {h_qi, h_add, h_rel, on_msg};

    if (g_ready)
        return;
    g_ready = 1;
    g_on_env.lpVtbl = &vt_env;
    g_on_env.ref = 1;
    g_on_env.kind = 0;
    g_on_ctl.lpVtbl = &vt_ctl;
    g_on_ctl.ref = 1;
    g_on_ctl.kind = 1;
    g_on_script.lpVtbl = &vt_script;
    g_on_script.ref = 1;
    g_on_script.kind = 2;
    g_on_msg.lpVtbl = &vt_msg;
    g_on_msg.ref = 1;
    g_on_msg.kind = 3;
}

int cap_open(HWND parent, RECT bounds, int dpi, const cap_req_t *req, cap_done_fn done, void *ctx)
{
    typedef HRESULT(STDMETHODCALLTYPE * create_fn)(PCWSTR, PCWSTR, void *, void *);
    create_fn create;
    wchar_t dir[MAX_PATH];
    HRESULT hr;

    const char *service;

    if (!parent || !req || !req->sitekey || !req->sitekey[0])
        return 0;
    service = req->service && req->service[0] ? req->service : "hcaptcha";
    if (!eq(service, "hcaptcha") && !eq(service, "recaptcha") && !eq(service, "recaptcha_enterprise"))
        return 0;
    init_handlers();
    ensure_pump();
    g_view++;
    close_now();
    g_reported = 0;
    g_parent = parent;
    g_done = done;
    g_ctx = ctx;
    g_bounds = bounds;
    if (dpi > 0)
        g_dpi = dpi;
    g_kind = eq(service, "recaptcha_enterprise") ? 1 : eq(service, "recaptcha") ? 2 : 0;
    g_invisible = req->invisible;
    sb_free(&g_sitekey);
    sb_free(&g_rqdata);
    sb_add(&g_sitekey, req->sitekey);
    if (req->rqdata)
        sb_add(&g_rqdata, req->rqdata);
    if (g_sitekey.len > 200 || g_rqdata.len > 8000)
        return 0;

    if (g_env) {
        create_controller();
        return 1;
    }
    if (!load_loader())
        return 0;
    create = (create_fn)GetProcAddress(g_loader, "CreateCoreWebView2EnvironmentWithOptions");
    if (!create || !data_dir(dir, MAX_PATH))
        return 0;
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        return 0;
    g_on_env.view = g_view;
    if (FAILED(create(NULL, dir, NULL, &g_on_env)))
        return 0;
    return 1;
}
