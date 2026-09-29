#include <string.h>
#include "mls_path.h"
#include "hpke.h"
#include "mem.h"
#include "mls_crypto.h"
#include "rng.h"
#include "sha2.h"

static const unsigned char *read_key(tls_reader_t *r)
{
    size_t n;
    const unsigned char *p = tls_read_vec(r, &n);

    return !r->bad && n == 65 && p[0] == 4 ? p : NULL;
}

int mls_path_read(mls_update_path_t *p, tls_reader_t *r)
{
    tls_reader_t nodes;
    size_t used;
    int cap = 0;

    memset(p, 0, sizeof *p);
    if (r->bad || !mls_leaf_parse(&p->leaf, r->p, (size_t)(r->end - r->p), &used))
        return 0;
    r->p += used;
    nodes = tls_read_nested(r);
    while (!nodes.bad && nodes.p < nodes.end) {
        mls_path_node_t *node;
        const unsigned char *key = read_key(&nodes);
        tls_reader_t cts;
        int ccap = 0;
        if (!key || p->n == MLS_MAX_PATH)
            goto bad;
        if (p->n == cap) {
            cap = cap ? cap * 2 : 4;
            p->nodes = mem_realloc(p->nodes, sizeof *p->nodes * (size_t)cap);
        }
        node = &p->nodes[p->n++];
        memset(node, 0, sizeof *node);
        memcpy(node->key, key, 65);
        cts = tls_read_nested(&nodes);
        while (!cts.bad && cts.p < cts.end) {
            mls_hpke_ct_t *c;
            const unsigned char *kem = read_key(&cts);
            if (!kem)
                goto bad;
            if (node->nct == ccap) {
                ccap = ccap ? ccap * 2 : 4;
                node->ct = mem_realloc(node->ct, sizeof *node->ct * (size_t)ccap);
            }
            c = &node->ct[node->nct++];
            c->kem = kem;
            c->ct = tls_read_vec(&cts, &c->ctn);
        }
        if (cts.bad)
            goto bad;
    }
    if (!nodes.bad && !r->bad)
        return 1;
bad:
    mls_path_free(p);
    return 0;
}

void mls_path_free(mls_update_path_t *p)
{
    for (int i = 0; i < p->n; i++)
        mem_free(p->nodes[i].ct);
    mem_free(p->nodes);
    mls_node_clear(&p->leaf);
    memset(p, 0, sizeof *p);
}

int mls_node_set_secret(mls_node_t *node, const unsigned char path_secret[32])
{
    unsigned char ns[32], pk[65];

    node->has_priv = mls_derive_secret(path_secret, "node", ns) && hpke_derive_keypair(ns, 32, node->priv, pk) &&
                     ct_equal(pk, node->key, 65);
    secure_wipe(ns, sizeof ns);
    if (!node->has_priv)
        secure_wipe(node->priv, sizeof node->priv);
    return node->has_priv;
}

int mls_path_next_secret(unsigned char secret[32])
{
    unsigned char next[32];
    int ok = mls_derive_secret(secret, "path", next);

    memcpy(secret, next, 32);
    secure_wipe(next, sizeof next);
    return ok;
}

/* Blanks the direct path of `leaf` and puts fresh keys on its filtered part. */
static void reset_path(mls_tree_t *t, unsigned leaf, const unsigned *path, int n)
{
    mls_tree_blank_path(t, leaf);
    for (int k = 0; k < n; k++)
        t->nodes[path[k]].present = 1;
}

/* Parent hashes from the root down; `leaf_ph` gets the one the leaf must carry. */
static void set_parent_hashes(mls_tree_t *t, const unsigned *path, const unsigned *copath, int n,
                              unsigned char leaf_ph[32])
{
    for (int k = n - 1; k >= 0; k--) {
        mls_node_t *node = &t->nodes[path[k]];
        unsigned char ph[32];
        mls_parent_hash(t, path[k], copath[k], node->parent_hash.data, node->parent_hash.len, ph);
        if (k) {
            sb_clear(&t->nodes[path[k - 1]].parent_hash);
            sb_addn(&t->nodes[path[k - 1]].parent_hash, (const char *)ph, 32);
        } else {
            memcpy(leaf_ph, ph, 32);
        }
    }
}

