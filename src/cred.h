#pragma once
#include "sb.h"

/*
 * Token storage in the Windows Credential Manager. The blob is encrypted by
 * Windows (DPAPI) and bound to the current user on this machine.
 */
int cred_save(const char *token, size_t len);
int cred_load(sb_t *out);
int cred_delete(void);
