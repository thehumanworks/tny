/* config.h — process context: workspace, flags, settings, provider config. */
#ifndef TNY_CONFIG_H
#define TNY_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include "json/json.h"

#define TNY_TASK_DIGEST_HEX_LEN 40u

#define TNY_MCP_IMPORT_CODEX  (1u << 0)
#define TNY_MCP_IMPORT_CLAUDE (1u << 1)
#define TNY_MCP_IMPORT_GROK   (1u << 2)
#define TNY_MCP_IMPORT_CURSOR (1u << 3)

struct tny_extensions;
struct tny_host_services_state;
struct custom_tool_registry;
struct tny_cursor_config;

/* TNY_VERSION lives in build/generated/tny_version.h, written by make from
 * `git describe` (docs/adr/0014). The fallback keeps editors and static
 * analysis working without a build. */
#if defined(__has_include)
#if __has_include("tny_version.h")
#include "tny_version.h"
#endif
#endif
#ifndef TNY_VERSION
#define TNY_VERSION "0.0.0-dev"
#endif

typedef enum { TNY_MODE_ASK = 0, TNY_MODE_AUTO, TNY_MODE_YOLO } tny_perm_mode;
typedef enum {
    TNY_TOOLS_ALL = 0,
    TNY_TOOLS_TERMINAL_EDIT,
    TNY_TOOLS_TERMINAL,
} tny_tool_profile;

typedef struct tny_ctx {
    /* workspace */
    char *cwd;         /* absolute primary workspace */
    char ws_hash[17];  /* fnv1a hex of cwd */
    char **extra_dirs; /* --add-dir + saved workspace dirs */
    int n_extra_dirs;

    /* selection */
    int backend;         /* tny_backend_id, resolved */
    char *provider_name; /* effective named profile (OpenAI or acp@NAME) */
    char *model;
    bool model_from_flag; /* --model on the command line beats settings and
                           * saved models */
    tny_perm_mode perm_mode;
    tny_tool_profile tool_profile; /* native-loop built-ins advertised/accepted */
    bool json_out;
    bool no_save;
    bool no_color;     /* --no-color | --color=never: no SGR at all */
    bool force_color;  /* --color=always: SGR even piped, beats NO_COLOR */
    bool library_mode; /* deterministic embed: never write host stdio */
    struct tny_host_services_state *host_services; /* borrowed from lib runtime */
    struct custom_tool_registry *custom_tools;     /* borrowed from lib runtime */

    /* optional Python extensions (~/.tny/extensions). CLI contexts enable
     * discovery by default; explicit/libtny contexts keep it off unless a
     * later public ABI opts in. A zero continuation cap means unlimited. */
    bool extensions_enabled;
    int max_extension_iterations;
    int extension_timeout_ms;
    struct tny_extensions *extensions; /* process-owned persistent host */

    /* openai-compatible provider */
    char *base_url;
    char *api_key;            /* resolved secret; never persisted by tny */
    char *auth_header_name;   /* default Authorization */
    char *auth_header_prefix; /* default "Bearer " */
    char *max_tokens_field;   /* NULL = omit */
    char *wire_api;           /* "responses" (default) | "chat" (docs/adr/0016) */
    char *output_schema;      /* normalized response_format JSON, or NULL */
    char **extra_headers;     /* NULL-terminated extra request header lines set
                               * by builtin profiles (claude oauth beta, grok
                               * proxy auth — docs/adr/0019); never persisted */

    /* cursor */
    char *bridge_bin;
    struct tny_cursor_config *cursor_config; /* user-level sdk.v1 options */
    /* codex (docs/adr/0065, 0066): a ChatGPT token + account id handed in
     * by flag (--chatgpt-token / --chatgpt-account-id) — the file-less
     * credential source; env and the stores are read in codex_auth.c */
    char *chatgpt_token;
    char *chatgpt_account_id;
    bool no_host_registry; /* background ask child: never publish a spawned
                            * host as an attach target (docs/adr/0031) */
    /* remote tool runtime (core/ssh.c, docs/adr/0022): when ssh_host is set
     * every workspace tool runs on that host over one ControlMaster */
    char *ssh_host;    /* user@host or [v6], NULL = local tools */
    char ssh_port[6];  /* "" = ssh default */
    char *ssh_cwd;     /* remote working dir (absolute after connect) */
    char *ssh_control; /* "ControlPath=…" option string */
    /* Speed tier for providers with TNY_CAP_FAST: NULL = provider default,
     * "fast"/"priority" = the paid fast tier, "default" = standard. Each
     * backend maps this to its own wire field. */
    char *service_tier;
    bool service_tier_explicit;      /* --fast / TUI /fast was used; settings
                                      * defaults must not replace it */
    bool service_tier_from_settings; /* recompute on provider switches */

    /* user system prompt (--system-prompt, all providers). The openai
     * backend carries it on its native system/instructions field; host
     * backends with no such schema field (cursor, acp) get it
     * prepended to the session's first user message instead (runtime.c). */
    char *system_prompt;

    /* Runtime-owned task preset selection. Bodies are resolved lazily for CLI
     * contexts and are never ambiently discovered by deterministic embedders. */
    char *task_name;
    char *task_source;
    char *task_instructions;
    /* SHA-1 hex digest of the private task snapshot (integrity marker, not a
     * credential).  The complete body is persisted alongside metadata. */
    char task_digest[TNY_TASK_DIGEST_HEX_LEN + 1];
    bool task_explicit;

    /* reasoning effort (all providers). Canonical levels are
     * TNY_EFFORT_LEVELS; other tokens are provider-advertised values passed
     * through verbatim. NULL = provider default (field omitted on the wire). */
    char *reasoning_effort;
    bool effort_explicit;      /* --effort / TUI /effort was used (any value,
                                * "default" included): settings defaults must
                                * never override an explicit choice */
    bool effort_from_settings; /* current value came from settings.json, so
                                * switching provider recomputes it instead
                                * of leaking one provider's default */
    /* acp */
    char **agent_argv;       /* NULL-terminated, or NULL */
    bool agent_from_profile; /* agent_argv belongs to settings acp.NAME */

    /* repo limits (.tny.json — never authority, only limits) */
    int max_steps;                /* 0 = unlimited (default); a cap comes
                                   * from --max-steps, /max-steps, or the
                                   * repo's .tny.json "steps" */
    size_t max_tool_result_bytes; /* default 32768 */
    bool context_enabled;         /* AGENTS.md loading */
    char *instructions_snapshot;  /* cached request/event snapshot */
    char **instruction_paths;
    int n_instruction_paths;
    char instructions_digest[17];
    bool instructions_snapshot_ready;
    bool mcp_disabled;            /* `tny acp` server: client owns MCP */
    unsigned mcp_import_mask;     /* explicit settings mcp.import_from opt-ins */
    unsigned mcp_import_order[4]; /* source bits, in user-authored order */
    int n_mcp_import_sources;
    bool mcp_import_warned; /* source diagnostics are emitted once per context */
    char *sandbox_mode;     /* "none" | "auto" | "os" */

    /* paths */
    char *tny_dir;       /* ~/.tny */
    char *settings_path; /* ~/.tny/settings.json */

    /* parsed docs kept for rule lookups (may be NULL) */
    yyjson_doc *settings;
    yyjson_doc *repo_cfg;
} tny_ctx;

