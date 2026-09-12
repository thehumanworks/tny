/* Standalone image CLI: same service as native tools (ADR 0075, ADR 0094). */
#include "cli/cli.h"
#include "core/image_export.h"
#include "core/image_preview.h"
#include "core/image_manifest.h"
#include "core/image_service.h"
#include "cli/cmd_control.h"
#include "util/process.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;
static void on_signal(int sig) {
    (void)sig;
    interrupted = 1;
}
static bool cancelled(void *ud) {
    (void)ud;
    /* The image adapter consults this on each bounded read/poll iteration, so
     * a job item whose supervisor died stops there too (docs/adr/0093). An
     * ordinary `tny image` run has no expected parent and is unaffected. */
    return interrupted != 0 || tny_process_parent_lost();
}

/* The only CLI preview transport. No stdout, manual attachment, or retry. */
static tny_image_preview_status preview_control(void *ud, const tny_image_preview_identity *id,
                                                tny_image_preview_result *result) {
    (void)ud;
    tny_control_reply reply;
    tny_control_exchange exchange = tny_control_request(
        TNY_CONTROL_OP_IMAGE_PREVIEW, id->path, id->sha256, id->job ? id->job->bytes : 0, &reply);
    tny_image_preview_status status = TNY_IMAGE_PREVIEW_FAILED;
    const char *code = "uncorrelated_or_missing_ack";
    if (exchange == TNY_CONTROL_EXCHANGE_NO_SOCKET) {
        status = TNY_IMAGE_PREVIEW_UNAVAILABLE_SESSION;
        code = TNY_IMAGE_PREVIEW_CODE_NO_SESSION;
    } else if (exchange == TNY_CONTROL_EXCHANGE_UNSUPPORTED) {
        status = TNY_IMAGE_PREVIEW_UNSUPPORTED;
        code = "socket_control_unavailable";
    } else if (exchange == TNY_CONTROL_EXCHANGE_OK && reply.status) {
        if (reply.ok && strcmp(reply.status, "queued") == 0) {
            status = TNY_IMAGE_PREVIEW_QUEUED;
            code = NULL;
            snprintf(result->receipt, sizeof result->receipt, "%s", reply.id);
        } else if (!reply.ok) {
            if (strcmp(reply.status, "unsupported") == 0) status = TNY_IMAGE_PREVIEW_UNSUPPORTED;
            else if (strcmp(reply.status, "unavailable_session") == 0)
                status = TNY_IMAGE_PREVIEW_UNAVAILABLE_SESSION;
            else if (strcmp(reply.status, "turn_not_ready") == 0)
                status = TNY_IMAGE_PREVIEW_TURN_NOT_READY;
            code = reply.error_code ? reply.error_code : "admission_failed";
        }
    }
    if (code) snprintf(result->code, sizeof result->code, "%s", code);
    tny_control_reply_free(&reply);
    return status;
}

static void preview_report(const tny_image_preview_result *r) {
    if (r->status != TNY_IMAGE_PREVIEW_QUEUED)
        fprintf(stderr, "tny: image: preview %s (%s): %s\n",
                tny_image_preview_status_name(r->status), r->code, tny_image_preview_fallback(r));
}

/* Metadata lookup needs only local paths/roots; never resolve a chat profile
 * or refresh unrelated authentication for a standalone image operation. */
static bool image_context_paths(tny_ctx *ctx, const cli_globals *g) {
    ctx->cwd = path_abs(".");
    ctx->tny_dir = path_tny_dir();
    ctx->extra_dirs = g->n_add_dirs ? calloc((size_t)g->n_add_dirs, sizeof(char *)) : NULL;
    if (!ctx->cwd || !ctx->tny_dir || (g->n_add_dirs && !ctx->extra_dirs)) return false;
    for (int i = 0; i < g->n_add_dirs; i++) {
        ctx->extra_dirs[i] = path_abs(g->add_dirs[i]);
        ctx->n_extra_dirs++;
        if (!ctx->extra_dirs[i]) return false;
    }
    return true;
}

