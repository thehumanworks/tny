/* tools_fs.c — file tools: list/glob/grep/read/write/edit/…, /undo support. */
#include "core/tools.h"
#include "core/edit.h"
#include "core/image.h"
#include "util/alloc.h"
#include "util/parallel.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>

#define WALK_MAX_FILES 20000
#define GREP_MAX_FILE  (2u * 1024u * 1024u)
#define GREP_MAX_HITS  500
/* Files scanned per fan-out round (ADR 0132): bounds both the overshoot past
 * the hit cap and the number of thread create/join rounds over a full walk. */
#define GREP_BATCH 256

static bool skip_dir(const char *name) {
    return name[0] == '.' || strcmp(name, "node_modules") == 0 || strcmp(name, "build") == 0 ||
           strcmp(name, "target") == 0 || strcmp(name, "dist") == 0 ||
           strcmp(name, "__pycache__") == 0;
}

typedef bool (*walk_cb)(const char *abs, const char *rel, void *ud);

/* Return false after allocator exhaustion. The caller must propagate NULL so
 * the public next_event boundary can publish its reserved OOM terminal pair. */
static bool walk(const char *root, const char *rel, int *budget, walk_cb cb, void *ud) {
    if (*budget <= 0) return true;
    char *dir = rel[0] ? path_join(root, rel) : xstrdup(root);
    if (!dir) return false;
    DIR *d = opendir(dir);
    if (!d) {
        free(dir);
        return true;
    }
    bool ok = true;
    struct dirent *e;
    while ((e = readdir(d)) && *budget > 0) {
        if (e->d_name[0] == '.') continue;
        char *nrel = rel[0] ? path_join(rel, e->d_name) : xstrdup(e->d_name);
        if (!nrel) {
            ok = false;
            break;
        }
        char *nabs = path_join(root, nrel);
        if (!nabs) {
            free(nrel);
            ok = false;
            break;
        }
        struct stat st;
        if (lstat(nabs, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                if (!skip_dir(e->d_name) && !walk(root, nrel, budget, cb, ud)) ok = false;
            } else if (S_ISREG(st.st_mode)) {
                (*budget)--;
                if (!cb(nabs, nrel, ud)) ok = false;
            }
        }
        free(nrel);
        free(nabs);
        if (!ok) break;
    }
    closedir(d);
    free(dir);
    return ok;
}

/* Snapshot of a walk: its regular files in visiting order. Per-file work that
 * only reads the file can then fan out (util/parallel.h) and fold back in the
 * order a serial walk would have produced. */
typedef struct {
    char *abs;
    char *rel;
} walk_entry;

typedef struct {
    walk_entry *items;
    size_t n, cap;
} walk_list;

static void walk_list_free(walk_list *l) {
    for (size_t i = 0; i < l->n; i++) {
        free(l->items[i].abs);
        free(l->items[i].rel);
    }
    free(l->items);
    l->items = NULL;
    l->n = l->cap = 0;
}

static bool collect_cb(const char *abs, const char *rel, void *ud) {
    walk_list *l = ud;
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 256;
        walk_entry *next = realloc(l->items, cap * sizeof *next);
        if (!next) return false;
        l->items = next;
        l->cap = cap;
    }
    walk_entry *e = &l->items[l->n];
    e->abs = xstrdup(abs);
    e->rel = xstrdup(rel);
    if (!e->abs || !e->rel) {
        free(e->abs);
        free(e->rel);
        return false;
    }
    l->n++;
    return true;
}

#ifdef TNY_ALLOC_TESTING
#if defined(__GNUC__) || defined(__clang__)
#define TNY_TOOLS_TEST_VISIBLE __attribute__((visibility("default")))
#else
#define TNY_TOOLS_TEST_VISIBLE
#endif
static bool walk_test_cb(const char *abs, const char *rel, void *ud) {
    (void)abs;
    int *files = ud;
    char *copy = xstrdup(rel); /* make callback exhaustion observable */
    if (!copy) return false;
    (*files)++;
    free(copy);
    return true;
}

