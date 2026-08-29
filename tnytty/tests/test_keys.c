/* test_keys.c — key press to pty bytes (docs/adr/0005). Platform-free:
 * the macOS window classifies an NSEvent, this table does the encoding,
 * so the encoding is testable on every CI host. */
#include "greatest.h"
#include "ui/keys.h"

#include <string.h>

static char out[64];

static const char *enc(tt_key key, const char *text, unsigned mods, bool app_cursor) {
    size_t n =
        tt_key_encode(key, text, text ? strlen(text) : 0, mods, app_cursor, out, sizeof out - 1);
    out[n] = '\0';
    return out;
}

TEST printable_text_passes_through(void) {
    ASSERT_STR_EQ("a", enc(TT_KEY_TEXT, "a", 0, false));
    ASSERT_STR_EQ("A", enc(TT_KEY_TEXT, "A", TT_MOD_SHIFT, false));
    ASSERT_STR_EQ("\xc3\xa9", enc(TT_KEY_TEXT, "\xc3\xa9", 0, false));
    PASS();
}

TEST named_keys_are_control_bytes(void) {
    ASSERT_STR_EQ("\r", enc(TT_KEY_ENTER, "", 0, false));
    ASSERT_STR_EQ("\t", enc(TT_KEY_TAB, "", 0, false));
    ASSERT_STR_EQ("\x7f", enc(TT_KEY_BACKSPACE, "", 0, false));
    ASSERT_STR_EQ("\x1b", enc(TT_KEY_ESCAPE, "", 0, false));
    ASSERT_STR_EQ("\x1b[Z", enc(TT_KEY_TAB, "", TT_MOD_SHIFT, false));
    PASS();
}

TEST arrows_follow_decckm(void) {
    ASSERT_STR_EQ("\x1b[A", enc(TT_KEY_UP, "", 0, false));
    ASSERT_STR_EQ("\x1bOA", enc(TT_KEY_UP, "", 0, true));
    ASSERT_STR_EQ("\x1b[D", enc(TT_KEY_LEFT, "", 0, false));
    ASSERT_STR_EQ("\x1bOC", enc(TT_KEY_RIGHT, "", 0, true));
    /* Modified arrows always take the CSI 1;m form, app-cursor or not. */
    ASSERT_STR_EQ("\x1b[1;5C", enc(TT_KEY_RIGHT, "", TT_MOD_CTRL, true));
    ASSERT_STR_EQ("\x1b[1;3D", enc(TT_KEY_LEFT, "", TT_MOD_ALT, false));
    ASSERT_STR_EQ("\x1b[1;2A", enc(TT_KEY_UP, "", TT_MOD_SHIFT, false));
    PASS();
}

TEST editing_keys_use_tilde_forms(void) {
    ASSERT_STR_EQ("\x1b[3~", enc(TT_KEY_DELETE, "", 0, false));
    ASSERT_STR_EQ("\x1b[2~", enc(TT_KEY_INSERT, "", 0, false));
    ASSERT_STR_EQ("\x1b[5~", enc(TT_KEY_PAGE_UP, "", 0, false));
    ASSERT_STR_EQ("\x1b[6;5~", enc(TT_KEY_PAGE_DOWN, "", TT_MOD_CTRL, false));
    ASSERT_STR_EQ("\x1b[H", enc(TT_KEY_HOME, "", 0, false));
    ASSERT_STR_EQ("\x1bOF", enc(TT_KEY_END, "", 0, true));
    PASS();
}

TEST function_keys(void) {
    ASSERT_STR_EQ("\x1bOP", enc(TT_KEY_F1, "", 0, false));
    ASSERT_STR_EQ("\x1bOS", enc(TT_KEY_F4, "", 0, false));
    ASSERT_STR_EQ("\x1b[15~", enc(TT_KEY_F5, "", 0, false));
    ASSERT_STR_EQ("\x1b[24~", enc(TT_KEY_F12, "", 0, false));
    PASS();
}

TEST ctrl_letters_become_control_bytes(void) {
    ASSERT_STR_EQ("\x03", enc(TT_KEY_TEXT, "c", TT_MOD_CTRL, false));
    ASSERT_STR_EQ("\x03", enc(TT_KEY_TEXT, "C", TT_MOD_CTRL | TT_MOD_SHIFT, false));
    ASSERT_STR_EQ("\x01", enc(TT_KEY_TEXT, "a", TT_MOD_CTRL, false));
    ASSERT_STR_EQ("\x1c", enc(TT_KEY_TEXT, "\\", TT_MOD_CTRL, false));
    ASSERT_STR_EQ("\x1f", enc(TT_KEY_TEXT, "_", TT_MOD_CTRL, false));
    /* Ctrl-space is NUL: a zero-length string, so check the byte count. */
    ASSERT_EQ(1u, tt_key_encode(TT_KEY_TEXT, " ", 1, TT_MOD_CTRL, false, out, sizeof out));
    ASSERT_EQ('\0', out[0]);
    PASS();
}

TEST ctrl_without_a_control_byte_stays_literal(void) {
    ASSERT_STR_EQ("1", enc(TT_KEY_TEXT, "1", TT_MOD_CTRL, false));
    PASS();
}

TEST option_is_meta_esc_prefix(void) {
    ASSERT_STR_EQ("\x1b"
                  "b",
                  enc(TT_KEY_TEXT, "b", TT_MOD_ALT, false));
    ASSERT_STR_EQ("\x1b\r", enc(TT_KEY_ENTER, "", TT_MOD_ALT, false));
    ASSERT_STR_EQ("\x1b\x7f", enc(TT_KEY_BACKSPACE, "", TT_MOD_ALT, false));
    ASSERT_STR_EQ("\x1b\x02", enc(TT_KEY_TEXT, "b", TT_MOD_ALT | TT_MOD_CTRL, false));
    PASS();
}

TEST command_never_reaches_the_pty(void) {
    ASSERT_EQ(0u, tt_key_encode(TT_KEY_TEXT, "c", 1, TT_MOD_SUPER, false, out, sizeof out));
    ASSERT_EQ(0u, tt_key_encode(TT_KEY_ENTER, "", 0, TT_MOD_SUPER, false, out, sizeof out));
    PASS();
}

TEST empty_press_and_tight_buffers_write_nothing(void) {
    ASSERT_EQ(0u, tt_key_encode(TT_KEY_TEXT, NULL, 0, 0, false, out, sizeof out));
    ASSERT_EQ(0u, tt_key_encode(TT_KEY_TEXT, "", 0, 0, false, out, sizeof out));
    ASSERT_EQ(0u, tt_key_encode(TT_KEY_UP, "", 0, 0, false, out, 2));
    PASS();
}

SUITE(keys_suite) {
    RUN_TEST(printable_text_passes_through);
    RUN_TEST(named_keys_are_control_bytes);
    RUN_TEST(arrows_follow_decckm);
    RUN_TEST(editing_keys_use_tilde_forms);
    RUN_TEST(function_keys);
    RUN_TEST(ctrl_letters_become_control_bytes);
    RUN_TEST(ctrl_without_a_control_byte_stays_literal);
    RUN_TEST(option_is_meta_esc_prefix);
    RUN_TEST(command_never_reaches_the_pty);
    RUN_TEST(empty_press_and_tight_buffers_write_nothing);
}
