#include "json/ownership.hpp"
#include "backends/openai/stream_decode.h"
#include <cstdio>
#include <cstring>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<tny::document>);
static_assert(!std::is_copy_assignable_v<tny::mutable_document>);
static_assert(std::is_nothrow_move_constructible_v<tny::document>);
static_assert(!std::is_copy_constructible_v<tny::owned<tny::string>>);
static_assert(std::is_nothrow_destructible_v<tny::mutable_document>);

#define OWN_CHECK(x)               \
    do {                           \
        if (!(x)) return __LINE__; \
    } while (false)
namespace {
struct construction {
    tny::string value;
    explicit construction(bool fail) : value(128, 'x') {
        if (fail) throw std::bad_alloc();
    }
};
struct capture {
    char text[2048]{};
    size_t len = 0;
    int done = 0, events = 0;
};
int collect(const oa_decoded_event *event, void *ud) {
    auto &c = *static_cast<capture *>(ud);
    ++c.events;
    if (event->kind == OA_DECODE_TEXT && event->len <= sizeof c.text - c.len - 1) {
        std::memcpy(c.text + c.len, event->text, event->len);
        c.len += event->len;
    }
    if (event->kind == OA_DECODE_DONE) ++c.done;
    return TNY_PARSE_OK;
}
int reject_event(const oa_decoded_event *, void *ud) {
    ++*static_cast<int *>(ud);
    return TNY_PARSE_OOM;
}
void count_sse(const char *, size_t, void *ud) { ++*static_cast<int *>(ud); }

constexpr char chat[] =
    R"({"choices":[{"delta":{"content":"hello","reasoning_content":"retained thinking that exceeds small string storage","reasoning_details":[{"index":0,"text":"reason","unknown":{"signed":"payload"}}],"tool_calls":[{"index":0,"id":"long_call_identity_exceeding_small_string_storage","function":{"name":"read_file","arguments":"{\"path\":\"long retained path for allocation failure tests\"}"}}]}}]})";
#ifdef TNY_ALLOC_TESTING
void fault_at(size_t index) {
    char number[32];
    std::snprintf(number, sizeof number, "%zu", index);
    setenv("TNY_TEST_ALLOC_SCOPE", "parser-test", 1);
    setenv("TNY_TEST_ALLOC_FAIL_AT", number, 1);
    tny_alloc_scope_begin("parser-test");
}
#endif
} // namespace

