/* speech.c — bounded synthesis through the existing HTTP and Codex auth
 * layers. Provider adapters produce MP3 bytes; playback/export is shared. */
#include "core/speech.h"
#include "json/json.h"
#include "net/net.h"
#include "util/audio.h"
#include "util/tny_poll.h"
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool codex_available(const tny_ctx *ctx) {
    tny_codex_creds c;
    tny_codex_credentials(ctx, &c);
    bool ok = c.access_token && *c.access_token && c.account_id && *c.account_id &&
              !strpbrk(c.access_token, "\r\n") && !strpbrk(c.account_id, "\r\n");
    tny_codex_creds_free(&c);
    return ok;
}

static bool stopped(const tny_speech_request *r) {
    return r->cancelled && r->cancelled(r->userdata);
}

static int codex_synthesize(const tny_ctx *ctx, const tny_speech_request *r, buf_t *audio,
                            char *err, size_t errlen) {
    if (!(ctx && ctx->chatgpt_token && *ctx->chatgpt_token)) tny_codex_refresh_if_stale();
    tny_codex_creds creds;
    tny_codex_credentials(ctx, &creds);
    if (!creds.access_token || !creds.account_id) {
        tny_codex_creds_free(&creds);
        snprintf(err, errlen, "speech requires a ChatGPT login: tny --provider codex login");
        return 1;
    }
    /* Independent of ctx->base_url: another chat provider must never receive
     * the Codex bearer. The same trusted override supports Codex gateways. */
    const char *base = getenv("TNY_CODEX_BASE_URL");
    if (!base || !*base) base = "https://chatgpt.com/backend-api/codex";
    buf_t url, headers, body;
    buf_init(&url);
    buf_init(&headers);
    buf_init(&body);
    size_t n = strlen(base);
    while (n && base[n - 1] == '/') n--;
    if (n >= 6 && memcmp(base + n - 6, "/codex", 6) == 0) n -= 6;
    buf_append(&url, base, n);
    buf_appends(&url, "/pronunciation/synthesize?format=mp3");
    int rc = 1;
    http_conn *conn = NULL;
    if (strpbrk(creds.access_token, "\r\n") || strpbrk(creds.account_id, "\r\n")) goto done;
    buf_appendf(&headers, "Authorization: Bearer %s", creds.access_token);
    buf_appends(&body, "{\"text\":");
    jescape(&body, r->text);
    buf_appends(&body, ",\"voice\":");
    jescape(&body, r->voice ? r->voice : "cove");
    buf_appends(&body, ",\"pronunciation_language\":\"en-US\",\"speed\":1.0}");
    buf_t account;
    buf_init(&account);
    buf_appendf(&account, "chatgpt-account-id: %s", creds.account_id);
    static const char user_agent[] = "User-Agent: tny/" TNY_VERSION;
    const char *hdrs[] = {headers.data,
                          account.data,
                          "Content-Type: application/json",
                          "Accept: application/octet-stream",
                          "originator: tny",
                          user_agent,
                          NULL};
    if (!url.oom && !headers.oom && !body.oom && !account.oom)
        conn = http_open(url.data, err, errlen);
    int status = -1;
    if (stopped(r)) rc = 130;
    if (conn && rc != 130 &&
        http_request(conn, "POST", http_prefix(conn), hdrs, body.data, body.len) == 0) {
        int64_t deadline = monotonic_ms() + 60000;
        do {
            if (stopped(r)) {
                rc = 130;
                break;
            }
            status = http_read_response(conn, 100);
        } while (status == -2 && monotonic_ms() < deadline);
    }
    buf_free(&account);
    if (rc == 130) goto done;
    if (status != 200) {
        snprintf(err, errlen, "speech synthesis failed (HTTP %d)%s", status,
                 status == 401 || status == 403 ? "; run tny --provider codex login" : "");
        rc = status > 0 ? 2 : 1;
        goto done;
    }
    int64_t deadline = monotonic_ms() + 60000;
    for (;;) {
        if (stopped(r)) {
            rc = 130;
            goto done;
        }
        if (monotonic_ms() >= deadline) {
            snprintf(err, errlen, "speech response timed out");
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
            snprintf(err, errlen, "incomplete speech response");
            goto done;
        }
        if ((size_t)got > TNY_SPEECH_AUDIO_MAX - audio->len) {
            snprintf(err, errlen, "speech response exceeds 16 MiB");
            goto done;
        }
        buf_append(audio, chunk, (size_t)got);
        if (audio->oom) goto done;
    }
    const char *ct = http_header(conn, "Content-Type");
    if (ct && strstr(ct, "json")) {
        yyjson_doc *doc = jparse(audio->data, audio->len);
        const char *encoded = doc ? jget_str(yyjson_doc_get_root(doc), "base64") : NULL;
        size_t decoded = encoded ? b64_decode(encoded, (uint8_t *)audio->data, audio->cap) : 0;
        yyjson_doc_free(doc);
        audio->len = decoded;
    } else if (!ct || (!str_starts(ct, "audio/") && !str_starts(ct, "application/octet-stream"))) {
        snprintf(err, errlen, "unexpected speech content type");
        goto done;
    }
    /* Reject JSON errors, empty bodies and obvious non-audio without a decoder. */
    if (audio->len < 10 ||
        (memcmp(audio->data, "ID3", 3) != 0 && !((unsigned char)audio->data[0] == 0xff &&
                                                 ((unsigned char)audio->data[1] & 0xe0) == 0xe0))) {
        snprintf(err, errlen, "invalid MP3 speech response");
        goto done;
    }
    rc = 0;
done:
    http_close(conn);
    if (headers.data) secure_zero(headers.data, headers.len);
    buf_free(&headers);
    buf_free(&body);
    buf_free(&url);
    tny_codex_creds_free(&creds);
    if (rc == 130) snprintf(err, errlen, "speech interrupted");
    else if (rc && !*err) snprintf(err, errlen, "speech synthesis failed");
    return rc;
}

