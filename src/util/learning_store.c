#include "util/learning_store.h"
#include <string.h>

void tny_learning_counter_add(tny_learning_counter *counter, bool ok) {
    if (counter->successes + counter->failures >= TNY_LEARNING_LIMIT) {
        counter->successes /= 2;
        counter->failures /= 2;
    }
    if (ok) ++counter->successes;
    else ++counter->failures;
}

static void counter_merge(tny_learning_counter *counter, const tny_learning_counter *delta) {
    counter->successes += delta->successes;
    counter->failures += delta->failures;
    while (counter->successes + counter->failures > TNY_LEARNING_LIMIT) {
        counter->successes /= 2;
        counter->failures /= 2;
    }
    if (delta->successes || delta->failures)
        memcpy(counter->session_id, delta->session_id, sizeof counter->session_id);
}

#ifndef __EMSCRIPTEN__
#include "util/util.h"
#include "yyjson.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static bool valid_session(const char *id) {
    if (!id || strlen(id) != 16) return false;
    for (unsigned i = 0; i < 16; ++i)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f') ||
              (id[i] >= 'A' && id[i] <= 'F')))
            return false;
    return true;
}

static bool private_fd(int fd, bool directory) {
    struct stat st;
    return fstat(fd, &st) == 0 && st.st_uid == geteuid() &&
           (directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode) && st.st_nlink == 1) &&
           (st.st_mode & 0777) == (directory ? 0700 : 0600);
}

/* All children are relative to a held directory fd; never follow a child link. */
static int open_directory(const tny_learning_store *store, bool create) {
    int root = open(store->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root < 0) return -1;
    struct stat st;
    if (fstat(root, &st) || st.st_uid != geteuid()) {
        close(root);
        return -1;
    }
    if (create && mkdirat(root, "learning", 0700) && errno != EEXIST) {
        close(root);
        return -1;
    }
    int dir = openat(root, "learning", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int saved = errno;
    close(root);
    errno = saved;
    if (dir >= 0 && !private_fd(dir, true)) {
        close(dir);
        errno = EACCES;
        return -1;
    }
    return dir;
}

/* Fixed schema: {"version":1,"rules":[{"successes":N,"failures":N,"session":""},...]}
 * Exact key counts and iteration reject duplicate and unknown fields too. */
static bool decode(const char *data, size_t len, tny_learning_counter counters[3]) {
    yyjson_doc *doc = yyjson_read(data, len, 0);
    if (!doc) return false;
    bool valid = false;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root) || yyjson_obj_size(root) != 2) goto done;
    yyjson_val *version = yyjson_obj_get(root, "version");
    yyjson_val *rules = yyjson_obj_get(root, "rules");
    if (!yyjson_is_uint(version) || yyjson_get_uint(version) != 1 || !yyjson_is_arr(rules) ||
        yyjson_arr_size(rules) != 3)
        goto done;
    for (size_t i = 0; i < 3; ++i) {
        yyjson_val *rule = yyjson_arr_get(rules, i);
        if (!yyjson_is_obj(rule) || yyjson_obj_size(rule) != 3) goto done;
        yyjson_val *success = yyjson_obj_get(rule, "successes");
        yyjson_val *failure = yyjson_obj_get(rule, "failures");
        yyjson_val *session = yyjson_obj_get(rule, "session");
        if (!yyjson_is_uint(success) || !yyjson_is_uint(failure) || !yyjson_is_str(session))
            goto done;
        uint64_t s = yyjson_get_uint(success), f = yyjson_get_uint(failure);
        size_t n = yyjson_get_len(session);
        const char *id = yyjson_get_str(session);
        if (s > TNY_LEARNING_LIMIT || f > TNY_LEARNING_LIMIT || s + f > TNY_LEARNING_LIMIT ||
            (n != 0 && (n != 16 || !valid_session(id))) || strlen(id) != n)
            goto done;
        counters[i].successes = (uint32_t)s;
        counters[i].failures = (uint32_t)f;
        memcpy(counters[i].session_id, id, n + 1);
    }
    valid = true;
done:
    yyjson_doc_free(doc);
    return valid;
}

static bool load(int dir, const char *name, tny_learning_counter counters[3]) {
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT;
    char data[1025];
    size_t n = 0;
    bool valid = private_fd(fd, false);
    while (valid && n < sizeof data) {
        ssize_t got = read(fd, data + n, sizeof data - n);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) valid = false;
        if (got <= 0) break;
        n += (size_t)got;
    }
    close(fd);
    return valid && n < sizeof data && decode(data, n, counters);
}

