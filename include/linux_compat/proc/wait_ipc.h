#ifndef _LINUX_COMPAT_PROC_WAIT_IPC_H_
#define _LINUX_COMPAT_PROC_WAIT_IPC_H_

#include <linux_compat/proc_compat.h>
#include <rendezvos/task/thread.h>

/*
 * wait4 IPC wake helpers (linux_layer/proc/proc_wait_ipc.c).
 * Protocol: doc/linux_compat/protocols/EXIT_CLEAN.md
 */

void linux_proc_wait_wake_for_signal(Thread_Base *thread, linux_proc_resource_t *proc);

bool linux_proc_wait_poke(pid_t parent_pid);

bool linux_proc_wait_pid_matches(i32 want_pid, pid_t child_pid,
                                 linux_proc_resource_t *parent);

typedef enum {
        LINUX_PROC_TRY_DELIVERED = 0,
        LINUX_PROC_TRY_AGAIN = 1,
        LINUX_PROC_TRY_FAIL = 2,
} linux_proc_try_result_t;

linux_proc_try_result_t linux_proc_try_post_exit_notify(pid_t parent_pid,
                                                        linux_proc_resource_t *child,
                                                        i32 exit_code);

bool linux_proc_wait_pending_push(linux_proc_resource_t *parent,
                                  linux_proc_resource_t *child, i32 exit_code);
bool linux_proc_wait_pending_take(linux_proc_resource_t *parent, i32 want_pid,
                                  linux_proc_resource_t **child_out,
                                  i32 *exit_code_out);
void linux_proc_wait_pending_drain(linux_proc_resource_t *parent);

#endif
