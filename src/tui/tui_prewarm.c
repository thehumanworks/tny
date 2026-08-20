/* tui_prewarm.c — background host warm-up (docs/adr/0002).
 *
 * The TUI's first prompt used to pay the whole host startup bill: spawn
 * `codex app-server` / `cursor-sdk-bridge` / the ACP agent, wait for its
 * ready signal, run the protocol handshake. Here that work starts the moment
 * the shell paints, on a detached pthread that runs exactly one backend
 * call — connect() — and then parks the connected backend for adoption.
 *
 * Threading contract (keeps the one-event-loop invariant intact):
 *   - the thread touches only the backend it was handed and this struct;
 *   - no events flow before send(), so nothing ever crosses threads mid-turn;
 *   - connect() must not depend on ctx fields the TUI mutates (/model and
 *     /fast ride on create_or_resume, which stays on the main thread);
 *   - whoever sees the handoff completed last cleans up: take() on the main
 *     thread, or the thread itself when the main thread abandoned the warm-up
 *     first (provider switch, shell exit).
 *
 * The CLI (`tny ask`, --help, --version) never calls into this file: one-shot
 * commands keep the lazy-connect startup contract.
 */
#include "tui/tui.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct tui_prewarm {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    tny_backend    *bk;
    int             backend_id;
    bool            done;      /* connect() returned */
    bool            ok;        /* ...and succeeded */
    bool            abandoned; /* main thread gave up: thread owns cleanup */
};

static void prewarm_free(tui_prewarm *p) {
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->cv);
    free(p);
}

static void backend_discard(tny_backend *bk) {
    bk->disconnect(bk);
    bk->destroy(bk);
}

static void *prewarm_main(void *arg) {
    tui_prewarm *p = arg;
    char err[512];
    err[0] = 0;
    int rc = p->bk->connect(p->bk, err, sizeof err);

    pthread_mutex_lock(&p->mu);
    p->ok = rc == 0;
    p->done = true;
    bool abandoned = p->abandoned;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mu);

    if (abandoned) { /* nobody will take it: last owner cleans up */
        backend_discard(p->bk);
        prewarm_free(p);
    }
    return NULL;
}

/* Only hosts with real startup cost. openai is a per-turn HTTPS request, and
 * a provider whose credentials are already known to be missing must fail at
 * the first prompt with today's error, not spawn doomed processes early. */
bool tui_prewarm_applicable(const struct tny_ctx *ctx, int backend_id) {
    switch (backend_id) {
    case TNY_BK_CODEX:
        return true; /* spawn or --codex-ws attach; auth errors surface later */
    case TNY_BK_CURSOR: {
        const char *key = getenv("CURSOR_API_KEY");
        return key && *key;
    }
    case TNY_BK_ACP:
        return ctx->agent_argv && ctx->agent_argv[0];
    default:
        return false;
    }
}

int tui_prewarm_launch(tui *t, tny_backend *bk, int backend_id) {
    tui_prewarm *p = calloc(1, sizeof *p);
    if (!p) { bk->destroy(bk); return -1; }
    p->bk = bk;
    p->backend_id = backend_id;
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);

    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&th, &at, prewarm_main, p);
    pthread_attr_destroy(&at);
    if (rc != 0) { /* no thread: stay lazy, exactly the old behavior */
        bk->destroy(bk);
        prewarm_free(p);
        return -1;
    }
    t->prewarm = p;
    return 0;
}

void tui_prewarm_start(tui *t) {
    if (t->prewarm) {
        if (t->prewarm->backend_id == t->ctx->backend) return; /* already warming */
        tui_prewarm_drop(t);
    }
    if (!tui_prewarm_applicable(t->ctx, t->ctx->backend)) return;
    tny_backend *bk = tny_backend_create((tny_backend_id)t->ctx->backend, t->ctx);
    if (!bk) return;
    tui_prewarm_launch(t, bk, t->ctx->backend);
}

tny_backend *tui_prewarm_take(tui *t) {
    tui_prewarm *p = t->prewarm;
    if (!p) return NULL;
    if (p->backend_id != t->ctx->backend) { /* provider changed under it */
        tui_prewarm_drop(t);
        return NULL;
    }
    t->prewarm = NULL;

    pthread_mutex_lock(&p->mu);
    while (!p->done) /* mid-connect: waiting here costs what lazy connect did */
        pthread_cond_wait(&p->cv, &p->mu);
    bool ok = p->ok;
    tny_backend *bk = p->bk;
    pthread_mutex_unlock(&p->mu);
    prewarm_free(p);

    if (!ok) { /* silent: the caller re-connects synchronously and reports */
        backend_discard(bk);
        return NULL;
    }
    return bk;
}

void tui_prewarm_drop(tui *t) {
    tui_prewarm *p = t->prewarm;
    if (!p) return;
    t->prewarm = NULL;

    pthread_mutex_lock(&p->mu);
    p->abandoned = true;
    bool done = p->done;
    pthread_mutex_unlock(&p->mu);

    if (done) { /* thread already finished and saw abandoned == false */
        backend_discard(p->bk);
        prewarm_free(p);
    }
    /* else: the thread sees abandoned when connect() returns and cleans up */
}
