#ifndef _LINUX_COMPAT_PROC_CLEAN_IPC_H_
#define _LINUX_COMPAT_PROC_CLEAN_IPC_H_

#include <common/types.h>
#include <rendezvos/error.h>
#include <rendezvos/task/tcb.h>

/*
 * clean_server client API — see doc/linux_compat/protocols/EXIT_CLEAN.md
 *
 * THREAD_REAP:      one-way delete_thread (sys_exit / fatal); link B may
 *                   finish delete_task in the same worker when REAPED.
 * TASK_REAP:        legacy one-way delete_task (not used by sys_exit).
 * TASK_REAP_SYNC:   RPC delete_task for wait4 / kernel init reaper.
 */

error_t linux_clean_send_thread_reap(Thread_Base* thread, i64 exit_code);
error_t linux_clean_send_task_reap(pid_t pid);
i64 linux_clean_task_reap_sync(pid_t caller_pid, pid_t target_pid);

#endif
