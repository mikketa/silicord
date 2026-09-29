#include <winsock2.h>
#include <ws2tcpip.h>
#include <string.h>
#include "voice.h"
#include "dave_session.h"
#include "json.h"
#include "mem.h"
#include "rng.h"
#include "rtp.h"
#include "sb.h"
#include "sc_asm.h"
#include "sha2.h"
#include "vp8_rtp.h"
#include "ws.h"

#define MAX_SPEAKERS 128
#define MAX_VIDEOS 16
#define KEEPALIVE_MS 5000
#define PLI_MS 1000 /* at most one key frame request a second per video */

enum {
    OP_IDENTIFY = 0,
    OP_SELECT_PROTOCOL = 1,
    OP_READY = 2,
    OP_HEARTBEAT = 3,
    OP_SESSION_DESCRIPTION = 4,
    OP_SPEAKING = 5,
    OP_HELLO = 8,
    OP_CLIENTS_CONNECT = 11,
    OP_VIDEO = 12,
    OP_CLIENT_DISCONNECT = 13,
    OP_MEDIA_SINK_WANTS = 15,
    OP_PREPARE_TRANSITION = 21,
    OP_EXECUTE_TRANSITION = 22,
    OP_PREPARE_EPOCH = 24,
};

typedef struct {
    unsigned ssrc;
    unsigned long long user;
} speaker_t;

/* A remote video stream: its SSRCs, and its frame being rebuilt. */
typedef struct {
    unsigned ssrc, rtx_ssrc;
    unsigned long long user;
    unsigned long long pli_at;
    vp8_rtp_t *rtp;
} video_t;

typedef struct {
    ws_t ws;
    CRITICAL_SECTION lock; /* the DAVE session, the UDP sender state and the speakers */
    int init;
    HANDLE thread, heartbeat, udp_thread, stop;
    voice_params_t p;
    voice_events_t ev;
    SOCKET udp;
    struct sockaddr_in server;
    unsigned ssrc;
    unsigned char key[32];
    int have_key, speaking;
    unsigned seq, timestamp;
    unsigned long nonce;
    volatile LONG seq_ack;
    DWORD interval;
    dave_session_t dave;
    speaker_t speakers[MAX_SPEAKERS];
    int nspeakers;
    video_t videos[MAX_VIDEOS];
    int nvideos;
    unsigned video_ssrc, rtx_ssrc; /* ours, from READY */
    unsigned video_seq, picture_id;
    int video_on;
} voice_t;

static voice_t g_voice;

static void state(voice_t *v, int s, const char *text)
{
    if (v->ev.state)
        v->ev.state(v->ev.ctx, s, text);
}

/* The --debug trace: a label and up to two numbers. */
static void vlog(voice_t *v, const char *what, long long a, long long b)
{
    sb_t m = {0};

    if (!v->ev.log)
        return;
    sb_add(&m, "voice: ");
    sb_add(&m, what);
    if (a != -1) {
        sb_add(&m, " ");
        sb_i64(&m, a);
    }
    if (b != -1) {
        sb_add(&m, " ");
        sb_i64(&m, b);
    }
    v->ev.log(v->ev.ctx, m.data);
    sb_free(&m);
}

/* Logs the DAVE session's state when it changes. */
static void dave_trace(voice_t *v)
{
    static int last = -1;
    int now = v->dave.has_group | v->dave.established << 1 | v->dave.has_pending << 2 | (v->dave.version & 15) << 3 |
              (v->dave.protocol & 15) << 7;

    if (now != last) {
        vlog(v, "dave group/established/pending", v->dave.has_group, v->dave.established * 10 + v->dave.has_pending);
        vlog(v, "dave protocol/media version", v->dave.protocol, v->dave.version);
        last = now;
    }
}

static unsigned long long now_ms(void)
{
    return GetTickCount64();
}

/* A snowflake as a JSON string or number. */
static unsigned long long json_u64(json_t j)
{
    char buf[32];
    unsigned long long u = 0;

    json_raw(j, buf, sizeof buf);
    for (const char *c = buf; *c; c++)
        if (*c >= '0' && *c <= '9')
            u = u * 10 + (unsigned)(*c - '0');
        else if (*c == '.')
            break;
    return u;
}

