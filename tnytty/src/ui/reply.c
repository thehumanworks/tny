#include "ui/reply.h"

static void reply_cb(void *user, const char *bytes, size_t len) {
    tt_reply *r = user;
    if (!r->sink || !len) return;
    r->bytes += len;
    r->sink(r->user, bytes, len);
}

void tt_reply_attach(vt *t, tt_reply *r, tt_reply_sink sink, void *user) {
    r->sink = sink;
    r->user = user;
    r->bytes = 0;
    vt_set_respond(t, reply_cb, r);
}
