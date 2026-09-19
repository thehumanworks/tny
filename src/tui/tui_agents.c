/* Shared interactive/noninteractive background-session dashboard. */
#include "tui/tui.h"
#include "core/jobs.h"
#include "mcp/mcp.h"
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

/* Job membership is authoritative. Never build a run by matching arbitrary
 * session ids, titles or process ancestry from the workspace session list. */
static yyjson_doc *agents_run_status(tny_ctx *ctx, const char *id, char *err, size_t errlen) {
    if (!tny_jobs_valid_id(id)) {
        snprintf(err, errlen, "--run requires a 32-character lowercase hex job id");
        return NULL;
    }
    char request[64];
    snprintf(request, sizeof request, "{\"id\":\"%s\"}", id);
    yyjson_doc *args = jparse(request, strlen(request));
    buf_t out;
    buf_init(&out);
    int rc =
        args ? tny_jobs_run(ctx, TNY_JOBS_OP_STATUS, yyjson_doc_get_root(args), &out, err, errlen)
             : 1;
    yyjson_doc_free(args);
    yyjson_doc *doc = rc == 0 && out.data ? jparse(out.data, out.len) : NULL;
    buf_free(&out);
    if (doc && !jget_bool(yyjson_doc_get_root(doc), "dag", false)) {
        snprintf(err, errlen, "--run requires an opt-in DAG job, not an ordinary batch");
        yyjson_doc_free(doc);
        doc = NULL;
    }
    if (!doc && !err[0]) snprintf(err, errlen, "could not read the run record");
    return doc;
}

static const char *agent_field(yyjson_val *obj, const char *key, const char *fallback) {
    const char *value = jget_str(obj, key);
    return value ? value : fallback;
}

/* Labels are untrusted display data, not terminal escape sequences. JSON
 * retains their exact value; human rows replace ASCII control characters. */
static void agent_label(yyjson_val *item, char out[257]) {
    snprintf(out, 257, "%s", agent_field(item, "label", "(unlabeled)"));
    for (char *p = out; *p; p++)
        if ((unsigned char)*p < 32 || (unsigned char)*p == 127) *p = ' ';
}

static void agents_run_refresh(tui *t) {
    char err[320] = "";
    yyjson_doc *doc = agents_run_status(t->ctx, t->g->agents_run, err, sizeof err);
    tui_overlay_clear(t);
    tui_overlay_linef(t, "Run %s — task status; Up/Down scroll; q exits", t->g->agents_run);
    t->agent_run_count = 0;
    if (!doc) tui_overlay_linef(t, "%s", err);
    else {
        yyjson_val *run = yyjson_doc_get_root(doc);
        yyjson_val *items = jget(run, "items");
        t->agent_run_count = (int)yyjson_arr_size(items);
        if (t->agent_selected >= t->agent_run_count) t->agent_selected = 0;
        tui_overlay_linef(t, "Execution: %s; verification: %s; attempt: %lld",
                          agent_field(run, "state", "unknown"),
                          agent_field(run, "verification", "unverified"),
                          (long long)jget_int(run, "attempt", 0));
        yyjson_val *usage = jget(run, "usage"), *admission = jget(run, "admission");
        tui_overlay_linef(t, "Known tokens: %lld in / %lld out; unknown tasks: %lld",
                          (long long)jget_int(usage, "known_input_tokens", 0),
                          (long long)jget_int(usage, "known_output_tokens", 0),
                          (long long)jget_int(usage, "unknown_items", 0));
        tui_overlay_linef(t, "Admission: %s; cap: %lld; token policy: %s",
                          admission && yyjson_is_obj(admission) ? "enrolled" : "not enrolled",
                          (long long)jget_int(admission, "cap", 0),
                          agent_field(run, "budget_state", "none"));
        int start = t->agent_selected / 8 * 8;
        for (int i = start; i < t->agent_run_count && i < start + 8; i++) {
            yyjson_val *item = yyjson_arr_get(items, (size_t)i);
            char label[257];
            agent_label(item, label);
            tui_overlay_linef(
                t, "%s +- %d %-6s %-11s %.45s [%s] %s", i == t->agent_selected ? ">" : " ", i,
                agent_field(item, "role", "worker"), agent_field(item, "state", "unknown"), label,
                agent_field(item, "verification", "unverified"),
                agent_field(item, "admission_reason", ""));
        }
    }
    yyjson_doc_free(doc);
    t->agents_refresh = monotonic_ms() + 500;
    t->dirty = true;
}

