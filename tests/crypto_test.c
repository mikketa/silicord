/* base64 and RSA-OAEP tests. The public key is re-imported by crypt32 as an independent check. */
#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include "test.h"
#include "b64.h"
#include "crypto.h"

static void test_b64(void)
{
    static const char *plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    static const char *coded[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    sb_t s = {0};
    int enc_ok = 1, dec_ok = 1;

    for (int i = 0; i < 7; i++) {
        sb_clear(&s);
        b64_encode((const unsigned char *)plain[i], sc_strlen(plain[i]), &s);
        enc_ok &= str_eq(&s, coded[i]);
        sb_clear(&s);
        dec_ok &= b64_decode(coded[i], sc_strlen(coded[i]), &s) && str_eq(&s, plain[i]);
    }
    check(enc_ok, "base64 encode, RFC 4648 vectors");
    check(dec_ok, "base64 decode, RFC 4648 vectors");

    sb_clear(&s);
    b64url_encode((const unsigned char *)"\xFB\xFF", 2, &s);
    check(str_eq(&s, "-_8"), "base64url alphabet, no padding");
    sb_clear(&s);
    check(b64_decode("-_8", 3, &s) && bytes_eq(&s, "\xFB\xFF", 2), "base64url decode");
    sb_clear(&s);
    check(!b64_decode("ab$d", 4, &s), "reject invalid characters");
    sb_free(&s);
}

static void test_rsa(void)
{
    static const char secret[] = "nonce from the remote auth gateway";
    BCRYPT_OAEP_PADDING_INFO pad = {BCRYPT_SHA256_ALGORITHM, NULL, 0};
    rsa_key_t *key = rsa_generate();
    CERT_PUBLIC_KEY_INFO *info = NULL;
    BCRYPT_KEY_HANDLE imported = NULL;
    sb_t der = {0}, cipher = {0}, plain = {0};
    DWORD info_size = 0;
    ULONG size = 0;

    check(key != NULL, "generate rsa-2048 key pair");
    if (!key)
        return;
    check(rsa_public_spki(key, &der) && der.len == 294, "spki is 294 bytes for rsa-2048");
    check(der.len > 4 && (unsigned char)der.data[0] == 0x30 && (unsigned char)der.data[1] == 0x82, "spki is a der sequence");

    check(CryptDecodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, (const BYTE *)der.data, (DWORD)der.len,
                              CRYPT_DECODE_ALLOC_FLAG, NULL, &info, &info_size) &&
          CryptImportPublicKeyInfoEx2(X509_ASN_ENCODING, info, 0, NULL, &imported),
          "crypt32 parses and imports the spki");

    if (imported) {
        BCryptEncrypt(imported, (PUCHAR)secret, sizeof secret - 1, &pad, NULL, 0, NULL, 0, &size, BCRYPT_PAD_OAEP);
        sb_reserve(&cipher, size);
        if (BCRYPT_SUCCESS(BCryptEncrypt(imported, (PUCHAR)secret, sizeof secret - 1, &pad, NULL, 0,
                                         (PUCHAR)cipher.data, size, &size, BCRYPT_PAD_OAEP)))
            cipher.len = size;
        check(cipher.len == 256, "encrypt with the imported public key");
        check(rsa_decrypt_oaep_sha256(key, cipher.data, cipher.len, &plain) &&
              bytes_eq(&plain, secret, sizeof secret - 1), "oaep-sha256 round trip through the spki");
        if (cipher.len) {
            cipher.data[10] ^= 1;
            sb_clear(&plain);
            check(!rsa_decrypt_oaep_sha256(key, cipher.data, cipher.len, &plain), "reject a tampered ciphertext");
        }
        BCryptDestroyKey(imported);
    }

    if (info)
        LocalFree(info);
    sb_free(&plain);
    sb_free(&cipher);
    sb_free(&der);
    rsa_free(key);
}

void entry(void)
{
    test_b64();
    test_rsa();
    finish();
}
