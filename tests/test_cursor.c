/* test_cursor.c — unit tests for the cursor bridge's stream mapping
 * (src/backends/cursor/map.c). The wire truth is sdk_messages.proto:
 * `sdkMessage` is a `type` discriminator plus a Struct payload in `message`,
 * and the payload is the @cursor/sdk stream event with its per-tool
 * `tool_call.<variant>ToolCall = {args, result}` union. Tool calls must come
 * out named with clipped args/results, never as an opaque "tool". */
#include "greatest.h"
#include "backends/cursor/impl.h"
#include "core/cursor_config.h"
#include "util/tny_poll.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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
    int64_t in_tok[REC_MAX], out_tok[REC_MAX];
    double cost[REC_MAX];
    bool has_cost[REC_MAX];
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
    r->in_tok[r->n] = ev->in_tokens;
    r->out_tok[r->n] = ev->out_tokens;
    r->cost[r->n] = ev->cost;
    r->has_cost[r->n] = ev->has_cost;
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
    free(o->observe_offset);
    free(o->observe_progress_offset);
    free(o->send_hashes);
    buf_free(&o->last_status);
    buf_free(&o->last_tool_start);
}

static void feed(cu_impl *o, const char *json) { cu_on_frame(0, json, strlen(json), o); }

static int reject_cancel_pump_thread(pthread_t *thread, const pthread_attr_t *attr,
                                     void *(*start)(void *), void *arg) {
    (void)thread;
    (void)attr;
    (void)start;
    (void)arg;
    return EAGAIN;
}

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

TEST result_cancelled_waits_for_and_maps_terminal_interruption(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);
    o.cancel_requested = true;

    feed(&o, "{\"result\":{\"runId\":\"run-cancelled\","
             "\"status\":\"RUN_LIFECYCLE_STATUS_CANCELLED\",\"result\":{}}}");

    ASSERT(o.run_id);
    ASSERT_STR_EQ("run-cancelled", o.run_id);
    ASSERT_EQ(TNY_EV_TURN_END, r.kind[r.n - 1]);
    ASSERT_EQ(TNY_STOP_INTERRUPTED, r.stop[r.n - 1]);
    ASSERT_FALSE(rec_has_kind(&r, TNY_EV_ERROR));

    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST interaction_and_step_typed_structs_map_without_inner_type(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);

    feed(&o, "{\"interactionUpdate\":{\"type\":\"text_delta\","
             "\"update\":{\"text\":\"delta\"}}}");
    feed(&o, "{\"step\":{\"type\":\"plan\",\"step\":{\"text\":\"next\"}}}");

    ASSERT_EQ(2, r.n);
    ASSERT_EQ(TNY_EV_TEXT_DELTA, r.kind[0]);
    ASSERT_STR_EQ("delta", r.text[0]);
    ASSERT_EQ(TNY_EV_PLAN, r.kind[1]);
    ASSERT_STR_EQ("next", r.text[1]);

    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST usage_maps_charged_cents_to_normalized_dollars(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);

    feed(&o, "{\"sdkMessage\":{\"type\":\"usage\",\"message\":{"
             "\"type\":\"usage\",\"usage\":{\"inputTokens\":9,"
             "\"outputTokens\":4},\"cost\":{\"rawCostCents\":8.0,"
             "\"chargedCents\":2.5}}}}");
    feed(&o, "{\"result\":{\"status\":\"RUN_LIFECYCLE_STATUS_FINISHED\","
             "\"result\":{}}}");

    int usage = -1;
    for (int i = 0; i < r.n; i++)
        if (r.kind[i] == TNY_EV_USAGE) usage = i;
    ASSERT(usage >= 0);
    ASSERT_EQ(9, r.in_tok[usage]);
    ASSERT_EQ(4, r.out_tok[usage]);
    ASSERT(r.has_cost[usage]);
    ASSERT(r.cost[usage] > 0.024 && r.cost[usage] < 0.026);

    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST observe_replay_deduplicates_send_prefix_and_tracks_observe_offset(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);
    const char *first = "{\"sdkMessage\":{\"type\":\"assistant\","
                        "\"message\":{\"text\":\"once\"}}}";
    o.stream_kind = CU_STREAM_SEND;
    feed(&o, first);
    ASSERT_EQ(1, r.n);
    o.stream_kind = CU_STREAM_OBSERVE;
    o.observe_replay = true;
    feed(&o, "{\"sdkMessage\":{\"type\":\"assistant\","
             "\"message\":{\"text\":\"once\"}},\"offset\":\"1\"}");
    ASSERT_EQ(1, r.n);
    ASSERT_STR_EQ("1", o.observe_offset);
    feed(&o, "{\"sdkMessage\":{\"type\":\"assistant\","
             "\"message\":{\"text\":\"twice\"}},\"offset\":\"2\"}");
    ASSERT_EQ(2, r.n);
    ASSERT_STR_EQ("twice", r.text[1]);
    ASSERT_STR_EQ("2", o.observe_offset);

    rec_free(&r);
    free(o.observe_offset);
    o.observe_offset = NULL;
    impl_free(&o);
    PASS();
}

