/* HPKE: RFC 9180's vector A.3.1 (DHKEM(P-256, HKDF-SHA256), HKDF-SHA256, AES-128-GCM, base mode). */
#include <windows.h>
#include "test.h"
#include "hpke.h"
#include "sha2.h"

static size_t unhex(const char *s, unsigned char *out)
{
    size_t n = 0;

    for (; s[0] && s[1]; s += 2) {
        int hi = s[0] <= '9' ? s[0] - '0' : (s[0] | 32) - 'a' + 10, lo = s[1] <= '9' ? s[1] - '0' : (s[1] | 32) - 'a' + 10;
        out[n++] = (unsigned char)(hi << 4 | lo);
    }
    return n;
}

static int same_hex(const unsigned char *got, const char *want)
{
    unsigned char w[160];
    size_t n = unhex(want, w);

    return ct_equal(got, w, n);
}

static const char k_pk_e[] = "04a92719c6195d5085104f469a8b9814d5838ff72b60501e2c4466e5e67b325ac98536d7b61a1af4b78e5b7f951c0900be"
                             "863c403ce65c9bfcb9382657222d18c4";
static const char k_pk_r[] = "04fe8c19ce0905191ebc298a9245792531f26f0cece2460639e8bc39cb7f706a826a779b4cf969b8a0e539c7f62fb3d30a"
                             "d6aa8f80e30f1d128aafd68a2ce72ea0";

void entry(void)
{
    unsigned char info[32], ikm_e[32], ikm_r[32], sk[32], pk[65], pk_r[65], sk_r[32], enc[65], pt[64], ct[80], back[64];
    size_t in = unhex("4f6465206f6e2061204772656369616e2055726e", info);
    size_t pn = unhex("4265617574792069732074727574682c20747275746820626561757479", pt);
    hpke_t s, r;

    unhex("4270e54ffd08d79d5928020af4686d8f6b7d35dbe470265f1f5aa22816ce860e", ikm_e);
    unhex("668b37171f1072f3cf12ea8a236a45df23fc13b82af3609ad1e354f6ef817550", ikm_r);
    check(hpke_derive_keypair(ikm_e, 32, sk, pk) &&
              same_hex(sk, "4995788ef4b9d6132b249ce59a77281493eb39af373d236a1fe415cb0c2d7beb") && same_hex(pk, k_pk_e),
          "ephemeral key pair");
    check(hpke_derive_keypair(ikm_r, 32, sk_r, pk_r) &&
              same_hex(sk_r, "f3ce7fdae57e1a310d87f1ebbde6f328be0a99cdbcadf4d6589cf29de4b8ffd2") && same_hex(pk_r, k_pk_r),
          "receiver key pair");
    check(hpke_setup_sender(pk_r, info, in, ikm_e, enc, &s) && same_hex(enc, k_pk_e), "enc");
    check(same_hex(s.key, "868c066ef58aae6dc589b6cfdd18f97e") && same_hex(s.base_nonce, "4e0bc5018beba4bf004cca59") &&
              same_hex(s.exporter, "14ad94af484a7ad3ef40e9f3be99ecc6fa9036df9d4920548424df127ee0d99f"),
          "key schedule");
    check(hpke_seal(&s, "Count-0", 7, pt, pn, ct) &&
              same_hex(ct, "5ad590bb8baa577f8619db35a36311226a896e7342a6d836d8b7bcd2f20b6c7f9076ac232e3ab2523f39513434"),
          "first encryption");
    check(hpke_seal(&s, "Count-1", 7, pt, pn, ct) &&
              same_hex(ct, "fa6f037b47fc21826b610172ca9637e82d6e5801eb31cbd3748271affd4ecb06646e0329cbdf3c3cd655b28e82"),
          "second encryption, next nonce");
    check(hpke_setup_receiver(enc, sk_r, info, in, &r) && same_hex(r.key, "868c066ef58aae6dc589b6cfdd18f97e"),
          "receiver's key schedule");
    unhex("5ad590bb8baa577f8619db35a36311226a896e7342a6d836d8b7bcd2f20b6c7f9076ac232e3ab2523f39513434", ct);
    check(hpke_open(&r, "Count-0", 7, ct, pn + 16, back) && ct_equal(back, pt, pn), "receiver opens");
    ct[3] ^= 1;
    check(!hpke_open(&r, "Count-1", 7, ct, pn + 16, back), "a changed ciphertext is refused");
    finish();
}
