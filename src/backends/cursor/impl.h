/* impl.h — shared state between cursor.c (vtable, request bodies) and
 * map.c (RunStreamMessage -> normalized events). Directory-private. */
#ifndef TNY_BACKENDS_CURSOR_IMPL_H
#define TNY_BACKENDS_CURSOR_IMPL_H

#include "backends/cursor/cursor.h"
#include "backends/cursor/callbacks.h"
#include "backends/cursor/sdk_client.h"

typedef enum {
    CU_STREAM_NONE = 0,
    CU_STREAM_SEND,
    CU_STREAM_OBSERVE,
} cu_stream_kind;

typedef enum {
    CU_TEXT_NONE = 0,
    CU_TEXT_SDK,
    CU_TEXT_INTERACTION,
    CU_TEXT_STEP,
} cu_text_source;

typedef struct {
    tny_ctx *ctx;
    cursor_bridge bridge;
    cursor_sdk_client sdk;
    cursor_callbacks *callbacks;
    bool connected;

    char *api_key;                 /* CURSOR_API_KEY (never logged) */
    char *model;                   /* explicit model id: local agents require one */
    char *agent_id;                /* session pointer */
    char *run_id;                  /* current run, for CancelRun */
    char *observe_offset;          /* last exclusive offset produced by ObserveRun */
    char *observe_progress_offset; /* last offset that reset the recovery budget */
    cu_stream_kind stream_kind;
    bool cancel_requested;
    bool cancel_sent;
    bool cancel_attempted;
    bool saw_terminal_result;
    bool saw_done;
    bool observe_replay;
    bool observe_retry_pending;
    unsigned observe_no_progress_attempts;
    int64_t observe_retry_at_ms;
    uint64_t *send_hashes;
    size_t send_hash_count;
    size_t send_hash_capacity;
    size_t replay_index;
    cu_text_source text_source;
    cu_text_source mapping_source;
    char *ephemeral_root;

    /* reasoning effort resolved against the ListModels catalog: model params
     * travel as ModelSelection.params [{id,value}] and both ids and values
     * are model-specific, so tny asks the catalog instead of guessing. */
    char *effort_for;   /* the ctx->reasoning_effort this resolution is for */
    char *effort_param; /* catalog parameter id (e.g. "effort") */
    char *effort_value; /* catalog-allowed value actually sent */
    char *effort_note;  /* one-shot status line when resolution degraded */

    tny_backend_event_cb cb;
    void *ud;
    bool active;   /* a turn is in flight */
    bool ended;    /* TURN_END already emitted for this turn */
    bool got_text; /* suppress duplicate text from `result` */
    bool saw_error;
    bool usage_sent;
    buf_t last_status;     /* failure text often lives on status.message */
    buf_t last_tool_start; /* id+name+detail: drop re-emitted `running` frames */
    int64_t in_tok, out_tok;
    double cost;
    bool has_cost;
} cu_impl;

void cu_emit(cu_impl *o, const tny_backend_event *ev);
void cu_emit_text(cu_impl *o, tny_event_kind k, const char *t, size_t n);
void cu_end_turn(cu_impl *o, tny_stop_reason stop);

/* Consume an offset before mapping a RunStreamMessage. Returns false for a
 * replay already delivered by Send or an earlier ObserveRun connection. */
bool cu_accept_frame(cu_impl *o, const char *payload, size_t len);
int cu_append_images(buf_t *body, const char **images, char *err, size_t errlen);
char *cu_ephemeral_root_create(char *err, size_t errlen);
void cu_ephemeral_root_remove(char **root);
int cu_start_observe(cu_impl *o, bool from_start, char *err, size_t errlen);
int cu_send_cancel(cu_impl *o, char *err, size_t errlen);

/* Connect envelope callback for the Send stream. */
void cu_on_frame(uint8_t flags, const char *payload, size_t len, void *ud);

#endif
