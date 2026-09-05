/* Speech capability/permission contract; network/playback live in fixtures. */
#include "greatest.h"
#include "core/speech.h"
#include "core/intercept.h"
#include "core/tools.h"
#include "util/audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static tny_ctx ctx;
static char root[256];
static const char *const vars[] = {"HOME", "CODEX_HOME", "CHATGPT_ACCESS_TOKEN",
                                   "CHATGPT_ACCOUNT_ID", "PATH"};
static char *saved[5];

static bool schema_speak(tools_env *env) {
    char *schema = tools_schema_json(env);
    bool found = schema && strstr(schema, "\"name\":\"speak\"");
    free(schema);
    return found;
}

TEST speech_requires_subscription_not_chat_provider(void) {
    char err[256];
    ASSERT(!tny_speech_available(&ctx, NULL, false, err, sizeof err));
    ASSERT(strstr(err, "login"));
    ctx.chatgpt_token = "fixture-token";
    ASSERT(!tny_speech_available(&ctx, NULL, false, err, sizeof err));
    ctx.chatgpt_account_id = "fixture-account";
    ASSERT(tny_speech_available(&ctx, "codex", false, err, sizeof err));
    ASSERT_STR_EQ("", err);
    ASSERT(!tny_speech_available(&ctx, "other", false, err, sizeof err));
    ctx.chatgpt_token = NULL;
    ctx.chatgpt_account_id = NULL;
    PASS();
}

TEST speech_schema_and_dispatch_are_gated(void) {
    tools_env env = {.ctx = &ctx};
    env.perm = perm_new(&ctx);
    ASSERT(env.perm);
    ASSERT(!schema_speak(&env));
    tools_call call;
    ASSERT_EQ(-1, tools_call_prepare(&env, "speak", "{\"text\":\"Hello\"}", &call));
    ASSERT(strstr(call.error, "unavailable"));
    tools_call_free(&call);
    ctx.chatgpt_token = "fixture-token";
    ctx.chatgpt_account_id = "fixture-account";
    ASSERT_EQ(audio_player_available(), schema_speak(&env));
    ctx.library_mode = true;
    ASSERT(!schema_speak(&env));
    ASSERT_EQ(-1, tools_call_prepare(&env, "speak", "{\"text\":\"Hello\"}", &call));
    tools_call_free(&call);
    ctx.library_mode = false;
    ctx.tool_profile = TNY_TOOLS_TERMINAL;
    ASSERT(!schema_speak(&env));
    ctx.tool_profile = TNY_TOOLS_ALL;
    if (audio_player_available()) {
        ASSERT_EQ(0, tools_call_prepare(&env, "speak", "{\"text\":\"Hello\"}", &call));
        ASSERT_EQ(PERM_PROMPT, call.verdict);
        tools_call_free(&call);
        ASSERT_EQ(-1, tools_call_prepare(&env, "speak", "{\"text\":3}", &call));
        tools_call_free(&call);
    }
    perm_free(env.perm);
    ctx.chatgpt_token = NULL;
    ctx.chatgpt_account_id = NULL;
    PASS();
}

TEST speech_intercept_keeps_permission_and_payload(void) {
    tools_env env = {.ctx = &ctx};
    env.perm = perm_new(&ctx);
    ASSERT(env.perm);
    ctx.tool_profile = TNY_TOOLS_TERMINAL;
    tools_call call;
    ASSERT_EQ(0, tools_call_prepare(
                     &env, "terminal",
                     "{\"command\":\"printf 'Hello' | tny speak --voice glimmer --json\"}", &call));
    ASSERT(call.intercept);
    ASSERT_EQ(TNY_INTERCEPT_SPEAK, call.intercept->kind);
    ASSERT_STR_EQ("speak", call.permission_tool);
    ASSERT_EQ(PERM_PROMPT, call.verdict);
    ASSERT_STR_EQ("Hello", call.intercept->stdin_data);
    ASSERT_STR_EQ("glimmer", call.intercept->target);
    ASSERT(call.intercept->json);
    tools_call_free(&call);
    tny_intercept *ic = tny_intercept_parse(&env, "tny speak <<'END'\nHello\nEND");
    ASSERT(ic);
    ASSERT_STR_EQ("Hello\n", ic->stdin_data);
    tny_intercept_free(ic);
    ASSERT_EQ(NULL, tny_intercept_parse(&env, "printf 'Hello' | tny speak --output-file x.mp3"));
    ASSERT_EQ(NULL, tny_intercept_parse(&env, "tny speak --check"));
    perm_free(env.perm);
    ctx.tool_profile = TNY_TOOLS_ALL;
    PASS();
}

TEST speech_invalid_input_fails_before_auth_or_network(void) {
    const char *const bad[] = {NULL, "", " \t\n", "\xff"};
    char err[256];
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        tny_speech_request r = {.text = bad[i]};
        ASSERT_EQ(1, tny_speech_run(&ctx, &r, err, sizeof err));
        ASSERT(strstr(err, "UTF-8"));
    }
    char *large = malloc(TNY_SPEECH_TEXT_MAX + 2);
    ASSERT(large);
    memset(large, 'x', TNY_SPEECH_TEXT_MAX + 1);
    large[TNY_SPEECH_TEXT_MAX + 1] = 0;
    tny_speech_request r = {.text = large};
    ASSERT_EQ(1, tny_speech_run(&ctx, &r, err, sizeof err));
    free(large);
    PASS();
}

SUITE(speech_suite) {
    snprintf(root, sizeof root, "/tmp/tny-speech-unit-XXXXXX");
    if (!mkdtemp(root)) abort();
    for (size_t i = 0; i < sizeof vars / sizeof vars[0]; i++) {
        saved[i] = getenv(vars[i]) ? xstrdup(getenv(vars[i])) : NULL;
        unsetenv(vars[i]);
    }
    setenv("HOME", root, 1);
    setenv("CODEX_HOME", root, 1);
    setenv("PATH", root, 1);
    char *player = path_join(root, "ffplay");
    if (!player || file_write_atomic(player, "fixture", 7) != 0 || chmod(player, 0700) != 0)
        abort();
    ctx = (tny_ctx){.cwd = root, .perm_mode = TNY_MODE_ASK, .tool_profile = TNY_TOOLS_ALL};
    RUN_TEST(speech_requires_subscription_not_chat_provider);
    RUN_TEST(speech_schema_and_dispatch_are_gated);
    RUN_TEST(speech_intercept_keeps_permission_and_payload);
    RUN_TEST(speech_invalid_input_fails_before_auth_or_network);
    unlink(player);
    free(player);
    rmdir(root);
    for (size_t i = 0; i < sizeof vars / sizeof vars[0]; i++) {
        if (saved[i]) setenv(vars[i], saved[i], 1);
        else unsetenv(vars[i]);
        free(saved[i]);
    }
}
