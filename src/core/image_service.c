#include "core/image_provider.h"
#include "core/image.h"
#include "json/json.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const tny_image_provider *const providers[] = {&tny_image_codex};
static const tny_image_provider *find_provider(const char *name) {
    if (!name) name = "codex";
    for (size_t i = 0; i < sizeof providers / sizeof providers[0]; i++)
        if (strcmp(name, providers[i]->name) == 0) return providers[i];
    return NULL;
}

bool tny_image_stopped(const tny_image_request *r) {
    return r->cancelled && r->cancelled(r->userdata);
}

bool tny_image_available(const tny_ctx *ctx, const char *name, bool edit, char *err, size_t len) {
    const tny_image_provider *p = find_provider(name);
    const char *why = !p                           ? "unknown image provider"
                      : edit && !p->max_references ? "image provider does not support editing"
                                                   : NULL;
    if (why) {
        if (err && len) snprintf(err, len, "%s", why);
        return false;
    }
    return p->available(ctx, err, len);
}

bool tny_image_capabilities(const tny_ctx *ctx, bool edit, buf_t *names) {
    bool found = false;
    for (size_t i = 0; i < sizeof providers / sizeof providers[0]; i++) {
        const tny_image_provider *p = providers[i];
        if ((edit && !p->max_references) || !p->available(ctx, NULL, 0)) continue;
        if (names) {
            if (found) buf_appends(names, ", ");
            buf_appends(names, p->name);
        }
        found = true;
    }
    return found;
}

static bool valid_string(const char *s, size_t max) {
    return s && *s && strlen(s) <= max && utf8_valid_bytes(s, strlen(s));
}

int tny_image_options(int argc, char **argv, tny_image_request *r, bool *json, bool *check) {
    if (argc < 1) return 1;
    r->edit = strcmp(argv[0], "edit") == 0;
    if (!r->edit && strcmp(argv[0], "generate") != 0) return 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) return -1;
        if (strcmp(a, "--json") == 0) {
            *json = true;
            continue;
        }
        if (strcmp(a, "--check") == 0) {
            *check = true;
            continue;
        }
        const char **slot = strcmp(a, "--image-provider") == 0 ? &r->provider
                            : strcmp(a, "--model") == 0        ? &r->model
                            : strcmp(a, "--quality") == 0      ? &r->quality
                            : strcmp(a, "--size") == 0         ? &r->size
                            : strcmp(a, "--output-file") == 0  ? &r->output_file
                                                               : NULL;
        if (strcmp(a, "--image") == 0 && r->image_count < TNY_IMAGE_REFERENCES_MAX)
            slot = &r->images[r->image_count++];
        if (!slot || *slot || i + 1 >= argc || !*argv[i + 1]) return 1;
        *slot = argv[++i];
    }
    return 0;
}

static int base64_digit(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int tny_image_decode(const char *s, size_t n, buf_t *out, char *err, size_t len) {
    if (!s || !n || n % 4 || n > ((TNY_IMAGE_OUTPUT_MAX + 2u) / 3u) * 4u) goto invalid;
    size_t padding = (s[n - 1] == '=') + (size_t)(s[n - 2] == '=');
    size_t decoded = n / 4 * 3 - padding;
    if (decoded > TNY_IMAGE_OUTPUT_MAX) goto invalid;
    for (size_t i = 0; i < n - padding; i++)
        if (base64_digit((unsigned char)s[i]) < 0) goto invalid;
    if ((padding == 1 && (base64_digit((unsigned char)s[n - 2]) & 3)) ||
        (padding == 2 && (base64_digit((unsigned char)s[n - 3]) & 15)))
        goto invalid;
    /* b64_decode is deliberately permissive elsewhere; the checks above make
     * it safe here without changing existing protocol consumers. */
    char *data = malloc(decoded + 1);
    if (!data) goto invalid;
    size_t got = b64_decode(s, (uint8_t *)data, decoded);
    const char *mime = image_mime((const uint8_t *)data, got);
    if (got != decoded || !mime || strcmp(mime, "image/gif") == 0) {
        free(data);
        goto invalid;
    }
    buf_append(out, data, got);
    free(data);
    if (out->oom) goto invalid;
    return 0;
invalid:
    snprintf(err, len, "invalid or oversized base64 image response (PNG/JPEG/WebP required)");
    return 1;
}

/* Bounded read even if a file grows after fstat; avoid blocking on FIFOs. */
static int load_input(const tny_image_request *r, const char *path, tny_image_input *image,
                      char *err, size_t len) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    struct stat st;
    int rc = 1;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > TNY_IMAGE_INPUT_MAX)
        goto done;
    for (;;) {
        if (tny_image_stopped(r)) {
            rc = 130;
            goto done;
        }
        char chunk[8192];
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || (n > 0 && (size_t)n > TNY_IMAGE_INPUT_MAX - image->data.len)) goto done;
        if (!n) break;
        buf_append(&image->data, chunk, (size_t)n);
        if (image->data.oom) goto done;
    }
    image->mime = image_mime((const uint8_t *)image->data.data, image->data.len);
    if (image->mime && strcmp(image->mime, "image/gif") != 0) rc = 0;
done:
    if (fd >= 0) close(fd);
    if (rc)
        snprintf(
            err, len,
            rc == 130
                ? "image operation interrupted"
                : "cannot load reference image (regular PNG/JPEG/WebP, at most 8 MiB required)");
    return rc;
}

