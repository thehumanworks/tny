/* codex.h — internal state shared by the codex app-server backend TUs.
 * Protocol: docs/backends/codex-app-server.md (JSON-RPC shape without the
 * "jsonrpc" header, one object per WebSocket TEXT frame). */
#ifndef TNY_BACKENDS_CODEX_H
#define TNY_BACKENDS_CODEX_H

#include "core/backend.h"
#include "core/config.h"
#include "json/json.h"
#include "net/net.h"
#include "util/util.h"

#include <sys/types.h>

/* Everything below is fed by an untrusted peer: keep the caps small. */
#define CX_MAX_MSG_BYTES  (8u * 1024u * 1024u)
#define CX_MAX_TEXT       (64u * 1024u) /* per rendered text fragment */
#define CX_MAX_PENDING    6   /* in-flight tracked requests */
#define CX_MAX_APPROVALS  8   /* unanswered server->client approvals */
#define CX_MAX_STREAMED   16  /* item ids that already streamed deltas */
#define CX_MAX_DETAIL     400 /* chars of a tool summary we render */
#define CX_MAX_ID_TEXT    64  /* raw JSON of a server request id */
#define CX_RETRY_MAX      3   /* -32001 attempts, per docs (backoff+jitter) */

typedef enum {
    CXR_FREE = 0,
    CXR_TURN,
    CXR_INTERRUPT
} cx_reqkind;

typedef struct {
    int        id;
    cx_reqkind kind;
    char      *method;
    char      *params;   /* JSON object text, kept for a -32001 resend */
    int        attempts;
    int64_t    due_ms;   /* >0 while a resend is scheduled */
} cx_pending;

typedef struct {
    char *id;      /* raw JSON of the request id, doubles as perm_id */
    char *method;
} cx_approval;

typedef struct {
    tny_ctx *ctx;
    ws_conn *ws;
    char    *ws_url;
    char    *token;        /* bearer for the upgrade; never logged */

    /* spawned `codex app-server` (absent when attaching to --codex-ws) */
    pid_t  child;
    int    child_err;      /* read end of its stderr pipe, or -1 */
    buf_t  child_line;     /* partial stderr line */
    buf_t  stderr_tail;    /* recent host stderr, shown only on failure */
    bool   child_reaped;

    int   next_id;
    char *thread_id;
    char *turn_id;

    tny_event_cb cb;
    void        *ud;
    bool    turn_active;
    bool    cancel_sent;
    int64_t cancel_deadline;
    bool    dead;

    /* outgoing frames are queued so we never call into wslay from inside a
     * wslay recv callback; cx_flush() drains this after each pump. */
    char **outq;
    int    n_out, cap_out;

    cx_pending  pending[CX_MAX_PENDING];
    cx_approval approvals[CX_MAX_APPROVALS];
    char       *streamed[CX_MAX_STREAMED];
    int         n_streamed;

    /* synchronous request/response (handshake, thread start/resume) */
    int         wait_id;   /* -1 when nothing is awaited */
    bool        wait_done;
    yyjson_doc *wait_doc;  /* owned once wait_done is set */
} cx_impl;

/* ---- codex_rpc.c: events, framing, queue, pump ---- */
void cx_emit(cx_impl *o, const tny_event *ev);
void cx_emit_text(cx_impl *o, tny_event_kind k, const char *t);
void cx_end_turn(cx_impl *o, tny_stop_reason stop);
void cx_fail_turn(cx_impl *o, const char *msg);
/* Queue one raw JSON frame; takes ownership of `json`. */
void cx_queue(cx_impl *o, char *json);
int  cx_flush(cx_impl *o);
int  cx_request(cx_impl *o, const char *method, const char *params, cx_reqkind kind);
cx_pending *cx_pending_find(cx_impl *o, int id);
void cx_pending_clear(cx_pending *p);
void cx_notify(cx_impl *o, const char *method, const char *params);
void cx_respond_result(cx_impl *o, const char *id_json, const char *result);
void cx_respond_error(cx_impl *o, const char *id_json, int code, const char *msg);
void cx_process_retries(cx_impl *o);
/* One non-blocking recv/send round (poll_ms > 0 waits first). 0 ok, -1 dead. */
int  cx_pump_once(cx_impl *o, int poll_ms);
/* Blocking request/response; handshake and thread setup only. */
yyjson_doc *cx_request_sync(cx_impl *o, const char *method, const char *params,
                            int timeout_ms, char *err, size_t errlen);

/* ---- codex_msg.c ---- */
void cx_on_ws_msg(const char *data, size_t len, void *ud);

/* ---- codex_items.c: rendering item payloads ---- */
extern const char *const CX_ITEM_TYPE_KEYS[];
extern const char *const CX_ITEM_ID_KEYS[];
extern const char *const CX_STATUS_KEYS[];
extern const char *const CX_TURN_ID_KEYS[];
extern const char *const CX_TEXT_KEYS[];
const char *cx_first_str(yyjson_val *obj, const char *const *keys);
void        cx_emit_capped(cx_impl *o, tny_event_kind k, const char *t);
yyjson_val *cx_item_of(yyjson_val *params);
void        cx_append_words(buf_t *b, yyjson_val *v);
char       *cx_item_text(yyjson_val *item);   /* caller frees */
char       *cx_item_detail(const char *type, yyjson_val *item); /* caller frees */
bool        cx_type_is_message(const char *type);
bool        cx_item_is_user_echo(const char *type, yyjson_val *item);
bool        cx_type_is_reasoning(const char *type);
bool        cx_type_is_plan(const char *type);
void        cx_emit_plan(cx_impl *o, yyjson_val *item);
bool        cx_item_ok(yyjson_val *item);
bool        cx_emit_usage(cx_impl *o, yyjson_val *params);

/* ---- codex_proc.c ---- */
int  cx_pick_port(void);
int  cx_spawn(cx_impl *o, int port, char *err, size_t errlen);
void cx_drain_child_stderr(cx_impl *o);
bool cx_child_gone(cx_impl *o);
void cx_stop_child(cx_impl *o);
/* Run argv, capture up to outcap-1 bytes of stdout+stderr. Exit code or -1. */
int  cx_capture(char *const argv[], char *out, size_t outcap, int timeout_ms);

#endif
