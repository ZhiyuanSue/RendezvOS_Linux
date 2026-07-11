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
#include <linux_compat/time/linux_time_sleep.h>
#include <linux_compat/test_sync_ipc.h>
#include <linux_compat/fault.h>
#include <linux_compat/test_runner.h>
void sys_exit(i64 exit_code)
{
        Thread_Base* self = get_cpu_current_thread();
        Tcb_Base* task = get_cpu_current_task();

        if (!self)
                goto out;

        linux_time_sleep_port_teardown(self);

        if (task && task->vs) {
                linux_thread_append_t* ta = linux_thread_append(self);

                if (ta && ta->clear_tid
                    && linux_vspace_is_user_table(task->vs)) {
                        i32 zero = 0;

                        if (linux_mm_store_to_user(task->vs,
                                                   ta->clear_tid,
                                                   &zero,
                                                   sizeof(zero))
                            != REND_SUCCESS) {
                                pr_warn("[PROC] sys_exit: clear_tid write failed\n");
                        }
                }
        }

        /*
         * Set task exit state for wait4().
         * exit_state: 0=running, 1=zombie, 2=reaped
         *
         * IMPORTANT: Set exit_state=1 (zombie) so wait4 can find this child.
         * wait4 will mark it as exit_state=2 (reaped) after retrieving status.
         * This follows Linux semantics where child becomes zombie on exit
         * and stays zombie until parent calls wait().
         */
        if (task) {
                linux_proc_append_t* pa = linux_proc_append(task);
                if (pa) {
                        pa->exit_code = (i32)exit_code;
                        pa->exit_state = 1; /* 1 = zombie (wait4 can find us) */
                }
        }

        /*
         * Approach 2: child sends THREAD_REAP only; clean_server posts
         * EXIT_NOTIFY to the parent after thread_number reaches zero.
         */
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
                                                &parent_ps->dispositions
                                                         [SIGCHLD - 1];
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

        /*
         * No wait reaper: mark reaped now; TASK_REAP after THREAD_REAP removes
         * the last thread. Live parent / kernel init keep exit_state==1.
         */
        if (task && !reaper_exists) {
                linux_proc_append_t* pa = linux_proc_append(task);
                if (pa) {
                        pa->exit_state = 2;
                }
        }

        thread_or_flags(self, THREAD_FLAG_EXIT_REQUESTED);

        /*
         * schedule() only transitions running -> zombie on exit. After wait4
         * recv_msg the parent may still be block_on_receive; force running so
         * the next schedule can reap this thread in clean_server.
         */
        if (self) {
                u64 st = thread_get_status(self);

                if (st == thread_status_block_on_receive
                    || st == thread_status_block_on_send) {
                        thread_set_status(self, thread_status_running);
                }
        }

        (void)linux_clean_send_thread_reap(self, exit_code);

        if (task && !reaper_exists && task->pid > 0) {
                (void)linux_clean_send_task_reap(task->pid);
        }

out:
        schedule(percpu(core_tm));
        while (1) {
                schedule(percpu(core_tm));
        }
}

void linux_fatal_user_fault(i64 exit_code)
{
        Thread_Base* self = get_cpu_current_thread();
        Tcb_Base* task = get_cpu_current_task();
        bool reaper_exists = false;

        if (task) {
                linux_proc_append_t* pa = linux_proc_append(task);
                if (pa) {
                        pa->exit_code = (i32)exit_code;
                        pa->exit_state = 1;
                        reaper_exists = proc_has_wait_reaper(pa);
                }
        }
        if (task && !reaper_exists) {
                linux_proc_append_t* pa = linux_proc_append(task);
                if (pa) {
                        pa->exit_state = 2;
                }
        }
        if (self) {
                thread_or_flags(self, THREAD_FLAG_EXIT_REQUESTED);
        }

        (void)linux_clean_send_thread_reap(self, exit_code);
        if (task && !reaper_exists && task->pid > 0) {
                (void)linux_clean_send_task_reap(task->pid);
        }

        schedule(percpu(core_tm));

        if (self) {
                (void)thread_set_status(self, thread_status_suspend);
        }
        for (;;)
                ;
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
