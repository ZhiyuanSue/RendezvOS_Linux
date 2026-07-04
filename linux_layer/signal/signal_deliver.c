#include <common/stdbool.h>
#include <common/types.h>
#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/signal/signal_types.h>
#include <linux_compat/signal/signal_context.h>
#include <linux_compat/signal/signal_altstack.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/trap/trap.h>
#include <syscall.h>

/*
 * Phase 2B: Signal Delivery Implementation
 *
 * This file implements linux_deliver_pending_signals() which is called
 * before returning to user space to deliver pending signals.
 */

static bool signal_thread_has_pending_helper(void)
{
        Thread_Base *current_thread = get_cpu_current_thread();
        if (!current_thread) {
                return false;
        }

        linux_thread_append_t *thread_append =
                linux_thread_append(current_thread);
        if (!thread_append) {
                return false;
        }

        for (int i = 0; i < (int)(64 / (8 * sizeof(unsigned long))); i++) {
                if (thread_append->pending_signals.sig[i] != 0) {
                        return true;
                }
        }

        return false;
}

static int signal_select_pending_helper(linux_thread_append_t *thread_append,
                                        linux_proc_append_t *proc_append)
{
        (void)proc_append;

        if (sigismember(&thread_append->pending_signals, SIGKILL)) {
                return SIGKILL;
        }
        if (sigismember(&thread_append->pending_signals, SIGSTOP)) {
                return SIGSTOP;
        }

        for (int sig = 1; sig <= 31; sig++) {
                if (sigismember(&thread_append->pending_signals, sig)
                    && !sigismember(&thread_append->blocked_signals, sig)) {
                        return sig;
                }
        }

        for (int sig = 32; sig <= 64; sig++) {
                if (sigismember(&thread_append->pending_signals, sig)
                    && !sigismember(&thread_append->blocked_signals, sig)) {
                        return sig;
                }
        }

        return 0;
}

static void
signal_apply_handler_mask_helper(linux_thread_append_t *thread_append,
                                 const sigaction_t *disp, int sig)
{
        if (!(disp->sa_flags & SA_NODEFER)) {
                sigaddset(&thread_append->blocked_signals, sig);
        }

        for (int s = 1; s <= NSIG; s++) {
                if (sigismember(&disp->sa_mask, s)) {
                        sigaddset(&thread_append->blocked_signals, s);
                }
        }
}

static inline void signal_clear_pending(linux_thread_append_t *thread_append,
                                        linux_proc_append_t *proc_append,
                                        int sig)
{
        sigdelset(&thread_append->pending_signals, sig);
        if (proc_append) {
                sigdelset(&proc_append->pending_signals, sig);
        }
}

static bool
signal_save_handler_context_helper(Thread_Base *th,
                                   linux_thread_append_t *thread_append,
                                   struct trap_frame *tf)
{
        linux_signal_restore_t *rs = &thread_append->signal_restore;

        if (thread_append->signal_inflight != 0 || rs->active) {
                return false;
        }

        rs->active = 1;
        thread_append->signal_inflight = 1;
        rs->saved_blocked = thread_append->blocked_signals;
        linux_signal_arch_save_context(tf, th ? &th->ctx : NULL, rs);
        return true;
}

static vaddr signal_build_frame_helper(vaddr base_sp, int sig, void *handler)
{
        (void)sig;
        (void)handler;

        vaddr user_sp = base_sp - 8;
        user_sp = user_sp & ~0xF;

        return user_sp;
}

bool linux_signal_thread_has_deliverable_pending(Thread_Base *thread)
{
        Tcb_Base *process;
        linux_thread_append_t *thread_append;
        linux_proc_append_t *proc_append;

        if (!thread) {
                return false;
        }

        process = thread->belong_tcb;
        if (!process) {
                return false;
        }

        thread_append = linux_thread_append(thread);
        proc_append = linux_proc_append(process);
        if (!thread_append || !proc_append) {
                return false;
        }

        return signal_select_pending_helper(thread_append, proc_append) != 0;
}

bool linux_signal_has_deliverable_pending(void)
{
        Thread_Base *current_thread = get_cpu_current_thread();

        if (!signal_thread_has_pending_helper()) {
                return false;
        }

        return linux_signal_thread_has_deliverable_pending(current_thread);
}

