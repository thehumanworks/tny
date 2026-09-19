#include "util/task_workspace.h"
#include "util/git.h"
#include "json/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool task_workspace_id_valid(task_workspace_id id) {
    if (!id.run || strlen(id.run) != 32 || id.task < 0 || id.attempt <= 0) return false;
    for (size_t i = 0; i < 32; ++i)
        if (!((id.run[i] >= '0' && id.run[i] <= '9') || (id.run[i] >= 'a' && id.run[i] <= 'f')))
            return false;
    return true;
}

struct task_workspace {
    char *run;
    int task, attempt;
    char *common, *dir, *path, *branch, *base, *origin, *origin_branch, *gitdir;
    int lock;
    bool removed;
};

static int error(char *err, size_t cap, const char *text) {
    if (err && cap) snprintf(err, cap, "%s", text);
    return -1;
}

const char *task_workspace_path(const task_workspace *w) { return w ? w->path : NULL; }

void task_workspace_result_free(task_workspace_result *r) {
    if (!r) return;
    free(r->run);
    free(r->path);
    free(r->branch);
    free(r->base);
    free(r->origin);
    free(r->origin_branch);
    free(r->revision);
    free(r->patch);
    free(r->status);
    memset(r, 0, sizeof *r);
}

#ifndef __EMSCRIPTEN__
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#define GIT(cwd, out, ...) git_run(cwd, (const char *const[]){__VA_ARGS__, NULL}, out)

static char *query(const char *cwd, const char *arg) {
    buf_t b = {0};
    int rc = GIT(cwd, &b, "rev-parse", "--path-format=absolute", arg);
    char *s = rc == 0 && b.len && b.len < 16000 ? buf_detach(&b) : NULL;
    buf_free(&b);
    return s;
}
static bool same(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }
static bool private_path(const char *p, bool directory) {
    struct stat st;
    return lstat(p, &st) == 0 && st.st_uid == getuid() && !(st.st_mode & 0077) &&
           (directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode));
}
static bool absent(const char *p) {
    struct stat st;
    return lstat(p, &st) < 0 && errno == ENOENT;
}
static char *ref(const char *p) {
    buf_t b = {0};
    int rc = GIT(p, &b, "symbolic-ref", "--quiet", "HEAD");
    char *s = rc == 0 ? buf_detach(&b) : NULL;
    buf_free(&b);
    return s;
}
static bool clean(const char *p, bool ignored) {
    buf_t b = {0};
    int rc = ignored ? GIT(p, &b, "status", "--porcelain=v1", "--untracked-files=all", "--ignored")
                     : GIT(p, &b, "status", "--porcelain=v1", "--untracked-files=all");
    bool ok = rc == 0 && b.len == 0;
    buf_free(&b);
    return ok;
}
static void field(buf_t *b, const char *key, const char *value) {
    if (b->len > 1) buf_appends(b, ",");
    jescape(b, key);
    buf_appends(b, ":");
    jescape(b, value);
}
static int save(task_workspace *w) {
    buf_t b = {0};
    buf_appends(&b, "{");
    field(&b, "run", w->run);
    buf_appendf(&b, ",\"task\":%d,\"attempt\":%d", w->task, w->attempt);
    field(&b, "base", w->base);
    field(&b, "origin", w->origin);
    field(&b, "origin_branch", w->origin_branch);
    field(&b, "gitdir", w->gitdir);
    field(&b, "state", w->removed ? "removed" : "ready");
    buf_appends(&b, "}\n");
    char *p = path_join(w->dir, "owner.json");
    int rc = p && !b.oom ? file_write_atomic(p, b.data, b.len) : -1;
    free(p);
    buf_free(&b);
    return rc;
}
/* Metadata never supplies the deletion path or branch. Both are derived from
 * the validated identity in a private repository-local namespace. */
