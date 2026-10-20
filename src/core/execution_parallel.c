/* Fresh-exec workers isolate allocator, transport and result-store state. No
 * worker can call owner hooks, prompt, or commit a whole session snapshot. */
#include "core/execution_parallel.h"
#include "core/checkpoint.h"
#include "core/execution_protocol.h"
#include "core/execution_state.h"
#include "util/util.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool tny_execution_parallel_safe(const tools_env *env, const tools_call *call) {
    if (!env || !env->ctx || env->ctx->ssh_host || env->ctx->custom_tools ||
        env->ctx->host_services || !call || !call->name || call->custom || call->intercept)
        return false;
    const char *names[] = {"read_file", "list_files",       "glob_files", "grep_files",
                           "file_info", "read_tool_result", "web_fetch"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(call->name, names[i]) == 0) return true;
    return false;
}

int tny_execution_parallel_start(tny_exec_host *host, tools_env *env, const tools_call *call,
                                 int64_t deadline) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *r = d ? yyjson_mut_obj(d) : NULL;
    if (d) yyjson_mut_doc_set_root(d, r);
    bool ok =
        r && tny_execution_parallel_safe(env, call) &&
        yyjson_mut_obj_add_val(d, r, "context", tny_checkpoint_context(d, env->ctx)) &&
        yyjson_mut_obj_add_val(d, r, "state", tny_execution_state_encode(d, env, NULL, true)) &&
        yyjson_mut_obj_add_strcpy(d, r, "name", call->name) &&
        yyjson_mut_obj_add_val(d, r, "args", yyjson_val_mut_copy(d, call->args)) &&
        yyjson_mut_obj_add_sint(d, r, "deadline", deadline);
    char *request = ok ? yyjson_mut_write(d, 0, NULL) : NULL;
    yyjson_mut_doc_free(d);
    int rc = !request                               ? ENOMEM
             : strlen(request) > TNY_EXEC_FRAME_MAX ? E2BIG
                                                    : tny_exec_host_start_tool(host);
    if (!rc)
        rc = tny_exec_host_send(host->fd, request, deadline, env->cancelled, env->cancelled_ud);
    secure_free(request);
    if (rc) (void)tny_exec_host_kill(host);
    return rc;
}

char *tny_execution_parallel_collect(tny_exec_host *host, tools_env *env, int64_t deadline,
                                     tny_exec_cancel_fn cancel, void *ud) {
    char *line = tny_exec_host_receive(host->fd, deadline, cancel, ud);
    yyjson_doc *d = line ? jparse(line, strlen(line)) : NULL;
    secure_free(line);
    yyjson_val *r = d ? yyjson_doc_get_root(d) : NULL;
    const char *output = jget_str(r, "output");
    bool ok = output && tny_exec_json_valid(r) && yyjson_obj_size(r) == 2 &&
              !tny_exec_host_expect_eof(host->fd, deadline, cancel, ud);
    if (!ok) (void)tny_exec_host_kill(host);
    if (tny_exec_host_close(host, ok)) ok = false;
    /* Only the read worker's delta (including ephemeral spill handles) is
     * merged. Existing unrelated owner appends are retained by this seam. */
    if (ok) ok = tny_execution_state_apply(env, jget(r, "state"), false);
    char *result =
        ok ? xstrdup(output) : tool_err("parallel read failed or interrupted; not replayed");
    yyjson_doc_free(d);
    return result;
}

int tny_execution_tool_main(void) {
    int fd = tny_exec_host_accept();
    if (fd < 0) return 2;
    char *line = tny_exec_host_receive(fd, monotonic_ms() + 10000, NULL, NULL);
    yyjson_doc *d = line ? jparse(line, strlen(line)) : NULL;
    secure_free(line);
    yyjson_val *r = d ? yyjson_doc_get_root(d) : NULL;
    tools_env env = {0};
    tools_call call = {0};
    yyjson_doc *baseline = NULL;
    char *output = NULL, *reply = NULL;
    int rc = 2;
    int64_t deadline = jget_int(r, "deadline", 0);
    const char *name = jget_str(r, "name");
    if (!r || !tny_exec_json_valid(r) || yyjson_obj_size(r) != 5 || !name ||
        deadline <= monotonic_ms())
        goto done;
    env.ctx = tny_checkpoint_context_restore(jget(r, "context"));
    env.perm = env.ctx ? perm_new(env.ctx) : NULL;
    if (!env.perm || !tny_execution_state_apply(&env, jget(r, "state"), true)) goto done;
    baseline = env.session ? yyjson_mut_doc_imut_copy(env.session->doc, NULL) : NULL;
    if (env.session && !baseline) goto done;
    char *args = yyjson_val_write(jget(r, "args"), 0, NULL);
    int prepared = args ? tools_call_prepare(&env, name, args, &call) : -1;
    free(args);
    if (prepared || !tny_execution_parallel_safe(&env, &call) || call.verdict == PERM_DENY)
        goto done;
    /* PROMPT was already resolved by the owning server. No worker prompt or
     * extension callback exists; immutable prepared identity is revalidated. */
    output = tools_call_execute(&env, &call);
    yyjson_mut_doc *out = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = out ? yyjson_mut_obj(out) : NULL;
    if (out) yyjson_mut_doc_set_root(out, root);
    bool ok = output && root && yyjson_mut_obj_add_strcpy(out, root, "output", output) &&
              yyjson_mut_obj_add_val(
                  out, root, "state",
                  tny_execution_state_encode(
                      out, &env, baseline ? yyjson_doc_get_root(baseline) : NULL, false));
    reply = ok ? yyjson_mut_write(out, 0, NULL) : NULL;
    yyjson_mut_doc_free(out);
    rc = reply ? tny_exec_host_send(fd, reply, deadline, NULL, NULL) : -1;
done:
    secure_free(reply);
    free(output);
    tools_call_free(&call);
    yyjson_doc_free(baseline);
    if (env.session) session_close(env.session);
    perm_free(env.perm);
    tny_ctx_free(env.ctx);
    yyjson_doc_free(d);
    close(fd);
    return rc ? 2 : 0;
}
