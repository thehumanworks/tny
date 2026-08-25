/* session.h — on-disk sessions (docs/features/sessions.md).
 * Native backend stores the full transcript in OpenAI message shape.
 * Host backends store only a resume pointer. Ephemeral sessions keep the
 * same working state in memory but never materialize conversation artifacts. */
#ifndef TNY_SESSION_H
#define TNY_SESSION_H

#include "core/config.h"

typedef struct {
    char *handle;
    char *data;
    size_t len;
} session_mem_result;

typedef struct {
    char *id;
    char *dir;             /* ~/.tny/sessions/<ws-hash>/<id> */
    tny_ctx *ctx;
    yyjson_mut_doc *doc;   /* working copy of session.json */
    /* Process-local extension lifecycle identity survives backend-engine
     * rebinds without changing the on-disk/provider message schema. */
    uint64_t extension_event_sequence;
    uint64_t extension_agent_sequence;
    bool extension_session_started;
    char *extension_start_reason;
    char *extension_previous_session_id;
    session_mem_result *mem_results; /* large results in ephemeral mode */
    int n_mem_results;
} tny_session_state;

/* Create a fresh session for this workspace (not yet saved). */
tny_session_state *session_new(tny_ctx *ctx);
/* Open by id or "last". NULL if not found / corrupt / ephemeral. */
tny_session_state *session_open(tny_ctx *ctx, const char *id_or_last);
int  session_save(tny_session_state *s);
void session_close(tny_session_state *s);

/* Transcript (OpenAI shape). */
void session_add_text(tny_session_state *s, const char *role, const char *content);
/* assistant msg with tool_calls; tc_json is the serialized array or NULL */
void session_add_assistant(tny_session_state *s, const char *content, const char *tc_json);
void session_add_tool_result(tny_session_state *s, const char *tool_call_id, const char *content);
/* Borrowed array of messages in the working doc. */
yyjson_mut_val *session_messages(tny_session_state *s);
int  session_turns(tny_session_state *s);
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
                                 const char *submitted, const char *effective,
                                 bool blocked, const char *extension,
                                 const char *reason);

/* Large tool results: store blob, return malloc'd handle id. Ephemeral
 * sessions retain the blob only until session_close(). */
char *session_store_result(tny_session_state *s, const char *data, size_t len);
/* Read a byte range from a stored blob; malloc'd or NULL. */
char *session_read_result(tny_session_state *s, const char *handle, size_t off,
                          size_t maxlen, size_t *out_len);

/* Compaction: after 8 completed turns keep latest 4 verbatim; force=true
 * condenses everything before the latest turn. Summary is mechanical
 * (requests, files, commands, outcomes). */
int  session_compact(tny_session_state *s, bool force); /* 1 changed, 0 no-op */
bool session_compact_needed(tny_session_state *s, bool force);
int  session_message_count(tny_session_state *s);
/* Index of first message the model should see verbatim + summary text. */
int  session_compact_boundary(tny_session_state *s, const char **summary);

/* Recovery checkpoint. */
void  session_recovery_write(tny_session_state *s, const char *partial);
char *session_recovery_read(tny_session_state *s);
void  session_recovery_clear(tny_session_state *s);

/* Listing. */
typedef struct {
    char *id, *title, *updated, *backend, *model, *workspace;
    int turns;
} session_meta;

session_meta *session_list(tny_ctx *ctx, bool all, int limit, const char *cursor,
                           int *count);
void session_meta_free(session_meta *m, int count);
char *session_latest_id(tny_ctx *ctx); /* malloc'd or NULL */

/* Copy a corrupt-but-recoverable session to a new id; returns new id. */
char *session_recover_copy(tny_ctx *ctx, const char *id);

#endif
