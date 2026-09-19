#ifndef TNY_SWARM_H
#define TNY_SWARM_H
#include "core/config.h"
#include "core/session.h"
/* Strict positive decimal, bounded by the existing 16-slot admission service. */
int tny_swarm_count(const char *text);
int tny_swarm_option(int argc, char **argv, int *index);
bool tny_swarm_supported(const tny_ctx *ctx);
int tny_swarm_bind(tny_session_state *session);
int tny_swarm_restore(tny_session_state *session, char *err, size_t cap);
void tny_swarm_policy(const tny_ctx *ctx, buf_t *out);
#endif
