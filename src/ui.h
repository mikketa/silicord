#pragma once
#include <windows.h>
#include "model.h"
#include "msg.h"
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
    UI_MESSAGES,        /* msg_batch_t* */
    UI_SEND_FAILED,     /* text = reason */
    UI_ACTIVITY,        /* activity_t*: a message anywhere, or a read marker from another device */
    UI_RECONNECTING,    /* connection lost, text = status */
    UI_ONLINE,          /* session resumed */
};

enum { ACTIVITY_MESSAGE, ACTIVITY_ACK };

typedef struct {
    int kind;
    char channel_id[24];
    char message_id[24];
    int from_me;
    int mentions_me;
    sb_t author;
    sb_t preview;
} activity_t;

HWND ui_create(HINSTANCE inst);
void ui_show_login(void);
void ui_show_loading(const char *text);
void ui_post(UINT msg, sb_t *payload);
void ui_post_model(model_t *model);
void ui_post_batch(msg_batch_t *batch);
void ui_post_activity(activity_t *a);
void activity_free(activity_t *a);
sb_t *ui_text(const char *text);

/* Implemented by the application, called on the UI thread. */
void app_login_token(const char *token);
void app_logout(void);
void app_reconnect(void);
void app_quit(void);
/* Live messages are only forwarded for the open channel (empty string for none). */
void app_open_channel(const char *channel_id);
/* Loads the latest 50 messages, or the 50 before `before` if it is not NULL. */
void app_fetch_messages(const char *channel_id, const char *before);
void app_send_message(const char *channel_id, const char *text);
/* Tells Discord (and your other devices) the channel was read up to message_id. */
void app_ack(const char *channel_id, const char *message_id);
