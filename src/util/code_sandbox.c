/* OS confinement for Python code-cell processes (docs/adr/0179). Host OS seam.
 *
 * The cell has already initialized its interpreter when it calls
 * tny_code_sandbox_enter(); after that it may compute, allocate, and exchange
 * frames over its inherited socket, and nothing else. Linux: a seccomp-BPF
 * allowlist (unknown syscalls fail with EPERM; a foreign architecture or the
 * x32 ABI kills the process). macOS: the system pure-computation sandbox.
 * Any other host, or any failure, fails closed: the cell refuses to run. */
#include "util/code_sandbox.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/resource.h>
#include <unistd.h>

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

#if defined(__APPLE__)
/* Declared here to keep the deprecated header out of the build; the symbols
 * are still exported by libSystem and used by current system components. */
extern const char kSBXProfilePureComputation[];
int sandbox_init(const char *profile, uint64_t flags, char **errorbuf);
void sandbox_free_error(char *errorbuf);
#define TNY_SANDBOX_NAMED 0x0001
#endif

static int limit(int resource, rlim_t value) {
    struct rlimit current;
    if (getrlimit(resource, &current)) return -1;
    struct rlimit next = {value, value};
    if (current.rlim_max != RLIM_INFINITY && current.rlim_max < value)
        next.rlim_cur = next.rlim_max = current.rlim_max;
    return setrlimit(resource, &next);
}

int tny_code_sandbox_limits(unsigned cpu_seconds) {
#if defined(__EMSCRIPTEN__)
    (void)cpu_seconds;
    errno = ENOTSUP;
    return -1;
#else
    /* No core files or regular-file growth; bounded CPU as a backstop for the
     * parent's wall-clock kill. Descriptor and process limits stop anything
     * new even before the syscall filter is installed. */
    if (limit(RLIMIT_CORE, 0) || limit(RLIMIT_FSIZE, 0) || limit(RLIMIT_CPU, cpu_seconds))
        return -1;
    if (limit(RLIMIT_NOFILE, 4)) return -1;
#if defined(RLIMIT_NPROC)
    if (limit(RLIMIT_NPROC, 0)) return -1;
#endif
    return 0;
#endif
}

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#if defined(__x86_64__)
#define TNY_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define TNY_AUDIT_ARCH AUDIT_ARCH_AARCH64
#endif

#define LOAD_NR BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr))
#define ALLOW(nr) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1), BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

#ifdef TNY_AUDIT_ARCH
/* Signals only to this process (abort/raise), identified at install time. */
#define SELF_SIGNAL(nr)                                                             \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 4),                                \
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])), \
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)self, 0, 1),                  \
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW), LOAD_NR
#endif

static int enter_linux(void) {
#ifndef TNY_AUDIT_ARCH
    errno = ENOTSUP;
    return -1;
#else
    pid_t self = getpid();
    struct sock_filter program[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, TNY_AUDIT_ARCH, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        LOAD_NR,
#if defined(__x86_64__)
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1), /* x32 ABI */
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
#endif
        /* Frames over the inherited socket and its readiness waits. */
        ALLOW(SYS_read),
        ALLOW(SYS_write),
        ALLOW(SYS_readv),
        ALLOW(SYS_writev),
        ALLOW(SYS_recvfrom),
        ALLOW(SYS_sendto),
        ALLOW(SYS_ppoll),
#ifdef SYS_poll
        ALLOW(SYS_poll),
#endif
        ALLOW(SYS_close),
        /* Memory for the interpreter heap. */
        ALLOW(SYS_brk),
        ALLOW(SYS_mmap),
        ALLOW(SYS_munmap),
        ALLOW(SYS_mremap),
        ALLOW(SYS_mprotect),
        ALLOW(SYS_madvise),
        /* Runtime housekeeping without authority. */
        ALLOW(SYS_futex),
        ALLOW(SYS_clock_gettime),
        ALLOW(SYS_getrandom),
        ALLOW(SYS_rt_sigreturn),
        ALLOW(SYS_rt_sigprocmask),
        ALLOW(SYS_sigaltstack),
        ALLOW(SYS_getpid),
        ALLOW(SYS_gettid),
        ALLOW(SYS_exit),
        ALLOW(SYS_exit_group),
        SELF_SIGNAL(SYS_tgkill),
        SELF_SIGNAL(SYS_tkill),
        /* Everything else, including open/openat, socket/connect, clone/fork,
         * execve, kill, ptrace, io_uring and bpf, fails with EPERM. */
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
    };
    struct sock_fprog filter = {.len = (unsigned short)(sizeof program / sizeof program[0]),
                                .filter = program};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return -1;
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &filter)) return -1;
    return 0;
#endif
}
#endif

int tny_code_sandbox_enter(void) {
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    return enter_linux();
#elif defined(__APPLE__)
    char *error = NULL;
    if (sandbox_init(kSBXProfilePureComputation, TNY_SANDBOX_NAMED, &error)) {
        if (error) sandbox_free_error(error);
        errno = EPERM;
        return -1;
    }
    return 0;
#else
    errno = ENOTSUP;
    return -1;
#endif
}
