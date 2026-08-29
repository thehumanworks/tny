/* cmd_misc.c — status, permissions, models, workspace, backends, usage,
 * login/logout/setup. All support --json where docs/cli.md requires it. */
#include "cli/cli.h"
#include "core/backend.h"
#include "core/tasks.h"
#include "core/session.h"
#include "backends/codex/codex.h" /* tny_codex_login */
#include "net/net.h"
#include "util/util.h"
#include "util/tny_poll.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>

static bool wants_json(const cli_globals *g, int argc, char **argv) {
    if (g->json) return true;
    for (int i = 0; i < argc; i++)
        if (strcmp(argv[i], "--json") == 0) return true;
    return false;
}

int cmd_tasks(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    bool json = g->json;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) json = true;
        else {
            fprintf(stderr, "tny: tasks: unexpected argument '%s'\nUsage: tny [--json] tasks\n",
                    argv[i]);
            return 1;
        }
    }
    tny_task_info *items = NULL;
    size_t count = 0;
    if (tny_task_list(ctx, &items, &count) != TNY_TASK_OK) return 1;
    if (json) {
        buf_t b;
        buf_init(&b);
        buf_appends(&b, "{\"kind\":\"tasks\",\"tasks\":[");
        for (size_t i = 0; i < count; i++) {
            if (i) buf_appends(&b, ",");
            buf_appends(&b, "{\"name\":");
            jescape(&b, items[i].name);
            buf_appends(&b, ",\"source\":");
            jescape(&b, items[i].source);
            buf_appends(&b, ",\"description\":");
            if (items[i].description) jescape(&b, items[i].description);
            else buf_appends(&b, "null");
            buf_appendf(&b, ",\"valid\":%s}", items[i].valid ? "true" : "false");
        }
        buf_appends(&b, "]}\n");
        fwrite(b.data, 1, b.len, stdout);
        buf_free(&b);
    } else {
        for (size_t i = 0; i < count; i++)
            printf("%-20s %-9s %s%s\n", items[i].name, items[i].source,
                   items[i].valid ? "" : "[invalid] ",
                   items[i].description ? items[i].description : "");
    }
    tny_task_list_free(items, count);
    return 0;
}

int cmd_task(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    bool json = g->json;
    const char *name = NULL;
    if (argc < 1 || strcmp(argv[0], "show") != 0) {
        fprintf(stderr, "tny: task: expected `show NAME`\nUsage: tny [--json] task show NAME\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) json = true;
        else if (!name) name = argv[i];
        else {
            fprintf(stderr,
                    "tny: task show: unexpected argument '%s'\n"
                    "Usage: tny [--json] task show NAME\n",
                    argv[i]);
            return 1;
        }
    }
    if (!name) {
        fprintf(stderr, "tny: task show: NAME is required\nUsage: tny [--json] task show NAME\n");
        return 1;
    }
    if (!tny_task_name_valid(name)) {
        fprintf(stderr, "tny: task show: invalid task name '%s'\n", name);
        return 1;
    }

    tny_task_info *items = NULL;
    size_t count = 0;
    if (tny_task_list(ctx, &items, &count) != TNY_TASK_OK) {
        fprintf(stderr, "tny: task show: could not inspect task presets\n");
        return 1;
    }
    const tny_task_info *info = NULL;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(items[i].name, name) == 0) {
            info = &items[i];
            break;
        }
    }
    if (!info || !info->valid) {
        fprintf(stderr, "tny: task show: %s task '%s'\n", info ? "invalid" : "unknown", name);
        tny_task_list_free(items, count);
        return 1;
    }
    if (tny_task_apply(ctx, name) != TNY_TASK_OK) {
        fprintf(stderr, "tny: task show: task '%s' changed or became unreadable\n", name);
        tny_task_list_free(items, count);
        return 1;
    }

    if (json) {
        buf_t out;
        buf_init(&out);
        buf_appends(&out, "{\"kind\":\"task\",\"name\":");
        jescape(&out, ctx->task_name);
        buf_appends(&out, ",\"source\":");
        jescape(&out, ctx->task_source);
        buf_appends(&out, ",\"digest\":");
        jescape(&out, ctx->task_digest);
        buf_appends(&out, ",\"description\":");
        if (info->description) jescape(&out, info->description);
        else buf_appends(&out, "null");
        buf_appends(&out, ",\"instructions\":");
        jescape(&out, ctx->task_instructions);
        buf_appends(&out, "}\n");
        if (buf_oom(&out)) {
            buf_free(&out);
            tny_task_list_free(items, count);
            fprintf(stderr, "tny: task show: out of memory\n");
            return 1;
        }
        fwrite(out.data, 1, out.len, stdout);
        buf_free(&out);
    } else {
        printf("task:        %s\nsource:      %s\ndigest:      %s\n", ctx->task_name,
               ctx->task_source, ctx->task_digest);
        if (info->description) printf("description: %s\n", info->description);
        printf("\n%s", ctx->task_instructions);
        size_t n = strlen(ctx->task_instructions);
        if (!n || ctx->task_instructions[n - 1] != '\n') putchar('\n');
    }
    tny_task_list_free(items, count);
    return 0;
}

