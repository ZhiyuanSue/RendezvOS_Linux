#ifndef _LINUX_COMPAT_PROC_CLEAN_IPC_H_
#define _LINUX_COMPAT_PROC_CLEAN_IPC_H_

#include <common/types.h>
#include <rendezvos/error.h>
#include <rendezvos/task/tcb.h>

/*
 * Thin one-way IPC to clean_server (core delete_thread / delete_task only).
 *
 * THREAD_REAP: sys_exit / fatal fault — physical thread detach.
 * TASK_REAP:   wait4 (reaped) or orphan exit — physical task delete.
 */

error_t linux_clean_send_thread_reap(Thread_Base* thread, i64 exit_code);
error_t linux_clean_send_task_reap(pid_t pid);

#endif
