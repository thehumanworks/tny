#include "greatest.h"
#include "core/execution_protocol.h"
#include "core/execution_control.h"
#include <stdlib.h>
#include <string.h>

TEST execution_protocol_admission_truth_table(void) {
    const uint64_t ids[] = {0, 1, 2, 3, UINT64_MAX};
    for (int phase = -1; phase <= 4; ++phase) {
        for (int kind = -1; kind <= 8; ++kind) {
            for (size_t i = 0; i < sizeof(ids) / sizeof(*ids); ++i) {
                for (size_t j = 0; j < sizeof(ids) / sizeof(*ids); ++j) {
                    uint64_t id = ids[i], expected = ids[j];
                    bool allowed = false;
                    if (id && expected) {
                        switch (phase) {
                        case TNY_EXEC_WAIT_START:
                            allowed = kind == TNY_EXEC_EXECUTE && id == 1 && expected == 1;
                            break;
                        case TNY_EXEC_WAIT_REPLY:
                            allowed = kind == TNY_EXEC_RESULT && id == expected;
                            break;
                        case TNY_EXEC_WAIT_RESULT:
                            if (kind == TNY_EXEC_RESULT) allowed = id == 1;
                            else if (kind == TNY_EXEC_CONTROL || kind == TNY_EXEC_EVENT ||
                                     kind == TNY_EXEC_PROMPT || kind == TNY_EXEC_ASK ||
                                     kind == TNY_EXEC_STATE)
                                allowed = expected > 1 && id == expected;
                            break;
                        default: break;
                        }
                    }
                    ASSERT_EQ(allowed, tny_exec_protocol_admit((tny_exec_phase)phase,
                                                               (tny_exec_kind)kind, id, expected));
                }
            }
        }
    }
    PASS();
}
TEST execution_protocol_envelope_roundtrip(void) {
    const char *input = "{\"value\":[1,true,null]}";
    yyjson_doc *params = jparse(input, strlen(input));
    ASSERT(params);
    for (int kind = TNY_EXEC_EXECUTE; kind <= TNY_EXEC_RESULT; ++kind) {
        char *encoded = tny_exec_message(27, (tny_exec_kind)kind, yyjson_doc_get_root(params));
        ASSERT(encoded);
        yyjson_doc *doc = jparse(encoded, strlen(encoded));
        ASSERT(doc);
        uint64_t id = 0;
        yyjson_val *body = NULL;
        ASSERT_EQ((tny_exec_kind)kind, tny_exec_envelope(yyjson_doc_get_root(doc), &id, &body));
        ASSERT_EQ(27u, id);
        ASSERT(yyjson_equals(body, yyjson_doc_get_root(params)));
        yyjson_doc_free(doc);
        free(encoded);
    }
    yyjson_doc_free(params);
    ASSERT(tny_exec_message(1, TNY_EXEC_INVALID, NULL) == NULL);
    PASS();
}
TEST execution_protocol_rejects_malformed_envelopes(void) {
    const char *invalid[] = {
        "null",
        "[]",
        "{}",
        "{\"jsonrpc\":\"1.0\",\"version\":1,\"id\":1,\"result\":{}}",
        "{\"jsonrpc\":\"2.0\",\"version\":2,\"id\":1,\"result\":{}}",
        "{\"jsonrpc\":\"2.0\",\"version\":1,\"id\":0,\"result\":{}}",
        "{\"jsonrpc\":\"2.0\",\"version\":1,\"id\":-1,\"result\":{}}",
        "{\"jsonrpc\":\"2.0\",\"version\":1,\"id\":1.0,\"result\":{}}",
        "{\"jsonrpc\":\"2.0\",\"version\":1,\"id\":\"1\",\"result\":{}}",
        "{\"jsonrpc\":\"2.0\",\"version\":1,\"method\":\"execute\",\"params\":{}}",
        "{\"jsonrpc\":\"2.0\",\"version\":1,\"id\":1,\"error\":{}}",
        "{\"jsonrpc\":\"2.0\",\"version\":1,\"id\":1,\"result\":{},\"extra\":0}",
        "{\"jsonrpc\":\"2.0\",\"version\":1,\"id\":1,\"method\":\"unknown\",\"params\":{}}",
        "{\"jsonrpc\":\"2.0\",\"version\":1,\"id\":1,\"method\":\"execute\",\"params\":[]}",
        "{\"jsonrpc\":\"2.0\",\"version\":1,\"id\":1,\"id\":2,\"result\":{}}",
        "{\"jsonrpc\":\"2.0\",\"version\":1,\"id\":1,\"result\":{\"a\":1,\"a\":2}}",
        "{\"jsonrpc\":\"2.0\",\"version\":1,\"id\":1,\"result\":\"x\\u0000y\"}",
        "{\"jsonrpc\":\"2.0\",\"version\":1,\"id\":1,\"result\":{\"x\\u0000y\":1}}",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        yyjson_doc *doc = jparse(invalid[i], strlen(invalid[i]));
        ASSERT(doc);
        uint64_t id = 0;
        yyjson_val *body = NULL;
        ASSERT_EQ(TNY_EXEC_INVALID, tny_exec_envelope(yyjson_doc_get_root(doc), &id, &body));
        yyjson_doc_free(doc);
    }
    PASS();
}
TEST execution_protocol_json_complexity_bounds(void) {
    buf_t b;
    buf_init(&b);
    for (int i = 0; i < 65; ++i) buf_appends(&b, "[");
    buf_appends(&b, "null");
    for (int i = 0; i < 65; ++i) buf_appends(&b, "]");
    yyjson_doc *doc = jparse(b.data, b.len);
    ASSERT(doc);
    ASSERT_FALSE(tny_exec_json_valid(yyjson_doc_get_root(doc)));
    yyjson_doc_free(doc);
    buf_clear(&b);
    buf_appends(&b, "{");
    for (int i = 0; i < 513; ++i) buf_appendf(&b, "%s\"key%d\":0", i ? "," : "", i);
    buf_appends(&b, "}");
    doc = jparse(b.data, b.len);
    ASSERT(doc);
    ASSERT_FALSE(tny_exec_json_valid(yyjson_doc_get_root(doc)));
    yyjson_doc_free(doc);
    buf_free(&b);
    PASS();
}
static yyjson_doc *control_wire(yyjson_mut_val *value) {
    char *wire = jwrite_mut_val(value);
    yyjson_doc *doc = wire ? jparse(wire, strlen(wire)) : NULL;
    free(wire);
    return doc;
}

