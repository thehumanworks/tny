#include "util/image_io.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GUARD_LINE_MAX 128

static void reason(char *err, size_t errlen, const char *why) {
    if (err && errlen) snprintf(err, errlen, "%s", why);
}

bool tny_image_io_sha256_hex(const void *data, size_t len, char out[65]) {
    uint8_t digest[32];
    if (!out) return false;
    out[0] = 0;
    if (!sha256((const uint8_t *)data, len, digest)) return false;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[64] = 0;
    return true;
}

char *tny_image_io_canonical(const char *path, char *err, size_t errlen) {
    if (!path || !*path || strlen(path) > TNY_IMAGE_IO_PATH_MAX) {
        reason(err, errlen, "image output path is missing or too long");
        return NULL;
    }
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    if (!*base || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
        reason(err, errlen, "image output needs a file name, not a directory path");
        return NULL;
    }
    char *dir = !slash          ? xstrdup(".")
                : slash == path ? xstrdup("/")
                                : xstrndup(path, (size_t)(slash - path));
    struct stat st;
    /* realpath() only answers for an existing directory, which is exactly the
     * precondition: tny never creates parent directories for an artifact. */
    char *parent = dir && stat(dir, &st) == 0 && S_ISDIR(st.st_mode) ? path_abs(dir) : NULL;
    free(dir);
    if (!parent) {
        reason(err, errlen, "image output directory does not exist");
        return NULL;
    }
    char *full = path_join(parent, base);
    free(parent);
    if (!full) {
        reason(err, errlen, "cannot resolve the image output path");
        return NULL;
    }
    if (lstat(full, &st) == 0) {
        const char *why = S_ISLNK(st.st_mode) ? "image output must not be a symlink"
                          : !S_ISREG(st.st_mode)
                              ? "image output must be a regular file, not a directory or device"
                          : st.st_nlink > 1
                              ? "image output has more than one hard link; tny replaces the name "
                                "atomically and will not record an ambiguous artifact identity"
                              : NULL;
        if (why) {
            reason(err, errlen, why);
            free(full);
            return NULL;
        }
    }
    return full;
}

bool tny_image_io_same_file(const char *a, const char *b) {
    struct stat sa, sb;
    if (!a || !b || stat(a, &sa) != 0 || stat(b, &sb) != 0) return false;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

/* Unique private temporary beside `path`; the name never contains the process
 * id, so two operations cannot collide on it. Returns the open fd. */
static int stage(const char *path, char **tmp_out) {
    buf_t tmp;
    buf_init(&tmp);
    buf_appendf(&tmp, "%s.tny-tmp-XXXXXX", path);
    if (tmp.oom) {
        buf_free(&tmp);
        return -1;
    }
    int fd = mkstemp(tmp.data);
    if (fd < 0) {
        buf_free(&tmp);
        return -1;
    }
    if (fchmod(fd, 0600) != 0) {
        close(fd);
        unlink(tmp.data);
        buf_free(&tmp);
        return -1;
    }
    *tmp_out = buf_detach(&tmp);
    buf_free(&tmp);
    if (!*tmp_out) {
        close(fd);
        return -1;
    }
    return fd;
}

static int write_all(int fd, const void *data, size_t len) {
    const char *p = data;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

int tny_image_io_write_new(const char *path, const void *data, size_t len) {
    if (!path) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    int rc = write_all(fd, data, len);
    if (close(fd) != 0) rc = -1;
    if (rc) unlink(path);
    return rc;
}

int tny_image_io_replace(const char *path, const void *data, size_t len) {
    char *tmp = NULL;
    int fd = path ? stage(path, &tmp) : -1;
    if (fd < 0) return -1;
    int rc = write_all(fd, data, len);
    if (close(fd) != 0) rc = -1;
    if (!rc && rename(tmp, path) != 0) rc = -1;
    if (rc) unlink(tmp);
    free(tmp);
    return rc;
}

static int read_acquired_bounded(int fd, size_t max, buf_t *out) {
    struct stat st;
    int rc = -1;
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > (uint64_t)max)
        goto done;
    for (;;) {
        char chunk[8192];
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || (n > 0 && (size_t)n > max - out->len)) goto done;
        if (!n) break;
        buf_append(out, chunk, (size_t)n);
        if (out->oom) goto done;
    }
    rc = 0;
done:
    close(fd);
    if (rc) {
        buf_free(out);
        buf_init(out);
    }
    return rc;
}

