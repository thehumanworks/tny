/* Deterministic HTTP boundary for owner lifetime/fault tests. Backend fixtures
 * separately exercise the real C scheduler and real loopback transport. */
#include "backends/openai/request_owner.h"
#include "json/ownership.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>

static_assert(!std::is_copy_constructible_v<tny::mutable_document>);
static int wipes, opens, closes, sends, live_connections;
static const char *first_body, *first_auth, *first_path, *first_addon;
static bool resend_matches;
static bool fail_write;

[[maybe_unused]] static void observed_zero(void *p, size_t n) {
    if (n == std::strlen("Authorization: Bearer fixture-secret") + 1 &&
        !std::strcmp(static_cast<char *>(p), "Authorization: Bearer fixture-secret"))
        ++wipes;
    secure_zero(p, n);
    for (size_t i = 0; i < n; ++i)
        if (static_cast<unsigned char *>(p)[i]) std::abort();
}
#define secure_zero observed_zero
#include "backends/openai/request_owner.cpp"
#undef secure_zero

struct http_conn {
    int serial;
};
extern "C" http_conn *http_open(const char *, char *, size_t) {
    auto *conn = static_cast<http_conn *>(tny_alloc_malloc(sizeof(http_conn)));
    if (conn) {
        conn->serial = ++opens;
        ++live_connections;
    }
    return conn;
}
extern "C" void http_close(http_conn *conn) {
    if (!conn) return;
    ++closes;
    --live_connections;
    std::free(conn);
}
extern "C" int http_request(http_conn *, const char *method, const char *path, const char **headers,
                            const char *body, size_t len) {
    ++sends;
    if (std::strcmp(method, "POST") || len != std::strlen(body)) std::abort();
    if (sends == 1) {
        first_body = body;
        first_auth = headers[2];
        first_addon = headers[4];
        first_path = path;
    } else {
        resend_matches = body == first_body && headers[2] == first_auth && path == first_path &&
                         headers[4] == first_addon && !std::strcmp(body, "{\"input\":[]}") &&
                         !std::strcmp(headers[2], "Authorization: Bearer fixture-secret") &&
                         !std::strcmp(headers[3], "X-Test: borrowed") &&
                         !std::strcmp(headers[4], "x-opencode-session: fixture-session") &&
                         !std::strcmp(headers[5], "session-id: fixture-session") &&
                         !std::strcmp(headers[6], "thread-id: fixture-thread") &&
                         !std::strcmp(headers[7], "x-codex-turn-state: fixture-state") &&
                         !headers[8] &&
                         !std::strcmp(path, "/long-prefix-for-path-allocation/responses");
    }
    return fail_write ? -1 : 0;
}

#define CHECK(x)                                                                       \
    do {                                                                               \
        if (!(x)) {                                                                    \
            std::fprintf(stderr, "native request ownership failed at %d\n", __LINE__); \
            return 1;                                                                  \
        }                                                                              \
    } while (false)

static void fault_at(size_t index) {
    char number[32];
    std::snprintf(number, sizeof number, "%zu", index);
    setenv("TNY_TEST_ALLOC_SCOPE", "native-request", 1);
    setenv("TNY_TEST_ALLOC_FAIL_AT", number, 1);
    tny_alloc_scope_begin("native-request");
}

