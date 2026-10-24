/*
 * Timings of the code on the paths that cost CPU in the client: the gateway's
 * inflate and JSON, READY into the model, a page of messages and their markdown,
 * and in calls the Opus and VP8 codecs.
 * Not a test: run it by hand on the files tools/make_bench_data.py writes.
 *
 *     bench DATA_DIR
 */
#include <windows.h>
#include <string.h>
#include "test.h"
#include "inflate.h"
#include "json.h"
#include "md.h"
#include "mem.h"
#include "model.h"
#include "msg.h"
#include "opus.h"
#include "vp8.h"
#include "vp8_enc.h"

static LARGE_INTEGER g_freq;

static long long now_us(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000000 / g_freq.QuadPart;
}

static int load(const char *dir, const char *name, sb_t *out)
{
    char path[MAX_PATH];
    HANDLE f;
    DWORD got;
    LARGE_INTEGER size;

    wsprintfA(path, "%s\\%s", dir, name);
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE || !GetFileSizeEx(f, &size))
        return 0;
    sb_reserve(out, (size_t)size.QuadPart + 1);
    ReadFile(f, out->data, (DWORD)size.QuadPart, &got, NULL);
    CloseHandle(f);
    out->len = got;
    out->data[got] = 0;
    return 1;
}

/* Prints the best of the runs: what the code costs once caches are warm. */
static void report(const char *name, long long best_us, size_t bytes)
{
    char line[160];

    if (bytes && best_us)
        wsprintfA(line, "%-28s %8d us  %6d MB/s\r\n", name, (int)best_us, (int)((long long)bytes / best_us));
    else
        wsprintfA(line, "%-28s %8d us\r\n", name, (int)best_us);
    out(line);
}

/* With BENCH_ONLY set to the start of a name, only those lines run, and longer: to profile them. */
static char g_only[64];

static int selected(const char *name)
{
    int i = 0;
    while (g_only[i] && g_only[i] == name[i])
        i++;
    return !g_only[i];
}

#define RUNS (g_only[0] ? 400 : 15)
#define TIME(name, bytes, ...)                           \
    do {                                                 \
        long long best_ = 0x7FFFFFFFFFFFFFFF;            \
        if (!selected(name))                             \
            break;                                       \
        for (int run_ = 0; run_ < RUNS; run_++) {        \
            long long t0_ = now_us();                    \
            __VA_ARGS__;                                      \
            long long dt_ = now_us() - t0_;              \
            if (dt_ < best_)                             \
                best_ = dt_;                             \
        }                                                \
        report(name, best_, bytes);                      \
    } while (0)

static inflate_t g_z;

/* The gateway's messages, framed by their lengths: compacts z to the bare stream and returns where each ends. */
static int split(sb_t *z, size_t *ends, int max)
{
    const unsigned char *p = (const unsigned char *)z->data;
    size_t at = 0, len = 0;
    int n = 0;

    while (at + 4 <= z->len && n < max) {
        size_t k = p[at] | (size_t)p[at + 1] << 8 | (size_t)p[at + 2] << 16 | (size_t)p[at + 3] << 24;
        if (at + 4 + k > z->len)
            break;
        memmove(z->data + len, z->data + at + 4, k);
        at += 4 + k;
        len += k;
        ends[n++] = len;
    }
    z->len = len;
    return n;
}

static volatile size_t g_sink;

