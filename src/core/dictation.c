#include "core/dictation_provider.h"
#include "util/audio_capture.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct tny_dictation {
    const tny_ctx *ctx;
    const tny_dictation_provider *provider;
    tny_dictation_request request;
    tny_dictation_state state;
    audio_capture *capture;
    void *job;
    buf_t audio, text;
    int64_t deadline;
    int rc;
    char error[256];
    /* Normalization (ADR 0175): every transition goes through tny_norm_step,
     * the C translation of the proven Lean lifecycle. */
    tny_norm_state norm;
    tny_norm_config config;
    char config_error[256];
    tny_norm_target target;
    tny_norm_job *norm_job;
    tny_dictionary dictionary;
    tny_norm_proposal accepted;
    buf_t raw, corrections;
    bool target_ready, effort_sent, tier_sent;
    int64_t norm_deadline;
    char skipped[64];
};

static const tny_dictation_provider *const providers[] = {&tny_dictation_codex, &tny_dictation_xai};
static const tny_dictation_provider *provider_find(const char *name) {
    if (!name || !*name) name = getenv("TNY_STT_PROVIDER");
    if (!name || !*name) name = "codex";
    for (size_t i = 0; i < sizeof providers / sizeof providers[0]; i++)
        if (strcmp(name, providers[i]->name) == 0) return providers[i];
    return NULL;
}

