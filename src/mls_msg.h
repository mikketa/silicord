#pragma once
#include <stddef.h>
#include "mls_path.h"
#include "mls_tree.h"
#include "sb.h"
#include "tls.h"

/*
 * MLS messages (RFC 9420, sections 6, 10, 12): readers that check the whole
 * structure and point into the input, which must outlive what they fill.
 */

enum { MLS_WIRE_PUBLIC = 1, MLS_WIRE_PRIVATE, MLS_WIRE_WELCOME, MLS_WIRE_GROUP_INFO, MLS_WIRE_KEY_PACKAGE };
enum { MLS_SENDER_MEMBER = 1, MLS_SENDER_EXTERNAL, MLS_SENDER_NEW_MEMBER_PROPOSAL, MLS_SENDER_NEW_MEMBER_COMMIT };
enum { MLS_CONTENT_APPLICATION = 1, MLS_CONTENT_PROPOSAL, MLS_CONTENT_COMMIT };
enum {
    MLS_PROPOSAL_ADD = 1,
    MLS_PROPOSAL_UPDATE,
    MLS_PROPOSAL_REMOVE,
    MLS_PROPOSAL_PSK,
    MLS_PROPOSAL_REINIT,
    MLS_PROPOSAL_EXTERNAL_INIT,
    MLS_PROPOSAL_GROUP_CONTEXT_EXTENSIONS
};
enum { MLS_EXT_RATCHET_TREE = 2, MLS_EXT_EXTERNAL_SENDERS = 5 };

typedef struct {
    const unsigned char *p;
    size_t n;
} mls_bytes_t;

/* FramedContent with its authentication data; a PublicMessage adds the membership tag. */
typedef struct {
    unsigned wire_format;
    mls_bytes_t group_id, authenticated_data;
    unsigned long long epoch;
    int sender_type;
    unsigned sender;
    int content_type;
    mls_bytes_t content; /* the application data, or the Proposal or Commit encoding */
    mls_bytes_t framed;  /* the whole FramedContent */
    mls_bytes_t auth;    /* the whole FramedContentAuthData */
    mls_bytes_t signature, confirmation_tag, membership_tag;
} mls_content_t;

typedef struct {
    int type;
    mls_bytes_t body;  /* after proposal_type */
    mls_bytes_t whole; /* the Proposal encoding */
} mls_proposal_t;

typedef struct {
    int by_ref;
    mls_bytes_t ref;
    mls_proposal_t proposal;
} mls_proposal_or_ref_t;

typedef struct {
    int n;
    mls_proposal_or_ref_t *items;
    int has_path;
    mls_update_path_t path;
} mls_commit_t;

typedef struct {
    mls_bytes_t whole, tbs, signature, extensions;
    unsigned char init_key[65];
    mls_node_t leaf;
} mls_key_package_t;

typedef struct {
    mls_bytes_t whole, tbs, signature;
    mls_bytes_t group_context, group_id, tree_hash, confirmed_transcript_hash, gc_extensions;
    unsigned long long epoch;
    mls_bytes_t extensions, confirmation_tag;
    unsigned signer;
} mls_group_info_t;

typedef struct {
    mls_bytes_t new_member, kem, ct;
} mls_group_secrets_ct_t;

typedef struct {
    int n;
    mls_group_secrets_ct_t *secrets;
    mls_bytes_t encrypted_group_info;
} mls_welcome_t;

typedef struct {
    mls_bytes_t joiner, path_secret; /* path_secret is empty when absent */
    int has_path_secret;
    int npsks;
    mls_bytes_t psks[16]; /* each PreSharedKeyID encoding */
} mls_group_secrets_t;

/* MLSMessage: version mls10 then the wire format; the body follows in the reader. */
int mls_message_header(tls_reader_t *r, unsigned *wire_format);
/* PublicMessage (after the MLSMessage header). */
int mls_public_read(mls_content_t *m, tls_reader_t *r);
/* AuthenticatedContent: wire_format, FramedContent, FramedContentAuthData. */
int mls_auth_content_read(mls_content_t *m, tls_reader_t *r);
/* The AuthenticatedContent encoding of a message (for proposal references). */
void mls_auth_content(const mls_content_t *m, sb_t *out);

int mls_proposal_read(mls_proposal_t *p, tls_reader_t *r);
/* One proposal type's body alone, as the messages vectors give it. */
int mls_proposal_body_read(int type, tls_reader_t *r);
int mls_commit_read(mls_commit_t *c, tls_reader_t *r);
void mls_commit_free(mls_commit_t *c);
int mls_key_package_read(mls_key_package_t *kp, tls_reader_t *r);
void mls_key_package_free(mls_key_package_t *kp);
/* The signature, the leaf (source key_package) and distinct init and encryption keys. */
int mls_key_package_verify(const mls_key_package_t *kp);
int mls_group_info_read(mls_group_info_t *gi, tls_reader_t *r);
int mls_welcome_read(mls_welcome_t *w, tls_reader_t *r);
void mls_welcome_free(mls_welcome_t *w);
int mls_group_secrets_read(mls_group_secrets_t *gs, tls_reader_t *r);
/* PreSharedKeyID: returns its type, 1 external (with `psk_id`) or 2 resumption, or 0 when malformed. */
int mls_psk_id_read(tls_reader_t *r, mls_bytes_t *psk_id);
/* An extension list's entry of `type` (empty when missing); returns whether it is there. */
int mls_extension_find(mls_bytes_t list, unsigned type, mls_bytes_t *data);

/* FramedContentTBS: `gc` is the group context for member and new-member-commit senders. */
void mls_content_tbs(const mls_content_t *m, const void *gc, size_t gcn, sb_t *out);
int mls_content_verify(const mls_content_t *m, const unsigned char sig_key[65], const void *gc, size_t gcn);
int mls_membership_tag(const mls_content_t *m, const unsigned char key[32], const void *gc, size_t gcn,
                       unsigned char tag[32]);

/* The new confirmed and interim transcript hashes after a commit. */
void mls_transcript(const void *interim, size_t in, const mls_content_t *commit, unsigned char confirmed[32],
                    unsigned char interim_next[32]);
/* interim = Hash(confirmed || MAC<V>). */
void mls_interim(const void *confirmed, size_t cn, const void *tag, size_t tn, unsigned char out[32]);
/* psk_secret from PreSharedKeyID encodings and their keys (zeros when n is 0). */
int mls_psk_secret(const mls_bytes_t *ids, const mls_bytes_t *psks, int n, unsigned char out[32]);
