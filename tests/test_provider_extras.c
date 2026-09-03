/* test_provider_extras.c — per-provider request add-ons (docs/adr/0067). */
#include "greatest.h"
#include "core/provider_extras.h"

#include <stdlib.h>
#include <string.h>

static int extras(const char *name, const char *url, const char *sid, char **out, int cap) {
    tny_request_scope s = {name, url, sid};
    return tny_provider_extras_headers(&s, out, cap);
}

TEST opencode_go_sends_session_header(void) {
    unsetenv("TNY_PROVIDER_EXTRAS");
    char *h[4];
    /* the documented Go base URL */
    int n = extras("opencodego", "https://opencode.ai/zen/go/v1", "sess-abc", h, 4);
    ASSERT_EQ(1, n);
    ASSERT_STR_EQ("x-opencode-session: sess-abc", h[0]);
    tny_provider_extras_free(h, n);
    /* matched by host alone: a settings profile under any name */
    n = extras("work", "https://api.opencode.ai/v1", "s2", h, 4);
    ASSERT_EQ(1, n);
    ASSERT_STR_EQ("x-opencode-session: s2", h[0]);
    tny_provider_extras_free(h, n);
    /* matched by env-profile name alone (OPENCODEGO_BASE_URL → a gateway) */
    n = extras("opencodego", "http://127.0.0.1:8080/v1", "s3", h, 4);
    ASSERT_EQ(1, n);
    ASSERT_STR_EQ("x-opencode-session: s3", h[0]);
    tny_provider_extras_free(h, n);
    PASS();
}

TEST opencode_go_needs_a_conversation(void) {
    unsetenv("TNY_PROVIDER_EXTRAS");
    char *h[4];
    tny_request_scope s = {"opencodego", "https://opencode.ai/zen/go/v1", NULL};
    ASSERT_STR_EQ("opencode-go", tny_provider_extras_match(&s));
    ASSERT_EQ(0, tny_provider_extras_headers(&s, h, 4)); /* no id, no header */
    ASSERT_EQ(0, extras("opencodego", "https://opencode.ai/zen/go/v1", "", h, 4));
    ASSERT_EQ(0, extras("opencodego", "https://opencode.ai/zen/go/v1", "x", h, 0));
    PASS();
}

TEST other_providers_are_untouched(void) {
    unsetenv("TNY_PROVIDER_EXTRAS");
    char *h[4];
    ASSERT_EQ(0, extras(NULL, "https://api.openai.com/v1", "s", h, 4));
    ASSERT_EQ(0, extras("openrouter", "https://openrouter.ai/api/v1", "s", h, 4));
    /* look-alike hosts must not match a suffix without a dot boundary */
    ASSERT_EQ(0, extras("x", "https://notopencode.ai/v1", "s", h, 4));
    ASSERT_EQ(0, extras("x", "https://opencode.ai.evil.example/v1", "s", h, 4));
    ASSERT_EQ(0, extras("x", "not a url", "s", h, 4));
    tny_request_scope s = {"openai", "https://api.openai.com/v1", "s"};
    ASSERT_EQ(NULL, tny_provider_extras_match(&s));
    ASSERT_EQ(0, tny_provider_extras_headers(NULL, h, 4));
    PASS();
}

TEST kill_switch_disables_every_extra(void) {
    char *h[4];
    setenv("TNY_PROVIDER_EXTRAS", "0", 1);
    ASSERT_EQ(0, extras("opencodego", "https://opencode.ai/zen/go/v1", "s", h, 4));
    tny_request_scope s = {"opencodego", "https://opencode.ai/zen/go/v1", "s"};
    ASSERT_EQ(NULL, tny_provider_extras_match(&s));
    setenv("TNY_PROVIDER_EXTRAS", "off", 1);
    ASSERT_EQ(0, extras("opencodego", "https://opencode.ai/zen/go/v1", "s", h, 4));
    setenv("TNY_PROVIDER_EXTRAS", "1", 1);
    int n = extras("opencodego", "https://opencode.ai/zen/go/v1", "s", h, 4);
    ASSERT_EQ(1, n);
    tny_provider_extras_free(h, n);
    unsetenv("TNY_PROVIDER_EXTRAS");
    PASS();
}

SUITE(provider_extras_suite) {
    RUN_TEST(opencode_go_sends_session_header);
    RUN_TEST(opencode_go_needs_a_conversation);
    RUN_TEST(other_providers_are_untouched);
    RUN_TEST(kill_switch_disables_every_extra);
}
