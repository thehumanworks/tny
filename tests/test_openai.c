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
#include "backends/openai/stream_decode.h"
#include "backends/openai/turn_owner.h"
#include "backends/openai/request_owner.h"
#include "core/config.h"
#include "core/runtime.h"
#include "lib/custom_tools.h"
#include "util/alloc.h"
#include "core/image.h"
#include "core/image_manifest.h"
#include "core/perm.h"
#include "core/session.h"
#include "core/tools.h"
#include "net/http_server.h"
#include "util/tny_poll.h"
#include "util/util.h"
#include "util/alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef _WIN32
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#endif

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

/* ---- explicit image preview on a real tool batch (docs/adr/0096) ----
 *
 * A loopback HTTP provider records every request body, so the assertions are
 * made on the ACTUAL next outbound request rather than on internal state. The
 * model asks one safe `ask_user_question`; the frontend hook runs while that
 * tool call is executing, which is exactly when a generated-image preview
 * arrives over the control socket in production. */

#define PV_TOKEN "preview-fixture-token"

static const uint8_t PV_PNG_A[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 'f',
                                   'i',  'r',  's',  't',  '-',  'g',  'e',  'n'};
static const uint8_t PV_PNG_B[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 's',
                                   'e',  'c',  'o',  'n',  'd',  '-',  'g',  'e',  'n'};

static const char PV_TOOL_BODY[] =
    "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\",\"message\":{\"role\":"
    "\"assistant\",\"tool_calls\":[{\"index\":0,\"id\":\"call_preview\",\"type\":\"function\","
    "\"function\":{\"name\":\"ask_user_question\",\"arguments\":\"{\\\"question\\\":\\\"ready to "
    "look?\\\"}\"}}]}}]}";
static const char PV_DONE_BODY[] =
    "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\","
    "\"content\":\"looks good\"}}]}";

/* What the frontend hook does once it has queued (or tried to queue) a
 * preview, so the disposition tests can drive one real seam each. */
typedef enum {
    PV_HOOK_NOTHING = 0,
    PV_HOOK_TWO_GENERATIONS,  /* same output path, two different versions */
    PV_HOOK_DROP_CAPABILITY,  /* the provider is no longer configured-true */
    PV_HOOK_REFUSALS_THEN_OK, /* wrong hash and a path outside the roots first */
} pv_hook;

typedef struct {
    http_server *server;
    tny_backend *backend;
    tny_ctx *ctx;
    tny_session_state *session;
    perm_engine *perm;
    char root[600], workspace[640], state[640], png[700], outside[700];
    char hash_a[65], hash_b[65];
    int requests;
    int turn_requests;                  /* reset per turn: only its first POST asks for a tool */
    const char *first_body, *done_body; /* optional per-test response overrides */
    buf_t bodies[6];
    int hook_calls;
    pv_hook hook;
    bool selected; /* actual image_preview tool, not the queue probe hook */
    bool unlink_at_permission;
    int preview_permissions;
    bool stop_batch;   /* an extension stops the batch after the tools ran */
    int terminal_case; /* defensive terminal-path matrix, below */
    char *saved_session_dir;
    tny_image_preview_status status, status_second, status_refused;
    const char *code, *code_refused;
    int errors;
    buf_t error_text;
    int turn_ends;
    tny_stop_reason stop;
    bool ended;
    int decode_wire; /* 1: JSON, 2: SSE, 3: final SSE event without delimiter */
    bool cancel_decode;
    bool cancel_usage;
    int decoded_texts;
    int decoded_thinking;
    int terminal_during_decode;
    buf_t callback_text;
#ifdef TNY_ALLOC_TESTING
    size_t owner_baseline, owner_inside, owner_terminal;
    bool callback_owners_retained;
#endif
} pv_fixture;

static void pv_hash(const uint8_t *data, size_t len, char out[65]) {
    static const char *hex = "0123456789abcdef";
    uint8_t digest[32];
    sha256(data, len, digest);
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0xf];
    }
    out[64] = '\0';
}

static int pv_post(const http_server_request *request, http_server_response *response, void *ud) {
    pv_fixture *f = ud;
    int index = f->requests++;
    if (index < (int)(sizeof f->bodies / sizeof f->bodies[0]))
        buf_append(&f->bodies[index], request->body, request->body_len);
    /* the first POST of each turn asks for the tool; anything after it ends the
     * turn, so the script cannot be confused by transcript history */
    bool first_of_turn = f->turn_requests++ == 0;
    const char *body = first_of_turn ? PV_TOOL_BODY : PV_DONE_BODY;
    static const char two_tools[] =
        "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"role\":\"assistant\","
        "\"tool_calls\":[{\"id\":\"call_preview\",\"type\":\"function\",\"function\":{"
        "\"name\":\"ask_user_question\",\"arguments\":\"{\\\"question\\\":\\\"ready?\\\"}\"}},"
        "{\"id\":\"call_later\",\"type\":\"function\",\"function\":{\"name\":\"terminal\","
        "\"arguments\":\"{\\\"command\\\":\\\"printf harmless\\\"}\"}}]}}]}";
    if (first_of_turn && f->terminal_case >= 3) body = two_tools;
    static const char selected[] =
        "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"role\":\"assistant\","
        "\"tool_calls\":[{\"id\":\"call_preview\",\"type\":\"function\",\"function\":{"
        "\"name\":\"image_preview\",\"arguments\":\"{\\\"manifest\\\":\\\"selected.json\\\"}\"}}]}}"
        "]}";
    static const char selected_two[] =
        "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"role\":\"assistant\","
        "\"tool_calls\":[{\"id\":\"call_preview\",\"type\":\"function\",\"function\":{"
        "\"name\":\"image_preview\",\"arguments\":\"{\\\"manifest\\\":\\\"selected.json\\\"}\"}},"
        "{\"id\":\"call_later\",\"type\":\"function\",\"function\":{\"name\":\"terminal\","
        "\"arguments\":\"{\\\"command\\\":\\\"printf harmless\\\"}\"}}]}}]}";
    if (first_of_turn && f->selected) body = f->terminal_case >= 3 ? selected_two : selected;
    if (first_of_turn && f->first_body) body = f->first_body;
    if (!first_of_turn && f->done_body) body = f->done_body;
    static const char decode_json[] =
        "{\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":2},"
        "\"choices\":[{\"delta\":{\"content\":\"cancel-now\","
        "\"reasoning_content\":\"retained reasoning beyond short string storage\"},"
        "\"finish_reason\":\"stop\"}]}";
    static const char decode_sse[] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"cancel-now\","
        "\"reasoning_content\":\"retained reasoning beyond short string storage\"}}]}\n\n"
        "data: [DONE]\n\n";
    static const char decode_flush[] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"cancel-now\"},"
        "\"finish_reason\":\"stop\"}]}";
    if (f->decode_wire)
        body =
            f->decode_wire == 1 ? decode_json : (f->decode_wire == 2 ? decode_sse : decode_flush);
    response->status = 200;
    response->content_type = f->decode_wire > 1 ? "text/event-stream" : "application/json";
    response->body = body;
    response->body_len = strlen(body);
    return HTTP_SERVER_POST_HANDLED;
}

static void pv_event(const tny_backend_event *ev, void *ud) {
    pv_fixture *f = ud;
    if (ev->kind == TNY_EV_THINKING) f->decoded_thinking++;
    if (ev->kind == TNY_EV_USAGE && f->cancel_usage) {
        f->cancel_usage = false;
        f->backend->cancel(f->backend);
    }
    if (ev->kind == TNY_EV_TEXT_DELTA && f->decode_wire) {
        f->decoded_texts++;
        if (f->cancel_decode) {
            int ends = f->turn_ends;
#ifdef TNY_ALLOC_TESTING
            f->owner_inside = tny_alloc_test_owned_live();
#endif
            f->backend->cancel(f->backend);
#ifdef TNY_ALLOC_TESTING
            f->callback_owners_retained = tny_alloc_test_owned_live() == f->owner_inside;
#endif
            f->terminal_during_decode += f->turn_ends - ends;
        }
        /* Borrowed event bytes must remain usable until this callback returns. */
        buf_append(&f->callback_text, ev->text, ev->text_len);
    }
    if (ev->kind == TNY_EV_ERROR) {
        f->errors++;
        buf_append(&f->error_text, ev->text, ev->text_len);
        buf_appends(&f->error_text, "\n");
    }
    if (ev->kind == TNY_EV_TURN_END) {
#ifdef TNY_ALLOC_TESTING
        f->owner_terminal = tny_alloc_test_owned_live();
#endif
        f->turn_ends++;
        f->stop = ev->stop;
        f->ended = true;
    }
}

/* Runs inside the tool call, with the batch live. */
static char *pv_ask_user(const char *question, void *ud) {
    (void)question;
    pv_fixture *f = ud;
    f->hook_calls++;
    char err[256];
    f->code = NULL;
    if (f->hook == PV_HOOK_REFUSALS_THEN_OK) {
        f->status_refused = tny_backend_openai_queue_image_preview(
            f->backend, f->png, f->hash_b, 0, &f->code_refused, err, sizeof err);
        if (f->status_refused != TNY_IMAGE_PREVIEW_QUEUED) {
            const char *roots_code = NULL;
            tny_image_preview_status outside = tny_backend_openai_queue_image_preview(
                f->backend, f->outside, f->hash_a, 0, &roots_code, err, sizeof err);
            if (outside != TNY_IMAGE_PREVIEW_FAILED ||
                strcmp(roots_code, TNY_IMAGE_PREVIEW_CODE_ROOTS) != 0)
                f->status_refused = TNY_IMAGE_PREVIEW_QUEUED; /* fails the assertion below */
        }
    }
    f->status = tny_backend_openai_queue_image_preview(f->backend, f->png, f->hash_a, 0, &f->code,
                                                       err, sizeof err);
    if (f->hook == PV_HOOK_TWO_GENERATIONS) {
        /* the same output pathname is regenerated before the batch flushes */
        file_write_atomic(f->png, PV_PNG_B, sizeof PV_PNG_B);
        const char *code = NULL;
        f->status_second = tny_backend_openai_queue_image_preview(f->backend, f->png, f->hash_b, 0,
                                                                  &code, err, sizeof err);
    }
    if (f->hook == PV_HOOK_DROP_CAPABILITY)
        f->ctx->image_input = TNY_IMAGE_INPUT_UNKNOWN; /* a provider switch, as resolved */
    if (f->terminal_case == 1) f->ctx->max_steps = 1;
    if (f->terminal_case == 3) {
        f->saved_session_dir = f->session->dir;
        f->session->dir = xstrdup(f->png); /* a regular file cannot hold session.json */
    }
    if (f->terminal_case >= 4) f->ctx->perm_mode = TNY_MODE_ASK;
    return xstrdup("yes");
}

static void pv_control(const tny_openai_control_request *request,
                       tny_openai_control_response *response, void *ud) {
    pv_fixture *f = ud;
    if (f->selected && request->tool_name && strcmp(request->tool_name, "image_preview") == 0) {
        if (request->kind == TNY_OPENAI_CONTROL_PERMISSION) {
            f->preview_permissions++;
            response->permission = TNY_OPENAI_PERMISSION_ALLOW_ONCE;
            if (f->unlink_at_permission) {
                char path[750];
                snprintf(path, sizeof path, "%s/selected.json", f->workspace);
                unlink(path); /* execution must use the owned, approved selection */
            }
        }
        if (request->kind == TNY_OPENAI_CONTROL_POST_TOOL) {
            f->hook_calls++;
            if (f->terminal_case == 1) f->ctx->max_steps = 1;
            if (f->terminal_case == 3) {
                f->saved_session_dir = f->session->dir;
                f->session->dir = xstrdup(f->png);
            }
            if (f->terminal_case >= 4) f->ctx->perm_mode = TNY_MODE_ASK;
        }
    }
    if (f->stop_batch && request->kind == TNY_OPENAI_CONTROL_TOOL_BATCH) response->stop = true;
    if (f->terminal_case == 2 && f->hook_calls &&
        request->kind == TNY_OPENAI_CONTROL_PROVIDER_REQUEST)
        response->stop = true;
    if (f->terminal_case == 4 && request->kind == TNY_OPENAI_CONTROL_PERMISSION)
        response->permission = TNY_OPENAI_PERMISSION_DENY;
}

