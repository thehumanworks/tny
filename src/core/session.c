#include "core/session.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static char *sessions_root(tny_ctx *ctx) {
    char *sess = path_join(ctx->tny_dir, "sessions");
    char *ws = path_join(sess, ctx->ws_hash);
    free(sess);
    return ws;
}

static bool valid_session_id(const char *id) {
    if (!id || strlen(id) != 16) return false;
    for (const unsigned char *p = (const unsigned char *)id; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
            return false;
    return true;
}

static yyjson_mut_val *root_of(tny_session_state *s) { return yyjson_mut_doc_get_root(s->doc); }

static void put_str(tny_session_state *s, const char *k, const char *v) {
    yyjson_mut_obj_put(root_of(s), yyjson_mut_strcpy(s->doc, k), yyjson_mut_strcpy(s->doc, v));
}

tny_session_state *session_new(tny_ctx *ctx) {
    tny_session_state *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->ctx = ctx;
    s->extension_start_reason = xstrdup("new");
    s->id = gen_id();
    char *root = sessions_root(ctx);
    s->dir = path_join(root, s->id);
    free(root);
    s->doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(s->doc, yyjson_mut_obj(s->doc));
    put_str(s, "id", s->id);
    put_str(s, "workspace", ctx->cwd);
    char *ts = now_iso8601();
    put_str(s, "created", ts);
    put_str(s, "updated", ts);
    free(ts);
    yyjson_mut_obj_put(root_of(s), yyjson_mut_strcpy(s->doc, "turns"), yyjson_mut_int(s->doc, 0));
    yyjson_mut_obj_put(root_of(s), yyjson_mut_strcpy(s->doc, "messages"), yyjson_mut_arr(s->doc));
    return s;
}

tny_session_state *session_open(tny_ctx *ctx, const char *id_or_last) {
    if (ctx->no_save) return NULL;
    char *id = NULL;
    if (!id_or_last || strcmp(id_or_last, "last") == 0) {
        id = session_latest_id(ctx);
        if (!id) return NULL;
    } else {
        if (!valid_session_id(id_or_last)) return NULL;
        id = xstrdup(id_or_last);
    }
    if (!valid_session_id(id)) { free(id); return NULL; }
    char *root = sessions_root(ctx);
    char *dir = path_join(root, id);
    free(root);
    char *file = path_join(dir, "session.json");
    yyjson_doc *doc = jparse_file(file);
    free(file);
    if (!doc) { free(id); free(dir); return NULL; }
    tny_session_state *s = calloc(1, sizeof *s);
    if (!s) { yyjson_doc_free(doc); free(id); free(dir); return NULL; }
    s->ctx = ctx;
    s->extension_start_reason = xstrdup("resume");
    s->id = id;
    s->dir = dir;
    s->doc = yyjson_doc_mut_copy(doc, NULL);
    yyjson_doc_free(doc);
    if (!s->doc) { session_close(s); return NULL; }
    return s;
}

int session_save(tny_session_state *s) {
    if (s->ctx->no_save) return 0;
    char *ts = now_iso8601();
    put_str(s, "updated", ts);
    free(ts);
    if (mkdir_p(s->dir) != 0) return -1;
    char *out = jwrite(s->doc);
    if (!out) return -1;
    char *file = path_join(s->dir, "session.json");
    int rc = file_write_atomic(file, out, strlen(out));
    free(file);
    free(out);
    return rc;
}

void session_close(tny_session_state *s) {
    if (!s) return;
    free(s->id);
    free(s->dir);
    free(s->extension_start_reason);
    free(s->extension_previous_session_id);
    yyjson_mut_doc_free(s->doc);
    for (int i = 0; i < s->n_mem_results; i++) {
        free(s->mem_results[i].handle);
        free(s->mem_results[i].data);
    }
    free(s->mem_results);
    free(s);
}

yyjson_mut_val *session_messages(tny_session_state *s) {
    return yyjson_mut_obj_get(root_of(s), "messages");
}

void session_add_text(tny_session_state *s, const char *role, const char *content) {
    yyjson_mut_val *m = yyjson_mut_obj(s->doc);
    yyjson_mut_obj_put(m, yyjson_mut_strcpy(s->doc, "role"), yyjson_mut_strcpy(s->doc, role));
    yyjson_mut_obj_put(m, yyjson_mut_strcpy(s->doc, "content"), yyjson_mut_strcpy(s->doc, content));
    yyjson_mut_arr_add_val(session_messages(s), m);
}

void session_add_assistant(tny_session_state *s, const char *content, const char *tc_json) {
    yyjson_mut_val *m = yyjson_mut_obj(s->doc);
    yyjson_mut_obj_put(m, yyjson_mut_strcpy(s->doc, "role"), yyjson_mut_strcpy(s->doc, "assistant"));
    yyjson_mut_obj_put(m, yyjson_mut_strcpy(s->doc, "content"),
                       content ? yyjson_mut_strcpy(s->doc, content) : yyjson_mut_null(s->doc));
    if (tc_json) {
        yyjson_doc *tc = jparse(tc_json, strlen(tc_json));
        if (tc) {
            yyjson_mut_val *v = yyjson_val_mut_copy(s->doc, yyjson_doc_get_root(tc));
            if (v) yyjson_mut_obj_put(m, yyjson_mut_strcpy(s->doc, "tool_calls"), v);
            yyjson_doc_free(tc);
        }
    }
    yyjson_mut_arr_add_val(session_messages(s), m);
}

void session_add_tool_result(tny_session_state *s, const char *tool_call_id, const char *content) {
    yyjson_mut_val *m = yyjson_mut_obj(s->doc);
    yyjson_mut_obj_put(m, yyjson_mut_strcpy(s->doc, "role"), yyjson_mut_strcpy(s->doc, "tool"));
    yyjson_mut_obj_put(m, yyjson_mut_strcpy(s->doc, "tool_call_id"),
                       yyjson_mut_strcpy(s->doc, tool_call_id));
    yyjson_mut_obj_put(m, yyjson_mut_strcpy(s->doc, "content"), yyjson_mut_strcpy(s->doc, content));
    yyjson_mut_arr_add_val(session_messages(s), m);
}

int session_turns(tny_session_state *s) {
    yyjson_mut_val *v = yyjson_mut_obj_get(root_of(s), "turns");
    return v ? (int)yyjson_mut_get_int(v) : 0;
}

void session_bump_turns(tny_session_state *s) {
    yyjson_mut_obj_put(root_of(s), yyjson_mut_strcpy(s->doc, "turns"),
                       yyjson_mut_int(s->doc, session_turns(s) + 1));
}

const char *session_title(tny_session_state *s) {
    yyjson_mut_val *v = yyjson_mut_obj_get(root_of(s), "title");
    return v ? yyjson_mut_get_str(v) : NULL;
}

void session_set_title(tny_session_state *s, const char *title) {
    char trunc[81];
    snprintf(trunc, sizeof trunc, "%s", title);
    for (char *p = trunc; *p; p++)
        if (*p == '\n' || *p == '\r') *p = ' ';
    put_str(s, "title", trunc);
}

void session_set_meta(tny_session_state *s, const char *backend, const char *model) {
    if (backend) put_str(s, "backend", backend);
    if (model) put_str(s, "model", model);
}

const char *session_backend(tny_session_state *s) {
    yyjson_mut_val *v = yyjson_mut_obj_get(root_of(s), "backend");
    return v ? yyjson_mut_get_str(v) : NULL;
}

void session_set_host_pointer(tny_session_state *s, const char *ptr) {
    put_str(s, "host_pointer", ptr);
}

const char *session_host_pointer(tny_session_state *s) {
    yyjson_mut_val *v = yyjson_mut_obj_get(root_of(s), "host_pointer");
    return v ? yyjson_mut_get_str(v) : NULL;
}

void session_add_usage(tny_session_state *s, int64_t in_tok, int64_t out_tok) {
    yyjson_mut_val *u = yyjson_mut_obj_get(root_of(s), "usage");
    int64_t pin = 0, pout = 0;
    if (u) {
        pin = yyjson_mut_get_int(yyjson_mut_obj_get(u, "in"));
        pout = yyjson_mut_get_int(yyjson_mut_obj_get(u, "out"));
    }
    yyjson_mut_val *nu = yyjson_mut_obj(s->doc);
    yyjson_mut_obj_put(nu, yyjson_mut_strcpy(s->doc, "in"), yyjson_mut_int(s->doc, pin + in_tok));
    yyjson_mut_obj_put(nu, yyjson_mut_strcpy(s->doc, "out"), yyjson_mut_int(s->doc, pout + out_tok));
    yyjson_mut_obj_put(root_of(s), yyjson_mut_strcpy(s->doc, "usage"), nu);
}

void session_get_usage(tny_session_state *s, int64_t *in_tok, int64_t *out_tok) {
    yyjson_mut_val *u = yyjson_mut_obj_get(root_of(s), "usage");
    *in_tok = u ? yyjson_mut_get_int(yyjson_mut_obj_get(u, "in")) : 0;
    *out_tok = u ? yyjson_mut_get_int(yyjson_mut_obj_get(u, "out")) : 0;
}

void session_set_extension_start(tny_session_state *s, const char *reason,
                                 const char *previous_session_id) {
    if (!s) return;
    free(s->extension_start_reason);
    free(s->extension_previous_session_id);
    s->extension_start_reason = xstrdup(reason && *reason ? reason : "new");
    s->extension_previous_session_id = previous_session_id && *previous_session_id
        ? xstrdup(previous_session_id) : NULL;
}

static yyjson_mut_val *extension_audit(tny_session_state *s) {
    yyjson_mut_val *audit = yyjson_mut_obj_get(root_of(s), "extension_audit");
    if (audit && yyjson_mut_is_arr(audit)) return audit;
    audit = yyjson_mut_arr(s->doc);
    yyjson_mut_obj_put(root_of(s), yyjson_mut_strcpy(s->doc, "extension_audit"),
                       audit);
    return audit;
}

void session_record_prompt_audit(tny_session_state *s, const char *submission_id,
                                 const char *submitted, const char *effective,
                                 bool blocked, const char *extension,
                                 const char *reason) {
    if (!s || (!blocked && (!submitted || !effective))) return;
    yyjson_mut_val *entry = yyjson_mut_obj(s->doc);
    yyjson_mut_obj_add_strcpy(s->doc, entry, "kind", "prompt");
    yyjson_mut_obj_add_strcpy(s->doc, entry, "id", submission_id ? submission_id : "");
    yyjson_mut_obj_add_bool(s->doc, entry, "blocked", blocked);
    if (!blocked) {
        yyjson_mut_obj_add_strcpy(s->doc, entry, "submitted", submitted);
        yyjson_mut_obj_add_strcpy(s->doc, entry, "effective", effective);
    }
    if (extension && *extension)
        yyjson_mut_obj_add_strcpy(s->doc, entry, "extension", extension);
    if (reason && *reason)
        yyjson_mut_obj_add_strcpy(s->doc, entry, "reason", reason);
    yyjson_mut_arr_add_val(extension_audit(s), entry);
}

char *session_store_result(tny_session_state *s, const char *data, size_t len) {
    char *handle = gen_id();
    if (s->ctx->no_save) {
        session_mem_result *next = realloc(
            s->mem_results,
            sizeof(*s->mem_results) * (size_t)(s->n_mem_results + 1));
        if (!next) {
            free(handle);
            return NULL;
        }
        s->mem_results = next;
        session_mem_result *r = &s->mem_results[s->n_mem_results++];
        r->handle = xstrdup(handle);
        r->data = xstrndup(data, len);
        r->len = len;
        return handle;
    }
    char *rdir = path_join(s->dir, "results");
    mkdir_p(rdir);
    char *fname = malloc(strlen(handle) + 5);
    sprintf(fname, "%s.txt", handle);
    char *file = path_join(rdir, fname);
    file_write_atomic(file, data, len);
    free(rdir);
    free(fname);
    free(file);
    return handle;
}

char *session_read_result(tny_session_state *s, const char *handle, size_t off,
                          size_t maxlen, size_t *out_len) {
    /* handle ids are hex only — reject anything path-like */
    for (const char *p = handle; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return NULL;
    if (s->ctx->no_save) {
        for (int i = 0; i < s->n_mem_results; i++) {
            session_mem_result *r = &s->mem_results[i];
            if (strcmp(r->handle, handle) != 0) continue;
            if (off >= r->len) {
                *out_len = 0;
                return xstrdup("");
            }
            size_t n = r->len - off;
            if (n > maxlen) n = maxlen;
            *out_len = n;
            return xstrndup(r->data + off, n);
        }
        return NULL;
    }
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "%s/results/%s.txt", s->dir, handle);
    size_t len = 0;
    char *all = file_slurp(b.data, &len);
    buf_free(&b);
    if (!all) return NULL;
    if (off >= len) { free(all); *out_len = 0; return xstrdup(""); }
    size_t n = len - off;
    if (n > maxlen) n = maxlen;
    char *out = xstrndup(all + off, n);
    free(all);
    *out_len = n;
    return out;
}

/* ---- compaction ---- */

int session_compact_boundary(tny_session_state *s, const char **summary) {
    yyjson_mut_val *c = yyjson_mut_obj_get(root_of(s), "compact");
    if (!c) { if (summary) *summary = NULL; return 0; }
    if (summary) *summary = yyjson_mut_get_str(yyjson_mut_obj_get(c, "summary"));
    return (int)yyjson_mut_get_int(yyjson_mut_obj_get(c, "before"));
}

/* Count messages belonging to the last n user turns; returns start index. */
static int start_of_last_turns(yyjson_mut_val *msgs, int keep) {
    int n = (int)yyjson_mut_arr_size(msgs);
    int users = 0;
    for (int i = n - 1; i >= 0; i--) {
        yyjson_mut_val *m = yyjson_mut_arr_get(msgs, (size_t)i);
        const char *role = yyjson_mut_get_str(yyjson_mut_obj_get(m, "role"));
        if (role && strcmp(role, "user") == 0) {
            users++;
            if (users == keep) return i;
        }
    }
    return 0;
}

int session_message_count(tny_session_state *s) {
    yyjson_mut_val *messages = session_messages(s);
    return messages ? (int)yyjson_mut_arr_size(messages) : 0;
}

bool session_compact_needed(tny_session_state *s, bool force) {
    yyjson_mut_val *msgs = session_messages(s);
    if (!msgs) return false;
    int turns = session_turns(s);
    if (!force && turns < 8) return false;
    int keep = force ? 1 : 4;
    int boundary = start_of_last_turns(msgs, keep);
    int old_boundary = session_compact_boundary(s, NULL);
    return boundary > old_boundary;
}

int session_compact(tny_session_state *s, bool force) {
    yyjson_mut_val *msgs = session_messages(s);
    if (!msgs || !session_compact_needed(s, force)) return 0;
    int keep = force ? 1 : 4;
    int boundary = start_of_last_turns(msgs, keep);
    int old_boundary = session_compact_boundary(s, NULL);

    /* mechanical structured summary of [old_boundary, boundary) */
    buf_t sum;
    buf_init(&sum);
    const char *prev = NULL;
    session_compact_boundary(s, &prev);
    if (prev) { buf_appends(&sum, prev); buf_appends(&sum, "\n"); }
    buf_appends(&sum, "Earlier in this session:\n");
    for (int i = old_boundary; i < boundary; i++) {
        yyjson_mut_val *m = yyjson_mut_arr_get(msgs, (size_t)i);
        const char *role = yyjson_mut_get_str(yyjson_mut_obj_get(m, "role"));
        const char *content = yyjson_mut_get_str(yyjson_mut_obj_get(m, "content"));
        if (!role) continue;
        if (strcmp(role, "user") == 0 && content) {
            buf_appendf(&sum, "- user asked: %.160s\n", content);
        } else if (strcmp(role, "assistant") == 0) {
            yyjson_mut_val *tcs = yyjson_mut_obj_get(m, "tool_calls");
            if (tcs) {
                size_t idx, max;
                yyjson_mut_val *tc;
                yyjson_mut_arr_foreach(tcs, idx, max, tc) {
                    yyjson_mut_val *fn = yyjson_mut_obj_get(tc, "function");
                    const char *name = yyjson_mut_get_str(yyjson_mut_obj_get(fn, "name"));
                    const char *args = yyjson_mut_get_str(yyjson_mut_obj_get(fn, "arguments"));
                    if (name) buf_appendf(&sum, "  - ran %s %.100s\n", name, args ? args : "");
                }
            }
            if (content && *content) buf_appendf(&sum, "- assistant: %.160s\n", content);
        }
    }
    yyjson_mut_val *c = yyjson_mut_obj(s->doc);
    yyjson_mut_obj_put(c, yyjson_mut_strcpy(s->doc, "before"), yyjson_mut_int(s->doc, boundary));
    yyjson_mut_obj_put(c, yyjson_mut_strcpy(s->doc, "summary"), yyjson_mut_strcpy(s->doc, sum.data));
    yyjson_mut_obj_put(root_of(s), yyjson_mut_strcpy(s->doc, "compact"), c);
    buf_free(&sum);
    return 1;
}

/* ---- recovery ---- */

void session_recovery_write(tny_session_state *s, const char *partial) {
    if (s->ctx->no_save) return;
    mkdir_p(s->dir);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *r = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, r);
    yyjson_mut_obj_put(r, yyjson_mut_strcpy(doc, "partial"), yyjson_mut_strcpy(doc, partial));
    char *ts = now_iso8601();
    yyjson_mut_obj_put(r, yyjson_mut_strcpy(doc, "at"), yyjson_mut_strcpy(doc, ts));
    free(ts);
    char *out = jwrite(doc);
    yyjson_mut_doc_free(doc);
    if (out) {
        char *file = path_join(s->dir, "recovery.json");
        file_write_atomic(file, out, strlen(out));
        free(file);
        free(out);
    }
}

