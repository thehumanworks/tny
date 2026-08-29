/* pty.h — the platform seam (docs/platforms.md). One implementation per
 * platform behind this header; no platform #ifdefs elsewhere. */
#ifndef TNYTTY_PTY_H
#define TNYTTY_PTY_H

#include <stdbool.h>
#include <sys/types.h>

typedef struct {
    int master; /* -1 when not open */
    pid_t pid;  /* -1 when not running */
} tt_pty;

/* Spawn argv on a fresh pty of cols x rows. Returns 0, or -1 with errno
 * set (and a clean "not supported" error on platforms without ptys).
 * The master fd is left non-blocking. */
int tt_pty_spawn(tt_pty *p, char *const argv[], int cols, int rows);
int tt_pty_resize(tt_pty *p, int cols, int rows);
/* SIGHUP the child's process group (session teardown). */
void tt_pty_kill(tt_pty *p);
/* SIGKILL the child's process group after a bounded graceful shutdown. */
void tt_pty_force_kill(tt_pty *p);
/* Reap if exited; returns 1 (reaped, *exit_code set), 0 (still running),
 * -1 (no child). Never blocks unless block is true. */
int tt_pty_reap(tt_pty *p, int *exit_code, bool block);
void tt_pty_close(tt_pty *p);

#endif
