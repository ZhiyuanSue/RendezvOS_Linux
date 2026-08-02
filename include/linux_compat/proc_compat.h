#ifndef _RENDEZVOS_LINUX_COMPAT_PROC_COMPAT_H_
#define _RENDEZVOS_LINUX_COMPAT_PROC_COMPAT_H_

#include <common/types.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/time.h>
#include <linux_compat/fs/linux_fd_table.h>
#include <linux_compat/signal/signal_state.h>

/*
 * Linux compat append model:
 * - linux-layer state lives in the append area of Tcb_Base / Thread_Base
 * - core does not interpret these bytes
 *
 * IMPORTANT: append sizes live in @c task_append_hooks / @c thread_append_hooks
 * (@p append_info_len). These macros are the linux-layer struct sizes for the
 * static hook tables only. Lifecycle: doc/linux_compat/APPEND_HOOKS.md
 */

typedef struct linux_proc_append {
        /* Memory management */
        u64 start_brk;
        u64 brk;
        u64 mmap_hint; /* Anonymous mmap search cursor (page-aligned VA past
                          last mmap) */

        /* Process relationships */
        u32 uid;
        u32 gid;
        u32 euid;
        u32 egid;
        /*
         * Parent PID. LINUX_INIT_REAP_PPID (0) means init-adopted / orphan:
         * exit uses protocol link B (REAPED + THREAD_REAP-only; listen
         * finishes delete_task), not EXIT_NOTIFY to kernel_port.
         * Live parent (ppid>0) uses link A.
         */
        pid_t ppid;
        pid_t pgid; /* Process group ID (for wait4 pid==0, pid<-1) */
        i32 exit_code; /* Exit code for wait() */
        /*
         * Exit / clean protocol (see doc/linux_compat/protocols/EXIT_CLEAN.md):
         *   0 RUNNING, 1 ZOMBIE, 2 REAPED, 3 TASK_CLAIMED
         */
        i32 exit_state;
        u8 exit_notify_sent; /* EXIT_NOTIFY posted at most once */
        /* EXIT_NOTIFY not yet consumed by wait4 (wait(pid) mismatch). */
        struct list_entry pending_exits;

        /*
         * Phase 2B / Phase 4: heap-backed Linux state (append = pointer only).
         */
        linux_signal_proc_state_t *signal;
        linux_fs_state_t *fs;
} linux_proc_append_t;

/** ppid after reparent-to-init (kernel_port reap on boot_thread; ≠ user /init). */
#define LINUX_INIT_REAP_PPID 0

#define LINUX_EXIT_RUNNING      0
#define LINUX_EXIT_ZOMBIE       1
#define LINUX_EXIT_REAPED       2
#define LINUX_EXIT_TASK_CLAIMED 3

typedef struct linux_thread_append {
        /*
         * Heap-backed / IPC fields first — fixed offsets used across
         * linux_layer (signal @ append+0, sleep_port @ +8, …). Do not insert
         * fields before signal without rebuilding all consumers.
         */
        linux_signal_thread_state_t *signal;

        Message_Port_t *sleep_port;
        u64 sleep_timer_token;
        rendezvos_timer_event sleep_timer_event;

        /* Thread management (after stable-prefix fields above). */
        u64 clear_tid; /* user pointer for set_tid_address/CLONE_CHILD_CLEARTID
                        */
        /* Path-B PID1 wait: set only on /init; copy_thread must clear. */
        u64 boot_wait_cookie;
} linux_thread_append_t;

#define LINUX_PROC_APPEND_BYTES   ((size_t)sizeof(linux_proc_append_t))
#define LINUX_THREAD_APPEND_BYTES ((size_t)sizeof(linux_thread_append_t))

static inline linux_proc_append_t *linux_proc_append(Tcb_Base *tcb)
{
        if (!tcb)
                return NULL;
        return (linux_proc_append_t *)tcb->append_tcb_info;
}

static inline linux_thread_append_t *linux_thread_append(Thread_Base *thread)
{
        if (!thread)
                return NULL;
        return (linux_thread_append_t *)thread->append_thread_info;
}

#endif
