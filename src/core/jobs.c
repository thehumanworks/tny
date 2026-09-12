/* jobs.c — durable ask/image jobs (see jobs.h, docs/jobs.md, docs/adr/0093).
 *
 * Layout of one job under <tny_dir>/jobs/<32 hex id>/ (0700):
 *
 *   job.json     0600, version 1, the current attempt's projection
 *   owner.lock   0600, held by the live supervisor for the job's lifetime
 *   state.lock   0600, short nonblocking transactions over job.json
 *   attempt-N.json  0600, immutable snapshot of a finished attempt
 *   attempt-A-item-N.log   0600, immutable per-attempt child stdout, bounded
 *
 * Every state transition happens under state.lock; every liveness question is
 * answered by an actual advisory lock or an actual child handle, never by a
 * stored pid. Nothing here starts a provider connection: items run the real
 * tny CLI as owned children. */
#include "core/jobs.h"
#include "core/image_manifest.h"
#include "core/image_preview.h"
#include "core/perm.h"
#include "util/image_io.h"
#include <limits.h>
#include "core/session.h"
#include "util/jobs_host.h"
#include "util/process.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef __EMSCRIPTEN__
#include <dirent.h>
#include <sys/wait.h>
#endif

extern char **environ;

#define JOBS_STATE_LOCK_TRIES   80
#define JOBS_STATE_LOCK_WAIT_MS 25
#define JOBS_POLL_MS            100
#define JOBS_CANCEL_GRACE_MS    (TNY_PROCESS_CANCEL_GRACE_MS + 1000)
#define JOBS_ENV_API_KEY        "TNY_JOB_API_KEY"
#define JOBS_ENV_BASE_URL       "TNY_JOB_BASE_URL"
#define JOBS_MAX_RESERVATIONS   8
#define JOBS_LIST_MAX           200

static const char *const JOB_STATES[] = {"queued", "running",   "succeeded",
                                         "failed", "cancelled", "interrupted"};

/* ---------------------------------------------------------------- helpers */

bool tny_jobs_valid_id(const char *id) {
    if (!id) return false;
    size_t n = strlen(id);
    if (n != TNY_JOBS_ID_LEN) return false;
    for (size_t i = 0; i < n; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    return true;
}

bool tny_jobs_execution_supported(void) { return tny_jobs_host_execution_supported(); }

tny_jobs_op tny_jobs_op_parse(const char *name) {
    if (!name) return TNY_JOBS_OP_NONE;
    static const char *const names[] = {"submit", "status", "wait", "cancel",
                                        "retry",  "logs",   "list", "rm"};
    for (int i = 0; i < (int)(sizeof names / sizeof names[0]); i++)
        if (strcmp(name, names[i]) == 0) return (tny_jobs_op)i;
    return TNY_JOBS_OP_NONE;
}

const char *tny_jobs_op_name(tny_jobs_op op) {
    switch (op) {
    case TNY_JOBS_OP_SUBMIT: return "submit";
    case TNY_JOBS_OP_STATUS: return "status";
    case TNY_JOBS_OP_WAIT: return "wait";
    case TNY_JOBS_OP_CANCEL: return "cancel";
    case TNY_JOBS_OP_RETRY: return "retry";
    case TNY_JOBS_OP_LOGS: return "logs";
    case TNY_JOBS_OP_LIST: return "list";
    case TNY_JOBS_OP_RM: return "rm";
    case TNY_JOBS_OP_NONE: break;
    }
    return "none";
}

bool tny_jobs_op_is_sensitive(tny_jobs_op op) {
    return op == TNY_JOBS_OP_SUBMIT || op == TNY_JOBS_OP_CANCEL || op == TNY_JOBS_OP_RETRY ||
           op == TNY_JOBS_OP_RM;
}

const char *tny_jobs_permission_tool(tny_jobs_op op) {
    switch (op) {
    case TNY_JOBS_OP_SUBMIT: return "job_submit";
    case TNY_JOBS_OP_CANCEL: return "job_cancel";
    case TNY_JOBS_OP_RETRY: return "job_retry";
    case TNY_JOBS_OP_RM: return "job_rm";
    default: break;
    }
    return "job_status"; /* status, wait, logs, list: read-only identity */
}

static void hex_of(const uint8_t *in, size_t n, char *out) {
    static const char *digits = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = digits[in[i] >> 4];
        out[i * 2 + 1] = digits[in[i] & 0xf];
    }
    out[n * 2] = 0;
}

static char *sha256_hex_of(const void *data, size_t len) {
    uint8_t digest[32];
    if (!sha256((const uint8_t *)data, len, digest)) return NULL;
    char *hex = calloc(1, 65);
    if (!hex) return NULL;
    hex_of(digest, sizeof digest, hex);
    return hex;
}

static char *sha256_hex_file(const char *path, size_t *len_out) {
    size_t len = 0;
    char *data = file_slurp(path, &len);
    if (!data) return NULL;
    char *hex = sha256_hex_of(data, len);
    free(data);
    if (len_out) *len_out = len;
    return hex;
}

static char *jobs_root(tny_ctx *ctx) { return path_join(ctx->tny_dir, "jobs"); }

static char *jobs_dir(tny_ctx *ctx, const char *id) {
    char *root = jobs_root(ctx);
    char *dir = root && tny_jobs_valid_id(id) ? path_join(root, id) : NULL;
    free(root);
    return dir;
}

static char *jobs_file(const char *dir, const char *name) { return path_join(dir, name); }

static char *jobs_item_log(const char *dir, int index, int attempt) {
    char name[64];
    snprintf(name, sizeof name, "attempt-%d-item-%d.log", attempt, index);
    return path_join(dir, name);
}

static void safe_err(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void safe_err(char *err, size_t errlen, const char *fmt, ...) {
    if (!err || !errlen) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static const char *artifact_string(yyjson_val *, const char *, size_t);

/* ---- mutable-document accessors (job.json is read-modify-written) ---- */

static void jm_set_str(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                       const char *value) {
    yyjson_mut_val *v = value ? yyjson_mut_strcpy(doc, value) : yyjson_mut_null(doc);
    yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc, key), v);
}

static void jm_set_int(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key, int64_t value) {
    yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc, key), yyjson_mut_sint(doc, value));
}

static void jm_set_null(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key) {
    yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc, key), yyjson_mut_null(doc));
}

static void jm_set_bool(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key, bool value) {
    yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc, key), yyjson_mut_bool(doc, value));
}

static const char *jm_str(yyjson_mut_val *obj, const char *key) {
    return yyjson_mut_get_str(yyjson_mut_obj_get(obj, key));
}

static int64_t jm_int(yyjson_mut_val *obj, const char *key, int64_t dflt) {
    yyjson_mut_val *v = yyjson_mut_obj_get(obj, key);
    return v && yyjson_mut_is_int(v) ? yyjson_mut_get_sint(v) : dflt;
}

static bool jm_bool(yyjson_mut_val *obj, const char *key, bool dflt) {
    yyjson_mut_val *v = yyjson_mut_obj_get(obj, key);
    return v && yyjson_mut_is_bool(v) ? yyjson_mut_get_bool(v) : dflt;
}

static yyjson_mut_val *jm_items(yyjson_mut_doc *doc) {
    yyjson_mut_val *items = yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc), "items");
    return items && yyjson_mut_is_arr(items) ? items : NULL;
}

static yyjson_mut_val *jm_item(yyjson_mut_doc *doc, int index) {
    yyjson_mut_val *items = jm_items(doc);
    return items ? yyjson_mut_arr_get(items, (size_t)index) : NULL;
}

static int jm_item_count(yyjson_mut_doc *doc) {
    yyjson_mut_val *items = jm_items(doc);
    return items ? (int)yyjson_mut_arr_size(items) : 0;
}

static bool state_is_terminal(const char *state) {
    return state && (strcmp(state, "succeeded") == 0 || strcmp(state, "failed") == 0 ||
                     strcmp(state, "cancelled") == 0 || strcmp(state, "interrupted") == 0);
}

static bool state_is_known(const char *state) {
    if (!state) return false;
    for (size_t i = 0; i < sizeof JOB_STATES / sizeof JOB_STATES[0]; i++)
        if (strcmp(state, JOB_STATES[i]) == 0) return true;
    return false;
}

/* ---- loading and validating a record (fails closed) ---- */

static bool record_state_is_known(yyjson_val *state) {
    const char *text = yyjson_get_str(state);
    return text && strlen(text) == yyjson_get_len(state) && state_is_known(text);
}

static yyjson_mut_doc *jobs_record_load(const char *dir, const char *id, char *err, size_t errlen) {
    char *path = jobs_file(dir, "job.json");
    if (!path) return NULL;
    size_t len = 0;
    char *data = file_slurp(path, &len);
    free(path);
    if (!data) {
        safe_err(err, errlen, "no job record");
        return NULL;
    }
    if (len > TNY_JOBS_PAYLOAD_MAX) {
        free(data);
        safe_err(err, errlen, "the job record is too large");
        return NULL;
    }
    yyjson_doc *doc = jparse(data, len);
    free(data);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        safe_err(err, errlen, "the job record is not a JSON object");
        return NULL;
    }
    int64_t version = jget_int(root, "version", 0);
    const char *kind = jget_str(root, "kind");
    const char *record_id = jget_str(root, "id");
    yyjson_val *items = jget(root, "items");
    bool ok = version == TNY_JOBS_SCHEMA_VERSION && kind && strcmp(kind, "job") == 0 && record_id &&
              tny_jobs_valid_id(record_id) && (!id || strcmp(record_id, id) == 0) && items &&
              yyjson_is_arr(items) && yyjson_arr_size(items) <= TNY_JOBS_MAX_ITEMS &&
              record_state_is_known(jget(root, "state"));
    if (ok) {
        size_t idx, max;
        yyjson_val *item;
        yyjson_arr_foreach(items, idx, max, item) {
            if (!yyjson_is_obj(item) || jget_int(item, "index", -1) != (int64_t)idx ||
                !record_state_is_known(jget(item, "state")))
                ok = false;
        }
    }
    if (!ok) {
        yyjson_doc_free(doc);
        safe_err(err, errlen, "the job record is not a supported version 1 record");
        return NULL;
    }
    yyjson_mut_doc *mut = yyjson_doc_mut_copy(doc, jallocator());
    yyjson_doc_free(doc);
    if (!mut) safe_err(err, errlen, "out of memory");
    return mut;
}

static int jobs_record_store(const char *dir, yyjson_mut_doc *doc) {
    char *json = jwrite_pretty(doc);
    if (!json) return ENOMEM;
    char *path = jobs_file(dir, "job.json");
    int rc = path ? tny_jobs_host_write_private(path, json, strlen(json)) : ENOMEM;
    free(path);
    free(json);
    return rc;
}

/* ---- state transactions ---- */

typedef struct {
    char *dir;
    int lock_fd;
    yyjson_mut_doc *doc;
} jobs_txn;

static void jobs_txn_end(jobs_txn *t) {
    if (!t) return;
    if (t->doc) yyjson_mut_doc_free(t->doc);
    if (t->lock_fd >= 0) tny_jobs_host_lock_close(t->lock_fd);
    free(t->dir);
    memset(t, 0, sizeof *t);
    t->lock_fd = -1;
}

/* Bounded, never blocking: the state lock is only ever held for a few
 * in-memory edits plus one atomic write. */
static int jobs_txn_begin(const char *dir, const char *id, jobs_txn *t, char *err, size_t errlen) {
    memset(t, 0, sizeof *t);
    t->lock_fd = -1;
    char *lock = jobs_file(dir, "state.lock");
    if (!lock) return ENOMEM;
    int fd = tny_jobs_host_lock_open(lock);
    free(lock);
    if (fd < 0) {
        safe_err(err, errlen, "cannot open the job state lock");
        return EIO;
    }
    tny_jobs_lock_rc rc = TNY_JOBS_LOCK_BUSY;
    for (int i = 0; i < JOBS_STATE_LOCK_TRIES; i++) {
        rc = tny_jobs_host_lock_try(fd);
        if (rc != TNY_JOBS_LOCK_BUSY) break;
        tny_jobs_host_sleep_ms(JOBS_STATE_LOCK_WAIT_MS);
    }
    if (rc != TNY_JOBS_LOCK_ACQUIRED) {
        tny_jobs_host_lock_close(fd);
        safe_err(err, errlen, "another process is updating this job");
        return EBUSY;
    }
    t->lock_fd = fd;
    t->dir = xstrdup(dir);
    t->doc = jobs_record_load(dir, id, err, errlen);
    if (!t->dir || !t->doc) {
        jobs_txn_end(t);
        return EINVAL;
    }
    return 0;
}

static int jobs_txn_commit(jobs_txn *t) {
    yyjson_mut_val *root = yyjson_mut_doc_get_root(t->doc);
    jm_set_int(t->doc, root, "revision", jm_int(root, "revision", 0) + 1);
    char *now = now_iso8601();
    jm_set_str(t->doc, root, "updated", now ? now : "");
    free(now);
    int rc = jobs_record_store(t->dir, t->doc);
    jobs_txn_end(t);
    return rc;
}

/* ---------------------------------------------------- output reservations */

/* A reservation lock file holds a bounded list of claims, one per canonical
 * output path that hashes into it. The lock is held continuously across the
 * probe of any referenced owner, the owner's state read and the rewrite — and
 * is released before any provider work (A11, ADR 0093). */

static char *reservation_path(tny_ctx *ctx, const char *canonical) {
    char *root = jobs_root(ctx);
    if (!root) return NULL;
    char *dir = path_join(root, "reservations");
    free(root);
    if (!dir) return NULL;
    if (tny_jobs_host_mkdir_private(dir) != 0) {
        free(dir);
        return NULL;
    }
    uint8_t digest[32];
    char name[80];
    if (sha256((const uint8_t *)canonical, strlen(canonical), digest)) {
        char hex[65];
        hex_of(digest, 16, hex);
        snprintf(name, sizeof name, "%s.lock", hex);
    } else {
        snprintf(name, sizeof name, "%016llx.lock",
                 (unsigned long long)fnv1a(canonical, strlen(canonical)));
    }
    char *path = path_join(dir, name);
    free(dir);
    return path;
}

static yyjson_mut_doc *reservation_read(int fd) {
    char buf[8192];
    ssize_t n = 0;
    if (lseek(fd, 0, SEEK_SET) == 0) n = read(fd, buf, sizeof buf - 1);
    yyjson_mut_doc *mut = NULL;
    if (n > 0) {
        yyjson_doc *doc = jparse(buf, (size_t)n);
        yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
        if (root && yyjson_is_obj(root) && jget_int(root, "version", 0) == 1)
            mut = yyjson_doc_mut_copy(doc, jallocator());
        yyjson_doc_free(doc);
    }
    if (!mut) {
        mut = yyjson_mut_doc_new(jallocator());
        yyjson_mut_val *root = mut ? yyjson_mut_obj(mut) : NULL;
        if (!root) {
            yyjson_mut_doc_free(mut);
            return NULL;
        }
        yyjson_mut_doc_set_root(mut, root);
        jm_set_int(mut, root, "version", 1);
        yyjson_mut_obj_put(root, yyjson_mut_strcpy(mut, "claims"), yyjson_mut_arr(mut));
    }
    return mut;
}

static int reservation_write(int fd, yyjson_mut_doc *doc) {
    char *json = jwrite(doc);
    if (!json) return ENOMEM;
    size_t len = strlen(json);
    int rc = 0;
    if (lseek(fd, 0, SEEK_SET) != 0 || ftruncate(fd, 0) != 0) rc = errno;
    for (size_t off = 0; !rc && off < len;) {
        ssize_t n = write(fd, json + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            rc = n < 0 ? errno : EIO;
            break;
        }
        off += (size_t)n;
    }
    free(json);
    return rc;
}

/* Caller already establishes actual owner freedom and holds the state lock.
 * An observed cleanup failure is not an A11 owner-loss projection (ADR 0101). */
static bool cleanup_reclaimable(yyjson_mut_val *root) {
    yyjson_mut_val *hold = root ? yyjson_mut_obj_get(root, "cleanup_hold") : NULL;
    if (!root || (hold && (!yyjson_mut_is_bool(hold) || yyjson_mut_get_bool(hold)))) return false;
    yyjson_mut_val *state = yyjson_mut_obj_get(root, "state");
    yyjson_mut_val *cleanup = yyjson_mut_obj_get(root, "cleanup");
    yyjson_mut_val *code = yyjson_mut_obj_get(root, "error_code");
    if (yyjson_mut_equals_str(cleanup, "complete"))
        return yyjson_mut_equals_str(state, "succeeded") ||
               yyjson_mut_equals_str(state, "failed") ||
               yyjson_mut_equals_str(state, "cancelled") ||
               yyjson_mut_equals_str(state, "interrupted");
    /* The original A11 abandoned queued/running claim remains reclaimable.
     * A persisted unknown result, however, needs the exact projection tuple. */
    if (yyjson_mut_equals_str(cleanup, "pending"))
        return yyjson_mut_equals_str(state, "queued") || yyjson_mut_equals_str(state, "running");
    return yyjson_mut_equals_str(state, "interrupted") &&
           yyjson_mut_equals_str(cleanup, "unknown") &&
           yyjson_mut_equals_str(code, TNY_JOBS_CODE_INTERRUPTED);
}

/* The reservation lock stays held throughout this nonblocking owner/state
 * inspection and the subsequent claim update. Unknown never means free. */
static bool reservation_owner_active(tny_ctx *ctx, const char *job_id) {
    if (!job_id || !tny_jobs_valid_id(job_id)) return true;
    char *dir = jobs_dir(ctx, job_id);
    if (!dir) return true;
    char *owner = jobs_file(dir, "owner.lock");
    tny_jobs_owner_state owner_state =
        owner ? tny_jobs_host_owner_state(owner) : TNY_JOBS_OWNER_UNKNOWN;
    bool active = true;
    if (owner_state == TNY_JOBS_OWNER_FREE) {
        char *lock = jobs_file(dir, "state.lock");
        int fd = lock ? tny_jobs_host_lock_open(lock) : -1;
        free(lock);
        if (fd >= 0 && tny_jobs_host_lock_try(fd) == TNY_JOBS_LOCK_ACQUIRED) {
            /* Recheck ownership under the state lock and inspect raw bytes;
             * do not project or rewrite an uncertain record while claiming. */
            if (tny_jobs_host_owner_state(owner) == TNY_JOBS_OWNER_FREE) {
                yyjson_mut_doc *doc = jobs_record_load(dir, job_id, NULL, 0);
                active = !doc || !cleanup_reclaimable(yyjson_mut_doc_get_root(doc));
                yyjson_mut_doc_free(doc);
            }
            tny_jobs_host_lock_release(fd);
        }
        if (fd >= 0) tny_jobs_host_lock_close(fd);
    }
    free(owner);
    free(dir);
    return active;
}

typedef struct {
    char *path; /* canonical output */
} reservation_claim;

/* Claim one canonical output for (job,item,attempt). 0 ok, EBUSY when another
 * live job owns it, else an errno. */
static int reservation_claim_one(tny_ctx *ctx, const char *canonical, const char *job_id, int item,
                                 int attempt, char *err, size_t errlen) {
    char *path = reservation_path(ctx, canonical);
    if (!path) return ENOMEM;
    int fd = tny_jobs_host_lock_open(path);
    free(path);
    if (fd < 0) return EIO;
    int rc = 0;
    if (tny_jobs_host_lock_try(fd) != TNY_JOBS_LOCK_ACQUIRED) {
        tny_jobs_host_lock_close(fd);
        safe_err(err, errlen, "another submitter is claiming this output right now");
        return EBUSY;
    }
    yyjson_mut_doc *doc = reservation_read(fd);
    yyjson_mut_val *claims =
        doc ? yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc), "claims") : NULL;
    if (!claims) rc = ENOMEM;
    yyjson_mut_doc *fresh = rc ? NULL : yyjson_mut_doc_new(jallocator());
    yyjson_mut_val *kept = fresh ? yyjson_mut_arr(fresh) : NULL;
    if (!rc && !kept) rc = ENOMEM;
    if (!rc) {
        yyjson_mut_doc_set_root(fresh, yyjson_mut_obj(fresh));
        yyjson_mut_val *root = yyjson_mut_doc_get_root(fresh);
        jm_set_int(fresh, root, "version", 1);
        yyjson_mut_obj_put(root, yyjson_mut_strcpy(fresh, "claims"), kept);
        size_t idx, max;
        yyjson_mut_val *claim;
        yyjson_mut_arr_foreach(claims, idx, max, claim) {
            const char *claim_path = jm_str(claim, "path");
            const char *claim_job = jm_str(claim, "job");
            if (!claim_path || !claim_job) {
                rc = EINVAL;
                safe_err(err, errlen, "the output reservation could not be verified");
                break;
            }
            bool same_output = strcmp(claim_path, canonical) == 0;
            bool active = reservation_owner_active(ctx, claim_job);
            if (same_output && active && strcmp(claim_job, job_id) != 0) {
                rc = EBUSY;
                safe_err(err, errlen, "another job already owns this output path");
                break;
            }
            if (same_output) continue; /* superseded by this claim */
            if (!active) continue;     /* abandoned claim for another path */
            if (yyjson_mut_arr_size(kept) >= JOBS_MAX_RESERVATIONS) continue;
            yyjson_mut_arr_append(kept, yyjson_mut_val_mut_copy(fresh, claim));
        }
    }
    if (!rc) {
        yyjson_mut_val *mine = yyjson_mut_obj(fresh);
        jm_set_str(fresh, mine, "path", canonical);
        jm_set_str(fresh, mine, "job", job_id);
        jm_set_int(fresh, mine, "item", item);
        jm_set_int(fresh, mine, "attempt", attempt);
        yyjson_mut_arr_append(kept, mine);
        rc = reservation_write(fd, fresh);
    }
    yyjson_mut_doc_free(fresh);
    yyjson_mut_doc_free(doc);
    tny_jobs_host_lock_release(fd);
    tny_jobs_host_lock_close(fd);
    return rc;
}

