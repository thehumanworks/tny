/* tui_commands.c — slash palette, @ file picker, $ skill picker and the
 * command implementations. Commands that already exist as CLI subcommands are
 * reused verbatim: the block is torn down first so their stdout scrolls. */
#include "tui/tui.h"
#include "core/skills.h"
#include "net/net.h"
#include "core/tools.h"
#include "cli/cli.h"
#include "core/ssh.h"
#include "mcp/mcp.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const struct { const char *name, *hint; } CMDS[] = {
    {"help",        "keys and commands"},
    {"clear",       "clear the screen"},
    {"new",         "start a new session"},
    {"reset",       "new session and clear the screen"},
    {"resume",      "/resume [id|last]"},
    {"continue",    "resume the latest session"},
    {"rename",      "/rename TITLE"},
    {"compact",     "condense the transcript now"},
    {"quit",        "leave tny"},
    {"models",      "list provider models"},
    {"model",       "/model [ID]"},
    {"permissions", "/permissions [ask|auto|yolo]"},
    {"sandbox",     "show the sandbox mode"},
    {"provider",    "/provider [NAME]"}, /* hint built from cmd_hint() */
    {"fast",        "/fast [fast|priority|default] — provider speed tier"},
    {"effort",      "/effort [" TNY_EFFORT_LEVELS "|default]"},
    {"max-steps",   "/max-steps [set N|clear] — cap the agent loop per turn"},
    {"status",      "provider, auth, workspace"},
    {"usage",       "token usage for this workspace"},
    {"sessions",    "list sessions for this workspace"},
    {"mcp",         "list configured MCP servers"},
    {"skills",      "list discovered skills"},
    {"workspace",   "/workspace [add|remove DIR]"},
    {"image",       "/image PATH — attach to the next prompt"},
    {"ssh",         "/ssh user@host[:port] [dir] | /ssh off — run tools on a remote host"},
    {"undo",        "undo the last file change"},
    {"copy",        "copy the last reply to the clipboard"},
    {"trace",       "toggle raw event tracing"},
    {"transcript",  "print the whole session transcript"},
    {"login",       "auth for the active provider"},
    {"logout",      "drop provider auth"},
    {"setup",       "write provider config (flags only)"},
};
#define N_CMDS ((int)(sizeof CMDS / sizeof *CMDS))

/* ---- picker item list ---- */

void tui_items_clear(tui *t) {
    for (int i = 0; i < t->n_items; i++) {
        free(t->items[i].label);
        free(t->items[i].hint);
    }
    free(t->items);
    t->items = NULL;
    t->n_items = 0;
}

void tui_items_add(tui *t, const char *label, const char *hint) {
    if (t->n_items >= 64) return;
    t->items = realloc(t->items, sizeof(pick_item) * (size_t)(t->n_items + 1));
    t->items[t->n_items].label = xstrdup(label);
    t->items[t->n_items].hint = hint ? xstrdup(hint) : NULL;
    t->n_items++;
}

/* case-insensitive substring; strcasestr is not portable enough to rely on */
static bool istr(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return true;
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, n) == 0) return true;
    return false;
}

/* case-insensitive subsequence match */
static bool fuzzy(const char *hay, const char *needle) {
    if (!*needle) return true;
    const char *n = needle;
    for (const char *h = hay; *h; h++) {
        if (tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            if (!*++n) return true;
        }
    }
    return false;
}

/* Hint for CMDS[i]; /provider lists the providers actually available here
 * (builtins + settings.json profiles + NAME_BASE_URL). malloc'd. */
static char *cmd_hint(tui *t, int i) {
    if (strcmp(CMDS[i].name, "provider") != 0) return xstrdup(CMDS[i].hint);
    char *names = tny_provider_names_joined(t->ctx);
    size_t n = strlen(names) + 32;
    char *h = malloc(n);
    if (!h) return xstrdup(CMDS[i].hint);
    snprintf(h, n, "/provider [%s]", names);
    free(names);
    return h;
}

