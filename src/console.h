#pragma once
#include <windows.h>

void con_init(void);
void con_write(const char *s, DWORD len);
void con_print(const char *s);
void con_banner(void);