/* Drop this job's own claim, comparing the opaque job/item/attempt identity so
 * a later attempt's or another job's record is never touched. */
static void reservation_release_one(tny_ctx *ctx, const char *canonical, const char *job_id,
                                    int item, int attempt) {
    char *path = reservation_path(ctx, canonical);
    if (!path) return;
    int fd = tny_jobs_host_lock_open(path);
    free(path);
    if (fd < 0) return;
    if (tny_jobs_host_lock_try(fd) != TNY_JOBS_LOCK_ACQUIRED) {
        tny_jobs_host_lock_close(fd);
        return; /* a live claimer owns the record; never force it */
    }
    yyjson_mut_doc *doc = reservation_read(fd);
    yyjson_mut_val *claims =
        doc ? yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc), "claims") : NULL;
    yyjson_mut_doc *fresh = claims ? yyjson_mut_doc_new(jallocator()) : NULL;
    yyjson_mut_val *kept = fresh ? yyjson_mut_arr(fresh) : NULL;
    if (kept) {
        yyjson_mut_doc_set_root(fresh, yyjson_mut_obj(fresh));
        yyjson_mut_val *root = yyjson_mut_doc_get_root(fresh);
        jm_set_int(fresh, root, "version", 1);
        yyjson_mut_obj_put(root, yyjson_mut_strcpy(fresh, "claims"), kept);
        size_t idx, max;
        yyjson_mut_val *claim;
        yyjson_mut_arr_foreach(claims, idx, max, claim) {
            const char *claim_path = jm_str(claim, "path");
            const char *claim_job = jm_str(claim, "job");
            bool mine = claim_path && claim_job && strcmp(claim_path, canonical) == 0 &&
                        strcmp(claim_job, job_id) == 0 && jm_int(claim, "item", -1) == item &&
                        jm_int(claim, "attempt", -1) == attempt;
            if (!mine && yyjson_mut_arr_size(kept) < JOBS_MAX_RESERVATIONS)
                yyjson_mut_arr_append(kept, yyjson_mut_val_mut_copy(fresh, claim));
        }
        reservation_write(fd, fresh);
    }
    yyjson_mut_doc_free(fresh);
    yyjson_mut_doc_free(doc);
    tny_jobs_host_lock_release(fd);
    tny_jobs_host_lock_close(fd);
}

/* Claim every output of one attempt in sorted canonical order (no lock
 * inversion), rolling back this submitter's own claims on the first refusal. */
static int reservations_claim_all(tny_ctx *ctx, const char *job_id, int attempt,
                                  reservation_claim *claims, const int *items, int n, char *err,
                                  size_t errlen) {
    for (int i = 1; i < n; i++) { /* insertion sort: n <= 64 */
        reservation_claim claim = claims[i];
        int item = items[i];
        int j = i - 1;
        while (j >= 0 && strcmp(claims[j].path, claim.path) > 0) {
            claims[j + 1] = claims[j];
            ((int *)items)[j + 1] = items[j];
            j--;
        }
        claims[j + 1] = claim;
        ((int *)items)[j + 1] = item;
    }
    for (int i = 0; i < n; i++) {
        int rc = reservation_claim_one(ctx, claims[i].path, job_id, items[i], attempt, err, errlen);
        if (rc) {
            for (int j = 0; j < i; j++)
                reservation_release_one(ctx, claims[j].path, job_id, items[j], attempt);
            return rc;
        }
    }
    return 0;
}

/* Release every output claim recorded for this job's current attempt. */
static void reservations_release_job(tny_ctx *ctx, yyjson_mut_doc *doc, const char *job_id) {
    int attempt = (int)jm_int(yyjson_mut_doc_get_root(doc), "attempt", 1);
    int count = jm_item_count(doc);
    for (int i = 0; i < count; i++) {
        yyjson_mut_val *item = jm_item(doc, i);
        const char *canonical = jm_str(item, "output_path");
        if (canonical) reservation_release_one(ctx, canonical, job_id, i, attempt);
    }
}

/* ------------------------------------------------------- request validation */

typedef struct {
    bool image;
    int concurrency;
    int n_items;
    /* per item, borrowed from the caller's parsed arguments */
    yyjson_val *items[TNY_JOBS_MAX_ITEMS];
    char *outputs[TNY_JOBS_MAX_ITEMS]; /* canonical image outputs, else NULL */
} jobs_request;

static void jobs_request_free(jobs_request *r) {
    for (int i = 0; i < r->n_items; i++) free(r->outputs[i]);
    memset(r, 0, sizeof *r);
}

static bool bounded_string(yyjson_val *value, size_t max) {
    if (!yyjson_is_str(value)) return false;
    size_t len = yyjson_get_len(value);
    const char *text = yyjson_get_str(value);
    return len && len <= max && strlen(text) == len && utf8_valid_bytes(text, len);
}

static int validate_ask_item(yyjson_val *item, char *err, size_t errlen) {
    if (!bounded_string(jget(item, "prompt"), TNY_JOBS_PROMPT_MAX)) {
        safe_err(err, errlen, "each ask item needs a nonempty UTF-8 prompt");
        return -1;
    }
    static const char *const optional[] = {"model", "effort", "task", "provider"};
    for (size_t i = 0; i < sizeof optional / sizeof optional[0]; i++) {
        yyjson_val *v = jget(item, optional[i]);
        /* An explicit null is "not set": replayed stored requests carry them. */
        if (v && !yyjson_is_null(v) && !bounded_string(v, 256)) {
            safe_err(err, errlen, "ask item %s must be a short string", optional[i]);
            return -1;
        }
    }
    return 0;
}

static int validate_image_item(tny_ctx *ctx, yyjson_val *item, char **canonical_out, char *err,
                               size_t errlen) {
    (void)ctx;
    *canonical_out = NULL;
    if (!bounded_string(jget(item, "prompt"), TNY_JOBS_PROMPT_MAX)) {
        safe_err(err, errlen, "each image item needs a nonempty UTF-8 prompt");
        return -1;
    }
    const char *operation = jget_str(item, "operation");
    if (operation && strcmp(operation, "generate") != 0 && strcmp(operation, "edit") != 0) {
        safe_err(err, errlen, "image operation must be generate or edit");
        return -1;
    }
    yyjson_val *output = jget(item, "output_file");
    if (!bounded_string(output, 3000)) {
        safe_err(err, errlen, "each image item needs an output_file path");
        return -1;
    }
    static const char *const optional[] = {"model", "quality", "size", "provider"};
    for (size_t i = 0; i < sizeof optional / sizeof optional[0]; i++) {
        yyjson_val *v = jget(item, optional[i]);
        if (v && !yyjson_is_null(v) && !bounded_string(v, 256)) {
            safe_err(err, errlen, "image item %s must be a short string", optional[i]);
            return -1;
        }
    }
    yyjson_val *persist_manifest = jget(item, "persist_manifest");
    if (persist_manifest && !yyjson_is_bool(persist_manifest)) {
        safe_err(err, errlen, "image item persist_manifest must be boolean");
        return -1;
    }
    yyjson_val *images = jget(item, "images");
    if (images && yyjson_is_null(images)) images = NULL;
    size_t n_images = images && yyjson_is_arr(images) ? yyjson_arr_size(images) : 0;
    if (images && (!yyjson_is_arr(images) || n_images > TNY_IMAGE_REFS_PER_ITEM)) {
        safe_err(err, errlen, "image item images must be at most 5 paths");
        return -1;
    }
    if (images) {
        size_t idx, max;
        yyjson_val *reference;
        yyjson_arr_foreach(images, idx, max, reference) {
            if (!bounded_string(reference, 3000)) {
                safe_err(err, errlen, "each image reference must be a path");
                return -1;
            }
        }
    }
    if (operation && strcmp(operation, "edit") == 0 && n_images == 0) {
        safe_err(err, errlen, "an image edit item needs at least one reference image");
        return -1;
    }
    bool alias = false;
    const char *reason = NULL;
    char *canonical = tny_jobs_host_canonical_output(yyjson_get_str(output), &alias, &reason);
    if (!canonical) {
        safe_err(err, errlen, "%s", reason ? reason : "invalid output path");
        return -1;
    }
    /* A new job's destination is absent by default; replacing an existing file
     * needs an explicit grant on this request. */
    if (file_exists(canonical) && !jget_bool(item, "overwrite", false)) {
        free(canonical);
        safe_err(err, errlen, "the output path already exists; pass overwrite to replace it");
        return -1;
    }
    *canonical_out = canonical;
    return 0;
}

/* Copy an item's reference paths, resolved to absolute, into a mutable array. */
static yyjson_mut_val *refs_copy(yyjson_mut_doc *doc, yyjson_val *images) {
    yyjson_mut_val *refs = yyjson_mut_arr(doc);
    if (!refs || !images || !yyjson_is_arr(images)) return refs;
    size_t idx, max;
    yyjson_val *reference;
    yyjson_arr_foreach(images, idx, max, reference) {
        const char *path = yyjson_get_str(reference);
        char *resolved = path ? path_abs(path) : NULL;
        if (resolved) yyjson_mut_arr_append(refs, yyjson_mut_strcpy(doc, resolved));
        free(resolved);
    }
    return refs;
}

static int jobs_request_parse(tny_ctx *ctx, yyjson_val *args, jobs_request *r, char *err,
                              size_t errlen) {
    memset(r, 0, sizeof *r);
    const char *kind = jget_str(args, "kind");
    if (!kind || (strcmp(kind, "ask") != 0 && strcmp(kind, "image") != 0)) {
        safe_err(err, errlen, "kind must be ask or image");
        return -1;
    }
    r->image = strcmp(kind, "image") == 0;
    int64_t concurrency = jget_int(args, "concurrency", TNY_JOBS_DEFAULT_CONCURRENCY);
    if (concurrency < 1 || concurrency > TNY_JOBS_MAX_CONCURRENCY) {
        safe_err(err, errlen, "concurrency must be between 1 and %d", TNY_JOBS_MAX_CONCURRENCY);
        return -1;
    }
    r->concurrency = (int)concurrency;
    yyjson_val *items = jget(args, "items");
    if (!items || !yyjson_is_arr(items) || !yyjson_arr_size(items) ||
        yyjson_arr_size(items) > TNY_JOBS_MAX_ITEMS) {
        safe_err(err, errlen, "items must be a list of 1 to %d entries", TNY_JOBS_MAX_ITEMS);
        return -1;
    }
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(items, idx, max, item) {
        if (!yyjson_is_obj(item)) {
            safe_err(err, errlen, "each item must be a JSON object");
            jobs_request_free(r);
            return -1;
        }
        const char *item_kind = jget_str(item, "kind");
        if (item_kind && strcmp(item_kind, kind) != 0) {
            safe_err(err, errlen, "every item in a batch must have the same kind");
            jobs_request_free(r);
            return -1;
        }
        r->items[r->n_items] = item;
        if (r->image) {
            if (validate_image_item(ctx, item, &r->outputs[r->n_items], err, errlen) != 0) {
                jobs_request_free(r);
                return -1;
            }
            for (int j = 0; j < r->n_items; j++)
                if (r->outputs[j] && strcmp(r->outputs[j], r->outputs[r->n_items]) == 0) {
                    safe_err(err, errlen, "two items in this batch write the same output path");
                    free(r->outputs[r->n_items]);
                    r->outputs[r->n_items] = NULL;
                    jobs_request_free(r);
                    return -1;
                }
        } else if (validate_ask_item(item, err, errlen) != 0) {
            jobs_request_free(r);
            return -1;
        }
        r->n_items++;
    }
    return 0;
}

/* A retry replays the stored requests of exactly the selected items. Items
 * that are carried forward are placeholders: they are never validated against
 * a provider, never re-executed and keep their recorded artifacts. */
