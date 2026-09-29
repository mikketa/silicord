#include <string.h>
#include "mls_group.h"
#include "aes.h"
#include "hpke.h"
#include "mem.h"
#include "mls_crypto.h"
#include "mls_path.h"
#include "rng.h"
#include "sha2.h"
#include "tls.h"

static const unsigned char k_zero[32] = {0};

static void sb_copy(sb_t *dst, const sb_t *src)
{
    memset(dst, 0, sizeof *dst);
    if (src->len)
        sb_addn(dst, src->data, src->len);
}

static mls_bytes_t bytes_of(const sb_t *s)
{
    mls_bytes_t b = {(const unsigned char *)s->data, s->len};

    return b;
}

static void root_hash(const mls_tree_t *t, unsigned char h[32])
{
    mls_tree_hash(t, mls_root(t->nleaves), h);
}

static void mls_key_package_ref(const void *kp, size_t n, unsigned char ref[32])
{
    mls_ref_hash("MLS 1.0 KeyPackage Reference", kp, n, ref);
}

/* ---- Members ---- */

static int keypair(unsigned char priv[32], unsigned char pub[65])
{
    unsigned char seed[32];
    int ok = rng_bytes(seed, 32) && hpke_derive_keypair(seed, 32, priv, pub);

    secure_wipe(seed, sizeof seed);
    return ok;
}

int mls_member_create(mls_member_t *m, const void *identity, size_t in)
{
    unsigned char init_pub[65], enc_pub[65], sig_pub[65];
    sb_t leaf = {0}, kp = {0}, sig = {0};
    int ok;

    memset(m, 0, sizeof *m);
    if (!keypair(m->init_priv, init_pub) || !keypair(m->enc_priv, enc_pub) || !keypair(m->sig_priv, sig_pub))
        return 0;
    tls_vec(&leaf, enc_pub, 65);
    tls_vec(&leaf, sig_pub, 65);
    tls_u16(&leaf, 1); /* basic credential */
    tls_vec(&leaf, identity, in);
    /* Capabilities: versions, cipher suites, extensions, proposals, credentials. */
    tls_varint(&leaf, 2);
    tls_u16(&leaf, 1);
    tls_varint(&leaf, 2);
    tls_u16(&leaf, 2);
    tls_varint(&leaf, 0);
    tls_varint(&leaf, 0);
    tls_varint(&leaf, 2);
    tls_u16(&leaf, 1);
    tls_u8(&leaf, 1); /* key_package, valid forever */
    tls_u64(&leaf, 0);
    tls_u64(&leaf, ~0ull);
    tls_varint(&leaf, 0);
    ok = mls_sign_with_label(m->sig_priv, "LeafNodeTBS", leaf.data, leaf.len, &sig);
    tls_vec(&leaf, sig.data, sig.len);
    sb_clear(&sig);
    tls_u16(&kp, 1);
    tls_u16(&kp, 2);
    tls_vec(&kp, init_pub, 65);
    sb_addn(&kp, leaf.data, leaf.len);
    tls_varint(&kp, 0);
    ok = ok && mls_sign_with_label(m->sig_priv, "KeyPackageTBS", kp.data, kp.len, &sig);
    tls_vec(&kp, sig.data, sig.len);
    m->key_package = kp;
    sb_free(&leaf);
    sb_free(&sig);
    if (!ok)
        mls_member_free(m);
    return ok;
}

void mls_member_free(mls_member_t *m)
{
    sb_free(&m->key_package);
    secure_wipe(m, sizeof *m);
}

/* ---- Group state ---- */

static void mls_group_context_of(const mls_group_t *g, sb_t *gc)
{
    unsigned char th[32];

    root_hash(&g->tree, th);
    mls_group_context(gc, g->group_id.data, g->group_id.len, g->epoch, th, g->cth, g->cth_n, g->extensions.data,
                      g->extensions.len);
}

static void clear_props(mls_group_t *g)
{
    for (int i = 0; i < g->nprops; i++)
        sb_free(&g->props[i].proposal);
    mem_free(g->props);
    g->props = NULL;
    g->nprops = 0;
}

void mls_group_free(mls_group_t *g)
{
    clear_props(g);
    sb_free(&g->group_id);
    sb_free(&g->extensions);
    mls_tree_free(&g->tree);
    secure_wipe(g, sizeof *g);
}

/* Keeps this epoch's resumption PSK for later PreSharedKey proposals. */
static void remember_resumption(mls_group_t *g)
{
    if (g->nresumption == 8) {
        memmove(g->resumption_epoch, g->resumption_epoch + 1, sizeof *g->resumption_epoch * 7);
        memmove(g->resumption, g->resumption + 1, sizeof *g->resumption * 7);
        g->nresumption--;
    }
    g->resumption_epoch[g->nresumption] = g->epoch;
    memcpy(g->resumption[g->nresumption++], g->keys.resumption, 32);
}

