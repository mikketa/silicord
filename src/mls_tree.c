#include <string.h>
#include "mls_tree.h"
#include "mem.h"
#include "mls_crypto.h"
#include "sha2.h"
#include "tls.h"

/* ---- Tree math (RFC 9420, appendix C) ---- */

static unsigned mls_level(unsigned x)
{
    unsigned k = 0;

    while ((x >> k) & 1)
        k++;
    return k;
}

unsigned mls_root(unsigned nleaves)
{
    return nleaves ? nleaves - 1 : 0;
}

unsigned mls_left(unsigned x)
{
    unsigned k = mls_level(x);

    return k ? x ^ (1u << (k - 1)) : x;
}

unsigned mls_right(unsigned x)
{
    unsigned k = mls_level(x);

    return k ? x ^ (3u << (k - 1)) : x;
}

unsigned mls_parent(unsigned x)
{
    unsigned k = mls_level(x), b = (x >> (k + 1)) & 1;

    return (x | (1u << k)) ^ (b << (k + 1));
}

unsigned mls_sibling(unsigned x)
{
    unsigned p = mls_parent(x);

    return x < p ? mls_right(p) : mls_left(p);
}

int mls_in_subtree(unsigned x, unsigned top)
{
    unsigned half = (1u << mls_level(top)) - 1;

    return x + half >= top && x <= top + half;
}

/* ---- Nodes ---- */

void mls_node_clear(mls_node_t *node)
{
    sb_free(&node->parent_hash);
    sb_free(&node->leaf);
    sb_free(&node->identity);
    mem_free(node->unmerged);
    secure_wipe(node, sizeof *node);
}

void mls_node_copy(mls_node_t *dst, const mls_node_t *src)
{
    *dst = *src;
    memset(&dst->parent_hash, 0, sizeof dst->parent_hash);
    memset(&dst->leaf, 0, sizeof dst->leaf);
    memset(&dst->identity, 0, sizeof dst->identity);
    dst->unmerged = NULL;
    if (src->parent_hash.len)
        sb_addn(&dst->parent_hash, src->parent_hash.data, src->parent_hash.len);
    if (src->leaf.len)
        sb_addn(&dst->leaf, src->leaf.data, src->leaf.len);
    if (src->identity.len)
        sb_addn(&dst->identity, src->identity.data, src->identity.len);
    if (src->nunmerged) {
        dst->unmerged = mem_alloc(sizeof *dst->unmerged * (size_t)src->nunmerged);
        memcpy(dst->unmerged, src->unmerged, sizeof *dst->unmerged * (size_t)src->nunmerged);
    }
}

int mls_point_read(tls_reader_t *r, unsigned char out[65])
{
    size_t n;
    const unsigned char *p = tls_read_vec(r, &n);

    if (r->bad || n != 65 || p[0] != 4)
        return 0;
    memcpy(out, p, 65);
    return 1;
}

/* A vector of uint16 values (capabilities). */
static void skip_u16s(tls_reader_t *r)
{
    tls_reader_t v = tls_read_nested(r);

    while (!v.bad && v.p < v.end)
        tls_read_u16(&v);
    if (v.bad)
        r->bad = 1;
}

static int leaf_read(mls_node_t *node, tls_reader_t *r)
{
    const unsigned char *start = r->p, *id;
    size_t n, sn;
    tls_reader_t ext;

    if (!mls_point_read(r, node->key) || !mls_point_read(r, node->sig_key))
        return 0;
    if (tls_read_u16(r) != 1) /* only basic credentials */
        return 0;
    id = tls_read_vec(r, &n);
    if (r->bad)
        return 0;
    sb_addn(&node->identity, (const char *)id, n);
    for (int i = 0; i < 5; i++) /* versions, cipher_suites, extensions, proposals, credentials */
        skip_u16s(r);
    node->source_off = (size_t)(r->p - start);
    node->source = (int)tls_read_u8(r);
    if (node->source == 1) {
        tls_read_u64(r); /* lifetime */
        tls_read_u64(r);
    } else if (node->source == 3) {
        const unsigned char *ph = tls_read_vec(r, &n);
        if (r->bad)
            return 0;
        sb_addn(&node->parent_hash, (const char *)ph, n);
    } else if (node->source != 2) {
        return 0;
    }
    node->ext_off = (size_t)(r->p - start);
    ext = tls_read_nested(r);
    while (!ext.bad && ext.p < ext.end) {
        tls_read_u16(&ext);
        tls_read_vec(&ext, &n);
    }
    if (ext.bad)
        return 0;
    node->tbs_n = (size_t)(r->p - start);
    tls_read_vec(r, &sn); /* signature */
    if (r->bad || !sn)
        return 0;
    sb_addn(&node->leaf, (const char *)start, (size_t)(r->p - start));
    node->present = 1;
    return 1;
}