/* Test-only direct seam: production libraries do not export this symbol. */
TNY_TOOLS_TEST_VISIBLE int tny_tools_test_walk(const char *root) {
    tny_alloc_scope_begin("tools_fs_walk");
    int budget = WALK_MAX_FILES;
    int files = 0;
    if (!walk(root, "", &budget, walk_test_cb, &files)) return -1;
    return files;
}
#undef TNY_TOOLS_TEST_VISIBLE
#endif

/* ---- undo: one-deep stack per session ---- */

void tools_undo_record(tools_env *env, const char *abs) {
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
    if (!doc) {
        free(meta);
        return tool_err("nothing to undo");
    }
    const char *path = jget_str(yyjson_doc_get_root(doc), "path");
    bool existed = jget_bool(yyjson_doc_get_root(doc), "existed", false);
    buf_t out;
    buf_init(&out);
    if (path && existed) {
        char *blob = path_join(env->session->dir, "undo.blob");
        size_t len = 0;
        char *prev = file_slurp(blob, &len);
        if (prev && file_write_atomic(path, prev, len) == 0) buf_appendf(&out, "restored %s", path);
        else buf_appendf(&out, "error: could not restore %s", path);
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

struct list_ud {
    buf_t *out;
};

static char *t_list_files(tools_env *env, yyjson_val *args) {
    const char *p = jget_str(args, "path");
    char *err = NULL;
    char *abs = tool_resolve_path(env, p && *p ? p : ".", &err);
    if (!abs) return err;
    DIR *d = opendir(abs);
    if (!d) {
        char *e = tool_err("cannot open %s", abs);
        free(abs);
        return e;
    }
    buf_t out;
    buf_init(&out);
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) && n < 2000) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char *fp = path_join(abs, e->d_name);
        if (!fp) {
            buf_free(&out);
            closedir(d);
            free(abs);
            return NULL;
        }
        struct stat st;
        bool isdir = stat(fp, &st) == 0 && S_ISDIR(st.st_mode);
        buf_appendf(&out, "%s%s\n", e->d_name, isdir ? "/" : "");
        free(fp);
        if (buf_oom(&out)) {
            closedir(d);
            free(abs);
            buf_free(&out);
            return NULL;
        }
        n++;
    }
    closedir(d);
    free(abs);
    if (!out.len) buf_appends(&out, "(empty)");
    if (buf_oom(&out)) {
        buf_free(&out);
        return NULL;
    }
    char *res = tool_bound_result(env, out.data, out.len);
    buf_free(&out);
    return res;
}

struct glob_ud {
    const char *pattern;
    buf_t *out;
    int hits;
};

static bool glob_cb(const char *abs, const char *rel, void *ud) {
    (void)abs;
    struct glob_ud *g = ud;
    if (g->hits >= 1000) return true;
    /* support ** loosely: our glob's '*' already crosses '/' */
    if (glob_match(g->pattern, rel)) {
        buf_appendf(g->out, "%s\n", rel);
        g->hits++;
    }
    return !buf_oom(g->out);
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
        if (*q == '*' && q[1] == '*') {
            buf_appends(&np, "*");
            q++;
        } else buf_append(&np, q, 1);
    }
    if (buf_oom(&np)) {
        free(abs);
        buf_free(&np);
        return NULL;
    }
    buf_t out;
    buf_init(&out);
    struct glob_ud g = {np.data, &out, 0};
    int budget = WALK_MAX_FILES;
    bool walked = walk(abs, "", &budget, glob_cb, &g);
    free(abs);
    buf_free(&np);
    if (!walked || buf_oom(&out) || tny_alloc_scope_failed()) {
        buf_free(&out);
        return NULL;
    }
    if (!out.len) buf_appends(&out, "(no matches)");
    if (buf_oom(&out)) {
        buf_free(&out);
        return NULL;
    }
    char *res = tool_bound_result(env, out.data, out.len);
    buf_free(&out);
    return res;
}

/* One file's matches, scanned into its own slot. */
struct grep_hits {
    buf_t out;
    int hits;
};

struct grep_job {
    const walk_entry *files;
    const char *pat;
    bool ci;
    struct grep_hits *slots;
    /* Hits from every finished scan. Indices are claimed in increasing order,
     * so once this reaches the cap every unclaimed file sits behind enough
     * earlier lines to fill the result, and skipping it changes nothing. */
    atomic_int found;
};

