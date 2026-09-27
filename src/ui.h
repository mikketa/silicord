#pragma once
#include <windows.h>
#include "model.h"
#include "sb.h"

/*
 * Win32 window. Worker threads talk to it with ui_post(): the payload is a
 * heap sb_t* (see ui_text) that the UI thread frees, or NULL. UI_READY
 * carries a model_t* instead (ui_post_model).
 */
enum {
    UI_QR = WM_APP + 1, /* URL to show as a QR code */
    UI_SCANNED,         /* account name, waiting for confirmation on the phone */
    UI_STATUS,          /* one-line status */
    UI_TOKEN,           /* token received from the QR login */
    UI_LOGIN_FAILED,    /* back to the login screen, text = reason */
    UI_ACCOUNT,         /* logged in, text = account name */
    UI_READY,           /* model_t* built from READY */
    UI_DISCONNECTED,    /* session ended, text = reason */
    UI_IMAGE,           /* image loader result, see img.h */
};

HWND ui_create(HINSTANCE inst);
void ui_show_login(void);
void ui_show_loading(const char *text);
void ui_post(UINT msg, sb_t *payload);
void ui_post_model(model_t *model);
sb_t *ui_text(const char *text);

/* Implemented by the application, called on the UI thread. */
void app_login_token(const char *token);
void app_logout(void);
void app_reconnect(void);
void app_quit(void);
