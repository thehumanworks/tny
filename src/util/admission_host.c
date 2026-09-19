/* Admission persistence seam. Locks and atomic writes reuse jobs_host. */
#include "util/admission_host.h"
#include "util/jobs_host.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int sync_dir(const char *dir) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno;
    int rc = fsync(fd) == 0 ? 0 : errno;
    close(fd);
    return rc;
}
int tny_admission_host_prepare(const char *root, const char *group, const char *dir) {
    int rc = sync_dir(root);
    if (!rc) rc = tny_jobs_host_mkdir_private(group);
    if (!rc) rc = sync_dir(root);
    if (!rc) rc = tny_jobs_host_mkdir_private(dir);
    if (!rc) rc = sync_dir(group);
    return rc;
}
int tny_admission_host_read(const char *path, char **data, size_t *len) {
    *data = NULL;
    *len = 0;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return errno;
    struct stat st;
    int rc = 0;
    if (fstat(fd, &st) != 0) rc = errno;
    else if (!S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 262144) rc = EIO;
    char *bytes = NULL;
    if (!rc) {
        *len = (size_t)st.st_size;
        bytes = malloc(*len);
        if (!bytes) rc = ENOMEM;
    }
    size_t used = 0;
    while (!rc && used < *len) {
        ssize_t n = read(fd, bytes + used, *len - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) rc = n < 0 ? errno : EIO;
        else used += (size_t)n;
    }
    close(fd);
    if (rc) free(bytes);
    else *data = bytes;
    return rc;
}
int tny_admission_host_write(const char *dir, const char *path, const void *data, size_t len) {
    int rc = tny_jobs_host_write_private(path, data, len);
    if (rc) return rc;
    return sync_dir(dir);
}
