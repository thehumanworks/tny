#include "core/admission.h"
#include "util/admission_host.h"
#include "util/jobs_host.h"
#include "util/util.h"
#include "yyjson.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Append-only identity history makes retries and canceled tickets idempotent. */
#define WAITING  0U
#define OWNED    1U
#define HOLD     2U
#define RELEASED 3U
#define CANCELED 4U
typedef struct {
    char run[64];
    uint32_t task, attempt, state;
} entry;
typedef struct {
    entry entries[TNY_ADMISSION_HISTORY_MAX];
    size_t count;
    uint64_t claims;
    uint32_t active, queued;
} ledger;

static bool public_id(const char *s) {
    if (!s || !*s || strlen(s) > 63) return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
              *s == '-' || *s == '_'))
            return false;
    return true;
}
static bool number(yyjson_val *obj, const char *key, uint64_t *out) {
    yyjson_val *v = yyjson_obj_get(obj, key);
    if (!yyjson_is_uint(v)) return false;
    *out = yyjson_get_uint(v);
    return true;
}
static bool same(const entry *e, const tny_admission_attempt *a) {
    return e->task == a->task && e->attempt == a->attempt && !strcmp(e->run, a->run);
}
static int load(const char *path, const tny_admission_scope *s, ledger *l) {
    char *bytes = NULL;
    size_t len = 0;
    int rc = tny_admission_host_read(path, &bytes, &len);
    if (rc) return rc;
    yyjson_doc *doc = yyjson_read(bytes, len, 0);
    free(bytes);
    if (!doc) return EIO;
    yyjson_val *root = yyjson_doc_get_root(doc);
    uint64_t version, cap, queue, limit, claims;
    rc = EIO;
    if (!yyjson_is_obj(root) || yyjson_obj_size(root) != 6 || !number(root, "version", &version) ||
        version != 1 || !number(root, "cap", &cap) || !number(root, "queue_cap", &queue) ||
        !number(root, "claim_limit", &limit) || !number(root, "claims", &claims))
        goto done;
    if (cap != s->cap || queue != s->queue_cap || limit != s->claim_limit) {
        rc = EINVAL;
        goto done;
    }
    yyjson_val *list = yyjson_obj_get(root, "entries");
    if (!yyjson_is_arr(list) || yyjson_arr_size(list) > TNY_ADMISSION_HISTORY_MAX) goto done;
    size_t i, max;
    yyjson_val *v;
    yyjson_arr_foreach(list, i, max, v) {
        uint64_t task, attempt, state;
        yyjson_val *run = yyjson_obj_get(v, "run");
        const char *id = yyjson_get_str(run);
        if (!yyjson_is_obj(v) || yyjson_obj_size(v) != 4 || !public_id(id) ||
            yyjson_get_len(run) != strlen(id) || !number(v, "task", &task) || task > UINT32_MAX ||
            !number(v, "attempt", &attempt) || !attempt || attempt > UINT32_MAX ||
            !number(v, "state", &state) || state > CANCELED)
            goto done;
        entry *e = &l->entries[l->count];
        memcpy(e->run, id, strlen(id) + 1);
        e->task = (uint32_t)task;
        e->attempt = (uint32_t)attempt;
        e->state = (uint32_t)state;
        tny_admission_attempt a = {e->run, e->task, e->attempt};
        for (size_t j = 0; j < l->count; j++)
            if (same(&l->entries[j], &a)) goto done;
        l->count++;
        if (state == WAITING) l->queued++;
        if (state == OWNED || state == HOLD) l->active++;
        if (state == OWNED || state == HOLD || state == RELEASED) l->claims++;
    }
    if (l->claims != claims || claims > limit || l->active > cap || l->queued > queue) goto done;
    rc = 0;
done:
    yyjson_doc_free(doc);
    return rc;
}
static int save(const char *dir, const char *path, const tny_admission_scope *s, const ledger *l) {
    buf_t b;
    buf_init(&b);
    buf_appendf(&b,
                "{\"version\":1,\"cap\":%u,\"queue_cap\":%u,\"claim_limit\":%llu,"
                "\"claims\":%llu,\"entries\":[",
                s->cap, s->queue_cap, (unsigned long long)s->claim_limit,
                (unsigned long long)l->claims);
    for (size_t i = 0; i < l->count; i++) {
        const entry *e = &l->entries[i];
        buf_appendf(&b, "%s{\"run\":\"%s\",\"task\":%u,\"attempt\":%u,\"state\":%u}", i ? "," : "",
                    e->run, e->task, e->attempt, e->state);
    }
    buf_appends(&b, "]}\n");
    int rc = buf_oom(&b) ? ENOMEM : tny_admission_host_write(dir, path, b.data, b.len);
    buf_free(&b);
    return rc;
}
static tny_admission_reason status(const ledger *l, size_t idx) {
    const entry *e = &l->entries[idx];
    if (e->state == OWNED) return TNY_ADMISSION_OWNED;
    if (e->state == HOLD) return TNY_ADMISSION_CLEANUP_HOLD;
    if (e->state == RELEASED) return TNY_ADMISSION_RELEASED;
    if (e->state == CANCELED) return TNY_ADMISSION_CANCELED;
    if (l->active) return TNY_ADMISSION_QUEUED_CAPACITY;
    return TNY_ADMISSION_QUEUED_FIFO;
}
static void counts(const ledger *l, tny_admission_result *out) {
    out->claims = l->claims;
    out->active = l->active;
    out->queued = l->queued;
}
static int transact(const tny_admission_scope *s, const tny_admission_attempt *a,
                    tny_admission_op op, bool proof, ledger *l, tny_admission_result *out,
                    bool *dirty) {
    size_t idx = 0;
    for (; idx < l->count; idx++)
        if (same(&l->entries[idx], a)) break;
    if (idx == l->count) {
        if (op != TNY_ADMISSION_CLAIM && op != TNY_ADMISSION_CANCEL) {
            out->reason = TNY_ADMISSION_NOT_FOUND;
            return 0;
        }
        if (op == TNY_ADMISSION_CLAIM && l->claims >= s->claim_limit) {
            out->reason = TNY_ADMISSION_EXHAUSTED;
            return 0;
        }
        if (l->count == TNY_ADMISSION_HISTORY_MAX) {
            out->reason = TNY_ADMISSION_HISTORY_FULL;
            return 0;
        }
        if (op == TNY_ADMISSION_CLAIM && l->queued == s->queue_cap) {
            out->reason = TNY_ADMISSION_QUEUE_FULL;
            return 0;
        }
        entry *e = &l->entries[l->count++];
        memcpy(e->run, a->run, strlen(a->run) + 1);
        e->task = a->task;
        e->attempt = a->attempt;
        e->state = op == TNY_ADMISSION_CANCEL ? CANCELED : WAITING;
        if (e->state == WAITING) l->queued++;
        *dirty = true;
    }
    entry *e = &l->entries[idx];
    out->ticket = idx + 1;
    if (op == TNY_ADMISSION_CANCEL && e->state == WAITING) {
        e->state = CANCELED;
        l->queued--;
        *dirty = true;
    } else if ((op == TNY_ADMISSION_CANCEL || op == TNY_ADMISSION_HOLD) && e->state == OWNED) {
        e->state = HOLD; /* cancellation after grant must not free capacity */
        *dirty = true;
    } else if (op == TNY_ADMISSION_RELEASE && (e->state == OWNED || e->state == HOLD)) {
        if (!proof) return EPERM;
        e->state = RELEASED;
        l->active--;
        *dirty = true;
    }
    out->reason = status(l, idx);
    if (e->state != WAITING) return 0;
    if (l->claims == s->claim_limit) out->reason = TNY_ADMISSION_EXHAUSTED;
    else if (l->active >= s->cap) out->reason = TNY_ADMISSION_QUEUED_CAPACITY;
    else {
        out->reason = TNY_ADMISSION_QUEUED_FIFO;
        size_t first = 0;
        while (first < idx && l->entries[first].state != WAITING) first++;
        if (op == TNY_ADMISSION_CLAIM && first == idx) {
            e->state = OWNED;
            l->queued--;
            l->active++;
            l->claims++;
            out->reason = TNY_ADMISSION_GRANTED;
            *dirty = true;
        }
    }
    return 0;
}
int tny_admission_apply(const tny_admission_scope *s, const tny_admission_attempt *a,
                        tny_admission_op op, bool nested, bool proof, tny_admission_result *out) {
    if (!s || !out || !s->root || !*s->root || !public_id(s->label) ||
        !public_id(s->provider_scope) || !s->cap || s->cap > TNY_ADMISSION_QUEUE_MAX ||
        !s->queue_cap || s->queue_cap > TNY_ADMISSION_QUEUE_MAX || !s->claim_limit ||
        op < TNY_ADMISSION_INIT || op > TNY_ADMISSION_RELEASE ||
        (op != TNY_ADMISSION_INIT && (!a || !public_id(a->run) || !a->attempt)))
        return EINVAL;
    if (!tny_jobs_host_execution_supported()) return ENOTSUP;
    if (op == TNY_ADMISSION_CLAIM && nested) return EDEADLK;
    *out = (tny_admission_result){.reason = TNY_ADMISSION_READY};
    char *group = path_join(s->root, s->label);
    char *dir = group ? path_join(group, s->provider_scope) : NULL;
    char *path = dir ? path_join(dir, "state.json") : NULL;
    char *lock = dir ? path_join(dir, "state.lock") : NULL;
    ledger *l = calloc(1, sizeof(*l));
    int fd = -1;
    int rc = !group || !dir || !path || !lock || !l ? ENOMEM : 0;
    if (!rc && op == TNY_ADMISSION_INIT) rc = tny_admission_host_prepare(s->root, group, dir);
    if (!rc) {
        fd = tny_jobs_host_lock_open(lock);
        if (fd < 0) rc = errno;
    }
    if (!rc) {
        tny_jobs_lock_rc locked = tny_jobs_host_lock_try(fd);
        if (locked == TNY_JOBS_LOCK_BUSY) {
            out->reason = TNY_ADMISSION_BUSY;
            goto done;
        }
        if (locked != TNY_JOBS_LOCK_ACQUIRED) rc = errno ? errno : EIO;
    }
    if (!rc) {
        rc = load(path, s, l);
        bool dirty = false;
        if (rc == ENOENT && op == TNY_ADMISSION_INIT) {
            rc = 0;
            dirty = true;
        }
        if (!rc && op != TNY_ADMISSION_INIT) rc = transact(s, a, op, proof, l, out, &dirty);
        if (!rc && dirty) rc = save(dir, path, s, l);
        if (!rc) counts(l, out);
    }
done:
    if (fd >= 0) tny_jobs_host_lock_close(fd);
    free(l);
    free(lock);
    free(path);
    free(dir);
    free(group);
    return rc;
}
const char *tny_admission_reason_name(tny_admission_reason reason) {
    static const char *const names[] = {
        "ready",      "granted",   "owned",        "queued_capacity", "queued_fifo",
        "queue_full", "exhausted", "history_full", "canceled",        "cleanup_hold",
        "released",   "not_found", "busy"};
    if ((unsigned)reason >= sizeof(names) / sizeof(names[0])) return "unknown";
    return names[reason];
}
