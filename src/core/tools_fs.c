/* tools_fs.c — file tools: list/glob/grep/read/write/edit/…, /undo support. */
#include "core/tools.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#define WALK_MAX_FILES 20000
#define GREP_MAX_FILE  (2u * 1024u * 1024u)

static bool skip_dir(const char *name) {
    return name[0] == '.' || strcmp(name, "node_modules") == 0 ||
           strcmp(name, "build") == 0 || strcmp(name, "target") == 0 ||
           strcmp(name, "dist") == 0 || strcmp(name, "__pycache__") == 0;
}

typedef void (*walk_cb)(const char *abs, const char *rel, void *ud);

static void walk(const char *root, const char *rel, int *budget, walk_cb cb, void *ud) {
    if (*budget <= 0) return;
    char *dir = rel[0] ? path_join(root, rel) : xstrdup(root);
    DIR *d = opendir(dir);
    if (!d) { free(dir); return; }
    struct dirent *e;
    while ((e = readdir(d)) && *budget > 0) {
        if (e->d_name[0] == '.') continue;
        char *nrel = rel[0] ? path_join(rel, e->d_name) : xstrdup(e->d_name);
        char *nabs = path_join(root, nrel);
        struct stat st;
        if (lstat(nabs, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                if (!skip_dir(e->d_name)) walk(root, nrel, budget, cb, ud);
            } else if (S_ISREG(st.st_mode)) {
                (*budget)--;
                cb(nabs, nrel, ud);
            }
        }
        free(nrel);
        free(nabs);
    }
    closedir(d);
    free(dir);
}

/* ---- undo: one-deep stack per session ---- */

static void undo_record(tools_env *env, const char *abs) {
    if (!env->session) return;
    mkdir_p(env->session->dir);
    char *meta = path_join(env->session->dir, "undo.json");
    char *blob = path_join(env->session->dir, "undo.blob");
    size_t len = 0;
    char *prev = file_slurp(abs, &len);
    buf_t j;
    buf_init(&j);
    buf_appends(&j, "{\"path\":");
    jescape(&j, abs);
    buf_appendf(&j, ",\"existed\":%s}", prev ? "true" : "false");
    file_write_atomic(meta, j.data, j.len);
    if (prev) file_write_atomic(blob, prev, len);
    else remove(blob);
    buf_free(&j);
    free(prev);
    free(meta);
    free(blob);
}

char *tools_undo_last(tools_env *env) {
    if (!env->session) return tool_err("no session to undo from");
    char *meta = path_join(env->session->dir, "undo.json");
    yyjson_doc *doc = jparse_file(meta);
    if (!doc) { free(meta); return tool_err("nothing to undo"); }
    const char *path = jget_str(yyjson_doc_get_root(doc), "path");
    bool existed = jget_bool(yyjson_doc_get_root(doc), "existed", false);
    buf_t out;
    buf_init(&out);
    if (path && existed) {
        char *blob = path_join(env->session->dir, "undo.blob");
        size_t len = 0;
        char *prev = file_slurp(blob, &len);
        if (prev && file_write_atomic(path, prev, len) == 0)
            buf_appendf(&out, "restored %s", path);
        else
            buf_appendf(&out, "error: could not restore %s", path);
        free(prev);
        free(blob);
    } else if (path) {
        remove(path);
        buf_appendf(&out, "removed %s (undid creation)", path);
    } else {
        buf_appends(&out, "nothing to undo");
    }
    yyjson_doc_free(doc);
    remove(meta);
    free(meta);
    return buf_detach(&out);
}

/* ---- individual tools ---- */

struct list_ud { buf_t *out; };

static char *t_list_files(tools_env *env, yyjson_val *args) {
    const char *p = jget_str(args, "path");
    char *err = NULL;
    char *abs = tool_resolve_path(env, p && *p ? p : ".", &err);
    if (!abs) return err;
    DIR *d = opendir(abs);
    if (!d) { char *e = tool_err("cannot open %s", abs); free(abs); return e; }
    buf_t out;
    buf_init(&out);
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) && n < 2000) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char *fp = path_join(abs, e->d_name);
        struct stat st;
        bool isdir = stat(fp, &st) == 0 && S_ISDIR(st.st_mode);
        buf_appendf(&out, "%s%s\n", e->d_name, isdir ? "/" : "");
        free(fp);
        n++;
    }
    closedir(d);
    free(abs);
    if (!out.len) buf_appends(&out, "(empty)");
    char *res = tool_bound_result(env, out.data, out.len);
    buf_free(&out);
    return res;
}

struct glob_ud { const char *pattern; buf_t *out; int hits; };

static void glob_cb(const char *abs, const char *rel, void *ud) {
    (void)abs;
    struct glob_ud *g = ud;
    if (g->hits >= 1000) return;
    /* support ** loosely: our glob's '*' already crosses '/' */
    if (glob_match(g->pattern, rel)) {
        buf_appendf(g->out, "%s\n", rel);
        g->hits++;
    }
}

