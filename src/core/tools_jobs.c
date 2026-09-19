/* tools_jobs.c — see tools_jobs.h. Three typed tools map onto the eight job
 * operations; each operation keeps its own exact permission identity, so a
 * grant to read status can never start, stop or delete work. */
#include "core/tools_jobs.h"
#include "core/team_runtime.h"
#include "util/util.h"

#include <stdio.h>
#include <string.h>

bool tool_jobs_is_tool(const char *name) {
    return name && (strcmp(name, "job_submit") == 0 || strcmp(name, "job_control") == 0 ||
                    strcmp(name, "job_status") == 0);
}

tny_jobs_op tool_jobs_op(const char *name, yyjson_val *args) {
    if (!tool_jobs_is_tool(name)) return TNY_JOBS_OP_NONE;
    if (strcmp(name, "job_submit") == 0) return TNY_JOBS_OP_SUBMIT;
    const char *action = jget_str(args, "action");
    if (!action) return TNY_JOBS_OP_NONE;
    tny_jobs_op op = tny_jobs_op_parse(action);
    if (op == TNY_JOBS_OP_NONE || op == TNY_JOBS_OP_SUBMIT) return TNY_JOBS_OP_NONE;
    bool sensitive = tny_jobs_op_is_sensitive(op);
    /* job_control owns the state-changing actions; job_status is read-only. */
    return sensitive == (strcmp(name, "job_control") == 0) ? op : TNY_JOBS_OP_NONE;
}

bool tool_jobs_available(const tny_ctx *ctx, const char *name) {
    if (!ctx || !tool_jobs_is_tool(name)) return false;
    /* A job's children run here, not on the --ssh host, and an embedded
     * runtime must never spawn one. */
    if (ctx->library_mode || ctx->ssh_host) return false;
    if (strcmp(name, "job_status") == 0) return true; /* bounded record reads */
    return tny_jobs_execution_supported();
}

int tool_jobs_run(tools_env *env, tny_jobs_op op, yyjson_val *args, buf_t *out, char *err,
                  size_t errlen) {
    if (!env || !env->ctx || op == TNY_JOBS_OP_NONE) {
        snprintf(err, errlen, "unsupported jobs action");
        return 1;
    }
    const char *parent = env->session ? env->session->id : env->session_id;
    int rc = tny_jobs_run_context(env->ctx, op, args, out, err, errlen, env->cancelled,
                                  env->cancelled_ud, parent);
    if (op == TNY_JOBS_OP_SUBMIT && jget_bool(args, "dag", false) && env->session && out->data) {
        yyjson_doc *doc = jparse(out->data, out->len);
        const char *id = doc ? jget_str(yyjson_doc_get_root(doc), "id") : NULL;
        if (id && tny_team_register_run(env, id) != 0) {
            snprintf(err, errlen,
                     "run %s was submitted, but notifications could not be saved; "
                     "use jobs status/wait, do not resubmit",
                     id);
            rc = 2;
        }
        yyjson_doc_free(doc);
    }
    return rc;
}
