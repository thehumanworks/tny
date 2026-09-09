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
    const char *base = getenv("TNY_CODEX_BASE_URL");
    if (!base || !*base) base = "https://chatgpt.com/backend-api/codex";
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

const tny_dictation_provider tny_dictation_codex = {"codex",
                                                    available,
                                                    start,
                                                    tny_dictation_http_fd,
                                                    tny_dictation_http_step,
                                                    tny_dictation_http_destroy};