static int jobs_request_parse_retry(tny_ctx *ctx, yyjson_val *args, jobs_request *r,
                                    const int *selected, int n_selected, char *err, size_t errlen) {
    memset(r, 0, sizeof *r);
    const char *kind = jget_str(args, "kind");
    if (!kind || (strcmp(kind, "ask") != 0 && strcmp(kind, "image") != 0)) {
        safe_err(err, errlen, "the stored job kind is not supported");
        return -1;
    }
    r->image = strcmp(kind, "image") == 0;
    int64_t concurrency = jget_int(args, "concurrency", TNY_JOBS_DEFAULT_CONCURRENCY);
    r->concurrency = concurrency >= 1 && concurrency <= TNY_JOBS_MAX_CONCURRENCY
                         ? (int)concurrency
                         : TNY_JOBS_DEFAULT_CONCURRENCY;
    yyjson_val *items = jget(args, "items");
    if (!items || !yyjson_is_arr(items) || yyjson_arr_size(items) > TNY_JOBS_MAX_ITEMS) {
        safe_err(err, errlen, "the stored item list is not usable");
        return -1;
    }
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(items, idx, max, item) {
        int index = (int)idx;
        bool chosen = false;
        for (int k = 0; k < n_selected; k++)
            if (selected[k] == index) chosen = true;
        r->items[index] = chosen ? item : NULL;
        r->outputs[index] = NULL;
        r->n_items = index + 1;
        if (!chosen) continue;
        if (r->image) {
            if (validate_image_item(ctx, item, &r->outputs[index], err, errlen) != 0) {
                jobs_request_free(r);
                return -1;
            }
        } else if (validate_ask_item(item, err, errlen) != 0) {
            jobs_request_free(r);
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------- permission detail string */

char *tny_jobs_detail(tny_ctx *ctx, tny_jobs_op op, yyjson_val *args, char **error) {
    if (error) *error = NULL;
    buf_t d;
    buf_init(&d);
    buf_appends(&d, tny_jobs_op_name(op));
    if (op == TNY_JOBS_OP_SUBMIT) {
        jobs_request request;
        char err[256] = "";
        if (jobs_request_parse(ctx, args, &request, err, sizeof err) != 0) {
            buf_free(&d);
            if (error) {
                buf_t e;
                buf_init(&e);
                buf_appendf(&e, "%s", err);
                *error = buf_detach(&e);
            }
            return NULL;
        }
        buf_appendf(&d, " kind=%s items=%d concurrency=%d", request.image ? "image" : "ask",
                    request.n_items, request.concurrency);
        buf_appendf(&d, " provider=%s", tny_provider_name(ctx));
        for (int i = 0; i < request.n_items; i++) {
            if (request.outputs[i]) buf_appendf(&d, " output=%s", request.outputs[i]);
            if (jget_bool(request.items[i], "overwrite", false)) buf_appends(&d, " overwrite");
            if (request.image && !jget_bool(request.items[i], "persist_manifest", true))
                buf_appends(&d, " no_manifest");
            yyjson_val *images = jget(request.items[i], "images");
            if (!images || !yyjson_is_arr(images)) continue;
            size_t idx, max;
            yyjson_val *reference;
            yyjson_arr_foreach(images, idx, max, reference) {
                const char *path = yyjson_get_str(reference);
                char *resolved = path ? path_abs(path) : NULL;
                buf_appendf(&d, " ref=%s", resolved ? resolved : (path ? path : ""));
                free(resolved);
            }
        }
        /* A digest of the exact request body, so a grant cannot silently
         * carry over to different prompts or settings. No prompt text. */
        char *body = jwrite_val(args);
        char *digest = body ? sha256_hex_of(body, strlen(body)) : NULL;
        if (digest) buf_appendf(&d, " request=%.16s", digest);
        free(digest);
        free(body);
        jobs_request_free(&request);
    } else if (op != TNY_JOBS_OP_LIST) {
        const char *id = jget_str(args, "id");
        if (!tny_jobs_valid_id(id)) {
            buf_free(&d);
            if (error) {
                buf_t e;
                buf_init(&e);
                buf_appends(&e, "a 32-character lowercase hex job id is required");
                *error = buf_detach(&e);
            }
            return NULL;
        }
        buf_appendf(&d, " id=%s", id);
        yyjson_val *items = jget(args, "items");
        if (items && yyjson_is_arr(items)) {
            size_t idx, max;
            yyjson_val *entry;
            yyjson_arr_foreach(items, idx, max, entry) {
                if (yyjson_is_int(entry))
                    buf_appendf(&d, " item=%lld", (long long)yyjson_get_sint(entry));
            }
        }
        if (jget(args, "item")) buf_appendf(&d, " item=%lld", (long long)jget_int(args, "item", 0));
        if (jget_bool(args, "failed", false)) buf_appends(&d, " failed");
    }
    return buf_detach(&d);
}

/* ------------------------------------------------------------ argv grammar */

typedef struct {
    const char *prompt, *model, *effort, *task, *quality, *size, *output_file, *request;
    const char *images[TNY_IMAGE_REFS_PER_ITEM];
    int n_images;
    const char *items, *timeout, *concurrency, *max_bytes, *item;
    bool edit, strict_size, overwrite, no_store_request, no_manifest, failed, json;
} jobs_flags;

static int jobs_parse_flags(int argc, char **argv, jobs_flags *f, const char **error) {
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        const char **slot = NULL;
        if (strcmp(a, "--json") == 0) f->json = true;
        else if (strcmp(a, "--edit") == 0) f->edit = true;
        else if (strcmp(a, "--strict-size") == 0) f->strict_size = true;
        else if (strcmp(a, "--no-manifest") == 0) f->no_manifest = true;
        else if (strcmp(a, "--overwrite") == 0) f->overwrite = true;
        else if (strcmp(a, "--no-store-request") == 0) f->no_store_request = true;
        else if (strcmp(a, "--failed") == 0) f->failed = true;
        else if (strcmp(a, "--prompt") == 0) slot = &f->prompt;
        else if (strcmp(a, "--model") == 0) slot = &f->model;
        else if (strcmp(a, "--effort") == 0) slot = &f->effort;
        else if (strcmp(a, "--task") == 0) slot = &f->task;
        else if (strcmp(a, "--quality") == 0) slot = &f->quality;
        else if (strcmp(a, "--size") == 0) slot = &f->size;
        else if (strcmp(a, "--output-file") == 0) slot = &f->output_file;
        else if (strcmp(a, "--request") == 0) slot = &f->request;
        else if (strcmp(a, "--items") == 0) slot = &f->items;
        else if (strcmp(a, "--item") == 0) slot = &f->item;
        else if (strcmp(a, "--timeout") == 0) slot = &f->timeout;
        else if (strcmp(a, "--concurrency") == 0) slot = &f->concurrency;
        else if (strcmp(a, "--max-bytes") == 0) slot = &f->max_bytes;
        else if (strcmp(a, "--image") == 0) {
            if (f->n_images >= TNY_IMAGE_REFS_PER_ITEM || i + 1 >= argc) {
                *error = "at most 5 --image references are accepted";
                return -1;
            }
            f->images[f->n_images++] = argv[++i];
            continue;
        } else {
            *error = "unknown option";
            return -1;
        }
        if (!slot) continue;
        if (i + 1 >= argc || !*argv[i + 1]) {
            *error = "that option needs a value";
            return -1;
        }
        if (*slot) {
            *error = "that option was given twice";
            return -1;
        }
        *slot = argv[++i];
    }
    return 0;
}

/* "0,2,5" -> a JSON array of item indexes. */
static bool jobs_indexes_json(const char *list, buf_t *out) {
    buf_appends(out, "[");
    int written = 0;
    for (const char *p = list; p && *p;) {
        char *end = NULL;
        long value = strtol(p, &end, 10);
        if (end == p || value < 0 || value >= TNY_JOBS_MAX_ITEMS) return false;
        if (written++) buf_appends(out, ",");
        buf_appendf(out, "%ld", value);
        p = end;
        if (*p == ',') p++;
        else if (*p) return false;
    }
    buf_appends(out, "]");
    return written > 0;
}

static long jobs_bounded_long(const char *text, long low, long high, bool *ok) {
    char *end = NULL;
    long value = text ? strtol(text, &end, 10) : 0;
    *ok = text && end && !*end && value >= low && value <= high;
    return value;
}

static char *jobs_submit_request(const char *sub, const jobs_flags *f, const char *stdin_text,
                                 size_t stdin_len, const char **error) {
    bool image = strcmp(sub, "image") == 0;
    bool batch = strcmp(sub, "batch") == 0;
    if (f->no_manifest && (!image || f->request)) {
        *error = "--no-manifest applies to a single image submission; use persist_manifest in "
                 "batch requests";
        return NULL;
    }
    if (f->request || batch) {
        /* A bounded request document: exactly one kind, 1-64 items. */
        const char *body = NULL;
        size_t len = 0;
        char *loaded = NULL;
        if (f->request && strcmp(f->request, "-") != 0) {
            loaded = file_slurp(f->request, &len);
            if (!loaded) {
                *error = "the request file could not be read";
                return NULL;
            }
            body = loaded;
        } else {
            body = stdin_text;
            len = stdin_len;
        }
        if (!body || !len || len > TNY_JOBS_REQUEST_MAX) {
            free(loaded);
            *error = "a batch needs a bounded JSON request on stdin or --request FILE";
            return NULL;
        }
        yyjson_doc *doc = jparse(body, len);
        yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
        const char *kind = jget_str(root, "kind");
        bool ok = root && yyjson_is_obj(root) && kind &&
                  (batch || strcmp(kind, image ? "image" : "ask") == 0);
        char *copy = ok ? xstrndup(body, len) : NULL;
        yyjson_doc_free(doc);
        free(loaded);
        if (!copy) *error = "the request document must be a JSON object with a matching kind";
        return copy;
    }
    const char *prompt = f->prompt;
    char *piped = NULL;
    if (!prompt) {
        size_t len = stdin_len;
        while (len && (stdin_text[len - 1] == '\n' || stdin_text[len - 1] == '\r')) len--;
        piped = len ? xstrndup(stdin_text, len) : NULL;
        prompt = piped;
    }
    if (!prompt || !*prompt || !utf8_valid_bytes(prompt, strlen(prompt))) {
        if (piped) secure_free(piped);
        *error = "a nonempty UTF-8 prompt is required (--prompt TEXT or stdin)";
        return NULL;
    }
    bool ok = true;
    long concurrency =
        f->concurrency ? jobs_bounded_long(f->concurrency, 1, TNY_JOBS_MAX_CONCURRENCY, &ok) : 1;
    if (!ok) {
        if (piped) secure_free(piped);
        *error = "--concurrency takes 1 to 16";
        return NULL;
    }
    buf_t body;
    buf_init(&body);
    buf_appendf(&body, "{\"kind\":\"%s\",\"concurrency\":%ld,\"items\":[{\"prompt\":",
                image ? "image" : "ask", concurrency);
    jescape(&body, prompt);
    if (piped) secure_free(piped);
    static const char *const keys[] = {"model", "effort", "task", "quality", "size"};
    const char *values[5] = {f->model, f->effort, f->task, f->quality, f->size};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        if (!values[i]) continue;
        buf_appendf(&body, ",\"%s\":", keys[i]);
        jescape(&body, values[i]);
    }
    if (image) {
        buf_appends(&body, ",\"operation\":");
        jescape(&body, f->edit ? "edit" : "generate");
        if (!f->output_file) {
            buf_free(&body);
            *error = "an image job needs --output-file PATH";
            return NULL;
        }
        buf_appends(&body, ",\"output_file\":");
        jescape(&body, f->output_file);
        if (f->strict_size) buf_appends(&body, ",\"strict_size\":true");
        if (f->no_manifest) buf_appends(&body, ",\"persist_manifest\":false");
        if (f->overwrite) buf_appends(&body, ",\"overwrite\":true");
        buf_appends(&body, ",\"images\":[");
        for (int i = 0; i < f->n_images; i++) {
            if (i) buf_appends(&body, ",");
            jescape(&body, f->images[i]);
        }
        buf_appends(&body, "]");
    }
    if (f->no_store_request) buf_appends(&body, ",\"persist_request\":false");
    buf_appends(&body, "}]}");
    if (buf_oom(&body)) {
        buf_free(&body);
        *error = "out of memory";
        return NULL;
    }
    return buf_detach(&body);
}

tny_jobs_op tny_jobs_parse_argv(int argc, char **argv, const char *stdin_text, size_t stdin_len,
                                char **request_out, bool *json_out, const char **error) {
    *request_out = NULL;
    *error = NULL;
    if (json_out) *json_out = false;
    if (argc < 1) {
        *error = "jobs needs a subcommand";
        return TNY_JOBS_OP_NONE;
    }
    tny_jobs_op op = tny_jobs_op_parse(argv[0]);
    if (op == TNY_JOBS_OP_NONE) {
        *error = "unknown jobs subcommand";
        return TNY_JOBS_OP_NONE;
    }
    int i = 1;
    const char *sub = NULL, *id = NULL;
    if (op == TNY_JOBS_OP_SUBMIT) {
        sub = i < argc ? argv[i++] : NULL;
        if (!sub ||
            (strcmp(sub, "ask") != 0 && strcmp(sub, "image") != 0 && strcmp(sub, "batch") != 0)) {
            *error = "submit needs ask, image or batch";
            return TNY_JOBS_OP_NONE;
        }
    } else if (op != TNY_JOBS_OP_LIST) {
        id = i < argc ? argv[i++] : NULL;
        if (!tny_jobs_valid_id(id)) {
            *error = "a job id is 32 lowercase hex characters";
            return TNY_JOBS_OP_NONE;
        }
    }
    jobs_flags flags = {0};
    if (jobs_parse_flags(argc - i, argv + i, &flags, error) != 0) return TNY_JOBS_OP_NONE;
    if (json_out) *json_out = flags.json;
    if (flags.no_manifest && op != TNY_JOBS_OP_SUBMIT) {
        *error = "--no-manifest applies only to image submission";
        return TNY_JOBS_OP_NONE;
    }

    if (op == TNY_JOBS_OP_SUBMIT) {
        char *body = jobs_submit_request(sub, &flags, stdin_text, stdin_len, error);
        if (!body) return TNY_JOBS_OP_NONE;
        /* A flag-built request already carries --concurrency; a supplied
         * request document gets it spliced in once. */
        if (flags.concurrency && (flags.request || strcmp(sub, "batch") == 0)) {
            bool ok = false;
            long concurrency =
                jobs_bounded_long(flags.concurrency, 1, TNY_JOBS_MAX_CONCURRENCY, &ok);
            yyjson_doc *doc = ok ? jparse(body, strlen(body)) : NULL;
            yyjson_mut_doc *mut = doc ? yyjson_doc_mut_copy(doc, jallocator()) : NULL;
            yyjson_mut_val *root = mut ? yyjson_mut_doc_get_root(mut) : NULL;
            if (root) jm_set_int(mut, root, "concurrency", concurrency);
            char *rewritten = root ? jwrite(mut) : NULL;
            yyjson_mut_doc_free(mut);
            yyjson_doc_free(doc);
            if (!rewritten) {
                secure_free(body);
                *error = ok ? "the request could not be rewritten" : "--concurrency takes 1 to 16";
                return TNY_JOBS_OP_NONE;
            }
            secure_free(body);
            body = rewritten;
        }
        *request_out = body;
        return op;
    }

    buf_t request;
    buf_init(&request);
    buf_appends(&request, "{\"op\":");
    jescape(&request, tny_jobs_op_name(op));
    if (id) {
        buf_appends(&request, ",\"id\":");
        jescape(&request, id);
    }
    if (flags.items) {
        buf_appends(&request, ",\"items\":");
        if (!jobs_indexes_json(flags.items, &request)) {
            buf_free(&request);
            *error = "--items takes a comma-separated list of item indexes";
            return TNY_JOBS_OP_NONE;
        }
    }
    if (flags.failed) buf_appends(&request, ",\"failed\":true");
    bool ok = true;
    if (flags.item)
        buf_appendf(&request, ",\"item\":%ld",
                    jobs_bounded_long(flags.item, 0, TNY_JOBS_MAX_ITEMS - 1, &ok));
    if (flags.max_bytes)
        buf_appendf(&request, ",\"max_bytes\":%ld",
                    jobs_bounded_long(flags.max_bytes, 1, (long)TNY_JOBS_LOG_READ_MAX, &ok));
    if (flags.timeout)
        buf_appendf(&request, ",\"timeout_s\":%ld",
                    jobs_bounded_long(flags.timeout, 0, 86400, &ok));
    buf_appends(&request, "}");
    if (!ok || buf_oom(&request)) {
        buf_free(&request);
        *error = "an option value is out of range";
        return TNY_JOBS_OP_NONE;
    }
    *request_out = buf_detach(&request);
    return op;
}

void tny_jobs_render_human(tny_jobs_op op, const char *json, buf_t *out) {
    yyjson_doc *doc = json ? jparse(json, strlen(json)) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *kind = jget_str(root, "kind");
    if (!root) {
        yyjson_doc_free(doc);
        return;
    }
    if (kind && strcmp(kind, "job_list") == 0) {
        yyjson_val *jobs = jget(root, "jobs");
        size_t idx, max;
        yyjson_val *job;
        if (jobs && yyjson_is_arr(jobs) && yyjson_arr_size(jobs)) {
            yyjson_arr_foreach(jobs, idx, max, job) {
                buf_appendf(out, "%s  %-9s %-11s items=%lld attempt=%lld  %s\n",
                            jget_str(job, "id"), jget_str(job, "job_kind"), jget_str(job, "state"),
                            (long long)jget_int(job, "items", 0),
                            (long long)jget_int(job, "attempt", 1), jget_str(job, "updated"));
            }
        } else buf_appends(out, "(no jobs)\n");
        yyjson_doc_free(doc);
        return;
    }
    if (kind && strcmp(kind, "job_logs") == 0) {
        buf_appendf(out, "%s\n", jget_str(root, "log_path"));
        const char *text = jget_str(root, "text");
        if (text) buf_appends(out, text);
        if (jget_bool(root, "truncated", false))
            buf_appendf(out, "\n…[%lld bytes total; raise --max-bytes for more]\n",
                        (long long)jget_int(root, "bytes", 0));
        yyjson_doc_free(doc);
        return;
    }
    const char *id = jget_str(root, "id");
    buf_appendf(out, "job %s (%s)\n", id ? id : "?", jget_str(root, "state"));
    const char *metadata = jget_str(root, "metadata_path");
    if (metadata) buf_appendf(out, "metadata: %s\n", metadata);
    const char *error = jget_str(root, "error");
    const char *code =
        jget_str(root, "code") ? jget_str(root, "code") : jget_str(root, "error_code");
    if (code || error)
        buf_appendf(out, "%s%s%s\n", code ? code : "", code && error ? ": " : "",
                    error ? error : "");
    yyjson_val *items = jget(root, "items");
    if (items && yyjson_is_arr(items)) {
        size_t idx, max;
        yyjson_val *item;
        yyjson_arr_foreach(items, idx, max, item) {
            buf_appendf(out, "  item %lld %-11s", (long long)jget_int(item, "index", 0),
                        jget_str(item, "state"));
            const char *session = jget_str(item, "session_id");
            const char *output = jget_str(item, "output_path");
            const char *manifest = jget_str(item, "manifest_path");
            const char *item_error = jget_str(item, "error");
            if (session) buf_appendf(out, " session=%s", session);
            if (output) buf_appendf(out, " output=%s", output);
            if (manifest) buf_appendf(out, " manifest=%s", manifest);
            if (item_error) buf_appendf(out, " (%s)", item_error);
            buf_appendf(out, "\n    log: %s\n", jget_str(item, "log_path"));
        }
    }
    if (op == TNY_JOBS_OP_SUBMIT && id)
        buf_appendf(out, "follow it with: tny jobs status %s\n", id);
    yyjson_doc_free(doc);
}

/* ------------------------------------------------------- state projection */

/* A queued or running record whose owner lock is free has lost its
 * supervisor. That is interrupted with unknown cleanup — never succeeded,
 * never cancelled, and never "still running" (A11, ADR 0093). */
static int jobs_project(const char *dir, const char *id) {
    char *owner = jobs_file(dir, "owner.lock");
    if (!owner) return ENOMEM;
    tny_jobs_owner_state owner_state = tny_jobs_host_owner_state(owner);
    free(owner);
    if (owner_state != TNY_JOBS_OWNER_FREE) return 0; /* held or unknown: leave it alone */
    yyjson_mut_doc *peek = jobs_record_load(dir, id, NULL, 0);
    const char *state = peek ? jm_str(yyjson_mut_doc_get_root(peek), "state") : NULL;
    bool active = state && !state_is_terminal(state);
    yyjson_mut_doc_free(peek);
    if (!active) return 0;

    jobs_txn t;
    char err[128] = "";
    if (jobs_txn_begin(dir, id, &t, err, sizeof err) != 0) return EBUSY;
    /* Re-probe under the state lock: the supervisor may have taken ownership
     * between the first probe and this transaction. */
    char *owner_path = jobs_file(dir, "owner.lock");
    tny_jobs_owner_state again =
        owner_path ? tny_jobs_host_owner_state(owner_path) : TNY_JOBS_OWNER_UNKNOWN;
    free(owner_path);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(t.doc);
    if (again != TNY_JOBS_OWNER_FREE || state_is_terminal(jm_str(root, "state"))) {
        jobs_txn_end(&t);
        return 0;
    }
    yyjson_mut_val *prior_state = yyjson_mut_obj_get(root, "state");
    yyjson_mut_val *prior_cleanup = yyjson_mut_obj_get(root, "cleanup");
    yyjson_mut_val *hold = yyjson_mut_obj_get(root, "cleanup_hold");
    if (!(yyjson_mut_equals_str(prior_state, "queued") ||
          yyjson_mut_equals_str(prior_state, "running")) ||
        !yyjson_mut_equals_str(prior_cleanup, "pending") || (hold && !yyjson_mut_is_bool(hold))) {
        /* Do not turn malformed/previously uncertain cleanup into fresh A11
         * reclaim authority. A true latch on a valid projection is preserved. */
        jobs_txn_end(&t);
        return EINVAL;
    }
    jm_set_str(t.doc, root, "state", "interrupted");
    jm_set_str(t.doc, root, "cleanup", "unknown");
    jm_set_int(t.doc, root, "exit_code", 2);
    jm_set_str(t.doc, root, "error_code", TNY_JOBS_CODE_INTERRUPTED);
    jm_set_str(t.doc, root, "error",
               "the job supervisor is gone; any owned child processes were not observed exiting");
    int count = jm_item_count(t.doc);
    for (int i = 0; i < count; i++) {
        yyjson_mut_val *item = jm_item(t.doc, i);
        const char *item_state = jm_str(item, "state");
        if (item_state && !state_is_terminal(item_state)) {
            jm_set_str(t.doc, item, "state", "interrupted");
            jm_set_str(t.doc, item, "error_code", TNY_JOBS_CODE_INTERRUPTED);
            jm_set_str(t.doc, item, "error", "the supervisor exited before this item finished");
        }
    }
    return jobs_txn_commit(&t);
}

/* ------------------------------------------------------------ result JSON */

static void job_items_json(yyjson_mut_doc *doc, buf_t *out) {
    buf_appends(out, ",\"items\":[");
    int count = jm_item_count(doc);
    for (int i = 0; i < count; i++) {
        yyjson_mut_val *item = jm_item(doc, i);
        if (i) buf_appends(out, ",");
        buf_appendf(out, "{\"index\":%d,\"state\":", i);
        jescape(out, jm_str(item, "state"));
        buf_appends(out, ",\"log_path\":");
        jescape(out, jm_str(item, "log_path"));
        yyjson_mut_val *exit_code = yyjson_mut_obj_get(item, "exit_code");
        if (exit_code && yyjson_mut_is_int(exit_code))
            buf_appendf(out, ",\"exit_code\":%lld", (long long)yyjson_mut_get_sint(exit_code));
        else buf_appends(out, ",\"exit_code\":null");
        static const char *const strings[] = {"error_code",    "error",       "started",
                                              "finished",      "session_id",  "result_sha256",
                                              "log_sha256",    "output_path", "output_sha256",
                                              "manifest_path", "operation_id"};
        for (size_t s = 0; s < sizeof strings / sizeof strings[0]; s++) {
            const char *value = jm_str(item, strings[s]);
            buf_appendf(out, ",\"%s\":", strings[s]);
            if (value) jescape(out, value);
            else buf_appends(out, "null");
        }
        yyjson_mut_val *bytes = yyjson_mut_obj_get(item, "output_bytes");
        if (bytes && yyjson_mut_is_int(bytes))
            buf_appendf(out, ",\"output_bytes\":%lld", (long long)yyjson_mut_get_sint(bytes));
        else buf_appends(out, ",\"output_bytes\":null");
        buf_appendf(out, ",\"carried_from_attempt\":%lld",
                    (long long)jm_int(item, "carried_from_attempt", 0));
        buf_appendf(out, ",\"cancel_requested\":%s}",
                    jm_bool(item, "cancel_requested", false) ? "true" : "false");
    }
    buf_appends(out, "]");
}

static void job_json(yyjson_mut_doc *doc, const char *dir, buf_t *out) {
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    const char *state = jm_str(root, "state");
    buf_appends(out, "{\"kind\":\"job\",\"schema_version\":1,\"id\":");
    jescape(out, jm_str(root, "id"));
    buf_appends(out, ",\"job_kind\":");
    jescape(out, jm_str(root, "job_kind"));
    buf_appends(out, ",\"state\":");
    jescape(out, state);
    buf_appendf(out, ",\"ok\":%s", state && strcmp(state, "succeeded") == 0 ? "true" : "false");
    buf_appendf(out, ",\"attempt\":%lld,\"revision\":%lld,\"concurrency\":%lld",
                (long long)jm_int(root, "attempt", 1), (long long)jm_int(root, "revision", 0),
                (long long)jm_int(root, "concurrency", 1));
    buf_appendf(out, ",\"cancel_requested\":%s",
                jm_bool(root, "cancel_requested", false) ? "true" : "false");
    buf_appends(out, ",\"cleanup\":");
    jescape(out, jm_str(root, "cleanup"));
    yyjson_mut_val *hold = yyjson_mut_obj_get(root, "cleanup_hold");
    buf_appendf(out, ",\"cleanup_hold\":%s",
                hold && (!yyjson_mut_is_bool(hold) || yyjson_mut_get_bool(hold)) ? "true"
                                                                                 : "false");
    yyjson_mut_val *exit_code = yyjson_mut_obj_get(root, "exit_code");
    if (exit_code && yyjson_mut_is_int(exit_code))
        buf_appendf(out, ",\"exit_code\":%lld", (long long)yyjson_mut_get_sint(exit_code));
    else buf_appends(out, ",\"exit_code\":null");
    static const char *const strings[] = {"error_code", "error", "created", "updated", "workspace"};
    for (size_t s = 0; s < sizeof strings / sizeof strings[0]; s++) {
        const char *value = jm_str(root, strings[s]);
        buf_appendf(out, ",\"%s\":", strings[s]);
        if (value) jescape(out, value);
        else buf_appends(out, "null");
    }
    char *metadata = jobs_file(dir, "job.json");
    buf_appends(out, ",\"metadata_path\":");
    jescape(out, metadata ? metadata : "");
    free(metadata);
    job_items_json(doc, out);
    buf_appends(out, "}\n");
}

/* Recheck after reservation acquisition and again at launch. A contender may
 * have validated absence while the previous reservation owner was finishing. */
static int outputs_revalidate(const jobs_request *request, const int *indexes, int n, char *err,
                              size_t errlen) {
    for (int k = 0; k < n; k++) {
        int i = indexes[k];
        char *canonical = NULL;
        if (validate_image_item(NULL, request->items[i], &canonical, err, errlen) != 0)
            return EEXIST;
        bool same = canonical && strcmp(canonical, request->outputs[i]) == 0;
        free(canonical);
        if (!same) {
            safe_err(err, errlen, "output identity changed after reservation");
            return EEXIST;
        }
    }
    return 0;
}

/* Explicit user settings, not request/agent supplied environment authority.
 * jobs.ask_env names at most 32 inherited carriers owned by ask tools/agents.
 * Image account values and harness controls cannot be lent through this map. */
static bool ask_env_forbidden(tny_ctx *ctx, const char *name, const char *value) {
    if (!name || !*name || strlen(name) > 128 || str_starts(name, "TNY_") ||
        strcmp(name, "CHATGPT_ACCESS_TOKEN") == 0 || strcmp(name, "CHATGPT_ACCOUNT_ID") == 0)
        return true;
    for (const char *p = name; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_' ||
              (p != name && *p >= '0' && *p <= '9')))
            return true;
    const char *foreign[] = {
        ctx->chatgpt_token,           ctx->chatgpt_account_id,      getenv("CHATGPT_ACCESS_TOKEN"),
        getenv("CHATGPT_ACCOUNT_ID"), getenv("TNY_CODEX_BASE_URL"), ctx->codex_base_url};
    for (size_t i = 0; value && *value && i < sizeof foreign / sizeof foreign[0]; i++)
        if (foreign[i] && *foreign[i] && strcmp(value, foreign[i]) == 0) return true;
    return false;
}

/* ----------------------------------------------------------- child payload */

/* The private payload: prompts, resolved credentials and private base URLs
 * travel through an anonymous pipe into the supervisor's memory. Nothing here
 * is ever written to argv, to a log or to the job directory. */
