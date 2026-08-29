/* test_http.c — the REST router (pure, no sockets) and auth. Sessions
 * spawn a real pty running sleep so create/list/get/delete are honest. */
#include "greatest.h"
#include "api/http.h"
#include "session/session.h"
#include "util/tt.h"

#include <stdlib.h>
#include <string.h>

static const char *body_of(const tt_buf *out) {
    const char *p = strstr(out->data, "\r\n\r\n");
    return p ? p + 4 : "";
}

static int status_of(const tt_buf *out) {
    int code = 0;
    sscanf(out->data, "HTTP/1.1 %d", &code);
    return code;
}

TEST health_and_unknown_routes(void) {
    tt_registry reg;
    tt_registry_init(&reg, 0);
    tt_api api = {&reg, NULL, "test"};
    tt_buf out;
    tt_buf_init(&out);
    tt_api_route(&api, "GET", "/v1/health", NULL, 0, &out);
    ASSERT_EQ(200, status_of(&out));
    ASSERT(strstr(body_of(&out), "\"ok\":true") != NULL);
    ASSERT(strstr(body_of(&out), "\"sessions\":0") != NULL);
    tt_buf_free(&out);

    tt_buf_init(&out);
    tt_api_route(&api, "GET", "/nope", NULL, 0, &out);
    ASSERT_EQ(404, status_of(&out));
    tt_buf_free(&out);

    tt_buf_init(&out);
    tt_api_route(&api, "DELETE", "/v1/health", NULL, 0, &out);
    ASSERT_EQ(405, status_of(&out));
    tt_buf_free(&out);
    tt_registry_free(&reg);
    PASS();
}

TEST bearer_auth_is_enforced(void) {
    tt_registry reg;
    tt_registry_init(&reg, 0);
    tt_api open_api = {&reg, NULL, "test"};
    tt_api locked = {&reg, "s3cret", "test"};
    ASSERT(tt_api_auth_ok(&open_api, NULL));
    ASSERT_FALSE(tt_api_auth_ok(&locked, NULL));
    ASSERT_FALSE(tt_api_auth_ok(&locked, "Bearer wrong"));
    ASSERT_FALSE(tt_api_auth_ok(&locked, "Basic s3cret"));
    ASSERT(tt_api_auth_ok(&locked, "Bearer s3cret"));
    tt_registry_free(&reg);
    PASS();
}

