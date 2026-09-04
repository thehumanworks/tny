/* test_openai.c — streamed tool_call assembly (src/backends/openai/toolcalls.c).
 *
 * The native loop must survive parallel tool calls in every streaming shape
 * seen in the wild. The regression that motivates this suite: a gateway
 * streamed three parallel calls but repeated "index" while giving each call
 * a fresh "id"; index-keyed assembly merged two calls, one call id vanished
 * from the transcript, and the provider rejected every later request with
 * HTTP 400 "no tool output found for function call …". */
#include "greatest.h"
#include "backends/openai/openai.h"

#include <stdlib.h>
#include <string.h>

/* Feed one delta's tool_calls array (JSON text) into the set. */
static void feed(oa_callset *cs, const char *tool_calls_json) {
    yyjson_doc *doc = jparse(tool_calls_json, strlen(tool_calls_json));
    if (doc) {
        oa_calls_feed(cs, yyjson_doc_get_root(doc));
        yyjson_doc_free(doc);
    }
}

TEST single_call_assembles_from_fragments(void) {
    oa_callset cs = {0};
    feed(&cs, "[{\"index\":0,\"id\":\"call_1\",\"type\":\"function\","
              "\"function\":{\"name\":\"list_files\",\"arguments\":\"\"}}]");
    feed(&cs, "[{\"index\":0,\"function\":{\"arguments\":\"{\\\"pa\"}}]");
    feed(&cs, "[{\"index\":0,\"function\":{\"arguments\":\"th\\\": \\\".\\\"}\"}}]");
    ASSERT_EQ(1, cs.n);
    ASSERT_STR_EQ("call_1", cs.calls[0].id);
    ASSERT_STR_EQ("list_files", cs.calls[0].name);
    ASSERT_STR_EQ("{\"pa"
                  "th\": \".\"}",
                  cs.calls[0].args.data);
    oa_calls_reset(&cs);
    ASSERT_EQ(0, cs.n);
    PASS();
}

TEST parallel_calls_keyed_by_index(void) {
    oa_callset cs = {0};
    /* spec shape: id+name once per call, argument fragments interleaved and
     * keyed only by index */
    feed(&cs, "[{\"index\":0,\"id\":\"call_A\",\"function\":{\"name\":\"read_file\",\"arguments\":"
              "\"\"}}]");
    feed(&cs, "[{\"index\":1,\"id\":\"call_B\",\"function\":{\"name\":\"read_file\",\"arguments\":"
              "\"\"}}]");
    feed(&cs, "[{\"index\":1,\"function\":{\"arguments\":\"{\\\"path\\\":\\\"b\\\"}\"}}]");
    feed(&cs, "[{\"index\":0,\"function\":{\"arguments\":\"{\\\"path\\\":\\\"a\\\"}\"}}]");
    ASSERT_EQ(2, cs.n);
    ASSERT_STR_EQ("call_A", cs.calls[0].id);
    ASSERT_STR_EQ("{\"path\":\"a\"}", cs.calls[0].args.data);
    ASSERT_STR_EQ("call_B", cs.calls[1].id);
    ASSERT_STR_EQ("{\"path\":\"b\"}", cs.calls[1].args.data);
    oa_calls_reset(&cs);
    PASS();
}

TEST parallel_calls_in_one_delta_array(void) {
    oa_callset cs = {0};
    /* non-stream fallback / whole-array shape: message.tool_calls complete,
     * no index members at all */
    feed(&cs, "[{\"id\":\"a1\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":"
              "\\\"x\\\"}\"}},"
              "{\"id\":\"a2\",\"function\":{\"name\":\"list_files\",\"arguments\":\"{}\"}}]");
    ASSERT_EQ(2, cs.n);
    ASSERT_STR_EQ("a1", cs.calls[0].id);
    ASSERT_STR_EQ("read_file", cs.calls[0].name);
    ASSERT_STR_EQ("a2", cs.calls[1].id);
    ASSERT_STR_EQ("list_files", cs.calls[1].name);
    oa_calls_reset(&cs);
    PASS();
}

/* The incident shape: the gateway reused index 1 for a third parallel call
 * that carried its own fresh id. Index-keyed assembly dropped id C and glued
 * its arguments onto call B ({"path":"…","path":"…"}). */
