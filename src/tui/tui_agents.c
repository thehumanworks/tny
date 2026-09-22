/* Shared interactive/noninteractive saved-session dashboard. */
#include "tui/tui.h"
#include "core/jobs.h"
#include "mcp/mcp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *agent_status(const session_meta *m) {
    if (m->status && strcmp(m->status, "running") != 0) return m->status;
    if (m->running) return "running";
    return m->status ? "stale" : "saved";
}

static bool agent_working(const session_meta *m) {
    return m->running && (!m->status || strcmp(m->status, "running") == 0);
}

/* Stored titles and workspace paths can contain terminal control characters.
 * Keep the original bytes in JSON and flatten only terminal-facing labels. */
static char *agent_display(const char *s, const char *fallback) {
    char *out = xstrdup(s ? s : fallback);
    if (!out) return NULL;
    for (unsigned char *p = (unsigned char *)out; *p; p++)
        if (*p < 32 || *p == 127) *p = ' ';
    return out;
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
    char selected_hash[17] = "";
    if (selected)
        snprintf(selected_hash, sizeof selected_hash, "%s", t->agents[t->agent_selected].ws_hash);
    session_meta_free(t->agents, t->n_agents);
    t->agents = session_agents(t->ctx, &t->n_agents);
    for (int i = 0; selected && i < t->n_agents; i++)
        if (strcmp(selected, t->agents[i].id) == 0 &&
            strcmp(selected_hash, t->agents[i].ws_hash) == 0)
            t->agent_selected = i;
    free(selected);
    if (t->agent_selected >= t->n_agents) t->agent_selected = t->n_agents ? t->n_agents - 1 : 0;
    tui_overlay_clear(t);
    tui_overlay_linef(t, "Agents — all saved sessions; Enter opens; q exits");
    if (!t->n_agents) tui_overlay_linef(t, "No saved sessions.");
    int start = t->agent_selected / 8 * 8;
    for (int i = start; i < t->n_agents && i < start + 8; i++) {
        const session_meta *m = &t->agents[i];
        char *title = agent_display(m->title, "(untitled)");
        char *workspace = agent_display(m->workspace, "(unknown workspace)");
        char *provider = agent_display(m->backend, "unknown");
        char *status = agent_display(agent_status(m), "saved");
        const char *name = workspace ? strrchr(workspace, '/') : NULL;
        tui_overlay_linef(t, "%s %s  %-10s  %s  %.70s", i == t->agent_selected ? ">" : " ", m->id,
                          status ? status : "saved", provider ? provider : "unknown",
                          title ? title : "(untitled)");
        tui_overlay_linef(t, "  %s  %s",
                          name ? name + 1 : (workspace ? workspace : "(unknown workspace)"),
                          workspace ? workspace : "(unknown workspace)");
        free(title);
        free(workspace);
        free(provider);
        free(status);
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
    /* Composer setup belongs to the previous foreground context. Never carry
     * its writable input route into a dashboard or saved session replica. */
    tui_wizard_cancel(t);
    /* A background runner still uses its checkout. Keep the managed worktree
     * when leaving this shell; the ordinary exit prompt may remove it. */
    if (t->background_view) t->worktree = NULL;
    if (t->rc) tui_runner_drop(t, "dashboard");
    if (t->engine) tny_engine_end_session(t->engine, "agents");
    tui_drop_backend(t); /* release an idle in-process engine before its session */
    t->turn_active = t->turn_done = false;
    t->cancel_ms = 0;
    t->background_view = false;
    t->session_readonly = false;
    t->rc_restart_pending = false;
    tui_queue_clear(t);
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
    if (!t->rc || t->ctx->no_save) {
        tui_err(t, "background requires a saved native session runner");
        return;
    }
    if (tny_runner_client_background(t->rc) != 0) {
        tui_err(t, "could not arm background handoff");
        return;
    }
    t->background_armed = true;
    t->dirty = true; /* the runner acknowledges the saved state once */
}

/* Replace local display/execution context without saving the read replica. */
static void agents_context(tui *t, tny_ctx *ctx) {
    if (t->engine) tny_engine_end_session(t->engine, "agents");
    tui_drop_backend(t);
    mcp_shutdown_all();
    perm_free(t->perm);
    if (t->owns_ctx) tny_ctx_free(t->ctx);
    t->ctx = ctx;
    t->owns_ctx = true;
    t->perm = perm_new(ctx);
    t->worktree = NULL; /* discovery does not acquire a managed-worktree lock */
    tui_files_free(t);
}

static bool agents_checkpoint(const tui *t) {
    return t->session &&
           yyjson_mut_obj_get(yyjson_mut_doc_get_root(t->session->doc), "continuation");
}

void tui_agents_select(tui *t) {
    if (t->agent_selected < 0 || t->agent_selected >= t->n_agents) return;
    session_meta *m = &t->agents[t->agent_selected];
    cli_globals next = *t->g;
    next.cwd = m->workspace ? m->workspace : t->ctx->cwd;
    next.ssh = next.ssh_cwd = NULL;
    /* Even /agents entered from a normal TUI must skip provider resolution.
     * Missing profiles, expired OAuth and removed providers cannot hide text. */
    next.agents_dashboard = true;
    tui_raw_begin(t);
    tny_ctx *ctx = cli_make_ctx(&next);
    tui_raw_end(t);
    if (!ctx) {
        tui_err(t, "cannot load the saved session's workspace");
        return;
    }
    /* The physical bucket is the row's identity. Old saved documents may
     * lack workspace, so the cwd used for viewing cannot rediscover it. */
    snprintf(ctx->ws_hash, sizeof ctx->ws_hash, "%s", m->ws_hash);
    tny_session_state *session = session_open(ctx, m->id);
    if (!session) {
        tny_ctx_free(ctx);
        tui_err(t, "saved session disappeared or is unreadable");
        return;
    }
    if (t->session) session_close(t->session);
    t->session = NULL;
    agents_context(t, ctx);
    t->session = session;
    t->background_view = true;
    t->session_readonly = true;
    t->agents_dashboard = false;
    t->background_armed = false;
    t->turn_active = t->turn_done = false;
    /* Display selectors only. Actual execution resolves a fresh context from
     * the locked snapshot, never credentials from this viewing context. */
    free(ctx->provider_name);
    ctx->provider_name = xstrdup(session_backend(session) ? session_backend(session) : "openai");
    free(ctx->model);
    ctx->model = xstrdup(
        yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_doc_get_root(session->doc), "model")));
    session_get_usage(session, &t->in_tok, &t->out_tok);
    bool running = session_is_running(ctx, session->id);
    if (running && tui_runner_attach(t, session)) t->session_readonly = false;
    tui_overlay_clear(t);
    if (!t->session_readonly)
        tui_sysf(t, "Attached %s (%s); /agents returns to the list; quit detaches", session->id,
                 agent_status(m));
    else {
        tui_sysf(t, "Saved read-only %s (%s); /agents returns to the list; quit detaches",
                 session->id, agent_status(m));
        if (running) tui_sys(t, "Owner unavailable or another owner is attached; saved text only.");
        tui_sys(t, "Submit a prompt or /continue to request ownership; retry when the writer "
                   "is available. No takeover.");
        if (agents_checkpoint(t))
            tui_sys(t, "Saved checkpoint: /continue explicitly recovers retained work; prompts "
                       "are not submitted or queued before recovery.");
    }
    char *workspace_label = agent_display(ctx->cwd, "(unknown workspace)");
    if (m->workspace)
        tui_sysf(t, "Workspace: %s", workspace_label ? workspace_label : "(unknown workspace)");
    else
        tui_sysf(t, "Saved workspace unknown; continuing uses current cwd: %s",
                 workspace_label ? workspace_label : "(unknown workspace)");
    free(workspace_label);
    tui_command(t, "/transcript");
    if (!running && m->status && strcmp(m->status, "running") == 0)
        tui_sys(t, "Stale: its writer is gone. Saved transcript is available; no work is running.");
    t->dirty = true;
}

