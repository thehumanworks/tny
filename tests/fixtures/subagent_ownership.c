/* Launch-plan fault/lifetime oracles. No child or provider is started. */
#include "core/subagent.h"
#include "util/alloc.h"
#include "util/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;
#define REQUIRE(expr)                                                                \
    do {                                                                             \
        if (!(expr)) {                                                               \
            fprintf(stderr, "subagent assertion at line %d: %s\n", __LINE__, #expr); \
            exit(1);                                                                 \
        }                                                                            \
    } while (0)

static size_t expected_wipe, wipes;
/* Only the test's plan object redirects secure_zero here. Observe the full
 * allocation before release; production has no test hook or global callback. */
void tny_subagent_test_secure_zero(void *data, size_t n);
void tny_subagent_test_secure_zero(void *data, size_t n) {
    REQUIRE(expected_wipe && n == expected_wipe);
    secure_zero(data, n);
    for (size_t i = 0; i < n; ++i) REQUIRE(((unsigned char *)data)[i] == 0);
    expected_wipe = 0;
    ++wipes;
}
static size_t snapshot_size(const tny_subagent_plan *p) {
    size_t size = 0;
    for (char **v = p->argv; *v; ++v) size += strlen(*v) + 1;
    for (char **v = p->envp; *v; ++v) size += strlen(*v) + 1;
    return size;
}
static void release(tny_subagent_plan *p) {
    size_t before = wipes;
    expected_wipe = snapshot_size(p);
    tny_subagent_plan_free(p);
    REQUIRE(wipes == before + 1 && !expected_wipe);
    REQUIRE(!p->owner && !p->argv && !p->envp);
    tny_subagent_plan_free(p);
    REQUIRE(wipes == before + 1);
}
static const char *value(char **envp, const char *name) {
    const char *result = NULL;
    size_t n = strlen(name);
    for (char **e = envp; *e; ++e) {
        if (strncmp(*e, name, n) || (*e)[n] != '=') continue;
        REQUIRE(!result); // replacement must remove every stale duplicate
        result = *e + n + 1;
    }
    return result;
}
static void equal(const char *a, const char *b) { REQUIRE(a && b && strcmp(a, b) == 0); }
static const char *option(const tny_subagent_plan *p, const char *name) {
    for (char **a = p->argv; *a; ++a)
        if (strcmp(*a, name) == 0) return a[1];
    return NULL;
}
static char fault[64];
static void fault_at(size_t n) {
    snprintf(fault, sizeof fault, "TNY_TEST_ALLOC_FAIL_AT=%zu", n);
    tny_alloc_scope_begin("subagent-owner");
}
static void selectors(const tny_subagent_plan *p, const tny_ctx *ctx, const char *resume) {
    const char *expected[] = {
        "--cwd",
        ctx->cwd,
        "--provider",
        ctx->provider_name,
        "--api-key-env",
        TNY_SUBAGENT_KEY_ENV,
        "--base-url-env",
        TNY_SUBAGENT_URL_ENV,
        "--wire-api",
        "chat",
        "--model",
        ctx->model,
        "--effort",
        ctx->reasoning_effort,
        "--permission-mode",
        tny_perm_mode_name(ctx->perm_mode),
        "--ephemeral",
        "ask",
        "--json",
        "--stdin",
        "--resume-id",
        resume,
    };
    REQUIRE(p->argv[0] && p->argv[0][0] == '/');
    for (size_t i = 0; i < sizeof expected / sizeof *expected; ++i)
        equal(p->argv[i + 1], expected[i]);
    REQUIRE(!p->argv[1 + sizeof expected / sizeof *expected]);
    for (char **a = p->argv; *a; ++a) REQUIRE(!strstr(*a, "SECRET"));
}
int main(void) {
    char **saved_environment = environ;
    char ambient[] = "AMBIENT=original";
    char *environment[] = {
        ambient,
        "CHATGPT_ACCESS_TOKEN=SECRET-ambient-token",
        "CHATGPT_ACCOUNT_ID=ambient-account",
        TNY_SUBAGENT_KEY_ENV "=SECRET-stale-key",
        TNY_SUBAGENT_KEY_ENV "=SECRET-duplicate",
        TNY_SUBAGENT_URL_ENV "=SECRET-stale-url",
        "TNY_NESTED=0",
        "TNY_NESTED_MODE=yolo",
        "TNY_TOOLS=all",
        "TNY_PERMISSION_MODE=yolo",
        "TNY_TEST_ALLOC_SCOPE=subagent-owner",
        fault,
        NULL,
    };
    environ = environment;
    char model[] = "model-original", cwd[] = "/fixture", provider[] = "fixture-provider";
    char effort[] = "high", resume[] = "0123456789abcdef";
    char key[8193];
    memset(key, 'x', sizeof key - 1);
    memcpy(key, "SECRET", 6);
    key[sizeof key - 1] = 0;
    tny_ctx ctx = {0};
    ctx.cwd = cwd;
    ctx.provider_name = provider;
    ctx.model = model;
    ctx.reasoning_effort = effort;
    ctx.api_key = key;
    ctx.base_url = "https://fixture.invalid/SECRET-url";
    ctx.wire_api = "chat";
    ctx.no_save = true;
    tools_env env = {.ctx = &ctx};
    size_t live = tny_alloc_test_owned_live();

    // Preserve every token/account, permission and tool-profile combination.
    for (int selected = 0; selected < 4; ++selected) {
        ctx.chatgpt_token = selected & 1 ? "SECRET-selected-token" : NULL;
        ctx.chatgpt_account_id = selected & 2 ? "selected-account" : NULL;
        for (int mode = TNY_MODE_ASK; mode <= TNY_MODE_YOLO; ++mode) {
            ctx.perm_mode = (tny_perm_mode)mode;
            for (int profile = TNY_TOOLS_ALL; profile <= TNY_TOOLS_TERMINAL; ++profile) {
                ctx.tool_profile = (tny_tool_profile)profile;
                tny_subagent_plan p = {0};
                fault_at(0);
                REQUIRE(tny_subagent_plan_build(&env, resume, &p) == 0);
                selectors(&p, &ctx, resume);
                equal(value(p.envp, TNY_SUBAGENT_KEY_ENV), key);
                equal(value(p.envp, TNY_SUBAGENT_URL_ENV), ctx.base_url);
                equal(value(p.envp, "CHATGPT_ACCESS_TOKEN"),
                      selected & 1 ? ctx.chatgpt_token : "SECRET-ambient-token");
                if (selected == 1) REQUIRE(!value(p.envp, "CHATGPT_ACCOUNT_ID"));
                else
                    equal(value(p.envp, "CHATGPT_ACCOUNT_ID"),
                          selected & 2 ? ctx.chatgpt_account_id : "ambient-account");
                equal(value(p.envp, "TNY_NESTED"), "1");
                equal(value(p.envp, "TNY_NESTED_MODE"), tny_perm_mode_name(ctx.perm_mode));
                equal(value(p.envp, "TNY_TOOLS"), tny_tool_profile_name(ctx.tool_profile));
                REQUIRE(!value(p.envp, "TNY_PERMISSION_MODE"));
                equal(value(environment, "TNY_NESTED"), "0");
                release(&p);
                REQUIRE(tny_alloc_test_owned_live() == live);
            }
        }
    }

    // Explicit selectors are independent snapshots, with exact same-provider
    // inheritance and a fresh CLI configuration for every different provider.
    const char *selections[] = {
        "{}",
        "{\"provider\":\"fixture-provider\"}",
        "{\"model\":\"child-model\"}",
        "{\"effort\":\"default\"}",
        "{\"provider\":\"fixture-provider\",\"model\":\"child-model\",\"effort\":\"low\"}",
        "{\"provider\":\"other\"}",
        "{\"provider\":\"other\",\"model\":\"child-model\",\"effort\":\"default\"}",
        "{\"provider\":\"acp@fixture\",\"model\":\"child-model\"}",
        "{\"provider\":\"cursor\",\"effort\":\"provider-token\"}",
    };
    for (size_t i = 0; i < sizeof selections / sizeof *selections; ++i) {
        yyjson_doc *doc = jparse(selections[i], strlen(selections[i]));
        REQUIRE(doc);
        yyjson_val *args = yyjson_doc_get_root(doc);
        const char *pick = jget_str(args, "provider");
        bool parent = !pick || strcmp(pick, provider) == 0;
        tny_subagent_plan selected = {0};
        fault_at(0);
        REQUIRE(tny_subagent_plan_build_selected(&env, resume, args, &selected) == 0);
        equal(option(&selected, "--provider"), pick ? pick : provider);
        const char *picked_model = jget_str(args, "model");
        const char *picked_effort = jget_str(args, "effort");
        const char *want_model = picked_model ? picked_model : parent ? model : NULL;
        const char *want_effort = picked_effort ? picked_effort : parent ? effort : NULL;
        if (want_model) equal(option(&selected, "--model"), want_model);
        else REQUIRE(!option(&selected, "--model"));
        if (want_effort) equal(option(&selected, "--effort"), want_effort);
        else REQUIRE(!option(&selected, "--effort"));
        if (parent) {
            equal(option(&selected, "--wire-api"), "chat");
            equal(value(selected.envp, TNY_SUBAGENT_KEY_ENV), key);
            equal(value(selected.envp, TNY_SUBAGENT_URL_ENV), ctx.base_url);
            equal(value(selected.envp, "CHATGPT_ACCESS_TOKEN"), ctx.chatgpt_token);
            equal(value(selected.envp, "CHATGPT_ACCOUNT_ID"), ctx.chatgpt_account_id);
        } else {
            REQUIRE(!option(&selected, "--wire-api"));
            REQUIRE(!option(&selected, "--api-key-env"));
            REQUIRE(!option(&selected, "--base-url-env"));
            REQUIRE(!value(selected.envp, TNY_SUBAGENT_KEY_ENV));
            REQUIRE(!value(selected.envp, TNY_SUBAGENT_URL_ENV));
            equal(value(selected.envp, "CHATGPT_ACCESS_TOKEN"), "SECRET-ambient-token");
            equal(value(selected.envp, "CHATGPT_ACCOUNT_ID"), "ambient-account");
        }
        // Mutating and freeing the tool JSON cannot change the launch snapshot.
        bool model_override = picked_model != NULL, effort_override = picked_effort != NULL;
        if (picked_model) ((char *)picked_model)[0] = 'X';
        if (picked_effort) ((char *)picked_effort)[0] = 'X';
        yyjson_doc_free(doc);
        if (model_override) equal(option(&selected, "--model"), "child-model");
        if (effort_override) REQUIRE(option(&selected, "--effort")[0] != 'X');
        equal(option(&selected, "--resume-id"), resume);
        equal(value(selected.envp, "TNY_NESTED"), "1");
        release(&selected);
        REQUIRE(tny_alloc_test_owned_live() == live);
    }

    tny_subagent_plan p = {0};
    fault_at(0);
    REQUIRE(tny_subagent_plan_build(&env, resume, &p) == 0);
    size_t count = tny_alloc_test_scope_count();
    REQUIRE(count >= 4);
    // None of these source buffers remains borrowed, including environ entries.
    model[0] = 'X';
    cwd[0] = 'X';
    provider[0] = 'X';
    effort[0] = 'X';
    resume[0] = 'f';
    ambient[8] = 'X';
    key[0] = 'X';
    equal(p.argv[2], "/fixture");
    equal(p.argv[4], "fixture-provider");
    equal(p.argv[12], "model-original");
    equal(p.argv[14], "high");
    equal(p.argv[22], "0123456789abcdef");
    equal(value(p.envp, "AMBIENT"), "original");
    REQUIRE(value(p.envp, TNY_SUBAGENT_KEY_ENV)[0] == 'S');
    model[0] = 'm';
    cwd[0] = '/';
    provider[0] = 'f';
    effort[0] = 'h';
    resume[0] = '0';
    ambient[8] = 'o';
    key[0] = 'S';

    // Every measured allocation must fail closed and preserve a live snapshot.
    for (size_t i = 1; i <= count; ++i) {
        struct tny_subagent_plan_owner *old = p.owner;
        char **argv = p.argv, **envp = p.envp;
        size_t retained = tny_alloc_test_owned_live();
        fault_at(i);
        REQUIRE(tny_subagent_plan_build(&env, resume, &p) == -1);
        REQUIRE(tny_alloc_test_scope_injected());
        REQUIRE(p.owner == old && p.argv == argv && p.envp == envp);
        REQUIRE(tny_alloc_test_owned_live() == retained);
        selectors(&p, &ctx, resume);
        // Also cover initial failure, repeated free, and reuse after failure.
        tny_subagent_plan empty = {0};
        fault_at(i);
        REQUIRE(tny_subagent_plan_build(&env, resume, &empty) == -1);
        REQUIRE(tny_alloc_test_scope_injected());
        REQUIRE(!empty.owner && !empty.argv && !empty.envp);
        tny_subagent_plan_free(&empty);
        tny_subagent_plan_free(&empty);
        REQUIRE(tny_alloc_test_owned_live() == retained);
    }
    // Successful replacement wipes exactly the old block, then exposes new data.
    fault_at(0);
    model[0] = 'N';
    expected_wipe = snapshot_size(&p);
    size_t before = wipes;
    REQUIRE(tny_subagent_plan_build(&env, resume, &p) == 0);
    REQUIRE(wipes == before + 1 && !expected_wipe);
    equal(p.argv[12], "Nodel-original");
    size_t allocations = tny_alloc_test_scope_count();
    release(&p);
    REQUIRE(tny_alloc_test_scope_count() == allocations); // destruction allocates nothing

    // Empty optionals remove stale carriers and omit selectors, without truncation.
    ctx.api_key = "";
    ctx.base_url = NULL;
    ctx.model = NULL;
    ctx.wire_api = NULL;
    ctx.reasoning_effort = "";
    ctx.no_save = false;
    fault_at(0);
    REQUIRE(tny_subagent_plan_build(&env, NULL, &p) == 0);
    REQUIRE(!value(p.envp, TNY_SUBAGENT_KEY_ENV) && !value(p.envp, TNY_SUBAGENT_URL_ENV));
    const char *short_args[] = {"--cwd",    cwd,       "--provider",        provider,
                                "--effort", "default", "--permission-mode", "yolo",
                                "ask",      "--json",  "--stdin",           NULL};
    for (size_t i = 0; short_args[i]; ++i) equal(p.argv[i + 1], short_args[i]);
    REQUIRE(!p.argv[12]);
    release(&p);

    // Both representations of an empty inherited environment are supported.
    char *empty_environment[] = {NULL};
    for (int empty = 0; empty < 2; ++empty) {
        fault_at(0);
        environ = empty ? empty_environment : NULL;
        ctx.wire_api = "responses";
        REQUIRE(tny_subagent_plan_build(&env, NULL, &p) == 0);
        equal(p.argv[5], "--wire-api");
        equal(p.argv[6], "responses");
        REQUIRE(!value(p.envp, "AMBIENT"));
        equal(value(p.envp, "TNY_NESTED"), "1");
        release(&p);
        environ = environment;
    }
    REQUIRE(tny_alloc_test_owned_live() == live);
    tny_subagent_plan_free(NULL);
    environ = saved_environment;
    printf("subagent ownership: 36 selector/authority cases, %zu allocation failures (empty/live), "
           "snapshot/rebuild/wipe/recovery passed\n",
           count);
    return 0;
}
