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
    ASSERT_STR_EQ("{\"pa" "th\": \".\"}", cs.calls[0].args.data);
    oa_calls_reset(&cs);
    ASSERT_EQ(0, cs.n);
    PASS();
}

TEST parallel_calls_keyed_by_index(void) {
    oa_callset cs = {0};
    /* spec shape: id+name once per call, argument fragments interleaved and
     * keyed only by index */
    feed(&cs, "[{\"index\":0,\"id\":\"call_A\",\"function\":{\"name\":\"read_file\",\"arguments\":\"\"}}]");
    feed(&cs, "[{\"index\":1,\"id\":\"call_B\",\"function\":{\"name\":\"read_file\",\"arguments\":\"\"}}]");
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
    feed(&cs, "[{\"id\":\"a1\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"x\\\"}\"}},"
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
    feed(&cs, "[{\"id\":\"only\",\"function\":{\"name\":\"grep_files\",\"arguments\":\"{\\\"pat\"}}]");
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
    feed(&cs, "[{\"index\":1,\"id\":\"idB\",\"function\":{\"name\":\"grep_files\",\"arguments\":\"{\\\"pat\"}}]");
    /* a fragment carrying a KNOWN id must merge into that call even when its
     * index points at a different, id-less slot */
    feed(&cs, "[{\"index\":0,\"id\":\"idB\",\"function\":{\"arguments\":\"tern\\\":\\\"x\\\"}\"}}]");
    ASSERT_EQ(2, cs.n);
    ASSERT_EQ(NULL, cs.calls[0].id);            /* slot 0 must not steal idB */
    ASSERT_STR_EQ("{}", cs.calls[0].args.data);
    ASSERT_STR_EQ("idB", cs.calls[1].id);
    ASSERT_STR_EQ("{\"pattern\":\"x\"}", cs.calls[1].args.data);
    oa_calls_reset(&cs);
    PASS();
}

TEST first_streamed_name_sticks(void) {
    oa_callset cs = {0};
    feed(&cs, "[{\"index\":0,\"id\":\"n1\",\"function\":{\"name\":\"read_file\",\"arguments\":\"\"}}]");
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
        snprintf(frag, sizeof frag,
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
}
