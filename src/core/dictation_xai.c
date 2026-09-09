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
 * permission to fall through to a different account. Unset api_key_env falls
 * back to stored api_key, as in apply_custom_provider. */
static char *credential(const tny_ctx *ctx, bool refresh, char *err, size_t len) {
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
        const char *base = jget_str(profile, "base_url");
        if (base && *base) {
            const char *env = jget_str(profile, "api_key_env");
            if (env && *env) key = getenv(env);
            if (!key) {
                yyjson_val *stored = jget(profile, "api_key");
                key = yyjson_get_str(stored);
                /* JSON strings can contain NUL; never silently truncate a key. */
                if (key && strlen(key) != yyjson_get_len(stored)) key = "";
            }
        }
    }
    bool login = !key;
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
    char *key = credential(ctx, false, err, len);
    bool ok = key != NULL;
    secure_free(key);
    return ok;
}

static void *start(const tny_ctx *ctx, const buf_t *wav, char *err, size_t len) {
    char *key = credential(ctx, true, err, len);
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

const tny_dictation_provider tny_dictation_xai = {"xai",
                                                  available,
                                                  start,
                                                  tny_dictation_http_fd,
                                                  tny_dictation_http_step,
                                                  tny_dictation_http_destroy};
