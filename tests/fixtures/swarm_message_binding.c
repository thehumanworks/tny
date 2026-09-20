/* Test-only allow-once boundary over real preparation and locked mailbox send. */
#include "core/tools.h"
#include "core/team_runtime.h"
#include "util/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 6) return 2;
    const char *cwd = argv[1], *session_id = argv[2], *job_path = argv[3];
    const char *mutation = argv[4], *request = argv[5];
    tny_ctx *ctx = tny_ctx_load(cwd);
    if (!ctx) return 3;
    ctx->perm_mode = TNY_MODE_ASK;
    tny_session_state *session = session_open(ctx, session_id);
    perm_engine *perm = perm_new(ctx);
    if (!session || !perm) return 4;
    tools_env env = {.ctx = ctx, .session = session, .session_id = session_id, .perm = perm};
    tools_call call = {0};
    if (tools_call_prepare(&env, "swarm_message", request, &call) != 0 ||
        call.verdict != PERM_PROMPT) {
        fprintf(stderr, "prepare failed: %s\n",
                call.error ? call.error : "expected permission prompt");
        return 5;
    }
    yyjson_doc *original = jparse_file(job_path);
    yyjson_mut_doc *record = original ? yyjson_doc_mut_copy(original, jallocator()) : NULL;
    if (!record) return 6;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(record);
    yyjson_mut_val *items = yyjson_mut_obj_get(root, "items");
    yyjson_mut_val *recipient = yyjson_mut_arr_get(items, 1);
    if (strcmp(mutation, "recipient_name") == 0)
        yyjson_mut_obj_put(recipient, yyjson_mut_strcpy(record, "swarm_name"),
                           yyjson_mut_strcpy(record, "renamed-beta"));
    else if (strcmp(mutation, "recipient_attempt") == 0)
        yyjson_mut_obj_put(recipient, yyjson_mut_strcpy(record, "attempt"),
                           yyjson_mut_int(record, 2));
    else if (strcmp(mutation, "sender_attempt") == 0)
        yyjson_mut_obj_put(root, yyjson_mut_strcpy(record, "attempt"), yyjson_mut_int(record, 2));
    else if (strcmp(mutation, "topology") == 0)
        yyjson_mut_obj_put(
            root, yyjson_mut_strcpy(record, "swarm_definition_sha256"),
            yyjson_mut_strcpy(record,
                              "0000000000000000000000000000000000000000000000000000000000000000"));
    else if (strcmp(mutation, "active_run") == 0) {
        yyjson_mut_val *meta =
            yyjson_mut_obj_get(yyjson_mut_doc_get_root(session->doc), "swarm_definition");
        yyjson_mut_obj_put(meta, yyjson_mut_strcpy(session->doc, "run_id"),
                           yyjson_mut_strcpy(session->doc, "ffffffffffffffffffffffffffffffff"));
    }
    char *changed = jwrite(record);
    if (!changed || file_write_atomic(job_path, changed, strlen(changed))) return 7;
    free(changed);
    yyjson_mut_doc_free(record);
    yyjson_doc_free(original);
    if (strcmp(mutation, "payload") == 0 || strcmp(mutation, "request_run") == 0) {
        yyjson_mut_doc *arguments = yyjson_doc_mut_copy(call.doc, jallocator());
        if (!arguments) return 9;
        yyjson_mut_val *args = yyjson_mut_doc_get_root(arguments);
        const char *key = strcmp(mutation, "payload") == 0 ? "text" : "run";
        const char *value = strcmp(mutation, "payload") == 0 ? "changed after approval"
                                                             : "ffffffffffffffffffffffffffffffff";
        yyjson_mut_obj_put(args, yyjson_mut_strcpy(arguments, key),
                           yyjson_mut_strcpy(arguments, value));
        yyjson_doc *replaced = yyjson_mut_doc_imut_copy(arguments, jallocator());
        yyjson_mut_doc_free(arguments);
        if (!replaced) return 10;
        yyjson_doc_free(call.doc);
        call.doc = replaced;
        call.args = yyjson_doc_get_root(replaced);
    }
    /* This models ALLOW_ONCE, not a reusable grant or a rule change. */
    call.verdict = PERM_ALLOW;
    char *result = tools_call_execute(&env, &call);
    printf("%s\n", result ? result : "NULL");
    int rc = result && (strcmp(mutation, "unchanged") == 0
                            ? !str_starts(result, "error:")
                            : (strstr(result, "MAILBOX_STALE") != NULL ||
                               (strcmp(mutation, "recipient_attempt") == 0 &&
                                strstr(result, "MAILBOX_CORRUPT"))))
                 ? 0
                 : 8;
    free(result);
    tools_call_release_storage(&call);
    session_close(session);
    perm_free(perm);
    tny_ctx_free(ctx);
    return rc;
}
