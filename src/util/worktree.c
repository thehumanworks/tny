#include "util/worktree.h"
#include "util/git.h"
#include "json/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool worktree_name_valid(const char *name) {
    if (!name || !*name || strlen(name) > 80) return false;
    for (size_t i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum && (i == 0 || (c != '-' && c != '_'))) return false;
    }
    return true;
}

void worktree_close(tny_worktree *w) {
    if (!w) return;
    if (w->lock_fd >= 0) close(w->lock_fd);
    free(w->name);
    free(w->path);
    free(w->origin);
    free(w->origin_ref);
    free(w->branch);
    free(w->common);
    free(w->gitdir);
    free(w);
}

#ifdef __EMSCRIPTEN__
tny_worktree *worktree_enter(const char *cwd, const char *name, char *err, size_t errlen) {
    (void)cwd;
    (void)name;
    snprintf(err, errlen, "Git worktrees are unavailable in wasm; use a native tny build");
    return NULL;
}
int worktree_merge(tny_worktree *w, char *err, size_t errlen) {
    (void)w;
    snprintf(err, errlen, "Git worktrees are unavailable in wasm");
    return -1;
}
int worktree_remove(tny_worktree *w, char *err, size_t errlen) {
    return worktree_merge(w, err, errlen);
}
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

#define GIT(cwd, out, ...) git_run(cwd, (const char *const[]){__VA_ARGS__, NULL}, out)

static char *query(const char *cwd, const char *arg) {
    buf_t b = {0};
    int rc = GIT(cwd, &b, "rev-parse", "--path-format=absolute", arg);
    char *s = rc == 0 && b.len ? buf_detach(&b) : NULL;
    buf_free(&b);
    return s;
}

static char *branch_ref(const char *cwd) {
    buf_t b = {0};
    int rc = GIT(cwd, &b, "symbolic-ref", "--quiet", "HEAD");
    char *s = rc == 0 ? buf_detach(&b) : xstrdup("");
    buf_free(&b);
    return s;
}

static bool same(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }

static bool same_repo(const char *path, const char *common) {
    char *other = query(path, "--git-common-dir");
    bool ok = same(other, common);
    free(other);
    return ok;
}

static bool control_free(const char *s) {
    for (; s && *s; s++)
        if ((unsigned char)*s < 0x20 || (unsigned char)*s == 0x7f) return false;
    return s != NULL;
}

static int metadata(tny_worktree *w) {
    char *path = path_join(w->gitdir, "tny-worktree.json");
    if (!path) return -1;
    int rc = -1;
    struct stat st;
    if (lstat(path, &st) == 0) {
        yyjson_doc *doc = S_ISREG(st.st_mode) ? jparse_file(path) : NULL;
        yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
        const char *origin = jget_str(root, "origin");
        const char *ref = jget_str(root, "origin_ref");
        if (origin && origin[0] == '/' && ref && control_free(origin) && control_free(ref) &&
            (!*ref || str_starts(ref, "refs/heads/"))) {
            char *o = xstrdup(origin), *r = xstrdup(ref);
            if (o && r) {
                free(w->origin);
                free(w->origin_ref);
                w->origin = o;
                w->origin_ref = r;
                rc = 0;
            } else {
                free(o);
                free(r);
            }
        }
        if (doc) yyjson_doc_free(doc);
    } else if (errno == ENOENT) {
        buf_t b = {0};
        buf_appends(&b, "{\"origin\":");
        jescape(&b, w->origin);
        buf_appends(&b, ",\"origin_ref\":");
        jescape(&b, w->origin_ref);
        buf_appends(&b, "}\n");
        if (!b.oom) rc = file_write_atomic(path, b.data, b.len);
        buf_free(&b);
    }
    free(path);
    return rc;
}

