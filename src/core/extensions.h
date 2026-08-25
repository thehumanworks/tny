/* extensions.h — optional out-of-process Python event hooks.
 *
 * The manager owns discovery and the Python host process.  Runtime callers
 * invoke it only from a safe point outside backend callbacks; extension
 * failures are returned as data and never make the agent loop fail closed. */
#ifndef TNY_EXTENSIONS_H
#define TNY_EXTENSIONS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct tny_extensions tny_extensions;

typedef enum {
    TNY_EXTENSIONS_EMPTY = 0,      /* no ~/.tny/extensions entries */
    TNY_EXTENSIONS_DORMANT,        /* entries found; host not spawned yet */
    TNY_EXTENSIONS_READY,
    TNY_EXTENSIONS_UNAVAILABLE     /* optional host/python unavailable */
} tny_extensions_state;

typedef enum {
    TNY_EXTENSION_ACTION_CONTEXT = 1,
    TNY_EXTENSION_ACTION_CONTINUE,
    TNY_EXTENSION_ACTION_STOP
} tny_extension_action_kind;

typedef enum {
    TNY_EXTENSION_MESSAGE_USER = 0,
    TNY_EXTENSION_MESSAGE_CUSTOM
} tny_extension_message_kind;

typedef struct {
    tny_extension_action_kind kind;
    char *extension;   /* extension name, owned */
    char *content;     /* context/continuation content, owned */
    char *custom_type; /* optional custom context/message type, owned */
    char *reason;      /* optional stop reason, owned */
    tny_extension_message_kind message_kind;
    bool display;      /* action should be visible in the transcript */
} tny_extension_action;

typedef struct {
    char *extension; /* extension name when known, owned */
    char *handler_id;
    char *event;     /* normalized event name, empty for discovery failures */
    char *code;      /* stable manager/host category, owned */
    char *message;   /* bounded diagnostic, owned; never host stderr */
} tny_extension_failure;

typedef struct {
    tny_extension_action *actions; /* registration/invocation order */
    size_t action_count;
    tny_extension_failure *failures;
    size_t failure_count;
} tny_extension_result;

/* Scan <tny_dir>/extensions for direct *.py files and one-level */
/* <name>/index.py entries. Discovery is deterministic and does not spawn. */
tny_extensions *tny_extensions_new(const char *tny_dir, const char *cwd,
                                   int handler_timeout_ms);
void tny_extensions_free(tny_extensions *extensions);

tny_extensions_state tny_extensions_get_state(const tny_extensions *extensions);
size_t tny_extensions_entry_count(const tny_extensions *extensions);
/* A short manager-owned status string; never contains child stderr. */
const char *tny_extensions_status(const tny_extensions *extensions);

/* Run every handler subscribed to event_name, serially. event_json must be a
 * bounded JSON object produced from tny's normalized event schema, not a raw
 * provider payload. Missing Python/host, handler exceptions, bad responses,
 * and timeouts are reported in out->failures and return 0 (fail open).
 * -1 is reserved for invalid caller input or local allocation failure. */
int tny_extensions_invoke(tny_extensions *extensions,
                          const char *event_name, const char *event_json,
                          tny_extension_result *out);

void tny_extension_result_free(tny_extension_result *result);

#endif
