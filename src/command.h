#pragma once
#include "json.h"
#include "sb.h"

/*
 * Slash commands: looking them up in an application-command-index answer, and
 * turning what is typed after "/name" into the interaction's "data", the way
 * Discord reads a pasted command: "sub opt:value other:more words", or just
 * the value when there is a single option to fill.
 */

enum {
    OPT_SUB_COMMAND = 1,
    OPT_SUB_GROUP = 2,
    OPT_STRING = 3,
    OPT_INTEGER = 4,
    OPT_BOOLEAN = 5,
    OPT_USER = 6,
    OPT_CHANNEL = 7,
    OPT_ROLE = 8,
    OPT_MENTIONABLE = 9,
    OPT_NUMBER = 10,
    OPT_ATTACHMENT = 11,
};

/* Iterates the chat-input commands of an index (its "application_commands"): start with it->p = NULL. */
int cmd_next(json_t index, json_iter_t *it, json_t *cmd);
/* Finds the chat-input command named `name` (n bytes). */
int cmd_find(json_t index, const char *name, size_t n, json_t *out);
/* Finds the application `app_id` of the index ("applications"). */
int cmd_app(json_t index, const char *app_id, json_t *out);

/*
 * Walks the subcommand names at the start of `args` and returns the options
 * that apply there, in `opts` (an array, or an empty slice). `*used` is how
 * many bytes of `args` named subcommands. Returns 0 when a subcommand is
 * still expected: `opts` then lists the choices.
 */
int cmd_leaf(json_t cmd, const char *args, size_t n, json_t *opts, size_t *used);

/*
 * Builds the interaction data for `cmd` from `args`. Returns 0 and a message
 * in `err` when an option is missing or does not fit its type.
 */
int cmd_build(json_t cmd, const char *args, size_t n, sb_t *out, char *err, size_t errn);
