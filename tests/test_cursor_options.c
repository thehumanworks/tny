/* Focused tests for Cursor sdk.v1 configuration and protojson composition. */
#include "backends/cursor/options.h"
#include "core/config.h"
#include "core/cursor_config.h"
#include "greatest.h"
#include "json/json.h"

#include <stdlib.h>
#include <string.h>

static tny_cursor_config *load_cfg(const char *settings, const char *repo, char *err,
                                   size_t errlen) {
    yyjson_doc *sd = settings ? jparse(settings, strlen(settings)) : NULL;
    yyjson_doc *rd = repo ? jparse(repo, strlen(repo)) : NULL;
    tny_cursor_config *cfg = tny_cursor_config_load(
        sd ? yyjson_doc_get_root(sd) : NULL, rd ? yyjson_doc_get_root(rd) : NULL, err, errlen);
    yyjson_doc_free(sd);
    yyjson_doc_free(rd);
    return cfg;
}

static tny_ctx test_ctx(tny_cursor_config *cfg) {
    static char *extras[] = {"/work/two", "/work/three"};
    tny_ctx ctx = {0};
    ctx.cwd = "/work/one";
    ctx.extra_dirs = extras;
    ctx.n_extra_dirs = 2;
    ctx.model = "composer-test";
    ctx.cursor_config = cfg;
    return ctx;
}

TEST cursor_defaults_and_trusted_values_are_injected(void) {
    char err[256] = {0};
    tny_cursor_config *cfg =
        load_cfg("{\"runtime\":\"local\",\"agent_options\":{\"apiKey\":\"stored-secret\"}}", NULL,
                 err, sizeof err);
    ASSERT(cfg);
    tny_ctx ctx = test_ctx(cfg);
    char *json = cursor_options_agent_json(
        &ctx, "runtime-secret",
        "{\"weather\":{\"description\":\"Weather\",\"inputSchema\":{\"type\":\"object\"}}}", err,
        sizeof err);
    ASSERTm(err, json);
    ASSERT_EQ(NULL, strstr(json, "stored-secret"));
    yyjson_doc *doc = jparse(json, strlen(json));
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_STR_EQ("runtime-secret", jget_str(root, "apiKey"));
    ASSERT_STR_EQ("composer-test", jget_str(jget(root, "model"), "id"));
    yyjson_val *local = jget(root, "local");
    ASSERT_STR_EQ("/work/one", yyjson_get_str(yyjson_arr_get(jget(local, "cwd"), 0)));
    ASSERT_EQ(2, (int)yyjson_arr_size(jget(local, "dirs")));
    ASSERT(yyjson_is_obj(jget(jget(local, "customTools"), "weather")));
    yyjson_doc_free(doc);
    free(json);
    tny_cursor_config_free(cfg);
    PASS();
}

TEST bridge_spawn_configuration_is_retained(void) {
    char err[256] = {0};
    tny_cursor_config *cfg =
        load_cfg("{\"state_root\":\"/state/cursor\","
                 "\"local_store\":{\"type\":\"jsonl\",\"rootDir\":\"/state/jsonl\"},"
                 "\"callbacks\":{\"custom_tools\":false,\"store\":true}}",
                 NULL, err, sizeof err);
    ASSERTm(err, cfg);
    ASSERT_STR_EQ("/state/cursor", cfg->state_root);
    ASSERT_STR_EQ("{\"type\":\"jsonl\",\"rootDir\":\"/state/jsonl\"}", cfg->local_store_json);
    ASSERT_FALSE(cfg->tool_callbacks);
    ASSERT(cfg->store_callbacks);
    tny_cursor_config_free(cfg);
    PASS();
}

TEST agent_pass_through_preserves_presence_sensitive_tools(void) {
    const char *settings = "{\"runtime\":\"local\",\"agent_options\":{"
                           "\"name\":\"configured\",\"agentId\":\"resume-me\","
                           "\"mode\":\"AGENT_MODE_OPTION_PLAN\",\"tools\":{\"names\":[]},"
                           "\"disallowedTools\":[\"shell\"],\"mcpServers\":{},\"agents\":{},"
                           "\"local\":{\"cwd\":[\"/configured\"],\"dirs\":[],"
                           "\"settingSources\":[\"SETTING_SOURCE_ALL\"],"
                           "\"sandboxOptions\":{\"enabled\":true},\"autoReview\":false,"
                           "\"store\":{\"type\":\"sqlite\"},\"customTools\":{}}}}";
    char err[256] = {0};
    tny_cursor_config *cfg = load_cfg(settings, NULL, err, sizeof err);
    ASSERTm(err, cfg);
    tny_ctx ctx = test_ctx(cfg);
    char *json = cursor_options_agent_json(&ctx, "key", NULL, err, sizeof err);
    ASSERTm(err, json);
    yyjson_doc *doc = jparse(json, strlen(json));
    yyjson_val *root = yyjson_doc_get_root(doc), *local = jget(root, "local");
    ASSERT(yyjson_is_obj(jget(root, "tools")));
    ASSERT_EQ(0, (int)yyjson_arr_size(jget(jget(root, "tools"), "names")));
    ASSERT_STR_EQ("/configured", yyjson_get_str(yyjson_arr_get(jget(local, "cwd"), 0)));
    ASSERT_EQ(0, (int)yyjson_arr_size(jget(local, "dirs")));
    ASSERT_STR_EQ("configured", jget_str(root, "name"));
    yyjson_doc_free(doc);
    free(json);
    tny_cursor_config_free(cfg);

    cfg = load_cfg("{\"agent_options\":{}}", NULL, err, sizeof err);
    ctx = test_ctx(cfg);
    json = cursor_options_agent_json(&ctx, "key", NULL, err, sizeof err);
    doc = jparse(json, strlen(json));
    ASSERT_EQ(NULL, (void *)jget(yyjson_doc_get_root(doc), "tools"));
    yyjson_doc_free(doc);
    free(json);
    tny_cursor_config_free(cfg);
    PASS();
}

