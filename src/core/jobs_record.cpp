// Durable record validation and bounded state-lock transactions.
#include "core/jobs_record.hpp"
extern "C" {
#include "core/jobs.h"
#include "util/jobs_host.h"
#include "util/util.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
}

namespace tny::jobs_detail {
static constexpr int JOBS_STATE_LOCK_TRIES = 80;
static constexpr int JOBS_STATE_LOCK_WAIT_MS = 25;
static const char *const JOB_STATES[] = {"queued", "running",   "succeeded",
                                         "failed", "cancelled", "interrupted"};
static void record_error(char *err, size_t errlen, const char *message) {
    if (err && errlen) snprintf(err, errlen, "%s", message);
}

bool state_is_known(const char *state) {
    if (!state) return false;
    for (size_t i = 0; i < sizeof JOB_STATES / sizeof JOB_STATES[0]; i++)
        if (strcmp(state, JOB_STATES[i]) == 0) return true;
    return false;
}

/* ---- loading and validating a record (fails closed) ---- */

bool record_state_is_known(yyjson_val *state) {
    const char *text = yyjson_get_str(state);
    return text && strlen(text) == yyjson_get_len(state) && state_is_known(text);
}

bool json_unique_keys(yyjson_val *value, unsigned depth) {
    if (!value || depth > 64) return false;
    size_t i, max;
    yyjson_val *key, *child;
    if (yyjson_is_obj(value)) {
        yyjson_obj_foreach(value, i, max, key, child) {
            const char *name = yyjson_get_str(key);
            size_t len = yyjson_get_len(key);
            if (!name || strlen(name) != len || yyjson_obj_getn(value, name, len) != child ||
                !json_unique_keys(child, depth + 1))
                return false;
        }
    } else if (yyjson_is_arr(value)) {
        yyjson_arr_foreach(value, i, max,
                           child) if (!json_unique_keys(child, depth + 1)) return false;
    } else if (yyjson_is_str(value) && strlen(yyjson_get_str(value)) != yyjson_get_len(value)) {
        return false;
    }
    return true;
}

yyjson_mut_doc *jobs_record_load(const char *dir, const char *id, char *err, size_t errlen) {
    tny::c_string path(path_join(dir, "job.json"));
    if (!path) return NULL;
    size_t len = 0;
    tny::c_string data(file_slurp(path.get(), &len));
    if (!data) {
        record_error(err, errlen, "no job record");
        return NULL;
    }
    if (len > TNY_JOBS_PAYLOAD_MAX) {
        record_error(err, errlen, "the job record is too large");
        return NULL;
    }
    tny::document doc(jparse(data.get(), len));
    data.reset(); /* The parsed document owns its bytes from this point. */
    yyjson_val *root = doc ? yyjson_doc_get_root(doc.get()) : NULL;
    if (!root || !yyjson_is_obj(root) || !json_unique_keys(root, 0)) {
        record_error(err, errlen, "the job record is not a JSON object");
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
        record_error(err, errlen, "the job record is not a supported version 1 record");
        return NULL;
    }
    yyjson_mut_doc *mut = yyjson_doc_mut_copy(doc.get(), jallocator());
    if (!mut) record_error(err, errlen, "out of memory");
    return mut;
}

int jobs_record_store(const char *dir, yyjson_mut_doc *doc) {
    tny::c_string json(jwrite_pretty(doc));
    if (!json) return ENOMEM;
    tny::c_string path(path_join(dir, "job.json"));
    return path ? tny_jobs_host_write_private(path.get(), json.get(), strlen(json.get())) : ENOMEM;
}

/* ---- state transactions ---- */

void jobs_txn::reset() noexcept {
    doc.reset();
    dir.reset();
    lock_fd.reset();
}

void jobs_txn_end(jobs_txn *t) {
    if (t) t->reset();
}

/* Bounded, never blocking: the state lock is only ever held for a few
 * in-memory edits plus one atomic write. */
int jobs_txn_begin(const char *dir, const char *id, jobs_txn *t, char *err, size_t errlen) {
    t->reset();
    char *lock = path_join(dir, "state.lock");
    if (!lock) return ENOMEM;
    tny::lock_descriptor state_lock;
    state_lock.adopt(tny_jobs_host_lock_open(lock));
    int fd = state_lock.borrow();
    free(lock);
    if (fd < 0) {
        record_error(err, errlen, "cannot open the job state lock");
        return EIO;
    }
    tny_jobs_lock_rc rc = TNY_JOBS_LOCK_BUSY;
    for (int i = 0; i < JOBS_STATE_LOCK_TRIES; i++) {
        rc = tny_jobs_host_lock_try(fd);
        if (rc != TNY_JOBS_LOCK_BUSY) break;
        tny_jobs_host_sleep_ms(JOBS_STATE_LOCK_WAIT_MS);
    }
    if (rc != TNY_JOBS_LOCK_ACQUIRED) {
        record_error(err, errlen, "another process is updating this job");
        return EBUSY;
    }
    t->lock_fd.adopt(state_lock.release());
    t->dir.reset(tny_alloc_strdup(dir));
    if (!t->dir) {
        record_error(err, errlen, "out of memory");
        jobs_txn_end(t);
        return ENOMEM;
    }
    t->doc.reset(jobs_record_load(dir, id, err, errlen));
    if (!t->doc) {
        jobs_txn_end(t);
        return EINVAL;
    }
    return 0;
}

int jobs_txn_commit(jobs_txn *t) {
    yyjson_mut_val *root = yyjson_mut_doc_get_root(t->doc.get());
    jm_set_int(t->doc.get(), root, "revision", jm_int(root, "revision", 0) + 1);
    char *now = now_iso8601();
    jm_set_str(t->doc.get(), root, "updated", now ? now : "");
    free(now);
    int rc = jobs_record_store(t->dir.get(), t->doc.get());
    jobs_txn_end(t);
    return rc;
}

} // namespace tny::jobs_detail
