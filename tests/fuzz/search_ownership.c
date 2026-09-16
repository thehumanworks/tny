/* Exercise the actual private search decoder under allocation faults. The
 * exported service is renamed only to link this copy beside the full core. */
#define tool_web_search_codex tny_search_ownership_unused_service
#include "../../src/core/search_codex.c"
#include "util/alloc.h"

static void fault_at(size_t index) {
    char value[32];
    snprintf(value, sizeof value, "%zu", index);
    setenv("TNY_TEST_ALLOC_SCOPE", "search-owner", 1);
    setenv("TNY_TEST_ALLOC_FAIL_AT", value, 1);
    tny_alloc_scope_begin("search-owner");
}

static void clear_response(search_response *response) {
    yyjson_mut_doc_free(response->items_doc);
    buf_free(&response->text);
    buf_free(&response->sources);
}

int main(void) {
    static const char complete[] =
        "{\"status\":\"completed\",\"output\":["
        "{\"type\":\"web_search_call\",\"id\":\"search\",\"status\":\"completed\"},"
        "{\"type\":\"message\",\"id\":\"message\",\"content\":["
        "{\"type\":\"output_text\",\"text\":\"answer\",\"annotations\":["
        "{\"type\":\"url_citation\",\"url\":\"https://example.com/source\"}]}]}]}";
    fault_at(0);
    search_response original = {0};
    search_event(complete, sizeof complete - 1, &original);
    size_t count = tny_alloc_test_scope_count();
    bool success = original.complete && !original.error && original.text.data &&
                   !strcmp(original.text.data, "answer");
    clear_response(&original);
    if (!success || !count) return 1;
    for (size_t index = 1; index <= count; ++index) {
        search_response response = {0};
        fault_at(index);
        search_event(complete, sizeof complete - 1, &response);
        bool rejected = tny_alloc_test_scope_injected() && !response.complete && response.error &&
                        !strcmp(response.error, "out of memory");
        clear_response(&response);
        if (!rejected) {
            fprintf(stderr, "search OOM classification failed at allocation %zu\n", index);
            return 1;
        }
    }
    fault_at(0);
    search_response invalid = {0};
    search_event("{", 1, &invalid);
    if (!invalid.error || strcmp(invalid.error, "malformed Codex search event")) return 1;
    search_response recovered = {0};
    search_event(complete, sizeof complete - 1, &recovered);
    success = recovered.complete && !recovered.error;
    clear_response(&recovered);
    printf("search ownership: %zu allocation failures, malformed input and recovery\n", count);
    return success ? 0 : 1;
}
