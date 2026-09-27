#include "img.h"
#include "gfx.h"
#include "http.h"
#include "mem.h"

#define WORKERS 3

typedef struct job {
    struct job *next;
    sb_t key;
    sb_t path;
} job_t;

static CRITICAL_SECTION g_lock;
static HANDLE g_wake;
static job_t *g_head, *g_tail;
static HWND g_wnd;
static UINT g_msg;
static volatile LONG g_generation;

static void job_free(job_t *j)
{
    sb_free(&j->key);
    sb_free(&j->path);
    mem_free(j);
}

static job_t *pop(LONG *generation)
{
    job_t *j;

    EnterCriticalSection(&g_lock);
    j = g_head;
    if (j) {
        g_head = j->next;
        if (!g_head)
            g_tail = NULL;
    } else {
        ResetEvent(g_wake);
    }
    *generation = g_generation;
    LeaveCriticalSection(&g_lock);
    return j;
}

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    for (;;) {
        LONG generation;
        job_t *j = pop(&generation);
        http_resp_t resp = {0};
        gfx_image_t *img = NULL;
        sb_t *key;

        if (!j) {
            WaitForSingleObject(g_wake, INFINITE);
            continue;
        }
        if (http_cdn_get(j->path.data, &resp) && resp.status == 200)
            img = gfx_image_load(resp.body.data, resp.body.len);
        http_resp_free(&resp);

        if (generation == g_generation) {
            key = mem_alloc(sizeof *key);
            *key = j->key;
            j->key.data = NULL;
            j->key.len = j->key.cap = 0;
            if (!PostMessageW(g_wnd, g_msg, (WPARAM)img, (LPARAM)key)) {
                gfx_image_free(img);
                sb_free(key);
                mem_free(key);
            }
        } else {
            gfx_image_free(img);
        }
        job_free(j);
    }
}

void img_init(HWND wnd, UINT msg)
{
    g_wnd = wnd;
    g_msg = msg;
    InitializeCriticalSection(&g_lock);
    g_wake = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (int i = 0; i < WORKERS; i++)
        CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
}

void img_request(const char *key, const char *cdn_path)
{
    job_t *j = mem_alloc(sizeof *j);

    sb_add(&j->key, key);
    sb_add(&j->path, cdn_path);
    EnterCriticalSection(&g_lock);
    if (g_tail)
        g_tail->next = j;
    else
        g_head = j;
    g_tail = j;
    SetEvent(g_wake);
    LeaveCriticalSection(&g_lock);
}

void img_clear(void)
{
    job_t *j;

    EnterCriticalSection(&g_lock);
    j = g_head;
    g_head = g_tail = NULL;
    InterlockedIncrement(&g_generation);
    LeaveCriticalSection(&g_lock);
    while (j) {
        job_t *next = j->next;
        job_free(j);
        j = next;
    }
}
