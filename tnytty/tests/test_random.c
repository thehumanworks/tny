#include "greatest.h"
#include "util/tt.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static bool fault_rand_hex(char *out, size_t n);
#define tt_rand_hex fault_rand_hex
#include "fixtures/random_faults.c"
#undef tt_rand_hex

static void reset_fault(void *ud) {
    (void)ud;
    unsetenv("TNYTTY_FIXTURE_RANDOM");
    open_interrupted = read_interrupted = false;
    bytes_read = 0;
}

TEST random_hex_covers_empty_odd_and_multiple_chunk_lengths(void) {
    const size_t lengths[] = {0, 1, 15, 32, 128, 129, 257};
    char out[259];
    for (size_t i = 0; i < sizeof lengths / sizeof *lengths; i++) {
        memset(out, 'X', sizeof out);
        size_t n = lengths[i];
        ASSERT(tt_rand_hex(out, n));
        ASSERT_EQ(n, strlen(out));
        ASSERT_EQ(n, strspn(out, "0123456789abcdef"));
        ASSERT_EQ('X', out[n + 1]);
    }
    PASS();
}

TEST random_hex_clears_output_on_source_failures_including_after_a_complete_chunk(void) {
    const char *modes[] = {"open", "read", "eof", "partial", "late"};
    for (size_t i = 0; i < sizeof modes / sizeof *modes; i++) {
        char out[258];
        memset(out, 'X', sizeof out);
        ASSERT_EQ(0, setenv("TNYTTY_FIXTURE_RANDOM", modes[i], 1));
        ASSERT_FALSE(fault_rand_hex(out, sizeof out - 1));
        ASSERT_STR_EQ("", out);
        ASSERT_EQ(i ? EIO : EACCES, errno);
    }
    PASS();
}

TEST random_hex_retries_interrupted_open_and_fragmented_reads(void) {
    ASSERT_EQ(0, setenv("TNYTTY_FIXTURE_RANDOM", "interrupted", 1));
    char out[258];
    ASSERT(fault_rand_hex(out, sizeof out - 1));
    ASSERT(open_interrupted);
    ASSERT(read_interrupted);
    ASSERT_EQ(129u, bytes_read);
    ASSERT_EQ(sizeof out - 1, strlen(out));
    ASSERT_EQ(sizeof out - 1, strspn(out, "0123456789abcdef"));
    PASS();
}

SUITE(random_suite) {
    SET_SETUP(reset_fault, NULL);
    SET_TEARDOWN(reset_fault, NULL);
    RUN_TEST(random_hex_covers_empty_odd_and_multiple_chunk_lengths);
    RUN_TEST(random_hex_clears_output_on_source_failures_including_after_a_complete_chunk);
    RUN_TEST(random_hex_retries_interrupted_open_and_fragmented_reads);
}