int cmd_status(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    bool json = wants_json(g, argc, argv);
    int n = 0;
    session_meta *m = session_list(ctx, false, 100, NULL, &n);
    session_meta_free(m, n);
    const char *bk = tny_provider_name(ctx);
    const char *model = ctx->model ? ctx->model : "default";
    bool auth = ctx->api_key != NULL || str_starts(ctx->base_url, "http://");
    if (json) {
        buf_t b;
        buf_init(&b);
        buf_appendf(&b, "{\"kind\":\"status\",\"version\":\"%s\",\"backend\":\"%s\",\"model\":",
                    TNY_VERSION, bk);
        jescape(&b, model);
        if (ctx->reasoning_effort) {
            buf_appends(&b, ",\"reasoning_effort\":");
            jescape(&b, ctx->reasoning_effort);
        }
        buf_appendf(&b,
                    ",\"auth\":\"%s\",\"permission_mode\":\"%s\",\"sandbox\":\"%s\","
                    "\"workspace\":",
                    auth ? "ok" : "missing", tny_perm_mode_name(ctx->perm_mode),
                    strcmp(ctx->sandbox_mode, "os") == 0 ? "os" : "none");
        jescape(&b, ctx->cwd);
        buf_appends(&b, ",\"task\":");
        if (ctx->task_name) {
            buf_appends(&b, "{\"name\":");
            jescape(&b, ctx->task_name);
            buf_appends(&b, ",\"source\":");
            jescape(&b, ctx->task_source ? ctx->task_source : "unknown");
            buf_appends(&b, ",\"digest\":");
            jescape(&b, ctx->task_digest);
            buf_appends(&b, "}");
        } else buf_appends(&b, "null");
        buf_appendf(&b,
                    ",\"sessions\":%d,\"agent_step_limit\":%d,"
                    "\"extensions_enabled\":%s,"
                    "\"extension_iteration_limit\":%d}\n",
                    n, ctx->max_steps, ctx->extensions_enabled ? "true" : "false",
                    ctx->max_extension_iterations);
        fwrite(b.data, 1, b.len, stdout);
        buf_free(&b);
    } else {
        printf("tny v%s\n", TNY_VERSION);
        printf("provider:   %s\n", bk);
        printf("model:      %s\n", model);
        if (ctx->reasoning_effort) printf("effort:     %s\n", ctx->reasoning_effort);
        printf("auth:       %s\n", auth ? "ok" : "missing (set OPENAI_API_KEY or run tny setup)");
        printf("permission: %s\n", tny_perm_mode_name(ctx->perm_mode));
        printf("sandbox:    %s\n",
               strcmp(ctx->sandbox_mode, "os") == 0 ? "os (unsupported: effective none)" : "none");
        printf("workspace:  %s\n", ctx->cwd);
        if (ctx->task_name)
            printf("task:       %s (%s)\n", ctx->task_name,
                   ctx->task_source ? ctx->task_source : "unknown");
        else printf("task:       none\n");
        for (int i = 0; i < ctx->n_extra_dirs; i++) printf("extra dir:  %s\n", ctx->extra_dirs[i]);
        printf("sessions:   %d\n", n);
        printf("extensions: %s (continuations: ", ctx->extensions_enabled ? "enabled" : "disabled");
        if (ctx->max_extension_iterations > 0) printf("max %d)\n", ctx->max_extension_iterations);
        else printf("unlimited)\n");
    }
    return 0;
}

