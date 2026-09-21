/* Optional ACP client MCP bridge. Tool authority stays in the owning runtime. */
#ifndef TNY_ACP_BRIDGE_H
#define TNY_ACP_BRIDGE_H
#include "backends/openai/openai.h"
#include "core/tools.h"

typedef struct tny_acp_bridge tny_acp_bridge;
tny_acp_bridge *tny_backend_acp_bridge(tny_backend *b);
tny_acp_bridge *tny_acp_bridge_new(tny_ctx *ctx, char *err, size_t len);
void tny_acp_bridge_bind(tny_acp_bridge *b, const tools_env *env, tny_openai_control_cb control,
                         void *ud);
const char *tny_acp_bridge_servers_json(const tny_acp_bridge *b);
void tny_acp_bridge_begin_turn(tny_acp_bridge *b, tny_backend_event_cb cb, void *ud);
int tny_acp_bridge_prepare_prompt(tny_acp_bridge *b, buf_t *context, char *err, size_t len);
int tny_acp_bridge_ack_context(tny_acp_bridge *b);
void tny_acp_bridge_end_turn(tny_acp_bridge *b);
/* OOM/teardown: release pending resources, no allocation, hooks, or protocol I/O. */
void tny_acp_bridge_abort(tny_acp_bridge *b);
const char *tny_acp_bridge_directory(const tny_acp_bridge *b);
void tny_acp_bridge_cancel(tny_acp_bridge *b);
bool tny_acp_bridge_stop_requested(const tny_acp_bridge *b);
bool tny_acp_bridge_permission_blocked(const tny_acp_bridge *b);
bool tny_acp_bridge_respond_permission(tny_acp_bridge *b, const char *id, tny_perm_decision d);
int tny_acp_bridge_pollfds(tny_acp_bridge *b, struct pollfd *fds, int max);
int tny_acp_bridge_dispatch(tny_acp_bridge *b);
void tny_acp_bridge_destroy(tny_acp_bridge *b);
/* Internal CLI stdio relay; does not load config, create a session or execute tools. */
int tny_acp_bridge_relay_main(const char *path);
#endif
