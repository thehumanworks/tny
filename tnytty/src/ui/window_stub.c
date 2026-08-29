/* window_stub.c — every platform without a native renderer yet
 * (docs/platforms.md: `gui` is a clean error, not a crash or a silent
 * no-op). Compiled instead of window_macos.c off Darwin. */
#include "ui/window.h"

#include <stdio.h>

tt_window *tt_window_open(const tt_config *cfg, int cols, int rows, const char *title, char *err,
                          size_t errcap) {
    (void)cfg;
    (void)cols;
    (void)rows;
    (void)title;
    if (err && errcap) snprintf(err, errcap, "gui: not supported on this platform yet");
    return NULL;
}

void tt_window_close(tt_window *w) { (void)w; }

void tt_window_render_config(tt_window *w, tt_render_config *out) {
    (void)w;
    (void)out;
}

void tt_window_surface(tt_window *w, int *px_w, int *px_h) {
    (void)w;
    if (px_w) *px_w = 0;
    if (px_h) *px_h = 0;
}

bool tt_window_pump(tt_window *w, tt_win_ev *ev) {
    (void)w;
    (void)ev;
    return false;
}

void tt_window_present(tt_window *w, const uint32_t *px, int px_w, int px_h, int y0, int y1) {
    (void)w;
    (void)px;
    (void)px_w;
    (void)px_h;
    (void)y0;
    (void)y1;
}

void tt_window_set_title(tt_window *w, const char *title) {
    (void)w;
    (void)title;
}

void tt_window_set_clipboard(tt_window *w, const char *utf8, size_t len) {
    (void)w;
    (void)utf8;
    (void)len;
}

const char *tt_window_clipboard(tt_window *w) {
    (void)w;
    return NULL;
}
