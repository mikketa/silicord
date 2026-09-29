#include <string.h>
#include "mls_msg.h"
#include "mem.h"
#include "mls_crypto.h"
#include "sha2.h"

static mls_bytes_t span(const unsigned char *start, const tls_reader_t *r)
{
    mls_bytes_t b = {start, (size_t)(r->p - start)};

    return b;
}

static mls_bytes_t read_vec(tls_reader_t *r)
{
    mls_bytes_t b;

    b.p = tls_read_vec(r, &b.n);
    return b;
}

/* Extension extensions<V>: each a uint16 type and opaque data. */
static mls_bytes_t read_extensions(tls_reader_t *r)
{
    tls_reader_t v = tls_read_nested(r);
    mls_bytes_t list = {v.p, (size_t)(v.end - v.p)};

    while (!v.bad && v.p < v.end) {
        tls_read_u16(&v);
        read_vec(&v);
    }
    if (v.bad)
        r->bad = 1;
    return list;
}

int mls_extension_find(mls_bytes_t list, unsigned type, mls_bytes_t *data)
{
    tls_reader_t r;

    tls_reader(&r, list.p, list.n);
    while (!r.bad && r.p < r.end) {
        unsigned t = tls_read_u16(&r);
        mls_bytes_t d = read_vec(&r);
        if (!r.bad && t == type) {
            *data = d;
            return 1;
        }
    }
    data->p = NULL;
    data->n = 0;
    return 0;
}

static int read_key(tls_reader_t *r, unsigned char out[65])
{
    mls_bytes_t k = read_vec(r);

    if (r->bad || k.n != 65 || k.p[0] != 4)
        return 0;
    memcpy(out, k.p, 65);
    return 1;
}

int mls_message_header(tls_reader_t *r, unsigned *wire_format)
{
    unsigned version = tls_read_u16(r);

    *wire_format = tls_read_u16(r);
    return !r->bad && version == 1 && *wire_format >= MLS_WIRE_PUBLIC && *wire_format <= MLS_WIRE_KEY_PACKAGE;
}

/* ---- Key packages ---- */

int mls_key_package_read(mls_key_package_t *kp, tls_reader_t *r)
{
    const unsigned char *start = r->p;
    size_t used;

    memset(kp, 0, sizeof *kp);
    if (tls_read_u16(r) != 1 || tls_read_u16(r) != 2 || !read_key(r, kp->init_key))
        return 0;
    if (!mls_leaf_parse(&kp->leaf, r->p, (size_t)(r->end - r->p), &used))
        return 0;
    r->p += used;
    read_extensions(r);
    kp->tbs = span(start, r);
    kp->signature = read_vec(r);
    if (r->bad || kp->leaf.source != 1) {
        mls_key_package_free(kp);
        return 0;
    }
    return 1;
}

void mls_key_package_free(mls_key_package_t *kp)
{
    mls_node_clear(&kp->leaf);
    memset(kp, 0, sizeof *kp);
}

int mls_key_package_verify(const mls_key_package_t *kp)
{
    return mls_leaf_verify(&kp->leaf, NULL, 0, 0) && !ct_equal(kp->init_key, kp->leaf.key, 65) &&
           mls_verify_with_label(kp->leaf.sig_key, "KeyPackageTBS", kp->tbs.p, kp->tbs.n, kp->signature.p,
                                 kp->signature.n);
}

/* ---- Proposals ---- */

int mls_psk_id_read(tls_reader_t *r, mls_bytes_t *psk_id)
{
    unsigned type = tls_read_u8(r);

    psk_id->p = NULL;
    psk_id->n = 0;
    if (type == 1) {
        *psk_id = read_vec(r);
    } else if (type == 2) {
        tls_read_u8(r); /* usage */
        read_vec(r);    /* psk_group_id */
        tls_read_u64(r);
    } else {
        return 0;
    }
    read_vec(r); /* psk_nonce */
    return r->bad ? 0 : (int)type;
}