static int export_image(const tny_image_request *r, int fd, const char *tmp, const buf_t *data) {
    int rc = 0;
    size_t offset = 0;
    while (offset < data->len) {
        if (tny_image_stopped(r)) {
            rc = 130;
            break;
        }
        ssize_t n = write(fd, data->data + offset, data->len - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            rc = 1;
            break;
        }
        offset += (size_t)n;
    }
    if (close(fd) != 0 && !rc) rc = 1;
    if (!rc && tny_image_stopped(r)) rc = 130;
    if (!rc && rename(tmp, r->output_file) != 0) rc = 1;
    return rc;
}

int tny_image_run(const tny_ctx *ctx, const tny_image_request *r, tny_image_result *result,
                  char *err, size_t len) {
    *err = 0;
    memset(result, 0, sizeof *result);
    if (!valid_string(r->prompt, TNY_IMAGE_PROMPT_MAX) ||
        str_ws_prefix(r->prompt, strlen(r->prompt)) == strlen(r->prompt) ||
        !valid_string(r->output_file, 4096) || (r->model && !valid_string(r->model, 128)) ||
        (r->quality && strcmp(r->quality, "auto") != 0 && strcmp(r->quality, "low") != 0 &&
         strcmp(r->quality, "medium") != 0 && strcmp(r->quality, "high") != 0) ||
        (r->size && !valid_string(r->size, 32)) || r->image_count > TNY_IMAGE_REFERENCES_MAX ||
        (r->edit != (r->image_count > 0))) {
        snprintf(err, len,
                 "images need UTF-8 prompt (1-16384 bytes), output file, valid options; edit needs "
                 "1-5 references, generate none");
        return 1;
    }
    if (!tny_image_available(ctx, r->provider, r->edit, err, len)) return 1;
    const tny_image_provider *p = find_provider(r->provider);
    if (!p) {
        snprintf(err, len, "unknown image provider");
        return 1;
    }
    if (r->image_count > p->max_references) {
        snprintf(err, len, "too many references for image provider");
        return 1;
    }
    if (tny_image_stopped(r)) {
        snprintf(err, len, "image operation interrupted");
        return 130;
    }
    tny_image_input inputs[TNY_IMAGE_REFERENCES_MAX] = {0};
    buf_t image, tmp;
    buf_init(&image);
    buf_init(&tmp);
    int rc = 1, fd = -1;
    bool reserved = false;
    for (size_t i = 0; i < r->image_count; i++) {
        if (!valid_string(r->images[i], 4096)) {
            rc = 1;
            snprintf(err, len, "invalid reference path");
            goto done;
        }
        rc = load_input(r, r->images[i], &inputs[i], err, len);
        if (rc) goto done;
    }
    rc = 1;
    /* Reserve a private sibling before spending provider quota. Existing
     * output is untouched, including when it is also an edit reference. */
    struct stat st;
    if (lstat(r->output_file, &st) == 0 && !S_ISREG(st.st_mode)) {
        snprintf(err, len, "image output must be a regular file, not a directory or symlink");
        goto done;
    }
    buf_appendf(&tmp, "%s.XXXXXX", r->output_file);
    fd = tmp.oom ? -1 : mkstemp(tmp.data);
    if (fd < 0) {
        snprintf(err, len, "cannot create image output file");
        goto done;
    }
    reserved = true;
    tny_image_request request = *r;
    request.model = r->model ? r->model : p->default_model;
    rc = p->render(ctx, &request, inputs, &image, err, len);
    if (!rc && tny_image_stopped(r)) rc = 130;
    const char *mime = image_mime((const uint8_t *)image.data, image.len);
    if (!rc && (!mime || image.len > TNY_IMAGE_OUTPUT_MAX || strcmp(mime, "image/gif") == 0)) {
        snprintf(err, len, "invalid image provider output");
        rc = 1;
    }
    if (!rc) {
        rc = export_image(r, fd, tmp.data, &image);
        fd = -1;
        if (!rc) *result = (tny_image_result){p->name, request.model, mime, image.len};
        else snprintf(err, len, "cannot write image output file");
    }
done:
    if (fd >= 0) close(fd);
    if (reserved) unlink(tmp.data);
    for (size_t i = 0; i < TNY_IMAGE_REFERENCES_MAX; i++) buf_free(&inputs[i].data);
    buf_free(&image);
    buf_free(&tmp);
    if (rc == 130) snprintf(err, len, "image operation interrupted");
    else if (rc && !*err) snprintf(err, len, "image operation failed");
    return rc;
}

void tny_image_result_json(const tny_image_request *r, const tny_image_result *result, buf_t *out) {
    buf_appends(out, "{\"kind\":\"image\",\"ok\":true,\"operation\":");
    jescape(out, r->edit ? "edit" : "generate");
    buf_appends(out, ",\"provider\":");
    jescape(out, result->provider);
    buf_appends(out, ",\"model\":");
    jescape(out, result->model);
    buf_appends(out, ",\"path\":");
    jescape(out, r->output_file);
    buf_appends(out, ",\"mime_type\":");
    jescape(out, result->mime);
    buf_appendf(out, ",\"bytes\":%zu}\n", result->bytes);
}
