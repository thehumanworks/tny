// Private durable-job record ownership; not part of the public C ABI.
#ifndef TNY_CORE_JOBS_RECORD_HPP
#define TNY_CORE_JOBS_RECORD_HPP
#include "util/resources.hpp"
#include "json/ownership.hpp"

namespace tny::jobs_detail {
/* ---- mutable-document accessors (job.json is read-modify-written) ---- */

inline void jm_set_str(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                       const char *value) {
    yyjson_mut_val *v = value ? yyjson_mut_strcpy(doc, value) : yyjson_mut_null(doc);
    yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc, key), v);
}

inline void jm_set_int(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key, int64_t value) {
    yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc, key), yyjson_mut_sint(doc, value));
}

inline void jm_set_null(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key) {
    yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc, key), yyjson_mut_null(doc));
}

inline void jm_set_bool(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key, bool value) {
    yyjson_mut_val *existing = yyjson_mut_obj_get(obj, key);
    if (yyjson_mut_is_bool(existing)) {
        /* Re-latching a durable cleanup hold must never allocate or erase it. */
        yyjson_mut_set_bool(existing, value);
        return;
    }
    yyjson_mut_val *name = yyjson_mut_strcpy(doc, key);
    yyjson_mut_val *replacement = yyjson_mut_bool(doc, value);
    /* yyjson interprets a NULL value as removal, not insertion failure. */
    if (name && replacement) yyjson_mut_obj_put(obj, name, replacement);
}

inline const char *jm_str(yyjson_mut_val *obj, const char *key) {
    return yyjson_mut_get_str(yyjson_mut_obj_get(obj, key));
}

inline int64_t jm_int(yyjson_mut_val *obj, const char *key, int64_t dflt) {
    yyjson_mut_val *v = yyjson_mut_obj_get(obj, key);
    return v && yyjson_mut_is_int(v) ? yyjson_mut_get_sint(v) : dflt;
}

inline bool jm_bool(yyjson_mut_val *obj, const char *key, bool dflt) {
    yyjson_mut_val *v = yyjson_mut_obj_get(obj, key);
    return v && yyjson_mut_is_bool(v) ? yyjson_mut_get_bool(v) : dflt;
}

inline yyjson_mut_val *jm_items(yyjson_mut_doc *doc) {
    yyjson_mut_val *items = yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc), "items");
    return items && yyjson_mut_is_arr(items) ? items : NULL;
}

inline yyjson_mut_val *jm_item(yyjson_mut_doc *doc, int index) {
    yyjson_mut_val *items = jm_items(doc);
    return items ? yyjson_mut_arr_get(items, (size_t)index) : NULL;
}

inline int jm_item_count(yyjson_mut_doc *doc) {
    yyjson_mut_val *items = jm_items(doc);
    return items ? (int)yyjson_mut_arr_size(items) : 0;
}

struct jobs_txn {
    // Reverse member destruction releases the document and path before the lock.
    tny::lock_descriptor lock_fd;
    tny::c_string dir;
    tny::mutable_document doc;
    jobs_txn() noexcept = default;
    ~jobs_txn() noexcept = default;
    jobs_txn(const jobs_txn &) = delete;
    jobs_txn &operator=(const jobs_txn &) = delete;
    jobs_txn(jobs_txn &&) = delete;
    jobs_txn &operator=(jobs_txn &&) = delete;
    void reset() noexcept;
};

bool state_is_known(const char *state);
bool record_state_is_known(yyjson_val *state);
bool json_unique_keys(yyjson_val *value, unsigned depth);
// Caller owns the returned document. Loading never projects or repairs a record.
yyjson_mut_doc *jobs_record_load(const char *dir, const char *id, char *err, size_t errlen);
int jobs_record_store(const char *dir, yyjson_mut_doc *doc);
// Begin acquires the lock and validated document; commit writes once and releases.
// Neither reset nor destruction publishes a terminal state or waits for a child.
void jobs_txn_end(jobs_txn *t);
int jobs_txn_begin(const char *dir, const char *id, jobs_txn *t, char *err, size_t errlen);
int jobs_txn_commit(jobs_txn *t);
} // namespace tny::jobs_detail

#endif // TNY_CORE_JOBS_RECORD_HPP
