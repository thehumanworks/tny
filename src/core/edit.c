/* edit.c — exact-match, atomic file replacement shared by tools and CLI. */
#include "core/edit.h"
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
