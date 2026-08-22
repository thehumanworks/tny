/* events.h — the one normalized event set every backend maps onto.
 * See docs/architecture.md. Never leak host-specific types past this. */
#ifndef TNY_EVENTS_H
#define TNY_EVENTS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
    TNY_EV_TEXT_DELTA,   /* assistant text fragment */
    TNY_EV_THINKING,     /* reasoning fragment (render dim) */
    TNY_EV_TOOL_START,   /* tool call began */
    TNY_EV_TOOL_END,     /* tool call finished */
    TNY_EV_PERMISSION,   /* backend needs an approval decision */
    TNY_EV_PLAN,         /* plan / todo update, plain text */
    TNY_EV_USAGE,        /* token usage numbers */
    TNY_EV_TURN_END,     /* turn finished (see stop) */
    TNY_EV_ERROR,        /* fatal-for-this-turn error */
    TNY_EV_STATUS,       /* one-line progress note (stderr / status area) */
    TNY_EV_STEER_REJECTED /* a steer() the host accepted was refused later.
                           * text (+len) carries the rejected user text; the
                           * frontend re-queues it (docs/adr/0011, 0013) */
} tny_event_kind;

typedef enum {
    TNY_STOP_DONE,        /* model finished */
    TNY_STOP_INTERRUPTED, /* user cancel */
    TNY_STOP_DENIED,      /* permission unresolved / denied in ask-mode CLI */
    TNY_STOP_STEP_LIMIT,
    TNY_STOP_ERROR
} tny_stop_reason;

/* Options a host offers for a permission request. Map onto y / a / n. */
typedef enum {
    TNY_PERM_ALLOW_ONCE   = 1 << 0,
    TNY_PERM_ALLOW_ALWAYS = 1 << 1, /* session grant */
    TNY_PERM_DENY         = 1 << 2
} tny_perm_options;

typedef struct {
    tny_event_kind kind;
    /* TEXT_DELTA / THINKING / PLAN / STATUS / ERROR: text (+len) */
    const char *text;
    size_t      text_len;
    /* TOOL_START / TOOL_END */
    const char *tool_name;
    const char *tool_id;
    const char *tool_detail;  /* short human line: args summary or result summary */
    bool        tool_ok;      /* TOOL_END */
    /* PERMISSION */
    const char *perm_id;      /* opaque id to pass to respond_permission */
    const char *perm_summary; /* what is being requested */
    int         perm_options; /* tny_perm_options bitmask */
    /* USAGE */
    int64_t     in_tokens, out_tokens;
    /* TURN_END */
    tny_stop_reason stop;
} tny_event;

/* Backends emit events through this callback. Must not block on I/O. */
typedef void (*tny_event_cb)(const tny_event *ev, void *ud);

#endif