void tui_pick_build_cmd(tui *t, const char *filter) {
    for (int i = 0; i < N_CMDS; i++)
        if (strncasecmp(CMDS[i].name, filter, strlen(filter)) == 0) {
            char *h = cmd_hint(t, i);
            tui_items_add(t, CMDS[i].name, h);
            free(h);
        }
    if (t->n_items) return;
    for (int i = 0; i < N_CMDS; i++)
        if (fuzzy(CMDS[i].name, filter)) {
            char *h = cmd_hint(t, i);
            tui_items_add(t, CMDS[i].name, h);
            free(h);
        }
}

/* ---- workspace file cache ---- */

static const char *SKIP_DIRS[] = {".git", "node_modules", "build", "dist", "target",
                                  "vendor", "__pycache__", ".venv", ".tny", ".cache", NULL};

typedef struct { char **pat; int n; } ignore_set;

static bool ignored(const ignore_set *ig, const char *name) {
    for (const char **s = SKIP_DIRS; *s; s++)
        if (strcmp(name, *s) == 0) return true;
    for (int i = 0; i < ig->n; i++)
        if (glob_match(ig->pat[i], name)) return true;
    return false;
}

static void load_gitignore(tny_ctx *ctx, ignore_set *ig) {
    char *p = path_join(ctx->cwd, ".gitignore");
    size_t len = 0;
    char *data = file_slurp(p, &len);
    free(p);
    if (!data) return;
    char *line = data;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || data[i] == '\n') {
            data[i] = 0;
            char *s = str_trim(line);
            size_t sl = strlen(s);
            while (sl && s[sl - 1] == '/') s[--sl] = 0;
            if (*s == '/') s++;
            if (*s && *s != '#' && *s != '!' && !strchr(s, '/') && ig->n < 128) {
                ig->pat = realloc(ig->pat, sizeof(char *) * (size_t)(ig->n + 1));
                ig->pat[ig->n++] = xstrdup(s);
            }
            line = data + i + 1;
        }
    }
    free(data);
}

static void scan_dir(tui *t, const char *abs, const char *rel, int depth,
                     const ignore_set *ig) {
    if (depth > 10 || t->n_files >= TUI_MAX_FILES) return;
    DIR *d = opendir(abs);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && t->n_files < TUI_MAX_FILES) {
        if (e->d_name[0] == '.') continue;
        if (ignored(ig, e->d_name)) continue;
        char *child_abs = path_join(abs, e->d_name);
        char *child_rel = *rel ? path_join(rel, e->d_name) : xstrdup(e->d_name);
        struct stat st;
        if (stat(child_abs, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                scan_dir(t, child_abs, child_rel, depth + 1, ig);
            } else if (S_ISREG(st.st_mode)) {
                t->files = realloc(t->files, sizeof(char *) * (size_t)(t->n_files + 1));
                t->files[t->n_files++] = xstrdup(child_rel);
            }
        }
        free(child_abs);
        free(child_rel);
    }
    closedir(d);
}

void tui_files_free(tui *t) {
    for (int i = 0; i < t->n_files; i++) free(t->files[i]);
    free(t->files);
    t->files = NULL;
    t->n_files = 0;
    t->files_scanned = false;
}

void tui_pick_build_file(tui *t, const char *filter) {
    if (!t->files_scanned) {
        ignore_set ig = {NULL, 0};
        load_gitignore(t->ctx, &ig);
        scan_dir(t, t->ctx->cwd, "", 0, &ig);
        for (int i = 0; i < ig.n; i++) free(ig.pat[i]);
        free(ig.pat);
        t->files_scanned = true;
    }
    /* exact substring first, then subsequence — keeps common cases on top */
    for (int pass = 0; pass < 2 && t->n_items < 40; pass++) {
        for (int i = 0; i < t->n_files && t->n_items < 40; i++) {
            const char *f = t->files[i];
            const char *base = strrchr(f, '/');
            base = base ? base + 1 : f;
            bool hit = pass == 0 ? istr(base, filter) : fuzzy(f, filter);
            if (!hit) continue;
            if (pass == 1) {
                bool dup = false;
                for (int j = 0; j < t->n_items; j++)
                    if (strcmp(t->items[j].label, f) == 0) { dup = true; break; }
                if (dup) continue;
            }
            tui_items_add(t, f, NULL);
        }
    }
}