char *session_recovery_read(tny_session_state *s) {
    if (s->ctx->no_save) return NULL;
    char *file = path_join(s->dir, "recovery.json");
    yyjson_doc *doc = jparse_file(file);
    free(file);
    if (!doc) return NULL;
    const char *p = jget_str(yyjson_doc_get_root(doc), "partial");
    char *out = p ? xstrdup(p) : NULL;
    yyjson_doc_free(doc);
    return out;
}

void session_recovery_clear(tny_session_state *s) {
    if (s->ctx->no_save) return;
    char *file = path_join(s->dir, "recovery.json");
    remove(file);
    free(file);
}

/* ---- listing ---- */

static int cmp_meta_updated(const void *a, const void *b) {
    const session_meta *x = a, *y = b;
    const char *ux = x->updated ? x->updated : "";
    const char *uy = y->updated ? y->updated : "";
    return strcmp(uy, ux); /* newest first */
}

static void scan_ws_dir(const char *wsdir, const char *wsname, session_meta **arr, int *n) {
    DIR *d = opendir(wsdir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        buf_t p;
        buf_init(&p);
        buf_appendf(&p, "%s/%s/session.json", wsdir, e->d_name);
        yyjson_doc *doc = jparse_file(p.data);
        buf_free(&p);
        if (!doc) continue;
        yyjson_val *r = yyjson_doc_get_root(doc);
        *arr = realloc(*arr, sizeof(session_meta) * (size_t)(*n + 1));
        session_meta *m = &(*arr)[(*n)++];
        memset(m, 0, sizeof *m);
        const char *v;
        m->id = xstrdup(e->d_name);
        if ((v = jget_str(r, "title"))) m->title = xstrdup(v);
        if ((v = jget_str(r, "updated"))) m->updated = xstrdup(v);
        if ((v = jget_str(r, "backend"))) m->backend = xstrdup(v);
        if ((v = jget_str(r, "model"))) m->model = xstrdup(v);
        if ((v = jget_str(r, "workspace"))) m->workspace = xstrdup(v);
        m->turns = (int)jget_int(r, "turns", 0);
        yyjson_doc_free(doc);
        (void)wsname;
    }
    closedir(d);
}

