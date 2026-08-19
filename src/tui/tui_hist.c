/* tui_hist.c — persistent prompt history (~/.tny/history). Multi-line prompts
 * are stored with escaped newlines so one entry stays one line on disk. */
#include "tui/tui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *hist_path(void) {
    char *dir = path_tny_dir();
    char *p = path_join(dir, "history");
    free(dir);
    return p;
}

void tui_hist_load(tui *t) {
    char *p = hist_path();
    size_t len = 0;
    char *data = file_slurp(p, &len);
    free(p);
    if (!data) return;
    char *line = data;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || data[i] == '\n') {
            data[i] = 0;
            if (*line) {
                /* unescape the "\n" we wrote for multi-line prompts */
                buf_t b;
                buf_init(&b);
                for (char *q = line; *q; q++) {
                    if (q[0] == '\\' && q[1] == 'n') { buf_appends(&b, "\n"); q++; }
                    else if (q[0] == '\\' && q[1] == '\\') { buf_appends(&b, "\\"); q++; }
                    else buf_append(&b, q, 1);
                }
                t->hist = realloc(t->hist, sizeof(char *) * (size_t)(t->n_hist + 1));
                t->hist[t->n_hist++] = buf_detach(&b);
            }
            line = data + i + 1;
        }
    }
    free(data);
    if (t->n_hist > TUI_MAX_HIST) {
        int drop = t->n_hist - TUI_MAX_HIST;
        for (int i = 0; i < drop; i++) free(t->hist[i]);
        memmove(t->hist, t->hist + drop, sizeof(char *) * (size_t)TUI_MAX_HIST);
        t->n_hist = TUI_MAX_HIST;
    }
    t->hist_pos = t->n_hist;
}

void tui_hist_add(tui *t, const char *line) {
    if (!line || !*line) return;
    if (t->n_hist && strcmp(t->hist[t->n_hist - 1], line) == 0) {
        t->hist_pos = t->n_hist;
        return;
    }
    t->hist = realloc(t->hist, sizeof(char *) * (size_t)(t->n_hist + 1));
    t->hist[t->n_hist++] = xstrdup(line);
    t->hist_pos = t->n_hist;

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

