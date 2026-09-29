#pragma once
#include <windows.h>
#include "model.h"
#include "msg.h"
#include "profile.h"
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
    UI_EVENT,           /* text = event name, a NUL, then the event JSON (servers, channels, roles) */
    UI_RECONNECTING,    /* connection lost, text = status */
    UI_ONLINE,          /* session resumed */
    UI_PROFILE,         /* profile_t* (username empty when it could not be loaded) */
    UI_DM_OPENED,       /* text = channel id of a direct message we asked to open */
    UI_FONT,            /* wParam = display name font id, payload = the font file (NULL on failure) */
    UI_TYPING,          /* text = channel id, NUL, user id, NUL, display name (empty in DMs) */
    UI_FRIEND_RESULT,   /* text = outcome of a friend request */
    UI_FORUM,           /* text = forum channel id, NUL, then the threads/search JSON (empty on failure) */
    UI_GIFS,            /* text = the query, NUL, then a JSON array of GIF objects (empty on failure) */
    UI_COMMANDS,        /* text = the guild or channel id, NUL, then the application-command-index (empty on failure) */
    UI_VOICE,           /* text = the VOICE_* state as a digit, NUL, then a status line */
    UI_VIDEO,           /* no payload: someone's video changed; see app_video_take() */
};

enum { ACTIVITY_MESSAGE, ACTIVITY_ACK };

typedef struct {
    int kind;
    char channel_id[24];
    char guild_id[24];
    char message_id[24];
    int from_me;
    int mentions_me;    /* named in the message */
    int everyone;       /* @everyone or @here */
    sb_t author;
    sb_t preview;
    sb_t mention_roles; /* comma-separated role ids */
} activity_t;

HWND ui_create(HINSTANCE inst);
void ui_show_login(void);
void ui_show_loading(const char *text);
void ui_post(UINT msg, sb_t *payload);
void ui_post_model(model_t *model);
void ui_post_batch(msg_batch_t *batch);
void ui_post_activity(activity_t *a);
void ui_post_profile(profile_t *p);
void ui_post_font(int id, sb_t *data);
sb_t *ui_text(const char *text);

/* Implemented by the application, called on the UI thread. */
void app_login_token(const char *token);
void app_logout(void);
void app_reconnect(void);
void app_quit(void);
/* Writes a line to the --debug log (no-op otherwise). */
void app_log(const char *text);
/* Live messages are only forwarded for the open channel (empty string for none). */
void app_open_channel(const char *channel_id);
/* Loads the latest 50 messages, or the 50 before `before` if it is not NULL. */
void app_fetch_messages(const char *channel_id, const char *before);
void app_send_message(const char *channel_id, const char *text);
/* Loads 50 messages around one (BATCH_HISTORY with `around` set), to jump to it. */
void app_fetch_around(const char *channel_id, const char *message_id);
/* Searches a server's messages (or a DM's); results come back as a BATCH_SEARCH batch. */
/* `params` are extra query parameters ("&author_id=1&has=image"), already encoded. */
void app_search(const char *guild_id, const char *dm_channel_id, const char *query, const char *params);
/* Trending GIFs (empty query) or a search, from Discord's GIF picker API; answered with UI_GIFS. */
void app_fetch_gifs(const char *query);
/* The slash commands usable in a server (guild_id) or a DM (channel_id); answered with UI_COMMANDS. */
void app_fetch_commands(const char *guild_id, const char *channel_id);
/* Clicks a bot's button, or picks `value` in its select menu. Failures come as UI_SEND_FAILED. */
void app_press_component(const char *guild_id, const char *channel_id, const char *message_id, const char *application_id,
                         int message_flags, int component_type, const char *custom_id, const char *value);
/* Runs a slash command: `data` is the interaction data built by cmd_build(). Failures come as UI_SEND_FAILED. */
void app_run_command(const char *guild_id, const char *channel_id, const char *application_id, const char *data);
/* Loads a forum's recent posts; they come back as UI_FORUM. */
void app_fetch_forum(const char *channel_id);
/* Loads the channel's pinned messages; they come back as a BATCH_PINS batch. */
void app_fetch_pins(const char *channel_id);
/* Recent mentions in every server and DM (Discord's inbox), answered as BATCH_PINS for INBOX_CHANNEL. */
#define INBOX_CHANNEL "@inbox"
void app_fetch_mentions(void);
/* Sends `text` as a reply to reply_id; `mention` pings its author. */
void app_send_reply(const char *channel_id, const char *text, const char *reply_id, int mention);
/* Forwards message_id (from channel_id, in guild_id or NULL for DMs) to `to_channel`. */
void app_forward(const char *to_channel, const char *channel_id, const char *guild_id, const char *message_id);
/* Sends a sticker, as a reply when reply_id is not NULL. */
void app_send_sticker(const char *channel_id, const char *sticker_id, const char *reply_id, int mention);
void app_edit_message(const char *channel_id, const char *message_id, const char *text);
/* Sends text with files (`paths`: n UTF-8 paths, each followed by a NUL); reply_id may be NULL. */
void app_send_files(const char *channel_id, const char *text, const char *reply_id, int mention, const char *paths, int n);
void app_delete_message(const char *channel_id, const char *message_id);
/* Shows "typing..." to the others for about ten seconds. */
void app_typing(const char *channel_id);
/* Mutes (for `minutes`, 0 until unmuted) or unmutes a server, or a channel of it (guild_id NULL for DMs). */
void app_mute(const char *guild_id, const char *channel_id, int muted, int minutes);
/* Changes notification settings: `fields` are JSON members such as "\"message_notifications\":1". */
void app_notify_settings(const char *guild_id, const char *channel_id, const char *fields);
/* Marks n channels read at once: `pairs` holds channel id, NUL, message id, NUL, repeated. */
void app_ack_bulk(const char *pairs, int n);
/* Leaves a server; it goes away with GUILD_DELETE. */
void app_leave_guild(const char *guild_id);
/* Tells Discord (and your other devices) the channel was read up to message_id. */
void app_ack(const char *channel_id, const char *message_id);
/* Marks a channel read up to message_id only, leaving what follows unread ("Mark Unread"). */
void app_ack_manual(const char *channel_id, const char *message_id);
/*
 * Starts a thread named `name` from message_id, or, with message_id NULL, a
 * forum post whose first message is `content`. The new thread comes back as
 * UI_EVENT "THREAD_OURS" (a channel object) to open; failures as UI_SEND_FAILED.
 */