/* Sets our leaf and the epoch-0 state. */
int mls_group_create(mls_group_t *g, const mls_member_t *m, const void *group_id, size_t gn, const void *extensions,
                     size_t en)
{
    mls_key_package_t kp;
    tls_reader_t r;
    unsigned char joiner[32], tag[32];
    sb_t gc = {0};
    int ok;

    memset(g, 0, sizeof *g);
    tls_reader(&r, m->key_package.data, m->key_package.len);
    if (!mls_key_package_read(&kp, &r))
        return 0;
    mls_tree_extend(&g->tree, 1);
    mls_node_copy(&g->tree.nodes[0], &kp.leaf);
    memcpy(g->tree.nodes[0].priv, m->enc_priv, 32);
    g->tree.nodes[0].has_priv = 1;
    mls_key_package_free(&kp);
    sb_addn(&g->group_id, group_id, gn);
    if (en)
        sb_addn(&g->extensions, extensions, en);
    memcpy(g->sig_priv, m->sig_priv, 32);
    mls_group_context_of(g, &gc);
    ok = rng_bytes(joiner, 32) && mls_key_schedule_joiner(joiner, k_zero, gc.data, gc.len, &g->keys);
    hmac256(g->keys.confirm, 32, "", 0, tag);
    mls_interim("", 0, tag, 32, g->interim);
    remember_resumption(g);
    secure_wipe(joiner, sizeof joiner);
    sb_free(&gc);
    if (!ok)
        mls_group_free(g);
    return ok;
}

/* ---- Welcome ---- */

static int welcome_key(const unsigned char joiner[32], const unsigned char psk[32], unsigned char key[16],
                       unsigned char nonce[12])
{
    unsigned char member[32], welcome[32];
    int ok;

    hkdf256_extract(joiner, 32, psk, 32, member);
    ok = mls_derive_secret(member, "welcome", welcome) && mls_expand_with_label(welcome, 32, "key", "", 0, key, 16) &&
         mls_expand_with_label(welcome, 32, "nonce", "", 0, nonce, 12);
    secure_wipe(member, sizeof member);
    secure_wipe(welcome, sizeof welcome);
    return ok;
}

static unsigned common_ancestor(unsigned a, unsigned b)
{
    unsigned x = 2 * a;

    while (!mls_in_subtree(2 * b, x))
        x = mls_parent(x);
    return x;
}

static int find_psk(const mls_psk_t *psks, int n, mls_bytes_t id, mls_bytes_t *secret)
{
    for (int i = 0; i < n; i++)
        if (psks[i].id.n == id.n && ct_equal(psks[i].id.p, id.p, id.n)) {
            *secret = psks[i].secret;
            return 1;
        }
    return 0;
}

/* The key a PreSharedKeyID names: an external one we hold, or this group's resumption PSK of an epoch. */
static int psk_lookup(const mls_group_t *g, mls_bytes_t encoded, mls_bytes_t *secret)
{
    tls_reader_t r;
    mls_bytes_t id;
    unsigned type;

    tls_reader(&r, encoded.p, encoded.n);
    type = tls_read_u8(&r);
    if (type == 1) {
        id.p = tls_read_vec(&r, &id.n);
        return !r.bad && find_psk(g->psks, g->npsks, id, secret);
    }
    if (type == 2 && tls_read_u8(&r) == 1) { /* usage: application */
        size_t gn;
        const unsigned char *gid = tls_read_vec(&r, &gn);
        unsigned long long epoch = tls_read_u64(&r);
        if (r.bad || gn != g->group_id.len || !ct_equal(gid, g->group_id.data, gn))
            return 0;
        for (int i = 0; i < g->nresumption; i++)
            if (g->resumption_epoch[i] == epoch) {
                secret->p = g->resumption[i];
                secret->n = 32;
                return 1;
            }
    }
    return 0;
}

/* psk_secret for PreSharedKeyID encodings. */
static int psk_secret_of(const mls_group_t *g, const mls_bytes_t *ids, int n, unsigned char out[32])
{
    mls_bytes_t secrets[16];

    if (n > 16)
        return 0;
    for (int i = 0; i < n; i++)
        if (!psk_lookup(g, ids[i], &secrets[i]))
            return 0;
    return mls_psk_secret(ids, secrets, n, out);
}

static int next_secret(unsigned char secret[32])
{
    unsigned char next[32];
    int ok = mls_derive_secret(secret, "path", next);

    memcpy(secret, next, 32);
    secure_wipe(next, sizeof next);
    return ok;
}

