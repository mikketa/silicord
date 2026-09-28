#pragma once
#include <stddef.h>
#include "sb.h"

/*
 * MLS ratchet trees (RFC 9420, section 7) as arrays: leaves at even node
 * indices, parents at odd ones, always a full tree (a power of two leaves).
 */

typedef struct {
    int present;
    unsigned char key[65];      /* encryption_key (HPKE, P-256) */
    sb_t parent_hash;           /* parents, and leaves added by a commit */
    unsigned *unmerged;         /* parents: leaf indices, ascending */
    int nunmerged;
    /* Leaves */
    sb_t leaf;                  /* the whole serialized LeafNode */
    size_t tbs_n;               /* the bytes before its signature */
    unsigned char sig_key[65];
    sb_t identity;              /* basic credential identity */
    int source;                 /* 1 key_package, 2 update, 3 commit */
    /* Our private key for this node, when we hold it */
    int has_priv;
    unsigned char priv[32];
} mls_node_t;

typedef struct {
    mls_node_t *nodes;
    unsigned nleaves;
} mls_tree_t;

/* Tree math on node indices. */
unsigned mls_level(unsigned x);
unsigned mls_root(unsigned nleaves);
unsigned mls_left(unsigned x);
unsigned mls_right(unsigned x);
unsigned mls_parent(unsigned x);
unsigned mls_sibling(unsigned x);
static __inline unsigned mls_nodes(unsigned nleaves)
{
    return nleaves ? 2 * nleaves - 1 : 0;
}

/* A LeafNode from its encoding (the reader's position moves past it). */
int mls_leaf_parse(mls_node_t *node, const unsigned char *data, size_t n, size_t *used);
/* The ratchet_tree extension: optional<Node> tree<V>. */
int mls_tree_parse(mls_tree_t *t, const unsigned char *data, size_t n);
void mls_tree_serialize(const mls_tree_t *t, sb_t *out);
void mls_tree_free(mls_tree_t *t);
void mls_node_clear(mls_node_t *node);
int mls_tree_copy(mls_tree_t *dst, const mls_tree_t *src);
/* Grows the tree to hold at least `nleaves` leaves (blank ones). */
void mls_tree_extend(mls_tree_t *t, unsigned nleaves);
/* Drops the right half while it is all blank. */
void mls_tree_truncate(mls_tree_t *t);

/* The tree hash of a node's subtree. */
void mls_tree_hash(const mls_tree_t *t, unsigned node, unsigned char out[32]);
/* The resolution of a node into `out` (room for every node); returns its size. */
int mls_resolution(const mls_tree_t *t, unsigned node, unsigned *out);
/* Every leaf's signature (group_id is the context for update and commit leaves). */
int mls_tree_verify_leaves(const mls_tree_t *t, const void *group_id, size_t gn);
/* Every non-blank parent is parent-hash valid for exactly one descendant. */
int mls_tree_verify_parent_hashes(const mls_tree_t *t);
/* ParentHash(P) with copath child S, where P's parent_hash field is `above`. */
void mls_parent_hash(const mls_tree_t *t, unsigned p, unsigned s, const void *above, size_t an, unsigned char out[32]);