TEST session_lifecycle_over_the_api(void) {
    tt_registry reg;
    tt_registry_init(&reg, 100);
    tt_api api = {&reg, NULL, "test"};
    tt_buf out;

    tt_buf_init(&out);
    const char *create = "{\"cmd\":[\"sleep\",\"30\"],\"cols\":40,\"rows\":10}";
    tt_api_route(&api, "POST", "/v1/sessions", create, strlen(create), &out);
    ASSERT_EQ(201, status_of(&out));
    ASSERT(strstr(body_of(&out), "\"cols\":40") != NULL);
    ASSERT(strstr(body_of(&out), "\"alive\":true") != NULL);
    /* pull the id out of the response */
    const char *idp = strstr(body_of(&out), "\"id\":\"");
    ASSERT(idp != NULL);
    char id[16] = {0};
    memcpy(id, idp + 6, TT_SESSION_ID_LEN);
    tt_buf_free(&out);
    ASSERT_EQ(1, reg.count);

    char path[64];
    snprintf(path, sizeof path, "/v1/sessions/%s", id);
    tt_buf_init(&out);
    tt_api_route(&api, "GET", path, NULL, 0, &out);
    ASSERT_EQ(200, status_of(&out));
    tt_buf_free(&out);

    /* screen: inject bytes into the vt directly and read them back */
    tt_session *s = tt_session_find(&reg, id);
    ASSERT(s != NULL);
    vt_feed(s->term, "\x1b[1mhello\x1b[0m world", 19);
    snprintf(path, sizeof path, "/v1/sessions/%s/screen", id);
    tt_buf_init(&out);
    tt_api_route(&api, "GET", path, NULL, 0, &out);
    ASSERT_EQ(200, status_of(&out));
    ASSERT(strstr(out.data, "text/plain") != NULL);
    ASSERT(strncmp(body_of(&out), "hello world\n", 12) == 0);
    tt_buf_free(&out);

    snprintf(path, sizeof path, "/v1/sessions/%s/screen?format=json", id);
    tt_buf_init(&out);
    tt_api_route(&api, "GET", path, NULL, 0, &out);
    ASSERT_EQ(200, status_of(&out));
    ASSERT(strstr(body_of(&out), "\"text\":\"hello world\"") != NULL);
    ASSERT(strstr(body_of(&out), "\"attrs\":[\"bold\"]") != NULL);
    ASSERT(strstr(body_of(&out), "\"cursor\"") != NULL);
    tt_buf_free(&out);

    /* input reaches the pty */
    snprintf(path, sizeof path, "/v1/sessions/%s/input", id);
    tt_buf_init(&out);
    const char *input = "{\"text\":\"hi\\r\"}";
    tt_api_route(&api, "POST", path, input, strlen(input), &out);
    ASSERT_EQ(200, status_of(&out));
    ASSERT(strstr(body_of(&out), "\"written\":3") != NULL);
    tt_buf_free(&out);

    /* base64 input too */
    tt_buf_init(&out);
    const char *input64 = "{\"base64\":\"aGk=\"}";
    tt_api_route(&api, "POST", path, input64, strlen(input64), &out);
    ASSERT_EQ(200, status_of(&out));
    ASSERT(strstr(body_of(&out), "\"written\":2") != NULL);
    tt_buf_free(&out);

    /* resize is visible in the vt */
    snprintf(path, sizeof path, "/v1/sessions/%s/resize", id);
    tt_buf_init(&out);
    const char *rs = "{\"cols\":100,\"rows\":30}";
    tt_api_route(&api, "POST", path, rs, strlen(rs), &out);
    ASSERT_EQ(200, status_of(&out));
    ASSERT_EQ(100, vt_cols(s->term));
    ASSERT_EQ(30, vt_rows(s->term));
    tt_buf_free(&out);

    snprintf(path, sizeof path, "/v1/sessions/%s", id);
    tt_buf_init(&out);
    tt_api_route(&api, "DELETE", path, NULL, 0, &out);
    ASSERT_EQ(200, status_of(&out));
    ASSERT_EQ(0, reg.count);
    tt_buf_free(&out);

    tt_buf_init(&out);
    tt_api_route(&api, "GET", path, NULL, 0, &out);
    ASSERT_EQ(404, status_of(&out));
    tt_buf_free(&out);
    tt_registry_free(&reg);
    PASS();
}

TEST bad_bodies_are_400(void) {
    tt_registry reg;
    tt_registry_init(&reg, 0);
    tt_api api = {&reg, NULL, "test"};
    tt_buf out;
    tt_buf_init(&out);
    tt_api_route(&api, "POST", "/v1/sessions", "{nope", 5, &out);
    ASSERT_EQ(400, status_of(&out));
    tt_buf_free(&out);
    tt_buf_init(&out);
    tt_api_route(&api, "POST", "/v1/sessions", "{\"cols\":0}", 10, &out);
    ASSERT_EQ(400, status_of(&out));
    tt_buf_free(&out);
    tt_registry_free(&reg);
    PASS();
}

TEST const_eq_ignores_length_leaks(void) {
    ASSERT(tt_const_eq("abc", "abc"));
    ASSERT_FALSE(tt_const_eq("abc", "abd"));
    ASSERT_FALSE(tt_const_eq("abc", "ab"));
    ASSERT_FALSE(tt_const_eq("", "x"));
    ASSERT(tt_const_eq("", ""));
    PASS();
}

SUITE(http_suite) {
    RUN_TEST(health_and_unknown_routes);
    RUN_TEST(bearer_auth_is_enforced);
    RUN_TEST(session_lifecycle_over_the_api);
    RUN_TEST(bad_bodies_are_400);
    RUN_TEST(const_eq_ignores_length_leaks);
}
