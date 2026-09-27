#include "console.h"
#include "sc_asm.h"
#include "utf.h"

#define INK    "\x1b[38;2;237;230;214m"
#define AMBER  "\x1b[38;2;255;176;0m"
#define RESET  "\x1b[0m"
#define BLOCK  "\xE2\x96\x88"   /* U+2588 in UTF-8 */

static HANDLE g_out;

void con_init(void)
{
    DWORD mode;

    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(g_out, &mode))
        SetConsoleMode(g_out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(CP_UTF8);
}

void con_write(const char *s, DWORD len)
{
    DWORD written;
    WriteFile(g_out, s, len, &written, NULL);
}

void con_print(const char *s)
{
    con_write(s, (DWORD)sc_strlen(s));
}

void con_print_sb(const sb_t *sb)
{
    if (sb->len)
        con_write(sb->data, (DWORD)sb->len);
}

static int is_trim(wchar_t c)
{
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'"' || c == L'\'';
}

int con_read_secret(sb_t *out)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    wchar_t buf[512];
    DWORD mode = 0, n = 0, start = 0;
    int ok;

    if (!GetConsoleMode(in, &mode))
        return 0;
    SetConsoleMode(in, mode & ~(DWORD)ENABLE_ECHO_INPUT);
    ok = ReadConsoleW(in, buf, ARRAYSIZE(buf) - 1, &n, NULL);
    SetConsoleMode(in, mode);
    con_print("\r\n");
    if (!ok)
        return 0;

    while (n > start && is_trim(buf[n - 1]))
        n--;
    while (start < n && is_trim(buf[start]))
        start++;
    wide_to_utf8(buf + start, n - start, out);
    SecureZeroMemory(buf, sizeof buf);
    return out->len > 0;
}

static char *append(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

/* 5x7 pixel font, one byte per row, bit 4 = leftmost pixel. Same font as assets/banner.svg. */
static const unsigned char k_word[8][7] = {
    {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E}, /* s */
    {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}, /* i */
    {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* l */
    {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}, /* i */
    {0x00, 0x00, 0x0F, 0x10, 0x10, 0x10, 0x0F}, /* c */
    {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}, /* o */
    {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}, /* r */
    {0x01, 0x01, 0x0F, 0x11, 0x11, 0x11, 0x0F}, /* d */
};

void con_banner(void)
{
    char line[512];

    for (int y = 0; y < 7; y++) {
        char *p = append(line, INK);
        for (int g = 0; g < 8; g++) {
            if (g)
                p = append(p, "  ");
            for (int bit = 4; bit >= 0; bit--)
                p = append(p, (k_word[g][y] >> bit) & 1 ? BLOCK BLOCK : "  ");
        }
        p = append(p, "  " AMBER BLOCK BLOCK BLOCK BLOCK BLOCK BLOCK BLOCK BLOCK RESET "\r\n");
        con_write(line, (DWORD)(p - line));
    }
}
