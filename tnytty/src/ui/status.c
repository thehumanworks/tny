#include "ui/status.h"

#include <stdio.h>
#include <string.h>

void tt_status_clear(tt_status *s) {
    s->text[0] = '\0';
    s->expires = 0;
}

void tt_status_set(tt_status *s, const char *msg, double now) {
    snprintf(s->text, sizeof s->text, "%s", msg ? msg : "");
    s->expires = s->text[0] ? now + TT_STATUS_TTL : 0;
}

size_t tt_status_utf8_len(const char *utf8) {
    size_t n = 0;
    if (!utf8) return 0;
    for (const unsigned char *p = (const unsigned char *)utf8; *p; p++)
        if ((*p & 0xc0) != 0x80) n++;
    return n;
}

void tt_status_copied(tt_status *s, const char *utf8, double now) {
    size_t n = tt_status_utf8_len(utf8);
    if (!n) return;
    char msg[TT_STATUS_MAX];
    snprintf(msg, sizeof msg, "Copied %zu character%s", n, n == 1 ? "" : "s");
    tt_status_set(s, msg, now);
}

bool tt_status_tick(tt_status *s, double now) {
    if (!s->text[0]) return false;
    if (now < s->expires) return false;
    tt_status_clear(s);
    return true;
}

const char *tt_status_text(const tt_status *s) { return s->text; }