TEST fresh_id_on_repeated_index_starts_a_new_call(void) {
    oa_callset cs = {0};
    feed(&cs, "[{\"index\":0,\"id\":\"idA\",\"function\":{\"name\":\"list_files\","
              "\"arguments\":\"{\\\"path\\\":\\\"one\\\"}\"}}]");
    feed(&cs, "[{\"index\":1,\"id\":\"idB\",\"function\":{\"name\":\"list_files\","
              "\"arguments\":\"{\\\"path\\\":\\\"two\\\"}\"}}]");
    feed(&cs, "[{\"index\":1,\"id\":\"idC\",\"function\":{\"name\":\"read_file\","
              "\"arguments\":\"{\\\"path\\\":\\\"three\\\"}\"}}]");
    ASSERT_EQ(3, cs.n);
    ASSERT_STR_EQ("idB", cs.calls[1].id);
    ASSERT_STR_EQ("{\"path\":\"two\"}", cs.calls[1].args.data);
    ASSERT_STR_EQ("idC", cs.calls[2].id);
    ASSERT_STR_EQ("read_file", cs.calls[2].name);
    ASSERT_STR_EQ("{\"path\":\"three\"}", cs.calls[2].args.data);
    /* continuation fragments for the reused index belong to the newest call */
    feed(&cs, "[{\"index\":1,\"function\":{\"arguments\":\"\"}}]");
    ASSERT_EQ(3, cs.n);
    oa_calls_reset(&cs);
    PASS();
}

TEST fragments_without_id_or_index_go_to_last_call(void) {
    oa_callset cs = {0};
    feed(&cs,
         "[{\"id\":\"only\",\"function\":{\"name\":\"grep_files\",\"arguments\":\"{\\\"pat\"}}]");
    feed(&cs, "[{\"function\":{\"arguments\":\"tern\\\":\\\"x\\\"}\"}}]");
    ASSERT_EQ(1, cs.n);
    ASSERT_STR_EQ("{\"pattern\":\"x\"}", cs.calls[0].args.data);
    /* an orphan fragment before any call exists is dropped, not crashed on */
    oa_callset empty = {0};
    feed(&empty, "[{\"function\":{\"arguments\":\"zzz\"}}]");
    ASSERT_EQ(0, empty.n);
    oa_calls_reset(&cs);
    PASS();
}

TEST id_arriving_after_index_fragments_adopts_the_call(void) {
    oa_callset cs = {0};
    feed(&cs, "[{\"index\":0,\"function\":{\"name\":\"list_files\",\"arguments\":\"{\\\"pa\"}}]");
    feed(&cs, "[{\"index\":0,\"id\":\"late\",\"function\":{\"arguments\":\"th\\\":\\\".\\\"}\"}}]");
    ASSERT_EQ(1, cs.n);
    ASSERT_STR_EQ("late", cs.calls[0].id);
    ASSERT_STR_EQ("{\"path\":\".\"}", cs.calls[0].args.data);
    oa_calls_reset(&cs);
    PASS();
}

TEST known_id_wins_over_an_id_less_call_at_the_same_index(void) {
    oa_callset cs = {0};
    /* call 0 never got an id; call 1 owns "idB" */
    feed(&cs, "[{\"index\":0,\"function\":{\"name\":\"read_file\",\"arguments\":\"{}\"}}]");
    feed(&cs, "[{\"index\":1,\"id\":\"idB\",\"function\":{\"name\":\"grep_files\",\"arguments\":\"{"
              "\\\"pat\"}}]");
    /* a fragment carrying a KNOWN id must merge into that call even when its
     * index points at a different, id-less slot */
    feed(&cs,
         "[{\"index\":0,\"id\":\"idB\",\"function\":{\"arguments\":\"tern\\\":\\\"x\\\"}\"}}]");
    ASSERT_EQ(2, cs.n);
    ASSERT_EQ(NULL, cs.calls[0].id); /* slot 0 must not steal idB */
    ASSERT_STR_EQ("{}", cs.calls[0].args.data);
    ASSERT_STR_EQ("idB", cs.calls[1].id);
    ASSERT_STR_EQ("{\"pattern\":\"x\"}", cs.calls[1].args.data);
    oa_calls_reset(&cs);
    PASS();
}

TEST first_streamed_name_sticks(void) {
    oa_callset cs = {0};
    feed(&cs,
         "[{\"index\":0,\"id\":\"n1\",\"function\":{\"name\":\"read_file\",\"arguments\":\"\"}}]");
    /* a later chunk repeating (or garbling) the name must not overwrite it */
    feed(&cs, "[{\"index\":0,\"function\":{\"name\":\"write_file\",\"arguments\":\"{}\"}}]");
    ASSERT_EQ(1, cs.n);
    ASSERT_STR_EQ("read_file", cs.calls[0].name);
    oa_calls_reset(&cs);
    PASS();
}

TEST overflow_and_garbage_are_dropped_safely(void) {
    oa_callset cs = {0};
    char frag[128];
    for (int i = 0; i < OA_MAX_TOOL_CALLS + 4; i++) {
        snprintf(
            frag, sizeof frag,
            "[{\"index\":%d,\"id\":\"id%d\",\"function\":{\"name\":\"t\",\"arguments\":\"{}\"}}]",
            i, i);
        feed(&cs, frag);
    }
    ASSERT_EQ(OA_MAX_TOOL_CALLS, cs.n);
    /* negative index, non-array, empty id: all ignored */
    feed(&cs, "[{\"index\":-2,\"function\":{\"arguments\":\"x\"}}]");
    feed(&cs, "{\"index\":0}");
    feed(&cs, "[{\"index\":0,\"id\":\"\",\"function\":{\"arguments\":\"\"}}]");
    ASSERT_EQ(OA_MAX_TOOL_CALLS, cs.n);
    oa_calls_reset(&cs);
    PASS();
}

