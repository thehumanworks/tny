/* Native Codex ChatGPT and Grok subscription profiles (ADR 0152).
 * OAuth login and refresh use HTTP directly. Explicit settings/env profiles
 * of the same name shadow the builtin. BYOK uses environment keys only. */
#include "core/config.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODEX_CHATGPT_BASE_URL "https://chatgpt.com/backend-api/codex"
#define CODEX_BETA_HEADER      "OpenAI-Beta: responses=v1"
/* The ChatGPT backend requires `?client_version=` and uses it as a minimum
 * version filter. tny fetches the catalog live and is not a Codex CLI build,
 * so use a high discovery value instead of pinning model visibility to a
 * particular CLI release. TNY_CODEX_CLIENT_VERSION can override it. */
#define CODEX_CATALOG_CLIENT_VERSION "999.999.999"
#define CODEX_DEFAULT_MODEL          "gpt-5.6-sol"
#define GROK_PROXY_BASE_URL          TNY_GROK_PROXY_BASE_URL
#define GROK_PROXY_HEADER            TNY_GROK_PROXY_HEADER
#define GROK_PROXY_VERSION           TNY_GROK_PROXY_VERSION
#define GROK_API_BASE_URL            TNY_GROK_API_BASE_URL
/* Both modes: the proxy routes the model-override header, api.x.ai the
 * JSON body — same catalog, one default. */
#define GROK_DEFAULT_MODEL "grok-4.6"

bool tny_builtin_profile_exists(const char *name) {
    return name && (strcmp(name, "codex") == 0 || strcmp(name, "grok") == 0);
}

/* ---------- extra request headers ---------- */

void tny_ctx_clear_extra_headers(tny_ctx *ctx) {
    if (!ctx->extra_headers) return;
    for (char **h = ctx->extra_headers; *h; h++) secure_free(*h);
    free(ctx->extra_headers);
    ctx->extra_headers = NULL;
}

