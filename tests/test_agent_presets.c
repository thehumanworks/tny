/* Global agent preset validation, immutable resolution and bounded discovery. */
#include "greatest.h"
#include "core/agent_presets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static yyjson_doc *settings_with_entry(const char *entry) {
    buf_t json;
    buf_init(&json);
    buf_appendf(&json, "{\"agent_presets\":{\"retrieve\":%s}}", entry);
    yyjson_doc *doc = jparse(json.data, json.len);
    buf_free(&json);
    return doc;
}

TEST agent_presets_are_optional_and_global_only(void) {
    tny_ctx ctx = {0};
    ASSERT_EQ(0, tny_agent_presets_validate(&ctx));
    ASSERT_EQ(0, tny_agent_presets_validate(NULL));
    const char *repo = "{\"agent_presets\":{\"retrieve\":{\"provider\":\"repo\","
                       "\"model\":\"not-authority\"}}}";
    ctx.repo_cfg = jparse(repo, strlen(repo));
    const char *settings = "{\"workspaces\":{\"/workspace\":{\"agent_presets\":{"
                           "\"retrieve\":{\"provider\":\"workspace\",\"model\":\"no\"}}}}}";
    ctx.settings = jparse(settings, strlen(settings));
    ASSERT(ctx.repo_cfg && ctx.settings);
    ASSERT_EQ(0, tny_agent_presets_validate(&ctx));
    ASSERT_FALSE(tny_agent_preset_resolve(&ctx, "retrieve"));
    buf_t discovery;
    buf_init(&discovery);
    tny_agent_presets_collect(&ctx, &discovery);
    ASSERT_EQ(0u, discovery.len);
    buf_free(&discovery);
    yyjson_doc_free(ctx.settings);
    yyjson_doc_free(ctx.repo_cfg);
    PASS();
}

