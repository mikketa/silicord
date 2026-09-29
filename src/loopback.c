#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include "loopback.h"

#define BLOCK 960 /* 20 ms at 48 kHz */

/*
 * AUDIOCLIENT_ACTIVATION_PARAMS for process loopback, and a PROPVARIANT
 * holding it as a blob, laid out by hand: not every SDK has the header,
 * and the variant's unions are named differently from one to the next.
 */
typedef struct {
    int activation_type; /* AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK */
    DWORD process_id;
    int mode; /* PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE */
} activation_params_t;

typedef struct {
    unsigned short vt, reserved[3];
    ULONG size;
    BYTE *data;
} blob_variant_t;

typedef HRESULT(WINAPI *activate_fn)(LPCWSTR path, REFIID riid, PROPVARIANT *params,
                                     IActivateAudioInterfaceCompletionHandler *handler,
                                     IActivateAudioInterfaceAsyncOperation **op);

static const IID k_iid_unknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID k_iid_agile = {0x94EA2B94, 0xE9CC, 0x49E0, {0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90}};
static const IID k_iid_handler = {0x41D949AB, 0x9862, 0x444A, {0x80, 0xF6, 0xC2, 0x61, 0x33, 0x4D, 0xA5, 0xEB}};
static const IID k_iid_audio_client = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID k_iid_capture = {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};

/* The completion handler: activation calls it on a thread of its own, which only sets an event. */
typedef struct {
    IActivateAudioInterfaceCompletionHandler iface;
    HANDLE done;
} handler_t;

static struct {
    HANDLE thread, stop, ready;
    int ok;
    loopback_block_fn block;
    void *ctx;
    handler_t handler;
} g_lb;

