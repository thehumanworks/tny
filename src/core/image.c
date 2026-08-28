/* image.c — detect common image types and attach them as Chat Completions
 * image_url parts. role:tool messages cannot carry those parts on most
 * OpenAI-compatible APIs, so the native loop injects a follow-up user
 * message instead (docs/adr/0008). */
#include "core/image.h"

#include <stdio.h>
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

uint8_t *image_load(const char *path, size_t *len_out, const char **mime_out, char *err,
                    size_t errlen) {
    if (len_out) *len_out = 0;
    if (mime_out) *mime_out = NULL;
    if (!path || !*path) {
        if (err && errlen) snprintf(err, errlen, "missing image path");
        return NULL;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (err && errlen) snprintf(err, errlen, "cannot read image %s", path);
        return NULL;
    }
    if ((size_t)st.st_size > IMAGE_MAX_BYTES) {
        if (err && errlen)
            snprintf(err, errlen, "image %s is %lld bytes (max %u)", path, (long long)st.st_size,
                     IMAGE_MAX_BYTES);
        return NULL;
    }
    size_t len = 0;
    char *data = file_slurp(path, &len);
    if (!data) {
        if (err && errlen) snprintf(err, errlen, "cannot read image %s", path);
        return NULL;
    }
    const char *mime = image_mime((const uint8_t *)data, len);
    if (!mime) {
        free(data);
        if (err && errlen) snprintf(err, errlen, "%s is not a png/jpeg/gif/webp image", path);
        return NULL;
    }
    if (len_out) *len_out = len;
    if (mime_out) *mime_out = mime;
    return (uint8_t *)data;
}

int session_add_user_images(tny_session_state *s, const char *text, const char **paths, char *err,
                            size_t errlen) {
    if (!s || !paths || !paths[0]) {
        if (err && errlen) snprintf(err, errlen, "no images");
        return -1;
    }
    yyjson_mut_doc *doc = s->doc;
    yyjson_mut_val *m = yyjson_mut_obj(doc);
    yyjson_mut_obj_put(m, yyjson_mut_strcpy(doc, "role"), yyjson_mut_strcpy(doc, "user"));
    yyjson_mut_val *parts = yyjson_mut_arr(doc);
    yyjson_mut_val *tp = yyjson_mut_obj(doc);
    yyjson_mut_obj_put(tp, yyjson_mut_strcpy(doc, "type"), yyjson_mut_strcpy(doc, "text"));
    yyjson_mut_obj_put(tp, yyjson_mut_strcpy(doc, "text"),
                       yyjson_mut_strcpy(doc, text ? text : ""));
    yyjson_mut_arr_add_val(parts, tp);

    for (int i = 0; paths[i]; i++) {
        size_t len = 0;
        const char *mime = NULL;
        char local[256];
        uint8_t *data =
            image_load(paths[i], &len, &mime, err ? err : local, err ? errlen : sizeof local);
        if (!data) return -1;
        buf_t url;
        buf_init(&url);
        image_data_url(data, len, mime, &url);
        free(data);
        yyjson_mut_val *ip = yyjson_mut_obj(doc);
        yyjson_mut_obj_put(ip, yyjson_mut_strcpy(doc, "type"), yyjson_mut_strcpy(doc, "image_url"));
        yyjson_mut_val *iu = yyjson_mut_obj(doc);
        yyjson_mut_obj_put(iu, yyjson_mut_strcpy(doc, "url"), yyjson_mut_strcpy(doc, url.data));
        yyjson_mut_obj_put(ip, yyjson_mut_strcpy(doc, "image_url"), iu);
        yyjson_mut_arr_add_val(parts, ip);
        buf_free(&url);
    }
    yyjson_mut_obj_put(m, yyjson_mut_strcpy(doc, "content"), parts);
    yyjson_mut_arr_add_val(session_messages(s), m);
    return 0;
}
