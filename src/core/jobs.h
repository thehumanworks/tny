/* jobs.h — durable ask/image jobs and bounded batches (docs/jobs.md,
 * docs/adr/0093).
 *
 * One shared service behind the `tny jobs` CLI, the typed job_* tools and the
 * terminal interception: the same validation, the same permission identities
 * and the same records. A job is a private directory under ~/.tny/jobs whose
 * `job.json` projects the current attempt, guarded by a live `owner.lock` that
 * a detached supervisor holds while it runs. The supervisor executes items by
 * running this same tny binary — `tny ask --events=jsonl` or `tny image … --json`
 * — so there is no second provider loop and the canonical event contract
 * (docs/adr/0090) is preserved byte for byte in the per-item log. */
#ifndef TNY_JOBS_H
#define TNY_JOBS_H

#include "core/config.h"
#include "util/util.h"

#define TNY_JOBS_SCHEMA_VERSION      1
#define TNY_JOBS_ID_LEN              32
#define TNY_JOBS_MAX_ITEMS           64
#define TNY_IMAGE_REFS_PER_ITEM      5
#define TNY_JOBS_MAX_CONCURRENCY     16
#define TNY_JOBS_DEFAULT_CONCURRENCY 2
#define TNY_JOBS_PROMPT_MAX          (64u * 1024u)
#define TNY_JOBS_REQUEST_MAX         (1024u * 1024u)
#define TNY_JOBS_PAYLOAD_MAX         (4u * 1024u * 1024u)
#define TNY_JOBS_LOG_MAX             (4u * 1024u * 1024u)
#define TNY_JOBS_LOG_READ_MAX        (256u * 1024u)
#define TNY_JOBS_ACK_TIMEOUT_MS      15000
#define TNY_JOBS_WAIT_TIMEOUT_EXIT   124

/* Stable machine-readable failure codes (docs/jobs.md). */
#define TNY_JOBS_CODE_INVALID       "JOB_INVALID_REQUEST"
#define TNY_JOBS_CODE_UNSUPPORTED   "JOB_UNSUPPORTED"
#define TNY_JOBS_CODE_NOT_FOUND     "JOB_NOT_FOUND"
#define TNY_JOBS_CODE_BUSY          "JOB_BUSY"
#define TNY_JOBS_CODE_OUTPUT_BUSY   "JOB_OUTPUT_RESERVED"
#define TNY_JOBS_CODE_OUTPUT_EXISTS "JOB_OUTPUT_EXISTS"
#define TNY_JOBS_CODE_UNCERTAIN     "JOB_SUBMISSION_UNCERTAIN"
#define TNY_JOBS_CODE_STALE         "JOB_STALE_SUCCESS"
#define TNY_JOBS_CODE_NO_RETRY      "JOB_NOTHING_TO_RETRY"
#define TNY_JOBS_CODE_PRIVACY       "JOB_REQUEST_NOT_STORED"
#define TNY_JOBS_CODE_OUTPUT_LIMIT  "JOB_OUTPUT_LIMIT"
#define TNY_JOBS_CODE_INTERRUPTED   "JOB_INTERRUPTED"
#define TNY_JOBS_CODE_TIMEOUT       "JOB_WAIT_TIMEOUT"
#define TNY_JOBS_CODE_STATE         "JOB_STATE_UNREADABLE"
#define TNY_JOBS_CODE_IO            "JOB_IO_FAILED"

typedef enum {
    TNY_JOBS_OP_SUBMIT = 0,
    TNY_JOBS_OP_STATUS,
    TNY_JOBS_OP_WAIT,
    TNY_JOBS_OP_CANCEL,
    TNY_JOBS_OP_RETRY,
    TNY_JOBS_OP_LOGS,
    TNY_JOBS_OP_LIST,
    TNY_JOBS_OP_RM,
    TNY_JOBS_OP_NONE
} tny_jobs_op;

/* "submit" -> TNY_JOBS_OP_SUBMIT, else TNY_JOBS_OP_NONE. */
tny_jobs_op tny_jobs_op_parse(const char *name);
const char *tny_jobs_op_name(tny_jobs_op op);
/* The exact permission identity of one operation: submit, cancel, retry and
 * rm are separate sensitive identities, and the read-only status/logs/list
 * identity can never authorize any of them. Static string. */
const char *tny_jobs_permission_tool(tny_jobs_op op);
/* True for operations that start or stop actual work. */
bool tny_jobs_op_is_sensitive(tny_jobs_op op);

