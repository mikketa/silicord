#include <string.h>
#include "dave_session.h"
#include "mem.h"
#include "sha2.h"
#include "tls.h"

static const char k_media_label[] = "Discord Secure Frames v0";

/* Opcode 26 carries the KeyPackage as libdave sends it, bare; the protocol text shows it in an MLSMessage. */
#define KEY_PACKAGE_IN_MLSMESSAGE 0

/* ---- Keys ---- */

static void keyring_free(dave_keyring_t *k)
{
    for (int i = 0; i < k->n; i++)
        dave_ratchet_wipe(&k->senders[i].ratchet);
    mem_free(k->senders);
    k->senders = NULL;
    k->n = 0;
}

static unsigned long long identity_user(const mls_node_t *leaf)
{
    unsigned long long u = 0;

    for (size_t i = 0; i < 8 && leaf->identity.len == 8; i++)
        u = u << 8 | (unsigned char)leaf->identity.data[i];
    return u;
}

/* A sender's base secret: the exporter with the user id in little-endian. */
static int base_secret(const mls_group_t *g, unsigned long long user, unsigned char out[16])
{
    unsigned char id[8];

    for (int i = 0; i < 8; i++)
        id[i] = (unsigned char)(user >> (8 * i));
    return mls_exporter(g->keys.exporter, k_media_label, sizeof k_media_label - 1, id, 8, out, 16);
}

/* Receivers: every member's ratchet for the group's epoch; the old ones stay a while. */
static void new_keyring(dave_session_t *s, unsigned long long now_ms)
{
    dave_keyring_t k = {0};
    const mls_tree_t *t = &s->group.tree;

    k.senders = mem_alloc(sizeof *k.senders * (t->nleaves + 1));
    for (unsigned i = 0; i < t->nleaves; i++) {
        unsigned char secret[16];
        const mls_node_t *leaf = &t->nodes[2 * i];
        if (!leaf->present || !base_secret(&s->group, identity_user(leaf), secret))
            continue;
        k.senders[k.n].user = identity_user(leaf);
        dave_ratchet_init(&k.senders[k.n++].ratchet, secret);
        secure_wipe(secret, sizeof secret);
    }
    keyring_free(&s->previous);
    s->previous = s->current;
    s->previous_until = now_ms + DAVE_KEEP_OLD_MS;
    s->current = k;
}

/* Our sending ratchet changes when a transition executes. */
static void new_own_key(dave_session_t *s)
{
    unsigned char secret[16];

    dave_ratchet_wipe(&s->own);
    s->has_own = s->established && base_secret(&s->group, s->self_id, secret);
    if (s->has_own)
        dave_ratchet_init(&s->own, secret);
    s->own_nonce = 0;
    secure_wipe(secret, sizeof secret);
}

/* ---- Group state ---- */

static void send_json(dave_session_t *s, int op, int transition_id)
{
    sb_t m = {0};

    sb_add(&m, "{\"op\":");
    sb_i64(&m, op);
    sb_add(&m, ",\"d\":{\"transition_id\":");
    sb_i64(&m, transition_id);
    sb_add(&m, "}}");
    s->send(s->ctx, 0, m.data, m.len);
    sb_free(&m);
}

static void drop_outbound(dave_session_t *s)
{
    if (s->has_outbound)
        mls_group_free(&s->outbound);
    sb_free(&s->outbound_commit);
    s->has_outbound = 0;
}

static void drop_group(dave_session_t *s)
{
    drop_outbound(s);
    if (s->has_group)
        mls_group_free(&s->group);
    s->has_group = s->established = 0;
}

/* Our own group of one, once we know the gateway's external sender. */
static void create_local_group(dave_session_t *s)
{
    sb_t senders = {0}, ext = {0};

    if (s->has_group || !s->has_member || !s->external_sender.len || s->protocol <= 0)
        return;
    tls_vec(&senders, s->external_sender.data, s->external_sender.len);
    tls_u16(&ext, MLS_EXT_EXTERNAL_SENDERS);
    tls_vec(&ext, senders.data, senders.len);
    s->has_group = mls_group_create(&s->group, &s->member, s->group_id, 8, ext.data, ext.len);
    sb_free(&senders);
    sb_free(&ext);
}

