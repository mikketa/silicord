/* DAVE sessions: three clients and a stand-in voice gateway create the group, grow it, shrink it and exchange media. */
#include "test.h"
#include "dave_session.h"
#include "hpke.h"
#include "mls_crypto.h"
#include "sb.h"
#include "sha2.h"
#include "tls.h"

#define CHANNEL 42ull

/* What a client last sent, by kind. */
typedef struct {
    sb_t key_package, commit_welcome, json;
    int ready, invalid;
} outbox_t;

static void capture(void *ctx, int binary, const void *data, size_t n)
{
    outbox_t *o = ctx;
    const unsigned char *b = data;

    if (binary && n && b[0] == 26) {
        sb_clear(&o->key_package);
        sb_addn(&o->key_package, (const char *)b + 1, n - 1);
    } else if (binary && n && b[0] == 28) {
        sb_clear(&o->commit_welcome);
        sb_addn(&o->commit_welcome, (const char *)b + 1, n - 1);
    } else if (!binary) {
        sb_clear(&o->json);
        sb_addn(&o->json, data, n);
        o->ready += n > 7 && b[6] == '2' && b[7] == '3';
        o->invalid += n > 7 && b[6] == '3' && b[7] == '1';
    }
}

static unsigned char g_gw_priv[32], g_gw_pub[65];
static sb_t g_sender;

/* An external proposal from the gateway for the group at `epoch`. */
static void proposal(unsigned long long epoch, unsigned type, const void *body, size_t n, sb_t *out)
{
    static const unsigned char gid[8] = {0, 0, 0, 0, 0, 0, 0, (unsigned char)CHANNEL};
    sb_t framed = {0}, tbs = {0}, sig = {0};

    tls_vec(&framed, gid, 8);
    tls_u64(&framed, epoch);
    tls_u8(&framed, MLS_SENDER_EXTERNAL);
    tls_u32(&framed, 0);
    tls_varint(&framed, 0);
    tls_u8(&framed, MLS_CONTENT_PROPOSAL);
    tls_u16(&framed, type);
    sb_addn(&framed, body, n);
    tls_u16(&tbs, 1);
    tls_u16(&tbs, MLS_WIRE_PUBLIC);
    sb_addn(&tbs, framed.data, framed.len);
    mls_sign_with_label(g_gw_priv, "FramedContentTBS", tbs.data, tbs.len, &sig);
    tls_u16(out, 1);
    tls_u16(out, MLS_WIRE_PUBLIC);
    sb_addn(out, framed.data, framed.len);
    tls_vec(out, sig.data, sig.len);
    sb_free(&framed);
    sb_free(&tbs);
    sb_free(&sig);
}

static void deliver(dave_session_t *s, unsigned op, const void *body, size_t n)
{
    sb_t m = {0};

    tls_u16(&m, 7);
    tls_u8(&m, op);
    sb_addn(&m, body, n);
    dave_on_binary(s, m.data, m.len, 1000);
    sb_free(&m);
}

/* Opcode 27 with one appended proposal. */
static void propose(dave_session_t *s, const sb_t *msg)
{
    sb_t body = {0};

    tls_u8(&body, 0);
    tls_vec(&body, msg->data, msg->len);
    deliver(s, 27, body.data, body.len);
    sb_free(&body);
}

static void add_proposal(unsigned long long epoch, const outbox_t *joiner, sb_t *out)
{
    proposal(epoch, MLS_PROPOSAL_ADD, joiner->key_package.data, joiner->key_package.len, out);
}

/* Splits opcode 28 into the commit MLSMessage and the welcome after it. */
static int split(const sb_t *cw, sb_t *commit, sb_t *welcome)
{
    tls_reader_t r;
    mls_content_t m;
    unsigned wf;

    tls_reader(&r, cw->data, cw->len);
    if (!mls_message_header(&r, &wf) || !mls_public_read(&m, &r))
        return 0;
    sb_clear(commit);
    sb_clear(welcome);
    sb_addn(commit, cw->data, (size_t)((const char *)r.p - cw->data));
    sb_addn(welcome, (const char *)r.p, (size_t)(r.end - r.p));
    return 1;
}