int cmd_permissions(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    bool json = wants_json(g, argc, argv);
    yyjson_val *sroot = ctx->settings ? yyjson_doc_get_root(ctx->settings) : NULL;
    yyjson_val *rules = jget(sroot, "permission");
    char *rules_json = rules ? jwrite_val(rules) : NULL;
    if (json) {
        buf_t b;
        buf_init(&b);
        buf_appendf(&b, "{\"kind\":\"permissions\",\"mode\":\"%s\",\"rules\":%s}\n",
                    tny_perm_mode_name(ctx->perm_mode), rules_json ? rules_json : "{}");
        fwrite(b.data, 1, b.len, stdout);
        buf_free(&b);
    } else {
        printf("mode: %s\n", tny_perm_mode_name(ctx->perm_mode));
        printf("rules (~/.tny/settings.json \"permission\"): %s\n",
               rules_json ? rules_json : "(none)");
        printf("session grants: die with the process\n");
    }
    free(rules_json);
    return 0;
}

/* Print one normalized [{"id","name","efforts":[…]},…] catalog. */
static void models_print(tny_ctx *ctx, const char *json_arr, bool json) {
    const char *name = tny_provider_name(ctx);
    if (json) {
        printf("{\"kind\":\"models\",\"provider\":\"%s\",\"models\":%s}\n", name, json_arr);
        return;
    }
    yyjson_doc *doc = jparse(json_arr, strlen(json_arr));
    yyjson_val *arr = doc ? yyjson_doc_get_root(doc) : NULL;
    size_t idx, max, n = 0;
    yyjson_val *m;
    if (arr && yyjson_is_arr(arr)) {
        yyjson_arr_foreach(arr, idx, max, m) {
            const char *id = jget_str(m, "id");
            const char *nm = jget_str(m, "name");
            if (!id) continue;
            n++;
            bool active = ctx->model && strcmp(ctx->model, id) == 0;
            printf("%s%s%s%s", id, nm ? "  —  " : "", nm ? nm : "", active ? "  (active)" : "");
            yyjson_val *ef = jget(m, "efforts");
            if (ef && yyjson_is_arr(ef) && yyjson_arr_size(ef)) {
                size_t ei, emax;
                yyjson_val *e;
                printf("  [effort:");
                yyjson_arr_foreach(ef, ei, emax, e) if (yyjson_is_str(e))
                    printf(" %s", yyjson_get_str(e));
                printf("]");
            }
            printf("\n");
        }
    }
    if (!n) printf("%s\n", ctx->model ? ctx->model : "default");
    yyjson_doc_free(doc);
}

/* Providers without a catalog still answer: the configured model or the
 * host's default. */
static int models_fallback(tny_ctx *ctx, bool json) {
    if (json) {
        buf_t j;
        buf_init(&j);
        buf_appends(&j, "[{\"id\":");
        jescape(&j, ctx->model ? ctx->model : "default");
        buf_appends(&j, "}]");
        models_print(ctx, j.data, true);
        buf_free(&j);
    } else {
        printf("%s (no catalog from the %s provider; use --model ID to override)\n",
               ctx->model ? ctx->model : "default", tny_provider_name(ctx));
    }
    return 0;
}

