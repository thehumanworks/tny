/* Runtime membership is captured C context, not a request/session-id hint. */
#include "greatest.h"
#include "core/team_runtime.h"
#include "core/swarm.h"
#include "util/util.h"
#include "util/jobs_host.h"
#include <errno.h>
#include <fcntl.h>
#if defined(__linux__)
#include <sys/inotify.h>
#endif
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

TEST swarm_count_and_failed_resume_are_bounded(void) {
    const char *invalid[] = {"", "0", "17", "-1", "+1", "1x", "1 2", "999999999999999999999"};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++)
        ASSERT_EQ(0, tny_swarm_count(invalid[i]));
    ASSERT_EQ(1, tny_swarm_count("1"));
    ASSERT_EQ(16, tny_swarm_count("16"));
    tny_ctx ctx = {.swarm_cap = 2, .task_explicit = true, .tny_dir = "/nonexistent-tny-swarm-test"};
    tny_session_state session = {.ctx = &ctx, .id = "0123456789abcdef"};
    session.doc = yyjson_mut_doc_new(jallocator());
    ASSERT(session.doc);
    yyjson_mut_val *root = yyjson_mut_obj(session.doc);
    yyjson_mut_doc_set_root(session.doc, root);
    ASSERT(yyjson_mut_obj_add_int(session.doc, root, "turns", 1));
    ASSERT(yyjson_mut_obj_add_int(session.doc, root, "swarm_cap", 0));
    char err[256];
    ASSERT_EQ(-1, session_task_reconcile(&session, err, sizeof err));
    ASSERT(strstr(err, "session has no saved task"));
    ASSERT_EQ(2, ctx.swarm_cap);
    yyjson_mut_doc_free(session.doc);
    PASS();
}

TEST collective_mailbox_schema_and_permission_identity(void) {
    tools_env env = {0};
    char *schema = tools_schema_json(&env);
    ASSERT(schema);
    yyjson_doc *doc = jparse(schema, strlen(schema));
    ASSERT(doc);
    size_t i, n;
    yyjson_val *item, *parameters = NULL;
    yyjson_val *message_parameters = NULL;
    yyjson_arr_foreach(yyjson_doc_get_root(doc), i, n, item) {
        yyjson_val *function = jget(item, "function");
        const char *name = jget_str(function, "name");
        if (name && !strcmp(name, "team_mailbox")) parameters = jget(function, "parameters");
        if (name && !strcmp(name, "swarm_message"))
            message_parameters = jget(function, "parameters");
    }
    ASSERT(parameters);
    ASSERT(message_parameters);
    yyjson_val *timeout = jget(jget(parameters, "properties"), "timeout_ms");
    ASSERT_STR_EQ("integer", jget_str(timeout, "type"));
    ASSERT_EQ(0, jget_int(timeout, "minimum", -1));
    ASSERT_EQ(30000, jget_int(timeout, "maximum", -1));
    ASSERT_FALSE(jget_bool(message_parameters, "additionalProperties", true));
    yyjson_val *message_properties = jget(message_parameters, "properties");
    ASSERT_EQ(256, jget_int(jget(message_properties, "topic"), "maxLength", -1));
    ASSERT_EQ(16384, jget_int(jget(message_properties, "text"), "maxLength", -1));
    ASSERT_EQ(7, (int)yyjson_arr_size(jget(jget(message_properties, "kind"), "enum")));
    yyjson_doc_free(doc);
    free(schema);
    const char *invalid[] = {
        "{\"action\":\"wait\",\"run\":\"0123456789abcdef0123456789abcdef\"}",
        "{\"action\":\"wait\",\"run\":\"0123456789abcdef0123456789abcdef\",\"timeout_ms\":-1}",
        "{\"action\":\"wait\",\"run\":\"0123456789abcdef0123456789abcdef\",\"timeout_ms\":30001}",
        ("{\"action\":\"publish\",\"run\":\"0123456789abcdef0123456789abcdef\",\"id\":\"p\","
         "\"text\":\"x\",\"to\":0}")};
    for (size_t k = 0; k < sizeof invalid / sizeof *invalid; k++) {
        doc = jparse(invalid[k], strlen(invalid[k]));
        ASSERT(doc);
        ASSERT_EQ(NULL, tny_team_mailbox_detail(yyjson_doc_get_root(doc)));
        yyjson_doc_free(doc);
    }
    PASS();
}

