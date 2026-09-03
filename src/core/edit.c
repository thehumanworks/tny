/* edit.c — exact-match, atomic file replacement shared by tools and CLI. */
#include "core/edit.h"
#include "json/json.h"
#include "util/util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EDIT_CONTEXT_COMPARE_MAX 512u

static size_t minimum3(size_t a, size_t b, size_t c) {
    size_t m = a < b ? a : b;
    return m < c ? m : c;
}

/* Bounded bytewise Levenshtein distance. Context is diagnostic only; the edit
 * itself remains an unbounded exact byte-string replacement. */
static size_t line_distance(const char *a, size_t an, const char *b, size_t bn) {
    if (an > EDIT_CONTEXT_COMPARE_MAX) an = EDIT_CONTEXT_COMPARE_MAX;
    if (bn > EDIT_CONTEXT_COMPARE_MAX) bn = EDIT_CONTEXT_COMPARE_MAX;
    size_t row[EDIT_CONTEXT_COMPARE_MAX + 1];
    for (size_t j = 0; j <= bn; j++) row[j] = j;
    for (size_t i = 1; i <= an; i++) {
        size_t diagonal = row[0];
        row[0] = i;
        for (size_t j = 1; j <= bn; j++) {
            size_t above = row[j];
            size_t substitution = diagonal + (a[i - 1] == b[j - 1] ? 0u : 1u);
            row[j] = minimum3(row[j] + 1, row[j - 1] + 1, substitution);
            diagonal = above;
        }
    }
    return row[bn];
}

static bool first_search_line(const char *old_text, const char **line, size_t *len) {
    const char *p = old_text;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n && p[n - 1] == '\r') n--;
        if (n) {
            *line = p;
            *len = n;
            return true;
        }
        if (!end) break;
        p = end + 1;
    }
    return false;
}

static bool nearest_unique_line(const char *data, size_t len, const char *old_text,
                                tny_edit_result *result) {
    const char *needle = NULL;
    size_t needle_len = 0;
    if (!first_search_line(old_text, &needle, &needle_len)) return true;

    size_t best_distance = SIZE_MAX;
    size_t best_offset = 0;
    size_t best_len = 0;
    size_t best_line = 0;
    size_t best_count = 0;
    size_t line_no = 1;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i != len && data[i] != '\n') continue;
        size_t n = i - start;
        if (n && data[start + n - 1] == '\r') n--;
        if (n) {
            size_t distance = line_distance(needle, needle_len, data + start, n);
            if (distance < best_distance) {
                best_distance = distance;
                best_offset = start;
                best_len = n;
                best_line = line_no;
                best_count = 1;
            } else if (distance == best_distance) {
                best_count++;
            }
        }
        start = i + 1;
        line_no++;
    }
    if (best_count != 1) return true;
    result->nearest_context = xstrndup(data + best_offset, best_len);
    if (!result->nearest_context) return false;
    result->nearest_line = best_line;
    return true;
}

static bool edit_is_interrupted(const tny_edit_hooks *hooks) {
    return hooks && hooks->interrupted && hooks->interrupted(hooks->interrupted_userdata);
}

/* Same temp-file + rename contract as file_write_atomic, with cancellation
 * checks before every write and before the commit point. */
static int write_atomic_checked(const char *path, const void *data, size_t len,
                                const tny_edit_hooks *hooks) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, getpid()) >= (int)sizeof tmp) return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    const char *p = data;
    size_t offset = 0;
    while (offset < len) {
        if (edit_is_interrupted(hooks)) {
            close(fd);
            unlink(tmp);
            return 1;
        }
        ssize_t written = write(fd, p + offset, len - offset);
        if (written < 0) {
            if (errno == EINTR && edit_is_interrupted(hooks)) {
                close(fd);
                unlink(tmp);
                return 1;
            }
            int saved = errno;
            close(fd);
            unlink(tmp);
            errno = saved;
            return -1;
        }
        if (written == 0) {
            close(fd);
            unlink(tmp);
            errno = EIO;
            return -1;
        }
        offset += (size_t)written;
    }
    if (edit_is_interrupted(hooks)) {
        close(fd);
        unlink(tmp);
        return 1;
    }
    if (close(fd) != 0) {
        int saved = errno;
        unlink(tmp);
        errno = saved;
        return -1;
    }
    if (edit_is_interrupted(hooks)) {
        unlink(tmp);
        return 1;
    }
    if (rename(tmp, path) != 0) {
        int saved = errno;
        unlink(tmp);
        errno = saved;
        return -1;
    }
    return 0;
}

