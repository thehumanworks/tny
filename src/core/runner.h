/* runner.h — the detached session-runner process and its wire client
 * (docs/adr/0053). Every native turn executes inside a forked, setsid()
 * runner that owns the backend, the engine, and every session.json write;
 * the caller renders events streamed over <session-dir>/sock as
 * newline-delimited JSON. A dead client is a rendering loss, never an
 * agent loss. wasm has no fork: tny_isolation_enabled() is false there and
 * callers keep their in-process paths (docs/adr/0017). */
#ifndef TNY_RUNNER_H
#define TNY_RUNNER_H

#include "core/backend.h"
#include "core/config.h"
#include "core/events.h"
#include "core/session.h"

#include <sys/types.h>

struct tny_engine;

/* Native default on; TNY_ISOLATE=0 (debug escape hatch) or wasm turn it
 * off. Ephemeral sessions always run in-process: there is no session
 * directory to serve from and nothing durable to survive for. On macOS,
 * once the caller has initialized SecureTransport, subsequent turns stay
 * in-process because Apple's trust runtime is unsafe after fork pre-exec. */
bool tny_isolation_enabled(const tny_ctx *ctx);
/* Pure policy seam for tests; production passes nstream_fork_safe(). */
bool tny_isolation_policy(const tny_ctx *ctx, bool transport_fork_safe);

/* <session-dir>/sock; malloc'd, NULL when the path exceeds sun_path. */
char *tny_runner_sock_path(const char *session_dir);

/* ---- spawn ---- */

typedef struct {
    bool serve;                  /* TUI: keep serving turns until `end`/EOF;
                                  * else finalize and exit after one turn */
    const char *initial_prompt;  /* -B: run detached with no client */
    const char **initial_images; /* NULL-terminated array or NULL */
    bool continue_recovery;      /* fold recovery.json into the first turn */
    bool no_host_registry;       /* docs/adr/0031 decision 8 */
} tny_runner_opts;

/* Fork the runner. The listener is bound in the parent before the fork so
 * a client connect never races runner startup. Parent: returns the child
 * pid (>0), listener closed. Child: never returns (_exit). -1 on error.
 * once mode requires the caller to hold the session writer flock — it is
 * inherited across the fork (docs/adr/0031 decision 4). */
pid_t tny_runner_spawn(tny_ctx *ctx, tny_session_state *session, const tny_runner_opts *opts,
                       char *err, size_t errlen);

/* The exact --json result object for one finished turn: shared by the
 * runner's finalize and cmd_ask's in-process path so foreground output and
 * the stored session `result` stay byte-identical (docs/adr/0031). Any
 * argument except ctx may be NULL/absent. Malloc'd, trailing newline. */
char *tny_turn_result_json(tny_ctx *ctx, struct tny_engine *engine, tny_session_state *session,
                           const char *output, const char *host_tools_items,
                           const char *extension_items, const char *errline, int exit_code);

/* ---- client ---- */

typedef enum {
    TNY_RMSG_EVENT = 0, /* ev is a normalized backend event */
    TNY_RMSG_HELLO,     /* pid/provider/model/state; snapshot may follow */
    TNY_RMSG_SNAPSHOT,  /* text: output accumulated before this attach */
    TNY_RMSG_RECOVERY,  /* text: replayed recovery partial */
    TNY_RMSG_LOG,       /* text: one runner-side stderr line (host stderr,
                         * diagnostics) — the pre-0053 terminal trail */
    TNY_RMSG_ASK_USER,  /* id + text: owner-only free-text question */
    TNY_RMSG_TURN_END,  /* ev.stop + exit_code + result_json */
    TNY_RMSG_TURN_ERR,  /* text: the turn could not start */
    TNY_RMSG_BYE        /* text: reason; the runner is exiting */
} tny_runner_msg_kind;

typedef struct tny_runner_msg {
    tny_runner_msg_kind kind;
    tny_backend_event ev; /* strings borrow from the owned line below */
    char *text;
    char *id;
    int exit_code;
    char *result_json;
    pid_t pid;
    char *provider;
    char *model;
    bool turn_active; /* HELLO: a turn is streaming right now */
    struct tny_runner_msg *next;
    void *doc; /* yyjson_doc backing every borrowed pointer above */
} tny_runner_msg;

typedef struct tny_runner_client tny_runner_client;

typedef enum { TNY_RUNNER_OWNER = 1, TNY_RUNNER_OBSERVER, TNY_RUNNER_TOOL } tny_runner_role;
bool tny_runner_role_allows(tny_runner_role role, const char *op);

/* Connect and send the mandatory client-role handshake. An owner is unique;
 * can_answer_questions distinguishes the interactive TUI from `tny ask`. */
tny_runner_client *tny_runner_client_connect(const char *sock_path, int timeout_ms,
                                             tny_runner_role role, bool can_answer_questions);
int tny_runner_client_fd(const tny_runner_client *c);
/* Drain readable bytes into parsed messages. 0 ok, -1 connection gone
 * (already-queued messages remain poppable). */
int tny_runner_client_pump(tny_runner_client *c);
tny_runner_msg *tny_runner_client_pop(tny_runner_client *c);
void tny_runner_msg_free(tny_runner_msg *m);

int tny_runner_client_turn(tny_runner_client *c, const char *prompt, const char **images,
                           bool continue_recovery);
int tny_runner_client_steer(tny_runner_client *c, const char *text);
int tny_runner_client_cancel(tny_runner_client *c, bool hard);
int tny_runner_client_perm(tny_runner_client *c, const char *perm_id, tny_perm_decision d);
int tny_runner_client_ask_user_reply(tny_runner_client *c, const char *id, const char *answer);
/* Graceful shutdown: the runner ends the session and exits. */
int tny_runner_client_end(tny_runner_client *c, const char *reason);
/* Plain close = detach: an active turn keeps running (docs/adr/0053). */
void tny_runner_client_close(tny_runner_client *c);

#endif
