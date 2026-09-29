#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <string.h>
#include "screen.h"
#include "mem.h"
#include "picture.h"

static struct {
    HANDLE thread, stop, ready;
    HWND wnd;
    int fps, ok;
    screen_frame_fn frame;
    void *ctx;
} g_scr;

/* One duplication of a monitor, and what the capture keeps of it. */
typedef struct {
    ID3D11Device *dev;
    ID3D11DeviceContext *dc;
    IDXGIOutputDuplication *dup;
    ID3D11Texture2D *staging;
    int w, h;
    unsigned *shown;              /* the screen with the pointer drawn on it: what is handed on */
    /* The pointer: its shape, where it is, and the pixels it covers (to put back before it moves). */
    unsigned char *shape;
    UINT shape_cap;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info;
    int have_shape, visible, px, py;
    unsigned *under;
    int under_cap, ux, uy, uw, uh;
} capture_t;

static void release(IUnknown **p)
{
    if (*p) {
        IUnknown_Release(*p);
        *p = NULL;
    }
}

static void close_duplication(capture_t *c)
{
    release((IUnknown **)&c->staging);
    release((IUnknown **)&c->dup);
    release((IUnknown **)&c->dc);
    release((IUnknown **)&c->dev);
}

/* Duplicates the monitor showing wnd, on the adapter it hangs off (the first monitor if none matches). */
static int open_duplication(capture_t *c, HWND wnd)
{
    HMONITOR mon = MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY);
    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL, *chosen_adapter = NULL;
    IDXGIOutput *output = NULL, *chosen = NULL;
    IDXGIOutput1 *output1 = NULL;
    DXGI_OUTDUPL_DESC desc;
    int ok = 0;

    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory)))
        return 0;
    for (UINT a = 0; IDXGIFactory1_EnumAdapters1(factory, a, &adapter) == S_OK; a++) {
        for (UINT o = 0; IDXGIAdapter1_EnumOutputs(adapter, o, &output) == S_OK; o++) {
            DXGI_OUTPUT_DESC od;
            int match = SUCCEEDED(IDXGIOutput_GetDesc(output, &od)) && od.AttachedToDesktop && od.Monitor == mon;
            if (!chosen || match) {
                release((IUnknown **)&chosen);
                release((IUnknown **)&chosen_adapter);
                chosen = output;
                chosen_adapter = adapter;
                IDXGIAdapter1_AddRef(adapter);
                output = NULL;
            } else {
                release((IUnknown **)&output);
            }
            if (match)
                break;
        }
        release((IUnknown **)&adapter);
    }
    release((IUnknown **)&factory);
    if (chosen &&
        SUCCEEDED(D3D11CreateDevice((IDXGIAdapter *)chosen_adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0,
                                    D3D11_SDK_VERSION, &c->dev, NULL, &c->dc)) &&
        SUCCEEDED(IDXGIOutput_QueryInterface(chosen, &IID_IDXGIOutput1, (void **)&output1)) &&
        SUCCEEDED(IDXGIOutput1_DuplicateOutput(output1, (IUnknown *)c->dev, &c->dup))) {
        IDXGIOutputDuplication_GetDesc(c->dup, &desc);
        /* Desktop Duplication hands BGRA 8-bit pictures of the monitor's mode. */
        ok = desc.ModeDesc.Width > 0 && desc.ModeDesc.Height > 0;
    }
    release((IUnknown **)&output1);
    release((IUnknown **)&chosen);
    release((IUnknown **)&chosen_adapter);
    if (!ok)
        close_duplication(c);
    return ok;
}

/* The staging texture the frames are copied into for the CPU, made (again) at the frame's size. */
static int staging_for(capture_t *c, ID3D11Texture2D *frame)
{
    D3D11_TEXTURE2D_DESC d;

    ID3D11Texture2D_GetDesc(frame, &d);
    if (d.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
        return 0;
    if (c->staging && c->w == (int)d.Width && c->h == (int)d.Height)
        return 1;
    release((IUnknown **)&c->staging);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.SampleDesc.Count = 1;
    d.SampleDesc.Quality = 0;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(c->dev, &d, NULL, &c->staging)))
        return 0;
    if (c->w != (int)d.Width || c->h != (int)d.Height) {
        mem_free(c->shown);
        c->w = (int)d.Width;
        c->h = (int)d.Height;
        c->shown = mem_alloc((size_t)c->w * c->h * 4);
    }
    return 1;
}

/* The frame's picture into c->shown, which drops the pointer drawn on the last one. */
static void copy_frame(capture_t *c, IDXGIResource *res)
{
    ID3D11Texture2D *tex = NULL;
    D3D11_MAPPED_SUBRESOURCE m;

    if (FAILED(IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex)))
        return;
    if (staging_for(c, tex)) {
        ID3D11DeviceContext_CopyResource(c->dc, (ID3D11Resource *)c->staging, (ID3D11Resource *)tex);
        if (SUCCEEDED(ID3D11DeviceContext_Map(c->dc, (ID3D11Resource *)c->staging, 0, D3D11_MAP_READ, 0, &m))) {
            for (int y = 0; y < c->h; y++)
                memcpy(c->shown + (size_t)y * c->w, (const unsigned char *)m.pData + (size_t)y * m.RowPitch,
                       (size_t)c->w * 4);
            ID3D11DeviceContext_Unmap(c->dc, (ID3D11Resource *)c->staging, 0);
            c->uw = 0; /* nothing under a pointer to put back */
        }
    }
    ID3D11Texture2D_Release(tex);
}

/* Puts back the pixels the pointer covered. */
static void pointer_hide(capture_t *c)
{
    for (int y = 0; y < c->uh; y++)
        memcpy(c->shown + (size_t)(c->uy + y) * c->w + c->ux, c->under + (size_t)y * c->uw, (size_t)c->uw * 4);
    c->uw = c->uh = 0;
}