int mls_path_merge(mls_tree_t *t, unsigned sender, const mls_update_path_t *p)
{
    unsigned path[MLS_MAX_PATH], copath[MLS_MAX_PATH];
    unsigned char ph[32];
    int n;

    if (sender >= t->nleaves || !t->nodes[2 * sender].present || p->leaf.source != 3)
        return 0;
    n = mls_filtered_path(t, sender, path, copath);
    if (n != p->n)
        return 0;
    reset_path(t, sender, path, n);
    for (int k = 0; k < n; k++)
        memcpy(t->nodes[path[k]].key, p->nodes[k].key, 65);
    set_parent_hashes(t, path, copath, n, ph);
    if (n ? p->leaf.parent_hash.len != 32 || !ct_equal(p->leaf.parent_hash.data, ph, 32) : p->leaf.parent_hash.len != 0)
        return 0;
    mls_node_clear(&t->nodes[2 * sender]);
    mls_node_copy(&t->nodes[2 * sender], &p->leaf);
    return 1;
}

/* The resolution of `x` without the leaves added in this commit. */
static int resolution_without(const mls_tree_t *t, unsigned x, const unsigned *added, int nadded, unsigned *res)
{
    int n = mls_resolution(t, x, res), m = 0;

    for (int i = 0; i < n; i++) {
        int k = 0;
        while (k < nadded && res[i] != 2 * added[k])
            k++;
        if (k == nadded)
            res[m++] = res[i];
    }
    return m;
}

int mls_path_decrypt(mls_tree_t *t, unsigned me, unsigned sender, const mls_update_path_t *p, const void *gc,
                     size_t gcn, const unsigned *added, int nadded, unsigned char path_secret[32],
                     unsigned char commit_secret[32])
{
    unsigned path[MLS_MAX_PATH], copath[MLS_MAX_PATH], *res;
    unsigned char secret[32];
    sb_t pt = {0};
    int n, k, nres, j, ok = 0;

    if (me == sender || me >= t->nleaves)
        return 0;
    n = mls_filtered_path(t, sender, path, copath);
    if (n != p->n)
        return 0;
    for (k = 0; k < n && !mls_in_subtree(2 * me, copath[k]); k++)
        ;
    if (k == n)
        return 0;
    res = mem_alloc(sizeof *res * mls_nodes(t->nleaves));
    nres = resolution_without(t, copath[k], added, nadded, res);
    for (j = 0; j < nres && !t->nodes[res[j]].has_priv; j++)
        ;
    if (j < nres && nres == p->nodes[k].nct) {
        const mls_hpke_ct_t *c = &p->nodes[k].ct[j];
        ok = mls_decrypt_with_label(t->nodes[res[j]].priv, "UpdatePathNode", gc, gcn, c->kem, c->ct, c->ctn, &pt) &&
             pt.len == 32;
    }
    mem_free(res);
    if (!ok) {
        sb_free(&pt);
        return 0;
    }
    memcpy(secret, pt.data, 32);
    sb_free(&pt);
    if (path_secret)
        memcpy(path_secret, secret, 32);
    for (; ok && k < n; k++)
        ok = mls_node_set_secret(&t->nodes[path[k]], secret) && mls_path_next_secret(secret);
    if (ok)
        memcpy(commit_secret, secret, 32);
    secure_wipe(secret, sizeof secret);
    return ok;
}

