/* options.h — compose validated sdk.v1 AgentOptions and SendOptions protojson. */
#ifndef TNY_CURSOR_OPTIONS_H
#define TNY_CURSOR_OPTIONS_H

#include <stdbool.h>
#include <stddef.h>

struct tny_ctx;

/* Return malloc'd compact protojson objects. Configuration is copied first,
 * then tny-owned values are applied. api_key is required and always replaces
 * configured apiKey. custom_tools_json, when non-NULL, is a trusted raw object
 * map of name -> CustomToolDefinition and replaces local.customTools. */
char *cursor_options_agent_json(const struct tny_ctx *ctx, const char *api_key,
                                const char *custom_tools_json, char *err, size_t errlen);

/* interactive_stream forces enableDeltas=true. Non-interactive callers retain
 * the configured presence/value. All other configured SendOptions survive. */
char *cursor_options_send_json(const struct tny_ctx *ctx, bool interactive_stream, char *err,
                               size_t errlen);

#endif
