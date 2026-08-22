#include "core/backend.h"
#include "core/config.h"
#include <stddef.h>

unsigned tny_backend_caps(tny_backend_id id) {
    switch (id) {
    case TNY_BK_OPENAI: return TNY_CAP_FAST; /* service_tier on the request */
    case TNY_BK_CURSOR: return TNY_CAP_FAST; /* model param {"id":"fast"} */
    case TNY_BK_CODEX:  return TNY_CAP_FAST; /* thread/start serviceTier */
    case TNY_BK_ACP:    return 0;            /* no tier field in session/new */
    default: return 0;
    }
}

tny_backend *tny_backend_create(tny_backend_id id, struct tny_ctx *ctx) {
    switch (id) {
    case TNY_BK_OPENAI: return tny_backend_openai_new(ctx);
    case TNY_BK_CURSOR: return tny_backend_cursor_new(ctx);
    case TNY_BK_CODEX:  return tny_backend_codex_new(ctx);
    case TNY_BK_ACP:    return tny_backend_acp_new(ctx);
    default: return NULL;
    }
}
