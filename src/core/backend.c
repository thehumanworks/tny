#include "core/backend.h"
#include "core/config.h"
#include <stddef.h>

unsigned tny_backend_caps(tny_backend_id id) {
    switch (id) {
    case TNY_BK_OPENAI: return TNY_CAP_FAST; /* service_tier on the request */
    default: return 0;
    }
}

tny_backend *tny_backend_create(tny_backend_id id, struct tny_ctx *ctx) {
    switch (id) {
    case TNY_BK_OPENAI: return tny_backend_openai_new(ctx);
    default: return NULL;
    }
}