typedef struct {
    const char *name;
    bool (*available)(const tny_ctx *);
    int (*synthesize)(const tny_ctx *, const tny_speech_request *, buf_t *, char *, size_t);
} speech_provider;
static const speech_provider providers[] = {{"codex", codex_available, codex_synthesize}};
static const speech_provider *provider_find(const char *name) {
    if (!name) name = "codex";
    for (size_t i = 0; i < sizeof providers / sizeof providers[0]; i++)
        if (strcmp(name, providers[i].name) == 0) return &providers[i];
    return NULL;
}

bool tny_speech_available(const tny_ctx *ctx, const char *provider, bool playback, char *err,
                          size_t errlen) {
    const speech_provider *p = provider_find(provider);
    const char *reason =
        !p                   ? "unknown speech provider"
        : !p->available(ctx) ? "speech requires a ChatGPT login: tny --provider codex login"
        : playback && !audio_player_available() ? "speech playback unavailable; install afplay, "
                                                  "ffplay, mpv or mpg123, or use --output-file"
                                                : NULL;
    if (err && errlen) snprintf(err, errlen, "%s", reason ? reason : "");
    return !reason;
}

/* Export is a separate, explicit mode: a private sibling temp is renamed
 * only after a complete response/write. Existing output survives failure. */
static int speech_export(const tny_speech_request *r, const buf_t *audio) {
    buf_t tmp;
    buf_init(&tmp);
    buf_appendf(&tmp, "%s.XXXXXX", r->output_file);
    int fd = tmp.oom ? -1 : mkstemp(tmp.data);
    if (fd < 0) {
        buf_free(&tmp);
        return 1;
    }
    int rc = 0;
    size_t offset = 0;
    while (offset < audio->len) {
        if (stopped(r)) {
            rc = 130;
            break;
        }
        ssize_t n = write(fd, audio->data + offset, audio->len - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            rc = 1;
            break;
        }
        offset += (size_t)n;
    }
    if (close(fd) != 0 && !rc) rc = 1;
    if (!rc && stopped(r)) rc = 130;
    if (!rc && rename(tmp.data, r->output_file) != 0) rc = 1;
    if (rc) unlink(tmp.data);
    buf_free(&tmp);
    return rc;
}

int tny_speech_run(const tny_ctx *ctx, const tny_speech_request *r, char *err, size_t errlen) {
    if (errlen) *err = 0;
    size_t n = r->text ? strlen(r->text) : 0;
    if (!n || n > TNY_SPEECH_TEXT_MAX || str_ws_prefix(r->text, n) == n ||
        !utf8_valid_bytes(r->text, n) ||
        (r->voice &&
         (!*r->voice || strlen(r->voice) > 64 || !utf8_valid_bytes(r->voice, strlen(r->voice)))) ||
        (r->output_file && !*r->output_file)) {
        snprintf(err, errlen,
                 "speech needs nonempty UTF-8 text (at most 16 KiB) and valid options");
        return 1;
    }
    if (!tny_speech_available(ctx, r->provider, !r->output_file, err, errlen)) return 1;
    if (stopped(r)) {
        snprintf(err, errlen, "speech interrupted");
        return 130;
    }
    buf_t audio;
    buf_init(&audio);
    int rc = provider_find(r->provider)->synthesize(ctx, r, &audio, err, errlen);
    if (!rc && stopped(r)) {
        snprintf(err, errlen, "speech interrupted");
        rc = 130;
    }
    if (!rc) {
        if (r->output_file) {
            rc = speech_export(r, &audio);
            if (rc)
                snprintf(err, errlen,
                         rc == 130 ? "speech interrupted" : "cannot write speech output file");
        } else rc = audio_play(audio.data, audio.len, r->cancelled, r->userdata, err, errlen);
    }
    buf_free(&audio);
    return rc;
}
