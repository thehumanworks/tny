/* test_cursor_management.c -- raw sdk.v1 route and request safety. */
#include "greatest.h"

#include "backends/cursor/management.h"
#include "cli/cli.h"

#include <stdlib.h>
#include <string.h>

int cursor_cli_base64_decode_strict(const char *encoded, size_t encoded_len, uint8_t *decoded,
                                    size_t capacity, size_t *decoded_len);

TEST cursor_management_accepts_only_the_27_outbound_routes(void) {
    for (int i = 0; i < CURSOR_SDK_RPC_COUNT; i++) {
        const cursor_sdk_route *route = cursor_sdk_route_by_id((cursor_sdk_rpc_id)i);
        const char *short_service = route->service + strlen("/sdk.v1.");
        ASSERT_EQ(route, cursor_management_route(short_service, route->method));
        ASSERT_EQ(route, cursor_management_route(route->service + 1, route->method));
        ASSERT_EQ(route, cursor_management_route(route->service, route->method));
    }
    ASSERT(!cursor_management_route("SdkCustomToolCallbackService", "CallCustomTool"));
    ASSERT(!cursor_management_route("SdkStoreCallbackService", "CallStore"));
    ASSERT(!cursor_management_route("SdkAgentService/../SdkCursorService", "Me"));
    ASSERT(!cursor_management_route("SdkAgentService", "send"));
    ASSERT(!cursor_management_route("", "Ping"));
    PASS();
}

TEST cursor_management_requires_one_bounded_json_object(void) {
    char err[160];
    ASSERT_EQ(0, cursor_management_validate_json("{}", err, sizeof err));
    ASSERT_EQ(0,
              cursor_management_validate_json("{\"nested\":{\"future\":true}}", err, sizeof err));
    ASSERT_EQ(-1, cursor_management_validate_json(NULL, err, sizeof err));
    ASSERT(strstr(err, "missing"));
    ASSERT_EQ(-1, cursor_management_validate_json("[]", err, sizeof err));
    ASSERT(strstr(err, "object"));
    ASSERT_EQ(-1, cursor_management_validate_json("{}{}", err, sizeof err));
    ASSERT_EQ(-1, cursor_management_validate_json("{", err, sizeof err));

    char bad_utf8[] = {'{', '"', 'x', '"', ':', '"', (char)0xff, '"', '}', 0};
    ASSERT_EQ(-1, cursor_management_validate_json(bad_utf8, err, sizeof err));
    ASSERT(strstr(err, "UTF-8"));

    char *large = malloc(CURSOR_MAX_MSG_BYTES + 2u);
    ASSERT(large);
    memset(large, ' ', CURSOR_MAX_MSG_BYTES + 1u);
    large[CURSOR_MAX_MSG_BYTES + 1u] = 0;
    ASSERT_EQ(-1, cursor_management_validate_json(large, err, sizeof err));
    ASSERT(strstr(err, "exceeds"));
    free(large);
    PASS();
}

