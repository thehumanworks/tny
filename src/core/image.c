/* image.c — detect common image types and attach them as Chat Completions
 * image_url parts. role:tool messages cannot carry those parts on most
 * OpenAI-compatible APIs, so the native loop injects a follow-up user
 * message instead (docs/adr/0008). */
#include "core/image.h"

#include "core/image_preview.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const char *image_mime(const uint8_t *data, size_t n) {
    if (!data || n < 12) return NULL;
    if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4e && data[3] == 0x47 &&
        data[4] == 0x0d && data[5] == 0x0a && data[6] == 0x1a && data[7] == 0x0a)
        return "image/png";
    if (data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) return "image/jpeg";
    if (data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8' &&
        (data[4] == '7' || data[4] == '9') && data[5] == 'a')
        return "image/gif";
    if (data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' && data[8] == 'W' &&
        data[9] == 'E' && data[10] == 'B' && data[11] == 'P')
        return "image/webp";
    return NULL;
}

void image_data_url(const uint8_t *data, size_t n, const char *mime, buf_t *out) {
    buf_appendf(out, "data:%s;base64,", mime ? mime : "application/octet-stream");
    b64_encode(data, n, out);
}

/* Bound the actual read, not a pathname's earlier size. Read one sentinel byte
 * beyond the limit to distinguish exact-limit content from a growing file. */
uint8_t *image_read_bounded(FILE *file, size_t *len_out, bool *too_large) {
    buf_t data;
    buf_init(&data);
    *len_out = 0;
    *too_large = false;
    unsigned char chunk[16384];
    while (!feof(file)) {
        size_t remaining = IMAGE_MAX_BYTES - data.len;
        size_t want = remaining < sizeof chunk ? remaining + 1 : sizeof chunk;
        size_t n = fread(chunk, 1, want, file);
        if (n > remaining) {
            *too_large = true;
            buf_free(&data);
            return NULL;
        }
        if (ferror(file)) {
            buf_free(&data);
            return NULL;
        }
        buf_append(&data, chunk, n);
        if (buf_oom(&data)) {
            buf_free(&data);
            return NULL;
        }
    }
    *len_out = data.len;
    return (uint8_t *)buf_detach(&data);
}

uint8_t *image_load_ex(const char *path, size_t *len_out, const char **mime_out,
                       const char **code_out, char *err, size_t errlen) {
    if (len_out) *len_out = 0;
    if (mime_out) *mime_out = NULL;
    if (code_out) *code_out = TNY_IMAGE_PREVIEW_CODE_UNREADABLE;
    if (!path || !*path) {
        if (err && errlen) snprintf(err, errlen, "missing image path");
        return NULL;
    }
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0) close(fd);
        if (err && errlen) snprintf(err, errlen, "cannot read image %s", path);
        return NULL;
    }
    FILE *file = fdopen(fd, "rb");
    if (!file) {
        close(fd);
        if (err && errlen) snprintf(err, errlen, "cannot read image %s", path);
        return NULL;
    }
    size_t len = 0;
    bool too_large = false;
    uint8_t *data = image_read_bounded(file, &len, &too_large);
    fclose(file);
    if (too_large) {
        if (code_out) *code_out = TNY_IMAGE_PREVIEW_CODE_TOO_LARGE;
        if (err && errlen)
            snprintf(err, errlen, "image %s exceeds max %u bytes", path, IMAGE_MAX_BYTES);
        return NULL;
    }
    if (!data) {
        if (err && errlen) snprintf(err, errlen, "cannot read image %s", path);
        return NULL;
    }
    const char *mime = image_mime((const uint8_t *)data, len);
    if (!mime) {
        free(data);
        if (code_out) *code_out = TNY_IMAGE_PREVIEW_CODE_FORMAT;
        if (err && errlen) snprintf(err, errlen, "%s is not a png/jpeg/gif/webp image", path);
        return NULL;
    }
    if (len_out) *len_out = len;
    if (mime_out) *mime_out = mime;
    if (code_out) *code_out = NULL;
    return (uint8_t *)data;
}

uint8_t *image_load(const char *path, size_t *len_out, const char **mime_out, char *err,
                    size_t errlen) {
    return image_load_ex(path, len_out, mime_out, NULL, err, errlen);
}

/* The one user-message shape (docs/adr/0008), built once and used by both the
 * path-loading wrapper and the captured-byte queue. The message is appended
 * only by builder_commit, so any failure leaves the transcript untouched. */
