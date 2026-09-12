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
#include "core/config.h"
#include "core/image.h"
#include "core/perm.h"
#include "core/session.h"
#include "core/tools.h"
#include "net/http_server.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    int turn_requests; /* reset per turn: only its first POST asks for a tool */
    buf_t bodies[6];
    int hook_calls;
    pv_hook hook;
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
    response->status = 200;
    response->content_type = "application/json";
    response->body = body;
    response->body_len = strlen(body);
    return HTTP_SERVER_POST_HANDLED;
}

static void pv_event(const tny_backend_event *ev, void *ud) {
    pv_fixture *f = ud;
    if (ev->kind == TNY_EV_ERROR) {
        f->errors++;
        buf_append(&f->error_text, ev->text, ev->text_len);
        buf_appends(&f->error_text, "\n");
    }
    if (ev->kind == TNY_EV_TURN_END) {
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
            f->backend, f->png, f->hash_b, &f->code_refused, err, sizeof err);
        if (f->status_refused != TNY_IMAGE_PREVIEW_QUEUED) {
            const char *roots_code = NULL;
            tny_image_preview_status outside = tny_backend_openai_queue_image_preview(
                f->backend, f->outside, f->hash_a, &roots_code, err, sizeof err);
            if (outside != TNY_IMAGE_PREVIEW_FAILED ||
                strcmp(roots_code, TNY_IMAGE_PREVIEW_CODE_ROOTS) != 0)
                f->status_refused = TNY_IMAGE_PREVIEW_QUEUED; /* fails the assertion below */
        }
    }
    f->status = tny_backend_openai_queue_image_preview(f->backend, f->png, f->hash_a, &f->code, err,
                                                       sizeof err);
    if (f->hook == PV_HOOK_TWO_GENERATIONS) {
        /* the same output pathname is regenerated before the batch flushes */
        file_write_atomic(f->png, PV_PNG_B, sizeof PV_PNG_B);
        const char *code = NULL;
        f->status_second = tny_backend_openai_queue_image_preview(f->backend, f->png, f->hash_b,
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
TEST openai_preview_needs_a_continuable_tool_batch(void) {
    pv_fixture f;
    pv_open(&f);
    char err[256];
    const char *code = NULL;
    ASSERT_EQ(
        TNY_IMAGE_PREVIEW_TURN_NOT_READY,
        tny_backend_openai_queue_image_preview(f.backend, f.png, f.hash_a, &code, err, sizeof err));
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
    ASSERT_EQ(
        TNY_IMAGE_PREVIEW_TURN_NOT_READY,
        tny_backend_openai_queue_image_preview(f.backend, f.png, f.hash_a, &code, err, sizeof err));
    ASSERT(code);
    ASSERT_STR_EQ(TNY_IMAGE_PREVIEW_CODE_NOT_READY, code);

    /* and a non-native backend handle is never a session for this */
    tny_backend other = {0};
    other.id = TNY_BK_ACP;
    ASSERT_EQ(
        TNY_IMAGE_PREVIEW_UNAVAILABLE_SESSION,
        tny_backend_openai_queue_image_preview(&other, f.png, f.hash_a, &code, err, sizeof err));
    ASSERT(code);
    ASSERT_STR_EQ(TNY_IMAGE_PREVIEW_CODE_NO_SESSION, code);
    pv_close(&f);
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
    RUN_TEST(openai_preview_rides_the_next_request_with_captured_bytes);
    RUN_TEST(openai_preview_refuses_wrong_hash_and_foreign_roots);
    RUN_TEST(openai_preview_fatal_flush_stops_the_next_request);
    RUN_TEST(openai_preview_reports_non_delivery_when_the_turn_ends_early);
    RUN_TEST(openai_preview_needs_a_continuable_tool_batch);
}