void tny_edit_result_free(tny_edit_result *result) {
    if (!result) return;
    free(result->nearest_context);
    memset(result, 0, sizeof *result);
}

tny_edit_status tny_edit_file_exact(const char *path, const char *old_text, const char *new_text,
                                    bool replace_all, const tny_edit_hooks *hooks,
                                    tny_edit_result *result) {
    if (!result) return TNY_EDIT_NOMEM;
    memset(result, 0, sizeof *result);
    if (!path || !*path || !old_text || !*old_text || !new_text) return TNY_EDIT_NOT_FOUND;

    char *resolved = path_abs(path);
    if (!resolved) {
        result->error_number = errno;
        return TNY_EDIT_READ_ERROR;
    }
    size_t len = 0;
    char *data = file_slurp(resolved, &len);
    if (!data) {
        result->error_number = errno;
        free(resolved);
        return TNY_EDIT_READ_ERROR;
    }

    size_t old_len = strlen(old_text);
    for (char *p = data; (p = strstr(p, old_text)); p += old_len) {
        if (edit_is_interrupted(hooks)) {
            free(data);
            free(resolved);
            return TNY_EDIT_INTERRUPTED;
        }
        result->matches++;
    }
    if (result->matches == 0) {
        bool ok = nearest_unique_line(data, len, old_text, result);
        free(data);
        free(resolved);
        if (edit_is_interrupted(hooks)) {
            tny_edit_result_free(result);
            return TNY_EDIT_INTERRUPTED;
        }
        return ok ? TNY_EDIT_NOT_FOUND : TNY_EDIT_NOMEM;
    }
    if (result->matches > 1 && !replace_all) {
        free(data);
        free(resolved);
        return TNY_EDIT_AMBIGUOUS;
    }

    buf_t out;
    buf_init(&out);
    char *p = data;
    for (;;) {
        if (edit_is_interrupted(hooks)) {
            buf_free(&out);
            free(data);
            free(resolved);
            return TNY_EDIT_INTERRUPTED;
        }
        char *hit = strstr(p, old_text);
        if (!hit) {
            buf_appends(&out, p);
            break;
        }
        buf_append(&out, p, (size_t)(hit - p));
        buf_appends(&out, new_text);
        p = hit + old_len;
        if (!replace_all) {
            buf_appends(&out, p);
            break;
        }
    }
    if (buf_oom(&out)) {
        buf_free(&out);
        free(data);
        free(resolved);
        return TNY_EDIT_NOMEM;
    }
    if (hooks && hooks->before_write) hooks->before_write(resolved, hooks->before_write_userdata);
    int write_status = write_atomic_checked(resolved, out.data, out.len, hooks);
    if (write_status != 0) {
        result->error_number = errno;
        buf_free(&out);
        free(data);
        free(resolved);
        if (write_status == 1) return TNY_EDIT_INTERRUPTED;
        return TNY_EDIT_WRITE_ERROR;
    }
    result->replaced = replace_all ? result->matches : 1;
    buf_free(&out);
    free(data);
    free(resolved);
    return TNY_EDIT_OK;
}

/* ---- verb contract: stdin payloads and rendered output (docs/adr/0064) ---- */