static bool line_contains(const char *line, size_t len, const char *pat, bool ci) {
    size_t pl = strlen(pat);
    if (pl == 0 || pl > len) return false;
    for (size_t i = 0; i + pl <= len; i++) {
        size_t j = 0;
        for (; j < pl; j++) {
            char a = line[i + j], b = pat[j];
            if (ci) {
                a = (char)tolower((unsigned char)a);
                b = (char)tolower((unsigned char)b);
            }
            if (a != b) break;
        }
        if (j == pl) return true;
    }
    return false;
}

/* Allocator exhaustion is not reported here: the caller reads the scope
 * oracle and the slot's sticky oom flag once every scan has joined. */
static void grep_scan(const char *abs, const char *rel, const char *pat, bool ci,
                      struct grep_hits *g) {
    size_t len = 0;
    char *data = file_slurp(abs, &len);
    if (!data) return;
    if (len > GREP_MAX_FILE || memchr(data, 0, len < 4096 ? len : 4096)) {
        free(data);
        return; /* binary or huge */
    }
    size_t start = 0;
    int lineno = 1;
    for (size_t i = 0; i <= len && g->hits < GREP_MAX_HITS; i++) {
        if (i == len || data[i] == '\n') {
            size_t ll = i - start;
            if (line_contains(data + start, ll, pat, ci)) {
                if (ll > 300) ll = 300;
                buf_appendf(&g->out, "%s:%d:", rel, lineno);
                buf_append(&g->out, data + start, ll);
                buf_appends(&g->out, "\n");
                g->hits++;
            }
            start = i + 1;
            lineno++;
        }
    }
    free(data);
}

static void grep_item(size_t i, void *ud) {
    struct grep_job *j = ud;
    if (atomic_load_explicit(&j->found, memory_order_relaxed) >= GREP_MAX_HITS) return;
    grep_scan(j->files[i].abs, j->files[i].rel, j->pat, j->ci, &j->slots[i]);
    atomic_fetch_add_explicit(&j->found, j->slots[i].hits, memory_order_relaxed);
}

/* Append a slot's lines in scan order, stopping at the global hit cap. */
static void grep_fold(buf_t *out, const struct grep_hits *g, int *total) {
    int take = GREP_MAX_HITS - *total;
    if (take > g->hits) take = g->hits;
    if (take <= 0) return;
    size_t end = g->out.len;
    if (take < g->hits) {
        end = 0;
        for (int seen = 0; seen < take; seen++) {
            const char *nl = memchr(g->out.data + end, '\n', g->out.len - end);
            end = (size_t)(nl - g->out.data) + 1;
        }
    }
    buf_append(out, g->out.data, end);
    *total += take;
}

/* Walk first, then scan the files in bounded rounds. Output order and the
 * GREP_MAX_HITS cap match a serial scan exactly; only the reads overlap. */
static bool grep_tree(const char *root, const char *pat, bool ci, buf_t *out) {
    walk_list files = {0};
    int budget = WALK_MAX_FILES;
    if (!walk(root, "", &budget, collect_cb, &files)) {
        walk_list_free(&files);
        return false;
    }
    int total = 0;
    bool ok = true;
    for (size_t done = 0; ok && done < files.n && total < GREP_MAX_HITS; done += GREP_BATCH) {
        size_t n = files.n - done < GREP_BATCH ? files.n - done : GREP_BATCH;
        struct grep_hits *slots = calloc(n, sizeof *slots);
        if (!slots) {
            ok = false;
            break;
        }
        for (size_t i = 0; i < n; i++) buf_init(&slots[i].out);
        struct grep_job job = {.files = files.items + done, .pat = pat, .ci = ci, .slots = slots};
        atomic_init(&job.found, 0);
        tny_parallel_for(n, grep_item, &job);
        for (size_t i = 0; i < n; i++) {
            if (slots[i].out.oom) ok = false;
            else if (ok) grep_fold(out, &slots[i], &total);
            buf_free(&slots[i].out);
        }
        free(slots);
        if (tny_alloc_scope_failed()) ok = false;
    }
    walk_list_free(&files);
    return ok;
}

