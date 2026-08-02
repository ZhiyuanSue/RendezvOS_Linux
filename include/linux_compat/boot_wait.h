#ifndef _LINUX_COMPAT_BOOT_WAIT_H_
#define _LINUX_COMPAT_BOOT_WAIT_H_

#include <common/types.h>
#include <linux_compat/ipc/clean_protocol.h>

/*
 * Kernel boot wait for Path-B PID1 (/init), until wait4-on-init is enough:
 * - /init is spawned with boot_wait_cookie on its thread append
 * - THREAD_REAP → linux_boot_notify_exit(cookie)
 * - linux_boot thread spins until the cookie is published, then may poweroff
 *
 * Uses KMSG_OP_CLEAN_THREAD_REAP / LINUX_KMSG_FMT_THREAD_REAP from
 * clean_protocol.h. Not a userspace test harness.
 */

/*
 * Notify linux_boot that the cookie'd user thread (/init) has exited.
 * Called from clean_server on THREAD_REAP before the thread is freed.
 *
 * owner_cpu is unused (single boot wait slot); kept for call-site stability.
 */
void linux_boot_notify_exit(i32 owner_cpu, u64 cookie, i64 exit_code);

#endif /* _LINUX_COMPAT_BOOT_WAIT_H_ */