TEST unknown_result_status_fails_closed(void) {
    cu_impl o;
    rec_t r;
    memset(&r, 0, sizeof r);
    impl_init(&o, &r);

    feed(&o, "{\"result\":{\"status\":\"RUN_LIFECYCLE_STATUS_FUTURE\","
             "\"result\":{}}}");
    ASSERT(rec_has_kind(&r, TNY_EV_ERROR));
    ASSERT_EQ(TNY_STOP_ERROR, r.stop[r.n - 1]);

    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST send_resets_cancel_rpc_state_for_each_turn(void) {
    tny_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    tny_cursor_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.agent_options_json = "{}";
    cfg.send_options_json = "{}";
    ctx.cursor_config = &cfg;
    ctx.model = xstrdup("cursor-model");

    tny_backend *backend = tny_backend_cursor_new(&ctx);
    ASSERT(backend);
    cu_impl *o = backend->impl;
    o->agent_id = xstrdup("agent-1");
    o->model = xstrdup("cursor-model");
    o->cancel_sent = true; /* prior turn successfully called CancelRun */
    char err[256];
    ASSERT_EQ(-1, backend->send(backend, "next turn", NULL, NULL, NULL, err, sizeof err));
    ASSERT_FALSE(o->cancel_sent);

    backend->destroy(backend);
    free(ctx.model);
    PASS();
}

TEST session_pointer_is_versioned_and_carries_durable_run_state(void) {
    tny_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    tny_cursor_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.runtime = TNY_CURSOR_RUNTIME_CLOUD;
    ctx.cursor_config = &cfg;
    tny_backend *backend = tny_backend_cursor_new(&ctx);
    ASSERT(backend);
    cu_impl *o = backend->impl;
    o->agent_id = xstrdup("agent-1");
    o->run_id = xstrdup("run-2");
    o->observe_offset = xstrdup("offset:9");

    char *pointer = backend->session_pointer(backend);
    ASSERT(pointer);
    ASSERT(str_starts(pointer, "cursor-sdk.v1:"));
    const char *json = pointer + strlen("cursor-sdk.v1:");
    yyjson_doc *doc = jparse(json, strlen(json));
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    ASSERT_STR_EQ("agent-1", jget_str(root, "agent_id"));
    ASSERT_STR_EQ("run-2", jget_str(root, "run_id"));
    ASSERT_STR_EQ("offset:9", jget_str(root, "after_offset"));
    ASSERT_STR_EQ("cloud", jget_str(root, "runtime"));
    yyjson_doc_free(doc);
    free(pointer);
    backend->destroy(backend);
    PASS();
}

TEST bridge_rejects_partial_store_callback_credentials_before_spawn(void) {
    cursor_bridge bridge;
    cursor_bridge_init(&bridge);
    tny_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    cursor_bridge_launch_options options = {
        .store_callback_url = "http://127.0.0.1:1234",
    };
    char err[256];
    ASSERT_EQ(-1, cursor_bridge_spawn(&bridge, &ctx, "key", &options, 1, err, sizeof err));
    ASSERT(strstr(err, "URL and token") != NULL);
    ASSERT_EQ(0, bridge.pid);
    ASSERT_EQ(-1, bridge.err_fd);
    cursor_bridge_stop(&bridge, 0);
    PASS();
}

TEST bridge_ready_timeout_override_is_strict_and_clamped(void) {
    int timeout_ms = -1;
    char err[160] = {0};

    ASSERT_EQ(0, cursor_bridge_ready_timeout_ms(NULL, &timeout_ms, err, sizeof err));
    ASSERT_EQ(30000, timeout_ms);
    ASSERT_EQ(0, cursor_bridge_ready_timeout_ms("60000", &timeout_ms, err, sizeof err));
    ASSERT_EQ(60000, timeout_ms);
    ASSERT_EQ(0, cursor_bridge_ready_timeout_ms("999", &timeout_ms, err, sizeof err));
    ASSERT_EQ(1000, timeout_ms);
    ASSERT_EQ(0, cursor_bridge_ready_timeout_ms("1000", &timeout_ms, err, sizeof err));
    ASSERT_EQ(1000, timeout_ms);
    ASSERT_EQ(0, cursor_bridge_ready_timeout_ms("120000", &timeout_ms, err, sizeof err));
    ASSERT_EQ(120000, timeout_ms);
    ASSERT_EQ(0, cursor_bridge_ready_timeout_ms("120001", &timeout_ms, err, sizeof err));
    ASSERT_EQ(120000, timeout_ms);
    ASSERT_EQ(
        0, cursor_bridge_ready_timeout_ms("999999999999999999999", &timeout_ms, err, sizeof err));
    ASSERT_EQ(120000, timeout_ms);

    const char *invalid[] = {"", " 60000", "+60000", "60000ms", "60\n000", NULL};
    for (size_t i = 0; invalid[i]; i++) {
        memset(err, 0, sizeof err);
        ASSERT_EQ(-1, cursor_bridge_ready_timeout_ms(invalid[i], &timeout_ms, err, sizeof err));
        ASSERT(strstr(err, "TNY_CURSOR_BRIDGE_READY_TIMEOUT_MS"));
    }
    PASS();
}

static int open_fd_count(void) {
    int count = 0;
    for (int fd = 0; fd < 512; fd++)
        if (fcntl(fd, F_GETFD) >= 0) count++;
    return count;
}

static bool process_group_gone(pid_t pgid) {
    for (int i = 0; i < 50; i++) {
        if (kill(-pgid, 0) != 0 && errno == ESRCH) return true;
        struct timespec delay = {0, 10 * 1000 * 1000};
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
    }
    return false;
}

typedef struct {
    const char *name;
    char *value;
    bool present;
} saved_env;

static saved_env save_env(const char *name) {
    const char *value = getenv(name);
    saved_env saved = {name, value ? xstrdup(value) : NULL, value != NULL};
    return saved;
}

static void restore_env(saved_env *saved) {
    if (saved->present) setenv(saved->name, saved->value, 1);
    else unsetenv(saved->name);
    free(saved->value);
    saved->value = NULL;
}

static const char bridge_spawn_fixture[] =
    "#!/bin/sh\n"
    "fail() { echo \"fixture check failed: $1\" >&2; exit 42; }\n"
    "actual_cwd=$(pwd -P) || fail cwd\n"
    "[ \"$actual_cwd\" = \"$EXPECT_CWD\" ] || fail cwd\n"
    "[ \"${CURSOR_API_KEY-}\" = \"$EXPECT_API_KEY\" ] || fail api-key\n"
    "[ \"${CURSOR_SDK_CLIENT_LANGUAGE-}\" = c ] || fail client-language\n"
    "if [ \"$EXPECT_STORE_MODE\" = set ]; then\n"
    "  [ \"${CURSOR_SDK_STORE_CALLBACK_AUTH_TOKEN-}\" = \"$EXPECT_STORE_TOKEN\" ] || "
    "fail store-token\n"
    "else\n"
    "  [ \"${CURSOR_SDK_STORE_CALLBACK_AUTH_TOKEN+x}\" != x ] || fail store-token-present\n"
    "fi\n"
    "[ \"$1\" = --workspace ] && [ \"$2\" = \"$EXPECT_CWD\" ] || fail workspace-argv\n"
    "[ \"$3\" = --host ] && [ \"$4\" = 127.0.0.1 ] || fail host-argv\n"
    "[ \"$5\" = --port ] && [ \"$6\" = 0 ] || fail port-argv\n"
    "if [ \"$EXPECT_ARGS_MODE\" = full ]; then\n"
    "  [ \"$#\" = 12 ] || fail argc\n"
    "  [ \"$7\" = --state-root ] && [ \"$8\" = \"$EXPECT_STATE_ROOT\" ] || fail "
    "state-argv\n"
    "  [ \"$9\" = --local-store ] && [ \"${10}\" = '{\"type\":\"memory\"}' ] || fail "
    "local-store-argv\n"
    "  [ \"${11}\" = --store-callback-url ] && [ \"${12}\" = http://127.0.0.1:7 ] || "
    "fail callback-argv\n"
    "else\n"
    "  [ \"$#\" = 6 ] || fail argc\n"
    "fi\n"
    "echo 'cursor-sdk-bridge ready {\"schemaVersion\":1,\"transport\":\"tcp\","
    "\"protocol\":\"connect\",\"url\":\"http://127.0.0.1:9\","
    "\"authToken\":\"fixture-bearer\"}' >&2\n"
    "while :; do :; done\n";

TEST bridge_spawn_sets_exact_cwd_argv_and_environment(void) {
    saved_env saved[] = {
        save_env("CURSOR_API_KEY"),
        save_env("CURSOR_SDK_CLIENT_LANGUAGE"),
        save_env("CURSOR_SDK_STORE_CALLBACK_AUTH_TOKEN"),
        save_env("EXPECT_CWD"),
        save_env("EXPECT_API_KEY"),
        save_env("EXPECT_STORE_MODE"),
        save_env("EXPECT_STORE_TOKEN"),
        save_env("EXPECT_ARGS_MODE"),
        save_env("EXPECT_STATE_ROOT"),
    };
    char root_template[] = "/tmp/tny-bridge-spawn-XXXXXX";
    char *root = mkdtemp(root_template);
    ASSERT(root);
    char cwd[PATH_MAX];
    ASSERT(realpath(root, cwd));
    char *fixture = path_join(cwd, "bridge-fixture");
    ASSERT(fixture);
    ASSERT_EQ(0, file_write_atomic(fixture, bridge_spawn_fixture, sizeof bridge_spawn_fixture - 1));
    ASSERT_EQ(0, chmod(fixture, 0700));
    int baseline_fds = open_fd_count();

    ASSERT_EQ(0, setenv("CURSOR_API_KEY", "ambient-api", 1));
    ASSERT_EQ(0, setenv("CURSOR_SDK_CLIENT_LANGUAGE", "ambient-language", 1));
    ASSERT_EQ(0, setenv("CURSOR_SDK_STORE_CALLBACK_AUTH_TOKEN", "ambient-store", 1));
    ASSERT_EQ(0, setenv("EXPECT_CWD", cwd, 1));
    ASSERT_EQ(0, setenv("EXPECT_API_KEY", "explicit-api", 1));
    ASSERT_EQ(0, setenv("EXPECT_STORE_MODE", "set", 1));
    ASSERT_EQ(0, setenv("EXPECT_STORE_TOKEN", "explicit-store", 1));
    ASSERT_EQ(0, setenv("EXPECT_ARGS_MODE", "full", 1));
    ASSERT_EQ(0, setenv("EXPECT_STATE_ROOT", cwd, 1));

    tny_ctx ctx = {0};
    ctx.cwd = cwd;
    ctx.bridge_bin = fixture;
    ctx.library_mode = true;
    cursor_bridge_launch_options options = {
        .state_root = cwd,
        .local_store_json = "{\"type\":\"memory\"}",
        .store_callback_url = "http://127.0.0.1:7",
        .store_callback_token = "explicit-store",
    };
    cursor_bridge bridge;
    cursor_bridge_init(&bridge);
    static char err[512];
    memset(err, 0, sizeof err);
    ASSERTm(err, cursor_bridge_spawn(&bridge, &ctx, "explicit-api", &options, 2000, err,
                                     sizeof err) == 0);
    pid_t first_pid = bridge.pid;
    ASSERT(first_pid > 0);
    cursor_bridge_stop(&bridge, 0);
    ASSERT(process_group_gone(first_pid));

    ASSERT_STR_EQ("ambient-api", getenv("CURSOR_API_KEY"));
    ASSERT_STR_EQ("ambient-language", getenv("CURSOR_SDK_CLIENT_LANGUAGE"));
    ASSERT_STR_EQ("ambient-store", getenv("CURSOR_SDK_STORE_CALLBACK_AUTH_TOKEN"));
    ASSERT_EQ(0, setenv("EXPECT_API_KEY", "ambient-api", 1));
    ASSERT_EQ(0, setenv("EXPECT_STORE_MODE", "unset", 1));
    ASSERT_EQ(0, setenv("EXPECT_ARGS_MODE", "basic", 1));
    cursor_bridge_init(&bridge);
    memset(err, 0, sizeof err);
    ASSERTm(err, cursor_bridge_spawn(&bridge, &ctx, NULL, NULL, 2000, err, sizeof err) == 0);
    cursor_bridge_stop(&bridge, 0);
    ASSERT_EQ(baseline_fds, open_fd_count());

    ASSERT_EQ(0, unlink(fixture));
    ASSERT_EQ(0, rmdir(cwd));
    free(fixture);
    for (size_t i = 0; i < sizeof saved / sizeof saved[0]; i++) restore_env(&saved[i]);
    PASS();
}

TEST bridge_spawn_failures_release_process_and_pipe(void) {
    char root_template[] = "/tmp/tny-bridge-failure-XXXXXX";
    char *root = mkdtemp(root_template);
    ASSERT(root);
    char cwd[PATH_MAX];
    ASSERT(realpath(root, cwd));
    char *fixture = path_join(cwd, "not-ready");
    char *missing = path_join(cwd, "missing-bridge");
    ASSERT(fixture);
    ASSERT(missing);
    static const char exits_before_ready[] = "#!/bin/sh\n"
                                             "echo 'fixture refused readiness' >&2\n"
                                             "exit 23\n";
    ASSERT_EQ(0, file_write_atomic(fixture, exits_before_ready, sizeof exits_before_ready - 1));
    ASSERT_EQ(0, chmod(fixture, 0700));
    int baseline_fds = open_fd_count();

    tny_ctx ctx = {0};
    ctx.cwd = cwd;
    ctx.bridge_bin = missing;
    ctx.library_mode = true;
    cursor_bridge bridge;
    cursor_bridge_init(&bridge);
    char err[512] = {0};
    ASSERT_EQ(-1, cursor_bridge_spawn(&bridge, &ctx, "key", NULL, 1000, err, sizeof err));
    ASSERT(strstr(err, "cannot run") != NULL);
    ASSERT_EQ(0, bridge.pid);
    ASSERT_EQ(-1, bridge.err_fd);
    ASSERT_EQ(baseline_fds, open_fd_count());

    ctx.bridge_bin = fixture;
    memset(err, 0, sizeof err);
    ASSERT_EQ(-1, cursor_bridge_spawn(&bridge, &ctx, "key", NULL, 1000, err, sizeof err));
    ASSERT(strstr(err, "status 23") != NULL);
    ASSERT(strstr(err, "fixture refused readiness") != NULL);
    ASSERT_EQ(0, bridge.pid);
    ASSERT_EQ(-1, bridge.err_fd);
    ASSERT_EQ(baseline_fds, open_fd_count());

    cursor_bridge_stop(&bridge, 0);
    ASSERT_EQ(0, unlink(fixture));
    ASSERT_EQ(0, rmdir(cwd));
    free(missing);
    free(fixture);
    PASS();
}

TEST sdk_text_is_authoritative_over_interaction_and_step_restatements(void) {
    cu_impl o;
    rec_t r = {0};
    impl_init(&o, &r);
    feed(&o, "{\"sdkMessage\":{\"type\":\"assistant\",\"message\":{"
             "\"type\":\"assistant\",\"text\":\"answer\"}},"
             "\"interactionUpdate\":{\"type\":\"text_delta\","
             "\"update\":{\"text\":\"answer\"}},"
             "\"step\":{\"type\":\"assistant\",\"step\":{\"text\":\"answer\"}}}");
    ASSERT_EQ(1, r.n);
    ASSERT_STR_EQ("answer", r.text[0]);
    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST done_without_result_is_not_a_terminal_success(void) {
    cu_impl o;
    rec_t r = {0};
    impl_init(&o, &r);
    feed(&o, "{\"done\":{\"runId\":\"r1\",\"agentId\":\"a1\"}}");
    ASSERT_FALSE(o.ended);
    ASSERT(o.saw_done);
    ASSERT_FALSE(rec_has_kind(&r, TNY_EV_TURN_END));
    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST cancel_lost_race_preserves_finished_result(void) {
    cu_impl o;
    rec_t r = {0};
    impl_init(&o, &r);
    o.cancel_requested = true;
    o.cancel_attempted = true;
    feed(&o, "{\"result\":{\"runId\":\"r1\","
             "\"status\":\"RUN_LIFECYCLE_STATUS_FINISHED\",\"result\":{}}}");
    ASSERT_EQ(TNY_STOP_DONE, r.stop[r.n - 1]);
    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST observe_replay_scales_and_preserves_distinct_identical_events(void) {
    cu_impl o;
    rec_t r = {0};
    impl_init(&o, &r);
    o.stream_kind = CU_STREAM_SEND;
    char payload[160];
    for (int i = 0; i < 600; i++) {
        snprintf(payload, sizeof payload,
                 "{\"sdkMessage\":{\"type\":\"status\","
                 "\"message\":{\"text\":\"%d\"}}}",
                 i);
        ASSERT(cu_accept_frame(&o, payload, strlen(payload)));
    }
    ASSERT_EQ(600, (int)o.send_hash_count);
    o.stream_kind = CU_STREAM_OBSERVE;
    o.observe_replay = true;
    for (int i = 0; i < 600; i++) {
        snprintf(payload, sizeof payload,
                 "{\"sdkMessage\":{\"type\":\"status\","
                 "\"message\":{\"text\":\"%d\"}},"
                 "\"offset\":\"%d\"}",
                 i, i + 1);
        ASSERT_FALSE(cu_accept_frame(&o, payload, strlen(payload)));
    }
    ASSERT_FALSE(o.observe_replay);
    snprintf(payload, sizeof payload,
             "{\"sdkMessage\":{\"type\":\"status\","
             "\"message\":{\"text\":\"0\"}},"
             "\"offset\":\"601\"}");
    ASSERT(cu_accept_frame(&o, payload, strlen(payload)));
    rec_free(&r);
    impl_free(&o);
    PASS();
}

TEST cursor_images_enforce_count_and_encoded_request_limit(void) {
    char template[] = "/tmp/tny-cursor-image.XXXXXX";
    int fd = mkstemp(template);
    ASSERT(fd >= 0);
    unsigned char png[12] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a,
                             0x1a, 0x0a, 0x00, 0x00, 0x00, 0x00};
    ASSERT_EQ((int)sizeof png, (int)write(fd, png, sizeof png));
    close(fd);
    const char *one[] = {template, NULL};
    buf_t body;
    buf_init(&body);
    buf_appends(&body, "{\"message\":{}");
    char err[256];
    ASSERT_EQ(0, cu_append_images(&body, one, err, sizeof err));
    ASSERT(strstr(body.data, "image/png"));
    buf_free(&body);
    const char *many[18];
    for (int i = 0; i < 17; i++) many[i] = template;
    many[17] = NULL;
    buf_init(&body);
    ASSERT_EQ(-1, cu_append_images(&body, many, err, sizeof err));
    buf_free(&body);
    fd = open(template, O_WRONLY);
    ASSERT(fd >= 0);
    ASSERT_EQ(0, ftruncate(fd, 7 * 1024 * 1024));
    close(fd);
    buf_init(&body);
    ASSERT_EQ(-1, cu_append_images(&body, one, err, sizeof err));
    buf_free(&body);
    unlink(template);
    PASS();
}

TEST ephemeral_bridge_root_is_recursively_removed(void) {
    char err[256];
    char *root = cu_ephemeral_root_create(err, sizeof err);
    ASSERT(root);
    char *nested = path_join(root, "nested");
    ASSERT_EQ(0, mkdir_p(nested));
    char *file = path_join(nested, "state.sqlite");
    ASSERT_EQ(0, file_write_atomic(file, "state", 5));
    char saved[1024];
    snprintf(saved, sizeof saved, "%s", root);
    cu_ephemeral_root_remove(&root);
    ASSERT_EQ(NULL, root);
    ASSERT_FALSE(dir_exists(saved));
    free(file);
    free(nested);
    PASS();
}

TEST cancel_state_machine_distinguishes_inactive_unidentified_and_failed_rpc(void) {
    tny_ctx ctx = {0};
    tny_cursor_config cfg = {0};
    ctx.cursor_config = &cfg;
    ctx.library_mode = true;
    tny_backend *backend = tny_backend_cursor_new(&ctx);
    ASSERT(backend);
    cu_impl *o = backend->impl;
    rec_t r = {0};
    o->cb = rec_cb;
    o->ud = &r;

    backend->cancel(backend);
    ASSERT_FALSE(o->cancel_requested);
    ASSERT_FALSE(o->cancel_attempted);
    ASSERT_EQ(0, r.n);

    o->active = true;
    backend->cancel(backend);
    ASSERT(o->cancel_requested);
    ASSERT_FALSE(o->cancel_attempted);
    ASSERT_FALSE(o->cancel_sent);
    ASSERT_EQ(1, r.n);
    ASSERT_STR_EQ("cursor: waiting for the run id before cancelling", r.text[0]);

    o->cancel_requested = false;
    o->run_id = xstrdup("run-1");
    backend->cancel(backend);
    ASSERT(o->cancel_requested);
    ASSERT(o->cancel_attempted);
    ASSERT_FALSE(o->cancel_sent);
    ASSERT_EQ(2, r.n);
    ASSERT(r.text[1] && strstr(r.text[1], "capability"));
    backend->cancel(backend); /* terminal failure is never retry-stormed */
    ASSERT_EQ(2, r.n);

    rec_free(&r);
    backend->destroy(backend);
    PASS();
}

TEST session_pointer_omits_absent_offset_and_handles_missing_runtime_context(void) {
    tny_ctx ctx = {0};
    tny_cursor_config cfg = {0};
    ctx.cursor_config = &cfg;
    tny_backend *backend = tny_backend_cursor_new(&ctx);
    ASSERT(backend);
    cu_impl *o = backend->impl;
    o->agent_id = xstrdup("agent-no-offset");
    char *pointer = backend->session_pointer(backend);
    ASSERT(pointer);
    ASSERT_EQ(NULL, strstr(pointer, "after_offset"));
    ASSERT(strstr(pointer, "\"runtime\":\"local\""));
    free(pointer);

    o->ctx = NULL;
    pointer = backend->session_pointer(backend);
    ASSERT(pointer);
    ASSERT(strstr(pointer, "\"runtime\":\"auto\""));
    free(pointer);
    o->ctx = &ctx;
    ctx.cursor_config = NULL;
    pointer = backend->session_pointer(backend);
    ASSERT(pointer);
    ASSERT(strstr(pointer, "\"runtime\":\"auto\""));
    free(pointer);
    backend->destroy(backend);
    PASS();
}

TEST cursor_images_distinguish_empty_invalid_and_unreadable_inputs(void) {
    char empty[] = "/tmp/tny-cursor-empty.XXXXXX";
    int fd = mkstemp(empty);
    ASSERT(fd >= 0);
    close(fd);
    const char *empty_images[] = {empty, NULL};
    buf_t body;
    buf_init(&body);
    char err[256] = {0};
    ASSERT_EQ(-1, cu_append_images(&body, empty_images, err, sizeof err));
    ASSERT(strstr(err, "not a png/jpeg/gif/webp image"));
    buf_free(&body);
    unlink(empty);

    const char *missing_images[] = {"/tmp/tny-cursor-image-does-not-exist", NULL};
    buf_init(&body);
    err[0] = 0;
    ASSERT_EQ(-1, cu_append_images(&body, missing_images, err, sizeof err));
    ASSERT(strstr(err, "cannot read image"));
    buf_free(&body);
    PASS();
}

static int recovery_listener(int *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    socklen_t len = sizeof address;
    if (fd < 0 || bind(fd, (struct sockaddr *)&address, sizeof address) != 0 ||
        listen(fd, 1) != 0 || getsockname(fd, (struct sockaddr *)&address, &len) != 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    *port = ntohs(address.sin_port);
    return fd;
}

static bool request_has(const char *bytes, size_t len, const char *needle) {
    size_t needle_len = strlen(needle);
    if (needle_len > len) return false;
    for (size_t i = 0; i <= len - needle_len; i++)
        if (memcmp(bytes + i, needle, needle_len) == 0) return true;
    return false;
}

static int close_empty_observe(tny_backend *backend, int listener) {
    cu_impl *o = backend->impl;
    o->observe_retry_at_ms = 0;
    if (backend->dispatch(backend, NULL, 0) != 0) return -1;
    int client = accept(listener, NULL, NULL);
    if (client < 0) return -1;
    char request[4096];
    if (recv(client, request, sizeof request, 0) <= 0) {
        close(client);
        return -1;
    }
    static const char response[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    if (send(client, response, sizeof response - 1, 0) != (ssize_t)(sizeof response - 1)) {
        close(client);
        return -1;
    }
    close(client);
    for (int spin = 0; spin < 100; spin++) {
        struct pollfd fd = {cursor_sdk_stream_fd(&o->sdk), POLLIN, 0};
        if (fd.fd >= 0 && tny_poll(&fd, 1, 10) < 0) return -1;
        int rc = backend->dispatch(backend, fd.fd >= 0 ? &fd : NULL, fd.fd >= 0 ? 1 : 0);
        if (rc != 0 || o->observe_retry_pending || o->ended) return rc;
    }
    return -1;
}

TEST observe_recovery_request_uses_offsets_only_after_initial_send_recovery(void) {
    for (int initial = 0; initial < 2; initial++) {
        int port = 0;
        int listener = recovery_listener(&port);
        ASSERT(listener >= 0);
        tny_ctx ctx = {0};
        tny_cursor_config cfg = {0};
        ctx.cursor_config = &cfg;
        ctx.library_mode = true;
        tny_backend *backend = tny_backend_cursor_new(&ctx);
        ASSERT(backend);
        cu_impl *o = backend->impl;
        o->active = true;
        o->stream_kind = initial ? CU_STREAM_SEND : CU_STREAM_OBSERVE;
        o->run_id = xstrdup("run-recover");
        if (initial) o->observe_offset = xstrdup("must-not-be-used");
        o->sdk.negotiated = true;
        o->sdk.version.capability_count = 1;
        snprintf(o->sdk.version.capabilities[0], sizeof o->sdk.version.capabilities[0],
                 "run.observe");
        snprintf(o->sdk.stream.base_url, sizeof o->sdk.stream.base_url, "http://127.0.0.1:%d",
                 port);
        o->sdk.stream.state = CS_HEADERS; /* completed/dropped prior stream */
        ASSERT_EQ(0, backend->dispatch(backend, NULL, 0));
        if (!initial) {
            ASSERT(o->observe_retry_pending);
            o->observe_retry_at_ms = 0;
            ASSERT_EQ(0, backend->dispatch(backend, NULL, 0));
        }
        ASSERT_EQ(CU_STREAM_OBSERVE, o->stream_kind);
        ASSERT_EQ(initial != 0, o->observe_replay);
        int client = accept(listener, NULL, NULL);
        ASSERT(client >= 0);
        char request[4096] = {0};
        size_t got = 0;
        for (int spin = 0; spin < 100 && !request_has(request, got, "run-recover"); spin++) {
            struct pollfd pollfd = {client, POLLIN, 0};
            ASSERT(tny_poll(&pollfd, 1, 10) >= 0);
            if (!(pollfd.revents & POLLIN)) continue;
            ssize_t chunk = recv(client, request + got, sizeof request - 1 - got, 0);
            ASSERT(chunk > 0);
            got += (size_t)chunk;
            request[got] = 0;
        }
        ASSERT(request_has(request, got, "run-recover"));
        ASSERT_FALSE(request_has(request, got, "afterOffset"));
        close(client);
        close(listener);
        backend->destroy(backend);
    }
    PASS();
}

TEST observe_recovery_failure_is_terminal(void) {
    tny_ctx ctx = {0};
    tny_cursor_config cfg = {0};
    ctx.cursor_config = &cfg;
    ctx.library_mode = true;
    tny_backend *backend = tny_backend_cursor_new(&ctx);
    ASSERT(backend);
    cu_impl *o = backend->impl;
    char direct_err[256] = {0};
    ASSERT_EQ(-1, cu_start_observe(o, true, direct_err, sizeof direct_err));
    ASSERT(strstr(direct_err, "before it identified the run"));
    rec_t r = {0};
    o->cb = rec_cb;
    o->ud = &r;
    o->active = true;
    o->stream_kind = CU_STREAM_SEND;
    o->run_id = xstrdup("run-no-route");
    o->sdk.stream.state = CS_HEADERS;
    ASSERT_EQ(-1, backend->dispatch(backend, NULL, 0));
    ASSERT(o->ended);
    ASSERT_FALSE(o->observe_replay);
    ASSERT(rec_has_kind(&r, TNY_EV_ERROR));
    ASSERT_EQ(TNY_STOP_ERROR, r.stop[r.n - 1]);
    rec_free(&r);
    backend->destroy(backend);
    PASS();
}

TEST observe_recovery_exhausts_after_repeated_empty_streams(void) {
    int port = 0;
    int listener = recovery_listener(&port);
    ASSERT(listener >= 0);
    tny_ctx ctx = {0};
    tny_cursor_config cfg = {0};
    ctx.cursor_config = &cfg;
    ctx.library_mode = true;
    tny_backend *backend = tny_backend_cursor_new(&ctx);
    ASSERT(backend);
    cu_impl *o = backend->impl;
    rec_t r = {0};
    o->cb = rec_cb;
    o->ud = &r;
    o->active = true;
    o->stream_kind = CU_STREAM_OBSERVE;
    o->run_id = xstrdup("run-empty-observe");
    o->sdk.negotiated = true;
    o->sdk.version.capability_count = 1;
    snprintf(o->sdk.version.capabilities[0], sizeof o->sdk.version.capabilities[0], "run.observe");
    snprintf(o->sdk.stream.base_url, sizeof o->sdk.stream.base_url, "http://127.0.0.1:%d", port);
    o->sdk.stream.state = CS_HEADERS; /* first ObserveRun just closed */

    ASSERT_EQ(0, backend->dispatch(backend, NULL, 0));
    ASSERT(o->observe_retry_pending);
    ASSERT_EQ(1, (int)o->observe_no_progress_attempts);
    for (int attempt = 1; attempt <= 4; attempt++) {
        int rc = close_empty_observe(backend, listener);
        if (attempt < 4) {
            ASSERT_EQ(0, rc);
            ASSERT_FALSE(o->ended);
            ASSERT(o->observe_retry_pending);
            ASSERT_EQ(attempt + 1, (int)o->observe_no_progress_attempts);
        } else {
            ASSERT_EQ(-1, rc);
            ASSERT(o->ended);
            ASSERT_FALSE(o->observe_retry_pending);
        }
    }
    ASSERT(rec_has_kind(&r, TNY_EV_ERROR));
    ASSERT(r.text[r.n - 2] && strstr(r.text[r.n - 2], "without durable progress"));
    ASSERT_EQ(TNY_EV_TURN_END, r.kind[r.n - 1]);
    ASSERT_EQ(TNY_STOP_ERROR, r.stop[r.n - 1]);

    close(listener);
    rec_free(&r);
    backend->destroy(backend);
    PASS();
}

TEST observe_offset_progress_resets_recovery_budget(void) {
    tny_ctx ctx = {0};
    tny_cursor_config cfg = {0};
    ctx.cursor_config = &cfg;
    ctx.library_mode = true;
    tny_backend *backend = tny_backend_cursor_new(&ctx);
    ASSERT(backend);
    cu_impl *o = backend->impl;
    rec_t r = {0};
    o->cb = rec_cb;
    o->ud = &r;
    o->active = true;
    o->stream_kind = CU_STREAM_OBSERVE;
    o->run_id = xstrdup("run-progress");
    o->observe_progress_offset = xstrdup("offset:40");
    o->observe_offset = xstrdup("offset:41");
    o->observe_no_progress_attempts = 4;
    o->sdk.stream.state = CS_HEADERS;

    ASSERT_EQ(0, backend->dispatch(backend, NULL, 0));
    ASSERT_FALSE(o->ended);
    ASSERT(o->observe_retry_pending);
    ASSERT_EQ(1, (int)o->observe_no_progress_attempts);
    ASSERT(o->observe_retry_at_ms > 0);
    ASSERT_EQ(0, r.n);

    rec_free(&r);
    backend->destroy(backend);
    PASS();
}

TEST observe_retry_poll_timeout_is_bounded_and_expires_at_deadline(void) {
    tny_ctx ctx = {0};
    tny_cursor_config cfg = {0};
    ctx.cursor_config = &cfg;
    tny_backend *backend = tny_backend_cursor_new(&ctx);
    ASSERT(backend);
    cu_impl *o = backend->impl;
    ASSERT(backend->poll_timeout);
    ASSERT_EQ(-1, backend->poll_timeout(backend));
    o->observe_retry_pending = true;
    o->observe_retry_at_ms = now_ms() + 1000;
    int timeout = backend->poll_timeout(backend);
    ASSERT(timeout > 0);
    ASSERT(timeout <= 1000);
    o->observe_retry_at_ms = now_ms();
    ASSERT_EQ(0, backend->poll_timeout(backend));
    o->observe_retry_at_ms = now_ms() - 1;
    ASSERT_EQ(0, backend->poll_timeout(backend));
    o->observe_retry_pending = false;
    ASSERT_EQ(-1, backend->poll_timeout(backend));
    backend->destroy(backend);
    PASS();
}

TEST cancel_store_pump_start_failure_returns_error_directly(void) {
    char state_template[] = "/tmp/tny-cursor-cancel-pump.XXXXXX";
    char *state_dir = mkdtemp(state_template);
    ASSERT(state_dir);
    cursor_callbacks_options options = {
        .state_dir = state_dir,
        .enable_store = true,
        .thread_create = reject_cancel_pump_thread,
    };
    char err[256] = {0};
    cursor_callbacks *callbacks = cursor_callbacks_start(&options, err, sizeof err);
    ASSERTm(err, callbacks);

    cu_impl o = {0};
    o.callbacks = callbacks;
    o.run_id = xstrdup("run-cancel-pump");
    o.cancel_requested = true;
    ASSERT_EQ(-1, cu_send_cancel(&o, err, sizeof err));
    ASSERT(o.cancel_attempted);
    ASSERT_FALSE(o.cancel_sent);
    ASSERT(strstr(err, "could not start callback pump thread"));

    cursor_callbacks_destroy(&callbacks);
    free(o.run_id);
    char *store_dir = path_join(state_dir, "cursor-sdk-store");
    ASSERT(store_dir);
    ASSERT_EQ(0, rmdir(store_dir));
    free(store_dir);
    ASSERT_EQ(0, rmdir(state_dir));
    PASS();
}

TEST cancel_success_sets_terminal_attempt_state_once(void) {
    int port = 0;
    int listener = recovery_listener(&port);
    ASSERT(listener >= 0);
    pid_t child = fork();
    ASSERT(child >= 0);
    if (child == 0) {
        int client = accept(listener, NULL, NULL);
        char request[4096];
        if (client < 0 || recv(client, request, sizeof request, 0) <= 0) _exit(2);
        static const char response[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                       "Content-Length: 2\r\nConnection: close\r\n\r\n{}";
        if (send(client, response, sizeof response - 1, 0) != (ssize_t)(sizeof response - 1))
            _exit(3);
        close(client);
        close(listener);
        _exit(0);
    }
    tny_ctx ctx = {0};
    tny_cursor_config cfg = {0};
    ctx.cursor_config = &cfg;
    ctx.library_mode = true;
    tny_backend *backend = tny_backend_cursor_new(&ctx);
    ASSERT(backend);
    cu_impl *o = backend->impl;
    rec_t r = {0};
    o->cb = rec_cb;
    o->ud = &r;
    o->active = true;
    o->agent_id = xstrdup("agent-cancel");
    o->run_id = xstrdup("run-cancel");
    o->sdk.negotiated = true;
    o->sdk.version.capability_count = 1;
    snprintf(o->sdk.version.capabilities[0], sizeof o->sdk.version.capabilities[0], "run.cancel");
    snprintf(o->sdk.rpc.base_url, sizeof o->sdk.rpc.base_url, "http://127.0.0.1:%d", port);
    backend->cancel(backend);
    ASSERT(o->cancel_requested);
    ASSERT(o->cancel_attempted);
    ASSERT(o->cancel_sent);
    ASSERT_EQ(0, r.n);
    backend->cancel(backend);
    ASSERT(o->cancel_sent);
    ASSERT_EQ(0, r.n);
    int status = 0;
    ASSERT_EQ(child, waitpid(child, &status, 0));
    ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(listener);
    rec_free(&r);
    backend->destroy(backend);
    PASS();
}

TEST resume_pointer_absent_and_empty_offsets_clear_stale_offset(void) {
    const char *pointers[] = {
        "cursor-sdk.v1:{\"agent_id\":\"agent-resume\",\"runtime\":\"local\"}",
        "cursor-sdk.v1:{\"agent_id\":\"agent-resume\",\"after_offset\":\"\","
        "\"runtime\":\"local\"}",
    };
    for (size_t i = 0; i < sizeof pointers / sizeof pointers[0]; i++) {
        int port = 0;
        int listener = recovery_listener(&port);
        ASSERT(listener >= 0);
        pid_t child = fork();
        ASSERT(child >= 0);
        if (child == 0) {
            int client = accept(listener, NULL, NULL);
            char request[4096];
            if (client < 0 || recv(client, request, sizeof request, 0) <= 0) _exit(2);
            static const char response[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                           "Content-Length: 26\r\nConnection: close\r\n\r\n"
                                           "{\"agentId\":\"agent-resume\"}";
            if (send(client, response, sizeof response - 1, 0) != (ssize_t)(sizeof response - 1))
                _exit(3);
            close(client);
            close(listener);
            _exit(0);
        }
        tny_ctx ctx = {0};
        tny_cursor_config cfg = {0};
        cfg.agent_options_json = "{}";
        cfg.send_options_json = "{}";
        ctx.cursor_config = &cfg;
        ctx.cwd = "/tmp";
        ctx.provider_name = "cursor";
        ctx.api_key = "key";
        tny_backend *backend = tny_backend_cursor_new(&ctx);
        ASSERT(backend);
        cu_impl *o = backend->impl;
        o->model = xstrdup("model");
        o->observe_offset = xstrdup("stale-offset");
        o->sdk.negotiated = true;
        o->sdk.version.capability_count = 1;
        snprintf(o->sdk.version.capabilities[0], sizeof o->sdk.version.capabilities[0],
                 "agent.resume");
        snprintf(o->sdk.rpc.base_url, sizeof o->sdk.rpc.base_url, "http://127.0.0.1:%d", port);
        char err[256];
        ASSERTm(err, backend->create_or_resume(backend, pointers[i], err, sizeof err) == 0);
        ASSERT_EQ(NULL, o->observe_offset);
        ASSERT_STR_EQ("agent-resume", o->agent_id);
        int status = 0;
        ASSERT_EQ(child, waitpid(child, &status, 0));
        ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        close(listener);
        backend->destroy(backend);
    }
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
    RUN_TEST(result_cancelled_waits_for_and_maps_terminal_interruption);
    RUN_TEST(interaction_and_step_typed_structs_map_without_inner_type);
    RUN_TEST(usage_maps_charged_cents_to_normalized_dollars);
    RUN_TEST(observe_replay_deduplicates_send_prefix_and_tracks_observe_offset);
    RUN_TEST(unknown_result_status_fails_closed);
    RUN_TEST(send_resets_cancel_rpc_state_for_each_turn);
    RUN_TEST(session_pointer_is_versioned_and_carries_durable_run_state);
    RUN_TEST(bridge_rejects_partial_store_callback_credentials_before_spawn);
    RUN_TEST(bridge_ready_timeout_override_is_strict_and_clamped);
    RUN_TEST(bridge_spawn_sets_exact_cwd_argv_and_environment);
    RUN_TEST(bridge_spawn_failures_release_process_and_pipe);
    RUN_TEST(sdk_text_is_authoritative_over_interaction_and_step_restatements);
    RUN_TEST(done_without_result_is_not_a_terminal_success);
    RUN_TEST(cancel_lost_race_preserves_finished_result);
    RUN_TEST(observe_replay_scales_and_preserves_distinct_identical_events);
    RUN_TEST(cursor_images_enforce_count_and_encoded_request_limit);
    RUN_TEST(ephemeral_bridge_root_is_recursively_removed);
    RUN_TEST(cancel_state_machine_distinguishes_inactive_unidentified_and_failed_rpc);
    RUN_TEST(session_pointer_omits_absent_offset_and_handles_missing_runtime_context);
    RUN_TEST(cursor_images_distinguish_empty_invalid_and_unreadable_inputs);
    RUN_TEST(observe_recovery_request_uses_offsets_only_after_initial_send_recovery);
    RUN_TEST(observe_recovery_failure_is_terminal);
    RUN_TEST(observe_recovery_exhausts_after_repeated_empty_streams);
    RUN_TEST(observe_offset_progress_resets_recovery_budget);
    RUN_TEST(observe_retry_poll_timeout_is_bounded_and_expires_at_deadline);
    RUN_TEST(cancel_store_pump_start_failure_returns_error_directly);
    RUN_TEST(cancel_success_sets_terminal_attempt_state_once);
    RUN_TEST(resume_pointer_absent_and_empty_offsets_clear_stale_offset);
}
