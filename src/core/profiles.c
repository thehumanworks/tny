/* profiles.c — builtin subscription provider profiles (docs/adr/0019).
 *
 * "claude": Anthropic's OpenAI-compatible endpoint driven by a Claude Code
 * OAuth token (`claude setup-token` / `claude /login`) or ANTHROPIC_API_KEY.
 * OAuth tokens ride `Authorization: Bearer` and need the
 * `anthropic-beta: oauth-2025-04-20` request header.
 *
 * "grok": the xAI session token from ~/.grok/auth.json — minted by
 * `tny --provider grok login` (native device flow, grok_login.c) or by the
 * grok CLI — against the CLI chat proxy, falling back to XAI_API_KEY
 * against api.x.ai. The proxy validates the session token only when
 * `X-XAI-Token-Auth: xai-grok-cli` rides along, and routes models via the
 * `x-grok-model-override` header.
 *
 * Both run on the openai backend like user-named profiles; a settings.json
 * object or NAME_BASE_URL env var with the same name shadows the builtin
 * (tny_resolve_backend checks custom providers first). Credentials are read
 * at resolve time; the only thing tny ever persists is the refreshed grok
 * token, written back into the grok store it came from. */
#include "core/config.h"
#include "util/util.h"

#include <stdlib.h>
#include <string.h>

#define CLAUDE_BASE_URL      "https://api.anthropic.com/v1"
#define CLAUDE_OAUTH_HEADER  "anthropic-beta: oauth-2025-04-20"
#define CLAUDE_DEFAULT_MODEL "claude-sonnet-4-6"
#define GROK_PROXY_BASE_URL  "https://cli-chat-proxy.grok.com/v1"
#define GROK_PROXY_HEADER    "X-XAI-Token-Auth: xai-grok-cli"
/* The proxy version-gates on x-grok-client-version and 426s requests that
 * claim less than its rolling minimum. Pinned to a known-accepted grok-build
 * release; TNY_GROK_CLIENT_VERSION overrides without a rebuild. */
#define GROK_PROXY_VERSION   "0.1.202"
#define GROK_API_BASE_URL    "https://api.x.ai/v1"
/* Both modes: the proxy routes the model-override header, api.x.ai the
 * JSON body — same catalog, one default. */
#define GROK_DEFAULT_MODEL   "grok-4.6"

bool tny_builtin_profile_exists(const char *name) {
    return name && (strcmp(name, "claude") == 0 || strcmp(name, "grok") == 0);
}

/* ---------- extra request headers ---------- */

void tny_ctx_clear_extra_headers(tny_ctx *ctx) {
    if (!ctx->extra_headers) return;
    for (char **h = ctx->extra_headers; *h; h++) free(*h);
    free(ctx->extra_headers);
    ctx->extra_headers = NULL;
}

void tny_ctx_add_extra_header(tny_ctx *ctx, const char *line) {
    if (!line || !*line) return;
    int n = 0;
    if (ctx->extra_headers)
        while (ctx->extra_headers[n]) n++;
    char **v = realloc(ctx->extra_headers, sizeof(char *) * (size_t)(n + 2));
    if (!v) return;
    ctx->extra_headers = v;
    v[n] = xstrdup(line);
    v[n + 1] = NULL;
}

/* ---------- credential lookups ---------- */

static char *home_join(const char *rel) {
    char *home = path_home();
    if (!home) return NULL;
    char *p = path_join(home, rel);
    free(home);
    return p;
}

/* Where `claude /login` keeps its credentials on Linux/Windows (macOS uses
 * the Keychain; there the env var or `claude setup-token` is the path in). */
static char *claude_credentials_path(void) {
    const char *dir = getenv("CLAUDE_CONFIG_DIR");
    if (dir && *dir) return path_join(dir, ".credentials.json");
    return home_join(".claude/.credentials.json");
}

char *tny_claude_token(const char **source) {
    if (source) *source = NULL;
    const char *env = getenv("CLAUDE_CODE_OAUTH_TOKEN");
    if (env && *env) {
        if (source) *source = "CLAUDE_CODE_OAUTH_TOKEN";
        return xstrdup(env);
    }
    env = getenv("ANTHROPIC_API_KEY");
    if (env && *env) {
        if (source) *source = "ANTHROPIC_API_KEY";
        return xstrdup(env);
    }
    char *path = claude_credentials_path();
    if (!path) return NULL;
    yyjson_doc *doc = jparse_file(path);
    free(path);
    if (!doc) return NULL;
    const char *tok = jget_str(jget(yyjson_doc_get_root(doc), "claudeAiOauth"),
                               "accessToken");
    char *out = tok && *tok ? xstrdup(tok) : NULL;
    yyjson_doc_free(doc);
    if (out && source) *source = "~/.claude/.credentials.json";
    return out;
}

/* Auto-detection keys off subscription-login artifacts only: the OAuth env
 * var or the credentials file. A bare ANTHROPIC_API_KEY belongs to whatever
 * tool set it and must never hijack the default provider. */
bool tny_claude_auth_present(void) {
    const char *env = getenv("CLAUDE_CODE_OAUTH_TOKEN");
    if (env && *env) return true;
    char *path = claude_credentials_path();
    if (!path) return false;
    bool ok = file_exists(path);
    free(path);
    return ok;
}

/* A Claude Code OAuth token needs the oauth beta header; a Console API key
 * must not carry it. Tokens from the OAuth sources are OAuth by origin;
 * ANTHROPIC_API_KEY is trusted to hold whatever its prefix says. */
