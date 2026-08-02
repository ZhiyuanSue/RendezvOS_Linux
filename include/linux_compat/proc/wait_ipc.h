#ifndef _LINUX_COMPAT_PROC_WAIT_IPC_H_
#define _LINUX_COMPAT_PROC_WAIT_IPC_H_

#include <linux_compat/proc_compat.h>
#include <rendezvos/task/tcb.h>

/*
 * wait4 IPC wake helpers (linux_layer/proc/proc_wait_ipc.c).
 *
 * Protocol: doc/linux_compat/protocols/EXIT_CLEAN.md
 *   - Child exit → KMSG_OP_PROC_EXIT_NOTIFY (authoritative wait wake / reap).
 *   - WAIT_INTERRUPT:
 *       (1) EINTR for non-SIGCHLD signals;
 *       (2) poke to drain pending_exits when EXIT_NOTIFY park alloc/hard-fails.
 *     SIGCHLD alone must not post interrupt.
 */

void linux_proc_wait_wake_for_signal(Thread_Base *thread, Tcb_Base *process);

/*
 * Unconditionally post WAIT_INTERRUPT so a blocked wait4 re-checks
 * pending_exits (try_pending). Not an EINTR by itself.
 */
bool linux_proc_wait_poke(pid_t parent_pid);

/*
 * Blocking EXIT_NOTIFY to parent's wait_port (enqueue + send_msg).
 * Prefer linux_proc_try_post_exit_notify from clean listen (coop).
 */
bool linux_proc_post_exit_notify(pid_t parent_pid, pid_t child_pid,
                                 i32 exit_code);

/*
 * Non-blocking EXIT_NOTIFY via ipc_system_try_deliver (no listen send_queue).
 * DELIVERED: rendezvous done (or port closed — no waiter).
 * AGAIN: parent not in recv yet — caller must park and retry from poll.
 * FAIL: hard error (bad pid / create failed).
 */
typedef enum {
        LINUX_PROC_TRY_DELIVERED = 0,
        LINUX_PROC_TRY_AGAIN = 1,
        LINUX_PROC_TRY_FAIL = 2,
} linux_proc_try_result_t;

linux_proc_try_result_t linux_proc_try_post_exit_notify(pid_t parent_pid,
                                                        pid_t child_pid,
                                                        i32 exit_code);

/*
 * Blocking EXIT_NOTIFY to kernel_port for reparented / parent-dead zombies.
 * Handled by linux_init_kernel_ipc_handler (boot_thread / kernel_port recv).
 */
bool linux_proc_post_kernel_exit_notify(pid_t child_pid, i32 exit_code);

/*
 * Mark exit_state REAPED and TASK_REAP_SYNC (init orphan path).
 * Must not run inside kernel_port EXIT_NOTIFY handler — see
 * linux_proc_schedule_init_reap().
 */
bool linux_proc_reap_zombie_by_pid(pid_t child_pid);

/*
 * Queue @p child_pid for init reap on a dedicated thread. The kernel_port
 * EXIT_NOTIFY handler must return quickly so a blocked send_msg(kernel_port)
 * from clean can complete (protocols/EXIT_CLEAN.md).
 */
void linux_proc_schedule_init_reap(pid_t child_pid);

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