void tui_agents_refresh(tui *t) {
    if (t->g->agents_run) {
        agents_run_refresh(t);
        return;
    }
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
    tui_clear_screen(t);
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
    if (m->backend &&
        (strcmp(m->backend, "cursor") == 0 || strcmp(m->backend, "acp") == 0 ||
         strncmp(m->backend, "acp@", 4) == 0 || strncmp(m->backend, "acp:", 4) == 0)) {
        tui_err(t, "cannot attach: this session uses a removed provider");
        return;
    }
    if (m->workspace && strcmp(m->workspace, t->ctx->cwd) != 0) {
        /* Attachment and subsequent turns must use the selected checkout's
         * storage, settings and permissions, not the dashboard's origin. */
        tui_prewarm_drop(t);
        cli_globals next = *t->g;
        next.cwd = m->workspace;
        next.ssh = next.ssh_cwd = NULL;
        tui_raw_begin(t);
        tny_ctx *ctx = cli_make_ctx(&next);
        tui_raw_end(t);
        if (!ctx) {
            tui_err(t, "cannot load the background session's workspace");
            return;
        }
        if (tny_resolve_backend(ctx, m->backend ? m->backend : "openai") < 0) {
            tny_ctx_free(ctx);
            tui_err(t, "cannot attach: provider configuration is unavailable");
            return;
        }
        if (t->engine) tny_engine_end_session(t->engine, "agents");
        tui_drop_backend(t);
        if (t->session) {
            session_close(t->session);
            t->session = NULL;
        }
        mcp_shutdown_all();
        perm_free(t->perm);
        if (t->owns_ctx) tny_ctx_free(t->ctx);
        t->ctx = ctx;
        t->owns_ctx = true;
        t->perm = perm_new(ctx);
        t->worktree = NULL; /* discovery does not acquire a managed-worktree lock */
        tui_files_free(t);
    }
    tui_prewarm_drop(t);
    if (tny_resolve_backend(t->ctx, m->backend ? m->backend : "openai") < 0) {
        tui_err(t, "cannot attach: provider configuration is unavailable");
        return;
    }
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
    free(t->ctx->model);
    t->ctx->model = m->model ? xstrdup(m->model) : NULL;
    tui_sysf(t, "Attached %s (%s); /agents returns to the list; quit detaches", m->id,
             agent_status(m));
    tui_command(t, "/transcript");
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
    const char *run_id = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) json = true;
        else if (strcmp(argv[i], "--run") == 0 && i + 1 < argc && !run_id) run_id = argv[++i];
        else {
            fputs("tny: agents accepts --json and --run ID\n", stderr);
            return 1;
        }
    }
    if (g->ephemeral) {
        fputs("tny: agents is unavailable in ephemeral mode\n", stderr);
        return 1;
    }
    yyjson_doc *run = NULL;
    if (run_id) {
        char err[320] = "";
        run = agents_run_status(ctx, run_id, err, sizeof err);
        if (!run) {
            fprintf(stderr, "tny: agents: %s\n", err);
            return 1;
        }
    }
    if (!json && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        yyjson_doc_free(run);
        cli_globals interactive = *g;
        interactive.agents_dashboard = true;
        interactive.agents_run = run_id;
        return cmd_tui(ctx, &interactive);
    }
    if (run) {
        yyjson_val *root = yyjson_doc_get_root(run);
        if (json) {
            char *body = jwrite_val(root);
            if (!body) {
                yyjson_doc_free(run);
                return 1;
            }
            printf("{\"kind\":\"agents\",\"run\":%s}\n", body);
            free(body);
        } else {
            printf("Run %s: %s (verification: %s)\n", run_id, agent_field(root, "state", "unknown"),
                   agent_field(root, "verification", "unverified"));
            yyjson_val *items = jget(root, "items");
            size_t index, count;
            yyjson_val *item;
            yyjson_arr_foreach(items, index, count, item) {
                char label[257];
                agent_label(item, label);
                printf("  +- %zu %-6s %-11s %s [%s]\n", index, agent_field(item, "role", "worker"),
                       agent_field(item, "state", "unknown"), label,
                       agent_field(item, "verification", "unverified"));
            }
        }
        yyjson_doc_free(run);
        return 0;
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