int tny_image_io_read_bounded(const char *path, size_t max, buf_t *out) {
    int fd = path ? open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK) : -1;
    return read_acquired_bounded(fd, max, out);
}

/* Walk beneath an acquired directory, rejecting every symlink and traversal
 * component. A renamed directory remains the same authority through its fd. */
static int open_beneath(int directory, const char *relative, bool leaf_directory) {
    if (!relative || !*relative || strlen(relative) > TNY_IMAGE_IO_PATH_MAX) return -1;
    char *components = xstrdup(relative);
    if (!components) return -1;
    int current = dup(directory);
    char *part = components;
    while (current >= 0) {
        char *slash = strchr(part, '/');
        if (slash) *slash = 0;
        if (!*part || strcmp(part, ".") == 0 || strcmp(part, "..") == 0) {
            close(current);
            current = -1;
            break;
        }
        int flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW;
        if (slash || leaf_directory) flags |= O_DIRECTORY;
        int next = openat(current, part, flags);
        close(current);
        current = next;
        if (!slash) break;
        part = slash + 1;
    }
    free(components);
    return current;
}

int tny_image_io_read_confined(const char *root, const char *path, size_t max, buf_t *out) {
    if (!root || *root != '/' || !path || *path != '/' || !path_is_within(root, path)) return -1;
    size_t root_len = strlen(root);
    while (root_len > 1 && root[root_len - 1] == '/') root_len--;
    const char *relative = path + root_len;
    if (*relative == '/') relative++;
    if (!*relative) return -1;
    int slash = open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (slash < 0) return -1;
    char *name = root_len > 1 ? xstrndup(root + 1, root_len - 1) : NULL;
    int authority = root_len == 1 ? dup(slash) : name ? open_beneath(slash, name, true) : -1;
    free(name);
    close(slash);
    if (authority < 0) return -1;
    int fd = open_beneath(authority, relative, false);
    close(authority);
    return read_acquired_bounded(fd, max, out);
}

/* ---- inputs and the destination commit protocol (ADR 0094) ---- */

int tny_image_io_read_input(const char *path, size_t max, buf_t *out, tny_image_io_id *id,
                            char *err, size_t errlen) {
    if (id) *id = (tny_image_io_id){0};
    /* O_NONBLOCK so a FIFO left in place of a picture cannot hang the export
     * before fstat rejects it. */
    int fd = path ? open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK) : -1;
    struct stat st;
    int rc = -1;
    if (fd < 0) {
        reason(err, errlen, "cannot open the image to export");
        return -1;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > (uint64_t)max) {
        reason(err, errlen, "image sources must be regular files within the size limit");
        goto done;
    }
    for (;;) {
        char chunk[8192];
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || (n > 0 && (size_t)n > max - out->len)) {
            reason(err, errlen, "cannot read the image to export");
            goto done;
        }
        if (!n) break;
        buf_append(out, chunk, (size_t)n);
        if (out->oom) {
            reason(err, errlen, "cannot read the image to export");
            goto done;
        }
    }
    /* The identity of the descriptor the bytes came from, not of the name. */
    if (id)
        *id = (tny_image_io_id){
            .present = true, .dev = (uint64_t)st.st_dev, .ino = (uint64_t)st.st_ino};
    rc = 0;
done:
    close(fd);
    if (rc) {
        buf_free(out);
        buf_init(out);
    }
    return rc;
}

