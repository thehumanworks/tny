/* status.h — the window's one-line status bar (docs/adr/0005).
 *
 * Message text and expiry only: no clock of its own, no drawing. The
 * caller passes a monotonic "now" from the event loop, so the timing is
 * deterministic in tests and needs no timer thread. */
#ifndef TNYTTY_UI_STATUS_H
#define TNYTTY_UI_STATUS_H

#include <stdbool.h>
#include <stddef.h>

#define TT_STATUS_MAX 128
#define TT_STATUS_TTL 2.0 /* seconds a transient message stays up */

typedef struct {
    char text[TT_STATUS_MAX];
    double expires; /* monotonic seconds; 0 when nothing is shown */
} tt_status;

void tt_status_clear(tt_status *s);
void tt_status_set(tt_status *s, const char *msg, double now);
/* "Copied 1 character" / "Copied 42 characters", counting codepoints in
 * the UTF-8 text rather than bytes. An empty copy shows nothing. */
void tt_status_copied(tt_status *s, const char *utf8, double now);
/* Drop the message once it is past its deadline. Returns true when the
 * visible text changed, which is the caller's cue to repaint. */
bool tt_status_tick(tt_status *s, double now);
const char *tt_status_text(const tt_status *s);
/* Number of codepoints in a UTF-8 string (continuation bytes excluded). */
size_t tt_status_utf8_len(const char *utf8);

#endif