/* Load settings + env + repo config for the given --cwd (NULL = getcwd).
 * Cheap: two small file reads, no network, no backend spawn. */
tny_ctx *tny_ctx_load(const char *cwd_flag);
/* Deterministic embedding context: no settings, repo config, or environment
 * provider/authority loading. Caller supplies an existing workspace and an
 * explicit state directory. Defaults to ask mode and the OpenAI backend. */
tny_ctx *tny_ctx_new_explicit(const char *cwd, const char *state_dir);
void tny_ctx_free(tny_ctx *ctx);

/* Resolve backend per docs/cli.md when no --backend flag was given. */
int tny_resolve_backend(tny_ctx *ctx, const char *flag_value);

/* ---- builtin subscription profiles (profiles.c, docs/adr/0019, 0065) ----
 * "codex" (ChatGPT subscription against chatgpt.com/backend-api/codex),
 * "claude" (Anthropic OpenAI-compat + Claude Code OAuth token) and "grok"
 * (xAI CLI session token / XAI_API_KEY) run on the openai backend like
 * user-named profiles, but ship with tny. A settings.json object or
 * NAME_BASE_URL env var with the same name shadows the builtin. */
bool tny_builtin_profile_exists(const char *name);
/* ---- codex_auth.c: ChatGPT credentials (docs/adr/0065, 0066) ----
 * Precedence: --chatgpt-token flag > CHATGPT_ACCESS_TOKEN env >
 * ~/.tny/codex-auth.json (tny's own login, native refresh) >
 * $CODEX_HOME/auth.json (the Codex CLI's login, refreshed in place). */