static char *payload_build(tny_ctx *ctx, const jobs_request *request, const char *job_id,
                           int attempt, const char *self, const int *indexes, int n_indexes) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(jallocator());
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!root) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, root);
    /* A configured credential carrier outranks the operational allowlist too
     * (for example a profile that deliberately names LANG as api_key_env). */
    yyjson_mut_val *blocked = yyjson_mut_arr(doc);
    yyjson_val *settings = ctx->settings ? yyjson_doc_get_root(ctx->settings) : NULL;
    size_t si, sm;
    yyjson_val *key, *profile;
    if (yyjson_is_obj(settings)) {
        yyjson_obj_foreach(settings, si, sm, key, profile) {
            const char *carrier = jget_str(profile, "api_key_env");
            if (jget_str(profile, "base_url") && carrier)
                yyjson_mut_arr_append(blocked, yyjson_mut_strcpy(doc, carrier));
        }
    }
    yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc, "blocked_env"), blocked);
    yyjson_mut_val *ask_env = yyjson_mut_obj(doc);
    yyjson_val *declared = jget(jget(settings, "jobs"), "ask_env");
    if (declared) {
        if (!yyjson_is_arr(declared) || yyjson_arr_size(declared) > 32) {
            yyjson_mut_doc_free(doc);
            return NULL;
        }
        yyjson_val *entry;
        yyjson_arr_foreach(declared, si, sm, entry) {
            const char *name = yyjson_get_str(entry);
            const char *value = name ? getenv(name) : NULL;
            if (ask_env_forbidden(ctx, name, request->image ? NULL : value)) {
                yyjson_mut_doc_free(doc);
                return NULL;
            }
            /* A declared ask carrier is foreign to image, even when its name
             * would otherwise be operational (LANG, HOME, etc.). */
            yyjson_mut_arr_append(blocked, yyjson_mut_strcpy(doc, name));
            if (!request->image && value) jm_set_str(doc, ask_env, name, value);
        }
    }
    yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc, "ask_env"), ask_env);
    jm_set_int(doc, root, "version", TNY_JOBS_SCHEMA_VERSION);
    jm_set_str(doc, root, "job", job_id);
    jm_set_int(doc, root, "attempt", attempt);
    jm_set_str(doc, root, "kind", request->image ? "image" : "ask");
    jm_set_int(doc, root, "concurrency", request->concurrency);
    jm_set_str(doc, root, "self", self);
    jm_set_str(doc, root, "cwd", ctx->cwd);
    /* The resolved provider travels with the item child: re-resolving it from
     * settings would let a remembered last_provider re-route paid work. */
    jm_set_str(doc, root, "provider", tny_provider_name(ctx));
    jm_set_str(doc, root, "perm_mode", tny_perm_mode_name(ctx->perm_mode));
    jm_set_str(doc, root, "tools", tny_tool_profile_name(ctx->tool_profile));
    /* Chat and image credentials are two separate allowances (A14): an ask
     * item never receives the image allowance and an image item never
     * receives the chat key. They are distinct mappings even when one account
     * happens to back both, so that gating one never disarms the other. */
    bool chat_is_codex = tny_provider_name(ctx) && strcmp(tny_provider_name(ctx), "codex") == 0;
    yyjson_mut_val *chat = yyjson_mut_obj(doc);
    bool chat_is_cursor = tny_provider_name(ctx) && strcmp(tny_provider_name(ctx), "cursor") == 0;
    jm_set_str(doc, chat, "api_key", chat_is_cursor ? NULL : ctx->api_key);
    jm_set_str(doc, chat, "cursor_key", chat_is_cursor ? getenv("CURSOR_API_KEY") : NULL);
    jm_set_str(doc, chat, "codex_url", chat_is_codex ? getenv("TNY_CODEX_BASE_URL") : NULL);
    jm_set_str(doc, chat, "base_url", ctx->base_url);
    jm_set_str(doc, chat, "wire_api",
               ctx->wire_api ? (tny_wire_is_chat(ctx->wire_api) ? "chat" : "responses") : NULL);
    jm_set_str(doc, chat, "model", ctx->model);
    jm_set_str(doc, chat, "effort", ctx->reasoning_effort);
    /* The selected conversation provider IS the ChatGPT account here, so the
     * account is this ask item's own chat credential — not a borrowed image
     * allowance. Any other provider gets none of it. */
    jm_set_str(doc, chat, "token",
               chat_is_codex
                   ? (ctx->chatgpt_token ? ctx->chatgpt_token : getenv("CHATGPT_ACCESS_TOKEN"))
                   : NULL);
    jm_set_str(doc, chat, "account",
               chat_is_codex ? (ctx->chatgpt_account_id ? ctx->chatgpt_account_id
                                                        : getenv("CHATGPT_ACCOUNT_ID"))
                             : NULL);
    jm_set_bool(doc, chat, "codex", chat_is_codex);
    yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc, "chat"), chat);
    yyjson_mut_val *image = yyjson_mut_obj(doc);
    jm_set_str(doc, image, "token",
               ctx->chatgpt_token ? ctx->chatgpt_token : getenv("CHATGPT_ACCESS_TOKEN"));
    jm_set_str(doc, image, "account",
               ctx->chatgpt_account_id ? ctx->chatgpt_account_id : getenv("CHATGPT_ACCOUNT_ID"));
    jm_set_str(doc, image, "codex_url", getenv("TNY_CODEX_BASE_URL"));
    yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc, "image"), image);
    yyjson_mut_val *items = yyjson_mut_arr(doc);
    yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc, "items"), items);
    for (int i = 0; i < request->n_items; i++) {
        bool selected = true;
        if (indexes) {
            selected = false;
            for (int k = 0; k < n_indexes; k++)
                if (indexes[k] == i) selected = true;
        }
        if (!selected || !request->items[i]) continue;
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        jm_set_int(doc, item, "index", i);
        jm_set_str(doc, item, "prompt", jget_str(request->items[i], "prompt"));
        static const char *const passthrough[] = {"model",   "effort", "task",
                                                  "quality", "size",   "operation"};
        for (size_t p = 0; p < sizeof passthrough / sizeof passthrough[0]; p++)
            jm_set_str(doc, item, passthrough[p], jget_str(request->items[i], passthrough[p]));
        jm_set_bool(doc, item, "strict_size", jget_bool(request->items[i], "strict_size", false));
        jm_set_bool(doc, item, "persist_manifest",
                    jget_bool(request->items[i], "persist_manifest", true));
        jm_set_bool(doc, item, "overwrite", jget_bool(request->items[i], "overwrite", false));
        if (request->outputs[i]) jm_set_str(doc, item, "output_file", request->outputs[i]);
        yyjson_mut_obj_put(item, yyjson_mut_strcpy(doc, "images"),
                           refs_copy(doc, jget(request->items[i], "images")));
        yyjson_mut_arr_append(items, item);
    }
    char *json = jwrite(doc);
    yyjson_mut_doc_free(doc);
    return json;
}

/* --------------------------------------------------------- record creation */

static yyjson_mut_doc *record_new(tny_ctx *ctx, const jobs_request *request, const char *job_id,
                                  const char *dir) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(jallocator());
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!root) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, root);
    char *now = now_iso8601();
    jm_set_int(doc, root, "version", TNY_JOBS_SCHEMA_VERSION);
    jm_set_str(doc, root, "kind", "job");
    jm_set_str(doc, root, "id", job_id);
    jm_set_str(doc, root, "job_kind", request->image ? "image" : "ask");
    jm_set_str(doc, root, "workspace", ctx->cwd);
    jm_set_str(doc, root, "created", now ? now : "");
    jm_set_str(doc, root, "updated", now ? now : "");
    jm_set_int(doc, root, "revision", 1);
    jm_set_int(doc, root, "attempt", 1);
    jm_set_int(doc, root, "concurrency", request->concurrency);
    jm_set_str(doc, root, "state", "queued");
    jm_set_bool(doc, root, "cancel_requested", false);
    jm_set_null(doc, root, "exit_code");
    jm_set_null(doc, root, "error_code");
    jm_set_null(doc, root, "error");
    jm_set_str(doc, root, "cleanup", "pending");
    jm_set_bool(doc, root, "cleanup_hold", false);
    yyjson_mut_val *items = yyjson_mut_arr(doc);
    yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc, "items"), items);
    for (int i = 0; i < request->n_items; i++) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        bool persist = jget_bool(request->items[i], "persist_request", true);
        jm_set_int(doc, item, "index", i);
        jm_set_str(doc, item, "state", "queued");
        jm_set_bool(doc, item, "cancel_requested", false);
        jm_set_int(doc, item, "attempt", 1);
        jm_set_int(doc, item, "carried_from_attempt", 0);
        jm_set_bool(doc, item, "persist_request", persist);
        char *log = jobs_item_log(dir, i, 1);
        jm_set_str(doc, item, "log_path", log);
        free(log);
        jm_set_null(doc, item, "started");
        jm_set_null(doc, item, "finished");
        jm_set_null(doc, item, "exit_code");
        jm_set_null(doc, item, "error_code");
        jm_set_null(doc, item, "error");
        jm_set_null(doc, item, "session_id");
        jm_set_null(doc, item, "result_sha256");
        jm_set_null(doc, item, "log_sha256");
        jm_set_null(doc, item, "output_sha256");
        jm_set_null(doc, item, "output_bytes");
        jm_set_null(doc, item, "manifest_path");
        jm_set_null(doc, item, "operation_id");
        jm_set_str(doc, item, "output_path", request->outputs[i]);
        /* Default persistence stores exactly the settings a later explicit
         * retry needs. A privacy opt-out stores no prompt, reference or
         * request body anywhere in the job directory; retrying it then needs
         * an explicit new submission. */
        if (persist) {
            yyjson_mut_val *stored = yyjson_mut_obj(doc);
            jm_set_str(doc, stored, "prompt", jget_str(request->items[i], "prompt"));
            if (request->outputs[i]) jm_set_str(doc, stored, "output_file", request->outputs[i]);
            static const char *const keys[] = {"model",   "effort", "task",
                                               "quality", "size",   "operation"};
            for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++)
                jm_set_str(doc, stored, keys[k], jget_str(request->items[i], keys[k]));
            jm_set_bool(doc, stored, "strict_size",
                        jget_bool(request->items[i], "strict_size", false));
            jm_set_bool(doc, stored, "persist_manifest",
                        jget_bool(request->items[i], "persist_manifest", true));
            jm_set_bool(doc, stored, "overwrite", jget_bool(request->items[i], "overwrite", false));
            yyjson_mut_obj_put(stored, yyjson_mut_strcpy(doc, "images"),
                               refs_copy(doc, jget(request->items[i], "images")));
            yyjson_mut_obj_put(item, yyjson_mut_strcpy(doc, "request"), stored);
        } else {
            jm_set_null(doc, item, "request");
        }
        yyjson_mut_arr_append(items, item);
    }
    free(now);
    return doc;
}

/* ------------------------------------------------------------ the launcher */

typedef struct {
    pid_t pid;
    int owner_fd;
    bool live; /* the child exists: this process may no longer write metadata */
} jobs_launch;

/* Start the detached supervisor with the payload on stdin, the acknowledgment
 * pipe on stdout and the live owner lock on descriptor 3 (ADR 0093). */
static int jobs_launch_worker(const char *self, const char *dir, const char *job_id, int owner_fd,
                              const char *payload, jobs_launch *launch, char *err, size_t errlen,
                              bool (*cancelled)(void *), void *cancel_ud) {
    (void)dir;
    memset(launch, 0, sizeof *launch);
    launch->owner_fd = owner_fd;
#ifdef __EMSCRIPTEN__
    (void)self;
    (void)job_id;
    (void)payload;
    (void)cancelled;
    (void)cancel_ud;
    safe_err(err, errlen, "this build cannot start a job supervisor");
    return ENOTSUP;
#else
    int payload_pipe[2] = {-1, -1}, ack_pipe[2] = {-1, -1};
    if (pipe(payload_pipe) != 0) {
        safe_err(err, errlen, "cannot create the payload pipe");
        return EIO;
    }
    if (pipe(ack_pipe) != 0) {
        close(payload_pipe[0]);
        close(payload_pipe[1]);
        safe_err(err, errlen, "cannot create the acknowledgment pipe");
        return EIO;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(payload_pipe[i], F_SETFD, FD_CLOEXEC);
        fcntl(ack_pipe[i], F_SETFD, FD_CLOEXEC);
    }
    char *argv[8];
    int n = 0;
    argv[n++] = (char *)self;
    argv[n++] = (char *)"jobs";
    argv[n++] = (char *)"_worker";
    argv[n++] = (char *)job_id;
    argv[n] = NULL;
    const tny_fd_mapping maps[] = {{payload_pipe[0], 0}, {ack_pipe[1], 1}, {owner_fd, 3}};
    pid_t pid = -1;
    int rc = tny_process_spawn_mapped(argv, environ, maps, 3, &pid);
    close(payload_pipe[0]);
    close(ack_pipe[1]);
    if (rc) {
        close(payload_pipe[1]);
        close(ack_pipe[0]);
        safe_err(err, errlen, "cannot start the job supervisor");
        return rc;
    }
    launch->pid = pid;
    launch->live = true; /* from here the supervisor owns this job's metadata */
    rc = tny_jobs_host_handshake(payload_pipe[1], ack_pipe[0], payload, strlen(payload), job_id,
                                 TNY_JOBS_ACK_TIMEOUT_MS, cancelled, cancel_ud);
    if (rc) safe_err(err, errlen, "the private supervisor handshake did not complete");
    return rc;
#endif
}

/* ------------------------------------------------------------ submit/retry */

static int submit_finish_failed(const char *dir, int attempt, const char *code,
                                const char *message) {
    jobs_txn t;
    if (jobs_txn_begin(dir, NULL, &t, NULL, 0) != 0) return EIO;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(t.doc);
    /* Caller retains the accepted owner description until this transaction and
     * its reservation cleanup finish. Never finalize another attempt. */
    if (jm_int(root, "attempt", -1) != attempt ||
        strcmp(jm_str(root, "state") ? jm_str(root, "state") : "", "queued") != 0) {
        jobs_txn_end(&t);
        return EBUSY;
    }
    jm_set_str(t.doc, root, "state", "failed");
    jm_set_str(t.doc, root, "cleanup", "complete");
    jm_set_bool(t.doc, root, "cleanup_hold", false);
    jm_set_int(t.doc, root, "exit_code", 2);
    jm_set_str(t.doc, root, "error_code", code);
    jm_set_str(t.doc, root, "error", message);
    int count = jm_item_count(t.doc);
    for (int i = 0; i < count; i++) {
        yyjson_mut_val *item = jm_item(t.doc, i);
        if (!state_is_terminal(jm_str(item, "state"))) {
            jm_set_str(t.doc, item, "state", "failed");
            jm_set_str(t.doc, item, "error_code", code);
            jm_set_str(t.doc, item, "error", message);
        }
    }
    return jobs_txn_commit(&t);
}

static int jobs_submit(tny_ctx *ctx, yyjson_val *args, buf_t *out, char *err, size_t errlen,
                       bool (*cancelled)(void *), void *cancel_ud) {
    if (!tny_jobs_execution_supported()) {
        safe_err(err, errlen,
                 "durable jobs need a native tny build; this runtime cannot own a child process");
        return 1;
    }
    jobs_request request;
    if (jobs_request_parse(ctx, args, &request, err, errlen) != 0) return 1;

    char *self = tny_process_self_path();
    uint8_t raw[16];
    char job_id[TNY_JOBS_ID_LEN + 1] = {0};
    if (!self || !random_bytes(raw, sizeof raw)) {
        free(self);
        jobs_request_free(&request);
        safe_err(err, errlen, "cannot identify this executable");
        return 2;
    }
    hex_of(raw, sizeof raw, job_id);

    char *root = jobs_root(ctx);
    char *dir = root ? path_join(root, job_id) : NULL;
    free(root);
    int rc = dir ? tny_jobs_host_mkdir_private(dir) : ENOMEM;
    if (rc) {
        free(dir);
        free(self);
        jobs_request_free(&request);
        safe_err(err, errlen, "cannot create the private job directory");
        return 2;
    }
    /* The owner lock exists and is held before any reservation is claimed, so
     * a concurrent submitter can never mistake this unacknowledged job for an
     * abandoned one. */
    char *owner_path = jobs_file(dir, "owner.lock");
    int owner_fd = owner_path ? tny_jobs_host_lock_open(owner_path) : -1;
    if (owner_fd < 0 || tny_jobs_host_lock_try(owner_fd) != TNY_JOBS_LOCK_ACQUIRED) {
        if (owner_fd >= 0) tny_jobs_host_lock_close(owner_fd);
        free(owner_path);
        free(dir);
        free(self);
        jobs_request_free(&request);
        safe_err(err, errlen, "cannot take ownership of the new job");
        return 2;
    }

    yyjson_mut_doc *record = record_new(ctx, &request, job_id, dir);
    rc = record ? jobs_record_store(dir, record) : ENOMEM;
    if (rc) {
        yyjson_mut_doc_free(record);
        tny_jobs_host_lock_close(owner_fd);
        free(owner_path);
        free(dir);
        free(self);
        jobs_request_free(&request);
        safe_err(err, errlen, "cannot write the job record");
        return 2;
    }

    reservation_claim claims[TNY_JOBS_MAX_ITEMS];
    int indexes[TNY_JOBS_MAX_ITEMS];
    int n_claims = 0;
    for (int i = 0; i < request.n_items; i++)
        if (request.outputs[i]) {
            claims[n_claims].path = request.outputs[i];
            indexes[n_claims] = i;
            n_claims++;
        }
    rc = n_claims ? reservations_claim_all(ctx, job_id, 1, claims, indexes, n_claims, err, errlen)
                  : 0;
    if (rc) {
        submit_finish_failed(dir, 1, TNY_JOBS_CODE_OUTPUT_BUSY, err);
        yyjson_mut_doc_free(record);
        tny_jobs_host_lock_close(owner_fd);
        free(owner_path);
        free(dir);
        free(self);
        jobs_request_free(&request);
        return 1;
    }

    rc = outputs_revalidate(&request, indexes, n_claims, err, errlen);
    char *payload = rc ? NULL : payload_build(ctx, &request, job_id, 1, self, NULL, 0);
    if (!rc && !payload)
        safe_err(err, errlen,
                 "cannot build private request; check jobs.ask_env declarations and memory "
                 "availability");
    jobs_launch launch = {0};
    if (!rc)
        rc = payload ? jobs_launch_worker(self, dir, job_id, owner_fd, payload, &launch, err,
                                          errlen, cancelled, cancel_ud)
                     : ENOMEM;
    if (payload) secure_free(payload);
    /* Keep ownership through local failure finalization. If launch succeeded,
     * the inherited description holds the same lock after our close. */
    int exit_code = 0;
    if (rc && !launch.live) {
        /* Nothing was started, so this process still owns the record. */
        for (int i = 0; i < n_claims; i++)
            reservation_release_one(ctx, claims[i].path, job_id, indexes[i], 1);
        submit_finish_failed(dir, 1, TNY_JOBS_CODE_IO, err);
        exit_code = 2;
    } else if (rc) {
        /* A live child owns this job now: report uncertainty with the durable
         * id and never write a competing terminal record or roll back the
         * child's reservations. */
        safe_err(err, errlen,
                 "the supervisor did not acknowledge in time; job %s is durable — check "
                 "`tny jobs status %s` and cancel it if it is unwanted",
                 job_id, job_id);
        exit_code = 2;
    }
    tny_jobs_host_lock_close(owner_fd);
    yyjson_mut_doc_free(record);

    if (rc && launch.live) {
        /* Uncertain, not failed: the durable id is the answer. */
        buf_appends(out, "{\"kind\":\"job\",\"schema_version\":1,\"id\":");
        jescape(out, job_id);
        buf_appends(out, ",\"state\":\"uncertain\",\"ok\":false,\"code\":\"" TNY_JOBS_CODE_UNCERTAIN
                         "\",\"error\":");
        jescape(out, err);
        char *metadata = jobs_file(dir, "job.json");
        buf_appends(out, ",\"metadata_path\":");
        jescape(out, metadata ? metadata : "");
        free(metadata);
        buf_appends(out, "}\n");
    } else {
        yyjson_mut_doc *current = jobs_record_load(dir, job_id, NULL, 0);
        if (current) {
            job_json(current, dir, out);
            yyjson_mut_doc_free(current);
        } else {
            safe_err(err, errlen, "the job record could not be read back");
            exit_code = 2;
        }
    }
    free(owner_path);
    free(dir);
    free(self);
    jobs_request_free(&request);
    return exit_code;
}

/* The durable answer of a finished ask item is its stored session's final
 * assistant message: the one thing a later attempt can re-verify. A session
 * that is gone, still running or has been continued since fails the check, so
 * a carried success can never be a guess. malloc'd hex, NULL when unusable. */
static char *tny_jobs_session_answer_sha(tny_ctx *ctx, const char *session_id) {
    if (!ctx || !session_id || session_is_running(ctx, session_id)) return NULL;
    tny_session_state *session = session_open(ctx, session_id);
    if (!session) return NULL;
    const char *status = session_status(session); /* absent on foreground turns */
    char *hex = NULL;
    if (!status || strcmp(status, "done") == 0) {
        yyjson_mut_val *messages = session_messages(session);
        const char *answer = NULL;
        size_t idx, max;
        yyjson_mut_val *message;
        if (messages && yyjson_mut_is_arr(messages)) {
            yyjson_mut_arr_foreach(messages, idx, max, message) {
                const char *role = yyjson_mut_get_str(yyjson_mut_obj_get(message, "role"));
                const char *content = yyjson_mut_get_str(yyjson_mut_obj_get(message, "content"));
                if (role && content && strcmp(role, "assistant") == 0) answer = content;
            }
        }
        if (answer) hex = sha256_hex_of(answer, strlen(answer));
    }
    session_close(session);
    return hex;
}

/* ---------------------------------------------------- carried-success check */

/* An image generation manifest (ADR 0088) is read here as data, not through
 * the image service: this checks only that the immutable record still
 * describes exactly the artifact this item carries forward. Field names are
 * the manifest's own — version/kind, committed, artifacts[].path/.sha256. */
