#include "tny/tny.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static tny_bytes view(const char *text) {
    return (tny_bytes){text, (uint64_t)strlen(text)};
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    for (int cycle = 0; cycle < 100; cycle++) {
        tny_runtime_options_v0 options;
        if (tny_runtime_options_init(&options, sizeof options) != TNY_STATUS_OK)
            return 10;
        options.workspace = view(argv[1]);
        options.base_url = view(argv[2]);
        options.api_key = view("sanitizer-host-not-real");
        options.permission_mode = TNY_PERMISSION_YOLO;
        tny_runtime *runtime = NULL;
        tny_session *session = NULL;
        if (tny_runtime_create(&options, sizeof options, &runtime, NULL) != TNY_STATUS_OK ||
            tny_session_create(runtime, &session, NULL) != TNY_STATUS_OK)
            return 3;
        if (cycle == 0) {
            if (tny_session_send(session, view("hallucinate forbidden tool"),
                                 NULL) != TNY_STATUS_OK)
                return 4;
            int terminals = 0;
            for (;;) {
                tny_event *event = NULL;
                int32_t status = tny_session_next_event(session, 5000, &event,
                                                        NULL);
                if (status == TNY_STATUS_DRAINED) break;
                if (status != TNY_STATUS_EVENT || !event) return 5;
                if (tny_event_get_kind(event) == TNY_EVENT_TURN_END)
                    terminals++;
                tny_event_free(event);
            }
            if (terminals != 1) return 6;
            /* Parent destroy owns the still-attached session. */
            if (tny_runtime_destroy(&runtime) != TNY_STATUS_OK || runtime)
                return 7;
        } else {
            if (tny_session_destroy(&session) != TNY_STATUS_OK || session ||
                tny_session_destroy(&session) != TNY_STATUS_OK)
                return 8;
            if (tny_runtime_destroy(&runtime) != TNY_STATUS_OK || runtime ||
                tny_runtime_destroy(&runtime) != TNY_STATUS_OK)
                return 9;
        }
    }
    puts("libtny-sanitizer-host: lifecycle passed");
    return 0;
}