int cmd_models(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    bool json = wants_json(g, argc, argv);
    if (ctx->backend != TNY_BK_OPENAI) {
        tny_backend *bk = tny_backend_create((tny_backend_id)ctx->backend, ctx);
        if (!bk) return 1;
        if (!bk->list_models) {
            bk->destroy(bk);
            return models_fallback(ctx, json);
        }
        char err[512];
        int rc = 1;
        if (bk->connect(bk, err, sizeof err) != 0) {
            fprintf(stderr, "tny: %s\n", err);
            models_fallback(ctx, json);
        } else {
            char *arr = NULL;
            if (bk->list_models(bk, &arr, err, sizeof err) == 0 && arr) {
                models_print(ctx, arr, json);
                free(arr);
                rc = 0;
            } else {
                fprintf(stderr, "tny: %s\n", err);
                models_fallback(ctx, json);
            }
            if (bk->disconnect) bk->disconnect(bk);
        }
        bk->destroy(bk);
        return rc;
    }
    char err[256];
    http_conn *c = http_open(ctx->base_url, err, sizeof err);
    if (!c) {
        fprintf(stderr, "tny: %s\n", err);
        if (ctx->model) printf("%s (configured)\n", ctx->model);
        return 1;
    }
    buf_t auth;
    buf_init(&auth);
    buf_appendf(&auth, "%s: %s%s", ctx->auth_header_name, ctx->auth_header_prefix,
                ctx->api_key ? ctx->api_key : "");
    const char *hdrs[12];
    int hn = 0;
    if (ctx->api_key) hdrs[hn++] = auth.data;
    for (char **e = ctx->extra_headers; e && *e && hn < 11; e++)
        hdrs[hn++] = *e; /* builtin-profile headers (docs/adr/0019) */
    hdrs[hn] = NULL;
    buf_t path;
    buf_init(&path);
    buf_appendf(&path, "%s/models", http_prefix(c));
    int status = -1;
    buf_t body;
    buf_init(&body);
    if (http_request(c, "GET", path.data, hdrs, NULL, 0) == 0 &&
        (status = http_read_response(c, 15000)) > 0) {
        int64_t deadline = now_ms() + 15000;
        for (;;) {
            char tmp[8192];
            ssize_t n = http_body_read(c, tmp, sizeof tmp);
            if (n == 0) break;
            if (n == -2) {
                if (now_ms() > deadline) break;
                struct pollfd pf = {http_fd(c), POLLIN, 0};
                tny_poll(&pf, 1, 500);
                continue;
            }
            if (n < 0) break;
            buf_append(&body, tmp, (size_t)n);
        }
    }
    buf_free(&auth);
    buf_free(&path);
    http_close(c);

    if (status != 200) {
        if (status == 401 || status == 403)
            fprintf(stderr,
                    "tny: /models refused the credentials (HTTP %d): %s; "
                    "showing configured\n",
                    status, ctx->api_key ? "check the API key" : "no API key in this environment");
        else
            fprintf(stderr,
                    "tny: provider has no /models (HTTP %d); "
                    "showing configured\n",
                    status);
        buf_free(&body);
        return models_fallback(ctx, json);
    }
    yyjson_doc *doc = jparse(body.data, body.len);
    buf_free(&body);
    buf_t out;
    buf_init(&out);
    if (json) buf_appends(&out, "{\"kind\":\"models\",\"models\":[");
    int count = 0;
    if (doc) {
        yyjson_val *data = jget(yyjson_doc_get_root(doc), "data");
        if (data && yyjson_is_arr(data)) {
            size_t idx, max;
            yyjson_val *v;
            yyjson_arr_foreach(data, idx, max, v) {
                const char *id = jget_str(v, "id");
                if (!id) continue;
                if (json) {
                    if (count) buf_appends(&out, ",");
                    jescape(&out, id);
                } else buf_appendf(&out, "%s\n", id);
                count++;
            }
        }
        yyjson_doc_free(doc);
    }
    if (json) buf_appends(&out, "]}\n");
    fwrite(out.data, 1, out.len, stdout);
    buf_free(&out);
    return 0;
}

int cmd_workspace(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    bool json = wants_json(g, argc, argv);
    const char *sub = argc > 0 ? argv[0] : "list";
    if (strcmp(sub, "list") == 0 || strcmp(sub, "--json") == 0) {
        if (json) {
            buf_t b;
            buf_init(&b);
            buf_appends(&b, "{\"kind\":\"workspace\",\"primary\":");
            jescape(&b, ctx->cwd);
            buf_appends(&b, ",\"additional\":[");
            for (int i = 0; i < ctx->n_extra_dirs; i++) {
                if (i) buf_appends(&b, ",");
                jescape(&b, ctx->extra_dirs[i]);
            }
            buf_appends(&b, "]}\n");
            fwrite(b.data, 1, b.len, stdout);
            buf_free(&b);
        } else {
            printf("primary: %s\n", ctx->cwd);
            for (int i = 0; i < ctx->n_extra_dirs; i++) printf("extra:   %s\n", ctx->extra_dirs[i]);
            if (!ctx->n_extra_dirs) printf("(no additional directories)\n");
        }
        return 0;
    }
    if (strcmp(sub, "add") == 0 && argc > 1) return tny_workspace_add(ctx, argv[1]) == 0 ? 0 : 1;
    if (strcmp(sub, "remove") == 0 && argc > 1)
        return tny_workspace_remove(ctx, argv[1]) == 0 ? 0 : 1;
    if (strcmp(sub, "clear") == 0) return tny_workspace_clear(ctx) == 0 ? 0 : 1;
    fprintf(stderr, "tny: workspace list|add DIR|remove DIR|clear\n");
    return 1;
}

