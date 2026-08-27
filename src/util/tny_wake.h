/* tny_wake.h — nonblocking self-pipe used to wake tny_poll.
 *
 * The pipe is process-local and carries no data: one byte means "re-check
 * owner-thread state".  Signal coalescing is intentional. */
#ifndef TNY_WAKE_H
#define TNY_WAKE_H

typedef struct {
    int read_fd;
    int write_fd;
} tny_wake;

int  tny_wake_init(tny_wake *wake);
void tny_wake_close(tny_wake *wake);
int  tny_wake_fd(const tny_wake *wake);
void tny_wake_signal(tny_wake *wake);
void tny_wake_drain(tny_wake *wake);

#endif
