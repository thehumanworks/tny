/* cmd_edit.c — stateless exact-match file editing (ADR 0064). */
#include "cli/cli.h"
#include "core/edit.h"
#include "json/json.h"
#include "util/util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *old_text;
    char *new_text;
    bool replace_all;
} edit_payload;

static volatile sig_atomic_t edit_interrupted;

static void edit_on_interrupt(int signal_number) {
    (void)signal_number;
    edit_interrupted = 1;
}

static bool edit_was_interrupted(void *userdata) {
    (void)userdata;
    return edit_interrupted != 0;
}

static void payload_free(edit_payload *payload) {
    free(payload->old_text);
    free(payload->new_text);
    memset(payload, 0, sizeof *payload);
}

static int read_stdin(buf_t *input) {
    char chunk[8192];
    while (!edit_interrupted) {
        size_t n = fread(chunk, 1, sizeof chunk, stdin);
        if (n) buf_append(input, chunk, n);
        if (buf_oom(input)) return -1;
        if (n < sizeof chunk) {
            if (ferror(stdin)) {
                if (edit_interrupted || errno == EINTR) return -2;
                return -1;
            }
            break;
        }
    }
    return edit_interrupted ? -2 : 0;
}

static bool json_field_known(const char *name) {
    return strcmp(name, "old") == 0 || strcmp(name, "new") == 0 || strcmp(name, "replace_all") == 0;
}

static bool parse_json_payload(const char *input, size_t len, edit_payload *payload, char *err,
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
        payload_free(payload);
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
                                edit_payload *payload, char *err, size_t errlen) {
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
        payload_free(payload);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    return true;
}

static int edit_usage(const char *message) {
    if (message) fprintf(stderr, "tny: edit: %s\n", message);
    fprintf(stderr, "Example: printf '%s' | tny edit FILE\n",
            "*** SEARCH\\nold\\n*** REPLACE\\nnew\\n*** END\\n");
    return 1;
}

int cmd_edit(const cli_globals *g, int argc, char **argv) {
    bool json = g->json;
    const char *marker = "***";
    const char *path = NULL;
    bool positional = false;
    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        if (!positional && strcmp(arg, "--") == 0) {
            positional = true;
        } else if (!positional && strcmp(arg, "--json") == 0) {
            json = true;
        } else if (!positional && strcmp(arg, "--marker") == 0) {
            if (++i >= argc) return edit_usage("--marker requires a value");
            marker = argv[i];
        } else if (!positional && (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0)) {
            help_for("edit");
            return 0;
        } else if (!positional && arg[0] == '-') {
            char message[256];
            snprintf(message, sizeof message, "unknown option '%s'", arg);
            return edit_usage(message);
        } else if (path) {
            return edit_usage("expected exactly one FILE");
        } else {
            path = arg;
        }
    }
    if (!path) return edit_usage("missing FILE");
    if (!*marker || strchr(marker, '\n') || strchr(marker, '\r'))
        return edit_usage("--marker must be a non-empty single-line string");
    if (g->ssh) return edit_usage("--ssh is not supported by the standalone edit verb");

    struct sigaction action = {0};
    struct sigaction previous = {0};
    action.sa_handler = edit_on_interrupt;
    sigemptyset(&action.sa_mask);
    edit_interrupted = 0;
    sigaction(SIGINT, &action, &previous);

    buf_t input;
    buf_init(&input);
    int read_status = read_stdin(&input);
    if (read_status != 0) {
        buf_free(&input);
        sigaction(SIGINT, &previous, NULL);
        if (read_status == -2) return 130;
        return edit_usage("could not read stdin");
    }

    edit_payload payload = {0};
    char parse_error[256];
    bool parsed =
        json ? parse_json_payload(input.data, input.len, &payload, parse_error, sizeof parse_error)
             : parse_fence_payload(input.data, input.len, marker, &payload, parse_error,
                                   sizeof parse_error);
    buf_free(&input);
    if (!parsed) {
        sigaction(SIGINT, &previous, NULL);
        return edit_usage(parse_error);
    }
    if (edit_interrupted) {
        payload_free(&payload);
        sigaction(SIGINT, &previous, NULL);
        return 130;
    }

    buf_t json_output;
    buf_init(&json_output);
    if (json) {
        buf_appends(&json_output, "{\"kind\":\"edit\",\"path\":");
        jescape(&json_output, path);
        /* Enough for both size_t values, punctuation, newline, and NUL. All
         * output allocation therefore succeeds before the file can change. */
        buf_reserve(&json_output, 96);
        if (buf_oom(&json_output)) {
            payload_free(&payload);
            sigaction(SIGINT, &previous, NULL);
            buf_free(&json_output);
            return edit_usage("out of memory while preparing JSON output");
        }
    }

    tny_edit_result result = {0};
    tny_edit_hooks hooks = {.interrupted = edit_was_interrupted};
    tny_edit_status status = tny_edit_file_exact(path, payload.old_text, payload.new_text,
                                                 payload.replace_all, &hooks, &result);
    payload_free(&payload);
    sigaction(SIGINT, &previous, NULL);
    if (status == TNY_EDIT_INTERRUPTED) {
        buf_free(&json_output);
        tny_edit_result_free(&result);
        return 130;
    }
    if (status == TNY_EDIT_NOT_FOUND) {
        buf_free(&json_output);
        fprintf(stderr, "tny: edit: 0 matches in %s\n", path);
        if (result.nearest_context)
            fprintf(stderr, "tny: edit: nearest unique context is line %zu: %s\n",
                    result.nearest_line, result.nearest_context);
        tny_edit_result_free(&result);
        return 2;
    }
    if (status == TNY_EDIT_AMBIGUOUS) {
        buf_free(&json_output);
        fprintf(stderr, "tny: edit: %zu matches in %s; widen SEARCH or use JSON replace_all\n",
                result.matches, path);
        return 2;
    }
    if (status != TNY_EDIT_OK) {
        buf_free(&json_output);
        const char *operation = status == TNY_EDIT_WRITE_ERROR ? "write" : "read";
        const char *detail = result.error_number ? strerror(result.error_number) : "out of memory";
        fprintf(stderr, "tny: edit: cannot %s %s: %s\n", operation, path, detail);
        tny_edit_result_free(&result);
        return 1;
    }
    if (json) {
        buf_appendf(&json_output, ",\"matches\":%zu,\"replaced\":%zu}\n", result.matches,
                    result.replaced);
        char *line = buf_detach(&json_output);
        fputs(line, stdout);
        free(line);
    } else {
        printf("edited %s: replaced %zu occurrence%s\n", path, result.replaced,
               result.replaced == 1 ? "" : "s");
    }
    tny_edit_result_free(&result);
    return 0;
}
