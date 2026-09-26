/* xAI REST STT, verified 2026-09-09; docs/sources.md and ADR 0079.
 * The official /v1/stt route selects its own model. Never use chat config. */
#include "core/dictation_provider.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool usable(const char *key) {
    return key && *key && strlen(key) <= 16384 && !strpbrk(key, "\r\n") &&
           str_ws_prefix(key, strlen(key)) != strlen(key);
}

/* Resolve only the user xai credential object. A standalone caller deliberately
 * has no loaded settings/context; do not invoke tny_ctx_load/profile resolution.
 * Presence wins: an explicitly supplied empty/invalid secret is an error, never
 * permission to fall through to a different account. A configured missing
 * api_key_env fails closed; Grok login is used only without an explicit source. */
static char *credential(const tny_ctx *ctx, bool refresh, bool *from_login, char *err, size_t len) {
    const char *key = ctx ? ctx->xai_api_key : NULL;
    yyjson_doc *loaded = NULL;
    if (!key) key = getenv("XAI_API_KEY");
    if (!key) {
        yyjson_doc *settings = ctx ? ctx->settings : NULL;
        if (!settings) {
            char *home = path_home();
            char *path = home ? path_join(home, ".tny/settings.json") : NULL;
            loaded = path ? jparse_file(path) : NULL;
            free(path);
            free(home);
            settings = loaded;
        }
        yyjson_val *profile = settings ? jget(yyjson_doc_get_root(settings), "xai") : NULL;
        if (profile) {
            const char *env = jget_str(profile, "api_key_env");
            if (env && *env) key = getenv(env);
            if (jget(profile, "api_key")) {
                yyjson_doc_free(loaded);
                snprintf(err, len,
                         "stored api_key was removed; export XAI_API_KEY or configure api_key_env");
                return NULL;
            }
            if (!key && env && *env) key = "";
        }
    }
    bool login = !key;
    if (from_login) *from_login = login;
    char *token = login ? tny_grok_session_token() : xstrdup(key);
    yyjson_doc_free(loaded);
    if (!usable(token)) goto invalid;
    if (login && refresh) {
        secure_free(token);
        tny_grok_refresh_if_stale();
        token = tny_grok_session_token();
        if (!usable(token)) goto invalid;
    }
    if (len) *err = 0;
    return token;
invalid:
    secure_free(token);
    snprintf(err, len,
             "xAI dictation needs a nonempty credential without CR/LF: --xai-api-key KEY, "
             "XAI_API_KEY, the xai settings profile, or tny --provider grok login");
    return NULL;
}

static bool available(const tny_ctx *ctx, char *err, size_t len) {
    char *key = credential(ctx, false, NULL, err, len);
    bool ok = key != NULL;
    secure_free(key);
    return ok;
}

static void *start(const tny_ctx *ctx, const buf_t *wav, char *err, size_t len) {
    char *key = credential(ctx, true, NULL, err, len);
    if (!key) return NULL;
    const char *url = "https://api.x.ai/v1/stt";
#ifdef TNY_DICTATION_FIXTURE
    /* Separate, never-installed test binary only. Production has no redirect
     * knob that could forward a Grok bearer to an arbitrary profile endpoint. */
    url = getenv("TNY_DICTATION_FIXTURE_URL");
    if (!url || !str_starts(url, "http://127.0.0.1:")) {
        secure_free(key);
        snprintf(err, len, "dictation fixture requires a loopback URL");
        return NULL;
    }
#endif
    void *job = tny_dictation_http_start(url, key, NULL, wav, err, len);
    secure_free(key);
    return job;
}

/* The source STT resolved: an API key uses the public chat API with a JSON
 * schema; the Grok login uses the streaming-only CLI chat proxy, which routes
 * on x-grok-model-override and gets JSON by instruction only (ADR 0175). */
static bool normalize_target(const tny_ctx *ctx, const char *model, tny_norm_target *t, char *err,
                             size_t len) {
    bool login = false;
    /* Transcription just refreshed a stale login; never refresh twice. */
    char *key = credential(ctx, false, &login, err, len);
    if (!key) return false;
    const char *base = login ? TNY_GROK_PROXY_BASE_URL : TNY_GROK_API_BASE_URL;
#ifdef TNY_DICTATION_FIXTURE
    base = getenv("TNY_DICTATION_FIXTURE_NORMALIZE_URL");
    if (!base || !str_starts(base, "http://127.0.0.1:")) {
        secure_free(key);
        snprintf(err, len, "dictation fixture requires a loopback normalizer URL");
        return false;
    }
#endif
    buf_t url = {0}, auth = {0}, version = {0}, route = {0};
    buf_appends(&url, base);
    buf_appends(&url, "/chat/completions");
    buf_appendf(&auth, "Authorization: Bearer %s", key);
    secure_free(key);
    const char *v = getenv("TNY_GROK_CLIENT_VERSION");
    buf_appendf(&version, "x-grok-client-version: %s", v && *v ? v : TNY_GROK_PROXY_VERSION);
    buf_appendf(&route, "x-grok-model-override: %s", model);
    bool ok = !url.oom && !auth.oom && !version.oom && !route.oom;
    if (ok) {
        t->url = buf_detach(&url);
        t->headers[0] = buf_detach(&auth);
        ok = t->url && t->headers[0];
        if (login) {
            t->headers[1] = xstrdup(TNY_GROK_PROXY_HEADER);
            t->headers[2] = buf_detach(&version);
            t->headers[3] = buf_detach(&route);
            ok = ok && t->headers[1] && t->headers[2] && t->headers[3];
        }
        t->chat = true;
        t->schema = !login;
    }
    if (auth.data) secure_zero(auth.data, auth.len);
    buf_free(&url);
    buf_free(&auth);
    buf_free(&version);
    buf_free(&route);
    if (!ok) {
        tny_norm_target_free(t);
        snprintf(err, len, "out of memory");
    }
    return ok;
}

const tny_dictation_provider tny_dictation_xai = {"xai",
                                                  available,
                                                  start,
                                                  tny_dictation_http_fd,
                                                  tny_dictation_http_step,
                                                  tny_dictation_http_destroy,
                                                  TNY_NORMALIZE_MODEL_XAI,
                                                  normalize_target};