static void pv_open(pv_fixture *f) {
    memset(f, 0, sizeof *f);
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(f->root, sizeof f->root, "%s/tny-preview-XXXXXX", tmp);
    if (!mkdtemp(f->root)) abort();
    snprintf(f->workspace, sizeof f->workspace, "%s/ws", f->root);
    snprintf(f->state, sizeof f->state, "%s/state", f->root);
    mkdir_p(f->workspace);
    mkdir_p(f->state);
    snprintf(f->png, sizeof f->png, "%s/generated.png", f->workspace);
    snprintf(f->outside, sizeof f->outside, "%s/elsewhere.png", f->state);
    file_write_atomic(f->png, PV_PNG_A, sizeof PV_PNG_A);
    file_write_atomic(f->outside, PV_PNG_A, sizeof PV_PNG_A);
    pv_hash(PV_PNG_A, sizeof PV_PNG_A, f->hash_a);
    pv_hash(PV_PNG_B, sizeof PV_PNG_B, f->hash_b);
    buf_init(&f->error_text);
    for (size_t i = 0; i < sizeof f->bodies / sizeof f->bodies[0]; i++) buf_init(&f->bodies[i]);

    char error[256];
    f->server = http_server_start(PV_TOKEN, pv_post, f, error, sizeof error);
    if (!f->server) abort();
    f->ctx = tny_ctx_new_explicit(f->workspace, f->state);
    if (!f->ctx) abort();
    char url[256];
    snprintf(url, sizeof url, "%s/v1", http_server_url(f->server));
    free(f->ctx->base_url);
    f->ctx->base_url = xstrdup(url);
    free(f->ctx->api_key);
    f->ctx->api_key = xstrdup(PV_TOKEN);
    free(f->ctx->wire_api);
    f->ctx->wire_api = xstrdup("chat");
    f->ctx->perm_mode = TNY_MODE_YOLO;
    /* the native loop, not an embedder: the runtime tools this fixture drives
     * are the ones a real CLI turn has */
    f->ctx->library_mode = false;
    f->ctx->image_input = TNY_IMAGE_INPUT_CONFIGURED_SUPPORTED;
    f->session = session_new(f->ctx);
    f->perm = perm_new(f->ctx);
    f->backend = tny_backend_openai_new(f->ctx);
    if (!f->session || !f->perm || !f->backend) abort();
    tny_backend_openai_bind(f->backend, f->session, f->perm, NULL, NULL, pv_ask_user, f, NULL, NULL,
                            NULL, NULL, pv_control, f);
    if (f->backend->connect(f->backend, error, sizeof error) != 0) abort();
    if (f->backend->create_or_resume(f->backend, NULL, error, sizeof error) != 0) abort();
}

static void pv_close(pv_fixture *f) {
    f->backend->destroy(f->backend);
    perm_free(f->perm);
    session_close(f->session);
    tny_ctx_free(f->ctx);
    http_server_destroy(&f->server);
    buf_free(&f->error_text);
    buf_free(&f->callback_text);
    for (size_t i = 0; i < sizeof f->bodies / sizeof f->bodies[0]; i++) buf_free(&f->bodies[i]);
}

/* One turn, driven with the fixture server in the same poll set. */
static int pv_turn(pv_fixture *f, const char *prompt) {
    char error[512];
    f->ended = false;
    f->turn_requests = 0;
    if (f->backend->send(f->backend, prompt, NULL, pv_event, f, error, sizeof error) != 0)
        return -1;
    int64_t deadline = monotonic_ms() + 10000;
    while (!f->ended && monotonic_ms() < deadline) {
        struct pollfd fds[HTTP_SERVER_POLLFD_CAPACITY + TNY_BACKEND_POLLFD_MAX];
        int sn = http_server_pollfds(f->server, fds, HTTP_SERVER_POLLFD_CAPACITY);
        int bn = f->backend->pollfds(f->backend, fds + sn, TNY_BACKEND_POLLFD_MAX);
        tny_poll(fds, (nfds_t)(sn + bn), 10);
        http_server_dispatch(f->server, fds, sn);
        if (f->backend->dispatch(f->backend, fds + sn, bn) < 0) break;
        if (f->terminal_case == 5 && f->hook_calls && !f->ended)
            f->backend->cancel(f->backend); /* parked permission, not re-entrant */
    }
    return f->ended ? 0 : -1;
}

/* Every image_url payload in one recorded request body, decoded. */
static int pv_request_images(const buf_t *body, uint8_t out[4][64], size_t len_out[4]) {
    int found = 0;
    yyjson_doc *doc = body->len ? jparse(body->data, body->len) : NULL;
    if (!doc) return 0;
    yyjson_val *messages = jget(yyjson_doc_get_root(doc), "messages");
    size_t mi, mmax;
    yyjson_val *message;
    yyjson_arr_foreach(messages, mi, mmax, message) {
        yyjson_val *content = jget(message, "content");
        if (!yyjson_is_arr(content)) continue;
        size_t pi, pmax;
        yyjson_val *part;
        yyjson_arr_foreach(content, pi, pmax, part) {
            const char *type = jget_str(part, "type");
            if (!type || strcmp(type, "image_url") != 0 || found >= 4) continue;
            const char *url = jget_str(jget(part, "image_url"), "url");
            const char *prefix = "data:image/png;base64,";
            if (!url || strncmp(url, prefix, strlen(prefix)) != 0) continue;
            len_out[found] = b64_decode(url + strlen(prefix), out[found], 64);
            found++;
        }
    }
    yyjson_doc_free(doc);
    return found;
}

/* The accepted preview reaches the ACTUAL next request, exactly once, with the
 * bytes each generation had when it was queued — two generations of one output
 * pathname stay distinct — and refusals never enter the queue. */
TEST openai_preview_rides_the_next_request_with_captured_bytes(void) {
    pv_fixture f;
    pv_open(&f);
    f.hook = PV_HOOK_TWO_GENERATIONS;
    ASSERT_EQ(0, pv_turn(&f, "generate and review"));
    ASSERT_EQ(1, f.hook_calls);
    ASSERT_EQ(TNY_IMAGE_PREVIEW_QUEUED, f.status);
    ASSERT_EQ(TNY_IMAGE_PREVIEW_QUEUED, f.status_second);
    ASSERT_EQ(NULL, f.code);
    ASSERT_EQ(2, f.requests); /* the tool round, then the review request */
    ASSERT_EQ(TNY_STOP_DONE, f.stop);

    uint8_t decoded[4][64];
    size_t lengths[4] = {0};
    ASSERT_EQ(0, pv_request_images(&f.bodies[0], decoded, lengths)); /* no pixels before */
    ASSERT_EQ(2, pv_request_images(&f.bodies[1], decoded, lengths));
    ASSERT_EQ_FMT(sizeof PV_PNG_A, lengths[0], "%zu");
    ASSERT_EQ_FMT(sizeof PV_PNG_B, lengths[1], "%zu");
    ASSERT_MEM_EQ(PV_PNG_A, decoded[0], sizeof PV_PNG_A);
    ASSERT_MEM_EQ(PV_PNG_B, decoded[1], sizeof PV_PNG_B);
    ASSERT(
        strstr(f.bodies[1].data, "Images queued by explicitly requested generation/edit preview."));

    /* a second turn carries no new pixels: nothing stale was retained */
    f.hook = PV_HOOK_NOTHING;
    f.ctx->image_input = TNY_IMAGE_INPUT_UNKNOWN; /* and no new preview may queue */
    ASSERT_EQ(0, pv_turn(&f, "anything else"));
    ASSERT_EQ(TNY_IMAGE_PREVIEW_UNSUPPORTED, f.status);
    ASSERT(f.code);
    ASSERT_STR_EQ(TNY_IMAGE_PREVIEW_CODE_CAPABILITY, f.code);
    ASSERT_EQ(4, f.requests);
    ASSERT_EQ(2, pv_request_images(&f.bodies[3], decoded, lengths)); /* only the history */
    pv_close(&f);
    PASS();
}

/* Hash and root refusals answer with a status and a safe code, and the
 * following valid admission still works. */
TEST openai_preview_refuses_wrong_hash_and_foreign_roots(void) {
    pv_fixture f;
    pv_open(&f);
    f.hook = PV_HOOK_REFUSALS_THEN_OK;
    ASSERT_EQ(0, pv_turn(&f, "review it"));
    ASSERT_EQ(TNY_IMAGE_PREVIEW_FAILED, f.status_refused);
    ASSERT(f.code_refused);
    ASSERT_STR_EQ(TNY_IMAGE_PREVIEW_CODE_HASH, f.code_refused);
    ASSERT_EQ(TNY_IMAGE_PREVIEW_QUEUED, f.status);
    uint8_t decoded[4][64];
    size_t lengths[4] = {0};
    ASSERT_EQ(1, pv_request_images(&f.bodies[1], decoded, lengths));
    ASSERT_MEM_EQ(PV_PNG_A, decoded[0], sizeof PV_PNG_A);
    pv_close(&f);
    PASS();
}

/* The provider this turn would post to stops being configured-true between
 * admission and flush: the batch is preserved, then the owner reports
 * IMAGE_PREVIEW_NOT_DELIVERED, makes NO further request, ends the turn failed
 * and releases the bytes — a later allowed turn starts clean. */
TEST openai_preview_fatal_flush_stops_the_next_request(void) {
    pv_fixture f;
    pv_open(&f);
    f.hook = PV_HOOK_DROP_CAPABILITY;
    ASSERT_EQ(0, pv_turn(&f, "generate and review"));
    ASSERT_EQ(TNY_IMAGE_PREVIEW_QUEUED, f.status);
    ASSERT_EQ(1, f.requests); /* the tool round only: no POST after the failure */
    ASSERT_EQ(TNY_STOP_ERROR, f.stop);
    ASSERT(f.error_text.len);
    ASSERT(strstr(f.error_text.data, TNY_IMAGE_PREVIEW_NOT_DELIVERED));
    uint8_t decoded[4][64];
    size_t lengths[4] = {0};
    ASSERT_EQ(0, pv_request_images(&f.bodies[0], decoded, lengths));

    /* the same session, allowed again: no stale preview bytes reappear */
    f.hook = PV_HOOK_NOTHING;
    f.ctx->image_input = TNY_IMAGE_INPUT_CONFIGURED_SUPPORTED;
    buf_clear(&f.error_text);
    ASSERT_EQ(0, pv_turn(&f, "try again"));
    ASSERT_EQ(3, f.requests);
    ASSERT_EQ(TNY_IMAGE_PREVIEW_QUEUED, f.status);
    ASSERT_EQ(1, pv_request_images(&f.bodies[2], decoded, lengths)); /* this turn's preview only */
    ASSERT_EQ_FMT(sizeof PV_PNG_A, lengths[0], "%zu");
    pv_close(&f);
    PASS();
}

/* A turn that ends without another request still tells the truth about the
 * preview it accepted, and keeps nothing pending. */