void tny_ctx_add_extra_header(tny_ctx *ctx, const char *line) {
    if (!line || !*line) return;
    int n = 0;
    if (ctx->extra_headers)
        while (ctx->extra_headers[n]) n++;
    char **v = realloc(ctx->extra_headers, sizeof(char *) * (size_t)(n + 2));
    if (!v) {
        ctx->provider_resolution_failed = true;
        return;
    }
    ctx->extra_headers = v;
    v[n] = xstrdup(line);
    if (!v[n]) ctx->provider_resolution_failed = true;
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

static char *grok_auth_path(void) { return home_join(".grok/auth.json"); }

char *tny_grok_session_token(void) {
    char *path = grok_auth_path();
    if (!path) return NULL;
    yyjson_doc *doc = jparse_file(path);
    free(path);
    if (!doc) return NULL;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *value = jget(jget(root, "https://accounts.x.ai/sign-in"), "key");
    const char *tok = yyjson_get_str(value);
    if (!tok && yyjson_is_obj(root)) {
        /* OIDC / external-provider logins store the entry under the issuer
         * URL; take the first object value carrying a "key" string */
        size_t idx, max;
        yyjson_val *k, *v;
        yyjson_obj_foreach(root, idx, max, k, v) {
            (void)k;
            value = jget(v, "key");
            tok = yyjson_get_str(value);
            if (tok) break;
        }
    }
    /* Do not turn a JSON token with embedded NUL into a different credential. */
    char *out = tok && *tok && strlen(tok) == yyjson_get_len(value) ? xstrdup(tok) : NULL;
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

static void set_str(tny_ctx *ctx, char **slot, const char *v) {
    secure_free(*slot);
    *slot = v ? xstrdup(v) : NULL;
    if (v && !*slot) ctx->provider_resolution_failed = true;
}

static void profile_reset(tny_ctx *ctx, const char *name) {
    set_str(ctx, &ctx->provider_name, name);
    set_str(ctx, &ctx->auth_header_name, "Authorization");
    set_str(ctx, &ctx->auth_header_prefix, "Bearer ");
    set_str(ctx, &ctx->max_tokens_field, NULL);
    tny_ctx_clear_extra_headers(ctx);
}

/* Builtin default model: set before apply_provider_model so --model and a
 * saved models.{provider} entry still win, but the openai backend's
 * gpt-4.1-mini fallback never leaks onto a foreign provider. */
static void profile_default_model(tny_ctx *ctx, const char *model) {
    if (ctx->model_from_flag) return;
    set_str(ctx, &ctx->model, model);
}

static int apply_codex(tny_ctx *ctx) {
    profile_reset(ctx, "codex");
    set_str(ctx, &ctx->wire_api, NULL); /* the ChatGPT backend is Responses-only */
    /* Without the Codex CLI's background refresher, tny runs the
     * refresh-token grant itself before reading (codex_auth.c); the file
     * that wins the precedence order is the one refreshed. */
    tny_codex_refresh_if_stale();
    tny_codex_creds c;
    int credential_rc = tny_codex_credentials(ctx, &c);
    if (credential_rc == -3) return -1;
    if (credential_rc == -2) {
        fputs("tny: stored OPENAI_API_KEY was removed; export OPENAI_API_KEY and select "
              "--provider openai, or use tny --provider codex login\n",
              stderr);
        return -1;
    }
    if (c.access_token) {
        /* TNY_CODEX_BASE_URL: test mocks / gateways; a plain CODEX_BASE_URL
         * would instead shadow the whole builtin as a user profile */
        const char *bu = getenv("TNY_CODEX_BASE_URL");
        set_str(ctx, &ctx->base_url, bu && *bu ? bu : CODEX_CHATGPT_BASE_URL);
        set_str(ctx, &ctx->api_key, c.access_token);
        if (c.account_id) {
            buf_t h;
            buf_init(&h);
            buf_appendf(&h, "chatgpt-account-id: %s", c.account_id);
            tny_ctx_add_extra_header(ctx, h.data);
            if (buf_oom(&h)) ctx->provider_resolution_failed = true;
            buf_free(&h);
        }
        tny_ctx_add_extra_header(ctx, CODEX_BETA_HEADER);
    } else {
        /* No login: connect() explains the fix. */
        set_str(ctx, &ctx->base_url, CODEX_CHATGPT_BASE_URL);
        set_str(ctx, &ctx->api_key, NULL);
    }
    tny_codex_creds_free(&c);
    profile_default_model(ctx, CODEX_DEFAULT_MODEL);
    return 0;
}

/* Fixture redirect: a numeric loopback authority only, with a strict port.
 * Never allow userinfo, DNS aliases, URL controls or arbitrary bearer hosts. */
static bool grok_override_valid(const char *url) {
    if (!url || !*url) return true;
    const char *p;
    if (strncmp(url, "http://127.0.0.1", 16) == 0) p = url + 16;
    else if (strncmp(url, "https://127.0.0.1", 17) == 0) p = url + 17;
    else return false;
    if (*p == ':') {
        p++;
        unsigned port = 0;
        if (*p < '0' || *p > '9') return false;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (unsigned)(*p++ - '0');
            if (port > 65535) return false;
        }
        if (!port) return false;
    }
    if (*p && *p != '/') return false;
    for (; *p; p++)
        if ((unsigned char)*p <= 32 || *p == 127 || *p == '\\' || *p == '#') return false;
    return true;
}

static int apply_grok(tny_ctx *ctx) {
    if (!grok_override_valid(getenv("TNY_GROK_BASE_URL"))) {
        fputs("tny: TNY_GROK_BASE_URL must be an HTTP(S) numeric loopback URL (127.0.0.1)\n",
              stderr);
        return -1;
    }
    profile_reset(ctx, "grok");
    /* The grok CLI refreshes its OIDC tokens in the background; without it
     * tny must run the refresh grant itself before reading (grok_login.c).
     * Only fires when an entry is actually at/near expiry. */
    tny_grok_refresh_if_stale();
    char *session = tny_grok_session_token();
    if (session) {
        /* subscription path: the CLI chat proxy, session token as bearer */
        const char *override = getenv("TNY_GROK_BASE_URL");
        set_str(ctx, &ctx->base_url, override && *override ? override : GROK_PROXY_BASE_URL);
        set_str(ctx, &ctx->wire_api, "chat"); /* proxy models are streaming chat */
        set_str(ctx, &ctx->api_key, session);
        tny_ctx_add_extra_header(ctx, GROK_PROXY_HEADER);
        const char *ver = getenv("TNY_GROK_CLIENT_VERSION");
        buf_t vh;
        buf_init(&vh);
        buf_appendf(&vh, "x-grok-client-version: %s", ver && *ver ? ver : GROK_PROXY_VERSION);
        tny_ctx_add_extra_header(ctx, vh.data);
        if (buf_oom(&vh)) ctx->provider_resolution_failed = true;
        buf_free(&vh);
        memset(session, 0, strlen(session));
        free(session);
        profile_default_model(ctx, GROK_DEFAULT_MODEL);
        return 0;
    }
    /* API-key fallback: the public xAI API (Responses wire, tny default) */
    const char *override = getenv("TNY_GROK_BASE_URL");
    set_str(ctx, &ctx->base_url, override && *override ? override : GROK_API_BASE_URL);
    set_str(ctx, &ctx->wire_api, NULL);
    const char *key = getenv("XAI_API_KEY");
    set_str(ctx, &ctx->api_key, key && *key ? key : NULL);
    profile_default_model(ctx, GROK_DEFAULT_MODEL);
    return 0;
}

/* ---------- ChatGPT-mode model catalog (docs/backends/codex.md) ---------- */

bool tny_codex_chatgpt_mode(const tny_ctx *ctx) {
    if (!ctx || !ctx->provider_name || strcmp(ctx->provider_name, "codex") != 0) return false;
    for (char **h = ctx->extra_headers; h && *h; h++)
        if (strcmp(*h, CODEX_BETA_HEADER) == 0) return true;
    return false; /* A custom profile named codex has ordinary HTTP catalog rules. */
}

const char *tny_codex_client_version(void) {
    const char *v = getenv("TNY_CODEX_CLIENT_VERSION");
    return v && *v ? v : CODEX_CATALOG_CLIENT_VERSION;
}

char *tny_codex_models_normalize(const char *body, size_t len) {
    yyjson_doc *doc = jparse(body, len);
    if (!doc) return NULL;
    yyjson_val *models = jget(yyjson_doc_get_root(doc), "models");
    if (!models || !yyjson_is_arr(models)) {
        yyjson_doc_free(doc);
        return NULL;
    }
    buf_t out;
    buf_init(&out);
    buf_appends(&out, "[");
    int n = 0;
    size_t idx, max;
    yyjson_val *m;
    yyjson_arr_foreach(models, idx, max, m) {
        const char *slug = jget_str(m, "slug");
        if (!slug || !*slug) continue;
        const char *vis = jget_str(m, "visibility");
        if (vis && strcmp(vis, "list") != 0) continue; /* hide | none */
        if (n++) buf_appends(&out, ",");
        buf_appends(&out, "{\"id\":");
        jescape(&out, slug);
        const char *name = jget_str(m, "display_name");
        if (name && *name) {
            buf_appends(&out, ",\"name\":");
            jescape(&out, name);
        }
        const char *desc = jget_str(m, "description");
        if (desc && *desc) {
            buf_appends(&out, ",\"description\":");
            jescape(&out, desc);
        }
        yyjson_val *levels = jget(m, "supported_reasoning_levels");
        if (levels && yyjson_is_arr(levels) && yyjson_arr_size(levels)) {
            buf_appends(&out, ",\"efforts\":[");
            size_t li, lmax;
            yyjson_val *lv;
            int ln = 0;
            yyjson_arr_foreach(levels, li, lmax, lv) {
                const char *effort = jget_str(lv, "effort");
                if (!effort) continue;
                if (ln++) buf_appends(&out, ",");
                jescape(&out, effort);
            }
            buf_appends(&out, "]");
        }
        const char *dflt = jget_str(m, "default_reasoning_level");
        if (dflt && *dflt) {
            buf_appends(&out, ",\"default_effort\":");
            jescape(&out, dflt);
        }
        int64_t ctxw = jget_int(m, "context_window", 0);
        if (ctxw > 0) buf_appendf(&out, ",\"context_window\":%lld", (long long)ctxw);
        buf_appends(&out, "}");
    }
    buf_appends(&out, "]");
    yyjson_doc_free(doc);
    return out.data;
}

int tny_apply_builtin_profile(tny_ctx *ctx, const char *name) {
    if (strcmp(name, "codex") == 0) return apply_codex(ctx);
    else if (strcmp(name, "grok") == 0) return apply_grok(ctx);
    return 0;
}

/* Runs after the model resolved: the grok proxy routes on the
 * x-grok-model-override header, not the JSON body. */
void tny_finish_builtin_profile(tny_ctx *ctx) {
    if (!ctx->provider_name || strcmp(ctx->provider_name, "grok") != 0) return;
    if (!ctx->model || !*ctx->model) return;
    bool proxy = false;
    for (char **p = ctx->extra_headers; p && *p; p++)
        if (strcmp(*p, GROK_PROXY_HEADER) == 0) proxy = true;
    if (!proxy) return;
    buf_t h;
    buf_init(&h);
    buf_appendf(&h, "x-grok-model-override: %s", ctx->model);
    tny_ctx_add_extra_header(ctx, h.data);
    if (buf_oom(&h)) ctx->provider_resolution_failed = true;
    buf_free(&h);
}
