/* acp_client.h — private state shared by the `--backend acp` client TUs.
 * See docs/backends/acp.md. Everything here is internal to src/backends/acp. */
#ifndef TNY_ACP_CLIENT_H
#define TNY_ACP_CLIENT_H

#include "backends/acp/acp_wire.h"
#include "core/backend.h"
#include "core/config.h"
#include "net/net.h"

#include <sys/types.h>

#define ACP_MAX_PERMS 8

/* One outstanding session/request_permission from the agent. */
typedef struct {
    char *id_raw;       /* verbatim JSON id of the pending request */
    char *allow_once;
    char *allow_always;
    char *reject;
    char *summary;
} ac_perm;

typedef struct {
    tny_ctx *ctx;
    pid_t pid;
    int    in_fd, out_fd, err_fd;
    ws_conn *ws;           /* --agent ws://…: remote agent, no spawn */
    acp_reader out_r, err_r;
    int64_t next_id;
    char  *session_id;
    bool   load_session;   /* agent advertises loadSession */
    bool   effort_noted;   /* the effort-unsupported status went out once */

    /* turn state */
    bool    turn_active;
    bool    cancelled;
    int64_t prompt_id;
    tny_backend_event_cb cb;
    void   *ud;
    ac_perm perms[ACP_MAX_PERMS];
    int     nperms;

    /* blocking-request slot (used only outside a turn) */
    int64_t     wait_id;
    yyjson_doc *wait_doc;
} ac_impl;

/* acp_events.c — normalization of everything the agent sends. */
void ac_emit(ac_impl *o, const tny_backend_event *ev);
void ac_emit_text(ac_impl *o, tny_event_kind k, const char *t, size_t n);
void ac_emit_end(ac_impl *o, tny_stop_reason stop);

ac_perm *ac_perm_find(ac_impl *o, const char *id_raw);
void     ac_perm_drop(ac_impl *o, ac_perm *p);
void     ac_perms_clear(ac_impl *o);

void ac_handle_update(ac_impl *o, yyjson_val *params);
void ac_handle_agent_request(ac_impl *o, yyjson_val *msg, const char *method,
                             yyjson_val *params);
void ac_handle_prompt_response(ac_impl *o, yyjson_val *msg);

/* acp_proc.c — transport. The agent is either a spawned process (JSONL over
 * stdio) or, when agent_argv[0] is a ws:// / wss:// URL, a remote host
 * reached over WebSocket (one JSON-RPC message per text frame). Everything
 * above these helpers is one code path (docs/adr/0017). */
bool ac_agent_is_ws(const char *argv0);
int  ac_connect_ws(ac_impl *o, char *errbuf, size_t errlen);
/* Route to the live transport (pipe write or ws frame). 0 ok, -1 dead. */
int  ac_tx_request(ac_impl *o, int64_t id, const char *method, const char *params);
int  ac_tx_notify(ac_impl *o, const char *method, const char *params);
int  ac_tx_result(ac_impl *o, const char *id_raw, const char *result_json);
int  ac_tx_error(ac_impl *o, const char *id_raw, int code, const char *msg);
/* Fill fds to wait on for agent traffic; returns count. */
int  ac_transport_pollfds(ac_impl *o, struct pollfd *fds, int max);
bool ac_on_path(const char *bin);
int  ac_spawn_agent(ac_impl *o, char *errbuf, size_t errlen);
/* Exit status once the agent's stdout closed, or -1 if it is still running. */
int  ac_reap_agent(ac_impl *o);
/* Read everything pending: 0 ok, -1 EOF/error, -2 message over the 8 MiB cap. */
int  ac_pump_reads(ac_impl *o);
/* Blocking request/response. Setup only — never during a turn. */
yyjson_doc *ac_rpc(ac_impl *o, const char *method, const char *params,
                   char *errbuf, size_t errlen);

#endif
