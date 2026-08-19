/* acp_server.h — private state shared by the `tny acp` server TUs.
 * One connection, one active session, one active prompt (docs/backends/acp.md). */
#ifndef TNY_ACP_SERVER_H
#define TNY_ACP_SERVER_H

#include "backends/acp/acp_wire.h"
#include "core/backend.h"
#include "core/config.h"
#include "core/perm.h"
#include "core/session.h"

typedef struct {
    tny_ctx *ctx;
    int  in_fd, out_fd;      /* stdin / stdout */
    acp_reader rd;
    int64_t next_id;
    bool initialized;
    bool eof;

    /* the one session */
    tny_session *session;
    perm_engine *perm;
    tny_backend *bk;
    char *session_id;

    /* turn state */
    bool turn_active;
    bool turn_done;
    bool cancel_requested;   /* session/cancel seen; applied outside dispatch */
    bool cancelled;
    tny_stop_reason stop;
    buf_t last_error;

    /* pending outbound permission request */
    bool    perm_waiting;
    bool    perm_answered;
    int64_t perm_req_id;
    tny_perm_decision perm_result;
} acp_srv;

/* acp_server.c */
int  acp_srv_pump(acp_srv *s, int timeout_ms); /* -1 when stdin ends */
void acp_srv_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* acp_turn.c */
/* Run one prompt to completion. Writes the ACP stopReason into `reason`
 * (borrowed static text). 0 ok, -1 when the transport died. */
int acp_srv_run_turn(acp_srv *s, const char *text, const char **reason);
/* Bind the native backend's approval hook to this connection. */
void acp_srv_bind(acp_srv *s);

#endif