TEST cursor_artifact_base64_is_strict_and_canonical(void) {
    static const struct {
        const char *encoded;
        const char *plain;
        size_t plain_len;
    } valid[] = {
        {"", "", 0},
        {"Zg==", "f", 1},
        {"Zm8=", "fo", 2},
        {"Zm9v", "foo", 3},
        {"/+7d", "\xff\xee\xdd", 3},
    };
    uint8_t decoded[16];
    size_t decoded_len = 99;
    for (size_t i = 0; i < sizeof valid / sizeof valid[0]; i++) {
        ASSERT_EQ(0, cursor_cli_base64_decode_strict(valid[i].encoded, strlen(valid[i].encoded),
                                                     decoded, sizeof decoded, &decoded_len));
        ASSERT_EQ(valid[i].plain_len, decoded_len);
        ASSERT_MEM_EQ(valid[i].plain, decoded, decoded_len);
    }

    static const char *invalid[] = {
        "Zg=",    "Zg===",    "=m9v", "Zm=v",
        "Zg=A",   "Zg==AAAA", "Zh==", /* non-zero unused low four bits */
        "Zm9=",                       /* non-zero unused low two bits */
        "Zm9v\n", "Zm-9",     "____",
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
        ASSERT_EQ(-1, cursor_cli_base64_decode_strict(invalid[i], strlen(invalid[i]), decoded,
                                                      sizeof decoded, &decoded_len));
    const char embedded_nul[] = {'Z', 'g', 0, '='};
    ASSERT_EQ(-1, cursor_cli_base64_decode_strict(embedded_nul, sizeof embedded_nul, decoded,
                                                  sizeof decoded, &decoded_len));
    ASSERT_EQ(-1, cursor_cli_base64_decode_strict("Zm9v", 4, decoded, 2, &decoded_len));
    ASSERT_EQ(-1, cursor_cli_base64_decode_strict(NULL, 0, decoded, sizeof decoded, &decoded_len));
    PASS();
}

TEST cursor_artifact_frame_reports_every_boundary_and_failure(void) {
    char err[160] = "unchanged";
    FILE *stream = tmpfile();
    ASSERT(stream);
    cursor_artifact_output output = {stream, 0};

    ASSERT_EQ(0, cursor_cli_artifact_frame(0, NULL, 0, &output, err, sizeof err));
    ASSERT_EQ(0, output.total);
    ASSERT_EQ(0, ftell(stream));

    static const char *malformed[] = {"{", "[]", "{}", "{\"data\":1}"};
    for (size_t i = 0; i < sizeof malformed / sizeof malformed[0]; i++) {
        err[0] = 0;
        ASSERT_EQ(-1, cursor_cli_artifact_frame(0, malformed[i], strlen(malformed[i]), &output, err,
                                                sizeof err));
        ASSERT(strstr(err, "malformed artifact chunk"));
        ASSERT_EQ(0, output.total);
    }

    const char *invalid = "{\"data\":\"Zh==\"}";
    ASSERT_EQ(-1, cursor_cli_artifact_frame(0, invalid, strlen(invalid), &output, err, sizeof err));
    ASSERT(strstr(err, "not valid base64"));
    ASSERT_EQ(0, output.total);

    output.total = CURSOR_MAX_MSG_BYTES - 1u;
    const char *two = "{\"data\":\"Zm8=\"}";
    ASSERT_EQ(-1, cursor_cli_artifact_frame(0, two, strlen(two), &output, err, sizeof err));
    ASSERT(strstr(err, "artifact exceeds"));
    ASSERT_EQ(CURSOR_MAX_MSG_BYTES - 1u, output.total);

    const char *one = "{\"data\":\"Zg==\"}";
    ASSERT_EQ(0, cursor_cli_artifact_frame(0, one, strlen(one), &output, err, sizeof err));
    ASSERT_EQ(CURSOR_MAX_MSG_BYTES, output.total);
    ASSERT_EQ(1, ftell(stream));
    fclose(stream);

    size_t encoded_len = (CURSOR_MAX_MSG_BYTES / 3u + 1u) * 4u;
    static const char prefix[] = "{\"data\":\"";
    static const char suffix[] = "\"}";
    size_t payload_len = sizeof prefix - 1u + encoded_len + sizeof suffix - 1u;
    char *payload = malloc(payload_len + 1u);
    ASSERT(payload);
    memcpy(payload, prefix, sizeof prefix - 1u);
    memset(payload + sizeof prefix - 1u, 'A', encoded_len);
    memcpy(payload + sizeof prefix - 1u + encoded_len, suffix, sizeof suffix);
    stream = tmpfile();
    ASSERT(stream);
    output = (cursor_artifact_output){stream, 0};
    ASSERT_EQ(-1, cursor_cli_artifact_frame(0, payload, payload_len, &output, err, sizeof err));
    ASSERT(strstr(err, "artifact exceeds"));
    ASSERT_EQ(0, output.total);
    ASSERT_EQ(0, ftell(stream));
    fclose(stream);
    free(payload);

    stream = fopen("/dev/null", "r");
    ASSERT(stream);
    ASSERT_EQ(0, setvbuf(stream, NULL, _IONBF, 0));
    output = (cursor_artifact_output){stream, 0};
    ASSERT_EQ(-1, cursor_cli_artifact_frame(0, one, strlen(one), &output, err, sizeof err));
    ASSERT(strstr(err, "could not write artifact"));
    ASSERT_EQ(0, output.total);
    fclose(stream);
    PASS();
}

SUITE(cursor_management_suite) {
    RUN_TEST(cursor_management_accepts_only_the_27_outbound_routes);
    RUN_TEST(cursor_management_requires_one_bounded_json_object);
    RUN_TEST(cursor_artifact_base64_is_strict_and_canonical);
    RUN_TEST(cursor_artifact_frame_reports_every_boundary_and_failure);
}
