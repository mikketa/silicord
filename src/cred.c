#include <windows.h>
#include <wincred.h>
#include "cred.h"

#define TARGET L"silicord/token"

int cred_save(const char *token, size_t len)
{
    CREDENTIALW c = {0};

    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = TARGET;
    c.UserName = L"silicord";
    c.CredentialBlob = (LPBYTE)token;
    c.CredentialBlobSize = (DWORD)len;
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&c, 0) != 0;
}

int cred_load(sb_t *out)
{
    PCREDENTIALW c;

    if (!CredReadW(TARGET, CRED_TYPE_GENERIC, 0, &c))
        return 0;
    sb_addn(out, (const char *)c->CredentialBlob, c->CredentialBlobSize);
    SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize);
    CredFree(c);
    return out->len > 0;
}

int cred_delete(void)
{
    return CredDeleteW(TARGET, CRED_TYPE_GENERIC, 0) != 0;
}
