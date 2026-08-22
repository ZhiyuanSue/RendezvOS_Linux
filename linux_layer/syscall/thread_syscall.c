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
#include <rendezvos/task/thread.h>
#include <linux_compat/ipc/exit_protocol.h>
#include <linux_compat/proc/clean_ipc.h>
#include <linux_compat/proc/wait_ipc.h>
#include <linux_compat/fs/linux_fd_table.h>
#include <linux_compat/time/linux_time_sleep.h>
#include <linux_compat/fault.h>

/*
 * Mark resource bundle for exit (EXIT_CLEAN.md). Returns true for Link A
 * (live parent wait reaper), false for Link B (clean inline reap).
 */
static bool linux_proc_mark_exiting(linux_proc_resource_t *proc, i64 exit_code)
{
        proc->exit_code = (i32)(exit_code & 0xff);
        proc->exit_state = LINUX_EXIT_ZOMBIE;
        lock_cas(&proc->thread_list_lock);
        proc->exit_last_thread = (proc->thread_number == 1);
        unlock_cas(&proc->thread_list_lock);
        return proc_has_wait_reaper(proc);
}

static void linux_proc_queue_sigchld(linux_proc_resource_t *proc)
{
        linux_proc_resource_t *parent;

        if (!proc || proc->ppid <= 0)
                return;
        parent = find_proc_by_pid(proc->ppid);
        if (!parent)
                return;

        linux_signal_proc_state_t *parent_ps = linux_signal_proc_state(parent);
        sigaction_t *chld_disp;

        if (!parent_ps)
                return;
        chld_disp = &parent_ps->dispositions[SIGCHLD - 1];
        if (chld_disp->sa_flags & SA_NOCLDWAIT)
                return;
        (void)linux_queue_signal(parent, SIGCHLD, proc->pid);
}

static void linux_thread_exit_rendezvous(Thread_Base *self, i64 exit_code)
{
        u64 st;

        thread_or_flags(self, THREAD_FLAG_EXIT_REQUESTED);

        st = thread_get_status(self);
        if (st == thread_status_block_on_receive
            || st == thread_status_block_on_send)
                thread_set_status(self, thread_status_running);

        (void)linux_clean_send_thread_reap(self, exit_code);
        (void)thread_set_status(self, thread_status_zombie);
}

void sys_exit(i64 exit_code)
{
        Thread_Base *self = get_cpu_current_thread();
        linux_proc_resource_t *proc = linux_current_proc();

        if (!self)
                goto out;

        if (proc)
                linux_fs_proc_release_for_exit(proc);

        linux_time_sleep_port_teardown(self);

        if (proc && linux_current_vs()) {
                linux_thread_append_t *ta = linux_thread_append(self);

                if (ta && ta->clear_tid
                    && linux_vspace_is_user_table(linux_current_vs())) {
                        i32 zero = 0;

                        (void)linux_mm_store_to_user(
                                linux_current_vs(), ta->clear_tid, &zero, sizeof(zero));
                        ta->clear_tid = 0;
                }
        }

        if (proc) {
                if (linux_proc_mark_exiting(proc, exit_code))
                        linux_proc_queue_sigchld(proc);
        }

        linux_thread_exit_rendezvous(self, exit_code);

out:
        if (!self) {
                for (;;)
                        schedule(percpu(core_tm));
        }
        for (;;)
                schedule(percpu(core_tm));
}

void linux_fatal_user_fault(i64 exit_code)
{
        Thread_Base *self = get_cpu_current_thread();
        linux_proc_resource_t *proc = linux_current_proc();

        if (proc)
                (void)linux_proc_mark_exiting(proc, exit_code);

        if (self)
                linux_thread_exit_rendezvous(self, exit_code);
        else {
                for (;;)
                        schedule(percpu(core_tm));
        }

        for (;;)
                schedule(percpu(core_tm));
}

void sys_exit_group(i64 exit_code)
{
        linux_proc_resource_t *proc = linux_current_proc();
        struct list_entry *pos;
        struct list_entry *next;

        if (!proc) {
                pr_error("[PROC] exit_group: No current task\n");
                return;
        }

        lock_cas(&proc->thread_list_lock);
        list_for_each_safe(pos, next, &proc->thread_head_node)
        {
                linux_thread_append_t *ta = container_of(
                        pos, linux_thread_append_t, res_thread_node);
                Thread_Base *thread = linux_thread_from_append(ta);

                if (thread != get_cpu_current_thread())
                        thread_or_flags(thread, THREAD_FLAG_EXIT_REQUESTED);
        }
        unlock_cas(&proc->thread_list_lock);

        sys_exit(exit_code);
}