static long long json_num(json_t obj, const char *key)
{
    json_t j;

    return json_get(obj, key, &j) ? (long long)json_u64(j) : -1;
}

/* A missing or odd version means no DAVE. */
static int clamp_version(long long version)
{
    return version > 0 && version < 16 ? (int)version : 0;
}

static void send_text(voice_t *v, sb_t *msg)
{
    ws_send(&v->ws, msg);
    sb_free(msg);
}

/* DAVE's replies: JSON opcodes as text, binary ones as they are. */
static void dave_send(void *ctx, int binary, const void *data, size_t n)
{
    voice_t *v = ctx;

    vlog(v, binary ? "sent binary op" : "sent json", binary && n ? ((const unsigned char *)data)[0] : -1, (long long)n);
    if (binary) {
        ws_send_binary(&v->ws, data, n);
    } else {
        sb_t m = {0};
        sb_addn(&m, data, n);
        send_text(v, &m);
    }
}

static DWORD WINAPI heartbeat_main(LPVOID arg)
{
    voice_t *v = arg;

    while (WaitForSingleObject(v->stop, v->interval) == WAIT_TIMEOUT) {
        sb_t m = {0};
        sb_add(&m, "{\"op\":3,\"d\":{\"t\":");
        sb_u64(&m, now_ms());
        sb_add(&m, ",\"seq_ack\":");
        sb_i64(&m, InterlockedCompareExchange(&v->seq_ack, 0, 0));
        sb_add(&m, "}}");
        send_text(v, &m);
    }
    return 0;
}

static void send_identify(voice_t *v)
{
    sb_t m = {0};

    sb_add(&m, "{\"op\":0,\"d\":{\"server_id\":\"");
    sb_u64(&m, v->p.server_id);
    sb_add(&m, "\",\"user_id\":\"");
    sb_u64(&m, v->p.user_id);
    sb_add(&m, "\",\"session_id\":");
    sb_json_str(&m, v->p.session_id, sc_strlen(v->p.session_id));
    sb_add(&m, ",\"token\":");
    sb_json_str(&m, v->p.token, sc_strlen(v->p.token));
    sb_add(&m, ",\"video\":true,\"streams\":[{\"type\":\"video\",\"rid\":\"100\",\"quality\":100}]");
    sb_add(&m, ",\"max_dave_protocol_version\":1}}");
    send_text(v, &m);
}

/* Op 12 with our stream, active or not. */
static void send_our_video(voice_t *v, int on)
{
    sb_t m = {0};

    sb_add(&m, "{\"op\":12,\"d\":{\"audio_ssrc\":");
    sb_u64(&m, v->ssrc);
    sb_add(&m, ",\"video_ssrc\":");
    sb_u64(&m, on ? v->video_ssrc : 0);
    sb_add(&m, ",\"rtx_ssrc\":");
    sb_u64(&m, on ? v->rtx_ssrc : 0);
    sb_add(&m, ",\"streams\":[");
    if (on) {
        sb_add(&m, "{\"type\":\"video\",\"rid\":\"100\",\"ssrc\":");
        sb_u64(&m, v->video_ssrc);
        sb_add(&m, ",\"rtx_ssrc\":");
        sb_u64(&m, v->rtx_ssrc);
        sb_add(&m, ",\"active\":true,\"quality\":100,\"max_bitrate\":2500000,\"max_framerate\":30,"
                   "\"max_resolution\":{\"type\":\"fixed\",\"width\":1280,\"height\":720}}");
    }
    sb_add(&m, "]}}");
    send_text(v, &m);
}

/* Our video state: none yet, which the server needs before any video flows either way. */
static void send_video_state(voice_t *v)
{
    sb_t m = {0};

    send_our_video(v, 0);
    /* Everyone's video at the best quality. */
    sb_add(&m, "{\"op\":15,\"d\":{\"any\":100}}");
    send_text(v, &m);
}