bool linux_deliver_pending_signals(struct trap_frame *tf)
{
        if (!signal_thread_has_pending_helper()) {
                return false;
        }

        Thread_Base *current_thread = get_cpu_current_thread();
        if (!current_thread) {
                return false;
        }

        Tcb_Base *current_process = current_thread->belong_tcb;
        if (!current_process) {
                return false;
        }

        linux_thread_append_t *thread_append =
                linux_thread_append(current_thread);
        linux_proc_append_t *proc_append = linux_proc_append(current_process);

        if (!thread_append || !proc_append) {
                return false;
        }

        int sig = signal_select_pending_helper(thread_append, proc_append);
        if (sig == 0) {
                return false;
        }

        sigaction_t *disp = &proc_append->signal_dispositions[sig - 1];

        if (linux_signal_handler_is_ign(disp->sa_handler)) {
                signal_clear_pending(thread_append, proc_append, sig);
                return false;
        }

        if (linux_signal_handler_is_dfl(disp->sa_handler)) {
                switch (sig) {
                case SIGHUP:
                case SIGINT:
                case SIGTERM:
                case SIGUSR1:
                case SIGUSR2:
                case SIGPIPE:
                case SIGALRM:
                case SIGPROF:
                case SIGVTALRM:
                case SIGSTKFLT:
                case SIGPWR:
                        signal_clear_pending(thread_append, proc_append, sig);
                        sys_exit(128 + sig);
                        __builtin_unreachable();

                case SIGCHLD:
                case SIGCONT:
                case SIGWINCH:
                case SIGURG:
                        signal_clear_pending(thread_append, proc_append, sig);
                        return false;

                case SIGQUIT:
                case SIGILL:
                case SIGTRAP:
                case SIGABRT:
                case SIGBUS:
                case SIGFPE:
                case SIGSEGV:
                case SIGXCPU:
                case SIGXFSZ:
                        signal_clear_pending(thread_append, proc_append, sig);
                        sys_exit(128 + sig);
                        __builtin_unreachable();

                case SIGSTOP:
                case SIGTSTP:
                case SIGTTIN:
                case SIGTTOU:
                        signal_clear_pending(thread_append, proc_append, sig);
                        return false;

                default:
                        signal_clear_pending(thread_append, proc_append, sig);
                        return false;
                }
        }

        if ((uintptr_t)disp->sa_handler < PAGE_SIZE) {
                signal_clear_pending(thread_append, proc_append, sig);
                return false;
        }

        /*
         * Single kernel restore slot: defer nested delivery until rt_sigreturn.
         * Also prevents overwriting saved context (SROP / forged-return
         * hardening).
         */
        if (thread_append->signal_inflight != 0) {
                return false;
        }

        vaddr user_sp, syscall_ret, user_pc_discarded;

        arch_syscall_get_user_return(tf,
                                     current_thread ? &current_thread->ctx :
                                                      NULL,
                                     &user_pc_discarded,
                                     &user_sp,
                                     &syscall_ret);

        if ((disp->sa_flags & SA_ONSTACK)
            && thread_append->alt_stack.ss_sp != NULL
            && !(thread_append->alt_stack.ss_flags & SS_DISABLE)
            && thread_append->alt_stack.ss_size >= MINSIGSTKSZ) {
                stack_t *alt_stack = &thread_append->alt_stack;
                vaddr alt_base = (vaddr)(uintptr_t)alt_stack->ss_sp;

                if (!linux_signal_altstack_region_mapped(current_process->vs,
                                                         alt_base,
                                                         alt_stack->ss_size)) {
                        signal_clear_pending(thread_append, proc_append, sig);
                        sys_exit(128 + sig);
                        __builtin_unreachable();
                }

                if (!(alt_stack->ss_flags & SS_ONSTACK)) {
                        thread_append->saved_main_sp = user_sp;
                        user_sp = ((vaddr)(uintptr_t)alt_stack->ss_sp
                                   + alt_stack->ss_size)
                                  & ~((vaddr)0xF);
                        alt_stack->ss_flags |= SS_ONSTACK;
                }
        }

        if (!signal_save_handler_context_helper(
                    current_thread, thread_append, tf)) {
                return false;
        }
        thread_append->signal_restore.sig = sig;

        if (disp->sa_flags & SA_RESETHAND) {
                disp->sa_handler = SIG_DFL;
        }
        signal_apply_handler_mask_helper(thread_append, disp, sig);

        user_sp = signal_build_frame_helper(user_sp, sig, disp->sa_handler);

        arch_syscall_set_user_return(tf,
                                     current_thread ? &current_thread->ctx :
                                                      NULL,
                                     (vaddr)(uintptr_t)disp->sa_handler,
                                     user_sp,
                                     syscall_ret);
        arch_syscall_set_user_int_arg(tf, 0, (u64)sig);

        signal_clear_pending(thread_append, proc_append, sig);

        return true;
}
