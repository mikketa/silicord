/* SHA-256 (FIPS 180-4 examples), HMAC-SHA256 (RFC 4231) and HKDF-SHA256 (RFC 5869) vectors. */
#include <windows.h>
#include "test.h"
#include "sha2.h"
#include "mem.h"

/* Hex to bytes; returns the count. */
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

static void test_sha256(void)
{
    unsigned char out[32];
    sha256_t h;
    static char a[1000];

    sha256_once("abc", 3, out);
    check(same_hex(out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256 abc");
    sha256_once("", 0, out);
    check(same_hex(out, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "sha256 empty");
    sha256_once("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, out);
    check(same_hex(out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "sha256 two blocks");
    for (int i = 0; i < 1000; i++)
        a[i] = 'a';
    sha256_init(&h);
    for (int i = 0; i < 1000; i++)
        sha256_update(&h, a, 1000);
    sha256_final(&h, out);
    check(same_hex(out, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "sha256 a million a's");
}

static void test_hmac(void)
{
    unsigned char key[131], out[32];

    for (int i = 0; i < 20; i++)
        key[i] = 0x0b;
    hmac256(key, 20, "Hi There", 8, out);
    check(same_hex(out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"), "hmac rfc 4231 case 1");
    hmac256("Jefe", 4, "what do ya want for nothing?", 28, out);
    check(same_hex(out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "hmac rfc 4231 case 2");
    for (int i = 0; i < 131; i++)
        key[i] = 0xaa;
    hmac256(key, 131, "Test Using Larger Than Block-Size Key - Hash Key First", 54, out);
    check(same_hex(out, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"), "hmac long key");
}

static void test_hkdf(void)
{
    unsigned char ikm[22], salt[13], info[10], prk[32], okm[42];

    for (int i = 0; i < 22; i++)
        ikm[i] = 0x0b;
    for (int i = 0; i < 13; i++)
        salt[i] = (unsigned char)i;
    for (int i = 0; i < 10; i++)
        info[i] = (unsigned char)(0xf0 + i);
    hkdf256_extract(salt, 13, ikm, 22, prk);
    check(same_hex(prk, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"), "hkdf extract a.1");
    check(hkdf256_expand(prk, info, 10, okm, 42) &&
              same_hex(okm, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"),
          "hkdf expand a.1");
    hkdf256_extract(NULL, 0, ikm, 22, prk);
    check(same_hex(prk, "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04"), "hkdf extract without salt");
    check(hkdf256_expand(prk, "", 0, okm, 42) &&
              same_hex(okm, "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"),
          "hkdf expand without info");
    check(!hkdf256_expand(prk, "", 0, okm, 255 * 32 + 1), "hkdf refuses too long an output");
}

void entry(void)
{
    test_sha256();
    test_hmac();
    test_hkdf();
    finish();
}