static char *t_grep_files(tools_env *env, yyjson_val *args) {
    const char *pat = jget_str(args, "pattern");
    if (!pat) return tool_err("missing pattern");
    const char *p = jget_str(args, "path");
    char *err = NULL;
    char *abs = tool_resolve_path(env, p && *p ? p : ".", &err);
    if (!abs) return err;
    bool ci = jget_bool(args, "case_insensitive", false);
    buf_t out;
    buf_init(&out);
    bool ok;
    struct stat st;
    if (stat(abs, &st) == 0 && S_ISREG(st.st_mode)) {
        struct grep_hits one = {.hits = 0};
        buf_init(&one.out);
        grep_scan(abs, p, pat, ci, &one);
        ok = !one.out.oom;
        int total = 0;
        if (ok) grep_fold(&out, &one, &total);
        buf_free(&one.out);
    } else ok = grep_tree(abs, pat, ci, &out);
    free(abs);
    if (!ok || buf_oom(&out) || tny_alloc_scope_failed()) {
        buf_free(&out);
        return NULL;
    }
    if (!out.len) buf_appends(&out, "(no matches)");
    if (buf_oom(&out)) {
        buf_free(&out);
        return NULL;
    }
    char *res = tool_bound_result(env, out.data, out.len);
    buf_free(&out);
    return res;
}

static char *read_file_exp_preview(tools_env *env, const char *path, const char *data, size_t len,
                                   int64_t offset, int64_t limit) {
    const tny_ctx *ctx = env->ctx;
    size_t lines = 0;
    for (size_t i = 0; i < len; i++)
        if (data[i] == '\n') lines++;
    if (len && data[len - 1] != '\n') lines++;
    if (!offset && !limit && len <= ctx->exp_read_bytes && !ctx->exp_read_lineno)
        return xstrndup(data, len);
    size_t first = offset > 0 ? (size_t)offset : 1;
    size_t pos = 0, line = 1;
    while (pos < len && line < first) {
        const char *end = memchr(data + pos, '\n', len - pos);
        if (!end) {
            pos = len;
            break;
        }
        pos = (size_t)(end + 1 - data);
        line++;
    }
    buf_t body;
    buf_init(&body);
    size_t shown = 0;
    while (pos < len && (limit <= 0 || shown < (size_t)limit)) {
        const char *end = memchr(data + pos, '\n', len - pos);
        size_t line_len = end ? (size_t)(end + 1 - (data + pos)) : len - pos;
        char marker[32];
        size_t marker_len = 0;
        if (ctx->exp_read_lineno && line % ctx->exp_read_lineno == 0)
            marker_len = (size_t)snprintf(marker, sizeof marker, "%zu|", line);
        if (line_len + marker_len > ctx->exp_read_bytes - body.len) break;
        if (marker_len) buf_append(&body, marker, marker_len);
        buf_append(&body, data + pos, line_len);
        if (buf_oom(&body)) {
            buf_free(&body);
            return NULL;
        }
        pos += line_len;
        line++;
        shown++;
    }
    buf_t result;
    buf_init(&result);
    buf_appendf(&result,
                "[%s: %zu lines, %zu bytes; showing lines %zu-%zu; continue with "
                "offset=%zu]\n",
                path, lines, len, first, shown ? first + shown - 1 : first - 1, first + shown);
    if (!shown && pos < len) {
        char *handle = env->session ? session_store_result(env->session, data, len) : NULL;
        if (handle) {
            buf_appendf(&result,
                        "[next line exceeds inline budget; full file: handle:%s; "
                        "use read_tool_result for byte ranges]\n",
                        handle);
            free(handle);
        } else buf_appends(&result, "[next line exceeds inline budget]\n");
    }
    if (body.len) buf_append(&result, body.data, body.len);
    buf_free(&body);
    if (buf_oom(&result)) {
        buf_free(&result);
        return NULL;
    }
    return buf_detach(&result);
}

