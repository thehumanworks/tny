/* Linked in place of image_io.o, with util.c rebuilt using
 * -Dcalloc=tny_image_fault_calloc. Faults exist only in this disposable binary. */
#include "util/image_io.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool fail_allocation;
static char retained_stage[TNY_IMAGE_IO_PATH_MAX + 16];

static bool fault(const char *name) {
    const char *mode = getenv("TNY_IMAGE_PUBLICATION_FAULT");
    return mode && strcmp(mode, name) == 0;
}

void *tny_image_fault_calloc(size_t count, size_t size);
void *tny_image_fault_calloc(size_t count, size_t size) {
    if (fail_allocation) {
        fputs("fixture: digest allocation failed\n", stderr);
        return NULL;
    }
    return calloc(count, size);
}

static bool fault_digest(const uint8_t *data, size_t len, uint8_t out[32]) {
    fail_allocation = fault("hash") && len >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0;
    bool ok = sha256(data, len, out);
    fail_allocation = false;
    if (fault("precommit-cancel") && len >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0)
        raise(SIGTERM);
    return ok;
}

static int publish_link(const char *source, const char *target) {
    if (fault("unsupported")) {
        errno = EOPNOTSUPP;
        return -1;
    }
    int rc = link(source, target);
    struct stat st;
    /* Capability probes link empty files; only paid output arms cleanup faults. */
    if (!rc && stat(source, &st) == 0 && st.st_size > 0) {
        if (fault("cleanup") || fault("cleanup-late-cancel"))
            snprintf(retained_stage, sizeof retained_stage, "%s", source);
        if (fault("late-cancel") || fault("cleanup-late-cancel")) raise(SIGTERM);
    }
    return rc;
}

static int cleanup_unlink(const char *path) {
    if (*retained_stage && strcmp(path, retained_stage) == 0) {
        fputs("fixture: committed temporary cleanup failed\n", stderr);
        errno = EACCES;
        return -1;
    }
    return unlink(path);
}

#define sha256 fault_digest
#define link   publish_link
#define unlink cleanup_unlink
#include "../../src/util/image_io.c"
#undef sha256
#undef link
#undef unlink