static void send_speaking(voice_t *v, int on)
{
    sb_t m = {0};

    sb_add(&m, "{\"op\":5,\"d\":{\"speaking\":");
    sb_i64(&m, on ? 1 : 0);
    sb_add(&m, ",\"delay\":0,\"ssrc\":");
    sb_u64(&m, v->ssrc);
    sb_add(&m, "}}");
    send_text(v, &m);
}

/* Opens UDP to the voice server and learns our public address. */
static int discover(voice_t *v, const char *ip, unsigned port, char out_ip[64], unsigned *out_port)
{
    unsigned char req[74], resp[128];
    DWORD timeout = 1000;

    v->udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (v->udp == INVALID_SOCKET)
        return 0;
    memset(&v->server, 0, sizeof v->server);
    v->server.sin_family = AF_INET;
    v->server.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, ip, &v->server.sin_addr) != 1)
        return 0;
    setsockopt(v->udp, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    rtp_discovery_request(v->ssrc, req);
    for (int tries = 0; tries < 5; tries++) {
        int got;
        sendto(v->udp, (const char *)req, sizeof req, 0, (struct sockaddr *)&v->server, sizeof v->server);
        got = recv(v->udp, (char *)resp, sizeof resp, 0);
        if (got > 0 && rtp_discovery_response(resp, (size_t)got, out_ip, out_port))
            return 1;
        if (WaitForSingleObject(v->stop, 0) == WAIT_OBJECT_0)
            return 0;
    }
    return 0;
}

static unsigned long long speaker_user(voice_t *v, unsigned ssrc)
{
    for (int i = 0; i < v->nspeakers; i++)
        if (v->speakers[i].ssrc == ssrc)
            return v->speakers[i].user;
    return 0;
}

static void set_speaker(voice_t *v, unsigned ssrc, unsigned long long user)
{
    int i;

    for (i = 0; i < v->nspeakers && v->speakers[i].user != user; i++)
        ;
    if (i == v->nspeakers) {
        if (v->nspeakers == MAX_SPEAKERS)
            return;
        v->nspeakers++;
    }
    v->speakers[i].ssrc = ssrc;
    v->speakers[i].user = user;
}

/* Forgets a user's video streams (under the lock). */
static void drop_videos(voice_t *v, unsigned long long user)
{
    for (int i = v->nvideos; i-- > 0;)
        if (v->videos[i].user == user) {
            vp8_rtp_free(v->videos[i].rtp);
            mem_free(v->videos[i].rtp);
            v->videos[i] = v->videos[--v->nvideos];
        }
}

static video_t *find_video(voice_t *v, unsigned ssrc, int *rtx)
{
    for (int i = 0; i < v->nvideos; i++) {
        if (v->videos[i].ssrc == ssrc) {
            *rtx = 0;
            return &v->videos[i];
        }
        if (v->videos[i].rtx_ssrc == ssrc) {
            *rtx = 1;
            return &v->videos[i];
        }
    }
    return NULL;
}

/* Asks a video's sender for a key frame, at most once a second. */
static void request_key_frame(voice_t *v, video_t *vid)
{
    unsigned char pli[12];
    sb_t pkt = {0};

    if (now_ms() - vid->pli_at < PLI_MS)
        return;
    vid->pli_at = now_ms();
    rtcp_pli(v->ssrc, vid->ssrc, pli);
    if (rtcp_seal(v->key, pli, sizeof pli, v->nonce++, &pkt))
        sendto(v->udp, pkt.data, (int)pkt.len, 0, (struct sockaddr *)&v->server, sizeof v->server);
    sb_free(&pkt);
}