static char *t_read_file(tools_env *env, yyjson_val *args) {
    char *err = NULL;
    char *abs = tool_resolve_path(env, jget_str(args, "path"), &err);
    if (!abs) return err;
    size_t len = 0;
    char *data = file_slurp(abs, &len);
    if (!data) {
        char *e = tool_err("cannot read %s", abs);
        free(abs);
        return e;
    }
    const char *mime = image_mime((const uint8_t *)data, len);
    if (mime) {
        free(data);
        char *e = tool_err("%s is %s; use read_image to view it", abs, mime);
        free(abs);
        return e;
    }
    int64_t off = jget_int(args, "offset", 0);
    int64_t lim = jget_int(args, "limit", 0);
    char *res;
    if (env->ctx->exp_spill) {
        res = read_file_exp_preview(env, abs, data, len, off, lim);
        if (res) tools_learning_read_result(env, TNY_LEARN_READ, abs, len > 0);
    } else if (off > 0 || lim > 0) {
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
        if (res) tools_learning_read_result(env, TNY_LEARN_READ, abs, out.len > 0);
        buf_free(&out);
    } else {
        res = tool_bound_result(env, data, len);
        if (res) tools_learning_read_result(env, TNY_LEARN_READ, abs, len > 0);
    }
    free(abs);
    free(data);
    return res;
}