static int same_iid(REFIID a, const IID *b)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;

    for (int i = 0; i < (int)sizeof(IID); i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

static HRESULT STDMETHODCALLTYPE handler_query(IActivateAudioInterfaceCompletionHandler *self, REFIID riid, void **out)
{
    /* Agile: activation may call it from any apartment. */
    if (same_iid(riid, &k_iid_unknown) || same_iid(riid, &k_iid_handler) || same_iid(riid, &k_iid_agile)) {
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

/* It lives as long as the module: counting references would change nothing. */
static ULONG STDMETHODCALLTYPE handler_add_ref(IActivateAudioInterfaceCompletionHandler *self)
{
    (void)self;
    return 2;
}

static ULONG STDMETHODCALLTYPE handler_release(IActivateAudioInterfaceCompletionHandler *self)
{
    (void)self;
    return 1;
}

static HRESULT STDMETHODCALLTYPE handler_completed(IActivateAudioInterfaceCompletionHandler *self,
                                                   IActivateAudioInterfaceAsyncOperation *op)
{
    (void)op;
    SetEvent(((handler_t *)self)->done);
    return S_OK;
}

static IActivateAudioInterfaceCompletionHandlerVtbl k_handler_vtbl = {handler_query, handler_add_ref, handler_release,
                                                                     handler_completed};

/* An audio client capturing everything played but by our process tree, or NULL (before Windows 10 2004). */
static IAudioClient *open_client(void)
{
    activate_fn activate = NULL;
    HMODULE lib = LoadLibraryW(L"mmdevapi.dll");
    activation_params_t params = {1, GetCurrentProcessId(), 1};
    blob_variant_t var = {65 /* VT_BLOB */, {0, 0, 0}, sizeof params, (BYTE *)&params};
    IActivateAudioInterfaceAsyncOperation *op = NULL;
    IUnknown *unk = NULL;
    IAudioClient *client = NULL;
    HRESULT result = E_FAIL;

    if (lib)
        activate = (activate_fn)(void *)GetProcAddress(lib, "ActivateAudioInterfaceAsync");
    if (!activate)
        return NULL;
    g_lb.handler.iface.lpVtbl = &k_handler_vtbl;
    g_lb.handler.done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (SUCCEEDED(activate(L"VAD\\Process_Loopback", &k_iid_audio_client, (PROPVARIANT *)&var, &g_lb.handler.iface, &op)) &&
        WaitForSingleObject(g_lb.handler.done, 5000) == WAIT_OBJECT_0 &&
        SUCCEEDED(IActivateAudioInterfaceAsyncOperation_GetActivateResult(op, &result, &unk)) && SUCCEEDED(result) && unk)
        IUnknown_QueryInterface(unk, &k_iid_audio_client, (void **)&client);
    if (unk)
        IUnknown_Release(unk);
    if (op)
        IActivateAudioInterfaceAsyncOperation_Release(op);
    CloseHandle(g_lb.handler.done);
    g_lb.handler.done = NULL;
    return client;
}

static DWORD WINAPI loopback_main(LPVOID arg)
{
    /* 16-bit stereo, as Windows converts to it: process loopback has no mix format of its own. */
    WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0};
    IAudioClient *client;
    IAudioCaptureClient *cap = NULL;
    HANDLE ready = CreateEventW(NULL, FALSE, FALSE, NULL), events[2];
    static float pcm[BLOCK]; /* not on the stack: no __chkstk without the CRT; one loopback thread at a time */
    int n = 0;
    unsigned long long last;

    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    client = open_client();
    g_lb.ok = client &&
              SUCCEEDED(IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
                                                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                                200000, 0, &f, NULL)) &&
              SUCCEEDED(IAudioClient_SetEventHandle(client, ready)) &&
              SUCCEEDED(IAudioClient_GetService(client, &k_iid_capture, (void **)&cap)) &&
              SUCCEEDED(IAudioClient_Start(client));
    SetEvent(g_lb.ready);
    events[0] = g_lb.stop;
    events[1] = ready;
    last = GetTickCount64();
    while (g_lb.ok && WaitForMultipleObjects(2, events, FALSE, 10) != WAIT_OBJECT_0) {
        UINT32 packet = 0;
        unsigned long long now;
        while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(cap, &packet)) && packet) {
            BYTE *data;
            UINT32 frames;
            DWORD flags;
            if (FAILED(IAudioCaptureClient_GetBuffer(cap, &data, &frames, &flags, NULL, NULL)))
                break;
            for (UINT32 i = 0; i < frames; i++) {
                const short *s = (const short *)data + 2 * i;
                pcm[n++] = flags & AUDCLNT_BUFFERFLAGS_SILENT ? 0.f : ((float)s[0] + (float)s[1]) * (1.f / 65536.f);
                if (n == BLOCK) {
                    g_lb.block(g_lb.ctx, pcm);
                    n = 0;
                    last = GetTickCount64();
                }
            }
            IAudioCaptureClient_ReleaseBuffer(cap, frames);
        }
        /* Nothing plays, so nothing comes: silent blocks at the pace of time instead. */
        now = GetTickCount64();
        while (now - last >= 40) {
            for (; n < BLOCK; n++)
                pcm[n] = 0.f;
            g_lb.block(g_lb.ctx, pcm);
            n = 0;
            last += 20;
        }
    }
    if (client)
        IAudioClient_Stop(client);
    if (cap)
        IAudioCaptureClient_Release(cap);
    if (client)
        IAudioClient_Release(client);
    CloseHandle(ready);
    CoUninitialize();
    return 0;
}

int loopback_start(loopback_block_fn block, void *ctx)
{
    loopback_stop();
    g_lb.block = block;
    g_lb.ctx = ctx;
    g_lb.ok = 0;
    g_lb.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_lb.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_lb.thread = CreateThread(NULL, 0, loopback_main, NULL, 0, NULL);
    if (g_lb.thread)
        WaitForSingleObject(g_lb.ready, 10000);
    if (!g_lb.ok) {
        loopback_stop();
        return 0;
    }
    return 1;
}

void loopback_stop(void)
{
    if (g_lb.thread) {
        SetEvent(g_lb.stop);
        WaitForSingleObject(g_lb.thread, INFINITE);
        CloseHandle(g_lb.thread);
        g_lb.thread = NULL;
    }
    if (g_lb.stop) {
        CloseHandle(g_lb.stop);
        CloseHandle(g_lb.ready);
        g_lb.stop = g_lb.ready = NULL;
    }
}
