/* Native context preparation and isolated-runner rebind. */
#include "tui/tui.h"
#include "mcp/mcp.h"

void tui_prewarm_start(tui *t) {
    if (tui_runner_mode(t)) {
        if (t->turn_active) t->rc_restart_pending = true;
        return;
    }
    mcp_warm_start(t->ctx);
}

void tui_prewarm_drop(tui *t) {
    if (!t->rc) return;
    if (t->turn_active) t->rc_restart_pending = true;
    else tui_runner_drop(t, "rebind");
}
