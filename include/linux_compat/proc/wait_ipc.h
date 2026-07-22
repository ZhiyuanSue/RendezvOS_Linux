#ifndef _LINUX_COMPAT_PROC_WAIT_IPC_H_
#define _LINUX_COMPAT_PROC_WAIT_IPC_H_

#include <linux_compat/proc_compat.h>
#include <rendezvos/task/tcb.h>

/*
 * wait4 IPC wake helpers (linux_layer/proc/proc_wait_ipc.c).
 * Child exit uses KMSG_OP_PROC_EXIT_NOTIFY; signal EINTR uses WAIT_INTERRUPT.
 */

void linux_proc_wait_wake_for_signal(Thread_Base *thread, Tcb_Base *process);

/*
 * Blocking EXIT_NOTIFY to parent's wait_port (enqueue + send_msg).
 * Sent by a clean_server per-message worker after THREAD_REAP when
 * thread_number==0.
 */
bool linux_proc_post_exit_notify(pid_t parent_pid, pid_t child_pid,
                                 i32 exit_code);

/*
 * Blocking EXIT_NOTIFY to kernel_port for reparented / parent-dead zombies.
 * Handled by linux_init_kernel_ipc_handler (init thread recv loop).
 */
bool linux_proc_post_kernel_exit_notify(pid_t child_pid, i32 exit_code);

/*
 * Mark exit_state reaped and queue TASK_REAP (init orphan path).
 */
bool linux_proc_reap_zombie_by_pid(pid_t child_pid);

/*
 * EXIT_NOTIFY that does not match the current wait(pid) filter is queued
 * until a later wait consumes it (avoids losing the only notify).
 */
bool linux_proc_wait_pending_push(linux_proc_append_t *parent_pa, pid_t pid,
                                  i32 exit_code);
bool linux_proc_wait_pending_take(linux_proc_append_t *parent_pa, i32 want_pid,
                                  Tcb_Base *parent, pid_t *pid_out,
                                  i32 *exit_code_out);
void linux_proc_wait_pending_drain(linux_proc_append_t *parent_pa);

#endif
