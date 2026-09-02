/* session.h — on-disk sessions (docs/features/sessions.md).
 * Native backend stores the full transcript in OpenAI message shape.
 * Host backends store only a resume pointer. Ephemeral sessions keep the
 * same working state in memory but never materialize conversation artifacts. */
#ifndef TNY_SESSION_H
#define TNY_SESSION_H

#include <sys/types.h>
#include "core/config.h"

typedef struct {
    char *handle;
    char *data;
    size_t len;
} session_mem_result;

typedef struct {
    char *id;
    char *dir; /* ~/.tny/sessions/<ws-hash>/<id> */
    tny_ctx *ctx;
    yyjson_mut_doc *doc; /* working copy of session.json */
    /* Process-local extension lifecycle identity survives backend-engine
     * rebinds without changing the on-disk/provider message schema. */
    uint64_t extension_event_sequence;
    uint64_t extension_agent_sequence;
    bool extension_session_started;
    char *extension_start_reason;
    char *extension_previous_session_id;
    session_mem_result *mem_results; /* large results in ephemeral mode */
    int n_mem_results;
    int lock_fd; /* <dir>/lock flock fd, -1 when not held */
    /* Exact resolved task instructions are persisted in the private
     * <session>/task.md sidecar. Public session JSON carries metadata only. */
    char *task_body;
} tny_session_state;

/* Create a fresh session for this workspace (not yet saved). */
tny_session_state *session_new(tny_ctx *ctx);
/* Open by id or "last". NULL if not found / corrupt / ephemeral. */
tny_session_state *session_open(tny_ctx *ctx, const char *id_or_last);
int session_save(tny_session_state *s);
void session_close(tny_session_state *s);

/* Transcript (OpenAI shape). */
void session_add_text(tny_session_state *s, const char *role, const char *content);
/* assistant msg with tool_calls; tc_json is the serialized array or NULL */
void session_add_assistant(tny_session_state *s, const char *content, const char *tc_json);
void session_add_tool_result(tny_session_state *s, const char *tool_call_id, const char *content);
/* Borrowed array of messages in the working doc. */
yyjson_mut_val *session_messages(tny_session_state *s);
int session_turns(tny_session_state *s);
void session_bump_turns(tny_session_state *s);
const char *session_title(tny_session_state *s);
void session_set_title(tny_session_state *s, const char *title);
void session_set_meta(tny_session_state *s, const char *backend, const char *model);
const char *session_backend(tny_session_state *s); /* provider that owns the transcript */
void session_set_host_pointer(tny_session_state *s, const char *ptr);
const char *session_host_pointer(tny_session_state *s);
void session_add_usage(tny_session_state *s, int64_t in_tok, int64_t out_tok);
void session_get_usage(tny_session_state *s, int64_t *in_tok, int64_t *out_tok);

/* Extension lifecycle/audit metadata is top-level and never serialized into
 * provider-facing messages[]. Old sessions simply lack it. */
void session_set_extension_start(tny_session_state *s, const char *reason,
                                 const char *previous_session_id);
void session_record_prompt_audit(tny_session_state *s, const char *submission_id,
                                 const char *submitted, const char *effective, bool blocked,
                                 const char *extension, const char *reason);
void session_replace_tool_arguments(tny_session_state *s, const char *tool_call_id,
                                    const char *arguments_json);
void session_record_tool_audit(tny_session_state *s, const char *tool_call_id,
                               const char *tool_name, const char *original_arguments,
                               const char *effective_arguments, const char *control_extension,
                               const char *control_reason, const char *original_result,
                               bool original_ok, const char *effective_result, bool effective_ok,
                               const char *replacement_extension, const char *annotations_json);

/* Skill mention injection (docs/adr/0056). Top-level `skill_injections`,
 * never serialized into messages[]: records which skill bodies were sent
 * ahead of the user message at `message_index` and the text the user
 * actually typed, so transcripts show that text and a later mention of a
 * skill still in the verbatim window is not re-sent. */
void session_record_skill_injection(tny_session_state *s, int message_index, char **names,
                                    int n_names, const char *display);
/* true when `name` was injected at or after the compaction boundary */
bool session_skill_injected(tny_session_state *s, const char *name);
/* The typed text for a message index whose stored content carries injected
 * skill blocks; NULL when the stored content is what the user typed. */
