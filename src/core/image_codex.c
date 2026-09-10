/* Codex Images API, transport pinned to rust-v0.154.0-alpha.3 (ADR 0074).
 * Model and quality defaults follow ADR 0084. */
#include "core/image_provider.h"
#include "core/image.h"
#include "json/json.h"
#include "net/net.h"
#include "util/tny_poll.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_WIRE_MAX   (((TNY_IMAGE_OUTPUT_MAX + 2u) / 3u) * 4u + 65536u)
#define IMAGE_TIMEOUT_MS 300000

static bool available(const tny_ctx *ctx, char *err, size_t len) {
    tny_codex_creds c;
    tny_codex_credentials(ctx, &c);
    bool ok = c.access_token && *c.access_token && c.account_id && *c.account_id &&
              !strpbrk(c.access_token, "\r\n") && !strpbrk(c.account_id, "\r\n");
    tny_codex_creds_free(&c);
    if (err && len)
        snprintf(err, len, "%s",
                 ok ? "" : "images require a ChatGPT login: tny --provider codex login");
    return ok;
}

static int render(const tny_ctx *ctx, const tny_image_request *r, const tny_image_input *inputs,
                  buf_t *image, char *err, size_t len) {
    if (!(ctx && ctx->chatgpt_token && *ctx->chatgpt_token)) tny_codex_refresh_if_stale();
    tny_codex_creds creds;
    tny_codex_credentials(ctx, &creds);
    buf_t url, auth, account, body, response;
    buf_init(&url);
    buf_init(&auth);
    buf_init(&account);
    buf_init(&body);
    buf_init(&response);
    http_conn *conn = NULL;
    int rc = 1;
    if (!creds.access_token || !creds.account_id || strpbrk(creds.access_token, "\r\n") ||
        strpbrk(creds.account_id, "\r\n")) {
        snprintf(err, len, "images require a ChatGPT login: tny --provider codex login");
        goto done;
    }
    /* Never inherit ctx->base_url/api_key from the conversation provider. */
    const char *base = tny_codex_service_base_url(ctx);
    size_t n = strlen(base);
    while (n && base[n - 1] == '/') n--;
    buf_append(&url, base, n);
    buf_appends(&url, r->edit ? "/images/edits" : "/images/generations");
    buf_appendf(&auth, "Authorization: Bearer %s", creds.access_token);
    buf_appendf(&account, "chatgpt-account-id: %s", creds.account_id);
    buf_appends(&body, "{\"model\":");
    jescape(&body, r->model);
    buf_appends(&body, ",\"prompt\":");
    jescape(&body, r->prompt);
    buf_appends(&body, ",\"background\":\"auto\",\"quality\":");
    jescape(&body, r->quality ? r->quality : "high");
    buf_appends(&body, ",\"size\":");
    jescape(&body, r->size ? r->size : "auto");
    if (r->edit) {
        buf_appends(&body, ",\"images\":[");
        for (size_t i = 0; i < r->image_count; i++) {
            if (i) buf_appends(&body, ",");
            buf_appends(&body, "{\"image_url\":\"");
            image_data_url((const uint8_t *)inputs[i].data.data, inputs[i].data.len, inputs[i].mime,
                           &body);
            buf_appends(&body, "\"}");
        }
        buf_appends(&body, "]");
    }
    buf_appends(&body, "}");
    if (url.oom || auth.oom || account.oom || body.oom) goto done;
    if (tny_image_stopped(r)) {
        rc = 130;
        goto done;
    }
    conn = http_open(url.data, err, len);
    static const char user_agent[] = "User-Agent: tny/" TNY_VERSION;
    const char *headers[] = {auth.data,
                             account.data,
                             "Content-Type: application/json",
                             "Accept: application/json",
                             "originator: tny",
                             user_agent,
                             NULL};
    if (!conn || http_request(conn, "POST", http_prefix(conn), headers, body.data, body.len))
        goto done;
    int status;
    int64_t deadline = monotonic_ms() + IMAGE_TIMEOUT_MS;
    do {
        if (tny_image_stopped(r)) {
            rc = 130;
            goto done;
        }
        status = http_read_response(conn, 100);
    } while (status == -2 && monotonic_ms() < deadline);
    if (status != 200) {
        snprintf(err, len, "image request failed (HTTP %d)%s", status,
                 status == 401 || status == 403 ? "; run tny --provider codex login" : "");
        rc = status > 0 ? 2 : 1;
        goto done;
    }
    const char *ct = http_header(conn, "Content-Type");
    if (!ct || !str_starts(ct, "application/json")) {
        snprintf(err, len, "unexpected image response content type");
        goto done;
    }
    for (;;) {
        if (tny_image_stopped(r)) {
            rc = 130;
            goto done;
        }
        if (monotonic_ms() >= deadline) {
            snprintf(err, len, "image response timed out");
            goto done;
        }
        char chunk[8192];
        ssize_t got = http_body_read(conn, chunk, sizeof chunk);
        if (!got) break;
        if (got == -2) {
            struct pollfd pf = {http_fd(conn), POLLIN, 0};
            tny_poll(&pf, 1, 100);
            continue;
        }
        if (got < 0) {
            snprintf(err, len, "incomplete image response");
            goto done;
        }
        if ((size_t)got > IMAGE_WIRE_MAX - response.len) {
            snprintf(err, len, "image response exceeds wire limit");
            goto done;
        }
        buf_append(&response, chunk, (size_t)got);
        if (response.oom) goto done;
    }
    yyjson_doc *doc = jparse(response.data, response.len);
    yyjson_val *data = doc ? jget(yyjson_doc_get_root(doc), "data") : NULL;
    size_t encoded_len = 0;
    const char *encoded = yyjson_arr_size(data) == 1
                              ? jget_strn(yyjson_arr_get_first(data), "b64_json", &encoded_len)
                              : NULL;
    rc = tny_image_decode(encoded, encoded_len, image, err, len);
    yyjson_doc_free(doc);
done:
    http_close(conn);
    if (auth.data) secure_zero(auth.data, auth.len);
    buf_free(&url);
    buf_free(&auth);
    buf_free(&account);
    buf_free(&body);
    buf_free(&response);
    tny_codex_creds_free(&creds);
    return rc;
}

const tny_image_provider tny_image_codex = {"codex", "gpt-image-2.5-sunburst",
                                            TNY_IMAGE_REFERENCES_MAX, available, render};
