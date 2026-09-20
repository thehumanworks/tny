#include "greatest.h"
#include "core/swarm_manifest.h"
#include "util/image_io.h"
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

static const char valid_v2[] =
    "{\"swarms\":[{\"swarms\":[],\"agents\":[],\"coordinator\":{"
    "\"workspace\":{\"policy\":\"shared_read_only\"},\"depends_on\":[\"implementer\"],"
    "\"acceptance\":[\"Find material defects or state none\"],\"deliverable\":\"Review "
    "report\",\"purpose\":\"independent review\",\"name\":\"reviewer\"},"
    "\"purpose\":\"review implementation\"}],\"agents\":[{\"purpose\":\"research\","
    "\"name\":\"analyst\"},{\"workspace\":{\"base\":\"main\",\"policy\":\"isolated\"},"
    "\"depends_on\":[\"analyst\"],\"acceptance\":[\"Focused tests pass\"],"
    "\"deliverable\":\"Tested patch\",\"purpose\":\"implement\",\"name\":\"implementer\"}],"
    "\"coordinator\":{\"acceptance\":[\"Report checks and blockers\"],"
    "\"deliverable\":\"Integrated result\",\"purpose\":\"own synthesis\",\"name\":\"lead\"},"
    "\"purpose\":\"deliver factory change\",\"version\":2}";

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
    char digest[65];
    ASSERT(tny_image_io_sha256_hex(manifest->canonical_json, manifest->canonical_len, digest));
    ASSERT_STR_EQ("0ae0df2fb2eb927b23338d11261e60398de08dd176bc141c3935bc30486f3c6a", digest);
    tny_swarm_manifest *roundtrip = NULL;
    ASSERT_EQ(0, tny_swarm_manifest_parse(manifest->canonical_json, manifest->canonical_len, 4,
                                          &roundtrip, err, sizeof err));
    ASSERT_STR_EQ(manifest->canonical_json, roundtrip->canonical_json);
    tny_swarm_manifest_free(roundtrip);
    tny_swarm_manifest_free(manifest);
    PASS();
}

