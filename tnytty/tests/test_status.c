/* test_status.c — the window status bar's message and expiry
 * (docs/adr/0005). Time is a parameter, not a clock, so the timing is
 * exact here and needs no timer thread in the product. */
#include "greatest.h"
#include "ui/reply.h"
#include "ui/status.h"
#include "vt/vt.h"

#include <string.h>

TEST copied_counts_characters_not_bytes(void) {
    tt_status s;
    tt_status_clear(&s);
    tt_status_copied(&s, "hello", 100.0);
    ASSERT_STR_EQ("Copied 5 characters", tt_status_text(&s));
    /* Four codepoints, ten bytes. */
    tt_status_copied(&s, "n\xc3\xa9\xe6\xbc\xa2\xf0\x9f\x9a\x80", 100.0);
    ASSERT_EQ(4u, tt_status_utf8_len("n\xc3\xa9\xe6\xbc\xa2\xf0\x9f\x9a\x80"));
    ASSERT_STR_EQ("Copied 4 characters", tt_status_text(&s));
    PASS();
}

TEST one_character_is_singular(void) {
    tt_status s;
    tt_status_clear(&s);
    tt_status_copied(&s, "x", 0.0);
    ASSERT_STR_EQ("Copied 1 character", tt_status_text(&s));
    PASS();
}

TEST copying_nothing_says_nothing(void) {
    tt_status s;
    tt_status_clear(&s);
    tt_status_copied(&s, "", 5.0);
    ASSERT_STR_EQ("", tt_status_text(&s));
    tt_status_copied(&s, NULL, 5.0);
    ASSERT_STR_EQ("", tt_status_text(&s));
    PASS();
}

TEST messages_expire_after_the_ttl(void) {
    tt_status s;
    tt_status_clear(&s);
    tt_status_copied(&s, "abc", 10.0);
    ASSERT(!tt_status_tick(&s, 10.0));
    ASSERT(!tt_status_tick(&s, 10.0 + TT_STATUS_TTL - 0.01));
    ASSERT_STR_EQ("Copied 3 characters", tt_status_text(&s));
    /* The tick that crosses the deadline reports the change once... */
    ASSERT(tt_status_tick(&s, 10.0 + TT_STATUS_TTL));
    ASSERT_STR_EQ("", tt_status_text(&s));
    /* ...and every tick after it is a no-op. */
    ASSERT(!tt_status_tick(&s, 100.0));
    PASS();
}

TEST a_new_message_restarts_the_clock(void) {
    tt_status s;
    tt_status_clear(&s);
    tt_status_copied(&s, "ab", 10.0);
    tt_status_copied(&s, "abcd", 11.5);
    ASSERT_STR_EQ("Copied 4 characters", tt_status_text(&s));
    ASSERT(!tt_status_tick(&s, 12.5)); /* would have expired under the first */
    ASSERT(tt_status_tick(&s, 13.5));
    PASS();
}

TEST long_messages_are_truncated_not_overflowed(void) {
    tt_status s;
    tt_status_clear(&s);
    char big[TT_STATUS_MAX * 2];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    tt_status_set(&s, big, 0.0);
    ASSERT(strlen(tt_status_text(&s)) < TT_STATUS_MAX);
    PASS();
}

/* ---- terminal replies ------------------------------------------------- */

typedef struct {
    char buf[64];
    size_t len;
} sink_state;

static int collect(void *user, const char *bytes, size_t len) {
    sink_state *st = user;
    if (st->len + len >= sizeof st->buf) return -1;
    memcpy(st->buf + st->len, bytes, len);
    st->len += len;
    st->buf[st->len] = '\0';
    return (int)len;
}

/* In the window there is no outer terminal to forward to, so DSR/DA
 * answers have to go back into the pty or programs that ask (vim, less,
 * shells probing the cursor column) hang waiting. */
TEST cursor_position_report_goes_back_to_the_child(void) {
    sink_state st = {{0}, 0};
    tt_reply r;
    vt *t = vt_new(80, 24, 0);
    tt_reply_attach(t, &r, collect, &st);

    vt_feed(t, "\x1b[6n", 4); /* DSR 6: report cursor position */
    ASSERT_STR_EQ("\x1b[1;1R", st.buf);

    st.len = 0;
    st.buf[0] = '\0';
    vt_feed(t, "\x1b[5;9Hx", 7); /* row 5, column 9, then advance one */
    vt_feed(t, "\x1b[6n", 4);
    ASSERT_STR_EQ("\x1b[5;10R", st.buf);
    ASSERT(r.bytes > 0);
    vt_free(t);
    PASS();
}

TEST device_attributes_are_answered(void) {
    sink_state st = {{0}, 0};
    tt_reply r;
    vt *t = vt_new(80, 24, 0);
    tt_reply_attach(t, &r, collect, &st);
    vt_feed(t, "\x1b[c", 3); /* primary DA */
    ASSERT(st.len > 0);
    ASSERT_EQ('\x1b', st.buf[0]);
    ASSERT_EQ('[', st.buf[1]);
    vt_free(t);
    PASS();
}

SUITE(status_suite) {
    RUN_TEST(copied_counts_characters_not_bytes);
    RUN_TEST(one_character_is_singular);
    RUN_TEST(copying_nothing_says_nothing);
    RUN_TEST(messages_expire_after_the_ttl);
    RUN_TEST(a_new_message_restarts_the_clock);
    RUN_TEST(long_messages_are_truncated_not_overflowed);
    RUN_TEST(cursor_position_report_goes_back_to_the_child);
    RUN_TEST(device_attributes_are_answered);
}
