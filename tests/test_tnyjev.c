#include "greatest.h"
#include "core/tnyjev_internal.h"
#include "json/json.h"
#include <stdlib.h>
#include <string.h>

static tnyjev_request score_request(void) {
    return (tnyjev_request){.kind = TNYJEV_SCORE,
                            .instructions = "Is this urgent?",
                            .state = {TNYJEV_TEXT, "Production is down"}};
}

static const tnyjev_choice routes[] = {{"billing", {TNYJEV_TEXT, "Payments"}},
                                       {"support", {TNYJEV_JSON, "{\"topic\":\"Bugs\"}"}}};
static tnyjev_request choose_request(void) {
    tnyjev_request r = score_request();
    r.kind = TNYJEV_CHOOSE;
    r.instructions = "Which team?";
    r.choices = routes;
    r.choice_count = 2;
    return r;
}

static const char score_response[] =
    "{\"model\":\"jev-1.13.0\",\"answers\":{\"decision\":{\"type\":\"noul\",\"noul\":0.75}},"
    "\"usage\":{\"input_tokens\":307,\"output_tokens\":20}}";

static tnyjev_status decode_answer(const tnyjev_request *r, const char *answer,
                                   tnyjev_result *result) {
    buf_t b = {0};
    buf_appendf(&b,
                "{\"model\":\"jev-latest\",\"answers\":{\"decision\":%s},"
                "\"usage\":{\"input_tokens\":0,\"output_tokens\":1}}",
                answer);
    tnyjev_status rc = tnyjev_decode(r, b.data, b.len, result);
    buf_free(&b);
    return rc;
}

