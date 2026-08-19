#include "core/backend.h"
#include "core/config.h"
#include <stddef.h>

tny_backend *tny_backend_create(tny_backend_id id, struct tny_ctx *ctx) {
    switch (id) {
    case TNY_BK_OPENAI: return tny_backend_openai_new(ctx);
    case TNY_BK_CURSOR: return tny_backend_cursor_new(ctx);
    case TNY_BK_CODEX:  return tny_backend_codex_new(ctx);
    case TNY_BK_ACP:    return tny_backend_acp_new(ctx);
    default: return NULL;
    }
}