static task_workspace *allocate(const char *cwd, task_workspace_id id, bool create) {
    if (!cwd || strstr(cwd, "://") || !task_workspace_id_valid(id)) return NULL;
    task_workspace *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->lock = -1;
    w->run = xstrdup(id.run);
    w->task = id.task;
    w->attempt = id.attempt;
    w->common = query(cwd, "--git-common-dir");
    char *top = query(cwd, "--show-toplevel");
    char *root = w->common ? path_join(w->common, "tny-tasks") : NULL;
    if (!w->run || !top || !root) goto fail;
    if (create && mkdir(root, 0700) != 0 && errno != EEXIST) goto fail;
    if (!private_path(root, true)) goto fail;
    char name[100], branch[128];
    snprintf(name, sizeof name, "%s-%d-%d", id.run, id.task, id.attempt);
    snprintf(branch, sizeof branch, "refs/heads/tny-task/%s", name);
    w->dir = path_join(root, name);
    w->branch = xstrdup(branch);
    if (!w->dir || !w->branch) goto fail;
    /* A failed creation reserves its identity. Never recover by adopting it. */
    if (create && mkdir(w->dir, 0700) != 0) goto fail;
    if (!private_path(w->dir, true)) goto fail;
    w->path = path_join(w->dir, "tree");
    char *lock = path_join(w->dir, "lock");
    if (!lock || !w->path) {
        free(lock);
        goto fail;
    }
    w->lock = open(lock, O_RDWR | O_CLOEXEC | O_NOFOLLOW | (create ? O_CREAT | O_EXCL : 0), 0600);
    bool safe = private_path(lock, false);
    free(lock);
    if (w->lock < 0 || !safe || flock(w->lock, LOCK_EX | LOCK_NB) != 0) goto fail;
    free(top);
    free(root);
    return w;
fail:
    free(top);
    free(root);
    task_workspace_close(w);
    return NULL;
}
static bool owned(task_workspace *w) {
    if (!w || w->removed || !private_path(w->dir, true)) return false;
    struct stat st;
    if (lstat(w->path, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    char *top = query(w->path, "--show-toplevel"), *common = query(w->path, "--git-common-dir");
    char *gd = query(w->path, "--absolute-git-dir"), *branch = ref(w->path);
    bool ok = same(top, w->path) && same(common, w->common) && same(gd, w->gitdir) &&
              same(branch, w->branch) && !same(gd, common);
    free(top);
    free(common);
    free(gd);
    free(branch);
    return ok;
}
int task_workspace_prepare(const char *cwd, task_workspace_id id, const char *base,
                           task_workspace **out, char *err, size_t cap) {
    if (!out) return error(err, cap, "missing workspace output");
    *out = NULL;
    if (!cwd || strstr(cwd, "://") || !task_workspace_id_valid(id))
        return error(err, cap, "local Git cwd and valid run/task/attempt required");
    if (!base && !clean(cwd, false))
        return error(err, cap,
                     "launch tree dirty/untracked or not Git; choose an explicit base commit");
    buf_t b = {0};
    buf_t spec = {0};
    buf_appendf(&spec, "%s^{commit}", base ? base : "HEAD");
    int rc = !spec.oom ? GIT(cwd, &b, "rev-parse", "--verify", "--end-of-options", spec.data) : -1;
    buf_free(&spec);
    if (rc != 0 || !b.len || b.len > 64) {
        buf_free(&b);
        return error(err, cap, "base must resolve to a local Git commit");
    }
    task_workspace *w = allocate(cwd, id, true);
    if (!w) {
        buf_free(&b);
        return error(err, cap, "workspace collision, unsafe state, non-Git cwd or busy attempt");
    }
    w->base = buf_detach(&b);
    w->origin = query(cwd, "--show-toplevel");
    w->origin_branch = ref(cwd);
    if (!w->origin_branch) w->origin_branch = xstrdup("");
    if (!w->base || !w->origin || !w->origin_branch || !absent(w->path)) goto fail;
    if (GIT(cwd, &b, "show-ref", "--verify", "--quiet", w->branch) != 1) goto fail;
    if (GIT(cwd, &b, "worktree", "add", "-b", w->branch + 11, "--", w->path, w->base) != 0)
        goto fail;
    w->gitdir = query(w->path, "--absolute-git-dir");
    if (!w->gitdir || !owned(w) || save(w) != 0) goto fail;
    buf_free(&b);
    *out = w;
    return 0;
fail:
    buf_free(&b);
    task_workspace_close(w);
    return error(err, cap,
                 "preparation failed; reserved state preserved, never adopted; use a new attempt");
}
int task_workspace_open(const char *cwd, task_workspace_id id, task_workspace **out, char *err,
                        size_t cap) {
    if (!out) return error(err, cap, "missing workspace output");
    *out = NULL;
    task_workspace *w = allocate(cwd, id, false);
    if (!w) return error(err, cap, "no private workspace or attempt busy");
    char *p = path_join(w->dir, "owner.json");
    yyjson_doc *d = p && private_path(p, false) ? jparse_file(p) : NULL;
    free(p);
    yyjson_val *r = d ? yyjson_doc_get_root(d) : NULL;
    const char *base = jget_str(r, "base"), *origin = jget_str(r, "origin");
    const char *branch = jget_str(r, "origin_branch"), *gd = jget_str(r, "gitdir");
    const char *state = jget_str(r, "state");
    bool valid = base && origin && branch && gd &&
                 (same(state, "ready") || same(state, "removed")) &&
                 same(jget_str(r, "run"), w->run) && jget_int(r, "task", -1) == w->task &&
                 jget_int(r, "attempt", -1) == w->attempt;
    if (valid) {
        w->base = xstrdup(base);
        w->origin = xstrdup(origin);
        w->origin_branch = xstrdup(branch);
        w->gitdir = xstrdup(gd);
        w->removed = same(state, "removed");
        valid = w->base && w->origin && w->origin_branch && w->gitdir;
    }
    if (d) yyjson_doc_free(d);
    /* Missing tree after removal is safe to finalize; never delete a replacement. */
    if (!valid || (!absent(w->path) && !owned(w))) {
        task_workspace_close(w);
        return error(err, cap, "ownership metadata missing/mismatched; state preserved");
    }
    *out = w;
    return 0;
}
int task_workspace_inspect(task_workspace *w, task_workspace_result *out, char *err, size_t cap) {
    if (!out) return error(err, cap, "missing result output");
    memset(out, 0, sizeof *out);
    if (!owned(w)) return error(err, cap, "workspace ownership changed or tree absent");
    out->run = xstrdup(w->run);
    out->task = w->task;
    out->attempt = w->attempt;
    out->path = xstrdup(w->path);
    out->branch = xstrdup(w->branch);
    out->base = xstrdup(w->base);
    out->origin = xstrdup(w->origin);
    out->origin_branch = xstrdup(w->origin_branch);
    out->revision = query(w->path, "HEAD");
    buf_t b = {0};
    if (GIT(w->path, &b, "diff", "--binary", "--no-ext-diff", "--no-textconv", w->base, "--") !=
            0 ||
        b.len >= 16000)
        goto fail;
    /* git_run trims terminal newlines. A nonempty Git patch needs its final LF. */
    if (b.len) buf_appends(&b, "\n");
    if (b.oom) goto fail;
    out->patch = xstrdup(b.data ? b.data : "");
    if (GIT(w->path, &b, "status", "--porcelain=v1", "--untracked-files=all", "--ignored") != 0 ||
        b.len >= 16000)
        goto fail;
    out->dirty = b.len != 0;
    out->status = xstrdup(b.data ? b.data : "");
    if (!out->run || !out->path || !out->branch || !out->base || !out->origin ||
        !out->origin_branch || !out->revision || !out->patch || !out->status)
        goto fail;
    buf_clear(&b);
    buf_appends(&b, "{");
    field(&b, "run", out->run);
    buf_appendf(&b, ",\"task\":%d,\"attempt\":%d", out->task, out->attempt);
    field(&b, "path", out->path);
    field(&b, "branch", out->branch);
    field(&b, "base", out->base);
    field(&b, "origin", out->origin);
    field(&b, "origin_branch", out->origin_branch);
    field(&b, "revision", out->revision);
    field(&b, "patch", out->patch);
    field(&b, "status", out->status);
    buf_appends(&b, "}\n");
    char *p = path_join(w->dir, "result.json");
    int rc = p && !b.oom ? file_write_atomic(p, b.data, b.len) : -1;
    free(p);
    if (rc != 0) goto fail;
    buf_free(&b);
    return 0;
fail:
    buf_free(&b);
    task_workspace_result_free(out);
    return error(err, cap, "cannot snapshot workspace (Git error, allocation or 16000-byte limit)");
}
static int integrate_locked(task_workspace *w, char *err, size_t cap) {
    if (!owned(w)) return error(err, cap, "workspace ownership changed");
    char *branch = ref(w->origin), *common = query(w->origin, "--git-common-dir");
    bool ok = same(branch, w->origin_branch) && same(common, w->common);
    free(branch);
    free(common);
    if (!ok || !clean(w->origin, true) || !clean(w->path, true))
        return error(err, cap, "integration needs recorded launch branch and two clean trees");
    task_workspace_result r;
    if (task_workspace_inspect(w, &r, err, cap) != 0) return -1;
    buf_t b = {0};
    int rc = GIT(w->origin, &b, "-c", "core.hooksPath=/dev/null", "merge", "--no-commit", "--no-ff",
                 "--no-edit", "--", r.revision);
    task_workspace_result_free(&r);
    if (rc != 0) {
        buf_t conflicts = {0};
        bool conflict = GIT(w->origin, &conflicts, "ls-files", "--unmerged") == 0 && conflicts.len;
        buf_free(&conflicts);
        error(err, cap,
              conflict ? "merge conflict preserved at launch tree; worker branch unchanged"
                       : "merge failed; inspect launch tree; no automatic reset/abort");
        rc = conflict ? 1 : -1;
    }
    buf_free(&b);
    return rc;
}
int task_workspace_integrate(task_workspace *w, char *err, size_t cap) {
    if (!w) return error(err, cap, "missing workspace");
    char *p = path_join(w->common, "tny-tasks/integrate.lock");
    int fd = p ? open(p, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600) : -1;
    bool safe = p && private_path(p, false);
    free(p);
    if (fd < 0 || !safe || flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (fd >= 0) close(fd);
        return error(err, cap, "repository integration busy or lock unsafe");
    }
    int rc = integrate_locked(w, err, cap);
    close(fd);
    return rc;
}
int task_workspace_cleanup(task_workspace *w, char *err, size_t cap) {
    if (!w) return error(err, cap, "missing workspace");
    if (absent(w->path)) {
        w->removed = true;
        return save(w) == 0 ? 0 : error(err, cap, "cannot record removal");
    }
    if (!owned(w) || !clean(w->path, true))
        return error(err, cap, "cleanup refused: foreign/dirty tree preserved");
    task_workspace_result r;
    if (task_workspace_inspect(w, &r, err, cap) != 0) return -1;
    task_workspace_result_free(&r);
    buf_t b = {0};
    int rc = GIT(w->origin, &b, "worktree", "remove", "--", w->path);
    buf_free(&b);
    if (rc != 0) return error(err, cap, "Git refused removal; no force used");
    w->removed = true;
    return save(w) == 0 ? 0 : error(err, cap, "tree removed; retry cleanup to record removal");
}
#else
int task_workspace_prepare(const char *cwd, task_workspace_id id, const char *base,
                           task_workspace **out, char *err, size_t cap) {
    (void)cwd;
    (void)id;
    (void)base;
    if (out) *out = NULL;
    return error(err, cap, "managed task workspaces require local native Git; unavailable in wasm");
}
int task_workspace_open(const char *cwd, task_workspace_id id, task_workspace **out, char *err,
                        size_t cap) {
    return task_workspace_prepare(cwd, id, NULL, out, err, cap);
}
int task_workspace_inspect(task_workspace *w, task_workspace_result *out, char *err, size_t cap) {
    (void)w;
    if (out) memset(out, 0, sizeof *out);
    return error(err, cap, "managed task workspaces unavailable in wasm");
}
int task_workspace_integrate(task_workspace *w, char *err, size_t cap) {
    (void)w;
    return error(err, cap, "managed task workspaces unavailable in wasm");
}
int task_workspace_cleanup(task_workspace *w, char *err, size_t cap) {
    return task_workspace_integrate(w, err, cap);
}
#endif
void task_workspace_close(task_workspace *w) {
    if (!w) return;
#ifndef __EMSCRIPTEN__
    if (w->lock >= 0) close(w->lock);
#endif
    free(w->run);
    free(w->common);
    free(w->dir);
    free(w->path);
    free(w->branch);
    free(w->base);
    free(w->origin);
    free(w->origin_branch);
    free(w->gitdir);
    free(w);
}
