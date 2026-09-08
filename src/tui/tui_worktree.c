#include "tui/tui.h"
#include "mcp/mcp.h"
#include "util/tny_poll.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

void tui_worktree_enter(tui *t, const char *name) {
    if (t->ctx->ssh_host || t->turn_active || t->dictation) {
        tui_err(t, "worktree entry requires an idle local workspace; disconnect /ssh first");
        return;
    }
    if (name && !*name) name = NULL;
    tny_worktree *w = t->worktrees;
    while (w && (!name || strcmp(name, w->name) != 0)) w = w->next;
    if (w == t->worktree && w) {
        tui_sysf(t, "already in worktree %s", w->path);
        return;
    }
    bool owned = w == NULL;
    char err[1024];
    if (!w) w = worktree_enter(t->ctx->cwd, name, err, sizeof err);
    if (!w) {
        tui_err(t, err);
        return;
    }
    /* A fresh context reloads project limits, tasks, extensions and workspace
     * grants at the new root. Keep the user's current provider selections. */
    bool had_runner = t->rc != NULL;
    pid_t runner_pid = t->rc_pid;
    tui_prewarm_drop(t);
    if (!tui_worktree_wait_runner(runner_pid)) {
        tui_err(t, "previous session runner is still stopping; worktree kept, retry entry later");
        if (owned) worktree_close(w);
        return;
    }
    cli_globals next = *t->g;
    next.cwd = w->path;
    next.ssh = NULL; /* /ssh off must also clear the original launch flags */
    next.ssh_cwd = NULL;
    next.backend = tny_provider_name(t->ctx);
    next.model = t->ctx->model;
    next.effort = t->ctx->reasoning_effort ? t->ctx->reasoning_effort : "default";
    next.perm_mode = t->ctx->perm_mode == TNY_MODE_ASK    ? "ask"
                     : t->ctx->perm_mode == TNY_MODE_AUTO ? "auto"
                                                          : "yolo";
    next.task = t->ctx->task_name;
    next.fast = false;
    tui_raw_begin(t);
    tny_ctx *ctx = cli_make_ctx(&next);
    tui_raw_end(t);
    if (!ctx) {
        tui_err(t, "cannot load worktree settings; previous workspace retained, worktree kept");
        if (owned) worktree_close(w);
        tui_prewarm_start(t);
        return;
    }
    ctx->model_from_flag = t->ctx->model_from_flag;
    ctx->effort_explicit = t->ctx->effort_explicit;
    ctx->effort_from_settings = t->ctx->effort_from_settings;
    if (t->ctx->service_tier_explicit) {
        free(ctx->service_tier);
        ctx->service_tier = xstrdup(t->ctx->service_tier);
        ctx->service_tier_explicit = true;
        ctx->service_tier_from_settings = false;
    }
    if (t->engine) tny_engine_end_session(t->engine, "worktree");
    tui_drop_backend(t);
    if (t->session) {
        /* prewarm_drop already ended the sole runner writer, if any. The
         * replica must never be saved over its final session snapshot. */
        if (!had_runner) session_save(t->session);
        session_close(t->session);
        t->session = NULL;
    }
    mcp_shutdown_all();
    perm_free(t->perm);
    if (t->owns_ctx) tny_ctx_free(t->ctx);
    t->ctx = ctx;
    t->owns_ctx = true;
    t->perm = perm_new(ctx);
    if (t->worktree) tui_sysf(t, "kept previous worktree %s", t->worktree->path);
    if (owned) {
        w->next = t->worktrees;
        t->worktrees = w;
    }
    t->worktree = w;
    tui_files_free(t);
    tui_items_clear(t);
    t->pick = PICK_NONE;
    tui_queue_clear(t);
    for (int i = 0; i < t->n_images; i++) free(t->images[i]);
    t->n_images = 0;
    t->in_tok = t->out_tok = 0;
    buf_clear(&t->last_reply);
    buf_clear(&t->prompt_text);
    tui_sysf(t, "%s worktree %s (%s); new session", owned && w->created ? "created" : "entered",
             w->path, w->branch);
    tui_prewarm_start(t);
    t->dirty = true;
}

bool tui_worktree_wait_runner(pid_t pid) {
    if (pid <= 0) return true;
    int64_t deadline = monotonic_ms() + 5000;
    do {
        pid_t got = waitpid(pid, NULL, WNOHANG);
        if (got == pid || (got < 0 && errno == ECHILD)) return true;
        if (got < 0 && errno != EINTR) return false;
        tny_poll(NULL, 0, 20);
    } while (monotonic_ms() < deadline);
    return false;
}

void tui_worktree_finish(tui *t, bool stopped) {
    tny_worktree *w = t->worktree;
    if (!w) return;
    unsigned char choice = 'k';
    if (t->tty && stopped) {
        /* Still raw: Enter, Esc, Ctrl-C/D, EOF and any unrecognized answer
         * all keep. Discard bytes typed before this prompt was displayed. */
        tcflush(STDIN_FILENO, TCIFLUSH);
        printf("\nWorktree: %s\n", w->path);
        if (*w->origin_ref)
            printf("Merge commits into %s at %s (keeps worktree).\n", w->origin_ref, w->origin);
        else puts("Origin had a detached HEAD; merging requires a manual destination.");
        fputs("On exit: [m]erge, [r]emove directory (keep branch), [K]eep (default): ", stdout);
        fflush(stdout);
        struct pollfd fd = {STDIN_FILENO, POLLIN, 0};
        if (tny_poll(&fd, 1, -1) > 0) {
            if (read(STDIN_FILENO, &choice, 1) != 1) choice = 'k';
        }
        putchar('\n');
    } else if (!stopped) {
        puts("Session runner is still stopping; worktree kept.");
    }
    char err[2048];
    int rc = 0;
    if (choice == 'm' || choice == 'M') {
        rc = worktree_merge(w, err, sizeof err);
        if (!rc) printf("Merged into %s; worktree kept at %s\n", w->origin_ref, w->path);
    } else if (choice == 'r' || choice == 'R') {
        rc = worktree_remove(w, err, sizeof err);
        if (!rc) printf("Removed worktree %s; branch %s kept\n", w->path, w->branch);
    } else printf("Kept worktree %s\n", w->path);
    if (rc) {
        fprintf(stderr, "tny: %s\nWorktree kept at %s\n", err, w->path);
        if (!t->exit_code) t->exit_code = 1;
    }
}