session_meta *session_list(tny_ctx *ctx, bool all, int limit, const char *cursor, int *count) {
    session_meta *arr = NULL;
    int n = 0;
    if (all) {
        char *root = path_join(ctx->tny_dir, "sessions");
        DIR *d = opendir(root);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                char *ws = path_join(root, e->d_name);
                scan_ws_dir(ws, e->d_name, &arr, &n);
                free(ws);
            }
            closedir(d);
        }
        free(root);
    } else {
        char *ws = sessions_root(ctx);
        scan_ws_dir(ws, ctx->ws_hash, &arr, &n);
        free(ws);
    }
    if (n) qsort(arr, (size_t)n, sizeof *arr, cmp_meta_updated);
    int start = 0;
    if (cursor) {
        for (int i = 0; i < n; i++)
            if (strcmp(arr[i].id, cursor) == 0) { start = i + 1; break; }
    }
    if (limit <= 0 || limit > 100) limit = 100;
    if (start > 0) {
        for (int i = 0; i < start; i++) {
            free(arr[i].id); free(arr[i].title); free(arr[i].updated);
            free(arr[i].backend); free(arr[i].model); free(arr[i].workspace);
        }
        memmove(arr, arr + start, sizeof *arr * (size_t)(n - start));
        n -= start;
    }
    int m = n > limit ? limit : n;
    for (int i = m; i < n; i++) {
        free(arr[i].id); free(arr[i].title); free(arr[i].updated);
        free(arr[i].backend); free(arr[i].model); free(arr[i].workspace);
    }
    *count = m;
    return arr;
}

