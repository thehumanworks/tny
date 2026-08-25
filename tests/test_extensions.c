/* test_extensions.c — discovery, host protocol, actions, and fail-open hooks. */
#include "greatest.h"
#include "core/extension_caps.h"
#include "core/extensions.h"
#include "json/json.h"
#include "util/util.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char *root;
    char *tny;
    char *workspace;
    char *host;
} ext_fixture;

static ext_fixture ext_fixture_new(bool entries) {
    char root[] = "/tmp/tny-extensions-test-XXXXXX";
    if (!mkdtemp(root)) abort();
    ext_fixture f = {xstrdup(root), NULL, NULL, NULL};
    f.tny = path_join(f.root, ".tny");
    f.workspace = path_join(f.root, "workspace");
    mkdir_p(f.workspace);
    if (entries) {
        char *extensions = path_join(f.tny, "extensions");
        char *alpha = path_join(extensions, "alpha");
        mkdir_p(alpha);
        char *index = path_join(alpha, "index.py");
        file_write_atomic(index, "# alpha\n", 8);
        char *zeta = path_join(extensions, "zeta.py");
        file_write_atomic(zeta, "# zeta\n", 7);
        char *ignored = path_join(extensions, "ignored.txt");
        file_write_atomic(ignored, "ignored\n", 8);
        free(ignored); free(zeta); free(index); free(alpha); free(extensions);
    }
    f.host = path_abs("tests/fixtures/fake_extension_host.py");
    return f;
}

static void ext_fixture_free(ext_fixture *f) {
    free(f->root); free(f->tny); free(f->workspace); free(f->host);
}

TEST extensions_empty_is_lazy_and_optional(void) {
    ext_fixture f = ext_fixture_new(false);
    ASSERT(f.host);
    setenv("TNY_EXTENSION_HOST", f.host, 1);
    tny_extensions *x = tny_extensions_new(f.tny, f.workspace, 100);
    ASSERT(x);
    ASSERT_EQ(0, tny_extensions_entry_count(x));
    ASSERT_EQ(TNY_EXTENSIONS_EMPTY, tny_extensions_get_state(x));
    tny_extension_result result;
    ASSERT_EQ(0, tny_extensions_invoke(x, "turn_end", "{\"type\":\"turn_end\"}",
                                       &result));
    ASSERT_EQ(0, result.action_count);
    ASSERT_EQ(0, result.failure_count);
    tny_extension_result_free(&result);
    tny_extensions_free(x);
    unsetenv("TNY_EXTENSION_HOST");
    ext_fixture_free(&f);
    PASS();
}

TEST extensions_discover_sorted_and_return_typed_actions(void) {
    ext_fixture f = ext_fixture_new(true);
    ASSERT(f.host);
    setenv("TNY_EXTENSION_HOST", f.host, 1);
    tny_extensions *x = tny_extensions_new(f.tny, f.workspace, 500);
    ASSERT(x);
    ASSERT_EQ(2, tny_extensions_entry_count(x));
    ASSERT_EQ(TNY_EXTENSIONS_DORMANT, tny_extensions_get_state(x));

    tny_extension_result result;
    ASSERT_EQ(0, tny_extensions_invoke(
        x, "tool_end", "{\"type\":\"tool_end\",\"tool\":{\"name\":\"read_file\"}}",
        &result));
    ASSERT_EQ(TNY_EXTENSIONS_READY, tny_extensions_get_state(x));
    ASSERT_EQ(2, result.action_count);
    ASSERT_EQ(0, result.failure_count);
    ASSERT_EQ(TNY_EXTENSION_ACTION_CONTEXT, result.actions[0].kind);
    ASSERT_STR_EQ("alpha", result.actions[0].extension);
    ASSERT_STR_EQ("visible context", result.actions[0].content);
    ASSERT_STR_EQ("fixture.context", result.actions[0].custom_type);
    ASSERT(result.actions[0].display);
    ASSERT_EQ(TNY_EXTENSION_ACTION_CONTINUE, result.actions[1].kind);
    ASSERT_STR_EQ("zeta", result.actions[1].extension);
    ASSERT_STR_EQ("please iterate", result.actions[1].content);
    ASSERT_EQ(TNY_EXTENSION_MESSAGE_CUSTOM, result.actions[1].message_kind);
    ASSERT_FALSE(result.actions[1].display);
    tny_extension_result_free(&result);

    ASSERT_EQ(0, tny_extensions_invoke(x, "usage", "{\"type\":\"usage\"}",
                                       &result));
    ASSERT_EQ(0, result.action_count);
    ASSERT_EQ(0, result.failure_count);
    tny_extension_result_free(&result);
    tny_extensions_free(x);
    unsetenv("TNY_EXTENSION_HOST");
    ext_fixture_free(&f);
    PASS();
}

