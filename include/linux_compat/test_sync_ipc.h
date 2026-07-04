#ifndef _LINUX_COMPAT_TEST_SYNC_IPC_H_
#define _LINUX_COMPAT_TEST_SYNC_IPC_H_

#include <common/types.h>
#include <linux_compat/ipc/clean_protocol.h>

/*
 * Linux-compat test synchronization:
 * - sys_exit() asks clean_server to reap the thread
 * - clean_server calls linux_user_test_notify_exit with the test cookie
 * - user_test_runner waits for linux_test_done_cookie[cpu] to match
 *
 * Uses KMSG_OP_CLEAN_THREAD_REAP / LINUX_KMSG_FMT_THREAD_REAP from
 * clean_protocol.h.
 */

#endif
