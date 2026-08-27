/* test_cursor.c — unit tests for the cursor bridge's stream mapping
 * (src/backends/cursor/map.c). The wire truth is sdk_messages.proto:
 * `sdkMessage` is a `type` discriminator plus a Struct payload in `message`,
 * and the payload is the @cursor/sdk stream event with its per-tool
 * `tool_call.<variant>ToolCall = {args, result}` union. Tool calls must come
 * out named with clipped args/results, never as an opaque "tool". */
#include "greatest.h"
#include "backends/cursor/impl.h"

#include <stdlib.h>
#include <string.h>

/* ---- event recorder ---- */

#define REC_MAX 16
typedef struct {
    tny_event_kind kind[REC_MAX];
    char *text[REC_MAX];           /* text events */
    char *name[REC_MAX];           /* tool_name */
    char *id[REC_MAX];             /* tool_id */
    char *detail[REC_MAX];         /* tool_detail */
    bool ok[REC_MAX];              /* tool_ok */
    tny_stop_reason stop[REC_MAX]; /* TURN_END */
    int n;
} rec_t;

static void rec_cb(const tny_backend_event *ev, void *ud) {
    rec_t *r = ud;
    if (r->n == REC_MAX) return;
    r->kind[r->n] = ev->kind;
    r->text[r->n] = ev->text ? xstrndup(ev->text, ev->text_len) : NULL;
    r->name[r->n] = ev->tool_name ? xstrdup(ev->tool_name) : NULL;
    r->id[r->n] = ev->tool_id ? xstrdup(ev->tool_id) : NULL;
    r->detail[r->n] = ev->tool_detail ? xstrdup(ev->tool_detail) : NULL;
    r->ok[r->n] = ev->tool_ok;
    r->stop[r->n] = ev->stop;
    r->n++;
}

static bool rec_has_kind(const rec_t *r, tny_event_kind k) {
    for (int i = 0; i < r->n; i++)
        if (r->kind[i] == k) return true;
    return false;
}

static void rec_free(rec_t *r) {
    for (int i = 0; i < r->n; i++) {
        free(r->text[i]);
        free(r->name[i]);
        free(r->id[i]);
        free(r->detail[i]);
    }
    memset(r, 0, sizeof *r);
}

static void impl_init(cu_impl *o, rec_t *r) {
    memset(o, 0, sizeof *o);
    buf_init(&o->last_status);
    buf_init(&o->last_tool_start);
    o->cb = rec_cb;
    o->ud = r;
}

static void impl_free(cu_impl *o) {
    free(o->run_id);
    buf_free(&o->last_status);
    buf_free(&o->last_tool_start);
}

static void feed(cu_impl *o, const char *json) { cu_on_frame(0, json, strlen(json), o); }

/* ---- the SdkMessage envelope: type + Struct payload in `message` ---- */