static bool save(int dir, const char *name, const tny_learning_counter counters[3]) {
    char data[1024];
    int len = snprintf(data, sizeof data,
                       "{\"version\":1,\"rules\":["
                       "{\"successes\":%u,\"failures\":%u,\"session\":\"%s\"},"
                       "{\"successes\":%u,\"failures\":%u,\"session\":\"%s\"},"
                       "{\"successes\":%u,\"failures\":%u,\"session\":\"%s\"}]}\n",
                       counters[0].successes, counters[0].failures, counters[0].session_id,
                       counters[1].successes, counters[1].failures, counters[1].session_id,
                       counters[2].successes, counters[2].failures, counters[2].session_id);
    if (len < 0 || (size_t)len >= sizeof data) return false;
    char temp[64];
    /* The stable per-workspace lock serializes temp creation. O_EXCL refuses stale files. */
    snprintf(temp, sizeof temp, "%s.%ld.tmp", name, (long)getpid());
    int fd = openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    bool ok = fchmod(fd, 0600) == 0;
    size_t offset = 0;
    while (ok && offset < (size_t)len) {
        ssize_t n = write(fd, data + offset, (size_t)len - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ok = false;
        else offset += (size_t)n;
    }
    if (ok) ok = fsync(fd) == 0;
    if (close(fd)) ok = false;
    if (ok) ok = renameat(dir, temp, dir, name) == 0;
    if (!ok) unlinkat(dir, temp, 0);
    return ok;
}

static void merge(tny_learning_store *store, tny_learning_counter counters[3]) {
    int dir = open_directory(store, true);
    if (dir < 0) return;
    char name[24], lockname[24];
    snprintf(name, sizeof name, "%s.json", store->key);
    snprintf(lockname, sizeof lockname, "%s.lock", store->key);
    int lock = openat(dir, lockname, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (lock < 0) {
        close(dir);
        return;
    }
    if (!private_fd(lock, false) || flock(lock, LOCK_EX | LOCK_NB)) goto done;
    tny_learning_counter merged[3] = {0};
    if (!load(dir, name, merged)) goto done;
    for (unsigned i = 0; i < 3; ++i) counter_merge(&merged[i], &store->delta[i]);
    if (save(dir, name, merged)) {
        memcpy(counters, merged, sizeof merged);
        memset(store->delta, 0, sizeof store->delta);
    }
done:
    close(lock); /* releases lock, including all failure paths */
    close(dir);
}
#endif

void tny_learning_store_init(tny_learning_store *store, const char *tny_dir, const char *workspace,
                             const char *session_id, bool persist,
                             tny_learning_counter counters[TNY_LEARNING_RULES]) {
    memset(store, 0, sizeof *store);
    memset(counters, 0, sizeof(tny_learning_counter) * TNY_LEARNING_RULES);
#ifdef __EMSCRIPTEN__
    (void)tny_dir;
    (void)workspace;
    (void)session_id;
    (void)persist;
#else
    if (valid_session(session_id)) memcpy(store->session_id, session_id, 17);
    if (!persist || !tny_dir || !*tny_dir || !workspace || !*workspace ||
        strlen(tny_dir) >= sizeof store->root)
        return;
    memcpy(store->root, tny_dir, strlen(tny_dir) + 1);
    snprintf(store->key, sizeof store->key, "%016llx",
             (unsigned long long)fnv1a(workspace, strlen(workspace)));
    store->active = true;
    int dir = open_directory(store, false);
    if (dir < 0) return;
    char name[24];
    snprintf(name, sizeof name, "%s.json", store->key);
    tny_learning_counter loaded[3] = {0};
    if (load(dir, name, loaded)) memcpy(counters, loaded, sizeof loaded);
    close(dir);
#endif
}

void tny_learning_store_flush(tny_learning_store *store,
                              tny_learning_counter counters[TNY_LEARNING_RULES]) {
#ifndef __EMSCRIPTEN__
    if (!store->active) return;
    for (unsigned i = 0; i < TNY_LEARNING_RULES; ++i) {
        if (store->delta[i].successes || store->delta[i].failures) {
            merge(store, counters);
            break;
        }
    }
#else
    (void)store;
    (void)counters;
#endif
}

void tny_learning_store_carry(tny_learning_store *store,
                              tny_learning_counter counters[TNY_LEARNING_RULES],
                              const tny_learning_counter pending[TNY_LEARNING_RULES]) {
    memcpy(store->delta, pending, sizeof store->delta);
    for (unsigned i = 0; i < TNY_LEARNING_RULES; ++i) counter_merge(&counters[i], &pending[i]);
    tny_learning_store_flush(store, counters);
}

void tny_learning_store_record(tny_learning_store *store,
                               tny_learning_counter counters[TNY_LEARNING_RULES], unsigned rule,
                               bool ok) {
    if (rule >= TNY_LEARNING_RULES) return;
    tny_learning_counter_add(&counters[rule], ok);
    memcpy(counters[rule].session_id, store->session_id, 17);
    if (!store->active) return;
    tny_learning_counter_add(&store->delta[rule], ok);
    memcpy(store->delta[rule].session_id, store->session_id, 17);
    tny_learning_store_flush(store, counters);
}