TEST extensions_fail_open_on_handler_error_timeout_and_restart(void) {
    ext_fixture f = ext_fixture_new(true);
    ASSERT(f.host);
    setenv("TNY_EXTENSION_HOST", f.host, 1);
    tny_extensions *x = tny_extensions_new(f.tny, f.workspace, 60);
    ASSERT(x);
    tny_extension_result result;

    ASSERT_EQ(0, tny_extensions_invoke(x, "status", "{\"type\":\"status\"}",
                                       &result));
    ASSERT_EQ(0, result.action_count);
    ASSERT_EQ(1, result.failure_count);
    ASSERT_STR_EQ("handler_exception", result.failures[0].code);
    ASSERT_STR_EQ("fixture failure", result.failures[0].message);
    tny_extension_result_free(&result);

    int64_t started = monotonic_ms();
    ASSERT_EQ(0, tny_extensions_invoke(x, "thinking", "{\"type\":\"thinking\"}",
                                       &result));
    ASSERT(monotonic_ms() - started < 700);
    ASSERT_EQ(2, result.failure_count);
    ASSERT_STR_EQ("timeout", result.failures[0].code);
    ASSERT_STR_EQ("host_state_reset", result.failures[1].code);
    ASSERT_EQ(TNY_EXTENSIONS_DORMANT, tny_extensions_get_state(x));
    tny_extension_result_free(&result);

    ASSERT_EQ(0, tny_extensions_invoke(x, "turn_end", "{\"type\":\"turn_end\"}",
                                       &result));
    ASSERT_EQ(1, result.action_count);
    ASSERT_EQ(TNY_EXTENSION_ACTION_STOP, result.actions[0].kind);
    ASSERT_STR_EQ("fixture stop", result.actions[0].reason);
    ASSERT_EQ(TNY_EXTENSIONS_READY, tny_extensions_get_state(x));
    tny_extension_result_free(&result);

    tny_extensions_free(x);
    unsetenv("TNY_EXTENSION_HOST");
    ext_fixture_free(&f);
    PASS();
}

TEST extensions_missing_host_is_clean_unavailable(void) {
    ext_fixture f = ext_fixture_new(true);
    setenv("TNY_EXTENSION_HOST", "/definitely/missing/host.py", 1);
    tny_extensions *x = tny_extensions_new(f.tny, f.workspace, 60);
    ASSERT(x);
    tny_extension_result result;
    ASSERT_EQ(0, tny_extensions_invoke(x, "turn_end", "{\"type\":\"turn_end\"}",
                                       &result));
    ASSERT_EQ(TNY_EXTENSIONS_UNAVAILABLE, tny_extensions_get_state(x));
    ASSERT_EQ(0, result.action_count);
    ASSERT_EQ(1, result.failure_count);
    ASSERT_STR_EQ("unavailable", result.failures[0].code);
    tny_extension_result_free(&result);
    tny_extensions_free(x);
    unsetenv("TNY_EXTENSION_HOST");
    ext_fixture_free(&f);
    PASS();
}

TEST extensions_missing_python_reports_once_and_stays_optional(void) {
    ext_fixture f = ext_fixture_new(true);
    ASSERT(f.host);
    setenv("TNY_EXTENSION_HOST", f.host, 1);
    const char *old_path_value = getenv("PATH");
    char *old_path = old_path_value ? xstrdup(old_path_value) : NULL;
    setenv("PATH", "/definitely/no-python-here", 1);
    tny_extensions *x = tny_extensions_new(f.tny, f.workspace, 200);
    ASSERT(x);
    tny_extension_result result;
    ASSERT_EQ(0, tny_extensions_invoke(x, "turn_end", "{\"type\":\"turn_end\"}",
                                       &result));
    ASSERT_EQ(TNY_EXTENSIONS_UNAVAILABLE, tny_extensions_get_state(x));
    ASSERT_EQ(1, result.failure_count);
    ASSERT_STR_EQ("unavailable", result.failures[0].code);
    tny_extension_result_free(&result);
    ASSERT_EQ(0, tny_extensions_invoke(x, "turn_end", "{\"type\":\"turn_end\"}",
                                       &result));
    ASSERT_EQ(0, result.failure_count); /* one optional-unavailable diagnostic */
    tny_extension_result_free(&result);
    tny_extensions_free(x);
    if (old_path) { setenv("PATH", old_path, 1); free(old_path); }
    else unsetenv("PATH");
    unsetenv("TNY_EXTENSION_HOST");
    ext_fixture_free(&f);
    PASS();
}