static void bench_gateway(sb_t *z)
{
    static size_t ends[4096];
    int n = split(z, ends, 4096);
    sb_t json = {0};
    size_t total = 0;

    for (int i = 0; i < n; i++) {
        size_t from = i ? ends[i - 1] : 0;
        json.len = 0;
        if (!i)
            inflate_init(&g_z);
        inflate_message(&g_z, (const unsigned char *)z->data + from, ends[i] - from, &json);
        total += json.len;
    }
    {
        /* The output's hash, to compare builds: the same input must give the same bytes. */
        unsigned h = 2166136261u;
        char line[64];
        inflate_init(&g_z);
        for (int i = 0; i < n; i++) {
            size_t from = i ? ends[i - 1] : 0;
            json.len = 0;
            inflate_message(&g_z, (const unsigned char *)z->data + from, ends[i] - from, &json);
            for (size_t k = 0; k < json.len; k++)
                h = (h ^ (unsigned char)json.data[k]) * 16777619u;
        }
        wsprintfA(line, "inflated %d bytes, hash %08x\r\n", (int)total, h);
        out(line);
    }
    TIME("inflate gateway", total, {
        inflate_init(&g_z);
        for (int i = 0; i < n; i++) {
            size_t from = i ? ends[i - 1] : 0;
            json.len = 0;
            inflate_message(&g_z, (const unsigned char *)z->data + from, ends[i] - from, &json);
        }
    });

    /* What gw.c does with each message: parse, then op, d, s and t. Events after READY only. */
    {
        sb_t all = {0};
        size_t *at = mem_alloc(sizeof *at * (size_t)(n + 1));
        size_t events = 0;
        inflate_init(&g_z);
        for (int i = 0; i < n; i++) {
            size_t from = i ? ends[i - 1] : 0;
            at[i] = all.len;
            inflate_message(&g_z, (const unsigned char *)z->data + from, ends[i] - from, &all);
        }
        at[n] = all.len;
        events = all.len - at[1];
        TIME("events: parse + dispatch", events, {
            for (int i = 1; i < n; i++) {
                json_t root, op, d, s, t;
                if (json_parse(all.data + at[i], at[i + 1] - at[i], &root) && json_get(root, "op", &op)) {
                    json_get(root, "d", &d);
                    json_get(root, "s", &s);
                    json_get(root, "t", &t);
                    g_sink += (size_t)(t.end - t.p) + (size_t)(d.end - d.p);
                }
            }
        });
        TIME("events: MESSAGE_CREATE", 0, {
            for (int i = 1; i < n; i++) {
                json_t root, d, t;
                if (json_parse(all.data + at[i], at[i + 1] - at[i], &root) && json_get(root, "t", &t) &&
                    json_str_eq(t, "MESSAGE_CREATE") && json_get(root, "d", &d)) {
                    msg_batch_t *b = msg_batch_one(d, BATCH_NEW);
                    g_sink += (size_t)b->n;
                    msg_batch_free(b);
                }
            }
        });
        mem_free(at);
        sb_free(&all);
    }
    sb_free(&json);
}

static void bench_ready(const sb_t *ready)
{
    json_t root, d, users;

    TIME("READY: json_parse", ready->len, json_parse(ready->data, ready->len, &root));
    if (!json_parse(ready->data, ready->len, &root) || !json_get(root, "d", &d))
        return;
    TIME("READY: model_from_ready", 0, model_free(model_from_ready(d)));
    TIME("READY: json_get(d, users)", 0, g_sink += (size_t)json_get(d, "users", &users));
    /* main.c indexes READY's users by id in one pass for the relationships, then looks each one up. */
    TIME("READY: users by id", 0, {
        json_iter_t uit;
        json_t u, uid;
        char got[24];
        json_get(d, "users", &users);
        g_sink += json_count(users);
        json_iter(users, &uit);
        while (json_next(&uit, NULL, &u))
            if (json_get(u, "id", &uid)) {
                json_raw(uid, got, sizeof got);
                g_sink += (size_t)got[0];
            }
    });
}

static void bench_messages(const sb_t *page)
{
    json_t root;
    msg_batch_t *b;

    json_parse(page->data, page->len, &root);
    TIME("messages: batch of 50", page->len, msg_batch_free(msg_batch_from_array(root, BATCH_HISTORY, "1", 50)));
    b = msg_batch_from_array(root, BATCH_HISTORY, "1", 50);
    TIME("messages: md_parse x50", 0, {
        for (int i = 0; i < b->n; i++) {
            md_doc_t doc = {0};
            md_parse(b->msgs[i].text.data ? b->msgs[i].text.data : "", b->msgs[i].text.len, &doc);
            g_sink += (size_t)doc.len;
            md_free(&doc);
        }
    });
    msg_batch_free(b);
}