TEST openai_preview_reports_non_delivery_when_the_turn_ends_early(void) {
    const tny_stop_reason stops[] = {TNY_STOP_INTERRUPTED, TNY_STOP_STEP_LIMIT,
                                     TNY_STOP_INTERRUPTED, TNY_STOP_ERROR,
                                     TNY_STOP_DENIED,      TNY_STOP_INTERRUPTED};
    for (int scenario = 0; scenario < 6; scenario++) {
        pv_fixture f;
        pv_open(&f);
        f.terminal_case = scenario;
        f.stop_batch = scenario == 0;
        ASSERT_EQ(0, pv_turn(&f, "generate and review"));
        ASSERT_EQ(TNY_IMAGE_PREVIEW_QUEUED, f.status);
        ASSERT_EQ(1, f.requests);
        ASSERT_EQ(stops[scenario], f.stop);
        ASSERT(f.error_text.len);
        ASSERT(strstr(f.error_text.data, TNY_IMAGE_PREVIEW_NOT_DELIVERED));
        if (f.saved_session_dir) {
            free(f.session->dir);
            f.session->dir = f.saved_session_dir;
        }
        /* Recover in the SAME session, with a different image. Both outbound
         * requests must be free of A, not just free of pending queue entries. */
        f.terminal_case = 0;
        f.stop_batch = false;
        f.ctx->max_steps = 0;
        f.ctx->perm_mode = TNY_MODE_YOLO;
        f.hook = PV_HOOK_NOTHING;
        ASSERT_EQ(0, file_write_atomic(f.png, PV_PNG_B, sizeof PV_PNG_B));
        memcpy(f.hash_a, f.hash_b, sizeof f.hash_a);
        ASSERT_EQ(0, pv_turn(&f, "fresh review"));
        ASSERT_EQ(3, f.requests);
        uint8_t decoded[4][64];
        size_t lengths[4] = {0};
        ASSERT_EQ(0, pv_request_images(&f.bodies[1], decoded, lengths));
        ASSERT_EQ(1, pv_request_images(&f.bodies[2], decoded, lengths));
        ASSERT_EQ_FMT(sizeof PV_PNG_B, lengths[0], "%zu");
        ASSERT_MEM_EQ(PV_PNG_B, decoded[0], sizeof PV_PNG_B);
        pv_close(&f);
    }
    PASS();
}

/* Readiness, not mere activity: an idle or completed session refuses, and so
 * does a live batch with no request left in its step budget. */
/* A real manifest fixture, describing the exact bytes in this private
 * workspace. No resolver stub and no queue helper stands in for the tool. */
static int pv_selected_record(pv_fixture *f) {
    tny_image_record record = {.operation_id = "0123456789abcdef",
                               .status = "succeeded",
                               .workspace = f->workspace,
                               .started = "2026-09-12T00:00:00Z",
                               .finished = "2026-09-12T00:00:01Z",
                               .prompt = "fixture",
                               .output = f->png,
                               .committed = true,
                               .requested_provider = "codex",
                               .effective_provider = "codex",
                               .requested_size = "auto",
                               .effective_size = "auto",
                               .size_status = "auto",
                               .output_sha256 = f->hash_a,
                               .mime = "image/png",
                               .bytes = sizeof PV_PNG_A};
    buf_t json;
    buf_init(&json);
    tny_image_manifest_serialize(&record, &json);
    char path[750];
    snprintf(path, sizeof path, "%s/selected.json", f->workspace);
    int rc = json.oom ? -1 : file_write_atomic(path, json.data, json.len);
    buf_free(&json);
    return rc;
}

TEST openai_selected_preview_allow_once_pins_before_permission(void) {
    pv_fixture f;
    pv_open(&f);
    f.selected = true;
    f.unlink_at_permission = true;
    f.ctx->perm_mode = TNY_MODE_ASK;
    for (int turn = 0; turn < 2; turn++) {
        ASSERT_EQ(0, pv_selected_record(&f));
        ASSERT_EQ(0, pv_turn(&f, "explicit selected preview"));
        /* The permission callback unlinks the record; no second read is allowed. */
        ASSERT_EQ(turn + 1, f.preview_permissions); /* not a remembered grant */
        ASSERT_EQ(2 * (turn + 1), f.requests);
        ASSERT_EQ(TNY_STOP_DONE, f.stop);
        uint8_t decoded[4][64];
        size_t lengths[4] = {0};
        ASSERT(pv_request_images(&f.bodies[f.requests - 1], decoded, lengths) > 0);
        ASSERT_MEM_EQ(PV_PNG_A, decoded[0], sizeof PV_PNG_A);
        ASSERT(strstr(f.bodies[f.requests - 1].data, "queued"));
    }
    pv_close(&f);
    PASS();
}

TEST openai_selected_preview_terminal_cleanup_and_recovery(void) {
    const tny_stop_reason stops[] = {TNY_STOP_INTERRUPTED, TNY_STOP_STEP_LIMIT,
                                     TNY_STOP_INTERRUPTED, TNY_STOP_ERROR,
                                     TNY_STOP_DENIED,      TNY_STOP_INTERRUPTED};
    for (int scenario = 0; scenario < 6; scenario++) {
        pv_fixture f;
        pv_open(&f);
        f.selected = true;
        f.terminal_case = scenario;
        f.stop_batch = scenario == 0;
        ASSERT_EQ(0, pv_selected_record(&f));
        ASSERT_EQ(0, pv_turn(&f, "explicit selected preview"));
        ASSERT_EQ(1, f.requests);
        ASSERT_EQ(stops[scenario], f.stop);
        ASSERT(f.error_text.data && strstr(f.error_text.data, TNY_IMAGE_PREVIEW_NOT_DELIVERED));
        if (f.saved_session_dir) {
            free(f.session->dir);
            f.session->dir = f.saved_session_dir;
            f.saved_session_dir = NULL;
        }
        f.terminal_case = 0;
        f.stop_batch = false;
        f.selected = false; /* next turn uses the existing independent queue fixture */
        f.ctx->max_steps = 0;
        f.ctx->perm_mode = TNY_MODE_YOLO;
        ASSERT_EQ(0, file_write_atomic(f.png, PV_PNG_B, sizeof PV_PNG_B));
        memcpy(f.hash_a, f.hash_b, sizeof f.hash_a);
        ASSERT_EQ(0, pv_turn(&f, "fresh turn"));
        ASSERT_EQ(3, f.requests);
        uint8_t decoded[4][64];
        size_t lengths[4] = {0};
        ASSERT_EQ(0, pv_request_images(&f.bodies[1], decoded, lengths));
        ASSERT_EQ(1, pv_request_images(&f.bodies[2], decoded, lengths));
        ASSERT_MEM_EQ(PV_PNG_B, decoded[0], sizeof PV_PNG_B);
        pv_close(&f);
    }
    PASS();
}

TEST openai_preview_needs_a_continuable_tool_batch(void) {
    pv_fixture f;
    pv_open(&f);
    char err[256];
    const char *code = NULL;
    ASSERT_EQ(TNY_IMAGE_PREVIEW_TURN_NOT_READY,
              tny_backend_openai_queue_image_preview(f.backend, f.png, f.hash_a, 0, &code, err,
                                                     sizeof err));
    ASSERT(code);
    ASSERT_STR_EQ(TNY_IMAGE_PREVIEW_CODE_NOT_READY, code);

    /* one step only: the batch that would carry it has no next request */
    f.ctx->max_steps = 1;
    ASSERT_EQ(0, pv_turn(&f, "review it"));
    ASSERT_EQ(1, f.hook_calls);
    ASSERT_EQ(TNY_IMAGE_PREVIEW_TURN_NOT_READY, f.status);
    ASSERT(f.code);
    ASSERT_STR_EQ(TNY_IMAGE_PREVIEW_CODE_NOT_READY, f.code);
    ASSERT_EQ(1, f.requests);
    ASSERT_EQ(TNY_STOP_STEP_LIMIT, f.stop);
    uint8_t decoded[4][64];
    size_t lengths[4] = {0};
    ASSERT_EQ(0, pv_request_images(&f.bodies[0], decoded, lengths));

    /* the completed session is idle again */
    code = NULL;
    ASSERT_EQ(TNY_IMAGE_PREVIEW_TURN_NOT_READY,
              tny_backend_openai_queue_image_preview(f.backend, f.png, f.hash_a, 0, &code, err,
                                                     sizeof err));
    ASSERT(code);
    ASSERT_STR_EQ(TNY_IMAGE_PREVIEW_CODE_NOT_READY, code);

    /* and a non-native backend handle is never a session for this */
    tny_backend other = {0};
    other.id = TNY_BK_ACP;
    ASSERT_EQ(
        TNY_IMAGE_PREVIEW_UNAVAILABLE_SESSION,
        tny_backend_openai_queue_image_preview(&other, f.png, f.hash_a, 0, &code, err, sizeof err));
    ASSERT(code);
    ASSERT_STR_EQ(TNY_IMAGE_PREVIEW_CODE_NO_SESSION, code);
    pv_close(&f);
    PASS();
}

/* ---- stream completion contract (docs/verification/stream-interruption.md) ---- */

/* SI-1: only a terminal event completes a stream; on the chat wire a
 * finish_reason stands in for gateways that never send [DONE]. */
TEST stream_complete_needs_a_terminal_event(void) {
    ASSERT(oa_stream_complete(true, false, ""));
    ASSERT(oa_stream_complete(true, true, ""));
    ASSERT_FALSE(oa_stream_complete(false, false, ""));
    ASSERT_FALSE(oa_stream_complete(false, false, NULL));
    /* responses wire: a finish_reason is never set, and would not count */
    ASSERT_FALSE(oa_stream_complete(false, false, "stop"));
    ASSERT(oa_stream_complete(false, true, "stop"));
    ASSERT(oa_stream_complete(false, true, "tool_calls"));
    ASSERT_FALSE(oa_stream_complete(false, true, ""));
    ASSERT_FALSE(oa_stream_complete(false, true, NULL));
    PASS();
}

/* SI-5: the stall window defaults to 300s, 0 or negative disables it, and
 * nothing larger than an hour is accepted. */
TEST stall_window_parses_and_clamps(void) {
    ASSERT_EQ(300, oa_stall_secs(NULL));
    ASSERT_EQ(300, oa_stall_secs(""));
    ASSERT_EQ(1, oa_stall_secs("1"));
    ASSERT_EQ(45, oa_stall_secs("45"));
    ASSERT_EQ(0, oa_stall_secs("0"));
    ASSERT_EQ(0, oa_stall_secs("-5"));
    ASSERT_EQ(0, oa_stall_secs("junk"));
    ASSERT_EQ(3600, oa_stall_secs("3600"));
    ASSERT_EQ(3600, oa_stall_secs("99999"));
    PASS();
}

/* SI-3/SI-4: the continuation request trails the shown partial as an
 * assistant message and one user turn, on both wires, verbatim. */