TEST extensions_duplicate_name_loads_neither_and_reports_collision(void) {
    ext_fixture f = ext_fixture_new(true);
    ASSERT(f.host);
    char *extensions = path_join(f.tny, "extensions");
    char *duplicate = path_join(extensions, "alpha.py");
    file_write_atomic(duplicate, "# duplicate\n", 12);
    free(duplicate); free(extensions);
    setenv("TNY_EXTENSION_HOST", f.host, 1);
    tny_extensions *x = tny_extensions_new(f.tny, f.workspace, 60);
    ASSERT(x);
    ASSERT_EQ(1, tny_extensions_entry_count(x)); /* zeta only */
    tny_extension_result result;
    ASSERT_EQ(0, tny_extensions_invoke(x, "usage", "{\"type\":\"usage\"}",
                                       &result));
    ASSERT_EQ(1, result.failure_count);
    ASSERT_STR_EQ("alpha", result.failures[0].extension);
    ASSERT_STR_EQ("name_collision", result.failures[0].code);
    tny_extension_result_free(&result);
    tny_extensions_free(x);
    unsetenv("TNY_EXTENSION_HOST");
    ext_fixture_free(&f);
    PASS();
}

TEST extensions_match_real_host_wire(void) {
    ext_fixture f = ext_fixture_new(false);
    char *extensions = path_join(f.tny, "extensions");
    mkdir_p(extensions);
    char *entry = path_join(extensions, "real.py");
    static const char source[] =
        "from tny_ext import context\n"
        "def setup(api):\n"
        "    @api.on('before_agent_start')\n"
        "    def add(event):\n"
        "        return context('real ' + event.prompt, custom_type='real_fixture')\n";
    file_write_atomic(entry, source, sizeof source - 1);
    free(entry); free(extensions);
    char *real_host = path_abs("python/tny_extension_host.py");
    ASSERT(real_host);
    setenv("TNY_EXTENSION_HOST", real_host, 1);
    tny_extensions *x = tny_extensions_new(f.tny, f.workspace, 1000);
    ASSERT(x);
    tny_extension_result result;
    ASSERT_EQ(0, tny_extensions_invoke(
        x, "before_agent_start",
        "{\"schema_version\":1,\"type\":\"before_agent_start\",\"prompt\":\"ship\"}",
        &result));
    ASSERT_EQ(0, result.failure_count);
    ASSERT_EQ(1, result.action_count);
    ASSERT_EQ(TNY_EXTENSION_ACTION_CONTEXT, result.actions[0].kind);
    ASSERT_STR_EQ("real ship", result.actions[0].content);
    ASSERT_STR_EQ("real_fixture", result.actions[0].custom_type);
    tny_extension_result_free(&result);
    tny_extensions_free(x);
    unsetenv("TNY_EXTENSION_HOST");
    free(real_host);
    ext_fixture_free(&f);
    PASS();
}

TEST extensions_reject_non_normalized_or_oversized_input(void) {
    ext_fixture f = ext_fixture_new(true);
    tny_extensions *x = tny_extensions_new(f.tny, f.workspace, 60);
    ASSERT(x);
    tny_extension_result result;
    ASSERT_EQ(-1, tny_extensions_invoke(x, "turn_end", "[]", &result));
    ASSERT_EQ(-1, tny_extensions_invoke(x, "", "{}", &result));
    tny_extensions_free(x);
    ext_fixture_free(&f);
    PASS();
}

