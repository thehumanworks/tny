/* Minimal libtny headless turn. Build with pkg-config after install, or see
 * tests/integration/test_libtny.py for an in-tree example. */
#include <tny/tny.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static tny_bytes bytes(const char *s) {
    tny_bytes b = {s, (uint64_t)strlen(s)};
    return b;
}

static int failed(const char *where, int32_t status, tny_error *error) {
    tny_bytes message = tny_error_message(error);
    fprintf(stderr, "%s: status %d: %.*s\n", where, status,
            (int)message.len, message.ptr ? message.ptr : "");
    tny_error_free(error);
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s BASE_URL WORKSPACE STATE_DIR\n", argv[0]);
        return 2;
    }
    tny_runtime_options_v0 options;
    tny_runtime_options_init(&options);
    options.workspace = bytes(argv[2]);
    options.state_dir = bytes(argv[3]);
    options.base_url = bytes(argv[1]);
    options.api_key = bytes("test-key-not-real");

    tny_runtime *runtime = NULL;
    tny_session *session = NULL;
    tny_error *error = NULL;
    int32_t status = tny_runtime_create(&options, &runtime, &error);
    if (status != TNY_STATUS_OK) return failed("runtime_create", status, error);
    status = tny_session_create(runtime, &session, &error);
    if (status != TNY_STATUS_OK) return failed("session_create", status, error);
    status = tny_session_send(session, bytes("list files in ."), &error);
    if (status != TNY_STATUS_OK) return failed("session_send", status, error);

    for (;;) {
        tny_event *event = NULL;
        status = tny_session_next_event(session, 5000, &event, &error);
        if (status == TNY_STATUS_TIMEOUT) continue;
        if (status == TNY_STATUS_DRAINED) break;
        if (status != TNY_STATUS_EVENT) return failed("next_event", status, error);
        uint32_t kind = tny_event_get_kind(event);
        if (kind == TNY_EVENT_TEXT_DELTA) {
            tny_bytes text = tny_event_text(event);
            fwrite(text.ptr, 1, (size_t)text.len, stdout);
        } else if (kind == TNY_EVENT_PERMISSION) {
            tny_bytes id = tny_event_permission_id(event);
            status = tny_session_respond_permission(session, id,
                                                    TNY_PERMISSION_DENY, &error);
            if (status != TNY_STATUS_OK) return failed("permission", status, error);
        }
        tny_event_free(event);
    }
    fputc('\n', stdout);
    tny_session_free(session);
    tny_runtime_free(runtime);
    return 0;
}