bool tny_jobs_manifest_describes(const char *manifest_path, const char *output_path,
                                 const char *output_sha256, char *err, size_t errlen) {
    if (!manifest_path || !*manifest_path) return true; /* nothing claimed */
    size_t len = 0;
    char *data = file_slurp(manifest_path, &len);
    if (!data || len > TNY_JOBS_REQUEST_MAX) {
        free(data);
        safe_err(err, errlen, "the recorded generation manifest is missing or unreadable");
        return false;
    }
    yyjson_doc *doc = jparse(data, len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *kind = jget_str(root, "kind");
    bool ok = kind && strcmp(kind, "image_manifest") == 0 && jget_bool(root, "committed", false);
    yyjson_val *artifacts = ok ? jget(root, "artifacts") : NULL;
    bool matched = false;
    if (artifacts && yyjson_is_arr(artifacts)) {
        size_t idx, max;
        yyjson_val *artifact;
        yyjson_arr_foreach(artifacts, idx, max, artifact) {
            const char *path = jget_str(artifact, "path");
            const char *sha = jget_str(artifact, "sha256");
            if (path && sha && output_path && output_sha256 && strcmp(path, output_path) == 0 &&
                strcmp(sha, output_sha256) == 0)
                matched = true;
        }
    }
    yyjson_doc_free(doc);
    free(data);
    if (!(ok && matched))
        safe_err(err, errlen,
                 "the recorded generation manifest no longer describes this item's artifact");
    return ok && matched;
}

/* Every success carried into a new attempt is verified against what is
 * actually on disk before a single new provider request is made. */
static int verify_carried_success(tny_ctx *ctx, yyjson_mut_val *item, bool image, char *err,
                                  size_t errlen) {
    if (image) {
        const char *path = jm_str(item, "output_path");
        const char *want = jm_str(item, "output_sha256");
        int64_t want_bytes = jm_int(item, "output_bytes", -1);
        if (!path || !want) {
            safe_err(err, errlen, "a successful image item has no recorded output hash");
            return -1;
        }
        size_t len = 0;
        char *have = sha256_hex_file(path, &len);
        bool ok = have && strcmp(have, want) == 0 && (want_bytes < 0 || (int64_t)len == want_bytes);
        free(have);
        if (!ok) {
            safe_err(err, errlen, "the successful output of item %lld is missing or changed",
                     (long long)jm_int(item, "index", 0));
            return -1;
        }
        /* A hash alone is not manifest-backed success. Legacy/unclaimed
         * artifacts cannot be carried into a paid retry without provenance. */
        const char *manifest = jm_str(item, "manifest_path");
        if (!manifest || !*manifest) {
            safe_err(err, errlen, "the successful image item has no generation manifest");
            return -1;
        }
        if (!tny_jobs_manifest_describes(jm_str(item, "manifest_path"), path, want, err, errlen))
            return -1;
        return 0;
    }
    const char *session_id = jm_str(item, "session_id");
    const char *want_result = jm_str(item, "result_sha256");
    const char *want_log = jm_str(item, "log_sha256");
    const char *log_path = jm_str(item, "log_path");
    if (!session_id || !want_result || !want_log || !log_path) {
        safe_err(err, errlen, "a successful ask item has no recorded session or hashes");
        return -1;
    }
    char *have_log = sha256_hex_file(log_path, NULL);
    bool ok = have_log && strcmp(have_log, want_log) == 0;
    free(have_log);
    char *have_result = ok ? tny_jobs_session_answer_sha(ctx, session_id) : NULL;
    ok = have_result && strcmp(have_result, want_result) == 0;
    free(have_result);
    if (!ok) {
        safe_err(err, errlen, "the successful session or log of item %lld is missing or changed",
                 (long long)jm_int(item, "index", 0));
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------- operations */

static int jobs_open_for_read(tny_ctx *ctx, yyjson_val *args, char **dir_out,
                              yyjson_mut_doc **doc_out, char *err, size_t errlen) {
    *dir_out = NULL;
    *doc_out = NULL;
    const char *id = jget_str(args, "id");
    if (!tny_jobs_valid_id(id)) {
        safe_err(err, errlen, "a 32-character lowercase hex job id is required");
        return 1;
    }
    char *dir = jobs_dir(ctx, id);
    if (!dir || !dir_exists(dir)) {
        free(dir);
        safe_err(err, errlen, "no job with that id in this workspace");
        return 1;
    }
    jobs_project(dir, id);
    yyjson_mut_doc *doc = jobs_record_load(dir, id, err, errlen);
    if (!doc) {
        free(dir);
        return 1;
    }
    *dir_out = dir;
    *doc_out = doc;
    return 0;
}

static int jobs_status(tny_ctx *ctx, yyjson_val *args, buf_t *out, char *err, size_t errlen) {
    char *dir = NULL;
    yyjson_mut_doc *doc = NULL;
    int rc = jobs_open_for_read(ctx, args, &dir, &doc, err, errlen);
    if (rc) return rc;
    job_json(doc, dir, out);
    const char *state = jm_str(yyjson_mut_doc_get_root(doc), "state");
    int exit_code = 0;
    if (state && (strcmp(state, "failed") == 0 || strcmp(state, "interrupted") == 0)) exit_code = 2;
    yyjson_mut_doc_free(doc);
    free(dir);
    return exit_code;
}

static int jobs_wait(tny_ctx *ctx, yyjson_val *args, buf_t *out, char *err, size_t errlen,
                     bool (*cancelled)(void *), void *cancel_ud) {
    int64_t timeout = jget_int(args, "timeout_s", 300);
    if (timeout < 0 || timeout > 86400) {
        safe_err(err, errlen, "timeout_s must be between 0 and 86400");
        return 1;
    }
    int64_t deadline = monotonic_ms() + timeout * 1000;
    for (;;) {
        if (cancelled && cancelled(cancel_ud)) {
            safe_err(err, errlen, "wait interrupted; the durable job is unchanged");
            return 130;
        }
        char *dir = NULL;
        yyjson_mut_doc *doc = NULL;
        int rc = jobs_open_for_read(ctx, args, &dir, &doc, err, errlen);
        if (rc) return rc;
        const char *state = jm_str(yyjson_mut_doc_get_root(doc), "state");
        bool terminal = state_is_terminal(state);
        int exit_code =
            state && (strcmp(state, "failed") == 0 || strcmp(state, "interrupted") == 0) ? 2 : 0;
        if (terminal) {
            job_json(doc, dir, out);
            yyjson_mut_doc_free(doc);
            free(dir);
            return exit_code;
        }
        if (monotonic_ms() >= deadline) {
            /* A deadline is not a cancellation: the job keeps running. */
            job_json(doc, dir, out);
            yyjson_mut_doc_free(doc);
            free(dir);
            safe_err(err, errlen, "the job is still running after the wait timeout");
            return TNY_JOBS_WAIT_TIMEOUT_EXIT;
        }
        yyjson_mut_doc_free(doc);
        free(dir);
        tny_jobs_host_sleep_ms(JOBS_POLL_MS);
    }
}

static bool selected_index(yyjson_val *args, int index, bool *had_selection) {
    yyjson_val *items = jget(args, "items");
    *had_selection = items && yyjson_is_arr(items) && yyjson_arr_size(items) > 0;
    if (!*had_selection) return true;
    size_t idx, max;
    yyjson_val *entry;
    yyjson_arr_foreach(items, idx, max, entry) {
        if (yyjson_is_int(entry) && yyjson_get_sint(entry) == index) return true;
    }
    return false;
}

static int jobs_cancel(tny_ctx *ctx, yyjson_val *args, buf_t *out, char *err, size_t errlen) {
    char *dir = NULL;
    yyjson_mut_doc *doc = NULL;
    int rc = jobs_open_for_read(ctx, args, &dir, &doc, err, errlen);
    if (rc) return rc;
    const char *id = jm_str(yyjson_mut_doc_get_root(doc), "id");
    char *job_id = xstrdup(id ? id : "");
    yyjson_mut_doc_free(doc);

    jobs_txn t;
    rc = jobs_txn_begin(dir, job_id, &t, err, errlen);
    if (rc) {
        free(job_id);
        free(dir);
        return 2;
    }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(t.doc);
    if (state_is_terminal(jm_str(root, "state"))) {
        jobs_txn_end(&t);
        yyjson_mut_doc *current = jobs_record_load(dir, job_id, err, errlen);
        if (current) {
            job_json(current, dir, out);
            yyjson_mut_doc_free(current);
        }
        free(job_id);
        free(dir);
        return 0; /* already finished: nothing to cancel, and nothing is lied about */
    }
    bool had_selection = false;
    int count = jm_item_count(t.doc);
    for (int i = 0; i < count; i++) {
        if (!selected_index(args, i, &had_selection)) continue;
        yyjson_mut_val *item = jm_item(t.doc, i);
        if (state_is_terminal(jm_str(item, "state"))) continue;
        jm_set_bool(t.doc, item, "cancel_requested", true);
    }
    if (!had_selection) jm_set_bool(t.doc, root, "cancel_requested", true);
    rc = jobs_txn_commit(&t);
    if (rc) {
        safe_err(err, errlen, "the cancellation request could not be recorded");
        free(job_id);
        free(dir);
        return 2;
    }
    yyjson_mut_doc *current = jobs_record_load(dir, job_id, err, errlen);
    if (current) {
        job_json(current, dir, out);
        yyjson_mut_doc_free(current);
    }
    free(job_id);
    free(dir);
    return 0;
}

static int jobs_logs(tny_ctx *ctx, yyjson_val *args, buf_t *out, char *err, size_t errlen) {
    char *dir = NULL;
    yyjson_mut_doc *doc = NULL;
    int rc = jobs_open_for_read(ctx, args, &dir, &doc, err, errlen);
    if (rc) return rc;
    int index = (int)jget_int(args, "item", 0);
    int64_t max_bytes = jget_int(args, "max_bytes", 16384);
    if (max_bytes < 1 || max_bytes > (int64_t)TNY_JOBS_LOG_READ_MAX)
        max_bytes = TNY_JOBS_LOG_READ_MAX;
    yyjson_mut_val *item = index >= 0 ? jm_item(doc, index) : NULL;
    if (!item) {
        yyjson_mut_doc_free(doc);
        free(dir);
        safe_err(err, errlen, "no such item in this job");
        return 1;
    }
    const char *log_path = jm_str(item, "log_path");
    size_t len = 0;
    char *data = log_path ? file_slurp(log_path, &len) : NULL;
    size_t shown = len > (size_t)max_bytes ? (size_t)max_bytes : len;
    const char *tail = data ? data + (len - shown) : "";
    buf_appends(out, "{\"kind\":\"job_logs\",\"schema_version\":1,\"id\":");
    jescape(out, jm_str(yyjson_mut_doc_get_root(doc), "id"));
    buf_appendf(out, ",\"item\":%d,\"log_path\":", index);
    jescape(out, log_path);
    buf_appendf(out, ",\"bytes\":%zu,\"truncated\":%s,\"text\":", len,
                shown < len ? "true" : "false");
    char *safe = xstrndup(tail, shown);
    jescape(out, safe ? safe : "");
    free(safe);
    buf_appends(out, "}\n");
    free(data);
    yyjson_mut_doc_free(doc);
    free(dir);
    return 0;
}

static int jobs_list(tny_ctx *ctx, buf_t *out, char *err, size_t errlen) {
#ifdef __EMSCRIPTEN__
    (void)ctx;
    (void)err;
    (void)errlen;
    buf_appends(out, "{\"kind\":\"job_list\",\"schema_version\":1,\"jobs\":[]}\n");
    return 0;
#else
    char *root = jobs_root(ctx);
    DIR *d = root ? opendir(root) : NULL;
    buf_appends(out, "{\"kind\":\"job_list\",\"schema_version\":1,\"jobs\":[");
    int shown = 0;
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d)) && shown < JOBS_LIST_MAX) {
            if (!tny_jobs_valid_id(entry->d_name)) continue;
            char *dir = path_join(root, entry->d_name);
            if (!dir) continue;
            jobs_project(dir, entry->d_name);
            yyjson_mut_doc *doc = jobs_record_load(dir, entry->d_name, NULL, 0);
            if (doc) {
                yyjson_mut_val *record = yyjson_mut_doc_get_root(doc);
                if (shown) buf_appends(out, ",");
                buf_appends(out, "{\"id\":");
                jescape(out, jm_str(record, "id"));
                buf_appends(out, ",\"job_kind\":");
                jescape(out, jm_str(record, "job_kind"));
                buf_appends(out, ",\"state\":");
                jescape(out, jm_str(record, "state"));
                buf_appends(out, ",\"created\":");
                jescape(out, jm_str(record, "created"));
                buf_appends(out, ",\"updated\":");
                jescape(out, jm_str(record, "updated"));
                buf_appendf(out, ",\"items\":%d,\"attempt\":%lld}", jm_item_count(doc),
                            (long long)jm_int(record, "attempt", 1));
                shown++;
                yyjson_mut_doc_free(doc);
            }
            free(dir);
        }
        closedir(d);
    }
    buf_appends(out, "]}\n");
    free(root);
    (void)err;
    (void)errlen;
    return 0;
#endif
}

static int jobs_rm(tny_ctx *ctx, yyjson_val *args, buf_t *out, char *err, size_t errlen) {
    char *dir = NULL;
    yyjson_mut_doc *doc = NULL;
    int rc = jobs_open_for_read(ctx, args, &dir, &doc, err, errlen);
    if (rc) return rc;
    char *id = xstrdup(jm_str(yyjson_mut_doc_get_root(doc), "id"));
    yyjson_mut_doc_free(doc);
    char *owner_path = jobs_file(dir, "owner.lock");
    int owner = owner_path ? tny_jobs_host_lock_open(owner_path) : -1;
    jobs_txn t = {0};
    if (!id || owner < 0 || tny_jobs_host_lock_try(owner) != TNY_JOBS_LOCK_ACQUIRED) {
        safe_err(err, errlen, "the job is owned; removal was refused");
        rc = 1;
        goto done;
    }
    rc = jobs_txn_begin(dir, id, &t, err, errlen);
    if (rc) goto done;
    if (!state_is_terminal(jm_str(yyjson_mut_doc_get_root(t.doc), "state"))) {
        safe_err(err, errlen, "only a finished job can be removed; cancel it first");
        jobs_txn_end(&t);
        rc = 1;
        goto done;
    }
    if (!cleanup_reclaimable(yyjson_mut_doc_get_root(t.doc))) {
        safe_err(err, errlen,
                 "cleanup is unverified; this job and its output claims must be retained");
        jobs_txn_end(&t);
        rc = 1;
        goto done;
    }
    /* Move the entire namespace out of lookup while BOTH locks are held.
     * A retry waiting on an old description cannot load job.json afterward;
     * no live worker ever owns the files we unlink. */
    buf_t tomb;
    buf_init(&tomb);
    buf_appendf(&tomb, "%s.removed", dir);
    if (buf_oom(&tomb) || rename(dir, tomb.data) != 0) {
        jobs_txn_end(&t);
        buf_free(&tomb);
        safe_err(err, errlen, "cannot tombstone the finished job");
        rc = 2;
        goto done;
    }
    reservations_release_job(ctx, t.doc, id);
    jobs_txn_end(&t);
#ifndef __EMSCRIPTEN__
    DIR *d = opendir(tomb.data);
    if (!d) rc = 2;
    else {
        struct dirent *entry;
        while ((entry = readdir(d))) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            char *victim = path_join(tomb.data, entry->d_name);
            if (!victim || unlink(victim) != 0) rc = 2;
            free(victim);
        }
        if (closedir(d) != 0) rc = 2;
    }
    if (rmdir(tomb.data) != 0) rc = 2;
#endif
    buf_free(&tomb);
    if (rc) safe_err(err, errlen, "job tombstoned; private directory cleanup is incomplete");
    else {
        buf_appends(out, "{\"kind\":\"job\",\"schema_version\":1,\"id\":");
        jescape(out, id);
        buf_appends(out, ",\"state\":\"removed\",\"ok\":true}\n");
    }
done:
    tny_jobs_host_lock_close(owner);
    free(owner_path);
    free(id);
    free(dir);
    return rc;
}

/* Rebuild a submit request from the stored per-item requests of an existing
 * job so a retry replays exactly the recorded selectors — never a credential,
 * and never a prompt that was deliberately not stored. */
static char *retry_request_json(yyjson_mut_doc *doc, const int *selected, int n, char *err,
                                size_t errlen) {
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "{\"kind\":");
    jescape(&b, jm_str(root, "job_kind"));
    buf_appendf(&b, ",\"concurrency\":%lld,\"items\":[",
                (long long)jm_int(root, "concurrency", TNY_JOBS_DEFAULT_CONCURRENCY));
    int count = jm_item_count(doc);
    for (int i = 0; i < count; i++) {
        yyjson_mut_val *item = jm_item(doc, i);
        bool chosen = false;
        for (int k = 0; k < n; k++)
            if (selected[k] == i) chosen = true;
        if (i) buf_appends(&b, ",");
        if (!chosen) {
            /* Placeholder for a carried item: never re-executed, so its
             * request body is irrelevant, but the index must line up. */
            buf_appends(&b, "{\"carried\":true}");
            continue;
        }
        yyjson_mut_val *stored = yyjson_mut_obj_get(item, "request");
        if (!stored || !yyjson_mut_is_obj(stored)) {
            safe_err(err, errlen,
                     "item %d was submitted without a stored request (privacy opt-out); submit a "
                     "new job with the prompt instead",
                     i);
            buf_free(&b);
            return NULL;
        }
        char *json = jwrite_mut_val(stored);
        if (!json) {
            buf_free(&b);
            return NULL;
        }
        buf_appends(&b, json);
        free(json);
    }
    buf_appends(&b, "]}");
    return buf_detach(&b);
}