/* A fresh KeyPackage (opcode 26) and a fresh local group. */
static void send_key_package(dave_session_t *s)
{
    unsigned char id[8];
    sb_t m = {0};

    drop_group(s);
    if (s->has_member)
        mls_member_free(&s->member);
    for (int i = 0; i < 8; i++)
        id[i] = (unsigned char)(s->self_id >> (56 - 8 * i));
    s->has_member = mls_member_create(&s->member, id, 8);
    if (!s->has_member)
        return;
    tls_u8(&m, 26);
    if (KEY_PACKAGE_IN_MLSMESSAGE) {
        tls_u16(&m, 1);
        tls_u16(&m, MLS_WIRE_KEY_PACKAGE);
    }
    sb_addn(&m, s->member.key_package.data, s->member.key_package.len);
    s->send(s->ctx, 1, m.data, m.len);
    sb_free(&m);
    create_local_group(s);
}

static int recognized(const dave_session_t *s, unsigned long long user)
{
    if (user == s->self_id)
        return 1;
    for (int i = 0; i < s->nusers; i++)
        if (s->users[i] == user)
            return 1;
    return 0;
}

/* Every leaf a distinct, expected user; `strict` also asks that the gateway be the only external sender. */
static int group_valid(const dave_session_t *s, const mls_group_t *g, int strict)
{
    const mls_tree_t *t = &g->tree;

    for (unsigned i = 0; i < t->nleaves; i++) {
        const mls_node_t *a = &t->nodes[2 * i];
        if (!a->present)
            continue;
        if (a->identity.len != 8 || (strict && !recognized(s, identity_user(a))))
            return 0;
        for (unsigned j = 0; j < i; j++)
            if (t->nodes[2 * j].present && identity_user(&t->nodes[2 * j]) == identity_user(a))
                return 0;
    }
    if (strict) {
        mls_bytes_t list, want;
        sb_t one = {0};
        int ok;
        mls_bytes_t exts = {(const unsigned char *)g->extensions.data, g->extensions.len};
        if (g->group_id.len != 8 || !ct_equal(g->group_id.data, s->group_id, 8) ||
            !mls_extension_find(exts, MLS_EXT_EXTERNAL_SENDERS, &list))
            return 0;
        tls_vec(&one, s->external_sender.data, s->external_sender.len);
        want.p = (const unsigned char *)one.data;
        want.n = one.len;
        ok = list.n == want.n && ct_equal(list.p, want.p, want.n);
        sb_free(&one);
        return ok;
    }
    return 1;
}

/* A commit or welcome we cannot use: flag it and start over with a new KeyPackage. */
static void invalid(dave_session_t *s, int transition_id)
{
    send_json(s, 31, transition_id);
    send_key_package(s);
}

static void execute(dave_session_t *s)
{
    s->version = s->pending_version;
    s->has_pending = 0;
    new_own_key(s);
}

/* After a commit or welcome: ready for the transition, or at once for transition 0. */
static void prepare(dave_session_t *s, int transition_id, unsigned long long now_ms)
{
    new_keyring(s, now_ms);
    s->pending_transition = transition_id;
    s->pending_version = s->protocol;
    s->has_pending = 1;
    if (transition_id == 0)
        execute(s);
    else
        send_json(s, 23, transition_id);
}

/* ---- Opcodes ---- */

void dave_session_init(dave_session_t *s, unsigned long long self_id, unsigned long long channel_id,
                       void (*send)(void *ctx, int binary, const void *data, size_t n), void *ctx)
{
    memset(s, 0, sizeof *s);
    s->self_id = self_id;
    for (int i = 0; i < 8; i++)
        s->group_id[i] = (unsigned char)(channel_id >> (56 - 8 * i));
    s->send = send;
    s->ctx = ctx;
}

void dave_session_free(dave_session_t *s)
{
    drop_group(s);
    if (s->has_member)
        mls_member_free(&s->member);
    sb_free(&s->external_sender);
    mem_free(s->users);
    keyring_free(&s->current);
    keyring_free(&s->previous);
    dave_ratchet_wipe(&s->own);
    secure_wipe(s, sizeof *s);
}

