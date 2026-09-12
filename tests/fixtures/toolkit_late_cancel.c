/* Test-only link interposition around the REAL strict-result serializer.
 * Production sources contain no hook. A second thread cancels the public job
 * only after the completed local error JSON exists, before run() finalizes. */
#include "core/image_service.h"
#include "tny/tny.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void tny_image_error_json_original(const tny_image_request *, const tny_image_result *,
                                   const char *, buf_t *);

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool entered, released;
static unsigned callbacks;

void tny_image_error_json(const tny_image_request *request, const tny_image_result *result,
                          const char *message, buf_t *out) {
    tny_image_error_json_original(request, result, message, out);
    pthread_mutex_lock(&gate);
    entered = true;
    callbacks++;
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
    deadline.tv_sec += 10;
    pthread_mutex_lock(&gate);
    while (!entered && !rc) rc = pthread_cond_timedwait(&changed, &gate, &deadline);
    bool observed = entered;
    pthread_mutex_unlock(&gate);
    if (cancel && observed) rc = tny_toolkit_job_cancel(c.job);
    pthread_mutex_lock(&gate);
    released = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    pthread_join(worker, NULL);
    int32_t expected = cancel ? TNY_STATUS_CANCELLED : TNY_STATUS_INVALID_ARGUMENT;
    tny_bytes detail = tny_toolkit_job_result(c.job);
    bool status_ok =
        observed && !rc && c.status == expected && c.error && tny_error_code(c.error) == expected;
    bool detail_ok;
    if (cancel) detail_ok = detail.len == 0;
    else {
        yyjson_doc *doc = jparse(detail.ptr, (size_t)detail.len);
        const char *code = doc ? jget_str(yyjson_doc_get_root(doc), "code") : NULL;
        detail_ok = code && strcmp(code, TNY_IMAGE_CODE_STRICT_INVALID) == 0;
        yyjson_doc_free(doc);
    }
    printf("{\"cancel\":%s,\"status\":%d,\"detail_bytes\":%llu,\"serializer_observed\":%s}\n",
           cancel ? "true" : "false", c.status, (unsigned long long)detail.len,
           observed ? "true" : "false");
    tny_error_free(c.error);
    bool destroyed = tny_toolkit_job_destroy(&c.job) == 0 && !c.job;
    if (!status_ok || !detail_ok || !destroyed) {
        fprintf(stderr, "ASSERT late cancellation must discard completed strict detail\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *file = fopen(argv[1], "rb");
    if (!file) return 2;
    char request[4096];
    size_t n = fread(request, 1, sizeof request - 1, file);
    bool invalid = ferror(file) || n == sizeof request - 1;
    fclose(file);
    if (invalid) return 2;
    request[n] = 0;
    int rc = check_case(request, n, false);
    if (!rc) rc = check_case(request, n, true);
    if (!rc && callbacks != 2) return 1;
    return rc;
}