int mls_group_join(mls_group_t *g, const mls_member_t *m, const void *welcome, size_t n, const void *tree, size_t tn,
                   const mls_psk_t *psks, int npsks)
{
    const unsigned char *wb = welcome;
    unsigned char ref[32], psk[32], key[16], nonce[12], secret[32], tag[32];
    mls_welcome_t w = {0};
    mls_key_package_t kp = {0};
    mls_group_secrets_t gs;
    mls_group_info_t gi;
    mls_bytes_t tree_bytes;
    sb_t gsb = {0}, gib = {0};
    tls_reader_t r;
    int ok = 0, i;

    memset(g, 0, sizeof *g);
    g->psks = psks;
    g->npsks = npsks;
    tls_reader(&r, welcome, n);
    if (n >= 2 && wb[0] == 0 && wb[1] == 1) {
        unsigned wf;
        if (!mls_message_header(&r, &wf) || wf != MLS_WIRE_WELCOME)
            return 0;
    }
    if (!mls_welcome_read(&w, &r))
        return 0;
    if (!tls_done(&r))
        goto out;
    tls_reader(&r, m->key_package.data, m->key_package.len);
    if (!mls_key_package_read(&kp, &r))
        goto out;
    mls_key_package_ref(m->key_package.data, m->key_package.len, ref);
    for (i = 0; i < w.n && !(w.secrets[i].new_member.n == 32 && ct_equal(w.secrets[i].new_member.p, ref, 32)); i++)
        ;
    if (i == w.n || w.secrets[i].kem.n != 65 ||
        !mls_decrypt_with_label(m->init_priv, "Welcome", w.encrypted_group_info.p, w.encrypted_group_info.n,
                                w.secrets[i].kem.p, w.secrets[i].ct.p, w.secrets[i].ct.n, &gsb))
        goto out;
    tls_reader(&r, gsb.data, gsb.len);
    if (!mls_group_secrets_read(&gs, &r) || !tls_done(&r) || !psk_secret_of(g, gs.psks, gs.npsks, psk))
        goto out;
    if (w.encrypted_group_info.n < 16 || !welcome_key(gs.joiner.p, psk, key, nonce))
        goto out;
    sb_reserve(&gib, w.encrypted_group_info.n);
    if (!aes_gcm_open(key, 16, nonce, "", 0, w.encrypted_group_info.p, w.encrypted_group_info.n - 16, gib.data,
                      w.encrypted_group_info.p + w.encrypted_group_info.n - 16, 16))
        goto out;
    gib.len = w.encrypted_group_info.n - 16;
    tls_reader(&r, gib.data, gib.len);
    if (!mls_group_info_read(&gi, &r) || !tls_done(&r))
        goto out;
    if (tree) {
        tree_bytes.p = tree;
        tree_bytes.n = tn;
    } else if (!mls_extension_find(gi.extensions, MLS_EXT_RATCHET_TREE, &tree_bytes)) {
        goto out;
    }
    if (!mls_tree_parse(&g->tree, tree_bytes.p, tree_bytes.n))
        goto out;
    sb_addn(&g->group_id, (const char *)gi.group_id.p, gi.group_id.n);

    /* The tree must be the signer's and hold our leaf. */
    root_hash(&g->tree, secret);
    if (gi.signer >= g->tree.nleaves || !g->tree.nodes[2 * gi.signer].present ||
        !mls_verify_with_label(g->tree.nodes[2 * gi.signer].sig_key, "GroupInfoTBS", gi.tbs.p, gi.tbs.n,
                               gi.signature.p, gi.signature.n) ||
        !ct_equal(secret, gi.tree_hash.p, 32) || !mls_tree_verify_parent_hashes(&g->tree) ||
        !mls_tree_verify_leaves(&g->tree, gi.group_id.p, gi.group_id.n))
        goto out;
    for (i = 0; i < (int)g->tree.nleaves; i++) {
        const mls_node_t *leaf = &g->tree.nodes[2 * i];
        if (leaf->present && leaf->leaf.len == kp.leaf.leaf.len &&
            ct_equal(leaf->leaf.data, kp.leaf.leaf.data, leaf->leaf.len))
            break;
    }
    if (i == (int)g->tree.nleaves)
        goto out;
    g->me = (unsigned)i;
    memcpy(g->tree.nodes[2 * i].priv, m->enc_priv, 32);
    g->tree.nodes[2 * i].has_priv = 1;
    if (gs.has_path_secret) {
        unsigned x = common_ancestor(g->me, gi.signer), root = mls_root(g->tree.nleaves);
        memcpy(secret, gs.path_secret.p, 32);
        for (;;) {
            if (g->tree.nodes[x].present &&
                (!mls_node_set_secret(&g->tree.nodes[x], secret) || !next_secret(secret)))
                goto out;
            if (x == root)
                break;
            x = mls_parent(x);
        }
    }

    if (!mls_key_schedule_joiner(gs.joiner.p, psk, gi.group_context.p, gi.group_context.n, &g->keys))
        goto out;
    hmac256(g->keys.confirm, 32, gi.confirmed_transcript_hash.p, gi.confirmed_transcript_hash.n, tag);
    if (gi.confirmation_tag.n != 32 || !ct_equal(tag, gi.confirmation_tag.p, 32) ||
        (gi.confirmed_transcript_hash.n != 32 && gi.confirmed_transcript_hash.n != 0))
        goto out;
    g->cth_n = gi.confirmed_transcript_hash.n;
    memcpy(g->cth, gi.confirmed_transcript_hash.p, g->cth_n);
    mls_interim(g->cth, g->cth_n, gi.confirmation_tag.p, 32, g->interim);
    g->epoch = gi.epoch;
    if (gi.gc_extensions.n)
        sb_addn(&g->extensions, (const char *)gi.gc_extensions.p, gi.gc_extensions.n);
    memcpy(g->sig_priv, m->sig_priv, 32);
    remember_resumption(g);
    ok = 1;
out:
    secure_wipe(secret, sizeof secret);
    secure_wipe(key, sizeof key);
    mls_welcome_free(&w);
    mls_key_package_free(&kp);
    sb_free(&gsb);
    sb_free(&gib);
    if (!ok) {
        mls_group_free(g);
        g->psks = NULL;
    }
    return ok;
}

