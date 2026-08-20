/* impl.h — shared state between cursor.c (vtable, request bodies) and
 * map.c (RunStreamMessage -> normalized events). Directory-private. */
#ifndef TNY_BACKENDS_CURSOR_IMPL_H
#define TNY_BACKENDS_CURSOR_IMPL_H

#include "backends/cursor/cursor.h"

typedef struct {
    tny_ctx      *ctx;
    cursor_bridge bridge;
    cursor_rpc    rpc;
    cursor_stream stream;
    bool          connected;

    char *api_key;    /* CURSOR_API_KEY (never logged) */
    char *model;      /* explicit model id: local agents require one */
    char *agent_id;   /* session pointer */
    char *run_id;     /* current run, for CancelRun */

    tny_event_cb cb;
    void        *ud;
    bool         active;     /* a turn is in flight */
    bool         ended;      /* TURN_END already emitted for this turn */
    bool         got_text;   /* suppress duplicate text from `result` */
    bool         saw_error;
    bool         usage_sent;
    buf_t        last_status; /* failure text often lives on status.message */
    int64_t      in_tok, out_tok;
} cu_impl;

void cu_emit(cu_impl *o, const tny_event *ev);
void cu_emit_text(cu_impl *o, tny_event_kind k, const char *t, size_t n);
void cu_end_turn(cu_impl *o, tny_stop_reason stop);

/* Connect envelope callback for the Send stream. */
void cu_on_frame(uint8_t flags, const char *payload, size_t len, void *ud);

#endif