static int jobs_retry(tny_ctx *ctx, yyjson_val *args, buf_t *out, char *err, size_t errlen,
                      bool (*cancelled)(void *), void *cancel_ud) {
    if (!tny_jobs_execution_supported()) {
        safe_err(err, errlen,
                 "durable jobs need a native tny build; this runtime cannot own a child process");
        return 1;
    }
    char *dir = NULL;
    yyjson_mut_doc *doc = NULL;
    int owner_fd = -1;
    int rc = jobs_open_for_read(ctx, args, &dir, &doc, err, errlen);
    if (rc) return rc;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    bool image = strcmp(jm_str(root, "job_kind") ? jm_str(root, "job_kind") : "ask", "image") == 0;
    char *job_id = xstrdup(jm_str(root, "id"));
    if (!state_is_terminal(jm_str(root, "state"))) {
        safe_err(err, errlen, "this job has not finished yet");
        goto invalid;
    }
    /* Ownership comes BEFORE any state mutation, exactly as submit does
     * (A14): the owner lock is what makes "this job is finished and unowned"
     * true for the whole preparation, so a second retry can neither reset a
     * live attempt nor write history for one. A contender that loses here has
     * changed nothing at all. */
    char *owner_path = jobs_file(dir, "owner.lock");
    owner_fd = owner_path ? tny_jobs_host_lock_open(owner_path) : -1;
    if (owner_fd < 0 || tny_jobs_host_lock_try(owner_fd) != TNY_JOBS_LOCK_ACQUIRED) {
        free(owner_path);
        safe_err(err, errlen,
                 "another owner holds this job; nothing was changed — check `tny jobs status %s`",
                 job_id ? job_id : "");
        goto invalid;
    }
    /* Re-read under ownership: the projection above was taken unlocked. */
    yyjson_mut_doc_free(doc);
    doc = jobs_record_load(dir, job_id, err, errlen);
    if (!doc) {
        free(owner_path);
        goto invalid;
    }
    root = yyjson_mut_doc_get_root(doc);
    image = strcmp(jm_str(root, "job_kind") ? jm_str(root, "job_kind") : "ask", "image") == 0;
    if (!state_is_terminal(jm_str(root, "state"))) {
        free(owner_path);
        safe_err(err, errlen, "this job has not finished yet");
        goto invalid;
    }
    /* What the selection, the verification and the new attempt number are all
     * derived from; the transaction below refuses to commit against anything
     * else. */
    int64_t base_revision = jm_int(root, "revision", 0);
    int base_attempt = (int)jm_int(root, "attempt", 1);

    bool failed_only = jget_bool(args, "failed", false);
    int selected[TNY_JOBS_MAX_ITEMS];
    int n_selected = 0;
    int count = jm_item_count(doc);
    for (int i = 0; i < count; i++) {
        yyjson_mut_val *item = jm_item(doc, i);
        const char *state = jm_str(item, "state");
        bool retryable =
            state && (strcmp(state, "failed") == 0 || strcmp(state, "cancelled") == 0 ||
                      strcmp(state, "interrupted") == 0);
        bool had_selection = false;
        bool wanted = selected_index(args, i, &had_selection);
        if (!had_selection && !failed_only) wanted = retryable; /* default: every failed item */
        if (!wanted) continue;
        if (!retryable) {
            /* A successful item is never selected, accidentally or not. */
            if (had_selection) {
                safe_err(err, errlen, "item %d already succeeded and is never re-executed", i);
                free(owner_path);
                goto invalid;
            }
            continue;
        }
        selected[n_selected++] = i;
    }
    if (!n_selected) {
        free(owner_path);
        safe_err(err, errlen, "no failed, cancelled or interrupted item to retry");
        goto invalid;
    }
    /* Verify every carried success before spending anything. */
    for (int i = 0; i < count; i++) {
        bool chosen = false;
        for (int k = 0; k < n_selected; k++)
            if (selected[k] == i) chosen = true;
        if (chosen) continue;
        yyjson_mut_val *item = jm_item(doc, i);
        const char *state = jm_str(item, "state");
        if (!state || strcmp(state, "succeeded") != 0) continue;
        if (verify_carried_success(ctx, item, image, err, errlen) != 0) {
            free(owner_path);
            tny_jobs_host_lock_close(owner_fd);
            buf_appends(out, "{\"kind\":\"job\",\"schema_version\":1,\"id\":");
            jescape(out, job_id ? job_id : "");
            buf_appends(out, ",\"ok\":false,\"code\":\"" TNY_JOBS_CODE_STALE "\",\"error\":");
            jescape(out, err);
            buf_appends(out, "}\n");
            yyjson_mut_doc_free(doc);
            free(job_id);
            free(dir);
            return 2;
        }
    }

    char *request_json = retry_request_json(doc, selected, n_selected, err, errlen);
    yyjson_doc *parsed = request_json ? jparse(request_json, strlen(request_json)) : NULL;
    free(request_json);
    jobs_request request;
    if (!parsed || jobs_request_parse_retry(ctx, yyjson_doc_get_root(parsed), &request, selected,
                                            n_selected, err, errlen) != 0) {
        yyjson_doc_free(parsed);
        free(owner_path);
        goto invalid;
    }

    int attempt = base_attempt + 1;
    /* One transaction resets the new attempt's controls: cancellation, timing,
     * errors and exit codes of the selected items only. Old attempt controls
     * are tagged with the old attempt and can never apply here. */
    jobs_txn t;
    rc = jobs_txn_begin(dir, job_id, &t, err, errlen);
    if (rc) {
        jobs_request_free(&request);
        yyjson_doc_free(parsed);
        free(owner_path);
        goto invalid;
    }
    yyjson_mut_val *live = yyjson_mut_doc_get_root(t.doc);
    /* Nothing is written unless the record under this lock is still exactly
     * the one the selection, the verification and `attempt` were derived
     * from. Otherwise the transaction is abandoned, not committed. */
    if (jm_int(live, "revision", 0) != base_revision ||
        jm_int(live, "attempt", 1) != base_attempt || !state_is_terminal(jm_str(live, "state"))) {
        jobs_txn_end(&t);
        jobs_request_free(&request);
        yyjson_doc_free(parsed);
        free(owner_path);
        safe_err(err, errlen,
                 "this job changed while the retry was being prepared; nothing was changed");
        goto invalid;
    }
    if (!cleanup_reclaimable(live)) {
        jobs_txn_end(&t);
        jobs_request_free(&request);
        yyjson_doc_free(parsed);
        free(owner_path);
        safe_err(err, errlen, "cleanup is unverified; retry cannot reuse this job's output claims");
        goto invalid;
    }
    /* Snapshot the finished attempt before the projection moves on. */
    char snapshot_name[32];
    snprintf(snapshot_name, sizeof snapshot_name, "attempt-%d.json", base_attempt);
    char *snapshot_path = jobs_file(dir, snapshot_name);
    char *snapshot = jwrite_pretty(t.doc);
    int snapshot_rc = snapshot_path && snapshot
                          ? tny_jobs_host_snapshot(snapshot_path, snapshot, strlen(snapshot))
                          : ENOMEM;
    free(snapshot_path);
    free(snapshot);
    if (snapshot_rc) {
        jobs_txn_end(&t);
        jobs_request_free(&request);
        yyjson_doc_free(parsed);
        free(owner_path);
        safe_err(err, errlen, "cannot preserve immutable attempt history; nothing was changed");
        goto invalid;
    }
    jm_set_int(t.doc, live, "attempt", attempt);
    jm_set_str(t.doc, live, "state", "queued");
    jm_set_bool(t.doc, live, "cancel_requested", false);
    jm_set_str(t.doc, live, "cleanup", "pending");
    jm_set_bool(t.doc, live, "cleanup_hold", false);
    jm_set_null(t.doc, live, "exit_code");
    jm_set_null(t.doc, live, "error_code");
    jm_set_null(t.doc, live, "error");
    for (int i = 0; i < count; i++) {
        yyjson_mut_val *item = jm_item(t.doc, i);
        bool chosen = false;
        for (int k = 0; k < n_selected; k++)
            if (selected[k] == i) chosen = true;
        if (!chosen) {
            if (jm_str(item, "state") && strcmp(jm_str(item, "state"), "succeeded") == 0)
                jm_set_int(t.doc, item, "carried_from_attempt", jm_int(item, "attempt", 1));
            continue;
        }
        jm_set_str(t.doc, item, "state", "queued");
        jm_set_bool(t.doc, item, "cancel_requested", false);
        jm_set_int(t.doc, item, "attempt", attempt);
        char *log = jobs_item_log(dir, i, attempt);
        jm_set_str(t.doc, item, "log_path", log);
        free(log);
        jm_set_int(t.doc, item, "carried_from_attempt", 0);
        jm_set_null(t.doc, item, "started");
        jm_set_null(t.doc, item, "finished");
        jm_set_null(t.doc, item, "exit_code");
        jm_set_null(t.doc, item, "error_code");
        jm_set_null(t.doc, item, "error");
    }
    rc = jobs_txn_commit(&t);
    free(owner_path);
    if (rc) {
        /* Still before acceptance: the record on disk is the old one. */
        jobs_request_free(&request);
        yyjson_doc_free(parsed);
        tny_jobs_host_lock_close(owner_fd);
        safe_err(err, errlen, "the retry could not be recorded; nothing was changed");
        yyjson_mut_doc_free(doc);
        free(job_id);
        free(dir);
        return 2;
    }

    /* Accepted: this attempt is now the record's truth, and this process
     * already holds the ownership that will pass to the supervisor. Every
     * failure from here on writes an honest terminal outcome. */
    char *self = tny_process_self_path();
    reservation_claim claims[TNY_JOBS_MAX_ITEMS];
    int indexes[TNY_JOBS_MAX_ITEMS];
    int n_claims = 0;
    for (int k = 0; k < n_selected; k++) {
        int i = selected[k];
        if (!request.outputs[i]) continue;
        claims[n_claims].path = request.outputs[i];
        indexes[n_claims] = i;
        n_claims++;
    }
    if (!self) safe_err(err, errlen, "cannot identify this executable");
    rc = !self ? EIO
         : n_claims
             ? reservations_claim_all(ctx, job_id, attempt, claims, indexes, n_claims, err, errlen)
             : 0;
    if (!rc) rc = outputs_revalidate(&request, indexes, n_claims, err, errlen);
    bool claim_refused = rc != 0 && self;
    char *payload =
        rc ? NULL : payload_build(ctx, &request, job_id, attempt, self, selected, n_selected);
    if (!rc && !payload)
        safe_err(err, errlen,
                 "cannot build private request; check jobs.ask_env declarations and memory "
                 "availability");
    jobs_launch launch = {0};
    if (!rc)
        rc = payload ? jobs_launch_worker(self, dir, job_id, owner_fd, payload, &launch, err,
                                          errlen, cancelled, cancel_ud)
                     : ENOMEM;
    if (payload) secure_free(payload);
    int exit_code = 0;
    if (rc && !launch.live) {
        /* The attempt was accepted but nothing runs it: say so terminally
         * rather than leave an ownerless "queued" promise (A14). */
        for (int i = 0; i < n_claims; i++)
            reservation_release_one(ctx, claims[i].path, job_id, indexes[i], attempt);
        submit_finish_failed(dir, attempt,
                             claim_refused ? TNY_JOBS_CODE_OUTPUT_BUSY : TNY_JOBS_CODE_IO, err);
        exit_code = 2;
    } else if (rc) {
        safe_err(err, errlen,
                 "the supervisor did not acknowledge attempt %d of job %s; check `tny jobs status "
                 "%s`",
                 attempt, job_id, job_id);
        exit_code = 2;
    }
    tny_jobs_host_lock_close(owner_fd);
    free(self);
    jobs_request_free(&request);
    yyjson_doc_free(parsed);
    yyjson_mut_doc_free(doc);
    yyjson_mut_doc *current = jobs_record_load(dir, job_id, NULL, 0);
    if (current) {
        job_json(current, dir, out);
        yyjson_mut_doc_free(current);
    }
    free(job_id);
    free(dir);
    return exit_code;

invalid:
    /* Every path here refused before acceptance, so releasing ownership
     * leaves the job exactly as it was found. */
    if (owner_fd >= 0) tny_jobs_host_lock_close(owner_fd);
    yyjson_mut_doc_free(doc);
    free(job_id);
    free(dir);
    return 1;
}

int tny_jobs_run(tny_ctx *ctx, tny_jobs_op op, yyjson_val *args, buf_t *out, char *err,
                 size_t errlen) {
    return tny_jobs_run_cancel(ctx, op, args, out, err, errlen, NULL, NULL);
}

int tny_jobs_run_cancel(tny_ctx *ctx, tny_jobs_op op, yyjson_val *args, buf_t *out, char *err,
                        size_t errlen, bool (*cancelled)(void *), void *cancel_ud) {
    if (err && errlen) err[0] = 0;
    if (!ctx || !out) return 1;
    char *root = jobs_root(ctx);
    int rc = root ? tny_jobs_host_mkdir_private(root) : ENOMEM;
    free(root);
    if (rc && op != TNY_JOBS_OP_LIST) {
        safe_err(err, errlen, "cannot create the private jobs directory");
        return 2;
    }
    switch (op) {
    case TNY_JOBS_OP_SUBMIT: return jobs_submit(ctx, args, out, err, errlen, cancelled, cancel_ud);
    case TNY_JOBS_OP_STATUS: return jobs_status(ctx, args, out, err, errlen);
    case TNY_JOBS_OP_WAIT: return jobs_wait(ctx, args, out, err, errlen, cancelled, cancel_ud);
    case TNY_JOBS_OP_CANCEL: return jobs_cancel(ctx, args, out, err, errlen);
    case TNY_JOBS_OP_RETRY: return jobs_retry(ctx, args, out, err, errlen, cancelled, cancel_ud);
    case TNY_JOBS_OP_LOGS: return jobs_logs(ctx, args, out, err, errlen);
    case TNY_JOBS_OP_LIST: return jobs_list(ctx, out, err, errlen);
    case TNY_JOBS_OP_RM: return jobs_rm(ctx, args, out, err, errlen);
    case TNY_JOBS_OP_NONE: break;
    }
    safe_err(err, errlen, "unknown jobs operation");
    return 1;
}

/* ============================ the detached supervisor ==================== */

typedef struct {
    int index;
    bool active, reaped, eof, limit_hit, launched;
    pid_t pid;
    int in_fd, out_fd, log_fd;
    size_t written, log_bytes;
    int status;
    bool cancel_signalled, killed, cleanup_unknown, reap_error;
    int64_t cancel_deadline, drain_deadline;
    char *prompt; /* private: never written to the job directory */
    char *log_path;
    char *output_path; /* image items only */
    bool image;
    tny_process_scope *scope;
    bool launch_pending, launch_failed, admission_ready, released;
    bool admission_failed, cleanup_done, residual_stopped;
} job_slot;

static void slot_close(job_slot *s) {
    if (s->in_fd >= 0) close(s->in_fd);
    if (s->out_fd >= 0) close(s->out_fd);
    if (s->log_fd >= 0) close(s->log_fd);
    s->in_fd = s->out_fd = s->log_fd = -1;
}

static void slot_free(job_slot *s) {
    slot_close(s);
    if (s->scope && tny_process_scope_destroy(s->scope) == 0) s->scope = NULL;
    if (s->prompt) secure_free(s->prompt);
    free(s->log_path);
    free(s->output_path);
    memset(s, 0, sizeof *s);
    s->in_fd = s->out_fd = s->log_fd = -1;
    s->pid = -1;
}

/* A child gets an operational environment, not a copy of the submitter's
 * credentials. Unknown names (including custom api_key_env and arbitrary
 * --api-key-env names) are denied. Provider allowances are appended only from
 * the selected private mapping below. No suffix denylist can cover arbitrary
 * user-selected credential names. */
static const char *const JOBS_OPERATIONAL_ENV[] = {"PATH",
                                                   "HOME",
                                                   "USER",
                                                   "LOGNAME",
                                                   "SHELL",
                                                   "TMPDIR",
                                                   "TMP",
                                                   "TEMP",
                                                   "LANG",
                                                   "LC_ALL",
                                                   "LC_CTYPE",
                                                   "TZ",
                                                   "TERM",
                                                   "NO_COLOR",
                                                   "CODEX_HOME",
                                                   "XDG_CONFIG_HOME",
                                                   "XDG_CACHE_HOME",
                                                   "XDG_DATA_HOME",
                                                   "XDG_RUNTIME_DIR",
                                                   "SSL_CERT_FILE",
                                                   "SSL_CERT_DIR",
                                                   "SYSTEMROOT",
                                                   "SystemRoot",
                                                   "COMSPEC",
                                                   "PATHEXT",
                                                   "USERPROFILE",
                                                   "LOCALAPPDATA",
                                                   "APPDATA"};

static bool env_name_is(const char *entry, const char *name) {
    size_t len = strlen(name);
    return strncmp(entry, name, len) == 0 && entry[len] == '=';
}

bool tny_jobs_env_entry_is_foreign(const char *entry, bool image, bool chat_codex) {
    (void)image;
    (void)chat_codex;
    if (!entry) return false;
    for (size_t i = 0; i < sizeof JOBS_OPERATIONAL_ENV / sizeof JOBS_OPERATIONAL_ENV[0]; i++)
        if (env_name_is(entry, JOBS_OPERATIONAL_ENV[i])) return false;
    return true;
}

/* The child's environment: private credential carriers and the parent-watch
 * value only. Nothing secret reaches argv, the job directory or a log, and no
 * inherited carrier of the other kind survives (A14). */
static char **worker_child_env(yyjson_val *payload, bool image, char ***owned_out, int *n_owned) {
    yyjson_val *chat = jget(payload, "chat");
    yyjson_val *image_creds = jget(payload, "image");
    bool chat_codex = jget_bool(chat, "codex", false);
    const char *api_key = image ? NULL : jget_str(chat, "api_key");
    const char *base_url = image ? NULL : jget_str(chat, "base_url");
    /* Exactly one side of the split supplies these, never both. */
    const char *token = image ? jget_str(image_creds, "token") : jget_str(chat, "token");
    const char *account = image ? jget_str(image_creds, "account") : jget_str(chat, "account");
    char **owned = calloc(44, sizeof *owned);
    if (!owned) return NULL;
    int n = 0;
    buf_t entry;
    char parent[64];
    snprintf(parent, sizeof parent, "%s=%lld", TNY_JOB_PARENT_ENV, (long long)getpid());
    owned[n++] = xstrdup(parent);
    if (!image) {
        buf_init(&entry);
        buf_appendf(&entry, "TNY_NESTED_MODE=%s", jget_str(payload, "perm_mode"));
        owned[n++] = buf_detach(&entry);
        buf_init(&entry);
        buf_appendf(&entry, "TNY_TOOLS=%s", jget_str(payload, "tools"));
        owned[n++] = buf_detach(&entry);
        owned[n++] = xstrdup("TNY_NESTED=1");
    }
    if (api_key) {
        buf_init(&entry);
        buf_appendf(&entry, "%s=", JOBS_ENV_API_KEY);
        buf_appends(&entry, api_key);
        owned[n++] = buf_detach(&entry);
    }
    if (base_url) {
        buf_init(&entry);
        buf_appendf(&entry, "%s=", JOBS_ENV_BASE_URL);
        buf_appends(&entry, base_url);
        owned[n++] = buf_detach(&entry);
    }
    if (token) {
        buf_init(&entry);
        buf_appends(&entry, "CHATGPT_ACCESS_TOKEN=");
        buf_appends(&entry, token);
        owned[n++] = buf_detach(&entry);
    }
    if (account) {
        buf_init(&entry);
        buf_appends(&entry, "CHATGPT_ACCOUNT_ID=");
        buf_appends(&entry, account);
        owned[n++] = buf_detach(&entry);
    }
    const char *codex_url = jget_str(image ? image_creds : chat, "codex_url");
    if (codex_url) {
        buf_init(&entry);
        buf_appendf(&entry, "TNY_CODEX_BASE_URL=%s", codex_url);
        owned[n++] = buf_detach(&entry);
    }
    const char *cursor_key = image ? NULL : jget_str(chat, "cursor_key");
    if (cursor_key) {
        buf_init(&entry);
        buf_appendf(&entry, "CURSOR_API_KEY=%s", cursor_key);
        owned[n++] = buf_detach(&entry);
    }
    yyjson_val *declared = image ? NULL : jget(payload, "ask_env");
    if (yyjson_is_obj(declared) && yyjson_obj_size(declared) <= 32) {
        size_t i, max;
        yyjson_val *key, *value;
        yyjson_obj_foreach(declared, i, max, key, value) {
            if (!yyjson_is_str(value) || tny_process_scope_env_reserved(yyjson_get_str(key)))
                continue;
            buf_init(&entry);
            buf_appendf(&entry, "%s=%s", yyjson_get_str(key), yyjson_get_str(value));
            owned[n++] = buf_detach(&entry);
        }
    }
    for (int i = 0; i < n; i++)
        if (!owned[i]) {
            for (int j = 0; j < n; j++) secure_free(owned[j]);
            free(owned);
            return NULL;
        }
    size_t inherited = 0;
    while (environ && environ[inherited]) inherited++;
    char **envp = calloc(inherited + (size_t)n + 1, sizeof *envp);
    if (!envp) {
        for (int i = 0; i < n; i++) secure_free(owned[i]);
        free(owned);
        return NULL;
    }
    size_t used = 0;
    for (size_t i = 0; i < inherited; i++) {
        bool skip = tny_jobs_env_entry_is_foreign(environ[i], image, chat_codex);
        size_t bi, bm;
        yyjson_val *carrier;
        yyjson_arr_foreach(jget(payload, "blocked_env"), bi, bm, carrier) {
            const char *name = yyjson_get_str(carrier);
            if (name && env_name_is(environ[i], name)) skip = true;
        }
        const char *value = strchr(environ[i], '=');
        const char *secrets[] = {jget_str(chat, "api_key"),         jget_str(chat, "token"),
                                 jget_str(chat, "account"),         jget_str(image_creds, "token"),
                                 jget_str(image_creds, "account"),  jget_str(chat, "cursor_key"),
                                 jget_str(chat, "base_url"),        jget_str(chat, "codex_url"),
                                 jget_str(image_creds, "codex_url")};
        for (size_t k = 0; value && k < sizeof secrets / sizeof secrets[0]; k++)
            if (secrets[k] && *secrets[k] && strcmp(value + 1, secrets[k]) == 0) skip = true;
        /* A carrier this item legitimately uses is still replaced by the
         * resolved value, never shadowed by the caller's ambient one. */
        if (token && env_name_is(environ[i], "CHATGPT_ACCESS_TOKEN")) skip = true;
        if (account && env_name_is(environ[i], "CHATGPT_ACCOUNT_ID")) skip = true;
        for (int k = 0; !skip && k < n; k++) {
            const char *eq = strchr(owned[k], '=');
            if (eq && strncmp(environ[i], owned[k], (size_t)(eq - owned[k]) + 1) == 0) skip = true;
        }
        if (!skip) envp[used++] = environ[i];
    }
    for (int i = 0; i < n; i++) envp[used++] = owned[i];
    envp[used] = NULL;
    *owned_out = owned;
    *n_owned = n;
    return envp;
}

static void worker_env_free(char **envp, char **owned, int n_owned) {
    for (int i = 0; i < n_owned; i++) secure_free(owned[i]);
    free(owned);
    free(envp);
}

/* argv carries selectors only: the canonical ask/image CLI this build already
 * ships, never a credential and never the prompt. */
static int worker_build_argv(yyjson_val *payload, yyjson_val *item, bool image, char **argv,
                             int cap) {
    int n = 0;
    const char *self = jget_str(payload, "self");
    const char *cwd = jget_str(payload, "cwd");
    yyjson_val *chat = jget(payload, "chat");
    if (!self || !cwd || cap < 24) return -1;
    argv[n++] = (char *)self;
    argv[n++] = (char *)"--cwd";
    argv[n++] = (char *)cwd;
    if (image) {
        argv[n++] = (char *)"image";
        if (!jget_bool(item, "overwrite", false)) argv[n++] = (char *)"--job-no-replace";
        const char *operation = jget_str(item, "operation");
        argv[n++] = (char *)(operation && strcmp(operation, "edit") == 0 ? "edit" : "generate");
        argv[n++] = (char *)"--json";
        argv[n++] = (char *)"--output-file";
        argv[n++] = (char *)jget_str(item, "output_file");
        static const char *const flags[] = {"--model", "--quality", "--size"};
        static const char *const keys[] = {"model", "quality", "size"};
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
            const char *value = jget_str(item, keys[i]);
            if (!value) continue;
            argv[n++] = (char *)flags[i];
            argv[n++] = (char *)value;
        }
        if (jget_bool(item, "strict_size", false)) argv[n++] = (char *)"--strict-size";
        if (!jget_bool(item, "persist_manifest", true)) argv[n++] = (char *)"--no-manifest";
        yyjson_val *images = jget(item, "images");
        if (images && yyjson_is_arr(images)) {
            size_t idx, max;
            yyjson_val *reference;
            yyjson_arr_foreach(images, idx, max, reference) {
                if (n + 4 >= cap) break;
                argv[n++] = (char *)"--image";
                argv[n++] = (char *)yyjson_get_str(reference);
            }
        }
        argv[n] = NULL;
        return jget_str(item, "output_file") ? 0 : -1;
    }
    const char *provider = jget_str(payload, "provider");
    if (provider) {
        argv[n++] = (char *)"--provider";
        argv[n++] = (char *)provider;
    }
    if (jget_str(chat, "api_key")) {
        argv[n++] = (char *)"--api-key-env";
        argv[n++] = (char *)JOBS_ENV_API_KEY;
    }
    if (jget_str(chat, "base_url")) {
        argv[n++] = (char *)"--base-url-env";
        argv[n++] = (char *)JOBS_ENV_BASE_URL;
    }
    if (jget_str(chat, "wire_api")) {
        argv[n++] = (char *)"--wire-api";
        argv[n++] = (char *)jget_str(chat, "wire_api");
    }
    const char *model = jget_str(item, "model") ? jget_str(item, "model") : jget_str(chat, "model");
    if (model) {
        argv[n++] = (char *)"--model";
        argv[n++] = (char *)model;
    }
    const char *effort =
        jget_str(item, "effort") ? jget_str(item, "effort") : jget_str(chat, "effort");
    if (effort) {
        argv[n++] = (char *)"--effort";
        argv[n++] = (char *)effort;
    }
    /* The child can never raise the permission ceiling it was given. */
    argv[n++] = (char *)"--permission-mode";
    argv[n++] = (char *)(jget_str(payload, "perm_mode") ? jget_str(payload, "perm_mode") : "ask");
    argv[n++] = (char *)"ask";
    argv[n++] = (char *)"--events=jsonl";
    argv[n++] = (char *)"--progress=none";
    argv[n++] = (char *)"--stdin";
    if (jget_str(item, "task")) {
        argv[n++] = (char *)"--task";
        argv[n++] = (char *)jget_str(item, "task");
    }
    argv[n] = NULL;
    return 0;
}