static char *t_glob_files(tools_env *env, yyjson_val *args) {
    const char *pat = jget_str(args, "pattern");
    if (!pat) return tool_err("missing pattern");
    const char *p = jget_str(args, "path");
    char *err = NULL;
    char *abs = tool_resolve_path(env, p && *p ? p : ".", &err);
    if (!abs) return err;
    /* normalize ** to * (our matcher crosses '/') */
    buf_t np;
    buf_init(&np);
    for (const char *q = pat; *q; q++) {
        if (*q == '*' && q[1] == '*') { buf_appends(&np, "*"); q++; }
        else buf_append(&np, q, 1);
    }
    buf_t out;
    buf_init(&out);
    struct glob_ud g = {np.data, &out, 0};
    int budget = WALK_MAX_FILES;
    walk(abs, "", &budget, glob_cb, &g);
    free(abs);
    buf_free(&np);
    if (!out.len) buf_appends(&out, "(no matches)");
    char *res = tool_bound_result(env, out.data, out.len);
    buf_free(&out);
    return res;
}

struct grep_ud { const char *pat; bool ci; buf_t *out; int hits; };

static bool line_contains(const char *line, size_t len, const char *pat, bool ci) {
    size_t pl = strlen(pat);
    if (pl == 0 || pl > len) return false;
    for (size_t i = 0; i + pl <= len; i++) {
        size_t j = 0;
        for (; j < pl; j++) {
            char a = line[i + j], b = pat[j];
            if (ci) { a = (char)tolower((unsigned char)a); b = (char)tolower((unsigned char)b); }
            if (a != b) break;
        }
        if (j == pl) return true;
    }
    return false;
}

static void grep_cb(const char *abs, const char *rel, void *ud) {
    struct grep_ud *g = ud;
    if (g->hits >= 500) return;
    size_t len = 0;
    char *data = file_slurp(abs, &len);
    if (!data) return;
    if (len > GREP_MAX_FILE || memchr(data, 0, len < 4096 ? len : 4096)) {
        free(data);
        return; /* binary or huge */
    }
    size_t start = 0;
    int lineno = 1;
    for (size_t i = 0; i <= len && g->hits < 500; i++) {
        if (i == len || data[i] == '\n') {
            size_t ll = i - start;
            if (line_contains(data + start, ll, g->pat, g->ci)) {
                if (ll > 300) ll = 300;
                buf_appendf(g->out, "%s:%d:", rel, lineno);
                buf_append(g->out, data + start, ll);
                buf_appends(g->out, "\n");
                g->hits++;
            }
            start = i + 1;
            lineno++;
        }
    }
    free(data);
}

static char *t_grep_files(tools_env *env, yyjson_val *args) {
    const char *pat = jget_str(args, "pattern");
    if (!pat) return tool_err("missing pattern");
    const char *p = jget_str(args, "path");
    char *err = NULL;
    char *abs = tool_resolve_path(env, p && *p ? p : ".", &err);
    if (!abs) return err;
    buf_t out;
    buf_init(&out);
    struct grep_ud g = {pat, jget_bool(args, "case_insensitive", false), &out, 0};
    int budget = WALK_MAX_FILES;
    struct stat st;
    if (stat(abs, &st) == 0 && S_ISREG(st.st_mode)) grep_cb(abs, p, &g);
    else walk(abs, "", &budget, grep_cb, &g);
    free(abs);
    if (!out.len) buf_appends(&out, "(no matches)");
    char *res = tool_bound_result(env, out.data, out.len);
    buf_free(&out);
    return res;
}

static char *t_read_file(tools_env *env, yyjson_val *args) {
    char *err = NULL;
    char *abs = tool_resolve_path(env, jget_str(args, "path"), &err);
    if (!abs) return err;
    size_t len = 0;
    char *data = file_slurp(abs, &len);
    if (!data) { char *e = tool_err("cannot read %s", abs); free(abs); return e; }
    free(abs);
    int64_t off = jget_int(args, "offset", 0);
    int64_t lim = jget_int(args, "limit", 0);
    char *res;
    if (off > 0 || lim > 0) {
        buf_t out;
        buf_init(&out);
        size_t start = 0;
        int line = 1;
        int emitted = 0;
        for (size_t i = 0; i <= len; i++) {
            if (i == len || data[i] == '\n') {
                if (line >= (off > 0 ? off : 1)) {
                    buf_append(&out, data + start, i - start);
                    buf_appends(&out, "\n");
                    emitted++;
                    if (lim > 0 && emitted >= lim) break;
                }
                start = i + 1;
                line++;
            }
        }
        res = tool_bound_result(env, out.data, out.len);
        buf_free(&out);
    } else {
        res = tool_bound_result(env, data, len);
    }
    free(data);
    return res;
}

