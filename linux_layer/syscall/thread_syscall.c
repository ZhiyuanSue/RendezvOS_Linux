#include <modules/log/log.h>
#include <common/types.h>
#include <rendezvos/error.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/signal/signal_queue.h>
#include <linux_compat/signal/signal_state.h>
#include <linux_compat/signal/signal_types.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/message.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <linux_compat/ipc/exit_protocol.h>
#include <linux_compat/proc/clean_ipc.h>
#include <linux_compat/proc/wait_ipc.h>
#include <linux_compat/fs/linux_fd_table.h>
#include <linux_compat/time/linux_time_sleep.h>
#include <linux_compat/fault.h>

void sys_exit(i64 exit_code)
{
        Thread_Base* self = get_cpu_current_thread();
        Tcb_Base* task = get_cpu_current_task();

        if (!self)
                goto out;

        if (task) {
                linux_fs_proc_release_for_exit(task);
        }

        linux_time_sleep_port_teardown(self);

        if (task && task->vs) {
                linux_thread_append_t* ta = linux_thread_append(self);

                if (ta && ta->clear_tid
                    && linux_vspace_is_user_table(task->vs)) {
                        i32 zero = 0;

                        /*
                         * Best-effort CLEARTID (musl set_tid_address). Failure
                         * is common on partial maps; must not be mistaken for
                         * the hang point — THREAD_REAP / wait follows this.
                         */
                        (void)linux_mm_store_to_user(
                                task->vs, ta->clear_tid, &zero, sizeof(zero));
                        ta->clear_tid = 0;
                }
        }

        /*
         * Protocol: doc/linux_compat/protocols/EXIT_CLEAN.md
         * Default ZOMBIE so wait4 can collect; orphans upgraded to REAPED
         * below.
         */
        if (task) {
                linux_proc_append_t* pa = linux_proc_append(task);
                if (pa) {
                        /* Linux exit status is 8-bit (see wait4 WEXITSTATUS).
                         */
                        pa->exit_code = (i32)(exit_code & 0xff);
                        pa->exit_state = LINUX_EXIT_ZOMBIE;
                }
        }
        bool reaper_exists = false;
        if (task && task->pid > 0) {
                linux_proc_append_t* pa = linux_proc_append(task);

                reaper_exists = proc_has_wait_reaper(pa);
                if (reaper_exists && pa && pa->ppid > 0) {
                        Tcb_Base* parent_task = find_task_by_pid(pa->ppid);

                        if (parent_task) {
                                linux_signal_proc_state_t* parent_ps =
                                        linux_signal_proc_state(parent_task);
                                sigaction_t* chld_disp;

                                if (parent_ps) {
                                        chld_disp =
                                                &parent_ps->dispositions[SIGCHLD
                                                                         - 1];
                                        if (!(chld_disp->sa_flags
                                              & SA_NOCLDWAIT)) {
                                                (void)linux_queue_signal(
                                                        parent_task,
                                                        SIGCHLD,
                                                        task->pid);
                                        }
                                }
                        }
                }
        }

        /* Link B: REAPED; listen THREAD_REAP finishes delete_task when last. */
        if (task && !reaper_exists) {
                linux_proc_append_t* pa = linux_proc_append(task);
                if (pa) {
                        pa->exit_state = LINUX_EXIT_REAPED;
                }
        }

        thread_or_flags(self, THREAD_FLAG_EXIT_REQUESTED);

        /*
         * If we were parked on IPC, get to a known state before send. Do NOT
         * mark zombie yet — THREAD_REAP send must finish first or clean_server
         * can delete_thread while we still sit in send_msg.
         */
        {
                u64 st = thread_get_status(self);

                if (st == thread_status_block_on_receive
                    || st == thread_status_block_on_send) {
                        thread_set_status(self, thread_status_running);
                }
        }

        (void)linux_clean_send_thread_reap(self, exit_code);

        /*
         * Link B: do not send a separate TASK_REAP from the exiting thread.
         * That raced THREAD_REAP (listen can run TASK_REAP before
         * delete_thread). Protocol: THREAD_REAP listen finishes
         * claim+delete_task when REAPED.
         */

        /*
         * Unconditional zombie: after send_msg the status is often "ready"
         * not "running", so "if running → zombie" skipped and clean_server
         * spun forever on EXIT_REQUESTED. Then a tight for(;;) starved the
         * same-CPU worker.
         */
        (void)thread_set_status(self, thread_status_zombie);

out:
        /*
         * Keep yielding until delete_thread reaps us. A bare for(;;) after
         * schedule returns burns the CPU and can block same-CPU clean workers.
         */
        for (;;)
                schedule(percpu(core_tm));
}

void linux_fatal_user_fault(i64 exit_code)
{
        Thread_Base* self = get_cpu_current_thread();
        Tcb_Base* task = get_cpu_current_task();
        bool reaper_exists = false;

        if (task) {
                linux_proc_append_t* pa = linux_proc_append(task);
                if (pa) {
                        pa->exit_code = (i32)(exit_code & 0xff);
                        pa->exit_state = LINUX_EXIT_ZOMBIE;
                        reaper_exists = proc_has_wait_reaper(pa);
                }
        }
        if (task && !reaper_exists) {
                linux_proc_append_t* pa = linux_proc_append(task);
                if (pa) {
                        pa->exit_state = LINUX_EXIT_REAPED;
                }
        }
        if (self) {
                thread_or_flags(self, THREAD_FLAG_EXIT_REQUESTED);
        }

        (void)linux_clean_send_thread_reap(self, exit_code);
        /* Link B: task delete is chained from THREAD_REAP when REAPED. */

        if (self)
                (void)thread_set_status(self, thread_status_zombie);

        for (;;)
                schedule(percpu(core_tm));
}

void sys_exit_group(i64 exit_code)
{
        Tcb_Base* task = get_cpu_current_task();
        if (!task) {
                pr_error("[PROC] exit_group: No current task\n");
                return;
        }

        /*
         * Kill all threads in the task except the current one.
         * We iterate through the task's thread list directly.
         */
        struct list_entry* pos;
        struct list_entry* next;

        lock_cas(&task->thread_list_lock);

        /*
         * Save next pointer before setting flags, as thread might
         * be removed from list by other CPU.
         */
        list_for_each_safe(pos, next, &task->thread_head_node)
        {
                Thread_Base* thread =
                        container_of(pos, Thread_Base, thread_list_node);

                /* Skip current thread - we kill it last */
                if (thread == get_cpu_current_thread()) {
                        continue;
                }

                /* Set exit flag for this thread */
                thread_or_flags(thread, THREAD_FLAG_EXIT_REQUESTED);
        }

        unlock_cas(&task->thread_list_lock);

        /* Finally kill current thread */
        sys_exit(exit_code);
}
