/* Real file-tool implementation, with only its environment/output hooks stubbed.
 * The driver compiles util.c with the same fread counter as tools_fs.c. */
#include <stdio.h>
#include <stdarg.h>
#include <sys/resource.h>
#include <time.h>

static size_t read_bytes;
size_t measured_fread(void *ptr, size_t size, size_t count, FILE *file) {
    size_t n = fread(ptr, size, count, file);
    read_bytes += n * size;
    return n;
}
#define fread measured_fread
#include TNY_FS_SOURCE
#undef fread

char *tool_resolve_path(tools_env *env, const char *path, char **error) {
    (void)env;
    (void)error;
    return xstrdup(path);
}
char *tool_bound_result(tools_env *env, const char *data, size_t len) {
    (void)env;
    return xstrndup(data, len);
}
void tools_learning_read_result(tools_env *env, tny_learning_event event, const char *path,
                                bool useful) {
    (void)env;
    (void)event;
    (void)path;
    (void)useful;
}
char *tool_err(const char *format, ...) {
    char text[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    return xstrdup(text);
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    yyjson_mut_doc *request = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *args = yyjson_mut_obj(request);
    yyjson_mut_doc_set_root(request, args);
    yyjson_mut_obj_add_str(request, args, "path", argv[1]);
    yyjson_mut_obj_add_int(request, args, "offset", 1);
    yyjson_mut_obj_add_int(request, args, "limit", 20);
    yyjson_doc *immutable = yyjson_mut_doc_imut_copy(request, NULL);
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    char *result = t_read_file(NULL, yyjson_doc_get_root(immutable));
    clock_gettime(CLOCK_MONOTONIC, &end);
    if (!result) return 3;
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    double rss = (double)usage.ru_maxrss;
#ifdef __APPLE__
    rss /= 1024;
#endif
    fprintf(stderr, "{\"read_bytes\":%zu,\"peak_rss_kib\":%.0f,\"elapsed_ms\":%.6f}\n", read_bytes,
            rss, (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1e6);
    fputs(result, stdout);
    free(result);
    yyjson_doc_free(immutable);
    yyjson_mut_doc_free(request);
    return 0;
}
