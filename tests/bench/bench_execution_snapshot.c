/* Production execution-state encoder benchmark; no provider or process launch. */
#include "core/execution_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    size_t turns = (size_t)strtoul(argv[1], NULL, 10);
    tny_ctx ctx = {.no_save = true};
    tny_session_state session = {.ctx = &ctx, .id = "benchmark", .dir = "/benchmark"};
    tools_env env = {.ctx = &ctx, .session = &session};
    session.doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(session.doc), *messages = yyjson_mut_arr(session.doc);
    yyjson_mut_doc_set_root(session.doc, root);
    if (!root || !messages || !yyjson_mut_obj_add_val(session.doc, root, "messages", messages))
        return 2;
    char content[16385];
    memset(content, 'x', sizeof(content) - 1);
    content[sizeof(content) - 1] = 0;
    for (size_t i = 0; i < turns; ++i) {
        yyjson_mut_val *message = yyjson_mut_obj(session.doc);
        if (!message || !yyjson_mut_obj_add_str(session.doc, message, "role", "user") ||
            !yyjson_mut_obj_add_strcpy(session.doc, message, "content", content) ||
            !yyjson_mut_arr_append(messages, message))
            return 2;
    }
    size_t bytes = 0;
    clock_t start = clock();
    const int repeats = 100;
    for (int i = 0; i < repeats; ++i) {
        yyjson_mut_doc *wire = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *state = tny_execution_state_encode(wire, &env, NULL, true);
        yyjson_mut_doc_set_root(wire, state);
        char *encoded = state ? yyjson_mut_write(wire, 0, &bytes) : NULL;
        if (!encoded) return 2;
        free(encoded);
        yyjson_mut_doc_free(wire);
    }
    double elapsed = (double)(clock() - start) / CLOCKS_PER_SEC;
    printf("{\"turns\":%zu,\"snapshot_bytes\":%zu,\"encode_cpu_ms\":%.6f}\n", turns, bytes,
           elapsed * 1000 / repeats);
    yyjson_mut_doc_free(session.doc);
    return 0;
}
