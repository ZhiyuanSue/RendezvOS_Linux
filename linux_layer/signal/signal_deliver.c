#include <common/stdbool.h>
#include <common/types.h>
#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/signal/signal_stack.h>
#include <linux_compat/signal/signal_types.h>
#include <modules/log/log.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/trap/trap.h>
#include <syscall.h>

#if defined(_X86_64_)
#include <arch/x86_64/boot/arch_setup.h>
#include <arch/x86_64/tcb_arch.h>
#elif defined(_AARCH64_)
#include <arch/aarch64/boot/arch_setup.h>
#include <arch/aarch64/sys_ctrl.h>
#include <arch/aarch64/tcb_arch.h>
#endif

/*
 * Phase 2B: Signal Delivery Implementation
 *
 * This file implements linux_deliver_pending_signals() which is called
 * before returning to user space to deliver pending signals.
 *
 * Implementation follows:
 * - doc/linux_compat/SIGNAL_DELIVERY_TRAP_PATHS.md
 * - Layer B of the two-layer model
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
                signal_restore_user_sp(thread_append, tf,
                                       thread_append->saved_main_sp);
                thread_append->alt_stack.ss_flags &= ~SS_ONSTACK;
                pr_debug("[SIGNAL] Restored main stack pointer\n");
        }
}

/*
 * Check if current thread has pending signals
 */
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

    /* Check if any signal is pending */
    for (int i = 0; i < (int)(64 / (8 * sizeof(unsigned long))); i++) {
        if (thread_append->pending_signals.sig[i] != 0) {
            return true;
        }
    }

    return false;
}

/*
 * Select the highest priority pending signal that is not blocked
 * Returns signal number, or 0 if no deliverable signal
 */
static int signal_select_pending_helper(linux_thread_append_t *thread_append,
                                        linux_proc_append_t *proc_append)
{
    (void)proc_append; /* TODO: Use for process-wide signals */
    /* Standard signals: 1-31, Real-time signals: 32-64 */
    /* Lower number = higher priority for standard signals */
    /* RT signals have FIFO ordering within same priority */

    /* SIGKILL/SIGSTOP cannot be blocked (Linux semantics). */
    if (sigismember(&thread_append->pending_signals, SIGKILL)) {
        return SIGKILL;
    }
    if (sigismember(&thread_append->pending_signals, SIGSTOP)) {
        return SIGSTOP;
    }

    /* Check standard signals first (1-31) */
    for (int sig = 1; sig <= 31; sig++) {
        if (sigismember(&thread_append->pending_signals, sig) &&
            !sigismember(&thread_append->blocked_signals, sig)) {
            return sig;
        }
    }

    /* Then check real-time signals (32-64) */
    for (int sig = 32; sig <= 64; sig++) {
        if (sigismember(&thread_append->pending_signals, sig) &&
            !sigismember(&thread_append->blocked_signals, sig)) {
            return sig;
        }
    }

    return 0; /* No deliverable signal */
}

/*
 * x86_64 specific: Setup signal delivery on syscall return path
 *
 * According to SIGNAL_DELIVERY_TRAP_PATHS.md:
 * - User PC → handler: modify syscall_ctx->rcx
 * - User SP → signal frame: modify percpu(user_rsp_scratch)
 * - First handler arg (sig): syscall_ctx->rdi (SysV AMD64 ABI)
 * - Do not use rax for the handler argument; syscall_entry sets rax last.
 */
#if defined(_X86_64_)
static void signal_x86_install_return_helper(struct trap_frame* tf, void* handler,
                                             vaddr user_sp, int sig)
{
    Thread_Base* th = get_cpu_current_thread();

    pr_info("[SIGNAL] x86_64 installing return: handler=%p, sp=%p, sig=%d\n",
            handler, (void*)user_sp, sig);

    /*
     * x86_64 syscall return (sysretq):
     * - User RIP comes from RCX (not trap_frame->rip!)
     * - User RSP comes from percpu(user_rsp_scratch) (not trap_frame->rsp!)
     */

    tf->rcx = (u64)(uintptr_t)handler;
    tf->rdi = (u64)sig;
    if (th) {
        signal_set_user_sp(linux_thread_append(th), tf, user_sp);
    }

    pr_debug("[SIGNAL] Modified trap_frame: rcx=%p, rdi=%llu\n",
             (void *)(uintptr_t)tf->rcx, tf->rdi);
}
#endif /* _X86_64_ */

/*
 * aarch64 syscall return (el0_trap_exit):
 * - User PC: ELR_EL1 from trap_frame->ELR
 * - User SP: trap_frame->SP (restored via mov SP, x21 before eret)
 * - Handler arg (int sig): REGS[0] (x0); syscall_entry sets REGS[0] before deliver
 */