bool tny_dictation_available(const tny_ctx *ctx, const char *name, bool microphone, char *err,
                             size_t len) {
    const tny_dictation_provider *p = provider_find(name);
    if (!p) {
        snprintf(err, len, "unknown dictation provider");
        return false;
    }
    if (!p->available(ctx, err, len)) return false;
    if (microphone && !audio_capture_available()) {
        snprintf(err, len,
                 "microphone capture unavailable; install ffmpeg (or arecord on Linux), or use "
                 "--input-file");
        return false;
    }
    return true;
}

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static unsigned le16(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static void put32(char *p, uint32_t x) {
    for (int i = 0; i < 4; i++) {
        p[i] = (char)(x & 255u);
        x >>= 8;
    }
}

bool tny_dictation_wav_valid(const void *data, size_t n) {
    if (!data || n < 44 || n > TNY_DICTATION_AUDIO_MAX) return false;
    const unsigned char *p = data;
    if (memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0 || le32(p + 4) != n - 8)
        return false;
    bool fmt = false, samples = false;
    uint32_t rate = 0, stride = 0;
    size_t at = 12;
    while (at < n) {
        if (n - at < 8) return false;
        size_t size = le32(p + at + 4);
        const unsigned char *chunk = p + at + 8;
        if (size > n - at - 8) return false;
        if (!memcmp(p + at, "fmt ", 4)) {
            if (fmt || size < 16 || le16(chunk) != 1 || le16(chunk + 14) != 16) return false;
            unsigned channels = le16(chunk + 2);
            rate = le32(chunk + 4);
            stride = channels * 2u;
            if (channels < 1 || channels > 2 || rate < 8000 || rate > 96000 ||
                le16(chunk + 12) != stride || le32(chunk + 8) != rate * stride)
                return false;
            fmt = true;
        } else if (!memcmp(p + at, "data", 4)) {
            if (!fmt || samples || size % stride || size < rate * stride ||
                size > (uint64_t)rate * stride * TNY_DICTATION_SECONDS_MAX)
                return false;
            samples = true;
        }
        at += 8 + size + (size & 1u);
    }
    return at == n && fmt && samples;
}

bool tny_dictation_text_valid(const void *data, size_t n) {
    if (!n || n > TNY_DICTATION_TEXT_MAX || !utf8_valid_bytes(data, n)) return false;
    const unsigned char *p = data;
    if (str_ws_prefix(data, n) == n) return false;
    for (size_t i = 0; i < n; i++)
        if ((p[i] < 0x20 && p[i] != '\n' && p[i] != '\r' && p[i] != '\t') || p[i] == 0x7f ||
            (p[i] == 0xc2 && i + 1 < n && p[i + 1] >= 0x80 && p[i + 1] <= 0x9f))
            return false;
    return true;
}

static void private_free(buf_t *b) {
    if (b->data) secure_zero(b->data, b->len);
    buf_free(b);
}

static void normalizer_release(tny_dictation *d) {
    tny_norm_job_free(d->norm_job);
    d->norm_job = NULL;
    tny_norm_target_free(&d->target);
    d->target_ready = false;
    tny_dictionary_free(&d->dictionary);
}

static void complete(tny_dictation *d, int rc) {
    audio_capture_free(d->capture);
    d->capture = NULL;
    if (d->job) d->provider->destroy(d->job);
    d->job = NULL;
    normalizer_release(d);
    private_free(&d->audio);
    if (rc) private_free(&d->text);
    d->state = TNY_DICTATION_DONE;
    d->rc = rc;
    if (rc && !*d->error) snprintf(d->error, sizeof d->error, "dictation failed");
}

/* Deliver the raw transcript or the accepted rewrite; see Lean deliver. */
static void norm_settle(tny_dictation *d, const char *reason) {
    if (d->norm.phase == TNY_NORM_NORMALIZING) return;
    if (d->norm.outcome == TNY_NORM_NORMALIZED) {
        secure_zero(d->text.data, d->text.len); /* d->raw keeps the transcript */
        buf_clear(&d->text);
        buf_append(&d->text, d->accepted.text, d->accepted.text_len);
        buf_appends(&d->corrections, "[");
        for (size_t i = 0; i < d->accepted.n; i++) {
            const tny_norm_correction *c = &d->accepted.corrections[i];
            buf_appends(&d->corrections, i ? ",{\"span\":" : "{\"span\":");
            jescape(&d->corrections, c->span);
            buf_appends(&d->corrections, ",\"replacement\":");
            jescape(&d->corrections, c->replacement);
            buf_appendf(&d->corrections, ",\"reason\":\"%s\"}", tny_norm_reason_name(c->reason));
        }
        buf_appends(&d->corrections, "]");
        if (d->text.oom || d->corrections.oom) {
            /* Never lose the transcript to an allocation failure. */
            buf_clear(&d->text);
            buf_append(&d->text, d->raw.data, d->raw.len);
            d->norm.outcome = TNY_NORM_RAW;
            reason = "out_of_memory";
        }
    }
    if (d->norm.outcome == TNY_NORM_RAW && d->norm.requests)
        snprintf(d->skipped, sizeof d->skipped, "%s", reason ? reason : "not_normalized");
    tny_norm_proposal_free(&d->accepted);
    complete(d, d->text.oom ? 1 : 0);
}

static void norm_event(tny_dictation *d, tny_norm_event ev, const char *reason) {
    d->norm = tny_norm_step(d->norm, ev);
    norm_settle(d, reason);
}

static void norm_start_request(tny_dictation *d) {
    char reason[64] = "";
    d->effort_sent = d->norm.effort && *d->config.effort;
    d->tier_sent = d->config.fast && d->target.tier;
    d->norm_job = tny_norm_job_start(&d->target, &d->config, d->effort_sent, &d->dictionary,
                                     d->raw.data, d->norm_deadline, reason, sizeof reason);
    if (!d->norm_job) norm_event(d, TNY_NORM_EV_FAILED, *reason ? reason : "transport");
}

/* First NORMALIZING step: configuration, dictionary and endpoint are
 * resolved here so a failure keeps the raw transcript. */
static void norm_begin(tny_dictation *d) {
    char err[256] = "";
    d->norm_deadline = monotonic_ms() + (int64_t)d->config.timeout_seconds * 1000;
    if (!d->config.valid) {
        norm_event(d, TNY_NORM_EV_FAILED, "invalid_config");
        return;
    }
    if (!tny_dictionary_load(d->ctx ? d->ctx->cwd : NULL, &d->dictionary, err, sizeof err)) {
        snprintf(d->config_error, sizeof d->config_error, "%s", err);
        norm_event(d, TNY_NORM_EV_FAILED, "dictionary_invalid");
        return;
    }
    if (!d->provider->normalize_target ||
        !d->provider->normalize_target(d->ctx, d->config.model, &d->target, err, sizeof err)) {
        norm_event(d, TNY_NORM_EV_FAILED, "no_credential");
        return;
    }
    d->target_ready = true;
    norm_start_request(d);
}

static void norm_step(tny_dictation *d) {
    if (!d->target_ready) {
        norm_begin(d);
        return;
    }
    buf_t out = {0};
    char reason[64] = "";
    int rc = tny_norm_job_step(d->norm_job, &out, reason, sizeof reason);
    if (rc < 0) return;
    tny_norm_job_free(d->norm_job);
    d->norm_job = NULL;
    if (rc == 3) {
        d->norm = tny_norm_step(d->norm, TNY_NORM_EV_EFFORT_REJECTED);
        if (d->norm.phase == TNY_NORM_NORMALIZING) norm_start_request(d);
        else norm_settle(d, "effort_rejected");
    } else if (rc) {
        norm_event(d, TNY_NORM_EV_FAILED, reason);
    } else if (!tny_norm_proposal_parse(out.data, out.len, &d->accepted)) {
        norm_event(d, TNY_NORM_EV_FAILED, "malformed_output");
    } else {
        tny_norm_verdict v = tny_norm_verify(&d->dictionary, d->raw.data, d->raw.len, &d->accepted);
        char rejected[64];
        snprintf(rejected, sizeof rejected, "rejected:%s", tny_norm_verdict_name(v));
        norm_event(d, v == TNY_NORM_VERDICT_OK ? TNY_NORM_EV_ACCEPTED : TNY_NORM_EV_REJECTED,
                   rejected);
    }
    if (out.data) secure_zero(out.data, out.len);
    buf_free(&out);
}

static bool load_wav(const char *path, buf_t *b) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 44 &&
              (uint64_t)st.st_size <= TNY_DICTATION_AUDIO_MAX;
    while (ok) {
        char chunk[8192];
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || (n > 0 && (size_t)n > TNY_DICTATION_AUDIO_MAX - b->len)) {
            ok = false;
            break;
        }
        if (!n) break;
        buf_append(b, chunk, (size_t)n);
        if (b->oom) ok = false;
    }
    close(fd);
    return ok && tny_dictation_wav_valid(b->data, b->len);
}

