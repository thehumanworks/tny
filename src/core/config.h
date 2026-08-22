/* config.h — process context: workspace, flags, settings, provider config. */
#ifndef TNY_CONFIG_H
#define TNY_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include "json/json.h"

#define TNY_VERSION "0.1.0"

typedef enum { TNY_MODE_ASK = 0, TNY_MODE_AUTO, TNY_MODE_YOLO } tny_perm_mode;

typedef struct tny_ctx {
    /* workspace */
    char  *cwd;             /* absolute primary workspace */
    char   ws_hash[17];     /* fnv1a hex of cwd */
    char **extra_dirs;      /* --add-dir + saved workspace dirs */
    int    n_extra_dirs;

    /* selection */
    int    backend;         /* tny_backend_id, resolved */
    char  *model;
    bool   model_from_flag; /* --model on the command line beats saved models */
    tny_perm_mode perm_mode;
    bool   json_out;
    bool   no_save;
    bool   no_color;

    /* openai-compatible provider */
    char *base_url;
    char *api_key;          /* resolved secret; never persisted by tny */
    char *auth_header_name; /* default Authorization */
    char *auth_header_prefix; /* default "Bearer " */
    char *max_tokens_field; /* NULL = omit */

    /* cursor */
    char *bridge_bin;
    /* codex */
    char *codex_ws;
    char *codex_bin;
    char *ws_token_file;
    char *service_tier;     /* codex thread/start serviceTier: priority|default */

    /* reasoning effort (all providers). Canonical levels are
     * TNY_EFFORT_LEVELS; other tokens are provider-advertised values passed
     * through verbatim. NULL = provider default (field omitted on the wire). */
    char *reasoning_effort;
    /* acp */
    char **agent_argv;      /* NULL-terminated, or NULL */

    /* repo limits (.tny.json — never authority, only limits) */
    int    max_steps;              /* default 24 */
    size_t max_tool_result_bytes;  /* default 32768 */
    bool   context_enabled;        /* AGENTS.md loading */
    bool   mcp_disabled;           /* `tny acp` server: client owns MCP */
    char  *sandbox_mode;           /* "none" | "auto" | "os" */

    /* paths */
    char *tny_dir;          /* ~/.tny */
    char *settings_path;    /* ~/.tny/settings.json */

    /* parsed docs kept for rule lookups (may be NULL) */
    yyjson_doc *settings;
    yyjson_doc *repo_cfg;
} tny_ctx;

/* Load settings + env + repo config for the given --cwd (NULL = getcwd).
 * Cheap: two small file reads, no network, no backend spawn. */
tny_ctx *tny_ctx_load(const char *cwd_flag);
void     tny_ctx_free(tny_ctx *ctx);

/* Resolve backend per docs/cli.md when no --backend flag was given. */
int tny_resolve_backend(tny_ctx *ctx, const char *flag_value);
bool tny_codex_auth_present(void); /* codex login (auth.json) on this machine */

/* Persist the provider (and its model) that just ran, so the next launch
 * defaults to them: settings last_provider + models.{provider}. */
int tny_settings_remember_use(tny_ctx *ctx);
/* Saved model for one provider (settings "models" object), or NULL. */
const char *tny_settings_provider_model(tny_ctx *ctx, const char *provider);

/* Persist a top-level string into ~/.tny/settings.json (e.g. last backend,
 * model). Creates the file if missing. Returns 0 on success. */
int tny_settings_set_str(tny_ctx *ctx, const char *key, const char *value);
const char *tny_settings_get_str(tny_ctx *ctx, const char *key);

/* Workspace extra-dir persistence (settings "workspaces" map). */
int tny_workspace_add(tny_ctx *ctx, const char *dir);
int tny_workspace_remove(tny_ctx *ctx, const char *dir);
int tny_workspace_clear(tny_ctx *ctx);

const char *tny_perm_mode_name(tny_perm_mode m);

/* Canonical reasoning-effort levels shared across providers (docs/adr/0009). */
#define TNY_EFFORT_LEVELS "off|light|medium|high|xhigh|max"
/* True when v is one of the canonical levels. */
bool tny_effort_canonical(const char *v);
/* Map a canonical level onto one provider's wire vocabulary (backend is a
 * tny_backend_id). Non-canonical tokens come back verbatim so users can pick
 * anything the provider's catalog advertises. Never returns NULL for a
 * non-NULL input. */
const char *tny_effort_wire(int backend, const char *v);

#endif