TEST tool_call_union_maps_name_args_and_clipped_result(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);

    /* real bridge shape: docs/backends/cursor-bridge.md "Tool call payloads" */
    feed(&o, "{\"sdkMessage\":{\"type\":\"tool_call\",\"message\":{"
             "\"type\":\"tool_call\",\"subtype\":\"started\","
             "\"call_id\":\"tc-9\",\"session_id\":\"s1\","
             "\"tool_call\":{\"readToolCall\":{\"args\":{\"path\":\"README.md\"}}}}}}");
    feed(&o, "{\"sdkMessage\":{\"type\":\"tool_call\",\"message\":{"
             "\"type\":\"tool_call\",\"subtype\":\"completed\","
             "\"call_id\":\"tc-9\",\"session_id\":\"s1\","
             "\"tool_call\":{\"readToolCall\":{\"args\":{\"path\":\"README.md\"},"
             "\"result\":{\"success\":{\"content\":\"hello world\","
             "\"totalLines\":1}}}}}}}");

    ASSERT_EQ(2, r.n);
    ASSERT_EQ(TNY_EV_TOOL_START, r.kind[0]);
    ASSERT_STR_EQ("read", r.name[0]);
    ASSERT_STR_EQ("tc-9", r.id[0]);
    ASSERT(r.detail[0] && strstr(r.detail[0], "README.md"));
    ASSERT(r.ok[0]);

    ASSERT_EQ(TNY_EV_TOOL_END, r.kind[1]);
    ASSERT_STR_EQ("read", r.name[1]);
    ASSERT(r.detail[1] && strstr(r.detail[1], "hello world"));
    /* completed detail is the clipped result, not the repeated args */
    ASSERT(!strstr(r.detail[1], "README.md"));
    ASSERT(r.ok[1]);

    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST tool_call_error_result_flags_not_ok(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);

    feed(&o, "{\"sdkMessage\":{\"type\":\"tool_call\",\"message\":{"
             "\"type\":\"tool_call\",\"subtype\":\"completed\",\"call_id\":\"tc-2\","
             "\"tool_call\":{\"shellToolCall\":{\"args\":{\"command\":\"false\"},"
             "\"result\":{\"error\":\"exit status 1\"}}}}}}");

    ASSERT_EQ(1, r.n);
    ASSERT_EQ(TNY_EV_TOOL_END, r.kind[0]);
    ASSERT_STR_EQ("shell", r.name[0]);
    ASSERT_FALSE(r.ok[0]);
    ASSERT(r.detail[0] && strstr(r.detail[0], "exit status 1"));

    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST tool_call_mcp_variant_prefers_inner_tool_name(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);

    feed(&o, "{\"sdkMessage\":{\"type\":\"tool_call\",\"message\":{"
             "\"type\":\"tool_call\",\"subtype\":\"started\",\"call_id\":\"tc-3\","
             "\"tool_call\":{\"mcpToolCall\":{\"name\":\"linear_search\","
             "\"args\":{\"query\":\"bug\"}}}}}}");

    ASSERT_EQ(1, r.n);
    ASSERT_EQ(TNY_EV_TOOL_START, r.kind[0]);
    ASSERT_STR_EQ("linear_search", r.name[0]);
    ASSERT(r.detail[0] && strstr(r.detail[0], "bug"));

    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST tool_call_unknown_variant_derives_name_from_key(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);

    /* a future tool: the *ToolCall key still names it, never "tool" */
    feed(&o, "{\"sdkMessage\":{\"type\":\"tool_call\",\"message\":{"
             "\"type\":\"tool_call\",\"subtype\":\"started\",\"call_id\":\"tc-4\","
             "\"tool_call\":{\"semanticSearchToolCall\":{\"args\":{\"q\":\"x\"}}}}}}");

    ASSERT_EQ(1, r.n);
    ASSERT_STR_EQ("semanticSearch", r.name[0]);

    rec_free(&r);
    impl_free(&o);
    PASS();
}

/* one started frame with the given union key; returns the recorded name */
static int feed_variant_key(const char *key, char *out, size_t cap) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);
    buf_t j;
    buf_init(&j);
    buf_appends(&j, "{\"sdkMessage\":{\"type\":\"tool_call\",\"message\":{"
                    "\"type\":\"tool_call\",\"subtype\":\"started\","
                    "\"call_id\":\"tc\",\"tool_call\":{");
    jescape(&j, key);
    buf_appends(&j, ":{\"args\":{\"q\":\"x\"}}}}}}");
    feed(&o, j.data);
    buf_free(&j);
    int rc = -1;
    if (r.n == 1 && r.name[0]) {
        snprintf(out, cap, "%s", r.name[0]);
        rc = 0;
    }
    rec_free(&r);
    impl_free(&o);
    return rc;
}

TEST tool_call_variant_suffix_stripping_edge_cases(void) {
    char name[80];

    /* proto snake_case spelling of the union key */
    ASSERT_EQ(0, feed_variant_key("read_tool_call", name, sizeof name));
    ASSERT_STR_EQ("read", name);

    /* degenerate keys: a bare suffix strips to nothing usable -> keep it */
    ASSERT_EQ(0, feed_variant_key("ToolCall", name, sizeof name));
    ASSERT_STR_EQ("ToolCall", name);
    ASSERT_EQ(0, feed_variant_key("_tool_call", name, sizeof name));
    ASSERT_STR_EQ("_tool_call", name);

    /* a long key without a recognized suffix passes through whole */
    ASSERT_EQ(0, feed_variant_key("customtoolthing", name, sizeof name));
    ASSERT_STR_EQ("customtoolthing", name);

    /* names longer than the scratch buffer fall back to the generic label
     * instead of truncating into a wrong name (64 chars + "ToolCall") */
    char big[80];
    memset(big, 'a', 64);
    memcpy(big + 64, "ToolCall", 9);
    ASSERT_EQ(0, feed_variant_key(big, name, sizeof name));
    ASSERT_STR_EQ("tool", name);

    PASS();
}

