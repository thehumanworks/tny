/* cmd_provider.c — `tny provider setup NAME`: write an OpenAI-compatible
 * provider profile to settings.json (docs/adr/0018). Flags carry every
 * field; on a tty, missing fields are prompted for interactively (the key
 * with echo off). Noninteractive-first: piped/CI runs need the flags. */
#include "cli/cli.h"
#include "core/config.h"
#include "net/net.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s + i, n - i + 1);
}

/* Read one line from the tty; echo off for secrets. NULL on EOF. */
static char *prompt_line(const char *label, bool secret) {
    fprintf(stderr, "%s", label);
    fflush(stderr);
    struct termios old, raw;
    bool masked = false;
    /* TCSADRAIN, never TCSAFLUSH: FLUSH discards queued input, eating an
     * answer pasted before the echo-off lands (docs/adr/0018) */
    if (secret && tcgetattr(0, &old) == 0) {
        raw = old;
        raw.c_lflag &= (tcflag_t)~ECHO;
        masked = tcsetattr(0, TCSADRAIN, &raw) == 0;
    }
    char buf[1024];
    char *got = fgets(buf, sizeof buf, stdin);
    if (masked) {
        tcsetattr(0, TCSADRAIN, &old);
        fputs("\n", stderr);
    }
    if (!got) return NULL;
    trim(buf);
    return xstrdup(buf);
}

static bool base_url_ok(const char *url) {
    url_parts u;
    return url_parse(url, &u) == 0 &&
           (strcmp(u.scheme, "http") == 0 || strcmp(u.scheme, "https") == 0);
}

static int provider_setup(tny_ctx *ctx, int argc, char **argv) {
    const char *name = NULL, *base_url = NULL, *api_key = NULL;
    const char *api_key_env = NULL, *model = NULL, *wire_api = NULL;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--base-url") == 0 && i + 1 < argc) base_url = argv[++i];
        else if (strcmp(a, "--api-key") == 0 && i + 1 < argc) api_key = argv[++i];
        else if (strcmp(a, "--api-key-env") == 0 && i + 1 < argc) api_key_env = argv[++i];
        else if (strcmp(a, "--model") == 0 && i + 1 < argc) model = argv[++i];
        else if (strcmp(a, "--wire-api") == 0 && i + 1 < argc) wire_api = argv[++i];
        else if (a[0] == '-') {
            fprintf(stderr,
                    "tny: provider setup: unknown flag %s\n"
                    "Example: tny provider setup openrouter "
                    "--base-url https://openrouter.ai/api/v1 --api-key sk-…\n",
                    a);
            return 1;
        } else if (!name) name = a;
        else {
            fprintf(stderr, "tny: provider setup takes one NAME\n");
            return 1;
        }
    }
    if (api_key && api_key_env) {
        fprintf(stderr, "tny: --api-key and --api-key-env are alternatives; "
                        "pick one\n");
        return 1;
    }
    if (wire_api && strcmp(wire_api, "responses") != 0 && strcmp(wire_api, "chat") != 0) {
        fprintf(stderr, "tny: --wire-api must be responses|chat\n");
        return 1;
    }

    bool tty = isatty(0) && isatty(2);
    char *p_name = NULL, *p_base = NULL, *p_key = NULL, *p_model = NULL;
    if (tty && !name) {
        p_name = prompt_line("provider name (e.g. openrouter): ", false);
        if (!p_name || !*p_name) {
            free(p_name);
            fprintf(stderr, "tny: cancelled\n");
            return 1;
        }
        name = p_name;
    }
    if (!name) {
        fprintf(stderr, "tny: provider setup needs a NAME (and flags when not on a "
                        "terminal)\nExample: tny provider setup openrouter "
                        "--base-url https://openrouter.ai/api/v1 "
                        "--api-key-env OPENROUTER_API_KEY\n");
        return 1;
    }
    bool exists = tny_custom_provider_exists(ctx, name) || strcmp(name, "openai") == 0;
    if (tty && !base_url && !exists) {
        p_base = prompt_line("base url (OpenAI-compatible /v1 endpoint): ", false);
        if (p_base && *p_base) base_url = p_base;
    }
    if (base_url && !base_url_ok(base_url)) {
        fprintf(stderr,
                "tny: base url must be http(s)://host[/prefix] "
                "(got '%s')\n",
                base_url);
        free(p_name);
        free(p_base);
        return 1;
    }
    if (tty && !api_key && !api_key_env) {
        p_key = prompt_line("api key (stored in ~/.tny/settings.json; $ENV_NAME to read an "
                            "env var instead; empty to skip): ",
                            true);
        if (p_key && p_key[0] == '$' && p_key[1]) api_key_env = p_key + 1;
        else if (p_key && *p_key) api_key = p_key;
    }
    if (tty && !model) {
        p_model = prompt_line("default model (empty to skip): ", false);
        if (p_model && *p_model) model = p_model;
    }

    tny_provider_fields f = {base_url, api_key, api_key_env, model, wire_api};
    char err[256];
    int rc = tny_provider_write_profile(ctx, name, &f, err, sizeof err);
    if (rc != 0) {
        fprintf(stderr, "tny: %s\n", err);
    } else {
        tny_settings_set_str(ctx, "last_provider", name);
        printf("provider '%s' written to %s%s\n", name, ctx->settings_path,
               api_key ? " (key stored; file is 0600)" : "");
        if (api_key_env && !getenv(api_key_env))
            printf("note: $%s is not set in this shell\n", api_key_env);
        printf("try: tny --provider %s ask \"hello\"\n", name);
    }
    free(p_name);
    free(p_base);
    free(p_key);
    free(p_model);
    return rc == 0 ? 0 : 1;
}

int cmd_provider(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    if (argc >= 1 && strcmp(argv[0], "setup") == 0) return provider_setup(ctx, argc - 1, argv + 1);
    if (argc == 0 || argv[0][0] == '-') /* bare / `provider --json`: list */
        return cmd_backends(ctx, g, argc, argv);
    if (strcmp(argv[0], "list") == 0) return cmd_backends(ctx, g, argc - 1, argv + 1);
    fprintf(stderr,
            "tny: provider: unknown subcommand '%s'\n"
            "Usage: tny provider [list] | tny provider setup NAME "
            "[--base-url URL] [--api-key KEY | --api-key-env ENV] "
            "[--model M] [--wire-api responses|chat]\n",
            argv[0]);
    return 1;
}
