/* Dictation display adapter. Core owns recording and transcription;
 * only this frontend decides when text enters the editable composer. */
#include "tui/tui.h"
#include <string.h>

void tui_dictation_start(tui *t, const char *provider) {
    if (t->session_readonly || t->background_view) {
        tui_err(t, "read-only session replica: submit a prompt or use /continue for execution");
        return;
    }
    if (t->dictation) {
        tny_dictation_finish(t->dictation);
        return;
    }
    if (!t->tty || t->approval || t->wiz_step) {
        tui_err(t, "dictation needs the interactive prompt; use tny dictate for scripts");
        return;
    }
    char err[256];
    tny_dictation_request r = {.provider = provider};
    t->dictation = tny_dictation_start(t->ctx, &r, err, sizeof err);
    if (!t->dictation) {
        tui_err(t, err);
        return;
    }
    tui_pick_close(t);
    tui_overlay_clear(t);
    tui_note(t, "Listening… Enter/Ctrl-R transcribes · Esc cancels");
}

void tui_dictation_step(tui *t) {
    if (!t->dictation) return;
    tny_dictation_step(t->dictation);
    tny_dictation_state state = tny_dictation_get_state(t->dictation);
    if (state == TNY_DICTATION_DONE) {
        const char *text, *err;
        int rc = tny_dictation_result(t->dictation, &text, &err);
        if (!rc) {
            tny_dictation_normalization norm;
            bool normalizing = tny_dictation_normalization_info(t->dictation, &norm);
            if (tui_dictation_insert(t, text)) {
                if (!normalizing) tui_note(t, "Dictation ready · Enter sends");
                else if (norm.normalized) tui_note(t, "Dictation ready (normalized) · Enter sends");
                else if (!strcmp(norm.skipped_reason, "cancelled"))
                    tui_note(t, "Normalization cancelled · raw transcript inserted");
                else tui_note(t, "Dictation ready (raw: %s) · Enter sends", norm.skipped_reason);
            } else {
                buf_clear(&t->note);
                tui_err(t, "dictation does not fit in the composer; draft unchanged");
            }
        } else if (rc == 130) tui_note(t, "Dictation cancelled · draft unchanged");
        else {
            buf_clear(&t->note);
            tui_err(t, err);
        }
        tny_dictation_free(t->dictation);
        t->dictation = NULL;
        t->dirty = true;
        return;
    }
    const char *note =
        state == TNY_DICTATION_RECORDING     ? "Listening… Enter/Ctrl-R transcribes · Esc cancels"
        : state == TNY_DICTATION_NORMALIZING ? "Normalizing… Esc inserts the raw transcript"
                                             : "Transcribing… Esc cancels";
    if (!t->note.data || strcmp(note, t->note.data) != 0) tui_note(t, "%s", note);
}
