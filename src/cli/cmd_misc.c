/* cmd_misc.c — status, permissions, models, workspace, backends, usage,
 * login/logout/setup. All support --json where docs/cli.md requires it. */
#include "cli/cli.h"
#include "core/backend.h"
#include "core/session.h"
#include "net/net.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>

static bool wants_json(const cli_globals *g, int argc, char **argv) {
    if (g->json) return true;
    for (int i = 0; i < argc; i++)
        if (strcmp(argv[i], "--json") == 0) return true;
    return false;
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
        buf_appendf(&b, ",\"auth\":\"%s\",\"permission_mode\":\"%s\",\"sandbox\":\"%s\","
                        "\"workspace\":",
                    auth ? "ok" : "missing", tny_perm_mode_name(ctx->perm_mode),
                    strcmp(ctx->sandbox_mode, "os") == 0 ? "os" : "none");
        jescape(&b, ctx->cwd);
        buf_appendf(&b, ",\"sessions\":%d,\"agent_step_limit\":%d}\n", n, ctx->max_steps);
        fwrite(b.data, 1, b.len, stdout);
        buf_free(&b);
    } else {
        printf("tny v%s\n", TNY_VERSION);
        printf("provider:   %s\n", bk);
        printf("model:      %s\n", model);
        if (ctx->reasoning_effort) printf("effort:     %s\n", ctx->reasoning_effort);
        printf("auth:       %s\n", auth ? "ok" : "missing (set OPENAI_API_KEY or run tny setup)");
        printf("permission: %s\n", tny_perm_mode_name(ctx->perm_mode));
        printf("sandbox:    %s\n", strcmp(ctx->sandbox_mode, "os") == 0 ? "os (unsupported: effective none)" : "none");
        printf("workspace:  %s\n", ctx->cwd);
        for (int i = 0; i < ctx->n_extra_dirs; i++)
            printf("extra dir:  %s\n", ctx->extra_dirs[i]);
        printf("sessions:   %d\n", n);
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
        printf("{\"kind\":\"models\",\"provider\":\"%s\",\"models\":%s}\n", name,
               json_arr);
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
            printf("%s%s%s%s", id, nm ? "  —  " : "", nm ? nm : "",
                   active ? "  (active)" : "");
            yyjson_val *ef = jget(m, "efforts");
            if (ef && yyjson_is_arr(ef) && yyjson_arr_size(ef)) {
                size_t ei, emax;
                yyjson_val *e;
                printf("  [effort:");
                yyjson_arr_foreach(ef, ei, emax, e)
                    if (yyjson_is_str(e)) printf(" %s", yyjson_get_str(e));
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
    const char *hdrs[] = {ctx->api_key ? auth.data : NULL, NULL};
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
                poll(&pf, 1, 500);
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
            fprintf(stderr, "tny: /models refused the credentials (HTTP %d): %s; "
                    "showing configured\n", status,
                    ctx->api_key ? "check the API key"
                                 : "no API key in this environment");
        else
            fprintf(stderr, "tny: provider has no /models (HTTP %d); "
                    "showing configured\n", status);
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
            for (int i = 0; i < ctx->n_extra_dirs; i++)
                printf("extra:   %s\n", ctx->extra_dirs[i]);
            if (!ctx->n_extra_dirs) printf("(no additional directories)\n");
        }
        return 0;
    }
    if (strcmp(sub, "add") == 0 && argc > 1)
        return tny_workspace_add(ctx, argv[1]) == 0 ? 0 : 1;
    if (strcmp(sub, "remove") == 0 && argc > 1)
        return tny_workspace_remove(ctx, argv[1]) == 0 ? 0 : 1;
    if (strcmp(sub, "clear") == 0)
        return tny_workspace_clear(ctx) == 0 ? 0 : 1;
    fprintf(stderr, "tny: workspace list|add DIR|remove DIR|clear\n");
    return 1;
}

/* One `tny providers` row for a user-named OpenAI-compatible provider.
 * profile_bu is the settings base_url or NULL; NAME_BASE_URL beats it,
 * mirroring resolution. */