TEST execution_control_request_roundtrip(void) {
    const tny_openai_control_kind kinds[] = {
        TNY_OPENAI_CONTROL_PRE_TOOL, TNY_OPENAI_CONTROL_PERMISSION, TNY_OPENAI_CONTROL_POST_TOOL,
        TNY_OPENAI_CONTROL_SUBAGENT_START, TNY_OPENAI_CONTROL_SUBAGENT_END};
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        tny_openai_control_request source = {.kind = kinds[i],
                                             .tool_id = "nested-7",
                                             .tool_name = "read_file",
                                             .arguments_json = "{\"path\":\"after\"}",
                                             .original_arguments_json = "{\"path\":\"before\"}",
                                             .result = "contents",
                                             .original_ok = true,
                                             .control_extension = "fixture",
                                             .control_reason = "rewritten",
                                             .permission_summary = "read after",
                                             .permission_options =
                                                 TNY_PERM_ALLOW_ONCE | TNY_PERM_DENY,
                                             .subagent_id = "child",
                                             .subagent_action = "create",
                                             .subagent_outcome = "done",
                                             .subagent_ok = true};
        yyjson_mut_doc *encoded = yyjson_mut_doc_new(jallocator());
        yyjson_mut_val *value = tny_execution_control_encode_request(encoded, &source);
        ASSERT(value);
        yyjson_doc *wire = control_wire(value);
        ASSERT(wire);
        tny_openai_control_request decoded;
        ASSERT(tny_execution_control_decode_request(yyjson_doc_get_root(wire), &decoded));
        ASSERT_EQ(source.kind, decoded.kind);
        ASSERT_STR_EQ(source.tool_id, decoded.tool_id);
        ASSERT_STR_EQ(source.tool_name, decoded.tool_name);
        ASSERT_STR_EQ(source.arguments_json, decoded.arguments_json);
        ASSERT_STR_EQ(source.original_arguments_json, decoded.original_arguments_json);
        ASSERT_STR_EQ(source.result, decoded.result);
        ASSERT_EQ(source.original_ok, decoded.original_ok);
        ASSERT_STR_EQ(source.control_extension, decoded.control_extension);
        ASSERT_STR_EQ(source.control_reason, decoded.control_reason);
        ASSERT_STR_EQ(source.permission_summary, decoded.permission_summary);
        ASSERT_EQ(source.permission_options, decoded.permission_options);
        ASSERT_STR_EQ(source.subagent_id, decoded.subagent_id);
        ASSERT_STR_EQ(source.subagent_action, decoded.subagent_action);
        ASSERT_STR_EQ(source.subagent_outcome, decoded.subagent_outcome);
        ASSERT_EQ(source.subagent_ok, decoded.subagent_ok);
        yyjson_doc_free(wire);
        yyjson_mut_doc_free(encoded);
    }
    PASS();
}

