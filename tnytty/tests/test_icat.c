/* test_icat.c — kitty graphics encoding: chunking, base64, PNG gate. */
#include "greatest.h"
#include "icat/icat.h"
#include "util/tt.h"

#include <stdlib.h>
#include <string.h>

static unsigned char *fake_png(size_t total, size_t *len_out) {
    static const unsigned char magic[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    unsigned char *buf = malloc(total);
    memcpy(buf, magic, sizeof magic);
    for (size_t i = sizeof magic; i < total; i++) buf[i] = (unsigned char)(i * 31);
    *len_out = total;
    return buf;
}

TEST base64_round_values(void) {
    char out[64];
    size_t n = tt_base64_encode(out, (const unsigned char *)"ABC", 3);
    ASSERT_EQ(4, (int)n);
    ASSERT_STR_EQ("QUJD", ((out[n] = 0), out));
    n = tt_base64_encode(out, (const unsigned char *)"AB", 2);
    ASSERT_STR_EQ("QUI=", ((out[n] = 0), out));
    n = tt_base64_encode(out, (const unsigned char *)"A", 1);
    ASSERT_STR_EQ("QQ==", ((out[n] = 0), out));
    PASS();
}

TEST small_png_single_chunk(void) {
    size_t len = 0;
    unsigned char *png = fake_png(64, &len);
    tt_buf out;
    tt_buf_init(&out);
    const char *err = NULL;
    ASSERT_EQ(0, tt_icat_encode(png, len, &out, &err));
    /* one chunk: final continuation from the start */
    ASSERT(strncmp(out.data, "\x1b_Ga=T,f=100,m=0;", 17) == 0);
    ASSERT(memcmp(out.data + out.len - 2, "\x1b\\", 2) == 0);
    /* the payload is the whole file, base64 */
    char b64[128];
    size_t bn = tt_base64_encode(b64, png, len);
    b64[bn] = 0;
    ASSERT(strstr(out.data, b64) != NULL);
    tt_buf_free(&out);
    free(png);
    PASS();
}

TEST large_png_chunks_with_continuations(void) {
    size_t len = 0;
    unsigned char *png = fake_png(4096, &len); /* b64 ~5462 => 2 chunks */
    tt_buf out;
    tt_buf_init(&out);
    const char *err = NULL;
    ASSERT_EQ(0, tt_icat_encode(png, len, &out, &err));
    ASSERT(strncmp(out.data, "\x1b_Ga=T,f=100,m=1;", 17) == 0);
    ASSERT(strstr(out.data, "\x1b_Gm=0;") != NULL);
    /* every chunk's payload stays within the 4096 base64 cap */
    const char *p = out.data;
    while ((p = strchr(p, ';')) != NULL) {
        const char *st = strstr(p, "\x1b\\");
        ASSERT(st != NULL);
        ASSERT((size_t)(st - p - 1) <= 4096);
        p = st;
    }
    tt_buf_free(&out);
    free(png);
    PASS();
}

TEST non_png_is_a_clean_error(void) {
    tt_buf out;
    tt_buf_init(&out);
    const char *err = NULL;
    ASSERT_EQ(-1, tt_icat_encode((const unsigned char *)"GIF89a....", 10, &out, &err));
    ASSERT(err != NULL);
    ASSERT(strstr(err, "PNG") != NULL);
    ASSERT_EQ(-1, tt_icat_encode((const unsigned char *)"", 0, &out, &err));
    tt_buf_free(&out);
    PASS();
}

SUITE(icat_suite) {
    RUN_TEST(base64_round_values);
    RUN_TEST(small_png_single_chunk);
    RUN_TEST(large_png_chunks_with_continuations);
    RUN_TEST(non_png_is_a_clean_error);
}
