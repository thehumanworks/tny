/* The edit implementation alone redirects fchmod here at compile time. */
#include "core/edit.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(expr)                                                                   \
    do {                                                                              \
        if (!(expr)) {                                                                \
            fprintf(stderr, "edit mode assertion at line %d: %s\n", __LINE__, #expr); \
            return 1;                                                                 \
        }                                                                             \
    } while (0)
int tny_test_fchmod(int fd, mode_t mode);
int tny_test_fchmod(int fd, mode_t mode) {
    (void)fd;
    (void)mode;
    errno = EACCES;
    return -1;
}
int main(void) {
    const char *path = "mode-test.txt";
    CHECK(file_write_atomic(path, "old", 3) == 0);
    CHECK(chmod(path, 0755) == 0);
    int before = open("/dev/null", O_RDONLY);
    CHECK(before >= 0 && close(before) == 0);
    for (int i = 0; i < 20; ++i) {
        tny_edit_result result = {0};
        CHECK(tny_edit_file_exact(path, "old", "new", false, NULL, &result) ==
              TNY_EDIT_WRITE_ERROR);
        CHECK(result.error_number == EACCES && result.replaced == 0);
        tny_edit_result_free(&result);
        struct stat st;
        CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0755);
        char *data = file_slurp(path, NULL);
        CHECK(data && !strcmp(data, "old"));
        free(data);
        char temporary[128];
        snprintf(temporary, sizeof temporary, "%s.tmp.%d", path, getpid());
        CHECK(access(temporary, F_OK) == -1 && errno == ENOENT);
    }
    int after = open("/dev/null", O_RDONLY);
    CHECK(after == before && close(after) == 0);
    CHECK(unlink(path) == 0);
    puts("edit mode failure preserves content/mode and releases temp files/descriptors");
    return 0;
}