tny_dictation *tny_dictation_start(const tny_ctx *ctx, const tny_dictation_request *r, char *err,
                                   size_t len) {
    if (len) *err = 0;
    if (!r || r->seconds < 0 || r->seconds > TNY_DICTATION_SECONDS_MAX ||
        (r->input_file && (!*r->input_file || r->seconds || r->device))) {
        snprintf(err, len, "invalid dictation options");
        return NULL;
    }
    tny_dictation *d = calloc(1, sizeof *d);
    if (!d) {
        snprintf(err, len, "out of memory");
        return NULL;
    }
    d->ctx = ctx;
    d->request = *r;
    d->provider = provider_find(r->provider);
    d->rc = -1;
    if (d->provider) {
        tny_norm_config_resolve(ctx, d->provider->name, r->normalize, &d->config, d->config_error,
                                sizeof d->config_error);
        if (d->config.enabled && !*d->config.model)
            snprintf(d->config.model, sizeof d->config.model, "%s", d->provider->normalize_model);
    }
    d->norm = tny_norm_init(d->config.enabled);
    if (r->input_file && !load_wav(r->input_file, &d->audio)) {
        snprintf(err, len,
                 "input must be a complete PCM16 WAV: 1–300 seconds, mono/stereo, 8–96 kHz, at "
                 "most 25 MiB");
        goto failed;
    }
    if (!tny_dictation_available(ctx, r->provider, !r->input_file, err, len)) goto failed;
    if (r->cancelled && r->cancelled(r->userdata)) {
        snprintf(err, len, "dictation interrupted");
        goto failed;
    }
    d->state = r->input_file ? TNY_DICTATION_TRANSCRIBING : TNY_DICTATION_RECORDING;
    if (!r->input_file) {
        const char header[44] = {0};
        buf_append(&d->audio, header, sizeof header);
        if (d->audio.oom) {
            snprintf(err, len, "out of memory");
            goto failed;
        }
        d->capture = audio_capture_start(r->device, err, len);
        if (!d->capture) goto failed;
        /* Leave bounded startup headroom; requested duration counts captured
         * samples, not time spent opening the microphone. */
        d->deadline = monotonic_ms() +
                      (int64_t)(r->seconds ? r->seconds : TNY_DICTATION_SECONDS_MAX) * 1000 + 10000;
    }
    return d;
failed:
    tny_dictation_free(d);
    return NULL;
}

tny_dictation_state tny_dictation_get_state(const tny_dictation *d) { return d->state; }
const char *tny_dictation_provider_name(const tny_dictation *d) { return d->provider->name; }
int tny_dictation_fd(const tny_dictation *d) {
    if (d->capture) return audio_capture_fd(d->capture);
    if (d->norm_job) return tny_norm_job_fd(d->norm_job);
    return d->job ? d->provider->fd(d->job) : -1;
}
void tny_dictation_finish(tny_dictation *d) {
    if (!d || d->state != TNY_DICTATION_RECORDING) return;
    audio_capture_stop(d->capture);
}
void tny_dictation_cancel(tny_dictation *d) {
    if (!d || d->state == TNY_DICTATION_DONE) return;
    if (d->state == TNY_DICTATION_NORMALIZING) {
        /* The transcript already exists: stop the rewrite and keep it raw. */
        norm_event(d, TNY_NORM_EV_CANCEL, "cancelled");
        return;
    }
    d->norm = tny_norm_step(d->norm, TNY_NORM_EV_CANCEL);
    snprintf(d->error, sizeof d->error, "dictation interrupted");
    complete(d, 130);
}