typedef enum {
    TNY_CODEX_CRED_NONE = 0,
    TNY_CODEX_CRED_FLAG,
    TNY_CODEX_CRED_ENV,
    TNY_CODEX_CRED_TNY_STORE,
    TNY_CODEX_CRED_CODEX_CLI
} tny_codex_cred_source;
typedef struct {
    char *access_token; /* ChatGPT OAuth bearer */
    char *account_id;   /* explicit, or the JWT `chatgpt_account_id` claim */
    char *api_key;      /* Codex CLI auth.json OPENAI_API_KEY (API-key mode) */
    tny_codex_cred_source source;
} tny_codex_creds;
char *tny_codex_home(void);       /* $CODEX_HOME or ~/.codex, malloc'd */
char *tny_codex_auth_path(void);  /* Codex CLI …/auth.json, malloc'd */
char *tny_codex_store_path(void); /* ~/.tny/codex-auth.json, malloc'd */
/* Any env or file credential source present (the flag is on ctx). */
bool tny_codex_auth_present(void);
/* 0 when a credential resolved; ctx may be NULL (no flag source). */
int tny_codex_credentials(const tny_ctx *ctx, tny_codex_creds *out);
void tny_codex_creds_free(tny_codex_creds *c);
const char *tny_codex_cred_source_name(tny_codex_cred_source s);
/* Refresh-token grant (auth.openai.com/oauth/token) on the store that will
 * be read — tny's first, else the Codex CLI's — when its access token is
 * expired/near expiry or last_refresh is stale; rewritten in place. */
void tny_codex_refresh_if_stale(void);
/* Write a fresh OAuth token response into ~/.tny/codex-auth.json (0600). */
int tny_codex_store_save(yyjson_val *token_response);
/* POST a body and slurp the JSON reply: HTTP status, or -1 with err. */
int tny_codex_http_post(const char *url, const char *content_type, const char *body, buf_t *out,
                        char *err, size_t errlen);
/* codex_login.c: native ChatGPT sign-in — browser PKCE flow with the
 * localhost callback, or the device-code flow (docs/adr/0066). Exit code. */
int tny_codex_login(tny_ctx *ctx, bool device);
int tny_codex_logout(void);         /* delete ~/.tny/codex-auth.json */
bool tny_claude_auth_present(void); /* subscription login artifacts only */
bool tny_grok_auth_present(void);   /* ~/.grok/auth.json session */
/* Resolved Claude credential: CLAUDE_CODE_OAUTH_TOKEN, then
 * ANTHROPIC_API_KEY, then ~/.claude/.credentials.json accessToken.
 * malloc'd; *source (optional) names where it came from. */
char *tny_claude_token(const char **source);
/* Session token from ~/.grok/auth.json (tny's own login or the grok
 * CLI's). malloc'd. */
char *tny_grok_session_token(void);
/* Native xAI sign-in (grok_login.c, docs/adr/0021): RFC 8628 device-code
 * login and logout against auth.x.ai — no grok CLI needed — plus the
 * refresh-token exchange run before each token read. */
int tny_grok_login(void);
int tny_grok_logout(void);
void tny_grok_refresh_if_stale(void);
void tny_apply_builtin_profile(tny_ctx *ctx, const char *name);
/* True when the codex profile is bound to the ChatGPT backend (OAuth
 * token + the responses beta header), as opposed to API-key mode on
 * api.openai.com. Decides the `/models` dialect below. */
bool tny_codex_chatgpt_mode(const tny_ctx *ctx);
/* Codex CLI version tny claims on `GET /models?client_version=` —
 * TNY_CODEX_CLIENT_VERSION or the pinned default. */
const char *tny_codex_client_version(void);
/* Normalize the ChatGPT backend's `{"models":[{slug,display_name,
 * visibility,supported_reasoning_levels,…}]}` into the catalog shape
 * `tny models` prints: [{"id","name","description","efforts":[…],
 * "default_effort","context_window"},…]. Entries with visibility other
 * than "list" are dropped. malloc'd; NULL on a malformed body. */
char *tny_codex_models_normalize(const char *body, size_t len);
/* Post-model-resolution fixups (grok proxy model-override header). */
void tny_finish_builtin_profile(tny_ctx *ctx);
void tny_ctx_clear_extra_headers(tny_ctx *ctx);
void tny_ctx_add_extra_header(tny_ctx *ctx, const char *line);

/* Effective provider name: a settings profile name ("openrouter" or
 * "acp@claude") when active, else the builtin backend name. Never NULL after
 * tny_resolve_backend. */
const char *tny_provider_name(const tny_ctx *ctx);
const char *tny_tool_profile_name(tny_tool_profile profile);
bool tny_tool_profile_is_shell(const tny_ctx *ctx);
/* Force an unsupported runtime surface back to `all`, emitting one status
 * line only when an opt-in profile was actually requested. */
void tny_tool_profile_ignore(tny_ctx *ctx, const char *surface);
/* True when `name` is a user-named OpenAI-compatible provider: a top-level
 * settings.json object with a base_url, or NAME_BASE_URL set in the
 * environment. Builtin names (openai|cursor|acp) are never custom. */
bool tny_custom_provider_exists(tny_ctx *ctx, const char *name);
/* True when provider is an `acp@NAME` (or legacy `acp:NAME`) selector whose
 * NAME is present under settings.json acp. The profile is validated when selected, so an
 * unused malformed entry never breaks startup. */