const char *session_message_display(tny_session_state *s, int message_index);

/* Large tool results: store blob, return malloc'd handle id. Ephemeral
 * sessions retain the blob only until session_close(). */
char *session_store_result(tny_session_state *s, const char *data, size_t len);
/* Read a byte range from a stored blob; malloc'd or NULL. */
char *session_read_result(tny_session_state *s, const char *handle, size_t off, size_t maxlen,
                          size_t *out_len);

/* Compaction: after 8 completed turns keep latest 4 verbatim; force=true
 * condenses everything before the latest turn. Summary is mechanical
 * (requests, files, commands, outcomes). */
int session_compact(tny_session_state *s, bool force); /* 1 changed, 0 no-op */
bool session_compact_needed(tny_session_state *s, bool force);
int session_message_count(tny_session_state *s);
/* Index of first message the model should see verbatim + summary text. */
int session_compact_boundary(tny_session_state *s, const char **summary);

/* Runtime status for background tasks (docs/adr/0031). Top-level fields
 * `status` ("running"|"done"|"error"|"interrupted"), `exit_code` and
 * `result` (the exact object foreground `ask --json` would have printed).
 * Old sessions simply lack all of them. */
void session_set_status_running(tny_session_state *s);
/* result_json is a serialized JSON object or NULL; a value that fails to
 * parse is dropped rather than corrupting the doc. */
void session_set_status_finished(tny_session_state *s, const char *status, int exit_code,
                                 const char *result_json);
const char *session_status(tny_session_state *s); /* NULL if absent */

/* Bind/clear the current ctx task on a fresh session. Reconcile restores the
 * saved snapshot when no task was explicitly requested and rejects a
 * mismatch (name+digest) before provider work begins. */
int session_task_bind_current(tny_session_state *s);
void session_task_clear(tny_session_state *s);
int session_task_reconcile(tny_session_state *s, char *err, size_t errsz);

/* Writer lock: flock(LOCK_EX|LOCK_NB) on <dir>/lock, held for the duration
 * of a turn. The lock lives on the open file description, so a forked child
 * inherits it and the parent exiting does not release it. 0 ok (idempotent
 * when already held), -1 when another process holds it. */
int session_lock_acquire(tny_session_state *s);
void session_lock_release(tny_session_state *s); /* also runs in session_close */
/* Reader liveness probe (no session open): true while some process holds
 * the writer lock. Missing lock file means not running. */
bool session_is_running(tny_ctx *ctx, const char *id);

/* <dir>/pid is the control channel for `session stop` — liveness is always
 * the lock probe, never kill(pid,0). */
int session_write_pid(tny_session_state *s, pid_t pid);
pid_t session_read_pid(tny_ctx *ctx, const char *id); /* -1 absent/garbage */

/* Stop sequence (docs/adr/0031 decisions 6, 7a): group-SIGTERM the holder,
 * bounded wait for the flock to free (~5 s; TNY_STOP_TIMEOUT_MS overrides,
 * tests use it). force_kill escalates to group-SIGKILL and writes the
 * terminal status ("interrupted", 137) on the child's behalf — the only
 * non-child terminal write. Returns 0 stopped, 1 was not running (caller
 * no-ops), 2 timed out and still running (suggest --kill), -1 error with
 * err filled. */
int session_stop(tny_ctx *ctx, const char *id, bool force_kill, char *err, size_t errsz);

/* Recovery checkpoint. */
void session_recovery_write(tny_session_state *s, const char *partial);
char *session_recovery_read(tny_session_state *s);
void session_recovery_clear(tny_session_state *s);

/* Listing. */
typedef struct {
    char *id, *title, *updated, *backend, *model, *workspace;
    char *status; /* stored status field; NULL for pre-0031 sessions */
    char *task_name, *task_source, *task_digest; /* secret-safe task metadata */
    bool running;                                /* live writer-lock probe at list time */
    int turns;
} session_meta;

session_meta *session_list(tny_ctx *ctx, bool all, int limit, const char *cursor, int *count);
void session_meta_free(session_meta *m, int count);
char *session_latest_id(tny_ctx *ctx); /* malloc'd or NULL */

/* Copy a corrupt-but-recoverable session to a new id; returns new id. */
char *session_recover_copy(tny_ctx *ctx, const char *id);

#endif