int main() {
    unsetenv("TNY_PROVIDER_EXTRAS");
    char extra[] = "X-Test: borrowed";
    char *extras[] = {extra, nullptr};
    oa_request_options options{"/long-prefix-for-path-allocation",
                               "/responses",
                               "Authorization",
                               "Bearer ",
                               "fixture-secret",
                               "opencode",
                               "http://localhost",
                               "fixture-session",
                               extras,
                               "session-id: fixture-session",
                               "thread-id: fixture-thread",
                               "x-codex-turn-state: fixture-state"};
    size_t baseline_live = tny_alloc_test_owned_live();
    size_t count = 0;
    for (size_t index = 0; index <= count; ++index) {
        wipes = sends = 0;
        fault_at(index);
        oa_connection_owner *connection = oa_connection_new();
        oa_request_owner *request = nullptr;
        int rc = -2;
        if (connection && oa_connection_open(connection, "http://localhost", nullptr, 0) == 0) {
            request = oa_request_new();
            if (request) {
                for (int slot = 0; slot < OA_BUILD_BUFFER_COUNT; ++slot)
                    buf_appends(oa_request_buffer(request, static_cast<oa_build_buffer>(slot)),
                                "owned builder scratch");
                for (int slot = 0; slot < OA_BUILD_STRING_COUNT; ++slot)
                    oa_request_take_string(request, static_cast<oa_build_string>(slot),
                                           tny_alloc_strdup("owned serialized scratch"));
                /* Real yyjson owner, including partial setup and borrowed-child
                 * lifetime through a connection drop and replacement. */
                auto *view = oa_request_take_view(request, yyjson_mut_doc_new(jallocator()));
                if (view) {
                    auto *root = yyjson_mut_arr(view);
                    yyjson_mut_doc_set_root(view, root);
                    if (root) {
                        char *body = tny_alloc_strdup("{\"input\":[]}");
                        rc = oa_request_prepare(request, body, &options);
                    }
                }
            }
        }
        if (!index) {
            CHECK(rc == 0);
            count = tny_alloc_test_scope_count();
            CHECK(count > 5);
            fail_write = true;
            CHECK(oa_request_send(request, connection) == -1);
            CHECK(oa_connection_open(connection, "http://localhost", nullptr, 0) == 0);
            CHECK(live_connections == 1);
            CHECK(!request->view);
            fail_write = false;
            CHECK(oa_request_send(request, connection) == 0);
            CHECK(resend_matches);
        } else {
            CHECK(tny_alloc_test_scope_injected());
            CHECK(rc == -2 && sends == 0);
        }
        if (request && request->attempted)
            CHECK(oa_request_prepare(request, nullptr, &options) == -2);
        bool secret_built = request && request->auth.bytes;
        size_t before = tny_alloc_test_scope_count();
        tny_alloc_provider_failed();
        tny_alloc_settlement_begin();
        if (connection) {
            oa_connection_drop(connection);
            oa_connection_drop(connection);
            CHECK(!oa_connection_get(connection));
        }
        CHECK(live_connections == 0);
        oa_connection_free(&connection);
        oa_connection_free(&connection);
        /* Connection release must not revoke the active request's borrows. */
        if (secret_built)
            CHECK(!std::strcmp(request->auth.bytes.get(), "Authorization: Bearer fixture-secret"));
        oa_request_free(&request);
        oa_request_free(&request);
        tny_alloc_settlement_end();
        CHECK(tny_alloc_test_scope_count() == before);
        CHECK(tny_alloc_test_settlement_allocations() == 0);
        CHECK(wipes == (secret_built ? 1 : 0));
        CHECK(live_connections == 0 && opens == closes);
        CHECK(tny_alloc_test_owned_live() == baseline_live);
    }
    /* Once attempted, success and failed preparation both reject reuse. */
    for (int failed = 0; failed < 2; ++failed) {
        fault_at(0);
        auto *request = oa_request_new();
        auto *connection = oa_connection_new();
        CHECK(request && connection);
        CHECK(oa_request_send(request, connection) == -2);
        char *body = tny_alloc_strdup("{}");
        if (failed) fault_at(1);
        wipes = 0;
        CHECK(oa_request_prepare(request, body, &options) == (failed ? -2 : 0));
        fault_at(0);
        CHECK(oa_request_prepare(request, tny_alloc_strdup("second"), &options) == -2);
        CHECK(!std::strcmp(request->body.get(), "{}"));
        oa_request_free(&request);
        CHECK(wipes == (failed ? 0 : 1));
        oa_connection_free(&connection);
    }
    fault_at(0);
    {
        auto *request = oa_request_new();
        options.auth_name = options.auth_prefix = nullptr;
        CHECK(oa_request_prepare(request, tny_alloc_strdup("{}"), &options) == 0);
        CHECK(!std::strcmp(request->auth.bytes.get(), "Authorization: Bearer fixture-secret"));
        oa_request_free(&request);
        options.auth_name = "Authorization";
        options.auth_prefix = "Bearer ";
    }
    /* A failed stale-connection reopen consumes the old connection, but
     * cannot revoke the still-active body/auth/document owners. */
    fault_at(0);
    auto *connection = oa_connection_new();
    auto *request = oa_request_new();
    CHECK(connection && request);
    CHECK(oa_connection_open(connection, "http://localhost", nullptr, 0) == 0);
    CHECK(oa_request_prepare(request, tny_alloc_strdup("{\"input\":[]}"), &options) == 0);
    fault_at(1);
    CHECK(oa_connection_open(connection, "http://localhost", nullptr, 0) == -2);
    CHECK(!oa_connection_get(connection) && live_connections == 0);
    CHECK(!std::strcmp(request->body.get(), "{\"input\":[]}"));
    size_t before = tny_alloc_test_scope_count();
    tny_alloc_provider_failed();
    tny_alloc_settlement_begin();
    oa_request_free(&request);
    oa_connection_free(&connection);
    tny_alloc_settlement_end();
    CHECK(tny_alloc_test_scope_count() == before);
    CHECK(tny_alloc_test_settlement_allocations() == 0);
    /* Repeated successful requests after the exhaustive partial-setup sweep. */
    fault_at(0);
    for (int i = 0; i < 32; ++i) {
        connection = oa_connection_new();
        request = oa_request_new();
        CHECK(connection && request);
        CHECK(oa_connection_open(connection, "http://localhost", nullptr, 0) == 0);
        CHECK(oa_request_prepare(request, tny_alloc_strdup("{\"input\":[]}"), &options) == 0);
        sends = 0;
        CHECK(oa_request_send(request, connection) == 0);
        int before_wipe = wipes;
        oa_request_free(&request);
        CHECK(wipes == before_wipe + 1);
        oa_connection_free(&connection);
    }
    CHECK(live_connections == 0 && opens == closes);
    CHECK(tny_alloc_test_owned_live() == baseline_live);
    std::printf("native request ownership: swept %zu allocation indices; stable resend, wipe, "
                "reset passed\n",
                count);
    return 0;
}