void tui_pick_build_skill(tui *t, const char *filter) {
    int n = 0;
    skill_meta *s = skills_discover(t->ctx, &n);
    for (int i = 0; i < n && t->n_items < 40; i++)
        if (fuzzy(s[i].name, filter)) tui_items_add(t, s[i].name, s[i].description);
    skills_free(s, n);
}

/* ---- command helpers ---- */

static int tokenize(char *s, char **av, int max) {
    int n = 0;
    char *p = s;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        av[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    return n;
}

static int run_cli(tui *t, int (*fn)(tny_ctx *, const cli_globals *, int, char **),
                   int argc, char **argv) {
    tui_raw_begin(t);
    int rc = fn(t->ctx, t->g, argc, argv);
    tui_raw_end(t);
    return rc;
}

/* Rebinding commands drop the live backend; warming the (possibly new)
 * provider right away keeps the next prompt as fast as the first one. */
static void drop_backend(tui *t) {
    tui_drop_backend(t);
    tui_prewarm_start(t);
}

/* Rendered as a transient overlay: visible while the user reads it, gone
 * from the buffer once they move on (esc dismisses, submit clears). */
static void cmd_help(tui *t) {
    const char *d = tui_attr(t, "\x1b[2m"), *b = tui_attr(t, "\x1b[1m"), *r = tui_attr(t, "\x1b[0m");
    tui_overlay_linef(t, "%skeys: enter submit · ctrl-j / alt-j / shift-enter newline · "
                         "up/down history%s", d, r);
    tui_overlay_linef(t, "%s      / commands · @ files · $ skills · tab complete · "
                         "esc cancel turn%s", d, r);
    tui_overlay_linef(t, "%s      ctrl-v paste image path · ctrl-o transcript · "
                         "ctrl-c interrupt (twice exits)%s", d, r);
    for (int i = 0; i < N_CMDS; i++) {
        char *h = cmd_hint(t, i);
        tui_overlay_linef(t, "  %s/%-12s%s %s", b, CMDS[i].name, r, h);
        free(h);
    }
    if (t->tty)
        tui_overlay_linef(t, "%s(esc hides this menu)%s", d, r);
}

static void cmd_transcript(tui *t) {
    if (!t->session) { tui_sys(t, "no session yet"); return; }
    yyjson_mut_val *msgs = session_messages(t->session);
    if (!msgs) { tui_sys(t, "no messages"); return; }
    size_t idx, max;
    yyjson_mut_val *m;
    yyjson_mut_arr_foreach(msgs, idx, max, m) {
        yyjson_mut_val *r = yyjson_mut_obj_get(m, "role");
        yyjson_mut_val *c = yyjson_mut_obj_get(m, "content");
        const char *role = r ? yyjson_mut_get_str(r) : NULL;
        const char *txt = c ? yyjson_mut_get_str(c) : NULL;
        if (!role) continue;
        tui_linef(t, "%s[%s]%s %s", tui_attr(t, "\x1b[2m"), role, tui_attr(t, "\x1b[0m"),
                  txt ? txt : "(structured)");
    }
}

static void cmd_copy(tui *t) {
    if (!t->last_reply.len) { tui_sys(t, "nothing to copy yet"); return; }
    static const char *TRY[] = {"pbcopy", "wl-copy", "xclip -selection clipboard", NULL};
    for (const char **c = TRY; *c; c++) {
        char probe[128];
        snprintf(probe, sizeof probe, "command -v %.*s >/dev/null 2>&1",
                 (int)strcspn(*c, " "), *c);
        if (system(probe) != 0) continue;
        FILE *p = popen(*c, "w");
        if (!p) continue;
        fwrite(t->last_reply.data, 1, t->last_reply.len, p);
        pclose(p);
        tui_sys(t, "copied the last reply to the clipboard");
        return;
    }
    tui_sys(t, "no clipboard helper found (pbcopy / wl-copy / xclip)");
}

static void cmd_undo(tui *t) {
    if (!t->session) { tui_sys(t, "no session yet"); return; }
    tools_env env = {0};
    env.ctx = t->ctx;
    env.session = t->session;
    env.perm = t->perm;
    char *line = tools_undo_last(&env);
    tui_sys(t, line ? line : "nothing to undo");
    free(line);
}

static void cmd_mcp(tui *t) {
    tools_env env = {0};
    env.ctx = t->ctx;
    env.session = t->session;
    env.perm = t->perm;
    char *out = mcp_features(&env);
    if (out) {
        tui_bol(t);
        tui_write(t, out, strlen(out));
        tui_bol(t);
        free(out);
    }
}

static void cmd_skills(tui *t) {
    int n = 0;
    skill_meta *s = skills_discover(t->ctx, &n);
    if (!n) tui_sys(t, "no skills found (see docs/features/mcp-and-skills.md)");
    for (int i = 0; i < n; i++)
        tui_linef(t, "  %-24s %s", s[i].name, s[i].description ? s[i].description : "");
    skills_free(s, n);
}

static void cmd_resume_id(tui *t, const char *id) {
    tny_session_state *s = session_open(t->ctx, id);
    if (!s) {
        tui_err(t, "no such session for this workspace (try /sessions)");
        return;
    }
    char *previous = t->session && t->session->id
        ? xstrdup(t->session->id) : NULL;
    if (t->engine) tny_engine_end_session(t->engine, "resume");
    drop_backend(t); /* old engine still owns the old session until here */
    if (t->session) session_close(t->session);
    t->session = s;
    session_set_extension_start(s, "resume", previous);
    free(previous);
    int64_t in_tok = 0, out_tok = 0;
    session_get_usage(s, &in_tok, &out_tok);
    t->in_tok = in_tok;
    t->out_tok = out_tok;
    tui_sys(t, "resumed session");
    tui_linef(t, "  %s  %s", s->id, session_title(s) ? session_title(s) : "(untitled)");
}

/* ---- dispatch ---- */

void tui_command(tui *t, const char *line) {
    char *copy = xstrdup(line + 1); /* skip '/' */
    char *sp = copy;
    while (*sp && *sp != ' ') sp++;
    char *arg = NULL;
    if (*sp) { *sp = 0; arg = str_trim(sp + 1); }
    const char *c = copy;

    /* commands that swap the session or backend must not race a live turn */
    static const char *LOCKED[] = {"new", "reset", "resume", "continue", "compact",
                                   "backend", "provider", "model", "fast", "ssh", "undo", NULL};
    if (t->turn_active) {
        for (const char **l = LOCKED; *l; l++)
            if (strcmp(c, *l) == 0) {
                tui_sys(t, "a turn is running — esc interrupts it first");
                free(copy);
                return;
            }
    }

    if (!*c || strcmp(c, "help") == 0) cmd_help(t);
    else if (strcmp(c, "ssh") == 0) {
        if (!arg || !*arg) {
            if (t->ctx->ssh_host)
                tui_note(t, "tools run on %s in %s", t->ctx->ssh_host, t->ctx->ssh_cwd);
            else tui_sys(t, "usage: /ssh user@host[:port] [remote-dir] | /ssh off");
        } else if (strcmp(arg, "off") == 0) {
            if (t->ctx->ssh_host) {
                tui_note(t, "disconnected from %s; tools run locally", t->ctx->ssh_host);
                ssh_disconnect(t->ctx);
            } else tui_sys(t, "not connected");
        } else {
            char *target = xstrdup(arg);
            char *dir = strchr(target, ' ');
            if (dir) { *dir++ = 0; while (*dir == ' ') dir++; }
            /* OpenSSH may prompt (password, host key): leave the block and
             * hand it the terminal. The backend/session stay as they are. */
            tui_raw_begin(t);
            fflush(stdout);
            int rc = cli_ssh_attach(t->ctx, target, dir);
            tui_raw_end(t);
            if (rc == 0) tui_note(t, "tools now run on %s in %s", t->ctx->ssh_host, t->ctx->ssh_cwd);
            else tui_err(t, "ssh: could not attach (see above)");
            free(target);
        }
    } else if (strcmp(c, "quit") == 0 || strcmp(c, "exit") == 0) t->quit = true;
    else if (strcmp(c, "clear") == 0) {
        tui_raw_begin(t);
        fputs("\x1b[H\x1b[2J\x1b[3J", stdout);
        tui_raw_end(t);
    } else if (strcmp(c, "new") == 0) {
        tui_new_session(t, false);
    } else if (strcmp(c, "reset") == 0) {
        tui_new_session(t, true);
    } else if (strcmp(c, "resume") == 0) {
        if (arg && *arg) {
            cmd_resume_id(t, arg);
        } else {
            run_cli(t, cmd_sessions, 0, NULL);
            tui_sys(t, "pick one: /resume <id>");
        }
    } else if (strcmp(c, "continue") == 0) {
        cmd_resume_id(t, "last");
    } else if (strcmp(c, "sessions") == 0) {
        run_cli(t, cmd_sessions, 0, NULL);
    } else if (strcmp(c, "rename") == 0) {
        if (!t->session) tui_sys(t, "no session yet");
        else if (!arg || !*arg) tui_sys(t, "usage: /rename TITLE");
        else {
            session_set_title(t->session, arg);
            session_save(t->session);
            tui_sys(t, "renamed");
        }
    } else if (strcmp(c, "compact") == 0) {
        if (!t->session) tui_sys(t, "no session yet");
        else {
            int rc = t->engine
                ? tny_engine_compact(t->engine, true, "manual")
                : session_compact(t->session, true);
            if (!t->engine && rc >= 0) session_save(t->session);
            if (rc > 0) tui_sys(t, "transcript compacted");
            else if (rc == 0) tui_sys(t, "nothing to compact");
            else tui_err(t, "compaction failed");
        }
    } else if (strcmp(c, "models") == 0) {
        run_cli(t, cmd_models, 0, NULL);
    } else if (strcmp(c, "model") == 0) {
        if (arg && *arg) {
            tui_prewarm_drop(t); /* the warm-up thread reads ctx->model */
            char *previous = t->ctx->model ? xstrdup(t->ctx->model) : NULL;
            free(t->ctx->model);
            t->ctx->model = xstrdup(arg);
            t->ctx->model_from_flag = false; /* explicit choice, persist it */
            tny_settings_remember_use(t->ctx); /* saved per provider */
            tny_engine_model_changed(t->engine, previous, t->ctx->model,
                                     "command");
            free(previous);
            drop_backend(t); /* rebind so the new model reaches the host */
            tui_sys(t, "model set");
        } else {
            tui_linef(t, "  model: %s", t->ctx->model ? t->ctx->model : "default");
        }
        t->dirty = true;
    } else if (strcmp(c, "permissions") == 0) {
        if (arg && *arg) {
            if (strcmp(arg, "ask") == 0) t->ctx->perm_mode = TNY_MODE_ASK;
            else if (strcmp(arg, "auto") == 0) t->ctx->perm_mode = TNY_MODE_AUTO;
            else if (strcmp(arg, "yolo") == 0) t->ctx->perm_mode = TNY_MODE_YOLO;
            else tui_sys(t, "usage: /permissions [ask|auto|yolo]");
        } else {
            t->ctx->perm_mode = (tny_perm_mode)((t->ctx->perm_mode + 1) % 3);
        }
        tui_linef(t, "  permission mode: %s", tny_perm_mode_name(t->ctx->perm_mode));
        t->dirty = true;
    } else if (strcmp(c, "sandbox") == 0) {
        tui_linef(t, "  sandbox: %s%s", t->ctx->sandbox_mode,
                  strcmp(t->ctx->sandbox_mode, "os") == 0 ? " (unsupported: effective none)" : "");
    } else if (strcmp(c, "provider") == 0 || strcmp(c, "backend") == 0) {
        if (arg && strncmp(arg, "setup", 5) == 0 &&
            (arg[5] == 0 || arg[5] == ' ')) {
            if (t->turn_active) { tui_sys(t, "finish the turn first"); return; }
            const char *nm = arg[5] ? arg + 6 : NULL;
            while (nm && *nm == ' ') nm++;
            tui_wizard_start(t, nm);
            return;
        }
        if (arg && *arg) {
            bool known = tny_backend_from_name(arg) >= 0 ||
                         tny_builtin_profile_exists(arg) ||
                         tny_custom_provider_exists(t->ctx, arg) ||
                         str_starts(arg, "acp:");
            if (!known) tui_err(t, "unknown provider (openai|cursor|codex|acp|"
                                   "claude|grok|acp:NAME, a settings.json profile, or "
                                   "NAME_BASE_URL) — /provider setup adds one");
            else {
                if (t->turn_active) tui_sys(t, "finish the turn first");
                else {
                    tui_prewarm_drop(t); /* resolve swaps ctx fields it reads */
                    char *previous_model = t->ctx->model
                        ? xstrdup(t->ctx->model) : NULL;
                    char *previous_effort = t->ctx->reasoning_effort
                        ? xstrdup(t->ctx->reasoning_effort) : NULL;
                    /* full resolve: also swaps in the provider's saved model */
                    int resolved = tny_resolve_backend(t->ctx, arg);
                    if (resolved < 0) {
                        tui_err(t, "provider switch failed; check settings.json");
                        if (!t->engine) tui_prewarm_start(t);
                    } else {
                        tny_engine_model_changed(t->engine, previous_model,
                                                 t->ctx->model, "provider");
                        tny_engine_effort_changed(t->engine, previous_effort,
                                                  t->ctx->reasoning_effort,
                                                  "provider");
                        drop_backend(t);
                        tny_settings_remember_use(t->ctx);
                        tui_sys(t, "provider switched");
                    }
                    free(previous_model);
                    free(previous_effort);
                }
            }
        }
        tui_linef(t, "  provider: %s (model %s)", tny_provider_name(t->ctx),
                  t->ctx->model ? t->ctx->model : "default");
        t->dirty = true;
    } else if (strcmp(c, "fast") == 0) {
        /* TNY_CAP_FAST speed tier: fast|priority select the paid fast tier,
         * default turns it off. No argument toggles. Each capable backend
         * maps the tier to its own wire field (codex serviceTier, openai
         * service_tier, cursor model param). */
        if (!(tny_backend_caps((tny_backend_id)t->ctx->backend) & TNY_CAP_FAST)) {
            tui_err(t, "/fast: no fast tier on this provider — try /provider codex");
        } else {
            const char *cur = t->ctx->service_tier;
            const char *next = NULL;
            if (!arg || !*arg) next = tny_tier_is_fast(cur) ? "default" : "fast";
            else if (strcmp(arg, "fast") == 0 || strcmp(arg, "priority") == 0)
                next = "fast";
            else if (strcmp(arg, "default") == 0 || strcmp(arg, "off") == 0)
                next = "default";
            if (!next) {
                tui_err(t, "usage: /fast [fast|priority|default]");
            } else {
                tui_prewarm_drop(t); /* the warm-up thread reads the tier */
                free(t->ctx->service_tier);
                t->ctx->service_tier = xstrdup(next);
                drop_backend(t); /* the tier rides on session creation */
                tui_linef(t, "  service tier: %s%s", next,
                          tny_tier_is_fast(next)
                              ? " (fast: higher speed, increased usage)" : "");
            }
        }
        t->dirty = true;
    } else if (strcmp(c, "effort") == 0) {
        /* Reasoning effort applies from the next turn without a rebind: it
         * rides on codex turn/start, cursor SendOptions.model.params and the
         * openai request body. Levels beyond the canonical set are fine when
         * the provider's catalog advertises them (/models shows those). */
        if (!arg || !*arg) {
            tui_linef(t, "  reasoning effort: %s",
                      t->ctx->reasoning_effort ? t->ctx->reasoning_effort
                                               : "default");
            tui_sys(t, "usage: /effort [" TNY_EFFORT_LEVELS "|default] "
                       "— /models lists provider levels");
        } else {
            tui_prewarm_drop(t); /* the warm-up thread reads the effort */
            char *previous = t->ctx->reasoning_effort
                ? xstrdup(t->ctx->reasoning_effort) : NULL;
            free(t->ctx->reasoning_effort);
            t->ctx->reasoning_effort =
                strcmp(arg, "default") == 0 ? NULL : xstrdup(arg);
            /* /provider re-resolves the backend; an explicit /effort
             * (default included) must survive that, not be replaced by a
             * settings.json default (docs/adr/0015) */
            t->ctx->effort_explicit = true;
            t->ctx->effort_from_settings = false;
            tny_engine_effort_changed(t->engine, previous,
                                      t->ctx->reasoning_effort, "command");
            free(previous);
            if (!t->engine) tui_prewarm_start(t);
            if (!t->ctx->reasoning_effort)
                tui_linef(t, "  reasoning effort: provider default");
            else
                tui_linef(t, "  reasoning effort: %s%s (next turn on)", arg,
                          tny_effort_canonical(arg)
                              ? "" : " (provider-advertised value, unverified)");
        }
        t->dirty = true;
    } else if (strcmp(c, "max-steps") == 0) {
        /* Cap on model calls per native-loop turn; 0 = unlimited (the
         * default). Read at step boundaries, so it applies immediately with
         * no rebind. Host providers run their own loops and ignore it. */
        if (!arg || !*arg) {
            if (t->ctx->max_steps > 0)
                tui_linef(t, "  max steps: %d per turn", t->ctx->max_steps);
            else
                tui_linef(t, "  max steps: unlimited");
            tui_sys(t, "usage: /max-steps set N | /max-steps clear");
        } else {
            const char *val = arg;
            if (strncmp(arg, "set", 3) == 0 && (arg[3] == 0 || arg[3] == ' ')) {
                val = arg + 3;
                while (*val == ' ') val++;
            } else if (strcmp(arg, "clear") == 0) {
                val = "unlimited";
            }
            int v = tny_parse_max_steps(val);
            if (v < 0) {
                tui_err(t, "usage: /max-steps set N | /max-steps clear");
            } else {
                t->ctx->max_steps = v;
                if (v > 0) tui_linef(t, "  max steps: %d per turn", v);
                else tui_linef(t, "  max steps: unlimited");
            }
        }
        t->dirty = true;
    } else if (strcmp(c, "status") == 0) {
        run_cli(t, cmd_status, 0, NULL);
    } else if (strcmp(c, "usage") == 0) {
        run_cli(t, cmd_usage, 0, NULL);
    } else if (strcmp(c, "workspace") == 0) {
        char *av[4];
        int ac = arg ? tokenize(arg, av, 4) : 0;
        if (ac) tui_prewarm_drop(t); /* add/remove edit dirs the warm-up reads */
        int rc = run_cli(t, cmd_workspace, ac, av);
        if (rc == 0 && ac) {
            const char *path = ac > 1 ? av[1] : NULL;
            tny_engine_workspace_changed(t->engine, av[0], path);
        }
        tui_files_free(t);
        if (ac && !t->engine) tui_prewarm_start(t);
    } else if (strcmp(c, "setup") == 0) {
        char *av[8];
        int ac = arg ? tokenize(arg, av, 8) : 0;
        run_cli(t, cmd_setup, ac, av);
    } else if (strcmp(c, "login") == 0) {
        run_cli(t, cmd_login, 0, NULL);
    } else if (strcmp(c, "logout") == 0) {
        run_cli(t, cmd_logout, 0, NULL);
    } else if (strcmp(c, "mcp") == 0) {
        cmd_mcp(t);
    } else if (strcmp(c, "skills") == 0) {
        cmd_skills(t);
    } else if (strcmp(c, "image") == 0) {
        if (!arg || !*arg) tui_sys(t, "usage: /image PATH");
        else if (t->n_images >= TUI_MAX_IMAGES) tui_sys(t, "too many images queued");
        else {
            int n = tui_queue_image(t, arg);
            if (!n) tui_err(t, "no such file");
            else tui_linef(t, "  attached %s", t->images[n - 1]);
        }
        t->dirty = true;
    } else if (strcmp(c, "undo") == 0) {
        cmd_undo(t);
    } else if (strcmp(c, "copy") == 0) {
        cmd_copy(t);
    } else if (strcmp(c, "trace") == 0) {
        t->trace = !t->trace;
        tui_linef(t, "  trace: %s", t->trace ? "on" : "off");
    } else if (strcmp(c, "transcript") == 0) {
        cmd_transcript(t);
    } else {
        tui_err(t, "unknown command — /help lists them");
    }
    free(copy);
}

/* ---- /provider setup wizard (docs/adr/0018) ----
 * A short Q&A through the composer, so it works identically in a native
 * terminal and the browser wasm build (where it is the main way to add a
 * provider). While active, tui_submit routes lines here. */

static void wiz_prompt(tui *t) {
    switch (t->wiz_step) {
    case 1:
        tui_sys(t, "  provider name (e.g. openrouter) — /cancel aborts:");
        break;
    case 2:
        tui_sys(t, "  base url (OpenAI-compatible /v1 endpoint; empty keeps"
                   " the current one for an existing provider):");
        break;
    case 3:
        tui_sys(t, "  api key — typed input is visible; $ENV_NAME reads an"
                   " env var instead; empty skips:");
        break;
    case 4:
        tui_sys(t, "  default model (empty skips):");
        break;
    }
    t->dirty = true;
}

void tui_wizard_start(tui *t, const char *name) {
    tui_wizard_cancel(t); /* drop any half-finished run */
    if (name && *name) {
        t->wiz_name = xstrdup(name);
        t->wiz_step = 2;
    } else {
        t->wiz_step = 1;
    }
    tui_sys(t, "  provider setup — answers go to ~/.tny/settings.json");
    wiz_prompt(t);
}

void tui_wizard_cancel(tui *t) {
    if (!t->wiz_step) return;
    t->wiz_step = 0;
    free(t->wiz_name); t->wiz_name = NULL;
    free(t->wiz_base); t->wiz_base = NULL;
    free(t->wiz_key); t->wiz_key = NULL;
    free(t->wiz_key_env); t->wiz_key_env = NULL;
    free(t->wiz_model); t->wiz_model = NULL;
}

static bool wiz_base_ok(const char *url) {
    url_parts u;
    return url_parse(url, &u) == 0 &&
           (strcmp(u.scheme, "http") == 0 || strcmp(u.scheme, "https") == 0);
}

static void wiz_finish(tui *t) {
    tny_provider_fields f = {
        t->wiz_base, t->wiz_key, t->wiz_key_env, t->wiz_model, NULL};
    char err[256];
    if (tny_provider_write_profile(t->ctx, t->wiz_name, &f,
                                   err, sizeof err) != 0) {
        tui_err(t, err);
        tui_wizard_cancel(t);
        return;
    }
    char *name = xstrdup(t->wiz_name);
    tui_wizard_cancel(t);
    tui_prewarm_drop(t);
    tny_resolve_backend(t->ctx, name);
    tui_drop_backend(t);
    tny_settings_remember_use(t->ctx);
    tui_linef(t, "  provider '%s' ready (model %s) — saved to settings.json",
              name, t->ctx->model ? t->ctx->model : "default");
    if (!t->ctx->api_key)
        tui_sys(t, "  no key resolved yet: the provider may refuse requests");
    free(name);
    t->dirty = true;
}

void tui_wizard_feed(tui *t, const char *line) {
    if (strcmp(line, "/cancel") == 0 || strcmp(line, "/quit") == 0) {
        tui_wizard_cancel(t);
        tui_sys(t, "  provider setup cancelled");
        if (strcmp(line, "/quit") == 0) t->quit = true;
        t->dirty = true;
        return;
    }
    switch (t->wiz_step) {
    case 1:
        if (!*line) { wiz_prompt(t); return; }
        if (tny_backend_from_name(line) >= 0 && strcmp(line, "openai") != 0) {
            tui_err(t, "host providers (cursor|codex|acp) have no base_url; "
                       "pick another name");
            wiz_prompt(t);
            return;
        }
        t->wiz_name = xstrdup(line);
        t->wiz_step = 2;
        wiz_prompt(t);
        return;
    case 2: {
        bool exists = strcmp(t->wiz_name, "openai") == 0 ||
                      tny_custom_provider_exists(t->ctx, t->wiz_name);
        if (!*line && !exists) {
            tui_err(t, "a new provider needs a base url");
            wiz_prompt(t);
            return;
        }
        if (*line && !wiz_base_ok(line)) {
            tui_err(t, "base url must be http(s)://host[/prefix]");
            wiz_prompt(t);
            return;
        }
        if (*line) t->wiz_base = xstrdup(line);
        t->wiz_step = 3;
        wiz_prompt(t);
        return;
    }
    case 3:
        if (line[0] == '$' && line[1]) t->wiz_key_env = xstrdup(line + 1);
        else if (*line) t->wiz_key = xstrdup(line);
        t->wiz_step = 4;
        wiz_prompt(t);
        return;
    case 4:
        if (*line) t->wiz_model = xstrdup(line);
        wiz_finish(t);
        return;
    }
}

