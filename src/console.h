#pragma once
#include <windows.h>
#include "sb.h"

/* Debug log, opened with --debug: a console window plus %TEMP%\silicord-debug.log. */
void con_init(void);
void con_print_sb(const sb_t *sb);