/* One `tny providers` row for a user-named OpenAI-compatible provider.
 * profile_bu is the settings base_url or NULL; NAME_BASE_URL beats it,
 * mirroring resolution. */
static void custom_provider_row(tny_ctx *ctx, buf_t *b, bool json, const char *name,
                                const char *profile_bu, const char *origin) {
    char *buvar = tny_provider_env_var(name, "_BASE_URL");
    const char *envbu = buvar ? getenv(buvar) : NULL;
    free(buvar);
    const char *bu = envbu && *envbu ? envbu : profile_bu;
    char *keyvar = tny_custom_provider_key_env(ctx, name);
    const char *key = keyvar ? getenv(keyvar) : NULL;
    bool healthy = (key && *key) || (bu && str_starts(bu, "http://"));
    char line[256];
    snprintf(line, sizeof line, "openai-compatible (%s): base_url %s (key env %s%s)", origin,
             bu ? bu : "?", keyvar ? keyvar : "?", key && *key ? "" : " unset");
    free(keyvar);
    bool active = ctx->provider_name && strcmp(ctx->provider_name, name) == 0;
    if (json) {
        buf_appends(b, ",{\"name\":");
        jescape(b, name);
        buf_appendf(b, ",\"active\":%s,\"healthy\":%s,\"hint\":", active ? "true" : "false",
                    healthy ? "true" : "false");
        jescape(b, line);
        buf_appends(b, "}");
    } else {
        buf_appendf(b, "%s %s — %s\n", active ? "*" : " ", name, line);
    }
}

static bool acp_name_valid(const char *name) {
    if (!name || !*name) return false;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_'))
            return false;
    }
    return true;
}

static bool executable_on_path(const char *bin) {
    if (!bin || !*bin) return false;
    if (strchr(bin, '/')) return access(bin, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path) return false;
    char *dup = xstrdup(path);
    bool found = false;
    for (char *p = strtok(dup, ":"); p && !found; p = strtok(NULL, ":")) {
        char *full = path_join(p, bin);
        found = full && access(full, X_OK) == 0;
        free(full);
    }
    free(dup);
    return found;
}

/* One provider-list row for settings.acp.NAME (legacy acp.agents supported).
 * Never render argv: command arguments may contain local paths or user
 * mistakes that should not become diagnostic output. */
static void acp_provider_row(tny_ctx *ctx, buf_t *b, bool json, const char *name,
                             yyjson_val *profile) {
    if (!acp_name_valid(name) || !yyjson_is_obj(profile)) return;
    yyjson_val *command = jget(profile, "command");
    yyjson_val *args = jget(profile, "args");
    bool legacy = yyjson_is_arr(command);
    bool valid =
        legacy ? yyjson_arr_size(command) > 0 : yyjson_is_str(command) && *yyjson_get_str(command);
    const char *exe = legacy ? NULL : yyjson_get_str(command);
    if (valid && args && (!yyjson_is_arr(args) || legacy)) valid = false;
    if (valid && legacy) {
        size_t idx, max;
        yyjson_val *v;
        yyjson_arr_foreach(command, idx, max, v) {
            const char *arg = yyjson_is_str(v) ? yyjson_get_str(v) : NULL;
            if (!arg || !*arg) {
                valid = false;
                break;
            }
            if (idx == 0) exe = arg;
        }
    } else if (valid && yyjson_is_arr(args)) {
        size_t idx, max;
        yyjson_val *v;
        yyjson_arr_foreach(args, idx, max, v) {
            const char *arg = yyjson_is_str(v) ? yyjson_get_str(v) : NULL;
            if (!arg || !*arg) {
                valid = false;
                break;
            }
        }
    }
    bool remote = exe && (str_starts(exe, "ws://") || str_starts(exe, "wss://"));
    size_t nargs = yyjson_is_arr(args) ? yyjson_arr_size(args) : 0;
    if (remote && ((legacy && yyjson_arr_size(command) != 1) || nargs != 0)) valid = false;
    yyjson_val *model = jget(profile, "model");
    if (model && (!yyjson_is_str(model) || !*yyjson_get_str(model))) valid = false;
    bool healthy = valid && (remote || executable_on_path(exe));
    const char *hint = !valid    ? "invalid settings.json ACP profile"
                       : healthy ? (remote ? "configured remote ACP agent"
                                           : "configured ACP agent; command resolves")
                                 : "configured ACP agent; command not found on PATH";
    buf_t full;
    buf_init(&full);
    buf_appendf(&full, "acp@%s", name);
    bool active = ctx->provider_name && strcmp(ctx->provider_name, full.data) == 0;
    if (json) {
        buf_appends(b, ",{\"name\":");
        jescape(b, full.data);
        buf_appendf(b, ",\"backend\":\"acp\",\"active\":%s,\"healthy\":%s,\"hint\":",
                    active ? "true" : "false", healthy ? "true" : "false");
        jescape(b, hint);
        buf_appends(b, "}");
    } else {
        buf_appendf(b, "%s %s — %s\n", active ? "*" : " ", full.data, hint);
    }
    buf_free(&full);
}