TEST extensions_malformed_action_is_a_structured_failure(void) {
    ext_fixture f = ext_fixture_new(true);
    ASSERT(f.host);
    setenv("TNY_EXTENSION_HOST", f.host, 1);
    tny_extensions *x = tny_extensions_new(f.tny, f.workspace, 200);
    ASSERT(x);
    tny_extension_result result;
    ASSERT_EQ(0, tny_extensions_invoke(x, "plan", "{\"type\":\"plan\"}",
                                       &result));
    ASSERT_EQ(0, result.action_count);
    ASSERT_EQ(1, result.failure_count);
    ASSERT_STR_EQ("plan", result.failures[0].event);
    ASSERT_STR_EQ("invalid_action", result.failures[0].code);
    tny_extension_result_free(&result);
    tny_extensions_free(x);
    unsetenv("TNY_EXTENSION_HOST");
    ext_fixture_free(&f);
    PASS();
}

TEST extension_capability_matrices_are_typed_and_secret_free(void) {
    ASSERT_EQ(29, (int)tny_extension_capability_count());
    ASSERT_STR_EQ("extensions.prompt.observe",
                  tny_extension_capability_name(TNY_EXT_CAP_PROMPT_OBSERVE));
    ASSERT_STR_EQ("extensions.project_local.trust",
                  tny_extension_capability_name(TNY_EXT_CAP_PROJECT_LOCAL_TRUST));

    ASSERT_EQ(TNY_EXT_CAP_SUPPORTED,
              tny_extension_capability_get(TNY_BK_OPENAI,
                                           TNY_EXT_CAP_PERMISSION_OBSERVE));
    ASSERT_EQ(TNY_EXT_CAP_UNAVAILABLE,
              tny_extension_capability_get(TNY_BK_CODEX,
                                           TNY_EXT_CAP_PERMISSION_ALLOW_ONCE));
    ASSERT_EQ(TNY_EXT_CAP_UNSUPPORTED,
              tny_extension_capability_get(TNY_BK_CURSOR,
                                           TNY_EXT_CAP_PERMISSION_ALLOW_ONCE));
    ASSERT_STR_EQ("protocol_missing",
                  tny_extension_capability_reason(
                      TNY_BK_CURSOR, TNY_EXT_CAP_PERMISSION_ALLOW_ONCE));
    ASSERT_EQ(TNY_EXT_CAP_UNSUPPORTED,
              tny_extension_capability_get(TNY_BK_ACP,
                                           TNY_EXT_CAP_TOOL_POST_REPLACE));

    static const char secret[] = "CAPABILITY_SENTINEL_SECRET";
    setenv("OPENAI_API_KEY", secret, 1);
    char *json = tny_extension_capabilities_json(TNY_BK_CURSOR, true, false);
    ASSERT(json);
    ASSERT_FALSE(strstr(json, secret));
    yyjson_doc *doc = jparse(json, strlen(json));
    ASSERT(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_EQ(1, (int)jget_int(root, "schema_version", 0));
    ASSERT_STR_EQ("cursor", jget_str(root, "selected_provider"));
    yyjson_val *runtime = jget(root, "extension_runtime");
    ASSERT(jget_bool(runtime, "enabled", false));
    ASSERT_STR_EQ("unavailable", jget_str(runtime, "python"));
    yyjson_val *providers = jget(root, "providers");
    yyjson_val *cursor = jget(providers, "cursor");
    ASSERT_STR_EQ("host", jget_str(cursor, "runtime"));
    yyjson_val *entries = jget(cursor, "entries");
    yyjson_val *permission = jget(entries, "extensions.permission.observe");
    ASSERT_STR_EQ("unsupported", jget_str(permission, "state"));
    ASSERT_STR_EQ("protocol_missing", jget_str(permission, "reason"));
    yyjson_doc_free(doc);
    free(json);
    unsetenv("OPENAI_API_KEY");
    PASS();
}

TEST extensions_known_unavailable_actions_get_capability_diagnostics(void) {
    ext_fixture f = ext_fixture_new(true);
    ASSERT(f.host);
    setenv("TNY_EXTENSION_HOST", f.host, 1);
    tny_extensions *x = tny_extensions_new(f.tny, f.workspace, 200);
    ASSERT(x);
    tny_extensions_set_provider(x, TNY_BK_CURSOR);
    tny_extension_result result;
    ASSERT_EQ(0, tny_extensions_invoke(
        x, "custom_message", "{\"type\":\"custom_message\"}", &result));
    ASSERT_EQ(0, result.action_count);
    ASSERT_EQ(1, result.failure_count);
    ASSERT_STR_EQ("unsupported_capability", result.failures[0].code);
    ASSERT(strstr(result.failures[0].message,
                  "extensions.permission.allow_once is unsupported"));
    tny_extension_result_free(&result);
    tny_extensions_free(x);
    unsetenv("TNY_EXTENSION_HOST");
    ext_fixture_free(&f);
    PASS();
}

TEST extensions_reject_event_over_the_wire_limit(void) {
    ext_fixture f = ext_fixture_new(true);
    tny_extensions *x = tny_extensions_new(f.tny, f.workspace, 60);
    ASSERT(x);
    size_t size = 128u * 1024u + 64u;
    char *event = malloc(size + 1);
    ASSERT(event);
    memset(event, 'x', size);
    event[size] = 0;
    static const char prefix[] = "{\"type\":\"turn_end\",\"padding\":\"";
    memcpy(event, prefix, sizeof prefix - 1);
    event[size - 2] = '"';
    event[size - 1] = '}';
    tny_extension_result result;
    ASSERT_EQ(-1, tny_extensions_invoke(x, "turn_end", event, &result));
    free(event);
    tny_extensions_free(x);
    ext_fixture_free(&f);
    PASS();
}

TEST extensions_negotiate_schema_and_send_selected_capabilities(void) {
    ext_fixture f = ext_fixture_new(true);
    ASSERT(f.host);
    setenv("TNY_EXTENSION_HOST", f.host, 1);
    setenv("FAKE_REQUIRE_SELECTED_PROVIDER", "codex", 1);
    tny_extensions *x = tny_extensions_new(f.tny, f.workspace, 200);
    ASSERT(x);
    tny_extensions_set_provider(x, TNY_BK_CODEX);
    tny_extension_result result;
    ASSERT_EQ(0, tny_extensions_invoke(
        x, "tool_end", "{\"type\":\"tool_end\"}", &result));
    ASSERT_EQ(2, result.action_count);
    ASSERT_EQ(0, result.failure_count);
    tny_extension_result_free(&result);
    tny_extensions_free(x);
    unsetenv("FAKE_REQUIRE_SELECTED_PROVIDER");

    setenv("FAKE_EXTENSION_SCHEMA_MAJOR", "2", 1);
    x = tny_extensions_new(f.tny, f.workspace, 200);
    ASSERT(x);
    ASSERT_EQ(0, tny_extensions_invoke(
        x, "turn_end", "{\"type\":\"turn_end\"}", &result));
    ASSERT_EQ(0, result.action_count);
    ASSERT_EQ(1, result.failure_count);
    ASSERT_STR_EQ("unavailable", result.failures[0].code);
    ASSERT_EQ(TNY_EXTENSIONS_UNAVAILABLE, tny_extensions_get_state(x));
    tny_extension_result_free(&result);
    tny_extensions_free(x);
    unsetenv("FAKE_EXTENSION_SCHEMA_MAJOR");
    unsetenv("TNY_EXTENSION_HOST");
    ext_fixture_free(&f);
    PASS();
}

SUITE(extensions_suite) {
    RUN_TEST(extensions_empty_is_lazy_and_optional);
    RUN_TEST(extensions_discover_sorted_and_return_typed_actions);
    RUN_TEST(extensions_fail_open_on_handler_error_timeout_and_restart);
    RUN_TEST(extensions_missing_host_is_clean_unavailable);
    RUN_TEST(extensions_missing_python_reports_once_and_stays_optional);
    RUN_TEST(extensions_duplicate_name_loads_neither_and_reports_collision);
    RUN_TEST(extensions_match_real_host_wire);
    RUN_TEST(extensions_reject_non_normalized_or_oversized_input);
    RUN_TEST(extensions_malformed_action_is_a_structured_failure);
    RUN_TEST(extension_capability_matrices_are_typed_and_secret_free);
    RUN_TEST(extensions_known_unavailable_actions_get_capability_diagnostics);
    RUN_TEST(extensions_reject_event_over_the_wire_limit);
    RUN_TEST(extensions_negotiate_schema_and_send_selected_capabilities);
}