static char *t_write_file(tools_env *env, yyjson_val *args) {
    char *err = NULL;
    char *abs = tool_resolve_path(env, jget_str(args, "path"), &err);
    if (!abs) return err;
    const char *content = jget_str(args, "content");
    if (!content) { free(abs); return tool_err("missing content"); }
    undo_record(env, abs);
    /* ensure parent exists */
    char *slash = strrchr(abs, '/');
    if (slash && slash != abs) {
        *slash = 0;
        mkdir_p(abs);
        *slash = '/';
    }
    int rc = file_write_atomic(abs, content, strlen(content));
    buf_t out;
    buf_init(&out);
    if (rc == 0) buf_appendf(&out, "wrote %zu bytes to %s", strlen(content), abs);
    else buf_appendf(&out, "error: write to %s failed", abs);
    free(abs);
    return buf_detach(&out);
}

static char *t_edit_file(tools_env *env, yyjson_val *args) {
    char *err = NULL;
    char *abs = tool_resolve_path(env, jget_str(args, "path"), &err);
    if (!abs) return err;
    const char *olds = jget_str(args, "old_string");
    const char *news = jget_str(args, "new_string");
    bool all = jget_bool(args, "replace_all", false);
    if (!olds || !news || !*olds) { free(abs); return tool_err("missing old_string/new_string"); }
    size_t len = 0;
    char *data = file_slurp(abs, &len);
    if (!data) { char *e = tool_err("cannot read %s", abs); free(abs); return e; }
    size_t ol = strlen(olds);
    /* count matches */
    int count = 0;
    for (char *p = data; (p = strstr(p, olds)); p += ol) count++;
    if (count == 0) {
        free(data);
        char *e = tool_err("old_string not found in %s", abs);
        free(abs);
        return e;
    }
    if (count > 1 && !all) {
        free(data);
        char *e = tool_err("old_string occurs %d times in %s; pass replace_all or a longer match", count, abs);
        free(abs);
        return e;
    }
    undo_record(env, abs);
    buf_t out;
    buf_init(&out);
    char *p = data;
    for (;;) {
        char *hit = strstr(p, olds);
        if (!hit) { buf_appends(&out, p); break; }
        buf_append(&out, p, (size_t)(hit - p));
        buf_appends(&out, news);
        p = hit + ol;
        if (!all) { buf_appends(&out, p); break; }
    }
    free(data);
    int rc = file_write_atomic(abs, out.data, out.len);
    buf_free(&out);
    buf_t msg;
    buf_init(&msg);
    if (rc == 0) buf_appendf(&msg, "replaced %d occurrence%s in %s", all ? count : 1,
                             (all && count > 1) ? "s" : "", abs);
    else buf_appendf(&msg, "error: write to %s failed", abs);
    free(abs);
    return buf_detach(&msg);
}

static char *t_simple_path_op(tools_env *env, yyjson_val *args, const char *op) {
    char *err = NULL;
    char *abs = tool_resolve_path(env, jget_str(args, "path"), &err);
    if (!abs) return err;
    buf_t out;
    buf_init(&out);
    if (strcmp(op, "delete") == 0) {
        undo_record(env, abs);
        if (remove(abs) == 0) buf_appendf(&out, "deleted %s", abs);
        else buf_appendf(&out, "error: cannot delete %s", abs);
    } else if (strcmp(op, "mkdir") == 0) {
        if (mkdir_p(abs) == 0) buf_appendf(&out, "created %s", abs);
        else buf_appendf(&out, "error: cannot create %s", abs);
    } else if (strcmp(op, "info") == 0) {
        struct stat st;
        if (stat(abs, &st) == 0)
            buf_appendf(&out, "%s: %s, %lld bytes, mtime %lld", abs,
                        S_ISDIR(st.st_mode) ? "directory" : "file",
                        (long long)st.st_size, (long long)st.st_mtime);
        else buf_appendf(&out, "error: cannot stat %s", abs);
    }
    free(abs);
    return buf_detach(&out);
}

static char *t_two_path_op(tools_env *env, yyjson_val *args, bool copy) {
    char *err = NULL;
    char *src = tool_resolve_path(env, jget_str(args, "path"), &err);
    if (!src) return err;
    char *dst = tool_resolve_path(env, jget_str(args, "new_path"), &err);
    if (!dst) { free(src); return err; }
    buf_t out;
    buf_init(&out);
    if (copy) {
        size_t len = 0;
        char *data = file_slurp(src, &len);
        if (data && file_write_atomic(dst, data, len) == 0)
            buf_appendf(&out, "copied %s -> %s", src, dst);
        else buf_appendf(&out, "error: copy %s -> %s failed", src, dst);
        free(data);
    } else {
        if (rename(src, dst) == 0) buf_appendf(&out, "renamed %s -> %s", src, dst);
        else buf_appendf(&out, "error: rename %s -> %s failed", src, dst);
    }
    free(src);
    free(dst);
    return buf_detach(&out);
}