/* ---- Proposals ---- */

typedef struct {
    int sender_type;
    unsigned sender;
    mls_proposal_t p;
} resolved_t;

typedef struct {
    int nadded;
    unsigned *added;
    mls_bytes_t *added_kp; /* each added member's KeyPackage */
    int npsks;
    mls_bytes_t psk_ids[16];
    int path_required, removed_me, has_ext;
    mls_bytes_t ext;
} applied_t;

static void applied_free(applied_t *a)
{
    mem_free(a->added);
    mem_free(a->added_kp);
}

static void blank_path(mls_tree_t *t, unsigned leaf)
{
    unsigned x = 2 * leaf, root = mls_root(t->nleaves);

    while (x != root) {
        x = mls_parent(x);
        mls_node_clear(&t->nodes[x]);
    }
}

/* Adds `leaf` to a parent's unmerged leaves, in order. */
static void add_unmerged(mls_node_t *node, unsigned leaf)
{
    int k = node->nunmerged;

    node->unmerged = mem_realloc(node->unmerged, sizeof *node->unmerged * (size_t)(k + 1));
    while (k > 0 && node->unmerged[k - 1] > leaf) {
        node->unmerged[k] = node->unmerged[k - 1];
        k--;
    }
    node->unmerged[k] = leaf;
    node->nunmerged++;
}

static int apply_add(const mls_group_t *g, mls_tree_t *t, const mls_proposal_t *p, applied_t *a)
{
    mls_key_package_t kp;
    tls_reader_t r;
    unsigned i, x, root;
    int ok;

    tls_reader(&r, p->body.p, p->body.n);
    if (!mls_key_package_read(&kp, &r))
        return 0;
    ok = tls_done(&r) && mls_key_package_verify(&kp);
    /* A signature key already in the group is the same client twice. */
    for (i = 0; ok && i < t->nleaves; i++)
        ok = !t->nodes[2 * i].present || !ct_equal(t->nodes[2 * i].sig_key, kp.leaf.sig_key, 65);
    if (ok) {
        for (i = 0; i < t->nleaves && t->nodes[2 * i].present; i++)
            ;
        if (i == t->nleaves)
            mls_tree_extend(t, t->nleaves + 1);
        root = mls_root(t->nleaves);
        for (x = 2 * i; x != root;) {
            x = mls_parent(x);
            if (t->nodes[x].present)
                add_unmerged(&t->nodes[x], i);
        }
        mls_node_copy(&t->nodes[2 * i], &kp.leaf);
        a->added[a->nadded] = i;
        a->added_kp[a->nadded++] = p->body;
    }
    (void)g;
    mls_key_package_free(&kp);
    return ok;
}

