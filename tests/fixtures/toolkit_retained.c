/* Test-only link interposition around the REAL retained-artifact serializer.
 * Production sources contain no hook. A second thread cancels the public job
 * after the image has been committed and its record has already failed, which
 * is the exact moment a late cancellation must not erase the paid artifact's
 * detail (ADR 0095). The provider, the filesystem failure, the job lifecycle
 * and the error classification are all real. */
#include "core/image_service.h"
#include "tny/tny.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void tny_image_retained_json_original(const tny_image_request *, const tny_image_result *,
                                      const char *, buf_t *);

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool entered, released;

void tny_image_retained_json(const tny_image_request *request, const tny_image_result *result,
                             const char *message, buf_t *out) {
    /* Prove the real provider parser delivered both fields before filtering. */
    if (!result->have_seed || result->seed != 4242 ||
        strcmp(result->request_id, "fixture-request-id") != 0)
        abort();
    tny_image_retained_json_original(request, result, message, out);
    pthread_mutex_lock(&gate);
    entered = true;
    pthread_cond_broadcast(&changed);
    while (!released) pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
}

typedef struct {
    tny_toolkit_job *job;
    tny_error *error;
    int32_t status;
} call;

static void *run_job(void *opaque) {
    call *c = opaque;
    c->status = tny_toolkit_job_run(c->job, &c->error);
    return NULL;
}

static int check_case(const char *request, size_t len, bool cancel) {
    call c = {0};
    int rc = tny_toolkit_job_create((tny_bytes){request, len}, &c.job, &c.error);
    if (rc) {
        fprintf(stderr, "create failed: %d\n", rc);
        tny_error_free(c.error);
        return 1;
    }
    pthread_mutex_lock(&gate);
    entered = released = false;
    pthread_mutex_unlock(&gate);
    pthread_t worker;
    if (pthread_create(&worker, NULL, run_job, &c) != 0) {
        tny_toolkit_job_destroy(&c.job);
        return 1;
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 60;
    pthread_mutex_lock(&gate);
    while (!entered && !rc) rc = pthread_cond_timedwait(&changed, &gate, &deadline);
    bool observed = entered;
    pthread_mutex_unlock(&gate);
    /* The artifact is already committed and its record has already failed. */
    if (cancel && observed) rc = tny_toolkit_job_cancel(c.job);
    pthread_mutex_lock(&gate);
    released = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    pthread_join(worker, NULL);
    tny_bytes detail = tny_toolkit_job_result(c.job);
    bool status_ok = observed && !rc && c.status == TNY_STATUS_IO && c.error &&
                     tny_error_code(c.error) == TNY_STATUS_IO;
    char *raw_detail = detail.ptr ? xstrndup(detail.ptr, (size_t)detail.len) : NULL;
    yyjson_doc *doc = jparse(detail.ptr, (size_t)detail.len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *code = root ? jget_str(root, "code") : NULL;
    const char *path = root ? jget_str(root, "path") : NULL;
    bool detail_ok = code && strcmp(code, TNY_IMAGE_CODE_MANIFEST) == 0 &&
                     jget_bool(root, "committed", false) && path && *path &&
                     !yyjson_obj_get(root, "seed") && !yyjson_obj_get(root, "request_id") &&
                     raw_detail && !strstr(raw_detail, "fixture-request-id");
    printf("{\"cancel\":%s,\"status\":%d,\"detail_bytes\":%llu,\"code\":\"%s\","
           "\"serializer_observed\":%s}\n",
           cancel ? "true" : "false", c.status, (unsigned long long)detail.len, code ? code : "",
           observed ? "true" : "false");
    yyjson_doc_free(doc);
    free(raw_detail);
    tny_error_free(c.error);
    bool destroyed = tny_toolkit_job_destroy(&c.job) == 0 && !c.job;
    if (!status_ok || !detail_ok || !destroyed) {
        fprintf(stderr, "ASSERT a committed artifact must keep its retained IO detail\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *file = fopen(argv[1], "rb");
    if (!file) return 2;
    char request[4096];
    size_t n = fread(request, 1, sizeof request - 1, file);
    bool invalid = ferror(file) || n == sizeof request - 1;
    fclose(file);
    if (invalid) return 2;
    request[n] = 0;
    bool cancel = strcmp(argv[2], "cancel") == 0;
    if (!cancel && strcmp(argv[2], "control") != 0) return 2;
    return check_case(request, n, cancel);
}