TEST continuation_trails_partial_then_user_turn(void) {
    yyjson_mut_doc *view = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *msgs = yyjson_mut_arr(view);
    yyjson_mut_doc_set_root(view, msgs);
    yyjson_mut_val *user = yyjson_mut_obj(view);
    yyjson_mut_obj_put(user, yyjson_mut_str(view, "role"), yyjson_mut_str(view, "user"));
    yyjson_mut_obj_put(user, yyjson_mut_str(view, "content"), yyjson_mut_str(view, "hello"));
    yyjson_mut_arr_add_val(msgs, user);

    const char *partial = "The workspace contains 3 entries; the first is cal";
    oa_view_append_continuation(view, partial);
    ASSERT_EQ(3, (int)yyjson_mut_arr_size(msgs));
    yyjson_mut_val *a = yyjson_mut_arr_get(msgs, 1);
    yyjson_mut_val *u = yyjson_mut_arr_get(msgs, 2);
    ASSERT_STR_EQ("assistant", yyjson_mut_get_str(yyjson_mut_obj_get(a, "role")));
    ASSERT_STR_EQ(partial, yyjson_mut_get_str(yyjson_mut_obj_get(a, "content")));
    ASSERT_STR_EQ("user", yyjson_mut_get_str(yyjson_mut_obj_get(u, "role")));
    const char *nudge = yyjson_mut_get_str(yyjson_mut_obj_get(u, "content"));
    ASSERT(nudge && strstr(nudge, "Continue from precisely where it stopped"));
    ASSERT(strstr(nudge, "do not repeat"));

    /* the responses wire translates the pair into two message items in order */
    char *input = tny_openai_responses_input_with_summary(msgs, NULL);
    ASSERT(input);
    yyjson_doc *doc = jparse(input, strlen(input));
    ASSERT(doc);
    yyjson_val *items = yyjson_doc_get_root(doc);
    ASSERT_EQ(3, (int)yyjson_arr_size(items));
    yyjson_val *ia = yyjson_arr_get(items, 1), *iu = yyjson_arr_get(items, 2);
    ASSERT_STR_EQ("assistant", jget_str(ia, "role"));
    ASSERT_STR_EQ(partial, jget_str(ia, "content"));
    ASSERT_STR_EQ("user", jget_str(iu, "role"));
    ASSERT_STR_EQ(nudge, jget_str(iu, "content"));
    yyjson_doc_free(doc);
    free(input);

    /* nothing shown: nothing appended (a plain retry carries no pair) */
    oa_view_append_continuation(view, "");
    oa_view_append_continuation(view, NULL);
    oa_view_append_continuation(NULL, partial);
    ASSERT_EQ(3, (int)yyjson_mut_arr_size(msgs));
    yyjson_mut_doc_free(view);
    PASS();
}

typedef struct {
    oa_decoder decoder;
    oa_callset calls;
    bool chat;
    int status, done, failed;
    tny_stop_reason stop;
    buf_t text, thinking;
} decode_capture;

static int decoded_collect(const oa_decoded_event *event, void *ud) {
    decode_capture *c = ud;
    switch (event->kind) {
    case OA_DECODE_TEXT: buf_append(&c->text, event->text, event->len); break;
    case OA_DECODE_THINKING: buf_append(&c->thinking, event->text, event->len); break;
    case OA_DECODE_DONE: c->done++; break;
    case OA_DECODE_ERROR: c->failed++; break;
    case OA_DECODE_FINISH: c->stop = event->stop; break;
    default: break;
    }
    return c->text.oom || c->thinking.oom ? TNY_PARSE_OOM : TNY_PARSE_OK;
}
static void decoded_sse(const char *data, size_t len, void *ud) {
    decode_capture *c = ud;
    int rc = oa_decoder_feed(&c->decoder, &c->calls, c->chat, true, data, len, decoded_collect, c);
    if (rc != TNY_PARSE_OK) c->status = rc;
}
static void decoded_free(decode_capture *c) {
    oa_decoder_reset(&c->decoder);
    oa_calls_reset(&c->calls);
    buf_free(&c->text);
    buf_free(&c->thinking);
}
TEST provider_decoding_every_split(void) {
    const char *wire[] = {
        ": comment\r\ndata: "
        "{\"choices\":[{\"delta\":{\"content\":\"h\u00e9\u4e16\u754c\",\"reasoning_content\":"
        "\"thinking\",\"tool_calls\":[{\"index\":0,\"function\":{\"name\":\"read_file\","
        "\"arguments\":\"{\"}}]}}]}\r\n\r\n"
        "data: "
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"late\",\"function\":{"
        "\"arguments\":\"}\"}}]},\"finish_reason\":\"length\"}]}\n\n"
        "data: [DONE]",
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"h\u00e9\u4e16\u754c\"}\n\n"
        "data: "
        "{\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"type\":\"function_"
        "call\",\"name\":\"read_file\"}}\n\n"
        "data: "
        "{\"type\":\"response.function_call_arguments.delta\",\"output_index\":0,\"delta\":"
        "\"broken prefix\"}\n\n"
        "data: "
        "{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"type\":\"function_"
        "call\",\"call_id\":\"late\",\"arguments\":\"{}\"}}\n\n"
        "data: "
        "{\"type\":\"response.incomplete\",\"response\":{\"incomplete_details\":{\"reason\":\"max_"
        "output_tokens\"}}}"};
    for (size_t w = 0; w < 2; w++) {
        size_t len = strlen(wire[w]);
        /* split==len+1 exercises the byte-at-a-time path too. */
        for (size_t split = 0; split <= len + 1; split++) {
            decode_capture c = {0};
            c.chat = w == 0;
            sse_parser parser;
            sse_parser_init(&parser);
            if (split <= len) {
                ASSERT_EQ(TNY_PARSE_OK, sse_feed(&parser, wire[w], split, decoded_sse, &c));
                ASSERT_EQ(TNY_PARSE_OK,
                          sse_feed(&parser, wire[w] + split, len - split, decoded_sse, &c));
            } else {
                for (size_t i = 0; i < len; i++)
                    ASSERT_EQ(TNY_PARSE_OK, sse_feed(&parser, wire[w] + i, 1, decoded_sse, &c));
            }
            ASSERT_EQ(TNY_PARSE_OK, sse_flush(&parser, decoded_sse, &c));
            ASSERT_EQ(TNY_PARSE_OK, c.status);
            ASSERT_EQ(1, c.done);
            ASSERT_EQ(0, c.failed);
            ASSERT_EQ(TNY_STOP_STEP_LIMIT, c.stop);
            ASSERT_STR_EQ("h\u00e9\u4e16\u754c", c.text.data);
            ASSERT_EQ(1, c.calls.n);
            ASSERT_STR_EQ("late", c.calls.calls[0].id);
            ASSERT_STR_EQ("read_file", c.calls.calls[0].name);
            ASSERT_STR_EQ("{}", c.calls.calls[0].args.data);
            if (c.chat) ASSERT_STR_EQ("thinking", c.thinking.data);
            sse_parser_free(&parser);
            decoded_free(&c);
        }
    }
    PASS();
}
TEST responses_reasoning_owns_unknown_fields(void) {
    const char *event =
        "{\"type\":\"response.output_item.done\",\"item\":{\"type\":\"reasoning\",\"id\":\"r\","
        "\"encrypted_content\":\"signed\",\"status\":\"completed\",\"future\":{\"key\":"
        "\"retained\"}}}";
    decode_capture c = {0};
    ASSERT_EQ(TNY_PARSE_OK, oa_decoder_feed(&c.decoder, &c.calls, false, false, event,
                                            strlen(event), decoded_collect, &c));
    char *extras = NULL;
    ASSERT_EQ(TNY_PARSE_OK, oa_decoder_extras(&c.decoder, &extras));
    ASSERT(extras);
    yyjson_doc *doc = jparse(extras, strlen(extras));
    ASSERT(doc);
    yyjson_val *item = yyjson_arr_get_first(jget(yyjson_doc_get_root(doc), "reasoning_items"));
    ASSERT_STR_EQ("retained", jget_str(jget(item, "future"), "key"));
    ASSERT_STR_EQ("signed", jget_str(item, "encrypted_content"));
    ASSERT_EQ(NULL, jget(item, "status"));
    yyjson_doc_free(doc);
    free(extras);
    decoded_free(&c);
    PASS();
}

/* The C suite calls the separately C++-compiled ownership checks. */
TEST cancellation_inside_decode_preserves_callback_and_reuse(void) {
    for (int wire = 1; wire <= 3; ++wire) {
        pv_fixture f;
        pv_open(&f);
        f.decode_wire = wire;
        f.cancel_decode = true;
#ifdef TNY_ALLOC_TESTING
        f.owner_baseline = tny_alloc_test_owned_live();
#endif
        ASSERT_EQ(0, pv_turn(&f, "cancel while decoding"));
        ASSERT_EQ(1, f.turn_ends);
        ASSERT_EQ(0, f.terminal_during_decode);
        ASSERT_EQ(TNY_STOP_INTERRUPTED, f.stop);
        ASSERT_EQ(0, f.errors);
        ASSERT_EQ(1, f.decoded_texts);
        ASSERT_EQ(0, f.decoded_thinking);
        ASSERT_STR_EQ("cancel-now", f.callback_text.data);
#ifdef TNY_ALLOC_TESTING
        ASSERT(f.owner_inside > f.owner_baseline);
        ASSERT(f.callback_owners_retained);
        ASSERT_EQ(f.owner_baseline, f.owner_terminal);
        ASSERT_EQ(f.owner_baseline, tny_alloc_test_owned_live());
#endif
        f.cancel_decode = false;
        f.cancel_usage = true;
        buf_clear(&f.callback_text);
        ASSERT_EQ(0, pv_turn(&f, "successful later turn"));
        ASSERT_EQ(2, f.turn_ends);
        ASSERT_EQ(TNY_STOP_DONE, f.stop);
        ASSERT_EQ(0, f.errors);
        ASSERT_STR_EQ("cancel-now", f.callback_text.data);
        pv_close(&f);
    }
    PASS();
}

extern int tny_ownership_selftest(void);
TEST cpp_ownership_boundary(void) {
    ASSERT_EQ(0, tny_ownership_selftest());
    PASS();
}

#ifdef TNY_ALLOC_TESTING
typedef struct {
    pv_fixture *fixture;
    bool body, parser;
    size_t fault_index, offset, prefix, construction_count;
    char *snapshot;
    char *path;
    bool armed, retained_view;
} request_fault_fixture;

static void request_fault_control(const tny_openai_control_request *request,
                                  tny_openai_control_response *response, void *ud) {
    (void)response;
    request_fault_fixture *f = ud;
    if (request->kind == TNY_OPENAI_CONTROL_PROVIDER_REQUEST)
        f->retained_view |= !oa_request_test_builder_released_view();
    if (f->armed) {
        if (f->body && !f->offset && request->kind == TNY_OPENAI_CONTROL_PROVIDER_REQUEST) {
            f->construction_count = tny_alloc_test_scope_count() - f->prefix;
            response->stop = true;
        }
        return;
    }
    if (f->body) {
        if (request->kind != TNY_OPENAI_CONTROL_TOOL_BATCH) return;
        /* Measure the ordinary batch save on the same session, then fail the
         * construction range immediately following that save and connection open. */
        tny_alloc_scope_begin("disabled");
        if (session_save(f->fixture->session) != 0) return;
        f->prefix = tny_alloc_test_scope_count();
        tny_alloc_scope_begin("disabled");
        char err[256];
        http_conn *probe = http_open(f->fixture->ctx->base_url, err, sizeof err);
        if (!probe) return;
        f->prefix += tny_alloc_test_scope_count();
        f->fault_index = f->offset ? f->prefix + f->offset : 0;
        http_close(probe);
    } else {
        tny_openai_control_kind edge =
            f->parser ? TNY_OPENAI_CONTROL_PROVIDER_RESPONSE : TNY_OPENAI_CONTROL_PROVIDER_REQUEST;
        if (request->kind != edge || request->step != 1) return;
        f->fault_index = 1;
    }
    f->snapshot = file_slurp(f->path, NULL);
    f->armed = true;
    /* The complete second request has reached the transport-construction
     * boundary. Fail its first owned HTTP request allocation, after the first
     * response's usage and tool/steer transcript have been persisted. */
    setenv("TNY_TEST_ALLOC_SCOPE", "openai-request", 1);
    char index[32];
    snprintf(index, sizeof index, "%zu", f->fault_index);
    setenv("TNY_TEST_ALLOC_FAIL_AT", index, 1);
    tny_alloc_scope_begin("openai-request");
}

/* Real engine + loopback provider + registered asynchronous host tool. Scopes
 * start at public control/invoke boundaries, never at guessed allocation offsets. */
typedef struct {
    tny_tool_call *host;
    uint64_t generation;
    size_t index;
    int mode, invokes, permissions, errors, ends, tools;
    bool armed, permission_seen;
    tny_stop_reason stop;
} native_pending_fixture;