void app_create_thread(const char *channel_id, const char *message_id, const char *name, const char *content);
/* Pins or unpins a message. */
void app_pin(const char *channel_id, const char *message_id, int pin);
/* Looks up a channel we do not know yet (a new DM); the answer comes back as UI_EVENT CHANNEL_CREATE. */
void app_fetch_channel(const char *channel_id);
/* Subscribes to a server channel's live typing and member list (op 14), as Discord does when opening it. */
void app_subscribe(const char *guild_id, const char *channel_id);
/* Friends: "PUT" accepts a request (or adds), "DELETE" removes, ignores, cancels or unblocks. */
void app_relationship(const char *user_id, const char *method);
/* Sends a friend request by username; the outcome comes back as UI_FRIEND_RESULT. */
void app_add_friend(const char *username);
/* Joins a server's voice channel (leaving any other); the connection reports back with UI_VOICE. */
void app_voice_join(const char *guild_id, const char *channel_id);
void app_voice_leave(void);
/* A call in a direct message: rings its other members, or (with `stop_for`, a user id) stops ringing that user. */
void app_call_ring(const char *channel_id, const char *stop_for);
/* Whether a member of our voice call is talking right now (any thread). */
int app_voice_speaking(const char *user_id);
/* Turns our camera on or off in the call; returns whether it is on. */
int app_video_camera(int on);
/* Mutes and deafens (deafened also mutes), and tells the others. */
void app_voice_set(int muted, int deafened);
/* This computer's voice settings. */
typedef struct {
    int in_device, out_device; /* 0 for the Windows default, else a device index + 1 */
    int in_volume, out_volume; /* percent, 0 to 200 */
    int sensitivity;           /* the level in dBFS that opens the microphone in voice activity mode */
    int push_to_talk, ptt_key; /* push to talk instead, on a virtual key */
} voice_prefs_t;
void app_voice_prefs(const voice_prefs_t *p);
/* The microphone's level in dBFS while it is open, else -100. */
int app_voice_mic_level(void);
/* Starts or stops hearing yourself (not during a call); returns whether the test runs. */
int app_voice_mic_test(int on);
/*
 * Someone's latest video picture in our call: calls take() with it (BGRA)
 * when it changed since *serial. Returns 0 without video from them, 1 when
 * unchanged, 2 when copied.
 */
int app_video_take(const char *user_id, unsigned *serial, void (*take)(void *ctx, const unsigned *bgra, int w, int h),
                   void *ctx);
/* Someone's volume in calls, in percent (0 to 200; 0 mutes them for you). */
void app_voice_user_volume(const char *user_id, int percent);
/* This session's presence: "online", "idle", "dnd" or "invisible", and custom status text (NULL or empty for none). */
void app_set_status(const char *status, const char *custom);
/* Changes synced user settings: `fields` are JSON members such as "\"developer_mode\":true". */
void app_user_settings(const char *fields);
/* Like app_subscribe, also asking for the member list rows [start, start + 99]. */
void app_subscribe_range(const char *guild_id, const char *channel_id, int start);
/* Asks the gateway for these members (nickname, roles); they come back as UI_EVENT GUILD_MEMBERS_CHUNK. */
void app_request_members(const char *guild_id, const char *const *user_ids, int n);
/* Sets our poll answers (none removes our vote). */
void app_vote(const char *channel_id, const char *message_id, const int *answers, int n);
/* Adds or removes our reaction; the gateway echoes it back as MESSAGE_REACTION_ADD / _REMOVE. */
void app_react(const char *channel_id, const char *message_id, const msg_reaction_t *r, int add);
/* The first users who reacted with r: UI_EVENT "REACTORS", text "key", NUL, then a JSON array of users. */
void app_fetch_reactors(const char *channel_id, const char *message_id, const msg_reaction_t *r, const char *key);
/* Loads a profile popout; `guild_id` (may be empty) adds the server profile and mutual servers. */
void app_fetch_profile(const char *user_id, const char *guild_id);
/*
 * Opens (or creates) the direct message with a user and sends `text` there if it is
 * not empty. The channel arrives as UI_EVENT CHANNEL_CREATE, then UI_DM_OPENED.
 */
void app_open_dm(const char *user_id, const char *text);
/*
 * Loads a display name font: `file` is its path in the Google Fonts
 * repository (all of them are under the SIL Open Font License). Cached on disk.
 */
void app_fetch_font(int id, const char *file);