bool tny_acp_profile_exists(tny_ctx *ctx, const char *provider);
/* malloc'd env-var name holding the profile's API key: its api_key_env,
 * or NAME_API_KEY derived from the profile name. NULL if no such profile. */
char *tny_custom_provider_key_env(tny_ctx *ctx, const char *name);
/* malloc'd NAME+suffix env-var name: ("xai","_BASE_URL") -> "XAI_BASE_URL"
 * (uppercased, non-alphanumerics -> '_'). */
char *tny_provider_env_var(const char *name, const char *suffix);
/* Provider names defined by NAME_BASE_URL env vars (lowercased, builtins
 * excluded). malloc'd NULL-terminated array; caller frees entries + array.
 * Lazy in-memory environ walk — never runs on startup paths. */
char **tny_env_provider_names(int *count);
/* malloc'd "a|b|c" of every provider name usable with --provider / /provider:
 * builtins first, then settings.json profiles, then env-only ones (deduped).
 * Drives help text; walks environ, so not for startup paths. */
char *tny_provider_names_joined(tny_ctx *ctx);

/* Persist the provider (and its model) that just ran, so the next launch
 * defaults to them: settings last_provider + models.{provider}. */
int tny_settings_remember_use(tny_ctx *ctx);
/* Saved model for one provider (settings "models" object), or NULL. */
const char *tny_settings_provider_model(tny_ctx *ctx, const char *provider);

/* Persist a top-level string into ~/.tny/settings.json (e.g. last backend,
 * model). Creates the file if missing. Returns 0 on success. */
int tny_settings_set_str(tny_ctx *ctx, const char *key, const char *value);

/* `provider setup` fields; NULL members keep the profile's current value
 * (docs/adr/0018). */
typedef struct {
    const char *base_url;
    const char *api_key; /* stored in settings.json; env still wins */
    const char *api_key_env;
    const char *model;
    const char *wire_api; /* "responses" | "chat" */
} tny_provider_fields;

int tny_provider_write_profile(tny_ctx *ctx, const char *name, const tny_provider_fields *f,
                               char *errbuf, size_t errlen);
const char *tny_settings_get_str(tny_ctx *ctx, const char *key);

/* Workspace extra-dir persistence (settings "workspaces" map). */
int tny_workspace_add(tny_ctx *ctx, const char *dir);
int tny_workspace_remove(tny_ctx *ctx, const char *dir);
int tny_workspace_clear(tny_ctx *ctx);

const char *tny_perm_mode_name(tny_perm_mode m);
/* True when this process runs inside another tny turn's `terminal` tool
 * (TNY_NESTED=1); *parent then holds the mode that turn resolved
 * (docs/adr/0063). */
bool tny_nested_perm_mode(tny_perm_mode *parent);
/* False when `requested` is wider than the nested parent's mode; err then
 * holds a one-line explanation. Always true outside a nested run. */
bool tny_perm_mode_allowed_nested(tny_perm_mode requested, char *err, size_t errlen);

/* Resolve SGR output for one session (docs/adr/0026). *color: SGR color
 * sequences; *attr: non-color SGR (bold/dim/reverse/reset). Precedence:
 * --color=never/--no-color (no SGR at all) > --color=always/CLICOLOR_FORCE
 * (SGR even when piped; CLICOLOR_FORCE must be non-empty and not "0") >
 * NO_COLOR (any value, even empty — colors off, attributes stay: they are
 * structural, not color) > tty default (both on). */
void tny_color_resolve(const tny_ctx *ctx, bool tty, bool *color, bool *attr);

/* Parse a --max-steps / /max-steps value: a positive integer caps the
 * native loop, "unlimited"|"none"|"0" clear the cap (0 = unlimited).
 * Returns the parsed value, or -1 when the token is not usable. */
int tny_parse_max_steps(const char *s);

/* Canonical reasoning-effort levels shared across providers (docs/adr/0009). */
#define TNY_EFFORT_LEVELS "off|light|medium|high|xhigh|max"
/* True when v is one of the canonical levels. */
bool tny_effort_canonical(const char *v);
/* Map a canonical level onto one provider's wire vocabulary (backend is a
 * tny_backend_id). Non-canonical tokens come back verbatim so users can pick
 * anything the provider's catalog advertises. Never returns NULL for a
 * non-NULL input. */
const char *tny_effort_wire(int backend, const char *v);

/* True when the tier string names the paid fast tier. OpenAI renamed
 * "priority" processing to "fast" mode; both spellings select it. */
bool tny_tier_is_fast(const char *tier);

/* True when ctx->wire_api selects the legacy Chat Completions wire. The
 * default (NULL or anything else) is the Responses API (docs/adr/0016). */
bool tny_wire_is_chat(const char *wire_api);

#endif