/* A video packet: into its frame; a finished frame is decrypted and handed on. */
static void video_packet(voice_t *v, const rtp_header_t *h, const unsigned char *p, size_t n, sb_t *frame, sb_t *plain)
{
    video_t *vid;
    int rtx, done = 0, ok = 0;
    unsigned long long user = 0;
    unsigned seq = h->seq;

    EnterCriticalSection(&v->lock);
    vid = find_video(v, h->ssrc, &rtx);
    if (vid && rtx) { /* a retransmission: the original sequence number, then the payload */
        if (n < 2)
            vid = NULL;
        else {
            seq = (unsigned)p[0] << 8 | p[1];
            p += 2;
            n -= 2;
        }
    }
    if (vid) {
        user = vid->user;
        done = vp8_rtp_push(vid->rtp, seq, h->timestamp, h->marker, p, n, frame);
        if (vp8_rtp_lost(vid->rtp))
            request_key_frame(v, vid);
        if (done) {
            sb_clear(plain);
            ok = dave_session_decrypt(&v->dave, user, (const unsigned char *)frame->data, frame->len, plain, now_ms());
            if (!ok)
                request_key_frame(v, vid);
        }
    }
    LeaveCriticalSection(&v->lock);
    if (ok && plain->len && v->ev.video)
        v->ev.video(v->ev.ctx, user, (const unsigned char *)plain->data, plain->len);
}

