/* test_config.c — the config file (docs/config.md): key = value lines,
 * defaults, unknown keys warn, bad values are a clean error. */
#include "greatest.h"
#include "util/config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char warn_log[512];

static void collect(void *user, const char *msg) {
    (void)user;
    size_t used = strlen(warn_log);
    snprintf(warn_log + used, sizeof warn_log - used, "%s\n", msg);
}

static int parse(tt_config *c, const char *text, char *err, size_t errcap) {
    warn_log[0] = '\0';
    if (err) err[0] = '\0';
    return tt_config_parse(c, text, strlen(text), err, errcap, collect, NULL);
}

TEST defaults_are_transparent_menlo_13(void) {
    tt_config c;
    tt_config_defaults(&c);
    ASSERT_STR_EQ("", c.font); /* empty = the platform default face */
    ASSERT_EQ(TT_TITLEBAR_TRANSPARENT, c.titlebar);
    ASSERT_EQ(8, c.padding);
    ASSERT(c.font_size > 12.9 && c.font_size < 13.1);
    PASS();
}

TEST parses_keys_comments_and_blanks(void) {
    tt_config c;
    tt_config_defaults(&c);
    char err[128];
    const char *text = "# tnytty\n"
                       "\n"
                       "  font =  JetBrains Mono  \n"
                       "font-size=15.5\n"
                       "padding = 12\n"
                       "macos-titlebar = opaque\n";
    ASSERT_EQ(0, parse(&c, text, err, sizeof err));
    ASSERT_STR_EQ("JetBrains Mono", c.font);
    ASSERT(c.font_size > 15.4 && c.font_size < 15.6);
    ASSERT_EQ(12, c.padding);
    ASSERT_EQ(TT_TITLEBAR_OPAQUE, c.titlebar);
    ASSERT_STR_EQ("", warn_log);
    PASS();
}

TEST last_line_without_newline_still_applies(void) {
    tt_config c;
    tt_config_defaults(&c);
    ASSERT_EQ(0, parse(&c, "padding = 3", NULL, 0));
    ASSERT_EQ(3, c.padding);
    PASS();
}

TEST unknown_key_warns_and_keeps_going(void) {
    tt_config c;
    tt_config_defaults(&c);
    char err[128];
    ASSERT_EQ(0, parse(&c, "bogus = 1\npadding = 4\n", err, sizeof err));
    ASSERT_EQ(4, c.padding);
    ASSERT(strstr(warn_log, "unknown key bogus") != NULL);
    ASSERT(strstr(warn_log, "config:1") != NULL);
    PASS();
}

TEST line_without_equals_warns(void) {
    tt_config c;
    tt_config_defaults(&c);
    ASSERT_EQ(0, parse(&c, "padding 4\n", NULL, 0));
    ASSERT(strstr(warn_log, "no '='") != NULL);
    ASSERT_EQ(8, c.padding);
    PASS();
}

TEST bad_titlebar_is_a_clean_error(void) {
    tt_config c;
    tt_config_defaults(&c);
    char err[128];
    ASSERT_EQ(-1, parse(&c, "macos-titlebar = translucent\n", err, sizeof err));
    ASSERT(strstr(err, "line 1") != NULL);
    ASSERT(strstr(err, "translucent") != NULL);
    ASSERT(strstr(err, "transparent") != NULL);
    PASS();
}

TEST bad_numbers_are_clean_errors(void) {
    tt_config c;
    char err[128];
    tt_config_defaults(&c);
    ASSERT_EQ(-1, parse(&c, "font-size = huge\n", err, sizeof err));
    tt_config_defaults(&c);
    ASSERT_EQ(-1, parse(&c, "font-size = 0\n", err, sizeof err));
    tt_config_defaults(&c);
    ASSERT_EQ(-1, parse(&c, "padding = -1\n", err, sizeof err));
    tt_config_defaults(&c);
    ASSERT_EQ(-1, parse(&c, "padding = 4px\n", err, sizeof err));
    PASS();
}

TEST overlong_font_name_is_an_error(void) {
    tt_config c;
    tt_config_defaults(&c);
    char name[TT_CONFIG_FONT_MAX + 4];
    char text[TT_CONFIG_FONT_MAX + 64];
    char err[128];
    memset(name, 'a', sizeof name - 1);
    name[sizeof name - 1] = '\0';
    snprintf(text, sizeof text, "font = %s\n", name);
    ASSERT_EQ(-1, parse(&c, text, err, sizeof err));
    PASS();
}

TEST overlong_lines_are_rejected_not_truncated(void) {
    tt_config c;
    tt_config_defaults(&c);
    char text[600];
    char err[128];
    memset(text, 'x', sizeof text);
    memcpy(text, "padding = 4 ", 12);
    text[sizeof text - 1] = '\0';
    ASSERT_EQ(-1, parse(&c, text, err, sizeof err));
    ASSERT(strstr(err, "exceeds 511 bytes") != NULL);
    ASSERT_EQ(8, c.padding);
    PASS();
}

/* tt_config_set is the flag path too: --titlebar reuses the same code,
 * and an unknown key comes back as 1 so the CLI can reject it. */
TEST set_reports_unknown_keys_separately(void) {
    tt_config c;
    tt_config_defaults(&c);
    char err[128];
    ASSERT_EQ(1, tt_config_set(&c, "nope", "1", err, sizeof err));
    ASSERT_EQ(0, tt_config_set(&c, "macos-titlebar", "opaque", err, sizeof err));
    ASSERT_EQ(TT_TITLEBAR_OPAQUE, c.titlebar);
    ASSERT_EQ(-1, tt_config_set(&c, "macos-titlebar", "", err, sizeof err));
    PASS();
}

