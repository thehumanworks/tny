/* edit.h — exact-match, atomic file replacement shared by tools and CLI. */
#ifndef TNY_EDIT_H
#define TNY_EDIT_H

#include "util/util.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    TNY_EDIT_OK = 0,
    TNY_EDIT_NOT_FOUND,
    TNY_EDIT_AMBIGUOUS,
    TNY_EDIT_READ_ERROR,
    TNY_EDIT_WRITE_ERROR,
    TNY_EDIT_NOMEM,
    TNY_EDIT_INTERRUPTED,
} tny_edit_status;

typedef struct {
    size_t matches;
    size_t replaced;
    char *nearest_context; /* malloc'd unique closest line, or NULL */
    size_t nearest_line;   /* one-based; zero when nearest_context is NULL */
    int error_number;      /* errno captured for read/write failures */
} tny_edit_result;

typedef void (*tny_edit_before_write_fn)(const char *resolved_path, void *userdata);
typedef bool (*tny_edit_interrupted_fn)(void *userdata);

typedef struct {
    tny_edit_before_write_fn before_write;
    void *before_write_userdata;
    tny_edit_interrupted_fn interrupted;
    void *interrupted_userdata;
} tny_edit_hooks;

/* Replace old_text exactly once, or every non-overlapping occurrence when
 * replace_all is true. The destination is untouched unless the match policy,
 * allocation, and complete replacement construction all succeed. Existing
 * symlinks are resolved so the target is edited without replacing the link.
 * hooks->before_write runs after all fallible preparation and immediately
 * before the atomic temp-file write; the file tool uses it to record undo
 * state. hooks->interrupted aborts before the atomic rename commit point. */
tny_edit_status tny_edit_file_exact(const char *path, const char *old_text, const char *new_text,
                                    bool replace_all, const tny_edit_hooks *hooks,
                                    tny_edit_result *result);

void tny_edit_result_free(tny_edit_result *result);

/* ---- the `tny edit` verb contract (docs/adr/0064), shared by the CLI in
 * src/cli/cmd_edit.c and the in-process intercept (docs/adr/0063) so both
 * accept the same stdin and print the same bytes. ---- */

typedef struct {
    char *old_text;
    char *new_text;
    bool replace_all;
} tny_edit_payload;

/* Parse one stdin payload: a single JSON object when json is true, else the
 * marker fence. Returns false with a one-line reason in err. */
bool tny_edit_parse_payload(const char *input, size_t len, bool json, const char *marker,
                            tny_edit_payload *payload, char *err, size_t errlen);
void tny_edit_payload_free(tny_edit_payload *payload);

/* The verb's usage diagnostic: the caller's message (when any) followed by
 * the example line, both as the CLI writes them to stderr. */
void tny_edit_usage(const char *message, buf_t *err);

/* Append what the verb prints on stdout to out and on stderr to err, and
 * return its exit code. TNY_EDIT_INTERRUPTED prints nothing and returns 130. */
int tny_edit_render(const char *path, bool json, tny_edit_status status,
                    const tny_edit_result *result, buf_t *out, buf_t *err);

#endif
