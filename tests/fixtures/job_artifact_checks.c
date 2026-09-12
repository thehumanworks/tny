/* Compile the real jobs implementation here to exercise its private producer
 * analyzer. The selector and all its filesystem dependencies stay real. */
#include "core/jobs.h"
#include "util/image_io.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *allocation_value;
static const char *swap_path, *foreign_path;
static int swapped, foreign_reads;

char *job_test_strdup(const char *s) {
    if (allocation_value && s && strcmp(s, allocation_value) == 0) return NULL;
    return xstrdup(s);
}

int job_test_openat(int dir, const char *path, int flags, ...);
int job_test_openat(int dir, const char *path, int flags, ...) {
    const char *leaf = swap_path ? strrchr(swap_path, '/') : NULL;
    if (!swapped && leaf && strcmp(path, leaf + 1) == 0) {
        char saved[8192];
        snprintf(saved, sizeof saved, "%s.held", swap_path);
        if (rename(swap_path, saved) || symlink(foreign_path, swap_path)) abort();
        swapped = 1;
    }
    return openat(dir, path, flags);
}

ssize_t job_test_read(int fd, void *data, size_t length);
ssize_t job_test_read(int fd, void *data, size_t length) {
    struct stat actual, foreign;
    if (foreign_path && fstat(fd, &actual) == 0 && stat(foreign_path, &foreign) == 0 &&
        actual.st_dev == foreign.st_dev && actual.st_ino == foreign.st_ino)
        foreign_reads++;
    return read(fd, data, length);
}

#define xstrdup job_test_strdup
#include "../../src/core/jobs.c"
#undef xstrdup

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (strcmp(argv[1], "analyze") == 0) {
        if (argc != 4 && argc != 5) return 2;
        if (argc == 5) allocation_value = argv[4];
        char *hash = NULL, *manifest = NULL, *operation = NULL;
        long long bytes = -1;
        bool ok = analyze_image_log(argv[2], argv[3], &hash, &bytes, &manifest, &operation);
        printf("{\"ok\":%s,\"bytes\":%lld,\"hash_present\":%s,\"manifest_present\":%s,\"operation_"
               "present\":%s}\n",
               ok ? "true" : "false", bytes, hash ? "true" : "false", manifest ? "true" : "false",
               operation ? "true" : "false");
        free(hash);
        free(manifest);
        free(operation);
        return 0;
    }
    if (strcmp(argv[1], "select") != 0 || (argc != 6 && argc != 8)) return 2;
    tny_ctx ctx = {.cwd = argv[2], .tny_dir = argv[3]};
    if (argc == 8) {
        swap_path = argv[6];
        foreign_path = argv[7];
    }
    char err[256];
    tny_job_artifact *a = tny_jobs_select_artifact(&ctx, argv[4], atoi(argv[5]), err, sizeof err);
    printf("{\"ok\":%s,\"swapped\":%d,\"foreign_reads\":%d}\n", a ? "true" : "false", swapped,
           foreign_reads);
    tny_jobs_artifact_free(a);
    if (swapped) {
        char saved[8192];
        snprintf(saved, sizeof saved, "%s.held", swap_path);
        if (unlink(swap_path) || rename(saved, swap_path)) abort();
    }
    return 0;
}
