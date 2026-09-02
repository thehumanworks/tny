#include "core/session.h"
#include "core/tasks.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>

static char *sessions_root(tny_ctx *ctx) {
    char *sess = path_join(ctx->tny_dir, "sessions");
    if (!sess) return NULL;
    char *ws = path_join(sess, ctx->ws_hash);
    free(sess);
    return ws;
}

static bool valid_session_id(const char *id) {
    if (!id || strlen(id) != 16) return false;
    for (const unsigned char *p = (const unsigned char *)id; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false;
    return true;
}

static yyjson_mut_val *root_of(tny_session_state *s) { return yyjson_mut_doc_get_root(s->doc); }

static bool put_str(tny_session_state *s, const char *k, const char *v) {
    if (!s || !s->doc || !k || !v) return false;
    yyjson_mut_val *key = yyjson_mut_strcpy(s->doc, k);
    yyjson_mut_val *value = yyjson_mut_strcpy(s->doc, v);
    return key && value && yyjson_mut_obj_put(root_of(s), key, value);
}

static void clear_ctx_task(tny_ctx *ctx) {
    if (!ctx) return;
    free(ctx->task_name);
    free(ctx->task_source);
    free(ctx->task_instructions);
    ctx->task_name = NULL;
    ctx->task_source = NULL;
    ctx->task_instructions = NULL;
    ctx->task_digest[0] = 0;
    ctx->task_explicit = false;
}

static const char *task_source_category(const tny_ctx *ctx) {
    const char *source = ctx ? ctx->task_source : NULL;
    return tny_task_source_valid(source) ? source : "explicit";
}

static bool safe_task_source(const char *source) { return tny_task_source_valid(source); }

typedef enum {
    TASK_SNAPSHOT_ABSENT = 0,
    TASK_SNAPSHOT_VALID,
    TASK_SNAPSHOT_INVALID,
    TASK_SNAPSHOT_OOM,
} task_snapshot_result;

/* Read through one no-follow descriptor, then enforce the bound while the
 * descriptor is held. This avoids the lstat()/fopen() swap in the original
 * draft and also rejects a regular file that grows after its initial stat. */
static task_snapshot_result read_task_snapshot(const char *path, char **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!path) return TASK_SNAPSHOT_OOM;
    int flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags);
    if (fd < 0) return errno == ENOENT ? TASK_SNAPSHOT_ABSENT : TASK_SNAPSHOT_INVALID;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size > TNY_TASK_BODY_MAX) {
        close(fd);
        return TASK_SNAPSHOT_INVALID;
    }
    buf_t contents;
    buf_init(&contents);
    char chunk[8192];
    ssize_t got;
    while ((got = read(fd, chunk, sizeof chunk)) > 0) {
        size_t n = (size_t)got;
        if (n > TNY_TASK_BODY_MAX - contents.len) {
            close(fd);
            buf_free(&contents);
            return TASK_SNAPSHOT_INVALID;
        }
        buf_append(&contents, chunk, n);
    }
    close(fd);
    if (got < 0 || !contents.len || buf_oom(&contents)) {
        bool oom = buf_oom(&contents);
        buf_free(&contents);
        return oom ? TASK_SNAPSHOT_OOM : TASK_SNAPSHOT_INVALID;
    }
    size_t len = contents.len;
    char *body = buf_detach(&contents);
    if (!body) return TASK_SNAPSHOT_OOM;
    if (!utf8_valid_bytes(body, len)) {
        free(body);
        return TASK_SNAPSHOT_INVALID;
    }
    if (out) *out = body;
    else free(body);
    if (out_len) *out_len = len;
    return TASK_SNAPSHOT_VALID;
}

static bool task_snapshot_matches(const char *body, size_t len, const char *digest) {
    char computed[TNY_TASK_DIGEST_HEX_LEN + 1];
    uint8_t digest_bytes[TNY_TASK_DIGEST_HEX_LEN / 2];
    if (!sha1((const uint8_t *)body, len, digest_bytes)) return false;
    for (size_t i = 0; i < sizeof digest_bytes; i++)
        snprintf(computed + i * 2, 3, "%02x", digest_bytes[i]);
    return strcmp(computed, digest) == 0;
}

