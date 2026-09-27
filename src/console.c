#include "console.h"
#include "sc_asm.h"

static HANDLE g_out;

void con_init(void)
{
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"Silicord debug log");
}

void con_write(const char *s, DWORD len)
{
    DWORD written;

    if (g_out)
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