static void image_context_free(tny_ctx *ctx) {
    free(ctx->cwd);
    free(ctx->tny_dir);
    for (int i = 0; i < ctx->n_extra_dirs; i++) free(ctx->extra_dirs[i]);
    free(ctx->extra_dirs);
}

static int cmd_image_preview(const cli_globals *g, bool json, int argc, char **argv) {
    const char *manifest = NULL, *job = NULL;
    int item = -1;
    int parsed = tny_image_preview_options(argc, argv, &manifest, &job, &item, &json);
    if (parsed < 0) {
        help_for("image");
        return 0;
    }
    if (parsed || g->ssh) {
        fputs("tny: image preview: use --manifest RECORD or --job ID --item N\n", stderr);
        return 1;
    }
    if (g->cwd && chdir(g->cwd) != 0) return 1;
    char err[256] = "cannot select preview artifact";
    tny_ctx ctx = {0};
    bool paths = image_context_paths(&ctx, g);
    tny_image_preview_selection *selection =
        paths ? tny_image_preview_select(&ctx, manifest, job, item, err, sizeof err) : NULL;
    image_context_free(&ctx);
    if (!selection) {
        fprintf(stderr, "tny: image preview: %s\n", err);
        return 1;
    }
    tny_image_preview_result preview;
    tny_image_preview_coordinate(true, &selection->identity, preview_control, NULL, &preview);
    bool queued = preview.status == TNY_IMAGE_PREVIEW_QUEUED;
    preview_report(&preview);
    int rc = queued ? 0 : 1;
    if (json) {
        buf_t out;
        buf_init(&out);
        buf_appendf(&out, "{\"kind\":\"image_preview\",\"ok\":%s}", queued ? "true" : "false");
        tny_image_preview_append(&out, &selection->identity, &preview);
        if (out.oom || fwrite(out.data, 1, out.len, stdout) != out.len) rc = 1;
        buf_free(&out);
    } else if (queued) puts("image preview queued for the next request; not proof of delivery");
    tny_image_preview_selection_free(selection);
    return rc;
}

/* An explicit local transform: no prompt, no stdin, no provider. The same
 * service the typed tools and terminal interception use (docs/adr/0094). */
static int cmd_image_export(const cli_globals *g, bool json, int argc, char **argv) {
    tny_image_export_request r = {.cancelled = cancelled};
    int parsed = tny_image_export_options(argc, argv, &r, &json);
    const char *topic =
        argc > 0 && strcmp(argv[0], "contact-sheet") == 0 ? "image contact-sheet" : "image export";
    if (parsed < 0) {
        help_for(topic);
        return 0;
    }
    if (parsed || g->ssh) {
        fprintf(stderr, "tny: image: invalid options or --ssh; see tny %s --help\n", topic);
        return 1;
    }
    if (g->cwd && chdir(g->cwd) != 0) {
        fputs("tny: image: cannot enter --cwd\n", stderr);
        return 1;
    }
    interrupted = 0;
    struct sigaction sa = {0}, oldint = {0}, oldterm = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    bool have_int = sigaction(SIGINT, &sa, &oldint) == 0;
    bool have_term = sigaction(SIGTERM, &sa, &oldterm) == 0;
    tny_ctx ctx = {0};
    tny_image_export_result result = {0};
    char err[256] = "";
    int rc = tny_image_export_run(&ctx, &r, &result, err, sizeof err);
    tny_image_preview_identity identity = {
        result.output, result.sha256, result.manifest_path, result.operation_id, true, NULL, NULL};
    tny_image_preview_result preview;
    if (r.preview) {
        tny_image_preview_coordinate(rc == 0, &identity, preview_control, NULL, &preview);
        preview_report(&preview);
    }
    if (have_int) sigaction(SIGINT, &oldint, NULL);
    if (have_term) sigaction(SIGTERM, &oldterm, NULL);
    /* Provenance and diagnostics are guidance on stderr; stdout stays the
     * destination path, or the result object under --json. */
    if (*result.manifest_path) fprintf(stderr, "tny: image: manifest: %s\n", result.manifest_path);
    if (rc) fprintf(stderr, "tny: image: %s\n", err);
    if (json && (!rc || result.code)) {
        buf_t out;
        buf_init(&out);
        if (!rc) tny_image_export_result_json(&r, &result, &out);
        else if (tny_image_export_retained(&result))
            tny_image_export_retained_json(&r, &result, err, &out);
        else tny_image_export_error_json(&r, &result, err, &out);
        if (r.preview) tny_image_preview_append(&out, &identity, &preview);
        if (out.oom || fwrite(out.data, 1, out.len, stdout) != out.len) rc = rc ? rc : 1;
        buf_free(&out);
    } else if (!rc) puts(*result.output ? result.output : r.output_file);
    return rc;
}

