#include "greatest.h"
#include "core/swarm_manifest.h"
#include "util/util.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char valid_nested[] =
    "{\n"
    "  \"swarms\": [{\"agents\":[{\"purpose\":\"write tests\",\"name\":\"tester\"}],"
    "\"coordinator\":{\"purpose\":\"synthesise implementation\",\"name\":\"builder\"},"
    "\"swarms\":[{\"purpose\":\"review deeply\",\"coordinator\":{\"name\":\"reviewer\","
    "\"purpose\":\"synthesise review\"},\"agents\":[],\"swarms\":[]}],"
    "\"purpose\":\"implement parser\"}],\n"
    "  \"agents\": [{\"purpose\":\"study contracts\",\"name\":\"analyst\"}],\n"
    "  \"coordinator\": {\"purpose\":\"own final result\",\"name\":\"lead\"},\n"
    "  \"purpose\": \"deliver purposeful swarms\",\n"
    "  \"version\": 1\n"
    "}";

TEST swarm_manifest_flattens_nested_groups_and_canonicalizes(void) {
    tny_swarm_manifest *manifest = NULL;
    char err[256];
    ASSERT_EQ(0, tny_swarm_manifest_parse(valid_nested, strlen(valid_nested), 4, &manifest, err,
                                          sizeof err));
    ASSERT(manifest);
    ASSERT_EQ(1, manifest->version);
    ASSERT_EQ(3, manifest->group_count);
    ASSERT_EQ(4, manifest->participant_count);
    ASSERT_STR_EQ("deliver purposeful swarms", manifest->groups[0].purpose);
    ASSERT_EQ(SIZE_MAX, manifest->groups[0].parent);
    ASSERT_EQ(SIZE_MAX, manifest->groups[0].coordinator_participant);
    ASSERT_EQ(0, manifest->groups[1].parent);
    ASSERT_EQ(1, manifest->groups[2].parent);
    /* Direct root agents precede recursively visited nested groups. */
    ASSERT_STR_EQ("analyst", manifest->participants[0].name);
    ASSERT_FALSE(manifest->participants[0].coordinator);
    ASSERT_EQ(0, manifest->participants[0].group);
    ASSERT_STR_EQ("builder", manifest->participants[1].name);
    ASSERT(manifest->participants[1].coordinator);
    ASSERT_EQ(1, manifest->participants[1].group);
    ASSERT_EQ(1, manifest->groups[1].coordinator_participant);
    ASSERT_STR_EQ("tester", manifest->participants[2].name);
    ASSERT_STR_EQ("reviewer", manifest->participants[3].name);
    ASSERT(manifest->participants[3].coordinator);
    ASSERT_EQ(3, manifest->groups[2].coordinator_participant);
    const char *expected =
        "{\"version\":1,\"purpose\":\"deliver purposeful swarms\",\"coordinator\":{"
        "\"name\":\"lead\",\"purpose\":\"own final result\"},\"agents\":[{\"name\":"
        "\"analyst\",\"purpose\":\"study contracts\"}],\"swarms\":[{\"purpose\":"
        "\"implement parser\",\"coordinator\":{\"name\":\"builder\",\"purpose\":"
        "\"synthesise implementation\"},\"agents\":[{\"name\":\"tester\",\"purpose\":"
        "\"write tests\"}],\"swarms\":[{\"purpose\":\"review deeply\",\"coordinator\":{"
        "\"name\":\"reviewer\",\"purpose\":\"synthesise review\"},\"agents\":[],"
        "\"swarms\":[]}]}]}";
    ASSERT_EQ(strlen(expected), manifest->canonical_len);
    ASSERT_STR_EQ(expected, manifest->canonical_json);
    tny_swarm_manifest *roundtrip = NULL;
    ASSERT_EQ(0, tny_swarm_manifest_parse(manifest->canonical_json, manifest->canonical_len, 4,
                                          &roundtrip, err, sizeof err));
    ASSERT_STR_EQ(manifest->canonical_json, roundtrip->canonical_json);
    tny_swarm_manifest_free(roundtrip);
    tny_swarm_manifest_free(manifest);
    PASS();
}