static char *t_write_file(tools_env *env, yyjson_val *args) {
    char *err = NULL;
    char *abs = tool_resolve_path(env, jget_str(args, "path"), &err);
    if (!abs) return err;
    const char *content = jget_str(args, "content");
    if (!content) {
        free(abs);
        return tool_err("missing content");
    }
    tools_undo_record(env, abs);
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

static void edit_record_undo(const char *path, void *userdata) {
    tools_undo_record(userdata, path);
}

static char *t_edit_file(tools_env *env, yyjson_val *args) {
    char *err = NULL;
    char *abs = tool_resolve_path(env, jget_str(args, "path"), &err);
    if (!abs) return err;
    const char *olds = jget_str(args, "old_string");
    const char *news = jget_str(args, "new_string");
    bool all = jget_bool(args, "replace_all", false);
    if (!olds || !news || !*olds) {
        free(abs);
        return tool_err("missing old_string/new_string");
    }
    tny_edit_result result = {0};
    tny_edit_hooks hooks = {.before_write = edit_record_undo, .before_write_userdata = env};
    tny_edit_status status = tny_edit_file_exact(abs, olds, news, all, &hooks, &result);
    tools_learning_edit_result(env, abs, news, all, status);
    if (status == TNY_EDIT_READ_ERROR) {
        char *e = tool_err("cannot read %s", abs);
        free(abs);
        return e;
    }
    if (status == TNY_EDIT_NOT_FOUND) {
        buf_t msg;
        buf_init(&msg);
        /* Do not put the hint through tool_err's fixed formatting buffer or
         * tool_bound_result's byte cut: either could silently split it. */
        buf_appendf(&msg, "error: old_string not found in %s", abs);
        if (result.nearest_context) {
            size_t len = strlen(result.nearest_context);
            size_t cut = len > 300 ? 300 : len;
            while (cut && ((unsigned char)result.nearest_context[cut] & 0xc0) == 0x80) cut--;
            /* File bytes need not be text. Never introduce invalid UTF-8 into
             * the tool result; the ordinary failure still reports the path. */
            if (cut && utf8_valid_bytes(result.nearest_context, cut)) {
                buf_appendf(&msg, "\nAdvisory (first nonempty search line only), line %zu: ",
                            result.nearest_line);
                buf_append(&msg, result.nearest_context, cut);
                if (cut < len) buf_appends(&msg, " [truncated]");
            }
        }
        tny_edit_result_free(&result);
        free(abs);
        return buf_detach(&msg);
    }
    if (status == TNY_EDIT_AMBIGUOUS) {
        char *e = tool_err("old_string occurs %zu times in %s; pass replace_all or a longer match",
                           result.matches, abs);
        free(abs);
        return e;
    }
    if (status == TNY_EDIT_NOMEM) {
        free(abs);
        return NULL;
    }
    buf_t msg;
    buf_init(&msg);
    if (status == TNY_EDIT_OK)
        buf_appendf(&msg, "replaced %zu occurrence%s in %s", result.replaced,
                    (all && result.matches > 1) ? "s" : "", abs);
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
        tools_undo_record(env, abs);
        if (remove(abs) == 0) buf_appendf(&out, "deleted %s", abs);
        else buf_appendf(&out, "error: cannot delete %s", abs);
    } else if (strcmp(op, "mkdir") == 0) {
        if (mkdir_p(abs) == 0) buf_appendf(&out, "created %s", abs);
        else buf_appendf(&out, "error: cannot create %s", abs);
    } else if (strcmp(op, "info") == 0) {
        struct stat st;
        if (stat(abs, &st) == 0)
            buf_appendf(&out, "%s: %s, %lld bytes, mtime %lld", abs,
                        S_ISDIR(st.st_mode) ? "directory" : "file", (long long)st.st_size,
                        (long long)st.st_mtime);
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
    if (!dst) {
        free(src);
        return err;
    }
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
struct sem_terms {
    char terms[8][64];
    int nterms;
};

struct sem_best {
    char *rel;
    int score;
};

struct sem_job {
    const walk_entry *files;
    const struct sem_terms *q;
    int *scores;
};

/* Score one file. Unreadable, binary and huge files score 0, as does a file
 * whose read hit allocator exhaustion (the scope oracle reports that). */
static int sem_score(const char *abs, const char *rel, const struct sem_terms *q) {
    size_t len = 0;
    char *data = file_slurp(abs, &len);
    if (!data) return 0;
    if (len > GREP_MAX_FILE || memchr(data, 0, len < 4096 ? len : 4096)) {
        free(data);
        return 0;
    }
    for (size_t i = 0; i < len; i++) data[i] = (char)tolower((unsigned char)data[i]);
    int score = 0;
    for (int t = 0; t < q->nterms; t++) {
        int hits = 0;
        for (char *p = data; (p = strstr(p, q->terms[t])); p++) hits++;
        if (hits) score += 1 + (hits > 10 ? 10 : hits);
    }
    /* filename hits are worth extra */
    for (int t = 0; t < q->nterms; t++)
        if (strstr(rel, q->terms[t])) score += 5;
    free(data);
    return score;
}

static void sem_item(size_t i, void *ud) {
    struct sem_job *j = ud;
    j->scores[i] = sem_score(j->files[i].abs, j->files[i].rel, j->q);
}

/* Insert into the top ten; an equal score keeps the earlier file. */
static bool sem_rank(struct sem_best best[10], const char *rel, int score) {
    for (int i = 0; i < 10; i++) {
        if (score > best[i].score) {
            free(best[9].rel);
            memmove(&best[i + 1], &best[i], sizeof best[0] * (size_t)(9 - i));
            best[i].rel = xstrdup(rel);
            best[i].score = score;
            return best[i].rel != NULL;
        }
    }
    return true;
}

static char *t_semantic_search(tools_env *env, yyjson_val *args) {
    const char *q = jget_str(args, "query");
    if (!q) return tool_err("missing query");
    struct sem_terms s;
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
    walk_list files = {0};
    int budget = WALK_MAX_FILES;
    bool ok = walk(env->ctx->cwd, "", &budget, collect_cb, &files);
    int *scores = NULL;
    if (ok && files.n) {
        scores = calloc(files.n, sizeof *scores);
        if (scores) {
            struct sem_job job = {files.items, &s, scores};
            tny_parallel_for(files.n, sem_item, &job);
        } else ok = false;
    }
    /* Rank in walk order so ties resolve exactly as a serial scan would. */
    struct sem_best best[10];
    memset(best, 0, sizeof best);
    for (size_t i = 0; ok && i < files.n; i++)
        if (scores[i] > 0 && !sem_rank(best, files.items[i].rel, scores[i])) ok = false;
    free(scores);
    walk_list_free(&files);
    if (!ok || tny_alloc_scope_failed()) {
        for (int i = 0; i < 10; i++) free(best[i].rel);
        return NULL;
    }
    buf_t out;
    buf_init(&out);
    for (int i = 0; i < 10; i++)
        if (best[i].rel) {
            buf_appendf(&out, "%s (score %d)\n", best[i].rel, best[i].score);
            free(best[i].rel);
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
