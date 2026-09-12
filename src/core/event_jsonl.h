/* event_jsonl.h — canonical public event lines for `tny ask --events=jsonl`
 * (docs/adr/0090). The names and numeric values are the public schema's
 * (docs/adr/0030, sdk/schema/events.json, include/tny/tny.h); the values are
 * the engine-owned envelope verbatim. Nothing here invents an event. */
#ifndef TNY_EVENT_JSONL_H
#define TNY_EVENT_JSONL_H

#include "core/runtime.h"
#include "util/util.h"

#include <stdbool.h>

/* Append one complete line (including its '\n') for one engine event. */
void tny_event_jsonl_append(buf_t *out, const tny_owned_event *ev);

typedef enum {
    TNY_EVENT_WRITE_OK = 0,
    TNY_EVENT_WRITE_IO = -1,       /* the consumer refused the bytes */
    TNY_EVENT_WRITE_CANCELLED = -2 /* interrupted while the consumer stalled */
} tny_event_write_rc;

/* Checked stdout seam. A chunk is only written once the poll seam reports
 * the descriptor writable, so a write cannot block for long, and every stall
 * re-checks the caller's signal flag: a full pipe — including one that was
 * already full before the first event — delays delivery, never cancellation.
 * The descriptor's file status flags are never modified, because stdout's
 * open file description is shared with whatever started tny. */
typedef struct {
    int fd;
    buf_t line;                    /* reused; bounds retained memory to one event */
    bool (*interrupted)(void *ud); /* signal-safe peek, never consumes the flag */
    void *ud;
    int last_errno;
} tny_event_writer;

void tny_event_writer_init(tny_event_writer *w, int fd, bool (*interrupted)(void *ud), void *ud);
tny_event_write_rc tny_event_writer_emit(tny_event_writer *w, const tny_owned_event *ev);
void tny_event_writer_free(tny_event_writer *w);

#endif