int cmd_backends(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    bool json = wants_json(g, argc, argv);
    buf_t b;
    buf_init(&b);
    if (json) buf_appends(&b, "{\"kind\":\"providers\",\"providers\":[");
    for (int i = 0; i < TNY_BK_COUNT; i++) {
        tny_backend *bk = tny_backend_create((tny_backend_id)i, ctx);
        char line[256] = "unknown";
        int healthy = 1;
        if (bk && bk->doctor) healthy = bk->doctor(ctx, line, sizeof line);
        if (bk) bk->destroy(bk);
        bool active = i == ctx->backend && !ctx->provider_name;
        if (json) {
            if (i) buf_appends(&b, ",");
            buf_appendf(&b, "{\"name\":\"%s\",\"active\":%s,\"healthy\":%s,\"hint\":",
                        tny_backend_name((tny_backend_id)i), active ? "true" : "false",
                        healthy == 0 ? "true" : "false");
            jescape(&b, line);
            buf_appends(&b, "}");
        } else {
            buf_appendf(&b, "%s %s — %s\n", active ? "*" : " ", tny_backend_name((tny_backend_id)i),
                        line);
        }
    }
    /* builtin subscription profiles (docs/adr/0019); a settings/env profile
     * of the same name shadows the builtin and is listed below instead */
    static const char *const builtin_profiles[] = {"claude", "grok"};
    for (size_t bi = 0; bi < sizeof builtin_profiles / sizeof *builtin_profiles; bi++) {
        const char *name = builtin_profiles[bi];
        if (tny_custom_provider_exists(ctx, name)) continue;
        char line[256];
        bool healthy;
        if (strcmp(name, "claude") == 0) {
            const char *source = NULL;
            char *tok = tny_claude_token(&source);
            healthy = tok != NULL;
            if (tok) {
                memset(tok, 0, strlen(tok));
                free(tok);
            }
            snprintf(line, sizeof line,
                     healthy ? "claude: credential from %s (Anthropic OpenAI-compat)"
                             : "claude: no credential (run `tny --provider claude login`)%s",
                     healthy ? source : "");
        } else {
            char *sess = tny_grok_session_token();
            const char *xk = getenv("XAI_API_KEY");
            healthy = sess != NULL || (xk && *xk);
            if (sess) {
                snprintf(line, sizeof line,
                         "grok: session token (~/.grok/auth.json, CLI chat proxy)");
                memset(sess, 0, strlen(sess));
                free(sess);
            } else if (xk && *xk) {
                snprintf(line, sizeof line, "grok: XAI_API_KEY set (api.x.ai)");
            } else {
                snprintf(line, sizeof line,
                         "grok: no credential (run `tny --provider grok login`)");
            }
        }
        bool active = ctx->provider_name && strcmp(ctx->provider_name, name) == 0;
        if (json) {
            buf_appends(&b, ",{\"name\":");
            jescape(&b, name);
            buf_appendf(&b, ",\"active\":%s,\"healthy\":%s,\"hint\":", active ? "true" : "false",
                        healthy ? "true" : "false");
            jescape(&b, line);
            buf_appends(&b, "}");
        } else {
            buf_appendf(&b, "%s %s — %s\n", active ? "*" : " ", name, line);
        }
    }
    /* user-named OpenAI-compatible providers: settings.json profiles first,
     * then env-only ones (NAME_BASE_URL with no settings entry) */
    if (ctx->settings) {
        yyjson_val *root = yyjson_doc_get_root(ctx->settings);
        size_t idx, max;
        yyjson_val *k, *v;
        if (yyjson_is_obj(root)) yyjson_obj_foreach(root, idx, max, k, v) {
                const char *name = yyjson_get_str(k);
                if (!name || !yyjson_is_obj(v)) continue;
                const char *bu = jget_str(v, "base_url");
                if (!bu || !*bu || !tny_custom_provider_exists(ctx, name)) continue;
                custom_provider_row(ctx, &b, json, name, bu, "settings");
            }
        yyjson_val *acp = jget(root, "acp");
        yyjson_val *agents = jget(acp, "agents");
        if (!yyjson_is_obj(agents)) agents = acp;
        if (yyjson_is_obj(agents)) yyjson_obj_foreach(agents, idx, max, k, v) {
                const char *name = yyjson_get_str(k);
                if (name && strcmp(name, "agents") != 0) acp_provider_row(ctx, &b, json, name, v);
            }
    }
    int n_env = 0;
    char **envnames = tny_env_provider_names(&n_env);
    for (int i = 0; i < n_env; i++) {
        const char *name = envnames[i];
        yyjson_val *root = ctx->settings ? yyjson_doc_get_root(ctx->settings) : NULL;
        const char *in_settings = jget_str(jget(root, name), "base_url");
        if (in_settings && *in_settings) {
            free(envnames[i]);
            continue; /* listed above */
        }
        custom_provider_row(ctx, &b, json, name, NULL, "env");
        free(envnames[i]);
    }
    free(envnames);
    if (json) buf_appends(&b, "]}\n");
    fwrite(b.data, 1, b.len, stdout);
    buf_free(&b);
    return 0;
}