int mls_leaf_parse(mls_node_t *node, const unsigned char *data, size_t n, size_t *used)
{
    tls_reader_t r;

    memset(node, 0, sizeof *node);
    tls_reader(&r, data, n);
    if (!leaf_read(node, &r)) {
        mls_node_clear(node);
        return 0;
    }
    if (used)
        *used = (size_t)(r.p - data);
    return 1;
}

static int parent_read(mls_node_t *node, tls_reader_t *r)
{
    const unsigned char *ph;
    size_t n;
    tls_reader_t um;

    if (!mls_point_read(r, node->key))
        return 0;
    ph = tls_read_vec(r, &n);
    if (r->bad)
        return 0;
    sb_addn(&node->parent_hash, (const char *)ph, n);
    um = tls_read_nested(r);
    if (um.bad || (um.end - um.p) % 4)
        return 0;
    node->nunmerged = (int)((um.end - um.p) / 4);
    if (node->nunmerged)
        node->unmerged = mem_alloc(sizeof *node->unmerged * (size_t)node->nunmerged);
    for (int i = 0; i < node->nunmerged; i++) {
        node->unmerged[i] = (unsigned)tls_read_u32(&um);
        if (i && node->unmerged[i] <= node->unmerged[i - 1])
            return 0;
    }
    node->present = 1;
    return 1;
}

/* ---- Trees ---- */

void mls_tree_free(mls_tree_t *t)
{
    for (unsigned i = 0; i < mls_nodes(t->nleaves); i++)
        mls_node_clear(&t->nodes[i]);
    mem_free(t->nodes);
    t->nodes = NULL;
    t->nleaves = 0;
}

void mls_tree_copy(mls_tree_t *dst, const mls_tree_t *src)
{
    unsigned n = mls_nodes(src->nleaves);

    dst->nleaves = src->nleaves;
    dst->nodes = n ? mem_alloc(sizeof *dst->nodes * n) : NULL;
    for (unsigned i = 0; i < n; i++)
        mls_node_copy(&dst->nodes[i], &src->nodes[i]);
}

void mls_tree_extend(mls_tree_t *t, unsigned nleaves)
{
    unsigned old = mls_nodes(t->nleaves), n = t->nleaves ? t->nleaves : 1;

    while (n < nleaves)
        n *= 2;
    if (n == t->nleaves)
        return;
    t->nodes = mem_realloc(t->nodes, sizeof *t->nodes * mls_nodes(n));
    memset(t->nodes + old, 0, sizeof *t->nodes * (mls_nodes(n) - old));
    t->nleaves = n;
}

void mls_tree_truncate(mls_tree_t *t)
{
    while (t->nleaves > 1) {
        unsigned half = t->nleaves / 2, i;
        /* The root is node nleaves - 1; its right subtree follows it. */
        for (i = t->nleaves; i < mls_nodes(t->nleaves) && !t->nodes[i].present; i++)
            ;
        if (i < mls_nodes(t->nleaves))
            break;
        for (i = mls_nodes(half); i < mls_nodes(t->nleaves); i++)
            mls_node_clear(&t->nodes[i]);
        t->nleaves = half;
    }
}

void mls_tree_blank_path(mls_tree_t *t, unsigned leaf)
{
    unsigned x = 2 * leaf, root = mls_root(t->nleaves);

    while (x != root) {
        x = mls_parent(x);
        mls_node_clear(&t->nodes[x]);
    }
}

