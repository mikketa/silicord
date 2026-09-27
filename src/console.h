#pragma once
#include <windows.h>
#include "sb.h"

void con_init(void);
void con_write(const char *s, DWORD len);
void con_print(const char *s);
void con_print_sb(const sb_t *sb);
void con_banner(void);
/* Reads one line without echoing it, trimmed of whitespace and surrounding quotes. */
int con_read_secret(sb_t *out);
