/* test_util.c — glob, codecs, buffers, strings, paths. */
#include "greatest.h"
#include "util/util.h"

#include <stdlib.h>
#include <string.h>

TEST glob_basics(void) {
    ASSERT(glob_match("*", ""));
    ASSERT(glob_match("*", "anything at all"));
    ASSERT(glob_match("a*b", "ab"));
    ASSERT(glob_match("a*b", "axxxb"));
    ASSERT_FALSE(glob_match("a*b", "axbz"));
    ASSERT(glob_match("?at", "cat"));
    ASSERT_FALSE(glob_match("?at", "at"));
    ASSERT(glob_match("a*b*c", "aXXbYYc"));
    ASSERT(glob_match("*ab", "aab")); /* needs backtracking */
    PASS();
}

TEST glob_crosses_slash(void) {
    /* '*' spans '/' per util.h — rule patterns like "git *" rely on it */
    ASSERT(glob_match("*.c", "src/main.c"));
    ASSERT(glob_match("git *", "git push origin/main"));
    PASS();
}

TEST glob_command_rules(void) {
    ASSERT(glob_match("git status*", "git status --short"));
    ASSERT_FALSE(glob_match("git status*", "git stash"));
    ASSERT(glob_match("npm run *", "npm run build"));
    ASSERT_FALSE(glob_match("npm run *", "npm install"));
    PASS();
}

TEST b64_roundtrip(void) {
    buf_t out;
    buf_init(&out);
    b64_encode((const uint8_t *)"foobar", 6, &out);
    ASSERT_STR_EQ("Zm9vYmFy", out.data);
    uint8_t dec[16];
    ASSERT_EQ_FMT((size_t)6, b64_decode(out.data, dec, sizeof dec), "%zu");
    ASSERT_MEM_EQ("foobar", dec, 6);
    buf_free(&out);

    uint8_t all[256];
    for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
    buf_init(&out);
    b64_encode(all, sizeof all, &out);
    uint8_t back[256];
    ASSERT_EQ_FMT(sizeof all, b64_decode(out.data, back, sizeof back), "%zu");
    ASSERT_MEM_EQ(all, back, sizeof all);
    buf_free(&out);
    PASS();
}

TEST sha1_known_vector(void) {
    /* FIPS 180-1 "abc" */
    static const uint8_t want[20] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                                     0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
    uint8_t got[20];
    sha1((const uint8_t *)"abc", 3, got);
    ASSERT_MEM_EQ(want, got, 20);
    PASS();
}

TEST fnv1a_stable(void) {
    ASSERT_EQ(fnv1a("workspace", 9), fnv1a("workspace", 9));
    ASSERT(fnv1a("a", 1) != fnv1a("b", 1));
    PASS();
}

TEST buf_ops(void) {
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "hello");
    buf_appendf(&b, " %d %s", 42, "world");
    ASSERT_STR_EQ("hello 42 world", b.data);
    buf_consume(&b, 6);
    ASSERT_STR_EQ("42 world", b.data);
    char *d = buf_detach(&b);
    ASSERT_STR_EQ("42 world", d);
    free(d);
    buf_init(&b);
    d = buf_detach(&b);
    ASSERT_STR_EQ("", d);
    free(d);
    PASS();
}

TEST str_helpers(void) {
    ASSERT(str_starts("git status --short", "git status"));
    ASSERT_FALSE(str_starts("git", "git status"));
    ASSERT(str_ends("session.json", ".json"));
    ASSERT_FALSE(str_ends("json", "session.json"));
    char s[] = "  padded\t\n";
    ASSERT_STR_EQ("padded", str_trim(s));
    PASS();
}

TEST path_helpers(void) {
    char *j = path_join("/a", "b");
    ASSERT_STR_EQ("/a/b", j);
    free(j);
    ASSERT(path_is_within("/a", "/a/b/c"));
    ASSERT_FALSE(path_is_within("/a", "/ab")); /* prefix, not a child */
    ASSERT_FALSE(path_is_within("/a/b", "/a"));
    PASS();
}

SUITE(util_suite) {
    RUN_TEST(glob_basics);
    RUN_TEST(glob_crosses_slash);
    RUN_TEST(glob_command_rules);
    RUN_TEST(b64_roundtrip);
    RUN_TEST(sha1_known_vector);
    RUN_TEST(fnv1a_stable);
    RUN_TEST(buf_ops);
    RUN_TEST(str_helpers);
    RUN_TEST(path_helpers);
}
