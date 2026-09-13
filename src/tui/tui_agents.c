/* Shared interactive/noninteractive background-session dashboard. */
#include "tui/tui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *agent_status(const session_meta *m) {
    if (m->status && strcmp(m->status, "running") != 0) return m->status;
    return m->running ? "running" : "stale";
}

static bool agent_working(const session_meta *m) {
    return m->running && m->status && strcmp(m->status, "running") == 0;
}

void tui_agents_refresh(tui *t) {
    char *selected = t->agent_selected >= 0 && t->agent_selected < t->n_agents
                         ? xstrdup(t->agents[t->agent_selected].id)
                         : NULL;
    session_meta_free(t->agents, t->n_agents);
    t->agents = session_agents(t->ctx, &t->n_agents);
    for (int i = 0; selected && i < t->n_agents; i++)
        if (strcmp(selected, t->agents[i].id) == 0) t->agent_selected = i;
    free(selected);
    if (t->agent_selected >= t->n_agents) t->agent_selected = t->n_agents ? t->n_agents - 1 : 0;
    tui_overlay_clear(t);
    tui_overlay_linef(t, "Background agents — Enter reattaches; q exits (work keeps running)");
    if (!t->n_agents) tui_overlay_linef(t, "No background sessions in this workspace.");
    int start = t->agent_selected / 8 * 8;
    for (int i = start; i < t->n_agents && i < start + 8; i++) {
        const session_meta *m = &t->agents[i];
        tui_overlay_linef(t, "%s %s  %-10s  %s  %.70s", i == t->agent_selected ? ">" : " ", m->id,
                          agent_status(m), m->backend ? m->backend : "unknown",
                          m->title ? m->title : "(untitled)");
    }
    t->agents_refresh = monotonic_ms() + 500;
    t->dirty = true;
}

void tui_agents_open(tui *t) {
    if (t->ctx->no_save) {
        tui_err(t, "background agents are unavailable in ephemeral mode");
        return;
    }
    if (t->turn_active && !t->background_view) {
        tui_background_arm(t);
        return;
    }
    if (t->rc) tui_runner_drop(t, "dashboard");
    else tui_prewarm_drop(t);
    t->turn_active = t->turn_done = false;
    t->cancel_ms = 0;
    t->background_view = false;
    if (t->session) {
        session_close(t->session);
        t->session = NULL;
    }
    tui_pick_close(t);
    t->agents_dashboard = true;
    tui_agents_refresh(t);
}

void tui_background_arm(tui *t) {
    if (t->background_armed || t->cancel_ms) return;
    if (!t->rc || t->ctx->backend != TNY_BK_OPENAI || t->ctx->no_save) {
        tui_err(t, "background handoff requires a saved native session runner (unavailable in "
                   "host, wasm, ephemeral or in-process mode)");
        return;
    }
    if (tny_runner_client_background(t->rc) != 0) {
        tui_err(t, "could not arm background handoff");
        return;
    }
    t->background_armed = true;
    t->dirty = true; /* the runner acknowledges the armed state once */
}

void tui_agents_select(tui *t) {
    if (t->agent_selected < 0 || t->agent_selected >= t->n_agents) return;
    session_meta *m = &t->agents[t->agent_selected];
    tny_session_state *session = session_open(t->ctx, m->id);
    if (!session) {
        tui_err(t, "background session disappeared or is unreadable");
        return;
    }
    bool running = session_is_running(t->ctx, m->id);
    if (running && !tui_runner_attach(t, session)) {
        session_close(session);
        tui_err(t, "cannot reattach: another owner is attached or the runner is unreachable");
        return;
    }
    if (t->session) session_close(t->session);
    t->session = session;
    t->background_view = true;
    t->agents_dashboard = false;
    t->background_armed = false;
    tui_overlay_clear(t);
    /* A live runner retains its exact configuration. The local context is
     * only its display/next-turn selection, never a provider startup here. */
    free(t->ctx->provider_name);
    t->ctx->provider_name = xstrdup(m->backend ? m->backend : "openai");
    t->ctx->backend =
        m->backend && strcmp(m->backend, "cursor") == 0 ? TNY_BK_CURSOR
        : m->backend && (strcmp(m->backend, "acp") == 0 || str_starts(m->backend, "acp@"))
            ? TNY_BK_ACP
            : TNY_BK_OPENAI;
    free(t->ctx->model);
    t->ctx->model = m->model ? xstrdup(m->model) : NULL;
    tui_sysf(t, "Attached %s (%s); /agents returns to the list; quit detaches", m->id,
             agent_status(m));
    tui_command(t, "/transcript");
    /* Resolve credentials/host mode for a later prompt, even if this live
     * runner exits after attachment. This does not start a provider. */
    char *model = t->ctx->model ? xstrdup(t->ctx->model) : NULL;
    tny_resolve_backend(t->ctx, m->backend ? m->backend : "openai");
    free(t->ctx->model);
    t->ctx->model = model;
    if (!running) {
        char err[192];
        if (session_task_reconcile(session, err, sizeof err) != 0) tui_err(t, err);
        if (yyjson_mut_obj_get(yyjson_mut_doc_get_root(session->doc), "continuation")) {
            if (!tui_runner_mode(t))
                tui_err(t, "checkpoint recovery requires a native session runner");
            else if (tui_runner_ensure(t, false) == 0) t->turn_active = true;
        }
    }
    if (!running && (!m->status || strcmp(m->status, "running") == 0))
        tui_sys(t, "Stale: its writer is gone. Saved transcript is available; no work is running.");
    t->dirty = true;
}

int cmd_agents(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    bool json = g->json;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) json = true;
        else {
            fputs("tny: agents accepts --json\n", stderr);
            return 1;
        }
    }
    if (g->ephemeral) {
        fputs("tny: agents is unavailable in ephemeral mode\n", stderr);
        return 1;
    }
    if (!json && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        cli_globals interactive = *g;
        interactive.agents_dashboard = true;
        return cmd_tui(ctx, &interactive);
    }
    int n = 0;
    session_meta *m = session_agents(ctx, &n);
    if (json) fputs("{\"kind\":\"agents\",\"agents\":[", stdout);
    for (int i = 0; i < n; i++) {
        if (json) {
            buf_t row;
            buf_init(&row);
            buf_appends(&row, i ? ",{\"session_id\":" : "{\"session_id\":");
            jescape(&row, m[i].id);
            buf_appends(&row, ",\"status\":");
            jescape(&row, agent_status(&m[i]));
            buf_appends(&row, ",\"provider\":");
            jescape(&row, m[i].backend ? m[i].backend : "unknown");
            buf_appendf(&row, ",\"running\":%s,\"live\":%s}",
                        agent_working(&m[i]) ? "true" : "false", m[i].running ? "true" : "false");
            fputs(row.data, stdout);
            buf_free(&row);
        } else
            printf("%s  %-10s  %s\n", m[i].id, agent_status(&m[i]),
                   m[i].title ? m[i].title : "(untitled)");
    }
    if (json) fputs("]}\n", stdout);
    else if (!n) puts("No background sessions in this workspace.");
    session_meta_free(m, n);
    return 0;
}
