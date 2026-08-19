/* session.h — on-disk sessions (docs/features/sessions.md).
 * Native backend stores the full transcript in OpenAI message shape.
 * Host backends store only a resume pointer. */
#ifndef TNY_SESSION_H
#define TNY_SESSION_H

#include "core/config.h"

typedef struct {
    char *id;
    char *dir;             /* ~/.tny/sessions/<ws-hash>/<id> */
    tny_ctx *ctx;
    yyjson_mut_doc *doc;   /* working copy of session.json */
} tny_session;

/* Create a fresh session for this workspace (not yet saved). */
tny_session *session_new(tny_ctx *ctx);
/* Open by id or "last". NULL if not found / corrupt. */
tny_session *session_open(tny_ctx *ctx, const char *id_or_last);
int  session_save(tny_session *s);
void session_close(tny_session *s);

/* Transcript (OpenAI shape). */
void session_add_text(tny_session *s, const char *role, const char *content);
/* assistant msg with tool_calls; tc_json is the serialized array or NULL */
void session_add_assistant(tny_session *s, const char *content, const char *tc_json);
void session_add_tool_result(tny_session *s, const char *tool_call_id, const char *content);
/* Borrowed array of messages in the working doc. */
yyjson_mut_val *session_messages(tny_session *s);
int  session_turns(tny_session *s);
void session_bump_turns(tny_session *s);
const char *session_title(tny_session *s);
void session_set_title(tny_session *s, const char *title);
void session_set_meta(tny_session *s, const char *backend, const char *model);
void session_set_host_pointer(tny_session *s, const char *ptr);
const char *session_host_pointer(tny_session *s);
void session_add_usage(tny_session *s, int64_t in_tok, int64_t out_tok);
void session_get_usage(tny_session *s, int64_t *in_tok, int64_t *out_tok);

/* Large tool results: store blob, return malloc'd handle id. */
char *session_store_result(tny_session *s, const char *data, size_t len);
/* Read a byte range from a stored blob; malloc'd or NULL. */
char *session_read_result(tny_session *s, const char *handle, size_t off,
                          size_t maxlen, size_t *out_len);

/* Compaction: after 8 completed turns keep latest 4 verbatim; force=true
 * condenses everything before the latest turn. Summary is mechanical
 * (requests, files, commands, outcomes). */
void session_compact(tny_session *s, bool force);
/* Index of first message the model should see verbatim + summary text. */
int  session_compact_boundary(tny_session *s, const char **summary);

/* Recovery checkpoint. */
void  session_recovery_write(tny_session *s, const char *partial);
char *session_recovery_read(tny_session *s);
void  session_recovery_clear(tny_session *s);

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