static int apply_proposals(const mls_group_t *g, mls_tree_t *t, const resolved_t *list, int n, unsigned committer,
                           applied_t *a)
{
    unsigned char *touched;
    int ok = 1, ngce = 0;

    memset(a, 0, sizeof *a);
    a->added = mem_alloc(sizeof *a->added * (size_t)(n + 1));
    a->added_kp = mem_alloc(sizeof *a->added_kp * (size_t)(n + 1));
    a->path_required = n == 0;
    touched = mem_alloc(t->nleaves + 1);
    for (int i = 0; ok && i < n; i++) {
        const resolved_t *rp = &list[i];
        if (rp->p.type == MLS_PROPOSAL_GROUP_CONTEXT_EXTENSIONS) {
            tls_reader_t r;
            tls_reader(&r, rp->p.body.p, rp->p.body.n);
            a->ext.p = tls_read_vec(&r, &a->ext.n);
            a->has_ext = 1;
            a->path_required = 1;
            ok = ++ngce == 1 && tls_done(&r);
        } else if (rp->p.type == MLS_PROPOSAL_REINIT || rp->p.type == MLS_PROPOSAL_EXTERNAL_INIT) {
            ok = 0;
        }
    }
    /* Updates, then removes, then adds. */
    for (int i = 0; ok && i < n; i++) {
        const resolved_t *rp = &list[i];
        mls_node_t leaf = {0};
        size_t used = 0;
        if (rp->p.type != MLS_PROPOSAL_UPDATE)
            continue;
        a->path_required = 1;
        ok = rp->sender_type == MLS_SENDER_MEMBER && rp->sender != committer && rp->sender < t->nleaves &&
             t->nodes[2 * rp->sender].present && !touched[rp->sender] &&
             mls_leaf_parse(&leaf, rp->p.body.p, rp->p.body.n, &used);
        if (!ok)
            break;
        ok = used == rp->p.body.n && leaf.source == 2 &&
             mls_leaf_verify(&leaf, g->group_id.data, g->group_id.len, rp->sender);
        if (ok) {
            touched[rp->sender] = 1;
            mls_node_clear(&t->nodes[2 * rp->sender]);
            t->nodes[2 * rp->sender] = leaf;
            blank_path(t, rp->sender);
        } else {
            mls_node_clear(&leaf);
        }
    }
    for (int i = 0; ok && i < n; i++) {
        const resolved_t *rp = &list[i];
        tls_reader_t r;
        unsigned removed;
        if (rp->p.type != MLS_PROPOSAL_REMOVE)
            continue;
        a->path_required = 1;
        tls_reader(&r, rp->p.body.p, rp->p.body.n);
        removed = (unsigned)tls_read_u32(&r);
        ok = tls_done(&r) && removed < t->nleaves && t->nodes[2 * removed].present && removed != committer &&
             !touched[removed];
        if (ok) {
            touched[removed] = 1;
            a->removed_me |= removed == g->me;
            mls_node_clear(&t->nodes[2 * removed]);
            blank_path(t, removed);
        }
    }
    if (ok)
        mls_tree_truncate(t);
    for (int i = 0; ok && i < n; i++)
        if (list[i].p.type == MLS_PROPOSAL_ADD)
            ok = apply_add(g, t, &list[i].p, a);
    for (int i = 0; ok && i < n; i++)
        if (list[i].p.type == MLS_PROPOSAL_PSK) {
            tls_reader_t r;
            mls_bytes_t id;
            tls_reader(&r, list[i].p.body.p, list[i].p.body.n);
            ok = a->npsks < 16 && mls_psk_id_read(&r, &id) && tls_done(&r);
            if (ok)
                a->psk_ids[a->npsks++] = list[i].p.body;
        }
    mem_free(touched);
    return ok;
}

/* Proposals by reference must be in the cache; by value they come from the committer. */
static int resolve(const mls_group_t *g, const mls_commit_t *c, unsigned committer, resolved_t *out)
{
    for (int i = 0; i < c->n; i++) {
        const mls_proposal_or_ref_t *item = &c->items[i];
        if (item->by_ref) {
            int k;
            tls_reader_t r;
            for (k = 0; k < g->nprops && !(item->ref.n == 32 && ct_equal(item->ref.p, g->props[k].ref, 32)); k++)
                ;
            if (k == g->nprops)
                return 0;
            tls_reader(&r, g->props[k].proposal.data, g->props[k].proposal.len);
            if (!mls_proposal_read(&out[i].p, &r))
                return 0;
            out[i].sender_type = g->props[k].sender_type;
            out[i].sender = g->props[k].sender;
        } else {
            out[i].p = item->proposal;
            out[i].sender_type = MLS_SENDER_MEMBER;
            out[i].sender = committer;
        }
    }
    return 1;
}

/* The signature key of external sender `index` in the group's extensions. */
static int external_sender_key(const mls_group_t *g, unsigned index, unsigned char key[65])
{
    mls_bytes_t list;
    tls_reader_t r, v;

    if (!mls_extension_find(bytes_of(&g->extensions), MLS_EXT_EXTERNAL_SENDERS, &list))
        return 0;
    tls_reader(&r, list.p, list.n);
    v = tls_read_nested(&r);
    for (unsigned i = 0; !v.bad && v.p < v.end; i++) {
        size_t n, cn;
        const unsigned char *k = tls_read_vec(&v, &n);
        tls_read_u16(&v); /* credential type */
        tls_read_vec(&v, &cn);
        if (!v.bad && i == index) {
            if (n != 65)
                return 0;
            memcpy(key, k, 65);
            return 1;
        }
    }
    return 0;
}

/* ---- Commits ---- */