static bool invalid_contains(const char *json, size_t capacity, const char *message) {
    tny_swarm_manifest *manifest = (void *)1;
    char err[256];
    int rc = tny_swarm_manifest_parse(json, strlen(json), capacity, &manifest, err, sizeof err);
    if (rc == 0) tny_swarm_manifest_free(manifest);
    return rc == -1 && manifest == NULL && strstr(err, message) != NULL;
}

TEST swarm_manifest_rejects_ambiguous_or_incomplete_shapes(void) {
    ASSERT(invalid_contains("{", 16, "invalid swarm JSON"));
    ASSERT(invalid_contains("{\"version\":1,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\","
                            "\"purpose\":\"p\"},\"agents\":[],\"swarms\":[]}",
                            16, "at least one"));
    ASSERT(invalid_contains("{\"version\":1,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\","
                            "\"purpose\":\"p\"},\"agents\":[{\"name\":\"a\",\"purpose\":\"p\"}],"
                            "\"swarms\":[{\"purpose\":\"nested\",\"agents\":[],\"swarms\":[]}]}",
                            16, "missing, duplicate, or unknown"));
    ASSERT(invalid_contains("{\"version\":1,\"purpose\":\"p\",\"purpose\":\"q\",\"coordinator\":{"
                            "\"name\":\"lead\",\"purpose\":\"p\"},\"agents\":[{\"name\":\"a\","
                            "\"purpose\":\"p\"}],\"swarms\":[]}",
                            16, "missing, duplicate, or unknown"));
    ASSERT(invalid_contains("{\"version\":1,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\","
                            "\"purpose\":\"p\",\"extra\":1},\"agents\":[{\"name\":\"a\","
                            "\"purpose\":\"p\"}],\"swarms\":[]}",
                            16, "exactly name and purpose"));
    ASSERT(invalid_contains("{\"version\":2,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\","
                            "\"purpose\":\"p\"},\"agents\":[{\"name\":\"a\",\"purpose\":\"p\"}],"
                            "\"swarms\":[]}",
                            16, "version must be 1"));
    ASSERT(invalid_contains(
        "{\"version\":1,\"purpose\":\"p\",\"coordinator\":{\"name\":\"le\\u0000ad\","
        "\"purpose\":\"p\"},\"agents\":[{\"name\":\"a\",\"purpose\":\"p\"}],"
        "\"swarms\":[]}",
        16, "coordinator name"));
    PASS();
}

TEST swarm_manifest_rejects_blank_purposes_and_global_duplicate_names(void) {
    ASSERT(
        invalid_contains("{\"version\":1,\"purpose\":\" \\t\",\"coordinator\":{\"name\":\"lead\","
                         "\"purpose\":\"p\"},\"agents\":[{\"name\":\"a\",\"purpose\":\"p\"}],"
                         "\"swarms\":[]}",
                         16, "non-blank"));
    ASSERT(invalid_contains("{\"version\":1,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\","
                            "\"purpose\":\"p\"},\"agents\":[{\"name\":\"lead\",\"purpose\":\"p\"}],"
                            "\"swarms\":[]}",
                            16, "duplicate swarm participant name"));
    ASSERT(invalid_contains("{\"version\":1,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\","
                            "\"purpose\":\"p\"},\"agents\":[{\"name\":\"a\",\"purpose\":\"p\"}],"
                            "\"swarms\":[{\"purpose\":\"n\",\"coordinator\":{\"name\":\"a\","
                            "\"purpose\":\"p\"},\"agents\":[],\"swarms\":[]}]}",
                            16, "duplicate swarm participant name"));
    PASS();
}

