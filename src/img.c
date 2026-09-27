#include "img.h"
#include "render.h"
#include "http.h"
#include "mem.h"

#define WORKERS 3
#define DISK_LIMIT (64ull << 20)

typedef struct job {
    struct job *next;
    sb_t key;
    sb_t path;
    int max_px;
} job_t;

static CRITICAL_SECTION g_lock;
static HANDLE g_wake;
static job_t *g_head, *g_tail;
static HWND g_wnd;
static UINT g_msg;
static volatile LONG g_generation;
static wchar_t g_dir[MAX_PATH]; /* disk cache, empty if unavailable */

static void job_free(job_t *j)
{
    sb_free(&j->key);
    sb_free(&j->path);
    mem_free(j);
}

/* ---- Disk cache ----
 * CDN paths name images by content hash, so a cached file never goes stale.
 * Files are named by a hash of the path; the oldest ones are removed past DISK_LIMIT.
 */

static void cache_file(const char *path, wchar_t *out)
{
    unsigned long long h = 14695981039346656037ull; /* FNV-1a */

    for (const char *p = path; *p; p++)
        h = (h ^ (unsigned char)*p) * 1099511628211ull;
    wsprintfW(out, L"%s\\%08x%08x", g_dir, (unsigned)(h >> 32), (unsigned)h);
}

static int cache_read(const char *path, sb_t *out)
{
    wchar_t file[MAX_PATH + 24];
    HANDLE f;
    DWORD size, got = 0;
    FILETIME now;

    if (!g_dir[0])
        return 0;
    cache_file(path, file);
    f = CreateFileW(file, GENERIC_READ | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    size = GetFileSize(f, NULL);
    if (size != INVALID_FILE_SIZE && size && size < (16u << 20)) {
        sb_reserve(out, size);
        if (ReadFile(f, out->data, size, &got, NULL) && got == size)
            out->len = got;
    }
    /* The write time orders files for eviction: mark it as recently used. */
    GetSystemTimeAsFileTime(&now);
    SetFileTime(f, NULL, NULL, &now);
    CloseHandle(f);
    return out->len > 0;
}

static void cache_write(const char *path, const sb_t *data)
{
    wchar_t file[MAX_PATH + 24];
    HANDLE f;
    DWORD put;

    if (!g_dir[0])
        return;
    cache_file(path, file);
    f = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    if (!WriteFile(f, data->data, (DWORD)data->len, &put, NULL) || put != data->len) {
        CloseHandle(f);
        DeleteFileW(file);
        return;
    }
    CloseHandle(f);
}

typedef struct {
    unsigned long long time, size;
    wchar_t name[20];
} entry_t;

/* Deletes the least recently used files until the cache is back under 90% of DISK_LIMIT. */
static DWORD WINAPI cache_trim(LPVOID arg)
{
    wchar_t pattern[MAX_PATH + 8], file[MAX_PATH + 24];
    WIN32_FIND_DATAW fd;
    unsigned long long total = 0;
    entry_t *e = NULL;
    int n = 0, cap = 0;
    HANDLE find;

    (void)arg;
    wsprintfW(pattern, L"%s\\*", g_dir);
    if ((find = FindFirstFileW(pattern, &fd)) == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || lstrlenW(fd.cFileName) >= 20)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            e = mem_realloc(e, (size_t)cap * sizeof *e);
        }
        e[n].time = ((unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        e[n].size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        lstrcpynW(e[n].name, fd.cFileName, 20);
        total += e[n].size;
        n++;
    } while (FindNextFileW(find, &fd));
    FindClose(find);

    if (total > DISK_LIMIT) {
        /* Shell sort, oldest first. */
        for (int gap = n / 2; gap > 0; gap /= 2)
            for (int i = gap; i < n; i++) {
                entry_t tmp = e[i];
                int k = i;
                for (; k >= gap && e[k - gap].time > tmp.time; k -= gap)
                    e[k] = e[k - gap];
                e[k] = tmp;
            }
        for (int i = 0; i < n && total > DISK_LIMIT / 10 * 9; i++) {
            wsprintfW(file, L"%s\\%s", g_dir, e[i].name);
            if (DeleteFileW(file))
                total -= e[i].size;
        }
    }
    mem_free(e);
    return 0;
}

static void cache_init(void)
{
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", g_dir, MAX_PATH);

    if (!n || n > MAX_PATH - 32) {
        g_dir[0] = 0;
        return;
    }
    lstrcatW(g_dir, L"\\Silicord");
    CreateDirectoryW(g_dir, NULL);
    lstrcatW(g_dir, L"\\images");
    if (!CreateDirectoryW(g_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        g_dir[0] = 0;
        return;
    }
    CloseHandle(CreateThread(NULL, 0, cache_trim, NULL, 0, NULL));
}

/* ---- Workers ---- */

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
        r_image_t *img = NULL;
        sb_t *key;

        if (!j) {
            WaitForSingleObject(g_wake, INFINITE);
            continue;
        }
        if (cache_read(j->path.data, &resp.body))
            img = r_image_decode(resp.body.data, resp.body.len, j->max_px);
        if (!img) {
            http_resp_free(&resp);
            if (http_cdn_get(j->path.data, &resp) && resp.status == 200 &&
                (img = r_image_decode(resp.body.data, resp.body.len, j->max_px)) != NULL)
                cache_write(j->path.data, &resp.body);
        }
        http_resp_free(&resp);

        if (generation == g_generation) {
            key = mem_alloc(sizeof *key);
            *key = j->key;
            j->key.data = NULL;
            j->key.len = j->key.cap = 0;
            if (!PostMessageW(g_wnd, g_msg, (WPARAM)img, (LPARAM)key)) {
                r_image_free(img);
                sb_free(key);
                mem_free(key);
            }
        } else {
            r_image_free(img);
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
    cache_init();
    for (int i = 0; i < WORKERS; i++)
        CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
}

r_image_t *img_cached(const char *cdn_path, int max_px)
{
    sb_t data = {0};
    r_image_t *img = NULL;

    if (cache_read(cdn_path, &data))
        img = r_image_decode(data.data, data.len, max_px);
    sb_free(&data);
    return img;
}

void img_request(const char *key, const char *cdn_path, int max_px)
{
    job_t *j = mem_alloc(sizeof *j);

    sb_add(&j->key, key);
    sb_add(&j->path, cdn_path);
    j->max_px = max_px;
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