/* Draws the pointer, keeping the pixels it covers. */
static void pointer_show(capture_t *c)
{
    int sw = (int)c->shape_info.Width, sh = (int)c->shape_info.Height;
    int rows = c->shape_info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? sh / 2 : sh;
    int x0 = c->px < 0 ? 0 : c->px, y0 = c->py < 0 ? 0 : c->py;
    int x1 = c->px + sw > c->w ? c->w : c->px + sw, y1 = c->py + rows > c->h ? c->h : c->py + rows;

    if (!c->visible || !c->have_shape || !c->shown || x1 <= x0 || y1 <= y0)
        return;
    if (c->under_cap < (x1 - x0) * (y1 - y0)) {
        mem_free(c->under);
        c->under_cap = (x1 - x0) * (y1 - y0);
        c->under = mem_alloc((size_t)c->under_cap * 4);
    }
    c->ux = x0;
    c->uy = y0;
    c->uw = x1 - x0;
    c->uh = y1 - y0;
    for (int y = 0; y < c->uh; y++)
        memcpy(c->under + (size_t)y * c->uw, c->shown + (size_t)(c->uy + y) * c->w + c->ux, (size_t)c->uw * 4);
    picture_draw_cursor(c->shown, c->w, c->h, c->w, c->px, c->py, (int)c->shape_info.Type, c->shape, sw, sh,
                        (int)c->shape_info.Pitch);
}

/* Takes what changed from one frame: the picture, the pointer's place and its shape. Returns whether anything did. */
static int take_frame(capture_t *c, const DXGI_OUTDUPL_FRAME_INFO *info, IDXGIResource *res)
{
    int changed = 0;

    pointer_hide(c);
    if (info->LastPresentTime.QuadPart) {
        copy_frame(c, res);
        changed = 1;
    }
    if (info->LastMouseUpdateTime.QuadPart) {
        c->visible = info->PointerPosition.Visible;
        c->px = info->PointerPosition.Position.x;
        c->py = info->PointerPosition.Position.y;
        changed = 1;
    }
    if (info->PointerShapeBufferSize) {
        UINT need = info->PointerShapeBufferSize, got = 0;
        if (c->shape_cap < need) {
            mem_free(c->shape);
            c->shape = mem_alloc(need);
            c->shape_cap = need;
        }
        c->have_shape = SUCCEEDED(IDXGIOutputDuplication_GetFramePointerShape(c->dup, need, c->shape, &got,
                                                                             &c->shape_info));
        changed = 1;
    }
    pointer_show(c);
    return changed;
}

static DWORD WINAPI screen_main(LPVOID arg)
{
    capture_t c = {0};
    unsigned long long period = (unsigned long long)(1000 / g_scr.fps), next, sent = 0;
    int dirty = 1;

    (void)arg;
    g_scr.ok = open_duplication(&c, g_scr.wnd);
    SetEvent(g_scr.ready);
    next = GetTickCount64();
    while (g_scr.ok && WaitForSingleObject(g_scr.stop, 0) == WAIT_TIMEOUT) {
        unsigned long long now = GetTickCount64();
        DXGI_OUTDUPL_FRAME_INFO info;
        IDXGIResource *res = NULL;
        HRESULT hr;
        if (!c.dup) {
            /* Lost (a mode change, the secure desktop): try again a few times a second. */
            if (WaitForSingleObject(g_scr.stop, 250) != WAIT_TIMEOUT)
                break;
            dirty |= open_duplication(&c, g_scr.wnd);
            continue;
        }
        hr = IDXGIOutputDuplication_AcquireNextFrame(c.dup, next > now ? (UINT)(next - now) : 0, &info, &res);
        if (SUCCEEDED(hr)) {
            dirty |= take_frame(&c, &info, res);
            IDXGIResource_Release(res);
            IDXGIOutputDuplication_ReleaseFrame(c.dup);
        } else if (hr != DXGI_ERROR_WAIT_TIMEOUT) {
            close_duplication(&c);
            continue;
        }
        now = GetTickCount64();
        if (now < next)
            continue;
        /* Changes at most fps times a second, and a still screen once a second. */
        if (c.shown && (dirty || now - sent >= 1000)) {
            g_scr.frame(g_scr.ctx, c.shown, c.w, c.h, c.w, now);
            sent = now;
            dirty = 0;
        }
        next += period;
        if (next < now)
            next = now + period;
    }
    close_duplication(&c);
    mem_free(c.shown);
    mem_free(c.shape);
    mem_free(c.under);
    return 0;
}

int screen_start(HWND wnd, int fps, screen_frame_fn frame, void *ctx)
{
    screen_stop();
    g_scr.wnd = wnd;
    g_scr.fps = fps > 0 ? fps : 15;
    g_scr.frame = frame;
    g_scr.ctx = ctx;
    g_scr.ok = 0;
    g_scr.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_scr.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_scr.thread = CreateThread(NULL, 0, screen_main, NULL, 0, NULL);
    if (g_scr.thread)
        WaitForSingleObject(g_scr.ready, 10000);
    if (!g_scr.ok) {
        screen_stop();
        return 0;
    }
    return 1;
}

void screen_stop(void)
{
    if (g_scr.thread) {
        SetEvent(g_scr.stop);
        WaitForSingleObject(g_scr.thread, INFINITE);
        CloseHandle(g_scr.thread);
        g_scr.thread = NULL;
    }
    if (g_scr.stop) {
        CloseHandle(g_scr.stop);
        CloseHandle(g_scr.ready);
        g_scr.stop = g_scr.ready = NULL;
    }
}
