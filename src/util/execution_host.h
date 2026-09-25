/* Private fresh-exec channel; all native effects live in this host seam. */
#ifndef TNY_EXECUTION_HOST_H
#define TNY_EXECUTION_HOST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
typedef struct {
    int fd;
    pid_t pid;
    bool reaped;
} tny_exec_host;
typedef bool (*tny_exec_cancel_fn)(void *);
int tny_exec_host_start(tny_exec_host *host);
/* Server validates an inherited connected AF_UNIX socket on fd3, marks CLOEXEC. */
int tny_exec_host_accept(void);
int tny_exec_host_send(int fd, const char *json, int64_t deadline, tny_exec_cancel_fn cancel,
                       void *ud);
/* NULL: EOF/invalid length/partial frame/deadline/cancel. errno distinguishes. */
char *tny_exec_host_receive(int fd, int64_t deadline, tny_exec_cancel_fn cancel, void *ud);
bool tny_exec_host_disconnected(int fd);
/* The sole final result must be followed by EOF, never extra frames/bytes. */
int tny_exec_host_expect_eof(int fd, int64_t deadline, tny_exec_cancel_fn cancel, void *ud);
/* Retains direct-child ownership until cleanup; never signals a reaped PID. */
int tny_exec_host_close(tny_exec_host *host, bool completed);
#endif
