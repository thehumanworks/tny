#ifndef TNY_ADMISSION_HOST_H
#define TNY_ADMISSION_HOST_H
#include <stddef.h>
/* root must already exist. Persist newly created scope directories. */
int tny_admission_host_prepare(const char *root, const char *group, const char *dir);
/* Bounded no-follow regular-file read. Caller owns returned bytes. */
int tny_admission_host_read(const char *path, char **data, size_t *len);
/* jobs_host atomic private write plus parent-directory fsync. */
int tny_admission_host_write(const char *dir, const char *path, const void *data, size_t len);
#endif