void session_meta_free(session_meta *m, int count) {
    for (int i = 0; i < count; i++) {
        free(m[i].id);
        free(m[i].title);
        free(m[i].updated);
        free(m[i].backend);
        free(m[i].model);
        free(m[i].workspace);
    }
    free(m);
}

char *session_latest_id(tny_ctx *ctx) {
    if (ctx->no_save) return NULL;
    int n = 0;
    session_meta *m = session_list(ctx, false, 1, NULL, &n);
    char *id = n > 0 ? xstrdup(m[0].id) : NULL;
    session_meta_free(m, n);
    return id;
}

char *session_recover_copy(tny_ctx *ctx, const char *id) {
    if (ctx->no_save) return NULL;
    char *root = sessions_root(ctx);
    char *src = path_join(root, id);
    char *file = path_join(src, "session.json");
    size_t len = 0;
    char *data = file_slurp(file, &len);
    free(file);
    if (!data) { free(root); free(src); return NULL; }
    /* Trim trailing garbage until it parses (torn write recovery). */
    while (len > 2) {
        yyjson_doc *doc = yyjson_read(data, len, 0);
        if (doc) { yyjson_doc_free(doc); break; }
        len--;
    }
    char *nid = gen_id();
    char *dst = path_join(root, nid);
    mkdir_p(dst);
    char *dfile = path_join(dst, "session.json");
    file_write_atomic(dfile, data, len);
    free(data);
    free(dfile);
    free(dst);
    free(src);
    free(root);
    return nid;
}
