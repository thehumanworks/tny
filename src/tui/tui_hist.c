/* tui_hist.c — prompt history. Persistent runs use ~/.tny/history; ephemeral
 * runs retain only prompts entered during the current process. Multi-line
 * prompts are escaped so one persistent entry stays one line on disk. */
#include "tui/tui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static char *hist_path(void) {
    char *dir = path_tny_dir();
    char *p = path_join(dir, "history");
    free(dir);
    return p;
}

/* Consume one owned prompt, preserving retained entries if allocation fails.
 * The disk log remains append-only. */
static bool hist_retain(tui *t, char *line) {
    if (!line) return false;
    if (t->n_hist < TUI_MAX_HIST) {
        char **grown = realloc(t->hist, sizeof(char *) * (size_t)(t->n_hist + 1));
        if (!grown) {
            free(line);
            return false;
        }
        t->hist = grown;
    } else {
        free(t->hist[0]);
        memmove(t->hist, t->hist + 1, sizeof(char *) * (size_t)(TUI_MAX_HIST - 1));
        t->n_hist--;
    }
    t->hist[t->n_hist++] = line;
    t->hist_pos = t->n_hist;
    return true;
}

void tui_hist_load(tui *t) {
    if (t->ctx->no_save) {
        tui_sys(t,
                "ephemeral mode · session, recovery, results, and prompt history stay in memory");
        return;
    }
    char *p = hist_path();
    FILE *f = fopen(p, "r");
    free(p);
    if (!f) return;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) >= 0) {
        if (len && line[len - 1] == '\n') line[len - 1] = '\0';
        if (!*line) continue;
        /* Decode in place: escaping never makes the prompt shorter on disk. */
        char *dst = line;
        for (const char *src = line; *src; src++) {
            if (src[0] == '\\' && src[1] == 'n') {
                *dst++ = '\n';
                src++;
            } else if (src[0] == '\\' && src[1] == '\\') {
                *dst++ = '\\';
                src++;
            } else *dst++ = *src;
        }
        *dst = '\0';
        (void)hist_retain(t, line);
        line = NULL;
        cap = 0;
    }
    free(line);
    fclose(f);
    t->hist_pos = t->n_hist;
}

void tui_hist_add(tui *t, const char *line) {
    if (!line || !*line) return;
    if (t->n_hist && strcmp(t->hist[t->n_hist - 1], line) == 0) {
        t->hist_pos = t->n_hist;
        return;
    }
    if (!hist_retain(t, xstrdup(line))) return;
    line = t->hist[t->n_hist - 1];

    if (t->ctx->no_save || t->session_readonly || t->background_view) return;
    char *dir = path_tny_dir();
    mkdir_p(dir);
    free(dir);
    char *p = hist_path();
    FILE *f = fopen(p, "a");
    free(p);
    if (!f) return;
    for (const char *q = line; *q; q++) {
        if (*q == '\n') fputs("\\n", f);
        else if (*q == '\\') fputs("\\\\", f);
        else fputc(*q, f);
    }
    fputc('\n', f);
    fclose(f);
}

void tui_hist_free(tui *t) {
    for (int i = 0; i < t->n_hist; i++) free(t->hist[i]);
    free(t->hist);
    t->hist = NULL;
    t->n_hist = 0;
    free(t->hist_draft);
    t->hist_draft = NULL;
}