static int worker_spawn_item(yyjson_val *payload, yyjson_val *item, bool image, job_slot *slot,
                             char *err, size_t errlen) {
    slot->image = image;
    if (image) {
        char *canonical = NULL;
        int valid = validate_image_item(NULL, item, &canonical, err, errlen);
        bool same = canonical && slot->output_path && strcmp(canonical, slot->output_path) == 0;
        free(canonical);
        if (valid || !same) {
            if (!valid) safe_err(err, errlen, "output identity changed before item launch");
            return -1;
        }
    }
    int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1};
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        for (int i = 0; i < 2; i++) {
            if (in_pipe[i] >= 0) close(in_pipe[i]);
            if (out_pipe[i] >= 0) close(out_pipe[i]);
        }
        safe_err(err, errlen, "cannot create the item pipes");
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(in_pipe[i], F_SETFD, FD_CLOEXEC);
        fcntl(out_pipe[i], F_SETFD, FD_CLOEXEC);
    }
    slot->log_fd = open(slot->log_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    char *argv[32];
    char **owned = NULL;
    int n_owned = 0;
    char **envp = worker_child_env(payload, image, &owned, &n_owned);
    int rc = slot->log_fd < 0 || !envp || worker_build_argv(payload, item, image, argv, 32) != 0
                 ? EINVAL
                 : 0;
    pid_t pid = -1;
    if (!rc) {
        if (tny_process_scope_native_jobs()) {
            rc = tny_process_scope_spawn(argv, envp, in_pipe[0], out_pipe[1], &slot->scope);
            if (!rc) pid = tny_process_scope_pid(slot->scope);
        } else {
            const tny_fd_mapping maps[] = {{in_pipe[0], 0}, {out_pipe[1], 1}};
            rc = tny_process_spawn_mapped(argv, envp, maps, 2, &pid);
        }
    }
    worker_env_free(envp, owned, n_owned);
    close(in_pipe[0]);
    close(out_pipe[1]);
    if (rc) {
        close(in_pipe[1]);
        close(out_pipe[0]);
        if (slot->log_fd >= 0) close(slot->log_fd);
        slot->log_fd = -1;
        safe_err(err, errlen, "cannot start the item process");
        return -1;
    }
    slot->pid = pid;
    slot->released = slot->scope == NULL;
    slot->in_fd = in_pipe[1];
    slot->out_fd = out_pipe[0];
    slot->active = true;
    slot->launched = true;
    slot->written = 0;
    slot->log_bytes = 0;
    return 0;
}

/* One poll round over every live item: feed the private prompt, drain the
 * child's own stdout into its bounded log, and reap what has exited. */
static void worker_pump(job_slot *slots, int n_slots) {
    struct pollfd fds[2 * TNY_JOBS_MAX_CONCURRENCY];
    int owner[2 * TNY_JOBS_MAX_CONCURRENCY];
    bool writer[2 * TNY_JOBS_MAX_CONCURRENCY];
    int n = 0;
    for (int i = 0; i < n_slots && n + 2 <= (int)(sizeof fds / sizeof fds[0]); i++) {
        if (!slots[i].active) continue;
        if (slots[i].in_fd >= 0 && slots[i].released) {
            fds[n].fd = slots[i].in_fd;
            fds[n].events = POLLOUT;
            fds[n].revents = 0;
            owner[n] = i;
            writer[n] = true;
            n++;
        }
        if (slots[i].out_fd >= 0 && !slots[i].eof) {
            fds[n].fd = slots[i].out_fd;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            owner[n] = i;
            writer[n] = false;
            n++;
        }
    }
    int pr = tny_poll(n ? fds : NULL, n, JOBS_POLL_MS);
    if (pr < 0 && errno != EINTR) return;
    for (int k = 0; k < n && pr > 0; k++) {
        job_slot *s = &slots[owner[k]];
        if (!fds[k].revents) continue;
        if (writer[k]) {
            size_t len = s->prompt ? strlen(s->prompt) : 0;
            if (s->written < len) {
                ssize_t written = write(s->in_fd, s->prompt + s->written, len - s->written);
                if (written > 0) s->written += (size_t)written;
                else if (written < 0 && errno != EINTR && errno != EAGAIN) s->written = len;
            }
            if (s->written >= len) {
                close(s->in_fd); /* EOF: the child's prompt is complete */
                s->in_fd = -1;
            }
            continue;
        }
        char chunk[8192];
        ssize_t got = read(s->out_fd, chunk, sizeof chunk);
        if (got == 0 || (got < 0 && errno != EINTR && errno != EAGAIN)) {
            s->eof = true;
            continue;
        }
        if (got <= 0) continue;
        if (s->log_bytes + (size_t)got > TNY_JOBS_LOG_MAX) {
            /* A pathological item is cancelled and reported, never truncated
             * into a success. */
            s->limit_hit = true;
            s->eof = true;
            continue;
        }
        for (ssize_t off = 0; off < got;) {
            ssize_t put = write(s->log_fd, chunk + off, (size_t)(got - off));
            if (put < 0 && errno == EINTR) continue;
            if (put <= 0) break;
            off += put;
        }
        s->log_bytes += (size_t)got;
    }
}

/* Admission, consuming waits and complete native scope cleanup run outside
 * state transactions. The lock only linearizes the nonblocking GO decision. */
static void worker_scope_progress(job_slot *s) {
    if (!s->active || !s->scope || s->cleanup_done) return;
    if (!s->released && !s->killed && !s->admission_failed) {
        int ack = tny_process_scope_ack(s->scope);
        if (ack == 1) s->admission_ready = true;
        else if (ack < 0) s->admission_failed = true;
    }
    int status = 0;
    int reaped = tny_process_scope_reap(s->scope, &status);
    if (reaped == 1 && !s->reaped) {
        s->reaped = true;
        s->status = status;
        s->drain_deadline = monotonic_ms() + 1000;
        if (!s->released && !s->killed) s->admission_failed = true;
    } else if (reaped < 0) {
        s->reap_error = true;
        s->cleanup_unknown = true;
    }
    if (s->admission_failed) s->killed = true;
    if (!s->killed && !s->reaped && !s->reap_error) return;
    if (s->in_fd >= 0) {
        close(s->in_fd); /* permanently forbid GO or more prompt bytes */
        s->in_fd = -1;
    }
    bool forced = false;
    int cleanup = tny_process_scope_cleanup(s->scope, s->killed, &forced);
    if (forced && !s->killed) s->residual_stopped = true;
    if (cleanup != 0) {
        s->cleanup_done = true;
        if (cleanup < 0) s->cleanup_unknown = true;
        reaped = tny_process_scope_reap(s->scope, &status);
        if (reaped == 1) {
            s->reaped = true;
            s->status = status;
        } else s->reap_error = true;
        s->drain_deadline = monotonic_ms() + 1000;
    }
}

/* Ask success needs an actual canonical turn_end, not merely exit 0. */
static bool analyze_ask_log(const char *path, char **session_id, char **log_sha) {
    *session_id = NULL;
    *log_sha = NULL;
    size_t len = 0;
    char *data = file_slurp(path, &len);
    if (!data) return false;
    *log_sha = sha256_hex_of(data, len);
    bool terminal = false;
    char *line = data;
    while (line && (size_t)(line - data) < len) {
        char *end = memchr(line, '\n', len - (size_t)(line - data));
        size_t line_len = end ? (size_t)(end - line) : len - (size_t)(line - data);
        yyjson_doc *doc = line_len ? jparse(line, line_len) : NULL;
        yyjson_val *event = doc ? yyjson_doc_get_root(doc) : NULL;
        const char *type = jget_str(event, "type");
        const char *session = jget_str(event, "session_id");
        if (session && *session && !*session_id) *session_id = xstrdup(session);
        if (type && strcmp(type, "turn_end") == 0 && jget_int(event, "stop_reason", -1) == 0)
            terminal = true;
        yyjson_doc_free(doc);
        if (!end) break;
        line = end + 1;
    }
    free(data);
    /* A canonical terminal event, not merely a zero exit status. */
    return terminal && *session_id && *log_sha;
}

/* Image success is the shared CLI's own result object plus the bytes it
 * committed. The manifest pointer and operation id are read literally from
 * that result when the image service supplies them, and stay null otherwise —
 * this component never invents provenance of its own (#127 integration). */
static bool analyze_image_log(const char *path, const char *expected_output, char **output_sha,
                              long long *bytes, char **manifest, char **operation_id) {
    *output_sha = NULL;
    *bytes = -1;
    *manifest = NULL;
    *operation_id = NULL;
    buf_t data;
    buf_init(&data);
    if (tny_image_io_read_bounded(path, TNY_JOBS_LOG_MAX, &data) != 0) {
        buf_free(&data);
        return false;
    }
    yyjson_doc *doc = jparse(data.data, data.len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *kind = artifact_string(root, "kind", 16);
    const char *committed = jget_str(root, "path");
    const char *producer_sha = jget_str(root, "sha256");
    yyjson_val *producer_bytes = jget(root, "bytes");
    yyjson_val *pointer = jget(root, "manifest_path");
    yyjson_val *operation = jget(root, "operation_id");
    bool ok =
        kind && strcmp(kind, "image") == 0 && jget_bool(root, "ok", false) && committed &&
        expected_output && strcmp(committed, expected_output) == 0 &&
        yyjson_get_len(jget(root, "path")) == strlen(committed) &&
        tny_image_preview_hash_valid(producer_sha) && yyjson_get_len(jget(root, "sha256")) == 64 &&
        yyjson_is_uint(producer_bytes) && yyjson_get_uint(producer_bytes) > 0 &&
        yyjson_get_uint(producer_bytes) <= TNY_IMAGE_OUTPUT_MAX && pointer &&
        (yyjson_is_null(pointer) || (yyjson_is_str(pointer) && yyjson_get_len(pointer) > 0 &&
                                     yyjson_get_len(pointer) < TNY_IMAGE_MANIFEST_PATH_MAX &&
                                     yyjson_get_len(pointer) == strlen(yyjson_get_str(pointer)))) &&
        yyjson_is_str(operation) && yyjson_get_len(operation) > 0 &&
        yyjson_get_len(operation) < TNY_IMAGE_OPERATION_ID_MAX &&
        yyjson_get_len(operation) == strlen(yyjson_get_str(operation));
    if (ok) {
        buf_t image;
        buf_init(&image);
        ok = tny_image_io_read_bounded(committed, TNY_IMAGE_OUTPUT_MAX, &image) == 0 &&
             image.len == yyjson_get_uint(producer_bytes) &&
             tny_image_preview_hash_matches((const uint8_t *)image.data, image.len, producer_sha);
        buf_free(&image);
        if (ok) {
            /* Preserve the producer identity; never bless replacement disk bytes. */
            *output_sha = xstrdup(producer_sha);
            *bytes = (long long)yyjson_get_uint(producer_bytes);
            if (yyjson_is_str(pointer)) *manifest = xstrdup(yyjson_get_str(pointer));
            *operation_id = xstrdup(yyjson_get_str(operation));
            ok = *output_sha && *operation_id && (!yyjson_is_str(pointer) || *manifest);
            if (!ok) {
                free(*output_sha);
                free(*operation_id);
                free(*manifest);
                *output_sha = *operation_id = *manifest = NULL;
                *bytes = -1;
            }
        }
    }
    yyjson_doc_free(doc);
    buf_free(&data);
    return ok;
}

static void worker_record_result(tny_ctx *ctx, yyjson_mut_doc *doc, job_slot *slot,
                                 bool cancelled) {
    yyjson_mut_val *item = jm_item(doc, slot->index);
    if (!item) return;
    char *now = now_iso8601();
    jm_set_str(doc, item, "finished", now ? now : "");
    free(now);
    int exit_code = WIFEXITED(slot->status) ? WEXITSTATUS(slot->status) : -1;
    jm_set_int(doc, item, "exit_code", exit_code);
    if (slot->reap_error) {
        jm_set_str(doc, item, "state", "interrupted");
        jm_set_null(doc, item, "exit_code");
        jm_set_str(doc, item, "error_code", TNY_JOBS_CODE_IO);
        jm_set_str(doc, item, "error",
                   "the owned child's exit could not be observed; cleanup is unknown");
        return;
    }
    if (slot->limit_hit) {
        jm_set_str(doc, item, "state", "failed");
        jm_set_str(doc, item, "error_code", TNY_JOBS_CODE_OUTPUT_LIMIT);
        jm_set_str(doc, item, "error",
                   "the item produced more output than the bounded log allows and was stopped; the "
                   "log is incomplete");
        return;
    }
    if (cancelled) {
        jm_set_str(doc, item, "state", "cancelled");
        jm_set_str(doc, item, "error_code", NULL);
        jm_set_str(doc, item, "error", "cancelled on request");
        return;
    }
    if (slot->admission_failed || slot->residual_stopped || slot->cleanup_unknown) {
        jm_set_str(doc, item, "state", "failed");
        jm_set_str(doc, item, "error_code", TNY_JOBS_CODE_IO);
        jm_set_str(doc, item, "error",
                   slot->admission_failed ? "the private item admission did not complete"
                   : slot->residual_stopped
                       ? "the item left running descendants that required cleanup"
                       : "the owned scope cleanup could not be verified");
        return;
    }
    bool ok = false;
    if (slot->image) {
        char *sha = NULL, *manifest = NULL, *operation_id = NULL;
        long long bytes = -1;
        ok = exit_code == 0 && analyze_image_log(slot->log_path, slot->output_path, &sha, &bytes,
                                                 &manifest, &operation_id);
        if (ok) {
            jm_set_str(doc, item, "output_sha256", sha);
            jm_set_int(doc, item, "output_bytes", bytes);
            jm_set_str(doc, item, "manifest_path", manifest);
            jm_set_str(doc, item, "operation_id", operation_id);
        }
        free(sha);
        free(manifest);
        free(operation_id);
    } else {
        char *session = NULL, *answer_sha = NULL, *log_sha = NULL;
        ok = exit_code == 0 && analyze_ask_log(slot->log_path, &session, &log_sha);
        /* Success also requires the durable session this item claims: the
         * recorded hash is of its stored answer, which is exactly what a later
         * retry re-verifies before carrying it forward. */
        if (ok) answer_sha = tny_jobs_session_answer_sha(ctx, session);
        ok = ok && answer_sha != NULL;
        if (ok) {
            jm_set_str(doc, item, "session_id", session);
            jm_set_str(doc, item, "result_sha256", answer_sha);
            jm_set_str(doc, item, "log_sha256", log_sha);
        }
        free(session);
        free(answer_sha);
        free(log_sha);
    }
    if (ok) {
        jm_set_str(doc, item, "state", "succeeded");
        jm_set_str(doc, item, "error_code", NULL);
        jm_set_str(doc, item, "error", NULL);
        return;
    }
    jm_set_str(doc, item, "state", "failed");
    jm_set_str(doc, item, "error_code", TNY_JOBS_CODE_IO);
    /* Only safe local categories: the child's stderr was discarded, and no
     * provider body or configuration ever enters this record. */
    if (WIFSIGNALED(slot->status))
        jm_set_str(doc, item, "error", "the item process was terminated by a signal");
    else if (exit_code != 0)
        jm_set_str(doc, item, "error",
                   slot->image ? "the image command exited with a failure status"
                               : "the ask command exited with a failure status");
    else
        jm_set_str(doc, item, "error",
                   slot->image ? "the image command exited 0 without committing a verified output"
                               : "the ask command exited 0 without a completed canonical turn");
}

static int worker_supervise(tny_ctx *ctx, const char *dir, const char *id, yyjson_val *payload) {
    yyjson_val *items = jget(payload, "items");
    bool image =
        strcmp(jget_str(payload, "kind") ? jget_str(payload, "kind") : "ask", "image") == 0;
    int concurrency = (int)jget_int(payload, "concurrency", TNY_JOBS_DEFAULT_CONCURRENCY);
    if (concurrency < 1) concurrency = 1;
    if (concurrency > TNY_JOBS_MAX_CONCURRENCY) concurrency = TNY_JOBS_MAX_CONCURRENCY;
    size_t n_payload = items && yyjson_is_arr(items) ? yyjson_arr_size(items) : 0;
    job_slot slots[TNY_JOBS_MAX_ITEMS];
    yyjson_val *entries[TNY_JOBS_MAX_ITEMS] = {0};
    for (int i = 0; i < TNY_JOBS_MAX_ITEMS; i++) {
        memset(&slots[i], 0, sizeof slots[i]);
        slots[i].index = i;
        slots[i].in_fd = slots[i].out_fd = slots[i].log_fd = -1;
        slots[i].pid = -1;
    }
    size_t idx, max;
    yyjson_val *entry;
    yyjson_arr_foreach(items, idx, max, entry) {
        int index = (int)jget_int(entry, "index", -1);
        if (index < 0 || index >= TNY_JOBS_MAX_ITEMS) continue;
        entries[index] = entry;
        slots[index].prompt = xstrdup(jget_str(entry, "prompt"));
        slots[index].log_path = jobs_item_log(dir, index, (int)jget_int(payload, "attempt", 1));
        const char *output = jget_str(entry, "output_file");
        slots[index].output_path = output ? xstrdup(output) : NULL;
        slots[index].image = image;
    }
    (void)n_payload;

    bool finished = false;
    int rc = 0;
    while (!finished) {
        for (int i = 0; i < TNY_JOBS_MAX_ITEMS; i++) worker_scope_progress(&slots[i]);
        jobs_txn t;
        char err[192] = "";
        if (jobs_txn_begin(dir, id, &t, err, sizeof err) != 0) {
            tny_jobs_host_sleep_ms(JOBS_POLL_MS);
            continue;
        }
        yyjson_mut_doc *doc = t.doc;
        yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
        if (jm_int(root, "attempt", -1) != jget_int(payload, "attempt", -2)) {
            jobs_txn_end(&t);
            rc = EBUSY;
            break;
        }
        bool dirty = false;
        bool job_cancel = jm_bool(root, "cancel_requested", false);
        int count = jm_item_count(doc);
        if (strcmp(jm_str(root, "state") ? jm_str(root, "state") : "", "running") != 0) {
            jm_set_str(doc, root, "state", "running");
            dirty = true;
        }
        int active = 0;
        for (int i = 0; i < count; i++)
            if (slots[i].active) active++;

        /* 1. results of children that already exited */
        for (int i = 0; i < count; i++) {
            if (!slots[i].active) continue;
            if (slots[i].launch_failed) {
                yyjson_mut_val *item = jm_item(doc, i);
                jm_set_str(doc, item, "state", "failed");
                jm_set_str(doc, item, "error_code", TNY_JOBS_CODE_IO);
                jm_set_str(doc, item, "error", "cannot start the item process");
                slot_close(&slots[i]);
                slots[i].active = false;
                active--;
                dirty = true;
                continue;
            }
            if (slots[i].scope && !slots[i].cleanup_done) continue;
            if (!slots[i].reaped && !slots[i].reap_error) continue;
            if (!slots[i].eof && monotonic_ms() < slots[i].drain_deadline) continue;
            yyjson_mut_val *item = jm_item(doc, i);
            bool cancelled = job_cancel || jm_bool(item, "cancel_requested", false);
            if (!slots[i].eof || slots[i].reap_error) slots[i].cleanup_unknown = true;
            worker_record_result(ctx, doc, &slots[i], cancelled && slots[i].cancel_signalled);
            slot_close(&slots[i]);
            slots[i].active = false;
            active--;
            dirty = true;
        }

        /* 2. cancellation and launch decisions, linearized under this lock */
        for (int i = 0; i < count; i++) {
            yyjson_mut_val *item = jm_item(doc, i);
            const char *state = jm_str(item, "state");
            bool cancel = job_cancel || jm_bool(item, "cancel_requested", false);
            if (!state || state_is_terminal(state)) continue;
            if (!entries[i]) continue; /* carried item of an earlier attempt */
            if (cancel && !slots[i].active) {
                /* The cancellation won the race with the launch claim: this
                 * item never reaches a provider. */
                jm_set_str(doc, item, "state", "cancelled");
                jm_set_str(doc, item, "error", "cancelled before it started");
                jm_set_int(doc, item, "exit_code", 0);
                char *now = now_iso8601();
                jm_set_str(doc, item, "finished", now ? now : "");
                free(now);
                dirty = true;
                continue;
            }
            if (slots[i].active && slots[i].scope && slots[i].admission_ready &&
                !slots[i].released && !slots[i].killed && !slots[i].admission_failed &&
                !slots[i].reaped && !slots[i].reap_error && !cancel) {
                int go = tny_process_scope_go(slots[i].scope, slots[i].in_fd);
                if (go == 1) {
                    slots[i].released = true;
                    char *now = now_iso8601();
                    jm_set_str(doc, item, "started", now ? now : "");
                    free(now);
                    dirty = true;
                } else if (go < 0) slots[i].admission_failed = true;
            }
            if (slots[i].active || slots[i].launched) continue;
            if (active >= concurrency) continue;
            if (tny_process_scope_native_jobs()) {
                /* Persist the claim and count its slot before spawning outside
                 * this lock. The bootstrap cannot do work until a later GO. */
                slots[i].active = true;
                slots[i].launched = true;
                slots[i].launch_pending = true;
                jm_set_str(doc, item, "state", "running");
                active++;
                dirty = true;
                continue;
            }
            char spawn_err[160] = "";
            if (worker_spawn_item(payload, entries[i], image, &slots[i], spawn_err,
                                  sizeof spawn_err) == 0) {
                char *now = now_iso8601();
                jm_set_str(doc, item, "state", "running");
                jm_set_str(doc, item, "started", now ? now : "");
                free(now);
                active++;
            } else {
                jm_set_str(doc, item, "state", "failed");
                jm_set_str(doc, item, "error_code", TNY_JOBS_CODE_IO);
                jm_set_str(doc, item, "error", spawn_err);
                slots[i].launched = true;
            }
            dirty = true;
        }

        /* 3. are we done? */
        bool all_terminal = true;
        for (int i = 0; i < count; i++)
            if (!state_is_terminal(jm_str(jm_item(doc, i), "state"))) all_terminal = false;
        if (all_terminal) {
            bool failed = false, cancelled = false, interrupted = false;
            for (int i = 0; i < count; i++) {
                const char *state = jm_str(jm_item(doc, i), "state");
                if (!state) continue;
                if (strcmp(state, "failed") == 0) failed = true;
                if (strcmp(state, "cancelled") == 0) cancelled = true;
                if (strcmp(state, "interrupted") == 0) interrupted = true;
            }
            const char *state = failed        ? "failed"
                                : interrupted ? "interrupted"
                                : cancelled   ? "cancelled"
                                              : "succeeded";
            jm_set_str(doc, root, "state", state);
            jm_set_int(doc, root, "exit_code",
                       strcmp(state, "succeeded") == 0   ? 0
                       : strcmp(state, "cancelled") == 0 ? 130
                                                         : 2);
            /* Only the host seam's bounded, identity-preserving tree sweep
             * proves cancellation cleanup. Reap/drain failures remain unknown. */
            bool cleanup_unknown = false;
            for (int i = 0; i < count; i++)
                if (slots[i].cleanup_unknown) cleanup_unknown = true;
            jm_set_str(doc, root, "cleanup", cleanup_unknown ? "unknown" : "complete");
            jm_set_bool(doc, root, "cleanup_hold", cleanup_unknown);
            if (strcmp(state, "succeeded") != 0) {
                jm_set_str(doc, root, "error_code",
                           strcmp(state, "cancelled") == 0 ? NULL : TNY_JOBS_CODE_IO);
                jm_set_str(doc, root, "error",
                           strcmp(state, "cancelled") == 0 ? "cancelled on request"
                                                           : "one or more items did not succeed");
            }
            finished = true;
            dirty = true;
        }
        /* Collect the cancellation intents while the record is in hand; the
         * signals themselves go out after the lock is released. */
        bool signal_now[TNY_JOBS_MAX_ITEMS] = {false};
        for (int i = 0; i < count; i++) {
            yyjson_mut_val *item = jm_item(doc, i);
            signal_now[i] = slots[i].active &&
                            (slots[i].scope ? !slots[i].cleanup_done : !slots[i].reaped) &&
                            (job_cancel || jm_bool(item, "cancel_requested", false));
        }
        if (dirty) rc = jobs_txn_commit(&t);
        else jobs_txn_end(&t);
        if (rc) break;

        if (finished) break;
        for (int i = 0; i < count; i++) {
            if (!slots[i].launch_pending) continue;
            slots[i].launch_pending = false;
            char spawn_err[160] = "";
            if (worker_spawn_item(payload, entries[i], image, &slots[i], spawn_err,
                                  sizeof spawn_err) != 0)
                slots[i].launch_failed = true;
        }

        for (int i = 0; i < count; i++) {
            if (!signal_now[i]) continue;
            job_slot *s = &slots[i];
            if (!s->cancel_signalled) {
                if (s->scope) {
                    s->cancel_signalled = true;
                    s->killed = true;
                    worker_scope_progress(s);
                    continue;
                }
                /* Only an actual live child handle this supervisor created is
                 * ever signalled. */
                /* Freeze before a cooperative signal could let the parent
                 * exit/reparent descendants. Keep its unreaped identity until
                 * the complete leaves-first sweep finishes. */
                s->cleanup_unknown =
                    tny_process_stop_owned_tree(s->pid, &s->status, &s->reaped) != 0;
                s->cancel_signalled = true;
                s->cancel_deadline = monotonic_ms() + JOBS_CANCEL_GRACE_MS;
                s->drain_deadline = monotonic_ms() + 1000;
                s->killed = true;
            }
        }
        for (int i = 0; i < count; i++) {
            job_slot *s = &slots[i];
            if (s->active && !s->reaped && s->limit_hit && !s->killed) {
                if (s->scope) {
                    s->killed = true;
                    worker_scope_progress(s);
                    continue;
                }
                s->cleanup_unknown =
                    tny_process_stop_owned_tree(s->pid, &s->status, &s->reaped) != 0;
                s->cancel_deadline = monotonic_ms() + JOBS_CANCEL_GRACE_MS;
                s->drain_deadline = monotonic_ms() + 1000;
                s->killed = true;
            }
        }
        worker_pump(slots, TNY_JOBS_MAX_ITEMS);
        for (int i = 0; i < count; i++) {
            job_slot *s = &slots[i];
            if (s->scope) {
                worker_scope_progress(s);
                continue;
            }
            if (!s->active || s->reaped) continue;
            int status = 0;
            int reaped = tny_jobs_host_reap(s->pid, &status);
            if (reaped == 1) {
                s->reaped = true;
                s->status = status;
                /* A descendant may still hold the pipe: drain briefly, then
                 * stop waiting. The log is whatever actually arrived. */
                s->drain_deadline = monotonic_ms() + 1000;
            } else if (reaped < 0 || ((s->cancel_signalled || s->limit_hit) && s->cleanup_unknown &&
                                      monotonic_ms() >= s->cancel_deadline)) {
                s->reaped = true;
                s->reap_error = true;
                s->cleanup_unknown = true;
                s->status = 2 << 8;
                s->eof = true;
            }
        }
    }
    for (int i = 0; i < TNY_JOBS_MAX_ITEMS; i++) {
        if (slots[i].scope) {
            if (!slots[i].cleanup_done) {
                slots[i].killed = true;
                int64_t deadline = monotonic_ms() + 3500;
                do {
                    worker_scope_progress(&slots[i]);
                    if (!slots[i].cleanup_done) tny_jobs_host_sleep_ms(10);
                } while (!slots[i].cleanup_done && monotonic_ms() < deadline);
            }
            slot_free(&slots[i]);
            continue;
        }
        if (slots[i].active && !slots[i].reaped && slots[i].pid > 1)
            (void)tny_process_stop_owned_tree(slots[i].pid, &slots[i].status, &slots[i].reaped);
        if (!slots[i].reaped && slots[i].pid > 1) tny_jobs_host_reap(slots[i].pid, NULL);
        slot_free(&slots[i]);
    }
    /* Reservations are released at terminal completion, never while the state
     * lock is held, and only for records still naming this job/item/attempt. */
    yyjson_mut_doc *final = jobs_record_load(dir, id, NULL, 0);
    if (final) {
        yyjson_mut_val *final_root = yyjson_mut_doc_get_root(final);
        if (strcmp(jm_str(final_root, "cleanup") ? jm_str(final_root, "cleanup") : "",
                   "complete") == 0)
            reservations_release_job(ctx, final, id);
        yyjson_mut_doc_free(final);
    }
    return rc;
}

static char *worker_read_payload(int fd, char *err, size_t errlen) {
    buf_t payload;
    buf_init(&payload);
    for (;;) {
        char chunk[8192];
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            safe_err(err, errlen, "the request payload could not be read");
            buf_free(&payload);
            return NULL;
        }
        if (n == 0) break;
        if (payload.len + (size_t)n > TNY_JOBS_PAYLOAD_MAX) {
            safe_err(err, errlen, "the request payload is too large");
            buf_free(&payload);
            return NULL;
        }
        buf_append(&payload, chunk, (size_t)n);
    }
    if (buf_oom(&payload) || !payload.len) {
        safe_err(err, errlen, "the request payload was incomplete");
        buf_free(&payload);
        return NULL;
    }
    return buf_detach(&payload);
}