TEST agent_preset_resolution_materializes_only_explicit_choices(void) {
    tny_ctx ctx = {.provider_name = "parent-provider",
                   .model = "parent-model",
                   .reasoning_effort = "parent-effort",
                   .api_key = "synthetic-secret"};
    ctx.settings = settings_with_entry("{\"model\":\"independent-model\",\"provider\":\"codex\"}");
    ASSERT(ctx.settings);
    ASSERT_EQ(0, tny_agent_presets_validate(&ctx));
    char *snapshot = tny_agent_preset_resolve(&ctx, "retrieve");
    ASSERT(snapshot);
    ASSERT_STR_EQ("{\"name\":\"retrieve\",\"provider\":\"codex\","
                  "\"model\":\"independent-model\",\"task\":\"context-retrieval\","
                  "\"description\":\"\",\"max_result_bytes\":8192}",
                  snapshot);
    ASSERT_FALSE(strstr(snapshot, "synthetic-secret"));
    ASSERT_FALSE(strstr(snapshot, "parent-"));
    yyjson_doc_free(ctx.settings);
    ctx.settings = settings_with_entry("{\"provider\":\"changed\",\"model\":\"changed\"}");
    ASSERT(ctx.settings);
    yyjson_doc *doc = jparse(snapshot, strlen(snapshot));
    ASSERT(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT(tny_agent_preset_snapshot_valid(root));
    ASSERT_STR_EQ("codex", jget_str(root, "provider"));
    ASSERT_FALSE(jget(root, "effort"));
    ASSERT_STR_EQ("parent-provider", ctx.provider_name);
    ASSERT_STR_EQ("parent-model", ctx.model);
    yyjson_doc_free(doc);
    yyjson_doc_free(ctx.settings);
    free(snapshot);
    PASS();
}

TEST agent_preset_explicit_fields_are_canonical_and_owned(void) {
    tny_ctx ctx = {0};
    ctx.settings = settings_with_entry("{\"description\":\"Find \\u4e16\\u754c\\nfiles\","
                                       "\"max_result_bytes\":1024,\"task\":\"custom.task\","
                                       "\"effort\":\"light\",\"model\":\"fast-model\","
                                       "\"provider\":\"gateway\"}");
    ASSERT(ctx.settings);
    char *snapshot = tny_agent_preset_resolve(&ctx, "retrieve");
    ASSERT(snapshot);
    yyjson_doc_free(ctx.settings);
    ctx.settings = NULL;
    yyjson_doc *doc = jparse(snapshot, strlen(snapshot));
    ASSERT(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT(tny_agent_preset_snapshot_valid(root));
    ASSERT_STR_EQ("gateway", jget_str(root, "provider"));
    ASSERT_STR_EQ("light", jget_str(root, "effort"));
    ASSERT_STR_EQ("custom.task", jget_str(root, "task"));
    ASSERT_STR_EQ("Find 世界\nfiles", jget_str(root, "description"));
    ASSERT_EQ(1024, jget_int(root, "max_result_bytes", 0));
    ASSERT_FALSE(tny_agent_preset_resolve(&ctx, "retrieve"));
    yyjson_doc_free(doc);
    free(snapshot);
    PASS();
}

TEST agent_presets_reject_invalid_maps_names_and_duplicates(void) {
    const char *bad[] = {
        "{\"agent_presets\":null}",
        "{\"agent_presets\":[]}",
        "{\"agent_presets\":true}",
        "{\"agent_presets\":\"text\"}",
        "{\"agent_presets\":{},\"agent_presets\":{}}",
        "{\"agent_presets\":{\"\":{}}}",
        "{\"agent_presets\":{\".hidden\":{}}}",
        "{\"agent_presets\":{\"bad..name\":{}}}",
        "{\"agent_presets\":{\"a/b\":{}}}",
        "{\"agent_presets\":{\"a b\":{}}}",
        "{\"agent_presets\":{\"a\\u0000b\":{}}}",
        "{\"agent_presets\":{\"retrieve\":{\"provider\":\"p\",\"model\":\"m\"},"
        "\"retrie\\u0076e\":{\"provider\":\"p\",\"model\":\"m\"}}}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        tny_ctx ctx = {.settings = jparse(bad[i], strlen(bad[i]))};
        ASSERT(ctx.settings);
        ASSERT_EQ(-1, tny_agent_presets_validate(&ctx));
        yyjson_doc_free(ctx.settings);
    }
    ASSERT_FALSE(tny_agent_preset_resolve(NULL, NULL));
    ASSERT_FALSE(tny_agent_preset_resolve(NULL, "../retrieve"));
    PASS();
}

TEST agent_presets_reject_invalid_and_authority_fields(void) {
    const char *bad[] = {
        "null",
        "[]",
        "{}",
        "{\"provider\":\"p\"}",
        "{\"model\":\"m\"}",
        "{\"provider\":null,\"model\":\"m\"}",
        "{\"provider\":\"\",\"model\":\"m\"}",
        "{\"provider\":\"p\",\"model\":true}",
        "{\"provider\":\"p\",\"model\":\"\"}",
        "{\"provider\":\"p\",\"model\":\"m\\u0000hidden\"}",
        "{\"provider\":\"p\",\"model\":\"m\",\"model\":\"other\"}",
        "{\"provider\":\"p\",\"model\":\"m\",\"mo\\u0064el\":\"other\"}",
        "{\"provider\":\"p\",\"model\":\"m\",\"model\\u0000\":\"other\"}",
        "{\"provider\":\"p\",\"model\":\"m\",\"effort\":null}",
        "{\"provider\":\"p\",\"model\":\"m\",\"effort\":\"\"}",
        "{\"provider\":\"p\",\"model\":\"m\",\"task\":\"../escape\"}",
        "{\"provider\":\"p\",\"model\":\"m\",\"task\":\"\"}",
        "{\"provider\":\"p\",\"model\":\"m\",\"task\":null}",
        "{\"provider\":\"p\",\"model\":\"m\",\"description\":null}",
        "{\"provider\":\"p\",\"model\":\"m\",\"description\":\"a\\u0000b\"}",
        "{\"provider\":\"p\",\"model\":\"m\",\"name\":\"rename\"}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        tny_ctx ctx = {.settings = settings_with_entry(bad[i])};
        ASSERT(ctx.settings);
        ASSERT_EQ(-1, tny_agent_presets_validate(&ctx));
        ASSERT_FALSE(tny_agent_preset_resolve(&ctx, "retrieve"));
        yyjson_doc_free(ctx.settings);
    }
    const char *fields[] = {"api_key",         "api_key_env", "base_url", "headers",
                            "command",         "args",        "tools",    "permissions",
                            "permission_mode", "subscription"};
    for (size_t i = 0; i < sizeof fields / sizeof *fields; ++i) {
        char entry[256];
        snprintf(entry, sizeof entry, "{\"provider\":\"p\",\"model\":\"m\",\"%s\":\"no\"}",
                 fields[i]);
        tny_ctx ctx = {.settings = settings_with_entry(entry)};
        ASSERT(ctx.settings);
        ASSERT_EQ(-1, tny_agent_presets_validate(&ctx));
        yyjson_doc_free(ctx.settings);
    }
    PASS();
}

TEST agent_presets_result_caps_are_strict_integers(void) {
    const char *caps[] = {
        "1024",   "65536",  "8192", "1023", "65537", "0",        "-1",
        "1024.0", "1024.5", "1e4",  "true", "null",  "\"8192\"", "18446744073709551615"};
    for (size_t i = 0; i < sizeof caps / sizeof *caps; ++i) {
        char entry[256];
        snprintf(entry, sizeof entry,
                 "{\"provider\":\"p\",\"model\":\"m\","
                 "\"max_result_bytes\":%s}",
                 caps[i]);
        tny_ctx ctx = {.settings = settings_with_entry(entry)};
        ASSERT(ctx.settings);
        ASSERT_EQ(i < 3 ? 0 : -1, tny_agent_presets_validate(&ctx));
        yyjson_doc_free(ctx.settings);
    }
    PASS();
}

TEST agent_presets_text_limits_count_utf8_bytes(void) {
    const char *fields[] = {"provider", "model", "effort", "description"};
    for (size_t i = 0; i < sizeof fields / sizeof *fields; ++i) {
        for (unsigned length = 256; length <= 258; length += 2) {
            char text[259];
            for (unsigned n = 0; n < length; n += 2) {
                text[n] = (char)0xc3;
                text[n + 1] = (char)0xa9;
            }
            text[length] = '\0';
            buf_t entry;
            buf_init(&entry);
            buf_appends(&entry, "{");
            if (strcmp(fields[i], "provider") != 0) buf_appends(&entry, "\"provider\":\"p\",");
            if (strcmp(fields[i], "model") != 0) buf_appends(&entry, "\"model\":\"m\",");
            buf_appendf(&entry, "\"%s\":", fields[i]);
            jescape(&entry, text);
            buf_appends(&entry, "}");
            tny_ctx ctx = {.settings = settings_with_entry(entry.data)};
            ASSERT(ctx.settings);
            ASSERT_EQ(length == 256 ? 0 : -1, tny_agent_presets_validate(&ctx));
            yyjson_doc_free(ctx.settings);
            buf_free(&entry);
        }
    }
    PASS();
}

TEST agent_presets_count_and_name_boundaries_are_bounded(void) {
    for (unsigned count = 32; count <= 33; ++count) {
        buf_t json;
        buf_init(&json);
        buf_appends(&json, "{\"agent_presets\":{");
        for (unsigned i = 0; i < count; ++i)
            buf_appendf(&json, "%s\"retrieve-%u\":{\"provider\":\"p\",\"model\":\"m\"}",
                        i ? "," : "", i);
        buf_appends(&json, "}}");
        tny_ctx ctx = {.settings = jparse(json.data, json.len)};
        ASSERT(ctx.settings);
        ASSERT_EQ(count == 32 ? 0 : -1, tny_agent_presets_validate(&ctx));
        yyjson_doc_free(ctx.settings);
        buf_free(&json);
    }
    for (unsigned length = 63; length <= 64; ++length) {
        char name[65], json[256];
        memset(name, 'a', length);
        name[length] = '\0';
        snprintf(json, sizeof json,
                 "{\"agent_presets\":{\"%s\":{\"provider\":\"p\","
                 "\"model\":\"m\"}}}",
                 name);
        tny_ctx ctx = {.settings = jparse(json, strlen(json))};
        ASSERT(ctx.settings);
        ASSERT_EQ(length == 63 ? 0 : -1, tny_agent_presets_validate(&ctx));
        yyjson_doc_free(ctx.settings);
    }
    PASS();
}

TEST agent_preset_snapshot_validator_rejects_noncanonical_fields(void) {
    const char *bad[] = {
        "null",
        "{}",
        "{\"provider\":\"p\",\"model\":\"m\"}",
        "{\"name\":\"retrieve\",\"provider\":\"p\",\"model\":\"m\","
        "\"task\":\"context-retrieval\",\"description\":\"\"}",
        "{\"name\":\"retrieve\",\"provider\":\"p\",\"model\":\"m\","
        "\"task\":\"context-retrieval\",\"max_result_bytes\":8192}",
        "{\"name\":\"retrieve\",\"provider\":\"p\",\"model\":\"m\","
        "\"description\":\"\",\"max_result_bytes\":8192}",
        "{\"provider\":\"p\",\"model\":\"m\",\"task\":\"context-retrieval\","
        "\"description\":\"\",\"max_result_bytes\":8192}",
        "{\"name\":\"retrieve\",\"provider\":\"p\",\"model\":\"m\",\"effort\":null,"
        "\"task\":\"context-retrieval\",\"description\":\"\",\"max_result_bytes\":8192}",
        "{\"name\":\"retrieve\",\"provider\":\"p\",\"model\":\"m\",\"api_key\":\"secret\","
        "\"task\":\"context-retrieval\",\"description\":\"\",\"max_result_bytes\":8192}",
        "{\"name\":\"retrieve\",\"provider\":\"p\",\"model\":\"m\",\"name\":\"other\","
        "\"task\":\"context-retrieval\",\"description\":\"\",\"max_result_bytes\":8192}",
    };
    ASSERT_FALSE(tny_agent_preset_snapshot_valid(NULL));
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        yyjson_doc *doc = jparse(bad[i], strlen(bad[i]));
        ASSERT(doc);
        ASSERT_FALSE(tny_agent_preset_snapshot_valid(yyjson_doc_get_root(doc)));
        yyjson_doc_free(doc);
    }
    PASS();
}

TEST agent_preset_discovery_is_sorted_and_selector_free(void) {
    const char *settings =
        "{\"agent_presets\":{"
        "\"z-last\":{\"provider\":\"private-provider\",\"model\":\"private-model\","
        "\"description\":\"line one\\nline two\",\"effort\":\"private-effort\"},"
        "\"a-first\":{\"provider\":\"p\",\"model\":\"m\"}}}";
    tny_ctx ctx = {.settings = jparse(settings, strlen(settings))};
    ASSERT(ctx.settings);
    buf_t discovery;
    buf_init(&discovery);
    tny_agent_presets_collect(&ctx, &discovery);
    ASSERT_FALSE(discovery.oom);
    ASSERT_STR_EQ("\nAvailable agent presets (names and descriptions only):\n"
                  "- a-first: \"\"\n- z-last: \"line one\\nline two\"\n",
                  discovery.data);
    ASSERT_FALSE(strstr(discovery.data, "private-"));
    yyjson_doc_free(ctx.settings);
    buf_free(&discovery);
    PASS();
}

TEST agent_preset_diagnostics_never_echo_unknown_values(void) {
    FILE *capture = tmpfile();
    ASSERT(capture);
    int saved = dup(STDERR_FILENO);
    ASSERT(saved >= 0);
    fflush(stderr);
    ASSERT(dup2(fileno(capture), STDERR_FILENO) >= 0);
    tny_ctx ctx = {.settings = settings_with_entry("{\"provider\":\"p\",\"model\":\"m\","
                                                   "\"synthetic-secret-key\":\"secret-value\"}")};
    int rc = tny_agent_presets_validate(&ctx);
    char *missing = tny_agent_preset_resolve(NULL, "secret-name");
    bool found = missing != NULL;
    fflush(stderr);
    int restored = dup2(saved, STDERR_FILENO);
    close(saved);
    rewind(capture);
    char diagnostic[2048];
    size_t size = fread(diagnostic, 1, sizeof diagnostic - 1, capture);
    diagnostic[size] = '\0';
    fclose(capture);
    yyjson_doc_free(ctx.settings);
    free(missing);
    ASSERT(restored >= 0);
    ASSERT_EQ(-1, rc);
    ASSERT_FALSE(found);
    ASSERT(strstr(diagnostic, "unsupported field"));
    ASSERT_FALSE(strstr(diagnostic, "secret"));
    PASS();
}

TEST agent_presets_survive_generic_settings_roundtrip(void) {
    char directory[] = "/tmp/tny-agent-presets-XXXXXX";
    ASSERT(mkdtemp(directory));
    tny_ctx ctx = {.tny_dir = directory};
    ctx.settings_path = path_join(directory, "settings.json");
    ctx.settings = settings_with_entry("{\"provider\":\"codex\",\"model\":\"explicit-model\"}");
    ASSERT(ctx.settings_path && ctx.settings);
    char *before = tny_agent_preset_resolve(&ctx, "retrieve");
    ASSERT(before);
    ASSERT_EQ(0, tny_settings_set_str(&ctx, "provider", "other"));
    ASSERT_EQ(0, tny_settings_set_ui(&ctx, "mode", "inline"));
    char *after = tny_agent_preset_resolve(&ctx, "retrieve");
    ASSERT(after);
    ASSERT_STR_EQ(before, after);
    unlink(ctx.settings_path);
    rmdir(directory);
    free(ctx.settings_path);
    yyjson_doc_free(ctx.settings);
    free(before);
    free(after);
    PASS();
}

SUITE(agent_presets_suite) {
    RUN_TEST(agent_presets_are_optional_and_global_only);
    RUN_TEST(agent_preset_resolution_materializes_only_explicit_choices);
    RUN_TEST(agent_preset_explicit_fields_are_canonical_and_owned);
    RUN_TEST(agent_presets_reject_invalid_maps_names_and_duplicates);
    RUN_TEST(agent_presets_reject_invalid_and_authority_fields);
    RUN_TEST(agent_presets_result_caps_are_strict_integers);
    RUN_TEST(agent_presets_text_limits_count_utf8_bytes);
    RUN_TEST(agent_presets_count_and_name_boundaries_are_bounded);
    RUN_TEST(agent_preset_snapshot_validator_rejects_noncanonical_fields);
    RUN_TEST(agent_preset_discovery_is_sorted_and_selector_free);
    RUN_TEST(agent_preset_diagnostics_never_echo_unknown_values);
    RUN_TEST(agent_presets_survive_generic_settings_roundtrip);
}