int cmd_usage(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    bool json = wants_json(g, argc, argv);
    int n = 0;
    session_meta *m = session_list(ctx, false, 100, NULL, &n);
    int64_t tin = 0, tout = 0;
    for (int i = 0; i < n; i++) {
        tny_session_state *s = session_open(ctx, m[i].id);
        if (s) {
            int64_t a, o;
            session_get_usage(s, &a, &o);
            tin += a;
            tout += o;
            session_close(s);
        }
    }
    session_meta_free(m, n);
    if (json)
        printf("{\"kind\":\"usage\",\"sessions\":%d,\"input_tokens\":%lld,"
               "\"output_tokens\":%lld}\n",
               n, (long long)tin, (long long)tout);
    else
        printf("workspace sessions: %d\ninput tokens:  %lld\noutput tokens: %lld\n", n,
               (long long)tin, (long long)tout);
    return 0;
}

int cmd_setup(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    const char *base_url = g->base_url, *key_env = g->api_key_env, *model = g->model;
    for (int i = 0; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--base-url") == 0) base_url = argv[++i];
        else if (strcmp(argv[i], "--api-key-env") == 0) key_env = argv[++i];
        else if (strcmp(argv[i], "--model") == 0) model = argv[++i];
    }
    if (!base_url && !key_env && !model) {
        fprintf(stderr, "tny: setup needs flags (no interactive prompts)\n"
                        "Example:\n  tny setup --base-url https://openrouter.ai/api/v1 "
                        "--api-key-env OPENROUTER_API_KEY --model anthropic/claude-sonnet-4.6\n");
        return 1;
    }
    /* merge into settings "openai" object */
    yyjson_mut_doc *doc =
        ctx->settings ? yyjson_doc_mut_copy(ctx->settings, NULL) : yyjson_mut_doc_new(NULL);
    if (!yyjson_mut_doc_get_root(doc)) yyjson_mut_doc_set_root(doc, yyjson_mut_obj(doc));
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    yyjson_mut_val *oa = yyjson_mut_obj_get(root, "openai");
    if (!oa) {
        oa = yyjson_mut_obj(doc);
        yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc, "openai"), oa);
    }
    if (base_url)
        yyjson_mut_obj_put(oa, yyjson_mut_strcpy(doc, "base_url"),
                           yyjson_mut_strcpy(doc, base_url));
    if (key_env)
        yyjson_mut_obj_put(oa, yyjson_mut_strcpy(doc, "api_key_env"),
                           yyjson_mut_strcpy(doc, key_env));
    if (model)
        yyjson_mut_obj_put(oa, yyjson_mut_strcpy(doc, "model"), yyjson_mut_strcpy(doc, model));
    char *out = jwrite_pretty(doc);
    yyjson_mut_doc_free(doc);
    if (!out) return 1;
    mkdir_p(ctx->tny_dir);
    int rc = file_write_atomic(ctx->settings_path, out, strlen(out));
    free(out);
    if (rc != 0) {
        fprintf(stderr, "tny: could not write %s\n", ctx->settings_path);
        return 1;
    }
    printf("wrote provider config to %s\n", ctx->settings_path);
    if (key_env && !getenv(key_env)) printf("note: $%s is not set in this shell\n", key_env);
    return 0;
}