typedef struct {
    yyjson_mut_val *msg;
    yyjson_mut_val *parts;
} image_msg_builder;

static int builder_begin(tny_session_state *s, const char *text, image_msg_builder *b) {
    yyjson_mut_doc *doc = s->doc;
    b->msg = yyjson_mut_obj(doc);
    b->parts = yyjson_mut_arr(doc);
    yyjson_mut_val *tp = yyjson_mut_obj(doc);
    if (!b->msg || !b->parts || !tp) return -1;
    yyjson_mut_obj_put(b->msg, yyjson_mut_strcpy(doc, "role"), yyjson_mut_strcpy(doc, "user"));
    yyjson_mut_obj_put(tp, yyjson_mut_strcpy(doc, "type"), yyjson_mut_strcpy(doc, "text"));
    yyjson_mut_obj_put(tp, yyjson_mut_strcpy(doc, "text"),
                       yyjson_mut_strcpy(doc, text ? text : ""));
    yyjson_mut_arr_add_val(b->parts, tp);
    return 0;
}

static int builder_add(tny_session_state *s, image_msg_builder *b, const uint8_t *data, size_t len,
                       const char *mime) {
    yyjson_mut_doc *doc = s->doc;
    buf_t url;
    buf_init(&url);
    image_data_url(data, len, mime, &url);
    if (buf_oom(&url)) {
        buf_free(&url);
        return -1;
    }
    yyjson_mut_val *ip = yyjson_mut_obj(doc);
    yyjson_mut_val *iu = yyjson_mut_obj(doc);
    if (!ip || !iu) {
        buf_free(&url);
        return -1;
    }
    yyjson_mut_obj_put(ip, yyjson_mut_strcpy(doc, "type"), yyjson_mut_strcpy(doc, "image_url"));
    yyjson_mut_obj_put(iu, yyjson_mut_strcpy(doc, "url"), yyjson_mut_strcpy(doc, url.data));
    yyjson_mut_obj_put(ip, yyjson_mut_strcpy(doc, "image_url"), iu);
    yyjson_mut_arr_add_val(b->parts, ip);
    buf_free(&url);
    return 0;
}

static void builder_commit(tny_session_state *s, image_msg_builder *b) {
    yyjson_mut_obj_put(b->msg, yyjson_mut_strcpy(s->doc, "content"), b->parts);
    yyjson_mut_arr_add_val(session_messages(s), b->msg);
}

int session_add_user_images(tny_session_state *s, const char *text, const char **paths, char *err,
                            size_t errlen) {
    if (!s || !paths || !paths[0]) {
        if (err && errlen) snprintf(err, errlen, "no images");
        return -1;
    }
    image_msg_builder b;
    if (builder_begin(s, text, &b) != 0) {
        if (err && errlen) snprintf(err, errlen, "out of memory");
        return -1;
    }
    for (int i = 0; paths[i]; i++) {
        size_t len = 0;
        const char *mime = NULL;
        char local[256];
        /* One path at a time: the wrapper never holds every file at once. */
        uint8_t *data =
            image_load(paths[i], &len, &mime, err ? err : local, err ? errlen : sizeof local);
        if (!data) return -1;
        int rc = builder_add(s, &b, data, len, mime);
        free(data);
        if (rc != 0) {
            if (err && errlen) snprintf(err, errlen, "out of memory");
            return -1;
        }
    }
    builder_commit(s, &b);
    return 0;
}

int session_add_user_loaded_images(tny_session_state *s, const char *text,
                                   const tny_image_part *parts, int n, char *err, size_t errlen) {
    if (!s || !parts || n <= 0 || n > 8) {
        if (err && errlen) snprintf(err, errlen, "no images");
        return -1;
    }
    image_msg_builder b;
    if (builder_begin(s, text, &b) != 0) {
        if (err && errlen) snprintf(err, errlen, "out of memory");
        return -1;
    }
    for (int i = 0; i < n; i++) {
        if (!parts[i].data || parts[i].len > IMAGE_MAX_BYTES ||
            !image_mime(parts[i].data, parts[i].len)) {
            if (err && errlen) snprintf(err, errlen, "invalid or oversized loaded image");
            return -1;
        }
        if (builder_add(s, &b, parts[i].data, parts[i].len, parts[i].mime) != 0) {
            if (err && errlen) snprintf(err, errlen, "out of memory");
            return -1;
        }
    }
    builder_commit(s, &b);
    return 0;
}