static DWORD WINAPI udp_main(LPVOID arg)
{
    voice_t *v = arg;
    unsigned char pkt[2048];
    unsigned long long keepalive = now_ms();
    unsigned long counter = 0;
    unsigned seen[32] = {0};
    DWORD timeout = 250;
    sb_t media = {0}, opus = {0}, frame = {0}, plain = {0};

    setsockopt(v->udp, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    while (WaitForSingleObject(v->stop, 0) == WAIT_TIMEOUT) {
        int got = recv(v->udp, (char *)pkt, sizeof pkt, 0);
        if (now_ms() - keepalive >= KEEPALIVE_MS) {
            unsigned char ka[8] = {0};
            for (int i = 0; i < 4; i++)
                ka[i] = (unsigned char)(counter >> (8 * i));
            counter++;
            sendto(v->udp, (const char *)ka, sizeof ka, 0, (struct sockaddr *)&v->server, sizeof v->server);
            keepalive = now_ms();
        }
        if (got > 8 && pkt[1] >= 200 && pkt[1] <= 206) {
            int want = 0;
            EnterCriticalSection(&v->lock);
            if (v->video_on && rtcp_open(v->key, pkt, (size_t)got, &media))
                want = v->video_ssrc && rtcp_key_frame_request((const unsigned char *)media.data, media.len) == v->video_ssrc;
            LeaveCriticalSection(&v->lock);
            if (want && v->ev.key_frame)
                v->ev.key_frame(v->ev.ctx);
            continue;
        }
        if (got > 0) {
            rtp_header_t h;
            unsigned long long user;
            int ok;
            sb_clear(&media);
            if (!rtp_open(v->key, pkt, (size_t)got, &h, &media))
                continue;
            if (h.type != RTP_OPUS) {
                video_packet(v, &h, (const unsigned char *)media.data, media.len, &frame, &plain);
                continue;
            }
            sb_clear(&opus);
            EnterCriticalSection(&v->lock);
            user = speaker_user(v, h.ssrc);
            ok = user && dave_session_decrypt(&v->dave, user, (const unsigned char *)media.data, media.len, &opus,
                                              now_ms());
            LeaveCriticalSection(&v->lock);
            if (ok && opus.len) {
                /* Which Opus modes Discord sends: config (mode, bandwidth, duration) and stereo, once each. */
                unsigned toc = (unsigned char)opus.data[0], config = toc >> 3, stereo = toc >> 2 & 1;
                if (!(seen[config] & 1u << stereo)) {
                    seen[config] |= 1u << stereo;
                    vlog(v, "opus config/stereo", config, stereo);
                }
            }
            if (ok && v->ev.frame)
                v->ev.frame(v->ev.ctx, user, h.seq, (const unsigned char *)opus.data, opus.len);
        }
    }
    sb_free(&media);
    sb_free(&opus);
    sb_free(&frame);
    sb_free(&plain);
    return 0;
}

/* A text message from the voice gateway. */
static int handle(voice_t *v, const sb_t *msg)
{
    json_t root, d, j;
    long long op, seq;

    if (!json_parse(msg->data, msg->len, &root) || !json_get(root, "op", &j))
        return 1;
    op = (long long)json_u64(j);
    vlog(v, "got op", op, (long long)msg->len);
    seq = json_num(root, "seq");
    if (seq >= 0)
        InterlockedExchange(&v->seq_ack, (LONG)seq);
    if (!json_get(root, "d", &d))
        return 1;
    switch (op) {
    case OP_HELLO:
        v->interval = (DWORD)json_num(d, "heartbeat_interval");
        if (v->interval < 1000)
            v->interval = 1000;
        if (!v->heartbeat) /* one per connection: a second would leak and outlive the socket */
            v->heartbeat = CreateThread(NULL, 0, heartbeat_main, v, 0, NULL);
        send_identify(v);
        break;
    case OP_READY: {
        char ip[64], mine[64];
        unsigned port, my_port;
        sb_t m = {0};
        v->ssrc = (unsigned)json_num(d, "ssrc");
        port = (unsigned)json_num(d, "port");
        v->video_ssrc = v->rtx_ssrc = 0;
        if (json_get(d, "streams", &j)) {
            json_iter_t it;
            json_t s;
            json_iter(j, &it);
            while (json_next(&it, NULL, &s) && !v->video_ssrc) {
                long long ssrc = json_num(s, "ssrc"), rtx = json_num(s, "rtx_ssrc");
                if (ssrc > 0) {
                    v->video_ssrc = (unsigned)ssrc;
                    v->rtx_ssrc = rtx > 0 ? (unsigned)rtx : (unsigned)ssrc + 1;
                }
            }
        }
        vlog(v, "our video ssrc", v->video_ssrc, -1);
        if (!json_get(d, "ip", &j))
            return 0;
        json_raw(j, ip, sizeof ip);
        state(v, VOICE_CONNECTING, "Finding a route\xE2\x80\xA6");
        if (!discover(v, ip, port, mine, &my_port))
            return 0;
        sb_add(&m, "{\"op\":1,\"d\":{\"protocol\":\"udp\",\"data\":{\"address\":");
        sb_json_str(&m, mine, sc_strlen(mine));
        sb_add(&m, ",\"port\":");
        sb_u64(&m, my_port);
        sb_add(&m, ",\"mode\":\"aead_aes256_gcm_rtpsize\"},\"codecs\":[");
        sb_add(&m, "{\"name\":\"opus\",\"type\":\"audio\",\"priority\":1000,\"payload_type\":120},");
        sb_add(&m, "{\"name\":\"VP8\",\"type\":\"video\",\"priority\":1000,\"payload_type\":101,");
        sb_add(&m, "\"rtx_payload_type\":102,\"encode\":true,\"decode\":true}]}}");
        send_text(v, &m);
        break;
    }
    case OP_SESSION_DESCRIPTION: {
        json_iter_t it;
        int k = 0;
        if (!json_get(d, "secret_key", &j))
            return 0;
        json_iter(j, &it);
        EnterCriticalSection(&v->lock);
        while (k < 32 && json_next(&it, NULL, &j))
            v->key[k++] = (unsigned char)json_u64(j);
        v->have_key = k == 32;
        vlog(v, "dave_protocol_version", json_num(d, "dave_protocol_version"), -1);
        dave_on_select_protocol_ack(&v->dave, clamp_version(json_num(d, "dave_protocol_version")));
        dave_trace(v);
        LeaveCriticalSection(&v->lock);
        if (!v->have_key)
            return 0;
        if (json_get(d, "video_codec", &j)) {
            char codec[16];
            json_raw(j, codec, sizeof codec);
            vlog(v, codec[0] == 'V' && codec[1] == 'P' && codec[2] == '8' ? "video codec VP8" : "video codec not VP8", -1, -1);
        }
        send_video_state(v);
        v->udp_thread = CreateThread(NULL, 0, udp_main, v, 0, NULL);
        state(v, VOICE_CONNECTED, "Voice connected");
        break;
    }
    case OP_SPEAKING: {
        unsigned long long user = (unsigned long long)json_num(d, "user_id");
        long long ssrc = json_num(d, "ssrc");
        if (user && ssrc >= 0) {
            EnterCriticalSection(&v->lock);
            set_speaker(v, (unsigned)ssrc, user);
            LeaveCriticalSection(&v->lock);
        }
        break;
    }
    case OP_VIDEO: {
        unsigned long long user = (unsigned long long)json_num(d, "user_id");
        json_iter_t it;
        json_t s;
        int on = 0;
        if (!user)
            break;
        EnterCriticalSection(&v->lock);
        drop_videos(v, user);
        if (json_get(d, "streams", &j)) {
            json_iter(j, &it);
            while (json_next(&it, NULL, &s)) {
                long long ssrc = json_num(s, "ssrc"), rtx = json_num(s, "rtx_ssrc");
                json_t a;
                if (ssrc <= 0 || !(json_get(s, "active", &a) && json_type(a) == JSON_TRUE) || v->nvideos == MAX_VIDEOS)
                    continue;
                v->videos[v->nvideos].ssrc = (unsigned)ssrc;
                v->videos[v->nvideos].rtx_ssrc = rtx > 0 ? (unsigned)rtx : (unsigned)ssrc + 1;
                v->videos[v->nvideos].user = user;
                v->videos[v->nvideos].pli_at = 0;
                v->videos[v->nvideos].rtp = mem_alloc(sizeof(vp8_rtp_t));
                v->nvideos++;
                on = 1;
            }
        }
        LeaveCriticalSection(&v->lock);
        vlog(v, on ? "video on" : "video off", (long long)(user % 1000000), -1);
        if (v->ev.video_state)
            v->ev.video_state(v->ev.ctx, user, on);
        break;
    }
    case OP_CLIENTS_CONNECT: {
        unsigned long long users[64];
        int n = 0;
        json_iter_t it;
        if (!json_get(d, "user_ids", &j))
            break;
        json_iter(j, &it);
        while (n < 64 && json_next(&it, NULL, &j))
            users[n++] = json_u64(j);
        EnterCriticalSection(&v->lock);
        dave_on_clients_connect(&v->dave, users, n);
        LeaveCriticalSection(&v->lock);
        break;
    }
    case OP_CLIENT_DISCONNECT: {
        unsigned long long user = (unsigned long long)json_num(d, "user_id");
        EnterCriticalSection(&v->lock);
        dave_on_client_disconnect(&v->dave, user);
        drop_videos(v, user);
        LeaveCriticalSection(&v->lock);
        if (v->ev.left)
            v->ev.left(v->ev.ctx, user);
        break;
    }
    case OP_PREPARE_TRANSITION:
        EnterCriticalSection(&v->lock);
        dave_on_prepare_transition(&v->dave, (int)json_num(d, "transition_id"),
                                   clamp_version(json_num(d, "protocol_version")));
        dave_trace(v);
        LeaveCriticalSection(&v->lock);
        break;
    case OP_EXECUTE_TRANSITION:
        EnterCriticalSection(&v->lock);
        dave_on_execute_transition(&v->dave, (int)json_num(d, "transition_id"));
        dave_trace(v);
        LeaveCriticalSection(&v->lock);
        break;
    case OP_PREPARE_EPOCH:
        EnterCriticalSection(&v->lock);
        dave_on_prepare_epoch(&v->dave, (unsigned long long)json_num(d, "epoch"),
                              clamp_version(json_num(d, "protocol_version")));
        dave_trace(v);
        LeaveCriticalSection(&v->lock);
        break;
    default:
        break;
    }
    return 1;
}

static DWORD WINAPI voice_main(LPVOID arg)
{
    voice_t *v = arg;
    wchar_t host[256];
    char narrow[256];
    int i, binary;
    sb_t msg = {0};

    for (i = 0; i < 255 && v->p.endpoint[i] && v->p.endpoint[i] != ':'; i++)
        narrow[i] = v->p.endpoint[i];
    narrow[i] = 0;
    MultiByteToWideChar(CP_UTF8, 0, narrow, -1, host, 256);
    state(v, VOICE_CONNECTING, "Connecting to voice\xE2\x80\xA6");
    if (!ws_connect(&v->ws, host, L"/?v=8", NULL)) {
        state(v, VOICE_FAILED, "Could not reach the voice server");
        return 0;
    }
    while (WaitForSingleObject(v->stop, 0) == WAIT_TIMEOUT && ws_recv_kind(&v->ws, &msg, &binary)) {
        if (binary) {
            if (msg.len >= 2)
                InterlockedExchange(&v->seq_ack, (LONG)((unsigned char)msg.data[0] << 8 | (unsigned char)msg.data[1]));
            vlog(v, "got binary op", msg.len >= 3 ? (unsigned char)msg.data[2] : -1, (long long)msg.len);
            EnterCriticalSection(&v->lock);
            dave_on_binary(&v->dave, msg.data, msg.len, now_ms());
            dave_trace(v);
            LeaveCriticalSection(&v->lock);
        } else if (!handle(v, &msg)) {
            break;
        }
    }
    /* Ended by the server or a failure, not by voice_stop(). */
    if (WaitForSingleObject(v->stop, 0) == WAIT_TIMEOUT)
        state(v, VOICE_FAILED, "Voice disconnected");
    SetEvent(v->stop);
    ws_shutdown(&v->ws, WS_CLOSE_NORMAL);
    sb_free(&msg);
    return 0;
}

static void cleanup(voice_t *v)
{
    HANDLE threads[3] = {v->heartbeat, v->udp_thread, v->thread};

    for (int i = 0; i < 3; i++)
        if (threads[i]) {
            WaitForSingleObject(threads[i], INFINITE);
            CloseHandle(threads[i]);
        }
    v->heartbeat = v->udp_thread = v->thread = NULL;
    ws_close(&v->ws);
    /* Under the lock: the audio and camera threads may be sending on the socket. */
    EnterCriticalSection(&v->lock);
    if (v->udp != INVALID_SOCKET)
        closesocket(v->udp);
    v->udp = INVALID_SOCKET;
    dave_session_free(&v->dave);
    secure_wipe(v->key, sizeof v->key);
    v->have_key = v->speaking = v->video_on = 0;
    v->nspeakers = 0;
    while (v->nvideos)
        drop_videos(v, v->videos[0].user);
    LeaveCriticalSection(&v->lock);
}

int voice_start(const voice_params_t *p, const voice_events_t *ev)
{
    voice_t *v = &g_voice;
    unsigned char r[6];

    if (!v->init) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa))
            return 0;
        ws_init(&v->ws);
        InitializeCriticalSection(&v->lock);
        v->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
        v->udp = INVALID_SOCKET;
        v->init = 1;
    }
    voice_stop();
    ResetEvent(v->stop);
    v->p = *p;
    v->ev = *ev;
    v->seq = 0;
    v->timestamp = 0;
    v->nonce = 0;
    v->seq_ack = -1;
    /* RTP starts from random sequence numbers and timestamps. */
    if (rng_bytes(r, sizeof r)) {
        v->seq = (unsigned)r[0] << 8 | r[1];
        v->timestamp = (unsigned)r[2] << 24 | (unsigned)r[3] << 16 | (unsigned)r[4] << 8 | r[5];
    }
    dave_session_init(&v->dave, p->user_id, p->channel_id, dave_send, v);
    v->thread = CreateThread(NULL, 0, voice_main, v, 0, NULL);
    return v->thread != NULL;
}