int cmd_image_service(const cli_globals *g, int argc, char **argv) {
    tny_image_request r = {.cancelled = cancelled};
    bool json = g->json, check = false;
    /* Private worker prefix, not part of the shared user/tool grammar. */
    while (argc > 0 &&
           (strcmp(argv[0], "--json") == 0 || strcmp(argv[0], "--job-no-replace") == 0)) {
        if (strcmp(argv[0], "--json") == 0) json = true;
        else r.no_replace = true;
        argc--;
        argv++;
    }
    if (argc > 0 && strcmp(argv[0], "attach") == 0) return cmd_image(json, argc, argv);
    if (argc > 0 && strcmp(argv[0], "preview") == 0) return cmd_image_preview(g, json, argc, argv);
    if (argc > 0 && (strcmp(argv[0], "export") == 0 || strcmp(argv[0], "contact-sheet") == 0))
        return cmd_image_export(g, json, argc, argv);
    int parsed = tny_image_options(argc, argv, &r, &json, &check);
    if (parsed < 0) {
        help_for("image");
        return 0;
    }
    if (parsed || g->ssh || (r.preview && (check || r.no_replace))) {
        fputs("tny: image: invalid options or --ssh; see tny image --help\n", stderr);
        return 1;
    }
    if (g->cwd && chdir(g->cwd) != 0) {
        fputs("tny: image: cannot enter --cwd\n", stderr);
        return 1;
    }
    tny_image_result result = {0};
    /* This service consumes only ChatGPT flag credentials from the context.
     * Loading a selected chat profile here could refresh unrelated accounts. */
    tny_ctx ctx = {.chatgpt_token = (char *)g->chatgpt_token,
                   .chatgpt_account_id = (char *)g->chatgpt_account_id};
    char err[256] = "";
    if (check) {
        bool available = tny_image_available(&ctx, r.provider, r.edit, err, sizeof err);
        if (json) printf("{\"kind\":\"image\",\"available\":%s}\n", available ? "true" : "false");
        else puts(available ? "image provider available" : "image provider unavailable");
        if (!available) fprintf(stderr, "tny: image: %s\n", err);
        return available ? 0 : 1;
    }
    /* A local export's record documents work tny did itself: rerunning it as
     * a provider request would invent a generation that never happened
     * (docs/adr/0094). The refusal names the operations that do apply. */
    if (r.from_manifest) {
        char why[256] = "";
        tny_image_manifest *record = tny_image_manifest_load(r.from_manifest, why, sizeof why);
        bool derived = tny_image_manifest_derived(record);
        tny_image_manifest_free(record);
        if (derived) {
            fputs("tny: image: this record documents a local image export, not a provider "
                  "operation; export again from its source, or pass the derived image as an "
                  "edit reference\n",
                  stderr);
            return 1;
        }
    }
    /* A pure replay reruns a recorded prompt, so a terminal is not an error
     * there; piped text still overrides that prompt explicitly. */
    bool have_stdin = !isatty(STDIN_FILENO);
    if (!have_stdin && !r.replay) {
        fputs("tny: image: pipe text on stdin (see tny image --help)\n", stderr);
        return 1;
    }
    /* A job item whose supervisor is already gone makes no paid request. */
    if (tny_process_parent_lost()) {
        fputs("tny: image: the job supervisor that started this operation is gone\n", stderr);
        return 1;
    }
    interrupted = 0;
    struct sigaction sa = {0}, oldint = {0}, oldterm = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    bool have_int = sigaction(SIGINT, &sa, &oldint) == 0;
    bool have_term = sigaction(SIGTERM, &sa, &oldterm) == 0;
    buf_t text;
    buf_init(&text);
    int rc = 0;
    while (have_stdin) {
        char chunk[4096];
        ssize_t n = read(STDIN_FILENO, chunk, sizeof chunk);
        if (interrupted) {
            rc = 130;
            break;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || (n > 0 && (size_t)n > TNY_IMAGE_PROMPT_MAX - text.len)) {
            rc = 1;
            break;
        }
        if (!n) break;
        buf_append(&text, chunk, (size_t)n);
        if (text.oom) {
            rc = 1;
            break;
        }
    }
    if (!rc && have_stdin && !utf8_valid_bytes(text.data, text.len)) rc = 1;
    /* Pin the producer's cwd before generation. The owning runner can have a
     * different cwd; resolving there would select another file. No image bytes
     * are read here, and the digest remains the producer's validated digest. */
    char *preview_path = r.preview ? path_abs(r.output_file) : NULL;
    if (!rc) {
        /* Empty piped text is not an override: a replay then keeps the
         * recorded prompt, and generate/edit still fail validation. */
        r.prompt = text.len ? text.data : r.replay ? NULL : "";
        bool paths = !r.job || image_context_paths(&ctx, g);
        if (paths) rc = tny_image_run(&ctx, &r, &result, err, sizeof err);
        else {
            rc = 1;
            snprintf(err, sizeof err, "cannot initialize image metadata roots");
        }
        if (r.job) image_context_free(&ctx);
    } else
        snprintf(err, sizeof err,
                 rc == 130 ? "image operation interrupted" : "invalid or oversized stdin text");
    buf_free(&text);
    if (have_int) sigaction(SIGINT, &oldint, NULL);
    if (have_term) sigaction(SIGTERM, &oldterm, NULL);
    char warning[320];
    /* Ordinary stdout stays the destination path; guidance goes to stderr. */
    if (!rc && tny_image_size_warning(result.requested_size,
                                      tny_image_size_status_name(result.size_status), result.width,
                                      result.height, warning, sizeof warning))
        fprintf(stderr, "tny: image: warning: %s\n", warning);
    /* Provenance is guidance, so it joins the warning on stderr and never
     * changes what a script reading stdout sees. */
    if (*result.manifest_path) fprintf(stderr, "tny: image: manifest: %s\n", result.manifest_path);
    if (rc) fprintf(stderr, "tny: image: %s\n", err);
    if (result.cleanup_warning)
        fputs("tny: image: warning: output was committed and kept, but its private temporary "
              "name could not be removed\n",
              stderr);
    tny_image_preview_identity identity = {
        preview_path, result.sha256, result.manifest_path, result.operation_id, false, NULL, NULL};
    tny_image_preview_result preview;
    if (r.preview) {
        tny_image_preview_coordinate(rc == 0, &identity, preview_control, NULL, &preview);
        preview_report(&preview);
    }
    bool retained = tny_image_retained_failure(&result);
    if (json && (!rc || result.code)) {
        buf_t out;
        buf_init(&out);
        if (retained) tny_image_retained_json(&r, &result, err, &out);
        else if (rc) tny_image_error_json(&r, &result, err, &out);
        else tny_image_result_json(&r, &result, &out);
        if (r.preview) tny_image_preview_append(&out, &identity, &preview);
        if (out.oom || fwrite(out.data, 1, out.len, stdout) != out.len) rc = rc ? rc : 1;
        buf_free(&out);
    } else if (!rc) puts(r.output_file);
    free(preview_path);
    return rc;
}
