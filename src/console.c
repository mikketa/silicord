#include "console.h"

static HANDLE g_out;
static HANDLE g_file;

void con_init(void)
{
    wchar_t path[MAX_PATH + 32];
    DWORD n = GetTempPathW(MAX_PATH, path);

    if (n && n < MAX_PATH) {
        lstrcpyW(path + n, L"silicord-debug.log");
        g_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_file == INVALID_HANDLE_VALUE)
            g_file = NULL;
    }
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"Silicord debug log");
}

static void con_write(const char *s, DWORD len)
{
    DWORD written;

    if (g_out)
        WriteFile(g_out, s, len, &written, NULL);
    if (g_file)
        WriteFile(g_file, s, len, &written, NULL);
}

void con_print_sb(const sb_t *sb)
{
    if (sb->len)
        con_write(sb->data, (DWORD)sb->len);
}
