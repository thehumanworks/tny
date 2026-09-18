/* Shared launch admission, not execution authority. See docs/admission.md. */
#ifndef TNY_ADMISSION_H
#define TNY_ADMISSION_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define TNY_ADMISSION_HISTORY_MAX 1024U
#define TNY_ADMISSION_QUEUE_MAX   128U

typedef struct {
    const char *root;           /* trusted private state root shared by enrolled supervisors */
    const char *label;          /* public user label: 1..63 ASCII alnum, '-' or '_' */
    const char *provider_scope; /* resolved public provider/account alias, same grammar */
    uint32_t cap;
    uint32_t queue_cap;
    uint64_t claim_limit; /* lifetime launch claims, NOT provider requests or tokens */
} tny_admission_scope;
typedef struct {
    const char *run;  /* existing job ID; same public grammar */
    uint32_t task;    /* existing item index */
    uint32_t attempt; /* existing attempt, nonzero */
} tny_admission_attempt;
// NOLINTNEXTLINE(performance-enum-size)
typedef enum {
    TNY_ADMISSION_INIT, /* explicit provisioning, never implicit in claim */
    TNY_ADMISSION_CLAIM,
    TNY_ADMISSION_INSPECT,
    TNY_ADMISSION_CANCEL,
    TNY_ADMISSION_HOLD,
    TNY_ADMISSION_RELEASE
} tny_admission_op;
// NOLINTNEXTLINE(performance-enum-size)
typedef enum {
    TNY_ADMISSION_READY,
    TNY_ADMISSION_GRANTED, /* ONLY fresh committed grant; may launch once */
    TNY_ADMISSION_OWNED,   /* replay: never authorizes another launch */
    TNY_ADMISSION_QUEUED_CAPACITY,
    TNY_ADMISSION_QUEUED_FIFO,
    TNY_ADMISSION_QUEUE_FULL,
    TNY_ADMISSION_EXHAUSTED,
    TNY_ADMISSION_HISTORY_FULL,
    TNY_ADMISSION_CANCELED,
    TNY_ADMISSION_CLEANUP_HOLD,
    TNY_ADMISSION_RELEASED,
    TNY_ADMISSION_NOT_FOUND,
    TNY_ADMISSION_BUSY
} tny_admission_reason;
typedef struct {
    tny_admission_reason reason;
    uint64_t ticket; /* stable append ordinal; 0 when not enrolled */
    uint64_t claims;
    uint32_t active;
    uint32_t queued;
} tny_admission_result;
/* Nonblocking transaction: 0 plus structured result, or errno (output invalid).
 * Config mismatch EINVAL; corrupt state EIO. No job state lock may be held.
 * No waits/callbacks/provider/Git operations. CLAIM rejects trusted enrolled
 * ancestry with EDEADLK. RELEASE requires jobs-owner proof of full process
 * cleanup (including descendants), or proof no launch occurred. Not PID absence.
 * Existing jobs execution ownership must span claim/launch; replay cannot launch.
 * On write error do not launch: persisted outcome can be uncertain. */
int tny_admission_apply(const tny_admission_scope *scope, const tny_admission_attempt *attempt,
                        tny_admission_op op, bool nested_enrolled, bool cleanup_proven,
                        tny_admission_result *out);
const char *tny_admission_reason_name(tny_admission_reason reason);
#ifdef __cplusplus
}
#endif
#endif