#define FRAMES 50

/* FNV-1a, to compare the output of two builds. */
static unsigned fnv(unsigned h, const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

/* A second of voice-like sound (a few partials and some noise), encoded and decoded 20 ms at a time. */
static void bench_opus(void)
{
    static float pcm[FRAMES][960], decoded[5760];
    static unsigned char packets[FRAMES][1276];
    static int sizes[FRAMES];
    static opus_encoder_t enc;
    static opus_decoder_t dec;
    float c[3] = {1, 1, 1}, s[3] = {0, 0, 0};
    const float kc[3] = {0.99973f, 0.99893f, 0.99760f}, ks[3] = {0.02318f, 0.04632f, 0.06931f}; /* 177, 354, 531 Hz */
    unsigned seed = 1;
    char line[96];
    int total = 0;

    for (int f = 0; f < FRAMES; f++)
        for (int i = 0; i < 960; i++) {
            float v = 0;
            for (int k = 0; k < 3; k++) {
                float nc = c[k] * kc[k] - s[k] * ks[k];
                s[k] = s[k] * kc[k] + c[k] * ks[k];
                c[k] = nc;
                v += s[k] * (0.3f / (float)(k + 1));
            }
            seed = seed * 1103515245u + 12345u;
            pcm[f][i] = v + (float)(int)(seed >> 16 & 0xFF) / 256.0f * 0.02f - 0.01f;
        }
    TIME("opus: encode 1 s", 0, {
        opus_encoder_init(&enc);
        for (int f = 0; f < FRAMES; f++)
            sizes[f] = opus_encode(&enc, pcm[f], packets[f]);
    });
    TIME("opus: decode 1 s", 0, {
        opus_decoder_init(&dec, 1);
        for (int f = 0; f < FRAMES; f++)
            total += opus_decode(&dec, packets[f], (size_t)sizes[f], decoded, 960);
    });
    {
        unsigned hp = 2166136261u, hd = 2166136261u;
        opus_decoder_init(&dec, 1);
        for (int f = 0; f < FRAMES; f++) {
            int n = opus_decode(&dec, packets[f], (size_t)sizes[f], decoded, 960);
            hp = fnv(hp, packets[f], (size_t)sizes[f]);
            hd = fnv(hd, (const unsigned char *)decoded, n > 0 ? (size_t)n * sizeof *decoded : 0);
        }
        wsprintfA(line, "  (%d samples; packets hash %08x, decoded hash %08x)\r\n", total, hp, hd);
    }
    if (!g_only[0])
        out(line);
}

#define VW 1280
#define VH 720
#define VFRAMES 15

/* A second of 720p at 15 fps: a gradient with moving blocks, the kind of picture a shared screen sends. */
static void bench_vp8(void)
{
    unsigned char *planes = mem_alloc((size_t)VFRAMES * VW * VH * 3 / 2);
    vp8_image_t img[VFRAMES];
    sb_t frames[VFRAMES] = {0};
    vp8_decoder_t *dec;
    vp8_image_t shown;
    unsigned hs = 2166136261u, hd = 2166136261u;
    char line[96];

    for (int f = 0; f < VFRAMES; f++) {
        unsigned char *y = planes + (size_t)f * VW * VH * 3 / 2, *u = y + VW * VH, *v = u + VW * VH / 4;
        img[f] = (vp8_image_t){VW, VH, y, u, v, VW, VW / 2};
        for (int r = 0; r < VH; r++)
            for (int x = 0; x < VW; x++) {
                int block = ((x + f * 24) / 80 + r / 60) & 1;
                y[r * VW + x] = (unsigned char)(block ? 200 - r / 8 : 40 + x / 16);
            }
        for (int i = 0; i < VW * VH / 4; i++) {
            u[i] = (unsigned char)(128 + (i % 640) / 20);
            v[i] = (unsigned char)(128 - (i / 640) / 12);
        }
    }
    TIME("vp8: encode 1 s (720p15)", 0, {
        vp8_encoder_t *enc = vp8_encoder_new(VW, VH, 2500, 15);
        for (int f = 0; f < VFRAMES; f++) {
            frames[f].len = 0;
            vp8_encode(enc, &img[f], f == 0, &frames[f]);
        }
        vp8_encoder_free(enc);
    });
    /* A screen nobody touches: the same picture again and again. */
    TIME("vp8: encode still 1 s", 0, {
        vp8_encoder_t *enc = vp8_encoder_new(VW, VH, 2500, 15);
        sb_t still = {0};
        for (int f = 0; f < VFRAMES; f++) {
            still.len = 0;
            vp8_encode(enc, &img[0], f == 0, &still);
        }
        sb_free(&still);
        vp8_encoder_free(enc);
    });
    TIME("vp8: decode 1 s (720p15)", 0, {
        dec = vp8_decoder_new();
        for (int f = 0; f < VFRAMES; f++)
            vp8_decode(dec, (const unsigned char *)frames[f].data, frames[f].len, &shown);
        vp8_decoder_free(dec);
    });
    /* The streams' and the pictures' hashes, to compare builds. */
    {
        vp8_encoder_t *enc = vp8_encoder_new(VW, VH, 2500, 15);
        sb_t still = {0};
        for (int f = 0; f < VFRAMES; f++) {
            still.len = 0;
            vp8_encode(enc, &img[0], f == 0, &still);
            hs = fnv(hs, (const unsigned char *)still.data, still.len);
        }
        sb_free(&still);
        vp8_encoder_free(enc);
    }
    dec = vp8_decoder_new();
    for (int f = 0; f < VFRAMES; f++) {
        hs = fnv(hs, (const unsigned char *)frames[f].data, frames[f].len);
        if (vp8_decode(dec, (const unsigned char *)frames[f].data, frames[f].len, &shown) == 1)
            for (int r = 0; r < shown.h; r++)
                hd = fnv(hd, shown.y + (size_t)r * shown.y_stride, (size_t)shown.w);
    }
    vp8_decoder_free(dec);
    wsprintfA(line, "  (stream hash %08x, pictures hash %08x)\r\n", hs, hd);
    if (!g_only[0])
        out(line);
    for (int f = 0; f < VFRAMES; f++)
        sb_free(&frames[f]);
    mem_free(planes);
}

void entry(void)
{
    const char *dir = GetCommandLineA();
    sb_t z = {0}, ready = {0}, page = {0};

    /* The last argument, unquoted. */
    for (const char *p = dir; *p; p++)
        if (*p == ' ' && p[1] && p[1] != ' ')
            dir = p + 1;
    if (*dir == '"') {
        static char unq[MAX_PATH];
        int n = 0;
        for (dir++; *dir && *dir != '"' && n < MAX_PATH - 1; dir++)
            unq[n++] = *dir;
        unq[n] = 0;
        dir = unq;
    }
    QueryPerformanceFrequency(&g_freq);
    GetEnvironmentVariableA("BENCH_ONLY", g_only, sizeof g_only);
    SetThreadAffinityMask(GetCurrentThread(), 1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    if (!load(dir, "gateway.z", &z) || !load(dir, "ready.json", &ready) || !load(dir, "messages.json", &page)) {
        out("usage: bench DATA_DIR (made by tools/make_bench_data.py)\r\n");
        ExitProcess(1);
    }
    bench_gateway(&z);
    bench_ready(&ready);
    bench_messages(&page);
    bench_opus();
    bench_vp8();
    ExitProcess(0);
}