void tny_edit_payload_free(tny_edit_payload *payload) {
    if (!payload) return;
    free(payload->old_text);
    free(payload->new_text);
    memset(payload, 0, sizeof *payload);
}

static bool json_field_known(const char *name) {
    return strcmp(name, "old") == 0 || strcmp(name, "new") == 0 || strcmp(name, "replace_all") == 0;
}

static bool parse_json_payload(const char *input, size_t len, tny_edit_payload *payload, char *err,
                               size_t errlen) {
    yyjson_doc *doc = jparse(input, len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!root || !yyjson_is_obj(root)) {
        snprintf(err, errlen, "stdin must be one JSON object");
        yyjson_doc_free(doc);
        return false;
    }
    yyjson_obj_iter iter = yyjson_obj_iter_with(root);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter))) {
        const char *name = yyjson_get_str(key);
        if (!name || !json_field_known(name)) {
            snprintf(err, errlen, "unknown JSON field '%s'", name ? name : "");
            yyjson_doc_free(doc);
            return false;
        }
    }
    yyjson_val *old_value = jget(root, "old");
    yyjson_val *new_value = jget(root, "new");
    yyjson_val *all_value = jget(root, "replace_all");
    if (!yyjson_is_str(old_value) || !yyjson_get_len(old_value) || !yyjson_is_str(new_value) ||
        (all_value && !yyjson_is_bool(all_value))) {
        snprintf(err, errlen,
                 "JSON needs non-empty string 'old', string 'new', and optional boolean "
                 "'replace_all'");
        yyjson_doc_free(doc);
        return false;
    }
    const char *old_text = yyjson_get_str(old_value);
    const char *new_text = yyjson_get_str(new_value);
    if (strlen(old_text) != yyjson_get_len(old_value) ||
        strlen(new_text) != yyjson_get_len(new_value)) {
        snprintf(err, errlen, "edit strings may not contain NUL bytes");
        yyjson_doc_free(doc);
        return false;
    }
    payload->old_text = xstrdup(old_text);
    payload->new_text = xstrdup(new_text);
    payload->replace_all = all_value ? yyjson_get_bool(all_value) : false;
    yyjson_doc_free(doc);
    if (!payload->old_text || !payload->new_text) {
        tny_edit_payload_free(payload);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    return true;
}

static bool next_line(const char *input, size_t len, size_t *position, size_t *start,
                      size_t *line_len) {
    if (*position >= len) return false;
    *start = *position;
    size_t end = *position;
    while (end < len && input[end] != '\n') end++;
    *line_len = end - *start;
    if (*line_len && input[*start + *line_len - 1] == '\r') (*line_len)--;
    *position = end < len ? end + 1 : end;
    return true;
}

static bool marker_line(const char *input, size_t start, size_t line_len, const char *marker,
                        const char *name) {
    size_t marker_len = strlen(marker);
    size_t name_len = strlen(name);
    return line_len == marker_len + 1 + name_len &&
           memcmp(input + start, marker, marker_len) == 0 && input[start + marker_len] == ' ' &&
           memcmp(input + start + marker_len + 1, name, name_len) == 0;
}

static size_t body_end(const char *input, size_t start, size_t end) {
    if (end > start && input[end - 1] == '\n') end--;
    if (end > start && input[end - 1] == '\r') end--;
    return end;
}

static bool only_line_endings(const char *input, size_t start, size_t len) {
    for (size_t i = start; i < len; i++)
        if (input[i] != '\r' && input[i] != '\n') return false;
    return true;
}

