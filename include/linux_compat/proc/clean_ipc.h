#ifndef _LINUX_COMPAT_PROC_CLEAN_IPC_H_
#define _LINUX_COMPAT_PROC_CLEAN_IPC_H_

#include <common/types.h>
#include <rendezvos/error.h>
#include <rendezvos/task/thread.h>

/*
 * clean_server client API — doc/linux_compat/protocols/EXIT_CLEAN.md (v2)
 *
 * THREAD_REAP: sys_exit / fatal → clean listen → delete_thread + notify/reap.
 */

error_t linux_clean_send_thread_reap(Thread_Base* thread, i64 exit_code);

#endif