TEST tool_call_without_subtype_ends_only_with_result(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);

    feed(&o, "{\"sdkMessage\":{\"type\":\"tool_call\",\"message\":{"
             "\"type\":\"tool_call\",\"call_id\":\"tc-5\","
             "\"tool_call\":{\"lsToolCall\":{\"args\":{\"path\":\".\"}}}}}}");
    feed(&o, "{\"sdkMessage\":{\"type\":\"tool_call\",\"message\":{"
             "\"type\":\"tool_call\",\"call_id\":\"tc-5\","
             "\"tool_call\":{\"lsToolCall\":{"
             "\"result\":{\"success\":{\"files\":[\"a\"]}}}}}}}");

    ASSERT_EQ(2, r.n);
    ASSERT_EQ(TNY_EV_TOOL_START, r.kind[0]);
    ASSERT_EQ(TNY_EV_TOOL_END, r.kind[1]);

    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST tool_call_args_win_over_raw_args(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);

    feed(&o, "{\"sdkMessage\":{\"type\":\"tool_call\",\"message\":{"
             "\"type\":\"tool_call\",\"subtype\":\"started\",\"call_id\":\"tc-6\","
             "\"tool_call\":{\"grepToolCall\":{\"args\":{\"pattern\":\"WANTED\"},"
             "\"rawArgs\":{\"pattern\":\"UNPARSED\"}}}}}}");

    ASSERT_EQ(1, r.n);
    ASSERT(r.detail[0] && strstr(r.detail[0], "WANTED"));
    ASSERT(!strstr(r.detail[0], "UNPARSED"));

    rec_free(&r);
    impl_free(&o);
    PASS();
}

/* The shapes below were captured live from cursor-sdk-bridge v1.0.28
 * (sdk 1.0.28): flat name/status/args in the payload, results wrapped as
 * {"status":"success"|"error","value":{…}}, and `running` frames re-emitted
 * for one call while it executes. */

