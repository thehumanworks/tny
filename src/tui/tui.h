/* tui.h — internal state for the interactive shell (docs/tui.md).
 * Layout is a scrolling transcript plus a redrawn bottom block:
 *   [committed transcript]  <- scrolls with the terminal
 *   [partial line][popover][status][composer]  <- erased and redrawn
 * Every block row is clamped to cols-1 columns so the terminal never
 * soft-wraps; the composer wraps itself onto extra rows. */
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

typedef struct tui_prewarm tui_prewarm; /* tui_prewarm.c */

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
    tui_prewarm *prewarm;   /* host warm-up running in the background */
    bool bk_adopted;        /* t->bk came pre-resumed from the warm-up and has
                             * not sent yet: one lazy retry if send() fails */

    bool turn_active, turn_done, want_cancel, quit, trace;
    bool in_thinking; /* streaming reasoning: keep it on its own lines */
    int  exit_code;
    tny_stop_reason stop;
    int64_t in_tok, out_tok, cancel_ms, last_ctrlc_ms;
    int     spin;      /* status-row spinner frame while a turn runs */
    int64_t spin_ms;   /* last frame advance */

    buf_t overlay;      /* transient menu block ('\n' lines, SGR allowed):
                         * drawn above the status row, dropped after the
                         * interaction ends — never enters the transcript */
    buf_t note;         /* transient status-line note */
    buf_t last_reply;   /* last assistant text, for /copy */
    buf_t prompt_text;  /* prompt that started the active turn */

    char *perm_id;      /* host approval awaiting an answer */
    int   perm_opts;
    bool  approval;     /* an approval owns the keyboard */

    char *images[TUI_MAX_IMAGES + 1];
    int   n_images;

    /* Transcript gap: one blank line before the next agent start. */
    int gap; /* 0 none, 1 before text or tools, 2 before text only */
} tui;

/* Key decoder (split-safe). Exposed for unit tests. */
typedef enum {
    TUI_K_NONE = 0, TUI_K_CHAR, TUI_K_ENTER, TUI_K_NEWLINE, TUI_K_BS, TUI_K_DEL,
    TUI_K_LEFT, TUI_K_RIGHT, TUI_K_UP, TUI_K_DOWN, TUI_K_HOME, TUI_K_END,
    TUI_K_WLEFT, TUI_K_WRIGHT, TUI_K_WBS, TUI_K_KILL_EOL, TUI_K_KILL_BOL,
    TUI_K_ESC, TUI_K_TAB, TUI_K_CTRLC, TUI_K_CTRLD, TUI_K_CTRLL, TUI_K_CTRLO,
    TUI_K_CTRLX, TUI_K_PASTE
} tui_key;

typedef struct {
    tui_key     key;
    const char *ch;
    size_t      chlen;
} tui_decoded;

/* Consume one key from p[0..n). 0 if more bytes are needed. */
size_t tui_decode_one(const char *p, size_t n, bool final, tui_decoded *out);

/* Composer wrap math. width is display columns after the "> " / "  " prefix. */
void   tui_wrap_locate(const char *s, size_t n, size_t cur, int width,
                       int *row, int *col, int *total);
size_t tui_wrap_index(const char *s, size_t n, int width, int row, int col);
int    tui_wrap_width(const tui *t);

/* Queue an image path for the next prompt. Returns 1-based index, or 0. */
int tui_queue_image(tui *t, const char *path);

/* tui.c */
void tui_submit(tui *t, const char *text);
void tui_cancel_turn(tui *t);
void tui_new_session(tui *t, bool clear_screen);
tny_perm_decision tui_ask_perm(tui *t, const char *tool, const char *summary);
void tui_drop_backend(tui *t);  /* disconnect + destroy the bound backend */

/* tui_prewarm.c — spawn + connect + create/resume the provider's host in the
 * background so the first turn pays neither the startup nor the session
 * round trip (docs/adr/0002). */
void tui_prewarm_start(tui *t);          /* warm ctx->backend if it applies */
tny_backend *tui_prewarm_take(tui *t);   /* resumed backend or NULL; consumes */
/* Abandon whatever is pending. Waits out an in-flight create_or_resume, so
 * ctx fields it reads (model, tier, workspace dirs) are safe to mutate the
 * moment this returns. */
void tui_prewarm_drop(tui *t);
bool tui_prewarm_applicable(const struct tny_ctx *ctx, int backend_id);
/* Internal seam, exposed for the unit tests: adopt an already-created
 * backend and run its connect() + create_or_resume() on the pre-warm
 * thread. resume_pointer may be NULL (new session). */
int tui_prewarm_launch(tui *t, tny_backend *bk, int backend_id,
                       const char *resume_pointer);

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
void tui_overlay_linef(tui *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void tui_overlay_clear(tui *t);
/* Append at most maxw columns of s; SGR escapes pass through at zero width,
 * other control bytes are dropped. Returns display columns written.
 * Exposed for the unit tests. */
int tui_push_ansi(buf_t *b, const char *s, size_t n, int maxw);
/* Rows the overlay may use given everything else in the block. Exposed for
 * the unit tests. */
int tui_overlay_budget(const tui *t);

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
