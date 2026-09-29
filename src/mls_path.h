#pragma once
#include <stddef.h>
#include "mls_tree.h"
#include "sb.h"
#include "tls.h"

/*
 * TreeKEM (RFC 9420, sections 7.4 to 7.6): update paths that give a
 * committer's fresh keys to the rest of the group.
 */

#define MLS_MAX_PATH 32

typedef struct {
    const unsigned char *kem, *ct; /* kem_output (65 bytes) and ciphertext, in the input */
    size_t ctn;
} mls_hpke_ct_t;

typedef struct {
    unsigned char key[65];
    int nct;
    mls_hpke_ct_t *ct;
} mls_path_node_t;

typedef struct {
    mls_node_t leaf;
    int n;
    mls_path_node_t *nodes;
} mls_update_path_t;

/* The secrets a committer generated, one per filtered direct path node. */
typedef struct {
    int n;
    unsigned path[MLS_MAX_PATH];
    unsigned char secret[MLS_MAX_PATH][32];
} mls_path_secrets_t;

/* An UpdatePath; it points into the reader's bytes, which must outlive it. */
int mls_path_read(mls_update_path_t *p, tls_reader_t *r);
void mls_path_free(mls_update_path_t *p);

/* Sets a node's private key from its path secret; fails if it does not match the node's public key. */
int mls_node_set_secret(mls_node_t *node, const unsigned char path_secret[32]);
/* The next path secret up the tree, in place. */
int mls_path_next_secret(unsigned char secret[32]);

/* Merges a received path into the tree and checks the leaf's parent hash (not its signature). */
int mls_path_merge(mls_tree_t *t, unsigned sender, const mls_update_path_t *p);
/* After the merge: decrypts our path secret with the provisional group context, sets the
   private keys it reaches and gives the commit secret. `added` are leaves new in this commit. */
int mls_path_decrypt(mls_tree_t *t, unsigned me, unsigned sender, const mls_update_path_t *p, const void *gc,
                     size_t gcn, const unsigned *added, int nadded, unsigned char path_secret[32],
                     unsigned char commit_secret[32]);

/* Refreshes our leaf and direct path in the tree, re-signing the leaf with `sig_priv`. */
int mls_path_create(mls_tree_t *t, unsigned me, const unsigned char sig_priv[32], const void *group_id, size_t gn,
                    mls_path_secrets_t *ps, unsigned char commit_secret[32]);
/* The UpdatePath for the secrets, encrypted with the provisional group context. */
int mls_path_encrypt(const mls_tree_t *t, unsigned me, const mls_path_secrets_t *ps, const void *gc, size_t gcn,
                     const unsigned *added, int nadded, sb_t *out);