TEST color_keys_parse_hex(void) {
    tt_config c;
    tt_config_defaults(&c);
    char err[128];
    ASSERT_EQ(0, parse(&c, "foreground = #ff8800\nbackground = 102030\npalette4 = #1a2b3c\n", err,
                       sizeof err));
    ASSERT_EQ(0xff8800u, c.fg);
    ASSERT_EQ(0x102030u, c.bg);
    /* The rule between split panes is a colour like any other. */
    ASSERT_EQ(0x3a4152u, c.divider); /* still the default at this point */
    ASSERT_EQ(0, parse(&c, "divider = #445566\n", err, sizeof err));
    ASSERT_EQ(0x445566u, c.divider);
    ASSERT_EQ(-1, parse(&c, "divider = grey\n", err, sizeof err));
    ASSERT_EQ(0x1a2b3cu, c.palette[4]);
    ASSERT_EQ(0, parse(&c, "palette15 = #ffffff\n", err, sizeof err));
    ASSERT_EQ(0xffffffu, c.palette[15]);
    /* Out-of-range and malformed values are clean errors... */
    ASSERT_EQ(-1, parse(&c, "foreground = red\n", err, sizeof err));
    ASSERT_EQ(-1, parse(&c, "palette0 = #12345\n", err, sizeof err));
    ASSERT_EQ(-1, parse(&c, "palette3 = #12345g\n", err, sizeof err));
    /* ...but palette16 is simply not a key, so it only warns. */
    ASSERT_EQ(0, parse(&c, "palette16 = #ffffff\n", err, sizeof err));
    ASSERT(strstr(warn_log, "unknown key palette16") != NULL);
    PASS();
}

TEST bool_keys_parse(void) {
    tt_config c;
    tt_config_defaults(&c);
    char err[128];
    ASSERT(c.bold_brightens);
    ASSERT(c.copy_on_select);
    ASSERT_EQ(0, parse(&c, "bold-brightens = false\ncopy-on-select = no\n", err, sizeof err));
    ASSERT(!c.bold_brightens);
    ASSERT(!c.copy_on_select);
    ASSERT_EQ(0, parse(&c, "bold-brightens = 1\n", err, sizeof err));
    ASSERT(c.bold_brightens);
    ASSERT_EQ(-1, parse(&c, "copy-on-select = maybe\n", err, sizeof err));
    PASS();
}

/* WCAG 2.x relative luminance and contrast ratio. */
static double luminance(uint32_t c) {
    double ch[3];
    for (int i = 0; i < 3; i++) {
        double v = (double)((c >> (16 - 8 * i)) & 0xffu) / 255.0;
        ch[i] = v <= 0.03928 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
    }
    return 0.2126 * ch[0] + 0.7152 * ch[1] + 0.0722 * ch[2];
}

static double contrast(uint32_t a, uint32_t b) {
    double la = luminance(a), lb = luminance(b);
    if (la < lb) {
        double t = la;
        la = lb;
        lb = t;
    }
    return (la + 0.05) / (lb + 0.05);
}

/* The bug this guards: a stock xterm palette puts SGR 34 at #0000ee,
 * which is unreadable on a near-black background -- exactly what a bold
 * blue path segment in a shell prompt uses. Every default palette entry
 * except index 0 (a background color) must clear WCAG AA for body text.
 * docs/config.md carries the measured table. */
TEST default_palette_is_readable_on_the_default_background(void) {
    tt_config c;
    tt_config_defaults(&c);
    ASSERT(contrast(c.fg, c.bg) >= 7.0); /* default text: AAA */
    for (int i = 1; i < TT_PALETTE_LEN; i++) {
        double r = contrast(c.palette[i], c.bg);
        if (r < 4.5) {
            char msg[128];
            snprintf(msg, sizeof msg, "palette%d #%06x is only %.2f:1 on the background", i,
                     c.palette[i], r);
            FAILm(msg);
        }
    }
    PASS();
}

TEST path_prefers_xdg_config_home(void) {
    char buf[512];
    char saved_home[512] = {0};
    const char *home = getenv("HOME");
    if (home) snprintf(saved_home, sizeof saved_home, "%s", home);
    setenv("XDG_CONFIG_HOME", "/tmp/xdg", 1);
    ASSERT_STR_EQ("/tmp/xdg/tnytty/config", tt_config_path(buf, sizeof buf));
    unsetenv("XDG_CONFIG_HOME");
    setenv("HOME", "/tmp/home", 1);
    ASSERT_STR_EQ("/tmp/home/.config/tnytty/config", tt_config_path(buf, sizeof buf));
    if (saved_home[0]) setenv("HOME", saved_home, 1);
    PASS();
}

SUITE(config_suite) {
    RUN_TEST(defaults_are_transparent_menlo_13);
    RUN_TEST(parses_keys_comments_and_blanks);
    RUN_TEST(last_line_without_newline_still_applies);
    RUN_TEST(unknown_key_warns_and_keeps_going);
    RUN_TEST(line_without_equals_warns);
    RUN_TEST(bad_titlebar_is_a_clean_error);
    RUN_TEST(bad_numbers_are_clean_errors);
    RUN_TEST(overlong_font_name_is_an_error);
    RUN_TEST(overlong_lines_are_rejected_not_truncated);
    RUN_TEST(set_reports_unknown_keys_separately);
    RUN_TEST(color_keys_parse_hex);
    RUN_TEST(bool_keys_parse);
    RUN_TEST(default_palette_is_readable_on_the_default_background);
    RUN_TEST(path_prefers_xdg_config_home);
}