int tny_jobs_worker_main(tny_ctx *ctx, const char *id, int payload_fd, int ack_fd, int owner_fd) {
    if (!ctx || !tny_jobs_valid_id(id) || !tny_jobs_execution_supported()) return 1;
    char *dir = jobs_dir(ctx, id);
    char *owner_path = dir ? jobs_file(dir, "owner.lock") : NULL;
    if (!dir || !owner_path) {
        free(dir);
        free(owner_path);
        return 1;
    }
    /* Verify file identity AND acquire/confirm the supplied open description's
     * own exclusive lock. Another description holding this inode is not
     * authority. Close-on-exec prevents transfer to an item child. */
    bool owned = owner_fd >= 0 && tny_jobs_host_fd_is_file(owner_fd, owner_path) &&
                 tny_jobs_host_lock_try(owner_fd) == TNY_JOBS_LOCK_ACQUIRED &&
                 tny_jobs_host_set_cloexec(owner_fd) == 0;
    if (!owned) {
        free(dir);
        free(owner_path);
        return 1; /* no ownership: never touch this job's metadata */
    }
    /* Detach and ignore SIGPIPE before reading or acknowledging. Acceptance
     * belongs to the durable record, not to the life of the ack reader. */
    tny_jobs_host_detach_session();
    yyjson_mut_doc *accepted = jobs_record_load(dir, id, NULL, 0);
    int accepted_attempt =
        accepted ? (int)jm_int(yyjson_mut_doc_get_root(accepted), "attempt", -1) : -1;
    yyjson_mut_doc_free(accepted);
    char err[192] = "";
    char *payload_json = worker_read_payload(payload_fd, err, sizeof err);
    close(payload_fd);
    yyjson_doc *doc = payload_json ? jparse(payload_json, strlen(payload_json)) : NULL;
    yyjson_val *payload = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *job = jget_str(payload, "job");
    yyjson_val *items = jget(payload, "items");
    bool valid = payload && yyjson_is_obj(payload) &&
                 jget_int(payload, "version", 0) == TNY_JOBS_SCHEMA_VERSION && job &&
                 strcmp(job, id) == 0 && items && yyjson_is_arr(items) &&
                 yyjson_arr_size(items) >= 1 && yyjson_arr_size(items) <= TNY_JOBS_MAX_ITEMS &&
                 jget_str(payload, "self") && jget_str(payload, "cwd");
    bool stale_attempt =
        payload && jget_int(payload, "attempt", accepted_attempt) != accepted_attempt;
    if (valid) {
        /* The attempt in the payload must still be the record's attempt: an
         * old attempt's payload can never drive a new one. */
        yyjson_mut_doc *record = jobs_record_load(dir, id, NULL, 0);
        yyjson_mut_val *current = record ? yyjson_mut_doc_get_root(record) : NULL;
        const char *state = current ? jm_str(current, "state") : NULL;
        valid = record && state && strcmp(state, "queued") == 0 &&
                jm_int(current, "attempt", -1) == jget_int(payload, "attempt", -2);
        yyjson_mut_doc_free(record);
    }
    if (!valid) {
        /* No spend: fail the job honestly, having never contacted a provider. */
        if (!stale_attempt)
            submit_finish_failed(dir, accepted_attempt, TNY_JOBS_CODE_INVALID,
                                 err[0] ? err
                                        : "the supervisor did not receive a complete request");
        yyjson_doc_free(doc);
        if (payload_json) secure_free(payload_json);
        free(dir);
        free(owner_path);
        return 1;
    }
    if (tny_process_supervisor_init() != 0) {
        submit_finish_failed(dir, accepted_attempt, TNY_JOBS_CODE_IO,
                             "cannot establish native supervisor ownership");
        yyjson_doc_free(doc);
        secure_free(payload_json);
        free(dir);
        free(owner_path);
        return 1;
    }
    /* Acknowledge, then become the sole writer of this terminal: stdout goes
     * to /dev/null so nothing can ever race the submitter's stream. */
    char ack[64];
    int len = snprintf(ack, sizeof ack, "ok %s\n", id);
    ssize_t ignored = ack_fd >= 0 && len > 0 ? write(ack_fd, ack, (size_t)len) : 0;
    (void)ignored;
    if (ack_fd >= 0) close(ack_fd);
    int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        if (null_fd > STDERR_FILENO) close(null_fd);
    }
    tny_jobs_host_detach_session();
    int rc = worker_supervise(ctx, dir, id, payload);
    yyjson_doc_free(doc);
    secure_free(payload_json);
    free(dir);
    free(owner_path);
    /* The owner lock is released here, with the process: every reader now sees
     * a terminal record rather than a free lock over an active one. */
    tny_jobs_host_lock_close(owner_fd);
    return rc ? 2 : 0;
}

/* Strict private snapshot fields: JSON strings may not hide embedded NULs. */
static const char *artifact_string(yyjson_val *obj, const char *key, size_t max) {
    yyjson_val *v = jget(obj, key);
    const char *s = yyjson_get_str(v);
    size_t n = yyjson_get_len(v);
    return s && n && n < max && strlen(s) == n ? s : NULL;
}

static bool artifact_uint(yyjson_val *obj, const char *key, uint64_t max, uint64_t *out) {
    yyjson_val *v = jget(obj, key);
    if (!yyjson_is_uint(v) || yyjson_get_uint(v) > max) return false;
    *out = yyjson_get_uint(v);
    return true;
}

/* Stored artifact/manifest paths must already be canonical absolute identities.
 * Resolve before confinement, then require identity equality: aliases or '..'
 * cannot cause a referenced metadata read outside the permission roots. */
static char *artifact_owned_path(const tny_ctx *ctx, const char *path) {
    char reason[128];
    char *absolute = path ? tny_image_io_canonical(path, reason, sizeof reason) : NULL;
    if (!absolute || strcmp(absolute, path) != 0 || !perm_path_allowed(ctx, absolute)) {
        free(absolute);
        return NULL;
    }
    return absolute;
}

void tny_jobs_artifact_free(tny_job_artifact *a) {
    if (!a) return;
    free(a->path);
    tny_image_manifest_free(a->manifest);
    free(a);
}

tny_job_artifact *tny_jobs_select_artifact(const tny_ctx *ctx, const char *id, int index, char *err,
                                           size_t errlen) {
    if (!tny_jobs_execution_supported()) {
        safe_err(err, errlen,
                 "job image selection is unsupported in this runtime; use a stored manifest");
        return NULL;
    }
    if (!ctx || !ctx->tny_dir || !ctx->cwd || !tny_jobs_valid_id(id) || index < 0 ||
        index >= TNY_JOBS_MAX_ITEMS) {
        safe_err(err, errlen, "job selection needs a lowercase 32-hex id and item index 0-63");
        return NULL;
    }
    /* Job records are trusted only at this context's private jobs location.
     * Neither a job directory nor job.json may redirect that read elsewhere. */
    char *base = path_abs(ctx->tny_dir);
    char *root_path = base ? path_join(base, "jobs") : NULL;
    char *dir = root_path ? path_join(root_path, id) : NULL;
    char *path = dir ? path_join(dir, "job.json") : NULL;
    char *canonical = path ? path_abs(path) : NULL;
    buf_t raw;
    buf_init(&raw);
    bool located = canonical && strcmp(canonical, path) == 0;
    int read_rc = located ? tny_image_io_read_confined(base, path, TNY_JOBS_PAYLOAD_MAX, &raw) : -1;
    free(base);
    free(root_path);
    free(dir);
    free(path);
    free(canonical);
    if (read_rc != 0) {
        safe_err(err, errlen,
                 "job snapshot is missing, outside its owned location or not bounded JSON");
        buf_free(&raw);
        return NULL;
    }
    yyjson_doc *doc = jparse(raw.data, raw.len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *items = jget(root, "items");
    yyjson_val *item = yyjson_is_arr(items) ? yyjson_arr_get(items, (size_t)index) : NULL;
    uint64_t version = 0, projection = 0, producing = 0, carried = 0, stored_index = 0, bytes = 0;
    const char *kind = artifact_string(root, "kind", 16);
    const char *job_kind = artifact_string(root, "job_kind", 16);
    const char *record_id = artifact_string(root, "id", TNY_JOBS_ID_LEN + 1);
    const char *state = artifact_string(item, "state", 16);
    const char *output = artifact_string(item, "output_path", TNY_IMAGE_PATH_MAX);
    const char *hash = artifact_string(item, "output_sha256", TNY_IMAGE_SHA256_HEX);
    yyjson_val *manifest_value = jget(item, "manifest_path");
    const char *manifest = artifact_string(item, "manifest_path", TNY_IMAGE_MANIFEST_PATH_MAX);
    yyjson_val *operation_value = jget(item, "operation_id");
    const char *operation = artifact_string(item, "operation_id", TNY_IMAGE_OPERATION_ID_MAX);
    bool ok = yyjson_is_obj(root) && artifact_uint(root, "version", 1, &version) && version == 1 &&
              kind && strcmp(kind, "job") == 0 && job_kind && strcmp(job_kind, "image") == 0 &&
              record_id && strcmp(record_id, id) == 0 &&
              state_is_known(artifact_string(root, "state", 16)) && yyjson_is_arr(items) &&
              yyjson_arr_size(items) <= TNY_JOBS_MAX_ITEMS && yyjson_is_obj(item) &&
              artifact_uint(item, "index", TNY_JOBS_MAX_ITEMS - 1, &stored_index) &&
              stored_index == (uint64_t)index && state && strcmp(state, "succeeded") == 0 &&
              artifact_uint(root, "attempt", INT_MAX, &projection) && projection > 0 &&
              artifact_uint(item, "attempt", INT_MAX, &producing) && producing > 0 &&
              artifact_uint(item, "carried_from_attempt", INT_MAX, &carried) &&
              ((!carried && producing == projection) ||
               (carried == producing && producing < projection)) &&
              artifact_uint(item, "output_bytes", TNY_IMAGE_OUTPUT_MAX, &bytes) && bytes > 0 &&
              output && tny_image_preview_hash_valid(hash) && manifest_value &&
              (manifest || yyjson_is_null(manifest_value)) && operation_value &&
              (operation || yyjson_is_null(operation_value)) && (!manifest || operation);
    if (operation) {
        for (const char *p = operation; *p; p++)
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) ok = false;
    }
    tny_job_artifact *a = ok ? calloc(1, sizeof *a) : NULL;
    if (a) {
        a->path = artifact_owned_path(ctx, output);
        ok = a->path != NULL;
        snprintf(a->job_id, sizeof a->job_id, "%s", id);
        snprintf(a->sha256, sizeof a->sha256, "%s", hash);
        if (operation) snprintf(a->operation_id, sizeof a->operation_id, "%s", operation);
        a->item_index = index;
        a->projection_attempt = (int)projection;
        a->item_attempt = (int)producing;
        a->carried_from_attempt = (int)carried;
        a->bytes = bytes;
        if (ok && manifest) {
            char *owned_manifest = artifact_owned_path(ctx, manifest);
            buf_t metadata;
            buf_init(&metadata);
            const char *authority = NULL;
            if (owned_manifest && path_is_within(ctx->cwd, owned_manifest)) authority = ctx->cwd;
            for (int i = 0; owned_manifest && !authority && i < ctx->n_extra_dirs; i++)
                if (path_is_within(ctx->extra_dirs[i], owned_manifest))
                    authority = ctx->extra_dirs[i];
            if (authority && tny_image_io_read_confined(authority, owned_manifest,
                                                        TNY_IMAGE_MANIFEST_MAX, &metadata) == 0)
                a->manifest = tny_image_manifest_parse(owned_manifest, metadata.data, metadata.len,
                                                       err, errlen);
            buf_free(&metadata);
            free(owned_manifest);
            tny_image_manifest *m = a->manifest;
            char *resolved =
                m && m->artifact_path ? tny_image_manifest_resolve(m, m->artifact_path) : NULL;
            char *destination = m ? tny_image_manifest_resolve(m, m->output) : NULL;
            ok = m && !m->derived && m->committed && strcmp(m->status, "succeeded") == 0 &&
                 resolved && destination && strcmp(resolved, a->path) == 0 &&
                 strcmp(destination, a->path) == 0 && strcmp(m->artifact_sha256, a->sha256) == 0 &&
                 strcmp(m->operation_id, a->operation_id) == 0 && m->bytes == a->bytes;
            free(destination);
            free(resolved);
        }
    }
    yyjson_doc_free(doc);
    buf_free(&raw);
    if (!a || !ok) {
        tny_jobs_artifact_free(a);
        safe_err(err, errlen,
                 "job item is not a valid succeeded image identity within allowed roots; check the "
                 "item, attempt and declared manifest");
        return NULL;
    }
    return a;
}