TEST swarm_message_ids_and_context_are_attempt_scoped(void) {
    const char *payload =
        "{\"version\":1,\"kind\":\"finding\",\"topic\":\"utf8-\xE2\x9C\x93\",\"body\":\"same\"}";
    char first[65], retry[65], next_attempt[65], other_recipient[65];
    ASSERT(tny_swarm_message_id(run_id, 0, 1, 1, -1, 0, payload, strlen(payload), first));
    ASSERT(tny_swarm_message_id(run_id, 0, 1, 1, -1, 0, payload, strlen(payload), retry));
    ASSERT(tny_swarm_message_id(run_id, 0, 2, 2, -1, 0, payload, strlen(payload), next_attempt));
    ASSERT(tny_swarm_message_id(run_id, 0, 1, 1, 1, 1, payload, strlen(payload), other_recipient));
    ASSERT_STR_EQ(first, retry);
    ASSERT(strcmp(first, next_attempt) != 0);
    ASSERT(strcmp(first, other_recipient) != 0);
    ASSERT_EQ(64, (int)strlen(first));
    ASSERT(str_starts(first, "sm1-"));

    const char *request = "{\"to\":\"root\",\"kind\":\"finding\",\"topic\":\"t\",\"text\":\"x\"}";
    yyjson_doc *args = jparse(request, strlen(request));
    ASSERT(args);
    tny_ctx ctx = {0};
    tny_session_state session = {.ctx = &ctx, .id = "0123456789abcdef"};
    session.doc = yyjson_mut_doc_new(jallocator());
    ASSERT(session.doc);
    yyjson_mut_val *root = yyjson_mut_obj(session.doc);
    yyjson_mut_doc_set_root(session.doc, root);
    tools_env env = {.ctx = &ctx, .session = &session};
    char err[256] = "";
    ASSERT_EQ(NULL, tny_swarm_message_detail(&env, yyjson_doc_get_root(args), err, sizeof err));
    ASSERT(strstr(err, "current purposeful activation"));

    yyjson_mut_val *meta = yyjson_mut_obj(session.doc);
    ASSERT(yyjson_mut_obj_add_strcpy(session.doc, meta, "activation", "active"));
    ASSERT(
        yyjson_mut_obj_add_strcpy(session.doc, meta, "run_id", "ffffffffffffffffffffffffffffffff"));
    ASSERT(yyjson_mut_obj_add_val(session.doc, root, "swarm_definition", meta));
    const char *prior = getenv("TNY_TEAM_RUN");
    char *saved = prior ? xstrdup(prior) : NULL;
    setenv("TNY_TEAM_RUN", run_id, 1);
    err[0] = 0;
    ASSERT_EQ(NULL, tny_swarm_message_detail(&env, yyjson_doc_get_root(args), err, sizeof err));
    ASSERT(strstr(err, "ambiguous"));
    if (saved) setenv("TNY_TEAM_RUN", saved, 1);
    else unsetenv("TNY_TEAM_RUN");
    secure_free(saved);
    yyjson_mut_doc_free(session.doc);
    yyjson_doc_free(args);
    PASS();
}

static bool schema_has(const char *schema, const char *name) {
    yyjson_doc *doc = jparse(schema, strlen(schema));
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    bool found = false;
    size_t i, n;
    yyjson_val *item;
    yyjson_arr_foreach(root, i, n, item) {
        const char *candidate = jget_str(jget(item, "function"), "name");
        if (candidate && strcmp(candidate, name) == 0) found = true;
    }
    yyjson_doc_free(doc);
    return found;
}