static void native_fault_begin(native_pending_fixture *f) {
    char number[32];
    snprintf(number, sizeof number, "%zu", f->index);
    setenv("TNY_TEST_ALLOC_SCOPE", "native-pending", 1);
    setenv("TNY_TEST_ALLOC_FAIL_AT", number, 1);
    tny_alloc_scope_begin("native-pending");
    f->armed = true;
}
static int32_t native_pending_invoke(void *ud, tny_tool_call *call, uint64_t generation,
                                     tny_bytes arguments, tny_tool_result_v1 *result) {
    (void)arguments;
    (void)result;
    native_pending_fixture *f = ud;
    f->host = call;
    f->generation = generation;
    f->invokes++;
    if (f->mode == 4 || f->mode == 6) native_fault_begin(f);
    return TNY_TOOL_INVOKE_ASYNC;
}
static void native_pending_control(const tny_openai_control_request *request,
                                   tny_openai_control_response *response, void *ud) {
    native_pending_fixture *f = ud;
    if (f->mode == 7 && request->kind == TNY_OPENAI_CONTROL_TOOL_BATCH) response->stop = true;
    if (request->kind == TNY_OPENAI_CONTROL_PRE_TOOL) {
        response->extension = xstrdup("fixture-extension");
        response->reason = xstrdup("fixture-reason");
    }
    if (request->kind == TNY_OPENAI_CONTROL_PERMISSION) {
        f->permissions++;
        if (f->mode == 5) native_fault_begin(f);
    }
}
static int native_pending_drive(pv_fixture *p, tny_engine *engine, native_pending_fixture *f) {
    struct pollfd fds[HTTP_SERVER_POLLFD_CAPACITY + TNY_BACKEND_POLLFD_MAX];
    int sn = http_server_pollfds(p->server, fds, HTTP_SERVER_POLLFD_CAPACITY);
    int bn = tny_engine_pollfds(engine, fds + sn, TNY_BACKEND_POLLFD_MAX);
    if (tny_poll(fds, (nfds_t)(sn + bn), 1) < 0) return -1;
    if (http_server_dispatch(p->server, fds, sn) != 0) return -1;
    (void)tny_engine_dispatch(engine, fds + sn, bn);
    char err[256];
    for (;;) {
        tny_owned_event *event = NULL;
        tny_engine_next status = tny_engine_next_event(engine, 0, &event, err, sizeof err);
        if (status == TNY_ENGINE_NEXT_DRAINED || status == TNY_ENGINE_NEXT_TIMEOUT) return 0;
        if (status != TNY_ENGINE_NEXT_EVENT) return -1;
        if (event->ev.kind == TNY_EV_PERMISSION) {
            if (strcmp(event->ev.perm_id, "native-pending-id")) return -1;
            f->permission_seen = true;
        }
        if (event->ev.kind == TNY_EV_TOOL_END) f->tools++;
        if (event->ev.kind == TNY_EV_ERROR) {
            if (event->ev.error_code != TNY_EVENT_ERROR_OOM || f->ends) return -1;
            f->errors++;
        }
        if (event->ev.kind == TNY_EV_TURN_END) {
            f->ends++;
            f->stop = event->ev.stop;
        }
        tny_owned_event_free(event);
    }
}

#ifndef _WIN32
/* Real TCP RST between turns deterministically exercises write-side keep-alive
 * replay. A separate thread owns all peer-side allocations, outside fault scopes. */