TEST execution_control_response_owns_strings(void) {
    tny_openai_control_response source = {.arguments_json = "{}",
                                          .result = "replaced",
                                          .extension = "fixture",
                                          .reason = "reason",
                                          .deny = true,
                                          .stop = true,
                                          .result_replaced = true,
                                          .result_is_error = true,
                                          .permission = TNY_OPENAI_PERMISSION_DENY};
    yyjson_mut_doc *encoded = yyjson_mut_doc_new(jallocator());
    yyjson_mut_val *value = tny_execution_control_encode_response(encoded, &source);
    ASSERT(value);
    yyjson_doc *wire = control_wire(value);
    ASSERT(wire);
    tny_openai_control_response decoded;
    ASSERT(tny_execution_control_decode_response(yyjson_doc_get_root(wire), &decoded));
    ASSERT(decoded.result != jget_str(yyjson_doc_get_root(wire), "result"));
    yyjson_doc_free(wire);
    yyjson_mut_doc_free(encoded);
    ASSERT_STR_EQ(source.arguments_json, decoded.arguments_json);
    ASSERT_STR_EQ(source.result, decoded.result);
    ASSERT_STR_EQ(source.extension, decoded.extension);
    ASSERT_STR_EQ(source.reason, decoded.reason);
    ASSERT(decoded.deny && decoded.stop && decoded.result_replaced && decoded.result_is_error);
    ASSERT_EQ(source.permission, decoded.permission);
    tny_execution_control_response_free(&decoded);
    ASSERT_EQ(NULL, decoded.result);
    ASSERT_EQ(NULL, decoded.arguments_json);
    tny_execution_control_response_free(&decoded);
    PASS();
}

TEST execution_control_rejects_invalid_fields(void) {
    tny_openai_control_request source = {.kind = TNY_OPENAI_CONTROL_PRE_TOOL,
                                         .tool_id = "nested-1",
                                         .tool_name = "read_file",
                                         .arguments_json = "{}"};
    for (int variant = 0; variant < 7; variant++) {
        yyjson_mut_doc *encoded = yyjson_mut_doc_new(jallocator());
        yyjson_mut_val *value = tny_execution_control_encode_request(encoded, &source);
        ASSERT(value);
        if (variant == 0) yyjson_mut_obj_remove_key(value, "tool_name");
        if (variant == 1) yyjson_mut_obj_add_str(encoded, value, "unexpected", "value");
        if (variant == 2) yyjson_mut_obj_add_str(encoded, value, "tool_id", "duplicate");
        if (variant == 3 || variant == 4) {
            yyjson_mut_obj_remove_key(value, "kind");
            yyjson_mut_obj_add_int(encoded, value, "kind",
                                   variant == 3 ? TNY_OPENAI_CONTROL_PROVIDER_REQUEST : 99);
        }
        if (variant == 5) {
            yyjson_mut_obj_remove_key(value, "tool_name");
            yyjson_mut_obj_add_strncpy(encoded, value, "tool_name", "bad\0name", 8);
        }
        if (variant == 6) {
            yyjson_mut_obj_remove_key(value, "permission_options");
            yyjson_mut_obj_add_int(encoded, value, "permission_options", 128);
        }
        yyjson_doc *wire = control_wire(value);
        ASSERT(wire);
        tny_openai_control_request decoded;
        ASSERT_FALSE(tny_execution_control_decode_request(yyjson_doc_get_root(wire), &decoded));
        ASSERT_EQ(NULL, decoded.tool_id);
        yyjson_doc_free(wire);
        yyjson_mut_doc_free(encoded);
    }
    yyjson_mut_doc *encoded = yyjson_mut_doc_new(jallocator());
    source.kind = TNY_OPENAI_CONTROL_PROVIDER_RESPONSE;
    ASSERT_EQ(NULL, tny_execution_control_encode_request(encoded, &source));
    source.kind = TNY_OPENAI_CONTROL_PRE_TOOL;
    char oversized[4098];
    memset(oversized, 'x', sizeof oversized - 1);
    oversized[sizeof oversized - 1] = '\0';
    source.tool_name = oversized;
    ASSERT_EQ(NULL, tny_execution_control_encode_request(encoded, &source));
    tny_openai_control_response response = {.permission = (tny_openai_permission_decision)99};
    ASSERT_EQ(NULL, tny_execution_control_encode_response(encoded, &response));
    response.permission = TNY_OPENAI_PERMISSION_ABSTAIN;
    response.result_replaced = true;
    ASSERT_EQ(NULL, tny_execution_control_encode_response(encoded, &response));
    yyjson_mut_doc_free(encoded);
    PASS();
}

SUITE(execution_protocol_suite) {
    RUN_TEST(execution_protocol_admission_truth_table);
    RUN_TEST(execution_protocol_envelope_roundtrip);
    RUN_TEST(execution_protocol_rejects_malformed_envelopes);
    RUN_TEST(execution_protocol_json_complexity_bounds);
    RUN_TEST(execution_control_request_roundtrip);
    RUN_TEST(execution_control_response_owns_strings);
    RUN_TEST(execution_control_rejects_invalid_fields);
}