int mls_tree_parse(mls_tree_t *t, const unsigned char *data, size_t n)
{
    tls_reader_t r, v;
    unsigned count = 0, cap = 0;
    mls_node_t *nodes = NULL;

    memset(t, 0, sizeof *t);
    tls_reader(&r, data, n);
    v = tls_read_nested(&r);
    if (!tls_done(&r))
        return 0;
    while (!v.bad && v.p < v.end) {
        mls_node_t *node;
        unsigned present, type;
        if (count == cap) {
            cap = cap ? cap * 2 : 16;
            nodes = mem_realloc(nodes, sizeof *nodes * cap);
            memset(nodes + count, 0, sizeof *nodes * (cap - count));
        }
        node = &nodes[count++];
        present = tls_read_u8(&v);
        if (present > 1)
            goto bad;
        if (!present)
            continue;
        type = tls_read_u8(&v);
        if (type != ((count - 1) % 2 ? 2u : 1u))
            goto bad;
        if (!(type == 1 ? leaf_read(node, &v) : parent_read(node, &v)))
            goto bad;
    }
    /* A non-empty, odd count of nodes, the last one present. */
    if (v.bad || !count || !(count % 2) || !nodes[count - 1].present)
        goto bad;
    /* Grow to a full tree; the nodes past `count` are already blank. */
    t->nleaves = 1;
    while (mls_nodes(t->nleaves) < count)
        t->nleaves *= 2;
    t->nodes = mem_realloc(nodes, sizeof *nodes * mls_nodes(t->nleaves));
    if (mls_nodes(t->nleaves) > cap)
        memset(t->nodes + cap, 0, sizeof *nodes * (mls_nodes(t->nleaves) - cap));
    /* Unmerged leaves must be leaves under their parent. */
    for (unsigned i = 1; i < mls_nodes(t->nleaves); i += 2)
        for (int k = 0; k < t->nodes[i].nunmerged; k++)
            if (!mls_in_subtree(2 * t->nodes[i].unmerged[k], i) ||
                2 * t->nodes[i].unmerged[k] >= mls_nodes(t->nleaves)) {
                mls_tree_free(t);
                return 0;
            }
    return 1;
bad:
    for (unsigned i = 0; i < count; i++)
        mls_node_clear(&nodes[i]);
    mem_free(nodes);
    return 0;
}

static int excluded(unsigned leaf, const unsigned *excl, int nexcl)
{
    for (int k = 0; k < nexcl; k++)
        if (excl[k] == leaf)
            return 1;
    return 0;
}

/* A ParentNode, without the unmerged leaves in `excl`. */
static void parent_encode(const mls_node_t *node, sb_t *out, const unsigned *excl, int nexcl)
{
    int keep = 0;

    tls_vec(out, node->key, 65);
    tls_vec(out, node->parent_hash.data, node->parent_hash.len);
    for (int i = 0; i < node->nunmerged; i++)
        keep += !excluded(node->unmerged[i], excl, nexcl);
    tls_varint(out, 4 * (size_t)keep);
    for (int i = 0; i < node->nunmerged; i++)
        if (!excluded(node->unmerged[i], excl, nexcl))
            tls_u32(out, node->unmerged[i]);
}

void mls_tree_serialize(const mls_tree_t *t, sb_t *out)
{
    sb_t body = {0};
    unsigned last = mls_nodes(t->nleaves);

    while (last && !t->nodes[last - 1].present)
        last--;
    for (unsigned i = 0; i < last; i++) {
        const mls_node_t *node = &t->nodes[i];
        tls_u8(&body, (unsigned)node->present);
        if (!node->present)
            continue;
        tls_u8(&body, i % 2 ? 2 : 1);
        if (i % 2)
            parent_encode(node, &body, NULL, 0);
        else
            sb_addn(&body, node->leaf.data, node->leaf.len);
    }
    tls_vec(out, body.data, body.len);
    sb_free(&body);
}

/* ---- Hashes and resolutions ---- */

/* The tree hash with the leaves in `excl` blanked and dropped from unmerged lists. */
static void tree_hash_ex(const mls_tree_t *t, unsigned x, const unsigned *excl, int nexcl, unsigned char out[32])
{
    const mls_node_t *node = &t->nodes[x];
    sb_t in = {0};

    if (!(x % 2)) {
        int present = node->present && !excluded(x / 2, excl, nexcl);
        tls_u8(&in, 1);
        tls_u32(&in, x / 2);
        tls_u8(&in, (unsigned)present);
        if (present)
            sb_addn(&in, node->leaf.data, node->leaf.len);
    } else {
        unsigned char lh[32], rh[32];
        tree_hash_ex(t, mls_left(x), excl, nexcl, lh);
        tree_hash_ex(t, mls_right(x), excl, nexcl, rh);
        tls_u8(&in, 2);
        tls_u8(&in, (unsigned)node->present);
        if (node->present)
            parent_encode(node, &in, excl, nexcl);
        tls_vec(&in, lh, 32);
        tls_vec(&in, rh, 32);
    }
    sha256_once(in.data, in.len, out);
    sb_free(&in);
}

void mls_tree_hash(const mls_tree_t *t, unsigned node, unsigned char out[32])
{
    tree_hash_ex(t, node, NULL, 0, out);
}

