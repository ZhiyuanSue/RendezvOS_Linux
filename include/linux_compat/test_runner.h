#ifndef _RENDEZVOS_LINUX_COMPAT_TEST_RUNNER_H_
#define _RENDEZVOS_LINUX_COMPAT_TEST_RUNNER_H_

#include <common/types.h>

/*
 * Notify the linux compat user test runner that a test-managed user thread
 * has exited. Intended to be called from the clean server before freeing the
 * thread.
 *
 * Parameters:
 * - owner_cpu: the CPU that owns the exiting thread's Task_Manager
 * - cookie:    per-thread cookie set by the runner (non-zero)
 * - exit_code: syscall-provided exit code
 */
void linux_user_test_notify_exit(i32 owner_cpu, u64 cookie, i64 exit_code);

#endif

