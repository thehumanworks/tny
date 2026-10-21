#ifndef TNY_RETRIEVAL_H
#define TNY_RETRIEVAL_H
#include "core/tools.h"

#define TNY_RETRIEVAL_INPUT_MAX (4u * 1024u * 1024u)

/* Private subagent transport; never a second inference or source loader. */
char *tny_retrieval_prepare(tools_env *env, yyjson_val *args, tny_session_state *child,
                            yyjson_doc **selection, char **error, bool *applies);
int tny_retrieval_input(tny_ctx *ctx, buf_t *prompt, bool resuming, yyjson_doc **input);
int tny_retrieval_import(tny_session_state *session, yyjson_doc *input, buf_t *prompt);
int tny_retrieval_reconcile(tny_session_state *session, char *error, size_t error_size);
void tny_retrieval_collect(const tny_ctx *ctx, buf_t *out);
char *tny_retrieval_result(tools_env *env, const char *session_id, const char *output,
                           bool *applies);
#endif
