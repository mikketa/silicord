/*
 * Prints QR codes for a set of inputs; tests/qr_check.py decodes them with an
 * independent reader and compares. Format per case:
 *   CASE <hex of input>
 *   <size lines of 0/1>
 */
#include <windows.h>
#include "test.h"
#include "mem.h"
#include "qr.h"

static void emit(const char *data, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    qr_t *qr = mem_alloc(sizeof *qr);
    sb_t s = {0};

    if (!qr_encode(data, n, qr)) {
        out("ENCODE_FAILED\r\n");
        g_failed++;
        mem_free(qr);
        return;
    }
    sb_add(&s, "CASE ");
    for (size_t i = 0; i < n; i++) {
        char h[2] = {hex[(unsigned char)data[i] >> 4], hex[data[i] & 15]};
        sb_addn(&s, h, 2);
    }
    sb_add(&s, "\n");
    for (int y = 0; y < qr->size; y++) {
        for (int x = 0; x < qr->size; x++)
            sb_addn(&s, qr_dark(qr, x, y) ? "1" : "0", 1);
        sb_add(&s, "\n");
    }
    out(s.data);
    sb_free(&s);
    mem_free(qr);
}

void entry(void)
{
    static char big[213];
    qr_t *qr = mem_alloc(sizeof *qr);

    emit("a", 1);
    emit("https://discord.com/ra/", 23);
    emit("https://discord.com/ra/Lkq3tWkfM7Hp2cBxN8vYd0aQs1uZe9RgJi4oXnTbPwC", 66);
    for (int i = 0; i < 213; i++)
        big[i] = (char)('A' + i % 26);
    emit(big, 90);  /* version 5 */
    emit(big, 150); /* version 7: version information blocks */
    emit(big, 213); /* version 10: longest supported */
    emit("caf\xC3\xA9 \xF0\x9F\x98\x80", 10);

    if (qr_encode(big, 213, qr))
        check(qr->size == 57, "213 bytes use version 10");
    check(!qr_encode(big, 214, qr), "214 bytes do not fit");
    mem_free(qr);
    ExitProcess((UINT)g_failed);
}
