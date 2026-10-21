#include "core/retrieval_sources.h"
#include "util/util.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool handle_valid(const char *s) {
    if (!s || strlen(s) != 16) return false;
    for (; *s; ++s)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return false;
    return true;
}

static int open_source(const char *path, struct stat *st) {
    int flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = path ? open(path, flags) : -1;
    if (fd >= 0 && (fstat(fd, st) != 0 || !S_ISREG(st->st_mode) || st->st_size < 0 ||
                    (uintmax_t)st->st_size > TNY_RETRIEVAL_BYTES_LIMIT)) {
        close(fd);
        fd = -1;
    }
    return fd;
}

/* Fixed-size SHA-256 chain avoids buffering a source or adding another crypto
 * implementation. This is an internal snapshot checksum, not a file SHA-256.
 * SHA256(previous_digest || next_64KiB_block), starting with 32 zero bytes.
 * The separately checked byte length disambiguates the final partial block. */
static bool copy_hash(int in, session_result_stream *out, size_t expected, char digest[65]) {
    unsigned char block[32 + 65536] = {0};
    size_t total = 0;
    for (;;) {
        size_t used = 0;
        while (used < sizeof block - 32) {
            ssize_t n = read(in, block + 32 + used, sizeof block - 32 - used);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) return false;
            if (!n) break;
            used += (size_t)n;
        }
        if (!used) break;
        if (used > expected - total) return false;
        if (out) session_result_stream_append(out, (const char *)block + 32, used);
        unsigned char next[32];
        if (!sha256(block, 32 + used, next)) return false;
        memcpy(block, next, sizeof next);
        total += used;
    }
    if (total != expected) return false;
    for (size_t i = 0; i < 32; ++i) snprintf(digest + 2 * i, 3, "%02x", block[i]);
    return true;
}

static bool unchanged(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
           a->st_mtime == b->st_mtime && a->st_ctime == b->st_ctime;
}

yyjson_mut_val *tny_retrieval_source_export(yyjson_mut_doc *doc, tny_session_state *owner,
                                            const char *artifact, const char *source,
                                            const char *coverage, size_t *bytes) {
    if (!doc || !owner || owner->ctx->no_save || !handle_valid(artifact) || !source || !coverage ||
        !bytes)
        return NULL;
    buf_t path;
    buf_init(&path);
    buf_appendf(&path, "%s/results/%s.txt", owner->dir, artifact);
    struct stat before, after;
    int fd = path.oom ? -1 : open_source(path.data, &before);
    char digest[65];
    bool ok = fd >= 0 && copy_hash(fd, NULL, (size_t)before.st_size, digest) &&
              fstat(fd, &after) == 0 && unchanged(&before, &after);
    if (fd >= 0) close(fd);
    yyjson_mut_val *entry = ok ? yyjson_mut_obj(doc) : NULL;
    ok = entry && yyjson_mut_obj_add_strcpy(doc, entry, "source", source) &&
         yyjson_mut_obj_add_strcpy(doc, entry, "path", path.data) &&
         yyjson_mut_obj_add_strcpy(doc, entry, "digest", digest) &&
         yyjson_mut_obj_add_strcpy(doc, entry, "coverage", coverage) &&
         yyjson_mut_obj_add_uint(doc, entry, "bytes", (uint64_t)before.st_size);
    buf_free(&path);
    if (ok) *bytes = (size_t)before.st_size;
    return ok ? entry : NULL;
}

yyjson_mut_val *tny_retrieval_source_import(yyjson_mut_doc *doc, tny_session_state *child,
                                            yyjson_val *entry, size_t *bytes) {
    if (!doc || !child || child->ctx->no_save || !bytes || !yyjson_is_obj(entry)) return NULL;
    const char *path = jget_str(entry, "path");
    const char *source = jget_str(entry, "source");
    const char *expected = jget_str(entry, "digest");
    const char *coverage = jget_str(entry, "coverage");
    yyjson_val *length = jget(entry, "bytes");
    if (!path || !source || !expected || strlen(expected) != 64 || !coverage ||
        !yyjson_is_uint(length) || yyjson_get_uint(length) > TNY_RETRIEVAL_BYTES_LIMIT)
        return NULL;
    struct stat before, after;
    int in = open_source(path, &before);
    session_result_stream *out = session_result_stream_new(child, 0);
    char digest[65];
    bool ok = in >= 0 && out && (uint64_t)before.st_size == yyjson_get_uint(length) &&
              copy_hash(in, out, (size_t)before.st_size, digest) && strcmp(digest, expected) == 0 &&
              fstat(in, &after) == 0 && unchanged(&before, &after);
    if (in >= 0) close(in);
    size_t stored_bytes = 0;
    char *handle = NULL;
    if (ok) handle = session_result_stream_finish_handle(out, &stored_bytes);
    else session_result_stream_abort(out);
    ok = handle && stored_bytes == yyjson_get_uint(length);
    yyjson_mut_val *result = ok ? yyjson_mut_obj(doc) : NULL;
    ok = result && yyjson_mut_obj_add_strcpy(doc, result, "source", source) &&
         yyjson_mut_obj_add_strcpy(doc, result, "handle", handle) &&
         yyjson_mut_obj_add_strcpy(doc, result, "digest", expected) &&
         yyjson_mut_obj_add_strcpy(doc, result, "coverage", coverage) &&
         yyjson_mut_obj_add_uint(doc, result, "bytes", yyjson_get_uint(length));
    if (!ok && handle) tny_retrieval_source_discard(child, handle);
    if (ok) *bytes = stored_bytes;
    free(handle);
    return ok ? result : NULL;
}

void tny_retrieval_source_discard(tny_session_state *child, const char *handle) {
    if (!child || child->ctx->no_save || !handle_valid(handle)) return;
    int root = open(child->dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root < 0) return;
    int results = openat(root, "results", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    close(root);
    if (results < 0) return;
    char name[21];
    snprintf(name, sizeof name, "%s.txt", handle);
    (void)unlinkat(results, name, 0);
    close(results);
}