static int process_commit(mls_group_t *g, const mls_content_t *m)
{
    mls_commit_t c;
    resolved_t *list = NULL;
    applied_t a = {0};
    mls_tree_t t = {0};
    tls_reader_t r;
    unsigned char commit_secret[32], psk[32], th[32], cth[32], interim[32], tag[32];
    unsigned committer = m->sender;
    mls_bytes_t ext = bytes_of(&g->extensions);
    mls_epoch_t keys;
    sb_t gc = {0};
    int ok = 0, applied = 0;

    tls_reader(&r, m->content.p, m->content.n);
    if (m->sender_type != MLS_SENDER_MEMBER || !mls_commit_read(&c, &r))
        return 0;
    list = mem_alloc(sizeof *list * (size_t)(c.n + 1));
    mls_tree_copy(&t, &g->tree);
    if (!resolve(g, &c, committer, list))
        goto out;
    applied = 1;
    if (!apply_proposals(g, &t, list, c.n, committer, &a) || (a.path_required && !c.has_path))
        goto out;
    if (a.has_ext)
        ext = a.ext;
    memset(commit_secret, 0, 32);
    if (c.has_path) {
        if (ct_equal(g->tree.nodes[2 * committer].key, c.path.leaf.key, 65) ||
            !mls_path_merge(&t, committer, &c.path) ||
            !mls_leaf_verify(&t.nodes[2 * committer], g->group_id.data, g->group_id.len, committer))
            goto out;
        if (!a.removed_me) {
            root_hash(&t, th);
            mls_group_context(&gc, g->group_id.data, g->group_id.len, g->epoch + 1, th, g->cth, g->cth_n, ext.p, ext.n);
            if (!mls_path_decrypt(&t, g->me, committer, &c.path, gc.data, gc.len, a.added, a.nadded, NULL,
                                  commit_secret))
                goto out;
        }
    }
    if (a.removed_me) {
        ok = MLS_HANDLED_REMOVED;
        goto out;
    }
    mls_transcript(g->interim, 32, m, cth, interim);
    root_hash(&t, th);
    sb_clear(&gc);
    mls_group_context(&gc, g->group_id.data, g->group_id.len, g->epoch + 1, th, cth, 32, ext.p, ext.n);
    if (!psk_secret_of(g, a.psk_ids, a.npsks, psk) ||
        !mls_key_schedule(g->keys.init, commit_secret, psk, gc.data, gc.len, &keys))
        goto out;
    hmac256(keys.confirm, 32, cth, 32, tag);
    if (m->confirmation_tag.n != 32 || !ct_equal(tag, m->confirmation_tag.p, 32))
        goto out;

    if (a.has_ext) {
        sb_t e = {0};
        if (ext.n)
            sb_addn(&e, (const char *)ext.p, ext.n);
        sb_free(&g->extensions);
        g->extensions = e;
    }
    mls_tree_free(&g->tree);
    g->tree = t;
    memset(&t, 0, sizeof t);
    g->epoch++;
    memcpy(g->cth, cth, 32);
    g->cth_n = 32;
    memcpy(g->interim, interim, 32);
    g->keys = keys;
    clear_props(g);
    remember_resumption(g);
    ok = MLS_HANDLED_COMMIT;
out:
    secure_wipe(commit_secret, sizeof commit_secret);
    secure_wipe(&keys, sizeof keys);
    if (applied)
        applied_free(&a);
    mls_tree_free(&t);
    mem_free(list);
    mls_commit_free(&c);
    sb_free(&gc);
    return ok;
}

int mls_group_handle(mls_group_t *g, const void *msg, size_t n, unsigned char ref[32])
{
    tls_reader_t r;
    mls_content_t m;
    unsigned wf;
    unsigned char key[65], tag[32];
    sb_t gc = {0};
    int ok = 0;

    tls_reader(&r, msg, n);
    if (!mls_message_header(&r, &wf) || wf != MLS_WIRE_PUBLIC || !mls_public_read(&m, &r) || !tls_done(&r))
        return 0;
    if (m.group_id.n != g->group_id.len || !ct_equal(m.group_id.p, g->group_id.data, m.group_id.n) ||
        m.epoch != g->epoch || m.content_type == MLS_CONTENT_APPLICATION)
        return 0;
    mls_group_context_of(g, &gc);
    if (m.sender_type == MLS_SENDER_MEMBER) {
        ok = m.sender < g->tree.nleaves && g->tree.nodes[2 * m.sender].present &&
             mls_membership_tag(&m, g->keys.membership, gc.data, gc.len, tag) && m.membership_tag.n == 32 &&
             ct_equal(tag, m.membership_tag.p, 32);
        if (ok)
            memcpy(key, g->tree.nodes[2 * m.sender].sig_key, 65);
    } else if (m.sender_type == MLS_SENDER_EXTERNAL) {
        ok = m.content_type == MLS_CONTENT_PROPOSAL && external_sender_key(g, m.sender, key);
    }
    ok = ok && mls_content_verify(&m, key, gc.data, gc.len);
    sb_free(&gc);
    if (!ok)
        return 0;
    if (m.content_type == MLS_CONTENT_PROPOSAL) {
        mls_cached_proposal_t *cp;
        mls_proposal_t p;
        sb_t ac = {0};
        tls_reader(&r, m.content.p, m.content.n);
        if (!mls_proposal_read(&p, &r))
            return 0;
        if (m.sender_type == MLS_SENDER_EXTERNAL && p.type != MLS_PROPOSAL_ADD && p.type != MLS_PROPOSAL_REMOVE &&
            p.type != MLS_PROPOSAL_PSK && p.type != MLS_PROPOSAL_REINIT &&
            p.type != MLS_PROPOSAL_GROUP_CONTEXT_EXTENSIONS)
            return 0;
        g->props = mem_realloc(g->props, sizeof *g->props * (size_t)(g->nprops + 1));
        cp = &g->props[g->nprops++];
        memset(cp, 0, sizeof *cp);
        cp->sender_type = m.sender_type;
        cp->sender = m.sender;
        sb_addn(&cp->proposal, (const char *)p.whole.p, p.whole.n);
        mls_auth_content(&m, &ac);
        mls_ref_hash("MLS 1.0 Proposal Reference", ac.data, ac.len, cp->ref);
        sb_free(&ac);
        if (ref)
            memcpy(ref, cp->ref, 32);
        return MLS_HANDLED_PROPOSAL;
    }
    return process_commit(g, &m);
}

