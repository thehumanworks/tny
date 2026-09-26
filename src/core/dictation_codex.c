/* ChatGPT WAV transcription, pinned to Codex rust-v0.105.0 voice.rs. */
#include "core/dictation_provider.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool available(const tny_ctx *ctx, char *err, size_t len) {
    tny_codex_creds c;
    tny_codex_credentials(ctx, &c);
    bool ok = c.access_token && *c.access_token && c.account_id && *c.account_id &&
              !strpbrk(c.access_token, "\r\n") && !strpbrk(c.account_id, "\r\n");
    tny_codex_creds_free(&c);
    snprintf(err, len, "%s",
             ok ? "" : "dictation requires a ChatGPT login: tny --provider codex login");
    return ok;
}

static void *start(const tny_ctx *ctx, const buf_t *wav, char *err, size_t len) {
    if (!(ctx && ctx->chatgpt_token && *ctx->chatgpt_token)) tny_codex_refresh_if_stale();
    if (!available(ctx, err, len)) return NULL;
    tny_codex_creds creds;
    tny_codex_credentials(ctx, &creds);
    buf_t url = {0};
    /* Only the trusted Codex gateway override can redirect this bearer. */
    const char *base = tny_codex_service_base_url(ctx);
    size_t n = strlen(base);
    while (n && base[n - 1] == '/') n--;
    if (n >= 6 && !memcmp(base + n - 6, "/codex", 6)) n -= 6;
    buf_append(&url, base, n);
    buf_appends(&url, "/transcribe");
    void *job = NULL;
    if (!url.oom && creds.access_token && creds.account_id &&
        !strpbrk(creds.access_token, "\r\n") && !strpbrk(creds.account_id, "\r\n"))
        job =
            tny_dictation_http_start(url.data, creds.access_token, creds.account_id, wav, err, len);
    buf_free(&url);
    tny_codex_creds_free(&creds);
    return job;
}

/* Responses on the same ChatGPT credential and trusted gateway as STT, with
 * structured output through text.format (ADR 0175). */
static bool normalize_target(const tny_ctx *ctx, const char *model, tny_norm_target *t, char *err,
                             size_t len) {
    (void)model; /* the Responses wire carries the model in the body */
    if (!available(ctx, err, len)) return false;
    tny_codex_creds creds;
    tny_codex_credentials(ctx, &creds);
    buf_t url = {0}, auth = {0}, account = {0};
    const char *base = tny_codex_service_base_url(ctx);
    size_t n = strlen(base);
    while (n && base[n - 1] == '/') n--;
    buf_append(&url, base, n);
    buf_appends(&url, "/responses");
    buf_appendf(&auth, "Authorization: Bearer %s", creds.access_token);
    buf_appendf(&account, "chatgpt-account-id: %s", creds.account_id);
    tny_codex_creds_free(&creds);
    bool ok = !url.oom && !auth.oom && !account.oom;
    if (ok) {
        t->url = buf_detach(&url);
        t->headers[0] = buf_detach(&auth);
        t->headers[1] = buf_detach(&account);
        t->headers[2] = xstrdup("OpenAI-Beta: responses=v1");
        t->headers[3] = xstrdup("originator: tny");
        ok = t->url && t->headers[0] && t->headers[1] && t->headers[2] && t->headers[3];
        t->schema = t->tier = true;
    }
    if (auth.data) secure_zero(auth.data, auth.len);
    buf_free(&url);
    buf_free(&auth);
    buf_free(&account);
    if (!ok) {
        tny_norm_target_free(t);
        snprintf(err, len, "out of memory");
    }
    return ok;
}

const tny_dictation_provider tny_dictation_codex = {"codex",
                                                    available,
                                                    start,
                                                    tny_dictation_http_fd,
                                                    tny_dictation_http_step,
                                                    tny_dictation_http_destroy,
                                                    TNY_NORMALIZE_MODEL_CODEX,
                                                    normalize_target};