static bool claude_token_is_oauth(const char *tok, const char *source) {
    if (!tok) return false;
    if (str_starts(tok, "sk-ant-oat")) return true;
    return source && strcmp(source, "ANTHROPIC_API_KEY") != 0;
}

static char *grok_auth_path(void) {
    return home_join(".grok/auth.json");
}

char *tny_grok_session_token(void) {
    char *path = grok_auth_path();
    if (!path) return NULL;
    yyjson_doc *doc = jparse_file(path);
    free(path);
    if (!doc) return NULL;
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *tok = jget_str(jget(root, "https://accounts.x.ai/sign-in"), "key");
    if (!tok && yyjson_is_obj(root)) {
        /* OIDC / external-provider logins store the entry under the issuer
         * URL; take the first object value carrying a "key" string */
        size_t idx, max;
        yyjson_val *k, *v;
        yyjson_obj_foreach(root, idx, max, k, v) {
            (void)k;
            tok = jget_str(v, "key");
            if (tok) break;
        }
    }
    char *out = tok && *tok ? xstrdup(tok) : NULL;
    yyjson_doc_free(doc);
    return out;
}

bool tny_grok_auth_present(void) {
    char *path = grok_auth_path();
    if (!path) return false;
    bool ok = file_exists(path);
    free(path);
    return ok;
}

/* ---------- profile application ---------- */

static void set_str(char **slot, const char *v) {
    free(*slot);
    *slot = v ? xstrdup(v) : NULL;
}

static void profile_reset(tny_ctx *ctx, const char *name) {
    set_str(&ctx->provider_name, name);
    set_str(&ctx->auth_header_name, "Authorization");
    set_str(&ctx->auth_header_prefix, "Bearer ");
    set_str(&ctx->max_tokens_field, NULL);
    tny_ctx_clear_extra_headers(ctx);
}

/* Builtin default model: set before apply_provider_model so --model and a
 * saved models.{provider} entry still win, but the openai backend's
 * gpt-4.1-mini fallback never leaks onto a foreign provider. */
static void profile_default_model(tny_ctx *ctx, const char *model) {
    if (ctx->model_from_flag) return;
    set_str(&ctx->model, model);
}

static void apply_claude(tny_ctx *ctx) {
    profile_reset(ctx, "claude");
    set_str(&ctx->base_url, CLAUDE_BASE_URL);
    /* Anthropic's OpenAI-compat surface is /v1/chat/completions only */
    set_str(&ctx->wire_api, "chat");
    const char *source = NULL;
    char *tok = tny_claude_token(&source);
    set_str(&ctx->api_key, tok);
    if (claude_token_is_oauth(tok, source))
        tny_ctx_add_extra_header(ctx, CLAUDE_OAUTH_HEADER);
    if (tok) {
        memset(tok, 0, strlen(tok));
        free(tok);
    }
    profile_default_model(ctx, CLAUDE_DEFAULT_MODEL);
}

static void apply_grok(tny_ctx *ctx) {
    profile_reset(ctx, "grok");
    /* The grok CLI refreshes its OIDC tokens in the background; without it
     * tny must run the refresh grant itself before reading (grok_login.c).
     * Only fires when an entry is actually at/near expiry. */
    tny_grok_refresh_if_stale();
    char *session = tny_grok_session_token();
    if (session) {
        /* subscription path: the CLI chat proxy, session token as bearer */
        set_str(&ctx->base_url, GROK_PROXY_BASE_URL);
        set_str(&ctx->wire_api, "chat"); /* proxy models are streaming chat */
        set_str(&ctx->api_key, session);
        tny_ctx_add_extra_header(ctx, GROK_PROXY_HEADER);
        const char *ver = getenv("TNY_GROK_CLIENT_VERSION");
        buf_t vh;
        buf_init(&vh);
        buf_appendf(&vh, "x-grok-client-version: %s",
                    ver && *ver ? ver : GROK_PROXY_VERSION);
        tny_ctx_add_extra_header(ctx, vh.data);
        buf_free(&vh);
        memset(session, 0, strlen(session));
        free(session);
        profile_default_model(ctx, GROK_DEFAULT_MODEL);
        return;
    }
    /* API-key fallback: the public xAI API (Responses wire, tny default) */
    set_str(&ctx->base_url, GROK_API_BASE_URL);
    set_str(&ctx->wire_api, NULL);
    const char *key = getenv("XAI_API_KEY");
    set_str(&ctx->api_key, key && *key ? key : NULL);
    profile_default_model(ctx, GROK_DEFAULT_MODEL);
}

void tny_apply_builtin_profile(tny_ctx *ctx, const char *name) {
    if (strcmp(name, "claude") == 0) apply_claude(ctx);
    else if (strcmp(name, "grok") == 0) apply_grok(ctx);
}

/* Runs after the model resolved: the grok proxy routes on the
 * x-grok-model-override header, not the JSON body. */
void tny_finish_builtin_profile(tny_ctx *ctx) {
    if (!ctx->provider_name || strcmp(ctx->provider_name, "grok") != 0) return;
    if (!ctx->model || !*ctx->model) return;
    if (!strstr(ctx->base_url ? ctx->base_url : "", "cli-chat-proxy")) return;
    buf_t h;
    buf_init(&h);
    buf_appendf(&h, "x-grok-model-override: %s", ctx->model);
    tny_ctx_add_extra_header(ctx, h.data);
    buf_free(&h);
}
