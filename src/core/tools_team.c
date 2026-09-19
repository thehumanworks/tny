#include "core/tools_team.h"
#include "core/team_runtime.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int inherited_number(const char *name, int max) {
    const char *p = getenv(name);
    if (!p || !*p) return -1;
    uint64_t n = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        n = n * 10 + (unsigned)(*p - '0');
        if (n > (uint64_t)max) return -1;
    }
    return (int)n;
}
void tool_team_caller(const tools_env *env, tny_team_caller *out) {
    memset(out, 0, sizeof *out);
    out->task_index = -1;
    if (!env) return;
    out->session_id = env->session ? env->session->id : env->session_id;
    const char *run = getenv("TNY_TEAM_RUN");
    if (tny_jobs_valid_id(run)) {
        out->run_id = run;
        out->task_index = inherited_number("TNY_TEAM_TASK", TNY_JOBS_MAX_ITEMS - 1);
        out->attempt = inherited_number("TNY_TEAM_ATTEMPT", INT_MAX);
        out->capability = getenv("TNY_TEAM_CAPABILITY");
    }
}
bool tool_team_available(const tny_ctx *ctx) {
    return ctx && !ctx->library_mode && !ctx->ssh_host && !ctx->no_save &&
           tny_jobs_execution_supported();
}
char *tool_team_detail(tools_env *env, tny_team_op op, yyjson_val *request, char **error) {
    tny_team_caller caller;
    tool_team_caller(env, &caller);
    return tny_team_detail(env->ctx, &caller, op, request, error);
}
int tool_team_run(tools_env *env, tny_team_op op, yyjson_val *request, buf_t *out, char *err,
                  size_t cap) {
    if (!env || !tool_team_available(env->ctx)) {
        snprintf(err, cap, "team control requires a saved native local runner");
        return 1;
    }
    tny_team_caller caller;
    tool_team_caller(env, &caller);
    int rc = tny_team_run(env->ctx, &caller, op, request, out, err, cap, env->cancelled,
                          env->cancelled_ud);
    if (op == TNY_TEAM_START && out->data && env->session) {
        yyjson_doc *doc = jparse(out->data, out->len);
        const char *id = doc ? jget_str(yyjson_doc_get_root(doc), "run_id") : NULL;
        if (id && tny_team_register_run(env, id) != 0) {
            snprintf(err, cap,
                     "run %s was submitted but notification persistence failed; "
                     "inspect that run, do not resubmit",
                     id);
            rc = 2;
        }
        yyjson_doc_free(doc);
    }
    return rc;
}