struct tny_image_commit {
    int parent_fd;
    char *name;  /* final basename, relative to parent_fd */
    char *stage; /* staged basename while it exists */
    bool overwrite;
    tny_image_io_id target;
};

static bool commit_observe(int parent_fd, const char *name, bool overwrite, tny_image_io_id *out,
                           char *err, size_t errlen) {
    struct stat st;
    *out = (tny_image_io_id){0};
    if (fstatat(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return true;
        reason(err, errlen, "cannot inspect the export destination");
        return false;
    }
    if (!overwrite) {
        reason(err, errlen,
               "the export destination already exists; pass --overwrite to replace it");
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        reason(err, errlen, "the export destination is a symlink; tny will not write through it");
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        reason(err, errlen, "the export destination is not a regular file");
        return false;
    }
    if (st.st_nlink > 1) {
        reason(err, errlen,
               "the export destination has more than one hard link; tny will not replace an "
               "ambiguous name");
        return false;
    }
    *out =
        (tny_image_io_id){.present = true, .dev = (uint64_t)st.st_dev, .ino = (uint64_t)st.st_ino};
    return true;
}

tny_image_commit *tny_image_io_commit_open(const char *canonical, bool overwrite, char *err,
                                           size_t errlen) {
    const char *slash = canonical ? strrchr(canonical, '/') : NULL;
    if (!canonical || *canonical != '/' || !slash || !slash[1]) {
        reason(err, errlen, "invalid export destination");
        return NULL;
    }
    char *dir =
        slash == canonical ? xstrdup("/") : xstrndup(canonical, (size_t)(slash - canonical));
    /* One resolution of the parent, then a retained descriptor: every later
     * step names the leaf relative to it, so a directory component swapped
     * afterwards cannot redirect the stage or the install. */
    int fd = dir ? open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
    free(dir);
    if (fd < 0) {
        reason(err, errlen, "cannot open the export destination directory");
        return NULL;
    }
    tny_image_commit *c = calloc(1, sizeof *c);
    if (c) {
        c->parent_fd = fd;
        c->overwrite = overwrite;
        c->name = xstrdup(slash + 1);
    }
    if (!c || !c->name || !commit_observe(fd, slash + 1, overwrite, &c->target, err, errlen)) {
        if (c) free(c->name);
        free(c);
        close(fd);
        if (!c) reason(err, errlen, "cannot prepare the export destination");
        return NULL;
    }
    return c;
}

tny_image_io_id tny_image_io_commit_target(const tny_image_commit *c) {
    return c ? c->target : (tny_image_io_id){0};
}

int tny_image_io_commit_stage(tny_image_commit *c, const void *data, size_t len, char *err,
                              size_t errlen) {
    if (!c || c->stage) {
        reason(err, errlen, "the export destination is not ready");
        return -1;
    }
    char *id = gen_id();
    buf_t name;
    buf_init(&name);
    if (id) buf_appendf(&name, ".tny-image-export-%s", id);
    free(id);
    c->stage = name.oom || !name.len ? NULL : buf_detach(&name);
    buf_free(&name);
    int fd = c->stage ? openat(c->parent_fd, c->stage,
                               O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)
                      : -1;
    if (fd < 0) {
        free(c->stage);
        c->stage = NULL;
        reason(err, errlen, "cannot create the export staging file");
        return -1;
    }
    int rc = write_all(fd, data, len);
    if (!rc && fsync(fd) != 0) rc = -1;
    if (close(fd) != 0) rc = -1;
    if (rc) {
        (void)unlinkat(c->parent_fd, c->stage, 0);
        free(c->stage);
        c->stage = NULL;
        reason(err, errlen, "cannot write the export staging file");
    }
    return rc;
}

int tny_image_io_commit_finish(tny_image_commit *c, char *err, size_t errlen) {
    if (!c || !c->stage) {
        reason(err, errlen, "nothing was staged for this export");
        return -1;
    }
    tny_image_io_id now;
    struct stat st;
    /* The destination as it is right now, without following a symlink that
     * may have appeared since it was opened. */
    if (fstatat(c->parent_fd, c->name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno != ENOENT) {
            reason(err, errlen, "cannot inspect the export destination");
            return -1;
        }
        now = (tny_image_io_id){0};
    } else
        now = (tny_image_io_id){
            .present = true, .dev = (uint64_t)st.st_dev, .ino = (uint64_t)st.st_ino};
    if (now.present != c->target.present ||
        (now.present && (now.dev != c->target.dev || now.ino != c->target.ino)) ||
        (now.present && (!S_ISREG(st.st_mode) || st.st_nlink > 1))) {
        reason(err, errlen,
               "the export destination changed while this export was running; nothing was "
               "replaced");
        return -1;
    }
    int rc;
    if (c->overwrite) {
        /* renameat replaces the directory entry. It does not open, truncate or
         * follow the old target, so bytes behind a substituted link stay
         * untouched. */
        rc = renameat(c->parent_fd, c->stage, c->parent_fd, c->name);
        if (rc != 0) reason(err, errlen, "cannot install the exported image");
    } else {
        /* A fresh destination is created by link, so two creators racing for
         * the same new name cannot both win: the loser gets EEXIST. */
        rc = linkat(c->parent_fd, c->stage, c->parent_fd, c->name, 0);
        if (rc != 0)
            reason(err, errlen,
                   errno == EEXIST
                       ? "another writer created the export destination first; nothing was replaced"
                       : "cannot install the exported image");
        (void)unlinkat(c->parent_fd, c->stage, 0);
    }
    if (rc == 0) {
        free(c->stage);
        c->stage = NULL;
        (void)fsync(c->parent_fd);
    }
    return rc == 0 ? 0 : -1;
}

void tny_image_io_commit_close(tny_image_commit *c) {
    if (!c) return;
    if (c->stage) (void)unlinkat(c->parent_fd, c->stage, 0);
    free(c->stage);
    free(c->name);
    close(c->parent_fd);
    free(c);
}

/* ---- per-destination writer guard ---- */

int tny_image_io_publish(const char *tmp, const char *canonical, bool no_replace) {
    if (!no_replace) return rename(tmp, canonical) == 0 ? 0 : -1;
    if (link(tmp, canonical) != 0) return -1;
    /* Link is the commit point. A cleanup failure cannot undo it. */
    return unlink(tmp) == 0 ? 0 : 1;
}

#ifdef __EMSCRIPTEN__

int tny_image_io_no_replace_preflight(const char *canonical, char *err, size_t errlen) {
    (void)canonical;
    reason(err, errlen, "atomic no-replace image publication is unsupported on wasm");
    return -1;
}
/* wasm has no advisory file locking: emscripten's filesystems are per-instance
 * and a flock() stub would answer "acquired" for every contender. The honest
 * guard is therefore a table inside this instance, which is exactly the scope
 * in which two concurrent tny operations can actually exist here. */
struct tny_image_guard {
    char *canonical;
    char operation[TNY_IMAGE_IO_ID_MAX];
};

#define GUARD_SLOTS 16
static tny_image_guard *held[GUARD_SLOTS];

static tny_image_guard *find_held(const char *canonical) {
    for (size_t i = 0; i < GUARD_SLOTS; i++)
        if (held[i] && strcmp(held[i]->canonical, canonical) == 0) return held[i];
    return NULL;
}

tny_image_guard *tny_image_io_guard_acquire(const char *canonical, const char *operation_id,
                                            char *err, size_t errlen) {
    if (!canonical || !operation_id || !*operation_id ||
        strlen(operation_id) >= TNY_IMAGE_IO_ID_MAX) {
        reason(err, errlen, "cannot reserve the image destination");
        return NULL;
    }
    if (find_held(canonical)) {
        reason(err, errlen, "another image operation is already writing this output");
        return NULL;
    }
    size_t slot = 0;
    while (slot < GUARD_SLOTS && held[slot]) slot++;
    tny_image_guard *g = slot < GUARD_SLOTS ? calloc(1, sizeof *g) : NULL;
    if (!g) {
        reason(err, errlen, "cannot reserve the image destination");
        return NULL;
    }
    g->canonical = xstrdup(canonical);
    if (!g->canonical) {
        free(g);
        reason(err, errlen, "cannot reserve the image destination");
        return NULL;
    }
    snprintf(g->operation, sizeof g->operation, "%s", operation_id);
    held[slot] = g;
    return g;
}

void tny_image_io_guard_release(tny_image_guard *g) {
    if (!g) return;
    for (size_t i = 0; i < GUARD_SLOTS; i++)
        if (held[i] == g) held[i] = NULL;
    free(g->canonical);
    free(g);
}

bool tny_image_io_guard_owner(const char *canonical, char out[TNY_IMAGE_IO_ID_MAX]) {
    out[0] = 0;
    tny_image_guard *g = canonical ? find_held(canonical) : NULL;
    if (!g) return false;
    snprintf(out, TNY_IMAGE_IO_ID_MAX, "%s", g->operation);
    return true;
}
#else

int tny_image_io_no_replace_preflight(const char *canonical, char *err, size_t errlen) {
    struct stat st;
    if (lstat(canonical, &st) == 0 || errno != ENOENT) {
        reason(err, errlen, "image no-replace destination exists or cannot be inspected");
        return -1;
    }
    /* Probe actual filesystem capability, not merely whether link() exists.
     * Two exclusively reserved names in the final directory avoid clobbering
     * anything while checking. No provider has been contacted yet. */
    buf_t source, target;
    buf_init(&source);
    buf_init(&target);
    buf_appendf(&source, "%s.XXXXXX", canonical);
    buf_appendf(&target, "%s.XXXXXX", canonical);
    int a = source.oom ? -1 : mkstemp(source.data);
    int b = target.oom ? -1 : mkstemp(target.data);
    int rc = -1;
    bool target_owned = b >= 0;
    if (a >= 0 && b >= 0 && unlink(target.data) == 0) {
        target_owned = false;
        if (link(source.data, target.data) == 0) {
            target_owned = true;
            rc = 0;
        }
    }
    if (a >= 0) {
        close(a);
        if (unlink(source.data) != 0) rc = -1;
    }
    if (b >= 0) {
        close(b);
        if (target_owned && unlink(target.data) != 0) rc = -1;
    }
    buf_free(&source);
    buf_free(&target);
    if (rc) reason(err, errlen, "cannot prepare atomic no-replace image publication");
    return rc;
}
#include <sys/file.h>

struct tny_image_guard {
    int fd;
    char *path;
};

/* Guards live under ~/.tny/image-guards/<sha256 of canonical path>.lock rather
 * than beside the artifact: a lock file that stays forever (see below) has no
 * business appearing in a user's picture directory, and the hashed name keeps
 * one fixed, collision-free entry per normalized destination. */
static char *guard_path(const char *canonical) {
    char digest[65];
    if (!canonical || !tny_image_io_sha256_hex(canonical, strlen(canonical), digest)) return NULL;
    char *root = path_tny_dir();
    char *dir = root ? path_join(root, "image-guards") : NULL;
    free(root);
    if (!dir) return NULL;
    if (mkdir_p(dir) != 0) {
        free(dir);
        return NULL;
    }
    buf_t name;
    buf_init(&name);
    buf_appendf(&name, "%s.lock", digest);
    char *file = name.oom ? NULL : path_join(dir, name.data);
    buf_free(&name);
    free(dir);
    return file;
}

static void owner_from_line(const char *line, size_t len, char out[TNY_IMAGE_IO_ID_MAX]) {
    /* "tny-image-guard 1 <operation-id> <pid>"; anything else is an unknown
     * holder, never a fabricated identity. */
    static const char prefix[] = "tny-image-guard 1 ";
    out[0] = 0;
    if (len < sizeof prefix - 1 || memcmp(line, prefix, sizeof prefix - 1) != 0) return;
    const char *id = line + sizeof prefix - 1;
    size_t n = 0;
    while (id + n < line + len && id[n] != ' ' && id[n] != '\n' && n < TNY_IMAGE_IO_ID_MAX - 1) n++;
    if (!n || (id + n < line + len && id[n] != ' ' && id[n] != '\n')) return;
    memcpy(out, id, n);
    out[n] = 0;
}

/* True when this fd still refers to the file that `path` names right now. A
 * lock taken on an inode that has since been unlinked or replaced guards
 * nothing, so it must never count as ownership. */
static bool still_current(int fd, const char *path) {
    struct stat open_file, named;
    return fstat(fd, &open_file) == 0 && stat(path, &named) == 0 &&
           open_file.st_dev == named.st_dev && open_file.st_ino == named.st_ino;
}

tny_image_guard *tny_image_io_guard_acquire(const char *canonical, const char *operation_id,
                                            char *err, size_t errlen) {
    reason(err, errlen, "cannot reserve the image destination");
    char *file = operation_id && *operation_id && strlen(operation_id) < TNY_IMAGE_IO_ID_MAX
                     ? guard_path(canonical)
                     : NULL;
    if (!file) return NULL;
    char line[GUARD_LINE_MAX];
    int n = snprintf(line, sizeof line, "tny-image-guard 1 %s %ld\n", operation_id, (long)getpid());
    tny_image_guard *g = n > 0 && (size_t)n < sizeof line ? calloc(1, sizeof *g) : NULL;
    /* A released guard is unlinked (below), so a contender can find itself
     * holding a name that no longer exists. Bounded retries settle that race
     * without ever letting two live owners through. */
    for (int attempt = 0; g && attempt < 8; attempt++) {
        int fd = open(file, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) break;
        if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
            close(fd);
            reason(err, errlen, "another image operation is already writing this output");
            free(file);
            free(g);
            return NULL;
        }
        if (!still_current(fd, file)) {
            close(fd); /* someone released and unlinked it; take the new name */
            continue;
        }
        /* Ownership is this live handle. The record below only lets a reader
         * name the holder; it is never trusted as proof of a live process. */
        if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) != 0 ||
            write_all(fd, line, (size_t)n) != 0) {
            close(fd); /* closing the last fd of this description unlocks it */
            break;
        }
        g->fd = fd;
        g->path = file;
        return g;
    }
    free(file);
    free(g);
    return NULL;
}

void tny_image_io_guard_release(tny_image_guard *g) {
    if (!g) return;
    /* Unlink while the lock is still held, then close. A contender that opened
     * this inode first can still acquire its lock, but it will see the name no
     * longer resolves here and retry, so the two are never both owners. */
    unlink(g->path);
    close(g->fd);
    free(g->path);
    free(g);
}

bool tny_image_io_guard_owner(const char *canonical, char out[TNY_IMAGE_IO_ID_MAX]) {
    out[0] = 0;
    char *file = guard_path(canonical);
    int fd = file ? open(file, O_RDONLY | O_CLOEXEC | O_NOFOLLOW) : -1;
    free(file);
    if (fd < 0) return false;
    if (flock(fd, LOCK_SH | LOCK_NB) == 0) {
        close(fd); /* success drops our shared lock: nobody owns this output */
        return false;
    }
    char line[GUARD_LINE_MAX];
    ssize_t n = read(fd, line, sizeof line - 1);
    close(fd);
    if (n > 0) owner_from_line(line, (size_t)n, out);
    return true;
}
#endif
