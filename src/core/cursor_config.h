/* cursor_config.h — validated user configuration for Cursor sdk.v1. */
#ifndef TNY_CURSOR_CONFIG_H
#define TNY_CURSOR_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include "json/json.h"

typedef enum {
    TNY_CURSOR_RUNTIME_AUTO = 0,
    TNY_CURSOR_RUNTIME_LOCAL,
    TNY_CURSOR_RUNTIME_CLOUD,
} tny_cursor_runtime;

typedef struct tny_cursor_config {
    tny_cursor_runtime runtime;
    char *state_root;         /* --state-root, NULL = bridge default */
    char *local_store_json;   /* --local-store protojson object, NULL = default */
    bool tool_callbacks;      /* start/register SdkCustomToolCallbackService */
    bool store_callbacks;     /* start SdkStoreCallbackService before bridge */
    char *agent_options_json; /* validated AgentOptions protojson object */
    char *send_options_json;  /* validated SendOptions protojson object */
} tny_cursor_config;

/* Load settings.cursor. Repository configuration is never an authority source;
 * repo_cursor is inspected only to reject accidentally persisted credentials.
 * Returns a default config when settings.cursor is absent. */
tny_cursor_config *tny_cursor_config_load(yyjson_val *settings_cursor, yyjson_val *repo_cursor,
                                          char *err, size_t errlen);
void tny_cursor_config_free(tny_cursor_config *cfg);

#endif