void dave_on_select_protocol_ack(dave_session_t *s, int version)
{
    s->protocol = s->version = version;
    if (version > 0)
        send_key_package(s);
}

void dave_on_clients_connect(dave_session_t *s, const unsigned long long *users, int n)
{
    for (int i = 0; i < n; i++)
        if (!recognized(s, users[i])) {
            s->users = mem_realloc(s->users, sizeof *s->users * (size_t)(s->nusers + 1));
            s->users[s->nusers++] = users[i];
        }
}

void dave_on_client_disconnect(dave_session_t *s, unsigned long long user)
{
    for (int i = 0; i < s->nusers; i++)
        if (s->users[i] == user) {
            s->users[i] = s->users[--s->nusers];
            return;
        }
}

void dave_on_prepare_transition(dave_session_t *s, int transition_id, int version, unsigned long long now_ms)
{
    (void)now_ms;
    s->pending_transition = transition_id;
    s->pending_version = version;
    s->has_pending = 1;
    if (transition_id == 0)
        execute(s);
    else
        send_json(s, 23, transition_id);
}

void dave_on_execute_transition(dave_session_t *s, int transition_id, unsigned long long now_ms)
{
    (void)now_ms;
    if (s->has_pending && s->pending_transition == transition_id)
        execute(s);
}

void dave_on_prepare_epoch(dave_session_t *s, unsigned long long epoch, int version)
{
    s->protocol = version;
    if (epoch == 1)
        send_key_package(s);
}

/* Opcode 27: append checked proposals, or revoke some; then commit what is cached. */
static void on_proposals(dave_session_t *s, tls_reader_t *r)
{
    unsigned op = tls_read_u8(r);
    tls_reader_t v = tls_read_nested(r);
    sb_t commit = {0}, welcome = {0}, m = {0};
    int ok = !r->bad && tls_done(r);

    if (!s->has_group)
        return;
    while (ok && !v.bad && v.p < v.end) {
        if (op == 0) {
            const unsigned char *start = v.p;
            mls_content_t c = {0};
            mls_proposal_t p;
            tls_reader_t pr;
            unsigned wf;
            ok = mls_message_header(&v, &wf) && wf == MLS_WIRE_PUBLIC && mls_public_read(&c, &v) &&
                 c.content_type == MLS_CONTENT_PROPOSAL && c.sender_type == MLS_SENDER_EXTERNAL;
            if (!ok)
                break;
            tls_reader(&pr, c.content.p, c.content.n);
            ok = mls_proposal_read(&p, &pr) && (p.type == MLS_PROPOSAL_ADD || p.type == MLS_PROPOSAL_REMOVE);
            if (ok && p.type == MLS_PROPOSAL_ADD) {
                mls_key_package_t kp;
                tls_reader(&pr, p.body.p, p.body.n);
                ok = mls_key_package_read(&kp, &pr);
                if (ok) {
                    ok = kp.leaf.identity.len == 8 && recognized(s, identity_user(&kp.leaf));
                    mls_key_package_free(&kp);
                }
            }
            ok = ok && mls_group_handle(&s->group, start, (size_t)(v.p - start), NULL) == MLS_HANDLED_PROPOSAL;
        } else if (op == 1) {
            size_t n;
            const unsigned char *ref = tls_read_vec(&v, &n);
            ok = !v.bad && mls_group_revoke(&s->group, ref, n);
        } else {
            ok = 0;
        }
    }
    if (!ok || v.bad || !s->group.nprops)
        return;
    drop_outbound(s);
    if (!mls_group_commit(&s->group, &commit, &welcome, &s->outbound))
        goto out;
    s->has_outbound = 1;
    sb_addn(&s->outbound_commit, commit.data, commit.len);
    tls_u8(&m, 28);
    sb_addn(&m, commit.data, commit.len);
    if (welcome.len)
        sb_addn(&m, welcome.data, welcome.len);
    s->send(s->ctx, 1, m.data, m.len);
out:
    sb_free(&commit);
    sb_free(&welcome);
    sb_free(&m);
}