void tny_dictation_step(tny_dictation *d) {
    if (!d || d->state == TNY_DICTATION_DONE) return;
    if (d->request.cancelled && d->request.cancelled(d->request.userdata)) {
        tny_dictation_cancel(d);
        return;
    }
    if (d->state == TNY_DICTATION_RECORDING) {
        if (monotonic_ms() >= d->deadline) tny_dictation_finish(d);
        int rc = audio_capture_read(d->capture, &d->audio, TNY_DICTATION_AUDIO_MAX, d->error,
                                    sizeof d->error);
        if (rc < 0) {
            complete(d, 1);
            return;
        }
        size_t target = 44u + AUDIO_CAPTURE_RATE * 2u *
                                  (unsigned)(d->request.seconds ? d->request.seconds
                                                                : TNY_DICTATION_SECONDS_MAX);
        if (d->audio.len >= target) tny_dictation_finish(d);
        if (!rc) return;
        audio_capture_free(d->capture);
        d->capture = NULL;
        if (d->audio.len > target) {
            secure_zero(d->audio.data + target, d->audio.len - target);
            d->audio.len = target;
        }
        memcpy(d->audio.data, "RIFF", 4);
        put32(d->audio.data + 4, (uint32_t)(d->audio.len - 8));
        memcpy(d->audio.data + 8, "WAVEfmt ", 8);
        put32(d->audio.data + 16, 16);
        d->audio.data[20] = 1;
        d->audio.data[22] = 1;
        put32(d->audio.data + 24, AUDIO_CAPTURE_RATE);
        put32(d->audio.data + 28, AUDIO_CAPTURE_RATE * 2);
        d->audio.data[32] = 2;
        d->audio.data[34] = 16;
        memcpy(d->audio.data + 36, "data", 4);
        put32(d->audio.data + 40, (uint32_t)(d->audio.len - 44));
        if (!tny_dictation_wav_valid(d->audio.data, d->audio.len)) {
            snprintf(d->error, sizeof d->error,
                     "recording is too short or incomplete; speak for at least one second");
            complete(d, 1);
            return;
        }
        d->state = TNY_DICTATION_TRANSCRIBING;
        return; /* let the frontend paint the transition before connecting */
    }
    if (d->state == TNY_DICTATION_NORMALIZING) {
        norm_step(d);
        return;
    }
    if (!d->job) {
        d->job = d->provider->start(d->ctx, &d->audio, d->error, sizeof d->error);
        private_free(&d->audio);
        if (!d->job) {
            d->norm = tny_norm_step(d->norm, TNY_NORM_EV_STT_FAILED);
            complete(d, 1);
            return;
        }
    }
    int rc = d->provider->step(d->job, &d->text, d->error, sizeof d->error);
    if (rc < 0) return;
    if (rc) {
        d->norm = tny_norm_step(d->norm, TNY_NORM_EV_STT_FAILED);
        complete(d, rc);
        return;
    }
    bool valid = tny_dictation_text_valid(d->text.data, d->text.len);
    d->norm = tny_norm_step(d->norm, valid ? TNY_NORM_EV_STT_VALID : TNY_NORM_EV_STT_INVALID);
    if (!valid) {
        snprintf(d->error, sizeof d->error, "empty or invalid dictation transcript");
        complete(d, 1);
        return;
    }
    if (d->norm.phase != TNY_NORM_NORMALIZING) {
        complete(d, 0);
        return;
    }
    d->provider->destroy(d->job);
    d->job = NULL;
    buf_append(&d->raw, d->text.data, d->text.len);
    if (d->raw.oom) {
        norm_event(d, TNY_NORM_EV_FAILED, "out_of_memory");
        return;
    }
    d->state = TNY_DICTATION_NORMALIZING;
    /* let the frontend paint the transition before connecting */
}

int tny_dictation_result(const tny_dictation *d, const char **text, const char **error) {
    if (text) *text = d->state == TNY_DICTATION_DONE && !d->rc ? d->text.data : NULL;
    if (error) *error = d->error;
    return d->state == TNY_DICTATION_DONE ? d->rc : -1;
}

bool tny_dictation_normalization_info(const tny_dictation *d, tny_dictation_normalization *out) {
    memset(out, 0, sizeof *out);
    if (!d || d->state != TNY_DICTATION_DONE || d->rc || !d->norm.requests) return false;
    out->normalized = d->norm.outcome == TNY_NORM_NORMALIZED;
    out->raw = d->raw.data;
    out->model = d->config.model;
    out->effort = d->effort_sent ? d->config.effort : NULL;
    out->service_tier = d->tier_sent ? "priority" : NULL;
    out->skipped_reason = out->normalized ? NULL : d->skipped;
    out->detail = d->config_error;
    out->corrections_json = out->normalized && d->corrections.data ? d->corrections.data : "[]";
    return true;
}

void tny_dictation_free(tny_dictation *d) {
    if (!d) return;
    audio_capture_free(d->capture);
    if (d->job) d->provider->destroy(d->job);
    normalizer_release(d);
    tny_norm_proposal_free(&d->accepted);
    private_free(&d->audio);
    private_free(&d->text);
    private_free(&d->raw);
    private_free(&d->corrections);
    free(d);
}
