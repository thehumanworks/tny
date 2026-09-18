#include "core/tools_workspace.h"
#include "core/jobs.h"
#include "util/jobs_host.h"
#include "util/git.h"
#include "util/task_workspace.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool same(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }
/* Embedded NULs must not create a second meaning for a durable identity/path. */
static const char *text(yyjson_val *obj, const char *key) {
    yyjson_val *v = jget(obj, key);
    const char *s = yyjson_get_str(v);
    return s && strlen(s) == yyjson_get_len(v) ? s : NULL;
}
tny_workspace_op tny_workspace_op_parse(const char *name) {
    if (same(name, "inspect")) return TNY_WORKSPACE_INSPECT;
    if (same(name, "integrate")) return TNY_WORKSPACE_INTEGRATE;
    if (same(name, "cleanup")) return TNY_WORKSPACE_CLEANUP;
    return TNY_WORKSPACE_NONE;
}
const char *tny_workspace_permission_tool(tny_workspace_op op) {
    switch (op) {
    case TNY_WORKSPACE_INSPECT: return "job_workspace_inspect";
    case TNY_WORKSPACE_INTEGRATE: return "job_workspace_integrate";
    case TNY_WORKSPACE_CLEANUP: return "job_workspace_cleanup";
    case TNY_WORKSPACE_NONE: break;
    }
    return NULL;
}
tny_workspace_op tool_workspace_op(const char *name) {
    for (int i = TNY_WORKSPACE_INSPECT; i < TNY_WORKSPACE_NONE; ++i)
        if (same(name, tny_workspace_permission_tool((tny_workspace_op)i)))
            return (tny_workspace_op)i;
    return TNY_WORKSPACE_NONE;
}
bool tool_workspace_available(const tny_ctx *ctx, const char *name) {
    return ctx && !ctx->library_mode && !ctx->ssh_host && tny_jobs_execution_supported() &&
           tool_workspace_op(name) != TNY_WORKSPACE_NONE;
}
static bool request_id(yyjson_val *args, task_workspace_id *id) {
    if (!yyjson_is_obj(args) || yyjson_obj_size(args) != 3) return false;
    size_t i, n;
    yyjson_val *key, *value;
    yyjson_obj_foreach(args, i, n, key, value) {
        const char *k = yyjson_get_str(key);
        if (!k || strlen(k) != yyjson_get_len(key) ||
            (!same(k, "run") && !same(k, "task") && !same(k, "attempt")))
            return false;
    }
    int64_t task = jget_int(args, "task", -1), attempt = jget_int(args, "attempt", -1);
    if (!yyjson_is_int(jget(args, "task")) || !yyjson_is_int(jget(args, "attempt")) || task < 0 ||
        task >= TNY_JOBS_MAX_ITEMS || attempt <= 0 || attempt > INT_MAX)
        return false;
    *id = (task_workspace_id){text(args, "run"), (int)task, (int)attempt};
    return task_workspace_id_valid(*id);
}
static bool number(const char *s, int minimum, int maximum, int *out) {
    if (!s || !*s) return false;
    for (const char *p = s; *p; ++p)
        if (*p < '0' || *p > '9') return false;
    errno = 0;
    char *end;
    long v = strtol(s, &end, 10);
    if (errno || *end || v < minimum || v > maximum) return false;
    *out = (int)v;
    return true;
}
tny_workspace_op tny_workspace_parse_argv(int argc, char **argv, char **request_out, bool *json_out,
                                          const char **error) {
    if (request_out) *request_out = NULL;
    if (json_out) *json_out = false;
    if (error) *error = "expected inspect|integrate|cleanup --run ID --task N --attempt N [--json]";
    tny_workspace_op op = argc > 0 ? tny_workspace_op_parse(argv[0]) : TNY_WORKSPACE_NONE;
    if (!request_out || op == TNY_WORKSPACE_NONE) return TNY_WORKSPACE_NONE;
    task_workspace_id id = {NULL, -1, 0};
    bool json = false;
    for (int i = 1; i < argc; ++i) {
        if (same(argv[i], "--json") && !json) {
            json = true;
            continue;
        }
        if (i + 1 >= argc) return TNY_WORKSPACE_NONE;
        if (same(argv[i], "--run") && !id.run) id.run = argv[++i];
        else if (same(argv[i], "--task") && id.task < 0) {
            if (!number(argv[++i], 0, TNY_JOBS_MAX_ITEMS - 1, &id.task)) return TNY_WORKSPACE_NONE;
        } else if (same(argv[i], "--attempt") && !id.attempt) {
            if (!number(argv[++i], 1, INT_MAX, &id.attempt)) return TNY_WORKSPACE_NONE;
        } else return TNY_WORKSPACE_NONE;
    }
    if (!task_workspace_id_valid(id)) return TNY_WORKSPACE_NONE;
    buf_t b = {0};
    buf_appendf(&b, "{\"run\":\"%s\",\"task\":%d,\"attempt\":%d}", id.run, id.task, id.attempt);
    *request_out = buf_detach(&b);
    buf_free(&b);
    if (!*request_out) return TNY_WORKSPACE_NONE;
    if (json_out) *json_out = json;
    if (error) *error = NULL;
    return op;
}
char *tny_workspace_detail(const tny_ctx *ctx, tny_workspace_op op, yyjson_val *args,
                           const char **error) {
    if (error) *error = "workspace control requires local native, non-embedded execution";
    if (!tool_workspace_available(ctx, tny_workspace_permission_tool(op))) return NULL;
    task_workspace_id id;
    if (error) *error = "expected only valid run, task and attempt fields";
    if (!request_id(args, &id)) return NULL;
    buf_t b = {0};
    buf_appendf(&b,
                "{\"permission\":\"%s\",\"run\":\"%s\",\"task\":%d,\"attempt\":%d,\"caller_cwd\":",
                tny_workspace_permission_tool(op), id.run, id.task, id.attempt);
    jescape(&b, ctx->cwd ? ctx->cwd : "");
    buf_appends(&b, "}");
    char *s = buf_detach(&b);
    buf_free(&b);
    if (error) *error = s ? NULL : "cannot allocate permission detail";
    return s;
}
static bool private_path(const char *p, bool directory) {
    struct stat st;
    return p && lstat(p, &st) == 0 && st.st_uid == geteuid() && !(st.st_mode & 0077) &&
           (directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode) && st.st_nlink == 1);
}
/* Never open caller-selected record files, symlink records or unbounded data. */
static yyjson_doc *record_read(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    buf_t b = {0};
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
              !(st.st_mode & 0077) && st.st_nlink == 1 && st.st_size > 0 &&
              st.st_size <= TNY_JOBS_PAYLOAD_MAX;
    while (ok && b.len < (size_t)st.st_size) {
        char chunk[4096];
        size_t remaining = (size_t)st.st_size - b.len;
        ssize_t n = read(fd, chunk, remaining < sizeof chunk ? remaining : sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ok = false;
            break;
        }
        buf_append(&b, chunk, (size_t)n);
        if (b.oom) ok = false;
    }
    close(fd);
    yyjson_doc *d = ok ? jparse(b.data, b.len) : NULL;
    buf_free(&b);
    return d;
}
static int64_t integer(yyjson_val *obj, const char *key, int64_t fallback) {
    yyjson_val *v = jget(obj, key);
    return yyjson_is_int(v) ? yyjson_get_sint(v) : fallback;
}
static bool terminal(const char *state) {
    return same(state, "succeeded") || same(state, "failed") || same(state, "cancelled") ||
           same(state, "interrupted");
}
static bool record_valid(yyjson_val *root, task_workspace_id id, bool projection) {
    if (!same(text(root, "kind"), "job") || !same(text(root, "id"), id.run) ||
        !same(text(root, "run_id"), id.run) || !same(text(root, "job_kind"), "ask") ||
        integer(root, projection ? "schema_version" : "version", -1) != TNY_JOBS_SCHEMA_VERSION ||
        !jget_bool(root, "dag", false) || integer(root, "attempt", -1) != id.attempt ||
        integer(root, "revision", -1) < 1 || !terminal(text(root, "state")) ||
        !same(text(root, "cleanup"), "complete") || !yyjson_is_false(jget(root, "cleanup_hold")))
        return false;
    yyjson_val *items = jget(root, "items");
    if (!yyjson_is_arr(items) || yyjson_arr_size(items) > TNY_JOBS_MAX_ITEMS ||
        (size_t)id.task >= yyjson_arr_size(items))
        return false;
    size_t i, n;
    yyjson_val *item;
    yyjson_arr_foreach(items, i, n, item) {
        if (integer(item, "index", -1) != (int64_t)i || !terminal(text(item, "state")))
            return false;
    }
    item = yyjson_arr_get(items, (size_t)id.task);
    return integer(item, "task_id", -1) == id.task && integer(item, "attempt", -1) == id.attempt;
}
static void field(buf_t *b, const char *key, const char *value) {
    buf_appendf(b, ",\"%s\":", key);
    jescape(b, value ? value : "");
}
static void result(buf_t *out, tny_workspace_op op, task_workspace_id id, const char *status,
                   const char *launch, const task_workspace_result *r) {
    buf_appends(out,
                "{\"kind\":\"task_workspace\",\"verification\":\"unverified\",\"accepted\":false");
    field(out, "operation", tny_workspace_permission_tool(op));
    field(out, "run", id.run);
    buf_appendf(out, ",\"task\":%d,\"attempt\":%d", id.task, id.attempt);
    field(out, "status", status);
    if (launch) field(out, "launch_cwd", launch);
    if (r) {
        field(out, "path", r->path);
        field(out, "branch", r->branch);
        field(out, "base", r->base);
        field(out, "origin", r->origin);
        field(out, "origin_branch", r->origin_branch);
        field(out, "revision", r->revision);
        field(out, "patch", r->patch);
        field(out, "worktree_status", r->status);
        buf_appendf(out, ",\"dirty\":%s", r->dirty ? "true" : "false");
    }
    buf_appendf(out, ",\"conflict\":%s}\n", same(status, "conflict") ? "true" : "false");
}
int tny_workspace_run(tny_ctx *ctx, tny_workspace_op op, yyjson_val *args, buf_t *out, char *err,
                      size_t cap) {
    if (err && cap) err[0] = 0;
    const char *why = NULL;
    char *detail = tny_workspace_detail(ctx, op, args, &why);
    if (!detail) {
        if (err && cap) snprintf(err, cap, "%s", why);
        return 1;
    }
    free(detail);
    task_workspace_id id;
    if (!request_id(args, &id) || !out) return 1;
    size_t output_start = out->len;
    int rc = 1, owner = -1;
    task_workspace *workspace = NULL;
    task_workspace_result snapshot = {0};
    yyjson_doc *raw = NULL, *projected = NULL;
    buf_t request = {0}, service = {0};
    char *state = ctx->tny_dir ? realpath(ctx->tny_dir, NULL) : NULL;
    char *jobs = state ? path_join(state, "jobs") : NULL;
    char *dir = jobs ? path_join(jobs, id.run) : NULL;
    char *lock = dir ? path_join(dir, "owner.lock") : NULL;
    char *record = dir ? path_join(dir, "job.json") : NULL;
    char *launch = NULL, *caller = NULL;
    why = "no confined private job record or existing owner lock";
    if (!private_path(jobs, true) || !private_path(dir, true) || !private_path(lock, false) ||
        !private_path(record, false))
        goto done;
    owner = open(lock, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    why = "job is owned or ownership is unknown; workspace control refused";
    if (owner < 0 || tny_jobs_host_lock_try(owner) != TNY_JOBS_LOCK_ACQUIRED ||
        !tny_jobs_host_fd_is_file(owner, lock))
        goto done;
    why = "job record is unreadable, malformed or over the private record limit";
    raw = record_read(record);
    if (!raw) goto done;
    /* The owner lock fences retry/rm. No state lock survives this read or Git.
     * Use the service's supported projection, plus only confined raw policy. */
    buf_appendf(&request, "{\"id\":\"%s\"}", id.run);
    yyjson_doc *req = !request.oom ? jparse(request.data, request.len) : NULL;
    if (!req) goto done;
    char ignored[256] = "";
    int service_rc = tny_jobs_run(ctx, TNY_JOBS_OP_STATUS, yyjson_doc_get_root(req), &service,
                                  ignored, sizeof ignored);
    yyjson_doc_free(req);
    why = "job service could not validate the record";
    if ((service_rc != 0 && service_rc != 2) || service.oom) goto done;
    projected = jparse(service.data, service.len);
    yyjson_doc_free(raw);
    raw = record_read(record); /* revalidate after owner acquisition and service read */
    yyjson_val *root = raw ? yyjson_doc_get_root(raw) : NULL;
    yyjson_val *view = projected ? yyjson_doc_get_root(projected) : NULL;
    why = "need a terminal DAG ask job, matching task/attempt and complete cleanup without a hold";
    if (!record_valid(root, id, false) || !record_valid(view, id, true) ||
        jget_int(root, "revision", -1) != jget_int(view, "revision", -1) ||
        !same(text(root, "state"), text(view, "state")) ||
        !same(text(root, "workspace"), text(view, "workspace")))
        goto done;
    yyjson_val *item = yyjson_arr_get(jget(root, "items"), (size_t)id.task);
    yyjson_val *policy = jget(item, "workspace");
    if (!policy) policy = jget(jget(item, "request"), "workspace");
    why = "job item has no authoritative isolated workspace policy";
    if (!same(text(policy, "policy"), "isolated")) goto done;
    const char *launch_path = text(root, "workspace");
    why = "job launch cwd is unavailable or not local";
    if (!launch_path || launch_path[0] != '/' || strstr(launch_path, "://")) goto done;
    launch = realpath(launch_path, NULL);
    if (!launch) goto done;
    if (op != TNY_WORKSPACE_INSPECT) {
        caller = ctx->cwd ? realpath(ctx->cwd, NULL) : NULL;
        why = "workspace mutation requires the recorded launch cwd";
        if (!same(caller, launch)) goto done;
    }
    why = "job ownership changed; workspace control refused";
    if (!tny_jobs_host_fd_is_file(owner, lock) || !private_path(dir, true)) goto done;
    char git_error[512] = "";
    why = "owned task workspace is unavailable; no path was adopted";
    if (task_workspace_open(launch, id, &workspace, git_error, sizeof git_error)) goto done;
    struct stat tree_stat;
    bool already_removed = op == TNY_WORKSPACE_CLEANUP &&
                           lstat(task_workspace_path(workspace), &tree_stat) < 0 && errno == ENOENT;
    if (!already_removed) {
        why = "cannot inspect workspace; edits preserved (Git error or artifact limit)";
        if (task_workspace_inspect(workspace, &snapshot, git_error, sizeof git_error)) goto done;
        /* A different linked checkout in the same common repository must not
         * redirect a job to the helper's original integration destination.
         * Accept launch subdirectories, but require the same Git top-level. */
        buf_t top = {0};
        int code = git_run(
            launch,
            (const char *const[]){"rev-parse", "--path-format=absolute", "--show-toplevel", NULL},
            &top);
        bool matches = code == 0 && same(top.data, snapshot.origin);
        buf_free(&top);
        why = "job launch checkout does not match managed workspace provenance";
        if (!matches) goto done;
    }
    if (op == TNY_WORKSPACE_CLEANUP) {
        int code = task_workspace_cleanup(workspace, git_error, sizeof git_error);
        rc = code ? 2 : 0;
        why = code ? "cleanup refused; workspace preserved" : NULL;
        result(out, op, id, code ? "refused" : "removed", launch, NULL);
    } else {
        if (op == TNY_WORKSPACE_INSPECT) {
            result(out, op, id, "inspected", launch, &snapshot);
            rc = 0;
            why = NULL;
        } else {
            int code = task_workspace_integrate(workspace, git_error, sizeof git_error);
            rc = code ? 2 : 0;
            why = code == 1 ? "merge conflict preserved; verification remains unverified"
                  : code    ? "integration refused/failed; inspect launch tree; no automatic reset"
                            : NULL;
            result(out, op, id,
                   code == 1 ? "conflict"
                   : code    ? "refused"
                             : "integrated",
                   launch, &snapshot);
        }
    }
done:
    if (out->len == output_start) result(out, op, id, "refused", NULL, NULL);
    if (out->oom) {
        rc = 1;
        why = "cannot allocate workspace result; inspect state before retry";
    }
    if (why && err && cap) snprintf(err, cap, "%s", why);
    task_workspace_result_free(&snapshot);
    task_workspace_close(workspace);
    if (raw) yyjson_doc_free(raw);
    if (projected) yyjson_doc_free(projected);
    buf_free(&request);
    buf_free(&service);
    if (owner >= 0) tny_jobs_host_lock_close(owner);
    free(state);
    free(jobs);
    free(dir);
    free(lock);
    free(record);
    free(launch);
    free(caller);
    return rc;
}
int tool_workspace_run(tools_env *env, tny_workspace_op op, yyjson_val *args, buf_t *out, char *err,
                       size_t cap) {
    if (!env || !env->ctx || (env->cancelled && env->cancelled(env->cancelled_ud))) {
        if (err && cap) snprintf(err, cap, "workspace control unavailable or cancelled");
        return 1;
    }
    return tny_workspace_run(env->ctx, op, args, out, err, cap);
}
void tny_workspace_render_human(const char *json, buf_t *out) {
    yyjson_doc *d = json ? jparse(json, strlen(json)) : NULL;
    yyjson_val *r = d ? yyjson_doc_get_root(d) : NULL;
    const char *status = text(r, "status");
    if (status) buf_appendf(out, "workspace %s: verification unverified; not accepted\n", status);
    if (d) yyjson_doc_free(d);
}