typedef struct {
    int listener, command[2], ready[2], status, requests;
    bool stop;
    buf_t received;
} native_replay_peer;
static int native_read_request(int fd, buf_t *body) {
    for (;;) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (tny_poll(&pfd, 1, 5000) <= 0) return -1;
        char bytes[8192];
        ssize_t n = read(fd, bytes, sizeof bytes);
        if (n == 0) return body->len ? -1 : 0;
        if (n < 0) return -1;
        buf_append(body, bytes, (size_t)n);
        char *end = strstr(body->data, "\r\n\r\n");
        char *length = strstr(body->data, "Content-Length: ");
        if (end && length &&
            body->len >= (size_t)(end + 4 - body->data) + strtoul(length + 16, NULL, 10))
            return 1;
    }
}
static void *native_replay_server(void *ud) {
    native_replay_peer *p = ud;
    int fd = accept(p->listener, NULL, NULL);
    if (fd < 0) {
        p->status = 1;
        return NULL;
    }
    buf_t first = {0};
    if (native_read_request(fd, &first) != 1) p->status = 2;
    p->requests++;
    const char *body = "{\"status\":\"completed\",\"output\":[]}";
    char reply[256];
    int len = snprintf(reply, sizeof reply,
                       "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                       "%zu\r\nConnection: keep-alive\r\n\r\n%s",
                       strlen(body), body);
    if (socket_write(fd, reply, (size_t)len) != len) p->status = 3;
    char signal;
    if (read(p->command[0], &signal, 1) != 1) p->status = 4;
    struct linger rst = {.l_onoff = 1, .l_linger = 0};
    if (setsockopt(fd, SOL_SOCKET, SO_LINGER, &rst, sizeof rst) != 0) p->status = 5;
    close(fd);
    if (write(p->ready[1], "x", 1) != 1) p->status = 6;
    fd = accept(p->listener, NULL, NULL);
    if (fd < 0) {
        p->status = 7;
        buf_free(&first);
        return NULL;
    }
    int rc = native_read_request(fd, &p->received);
    if (rc != (p->stop ? 0 : 1)) p->status = 8;
    if (rc == 1) {
        p->requests++;
        if (socket_write(fd, reply, (size_t)len) != len) p->status = 9;
    }
    close(fd);
    buf_free(&first);
    return NULL;
}
typedef struct {
    bool active, stop, reentrant;
    tny_backend *backend;
    int attempts, sequence[3];
} native_replay_control_state;
static void native_replay_control(const tny_openai_control_request *request,
                                  tny_openai_control_response *response, void *ud) {
    native_replay_control_state *s = ud;
    if (!s->active || request->kind != TNY_OPENAI_CONTROL_PROVIDER_REQUEST) return;
    if (s->attempts < 3) s->sequence[s->attempts] = request->attempt;
    s->attempts++;
    if (s->stop && s->attempts == 2) {
        if (s->reentrant) s->backend->cancel(s->backend);
        else response->stop = true;
    }
}
#endif
TEST native_request_real_stale_replay_and_control_stop(void) {
#ifdef _WIN32
    SKIPm("TCP RST peer fixture uses POSIX thread/socket APIs");
#else
    for (int stop = 0; stop < 3; ++stop) {
        native_replay_peer peer = {.stop = stop != 0};
        peer.listener = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT(peer.listener >= 0);
        struct sockaddr_in addr = {.sin_family = AF_INET,
                                   .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        ASSERT_EQ(0, bind(peer.listener, (struct sockaddr *)&addr, sizeof addr));
        ASSERT_EQ(0, listen(peer.listener, 2));
        socklen_t size = sizeof addr;
        ASSERT_EQ(0, getsockname(peer.listener, (struct sockaddr *)&addr, &size));
        ASSERT_EQ(0, pipe(peer.command));
        ASSERT_EQ(0, pipe(peer.ready));
        pthread_t thread;
        ASSERT_EQ(0, pthread_create(&thread, NULL, native_replay_server, &peer));
        pv_fixture p;
        pv_open(&p);
        p.ctx->library_mode = true;
        char url[128];
        snprintf(url, sizeof url, "http://127.0.0.1:%u/v1", ntohs(addr.sin_port));
        free(p.ctx->base_url);
        p.ctx->base_url = xstrdup(url);
        free(p.ctx->wire_api);
        p.ctx->wire_api = xstrdup("responses");
        native_replay_control_state control = {
            .stop = stop != 0, .reentrant = stop == 2, .backend = p.backend};
        tny_backend_openai_bind(p.backend, p.session, p.perm, NULL, NULL, NULL, NULL, NULL, NULL,
                                NULL, NULL, native_replay_control, &control);
        ASSERT_EQ(0, pv_turn(&p, "first keep-alive turn"));
        ASSERT_EQ(TNY_STOP_DONE, p.stop);
        ASSERT_EQ(1, write(peer.command[1], "x", 1));
        char signal;
        ASSERT_EQ(1, read(peer.ready[0], &signal, 1));
        control.active = true;
        ASSERT_EQ(0, pv_turn(&p, "stale retry preserves this request"));
        ASSERT_EQ(2, control.attempts);
        ASSERT_EQ(1, control.sequence[0]);
        ASSERT_EQ(2, control.sequence[1]);
        ASSERT_EQ(stop ? TNY_STOP_INTERRUPTED : TNY_STOP_DONE, p.stop);
        ASSERT_EQ(2, p.turn_ends); /* one terminal per turn, even when cancel reenters */
        ASSERT_EQ(0, p.errors);
        pv_close(&p); /* closes an unsubmitted reopened socket after control stop */
        ASSERT_EQ(0, pthread_join(thread, NULL));
        ASSERT_EQ(0, peer.status);
        ASSERT_EQ(stop ? 1 : 2, peer.requests);
        if (!stop) {
            ASSERT(strstr(peer.received.data, "POST /v1/responses HTTP/1.1"));
            ASSERT(strstr(peer.received.data, "stale retry preserves this request"));
            ASSERT(strstr(peer.received.data, "Authorization: Bearer " PV_TOKEN));
        } else ASSERT_EQ(0, peer.received.len);
        close(peer.listener);
        close(peer.command[0]);
        close(peer.command[1]);
        close(peer.ready[0]);
        close(peer.ready[1]);
        buf_free(&peer.received);
    }
    PASS();
#endif
}

TEST native_cancel_headers_raw_error_and_retry_waits(void) {
#ifdef _WIN32
    SKIPm("loopback wait-state fixture uses POSIX socket APIs");
#else
    for (int wire = 0; wire < 2; ++wire) {
        for (int mode = 0; mode < 4; ++mode) {
            tny_alloc_scope_begin("disabled");
            size_t live = tny_alloc_test_owned_live();
            int listener = socket(AF_INET, SOCK_STREAM, 0);
            ASSERT(listener >= 0);
            struct sockaddr_in addr = {.sin_family = AF_INET,
                                       .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
            ASSERT_EQ(0, bind(listener, (struct sockaddr *)&addr, sizeof addr));
            ASSERT_EQ(0, listen(listener, 1));
            socklen_t size = sizeof addr;
            ASSERT_EQ(0, getsockname(listener, (struct sockaddr *)&addr, &size));
            pv_fixture p;
            pv_open(&p);
            p.ctx->library_mode = true;
            char *saved_url = p.ctx->base_url;
            char url[128];
            snprintf(url, sizeof url, "http://127.0.0.1:%u/v1", ntohs(addr.sin_port));
            p.ctx->base_url = xstrdup(url);
            free(p.ctx->wire_api);
            p.ctx->wire_api = xstrdup(wire ? "chat" : "responses");
            tny_engine *engine = tny_engine_new(p.ctx, p.session, p.perm, NULL, NULL);
            ASSERT(engine);
            char err[256];
            ASSERT_EQ(0, tny_engine_prepare(engine, p.backend, TNY_ENGINE_PREPARE_RESUMED, err,
                                            sizeof err));
            ASSERT_EQ(0, tny_engine_start(engine, "cancel wait", NULL, err, sizeof err));
            int fd = accept(listener, NULL, NULL);
            ASSERT(fd >= 0);
            buf_t request = {0};
            ASSERT_EQ(1, native_read_request(fd, &request));
            buf_free(&request);
            const char *reply =
                mode == 1 ? "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                            "999\r\n\r\n{\"partial\":"
                : mode == 2 ? "HTTP/1.1 503 Error\r\nContent-Length: 999\r\n\r\n{\"error\":"
                            : "HTTP/1.1 503 Error\r\nContent-Length: 2\r\n\r\n{}";
            if (mode) ASSERT_EQ((ssize_t)strlen(reply), socket_write(fd, reply, strlen(reply)));
            native_pending_fixture f = {0};
            for (int i = 0; i < (mode ? 3 : 0); ++i)
                ASSERT_EQ(0, native_pending_drive(&p, engine, &f));
            if (mode == 3) {
                int timeout = p.backend->poll_timeout(p.backend);
                ASSERT(timeout > 0 && timeout <= 1000); /* backoff has not expired */
            }
            ASSERT_EQ(0, f.ends);
            tny_engine_cancel(engine);
            ASSERT_EQ(0, native_pending_drive(&p, engine, &f));
            ASSERT_EQ(0, f.errors);
            ASSERT_EQ(1, f.ends);
            ASSERT_EQ(TNY_STOP_INTERRUPTED, f.stop);
            close(fd);
            close(listener);
            free(p.ctx->base_url);
            p.ctx->base_url = saved_url;
            p.first_body = wire ? "{\"choices\":[{\"message\":{\"content\":\"reused\"},\"finish_"
                                  "reason\":\"stop\"}]}"
                                : "{\"status\":\"completed\",\"output\":[]}";
            f.ends = 0;
            ASSERT_EQ(0, tny_engine_start(engine, "after cancellation", NULL, err, sizeof err));
            for (int i = 0; i < 2000 && !f.ends; ++i)
                ASSERT_EQ(0, native_pending_drive(&p, engine, &f));
            ASSERT_EQ(1, f.ends);
            ASSERT_EQ(0, f.errors);
            ASSERT_EQ(TNY_STOP_DONE, f.stop);
            tny_engine_free(engine);
            perm_free(p.perm);
            session_close(p.session);
            tny_ctx_free(p.ctx);
            http_server_destroy(&p.server);
            buf_free(&p.error_text);
            buf_free(&p.callback_text);
            for (size_t i = 0; i < sizeof p.bodies / sizeof p.bodies[0]; ++i)
                buf_free(&p.bodies[i]);
            ASSERT_EQ(live, tny_alloc_test_owned_live());
        }
    }
    PASS();
#endif
}

static void native_first_control_cancel(const tny_openai_control_request *request,
                                        tny_openai_control_response *response, void *ud) {
    (void)response;
    pv_fixture *p = ud;
    if (request->kind != TNY_OPENAI_CONTROL_PROVIDER_REQUEST) return;
    p->hook_calls++;
    if (p->terminal_case) {
        p->backend->disconnect(p->backend);
        return;
    }
    p->backend->cancel(p->backend);
    p->backend->cancel(p->backend);
}
TEST native_request_reentrant_first_control_cancel(void) {
#ifdef _WIN32
    SKIPm("POSIX socket fixture");
#else
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(listener >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    ASSERT_EQ(0, bind(listener, (struct sockaddr *)&addr, sizeof addr));
    ASSERT_EQ(0, listen(listener, 2));
    socklen_t size = sizeof addr;
    ASSERT_EQ(0, getsockname(listener, (struct sockaddr *)&addr, &size));
    pv_fixture p;
    pv_open(&p);
    p.ctx->library_mode = true;
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%u/v1", ntohs(addr.sin_port));
    free(p.ctx->base_url);
    p.ctx->base_url = xstrdup(url);
    tny_backend_openai_bind(p.backend, p.session, p.perm, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                            NULL, native_first_control_cancel, &p);
    for (int i = 1; i <= 2; ++i) {
        ASSERT_EQ(0, pv_turn(&p, "cancel before sending"));
        ASSERT_EQ(i, p.turn_ends);
        ASSERT_EQ(i, p.hook_calls);
        ASSERT_EQ(TNY_STOP_INTERRUPTED, p.stop);
        ASSERT_EQ(0, p.errors);
        int fd = accept(listener, NULL, NULL);
        ASSERT(fd >= 0);
        buf_t received = {0};
        ASSERT_EQ(0, native_read_request(fd, &received));
        ASSERT_EQ(0, received.len);
        buf_free(&received);
        close(fd);
    }
    p.terminal_case = 1;
    char err[256];
    ASSERT_EQ(-2, p.backend->send(p.backend, "invalidated connection", NULL, pv_event, &p, err,
                                  sizeof err));
    ASSERT(!tny_alloc_scope_failed());
    ASSERT_EQ(3, p.turn_ends);
    ASSERT_EQ(1, p.errors);
    ASSERT_EQ(TNY_STOP_ERROR, p.stop);
    int empty = accept(listener, NULL, NULL);
    ASSERT(empty >= 0);
    buf_t received = {0};
    ASSERT_EQ(0, native_read_request(empty, &received));
    buf_free(&received);
    close(empty);
    pv_close(&p);
    close(listener);
    PASS();
#endif
}

TEST native_continuation_retains_text(void) {
    pv_fixture p;
    pv_open(&p);
    p.ctx->library_mode = true;
    p.first_body = "data: {\"choices\":[{\"delta\":{\"content\":\"partial \"}}]}\n\n";
    p.done_body = "{\"choices\":[{\"message\":{\"content\":\"tail\"},\"finish_reason\":\"stop\"}]}";
    ASSERT_EQ(0, pv_turn(&p, "continue from a partial response"));
    ASSERT_EQ(2, p.requests);
    ASSERT_EQ(1, p.turn_ends);
    ASSERT_EQ(TNY_STOP_DONE, p.stop);
    ASSERT(strstr(p.bodies[1].data, "partial "));
    yyjson_mut_val *last = yyjson_mut_arr_get_last(session_messages(p.session));
    ASSERT_STR_EQ("partial tail", yyjson_mut_get_str(yyjson_mut_obj_get(last, "content")));
    pv_close(&p);
    PASS();
}

typedef struct {
    pv_fixture *p;
    int first, second;
} native_checkpoint_fixture;
static void native_checkpoint_control(const tny_openai_control_request *request,
                                      tny_openai_control_response *response, void *ud) {
    (void)response;
    native_checkpoint_fixture *f = ud;
    if (request->kind != TNY_OPENAI_CONTROL_POST_TOOL) return;
    if (!strcmp(request->tool_id, "consumed-first")) {
        f->first++;
        tny_backend_openai_background(f->p->backend);
    } else if (!strcmp(request->tool_id, "remaining-second")) f->second++;
}
static void native_backend_tick(pv_fixture *p) {
    struct pollfd fds[HTTP_SERVER_POLLFD_CAPACITY + TNY_BACKEND_POLLFD_MAX];
    int sn = http_server_pollfds(p->server, fds, HTTP_SERVER_POLLFD_CAPACITY);
    int bn = p->backend->pollfds(p->backend, fds + sn, TNY_BACKEND_POLLFD_MAX);
    (void)tny_poll(fds, (nfds_t)(sn + bn), 1);
    (void)http_server_dispatch(p->server, fds, sn);
    (void)p->backend->dispatch(p->backend, fds + sn, bn);
}
TEST native_checkpoint_retains_steer_and_consumed_index(void) {
    pv_fixture p;
    pv_open(&p);
    p.ctx->library_mode = true;
    p.first_body = "{\"choices\":[{\"message\":{\"content\":\"batch "
                   "text\",\"tool_calls\":[{\"id\":\"consumed-first\",\"type\":\"function\","
                   "\"function\":{\"name\":\"list_files\",\"arguments\":\"{}\"}},{\"id\":"
                   "\"remaining-second\",\"type\":\"function\",\"function\":{\"name\":\"list_"
                   "files\",\"arguments\":\"{}\"}}]},\"finish_reason\":\"tool_calls\"}]}";
    native_checkpoint_fixture f = {.p = &p};
    tny_backend_openai_bind(p.backend, p.session, p.perm, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                            NULL, native_checkpoint_control, &f);
    char err[256];
    ASSERT_EQ(0, p.backend->send(p.backend, "checkpoint", NULL, pv_event, &p, err, sizeof err));
    for (int i = 0; i < 2000 && !tny_backend_openai_parked(p.backend) && !p.ended; ++i)
        native_backend_tick(&p);
    ASSERT(tny_backend_openai_parked(p.backend));
    ASSERT_EQ(1, f.first);
    ASSERT_EQ(0, f.second);
    ASSERT_EQ(0, p.turn_ends);
    ASSERT_EQ(0, p.backend->steer(p.backend, "parked checkpoint steer", err, sizeof err));
    yyjson_mut_doc *doc = yyjson_mut_doc_new(jallocator());
    yyjson_mut_val *checkpoint = tny_backend_openai_checkpoint(p.backend, doc);
    ASSERT(checkpoint);
    yyjson_mut_doc_set_root(doc, checkpoint);
    ASSERT_EQ(1, yyjson_mut_get_int(yyjson_mut_obj_get(checkpoint, "tool_index")));
    char *serialized = jwrite(doc);
    yyjson_mut_doc_free(doc);
    ASSERT(serialized);
    yyjson_doc *saved = jparse(serialized, strlen(serialized));
    free(serialized);
    ASSERT(saved);
    p.backend->destroy(p.backend);
    p.backend = tny_backend_openai_new(p.ctx);
    ASSERT(p.backend);
    tny_backend_openai_bind(p.backend, p.session, p.perm, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                            NULL, native_checkpoint_control, &f);
    ASSERT_EQ(0, tny_backend_openai_restore(p.backend, yyjson_doc_get_root(saved), pv_event, &p));
    yyjson_doc_free(saved);
    ASSERT_EQ(0, tny_backend_openai_continue(p.backend));
    for (int i = 0; i < 2000 && !p.ended; ++i) native_backend_tick(&p);
    ASSERT_EQ(1, p.turn_ends); /* restore reopens terminal authority */
    ASSERT_EQ(TNY_STOP_DONE, p.stop);
    ASSERT_EQ(1, f.first);
    ASSERT_EQ(1, f.second);
    ASSERT_EQ(2, p.requests);
    ASSERT(strstr(p.bodies[1].data, "parked checkpoint steer"));
    ASSERT(strstr(p.bodies[1].data, "batch text"));
    const char *log_text = tny_backend_openai_toolcalls_json(p.backend);
    yyjson_doc *log = jparse(log_text, strlen(log_text));
    ASSERT(log);
    ASSERT_EQ(2, yyjson_arr_size(yyjson_doc_get_root(log)));
    yyjson_doc_free(log);
    pv_close(&p);
    PASS();
}

TEST native_pending_lifecycle_and_allocation_sweeps(void) {
    for (int wire = 0; wire < 2; ++wire) {
        for (int mode = 0; mode < 8; ++mode) {
            size_t count = 0;
            for (size_t index = 0; index <= count; ++index) {
                tny_alloc_scope_begin("disabled");
                size_t live = tny_alloc_test_owned_live();
                pv_fixture p;
                pv_open(&p);
                p.ctx->library_mode = true;
                p.ctx->perm_mode = mode == 4 ? TNY_MODE_YOLO : TNY_MODE_ASK;
                free(p.ctx->wire_api);
                p.ctx->wire_api = xstrdup(wire ? "chat" : "responses");
                p.first_body =
                    wire ? "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"native-pending-"
                           "id\",\"type\":\"function\",\"function\":{\"name\":\"native_pending\","
                           "\"arguments\":\"{}\"}}]},\"finish_reason\":\"tool_calls\"}],\"usage\":{"
                           "\"prompt_tokens\":123,\"completion_tokens\":7}}"
                         : "{\"status\":\"completed\",\"usage\":{\"input_tokens\":123,\"output_"
                           "tokens\":7},\"output\":[{\"type\":\"function_call\",\"call_id\":"
                           "\"native-pending-id\",\"name\":\"native_pending\",\"arguments\":\"{}\"}"
                           "]}";
                p.done_body = wire ? "{\"choices\":[{\"message\":{\"content\":\"done\"},\"finish_"
                                     "reason\":\"stop\"}]}"
                                   : "{\"status\":\"completed\",\"output\":[]}";
                native_pending_fixture f = {.mode = mode, .index = index};
                p.ctx->custom_tools = custom_tools_new();
                ASSERT(p.ctx->custom_tools);
                tny_tool_spec_v1 spec = {0};
                spec.abi_version = TNY_TOOL_SPEC_ABI_VERSION;
                spec.struct_size = sizeof spec;
                spec.name = (tny_bytes){"native_pending", 14};
                spec.description = (tny_bytes){"fixture", 7};
                spec.input_schema_json = (tny_bytes){"{\"type\":\"object\"}", 17};
                spec.sensitivity = TNY_TOOL_SENSITIVITY_SENSITIVE;
                spec.invoke = native_pending_invoke;
                spec.user_data = &f;
                tny_tool_registration *registration = NULL;
                ASSERT_EQ(TNY_STATUS_OK,
                          custom_tools_register(p.ctx->custom_tools, NULL, &spec, &registration));
                tny_engine *engine = tny_engine_new(p.ctx, p.session, p.perm, NULL, NULL);
                ASSERT(engine);
                char err[256];
                ASSERT_EQ(0, tny_engine_prepare(engine, p.backend, TNY_ENGINE_PREPARE_RESUMED, err,
                                                sizeof err));
                tny_backend_openai_bind(p.backend, p.session, p.perm, NULL, NULL, NULL, NULL, NULL,
                                        NULL, NULL, NULL, native_pending_control, &f);
                ASSERT_EQ(0, tny_engine_start(engine, "native pending", NULL, err, sizeof err));
                for (int i = 0; i < 2000 && !f.ends && !f.permission_seen && !f.host; ++i)
                    ASSERT_EQ(0, native_pending_drive(&p, engine, &f));
                if (mode == 5 && !index) count = tny_alloc_test_scope_count();
                if (mode != 4 && !f.ends) {
                    ASSERT(f.permission_seen);
                    ASSERT_EQ(0, f.invokes);
                    if (mode == 2) tny_engine_cancel(engine);
                    else
                        tny_engine_respond_permission(engine, "native-pending-id",
                                                      mode == 1 ? TNY_PERM_DECISION_DENY
                                                                : TNY_PERM_DECISION_ALLOW);
                    for (int i = 0; i < 2000 && !f.ends && !f.host; ++i)
                        ASSERT_EQ(0, native_pending_drive(&p, engine, &f));
                }
                if ((mode == 4 || mode == 6) && !index) count = tny_alloc_test_scope_count();
                bool injected = index && tny_alloc_test_scope_injected();
                tny_tool_result_v1 result = {0};
                result.abi_version = TNY_TOOL_RESULT_ABI_VERSION;
                result.struct_size = sizeof result;
                char payload[] = "queued-owned-result";
                result.data = (tny_bytes){payload, strlen(payload)};
                if (f.host && !injected) {
                    ASSERT_EQ(1, f.invokes);
                    ASSERT_EQ(1, p.requests);
                    ASSERT_EQ(TNY_STATUS_BAD_STATE,
                              custom_tool_complete(f.host, f.generation + 1, &result));
                    if (mode == 7) native_fault_begin(&f);
                    int complete = custom_tool_complete(f.host, f.generation, &result);
                    injected = index && tny_alloc_test_scope_injected();
                    ASSERT_EQ(injected ? TNY_STATUS_OOM : TNY_STATUS_OK, complete);
                    memset(payload, 'x', sizeof payload - 1);
                    ASSERT_EQ(1, p.requests); /* queued bytes have not been consumed */
                    if (mode == 3) tny_engine_cancel(engine);
                }
                for (int i = 0; i < 2000 && !f.ends; ++i)
                    ASSERT_EQ(0, native_pending_drive(&p, engine, &f));
                if (mode == 7 && !index) count = tny_alloc_test_scope_count();
                injected = index && tny_alloc_test_scope_injected();
                ASSERT_EQ(index != 0, injected);
                ASSERT_EQ(1, f.ends);
                ASSERT_EQ(injected ? 1 : 0, f.errors);
                ASSERT_EQ(injected                                ? TNY_STOP_ERROR
                          : mode == 1                             ? TNY_STOP_DENIED
                          : (mode == 2 || mode == 3 || mode == 7) ? TNY_STOP_INTERRUPTED
                                                                  : TNY_STOP_DONE,
                          f.stop);
                ASSERT_EQ(0, tny_alloc_test_settlement_allocations());
                if (injected) {
                    size_t settled = tny_alloc_test_scope_count();
                    ASSERT_EQ(0, native_pending_drive(&p, engine, &f));
                    ASSERT_EQ(settled, tny_alloc_test_scope_count());
                    ASSERT_EQ(1, f.errors);
                    ASSERT_EQ(1, f.ends);
                }
                tny_alloc_scope_begin("disabled");
                if (f.host) {
                    ASSERT_EQ(TNY_STATUS_BAD_STATE,
                              custom_tool_complete(f.host, f.generation, &result));
                    tny_tool_call_release(f.host);
                    f.host = NULL;
                }
                if (!injected && (mode == 1 || mode == 2 || mode == 3)) ASSERT_EQ(1, f.tools);
                if (!injected && mode != 1 && mode != 2 && mode != 3 && mode != 7) {
                    ASSERT_EQ(1, f.tools);
                    ASSERT_EQ(2, p.requests);
                    ASSERT(strstr(p.bodies[1].data, "queued-owned-result"));
                    const char *log = tny_backend_openai_toolcalls_json(p.backend);
                    ASSERT(strstr(log, "native_pending"));
                    ASSERT_STR_EQ(log, tny_backend_openai_toolcalls_json(p.backend));
                }
                /* Recovery uses the same engine/owner after every failed index. */
                f.mode = 0;
                f.errors = f.ends = 0;
                p.first_body = p.done_body;
                p.turn_requests = 0;
                ASSERT_EQ(0,
                          tny_engine_start(engine, "reuse after pending", NULL, err, sizeof err));
                for (int i = 0; i < 2000 && !f.ends; ++i)
                    ASSERT_EQ(0, native_pending_drive(&p, engine, &f));
                ASSERT_EQ(1, f.ends);
                ASSERT_EQ(0, f.errors);
                ASSERT_EQ(TNY_STOP_DONE, f.stop);
                tny_engine_free(engine);
                p.backend = NULL;
                custom_tools_free(p.ctx->custom_tools);
                p.ctx->custom_tools = NULL;
                perm_free(p.perm);
                session_close(p.session);
                tny_ctx_free(p.ctx);
                http_server_destroy(&p.server);
                buf_free(&p.error_text);
                buf_free(&p.callback_text);
                for (size_t i = 0; i < sizeof p.bodies / sizeof p.bodies[0]; ++i)
                    buf_free(&p.bodies[i]);
                ASSERT_EQ(live, tny_alloc_test_owned_live());
                unsetenv("TNY_TEST_ALLOC_SCOPE");
                unsetenv("TNY_TEST_ALLOC_FAIL_AT");
            }
            if (mode >= 4) ASSERT(count > 0);
            printf("native pending wire=%d mode=%d discovered=%zu\n", wire, mode, count);
        }
    }
    PASS();
}

TEST native_request_constructor_allocation_sweep(void) {
    tny_alloc_scope_begin("disabled");
    tny_ctx *ctx = tny_ctx_new_explicit("/tmp", "/tmp");
    ASSERT(ctx);
    size_t live = tny_alloc_test_owned_live(), count = 0;
    for (size_t index = 0; index <= count; ++index) {
        native_pending_fixture f = {.index = index};
        native_fault_begin(&f);
        tny_backend *backend = tny_backend_openai_new(ctx);
        if (!index) {
            ASSERT(backend);
            count = tny_alloc_test_scope_count();
        } else {
            ASSERT(tny_alloc_test_scope_injected());
            ASSERT(!backend);
        }
        size_t before = tny_alloc_test_scope_count();
        tny_alloc_provider_failed();
        tny_alloc_settlement_begin();
        if (backend) backend->destroy(backend);
        tny_alloc_settlement_end();
        ASSERT_EQ(before, tny_alloc_test_scope_count());
        ASSERT_EQ(0, tny_alloc_test_settlement_allocations());
        ASSERT_EQ(live, tny_alloc_test_owned_live());
    }
    ASSERT(count > 0);
    tny_alloc_scope_begin("disabled");
    tny_ctx_free(ctx);
    unsetenv("TNY_TEST_ALLOC_SCOPE");
    unsetenv("TNY_TEST_ALLOC_FAIL_AT");
    PASS();
}

TEST native_pending_transfer_preserves_source_on_failure(void) {
    size_t count = 0;
    for (int cycle = 0; cycle < 8; ++cycle) {
        for (size_t index = 0; index <= count; ++index) {
            tny_alloc_scope_begin("disabled");
            size_t live = tny_alloc_test_owned_live();
            oa_turn_storage *turn = oa_turn_new();
            ASSERT(turn);
            buf_appends(&turn->text, "partial answer");
            buf_appends(&turn->rawbody, "raw error");
            buf_appends(&turn->toolcall_log, "retained final log");
            oa_turn_take_steer(turn, xstrdup("parked steer"));
            tools_call call = {0};
            call.name = xstrdup("source call");
            call.doc = jparse("{}", 2);
            call.args = yyjson_doc_get_root(call.doc);
            ASSERT_EQ(0, oa_pending_admit(&turn->permission, "id", "original", "effective",
                                          "extension", "reason", &call));
            ASSERT(!call.name && !call.doc);
            char *name = turn->permission.call.name;
            yyjson_doc *doc = turn->permission.call.doc;
            char *id = turn->permission.id;
            native_pending_fixture f = {.index = index};
            native_fault_begin(&f);
            int rc = oa_pending_admit(
                &turn->custom, turn->permission.id, turn->permission.original_args,
                turn->permission.effective_args, turn->permission.control_extension,
                turn->permission.control_reason, &turn->permission.call);
            if (!index) {
                count = tny_alloc_test_scope_count();
                ASSERT(count > 0);
                ASSERT_EQ(0, rc);
                ASSERT_EQ(name, turn->custom.call.name);
                ASSERT_EQ(doc, turn->custom.call.doc);
                ASSERT(!turn->permission.call.name && !turn->permission.call.doc);
                ASSERT_EQ(-2,
                          oa_pending_admit(&turn->custom, "again", "{}", "{}", NULL, NULL, &call));
                oa_pending_reset(&turn->permission);
                ASSERT_STR_EQ("id", turn->custom.id);
                ASSERT_STR_EQ("original", turn->custom.original_args);
                ASSERT_STR_EQ("effective", turn->custom.effective_args);
                ASSERT_STR_EQ("extension", turn->custom.control_extension);
                ASSERT_STR_EQ("reason", turn->custom.control_reason);
            } else {
                ASSERT(tny_alloc_test_scope_injected());
                ASSERT_EQ(-2, rc);
                ASSERT_EQ(name, turn->permission.call.name);
                ASSERT_EQ(doc, turn->permission.call.doc);
                ASSERT_EQ(id, turn->permission.id);
                ASSERT(!turn->custom.id && !turn->custom.call.name && !turn->custom.call.doc);
            }
            size_t before = tny_alloc_test_scope_count();
            tny_alloc_provider_failed();
            tny_alloc_settlement_begin();
            oa_pending_reset(&turn->permission);
            oa_pending_reset(&turn->permission);
            oa_pending_reset(&turn->custom);
            oa_pending_reset(&turn->custom);
            ASSERT_STR_EQ("partial answer", turn->text.data);
            ASSERT_STR_EQ("raw error", turn->rawbody.data);
            ASSERT_STR_EQ("retained final log", turn->toolcall_log.data);
            ASSERT_STR_EQ("parked steer", turn->steer);
            oa_turn_free(&turn);
            oa_turn_free(&turn);
            tny_alloc_settlement_end();
            ASSERT_EQ(before, tny_alloc_test_scope_count());
            ASSERT_EQ(0, tny_alloc_test_settlement_allocations());
            ASSERT_EQ(live, tny_alloc_test_owned_live());
        }
    }
    unsetenv("TNY_TEST_ALLOC_SCOPE");
    unsetenv("TNY_TEST_ALLOC_FAIL_AT");
    tny_alloc_scope_begin("disabled");
    PASS();
}

TEST request_construction_oom_after_usage_skips_finalization(void) {
    /* This regression needs the complete allocator-instrumented object graph
     * (transport, session and provider), which the provider-fault host links.
     * A partially instrumented unit binary cannot inject the request fault. */
    tny_alloc_scope_begin("instrumentation-probe");
    free(xstrdup("instrumentation probe"));
    if (!tny_alloc_test_scope_count())
        SKIPm("transport objects are not allocator-instrumented in this binary");
    char isolated[] = "/tmp/tny-cli-request-XXXXXX";
    ASSERT(mkdtemp(isolated));
    char *saved_home = getenv("HOME") ? xstrdup(getenv("HOME")) : NULL;
    char *saved_tmp = getenv("TMPDIR") ? xstrdup(getenv("TMPDIR")) : NULL;
    char canonical_home[4096];
    ASSERT(realpath(isolated, canonical_home));
    setenv("HOME", canonical_home, 1);
    setenv("TMPDIR", canonical_home, 1);
    for (int cli = 0; cli < 2; ++cli)
        for (int wire = 0; wire < 2; ++wire)
            for (int mode = 0; mode < 4; mode++) {
                size_t construction_count = 0;
                for (size_t offset = 0; offset <= construction_count; ++offset) {
                    bool steer = mode == 1;
                    tny_alloc_scope_begin("disabled");
                    pv_fixture f;
                    pv_open(&f);
                    f.ctx->library_mode = !cli;
                    if (cli) {
                        char directory[1024], path[1100];
                        snprintf(directory, sizeof directory, "%s/skills/fixture", f.workspace);
                        ASSERT_EQ(0, mkdir_p(directory));
                        snprintf(path, sizeof path, "%s/SKILL.md", directory);
                        const char *skill = "---\nname: fixture\ndescription: dummy request "
                                            "catalog\n---\nNo external actions.\n";
                        ASSERT_EQ(0, file_write_atomic(path, skill, strlen(skill)));
                    }
                    free(f.ctx->wire_api);
                    f.ctx->wire_api = xstrdup(wire ? "chat" : "responses");
                    const char *schema =
                        "{\"type\":\"object\",\"properties\":{\"answer\":{\"type\":\"string\"}}}";
                    free(f.ctx->output_schema);
                    f.ctx->output_schema = tny_openai_response_format(schema, strlen(schema));
                    ASSERT(f.ctx->output_schema);
                    f.first_body =
                        steer
                            ? "{\"status\":\"completed\",\"usage\":{\"input_tokens\":123,\"output_"
                              "tokens\":7},"
                              "\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_"
                              "text\","
                              "\"text\":\"first answer\"}]}]}"
                            : "{\"status\":\"completed\",\"usage\":{\"input_tokens\":123,\"output_"
                              "tokens\":7},"
                              "\"output\":[{\"type\":\"function_call\",\"call_id\":\"request-oom\","
                              "\"name\":\"list_files\",\"arguments\":\"{}\"}]}";
                    if (wire)
                        f.first_body =
                            steer ? "{\"choices\":[{\"message\":{\"content\":\"first "
                                    "answer\"},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_"
                                    "tokens\":123,\"completion_tokens\":7}}"
                                  : "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"request-"
                                    "oom\",\"type\":\"function\",\"function\":{\"name\":\"list_"
                                    "files\","
                                    "\"arguments\":\"{}\"}}]},\"finish_reason\":\"tool_calls\"}],"
                                    "\"usage\":{\"prompt_tokens\":123,\"completion_tokens\":7}}";
                    tny_engine *engine = tny_engine_new(f.ctx, f.session, f.perm, NULL, NULL);
                    ASSERT(engine);
                    char err[512];
                    ASSERT_EQ(0, tny_engine_prepare(engine, f.backend, TNY_ENGINE_PREPARE_RESUMED,
                                                    err, sizeof err));
                    request_fault_fixture fault = {
                        .fixture = &f, .body = mode == 2, .parser = mode == 3, .offset = offset};
                    fault.path = path_join(f.session->dir, "session.json");
                    tny_backend_openai_bind(f.backend, f.session, f.perm, NULL, NULL, NULL, NULL,
                                            NULL, NULL, NULL, NULL, request_fault_control, &fault);
                    ASSERT_EQ(0, tny_engine_start(engine, "first", NULL, err, sizeof err));
                    if (steer) ASSERT_EQ(0, tny_engine_steer(engine, "continue", err, sizeof err));
                    for (int i = 0; i < 1000 && !fault.armed; i++) {
                        struct pollfd fds[HTTP_SERVER_POLLFD_CAPACITY + TNY_BACKEND_POLLFD_MAX];
                        int sn = http_server_pollfds(f.server, fds, HTTP_SERVER_POLLFD_CAPACITY);
                        int bn = tny_engine_pollfds(engine, fds + sn, TNY_BACKEND_POLLFD_MAX);
                        ASSERT(tny_poll(fds, (nfds_t)(sn + bn), 10) >= 0);
                        ASSERT_EQ(0, http_server_dispatch(f.server, fds, sn));
                        (void)tny_engine_dispatch(engine, fds + sn, bn);
                    }
                    ASSERT(fault.armed && fault.snapshot);
                    ASSERT(!fault.retained_view);
                    bool discovery = mode == 2 && offset == 0;
                    if (discovery) {
                        construction_count = fault.construction_count;
                        ASSERT(construction_count > 5);
                    } else {
                        ASSERT(tny_alloc_test_scope_injected());
                        ASSERT(tny_alloc_test_scope_count() >= fault.fault_index);
                    }
                    ASSERT_EQ(0, tny_alloc_test_settlement_allocations());
                    ASSERT_EQ(fault.parser ? 2 : 1,
                              f.requests); /* construction failure never submits */
                    unsetenv("TNY_TEST_ALLOC_SCOPE");
                    unsetenv("TNY_TEST_ALLOC_FAIL_AT");
                    size_t settled_count = tny_alloc_test_scope_count();
                    int errors = 0, terminals = 0;
                    tny_owned_event *event = NULL;
                    for (;;) {
                        tny_engine_next rc =
                            tny_engine_next_event(engine, 0, &event, err, sizeof err);
                        if (rc == TNY_ENGINE_NEXT_DRAINED) break;
                        ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, rc);
                        if (event->ev.kind == TNY_EV_ERROR) {
                            ASSERT_EQ(TNY_EVENT_ERROR_OOM, event->ev.error_code);
                            errors++;
                        }
                        if (event->ev.kind == TNY_EV_TURN_END) {
                            ASSERT_EQ(discovery ? 0 : 1, errors);
                            ASSERT_EQ(discovery ? TNY_STOP_INTERRUPTED : TNY_STOP_ERROR,
                                      event->ev.stop);
                            terminals++;
                        }
                        tny_owned_event_free(event);
                    }
                    ASSERT_EQ(discovery ? 0 : 1, errors);
                    ASSERT_EQ(1, terminals);
                    if (!discovery)
                        ASSERT_EQ(
                            settled_count,
                            tny_alloc_test_scope_count()); /* delivery also allocated nothing */
                    tny_alloc_scope_begin("disabled");
                    char *after = file_slurp(fault.path, NULL);
                    ASSERT(after);
                    if (!discovery) ASSERT_STR_EQ(fault.snapshot, after);
                    yyjson_doc *saved = jparse(after, strlen(after));
                    ASSERT(saved);
                    yyjson_val *usage = jget(yyjson_doc_get_root(saved), "usage");
                    ASSERT_EQ(123, jget_int(usage, "in", -1));
                    yyjson_doc_free(saved);
                    free(after);
                    free(fault.snapshot);
                    free(fault.path);
                    /* The same backend must admit a fresh request after every failure. */
                    tny_backend_openai_bind(f.backend, f.session, f.perm, NULL, NULL, NULL, NULL,
                                            NULL, NULL, NULL, NULL, NULL, NULL);
                    f.first_body = f.done_body =
                        wire ? "{\"choices\":[{\"message\":{\"content\":\"recovered\"},\"finish_"
                               "reason\":\"stop\"}]}"
                             : "{\"status\":\"completed\",\"output\":[]}";
                    native_pending_fixture recovered = {0};
                    ASSERT_EQ(0, tny_engine_start(engine, "recover", NULL, err, sizeof err));
                    for (int i = 0; i < 2000 && !recovered.ends; ++i)
                        ASSERT_EQ(0, native_pending_drive(&f, engine, &recovered));
                    ASSERT_EQ(1, recovered.ends);
                    ASSERT_EQ(0, recovered.errors);
                    ASSERT_EQ(TNY_STOP_DONE, recovered.stop);
                    tny_engine_free(engine); /* owns f.backend */
                    perm_free(f.perm);
                    session_close(f.session);
                    tny_ctx_free(f.ctx);
                    http_server_destroy(&f.server);
                    buf_free(&f.error_text);
                    buf_free(&f.callback_text);
                    for (size_t i = 0; i < sizeof f.bodies / sizeof f.bodies[0]; i++)
                        buf_free(&f.bodies[i]);
                }
            }
    if (saved_home) setenv("HOME", saved_home, 1);
    else unsetenv("HOME");
    if (saved_tmp) setenv("TMPDIR", saved_tmp, 1);
    else unsetenv("TMPDIR");
    free(saved_home);
    free(saved_tmp);
    PASS();
}
#endif

SUITE(openai_suite) {
#ifdef TNY_ALLOC_TESTING
    RUN_TEST(request_construction_oom_after_usage_skips_finalization);
    RUN_TEST(native_pending_lifecycle_and_allocation_sweeps);
    RUN_TEST(native_pending_transfer_preserves_source_on_failure);
    RUN_TEST(native_request_constructor_allocation_sweep);
    RUN_TEST(native_request_real_stale_replay_and_control_stop);
    RUN_TEST(native_cancel_headers_raw_error_and_retry_waits);
    RUN_TEST(native_request_reentrant_first_control_cancel);
    RUN_TEST(native_continuation_retains_text);
    RUN_TEST(native_checkpoint_retains_steer_and_consumed_index);
#endif
    RUN_TEST(cancellation_inside_decode_preserves_callback_and_reuse);
    RUN_TEST(cpp_ownership_boundary);
    RUN_TEST(provider_decoding_every_split);
    RUN_TEST(responses_reasoning_owns_unknown_fields);
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
    RUN_TEST(openai_preview_rides_the_next_request_with_captured_bytes);
    RUN_TEST(openai_preview_refuses_wrong_hash_and_foreign_roots);
    RUN_TEST(openai_preview_fatal_flush_stops_the_next_request);
    RUN_TEST(openai_preview_reports_non_delivery_when_the_turn_ends_early);
    RUN_TEST(openai_preview_needs_a_continuable_tool_batch);
    RUN_TEST(openai_selected_preview_allow_once_pins_before_permission);
    RUN_TEST(openai_selected_preview_terminal_cleanup_and_recovery);
    RUN_TEST(stream_complete_needs_a_terminal_event);
    RUN_TEST(stall_window_parses_and_clamps);
    RUN_TEST(continuation_trails_partial_then_user_turn);
}
