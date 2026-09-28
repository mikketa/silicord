#pragma once
#include <stddef.h>
#include "dave.h"
#include "mls_group.h"
#include "sb.h"

/*
 * DAVE's group key exchange over the voice gateway (protocol 1): opcodes
 * 21 to 31, the MLS group they drive and the per-sender media keys. The
 * caller moves bytes; this decides what to send back through `send`.
 */

#define DAVE_KEEP_OLD_MS 10000 /* how long the previous epoch's keys decrypt late media */

typedef struct {
    unsigned long long user;
    dave_ratchet_t ratchet;
} dave_sender_t;

typedef struct {
    int n;
    dave_sender_t *senders;
} dave_keyring_t;

typedef struct {
    unsigned long long self_id;
    unsigned char group_id[8]; /* the channel id, big-endian */
    int protocol;              /* negotiated with the gateway */
    int version;               /* in use for media: 0 is transport encryption only */
    void (*send)(void *ctx, int binary, const void *data, size_t n);
    void *ctx;

    sb_t external_sender;
    mls_member_t member;
    int has_member;
    mls_group_t group;
    int has_group, established;
    mls_group_t outbound; /* the state after our own commit, if the gateway picks it */
    sb_t outbound_commit;
    int has_outbound;

    unsigned long long *users; /* who may be in the group */
    int nusers;

    int pending_transition, pending_version, has_pending;
    dave_keyring_t current, previous;
    unsigned long long previous_until;
    dave_ratchet_t own;
    unsigned long own_nonce;
    int has_own;
} dave_session_t;

void dave_session_init(dave_session_t *s, unsigned long long self_id, unsigned long long channel_id,
                       void (*send)(void *ctx, int binary, const void *data, size_t n), void *ctx);
void dave_session_free(dave_session_t *s);

/* JSON opcodes, parsed by the caller. */
void dave_on_select_protocol_ack(dave_session_t *s, int version);
void dave_on_clients_connect(dave_session_t *s, const unsigned long long *users, int n);
void dave_on_client_disconnect(dave_session_t *s, unsigned long long user);
void dave_on_prepare_transition(dave_session_t *s, int transition_id, int version, unsigned long long now_ms);
void dave_on_execute_transition(dave_session_t *s, int transition_id, unsigned long long now_ms);
void dave_on_prepare_epoch(dave_session_t *s, unsigned long long epoch, int version);
/* A binary gateway message: sequence number, opcode (25, 27, 29 or 30), payload. */
void dave_on_binary(dave_session_t *s, const void *data, size_t n, unsigned long long now_ms);

/* The epoch authenticator as 30 digits (shown in groups of 5); 0 without a group. */
int dave_authenticator(const dave_session_t *s, char *out, size_t size);
/* Our outgoing frame: encrypted under DAVE, or copied as is at version 0. */
int dave_session_encrypt(dave_session_t *s, const unsigned char *frame, size_t n, sb_t *out);
/* A VP8 frame: its first bytes (which the SFU reads) stay in the clear, authenticated. */
int dave_session_encrypt_vp8(dave_session_t *s, const unsigned char *frame, size_t n, sb_t *out);
/* An incoming frame from `user`. */
int dave_session_decrypt(dave_session_t *s, unsigned long long user, const unsigned char *frame, size_t n, sb_t *out,
                         unsigned long long now_ms);