TEST swarm_message_profile_and_unsupported_boundaries_are_explicit(void) {
    char root[] = "/tmp/tny-swarm-message-boundary-XXXXXX";
    ASSERT(mkdtemp(root));
    char *state = path_join(root, "state");
    ASSERT(state);
    tny_ctx ctx = {.tool_profile = TNY_TOOLS_ALL, .workspace_read_only = true, .tny_dir = state};
    tools_env env = {.ctx = &ctx};
    const char *prior = getenv("TNY_SWARM_NAME");
    char *saved = prior ? xstrdup(prior) : NULL;
    setenv("TNY_SWARM_NAME", "reviewer", 1);
    char *schema = tools_schema_json(&env);
    ASSERT(schema && schema_has(schema, "swarm_message"));
    free(schema);

    ctx.tool_profile = TNY_TOOLS_TERMINAL;
    schema = tools_schema_json(&env);
    ASSERT(schema && !schema_has(schema, "swarm_message"));
    free(schema);
    ctx.tool_profile = TNY_TOOLS_ALL;
    ctx.ssh_host = "fixture.invalid";
    schema = tools_schema_json(&env);
    ASSERT(schema && !schema_has(schema, "swarm_message"));
    free(schema);
    const char *request = "{\"to\":\"root\",\"kind\":\"finding\",\"topic\":\"t\",\"text\":\"x\"}";
    yyjson_doc *args = jparse(request, strlen(request));
    ASSERT(args);
    buf_t output = {0};
    char err[256] = "";
    ASSERT_EQ(1, tny_swarm_message_run(&env, yyjson_doc_get_root(args), &output, err, sizeof err));
    ASSERT(strstr(err, "native local CLI runner"));
    ASSERT_FALSE(dir_exists(state));
    buf_free(&output);
    ctx.ssh_host = NULL;
    ctx.library_mode = true;
    schema = tools_schema_json(&env);
    ASSERT(schema && !schema_has(schema, "swarm_message"));
    free(schema);
    err[0] = 0;
    ASSERT_EQ(1, tny_swarm_message_run(&env, yyjson_doc_get_root(args), &output, err, sizeof err));
    ASSERT(strstr(err, "native local CLI runner"));
    ASSERT_FALSE(dir_exists(state));
    buf_free(&output);
    ctx.library_mode = false;

    tools_call call;
    ASSERT_EQ(-1, tools_call_prepare(&env, "swarm_message", request, &call));
    ASSERT(call.error && strstr(call.error, "current purposeful activation"));
    tools_call_free(&call);
    if (saved) setenv("TNY_SWARM_NAME", saved, 1);
    else unsetenv("TNY_SWARM_NAME");
    secure_free(saved);
    yyjson_doc_free(args);
    free(state);
    ASSERT_EQ(0, rmdir(root));
    PASS();
}

TEST linux_team_watch_rejects_lost_or_malformed_events(void) {
#if defined(__linux__)
    /* Inject kernel-shaped records through a nonblocking pipe, including lost
     * notifications and truncation; no live inotify limit needs changing. */
    for (int mode = 0; mode < 8; mode++) {
        int descriptors[2];
        ASSERT_EQ(0, pipe(descriptors));
        ASSERT_EQ(0, fcntl(descriptors[0], F_SETFL, O_NONBLOCK));
        struct inotify_event event = {.wd = 1, .mask = IN_CREATE};
        char bytes[2 * sizeof event + 4] = {0};
        size_t count = sizeof event;
        if (mode == 1) {
            event.len = 4;
            struct inotify_event second = {.wd = 1, .mask = IN_MOVED_TO};
            memcpy(bytes + sizeof event + 4, &second, sizeof second);
            count = sizeof bytes;
        } else if (mode == 2) count = sizeof event - 1;
        else if (mode == 3) event.len = 100;
        else if (mode == 4) event.mask = IN_Q_OVERFLOW;
        else if (mode == 5) event.mask = IN_IGNORED;
        else if (mode == 6) event.mask = IN_DELETE_SELF;
        else if (mode == 7) event.mask = IN_MOVE_SELF;
        memcpy(bytes, &event, sizeof event);
        ASSERT_EQ((ssize_t)count, write(descriptors[1], bytes, count));
        tny_jobs_watch watch = {.fd = descriptors[0], .directory_fd = -1};
        int rc = tny_jobs_host_watch_drain(&watch);
        int saved_errno = errno;
        tny_jobs_host_watch_close(&watch);
        close(descriptors[1]);
        ASSERT_EQ(mode < 2 ? 0 : -1, rc);
        if (mode >= 2) ASSERT_EQ(EIO, saved_errno);
    }
    PASS();
#else
    SKIP(); /* The Linux event decoder is exercised in native Linux CI. */
#endif
}

SUITE(team_runtime_suite) {
    RUN_TEST(linux_team_watch_rejects_lost_or_malformed_events);
    RUN_TEST(collective_mailbox_schema_and_permission_identity);
    RUN_TEST(swarm_message_ids_and_context_are_attempt_scoped);
    RUN_TEST(swarm_message_profile_and_unsupported_boundaries_are_explicit);
    RUN_TEST(swarm_count_and_failed_resume_are_bounded);
    RUN_TEST(team_owned_background_is_refused_without_a_task_record);
    RUN_TEST(team_runtime_authentication_is_scoped_and_fenced);
    RUN_TEST(team_mailbox_rejects_ambiguous_request_identity);
}