#if defined(_AARCH64_)
static void signal_aarch64_install_return_helper(struct trap_frame *tf,
                                                 void *handler, vaddr user_sp,
                                                 int sig)
{
    Thread_Base *th = get_cpu_current_thread();

    pr_info("[SIGNAL] aarch64 installing return: handler=%p, sp=%p, sig=%d\n",
            handler, (void *)user_sp, sig);

    tf->ELR = (u64)(uintptr_t)handler;
    tf->REGS[0] = (u64)sig;
    if (th) {
        signal_set_user_sp(linux_thread_append(th), tf, user_sp);
    }

    pr_debug("[SIGNAL] Modified trap_frame: ELR=%p, SP=%p, x0=%d\n",
             (void *)(uintptr_t)tf->ELR, (void *)(uintptr_t)tf->SP, sig);
}
#endif /* _AARCH64_ */

static void signal_apply_handler_mask_helper(linux_thread_append_t* thread_append,
                                             const sigaction_t* disp, int sig)
{
        if (!(disp->flags & SA_NODEFER)) {
                sigaddset(&thread_append->blocked_signals, sig);
        }

        for (int s = 1; s <= NSIG; s++) {
                if (sigismember(&disp->mask, s)) {
                        sigaddset(&thread_append->blocked_signals, s);
                }
        }
}

static void signal_save_handler_context_helper(linux_thread_append_t* thread_append,
                                               struct trap_frame* tf)
{
        linux_signal_restore_t* rs = &thread_append->signal_restore;

        rs->active = 1;
        rs->sig = 0;
        rs->saved_blocked = thread_append->blocked_signals;

#if defined(_X86_64_)
        rs->saved_user_pc = tf->rcx;
        rs->saved_user_sp = percpu(user_rsp_scratch);
        rs->saved_syscall_ret = tf->rax;
#elif defined(_AARCH64_)
        rs->saved_user_pc = tf->ELR;
        rs->saved_user_sp = tf->SP;
        rs->saved_syscall_ret = tf->REGS[0];
#endif
}

/*
 * Build a minimal signal frame on user stack
 *
 * This is a simplified version - full Linux rt_sigframe is much more complex
 * For now, we just create a basic frame to test the delivery mechanism
 */
static vaddr signal_build_frame_helper(vaddr base_sp, int sig, void *handler)
{
    (void)sig;      /* TODO: Use for full sigframe */
    (void)handler;  /* TODO: Use for sigreturn trampoline */

    /*
     * Simplified signal frame layout (x86_64):
     * We just need to make sure there's something on the stack
     * and align it properly.
     *
     * Full implementation would create:
     * - struct rt_sigframe (with siginfo, ucontext, fp state)
     * - Proper alignment
     * - Signal mask
     * - Return trampoline (sigreturn)
     */

    /* Align stack to 16 bytes (System V ABI requirement) */
    vaddr user_sp = base_sp - 8; /* Reserve space for alignment */
    user_sp = user_sp & ~0xF;   /* Align to 16 bytes */

    pr_debug("[SIGNAL] Built signal frame at sp=%p\n", (void *)user_sp);

    return user_sp;
}

/*
 * Main signal delivery function (called from syscall return path)
 */
