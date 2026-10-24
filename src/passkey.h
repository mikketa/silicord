#pragma once
#include <windows.h>
#include "auth.h"
#include "sb.h"

/*
 * Passkeys and security keys through Windows (webauthn.dll): Windows Hello,
 * a USB or NFC key, or a phone nearby. Windows shows its own dialog; we sign
 * the clientDataJSON a browser on discord.com would.
 */

/* webauthn.dll is there (Windows 10 1903 and later). */
int passkey_available(void);
/*
 * Asks for an assertion matching `w` and writes the credential JSON for Discord.
 * Blocks until the user is done: call from a worker thread. Returns 1, or 0 with
 * `error` set to a sentence (left empty when the user cancelled).
 */
int passkey_get(HWND parent, const auth_webauthn_t *w, sb_t *credential, sb_t *error);
/* Closes the Windows dialog of a passkey_get() in progress, from any thread. */
void passkey_cancel(void);