int mls_group_revoke(mls_group_t *g, const void *ref, size_t rn)
{
    for (int i = 0; i < g->nprops; i++)
        if (rn == 32 && ct_equal(g->props[i].ref, ref, 32)) {
            sb_free(&g->props[i].proposal);
            memmove(&g->props[i], &g->props[i + 1], sizeof *g->props * (size_t)(g->nprops - i - 1));
            g->nprops--;
            return 1;
        }
    return 0;
}

/* The Welcome for the members a commit adds. */
static int make_welcome(const mls_group_t *next, const applied_t *a, const mls_path_secrets_t *ps,
                        const unsigned char psk[32], const unsigned char tag[32], sb_t *out)
{
    sb_t gi = {0}, ext = {0}, tree = {0}, sig = {0}, egi = {0}, secrets = {0};
    unsigned char key[16], nonce[12];
    int ok;

    mls_group_context_of(next, &gi);
    mls_tree_serialize(&next->tree, &tree);
    tls_u16(&ext, MLS_EXT_RATCHET_TREE);
    tls_vec(&ext, tree.data, tree.len);
    tls_vec(&gi, ext.data, ext.len);
    tls_vec(&gi, tag, 32);
    tls_u32(&gi, next->me);
    ok = mls_sign_with_label(next->sig_priv, "GroupInfoTBS", gi.data, gi.len, &sig) &&
         welcome_key(next->keys.joiner, psk, key, nonce);
    tls_vec(&gi, sig.data, sig.len);
    if (ok) {
        sb_reserve(&egi, gi.len + 16);
        ok = aes_gcm_seal(key, 16, nonce, "", 0, gi.data, gi.len, egi.data, (unsigned char *)egi.data + gi.len, 16);
        egi.len = gi.len + 16;
    }
    for (int i = 0; ok && i < a->nadded; i++) {
        mls_key_package_t kp;
        tls_reader_t r;
        unsigned char ref[32], kem[65];
        unsigned x = common_ancestor(next->me, a->added[i]);
        sb_t gs = {0}, ids = {0}, ct = {0};
        int k;
        for (k = 0; k < ps->n && ps->path[k] != x; k++)
            ;
        tls_reader(&r, a->added_kp[i].p, a->added_kp[i].n);
        ok = k < ps->n && mls_key_package_read(&kp, &r);
        if (!ok)
            break;
        tls_vec(&gs, next->keys.joiner, 32);
        tls_u8(&gs, 1);
        tls_vec(&gs, ps->secret[k], 32);
        for (int j = 0; j < a->npsks; j++)
            sb_addn(&ids, (const char *)a->psk_ids[j].p, a->psk_ids[j].n);
        tls_vec(&gs, ids.data, ids.len);
        ok = mls_encrypt_with_label(kp.init_key, "Welcome", egi.data, egi.len, gs.data, gs.len, kem, &ct);
        mls_key_package_ref(a->added_kp[i].p, a->added_kp[i].n, ref);
        tls_vec(&secrets, ref, 32);
        tls_vec(&secrets, kem, 65);
        tls_vec(&secrets, ct.data, ct.len);
        mls_key_package_free(&kp);
        secure_wipe(gs.data, gs.len);
        sb_free(&gs);
        sb_free(&ids);
        sb_free(&ct);
    }
    if (ok) {
        tls_u16(out, 2);
        tls_vec(out, secrets.data, secrets.len);
        tls_vec(out, egi.data, egi.len);
    }
    secure_wipe(key, sizeof key);
    secure_wipe(gi.data, gi.len);
    sb_free(&gi);
    sb_free(&ext);
    sb_free(&tree);
    sb_free(&sig);
    sb_free(&egi);
    sb_free(&secrets);
    return ok;
}

