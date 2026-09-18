#include "core/team_mailbox.h"
#include "json/json.h"
#include "util/image_io.h"
#include "util/jobs_host.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Worst-case JSON escaping expands each payload byte to six bytes. */
#define MAILBOX_RECORD_MAX (TNY_MAILBOX_HISTORY_MAX * (TNY_MAILBOX_PAYLOAD_MAX * 6u + 1024u))
#define MAILBOX_JOB_MAX    (4u * 1024u * 1024u)
#define MAILBOX_TASK_MAX   64

typedef struct {
    int lock;
    char *path;
    yyjson_doc *job;
    tny_mailbox_message *messages;
    size_t count;
    bool peers;
} mailbox_txn;

static bool valid_run(const char run[33]) {
    for (size_t i = 0; i < 32; i++)
        if (!((run[i] >= '0' && run[i] <= '9') || (run[i] >= 'a' && run[i] <= 'f'))) return false;
    return run[32] == 0;
}
static bool valid_id(const char *id) {
    if (!id || !*id) return false;
    size_t n = 0;
    for (; n <= 64 && id[n]; n++) {
        char c = id[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-'))
            return false;
    }
    return n <= 64;
}
static bool valid_member(int task, uint32_t attempt) {
    return task == TNY_MAILBOX_LEAD ? attempt == 0
                                    : task >= 0 && task < MAILBOX_TASK_MAX && attempt > 0;
}
static bool text_is(yyjson_val *v, const char *s) {
    return yyjson_is_str(v) && yyjson_get_len(v) == strlen(s) &&
           memcmp(yyjson_get_str(v), s, strlen(s)) == 0;
}
static bool uint_field(yyjson_val *v, const char *key, uint32_t *out) {
    yyjson_val *n = jget(v, key);
    if (!yyjson_is_uint(n) || yyjson_get_uint(n) > UINT32_MAX) return false;
    *out = (uint32_t)yyjson_get_uint(n);
    return true;
}
static bool task_field(yyjson_val *v, const char *key, int *out) {
    yyjson_val *n = jget(v, key);
    if (!yyjson_is_int(n)) return false;
    int64_t value = yyjson_get_sint(n);
    if (value < TNY_MAILBOX_LEAD || value >= MAILBOX_TASK_MAX) return false;
    *out = (int)value;
    return true;
}
static bool active(yyjson_val *v) {
    return text_is(jget(v, "state"), "queued") || text_is(jget(v, "state"), "running");
}
static bool known_state(yyjson_val *v) {
    return active(v) || text_is(jget(v, "state"), "succeeded") ||
           text_is(jget(v, "state"), "failed") || text_is(jget(v, "state"), "cancelled") ||
           text_is(jget(v, "state"), "interrupted");
}
static tny_mailbox_rc member_check(yyjson_val *root, int task, uint32_t attempt,
                                   bool require_active) {
    if (task == TNY_MAILBOX_LEAD) return attempt == 0 ? TNY_MAILBOX_OK : TNY_MAILBOX_STALE;
    yyjson_val *item = task >= 0 ? yyjson_arr_get(jget(root, "items"), (size_t)task) : NULL;
    if (!item) return TNY_MAILBOX_DENIED;
    uint32_t current = 0;
    if (!uint_field(item, "attempt", &current) || current != attempt) return TNY_MAILBOX_STALE;
    if (require_active && (!active(item) || jget_bool(item, "cancel_requested", false)))
        return TNY_MAILBOX_TERMINAL;
    return TNY_MAILBOX_OK;
}
static void txn_end(mailbox_txn *t) {
    free(t->messages);
    free(t->path);
    yyjson_doc_free(t->job);
    if (t->lock >= 0) tny_jobs_host_lock_close(t->lock);
}
static bool addressed(const tny_mailbox_message *m, const tny_mailbox_identity *caller) {
    return m->sender.job_attempt == caller->job_attempt && m->recipient.task == caller->task &&
           m->recipient.task_attempt == caller->task_attempt;
}
static bool load_message(yyjson_val *v, const char *run, size_t index, tny_mailbox_message *m) {
    const char *id = jget_str(v, "id");
    yyjson_val *payload = jget(v, "payload");
    uint32_t state = 0;
    if (!yyjson_is_obj(v) || !valid_id(id) || strlen(id) != yyjson_get_len(jget(v, "id")) ||
        !yyjson_is_uint(jget(v, "sequence")) || yyjson_get_uint(jget(v, "sequence")) != index + 1 ||
        !uint_field(v, "job_attempt", &m->sender.job_attempt) || !m->sender.job_attempt ||
        !task_field(v, "sender_task", &m->sender.task) ||
        !uint_field(v, "sender_attempt", &m->sender.task_attempt) ||
        !valid_member(m->sender.task, m->sender.task_attempt) ||
        !task_field(v, "recipient_task", &m->recipient.task) ||
        !uint_field(v, "recipient_attempt", &m->recipient.task_attempt) ||
        !valid_member(m->recipient.task, m->recipient.task_attempt) ||
        !uint_field(v, "state", &state) || state > TNY_MAILBOX_ACKED || !yyjson_is_str(payload) ||
        yyjson_get_len(payload) > TNY_MAILBOX_PAYLOAD_MAX ||
        memchr(yyjson_get_str(payload), 0, yyjson_get_len(payload)))
        return false;
    memcpy(m->sender.run, run, 33);
    memcpy(m->id, id, strlen(id) + 1);
    m->sequence = index + 1;
    m->state = (tny_mailbox_state)state;
    m->payload_len = yyjson_get_len(payload);
    memcpy(m->payload, yyjson_get_str(payload), m->payload_len);
    m->payload[m->payload_len] = 0;
    return true;
}
static tny_mailbox_rc load_mailbox(const tny_mailbox_service *s, const char *run, mailbox_txn *t) {
    buf_t data;
    buf_init(&data);
    errno = 0;
    int rc = tny_image_io_read_confined(s->job_dir, t->path, MAILBOX_RECORD_MAX, &data);
    int error = errno;
    if (rc) {
        buf_free(&data);
        return error == ENOENT ? TNY_MAILBOX_OK : TNY_MAILBOX_IO;
    }
    yyjson_doc *doc = jparse(data.data, data.len);
    buf_free(&data);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *messages = jget(root, "messages");
    bool ok = text_is(jget(root, "run"), run) && jget_int(root, "version", 0) == 1 &&
              yyjson_is_arr(messages) && yyjson_arr_size(messages) <= TNY_MAILBOX_HISTORY_MAX;
    if (ok) {
        t->count = yyjson_arr_size(messages);
        for (size_t i = 0; i < t->count && ok; i++) {
            tny_mailbox_message *m = &t->messages[i];
            yyjson_val *job = yyjson_doc_get_root(t->job);
            size_t members = yyjson_arr_size(jget(job, "items"));
            ok = load_message(yyjson_arr_get(messages, i), run, i, m) &&
                 m->sender.job_attempt <= (uint64_t)jget_int(job, "attempt", 0) &&
                 m->sender.task_attempt <= m->sender.job_attempt &&
                 m->recipient.task_attempt <= m->sender.job_attempt &&
                 (m->sender.task == TNY_MAILBOX_LEAD || (size_t)m->sender.task < members) &&
                 (m->recipient.task == TNY_MAILBOX_LEAD || (size_t)m->recipient.task < members);
            for (size_t k = 0; k < i && ok; k++)
                if (strcmp(t->messages[k].id, t->messages[i].id) == 0) ok = false;
        }
    }
    yyjson_doc_free(doc);
    return ok ? TNY_MAILBOX_OK : TNY_MAILBOX_CORRUPT;
}
static tny_mailbox_rc txn_begin(const tny_mailbox_service *s, const tny_mailbox_identity *caller,
                                mailbox_txn *t) {
    if (!s || !caller || !valid_run(caller->run) || !caller->job_attempt ||
        !valid_member(caller->task, caller->task_attempt) || !s->job_dir || s->job_dir[0] != '/')
        return TNY_MAILBOX_INVALID;
    if (!s->native_local || !tny_jobs_host_execution_supported()) return TNY_MAILBOX_UNSUPPORTED;
    if (!s->authorize) return TNY_MAILBOX_DENIED;
    /* Verify the trusted directory's whole prefix before lock creation. */
    char *job_path = path_join(s->job_dir, "job.json");
    buf_t data;
    buf_init(&data);
    int read_rc =
        job_path ? tny_image_io_read_confined(s->job_dir, job_path, MAILBOX_JOB_MAX, &data) : -1;
    buf_free(&data);
    if (read_rc) {
        free(job_path);
        return TNY_MAILBOX_IO;
    }
    char *lock_path = path_join(s->job_dir, "state.lock");
    t->lock = lock_path ? tny_jobs_host_lock_open(lock_path) : -1;
    free(lock_path);
    if (t->lock < 0) {
        free(job_path);
        return TNY_MAILBOX_IO;
    }
    tny_jobs_lock_rc lock_rc = tny_jobs_host_lock_try(t->lock);
    if (lock_rc != TNY_JOBS_LOCK_ACQUIRED) {
        free(job_path);
        return lock_rc == TNY_JOBS_LOCK_BUSY ? TNY_MAILBOX_BUSY : TNY_MAILBOX_IO;
    }
    read_rc = tny_image_io_read_confined(s->job_dir, job_path, MAILBOX_JOB_MAX, &data);
    free(job_path);
    if (read_rc) return TNY_MAILBOX_IO;
    t->job = jparse(data.data, data.len);
    yyjson_val *root = t->job ? yyjson_doc_get_root(t->job) : NULL;
    yyjson_val *items = jget(root, "items");
    uint32_t attempt = 0;
    bool valid = yyjson_is_obj(root) && jget_int(root, "version", 0) == 1 &&
                 text_is(jget(root, "kind"), "job") && text_is(jget(root, "id"), caller->run) &&
                 uint_field(root, "attempt", &attempt) && attempt > 0 && known_state(root) &&
                 yyjson_is_bool(jget(root, "cancel_requested")) && yyjson_is_arr(items) &&
                 yyjson_arr_size(items) > 0 && yyjson_arr_size(items) <= MAILBOX_TASK_MAX;
    for (size_t i = 0; valid && i < yyjson_arr_size(items); i++) {
        yyjson_val *item = yyjson_arr_get(items, i);
        uint32_t item_attempt = 0;
        valid = yyjson_is_obj(item) && jget_int(item, "index", -1) == (int64_t)i &&
                uint_field(item, "attempt", &item_attempt) && item_attempt > 0 &&
                item_attempt <= attempt && known_state(item) &&
                yyjson_is_bool(jget(item, "cancel_requested"));
    }
    tny_mailbox_rc rc = valid ? TNY_MAILBOX_OK : TNY_MAILBOX_CORRUPT;
    if (rc == TNY_MAILBOX_OK && !s->authorize(s->userdata, caller, data.data, data.len, &t->peers))
        rc = TNY_MAILBOX_DENIED;
    buf_free(&data);
    if (rc != TNY_MAILBOX_OK) return rc;
    if (attempt != caller->job_attempt) return TNY_MAILBOX_STALE;
    rc = member_check(root, caller->task, caller->task_attempt, false);
    if (rc != TNY_MAILBOX_OK) return rc;
    t->path = path_join(s->job_dir, "mailbox.json");
    t->messages = calloc(TNY_MAILBOX_HISTORY_MAX, sizeof(*t->messages));
    if (!t->path || !t->messages) return TNY_MAILBOX_IO;
    return load_mailbox(s, caller->run, t);
}
static tny_mailbox_rc store(mailbox_txn *t, const char *run) {
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "{\"version\":1,\"run\":\"%s\",\"messages\":[", run);
    for (size_t i = 0; i < t->count; i++) {
        const tny_mailbox_message *m = &t->messages[i];
        buf_appendf(&b,
                    "%s{\"id\":\"%s\",\"sequence\":%llu,\"job_attempt\":%u,"
                    "\"sender_task\":%d,\"sender_attempt\":%u,\"recipient_task\":%d,"
                    "\"recipient_attempt\":%u,\"state\":%d,\"payload\":",
                    i ? "," : "", m->id, (unsigned long long)m->sequence, m->sender.job_attempt,
                    m->sender.task, m->sender.task_attempt, m->recipient.task,
                    m->recipient.task_attempt, (int)m->state);
        jescape(&b, m->payload);
        buf_append(&b, "}", 1);
    }
    buf_append(&b, "]}", 2);
    int rc = b.oom ? ENOMEM : tny_jobs_host_write_private(t->path, b.data, b.len);
    buf_free(&b);
    return rc ? TNY_MAILBOX_IO : TNY_MAILBOX_OK;
}
static tny_mailbox_message *find(mailbox_txn *t, const char *id) {
    for (size_t i = 0; i < t->count; i++)
        if (strcmp(t->messages[i].id, id) == 0) return &t->messages[i];
    return NULL;
}
static bool valid_payload(const char *payload, size_t len) {
    if (!payload || len > TNY_MAILBOX_PAYLOAD_MAX || memchr(payload, 0, len)) return false;
    char *copy = xstrndup(payload, len);
    if (!copy) return false;
    buf_t b;
    buf_init(&b);
    jescape(&b, copy);
    free(copy);
    yyjson_doc *doc = b.oom ? NULL : jparse(b.data, b.len);
    bool ok = doc != NULL;
    yyjson_doc_free(doc);
    buf_free(&b);
    return ok;
}
tny_mailbox_rc tny_team_mailbox_send(const tny_mailbox_service *s,
                                     const tny_mailbox_identity *caller,
                                     tny_mailbox_recipient recipient, const char *id,
                                     const char *payload, size_t payload_len,
                                     tny_mailbox_message *out) {
    if (!out || !valid_id(id) || !valid_member(recipient.task, recipient.task_attempt) ||
        !valid_payload(payload, payload_len))
        return TNY_MAILBOX_INVALID;
    mailbox_txn t = {.lock = -1};
    tny_mailbox_rc rc = txn_begin(s, caller, &t);
    if (rc != TNY_MAILBOX_OK) goto done;
    yyjson_val *root = yyjson_doc_get_root(t.job);
    /* Fences and membership still apply to duplicate receipts. Terminal state
     * does not invalidate a receipt for a send already durably accepted. */
    rc = member_check(root, recipient.task, recipient.task_attempt, false);
    if (rc != TNY_MAILBOX_OK) goto done;
    if (caller->task != TNY_MAILBOX_LEAD && recipient.task != TNY_MAILBOX_LEAD && !t.peers) {
        rc = TNY_MAILBOX_DENIED;
        goto done;
    }
    tny_mailbox_message *m = find(&t, id);
    if (m) {
        if (m->sender.job_attempt != caller->job_attempt || m->sender.task != caller->task ||
            m->sender.task_attempt != caller->task_attempt || m->recipient.task != recipient.task ||
            m->recipient.task_attempt != recipient.task_attempt || m->payload_len != payload_len ||
            memcmp(m->payload, payload, payload_len) != 0)
            rc = TNY_MAILBOX_CONFLICT;
        else *out = *m;
        goto done;
    }
    if (!active(root) || jget_bool(root, "cancel_requested", false)) {
        rc = TNY_MAILBOX_TERMINAL;
        goto done;
    }
    rc = member_check(root, caller->task, caller->task_attempt, true);
    if (rc == TNY_MAILBOX_OK) rc = member_check(root, recipient.task, recipient.task_attempt, true);
    if (rc != TNY_MAILBOX_OK) goto done;
    size_t outstanding = 0;
    for (size_t i = 0; i < t.count; i++)
        if (t.messages[i].recipient.task == recipient.task &&
            t.messages[i].state != TNY_MAILBOX_ACKED)
            outstanding++;
    if (outstanding >= TNY_MAILBOX_OUTSTANDING_MAX) {
        rc = TNY_MAILBOX_FULL;
        goto done;
    }
    if (t.count == TNY_MAILBOX_HISTORY_MAX) {
        rc = TNY_MAILBOX_HISTORY_FULL;
        goto done;
    }
    m = &t.messages[t.count++];
    memcpy(m->id, id, strlen(id) + 1);
    m->sender = *caller;
    m->recipient = recipient;
    m->sequence = t.count;
    m->state = TNY_MAILBOX_QUEUED;
    m->payload_len = payload_len;
    memcpy(m->payload, payload, payload_len);
    m->payload[payload_len] = 0;
    rc = store(&t, caller->run);
    if (rc == TNY_MAILBOX_OK) *out = *m;
done:
    txn_end(&t);
    return rc;
}
tny_mailbox_rc tny_team_mailbox_inbox(const tny_mailbox_service *s,
                                      const tny_mailbox_identity *caller, uint64_t after_sequence,
                                      tny_mailbox_message *out, size_t capacity, size_t byte_limit,
                                      size_t *count) {
    if (count) *count = 0;
    if (!out || !count || !capacity || capacity > TNY_MAILBOX_BATCH_MAX || !byte_limit ||
        byte_limit > TNY_MAILBOX_BATCH_BYTES_MAX)
        return TNY_MAILBOX_INVALID;
    mailbox_txn t = {.lock = -1};
    tny_mailbox_rc rc = txn_begin(s, caller, &t);
    if (rc == TNY_MAILBOX_OK) {
        size_t bytes = 0;
        for (size_t i = 0; i < t.count && *count < capacity; i++) {
            tny_mailbox_message *m = &t.messages[i];
            if (!addressed(m, caller) || m->state == TNY_MAILBOX_ACKED ||
                m->sequence <= after_sequence)
                continue;
            if (m->payload_len > byte_limit - bytes) {
                if (!*count) rc = TNY_MAILBOX_FULL;
                break;
            }
            out[(*count)++] = *m;
            bytes += m->payload_len;
        }
    }
    txn_end(&t);
    return rc;
}
static tny_mailbox_rc access_message(const tny_mailbox_service *s,
                                     const tny_mailbox_identity *caller, const char *id,
                                     int transition, tny_mailbox_message *out) {
    if (!valid_id(id)) return TNY_MAILBOX_INVALID;
    mailbox_txn t = {.lock = -1};
    tny_mailbox_rc rc = txn_begin(s, caller, &t);
    if (rc == TNY_MAILBOX_OK) {
        tny_mailbox_message *m = find(&t, id);
        if (!m) rc = TNY_MAILBOX_NOT_FOUND;
        else if (!addressed(m, caller)) rc = TNY_MAILBOX_DENIED;
        else if (transition == TNY_MAILBOX_ACKED && m->state == TNY_MAILBOX_QUEUED)
            rc = TNY_MAILBOX_BAD_STATE;
        else {
            if (transition > (int)m->state) {
                m->state = (tny_mailbox_state)transition;
                rc = store(&t, caller->run);
            }
            if (rc == TNY_MAILBOX_OK && out) *out = *m;
        }
    }
    txn_end(&t);
    return rc;
}
tny_mailbox_rc tny_team_mailbox_read(const tny_mailbox_service *s,
                                     const tny_mailbox_identity *caller, const char *id,
                                     tny_mailbox_message *out) {
    if (!out) return TNY_MAILBOX_INVALID;
    return access_message(s, caller, id, -1, out);
}
tny_mailbox_rc tny_team_mailbox_mark_delivered(const tny_mailbox_service *s,
                                               const tny_mailbox_identity *caller, const char *id) {
    return access_message(s, caller, id, TNY_MAILBOX_DELIVERED, NULL);
}
tny_mailbox_rc tny_team_mailbox_ack(const tny_mailbox_service *s,
                                    const tny_mailbox_identity *caller, const char *id) {
    return access_message(s, caller, id, TNY_MAILBOX_ACKED, NULL);
}
const char *tny_team_mailbox_error(tny_mailbox_rc rc) {
    static const char *const names[] = {
        "MAILBOX_OK",           "MAILBOX_INVALID",  "MAILBOX_UNSUPPORTED", "MAILBOX_DENIED",
        "MAILBOX_STALE",        "MAILBOX_TERMINAL", "MAILBOX_BUSY",        "MAILBOX_FULL",
        "MAILBOX_HISTORY_FULL", "MAILBOX_CONFLICT", "MAILBOX_NOT_FOUND",   "MAILBOX_BAD_STATE",
        "MAILBOX_CORRUPT",      "MAILBOX_IO"};
    return (unsigned)rc < sizeof(names) / sizeof(names[0]) ? names[rc] : "MAILBOX_UNKNOWN";
}
