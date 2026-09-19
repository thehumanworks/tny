/* Durable collaboration context, not an execution queue. See docs/team-mailbox.md.
 * SECURITY: caller is TRUSTED C input, never decoded from public tool arguments.
 * Session possession is NOT membership. authorize must verify a private child
 * capability (or lead authority) against the locked job record and permission
 * ceilings. No production allow-all callback. Same-user shells are not sandboxed. */
#ifndef TNY_TEAM_MAILBOX_H
#define TNY_TEAM_MAILBOX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define TNY_MAILBOX_PAYLOAD_MAX     16384u
#define TNY_MAILBOX_OUTSTANDING_MAX 64u
#define TNY_MAILBOX_HISTORY_MAX     256u
#define TNY_MAILBOX_BATCH_MAX       16u
#define TNY_MAILBOX_BATCH_BYTES_MAX 65536u
#define TNY_MAILBOX_LEAD            (-1)
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TNY_MAILBOX_OK = 0,
    TNY_MAILBOX_INVALID,
    TNY_MAILBOX_UNSUPPORTED,
    TNY_MAILBOX_DENIED,
    TNY_MAILBOX_STALE,
    TNY_MAILBOX_TERMINAL,
    TNY_MAILBOX_BUSY,
    TNY_MAILBOX_FULL,
    TNY_MAILBOX_HISTORY_FULL,
    TNY_MAILBOX_CONFLICT,
    TNY_MAILBOX_NOT_FOUND,
    TNY_MAILBOX_BAD_STATE,
    TNY_MAILBOX_CORRUPT,
    TNY_MAILBOX_IO,
    TNY_MAILBOX_EMPTY,
    TNY_MAILBOX_DEADLINE,
    TNY_MAILBOX_CANCELLED
} tny_mailbox_rc;
typedef enum {
    TNY_MAILBOX_QUEUED = 0,
    TNY_MAILBOX_DELIVERED,
    TNY_MAILBOX_ACKED,
    TNY_MAILBOX_RETIRED /* old-attempt tombstone, never delivery or acknowledgment */
} tny_mailbox_state;
typedef struct {
    char run[33]; /* existing job ID, lowercase 32hex */
    uint32_t job_attempt;
    int task;              /* item index, or TNY_MAILBOX_LEAD */
    uint32_t task_attempt; /* zero for lead */
} tny_mailbox_identity;
typedef struct {
    int task;
    uint32_t task_attempt;
} tny_mailbox_recipient;
/* Under state.lock: bounded, no locks/reentry/provider calls. job_json is valid
 * only during callback. peers_allowed requires authoritative explicit opt-in.
 * Returning false denies ALL access. userdata/caller are privately supplied. */
typedef bool (*tny_mailbox_authorize)(void *userdata, const tny_mailbox_identity *caller,
                                      const char *job_json, size_t len, bool *peers_allowed);
typedef struct {
    const char *job_dir; /* trusted absolute canonical directory, not request input */
    bool native_local;   /* false for SSH/embedded/wasm: reject before side effects */
    tny_mailbox_authorize authorize;
    void *userdata;
} tny_mailbox_service;
typedef struct {
    char id[65];       /* caller-chosen 1..64 ASCII letters, digits, '.', '_' or '-' */
    uint64_t sequence; /* run-global, starts at 1 */
    tny_mailbox_identity sender;
    tny_mailbox_recipient recipient;
    tny_mailbox_state state;
    size_t payload_len;
    char payload[TNY_MAILBOX_PAYLOAD_MAX + 1]; /* untrusted UTF-8 user context */
    char publication[65]; /* empty for direct sends; stable channel publication ID */
} tny_mailbox_message;
/* All calls try the lock once. Send OK follows atomic private persistence.
 * IDs are run-global across attempts. Exact duplicate returns original receipt;
 * conflicting content/identity/fences returns CONFLICT. No silent eviction. */
tny_mailbox_rc tny_team_mailbox_send(const tny_mailbox_service *service,
                                     const tny_mailbox_identity *caller,
                                     tny_mailbox_recipient recipient, const char *id,
                                     const char *payload, size_t payload_len,
                                     tny_mailbox_message *out);
/* Atomic channel snapshot: all currently active peers plus lead, excluding sender.
 * Publication IDs are 1..48 valid ID bytes. Output has 64 slots. Retry returns
 * the original recipient set, even after terminal outcomes. Quotas are per
 * receipt, and no receipt is accepted unless the entire transaction commits. */
tny_mailbox_rc tny_team_mailbox_publish(const tny_mailbox_service *service,
                                        const tny_mailbox_identity *caller, const char *id,
                                        const char *payload, size_t payload_len,
                                        tny_mailbox_message *out, size_t *count);
/* Subscribe before snapshot. No periodic durable reads. Timeout 0 is a snapshot;
 * 1..30000ms waits on kernel hints. cancel may also pump native control traffic. */
tny_mailbox_rc tny_team_mailbox_wait(const tny_mailbox_service *service,
                                     const tny_mailbox_identity *caller, int timeout_ms,
                                     bool (*cancelled)(void *), void *userdata,
                                     tny_mailbox_message *out, size_t *count);
/* Pure snapshot of caller's queued/delivered messages; retired/acked excluded.
 * after_sequence paginates, never acks; recover from zero. out has capacity
 * slots (1..BATCH_MAX), byte_limit is 1..BATCH_BYTES_MAX. FULL if first pending
 * payload cannot fit: no skipping. count is zero on failure. */
tny_mailbox_rc tny_team_mailbox_inbox(const tny_mailbox_service *service,
                                      const tny_mailbox_identity *caller, uint64_t after_sequence,
                                      tny_mailbox_message *out, size_t capacity, size_t byte_limit,
                                      size_t *count);
/* Pure read includes acked records; only addressed caller/attempt may read. */
tny_mailbox_rc tny_team_mailbox_read(const tny_mailbox_service *service,
                                     const tny_mailbox_identity *caller, const char *id,
                                     tny_mailbox_message *out);
/* Persist transcript first, then mark. Replays remain until explicit ack.
 * Consumers must dedup persisted transcript by (run,id), never by payload. */
tny_mailbox_rc tny_team_mailbox_mark_delivered(const tny_mailbox_service *service,
                                               const tny_mailbox_identity *caller, const char *id);
/* Requires delivered (BAD_STATE otherwise). Repeated ack is idempotent. */
tny_mailbox_rc tny_team_mailbox_ack(const tny_mailbox_service *service,
                                    const tny_mailbox_identity *caller, const char *id);
/* Explicit old-attempt abandonment, only authenticated TNY_MAILBOX_LEAD (-1).
 * Indexed role metadata (including role:"lead") grants no routing authority.
 * recipient_task must be a member (-1 is the lead). before_job_attempt must be
 * 1..current job attempt; ONLY queued/delivered records strictly older than it
 * retire. Current-attempt and acked records cannot change. Tombstones retain
 * IDs/content/fences/sequence; retirement frees outstanding quota, not history.
 * *retired is the count changed by THIS call, zero on error or repeated request.
 * On I/O error publication may have occurred: reconcile/retry, not rollback. */
tny_mailbox_rc tny_team_mailbox_retire(const tny_mailbox_service *service,
                                       const tny_mailbox_identity *trusted_caller,
                                       int recipient_task, uint32_t before_job_attempt,
                                       size_t *retired);
const char *tny_team_mailbox_error(tny_mailbox_rc rc);
#ifdef __cplusplus
}
#endif
#endif
