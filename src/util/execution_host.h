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
/* A second descriptor serviced while a channel operation waits: when it is
 * readable or hung up, drain(ud) runs; drain sets fd to -1 once it is done.
 * Code cells use it for their captured stdout/stderr pipe, so a child that
 * writes output never blocks behind the parent's frame wait. */
typedef struct {
    int fd;
    void (*drain)(void *ud);
    void *ud;
} tny_exec_aux;
int tny_exec_host_start(tny_exec_host *host);
/* Same channel for a `--code-cell` child: the parent's environment minus
 * tny's reserved process-scope fields, stdin on /dev/null, and stdout and
 * stderr both on output_fd. */
int tny_exec_host_start_cell(tny_exec_host *host, int output_fd);
/* Server validates an inherited connected AF_UNIX socket on fd3, marks CLOEXEC. */
int tny_exec_host_accept(void);
int tny_exec_host_send(int fd, const char *json, int64_t deadline, tny_exec_cancel_fn cancel,
                       void *ud);
/* NULL: EOF/invalid length/partial frame/deadline/cancel. errno distinguishes. */
char *tny_exec_host_receive(int fd, int64_t deadline, tny_exec_cancel_fn cancel, void *ud);
/* The same operations, also servicing aux (NULL behaves as above). */
int tny_exec_host_send_aux(int fd, const char *json, tny_exec_aux *aux, int64_t deadline,
                           tny_exec_cancel_fn cancel, void *ud);
char *tny_exec_host_receive_aux(int fd, tny_exec_aux *aux, int64_t deadline,
                                tny_exec_cancel_fn cancel, void *ud);
int tny_exec_host_expect_eof_aux(int fd, tny_exec_aux *aux, int64_t deadline,
                                 tny_exec_cancel_fn cancel, void *ud);
bool tny_exec_host_disconnected(int fd);
/* The sole final result must be followed by EOF, never extra frames/bytes. */
int tny_exec_host_expect_eof(int fd, int64_t deadline, tny_exec_cancel_fn cancel, void *ud);
/* Retains direct-child ownership until cleanup; never signals a reaped PID. */
int tny_exec_host_close(tny_exec_host *host, bool completed);
/* Immediate generation-safe stop of an unreaped child, without the cooperative
 * EOF grace period close() allows. 0 when strict absence was established. */
int tny_exec_host_kill(tny_exec_host *host);
#endif
