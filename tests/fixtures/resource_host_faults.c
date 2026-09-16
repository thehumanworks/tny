/* Syscall failures around the unchanged C host seams. Test binary only. */
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

int tny_resource_fault;
int tny_resource_dup_at;
static int dup_calls;
void tny_resource_fault_set(int fault, int dup_at);
int tny_resource_open(const char *path, int flags, ...);
ssize_t tny_resource_write(int fd, const void *data, size_t size);
int tny_resource_fsync(int fd);
int tny_resource_rename(const char *from, const char *to);
int tny_resource_fcntl(int fd, int command, ...);
int tny_resource_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                       const posix_spawnattr_t *attrs, char *const argv[], char *const envp[]);

void tny_resource_fault_set(int fault, int dup_at) {
    tny_resource_fault = fault;
    tny_resource_dup_at = dup_at;
    dup_calls = 0;
}
int tny_resource_open(const char *path, int flags, ...) {
    if (tny_resource_fault == 1) {
        errno = EMFILE;
        return -1;
    }
    if (!(flags & O_CREAT)) return open(path, flags);
    va_list args;
    va_start(args, flags);
    int mode = va_arg(args, int);
    va_end(args);
    return open(path, flags, mode);
}
ssize_t tny_resource_write(int fd, const void *data, size_t size) {
    if (tny_resource_fault == 2) {
        errno = ENOSPC;
        return -1;
    }
    return write(fd, data, size);
}
int tny_resource_fsync(int fd) {
    if (tny_resource_fault == 3) {
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}
int tny_resource_rename(const char *from, const char *to) {
    if (tny_resource_fault == 4) {
        errno = EIO;
        return -1;
    }
    return rename(from, to);
}
int tny_resource_fcntl(int fd, int command, ...) {
    if (command == F_DUPFD_CLOEXEC && tny_resource_fault == 5 &&
        ++dup_calls == tny_resource_dup_at) {
        errno = EMFILE;
        return -1;
    }
    if (command == F_GETFD || command == F_GETFL) return fcntl(fd, command);
    va_list args;
    va_start(args, command);
    int value = va_arg(args, int);
    va_end(args);
    return fcntl(fd, command, value);
}
int tny_resource_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                       const posix_spawnattr_t *attrs, char *const argv[], char *const envp[]) {
    if (tny_resource_fault == 6) return EAGAIN;
    return posix_spawn(pid, path, actions, attrs, argv, envp);
}