tny_worktree *worktree_enter(const char *cwd, const char *name, char *err, size_t errlen) {
    snprintf(err, errlen, "cannot enter worktree");
    if (name && !worktree_name_valid(name)) {
        snprintf(err, errlen,
                 "worktree name must be 1-80 letters, digits, '-' or '_', starting "
                 "with a letter or digit");
        return NULL;
    }
    const char *source = cwd ? cwd : ".";
    tny_worktree *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->lock_fd = -1;
    buf_t b = {0};
    char *state = NULL, *root = NULL, *top = NULL;
    w->origin = query(source, "--show-toplevel");
    w->common = query(source, "--git-common-dir");
    if (!w->origin || !w->common) {
        snprintf(err, errlen,
                 "current directory does not belong to a Git working repository "
                 "(git must be installed)");
        goto fail;
    }
    if (!control_free(w->origin) || !control_free(w->common)) goto fail;
    if (GIT(source, &b, "rev-parse", "--verify", "HEAD") != 0) {
        snprintf(err, errlen, "repository has no HEAD commit; create an initial commit first");
        goto fail;
    }
    w->origin_ref = branch_ref(source);
    w->name = name ? xstrdup(name) : gen_id();
    state = path_tny_dir();
    root = state ? path_join(state, "worktrees") : NULL;
    if (!root || !w->name || !w->origin_ref || mkdir_p(root) != 0) goto fail;
    char *abs_root = path_abs(root);
    free(root);
    root = abs_root;
    w->path = root ? path_join(root, w->name) : NULL;
    if (!w->path) goto fail;
    struct stat st;
    if (lstat(w->path, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            snprintf(err, errlen, "worktree name already exists and is not a worktree directory");
            goto fail;
        }
    } else {
        if (errno != ENOENT) goto fail;
        char branch[100], ref[120];
        snprintf(branch, sizeof branch, "worktree/%s", w->name);
        snprintf(ref, sizeof ref, "refs/heads/%s", branch);
        bool exists = GIT(source, &b, "show-ref", "--verify", "--quiet", ref) == 0;
        int rc = exists ? GIT(source, &b, "worktree", "add", "--", w->path, branch)
                        : GIT(source, &b, "worktree", "add", "-b", branch, "--", w->path, "HEAD");
        if (rc != 0) {
            snprintf(err, errlen, "git worktree add failed: %s", b.data ? b.data : "unknown error");
            goto fail;
        }
        w->created = true;
    }
    top = query(w->path, "--show-toplevel");
    w->gitdir = query(w->path, "--absolute-git-dir");
    if (!same(top, w->path) || !same_repo(w->path, w->common) || !w->gitdir ||
        same(w->gitdir, w->common)) {
        snprintf(err, errlen,
                 "name already exists but is not a linked worktree of this repository");
        goto fail;
    }
    w->branch = branch_ref(w->path);
    if (!w->branch || !*w->branch) {
        snprintf(err, errlen, "existing worktree has a detached HEAD; check out a branch first");
        goto fail;
    }
    char *lock = path_join(w->gitdir, "tny-use.lock");
    w->lock_fd = lock ? open(lock, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600) : -1;
    free(lock);
    if (w->lock_fd < 0 || flock(w->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        snprintf(err, errlen, "worktree is in use by another tny process or cannot be locked");
        goto fail;
    }
    if (metadata(w) != 0) {
        snprintf(err, errlen, "cannot read or save worktree origin metadata; worktree kept at %s",
                 w->path);
        goto fail;
    }
    free(top);
    free(root);
    free(state);
    buf_free(&b);
    return w;
fail:
    free(top);
    free(root);
    free(state);
    buf_free(&b);
    worktree_close(w);
    return NULL;
}

static bool clean_checkout(const char *path, bool removing, char *err, size_t errlen) {
    char *dir = query(path, "--absolute-git-dir");
    if (!dir) return false;
    const char *markers[] = {
        "MERGE_HEAD", "CHERRY_PICK_HEAD", "REVERT_HEAD", "rebase-merge", "rebase-apply",
        "sequencer",  "BISECT_LOG",       "index.lock",  "locked",       NULL};
    bool ok = true;
    for (size_t i = 0; markers[i]; i++) {
        char *p = path_join(dir, markers[i]);
        struct stat st;
        if (!p || lstat(p, &st) == 0 || errno != ENOENT) {
            snprintf(err, errlen, "checkout has a Git operation or lock in progress: %s", path);
            ok = false;
        }
        free(p);
        if (!ok) break;
    }
    free(dir);
    if (!ok) return false;
    buf_t b = {0};
    int rc = GIT(path, &b, "status", "--porcelain=v1", "--untracked-files=all",
                 "--ignore-submodules=none", removing ? "--ignored" : "--ignored=no");
    ok = rc == 0 && b.len == 0;
    if (!ok)
        snprintf(err, errlen,
                 "checkout has uncommitted, untracked%s files or cannot be inspected: %s",
                 removing ? " or ignored" : "", path);
    buf_free(&b);
    return ok;
}

static bool verify_worktree(tny_worktree *w, char *err, size_t errlen) {
    if (!w || w->lock_fd < 0) return false;
    struct stat st;
    char *top = query(w->path, "--show-toplevel");
    char *dir = query(w->path, "--absolute-git-dir");
    char *branch = branch_ref(w->path);
    bool ok = lstat(w->path, &st) == 0 && S_ISDIR(st.st_mode) && same(top, w->path) &&
              same(dir, w->gitdir) && same_repo(w->path, w->common) && same(branch, w->branch);
    if (!ok) snprintf(err, errlen, "worktree repository, path or branch changed; worktree kept");
    free(top);
    free(dir);
    free(branch);
    return ok;
}

int worktree_merge(tny_worktree *w, char *err, size_t errlen) {
    snprintf(err, errlen, "cannot merge worktree");
    if (!verify_worktree(w, err, errlen)) return -1;
    char *branch = branch_ref(w->origin);
    char *top = query(w->origin, "--show-toplevel");
    bool ok = *w->origin_ref && same(branch, w->origin_ref) && same(top, w->origin) &&
              !same(w->path, w->origin) && same_repo(w->origin, w->common);
    free(branch);
    free(top);
    if (!ok) {
        snprintf(err, errlen,
                 "original checkout is missing, detached, or on a different branch; "
                 "merge manually (worktree kept)");
        return -1;
    }
    if (!clean_checkout(w->path, false, err, errlen) ||
        !clean_checkout(w->origin, false, err, errlen))
        return -1;
    buf_t head = {0}, out = {0};
    int rc = GIT(w->path, &head, "rev-parse", "--verify", "HEAD");
    if (!rc && head.len)
        rc = GIT(w->origin, &out, "merge", "--ff", "--no-edit", "--no-autostash",
                 "--no-overwrite-ignore", "--", head.data);
    else rc = -1;
    if (rc != 0)
        snprintf(err, errlen,
                 "merge failed; worktree kept. Resolve in %s or use git merge --abort: %s",
                 w->origin, out.data ? out.data : "cannot read HEAD");
    buf_free(&head);
    buf_free(&out);
    return rc;
}

int worktree_remove(tny_worktree *w, char *err, size_t errlen) {
    snprintf(err, errlen, "cannot remove worktree");
    if (!verify_worktree(w, err, errlen) || !clean_checkout(w->path, true, err, errlen)) return -1;
    /* Run from the common Git directory, so removal also works after the
     * original checkout has moved. Never force or delete the branch. */
    buf_t b = {0};
    int rc = GIT(w->common, &b, "worktree", "remove", "--", w->path);
    if (rc)
        snprintf(err, errlen, "git worktree remove failed: %s", b.data ? b.data : "unknown error");
    buf_free(&b);
    return rc;
}
#endif