static bool digest_valid(const char *digest) {
    if (!digest || strlen(digest) != TNY_TASK_DIGEST_HEX_LEN) return false;
    for (const unsigned char *p = (const unsigned char *)digest; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false;
    return true;
}

int session_task_bind_current(tny_session_state *s) {
    if (!s || !s->ctx || !s->ctx->task_name || !s->ctx->task_instructions ||
        !tny_task_name_valid(s->ctx->task_name) || !tny_task_source_valid(s->ctx->task_source) ||
        !digest_valid(s->ctx->task_digest))
        return -1;
    size_t body_len = strlen(s->ctx->task_instructions);
    if (!body_len || body_len > TNY_TASK_BODY_MAX ||
        !utf8_valid_bytes(s->ctx->task_instructions, body_len) ||
        !task_snapshot_matches(s->ctx->task_instructions, body_len, s->ctx->task_digest))
        return -1;
    char *body = xstrdup(s->ctx->task_instructions);
    if (!body) return -1;
    yyjson_mut_val *task = yyjson_mut_obj(s->doc);
    yyjson_mut_val *name = yyjson_mut_strcpy(s->doc, s->ctx->task_name);
    yyjson_mut_val *source = yyjson_mut_strcpy(s->doc, task_source_category(s->ctx));
    yyjson_mut_val *digest = yyjson_mut_strcpy(s->doc, s->ctx->task_digest);
    if (!task || !name || !source || !digest ||
        !yyjson_mut_obj_put(task, yyjson_mut_strcpy(s->doc, "name"), name) ||
        !yyjson_mut_obj_put(task, yyjson_mut_strcpy(s->doc, "source"), source) ||
        !yyjson_mut_obj_put(task, yyjson_mut_strcpy(s->doc, "digest"), digest) ||
        !yyjson_mut_obj_put(root_of(s), yyjson_mut_strcpy(s->doc, "task"), task)) {
        free(body);
        return -1;
    }
    free(s->task_body);
    s->task_body = body;
    return 0;
}

void session_task_clear(tny_session_state *s) {
    if (!s) return;
    yyjson_mut_obj_remove_key(root_of(s), "task");
    free(s->task_body);
    s->task_body = NULL;
}

int session_task_reconcile(tny_session_state *s, char *err, size_t errsz) {
    if (!s || !s->ctx) return -1;
    yyjson_mut_val *task = yyjson_mut_obj_get(root_of(s), "task");
    if (!task) {
        if (s->ctx->task_explicit && session_turns(s) > 0) {
            snprintf(err, errsz,
                     "session has no saved task; a task cannot be added after turns exist");
            return -1;
        }
        if (s->ctx->task_explicit) return session_task_bind_current(s);
        clear_ctx_task(s->ctx); /* do not carry a restored task into an old no-task session */
        session_task_clear(s);
        return 0;
    }
    if (!yyjson_mut_is_obj(task)) {
        snprintf(err, errsz, "saved task metadata is invalid");
        return -1;
    }
    const char *name = yyjson_mut_get_str(yyjson_mut_obj_get(task, "name"));
    const char *source = yyjson_mut_get_str(yyjson_mut_obj_get(task, "source"));
    const char *digest = yyjson_mut_get_str(yyjson_mut_obj_get(task, "digest"));
    if (!tny_task_name_valid(name) || !tny_task_source_valid(source) || !digest_valid(digest)) {
        snprintf(err, errsz, "saved task metadata is invalid");
        return -1;
    }
    char *path = path_join(s->dir, "task.md");
    char *pending_path = path_join(s->dir, "task.md.next");
    size_t n = 0;
    char *body = NULL;
    task_snapshot_result snapshot = read_task_snapshot(path, &body, &n);
    /* session_save publishes metadata before the final sidecar rename. If a
     * process dies in that tiny window, the fully written .next file is the
     * committed metadata's recoverable snapshot. A stale .next with another
     * digest is never accepted. */
    if (snapshot != TASK_SNAPSHOT_VALID || !task_snapshot_matches(body, n, digest)) {
        free(body);
        body = NULL;
        n = 0;
        snapshot = read_task_snapshot(pending_path, &body, &n);
    }
    /* The writer may have renamed .next between those two opens. Retry the
     * canonical spelling once so a concurrent atomic publish cannot create a
     * false corruption result. Session writer locking prevents an unbounded
     * stream of such transitions during normal turns. */
    if (snapshot != TASK_SNAPSHOT_VALID || !task_snapshot_matches(body, n, digest)) {
        free(body);
        body = NULL;
        n = 0;
        snapshot = read_task_snapshot(path, &body, &n);
    }
    free(path);
    free(pending_path);
    if (snapshot != TASK_SNAPSHOT_VALID || !task_snapshot_matches(body, n, digest)) {
        free(body);
        snprintf(err, errsz, "saved task snapshot is missing or invalid");
        return -1;
    }
    bool requested = s->ctx->task_explicit;
    if (requested &&
        (!tny_task_name_valid(s->ctx->task_name) || !digest_valid(s->ctx->task_digest) ||
         strcmp(s->ctx->task_name, name) != 0 || strcmp(s->ctx->task_digest, digest) != 0)) {
        free(body);
        snprintf(
            err, errsz,
            "requested task does not match the session's saved task (name and digest must match)");
        return -1;
    }
    if (tny_task_set_explicit(s->ctx, name, body, source) != 0) {
        free(body);
        snprintf(err, errsz, "could not restore the saved task snapshot");
        return -1;
    }
    s->ctx->task_explicit = requested;
    free(s->task_body);
    s->task_body = body;
    return 0;
}

tny_session_state *session_new(tny_ctx *ctx) {
    tny_session_state *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->lock_fd = -1;
    s->ctx = ctx;
    s->extension_start_reason = xstrdup("new");
    s->id = gen_id();
    char *root = sessions_root(ctx);
    s->dir = root && s->id ? path_join(root, s->id) : NULL;
    free(root);
    s->doc = yyjson_mut_doc_new(jallocator());
    yyjson_mut_val *doc_root = s->doc ? yyjson_mut_obj(s->doc) : NULL;
    if (!s->extension_start_reason || !s->id || !s->dir || !s->doc || !doc_root) {
        session_close(s);
        return NULL;
    }
    yyjson_mut_doc_set_root(s->doc, doc_root);
    if (!put_str(s, "id", s->id) || !put_str(s, "workspace", ctx->cwd)) {
        session_close(s);
        return NULL;
    }
    char *ts = now_iso8601();
    if (!ts || !put_str(s, "created", ts) || !put_str(s, "updated", ts)) {
        free(ts);
        session_close(s);
        return NULL;
    }
    free(ts);
    yyjson_mut_val *turns_key = yyjson_mut_strcpy(s->doc, "turns");
    yyjson_mut_val *turns = yyjson_mut_int(s->doc, 0);
    yyjson_mut_val *messages_key = yyjson_mut_strcpy(s->doc, "messages");
    yyjson_mut_val *messages = yyjson_mut_arr(s->doc);
    if (!turns_key || !turns || !messages_key || !messages ||
        !yyjson_mut_obj_put(root_of(s), turns_key, turns) ||
        !yyjson_mut_obj_put(root_of(s), messages_key, messages)) {
        session_close(s);
        return NULL;
    }
    bool has_task_state = ctx->task_name || ctx->task_source || ctx->task_instructions ||
                          ctx->task_digest[0] || ctx->task_explicit;
    if (has_task_state && session_task_bind_current(s) != 0) {
        session_close(s);
        return NULL;
    }
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
    if (!valid_session_id(id)) {
        free(id);
        return NULL;
    }
    char *root = sessions_root(ctx);
    char *dir = root ? path_join(root, id) : NULL;
    free(root);
    char *file = dir ? path_join(dir, "session.json") : NULL;
    yyjson_doc *doc = file ? jparse_file(file) : NULL;
    free(file);
    if (!doc) {
        free(id);
        free(dir);
        return NULL;
    }
    tny_session_state *s = calloc(1, sizeof *s);
    if (!s) {
        yyjson_doc_free(doc);
        free(id);
        free(dir);
        return NULL;
    }
    s->lock_fd = -1;
    s->ctx = ctx;
    s->extension_start_reason = xstrdup("resume");
    s->id = id;
    s->dir = dir;
    s->doc = yyjson_doc_mut_copy(doc, jallocator());
    yyjson_doc_free(doc);
    if (!s->extension_start_reason || !s->doc) {
        session_close(s);
        return NULL;
    }
    return s;
}

int session_save(tny_session_state *s) {
    if (s->ctx->no_save) return 0;
    char *ts = now_iso8601();
    if (!ts || !put_str(s, "updated", ts)) {
        free(ts);
        return -1;
    }
    free(ts);
    if (mkdir_p(s->dir) != 0) return -1;
    char *out = jwrite(s->doc);
    if (!out) return -1;
    char *file = path_join(s->dir, "session.json");
    char *task_file = path_join(s->dir, "task.md");
    char *pending_task_file = path_join(s->dir, "task.md.next");
    if (!file || !task_file || !pending_task_file) {
        free(pending_task_file);
        free(task_file);
        free(file);
        free(out);
        return -1;
    }
    bool has_task = yyjson_mut_obj_get(root_of(s), "task") != NULL;
    if (has_task && (!s->task_body || file_write_atomic(pending_task_file, s->task_body,
                                                        strlen(s->task_body)) != 0)) {
        free(pending_task_file);
        free(task_file);
        free(file);
        free(out);
        return -1;
    }
    int rc = file_write_atomic(file, out, strlen(out));
    if (rc != 0) {
        if (has_task) unlink(pending_task_file);
    } else if (has_task) {
        /* Readers that race this rename can validate task.md.next against the
         * already committed metadata. */
        if (rename(pending_task_file, task_file) != 0) rc = -1;
    } else {
        if (unlink(task_file) != 0 && errno != ENOENT) rc = -1;
        if (unlink(pending_task_file) != 0 && errno != ENOENT) rc = -1;
    }
    free(pending_task_file);
    free(task_file);
    free(file);
    free(out);
    return rc;
}

void session_close(tny_session_state *s) {
    if (!s) return;
    session_lock_release(s);
    free(s->id);
    free(s->dir);
    free(s->extension_start_reason);
    free(s->extension_previous_session_id);
    free(s->task_body);
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
    yyjson_mut_obj_put(m, yyjson_mut_strcpy(s->doc, "role"),
                       yyjson_mut_strcpy(s->doc, "assistant"));
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
    yyjson_mut_obj_put(nu, yyjson_mut_strcpy(s->doc, "out"),
                       yyjson_mut_int(s->doc, pout + out_tok));
    yyjson_mut_obj_put(root_of(s), yyjson_mut_strcpy(s->doc, "usage"), nu);
}

void session_get_usage(tny_session_state *s, int64_t *in_tok, int64_t *out_tok) {
    yyjson_mut_val *u = yyjson_mut_obj_get(root_of(s), "usage");
    *in_tok = u ? yyjson_mut_get_int(yyjson_mut_obj_get(u, "in")) : 0;
    *out_tok = u ? yyjson_mut_get_int(yyjson_mut_obj_get(u, "out")) : 0;
}

/* ---- background status / writer lock / pid (docs/adr/0031) ---- */

void session_set_status_running(tny_session_state *s) {
    if (!s) return;
    put_str(s, "status", "running");
    /* a resumed finished session goes live again: drop the stale record */
    yyjson_mut_obj_remove_key(root_of(s), "exit_code");
    yyjson_mut_obj_remove_key(root_of(s), "result");
}

void session_set_status_finished(tny_session_state *s, const char *status, int exit_code,
                                 const char *result_json) {
    if (!s || !status) return;
    put_str(s, "status", status);
    yyjson_mut_obj_put(root_of(s), yyjson_mut_strcpy(s->doc, "exit_code"),
                       yyjson_mut_int(s->doc, exit_code));
    if (!result_json) return;
    yyjson_doc *r = jparse(result_json, strlen(result_json));
    if (!r) return; /* unparseable result: store nothing, keep the doc sane */
    yyjson_val *root = yyjson_doc_get_root(r);
    if (yyjson_is_obj(root)) {
        yyjson_mut_val *v = yyjson_val_mut_copy(s->doc, root);
        if (v) yyjson_mut_obj_put(root_of(s), yyjson_mut_strcpy(s->doc, "result"), v);
    }
    yyjson_doc_free(r);
}

const char *session_status(tny_session_state *s) {
    if (!s) return NULL;
    yyjson_mut_val *v = yyjson_mut_obj_get(root_of(s), "status");
    return v ? yyjson_mut_get_str(v) : NULL;
}

int session_lock_acquire(tny_session_state *s) {
    if (!s || s->ctx->no_save) return 0;
    if (s->lock_fd >= 0) return 0; /* already held by this state */
    if (mkdir_p(s->dir) != 0) return -1;
    char *file = path_join(s->dir, "lock");
    /* O_CLOEXEC: spawned hosts must not inherit the writer lock — only the
     * fork()ed background child (no exec) keeps it. */
    int fd = open(file, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    free(file);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1; /* another process is running a turn on this session */
    }
    s->lock_fd = fd;
    return 0;
}

void session_lock_release(tny_session_state *s) {
    if (!s || s->lock_fd < 0) return;
    close(s->lock_fd); /* flock releases with the last fd of the description */
    s->lock_fd = -1;
}

/* Shared probe for a session directory's writer lock. */
static bool lock_dir_held(const char *dir) {
    char *file = path_join(dir, "lock");
    int fd = open(file, O_RDONLY | O_CLOEXEC);
    free(file);
    if (fd < 0) return false; /* never backgrounded / already cleaned */
    bool held = flock(fd, LOCK_SH | LOCK_NB) != 0;
    close(fd); /* success: close drops our shared lock immediately */
    return held;
}

bool session_is_running(tny_ctx *ctx, const char *id) {
    if (!ctx || !id || ctx->no_save) return false;
    char *root = sessions_root(ctx);
    char *dir = path_join(root, id);
    free(root);
    bool held = lock_dir_held(dir);
    free(dir);
    return held;
}

int session_write_pid(tny_session_state *s, pid_t pid) {
    if (!s || s->ctx->no_save) return -1;
    if (mkdir_p(s->dir) != 0) return -1;
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%ld\n", (long)pid);
    char *file = path_join(s->dir, "pid");
    int rc = file_write_atomic(file, buf, (size_t)n);
    free(file);
    return rc;
}

pid_t session_read_pid(tny_ctx *ctx, const char *id) {
    if (!ctx || !id) return -1;
    char *root = sessions_root(ctx);
    char *dir = path_join(root, id);
    char *file = path_join(dir, "pid");
    free(root);
    free(dir);
    size_t len = 0;
    char *data = file_slurp(file, &len);
    free(file);
    if (!data) return -1;
    char *end = NULL;
    long v = strtol(data, &end, 10);
    bool ok = end != data && v > 0 && (*end == 0 || *end == '\n' || *end == '\r');
    free(data);
    return ok ? (pid_t)v : -1;
}

/* Bounded wait for the writer lock to free. A plain sleep with no fds to
 * watch, so this stays a nanosleep loop rather than tny_poll (which needs
 * a pollable fd; docs/adr/0017's rule targets fd waits). */
static bool stop_wait(tny_ctx *ctx, const char *id, int total_ms) {
    int waited = 0;
    for (;;) {
        if (!session_is_running(ctx, id)) return true;
        if (waited >= total_ms) return false;
        struct timespec ts = {0, 100L * 1000000L};
        nanosleep(&ts, NULL);
        waited += 100;
    }
}

int session_stop(tny_ctx *ctx, const char *id, bool force_kill, char *err, size_t errsz) {
    if (err && errsz) err[0] = 0;
    if (!session_is_running(ctx, id)) return 1; /* caller no-ops */
    pid_t pid = session_read_pid(ctx, id);
    if (pid <= 0) {
        snprintf(err, errsz,
                 "session %s is running but has no pid file; "
                 "cannot signal it",
                 id);
        return -1;
    }
    int timeout_ms = 5000;
    const char *env = getenv("TNY_STOP_TIMEOUT_MS");
    if (env && atoi(env) > 0) timeout_ms = atoi(env);
    /* Recycled-pgid guard (docs/adr/0031 decision 6): signal the process
     * GROUP, and only while the lock probe confirms a live holder. */
    if (session_is_running(ctx, id)) kill(-pid, SIGTERM);
    if (stop_wait(ctx, id, timeout_ms)) return 0; /* the child finalized "interrupted" itself */
    if (!force_kill) return 2;                    /* still running; caller suggests --kill */
    if (session_is_running(ctx, id)) kill(-pid, SIGKILL);
    if (!stop_wait(ctx, id, 2000)) {
        snprintf(err, errsz,
                 "session %s did not release its lock after "
                 "SIGKILL",
                 id);
        return -1;
    }
    /* The only case where a non-child writes a terminal status: the killed
     * child could not finalize, so record the outcome on its behalf. */
    tny_session_state *s = session_open(ctx, id);
    if (!s) {
        snprintf(err, errsz, "session %s: cannot open after kill", id);
        return -1;
    }
    if (session_lock_acquire(s) != 0) {
        session_close(s);
        snprintf(err, errsz,
                 "session %s: another writer took over after "
                 "kill",
                 id);
        return -1;
    }
    session_set_status_finished(s, "interrupted", 137, NULL);
    int rc = session_save(s);
    session_close(s);
    if (rc != 0) {
        snprintf(err, errsz, "session %s: cannot write terminal status", id);
        return -1;
    }
    return 0;
}

void session_set_extension_start(tny_session_state *s, const char *reason,
                                 const char *previous_session_id) {
    if (!s) return;
    free(s->extension_start_reason);
    free(s->extension_previous_session_id);
    s->extension_start_reason = xstrdup(reason && *reason ? reason : "new");
    s->extension_previous_session_id =
        previous_session_id && *previous_session_id ? xstrdup(previous_session_id) : NULL;
}

static yyjson_mut_val *extension_audit(tny_session_state *s) {
    yyjson_mut_val *audit = yyjson_mut_obj_get(root_of(s), "extension_audit");
    if (audit && yyjson_mut_is_arr(audit)) return audit;
    audit = yyjson_mut_arr(s->doc);
    yyjson_mut_obj_put(root_of(s), yyjson_mut_strcpy(s->doc, "extension_audit"), audit);
    return audit;
}

void session_record_prompt_audit(tny_session_state *s, const char *submission_id,
                                 const char *submitted, const char *effective, bool blocked,
                                 const char *extension, const char *reason) {
    if (!s || (!blocked && (!submitted || !effective))) return;
    yyjson_mut_val *entry = yyjson_mut_obj(s->doc);
    yyjson_mut_obj_add_strcpy(s->doc, entry, "kind", "prompt");
    yyjson_mut_obj_add_strcpy(s->doc, entry, "id", submission_id ? submission_id : "");
    yyjson_mut_obj_add_bool(s->doc, entry, "blocked", blocked);
    if (!blocked) {
        yyjson_mut_obj_add_strcpy(s->doc, entry, "submitted", submitted);
        yyjson_mut_obj_add_strcpy(s->doc, entry, "effective", effective);
    }
    if (extension && *extension) yyjson_mut_obj_add_strcpy(s->doc, entry, "extension", extension);
    if (reason && *reason) yyjson_mut_obj_add_strcpy(s->doc, entry, "reason", reason);
    yyjson_mut_arr_add_val(extension_audit(s), entry);
}

static yyjson_mut_val *skill_injections(tny_session_state *s) {
    yyjson_mut_val *arr = yyjson_mut_obj_get(root_of(s), "skill_injections");
    if (arr && yyjson_mut_is_arr(arr)) return arr;
    arr = yyjson_mut_arr(s->doc);
    yyjson_mut_obj_put(root_of(s), yyjson_mut_strcpy(s->doc, "skill_injections"), arr);
    return arr;
}

void session_record_skill_injection(tny_session_state *s, int message_index, char **names,
                                    int n_names, const char *display) {
    if (!s || n_names <= 0) return;
    yyjson_mut_val *entry = yyjson_mut_obj(s->doc);
    yyjson_mut_obj_add_int(s->doc, entry, "message", message_index);
    yyjson_mut_val *arr = yyjson_mut_arr(s->doc);
    for (int i = 0; i < n_names; i++) yyjson_mut_arr_add_strcpy(s->doc, arr, names[i]);
    yyjson_mut_obj_put(entry, yyjson_mut_strcpy(s->doc, "skills"), arr);
    if (display) yyjson_mut_obj_add_strcpy(s->doc, entry, "display", display);
    yyjson_mut_arr_add_val(skill_injections(s), entry);
}

bool session_skill_injected(tny_session_state *s, const char *name) {
    if (!s || !name) return false;
    yyjson_mut_val *arr = yyjson_mut_obj_get(root_of(s), "skill_injections");
    if (!arr || !yyjson_mut_is_arr(arr)) return false;
    int boundary = session_compact_boundary(s, NULL);
    size_t idx, max;
    yyjson_mut_val *entry;
    yyjson_mut_arr_foreach(arr, idx, max, entry) {
        if (!entry) break;
        if ((int)yyjson_mut_get_int(yyjson_mut_obj_get(entry, "message")) < boundary) continue;
        yyjson_mut_val *skills = yyjson_mut_obj_get(entry, "skills");
        size_t j, jn;
        yyjson_mut_val *v;
        yyjson_mut_arr_foreach(skills, j, jn, v) {
            const char *n = yyjson_mut_get_str(v);
            if (n && strcmp(n, name) == 0) return true;
        }
    }
    return false;
}

const char *session_message_display(tny_session_state *s, int message_index) {
    if (!s) return NULL;
    yyjson_mut_val *arr = yyjson_mut_obj_get(root_of(s), "skill_injections");
    if (!arr || !yyjson_mut_is_arr(arr)) return NULL;
    size_t idx, max;
    yyjson_mut_val *entry;
    yyjson_mut_arr_foreach(arr, idx, max, entry) {
        if (!entry) break;
        if ((int)yyjson_mut_get_int(yyjson_mut_obj_get(entry, "message")) != message_index)
            continue;
        return yyjson_mut_get_str(yyjson_mut_obj_get(entry, "display"));
    }
    return NULL;
}

void session_replace_tool_arguments(tny_session_state *s, const char *tool_call_id,
                                    const char *arguments_json) {
    if (!s || !tool_call_id || !arguments_json) return;
    yyjson_mut_val *messages = session_messages(s);
    size_t count = yyjson_mut_arr_size(messages);
    size_t i = count;
    while (i) {
        i--;
        yyjson_mut_val *message = yyjson_mut_arr_get(messages, i);
        yyjson_mut_val *calls = yyjson_mut_obj_get(message, "tool_calls");
        if (!yyjson_mut_is_arr(calls)) continue;
        size_t idx, max;
        yyjson_mut_val *call;
        yyjson_mut_arr_foreach(calls, idx, max, call) {
            if (!call) break;
            yyjson_mut_val *id = yyjson_mut_obj_get(call, "id");
            const char *value = id ? yyjson_mut_get_str(id) : NULL;
            if (!value || strcmp(value, tool_call_id) != 0) continue;
            yyjson_mut_val *function = yyjson_mut_obj_get(call, "function");
            if (yyjson_mut_is_obj(function))
                yyjson_mut_obj_put(function, yyjson_mut_strcpy(s->doc, "arguments"),
                                   yyjson_mut_strcpy(s->doc, arguments_json));
            return;
        }
    }
}

void session_record_tool_audit(tny_session_state *s, const char *tool_call_id,
                               const char *tool_name, const char *original_arguments,
                               const char *effective_arguments, const char *control_extension,
                               const char *control_reason, const char *original_result,
                               bool original_ok, const char *effective_result, bool effective_ok,
                               const char *replacement_extension, const char *annotations_json) {
    if (!s) return;
    yyjson_mut_val *entry = yyjson_mut_obj(s->doc);
    yyjson_mut_obj_add_strcpy(s->doc, entry, "kind", "tool");
    yyjson_mut_obj_add_strcpy(s->doc, entry, "id", tool_call_id ? tool_call_id : "");
    yyjson_mut_obj_add_strcpy(s->doc, entry, "tool", tool_name ? tool_name : "tool");
    yyjson_mut_obj_add_strcpy(s->doc, entry, "original_arguments",
                              original_arguments ? original_arguments : "{}");
    yyjson_mut_obj_add_strcpy(s->doc, entry, "effective_arguments",
                              effective_arguments ? effective_arguments : "{}");
    if (control_extension)
        yyjson_mut_obj_add_strcpy(s->doc, entry, "control_extension", control_extension);
    if (control_reason) yyjson_mut_obj_add_strcpy(s->doc, entry, "control_reason", control_reason);
    yyjson_mut_obj_add_strcpy(s->doc, entry, "original_result",
                              original_result ? original_result : "");
    yyjson_mut_obj_add_bool(s->doc, entry, "original_ok", original_ok);
    yyjson_mut_obj_add_strcpy(s->doc, entry, "effective_result",
                              effective_result ? effective_result : "");
    yyjson_mut_obj_add_bool(s->doc, entry, "effective_ok", effective_ok);
    if (replacement_extension)
        yyjson_mut_obj_add_strcpy(s->doc, entry, "replacement_extension", replacement_extension);
    if (annotations_json) {
        yyjson_doc *annotations = jparse(annotations_json, strlen(annotations_json));
        yyjson_val *root = annotations ? yyjson_doc_get_root(annotations) : NULL;
        if (yyjson_is_arr(root)) {
            yyjson_mut_val *copy = yyjson_val_mut_copy(s->doc, root);
            if (copy) yyjson_mut_obj_put(entry, yyjson_mut_strcpy(s->doc, "annotations"), copy);
        }
        yyjson_doc_free(annotations);
    }
    yyjson_mut_arr_add_val(extension_audit(s), entry);
}

char *session_store_result(tny_session_state *s, const char *data, size_t len) {
    if (!s || !data) return NULL;
    char *handle = gen_id();
    if (!handle) return NULL;
    if (s->ctx->no_save) {
        session_mem_result *next =
            realloc(s->mem_results, sizeof(*s->mem_results) * (size_t)(s->n_mem_results + 1));
        if (!next) {
            free(handle);
            return NULL;
        }
        s->mem_results = next;
        session_mem_result *r = &s->mem_results[s->n_mem_results];
        memset(r, 0, sizeof *r);
        char *handle_copy = xstrdup(handle);
        char *data_copy = xstrndup(data, len);
        if (!handle_copy || !data_copy) {
            free(handle_copy);
            free(data_copy);
            free(handle);
            return NULL;
        }
        r->handle = handle_copy;
        r->data = data_copy;
        r->len = len;
        s->n_mem_results++;
        return handle;
    }
    char *rdir = path_join(s->dir, "results");
    if (!rdir || mkdir_p(rdir) != 0) {
        free(rdir);
        free(handle);
        return NULL;
    }
    char *fname = malloc(strlen(handle) + 5);
    if (!fname) {
        free(rdir);
        free(handle);
        return NULL;
    }
    sprintf(fname, "%s.txt", handle);
    char *file = path_join(rdir, fname);
    int rc = file ? file_write_atomic(file, data, len) : -1;
    free(rdir);
    free(fname);
    free(file);
    if (rc != 0) {
        free(handle);
        return NULL;
    }
    return handle;
}

char *session_read_result(tny_session_state *s, const char *handle, size_t off, size_t maxlen,
                          size_t *out_len) {
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
    if (off >= len) {
        free(all);
        *out_len = 0;
        return xstrdup("");
    }
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
    if (!c) {
        if (summary) *summary = NULL;
        return 0;
    }
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
    if (prev) {
        buf_appends(&sum, prev);
        buf_appends(&sum, "\n");
    }
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
                    if (!tc) break;
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
    yyjson_mut_obj_put(c, yyjson_mut_strcpy(s->doc, "summary"),
                       yyjson_mut_strcpy(s->doc, sum.data));
    yyjson_mut_obj_put(root_of(s), yyjson_mut_strcpy(s->doc, "compact"), c);
    buf_free(&sum);
    return 1;
}

/* ---- recovery ---- */

void session_recovery_write(tny_session_state *s, const char *partial) {
    if (s->ctx->no_save) return;
    mkdir_p(s->dir);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(jallocator());
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
    if (!wsdir) return;
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
        session_meta *grown = realloc(*arr, sizeof(session_meta) * (size_t)(*n + 1));
        if (!grown) {
            yyjson_doc_free(doc);
            continue;
        }
        *arr = grown;
        session_meta *m = &(*arr)[(*n)++];
        memset(m, 0, sizeof *m);
        const char *v;
        m->id = xstrdup(e->d_name);
        if ((v = jget_str(r, "title"))) m->title = xstrdup(v);
        if ((v = jget_str(r, "updated"))) m->updated = xstrdup(v);
        if ((v = jget_str(r, "backend"))) m->backend = xstrdup(v);
        if ((v = jget_str(r, "model"))) m->model = xstrdup(v);
        if ((v = jget_str(r, "workspace"))) m->workspace = xstrdup(v);
        if ((v = jget_str(r, "status"))) m->status = xstrdup(v);
        yyjson_val *task = jget(r, "task");
        if (yyjson_is_obj(task)) {
            if ((v = jget_str(task, "name")) && tny_task_name_valid(v)) m->task_name = xstrdup(v);
            if ((v = jget_str(task, "source")) && safe_task_source(v)) m->task_source = xstrdup(v);
            if ((v = jget_str(task, "digest")) && digest_valid(v)) m->task_digest = xstrdup(v);
        }
        m->turns = (int)jget_int(r, "turns", 0);
        buf_t ld;
        buf_init(&ld);
        buf_appendf(&ld, "%s/%s", wsdir, e->d_name);
        m->running = lock_dir_held(ld.data); /* one flock probe per entry */
        buf_free(&ld);
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
        DIR *d = root ? opendir(root) : NULL;
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
            if (strcmp(arr[i].id, cursor) == 0) {
                start = i + 1;
                break;
            }
    }
    if (limit <= 0 || limit > 100) limit = 100;
    if (start > 0) {
        for (int i = 0; i < start; i++) {
            free(arr[i].id);
            free(arr[i].title);
            free(arr[i].updated);
            free(arr[i].backend);
            free(arr[i].model);
            free(arr[i].workspace);
            free(arr[i].status);
            free(arr[i].task_name);
            free(arr[i].task_source);
            free(arr[i].task_digest);
        }
        memmove(arr, arr + start, sizeof *arr * (size_t)(n - start));
        n -= start;
    }
    int m = n > limit ? limit : n;
    for (int i = m; i < n; i++) {
        free(arr[i].id);
        free(arr[i].title);
        free(arr[i].updated);
        free(arr[i].backend);
        free(arr[i].model);
        free(arr[i].workspace);
        free(arr[i].status);
        free(arr[i].task_name);
        free(arr[i].task_source);
        free(arr[i].task_digest);
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
        free(m[i].status);
        free(m[i].task_name);
        free(m[i].task_source);
        free(m[i].task_digest);
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
    if (!data) {
        free(root);
        free(src);
        return NULL;
    }
    /* Trim trailing garbage until it parses (torn write recovery). */
    yyjson_doc *recovered = NULL;
    while (len > 2) {
        recovered = yyjson_read(data, len, 0);
        if (recovered) break;
        len--;
    }
    if (!recovered) {
        free(data);
        free(src);
        free(root);
        return NULL;
    }

    /* A recovered task-bearing session is usable only with its exact private
     * snapshot. Accept the crash-recovery .next spelling by digest, just as
     * session_task_reconcile does, and copy it into the canonical name. */
    char *task_body = NULL;
    size_t task_len = 0;
    yyjson_val *task = jget(yyjson_doc_get_root(recovered), "task");
    if (task) {
        const char *digest = jget_str(task, "digest");
        char *task_src = path_join(src, "task.md");
        task_snapshot_result snapshot = read_task_snapshot(task_src, &task_body, &task_len);
        free(task_src);
        if (!digest_valid(digest) || snapshot != TASK_SNAPSHOT_VALID ||
            !task_snapshot_matches(task_body, task_len, digest)) {
            free(task_body);
            task_body = NULL;
            task_len = 0;
            task_src = path_join(src, "task.md.next");
            snapshot = read_task_snapshot(task_src, &task_body, &task_len);
            free(task_src);
        }
        if (!digest_valid(digest) || snapshot != TASK_SNAPSHOT_VALID ||
            !task_snapshot_matches(task_body, task_len, digest)) {
            free(task_body);
            yyjson_doc_free(recovered);
            free(data);
            free(src);
            free(root);
            return NULL;
        }
    }
    yyjson_doc_free(recovered);

    char *nid = gen_id();
    char *dst = nid ? path_join(root, nid) : NULL;
    char *dfile = dst ? path_join(dst, "session.json") : NULL;
    char *task_dst = task_body && dst ? path_join(dst, "task.md") : NULL;
    if (!nid || !dst || !dfile || (task_body && !task_dst) || mkdir_p(dst) != 0 ||
        (task_body && file_write_atomic(task_dst, task_body, task_len) != 0) ||
        file_write_atomic(dfile, data, len) != 0) {
        if (task_dst) unlink(task_dst);
        if (dfile) unlink(dfile);
        if (dst) rmdir(dst);
        free(nid);
        nid = NULL;
    }
    free(task_dst);
    free(task_body);
    free(data);
    free(dfile);
    free(dst);
    free(src);
    free(root);
    return nid;
}
