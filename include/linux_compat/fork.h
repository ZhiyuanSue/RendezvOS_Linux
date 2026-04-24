#ifndef _RENDEZVOS_LINUX_COMPAT_FORK_H_
#define _RENDEZVOS_LINUX_COMPAT_FORK_H_

#include <common/types.h>
#include <rendezvos/trap/trap.h>

/*
 * Fork system call - create child process.
 *
 * Returns:
 * - >0: Child PID (in parent process)
 * - 0: Child process (should return in child after context setup)
 * - <0: Error code
 *
 * Note: Uses the provided trap_frame to copy parent's execution state.
 *       The child process will resume from the syscall return point.
 */
i64 sys_fork();

#endif