TEST fallback_ids_are_slot_unique(void) {
    oa_call with_id = {0}, without_id = {0};
    with_id.id = "prov_9";
    char b1[16], b2[16];
    ASSERT_STR_EQ("prov_9", oa_call_id(&with_id, 3, b1, sizeof b1));
    ASSERT_STR_EQ("call_0", oa_call_id(&without_id, 0, b1, sizeof b1));
    ASSERT_STR_EQ("call_7", oa_call_id(&without_id, 7, b2, sizeof b2));
    PASS();
}

/* ---- provider failure classification (docs/adr/0069) ---- */

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

/* ---- reasoning passthrough: OpenRouter reasoning_details fragments ---- */

static yyjson_doc *frag(const char *json) { return jparse(json, strlen(json)); }

TEST reasoning_details_merge_by_index(void) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, arr);
    yyjson_doc *f1 = frag("[{\"type\":\"reasoning.text\",\"index\":0,\"text\":\"think \","
                          "\"format\":\"anthropic-claude-v1\",\"signature\":null}]");
    yyjson_doc *f2 = frag("[{\"type\":\"reasoning.text\",\"index\":0,\"text\":\"hard\"}]");
    yyjson_doc *f3 = frag("[{\"type\":\"reasoning.text\",\"index\":0,\"text\":\"\","
                          "\"signature\":\"sig-1\"}]");
    /* a second block, and a fragment without an index, stay separate */
    yyjson_doc *f4 = frag("[{\"type\":\"reasoning.encrypted\",\"index\":1,\"data\":\"enc\"},"
                          "{\"type\":\"reasoning.summary\",\"summary\":\"loose\"}]");
    oa_reasoning_details_merge(d, arr, yyjson_doc_get_root(f1));
    oa_reasoning_details_merge(d, arr, yyjson_doc_get_root(f2));
    oa_reasoning_details_merge(d, arr, yyjson_doc_get_root(f3));
    oa_reasoning_details_merge(d, arr, yyjson_doc_get_root(f4));
    ASSERT_EQ_FMT(3, (int)yyjson_mut_arr_size(arr), "%d");
    yyjson_mut_val *i0 = yyjson_mut_arr_get(arr, 0);
    ASSERT_STR_EQ("think hard", yyjson_mut_get_str(yyjson_mut_obj_get(i0, "text")));
    ASSERT_STR_EQ("sig-1", yyjson_mut_get_str(yyjson_mut_obj_get(i0, "signature")));
    ASSERT_STR_EQ("anthropic-claude-v1", yyjson_mut_get_str(yyjson_mut_obj_get(i0, "format")));
    ASSERT_STR_EQ("reasoning.text", yyjson_mut_get_str(yyjson_mut_obj_get(i0, "type")));
    yyjson_mut_val *i1 = yyjson_mut_arr_get(arr, 1);
    ASSERT_STR_EQ("enc", yyjson_mut_get_str(yyjson_mut_obj_get(i1, "data")));
    yyjson_mut_val *i2 = yyjson_mut_arr_get(arr, 2);
    ASSERT_STR_EQ("loose", yyjson_mut_get_str(yyjson_mut_obj_get(i2, "summary")));
    /* garbage never merges */
    yyjson_doc *f5 = frag("[7,\"x\",null]");
    oa_reasoning_details_merge(d, arr, yyjson_doc_get_root(f5));
    oa_reasoning_details_merge(d, arr, NULL);
    ASSERT_EQ_FMT(3, (int)yyjson_mut_arr_size(arr), "%d");
    yyjson_doc_free(f1);
    yyjson_doc_free(f2);
    yyjson_doc_free(f3);
    yyjson_doc_free(f4);
    yyjson_doc_free(f5);
    yyjson_mut_doc_free(d);
    PASS();
}

SUITE(openai_suite) {
    RUN_TEST(single_call_assembles_from_fragments);
    RUN_TEST(parallel_calls_keyed_by_index);
    RUN_TEST(parallel_calls_in_one_delta_array);
    RUN_TEST(fresh_id_on_repeated_index_starts_a_new_call);
    RUN_TEST(fragments_without_id_or_index_go_to_last_call);
    RUN_TEST(id_arriving_after_index_fragments_adopts_the_call);
    RUN_TEST(known_id_wins_over_an_id_less_call_at_the_same_index);
    RUN_TEST(first_streamed_name_sticks);
    RUN_TEST(overflow_and_garbage_are_dropped_safely);
    RUN_TEST(fallback_ids_are_slot_unique);
    RUN_TEST(error_token_keeps_only_identifiers);
    RUN_TEST(retryable_statuses_and_permanent_tokens);
    RUN_TEST(reasoning_details_merge_by_index);
}
