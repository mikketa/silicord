/* P-256: RFC 6979 (A.2.5) keys and deterministic signatures, and NIST's first ECDH (CAVS) vector. */
#include <windows.h>
#include "test.h"
#include "p256.h"
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
    unsigned char w[128];
    size_t n = unhex(want, w);

    return ct_equal(got, w, n);
}

static const char k_sk[] = "c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721";
static const char k_ux[] = "60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6";
static const char k_uy[] = "7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299";

static void test_keys(void)
{
    unsigned char sk[32], pub[65], zero[32] = {0}, n[32], shared[32];

    unhex(k_sk, sk);
    check(p256_public(sk, pub) && pub[0] == 4 && same_hex(pub + 1, k_ux) && same_hex(pub + 33, k_uy),
          "public key of rfc 6979's key");
    check(p256_ecdh(sk, pub, shared), "it is on the curve");
    pub[64] ^= 1;
    check(!p256_ecdh(sk, pub, shared), "a point off the curve is refused");
    check(!p256_scalar_ok(zero), "zero is not a scalar");
    unhex("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551", n);
    check(!p256_scalar_ok(n), "nor is the order");
    n[31]--;
    check(p256_scalar_ok(n), "n - 1 is");
}

static void sign_case(const char *msg, const char *r, const char *s)
{
    unsigned char sk[32], pub[65], hash[32], sig[64];

    unhex(k_sk, sk);
    p256_public(sk, pub);
    sha256_once(msg, (size_t)lstrlenA(msg), hash);
    check(p256_sign(sk, hash, sig) && same_hex(sig, r) && same_hex(sig + 32, s), "rfc 6979 signature");
    check(p256_verify(pub, hash, sig), "the signature verifies");
    hash[0] ^= 1;
    check(!p256_verify(pub, hash, sig), "not for another message");
}

static void test_ecdh(void)
{
    unsigned char their[65], sk[32], ours[65], shared[32];

    their[0] = 4;
    unhex("700c48f77f56584c5cc632ca65640db91b6bacce3a4df6b42ce7cc838833d287", their + 1);
    unhex("db71e509e3fd9b060ddb20ba5c51dcc5948d46fbf640dfe0441782cab85fa4ac", their + 33);
    unhex("7d7dc5f71eb29ddaf80d6214632eeae03d9058af1fb6d22ed80badb62bc1a534", sk);
    check(p256_public(sk, ours) && same_hex(ours + 1, "ead218590119e8876b29146ff89ca61770c4edbbf97d38ce385ed281d8a6b230") &&
              same_hex(ours + 33, "28af61281fd35e2fa7002523acc85a429cb06ee6648325389f59edfce1405141"),
          "cavs public key");
    check(p256_ecdh(sk, their, shared) && same_hex(shared, "46fc62106420ff012e54a434fbdd2d25ccc5852060561e68040dd7778997bd7b"),
          "cavs shared secret");
}

void entry(void)
{
    test_keys();
    sign_case("sample", "efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716",
              "f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8");
    sign_case("test", "f1abb023518351cd71d881567b1ea663ed3efcf6c5132b354f28d3b0b7d38367",
              "019f4113742a2b14bd25926b49c649155f267e60d3814b4c0cc84250e46f0083");
    test_ecdh();
    finish();
}