/* Claude: report the credential tny would use, or hand the browser ceremony
 * to `claude setup-token` (prints a one-year OAuth token the user exports as
 * CLAUDE_CODE_OAUTH_TOKEN). tny never captures or stores the token itself. */
static int login_claude(tny_ctx *ctx) {
    (void)ctx;
    const char *source = NULL;
    char *tok = tny_claude_token(&source);
    if (tok) {
        printf("Claude credential found (%s) — `tny --provider claude` uses it.\n", source);
        memset(tok, 0, strlen(tok));
        free(tok);
        return 0;
    }
    const char *bin = getenv("TNY_CLAUDE_BIN");
    if (!bin || !*bin) bin = "claude";
    printf("No Claude credential. Starting `%s setup-token` (browser sign-in;\n"
           "requires a Pro/Max/Team/Enterprise subscription)…\n",
           bin);
    fflush(stdout);
    buf_t cmd;
    buf_init(&cmd);
    buf_appendf(&cmd, "%s setup-token", bin);
    int rc = system(cmd.data);
    buf_free(&cmd);
    if (rc != 0) {
        fprintf(stderr,
                "tny: `%s setup-token` failed. Install the Claude Code CLI "
                "(or set TNY_CLAUDE_BIN), run `claude /login` once, or set "
                "CLAUDE_CODE_OAUTH_TOKEN / ANTHROPIC_API_KEY.\n",
                bin);
        return 1;
    }
    printf("Copy the token it printed and export it:\n"
           "  export CLAUDE_CODE_OAUTH_TOKEN=<token>\n"
           "tny also auto-detects ~/.claude/.credentials.json from `claude /login`.\n");
    return 0;
}

/* Grok: native RFC 8628 device-code sign-in against auth.x.ai
 * (grok_login.c, docs/adr/0021) — no grok CLI needed. */
static int login_grok(tny_ctx *ctx) {
    (void)ctx;
    return tny_grok_login();
}

int cmd_login(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    (void)g;
    bool device = false;
    for (int i = 0; i < argc; i++)
        if (strcmp(argv[i], "--device") == 0 || strcmp(argv[i], "--device-code") == 0)
            device = true;
    const char *pn = tny_provider_name(ctx);
    if (strcmp(pn, "claude") == 0) return login_claude(ctx);
    if (strcmp(pn, "grok") == 0) return login_grok(ctx);
    const char *cursor_key = getenv("CURSOR_API_KEY");
    switch (ctx->backend) {
    case TNY_BK_CURSOR:
        printf(cursor_key && *cursor_key
                   ? "CURSOR_API_KEY is set — the bridge will use it.\n"
                   : "Set CURSOR_API_KEY (user or service-account key) for the SDK bridge.\n");
        return 0;
    case TNY_BK_CODEX:
        /* app-server account/login/start (browser flow, or the device-code
         * flow with --device); falls back to `codex login` on old hosts */
        return tny_codex_login(ctx, device);
    case TNY_BK_ACP:
        printf("ACP agents authenticate themselves; pre-authorize the agent CLI.\n");
        return 0;
    default:
        printf(ctx->api_key ? "Provider key found.\n"
                            : "Set OPENAI_API_KEY (or run tny setup --api-key-env NAME).\n");
        return ctx->api_key ? 0 : 1;
    }
}

int cmd_logout(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    (void)g;
    (void)argc;
    (void)argv;
    const char *pn = tny_provider_name(ctx);
    if (strcmp(pn, "claude") == 0) {
        printf("Unset CLAUDE_CODE_OAUTH_TOKEN / ANTHROPIC_API_KEY, or remove "
               "~/.claude/.credentials.json (`claude /logout`). tny stores no "
               "secrets itself.\n");
        return 0;
    }
    if (strcmp(pn, "grok") == 0) return tny_grok_logout();
    if (ctx->backend == TNY_BK_CODEX) {
        buf_t cmd;
        buf_init(&cmd);
        buf_appendf(&cmd, "%s logout", ctx->codex_bin);
        int rc = system(cmd.data);
        buf_free(&cmd);
        return rc == 0 ? 0 : 1;
    }
    printf("tny stores no provider secrets; unset the environment variable to log out.\n");
    return 0;
}