int mls_group_commit(const mls_group_t *g, sb_t *commit, sb_t *welcome, mls_group_t *next)
{
    resolved_t *list = mem_alloc(sizeof *list * (size_t)(g->nprops + 1));
    applied_t a = {0};
    mls_path_secrets_t ps;
    mls_tree_t t = {0};
    mls_content_t m;
    mls_bytes_t ext = bytes_of(&g->extensions);
    unsigned char commit_secret[32], psk[32], th[32], cth[32], tag[32], mtag[32];
    sb_t gc = {0}, path = {0}, body = {0}, framed = {0}, tbs = {0}, sig = {0}, auth = {0};
    int ok = 0, applied = 0;

    memset(next, 0, sizeof *next);
    for (int i = 0; i < g->nprops; i++) {
        tls_reader_t r;
        tls_reader(&r, g->props[i].proposal.data, g->props[i].proposal.len);
        if (!mls_proposal_read(&list[i].p, &r))
            goto out;
        list[i].sender_type = g->props[i].sender_type;
        list[i].sender = g->props[i].sender;
    }
    mls_tree_copy(&t, &g->tree);
    applied = 1;
    if (!apply_proposals(g, &t, list, g->nprops, g->me, &a) || a.removed_me)
        goto out;
    if (a.has_ext)
        ext = a.ext;
    if (!mls_path_create(&t, g->me, g->sig_priv, g->group_id.data, g->group_id.len, &ps, commit_secret))
        goto out;
    root_hash(&t, th);
    mls_group_context(&gc, g->group_id.data, g->group_id.len, g->epoch + 1, th, g->cth, g->cth_n, ext.p, ext.n);
    if (!mls_path_encrypt(&t, g->me, &ps, gc.data, gc.len, a.added, a.nadded, &path))
        goto out;

    /* FramedContent with the Commit: every proposal by reference, then the path. */
    for (int i = 0; i < g->nprops; i++) {
        tls_u8(&body, 2);
        tls_vec(&body, g->props[i].ref, 32);
    }
    tls_vec(&framed, g->group_id.data, g->group_id.len);
    tls_u64(&framed, g->epoch);
    tls_u8(&framed, MLS_SENDER_MEMBER);
    tls_u32(&framed, g->me);
    tls_varint(&framed, 0);
    tls_u8(&framed, MLS_CONTENT_COMMIT);
    tls_vec(&framed, body.data, body.len);
    tls_u8(&framed, 1);
    sb_addn(&framed, path.data, path.len);

    memset(&m, 0, sizeof m);
    m.wire_format = MLS_WIRE_PUBLIC;
    m.sender_type = MLS_SENDER_MEMBER;
    m.sender = g->me;
    m.content_type = MLS_CONTENT_COMMIT;
    m.framed.p = (const unsigned char *)framed.data;
    m.framed.n = framed.len;
    sb_clear(&gc);
    mls_group_context_of(g, &gc);
    mls_content_tbs(&m, gc.data, gc.len, &tbs);
    if (!mls_sign_with_label(g->sig_priv, "FramedContentTBS", tbs.data, tbs.len, &sig))
        goto out;
    m.signature.p = (const unsigned char *)sig.data;
    m.signature.n = sig.len;
    mls_transcript(g->interim, 32, &m, cth, tag); /* the interim part is redone with the real tag below */

    /* The next state. */
    sb_copy(&next->group_id, &g->group_id);
    if (ext.n)
        sb_addn(&next->extensions, (const char *)ext.p, ext.n);
    next->epoch = g->epoch + 1;
    next->tree = t;
    memset(&t, 0, sizeof t);
    memcpy(next->cth, cth, 32);
    next->cth_n = 32;
    next->me = g->me;
    memcpy(next->sig_priv, g->sig_priv, 32);
    next->psks = g->psks;
    next->npsks = g->npsks;
    sb_clear(&gc);
    mls_group_context_of(next, &gc);
    if (!psk_secret_of(g, a.psk_ids, a.npsks, psk) ||
        !mls_key_schedule(g->keys.init, commit_secret, psk, gc.data, gc.len, &next->keys))
        goto out;
    hmac256(next->keys.confirm, 32, cth, 32, tag);
    mls_interim(cth, 32, tag, 32, next->interim);
    memcpy(next->resumption_epoch, g->resumption_epoch, sizeof g->resumption_epoch);
    memcpy(next->resumption, g->resumption, sizeof g->resumption);
    next->nresumption = g->nresumption;
    remember_resumption(next);

    tls_vec(&auth, sig.data, sig.len);
    tls_vec(&auth, tag, 32);
    m.auth.p = (const unsigned char *)auth.data;
    m.auth.n = auth.len;
    sb_clear(&gc);
    mls_group_context_of(g, &gc);
    mls_membership_tag(&m, g->keys.membership, gc.data, gc.len, mtag);
    tls_u16(commit, 1);
    tls_u16(commit, MLS_WIRE_PUBLIC);
    sb_addn(commit, framed.data, framed.len);
    sb_addn(commit, auth.data, auth.len);
    tls_vec(commit, mtag, 32);
    ok = !a.nadded || make_welcome(next, &a, &ps, psk, tag, welcome);
out:
    secure_wipe(commit_secret, sizeof commit_secret);
    secure_wipe(&ps, sizeof ps);
    if (applied)
        applied_free(&a);
    if (!ok)
        mls_group_free(next);
    mls_tree_free(&t);
    mem_free(list);
    sb_free(&gc);
    sb_free(&path);
    sb_free(&body);
    sb_free(&framed);
    sb_free(&tbs);
    sb_free(&sig);
    sb_free(&auth);
    return ok;
}