TEST tool_call_live_bridge_shape_maps_and_unwraps_value(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);

    feed(&o, "{\"sdkMessage\":{\"type\":\"tool_call\",\"message\":{"
             "\"type\":\"tool_call\",\"agent_id\":\"a1\",\"run_id\":\"run-9\","
             "\"call_id\":\"call-1\",\"name\":\"read\",\"status\":\"running\","
             "\"args\":{\"path\":\"note.txt\"}}},\"offset\":\"18\"}");
    feed(&o, "{\"sdkMessage\":{\"type\":\"tool_call\",\"message\":{"
             "\"type\":\"tool_call\",\"call_id\":\"call-1\",\"name\":\"read\","
             "\"status\":\"completed\",\"args\":{\"path\":\"note.txt\"},"
             "\"result\":{\"status\":\"success\",\"value\":{"
             "\"content\":\"hello live\\n\",\"totalLines\":2}}}}}");

    ASSERT_EQ(2, r.n);
    ASSERT_EQ(TNY_EV_TOOL_START, r.kind[0]);
    ASSERT_STR_EQ("read", r.name[0]);
    ASSERT(r.detail[0] && strstr(r.detail[0], "note.txt"));
    ASSERT_EQ(TNY_EV_TOOL_END, r.kind[1]);
    ASSERT(r.ok[1]);
    /* detail is the unwrapped value, not the {"status","value"} wrapper */
    ASSERT(r.detail[1] && strstr(r.detail[1], "hello live"));
    ASSERT(!strstr(r.detail[1], "success"));
    ASSERT(o.run_id);
    ASSERT_STR_EQ("run-9", o.run_id);

    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST tool_call_live_error_wrapper_flags_not_ok(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);

    feed(&o, "{\"sdkMessage\":{\"type\":\"tool_call\",\"message\":{"
             "\"type\":\"tool_call\",\"call_id\":\"call-2\",\"name\":\"shell\","
             "\"status\":\"completed\",\"result\":{\"status\":\"error\","
             "\"value\":{\"stderr\":\"boom\"}}}}}");

    ASSERT_EQ(1, r.n);
    ASSERT_EQ(TNY_EV_TOOL_END, r.kind[0]);
    ASSERT_FALSE(r.ok[0]);
    ASSERT(r.detail[0] && strstr(r.detail[0], "boom"));

    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST tool_call_repeated_running_frames_are_deduped(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);

    const char *running = "{\"sdkMessage\":{\"type\":\"tool_call\",\"message\":{"
                          "\"type\":\"tool_call\",\"call_id\":\"call-3\",\"name\":\"edit\","
                          "\"status\":\"running\",\"args\":{\"path\":\"out.txt\"}}}}";
    feed(&o, running);
    feed(&o, running); /* re-emitted while the tool executes: dropped */
    /* changed args stream through */
    feed(&o, "{\"sdkMessage\":{\"type\":\"tool_call\",\"message\":{"
             "\"type\":\"tool_call\",\"call_id\":\"call-3\",\"name\":\"edit\","
             "\"status\":\"running\",\"args\":{\"path\":\"out.txt\",\"n\":2}}}}");
    feed(&o, "{\"sdkMessage\":{\"type\":\"tool_call\",\"message\":{"
             "\"type\":\"tool_call\",\"call_id\":\"call-3\",\"name\":\"edit\","
             "\"status\":\"completed\",\"result\":{\"status\":\"success\","
             "\"value\":{}}}}}");
    /* after the end, the same start signature is a new call: rendered */
    feed(&o, running);

    ASSERT_EQ(4, r.n);
    ASSERT_EQ(TNY_EV_TOOL_START, r.kind[0]);
    ASSERT_EQ(TNY_EV_TOOL_START, r.kind[1]);
    ASSERT(r.detail[1] && strstr(r.detail[1], "\"n\":2"));
    ASSERT_EQ(TNY_EV_TOOL_END, r.kind[2]);
    ASSERT_EQ(TNY_EV_TOOL_START, r.kind[3]);

    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST result_usage_accepts_protojson_string_counts(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);

    /* RunResult renders int64 token counts as strings (observed live) */
    feed(&o, "{\"result\":{\"status\":\"RUN_LIFECYCLE_STATUS_FINISHED\","
             "\"result\":{\"result\":\"DONE\",\"usage\":{"
             "\"inputTokens\":\"24530\",\"outputTokens\":\"69\"}}}}");

    ASSERT_EQ(24530, o.in_tok);
    ASSERT_EQ(69, o.out_tok);
    rec_free(&r);
    impl_free(&o);

    /* a count with trailing junk is not a number: leave the total alone */
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);
    feed(&o, "{\"sdkMessage\":{\"type\":\"usage\",\"message\":{"
             "\"type\":\"usage\",\"usage\":{\"inputTokens\":\"24abc\","
             "\"outputTokens\":\"7\"}}}}");
    ASSERT_EQ(0, o.in_tok);
    ASSERT_EQ(7, o.out_tok);

    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST tool_call_flat_legacy_shape_still_maps(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);

    /* pre-envelope shape (older mocks/builds): name/args at the top level */
    feed(&o, "{\"sdkMessage\":{\"type\":\"tool_call\",\"subtype\":\"started\","
             "\"toolCallId\":\"tc1\",\"name\":\"read_file\","
             "\"args\":{\"path\":\"README.md\"}}}");

    ASSERT_EQ(1, r.n);
    ASSERT_EQ(TNY_EV_TOOL_START, r.kind[0]);
    ASSERT_STR_EQ("read_file", r.name[0]);
    ASSERT_STR_EQ("tc1", r.id[0]);
    ASSERT(r.detail[0] && strstr(r.detail[0], "README.md"));

    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST envelope_unwrap_reads_status_text_and_run_id(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);

    feed(&o, "{\"sdkMessage\":{\"type\":\"system\",\"message\":{"
             "\"type\":\"system\",\"subtype\":\"init\",\"run_id\":\"run-77\"}}}");
    feed(&o, "{\"sdkMessage\":{\"type\":\"status\",\"message\":{"
             "\"type\":\"status\",\"status\":\"running\","
             "\"message\":\"tools are running\"}}}");

    ASSERT(o.run_id);
    ASSERT_STR_EQ("run-77", o.run_id);
    ASSERT_EQ(1, r.n);
    ASSERT_EQ(TNY_EV_STATUS, r.kind[0]);
    ASSERT_STR_EQ("tools are running", r.text[0]);

    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST assistant_text_survives_envelope_and_flat_shapes(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);

    feed(&o, "{\"sdkMessage\":{\"type\":\"assistant\",\"message\":{"
             "\"type\":\"assistant\",\"message\":{\"role\":\"assistant\","
             "\"content\":[{\"type\":\"text\",\"text\":\"enveloped\"}]}}}}");
    feed(&o, "{\"sdkMessage\":{\"type\":\"assistant\",\"message\":{"
             "\"content\":[{\"type\":\"text\",\"text\":\" flat\"}]}}}");

    ASSERT_EQ(2, r.n);
    ASSERT_EQ(TNY_EV_TEXT_DELTA, r.kind[0]);
    ASSERT_STR_EQ("enveloped", r.text[0]);
    ASSERT_STR_EQ(" flat", r.text[1]);

    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST result_final_text_falls_back_to_run_result(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);

    /* no assistant events streamed: the RunResult carries the final text */
    feed(&o, "{\"result\":{\"runId\":\"run-1\",\"agentId\":\"a1\","
             "\"status\":\"RUN_LIFECYCLE_STATUS_FINISHED\","
             "\"result\":{\"runId\":\"run-1\",\"result\":\"the final answer\","
             "\"usage\":{\"inputTokens\":7,\"outputTokens\":3}}}}");

    ASSERT(r.n >= 2);
    ASSERT_EQ(TNY_EV_TEXT_DELTA, r.kind[0]);
    ASSERT_STR_EQ("the final answer", r.text[0]);
    ASSERT_EQ(TNY_EV_TURN_END, r.kind[r.n - 1]);
    /* FINISHED is a success: no error event, and the turn stops clean */
    ASSERT_FALSE(rec_has_kind(&r, TNY_EV_ERROR));
    ASSERT_EQ(TNY_STOP_DONE, r.stop[r.n - 1]);
    ASSERT_EQ(7, o.in_tok);
    ASSERT_EQ(3, o.out_tok);

    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST result_streamed_text_is_not_doubled_by_the_fallback(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);

    /* a result frame that carries both loose text and RunResult.result:
     * the fallback must not append the final text on top */
    feed(&o, "{\"result\":{\"status\":\"RUN_LIFECYCLE_STATUS_FINISHED\","
             "\"text\":\"streamed\",\"result\":{\"result\":\"final\"}}}");

    ASSERT(r.n >= 2);
    ASSERT_EQ(TNY_EV_TEXT_DELTA, r.kind[0]);
    ASSERT_STR_EQ("streamed", r.text[0]);

    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST result_lowercase_error_status_fails_the_turn(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);

    feed(&o, "{\"result\":{\"status\":\"error\",\"result\":{}}}");

    ASSERT(rec_has_kind(&r, TNY_EV_ERROR));
    ASSERT_EQ(TNY_EV_TURN_END, r.kind[r.n - 1]);
    ASSERT_EQ(TNY_STOP_ERROR, r.stop[r.n - 1]);

    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST result_expired_status_fails_the_turn(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);

    feed(&o, "{\"result\":{\"status\":\"RUN_LIFECYCLE_STATUS_EXPIRED\","
             "\"result\":{}}}");

    ASSERT(rec_has_kind(&r, TNY_EV_ERROR));
    ASSERT_EQ(TNY_STOP_ERROR, r.stop[r.n - 1]);

    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST result_lifecycle_error_status_fails_the_turn(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);

    feed(&o, "{\"sdkMessage\":{\"type\":\"status\",\"message\":{"
             "\"type\":\"status\",\"message\":\"model refused the request\"}}}");
    feed(&o, "{\"result\":{\"runId\":\"run-1\","
             "\"status\":\"RUN_LIFECYCLE_STATUS_ERROR\",\"result\":{}}}");

    ASSERT(r.n >= 2);
    ASSERT_EQ(TNY_EV_ERROR, r.kind[r.n - 2]);
    ASSERT(r.text[r.n - 2] && strstr(r.text[r.n - 2], "model refused the request"));
    ASSERT_EQ(TNY_EV_TURN_END, r.kind[r.n - 1]);

    rec_free(&r);
    impl_free(&o);
    PASS();
}

