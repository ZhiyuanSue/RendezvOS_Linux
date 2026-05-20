#include <common/stdbool.h>
#include <common/types.h>
#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/signal/signal_types.h>
#include <rendezvos/error.h>
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

void linux_restore_main_stack_if_needed(struct trap_frame* tf)
{
        Thread_Base* current_thread = get_cpu_current_thread();
        linux_thread_append_t* thread_append;

        if (!current_thread) {
                return;
        }

        thread_append = linux_thread_append(current_thread);
        if (!thread_append) {
                return;
        }

        if (thread_append->alt_stack.ss_flags & SS_ONSTACK) {
                vaddr user_pc, user_sp, syscall_ret;

                arch_syscall_get_user_return(
                        tf, &current_thread->ctx, &user_pc, &user_sp,
                        &syscall_ret);
                arch_syscall_set_user_return(tf, &current_thread->ctx, user_pc,
                                             thread_append->saved_main_sp,
                                             syscall_ret);
                thread_append->alt_stack.ss_flags &= ~SS_ONSTACK;
        }
}

static bool signal_thread_has_pending_helper(void)
{
    Thread_Base *current_thread = get_cpu_current_thread();
    if (!current_thread) {
        return false;
    }

    linux_thread_append_t *thread_append = linux_thread_append(current_thread);
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
        if (sigismember(&thread_append->pending_signals, sig) &&
            !sigismember(&thread_append->blocked_signals, sig)) {
            return sig;
        }
    }

    for (int sig = 32; sig <= 64; sig++) {
        if (sigismember(&thread_append->pending_signals, sig) &&
            !sigismember(&thread_append->blocked_signals, sig)) {
            return sig;
        }
    }

    return 0;
}

static void signal_apply_handler_mask_helper(linux_thread_append_t* thread_append,
                                             const sigaction_t* disp, int sig)
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

static void signal_save_handler_context_helper(Thread_Base* th,
                                               linux_thread_append_t* thread_append,
                                               struct trap_frame* tf)
{
        linux_signal_restore_t* rs = &thread_append->signal_restore;

        rs->active = 1;
        rs->sig = 0;
        rs->saved_blocked = thread_append->blocked_signals;
        arch_syscall_get_user_return(tf, th ? &th->ctx : NULL, &rs->saved_user_pc,
                                     &rs->saved_user_sp, &rs->saved_syscall_ret);
}

static vaddr signal_build_frame_helper(vaddr base_sp, int sig, void *handler)
{
    (void)sig;
    (void)handler;

    vaddr user_sp = base_sp - 8;
    user_sp = user_sp & ~0xF;

    return user_sp;
}

/*
 * Push the saved user return PC so a handler that ends with "ret" can fall
 * back to the pre-signal site. rt_sigreturn remains the preferred path.
 */
static vaddr signal_push_user_return_link(VSpace* vs, vaddr user_sp, vaddr return_pc)
{
        vaddr sp;
        u64 link;

        if (!vs || !linux_vspace_is_user_table(vs) || return_pc == 0) {
                return user_sp;
        }

        sp = (user_sp - 8) & ~0xF;
        link = (u64)return_pc;
        if (linux_mm_store_to_user(vs, sp, &link, sizeof(link)) != REND_SUCCESS) {
                return user_sp;
        }
        return sp;
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

    linux_thread_append_t *thread_append = linux_thread_append(current_thread);
    linux_proc_append_t *proc_append = linux_proc_append(current_process);

    if (!thread_append || !proc_append) {
        return false;
    }

    int sig = signal_select_pending_helper(thread_append, proc_append);
    if (sig == 0) {
        return false;
    }

    sigaction_t *disp = &proc_append->signal_dispositions[sig - 1];

    if (disp->sa_handler == SIG_IGN) {
        sigdelset(&thread_append->pending_signals, sig);
        return false;
    }

    if (disp->sa_handler == SIG_DFL) {
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
            sigdelset(&thread_append->pending_signals, sig);
            sys_exit(128 + sig);
            __builtin_unreachable();

        case SIGCHLD:
        case SIGCONT:
        case SIGWINCH:
        case SIGURG:
            sigdelset(&thread_append->pending_signals, sig);
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
            sigdelset(&thread_append->pending_signals, sig);
            sys_exit(128 + sig);
            __builtin_unreachable();

        case SIGSTOP:
        case SIGTSTP:
        case SIGTTIN:
        case SIGTTOU:
            sigdelset(&thread_append->pending_signals, sig);
            return false;

        default:
            sigdelset(&thread_append->pending_signals, sig);
            return false;
        }
    }

    vaddr user_pc, user_sp, syscall_ret;

    arch_syscall_get_user_return(tf, current_thread ? &current_thread->ctx : NULL,
                                 &user_pc, &user_sp, &syscall_ret);

    if ((disp->sa_flags & SA_ONSTACK) && thread_append->alt_stack.ss_sp != NULL) {
        stack_t *alt_stack = &thread_append->alt_stack;

        if (!(alt_stack->ss_flags & SS_ONSTACK)) {
            thread_append->saved_main_sp = user_sp;
            user_sp = (vaddr)alt_stack->ss_sp + alt_stack->ss_size;
            alt_stack->ss_flags |= SS_ONSTACK;
        }
    }

    signal_save_handler_context_helper(current_thread, thread_append, tf);
    thread_append->signal_restore.sig = sig;

    if (disp->sa_flags & SA_RESETHAND) {
        disp->sa_handler = SIG_DFL;
    }
    signal_apply_handler_mask_helper(thread_append, disp, sig);

    user_sp = signal_build_frame_helper(user_sp, sig, disp->sa_handler);
    user_sp = signal_push_user_return_link(current_process->vs, user_sp, user_pc);

    arch_syscall_set_user_return(tf, current_thread ? &current_thread->ctx : NULL,
                                 (vaddr)(uintptr_t)disp->sa_handler, user_sp,
                                 syscall_ret);
    arch_syscall_set_user_int_arg(tf, 0, (u64)sig);

    sigdelset(&thread_append->pending_signals, sig);
    sigdelset(&proc_append->pending_signals, sig);

    return true;
}
