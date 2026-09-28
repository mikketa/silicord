#include <string.h>
#include "mls_schedule.h"
#include "hpke.h"
#include "mls_crypto.h"
#include "sha2.h"
#include "tls.h"

void mls_group_context(sb_t *out, const void *group_id, size_t gn, unsigned long long epoch,
                       const unsigned char tree_hash[32], const void *confirmed_transcript_hash, size_t tn,
                       const void *extensions, size_t en)
{
    tls_u16(out, 1); /* mls10 */
    tls_u16(out, 2); /* DHKEMP256_AES128GCM_SHA256_P256 */
    tls_vec(out, group_id, gn);
    tls_u64(out, epoch);
    tls_vec(out, tree_hash, 32);
    tls_vec(out, confirmed_transcript_hash, tn);
    tls_vec(out, extensions, en);
}

int mls_key_schedule_joiner(const unsigned char joiner[32], const unsigned char psk_secret[32], const void *group_context,
                            size_t cn, mls_epoch_t *e)
{
    unsigned char member[32];
    unsigned char sk[32];
    int ok;

    if (e->joiner != joiner)
        memcpy(e->joiner, joiner, 32);
    hkdf256_extract(joiner, 32, psk_secret, 32, member);
    ok = mls_derive_secret(member, "welcome", e->welcome) &&
         mls_expand_with_label(member, 32, "epoch", group_context, cn, e->epoch, 32) &&
         mls_derive_secret(e->epoch, "sender data", e->sender_data) &&
         mls_derive_secret(e->epoch, "encryption", e->encryption) && mls_derive_secret(e->epoch, "exporter", e->exporter) &&
         mls_derive_secret(e->epoch, "external", e->external) && mls_derive_secret(e->epoch, "confirm", e->confirm) &&
         mls_derive_secret(e->epoch, "membership", e->membership) &&
         mls_derive_secret(e->epoch, "resumption", e->resumption) &&
         mls_derive_secret(e->epoch, "authentication", e->authentication) && mls_derive_secret(e->epoch, "init", e->init) &&
         hpke_derive_keypair(e->external, 32, sk, e->external_pub);
    secure_wipe(member, sizeof member);
    secure_wipe(sk, sizeof sk);
    return ok;
}

int mls_key_schedule(const unsigned char init_prev[32], const unsigned char commit_secret[32],
                     const unsigned char psk_secret[32], const void *group_context, size_t cn, mls_epoch_t *e)
{
    unsigned char prk[32];
    int ok;

    hkdf256_extract(init_prev, 32, commit_secret, 32, prk);
    ok = mls_expand_with_label(prk, 32, "joiner", group_context, cn, e->joiner, 32) &&
         mls_key_schedule_joiner(e->joiner, psk_secret, group_context, cn, e);
    secure_wipe(prk, sizeof prk);
    return ok;
}

int mls_exporter(const unsigned char exporter_secret[32], const void *label, size_t ln, const void *ctx, size_t cn,
                 unsigned char *out, size_t len)
{
    unsigned char derived[32], hash[32];
    int ok;

    sha256_once(ctx, cn, hash);
    ok = mls_expand_with_label_n(exporter_secret, 32, label, ln, "", 0, derived, 32) &&
         mls_expand_with_label(derived, 32, "exported", hash, 32, out, len);
    secure_wipe(derived, sizeof derived);
    return ok;
}