static void announce(dave_session_t *s, int transition, const sb_t *commit)
{
    sb_t body = {0};

    tls_u16(&body, (unsigned)transition);
    sb_addn(&body, commit->data, commit->len);
    deliver(s, 29, body.data, body.len);
    sb_free(&body);
}

static void welcome_to(dave_session_t *s, int transition, const sb_t *welcome)
{
    sb_t body = {0};

    tls_u16(&body, (unsigned)transition);
    sb_addn(&body, welcome->data, welcome->len);
    deliver(s, 30, body.data, body.len);
    sb_free(&body);
}

static int same_code(const dave_session_t *a, const dave_session_t *b)
{
    char x[40], y[40];

    return dave_authenticator(a, x, sizeof x) && dave_authenticator(b, y, sizeof y) && ct_equal(x, y, 31);
}

/* A frame from `from` that `to` decrypts back. */
static int media(dave_session_t *from, dave_session_t *to)
{
    static const unsigned char opus[] = {0x78, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    sb_t enc = {0}, dec = {0};
    int ok = dave_session_encrypt(from, opus, sizeof opus, &enc) && enc.len > sizeof opus &&
             dave_session_decrypt(to, from->self_id, (const unsigned char *)enc.data, enc.len, &dec, 2000) &&
             dec.len == sizeof opus && ct_equal(dec.data, opus, sizeof opus);

    sb_free(&enc);
    sb_free(&dec);
    return ok;
}

/* A VP8 key frame: its first 10 bytes travel in the clear, the rest encrypted, and it decrypts back. */
static int video(dave_session_t *from, dave_session_t *to)
{
    static const unsigned char vp8[] = {0x50, 0x1d, 0x00, 0x9d, 0x01, 0x2a, 0xb0, 0x00, 0x90, 0x00, 11, 12, 13, 14, 15, 16};
    sb_t enc = {0}, dec = {0};
    int ok = dave_session_encrypt_vp8(from, vp8, sizeof vp8, &enc) && enc.len > sizeof vp8 &&
             ct_equal(enc.data, vp8, 10) && !ct_equal(enc.data + 10, vp8 + 10, 6) &&
             dave_session_decrypt(to, from->self_id, (const unsigned char *)enc.data, enc.len, &dec, 2000) &&
             dec.len == sizeof vp8 && ct_equal(dec.data, vp8, sizeof vp8);

    sb_free(&enc);
    sb_free(&dec);
    return ok;
}

void entry(void)
{
    static dave_session_t a, b, c;
    static outbox_t oa, ob, oc;
    unsigned long long users[3] = {1, 2, 3};
    unsigned char seed[32] = {9}, leaf[4] = {0, 0, 0, 1};
    sb_t msg = {0}, commit = {0}, welcome = {0};
    char code[40];
    int ok;

    hpke_derive_keypair(seed, 32, g_gw_priv, g_gw_pub);
    tls_vec(&g_sender, g_gw_pub, 65);
    tls_u16(&g_sender, 1);
    tls_vec(&g_sender, "voice gateway", 13);

    dave_session_init(&a, 1, CHANNEL, capture, &oa);
    dave_session_init(&b, 2, CHANNEL, capture, &ob);
    dave_session_init(&c, 3, CHANNEL, capture, &oc);
    dave_on_clients_connect(&a, users, 3);
    dave_on_clients_connect(&b, users, 3);
    dave_on_clients_connect(&c, users, 3);
    dave_on_select_protocol_ack(&a, 1);
    dave_on_select_protocol_ack(&b, 1);
    deliver(&a, 25, g_sender.data, g_sender.len);
    deliver(&b, 25, g_sender.data, g_sender.len);
    check(oa.key_package.len && ob.key_package.len && a.has_group && b.has_group, "key packages and local groups");

    /* Group creation: A and B each propose the other; A's commit wins. */
    add_proposal(0, &ob, &msg);
    propose(&a, &msg);
    sb_clear(&msg);
    add_proposal(0, &oa, &msg);
    propose(&b, &msg);
    check(oa.commit_welcome.len && ob.commit_welcome.len, "both commit");
    ok = split(&oa.commit_welcome, &commit, &welcome) && welcome.len;
    announce(&a, 5, &commit);
    announce(&b, 5, &commit);
    welcome_to(&b, 5, &welcome);
    check(ok && a.established && b.established && oa.ready == 1 && ob.ready == 1, "A's group, B welcomed, both ready");
    dave_on_execute_transition(&a, 5);
    dave_on_execute_transition(&b, 5);
    check(same_code(&a, &b) && dave_authenticator(&a, code, sizeof code) && code[29] && !code[30],
          "same epoch authenticator");
    check(media(&a, &b) && media(&b, &a), "media both ways");
    check(video(&a, &b) && video(&b, &a), "VP8 frames keep their header in the clear");

    /* C joins: B's commit wins; A follows it. */
    dave_on_select_protocol_ack(&c, 1);
    deliver(&c, 25, g_sender.data, g_sender.len);
    sb_clear(&msg);
    add_proposal(1, &oc, &msg);
    propose(&a, &msg);
    propose(&b, &msg);
    ok = split(&ob.commit_welcome, &commit, &welcome) && welcome.len;
    announce(&a, 6, &commit);
    announce(&b, 6, &commit);
    welcome_to(&c, 6, &welcome);
    check(ok && c.established && oa.invalid == 0 && same_code(&a, &b) && same_code(&b, &c), "C joins");
    dave_on_execute_transition(&a, 6);
    dave_on_execute_transition(&b, 6);
    dave_on_execute_transition(&c, 6);
    check(media(&c, &a) && media(&a, &c) && media(&b, &c), "media with C");

    /* B leaves: A commits the removal, C follows. */
    dave_on_client_disconnect(&a, 2);
    dave_on_client_disconnect(&c, 2);
    sb_clear(&msg);
    proposal(2, MLS_PROPOSAL_REMOVE, leaf, 4, &msg);
    propose(&a, &msg);
    propose(&c, &msg);
    ok = split(&oa.commit_welcome, &commit, &welcome) && !welcome.len;
    announce(&a, 7, &commit);
    announce(&c, 7, &commit);
    check(ok && same_code(&a, &c) && a.group.tree.nodes[2].present == 0, "B removed");
    dave_on_execute_transition(&a, 7);
    dave_on_execute_transition(&c, 7);
    check(media(&a, &c) && media(&c, &a), "media after the removal");

    /* An add for someone not in the call is refused: no commit follows. */
    {
        static dave_session_t d;
        static outbox_t od;
        dave_session_init(&d, 99, CHANNEL, capture, &od);
        dave_on_select_protocol_ack(&d, 1);
        sb_clear(&oa.commit_welcome);
        sb_clear(&msg);
        add_proposal(3, &od, &msg);
        propose(&a, &msg);
        check(!oa.commit_welcome.len, "strangers are not added");
        dave_session_free(&d);
        sb_free(&od.key_package);
        sb_free(&od.commit_welcome);
        sb_free(&od.json);
    }

    /* A broken commit is flagged and a new key package goes out. */
    {
        int before = oc.invalid;
        sb_clear(&commit);
        sb_addn(&commit, "\0\1\0\1junk", 8);
        announce(&c, 8, &commit);
        check(oc.invalid == before + 1 && oc.key_package.len && !c.established, "invalid commits are reported");
    }

    /* Version 0 passes media through. */
    dave_on_prepare_transition(&a, 0, 0);
    {
        sb_t out = {0};
        check(a.version == 0 && dave_session_encrypt(&a, (const unsigned char *)"abc", 3, &out) && out.len == 3,
              "passthrough at version 0");
        sb_free(&out);
    }

    dave_session_free(&a);
    dave_session_free(&b);
    dave_session_free(&c);
    sb_free(&msg);
    sb_free(&commit);
    sb_free(&welcome);
    sb_free(&g_sender);
    {
        outbox_t *boxes[3] = {&oa, &ob, &oc};
        for (int i = 0; i < 3; i++) {
            sb_free(&boxes[i]->key_package);
            sb_free(&boxes[i]->commit_welcome);
            sb_free(&boxes[i]->json);
        }
    }
    finish();
}