/* Explicit execution intent only. A failed handshake is never authority to
 * bind a replacement listener. Keep ADR0104's lock through reload and fork. */
bool tui_agents_continue(tui *t, bool prompt) {
    if (!t->background_view || !t->session) return false;
    if (t->rc) return true;
    t->session_readonly = true;
    if (session_is_running(t->ctx, t->session->id)) {
        if (!tui_runner_attach(t, t->session)) {
            tui_err(t, "Still read-only: owner unavailable or another owner is attached; "
                       "retry /continue when ownership is available. No prompt was submitted.");
            return false;
        }
        t->session_readonly = false;
        tui_sysf(t, "Attached %s; continuing the existing runner", t->session->id);
        return true;
    }
    if (!tny_isolation_enabled(t->ctx)) {
        tui_err(t, "Still read-only: continuation requires a native session runner "
                   "(unavailable in wasm or in-process mode)");
        return false;
    }
    if (session_lock_acquire(t->session) != 0) {
        tui_err(t, "Still read-only: session is locked; retry /continue when ownership is "
                   "available. No prompt was submitted.");
        return false;
    }
    bool ok = false;
    char err[256];
    if (session_reload_locked(t->session, err, sizeof err) != 0) {
        tui_err(t, err);
        goto done;
    }
    bool checkpoint = agents_checkpoint(t);
    if (prompt && checkpoint) {
        tui_err(t, "Saved checkpoint: use /continue to explicitly recover retained work. "
                   "Your prompt was not submitted or queued.");
        goto done;
    }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(t->session->doc);
    const char *workspace = yyjson_mut_get_str(yyjson_mut_obj_get(root, "workspace"));
    if (workspace && strcmp(workspace, t->ctx->cwd) != 0) {
        tui_err(t, "saved workspace changed; reopen the session from /agents");
        goto done;
    }
    cli_globals next = *t->g;
    next.agents_dashboard = false;
    next.cwd = t->ctx->cwd;
    next.ssh = next.ssh_cwd = NULL;
    next.backend = session_backend(t->session);
    if (!next.backend) next.backend = "openai";
    next.model = yyjson_mut_get_str(yyjson_mut_obj_get(root, "model"));
    tui_raw_begin(t);
    tny_ctx *ctx = cli_make_ctx(&next);
    tui_raw_end(t);
    if (!ctx) {
        tui_err(t, "Still read-only: execution configuration is unavailable; restore the "
                   "selected provider's configuration and retry /continue");
        goto done;
    }
    if (!tny_isolation_enabled(ctx)) {
        tny_ctx_free(ctx);
        tui_err(t, "Still read-only: continuation requires a native session runner");
        goto done;
    }
    /* Preserve the selected physical session bucket for the child exec.
     * Its context snapshot already carries ws_hash separately from cwd. */
    snprintf(ctx->ws_hash, sizeof ctx->ws_hash, "%s", t->ctx->ws_hash);
    t->session->ctx = ctx;
    if (session_task_reconcile(t->session, err, sizeof err) != 0) {
        t->session->ctx = t->ctx;
        tny_ctx_free(ctx);
        tui_err(t, err);
        goto done;
    }
    agents_context(t, ctx);
    if (tui_runner_ensure(t, false) != 0) goto done;
    t->session_readonly = false;
    t->turn_active = checkpoint;
    tui_sysf(t, "Continuing %s%s", t->session->id,
             checkpoint ? ": recovering saved work (no new prompt)" : "; ready for a prompt");
    ok = true;
done:
    /* The child inherited the same open description. The UI must not pin it
     * after the child exits, even on a failed connection. */
    session_lock_release(t->session);
    t->dirty = true;
    return ok;
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
            buf_appends(&row, ",\"workspace\":");
            if (m[i].workspace) jescape(&row, m[i].workspace);
            else buf_appends(&row, "null");
            buf_appends(&row, ",\"workspace_bucket\":");
            jescape(&row, m[i].ws_hash);
            buf_appendf(&row, ",\"running\":%s,\"live\":%s}",
                        agent_working(&m[i]) ? "true" : "false", m[i].running ? "true" : "false");
            fputs(row.data, stdout);
            buf_free(&row);
        } else {
            char *title = agent_display(m[i].title, "(untitled)");
            char *workspace = agent_display(m[i].workspace, "(unknown workspace)");
            char *status = agent_display(agent_status(&m[i]), "saved");
            printf("%s  %-10s  %s  %s\n", m[i].id, status ? status : "saved",
                   workspace ? workspace : "(unknown workspace)", title ? title : "(untitled)");
            free(title);
            free(workspace);
            free(status);
        }
    }
    if (json) fputs("]}\n", stdout);
    else if (!n) puts("No saved sessions.");
    session_meta_free(m, n);
    return 0;
}
