#pragma once
#include <stddef.h>
#include "mls_msg.h"
#include "mls_schedule.h"
#include "mls_tree.h"
#include "sb.h"

/*
 * An MLS group as one member sees it (RFC 9420, sections 11 and 12), with
 * handshake messages as PublicMessage only, which is what DAVE uses.
 */

/* Our KeyPackage and its private keys. */
typedef struct {
    sb_t key_package; /* the KeyPackage encoding, without an MLSMessage header */
    unsigned char init_priv[32], enc_priv[32], sig_priv[32];
} mls_member_t;

/* An external PSK the application holds. */
typedef struct {
    mls_bytes_t id, secret;
} mls_psk_t;

typedef struct {
    int sender_type;
    unsigned sender;
    unsigned char ref[32];
    sb_t proposal; /* the Proposal encoding */
} mls_cached_proposal_t;

typedef struct {
    sb_t group_id, extensions; /* the GroupContext extensions, without the list's length */
    unsigned long long epoch;
    mls_tree_t tree;
    unsigned char cth[32], interim[32];
    size_t cth_n; /* 0 at epoch 0 */
    mls_epoch_t keys;
    unsigned me;
    unsigned char sig_priv[32];
    int nprops;
    mls_cached_proposal_t *props;
    /* Resumption PSKs of the latest epochs, oldest first. */
    int nresumption;
    unsigned long long resumption_epoch[8];
    unsigned char resumption[8][32];
    /* External PSKs, owned by the caller. */
    const mls_psk_t *psks;
    int npsks;
} mls_group_t;

enum { MLS_HANDLED_PROPOSAL = 1, MLS_HANDLED_COMMIT, MLS_HANDLED_REMOVED };

/* A fresh KeyPackage for `identity` (DAVE: the user id as 8 big-endian bytes). */
int mls_member_create(mls_member_t *m, const void *identity, size_t in);
void mls_member_free(mls_member_t *m);
/* KeyPackageRef. */
void mls_key_package_ref(const void *kp, size_t n, unsigned char ref[32]);

/* A one-member group at epoch 0; `extensions` is the list's content. */
int mls_group_create(mls_group_t *g, const mls_member_t *m, const void *group_id, size_t gn, const void *extensions,
                     size_t en);
/* Joins from a Welcome (bare or in an MLSMessage). `tree` is the ratchet_tree encoding when it is not in the
   GroupInfo, else NULL. */
int mls_group_join(mls_group_t *g, const mls_member_t *m, const void *welcome, size_t n, const void *tree, size_t tn,
                   const mls_psk_t *psks, int npsks);
void mls_group_free(mls_group_t *g);
int mls_group_copy(mls_group_t *dst, const mls_group_t *src);
void mls_group_context_of(const mls_group_t *g, sb_t *gc);

/* A handshake MLSMessage: caches a proposal or applies a commit (MLS_HANDLED_*); 0 when invalid. `ref` gets
   a proposal's reference. */
int mls_group_handle(mls_group_t *g, const void *msg, size_t n, unsigned char ref[32]);
/* Drops a cached proposal; 0 when unknown. */
int mls_group_revoke(mls_group_t *g, const void *ref, size_t rn);
/* Commits every cached proposal by reference with a path. `commit` gets the MLSMessage, `welcome` a bare
   Welcome when members are added (left empty otherwise), `next` the state once the commit is accepted. */
int mls_group_commit(const mls_group_t *g, sb_t *commit, sb_t *welcome, mls_group_t *next);
