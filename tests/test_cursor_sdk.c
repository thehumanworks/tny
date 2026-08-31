/* test_cursor_sdk.c -- v1.0.30 route, negotiation, and error-detail tests. */
#include "greatest.h"

#include "backends/cursor/sdk_client.h"

#include <arpa/inet.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int sdk_stream_listener(int *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = 0};
    socklen_t len = sizeof address;
    int one = 1;
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0 ||
        bind(fd, (struct sockaddr *)&address, sizeof address) != 0 || listen(fd, 1) != 0 ||
        getsockname(fd, (struct sockaddr *)&address, &len) != 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    *port = ntohs(address.sin_port);
    return fd;
}

static pid_t sdk_stream_serve(int listener, const char *body, size_t body_len, size_t slice) {
    pid_t child = fork();
    if (child != 0) return child;
    int client = accept(listener, NULL, NULL);
    if (client < 0) _exit(2);
    char request[4096];
    (void)read(client, request, sizeof request);
    char headers[256];
    int header_len = snprintf(headers, sizeof headers,
                              "HTTP/1.1 200 OK\r\nContent-Type: application/connect+json\r\n"
                              "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                              body_len);
    if (write(client, headers, (size_t)header_len) != header_len) _exit(3);
    if (!slice) slice = body_len ? body_len : 1u;
    for (size_t offset = 0; offset < body_len;) {
        size_t count = body_len - offset < slice ? body_len - offset : slice;
        ssize_t written = write(client, body + offset, count);
        if (written <= 0) _exit(4);
        offset += (size_t)written;
    }
    close(client);
    close(listener);
    _exit(0);
}

typedef struct {
    int count;
    uint8_t flags;
    char payload[32];
} sdk_stream_capture;

static void capture_sdk_frame(uint8_t flags, const char *payload, size_t len, void *ud) {
    sdk_stream_capture *capture = ud;
    capture->flags = flags;
    capture->count++;
    if (len >= sizeof capture->payload) len = sizeof capture->payload - 1u;
    memcpy(capture->payload, payload, len);
    capture->payload[len] = 0;
}

static int pump_sdk_fixture(const char *body, size_t body_len, size_t slice,
                            sdk_stream_capture *capture, cursor_sdk_error *sdk_error, char *err,
                            size_t errlen) {
    int port = 0;
    int listener = sdk_stream_listener(&port);
    if (listener < 0) return -99;
    pid_t child = sdk_stream_serve(listener, body, body_len, slice);
    if (child < 0) {
        close(listener);
        return -98;
    }
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    cursor_sdk_client client;
    cursor_sdk_client_init(&client, url, "test-token");
    client.negotiated = true;
    client.version.capability_count = 1;
    strcpy(client.version.capabilities[0], "agent.send");
    int rc = cursor_sdk_invoke_stream(&client, CURSOR_SDK_RPC_SEND, "{}", err, errlen);
    for (int attempts = 0; rc == 0 && attempts < 1000; attempts++) {
        rc = cursor_sdk_stream_pump(&client, capture_sdk_frame, capture, sdk_error, err, errlen);
        if (rc == 0) {
            struct pollfd pfd = {cursor_sdk_stream_fd(&client), POLLIN, 0};
            (void)poll(&pfd, 1, 10);
        }
    }
    cursor_sdk_client_close(&client);
    close(listener);
    int status = 0;
    (void)waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -97;
    return rc;
}

TEST sdk_v1_route_table_is_complete_and_lookup_is_stable(void) {
    ASSERT_EQ(27, CURSOR_SDK_RPC_COUNT);
    for (int i = 0; i < CURSOR_SDK_RPC_COUNT; i++) {
        const cursor_sdk_route *route = cursor_sdk_route_by_id((cursor_sdk_rpc_id)i);
        ASSERT(route);
        ASSERT_EQ(i, (int)route->id);
        ASSERT(route->service && route->service[0] == '/');
        ASSERT(route->method && *route->method);
        ASSERT_EQ(route, cursor_sdk_route_find(route->service, route->method));
    }
    ASSERT(!cursor_sdk_route_by_id((cursor_sdk_rpc_id)-1));
    ASSERT(!cursor_sdk_route_by_id(CURSOR_SDK_RPC_COUNT));
    ASSERT(!cursor_sdk_route_find(CURSOR_SVC_AGENT, "NotAnRpc"));

    const cursor_sdk_route *send = cursor_sdk_route_by_id(CURSOR_SDK_RPC_SEND);
    ASSERT_EQ(CURSOR_SDK_SERVER_STREAM, send->kind);
    ASSERT_STR_EQ("agent.send", send->required_capability);
    const cursor_sdk_route *download = cursor_sdk_route_by_id(CURSOR_SDK_RPC_DOWNLOAD_ARTIFACT);
    ASSERT_EQ(CURSOR_SDK_SERVER_STREAM, download->kind);
    ASSERT_STR_EQ("artifacts.chunked", download->required_capability);
    ASSERT(!cursor_sdk_route_by_id(CURSOR_SDK_RPC_GET_VERSION)->required_capability);
    PASS();
}

TEST sdk_v1_version_parser_keeps_unknown_caps_and_deduplicates(void) {
    const char *json = "{\"bridgeVersion\":\"1.0.30\",\"protocolVersion\":\"sdk.v1\","
                       "\"capabilities\":[\"agent.send\",\"future.widget\",\"agent.send\"],"
                       "\"futureField\":true}";
    cursor_sdk_version version;
    char err[128];
    ASSERT_EQ(0, cursor_sdk_version_parse(&version, json, strlen(json), err, sizeof err));
    ASSERT_STR_EQ("1.0.30", version.bridge_version);
    ASSERT_STR_EQ("sdk.v1", version.protocol_version);
    ASSERT_EQ(2, version.capability_count);
    ASSERT(cursor_sdk_has_capability(&version, "agent.send"));
    ASSERT(cursor_sdk_has_capability(&version, "future.widget"));
    ASSERT_FALSE(cursor_sdk_has_capability(&version, "run.wait"));
    ASSERT(
        cursor_sdk_route_supported(&version, cursor_sdk_route_by_id(CURSOR_SDK_RPC_GET_VERSION)));
    ASSERT(cursor_sdk_route_supported(&version, cursor_sdk_route_by_id(CURSOR_SDK_RPC_SEND)));
    ASSERT_FALSE(
        cursor_sdk_route_supported(&version, cursor_sdk_route_by_id(CURSOR_SDK_RPC_WAIT_LIVE_RUN)));
    PASS();
}

TEST sdk_v1_version_parser_rejects_missing_contract_fields(void) {
    cursor_sdk_version version;
    char err[128];
    ASSERT_EQ(-1, cursor_sdk_version_parse(&version, "{}", 2, err, sizeof err));
    ASSERT(strstr(err, "malformed"));
    const char *wrong_capabilities = "{\"bridgeVersion\":\"1\","
                                     "\"protocolVersion\":\"sdk.v1\","
                                     "\"capabilities\":{}}";
    ASSERT_EQ(-1, cursor_sdk_version_parse(&version, wrong_capabilities, strlen(wrong_capabilities),
                                           err, sizeof err));
    PASS();
}

TEST sdk_v1_version_parser_enforces_exact_bounds_and_truncation(void) {
    cursor_sdk_version version;
    char err[160];
    ASSERT_EQ(-1, cursor_sdk_version_parse(NULL, "{}", 2, err, sizeof err));
    ASSERT_EQ(-1, cursor_sdk_version_parse(&version, NULL, 1, err, sizeof err));

    const char *head = "{\"bridgeVersion\":\"1\",\"protocolVersion\":\"sdk.v1\","
                       "\"capabilities\":[]}";
    size_t head_len = strlen(head);
    char *exact = malloc(CURSOR_MAX_MSG_BYTES + 1u);
    ASSERT(exact);
    memcpy(exact, head, head_len);
    memset(exact + head_len, ' ', CURSOR_MAX_MSG_BYTES - head_len);
    exact[CURSOR_MAX_MSG_BYTES] = 0;
    ASSERT_EQ(0, cursor_sdk_version_parse(&version, exact, CURSOR_MAX_MSG_BYTES, err, sizeof err));
    ASSERT_EQ(0, version.capability_count);
    ASSERT_FALSE(version.capabilities_truncated);
    /* The allocation has one terminator slot; use an independent oversize
     * length with NULL input to hit the guard without an out-of-bounds read. */
    ASSERT_EQ(-1,
              cursor_sdk_version_parse(&version, NULL, CURSOR_MAX_MSG_BYTES + 1u, err, sizeof err));
    free(exact);

    buf_t json;
    buf_init(&json);
    buf_appends(&json, "{\"bridgeVersion\":\"1\",\"protocolVersion\":\"sdk.v1\","
                       "\"capabilities\":[");
    for (size_t i = 0; i < CURSOR_SDK_MAX_CAPABILITIES + 1u; i++) {
        if (i) buf_appends(&json, ",");
        buf_appendf(&json, "\"cap-%zu\"", i);
    }
    buf_appends(&json, "]}");
    ASSERT_EQ(0, cursor_sdk_version_parse(&version, json.data, json.len, err, sizeof err));
    ASSERT_EQ(CURSOR_SDK_MAX_CAPABILITIES, version.capability_count);
    ASSERT(version.capabilities_truncated);
    ASSERT_STR_EQ("cap-63", version.capabilities[63]);
    buf_free(&json);

    char exact_name[CURSOR_SDK_MAX_CAPABILITY_LEN + 1u];
    memset(exact_name, 'e', sizeof exact_name - 1u);
    exact_name[sizeof exact_name - 1u] = 0;
    char long_name[CURSOR_SDK_MAX_CAPABILITY_LEN + 2u];
    memset(long_name, 'x', sizeof long_name - 1u);
    long_name[sizeof long_name - 1u] = 0;
    buf_init(&json);
    buf_appends(&json, "{\"bridgeVersion\":\"1\",\"protocolVersion\":\"sdk.v1\","
                       "\"capabilities\":[");
    jescape(&json, exact_name);
    buf_appends(&json, ",");
    jescape(&json, long_name);
    buf_appends(&json, "]}");
    ASSERT_EQ(0, cursor_sdk_version_parse(&version, json.data, json.len, err, sizeof err));
    ASSERT_EQ(1, version.capability_count);
    ASSERT_STR_EQ(exact_name, version.capabilities[0]);
    ASSERT(version.capabilities_truncated);
    buf_free(&json);
    PASS();
}

TEST sdk_v1_capability_scans_and_route_gates_stop_at_count(void) {
    cursor_sdk_version version;
    memset(&version, 0, sizeof version);
    /* Poison the first unused slot: a <= mutation must not inspect it. */
    strcpy(version.capabilities[0], "poison-unused");
    ASSERT_FALSE(cursor_sdk_has_capability(&version, "poison-unused"));
    ASSERT_FALSE(cursor_sdk_has_capability(NULL, "x"));
    ASSERT_FALSE(cursor_sdk_has_capability(&version, NULL));

    const cursor_sdk_route *control = cursor_sdk_route_by_id(CURSOR_SDK_RPC_GET_VERSION);
    const cursor_sdk_route *send = cursor_sdk_route_by_id(CURSOR_SDK_RPC_SEND);
    ASSERT(cursor_sdk_route_supported(&version, control));
    ASSERT_FALSE(cursor_sdk_route_supported(&version, send));
    ASSERT_FALSE(cursor_sdk_route_supported(&version, NULL));

    version.capability_count = 1;
    strcpy(version.capabilities[0], "agent.send");
    ASSERT(cursor_sdk_has_capability(&version, "agent.send"));
    ASSERT(cursor_sdk_route_supported(&version, send));

    memset(&version, 0, sizeof version);
    version.capability_count = 3;
    strcpy(version.capabilities[0], "first");
    strcpy(version.capabilities[1], "second");
    strcpy(version.capabilities[2], "last");
    strcpy(version.capabilities[3], "poison-one-past");
    ASSERT(cursor_sdk_has_capability(&version, "second"));
    ASSERT(cursor_sdk_has_capability(&version, "last"));
    ASSERT_FALSE(cursor_sdk_has_capability(&version, "poison-one-past"));

    const char *duplicates = "{\"bridgeVersion\":\"1\",\"protocolVersion\":\"sdk.v1\","
                             "\"capabilities\":[\"first\",\"second\",\"second\"]}";
    char err[160];
    ASSERT_EQ(0,
              cursor_sdk_version_parse(&version, duplicates, strlen(duplicates), err, sizeof err));
    ASSERT_EQ(2, version.capability_count);
    ASSERT_STR_EQ("first", version.capabilities[0]);
    ASSERT_STR_EQ("second", version.capabilities[1]);
    PASS();
}

TEST sdk_v1_request_validation_fails_before_transport(void) {
    cursor_sdk_client client;
    cursor_sdk_client_init(&client, "http://127.0.0.1:1", "test-token");
    cursor_sdk_error sdk_error;
    cursor_sdk_error_init(&sdk_error);
    char err[200];

    ASSERT(!cursor_sdk_invoke_unary(&client, CURSOR_SDK_RPC_COUNT, "{}", 1, &sdk_error, err,
                                    sizeof err));
    ASSERT(strstr(err, "invalid sdk.v1 invocation"));
    ASSERT(!cursor_sdk_invoke_unary(&client, CURSOR_SDK_RPC_SEND, "{}", 1, &sdk_error, err,
                                    sizeof err));
    ASSERT(strstr(err, "invalid sdk.v1 invocation"));
    ASSERT(!cursor_sdk_invoke_unary(&client, CURSOR_SDK_RPC_PING, NULL, 1, &sdk_error, err,
                                    sizeof err));
    ASSERT(strstr(err, "invalid sdk.v1 invocation"));
    ASSERT(!cursor_sdk_invoke_unary(&client, CURSOR_SDK_RPC_PING, "[]", 1, &sdk_error, err,
                                    sizeof err));
    ASSERT(strstr(err, "JSON object"));
    ASSERT(!cursor_sdk_invoke_unary(&client, CURSOR_SDK_RPC_CREATE_AGENT, "{}", 1, &sdk_error, err,
                                    sizeof err));
    ASSERT(strstr(err, "required capability agent.create"));

    client.negotiated = true;
    ASSERT(!cursor_sdk_invoke_unary(&client, CURSOR_SDK_RPC_CREATE_AGENT, "{}", 1, &sdk_error, err,
                                    sizeof err));
    ASSERT(strstr(err, "required capability agent.create"));
    client.negotiated = false;
    client.version.capability_count = 1;
    strcpy(client.version.capabilities[0], "agent.create");
    ASSERT(!cursor_sdk_invoke_unary(&client, CURSOR_SDK_RPC_CREATE_AGENT, "{}", 1, &sdk_error, err,
                                    sizeof err));
    ASSERT(strstr(err, "required capability agent.create"));

    size_t exact_len = CURSOR_MAX_MSG_BYTES;
    char *exact = malloc(exact_len + 2u);
    ASSERT(exact);
    memcpy(exact, "{\"x\":\"", 6);
    memset(exact + 6, 'a', exact_len - 8u);
    memcpy(exact + exact_len - 2u, "\"}", 2);
    exact[exact_len] = 0;
    ASSERT(!cursor_sdk_invoke_unary(&client, CURSOR_SDK_RPC_PING, exact, 1, &sdk_error, err,
                                    sizeof err));
    ASSERT_FALSE(strstr(err, "exceeds"));
    exact[exact_len] = ' ';
    exact[exact_len + 1u] = 0;
    ASSERT(!cursor_sdk_invoke_unary(&client, CURSOR_SDK_RPC_PING, exact, 1, &sdk_error, err,
                                    sizeof err));
    ASSERT(strstr(err, "exceeds"));
    free(exact);
    cursor_sdk_error_free(&sdk_error);
    cursor_sdk_client_close(&client);
    PASS();
}

TEST sdk_v1_structured_error_decodes_all_supported_metadata(void) {
    static const char detail[] =
        "CiRyZXF1ZXN0LWlkLXRoYXQtbXVzdC1yZW1haW4tY29tcGxldGUQEhoJc2xvdyBkb3duIhlodHRw"
        "czovL2hlbHAuZXhhbXBsZS9yYXRlKglhbnRocm9waWMyCAgDEIDKte4BOgcIZBACGNIJmAYq";
    char json[1024];
    snprintf(json, sizeof json,
             "{\"code\":\"resource_exhausted\",\"message\":\"fallback\","
             "\"details\":[{\"type\":\"type.googleapis.com/sdk.v1.SdkErrorDetails\","
             "\"value\":\"%s\"}]}",
             detail);
    cursor_sdk_error error;
    cursor_sdk_error_init(&error);
    ASSERT_EQ(0, cursor_sdk_error_parse(&error, json, strlen(json), 429));
    ASSERT_EQ(429, error.http_status);
    ASSERT_STR_EQ("resource_exhausted", error.connect_code);
    ASSERT(error.has_sdk_details);
    ASSERT_EQ(CURSOR_SDK_ERR_RATE_LIMIT_EXCEEDED, error.sdk_error_code);
    ASSERT_STR_EQ("RATE_LIMIT_EXCEEDED", cursor_sdk_error_code_name(error.sdk_error_code));
    ASSERT_STR_EQ("slow down", error.message);
    ASSERT_STR_EQ("request-id-that-must-remain-complete", error.request_id);
    ASSERT_STR_EQ("https://help.example/rate", error.help_url);
    ASSERT_STR_EQ("anthropic", error.provider);
    ASSERT(error.has_retry_after);
    ASSERT_EQ(3, error.retry_after_seconds);
    ASSERT_EQ(500000000, error.retry_after_nanos);
    ASSERT(error.has_rate_limit);
    ASSERT(error.rate_limit.has_limit);
    ASSERT_EQ(100, error.rate_limit.limit);
    ASSERT(error.rate_limit.has_remaining);
    ASSERT_EQ(2, error.rate_limit.remaining);
    ASSERT(error.rate_limit.has_reset_epoch_seconds);
    ASSERT_EQ(1234, error.rate_limit.reset_epoch_seconds);
    cursor_sdk_error_free(&error);
    PASS();
}

TEST sdk_v1_plain_connect_error_remains_useful(void) {
    const char *json = "{\"code\":\"unauthenticated\",\"message\":\"Unauthorized\","
                       "\"details\":[{\"type\":\"future.Detail\",\"value\":\"AA\"}]}";
    cursor_sdk_error error;
    cursor_sdk_error_init(&error);
    ASSERT_EQ(0, cursor_sdk_error_parse(&error, json, strlen(json), 401));
    ASSERT_FALSE(error.has_sdk_details);
    ASSERT_EQ(CURSOR_SDK_ERR_UNSPECIFIED, error.sdk_error_code);
    ASSERT_STR_EQ("unauthenticated", error.connect_code);
    ASSERT_STR_EQ("Unauthorized", error.message);
    ASSERT_STR_EQ("UNKNOWN", cursor_sdk_error_code_name(9001));
    cursor_sdk_error_free(&error);
    PASS();
}

TEST sdk_v1_stream_end_error_wrapper_is_decoded(void) {
    const char *json = "{\"error\":{\"code\":\"unavailable\",\"message\":\"try later\"},"
                       "\"metadata\":{\"retry-after\":[\"2\"]}}";
    cursor_sdk_error error;
    cursor_sdk_error_init(&error);
    ASSERT_EQ(0, cursor_sdk_error_parse(&error, json, strlen(json), 200));
    ASSERT_STR_EQ("unavailable", error.connect_code);
    ASSERT_STR_EQ("try later", error.message);
    cursor_sdk_error_free(&error);
    PASS();
}

TEST sdk_v1_malformed_detail_does_not_hide_connect_error(void) {
    const char *json = "{\"code\":\"internal\",\"message\":\"outer\","
                       "\"details\":[{\"type\":\"sdk.v1.SdkErrorDetails\",\"value\":\"%%%\"}]}";
    cursor_sdk_error error;
    cursor_sdk_error_init(&error);
    ASSERT_EQ(0, cursor_sdk_error_parse(&error, json, strlen(json), 500));
    ASSERT_FALSE(error.has_sdk_details);
    ASSERT_STR_EQ("internal", error.connect_code);
    ASSERT_STR_EQ("outer", error.message);
    cursor_sdk_error_free(&error);
    PASS();
}

TEST sdk_v1_partially_decodable_detail_is_transactional(void) {
    /* request_id, followed by a forbidden protobuf group wire type */
    const char *json = "{\"code\":\"internal\",\"message\":\"outer\","
                       "\"details\":[{\"type\":\"sdk.v1.SdkErrorDetails\","
                       "\"value\":\"CgNyZXMP\"}]}";
    cursor_sdk_error error;
    cursor_sdk_error_init(&error);
    ASSERT_EQ(0, cursor_sdk_error_parse(&error, json, strlen(json), 500));
    ASSERT_FALSE(error.has_sdk_details);
    ASSERT(!error.request_id);
    ASSERT_STR_EQ("outer", error.message);
    cursor_sdk_error_free(&error);
    PASS();
}

TEST sdk_v1_stream_requires_one_valid_end_stream_envelope(void) {
    static const struct {
        const char *name;
        uint8_t first_flags;
        const char *first_payload;
        uint8_t second_flags;
        const char *second_payload;
        uint8_t third_flags;
        const char *third_payload;
        const char *error_text;
    } cases[] = {
        {"missing", 0, "{\"message\":\"one\"}", 0, NULL, 0, NULL, "without an EndStream"},
        {"duplicate", 2, "{}", 2, "{}", 0, NULL, "duplicate EndStream"},
        {"data-after", 2, "{}", 0, "{\"late\":true}", 0, NULL, "data after EndStream"},
        {"compressed", 1, "{\"message\":\"one\"}", 2, "{}", 0, NULL, "compressed Connect"},
        {"compressed-end", 3, "{}", 0, NULL, 0, NULL, "compressed Connect"},
        {"unknown-flags", 4, "{}", 2, "{}", 0, NULL, "unknown Connect"},
        {"empty-end", 2, "", 0, NULL, 0, NULL, "malformed EndStream"},
        {"array-end", 2, "[]", 0, NULL, 0, NULL, "malformed EndStream"},
        {"bad-error", 2, "{\"error\":\"bad\"}", 0, NULL, 0, NULL, "malformed EndStream"},
        {"bad-metadata", 2, "{\"metadata\":{\"x\":\"y\"}}", 0, NULL, 0, NULL,
         "malformed EndStream"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        buf_t body;
        buf_init(&body);
        connect_frame_encode(&body, cases[i].first_flags, cases[i].first_payload,
                             strlen(cases[i].first_payload));
        if (cases[i].second_payload)
            connect_frame_encode(&body, cases[i].second_flags, cases[i].second_payload,
                                 strlen(cases[i].second_payload));
        if (cases[i].third_payload)
            connect_frame_encode(&body, cases[i].third_flags, cases[i].third_payload,
                                 strlen(cases[i].third_payload));
        sdk_stream_capture capture = {0};
        cursor_sdk_error sdk_error;
        cursor_sdk_error_init(&sdk_error);
        char err[256] = {0};
        ASSERT_EQm(cases[i].name, -1,
                   pump_sdk_fixture(body.data, body.len, 1, &capture, &sdk_error, err, sizeof err));
        ASSERTm(cases[i].name, strstr(err, cases[i].error_text));
        ASSERT_EQm(cases[i].name, 0, capture.flags);
        cursor_sdk_error_free(&sdk_error);
        buf_free(&body);
    }

    buf_t valid;
    buf_init(&valid);
    const char *data = "{\"message\":\"one\"}";
    const char *end = "{\"error\":null,\"metadata\":null,\"future\":true}";
    connect_frame_encode(&valid, 0, data, strlen(data));
    connect_frame_encode(&valid, 2, end, strlen(end));
    sdk_stream_capture capture = {0};
    cursor_sdk_error sdk_error;
    cursor_sdk_error_init(&sdk_error);
    char err[256] = {0};
    ASSERT_EQ(1, pump_sdk_fixture(valid.data, valid.len, 1, &capture, &sdk_error, err, sizeof err));
    ASSERT_EQ(1, capture.count);
    ASSERT_STR_EQ("{\"message\":\"one\"}", capture.payload);
    cursor_sdk_error_free(&sdk_error);
    buf_free(&valid);

    buf_t structured;
    buf_init(&structured);
    const char *end_error = "{\"error\":{\"code\":\"unavailable\",\"message\":\"try later\"}}";
    connect_frame_encode(&structured, 2, end_error, strlen(end_error));
    capture = (sdk_stream_capture){0};
    cursor_sdk_error_init(&sdk_error);
    ASSERT_EQ(-1, pump_sdk_fixture(structured.data, structured.len, 2, &capture, &sdk_error, err,
                                   sizeof err));
    ASSERT_STR_EQ("unavailable", sdk_error.connect_code);
    ASSERT_STR_EQ("try later", sdk_error.message);
    ASSERT(strstr(err, "try later"));
    cursor_sdk_error_free(&sdk_error);
    buf_free(&structured);

    buf_init(&structured);
    end_error = "{\"error\":{\"message\":\"missing code remains an error\",\"future\":1},"
                "\"metadata\":{\"trace\":[\"one\",\"two\"]}}";
    connect_frame_encode(&structured, 2, end_error, strlen(end_error));
    capture = (sdk_stream_capture){0};
    cursor_sdk_error_init(&sdk_error);
    ASSERT_EQ(-1, pump_sdk_fixture(structured.data, structured.len, 3, &capture, &sdk_error, err,
                                   sizeof err));
    ASSERT_STR_EQ("missing code remains an error", sdk_error.message);
    ASSERT_EQ(0, sdk_error.connect_code[0]);
    cursor_sdk_error_free(&sdk_error);
    buf_free(&structured);

    const char truncated[] = {0, 0, 0, 0, 4, '{', '}'};
    capture = (sdk_stream_capture){0};
    cursor_sdk_error_init(&sdk_error);
    ASSERT_EQ(-1, pump_sdk_fixture(truncated, sizeof truncated, 1, &capture, &sdk_error, err,
                                   sizeof err));
    ASSERT(strstr(err, "truncated Connect envelope"));
    cursor_sdk_error_free(&sdk_error);
    PASS();
}

SUITE(cursor_sdk_suite) {
    RUN_TEST(sdk_v1_route_table_is_complete_and_lookup_is_stable);
    RUN_TEST(sdk_v1_version_parser_keeps_unknown_caps_and_deduplicates);
    RUN_TEST(sdk_v1_version_parser_rejects_missing_contract_fields);
    RUN_TEST(sdk_v1_version_parser_enforces_exact_bounds_and_truncation);
    RUN_TEST(sdk_v1_capability_scans_and_route_gates_stop_at_count);
    RUN_TEST(sdk_v1_request_validation_fails_before_transport);
    RUN_TEST(sdk_v1_structured_error_decodes_all_supported_metadata);
    RUN_TEST(sdk_v1_plain_connect_error_remains_useful);
    RUN_TEST(sdk_v1_stream_end_error_wrapper_is_decoded);
    RUN_TEST(sdk_v1_malformed_detail_does_not_hide_connect_error);
    RUN_TEST(sdk_v1_partially_decodable_detail_is_transactional);
    RUN_TEST(sdk_v1_stream_requires_one_valid_end_stream_envelope);
}
