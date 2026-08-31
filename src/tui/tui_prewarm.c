/* tui_prewarm.c — background host warm-up (docs/adr/0002).
 *
 * The TUI's first prompt used to pay the whole host startup bill: spawn
 * `codex app-server` / `cursor-sdk-bridge` / the ACP agent, wait for its
 * ready signal, run the protocol handshake, then create or resume the host
 * session (for cursor a CreateAgent round trip to the cloud). Here that work
 * starts the moment the shell paints, on a detached pthread that runs
 * connect() and create_or_resume(), then parks the ready backend for
 * adoption.
 *
 * Threading contract (keeps the one-event-loop invariant intact):
 *   - the thread touches only the backend it was handed and this struct; the
 *     resume pointer is its own copy, frozen at start time;
 *   - no events flow before send(), so nothing ever crosses threads mid-turn;
 *   - create_or_resume reads ctx fields (/model, /fast, /workspace): every
 *     command that mutates them calls tui_prewarm_drop first, and drop waits
 *     out a create_or_resume already in flight;
 *   - a pending warm-up whose provider or resume pointer no longer matches
 *     the shell's state is dropped and restarted, never adopted stale;
 *   - whoever sees the handoff completed last cleans up: take() on the main
 *     thread, or the thread itself when the main thread abandoned the warm-up
 *     first (provider switch, shell exit).
 *
 * The CLI (`tny ask`, --help, --version) never calls into this file: one-shot
 * commands keep the lazy-connect startup contract.
 */
#include "tui/tui.h"
#include "mcp/mcp.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct tui_prewarm {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    tny_backend *bk;
    int backend_id;
    char *resume_pointer; /* frozen at start; NULL for a new session */
    bool resuming;        /* create_or_resume in flight: it reads ctx */
    bool done;            /* connect() + create_or_resume() returned */
    bool ok;              /* ...and both succeeded */
    bool abandoned;       /* main thread gave up: thread owns cleanup */
};

static void prewarm_free(tui_prewarm *p) {
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->cv);
    free(p->resume_pointer);
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

    if (rc == 0 && p->bk->create_or_resume) {
        pthread_mutex_lock(&p->mu);
        /* abandoned mid-connect: ctx may already be mutating on the main
         * thread, so the session must not be touched from here */
        p->resuming = !p->abandoned;
        pthread_mutex_unlock(&p->mu);
        if (p->resuming) rc = p->bk->create_or_resume(p->bk, p->resume_pointer, err, sizeof err);
    }

    pthread_mutex_lock(&p->mu);
    p->resuming = false;
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
    case TNY_BK_CODEX: return true; /* spawn or --codex-ws attach; auth errors surface later */
    case TNY_BK_CURSOR: {
        const char *key = getenv("CURSOR_API_KEY");
        return key && *key;
    }
    case TNY_BK_ACP: return ctx->agent_argv && ctx->agent_argv[0];
    default: return false;
    }
}

int tui_prewarm_launch(tui *t, tny_backend *bk, int backend_id, const char *resume_pointer) {
    tui_prewarm *p = calloc(1, sizeof *p);
    if (!p) {
        bk->destroy(bk);
        return -1;
    }
    p->bk = bk;
    p->backend_id = backend_id;
    p->resume_pointer = resume_pointer ? xstrdup(resume_pointer) : NULL;
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

static bool same_pointer(const char *a, const char *b) {
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

/* The resume pointer the warm-up should replay: the session's host pointer,
 * but only if the session belongs to the provider being warmed — the same
 * owner check the lazy bind applies at Enter. */
static const char *resume_pointer_for(tui *t) {
    if (!t->session) return NULL;
    const char *hp = session_host_pointer(t->session);
    if (!hp) return NULL;
    const char *owner = session_backend(t->session);
    if (owner && strcmp(owner, tny_provider_name(t->ctx)) != 0) return NULL;
    return hp;
}

void tui_prewarm_start(tui *t) {
    /* MCP servers warm alongside the host (docs/adr/0049): only the native
     * loop dispatches mcp_* tools, and mcp_warm_start latches after the
     * first call, so provider switches back to native re-enter for free. */
    if (t->ctx->backend == TNY_BK_OPENAI) mcp_warm_start(t->ctx);
    const char *hp = resume_pointer_for(t);
    if (t->prewarm) {
        if (t->prewarm->backend_id == t->ctx->backend &&
            same_pointer(t->prewarm->resume_pointer, hp))
            return;          /* already warming exactly this */
        tui_prewarm_drop(t); /* stale provider or session: never adopt it */
    }
    if (!tui_prewarm_applicable(t->ctx, t->ctx->backend)) return;
    tny_backend *bk = tny_backend_create((tny_backend_id)t->ctx->backend, t->ctx);
    if (!bk) return;
    tui_prewarm_launch(t, bk, t->ctx->backend, hp);
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
    while (!p->done) /* mid-warm-up: waiting here costs what the lazy path did */
        pthread_cond_wait(&p->cv, &p->mu);
    bool ok = p->ok;
    tny_backend *bk = p->bk;
    pthread_mutex_unlock(&p->mu);
    prewarm_free(p);

    if (!ok) { /* silent: the caller rebuilds synchronously and reports */
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
    if (p->resuming && !p->done) {
        /* create_or_resume is reading ctx right now: outlast it so the
         * caller may mutate model/tier/workspace state the moment we return */
        while (!p->done) pthread_cond_wait(&p->cv, &p->mu);
        pthread_mutex_unlock(&p->mu); /* thread saw abandoned == false */
        backend_discard(p->bk);
        prewarm_free(p);
        return;
    }
    p->abandoned = true;
    bool done = p->done;
    pthread_mutex_unlock(&p->mu);

    if (done) { /* thread already finished and saw abandoned == false */
        backend_discard(p->bk);
        prewarm_free(p);
    }
    /* else: still in connect(); the thread sees abandoned before
     * create_or_resume could touch ctx and cleans up on its own */
}