TEST cloud_and_send_options_pass_through(void) {
    const char *settings =
        "{\"runtime\":\"cloud\",\"agent_options\":{\"cloud\":{"
        "\"env\":{\"type\":\"CLOUD_ENVIRONMENT_TYPE_POOL\",\"name\":\"pool-a\"},"
        "\"repos\":[{\"url\":\"https://example/repo\",\"startingRef\":\"main\"}],"
        "\"envVars\":{\"A\":\"B\"},\"metadata\":{\"team\":\"sdk\"}}},"
        "\"send_options\":{\"enableDeltas\":false,\"enableSteps\":true,"
        "\"mode\":\"AGENT_MODE_OPTION_PLAN\",\"cloud\":{\"envVars\":{\"RUN\":\"1\"}}}}";
    char err[256] = {0};
    tny_cursor_config *cfg = load_cfg(settings, NULL, err, sizeof err);
    ASSERTm(err, cfg);
    tny_ctx ctx = test_ctx(cfg);
    char *agent = cursor_options_agent_json(&ctx, "key", NULL, err, sizeof err);
    ASSERTm(err, agent);
    yyjson_doc *adoc = jparse(agent, strlen(agent));
    ASSERT(yyjson_is_obj(jget(yyjson_doc_get_root(adoc), "cloud")));
    ASSERT_EQ(NULL, (void *)jget(yyjson_doc_get_root(adoc), "local"));
    yyjson_doc_free(adoc);
    free(agent);

    char *batch = cursor_options_send_json(&ctx, false, err, sizeof err);
    yyjson_doc *bdoc = jparse(batch, strlen(batch));
    yyjson_val *broot = yyjson_doc_get_root(bdoc);
    ASSERT_FALSE(jget_bool(broot, "enableDeltas", true));
    ASSERT(jget_bool(broot, "enableSteps", false));
    ASSERT_STR_EQ("1", jget_str(jget(jget(broot, "cloud"), "envVars"), "RUN"));
    yyjson_doc_free(bdoc);
    free(batch);

    char *interactive = cursor_options_send_json(&ctx, true, err, sizeof err);
    yyjson_doc *idoc = jparse(interactive, strlen(interactive));
    ASSERT(jget_bool(yyjson_doc_get_root(idoc), "enableDeltas", false));
    ASSERT(jget_bool(yyjson_doc_get_root(idoc), "enableSteps", false));
    yyjson_doc_free(idoc);
    free(interactive);
    tny_cursor_config_free(cfg);
    PASS();
}

TEST invalid_config_is_rejected(void) {
    static const char *bad[] = {
        "{\"runtime\":\"remote\"}",
        "{\"runtime\":\"local\",\"agent_options\":{\"cloud\":{}}}",
        "{\"agent_options\":{\"local\":{},\"cloud\":{}}}",
        "{\"agent_options\":{\"local\":{\"cwd\":[\"a\",\"b\"]}}}",
        "{\"agent_options\":{\"mcpServers\":[]}}",
        "{\"agent_options\":{\"agents\":{\"bad\":1}}}",
        "{\"agent_options\":{\"mode\":\"plan\"}}",
        "{\"send_options\":{\"cloud\":{\"envVars\":[]}}}",
        "{\"local_store\":{\"type\":\"memory\"}}",
        "{\"local_store\":{\"type\":\"jsonl\"}}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char err[256] = {0};
        tny_cursor_config *cfg = load_cfg(bad[i], NULL, err, sizeof err);
        ASSERT_EQ(NULL, (void *)cfg);
        ASSERT(*err);
    }
    PASS();
}

TEST repository_cursor_credentials_are_rejected(void) {
    char err[256] = {0};
    tny_cursor_config *cfg =
        load_cfg(NULL, "{\"agent_options\":{\"apiKey\":\"nope\"}}", err, sizeof err);
    ASSERT_EQ(NULL, (void *)cfg);
    ASSERT(strstr(err, "forbidden"));
    cfg = load_cfg(NULL, "{\"callbacks\":{\"authToken\":\"nope\"}}", err, sizeof err);
    ASSERT_EQ(NULL, (void *)cfg);
    PASS();
}

TEST invalid_registered_custom_tools_are_rejected(void) {
    char err[256] = {0};
    tny_cursor_config *cfg = load_cfg("{\"runtime\":\"local\"}", NULL, err, sizeof err);
    tny_ctx ctx = test_ctx(cfg);
    char *json = cursor_options_agent_json(&ctx, "key", "[]", err, sizeof err);
    ASSERT_EQ(NULL, (void *)json);
    json = cursor_options_agent_json(&ctx, "key", "{\"bad\":1}", err, sizeof err);
    ASSERT_EQ(NULL, (void *)json);
    tny_cursor_config_free(cfg);
    PASS();
}

SUITE(cursor_options_suite) {
    RUN_TEST(cursor_defaults_and_trusted_values_are_injected);
    RUN_TEST(bridge_spawn_configuration_is_retained);
    RUN_TEST(agent_pass_through_preserves_presence_sensitive_tools);
    RUN_TEST(cloud_and_send_options_pass_through);
    RUN_TEST(invalid_config_is_rejected);
    RUN_TEST(repository_cursor_credentials_are_rejected);
    RUN_TEST(invalid_registered_custom_tools_are_rejected);
}

#ifdef CURSOR_OPTIONS_STANDALONE
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(cursor_options_suite);
    GREATEST_MAIN_END();
}
#endif
