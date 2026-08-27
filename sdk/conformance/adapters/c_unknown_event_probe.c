#include "tny/tny.h"
#include "core/runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    static const char payload[] = "future-payload";
    tny_owned_event owned = {0};
    owned.ev.kind = (tny_event_kind)UINT32_C(65535);
    owned.ev.text = payload;
    owned.ev.text_len = sizeof(payload) - 1;
    owned.sequence = 7;
    owned.timestamp_ms = 8;
    owned.provider = "fixture";
    owned.session_id = "fixture-session";
    owned.turn_id = "fixture-turn";

    const tny_event *event = (const tny_event *)(const void *)&owned;
    tny_event_view_v0 view;
    tny_event_view_init(&view);
    if (tny_event_read(event, &view) != TNY_STATUS_OK) return 1;
    if (tny_event_get_kind(event) != UINT32_C(65535) ||
        view.kind != UINT32_C(65535) || view.kind <= TNY_EVENT_TOOL_PROGRESS)
        return 2;
    if (view.text.len != sizeof(payload) - 1 ||
        memcmp(view.text.ptr, payload, sizeof(payload) - 1) != 0)
        return 3;
    tny_bytes legacy = tny_event_text(event);
    if (legacy.len != view.text.len ||
        memcmp(legacy.ptr, view.text.ptr, (size_t)view.text.len) != 0)
        return 4;
    printf("{\"type\":\"unknown\",\"sequence\":7,\"timestamp_ms\":8}\n");
    return 0;
}
