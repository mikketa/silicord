#define COBJMACROS
#include <windows.h>
#include <dxgi.h>
#include <dwmapi.h>
#include "vsync.h"

static struct {
    HWND wnd;
    UINT msg;
    HANDLE want; /* auto-reset: a frame is asked for; asks before the next refresh fold into one */
} g_vs;

/* The DXGI output showing `monitor`; NULL if none does. */
static IDXGIOutput *find_output(HMONITOR monitor)
{
    IDXGIFactory1 *factory;
    IDXGIAdapter1 *adapter;
    IDXGIOutput *output, *found = NULL;

    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory)))
        return NULL;
    for (UINT a = 0; !found && IDXGIFactory1_EnumAdapters1(factory, a, &adapter) == S_OK; a++) {
        for (UINT o = 0; !found && IDXGIAdapter1_EnumOutputs(adapter, o, &output) == S_OK; o++) {
            DXGI_OUTPUT_DESC d;
            if (SUCCEEDED(IDXGIOutput_GetDesc(output, &d)) && d.Monitor == monitor)
                found = output;
            else
                IDXGIOutput_Release(output);
        }
        IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory1_Release(factory);
    return found;
}

static DWORD WINAPI vsync_main(void *arg)
{
    IDXGIOutput *output = NULL;
    HMONITOR on = NULL;

    (void)arg;
    for (;;) {
        HMONITOR monitor;
        WaitForSingleObject(g_vs.want, INFINITE);
        /* the monitor the window is on now: it may have been moved to another */
        monitor = MonitorFromWindow(g_vs.wnd, MONITOR_DEFAULTTONEAREST);
        if (monitor != on) {
            if (output)
                IDXGIOutput_Release(output);
            output = find_output(monitor);
            on = monitor;
        }
        if (!output || FAILED(IDXGIOutput_WaitForVBlank(output)))
            if (FAILED(DwmFlush()))
                Sleep(4);
        PostMessageW(g_vs.wnd, g_vs.msg, 0, 0);
    }
}

void vsync_start(HWND wnd, UINT msg)
{
    if (g_vs.want)
        return;
    g_vs.wnd = wnd;
    g_vs.msg = msg;
    g_vs.want = CreateEventW(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, vsync_main, NULL, 0, NULL));
}

void vsync_request(void)
{
    if (g_vs.want)
        SetEvent(g_vs.want);
}
