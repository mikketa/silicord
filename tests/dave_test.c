/* DAVE frames: ratchet generations, the frame layout, unencrypted ranges, the frame check and displayable codes. */
#include <windows.h>
#include "test.h"
#include "dave.h"
#include "sha2.h"

static const unsigned char k_base[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

static void test_ratchet(void)
{
    dave_ratchet_t r;
    unsigned char k0[16], k0b[16], k1[16], k2[16];

    dave_ratchet_init(&r, k_base);
    check(dave_ratchet_key(&r, 0, k0) && dave_ratchet_key(&r, 0, k0b) && ct_equal(k0, k0b, 16), "a generation's key is stable");
    check(dave_ratchet_key(&r, 2, k2) && dave_ratchet_key(&r, 1, k1) && !ct_equal(k1, k2, 16) && !ct_equal(k0, k1, 16),
          "later generations ratchet, earlier ones stay cached");
    for (unsigned long g = 3; g < 3 + DAVE_KEEP + 1; g++)
        dave_ratchet_key(&r, g, k2);
    check(!dave_ratchet_key(&r, 0, k0b), "old generations are erased");
    dave_ratchet_wipe(&r);
}

static void test_opus(void)
{
    static const unsigned char frame[] = "an opus frame";
    size_t n = sizeof frame - 1;
    dave_ratchet_t enc, dec;
    unsigned char key[16];
    unsigned long nonce;
    sb_t out = {0}, back = {0};

    dave_ratchet_init(&enc, k_base);
    dave_ratchet_init(&dec, k_base);
    dave_ratchet_key(&enc, 0, key);
    check(dave_encrypt(key, 5, frame, n, NULL, 0, &out) && out.len == n + 12, "an opus frame grows by 12 bytes");
    check(out.len == n + 12 && (unsigned char)out.data[n + 8] == 5 && (unsigned char)out.data[n + 9] == 12 &&
              (unsigned char)out.data[n + 10] == 0xFA && (unsigned char)out.data[n + 11] == 0xFA,
          "nonce, supplemental size and marker at the end");
    check(dave_frame_nonce((unsigned char *)out.data, out.len, &nonce) && nonce == 5, "the frame check reads the nonce");
    check(dave_decrypt(&dec, (unsigned char *)out.data, out.len, &back) && bytes_eq(&back, (const char *)frame, n),
          "decrypts with the sender's ratchet");
    out.data[2] ^= 1;
    sb_clear(&back);
    check(!dave_decrypt(&dec, (unsigned char *)out.data, out.len, &back), "a changed frame is refused");

    /* A generation-1 nonce uses the next key. */
    sb_clear(&out);
    sb_clear(&back);
    dave_ratchet_key(&enc, 1, key);
    check(dave_encrypt(key, 0x01000007ul, frame, n, NULL, 0, &out) &&
              dave_decrypt(&dec, (unsigned char *)out.data, out.len, &back) && bytes_eq(&back, (const char *)frame, n),
          "the nonce's top byte picks the generation");
    sb_free(&out);
    sb_free(&back);
}

static void test_ranges(void)
{
    unsigned char frame[20], key[16];
    dave_range_t ranges[2] = {{0, 1}, {10, 2}};
    dave_ratchet_t r;
    sb_t out = {0}, back = {0};

    for (int i = 0; i < 20; i++)
        frame[i] = (unsigned char)(0x40 + i);
    dave_ratchet_init(&r, k_base);
    dave_ratchet_key(&r, 0, key);
    check(dave_encrypt(key, 300, frame, 20, ranges, 2, &out) && out.data[0] == 0x40 && out.data[10] == 0x4a &&
              out.data[11] == 0x4b && out.data[1] != 0x41,
          "unencrypted ranges stay in place");
    check(dave_decrypt(&r, (unsigned char *)out.data, out.len, &back) && bytes_eq(&back, (const char *)frame, 20),
          "and the frame comes back whole");
    out.data[10] ^= 1;
    sb_clear(&back);
    check(!dave_decrypt(&r, (unsigned char *)out.data, out.len, &back), "unencrypted bytes are authenticated");
    ranges[1].offset = 0;
    sb_clear(&out);
    check(!dave_encrypt(key, 1, frame, 20, ranges, 2, &out), "overlapping ranges are refused");
    sb_free(&out);
    sb_free(&back);
}

static void test_misc(void)
{
    unsigned char data[30];
    char code[40];
    unsigned long nonce;

    check(dave_is_silence((const unsigned char *)"\xF8\xFF\xFE", 3) && !dave_is_silence((const unsigned char *)"\xF8\xFF", 2),
          "silence packets pass through");
    check(!dave_frame_nonce((const unsigned char *)"plain opus data", 15, &nonce), "plain frames fail the check");
    for (int i = 0; i < 30; i++)
        data[i] = (unsigned char)(i + 1);
    check(dave_displayable_code(data, 30, 30, 5, code, sizeof code) && lstrcmpA(code, "193657089022415739402546576990") == 0,
          "displayable code, 30 digits in groups of 5");
    check(!dave_displayable_code(data, 30, 30, 8, code, sizeof code), "groups must be shorter than 8");
}

void entry(void)
{
    test_ratchet();
    test_opus();
    test_ranges();
    test_misc();
    finish();
}
