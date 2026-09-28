/* MLS labeled primitives: the IETF mls-implementations crypto-basics vectors for ciphersuite 2, and TLS varints. */
#include <windows.h>
#include "test.h"
#include "mls_crypto.h"
#include "sha2.h"
#include "tls.h"

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

static void test_varints(void)
{
    sb_t out = {0};
    tls_reader_t r;

    tls_varint(&out, 37);
    tls_varint(&out, 15293);
    tls_varint(&out, 494878333);
    check(bytes_eq(&out, "\x25\x7b\xbd\x9d\x7f\x3e\x7d", 7), "varints of rfc 9420's examples");
    tls_reader(&r, out.data, out.len);
    check(tls_read_varint(&r) == 37 && tls_read_varint(&r) == 15293 && tls_read_varint(&r) == 494878333 && tls_done(&r),
          "varints read back");
    tls_reader(&r, "\x40\x25", 2);
    tls_read_varint(&r);
    check(r.bad, "a varint longer than needed is refused");
    sb_free(&out);
}

static void test_labels(void)
{
    unsigned char secret[32], ctx[32], out[32];

    unhex("194d4e81c3f9fcfbc40e2cf3f5b104d0f51e71b5a7f1e4ddd70cc8d4a3620e5f", secret);
    unhex("453c420d63ecb1a8da89d3569770c42bad572864f3709d1e9dd19a0961cbf5e1", ctx);
    check(mls_expand_with_label(secret, 32, "ExpandWithLabel", ctx, 32, out, 16) &&
              same_hex(out, "5710680c556304f4aec67aab4abbc1b1"),
          "expand with label");
    unhex("7165576616048aa145d20f7c1d460e2d9d8bbde882bc3bb0750d50e369809f99", secret);
    check(mls_derive_secret(secret, "DeriveSecret", out) &&
              same_hex(out, "1ecafd3d40cb32cae416e09bc01da56357d00cb094f74e3c69c2969216e50afc"),
          "derive secret");
    unhex("62db8a06300e98dbfd2c831e18544873cec3a2c1ba852fcee8423ff3c08e397e", secret);
    check(mls_derive_tree_secret(secret, 32, "DeriveTreeSecret", 2694881440ul, out, 32) &&
              same_hex(out, "298ab27d2e621d9fc079126d9ffce5259fa0d58697267b40bfadf805b01d0d3c"),
          "derive tree secret");
    unhex("feb64672017685dc26d0b7fc41cd96d8bef40af09002eb5aa9a52580c80f0b2e", secret);
    mls_ref_hash("RefHash", secret, 32, out);
    check(same_hex(out, "8f508c2f89d2797b6882ee487dc2832b2b6d59b27681293c2c2c3f1f0e6cd54f"), "ref hash");
}

static void test_signatures(void)
{
    unsigned char pub[65], sk[32], content[32], sig[80];
    size_t sn = unhex("304402206042e397e1bb78951709790e3446b13bf17f9c641aaed92fb1768b5bc99dd98402202465153d91dd79e808286"
                      "088768d8ed150381f3675498d6e150ae713be43387b",
                      sig);
    sb_t ours = {0};

    unhex("04448971fc06de011d780cf68fd27e2570322d04079f529c3deb48a2015fdd828162c570264e051b5856e8111171fb0341907173aefa66"
          "5682c2549af982a31483",
          pub);
    unhex("207c472d3efaf6737a6f5ae14a3c33a139034865364a128bca5475c85cc02fe0", sk);
    unhex("a0dda617ce4685d764c762b11186b6d60ff8de85ff01eca2413bd4ecfd3b3a57", content);
    check(mls_verify_with_label(pub, "SignWithLabel", content, 32, sig, sn), "the vector's signature verifies");
    check(!mls_verify_with_label(pub, "OtherLabel", content, 32, sig, sn), "not under another label");
    check(mls_sign_with_label(sk, "SignWithLabel", content, 32, &ours) &&
              mls_verify_with_label(pub, "SignWithLabel", content, 32, (const unsigned char *)ours.data, ours.len),
          "our DER signature verifies");
    sb_free(&ours);
}

static void test_encryption(void)
{
    unsigned char sk[32], pub[65], ctx[32], kem[65], ct[64], pt[32];
    size_t cn = unhex("98b7d4edc79f4a90f65434050f75e31a3bc6501c584d41abd6ed5fe3c2db9de22d25ef40c45cce7d0fcc881d7d27af2f", ct);
    sb_t out = {0}, mine = {0};

    unhex("ff21771424dadd640e05c67983aafe19b4d8df50783a0c2decc17d0c7ca4cc17", sk);
    unhex("047b27b0be346d14d7b4df30296a030deeba088746da7cfda43d0ec739df3ce90d3c96d5f302e41f935ac9020651285c7bcf073172d375"
          "c5abcfc9e491b3491f88",
          pub);
    unhex("e27e3b0104990cca866751732c82787af4dcf265c893f77e31bcfd679370e24e", ctx);
    unhex("041fae8a8173cad50c0cc6d55f148ff8edda63083b3673cf6dce6a0ae6ee3f0b61505470309dade87ac5ccdb581da3e9ddf6726949d5a9"
          "2b65dcad6c8679c7313e",
          kem);
    unhex("38a6b327573639d654b5b729336cf74d01728cf4fa9af81a0ef1814ffc1d492f", pt);
    check(mls_decrypt_with_label(sk, "EncryptWithLabel", ctx, 32, kem, ct, cn, &out) && out.len == 32 &&
              ct_equal(out.data, pt, 32),
          "the vector decrypts");
    sb_clear(&out);
    check(mls_encrypt_with_label(pub, "EncryptWithLabel", ctx, 32, pt, 32, kem, &mine) &&
              mls_decrypt_with_label(sk, "EncryptWithLabel", ctx, 32, kem, (const unsigned char *)mine.data, mine.len, &out) &&
              out.len == 32 && ct_equal(out.data, pt, 32),
          "our encryption round-trips");
    sb_free(&out);
    sb_free(&mine);
}

void entry(void)
{
    test_varints();
    test_labels();
    test_signatures();
    test_encryption();
    finish();
}
