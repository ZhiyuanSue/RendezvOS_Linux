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
 * IMPORTANT: the creator path must pass append sizes consistently (single
 * source of truth: these macros).
 */

typedef struct linux_proc_append {
        /* Memory management */
        u64 start_brk;
        u64 brk;
        u64 mmap_hint; /* Anonymous mmap search cursor (page-aligned VA past
                          last mmap) */

        /* Process relationships */
        /*
         * Parent PID. LINUX_INIT_REAP_PPID (0) means kernel init reaps via
         * kernel_port after parent exit (reparent in task append_fini).
         */
        pid_t ppid;
        pid_t pgid; /* Process group ID (for wait4 pid==0, pid<-1) */
        i32 exit_code; /* Exit code for wait() */
        i32 exit_state; /* Exit state: 0=running, 1=zombie, 2=reaped */
        struct list_entry wait_queue; /* Parent processes waiting */

        /*
         * Phase 2B / Phase 4: heap-backed Linux state (append = pointer only).
         */
        linux_signal_proc_state_t *signal;
        linux_fs_state_t *fs;
} linux_proc_append_t;

/** ppid after reparent-to-init (kernel init thread / kernel_port reap). */
#define LINUX_INIT_REAP_PPID 0

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
        u64 clear_tid; /* user pointer for set_tid_address/CLONE_CHILD_CLEARTID */
        u64 test_cookie; /* TEST ONLY: runner correlation cookie (0 = not test) */
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