/* semantic_search: lexical scoring — count query-term hits per file. */
struct sem_ud {
    char terms[8][64];
    int nterms;
    struct { char *rel; int score; } best[10];
};

static void sem_cb(const char *abs, const char *rel, void *ud) {
    struct sem_ud *s = ud;
    size_t len = 0;
    char *data = file_slurp(abs, &len);
    if (!data) return;
    if (len > GREP_MAX_FILE || memchr(data, 0, len < 4096 ? len : 4096)) { free(data); return; }
    for (size_t i = 0; i < len; i++) data[i] = (char)tolower((unsigned char)data[i]);
    int score = 0;
    for (int t = 0; t < s->nterms; t++) {
        int hits = 0;
        for (char *p = data; (p = strstr(p, s->terms[t])); p++) hits++;
        if (hits) score += 1 + (hits > 10 ? 10 : hits);
    }
    /* filename hits are worth extra */
    for (int t = 0; t < s->nterms; t++)
        if (strstr(rel, s->terms[t])) score += 5;
    free(data);
    if (score == 0) return;
    for (int i = 0; i < 10; i++) {
        if (score > s->best[i].score) {
            free(s->best[9].rel);
            memmove(&s->best[i + 1], &s->best[i], sizeof s->best[0] * (size_t)(9 - i));
            s->best[i].rel = xstrdup(rel);
            s->best[i].score = score;
            break;
        }
    }
}

static char *t_semantic_search(tools_env *env, yyjson_val *args) {
    const char *q = jget_str(args, "query");
    if (!q) return tool_err("missing query");
    struct sem_ud s;
    memset(&s, 0, sizeof s);
    const char *p = q;
    while (*p && s.nterms < 8) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        const char *start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        size_t tl = (size_t)(p - start);
        if (tl >= 3 && tl < 63) {
            for (size_t i = 0; i < tl; i++)
                s.terms[s.nterms][i] = (char)tolower((unsigned char)start[i]);
            s.terms[s.nterms][tl] = 0;
            s.nterms++;
        }
    }
    if (!s.nterms) return tool_err("query has no searchable terms");
    int budget = WALK_MAX_FILES;
    walk(env->ctx->cwd, "", &budget, sem_cb, &s);
    buf_t out;
    buf_init(&out);
    for (int i = 0; i < 10; i++)
        if (s.best[i].rel) {
            buf_appendf(&out, "%s (score %d)\n", s.best[i].rel, s.best[i].score);
            free(s.best[i].rel);
        }
    if (!out.len) buf_appends(&out, "(no relevant files found)");
    return buf_detach(&out);
}

static char *t_open_file(tools_env *env, yyjson_val *args) {
    char *err = NULL;
    char *abs = tool_resolve_path(env, jget_str(args, "path"), &err);
    if (!abs) return err;
    buf_t cmd;
    buf_init(&cmd);
#ifdef __APPLE__
    buf_appends(&cmd, "open ");
#else
    buf_appends(&cmd, "xdg-open ");
#endif
    buf_appendf(&cmd, "'%s' >/dev/null 2>&1 &", abs);
    int rc = system(cmd.data);
    buf_free(&cmd);
    buf_t out;
    buf_init(&out);
    buf_appendf(&out, rc == 0 ? "opened %s" : "error: could not open %s", abs);
    free(abs);
    return buf_detach(&out);
}

char *tool_fs_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled) {
    *handled = true;
    if (strcmp(name, "list_files") == 0) return t_list_files(env, args);
    if (strcmp(name, "glob_files") == 0) return t_glob_files(env, args);
    if (strcmp(name, "grep_files") == 0) return t_grep_files(env, args);
    if (strcmp(name, "read_file") == 0) return t_read_file(env, args);
    if (strcmp(name, "write_file") == 0) return t_write_file(env, args);
    if (strcmp(name, "edit_file") == 0) return t_edit_file(env, args);
    if (strcmp(name, "delete_file") == 0) return t_simple_path_op(env, args, "delete");
    if (strcmp(name, "create_folder") == 0) return t_simple_path_op(env, args, "mkdir");
    if (strcmp(name, "file_info") == 0) return t_simple_path_op(env, args, "info");
    if (strcmp(name, "rename_file") == 0) return t_two_path_op(env, args, false);
    if (strcmp(name, "copy_file") == 0) return t_two_path_op(env, args, true);
    if (strcmp(name, "semantic_search") == 0) return t_semantic_search(env, args);
    if (strcmp(name, "open_file") == 0) return t_open_file(env, args);
    *handled = false;
    return NULL;
}
