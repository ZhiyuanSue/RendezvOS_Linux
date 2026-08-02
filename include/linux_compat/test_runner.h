#ifndef _RENDEZVOS_LINUX_COMPAT_TEST_RUNNER_H_
#define _RENDEZVOS_LINUX_COMPAT_TEST_RUNNER_H_

#include <common/types.h>

/*
 * Notify linux_boot that the cookie'd user thread (/init) has exited.
 * Called from clean_server on THREAD_REAP before the thread is freed.
 *
 * owner_cpu is unused (single boot wait slot); kept for call-site stability.
 */
void linux_user_test_notify_exit(i32 owner_cpu, u64 cookie, i64 exit_code);

#endif