static bool parse_fence_payload(const char *input, size_t len, const char *marker,
                                tny_edit_payload *payload, char *err, size_t errlen) {
    if (!len || memchr(input, '\0', len)) {
        snprintf(err, errlen, "fence input is empty or contains a NUL byte");
        return false;
    }
    size_t position = 0;
    size_t start = 0;
    size_t line_len = 0;
    if (!next_line(input, len, &position, &start, &line_len) ||
        !marker_line(input, start, line_len, marker, "SEARCH")) {
        snprintf(err, errlen, "expected '%s SEARCH' as the first line", marker);
        return false;
    }
    size_t old_start = position;
    size_t old_end = SIZE_MAX;
    while (next_line(input, len, &position, &start, &line_len)) {
        if (marker_line(input, start, line_len, marker, "REPLACE")) {
            old_end = body_end(input, old_start, start);
            break;
        }
    }
    if (old_end == SIZE_MAX) {
        snprintf(err, errlen, "missing '%s REPLACE' line", marker);
        return false;
    }
    size_t new_start = position;
    size_t new_end = SIZE_MAX;
    while (next_line(input, len, &position, &start, &line_len)) {
        if (marker_line(input, start, line_len, marker, "END")) {
            new_end = body_end(input, new_start, start);
            break;
        }
    }
    if (new_end == SIZE_MAX) {
        snprintf(err, errlen, "missing '%s END' line", marker);
        return false;
    }
    if (!only_line_endings(input, position, len)) {
        snprintf(err, errlen, "unexpected data after '%s END'", marker);
        return false;
    }
    if (old_end == old_start) {
        snprintf(err, errlen, "SEARCH block may not be empty");
        return false;
    }
    payload->old_text = xstrndup(input + old_start, old_end - old_start);
    payload->new_text = xstrndup(input + new_start, new_end - new_start);
    if (!payload->old_text || !payload->new_text) {
        tny_edit_payload_free(payload);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    return true;
}

bool tny_edit_parse_payload(const char *input, size_t len, bool json, const char *marker,
                            tny_edit_payload *payload, char *err, size_t errlen) {
    if (!payload || !err || !errlen) return false;
    memset(payload, 0, sizeof *payload);
    if (!input) {
        snprintf(err, errlen, "could not read stdin");
        return false;
    }
    if (json) return parse_json_payload(input, len, payload, err, errlen);
    return parse_fence_payload(input, len, marker && *marker ? marker : "***", payload, err,
                               errlen);
}

void tny_edit_usage(const char *message, buf_t *err) {
    if (!err) return;
    if (message) buf_appendf(err, "tny: edit: %s\n", message);
    buf_appendf(err, "Example: printf '%s' | tny edit FILE\n",
                "*** SEARCH\\nold\\n*** REPLACE\\nnew\\n*** END\\n");
}

int tny_edit_render(const char *path, bool json, tny_edit_status status,
                    const tny_edit_result *result, buf_t *out, buf_t *err) {
    if (!path || !result || !out || !err) return 1;
    switch (status) {
    case TNY_EDIT_INTERRUPTED: return 130;
    case TNY_EDIT_NOT_FOUND:
        buf_appendf(err, "tny: edit: 0 matches in %s\n", path);
        if (result->nearest_context)
            buf_appendf(err, "tny: edit: nearest unique context is line %zu: %s\n",
                        result->nearest_line, result->nearest_context);
        return 2;
    case TNY_EDIT_AMBIGUOUS:
        buf_appendf(err, "tny: edit: %zu matches in %s; widen SEARCH or use JSON replace_all\n",
                    result->matches, path);
        return 2;
    case TNY_EDIT_OK:
        if (json) {
            buf_appends(out, "{\"kind\":\"edit\",\"path\":");
            jescape(out, path);
            buf_appendf(out, ",\"matches\":%zu,\"replaced\":%zu}\n", result->matches,
                        result->replaced);
        } else {
            buf_appendf(out, "edited %s: replaced %zu occurrence%s\n", path, result->replaced,
                        result->replaced == 1 ? "" : "s");
        }
        return buf_oom(out) ? 1 : 0;
    default: break;
    }
    buf_appendf(err, "tny: edit: cannot %s %s: %s\n",
                status == TNY_EDIT_WRITE_ERROR ? "write" : "read", path,
                result->error_number ? strerror(result->error_number) : "out of memory");
    return 1;
}