void voice_stop(void)
{
    voice_t *v = &g_voice;

    if (!v->init || !v->thread)
        return;
    SetEvent(v->stop);
    ws_shutdown(&v->ws, WS_CLOSE_NORMAL);
    cleanup(v);
}

static int send_frame(voice_t *v, const unsigned char *opus, size_t n)
{
    sb_t frame = {0}, pkt = {0};
    rtp_header_t h;
    int ok;

    ok = dave_session_encrypt(&v->dave, opus, n, &frame);
    h.seq = v->seq & 0xFFFF;
    h.timestamp = v->timestamp;
    h.ssrc = v->ssrc;
    h.type = RTP_OPUS;
    ok = ok && rtp_seal(v->key, &h, v->nonce, frame.data, frame.len, &pkt);
    if (ok) {
        v->seq++;
        v->timestamp += 960;
        v->nonce++;
        ok = sendto(v->udp, pkt.data, (int)pkt.len, 0, (struct sockaddr *)&v->server, sizeof v->server) > 0;
    }
    sb_free(&frame);
    sb_free(&pkt);
    return ok;
}

int voice_send(const unsigned char *opus, size_t n)
{
    voice_t *v = &g_voice;
    int ok = 0;

    if (!v->init)
        return 0;
    EnterCriticalSection(&v->lock);
    if (v->have_key && v->udp != INVALID_SOCKET) {
        if (!v->speaking) {
            send_speaking(v, 1);
            v->speaking = 1;
        }
        ok = send_frame(v, opus, n);
    }
    LeaveCriticalSection(&v->lock);
    return ok;
}