extern "C" int tny_ownership_selftest(void) {
    bool caught = false;
    try {
        auto owner = tny::make_owned<construction>(true);
    } catch (const std::bad_alloc &) { caught = true; }
    OWN_CHECK(caught);
    auto original = tny::make_owned<construction>(false);
    auto moved = std::move(original);
    OWN_CHECK(!original && moved->value.size() == 128);
    oa_decoder decoder{};
    oa_callset calls{};
    capture got;
    OWN_CHECK(oa_decoder_feed(&decoder, &calls, true, false, "{", 1, collect, &got) ==
              TNY_PARSE_INVALID);
    OWN_CHECK(oa_decoder_feed(&decoder, &calls, true, false, chat, sizeof chat - 1, collect,
                              &got) == TNY_PARSE_OK);
    OWN_CHECK(!std::strcmp(got.text, "hello") && calls.n == 1);
    /* Input documents are gone. Churn allocator blocks before reading owners. */
    for (int i = 0; i < 10; ++i) { auto junk = tny::parse("{\"junk\":true}", 13); }
    OWN_CHECK(!std::strcmp(calls.calls[0].name, "read_file"));
    OWN_CHECK(!std::strcmp(calls.calls[0].id, "long_call_identity_exceeding_small_string_storage"));
    OWN_CHECK(std::strstr(calls.calls[0].args.data, "long retained path"));
    char *extras = nullptr;
    OWN_CHECK(oa_decoder_extras(&decoder, &extras) == TNY_PARSE_OK);
    tny::c_string extra_owner(extras);
    OWN_CHECK(extras && std::strstr(extras, "unknown") && std::strstr(extras, "payload"));
    const char more[] =
        R"([{"index":0,"id":"fresh","function":{"name":"other","arguments":"{}"}}])";
    auto more_doc = tny::parse(more, sizeof more - 1);
    OWN_CHECK(oa_calls_feed(&calls, yyjson_doc_get_root(more_doc.get())) == TNY_PARSE_OK);
    OWN_CHECK(calls.n == 2 && !std::strcmp(calls.calls[1].id, "fresh"));
    OWN_CHECK(!std::strcmp(calls.calls[0].id, "long_call_identity_exceeding_small_string_storage"));
    oa_decoder_reset(&decoder);
    oa_calls_reset(&calls);
    int rejected = 0;
    OWN_CHECK(oa_decoder_feed(&decoder, &calls, true, false, chat, sizeof chat - 1, reject_event,
                              &rejected) == TNY_PARSE_OOM);
    OWN_CHECK(rejected == 1);
    OWN_CHECK(oa_decoder_feed(&decoder, &calls, true, false, "[DONE]", 6, reject_event,
                              &rejected) == TNY_PARSE_OOM);
    OWN_CHECK(rejected == 1);
    oa_decoder_reset(&decoder);
    oa_calls_reset(&calls);
    got = {};
    OWN_CHECK(oa_decoder_feed(&decoder, &calls, true, false, chat, sizeof chat - 1, collect,
                              &got) == TNY_PARSE_OK);
    OWN_CHECK(got.events > 1 && !std::strcmp(got.text, "hello"));
    oa_decoder_reset(&decoder);
    oa_calls_reset(&calls);
#ifdef TNY_ALLOC_TESTING
    fault_at(0);
    got = {};
    OWN_CHECK(oa_decoder_feed(&decoder, &calls, true, false, chat, sizeof chat - 1, collect,
                              &got) == TNY_PARSE_OK);
    size_t count = tny_alloc_test_scope_count();
    oa_decoder_reset(&decoder);
    oa_calls_reset(&calls);
    OWN_CHECK(count > 5);
    for (size_t i = 1; i <= count; ++i) {
        /* Repeated failures, then recovery of the same C facade. */
        for (int repeat = 0; repeat < 2; ++repeat) {
            fault_at(i);
            got = {};
            int rc = oa_decoder_feed(&decoder, &calls, true, false, chat, sizeof chat - 1, collect,
                                     &got);
            OWN_CHECK(tny_alloc_test_scope_injected());
            OWN_CHECK(rc == TNY_PARSE_OOM && got.events == 0);
            OWN_CHECK(oa_decoder_feed(&decoder, &calls, true, false, "[DONE]", 6, collect, &got) ==
                      TNY_PARSE_OOM);
            oa_decoder_reset(&decoder);
            oa_calls_reset(&calls);
        }
        fault_at(0);
        got = {};
        OWN_CHECK(oa_decoder_feed(&decoder, &calls, true, false, chat, sizeof chat - 1, collect,
                                  &got) == TNY_PARSE_OK);
        OWN_CHECK(calls.n == 1 && !std::strcmp(got.text, "hello"));
        oa_decoder_reset(&decoder);
        oa_calls_reset(&calls);
    }
    /* Sweep serialization too: failure cannot return partial extras. */
    size_t extra_count = 0;
    for (size_t i = 0; i <= extra_count; ++i) {
        fault_at(0);
        got = {};
        OWN_CHECK(oa_decoder_feed(&decoder, &calls, true, false, chat, sizeof chat - 1, collect,
                                  &got) == TNY_PARSE_OK);
        fault_at(i);
        extras = nullptr;
        int rc = oa_decoder_extras(&decoder, &extras);
        tny::c_string serialized(extras);
        if (i == 0) {
            OWN_CHECK(rc == TNY_PARSE_OK && extras);
            extra_count = tny_alloc_test_scope_count();
            OWN_CHECK(extra_count > 0);
        } else {
            OWN_CHECK(tny_alloc_test_scope_injected());
            OWN_CHECK(rc == TNY_PARSE_OOM && !extras);
        }
        oa_decoder_reset(&decoder);
        oa_calls_reset(&calls);
    }
    /* Distinguish JSON parse allocation failure from malformed JSON. */
    fault_at(1);
    got = {};
    OWN_CHECK(oa_decoder_feed(&decoder, &calls, true, false, "{}", 2, collect, &got) ==
              TNY_PARSE_OOM);
    oa_decoder_reset(&decoder);
    fault_at(0);
    OWN_CHECK(oa_decoder_feed(&decoder, &calls, true, false, "{", 1, collect, &got) ==
              TNY_PARSE_INVALID);
    oa_decoder_reset(&decoder);
    const char *wire = "data: a long SSE event which exceeds small string storage\n\n";
    for (size_t i = 1; i <= 3; ++i) {
        sse_parser parser;
        sse_parser_init(&parser);
        fault_at(i);
        int events = 0;
        OWN_CHECK(sse_feed(&parser, wire, std::strlen(wire), count_sse, &events) == TNY_PARSE_OOM);
        OWN_CHECK(events == 0);
        OWN_CHECK(sse_flush(&parser, count_sse, &events) == TNY_PARSE_OOM);
        sse_parser_free(&parser);
    }
    fault_at(0);
    unsetenv("TNY_TEST_ALLOC_SCOPE");
    unsetenv("TNY_TEST_ALLOC_FAIL_AT");
#endif
    /* More than the inline action count forces moves of generated citation
     * strings. Views must be rebuilt after every vector/small-string move. */
    tny::string hosted =
        R"({"output":[{"type":"message","id":"m","status":"completed","content":[{"type":"output_text","text":"hello","annotations":[)";
    for (int i = 0; i < 12; ++i) {
        if (i) hosted += ',';
        hosted += R"({"type":"url_citation","url":"https://x.test"})";
    }
    hosted += R"(]}]}],"status":"completed"})";
    got = {};
    OWN_CHECK(oa_decoder_feed(&decoder, &calls, false, true, hosted.data(), hosted.size(), collect,
                              &got) == TNY_PARSE_OK);
    OWN_CHECK(got.done == 1 && got.len == 5 + 12 * std::strlen("\n[Source](https://x.test)\n"));
    OWN_CHECK(std::strstr(got.text, "hello\n[Source](https://x.test)\n"));
    oa_decoder_reset(&decoder);
    oa_calls_reset(&calls);
#ifdef TNY_ALLOC_TESTING
    size_t hosted_count = 0;
    for (size_t i = 0; i <= hosted_count; ++i) {
        fault_at(i);
        got = {};
        int rc = oa_decoder_feed(&decoder, &calls, false, true, hosted.data(), hosted.size(),
                                 collect, &got);
        if (!i) {
            OWN_CHECK(rc == TNY_PARSE_OK);
            hosted_count = tny_alloc_test_scope_count();
            OWN_CHECK(hosted_count > 0);
        } else {
            OWN_CHECK(tny_alloc_test_scope_injected());
            OWN_CHECK(rc == TNY_PARSE_OOM && got.events == 0);
        }
        oa_decoder_reset(&decoder);
        oa_calls_reset(&calls);
    }
    fault_at(0);
    unsetenv("TNY_TEST_ALLOC_SCOPE");
    unsetenv("TNY_TEST_ALLOC_FAIL_AT");
#endif
    const char checkpoint[] =
        R"([{"id":"one","index":-1,"name":"tool","args":"{}"},{"id":"two","index":-1,"name":"tool","args":"[]"}])";
    auto records = tny::parse(checkpoint, sizeof checkpoint - 1);
    OWN_CHECK(oa_calls_restore(&calls, yyjson_doc_get_root(records.get())) == TNY_PARSE_OK);
    records.reset();
    OWN_CHECK(calls.n == 2 && calls.calls[0].wire_index == -1 && calls.calls[1].wire_index == -1);
    OWN_CHECK(!std::strcmp(calls.calls[1].id, "two") &&
              !std::strcmp(calls.calls[1].args.data, "[]"));
    oa_calls_reset(&calls);
    /* Empty feeds and reset are allocation-free and valid repeatedly. */
    sse_parser parser;
    sse_parser_init(&parser);
    int events = 0;
    OWN_CHECK(sse_feed(&parser, nullptr, 0, count_sse, &events) == TNY_PARSE_OK);
    OWN_CHECK(sse_flush(&parser, count_sse, &events) == TNY_PARSE_OK);
    sse_parser_free(&parser);
    sse_parser_free(&parser);
    return 0;
}
#ifdef TNY_OWNERSHIP_STANDALONE
int main() {
    int result = tny_ownership_selftest();
    if (result) std::fprintf(stderr, "ownership self-test failed at line %d\n", result);
    return result ? 1 : 0;
}
#endif