int mls_resolution(const mls_tree_t *t, unsigned x, unsigned *out)
{
    const mls_node_t *node = &t->nodes[x];
    int n;

    if (node->present) {
        out[0] = x;
        for (int k = 0; k < node->nunmerged; k++)
            out[1 + k] = 2 * node->unmerged[k];
        return 1 + node->nunmerged;
    }
    if (!(x % 2))
        return 0;
    n = mls_resolution(t, mls_left(x), out);
    return n + mls_resolution(t, mls_right(x), out + n);
}

int mls_leaf_verify(const mls_node_t *node, const void *group_id, size_t gn, unsigned leaf_index)
{
    sb_t tbs = {0};
    tls_reader_t r;
    const unsigned char *sig;
    size_t sn;
    int ok;

    sb_addn(&tbs, node->leaf.data, node->tbs_n);
    if (node->source != 1) {
        tls_vec(&tbs, group_id, gn);
        tls_u32(&tbs, leaf_index);
    }
    tls_reader(&r, node->leaf.data + node->tbs_n, node->leaf.len - node->tbs_n);
    sig = tls_read_vec(&r, &sn);
    ok = tls_done(&r) && mls_verify_with_label(node->sig_key, "LeafNodeTBS", tbs.data, tbs.len, sig, sn);
    sb_free(&tbs);
    return ok;
}

int mls_tree_verify_leaves(const mls_tree_t *t, const void *group_id, size_t gn)
{
    for (unsigned i = 0; i < t->nleaves; i++)
        if (t->nodes[2 * i].present && !mls_leaf_verify(&t->nodes[2 * i], group_id, gn, i))
            return 0;
    return 1;
}

int mls_filtered_path(const mls_tree_t *t, unsigned leaf, unsigned *path, unsigned *copath)
{
    unsigned x = 2 * leaf, root = mls_root(t->nleaves), *res = mem_alloc(sizeof *res * mls_nodes(t->nleaves));
    int n = 0;

    while (x != root) {
        unsigned p = mls_parent(x), s = mls_sibling(x);
        if (mls_resolution(t, s, res)) {
            path[n] = p;
            copath[n++] = s;
        }
        x = p;
    }
    mem_free(res);
    return n;
}

void mls_parent_hash(const mls_tree_t *t, unsigned p, unsigned s, const void *above, size_t an, unsigned char out[32])
{
    const mls_node_t *node = &t->nodes[p];
    unsigned char sh[32];
    sb_t in = {0};

    tree_hash_ex(t, s, node->unmerged, node->nunmerged, sh);
    tls_vec(&in, node->key, 65);
    tls_vec(&in, above, an);
    tls_vec(&in, sh, 32);
    sha256_once(in.data, in.len, out);
    sb_free(&in);
}

/* Whether `d` in the resolution of child `c` holds P's parent hash, with P's
   unmerged leaves under C being exactly the rest of that resolution. */
static int valid_for(const mls_tree_t *t, unsigned p, unsigned c, unsigned d, const unsigned *res, int nres,
                     const unsigned char ph[32])
{
    const mls_node_t *pn = &t->nodes[p], *dn = &t->nodes[d];
    int under = 0;

    if (dn->parent_hash.len != 32 || !ct_equal(dn->parent_hash.data, ph, 32))
        return 0;
    for (int k = 0; k < pn->nunmerged; k++) {
        unsigned leaf = 2 * pn->unmerged[k];
        int found = 0;
        if (!mls_in_subtree(leaf, c))
            continue;
        under++;
        for (int j = 0; j < nres; j++)
            found |= res[j] == leaf && leaf != d;
        if (!found)
            return 0;
    }
    return under == nres - 1;
}

int mls_tree_verify_parent_hashes(const mls_tree_t *t)
{
    unsigned n = mls_nodes(t->nleaves), *res = n ? mem_alloc(sizeof *res * n) : NULL;
    int ok = 1;

    for (unsigned p = 1; ok && p < n; p += 2) {
        const mls_node_t *pn = &t->nodes[p];
        int valid = 0;
        if (!pn->present)
            continue;
        for (int side = 0; side < 2; side++) {
            unsigned c = side ? mls_right(p) : mls_left(p), s = side ? mls_left(p) : mls_right(p);
            unsigned char ph[32];
            int nres = mls_resolution(t, c, res);
            mls_parent_hash(t, p, s, pn->parent_hash.data, pn->parent_hash.len, ph);
            for (int j = 0; j < nres; j++)
                valid += valid_for(t, p, c, res[j], res, nres, ph);
        }
        ok = valid == 1;
    }
    mem_free(res);
    return ok;
}
