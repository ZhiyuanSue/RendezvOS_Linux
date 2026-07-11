#ifndef _LINUX_COMPAT_PROC_WAIT_IPC_H_
#define _LINUX_COMPAT_PROC_WAIT_IPC_H_

#include <rendezvos/task/tcb.h>

/*
 * wait4 IPC wake helpers (linux_layer/proc/proc_wait_ipc.c).
 * Child exit uses KMSG_OP_PROC_EXIT_NOTIFY; signal EINTR uses WAIT_INTERRUPT.
 */

void linux_proc_wait_wake_for_signal(Thread_Base *thread, Tcb_Base *process);

/*
 * Blocking EXIT_NOTIFY to parent's wait_port (enqueue + send_msg).
 * Sent by clean_server after THREAD_REAP when thread_number==0.
 */
bool linux_proc_post_exit_notify(pid_t parent_pid, pid_t child_pid,
                                 i32 exit_code);

/*
 * If clean_server is blocked on send_msg while the parent reaped without
 * recv_msg, complete the rendezvous and discard the notify message.
 */
void linux_proc_wait_accept_pending_notify(pid_t parent_pid);

/*
 * Blocking EXIT_NOTIFY to kernel_port for reparented / parent-dead zombies.
 * Handled by linux_init_kernel_ipc_handler (init thread recv loop).
 */
bool linux_proc_post_kernel_exit_notify(pid_t child_pid, i32 exit_code);

/*
 * Mark exit_state reaped and queue TASK_REAP (init orphan path).
 */
bool linux_proc_reap_zombie_by_pid(pid_t child_pid);

#endif