int mls_proposal_body_read(int type, tls_reader_t *r)
{
    mls_bytes_t id;

    switch (type) {
    case MLS_PROPOSAL_ADD: {
        mls_key_package_t kp;
        if (!mls_key_package_read(&kp, r))
            return 0;
        mls_key_package_free(&kp);
        return 1;
    }
    case MLS_PROPOSAL_UPDATE: {
        mls_node_t leaf;
        size_t used;
        if (r->bad || !mls_leaf_parse(&leaf, r->p, (size_t)(r->end - r->p), &used))
            return 0;
        r->p += used;
        mls_node_clear(&leaf);
        return 1;
    }
    case MLS_PROPOSAL_REMOVE:
        tls_read_u32(r);
        break;
    case MLS_PROPOSAL_PSK:
        if (!mls_psk_id_read(r, &id))
            return 0;
        break;
    case MLS_PROPOSAL_REINIT:
        read_vec(r);
        tls_read_u16(r);
        tls_read_u16(r);
        read_extensions(r);
        break;
    case MLS_PROPOSAL_EXTERNAL_INIT:
        read_vec(r);
        break;
    case MLS_PROPOSAL_GROUP_CONTEXT_EXTENSIONS:
        read_extensions(r);
        break;
    default:
        return 0;
    }
    return !r->bad;
}

int mls_proposal_read(mls_proposal_t *p, tls_reader_t *r)
{
    const unsigned char *start = r->p, *body;

    p->type = (int)tls_read_u16(r);
    body = r->p;
    if (r->bad || !mls_proposal_body_read(p->type, r))
        return 0;
    p->body = span(body, r);
    p->whole = span(start, r);
    return 1;
}

/* ---- Commits ---- */

int mls_commit_read(mls_commit_t *c, tls_reader_t *r)
{
    tls_reader_t v;
    int cap = 0;
    unsigned has_path;

    memset(c, 0, sizeof *c);
    v = tls_read_nested(r);
    while (!v.bad && v.p < v.end) {
        mls_proposal_or_ref_t *item;
        unsigned type = tls_read_u8(&v);
        if (c->n == cap) {
            cap = cap ? cap * 2 : 8;
            c->items = mem_realloc(c->items, sizeof *c->items * (size_t)cap);
        }
        item = &c->items[c->n++];
        memset(item, 0, sizeof *item);
        if (type == 1) {
            if (!mls_proposal_read(&item->proposal, &v))
                goto bad;
        } else if (type == 2) {
            item->by_ref = 1;
            item->ref = read_vec(&v);
        } else {
            goto bad;
        }
    }
    if (v.bad)
        goto bad;
    has_path = tls_read_u8(r);
    if (has_path > 1 || r->bad)
        goto bad;
    if (has_path) {
        if (!mls_path_read(&c->path, r))
            goto bad;
        c->has_path = 1;
    }
    return 1;
bad:
    mls_commit_free(c);
    return 0;
}

void mls_commit_free(mls_commit_t *c)
{
    if (c->has_path)
        mls_path_free(&c->path);
    mem_free(c->items);
    memset(c, 0, sizeof *c);
}

/* ---- Framing ---- */

static int framed_read(mls_content_t *m, tls_reader_t *r)
{
    const unsigned char *start = r->p, *content;

    m->group_id = read_vec(r);
    m->epoch = tls_read_u64(r);
    m->sender_type = (int)tls_read_u8(r);
    if (m->sender_type == MLS_SENDER_MEMBER || m->sender_type == MLS_SENDER_EXTERNAL)
        m->sender = (unsigned)tls_read_u32(r);
    else if (m->sender_type != MLS_SENDER_NEW_MEMBER_PROPOSAL && m->sender_type != MLS_SENDER_NEW_MEMBER_COMMIT)
        return 0;
    read_vec(r); /* authenticated_data, covered by `framed` */
    m->content_type = (int)tls_read_u8(r);
    content = r->p;
    if (r->bad)
        return 0;
    if (m->content_type == MLS_CONTENT_APPLICATION) {
        m->content = read_vec(r);
        content = NULL;
    } else if (m->content_type == MLS_CONTENT_PROPOSAL) {
        mls_proposal_t p;
        if (!mls_proposal_read(&p, r))
            return 0;
    } else if (m->content_type == MLS_CONTENT_COMMIT) {
        mls_commit_t c;
        if (!mls_commit_read(&c, r))
            return 0;
        mls_commit_free(&c);
    } else {
        return 0;
    }
    if (content)
        m->content = span(content, r);
    m->framed = span(start, r);
    return !r->bad;
}