/* Opcode 29: the commit the gateway picked for the epoch. */
static void on_commit(dave_session_t *s, int transition_id, const unsigned char *commit, size_t n,
                      unsigned long long now_ms)
{
    if (s->has_outbound && s->outbound_commit.len == n && ct_equal(s->outbound_commit.data, commit, n)) {
        mls_group_free(&s->group);
        s->group = s->outbound;
        s->has_outbound = 0;
        sb_free(&s->outbound_commit);
        s->established = 1;
    } else if (s->established) {
        int r = mls_group_handle(&s->group, commit, n, NULL);
        drop_outbound(s);
        if (r == MLS_HANDLED_REMOVED) {
            drop_group(s);
            return;
        }
        if (r != MLS_HANDLED_COMMIT) {
            invalid(s, transition_id);
            return;
        }
    } else {
        return; /* someone else's first commit: a welcome follows */
    }
    if (!group_valid(s, &s->group, 0)) {
        invalid(s, transition_id);
        return;
    }
    prepare(s, transition_id, now_ms);
}

/* Opcode 30: our welcome into the group. */
static void on_welcome(dave_session_t *s, int transition_id, const unsigned char *welcome, size_t n,
                       unsigned long long now_ms)
{
    mls_group_t g;

    if (s->established || !s->has_member)
        return;
    if (!mls_group_join(&g, &s->member, welcome, n, NULL, 0, NULL, 0)) {
        invalid(s, transition_id);
        return;
    }
    if (!group_valid(s, &g, 1)) {
        mls_group_free(&g);
        invalid(s, transition_id);
        return;
    }
    drop_group(s);
    s->group = g;
    s->has_group = s->established = 1;
    prepare(s, transition_id, now_ms);
}

void dave_on_binary(dave_session_t *s, const void *data, size_t n, unsigned long long now_ms)
{
    tls_reader_t r;
    unsigned op;

    tls_reader(&r, data, n);
    tls_read_u16(&r); /* sequence number */
    op = tls_read_u8(&r);
    if (r.bad)
        return;
    if (op == 25) {
        sb_clear(&s->external_sender);
        sb_addn(&s->external_sender, (const char *)r.p, (size_t)(r.end - r.p));
        create_local_group(s);
    } else if (op == 27) {
        on_proposals(s, &r);
    } else if (op == 29 || op == 30) {
        int transition_id = (int)tls_read_u16(&r);
        if (r.bad)
            return;
        if (op == 29)
            on_commit(s, transition_id, r.p, (size_t)(r.end - r.p), now_ms);
        else
            on_welcome(s, transition_id, r.p, (size_t)(r.end - r.p), now_ms);
    }
}

/* ---- Media ---- */

int dave_authenticator(const dave_session_t *s, char *out, size_t size)
{
    return s->established && dave_displayable_code(s->group.keys.authentication, 32, 30, 5, out, size);
}

int dave_session_encrypt(dave_session_t *s, const unsigned char *frame, size_t n, sb_t *out)
{
    unsigned char key[16];
    unsigned long nonce;
    int ok;

    if (s->version == 0) {
        sb_addn(out, (const char *)frame, n);
        return 1;
    }
    if (!s->has_own)
        return 0;
    nonce = s->own_nonce++;
    ok = dave_ratchet_key(&s->own, nonce >> 24, key) && dave_encrypt(key, nonce, frame, n, NULL, 0, out);
    secure_wipe(key, sizeof key);
    return ok;
}

static int decrypt_with(dave_keyring_t *k, unsigned long long user, const unsigned char *frame, size_t n, sb_t *out)
{
    for (int i = 0; i < k->n; i++)
        if (k->senders[i].user == user)
            return dave_decrypt(&k->senders[i].ratchet, frame, n, out);
    return 0;
}

int dave_session_decrypt(dave_session_t *s, unsigned long long user, const unsigned char *frame, size_t n, sb_t *out,
                         unsigned long long now_ms)
{
    unsigned long nonce;
    size_t at = out->len;

    if (dave_is_silence(frame, n) || (!dave_frame_nonce(frame, n, &nonce) && (s->version == 0 || s->has_pending))) {
        sb_addn(out, (const char *)frame, n);
        return 1;
    }
    if (decrypt_with(&s->current, user, frame, n, out))
        return 1;
    out->len = at;
    if (now_ms < s->previous_until && decrypt_with(&s->previous, user, frame, n, out))
        return 1;
    out->len = at;
    return 0;
}
