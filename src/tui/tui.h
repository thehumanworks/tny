/* tui.h — internal state for the interactive shell (docs/tui.md).
 * Layout is a scrolling transcript plus a redrawn bottom block:
 *   [committed transcript]  <- scrolls with the terminal
 *   [partial line][popover][status][composer]  <- erased and redrawn
 * Every block row is clamped to cols-1 columns so no row ever wraps and the
 * row arithmetic stays exact. */
#ifndef TNY_TUI_H
#define TNY_TUI_H

#include "cli/cli.h"
#include "core/backend.h"
#include "core/perm.h"
#include "core/session.h"
#include "util/util.h"

#include <stdbool.h>
#include <stdint.h>

#define TUI_POP_ROWS    8
#define TUI_COMP_ROWS   8
#define TUI_MAX_IMAGES  8
#define TUI_MAX_FILES   6000
#define TUI_MAX_HIST    500

typedef enum { PICK_NONE = 0, PICK_CMD, PICK_FILE, PICK_SKILL } pick_kind;

typedef struct { char *label, *hint; } pick_item;

typedef struct tui {
    tny_ctx           *ctx;
    const cli_globals *g;

    bool tty, color;
    int  rows, cols;
    int  block_rows, cur_row;

    buf_t out;      /* committed transcript bytes not yet written */
    buf_t partial;  /* transcript line still being streamed */
    bool  dirty;

    buf_t  input;   /* composer, '\n' separates continuation lines */
    size_t cur;     /* byte offset of the caret in input */

    char **hist;
    int    n_hist, hist_pos;
    char  *hist_draft;

    pick_kind  pick;
    pick_item *items;
    int        n_items, sel;
    size_t     pick_at;   /* byte index of the trigger char in input */

    char **files;         /* workspace file cache for the @ picker */
    int    n_files;
    bool   files_scanned;

    tny_backend *bk;
    tny_session *session;
    perm_engine *perm;

    bool turn_active, turn_done, want_cancel, quit, trace;
    int  exit_code;
    tny_stop_reason stop;
    int64_t in_tok, out_tok, cancel_ms, last_ctrlc_ms;

    buf_t note;         /* transient status-line note */
    buf_t last_reply;   /* last assistant text, for /copy */
    buf_t prompt_text;  /* prompt that started the active turn */

    char *perm_id;      /* host approval awaiting an answer */
    int   perm_opts;
    bool  approval;     /* an approval owns the keyboard */

    char *images[TUI_MAX_IMAGES + 1];
    int   n_images;
} tui;

/* tui.c */
void tui_submit(tui *t, const char *text);
void tui_cancel_turn(tui *t);
void tui_new_session(tui *t, bool clear_screen);
tny_perm_decision tui_ask_perm(tui *t, const char *tool, const char *summary);

/* tui_draw.c */
void tui_size(tui *t);
void tui_render(tui *t);
void tui_render_force(tui *t);
void tui_raw_begin(tui *t);  /* drop the block so plain printf output scrolls */
void tui_raw_end(tui *t);
void tui_write(tui *t, const char *s, size_t n);
void tui_bol(tui *t);        /* finish the current transcript line */
void tui_linef(tui *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void tui_sys(tui *t, const char *s);   /* dim system line */
void tui_err(tui *t, const char *s);   /* red error line */
const char *tui_c(const tui *t, const char *code);
void tui_note(tui *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* tui_input.c */
void tui_hist_load(tui *t);
void tui_hist_add(tui *t, const char *line);
void tui_hist_free(tui *t);
int  tui_read_input(tui *t);  /* -1 on EOF */
void tui_pick_close(tui *t);
void tui_pick_refresh(tui *t);

/* tui_commands.c */
void tui_command(tui *t, const char *line);
void tui_items_clear(tui *t);
void tui_items_add(tui *t, const char *label, const char *hint);
void tui_pick_build_cmd(tui *t, const char *filter);
void tui_pick_build_file(tui *t, const char *filter);
void tui_pick_build_skill(tui *t, const char *filter);
void tui_files_free(tui *t);

#endif