static int auth_read(mls_content_t *m, tls_reader_t *r)
{
    const unsigned char *start = r->p;

    m->signature = read_vec(r);
    if (m->content_type == MLS_CONTENT_COMMIT)
        m->confirmation_tag = read_vec(r);
    m->auth = span(start, r);
    return !r->bad;
}

int mls_public_read(mls_content_t *m, tls_reader_t *r)
{
    memset(m, 0, sizeof *m);
    m->wire_format = MLS_WIRE_PUBLIC;
    if (!framed_read(m, r) || !auth_read(m, r))
        return 0;
    if (m->sender_type == MLS_SENDER_MEMBER)
        m->membership_tag = read_vec(r);
    return !r->bad;
}

int mls_auth_content_read(mls_content_t *m, tls_reader_t *r)
{
    memset(m, 0, sizeof *m);
    m->wire_format = tls_read_u16(r);
    return framed_read(m, r) && auth_read(m, r);
}

void mls_auth_content(const mls_content_t *m, sb_t *out)
{
    tls_u16(out, m->wire_format);
    sb_addn(out, (const char *)m->framed.p, m->framed.n);
    sb_addn(out, (const char *)m->auth.p, m->auth.n);
}

void mls_content_tbs(const mls_content_t *m, const void *gc, size_t gcn, sb_t *out)
{
    tls_u16(out, 1);
    tls_u16(out, m->wire_format);
    sb_addn(out, (const char *)m->framed.p, m->framed.n);
    if (m->sender_type == MLS_SENDER_MEMBER || m->sender_type == MLS_SENDER_NEW_MEMBER_COMMIT)
        sb_addn(out, gc, gcn);
}

int mls_content_verify(const mls_content_t *m, const unsigned char sig_key[65], const void *gc, size_t gcn)
{
    sb_t tbs = {0};
    int ok;

    mls_content_tbs(m, gc, gcn, &tbs);
    ok = mls_verify_with_label(sig_key, "FramedContentTBS", tbs.data, tbs.len, m->signature.p, m->signature.n);
    sb_free(&tbs);
    return ok;
}

int mls_membership_tag(const mls_content_t *m, const unsigned char key[32], const void *gc, size_t gcn,
                       unsigned char tag[32])
{
    sb_t tbm = {0};

    mls_content_tbs(m, gc, gcn, &tbm);
    sb_addn(&tbm, (const char *)m->auth.p, m->auth.n);
    hmac256(key, 32, tbm.data, tbm.len, tag);
    sb_free(&tbm);
    return 1;
}

/* ---- Group info and welcome ---- */

int mls_group_info_read(mls_group_info_t *gi, tls_reader_t *r)
{
    const unsigned char *start = r->p, *gc = r->p;

    memset(gi, 0, sizeof *gi);
    if (tls_read_u16(r) != 1 || tls_read_u16(r) != 2)
        return 0;
    gi->group_id = read_vec(r);
    gi->epoch = tls_read_u64(r);
    gi->tree_hash = read_vec(r);
    gi->confirmed_transcript_hash = read_vec(r);
    gi->gc_extensions = read_extensions(r);
    gi->group_context = span(gc, r);
    gi->extensions = read_extensions(r);
    gi->confirmation_tag = read_vec(r);
    gi->signer = (unsigned)tls_read_u32(r);
    gi->tbs = span(start, r);
    gi->signature = read_vec(r);
    return !r->bad && gi->tree_hash.n == 32;
}