int mls_path_create(mls_tree_t *t, unsigned me, const unsigned char sig_priv[32], const void *group_id, size_t gn,
                    mls_path_secrets_t *ps, unsigned char commit_secret[32])
{
    unsigned copath[MLS_MAX_PATH];
    unsigned char secret[32], ph[32], seed[32], leaf_priv[32], leaf_pub[65];
    const mls_node_t *old = &t->nodes[2 * me];
    sb_t leaf = {0}, tbs = {0}, sig = {0};
    mls_node_t fresh;
    int ok;

    if (me >= t->nleaves || !old->present)
        return 0;
    ps->n = mls_filtered_path(t, me, ps->path, copath);
    if (!rng_bytes(secret, 32) || !rng_bytes(seed, 32) || !hpke_derive_keypair(seed, 32, leaf_priv, leaf_pub))
        return 0;
    reset_path(t, me, ps->path, ps->n);
    for (int k = 0; k < ps->n; k++) {
        mls_node_t *node = &t->nodes[ps->path[k]];
        unsigned char ns[32];
        memcpy(ps->secret[k], secret, 32);
        if (!mls_derive_secret(secret, "node", ns) || !hpke_derive_keypair(ns, 32, node->priv, node->key) ||
            !mls_path_next_secret(secret))
            return 0;
        node->has_priv = 1;
        secure_wipe(ns, sizeof ns);
    }
    memcpy(commit_secret, secret, 32);
    set_parent_hashes(t, ps->path, copath, ps->n, ph);

    /* The leaf keeps its credential, capabilities and extensions. */
    tls_vec(&leaf, leaf_pub, 65);
    sb_addn(&leaf, old->leaf.data + 67, old->source_off - 67); /* after the key: a 2-byte length and 65 bytes */
    tls_u8(&leaf, 3);
    tls_vec(&leaf, ph, ps->n ? 32 : 0);
    sb_addn(&leaf, old->leaf.data + old->ext_off, old->tbs_n - old->ext_off);
    sb_addn(&tbs, leaf.data, leaf.len);
    tls_vec(&tbs, group_id, gn);
    tls_u32(&tbs, me);
    ok = mls_sign_with_label(sig_priv, "LeafNodeTBS", tbs.data, tbs.len, &sig);
    tls_vec(&leaf, sig.data, sig.len);
    ok = ok && mls_leaf_parse(&fresh, (const unsigned char *)leaf.data, leaf.len, NULL);
    if (ok) {
        memcpy(fresh.priv, leaf_priv, 32);
        fresh.has_priv = 1;
        mls_node_clear(&t->nodes[2 * me]);
        t->nodes[2 * me] = fresh;
    }
    secure_wipe(secret, sizeof secret);
    secure_wipe(seed, sizeof seed);
    secure_wipe(leaf_priv, sizeof leaf_priv);
    sb_free(&leaf);
    sb_free(&tbs);
    sb_free(&sig);
    return ok;
}

int mls_path_encrypt(const mls_tree_t *t, unsigned me, const mls_path_secrets_t *ps, const void *gc, size_t gcn,
                     const unsigned *added, int nadded, sb_t *out)
{
    unsigned path[MLS_MAX_PATH], copath[MLS_MAX_PATH], *res = mem_alloc(sizeof *res * mls_nodes(t->nleaves));
    sb_t nodes = {0};
    int n = mls_filtered_path(t, me, path, copath), ok = n == ps->n;

    for (int k = 0; ok && k < n; k++) {
        int nres = resolution_without(t, copath[k], added, nadded, res);
        sb_t cts = {0};
        tls_vec(&nodes, t->nodes[path[k]].key, 65);
        for (int j = 0; ok && j < nres; j++) {
            unsigned char kem[65];
            sb_t ct = {0};
            ok = mls_encrypt_with_label(t->nodes[res[j]].key, "UpdatePathNode", gc, gcn, ps->secret[k], 32, kem, &ct);
            tls_vec(&cts, kem, 65);
            tls_vec(&cts, ct.data, ct.len);
            sb_free(&ct);
        }
        tls_vec(&nodes, cts.data, cts.len);
        sb_free(&cts);
    }
    if (ok) {
        const mls_node_t *leaf = &t->nodes[2 * me];
        sb_addn(out, leaf->leaf.data, leaf->leaf.len);
        tls_vec(out, nodes.data, nodes.len);
    }
    mem_free(res);
    sb_free(&nodes);
    return ok;
}
