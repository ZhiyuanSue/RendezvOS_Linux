#ifndef _RENDEZVOS_LINUX_COMPAT_PROC_COMPAT_H_
#define _RENDEZVOS_LINUX_COMPAT_PROC_COMPAT_H_

#include <common/types.h>
#include <rendezvos/task/tcb.h>
#include <linux_compat/signal/signal_types.h>
#include <linux_compat/signal/signal_restore_arch.h>

/*
 * Linux compat append model:
 * - linux-layer state lives in the append area of Tcb_Base / Thread_Base
 * - core does not interpret these bytes
 *
 * IMPORTANT: the creator path must pass append sizes consistently (single
 * source of truth: these macros).
 */

typedef struct linux_proc_append {
        /* Memory management */
        u64 start_brk;
        u64 brk;
        u64 mmap_hint;  /* Anonymous mmap search cursor (page-aligned VA past last mmap) */

        /* Process relationships */
        pid_t ppid;     /* Parent process PID */
        pid_t pgid;     /* Process group ID (for wait4 pid==0, pid<-1) */
        i32 exit_code;  /* Exit code for wait() */
        i32 exit_state; /* Exit state: 0=running, 1=zombie, 2=reaped */
        struct list_entry wait_queue; /* Parent processes waiting */

        /* Phase 2B: Signal disposition (per-process, per-signal) */
        sigaction_t signal_dispositions[NSIG]; /* Signal handlers */
        sigset_t pending_signals;              /* Process-wide pending signals */
} linux_proc_append_t;

/*
 * Kernel-stored pre-handler context for rt_sigreturn (Phase 2B).
 * Not a full Linux rt_sigframe on the user stack — sufficient for compat tests
 * and explicit rt_sigreturn(); glibc may need user-stack sigframe later.
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

typedef struct linux_thread_append {
        /* Thread management */
        u64 clear_tid;  /* user pointer for set_tid_address/CLONE_CHILD_CLEARTID */
        u64 test_cookie; /* TEST ONLY: test runner correlation cookie (0 = not test thread) */

        /* Phase 2B: Signal state (per-thread) */
        sigset_t blocked_signals;  /* Signal mask (blocked signals) */
        sigset_t pending_signals;  /* Thread-specific pending signals */
        stack_t alt_stack;         /* Alternate signal stack (embedded structure) */
        vaddr saved_main_sp;       /* Saved main stack pointer when using alt stack */
        linux_signal_restore_t signal_restore;
        /*
         * Single-slot handler depth: 0 = none, 1 = in handler awaiting
         * rt_sigreturn. Must match signal_restore.active (0/1 only).
         */
        u8 signal_inflight;
} linux_thread_append_t;

#define LINUX_PROC_APPEND_BYTES   ((size_t)sizeof(linux_proc_append_t))
#define LINUX_THREAD_APPEND_BYTES ((size_t)sizeof(linux_thread_append_t))

static inline linux_proc_append_t* linux_proc_append(Tcb_Base* tcb)
{
        if (!tcb)
                return NULL;
        return (linux_proc_append_t*)tcb->append_tcb_info;
}

static inline linux_thread_append_t* linux_thread_append(Thread_Base* thread)
{
        if (!thread)
                return NULL;
        return (linux_thread_append_t*)thread->append_thread_info;
}

#endif
