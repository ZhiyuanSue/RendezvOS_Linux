#ifndef _LINUX_COMPAT_TEST_SYNC_IPC_H_
#define _LINUX_COMPAT_TEST_SYNC_IPC_H_

#include <common/types.h>
#include <linux_compat/ipc/clean_protocol.h>

/*
 * Boot wait glue (until PID1 can be waited with wait4 alone):
 * - /init is spawned with a test_cookie on its thread append
 * - sys_exit → clean_server THREAD_REAP → linux_user_test_notify_exit(cookie)
 * - linux_boot waits until that cookie is published, then may poweroff
 *
 * Uses KMSG_OP_CLEAN_THREAD_REAP / LINUX_KMSG_FMT_THREAD_REAP from
 * clean_protocol.h.
 */

#endif
