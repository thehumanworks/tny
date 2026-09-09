#include "tui/tui.h"

#include <stdlib.h>
#include <string.h>

void tui_optimise_start(tui *t, const char *arg) {
    if (!t->tty || t->approval || t->wiz_step || t->turn_active || t->dictation || t->optimise) {
        tui_err(t, "optimisation needs an idle interactive prompt; use tny optimise for scripts");
        return;
    }
    char *copy = xstrdup(arg ? arg : t->input.len ? t->input.data : "");
    if (!copy) return;
    char *prompt = copy;
    tny_optimise_request r = {0};
    /* IDs have no whitespace. Options precede the draft; -- escapes a
     * draft that itself starts with an option. The rest remains verbatim. */
    if (arg) {
        while (str_starts(prompt, "--")) {
            char *end = prompt + strcspn(prompt, " \t\r\n");
            if (*end) *end++ = 0;
            end += strspn(end, " \t\r\n");
            if (strcmp(prompt, "--") == 0) {
                prompt = end;
                break;
            }
            const char **slot = strcmp(prompt, "--model") == 0      ? &r.model
                                : strcmp(prompt, "--provider") == 0 ? &r.provider
                                                                    : NULL;
            if (!slot || !*end) goto invalid;
            *slot = end;
            prompt = end + strcspn(end, " \t\r\n");
            if (*prompt) *prompt++ = 0;
            prompt += strspn(prompt, " \t\r\n");
        }
    }
    r.text = prompt;
    if (!tny_dictation_text_valid(prompt, strlen(prompt))) {
        tui_err(t, "type or dictate a prompt first (maximum 64 KiB)");
        free(copy);
        return;
    }
    if (arg) {
        buf_t draft = {0};
        buf_appends(&draft, prompt);
        if (draft.oom) {
            buf_free(&draft);
            free(copy);
            return;
        }
        buf_free(&t->input);
        t->input = draft;
        t->cur = t->input.len;
    }
    tui_pick_close(t);
    tui_overlay_clear(t);
    tui_note(t, "Optimising prompt… Esc cancels");
    tui_render_force(t);
    char error[256] = "";
    t->optimise = tny_optimise_start(t->ctx, &r, error, sizeof error);
    if (!t->optimise) {
        buf_clear(&t->note);
        tui_err(t, *error ? error : "cannot start prompt optimisation");
    } else tui_note(t, "Optimising · %s · Esc cancels", tny_optimise_model(t->optimise));
    t->dirty = true;
    free(copy);
    return;
invalid:
    tui_err(t, "usage: /optimise [--provider NAME] [--model MODEL] [--] PROMPT");
    free(copy);
}

void tui_optimise_step(tui *t) {
    if (!t->optimise) return;
    tny_optimise_step(t->optimise);
    const char *text, *error;
    int rc = tny_optimise_result(t->optimise, &text, &error);
    if (rc < 0) return;
    if (!rc) {
        buf_t draft = {0};
        buf_appends(&draft, text);
        if (!draft.oom) {
            buf_free(&t->input);
            t->input = draft;
            t->cur = t->input.len;
            t->hist_pos = t->n_hist;
            tui_note(t, "Prompt optimised · edit or Enter to send");
        } else {
            buf_free(&draft);
            tui_note(t, "Cannot insert optimised prompt · draft unchanged");
        }
    } else if (rc == 130) tui_note(t, "Optimisation cancelled · draft unchanged");
    else {
        tui_note(t, "Optimisation failed · draft unchanged");
        tui_err(t, error);
    }
    tny_optimise_free(t->optimise);
    t->optimise = NULL;
    t->dirty = true;
}