int mls_welcome_read(mls_welcome_t *w, tls_reader_t *r)
{
    tls_reader_t v;
    int cap = 0;

    memset(w, 0, sizeof *w);
    if (tls_read_u16(r) != 2)
        return 0;
    v = tls_read_nested(r);
    while (!v.bad && v.p < v.end) {
        mls_group_secrets_ct_t *s;
        tls_reader_t ct;
        if (w->n == cap) {
            cap = cap ? cap * 2 : 4;
            w->secrets = mem_realloc(w->secrets, sizeof *w->secrets * (size_t)cap);
        }
        s = &w->secrets[w->n++];
        s->new_member = read_vec(&v);
        /* HPKECiphertext is a plain struct: kem_output then ciphertext. */
        ct = v;
        s->kem = read_vec(&ct);
        s->ct = read_vec(&ct);
        v = ct;
    }
    w->encrypted_group_info = read_vec(r);
    if (v.bad || r->bad) {
        mls_welcome_free(w);
        return 0;
    }
    return 1;
}

void mls_welcome_free(mls_welcome_t *w)
{
    mem_free(w->secrets);
    memset(w, 0, sizeof *w);
}

int mls_group_secrets_read(mls_group_secrets_t *gs, tls_reader_t *r)
{
    tls_reader_t v;
    unsigned has;

    memset(gs, 0, sizeof *gs);
    gs->joiner = read_vec(r);
    has = tls_read_u8(r);
    if (has > 1)
        return 0;
    if (has) {
        gs->path_secret = read_vec(r);
        gs->has_path_secret = 1;
    }
    v = tls_read_nested(r);
    while (!v.bad && v.p < v.end) {
        const unsigned char *at = v.p;
        mls_bytes_t id;
        if (gs->npsks == 16)
            return 0;
        if (!mls_psk_id_read(&v, &id))
            return 0;
        gs->psks[gs->npsks++] = span(at, &v);
    }
    return !v.bad && !r->bad && gs->joiner.n == 32 && (!has || gs->path_secret.n == 32);
}

/* ---- Transcript hashes and PSKs ---- */

void mls_transcript(const void *interim, size_t in, const mls_content_t *commit, unsigned char confirmed[32],
                    unsigned char interim_next[32])
{
    sha256_t h;
    sb_t sig = {0};

    sha256_init(&h);
    sha256_update(&h, interim, in);
    tls_u16(&sig, commit->wire_format);
    sha256_update(&h, sig.data, sig.len);
    sha256_update(&h, commit->framed.p, commit->framed.n);
    sb_clear(&sig);
    tls_vec(&sig, commit->signature.p, commit->signature.n);
    sha256_update(&h, sig.data, sig.len);
    sha256_final(&h, confirmed);
    sb_free(&sig);
    mls_interim(confirmed, 32, commit->confirmation_tag.p, commit->confirmation_tag.n, interim_next);
}

void mls_interim(const void *confirmed, size_t cn, const void *tag, size_t tn, unsigned char out[32])
{
    sha256_t h;
    sb_t v = {0};

    tls_vec(&v, tag, tn);
    sha256_init(&h);
    sha256_update(&h, confirmed, cn);
    sha256_update(&h, v.data, v.len);
    sha256_final(&h, out);
    sb_free(&v);
}

int mls_psk_secret(const mls_bytes_t *ids, const mls_bytes_t *psks, int n, unsigned char out[32])
{
    static const unsigned char zero[32] = {0};
    unsigned char extracted[32], input[32], secret[32];
    int ok = 1;

    memset(secret, 0, 32);
    for (int i = 0; ok && i < n; i++) {
        sb_t label = {0};
        sb_addn(&label, (const char *)ids[i].p, ids[i].n);
        tls_u16(&label, (unsigned)i);
        tls_u16(&label, (unsigned)n);
        hkdf256_extract(zero, 32, psks[i].p, psks[i].n, extracted);
        ok = mls_expand_with_label(extracted, 32, "derived psk", label.data, label.len, input, 32);
        hkdf256_extract(input, 32, secret, 32, secret);
        sb_free(&label);
    }
    memcpy(out, secret, 32);
    secure_wipe(extracted, sizeof extracted);
    secure_wipe(input, sizeof input);
    secure_wipe(secret, sizeof secret);
    return ok;
}