SUITE(cursor_suite) {
    RUN_TEST(tool_call_union_maps_name_args_and_clipped_result);
    RUN_TEST(tool_call_error_result_flags_not_ok);
    RUN_TEST(tool_call_mcp_variant_prefers_inner_tool_name);
    RUN_TEST(tool_call_unknown_variant_derives_name_from_key);
    RUN_TEST(tool_call_variant_suffix_stripping_edge_cases);
    RUN_TEST(tool_call_without_subtype_ends_only_with_result);
    RUN_TEST(tool_call_args_win_over_raw_args);
    RUN_TEST(tool_call_live_bridge_shape_maps_and_unwraps_value);
    RUN_TEST(tool_call_live_error_wrapper_flags_not_ok);
    RUN_TEST(tool_call_repeated_running_frames_are_deduped);
    RUN_TEST(result_usage_accepts_protojson_string_counts);
    RUN_TEST(tool_call_flat_legacy_shape_still_maps);
    RUN_TEST(envelope_unwrap_reads_status_text_and_run_id);
    RUN_TEST(assistant_text_survives_envelope_and_flat_shapes);
    RUN_TEST(result_final_text_falls_back_to_run_result);
    RUN_TEST(result_streamed_text_is_not_doubled_by_the_fallback);
    RUN_TEST(result_lowercase_error_status_fails_the_turn);
    RUN_TEST(result_expired_status_fails_the_turn);
    RUN_TEST(result_lifecycle_error_status_fails_the_turn);
}
