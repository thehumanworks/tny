/* config.h — tnytty's minimal config file (docs/config.md, docs/adr/0005).
 *
 * `key = value` lines, `#` comments, no sections. Platform-free: parsing
 * is a pure function over a text buffer so it unit-tests without a
 * filesystem. Unknown keys warn; bad values are a clean error. */
#ifndef TNYTTY_CONFIG_H
#define TNYTTY_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    TT_TITLEBAR_TRANSPARENT = 0, /* default: content extends under the traffic lights */
    TT_TITLEBAR_OPAQUE = 1,      /* standard system titlebar showing the session title */
};

#define TT_CONFIG_FONT_MAX 96
#define TT_PALETTE_LEN     16

typedef struct {
    /* Empty = the platform's default monospaced face. */
    char font[TT_CONFIG_FONT_MAX];
    double font_size; /* points */
    int titlebar;     /* TT_TITLEBAR_* */
    int padding;      /* points of blank margin around the grid */

    /* Colors, 0x00RRGGBB. The defaults are a dark theme in which every
     * palette entry 1..15 clears WCAG 4.5:1 against the background
     * (docs/config.md has the table); index 0 is a background color and
     * is exempt. */
    uint32_t fg, bg;
    /* The 1-px rule drawn between split panes (docs/adr/0006). */
    uint32_t divider;
    uint32_t palette[TT_PALETTE_LEN];
    bool bold_brightens; /* SGR 1 on an indexed 0..7 fg also picks 8..15 */
    bool copy_on_select; /* releasing a drag copies to the pasteboard */
    bool status_bar;     /* one-line bar along the bottom edge */
    /* Opacity of cells using the default background, 0..100 percent. */
    int backdrop_opacity;
    /* Use the public system compositor material behind translucent cells. */
    bool backdrop_blur;
} tt_config;

typedef void (*tt_config_warn_fn)(void *user, const char *msg);

void tt_config_defaults(tt_config *c);

/* Apply `key = value` lines from text onto c. Returns 0, or -1 with err
 * filled by the first bad value (c is left partially applied; callers
 * treat the error as fatal). Unknown keys and malformed lines go to warn
 * (may be NULL) and do not fail the parse. */
int tt_config_parse(tt_config *c, const char *text, size_t len, char *err, size_t errcap,
                    tt_config_warn_fn warn, void *warn_user);

/* Apply one key/value pair. Same return contract as tt_config_parse. */
int tt_config_set(tt_config *c, const char *key, const char *value, char *err, size_t errcap);

/* $XDG_CONFIG_HOME/tnytty/config, else $HOME/.config/tnytty/config.
 * Returns buf, or NULL when neither variable is set. */
const char *tt_config_path(char *buf, size_t cap);

/* Defaults, then the config file if it exists. A missing file is not an
 * error; an unreadable or invalid file is (-1 with err filled). */
int tt_config_load(tt_config *c, char *err, size_t errcap, tt_config_warn_fn warn, void *warn_user);

#endif