/* The one `tny jobs …` argv grammar, shared by the CLI and by the terminal
 * interception so a typed command can never reach a different validator or a
 * weaker permission identity. argv excludes the leading "tny jobs".
 * `stdin_text` supplies the prompt or request document when the arguments do
 * not carry one (NULL when the caller has no stdin). On success returns the
 * operation and stores a malloc'd request object in *request_out; on failure
 * returns TNY_JOBS_OP_NONE with a short static reason in *error. */
tny_jobs_op tny_jobs_parse_argv(int argc, char **argv, const char *stdin_text, size_t stdin_len,
                                char **request_out, bool *json_out, const char **error);

/* Render one result object as the human lines the CLI and the intercepted
 * terminal command print. */
void tny_jobs_render_human(tny_jobs_op op, const char *json, buf_t *out);

/* Canonical permission detail for one validated request: the job id/attempt,
 * item indexes, provider, request digest, output and reference paths,
 * overwrite and concurrency — never a credential or a prompt. malloc'd;
 * NULL with *error set (malloc'd, safe) when the request is invalid. */
char *tny_jobs_detail(tny_ctx *ctx, tny_jobs_op op, yyjson_val *args, char **error);

/* Run one operation with an already permitted request. Appends exactly one
 * JSON object (kind:"job"/"job_list"/"job_logs") to `out`, and returns the
 * process exit code: 0 ok, 1 invalid/unsupported/not found, 2 the job failed
 * or the request could not be completed, 124 for a wait timeout, 130 for an
 * interrupted wait. `err` gets a
 * short safe message; no provider body, credential or child stderr. */
int tny_jobs_run(tny_ctx *ctx, tny_jobs_op op, yyjson_val *args, buf_t *out, char *err,
                 size_t errlen);
/* Tool cancellation interrupts submission and status waits without cancelling
 * an already accepted durable job. */
int tny_jobs_run_cancel(tny_ctx *ctx, tny_jobs_op op, yyjson_val *args, buf_t *out, char *err,
                        size_t errlen, bool (*cancelled)(void *), void *cancel_ud);

/* The hidden supervisor entry point: `tny jobs _worker <id>` with the bounded
 * payload on stdin, the acknowledgment pipe on stdout and the live owner lock
 * on descriptor 3. Returns the process exit code. */
int tny_jobs_worker_main(tny_ctx *ctx, const char *id, int payload_fd, int ack_fd, int owner_fd);

/* False in runtimes that cannot own a child process (wasm). Execution
 * operations refuse before any file or provider side effect; reading existing
 * records stays available and never claims process ownership. */
bool tny_jobs_execution_supported(void);

/* 32 lowercase hex, no path separators. */
bool tny_jobs_valid_id(const char *id);

/* Inherited item environment allowlist (A14). True means this name is not
 * operational and must be removed. Credentials of either kind are supplied
 * separately from their resolved private mapping, never inherited. */
bool tny_jobs_env_entry_is_foreign(const char *entry, bool image, bool chat_codex);

/* True when `manifest_path` is absent, or when that image generation manifest
 * (ADR 0088) still records exactly this committed artifact and hash. Read as
 * data: no image service call, no provider contact, no file opened but the
 * manifest itself. This is the manifest half of the carried-success check a
 * selective retry performs before spending anything (#127 integration).
 * Writes a short safe reason into `err` when it answers false. */
bool tny_jobs_manifest_describes(const char *manifest_path, const char *output_path,
                                 const char *output_sha256, char *err, size_t errlen);

/* Metadata-only, owned image selection (ADR 0098). The item may have succeeded
 * while siblings still run. This never projects state, reads image bytes or
 * initializes a provider. The manifest, when declared, is strictly validated
 * after root confinement and retained for permission/execution ownership. */
struct tny_image_manifest;
typedef struct {
    char job_id[TNY_JOBS_ID_LEN + 1];
    int item_index, projection_attempt, item_attempt, carried_from_attempt;
    char *path;
    char sha256[65];
    uint64_t bytes;
    char operation_id[33];
    struct tny_image_manifest *manifest;
} tny_job_artifact;

tny_job_artifact *tny_jobs_select_artifact(const tny_ctx *, const char *id, int item, char *err,
                                           size_t errlen);
void tny_jobs_artifact_free(tny_job_artifact *);

#endif