TEST swarm_manifest_enforces_depth_size_and_shared_capacity(void) {
    const char *too_deep =
        "{\"version\":1,\"purpose\":\"0\",\"coordinator\":{\"name\":\"c0\",\"purpose\":\"p\"},"
        "\"agents\":[],\"swarms\":[{\"purpose\":\"1\",\"coordinator\":{\"name\":\"c1\","
        "\"purpose\":\"p\"},\"agents\":[],\"swarms\":[{\"purpose\":\"2\",\"coordinator\":{"
        "\"name\":\"c2\",\"purpose\":\"p\"},\"agents\":[],\"swarms\":[{\"purpose\":\"3\","
        "\"coordinator\":{\"name\":\"c3\",\"purpose\":\"p\"},\"agents\":[],\"swarms\":[{"
        "\"purpose\":\"4\",\"coordinator\":{\"name\":\"c4\",\"purpose\":\"p\"},"
        "\"agents\":[],\"swarms\":[]}]}]}]}]}";
    ASSERT(invalid_contains(too_deep, 16, "maximum depth 4"));
    ASSERT(invalid_contains(valid_nested, 3, "available capacity of 3"));
    buf_t many;
    buf_init(&many);
    buf_appends(&many, "{\"version\":1,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\","
                       "\"purpose\":\"p\"},\"agents\":[");
    for (int i = 0; i < 16; ++i) {
        if (i) buf_appends(&many, ",");
        buf_appendf(&many, "{\"name\":\"a%d\",\"purpose\":\"p\"}", i);
    }
    buf_appends(&many, "],\"swarms\":[]}");
    tny_swarm_manifest *maximum = NULL;
    char err[256];
    ASSERT_EQ(0, tny_swarm_manifest_parse(many.data, many.len, 16, &maximum, err, sizeof err));
    ASSERT_EQ(16, maximum->participant_count);
    tny_swarm_manifest_free(maximum);
    /* One nested coordinator would be the seventeenth launched participant. */
    const char suffix[] = "],\"swarms\":[]}";
    ASSERT(str_ends(many.data, suffix));
    many.len -= strlen(suffix);
    many.data[many.len] = 0;
    buf_appends(&many, "],\"swarms\":[{\"purpose\":\"n\",\"coordinator\":{\"name\":\"nested\","
                       "\"purpose\":\"p\"},\"agents\":[],\"swarms\":[]}]}");
    ASSERT(invalid_contains(many.data, 16, "available capacity of 16"));
    buf_free(&many);
    char *oversize = malloc(TNY_SWARM_MANIFEST_MAX_BYTES + 2u);
    ASSERT(oversize);
    memset(oversize, ' ', TNY_SWARM_MANIFEST_MAX_BYTES + 1u);
    oversize[TNY_SWARM_MANIFEST_MAX_BYTES + 1u] = 0;
    tny_swarm_manifest *manifest = NULL;
    ASSERT_EQ(-1, tny_swarm_manifest_parse(oversize, TNY_SWARM_MANIFEST_MAX_BYTES + 1u, 16,
                                           &manifest, err, sizeof err));
    ASSERT(strstr(err, "1..65536 bytes"));
    ASSERT_EQ(-1, tny_swarm_manifest_parse(valid_nested, strlen(valid_nested), 0, &manifest, err,
                                           sizeof err));
    ASSERT(strstr(err, "capacity must be between 1 and 16"));
    free(oversize);
    PASS();
}

TEST swarm_manifest_file_load_is_bounded_and_owned(void) {
    char dir[] = "/tmp/tny-swarm-manifest-XXXXXX";
    ASSERT(mkdtemp(dir));
    char *path = path_join(dir, "swarm.json");
    ASSERT(path);
    ASSERT_EQ(0, file_write_atomic(path, valid_nested, strlen(valid_nested)));
    tny_swarm_manifest *manifest = NULL;
    char err[256];
    ASSERT_EQ(0, tny_swarm_manifest_parse_file(path, 4, &manifest, err, sizeof err));
    ASSERT(manifest && manifest->canonical_json);
    ASSERT_EQ(0, unlink(path));
    ASSERT_STR_EQ("lead", manifest->groups[0].coordinator_name);
    tny_swarm_manifest_free(manifest);
    free(path);
    ASSERT_EQ(0, rmdir(dir));
    PASS();
}

SUITE(swarm_manifest_suite) {
    RUN_TEST(swarm_manifest_flattens_nested_groups_and_canonicalizes);
    RUN_TEST(swarm_manifest_rejects_ambiguous_or_incomplete_shapes);
    RUN_TEST(swarm_manifest_rejects_blank_purposes_and_global_duplicate_names);
    RUN_TEST(swarm_manifest_enforces_depth_size_and_shared_capacity);
    RUN_TEST(swarm_manifest_file_load_is_bounded_and_owned);
}