void voice_quiet(void)
{
    static const unsigned char silence[3] = {0xF8, 0xFF, 0xFE};
    voice_t *v = &g_voice;

    if (!v->init)
        return;
    EnterCriticalSection(&v->lock);
    if (v->have_key && v->speaking) {
        for (int i = 0; i < 5; i++)
            send_frame(v, silence, sizeof silence);
        send_speaking(v, 0);
        v->speaking = 0;
    }
    LeaveCriticalSection(&v->lock);
}

int voice_video_active(int on)
{
    voice_t *v = &g_voice;
    int ok;

    if (!v->init)
        return 0;
    EnterCriticalSection(&v->lock);
    ok = v->have_key && v->video_ssrc;
    if (ok && v->video_on != on) {
        v->video_on = on;
        send_our_video(v, on);
    }
    LeaveCriticalSection(&v->lock);
    return ok;
}

typedef struct {
    voice_t *v;
    unsigned timestamp;
    int ok;
} video_out_t;

/* One packet of our frame: the RID and playout delay extensions Discord expects on video, then sealed. */
static void send_video_packet(void *ctx, const unsigned char *payload, size_t n, int last)
{
    static const unsigned char ext[] = {0xB2, '1', '0', '0', /* 11: rtp-stream-id "100" */
                                        0x62, 0, 0, 0};      /* 6: playout delay, none */
    video_out_t *o = ctx;
    voice_t *v = o->v;
    rtp_header_t h;
    sb_t pkt = {0};

    h.seq = v->video_seq++ & 0xFFFF;
    h.timestamp = o->timestamp;
    h.ssrc = v->video_ssrc;
    h.type = RTP_VP8 | (last ? RTP_MARKER : 0);
    if (rtp_seal_ext(v->key, &h, v->nonce++, ext, sizeof ext, payload, n, &pkt))
        o->ok &= sendto(v->udp, pkt.data, (int)pkt.len, 0, (struct sockaddr *)&v->server, sizeof v->server) > 0;
    else
        o->ok = 0;
    sb_free(&pkt);
}

int voice_video_send(const unsigned char *vp8, size_t n, unsigned timestamp)
{
    voice_t *v = &g_voice;
    video_out_t o = {v, timestamp, 1};
    sb_t frame = {0};

    if (!v->init || !n)
        return 0;
    EnterCriticalSection(&v->lock);
    if (v->have_key && v->video_on && v->udp != INVALID_SOCKET && dave_session_encrypt_vp8(&v->dave, vp8, n, &frame))
        vp8_rtp_packetize((const unsigned char *)frame.data, frame.len, v->picture_id++ & 0x7FFF, 1100, send_video_packet,
                          &o);
    else
        o.ok = 0;
    LeaveCriticalSection(&v->lock);
    sb_free(&frame);
    return o.ok;
}

int voice_privacy_code(char *out, size_t size)
{
    voice_t *v = &g_voice;
    int ok;

    if (!v->init)
        return 0;
    EnterCriticalSection(&v->lock);
    ok = v->dave.version > 0 && dave_authenticator(&v->dave, out, size);
    LeaveCriticalSection(&v->lock);
    return ok;
}
