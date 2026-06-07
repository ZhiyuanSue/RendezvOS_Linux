#include <common/dsa/list.h>
#include <common/stddef.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/signal/signal_types.h>
#include <modules/log/log.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/sync/cas_lock.h>
#include <rendezvos/task/tcb.h>

/*
 * Phase 2B: Signal Queue Implementation (Layer A)
 *
 * Queues pending signals on a target thread. Disposition (SIG_DFL / handler)
 * is applied in linux_deliver_pending_signals() at syscall return.
 */

static inline bool signal_is_uncatchable(int sig)
{
        return sig == SIGKILL || sig == SIGSTOP;
}

static Thread_Base* signal_select_thread_helper(Tcb_Base* process, int sig)
{
        Thread_Base* current_thread;
        Thread_Base* pick = NULL;
        Thread_Base* th;
        Thread_Base* tmp;

        (void)sig;

        if (!process) {
                return NULL;
        }

        current_thread = get_cpu_current_thread();
        if (current_thread && current_thread->belong_tcb == process) {
                return current_thread;
        }

        lock_cas(&process->thread_list_lock);
        /* Manually expanded list_for_each_entry_safe to avoid typeof issues */
        for (th = container_of((process->thread_head_node.next), Thread_Base, thread_list_node), \
             tmp = container_of((th->thread_list_node.next), Thread_Base, thread_list_node); \
             &th->thread_list_node != &process->thread_head_node; \
             th = tmp, tmp = container_of((tmp->thread_list_node.next), Thread_Base, thread_list_node)) {
                pick = th;
                break;
        }
        unlock_cas(&process->thread_list_lock);

        return pick;
}

void linux_signal_flush_pending(Tcb_Base* target, int sig)
{
        linux_proc_append_t* proc_append;
        Thread_Base* th;
        Thread_Base* tmp;

        if (!target || sig < 1 || sig > NSIG) {
                return;
        }

        proc_append = linux_proc_append(target);
        if (!proc_append) {
                return;
        }

        sigdelset(&proc_append->pending_signals, sig);

        lock_cas(&target->thread_list_lock);
        for (th = container_of((target->thread_head_node.next), Thread_Base,
                              thread_list_node),
             tmp = container_of((th->thread_list_node.next), Thread_Base,
                                thread_list_node);
             &th->thread_list_node != &target->thread_head_node;
             th = tmp, tmp = container_of((tmp->thread_list_node.next),
                                          Thread_Base, thread_list_node)) {
                linux_thread_append_t* thread_append = linux_thread_append(th);

                if (thread_append) {
                        sigdelset(&thread_append->pending_signals, sig);
                }
        }
        unlock_cas(&target->thread_list_lock);
}

static i64 signal_queue_on_thread_helper(Tcb_Base* target,
                                         Thread_Base* target_thread, int sig)
{
        linux_proc_append_t* proc_append;
        linux_thread_append_t* thread_append;

        proc_append = linux_proc_append(target);
        thread_append = linux_thread_append(target_thread);
        if (!proc_append || !thread_append) {
                return -LINUX_ESRCH;
        }

        sigaddset(&proc_append->pending_signals, sig);
        sigaddset(&thread_append->pending_signals, sig);

        if (thread_get_status(target_thread) != thread_status_running) {
                thread_set_status(target_thread, thread_status_ready);
        }

        return 0;
}

i64 linux_queue_signal(Tcb_Base* target, int sig, pid_t sender_tid)
{
        sigaction_t* disp;
        Thread_Base* target_thread;
        linux_proc_append_t* proc_append;

        (void)sender_tid;

        if (!target) {
                return -LINUX_ESRCH;
        }

        if (sig < 1 || sig > NSIG) {
                return -LINUX_EINVAL;
        }

        proc_append = linux_proc_append(target);
        if (!proc_append) {
                return -LINUX_ESRCH;
        }

        disp = &proc_append->signal_dispositions[sig - 1];
        if (linux_signal_handler_is_ign(disp->sa_handler)) {
                linux_signal_flush_pending(target, sig);
                return 0;
        }

        target_thread = signal_select_thread_helper(target, sig);
        if (!target_thread) {
                return -LINUX_ESRCH;
        }

        return signal_queue_on_thread_helper(target, target_thread, sig);
}

i64 linux_queue_signal_thread(Thread_Base* target_thread, int sig,
                              pid_t sender_tid)
{
        Tcb_Base* process;
        sigaction_t* disp;
        linux_proc_append_t* proc_append;

        (void)sender_tid;

        if (!target_thread) {
                return -LINUX_ESRCH;
        }

        process = target_thread->belong_tcb;
        if (!process) {
                return -LINUX_ESRCH;
        }

        if (sig < 1 || sig > NSIG) {
                return -LINUX_EINVAL;
        }

        proc_append = linux_proc_append(process);
        if (!proc_append) {
                return -LINUX_ESRCH;
        }

        disp = &proc_append->signal_dispositions[sig - 1];
        if (linux_signal_handler_is_ign(disp->sa_handler)) {
                linux_signal_flush_pending(process, sig);
                return 0;
        }

        return signal_queue_on_thread_helper(process, target_thread, sig);
}