static void custom_provider_row(tny_ctx *ctx, buf_t *b, bool json,
                                const char *name, const char *profile_bu,
                                const char *origin) {
    char *buvar = tny_provider_env_var(name, "_BASE_URL");
    const char *envbu = buvar ? getenv(buvar) : NULL;
    free(buvar);
    const char *bu = envbu && *envbu ? envbu : profile_bu;
    char *keyvar = tny_custom_provider_key_env(ctx, name);
    const char *key = keyvar ? getenv(keyvar) : NULL;
    bool healthy = (key && *key) || (bu && str_starts(bu, "http://"));
    char line[256];
    snprintf(line, sizeof line, "openai-compatible (%s): base_url %s (key env %s%s)",
             origin, bu ? bu : "?", keyvar ? keyvar : "?",
             key && *key ? "" : " unset");
    free(keyvar);
    bool active = ctx->provider_name && strcmp(ctx->provider_name, name) == 0;
    if (json) {
        buf_appends(b, ",{\"name\":");
        jescape(b, name);
        buf_appendf(b, ",\"active\":%s,\"healthy\":%s,\"hint\":",
                    active ? "true" : "false", healthy ? "true" : "false");
        jescape(b, line);
        buf_appends(b, "}");
    } else {
        buf_appendf(b, "%s %s — %s\n", active ? "*" : " ", name, line);
    }
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
                        tny_backend_name((tny_backend_id)i),
                        active ? "true" : "false",
                        healthy == 0 ? "true" : "false");
            jescape(&b, line);
            buf_appends(&b, "}");
        } else {
            buf_appendf(&b, "%s %s — %s\n", active ? "*" : " ",
                        tny_backend_name((tny_backend_id)i), line);
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
    }
    int n_env = 0;
    char **envnames = tny_env_provider_names(&n_env);
    for (int i = 0; i < n_env; i++) {
        const char *name = envnames[i];
        yyjson_val *root = ctx->settings ? yyjson_doc_get_root(ctx->settings) : NULL;
        const char *in_settings = jget_str(jget(root, name), "base_url");
        if (in_settings && *in_settings) { free(envnames[i]); continue; /* listed above */ }
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
        tny_session *s = session_open(ctx, m[i].id);
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
               "\"output_tokens\":%lld}\n", n, (long long)tin, (long long)tout);
    else
        printf("workspace sessions: %d\ninput tokens:  %lld\noutput tokens: %lld\n",
               n, (long long)tin, (long long)tout);
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
        fprintf(stderr,
                "tny: setup needs flags (no interactive prompts)\n"
                "Example:\n  tny setup --base-url https://openrouter.ai/api/v1 "
                "--api-key-env OPENROUTER_API_KEY --model anthropic/claude-sonnet-4.6\n");
        return 1;
    }
    /* merge into settings "openai" object */
    yyjson_mut_doc *doc = ctx->settings ? yyjson_doc_mut_copy(ctx->settings, NULL)
                                        : yyjson_mut_doc_new(NULL);
    if (!yyjson_mut_doc_get_root(doc))
        yyjson_mut_doc_set_root(doc, yyjson_mut_obj(doc));
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    yyjson_mut_val *oa = yyjson_mut_obj_get(root, "openai");
    if (!oa) {
        oa = yyjson_mut_obj(doc);
        yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc, "openai"), oa);
    }
    if (base_url) yyjson_mut_obj_put(oa, yyjson_mut_strcpy(doc, "base_url"),
                                     yyjson_mut_strcpy(doc, base_url));
    if (key_env) yyjson_mut_obj_put(oa, yyjson_mut_strcpy(doc, "api_key_env"),
                                    yyjson_mut_strcpy(doc, key_env));
    if (model) yyjson_mut_obj_put(oa, yyjson_mut_strcpy(doc, "model"),
                                  yyjson_mut_strcpy(doc, model));
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
    if (key_env && !getenv(key_env))
        printf("note: $%s is not set in this shell\n", key_env);
    return 0;
}

int cmd_login(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    (void)g; (void)argc; (void)argv;
    switch (ctx->backend) {
    case TNY_BK_CURSOR:
        printf(getenv("CURSOR_API_KEY") && *getenv("CURSOR_API_KEY")
                   ? "CURSOR_API_KEY is set — the bridge will use it.\n"
                   : "Set CURSOR_API_KEY (user or service-account key) for the SDK bridge.\n");
        return 0;
    case TNY_BK_CODEX: {
        buf_t cmd;
        buf_init(&cmd);
        buf_appendf(&cmd, "%s login", ctx->codex_bin);
        int rc = system(cmd.data);
        buf_free(&cmd);
        return rc == 0 ? 0 : 1;
    }
    case TNY_BK_ACP:
        printf("ACP agents authenticate themselves; pre-authorize the agent CLI.\n");
        return 0;
    default:
        printf(ctx->api_key
                   ? "Provider key found.\n"
                   : "Set OPENAI_API_KEY (or run tny setup --api-key-env NAME).\n");
        return ctx->api_key ? 0 : 1;
    }
}

int cmd_logout(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    (void)g; (void)argc; (void)argv;
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
