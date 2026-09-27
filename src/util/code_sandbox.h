/* OS confinement for Python code-cell processes (docs/adr/0179). */
#ifndef TNY_CODE_SANDBOX_H
#define TNY_CODE_SANDBOX_H

/* Resource limits for a fresh cell process: no core dumps, no regular-file
 * growth, no new descriptors or processes, bounded CPU. 0 on success. */
int tny_code_sandbox_limits(unsigned cpu_seconds);
/* Irreversibly confine this process to computation plus I/O on descriptors it
 * already holds. 0 on success; -1 with errno (ENOTSUP on unsupported hosts). */
int tny_code_sandbox_enter(void);

#endif
