/* Runtime membership is captured C context, not a request/session-id hint. */
#include "greatest.h"
#include "core/team_runtime.h"
#include "util/util.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char run_id[] = "0123456789abcdef0123456789abcdef";
static const char token[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

TEST team_runtime_authentication_is_scoped_and_fenced(void) {
    tny_ctx ctx = {0};
    tools_env env = {.ctx = &ctx};
    const char *names[] = {"TNY_NESTED",       "TNY_TEAM_RUN",        "TNY_TEAM_TASK",
                           "TNY_TEAM_ATTEMPT", "TNY_TEAM_CAPABILITY", "TNY_SESSION_ID"};
    char *saved[6] = {0};
    for (size_t i = 0; i < 6; i++) {
        const char *value = getenv(names[i]);
        saved[i] = value ? xstrdup(value) : NULL;
        unsetenv(names[i]);
    }
    uint8_t digest[32];
    ASSERT(sha256((const uint8_t *)token, strlen(token), digest));
    char hash[65];
    for (size_t i = 0; i < 32; i++) snprintf(hash + i * 2, 3, "%02x", digest[i]);
    buf_t json = {0};
    buf_appendf(&json,
                "{\"dag\":true,\"id\":\"%s\",\"attempt\":1,"
                "\"parent_session_id\":\"0123456789abcdef\",\"items\":[{\"attempt\":1,"
                "\"mailbox_capability_sha256\":\"%s\"}]}",
                run_id, hash);
    yyjson_doc *doc = jparse(json.data, json.len);
    ASSERT(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_EQ(-2, tny_team_record_authority(&env, root, false));
    setenv("TNY_SESSION_ID", "0123456789abcdef", 1);
    ASSERT_EQ(-2, tny_team_record_authority(&env, root, false));
    ASSERT_EQ(-1, tny_team_record_authority(&env, root, true));
    setenv("TNY_NESTED", "1", 1);
    ASSERT_EQ(-2, tny_team_record_authority(&env, root, true));
    env.session_id = "0123456789abcdef";
    ASSERT_EQ(-1, tny_team_record_authority(&env, root, false));
    env.session_id = "fedcba9876543210";
    setenv("TNY_TEAM_RUN", run_id, 1);
    setenv("TNY_TEAM_TASK", "0", 1);
    setenv("TNY_TEAM_ATTEMPT", "1", 1);
    setenv("TNY_TEAM_CAPABILITY", token, 1);
    ASSERT_EQ(0, tny_team_record_authority(&env, root, false));
    ASSERT(tny_team_capability_matches(root, 0, 1, token));
    ASSERT_FALSE(tny_team_capability_matches(root, 1, 1, token));
    ASSERT_FALSE(tny_team_capability_matches(root, 0, 2, token));
    setenv("TNY_TEAM_ATTEMPT", "2", 1);
    ASSERT_EQ(-2, tny_team_record_authority(&env, root, false));
    setenv("TNY_TEAM_ATTEMPT", "1", 1);
    setenv("TNY_TEAM_RUN", "ffffffffffffffffffffffffffffffff", 1);
    ASSERT_EQ(-2, tny_team_record_authority(&env, root, false));
    setenv("TNY_TEAM_RUN", run_id, 1);
    setenv("TNY_TEAM_CAPABILITY",
           "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 1);
    ASSERT_EQ(-2, tny_team_record_authority(&env, root, false));
    yyjson_doc_free(doc);
    buf_free(&json);
    for (size_t i = 0; i < 6; i++) {
        if (saved[i]) setenv(names[i], saved[i], 1);
        else unsetenv(names[i]);
        secure_free(saved[i]);
    }
    PASS();
}

TEST team_mailbox_rejects_ambiguous_request_identity(void) {
    static const char *const bad[] = {
        "{\"action\":\"inbox\",\"run\":\"0123456789abcdef0123456789abcdef\\u0000other\"}",
        "{\"action\":\"inbox\",\"action\":\"ack\",\"run\":\"0123456789abcdef0123456789abcdef\"}",
        "{\"action\":\"inbox\",\"run\":\"0123456789abcdef0123456789abcdef\",\"sender\":0}",
        "{\"action\":\"send\",\"run\":\"0123456789abcdef0123456789abcdef\",\"to\":0,\"id\":\"m\","
        "\"text\":\"x\\u0000y\"}",
        "{\"action\":\"send\",\"run\":\"0123456789abcdef0123456789abcdef\",\"to\":-2,\"id\":\"m\","
        "\"text\":\"x\"}",
        "{\"action\":\"retire\",\"run\":\"0123456789abcdef0123456789abcdef\",\"to\":0,\"before_"
        "attempt\":0}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        yyjson_doc *doc = jparse(bad[i], strlen(bad[i]));
        ASSERT(doc);
        ASSERT_EQ(NULL, tny_team_mailbox_detail(yyjson_doc_get_root(doc)));
        yyjson_doc_free(doc);
    }
    const char *good = "{\"action\":\"send\",\"run\":\"0123456789abcdef0123456789abcdef\","
                       "\"to\":0,\"id\":\"m\",\"text\":\"private-content\"}";
    yyjson_doc *doc = jparse(good, strlen(good));
    char *detail = tny_team_mailbox_detail(yyjson_doc_get_root(doc));
    ASSERT(detail && strstr(detail, "payload_sha256="));
    ASSERT_EQ(NULL, strstr(detail, "private-content"));
    free(detail);
    yyjson_doc_free(doc);
    PASS();
}

TEST team_owned_background_is_refused_without_a_task_record(void) {
    char root[] = "/tmp/tny-owned-background-XXXXXX";
    ASSERT(mkdtemp(root));
    char *state = path_join(root, "state");
    ASSERT(state);
    tny_ctx ctx = {.cwd = root, .tny_dir = state, .perm_mode = TNY_MODE_YOLO};
    tools_env env = {.ctx = &ctx};
    const char *prior = getenv("TNY_JOB_PARENT_PID");
    char *saved = prior ? xstrdup(prior) : NULL;
    setenv("TNY_JOB_PARENT_PID", "1", 1);
    const char *request = "{\"command\":\"true\",\"background\":true}";
    yyjson_doc *doc = jparse(request, strlen(request));
    ASSERT(doc);
    for (int profile = TNY_TOOLS_ALL; profile <= TNY_TOOLS_TERMINAL; profile++) {
        ctx.tool_profile = (tny_tool_profile)profile;
        bool handled = false;
        char *result = tool_shell_execute(&env, "terminal", yyjson_doc_get_root(doc), &handled);
        ASSERT(handled && result);
        ASSERT(str_starts(result, "error:"));
        ASSERT(strstr(result, "owned job"));
        ASSERT_EQ(NULL, strstr(result, "exit: 0"));
        ASSERT_FALSE(dir_exists(state));
        free(result);
    }
    if (saved) setenv("TNY_JOB_PARENT_PID", saved, 1);
    else unsetenv("TNY_JOB_PARENT_PID");
    secure_free(saved);
    yyjson_doc_free(doc);
    free(state);
    ASSERT_EQ(0, rmdir(root));
    PASS();
}

SUITE(team_runtime_suite) {
    RUN_TEST(team_owned_background_is_refused_without_a_task_record);
    RUN_TEST(team_runtime_authentication_is_scoped_and_fenced);
    RUN_TEST(team_mailbox_rejects_ambiguous_request_identity);
}