TEST swarm_manifest_v2_canonicalizes_contracts_and_resolves_names(void) {
    tny_swarm_manifest *manifest = NULL;
    char err[256];
    ASSERT_EQ(0,
              tny_swarm_manifest_parse(valid_v2, strlen(valid_v2), 3, &manifest, err, sizeof err));
    ASSERT_EQ(TNY_SWARM_MANIFEST_VERSION_V2, manifest->version);
    ASSERT_EQ(2, manifest->group_count);
    ASSERT_EQ(3, manifest->participant_count);
    ASSERT_STR_EQ("Integrated result", manifest->groups[0].coordinator_contract.deliverable);
    ASSERT_EQ(1, manifest->groups[0].coordinator_contract.acceptance_count);
    ASSERT_STR_EQ("Report checks and blockers",
                  manifest->groups[0].coordinator_contract.acceptance[0]);
    ASSERT_FALSE(manifest->groups[0].coordinator_contract.workspace_declared);
    ASSERT_STR_EQ("analyst", manifest->participants[0].name);
    ASSERT_FALSE(manifest->participants[0].contract.workspace_declared);
    ASSERT_EQ(TNY_SWARM_WORKSPACE_SHARED_READ_ONLY,
              manifest->participants[0].contract.workspace_policy);
    ASSERT_STR_EQ("implementer", manifest->participants[1].name);
    ASSERT_EQ(1, manifest->participants[1].contract.dependency_count);
    ASSERT_EQ(0, manifest->participants[1].contract.dependencies[0]);
    ASSERT_EQ(TNY_SWARM_WORKSPACE_ISOLATED, manifest->participants[1].contract.workspace_policy);
    ASSERT_STR_EQ("main", manifest->participants[1].contract.workspace_base);
    ASSERT_STR_EQ("reviewer", manifest->participants[2].name);
    ASSERT(manifest->participants[2].coordinator);
    ASSERT_EQ(1, manifest->participants[2].contract.dependencies[0]);
    ASSERT_EQ(TNY_SWARM_WORKSPACE_SHARED_READ_ONLY,
              manifest->participants[2].contract.workspace_policy);
    const char *expected =
        "{\"version\":2,\"purpose\":\"deliver factory change\",\"coordinator\":{\"name\":"
        "\"lead\",\"purpose\":\"own synthesis\",\"deliverable\":\"Integrated result\","
        "\"acceptance\":[\"Report checks and blockers\"]},\"agents\":[{\"name\":\"analyst\","
        "\"purpose\":\"research\"},{\"name\":\"implementer\",\"purpose\":\"implement\","
        "\"deliverable\":\"Tested patch\",\"acceptance\":[\"Focused tests pass\"],"
        "\"depends_on\":[\"analyst\"],\"workspace\":{\"policy\":\"isolated\",\"base\":"
        "\"main\"}}],\"swarms\":[{\"purpose\":\"review implementation\",\"coordinator\":{"
        "\"name\":\"reviewer\",\"purpose\":\"independent review\",\"deliverable\":\"Review "
        "report\",\"acceptance\":[\"Find material defects or state none\"],\"depends_on\":["
        "\"implementer\"],\"workspace\":{\"policy\":\"shared_read_only\"}},\"agents\":[],"
        "\"swarms\":[]}]}";
    ASSERT_STR_EQ(expected, manifest->canonical_json);
    tny_swarm_manifest *roundtrip = NULL;
    ASSERT_EQ(0,
              tny_swarm_manifest_parse(expected, strlen(expected), 3, &roundtrip, err, sizeof err));
    ASSERT_STR_EQ(expected, roundtrip->canonical_json);
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
    ASSERT(invalid_contains("{\"version\":3,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\","
                            "\"purpose\":\"p\"},\"agents\":[{\"name\":\"a\",\"purpose\":\"p\"}],"
                            "\"swarms\":[]}",
                            16, "version must be 1 or 2"));
    ASSERT(invalid_contains("{\"version\":1,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\","
                            "\"purpose\":\"p\"},\"agents\":[{\"name\":\"a\",\"purpose\":\"p\","
                            "\"deliverable\":\"not in v1\"}],\"swarms\":[]}",
                            16, "exactly name and purpose"));
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

TEST swarm_manifest_v2_rejects_malformed_contracts_and_root_worker_fields(void) {
    ASSERT(invalid_contains(
        "{\"version\":2,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\",\"purpose\":\"p\","
        "\"depends_on\":[]},\"agents\":[{\"name\":\"a\",\"purpose\":\"p\"}],\"swarms\":[]}",
        16, "root coordinator cannot declare"));
    ASSERT(invalid_contains(
        "{\"version\":2,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\",\"purpose\":\"p\","
        "\"workspace\":{\"policy\":\"isolated\"}},\"agents\":[{\"name\":\"a\",\"purpose\":"
        "\"p\"}],\"swarms\":[]}",
        16, "root coordinator cannot declare"));
    ASSERT(invalid_contains(
        "{\"version\":2,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\",\"purpose\":\"p\"},"
        "\"agents\":[{\"name\":\"a\",\"purpose\":\"p\",\"acceptance\":[]}],\"swarms\":[]}",
        16, "acceptance must contain 1..16"));
    ASSERT(invalid_contains(
        "{\"version\":2,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\",\"purpose\":\"p\"},"
        "\"agents\":[{\"name\":\"a\",\"purpose\":\"p\",\"acceptance\":[\" \"]}],\"swarms\":[]}",
        16, "acceptance criteria"));
    ASSERT(invalid_contains(
        "{\"version\":2,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\",\"purpose\":\"p\"},"
        "\"agents\":[{\"name\":\"a\",\"purpose\":\"p\",\"deliverable\":\"x\","
        "\"deliverable\":\"y\"}],\"swarms\":[]}",
        16, "missing, duplicate, or unknown"));
    ASSERT(invalid_contains(
        "{\"version\":2,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\",\"purpose\":\"p\"},"
        "\"agents\":[{\"name\":\"a\",\"purpose\":\"p\",\"workspace\":{\"policy\":"
        "\"shared_read_only\",\"base\":\"main\"}}],\"swarms\":[]}",
        16, "cannot declare base"));
    ASSERT(invalid_contains(
        "{\"version\":2,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\",\"purpose\":\"p\"},"
        "\"agents\":[{\"name\":\"a\",\"purpose\":\"p\",\"workspace\":{\"policy\":"
        "\"shared_writable\"}}],\"swarms\":[]}",
        16, "must be shared_read_only or isolated"));
    ASSERT(invalid_contains(
        "{\"version\":2,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\",\"purpose\":\"p\"},"
        "\"agents\":[{\"name\":\"a\",\"purpose\":\"p\",\"workspace\":{\"policy\":"
        "\"isolated\",\"extra\":true}}],\"swarms\":[]}",
        16, "workspace has missing, duplicate, or unknown fields"));
    PASS();
}

TEST swarm_manifest_v2_rejects_invalid_dependencies_and_cycles(void) {
    ASSERT(invalid_contains(
        "{\"version\":2,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\",\"purpose\":\"p\"},"
        "\"agents\":[{\"name\":\"a\",\"purpose\":\"p\",\"depends_on\":[\"missing\"]}],"
        "\"swarms\":[]}",
        16, "unknown dependency"));
    ASSERT(invalid_contains(
        "{\"version\":2,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\",\"purpose\":\"p\"},"
        "\"agents\":[{\"name\":\"a\",\"purpose\":\"p\",\"depends_on\":[\"lead\"]}],"
        "\"swarms\":[]}",
        16, "cannot depend on root coordinator"));
    ASSERT(invalid_contains(
        "{\"version\":2,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\",\"purpose\":\"p\"},"
        "\"agents\":[{\"name\":\"a\",\"purpose\":\"p\",\"depends_on\":[\"a\"]}],\"swarms\":[]}",
        16, "cannot depend on itself"));
    ASSERT(invalid_contains(
        "{\"version\":2,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\",\"purpose\":\"p\"},"
        "\"agents\":[{\"name\":\"a\",\"purpose\":\"p\",\"depends_on\":[\"b\",\"b\"]},"
        "{\"name\":\"b\",\"purpose\":\"p\"}],\"swarms\":[]}",
        16, "duplicate dependency"));
    ASSERT(invalid_contains(
        "{\"version\":2,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\",\"purpose\":\"p\"},"
        "\"agents\":[{\"name\":\"a\",\"purpose\":\"p\",\"depends_on\":[\"b\"]},"
        "{\"name\":\"b\",\"purpose\":\"p\",\"depends_on\":[\"a\"]}],\"swarms\":[]}",
        16, "dependencies contain a cycle"));
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
    ASSERT(invalid_contains(many.data, 15, "available capacity of 15"));
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
    char *padded = malloc(TNY_SWARM_MANIFEST_MAX_BYTES);
    ASSERT(padded);
    memcpy(padded, valid_nested, strlen(valid_nested));
    memset(padded + strlen(valid_nested), ' ', TNY_SWARM_MANIFEST_MAX_BYTES - strlen(valid_nested));
    ASSERT_EQ(0, file_write_atomic(path, padded, TNY_SWARM_MANIFEST_MAX_BYTES));
    free(padded);
    tny_swarm_manifest *boundary = NULL;
    ASSERT_EQ(0, tny_swarm_manifest_parse_file(path, 4, &boundary, err, sizeof err));
    ASSERT_STR_EQ(manifest->canonical_json, boundary->canonical_json);
    tny_swarm_manifest_free(boundary);
    ASSERT_EQ(0, unlink(path));
    ASSERT_STR_EQ("lead", manifest->groups[0].coordinator_name);
    tny_swarm_manifest_free(manifest);
    free(path);
    ASSERT_EQ(0, rmdir(dir));
    PASS();
}

TEST swarm_manifest_accepts_exact_depth_text_and_file_boundaries(void) {
    buf_t json;
    buf_init(&json);
    for (unsigned depth = 0; depth < TNY_SWARM_MANIFEST_MAX_DEPTH; ++depth) {
        buf_appends(&json, depth ? "{" : "{\"version\":1,");
        buf_appendf(&json,
                    "\"purpose\":\"p\",\"coordinator\":{\"name\":\"c%u\","
                    "\"purpose\":\"p\"},\"agents\":[],\"swarms\":[",
                    depth);
    }
    for (unsigned depth = 0; depth < TNY_SWARM_MANIFEST_MAX_DEPTH; ++depth)
        buf_appends(&json, "]}");
    char err[256];
    tny_swarm_manifest *manifest = NULL;
    ASSERT_EQ(0, tny_swarm_manifest_parse(json.data, json.len, 16, &manifest, err, sizeof err));
    ASSERT_EQ(TNY_SWARM_MANIFEST_MAX_DEPTH, manifest->group_count);
    tny_swarm_manifest_free(manifest);
    buf_free(&json);

    char name[TNY_SWARM_MANIFEST_MAX_NAME_BYTES + 1u];
    char purpose[TNY_SWARM_MANIFEST_MAX_PURPOSE_BYTES + 1u];
    memset(name, 'n', sizeof name - 1u);
    name[sizeof name - 1u] = 0;
    memset(purpose, 'p', sizeof purpose - 1u);
    purpose[sizeof purpose - 1u] = 0;
    buf_init(&json);
    buf_appendf(&json,
                "{\"version\":1,\"purpose\":\"p\",\"coordinator\":{\"name\":\"root\","
                "\"purpose\":\"p\"},\"agents\":[{\"name\":\"%s\",\"purpose\":\"%s\"}],"
                "\"swarms\":[]}",
                name, purpose);
    ASSERT_EQ(0, tny_swarm_manifest_parse(json.data, json.len, 1, &manifest, err, sizeof err));
    ASSERT_EQ(TNY_SWARM_MANIFEST_MAX_NAME_BYTES, strlen(manifest->participants[0].name));
    ASSERT_EQ(TNY_SWARM_MANIFEST_MAX_PURPOSE_BYTES, strlen(manifest->participants[0].purpose));
    tny_swarm_manifest_free(manifest);
    char *padded = malloc(TNY_SWARM_MANIFEST_MAX_BYTES);
    ASSERT(padded);
    ASSERT(json.len < TNY_SWARM_MANIFEST_MAX_BYTES);
    memcpy(padded, json.data, json.len);
    memset(padded + json.len, ' ', TNY_SWARM_MANIFEST_MAX_BYTES - json.len);
    ASSERT_EQ(0, tny_swarm_manifest_parse(padded, TNY_SWARM_MANIFEST_MAX_BYTES, 1, &manifest, err,
                                          sizeof err));
    tny_swarm_manifest_free(manifest);
    free(padded);
    buf_free(&json);
    PASS();
}

TEST swarm_manifest_v2_enforces_contract_boundaries(void) {
    char deliverable[TNY_SWARM_MANIFEST_MAX_DELIVERABLE_BYTES + 2u];
    char criterion[TNY_SWARM_MANIFEST_MAX_ACCEPTANCE_BYTES + 2u];
    char base[TNY_SWARM_MANIFEST_MAX_WORKSPACE_BASE + 2u];
    memset(deliverable, 'd', sizeof deliverable - 1u);
    deliverable[sizeof deliverable - 1u] = 0;
    memset(criterion, 'c', sizeof criterion - 1u);
    criterion[sizeof criterion - 1u] = 0;
    memset(base, 'b', sizeof base - 1u);
    base[sizeof base - 1u] = 0;

    buf_t json;
    buf_init(&json);
    buf_appendf(&json,
                "{\"version\":2,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\","
                "\"purpose\":\"p\"},\"agents\":[{\"name\":\"a\",\"purpose\":\"p\","
                "\"deliverable\":\"%.*s\",\"acceptance\":[\"%.*s\"],\"workspace\":{"
                "\"policy\":\"isolated\",\"base\":\"%.*s\"}}],\"swarms\":[]}",
                TNY_SWARM_MANIFEST_MAX_DELIVERABLE_BYTES, deliverable,
                TNY_SWARM_MANIFEST_MAX_ACCEPTANCE_BYTES, criterion,
                TNY_SWARM_MANIFEST_MAX_WORKSPACE_BASE, base);
    tny_swarm_manifest *manifest = NULL;
    char err[256];
    ASSERT_EQ(0, tny_swarm_manifest_parse(json.data, json.len, 1, &manifest, err, sizeof err));
    tny_swarm_manifest_free(manifest);
    buf_free(&json);

    buf_init(&json);
    buf_appendf(&json,
                "{\"version\":2,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\","
                "\"purpose\":\"p\"},\"agents\":[{\"name\":\"a\",\"purpose\":\"p\","
                "\"deliverable\":\"%s\"}],\"swarms\":[]}",
                deliverable);
    ASSERT(invalid_contains(json.data, 1, "deliverable must be 1..4096"));
    buf_free(&json);

    buf_init(&json);
    buf_appendf(&json,
                "{\"version\":2,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\","
                "\"purpose\":\"p\"},\"agents\":[{\"name\":\"a\",\"purpose\":\"p\","
                "\"acceptance\":[\"%s\"]}],\"swarms\":[]}",
                criterion);
    ASSERT(invalid_contains(json.data, 1, "acceptance criteria must be 1..1024"));
    buf_free(&json);

    buf_init(&json);
    buf_appendf(&json,
                "{\"version\":2,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\","
                "\"purpose\":\"p\"},\"agents\":[{\"name\":\"a\",\"purpose\":\"p\","
                "\"workspace\":{\"policy\":\"isolated\",\"base\":\"%s\"}}],\"swarms\":[]}",
                base);
    ASSERT(invalid_contains(json.data, 1, "workspace base must be 1..256"));
    buf_free(&json);

    buf_init(&json);
    buf_appends(&json, "{\"version\":2,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\","
                       "\"purpose\":\"p\"},\"agents\":[{\"name\":\"a\",\"purpose\":\"p\","
                       "\"acceptance\":[");
    for (unsigned i = 0; i < TNY_SWARM_MANIFEST_MAX_ACCEPTANCE; ++i) {
        if (i) buf_appends(&json, ",");
        buf_appendf(&json, "\"criterion%u\"", i);
    }
    buf_appends(&json, "]}],\"swarms\":[]}");
    ASSERT_EQ(0, tny_swarm_manifest_parse(json.data, json.len, 1, &manifest, err, sizeof err));
    ASSERT_EQ(TNY_SWARM_MANIFEST_MAX_ACCEPTANCE,
              manifest->participants[0].contract.acceptance_count);
    tny_swarm_manifest_free(manifest);
    const char suffix[] = "]}],\"swarms\":[]}";
    ASSERT(str_ends(json.data, suffix));
    json.len -= strlen(suffix);
    json.data[json.len] = 0;
    buf_appends(&json, ",\"one too many\"]}],\"swarms\":[]}");
    ASSERT(invalid_contains(json.data, 1, "acceptance must contain 1..16"));
    buf_free(&json);

    buf_init(&json);
    buf_appends(&json, "{\"version\":2,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\","
                       "\"purpose\":\"p\"},\"agents\":[");
    for (unsigned i = 0; i < TNY_SWARM_MANIFEST_MAX_PARTICIPANTS; ++i) {
        if (i) buf_appends(&json, ",");
        buf_appendf(&json, "{\"name\":\"a%u\",\"purpose\":\"p\"", i);
        if (i + 1u == TNY_SWARM_MANIFEST_MAX_PARTICIPANTS) {
            buf_appends(&json, ",\"depends_on\":[");
            for (unsigned d = 0; d < TNY_SWARM_MANIFEST_MAX_DEPENDENCIES; ++d) {
                if (d) buf_appends(&json, ",");
                buf_appendf(&json, "\"a%u\"", d);
            }
            buf_appends(&json, "]");
        }
        buf_appends(&json, "}");
    }
    buf_appends(&json, "],\"swarms\":[]}");
    ASSERT_EQ(0, tny_swarm_manifest_parse(json.data, json.len, 16, &manifest, err, sizeof err));
    ASSERT_EQ(TNY_SWARM_MANIFEST_MAX_DEPENDENCIES,
              manifest->participants[15].contract.dependency_count);
    tny_swarm_manifest_free(manifest);
    buf_free(&json);

    buf_init(&json);
    buf_appends(&json, "{\"version\":2,\"purpose\":\"p\",\"coordinator\":{\"name\":\"lead\","
                       "\"purpose\":\"p\"},\"agents\":[{\"name\":\"a\",\"purpose\":\"p\","
                       "\"depends_on\":[");
    for (unsigned i = 0; i <= TNY_SWARM_MANIFEST_MAX_DEPENDENCIES; ++i) {
        if (i) buf_appends(&json, ",");
        buf_appendf(&json, "\"dependency%u\"", i);
    }
    buf_appends(&json, "]}],\"swarms\":[]}");
    ASSERT(invalid_contains(json.data, 1, "depends_on may contain at most 15 names"));
    buf_free(&json);
    PASS();
}

SUITE(swarm_manifest_suite) {
    RUN_TEST(swarm_manifest_flattens_nested_groups_and_canonicalizes);
    RUN_TEST(swarm_manifest_v2_canonicalizes_contracts_and_resolves_names);
    RUN_TEST(swarm_manifest_rejects_ambiguous_or_incomplete_shapes);
    RUN_TEST(swarm_manifest_rejects_blank_purposes_and_global_duplicate_names);
    RUN_TEST(swarm_manifest_v2_rejects_malformed_contracts_and_root_worker_fields);
    RUN_TEST(swarm_manifest_v2_rejects_invalid_dependencies_and_cycles);
    RUN_TEST(swarm_manifest_enforces_depth_size_and_shared_capacity);
    RUN_TEST(swarm_manifest_file_load_is_bounded_and_owned);
    RUN_TEST(swarm_manifest_accepts_exact_depth_text_and_file_boundaries);
    RUN_TEST(swarm_manifest_v2_enforces_contract_boundaries);
}
