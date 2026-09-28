/* AES (FIPS-197 appendix C) and GCM (the test cases of McGrew and Viega's GCM specification). */
#include <windows.h>
#include "test.h"
#include "aes.h"
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

static void test_blocks(void)
{
    unsigned char key[32], in[16], out[16];
    aes_t a;

    for (int i = 0; i < 32; i++)
        key[i] = (unsigned char)i;
    unhex("00112233445566778899aabbccddeeff", in);
    check(aes_init(&a, key, 16), "aes-128 key");
    aes_encrypt_block(&a, in, out);
    check(same_hex(out, "69c4e0d86a7b0430d8cdb78070b4c55a"), "aes-128 block");
    check(aes_init(&a, key, 32), "aes-256 key");
    aes_encrypt_block(&a, in, out);
    check(same_hex(out, "8ea2b7ca516745bfeafc49904b496089"), "aes-256 block");
    check(!aes_init(&a, key, 24), "other key sizes are refused");
}

/* One GCM case both ways, and a flipped bit refused. */
static void gcm_case(const char *name, const char *k, const char *iv, const char *p, const char *aad, const char *c,
                     const char *t)
{
    unsigned char key[32], nonce[12], pt[64], ad[32], ct[64], want_ct[64], tag[16], back[64];
    size_t kn = unhex(k, key), pn = unhex(p, pt), an = unhex(aad, ad);
    int ok;

    unhex(iv, nonce);
    unhex(c, want_ct);
    ok = aes_gcm_seal(key, kn, nonce, ad, an, pt, pn, ct, tag, 16) && ct_equal(ct, want_ct, pn) && same_hex(tag, t);
    ok = ok && aes_gcm_open(key, kn, nonce, ad, an, ct, pn, back, tag, 16) && ct_equal(back, pt, pn);
    check(ok, name);
    if (pn) {
        ct[0] ^= 1;
        check(!aes_gcm_open(key, kn, nonce, ad, an, ct, pn, back, tag, 16), "a changed ciphertext is refused");
        ct[0] ^= 1;
    }
    check(aes_gcm_open(key, kn, nonce, ad, an, ct, pn, back, tag, 8), "a tag cut to 8 bytes, as DAVE sends");
}

void entry(void)
{
    static const char p64[] = "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525"
                              "b16aedf5aa0de657ba637b391aafd255";
    static const char p60[] = "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525"
                              "b16aedf5aa0de657ba637b39";

    test_blocks();
    gcm_case("gcm case 1: empty", "00000000000000000000000000000000", "000000000000000000000000", "", "", "",
             "58e2fccefa7e3061367f1d57a4e7455a");
    gcm_case("gcm case 2", "00000000000000000000000000000000", "000000000000000000000000", "00000000000000000000000000000000",
             "", "0388dace60b6a392f328c2b971b2fe78", "ab6e47d42cec13bdf53a67b21257bddf");
    gcm_case("gcm case 3", "feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888", p64, "",
             "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d"
             "58e091473f5985",
             "4d5c2af327cd64a62cf35abd2ba6fab4");
    gcm_case("gcm case 4: with aad", "feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888", p60,
             "feedfacedeadbeeffeedfacedeadbeefabaddad2",
             "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d"
             "58e091",
             "5bc94fbc3221a5db94fae95ae7121a47");
    gcm_case("gcm case 14: aes-256", "0000000000000000000000000000000000000000000000000000000000000000",
             "000000000000000000000000", "00000000000000000000000000000000", "", "cea7403d4d606b6e074ec5d3baf39d18",
             "d0d1c8a799996bf0265b98b5d48ab919");
    gcm_case("gcm case 16: aes-256 with aad", "feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
             "cafebabefacedbaddecaf888", p60, "feedfacedeadbeeffeedfacedeadbeefabaddad2",
             "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0a"
             "bcc9f662",
             "76fc6ece0f4e1768cddf8853bb2d551b");
    finish();
}
