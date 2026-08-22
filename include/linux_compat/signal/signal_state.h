#ifndef _LINUX_COMPAT_SIGNAL_STATE_H_
#define _LINUX_COMPAT_SIGNAL_STATE_H_

#include <linux_compat/signal/signal_types.h>
#include <linux_compat/signal/signal_restore_arch.h>
#include <rendezvos/error.h>
#include <rendezvos/task/thread.h>

struct linux_proc_resource;

/*
 * Heap-backed signal state (append holds pointers only — same pattern as fs).
 */

typedef struct linux_signal_restore {
        u8 active;
        int sig;
        sigset_t saved_blocked;
        u64 saved_syscall_ret;
        u64 saved_user_pc;
        u64 saved_user_sp;
        linux_signal_restore_arch_t arch;
} linux_signal_restore_t;

typedef struct linux_signal_proc_state {
        sigaction_t dispositions[NSIG];
        sigset_t pending_signals;
        /* Per-process RX page with rt_sigreturn stub (0 = not mapped yet). */
        vaddr sigreturn_page;
} linux_signal_proc_state_t;

typedef struct linux_signal_thread_state {
        sigset_t blocked_signals;
        sigset_t pending_signals;
        stack_t alt_stack;
        vaddr saved_main_sp;
        linux_signal_restore_t signal_restore;
        u8 signal_inflight;
} linux_signal_thread_state_t;

void linux_signal_reinit_proc_state(linux_signal_proc_state_t *ps);
void linux_signal_reinit_thread_state(linux_signal_thread_state_t *ts);

error_t linux_signal_proc_attach(struct linux_proc_resource *proc);
void linux_signal_proc_destroy(struct linux_proc_resource *proc);
error_t linux_signal_proc_fork(struct linux_proc_resource *child,
                               struct linux_proc_resource *parent);
void linux_signal_proc_reset(struct linux_proc_resource *proc);

error_t linux_signal_thread_attach(Thread_Base *thread);
void linux_signal_thread_destroy(Thread_Base *thread);
error_t linux_signal_thread_fork_inherit(Thread_Base *child,
                                         Thread_Base *parent,
                                         bool copy_blocked);

linux_signal_proc_state_t *linux_signal_proc_state(struct linux_proc_resource *proc);
linux_signal_thread_state_t *linux_signal_thread_state(Thread_Base *thread);

#endif /* _LINUX_COMPAT_SIGNAL_STATE_H_ */