bool linux_deliver_pending_signals(struct trap_frame *tf)
{
    pr_debug("[SIGNAL] linux_deliver_pending_signals called\n");

    if (!signal_thread_has_pending_helper()) {
        pr_debug("[SIGNAL] No pending signals\n");
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

    /* Select signal to deliver */
    int sig = signal_select_pending_helper(thread_append, proc_append);
    if (sig == 0) {
        pr_debug("[SIGNAL] No deliverable signals (all blocked)\n");
        return false;
    }

    pr_info("[SIGNAL] Delivering signal %d to thread %d\n", sig, current_thread->tid);

    /* Get signal disposition */
    sigaction_t *disp = &proc_append->signal_dispositions[sig - 1];

    /* Handle signal disposition */
    if (disp->handler == SIG_IGN) {
        pr_debug("[SIGNAL] Signal %d is ignored (SIG_IGN)\n", sig);
        sigdelset(&thread_append->pending_signals, sig);
        return false;
    }

    if (disp->handler == SIG_DFL) {
        pr_debug("[SIGNAL] Signal %d has default disposition\n", sig);

        /* Default signal actions based on Linux signal semantics */
        switch (sig) {
        /* Term: terminate process */
        case SIGHUP:     /* 1  - hangup */
        case SIGINT:     /* 2  - interrupt */
        case SIGTERM:    /* 15 - termination signal */
        case SIGUSR1:    /* 10 - user-defined signal 1 */
        case SIGUSR2:    /* 12 - user-defined signal 2 */
        case SIGPIPE:    /* 13 - broken pipe */
        case SIGALRM:    /* 14 - alarm clock */
        case SIGPROF:    /* 27 - profiling timer */
        case SIGVTALRM:  /* 26 - virtual timer */
        case SIGSTKFLT:  /* 16 - stack fault */
        case SIGPWR:     /* 30 - power failure */
            pr_info("[SIGNAL] Default action: terminating process (signal %d)\n", sig);
            sigdelset(&thread_append->pending_signals, sig);
            /* Reuse existing exit mechanism */
            sys_exit(128 + sig);  /* Linux standard: signal termination = 128 + signal number */
            __builtin_unreachable();  /* sys_exit never returns */
            break;

        /* Ign: ignore signal */
        case SIGCHLD:    /* 17 - child status changed */
        case SIGCONT:    /* 18 - continue */
        case SIGWINCH:   /* 28 - window size change */
        case SIGURG:     /* 23 - urgent data on socket */
            pr_debug("[SIGNAL] Default action: ignoring signal %d\n", sig);
            sigdelset(&thread_append->pending_signals, sig);
            return false;

        /* Core: core dump + terminate (not fully implemented yet) */
        case SIGQUIT:    /* 3  - quit */
        case SIGILL:     /* 4  - illegal instruction */
        case SIGTRAP:    /* 5  - trap */
        case SIGABRT:    /* 6  - abort */
        case SIGBUS:     /* 7  - bus error */
        case SIGFPE:     /* 8  - floating point exception */
        case SIGSEGV:    /* 11 - segmentation fault */
        case SIGXCPU:    /* 24 - CPU limit */
        case SIGXFSZ:    /* 25 - file size limit */
            pr_warn("[SIGNAL] Default action: core dump for signal %d (not fully implemented, terminating)\n", sig);
            sigdelset(&thread_append->pending_signals, sig);
            /* TODO: implement proper core dump when ELF core is available */
            /* For now, just terminate the process */
            sys_exit(128 + sig);
            __builtin_unreachable();
            break;

        /* Stop: stop process (not fully implemented yet) */
        case SIGSTOP:    /* 19 - stop process */
        case SIGTSTP:    /* 20 - stop from tty */
        case SIGTTIN:    /* 21 - background read from tty */
        case SIGTTOU:    /* 22 - background write to tty */
            pr_warn("[SIGNAL] Default action: stop process for signal %d (not fully implemented)\n", sig);
            sigdelset(&thread_append->pending_signals, sig);
            /* TODO: implement proper process stop when scheduler supports it */
            /* For now, just log and continue */
            return false;

        default:
            pr_warn("[SIGNAL] Unknown default action for signal %d, ignoring\n", sig);
            sigdelset(&thread_append->pending_signals, sig);
            return false;
        }
    }

    /* User handler - build signal frame and modify trap_frame */
    pr_info("[SIGNAL] Delivering to user handler at %p\n", disp->handler);

    /* Get current user stack pointer */
    vaddr user_sp;
#if defined(_X86_64_)
    user_sp = percpu(user_rsp_scratch);
#elif defined(_AARCH64_)
    user_sp = tf->SP;
#endif

    /* Check if we should use alternate signal stack */
    if ((disp->flags & SA_ONSTACK) && thread_append->alt_stack.ss_sp != NULL) {
        stack_t *alt_stack = &thread_append->alt_stack;

        /* Check if we're not already on the alternate stack */
        if (!(alt_stack->ss_flags & SS_ONSTACK)) {
            pr_info("[SIGNAL] Switching to alternate signal stack: sp=%p, size=%zu\n",
                    alt_stack->ss_sp, alt_stack->ss_size);

            /* Save main stack pointer for restoration later */
            thread_append->saved_main_sp = user_sp;

            /* Calculate alternate stack pointer (stack grows down) */
            user_sp = (vaddr)alt_stack->ss_sp + alt_stack->ss_size;

            /* Mark that we're on the alternate stack */
            alt_stack->ss_flags |= SS_ONSTACK;

            pr_debug("[SIGNAL] Saved main SP=%p, new alt SP=%p\n",
                     (void *)thread_append->saved_main_sp, (void *)user_sp);
        }
    }

    signal_save_handler_context_helper(thread_append, tf);
    thread_append->signal_restore.sig = sig;

    if (disp->flags & SA_RESETHAND) {
        disp->handler = SIG_DFL;
    }
    signal_apply_handler_mask_helper(thread_append, disp, sig);

    /* Build signal frame on user stack */
    user_sp = signal_build_frame_helper(user_sp, sig, disp->handler);

    /* Install return to signal handler */
#if defined(_X86_64_)
    signal_x86_install_return_helper(tf, disp->handler, user_sp, sig);
#elif defined(_AARCH64_)
    signal_aarch64_install_return_helper(tf, disp->handler, user_sp, sig);
#else
#error "Unsupported architecture for signal delivery"
#endif

    sigdelset(&thread_append->pending_signals, sig);
    sigdelset(&proc_append->pending_signals, sig);

    pr_info("[SIGNAL] Signal %d delivery complete\n", sig);
    return true;
}