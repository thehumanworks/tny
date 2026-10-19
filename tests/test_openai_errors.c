/* Pure provider error policy: no backend, runtime, transport or credentials. */
#include "greatest.h"
#include "backends/openai/errors.h"
#include <string.h>

TEST error_token_keeps_only_identifiers(void) {
    char t[33];
    oa_error_token(t, sizeof t, "invalid_request_error");
    ASSERT_STR_EQ("invalid_request_error", t);
    oa_error_token(t, sizeof t, "Rate-Limit Exceeded");
    ASSERT_STR_EQ("rate_limit_exceeded", t);
    oa_error_token(t, sizeof t, "server_error.upstream");
    ASSERT_STR_EQ("server_error_upstream", t);
    /* free text, punctuation, or anything key-shaped is dropped whole */
    oa_error_token(t, sizeof t, "no tool output found for call_1: {bad}");
    ASSERT_STR_EQ("", t);
    oa_error_token(t, sizeof t, "sk-proj-ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    ASSERT_STR_EQ("", t);
    oa_error_token(t, sizeof t, "a_category_name_that_is_far_too_long_to_be_one");
    ASSERT_STR_EQ("", t);
    oa_error_token(t, sizeof t, "");
    ASSERT_STR_EQ("", t);
    oa_error_token(t, sizeof t, NULL);
    ASSERT_STR_EQ("", t);
    PASS();
}

TEST retryable_statuses_and_permanent_tokens(void) {
    ASSERT(oa_status_is_retryable(429));
    ASSERT(oa_status_is_retryable(408));
    ASSERT(oa_status_is_retryable(500));
    ASSERT(oa_status_is_retryable(502));
    ASSERT(oa_status_is_retryable(529));
    ASSERT_FALSE(oa_status_is_retryable(400));
    ASSERT_FALSE(oa_status_is_retryable(401));
    ASSERT_FALSE(oa_status_is_retryable(403));
    ASSERT_FALSE(oa_status_is_retryable(404));
    ASSERT_FALSE(oa_status_is_retryable(422));
    ASSERT(oa_error_token_is_permanent("invalid_request_error"));
    ASSERT(oa_error_token_is_permanent("context_length_exceeded"));
    ASSERT(oa_error_token_is_permanent("insufficient_quota"));
    ASSERT(oa_error_token_is_permanent("model_not_found"));
    ASSERT_FALSE(oa_error_token_is_permanent("server_error"));
    ASSERT_FALSE(oa_error_token_is_permanent("overloaded_error"));
    ASSERT_FALSE(oa_error_token_is_permanent(""));
    PASS();
}

TEST plan_error_codes_classify(void) {
    ASSERT_EQ(OA_PLAN_ERROR_USAGE_LIMIT,
              oa_plan_error_of("subscription_sharing_usage_limit_exceeded"));
    ASSERT_EQ(OA_PLAN_ERROR_UNAVAILABLE,
              oa_plan_error_of("subscription_sharing_usage_unavailable"));
    ASSERT_EQ(OA_PLAN_ERROR_UNAVAILABLE, oa_plan_error_of("subscription_sharing_user_unavailable"));
    ASSERT_EQ(OA_PLAN_ERROR_NOT_ELIGIBLE,
              oa_plan_error_of("subscription_sharing_user_not_eligible"));
    ASSERT_EQ(OA_PLAN_ERROR_ROUTE, oa_plan_error_of("subscription_sharing_route_not_supported"));
    ASSERT_EQ(OA_PLAN_ERROR_GRANT, oa_plan_error_of("chatpass_v2_scope_not_authorized"));
    ASSERT_EQ(OA_PLAN_ERROR_GRANT, oa_plan_error_of("chatpass_v2_invalid_authorization_context"));
    ASSERT_EQ(OA_PLAN_ERROR_INVALID_USER, oa_plan_error_of("subscription_sharing_invalid_user"));
    ASSERT_EQ(OA_PLAN_ERROR_UNSUPPORTED,
              oa_plan_error_of("subscription_sharing_unsupported_capability"));
    /* exact codes only: no prefixes, no case folding */
    ASSERT_EQ(OA_PLAN_ERROR_NONE, oa_plan_error_of("subscription_sharing_usage_limit"));
    ASSERT_EQ(OA_PLAN_ERROR_NONE, oa_plan_error_of("SUBSCRIPTION_SHARING_INVALID_USER"));
    ASSERT_EQ(OA_PLAN_ERROR_NONE, oa_plan_error_of("rate_limit_exceeded"));
    ASSERT_EQ(OA_PLAN_ERROR_NONE, oa_plan_error_of(NULL));
    /* only a temporary unavailability is worth retrying; a usage limit
     * waits for the user, not a backoff */
    ASSERT(oa_plan_error_retryable(OA_PLAN_ERROR_UNAVAILABLE));
    ASSERT_FALSE(oa_plan_error_retryable(OA_PLAN_ERROR_USAGE_LIMIT));
    ASSERT_FALSE(oa_plan_error_retryable(OA_PLAN_ERROR_INVALID_USER));
    ASSERT_FALSE(oa_plan_error_retryable(OA_PLAN_ERROR_NONE));
    PASS();
}

TEST plan_error_param_is_sanitized(void) {
    char p[16];
    oa_error_param(p, sizeof p, "tools[0].type");
    ASSERT_STR_EQ("tools[0].type", p);
    oa_error_param(p, sizeof p, "max-output_1");
    ASSERT_STR_EQ("max-output_1", p);
    oa_error_param(p, sizeof p, "a\x1b[2Jb"); /* terminal escapes never print */
    ASSERT_STR_EQ("", p);
    oa_error_param(p, sizeof p, "a b");
    ASSERT_STR_EQ("", p);
    oa_error_param(p, sizeof p, "`rm`");
    ASSERT_STR_EQ("", p);
    oa_error_param(p, sizeof p, "0123456789abcdef"); /* does not fit: dropped, not cut */
    ASSERT_STR_EQ("", p);
    oa_error_param(p, sizeof p, NULL);
    ASSERT_STR_EQ("", p);
    PASS();
}

TEST error_classification_preserves_precedence(void) {
    static const struct {
        const char *json;
        int status;
        const char *token;
        int code;
        bool retryable;
    } cases[] = {
        {"{\"type\":\"invalid_request_error\"}", 500, "invalid_request_error", 0, true},
        {"{\"type\":\"server_error\",\"code\":503}", 400, "server_error", 503, false},
        {"{\"code\":503}", 0, "", 503, true},
        {"{\"code\":400}", 0, "", 400, false},
        {"{\"code\":0,\"status\":429}", 0, "", 429, true},
        {"{\"status\":503.5}", 0, "", 503, true},
        {"{\"type\":\"invalid_request_error\"}", 0, "invalid_request_error", 0, false},
        {"{\"type\":\"provider: secret\",\"code\":\"server_error\"}", 0, "server_error", 0, true},
        {"{\"type\":\"\",\"code\":\"unsupported_model\"}", 0, "unsupported_model", 0, false},
        {"{\"code\":\"model_not_found\"}", 0, "model_not_found", 0, false},
        {"\"request body contains secret\"", 429, "", 0, true},
        {"null", 0, "", 0, true},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        yyjson_doc *doc = jparse(cases[i].json, strlen(cases[i].json));
        ASSERT(doc);
        char detail[400] = "";
        oa_error_info info;
        oa_error_classify(yyjson_doc_get_root(doc), cases[i].status, false, detail, sizeof detail,
                          &info);
        yyjson_doc_free(doc);
        ASSERT_EQ(cases[i].status, info.status);
        ASSERT_STR_EQ(cases[i].token, info.token);
        ASSERT_EQ(cases[i].code, info.code);
        ASSERT_EQ(cases[i].retryable, info.retryable);
        ASSERT_STR_EQ("", detail);
        ASSERT_EQ(0, info.retry_after_ms);
    }
    PASS();
}

TEST error_plan_policy_overrides_status_and_owns_code(void) {
    static const struct {
        const char *code;
        int status;
        oa_plan_error plan;
        bool retryable;
        const char *diagnostic;
    } cases[] = {
        {"subscription_sharing_usage_limit_exceeded", 429, OA_PLAN_ERROR_USAGE_LIMIT, false,
         "https://chatgpt.com/settings/usage"},
        {"subscription_sharing_usage_unavailable", 400, OA_PLAN_ERROR_UNAVAILABLE, true,
         "temporarily unavailable"},
        {"subscription_sharing_user_unavailable", 403, OA_PLAN_ERROR_UNAVAILABLE, true,
         "temporarily unavailable"},
        {"subscription_sharing_user_not_eligible", 503, OA_PLAN_ERROR_NOT_ELIGIBLE, false,
         "use an OpenAI API key instead"},
        {"subscription_sharing_route_not_supported", 0, OA_PLAN_ERROR_ROUTE, false,
         "does not serve this endpoint"},
        {"chatpass_v2_scope_not_authorized", 0, OA_PLAN_ERROR_GRANT, false,
         "allow ChatGPT plan use"},
        {"chatpass_v2_invalid_authorization_context", 500, OA_PLAN_ERROR_GRANT, false,
         "allow ChatGPT plan use"},
        {"subscription_sharing_invalid_user", 502, OA_PLAN_ERROR_INVALID_USER, false,
         "sign in again"},
        {"subscription_sharing_unsupported_capability", 500, OA_PLAN_ERROR_UNSUPPORTED, false,
         "does not support `tools[0].type`"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char payload[256];
        snprintf(payload, sizeof payload, "{\"code\":\"%s\",\"param\":\"tools[0].type\"}",
                 cases[i].code);
        yyjson_doc *doc = jparse(payload, strlen(payload));
        ASSERT(doc);
        char detail[400] = "";
        oa_error_info info;
        oa_error_classify(yyjson_doc_get_root(doc), cases[i].status, false, detail, sizeof detail,
                          &info);
        yyjson_doc_free(doc);
        /* No JSON borrow survives classification, including long plan codes. */
        ASSERT_EQ(cases[i].plan, info.plan);
        ASSERT_EQ(cases[i].retryable, info.retryable);
        ASSERT_STR_EQ(cases[i].code, info.plan_code);
        char text[512];
        oa_error_format(&info, false, false, NULL, text, sizeof text);
        ASSERT(strstr(text, cases[i].code));
        ASSERT(strstr(text, cases[i].diagnostic));
    }
    PASS();
}

TEST error_detail_is_opt_in_bounded_and_control_safe(void) {
    const char *payload = "{\"message\":\"secret\\nvalue\\t\\u001b[2J\"}";
    yyjson_doc *doc = jparse(payload, strlen(payload));
    ASSERT(doc);
    char detail[400] = "previous";
    oa_error_info info;
    oa_error_classify(yyjson_doc_get_root(doc), 400, false, detail, sizeof detail, &info);
    ASSERT_STR_EQ("", detail);
    oa_error_classify(yyjson_doc_get_root(doc), 400, true, detail, sizeof detail, &info);
    ASSERT_STR_EQ("secret value  [2J", detail);
    yyjson_doc_free(doc);
    char text[512];
    oa_error_format(&info, false, false, NULL, text, sizeof text);
    ASSERT_STR_EQ("provider rejected the request (HTTP 400)", text);
    oa_error_format(&info, false, false, detail, text, sizeof text);
    ASSERT_STR_EQ("provider rejected the request (HTTP 400): secret value  [2J", text);
    /* No error node leaves the caller's existing detail intact. */
    oa_error_classify(NULL, 503, true, detail, sizeof detail, &info);
    ASSERT_STR_EQ("secret value  [2J", detail);
    PASS();
}

TEST error_detail_truncation_preserves_utf8_boundaries(void) {
    const char *payload = "\"a\\u20acb\"";
    yyjson_doc *doc = jparse(payload, strlen(payload));
    ASSERT(doc);
    for (size_t cap = 1; cap <= 6; cap++) {
        char detail[8];
        memset(detail, 'X', sizeof detail);
        oa_error_info info;
        oa_error_classify(yyjson_doc_get_root(doc), 500, true, detail, cap, &info);
        ASSERT_EQ('X', detail[cap]);
        /* Preserve the conservative existing truncation: a trailing multibyte
         * character is dropped whenever more input remained. */
        ASSERT_STR_EQ(cap == 1  ? ""
                      : cap < 6 ? "a"
                                : "a\xe2\x82\xac"
                                  "b",
                      detail);
    }
    yyjson_doc_free(doc);
    PASS();
}

TEST error_format_routes_only_matching_auth_and_proxy_failures(void) {
    static const struct {
        int status;
        bool plan;
        bool grok;
        const char *text;
    } cases[] = {
        {401, false, false, "authentication failed (HTTP 401): check the API key"},
        {401, true, false,
         "ChatGPT sign-in was not accepted (HTTP 401): sign in again with "
         "`tny --provider openai login`"},
        {403, false, false,
         "provider refused the request (HTTP 403): the key may lack access to "
         "this model, or a gateway blocked the request"},
        {403, true, false,
         "ChatGPT plan request refused (HTTP 403): a policy or permission check, "
         "such as the serving region, blocked it"},
        {426, false, false, "provider rejected the request (HTTP 426)"},
        {426, false, true,
         "Grok requires a newer client version (HTTP 426): update tny or set "
         "TNY_GROK_CLIENT_VERSION to a supported Grok Build version"},
        {429, false, true, "provider rate limit (HTTP 429)"},
        {503, true, true, "provider error (HTTP 503)"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        oa_error_info info = {.status = cases[i].status};
        char text[512];
        oa_error_format(&info, cases[i].plan, cases[i].grok, NULL, text, sizeof text);
        ASSERT_STR_EQ(cases[i].text, text);
    }
    oa_error_info stream = {.code = 502, .token = "server_error"};
    char text[512];
    oa_error_format(&stream, false, false, NULL, text, sizeof text);
    ASSERT_STR_EQ("provider stream reported an error (code 502, server_error)", text);
    stream.code = 0;
    oa_error_format(&stream, false, false, NULL, text, sizeof text);
    ASSERT_STR_EQ("provider stream reported an error, server_error", text);
    PASS();
}

TEST error_format_and_params_stay_bounded(void) {
    const char *payload = "{\"code\":\"subscription_sharing_unsupported_capability\","
                          "\"param\":\"secret: value\"}";
    yyjson_doc *doc = jparse(payload, strlen(payload));
    ASSERT(doc);
    oa_error_info info;
    char detail[400] = "";
    oa_error_classify(yyjson_doc_get_root(doc), 400, false, detail, sizeof detail, &info);
    yyjson_doc_free(doc);
    ASSERT_STR_EQ("", info.param);
    char text[512];
    oa_error_format(&info, true, false, NULL, text, sizeof text);
    ASSERT(strstr(text, "does not support part of this request"));
    ASSERT_FALSE(strstr(text, "secret"));
    for (size_t cap = 1; cap < 32; cap++) {
        memset(text, 'X', sizeof text);
        oa_error_format(&info, true, false, "debug", text, cap);
        ASSERT_EQ('X', text[cap]);
        ASSERT_EQ(0, text[cap - 1]);
    }
    PASS();
}

TEST retry_after_delta_seconds_stays_capped(void) {
    ASSERT_EQ(0, oa_error_retry_after_ms(NULL));
    ASSERT_EQ(0, oa_error_retry_after_ms(""));
    ASSERT_EQ(0, oa_error_retry_after_ms("0"));
    ASSERT_EQ(0, oa_error_retry_after_ms("-1"));
    ASSERT_EQ(0, oa_error_retry_after_ms("Mon, 19 Oct 2026 12:00:00 GMT"));
    ASSERT_EQ(1000, oa_error_retry_after_ms("1"));
    ASSERT_EQ(2000, oa_error_retry_after_ms("   2"));
    ASSERT_EQ(2000, oa_error_retry_after_ms("2 trailing"));
    ASSERT_EQ(29000, oa_error_retry_after_ms("29"));
    ASSERT_EQ(OA_RETRY_MAX_MS, oa_error_retry_after_ms("30"));
    ASSERT_EQ(OA_RETRY_MAX_MS, oa_error_retry_after_ms("9999999999999999999999999999"));
    PASS();
}

SUITE(openai_errors_suite) {
    RUN_TEST(error_token_keeps_only_identifiers);
    RUN_TEST(retryable_statuses_and_permanent_tokens);
    RUN_TEST(plan_error_codes_classify);
    RUN_TEST(plan_error_param_is_sanitized);
    RUN_TEST(error_classification_preserves_precedence);
    RUN_TEST(error_plan_policy_overrides_status_and_owns_code);
    RUN_TEST(error_detail_is_opt_in_bounded_and_control_safe);
    RUN_TEST(error_detail_truncation_preserves_utf8_boundaries);
    RUN_TEST(error_format_routes_only_matching_auth_and_proxy_failures);
    RUN_TEST(error_format_and_params_stay_bounded);
    RUN_TEST(retry_after_delta_seconds_stays_capped);
}

#ifdef TNY_OPENAI_ERRORS_STANDALONE
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(openai_errors_suite);
    GREATEST_MAIN_END();
}
#endif
