#include "greatest.h"
#include "util/tt.h"

#include <stdint.h>
#include <string.h>

TEST buffer_overflow_preserves_contents_and_fails_closed(void) {
    const size_t lengths[] = {SIZE_MAX, SIZE_MAX - 4, SIZE_MAX - 5};
    for (size_t i = 0; i < sizeof lengths / sizeof *lengths; i++) {
        tt_buf b;
        tt_buf_init(&b);
        ASSERT(tt_buf_append(&b, "hello", 5));
        ASSERT_FALSE(tt_buf_append(&b, "", lengths[i]));
        ASSERT(b.oom);
        ASSERT_EQ(5u, b.len);
        ASSERT_STR_EQ("hello", b.data);
        ASSERT_FALSE(tt_buf_append(&b, "!", 1));
        ASSERT_FALSE(tt_buf_appendf(&b, "%s", "!"));
        ASSERT_STR_EQ("hello", b.data);
        tt_buf_free(&b);
    }
    PASS();
}

TEST buffer_growth_retains_bytes_and_terminator(void) {
    tt_buf b;
    tt_buf_init(&b);
    ASSERT(tt_buf_append(&b, NULL, 0));
    ASSERT_STR_EQ("", b.data);
    char bytes[1024];
    memset(bytes, 'x', sizeof bytes);
    ASSERT(tt_buf_append(&b, bytes, sizeof bytes));
    ASSERT(tt_buf_appendf(&b, " %d", 42));
    ASSERT_EQ(sizeof bytes + 3, b.len);
    ASSERT_MEM_EQ(bytes, b.data, sizeof bytes);
    ASSERT_STR_EQ(" 42", b.data + sizeof bytes);
    ASSERT_FALSE(b.oom);
    tt_buf_free(&b);
    PASS();
}

TEST listen_address_shared_grammar(void) {
    const struct {
        const char *spec, *host;
        int port;
    } valid[] = {{"127.0.0.1:7681", "127.0.0.1", 7681},
                 {"localhost:1", "localhost", 1},
                 {"::1:65535", "::1", 65535}};
    char host[128];
    int port = 0;
    for (size_t i = 0; i < sizeof valid / sizeof *valid; i++) {
        ASSERT_EQ(0, tt_parse_listen(valid[i].spec, host, sizeof host, &port));
        ASSERT_STR_EQ(valid[i].host, host);
        ASSERT_EQ(valid[i].port, port);
    }
    const char *invalid[] = {"",
                             "localhost",
                             ":7681",
                             "localhost:",
                             "localhost:0",
                             "localhost:-1",
                             "localhost:65536",
                             "localhost:80x"};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++)
        ASSERT_EQ(-1, tt_parse_listen(invalid[i], host, sizeof host, &port));
    ASSERT_EQ(-1, tt_parse_listen("localhost:7681", host, strlen("localhost"), &port));
    ASSERT_EQ(0, tt_parse_listen("localhost:7681", host, strlen("localhost") + 1, &port));
    PASS();
}

SUITE(util_suite) {
    RUN_TEST(buffer_overflow_preserves_contents_and_fails_closed);
    RUN_TEST(buffer_growth_retains_bytes_and_terminator);
    RUN_TEST(listen_address_shared_grammar);
}
