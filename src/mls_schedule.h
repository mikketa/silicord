#pragma once
#include <stddef.h>
#include "sb.h"

/* The MLS key schedule (RFC 9420, section 8) for ciphersuite 2. */

typedef struct {
    unsigned char joiner[32], welcome[32], epoch[32];
    unsigned char sender_data[32], encryption[32], exporter[32], external[32], confirm[32], membership[32],
        resumption[32], authentication[32], init[32];
    unsigned char external_pub[65];
} mls_epoch_t;

/* GroupContext: version 1.0, ciphersuite 2, the group id, epoch, tree hash, confirmed transcript hash (empty at
   epoch 0), extensions. */
void mls_group_context(sb_t *out, const void *group_id, size_t gn, unsigned long long epoch,
                       const unsigned char tree_hash[32], const void *confirmed_transcript_hash, size_t tn,
                       const void *extensions, size_t en);
/* One epoch's secrets from the previous init secret, the commit secret and the PSK secret (zeros without PSKs). */
int mls_key_schedule(const unsigned char init_prev[32], const unsigned char commit_secret[32],
                     const unsigned char psk_secret[32], const void *group_context, size_t cn, mls_epoch_t *e);
/* The epoch secret onwards, for a member who got the joiner secret from a Welcome. */
int mls_key_schedule_joiner(const unsigned char joiner[32], const unsigned char psk_secret[32], const void *group_context,
                            size_t cn, mls_epoch_t *e);
/* MLS-Exporter(label, context, length). */
int mls_exporter(const unsigned char exporter_secret[32], const void *label, size_t ln, const void *ctx, size_t cn,
                 unsigned char *out, size_t len);
