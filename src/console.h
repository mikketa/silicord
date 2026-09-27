#pragma once
#include <windows.h>
#include "sb.h"

/* Debug log console, opened with --debug. */
void con_init(void);
void con_write(const char *s, DWORD len);
void con_print(const char *s);
void con_print_sb(const sb_t *sb);
