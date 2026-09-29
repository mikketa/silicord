#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include "camera.h"
#include "mem.h"

static struct {
    HANDLE thread, stop, ready;
    int index, w, h, fps, ok;
    camera_frame_fn frame;
    void *ctx;
} g_cam;

/* The video capture devices, which the caller releases. */
static IMFActivate **devices(UINT32 *n)
{
    IMFAttributes *attr = NULL;
    IMFActivate **list = NULL;

    *n = 0;
    if (FAILED(MFCreateAttributes(&attr, 1)))
        return NULL;
    if (FAILED(IMFAttributes_SetGUID(attr, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)) ||
        FAILED(MFEnumDeviceSources(attr, &list, n)))
        list = NULL;
    IMFAttributes_Release(attr);
    return list;
}

static void release_devices(IMFActivate **list, UINT32 n)
{
    for (UINT32 i = 0; i < n; i++)
        IMFActivate_Release(list[i]);
    CoTaskMemFree(list);
}

int camera_list(wchar_t (*names)[CAMERA_NAME], int max)
{
    IMFActivate **list;
    UINT32 n;
    int k = 0;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if ((list = devices(&n)) != NULL) {
        for (UINT32 i = 0; i < n && k < max; i++, k++) {
            UINT32 len;
            names[k][0] = 0;
            IMFActivate_GetString(list[i], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, names[k], CAMERA_NAME, &len);
        }
        release_devices(list, n);
    }
    MFShutdown();
    return k;
}

static IMFSourceReader *open_reader(int index, int w, int h, int fps, int *got_w, int *got_h)
{
    IMFActivate **list;
    IMFMediaSource *source = NULL;
    IMFAttributes *attr = NULL;
    IMFSourceReader *reader = NULL;
    IMFMediaType *type = NULL;
    UINT32 n;
    UINT64 size;

    if (!(list = devices(&n)))
        return NULL;
    if ((UINT32)index < n)
        IMFActivate_ActivateObject(list[index], &IID_IMFMediaSource, (void **)&source);
    release_devices(list, n);
    if (!source)
        return NULL;
    /* Advanced processing lets Windows scale and convert to what we ask. */
    if (SUCCEEDED(MFCreateAttributes(&attr, 1)) &&
        SUCCEEDED(IMFAttributes_SetUINT32(attr, &MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE)))
        MFCreateSourceReaderFromMediaSource(source, attr, &reader);
    if (attr)
        IMFAttributes_Release(attr);
    IMFMediaSource_Release(source);
    if (!reader)
        return NULL;
    if (FAILED(MFCreateMediaType(&type)) || FAILED(IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video)) ||
        FAILED(IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, &MFVideoFormat_NV12)) ||
        FAILED(IMFMediaType_SetUINT64(type, &MF_MT_FRAME_SIZE, (UINT64)w << 32 | (UINT64)h)) ||
        FAILED(IMFMediaType_SetUINT64(type, &MF_MT_FRAME_RATE, (UINT64)fps << 32 | 1)) ||
        FAILED(IMFSourceReader_SetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, type))) {
        if (type)
            IMFMediaType_Release(type);
        IMFSourceReader_Release(reader);
        return NULL;
    }
    IMFMediaType_Release(type);
    /* The size we actually get. */
    if (SUCCEEDED(IMFSourceReader_GetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type))) {
        if (SUCCEEDED(IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &size))) {
            *got_w = (int)(size >> 32);
            *got_h = (int)(size & 0xFFFFFFFF);
        }
        IMFMediaType_Release(type);
    }
    return reader;
}

static DWORD WINAPI camera_main(LPVOID arg)
{
    IMFSourceReader *reader;
    int w = g_cam.w, h = g_cam.h;
    unsigned char *uv = NULL;

    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    reader = open_reader(g_cam.index, g_cam.w, g_cam.h, g_cam.fps, &w, &h);
    g_cam.ok = reader != NULL;
    SetEvent(g_cam.ready);
    if (reader)
        uv = mem_alloc((size_t)(w + 1) / 2 * (size_t)(h + 1) / 2 * 2);
    while (reader && WaitForSingleObject(g_cam.stop, 0) == WAIT_TIMEOUT) {
        DWORD stream, flags = 0;
        LONGLONG ts;
        IMFSample *sample = NULL;
        IMFMediaBuffer *buf = NULL;
        BYTE *data;
        DWORD len;
        if (FAILED(IMFSourceReader_ReadSample(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts,
                                              &sample)) ||
            (flags & MF_SOURCE_READERF_ENDOFSTREAM))
            break;
        if (!sample)
            continue;
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &buf)) &&
            SUCCEEDED(IMFMediaBuffer_Lock(buf, &data, NULL, &len))) {
            int cw = (w + 1) / 2, ch = (h + 1) / 2;
            if (len >= (DWORD)(w * h + cw * ch * 2)) {
                /* NV12: Y, then U and V interleaved; split the chroma. */
                const BYTE *s = data + w * h;
                vp8_image_t img;
                for (int i = 0; i < cw * ch; i++) {
                    uv[i] = s[2 * i];
                    uv[cw * ch + i] = s[2 * i + 1];
                }
                img.w = w;
                img.h = h;
                img.y = data;
                img.u = uv;
                img.v = uv + cw * ch;
                img.y_stride = w;
                img.uv_stride = cw;
                g_cam.frame(g_cam.ctx, &img, (unsigned long long)ts / 10000);
            }
            IMFMediaBuffer_Unlock(buf);
        }
        if (buf)
            IMFMediaBuffer_Release(buf);
        IMFSample_Release(sample);
    }
    if (reader)
        IMFSourceReader_Release(reader);
    mem_free(uv);
    MFShutdown();
    CoUninitialize();
    return 0;
}

int camera_start(int index, int w, int h, int fps, camera_frame_fn frame, void *ctx)
{
    camera_stop();
    g_cam.index = index;
    g_cam.w = w;
    g_cam.h = h;
    g_cam.fps = fps;
    g_cam.frame = frame;
    g_cam.ctx = ctx;
    g_cam.ok = 0;
    g_cam.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_cam.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_cam.thread = CreateThread(NULL, 0, camera_main, NULL, 0, NULL);
    if (g_cam.thread)
        WaitForSingleObject(g_cam.ready, 10000);
    if (!g_cam.ok) {
        camera_stop();
        return 0;
    }
    return 1;
}

void camera_stop(void)
{
    if (g_cam.thread) {
        SetEvent(g_cam.stop);
        WaitForSingleObject(g_cam.thread, INFINITE);
        CloseHandle(g_cam.thread);
        g_cam.thread = NULL;
    }
    if (g_cam.stop) {
        CloseHandle(g_cam.stop);
        CloseHandle(g_cam.ready);
        g_cam.stop = g_cam.ready = NULL;
    }
}