TEST tnyjev_score_is_noul_and_text_is_escaped(void) {
    tnyjev_request r = score_request();
    r.state.data = "\"hello\"\n世界";
    buf_t body = {0};
    ASSERT_EQ(TNYJEV_OK, tnyjev_encode(&r, "jev-latest", &body));
    yyjson_doc *doc = jparse(body.data, body.len);
    ASSERT(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_STR_EQ(r.state.data, jget_str(root, "state"));
    ASSERT_STR_EQ("jev-latest", jget_str(root, "model"));
    yyjson_val *q = jget(jget(root, "questions"), "decision");
    ASSERT_STR_EQ("noul", jget_str(q, "type"));
    ASSERT_STR_EQ(r.instructions, jget_str(q, "instructions"));
    ASSERT(!jget(q, "criteria"));
    yyjson_doc_free(doc);
    buf_free(&body);
    PASS();
}

TEST tnyjev_choose_preserves_structured_values(void) {
    tnyjev_request r = choose_request();
    r.state = (tnyjev_value){TNYJEV_JSON, "[{\"role\":\"user\",\"content\":\"Help\"}]"};
    buf_t body = {0};
    ASSERT_EQ(TNYJEV_OK, tnyjev_encode(&r, "jev-1.13.0", &body));
    yyjson_doc *doc = jparse(body.data, body.len);
    ASSERT(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT(yyjson_is_arr(jget(root, "state")));
    yyjson_val *q = jget(jget(root, "questions"), "decision");
    ASSERT_STR_EQ("choice", jget_str(q, "type"));
    ASSERT_STR_EQ("Payments", jget_str(jget(q, "criteria"), "billing"));
    ASSERT_STR_EQ("Bugs", jget_str(jget(jget(q, "criteria"), "support"), "topic"));
    yyjson_doc_free(doc);
    buf_free(&body);
    PASS();
}

TEST tnyjev_request_rejects_invalid_types_duplicates_and_limits(void) {
    const char *bad[] = {"null", "true", "12", "{} trailing", "{", "", "\xff"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        tnyjev_request r = score_request();
        r.state = (tnyjev_value){TNYJEV_JSON, bad[i]};
        buf_t b = {0};
        ASSERT_EQ(TNYJEV_INVALID, tnyjev_encode(&r, "jev-latest", &b));
        buf_free(&b);
    }
    tnyjev_request r = choose_request();
    tnyjev_choice choices[2] = {routes[0], routes[0]};
    r.choices = choices;
    buf_t b = {0};
    ASSERT_EQ(TNYJEV_INVALID, tnyjev_encode(&r, "jev-latest", &b));
    r.choice_count = TNYJEV_MAX_CHOICES + 1;
    ASSERT_EQ(TNYJEV_INVALID, tnyjev_encode(&r, "jev-latest", &b));
    r.choice_count = 0;
    ASSERT_EQ(TNYJEV_INVALID, tnyjev_encode(&r, "jev-latest", &b));
    r = score_request();
    r.kind = (tnyjev_kind)99;
    ASSERT_EQ(TNYJEV_INVALID, tnyjev_encode(&r, "jev-latest", &b));
    r = score_request();
    char *large = malloc(TNYJEV_MAX_BYTES + 2);
    ASSERT(large);
    memset(large, 'x', TNYJEV_MAX_BYTES + 1);
    large[TNYJEV_MAX_BYTES + 1] = '\0';
    r.state.data = large;
    ASSERT_EQ(TNYJEV_INVALID, tnyjev_encode(&r, "jev-latest", &b));
    free(large);
    buf_free(&b);
    PASS();
}

TEST tnyjev_choice_count_bounds_and_null_descriptions(void) {
    tnyjev_choice choices[TNYJEV_MAX_CHOICES] = {0};
    char keys[TNYJEV_MAX_CHOICES][16];
    for (size_t i = 0; i < TNYJEV_MAX_CHOICES; i++) {
        snprintf(keys[i], sizeof keys[i], "route-%zu", i);
        choices[i].key = keys[i];
    }
    tnyjev_request r = choose_request();
    r.choices = choices;
    r.choice_count = TNYJEV_MAX_CHOICES;
    buf_t b = {0};
    ASSERT_EQ(TNYJEV_OK, tnyjev_encode(&r, "jev-latest", &b));
    buf_free(&b);
    r.choice_count = 1;
    ASSERT_EQ(TNYJEV_OK, tnyjev_encode(&r, "jev-latest", &b));
    buf_free(&b);
    PASS();
}

TEST tnyjev_decode_score_boundaries_and_usage(void) {
    tnyjev_request r = score_request();
    tnyjev_result result;
    ASSERT_EQ(TNYJEV_OK, tnyjev_decode(&r, score_response, strlen(score_response), &result));
    ASSERT_EQ(TNYJEV_SCORE, result.kind);
    ASSERT_EQ(0.75, result.value.score);
    ASSERT_EQ(307u, result.input_tokens);
    ASSERT_EQ(20u, result.output_tokens);
    ASSERT_STR_EQ("jev-1.13.0", result.model);
    ASSERT_EQ(TNYJEV_OK, decode_answer(&r, "{\"type\":\"noul\",\"noul\":0}", &result));
    ASSERT_EQ(0.0, result.value.score);
    ASSERT_EQ(TNYJEV_OK, decode_answer(&r, "{\"type\":\"noul\",\"noul\":1}", &result));
    ASSERT_EQ(1.0, result.value.score);
    PASS();
}

TEST tnyjev_decode_rejects_mistyped_out_of_range_and_duplicate_answers(void) {
    const char *bad[] = {"{}",
                         "null",
                         "{\"type\":\"noul\"}",
                         "{\"type\":\"noul\",\"noul\":1.01}",
                         "{\"type\":\"noul\",\"noul\":-0.01}",
                         "{\"type\":\"noul\",\"noul\":\"0.5\"}",
                         "{\"type\":\"noul\",\"noul\":true}",
                         "{\"type\":\"noul\",\"noul\":1e999}",
                         "{\"type\":\"score\",\"score\":0.5}",
                         "{\"type\":\"noul\\u0000x\",\"noul\":0.5}",
                         "{\"type\":\"noul\",\"noul\":0.5,\"noul\":0.6}"};
    tnyjev_request r = score_request();
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        tnyjev_result result;
        memset(&result, 0xff, sizeof result);
        ASSERT_EQ(TNYJEV_PROTOCOL, decode_answer(&r, bad[i], &result));
        ASSERT_EQ(0.0, result.value.score);
        ASSERT_EQ('\0', result.model[0]);
    }
    PASS();
}

TEST tnyjev_decode_rejects_every_truncated_prefix_and_invalid_envelope(void) {
    tnyjev_request r = score_request();
    tnyjev_result result;
    for (size_t i = 0; i < strlen(score_response); i++)
        ASSERT_EQ(TNYJEV_PROTOCOL, tnyjev_decode(&r, score_response, i, &result));
    const char *bad[] = {
        "[]", "{}", "null",
        "{\"model\":\"jev\",\"answers\":{\"other\":{\"type\":\"noul\",\"noul\":1}},"
        "\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}",
        "{\"model\":\"jev\",\"answers\":{\"decision\":{\"type\":\"noul\",\"noul\":1}},"
        "\"usage\":{\"input_tokens\":-1,\"output_tokens\":1}}"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        ASSERT_EQ(TNYJEV_PROTOCOL, tnyjev_decode(&r, bad[i], strlen(bad[i]), &result));
    ASSERT_EQ(TNYJEV_PROTOCOL, tnyjev_decode(&r, "{}", TNYJEV_MAX_BYTES + 1, &result));
    PASS();
}

TEST tnyjev_choice_typed_distribution_and_ties(void) {
    tnyjev_request r = choose_request();
    tnyjev_result result;
    ASSERT_EQ(TNYJEV_OK, decode_answer(&r,
                                       "{\"type\":\"choice\",\"choice\":\"billing\","
                                       "\"probabilities\":{\"support\":0.25,\"billing\":0.75},"
                                       "\"confidence\":0.8}",
                                       &result));
    ASSERT_EQ(TNYJEV_CHOOSE, result.kind);
    ASSERT_EQ(0u, result.value.choose.choice_index);
    ASSERT_EQ(2u, result.value.choose.count);
    ASSERT_EQ(0.75, result.value.choose.probabilities[0]);
    ASSERT_EQ(0.25, result.value.choose.probabilities[1]);
    ASSERT_EQ(0.8, result.value.choose.confidence);
    ASSERT_EQ(TNYJEV_OK, decode_answer(&r,
                                       "{\"type\":\"choice\",\"choice\":\"support\","
                                       "\"probabilities\":{\"support\":0.5,\"billing\":0.5},"
                                       "\"confidence\":0}",
                                       &result));
    ASSERT_EQ(1u, result.value.choose.choice_index);
    /* A sub-nanounit discrepancy at the documented comparison tolerance is
     * accepted; changing >= to > must not silently narrow that contract. */
    ASSERT_EQ(TNYJEV_OK,
              decode_answer(&r,
                            "{\"type\":\"choice\",\"choice\":\"billing\","
                            "\"probabilities\":{\"support\":0.5000000005,\"billing\":0.4999999995},"
                            "\"confidence\":0}",
                            &result));
    ASSERT_EQ(TNYJEV_PROTOCOL,
              decode_answer(&r,
                            "{\"type\":\"choice\",\"choice\":\"billing\","
                            "\"probabilities\":{\"support\":0.500000005,\"billing\":0.499999995},"
                            "\"confidence\":0}",
                            &result));
    PASS();
}

TEST tnyjev_choice_fails_closed_for_inconsistent_distributions(void) {
    const char *bad[] = {
        "\"choice\":\"other\",\"probabilities\":{\"billing\":0.8,\"support\":0.2},\"confidence\":0."
        "7",
        "\"choice\":\"support\",\"probabilities\":{\"billing\":0.8,\"support\":0.2},\"confidence\":"
        "0.7",
        "\"choice\":\"billing\",\"probabilities\":{\"billing\":0.8,\"support\":0.8},\"confidence\":"
        "0.7",
        "\"choice\":\"billing\",\"probabilities\":{\"billing\":0.8},\"confidence\":0.7",
        "\"choice\":\"billing\",\"probabilities\":{\"billing\":1,\"billing\":0},\"confidence\":0.7",
        "\"choice\":\"billing\",\"probabilities\":{\"billing\":1,\"support\":0,\"extra\":0},"
        "\"confidence\":0.7",
        "\"choice\":\"billing\",\"probabilities\":{\"billing\":1,\"support\":0},\"confidence\":2",
        "\"choice\":\"billing\",\"probabilities\":{\"billing\":1,\"support\":false},\"confidence\":"
        "1",
        "\"choice\":\"billing\",\"probabilities\":{\"billing\":1,\"support\":0}"};
    tnyjev_request r = choose_request();
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        buf_t b = {0};
        buf_appendf(&b, "{\"type\":\"choice\",%s}", bad[i]);
        tnyjev_result result;
        ASSERT_EQ(TNYJEV_PROTOCOL, decode_answer(&r, b.data, &result));
        buf_free(&b);
    }
    PASS();
}

static bool cancel_now(void *ud) {
    (void)ud;
    return true;
}

TEST tnyjev_preflight_has_no_network_and_secret_safe_errors(void) {
    tnyjev_request r = score_request();
    tnyjev_config c = {.api_key = "fixture-secret", .cancelled = cancel_now};
    tnyjev_result result;
    char err[256];
    ASSERT_EQ(TNYJEV_CANCELLED, tnyjev_evaluate(&c, &r, &result, err, sizeof err));
    const char *bad[] = {"http://example.com/v1/systemone",
                         "https://secret@typesafe.ai/x",
                         "https://typesafe.ai/x?secret",
                         "https://typesafe.ai/x#secret",
                         "https://typesafe.ai:443junk/x",
                         "https://typesafe.ai:99999/x",
                         "file:///tmp/test",
                         "https://typesafe.ai/x\r\nsecret",
                         ""};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        c.url = bad[i];
        ASSERT_EQ(TNYJEV_INVALID, tnyjev_evaluate(&c, &r, &result, err, sizeof err));
        ASSERT(!strstr(err, "secret"));
    }
    c.url = NULL;
    c.api_key = NULL;
    ASSERT_EQ(TNYJEV_AUTH, tnyjev_evaluate(&c, &r, &result, err, sizeof err));
    c.api_key = "fixture-secret\r\n";
    ASSERT_EQ(TNYJEV_AUTH, tnyjev_evaluate(&c, &r, &result, err, sizeof err));
    ASSERT(!strstr(err, "fixture-secret"));
    ASSERT_EQ(TNYJEV_AUTH, tnyjev_evaluate(&c, &r, &result, NULL, 0));
    ASSERT_EQ(TNYJEV_INVALID, tnyjev_evaluate(NULL, &r, &result, err, sizeof err));
    PASS();
}

SUITE(tnyjev_suite) {
    RUN_TEST(tnyjev_score_is_noul_and_text_is_escaped);
    RUN_TEST(tnyjev_choose_preserves_structured_values);
    RUN_TEST(tnyjev_request_rejects_invalid_types_duplicates_and_limits);
    RUN_TEST(tnyjev_choice_count_bounds_and_null_descriptions);
    RUN_TEST(tnyjev_decode_score_boundaries_and_usage);
    RUN_TEST(tnyjev_decode_rejects_mistyped_out_of_range_and_duplicate_answers);
    RUN_TEST(tnyjev_decode_rejects_every_truncated_prefix_and_invalid_envelope);
    RUN_TEST(tnyjev_choice_typed_distribution_and_ties);
    RUN_TEST(tnyjev_choice_fails_closed_for_inconsistent_distributions);
    RUN_TEST(tnyjev_preflight_has_no_network_and_secret_safe_errors);
}
