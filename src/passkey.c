#include <windows.h>
#include <webauthn.h>
#include "passkey.h"
#include "mem.h"
#include "utf.h"

/* Loaded at run time: webauthn.dll is missing before Windows 10 1903, and the client must still start. */
typedef HRESULT(WINAPI *get_assertion_fn)(HWND, LPCWSTR, PCWEBAUTHN_CLIENT_DATA,
                                          PCWEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS, PWEBAUTHN_ASSERTION *);
typedef void(WINAPI *free_assertion_fn)(PWEBAUTHN_ASSERTION);
typedef HRESULT(WINAPI *cancellation_id_fn)(GUID *);
typedef HRESULT(WINAPI *cancel_fn)(const GUID *);

static struct {
    INIT_ONCE once;
    HMODULE dll;
    get_assertion_fn get_assertion;
    free_assertion_fn free_assertion;
    cancellation_id_fn cancellation_id;
    cancel_fn cancel;
    SRWLOCK lock;
    GUID op;        /* the operation in progress, for passkey_cancel() */
    int running;
} g_pk; /* all zeros: INIT_ONCE_STATIC_INIT and SRWLOCK_INIT */

static BOOL CALLBACK load(PINIT_ONCE once, PVOID arg, PVOID *ctx)
{
    (void)once;
    (void)arg;
    (void)ctx;
    g_pk.dll = LoadLibraryExW(L"webauthn.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (g_pk.dll) {
        g_pk.get_assertion = (get_assertion_fn)(void *)GetProcAddress(g_pk.dll, "WebAuthNAuthenticatorGetAssertion");
        g_pk.free_assertion = (free_assertion_fn)(void *)GetProcAddress(g_pk.dll, "WebAuthNFreeAssertion");
        g_pk.cancellation_id = (cancellation_id_fn)(void *)GetProcAddress(g_pk.dll, "WebAuthNGetCancellationId");
        g_pk.cancel = (cancel_fn)(void *)GetProcAddress(g_pk.dll, "WebAuthNCancelCurrentOperation");
    }
    return TRUE;
}

int passkey_available(void)
{
    InitOnceExecuteOnce(&g_pk.once, load, NULL, NULL);
    return g_pk.get_assertion && g_pk.free_assertion;
}

/* What the user should read when Windows could not give an assertion; empty when they cancelled. */
static void explain(HRESULT hr, sb_t *error)
{
    char line[96];

    if (hr == NTE_USER_CANCELLED || hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return;
    if (hr == NTE_NOT_FOUND || hr == NTE_DEVICE_NOT_FOUND) {
        sb_add(error, "No passkey for Discord was found.");
        return;
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) {
        sb_add(error, "The passkey request timed out. Try again.");
        return;
    }
    wsprintfA(line, "Windows could not use a passkey (error 0x%08X).", (unsigned)hr);
    sb_add(error, line);
}

int passkey_get(HWND parent, const auth_webauthn_t *w, sb_t *credential, sb_t *error)
{
    WEBAUTHN_CREDENTIAL creds[AUTH_MAX_CREDS];
    WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS opt = {0};
    WEBAUTHN_CLIENT_DATA cd = {0};
    PWEBAUTHN_ASSERTION a = NULL;
    GUID op;
    sb_t client = {0};
    wchar_t *rp;
    HRESULT hr;
    int ok = 0;

    if (!passkey_available()) {
        sb_add(error, "Passkeys need Windows 10 version 1903 or later.");
        return 0;
    }
    auth_client_data(&client, (const unsigned char *)w->challenge.data, w->challenge.len);
    cd.dwVersion = WEBAUTHN_CLIENT_DATA_CURRENT_VERSION;
    cd.cbClientDataJSON = (DWORD)client.len;
    cd.pbClientDataJSON = (PBYTE)client.data;
    cd.pwszHashAlgId = WEBAUTHN_HASH_ALGORITHM_SHA_256;

    /* Version 3: the credential list, user verification and a cancellation id, all in the first webauthn.dll. */
    opt.dwVersion = WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS_VERSION_3;
    opt.dwTimeoutMilliseconds = w->timeout;
    for (int i = 0; i < w->ncreds; i++) {
        creds[i].dwVersion = WEBAUTHN_CREDENTIAL_CURRENT_VERSION;
        creds[i].cbId = (DWORD)w->creds[i].len;
        creds[i].pbId = (PBYTE)w->creds[i].data;
        creds[i].pwszCredentialType = WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY;
    }
    opt.CredentialList.cCredentials = (DWORD)w->ncreds;
    opt.CredentialList.pCredentials = w->ncreds ? creds : NULL;
    opt.dwAuthenticatorAttachment = WEBAUTHN_AUTHENTICATOR_ATTACHMENT_ANY;
    opt.dwUserVerificationRequirement = w->uv == 2   ? WEBAUTHN_USER_VERIFICATION_REQUIREMENT_REQUIRED
                                        : w->uv == 1 ? WEBAUTHN_USER_VERIFICATION_REQUIREMENT_PREFERRED
                                                     : WEBAUTHN_USER_VERIFICATION_REQUIREMENT_DISCOURAGED;
    if (g_pk.cancellation_id && SUCCEEDED(g_pk.cancellation_id(&op))) {
        opt.pCancellationId = &op;
        AcquireSRWLockExclusive(&g_pk.lock);
        g_pk.op = op;
        g_pk.running = 1;
        ReleaseSRWLockExclusive(&g_pk.lock);
    }

    rp = utf8_to_wide(w->rp_id.data ? w->rp_id.data : "discord.com", w->rp_id.data ? w->rp_id.len : 11);
    hr = g_pk.get_assertion(parent, rp, &cd, &opt, &a);
    mem_free(rp);
    AcquireSRWLockExclusive(&g_pk.lock);
    g_pk.running = 0;
    ReleaseSRWLockExclusive(&g_pk.lock);

    if (SUCCEEDED(hr) && a) {
        auth_assertion_t out = {0};
        out.id = a->Credential.pbId;
        out.id_n = a->Credential.cbId;
        out.auth_data = a->pbAuthenticatorData;
        out.auth_data_n = a->cbAuthenticatorData;
        out.client_data = (const unsigned char *)client.data;
        out.client_data_n = client.len;
        out.signature = a->pbSignature;
        out.signature_n = a->cbSignature;
        out.user_handle = a->pbUserId;
        out.user_handle_n = a->cbUserId;
        /* Which kind of authenticator answered is only known from version 4 on, through its transport. */
        if (a->dwVersion >= WEBAUTHN_ASSERTION_VERSION_4)
            out.attachment = a->dwUsedTransport & WEBAUTHN_CTAP_TRANSPORT_INTERNAL ? "platform" : "cross-platform";
        auth_credential(credential, &out);
        ok = 1;
    } else {
        explain(hr, error);
    }
    if (a)
        g_pk.free_assertion(a);
    sb_free(&client);
    return ok;
}

void passkey_cancel(void)
{
    if (!passkey_available() || !g_pk.cancel)
        return;
    AcquireSRWLockExclusive(&g_pk.lock);
    if (g_pk.running)
        g_pk.cancel(&g_pk.op);
    ReleaseSRWLockExclusive(&g_pk.lock);
}
